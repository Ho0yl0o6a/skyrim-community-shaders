# Animation continuity

## 2026-09-21 01:48 — static-probe attachment hold fixed in controlled tests

Deployed3D3E4FC73F4BA1C4528B15A846EA648C84D7814C29FCF5A26A77BCD8338CC7DE,
build95317 exit0, backup20260921-attachment-probe-exclusion preservesA97CCD03.
Includes snapshot lifetime ownership/reset correction described below.143 tests
pass. NormalPID34012 started01:44:25, runtime016AD3F5 unchanged.

Two drawn-axe third-person runs, each64frames, full audit/integrity and fixed-POV
movement guards pass; all-frame body sheets inspected:
- third-person-attachment-probe-fixed-1789951529,26911..26974
- third-person-attachment-probe-repeat-1789951585,29014..29077
Both have zero held axe/scabbard rotations and EXACT zero reconstructed-to-
submitted translation/rotation mismatch across63transitions. First run body/head
also zero holds; axe max camera-relative step2.36156 versus10.9262 in preceding
controlled failing run. Those captures are not pixel-matched/native parity.
MeasurePlayerPoseMotion now records attachment-to-submission mismatch explicitly.

Repeat exposes a separate large camera-relative jump at29070: axe world step
15.3207, camera-relative110.1183, attachment mismatch0. Body suddenly fills more
of image. Current mode remains third, native world camera valid. Do NOT claim
camera movement resolved; next investigate native camera collision/phase versus
imported view, not attachment stale submission. Other animations/attacks and
interior/travel still unqualified. CheckViewModel12samples passes. Save1 restored,
normal first person/freeCamfalse, auditfalse; inputs released. All jobs terminal.

## 2026-09-21 01:43 — controlled run isolates static-probe interference

User explicitly authorized control takeover. Snapshot lifetime fix built73943
exit0/deployedA97CCD03D7116DDB6ADAFCF11BC0497FFDDCF6C0EA111B2E64DAF64DD6602F2F,
backup20260921-viewmodel-snapshot-lifetime preservesC87FC875.142 tests pass.
Snapshots now hold NiPointer mesh references across discovery, skip removed
view-model members before dereferencing them, and clear all snapshot/audit data
on DiscardFrame. Preventive source-confirmed lifetime hazard, not a reproduced
crash. NormalPID15412 started01:39:48, runtime016AD3F5 unchanged.

Controlled drawn-weapon third-person-attachment-controlled-1789951270 has64frame
integrity/full pose coverage, movement and fixed-third-person gates pass.
Body/head rotations have zero holds; axe/scabbard hold28863. Crucially at28863
attachmentRotation CHANGES but submitted rotation/world remain exactly28862.
At28864 submission catches up. This isolates the static-probe early return:
it hashes cached geometry->world, not the reconstructed skeleton attachment.
Candidate disables that shortcut AND prevents rearming it for mapped animated
attachments. Static world geometry optimization remains.143 tests pass.
Build/deployment/after-run evidence pending; do not yet claim equipment fixed.

## 2026-09-21 01:32 — equipment candidate deployed, comparison inconclusive

HostC87FC875C8CEB88C070C25C66CF191D018E7C90B3AB0D5F1E6448D6BE7ED38B4,
build43153 exit0, backup20260921-third-person-attachment preservesB0415D2B.
Runtime016AD3F5 unchanged. NormalPID37436 started01:28:41;140 tests pass.
Native first-person pose capture preserved, playerBody rigid equipment now
uses shared bone/local-chain reconstruction. No other rendering changes.

third-person-attachment-fixed-1789950578,26307..26370, was REJECTED by movement
gate (zero horizontal displacement). Integrity and idle sheet inspected, not
locomotion evidence. Retry third-person-attachment-fixed-retry-1789950636,
28301..28364, integrity/full pose coverage but POV changes third-to-first during
capture; sheet visibly confirms transition, final manifestpovfirst. All player
parts have held poses during this mixed run; do not claim fix acceptance or
attribute those holds specifically to the equipment change. Axe anchorNPC RHand
is present, but correct sampling alone is not visual/native parity proof.

