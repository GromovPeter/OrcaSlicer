#include "libslic3r/InterlayerAdhesionCalibration.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/CustomGCode.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "test_helpers.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <cstddef>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <string>
#include <vector>
using namespace Slic3r;
namespace {
DynamicPrintConfig config()
{
    auto c=DynamicPrintConfig::full_print_config();
    c.set_deserialize_strict({{"gcode_flavor","klipper"},{"printer_model","Creality K2 Pro"},
        {"printable_area","-150x-150,150x-150,150x150,-150x150"},{"printable_height","300"},
        {"nozzle_temperature_range_low","190"},{"nozzle_temperature_range_high","260"},
        {"filament_flow_ratio","1"},{"filament_max_volumetric_speed","20"},
        {"slow_down_for_layer_cooling","1"},{"slow_down_layer_time","3"},
        {"full_fan_speed_layer","3"},{"close_fan_the_first_x_layers","1"},
        {"default_acceleration","5000"},{"outer_wall_acceleration","3000"},
        {"adaptive_temperature_fall_time","2.3"},{"adaptive_temperature_lead_time","4.7"},
        {"machine_start_gcode","M104 S230\nM109 S230\n"},{"machine_end_gcode","M104 S0\n"},
        {"layer_change_gcode","G92 E0\n"},{"before_layer_change_gcode",""}});
    return c;
}
}
TEST_CASE("Native spiral adhesion export retains continuous measurement flow and temperature steps", "[InterlayerAdhesion][GCode]")
{
    auto c=config();AdhesionCalibrationInput a;a.flow=2;a.end_temperature=228;
    const auto plan=plan_adhesion_calibration(c,a);c.apply(adhesion_process_patch(plan));c.apply(adhesion_filament_patch(plan));
    Print print;Model model;Test::init_print({make_cylinder(plan.diameter/2,plan.height)},print,model,c);
    model.plates_custom_gcodes[model.curr_plate_index]=adhesion_custom_gcodes(plan);print.apply(model,c);print.process();
    REQUIRE(print.objects().front()->layers().size()==plan.total_layers);
    CHECK_THAT(print.objects().front()->layers().back()->print_z,Catch::Matchers::WithinAbs(plan.height,0.001));
    GCodeProcessorResult result;const auto text=Test::gcode(print,&result);
    CHECK(text.find("M104 S229 ; _ORCA_ADHESION_STEP")!=std::string::npos);
    CHECK(text.find("M104 S228 ; _ORCA_ADHESION_STEP")!=std::string::npos);
    CHECK(text.find("_ORCA_ADAPTIVE_TEMPERATURE")==std::string::npos);
    CHECK(text.find("_ORCA_ADHESION_CALIBRATION_V1")!=std::string::npos);
    CHECK(result.print_statistics.modes[0].time>0);
    unsigned layers=0;size_t at=0;
    while((at=text.find(";LAYER_CHANGE",at))!=std::string::npos) {++layers;++at;}
    CHECK(layers==plan.total_layers);
    float last_z=0;std::vector<float> changes;GCodeReader reader;
    reader.parse_buffer(text,[&](GCodeReader&,const GCodeReader::GCodeLine& line){
        if(line.cmd_is("G1") && line.has_z()) last_z=line.z();
        if(line.cmd_is("M104") && line.raw().find("_ORCA_ADHESION_STEP")!=std::string::npos) changes.push_back(last_z);
    });
    REQUIRE(changes.size()==plan.bands.size());
    for(size_t i=0;i<changes.size();++i)
        CHECK_THAT(changes[i],Catch::Matchers::WithinAbs(plan.bands[i].begin_z,0.025));
}
TEST_CASE("Modified adhesion geometry cannot export with a stale temperature map", "[InterlayerAdhesion][GCode]")
{
    auto c=config();AdhesionCalibrationInput a;a.flow=2;a.end_temperature=228;const auto plan=plan_adhesion_calibration(c,a);
    c.apply(adhesion_process_patch(plan));c.apply(adhesion_filament_patch(plan));
    Print print;Model model;Test::init_print({make_cylinder(plan.diameter/2,plan.height+1)},print,model,c);
    model.plates_custom_gcodes[model.curr_plate_index]=adhesion_custom_gcodes(plan);print.apply(model,c);print.process();
    CHECK_THROWS(Test::gcode(print));
}

