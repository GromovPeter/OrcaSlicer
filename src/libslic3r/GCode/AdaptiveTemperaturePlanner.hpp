#pragma once

#include "FlowTimeline.hpp"
#include "AdaptiveTemperatureReport.hpp"

#include <string_view>
#include <vector>

namespace Slic3r {

struct FlowTemperaturePoint { double flow; double temperature; };
struct AdaptiveTemperatureSettings
{
    std::vector<FlowTemperaturePoint> table;
    double minimum_temperature{190.0};
    double maximum_temperature{260.0};
    double smoothing_period{5.0};
    double rise_time{2.0}; // seconds per degree of commanded target change
    double fall_time{3.0};
    double additional_lead_time{0.0};
    double minimum_command_interval{2.0};
    int temperature_step{1};
    bool deposition_time_only{false};
};

struct AdaptiveTemperatureCommand
{
    unsigned int source_line;
    double time;
    int temperature;
};
struct AdaptiveTemperaturePlan
{
    std::vector<AdaptiveTemperatureCommand> commands;
    bool limited_by_transition{false};
    std::vector<AdaptiveTemperatureIssue> issues;
};

std::vector<FlowTemperaturePoint> parse_flow_temperature_table(std::string_view text);
void validate_adaptive_temperature_settings(const AdaptiveTemperatureSettings& settings);
AdaptiveTemperaturePlan plan_adaptive_temperature(const FlowTimeline& timeline, const AdaptiveTemperatureSettings& settings);

} // namespace Slic3r
