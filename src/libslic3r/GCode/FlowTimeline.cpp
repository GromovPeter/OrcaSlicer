#include "FlowTimeline.hpp"
#include "GCode/GCodeProvenance.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace Slic3r {

double FlowTimelineInterval::raw_distance_at(double local_time) const
{
    const double t = std::clamp(local_time, 0.0, motion.duration);
    const double up = std::min(t, acceleration_time);
    double distance = motion.entry_speed * up;
    if (acceleration_time > 0.0)
        distance += 0.5 * (motion.cruise_speed - motion.entry_speed) * up * up / acceleration_time;
    const double cruise_end = motion.duration - deceleration_time;
    distance += motion.cruise_speed * std::max(0.0, std::min(t, cruise_end) - acceleration_time);
    const double down = std::max(0.0, t - cruise_end);
    distance += motion.cruise_speed * down;
    if (deceleration_time > 0.0)
        distance -= 0.5 * (motion.cruise_speed - motion.exit_speed) * down * down / deceleration_time;
    return distance;
}

double FlowTimelineInterval::distance_at(double local_time) const
{
    const double integrated_distance = raw_distance_at(motion.duration);
    // Planner profiles use floats. Normalize their integrated distance to the
    // original length so many short segments do not accumulate volume error.
    return integrated_distance > 0.0 ?
        motion.distance * raw_distance_at(local_time) / integrated_distance : 0.0;
}

double FlowTimelineInterval::deposited_volume(double begin, double end) const
{
    if (end < begin)
        throw std::invalid_argument("Flow timeline interval ends before it begins");
    return volume_per_distance *
        (std::max(0.0, distance_at(end) - recovery_distance) -
         std::max(0.0, distance_at(begin) - recovery_distance));
}

double FlowTimelineInterval::volumetric_flow(double local_time) const
{
    if (local_time < 0.0 || local_time > motion.duration ||
        distance_at(local_time) < recovery_distance || volume_per_distance == 0.0)
        return 0.0;
    double speed = motion.cruise_speed;
    if (acceleration_time > 0.0 && local_time < acceleration_time)
        speed = motion.entry_speed + (motion.cruise_speed - motion.entry_speed) * local_time / acceleration_time;
    else if (deceleration_time > 0.0 && local_time > motion.duration - deceleration_time)
        speed -= (motion.cruise_speed - motion.exit_speed) *
            (local_time - (motion.duration - deceleration_time)) / deceleration_time;
    const double integrated_distance = raw_distance_at(motion.duration);
    return integrated_distance > 0.0 ? volume_per_distance * speed * motion.distance / integrated_distance : 0.0;
}

void FlowTimeline::clear()
{
    m_intervals.clear();
    m_temperature_targets.clear();
    m_retraction_debt.clear();
}

void FlowTimeline::append_delay(double end_time)
{
    const double begin = total_time();
    // Suppress only round-off gaps between adjacent planner blocks.
    if (end_time > begin + 1e-7) {
        FlowTimelineInterval interval;
        interval.motion.kind = FlowMotionKind::Delay;
        interval.motion.protected_region = true;
        interval.motion.start_time = begin;
        interval.motion.duration = end_time - begin;
        m_intervals.push_back(interval);
    }
}

