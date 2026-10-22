#include "AdaptiveFlowPlanner.hpp"
#include "AdaptiveTemperatureReport.hpp"
#include "AdaptiveTemperaturePlanner.hpp"
#include "FlowTimeline.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/I18N.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/libslic3r.h"
#include <boost/nowide/fstream.hpp>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <deque>
#include <ios>
#include <map>
#include <stdexcept>
#include <set>
#include <string>
#include <vector>

namespace Slic3r {
namespace {
struct FlowNode {
    unsigned int line{};
    double time{}, distance{}, volume{}, duration{}, feedrate{}, initial_target{};
    bool fixed_speed{false};
    unsigned int layer{};
};
double temperature(double flow, const std::vector<FlowTemperaturePoint>& table)
{
    for (size_t i = 1; i < table.size(); ++i) {
        if (flow <= table[i].flow) {
            const auto& a = table[i - 1]; const auto& b = table[i];
            const double fraction = std::clamp((flow - a.flow) / (b.flow - a.flow), 0.0, 1.0);
            return a.temperature + fraction * (b.temperature - a.temperature);
        }
    }
    return table.back().temperature;
}
double flow_at_temperature(double value, double ceiling, const std::vector<FlowTemperaturePoint>& table)
{
    if (value >= temperature(ceiling, table) - 1e-9) return ceiling;
    if (value <= table.front().temperature) return std::min(ceiling, table.front().flow);
    for (size_t i = 1; i < table.size(); ++i)
        if (value <= table[i].temperature) {
            const auto& a = table[i - 1]; const auto& b = table[i];
            return std::min(ceiling, a.flow + (b.flow - a.flow) *
                (value - a.temperature) / (b.temperature - a.temperature));
        }
    return ceiling;
}
void plan_region(const std::vector<FlowNode>& nodes, const AdaptiveTemperatureSettings& s,
                 double minimum_speed, AdaptiveFlowPlan& result)
{
    if (nodes.empty()) return;
    std::vector<double> caps;
    for (const auto& node : nodes) caps.push_back(temperature(node.feedrate / 60.0 * node.volume / node.distance, s.table));
    // Do not turn a short gap/overhang into a long thermal slowdown. A lower
    // reference must persist through the configured neighbourhood. The final
    // speed clamp still retains every original local ceiling, including these
    // short low-flow moves. Long source moves remain anchors at their start.
    if (s.smoothing_period > 0) {
        const auto reference = caps;
        std::deque<size_t> maxima;
        size_t right = 0;
        for (size_t i = 0; i < nodes.size(); ++i) {
            while (right < nodes.size() && nodes[right].time <= nodes[i].time + 0.5 * s.smoothing_period) {
                while (!maxima.empty() && reference[maxima.back()] <= reference[right]) maxima.pop_back();
                maxima.push_back(right++);
            }
            while (!maxima.empty() && nodes[maxima.front()].time < nodes[i].time - 0.5 * s.smoothing_period)
                maxima.pop_front();
            // A long atomic move has no interior source boundaries to anchor
            // its sustained low flow. Retain its own reference explicitly.
            caps[i] = nodes[i].duration >= s.smoothing_period ? reference[i] : reference[maxima.front()];
        }
    }
    // These settings are provisional target limits, not identified heater rates.
    const double span = s.table.back().temperature - s.table.front().temperature;
    const double rise = std::max(s.rise_time, s.smoothing_period / span);
    const double fall = std::max(s.fall_time, s.smoothing_period / span);
    caps.front() = std::min(caps.front(), nodes.front().initial_target);
    for (size_t i = 1; i < nodes.size(); ++i)
        caps[i] = std::min(caps[i], caps[i - 1] + (nodes[i].time - nodes[i - 1].time) / rise);
    for (size_t i = nodes.size(); i > 1; --i)
        caps[i - 2] = std::min(caps[i - 2], caps[i - 1] + (nodes[i - 1].time - nodes[i - 2].time) / fall);
    if (std::abs(caps.front() - nodes.front().initial_target) > s.temperature_step) {
        result.limited_by_boundary = true;
        add_adaptive_temperature_issue(result.issues, AdaptiveTemperatureReason::RegionEntry,
            nodes.front().layer, nodes.front().time, caps.front() - nodes.front().initial_target, true);
    }
    for (size_t i = 0; i < nodes.size(); ++i) {
        const auto& node = nodes[i];
        const double density = node.volume / node.distance;
        const double ceiling = node.feedrate / 60.0 * density;
        const double requested = 60.0 * flow_at_temperature(caps[i], ceiling, s.table) / density;
        const double floor = node.fixed_speed ? node.feedrate : std::min(node.feedrate, 60.0 * minimum_speed);
        const double limited = std::max(floor, std::min(requested, node.feedrate));
        if (temperature(limited / 60.0 * density, s.table) > caps[i] + s.temperature_step) {
            result.limited_by_boundary = true;
            add_adaptive_temperature_issue(result.issues,
                node.fixed_speed ? AdaptiveTemperatureReason::BridgeSpeed : AdaptiveTemperatureReason::MinimumSpeed,
                node.layer, node.time, temperature(limited / 60.0 * density, s.table) - caps[i], true);
        }
        // Round down without crossing the configured lower speed limit.
        const double feedrate = std::max(floor, std::floor(limited * 1000.0) / 1000.0);
        if (feedrate < node.feedrate - 0.001)
            result.commands.push_back({node.line, feedrate});
    }
}
}
AdaptiveFlowPlan plan_adaptive_flow(const FlowTimeline& timeline,
    const std::map<unsigned int, double>& feedrates, const AdaptiveTemperatureSettings& settings, double minimum_speed)
{
    validate_adaptive_temperature_settings(settings);
    if (!std::isfinite(minimum_speed) || minimum_speed <= 0)
        throw std::invalid_argument(_u8L("The coordinated printing speed floor must be positive."));
    for (size_t i = 1; i < settings.table.size(); ++i)
        if (settings.table[i].temperature <= settings.table[i - 1].temperature)
            throw std::invalid_argument(_u8L("Coordinated flow requires a strictly increasing flow/temperature table."));
    AdaptiveFlowPlan result;
    // An arc may have recovery in only its first subdivided interval. Exclude
    // its entire source command so recovery speed is preserved too.
    std::set<unsigned int> recovery_lines;
    for (const auto& interval : timeline.intervals())
        if (interval.recovery_distance > 1e-6) recovery_lines.insert(interval.motion.source_line);
    std::vector<FlowNode> nodes;
    unsigned int filament = 0;
    double target = 0;
    unsigned int last_recovery_line = 0;
    auto finish = [&]() { plan_region(nodes, settings, minimum_speed, result); nodes.clear(); };
    for (const auto& interval : timeline.intervals()) {
        const auto& m = interval.motion;
        if (m.protected_region || m.layer_id <= 1 || !m.heater_id || *m.heater_id != 0 ||
            m.thermal_barrier_before || (!nodes.empty() && (m.filament_id != filament || m.target_temperature != target))) {
            finish();
            if (m.protected_region || m.layer_id <= 1 || !m.heater_id || *m.heater_id != 0) continue;
        }
        filament = m.filament_id; target = m.target_temperature;
        const double volume = interval.deposited_volume(0, m.duration);
        if (volume <= 1e-9 || m.distance <= 1e-9 || m.kind != FlowMotionKind::Extrusion) continue;
        if (recovery_lines.count(m.source_line) != 0) {
            finish(); result.limited_by_boundary = true;
            if (last_recovery_line != m.source_line)
                add_adaptive_temperature_issue(result.issues, AdaptiveTemperatureReason::ProtectedRecovery,
                    m.layer_id, m.start_time, 0, true);
            last_recovery_line = m.source_line;
            continue;
        }
        const auto feedrate = feedrates.find(m.source_line);
        if (feedrate == feedrates.end() || !std::isfinite(feedrate->second) || feedrate->second <= 0)
            throw std::invalid_argument(_u8L("A coordinated extrusion has no valid source feedrate."));
        const bool fixed = m.role == erBridgeInfill || m.role == erInternalBridgeInfill;
        if (!nodes.empty() && nodes.back().line == m.source_line) {
            nodes.back().distance += m.distance; nodes.back().volume += volume; nodes.back().duration += m.duration;
            nodes.back().fixed_speed = nodes.back().fixed_speed || fixed;
        } else nodes.push_back({m.source_line, m.start_time, m.distance, volume, m.duration, feedrate->second, m.target_temperature, fixed, m.layer_id});
    }
    finish();
    return result;
}

std::map<unsigned int, double> read_adaptive_flow_feedrates(const std::string& filename)
{
    std::map<unsigned int, double> rates;
    GCodeReader reader;
    unsigned int number = 0;
    double feedrate = 0;
    if (!reader.parse_file(filename, [&](GCodeReader&, const GCodeReader::GCodeLine& line) {
        ++number;
        float override_value = 100;
        if (line.cmd_is("G20") || line.cmd_is("M200") ||
            ((line.cmd_is("M220") || line.cmd_is("M221")) && line.has_value('S', override_value) && override_value != 100))
            throw std::invalid_argument(_u8L("Coordinated flow does not support inch units or runtime speed/flow overrides."));
        if (line.cmd_is("G0") || line.cmd_is("G1") || line.cmd_is("G2") || line.cmd_is("G3")) {
            if (line.has(F)) feedrate = line.precise_value(F);
            rates.emplace(number, feedrate);
        }
    })) throw std::runtime_error(_u8L("Could not read source feedrates for coordinated flow."));
    return rates;
}

void write_adaptive_flow_plan(const std::string& filename, const AdaptiveFlowPlan& plan)
{
    if (plan.commands.empty()) return;
    const std::string temporary = filename + ".thermal_flow_tmp";
    boost::nowide::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error(_u8L("Could not open the coordinated flow output."));
    GCodeReader reader;
    unsigned int number = 0;
    size_t command = 0;
    double original_feedrate = 0, output_feedrate = 0;
    const bool read = reader.parse_file(filename, [&](GCodeReader&, const GCodeReader::GCodeLine& original) {
        ++number;
        if (command < plan.commands.size() && plan.commands[command].source_line < number)
            throw std::logic_error("Coordinated flow command lost its source line");
        const bool motion = original.cmd_is("G0") || original.cmd_is("G1") || original.cmd_is("G2") || original.cmd_is("G3");
        if (motion && original.has(F)) original_feedrate = original.precise_value(F);
        double desired = original_feedrate;
        bool changed = false;
        if (command < plan.commands.size() && plan.commands[command].source_line == number) {
            desired = plan.commands[command++].feedrate_mm_min;
            if (!motion || !std::isfinite(desired) || desired <= 0 || desired > original_feedrate + 1e-6)
                throw std::logic_error("Coordinated flow cannot increase a source feedrate");
            changed = true;
        }
        if (motion && (changed || (!original.has(F) && std::abs(output_feedrate - desired) > 0.001))) {
            auto line = original;
            line.set(F, static_cast<float>(desired), 3);
            output << line.raw() << '\n';
            output_feedrate = desired;
        } else {
            output << original.raw() << '\n';
            if (motion && original.has(F)) output_feedrate = original_feedrate;
        }
    });
    output.flush();
    if (!read || !output || command != plan.commands.size())
        throw std::runtime_error(_u8L("Could not write the coordinated flow plan."));
    output.close();
    if (rename_file(temporary, filename)) throw std::runtime_error(_u8L("Could not replace the coordinated flow output."));
}
} // namespace Slic3r
