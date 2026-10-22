#include "libslic3r/InterlayerAdhesionCalibration.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/CustomGCode.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/GCode/FlowTimeline.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/Semver.hpp"
#include "test_utils.hpp"
#include <vector>
#include "libslic3r/PrintConfig.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <cstddef>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <limits>
#include <string>
using namespace Slic3r;
namespace {
DynamicPrintConfig settings()
{
    auto c=DynamicPrintConfig::full_print_config();
    c.set_deserialize_strict({{"gcode_flavor","klipper"},{"nozzle_diameter","0.4"},
        {"printable_area","0x0,300x0,300x300,0x300"},{"printable_height","300"},
        {"filament_flow_ratio","1"},{"filament_max_volumetric_speed","20"},
        {"nozzle_temperature_range_low","190"},{"nozzle_temperature_range_high","260"},
        {"slow_down_for_layer_cooling","1"},{"slow_down_layer_time","7"},
        {"full_fan_speed_layer","6"},{"close_fan_the_first_x_layers","1"},
        {"default_acceleration","5000"},{"outer_wall_acceleration","3000"},
        {"adaptive_temperature_fall_time","2.3"},{"adaptive_temperature_lead_time","4.7"}});
    return c;
}
}
TEST_CASE("Adhesion sizing retains cooling time and bounds cylinder height", "[InterlayerAdhesion]")
{
    auto c=settings();AdhesionCalibrationInput a;const auto p=plan_adhesion_calibration(c,a);
    CHECK(p.turn_time>=7.7);CHECK(p.stable_layers>=3);CHECK(p.stable_layers*p.turn_time>=5);
    CHECK(p.transition_layers*p.turn_time>=1.25*2.3+4.7);CHECK(p.warmup_layers>6);
    CHECK(p.height<50);CHECK(p.diameter<290);CHECK(p.bands.size()==31);
    CHECK_THAT(p.stable_height,Catch::Matchers::WithinAbs(0.6,1e-6));
    CHECK_THAT(p.bands.back().end_z,Catch::Matchers::WithinAbs(p.height,1e-6));
}
TEST_CASE("Adhesion sizing rejects flow that cannot fit cooling constraints", "[InterlayerAdhesion]")
{
    auto c=settings();AdhesionCalibrationInput a;a.flow=20;
    CHECK_THROWS(plan_adhesion_calibration(c,a));
    a.flow=21;CHECK_THROWS(plan_adhesion_calibration(c,a));
    a.flow=5;c.set_key_value("printable_height",new ConfigOptionFloat(5));CHECK_THROWS(plan_adhesion_calibration(c,a));
}
TEST_CASE("Adhesion sections retain complete turns and minimum stable dwell", "[InterlayerAdhesion]")
{
    auto c=settings();AdhesionCalibrationInput a;auto p=plan_adhesion_calibration(c,a);
    a.stable_height=0.1;CHECK_THROWS(plan_adhesion_calibration(c,a));
    a.stable_height=1.01;p=plan_adhesion_calibration(c,a);
    CHECK_THAT(p.stable_height,Catch::Matchers::WithinAbs(1.2,1e-6));
    a.stable_height=0;a.minimum_diameter=180;p=plan_adhesion_calibration(c,a);CHECK(p.diameter>=180);
}
TEST_CASE("Adhesion height lookup separates base cooling and stable material", "[InterlayerAdhesion]")
{
    auto p=plan_adhesion_calibration(settings(),{});using Kind=AdhesionHeightResult::Kind;
    CHECK(adhesion_temperature_at_height(p,0).kind==Kind::Warmup);
    CHECK(adhesion_temperature_at_height(p,p.measurement_z+0.1).kind==Kind::Stable);
    const auto& b=p.bands[1];const auto cooling=adhesion_temperature_at_height(p,b.begin_z+0.01);
    CHECK(cooling.kind==Kind::Transition);CHECK(cooling.temperature==229);CHECK(cooling.previous_temperature==230);
    CHECK(adhesion_temperature_at_height(p,b.stable_z+0.01).kind==Kind::Stable);
    CHECK(adhesion_temperature_at_height(p,p.height).temperature==200);
    CHECK(adhesion_temperature_at_height(p,p.height+1).kind==Kind::Outside);
    CHECK(adhesion_temperature_at_height(p,std::numeric_limits<double>::quiet_NaN()).kind==Kind::Outside);
}
TEST_CASE("Adhesion map round trips through native custom height commands", "[InterlayerAdhesion]")
{
    const auto p=plan_adhesion_calibration(settings(),{});const auto codes=adhesion_custom_gcodes(p);
    const auto loaded=adhesion_plan_from_codes(codes);REQUIRE(loaded.has_value());
    CHECK_THAT(loaded->height,Catch::Matchers::WithinAbs(p.height,1e-6));
    CHECK(adhesion_custom_gcodes(*loaded)==codes);CHECK(codes.gcodes.size()==p.bands.size());
    CHECK(codes.gcodes[1].extra.find("M104 S229")!=std::string::npos);
    CHECK(codes.gcodes[1].extra.find("M109")==std::string::npos);
    CHECK_FALSE(adhesion_plan_from_codes({}).has_value());
}
TEST_CASE("Adhesion calibration patches preserve original fan and cooling settings", "[InterlayerAdhesion]")
{
    auto c=settings();c.set_deserialize_strict({{"enable_support","1"},{"raft_layers","3"},{"fuzzy_skin","all"},{"ironing_type","top"}});
    const auto p=plan_adhesion_calibration(c,{});const auto fan=c.option("fan_max_speed")->serialize();
    const auto cooling=c.option("slow_down_layer_time")->serialize();c.apply(adhesion_process_patch(p));c.apply(adhesion_filament_patch(p));
    CHECK(c.option("fan_max_speed")->serialize()==fan);CHECK(c.option("slow_down_layer_time")->serialize()==cooling);
    CHECK_FALSE(c.opt_bool("enable_support"));CHECK(c.opt_int("raft_layers")==0);
    CHECK(c.option("fuzzy_skin")->serialize()=="disabled_fuzzy");CHECK(c.option("ironing_type")->serialize()=="no ironing");
    CHECK(c.opt_bool("spiral_mode"));CHECK_FALSE(c.opt_bool("filament_adaptive_temperature",0));
    CHECK_THAT(c.option<ConfigOptionFloatOrPercent>("outer_wall_line_width")->value,Catch::Matchers::WithinAbs(0.5,1e-6));
}
TEST_CASE("Adhesion measurements update exactly one increasing table point", "[InterlayerAdhesion]")
{
    CHECK(adhesion_table_with_point("1,197\n10,213",5,204)=="1,197\n5,204\n10,213");
    CHECK(adhesion_table_with_point("1,197\n5,204\n10,213",5,205)=="1,197\n5,205\n10,213");
    CHECK_THROWS(adhesion_table_with_point("1,197\n10,213",5,240));
}
TEST_CASE("Adhesion input rejects unsafe ranges nonfinite values and layer macros", "[InterlayerAdhesion]")
{
    auto c=settings();AdhesionCalibrationInput a;a.start_temperature=280;CHECK_THROWS(plan_adhesion_calibration(c,a));
    a={};a.temperature_step=4;CHECK_THROWS(plan_adhesion_calibration(c,a));
    a={};a.flow=std::numeric_limits<double>::infinity();CHECK_THROWS(plan_adhesion_calibration(c,a));
    a={};c.set_key_value("layer_change_gcode",new ConfigOptionString("M104 S200"));CHECK_THROWS(plan_adhesion_calibration(c,a));
    c.set_key_value("layer_change_gcode",new ConfigOptionString("G92 Z0"));CHECK_THROWS(plan_adhesion_calibration(c,a));
    c.set_key_value("layer_change_gcode",new ConfigOptionString("G92 E0 ; extrusion only"));CHECK_NOTHROW(plan_adhesion_calibration(c,a));
}

