#ifndef slic3r_GCode_InterestRegion_hpp_
#define slic3r_GCode_InterestRegion_hpp_

#include "../libslic3r.h"
#include "GCodeProcessor.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

namespace Slic3r {
namespace InterestRegion {

// Segment range expressed by end-vertex ssid indices (inclusive).
// A segment is the motion from (end_ssid-1) -> end_ssid.
// Therefore first_end_ssid >= 1 when used.
struct SegmentSpan
{
    size_t first_end_ssid{ 0 };
    size_t last_end_ssid{ 0 };
};

enum class SegmentTag : unsigned char
{
    None    = 0,
    Trigger = 1,
    Defect  = 2,
};

struct TaggedSpan
{
    SegmentSpan span;
    SegmentTag  tag{ SegmentTag::None };
};

class InterestObject
{
public:
    virtual ~InterestObject() = default;

    virtual const char* type_name() const = 0;
    virtual const std::vector<TaggedSpan>& spans() const = 0;
};

struct InterestRegion
{
    std::vector<std::unique_ptr<InterestObject>> objects;

    bool empty() const { return objects.empty(); }

    void apply_to_mask(std::vector<unsigned char>& mask) const
    {
        if (mask.size() < 2)
            return;

        for (const std::unique_ptr<InterestObject>& obj : objects) {
            for (const TaggedSpan& ts : obj->spans()) {
                const size_t first = std::max<size_t>(ts.span.first_end_ssid, 1);
                const size_t last  = std::min(ts.span.last_end_ssid, mask.size() - 1);
                if (last < first)
                    continue;

                const unsigned char v = static_cast<unsigned char>(ts.tag);
                for (size_t end_ssid = first; end_ssid <= last; ++end_ssid)
                    mask[end_ssid] = std::max(mask[end_ssid], v);
            }
        }
    }
};

inline bool is_extruding_move_type(EMoveType type)
{
    return (type == EMoveType::Extrude || type == EMoveType::Extrude_Alter);
}

inline bool defect_roles_contains(const std::vector<ExtrusionRole>& roles, ExtrusionRole role)
{
    return std::find(roles.begin(), roles.end(), role) != roles.end();
}

namespace detail {

class MoveVertexSegments
{
public:
    MoveVertexSegments(const std::vector<GCodeProcessorResult::MoveVertex>& moves,
                       const std::vector<size_t>& ssid_to_moveid_map,
                       const std::vector<int>* object_id_by_move_id)
        : m_moves(moves)
        , m_ssid_to_moveid_map(ssid_to_moveid_map)
        , m_object_id_by_move_id(object_id_by_move_id)
    {
    }

    size_t ssid_count() const { return m_ssid_to_moveid_map.size(); }

    bool valid_end_ssid(size_t end_ssid) const
    {
        if (end_ssid == 0 || end_ssid >= m_ssid_to_moveid_map.size())
            return false;
        const size_t move_id_end = m_ssid_to_moveid_map[end_ssid];
        const size_t move_id_beg = m_ssid_to_moveid_map[end_ssid - 1];
        return move_id_end < m_moves.size() && move_id_beg < m_moves.size();
    }

    bool is_extruding(size_t end_ssid) const
    {
        const size_t move_id_end = m_ssid_to_moveid_map[end_ssid];
        return is_extruding_move_type(m_moves[move_id_end].type);
    }

    bool is_arc(size_t end_ssid) const
    {
        const size_t move_id_end = m_ssid_to_moveid_map[end_ssid];
        return m_moves[move_id_end].is_arc_move();
    }

    float speed_mm_s(size_t end_ssid) const
    {
        const size_t move_id_end = m_ssid_to_moveid_map[end_ssid];
        return m_moves[move_id_end].feedrate;
    }

    double length_mm(size_t end_ssid) const
    {
        const size_t move_id_end = m_ssid_to_moveid_map[end_ssid];
        const size_t move_id_beg = m_ssid_to_moveid_map[end_ssid - 1];
        const auto&  mv_end      = m_moves[move_id_end];
        const Vec3f& p0          = m_moves[move_id_beg].position;
        const Vec3f& p1          = mv_end.position;

        double dist = 0.0;
        if (mv_end.is_arc_move_with_interpolation_points()) {
            Vec3f prev = p0;
            for (const Vec3f& ip : mv_end.interpolation_points) {
                dist += static_cast<double>((ip - prev).norm());
                prev = ip;
            }
            dist += static_cast<double>((p1 - prev).norm());
        } else {
            dist = static_cast<double>((p1 - p0).norm());
        }
        return dist;
    }

    float end_z(size_t end_ssid) const
    {
        const size_t move_id_end = m_ssid_to_moveid_map[end_ssid];
        return m_moves[move_id_end].position[2];
    }

