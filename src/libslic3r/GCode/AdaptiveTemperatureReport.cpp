#include "AdaptiveTemperatureReport.hpp"
#include "libslic3r/I18N.hpp"
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <ios>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace Slic3r {
namespace {
constexpr const char* begin_marker = ";_ORCA_ADAPTIVE_REPORT_BEGIN_V1";
constexpr const char* end_marker = ";_ORCA_ADAPTIVE_REPORT_END";
constexpr std::size_t maximum_bytes = 4 * 1024 * 1024;
double number(const nlohmann::json& value)
{
    const double result = value.get<double>();
    if (!std::isfinite(result) || result < 0) throw std::invalid_argument("Invalid adaptive report number");
    return result;
}
std::size_t count(const nlohmann::json& value)
{
    const double result = number(value);
    if (result > 1000000000 || std::floor(result) != result) throw std::invalid_argument("Invalid adaptive report count");
    return static_cast<std::size_t>(result);
}
}
void add_adaptive_temperature_issue(std::vector<AdaptiveTemperatureIssue>& issues,
    AdaptiveTemperatureReason reason, unsigned int layer, double time, double gap, bool original_clock)
{
    auto found = std::find_if(issues.rbegin(), issues.rend(), [&](const auto& issue) {
        return issue.reason == reason && issue.layer == layer && issue.original_clock == original_clock;
    });
    if (found == issues.rend()) issues.push_back({reason, layer, time, time, std::abs(gap), 1, original_clock});
    else {
        found->first_time = std::min(found->first_time, time);
        found->last_time = std::max(found->last_time, time);
        found->maximum_gap = std::max(found->maximum_gap, std::abs(gap));
        ++found->boundaries;
    }
}
void append_adaptive_temperature_report(const std::string& filename,
    const AdaptiveTemperatureReport& report, std::vector<std::size_t>& line_ends)
{
    nlohmann::json data = {{"version", 1}, {"coordinated", report.coordinated},
        {"original_time", report.original_time}, {"planned_time", report.planned_time},
        {"has_target_range", report.has_target_range}, {"minimum_target", report.minimum_target},
        {"maximum_target", report.maximum_target}, {"commands", report.commands},
        {"slowed_moves", report.slowed_moves}, {"constrained", report.constrained}};
    data["issues"] = nlohmann::json::array();
    // Bound file size for very tall models. Retain an explicit truncation count.
    data["issue_groups"] = report.issues.size();
    for (std::size_t i = 0; i < std::min<std::size_t>(report.issues.size(), 5000); ++i) {
        const auto& issue = report.issues[i];
        data["issues"].push_back({{"reason", static_cast<int>(issue.reason)}, {"layer", issue.layer},
            {"first_time", issue.first_time}, {"last_time", issue.last_time},
            {"maximum_gap", issue.maximum_gap}, {"boundaries", issue.boundaries}, {"original_clock", issue.original_clock}});
    }
    std::string text = std::string(begin_marker) + '\n';
    std::istringstream json_lines(data.dump(2));
    std::string line;
    while (std::getline(json_lines, line)) text += ";" + line + '\n';
    text += std::string(end_marker) + '\n';
    if (text.size() > maximum_bytes) throw std::runtime_error(_u8L("The adaptive temperature report is too large."));
    boost::nowide::ifstream input(filename, std::ios::binary);
    input.seekg(0, std::ios::end);
    const auto end = input.tellg();
    if (!input || end <= 0) throw std::runtime_error(_u8L("Could not read the adaptive temperature report output."));
    std::size_t bytes = static_cast<std::size_t>(end);
    input.seekg(-1, std::ios::end);
    char last = 0; input.get(last); input.close();
    boost::nowide::ofstream output(filename, std::ios::binary | std::ios::app);
    if (!output) throw std::runtime_error(_u8L("Could not append the adaptive temperature report."));
    if (last != '\n') {
        output.put('\n'); ++bytes;
        if (!line_ends.empty()) line_ends.back() = bytes;
    }
    std::istringstream lines(text);
    while (std::getline(lines, line)) {
        output << line << '\n'; bytes += line.size() + 1;
        line_ends.push_back(bytes);
    }
    output.flush();
    if (!output) throw std::runtime_error(_u8L("Could not save the adaptive temperature report."));
}
std::optional<AdaptiveTemperatureReport> read_adaptive_temperature_report(const std::string& filename)
{
    boost::nowide::ifstream input(filename, std::ios::binary);
    if (!input) throw std::runtime_error(_u8L("Could not open the G-code temperature report."));
    input.seekg(0, std::ios::end);
    const auto end = input.tellg();
    if (end < 0) throw std::runtime_error(_u8L("Could not read the G-code temperature report."));
    const auto bytes = std::min<std::size_t>(static_cast<std::size_t>(end), maximum_bytes);
    input.seekg(end - static_cast<std::streamoff>(bytes));
    std::string tail(bytes, '\0'); input.read(tail.data(), static_cast<std::streamsize>(bytes));
    if (!input && !input.eof()) throw std::runtime_error(_u8L("Could not read the G-code temperature report."));
    const auto begin = tail.rfind(begin_marker);
    if (begin == std::string::npos) return std::nullopt;
    const auto finish = tail.find(end_marker, begin);
    if (finish == std::string::npos) throw std::invalid_argument(_u8L("The adaptive temperature report is incomplete."));
    std::istringstream lines(tail.substr(begin + std::string(begin_marker).size(), finish - begin - std::string(begin_marker).size()));
    std::string line, json_text;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (line.front() != ';') throw std::invalid_argument(_u8L("The adaptive temperature report is damaged."));
        json_text += line.substr(1) + '\n';
    }
    const auto data = nlohmann::json::parse(json_text);
    if (data.at("version").get<int>() != 1) throw std::invalid_argument(_u8L("This adaptive temperature report version is unsupported."));
    AdaptiveTemperatureReport report;
    report.coordinated = data.at("coordinated").get<bool>();
    report.original_time = number(data.at("original_time")); report.planned_time = number(data.at("planned_time"));
    report.has_target_range = data.at("has_target_range").get<bool>();
    report.minimum_target = number(data.at("minimum_target")); report.maximum_target = number(data.at("maximum_target"));
    if (report.minimum_target > report.maximum_target) throw std::invalid_argument("Invalid adaptive target range");
    report.commands = count(data.at("commands")); report.slowed_moves = count(data.at("slowed_moves"));
    report.constrained = data.at("constrained").get<bool>();
    report.total_issue_groups = count(data.at("issue_groups"));
    const auto& issues = data.at("issues");
    if (!issues.is_array() || issues.size() > 5000) throw std::invalid_argument("Invalid adaptive report issue list");
    if (report.total_issue_groups < issues.size()) throw std::invalid_argument("Invalid adaptive report group count");
    for (const auto& row : issues) {
        const auto reason = count(row.at("reason"));
        if (reason > static_cast<std::size_t>(AdaptiveTemperatureReason::CommandBoundary)) throw std::invalid_argument("Invalid adaptive report reason");
        AdaptiveTemperatureIssue issue;
        issue.reason = static_cast<AdaptiveTemperatureReason>(reason);
        issue.layer = static_cast<unsigned int>(count(row.at("layer")));
        issue.first_time = number(row.at("first_time")); issue.last_time = number(row.at("last_time"));
        issue.maximum_gap = number(row.at("maximum_gap")); issue.boundaries = count(row.at("boundaries"));
        issue.original_clock = row.at("original_clock").get<bool>();
        if (issue.first_time > issue.last_time || issue.last_time > (issue.original_clock ? report.original_time : report.planned_time) + 1)
            throw std::invalid_argument("Invalid adaptive report time range");
        report.issues.push_back(issue);
    }
    return report;
}
} // namespace Slic3r
