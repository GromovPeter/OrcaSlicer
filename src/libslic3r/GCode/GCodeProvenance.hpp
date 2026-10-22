#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace Slic3r {

enum class GCodeSectionKind { MachineStart, MachineEnd, FilamentStart, FilamentEnd, LayerChange, Custom };

struct GCodeSection
{
    unsigned int first_line{0};
    unsigned int last_line{0};
    GCodeSectionKind kind{GCodeSectionKind::Custom};
    bool thermal_barrier{false};
    bool unknown_runtime_command{false};
};

// Private transport through string-based speed/fan filters. Markers are consumed
// before either the output file or GCodeProcessor sees them; only typed ranges
// survive. Producer registration and ordered output consumption can run in
// different pipeline stages, so the registry is protected independently.
class GCodeProvenance
{
public:
    std::string wrap(std::string body, GCodeSectionKind kind);
    std::string consume(std::string_view text);
    std::string finish();
    const std::vector<GCodeSection>& sections() const { return m_sections; }

private:
    struct Entry {
        GCodeSectionKind kind;
        bool thermal_barrier;
        bool unknown_runtime_command;
    };
    struct Active { std::uint64_t id; unsigned int first_line; Entry entry; };
    std::mutex m_mutex;
    std::uint64_t m_next_id{1};
    std::map<std::uint64_t, Entry> m_registry;
    std::vector<Active> m_active;
    std::vector<GCodeSection> m_sections;
    std::string m_pending;
    unsigned int m_line{0};
    std::string drain(bool final);
};

} // namespace Slic3r