TEST_CASE("Adhesion flow validation rejects a slowed measurement wall", "[InterlayerAdhesion]")
{
    const auto p=plan_adhesion_calibration(settings(),{});
    auto make_timeline=[&](double duration) {
        FlowTimeline timeline;FlowTimelineMotion m;
        m.kind=FlowMotionKind::Extrusion;m.layer_id=p.total_layers;m.source_line=1;m.heater_id=0;
        m.distance=10;m.duration=duration;m.entry_speed=m.cruise_speed=m.exit_speed=10/duration;
        m.delta_e=p.input.flow;m.filament_area=1;
        timeline.append(m);timeline.finish(duration);return timeline;
    };
    CHECK_NOTHROW(validate_adhesion_flow(make_timeline(1),p));
    CHECK_THROWS(validate_adhesion_flow(make_timeline(2),p));
}
TEST_CASE("Saved Orca projects retain the adhesion temperature height map", "[InterlayerAdhesion][3mf]")
{
    auto c=settings();AdhesionCalibrationInput a;a.disable_layer_time_slowdown=GENERATE(false,true);
    const auto p=plan_adhesion_calibration(c,a);c.apply(adhesion_process_patch(p));c.apply(adhesion_filament_patch(p));
    Model model;auto* object=model.add_object("adhesion","",make_cylinder(p.diameter/2,p.height));object->add_instance();
    model.plates_custom_gcodes[0]=adhesion_custom_gcodes(p);
    ScopedTemporaryDir backup("adhesion_backup");model.set_backup_path(backup.string());
    ScopedTemporaryFile file(".3mf");const auto filename=file.string();StoreParams sp;
    sp.path=filename.c_str();sp.model=&model;sp.config=&c;sp.strategy=SaveStrategy::Zip64|SaveStrategy::Silence;
    REQUIRE(store_bbs_3mf(sp));
    Model loaded;ScopedTemporaryDir loaded_backup("adhesion_loaded");loaded.set_backup_path(loaded_backup.string());
    DynamicPrintConfig config;ConfigSubstitutionContext context{ForwardCompatibilitySubstitutionRule::Enable};
    PlateDataPtrs plates;std::vector<Preset*> presets;bool bbl=false,orca=false;Semver version;
    REQUIRE(load_bbs_3mf(filename.c_str(),&config,&context,&loaded,&plates,&presets,&bbl,&orca,&version,nullptr,LoadStrategy::LoadModel|LoadStrategy::LoadConfig));
    REQUIRE(loaded.plates_custom_gcodes.count(0)==1);
    const auto restored=adhesion_plan_from_codes(loaded.plates_custom_gcodes.at(0));
    REQUIRE(restored.has_value());CHECK(restored->input.disable_layer_time_slowdown==a.disable_layer_time_slowdown);CHECK_THAT(restored->height,Catch::Matchers::WithinAbs(p.height,1e-6));
    CHECK(adhesion_custom_gcodes(*restored)==adhesion_custom_gcodes(p));
    for(auto* preset:presets) delete preset;
    for(auto* plate:plates) delete plate;
}

