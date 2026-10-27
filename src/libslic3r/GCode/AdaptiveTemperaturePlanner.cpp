#include "AdaptiveTemperaturePlanner.hpp"
#include "AdaptiveTemperatureReport.hpp"
#include "GCode/FlowTimeline.hpp"
#include "libslic3r/I18N.hpp"
#include "libslic3r/ExtrusionEntity.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <deque>
#include <istream>
#include <locale>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace Slic3r {

std::vector<FlowTemperaturePoint> parse_flow_temperature_table(std::string_view text)
{
    std::string normalized(text);
    for (char& c : normalized)
        if (c == ',' || c == ';' || c == '\n' || c == '\r') c = ' ';
    std::istringstream input(normalized);
    input.imbue(std::locale::classic());
    std::vector<FlowTemperaturePoint> table;
    for (;;) {
        input >> std::ws;
        if (input.eof()) break;
        FlowTemperaturePoint point{};
        if (!(input >> point.flow >> point.temperature))
            throw std::invalid_argument(_u8L("Invalid flow/temperature table; enter pairs of numbers"));
        table.push_back(point);
    }
    return table;
}

void validate_adaptive_temperature_settings(const AdaptiveTemperatureSettings& s)
{
    for (double v : {s.minimum_temperature, s.maximum_temperature, s.smoothing_period, s.additional_lead_time})
        if (!std::isfinite(v) || v < 0.0) throw std::invalid_argument(_u8L("Invalid adaptive-temperature limits"));
    for (double v : {s.rise_time, s.fall_time, s.minimum_command_interval})
        if (!std::isfinite(v) || v <= 0.0) throw std::invalid_argument(_u8L("Adaptive-temperature times must be positive"));
    if (!std::isfinite(s.temperature_step) || s.temperature_step < 0.5 || s.temperature_step > 5.0)
        throw std::invalid_argument(_u8L("Temperature command step must be between 0.5 and 5 °C."));
    if (s.minimum_temperature >= s.maximum_temperature || s.table.size() < 2)
        throw std::invalid_argument(_u8L("Adaptive temperature needs valid limits and at least two table points"));
    if (s.ironing_temperature && (!std::isfinite(*s.ironing_temperature) ||
        *s.ironing_temperature < s.minimum_temperature || *s.ironing_temperature > s.maximum_temperature))
        throw std::invalid_argument(_u8L("Invalid adaptive-temperature limits"));
    double previous = -1.0;
    for (const auto& p : s.table) {
        if (!std::isfinite(p.flow) || !std::isfinite(p.temperature) || p.flow < 0.0 || p.flow <= previous ||
            p.temperature < s.minimum_temperature || p.temperature > s.maximum_temperature)
            throw std::invalid_argument(_u8L("Table flows must increase and temperatures must stay within the working limits"));
        previous = p.flow;
    }
}