Camera continued moving afterward independently of harness input (latestfirst,
freeCamfalse); avoid fighting apparent live user control with further scripted
movement or save reloads. Harness input released/audit disabled. Game remains
running at current player location (NOT restored Save1 after this retry).
CapturePlayerMovement now persists requested/start/end camera mode and rejects
captured audit frames with wrong/missing current view-model camera coverage.
PowerShell parser check passes. Audit-off only checks endpoint camera states,
so cannot guarantee no intermediate POV switches. Need uninterrupted third-person
drawn/sheathed movement validation; attack/turning tests not performed this turn.

## 2026-09-21 — third-person rigid equipment timing

Continuation after native first-person snapshot fix is PROGRESS. NormalPID34868,
hostB0415D2B, third-person-after-native-viewmodel-1789950326,26555..26618,
64frame integrity/full pose coverage. Body/head/facial skinned meshes have ZERO
held rotations, but WarAxe:0, WarAxeBloodLighting and Scb all hold26575/26610.
Axe maximum camera-relative origin step8.74467; scabbard9.01354. Body bind-origin
steps are not actual vertex displacement. All-frame body sheet inspected.

Candidate extends rigid attachment ancestor mapping to playerBody, sampled
against its entry BoneSnapshot before discovery (new members added afterward).
Native first-person snapshot overrides remain unchanged. Only anchored rigid
player equipment uses the reconstructed transform; no mapped ancestor means
keep native world. No interpolation or guessing attachment offsets. Build/live
validation pending; do not call third-person equipment resolved yet.

## Discovery-crossing skeleton snapshot fixed — 2026-09-21 00:48

PROGRESS: identified and removed the periodic skeleton hold in tested locomotion.
Opt-in bonePhases audit compares59 shared player bone sources at Submit entry
and after discovery, with internal sceneFrame/rediscovered/timing. Intermediate
host0E301448 (build83080 exit0, backup20260921-bone-phases) normalPID51996:
.research/sequences/player-bone-phases-1789947720, frames1080..1143, integrity
pass. Exactly16 discovery frames: all59 sources changed, ~3.4–4ms elapsed;
all48 non-discovery frames: zero changed, ~.5ms. The frame immediately after
each discovery repeats body/head rotations:1083,1087,...1143. Worst body probe
jump77.220units at1122 is a discovery frame. This connects the periodic hold
to sampling across the producer update during discovery, not denoising.

SnapshotBones now captures existing captureOrder skeletons BEFORE Gather.
After discovery, only newly encountered bone pointers are added; existing
shared pointers keep their entry pose. All meshes reuse this frame-local map.
World inverse cancellation and native bind transforms unchanged. No interpolation,
pose skipping, stale prior-frame cache or frame-rate cap. This is not an atomic
whole-engine animation snapshot guarantee; morph/rigid-node timing remains open.

Build87784 exit0,135 CPU/source tests pass (ordering/shared-source guards included).
Host4506FB59A917FEFB8A92002BB23CF72ADEA425F6070D5D4EDD897A78D47EF9B7 deployed,
backup/symbols20260921-entry-bone-snapshot preserve0E301448. Runtime016AD3F5
unchanged. Normal PID49876, RR1, no NativePreparation override.
Two confirmed forward runs,64frames each, full audit coverage/integrity:
- player-entry-bones-1789947922,1089..1152
- player-entry-bones-repeat-1789947995, reload then second run
Both have ZERO repeated composed body/head rotations (<1e-5 matrix delta),
63 changed pose frames and no unsubmitted player pieces. Body bind-origin
probe maxima56.34/49.10units are not vertex displacements or acceptance gates.
First all-frame body sheet inspected, no same four-frame hold pattern.
Audit-off forward run player-entry-bones-audit-off-1789948078,7121..7184,
integrity passes and all-frame body sheet inspected. No pose proof in audit-off
run; screenshot-copy overhead remains. No display pacing/native pose parity,
all-animation, interior or first-person acceptance. Eyes remain parked by user.
Startup NRC initialization error/fallback logged again; no crash observed.
Restored existing Save1 after runs, normal third-person/freeCamfalse, audit off.

