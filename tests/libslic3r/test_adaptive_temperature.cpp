#include "libslic3r/GCode/AdaptiveTemperaturePlanner.hpp"
#include "libslic3r/GCode/AdaptiveFlowPlanner.hpp"
#include "libslic3r/GCode/AdaptiveTemperatureReport.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/GCodeWriter.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/libslic3r.h"
#include "test_utils.hpp"
#include <fstream>
#include <ios>
#include <map>
#include <iterator>
#include <string>
#include "libslic3r/GCode/FlowTimeline.hpp"
#include "libslic3r/GCode/GCodeProvenance.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <locale>
#include <stdexcept>
#include <vector>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;
namespace {
FlowTimelineMotion motion(double time, double duration, double flow, unsigned int line)
{
    FlowTimelineMotion m;
    m.start_time = time;
    m.duration = duration;
    m.source_line = line;
    m.heater_id = 0;
    m.layer_id = 2;
    m.target_temperature = 230.0;
    m.kind = FlowMotionKind::Extrusion;
    m.distance = duration * 10.0;
    m.entry_speed = m.cruise_speed = m.exit_speed = 10.0;
    m.delta_e = flow * duration;
    m.filament_area = 1.0;
    return m;
}
AdaptiveTemperatureSettings settings()
{
    AdaptiveTemperatureSettings s;
    s.table = {{1.0, 230.0}, {10.0, 240.0}};
    s.minimum_temperature = 220.0;
    s.maximum_temperature = 250.0;
    s.smoothing_period = 0.0;
    return s;
}
}

TEST_CASE("Flow temperature tables reject incomplete and unordered data", "[AdaptiveTemperature]")
{
    auto s = settings();
    s.table = parse_flow_temperature_table("1,230\n5,235;10,240");
    REQUIRE(s.table.size() == 3);
    CHECK_THAT(s.table[1].flow, WithinAbs(5.0, 1e-9));
    CHECK_NOTHROW(validate_adaptive_temperature_settings(s));
    CHECK_THROWS_AS(parse_flow_temperature_table("1,230\n5"), std::invalid_argument);
    CHECK_THROWS_AS(parse_flow_temperature_table("1,230\nwrong"), std::invalid_argument);
    s.table[1].flow = 1.0;
    CHECK_THROWS_AS(validate_adaptive_temperature_settings(s), std::invalid_argument);
    s = settings();
    s.table[0].temperature = 210.0;
    CHECK_THROWS_AS(validate_adaptive_temperature_settings(s), std::invalid_argument);
    s = settings();
    s.fall_time = 0.0;
    CHECK_THROWS_AS(validate_adaptive_temperature_settings(s), std::invalid_argument);
    s = settings();
    s.smoothing_period = std::numeric_limits<double>::quiet_NaN();
    CHECK_THROWS_AS(validate_adaptive_temperature_settings(s), std::invalid_argument);
}

TEST_CASE("Target commands obey rise fall and emission limits together", "[AdaptiveTemperature]")
{
    auto s = settings();
    s.temperature_step = GENERATE(0.5, 0.6, 1.0);
    FlowTimeline timeline;
    for (unsigned int i = 0; i < 100; ++i) timeline.append(motion(i, 1.0, i < 40 ? 10.0 : 1.0, i + 1));
    const auto plan = plan_adaptive_temperature(timeline, s);
    REQUIRE_FALSE(plan.commands.empty());
    double time = 0.0;
    double temperature = 230;
    bool saw_cooling = false;
    for (const auto& cmd : plan.commands) {
        const double change = cmd.temperature - temperature;
        CHECK_THAT(std::abs(change), WithinAbs(s.temperature_step, 1e-9));
        CHECK(cmd.time - time + 1e-7 >= s.minimum_command_interval);
        CHECK(cmd.time - time + 1e-7 >= std::abs(change) * (change > 0 ? s.rise_time : s.fall_time));
        CHECK(cmd.temperature >= 220);
        CHECK(cmd.temperature <= 250);
        if (change < 0) saw_cooling = true;
        temperature = cmd.temperature;
        time = cmd.time;
    }
    CHECK(saw_cooling);
    CHECK_THAT(plan.commands.back().temperature, WithinAbs(230.0, 1e-9));
}