    ExtrusionRole role(size_t end_ssid) const
    {
        const size_t move_id_end = m_ssid_to_moveid_map[end_ssid];
        return m_moves[move_id_end].extrusion_role;
    }

    int object_id(size_t end_ssid) const
    {
        if (m_object_id_by_move_id == nullptr)
            return -1;
        const size_t move_id_end = m_ssid_to_moveid_map[end_ssid];
        if (move_id_end >= m_object_id_by_move_id->size())
            return -1;
        return (*m_object_id_by_move_id)[move_id_end];
    }

private:
    const std::vector<GCodeProcessorResult::MoveVertex>& m_moves;
    const std::vector<size_t>&                           m_ssid_to_moveid_map;
    const std::vector<int>*                              m_object_id_by_move_id;
};

template<class MotionT>
class MotionSegments
{
public:
    explicit MotionSegments(const std::vector<MotionT>& motions)
        : m_motions(motions)
    {
    }

    size_t ssid_count() const { return m_motions.size() + 1; }

    bool valid_end_ssid(size_t end_ssid) const
    {
        return end_ssid > 0 && end_ssid <= m_motions.size();
    }

    bool is_extruding(size_t end_ssid) const { return m_motions[end_ssid - 1].extruding; }
    bool is_arc(size_t end_ssid) const { return m_motions[end_ssid - 1].is_arc; }

    float speed_mm_s(size_t end_ssid) const
    {
        return static_cast<float>(m_motions[end_ssid - 1].feedrate_mm_s);
    }

    double length_mm(size_t end_ssid) const { return static_cast<double>(m_motions[end_ssid - 1].length_mm); }

    float end_z(size_t end_ssid) const { return static_cast<float>(m_motions[end_ssid - 1].end.z()); }

    ExtrusionRole role(size_t end_ssid) const { return m_motions[end_ssid - 1].role; }

    int object_id(size_t end_ssid) const { return m_motions[end_ssid - 1].object_id; }

private:
    const std::vector<MotionT>& m_motions;
};

template<class SegmentsT>
inline std::vector<SegmentSpan> detect_continuous_slow_spans_impl(const SegmentsT& segments,
                                                                  float            max_speed_mm_s,
                                                                  float            min_continuous_time_s,
                                                                  bool             ignore_arc_moves)
{
    std::vector<SegmentSpan> spans;

    const size_t ssid_count = segments.ssid_count();
    if (ssid_count < 2)
        return spans;

    bool   in_run        = false;
    size_t run_first_end = 0;
    double run_time_s    = 0.0;

    auto flush_run = [&](size_t last_end_exclusive) {
        if (!in_run)
            return;
        if (run_time_s >= static_cast<double>(min_continuous_time_s) && run_first_end < last_end_exclusive)
            spans.push_back({ run_first_end, last_end_exclusive - 1 });
        in_run     = false;
        run_time_s = 0.0;
    };

    for (size_t end_ssid = 1; end_ssid < ssid_count; ++end_ssid) {
        if (!segments.valid_end_ssid(end_ssid)) {
            flush_run(end_ssid);
            continue;
        }

        if (!segments.is_extruding(end_ssid)) {
            flush_run(end_ssid);
            continue;
        }

        if (ignore_arc_moves && segments.is_arc(end_ssid)) {
            flush_run(end_ssid);
            continue;
        }

        const float speed_mm_s = segments.speed_mm_s(end_ssid);
        if (!(speed_mm_s > 0.0f) || speed_mm_s > max_speed_mm_s) {
            flush_run(end_ssid);
            continue;
        }

        const double dist_mm = segments.length_mm(end_ssid);
        const double dt_sec  = dist_mm / static_cast<double>(speed_mm_s);
        if (!std::isfinite(dt_sec) || dt_sec <= 0.0) {
            flush_run(end_ssid);
            continue;
        }

        if (!in_run) {
            in_run        = true;
            run_first_end = end_ssid;
            run_time_s    = 0.0;
        }

        run_time_s += dt_sec;
    }

    flush_run(ssid_count);
    return spans;
}

} // namespace detail

// Detect continuous slow extrusion segments.
// - `moves` are the original GCodeProcessor moves.
// - `ssid_to_moveid_map` maps "ssid" vertex indices used by the preview (seams removed) to indices into `moves`.
inline std::vector<SegmentSpan> detect_continuous_slow_spans(const std::vector<GCodeProcessorResult::MoveVertex>& moves,
                                                             const std::vector<size_t>& ssid_to_moveid_map,
                                                             float max_speed_mm_s,
                                                             float min_continuous_time_s,
                                                             bool ignore_arc_moves)
{
    if (moves.empty())
        return {};

    detail::MoveVertexSegments segments(moves, ssid_to_moveid_map, nullptr);
    return detail::detect_continuous_slow_spans_impl(segments, max_speed_mm_s, min_continuous_time_s, ignore_arc_moves);
}

// ---- Specific interest objects ----

struct AppearanceUnderExtrusionDefinition
{
    // Inducing segment criteria.
    // A continuous run of slow segments whose accumulated duration >= min_trigger_time_s is a trigger.
    float max_trigger_speed_mm_s{ 35.0f };
    float min_trigger_time_s{ 0.2f };