## Whole-player cadence evidence — 2026-09-21 00:37

User explicitly prioritised movement/animation judder over eyes. Added opt-in
whole-player audit (removed dynamicPositions-only filter), plus submitted
eyeWorld/cameraValid per record. Audit API scope string still says dynamic
geometry but coverage now includes static-bind skinned body/clothes as well.
No production animation change. Host C6DAB9110DD253578F60E9300071355610E93E57F6A7F9083E6A30990FEC5073,
build96927 exit0; backup20260921-full-player-audit preserves BB430485.

CapturePlayerMovement.ps1 uses mapped Forward key, bounded hold/release in
finally, audit stopped before disk encoding can roll256-frame history; captures
64 frames. It now rejects <1unit horizontal displacement after preserving the
manifest. MeasurePlayerPoseMotion.py composes world and bone matrices, reports
rotation repeats and bone-origin displacement/camera-relative displacement.
Bone origins are bind-transform probes, NOT actual vertex positions or GPU
proof. Copy-queue times are NOT engine animation update times.133 tests pass.

Normal PID53812 first .research/sequences/player-judder-normal-1789946834,
2779..2842, integrity/pose coverage pass. Head world-bone probe jumps24.493units
at2819 (34.905ms copy interval), matching a visible lurch. Facial-only old audit.
Normal PID15792 .research/sequences/player-judder-full-1789947091,
2051..2114, integrity/64 full-player pose joins pass,63 changed frames/no missing
submissions. Body AND head composed rotations repeat at2054,2058,...2114:
exactly16 every-fourth-frame repeats (max matrix delta<1e-5). At2061 body bone3
origin jumps73.868units, next2062 step.003. Individual matrices remain nearly
orthonormal (max normalized Gram error<1e-6), so no evidence for torn rotation
elements. All-frame body sheet inspected; abrupt transitions remain visible.
Do not confuse63 generic changed frames with continuous animation: root/morph
changes can hide a repeated skeleton pose.

Startup NativePreparation PID16252 (native GPU world still suppressed) tested
whether skipped render CPU work caused cadence. First
player-judder-native-preparation-1789947253 is IDLE, not locomotion: unchangedXY,
still repeated rotations every4frames. Repeat
player-judder-preparation-repeat-1789947337 has confirmed~679units XY movement,
head/body repeated rotations4304,4308,...4364 and70.276unit max body probe jump.
Thus retaining native preparation does not remove this cadence. First idle and
normal full-player capture integrity pass; repeat has full pose coverage via
motion analyzer but run AnalyzeFrameSequence for repeat before integrity claim.
No visual smoothness, display pacing or native animation parity pass.

Source lead: kSceneDiscoveryInterval=4; expensive Gather precedes BoneSnapshot
in Submit. Hypothesis: delayed bone sampling crosses an asynchronous animation
update, then next frame repeats that pose. NOT proven: need record discovery
phase/timing and sample native skeleton at stable render boundary. Do not
interpolate/hide repeats or blame denoising without this evidence. Keep earlier
shared-bone fix. Normal restored PID53332 started00:37:06, Save1 reload requested.

## Native movement reference, 2026-09-20 12:29

Repeated controlled Forward movement from the same existing Riverwood save with
native reference enabled at the main menu. Input.HoldKey(17), 500 ms lead-in,
64-frame capture, then Input.ReleaseKey(17) in finally. No saves were written.

- PID26712: frames15027–15090, 384.909 ms. Raw Screenshots folder
  `CS_2026-09-20_12-18-19_460-sequence-26712`. Too fast for a cadence comparison.
- PID44372: process-local `DXVK_FRAME_RATE=40`, frames2386–2449, 1574.405 ms.
  Raw folder `CS_2026-09-20_12-21-39_572-sequence-44372`. Parent environment restored
  in finally; no persistent frame-rate setting changed. This duration is close
  to the earlier Remix burst's1596.701 ms.

