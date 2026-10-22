#include "libslic3r/GCode/GCodeProvenance.hpp"

#include <catch2/catch_test_macros.hpp>

#include <stdexcept>
#include <string>

using namespace Slic3r;

TEST_CASE("Private section markers disappear across arbitrarily split output chunks", "[GCodeProvenance]")
{
    GCodeProvenance provenance;
    const auto wrapped = provenance.wrap("G92 E0\nM104 S240\n", GCodeSectionKind::LayerChange);
    std::string output;
    for (char c : wrapped) output += provenance.consume(std::string(1, c));
    output += provenance.finish();
    CHECK(output.find("PRIVATE") == std::string::npos);
    CHECK(output.find("G92 E0\nM104 S240\n") != std::string::npos);
    REQUIRE(provenance.sections().size() == 1);
    CHECK(provenance.sections()[0].first_line == 2);
    CHECK(provenance.sections()[0].last_line == 3);
    CHECK(provenance.sections()[0].thermal_barrier);
    CHECK_FALSE(provenance.sections()[0].unknown_runtime_command);
}

TEST_CASE("Layer position resets carry provenance without a thermal barrier", "[GCodeProvenance]")
{
    GCodeProvenance provenance;
    provenance.consume(provenance.wrap("G92 E0", GCodeSectionKind::LayerChange));
    provenance.finish();
    REQUIRE(provenance.sections().size() == 1);
    CHECK_FALSE(provenance.sections()[0].thermal_barrier);
    CHECK_FALSE(provenance.sections()[0].unknown_runtime_command);
}

TEST_CASE("Unknown printer macros are explicit in custom command provenance", "[GCodeProvenance]")
{
    GCodeProvenance provenance;
    provenance.consume(provenance.wrap("START_PRINT EXTRUDER_TEMP=230", GCodeSectionKind::MachineStart));
    provenance.finish();
    REQUIRE(provenance.sections().size() == 1);
    CHECK(provenance.sections()[0].kind == GCodeSectionKind::MachineStart);
    CHECK(provenance.sections()[0].thermal_barrier);
    CHECK(provenance.sections()[0].unknown_runtime_command);
}

TEST_CASE("Missing section ends fail closed instead of losing protection", "[GCodeProvenance]")
{
    GCodeProvenance provenance;
    const auto wrapped = provenance.wrap("G1 X10", GCodeSectionKind::Custom);
    const auto end = wrapped.find(";_ORCA_AT_PRIVATE_END:");
    provenance.consume(wrapped.substr(0, end));
    CHECK_THROWS_AS(provenance.finish(), std::runtime_error);
}