    // Defect segment criteria: immediately following segment must be one of these extrusion roles.
    // Include OverhangPerimeter by default, as it is a visible surface and frequently follows overhang slow-down triggers.
    std::vector<ExtrusionRole> defect_roles{ erExternalPerimeter, erOverhangPerimeter };

    // Defect span range control (cap):
    // Stop extending the defect span as soon as its accumulated length exceeds this value.
    // The terminating end-ssid is the first one that makes the accumulated length exceed the limit.
    // This is an upper bound only. The defect span may terminate earlier if:
    // 1) there are no more moves/ssids,
    // 2) Z leaves the trigger layer (when restrict_to_same_layer is true),
    // 3) a move no longer matches defect criteria (non-extruding, speed slow again, role mismatch, arc ignored, etc.).
    float defect_span_cap{ 20.0f };
    // Optional margin used when defect_span_cap is derived dynamically (e.g. from AUE L_safe_total):
    //   cap_mm = base_cap_mm + base_cap_mm * defect_span_cap_margin_percent + defect_span_cap_margin_mm
    // where percent is expressed as a fraction (0.10 means +10%).
    float defect_span_cap_margin_mm{ 1.0f };
    float defect_span_cap_margin_percent{ 0.0f };

    // If true, the defect segment must start and continue on the same layer (Z) as the trigger.
    // If it is set to false,may trigger out of sync!!
    // FIX ME
    bool restrict_to_same_layer{ true };

    // Default: Do not ignore arc moves (G2/G3) even if they were parsed for previewing.
    bool ignore_arc_moves{ false };
};

using DefectSpanCapBaseMmFn = std::function<double(float /*v_low_mm_s*/, int /*object_id*/)>;

inline double compute_aue_L_safe_accel_mm(const double v_low_mm_s, const double v_safe_mm_s, const double accel_mm_s2)
{
    if (!(accel_mm_s2 > 0.0))
        return 0.0;
    if (!(v_safe_mm_s > v_low_mm_s))
        return 0.0;
    const double L = (v_safe_mm_s * v_safe_mm_s - v_low_mm_s * v_low_mm_s) / (2.0 * accel_mm_s2);
    if (!std::isfinite(L) || L < 0.0)
        return 0.0;
    return L;
}

inline double compute_aue_L_safe_total_mm(const double v_low_mm_s, const double v_safe_mm_s, const double accel_mm_s2,
                                         const double L_safe_transition_mm, const double L_safe_cruise_mm)
{
    auto clamp_non_negative = [](double v) -> double { return (std::isfinite(v) && v > 0.0) ? v : 0.0; };
    const double Lt = clamp_non_negative(L_safe_transition_mm);
    const double Lc = clamp_non_negative(L_safe_cruise_mm);
    const double La = compute_aue_L_safe_accel_mm(v_low_mm_s, v_safe_mm_s, accel_mm_s2);
    return Lt + La + Lc;
}

class AppearanceUnderExtrusionInterestObject final : public InterestObject
{
public:
    AppearanceUnderExtrusionInterestObject(const SegmentSpan& trigger_span, const SegmentSpan& defect_span)
    {
        m_spans.reserve(2);
        m_spans.push_back({ trigger_span, SegmentTag::Trigger });
        m_spans.push_back({ defect_span, SegmentTag::Defect });
    }

