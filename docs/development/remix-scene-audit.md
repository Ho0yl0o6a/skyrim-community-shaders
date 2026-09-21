# Per-world-frame scene audit

The settled-cell membership script cannot catch one-frame exterior geometry.
Opt-in `cs.sceneAudit=true` resets/enables a bounded audit at every Scene-mode
BeforeUI submission. False freezes the evidence without clearing it; ordinary
play pays only a disabled atomic load. Main Menu, Loading Menu and
native-reference frames are excluded; Console and Fader Menu may be included.
The audit does not change rendering or membership policy.

`communityshaders.inspect` kind `remixScene`, filter `audit` returns cumulative
frame/interior/failure/suspect counts, cell-change count, per-class interior and
exterior maxima, accepted dome-call counts, the first sample, the first suspect
and the most recent 256 samples. A single suspect survives history rollover.
Missing-current-camera and camera-ready failures are counted separately, with
the first failed attempt preserved and the native Fader Menu state recorded.
Ready-but-empty scenes have a separate counter. A failure during a fade with no
native world-camera call is not by itself proof of a visible dropped frame.
Frame IDs use CS's present counter, not the scene discovery cadence. Reset is
explicit; loading a cell does not erase accumulated evidence.

Classes are sky shader property, distant-tree shader property, terrain LOD,
object LOD (feature or flag), cloud feature, noisy terrain LOD and submitted
geometry missing from host membership. Neither an ancestor called ObjectLODRoot
nor an effect texture named Cloud is a classifier. An interior dome or one of
these classes is a suspect, not universally an error: some authored interiors
intentionally show sky. Analyze the actual cell before changing policy.

Use `tools/remix/CheckSceneAudit.ps1 -Start`, exercise transitions, then
`-Freeze -MinimumCellChanges 4 -RequireExteriorControls`. This requires outdoor
geometry/dome positive controls and at least 60 interior submissions. Unit test
`CheckSceneAudit.cpp` checks a single-frame defect, rollover, counters and reset.

This is host-side evidence, not proof of runtime-retained TLAS contents or final
pixels. In particular, an accepted dome API call does not prove GPU activation,
and `visible` records host instances rather than independent GPU readback. If a
visual leak occurs with clean host evidence, inspect runtime retirement, dome
activation, composition and temporal history next. Do not claim whole-goal or
all-scenario correctness from this audit.

## 2026-09-20 live evidence

Final plugin `20260920-scene-audit-context` ran as PID48820. Eight console
transitions across Riverwood, Sleeping Giant Inn and Riverwood Trader produced
2,007 attempts, 1,218 interior attempts, zero suspect membership/dome samples,
zero camera-ready failures and zero ready-but-empty scenes. All 24 not-ready
attempts lacked a current native world-camera capture. First failure had Fader
Menu open and zero submitted geometry. Only the first failure's fade state is
preserved; do not extrapolate that flag to every failed attempt.

Three native door activations produced1,106 attempts/768 interior attempts and
the same zero-suspect/camera-ready-failure/empty-ready result. Nine attempts had
no current camera; first failure was under Fader Menu. Outdoor positive controls
observed up to386 distant-tree,130 object-LOD and73 noisy-terrain-LOD host records,
with335 accepted dome calls. This verifies those classifier paths, not every
class (sky geometry and cloud feature were absent outdoors too).

Full reports: `.research/testlogs/20260920-scene-audit-context.json` and
`20260920-scene-audit-doors.json`. The strict checker intentionally returns
failure for any not-ready attempt; neither report is an overall passing audit.
The separate settled-cell roundtrip passes. No exterior leak was reproduced,
and no rendering-policy fix was justified by this evidence alone.

Stationary inn control subsequently passed11,241 consecutive CS frames
14,298..25,538 with zero suspect, missing-camera, not-ready or empty-ready
samples. Saved as `20260920-scene-audit-stationary.json`; audit frozen/disabled
after collection. This validates host submission continuity for that interval,
not final pixels or animated-object correctness.
