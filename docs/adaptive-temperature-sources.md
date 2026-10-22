# Source attribution

This branch builds on OrcaSlicer at development base
`5a36120c588100e6bff01fa595e18de83ca67af2`. The repository's existing license
and upstream authorship notices are retained. This fork does not claim authorship
of OrcaSlicer or of the Creality recovery algorithm.

## CrealityPrint recovery

- Repository: https://github.com/CrealityOfficial/CrealityPrint
- Tag: `v7.1.0`
- Immutable revision: `7622ae4518f7988965a6bd32ca41ea1c59e4dec0`
- Source directory: `src/libslic3r/GCode/`
- Files: `AppearanceUnderExtrusionAccelRecoveryFilter.cpp`, its header, and
  `InterestRegion.hpp`.
- License: GNU Affero General Public License v3; the original license text is
  retained with the snapshot, in addition to the repository's `LICENSE.txt`.

The immutable reference files, source hashes and license are in
[`third_party/crealityprint-recovery`](../third_party/crealityprint-recovery/).
They are reference material, not separate compilation units.

The recovery filter's executable logic is unchanged. Direct includes and
attribution are added for Orca's build. The header uses Orca's include convention.
The motion-only InterestRegion adapter retains original detection, while the
unused Creality preview adapter is omitted because its renderer types are not
available in this project. Orca supplies pipeline integration, configuration/UI
and object identifiers. Run `python scripts/check_creality_recovery.py` to
verify the immutable reference hashes and executable-code equivalence.

Existing native Orca generation/cooling behavior is not replaced by the
Creality wall-generation or overhang-speed implementations. Matching recovery
code therefore does not promise matching full slicer output or add Creality-only
process settings.

## Other material

The native adaptive planners, reports and calibration integration are maintained
as this fork's changes to OrcaSlicer. The optional Python flow prototype is an
offline reference, not an imported external postprocessor or printer controller.
The numerical thermal test fixture retains no printer endpoint or credentials.
The prepared public K2PRO/PLA G-code omits private preset metadata and individual
Z-offset tuning; see the user guide for its fixed operating assumptions.

Additional references discussed during development do not imply that their code
was incorporated. The branch does not require SB53 or another external
temperature-processing script.