TEST_CASE("Sustained future flow starts the temperature rise in advance", "[AdaptiveTemperature]")
{
    FlowTimeline timeline;
    for (unsigned int i = 0; i < 70; ++i) timeline.append(motion(i, 1.0, i < 40 ? 1.0 : 10.0, i + 1));
    const auto plan = plan_adaptive_temperature(timeline, settings());
    CHECK(std::any_of(plan.commands.begin(), plan.commands.end(), [](const auto& c) { return c.time < 40.0 && c.temperature > 230; }));
    double at_transition = 230;
    for (const auto& c : plan.commands) if (c.time <= 40.0) at_transition = c.temperature;
    CHECK(at_transition >= 239);
}

TEST_CASE("A time window suppresses short alternating wall flow demand", "[AdaptiveTemperature]")
{
    FlowTimeline timeline;
    for (unsigned int i = 0; i < 120; ++i) timeline.append(motion(i * 0.5, 0.5, i % 2 ? 10.0 : 1.0, i + 1));
    auto s = settings();
    const auto unsmoothed = plan_adaptive_temperature(timeline, s);
    s.smoothing_period = 5.0;
    const auto smoothed = plan_adaptive_temperature(timeline, s);
    REQUIRE_FALSE(smoothed.commands.empty());
    REQUIRE_FALSE(unsmoothed.commands.empty());
    double smooth_peak = 230;
    double raw_peak = 230;
    for (const auto& c : smoothed.commands) smooth_peak = std::max(smooth_peak, c.temperature);
    for (const auto& c : unsmoothed.commands) raw_peak = std::max(raw_peak, c.temperature);
    CHECK(smooth_peak <= 236);
    CHECK(raw_peak >= 239);
}

TEST_CASE("Protected first layer receives no adaptive or anticipatory commands", "[AdaptiveTemperature]")
{
    FlowTimeline timeline;
    for (unsigned int i = 0; i < 50; ++i) {
        auto m = motion(i, 1.0, 10.0, i + 1);
        if (i < 10) { m.layer_id = 1; m.protected_region = true; }
        timeline.append(m);
    }
    const auto plan = plan_adaptive_temperature(timeline, settings());
    REQUIRE_FALSE(plan.commands.empty());
    for (const auto& cmd : plan.commands) {
        CHECK(cmd.time >= 10.0);
        CHECK(cmd.source_line > 10);
    }
}

TEST_CASE("Arc subdivisions create no extra temperature insertion boundaries", "[AdaptiveTemperature]")
{
    FlowTimeline timeline;
    for (unsigned int i = 0; i < 50; ++i)
        timeline.append(motion(i, 1.0, i < 30 ? 1.0 : 10.0, i < 30 ? i + 1 : 42));
    const auto plan = plan_adaptive_temperature(timeline, settings());
    CHECK(std::count_if(plan.commands.begin(), plan.commands.end(), [](const auto& c) { return c.source_line == 42; }) <= 1);
    for (const auto& c : plan.commands) if (c.source_line == 42) CHECK_THAT(c.time, WithinAbs(30.0, 1e-9));
}

TEST_CASE("A harmless layer script does not reset flow lookahead", "[AdaptiveTemperature]")
{
    FlowTimeline timeline;
    for (unsigned int i = 0; i < 70; ++i) timeline.append(motion(i, 1.0, i < 40 ? 1.0 : 10.0, 2 * i + 1));
    const auto original = plan_adaptive_temperature(timeline, settings());
    timeline.apply_provenance({{40, 40, GCodeSectionKind::LayerChange, false, false}}, true);
    const auto after = plan_adaptive_temperature(timeline, settings());
    REQUIRE(original.commands.size() == after.commands.size());
    for (size_t i = 0; i < original.commands.size(); ++i) {
        CHECK(original.commands[i].temperature == after.commands[i].temperature);
        CHECK_THAT(original.commands[i].time, WithinAbs(after.commands[i].time, 1e-9));
    }
}

