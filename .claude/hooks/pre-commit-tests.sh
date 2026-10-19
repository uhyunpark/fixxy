#!/usr/bin/env bash
# PreToolUse hook: run our test suite before allowing a git commit.
# Reads the hook JSON payload from stdin; only acts on commands containing
# "git commit". Missing test binaries (e.g. no build dir yet) are skipped
# silently so a clean checkout never gets blocked.
set -u

payload="$(cat)"
command="$(printf '%s' "$payload" | jq -r '.tool_input.command // empty')"

case "$command" in
  *"git commit"*) ;;
  *) exit 0 ;;
esac

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build"

BAND_RESPONSE_TEST="$BUILD_DIR/BandResponseTest"
MATCHED_KERNEL_TEST="$BUILD_DIR/MatchedKernelTest"
# Artefact dir varies with CMAKE_BUILD_TYPE (Debug/ subdir or flat) — try both.
ANALYZER_ENGINE_TEST="$BUILD_DIR/AnalyzerEngineTest_artefacts/Debug/AnalyzerEngineTest"
[ -x "$ANALYZER_ENGINE_TEST" ] || ANALYZER_ENGINE_TEST="$BUILD_DIR/AnalyzerEngineTest_artefacts/AnalyzerEngineTest"
COMPRESSOR_NODE_TEST="$BUILD_DIR/CompressorNodeTest"
DYNAMIC_EQ_TEST="$BUILD_DIR/DynamicEqTest"
COMP_UI_MAP_TEST="$BUILD_DIR/CompUiMapTest"
SPECTRAL_NODE_TEST="$BUILD_DIR/SpectralNodeTest"
SPECTRAL_CAP_NODE_TEST="$BUILD_DIR/SpectralCapNodeTest"
SPECTRAL_CAP_SCALAR_TEST="$BUILD_DIR/SpectralCapScalarTest"
SPECTRAL_UI_MAP_TEST="$BUILD_DIR/SpectralUiMapTest"
SPECTRAL_PROCESSOR_TEST="$BUILD_DIR/SpectralProcessorTest"
SPECTRAL_WORKFLOW_TEST="$BUILD_DIR/SpectralWorkflowTest"
SPECTRAL_FIR_TEST="$BUILD_DIR/SpectralFirTest"
SPECTRAL_FIR_SCALAR_TEST="$BUILD_DIR/SpectralFirScalarTest"
SPECTRAL_CONTROL_TEST="$BUILD_DIR/SpectralControlTest"
SPECTRAL_TRANSITION_TEST="$BUILD_DIR/SpectralTransitionTest"
SPECTRAL_TRANSITION_SCALAR_TEST="$BUILD_DIR/SpectralTransitionScalarTest"

fail=0

for cap_test in "$SPECTRAL_CAP_NODE_TEST" "$SPECTRAL_CAP_SCALAR_TEST"; do
  if [ -x "$cap_test" ] && ! "$cap_test" >/dev/null 2>&1; then
    echo "pre-commit hook: $(basename "$cap_test") FAILED" >&2
    fail=1
  fi
done

if [ -x "$BAND_RESPONSE_TEST" ]; then
  if ! "$BAND_RESPONSE_TEST" >/dev/null 2>&1; then
    echo "pre-commit hook: BandResponseTest FAILED" >&2
    fail=1
  fi
fi

if [ -x "$MATCHED_KERNEL_TEST" ]; then
  if ! "$MATCHED_KERNEL_TEST" >/dev/null 2>&1; then
    echo "pre-commit hook: MatchedKernelTest FAILED" >&2
    fail=1
  fi
fi

if [ -x "$COMPRESSOR_NODE_TEST" ]; then
  if ! "$COMPRESSOR_NODE_TEST" >/dev/null 2>&1; then
    echo "pre-commit hook: CompressorNodeTest FAILED" >&2
    fail=1
  fi
fi

if [ -x "$DYNAMIC_EQ_TEST" ]; then
  if ! "$DYNAMIC_EQ_TEST" >/dev/null 2>&1; then
    echo "pre-commit hook: DynamicEqTest FAILED" >&2
    fail=1
  fi
fi

if [ -x "$COMP_UI_MAP_TEST" ]; then
  if ! "$COMP_UI_MAP_TEST" >/dev/null 2>&1; then
    echo "pre-commit hook: CompUiMapTest FAILED" >&2
    fail=1
  fi
fi

if [ -x "$SPECTRAL_NODE_TEST" ]; then
  if ! "$SPECTRAL_NODE_TEST" >/dev/null 2>&1; then
    echo "pre-commit hook: SpectralNodeTest FAILED" >&2
    fail=1
  fi
fi

if [ -x "$SPECTRAL_UI_MAP_TEST" ]; then
  if ! "$SPECTRAL_UI_MAP_TEST" >/dev/null 2>&1; then
    echo "pre-commit hook: SpectralUiMapTest FAILED" >&2
    fail=1
  fi
fi

if [ -x "$SPECTRAL_PROCESSOR_TEST" ]; then
  if ! "$SPECTRAL_PROCESSOR_TEST" >/dev/null 2>&1; then
    echo "pre-commit hook: SpectralProcessorTest FAILED" >&2
    fail=1
  fi
fi

if [ -x "$SPECTRAL_WORKFLOW_TEST" ]; then
  if ! "$SPECTRAL_WORKFLOW_TEST" >/dev/null 2>&1; then
    echo "pre-commit hook: SpectralWorkflowTest FAILED" >&2
    fail=1
  fi
fi

if [ -x "$ANALYZER_ENGINE_TEST" ]; then
  if ! "$ANALYZER_ENGINE_TEST" >/dev/null 2>&1; then
    echo "pre-commit hook: AnalyzerEngineTest FAILED" >&2
    fail=1
  fi
fi

for fir_test in "$SPECTRAL_FIR_TEST" "$SPECTRAL_FIR_SCALAR_TEST" "$SPECTRAL_CONTROL_TEST" "$SPECTRAL_TRANSITION_TEST" "$SPECTRAL_TRANSITION_SCALAR_TEST"; do
  if [ -x "$fir_test" ] && ! "$fir_test" >/dev/null 2>&1; then
    echo "pre-commit hook: $(basename "$fir_test") FAILED" >&2
    fail=1
  fi
done

if [ "$fail" -ne 0 ]; then
  exit 2
fi

exit 0