Both pass capture completeness/consecutive-frame checks. Reports and all-frame
contact sheets: `.research/testlogs/20260920-native-moving-sequence.{json,png}`
and `20260920-native40-moving-sequence.{json,png}`, with matching archived CS logs.
Both sheets inspected. Broad posed-body transition resembles the Remix burst;
no obvious T-pose in these samples. This is NOT exact pose-by-pose registration:
lead-in camera state, pose phase, time and lighting are not identical. It does
not certify animation continuity, first-person behaviour or presentation pacing.

Both native processes exited normally via verified Console+qqq. PID40220 started
12:27:08 with normal LaunchTest/ConfigureRunningTest, no frame cap/native reference/
SkinProbe overrides, and loaded the same save. Capture
`.research/captures/122813-native-comparison-restored-remix.png` inspected: world,
posed third-person character and HUD visible, freecamfalse. At12:28:24 native
14draw/75compute suppressed. No production code or binary changes this step.

## Consecutive rendered-frame capture, 2026-09-20 12:14

Added `communityshaders.capture {kind:"sequence",frames:2..64}` for opt-in Remix
test processes. It queues one full, uncropped SDR kFRAMEBUFFER copy per host frame
at the beginning of the Present hook, before State::Reset. PNG encoding/mapping
is deferred until the burst's copies have been queued. Nominal staging pixel
storage is capped at 512 MiB; allocation overhead is additional. Requests while
busy are rejected. Clipboard and HUD notifications are suppressed for sequences.
Each image has a JSON sidecar with producer frame, sequence index/count, process,
monotonic CPU copy-queue timestamp, format/dimensions and save result. Missing or
failed copies produce an incomplete sequence, not a passing report. No permanent
setting changes and no automatic captures on normal launch.

This is a RENDERED framebuffer sequence, not a scanout/WSI capture or unperturbed
performance benchmark. GPU copies cost bandwidth; texture creation costs CPU;
the existing screenshot worker can block on mapping after the burst. The hook
does not observe compositor/monitor timing, and distinct pixel hashes do not
prove animation updated (sampling noise also changes pixels).

Initial deployed capture candidate rejected requests because `State::inWorld`
only holds during the world-render pass, not at Present/main-thread requests.
The final gate uses loaded player/cell and non-main/loading-menu state, rejecting
HDR. No accepted frames came from the faulty initial gate.

Final plugin SHA256:
`9F6E970792C9F3584D19A4C0658B0BA39EDB2A2BC384B3AEB8FD15F172799710`.
BuildDev sessions44448/14373 exit0; backup/symbol label20260920-frame-sequence-world.
Runtime unchanged from the GPU probe below. All tests used PID44588 started12:05:58
with normal LaunchTest/ConfigureRunningTest (SkinProbe off, normal quality).

Actual raw sequences under the game's Screenshots folder:

- `CS_2026-09-20_12-06-42_453-sequence-44588`:32frames2144–2175,0.774seconds.
  Third-person stationary character. A second overlapping request was rejected.
- `CS_2026-09-20_12-07-21_273-sequence-44588`:64frames3700–3763,1.653seconds.
  Desktop C/W key actions did not produce measurable locomotion; this is another
  stationary-animation sample, NOT a movement pass.
- `CS_2026-09-20_12-09-29_866-sequence-44588`:64frames8472–8535,1.597seconds.
  Live Papyrus Input.GetMappedKey("Forward",0) returned17. HoldKey17, setPovthird,
  capture, wait3seconds, ReleaseKey17 in finally produced actual forward motion.
  Camera after movement(13738.94,-47563.80,-212.79),third/freecamfalse, compared
  with earlier third(13689.90,-48295.25,-170.84). Immediate pre-test POV was vanity,
  so the before/after vanity-camera vector is NOT a locomotion-distance metric.
  Input.IsKeyPressed17 was verifiedfalse. No save was written; existing save
  Save1_2B23D269_0_73647364_Tamriel_000002_20260920044800_1_1 reloaded afterward.

