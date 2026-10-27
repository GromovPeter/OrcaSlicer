# Adaptive Temperature B11 — Windows x64

B11 is an experimental OrcaSlicer fork release based on the adaptive-temperature
branch. It is independent of printer brand within the supported workflow:
one material, one physical Klipper hotend and layer-by-layer printing, including
spiral vase mode. Adaptive planning is not currently available for RRF or Marlin.

## Download and run

Download the Windows ZIP from the
[B11 release](https://github.com/GromovPeter/OrcaSlicer/releases/tag/adaptive-temperature-b11-20261027),
extract the entire archive and run **Launch-Adaptive-Orca.cmd**.
Keep the executable, DLLs, resources and bundled Python together.
Microsoft Edge WebView2 Runtime is required for the setup/web interface.

No personal presets are included. Configure the printer and filament or import
copies of your own presets. The launcher uses
`%LOCALAPPDATA%\OrcaSlicer-Adaptive-Temperature-Experimental` for application
settings and its own WebView2 profile, separate from the official OrcaSlicer.
An earlier public adaptive build using the same launcher shares this settings
directory. Local development launchers can use their own `gui-data` directory.

The title bar and **Help → About** show **Adaptive Temperature B11**.
About also shows the exact source commit abbreviated to seven characters.
The regular OrcaSlicer version remains separate for compatibility with profiles.

## Functionality

- Native flow/temperature tables, smoothing, lookahead, asymmetric target rise/fall
  limits, Temperature preview and an adaptive-temperature report.
- Optional coordinated flow that can lower eligible printing feedrates while
  preserving first-layer/custom motion, travel, bridge and ironing speeds.
- **Ironing temperature offset** in the filament preset, default **+5 °C**.
  During ironing the target is the normal filament nozzle temperature plus this
  offset, bounded by the material nozzle-temperature limits. It replaces the
  low-flow table target for ironing. The existing transition limits still apply;
  there is no blocking heating wait.
- **Temperature command step** supports fractional values from **0.5 to 5 °C**.
  Its editor converts a decimal comma to a decimal point.
- Spiral vase support for adaptive temperature. VFA calibration warns about
  coordinated flow and rejects slicing while that checkbox is enabled.
- Thermal-response CSV analysis and optional supervised under-load recording.
  Calibration G-code is generated from the selected printer, filament, nozzle,
  bed and process presets. Moonraker is used only for this optional recording
  controller; offline slicing and adaptive G-code export do not require it.
- Native interlayer-adhesion cylinder calibration and the original pinned
  CrealityPrint overhang recovery filter.

See the [user guide](../adaptive-temperature.md) for setup and limits.

## Validation

The B11 feature build passed 22 adaptive planner cases / 748 assertions and
19 FFF integration/calibration cases / 319,412 assertions. The relevant changed
C++ units passed clang-tidy. The developer's user reported that B11 works.
Public packaging additionally verifies source/binary hashes, a CLI slice,
ZIP integrity and the published asset checksum.

These checks do not establish an optimum ironing offset for every material or
validate all printers. The default +5 °C is a starting value for print testing.
This is an unsigned Windows experimental build, not an official OrcaSlicer
release. Linux/macOS feature builds have not been validated.

Source and license: the release tag identifies the matching complete source;
the fork is distributed under AGPL-3.0 with the included notices and attribution.

