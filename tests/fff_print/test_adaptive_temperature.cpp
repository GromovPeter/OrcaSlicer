#include "libslic3r/Config.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/GCode/AdaptiveTemperatureReport.hpp"
#include <cmath>
#include <cstddef>
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Utils.hpp"
#include "test_helpers.hpp"
#include "test_utils.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace Slic3r;
namespace {
DynamicPrintConfig adaptive_config(bool enabled)
{
    auto config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"gcode_flavor", "klipper"}, {"printer_model", "Creality K2 Pro"},
        {"layer_height", "0.2"}, {"initial_layer_print_height", "0.2"},
        {"nozzle_temperature", "230"}, {"nozzle_temperature_initial_layer", "230"},
        {"nozzle_temperature_range_low", "220"}, {"nozzle_temperature_range_high", "260"},
        {"outer_wall_speed", "30"}, {"inner_wall_speed", "120"}, {"sparse_infill_speed", "140"},
        {"adaptive_temperature_smoothing_period", "5"}});
    config.set_key_value("filament_adaptive_temperature", new ConfigOptionBools{enabled});
    config.set_key_value("filament_flow_temperature_table", new ConfigOptionStrings{"1,232\n12,236\n24,243"});
    config.set_key_value("machine_start_gcode", new ConfigOptionString("M104 S230\nM109 S230\n"));
    config.set_key_value("machine_end_gcode", new ConfigOptionString("M104 S0\n"));
    config.set_key_value("layer_change_gcode", new ConfigOptionString("G92 E0\n"));
    return config;
}
std::vector<std::string> executable_commands(const std::string& gcode, bool include_temperature = true)
{
    std::vector<std::string> commands;
    std::istringstream input(gcode);
    std::string line;
    while (std::getline(input, line)) {
        line.erase(line.find(';') == std::string::npos ? line.size() : line.find(';'));
        const auto begin = line.find_first_not_of(" \t\r");
        if (begin == std::string::npos) continue;
        line = line.substr(begin, line.find_last_not_of(" \t\r") - begin + 1);
        if (include_temperature || line.rfind("M104 ", 0) != 0) commands.push_back(line);
    }
    return commands;
}
}

TEST_CASE("Native adaptive export changes only nonblocking temperature commands", "[AdaptiveTemperature][GCode]")
{
    const bool original_bbl = GCodeProcessor::s_IsBBLPrinter;
    const ScopeGuard restore([original_bbl] { GCodeProcessor::s_IsBBLPrinter = original_bbl; });
    const auto baseline = Test::slice({make_cube(20.0, 20.0, 4.0)}, adaptive_config(false));
    auto enabled = adaptive_config(true);
    const double step = GENERATE(0.5, 1.0);
    enabled.set_key_value("adaptive_temperature_step", new ConfigOptionFloat(step));
    Print print;
    Test::init_and_process_print({make_cube(20.0, 20.0, 4.0)}, print, enabled);
    GCodeProcessorResult result;
    const auto adapted = Test::gcode(print, &result);
    CHECK(executable_commands(adapted, false) == executable_commands(baseline, false));
    CHECK(adapted.find("_ORCA_AT_PRIVATE") == std::string::npos);
    if (step == 0.5) CHECK(adapted.find("M104 S230.5 ; _ORCA_ADAPTIVE_TEMPERATURE") != std::string::npos);
    CHECK(adapted.find("M104 S231") != std::string::npos);
    bool changed_target = false;
    for (const auto& move : result.moves) {
        if (move.type != EMoveType::Extrude) continue;
        if (move.layer_id == 0) CHECK(move.temperature == 230.0f);
        if (move.layer_id > 0 && move.temperature > 230.0f) changed_target = true;
    }
    CHECK(changed_target);
    ScopedTemporaryFile reopened(".gcode");
    std::ofstream(reopened.string()) << adapted;
    GCodeProcessor reader;
    reader.process_file(reopened.string());
    const auto& loaded = reader.get_result().moves;
    // Compare real extrusion endpoints and the extra actual-speed vertices too.
    for (const auto& move : result.moves) {
        if (move.type != EMoveType::Extrude) continue;
        const auto matching = std::find_if(loaded.begin(), loaded.end(), [&](const auto& other) {
            return other.type == move.type && other.gcode_id == move.gcode_id &&
                (other.position - move.position).norm() < 1e-5f;
        });
        REQUIRE(matching != loaded.end());
        CHECK_THAT(matching->temperature, Catch::Matchers::WithinAbs(move.temperature, 0.0001));
    }

}