namespace {
double temperature_for_flow(double flow, const std::vector<FlowTemperaturePoint>& table)
{
    const auto upper = std::upper_bound(table.begin(), table.end(), flow,
        [](double q, const FlowTemperaturePoint& p) { return q < p.flow; });
    if (upper == table.begin()) return table.front().temperature;
    if (upper == table.end()) return table.back().temperature;
    const auto& a = *(upper - 1);
    const auto& b = *upper;
    return a.temperature + (b.temperature - a.temperature) * (flow - a.flow) / (b.flow - a.flow);
}

void plan_region(const std::vector<FlowTimelineInterval>& intervals, size_t begin, size_t end,
                 const AdaptiveTemperatureSettings& s, AdaptiveTemperaturePlan& plan,
                 std::optional<double>& emitted_target, double& last_command_time)
{
    const double region_start = intervals[begin].motion.start_time;
    const double region_end = intervals[end - 1].end_time();
    std::vector<double> starts;
    std::vector<double> volumes(1, 0.0), active_times(1, 0.0);
    std::vector<size_t> candidates;
    for (size_t i = begin; i < end; ++i) {
        starts.push_back(intervals[i].motion.start_time);
        const double volume = intervals[i].deposited_volume(0.0, intervals[i].motion.duration);
        volumes.push_back(volumes.back() + volume);
        active_times.push_back(active_times.back() + (volume > 1e-9 ? intervals[i].motion.duration : 0.0));
        if (candidates.empty() || intervals[candidates.back()].motion.source_line != intervals[i].motion.source_line)
            candidates.push_back(i);
    }
    auto volume_at = [&](double time) {
        time = std::clamp(time, region_start, region_end);
        if (time >= region_end) return volumes.back();
        const auto upper = std::upper_bound(starts.begin(), starts.end(), time);
        const size_t local = upper == starts.begin() ? 0 : static_cast<size_t>(upper - starts.begin() - 1);
        const auto& interval = intervals[begin + local];
        return volumes[local] + interval.deposited_volume(0.0, time - interval.motion.start_time);
    };
    auto active_at = [&](double time) {
        time = std::clamp(time, region_start, region_end);
        if (time >= region_end) return active_times.back();
        const auto upper = std::upper_bound(starts.begin(), starts.end(), time);
        const size_t local = upper == starts.begin() ? 0 : static_cast<size_t>(upper - starts.begin() - 1);
        const double partial = volumes[local + 1] > volumes[local] + 1e-9 ? time - starts[local] : 0.0;
        return active_times[local] + partial;
    };
    std::vector<double> requested;
    for (size_t i : candidates) {
        const auto& interval = intervals[i];
        const double center = std::clamp(interval.motion.start_time + s.additional_lead_time, region_start, region_end);
        const double left = std::max(region_start, center - 0.5 * s.smoothing_period);
        const double right = std::min(region_end, center + 0.5 * s.smoothing_period);
        const double denominator = s.deposition_time_only ? active_at(right) - active_at(left) : right - left;
        double flow = denominator > 1e-9 ? (volume_at(right) - volume_at(left)) / denominator :
            interval.volumetric_flow(0.5 * interval.motion.duration);
        if (s.deposition_time_only && denominator <= 1e-9) {
            // During non-printing travel retain a printing-flow reference. Prefer
            // the next deposition, allowing existing lookahead to precondition.
            auto next = std::lower_bound(starts.begin(), starts.end(), center);
            size_t k = static_cast<size_t>(next - starts.begin());
            while (k < end - begin && volumes[k + 1] - volumes[k] <= 1e-9) ++k;
            if (k == end - begin) {
                k = end - begin;
                while (k > 0 && volumes[k] - volumes[k - 1] <= 1e-9) --k;
                if (k > 0) --k;
            }
            if (k < end - begin && intervals[begin + k].motion.duration > 0)
                flow = (volumes[k + 1] - volumes[k]) / intervals[begin + k].motion.duration;
        }
        // Ironing heats the existing surface: its small extrusion flow is not
        // a useful temperature reference. Use the same lookahead and transition
        // envelope as ordinary deposition, without adding blocking commands.
        const auto role_position = std::upper_bound(starts.begin(), starts.end(), center);
        const size_t role_index = role_position == starts.begin() ? 0 :
            static_cast<size_t>(role_position - starts.begin() - 1);
        const bool ironing = interval.motion.role == erIroning || intervals[begin + role_index].motion.role == erIroning;
        requested.push_back(ironing && s.ironing_temperature ? *s.ironing_temperature : temperature_for_flow(flow, s.table));
    }
    std::vector<double> envelope = requested;
    // Backward feasibility envelope anticipates sustained demand. The forward
    // pass preserves cooling/rise limits and cannot cross protected regions.
    for (size_t n = candidates.size(); n > 1; --n) {
        const size_t i = n - 2;
        const double dt = intervals[candidates[i + 1]].motion.start_time - intervals[candidates[i]].motion.start_time;
        envelope[i] = std::max(envelope[i], envelope[i + 1] - dt / s.rise_time);
    }
    // A short requested excursion must not turn into a heat/cool command
    // cycle. Confirm each prospective target against the already-known future
    // envelope, without waiting at runtime or delaying sustained preheating.
    // The horizon is a target-planning policy, not a measured thermal model.
    const double confirmation_period = std::max(s.smoothing_period,
        s.additional_lead_time + s.temperature_step * (s.rise_time + s.fall_time));
    std::vector<double> future_min(envelope.size()), future_max(envelope.size());
    std::deque<size_t> minima, maxima;
    size_t right = 0;
    for (size_t n = 0; n < candidates.size(); ++n) {
        const double time = intervals[candidates[n]].motion.start_time;
        const double until = std::min(region_end, time + confirmation_period);
        while (right < candidates.size() && intervals[candidates[right]].motion.start_time <= until) {
            while (!minima.empty() && envelope[minima.back()] >= envelope[right]) minima.pop_back();
            while (!maxima.empty() && envelope[maxima.back()] <= envelope[right]) maxima.pop_back();
            minima.push_back(right); maxima.push_back(right);
            ++right;
        }
        while (!minima.empty() && minima.front() < n) minima.pop_front();
        while (!maxima.empty() && maxima.front() < n) maxima.pop_front();
        // Include the partial interval at the right edge; a sparse source-line
        // boundary beyond the horizon must not reject a shorter valid plateau.
        double endpoint = envelope.back();
        if (right < candidates.size()) {
            const double left_time = intervals[candidates[right - 1]].motion.start_time;
            const double right_time = intervals[candidates[right]].motion.start_time;
            const double fraction = (until - left_time) / (right_time - left_time);
            endpoint = envelope[right - 1] + fraction * (envelope[right] - envelope[right - 1]);
        }
        future_min[n] = std::min(envelope[minima.front()], endpoint);
        future_max[n] = std::max(envelope[maxima.front()], endpoint);
    }
    const auto& first = intervals[begin].motion;
    if (!emitted_target || first.reset_target_before) {
        emitted_target = first.target_temperature;
        last_command_time = region_start;
    }
    double continuous = *emitted_target;
    if (!std::isfinite(continuous) || continuous < s.minimum_temperature || continuous > s.maximum_temperature)
        throw std::invalid_argument(_u8L("The temperature entering an adaptive region is outside its working limits"));
    double emitted = *emitted_target;
    double previous_time = region_start;
    for (size_t n = 0; n < candidates.size(); ++n) {
        const auto& motion = intervals[candidates[n]].motion;
        const double dt = std::max(0.0, motion.start_time - previous_time);
        continuous = std::clamp(envelope[n], continuous - dt / s.fall_time, continuous + dt / s.rise_time);
        continuous = std::clamp(continuous, s.minimum_temperature, s.maximum_temperature);
        previous_time = motion.start_time;
        const int direction = continuous + 1e-7 >= emitted + s.temperature_step ? 1 :
                              continuous - 1e-7 <= emitted - s.temperature_step ? -1 : 0;
        const double elapsed = motion.start_time - last_command_time;
        const double required = s.temperature_step * (direction > 0 ? s.rise_time : s.fall_time);
        const bool sustained = direction > 0 ? future_min[n] + 1e-7 >= emitted + s.temperature_step :
                               direction < 0 ? future_max[n] - 1e-7 <= emitted - s.temperature_step : false;
        if (sustained && elapsed + 1e-7 >= std::max(required, s.minimum_command_interval)) {
            emitted += direction * s.temperature_step;
            plan.commands.push_back({motion.source_line, motion.start_time, emitted});
            last_command_time = motion.start_time;
        }
        if (emitted + s.temperature_step < requested[n]) {
            plan.limited_by_transition = true;
            const auto reason = continuous + s.temperature_step < requested[n] ? AdaptiveTemperatureReason::HeatingRate :
                direction > 0 && !sustained ? AdaptiveTemperatureReason::ExcursionFilter :
                direction > 0 && elapsed + 1e-7 < std::max(required, s.minimum_command_interval) ? AdaptiveTemperatureReason::CommandSpacing :
                AdaptiveTemperatureReason::CommandBoundary;
            add_adaptive_temperature_issue(plan.issues, reason, motion.layer_id, motion.start_time, requested[n] - emitted);
        } else if (emitted > requested[n] + s.temperature_step) {
            const auto reason = continuous > envelope[n] + s.temperature_step ? AdaptiveTemperatureReason::CoolingRate :
                envelope[n] > requested[n] + s.temperature_step ? AdaptiveTemperatureReason::Lookahead :
                direction < 0 && !sustained ? AdaptiveTemperatureReason::ExcursionFilter :
                direction < 0 && elapsed + 1e-7 < std::max(required, s.minimum_command_interval) ? AdaptiveTemperatureReason::CommandSpacing :
                AdaptiveTemperatureReason::CommandBoundary;
            add_adaptive_temperature_issue(plan.issues, reason, motion.layer_id, motion.start_time, emitted - requested[n]);
        }
    }
    emitted_target = emitted;
}
}

