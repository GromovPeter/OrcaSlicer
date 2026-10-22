#include "libslic3r/ThermalResponse.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Config.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <algorithm>
#include <cstddef>
#include <sstream>
#include <fstream>
#include <limits>
#include <string>
#include <stdexcept>
#include <vector>

using namespace Slic3r;
namespace {
std::vector<ThermalSample> response_log()
{
    std::vector<ThermalSample> result;
    for (int time = 0; time <= 150; ++time) {
        double target = 220, actual = 220;
        if (time >= 10 && time < 70) {
            target = 240;
            actual = std::clamp(220 + (time - 12) / 2.0, 220.0, 240.0);
        } else if (time >= 70) actual = std::clamp(240 - (time - 73) / 3.0, 220.0, 240.0);
        result.push_back({double(time), target, actual, 0.6, 0.5, 0.0});
    }
    return result;
}
}
TEST_CASE("Recorded heating and cooling retain independent response rates and delays", "[ThermalResponse]")
{
    const auto result = analyze_thermal_response(response_log());
    REQUIRE(result.transitions.size() == 2);
    CHECK(result.transitions[0].usable());
    CHECK(result.transitions[1].usable());
    REQUIRE(result.rise_seconds_per_degree);
    REQUIRE(result.fall_seconds_per_degree);
    CHECK_THAT(*result.rise_seconds_per_degree, Catch::Matchers::WithinAbs(2.0, 1e-8));
    CHECK_THAT(*result.fall_seconds_per_degree, Catch::Matchers::WithinAbs(3.0, 1e-8));
    CHECK_THAT(result.transitions[0].delay_s, Catch::Matchers::WithinAbs(2.0, 1e-8));
    CHECK_THAT(result.transitions[1].delay_s, Catch::Matchers::WithinAbs(3.0, 1e-8));
    CHECK(result.transitions[0].settling_s.has_value());
}
TEST_CASE("Thermal CSV accepts reordered numeric columns and optional context", "[ThermalResponse]")
{
    std::istringstream input("\xEF\xBB\xBF# synthetic\r\ntemperature_c,target_c,time_s,fan,flow_mm3_s,power\r\n220,220,0,0.5,0,0.2\r\n220,220,1,0.5,0,0.3\r\n");
    const auto samples = read_thermal_response_csv(input);
    REQUIRE(samples.size() == 2);
    CHECK_THAT(samples[1].time_s, Catch::Matchers::WithinAbs(1, 1e-8));
    REQUIRE(samples[0].fan);
    CHECK_THAT(*samples[0].fan, Catch::Matchers::WithinAbs(0.5, 1e-8));
}
TEST_CASE("Malformed thermal CSV and unordered measurements fail explicitly", "[ThermalResponse]")
{
    for (const auto* text : {
        "time_s,target_c\n0,220\n1,220\n",
        "time_s,target_c,temperature_c\n0,220,220\n0,220,220\n",
        "time_s,target_c,temperature_c\n0,220,220\n1,220,nan\n",
        "time_s,target_c,temperature_c\n0,220,220\n1,220,220,4\n",
        "time_s,target_c,temperature_c,fan\n0,220,220,50\n1,220,220,50\n"}) {
        std::istringstream input(text);
        CHECK_THROWS_AS(read_thermal_response_csv(input), std::invalid_argument);
    }
}
TEST_CASE("Incomplete cooling and changing fan conditions cannot produce a model", "[ThermalResponse]")
{
    auto samples = response_log();
    samples.resize(95);
    const auto incomplete = analyze_thermal_response(samples);
    REQUIRE(incomplete.rise_seconds_per_degree);
    CHECK_FALSE(incomplete.fall_seconds_per_degree);
    samples = response_log();
    for (size_t i = 10; i < samples.size(); ++i) samples[i].fan = 0.8;
    const auto changed = analyze_thermal_response(samples);
    CHECK_FALSE(changed.rise_seconds_per_degree);
    REQUIRE(changed.transitions.size() == 2);
    CHECK(std::find(changed.transitions[0].issues.begin(), changed.transitions[0].issues.end(), ThermalIssue::ChangingConditions) != changed.transitions[0].issues.end());
}
TEST_CASE("Missing extrusion context and sampling gaps exclude an otherwise complete response", "[ThermalResponse]")
{
    auto samples = response_log();
    for (auto& sample : samples) sample.flow_mm3_s.reset();
    const auto missing = analyze_thermal_response(samples);
    CHECK_FALSE(missing.rise_seconds_per_degree);
    CHECK_FALSE(missing.fall_seconds_per_degree);
    samples = response_log();
    samples.erase(samples.begin() + 20, samples.begin() + 24);
    const auto gap = analyze_thermal_response(samples);
    CHECK_FALSE(gap.rise_seconds_per_degree);
    CHECK(gap.fall_seconds_per_degree.has_value());
}
TEST_CASE("A brief target crossing is not mistaken for a settled thermal response", "[ThermalResponse]")
{
    auto samples = response_log();
    for (int i = 53; i < 70; ++i) samples[size_t(i)].temperature_c = 243;
    const auto result = analyze_thermal_response(samples);
    CHECK_FALSE(result.rise_seconds_per_degree);
    REQUIRE(result.transitions.size() == 2);
    CHECK_THAT(result.transitions[0].overshoot_c, Catch::Matchers::WithinAbs(3, 1e-8));
}

