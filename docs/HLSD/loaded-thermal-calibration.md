# Thermal response under printing load

Calibration > Hotend thermal response generates a continuous spiral fixture using the selected printer, nozzle, filament, plate and process. The native Print pipeline slices the mesh with copies of the effective presets; it does not modify or save those presets or replace the current plate. Supported recording is one material, one physical Klipper hotend and a horizontal bed through Moonraker. File generation and CSV import do not contact a printer.

## Protocol and geometry

The dialog exposes three temperatures and three positive increasing volumetric flows. Defaults come from the filament nozzle temperature, recommended temperature range and maximum volumetric speed. An existing nondecreasing flow/temperature table additionally constrains the targets. Both temperature gaps must be at least five degrees. A high-flow stage never uses the low-temperature target.

The selected nozzle, layer height, wall width, filament diameter, flow ratio, acceleration and velocity limits determine cylinder size and source speed. Bed boundaries/exclusions and printable height are checked. The cylinder reserves deposition volume for every stage's full timeout plus margins. Tests that do not fit are rejected rather than silently shortening measurement. Geometry planning reuses the interlayer-calibration sizing constraints. The mesh is centred on the actual selected bed, including beds with offset origins.

The selected start/end and filament G-code, first-layer nozzle temperature and bed temperatures remain in the native export. Calibration replaces layer-change scripts with synchronized Klipper print statistics and one M400 at the measurement boundary. It uses one spiral wall without a solid bottom, a brim, fixed part cooling from the filament maximum fan setting, and no layer-time slowdown. Adaptive temperature, coordinated flow, recovery and unsupported external postprocessing are excluded. These temporary settings and test conditions are shown before generation.

Nine stages measure a low-flow baseline, cooling/heating at low flow, a rise to the high target, a change to reference flow, cooling/heating at reference flow, high-flow loading and unloading to reference flow. Stage duration is determined by observed settling, not a fixed dwell timer. The file has a constant nominal reference flow; the worker uses M220 to establish each requested load. Commanded extruder velocity multiplied by the generated filament cross-section qualifies that load. It does not measure melt flow or skipped steps.

## File identity and recording

The generated G-code carries a versioned JSON protocol in its first comment. A local file must be selected before recording, including when recording a previously generated file. The worker validates the protocol and hashes the complete local file, then downloads and verifies the exact uploaded bytes before any printer command. The remote filename is checked again throughout recording and cleanup. Upload and print start remain explicit actions in the printer interface. An unrelated file is not authorized by its name alone.

Generated protocols use normal Klipper print_stats layer markers and live Z, without requiring Creality virtual-SD layer extensions. Moonraker fan objects are discovered. Stock fan telemetry and Creality output_pin fan0 are supported; other generic fans and fan output pins are monitored for constancy. Missing installed telemetry, changed cooling, unexpected target, changed extrusion multiplier, paused/replaced jobs, clock reversals, sampling gaps or timeouts abort the experiment. Additional cooling channels are retained in the raw CSV; absent auxiliary/chamber channels are recorded as zero, with the discovered mapping retained in the summary.

Progress requires ten continuous seconds within one degree, a five-second time-weighted commanded flow within ten percent of the requested flow, and ongoing positive extrusion. A stationary hot nozzle cannot qualify. Each fixed-load temperature response gets its own preceding baseline and following plateau. Load transitions are distinct from temperature response fits. An interrupted load excludes that step from the fit even if it later settles.

The original supplied K2PRO/PLA fixture remains available for legacy command-line recording with its pinned checksum and fixed protocol. The new GUI generates preset-specific files instead of copying that fixture.

## Results and lifecycle

Raw CSV retains target, sensor, power, cooling, bed temperature, stage identity and commanded flow. The constant-deposition field is intentionally empty so variable commanded extrusion cannot be mistaken for measured constant deposition. JSON summaries retain the generated protocol/checksum, observed cooling baseline and stage results. Native CSV analysis remains the source for proposals; summaries are not treated as measurement evidence.

Recording uses the bundled Python runtime and standard library. CMake stages the canonical workers from scripts into resources/calibration. The GUI slices asynchronously, then launches recording with an argument vector and no shell. A heartbeat and cooperative Stop request preserve the existing worker lifecycle; the worker is not forcibly killed to stop an experiment.

On completion, Stop or failure the worker rechecks job identity, attempts speed restoration and normal cancellation, and confirms cancellation, virtual SD inactivity, heater shutdown and speed restoration separately from measurement completion. Cancellation is not retried after an HTTP timeout. A changed job is never cancelled. Connection failure can prevent confirmation; supervision and the printer interface remain necessary. Bedless printers do not require a fictitious bed-heater telemetry channel.

Sensor slopes, settling times and delays are provisional observations under the recorded conditions, not melt-temperature measurements or universal hotend models. Applying and saving printer proposals remain explicit actions. Offline native slicing and mocked API tests do not establish physical print quality on an arbitrary machine.
