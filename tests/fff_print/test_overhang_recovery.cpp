#include "libslic3r/GCode/AppearanceUnderExtrusionAccelRecoveryFilter.hpp"
#include "libslic3r/GCode.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/InterlayerAdhesionCalibration.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "test_helpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace Slic3r;
namespace {
std::string path(const std::string& next="G1 X38 E1.5 F9000\nG1 X48 E0.5\n")
{
    return "SET_VELOCITY_LIMIT ACCEL=1500\nG1 X0 Y0 Z0.4 F1200\n"
        ";TYPE:Overhang wall\nG1 X8 E0.4 F1200\n;TYPE:Outer wall\n"
        "SET_VELOCITY_LIMIT ACCEL=3000\n" + next;
}
std::string filtered(const std::string& text, bool enabled=true, int layer=1, bool vase=false,
                     bool relative=true, double speed=50, double accel=200, bool accel_to_decel=false)
{
    GCodeConfig gc;gc.use_relative_e_distances.value=relative;
    gc.accel_to_decel_enable.value=accel_to_decel;gc.accel_to_decel_factor.value=50;
    PrintObjectConfig obj;obj.msao_recovery_enable.value=enabled;
    obj.msao_safe_velocity.value=speed;obj.msao_safe_accel.value=accel;
    AppearanceUnderExtrusionAccelRecoveryFilter filter(gc,obj,{},gcfKlipper);
    return filter.process_layer(LayerResult{text,static_cast<size_t>(layer),vase,true}).gcode;
}
struct Sample {double x=0,y=0,z=0,e=0,f=0,a=0;};
std::vector<Sample> samples(const std::string& text, bool relative=true)
{
    std::vector<Sample> out;Sample state;GCodeReader reader;
    reader.parse_buffer(text,[&](GCodeReader&,const GCodeReader::GCodeLine& line) {
        if (line.raw().rfind("SET_VELOCITY_LIMIT ACCEL=",0)==0)
            state.a=std::stod(line.raw().substr(std::string("SET_VELOCITY_LIMIT ACCEL=").size()));
        if (!line.cmd_is("G1") && !line.cmd_is("G2") && !line.cmd_is("G3")) return;
        if(line.has_x())state.x=line.x();if(line.has_y())state.y=line.y();if(line.has_z())state.z=line.z();
        if(line.has_f())state.f=line.f()/60.0;
        if(line.has_e()) {state.e=relative ? state.e+line.e() : line.e();out.push_back(state);}
    });
    return out;
}
}

TEST_CASE("Recovery follows the upstream continuous slow extrusion threshold", "[OverhangRecovery]")
{
    const double length=GENERATE(3.9,4.1);
    const std::string text="SET_VELOCITY_LIMIT ACCEL=3000\nG1 X0 Y0 Z0.4 F1200\n"
        ";TYPE:Outer wall\nG1 X"+std::to_string(length)+" E0.2 F1200\nG1 X40 E1.8 F9000\n";
    const auto output=filtered(text);
    CHECK((output.find("AUE accel_safe")!=std::string::npos)==(length/20.0>=0.2));
    CHECK(filtered(text,false)==text);
}

TEST_CASE("Recovery uses the upstream configured safe speed and recovery distance", "[OverhangRecovery]")
{
    const auto output=filtered(path());
    REQUIRE(output.find("AUE velocity_safe")!=std::string::npos);
    double x=0,recovery_x=-1;GCodeReader reader;
    reader.parse_buffer(output,[&](GCodeReader&,const GCodeReader::GCodeLine& line){
        if(line.cmd_is("G1") && line.has_x())x=line.x();
        if(line.raw().find("AUE accel_recovered")!=std::string::npos)recovery_x=x;
    });
    const double distance=1+(50.0*50-20.0*20)/(2*200)+1;
    CHECK_THAT(recovery_x,Catch::Matchers::WithinAbs(8+distance,0.002));
    CHECK(output.find("SET_VELOCITY_LIMIT ACCEL=200 ; AUE accel_safe")!=std::string::npos);
    CHECK(output.find("G1 F3000 ; AUE velocity_safe")!=std::string::npos);
}

TEST_CASE("Recovery retains extrusion totals in relative and absolute E modes", "[OverhangRecovery]")
{
    const bool relative=GENERATE(true,false);
    const auto text=path(relative ? "G1 X38 E1.51237 F9000\nG1 X48 E0.5\n" :
                                   "G1 X38 E1.91237 F9000\nG1 X48 E2.41237\n");
    const auto output=filtered(text,true,1,false,relative);
    REQUIRE(output.find("AUE velocity_safe")!=std::string::npos);
    const auto before=samples(text,relative),after=samples(output,relative);
    CHECK_THAT(after.back().e,Catch::Matchers::WithinAbs(before.back().e,0.000001));
    CHECK_THAT(after.back().x,Catch::Matchers::WithinAbs(before.back().x,0.001));
}

