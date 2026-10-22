#pragma once

#include "GCodeProvenance.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <vector>

namespace Slic3r {

enum ExtrusionRole : std::uint8_t;
enum class FlowMotionKind { Travel, Extrusion, Retract, Unretract, Wipe, Delay, Unknown };

// Finalized planner block, before preview vertices are inserted. Source IDs
// identify the input stream, not lines after the exporter's rewrite passes.
struct FlowTimelineMotion
{
    unsigned int source_line{0};
    unsigned int remaining_internal_moves{0};
    unsigned int filament_id{0};
    std::optional<unsigned int> heater_id;
    unsigned int layer_id{0}; // one-based, zero means not yet identified
    ExtrusionRole role{};
    FlowMotionKind kind{FlowMotionKind::Unknown};
    bool prepare_stage{false};
    bool thermal_barrier_before{false};
    bool reset_target_before{false};
    double target_temperature{0.0};
    bool protected_region{false};
    double start_time{0.0};
    double duration{0.0};
    double distance{0.0};
    double entry_speed{0.0};
    double cruise_speed{0.0};
    double exit_speed{0.0};
    double acceleration{0.0};
    double delta_e{0.0};
    double filament_area{0.0};
};

struct FlowTimelineInterval
{
    FlowTimelineMotion motion;
    double acceleration_time{0.0};
    double deceleration_time{0.0};
    double recovery_distance{0.0};
    double volume_per_distance{0.0};

    double end_time() const { return motion.start_time + motion.duration; }
    // Times are local to this interval. Recovery during a combined XY+E move
    // occupies time and distance but deposits no material until the debt is paid.
    double deposited_volume(double begin, double end) const;
    double volumetric_flow(double local_time) const;

private:
    double distance_at(double local_time) const;
    double raw_distance_at(double local_time) const;
};

// Explicitly enabled diagnostic collector. It never changes a motion plan or
// emits G-code. Retraction accounting currently supports one material per heater;
// tool changes and generator-owned protected boundaries need a separate adapter.
class FlowTimeline
{
public:
    void clear();
    void append(const FlowTimelineMotion& motion);
    void finish(double total_time);
    struct TemperatureTarget { unsigned int source_line; double temperature; };
    const std::vector<TemperatureTarget>& temperature_targets() const { return m_temperature_targets; }
    void note_temperature_target(unsigned int source_line, double temperature);
    void apply_provenance(const std::vector<GCodeSection>& sections, bool single_heater);
    void remap_source_lines(const std::function<unsigned int(unsigned int)>& remap);
    const std::vector<FlowTimelineInterval>& intervals() const { return m_intervals; }
    double total_time() const { return m_intervals.empty() ? 0.0 : m_intervals.back().end_time(); }
    double deposited_volume() const;

private:
    std::vector<FlowTimelineInterval> m_intervals;
    std::vector<TemperatureTarget> m_temperature_targets;
    std::map<unsigned int, double> m_retraction_debt;
    void append_delay(double end_time);
};

} // namespace Slic3r