TEST_CASE("Ordinary vase export retains its finishing taper", "[InterlayerAdhesion][GCode]")
{
    auto c=config();AdhesionCalibrationInput a;a.flow=2;a.end_temperature=228;
    const auto plan=plan_adhesion_calibration(c,a);c.apply(adhesion_process_patch(plan));c.apply(adhesion_filament_patch(plan));
    Print print;Model model;Test::init_print({make_cylinder(plan.diameter/2,plan.height)},print,model,c);print.process();
    const auto text=Test::gcode(print);
    unsigned layers=0;size_t at=0;
    while((at=text.find(";LAYER_CHANGE",at))!=std::string::npos) {++layers;++at;}
    CHECK(layers==plan.total_layers+1);
    CHECK(text.find("_ORCA_ADHESION_CALIBRATION_V1")==std::string::npos);
}

TEST_CASE("Native adhesion export maintains high flow with the explicit layer time override", "[InterlayerAdhesion][GCode]")
{
    auto c=config();c.set_deserialize_strict({{"slow_down_layer_time","7"},{"filament_flow_ratio","0.965"}});
    AdhesionCalibrationInput a;a.flow=18;a.start_temperature=260;a.end_temperature=258;a.disable_layer_time_slowdown=true;
    const auto p=plan_adhesion_calibration(c,a);c.apply(adhesion_process_patch(p));c.apply(adhesion_filament_patch(p));
    Print print;Model model;Test::init_print({make_cylinder(p.diameter/2,p.height)},print,model,c);
    model.plates_custom_gcodes[model.curr_plate_index]=adhesion_custom_gcodes(p);print.apply(model,c);print.process();
    // Export itself rejects slowed measurement flow, stale conditions or an extra taper-out revolution.
    GCodeProcessorResult result;const auto text=Test::gcode(print,&result);
    CHECK(p.diameter<200);CHECK(text.find("M104 S259 ; _ORCA_ADHESION_STEP")!=std::string::npos);
    CHECK(text.find("M104 S258 ; _ORCA_ADHESION_STEP")!=std::string::npos);
    CHECK(result.print_statistics.modes[0].time>0);
    const auto restored=adhesion_plan_from_codes(model.get_curr_plate_custom_gcodes());REQUIRE(restored.has_value());
    CHECK(restored->input.disable_layer_time_slowdown);
    c.set_key_value("slow_down_for_layer_cooling",new ConfigOptionBools{true});
    CHECK_THROWS(validate_adhesion_project(model,c,*restored));
}

TEST_CASE("A calibration reslices after its commands round trip through preview layers", "[InterlayerAdhesion][GCode]")
{
    auto c=config();AdhesionCalibrationInput a;a.flow=2;a.end_temperature=228;
    const auto p=plan_adhesion_calibration(c,a);c.apply(adhesion_process_patch(p));c.apply(adhesion_filament_patch(p));
    Print print;Model model;Test::init_print({make_cylinder(p.diameter/2,p.height)},print,model,c);
    auto codes=adhesion_custom_gcodes(p);codes.mode=CustomGCode::Undef;
    for(auto& command:codes.gcodes) command.print_z=double(float(command.print_z));
    model.plates_custom_gcodes[model.curr_plate_index]=codes;print.apply(model,c);print.process();
    const auto text=Test::gcode(print);
    CHECK(text.find("M104 S229 ; _ORCA_ADHESION_STEP")!=std::string::npos);
    CHECK(text.find("M104 S228 ; _ORCA_ADHESION_STEP")!=std::string::npos);
}
