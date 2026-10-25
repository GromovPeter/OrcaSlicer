#include "libslic3r/LoadedThermalCalibration.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "test_utils.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <nlohmann/json.hpp>
#include <fstream>
#include <ios>
#include <cmath>
#include <iterator>
#include <string>
#include <stdexcept>

using namespace Slic3r;
namespace {
DynamicPrintConfig config(double nozzle)
{
    auto c=DynamicPrintConfig::full_print_config();
    c.set_deserialize_strict({{"gcode_flavor","klipper"},{"printable_area","0x0,220x0,220x220,0x220"},
        {"printable_height","180"},{"filament_max_volumetric_speed","6"},{"filament_flow_ratio","1"},
        {"nozzle_temperature","230"},{"nozzle_temperature_initial_layer","235"},
        {"nozzle_temperature_range_low","190"},{"nozzle_temperature_range_high","270"},
        {"default_acceleration","3000"},{"outer_wall_acceleration","2000"},
        {"machine_start_gcode","; SELECTED_PRINTER_START\nM190 S[bed_temperature_initial_layer_single]\nM104 S[nozzle_temperature_initial_layer]\nM109 S[nozzle_temperature_initial_layer]\n"},
        {"machine_end_gcode","; SELECTED_PRINTER_END\nM104 S0\n"},
        {"curr_bed_type","Textured PEI Plate"},{"textured_plate_temp_initial_layer","63"},{"textured_plate_temp","61"},
        {"time_lapse_gcode","UNWANTED_TIMELAPSE_COMMAND\n"},{"fan_max_speed","35"},{"filament_flow_temperature_table",""}});
    c.set_key_value("nozzle_diameter",new ConfigOptionFloats{nozzle});
    c.set_key_value("layer_height",new ConfigOptionFloat(nozzle/2));
    c.set_key_value("outer_wall_line_width",new ConfigOptionFloatOrPercent(nozzle*1.2,false));
    return c;
}
}
TEST_CASE("Generated thermal fixtures follow selected geometry filament and start code", "[LoadedThermalCalibration][GCode]")
{
    const double nozzle=GENERATE(0.4,0.6);
    auto c=config(nozzle);
    if(nozzle==0.6) {
        c.set_deserialize_strict({{"printable_area","-100x-100,100x-100,100x100,-100x100"},
            {"filament_diameter","2.85"},{"nozzle_temperature","250"},{"nozzle_temperature_initial_layer","255"}});
    }
    const auto before=c.opt_serialize("machine_start_gcode");
    const auto plan=plan_loaded_thermal_calibration(c,loaded_thermal_defaults(c));
    CHECK_THAT(plan.layer_height,Catch::Matchers::WithinAbs(nozzle/2,1e-8));
    CHECK(plan.height<180);
    CHECK(plan.diameter<200);
    ScopedTemporaryFile output(".gcode");
    export_loaded_thermal_calibration(c,plan,output.string());
    std::ifstream input(output.string(),std::ios::binary);
    std::string first;std::getline(input,first);
    const std::string marker="; _ORCA_LOADED_THERMAL_V1 ";
    REQUIRE(first.rfind(marker,0)==0);
    const auto metadata=nlohmann::json::parse(first.substr(marker.size()));
    CHECK(metadata.at("stages").size()==9);
    CHECK_THAT(metadata.at("filament_diameter").get<double>(),Catch::Matchers::WithinAbs(nozzle==0.6 ? 2.85 : 1.75,1e-6));
    const std::string text((std::istreambuf_iterator<char>(input)),std::istreambuf_iterator<char>());
    CHECK(text.find("SELECTED_PRINTER_START")!=std::string::npos);
    CHECK(text.find("SELECTED_PRINTER_END")!=std::string::npos);
    CHECK(text.find("SET_PRINT_STATS_INFO TOTAL_LAYER=")!=std::string::npos);
    CHECK(text.find("CURRENT_LAYER=8")!=std::string::npos);
    CHECK(text.find("_ORCA_ADAPTIVE_TEMPERATURE")==std::string::npos);
    CHECK(text.find("Z_OFFSET=")==std::string::npos);
    CHECK(text.find("UNWANTED_TIMELAPSE_COMMAND")==std::string::npos);
    bool bed_target=false;std::string bed_commands;
    GCodeReader reader;
    reader.parse_buffer(text,[&](GCodeReader&,const GCodeReader::GCodeLine& line) {
        float temperature=0;
        if(line.cmd_is("M140") || line.cmd_is("M190")) bed_commands+=line.raw()+"\n";
        if((line.cmd_is("M140") || line.cmd_is("M190")) && line.has_value('S',temperature) && std::abs(temperature-63)<.01)
            bed_target=true;
        if(line.cmd_is("G1") && line.has_e() && line.e()>0) {
            const double minimum=nozzle==0.6 ? -100.0 : 0.0;
            const double maximum=nozzle==0.6 ? 100.0 : 220.0;
            if(line.has_x()) {CHECK(line.x()>=minimum-.001);CHECK(line.x()<=maximum+.001);}
            if(line.has_y()) {CHECK(line.y()>=minimum-.001);CHECK(line.y()<=maximum+.001);}
        }
    });
    INFO("Native bed commands: " << bed_commands);
    CHECK(bed_target);
    CHECK(text.find("START_PRINT") == std::string::npos);
    CHECK(c.opt_serialize("machine_start_gcode")==before);
}
TEST_CASE("Loaded thermal calibration rejects oversized or incompatible test conditions", "[LoadedThermalCalibration]")
{
    auto c=config(0.4);auto input=loaded_thermal_defaults(c);
    input.high_flow=7;CHECK_THROWS_AS(plan_loaded_thermal_calibration(c,input),std::invalid_argument);
    input=loaded_thermal_defaults(c);input.low_temperature=180;
    CHECK_THROWS_AS(plan_loaded_thermal_calibration(c,input),std::invalid_argument);
    input=loaded_thermal_defaults(c);c.set_deserialize_strict("printable_area","0x0,20x0,20x20,0x20");
    CHECK_THROWS_AS(plan_loaded_thermal_calibration(c,input),std::invalid_argument);
    c=config(0.4);c.set_deserialize_strict("gcode_flavor","marlin");
    CHECK_THROWS_AS(plan_loaded_thermal_calibration(c,input),std::invalid_argument);
}