TEST_CASE("Unknown runtime macros stop adaptive export instead of guessing their effect", "[AdaptiveTemperature][GCode]")
{
    auto config = adaptive_config(true);
    config.set_key_value("layer_change_gcode", new ConfigOptionString("UNKNOWN_TEMPERATURE_MACRO\n"));
    CHECK_THROWS(Test::slice({make_cube(10.0, 10.0, 2.0)}, config));
}

TEST_CASE("Known first layer heating remains protected around a start macro", "[AdaptiveTemperature][GCode]")
{
    auto config = adaptive_config(true);
    config.set_key_value("machine_start_gcode", new ConfigOptionString("START_PRINT EXTRUDER_TEMP=230\nM104 S230\nM109 S230\n"));
    const auto result = Test::slice({make_cube(10.0, 10.0, 2.0)}, config);
    CHECK(result.find("START_PRINT EXTRUDER_TEMP=230") != std::string::npos);
    CHECK(result.find("M109 S230") != std::string::npos);
    CHECK(result.find("_ORCA_AT_PRIVATE") == std::string::npos);
}

TEST_CASE("Macro-only startup establishes the adaptive setpoint after the first layer", "[AdaptiveTemperature][GCode]")
{
    auto config = adaptive_config(true);
    config.set_key_value("machine_start_gcode", new ConfigOptionString("M104 S0\nSTART_PRINT EXTRUDER_TEMP=230\n"));
    const auto result = Test::slice({make_cube(10.0, 10.0, 2.0)}, config);
    CHECK(result.find("START_PRINT EXTRUDER_TEMP=230") != std::string::npos);
    CHECK(result.find("M104 S230") != std::string::npos);
    CHECK(result.find("M104 S231") != std::string::npos);
    CHECK(result.find("_ORCA_AT_PRIVATE") == std::string::npos);
}
TEST_CASE("Native coordinated export preserves motion geometry and rebuilds its preview clock", "[AdaptiveTemperature][AdaptiveFlow][GCode]")
{
    const bool original_bbl = GCodeProcessor::s_IsBBLPrinter;
    const ScopeGuard restore([original_bbl] { GCodeProcessor::s_IsBBLPrinter = original_bbl; });
    auto config = adaptive_config(true);
    config.set_key_value("adaptive_temperature_step", new ConfigOptionFloat(GENERATE(0.5, 1.0)));
    config.set_key_value("filament_coordinated_temperature", new ConfigOptionBools{false});
    const auto baseline = Test::slice({make_cube(20, 20, 4)}, config);
    config.set_key_value("filament_coordinated_temperature", new ConfigOptionBools{true});
    Print print;
    Test::init_and_process_print({make_cube(20, 20, 4)}, print, config);
    GCodeProcessorResult result;
    const auto adapted = Test::gcode(print, &result);
    auto motions = [](const std::string& gcode) {
        std::vector<std::string> geometry;
        GCodeReader reader;
        reader.parse_buffer(gcode, [&](GCodeReader&, const GCodeReader::GCodeLine& line) {
            if (!line.cmd_is("G0") && !line.cmd_is("G1") && !line.cmd_is("G2") && !line.cmd_is("G3")) return;
            std::istringstream words(line.raw().substr(0, line.raw().find(';')));
            std::string word, value;
            while (words >> word) if (word.front() != 'F') value += word + " ";
            geometry.push_back(value);
        });
        return geometry;
    };
    CHECK(motions(adapted) == motions(baseline));
    CHECK(adapted.find("_ORCA_AT_PRIVATE") == std::string::npos);
    ScopedTemporaryFile file(".gcode");
    std::ofstream(file.string()) << adapted;
    GCodeProcessor reader;
    reader.process_file(file.string());
    CHECK_THAT(reader.get_result().print_statistics.modes[0].time,
               Catch::Matchers::WithinAbs(result.print_statistics.modes[0].time, 0.1));
    REQUIRE_FALSE(result.moves.empty());
    CHECK(result.print_statistics.modes[0].time > 0);
    const auto report = read_adaptive_temperature_report(file.string());
    REQUIRE(report.has_value());
    CHECK(report->coordinated);
    CHECK(report->slowed_moves > 0);
    CHECK_THAT(report->planned_time, Catch::Matchers::WithinAbs(result.print_statistics.modes[0].time, 0.1));
    CHECK(result.lines_ends.back() == adapted.size());
}

