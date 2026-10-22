#pragma once

#include <istream>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r {
class DynamicPrintConfig;
struct ThermalSample {
    double time_s{}, target_c{}, temperature_c{};
    std::optional<double> power, fan, flow_mm3_s;
    // Commanded extrusion is deliberately separate from deposited flow.
    std::optional<double> commanded_flow_mm3_s, auxiliary_fan, chamber_fan;
    std::string stage;
};

enum class ThermalIssue { UnstableBaseline, DataGap, Unsettled, MissingConditions, ChangingConditions, NoisyResponse };
struct ThermalTransition {
    double time_s{}, from_c{}, to_c{}, delay_s{}, seconds_per_degree{}, overshoot_c{};
    std::optional<double> settling_s;
    std::vector<ThermalIssue> issues;
    bool usable() const { return issues.empty(); }
};
struct ThermalResponse {
    std::vector<ThermalTransition> transitions;
    std::optional<double> rise_seconds_per_degree, fall_seconds_per_degree;
    double delay_s{};
};
struct LoadedThermalStage {
    std::string name;
    double begin_s{}, end_s{}, target_c{};
    std::optional<double> commanded_flow_mm3_s;
    bool load_change{false};
    std::optional<ThermalTransition> transition;
};
// Analyze each recorded stage independently so a later load change cannot
// contaminate the preceding fixed-load temperature step. No sidecar is trusted.
std::vector<LoadedThermalStage> analyze_loaded_thermal_response(const std::vector<ThermalSample>& samples);
// Provisional target-slew policy, distinct from measured sensor slope.
struct ThermalTargetProposal {
    double rise_time{}, fall_time{}, lead_time{}, smoothing_period{5.0};
    double measured_rise_time{}, measured_fall_time{};
};
// Needs accepted heating and cooling under positive printing load.
std::optional<ThermalTargetProposal> propose_thermal_target_settings(const std::vector<ThermalSample>& samples);
// A whitelist patch; never touches filament data or enables adaptation.
DynamicPrintConfig thermal_target_settings_patch(const ThermalTargetProposal& proposal, bool include_smoothing);
// Numeric CSV, decimal dot; time_s,target_c,temperature_c are required.
// Optional columns: power and fan (0..1), flow_mm3_s (>=0).
std::vector<ThermalSample> read_thermal_response_csv(std::istream& input);
ThermalResponse analyze_thermal_response(const std::vector<ThermalSample>& samples);
} // namespace Slic3r
