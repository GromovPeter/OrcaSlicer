#include "LoadedThermalCalibration.hpp"
#include "Config.hpp"
#include "format.hpp"
#include "GCode/AdaptiveTemperaturePlanner.hpp"
#include "I18N.hpp"
#include "InterlayerAdhesionCalibration.hpp"
#include "Model.hpp"
#include "Point.hpp"
#include "Print.hpp"
#include "PrintConfig.hpp"
#include "TriangleMesh.hpp"
#include "Utils.hpp"
#include <boost/nowide/fstream.hpp>
#include <boost/nowide/cstdio.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <ios>
#include <stdexcept>
#include <string>
#include <vector>

namespace Slic3r {
namespace {
constexpr double pi = 3.14159265358979323846;
constexpr double stage_timeout = 120.0;
void require(bool condition, const std::string& message) { if (!condition) throw std::invalid_argument(message); }
double scalar(const DynamicPrintConfig& c, const char* key) { return c.option(key)->getFloat(); }
double filament(const DynamicPrintConfig& c, const char* key) { return c.opt_float(key,0); }
std::vector<FlowTemperaturePoint> table(const DynamicPrintConfig& c)
{
    const auto* stored=c.option<ConfigOptionStrings>("filament_flow_temperature_table");
    if(!stored || stored->values.empty() || stored->get_at(0).empty()) return {};
    const auto& text=stored->get_at(0);
    auto points = parse_flow_temperature_table(text);
    AdaptiveTemperatureSettings settings;
    settings.table = points;
    settings.minimum_temperature = c.opt_int("nozzle_temperature_range_low", 0);
    settings.maximum_temperature = c.opt_int("nozzle_temperature_range_high", 0);
    validate_adaptive_temperature_settings(settings);
    for (size_t i = 1; i < points.size(); ++i)
        require(points[i].temperature >= points[i-1].temperature,
            _u8L("Use a nondecreasing flow/temperature table for loaded thermal calibration."));
    return points;
}
double temperature_at(const std::vector<FlowTemperaturePoint>& points, double q)
{
    auto upper = std::upper_bound(points.begin(), points.end(), q,
        [](double flow, const FlowTemperaturePoint& point) { return flow < point.flow; });
    if (upper == points.begin()) return upper->temperature;
    if (upper == points.end()) return points.back().temperature;
    const auto& a = *(upper-1); const auto& b = *upper;
    return a.temperature+(b.temperature-a.temperature)*(q-a.flow)/(b.flow-a.flow);
}
nlohmann::json protocol(const LoadedThermalPlan& p)
{
    const auto& a = p.input;
    nlohmann::json stages = nlohmann::json::array();
    const auto add = [&](const char* name, double flow, double target) {
        stages.push_back({{"name",name},{"flow",flow},{"target",target}});
    };
    add("baseline",a.low_flow,a.reference_temperature);
    add("cool_low",a.low_flow,a.low_temperature);
    add("heat_low",a.low_flow,a.reference_temperature);
    add("heat_high_low",a.low_flow,a.high_temperature);
    add("load_reference",a.reference_flow,a.high_temperature);
    add("cool_reference",a.reference_flow,a.reference_temperature);
    add("heat_reference",a.reference_flow,a.high_temperature);
    add("load_high",a.high_flow,a.high_temperature);
    add("unload_reference",a.reference_flow,a.high_temperature);
    return {{"version",1},{"kind","orca_loaded_thermal"},{"description",p.description},{"stages",stages},
        {"nominal_flow",a.reference_flow},{"filament_diameter",p.filament_diameter},
        {"fan",p.fan},{"warmup_layers",p.warmup_layers},{"measurement_z",p.warmup_layers*p.layer_height},
        {"height",p.height},{"total_layers",p.total_layers},{"stage_timeout_s",stage_timeout},
        {"total_timeout_s",1500},{"temperature_min",a.low_temperature},{"temperature_max",a.high_temperature}};
}
}

LoadedThermalInput loaded_thermal_defaults(const DynamicPrintConfig& c)
{
    LoadedThermalInput a;
    const double limit = filament(c,"filament_max_volumetric_speed");
    require(std::isfinite(limit) && limit > 1,
        _u8L("Set a positive filament maximum volumetric speed above 1 mm3/s before generating this calibration."));
    a.high_flow = std::floor(limit*1000)/1000;
    a.reference_flow = std::floor(a.high_flow*500)/1000;
    a.low_flow = std::floor(std::max(0.1,a.high_flow/10)*1000)/1000;
    const double minimum = c.opt_int("nozzle_temperature_range_low",0);
    const double maximum = c.opt_int("nozzle_temperature_range_high",0);
    a.reference_temperature = c.opt_int("nozzle_temperature",0);
    const auto points = table(c);
    if (!points.empty()) {
        a.low_temperature = temperature_at(points,a.low_flow);
        a.reference_temperature = std::max(a.reference_temperature,temperature_at(points,a.reference_flow));
        a.high_temperature = std::max(a.reference_temperature+5,temperature_at(points,a.high_flow));
        a.high_temperature = std::min(maximum,a.high_temperature);
        a.low_temperature = std::min(a.low_temperature,a.reference_temperature-5);
    } else {
        a.low_temperature = std::max(minimum,a.reference_temperature-10);
        a.high_temperature = std::min(maximum,a.reference_temperature+10);
    }
    a.low_temperature=std::ceil(a.low_temperature*10)/10;
    a.reference_temperature=std::ceil(a.reference_temperature*10)/10;
    a.high_temperature=std::ceil(a.high_temperature*10)/10;
    return a;
}

LoadedThermalPlan plan_loaded_thermal_calibration(const DynamicPrintConfig& c, const LoadedThermalInput& a)
{
    for (double v : {a.low_temperature,a.reference_temperature,a.high_temperature,a.low_flow,a.reference_flow,a.high_flow})
        require(std::isfinite(v),_u8L("Calibration temperatures and flows must be finite."));
    require(a.low_flow > 0 && a.low_flow < a.reference_flow && a.reference_flow < a.high_flow,
        _u8L("Calibration flows must be positive and increase from low to reference to high."));
    require(a.reference_temperature-a.low_temperature+1e-6 >= 5 && a.high_temperature-a.reference_temperature+1e-6 >= 5,
        _u8L("Use at least 5 C between each of the three calibration temperatures."));
    require(a.low_temperature >= c.opt_int("nozzle_temperature_range_low",0) &&
        a.high_temperature <= c.opt_int("nozzle_temperature_range_high",0),
        _u8L("Calibration temperatures must stay within the selected filament's working limits."));
    require(a.low_flow >= 0.05 && a.high_flow <= 200 && 100*a.low_flow/a.reference_flow >= 1 && 100*a.high_flow/a.reference_flow <= 1000,
        _u8L("Use calibration flows between 0.05 and 200 mm3/s and speed scaling between 1 and 1000 percent."));
    require(c.option<ConfigOptionFloats>("nozzle_diameter")->values.size()==1 && !c.opt_bool("belt_printer") && c.opt_enum<GCodeFlavor>("gcode_flavor")==gcfKlipper,
        _u8L("Loaded thermal calibration requires one Klipper hotend on a horizontal bed."));
    const double cap = filament(c,"filament_max_volumetric_speed");
    require(cap > 0 && a.high_flow <= cap,
        _u8L("Calibration flow exceeds the selected filament's maximum volumetric speed."));
    const auto points = table(c);
    if (!points.empty())
        require(a.low_temperature+1e-6 >= temperature_at(points,a.low_flow) &&
            a.reference_temperature+1e-6 >= temperature_at(points,a.reference_flow) &&
            a.high_temperature+1e-6 >= temperature_at(points,a.high_flow),
            _u8L("A calibration target is below the selected filament's flow/temperature table. Raise the target or reduce its flow."));
    const BedType selected_plate=c.opt_enum<BedType>("curr_bed_type");
    require(!get_bed_temp_key(selected_plate).empty() && !get_bed_temp_1st_layer_key(selected_plate).empty(),
        _u8L("Select a supported build plate before generating the calibration."));
    LoadedThermalPlan p; p.input = a;
    const double nozzle = filament(c,"nozzle_diameter");
    p.filament_diameter = filament(c,"filament_diameter");
    require(std::isfinite(p.filament_diameter) && p.filament_diameter >= 1 && p.filament_diameter <= 3,
        _u8L("The selected filament diameter is invalid."));
    p.layer_height = scalar(c,"layer_height");
    const auto* width = c.option<ConfigOptionFloatOrPercent>("outer_wall_line_width");
    p.width = width->get_abs_value(nozzle);
    if (p.width <= 0) p.width = 1.2*nozzle;
    AdhesionCalibrationInput sizing;
    sizing.width = p.width; sizing.layer_height = p.layer_height; sizing.flow = a.reference_flow;
    sizing.start_temperature = static_cast<int>(std::ceil(a.high_temperature));
    sizing.end_temperature = static_cast<int>(std::floor(a.low_temperature));
    sizing.temperature_step = sizing.start_temperature-sizing.end_temperature;
    sizing.disable_layer_time_slowdown = true;
    DynamicPrintConfig sizing_config = c;
    sizing_config.set_key_value("layer_change_gcode",new ConfigOptionString(""));
    sizing_config.set_key_value("before_layer_change_gcode",new ConfigOptionString(""));
    // Thermal-response references are being measured, so do not use them to size this fixture.
    sizing_config.set_key_value("adaptive_temperature_fall_time",new ConfigOptionFloat(1));
    sizing_config.set_key_value("adaptive_temperature_lead_time",new ConfigOptionFloat(0));
    auto dimensions = plan_adhesion_calibration(sizing_config,sizing);
    const double high_speed = dimensions.speed*a.high_flow/a.reference_flow;
    double accel = filament(c,"outer_wall_acceleration");
    if (accel <= 0) accel = filament(c,"default_acceleration");
    for (const char* key : {"machine_max_acceleration_x","machine_max_acceleration_y"}) {
        const double value = filament(c,key); if (value > 0) accel = accel > 0 ? std::min(accel,value) : value;
    }
    require(accel > 0,_u8L("The printer acceleration limit is unavailable."));
    for (const char* key : {"machine_max_speed_x","machine_max_speed_y"}) {
        const double value = filament(c,key);
        require(value <= 0 || high_speed <= value,_u8L("The highest calibration flow requires a speed above the printer limit."));
    }
    sizing.minimum_diameter = std::max(dimensions.diameter,4*high_speed*high_speed/accel+p.width);
    dimensions = plan_adhesion_calibration(sizing_config,sizing);
    p.diameter = dimensions.diameter; p.speed = dimensions.speed;
    p.warmup_layers = std::max(7u,dimensions.warmup_layers);
    // Reserve deposited volume for every stage's full timeout, with acknowledgement and end margins.
    const double volume = 1.15*(stage_timeout+5)*(4*a.low_flow+4*a.reference_flow+a.high_flow);
    const double volume_per_turn = pi*(p.diameter-p.width)*a.reference_flow/p.speed;
    p.total_layers = p.warmup_layers+static_cast<unsigned>(std::ceil(volume/volume_per_turn))+3;
    p.height = p.total_layers*p.layer_height;
    require(p.height+1 <= scalar(c,"printable_height"),
        _u8L("The thermal calibration cylinder does not fit the printer height. Reduce the test flows."));
    p.fan = std::clamp(filament(c,"fan_max_speed")/100.0,0.0,1.0);
    const auto* filament_names=c.option<ConfigOptionStrings>("filament_settings_id");
    const std::string filament_name=filament_names && !filament_names->values.empty() ? filament_names->get_at(0) : std::string{};
    const BedType plate=c.opt_enum<BedType>("curr_bed_type");
    const auto* printer_name=c.option<ConfigOptionString>("printer_settings_id");
    p.description = Slic3r::format(_u8L("Printer: %1%\nFilament: %2%\nPlate: %3%; bed initial/other: %4% / %5% C\n"),
        printer_name ? printer_name->value : std::string{},filament_name,c.opt_serialize("curr_bed_type"),
        c.opt_int(get_bed_temp_1st_layer_key(plate),0),c.opt_int(get_bed_temp_key(plate),0));
    p.description += Slic3r::format(_u8L("Nozzle %1% mm; filament %2% mm; layer %3% mm; wall %4% mm\nCylinder %5% x %6% mm; part fan duty %7% / 100\nFlow %8% / %9% / %10% mm3/s; temperature %11% / %12% / %13% C\nUses selected start/end G-code and bed temperatures. Layer-time slowdown and adaptive temperature/flow are disabled in this file."),
        nozzle,p.filament_diameter,p.layer_height,p.width,p.diameter,p.height,p.fan*100,
        a.low_flow,a.reference_flow,a.high_flow,a.low_temperature,a.reference_temperature,a.high_temperature);
    return p;
}

void export_loaded_thermal_calibration(const DynamicPrintConfig& selected, const LoadedThermalPlan& plan, const std::string& filename)
{
    const auto p = plan_loaded_thermal_calibration(selected,plan.input);
    AdhesionCalibrationPlan patch_plan; patch_plan.input.width=p.width; patch_plan.input.layer_height=p.layer_height;
    patch_plan.input.start_temperature=static_cast<int>(std::ceil(p.input.reference_temperature));
    patch_plan.input.disable_layer_time_slowdown=true; patch_plan.speed=p.speed;
    DynamicPrintConfig config=selected;
    config.apply(adhesion_process_patch(patch_plan));config.apply(adhesion_filament_patch(patch_plan));
    // Retain the selected first-layer setpoint; the ordinary second-layer target prepares the reference load.
    config.set_key_value("nozzle_temperature_initial_layer",selected.option("nozzle_temperature_initial_layer")->clone());
    config.set_key_value("nozzle_temperature",new ConfigOptionInts{static_cast<int>(std::ceil(p.input.reference_temperature))});
    config.set_deserialize_strict({{"print_sequence","by layer"},{"print_flow_ratio","1"},{"set_other_flow_ratios","0"},
        {"bottom_shell_layers","0"},{"bottom_shell_thickness","0"},{"gcode_comments","1"},
        {"enable_overhang_speed","0"},{"slow_down_layers","1"}});
    const int fan=static_cast<int>(std::lround(p.fan*100));
    config.set_key_value("fan_min_speed",new ConfigOptionFloats{double(fan)});config.set_key_value("fan_max_speed",new ConfigOptionFloats{double(fan)});
    config.set_key_value("overhang_fan_speed",new ConfigOptionInts{fan});
    config.set_key_value("reduce_fan_stop_start_freq",new ConfigOptionBools{true});
    config.set_key_value("layer_change_gcode",new ConfigOptionString(
        "{if layer_num == "+std::to_string(p.warmup_layers)+"}M400\n{endif}SET_PRINT_STATS_INFO TOTAL_LAYER={total_layer_count} CURRENT_LAYER={layer_num + 1}\n"));
    config.set_key_value("before_layer_change_gcode",new ConfigOptionString(""));
    config.set_key_value("time_lapse_gcode",new ConfigOptionString(""));
    Model model;
    auto* object=model.add_object();object->name=_u8L("Loaded thermal calibration");
    object->add_volume(make_cylinder(p.diameter/2,p.height));
    const auto& bed=config.option<ConfigOptionPoints>("printable_area")->values;
    double lx=bed.front().x(),hx=lx,ly=bed.front().y(),hy=ly;
    for(const auto& point:bed){lx=std::min(lx,point.x());hx=std::max(hx,point.x());ly=std::min(ly,point.y());hy=std::max(hy,point.y());}
    object->add_instance()->set_offset(Vec3d((lx+hx)/2,(ly+hy)/2,0));object->ensure_on_bed();
    Print print; print.set_status_silent(); print.auto_assign_extruders(object); print.apply(model,config);
    const auto validation=print.validate();
    if(!validation.string.empty()) throw std::invalid_argument(validation.string);
    print.process();
    const std::string temporary=filename+".sliced.gcode";
    const std::string assembled=filename+".thermal.tmp";
    const ScopeGuard cleanup([&]{boost::nowide::remove(temporary.c_str());boost::nowide::remove(assembled.c_str());});
    print.export_gcode(temporary,nullptr);
    boost::nowide::ifstream input(temporary,std::ios::binary);
    boost::nowide::ofstream output(assembled,std::ios::binary|std::ios::trunc);
    if(!input || !output) throw std::runtime_error(_u8L("Cannot write the generated calibration file."));
    output << "; _ORCA_LOADED_THERMAL_V1 " << protocol(p).dump() << '\n' << input.rdbuf();
    output.flush();
    if(!output || input.bad()) throw std::runtime_error(_u8L("Failed to save the generated calibration file."));
    output.close();
    if(!output || rename_file(assembled,filename)) throw std::runtime_error(_u8L("Failed to save the generated calibration file."));
}
} // namespace Slic3r
