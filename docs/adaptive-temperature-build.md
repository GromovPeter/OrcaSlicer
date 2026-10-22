# Building the adaptive-temperature branch

The feature code is part of the normal OrcaSlicer targets; there is no external
temperature postprocessor. The development platform is Windows x64, MSVC 2022
and Release configuration. Linux and macOS use upstream build entry points but
have not been validated for these features.

## Windows prerequisites

- Git, CMake and Perl available to the build environment.
- Visual Studio 2022 with Desktop development with C++, MSVC x64 tools and a
  compatible Windows SDK.
- An internet connection for the initial dependency download/build.
- Adequate local disk space for dependencies and the application build.

A short local checkout path such as `C:\src\OrcaSlicer` avoids the extra cost
and path handling problems of building on a network share. A first dependency
build is substantially more expensive than an incremental application build.

## Initial build

Run these commands in the repository root from a Windows command prompt:

```bat
build_win.bat --help
build_win.bat -d -s --config release --arch x64 --vs 2022 --tests
```

The script selects its build directories and prints their paths. Use those
paths for subsequent commands; do not assume the private development directory
layout is available. `--tests` enables/builds the native tests. `--run-tests`
also runs the suites selected by the upstream build script.

For an incremental build with the same toolchain and existing dependency tree:

```bat
build_win.bat -s --config release --arch x64 --vs 2022 --no-configure
```

Configuration must be rerun when CMake inputs or configuration options change.
From PowerShell, prefix script invocations with `.\`.

For a configured CMake tree, the targets used in development are:

```bat
cmake --build <build-directory> --config Release --target libslic3r_tests fff_print_tests OrcaSlicer_app_gui -- /m:6
```

Replace `<build-directory>` with the path printed by the build script. The
`/m:6` argument is specific to MSBuild; choose an appropriate parallelism level.

## Resources and the calibration worker

Keep the application's `resources` directory, DLLs and bundled Python runtime
with the executable. Launching an isolated EXE copied out of the build is not
a complete installation. Follow the upstream installation/package layout.

During CMake configuration, the three canonical Python worker files in
`scripts/` are copied to `resources/calibration/`. Those generated copies are
ignored by Git. Reconfigure after editing the canonical workers so the copies
used by the application are current. The GUI uses the bundled runtime; worker
scripts use only the Python standard library.

The prepared G-code is verified byte-for-byte against `FIXTURE_SHA256` in the
worker. Its `.gitattributes` rule disables line-ending conversion. Do not
rewrite its line endings, re-slice it or mix files from different revisions.
The offline worker tests verify the bundled fixture against the expected hash.

The optional `flow_timeline` developer executable requires `ORCA_TOOLS=ON`
at configuration time. It is not required for ordinary printing.

## Isolated application settings

Use a separate data directory when testing alongside an installed OrcaSlicer:

```bat
orca-slicer.exe --datadir "C:\orca-adaptive-data"
```

Use the executable from the complete built/installed layout. Configure or import
your printer/filament/process presets explicitly in that instance. Private
development presets, addresses and calibration recordings are not shipped.
The thermal-recording URL starts empty; enter your own Moonraker endpoint.

## Validation and packaging

Follow [the validation instructions](adaptive-temperature-validation.md) after
building. Preserve the tested revision, toolchain/configuration and any local
packaging changes when reporting a result.

Publishing source commits does not create a Windows installer. A downloadable
release, installer verification and broader platform/print testing are separate
tasks. No CI artifact or installer is claimed to have been published by this branch.
