#pragma once
#include <string>

namespace Slic3r {
class DynamicPrintConfig;
struct LoadedThermalInput {
    double low_temperature{}, reference_temperature{}, high_temperature{};
    double low_flow{}, reference_flow{}, high_flow{};
};
struct LoadedThermalPlan {
    LoadedThermalInput input;
    double diameter{}, height{}, speed{}, layer_height{}, width{}, filament_diameter{}, fan{};
    unsigned warmup_layers{}, total_layers{};
    std::string description;
};
LoadedThermalInput loaded_thermal_defaults(const DynamicPrintConfig&);
LoadedThermalPlan plan_loaded_thermal_calibration(const DynamicPrintConfig&, const LoadedThermalInput&);
void export_loaded_thermal_calibration(const DynamicPrintConfig&, const LoadedThermalPlan&, const std::string& filename);
} // namespace Slic3r
