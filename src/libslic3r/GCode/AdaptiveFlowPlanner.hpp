#pragma once

#include "AdaptiveTemperaturePlanner.hpp"
#include "FlowTimeline.hpp"
#include "AdaptiveTemperatureReport.hpp"
#include <map>
#include <string>
#include <vector>

namespace Slic3r {
struct AdaptiveFlowCommand { unsigned int source_line{}; double feedrate_mm_min{}; };
struct AdaptiveFlowPlan {
    std::vector<AdaptiveFlowCommand> commands;
    bool limited_by_boundary{false};
    std::vector<AdaptiveTemperatureIssue> issues;
};
// Conservative nominal-flow envelope on the native baseline clock. Travel time
// is available for transitions, but travel/retraction speeds are never lowered.
AdaptiveFlowPlan plan_adaptive_flow(const FlowTimeline& timeline,
    const std::map<unsigned int, double>& feedrates_mm_min,
    const AdaptiveTemperatureSettings& settings, double minimum_speed_mm_s);
std::map<unsigned int, double> read_adaptive_flow_feedrates(const std::string& filename);
// Preserves source line count and all words except F. Restores modal F on the
// next original motion when needed, so scripts/travel retain their original speed.
void write_adaptive_flow_plan(const std::string& filename, const AdaptiveFlowPlan& plan);
} // namespace Slic3r
