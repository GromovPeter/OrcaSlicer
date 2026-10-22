#include "libslic3r/GCode/FlowTimeline.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <limits>
#include <stdexcept>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

namespace {
FlowTimelineMotion linear_motion(double start = 0.0)
{
    FlowTimelineMotion motion;
    motion.kind = FlowMotionKind::Extrusion;
    motion.start_time = start;
    motion.duration = 10.0;
    motion.distance = 100.0;
    motion.entry_speed = motion.cruise_speed = motion.exit_speed = 10.0;
    motion.delta_e = 10.0;
    motion.filament_area = 2.0;
    return motion;
}
}

TEST_CASE("Flow integrates acceleration and braking without duplicating volume", "[FlowTimeline]")
{
    FlowTimeline timeline;
    auto motion = linear_motion();
    motion.duration = 12.0;
    motion.entry_speed = motion.exit_speed = 0.0;
    motion.acceleration = 5.0;
    timeline.append(motion);
    const auto& interval = timeline.intervals().front();
    CHECK_THAT(interval.volumetric_flow(1.0), WithinAbs(1.0, 1e-10));
    CHECK_THAT(interval.volumetric_flow(6.0), WithinAbs(2.0, 1e-10));
    CHECK_THAT(interval.volumetric_flow(11.0), WithinAbs(1.0, 1e-10));
    CHECK_THAT(interval.deposited_volume(0.0, 2.0), WithinAbs(2.0, 1e-10));
    CHECK_THAT(interval.deposited_volume(2.0, 10.0), WithinAbs(16.0, 1e-10));
    CHECK_THAT(interval.deposited_volume(10.0, 12.0), WithinAbs(2.0, 1e-10));
    CHECK_THAT(timeline.deposited_volume(), WithinAbs(20.0, 1e-10));
}

TEST_CASE("Travel and unassigned delays remain on the flow time axis", "[FlowTimeline]")
{
    FlowTimeline timeline;
    auto travel = linear_motion(3.0);
    travel.kind = FlowMotionKind::Travel;
    travel.delta_e = 0.0;
    timeline.append(travel);
    timeline.append(linear_motion(13.0));
    timeline.finish(25.0);
    REQUIRE(timeline.intervals().size() == 4);
    CHECK(timeline.intervals()[0].motion.kind == FlowMotionKind::Delay);
    CHECK(timeline.intervals()[1].motion.kind == FlowMotionKind::Travel);
    CHECK(timeline.intervals()[3].motion.kind == FlowMotionKind::Delay);
    CHECK_THAT(timeline.total_time(), WithinAbs(25.0, 1e-10));
    CHECK_THAT(timeline.deposited_volume(), WithinAbs(20.0, 1e-10));
}

TEST_CASE("A moving unretract deposits only after it repays the filament debt", "[FlowTimeline]")
{
    FlowTimeline timeline;
    auto retract = linear_motion();
    retract.kind = FlowMotionKind::Retract;
    retract.delta_e = -4.0;
    timeline.append(retract);
    timeline.append(linear_motion(10.0));
    const auto& interval = timeline.intervals().back();
    CHECK_THAT(interval.recovery_distance, WithinAbs(40.0, 1e-10));
    CHECK_THAT(interval.volumetric_flow(3.0), WithinAbs(0.0, 1e-10));
    CHECK_THAT(interval.deposited_volume(0.0, 4.0), WithinAbs(0.0, 1e-10));
    CHECK_THAT(interval.volumetric_flow(5.0), WithinAbs(2.0, 1e-10));
    CHECK_THAT(timeline.deposited_volume(), WithinAbs(12.0, 1e-10));
}

TEST_CASE("Pure unretraction does not become deposited print flow", "[FlowTimeline]")
{
    FlowTimeline timeline;
    auto retract = linear_motion();
    retract.kind = FlowMotionKind::Retract;
    retract.delta_e = -4.0;
    timeline.append(retract);
    auto recover = linear_motion(10.0);
    recover.kind = FlowMotionKind::Unretract;
    recover.delta_e = 4.0;
    timeline.append(recover);
    timeline.append(linear_motion(20.0));
    CHECK_THAT(timeline.intervals()[1].deposited_volume(0.0, 10.0), WithinAbs(0.0, 1e-10));
    CHECK_THAT(timeline.deposited_volume(), WithinAbs(20.0, 1e-10));
}

TEST_CASE("Dividing a constant flow path preserves its time and deposited volume", "[FlowTimeline]")
{
    FlowTimeline whole;
    whole.append(linear_motion());
    FlowTimeline split;
    for (unsigned int i = 0; i < 100; ++i) {
        auto motion = linear_motion(0.1 * i);
        motion.duration = 0.1;
        motion.distance = 1.0;
        motion.delta_e = 0.1;
        split.append(motion);
    }
    CHECK_THAT(split.total_time(), WithinAbs(whole.total_time(), 1e-10));
    CHECK_THAT(split.deposited_volume(), WithinAbs(whole.deposited_volume(), 1e-10));
    for (const auto& interval : split.intervals())
        CHECK_THAT(interval.volumetric_flow(0.05), WithinAbs(2.0, 1e-10));
}

TEST_CASE("Filament retraction debts do not leak into another material", "[FlowTimeline]")
{
    FlowTimeline timeline;
    auto retract = linear_motion();
    retract.kind = FlowMotionKind::Retract;
    retract.delta_e = -20.0;
    timeline.append(retract);
    auto other = linear_motion(10.0);
    other.filament_id = 1;
    timeline.append(other);
    CHECK_THAT(timeline.deposited_volume(), WithinAbs(20.0, 1e-10));
}

TEST_CASE("Clearing the flow timeline removes its time and retraction state", "[FlowTimeline]")
{
    FlowTimeline timeline;
    auto retract = linear_motion();
    retract.delta_e = -20.0;
    timeline.append(retract);
    timeline.clear();
    timeline.append(linear_motion());
    CHECK_THAT(timeline.total_time(), WithinAbs(10.0, 1e-10));
    CHECK_THAT(timeline.deposited_volume(), WithinAbs(20.0, 1e-10));
}

TEST_CASE("Invalid motion profiles fail before they enter the flow timeline", "[FlowTimeline]")
{
    FlowTimeline timeline;
    auto motion = linear_motion();
    motion.duration = std::numeric_limits<double>::quiet_NaN();
    CHECK_THROWS_AS(timeline.append(motion), std::invalid_argument);
    motion = linear_motion();
    motion.duration = 0.0;
    CHECK_THROWS_AS(timeline.append(motion), std::invalid_argument);
    motion = linear_motion();
    motion.duration = 1.0;
    motion.entry_speed = motion.exit_speed = 0.0;
    motion.acceleration = 1.0;
    CHECK_THROWS_AS(timeline.append(motion), std::invalid_argument);
    CHECK(timeline.intervals().empty());
    timeline.append(linear_motion());
    CHECK_THROWS_AS(timeline.append(linear_motion(1.0)), std::invalid_argument);
    CHECK_THROWS_AS(timeline.finish(1.0), std::invalid_argument);
}