TEST_CASE("A protected command that resets the target reinitializes the schedule", "[AdaptiveTemperature]")
{
    FlowTimeline timeline;
    for (unsigned int i = 0; i < 80; ++i) timeline.append(motion(i, 1.0, 10.0, i + 1));
    timeline.note_temperature_target(40, 230.0);
    timeline.apply_provenance({{40, 45, GCodeSectionKind::Custom, true, false}}, true);
    const auto plan = plan_adaptive_temperature(timeline, settings());
    for (const auto& c : plan.commands) {
        const bool protected_line = c.source_line >= 40 && c.source_line <= 45;
        CHECK_FALSE(protected_line);
    }
    const auto after = std::find_if(plan.commands.begin(), plan.commands.end(), [](const auto& c) { return c.source_line > 45; });
    REQUIRE(after != plan.commands.end());
    CHECK(after->temperature == 231);
}

TEST_CASE("Short temperature excursions do not create repeated heat cool cycles", "[AdaptiveTemperature]")
{
    auto s = settings();
    s.smoothing_period = 5.0;
    s.additional_lead_time = 4.7;
    s.rise_time = 2.6; s.fall_time = 2.3;
    FlowTimeline timeline;
    // Begin at the normal temperature, then alternate small demand excursions
    // much faster than the confirmation horizon. No motion is changed.
    for (unsigned int i = 0; i < 200; ++i) {
        auto m = motion(i * 0.5, 0.5, (i / 8) % 2 ? 4.0 : 5.8, i + 1);
        m.target_temperature = 235;
        timeline.append(m);
    }
    const auto plan = plan_adaptive_temperature(timeline, s);
    double previous = 235;
    int direction = 0;
    size_t reversals = 0;
    for (const auto& command : plan.commands) {
        const int next = command.temperature > previous ? 1 : -1;
        if (direction != 0 && next != direction) ++reversals;
        direction = next; previous = command.temperature;
    }
    CHECK(reversals == 0);
    CHECK(plan.commands.size() <= 1);
}
TEST_CASE("Sustained heating and cooling survive short excursion suppression", "[AdaptiveTemperature]")
{
    auto s = settings();
    s.smoothing_period = 5.0; s.additional_lead_time = 4.7;
    FlowTimeline timeline;
    for (unsigned int i = 0; i < 180; ++i) {
        auto m = motion(i, 1.0, i < 40 || i >= 110 ? 1.0 : 10.0, i + 1);
        timeline.append(m);
    }
    const auto plan = plan_adaptive_temperature(timeline, s);
    CHECK(std::any_of(plan.commands.begin(), plan.commands.end(), [](const auto& c) { return c.time < 40 && c.temperature >= 239; }));
    CHECK_THAT(plan.commands.back().temperature, WithinAbs(230.0, 1e-9));
}

TEST_CASE("Coordinated flow slows before a sustained flow drop and retains protected speeds", "[AdaptiveTemperature][AdaptiveFlow]")
{
    auto s = settings();
    s.rise_time = 2.0; s.fall_time = 2.0;
    FlowTimeline timeline;
    std::map<unsigned int, double> rates;
    for (unsigned int i = 0; i < 100; ++i) {
        auto m = motion(i, 1, i < 50 ? 10 : 1, i + 1);
        m.target_temperature = 240;
        if (i == 10) m.protected_region = true;
        if (i == 25) m.role = erBridgeInfill;
        timeline.append(m);
        rates.emplace(i + 1, 600);
    }
    const auto plan = plan_adaptive_flow(timeline, rates, s, 2);
    REQUIRE_FALSE(plan.commands.empty());
    bool slows_before_drop = false;
    for (const auto& command : plan.commands) {
        CHECK(command.feedrate_mm_min >= 120);
        CHECK(command.feedrate_mm_min <= 600);
        CHECK(command.source_line != 11);
        CHECK(command.source_line != 26);
        if (command.source_line > 30 && command.source_line < 50) slows_before_drop = true;
    }
    CHECK(slows_before_drop);
    s.table[1].temperature = s.table[0].temperature;
    CHECK_THROWS_AS(plan_adaptive_flow(timeline, rates, s, 2), std::invalid_argument);
}

