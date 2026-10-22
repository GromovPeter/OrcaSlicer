# Local under-extrusion recovery

The process options msao_recovery_enable, msao_safe_accel and msao_safe_velocity
connect the original CrealityPrint v7.1.0 filter to OrcaSlicer. Defaults are off,
200 mm/s² and 50 mm/s; UI: Process → Speed → Advanced. Configuration validation
requires Klipper and excludes Extrusion rate smoothing.

The implementation comes from commit 7622ae4518f7988965a6bd32ca41ea1c59e4dec0.
Only direct includes and attribution differ in the filter implementation file.
Detection requires continuous extrusion at no more than 35 mm/s for at least
0.2 seconds, followed immediately by faster extrusion on an external or overhang
wall at the same Z. Duration is length / feedrate, not volumetric flow.

The recovery distance is 1 mm transition plus theoretical acceleration distance
from slow speed to configured safe speed plus 1 mm cruise, clipped to the
eligible path. Native commands within the span, first-layer/vase handling,
coordinate parsing, extrusion splitting and restoration follow upstream.
There are no additional Orca-specific eligibility barriers, caps or rewrites.

The filter runs after cooling rewrites the layer and before FanMover and the
final G-code processor. Object IDs provide per-object settings. Preview, time
estimation and adaptive temperature see the resulting motion. Adhesion
calibration disables recovery in its temporary process. InterestRegion retains
upstream motion detection; its unused Creality preview adapter is omitted.
Equal filter code does not make wall generation or overhang speeds identical
between two slicers. Source equivalence and slicing output are distinct checks.

## Source provenance

https://github.com/CrealityOfficial/CrealityPrint/tree/7622ae4518f7988965a6bd32ca41ea1c59e4dec0/src/libslic3r/GCode

Upstream and this fork use GNU Affero General Public License v3; see LICENSE.txt.
Immutable snapshots, hashes and source-equivalence results accompany the build.