AdaptiveTemperaturePlan plan_adaptive_temperature(const FlowTimeline& timeline, const AdaptiveTemperatureSettings& settings)
{
    validate_adaptive_temperature_settings(settings);
    AdaptiveTemperaturePlan plan;
    const auto& intervals = timeline.intervals();
    std::optional<double> emitted_target;
    double last_command_time = 0.0;
    size_t begin = 0;
    while (begin < intervals.size()) {
        const auto eligible = [](const auto& m) {
            return !m.protected_region && m.layer_id > 1 && m.source_line > 0 && m.heater_id == 0;
        };
        while (begin < intervals.size() && !eligible(intervals[begin].motion)) {
            if (intervals[begin].motion.reset_target_before) {
                emitted_target = intervals[begin].motion.target_temperature;
                last_command_time = intervals[begin].motion.start_time;
            }
            ++begin;
        }
        if (begin == intervals.size()) break;
        size_t end = begin + 1;
        while (end < intervals.size() && eligible(intervals[end].motion) &&
               intervals[end].motion.filament_id == intervals[begin].motion.filament_id &&
               intervals[end].motion.target_temperature == intervals[begin].motion.target_temperature &&
               !intervals[end].motion.thermal_barrier_before) ++end;
        plan_region(intervals, begin, end, settings, plan, emitted_target, last_command_time);
        begin = end;
    }
    return plan;
}

} // namespace Slic3r