TEST_CASE("Adaptive handoff ignores the ordinary other layers temperature", "[AdaptiveTemperature][GCode]")
{
    const bool coordinated = GENERATE(false, true);
    auto config = adaptive_config(true);
    config.set_deserialize_strict({{"nozzle_temperature_initial_layer", "210"}, {"nozzle_temperature", "210"},
        {"nozzle_temperature_range_low", "190"}, {"nozzle_temperature_range_high", "260"}});
    config.set_key_value("filament_flow_temperature_table", new ConfigOptionStrings{"1,196\n10,213\n22,228"});
    config.set_key_value("filament_coordinated_temperature", new ConfigOptionBools{coordinated});
    config.set_key_value("machine_start_gcode", new ConfigOptionString("M104 S210\nM109 S210\n"));
    Print print;
    Test::init_and_process_print({make_cube(20, 20, 4)}, print, config);
    GCodeProcessorResult preview;
    const auto reference = Test::gcode(print, &preview);
    REQUIRE(reference.find("_ORCA_ADAPTIVE_TEMPERATURE") != std::string::npos);
    size_t first_layer_moves = 0;
    for (const auto& move : preview.moves) {
        if (move.type == EMoveType::Extrude && move.layer_id == 0) {
            ++first_layer_moves;
            CHECK_THAT(move.temperature, Catch::Matchers::WithinAbs(210.0, 0.0001));
        }
    }
    REQUIRE(first_layer_moves > 0);
    for (int other : {0, 180, 230, 260}) {
        CAPTURE(coordinated, other);
        config.set_key_value("nozzle_temperature", new ConfigOptionInts{other});
        const auto actual = Test::slice({make_cube(20, 20, 4)}, config);
        CHECK(executable_commands(actual) == executable_commands(reference));
    }
}

TEST_CASE("Ordinary second layer temperatures remain active without adaptation", "[AdaptiveTemperature][GCode]")
{
    auto config = adaptive_config(false);
    config.set_deserialize_strict({{"nozzle_temperature_initial_layer", "210"}, {"nozzle_temperature", "230"}});
    config.set_key_value("machine_start_gcode", new ConfigOptionString("M104 S210\nM109 S210\n"));
    const auto baseline = Test::slice({make_cube(10, 10, 2)}, config);
    CHECK(baseline.find("M104 S230") != std::string::npos);
    CHECK(baseline.find("M109 S210") != std::string::npos);
    CHECK(baseline.find("_ORCA_ADAPTIVE_TEMPERATURE") == std::string::npos);
    config.set_key_value("filament_coordinated_temperature", new ConfigOptionBools{true});
    CHECK(executable_commands(Test::slice({make_cube(10, 10, 2)}, config)) == executable_commands(baseline));
}

TEST_CASE("Macro startup anchors adaptive handoff to the first layer profile", "[AdaptiveTemperature][GCode]")
{
    const bool coordinated = GENERATE(false, true);
    auto config = adaptive_config(true);
    config.set_deserialize_strict({{"nozzle_temperature_initial_layer", "210"}, {"nozzle_temperature", "260"},
        {"nozzle_temperature_range_low", "190"}, {"nozzle_temperature_range_high", "260"}});
    config.set_key_value("filament_flow_temperature_table", new ConfigOptionStrings{"1,196\n10,213\n22,228"});
    config.set_key_value("filament_coordinated_temperature", new ConfigOptionBools{coordinated});
    config.set_key_value("machine_start_gcode", new ConfigOptionString("M104 S0\nSTART_PRINT EXTRUDER_TEMP=210\n"));
    const auto result = Test::slice({make_cube(10, 10, 3)}, config);
    CHECK(result.find("START_PRINT EXTRUDER_TEMP=210") != std::string::npos);
    CHECK(result.find("M104 S210") != std::string::npos);
    CHECK(result.find("M104 S260") == std::string::npos);
    CHECK(result.find("_ORCA_ADAPTIVE_TEMPERATURE") != std::string::npos);
}

