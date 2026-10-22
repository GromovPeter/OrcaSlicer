#pragma once
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r {
enum class AdaptiveTemperatureReason {
    HeatingRate, CoolingRate, CommandSpacing, ExcursionFilter, Lookahead,
    RegionEntry, MinimumSpeed, BridgeSpeed, ProtectedRecovery, CommandBoundary
};
struct AdaptiveTemperatureIssue {
    AdaptiveTemperatureReason reason{};
    unsigned int layer{};
    double first_time{}, last_time{}, maximum_gap{};
    std::size_t boundaries{};
    bool original_clock{false};
};
// Groups are planning boundaries by layer/reason, not measured heater failures.
void add_adaptive_temperature_issue(std::vector<AdaptiveTemperatureIssue>& issues,
    AdaptiveTemperatureReason reason, unsigned int layer, double time, double gap, bool original_clock = false);
struct AdaptiveTemperatureReport {
    bool coordinated{false};
    double original_time{}, planned_time{}, minimum_target{}, maximum_target{};
    bool has_target_range{false};
    std::size_t commands{}, slowed_moves{}, total_issue_groups{};
    bool constrained{false};
    std::vector<AdaptiveTemperatureIssue> issues;
};
// Only comments are appended, after all motion. Existing move line IDs are stable.
void append_adaptive_temperature_report(const std::string& filename,
    const AdaptiveTemperatureReport& report, std::vector<std::size_t>& line_ends);
std::optional<AdaptiveTemperatureReport> read_adaptive_temperature_report(const std::string& filename);
} // namespace Slic3r
