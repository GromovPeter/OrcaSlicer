#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCode/FlowTimeline.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "test_utils.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <string>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

TEST_CASE("Native flow collection preserves the processor motion estimate", "[FlowTimeline][GCodeProcessor]")
{
    const auto flavor = GENERATE(gcfKlipper, gcfMarlinFirmware);
    const bool relative_e = GENERATE(true, false);
    FullPrintConfig config;
    config.gcode_flavor.value = flavor;
    config.filament_diameter.values = {1.75};
    config.filament_map.values = {1};
    const bool old_bbl = GCodeProcessor::s_IsBBLPrinter;
    const ScopeGuard restore([old_bbl] { GCodeProcessor::s_IsBBLPrinter = old_bbl; });
    GCodeProcessor::s_IsBBLPrinter = false;
    ScopedTemporaryFile file(".gcode");
    std::ofstream(file.string()) << "G21\nG90\n" << (relative_e ? "M83\n" : "M82\n") <<
        "G92 E0\nG1 X10 Y10 Z0.2 F6000\nG1 X110 E10 F1200\n" <<
        (relative_e ? "G1 E-2 F1800\n" : "G1 E8 F1800\n") <<
        "G1 Y20 F6000\nG92 E0\nG1 X10 E3 F1200\n" <<
        (relative_e ? "G1 E-1 F1800\nG1 E1 F1800\nG1 X110 E2 F600\n" :
                      "G1 E2 F1800\nG1 E3 F1800\nG1 X110 E5 F600\n") <<
        "G4 P500\nG1 Y30 F6000\n";
    GCodeProcessor baseline;
    baseline.apply_config(config);
    baseline.process_file(file.string());
    CHECK(baseline.flow_timeline() == nullptr);
    GCodeProcessor captured;
    captured.enable_flow_timeline();
    captured.apply_config(config);
    captured.process_file(file.string());
    const auto* timeline = captured.flow_timeline();
    REQUIRE(timeline != nullptr);
    const double area = 0.25 * std::acos(-1.0) * 1.75 * 1.75;
    // E10 + (E3 repays E2) + E2; pure E1 recovery deposits no material.
    CHECK_THAT(timeline->deposited_volume(), WithinAbs(13.0 * area, 1e-4));
    CHECK_THAT(timeline->total_time(), WithinAbs(captured.get_time(PrintEstimatedStatistics::ETimeMode::Normal), 1e-7));
    CHECK_THAT(captured.get_time(PrintEstimatedStatistics::ETimeMode::Normal),
               WithinAbs(baseline.get_time(PrintEstimatedStatistics::ETimeMode::Normal), 1e-7));
    const auto& before = baseline.get_result().moves;
    const auto& after = captured.get_result().moves;
    REQUIRE(before.size() == after.size());
    for (size_t i = 0; i < before.size(); ++i) {
        CHECK(before[i].gcode_id == after[i].gcode_id);
        CHECK(before[i].type == after[i].type);
        CHECK_THAT((before[i].position - after[i].position).norm(), WithinAbs(0.0, 1e-7));
        CHECK_THAT(before[i].feedrate, WithinAbs(after[i].feedrate, 1e-7));
    }
    CHECK(std::any_of(timeline->intervals().begin(), timeline->intervals().end(), [](const auto& interval) {
        return interval.motion.kind == FlowMotionKind::Travel;
    }));
}

TEST_CASE("Arc subdivisions keep one source command without duplicating its extrusion", "[FlowTimeline][GCodeProcessor]")
{
    FullPrintConfig config;
    config.gcode_flavor.value = gcfKlipper;
    config.filament_diameter.values = {1.75};
    ScopedTemporaryFile file(".gcode");
    std::ofstream(file.string()) << "G21\nG90\nM83\nG1 X10 Y0 Z0.2 F1200\nG3 X-10 Y0 I-10 J0 E5 F1200\n";
    GCodeProcessor processor;
    processor.enable_flow_timeline();
    processor.apply_config(config);
    processor.process_file(file.string());
    const auto* timeline = processor.flow_timeline();
    REQUIRE(timeline != nullptr);
    CHECK_THAT(timeline->deposited_volume(), WithinAbs(5.0 * 0.25 * std::acos(-1.0) * 1.75 * 1.75, 1e-4));
    CHECK_THAT(timeline->total_time(), WithinAbs(processor.get_time(PrintEstimatedStatistics::ETimeMode::Normal), 1e-7));
    unsigned int source_line = 0;
    size_t extrusion_blocks = 0;
    for (const auto& interval : timeline->intervals()) {
        if (interval.motion.kind == FlowMotionKind::Extrusion) {
            ++extrusion_blocks;
            if (source_line == 0) source_line = interval.motion.source_line;
            CHECK(interval.motion.source_line == source_line);
        }
    }
    CHECK(extrusion_blocks > 1);
}