TEST_CASE("Spiral vase adaptive temperature preserves executable motion", "[AdaptiveTemperature][GCode]")
{
    auto config = adaptive_config(false);
    config.set_key_value("adaptive_temperature_step", new ConfigOptionFloat(GENERATE(0.5, 1.0)));
    config.set_deserialize_strict({{"spiral_mode", "1"}, {"wall_loops", "1"},
        {"top_shell_layers", "0"}, {"bottom_shell_layers", "1"}, {"sparse_infill_density", "0%"}});
    const bool vfa = GENERATE(false, true);
    const auto slice = [vfa](const DynamicPrintConfig& settings) {
        Print print;
        Test::init_and_process_print({make_cube(20, 20, 4)}, print, settings);
        if (vfa) {
            Calib_Params params;
            params.mode = CalibMode::Calib_VFA_Tower;
            params.start = 40;
            params.end = 100;
            params.step = 10;
            print.set_calib_params(params);
        }
        return Test::gcode(print);
    };
    const auto baseline = slice(config);
    config.set_key_value("filament_adaptive_temperature", new ConfigOptionBools{true});
    const auto adapted = slice(config);
    CHECK(executable_commands(adapted, false) == executable_commands(baseline, false));
    CHECK(adapted.find("M104 S231") != std::string::npos);
    CHECK(adapted.find("_ORCA_AT_PRIVATE") == std::string::npos);
}

TEST_CASE("VFA rejects coordinated flow even when temperature adaptation is disabled", "[AdaptiveTemperature][GCode]")
{
    auto config = adaptive_config(GENERATE(false, true));
    config.set_key_value("filament_coordinated_temperature", new ConfigOptionBools{true});
    Print print;
    Test::init_and_process_print({make_cube(20, 20, 4)}, print, config);
    Calib_Params params;
    params.mode = CalibMode::Calib_VFA_Tower;
    params.start = 40;
    params.end = 100;
    params.step = 10;
    print.set_calib_params(params);
    CHECK(print.validate().opt_key == "filament_coordinated_temperature");
    CHECK_THROWS_WITH(Test::gcode(print), Catch::Matchers::ContainsSubstring("Disable Coordinate flow"));
}

TEST_CASE("Adaptive ironing exports the material offset without changing motion", "[AdaptiveTemperature][GCode]")
{
    const bool original_bbl = GCodeProcessor::s_IsBBLPrinter;
    const ScopeGuard restore([original_bbl] { GCodeProcessor::s_IsBBLPrinter = original_bbl; });
    auto config = adaptive_config(false);
    config.set_deserialize_strict({{"ironing_type", "top"}, {"ironing_speed", "20"},
        {"adaptive_temperature_rise_time", "0.1"}, {"adaptive_temperature_fall_time", "0.1"},
        {"adaptive_temperature_min_interval", "0.1"}, {"adaptive_temperature_smoothing_period", "0"}});
    const double offset = GENERATE(5.0, 50.0, -50.0);
    config.set_key_value("filament_ironing_temperature_offset", new ConfigOptionFloats{offset});
    const auto baseline = Test::slice({make_cube(20.0, 20.0, 2.0)}, config);
    config.set_key_value("filament_adaptive_temperature", new ConfigOptionBools{true});
    Print print;
    Test::init_and_process_print({make_cube(20.0, 20.0, 2.0)}, print, config);
    GCodeProcessorResult result;
    const auto adapted = Test::gcode(print, &result);
    CHECK(executable_commands(adapted, false) == executable_commands(baseline, false));
    const double target = std::clamp(230.0 + offset, 220.0, 260.0);
    bool ironing = false, reached = false;
    for (const auto& move : result.moves) {
        if (move.type != EMoveType::Extrude || move.extrusion_role != erIroning) continue;
        ironing = true;
        if (std::abs(move.temperature - target) <= 1.0) reached = true;
    }
    CHECK(ironing);
    CHECK(reached);
}