AnalyzeFrameSequence validates complete/consecutive indices, frame IDs, increasing
timestamps, stable process/format/dimensions, successful saves and actual images.
Reports/contact sheets in .research/testlogs:
`20260920-third-idle-sequence`, `20260920-third-sequence`,
`20260920-third-moving-sequence` (.json/.png). All three pass capture integrity.
This field is deliberately not called an animation pass. The moving sample has
64distinctpixelhashes. Inspected all-frame contact sheet and full frames8512–8514:
posed body throughout, no obvious T-pose swap, but significant body changes and
remaining angular skin shading. No smoothness/native-fidelity acceptance.

MeasureSequenceMotion performs integer phase correlation on a manually chosen
patch. Moving distant cliff patch(1050,300,400,350) gave dx0..1/dy-2..0, peaks
.470..924 (`20260920-third-moving-patch.json`). It does not establish exact camera
motion, subpixel stability, all-scene continuity or correct temporal reconstruction.
RGB MAD peaks26.66 in the full moving image, including body/vegetation/parallax;
do not attribute that magnitude to jitter without isolating the cause.

43Python tests pass, including malformed/missing sequence rejection and signed
synthetic translation tests. Source diff checks pass. Full CS log archived as
20260920-frame-sequence.log. Next: repeat the controlled Forward sequence with
native reference and compare body motion/shape, and independently assess actual
presentation pacing. Goal remains active; no production animation fix this turn.

## Bounded GPU skinning readback, 2026-09-20

Animation jitter is still open. User-accepted water, grass normals and anisotropic
filtering are out of scope for this work.

Source inspection found changed host bone palettes enter `changedInstances`,
increment the retained draw revision, and resubmit through the runtime. The
geometry cache uses bone hashes to select deformation/BVH updates. This is not
proof that native pose sampling or presentation timing is correct.

`LaunchTest.ps1 -SkinProbe` enables an opt-in diagnostic in `RtxGeometryUtils`.
It selects the first indexed, 24-bone model-space/tangent-basis geometry and
keeps its input position hash. For twelve samples it reads back 32 vertices,
including source positions, three normal-basis columns, weights and indices,
plus the GPU-skinned positions and basis. CPU evaluation uses the captured
palette and the shared skinning function. The selection is not a proven player
identity. Both model-space maps and native tangent frames use this basis path.

Readbacks are recorded after `csRemixRender` flushes uploads and the separate
skinning command list, before tracing. Completion is polled without waiting on
the GPU; the next sample starts only after completion. Consequently samples
skip frames and cannot qualify all-frame continuity. The probe releases its
readback after twelve samples; ordinary launches leave it disabled.

The first diagnostic failed: its span omitted `offsetFromSlice()`, so normal
readbacks began at the position attribute. Archived failing evidence is
`.research/testlogs/20260920-skin-probe-initial.log`. This was a diagnostic bug,
not evidence of a shader defect. Corrected spans use both the underlying slice
offset and the attribute offset. Validation thresholds were not relaxed.

Corrected runtime D3D11 SHA256:
`46A77FF560575E8CCC2DE091C2A20456DA6984076C3BB0726B438B761E9378B5`.
BuildRuntime sessions 8954 and 83791 completed with exit 0. Deployment backups
and symbols are under `20260920-skinning-probe-offset`.

Live PID53360, existing Riverwood save, runtime frames819–878:

- Twelve samples, 32 vertices each, twelve different bone hashes.
- Maximum position error 0.0000152588 game units.
- Maximum basis-component error 0.000000119209.
- Maximum sampled component movement 0.398106 game units; no non-finite values.
- `CheckSkinProbe.py` passed. Full log and JSON report:
  `.research/testlogs/20260920-skin-probe-offset.{log,json}`.
- Inspected capture `.research/captures/115040-skin-probe-offset.png` shows
  rendered world and HUD; camera reports first person, free camera off. It does
  not show the selected skinned mesh and is not a temporal animation test.

This only establishes GPU output agrees with CPU evaluation of submitted data
for those samples. It does not independently validate native bone poses, actual
BLAS contents, first-person projection, temporal history or displayed pacing.
Next investigation should correlate native pose/camera timing and presentation,
or inspect geometry as consumed by tracing; do not label jitter fixed from this
readback. No production deformation behavior was changed by the diagnostic.