TEST_CASE("High flow adhesion calibration can explicitly bypass layer time without changing fans", "[InterlayerAdhesion]")
{
    auto c=settings();c.set_key_value("filament_flow_ratio",new ConfigOptionFloats{0.965});
    AdhesionCalibrationInput a;a.flow=18;a.start_temperature=260;a.end_temperature=240;
    CHECK_THROWS(plan_adhesion_calibration(c,a));
    a.disable_layer_time_slowdown=true;const auto p=plan_adhesion_calibration(c,a);
    CHECK(p.diameter<200);CHECK(p.height<50);CHECK(p.turn_time>=2);CHECK(p.turn_time<3);
    CHECK(p.stable_layers>=3);CHECK(p.stable_layers*p.turn_time>=5);
    CHECK(p.transition_layers*p.turn_time>=1.25*2.3+4.7);
    const auto patch=adhesion_filament_patch(p);auto effective=c;effective.apply(patch);
    CHECK(c.opt_bool("slow_down_for_layer_cooling",0));CHECK_FALSE(effective.opt_bool("slow_down_for_layer_cooling",0));
    for(const char* key:{"fan_min_speed","fan_max_speed","fan_cooling_layer_time","slow_down_layer_time","full_fan_speed_layer","close_fan_the_first_x_layers"})
        CHECK(effective.option(key)->serialize()==c.option(key)->serialize());
    const auto restored=adhesion_plan_from_codes(adhesion_custom_gcodes(p));REQUIRE(restored.has_value());
    CHECK(restored->input.disable_layer_time_slowdown);CHECK(adhesion_custom_gcodes(*restored)==adhesion_custom_gcodes(p));
    CHECK(plan_adhesion_calibration(effective,a).conditions==p.conditions);
    a.flow=21;CHECK_THROWS(plan_adhesion_calibration(c,a));
}
TEST_CASE("Existing adhesion maps retain the inherited layer cooling policy", "[InterlayerAdhesion]")
{
    const auto p=plan_adhesion_calibration(settings(),{});const auto codes=adhesion_custom_gcodes(p);
    CHECK(codes.gcodes.front().extra.find("disable_layer_time_slowdown")==std::string::npos);
    const auto restored=adhesion_plan_from_codes(codes);REQUIRE(restored.has_value());
    CHECK_FALSE(restored->input.disable_layer_time_slowdown);
    CHECK_FALSE(adhesion_filament_patch(p).has("slow_down_for_layer_cooling"));
    CHECK(adhesion_custom_gcodes(*restored)==codes);
}

TEST_CASE("Adhesion schedule validation accepts preview rounding but rejects command edits", "[InterlayerAdhesion]")
{
    auto c=settings();const auto p=plan_adhesion_calibration(c,{});c.apply(adhesion_process_patch(p));c.apply(adhesion_filament_patch(p));
    Model model;auto* object=model.add_object("adhesion","",make_cylinder(p.diameter/2,p.height));object->add_instance();
    const auto original=adhesion_custom_gcodes(p);auto& codes=model.plates_custom_gcodes[0];codes=original;
    CHECK(adhesion_has_height_map(codes));CHECK_FALSE(adhesion_has_height_map({}));
    CHECK_NOTHROW(validate_adhesion_project(model,c,p));
    codes.mode=CustomGCode::Undef;
    for(auto& command:codes.gcodes) command.print_z=double(float(command.print_z));
    CHECK_NOTHROW(validate_adhesion_project(model,c,p));
    codes.gcodes.back().print_z+=0.01;CHECK_THROWS(validate_adhesion_project(model,c,p));
    codes=original;codes.gcodes.back().extra="M104 S100";CHECK_THROWS(validate_adhesion_project(model,c,p));
    codes=original;codes.gcodes.pop_back();CHECK_THROWS(validate_adhesion_project(model,c,p));
    codes=original;codes.mode=CustomGCode::MultiExtruder;CHECK_THROWS(validate_adhesion_project(model,c,p));
}
