#include "ThermalResponse.hpp"
#include "PrintConfig.hpp"
#include "Config.hpp"
#include "I18N.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <istream>
#include <locale>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {
namespace {
std::vector<std::string> split_row(const std::string& line)
{
    std::vector<std::string> fields;
    size_t begin = 0;
    for (;;) {
        const auto end = line.find(',', begin);
        auto value = line.substr(begin, end == std::string::npos ? end : end - begin);
        const auto first = value.find_first_not_of(" \t\r");
        if (first == std::string::npos) value.clear();
        else value = value.substr(first, value.find_last_not_of(" \t\r") - first + 1);
        fields.push_back(std::move(value));
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return fields;
}
void validate(const std::vector<ThermalSample>& samples)
{
    if (samples.size() < 2) throw std::invalid_argument(_u8L("The thermal log needs at least two samples."));
    double previous = -1.0;
    for (const auto& s : samples) {
        if (!std::isfinite(s.time_s) || s.time_s < 0 || s.time_s <= previous ||
            !std::isfinite(s.target_c) || s.target_c < 0 || s.target_c > 500 ||
            !std::isfinite(s.temperature_c) || s.temperature_c < -50 || s.temperature_c > 500)
            throw std::invalid_argument(_u8L("Thermal samples need increasing timestamps and finite temperatures within the supported range."));
        for (const auto& v : {s.power, s.fan, s.auxiliary_fan, s.chamber_fan})
            if (v && (!std::isfinite(*v) || *v < 0 || *v > 1))
                throw std::invalid_argument(_u8L("Heater power and fan values must be between zero and one."));
        if (s.flow_mm3_s && (!std::isfinite(*s.flow_mm3_s) || *s.flow_mm3_s < 0))
            throw std::invalid_argument(_u8L("Recorded flow must be finite and nonnegative."));
        if (s.commanded_flow_mm3_s && (!std::isfinite(*s.commanded_flow_mm3_s) || *s.commanded_flow_mm3_s < -0.05))
            throw std::invalid_argument(_u8L("Commanded flow must be finite and cannot contain retractions in the loaded calibration."));
        previous = s.time_s;
    }
}
}

std::vector<ThermalSample> read_thermal_response_csv(std::istream& input)
{
    std::map<std::string, size_t> columns;
    std::vector<ThermalSample> samples;
    std::string line;
    while (std::getline(input, line)) {
        if (line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);
        const auto start = line.find_first_not_of(" \t\r");
        if (start == std::string::npos || line[start] == '#') continue;
        const auto values = split_row(line);
        if (columns.empty()) {
            for (size_t i = 0; i < values.size(); ++i)
                if (values[i].empty() || !columns.emplace(values[i], i).second)
                    throw std::invalid_argument(_u8L("The thermal CSV has empty or duplicate column names."));
            for (const auto* key : {"time_s", "target_c", "temperature_c"})
                if (columns.find(key) == columns.end())
                    throw std::invalid_argument(_u8L("The thermal CSV needs time_s, target_c and temperature_c columns."));
            continue;
        }
        if (values.size() != columns.size())
            throw std::invalid_argument(_u8L("The thermal CSV has a row with an incorrect number of columns."));
        auto number = [&](const char* key) -> std::optional<double> {
            const auto it = columns.find(key);
            if (it == columns.end() || values[it->second].empty()) return {};
            std::istringstream parser(values[it->second]);
            parser.imbue(std::locale::classic());
            double value{};
            if (!(parser >> value)) throw std::invalid_argument(_u8L("The thermal CSV contains an invalid number. Use a decimal point."));
            parser >> std::ws;
            if (!parser.eof()) throw std::invalid_argument(_u8L("The thermal CSV contains an invalid number. Use a decimal point."));
            return value;
        };
        auto time = number("time_s"), target = number("target_c"), temperature = number("temperature_c");
        if (!time || !target || !temperature)
            throw std::invalid_argument(_u8L("A required thermal CSV value is missing."));
        ThermalSample sample{*time, *target, *temperature, number("power"), number("fan"), number("flow_mm3_s")};
        sample.commanded_flow_mm3_s = number("requested_flow_mm3_s");
        sample.auxiliary_fan = number("pin_fan2");
        sample.chamber_fan = number("pin_fan1");
        const auto stage = columns.find("stage");
        if (stage != columns.end()) sample.stage = values[stage->second];
        samples.push_back(std::move(sample));
    }
    if (input.bad()) throw std::runtime_error(_u8L("The thermal CSV could not be read completely."));
    validate(samples);
    return samples;
}

ThermalResponse analyze_thermal_response(const std::vector<ThermalSample>& samples)
{
    validate(samples);
    ThermalResponse result;
    for (size_t begin = 1; begin < samples.size(); ++begin) {
        const auto& first = samples[begin];
        const double change = first.target_c - samples[begin - 1].target_c;
        if (std::abs(change) < 5.0 || first.target_c <= 0) continue;
        size_t end = begin + 1;
        while (end < samples.size() && std::abs(samples[end].target_c - first.target_c) < 0.1) ++end;
        ThermalTransition transition;
        transition.time_s = first.time_s;
        transition.from_c = samples[begin - 1].target_c;
        transition.to_c = first.target_c;
        auto issue = [&](ThermalIssue value) {
            if (std::find(transition.issues.begin(), transition.issues.end(), value) == transition.issues.end())
                transition.issues.push_back(value);
        };
        size_t baseline = begin - 1;
        while (baseline > 0 && samples[baseline].time_s > first.time_s - 5.0 &&
               std::abs(samples[baseline - 1].target_c - transition.from_c) < 0.1) --baseline;
        if (first.time_s - samples[baseline].time_s < 5.0) issue(ThermalIssue::UnstableBaseline);
        for (size_t i = baseline; i < begin; ++i)
            if (std::abs(samples[i].temperature_c - transition.from_c) > 1.0) issue(ThermalIssue::UnstableBaseline);
        const double direction = change > 0 ? 1.0 : -1.0;
        std::optional<double> t10, t90;
        for (size_t i = baseline; i < end; ++i) {
            const auto& s = samples[i];
            if (i > baseline && s.time_s - samples[i - 1].time_s > 2.5) issue(ThermalIssue::DataGap);
            if (!s.fan || !s.flow_mm3_s) issue(ThermalIssue::MissingConditions);
            else if (first.fan && first.flow_mm3_s &&
                (std::abs(*s.fan - *first.fan) > 0.02 ||
                 std::abs(*s.flow_mm3_s - *first.flow_mm3_s) > std::max(0.05, 0.05 * *first.flow_mm3_s)))
                issue(ThermalIssue::ChangingConditions);
            if (i < begin) continue;
            transition.overshoot_c = std::max(transition.overshoot_c, direction * (s.temperature_c - first.target_c));
            auto crossing = [&](double fraction, std::optional<double>& time) {
                if (time) return;
                const double level = transition.from_c + fraction * change;
                const auto& prior = samples[i - 1];
                if (direction * (s.temperature_c - level) >= 0 && direction * (prior.temperature_c - level) < 0) {
                    const double estimate = prior.time_s + (s.time_s - prior.time_s) *
                        (level - prior.temperature_c) / (s.temperature_c - prior.temperature_c);
                    time = std::max(0.0, estimate - first.time_s);
                }
            };
            const double progress = direction * (s.temperature_c - transition.from_c);
            if (progress > 0.1 * std::abs(change) && progress < 0.9 * std::abs(change) &&
                direction * (s.temperature_c - samples[i - 1].temperature_c) < -2.0)
                issue(ThermalIssue::NoisyResponse);
            crossing(0.1, t10);
            crossing(0.9, t90);
        }
        // Settling requires every remaining sample to stay in the band and at
        // least ten seconds of observed dwell, not a single noisy crossing.
        size_t settled = end;
        for (size_t i = end; i > begin; --i) {
            if (std::abs(samples[i - 1].temperature_c - first.target_c) > 1.0) break;
            settled = i - 1;
        }
        if (settled < end && samples[end - 1].time_s - samples[settled].time_s >= 10.0)
            transition.settling_s = samples[settled].time_s - first.time_s;
        if (!transition.settling_s || !t10 || !t90 || *t90 <= *t10) issue(ThermalIssue::Unsettled);
        if (t10 && t90 && *t90 > *t10) {
            transition.seconds_per_degree = (*t90 - *t10) / (0.8 * std::abs(change));
            transition.delay_s = std::max(0.0, *t10 - 0.1 * std::abs(change) * transition.seconds_per_degree);
        }
        if (transition.usable()) {
            auto& rate = change > 0 ? result.rise_seconds_per_degree : result.fall_seconds_per_degree;
            rate = std::max(rate.value_or(0.0), transition.seconds_per_degree);
            result.delay_s = std::max(result.delay_s, transition.delay_s);
        }
        result.transitions.push_back(std::move(transition));
        begin = end - 1;
    }
    return result;
}

std::vector<LoadedThermalStage> analyze_loaded_thermal_response(const std::vector<ThermalSample>& samples)
{
    validate(samples);
    std::vector<LoadedThermalStage> result;
    for (size_t begin = 0; begin < samples.size();) {
        size_t end = begin + 1;
        while (end < samples.size() && samples[end].stage == samples[begin].stage) ++end;
        if (samples[begin].stage.empty() || samples[begin].stage == "first_layers") {
            begin = end;
            continue;
        }
        for (const auto& stage : result)
            if (stage.name == samples[begin].stage)
                throw std::invalid_argument(_u8L("A loaded calibration stage occurs in separate parts of the log."));
        LoadedThermalStage stage;
        stage.name = samples[begin].stage;
        stage.begin_s = samples[begin].time_s;
        stage.end_s = samples[end - 1].time_s;
        stage.target_c = samples[end - 1].target_c;
        double volume = 0.0, duration = 0.0;
        bool complete_flow = true;
        for (size_t i = begin + 1; i < end; ++i) {
            const auto& a = samples[i - 1]; const auto& b = samples[i];
            if (!a.commanded_flow_mm3_s || !b.commanded_flow_mm3_s) { complete_flow = false; continue; }
            const double dt = b.time_s - a.time_s;
            const double overlap = std::max(0.0, b.time_s - std::max(a.time_s, stage.end_s - 5.0));
            if (overlap > 0) {
                const double v0 = *a.commanded_flow_mm3_s + (*b.commanded_flow_mm3_s - *a.commanded_flow_mm3_s) * (1 - overlap / dt);
                volume += 0.5 * (v0 + *b.commanded_flow_mm3_s) * overlap;
                duration += overlap;
            }
        }
        if (complete_flow && duration >= 4.99) stage.commanded_flow_mm3_s = volume / duration;
        if (!result.empty() && result.back().commanded_flow_mm3_s && stage.commanded_flow_mm3_s)
            stage.load_change = std::abs(*result.back().commanded_flow_mm3_s - *stage.commanded_flow_mm3_s) >
                std::max(0.1, 0.1 * *stage.commanded_flow_mm3_s);
        if (!result.empty() && std::abs(stage.target_c - result.back().target_c) >= 5.0) {
            // Include enough of the preceding plateau for baseline qualification.
            size_t baseline = begin;
            while (baseline > 0 && samples[baseline].time_s > stage.begin_s - 6.0) --baseline;
            std::vector<ThermalSample> window(samples.begin() + baseline, samples.begin() + end);
            bool missing = false, changing = stage.load_change;
            const auto& reference = samples[end - 1];
            for (auto& sample : window) {
                sample.flow_mm3_s = sample.commanded_flow_mm3_s;
                if (!sample.auxiliary_fan || !sample.chamber_fan) missing = true;
                else if (!reference.auxiliary_fan || !reference.chamber_fan ||
                         std::abs(*sample.auxiliary_fan - *reference.auxiliary_fan) > 0.02 ||
                         std::abs(*sample.chamber_fan - *reference.chamber_fan) > 0.02) changing = true;
            }
            const auto response = analyze_thermal_response(window);
            if (!response.transitions.empty()) {
                stage.transition = response.transitions.back();
                if (missing) stage.transition->issues.push_back(ThermalIssue::MissingConditions);
                if (changing) stage.transition->issues.push_back(ThermalIssue::ChangingConditions);
            }
        }
        result.push_back(std::move(stage));
        begin = end;
    }
    return result;
}

std::optional<ThermalTargetProposal> propose_thermal_target_settings(const std::vector<ThermalSample>& samples)
{
    const auto loaded = analyze_loaded_thermal_response(samples);
    std::vector<ThermalTransition> transitions;
    if (loaded.empty()) {
        // Ordinary logs have a declared deposition flow, not a commanded one.
        // A stationary/no-load experiment cannot establish printing settings.
        for (const auto& sample : samples)
            if (!sample.flow_mm3_s || *sample.flow_mm3_s <= 0.05) return {};
        transitions = analyze_thermal_response(samples).transitions;
    } else {
        for (const auto& stage : loaded)
            if (!stage.load_change && stage.commanded_flow_mm3_s && *stage.commanded_flow_mm3_s > 0.05 && stage.transition)
                transitions.push_back(*stage.transition);
    }
    ThermalTargetProposal proposal;
    for (const auto& t : transitions) {
        if (!t.usable() || !t.settling_s || t.seconds_per_degree <= 0) continue;
        const double delta = std::abs(t.to_c - t.from_c);
        if (delta < 5.0) continue;
        // Map a measured step to a provisional target ramp: remove the delay
        // (supplied separately as lead), retain the slower settling envelope,
        // and add 25% margin. This policy is not a fitted thermal/PID model.
        const double envelope = std::max(t.seconds_per_degree, std::max(0.0, *t.settling_s - t.delay_s) / delta);
        const bool heating = t.to_c > t.from_c;
        auto& slew = heating ? proposal.rise_time : proposal.fall_time;
        auto& measured = heating ? proposal.measured_rise_time : proposal.measured_fall_time;
        slew = std::max(slew, 1.25 * envelope);
        measured = std::max(measured, t.seconds_per_degree);
        proposal.lead_time = std::max(proposal.lead_time, t.delay_s);
    }
    if (proposal.rise_time <= 0 || proposal.fall_time <= 0) return {};
    auto round_up = [](double value) { return std::ceil(value * 10.0 - 1e-9) / 10.0; };
    proposal.rise_time = round_up(std::max(0.1, proposal.rise_time));
    proposal.fall_time = round_up(std::max(0.1, proposal.fall_time));
    // Target changes are first observed at a sample, not timestamped at
    // command transmission. Retain one observed acquisition interval as lead.
    double acquisition_interval = 0.0;
    for (size_t i = 1; i < samples.size(); ++i) {
        if (!loaded.empty() && (samples[i].stage.empty() || samples[i].stage == "first_layers")) continue;
        const double gap = samples[i].time_s - samples[i - 1].time_s;
        if (gap <= 2.5) acquisition_interval = std::max(acquisition_interval, gap);
    }
    proposal.lead_time = round_up(proposal.lead_time + acquisition_interval);
    // Never clamp an unsupported result to a faster, optimistic setting.
    if (proposal.rise_time > 120 || proposal.fall_time > 120 || proposal.lead_time > 120) return {};
    return proposal;
}

DynamicPrintConfig thermal_target_settings_patch(const ThermalTargetProposal& proposal, bool include_smoothing)
{
    for (double value : {proposal.rise_time, proposal.fall_time})
        if (!std::isfinite(value) || value < 0.1 || value > 120)
            throw std::invalid_argument(_u8L("The proposed target slew setting is outside the supported range."));
    for (double value : {proposal.lead_time, proposal.smoothing_period})
        if (!std::isfinite(value) || value < 0 || value > 120)
            throw std::invalid_argument(_u8L("The proposed temperature lead or smoothing period is outside the supported range."));
    DynamicPrintConfig patch;
    patch.set_key_value("adaptive_temperature_rise_time", new ConfigOptionFloat(proposal.rise_time));
    patch.set_key_value("adaptive_temperature_fall_time", new ConfigOptionFloat(proposal.fall_time));
    patch.set_key_value("adaptive_temperature_lead_time", new ConfigOptionFloat(proposal.lead_time));
    if (include_smoothing)
        patch.set_key_value("adaptive_temperature_smoothing_period", new ConfigOptionFloat(proposal.smoothing_period));
    return patch;
}
} // namespace Slic3r
