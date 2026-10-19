# fixxy

JUCE audio plugins: **fixxy EQ**, **fixxy Comp**, **fixxy Spectral**.
Each effect is both a standalone product and a shared DSP node. The long-term goal
is a meta-plugin that builds DSP graphs from natural-language requests.
The natural-language and graph engine is not implemented yet.

## Current products

- **EQ**: 16-band parametric/dynamic EQ, matched filters, response curves and pre/post analyzers.
- **Comp**: feed-forward compressor, envelope/knee controls, level and gain-reduction meters, and a transfer curve.
- **Spectral**: Soft/Hard spectral reduction, eight weighting bands, external sidechain and key listening,
  max cut, A/B, undo/redo, file presets and a custom editor.

Spectral currently uses an N1024/H256 WOLA engine with 1024 samples of latency.
It does not provide full functional or sonic parity with soothe3, minimum phase or zero latency.
See the [progress log](docs/soothe3-parity-progress.md) for implementation scope and current
verification results, and the [Oct 19 report](docs/soothe3-oct19-release-report.md) for earlier
installation checks. Earlier results do not validate subsequent source changes.

## Local build

The current development environment is macOS with the Clang/SDK from Xcode Command Line Tools,
CMake 3.25 or later, Ninja and C++20. JUCE is pinned as a Git submodule.
The build produces AU, VST3 and Standalone targets; other platforms have not been verified.

From the repository root:

```sh
git submodule update --init --recursive
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 6
```

By default, `COPY_PLUGIN_AFTER_BUILD=TRUE` copies plugins into the user's AU/VST3 plugin folders.
This workspace uses a local development hook to redirect those copies; that hook is not checked in.
Development builds that should avoid the installation folders must specify alternative copy paths.
To build one product:

```sh
cmake --build build --target Spectral_AU Spectral_VST3 Spectral_Standalone --parallel 6
```

Standalone apps and format-specific artifacts are under `build/{EQ,Comp,Spectral}_artefacts/Release/`.

## Tests

After a full build, run the 18 console suites directly. They are not currently registered with CTest.

```sh
for suite in BandResponseTest MatchedKernelTest CompressorNodeTest DynamicEqTest \
  ParamSchemaTest CompUiMapTest SpectralNodeTest SpectralCapNodeTest \
  SpectralCapScalarTest SpectralUiMapTest SpectralProcessorTest SpectralWorkflowTest \
  SpectralFirTest SpectralFirScalarTest SpectralControlTest \
  SpectralTransitionTest SpectralTransitionScalarTest; do
  "./build/$suite" || exit 1
done
./build/AnalyzerEngineTest_artefacts/Release/AnalyzerEngineTest
```

Product verification also includes strictness 5 checks with a separately installed pluginval,
macOS `auval`, and UI inspection with a real signal in Standalone or a DAW.
Console tests do not replace these checks. Run numerical DSP prototypes in a Python virtual environment:

```sh
python3 -m venv proto/.venv
./proto/.venv/bin/python -m pip install -r proto/requirements.txt
./proto/.venv/bin/python proto/spectral_core.py
```

## Documentation

- [Architecture and history](docs/architecture.md): meta-plugin architecture, reasons for real-time constraints, DSP decisions and the early roadmap.
- [Current progress and verification](docs/soothe3-parity-progress.md): completed scope, remaining work and verification evidence.
- [AGENTS.md](AGENTS.md): agent working rules. Subdirectories have their own `AGENTS.md` for local conventions.
- [DSP/UI references](docs/): guides to EQ, compression, spectral processing and interfaces.
