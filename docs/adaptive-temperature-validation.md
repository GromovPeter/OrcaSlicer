# Adaptive-temperature validation

Run checks offline first. The commands below do not request a live printer.
Read `tests/AGENTS.md` before changing native tests. The native feature tests
belong to `libslic3r` and `fff_print`; test names/tags distinguish them from
upstream suites.

## Native tests

After a Release build with tests enabled, run from the repository root:

```bat
<build-directory>\tests\libslic3r\Release\libslic3r_tests.exe "[InterlayerAdhesion],[FlowTimeline],[AdaptiveTemperature],[ThermalResponse],[GCodeProvenance],[Preset],[Config]" --order rand
<build-directory>\tests\fff_print\Release\fff_print_tests.exe "[OverhangRecovery],[InterlayerAdhesion],[AdaptiveTemperature],[FlowTimeline]" --order rand
```

Replace `<build-directory>` with the configured build path. Standard CTest
entry points remain available through the upstream build.

The development B7c source baseline passed 325 core cases / 8,749 assertions
and 23 FFF cases / 10,356 assertions. Its original Creality filter was checked
against the immutable upstream source; enabled/disabled CLI slices succeeded
and disabling recovery retained the baseline executable commands. These are
historical baseline results, not a claim about every future revision.

## B11 validation

The Windows x64 Release B11 ironing implementation passed 22 adaptive planner
cases / 748 assertions and 19 FFF integration/calibration cases / 319,412
assertions. These cover ironing preheating, return to flow control, retained
ironing speed, material target limits and unchanged executable motion in
temperature-only mode. All changed C++ units passed clang-tidy. The development
user reported that B11 works; this is not a comparative study of ironing surface
quality across materials.

## Python tests

With a Python 3 interpreter available, run these five offline modules:

```bat
python -m unittest discover -s scripts/tests -p test_record_hotend_response.py
python -m unittest discover -s scripts/tests -p test_loaded_thermal_calibration.py
python -m unittest discover -s scripts/tests -p test_loaded_calibration_gui_worker.py
python -m unittest discover -s scripts/tests -p test_thermal_flow_prototype.py
python -m unittest discover -s scripts/tests -p test_creality_recovery_source.py
python scripts/check_creality_recovery.py
```

Recorder HTTP operations are mocked in the tests. Fixture verification checks
the actual bundled G-code, including its checksum. The volume-domain Python
prototype is an offline planning reference, not the native export algorithm.
The recorded numerical fixture in `tests/data/thermal/printing-load.csv` contains
relative timestamps, process measurements and stage names, without printer
addresses or credentials. It is test data, not a universal hotend model.

Run the repository clang-tidy workflow for changed C++ units. New UI strings
require the normal gettext extraction/catalog checks. Privacy cleanup changes
the blank URL default, fixture content/checksum and documentation; it introduces
no new translated UI strings.

## Manual review

1. Launch with isolated application settings and configure the intended presets.
2. With adaptation disabled, slice a baseline and retain its G-code/settings.
3. Enable temperature-only planning and inspect target commands, Temperature
   preview and the adaptive-temperature report. Check the first/second-layer
   handoff and an ordinary-temperature value above the adaptive table.
4. Enable coordinated flow separately. Compare Speed, Flow, Temperature,
   estimated duration and retained bridge/protected motion.
5. Import a thermal CSV and review the graph and Apply/Save/Undo behavior without
   starting recording. Create an adhesion cylinder and re-open its saved 3MF.
6. Test overhang recovery with a known long slow-to-fast outer-wall transition.
   Compare its commands, not only color scales between two slicers.

GUI acceptance is manual. User review of B7c reported Creality-like overhang
recovery appearance and closed that development issue. It was a preview check,
not a new physical print comparison.

## Physical and platform limits

Native tests and mocked API tests do not validate firmware, network failure
recovery on every printer, melt temperature or printed surface consistency.
The public cleaned fixture has not been physically reprinted during publication
preparation. Its checksum intentionally excludes the private development file.
Use the worker/fixture from the same revision and review the documented protocol.

Publication preparation also passed a Windows x64 Release application rebuild,
clang-tidy for the changed calibration dialog, all 44 tests in the five Python
modules above, the pinned Creality source check, and Git whitespace checks.
These checks do not replace a physical run of the cleaned fixture.

A comparative printing study across materials/profiles and Linux/macOS feature
validation remain open. When reporting an issue, include the revision, relevant
settings, feature mode and a minimal reproducible project or G-code region.
Remove credentials, private endpoints and irrelevant embedded configuration
before sharing files.
