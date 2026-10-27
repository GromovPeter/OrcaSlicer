# Experimental adaptive-temperature fork

This fork adds flow-dependent nozzle temperature, optional coordinated feedrate
planning, calibration tools and Creality's overhang recovery to OrcaSlicer.
It is an experimental development branch, not an official OrcaSlicer release.
The development base is `5a36120c588100e6bff01fa595e18de83ca67af2`.

The supported adaptive-temperature workflow is one material, one physical
Klipper hotend and layer-by-layer printing, including spiral vase mode. Unsupported combinations
are rejected by validation. Defaults leave adaptation, coordinated flow and
overhang recovery disabled. Existing profiles do not need these features.

## Downloads

Ready-to-run builds and release notes are available on the
[GitHub Releases page](https://github.com/GromovPeter/OrcaSlicer/releases).
Download the Windows ZIP, extract the entire archive and run
**Launch-Adaptive-Orca.cmd**.

## Features

- A native flow/temperature table in the filament preset.
- A material-specific ironing temperature offset, with ironing speed preserved.
- Time-based flow smoothing, asymmetric target rise/fall limits and lookahead.
- Protection of the first layer and explicit custom G-code.
- Optional feedrate reduction when planned thermal transitions need more time.
- A native preview and an adaptive-temperature report for the current slice.
- Thermal-response CSV analysis with explicit application to a printer preset.
- A preset-specific under-load measurement workflow through Moonraker.
- A native spiral-cylinder interlayer adhesion calibration with a height map.
- The original CrealityPrint v7.1.0 under-extrusion recovery filter.

## Start with temperature-only planning

1. Select your printer, filament and process. Keep a copy of the original presets.
2. Open **Printer > Basic information > Adaptive nozzle temperature** and review
   the printer's target rise/fall references and flow smoothing period.
3. Open **Filament > Filament > Adaptive nozzle temperature**. Enter a table
   established for your own material and flow range, then enable adaptation.
4. Leave **Coordinate flow with nozzle temperature** disabled for the first
   comparison. Slice and inspect **Temperature** in Preview.
5. Open **View > Adaptive temperature report** to review constrained transitions.
   Re-slice after changing settings; the report describes the latest slice.

The first-layer nozzle target remains the starting target. While adaptation is
enabled, the ordinary **Other layers** temperature is used only as the ironing
reference: the second layer does not jump to that value before following the
adaptive schedule. Explicit
heater commands in protected custom G-code retain their own meaning.

### Flow/temperature table

Enter one `flow, temperature` pair per row, with a decimal point for fractional
numbers. Units are mm³/s and °C. For example, the syntax is:

```text
1,197
10,213
15,218
20,225
```

These numbers illustrate the format; they are not a supplied material profile.
Flow coordinates must increase. Temperatures must be within the filament's
configured recommended range. Intermediate points are linearly interpolated:
in the example, 5 mm³/s gives approximately 204.1 °C. Values outside the table
use the nearest endpoint rather than extrapolating an unbounded temperature.
Coordinated flow additionally requires temperatures to increase strictly with flow.
The current editor is multiline text, not a dedicated row editor.

### Ironing temperature

When adaptive nozzle temperature is enabled, ironing uses the normal filament
nozzle temperature plus **Ironing temperature offset** (Filament > Adaptive
nozzle temperature, default +5 °C), clamped to the filament nozzle temperature
limits. It does not use the flow/temperature table for its target.

The existing lookahead, rise/fall limits, command step and spacing still apply,
so short passes may not reach the requested target. No blocking heat waits are
added. Coordinated flow preserves ironing speed and excludes ironing from its
neighbouring flow reference. Ordinary flow-based temperature control resumes
after ironing. The first layer and custom G-code remain protected. The offset
is material-specific and needs print testing; +5 °C is a starting value.

### Printer settings

| Setting | Units | Purpose |
| --- | --- | --- |
| Target rise time | s/°C | Minimum time per degree of increasing target |
| Target fall time | s/°C | Minimum time per degree of decreasing target |
| Flow smoothing period | s | Suppress short demand changes between features |
| Minimum coordinated print speed | mm/s | Floor for additional thermal slowdown |
| Additional temperature lead time | s | Extra lookahead allowance |
| Minimum temperature command interval | s | Limit adaptive command frequency |
| Temperature command step | °C | Target change size, 0.5 to 5; decimal commas are accepted |

Rise/fall settings limit commanded targets. They are not a live measurement of
heater performance or melt temperature. Flow smoothing does not set motion
acceleration. A temperature preview shows commanded setpoints, not sensor data.

## Optional coordinated flow

Enable **Coordinate flow with nozzle temperature** in the filament's advanced
settings after reviewing temperature-only output. This mode can lower source
printing feedrates to make thermal transitions more gradual. It does not raise
feedrates and retains protected first-layer/custom motion, travel, bridge and
ironing speeds. Existing slow moves and machine, cooling and flow limits remain relevant.
The planner works across layer boundaries rather than restarting on every layer.

Additional slowdown can increase print time. Some transitions remain constrained
by protected motion, bridge speeds, command boundaries or the minimum speed.
The warning and report identify these limits. They do not establish that actual
extrusion is defective, or that the nozzle reached the requested temperature.
Inspect Speed, Flow and Temperature together when evaluating the result.

VFA calibration supports adaptive temperature, but requires **Coordinate flow with
nozzle temperature** to be disabled. The VFA dialog displays this requirement;
validation and export reject the test while coordination is enabled because it
can change the test speeds.

## Thermal-response calibration

Open **Calibration > Hotend thermal response**. Importing an existing CSV and
reviewing its analysis does not control the printer. The analysis displays
accepted/excluded transitions, load/fan context and provisional response values.
**Apply to printer settings**, **Save printer preset** and **Undo application**
are explicit actions; importing a CSV alone does not alter a saved preset.

The under-load recording workflow is a separate action within that window:

1. Select one filament slot and the intended Klipper printer, nozzle, plate and process.
2. Review the proposed low/reference/high temperatures and flows. Defaults use the
   filament temperature range, nozzle temperature and maximum volumetric speed;
   an existing flow/temperature table also constrains the targets.
3. Choose **Generate from current presets** to slice a continuous spiral cylinder.
   Its start/end G-code and bed temperatures come from the selected presets.
   Dimensions account for the bed, nozzle, filament diameter and machine limits.
4. Transfer and start the exact file yourself. The dialog shows the file name;
   enter its actual remote path and your Moonraker URL.
5. Confirm the file and conditions, then choose **Start recording**. A previously
   generated file can be selected using its local copy.
6. Analyze the CSV and review proposals before applying or saving printer settings.

The calibration file uses fixed part cooling from the filament maximum fan setting,
replaces layer-change scripts with Klipper statistics, and disables layer-time
slowdown, adaptive temperature/flow and overhang recovery. Selected presets and
current plate contents are not modified. Generation and CSV import do not send
printer commands. Unsupported hardware or tests that do not fit fail explicitly.

Recording verifies the generated protocol and complete uploaded-file checksum.
It discovers stock Klipper fan telemetry or Creality fan pins and monitors cooling
conditions. It changes the verified job's temperature and speed, and attempts
cancellation and speed restoration on completion, Stop or failure. Supervise the
experiment; network failure can prevent confirmation. Authentication can use
`MOONRAKER_API_KEY` from the environment. The recorder does not upload or start a
print. Sensor response is not melt-temperature measurement.

The separate `scripts/record_hotend_response.py` helper only performs read-only
Moonraker queries. It is different from the controlling under-load worker.
Both use the Python standard library. See [thermal-response design](HLSD/hotend-thermal-response.md)
and [loaded-calibration design](HLSD/loaded-thermal-calibration.md).

## Interlayer adhesion / temperature

Open **Calibration > Interlayer adhesion / temperature** with the intended
printer, filament and plate selected. Enter wall width, layer height, target
volumetric flow, start/end temperatures and temperature step. The tool creates
a normal spiral-cylinder project; use the ordinary Slice plate and Print plate
workflow. One cylinder tests one nominal flow while temperature descends.

Diameter and step heights account for the profile's conditions and response
references. The previewed size/time must fit the printer and requested flow.
Fan settings come from the filament. For high flow, the advanced **Disable
layer-time slowdown for this calibration** option relaxes only that cooling
slowdown in the temporary calibration settings; it changes specimen cooling
conditions and does not bypass the other feasibility limits.

Save the generated 3MF. After peeling the cylinder, reopen it and enter the
remaining height measured from the build plate. The stored map distinguishes
temperature transitions from stable sections and allows an explicitly selected
stable point to be transferred to the filament table. A transition height does
not identify a precise stable test temperature. See [the calibration design](HLSD/interlayer-adhesion-calibration.md).

## Recovery after overhangs

Find **Under-extrusion recovery after overhangs** in **Process > Speed > Advanced**.
The settings are **Safe acceleration after overhang** and **Safe speed after
overhang**, defaulting to 200 mm/s² and 50 mm/s. This Klipper option conflicts
with Extrusion rate smoothing; enable only one of them.

The filter is taken from CrealityPrint v7.1.0 without local changes to its
executable logic. Continuous extrusion at no more than 35 mm/s for at least
0.2 seconds, followed by faster eligible external/overhang-wall extrusion at
the same Z, triggers its original recovery plan. It uses speed and duration,
not a volumetric-flow threshold. Whole slicer output can still differ because
the two slicers generate different walls and overhang speeds.
See [source attribution](adaptive-temperature-sources.md) and [the filter design](HLSD/overhang-recovery.md).

## Limits and validation status

Windows x64 Release is the development platform. Linux/macOS feature builds
have not been validated. Adaptive planning does not currently provide general
multi-material, multiple-heater, or sequential printing support; consult
validation errors instead of assuming an unsupported project is covered.
The separate adhesion calibration intentionally uses its own vase workflow
with normal temperature adaptation disabled.

Offline tests cover the planners, export integration, calibration analysis and
mocked recorder lifecycle. Development printing on K2PRO has produced usable
results, but there is no broad comparative print study or universal material
profile. Sensor response is not melt-temperature measurement. The public
privacy-cleaned fixture has not been physically reprinted as part of publication
preparation. Packaging this branch as a public installer is a separate step.

See [build instructions](adaptive-temperature-build.md),
[validation instructions](adaptive-temperature-validation.md), and the
[high-level designs](HLSD/thermal-flow-planning.md).

<img width="1187" height="664" alt="image" src="https://github.com/user-attachments/assets/ea874b89-8666-4083-b25d-02a7100e62f8" />