TEST_CASE("A noisy reversal cannot become an optimistic heating model", "[ThermalResponse]")
{
    auto samples = response_log();
    samples[30].temperature_c = 237;
    const auto result = analyze_thermal_response(samples);
    CHECK_FALSE(result.rise_seconds_per_degree);
    REQUIRE(result.transitions.size() == 2);
    const auto& issues = result.transitions[0].issues;
    CHECK(std::find(issues.begin(), issues.end(), ThermalIssue::NoisyResponse) != issues.end());
}

TEST_CASE("Recorded printing stages keep load changes separate from temperature responses", "[ThermalResponse][LoadedThermalResponse]")
{
    std::ifstream input(std::string(TEST_DATA_DIR) + "/thermal/printing-load.csv");
    REQUIRE(input.good());
    const auto samples = read_thermal_response_csv(input);
    const auto stages = analyze_loaded_thermal_response(samples);
    REQUIRE(stages.size() == 9);
    size_t accepted = 0, load_changes = 0;
    for (const auto& stage : stages) {
        CAPTURE(stage.name);
        if (stage.transition) {
            CHECK(stage.transition->usable());
            if (stage.transition->usable()) ++accepted;
        }
        if (stage.load_change) {
            ++load_changes;
            CHECK_FALSE(stage.transition.has_value());
        }
    }
    CHECK(accepted == 5);
    CHECK(load_changes == 3);
    REQUIRE(stages[7].commanded_flow_mm3_s);
    CHECK_THAT(*stages[7].commanded_flow_mm3_s, Catch::Matchers::WithinAbs(22, 0.1));
    CHECK_FALSE(samples.back().flow_mm3_s.has_value());
}
TEST_CASE("Loaded analysis refuses missing fan channels and interrupted extrusion", "[ThermalResponse][LoadedThermalResponse]")
{
    auto samples = response_log();
    for (auto& sample : samples) {
        sample.stage = sample.time_s < 10 ? "baseline" : sample.time_s < 70 ? "heating" : "cooling";
        sample.commanded_flow_mm3_s = 1;
        sample.auxiliary_fan = 0;
        sample.chamber_fan = 0;
    }
    const auto good = analyze_loaded_thermal_response(samples);
    REQUIRE(good.size() == 3);
    REQUIRE(good[1].transition);
    CHECK(good[1].transition->usable());
    for (auto& sample : samples) sample.auxiliary_fan.reset();
    const auto missing = analyze_loaded_thermal_response(samples);
    REQUIRE(missing[1].transition);
    CHECK_FALSE(missing[1].transition->usable());
    for (auto& sample : samples) sample.auxiliary_fan = 0;
    samples[20].commanded_flow_mm3_s = 0;
    const auto interrupted = analyze_loaded_thermal_response(samples);
    REQUIRE(interrupted[1].transition);
    CHECK_FALSE(interrupted[1].transition->usable());
}

