#include "GCodeProvenance.hpp"
#include "libslic3r/I18N.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <charconv>
#include <cctype>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace Slic3r {
namespace {
constexpr std::string_view begin_marker = ";_ORCA_AT_PRIVATE_BEGIN:";
constexpr std::string_view end_marker = ";_ORCA_AT_PRIVATE_END:";

std::pair<bool, bool> classify_commands(std::string_view text)
{
    bool barrier = false;
    bool unknown = false;
    while (!text.empty()) {
        const size_t end = text.find('\n');
        auto line = text.substr(0, end);
        if (end == std::string_view::npos) text = {};
        else text.remove_prefix(end + 1);
        const size_t comment = line.find(';');
        line = line.substr(0, comment);
        const size_t start = line.find_first_not_of(" \t\r");
        if (start == std::string_view::npos) continue;
        line.remove_prefix(start);
        std::string command;
        if (line.front() == 'G' || line.front() == 'M' || line.front() == 'T') {
            size_t i = 1;
            while (i < line.size() && std::isdigit(static_cast<unsigned char>(line[i]))) ++i;
            command = line.substr(0, i);
        } else command = line.substr(0, line.find_first_of(" \t\r"));
        if (command == "M104" || command == "M109") barrier = true;
        else if (command != "G0" && command != "G1" && command != "G2" && command != "G3" &&
                 command != "G4" && command != "G10" && command != "G11" && command != "G90" &&
                 command != "G91" && command != "G92" && command != "M82" && command != "M83" &&
                 command != "M106" && command != "M107" && command != "M204" && command != "M205" &&
                 command != "SET_VELOCITY_LIMIT" && command != "SET_PRESSURE_ADVANCE" &&
                 command != "M400") {
            barrier = true;
            unknown = true;
        }
    }
    return {barrier, unknown};
}
}

std::string GCodeProvenance::wrap(std::string body, GCodeSectionKind kind)
{
    if (body.find_first_not_of(" \t\r\n") == std::string::npos) return body;
    if (body.find(begin_marker) != std::string::npos || body.find(end_marker) != std::string::npos)
        throw std::invalid_argument(_u8L("Custom G-code contains a reserved adaptive-temperature transport marker"));
    const auto [barrier, unknown] = classify_commands(body);
    std::uint64_t id;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        id = m_next_id++;
        m_registry.emplace(id, Entry{kind, barrier, unknown});
    }
    const auto key = std::to_string(id);
    return "\n" + std::string(begin_marker) + key + "\n" + body +
        (body.back() == '\n' ? "" : "\n") + std::string(end_marker) + key + "\n";
}

std::string GCodeProvenance::consume(std::string_view text)
{
    m_pending.append(text.data(), text.size());
    return drain(false);
}

std::string GCodeProvenance::drain(bool final)
{
    std::string output;
    size_t consumed = 0;
    while (consumed < m_pending.size()) {
        const size_t newline = m_pending.find('\n', consumed);
        if (newline == std::string::npos && !final) break;
        const size_t length = (newline == std::string::npos ? m_pending.size() : newline + 1) - consumed;
        std::string_view line(m_pending.data() + consumed, length);
        auto command = line;
        while (!command.empty() && (command.back() == '\n' || command.back() == '\r')) command.remove_suffix(1);
        const bool begin = command.substr(0, begin_marker.size()) == begin_marker;
        const bool end = command.substr(0, end_marker.size()) == end_marker;
        if (begin || end) {
            auto number = command.substr(begin ? begin_marker.size() : end_marker.size());
            std::uint64_t id = 0;
            const auto parsed = std::from_chars(number.data(), number.data() + number.size(), id);
            if (parsed.ec != std::errc{} || parsed.ptr != number.data() + number.size())
                throw std::runtime_error(_u8L("Invalid adaptive-temperature transport marker"));
            if (begin) {
                std::lock_guard<std::mutex> lock(m_mutex);
                const auto entry = m_registry.find(id);
                if (entry == m_registry.end()) throw std::runtime_error(_u8L("Unknown adaptive-temperature section"));
                m_active.push_back({id, m_line + 1, entry->second});
            } else {
                if (m_active.empty() || m_active.back().id != id)
                    throw std::runtime_error(_u8L("Unbalanced adaptive-temperature sections"));
                const auto active = m_active.back();
                m_active.pop_back();
                if (active.first_line <= m_line)
                    m_sections.push_back({active.first_line, m_line, active.entry.kind,
                                          active.entry.thermal_barrier, active.entry.unknown_runtime_command});
            }
        } else {
            output.append(line.data(), line.size());
            ++m_line;
        }
        consumed += length;
    }
    m_pending.erase(0, consumed);
    return output;
}

std::string GCodeProvenance::finish()
{
    std::string output = drain(true);
    if (!m_active.empty()) throw std::runtime_error(_u8L("An adaptive-temperature section was lost by a G-code filter"));
    std::sort(m_sections.begin(), m_sections.end(), [](const auto& a, const auto& b) { return a.first_line < b.first_line; });
    return output;
}

} // namespace Slic3r
