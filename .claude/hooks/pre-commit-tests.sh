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
ANALYZER_ENGINE_TEST="$BUILD_DIR/AnalyzerEngineTest_artefacts/Debug/AnalyzerEngineTest"
COMPRESSOR_NODE_TEST="$BUILD_DIR/CompressorNodeTest"
DYNAMIC_EQ_TEST="$BUILD_DIR/DynamicEqTest"
COMP_UI_MAP_TEST="$BUILD_DIR/CompUiMapTest"
SPECTRAL_NODE_TEST="$BUILD_DIR/SpectralNodeTest"

fail=0

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

if [ -x "$ANALYZER_ENGINE_TEST" ]; then
  if ! "$ANALYZER_ENGINE_TEST" >/dev/null 2>&1; then
    echo "pre-commit hook: AnalyzerEngineTest FAILED" >&2
    fail=1
  fi
fi

if [ "$fail" -ne 0 ]; then
  exit 2
fi

exit 0
