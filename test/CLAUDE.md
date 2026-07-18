# test/CLAUDE.md

Console tests are the C++-side half of the proto -> C++ verification bridge.
JUCE-free plain C++ by default; pull in a JUCE module only when the tested unit
itself needs one.

## Adding a console test

1. Add an `add_executable` target in the root `CMakeLists.txt`, following the
   existing strict-flags pattern:
   ```
   add_executable(<Name>Test test/<name>_test.cpp)
   target_include_directories(<Name>Test PRIVATE ${CMAKE_SOURCE_DIR}/src)
   target_compile_features(<Name>Test PRIVATE cxx_std_20)
   ... COMPILE_OPTIONS "-Wall;-Wextra;-Wconversion;-Wshadow")
   ```
   If the unit needs a JUCE module (see the `AnalyzerEngineTest` pattern —
   `AnalyzerEngine` pulls in `juce_dsp`/`juce_audio_basics` for FFT), use
   `juce_add_console_app` + `target_sources` + `target_link_libraries` instead of
   a bare `add_executable`.
2. Register the new binary in `.claude/hooks/pre-commit-tests.sh` so `git commit`
   runs it automatically and blocks on failure. Follow the existing per-binary
   pattern: check the executable exists (skips silently on a clean checkout with
   no build dir), run it, flag failure.
3. If the test needs numeric fixtures, generate `test/<name>_fixtures.h` from a
   `proto/dump_*.py` script (`proto/CLAUDE.md`) — never hand-transcribe numbers
   from a proto's printed output.

## Current test roster

`BandResponseTest`, `MatchedKernelTest`, `CompressorNodeTest`, `DynamicEqTest`,
`ParamSchemaTest` — plain C++. `AnalyzerEngineTest` — JUCE console app (needs
`AnalyzerEngine`'s FFT dependency).
