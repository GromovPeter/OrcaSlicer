#pragma once
#include "CustomGCode.hpp"
#include <optional>
#include <string>
#include <vector>

namespace Slic3r {
class DynamicPrintConfig;
class FlowTimeline;
class Model;
struct AdhesionCalibrationInput {
    double width{0.5}, layer_height{0.2}, flow{5.0};
    int start_temperature{230}, end_temperature{200}, temperature_step{1};
    double stable_height{0.0}, minimum_diameter{0.0}; // zero selects automatic sizing
    bool disable_layer_time_slowdown{false};
};
struct AdhesionCalibrationBand {
    int temperature{};
    double begin_z{}, stable_z{}, end_z{};
};
struct AdhesionCalibrationPlan {
    AdhesionCalibrationInput input;
    double diameter{}, height{}, speed{}, turn_time{}, stable_height{}, transition_height{};
    double measurement_z{}, estimated_wall_time{}, estimated_wall_volume{};
    unsigned warmup_layers{}, stable_layers{}, transition_layers{}, total_layers{};
    std::string conditions;
    std::vector<AdhesionCalibrationBand> bands;
};
struct AdhesionHeightResult {
    enum class Kind { Outside, Warmup, Transition, Stable } kind{Kind::Outside};
    int temperature{}, previous_temperature{};
};
AdhesionCalibrationPlan plan_adhesion_calibration(const DynamicPrintConfig&, const AdhesionCalibrationInput&);
DynamicPrintConfig adhesion_process_patch(const AdhesionCalibrationPlan&);
DynamicPrintConfig adhesion_filament_patch(const AdhesionCalibrationPlan&);
CustomGCode::Info adhesion_custom_gcodes(const AdhesionCalibrationPlan&);
bool adhesion_has_height_map(const CustomGCode::Info&);
std::optional<AdhesionCalibrationPlan> adhesion_plan_from_codes(const CustomGCode::Info&);
AdhesionHeightResult adhesion_temperature_at_height(const AdhesionCalibrationPlan&, double height);
void validate_adhesion_project(const Model&, const DynamicPrintConfig&, const AdhesionCalibrationPlan&);
void validate_adhesion_flow(const FlowTimeline&, const AdhesionCalibrationPlan&);
std::string adhesion_table_with_point(const std::string& table, double flow, double temperature);
} // namespace Slic3r