TEST_CASE("Recovery preserves native arcs and returns to the original endpoint", "[OverhangRecovery]")
{
    const auto text=path("G3 X28 Y20 I0 J20 E1.5 F9000\nG1 X48 E0.5\n");
    const auto output=filtered(text);
    REQUIRE(output.find("AUE velocity_safe")!=std::string::npos);
    CHECK(output.find("G3")!=std::string::npos);
    const auto before=samples(text),after=samples(output);
    // Upstream rounds the independently split E values to five decimal places.
    CHECK_THAT(after.back().e,Catch::Matchers::WithinAbs(before.back().e,0.00002));
    CHECK_THAT(after.back().x,Catch::Matchers::WithinAbs(before.back().x,0.001));
    CHECK_THAT(after.back().y,Catch::Matchers::WithinAbs(before.back().y,0.001));
}

TEST_CASE("Recovery options reject conflicting filters and unsupported firmware", "[OverhangRecovery]")
{
    auto c=DynamicPrintConfig::full_print_config();
    c.set_deserialize_strict({{"msao_recovery_enable","1"},{"gcode_flavor","klipper"},
        {"max_volumetric_extrusion_rate_slope","1"}});
    Print print;Model model;Test::init_print({Test::cube(10)},print,model,c);
    CHECK(print.validate().opt_key=="msao_recovery_enable");
    c.set_deserialize_strict({{"max_volumetric_extrusion_rate_slope","0"},{"gcode_flavor","marlin"}});
    print.apply(model,c);CHECK(print.validate().opt_key=="msao_recovery_enable");
}

TEST_CASE("Adhesion calibration disables local recovery in its temporary process", "[OverhangRecovery][InterlayerAdhesion]")
{
    auto c=DynamicPrintConfig::full_print_config();
    c.set_deserialize_strict({{"gcode_flavor","klipper"},{"msao_recovery_enable","1"}});
    AdhesionCalibrationInput input;input.flow=2;
    const auto plan=plan_adhesion_calibration(c,input);
    CHECK_FALSE(adhesion_process_patch(plan).opt_bool("msao_recovery_enable"));
    CHECK(c.opt_bool("msao_recovery_enable"));
}

TEST_CASE("Native overhang export includes local recovery controls", "[OverhangRecovery][GCode]")
{
    auto c=DynamicPrintConfig::full_print_config();
    c.set_deserialize_strict({{"gcode_flavor","klipper"},{"msao_recovery_enable","1"},
        {"msao_safe_accel","200"},{"msao_safe_velocity","50"},
        {"use_relative_e_distances","1"},{"layer_height","0.2"},
        {"max_volumetric_extrusion_rate_slope","0"},{"filament_max_volumetric_speed","30"},
        {"slow_down_for_layer_cooling","0"},{"outer_wall_speed","150"},
        {"inner_wall_speed","150"},{"default_acceleration","3000"},
        {"outer_wall_acceleration","3000"},{"enable_overhang_speed","1"},
        {"overhang_1_4_speed","20"},{"overhang_2_4_speed","20"},
        {"overhang_3_4_speed","20"},{"overhang_4_4_speed","20"},
        {"gcode_label_objects","0"},{"enable_arc_fitting","0"}});
    Print print;Model model;Test::init_print({Test::mesh(Test::TestMesh::sloping_hole)},print,model,c);
    print.process();const auto text=Test::gcode(print);
    CHECK(text.find("AUE accel_safe")!=std::string::npos);
    CHECK(text.find("AUE accel_recovered")!=std::string::npos);
    CHECK(text.find("; OBJECT_ID:")!=std::string::npos);
}

TEST_CASE("Effective object overrides gate local recovery", "[OverhangRecovery]")
{
    GCodeConfig gc;gc.use_relative_e_distances.value=true;PrintObjectConfig obj;
    obj.msao_recovery_enable.value=false;
    const std::unordered_map<int,AppearanceUnderExtrusionAccelRecoveryFilter::ObjectParams> objects{
        {1,{false,200,50}},{2,{true,200,50}}};
    AppearanceUnderExtrusionAccelRecoveryFilter filter(gc,obj,objects,gcfKlipper);
    const auto off="; OBJECT_ID: 1\n"+path();
    CHECK(filter.process_layer(LayerResult{off,1,false,true}).gcode==off);
    const auto on="; OBJECT_ID: 2\n"+path();
    CHECK(filter.process_layer(LayerResult{on,2,false,true}).gcode.find("AUE velocity_safe")!=std::string::npos);
}