void FlowTimeline::append(const FlowTimelineMotion& motion)
{
    for (double value : {motion.start_time, motion.duration, motion.distance, motion.entry_speed,
                         motion.cruise_speed, motion.exit_speed, motion.acceleration, motion.filament_area})
        if (!std::isfinite(value) || value < 0.0)
            throw std::invalid_argument("Invalid flow timeline motion");
    if (!std::isfinite(motion.delta_e) || !std::isfinite(motion.start_time + motion.duration) ||
        motion.start_time < total_time() - 1e-7)
        throw std::invalid_argument("Invalid flow timeline ordering or extrusion");

    FlowTimelineInterval interval;
    interval.motion = motion;
    if (std::abs(motion.start_time - total_time()) <= 1e-7)
        interval.motion.start_time = total_time();
    if (motion.acceleration > 0.0) {
        interval.acceleration_time = std::max(0.0, motion.cruise_speed - motion.entry_speed) / motion.acceleration;
        interval.deceleration_time = std::max(0.0, motion.cruise_speed - motion.exit_speed) / motion.acceleration;
        const double ramp_time = interval.acceleration_time + interval.deceleration_time;
        if (ramp_time > motion.duration + 1e-5 * std::max(1.0, motion.duration))
            throw std::invalid_argument("Flow timeline ramps exceed motion duration");
        if (ramp_time > motion.duration) {
            interval.acceleration_time *= motion.duration / ramp_time;
            interval.deceleration_time *= motion.duration / ramp_time;
        }
    }
    if (motion.distance > 0.0 && (motion.duration == 0.0 || motion.cruise_speed == 0.0))
        throw std::invalid_argument("Flow timeline motion has no duration or speed");

    double& debt = m_retraction_debt[motion.filament_id];
    if (motion.delta_e < 0.0)
        debt -= motion.delta_e;
    else if (motion.delta_e > 0.0) {
        const double recovered = std::min(debt, motion.delta_e);
        debt -= recovered;
        if (motion.kind == FlowMotionKind::Extrusion && motion.distance > 0.0) {
            interval.recovery_distance = motion.distance * recovered / motion.delta_e;
            interval.volume_per_distance = motion.delta_e * motion.filament_area / motion.distance;
        }
    }
    append_delay(motion.start_time);
    m_intervals.push_back(interval);
}

void FlowTimeline::finish(double total)
{
    if (!std::isfinite(total) || total < total_time() - 1e-7)
        throw std::invalid_argument("Invalid flow timeline total time");
    append_delay(total);
}

double FlowTimeline::deposited_volume() const
{
    double volume = 0.0;
    for (const auto& interval : m_intervals)
        volume += interval.deposited_volume(0.0, interval.motion.duration);
    return volume;
}

void FlowTimeline::note_temperature_target(unsigned int source_line, double temperature)
{
    m_temperature_targets.push_back({source_line, temperature});
}

void FlowTimeline::apply_provenance(const std::vector<GCodeSection>& sections, bool single_heater)
{
    // Merge protected ranges once. Looking up every custom region for every
    // movement would be quadratic on prints with per-layer scripts.
    std::vector<std::pair<unsigned int, unsigned int>> ranges;
    std::vector<unsigned int> barriers;
    for (const auto& section : sections) {
        if (ranges.empty() || section.first_line > ranges.back().second)
            ranges.emplace_back(section.first_line, section.last_line);
        else ranges.back().second = std::max(ranges.back().second, section.last_line);
        if (section.thermal_barrier) barriers.push_back(section.last_line);
    }
    std::sort(barriers.begin(), barriers.end());
    size_t range = 0;
    size_t barrier = 0;
    size_t event = 0;
    for (auto& interval : m_intervals) {
        auto& motion = interval.motion;
        if (motion.source_line == 0) continue;
        if (single_heater) motion.heater_id = 0;
        while (event < m_temperature_targets.size() && m_temperature_targets[event].source_line < motion.source_line) {
            motion.thermal_barrier_before = true;
            motion.reset_target_before = true;
            motion.target_temperature = m_temperature_targets[event].temperature;
            ++event;
        }
        while (range < ranges.size() && ranges[range].second < motion.source_line) ++range;
        if (range < ranges.size() && ranges[range].first <= motion.source_line) motion.protected_region = true;
        while (barrier < barriers.size() && barriers[barrier] < motion.source_line) {
            motion.thermal_barrier_before = true;
            ++barrier;
        }
    }
}

void FlowTimeline::remap_source_lines(const std::function<unsigned int(unsigned int)>& remap)
{
    for (auto& interval : m_intervals)
        if (interval.motion.source_line != 0) interval.motion.source_line = remap(interval.motion.source_line);
    for (auto& event : m_temperature_targets) event.source_line = remap(event.source_line);
}

} // namespace Slic3r