TEST_CASE("Target proposals separate measured slopes from conservative ramp settings", "[ThermalResponse][ThermalTargetProposal]")
{
    auto samples = response_log();
    for (auto& sample : samples) sample.flow_mm3_s = 1;
    const auto proposal = propose_thermal_target_settings(samples);
    REQUIRE(proposal);
    CHECK_THAT(proposal->measured_rise_time, Catch::Matchers::WithinAbs(2, 1e-8));
    CHECK_THAT(proposal->measured_fall_time, Catch::Matchers::WithinAbs(3, 1e-8));
    CHECK_THAT(proposal->rise_time, Catch::Matchers::WithinAbs(2.5, 1e-8));
    CHECK_THAT(proposal->fall_time, Catch::Matchers::WithinAbs(3.8, 1e-8));
    CHECK_THAT(proposal->lead_time, Catch::Matchers::WithinAbs(4, 1e-8));
}
TEST_CASE("Stationary and incomplete thermal logs cannot propose printer settings", "[ThermalResponse][ThermalTargetProposal]")
{
    CHECK_FALSE(propose_thermal_target_settings(response_log()));
    auto samples = response_log();
    for (auto& sample : samples) sample.flow_mm3_s = 1;
    samples.resize(70);
    CHECK_FALSE(propose_thermal_target_settings(samples));
    samples = response_log();
    for (auto& sample : samples) { sample.flow_mm3_s = 1; sample.fan.reset(); }
    CHECK_FALSE(propose_thermal_target_settings(samples));
}
TEST_CASE("Measured printing data proposes target limits without copying sensor slopes", "[ThermalResponse][ThermalTargetProposal]")
{
    std::ifstream input(std::string(TEST_DATA_DIR) + "/thermal/printing-load.csv");
    REQUIRE(input.good());
    const auto proposal = propose_thermal_target_settings(read_thermal_response_csv(input));
    REQUIRE(proposal);
    CHECK(proposal->rise_time > proposal->measured_rise_time);
    CHECK(proposal->fall_time > proposal->measured_fall_time);
    CHECK(proposal->lead_time >= 3.5);
    CHECK(proposal->lead_time <= 5.0);
}
TEST_CASE("Thermal setting application changes only its printer fields and can be restored", "[ThermalResponse][ThermalTargetProposal]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("adaptive_temperature_smoothing_period", new ConfigOptionFloat(11));
    const auto original = config;
    ThermalTargetProposal proposal;
    proposal.rise_time = 2.6; proposal.fall_time = 2.4; proposal.lead_time = 3.6;
    const auto patch = thermal_target_settings_patch(proposal, false);
    REQUIRE(patch.keys().size() == 3);
    config.apply(patch);
    CHECK(config.diff(original).size() == 3);
    CHECK_THAT(config.opt_float("adaptive_temperature_smoothing_period"), Catch::Matchers::WithinAbs(11, 1e-8));
    CHECK_FALSE(patch.has("filament_adaptive_temperature"));
    CHECK_FALSE(patch.has("filament_flow_temperature_table"));
    DynamicPrintConfig previous;
    for (const auto& key : patch.keys()) previous.set_key_value(key, original.option(key)->clone());
    config.apply(previous);
    CHECK(config.diff(original).empty());
    const auto smoothed = thermal_target_settings_patch(proposal, true);
    REQUIRE(smoothed.keys().size() == 4);
    config.apply(smoothed);
    CHECK_THAT(config.opt_float("adaptive_temperature_smoothing_period"), Catch::Matchers::WithinAbs(5, 1e-8));
}
TEST_CASE("Invalid thermal proposals cannot create a partial settings patch", "[ThermalResponse][ThermalTargetProposal]")
{
    ThermalTargetProposal proposal;
    proposal.rise_time = 2; proposal.fall_time = 3; proposal.lead_time = 4;
    proposal.rise_time = std::numeric_limits<double>::quiet_NaN();
    CHECK_THROWS_AS(thermal_target_settings_patch(proposal, false), std::invalid_argument);
    proposal.rise_time = 121;
    CHECK_THROWS_AS(thermal_target_settings_patch(proposal, false), std::invalid_argument);
    proposal.rise_time = 2; proposal.lead_time = -1;
    CHECK_THROWS_AS(thermal_target_settings_patch(proposal, false), std::invalid_argument);
}