    const char* type_name() const override { return "AppearanceUnderExtrusion"; }
    const std::vector<TaggedSpan>& spans() const override { return m_spans; }

private:
    std::vector<TaggedSpan> m_spans;
};

namespace detail {

template<class SegmentsT>
inline InterestRegion detect_appearance_under_extrusion_interest_region_impl(const SegmentsT&                       segments,
                                                                            const AppearanceUnderExtrusionDefinition& def,
                                                                            const DefectSpanCapBaseMmFn&             defect_span_cap_base_mm)
{
    InterestRegion region;

    const size_t ssid_count = segments.ssid_count();
    if (ssid_count < 2)
        return region;

    const std::vector<SegmentSpan> triggers =
        detect_continuous_slow_spans_impl(segments, def.max_trigger_speed_mm_s, def.min_trigger_time_s, def.ignore_arc_moves);

    constexpr float same_layer_z_eps = 1e-3f;

    for (const SegmentSpan& trigger : triggers) {
        if (!segments.valid_end_ssid(trigger.last_end_ssid))
            continue;

        const float trigger_z = segments.end_z(trigger.last_end_ssid);

        double defect_cap_mm = static_cast<double>(def.defect_span_cap);
        if (defect_span_cap_base_mm) {
            const int   object_id  = segments.object_id(trigger.last_end_ssid);
            const float v_low_mm_s = segments.speed_mm_s(trigger.last_end_ssid);
            const double base      = defect_span_cap_base_mm(v_low_mm_s, object_id);
            if (std::isfinite(base) && base > 0.0) {
                const double mm = (std::isfinite(def.defect_span_cap_margin_mm) && def.defect_span_cap_margin_mm > 0.0f) ? static_cast<double>(def.defect_span_cap_margin_mm) : 0.0;
                const double pct = (std::isfinite(def.defect_span_cap_margin_percent) && def.defect_span_cap_margin_percent > 0.0f) ? static_cast<double>(def.defect_span_cap_margin_percent) : 0.0;
                defect_cap_mm = base + base * pct + mm;
            }
        }

        const size_t effect_first_end = trigger.last_end_ssid + 1;
        if (effect_first_end >= ssid_count)
            continue;
        if (!segments.valid_end_ssid(effect_first_end))
            continue;

        if (def.restrict_to_same_layer && std::fabs(segments.end_z(effect_first_end) - trigger_z) > same_layer_z_eps)
            continue;
        if (!segments.is_extruding(effect_first_end))
            continue;

        const float v_effect = segments.speed_mm_s(effect_first_end);
        // Do not start the defect span if speed is still in the "slow" regime.
        // So defect span will not overlap with trigger span.
        // 这里有优化的空间：TODO
        if (!(v_effect > 0.0f) || v_effect < def.max_trigger_speed_mm_s)
            continue;
        if (!defect_roles_contains(def.defect_roles, segments.role(effect_first_end)))
            continue;
        if (def.ignore_arc_moves && segments.is_arc(effect_first_end))
            continue;

        size_t effect_last_end = effect_first_end;
        double effect_len_mm   = 0.0;
        for (size_t end_ssid = effect_first_end; end_ssid < ssid_count; ++end_ssid) {
            if (!segments.valid_end_ssid(end_ssid))
                break;

            if (def.restrict_to_same_layer && std::fabs(segments.end_z(end_ssid) - trigger_z) > same_layer_z_eps)
                break;
            if (!segments.is_extruding(end_ssid))
                break;
            // Stop if speed drops back into the "slow" regime.
            const float speed_mm_s = segments.speed_mm_s(end_ssid);
            if (!(speed_mm_s > 0.0f) || speed_mm_s < def.max_trigger_speed_mm_s)
                break;
            if (!defect_roles_contains(def.defect_roles, segments.role(end_ssid)))
                break;
            if (def.ignore_arc_moves && segments.is_arc(end_ssid))
                break;

            // Accumulate length and cap the defect span.
            const double dist = segments.length_mm(end_ssid);
            if (!std::isfinite(dist) || dist <= 0.0)
                break;
            effect_len_mm += dist;

            effect_last_end = end_ssid;
            if (effect_len_mm > defect_cap_mm)
                break;
        }

        region.objects.emplace_back(std::make_unique<AppearanceUnderExtrusionInterestObject>(
            trigger, SegmentSpan{ effect_first_end, effect_last_end }));
    }

    return region;
}

} // namespace detail

inline InterestRegion detect_appearance_under_extrusion_interest_region(const std::vector<GCodeProcessorResult::MoveVertex>& moves,
                                                                        const std::vector<size_t>& ssid_to_moveid_map,
                                                                        const AppearanceUnderExtrusionDefinition& def,
                                                                        const std::vector<int>* object_id_by_move_id = nullptr,
                                                                        const DefectSpanCapBaseMmFn& defect_span_cap_base_mm = {})
{
    detail::MoveVertexSegments segments(moves, ssid_to_moveid_map, object_id_by_move_id);
    return detail::detect_appearance_under_extrusion_interest_region_impl(segments, def, defect_span_cap_base_mm);
}

template<class MotionT>
inline InterestRegion detect_appearance_under_extrusion_interest_region(const std::vector<MotionT>& motions,
                                                                        const AppearanceUnderExtrusionDefinition& def,
                                                                        const DefectSpanCapBaseMmFn& defect_span_cap_base_mm = {})
{
    detail::MotionSegments<MotionT> segments(motions);
    return detail::detect_appearance_under_extrusion_interest_region_impl(segments, def, defect_span_cap_base_mm);
}

} // namespace InterestRegion
} // namespace Slic3r

#endif /* slic3r_GCode_InterestRegion_hpp_ */