TEST_CASE("Coordinated feedrate rewriting restores modal speed on travel and retains arc geometry", "[AdaptiveTemperature][AdaptiveFlow]")
{
    ScopedTemporaryFile file(".gcode");
    std::ofstream(file.string()) << "; test\nG1 X0 F6000\nG1 X1 E1 F1200\nG0 X2\nG2 X3 Y4 I1 J2 E2 F1800\nG1 E-1\n";
    auto rates = read_adaptive_flow_feedrates(file.string());
    REQUIRE(rates.size() == 5);
    CHECK_THAT(rates.at(4), WithinAbs(1200, 1e-6));
    AdaptiveFlowPlan plan;
    plan.commands = {{3, 600}, {5, 900}};
    write_adaptive_flow_plan(file.string(), plan);
    rates = read_adaptive_flow_feedrates(file.string());
    CHECK_THAT(rates.at(3), WithinAbs(600, 1e-6));
    CHECK_THAT(rates.at(4), WithinAbs(1200, 1e-6));
    CHECK_THAT(rates.at(5), WithinAbs(900, 1e-6));
    CHECK_THAT(rates.at(6), WithinAbs(1800, 1e-6));
    std::ifstream input(file.string());
    const std::string output{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    CHECK(output.find("G2 X3 Y4 I1 J2 E2") != std::string::npos);
    bool retraction_preserved = false;
    GCodeReader rewritten;
    rewritten.parse_buffer(output, [&](GCodeReader&, const GCodeReader::GCodeLine& line) {
        if (line.cmd_is("G1") && line.has(E) && std::abs(line.precise_value(E) + 1) < 1e-9)
            retraction_preserved = true;
    });
    CHECK(retraction_preserved);
    std::ofstream(file.string()) << "M220 S80\nG1 X1 E1 F1200\n";
    CHECK_THROWS_AS(read_adaptive_flow_feedrates(file.string()), std::invalid_argument);
}

TEST_CASE("Coordinated target demand excludes travel time from printing flow", "[AdaptiveTemperature][AdaptiveFlow]")
{
    FlowTimeline timeline;
    for (unsigned int i = 0; i < 60; ++i) {
        auto m = motion(i, 1, 10, i + 1);
        m.target_temperature = 240;
        if (i >= 10 && i < 40) { m.kind = FlowMotionKind::Travel; m.delta_e = 0; }
        timeline.append(m);
    }
    auto s = settings(); s.smoothing_period = 5;
    const auto ordinary = plan_adaptive_temperature(timeline, s);
    CHECK(std::any_of(ordinary.commands.begin(), ordinary.commands.end(), [](const auto& c) { return c.temperature < 239; }));
    s.deposition_time_only = true;
    const auto coordinated = plan_adaptive_temperature(timeline, s);
    for (const auto& command : coordinated.commands) CHECK(command.temperature >= 239);
}

TEST_CASE("Brief low flow does not impose a long coordinated slowdown", "[AdaptiveTemperature][AdaptiveFlow]")
{
    FlowTimeline timeline;
    std::map<unsigned int, double> rates;
    for (unsigned int i = 0; i < 100; ++i) {
        auto m = motion(i, 1, i == 50 ? 1 : 10, i + 1);
        m.target_temperature = 240;
        timeline.append(m); rates.emplace(i + 1, 600);
    }
    auto s = settings();
    CHECK_FALSE(plan_adaptive_flow(timeline, rates, s, 2).commands.empty());
    s.smoothing_period = 5;
    CHECK(plan_adaptive_flow(timeline, rates, s, 2).commands.empty());
    // A single long low-flow command must still constrain its approach.
    FlowTimeline long_valley;
    for (unsigned int i = 0; i < 100; ++i) {
        auto m = motion(i > 50 ? i + 9 : i, i == 50 ? 10 : 1, i == 50 ? 1 : 10, i + 1);
        m.target_temperature = 240;
        long_valley.append(m);
    }
    const auto plan = plan_adaptive_flow(long_valley, rates, s, 2);
    CHECK(std::any_of(plan.commands.begin(), plan.commands.end(), [](const auto& c) { return c.source_line < 51; }));
}

TEST_CASE("Adaptive diagnostics identify target rate and retained speed limits", "[AdaptiveTemperature][AdaptiveReport]")
{
    FlowTimeline timeline;
    std::map<unsigned int, double> rates;
    for (unsigned int i = 0; i < 100; ++i) {
        auto m = motion(i, 1, i < 50 ? 10 : 1, i + 1);
        if (i == 45) m.role = erBridgeInfill;
        timeline.append(m); rates.emplace(i + 1, 600);
    }
    const auto target = plan_adaptive_temperature(timeline, settings());
    CHECK(std::any_of(target.issues.begin(), target.issues.end(), [](const auto& issue) { return issue.reason == AdaptiveTemperatureReason::HeatingRate; }));
    const auto flow = plan_adaptive_flow(timeline, rates, settings(), 10);
    CHECK(std::any_of(flow.issues.begin(), flow.issues.end(), [](const auto& issue) { return issue.reason == AdaptiveTemperatureReason::MinimumSpeed; }));
    CHECK(std::any_of(flow.issues.begin(), flow.issues.end(), [](const auto& issue) { return issue.reason == AdaptiveTemperatureReason::BridgeSpeed; }));
    for (const auto& issue : flow.issues) {
        CHECK(issue.original_clock);
        CHECK(issue.layer == 2);
        CHECK(issue.boundaries > 0);
    }
}

TEST_CASE("Embedded adaptive reports preserve G-code and retain layer reason groups", "[AdaptiveTemperature][AdaptiveReport]")
{
    ScopedTemporaryFile file(".gcode");
    const std::string motion_text = "G1 X1 E1 F600\n";
    std::ofstream(file.string(), std::ios::binary) << motion_text;
    CHECK_FALSE(read_adaptive_temperature_report(file.string()).has_value());
    AdaptiveTemperatureReport report;
    report.coordinated = true; report.original_time = 120; report.planned_time = 132;
    report.has_target_range = true; report.minimum_target = 200; report.maximum_target = 220;
    report.commands = 2; report.slowed_moves = 4; report.constrained = true;
    add_adaptive_temperature_issue(report.issues, AdaptiveTemperatureReason::MinimumSpeed, 2, 15, 3, true);
    add_adaptive_temperature_issue(report.issues, AdaptiveTemperatureReason::MinimumSpeed, 2, 22, 4, true);
    std::vector<std::size_t> ends{motion_text.size()};
    append_adaptive_temperature_report(file.string(), report, ends);
    const auto loaded = read_adaptive_temperature_report(file.string());
    REQUIRE(loaded.has_value());
    CHECK(loaded->coordinated);
    CHECK_THAT(loaded->planned_time - loaded->original_time, WithinAbs(12, 1e-9));
    REQUIRE(loaded->issues.size() == 1);
    CHECK(loaded->total_issue_groups == 1);
    CHECK(loaded->issues[0].boundaries == 2);
    CHECK_THAT(loaded->issues[0].first_time, WithinAbs(15, 1e-9));
    CHECK_THAT(loaded->issues[0].last_time, WithinAbs(22, 1e-9));
    CHECK_THAT(loaded->issues[0].maximum_gap, WithinAbs(4, 1e-9));
    std::ifstream input(file.string(), std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    CHECK(text.substr(0, motion_text.size()) == motion_text);
    CHECK(ends.back() == text.size());
    std::ofstream(file.string()) << ";_ORCA_ADAPTIVE_REPORT_BEGIN_V1\n;{}\n";
    CHECK_THROWS(read_adaptive_temperature_report(file.string()));
}

TEST_CASE("Constant demand quantization is not reported as a short excursion", "[AdaptiveTemperature][AdaptiveReport]")
{
    FlowTimeline timeline;
    for (unsigned int i = 0; i < 3; ++i) timeline.append(motion(i, 1, 2.35, i + 1));
    const auto plan = plan_adaptive_temperature(timeline, settings());
    CHECK(std::any_of(plan.issues.begin(), plan.issues.end(), [](const auto& issue) { return issue.reason == AdaptiveTemperatureReason::CommandBoundary; }));
    CHECK_FALSE(std::any_of(plan.issues.begin(), plan.issues.end(), [](const auto& issue) { return issue.reason == AdaptiveTemperatureReason::ExcursionFilter; }));
}

TEST_CASE("Temperature steps reject values below half a degree and nonfinite values", "[AdaptiveTemperature]")
{
    auto s = settings();
    s.temperature_step = GENERATE(0.0, 0.49, -0.5, 5.1,
        std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN());
    CHECK_THROWS_AS(validate_adaptive_temperature_settings(s), std::invalid_argument);
}

TEST_CASE("Fractional starting targets are preserved by temperature planning", "[AdaptiveTemperature]")
{
    auto s = settings();
    s.temperature_step = 0.5;
    FlowTimeline timeline;
    for (unsigned int i = 0; i < 50; ++i) {
        auto m = motion(i, 1.0, 10.0, i + 1);
        m.target_temperature = 230.25;
        timeline.append(m);
    }
    const auto plan = plan_adaptive_temperature(timeline, s);
    REQUIRE_FALSE(plan.commands.empty());
    CHECK_THAT(plan.commands.front().temperature, WithinAbs(230.75, 1e-9));
}

TEST_CASE("Adaptive temperature commands retain decimals regardless of locale", "[AdaptiveTemperature]")
{
    struct CommaDecimal : std::numpunct<char> {
        char do_decimal_point() const override { return ','; }
    };
    const auto previous = std::locale::global(std::locale(std::locale::classic(), new CommaDecimal));
    const ScopeGuard restore([previous] { std::locale::global(previous); });
    CHECK(GCodeWriter::set_temperature(230.5, gcfKlipper) == "M104 S230.5 ; set nozzle temperature\n");
    CHECK(GCodeWriter::set_temperature(230.75, gcfKlipper) == "M104 S230.75 ; set nozzle temperature\n");
    CHECK(GCodeWriter::set_temperature(230, gcfKlipper) == "M104 S230 ; set nozzle temperature\n");
}

TEST_CASE("Ironing uses a separate temperature and returns to flow control", "[AdaptiveTemperature]")
{
    auto s = settings();
    s.ironing_temperature = 245.0;
    FlowTimeline timeline;
    std::map<unsigned int, double> feedrates;
    for (unsigned int i = 0; i < 300; ++i) {
        auto m = motion(i, 1.0, 0.1, i + 1);
        if (i >= 80 && i < 200) m.role = erIroning;
        timeline.append(m);
        feedrates.emplace(i + 1, 600.0);
    }
    const auto plan = plan_adaptive_temperature(timeline, s);
    REQUIRE_FALSE(plan.commands.empty());
    bool anticipates = false, reaches_target = false;
    for (const auto& command : plan.commands) {
        if (command.time < 80 && command.temperature > 230.0) anticipates = true;
        if (command.time >= 80 && command.time < 200 && command.temperature >= 244.0) reaches_target = true;
        CHECK(command.temperature <= s.maximum_temperature);
    }
    CHECK(anticipates);
    CHECK(reaches_target);
    CHECK_THAT(plan.commands.back().temperature, WithinAbs(230.0, s.temperature_step));
    FlowTimeline speed_timeline;
    for (unsigned int i = 0; i < 300; ++i) {
        auto m = motion(i, 1.0, 10.0, i + 1);
        if (i >= 80 && i < 200) m.role = erIroning;
        speed_timeline.append(m);
    }
    const auto flow_plan = plan_adaptive_flow(speed_timeline, feedrates, s, 5.0);
    REQUIRE_FALSE(flow_plan.commands.empty());
    for (const auto& command : flow_plan.commands) {
        CHECK((command.source_line < 81 || command.source_line > 200));
    }
    s.ironing_temperature.reset();
    const auto ordinary = plan_adaptive_temperature(timeline, s);
    CHECK(ordinary.commands.empty());
    s.ironing_temperature = std::numeric_limits<double>::quiet_NaN();
    CHECK_THROWS_AS(validate_adaptive_temperature_settings(s), std::invalid_argument);
}
