# Oct 19, 2026 release verification

Installed all six validated bundles after root's visual approval: `fixxy EQ`,
`fixxy Comp`, and `fixxy Spectral`, each as AU and VST3, under
`/Users/uhyun/Library/Audio/Plug-Ins/{Components,VST3}`.

## Verification

- Existing Release Ninja cache: combined Spectral AU/VST3/Standalone plus 18
  console targets built successfully with no compiler warnings. The final meter
  UTF-8 correction received an additional warning-free product/affected-test build.
- 18/18 console suites passed. The two affected suites were rerun after the meter
  correction; the final Workflow suite passed 424 assertions. The other 16 results
  remain applicable because the final correction changed meter text only.
- Final staged Spectral VST3: pluginval strictness 5 `SUCCESS`.
- EQ/Comp's six format targets were current: only manifest-helper no-op checks,
  with no compilation or relinking. Their earlier branding pluginval passes were
  retained; they were not represented as fresh Oct 19 validations.
- Native real-main/key-signal snapshots passed. Root inspected minimum/collapsed
  sidechain views and the actual final standalone app, including corrected Unicode,
  Hard-mode controls, and Undo back to a clean Soft baseline.
- Actual staged VST3 Soft/Hard renders, with latency trimmed, exactly match the
  preserved `hard-after-soft.wav` / `hard-after-hard.wav` baselines. Both are
  44,100 Hz, 882,000 stereo float32 frames; maximum sample error is zero and
  `numpy.array_equal` is true.

## Installation and recovery

The exact six legacy `LpngEQ`, `LpngComp`, and `LpngSpectral` AU/VST3 bundles were
moved, without deletion, to:

`/Users/uhyun/Library/Application Support/fixxy/oct19-backup.RdSuzq`

The backup has separate `Components` and `VST3` subfolders. Recovery consists of
moving the corresponding new installed bundles out of the scanner folders and
moving these exact backup bundles back; do not overwrite a later installation.

Before installation, all six new bundles were copied to the unique off-scanner
directory `/Users/uhyun/Library/Application Support/fixxy/oct19-install-stage.a9tUpg`,
verified with strict deep codesign and full recursive file comparison against
build staging, and then moved into the scanner folders. Installed bundles passed
the same checks; all six installed executable SHA-256 hashes equal build staging.

Bundle IDs `com.uhyun.{EQ,Comp,Spectral}`, AU type/manufacturer `aufx`/`Uhyn`,
AU subtypes `Lpeq`/`Lpcm`/`Lpsp`, AU factory symbols, and both VST3 class IDs per
product match the old bundles. Display names/vendor strings are now fixxy.

## Installed AU validation

All three final installed AU validations exited zero and reported manufacturer
`fixxy`, the matching `fixxy EQ` / `fixxy Comp` / `fixxy Spectral` name, and
`AU VALIDATION SUCCEEDED`.

The first EQ attempt hit the old cached `uhyun: LpngEQ` identity and failed to
open; this is preserved as stale-registry evidence, not a current-code failure or
success. The registry refreshed automatically. Read-only `auval -l` then reported
all three exact installed fixxy file URLs, and the EQ retry validated fixxy EQ.
No registration helper restart or cache clearing was needed.
The read-only `auval -al` inventory also completed naturally with all three
fixxy file URLs; no process signals were sent.

## Evidence

All logs are under `build/validation/`:

- `oct19-final-build.log`, `oct19-final-post-meter-build.log`,
  `oct19-final-eq-comp-build.log`.
- `oct19-final-<TestTarget>.log` for the 18 suites; final affected results are
  `oct19-final-post-meter-{SpectralProcessorTest,SpectralWorkflowTest}.log`.
- `oct19-final-post-meter-pluginval.log` and
  `oct19-final-post-meter-snapshots.log`.
- `oct19-final-render-{soft,hard}.log`,
  `oct19-final-preservation-comparison.log`, and `oct19-final-install.log`.
- `oct19-final-auval-{EQ,Comp,Spectral}.log`, final EQ result
  `oct19-final-auval-EQ-refreshed.log`, `oct19-final-auval-list.log`, and
  `oct19-final-auval-registry.log`.

Native snapshots and root's actual-standalone capture are under
`build/parity/fixxy-oct19-final/`, including `spectral-sidechain-minimum.png`,
`spectral-sidechain-collapsed.png`, and `standalone-final.jpeg`.

The render comparison proves SC-Off/MaxCut-Off legacy preservation, not parity
with original soothe. Native signal captures are not a DAW listening test.
Existing DAW sessions were not restarted or tested. No commits were made, no
unrelated installed bundles were changed, and the development CMake staging
redirect remains in place.
