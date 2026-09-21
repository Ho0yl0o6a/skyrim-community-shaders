# Remix integration — handover, 2026-09-20

## Checkpoint 2026-09-21 — frame generation: rate solved, pacing NOT solved

Goal was "100fps at 1080p with correct frame-generation pacing and Reflex".
**Frame rate: met. Pacing: NOT met. Reflex: NOT met.** Read this before touching
the DLFG present path. Full measurements are in `remix-performance.md`; that file
also records six conclusions that were reached and later disproven, so read it
before forming a theory.

### What the rate was, and what unlocked it

| config | rendered | presented |
| --- | --- | --- |
| session start (default) | 26.6 | 53.2 |
| `CS_REMIX_NO_OOB_MARKERS`, Auto quality | 53.5 | **107.0** |
| + DLSS UltraPerformance, `-TlasRefit` | 74.7 | **149.4** |

The wall was not the GPU. An elevated nsys capture (`--trace=vulkan`; see the
`nsys-capture-workflow` memory — adding `wddm` hangs report generation and loses
the capture) showed the `dxvk-dlfg-present` thread spending **93% of wall time in
`DelayExecution`** — a timed sleep — *inside* `vkQueuePresentKHR`: ~18.6 ms per
present, two serial presents per job, which is exactly the frame period. The
present ended a median **9.15 ms after GPU work had finished**, with the GPU 43%
idle. `vkAcquireNextImageKHR` was 3.8 us and `vkQueueSubmit` 31 us, which rules
out the swapchain and queue contention. The acquire counters showed `ring 5, in
flight 5, blocked 100% of acquires` — the renderer was entirely gated by that
sleep.

The driver applies that sleep because Remix tells it these are frame-generation
presents, through the Reflex **out-of-band markers**:
`VK_OUT_OF_BAND_PRESENT_START/END` and `VK_OUT_OF_BAND_RENDERSUBMIT_START/END` in
`rtx_reflex.cpp`, plus `NvLL_VK_NotifyOutOfBandQueue(...PRESENT)` on the DLFG
queue. Gating all five (`CS_REMIX_NO_OOB_MARKERS`) removes the sleep and doubles
throughput.

**Correction to earlier work: `rtx.reflexMode=0` does not disable those markers.**
It only changes the sleep mode passed to `NvLL_VK_SetSleepMode`; `setMarker` fires
whenever Reflex *initialised*, which happens at device creation regardless of
mode. Every measurement in these docs labelled "Reflex off" was mislabelled and
did not test what it claimed to test.

### Why this is not done: the pacing is measurably bad

The user reported it "does not feel smooth when moving". That is correct, and it
reproduces objectively. PresentMon (`tools/pacing/`) attached to the user's own
play session — markers off, 95.8 presented — over 1913 presents:

```
MsBetweenDisplayChange  mean 10.51 ms  sd 4.76 ms
|dev| <0.5ms 11.2% | 0.5-1 11.4% | 1-2 23.8% | 2-4 26.3% | 4-10 22.4% | >10 4.8%
pairs with one frame bunched (<40% of the pair): 37.1%
MsInPresentAPI: mean 0.151 ms   (the sleep is gone, as intended)
```

Interpolation still runs (rendered 47.9 -> presented 95.8, ~2x), but with the
markers gone the driver no longer spaces the generated frame, so it lands next to
its real frame instead of halfway between: **37% of frame pairs are bunched.**
That is the judder the user is describing.

This capture is trustworthy, unlike the DLSS-G figures in
`tools/pacing/README.md`. That file's warning — PresentMon cannot resolve a
software-metered DLSS-G pair — applies when metering is active. With the markers
off there is no metering to resolve, so the display-change timestamps mean what
they say.

The current state is therefore a straight trade, not a fix: **markers on =
correctly paced but locked to half rate; markers off = full rate but unpaced.**

### The single most valuable untested experiment

`d3d11_swapchain.cpp:23`:

```cpp
static const bool kD3D11ReflexSimulation = std::getenv("CS_REMIX_D3D11_REFLEX_SIM") != nullptr;
```

**The D3D11 in-band Reflex SIMULATION markers, the latency ping and
`reflex.sleep()` are opt-in and default OFF.** This gate was added by an earlier
session, not this one, and its own comment already states the hypothesis: a closed
loop that settles at half the achievable rate. It appears never to have been
measured — no run recorded anywhere in these docs sets that variable.

The mechanism this predicts: without `SIMULATION_START/END` the driver has no
independent measure of the render cadence, so its frame-generation pacing can only
reference the cadence it is itself producing. That is self-referential and stable
at any rate, which is exactly the bistable lock-in observed — and it also explains
why removing the out-of-band markers "fixes" the rate while destroying the pacing.

**The untested combination is out-of-band markers ON *and*
`CS_REMIX_D3D11_REFLEX_SIM=1`.** That is the only configuration in which the
driver has both a frame-generation present to pace and a real simulation interval
to pace it against. Run it with `LaunchTest.ps1 -ReflexSim` (and *without*
`-NoOobMarkers`), and judge it on `MsBetweenDisplayChange` and the bunched-pair
percentage above, not on frame rate alone.

Two cautions. `reflex.sleep()` is what enforces Reflex low-latency mode and
currently measures ~8.3 ms per frame on the game thread, so expect the rendered
rate to *drop* — that is the pacing doing its job, and the run must be judged on
cadence, not throughput. Separately, the frame-latency wait (`SyncFrameLatency`)
sits after `beginSimulation` on this path, so a GPU-pacing block lands inside the
simulation region; if the measured sim interval looks wrong, that ordering is the
first suspect. Both points come from an analysis pass whose verification agents
died on a usage limit, so treat them as leads, not findings.

Verified directly: `sleepParams.minimumIntervalUs = 0` in `RtxReflex::updateMode`,
so Reflex's own frame limiter is never in play at any refresh rate.

### Landed this session (all env-gated, all default-off)

- `CS_REMIX_NO_OOB_MARKERS` (`rtx_reflex.cpp`) — gates the five out-of-band call
  sites. Doubles throughput, destroys pacing. Opt-in deliberately.
- `CS_REMIX_EARLY_BB_RELEASE` (`rtx_dlfg.cpp`) — a real defect, independent of the
  above. `m_backbufferInFlight[i]` was released only in the job's exit guard,
  after both presents, but the backbuffer's last use is the blit into the
  swapchain image, and that blit already signals `m_backbufferAcquireSemaphores[i]`
  — the exact semaphore the next writer of that backbuffer waits on. The CPU flag
  was a second, ~34 ms more conservative gate on top of a GPU ordering guarantee
  that already existed. Worth ~+1.4 presented on its own (inside noise, since in
  steady state throughput equals the consumer's rate), so it stays opt-in pending
  a better test.
- `CS_REMIX_REFRESH_HZ` (`dxgi_swapchain.cpp`) — requests a specific fullscreen
  refresh rate. Used to disprove vblank pacing. **It does not revert the desktop
  mode on exit**, which is how this machine was left at 164 Hz (it was 59).
- `-TlasRefit` proven: `scenePrep` 6.2 -> 4.0 ms, ~2.2 ms of GPU time returned,
  geometry correct. The largest single GPU win found. `scenePrep` does not scale
  with resolution, which is why dropping to UltraPerformance alone stopped
  helping.
- Instrumentation, all behind `CS_REMIX_GPU_PHASES`: `[RTX.dlfgphase]` (per-phase
  job timing), `[RTX.acquire]` (ring occupancy and blocked-acquire rate),
  `[RTX.gpu]` (per-pass GPU cost), and a present fence split.
- `LaunchTest.ps1`: `-NoOobMarkers`, `-EarlyBackbufferRelease`, `-ReflexSim`.

### Method notes

Runtime option changes do **not** return to the clean-launch number — after
toggling several settings and reverting them, an identical configuration measured
64.1 against 69.9. Set quality at launch and use a fresh process for any
comparison. `rtx.enableVolumetricLighting` is rejected by the config API
outright. Disabling `enableSecondaryBounces` makes the frame *more* expensive, not
less.

Do not `Stop-Process SkyrimSE` without first checking `StartTime` against your own
launches — the user plays on this same install and writes to the same log. See the
`never-kill-users-game` memory.

## Checkpoint 2026-09-21 — reload GPU dump decoded offline

PROGRESS; goal ACTIVE/incomplete. Added tools/remix/DecodeGpuCrash.py using the
installed Aftermath2.27 SDK in D:/dev/aftermath. Runs offline, exclusive output
creation, decoder destroyed in finally; optional SPIR-V hash inventory. No
binary/config change. Python compilation and148 existing tests pass; actual
archived dump decoding and362-file hashing both exit0.

`20260921-022350-paused-reload-decoded.json` establishes Vulkan PID23720,
Error_DMA_PageFault, read at GPU virtual address87064903680, Graphics Processing
Cluster. Compute shader hash11843068287573490492, compiled size198912, fault PC
compute_01+0x28860, MMU fault. Engine reset true, adapter reset false. No source
mapping/markers available in this decode. This is stronger than generic device
lost, but does NOT distinguish a stale allocation from indexing/binding bugs.

`20260921-022350-paused-reload-shader-scan.shader-hashes.json` inventories362
SPIR-Vs from `_Comp64Release/src/dxvk/rtx_shaders`; none matches the dump hash.
Do NOT attribute to NGX or exclude Remix: on-disk candidates may differ from
runtime-modified/deployed binaries, and external shaders were not inventoried.
Next useful diagnostic is exact submitted-shader hash/binary capture or richer
Aftermath shader debug lookup, alongside reload resource-lifetime audit.
Normal PID24208 remains alive; no game control or reload performed this turn.

## Checkpoint 2026-09-21 02:25 — paused reload GPU crash reproduced

PROGRESS; goal ACTIVE/incomplete. The NEXT paused Save1 reload after the three
successful attempts killed PID23720. Actual Console menu was verified open
before dispatch. Thus pausing is NOT a reliable workaround, and interior trips
are NOT necessary to trigger a reload failure. This failure differs from the
02:10 CPU exception: runtime logged VK_ERROR_DEVICE_LOST at02:23:50.204, host
wrote a3,079,484-byte Aftermath dump. Old CommunityShaders.dmp is still02:10;
do NOT treat it as the new failure's dump.

Preserved `.research/testlogs/20260921-022350-paused-reload-host.log`,
`20260921-022350-paused-reload-runtime.log`, and
`20260921-022350-paused-reload.nv-gpudmp`. Before the fault, runtime had8818
instances, prepareSceneData6.63ms, mergeInstancesIntoBlas3.55ms; earlier
buildBlases1.69ms. Not the prior50.6ms BLAS slowdown. No causal attribution yet.
The planned matched-position pre/post-interior performance test aborted before
its baseline, so it provides NO performance comparison. Historical performance
document now explicitly labels old FG results as historical, not acceptance.

No binary/config changes. Restarted normal PID24208 at02:24:59, configured Scene
with native world suppression, loaded existing Save1 from main menu. Next:
decode GPU dump / investigate resource lifetime across reload before more broad
travel repetitions. Three earlier successes do not invalidate this failure.

## Checkpoint 2026-09-21 02:22 — paused reload control survives three attempts

PROGRESS, goal ACTIVE/incomplete. No binary or setting changes. Same PID23720,
host5415D7D7/runtime016AD3F5. Three existing Save1 reloads while actual Console
menu was verified open all survived. First reload reset gameHour10.675995 to
8.607417 (daysPassed .444833 to .358642), confirming a real load; Console closed
automatically, HUD returned. Subsequent rounds ended engine frames59485/59916,
gameHour8.607606/8.607692, native-world-call cameras valid, first-person native
cameras valid, three view-model submissions, loaded geometry10760/10761 and
submitted geometry8622/8567. First reload resumed at about38.3FPS. This is bounded
reload/control evidence, NOT proof that pausing fixes the previous crash. Unlike
the crashed run, this session had not performed the same interior round trips.
No native-renderer A/B performed this turn; no attribution to Remix or game yet.
After the third reload,02:22:09..25 logs show31.8–35.5FPS, not the7–9FPS from
the earlier post-cell run. Performance still needs matched-scene analysis.
No saves created/overwritten. Game remains running after the third reload.

## Checkpoint 2026-09-21 — cell-load coverage, reload crash, capture wait correction

PROGRESS; goal ACTIVE/incomplete. Host5415D7D7/runtime016AD3F5 unchanged.
Normal PID23720 restarted at02:13:36 after PID46340 crashed during an unpaused
in-world Save1 reload. Crash at02:10:00.785: SkyrimSE.exe+155ddd3, access violation,
RCX7265646e6552. Archived host/runtime logs and dump under
`.research/testlogs/20260921-021000-reload-crash*`. Cause NOT established; do not
attribute it to scene lights, animation fixes, or unpaused loading without controls.
Fresh main-menu Save1 load succeeded. No saves written.

Eight paused-console coc transitions passed: SleepingGiantInn/Riverwood and
RiverwoodTrader/Riverwood in each POV. Evidence animation-third-person-cell-roundtrip-retry.json
and animation-first-person-cell-roundtrip.json under testlogs. These are host
camera/cell-membership checks, NOT door/fast-travel/image/pacing acceptance.
Old session slowed to7–9FPS near Riverwood; fresh Save1 about40–41FPS, different
positions/scenes, so no controlled regression cause established. Investigate.

The post-cell draw capture was rejected (vanity camera, only17/64 audit coverage).
CapturePlayerMovement now waits for observed render-frame progress instead of a
fixed2.5seconds before stopping the audit; bounded polling and process guard.
148 tests pass. Fresh capture first-person-fresh-load-framewait-1789953287:
frames42691..42754, full64-frame audit/integrity, zero horizontal motion, first
POV unchanged, weapon false→true, sequenceFramesObserved true. Sheet inspected.
Rigid attachment-to-submission mismatch zero. First-person hand holds42692..94
also occur in native snapshots. Extra axe holds42702..10 precede visible draw;
no claim that movement/attack judder or GPU/display pacing is solved.
Capture ends with weapon drawn, audit disabled, no injected input held.

## Checkpoint2026-09-21 02:03 — draw/sheathe transitions exercised

PROGRESS, goal ACTIVE/incomplete. No binary changes: host5415D7D7/runtime016AD3F5,
normalPID46340 stays live. Added optional CapturePlayerMovement -WeaponAction
Draw/Sheathe, queued after capture starts, before/after IsWeaponDrawn recorded
and validated. Forbidden with ObserveOnly. Stationary captures need zero motion;
147 tests pass (transition order/observer guards). Idle vanity camera twice
rejected preflight; briefly held/released mappedForward17 to exit vanity, then
captured after settling. No game settings changed or saves written.

Four64-frame captures, full pose coverage/integrity, zero horizontal movement,
expected weapon state transitions verified, camera mode guards pass:
- third-person-draw-transition-1789952209,38498..38561
- third-person-sheathe-transition-1789952332,42910..42973
- first-person-draw-transition-1789952396,45614..45677
- first-person-sheathe-transition-1789952494,49427..49490
All-frame sheets inspected. Reconstructed-to-submitted equipment mismatch ZERO
in all four. Third-person axe maximum origin steps8.57(draw)/6.17(sheathe), not
native vertex parity. First-person hand holds45615..45617 ALSO occur in native
camera samples, unlike earlier late-submission bug. Axe's additional draw holds
are offscreen before its visible draw; sheath holds49473..49490 are after it has
left the image. First-person hands have no held rotations during sheath.
Don't equate every repeated transform with a bug or claim attacks/equipment/
native image parity from these four transitions. No full-goal acceptance.

Save1 restored, playerLoadedtrue, auditfalse, all inputs released and all jobs
terminal. Next meaningful coverage: attack/animation changes, interior/travel
continuity, remaining native buffer/material parity. Earlier camera jump still
unreproduced; underwater artifacts recorded separately. Eyes parked.

## Checkpoint2026-09-21 01:55 — native camera comparison, jump not reproduced

PROGRESS, goal ACTIVE/incomplete. User permits control takeover. Deployed camera
diagnostic host5415D7D7A99AEF67638BA502E9E8BCBA5A05283E89F00D037F49CA090477C877,
build77597 exit0, backup20260921-native-camera-positions preserves3D3E4FC7.
NormalPID46340 started01:51:22; runtime016AD3F5 unchanged.146 CPU/source tests
pass. No camera behavior change. Native render-call snapshot now includes direct
scene camera world translation and player position, alongside frozen view/origin.
RecordAudit exposes them frame-tagged. MeasureCameraMotion.py checks exact frame
coverage and compares submitted eye with native scene eye and restored view eye;
tests distinguish native movement from stale submission.

Three64-frame fixed-third-person locomotion captures:
- camera-native-route-start-1789951944: maxnative/restore error0.008344units
- camera-native-route-crossing-1789951981: maxerror0.008376units
- camera-native-route-far-1789952038: maxerror0.008055units
Largest submitted steps12.54,8.28,5.83 respectively closely match native scene
steps. Earlier~116unit jump NOT reproduced; cannot attribute it to collision or
claim solved. Do not add smoothing/offset based on speculation. Crossing/far
integrity pass; far body sheet inspected. Far31292 onward camera goes underwater
and shows severe waterline/refraction/black-surface artifacts despite camera
agreement; separate broader-goal issue, not a camera-transform conclusion.
Previous user accepted above-water water and asked to move on; don't reopen all
water tuning indiscriminately. Prioritize remaining animation/transition testing.

Save1 restored, game loaded normally, audit off, inputs released, all jobs
terminal. Eyes parked. First-person/native-phase and attachment/static-probe
fixes preserved. Full rendering/native parity/travel/performance goal remains open.

## Checkpoint2026-09-21 01:48 — third-person attachment cache bug fixed

PROGRESS, goal ACTIVE/incomplete. User explicitly permits taking over controls.
New snapshot lifetime fix holds NiPointer meshes across discovery, filters stale
members before dereference, clears snapshots on DiscardFrame. Then controlled
third-person test isolates static-probe cache retaining old equipment despite
updated reconstructed attachment (axe9.1units behind at28863). Mapped animated
attachments now bypass and cannot arm static probe. Other static optimization
unchanged. Full evidence in newestremix-animation.md.

Currenthost3D3E4FC73F4BA1C4528B15A846EA648C84D7814C29FCF5A26A77BCD8338CC7DE,
build95317 exit0, backup20260921-attachment-probe-exclusion.143 tests pass.
Runtime016AD3F5 unchanged. NormalPID34012 started01:44:25. Two64-frame controlled
third-person runs pass integrity/full pose/fixedPOV/movement. Zero equipment
rotation holds and zero attachment-to-submission mismatch in both. Sheets read.

Next issue: repeat29070 has large third-person camera jump: axe world step15.32,
camera-relative110.12; sheet shows sudden closer body, attachment mismatch0.
Investigate native camera collision/phase versus imported view. Do NOT claim all
judder resolved. POV ownership12samples passes. Save1 restored, first person,
freeCamfalse, auditfalse; no input held, all jobs terminal. Eyes parked. Full
native buffer parity/materials/effects/travel/performance goals remain open.

## Checkpoint2026-09-21 01:36 — passive movement/turning evidence

PROGRESS, goal ACTIVE/incomplete. Added CapturePlayerMovement -ObserveOnly:
no SetPov, HoldKey, ReleaseKey, save loading or player control changes. Captures
current gameplay and flags mode mismatch in manifest without rejecting natural
transitions as a failed controlled test. Controlled mode retains strict gates.
141 CPU/source tests pass, including observer input guards. No binary change:
hostC87FC875/runtime016AD3F5, normalPID37436 continues running; all jobs terminal.

passive-player-motion-1789950897,37975..38038:64frame integrity/full pose coverage,
first-person mode throughout. Hands and axe have ZERO repeated rotations across
63 transitions. Native hand samples also zero repeats. Manifest reports1108.7
units horizontal movement and endpoint yaw1.0185 to-0.7675, but endpoint interval
includes capture encoding, NOT exactly the64frames. All-frame weapon sheet
inspected and shows movement/turning during capture. Additional evidence for
native first-person timing fix, not display pacing, whole-render or third-person
equipment acceptance. Audit disabled after capture; current user location/POV
untouched. Third-person equipment uninterrupted test still pending; don't fight
live user control. Eyes parked; broad goal requirements still open.

## Checkpoint2026-09-21 01:32 — third-person equipment candidate, not accepted

PROGRESS, goal ACTIVE/incomplete. Third-person baseline onB0415D2B shows body/head
advance every captured frame while rigid axe/scabbard hold26575/26610. Extended
shared-bone attachment reconstruction to playerBody, preserving native first-
person capture. DeployedC87FC875C8CEB88C070C25C66CF191D018E7C90B3AB0D5F1E6448D6BE7ED38B4,
build43153 exit0, backup20260921-third-person-attachment,140 tests pass.
NormalPID37436 started01:28:41, runtime016AD3F5 unchanged. All jobs terminal.

Validation INCONCLUSIVE: first post-fix run has no locomotion; retry switches
third-to-first mid-capture with moving camera. See newestremix-animation.md for
paths/evidence. Harness now rejects camera-mode mismatch; syntax validated.
Latest camera continued moving outside automation, likely user control: don't
fight it with repeated save reloads or movement. Inputs released, audit disabled,
freeCamfalse, currentPOVfirst. Save NOT restored after retry, current player
location kept. Third-person equipment fix needs uninterrupted validation; do
not report judder solved. Eyes parked. Full rendering/native parity goals open.

## Checkpoint2026-09-21 01:24 — native first-person pose timing fixed in tests

PROGRESS, goal ACTIVE/incomplete. User confirmed axe still judders after rigid
attachment fix. Native camera probe proved both hands advance while late Remix
poses hold54380/54389/54393, shared with axe. Production now freezes first-person
bones/attachment chains at that camera hook and restores both with identical
translation. See newest remix-first-person.md for complete evidence/limitations.

Deployed hostB0415D2B3D5F244FEC7C21974DAC96C051DCBDBC8FB548B359D39C15BFEAE61B,
backup20260921-native-viewmodel-pose, build89940 exit0.139 CPU/source tests pass.
Runtime016AD3F5 unchanged. NormalPID34868 started01:19:54. Two64-frame movement
runs have ZERO held hand/axe rotations, native coverage complete; axe maximum
camera-relative step~0.29units each. POV ownership12samples passes. Third
audit-off capture integrity passes, but enters water with waterline artifacts;
NOT whole-rendering acceptance. Sheets inspected. Save1 restored, first person
axe drawn, freeCamfalse/auditfalse, normal scene. All jobs terminal.

Keep eyes parked. Third-person periodic entry-bone fix preserved. Native-phase
sampling has NOT been generalized to all world/third-person geometry. Pending:
user visual confirmation, broader first-person attack/equipment/turning tests,
native vertex/camera parity and display pacing; other goal rendering gaps remain.

## Checkpoint2026-09-21 00:57 — first-person rigid attachment isolated

PROGRESS, full goal ACTIVE/incomplete. Third-person entry-bone fix remains.
First-person locomotion shows skinned hands updating every frame but rigid axe
holds at1310 then catches up1311. See newest remix-first-person.md for exact
evidence, identities and limitations. Opt-in audit expanded to view models with
entry/submitted world transforms and separate camera;136 tests pass. Analyzer
now handles rigid objects and groups by identity, not duplicate mesh names.

Host1A546792F1E4AA83AD5DFED5C71A40FE28798ABF158295369D5EE4D0394E4082 deployed,
backup20260921-viewmodel-pose-audit preserves4506FB59; build73489 exit0.
Runtime016AD3F5 unchanged. NormalPID47036 started00:54:03; Save1 restored after
tests, audit off. No production weapon fix yet, no all-animation acceptance.
Next: rigid attachment world currently sampled independently after bone snapshot;
audit/reconstruct its chain from sampled skeleton anchor, accounting for flattened
boneWorldTransforms versus cached node.world. Do not assume early cached-world
sampling alone fixes it. Eyes remain parked per user. All jobs terminal.

## Checkpoint2026-09-21 00:48 — periodic animation hold fix deployed

PROGRESS, full goal ACTIVE/incomplete. Current user priority animations/player
movement; eyes parked. New bonePhases audit confirms59/59 player bones advance
during each4-frame scene discovery,0/59 during other frames; following frames
repeat that advanced pose. SnapshotBones now samples existing skeletons BEFORE
Gather, adds only new sources afterward. Existing shared-bone consistency kept.
See newest remix-animation.md for exact evidence/caveats. No interpolation.

Host4506FB59A917FEFB8A92002BB23CF72ADEA425F6070D5D4EDD897A78D47EF9B7,
backup20260921-entry-bone-snapshot; build87784 exit0,135 tests pass.
Runtime016AD3F5/DXGI unchanged. Normal PID49876 started00:44:40.
Two64-frame forward runs show ZERO repeated head/body rotations versus16 each
before; full pose coverage/integrity pass. Third64-frame audit-off rendered run
passes integrity and sheet inspected. Save1 restored; normal rendering/audit off.
Not proof of all judder fixed, display pacing, native animation parity or first-
person/interior continuity. Morph and rigid-node capture timing still separate.
NRC startup initialization failure/fallback logged; no crash observed this turn.
All build/capture jobs terminal. Do not mark full goal complete.

## Checkpoint2026-09-21 00:37 — user priority movement judder

PROGRESS, goal ACTIVE/incomplete. Eyes explicitly parked by user. Added827
same-frame lobe diagnostic before redirect: build61506 exit0, compositeSPV
validates,131 tests passed then. Runtime016AD3F510A0C250CE44581F72AC31083B17ECCCE6CA65BCCC3FE437CABF3D3F
deployed backup20260921-paired-lighting; NO827 GPU captures/eye conclusion.

Whole-player opt-in audit deployed hostC6DAB911 (backup20260921-full-player-audit).
133 CPU/source tests pass. New movement harness and composed-pose analyzer.
See newest remix-animation.md for evidence and caveats. Confirmed head/body
repeat rotations every fourth rendered frame across normal and native-CPU-
preparation runs. Abrupt submitted bone-origin transitions match visible body
lurches, but bone origins are not vertices. No production judder fix yet.
Potential lead: expensive4-frame scene discovery before bone snapshot; need
measure discovery phase/animation producer, not assume causal connection.
NativePreparation restored off; normalPID53332 started00:37:06, existingSave1
load requested. No eye settings changed. Runtime grazing correction retained.

## Checkpoint2026-09-21 00:17 — native grazing-normal correction deployed

PROGRESS, full goal ACTIVE/incomplete. User reports grazing black on body/rock
grass. Native MSN/grass bypass now preserves only view-facing authored normals;
backward-facing inputs use existing getBentNormal. See newest remix-normals.md
entry for captures, limitations and exact hash. Build2315 exit0,130 tests pass.
Runtime65BC214D deployed with backup20260921-native-normal-facing; host unchanged.
Normal PID12692 loaded Save1, RR1, debug0, audit off, third/freeCam false.
808 red counts before2589–2752/frame, after0–10, orbit0–7. Not matched pixel
parity or complete visual acceptance. Startup black diagnostic rejected; NRC
initialization error logged, subsequent scene renders. Lit pose audit incomplete.
Eye flash and other full-goal issues remain open. No live build/test jobs remain.

## Checkpoint2026-09-21 00:03 — bright eye comes from background, not alpha composition

PROGRESS, full goal ACTIVE/incomplete. New opt-in debug826 displays same-frame
final/background-light luminance/alpha-light luminance/backgroundAlpha. No
history, post-tonemap only, native refraction retained; terms at composite sum.
See newest character doc for exact definitions and caveats. Build20518 exit0
after correcting initial field-name compile error.129 CPU/source tests pass;
composite+both debug shaders SPIR-V validate. No GPU unit/performance acceptance.
Deployed runtimeBD4A2208AB2ABB47E7228848BD7E66BD18FADCA51EF00FF60AA8F34DC7A9510F;
backup20260920-paired-alpha-composite preservesDCF07D06; hostBB430485/DXGI unchanged.

NoUpscaling PID6148 two frozen64-frame826 orbits pass integrity/zero pose changes:
eyes-alpha-terms1-1789945214 and terms2-1789945223. Full1067 bright vs1129 dark:
bright eye is in background contribution; alpha ROI encoded0, transparency255
in BOTH. Not an alpha-composition flash in these samples. Next same-frame main
diffuse/specular/direct/indirect split, not more alpha tuning. No production fix.

Archive eyes-alpha-terms-complete.log. NORMAL PID43460 started00:02:11,
Riverwood Save1 loaded/playerLoadedtrue, third/freeCamfalse/RR1 verified,
debug0 configured, native22draw/75compute suppressed after load. All jobs done.
No saves/deletes/persistent settings changes; grass/water/posefix unchanged.

## Checkpoint23:49 — eye flash does not require animation changes

PROGRESS by positive isolation, full goal ACTIVE/incomplete. Same normalPID38924
still live; no new crash observed. Three frozen-pose XY/yaw orbit sequences
under .research/sequences: eyes-shared-frozen-material-1789944407(view825),
eyes-shared-frozen-surface-1789944417(view823),
eyes-frozen-visibility-1789944509(view805). Each64frames integrity/pose join pass,
ZERO changed host poses/unsubmitted meshes. Eyes still switch white/dark;
full9672/9735 inspected. Beard attached. Thus host animation changes are not
required for remaining eye flash. Eye normal path currently EyeBrown_n.dds.
View805 broadly invalid-sample red around eye, but separate capture without
simultaneous final image cannot attribute the flash. Next paired same-frame
lighting contributions/validity, not global brightness or more pose tuning.
See character doc for semantics and limitations. No new code/deploy/settings
changes; BB430485 host/DCF07D06 runtime remain. Harness restored normal rendering,
animation, freeCamfalse and audit off. Final frame17059 live check reports idle
vanity camera; third-person request acknowledged but not verified effective.
Debug0 explicitly restored. No saves or deletes. All jobs done.

## Checkpoint23:44 — reproduced facial pose race corrected; eyes still open

PROGRESS, full goal ACTIVE/incomplete. Exact failure evidence: original2
pose capture5745/5769 beard disappears in final+albedo; ONLY those2 of64frames
have head/beard bone rotation disagreement. Same mesh/morph/retained throughout.
Submit now snapshots each shared bone source once before expensive capture;
Capture uses frame-local sampled bones and inverse of submitted instance.world.
No material tuning. See newest character investigation for scope/limitations.

Build39327 exit0,128 CPU/source tests pass. Host deployed
BB430485173514DAACEB26E0AF555B1B85CF3A328F2F46B4DAD234C4DDA5E077;
backup20260920-shared-bone-snapshot preserves883D28CF. RuntimeDCF07D06/DXGI066D7215
unchanged. Five64-frame audited sequences: zero head/beard rotation mismatches,
all animate, no unsubmitted meshes. Three stationary + two XY/yaw orbit sheets
keep beard attached. Additional64-frame normaldebug0/auditoff capture also
keeps beard. Evidence paths/ranges in character doc. Eyes STILL show sharp
brightness changes on orbit; no eye fix or full animation acceptance.

PID52056 native crash23:41:29 AFTER stationary captures, faultSkyrimSE+100efa4.
Cause unknown; not proven unrelated. Logs/dump archived shared-bones-first* in
.research/testlogs. Restarted once; NORMAL PID38924 started23:42:15, remaining
captures completed and playerLoadedtrue atframe4524. Final third/freeCamfalse/
auditfalse/debug0/RR1, native14draw/75compute suppressed. No live jobs, saves,
deletes or persistent config writes. Grass/water unchanged. Next eye isolation,
longer stability/NPC/first-person testing; not broad goal acceptance.

## Checkpoint23:29 — frame-correlated facial host poses available

PROGRESS, full goal ACTIVE/incomplete. Added opt-in bounded character pose
history under cs.sceneAudit, inspected via remixScene filter characterAudit.
CaptureCharacterSequence -PoseAudit writes pose-audit.json; AnalyzeCharacterPoses
joins exact rendered host frames and reports changed fields/unsubmitted meshes.
Seven dynamic player face classes carry identity/morph hash/world/full bones.
This is cached host data, NOT native/GPU/BLAS parity. See character investigation.

Host build32324 exit0;126 CPU/source Python tests pass. Deployed host
883D28CF79AFFAF890E4278AF3691FE6A72A6522521458F8B304DB2DC484412F;
backup20260920-character-pose-audit preserves BB6D1676. RuntimeDCF07D06 and
DXGI066D7215 unchanged. No production shading/pose fix.
Nine64-frame audited sequences pass integrity AND pose coverage. Frozen run
has zero pose changes, animated runs change poses; no facial mesh unsubmitted.
No large dropout reproduced in inspected sheets, and NRD head angle differed:
not matched improvement evidence. JSON audit overhead may perturb timing.
Next catch an actual failing image with -PoseAudit and inspect exact matching
host fields. Do not infer success from negative samples or repeat disproved
upload/skinning flush-order hypotheses. Eyes/beard/face remain unresolved.

NORMAL PID53148 started23:29:01, Riverwood Save1 loaded; playerLoadedtrue,
third/freeCamfalse/RR1/auditfalse verified, debug0 configured. Native22draw/
75compute suppressed after load. All jobs completed. No saves/deletes/persistent
settings writes; accepted grass/water unchanged. No broad acceptance claim.

## Checkpoint23:11 — eye material paired capture; beard/face still flicker

PROGRESS, full goal ACTIVE/incomplete. User explicitly reconfirms beard/face
flicker. Added opt-in debug825: final/specular-albedo/virtual-normal/roughness,
same-frame/source coordinates, no history, post-tonemap only, native refraction
preserved. See character investigation for semantics and primary-surface caveat.
Build8667 exit0;122 CPU/source tests pass; both debug shaders validate SPIR-V.
Deployed DCF07D0697B85C55C5FDEA13564C127F566F43786AD25F50C6BA0379310A5C55;
backup20260920-paired-material-frame preserves83B2618E. Host/DXGI unchanged.

PID44876 NoUpscaling three64-frame captures pass integrity. Full1767/1768
(eyes-paired-material3-1789942153) show eye dark-to-bright without obvious
roughness/specular-albedo jump or wholesale normal flip. Visual evidence only,
not numeric/object-identity proof. Full1485 (material2-1789942144) shows most
beard missing. No eye/character fix. Next visibility/shadow traversal and
animated-geometry lifecycle, rather than brightness tuning. Earlier view163
was invalid-alpha magenta, not support for borrowed-alpha-light attribution.
Archive eyes-paired-material-complete.log; all jobs completed.

NORMAL PID47820 started23:11:04, existing Riverwood Save1 loaded. playerLoaded,
third/freeCamfalse/RR1 verified; ConfigureRunningTest restored debug0; native
22draw/75compute suppression after load. No saves/deletes/persistent settings
changes. Accepted grass/water unchanged. No GPU unit suite or broad acceptance.

## Checkpoint22:57 — transparency safety corrected, eye flash NOT fixed

PROGRESS, full goal ACTIVE/incomplete. See latest character investigation.
composite_alpha_blend now has uniform barrier participation, bounded shared
neighbor coordinates and a read/write iteration barrier. No shading retuning.
Build21486 exit0;121 CPU/source tests pass; compiled shader SPIR-V validates.
No GPU unit suite/performance acceptance. Deployed D3D11
83B2618EEC46553CD5DC2746E74663AD7BDB921AC095FAA5A3C72FEF1E6AE7A4;
backup20260920-alpha-neighbor-safety preserves E34AAEE. Host/DXGI unchanged.
Three64-frame debug824 orbits pass integrity, but full2610 in
eyes-alpha-safe3-1789941160 still has bright eye in final and composite.
This is not an eye/character fix. Next inspect pre-composite eye alpha inputs.
Archive alpha-neighbor-safety-complete.log. Normal PID10472 started22:56:47,
existing Riverwood Save1 loaded/playerLoadedtrue/third/freeCamfalse verified;
native22draw/75compute suppressed after load. No live jobs or persistent
config changes, saves or deletions. Accepted foliage/water left unchanged.

## Checkpoint22:45 — eye brightening present before post-processing

PROGRESS, full goal ACTIVE/incomplete. New opt-in debug824 shows SAME-FRAME
final/composite-HDR/depth/motion. No debug history, post-tonemap only; native
refraction retained. See newest character investigation for exact semantics.
NoUpscaling PID43540 captured3x64frames, all integrity pass. Full5002/5003 in
eyes-orbit-composite3-1789940584 show eye brightening in both final and pre-
upscale/pre-postfx composite. Later effects may amplify but don't originate it.
Next pre-composite alpha lighting/visibility inputs, not global brightness.
Stochastic-alpha off/on earlier was inconclusive(no strong flash either run),
option restored. Source neighbor-search bounds/barriers warrant separate audit.

RuntimeE34AAEE13555D0085403DB92D3D8DE150B190D296801D409F04E7C704FA80B0F deployed;
backup20260920-paired-composite-frame preservesAA375669. Host/DXGI unchanged.
Build11297 completedexit0;118 CPU/source tests pass, no GPU unit suite.
Diagnostic only, no production eye/character fix. Archive
eyes-paired-composite-complete.log. No live jobs.

NORMAL PID48564 started22:45:16, Riverwood existing Save1; playerLoadedtrue,
freeCamfalse/RR1/debug0 verified, initial native22draw/75compute suppressed,
third-person requested after first-person save load. No saves/deletes/config
writes. Foliage/water/SSS untouched. All unresolved goal requirements remain.

## Checkpoint22:28 — bright eye reproduced with actual camera orbit

PROGRESS, full goal ACTIVE/incomplete. See newest character investigation.
CaptureCharacterSequence now supports OrbitCenter, moves camera XY+yaw and
persists capture-manifest.json with asynchronous24-step observed telemetry.
Yaw-only tests did not change surface viewing vector and were insufficient.

Confirmed bright eye in same-frame paired captures: normalRR13118 in
eyes-orbit-rr-1789939255; NRD/no-upscaling1826 in eyes-orbit-nrd-1789939385;
raw/no-denoiser1849 in eyes-orbit-raw-1789939501. All64-frame integrity passes.
Albedo remains dark at eye; may not represent blended eye final shading.
RR/NRD-only explanation excluded. Raw still has postfx and other temporal
systems. Subsequent motion-blur off/on comparison failed to reproduce strong
flash in either run, INCONCLUSIVE; motion blur restoredTrue. No fix claimed.
Next: pre-postfx eye visibility/shading and native eye alpha/material audit.
VANILLA_EYE_NORMAL is conditional and not defined in package/src; do not claim
missing that override causes current CS mismatch. Standard eye blend is not
emissive. Full eye material import still incomplete.

NORMAL PID40896 started22:26:05, Riverwood Save1, RR1/debug0/freeCamfalse/third;
22:28 native14draw/75compute suppressed. All jobs completed. RuntimeAA375669
unchanged.117 CPU/source tests pass, no GPU unit suite. No saves/deletes/config
writes; foliage/water/SSS unchanged. Goal and character issues remain OPEN.

## Checkpoint22:16 — beard-specific GPU check; eye flash still unresolved

PROGRESS, full goal ACTIVE/incomplete. Read newest character investigation.
Diagnostic runtime AA3756691F879B7FEC7AEA9348BFD4CEB6BB925EA4D6A74178121A74FBF88A17
deployed, backup20260920-selected-face-probe preserves DE9AB880. Host/DXGI
unchanged. Build succeeded;117 CPU/source checks pass, no GPU unit suite.
LaunchTest -SkinProbeVertices 250 arms64 selected facial-class samples only
in debug823. Beard GPU math passed(maxpos1.52588e-5,maxnormal1.78814e-7).
Not native-pose/BLAS/visibility acceptance or an explicit bad-frame join.
Three new64-frame beard sheets have no unmistakable dropout; prior5298
same-frame pre-denoise failure remains authoritative, not resolved.

Latest user eyes very bright on camera movement: still OPEN. Close-up normal
RR sweep eyes-close-rr-1789938925(1374..1437) shows eye but no bright flash.
Working close-up camera13700,-48192,-150,yaw-2.07738137. Harness has explicit
camera params now. Earlier close-up attempts had poor framing/yaw mismatch,
not valid negative eye evidence. No eye shader/SSS/brightness fix claimed.

NORMAL PID44360 started22:14:41, existing Riverwood Save1, RR1 confirmed;
22:16 playerLoadedtrue/third/freeCamfalse, debug0 restored, native14draw/75compute
suppressed. No live jobs. No saves/unowned deletes/persistent config writes.

## Checkpoint 22:00 — same-frame evidence places beard failure before denoising

PROGRESS, full goal ACTIVE/incomplete. Opt-in debug823 deployed: same-frame
final/albedo/depth/motion panels, no debug history, post-tonemap only, native
refraction retained. See latest [character investigation](remix-character-visibility.md).
Key evidence: paired-surface3-1789937666 frame5298 (previous5297) has beard
missing in BOTH final and resolved material albedo. This contradicts a purely
denoiser/DLSS cause. Next: investigate submitted facial pose/geometry, retained
updates, skinning and cutout visibility on a failing frame; head-only probe
is insufficient. Do not retune SSS or disable denoising to mask it.

Latest user specifically reports eyes becoming very bright while moving camera.
Separate OPEN issue. NRD and RR small camera sweeps captured; no unmistakable
white-eye flash in tiny inspected eye ROI, NOT acceptance. Need close-up sweep.
Lighting.hlsl EYE-specific normal/material handling is incompletely mapped by
generic importer; source lead only, not proven flash cause.

Deployed D3D11 SHA256 DE9AB880A2C2C8CA88D9E858D34251E7F72E694DE1D9064444AD8EBAE192DB46;
hostBB6D1676/DXGI066D7215 unchanged. Backup/symbols20260920-paired-surface-frame
preserve4B0C70B4 runtime. Builds69664/4158 exit0 (initial98321 compile failure
corrected before deployment).116 CPU/source tests pass, no GPU unit suite.
Only diagnostic runtime behavior changed; no production fix claimed. Capture
harness now has AllowCameraMotion/YawSweep with explicit motion reporting.
No deletes/saves/persistent configuration writes. Archive paired-surface-complete.log.

NORMAL PID17144 started21:58:52. Existing Riverwood Save1 loaded, normal RR1
confirmed, debug0/freeCamfalse restored after final sweep, third person.
22:00:01 native14draw/75compute suppressed. No live jobs. All unresolved
character/eye/skin leakage and full goal requirements remain OPEN.

## Checkpoint 21:42 — no-upscaling test reproduces beard dropout

PROGRESS, full goal ACTIVE/incomplete. No production binaries changed. Added
startup-only LaunchTest -NoUpscaling (RR off/upscaler None/denoiser preserved).
Three64-frame sequences on PID21148 all visibly fail: morph-nrd-native1-
1789936646 at1515, native2-1789936661 at1990, native3-1789936676 at2536.
Read [character investigation](remix-character-visibility.md) newest section.
DLSS is not required for dropout; raw negative captures do not prove NRD is
root cause. Next action: bounded simultaneous beauty/albedo/depth capture on
an actual bad frame, using existing raw export stages or a small ROI burst.
Built-in composite debug tiles cycle across frames; NOT a paired-frame test.

113 CPU/source tests and LaunchTest syntax pass. No GPU unit suite. Archive
morph-nrd-native-complete.log under .research/testlogs; sheets and sequences
documented. No saves/deletes/persistent configuration writes.
Normal rendering restored: PID25928 started21:41:57, Scene, existing Riverwood
Save1 loaded.21:42 inspect playerLoadedtrue/third/freeCamfalse, RR1 preset log,
native22draw/75compute suppressed. No live jobs. Current D3D11 4B0C70B4 and
host BB6D1676 unchanged. Eyes/beard/brows/skin leakage remain OPEN.

## Checkpoint 21:35 — facial isolation tests; no new production fix

PROGRESS, full goal ACTIVE/incomplete. Latest user: beard/eyebrows still jump,
eyes buggy too. Keep these and head/body/skin leakage OPEN. Detailed capture
inventory/caveats in newest [character investigation](remix-character-visibility.md).
Beard dropout reproduced with RR off (1776), RR+OMM binding off (9136), and
OMM restored (9704). RR-off still used DLSS SR/NRD. Four raw-lighting sequences
(no upscaler/no denoiser startup overrides) show no obvious isolated dropout;
negative evidence only, not root cause or accessory validation. Debug albedo/
facing and frozen short sequences are similarly not acceptance. Do not use
noise/disabled denoising as a production workaround.

Harness additions: CaptureCharacterSequence -FreezeAnimation/-DebugView with
finally restoration; LaunchTest -NoRayReconstruction/-RawLighting startup-only.
Normal launch clears all diagnostic env vars. 113 CPU/source checks pass,
LaunchTest syntax passes; no GPU unit suite. No binaries changed/deployed;
D3D11 4B0C70B4 and host BB6D1676 remain current. No deletion/save/config writes.
Archives morph-pre-rr-isolation.log, morph-rr-off-complete.log,
morph-raw-complete.log in .research/testlogs. Next: separate NRD from SR, or
registered native/RTX accessory depth/position/motion at a real bad frame.

NORMAL runtime restored: PID52392 started21:34:38, Scene configured, existing
Riverwood Save1 loaded. Inspect21:35 playerLoadedtrue/freeCamfalse; third-person
requested afterward. Runtime preset log confirms rayReconstruction=1 again;
debug0/native suppression requested by ConfigureRunningTest. No live jobs.
Foliage/water/SSS brightness unchanged. Eyes need their own material/geometry
investigation; passing head-only GPU probe is not eye/beard/brow correctness.

## Checkpoint 21:15 — compatible morph update deployed; character still FAILS

PROGRESS, full goal ACTIVE/incomplete. Implemented in-place compatible morph
snapshots across host/runtime: stable mesh handle and topology hashes, fresh
immutable vertex/basis snapshots, retained revision/cache invalidation. Layout,
triangle, skin-weight/index and material signature changes reject the update
and preserve existing rebuild fallback. Actual deletion still destroys meshes;
no stale geometry workaround. Read newest
[character visibility investigation](remix-character-visibility.md) for code and
exact test scope. This fixes the identified history-loss path, NOT all flicker.

113 CPU/source checks pass; no GPU unit suite run. Host/runtime builds92754 and
20237/46302 exit0. Deployed D3D11
4B0C70B4A9335F0F0B32143A572780006544986A70FD9E583E42E5CE32C8ABC9;
host BB6D1676EAD5D9993954A85C6038C066597D37BCFD56F8A0BA7AFAC0E22A7A13;
DXGI066D7215 unchanged. Owned backups/symbols20260920-morph-snapshot preserve
prior C13E runtime/A7AE host. No deletes, saves or persistent settings changes.

Host log morph-snapshot-plugin.log proves same-handle updates to head/beard/brows/
eyes. GPU morph-snapshot-skin.json passes64samples/all898headvertices with ONE
topology key, max position error7.62939e-6, normal1.78814e-7. This is limited
submitted-pose evaluation, not native timing/actual pointer lifetime/all frames.
Front-facing sequence morph-snapshot-character-1789935046 (1398..1461) STILL
shows head/beard jump at1454. Under .research/sequences; *-face contact sheet
under .research/testlogs. Keep character, head/body and skin leakage OPEN.

Normal-settings sequence morph-normal-character-1789935190 (2389..2452) is
REJECTED as face validation: character turned away, old ROI shows shoulder.
Full frame2389 inspected. Do not count as clean/pass or blame camera jitter.
Current normal PID47304 started21:11:50, Scene, existingRiverwoodSave1 loaded,
no diagnostic overrides. Latest inspect playerLoadedtrue/third/freeCamfalse.
No live jobs. Next useful step: bad-frame native/RTX pose/depth comparison to
separate real geometry/visibility mismatch from temporal shading. No further
SSS/brightness tuning justified by this evidence. Performance remains unmeasured.

## Checkpoint 21:00 — bounded head skinning validation; morph history still discarded

PROGRESS, full goal ACTIVE/incomplete. No visual fix claimed. Read latest section
of [character visibility investigation](remix-character-visibility.md) first.
GPU head diagnostic implemented/deployed and run:64 sparse samples, all898 head
vertices, four recreated geometry keys; max position error7.62939e-6 and normal
error1.78814e-7 against CPU calculation of submitted inputs. It does NOT prove
native-pose timing, beard correctness, bad-frame GPU/BLAS state or visual fidelity.
Archive/report `.research/testlogs/face-skin-probe-v2-complete.log` and
`face-skin-probe-v2.json`. First version stopped18/64 on synthetic topology-key
change and is explicitly incomplete. LaunchTest -FaceSkinProbe is opt-in, normal
launch clears both probe env vars.109 CPU/source tests pass; no GPU unit tests.

New concrete source lead: host preserves retained handle on morph, but
destroyExternalMesh -> removeReplacementInstancesWithSpatialMapHash destroys its
runtime node anyway (including hostOwned). trackRetainedDraw ALSO clears prims
when the mesh spatial hash changes. Imported morphs get fresh synthetic topology
hashes, creating new BLAS buckets. Need compatible dynamic-mesh/history handling,
not simply skipping deletions. Actual removed meshes must still disappear; no
replaying missing mesh or unbounded retained history. Not yet proved root cause.

New sequences under .research/sequences: head-probe-character-1789934062 visibly
fails at21414; head-probe-v2-character-1789934270 has no obvious isolated dropout
in its short2223..2286 run. Both fixed camera/integrity pass; neither overlaps
GPU readback frames410..745. Keep animation/head-body/skin leakage all OPEN.

Current deployed D3D11 SHA256
C13ECB2037C28B127B2C738D0D7D5987F517D39D14CD293A15D246F26C114F8C;
DXGI066D7215 and hostA7AEA2A7 unchanged. Owned backups/symbols
20260920-face-skin-probe-v2 (previous diagnostic16758D7F), original runtimeBF1D
in20260920-face-skin-probe. Builds64353/98368 exit0, logsbuild-face-skin-probe*.log.
Only opt-in diagnostics changed, not lighting/SSS/foliage/water/geometry behavior.
No deletes, saves or persistent settings. NORMAL PID33016 started20:58:47;
Scene configured and existingRiverwoodSave1 loaded, no probe/native-prep
or matched-sample overrides.21:00 inspect frame3118/playerLoadedtrue, thirdperson,
freeCamfalse; log21:00:17 confirms native14draw/75compute suppressed. No live jobs.

## Checkpoint 20:49 — ordered-ray fix deployed; normal character flicker reproduced

PROGRESS, full goal ACTIVE/incomplete. Latest user: "still flickering. i think
that the black might have been a symptom of another issue". The broad black brow
band is absent in the inspected new static capture, but animated beard/head
alignment still FAILS under BOTH diagnostic and normal launch settings. Do NOT
claim character, head/body motion, or skin leakage fixed. See
[character visibility investigation](remix-character-visibility.md).

Ordered transparency now preserves the hardware ray origin/direction and advances
TMin to nextFloat(last absolute hit T), while converting hit distances to current
shading intervals. Portal continuation resets the absolute-T bookkeeping and ray
limit. Query and Trace/SER paths share the implementation. No global bias,
material brightness, SSS, foliage or accepted water changes. Build15636 exit0,
`.research/testlogs/build-ordered-continuation.log`; 107 CPU/source tests pass,
NOT GPU unit tests. Full performance and animation acceptance still open.

Deployed D3D11 SHA256
BF1D1EF0F1FE44715F87B44D5CE2DD39E08D822A0E5C1C382BF674C45118A7E5;
DXGI066D7215 / hostA7AEA2A7 unchanged. Owned-DLL backup/symbol label
20260920-ordered-continuation. No deletes, saves, or persisted settings.

Evidence: frozen `.research/testlogs/face-ordered-continuation-layers.json`
reports105 raw float32 depth samples deeper than native by>5 in the face ROI,
including misses; debug817 reports only83 deep hits and excludes misses. These
residuals remain unclassified. Poses differ across launches: no matched A/B
improvement percentage. New CaptureCharacterSequence.ps1 captures animation
with fixed freecam, NO tfc1 freeze, and restores freecamfalse.

- Diagnostic sequence ordered-character-1789932946: frames3957..4020, failures
  at3964,3970,3973,3981,4014 (examples).
- First normal sequence ordered-normal-character-1789933352: frames1326..1389,
  no obvious isolated beard dropout. NOT acceptance.
- Second normal sequence ordered-normal-repeat-1789933477: frames6098..6161,
  failures at6102,6109,6133,6134,6142,6158. Thus not diagnostic-only.

All sequences under `.research/sequences`; corresponding `*-face.png/json`
contact sheets/reports under `.research/testlogs`. All integrity/camera guards
passed. These are pre-Present rendered-frame failures, not WSI pacing evidence.

Current NORMAL PID47244 started20:41:43, existing RiverwoodSave1 loaded, configured
Scene, no matched-sample/native-prep overrides. Latest inspect playerLoadedtrue,
freeCamfalse; idle vanilla vanity camera active (not a frozen diagnostic camera).
Log20:43:22 reported native14draw/75compute suppressed. Debug0/firstHitfalse on
fresh normal launch. No running build/capture jobs. Normal timing396frames in
10.113s=39.16renderedfps, GPU timing unavailable, no matched baseline.

Next useful action: frame-tag native/API/GPU head/accessory poses on failing
frames, including draw membership/retirement and BLAS/history assignment. Existing
bounded skinning probe only covers24-bone MSN body, not2-bone head/1-bone facial
accessories. API already deep-copies matrices; dispatch uses geometry's bone
count; retained replay occurs BEFORE upload/skinning flush. Those source leads
were checked and are not established bugs. Do not substitute further SSS tuning
for pose/geometry evidence. Skin underarm leakage also remains open.

## Checkpoint 20:23 — facial continuation skips close layers; still not fixed

PROGRESS, goal ACTIVE/incomplete. User: beard and head/hair blend also flicker;
whole head separates from body; latest "happening less but still happens".
Do NOT claim fix or general animation improvement. Full details/reproduction:
[character visibility investigation](remix-character-visibility.md).

Deployed full-float GeometryResolverState/GeometryPSRResolverState directions,
removing a provable f16 continuation precision loss. PathState remains f16.
Black brow band still visibly fails. New diagnostics817/818 hit consistency /
identity,819 unshifted next hit. Exported FLOAT16 debug alpha is always1, not
frontHit/indexBuffer. IDs quantize. Pre-fix817 also included f16 ray-direction
reconstruction error, so wasn't exact initial hardware-ray position.

Valid frozen PID14148 captures face-next-unshifted-{Pair-817-1789931932,
Remix-817-1789931944,first-hit-817-1789931955,first-hit-819-1789931964}.
Manual failed face mask1683pixels: final depth error median+12.3976units.
Unshifted original ray with nextFloat(T) finds close geometry at ALL1683,
zero repeated primitive/miss; first-to-next gap median.0108185units,
next depth error median-.02635 (FLOAT16 export precision). Mostly same head
surface as final back-head hit, not a missing mesh. Native pose/depth identical
at sampled pixels across captures. Pair and Remix-only diagnostics identical.
Current normal-offset continuation is ~.023units here, larger than layer gap.
Strong origin-offset skipping lead; robust fix must preserve segment distances,
cone/volume/unordered handling, portal/PSR and Query/Trace paths. Do NOT simply
edit TMin in macros, globally shrink bias or flip skin normals. Head/body motion
still separate, source synchronization/skin palette/history NOT yet isolated.

CompareFaceLayers.py reproduces stationary/float-direction/next-unshifted JSONs
under .research/testlogs. CaptureBrowBuffers HitDiagnostics includes819firsthit.
102 Python analytic/source tests pass; PowerShell parses; no GPU unit tests run.
Build11437 exit0 (build-next-unshifted-hit.log); deploy97827 and capture30708 exit0.
D3D11 SHA25616FD196BFB4B897DC99D40EC66D374F9E23F2109A977E226784940B3A3B57E85;
DXGI066D7215 / hostA7AEA2A7 unchanged. Owned-DLL backup/symbol label
20260920-next-unshifted-hit. Prior precision deployment5F83A166 archived under
20260920-primary-ray-precision. No deletes, saves, persisted settings, material
brightness changes or accepted grass/water changes.

Diagnostic14148 quit normally. NORMAL PID42796 started20:21:40, no matched
samples/native-prep overrides, configured Scene, existingRiverwoodSave1 loaded.
20:22 playerLoadedtrue, HUDonly, firstperson/freeCamfalse, debug0/firstHitfalse.
Native log20:22:29 confirms22draw/75compute suppressed. Screenshot202232-
face-layers-normal-restored.png inspected. No running jobs. Next safe work is
robust ordered-alpha continuation + moving-character/native comparison, NOT
further SSS brightness tuning. Skin underarm leakage also still open.

## Checkpoint 19:43 — black brow band has a registered depth mismatch

This turn PROGRESS, no production patch. Full goal ACTIVE/incomplete. See
[character visibility investigation](remix-character-visibility.md) for source
leads, exact captures, valid isolation and rejected interpretations. Do not
attribute the band to missing eyebrow alpha or fix it with normal bending/SSS
brightness: failed Pair frame1723 proves native face depth63.94 versus RTX75.51
in band, median discrepancy+11.576gameunits; nativeNs.V+.829/RTX-.678.
Forehead control agrees within.0042units. All raw noisy lighting is alreadyzero
in band. Same-frame identity and primary-ray registration validated. Head
interior / missing or mismatched front geometry is a lead, not established cause.

New CompareBrowBuffers.py reproduces report .research/testlogs/brows-failed-pair.json
from .research/buffers/brows-fresh-pair-lit-1789928760. CaptureBrowBuffers.ps1
freezes/positions/guards camera, supports Pair0/808 and optional OMM/alpha-test
isolation with finally restoration. Two OMM sequences lacked band in baseline
screenshots, so NOT evidence for OMM. Some raw paired frames differ from adjacent
screenshots despite frozen native pose; investigate native reference capture
side-effects versus actual runtime dynamic geometry/BLAS data. No definitive
production fix yet. Both SSS off still left band in valid earlier screenshot set.

All 99 Python analytic/source tests pass; new Python and PowerShell scripts
syntax-checked. These are not GPU unit tests. No binary deployments this turn:
D3D11 remains617DEB83, DXGI066D7215, hostA7AEA2A7. No deletes/save writes or
persisted settings. Accepted foliage/water unchanged; skin leakage still open.

Diagnostic PID35672 closed normally with qqq. Current NORMAL launch PID27944
started19:42:20, no MatchCaptureSamples/native-prep override. Configured Scene,
existing RiverwoodSave1 loaded;19:43 playerLoadedtrue, firstperson/freeCamfalse,
debug0. Log19:43:05 confirms22draw/75compute submissions suppressed. Final
194309-brows-investigation-normal-restored.png inspected. No running jobs.

## Checkpoint 19:15 — grazing terrain black-patch fix deployed and verified

This turn PROGRESS. User reported dark grazing angles on some objects. Concrete
terrain defect isolated and fixed; do NOT claim all materials/all angles passed.
Skin diffusion leakage remains unresolved; accepted grass/foliage unchanged.
Full goal ACTIVE/incomplete.

Production D3D11 SHA256
617DEB8314D9031A6ACF5A833EDBB09835F993884BC426A0EB26B3298AB65DDB;
DXGI066D7215 and hostA7AEA2A7 unchanged. Backup/symbol label
20260920-grazing-hemisphere. Build5408 exit0, logbuild-grazing-hemisphere.log;
99 Python analytic/source tests pass (not GPU unit tests). Build/deploy skills
followed; no deletes or persisted game settings/save writes.

Runtime utility/math.slangh getBentNormal early return checked reflected ray
hemisphere but not Ns.V. Reflection is invariant under N -> -N, so backward
normals could pass that check and then be rejected by opaque BSDF lighting.
Early return now additionally requires dot(shadingNormal,incidentDirection)>0.
Existing correction/fallback handles other cases; valid view-facing inputs are
unchanged. Native grass/MSN bypass remains intact. New test_grazing_normals.py
includes sign-ambiguity reproduction and 200k fixed-seed float32 sphere cases.

Matched full-resolution/zero-jitter Pair captures, 23artifacts each:
- Before: grazing-pair-facing-1789927404 and repeat-1789927409.
- After: grazing-fixed-facing-1789927877 and repeat-1789927881.
All .research/buffers. Same camera13796.2705,-48428.0859,-137.3191,pitch0,
ACTUAL yaw-.35641089. Input yaw+.35639989 produced the negative actual value
in these runs: always inspect metadata, do not assume sign conventions.
CompareGrazingBuffers.py verifies same-frame pair, matching camera and primary
pixel centres; JSON reports .research/testlogs/grazing-{pair,fixed}-facing*.json.
Explicit foreground ROI x>=1000,y>=800 contains257600opaque pixels.
449 backward-facing/black pixels before, zero after in both repetitions.
At those original449pixels native normals/depth unchanged; new Remix Ns.V
range.10989..49157, old-.98710..-.28065. Native-vs-Remix normal discrepancy
increases intentionally at these invalid BSDF orientations; NOT normal parity.
Lit190324-grazing-pair-lit.png /191129-grazing-fixed-lit.png inspected: discrete
black foreground patches gone. Lighting/foliage differ across launches, not
claimed a fully temporally matched lighting comparison.

Whole-scene red808 count4979 ->1877. Followup Pair15/16 under
grazing-residual-{0-15-1789927955,1-16-1789927959}: geometric Ns.V positive at
all original residual1877pixels, shading negative1875 (cross-frame diagnostic).
804 capturegrazing-residual-grass-0-804-1789928015 labels1857of1877 ordinary
grass,20other. Do NOT reopen accepted grass based on this; remaining20 may
include cross-frame coverage and are not classified as a new root cause.
808 red threshold is materialEpsilon, not strict zero; analysis uses decoded
world normals and exact ray directions to distinguish actually negative dots.

Current normal launch PID47192 started19:14:17, no MatchCaptureSamples/native
prep override. ConfigureRunningTest Scene, existingRiverwoodSave1 loaded.
19:15 HUDonly, playerLoadedtrue, freeCamfalse/firstperson, debug0/printoff.
Log19:15:02 confirms22draw/75compute submissions suppressed. No pending jobs.
New CaptureCurrentMaterialViews.ps1 supports frozen screenshot/Pair sweeps,
optional camera position and capture movement rejection; finally restores0 and
unfreezes. Next safe work: skin spatial-leak validation / eyebrow material bug.

## Checkpoint 18:53 — diffusion spatial falloff corrected; grazing report open

New user report: grazing angles are dark on some objects. Asked asynchronously
which objects / for a pointed camera; no answer yet. Current screenshot
185119-grazing-current.png inspected, but no clearly identified target. Do NOT
globally change Fresnel, normals, brightness or accepted foliage based on this
alone. Source candidates: opaque lobe/projected-weight checks reject negative
Ns.V; native MSN deliberately bypasses getBentNormal, unlike generic materials.
getBentNormal itself has a custom closed-form grazing/reflection-clearance
implementation in utility/math.slangh. Need actual object/normal/visibility
evidence to separate these from expected Fresnel or the skin-specific issue.

User correctly rejected skin completion after units fix. Valid frozen isolation
183723default/183726no-transmission/183730small-diffusion/183733no-diffusion-or-
transmission (all .research/captures, camera13565.2559,-48227.9336,-130.3105,
pitch.0175924,yaw1.1497774) showed the under-shoulder light patch persists with
transmission off but disappears with diffusion off. User explicitly held controls
for this set. Earlier183346series was inside body;183537series moved substantially
midcapture. Reject those sets. All settings restored afterwards.

New production runtime F99FC94F5D814A60A15F04F5F6A547344B56CFAE562958B85AC3AA35649F156F,
hostA7AEA2A7 unchanged, DXGI066D7215 unchanged. Backup/symbol label
20260920-skin-surface-distance; build8541exit0, logbuild-skin-surface-distance.log.
Deployment/relaunch33741exit0; currentPID28764 started18:46:46, normal launch,
configured Scene/suppressWorld, existingRiverwoodSave1.93Python tests pass.

Adapter now uses an orthonormal disk frame for the selected axis, then evaluates
Burley spatial falloff at actual3D hit separation. Combined area PDF accounts
for both existing .5-weight projection axes, per-channel probability and hit
normal Jacobian. Optional single-scattering subtraction evaluated at actual
radius too. Rejects hits outside existing maxSampleRadius. SDK remains intact.
This is NOT a complete new BSSRDF integrator: closest front hit/material-only
matching still approximate; disjoint meshes/occluded alternative projection PDF
and all-pose behavior need further validation. See remix-skin.md for formulas.

New frozen comparison184754default/184757no-transmission/184800small-diffusion/
184803no-diffusion-or-transmission inspected. Camera13620,-48240,-158,pitch0,
yaw1.17161119, unchanged. Large broad/angular skin shadows remain in both default
and no-diffusion views. The prior interior-skin-colored patch is not obvious in
this angle, but restarted pose/camera differs from183723: NOT a matched old/new
leakage proof. Do not claim the user's reported leak fully fixed yet.

Diagnostic816 writes actualDistance/maxBound red, projectedDistance/maxBound
green, blue0, to primary debug image; non-skin cleared in geometry resolver.
GPU-print fields actualDistance,projectedDistance,maxWeight,normalAlignment.
ProbeSkinDiffusion.ps1 captured185050 inspected: bright red near underarms with
little green, demonstrating distant hits at small projected radius, before
weight rejection. Selected four probe pixels missed skin: records=[]; NOT numeric
distance evidence. Camera/actor had moved relative to earlier captures; skin
occupies right side of this diagnostic. No debug/print/freeze left active.

CompareSkinTransmission now includes diffusion-off and camera-consistency guard;
restores all defaults through nested finally. Test script rejects moved captures.
Newest185119lit capture normal gameplay; full goal remains ACTIVE/incomplete.

## Checkpoint 18:32 — RTXCR world-unit conversion deployed and compared

Previous turn PROGRESS: dimensional audit confirmed meter coefficients were
combined with raw Skyrim positions/thickness. This turn PROGRESS: built/deployed
the runtime and host corrections; frozen in-game coefficient A/B/repeat shows
softer fine shadows on skin. NOT finished: black eyebrow band and larger angular
arm/shoulder shadows remain visible. Full goal ACTIVE; no broad parity claim.

Current PID40260 started18:26:21, normal LaunchTest/ConfigureRunningTest, existing
Riverwood Save1 loaded. D3D11 SHA256
213529A9F92FA7A11E37DAD2919B299251D0832F311A2E591F7C6650AD684C1D;
plugin A7AEA2A7C059DF6AFD7A194766CF1210B483D11E43C11024799AC56977E3D0E6;
DXGI066D7215 unchanged. Backup/symbol label20260920-skin-world-units.
Build sessions81617/runtime and19185/plugin exit0; logs build-skin-world-units.log
and build-plugin-world-units.log. Runtime build/deploy skills followed.
88 Python tests pass, including dimensional regressions/source contracts, NOT
GPU shader execution tests. No files deleted, no saves/settings persisted.

Host reads bhkWorld::GetWorldScaleInverse: logged69.99125units/m; sceneScale
0.699912 passed successfully. Do not substitute Streamline's hardcoded scale:
this is the actual engine value. RTXCR material scale now includes
cb.metersToWorldUnitScale, making sigma values inverse-world-unit for BOTH
diffusion and transmission. Raw traced distances need no second conversion.
Host disk bound16gameunits converted with GetWorldScale into meters (~.2286),
inside normalized8-bit material storage. Prior16 was outside that valid range;
packing casts before clamping, so do not assume a defined old effective bound.
The new bound decodes to roughly15.92gameunits after quantization.
No foliage/water material settings or sun/exposure changes. Global sceneScale
also has other consumers (NEE cache range, particles, terrain baking); full
regression of those is still outstanding, not claimed unchanged in all cases.

Useful capture comparison:183010-skin-units-body-corrected.png,
183015-skin-units-body-legacy-coefficients.png,
183019-skin-units-body-corrected-repeat.png, all .research/captures and inspected.
Frozen camera13735,-48155,-158,pitch0,yaw-2.07738137; metadata identical.
CompareSkinUnits.ps1 temporarily sets diffusionProfileScale=1/69.99125 for the
control, then restores1 and unfreezes. This recreates the old coefficient scale
ONLY, not the old invalid disk bound/global scale. Fine fern/hair shadow edges
are visibly softer with corrected coefficients, repeated; eyebrow/large angular
shadows persist. Earlier182729series missed character,182822series showed only
hair at bottom: discard both as skin evidence. Use screenshot preflight, not old
camera assumptions. In current freecam, yaw input retains its sign.

At18:31 HUDonly/no modal, freeCamfalse (idle vanity camera); default scale1,
debug0 and transmissiontrue retained, no test capture process pending.
Native14draw/75compute submissions suppressed18:27:49. User can take over.
Next: correct diffusion disk orthonormal basis independently and inspect sample
hit distance/identity; inspect eyebrow alpha in its now-reproducible black-band
angle. Native skin specular/gloss/_sk calibration remains outstanding.

## Checkpoint 18:16 — authored MSN retained; skin appearance still visibly wrong

Previous goal turn PROGRESS (invalid-light safety). This turn PROGRESS: paired
GPU/native evidence isolated normal bending, and production now preserves loaded
native MSN shading normals, retaining geometry/ray-offset normals. NOT a skin
appearance pass: angular shadows, reported arm light/shadow leakage and a black
eyebrow band are still unresolved. User additionally asks whether the integration
uses all RTX skin because it looks harsh. Honest status: RTXCR diffusion and
single-scattering transmission active, but generic skin parameters and fixed
roughness.8 remain; native specular/gloss/_sk calibration not implemented.
Full goal ACTIVE/incomplete. Accepted foliage1×/sun1/3/water/grass untouched.

Current deployed D3D11 SHA256E5C2EC8F202152D61AC06B475E85F782DE05AB18D4E6F1FDE00826B910A5C6BC,
DXGI066D7215/plugin5F76C431 unchanged. Backup/symbol label20260920-skin-preserve-msn.
Build81928 exit0 (build-skin-preserve-msn.log);82Python tests pass. Diagnostic815
added first with build57168 exit0, runtime1757298020..., labelskin-authored-normal.
Runtime build/deploy skills followed; no new options/GPU layouts. Two production
paths retained: previous invalid-light safety, now authored-MSN preservation.

Normal test evidence in newestremix-skin.md and testlogs JSON reports:
- Before preservation, same-frame authored normalp99=4.50deg vs final24.59deg
  against native,393086pixels; two repeats agree. Mode815 raw encoded normal
  uses the material sampler/animated basis, separately from finalGbuffer normal.
- After preservation, authored-to-final max.081deg. First camera's all-mask
  normalp99=97deg/depthp99=17units due separate foreground coverage mismatches;
  retain those failures, do not call a parity pass. Interior/depth<.1 diagnostic
  p99=4.60deg, but never substitute that subset as acceptance.
- Clearer second camera13690,-48295,-148,pitch/yaw0,242424pixels, native
  normalp99=3.158deg/depthp99=.01974units, repeated. Still extreme outliers.
  Captures skin-msn-clear-authored-1789924094/-repeat-1789924098,23artifacts each.
- Lit180146before/180604after inspected: angular shading remains. Different
  restarted poses, not exact A/B. Current preservation is a normal-data fidelity
  improvement, not proof of correct shadow/specular behavior in all poses.

User image I:/SteamLibrary/steamapps/common/Skyrim Special Edition/ScreenShot11.png
shows black brow band and harsh polygonal torso/face shadows. New report: arm
shadows pass through as if too thin. CompareSkinTransmission captured181217
default,181220transmissionfalse,181223also diffusionProfileScale.1, all identical
frozen current-camera metadata; all inspected, no clear resolution. Restored
transmissiontrue/scale1/freecamfalse. No permanent brightness workaround.

Important next checks (source evidence, not fixed yet):
1. Diffusion disk normal switches to shading normal or view direction but keeps
   old geometric tangents. RTXCR_CreateSubsurfaceInteraction only copies inputs;
   EvalBurleyDiffusionProfile uses those tangents for sample placement. Construct
   an orthonormal disk basis and verify sampled hit distances/lighting.
2. Imported maxSampleRadius16, RTXCR coefficient units(.01), normalized8-bit
   interaction radius and100*sceneScale disk bound need a coherent units audit.
   Generic(.5,.5,.5) radii/scale1 are not calibrated skin. Don't guess thickness
   or globally change sceneScale without ray-distance evidence.
3. Map authored native specular colour/strength/gloss instead of constant.8.
4. Brows are resident-textured feature6Hair,alphaFlags4333,threshold128,1bone,
   nativeTangentFrame,importedMaterialtrue/hairCardsRequestedtrue. Empty texture
   debugname does NOT mean missing texture; SRVformat77/pathMaleBrow_1.dds valid.
   Census filtercase-sensitive. Lowercase'brow' matched brown dust; useexact
   BrowsMaleHumanoid01 orBSFaceGenNiNodeSkinned. Later181217brows lack the black
   band, so inspect view-dependent alpha/hair/retained behavior.

All diagnostic processes exited normally. Current PID10480 started18:14:09 via
normalLaunchTest (no full-resolution, zero-jitter or native-preparation override).
Restore session94007 exit0, ConfigureRunningTest Scene, existingSave1 loaded.
At18:15:33: playerLoadedtrue, HUDonly/no modal, native14draw/75compute suppressed.
No freezes or debugviews left active, transmissiontrue/scale1 fresh defaults.
No pending builds/captures/tool sessions, no settings/save writes or deletions.
User is interacting with the game; do not infer its current camera from older
captures or call idle vanity movement a renderer bug.

## Checkpoint 17:51 — invalid skin light-sample path made safe; shadows unresolved

Prior acknowledgement-only turn was NO PROGRESS. This turn made concrete
progress: identified the known albedo boost behind most measured skin colour
error, and fixed undefined-data use in diffusion lighting without changing
accepted foliage1×, sunlight1/3, water, grass normals or tone. Full goal remains
ACTIVE/incomplete, including all native Lighting.hlsl material properties.

Current D3D11 SHA256DABF35382F2335147004CE9D5BBFC4A542A0EBDC2735AF776C867C98660438C2;
DXGI066D7215/plugin5F76C431 unchanged. Backup/symbol label20260920-skin-invalid-light.
Build16216 exit0, logbuild-skin-invalid-light.log. Deployment/relaunch90984 exit0.
PID38388 started17:46:57, normal LaunchTest/ConfigureRunningTest and existingSave1.
No matched-sample/native-preparation override, no settings/save writes/deletions.
All77Python tests pass (new4albedo numeric tests and2skin source safety tests).
Runtime build/deployment skills used; no new runtime options or GPU layouts.

evalNEEPrimary legitimately runs with no valid surface light when diffusion is
enabled. The fallback VisibilityResult was only partially initialized, and a
successful diffusion sample could continue into uninitialized light/attenuation
reads. Initialized absent inputs and added a diffusion-only accumulation/return
before those reads. Valid surface-light shading unchanged. Detailed evidence
in remix-skin.md. This is a safety fix, NOT a fix for all angular skin shading.

Baseline captures174423..174440 and new174751..174807-skin-invalid-light-after
use the same camera coordinates. Lit174440before and174751after inspected:
polygonal shoulder/back shadows persist. Restart changes pose/weather/time;
do not claim strict A/B image parity. CaptureSkinSun restores debug/freecam.
CaptureSkinBuffers.ps1 additionally captured raw buffers at fixed frozen pose:
buffers/skin-invalid-light-classification-1789922946 and
buffers/skin-invalid-light-visibility-1789922950. Both19artifacts plus manifest.
Debug output1080p versus raw Gbuffers720p; do not directly mask raw buffers with
display-resolution classifications. Approx67.39% of372737eroded display skin
pixels show invalid surface-light state in the next captured frame. Jitter and
frames differ, so this is approximate coverage, not exact path execution proof.
Raw noisy/denoised diffuse/specular, albedo and linearZ are all finite; sanitized
outputs are only a smoke check and cannot prove absence of internalNaNs.

Final capture175035-skin-invalid-light-restored-play inspected, world visible.
Freecamfalse, debug0, GPU printoff; idle vanity camera had automatically engaged
by17:50:35 (normal gameplay idle state, not a held diagnostic camera). HUD was
visible in earlier post-fix lit captures; vanity hides it. Native14draw/75compute
suppressed17:49:12; suppressed native-render CPU mean1.125ms at17:49:13. No
unperturbed performance benchmark. No live builds/captures/tool sessions remain.

CompareSkinBuffers --configured-albedo-scale1.53846153846 explicitly accounts
for the retained earlier artistic1/0.65 ordinary-albedo boost, never fits it,
preserves raw errors and flags clipping. Same-frame393698skin pixels now show
known-scale/pow2.2 RGB MAE(.003773,.002386,.002331), zero saturated channels.
Reporttestlogs/skin-matched-normal-known-scale.json. Corrected source comment
claiming textures were sRGB-tagged linear data; host binary unchanged (comment
only). Normals/outliers remain; actual native specular/gloss transport remains
unfinished (ordinary/skinroughness stillconstant.8). Next isolate residual
normal bending/self-occlusion or implement those authored material properties;
do not repeat the already reverted texture-derived vertex-normal experiments.

## Checkpoint 17:31 — 1× foliage accepted; skin-normal comparison narrows investigation

Latest user accepted foliage at1×. Keep colour/transmission corrections and
sun1/3; stop tuning foliage. Updated goal additionally calls out eyebrows/other
character parts, shader-based LOD, and existing non-PBR Lighting.hlsl material
properties working as an extension of game lighting. Full goal ACTIVE/incomplete.

Current runtime D3D11 SHA256807E2D36659228CA23391A64D65550EE39E1E8F54AB49B61317B9F9F01E2ECDC,
DXGI066D7215/plugin5F76C431 unchanged. Backup/symbol labelskin-sun-diagnostic.
Adds opt-in debug813/814 sun-normal diagnostics only; production skin path
unchanged. Build75357 exit0, deployment67149 exit0. Shader guide and runtime
build/test/deployment skills followed; no new options or layouts.

CaptureSkinSun.ps1 same-pose screenshots172204..172220 and ProbeSkinSun.ps1
GPU values show dark patches include both triangle and shading normals facing
away from the single sun. A geometric-gate bypass alone is not a justified fix.
No broad axis inversion at sampled points. Both scripts restore debug/freecam.
Details and precise evidence in remix-skin.md newest section.

Matched-sample PID50728 captured two native/Remix same-host-frame skin pairs
at1981/2073. CompareSkinBuffers.py enforces camera/sample registration and uses
the actual debug801 diffusion mask. Across393698pixels, median normal error
.1805deg,90th2.5266deg,99th30.7015deg; absolute depth99th.04635units.
Both repeats agree. Outliers and albedo discrepancy remain: no parity pass.
71Python tests pass, including4new analyzer helper tests. No skin fix deployed.

Diagnostic process exited normally. Restore session51905 exit0: PID15800
started17:30:43 with normal LaunchTest/ConfigureRunningTest and existing Save1.
No MatchCaptureSamples/NativePreparation/frame cap; debug0, GPU printing off,
nativeReferencefalse, suppressWorldtrue, no freeze. No save/settings persistence
or deletions. Verification74009 exit0: capture173133-skin-diagnostic-restored-play,
first-person/freecamfalse; normal world and HUD visible. At17:31:31 native22draw
batches/75compute suppressed; native suppressed-render CPU mean1.326ms. Reported
render rate36fps is a spot observation, not an unperturbed performance benchmark.
No active builds, captures, freezes or tool sessions remain.

## Checkpoint 17:13 — foliage response returned to 1× at user request

User accepted the transmission correction ("seems to have worked") and requested
returning brightness to 1×. Removed the shared native foliage response's 5×
multiplier; this affects both visible SSS and per-card transmission. Linear
soft/back texture weights, tint-only transmission, RTXDI native flag repair,
grass colour/normal formulas, sunlight at one-third, and tone remain unchanged.
Updated the response reference and structural test for 1×.

Build session63097 exit0 (testlogs/build-foliage-linear-1x.log), dedicated
test_native_foliage session26213 passed1/1, and all67 Python tests passed.
Normal qqq shutdown, owned-DLL backup/deployment and launch/configure/load
session72906 exit0. Backup/symbol label20260920-foliage-linear-1x.
D3D11 SHA2569150B1D2EB7892C5FA9796C6AFC71C71A5F26217D1349732B5BACCC08C1CC901;
DXGI066D7215/plugin5F76C431 unchanged. PID53012 started17:13:20.
Scene configured with native world suppressed; existing Riverwood Save1 loaded.
Full goal remains ACTIVE/incomplete; this is a foliage-strength adjustment only.
Capture session71974 exit0: captures/171418-foliage-linear-1x.png. Camera metadata
differs from the requested comparison transform; do not treat it as a matched
before/after. Cleanup requested freecam off and first-person. User subsequently
said "acceptable for now"; stop foliage tuning and leave the game running.

Before this request, skin captures170714lit/170716debug808/170718debug806/
170720debug807/170723debug16 still showed angular dark shoulder/back patches.
808 faces camera; 806 marks many patches as invalid light samples under RTXDI.
Earlier RIS-only nearby self-occlusion evidence remains relevant. No skin edits
were made; diagnostics restored debug0/freecam off before this deployment.

## Checkpoint 17:03 — linear soft/back weights deployed; clear canopy improvement

Current D3D11SHA2569DFB920E1309F7266C44C880893D7D9CC8AE415D015D3AEC33A62C49CA2CE601.
DXGI066D7215/plugin5F76C431 unchanged. Owned backup/symbol labelfoliage-linear-weights.
PID15944 started17:01:31, launch/configure/load88527exit0. Scene/worldsuppression
configured, Riverwood existingSave1loaded. Compare44517exit0. Captures170214
sss-tint/170218diffuse-times-sss/170223sss-tint-restored have identical camera
metadata and match165600baseline transform. Acrossrestartweather/wind differ,
but within-run three-way test shows a clear effect: authored linear tint alone
lights the green pine canopies; diffuse-modulated transmission darkens their
centres again. 170214 and170218 inspected. Kept defaultfalse (SSS tint alone),
5xresponse unchanged. Not yet complete all-foliage or full-goal verification.
Closer15133,-46900,650 test71966exit0:170329tint/170334diffuse-times-SSS/
170338tint-restored allsamecamera (.14998868pitch,yaw0). Bothfirsttwocaptures
inspected: brightgreenfoliageextendsintocanopywithtint-only; diffuse-modulated
visibilityagainblackensdenseregions. Solidtrunkremainsdark/opaque,asexpected;
do notclaimallremainingdarkareasarewrongorallfoliageiscomplete. Restoredfalse,
debug0/GPUprintoff,freecamfalse/firstperson. PID15944left running. No pending
builds/sessions. No savedsettings/save/deletions. FullgoalACTIVE/incomplete.

Prior turn classified PROGRESS: repaired RTXDI native payload flag and measured
dark pre-shadow response. Broad goal remains ACTIVE and unqualified.

GPU input probe809 added to geometry_resolver, source values only for native
non-grass foliage, disabled outside debug809. It displays raw soft RGB and prints
frame-cycled w0 diffuse,w1 soft,w2 linear soft product,w3 rolloff/gamma/diffuse scale.
ProbeFoliageInputs.ps1 controls selected internal pixel and restores debug0/printing
off. Probe runtime96BED651... deployed labelfoliage-input-probe PID29944. Build11782
exit0,67structuraltests. Raw captures165600baseline/165602softmap samecamera.
At internal(280,290),(310,330),(1130,170), soft RGB≈0.16–0.32 but linear product
≈0.0002–0.0035. Roll8/gamma1/scale1, live diffuse+soft SRVformat77BC3_UNORM.
Records archived in testlogs/foliage-input-probe-run.log. Different phases use
different jittered frames, not identical samples. Raw buffer capture
buffers/foliage-resolved-sss-1789919481 also exists but was during vanity camera
movement: do NOT use as a matched-view comparison. Render extent1280x720.

Lighting.hlsl converts only diffuse via Color::Diffuse, but soft/back textures
are direct linear multipliers in LightingEval.hlsli even with LL enabled.
Candidate now uses nativeFoliageLightingChannel = linear(base)*rawWeight, instead
of linear(base*rawWeight) for trees/non-grass. This preserves linear lighting
weights instead of gamma-decoding them in LL-off imports. Grass base-squared
formula, visible normals,5x angular strength, sun1/3, water, skin unchanged.
No custom tone or separate soft-shadow path. This mapping matches native linear
lighting structure, not proven parity to legacy gamma-space summation.

Build21843 exit0, logbuild-foliage-linear-weights.log. Dedicated unit1409 exit0,
test_native_foliage passed1/1: linear mapweights bothcolourmodes/zero/neutral,
existing1005angle-rolloff cases andgrass.67 Python tests passed after updating
the source contract from the old product-conversion expectation. Skills followed.
Closed PID29944 throughqqq; DeployRuntime -Labelfoliage-linear-weights/LaunchTest/
ConfigureRunningTest/loadRiverwood completed as recorded above.
CompareFoliageTransmission nowaccepts Position/Pitch/Yaw whilefrozen. Use input
Position13665.1552734375,-48229.41015625,-121.34022521972656;
Pitch-.157042846,Yaw+.080073 givesreadbackpitch.1565368/yaw-.0790826.
That matches165600. Normalcamera/freecamrestoredbeforepreviousprocessclosed.

## Checkpoint 16:46 — native foliage flag repaired in RTXDI; canopy still unqualified

Latest user: still really dark at the centre. Keep5x, sun1/3, accepted grass
normals/water unchanged. No separate shadow term or custom tonemapping added.

Found actual source bug: RAB_GetGBufferSurface copies SharedSubsurfaceData but
zero-initializes flags. It interpreted native soft/back/rolloff as generic thin
optics. Soft-only foliage has backColor0, giving zero generic back-facing weight
in light-selection PDFs. Reconstruct native interaction flag from the current
source material with producer's thin/maps gates; guard invalid surface indices
and non-thin interactions before material load. No bindings/strides changed.
Full builds34270 and18857 exit0 (logs build-foliage-rtxdi-payload[-guard].log).
67 Python structural tests passed. Guarded version deployed with owned backups
under foliage-rtxdi-payload-guard. Current D3D11SHA256:
9DCA3457FDE37E5F81DE4071889F37F300DC64E4F79CD6A12DBC816B147620E5.
DXGI066D7215 and host5F76C431 unchanged. PID38220 started16:45:34.
Launch/configure/load session57888 completed exit0. Riverwood HUD only.
Final guarded-run164644-foliage-guarded-live.png and164647-foliage-guarded-response.png
have identical camera metadata (13665.155,-48229.410,-148.007,pitch.157043,
yaw-.080073). Debug803 resolved-response view is itself nearly black on green
pine foliage, whereas snowy foliage is bright. This points to very low native
soft/back colour products even before visibility, not proof of a shadows-only
cause. Next inspect actual source SRV/colour-domain values against Lighting.hlsl;
do not increase exposure or claim that the sampler repair solves the centre.
Restored debug0, freecamfalse/first person, game left running. No pending builds.

Previous unguarded version0978C8E8 ran PID7604, normal Console/qqq exit.
164327-foliage-rtxdi-after.png inspected: working Riverwood/UI but dark cores
remain. 164330debug805 still has invalid(red) samples inside tree silhouettes.
Before164017/164020 has different yaw: camera drive flips BOTH pitch and yaw
in these views and readback is slightly offset. Do not claim pixel-matched A/B.
Debug restored0/freecam toggled off each time. Need final guarded-run capture.

Prior experiment now included in runtime: visibility-only non-grass response
omits diffuse multiplication (authored soft/back tint alone). Grass and visible
native shading untouched. rtx.debugView.foliageDiffuseTransmission true restores
diffuse*SSS visibility, false(default) is experimental SSS-only tint. Compare
script CompareFoliageTransmission.ps1 freezes via tfc1, takes false/true/false,
restores false/freecam off. 163535 first frame moved vertically, not comparable;
163540/163544 matchcamera but no compelling core-brightness improvement. Reassess
mapping after fixing light-selection flag; do not claim it solves dark cores.
803debug now previews grazing+full-back native response*pi, including5x, not
raw SSA/back-only. RtxOptions.md has not yet been regenerated for diagnostic
option (existing game-root output ownership unverified; do not overwrite blindly).

No save/settings persistence or unowned-file deletion. Broad goal ACTIVE and
incomplete. Dark foliage remains unresolved; source repair is progress, not proof.

## Checkpoint 16:23 — requested 5x native foliage SSS deployed

Latest request: "try 5x". Native soft/back response is now multiplied by 5 once
in shared nativeFoliageResponseChannel. Visible shading is not energy-clamped;
per-card coloured visibility remains clamped to 0..1. Generic thin materials,
skin diffusion, ordinary diffuse, accepted grass normals/water and sunlight
intensity are unchanged. No separate soft-shadow path was added, per the user.

BuildRuntime session19629 exit0; log build-native-foliage-5x.log. Dedicated unit
session54550 exit0, test_native_foliage passed1/1 including1005 angle/rolloff
combinations with exact5x scaling.65 Python tooling tests passed. The prior2x
variant failed one Slang subprocess with a path-not-found message; the complete
5x rebuild succeeded.2x was never deployed. Do not treat intermediate binaries
as current. Runtime build/deploy/test repository skills were followed.

Closed PID46468 through Console/qqq, then deployed with owned-DLL backups and
symbols under label native-foliage-5x. Current D3D11 SHA256:
E0DE07C6F86F3C977B32B819E47DA33D93C9F8D11DAAAD791C514D5C3B59AA6A.
DXGI remains066D721569675B2172754F23A40DA3044E873B8B167A628A3534DA4780EF6788;
host plugin remains5F76C431 from the menu-input deployment. PID22868 started
16:22:24; configured Scene/nativeReferencefalse/suppressWorldtrue, loaded existing
Riverwood Save1...20260920044800. No save, saved settings, or unowned file deletion.

1x native-response on/off captures and user feedback confirm improvement, not
complete foliage correctness. Dense canopy remained dark at1x. User was moving
the camera; no new stationary-camera requirement. Capture session96350 exit0:
162305-native-foliage-5x-live.png was inspected and shows the live scene, bright
grass/foliage and working HUD, not a black/loading frame. Camera was user-driven
first person at13419.05,-49409.00,77.10; this is not a matched1x/5x comparison.
HUD only at16:23:41; process still rendering. Some leaf-card regions remain dark,
so no full canopy/light-transport correctness claim. Left game running for user.
Broad goal remains ACTIVE and incomplete. Latest work is progress, not a blocker.


## Checkpoint 16:17 — native formula deployed at1x; latest user requests5x

User rejects pursuing a separate soft-shadow term: "that was just about simulating
scattering". NO separate shadow path was added. User says starting to look better
but still too dim, requested2x then5x. Explained live game is still1x;2x was never
deployed. Current source nativeFoliageResponseChannel multiplies soft+back by5.
Generic thin/skin, ordinary diffuse and sun unchanged. Native coloured visibility
uses the same scaled response, clamped0..1. New shader helper is shared with CPU
tests, which check the exact multiplier over1005angle/rolloff combinations.

1x build49792 exit0, unit82842 test_native_foliage1/1passed,65Python tooling tests
passed. Deployed labelnative-foliage-response. Live PID46468 started16:10:51;
D3D11SHA7A63972E29D93269DEA78E2BCA48AFE1265312065673ED27B9B8D059873920AB.
DXGI066D7215/plugin5F76C431 unchanged. ConfiguredScene/suppressWorld, sameRiverwood
save loaded. Initial161122capture was black while startup settled: INVALID.
Settledcanopy161222on/161227off/161232restored allsamecamera and visibly stronger
greenbranches on, canopy core still dark. No full foliage correctness claim.
Grass-bank161418on had camera movement: INVALIDforcomparison.161422off/161427
restored at13000,-47300,650,pitch-.450005,yaw0 matchcamera; vegetation visible.
Bothscripts restoredthintrue/mapsTrue/debug0 andfreecamfalse. User nowactively
movinginfirstperson; do notfighttheirinputor claimallcaptures stationary.

TraversalrunPID7368 exited~16:05, no newdump/error seen; reasonunestablished.
Archived logremix-thin-traversal-run.log. ExistingCommunityShaders.dmp15:49 predates
thisrun. Do not claima newcrash diagnosis. No saves/settings persist/deletions.

2x build30839 FAILED duringoneSlangvariant with "The system cannot find the path
specified"; no shader syntaxerrorprinted.2xCPUtest11095passed. Latest5xsourcebuild
session19629 running, logbuild-native-foliage-5x.log.5xunit54550running,
logtest-native-foliage-5x.log. Pollthese, don'tduplicaterestartbuilds. Candidate5x
NOTdeployed yet. Nativeformula1x is live. Needdeploy5xoncebuilt/verified, with
controlledrestartandownedDLLbackups. GoalACTIVE,incomplete.


## In-progress checkpoint — native foliage angular response, 2026-09-20

Latest user explicitly says compute from Lighting.hlsl; no need for physically
based model. Current task is all foliage thin SSS scaled by native soft/back.
Goal ACTIVE, not complete; substantial in-game verification still required.

Traversal flags build1595 exit0. Game PID4984 had already exited before testing.
Deployed traversal + integer slider fix, labelthin-traversal. Live D3D11:
4142E72B2C6410E5B25DB5FEA431A0286FFFCEBF20C54EB9FA750F0EFBD663A3.
DXGI unchanged066D7215; plugin unchanged5F76C431. PID7368 started15:54:40.
Configured Scene/suppressWorld and loaded existing Riverwood Save1...044800.
Free camera15133,-46900,650,pitch.14998868,yaw0. Capture155538-thin-traversal-canopy
still dark. No saved settings/save changes or deletions. No freeze currently.

Intermediate Beer-Lambert shader build30037 exit0 but NOT deployed. User then
preferred native formulas; candidate helper/visibility replaced accordingly.
Current source is native angular response described in remix-foliage.md.
BuildRuntime session49792 running, logbuild-native-foliage-response.log.
Dedicated unit build/test session82842 running, logtest-native-foliage-response.log;
correct test name test_native_foliage. Previous1262 built the intermediate unit
exe but test invocation failed because name was native_foliage; not a test pass.
64 Python structural tooling tests pass. Poll existing sessions, don't duplicate.
Current candidate not deployed or visually proven. Next: finish build, test flags
and payload/normal behavior, deploy with owned-DLL backup, compare same camera,
then grass and native response/maps toggles. Do not claim all foliage is correct.


## Checkpoint 15:45 — integer slider memory bug fixed/built, NOT deployed

Previous turn PROGRESS (menu and input deployment). This turn PROGRESS: found
and fixed unsafe RtxOption integral slider adapter. Live PID4984 unchanged;
user is testing menu settings, so no restart/deploy/config writes/camera input.

The menu showed RIS Light Sample Count1065353223 (=0x3f800007). Source option
risLightSampleCount is uint16_t default7. rtx_imgui.h SliderInt(RtxOption<T>*)
cast the address of its typed local value to int*, bypassing the safe integral
overload. This reads/writes4bytes even when the local is only2bytes. Removed
the cast so overload resolution uses the existing clamped conversion adapter,
matching DragInt/Combo/InputInt. This is a UI memory-safety fix, not a lighting
change or proof that the GPU was using a billion RIS samples.

New test_integer_widget_contract.py failed on old SliderInt cast, then passed;
all60 Python tooling tests pass. These are structural checks, not GUI execution.
BuildRuntime session3056 exit0, logbuild-remix-integer-slider.log. Built D3D11:
9F98E90462EC56F185AEC2B91B9835699F4BF4820D02011CEFD3D826062251B8.
NOT DEPLOYED; live remains912D9DB1/plugin5F76C431 fromcheckpoint15:42. User was
warned to avoid RIS sample slider until a later restart with this build.
Physical-keyboard Alt+X reply still outstanding; no new claim of verification.

Read-only foliage follow-up: rtx_instance_manager.cpp1273 alpha-tested opaque
materials get geometryFlags0, no FORCE_NO_OPAQUE; fully opaque get OPAQUE.
No thin-SSS exception found there. OpacityMicromapManager::calculateInstanceUsesOpacityMicromap
allows alpha-tested materials and does not inspect thinSSS. visibility.slangh
handleVisibilityVertex (and thus thin transmission) is evaluated only for
CANDIDATE_NON_OPAQUE_TRIANGLE; rayFlags has no FORCE_NON_OPAQUE. Hardware opaque
hits clear attenuation. This is a plausible bypass of leaf transmission,
especially for OMM opaque regions/fully opaque thin surfaces; NOT GPU-proven
as the canopy cause. Next controlled experiment should isolate traversal
opacity versus transmission, without changing SSS colour or shadow threshold.
No foliage source change this turn. Goal remains ACTIVE and incomplete.

## Checkpoint 15:42 — Remix menu rendering and DirectInput capture deployed

User explicitly rejected adding Reinhard/custom tonemapping. No tone-curve code
was changed. Current priority became making Alt+X and Remix settings usable.

Root cause: D3D11SwapChain::PresentImage never called ImGUI::render. Added the
primary-only call after the game/HUD blit and before OnPresent, with game HWND.
First rendering-only build F8863D88 deployed15:29, PID46920: menu visible but
clicks failed and game camera moved (user confirmed). Do not reuse that build.

Current fix routes Skyrim DirectInput events through csRemixGuiInput, queued
under mutex and consumed on the Remix GUI render thread before NewFrame.
D3D11 host-input mode discards the separate raw-input overlay before startup;
D3D9 is unchanged. Atomic capture flag makes the existing Skyrim input hook
consume the entire input list before game/CS menu dispatch while Remix is open.
Characters, left/right modifiers, mouse buttons/wheel, focus loss are routed.
See remix-menu.md for ABI and limits. No independent rendering window.

BuildRuntime session53946 and BuildDev session51585 both exit0. Logs:
build-remix-menu-input.log and build-remix-menu-input-host.log. 59 Python tooling
tests passed (3 new structural menu contracts; not behavioral proof).
DeployRuntime/DeployPlugin label20260920-remix-menu-input kept backups/symbols.
Current D3D11 SHA256:
912D9DB1344339EEA5F48E79D9A44B4AF859E2F47F87F1B1B05442E6F37682A4
Current plugin:
5F76C431F0AE8F19410F7E406A5464E9892A8161D9F46F6E04DE4B6F449F3D7E
DXGI unchanged066D721569675B2172754F23A40DA3044E873B8B167A628A3534DA4780EF6788.

Live PID4984 started15:37:57, configured Scene/suppressWorld, loaded existing
Save1_2B23D269_0_73647364_Tamriel_000002_20260920044800_1_1. Not saved.
Computer-use desktop observations show user successfully navigating Basic ->
Developer, opening sections/dropdowns and scrolling. Two DevBench camera reads
across menu interaction were exactly identical:13665.1552734375,-48229.41015625,
-148.00704956054688,pitch.10112107545137405,yaw-.04842371866106987,freeCamfalse,
POVfirst. HUD Menu only. Thus clicks/scroll/camera blocking visibly working.
Injected Alt+X through computer-use did NOT reliably close the menu (it stayed
open); physical keyboard shortcut confirmation is outstanding, asked via async
question. Do not claim Alt+X is behaviorally verified yet. User is actively
adjusting settings; do not reset their live changes. No graphics values were
changed by the assistant this turn beyond normal test configuration. Do not
persist/reset/delete user settings as part of input testing.

Known unrelated warning remains: changing DLSS profile live with FG can crash.
Broad goal remains ACTIVE, not complete. No foliage/skin/lighting fix claimed.

## Checkpoint 15:21 — local shadow comparison and tone-curve audit

Previous turn PROGRESS (diagnostics deployed/tested). This turn PROGRESS:
PID49528 unchanged, frozen canopy camera15133,-46900,650,pitch0.14998868,yaw0.
Only rtx.localtonemap.shadows varied2/3/4/2 with4second settle; captures151922,
151926,151931,151936-local-shadows-* respectively. Levels2/3 images inspected:
3 lifts some detail, canopy interior still dark. No quantitative raw-HDR claim.
Restored shadows2; camera freeCamfalse,first-person. No other tone setting changed,
no source rendering edits/build/deployment, no saved settings/saves/deletions.
Exec1822 completedexit0. Goal ACTIVE; not a canopy fix.

User asks whether current tonemapping is ACES and whether Reinhard is possible.
Source: local luminance.comp.slang uses ACESFilm on all3 exposure variants;
final_combine uses ACESFilm even for local multiplier estimation, with optional
final ACESFilm controlled by localtonemap.finalizeWithACES(defaulttrue).
useLegacyACES defaults true: Narkowicz rational approximation. False selects
BakingLab ACES fitted matrices/curve, NOT Reinhard. Local shadows settings have
visible live response, supporting active local path. No live option getter used.
Global apply shader contains reinhardToneMapper atline70, but rg of all src finds
no call site: global mode actually invokes dynamicToneMapper(histogram curve).
Thus disabling ACES finalization or selecting Global is NOT selecting Reinhard.
An explicit Reinhard option requires wiring through runtime/shaders, preferably
the same selected operator for local exposure pyramid and final combine.
Existing Reinhard helper divides by inputLuminance without a zero guard; do not
wire it in unchanged. Answer only so far; no Reinhard implementation yet.

## Checkpoint 15:18 — canopy visibility/facing diagnostics; one-third deployed

Previous goal turn PROGRESS (requested sun balance). This turn PROGRESS adds
and GPU-verifies diagnostics and rules out broad inverted normals in this view;
not a canopy fix. Goal ACTIVE, full objective unchanged.

Live cutoff test on PID54228: resolveOpaquenessThreshold254/255 versus0.999999,
frozen same canopy camera. Captures150730-default-lit,150732-default-visibility,
150734-low-visibility,150738-low-lit. Restored254/255. Visibility categories
change but lit canopy stays dark. This global threshold is NOT retained as a
fix. Current thin attenuation can be very small (SSA*x*exp(-x)/(4*pi), with
Fresnel factors), but testing the cutoff alone gives no meaningful canopy lift.

Found diagnostic gap: invalid light samples skip evalNEEPrimary, so debug805
previously left those opaque pixels black instead of its documented red. Added
red write before the NEE valid-sample guard. New debug808 evaluates actual
material shadingNormal dot rayInteraction.viewDirection: green >=epsilon,
red <epsilon, magenta nonfinite, blue non-opaque. Registered view in runtime UI.
No production BSDF/normal/transport changes. Two new source-contract tests,
56Python tests pass; scoped diffcheck passes. BuildRuntime session8234exit0,
.research/testlogs/build-leaf-facing-diagnostics.log, two-pass build completed.

Normally exited54228; deployed backed-up owned DLLs only. Current D3D11 SHA256
A31D3ADA4BE0FE30815AE6DBB3B8248FAA0A77DB11CC20E8240D67DCCFDC9836;
label20260920-leaf-facing-diagnostics. DXGI unchanged066D7215... . Also deployed
the already-built one-third-default plugin5908DA4745DB86D3EF0ABE04CEE29525679A8778AA7139FD2DABC4A4317959F3,
label20260920-directional-one-third. Prior checkpoint's pending-deployment note
is now superseded. PID49528 started15:14:43, same existing Riverwood save.

New CompareLeafFacing.ps1 captured same frozen camera15133,-46900,650,
pitch0.14998868,yaw0:151554-lit,151556-normal-facing,151558-thin-selection,
151600-rtxdi-visibility,151602-ris-visibility,151606-ris-lit. Facing view is
overwhelmingly green including dark canopy, with narrow red grazing patches;
not evidence of normals all correct, but rejects widespread reversed N.V here.
RTXDI red/no selected samples covers dark canopy. RIS changes most of these to
blue/selected sample blocked, and final lit image stays similar/dark. Hence do
not infer an RTXDI sampling bug merely from red: visibility rejection is involved.
Both lit images and facing/visibility views inspected; no exact raw-buffer parity.

Restored useRTXDItrue,debug0,unfrozen/freeCamfalse,POVfirst. Two light readbacks
pass at default1/3 (no live scale override needed).15:16:41 log confirms native
22draw/75compute suppressed. No new crash, no saves, no persistent settings,
no deletes, no active sessions. User asks whether local tonemapping could help.
Source defaults: mode1Local, shadows2,highlights4,exposure.75,boostContrastfalse.
No tonemap override found in examined host/harness/game dxvk.conf; no live-option
getter used, so do not assert queried runtime values. No tone changes made yet.
Official Remix docs confirm higher local shadows lifts shadow areas; possible
presentation improvement, not restoration of missing physical scattering.

## Checkpoint 15:04 — requested one-third directional intensity

Previous goal turn PROGRESS (packed colour flag repair). This turn PROGRESS:
user revised the desired sun balance from0.2 to one third. Applied live
cs.directionalLightScale=0.333333333, confirmed effective0.333333343 and fresh
native/API light samples (CheckDirectionalLight.ps1,3 samples pass).
DefaultRadianceScale source changed to1.0f/3.0f with regression expectation;
standalone test13 checks pass. BuildDev session94916 exit0; log
.research/testlogs/build-directional-one-third.log. Built plugin SHA256
5908DA4745DB86D3EF0ABE04CEE29525679A8778AA7139FD2DABC4A4317959F3.

IMPORTANT: new plugin is BUILT, NOT DEPLOYED, to leave the user's current
session running. DeployPlugin.ps1 with a fresh label at the next needed
restart. Installed plugin still6D14DDFC... and therefore starts at0.2 if
restarted before deployment. Current PID54228 has the one-third live override.
No runtime rebuild or deployment; D3D11 remains8E78E30F... .

Before latest preference, a frozen0.2/0.4 comparison produced
150157-sun-rebalance-02.png and150202-sun-rebalance-04.png, both visually
inspected. Fixed camera15133,-46900,650,pitch0.14998868,yaw0; autoexposure left
enabled,5second settle per frame. Sunlit surfaces change; black canopy persists.
The one-third value was applied after the freeze ended; no claim of a matched
one-third image.15:04 camera freeCamfalse,povvanity. No saves/settings written,
no deletes, no active build sessions. Exposure, sky, materials and other lights
unchanged. Full goal remains ACTIVE; tree transport and broader defects remain.

## Checkpoint 14:55 — native colour flag repaired and GPU verified

Previous turn PROGRESS identified truncation. This turn PROGRESS fixes and tests
the actual packed-record/GPU contract. Goal ACTIVE; no canopy-lighting claim.

NATIVE_SRGB_ALBEDO moved from type-relative14 (absolute bit16, discarded) to
type-relative8 (absolute bit10). Native foliage staysbit9; landscape/effect14/15.
No material struct/stride/offset changes: flagsuint16, record112bytes. Added
C++ width static_assert and Python all-opaque-flags width/collision check.
Dedicated test_material_layout now sets/clears the native colour flag through
actual writeGPUData and checks sampler index at byte2 remains1.

Reproduction: Python width test failed65536>65535; dedicated UnitTest build/test
session75354 exit1 with "Native albedo flag lost in 16-bit GPU field". Corrected
test75909exit0,1/1passed; logs test-native-colour-flag-{before,after}.log.
Runtime two-pass BuildRuntime35815exit0, build-native-colour-flag.log.
54Python tooling tests pass; new CompareNativeAlbedoScale.ps1 parser clean.
Scoped diff --check passes (line-ending warning only). No unit tests executed
from the Release game build directory.

OldPID28236 exited normally after baseline frozen captures144126-scale1 and
144129-scale05. Deployed D3D11 SHA256
8E78E30FDA395508E0341849E841A50336D59B33C7EED532ED3DDB2DA8B7DC50;
backup/symbol label20260920-native-colour-flag. DXGI unchanged
066D721569675B2172754F23A40DA3044E873B8B167A628A3534DA4780EF6788.
Host unchanged6D14DDFCFA1957EBE1B97C67D2E6D7E5FE50A8FF335DD2C8A3070D1D87E35EEF.
CurrentPID54228 started14:53:14, same existing Riverwood save loaded.

GPU verification in frozen camera15133,-46900,650,pitch0.14998868,yaw0:
145405-native-flag-after-scale1.png;145407-...-scale05;145409-...-scale1-return;
145412-...-lit, all inspected as applicable in .research/captures. Within-run
albedo23 ROI x450:1700,y200:950 RGB mean half/full ratios0.5101/0.5071/0.4983;
return/full0.99994/1.00018/1.00205. Before fix corresponding ratios were
1.0012/1.0004/1.0037 (scale had no effect). Read-only Pillow/NumPy analysis;
these are PNG ROI means, not exact raw-buffer parity or broad visual proof.
GPU response now demonstrates metadata survives to the shader. Default scale1
does NOT brighten the canopy, as intended. Retained single-decode cleanup must
remain: restoring old early decode with the now-working flag would darken imports.

Restored nativeAlbedoScale1,debug0,unfrozen/freeCamfalse.14:55:14 HUDonly/no modal,
POVfirst (saved view), native22draw/75compute suppressed. High/RTXDItrue,sun0.2,
tree-AOfalse,vertexStrength0.6,maps/thintrue unchanged. No new crashes, saves,
persistent settings or deletions. No active build/tool sessions.

Next: return to tree vertex-AO/transport investigation, without citing repaired
flag as the darkness cause. Native hardware-SRGB views and merge metadata still
need separate coverage; current proof is native UNORM. Skin self-occlusion and
all broader completion requirements remain open. Avoid repeated generic
brightness tuning or reinstating the rejected soft-texture-only tree mapping.

## Checkpoint 14:38 — rejected colour candidate; native flag truncation found

Previous turn PROGRESS: GPU SSS switches verified. This turn PROGRESS: tested and
rejected a tree-colour remap, added live texture-format inspection, and disproved
an apparent duplicate-decode darkness cause by finding a material flag truncation.
Goal ACTIVE. Canopy darkness, skin and full stability/navigation remain unfinished.

Tree-only trial removed BaseColor from soft/back scattering products (grass
untouched). Build14379exit0, logbuild-tree-scattering-colour.log; deployed D3D11
05B61D37097BE7CED7730490E363D83BFA0EBE1C7A9B1D8DD8B0C83CE97506FD.
PID41136, captures142230..142245-tree-scattering-colour-* inspected. Debug803
colour increases, lit canopy still dark and close to thin-off. Trial REVERTED
in source and deployment; preserves CS-derived base*soft/base*back equations.
Revert build84334exit0, logbuild-tree-scattering-revert.log, D3D11534D989B...,
intermediatePID38860. All process exits normal via verified Console+qqq; no new
device-loss crash observed this turn.

Host inspector now exposes lightingTextureViews diffuse/soft/back name, SRV
format/dimension. Runs ONLY on explicit inspect; no per-frame work/readback.
BuildDev29038exit0, logbuild-lighting-texture-inspection.log. Plugin SHA256
6D14DDFCFA1957EBE1B97C67D2E6D7E5FE50A8FF335DD2C8A3070D1D87E35EEF;
backup/symbol label20260920-lighting-texture-inspection. Live BSTreeNode entries:
TreePineForestBranchComp diffuse+soft format77 BC3_UNORM, dimensions4;
TreePineForestBarkComp diffuse71 BC1_UNORM; nativeFoliage true onlybranches.
SRV names empty, lightingMaterial.diffusePath identifies these assets.

Source appeared to decode native UNORM twice: once after sampling and again
after texture operations. Removed early decode; nativeAlbedoScale now follows
the existing final decode for non-grass/non-effect flagged imports. Build50842
exit0, logbuild-native-single-albedo-decode.log.53Python checks pass, including
3new source/arithmetic tests test_native_albedo_decode.py and a CS-product
contract in test_foliage_controls.py. These do NOT prove GPU conversion coverage.
Retained cleanup is deployed D3D11
A08243C33D44906A6C0E99AA3B5DE9F422FC6D196EC6D53731D334C955D6BA2D;
label20260920-native-single-albedo-decode. DXGI unchanged
066D721569675B2172754F23A40DA3044E873B8B167A628A3534DA4780EF6788.

CRITICAL correction: no visual lift from this cleanup. NATIVE_SRGB_ALBEDO is
1<<COMMON_MATERIAL_FLAG_TYPE_OFFSET(14) = bit16, but writeGPUData uses uint16_t
flags and shader OpaqueSurfaceMaterial.flags is uint16_t. The flag is discarded!
Therefore the early software decode NEVER ran; the current canopy was NOT
double-decoded through this branch. The earlier commentary inference was wrong
and was explicitly corrected. NativeAlbedoScale is also inactive until repaired.
This turn has NOT fixed flag packing. Do not widen flags without layout audit.
Available lower flag positions appear possible but require collision/packing
tests. Also distinguish imported UNORM versus hardware SRGB views explicitly;
ordinary generic gamma conversion still has its original SRGB-view TODO.

Captures143021 duplicate-decode-lit-before,143113 albedo-before versus143411
single-decode-albedo-after /143416 lit-after show no expected material lift.
All use15133,-46900,650,pitch0.14998868,yaw0, but different restarts/wind phases;
not pixel-identical A/B. Within frozen after-run,143419 normalized-tree-lit and
143421 normalized-tree-albedo again show partial AO/tint lift, dark undersides
persist. Tree-AO diagnostic restoredfalse,vertexStrength0.6,debug0,maps/thintrue,
High/RTXDItrue/sun0.2 unchanged. SSS/grass/water equations unchanged.

CurrentPID28236 started14:33:09, existing Riverwood save.14:37 normal scene
143719-colour-audit-restored.png inspected; time unfrozen/freeCamfalse. Idle
vanity overrides setPovthird, so latest readback is povvanity, not a verified
third-person restoration.14:37:18 native14draw/75compute suppressed. No saves,
persistent settings or deletions. No active build/tool sessions. Next: repair
and verify native colour flag contract, then continue tree AO/transport and
skin self-occlusion; do not claim the latent decode cleanup fixed brightness.

## Checkpoint 14:15 — verified live SSS controls; optical-colour lead

Goal ACTIVE. This turn is PROGRESS (diagnostic fixes built/deployed and GPU
verified), not a canopy-lighting fix. User's Lighting.hlsl vertex-AO lead remains
valid; prior normalization captures showed only partial improvement.

IMPORTANT correction to older checkpoints: the earlier thin-SSS-off comparison
was unreliable. Retained materials kept their SSS record, and the shader's thin
classification did not check the live enableThinOpaque flag. Native foliage also
ignored enableTextureMaps and retained generic texture indices survived toggles.
Do not cite 131932/131935 as proof that disabling thin SSS has little effect.

Runtime changes: SssArgs now32 bytes, enableTextureMaps at offset16 with C++
assertions; rtx_context sends that option every frame. Three generic SSS texture
reads and native foliage SSA override honor the live maps flag. Thin material
classification checks enableThinOpaque and explicitly maxSampleRadius==0, avoiding
misclassifying diffusion materials. Defaults still enabled; no change to native
tree/grass equations or accepted grass normals/water. These are global diagnostic
controls, not tree-only shading changes. Previous geometric ray-offset candidate
retained, still not fully regression-qualified.

Initial build-foliage-map-control.log failed (missing shared field), corrected.
Build sessions29219 and78556 completed exit0; final two-pass shader/C++ build is
build-foliage-map-control-3.log and includes all shader gates.49Python tests pass,
including3new source-contract tests in test_foliage_controls.py (not GPU proof).
Both repos diff --check exit0, with pre-existing access/line-ending warnings.
CompareFoliageControls.ps1 added: frozen-camera sequence with finally restoration;
PowerShell parser clean. No new unit binary run this turn.

OldPID44756 exited normally via Console+qqq, no new crash. Deployed backup/symbol
label20260920-foliage-live-controls. D3D11 SHA256
0EE0F6FB8DD51D6C974165668BCA4EE9C83CB6C825FC3D96646B3E0865C35D36;
DXGI unchanged066D721569675B2172754F23A40DA3044E873B8B167A628A3534DA4780EF6788.
Host unchanged61C79D1B487688987EC99352A538288645BA47ED5154B31009A2922FB816C1AB.
CurrentPID13968 started14:12:47, same existing Save1...20260920044800_1_1 loaded.

Matched frozen camera15133,-46900,650,pitch0.14998868,yaw0, captures inspected:
- 141344-foliage-live-controls-authored-lit: dark canopy baseline.
- 141347-...-constant-lit: neutral SSA0.5 noticeably lifts outer branches,
  but dense interior remains dark. Not a proposed gray-foliage production fix.
- 141349-...-constant-ssa: debug803 neutral gray across leaf surfaces.
- 141352-...-authored-ssa: same surfaces almost black, faint green authored colour.
- 141355-...-thin-off-selection: debug800 red/nonthin leaves, proves actual off.
- 141357-...-thin-off-lit: visually close to authored baseline, dark undersides.
- 141400-...-thin-on-selection: leaves green/thin again, bark red; reversible.
All paths .research/captures. These now validate the switches on GPU. The authored
scattering conversion contributes weak transmission; not proof it is the sole
cause, nor calibrated optical coefficients. Tree SSA currently derives
linear(base*soft)*grazingSoftWeight + linear(base*back), a raster-derived colour
used as an optical coefficient; review this mapping separately from vertex AO.

Maps/Thin restoredtrue, debug0; High/RTXDItrue/sun0.2/tree-AOfalse/tint0.6 unchanged.
Unfroze with tfc1 and verified freeCamfalse; third-person restored14:14:45.
141445-foliage-controls-restored.png inspected, normal scene and HUD. Wind timers
advance.14:14:56 log confirms14native draw/75compute suppressed. No saves or
persistent settings written, no file deletions. No active tool/build sessions.
Next: tree-only optical colour/transmission qualification, preserving grass;
existing skin self-occlusion and device-loss stability issues remain open.

## Checkpoint 13:58 — thin-ray offset candidate; canopy darkness remains

Previous goal turn PROGRESS: tree-only AO diagnostic and matched captures narrowed
the next action. This turn tested leaf visibility and built/deployed a geometric
ray-offset correction. Goal ACTIVE; no tree, skin, movement or stability completion.

PID34148 fixed canopy camera15133,-46900,650, pitch0.14998868,yaw0:
134645 lit reference;134648 distance806 with RTXDI;134738 distance806 fallback
RIS;134740 material800 confirms leaf thin selection (bark red/nonthin);
134743 fallback RIS lit remains dark. RTXDI restoredtrue. Debug806 with RTXDI
can have black/unwritten regions; fallback RIS reveals blocked rays there, so
do not read black as a measured visibility value. Many blocks >10units (blue),
also close red/yellow regions. Same-camera cutoff A/B134845(default254/255) and
134849(exact1) does not resolve darkness. Default threshold restored. These
results do not establish a complete optical-material cause.

Found rayCreatePositionSubsurface selected penetrateSurface using shadingNormal
while rayOffsetSurfaceOriginHelper offsets along minimal geometryNormal. Changed
the side decision to dot(minimalSurfaceInteraction.geometryNormal,direction).
This affects thin-surface visibility rays, including grass; it does NOT modify
grass normals/materials, nor ordinary opaque or RTXCR diffusion ray origins.
No added GPU texture reads or CPU per-frame work. Shader's shadingNormal argument
is now unused (can clean signature later). Existing unrelated edits preserved.
tools/remix/test_thin_ray_offset.py checks the actual shader expression and a
plane-intersection model: opposite signs produce forward self-hits under the old
rule; geometric-side offsets move those intersections behind the ray.46Python
tests pass;2existing dedicated UnitTest skinning/basis tests pass (--no-rebuild,
these are regression checks, not tests of the changed shader). BuildRuntime87074
two-pass shader/C++ build exit0. Build logbuild-thin-geometric-offset.log.

PID34148 crashed13:51:34 VK_ERROR_DEVICE_LOST BEFORE candidate deployment;
runtime still13366... at that point. No attribution to candidate. Archived logs
20260920-135134-pre-thin-offset-device-lost-{runtime,cs}.log and corresponding
gpu-crash-2026-09-20-13-51-34.nv-gpudmp in .research/testlogs. Second observed
pre-candidate device loss; stability remains open.

Candidate deployed label20260920-thin-geometric-offset, D3D11
FD1D834784568E7F3E4485100C0C13AD551EE867EE0A67261D4C74DB2E464BFE;
DXGI unchanged066D721569675B2172754F23A40DA3044E873B8B167A628A3534DA4780EF6788.
Host61C79D1B487688987EC99352A538288645BA47ED5154B31009A2922FB816C1AB.
CurrentPID44756 started13:55:42 and loaded same existing save.135650 candidate
canopy lit still dark;135653 candidate fallback RIS distance806 shows substantially
different near-hit distribution (many previous red leaf regions no longer red).
These use the recorded camera but DIFFERENT restarts/wind/game times; not a
pixel-identical controlled before/after. Physical plane-side invariant supports
the fix; full visual/regression qualification, including grass, is still pending.
Candidate retained, not described as a canopy transmission fix.

At13:57:52 verified freeCamfalse/povthird after tfc1 then delayed setPov; normal
time restored, debug0,RTXDItrue,High,sun0.2,tree-AOfalse,vertexStrength0.6.
135752 restored scene screenshot inspected. No active build/tool sessions.
No saved settings/save writes/deletions. Next investigate native leaf optical
coefficient mapping / multi-layer transmission and remaining close blockers,
with the ray-offset change kept separate from brightness or SSS calibration.

## Checkpoint 13:42 — tree vertex-AO experiment, not a canopy-lighting fix

User confirmed tree vertex colours contain AO and pointed to Lighting.hlsl's
anti-overdarkening tricks. Audited: Lighting.hlsl2536 extracts max RGB as linear
vertexAO, passes it to Skylighting; Skylighting.hlsli174 returns
saturate(skylightingDiffuse/max(vertexAO,EPSILON)). Lighting writes1-vertexAO to
Masks2 at2938; DeferredCompositeCS126 similarly divides SSGI AO by vertexAO.
TruePBR separately normalizes linear vertex RGB with adjustable VertexAOStrength
(Lighting1730). These are raster compensation, not directly transferable ray
visibility probabilities. Remix import had none of these compensations.

Added OFF-BY-DEFAULT host diagnostic cs.treeVertexBakedLighting. Mesh creation
classifies eligible lighting vertex-colour shapes beneath BSTreeNode (bounded
32-parent walk only on creation), covering bark as well as leaves. No filename
classification or change to grass/water/hair/landscape/effects/LOD atlases.
Toggle sets API isVertexColorBakedLighting and updates retained descriptions,
including static-probe hits; no mesh uploads. Inspector vertices: reports both
treeVertexColor and vertexColorBakedLighting. Invalid boolean rejected live.
Uses Remix's existing shader normalization; no runtime shader/DLL change.

BuildDev71731exit0;43Python tooling regression tests pass (not pixel proof).
Deployed plugin61C79D1B487688987EC99352A538288645BA47ED5154B31009A2922FB816C1AB,
backup/symbol label20260920-tree-vertex-ao. PriorPID28552 exited normally via
verified Console+qqq. CurrentPID34148 started13:37:23, same existing Riverwood
save loaded. No saved settings/save writes or unrelated file changes/deletions.

Matched frozen lit pair133828-tree-vertex-ao-off /133832-normalized-tint:
normalization modestly lifts bark/foliage, does NOT solve dark canopy.
Albedo23 pair133912-on /133914-off shows tree colour changes; same camera within
that pair (slight user yaw shift between lit and albedo pairs). Near-canopy
pair134007-tree-interior-ao-off /134010-on at15133,-46900,650,
pitch0.14998868,yaw0 inspected: brighter coloured foliage/trunk regions, black
undersides persist.134014 also tests default tint-strength0.6; default mixes
normalized tint with white, whereas strength1 preserves normalized chroma.
All paths in .research/captures. Not exact native albedo parity or proof all
darkness is AO. Next: qualify thin transmission/leaf visibility independently.

Restored tree diagnosticfalse, vertexColorStrength0.6, debug0; High and sun0.2
unchanged. Unfroze tfc1; immediate setPov raced queued console restoration and
left freeCam reportedtrue; camera freecam(on:false) fixed it. Verified13:41:53
freeCamfalse/povthird, HUDonly, wind timer advancing.13:41:27 native14draw/75
compute suppressed. No active build/tool session; goal ACTIVE, trees NOT fixed.

## Checkpoint 13:32 — AO/default audit; matched High/Ultra comparison

User reports generally excessive AO and asks whether performance settings are
responsible. No rendering fix or new deployment in this audit. Goal ACTIVE.
PID28552 startup Auto resolved High (1), not the older forced Medium. Runtime
log confirms RTXDI, ray reconstruction, first-bounce NEE, unordered indirect
resolve, post FX and volumetrics enabled. High uses max2 bounces versus Ultra4;
transmission approximation enabled on High, disabled on Ultra. Host import
overrides and requested directional scale0.2 remain, so not all-default Remix.
No explicit extra AO-strength override found; native DeferredPasses returns
when Remix world suppression is active.

Earlier captures132646/132655 INVALID for A/B: camera moved. User subsequently
held camera stationary. Captures133032-canopy-high-stationary.png and
133037-canopy-ultra-stationary.png in .research/captures were inspected and have
identical reported camera xyz13655,-48390,42.933334, pitch0.0193737,
yaw0.1647095. Ultra does not noticeably lift canopy/crevice darkness in this
view. This limited comparison is not general preset equivalence or FPS proof.
Runtime log13:30:32 confirms Ultra;13:30:37 confirms restored High.

Concrete source lead: RemixScene initializes InstanceInfoBlendEXT to zero and
never sets isVertexColorBakedLighting. API>=0.5.2 takes this false field instead
of global vertexColorIsBakedLighting default. Thus native lighting vertex RGB
is multiplied directly, bypassing Remix brightness-normalization for baked
vertex lighting. Live branch sample RGB115,143,75 (734/1422 nonwhite vertices),
bark samples107,116,100 and45,57,35 (817/1004 nonwhite red vertices) match uploaded
RGB. Tree animation alpha correctly replaced with255. This proves additional
albedo darkening, NOT that all colour variation is baked AO or that normalization
is the correct fix. A targeted comparison is still needed; do not globally strip
vertex colour, change accepted grass shading, or claim causality yet.

Restored game time via tfc1 and verified freeCamfalse/third-person at13:31.
High/debug0/sun0.2 retained. No persistent settings, saves or DLL writes.

## Checkpoint 13:23 — reduced directional light; skin self-occlusion identified

Goal ACTIVE. Latest user: reduce directional light for sun/ambient balance;
after comparison "maybe better, trees still look pretty dark"; asks whether
Remix has stronger transmission and specifically RTX SSS. Answer from source:
trees already use thin-opaque SSS, skin uses RTXCR diffusion + single-scattering
transmission. These are distinct models, not a simple stronger/weaker switch.
No new tree material change implemented. Water/grass normals/aniso untouched.

Production change: host directional radiance multiplied0.2 (80% reduction),
process-local `cs.directionalLightScale` calibration0..16. Dome/point lights/
exposure unchanged. Applies interior directional templates too. Tests13scalar
plus invalidscale bounds and5live Riverwood CPU/API samples pass. Same-camera
1/.2/.1 captures inspected; retained.2. User response above, not visual parity.
See remix-directional-light.md for details. BuildDev24209exit0. Plugin SHA256
88B7D119623CE2C982B607A0A3B404F99793CF8BCECCD8BCB0B8100F4CA6542D,
backup/symbol label20260920-directional-balance.

Runtime now has opt-in direct visibility debug805–807 (not a shading fix).
Build89722exit0, first75275failed duplicate macro declaration corrected.
D3D11 SHA25613366B93F8F6508DD5C3629EB16D1CAB0E203D73DF8C6598BFFE583E6E6B8033;
DXGI unchanged066D721569675B2172754F23A40DA3044E873B8B167A628A3534DA4780EF6788.
Backup/symbol label20260920-direct-visibility.43Python tooling tests pass.
Debug806 body patches hit within0.1–1game units, some1–10; debug807 matching
patches hit SAME body surface. Strong self-occlusion evidence, numerical cause
still unknown. Directoff removes patches; RTXDIoff does not. See remix-skin.md.

OldPID9608 lost GPU device13:05:45 BEFORE deploying new runtime; original46A...
runtime. Runtime/CS logs and Aftermathdump preserved in .research/testlogs.
Do not attribute this unresolved crash to not-yet-deployed diagnostics.
NextPID11236 exited normally via verified Console+qqq for host replacement.

CurrentPID28552 started13:14:45, same existing Riverwood save. At13:23 restored
third-person/freecamfalse and game time after frozen tests; HUDonly/no modal.
Debug0,RTXDItrue,directtrue,thinSSStrue,diffusiontrue,sunscale.2. Native world
suppression configuration active, no saved graphics/save writes or deletions.
No active tool/build sessions. Camera/player may have been moved by user; do
not assume original saved placement. No character/tree correctness claim.

Canopy debug800/803/23 captures131723/131724/131726 inspected: thinselection
active; native tree scattering colour very weak. BSTreeNode census confirms
pine branch material flags34(soft+gamma), softSRVvalid,rolloff8; bark nonthin.
Canopy thin-on/off131932/131935 samecamera inspected: disabling thin does NOT
resolve dark canopy (some small differences), restoredtrue. No calibrated
tree material fix established. Next qualify tree SSS/ambient inputs, and skin
self-shadow origin/geometry consistency separately. Do not keep reducing sun
to mask these defects.

## Checkpoint 12:54 — MSN lighting experiments failed; reverted cleanly

Experimental/live-evidence PROGRESS, NOT a character fix. Goal ACTIVE. Water,
grass normals and anisotropic filtering unchanged. No net production shader or
host change retained from this turn. Details/evidence in remix-skin.md12:52.

Tested actual normal-map-derived vertex directions for bounded shadow offsets,
including their transfer to diffusion visibility rays. Debug279 proved offsets
active, but same-pose enabled/disabled pairs retained angular skin patches.
Then used those directions as smooth interaction normals; debug15 proved the
GPU field changed, but the lit patches remained. Both candidates were removed
instead of retaining extra texture reads without demonstrated visual benefit.
This does NOT rule out all normal-bending/self-intersection hypotheses.

Build22627/26314 (offset),57050 (smooth),5019 (revert) all exit0 through two-pass
BuildRuntime.43Python tests + dedicated skinning_basis/native_basis_vertex2/2 pass
(regression scope only). Candidate DLLs/symbols/logs/captures archived with labels
20260920-msn-terminator and20260920-msn-smooth-normal. Revert build completed12:53;
no running build/tool handles. Source diff checks pass with existing warnings.

Exact original deployed D3D11 restored from verified backup:
46A77FF560575E8CCC2DE091C2A20456DA6984076C3BB0726B438B761E9378B5.
DXGI066D721569675B2172754F23A40DA3044E873B8B167A628A3534DA4780EF6788.
Plugin unchanged9F6E970792C9F3584D19A4C0658B0BA39EDB2A2BC384B3AEB8FD15F172799710.
OldPID40220/53468/15792 exited normally through verified Console+qqq before writes.
CurrentPID9608 started12:51:39, normal launch/configure, same existing save. No
native reference/freeze/diagnostic quality overrides. Capture125326 inspected:
third person/freecamfalse, world and HUD. Input17notheld. At12:53:52 native
14draw/75compute suppressed. No saves, persistent settings or unrelated deletes.

Next: isolate character light visibility and normal bending with actual rendered
buffers/rays. Large patch boundaries were absent in debug23albedo. Disabling
diffusion altered the back crease but did not remove the polygonal boundaries.
Do not retry these shader candidates without new evidence or call jitter fixed.

## Checkpoint 12:29 — native movement comparison; normal Remix restored

Live diagnostic PROGRESS, no production animation/shading fix. Goal ACTIVE.
Accepted water, grass normals and anisotropic filtering remain out of scope.
No binary/source changes or deployments in this comparison step.

Native movement bursts captured/inspected: PID26712 frames15027–15090/384.909ms;
PID44372 with process-local DXVK_FRAME_RATE40 frames2386–2449/1574.405ms. The second
is similar duration to Remix's1596.701ms. Both64frame sequences pass integrity;
broad body transition similar, no obvious T-pose. NOT phase-aligned native parity
or presentation qualification. See remix-animation.md for artifacts/limitations.

Skin shading audit found MSN bypasses ordinary smooth-vertex terminator offset.
Live nearby feature5 meshes use descriptor0006300065000409 with NO native vertex
normals/tangents; orientation-basis columns are not substitutes. Candidate cause
only, no speculative shading patch. See remix-skin.md for exact evidence.

Both native processes exited normally via verified Console+qqq. CurrentPID40220
started12:27:08, normal LaunchTest/ConfigureRunningTest, no40FPS cap/native reference/
SkinProbe. Same existing Riverwood save loaded. Capture122813 inspected: third person,
freecamfalse, world/HUD visible. Native14draw/75compute suppressed at12:28:24.
No saves or persistent graphics settings changed, no unrelated deletes. Plugin/
runtime hashes unchanged from12:14. No active builds/tool handles/capture bursts.

Next: establish a valid MSN smooth-normal source and controlled self-shadow A/B;
continue character/presentation stability work. Do not call jitter fixed from
these short rendered-frame sequences.

## Checkpoint 12:14 — consecutive rendered-frame animation evidence

Implementation/live validation PROGRESS, not a production animation fix. Goal
ACTIVE. Accepted water/grass normals/aniso unchanged. Next substantive work:
repeat controlled Forward movement in native reference to compare body motion,
and independently qualify actual presentation pacing. See remix-animation.md.

Added opt-in communityshaders.capture kindsequence frames2..64. Full SDR
kFRAMEBUFFER copies before Present/State::Reset, per-frame sidecars, bounded
512MiB nominal pixels, PNG mapping/encoding after all burst copies queued.
No notification/clipboard pollution; overlap rejected. Initial inWorld-pass gate
incorrectly rejected requests, fixed to loaded-world/nonloading state. Default
idle; no changes to production animation or graphics settings. This is not a
WSI/scanout capture or an unperturbed performance benchmark.

BuildDev44448/14373 exit0. Backup/symbol label20260920-frame-sequence-world.
Current plugin SHA256:
9F6E970792C9F3584D19A4C0658B0BA39EDB2A2BC384B3AEB8FD15F172799710.
D3D11 unchanged46A77FF560575E8CCC2DE091C2A20456DA6984076C3BB0726B438B761E9378B5.
DXGI unchanged066D721569675B2172754F23A40DA3044E873B8B167A628A3534DA4780EF6788.
All predecessor games exited normally via Console+qqq before owned-DLL overwrite.

Current PID44588 started12:05:58, normal Remix launch, SkinProbeoff. Three actual
captures:32frames2144–2175 stationary,64frames3700–3763 stationary,64frames8472–8535
moving (1.597seconds). Raw game Screenshots folders end12-06-42_453-sequence-44588,
12-07-21_273-sequence-44588,12-09-29_866-sequence-44588. Reports/contact sheets in
.research/testlogs/20260920-third-{idle-sequence,sequence,moving-sequence}.{json,png}.
All complete/consecutive (capture integrity ONLY). Moving sheet/full8512–8514
inspected: body posed, no obvious T-pose swap, but large body changes/angular skin
remain. Distant-cliff phase correlation dx0..1/dy-2..0; not general stability proof.

Computer-use skill's sky C/W key presses did not move the player; no locomotion
claim from that attempt. DevBench Papyrus describeInput exposed HoldKey/ReleaseKey;
GetMappedKeyForward0 returned17. Hold17/capture/release17 in finally moved the
player, and IsKeyPressed17 verifiedfalse. Existing saved Riverwood location was
reloaded after the test, no saves written. At12:13:54 samePID loaded/HUDonly,
native22draw/75compute suppressed, no modal. Camera may enter idle vanity later.

43Python tests pass including sequence completeness rejection and signed synthetic
translation; diff --check passes with line-ending warning. No active build/tool
handles or capture bursts. CS log archived20260920-frame-sequence.log. No unrelated
deletions or persistent settings writes. Do not call jitter fixed from these tests.

## Checkpoint 11:53 — bounded GPU skinning check

Diagnostic/live validation PROGRESS, not an animation fix. Goal remains ACTIVE.
User-accepted water, grass normals and anisotropic filtering unchanged. Next:
native pose/camera timing, tracing consumption and presentation continuity.

Added opt-in `LaunchTest.ps1 -SkinProbe`: twelve asynchronous readbacks of 32
vertices from one indexed24-bone basis geometry, compared with CPU evaluation
of the captured palette/input. Runs after upload/skinning flush, before tracing;
default off and stops after12. The selected hash is not a proven player identity.
Initial diagnostic omitted per-attribute slice offset and correctly failed the
normal gate; fixed the readback only. Initial failing log retained. No production
deformation changes or threshold relaxation. See remix-animation.md.

Corrected PID53360 samples runtimeframes819–878 pass: maxpositionerror1.52588e-5,
maxnormalbasiserror1.19209e-7,12distinctbonehashes,nonFinite0,observedmovement.
Evidence:20260920-skin-probe-offset.{log,json} in .research/testlogs. This does NOT
prove native pose freshness, every-frame animation, actual BLAS contents,
first-person correctness or presented pacing. Capture115040 shows world/HUD,
not the selected mesh. Source audit found changed host palettes resubmitted with
retained revisions; no missing palette update was established.

BuildRuntime8954/83791 exit0. Runtime backup/symbol label
20260920-skinning-probe-offset; deployed D3D11 SHA256:
46A77FF560575E8CCC2DE091C2A20456DA6984076C3BB0726B438B761E9378B5.
DXGI unchanged066D721569675B2172754F23A40DA3044E873B8B167A628A3534DA4780EF6788.
Plugin unchangedA71A0444F14BA4BF030D548638CEB89B071084360EB7FB868F3D7CC9BE063C36.
38Python tests pass; LaunchTest parser and runtime diff --check pass.

Old diagnostic processes exited via verified Console+qqq; absent before launch.
Current PID42540 started11:51:44, default LaunchTest/ConfigureRunningTest, same
existing Riverwood save. SkinProbe disabled,0records in current log. Third person,
freecamfalse;HUDonly/nonmodal. Inspected115251-skin-probe-disabled-third.png.
Native world14draw/75compute suppressed at11:52:50. No active build/tool handles,
no unrelated deletions, no save or persistent setting writes. Jitter remains open.

## Checkpoint 11:37 — RTX skin diffusion and secondary-hit normals

Implementation/live GPU PROGRESS. Full goal ACTIVE. Accepted water/grass normals/
anisotropic unchanged. Character animation continuity and skin calibration remain
open; next work must not return to accepted grass/water diagnostics.

FaceGen/FaceGenRGBTint now route to RTXCR diffusion via new resident-texture API
csRemixCreateSkinMaterialD3D11V1. Authored-space composed FaceGen albedo supplies
transmission colour; sRGB transmission views are rejected to avoid double decode.
Radius(.5,.5,.5)/scale1/max16 are Remix defaults, NOT calibrated Skyrim values.
Native _sk is not a physical radius map and is not yet mapped. Hair/MSN-only
non-skin classification unchanged; cached successful skin-import status survives
facial morph mesh replacements. See remix-skin.md for exact limitations.

Initial diffusion lit capture112715 visibly faceted skin. Found traceSssRay used
geometric normals for secondary-hit lighting, bypassing MSN texture/bone basis.
It now evaluates the material shading normal, preserving geometric ray offsets;
misses return before buffer indexing. The shared normal also feeds transmission
boundaries. New lit113252 shows smoother shoulders; angular shadows remain, and
idle pose/light differences prevent a pixel A/B or complete appearance pass.

BuildDev39626 exit0. Runtime90509 (API),8514 (shader),81532 (final annotated shader)
all exit0 via two-pass Meson. Final embedded header11:31:23, owning object11:31:29,
D3D11 DLL11:31:34 verified. Owned backup/symbol labels:
- CS20260920-skin-diffusion:
  A71A0444F14BA4BF030D548638CEB89B071084360EB7FB868F3D7CC9BE063C36
- Runtime20260920-skin-diffusion-normals D3D11:
  2683F2CD4049447D1D89E232FDEBE995F41401FF717D1737B0BC5ACA42823FB6
- DXGI unchanged066D721569675B2172754F23A40DA3044E873B8B167A628A3534DA4780EF6788.

Current game PID39504 started11:32:06, default LaunchTest (no sampling/quality
overrides), same saved Riverwood location, third-person/freecamfalse at capture.
NativeReferencefalse/suppressWorldtrue; native14draw/75compute suppressed at11:33,
HUD visible. Old PIDs44152/50080 exited via Console+qqq, absent before deploy.
Debug801 GPU mask113345 inspected: exposed skin green, hair/scenery red. Debug0
restored. Actual evidence under .research/captures and .research/testlogs:
20260920-skin-normal-fix-routing.json:18queries frames1092..1148, 15FaceGen/41RGBTint/
29Hair records, all active matching skin imports true/hair false. Hair regression
also passed.20260920-skin-viewmodel-routing.json:12first/third samples pass ONLY
host camera/category lifetime, not animation. No temporal/jitter pass claimed.

30Python buffer tests pass. Dedicated _Comp64UnitTest material-layout test extended
to verify diffusion flag/per-channel radii/scales survive API conversion; rebuilt
64613 exit0 and material_layout+skinning_basis2/2 pass. Final PS parser/diff checks
pass with environment/line-ending warnings. No active build/tool handles. No saves,
unrelated deletes or persistent settings writes. Three pre-existing failed uploads
still logged. Full character fidelity, calibrated skin and motion remain open.

## Checkpoint 11:21 — character hair-card routing deployed

User acceptance of water, grass normals and anisotropic filtering remains in
force. No changes to those paths/settings. This turn implementation/live test
PROGRESS; goal remains ACTIVE. Next: character skin diffusion and animation
continuity, not grass/water.

RemixScene now tags native HairTint geometry with the public HAIR_CARDS category,
preserving VIEW_MODEL/THIRD_PERSON_PLAYER_MODEL and effect flags. The runtime
already converts that category and implements hair-card mip/roughness and cutout
handling. This is NOT a dedicated hair-fiber BRDF or a full hair appearance pass.
Skin still uses the generic opaque importer, with no diffusion-profile mapping.
Read-only census adds hairCardsRequested and submittedBones. No runtime changes.

BuildDev58957 exit0; owned plugin backup/symbol label20260920-character-hair-cards.
Deployed CS SHA256
06BE7A4A5FB247B570D16C98C3F6805F72E0EF0D39CF22D241CB86CAA092083C.
Runtime hashes unchanged from11:10 below. Old PID38484 exited normally via
Console+qqq; absent before deployment. Current PID44152 started11:18:07 with
normal LaunchTest/ConfigureRunningTest, no diagnostic/quality overrides. Same
existing Riverwood save loaded, third-person selected, freecamfalse. Idle vanity
can subsequently take over; setPov alone did not defeat active vanity in the old
process. The old 111641-character-hair-before.png was vanity, NOT a matched A/B.

Inspected .research/captures/111909-character-hair-cards-third.png: player/back/hair,
world and HUD render. Same saved third-person camera as110939 baseline, but no
pixel or temporal appearance pass claimed. Native suppression logs14draw/75compute
at11:20; three existing failed uploads remain, unrelated to this change.

CheckCharacterRouting.ps1 initially correctly rejected feature:6:52matches exceed
the48-entry census limit. The gate remains. Explicit complete bounded name subsets
then passed six advancing-frame samples each: Hair29 (frames4023..4065), Brows13
(4076..4114), Beard10 (4123..4161). Every submitted matching entry must still be
feature6, imported, hair-tagged, not thin foliage; each sample must include GPU
bone palettes. Reports .research/testlogs/20260920-character-{hair,brow,beard}-routing.json.
These prove HOST routing for the recorded subsets, not GPU category readback,
fiber scattering, motion continuity or native parity.30 Python buffer-analysis
tests pass; PowerShell parser and git diff --check pass (environment/line-ending
warnings). No active builds/tools, no saves or unrelated deletes.

## Checkpoint 11:10 — accepted grass/water; character work next

User direction supersedes historical open-item lists: anisotropic filtering is
fine; grass normals are fine; water is working. Move on from all three. Do not
reopen them because older diagnostic numbers or screenshots are imperfect.
Character rendering/animation remains the next focus; the full goal is ACTIVE.

Matched native/RTX primary samples now work through the diagnostic-only native
world camera cache-construction hook (RVA0x656ee4 -> 0x101c8f0), not the later
SetCameraData hook. See remix-normals.md for evidence and limitations. Two actual
23-artifact pairs at .research/buffers/grass-bank-pair-cache-construction-1789898111
and grass-bank-pair-cache-construction-repeat-1789898151 pass analytic matching:
maximum sample displacement about 0.000025 pixels. This is registration evidence,
not a normal/albedo/depth parity pass. Bounded raster audit found zero grass
depth bias/slope and native trilinear sampling. A tentative trilinear importer
change was reverted and rebuilt; it was NEVER deployed. Keep 8x anisotropic.

Plugin built/deployed with owned backup/symbol label
20260920-capture-viewport-metadata, SHA256
8D8989EA6C1E4F117D35212760255ED7C49C2D64981BB7F97FA500C7FF208489.
Runtime remains label20260920-matched-capture-samples, D3D11 SHA256
A3A9CA649A99391D47EECC01F8176BECF2054E87E56BCD5ACC1C9D2F5FFB237A;
DXGI066D721569675B2172754F23A40DA3044E873B8B167A628A3534DA4780EF6788.

Normal launch PID38484 at11:08:19, no native-preparation, matched-sample, or DLSS
quality overrides. Scene/nativeReferencefalse/suppressWorldtrue. Existing save
Save1_2B23D269_0_73647364_Tamriel_000002_20260920044800_1_1 loaded; freecamfalse,
third-person selected. GrassLighting restoredfalse before diagnostic process exit.
Inspected .research/captures/110939-character-third-person-baseline.png: player,
world and HUD visible. A still is not animation validation. No saves or unrelated
files deleted. No active builds at this checkpoint.

Character audit: GPU bone palettes are already submitted. Host skin/hair still
uses generic opaque materials; the existing runtime HAIR_CARDS category affects
texture mip bias/roughness, not a dedicated hair-fiber scattering model. Native
SKIN classification is FaceGen/FaceGenRGBTint, NOT every model-space-normal mesh.
RTX API already supports opaque subsurface diffusion profiles; authored-input
mapping and live character validation remain outstanding.

## Checkpoint 10:19 — actual uploaded-camera registration deployed and verified

Previous turn PROGRESS; this turn implementation/live experimental PROGRESS.
Runtime rtx_context.cpp now writes a depth-associated .dds.camera.json sidecar
only on G-buffer capture. It serializes RaytracingOutput::m_raytraceArgs.camera,
runtime/rng frames, jitter, extent/resolution and column-major float32 matrices.
Also captures gbufferWorldNormals before composite, keeping original final
worldNormals. First live sample's two normal buffers are byte-equivalent in all
packed pixels. CaptureBuffers.ps1 now requires/copies the associated sidecar;
successful pairs have23artifacts. No per-frame capture overhead when inactive.

Runtime build25535 exit0 through two-pass BuildRuntime.ps1. Owned runtime backup/
symbols label20260920-ray-camera-capture. Deployed D3D11 SHA256
A8B7C54097768A14D9B1E4C7854406B8CE551B22ADF461F966572772B94609E7;
DXGI unchanged066D721569675B2172754F23A40DA3044E873B8B167A628A3534DA4780EF6788.
Initial PID45024 capture grass-bank-pair-ray-camera-1789895380 (host/runtime895)
correctly refused analytic matching: native view is relative, RTX absolute.
Host BufferCaptureMetadata now adds actual shadowOrigin at each capture boundary.
BuildDev27392 exit0, plugin backed up/deployed label20260920-ray-camera-origin,
SHA256 C2B82AFA239FEFD14C21AAC404D4D2DA6238A9C1AC894678E3361BD08D40D644.

CameraBuffers.py restores native world translation with host-equivalent float
arithmetic; validates view equality, matrix inverse/layout and buffer dimensions;
then projects RTX rays to native pixels analytically, without looking at images.
JSON9digit floats must round-trip through float32 before double analysis. An
initial2.5e-5 apparent translation mismatch was decimal parsing, confirmed zero
after float32 recovery; no view tolerance was relaxed. Synthetic tests include
that actual serialization issue. All29reader/registration tests pass, capture
PowerShell parse passes, both worktree diff --check commands pass with warnings.
CompareBuffers/ProbeBufferRegistration prefer pre-composite normal capture and
record provenance; older captures still use final normals explicitly.

PID52084 started10:13:16 with -NativePreparation, default quality; same Riverwood
save/bankcamera, temporary GrassLightingtrue. Two actual23-artifact pairs:
- grass-bank-pair-ray-origin-1789895624, host/runtimeframe876.
- grass-bank-pair-ray-origin-repeat-1789895761, host/runtimeframe5847.
Both under .research/buffers; analytic reports grass-bank-pair-ray-origin-registration.json
and -registration-repeat.json. Runtime/host frame agreement is observed here,
not a general assumed identity. Analytic native-pixel shifts[-.4296875,.8981481]
and[.609375,.4722222]. First67597fixed grass pixels p90 improves92.459->72.881deg;
61155depthmatched registered p50/p90=.269/66.004deg (unregistered1.249/81.688).
Repeat67499fixed pixels p50/p90=.331/73.116; 61016depthmatched .274/66.059deg.
No normal parity pass: large tails remain. Nearest native pixels still represent
different subpixel rays; matching actual sample locations and alpha/mip coverage
is the next causal check, before attributing everything to normal transport.
Do not fit per-pixel normals or treat filtered subsets as the success criterion.
Detailed limitations and source pointers in remix-normals.md.

After pairs, normal native GPU suppression resumed~2606-2607draw/78compute;
diagnostic CPU section~3.23ms. No performance pass. GrassLighting restoredfalse,
normal Console+qqq exit, PID52084 verified gone. Relaunched WITHOUT startup
diagnostic/quality overrides: PID16772 at10:18:06, Scene/nativeReferencefalse/
suppressWorldtrue. Existing save loaded, freecamfalse/first-person restored;
inspected `.research/captures/101848-ray-camera-restored-normal.png` with HUD.
Camera saved pose(13665.155,-48229.410,-148.007), pitch.157043/yaw-.080082.
debug0/capturefalse restored by harness. No active build/tool sessions, no saves,
no unrelated deletes or persistent settings changes. Full goal remains ACTIVE.

## Checkpoint 10:02 — sampling probe and native-resolution pairs; residual errors

Previous turn PROGRESS; this turn diagnostic implementation/experimental PROGRESS.
No renderer changes, builds or DLL deployments. Added ProbeBufferRegistration.py
and five synthetic tests; full reader/comparison/registration suite24/24 passes.
Depth-only single-translation calibration on fixed rock pixels, then independent
grass/ground/stump normal evaluation. No per-pixel matching or normals in fit;
reports expose both all fixed samples and depth-filtered samples. Invalid depth
aborts; exact bank camera/paired identity/GrassLighting/dimensions are validated.
See remix-normals.md sampling section for formulas, report paths and limitations.

Reprocessed prior PID6116 pairs2050/5981. Rock-depth-fit shifts[-.5,.5]/[.625,.25]
native pixels reduce held-out fixed-grass normal p90 from92.226/84.085deg to
75.126/73.082deg; depth-matched p90 from81.550/70.287 to66.279/66.041deg. Thus
global sampling offset accounts for part of mismatch, but large residuals remain.
This is inferred translation, NOT actual RTX camera/jitter measurement or parity.

Normal PID39484 quit via Console+qqq and verified absent. Launched PID18100 at
09:56:49 with -NativePreparation -DlssProfile5 (startup-only; no live resizing or
saved quality changes). Existing Riverwood save, temporary GrassLightingtrue,
same bank camera. Two21-artifact pairs actually have1920x1080 native AND RTX:
`.research/buffers/grass-bank-pair-native-resolution-1789894649`, frame1570;
`grass-bank-pair-native-resolution-repeat-1789894714`, frame2917. Both populated
and identity-validated. First inferred shift[.125,-.5] changes no nearest pixel
because of half-pixel tie: grass depth-matched p90 stays78.216deg. Repeat[1,-.5]
changes all nearest pixels: grass p90 only74.090->73.667deg; fixed-grass p90
worsens86.464->86.935deg. Native resolution alone doesn't resolve the mismatch.
Reports explicitly expose nearestSampleChangedFraction; no interpolated normals.
Inspected no temporal sequence for jitter in this experiment; no claim of visual
or traversal pass. Logs after pairs resume~2643draw/78compute suppression.

Next substantive action: capture actual runtime ray-camera projection/jitter,
runtime frame/resolution alongside the raw DDS, then analytically register rays.
Current host metadata explicitly holds shadow matrices, NOT submitted RTX camera.
Native TAA jitter is present; host removes it before camera submission, runtime
RtCamera::calcPixelJitter applies its own Halton sequence. Do not infer runtime
frame index from host frame or timestamp filenames. Coverage, derivatives and
normal transport may still be wrong after correspondence is known.

Restored temporary GrassLightingfalse and quit PID18100 normally. Relaunched
DEFAULT settings, no -NativePreparation or -DlssProfile override: PID14844 at
10:00:05. Scene/nativeReferencefalse/suppressWorldtrue, same save loaded, camera
freecamfalse/first-person at saved pose(13665.155,-48229.410,-148.007), pitch.157043,
yaw-.080082. Final screenshot `.research/captures/100139-registration-diagnostic-restored.png`
inspected, scene/HUD visible. Capture harness restored debug0/capturefalse.
No active tool/build sessions; no saves, unrelated deletes or persistent settings
writes. Full goal ACTIVE; grass/other surfaces and broader requirements unfinished.

## Checkpoint 09:50 — populated same-frame pairs; normal mismatch remains

Implementation and experimental PROGRESS, no fidelity pass. Startup-only
CS_REMIX_KEEP_NATIVE_PREPARATION=1 (`LaunchTest.ps1 -NativePreparation`) keeps
native shadow/world CPU preparation and cleanup alive from load. Independent
low-level native draw/dispatch suppression remains active on normal frames;
DeferredPasses also explicitly skips its direct compute compositor when world
GPU work is suppressed. A paired frame allows the original native routines.
cs.captureBufferPair now rejects default processes without startup preparation;
both capture metadata halves and the comparison validator require
nativePreparationtrue. No nativeReference lifetime guard was bypassed.

Build66242 exit0; owned plugin deployed with backup/symbol label
`20260920-native-preparation-probe`, SHA256
6DE64801F4441A133B86586D323E11A6DE4E206A32E12662B14CD861B192F2EE.
Runtime unchanged: D3D11 76E02DB12B1EAFA79158D60F2E56011EE13CB8E5A2FBED3E4562C58277B7C34F,
DXGI 066D721569675B2172754F23A40DA3044E873B8B167A628A3534DA4780EF6788.
PID6116 launched09:42:25 with -NativePreparation; existing Riverwood save,
GrassLighting temporarily enabled, bank camera(13000,-47300,650), pitch-.450005.
Two successful populated21-artifact pairs:
- `.research/buffers/grass-bank-pair-prepared-1789893796`, frame2050.
- `.research/buffers/grass-bank-pair-prepared-repeat-1789893908`, frame5981.

Both native/RTX metadata halves agree on PID/request/frame and preparation;
native saves HRESULT0 and RTXqueuedtrue. First native depth ranges.981634-.997965,
albedo0-.922776, normals0-1: actual geometry, unlike the earlier clear targets.
Comparison reports beside capture directories report sameHostFramePairtrue.
Ordinary grass first53745depth-matched pixels: normal p10/p50/p90/p99
.105/1.232/81.551/121.862deg; repeat57304pixels .085/.599/70.278/117.243deg.
Rock p50/p90 first8.663/30.376deg, repeat6.429/25.629deg. Large errors remain;
same-host-frame is NOT exact GPU sample/ray correspondence or a fidelity pass.
Continuous-normal subsets remain diagnostic only. Pixel jitter/footprint,
coverage/overlapping blades, texture derivatives and PSR boundaries need work.

Normal-frame native GPU suppression resumed after both pairs, logs2610-2686draw
batches/78compute at bank (about4840/78 at saved first-person pose). Native CPU
section adds~3.25-3.36ms at bank, ~4.7-5.4ms first-person. Diagnostic framecounter
measurement34.83fps is NOT comparable to the old34.93fps result taken at another
pose during a build; GPU timing samples absent. No performance pass claimed.
Grass native/independent wind audit7200samples maxerror1.9073486e-6. Gamma/Authored
foliage import census passes23grass/10leaves/12bark; report
`.research/testlogs/20260920-native-preparation-foliage.json`.
Reader/comparison19/19 tests pass, PowerShell parse checks pass.

Rechecked user's Lighting.hlsl point: same smoothstep-difference equation as
RunGrass, different rolloff inputs. Grass scattering tint is albedo squared;
tree soft tint is base times rimSoftLightColor. Current runtime still uses only
native grazing weight with the RTX thin-SSS angular lobe, NOT full angular parity.
Inspected `.research/captures/094315-native-preparation-bank.png`: visible scene
and HUD, not proof that foliage/water/other defects are solved.

Temporary GrassLighting toggle restoredfalse before normal Console+qqq exit.
PID6116 verified gone. Relaunched without -NativePreparation: PID39484 at09:50:27.
Normal-view recovery verified: Scene/nativeReferencefalse/suppressWorldtrue;
same existing Riverwood save loaded, GrassLighting not loaded, freecamfalse and
first-person. Inspected `.research/captures/095141-paired-reference-restored-normal.png`:
scene/HUD visible at saved pose(13665.155,-48229.410,-148.007), pitch.157043,
yaw-.080082. Logs09:52 show22draw batches/75compute suppressed; normal world CPU
section~1.21-1.26ms. Game left running PID39484; no active tool/build sessions.
No unrelated deletes/save writes. git diff --check passes (line-ending warnings).
Full goal remains ACTIVE. Startup preparation is a diagnostic finding, not a
validated all-traversal fix or an approved permanent CPU-performance regression.

## Checkpoint 09:38 — paired capture exposed native preparation failure; NOT usable

Previous turn implementation/GPU PROGRESS. This turn diagnostic implementation
and crash/localization PROGRESS, no fidelity pass. New cs.captureBufferPair
reserves a uint64 request with bit32 marking paired mode; world latch allows
one native frame, end-deferred saves native buffers, BeforeUI queues RTX only
after successful same-frame native saves. NativeReference guard unchanged.
Hooks collect grass setup metadata on paired frames too. Script -Mode Pair
copies both halves and validates PID/request/frame; comparison validates paired
acknowledgement.19reader/comparison tests pass (three new pair-identity tests).
Capture script cleanup now warns rather than hiding the original error if the
game exits. Build89542 exit0 and deployed first plugin:
F2105D182F68628E36EA4DE1BECA6A4FB5BE74D0105F523B209102C9BEB6080C,
backup/symbols `20260920-paired-buffer-capture`.

PID34148 loaded existing Riverwood save with GrassLighting hot-enabled. First
paired request1789893018 atframe1966 CRASHED immediately09:30:19. Process verified
absent; capture15655 terminated exit1. Preserved artifacts:
`.research/testlogs/20260920-paired-world-crash.dmp` (1627958bytes) and .log.
CDB with matching PDB locates fault SkyrimSE+0x156016a, call[rax+0x10], RAX0,
RDX0xc066; RCX0x193d8833e20 points into memory containing a GenericBehaviors path,
not a valid shader vtable. Stack includes EngineFixes+0x39fde and
Deferred::Hooks::Main_RenderShadowMaps::thunk+0x22, then WorldFrame::thunk.
This resembles the older native-resume/cell-transition stale-render-pass crash.
No pointer patch, exception swallowing, or unrelated mod removal was attempted.

Revised paired diagnostic keeps Main_RenderShadowMaps skipped because captures
are pre-lighting; normal suppression unchanged. Build93032 exit0. New plugin
B38B576AE9DD3F0E791F37F1643048AE02BC71A1DD806AA306D3523C7040AB04,
backup/symbols `20260920-paired-gbuffer-no-shadows`. Runtime unchanged from09:22
(D3D11 76E02DB12B1EAFA79158D60F2E56011EE13CB8E5A2FBED3E4562C58277B7C34F).
PID51584 started09:34:21, Scene/nativeReferencefalse/suppressWorldtrue, same save
loaded; GrassLighting enabled for tests only. Two paired captures survived:
`.research/buffers/grass-bank-pair-no-shadows-1789893300` (frame1320) and
`grass-bank-pair-repeat-1789893377` (frame4045), each21artifacts, same bankcamera.
Both metadata halves agree on request/PID/frame, pairedtrue, native saves HRESULT0,
RTXqueuedtrue, GrassLightingLoadedtrue. HOWEVER all native albedo/normal values
are0 and depth is uniformly1 in BOTH attempts. Comparison correctly failed
`No approximately depth-matched samples: rock`; no comparison report created.
GrassWindAudit remains0 samples. These are cleared targets, NOT reference images.

Important next action: inspect what Main_RenderShadowMaps prepares/cleans for the
main geometry pass. Simply skipping it is not a working capture solution. Normal
Remix currently skips entire Deferred Main_RenderWorld AND Main_RenderShadowMaps
functions, in addition to independent low-level native draw/dispatch suppression.
A controlled startup test preserving native CPU preparation/cleanup while keeping
the audited low-level GPU suppression could isolate why batches become stale and
why the main pass is empty. It is NOT implemented yet; measure CPU cost and do
not claim this speculation is the fix. This also bears on the full traversal/
cell-transition objective, not just grass. Do not loosen pixel comparison gates.

Normal operation recovered: native suppression logs again14draw batches/75compute
submissions per sampled frame after both pairs. GrassLighting restoredfalse,
debug0/capturefalse via script, freecamfalse/first-person restored. Inspected
`.research/captures/093735-pair-diagnostic-restored.png`; camera saved pose
(13665.155,-48229.410,-148.007), pitch.157043/yaw-.080082. Game left running.
No active build/tool sessions. No saves written, no unrelated files deleted.
The paired feature remains experimental/unusable; full goal stays ACTIVE.

## Checkpoint 09:22 — native grass matrix/interpolation correction deployed

Previous turn diagnostic PROGRESS; this turn implementation + GPU verification
PROGRESS, not a visual/fidelity pass. Native Grass Lighting normals now use the
original instance3x3, excluding position-only ScaleMask variation and World,
and remain unnormalized until hit interpolation. New csRemixCreateGrassInstanceSetV2
takes separate row-major normal matrices. Old export stays compatible. Grass
GPU record128bytes (position80 + normal rows48), non-grass record80 unchanged;
normal matrices enter immutable placement identity. No per-frame CPU upload.
Details in remix-normals.md. Generic geometry offsets, nativeTBN and MSN remain
on their existing paths. Position/wind equations unchanged.

BuildDev11053 and two-pass runtime1561 finished exit0. Dedicated unit build39492
exit0; standalone test67844 and final combined run passed (2/2 native_foliage +
material_layout). Math tests detect premature normalization and verify skewed
row order; layout tests verify128-byte stride, word19 flag and row offsets20/24/28.
No live build sessions remain. Actual shader embed timestamps: native_grass.h
09:10:34, gbuffer_debug_raygen.h09:10:45, owning geometry_utils object09:16:04,
pathtracer_gbuffer object09:17:17, DLL09:18:53. Runtime diff --check passed.

Old PID20648 quit normally through Console+qqq; absent before deployment. Owned
DLLs only backed up/overwritten, no deletes/save writes. Backup and symbol label
`20260920-grass-native-matrix`:
- CS `4BE8F1DF0E0DDD474EA61BFD6CECBF3893DDA272E25A7834A5E6ABFAA93763A1`
- D3D11 `76E02DB12B1EAFA79158D60F2E56011EE13CB8E5A2FBED3E4562C58277B7C34F`
- DXGI unchanged `066D721569675B2172754F23A40DA3044E873B8B167A628A3534DA4780EF6788`
PID35656 started09:19:16 with LaunchTest -GrassProbe. Exact existing Riverwood
save loaded, HUD present, nativeReferencefalse, native-world suppressiontrue.
No graphics-quality or exposure overrides. Bounded GPU probe now validates
normal row products as well as wind; alpha check correctly compares source alpha.
Two actual GPU readbacks in `.research/testlogs/remix-dxvk.log` at09:19:37/38:
12vertices each, nativeNormalRows1, maxNormalError5.96046e-08, maxPositionError
.00390625, attributeErrors0; timer2.87239->3.89948 and maxMovement1.27344.
This proves sampled GPU transport/animation, NOT every blade or pixel parity.

Bank camera same(13000,-47300,650), pitch-.450005263/yaw0. Inspected lit images
`.research/captures/091106-grass-native-matrix-before.png` and
`092002-grass-native-matrix-after.png`. Sun/shadows differ after reloading the
save versus the long-running baseline; do not attribute general lighting change
to this normal correction. Raw debug804 captures each17artifacts:
`.research/buffers/grass-native-matrix-before-1789891914` and
`grass-native-matrix-after-1789892415`. Comparison with native1789885985 saved at
`grass-native-matrix-comparison.json`. Ordinary mask67874pixels,56258depthmatched;
normal median.776deg/p9076.364deg: large tail remains. Stricter2-unit depth subset
11916pixels median.342/p902.327, >30deg6.915%; normal-continuous510pixels
p50/p90/p99=.303/.783/3.215. These subsets are NOT pass gates.
Mixed grass paired2695pixels median5.420->2.546/p9066.276->61.153deg, meanchange
-3.979deg. Unchanged rock also improves (median9.476->7.642/meanchange-2.547), so
do not claim full visual improvement from this confounded temporal comparison.
Still need deterministic native/RTX correspondence and positive complex/sphere
GPU coverage; SSS angular response remains the documented RTX approximation.

Live Gamma/Authored census passed23grass/10leaves/12bark; report
`.research/testlogs/20260920-grass-native-matrix.json`. Capture script restored
debug0/capturefalse. Normal first-person/freecamfalse restored and inspected
`.research/captures/092112-grass-native-matrix-first-person.png`; camera matches
saved position(13665.155,-48229.410,-148.007), pitch.157043/yaw-.080082.
Goal ACTIVE, all wider water/temporal/traversal/character requirements unfinished.

## Checkpoint after 08:55 — ordinary-normal outliers localized; no renderer change

Offline diagnostic PROGRESS, not a visual pass. No DLL rebuild/deployment or game
mutation in this continuation. Prior PID20648/session configuration remains the
last live verified state; do not infer fresh live verification from these results.

Rechecked the user's Lighting.hlsl point: GetSoftLightMultiplier at583 and
RunGrass.hlsl at377 implement the SAME smoothstep-difference polynomial. Only
rolloff input differs (LightingEffectParams.x versus saturated vertex alpha times
Grass Lighting SSSAmount*2). The current nativeSoftLightWeight remains its
angle-zero/grazing value, feeding RTX thin SSS; full angular equivalence is NOT
implemented. Grass native colour is squared by RunGrass's two albedo multipliers.
Do not replace it with a generic leaf tint or claim the angular approximation is
the complete native soft-light function.

CompareBuffers now reports top32x32 outlier tiles, absolute-depth sensitivity and
normal-continuity sensitivity. Report:
`.research/buffers/grass-bank-outlier-localization.json`, using existing native
1789885985 and RTX debug804 capture1789890603. Broad grass p90 remains66.417deg;
that result is NOT replaced by stricter subsets. Within material interiors with
native gloss1, absolute depth error below10/2/.5/.1 game units yields respectively
20673/15942/6564/1343 samples, p90 angles15.089/1.015/.793/.868deg and >30deg
fractions9.21/3.87/2.96/3.05%. Both3x3 normal neighborhoods continuous within10deg
leaves only753 samples, p50/p90/p99=.163/.452/1.506deg. Strong dependence on
depth/boundaries suggests overlapping blades or temporal/sample correspondence,
not a global normal inversion; residual errors are NOT dismissed or fixed.
Top outlier tile704,128..736,160 has106 bad of574, overall median.229deg, bad
median relative depth mismatch.003884. Complex pixels remain zero.

Sensitivity helper has four synthetic tests: identical normals, empty selection
must yield null rather than zero error, strict depth thresholds/invalid depths,
and discontinuities in EITHER image. Combined reader+diagnostic16/16 pass.
Full real comparison rerun successfully after extraction. These test selection
logic, not GPU correctness. Tests: tools/remix/test_compare_buffers.py.

Next capture work must control time and pixel correspondence. Existing native
reference is intentionally one-way per process; CaptureReferenceBuffers only
runs in nativeReference and consumes bufferCaptureRequest. Simply toggling
cs.suppressWorld=false does NOT provide paired native/RTX captures. A simultaneous
diagnostic needs explicit capture plumbing and frame metadata, with normal world
suppression restored, and must validate native Grass Lighting is active. Do not
disable the nativeReference lifetime guard casually. Alternatively prove a shared
grass animation-time freeze before relying on separate captures. No such capture
change is implemented here. Known placement scale/per-vertex normalization
differences and all broader goal defects remain open. Goal stays ACTIVE.

## Checkpoint 08:55 — complex-normal path and grass mask; ordinary outliers remain

Previous turn PROGRESS; this turn implementation and diagnostic PROGRESS. Complex
grass now samples the lower atlas half from the same resident SRV/sampler, using
already-halved gradients and UV.y+0.5. RGB*2-1 without colour conversion. Shared
nativeGrassMapNormal derives CS's negative-world-position cotangent frame from
existing raw UV derivatives, preserving handedness and common T/B scale. Tests
compare against the CS screen-derivative formula for skewed/non-axis-aligned,
mirrored, front/back, sphere, neutral and degenerate cases. See remix-normals.md.
No extra CPU work/readback/texture bake. Specular gloss/far-detail cutoff unchanged.

First build90592 terminated on unsupported Slang inversesqrt; fixed to rsqrt.
Replacement runtime33867 finished exit0. Unit42409 passed before that shader-only
intrinsic correction; final unit37665 passed1/1 against final header. Reader12/12
and CaptureBuffers PowerShell parse pass. All sessions terminal, no build pending.
Compact unit results `.research/testlogs/20260920-grass-complex-unit-tests.json`.
Correct embedded-shader evidence: gbuffer_debug_raygen.h08:45:28,
rtx_pathtracer_gbuffer.cpp.obj08:48:08, indirect object08:47:57, DLL08:48:16.
Shader-manager object did NOT rebuild and is not the owner of these shader headers;
use the actual owning translation unit, not shader-manager timestamp alone.

Old PID32304 quit normally via Console+qqq; no force kill/save write. Owned runtime
backups/symbols label `20260920-grass-complex-normals`:
D3D11 `EF449A40B44380A20A0A38654AE2E799C60232A1DC3FB53E1D9632EAB103DA00`;
DXGI unchanged `066D721569675B2172754F23A40DA3044E873B8B167A628A3534DA4780EF6788`.
CS unchanged `64A6A6DD6FEDBA13D392F8A9F287029E6F9FB91008FE8E6A3385826F3759E659`.
New PID20648 started08:48:54, exact existing Riverwood save loaded, HUD present,
scene mode/nativeReferencefalse/native-world bypass requestedtrue/debug0.

New debug804 Native Grass Atlas Kind: green ordinary, blue complex, black others.
CaptureBuffers accepts -DebugView, records it in manifest and restores debug0
plus captureDebugImagefalse in finally. CompareBuffers recognizes804 and derives
same-frame RTX grass masks from rtxImageDebugView (output-resolution pixel-centre
resampled; pure colours only). It has NO native material-ID mask or wind/jitter
registration; this is exploratory evidence, not parity certification.

Bank pose unchanged(13000,-47300,650), pitch-.450005263/yaw0:
- `.research/captures/085000-grass-complex-normal-after.png` inspected.
- `.research/buffers/grass-bank-complex-normals-1789890600` normal scene raw capture.
- `.research/buffers/grass-bank-kind-mask-1789890603` debug804 raw capture,17artifacts.
  `rtxImageDebugView_208126-8504.raw.png` decoded and inspected.
- `.research/buffers/grass-bank-complex-mask-comparison.json` uses native1789885985
  and paired baseline native-orientation1789889758.

CRITICAL: zero complex pixels in this view. The new positive atlas-normal branch
has NOT been GPU-qualified and CANNOT explain changing metrics here. Actual RTX
ordinary-grass mask has67911 pixels;60223 depth-matched within1% give normal
angle percentiles[10,50,90,99]=[.0698,.2996,66.417,114.316]deg. Eroding to3x3
mask interiors leaves22903 matched pixels with[.0669,.2528,66.874,113.315].
Native ordinary Grass Lighting writes gloss1; additionally requiring native
normalGloss B>=.999 leaves57700 samples with median.2777/p9065.967deg. This
necessary-but-not-unique native signature is NOT a material-ID proof. Large
tail survives both filters; do not dismiss it as silhouette noise alone.

Mixed-region paired median3.962->1.450deg/p9067.065->20.106deg in debug capture
is NOT attributable to the complex-map code (branch absent). Native/RTX sampling,
wind, frame variation and debug shader variant remain confounders. Most isolated
grass normals agree closely, but the remaining population needs localization
and source/placement investigation. Sphere-positive assets also still untested.

Gamma/Authored host census passed23grass,10leaves,12bark; report
`.research/testlogs/20260920-grass-complex-normals.json`. First-person/freecamfalse
restored at saved camera(13665.155,-48229.410,-148.007), pitch.157043/yaw-.080082.
Final `.research/captures/085425-grass-complex-first-person.png` inspected.
No unrelated files deleted; no exposure/quality change. Goal stays ACTIVE,
unfinished. Next: localize ordinary-grass outliers with same-frame masks and
native evidence; positive complex/sphere GPU test; placement/interpolation
normal rules. All broader water/temporal/traversal/character requirements persist.

## Checkpoint 08:38 — grass orientation rule deployed; large normal mismatch persists

Previous turn PROGRESS; this turn source/verification PROGRESS, NOT visual pass.
CS kEffectLighting now becomes foliage flag64 NATIVE_FOLIAGE_SPHERE_NORMAL, matching
Hooks' native GrassSphereNormal permutation. V1 material import accepts bit64 only
with grass. Material hash/packing preserve it. Grass shading consumes authored
interpolated normals BEFORE Remix's per-vertex hemisphere correction, flips only
for ordinary backfaces, and leaves sphere normals unflipped. Final grass shading
normal bypasses getBentNormal; geometric normals/ray offsets retain Remix rules.
No global normal or exposure override. Complex normal maps, placement scaling,
per-vertex normalization and wind/temporal correctness remain unqualified.

BuildDev54795 and two-pass runtime61138 exit0. Dedicated unit49858 exit0;2/2 pass
(native normal sign's four cases plus colour equations, sphere flag layout/hash).
Archived compact results `.research/testlogs/20260920-grass-orientation-unit-tests.json`.
Reader tests12/12 and PowerShell checker parse pass. All build/test sessions terminal.
Shader header08:25:47 predates shader-manager object08:31:27 and DLL08:34:20.

Old PID27536 quit normally via Console+qqq. Exit took longer than the first2s
check; recheck confirmed process absent. No force kill, no save write.
Deployed owned DLLs with backups/symbols label `20260920-grass-native-orientation`:
CS `64A6A6DD6FEDBA13D392F8A9F287029E6F9FB91008FE8E6A3385826F3759E659`;
D3D11 `644F75A3AAC15F6C9A2CA2F1BD34047DE8D36A955BD54A1B8DF3F7FC907F0FF4`;
DXGI unchanged `066D721569675B2172754F23A40DA3044E873B8B167A628A3534DA4780EF6788`.
New PID32304 started08:35:08, exact existing Riverwood save loaded, HUD present,
scene/nativeReferencefalse/native-world bypass requestedtrue/debug0. No unrelated
files deleted or quality settings changed.

CheckFoliageRuntime now compares sphere bit64 with inspector shaderFlags bit62
when ExpectedGrassNormals is set. Gamma/Authored census passed23grass,10leaves,
12bark; report `.research/testlogs/20260920-grass-native-orientation.json`.
All sampled grass has sphere bit FALSE. This confirms negative import cases,
not positive sphere-normal GPU coverage; find a suitable asset for that test.

Same bank camera(13000,-47300,650), pitch-.450005263/yaw0. Lit captures
`.research/captures/082713-grass-orientation-before.png` and
`.research/captures/083558-grass-orientation-after.png`; after image inspected.
Raw `.research/buffers/grass-bank-native-orientation-1789889758` has17 artifacts.
CompareBuffers accepts --baseline-remix-directory and reports paired normals
under a COMMON depth<1% mask at the same pixel locations. Self-comparison against
one identical capture gives exactly zero change for all4regions. This does NOT
add material-ID isolation, temporal registration or eliminate wind/jitter/mips.

Report `.research/buffers/grass-bank-native-orientation-comparison.json` uses
authored-normals-repeat baseline1789888730 and native reference1789885985.
2470 common mixed-grass samples: median4.588->4.206deg, p9065.798->67.017deg;
mean error change+1.547deg,48.8% improved. Unchanged rock control median
7.463->10.320deg, mean+2.263deg. Hence no overall grass-normal improvement claim
is justified. Large tail remains. Avoid interpreting a small median drop as a fix.
Closer lit `.research/captures/083650-grass-orientation-close.png` inspected at
(12550,-47000,70), pitch-.300006/yaw0; grass renders but dark foliage/shadows and
water correctness are still unfinished. No native close-pose reference exists.

Normal first-person/freecamfalse restored after testing. No goal completion or
blocked status change. Next meaningful normal work: grass-specific GPU material
mask/complex-normal sampling and native matching capture; remaining broad goal
requirements (water, traversal, temporal, particles, characters, etc.) all persist.

## Checkpoint 08:20 — authored grass normals deployed; fidelity remains unqualified

Transport PROGRESS, not a grass visual pass. CS now imports authored VF_NORMAL
for grass, matching the CS Grass Lighting path (vanilla grass instead uses
instance-up). Only grass assets lacking normals request face-normal fallback.
Native mesh API accepts grass flag4 without face flag2; existing fallback flag6
remains valid, incompatible model-space/skinned/terrain combinations rejected.
GPU grass expansion already rotates the transported normal. No shader/layout
change in this build. Sphere-normal backface handling, complex atlas normal maps,
placement scale and Remix normal bending remain separate unresolved differences.

BuildDev session70785 and two-pass runtime20297 finished exit0. No build sessions
remain. Previous PID21544 quit normally; owned DLLs backed up and deployed as
`20260920-grass-authored-normals`, matching symbols archived:
CS `A98AE8047719F0B9B5634B4096468AAE71AFEF5C7D261118ADB19FF1F0CF629C`;
D3D11 `C55AF2890CAF0B01D3A9C9186371EDFD2F7F0B5DD607A10428784DBFA060D4C6`;
DXGI unchanged `066D721569675B2172754F23A40DA3044E873B8B167A628A3534DA4780EF6788`.
PID27536 launched08:17:03, exact existing Riverwood save loaded, HUD present.
Scene mode/nativeReferencefalse/native-world bypass requestedtrue; no quality or
exposure changes. No save writes or unrelated file deletion.

NormalBuffers.py decodes native negated unsigned-oct view normals to world space,
and Remix packed biased-unsigned16 oct normals (0=-1,32767=0,65534=+1).
IMPORTANT: this is NOT two's-complement SNORM. An initial diagnostic decoder
mistake falsely implied global inversion and was corrected before these results.
DDS/normal reader tests pass12/12. These verify decoder arithmetic, not GPU fidelity.
CompareBuffers now includes normal angle percentiles, subject to its existing
nearest-resolution/depth-filter limitations; no grass-only material mask.

Matching camera(13000,-47300,650), pitch-.450005263/yaw0:
- Baseline report `.research/buffers/grass-bank-normal-baseline.json` uses the
  previous linear-boundary capture and native Grass Lighting reference.
- New `.research/buffers/grass-bank-authored-normals-1789888678` and
  `grass-bank-authored-normals-repeat-1789888730`, each17 artifacts.
- Comparison reports `grass-bank-authored-normals-comparison.json` and
  `grass-bank-authored-normals-repeat-comparison.json` in the same buffers folder.

Mixed grass median angular error5.90deg before ->2.75deg then4.29deg; p90
24.32deg ->65.59deg then65.53deg. Depth-matched fraction89.4% ->82.9/82.2%.
Unchanged rock control median4.22deg ->8.42/7.47deg, demonstrating material/mip/
sampling or other frame differences also affect this comparison. Do NOT claim
overall improvement from the median alone: the tail is worse, wind/cutout/jitter,
different native/RTX pixel footprints and PSR remain confounders. The authored
transport is source-motivated but full grass normal correctness is NOT established.

CheckFoliageRuntime now offers scene-specific ExpectedGrassNormals assertion.
`-ExpectedColorSpace Gamma -ExpectedGrassNormals Authored` passed all23 submitted
grass entries;10 soft-light leaves and12 bark controls also pass. Report:
`.research/testlogs/20260920-grass-authored-normals-verified.json`.
This is host import metadata, NOT GPU normal correctness or complete scene census.
Lit `.research/captures/081757-grass-authored-normals.png` inspected; no gross
missing scene, but grass, water and other unfinished materials are not qualified.

Rechecked the latest user reminder directly: RunGrass377 and Lighting583 have
identical soft-light polynomials; rolloff source differs. Current derived colour
transport preserves grass albedo-squared vs tree base*rimSoftLightColor. RTX still
uses a grazing-weight approximation plus its thin-scattering angular lobe, NOT
the full native directional response. This limitation remains explicit.

Game left running first-person/freecamfalse at saved camera
(13665.155,-48229.410,-148.007), pitch.157043/yaw-.080082. Final screenshot
`.research/captures/081946-grass-authored-first-person.png` inspected.
Capture harness restores captureDebugImagefalse; normal scene debug0.
Next: isolate grass-only normals and native complex/sphere rules, resolve the
upper-tail mismatch, then qualify animation/SSS/water and the other goal checks.
Goal remains ACTIVE and unfinished; do not infer completion from this checkpoint.

## Checkpoint 08:03 — explicit foliage colour boundary deployed and measured

Previous turn PROGRESS (raw captures/reader/comparison); this turn also PROGRESS.
Added foliage flag32 NATIVE_FOLIAGE_GAMMA_COLOR when CS GetCommonBufferData reports
Linear Lighting inactive. Preserve CS material derivation first, then convert its
gamma-space output once with pow(abs(colour),2.2) at the RTX boundary. LL-on output
passes through unchanged. Applies to reflected grass and grass/tree SSS colour
products; angular/soft/back weights remain linear. This is NOT a global exposure
or albedoScale adjustment. SRV values still follow the native sampler/CS equations;
hardware sRGB format coverage and native LL-on GPU comparison remain unverified.
Details in remix-foliage.md. Generic non-grass reflection path unchanged.

API accepts additional bit32 and rejects flags with no grass/soft/back kind.
GPU stride unchanged; material identity includes domain flag. Shared CPU/Slang
nativeFoliageLinearChannel tests LL off/on, endpoints and squared SSS tint.
test_native_foliage and test_material_layout both passed (2/2), including GPU
flag offset/hash. Dedicated unit session36504 exit0; archived JSON results:
`.research/testlogs/20260920-foliage-linear-unit-tests.json`.
CS BuildDev27907 exit0; two-pass Release runtime58125 exit0; all sessions terminal.
Initial sandbox build attempts failed dependency access and were rerun with
approved escalation. No live/stalled build is pending. Shader header07:51 predates
shader-manager object07:57; new embedded code linked into final DLL.

Owned files backed up/deployed as `20260920-foliage-linear-boundary`, symbols archived:
CS `107F081AC18E3685A054AB22228400B17357DCF950765B194887E2F9643BB4FF`;
D3D11 `61944D0C3904F4BADF37ED19439B21139CAAE92F0BD15870D3D74B1DC8DFFF58`;
DXGI unchanged `066D721569675B2172754F23A40DA3044E873B8B167A628A3534DA4780EF6788`.
Old PID26128 quit normally via menu open/name Console then console qqq. NOTE:
`menu action=openConsole` is INVALID; use `action=open,name=Console` and verify.
New PID21544 started08:01:02, exact existing Riverwood save loaded, HUD present.
Scene mode, nativeReferencefalse, native-world bypass requestedtrue, debug0.

Same freecam comparison pose as07:46; inspected before/after lit captures:
`.research/captures/075150-grass-colour-domain-before.png`
`.research/captures/080150-grass-colour-domain-after.png`.
Grass is less washed out, but shadows differ between runs (game time was not
frozen). Do NOT attribute every lit difference to the correction or call it parity.
The raw result is stronger evidence:
`.research/buffers/grass-bank-linear-boundary-1789887711` (17 artifacts),
`.research/buffers/grass-bank-linear-boundary-comparison.json`.
Mixed grass-band mean RGB MAE against pow2.2(native raw) fell~.132 to~.013.
After grass RGB median(.050,.077,.018), versus before(.241,.303,.144).
Rock control median(.048,.057,.052) unchanged to10-bit quantization; this confirms
the colour correction is not a global gain. Mixed grass depth agreement89.4%
within1% (before82%); this is wind/jitter/sample variation, NOT a depth fix.
Raw albedo preview decoded with explicit Remix packing correction and inspected.

Live CheckFoliageRuntime -ExpectedColorSpace Gamma passed23grass(flags33),
10authored leaves(flags34),12bark controls; report
`.research/testlogs/20260920-foliage-linear-boundary.json`.
This is host metadata evidence only, not all foliage pixels. CPU tests are not a
GPU LL-on validation. Do not claim full native buffer parity or correct angular
SSS/normals/animation/shadows from this patch.

Current game left normal first-person/freecamfalse at saved camera
(13665.155,-48229.410,-148.007), pitch.157043/yaw-.080082. Final screenshot
`.research/captures/080309-foliage-linear-first-person.png` inspected. Raw capture
option resetfalse by harness. No saves modified or unrelated files deleted.
Next fidelity work: native grass/foliage normal and animation comparisons (still
face-normal proxy), LL-on and SRV colour-domain coverage, remaining generic albedo
conversions, water/temporal regressions. Entire goal remains ACTIVE and unfinished.

## Checkpoint 07:46 — raw native/RTX capture implemented; colour-domain mismatch measured

This goal turn is PROGRESS, not a visual completion. Added one-shot
`cs.captureBuffers` and `csRemixCaptureBuffers` diagnostics. Native saves ALBEDO,
NORMALROUGHNESS and MAIN depth at EndDeferred before deferred compositing/water.
RTX queues capture on its command stream and saves `gbufferAlbedo`/`gbufferLinearZ`
BEFORE composite (the old `albedo` export can already have compositing changes).
No continuous readback when capture is inactive. CS BuildDev session35800 and
runtime BuildRuntime sessions91367/69724 finished exit0; all sessions terminal.

Owned deployment/backups/symbols label `20260920-raw-buffer-capture`:
CS `865C6D20D66C7B2166E0B6363A99DF5925DE3CEA2EDD394C5AB124598402C447`;
D3D11 `08A422634D046051D8B0D56875C21700D40DB40BC2A6D1EF2FCF7A97ADF960B0`;
DXGI unchanged `066D721569675B2172754F23A40DA3044E873B8B167A628A3534DA4780EF6788`.
These are diagnostic changes; grass shader remains the 07:23 version.

Exact existing Riverwood save, matching freecam (13000,-47300,650), pitch
-0.450005263/yaw0. Native PID7992 used nativeReference at main menu plus session-only
GrassLighting; quit normally before launching RTX PID26128 at07:33:52. Captures:
- `.research/buffers/grass-bank-native-1789885985` (1920x1080 native buffers).
- `.research/buffers/grass-bank-remix-1789886137` (1280x720 RTX buffers).
- `.research/buffers/grass-bank-remix-stable-reader-1789886716` (repeat RTX capture).
- `.research/buffers/grass-bank-raw-comparison.json` and
  `grass-bank-stable-reader-comparison.json` contain numerical mixed-region probes.

Tools added: CaptureBuffers.ps1, ReadBuffers.py, CompareBuffers.py,
test_read_buffers.py. Python requires the bundled runtime (not PATH python).
Reader tests passed8/8, including channel order, padded rows, depth/stencil,
legacy float and truncated payload/header rejection. Capture harness now polls
the observed file set for stable size/time plus exclusive readability, not a
fixed sleep; live repeat completed with17 artifacts and matching camera records.
PowerShell parse check passed. Stability is not a runtime completion fence for
all possible later exports; required GBuffer files were fully decoded afterwards.

IMPORTANT exporter format trap: AssetExporter casts VkFormat64 (A2B10G10R10) to
GLI's BGR10A2 enum and copies bytes unchanged. GLI emits GLI1 format56 with reversed
R/B masks. ReadBuffers respects standard GLI masks by default; explicitly pass
`--remix-vulkan-packing` for these RTX exports to decode actual low10-bit R.
Native DX10 format24 uses low10-bit R normally. Both raw PNG previews inspected.
Do NOT conclude the GPU itself swaps R/B from this DDS header mismatch.

Results reproduce on the second capture: rock probe native medianRGB
(.213,.229,.221), RTX(.048,.056,.052), depth median relative error~0.034%,
99.6% within1%. Ground similarly fits a power2.2-decoded native hypothesis much
better than raw values. Mixed grass-band probe native(.248,.305,.160),
RTX(.241,.303,.144): grass stays close to native raw values while surroundings
are gamma-decoded. This is consistent with the pale-grass colour-domain problem,
NOT proof of the correct physical mapping. LL is disabled in the native reference;
native raw values cannot automatically be treated as linear reflectance.
Mixed grass only~82% depth within1%; ground~76%. Different resolution, pixel
footprints/TAA jitter, foliage wind/cutout and water PSR remain confounders. No
full-buffer parity claim. Do not tune exposure to conceal this discrepancy.

Latest user reminder was reconfirmed: RunGrass377 and Lighting583 have identical
soft-light polynomials. Grass derives rolloff from vertex alpha and SSSAmount,
trees from LightingEffectParams.x. Grass final native SSS tint is albedo squared;
trees use BaseColor*rimSoftLightColor (BACK separately). Current RTX angular lobe
is still an approximation. Next: establish ONE explicit native-to-RTX colour-domain
mapping for reflected grass and derived SSS, covering LL on/off and SRV sRGB;
compare raw data after changing it. Do not blindly add another gamma or change
global albedoScale. Existing generic native path also has two gamma sites requiring
an audit of actual flag/scale values before changing unrelated materials.

Current PID26128 remains running normal Remix scene, nativeReferencefalse, debug0,
captureDebugImage restoredfalse. Freecam OFF and first-person restored, confirmed
(13665.155,-48229.410,-148.007), pitch.157043/yaw-.080082. No saves modified or
unrelated files deleted. Water, normals, animation, temporal correctness and the
full goal remain unfinished. Goal stays ACTIVE; no new visual qualification.

## Checkpoint 07:23 — native grass albedo equation deployed; appearance NOT qualified

Previous goal turn was PROGRESS (803 GPU verification/deployment). This turn
found a separate reflected-colour mismatch: only SSS used the CS-derived grass
colour, while reflection retained native-import gamma, fixed-function vertex AO,
then generic gamma again. Added shared nativeGrassAlbedo derivation for both
reflection and SSS plus native_foliage_math.h/test_native_foliage. Dedicated unit
test passed1/1 (gamma, AO normalization, brightness, complex override, zero input).
Unit session94368 initially failed target discovery; reconfigure+95872 passed.
Release builds36692 and75905 both finished exit0. No build/test sessions live.

First build019491CD deployed as20260920-grass-native-albedo still applied the
host's generic albedoScale1.53846 compensation and visibly clipped grass. Final
source bypasses generic albedoScale/bias for resolved native grass (still clamps
to0..1); other materials and exposure unchanged. Final runtime backup/deployment
20260920-grass-native-colour-final:
D3D11 `31D2051DC10E15A572A4C9D9C412DB66492BAE71993BFA6C60D55794A9D7C094`.
DXGI and CS unchanged from07:03 checkpoint. Matching PDB/DLL archive present.

Matching free-camera comparison(13000,-47300,650), pitch-0.450005/yaw0:
-070618-grass-bank-before-scene /070621-grass-bank-before-albedo: old954A build.
-071440-grass-bank-after-scene /071445-grass-bank-after-albedo: interim019491CD.
-072150-grass-bank-final-scene /072155-grass-bank-final-albedo: final31D2051D.
All images inspected. Grass is brighter/paler; removing compensation reduces it
but it STILL does not visually match the native reference. DO NOT call this a
grass fidelity pass. Raw-CS-vs-RTX buffer colour-domain verification is the next
important check (native LL-disabled albedo is not necessarily linear reflectance).
No actual native albedo/depth readback has been compared in this turn.

Native reference071943-grass-bank-native-reference.png used a separate process
PID46760, nativeReference enabled at MAIN MENU, GrassLighting hot-enabled ONLY
for that session, LL/other CS world features still off. Shadercache161/161 complete,
0 failures; GrassLighting settings checked (brightness1,threshold.03,SSS1).
This is a same-camera lit reference, NOT a raw-buffer parity test. It shows
native grass much less pale and water substantially darker. Native-reference
process quit normally via Console+qqq before restarting Remix. No settings saved.

Current PID28108 launched07:20:48, exact existing Riverwood save loaded, native
reference false, world bypass requested true, debug0. Final census report
.research/testlogs/20260920-grass-native-colour-final.json. Freecam disabled and
first-person restored for handoff. No saves modified, no unrelated files deleted.
All normals, grass animation, optical/angular SSS, water, temporal correctness,
and full goal requirements remain open. Do not hide the new pale-grass result
with exposure tuning; establish the colour-domain mapping against native buffers.

## Checkpoint 07:03 — final foliage runtime deployed and GPU diagnostic verified

Supersedes pending build/process state below. Release session50596 finished exit0;
dedicated unit session23647 passed1/1 including soft/back texture retain/release
traversal. All build sessions are terminal. PID28716 was quit normally before
deployment. Final owned runtime DLLs backed up/deployed under
20260920-foliage-native-final with matching symbols:
D3D11 `954A87DB245B2B57FC5DF3A0D6618FFEF41BA10F8B176DFE10C11037BB64CB83`;
DXGI `066D721569675B2172754F23A40DA3044E873B8B167A628A3534DA4780EF6788`.
CS remains `2C795DF0DFF4524195D25B38D30C1F0D57E097000E00977DDE3CDD04CDB2DA7E`.

PID16012 launched07:00:41, Scene mode configured (native reference false, vanilla
world bypass requested), exact existing Save1_2B23D269_0_73647364_Tamriel_000002_
20260920044800_1_1 loaded. No saves modified. HUD present and playerLoaded true.
Capture070141-foliage-scattering-803.png is the first VALID unique803 diagnostic:
non-thin scene black, varied coloured authored foliage, neutral-grey fallback
cards. This establishes resolved GPU scattering inputs, NOT native angular/visual
parity or correct foliage shadows. Earlier802 captures remain invalid evidence.

Restored debug0 and thin opaque true. Capture070223-foliage-native-final-scene.png
visually inspected: normal scene and HUD, river visible, no overhead water sheet
in this stationary view; dark tree regions persist. Both captures have matching
first-person, freecamfalse camera(13665.155,-48229.410,-148.007), pitch0.157043,
yaw-0.080082. Game left running normally. Final host census
.research/testlogs/20260920-foliage-native-final-803.json passes23grass/10authored
leaf/12bark controls; nearest host sample only. No movement/water/animation or
normal correctness claim. Full goal remains incomplete.

## Checkpoint 06:34 — native foliage inputs built/deployed; live verification pending

Source freeze06:50: also added foliage soft/back indices to RtOpaqueSurfaceMaterial
forEachTextureIndex (used by retainSurfaceMaterial/releaseSurfaceMaterial). New
unit assertions require each view visited exactly once. Dedicated unit build/test
session23647 pending; Release build11437 predates this late header edit and MUST
be followed by another BuildRuntime pass before deployment. No more source edits
planned before final visual verification. Never deploy while build is live.

Follow-up06:47: PID9796 reached the exact save; normal screenshot063601 showed
river/HUD, no overhead water sheet, but dark leaf regions persist. Host input
census passed23grass/10authoredleaf/12bark controls (nearest entries only).
Debug800 mask063624 shows thin foliage green and bark/terrain red. Initial802
captures063622 and064548 are INVALID scattering-colour evidence:802 already
belongs to the diffusion-profile overlay. The new diagnostic is now registered
as803; corrected source is building with BuildRuntime session11437.

Runtime149EB33F87DF4C406B16824CC39EAF25693F2B20121FE71B6AD683D181653D5B
was deployed under20260920-foliage-native-debug, CS unchanged. Current PID28716
started06:44:49, exact save loaded, debug restored0, first POV/freecamfalse at
saved camera. Rerun host census20260920-foliage-native-final.json passes23/10/12.
Do not confuse deployment hashes with the pending803 build.

First off/on pair063732/063738 INVALID camera comparison (idle vanity camera).
Locked-freecam pair063857/063902 has matching camera(13665.155,-48229.410,300),
pitch-0.44875/yaw0.07213; both inspected. Lighting changes are subtle; this is not
proof of realistic foliage shadows or native parity. Thin scattering restoredtrue.
PID9796 quit normally via open Console+qqq; its exit-watcher session36617 is done.
All other prior sessions terminal except11437.

Use tools/remix/BuildRuntime.ps1: existing helper already knew headers are generated
as side effects and requires shader-first then a NEW dependency scan/link pass.
Direct single meson compile can regenerate shader headers without updating DLLs;
this happened during diagnostic edits (gbuffer .h06:38 > object06:26, unchanged
DLL hash). Helper updated to use meson compile in BOTH phases, per runtime skill,
instead of direct ninja. It was run successfully on the earlier diagnostic build.

Previous acknowledgement turn was NO PROGRESS (reconfirmed an existing audit).
This continuation implements the native grass/soft/back colour transport described
in remix-foliage.md. New csRemixCreateFoliageMaterialD3D11V1; V3/V4 preserved.
GPU112B stride preserved; offset/guard/hash/merge tests pass1/1 in dedicated unit
build. Optimized runtime and CS builds pass. Native grass vertex alpha preserved
for rolloff, not opacity; complex diffuse UV and derivatives half-height. Native
colour goes into single-scattering albedo ONCE, neutral extinction remains an
optical approximation. Soft angular response mapped at grazing, NOT exact native
lobe parity. Leaf cards without SOFT/BACK keep V4 fallback. No visual pass yet.

Owned DLLs backed up/deployed with label20260920-foliage-native-inputs:
CS `2C795DF0DFF4524195D25B38D30C1F0D57E097000E00977DDE3CDD04CDB2DA7E`
D3D11 `F029210CB95226C46987376109EAA2D09191A3BA610AC0B1CA7F10E7D2D1AB49`
DXGI unchanged `066D721569675B2172754F23A40DA3044E873B8B167A628A3534DA4780EF6788`.
Matching PDB/DLL archives and prior DLL backups are under that label.

Original PID47044 exited during builds. New PID35332 launched06:32:33 then exited
before configuration/save load completed. No new crash dump or Application event
found; logs stopped06:32:49 without an explicit failure. Do not infer cause yet.
Retry PID9796 launched06:34:43 with exit-watching exec session36617. Configure/load
in progress. This checkpoint is interim, not proof of runtime success.

All original water/animation/normal/interior/SSS fidelity requirements remain open.


## Checkpoint 06:08 — movement invalidated hidden-only fix; raster overlays excluded

IMPORTANT: The user moved and the overhead sheets returned on CS446DE6FA.
The05:57 stationary result below was insufficient, not a resolved water defect.
At(9805,-51782,-228.644), Water2048 overlays at Z=-250,30,600 were AppCulled=false
and submitted. Screenshot055846-water-movement-regression.png confirms the ceiling.
20260920-water-movement-negative.json later has1active submitted overlay.

Read-only audit water-wading-depth-state.json includes native water manager5299b0,
SetupTechnique154d0d0 and restore154cec0. Existing water-native-setup.json154db70
shows WADING technique bit0x40 uses water-specific stencil mode/reference pairs
(e.g. property+0x8c==2 -> stencil mode0xe/ref3,4->0xf/ref5). The manager assigns
per-water height stencil bits and updates multiple player-centred overlays. These
are masked raster passes, not additional physical water-medium boundaries.

Source now excludes ALL water-property-bit0 overlays from standalone RT geometry,
regardless of AppCulled, with explicit retained retirement. Ordinary procedural,
authored trough and distant water remain eligible. Wading ripple displacement
must ultimately be sampled on the real surface; it is still NOT implemented.
This removes the incorrect independent-boundary representation, not all water
bugs. CheckWaterVisibility now tests131072 property flag combinations; optimized
BuildDev passes. No runtime change in this checkpoint.

New CS label20260920-water-raster-overlay SHA256:
`6431F506AF323976548ABE326D8F94B900CE7FB3C6E8EB854EE2C8213D7A60F2`.
Backups and matching DLL/PDB under that label. PID52108 quit normally; current
PID47044 started06:04:17 with Scene/nativeReferencefalse/suppressWorldtrue/debug0.
Loaded exact user save below. FreecamOFF, first person, restored original player
position after testing; camera(13665.155,-48229.410,-148.007), pitch0/yaw-0.08108.
User may move it later. All build/launch/Ghidra/test handles terminal.

CheckWaterOverlayMovement.ps1 uses four controlled SetPosition waypoints and
first/third POV, then restores original player position/POV in finally. It is NOT
a walking/animation/fast-travel test. Report20260920-water-overlay-movement.json
has8samples,0submitted overlays and4..43ordinary water surfaces per sample, BUT
native overlays were hidden in every sample. It correctly FAILS its requirement
to exercise native-visible overlays (inconclusive recurrence coverage). Do not
weaken this requirement or claim a movement pass. After-test census
20260920-water-raster-final.json passes no-overlay predicate. Visually inspected
060611-water-raster-overlay-after-movement.png shows river/HUD and no ceiling;
view pitch changed during POV switching, so not an exact pixel A/B.

User requests added: grass SSS must match CS Grass Lighting's derivation; soft
lighting is shared with Lighting.hlsl. Confirmed RunGrass377/Lighting583 identical
soft-light polynomial, with different rolloff/tint inputs. Detailed equations and
exact inputs recorded in remix-foliage.md; current diffuse transmission proxy is
NOT corrected yet. User's updated goal also requires realistic tree SSS without
dark uncoloured shadows. Water's frozen patches, first/third-person jitter,
normals and original interior leak remain unfinished. Goal ACTIVE/incomplete.

## Checkpoint 05:57 — stationary overhead-water result (superseded by recurrence)

Latest user reports explicitly keep FIRST-PERSON motion/animation, THIRD-PERSON
animation jitter, and WATER unfinished. The saved water case also has apparently
non-animating patches on the real river. Do not reinterpret earlier CPU ownership,
camera or parameter tests as visual animation passes. Interior exterior clouds/LOD
and normals remain unqualified too. Goal ACTIVE/incomplete.

User save (preserved, not overwritten):
`Save1_2B23D269_0_73647364_Tamriel_000002_20260920044800_1_1`.
Character sdsd, RiverwoodEdge01/Tamriel, player(13665.1553,-48229.4102,-234.4399).
First-person camera(13665.1553,-48229.4102,-148.007), pitch0.157043/yaw-0.080082.
Loading this exact save in PID40432 reproduced a water ceiling in screenshot
055018-water-saved-repro.png. Two player-centred Water2048 overlays at Z=-250/30
were hidden natively but submitted to Remix. Ordinary procedural river is Z=-250.
Read-only native audit water-wading-visibility.json confirms the water manager
0x52a890 directly changes the overlay hidden bit, independently of our retained
scene. Property bit0 selects WADING (existing GetRenderPasses audit152c4a0).

RemixWaterVisibility.h restricts the correction to water-property bit0 plus the
geometry's own AppCulled state. Capture retires the retained registration, not
merely skipping an update. Ordinary/trough/distant water keep existing policy;
active wading geometry remains eligible. No broad frustum-culling filter added.
CheckWaterVisibility.cpp passes262144 flag/visibility combinations. Optimized
BuildDev passes. New CheckWaterSavedCase.ps1 records native/imported scroll and
flow-clock changes and hidden-overlay submission; bounds disambiguate distant
segments sharing names/origins. Movement changes keys and is marked unmatched.

Deployed CS label20260920-water-hidden-overlay, SHA256
`446DE6FA1C7EDA722E486C58EE4E2F4C41B3AF5E3385038B2F1131CCA7E4EF57`.
Owned DLL backup and matching DLL/PDB archived under that label. Runtime remains
foliage-thin34959838 (below). PID40432 quit normally via Console/qqq.
Current PID52108 started05:55:30, loaded the user's save; Remix Scene on,
nativeReference=false/suppressWorld=true, debug0 restored, freecam OFF, first POV.
No saves, deleted files or saved graphics settings. All build/launch/audit sessions
terminal. Game left running at the saved scene (unless user subsequently moves).

After-deploy report20260920-water-saved-after.json frames754..828:0hidden overlays
submitted, ordinary river submitted with changing imported scroll/flow clock.
Visually inspected055612-water-hidden-overlay-fixed.png: ceiling gone, river/HUD
remain; camera matches055018 within0.0002Z. This is a verified narrow fix, NOT
water parity. Before report was sampled later while the user moved, and has one
hidden overlay rather than the two in the original screenshot/census.
Debug16 captures055648-water-saved-normal-a.png and055652-water-saved-normal-b.png
share that camera and show some changing fine water detail. These two images do
not establish continuous animation across every patch. Full rendering restored
in finally. Frozen patches remain to localize (base water, active wading overlap,
flow sampling, or effects); no blanket animation fix was made.

### Foliage deployment carried forward from the interrupted preceding work

Label20260920-foliage-thin was built/deployed before these water reports:
CS05B65CF40FF08CAA8FB6D3B2C3D5C1FD4B734520B7B569182CC02A47F361588C
(now superseded by446D); D3D11
`34959838C8D850171B9186C11376A767FCB7CA9E1A7816312222C5CDF9EE2321`;
DXGI`066D721569675B2172754F23A40DA3044E873B8B167A628A3534DA4780EF6788`.
See remix-foliage.md: V4 resident transmission SRV import and thin-opaque pNext,
grass or TREE_ANIM+two-sided+alpha-test selector, diffuse-transmission proxy.
Optimized runtime/plugin builds and dedicated material test passed. GPU debug800
selection/A-B NOT yet performed; foliage visuals/normals remain unqualified.

User suggested BACK_LIGHTING/SOFT_LIGHTING detection and rimSoftLightColor for
transmission. Source confirms rimSoftLightingTexture at material+0x60 and separate
specularBackLightingTexture+0x68. Lighting.hlsl samples rimSoftLightColor for
RIM/SOFT, backLightColor separately for BACK. This requested native-input follow-up
is not yet implemented; do not present diffuse proxy as final. Skin/hair need
their own models rather than blindly making every flagged character thin foliage.

## Final state 05:34 — sky policy and reflection override correction

PID54164 (started05:31:57) remains in Sleeping Giant Inn, Remix Scene on,
nativeReference=false, suppressWorld=true, debug0/GPUprint false. Audit frozen/off,
freecam OFF, third person at (-226.1058,-412.5519,113.0675), pitch0/yaw0.008404.
World camera valid; frame12096 submitted810/loaded836. Two impossible-jump counts
follow intentional free-camera relocation and restoration; not a pacing test.
Capture053357-sky-policy-final-playable.png inspected; HUD/player/interior present.
All builds, audits and launch sessions terminal. Goal NOT complete.

New optimized CS label20260920-sky-policy-final:
`19F306FDFC2298AC1F81CF1E341B6669F1148CFB802970BB3F3605703AD1E994`.
Owned plugin backup and matching symbols archived under that label. Runtime
unchanged861C8158... clone-retirement. No game files deleted, no saves/INI writes.

Found a real defect: SetReflectionLodEnabled returned early while already active,
so the advertised per-frame LOD exclusion was not enforced after INI rewrites.
It now reapplies before native world rendering (and at UI initialization), retaining
the original setting once per ownership period. New RemixSkyPolicy.h and
CheckSkyPolicy.cpp cover96 weather-cube gate cases and restore/reload lifecycle.
CheckReflectionOverride.ps1 perturbs all4 settings in memory, advances frames,
and restores all in finally. Old B25E failed4/4; final19F passed4/4. Evidence:
20260920-sky-policy-negative.json and -final-settings.json.

Read-only Ghidra audit confirmed ShowSky versus UseSkyLighting, sky modes and
HideSky flag. Weather-cube submission now requires ShowSky for interiors plus
full mode3 and !HideSky. IMPORTANT: a trial allowing native dome-only mode2
(CS2CAAE030, PID24544) visibly regressed into exterior clouds in the inn. That
trial was corrected, not left deployed. Its screenshot052927 and failed
20260920-sky-policy-after-audit.json are preserved. Native dome-only mode needs
its own valid source; the reflection cube can still contain previous weather.
Do NOT re-enable mode2 merely because native root visibility allows it.

Final native door activation Riverwood->inn:3685 audited attempts,3554 interior,
zero suspect classes/dome,131 exterior dome samples.3 not-ready attempts lack
current camera; first is under Fader Menu. Strict audit intentionally FAILS;
camera-ready failures0.20260920-sky-policy-final-audit.json, final-membership.json.
Captured053256 outdoor sky;053323 black outside interior shell; runtime dome
active outdoors/inactive indoors. This does not reproduce or conclusively resolve
the user's original intermittent clouds/LOD report. Whole-scene reflection bit12
bypass remains unqualified; original visual/material/normal issues remain.

Prior PID47604 inn observation:95988 interior attempts with0suspects/dome;
3missing cameras across transition.20260920-sky-policy-before-audit.json.
See remix-native-render-audit.md top for exact native addresses and qualifications.
Next meaningful work: actual material/normal fidelity or a reproducible remaining
leak, not repeating passing ownership/settings loops. Portable runtime export
still stale.

## Final state 05:15

PID47604 remains in Riverwood, Remix Scene on, nativeReference=false,
suppressWorld=true, debug0, GPUprint false. Retained audit is off (zero records
in active log). Third-person camera/freecam OFF at approximately
(17183.9336,-47264.3516,-18.6541), pitch0/yaw1.02502. Native-world camera valid,
impossibleJumps0,8368 submissions at frame3312; view-model camera false/count0.
Final capture051500-clone-retirement-final.png saved and inspected. HUD and
third-person player visible; this remains visually unqualified. All build/test/
launch handles terminal. No goal completion claim; concrete progress this turn
is the reproduced/fixed/validated derived-copy lifetime defect.

## Continuation 05:14 — clone retirement deployed; first-person transition audit clean

Optimized runtime label20260920-clone-retirement deployed with owned-DLL backups:
D3D11 `861C8158FC6CE60153C4EF504CDCD8F997E286B3797CC8208D3A52C7B027F289`;
DXGI unchanged `066D721569675B2172754F23A40DA3044E873B8B167A628A3534DA4780EF6788`.
CS unchanged B25E9E1FE638CAF755E4BEAE6FC1DBAAE713936BAB3689C6A9F3C382CF534A86.
Matching symbols/backups under the runtime label. No deletion or saved settings.

Diagnostic PID32920 started05:10:36, loaded inn directly, drew iron axe.24 POV
samples passed. Native exit0x13419 reached Riverwood02; observed Survival prompt
declined; native entry0x13424 reached inn. Paused-console move to Riverwood then
eight first-person transitions through inn/Trader/Riverwood passed. All8 settled
results had current view-model camera and3 model submissions. This uses native
door Activate plus console travel, not walking input or fast travel.

Full archive20260920-clone-retirement-firstperson.log:8624 sequential censuses,
runtime frames270..9254, zero cumulative violations, gcMarked, badEntries,
badOwners, badPrims or staleUnowned. Retained range23..8333. Summary and route/POV
JSONs share the clone-retirement prefix. Audit thresholds unchanged. Original
failing firstperson log from preceding turn retained for comparison.

Inspected051259-clone-retirement-firstperson-riverwood.png shows first-person
axe/HUD and outdoor scene. Foliage/material/lighting still visibly questionable
(very dark trees, bright patches); not native-image/depth parity. Lifetime tests
do not prove no motion judder or complete scene correctness. Other goal items
remain incomplete, including normals/skin/water/glass/foliage fidelity.

PID32920 quit normally. Non-audit PID47604 started05:13:22 to restore Riverwood.
Launch/Configure completed; see subsequent entry for final camera. No build or
test jobs remain. Next meaningful visual investigation: normal/material fidelity
in Riverwood and the inn, not more repetitions of this already-passing CPU
lifetime route. Portable runtime export remains stale.

## Continuation 05:10 — derived-copy collector regression reproduced and repaired

Previous turn was progress. InstanceManager's real collector reproduced the
first-person pending-GC issue under a focused unit fixture. Source/view/player/
survivor/virtual ordering0,1,3,4,2 left2 survivors instead of1. Archived negative
test20260920-clone-retirement-negative.txt returns-1 with that explicit failure.
The first test invocation terminated on uncaught DxvkError; test main now catches
it correctly, and the archived rerun is the controlled negative result.

Fix: erasePersistentMapEntries returns the earliest marked derived index; after
swap deletion GC resumes at min(current,index), recursively collecting dependent
copies in the same call. No unconditional second full-scene sweep. Dedicated
tests pass2/2, including120 permutations of the reference/view/virtual/player/
unrelated survivor, correct vector indices, exact callbacks and4 scene-generation
increments. Direct clone deletion preserves its source; repeated and empty GC
are covered. Positive log20260920-clone-retirement-positive.txt. See lifetime doc.

Source inspection confirms renderer-created copies are excluded from persistent
bucket caching; full merge skips marked instances. No extra speculative cache
policy was changed. Header adds friendship solely for the actual manager fixture.
CheckCellRoundTrip now supports -Pov first (default third unchanged), requires the
first-person native camera when selected, and records view-model counts.

At this entry optimized runtime build is still running (exec72084). Old PID32324
quit normally via Console qqq; no game currently running. Runtime/CS deployment
unchanged until the later validation entry. No deletions or saved settings.
Goal remains incomplete; this fixes CPU lifetime, not the dark hands/skin or
incorrect alchemy glass, nor broad normal/depth/animation/pacing parity.

## Final state 04:59 — short route passes, game left running

PID52784 completed all8 console transitions Riverwood/inn/Trader, normal third
person, without crash. All settled interior membership checks passed. Archived
20260920-retained-identity-route.log contains4122 sequential ownership censuses,
0 violations/stale unowned, retained range24..8342. Its summary and roundtrip JSON
are alongside it. This clean short third-person run does not supersede the two
first-person GC-marked frames in the earlier archive.

Diagnostic process quit normally. Current PID32324 started04:57:46, Remix Scene
on/nativeReference=false/suppressWorld=true; no retained audit enabled. Inn loaded
directly from Main Menu. Debug0/GPUprint false, no saved quality changes. Current
camera third person, freecam OFF, approximately(-234.1146,-406.6280,111), pitch0,
yaw0.008606. HUD working. No active build/test/launch jobs remain.

Inspected captures:045832-retained-identity-interior.png, still incorrect bright
alchemy glass;045834-retained-identity-outside-shell.png, black background with
HUD and no visible exterior clouds/LOD;045836-retained-identity-final.png, restored
third-person scene, player very dark. These are limited views, NOT a confirmed
fix of the reported intermittent interior leak or full rendering correctness.

Next concrete follow-up: InstanceManager's single-pass GC can leave derived
view-model clones marked after their vector slot was visited. Check cache safety
and add a focused same-frame retirement regression; do not discard the audit
failure. First-person appearance/native depth parity and broader normals/skin/
water/foliage/temporal issues remain unresolved. Runtime export tooling still
needs synchronization with the development clone. Goal remains incomplete.

## Continuation 04:57 — runtime repair deployed; first-person validated with caveats

Runtime label20260920-retained-identity deployed with backups/symbols:
D3D11 `4DC4243EA65829CF214553971358213F484AE8FB54925EF60C21CF0AEF7EA031`;
DXGI unchanged `066D721569675B2172754F23A40DA3044E873B8B167A628A3534DA4780EF6788`.
Full optimized build, final incremental relink and dedicated tests passed2/2.
Only owned two runtime DLLs overwritten. CS remainsB25E9E1F from below.

PID51112 loaded the inn without the previous crash. CheckViewModel passed24
samples: first-person3 meshes, third0. Culled blood overlay now excluded for
view model; world similarly named overlays intentionally unchanged. Captures
045214-first-person-final-axe.png and045249-first-person-bare-hands.png were
inspected. Axe and both fists visible, HUD intact; shading still too dark.
Iron axe0x13790 restored after temporary UnequipItem. No save made.

CheckSceneReset passed3 RT-off/on cycles in same PID; runtime confirms384 nonempty
retainedClear events. After that phase10238 census records were clean. Subsequent
first-person transitions exposed2 gcMarked=3 frames,11535/13715; no badEntries,
badOwners or badPrims. Archive20260920-retained-identity-firstperson.log is NOT a
clean ownership pass. Likely source: single-pass InstanceManager GC marks derived
clones behind its scan index. Full merge skips marked instances; cached-bucket
behavior remains to check. Do not weaken the audit to suppress this evidence.

Native exit0x13419 reached Riverwood02; Survival prompt declined. Roundtrip harness
requires Riverwood exactly, so paused console moved there. Three transitions then
passed before the fourth stopped on POV=vanity; no game crash. PID51112 quit
normally. Fresh diagnostic PID52784 started04:56:00 for the short eight-transition
route; see later entry for completion/final process. No task remains running from
earlier builds. Goal ACTIVE/incomplete; exterior leak still unconfirmed.

## Continuation 04:50 — first-person import; retained-node crash repair in validation

Goal remains incomplete. Latest interior exterior-cloud/LOD report is still not
reproduced in settled inn captures. Do not infer correctness from membership tests.

First-person camera/geometry implemented in CS (details remix-first-person.md).
Current deployed CS label20260920-first-person-visibility, optimized DLL SHA256
`B25E9E1FE638CAF755E4BEAE6FC1DBAAE713936BAB3689C6A9F3C382CF534A86`.
It captures native view-model camera at verified RVA1514ff7, restores the camera
origin after native root rebasing, imports VIEW_MODEL geometry, and honors its
own AppCulled flags. World camera unchanged. Remix viewModel enable=true/scale1.
Initial build C645A2C6 passed inn/Riverwood POV lifetime checks and showed the axe;
native reference projection was plausible, but idle phase differed and Remix
axe is too dark. Final AppCulled correction was not yet tested at this entry.

PID51248 crashed04:37:44 on initial inn load before its test. Archived dump
`.research/testlogs/20260920-043744-first-person-crash.dmp`, matching runtime
symbols20260920-runtime-ownership. CDB identifies freed/invalid ReplacementInstance
at0x11f2f71c0a0, accessed by old-node release in submitExternalDraw+0x436.
No earlier allocation history is present in the minidump.

Runtime code has a definite bulk-clear defect: tracker.clear destroyed all nodes
without destruction callbacks, leaving retained entries with raw dangling pointers.
Callbacks now run before destruction. A separate heuristic-alias hazard is also
removed: retained registrations use their own distinct node, not L1/L2 matching.
Mesh-specific destruction still invalidates owners; motion updates dirty flags;
released nodes are collected without entering heuristic maps. No null-vector
guard was added to hide the fault. See remix-retained-lifetime.md.

Dedicated retirement/material tests passed2/2 after the ownership implementation.
Test covers identical independent draws, movement, bulk invalidation/recreation,
mesh-specific invalidation and release. Full log20260920-retained-identity-unit.txt.
At this entry optimized runtime build and final diagnostic rebuild were running;
runtime has NOT yet been deployed and no game is running. Follow later entry for
final state. CheckSceneReset.ps1 added to exercise temporary RT disable/restore;
RetainedAudit emits retainedClear events to prove nonempty bulk clears occurred.
No deletions, saved games or persistent graphics changes this turn.

## Continuation 04:19 — interior leak rechecked, not reproduced

No rendering changes or deployment this turn. Goal remains incomplete. PID54068
still runs the same build in Sleeping Giant Inn. Current-view capture
`.research/captures/041532-interior-current.png` shows the inn and incorrect
fluorescent alchemy glass, but no exterior clouds/LOD. Camera temporarily moved
from (-700,200,121) to (-700,200,1800): inspected
`041649-interior-outside-shell.png` shows a black background with HUD, not an
exterior scene. Original camera restored; `041650-interior-restored.png` saved.
The two deliberate camera teleports will increment the camera-jump diagnostic.

Settled CheckInteriorMembership passed:808 submitted,834 loaded, zero matches
in all nine exterior-negative groups;6 flame and2 head positive samples.
Current runtime log reports dome registered0/instanced0/active0. These are
limited settled observations, not transition-frame or full-scene proof.

Source inspection: instance removal invalidates its cached BLAS bucket and
scene generation; opaque/unordered TLAS builds use current instance counts.
No new concrete cache-lifetime defect established. Inactive dome does NOT by
itself prove a black sky: composite and secondary miss shaders sample separate
sky-matte/probe resources as fallback. Those resources are allocated by native
sky rasterization; this D3D11 host is API-only, and its sky-dome export does not
write them. Current outside-shell capture is black. No evidence warrants a
speculative sky or geometry filter change. Optional request sent for a screenshot
and affected interior; do not treat lack of response as blocking the larger goal.

First-person mesh submission remains unimplemented as recorded below; no
first-person work was performed in this turn, which focused on the latest
interior report. Both expensive audits remain off, Remix on, HUD working.

## Continuation 04:10 — runtime ownership audit clean; normal-overhead game restored

Previous turn was progress. This turn adds runtime-side ownership evidence;
NO rendering/lifetime/material policy fix. Goal ACTIVE/incomplete. Exact
intermittent interior clouds/LOD still not reproduced, and clean CPU state is
not proof of correct GPU structures or pixels. Glass/character/normal/water/
foliage/temporal/first-person/native-buffer parity remain unresolved.

Current PID54068 started04:08:23, loaded directly into RiverwoodSleepingGiantInn
from Main Menu after configuring Scene Remix/nativeReference=false/suppressWorld
true. Debug0/GPUprint false. Both audits disabled on this new process; verified
zero retainedAudit records in its active runtime log. HUD only, Console closed.
Freecam ON (-700,200,121), REQUEST pitch0.28/yaw-0.747; reported pitch
-0.207930624485/yaw0.727235555649. Final inspected capture
`.research/captures/040954-runtime-ownership-final.png` shows the inn/HUD and
still-wrong fluorescent glass, no exterior leak in that limited view.

CS unchanged: `8A2ADE4472E36C33AD667EC192140738D5E9E68602E0CA3B238BB47F781E7E25`.
Runtime label20260920-runtime-ownership, built/deployed D3D11 hash verified:
`C42DD380FD2D9498555FED718BA8DFF202EF6F90AB47874A5E104366BD0D83FD`.
DXGI `066D721569675B2172754F23A40DA3044E873B8B167A628A3534DA4780EF6788`.
Only two owned runtime DLLs overwritten, with backups/symbols under the label.
No deletion or saved graphics changes. Earlier PID48820 and diagnostic PID42164
quit normally through paused Console qqq. All build/test/launch sessions terminal.

Implementation: opt-in CS_REMIX_AUDIT_RETAINED=1 (LaunchTest -RetainedAudit)
scans replacement nodes, retained registrations and live RtInstances after GC,
before graph overrides and acceleration-structure preparation. Checks reciprocal
host ownership, primitive back-pointers, GC-marked survivors; reports unowned
instances and those whose node missed current-frame submission. Per-frame
numbered records plus cumulative violation count. Default is off; enabled
allocation/logging overhead makes this unsuitable for performance measurement.
No GPU readback and no changes to retention, sky, shading or composition.

Release runtime build passed; dedicated UnitTest retirement/material-layout
targets rebuilt and passed2/2 (0.12s/0.09s). Existing external-PDB warnings only.
Initial sandbox build could not open dependency log; escalation succeeded.
CheckRuntimeOwnership.ps1 parser/validator and two fixtures added: clean log
passes; a single bad frame followed by a clean frame is rejected. This tests
the parser, NOT injected runtime corruption. PS syntax/diff checks passed.

PID42164 ran with diagnostics from04:02:31. Eight console transitions across
Riverwood/inn/Trader passed settled checks. Simultaneous host audit:2041 attempts,
1219 interior,8 changes,0 suspects/camera-ready failures/empty-ready scenes;
22 no-current-camera attempts. Runtime full archive contains27008 consecutive
census samples (runtime frame417..27594, gaps while not building a world scene),
0 violationFrames/badEntries/badOwners/badPrims/gcMarked/staleUnowned.
Outdoor maximum8350 retained draws; non-retained grass draws current.
Settled inn808/809 and Trader371 match host-sized scene counts. A subsequent
native Activate(player,false) on entry0x13424 returned true and reached inn.
This is NOT walking input/fast travel/first-person verification.

Evidence in `.research/testlogs`:20260920-runtime-ownership-full.log and
-full-summary.json; -cells.log/-cells-summary.json earlier6117-sample snapshot;
-roundtrip.json, -host.json, -unit.txt. See remix-retained-lifetime.md for scope.
Active remix-dxvk.log belongs to the final non-diagnostic process.

Alchemy inspection archived -alchemy-inputs.json. InnerHaze layers retain
scale3/flags4208 and animated native/imported UVs; Outer/InnerGlass lighting
meshes remain nonemissive native environment-map materials. Native shader and
existing remix-effects.md audits were rechecked; no new causal finding warrants
reducing brightness. Final screenshot remains wrong. Runtime ownership check
does not cover graph overrides, GPU culling, cached BLAS/TLAS contents, sky-dome
activation or composited-frame history; these remain possible investigation
areas, not diagnosed causes.

Portable runtime tooling remains STALE: PrepareRuntime.ps1 pins Titanfall
62378bcf and copies an older d3d11_rtx host, while current development clone is
.research/dxvk-remix at local custom HEAD12b6665d (origin doodlum/dxvk-remix,
upstream NVIDIAGameWorks/dxvk-remix), with tracked AND untracked source changes.
runtime.patch last modified09-18 and was NOT regenerated. Reproducible delivery
must eventually include committed fork history plus untracked new files without
overwriting the user's index; simply diffing HEAD or applying the old script is
not a complete export of the current runtime.

## Continuation 03:55 — per-frame interior audit deployed and exercised

Previous turn was progress; this turn adds stronger evidence, not an exterior
leak fix. Goal ACTIVE, incomplete. Exact user-reported clouds/LOD inside remain
unreproduced. Fluorescent alchemy glass is visibly wrong in the final capture;
normal/character/water/foliage/temporal/first-person parity remain unqualified.

Current PID48820 started03:51:46, RiverwoodSleepingGiantInn, Scene Remix,
nativeReference=false/suppressWorld=true, debug0/GPUprint false. Last inspected
capture `.research/captures/035422-scene-audit-final.png`: HUD visible, no visible
exterior leak in this limited view, fluorescent green/yellow glass still present.
Console closed; freecam ON (-700,200,121), REQUEST pitch0.28/yaw-0.747; actual
pitch-0.207930624485/yaw0.727235555649. Native boundary logs12 draws/2 compute
suppressed and sky mode2/no sky submission. Do not interpret this as all-pass
native suppression or whole-scene/GPU proof.

Final CS `8A2ADE4472E36C33AD667EC192140738D5E9E68602E0CA3B238BB47F781E7E25`,
label20260920-scene-audit-context, built03:51:13, built/deployed hashes matched.
Runtime unchanged from material-stride: D3D11
`BE90520A80A9B20FEDD474FF7B7DD245EC1219821A0476BD8D176EE61EC44B53`, DXGI
`066D721569675B2172754F23A40DA3044E873B8B167A628A3534DA4780EF6788`.
Only owned CS DLL overwritten with backups/symbols under usual label directories.
No deletions/saved graphics changes. Former PID44912 and intermediate PID33384
quit normally using paused Console qqq. Intermediate CS D1B31F...059FB is
superseded. Archived logs20260920-before-scene-audit-runtime.log and
20260920-scene-audit-initial-runtime.log. Active log remains remix-dxvk.log.

New `cs.sceneAudit=true` enables/resets, false freezes. Disabled by default,
atomic check only; enabled adds a host-visible membership census per BeforeUI
Scene submission, after sky submission. No filtering/rendering policy changed.
Sky Submit returns accepted API-call status. `Inspect(remixScene,audit)` returns
cumulative counters, first suspect/failure, per-class maxima and256 recent frames.
Preserves a single bad frame beyond ring rollover, across cell-load discards.
Tracks current native camera and Fader Menu, camera-ready failures, empty-ready
scenes. Classifiers use property/feature/LOD flags, never ancestor ObjectLODRoot
or texture Cloud names. Explicit scope/usage in remix-scene-audit.md.

CheckSceneAudit.cpp production-history test passed (rollover, single-frame fault,
classification/counters/reset); PS checker parsed, final optimized BuildDev
passed. Initial build required escalation for extern/dxvk worktree verification;
two local compilation issues (partial patch left return declaration, UI const
qualifier) were corrected before deployment. No runtime rebuild needed.

Initial audit PID33384:8 console transitions,2953 attempts/1210 interior, no
interior sky/LOD or notLoaded records,23 not-ready attempts. Added camera/fade
context to investigate rather than calling these visible failures.
Final PID48820:8 console transitions passed settled-cell checks. Per-frame report
20260920-scene-audit-context.json:2007 attempts/1218 interior,8 cell changes,
zero suspects/zero camera-ready failures/zero ready-but-empty.24 not-ready
attempts ALL lacked current native world camera. First failure frame2228 in
inn cell78790, Fader Menu true, submitted0, no dome. Only first failure's fade
state preserved; not proof every failure was faded. Outdoor maxima386 distant
tree/130 objectLOD/73 landLODNoise;777 accepted dome calls, none indoors.

Native door entry0x13424, exit0x13419, entry0x13424 all Activate(player,false)
returned true and reached inn/Riverwood02/inn under same PID. Report
20260920-scene-audit-doors.json:1106 attempts/768 interior,3 cell changes,
zero suspects/camera-ready failures/empty-ready;9 missing-camera attempts,
first frame5449 Fader true,submitted0.335 outdoor dome calls,0 interior. This
is native activation, NOT walking input or fast travel or first-person proof.
Both strict audit checker runs intentionally FAILED on not-ready attempts;
do NOT report them as wholly passing. No crashes in these checks.

Final stationary inn control PASSED:11241 consecutive CS frames14298..25538,
all interior;0 not-ready/missing-camera/suspect/empty-ready samples. Report
20260920-scene-audit-stationary.json. Audit now frozen/disabled; game still alive
PID48820. This is host evidence at one camera, not visual temporal/normal parity.
All build/test/launch sessions are terminal.

Next useful exterior-leak evidence is runtime active-instance/TLAS/dome state
and final composited frame history correlated to cell transitions. Clean host
census does not prove runtime retirement or pixel correctness. API-return status
does not prove GPU dome activation. Sky/cloud geometry classifier paths lacked
outdoor positive controls; do not claim every classifier verified. Also note
DiscardFrame leaves Submit's static lastSceneCell intact: same-cell reload could
skip rediscovery until its4-frame cadence, but this has NOT been reproduced
(same-cell coc test triggered no discard/empty frames). Do not present it as a
confirmed cause or blindly change cell/sky policy.

## Continuation 03:35 — material stride and null child-array guard deployed

This section supersedes older current-process/deployment statements below.
Goal ACTIVE, incomplete. The reported intermittent interior clouds/LOD have NOT
been reproduced or qualified. Water, character/normal parity, fluorescent indoor
glass, foliage and temporal/navigation validation remain open.

Current PID44912, started03:28:05, last checked alive03:34:52. Left in
RiverwoodSleepingGiantInn, Remix Scene/nativeReference=false/suppressWorld=true,
debug0/GPUprint false, Console closed, HUD visible. Freecam ON (-700,200,121),
REQUEST pitch0.28/yaw-0.747; actual pitch-0.207930624485/yaw0.727235555649.
Last inspected capture `.research/captures/033416-material-stride-final.png`
shows the inn, including still-incorrect fluorescent green/yellow alchemy glass.
No exterior sky/LOD visible in this limited view. Runtime dome registered1,
instanced0/active0/texture65535; CS reports sky mode2 with no sky submitted and
native-world boundary suppression of12 draw batches/2 compute dispatches.
All build/test sessions terminal. No deletions or saved graphics changes.

Verified built/deployed hashes:
- CS `4FA433FD0164EAEA7D1EEDC46BE758C903DC7D7894AEC833B13F2B0FC2F571DD`
  label `20260920-material-stride-node-guard`, optimized BuildDev Release.
- D3D11 `BE90520A80A9B20FEDD474FF7B7DD245EC1219821A0476BD8D176EE61EC44B53`
- DXGI `066D721569675B2172754F23A40DA3044E873B8B167A628A3534DA4780EF6788`
  runtime label `20260920-material-stride`.
Backups/symbols under corresponding `.research/deployment-backups` and
`.research/deployed-symbols` labels. Active runtime log `.research/testlogs/remix-dxvk.log`.

Material defect: GPU records are112 bytes, but CPU subsurface and portal packers
advanced96. Extension uploads concatenate using returned offsets, so shader
112-byte indexing read wrong data after the first subsurface record. Shared
SURFACE_MATERIAL_GPU_SIZE now drives CPU/GPU record and padding sizes; existing
payload semantics unchanged. See `remix-material-layout.md`. Production-packer
negative control failed before the edit; final dedicated `_Comp64UnitTest`
test_material_layout, test_skinning_basis and test_native_basis_vertex pass3/3.
Release padding deliberately remains caller-initialized; an initial test's
zero-padding assumption was corrected, not treated as another runtime bug.
Shader build followed by fresh full Release build passed; usual external PDB
warnings only. Runtime.patch NOT regenerated this turn.

Crash: former PID53728 died03:05:43 at RemixSceneGraph::Gather+0x667,
RemixSceneGraph.cpp:117, dereferencing a null NiTArray child iterator (RBX0,
end8). Dump/runtime log archived as20260920-030543-land-tbn-crash.* under
.research/testlogs; matched20260920-land-tbn symbols. NiTArray range iteration
uses capacity. New RemixNodeChildren.h ChildSlots snapshots pointer and bounds
by min(free_idx,capacity), returning empty for null data. Gather uses this span;
switch-child bounds now use occupied high-water slots, not nonnull element count.
tools/remix/CheckNodeChildren.cpp passes null data, sparse slots, bounded malformed
high-water and empty reserved-capacity cases. Underlying allocation/lifetime
timing is not fully traced; this guard is NOT proof all races are solved.
No new dump observed during approximately7 minutes of subsequent smoke testing.

Validation on PID44912:
- Landscape100 imports/0 failures,48/48 bounded nearest samples have authored
  tangent basis. Six-layer63, five20, four8, three5, two2, one2.
- Two inn/Riverwood/trader/Riverwood rounds:8 console transitions passed;
  settled inn808/trader371 submitted, all9 exterior-negative groups absent,
  flames6/2 and heads2/2. Same process, third-person throughout.
- Three native ObjectReference.Activate(player,false) door calls succeeded:
  entry0x13424 ->inn, exit0x13419 ->Riverwood02, entry0x13424 ->inn.
  This is not walking-input, pacing, fast-travel or per-frame GPU leakage proof.
  SetPov first was attempted, but idle vanity took over: first-person NOT verified.

Inspected matching-camera captures:031602-material-stride-before and
032910-material-stride-after at Riverwood mill show no obvious new terrain/water
regression; water remains dark/over-reflective with incorrect ring detail.
031931-material-stride-inn-before and033039-material-stride-inn-after still show
fluorescent glass. NPC changed position, so no character improvement claim.
033128-material-stride-first-exit is misleadingly named (camera was vanity);
033240-material-stride-character and033322-...-character-reverse are bad wall
framing, NOT character validation. Final restored view is033416 above.

Water source audit, no water implementation this turn: native above-water
shallow/deep/depth-fog and weather/light modulation are missing from current
Beer-Lambert-only transport. Parameter24 noiseFalloff is not currently packed;
GPU bytes108..109 are sampler-feedback stamp, NOT spare falloff space. Updated
`remix-water.md` with native equations/offsets and prospective extension-buffer
approach. Do not mask parity problems with exposure or emission adjustments.

## Continuation 03:02 — terrain authored basis deployed; native water reference captured

Previous turn classified progress. This turn extends native tangent transport to
terrain while preserving six-layer weights, adds production-packing tests and
live regression, builds/deploys, and captures a native control. Goal ACTIVE and
far from complete. No native normal/depth/albedo parity or full temporal proof.
Exact intermittent interior-cloud/LOD report remains unqualified. Fluorescent
glass/bright beams, character/foliage issues and broader requirements remain.

Current PID53728, started03:00:38, Riverwood, Remix Scene/nativeReference=false,
suppressWorld=true, debug0/GPUprintfalse. Restored stationary freecam at
(20500,-44500,550), REQUEST pitch1.1,yaw0; actual pitch1.099998474,yaw-0.0.
This looks down at mill water/riverbank, NOT a dry road. HUD only, Console closed.
All build/test/setup sessions terminal. No deletes or saved graphics changes.

Final runtime `20260920-land-tbn-resolve`:
D3D11 `C815644739704FD25E6FF8C55B7C3C248977A5C9D227905DC26735E70DB299D2`
DXGI `066D721569675B2172754F23A40DA3044E873B8B167A628A3534DA4780EF6788`.
CS deployed under `20260920-land-tbn`, unchanged for final shader rebuild:
`66314E396C6EC3608C49DC5FAD7E4BB60451E5E28A616A2AE10D6271508758E9`.
Built/deployed hashes match. Backups/symbols in usual corresponding directories.
Intermediate D3D11 `1B63D6A240B4886125A86DBF178E205E22DF22FB5A803790E1EA085A6B28F369`
ran as PID39320, superseded. PID52428 was native-reference control, quit normally.
Earlier PID20152 also quit via paused Console qqq. Archived runtime logs:
20260920-before-land-tbn.log, 20260920-land-tbn-intermediate.log,
20260920-land-tbn-native-control.log in .research/testlogs. Active remix-dxvk.log.

Implementation: csRemixCreateMeshTBNV2(info,T/B,count,flags,out) accepts flag8
for landscape. V1 export remains flag0 wrapper. Reject unknown flags, missing
data, count mismatch, nonfinite basis, skinned terrain. New shared CPU helper
rtx_native_basis_vertex.h writes N/T/B, position, UV, color and optional two raw
weight words. Ordinary/MSN stride remains60; landscape68, weights at color+4/+8.
Exact interleaved GPU copy preserves layout. All existing six-layer material
textures and interpolation-before-normalization retained. No per-frame CPU work
added. CS removes terrain exclusion from basis extraction, passesflag8. Missing
native attributes still use old terrain API. GPU shader terrain branch explicitly
calls shared nativeTangentNormalToWorld, as ordinary branch does, preserving
authored N rather than substituting adjusted geometryNormal. Final Remix bending
and backface convention remain. Source/limits in remix-normals/remix-landscape.

Build/unit/deploy skills used. Release API build and optimized CS BuildDev passed.
New unit target initially absent from Meson introspection; explicit reconfigure
then dedicated _Comp64UnitTest build passed. test_native_basis_vertex and
test_skinning_basis passed2/2, 0.03s each. New unit exercises actual packer, both
strides, two consecutive vertices, identity MSN, arbitrary mirrored N/T/B,
position/UV/color, all8 raw weight bytes including sums>255, byte guards. Not GPU
execution proof. Final shader change built rtx_shaders first then fresh full
Release embedding/link, passed. Usual missing external PDB warnings. Diff-check
passed; modified PS script parsed. No unit tests executed from Release directory.

CheckLandscapeRuntime now optional -RequireTangentFrames; bounded48-nearest
submitted terrain sample, NOT whole-scene/GPU census. Negative control old build
failed48/48 missing basis. Intermediate and final builds pass48/48, matched100.
Material imports100, failures0, sixLayer63, five20, four8, three5, two2, one2.
Final roundtrip inn/Riverwood/Trader/Riverwood passed4 console transitions,
PID53728, third-person throughout, frames2302..3253. Inn808/Trader371 submitted,
Riverwood4494/4499; all9 exterior-negative groups absent, flames6/2, heads2/2.
Settled load/plugin membership only, NOT door walking, fast travel, pacing or
per-frame GPU leakage. Returned stationary mill camera afterward.
Post-roundtrip cumulative imports215, failures0, sixLayer137, five43, four16,
three11, two4, one4; live48/48 basis still passes. Final capture
030341-land-tbn-after-roundtrip.png inspected with world/water/HUD present.

Captures inspected (.research/captures):
- Old 024735-land-tbn-before-normal has foreground branches; poor framing.
- Better old 025218-land-tbn-open-before-normal /025220-...-albedo.
- Intermediate 025504-...-after-normal /025506-...-after-albedo /025508-...-after-lit.
- Final 030137-land-tbn-final-normal /030139-...-albedo /030141-...-lit.
Same requested camera for better old/intermediate/final: terrain layer pattern
retained visually, no disappearance, no quantitative/native buffer comparison.
An attempted Node sharp pixel comparison failed import resolution before any
measurement; DO NOT report numerical agreement.

Native reference control selected at MAIN MENU before loading, shadercache218/218,
failed0, then captured030013-land-tbn-native-lit at matching camera; inspected.
Native shows green depth-faded water; Remix much darker/more reflective, strong
bright ring details, differently composited foam. This is clear visual remaining
work, not a claim terrain transport caused it. Normaldebug shows opaque riverbed,
NOT final water normal. Lighting/time/exposure were not numerically normalized.
No raw native-buffer capture available in current DevBench: CS capture supports
screenshot/renderdoc only. Need proper native normal/albedo/depth capture path
for real parity; a lit screenshot cannot certify this. Next useful investigations:
native water depth-color/absorption and source normal equations, or opt-in raw
native-buffer QA; do not mask this with exposure/quality adjustments.

## Continuation 02:44 — authored tangent basis deployed; interior regressions passed

Goal remains active/unqualified. Ordinary mesh tangent transport corrected from
native Lighting.hlsl source, NOT a claim of native normal-buffer parity. The
exact intermittent exterior-cloud/LOD report was not reproduced. Fluorescent
alchemy glass and excessive indoor beams remain; full character, terrain/grass,
temporal, navigation, depth/albedo/normal parity and other goal requirements open.

Current PID20152, started02:38:11, RiverwoodSleepingGiantInn, Remix Scene,
nativeReference=false, suppressWorld=true, debug0. HUD only, Console closed,
GPUprint never enabled this turn. Freecam ON (-700,200,121), actual pitch
-0.207930624485/yaw0.727235555649. Capture REQUEST pitch0.28/yaw-0.747 reproduces
this orientation; do not pass actual reported Euler angles back. Last inspected
024415-authored-tbn-inn-final.png still shows fluorescent alchemy glass. Above
ceiling 024413-authored-tbn-inn-above.png (z1300) is black with HUD, no visible
exterior sky/LOD. Runtime dome at02:44 registered1, instanced0, active0.
All tool/build/test sessions terminal. No deletes or saved graphics changes.

Matching plugin/runtime deployed overwrite-only with backups and symbols under
`20260920-authored-tbn`:
- D3D11 `EE1018CFDF71D480B3070DF63F6A5582F6A1F855D7E399F56F4D7D8A10E5EEA3`
- DXGI `066D721569675B2172754F23A40DA3044E873B8B167A628A3534DA4780EF6788`
- CS `FA43EF49A17EF704EDFA2A9FF6ED5423FD55F5515A7AC7639BBACB345B5873AC`
PID53160 quit normally via paused Console qqq. Previous runtime log archived
`.research/testlogs/20260920-before-authored-tbn.log`. Active runtime log is
`.research/testlogs/remix-dxvk.log`, NOT game-root SkyrimSE_d3d11.log (old).

Source proof: Lighting VS packs T=(position.w,normal.w*2-1,binormal.w*2-1),
B=binormal.xyz*2-1,N=normal.xyz*2-1. Previous generic UV-derivative basis loses
authored tangents. New RemixTangentFrame::Decode and csRemixCreateMeshTBN copy
six floats T/B per vertex into a 60-byte N/T/B interleaved layout. Optional API
validates single surface, matching nonzero count, finite components and copies
synchronously before deferred upload. Requires matching new runtime. Instance
surface flags0 bit2 carries nativeTangentFrame. Existing nine-float GPU skinning
enabled for either MSN or authoredTBN; hit shader transforms/normalizes columns
per vertex and after interpolation, material evaluates sampled RGB against them.
No new per-frame CPU skinning/readbacks. See remix-normals.md for limitations.
MSN sample.xzy is SOURCE-CORRECT; do not change it to RGB. Terrain still needs
its basis alongside layer weights; cannot reuse layout while dropping weights.

Build/unit/deploy skills followed. Shader-only meson compile passed, followed by
full Release embedding/link (usual missing external PDB warnings). CS first build
failed C4459 local frame hiding global; renamed decodedBasis and rebuild passed.
Standalone CheckTangentFrame passed. Dedicated unit build/test_skinning_basis
passed1/1 (0.03s), including arbitrary mirrored/nonorthogonal N/T/B blended bones.
CPU equation checks only, not GPU execution proof. Diff checks passed.

Live census after Riverwood load reports nativeTangentFrame=true on ordinary
rocks, shrubs, ivy, stumps, armor, hair/eyes/mouth; MSN head/body remain separate.
023429-authored-tbn-before and023938-authored-tbn-after are same-camera Remix
normaldebug16 comparisons, inspected; no native reference comparison. NPC moved
between them. 023940-authored-tbn-lit inspected. 023933-authored-tbn-riverwood
inspected: world/UI visible, dark foliage remains, not native appearance parity.

New-build CheckCellRoundTrip one round inn/Riverwood/Trader/Riverwood passed4
console transitions in same PID, third-person throughout, frames31941..32926.
Inn809/Trader371 submitted; Riverwood4476/4402. All9 exterior-negative filter
groups absent in both interiors; positive flames6/2 and heads2/2. Separate inn
check also passed. These are settled plugin-membership/load checks, NOT physical
door/navigation, transient per-frame GPU membership or pacing. Returned to inn
for above-ceiling and final captures described above. User issue remains open.

## Continuation 02:25 — authored soft-depth path deployed; actual inn door tested

Goal remains active/unqualified. This continuation corrected one missing native
effect equation. It did NOT fix fluorescent glass or bright cloud-like indoor
beams, nor reproduce the user's exact exterior-cloud/LOD artifact. No normal
mapping, character, full navigation, temporal or buffer-parity certification.

Current game PID53160, started02:21:00, RiverwoodSleepingGiantInn, Remix Scene,
nativeReference=false, suppressWorld=true, debug0. HUD only, Console closed,
GPUprint disabled/defaults. Freecam ON at(-210,-200,121), actual
pitch-0.207930624485 yaw0.727235555649. Capture.ps1 REQUEST pitch0.28 yaw-0.747
to reproduce this orientation (do NOT feed the reported Euler angles back).
Last inspected capture022458-native-soft-hearth.png: hearth/flames/UI visible,
white indoor beams still excessive, fluorescent alchemy glass behind hearth.
022438-native-soft-alchemy.png confirms glass still wrong. No improvement claim.
All tool/build/test sessions terminal.

Runtime deployed overwrite-only with backups/symbols `20260920-native-soft`:
D3D11 `19BE2DCC3B9FCBC3350639D328DB85FBF13CFAE9B95EA328813AE717E828216B`
DXGI `066D721569675B2172754F23A40DA3044E873B8B167A628A3534DA4780EF6788`.
CS unchanged `3EB1971C39160F99692ABD3F96F3EA7476E728807C03D51C291D4A5A77843AFF`.
PID52908 exited normally via paused Console qqq. Previous runtime log archived
as `.research/testlogs/20260920-before-native-soft.log`. No deletes or saved
graphics-setting changes. Native door transition did trigger the game's autosave.

Source: native Effect SOFT uses clip/view depth divided by material softDepth,
and multiplies opacity BEFORE PropertyColor.w / grayscale-alpha palette lookup.
Parameter14/flag256 already transported this data. Both unordered RTX resolvers
now set SurfaceInteraction.nativeEffectSoftOpacity before material evaluation.
Primary gaps project ray.direction through worldToView; secondary rays use a
ray-local approximation. Nonpositive range gives a guarded hard intersection.
All native effects bypass generic particleSoftnessFactor fade (including those
without SOFT). Other materials unchanged. No packed GPU/API changes or added
CPU scene work. Details/limitations in remix-effects.md: native 0.003 discard,
FALLOFF/MULTBLEND exception, portal/PSR rays and native/RTX depth disagreement
still unqualified. Engine lighting and gamma-space/ordered compositing remain.

Repository build/unit/deploy skills used. Shader-only meson build passed, then
full Release embedding/link passed (usual missing external PDB warnings).
Dedicated test_native_effect rebuilt. First test invocation used wrong name
`native_effect` and matched none; corrected names passed3/3: test_native_effect
0.07s, test_native_refraction0.12s, test_retained_retirement0.13s. Effect test adds
depth clamp, oblique projection, invalid range and camera-distance independence.
These are scalar CPU tests, not GPU/native image parity. Runtime diff-check pass.

Live tests: one CheckCellRoundTrip round across inn/Riverwood/Trader/Riverwood
passed4 transitions, frames1540..2518, PID53160, third-person throughout. Inn808,
Trader371, Riverwood4490..4605 submitted. Nine negative exterior groups absent;
positive flames6/2 and heads2/2 present. Then loaded inn and tested REAL door
activation through Papyrus ObjectReference.Activate(player,false):
- Inn exit ref0x00013419 -> Riverwood02 (Tamriel), player(23259.77,-44476.16,-8.28).
- Nearby sole exterior door ref0x00013424 -> RiverwoodSleepingGiantInn.
Both calls returnedtrue, same process survived, interior membership passed again
(810 submitted, flames6, heads2). This tests native door activation, NOT walking
to doors, input responsiveness or consecutive-frame pacing. Idle vanity timer
overrode third-person after this manual test; don't report third-person stability
for the door round trip. 022354-native-soft-door-entry inspected, HUD/autosave
and interior visible, no exterior sky/LOD in that view. Runtime dome at02:25:
registered1, instanced0, active0. Indoor BeamMeshStatic01 is CloudTileLight.dds
under FXAmbBeamDust02, flags4472, softDepth64; billboard DustTiny sibling4464.
Do NOT remove these legitimate effects or broadly exclude ObjectLODRoot.

Capture attempts021700 and021951 used wrong requested orientation (wall/black
view), unsuitable for before/after parity. 022023 was moving vanity camera.
Use documented REQUEST angles above for subsequent native/RTX comparisons.

## Continuation 02:08 — explicit retained retirement fixed; interior report not fully qualified

Goal remains active. This continuation made a source-proven lifetime correction,
deployed it, and tested two interiors. It did NOT reproduce/prove the cause of
the user's exact exterior clouds/LOD artifact. Normals, fluorescent glass, overly
bright indoor dust beams and the rest of the full goal remain unresolved.

Current game PID52908, started02:03:21, RiverwoodSleepingGiantInn, Remix Scene,
nativeReference=false, suppressWorld=true, debug0, GPUprint disabled/defaults.
Freecam OFF; the idle game entered native vanity POV even after setPov third.
HUD Menu is the only open menu; Console closed, no held input. Last captures
020635-retirement-inn-above.png and020637-retirement-inn-final.png inspected.
Above-ceiling view has black background with some dark inn geometry below, no
exterior clouds/LOD. Final ordinary view has legitimate but overbright dust beams
and fluorescent alchemy glass. Do not call it visual parity.
All build/test/deploy/live-command sessions terminal.

Runtime deployed with overwrite-only backups/symbols under
`20260920-retained-retirement`:
D3D11 `98595AE38D99D645FD044C0A8B77D59D128B1BF427F019B840D8887369D3891B`
DXGI `066D721569675B2172754F23A40DA3044E873B8B167A628A3534DA4780EF6788`
CS unchanged `3EB1971C39160F99692ABD3F96F3EA7476E728807C03D51C291D4A5A77843AFF`.
No deletes or saved-setting changes. PID44368 exited via paused Console qqq;
Wait-Process raced its successful exit and reported nonexistent PID, subsequently
verified absent. Runtime log archived as
`.research/testlogs/20260920-effect-layer-probe-before-retirement.log`.

Runtime correction: removeRetainedExternalDraw previously only cleared hostOwned
and hostRetainedHandle, allowing normal unseen-draw lifetime/anti-culling to retain
its prims. New ReplacementInstance::releaseHost clears ownership AND calls clear:
mesh/light GC marks, graph removal, owner-pointer detach and bounds invalidation.
Used both explicit removal and old-node reassociation. Next GC/TLAS excludes old
prims. Tracker node itself can be reused. Full rationale/scope in
`docs/development/remix-retained-lifetime.md`.
IMPORTANT: DestroyMesh already removes replacement nodes by spatial-map hash.
Thus this correction matters for instance removal/reassociation WITHOUT mesh
destruction; do not overclaim it explains all cross-cell artifacts.

Build/unit skills followed. Release meson compile passed (usual external PDB
warnings). Dedicated _Comp64UnitTest rebuilt test_retained_retirement. First runs
failed in Windows loader C0000135 (NRC_Vulkan and USD support DLL search paths),
not assertions. Test-specific Meson environment now includes nrc_dll_path and
usd_lib_path/bin plus /lib (usd_lib_path is Release ROOT here, not its lib folder).
Fresh no-shell-override run passed retained_retirement/native_effect/
native_refraction 3/3 at02:08 (0.04/0.01/0.03 seconds). Retirement test covers
same-frame mesh retirement, null prim slots, ownership/bounds reset, repeated
retirement, and node reuse; not Vulkan/light/graph integration. Runtime diff-check
passed with CRLF warnings. Changed PS scripts parsed successfully.

CheckInteriorMembership now accepts ExpectedCell inn or Trader. CheckCellRoundTrip
accepts Interiors route and checks membership after each settled interior load.
Two rounds inn/Riverwood/Trader/Riverwood passed8 transitions, PID52908 throughout,
third-person throughout test, frames1988..3977. Inn808 submitted, Trader371,
Riverwood4263..4381. All9 exterior negative groups absent; flames6/2 and heads2/2
positive. These are console-load and settled plugin-membership checks, NOT physical
doors/navigation, per-frame GPU membership or pacing. Captured020524 Riverwood
afterwards; inspected, world/UI/third-person render but full correctness unqualified.
Later returned inn and checked runtime dome registered1,instanced0,active0 at02:07.

Before deployment, Trader also had all9 groups absent, no geometry with
BSTempNodeManager ancestor, dome inactive;015412-trader-entry and015743-trader-
before-above inspected (above view black). Global tempNodeManager traversal indoors
remains a source to audit, NOT demonstrated as a leak. Do not remove it blindly.

### Filtered effect GPU diagnostics completed before lifetime investigation

Intermediate runtime `20260920-effect-layer-probe` D3D11
`6B09A9862700DC886C0FEBAEF1086A4FEA14F1D5892306905E97C17C1C67A687`
was deployed at01:48, now superseded. It added GPUprint nativeEffectFlags and
nativeEffectPhase (both default-1), passed through debugKnob x/y only when printing.
Phases0..7: bindings, sourceRGBA, resolvedRGBA, UV, vertexRGBA, alphaFactors,
emissionAndOpacity, blendClassification. Exact flags filter; still LAST MATCHING
layer at pixel, not identity/order/count. RtxOptions.md regenerated via one child
launch with DXVK_DOCUMENTATION_WRITE_RTX_OPTIONS_MD=1. Owned generated game-root
RtxOptions.md retained, repo copy updated. No persistent env override.

New tools/remix/ProbeEffectLayer.ps1 -Flags4208 -X820 -Y347 completed successfully.
It restores printingfalse, requireCtrl/useMousePositiontrue, flags/phase-1 in finally.
GPU evidence at inn alchemy camera(-700,200,121): bindings4208,13,32768,3;
sourceRGBA about(.129412,.172549,.094118,.835); resolvedRGBA about
(.40..43,.511581,.0066435,.029..039); vertexRGBA(1,1,1,1);
alpha factors(1,.034..046,1,.83..84); emitted radiance about
(.0086..0095,.0104..0118,.000013..000015), opacity.014..016;
blend classification(1,1,1,emissionWeight.029..035). UV scale animated near1.6.
The initial record in phase4 was stale previous-phase data from async GPU readback;
laterrecord(1,1,1,1) correct. Different phases may see different glass layers.
015011-alchemy-layer-probe inspected: glass STILL fluorescent. No authored colour
or intensity adjustment. These data narrow shader-input hypotheses but don't
prove final ordered composition or exposure. Continue those separately.

## Continuation 01:43 — native effect radiance transfer corrected; glass still wrong

Previous turn classified progress. This turn corrected a source-proven equation,
built/deployed it and obtained fresh live evidence, but did NOT solve the
fluorescent glass appearance. Full goal remains active/unqualified.

Current PID54112, started01:38:19, RiverwoodSleepingGiantInn. Normal Remix Scene,
nativeReference=false,suppressWorld=true,debug0. Freecam(-700,200,121), actual
pitch-0.207930624485 yaw0.727235555649. HUD visible, Console closed, noheldinput.
GPUprint disabled again; requireCtrl/useMousePosition restoredtrue. Its pixelIndex
still759,319 but inactive. Lastcapture013943-alchemy-native-radiance.png.
All build/test/setup sessions terminal; no process restart needed for deployment.

Deployed matching plugin/runtime under `20260920-effect-native-radiance`:
CS `3EB1971C39160F99692ABD3F96F3EA7476E728807C03D51C291D4A5A77843AFF`
D3D11 `E6ECC0AF8D47FC000D924A3147EEEF81F89CE9FCA700AD5D9707CB5ACCFB81D7`
DXGI unchanged `066D721569675B2172754F23A40DA3044E873B8B167A628A3534DA4780EF6788`.
Previous CS B01EE281...,D3D1183E11F15... backedup in deployment-backups;
new DLL/PDB archives in deployed-symbols. PID43776 quit via pausedConsoleqqq.
Overwriteonly ownedfiles, no deletes/savedsettingschanges. Runtime build,
unit-test and deploy-and-test-game skills read/followed.

Source proof: Color.hlsli RadianceToLinear/SkyrimGammaToLinear use pow(abs(c),1.6)
when LinearLightingoff; Effect.hlsl leaves authored gamma RGB until framebuffer.
Runtime nativeEffect path had generic gammaToLinear=pow(c,2.2). HDR3 decodes to
5.799546 vs11.211578; not a reason to lower authored baseColorScale3.
RemixScene now sets effectflag4096 when GetCommonBufferData().enableLLfalse.
New shared nativeEffectRadianceToLinear scalar helper; shader
nativeEffectDecodeRadiance chooses1.6 forflaggedeffectsalbedo/emission/external
colour, otherwise previous2.2. APIcreate/update limits extend4092->8188 toaccept
bit12. Existing20float/112byteGPUlayoutunchanged, uint16effectflags carriesbit.
Refractionflagpath notgivennewcolorflag. Newplugin mustusematchingnewruntime.

Limits: no-flag LL-enabled branch is still unqualified (doesnottransportnative
effectGamma/multiplier). Native gamma compositing then decoding NOTequivalentto
linearblending decodedlayers. Orderednode semantics, softintersection,
nativeenvironmentglassinputs and exposure remainopen. Do NOT claimimageparity.

NativeEffect unit adds0,1,.5,-.5,3 colourchecks to angular tests. Dedicated
_Comp64UnitTest test_native_effect+test_native_refraction 2/2pass0.11seach.
One shell then attemptedcmakefromruntimecwd andfailed (wrongdirectory), but
separatecorrect-rootCSReleasebuildpassed. Shader-onlymesonpass and subsequent
fullReleaseC++/embeddedshaderbuildpassed; unrelatedmissingSentryPDBlinkwarnings.
No tests executedfromgamebuilddir (fullbuildcompiledthem as defaulttargets).

Inspectedmatchingcloseups013446-before and013943-after versus native003011.
Glass remainsobviouslyfluorescent, nooverallappearancefix. NPC/UV/timeconditions
differ; notpixelparity. Nativecolour/scaleunchanged. BothInnerHaze02 effects
reportflag4208,scale3,submitted/animated. EffectinputsInnerHaze2/2 andFlames6/6
triangleUV(native+import)pass;refraction3/3UV/strengthpass;inn directional3samples
6705-6787pass;all9exteriornegativegroupspass,Flames6/heads2positive.

GPUprobe enabledbrieflythroughAPI. Pixel615,260 nohits;820,347,810,360,830,352,
759,319 readflag4400,palette72,material32768,intensity1 (smokelayer, notglass).
Showsnew4096bit survivesGPUtransport; last-hit-wins phases cannotproveglass
identity/finalradiance. Restoreddebugsettings afterprobe. Archivedruntime log
`.research/testlogs/20260920-effect-native-radiance-gpu.log`.

Nextaction: investigateactual orderedglass/effectcomposition and exposure with
layer-specificGPUdiagnostics, not repeatedgenericUVchecks or arbitraryscale.
Native reference entireinn is brighterwhilevesselsdimmer; globalexposure and
missingnativeambient vs pathtracedlighting mayamplify the apparentemission
mismatch. This is a hypothesis, notprovedrootcause. Normals/SSS/navigation,
temporalparity and other fullgoal requirements remainopen.

## Continuation 01:30 — live native directional light and reliable frame freshness

Previous turn: progress on vertex RGBA. This turn: progress, with two deployed
corrections and in-game evidence. Goal remains active, NOT qualified complete.
Latest user report exterior clouds/LOD indoors remains only partly investigated;
settled Sleeping Giant Inn now shows no exterior sky or LOD in checked views.

Current PID43776, started01:25:12, RiverwoodSleepingGiantInn, normal Remix Scene,
nativeReference=false, suppressWorld=true, debug0. Freecam(-210,-200,121), actual
pitch-0.207930624485 yaw0.727235555649. HUD visible, console closed, no held input.
Final capture012821-native-sun-final-inn.png. All build/test sessions terminal.
No game files deleted, no saved settings modified. Deploy-and-test-game used.

Deployed plugin `20260920-native-directional-present-frame`, SHA256
`B01EE281993E7A5A65520185D2F0E2156AF099C44865A5FE06F4169179011B00`.
DLL/PDB archive and replaced-plugin backup under usual .research directories.
Runtime unchanged effect-vertex-falloff-final (D3D11 83E11F15...,DXGI066D7215...).
Intermediate native-directional-light plugin1A24609F... superseded after finding
bad direct engine frame field; previous PID46292 exited cleanly via console qqq.
Earlier PID46360 also exited cleanly. Release build passed after adding State.h.

RemixScene no longer creates a fixed sun at initialization. CaptureSceneLights
copies native shadowSceneNode[0].sunLight NiDirectionalLight direction,diffuse,
fade,HDR sunlightScale and LinearLighting settings at world boundary. Normalize
native ray direction unchanged (Skyrim shader L is its negative). New helper
RemixDirectionalLightMath.h maps normal-incidence unit-white Lambert intensity:
disabledLL=pow(diffuse*fade*scale,1.6), enabledLL mirrors native gamma/multiplier
and /pi normalization. No extra solid-angle division: Remix distant_light.slangh
already divides by sin(halfAngle)^2, which cancels Lambert cone integral. Disk
diameter remains approximate0.5deg. Generic nonlinear native lighting, fog,
PBR compensation and sun/dome double contribution are NOT qualified.

Submit uses stable hash0x43535343454e4502, updates native values, refuses old
cell/frame captures and destroys inactive light. Discard clears capture/light.
Inspector filterdirectional exposes capturedFrame,submitFrame,frameSource
CS-present and native/mapped values plus submission state (CPU/API, notGPU).

IMPORTANT second correction: direct CommonLib BSGraphics::State::frameCount
at+0x4c is NOT a valid counter on1.7.99. Live values965249161/3112732809 repeat
and alternate while actual framesadvance. Both newly added sun freshness AND
existing WorldCameraCall/ReadWorldCamera checks had used this field. Both now
use globals::state->frameCount, incremented once in IDXGISwapChain_Present after
world capture/UI submit. Source confirms same-frame lifetime. Live counters
advance normally and remain equal at directional capture/submit. This removes
an invalid camera freshness predicate; NOT proof all jitter is solved.

New CheckDirectionalLightMath.cpp standalone helper test passed12 scalar checks
incl independent numerical cone integral. New CheckDirectionalLight.ps1 passed
6Riverwood samples frames2965-3023,3afterroundtrip4331-4353,6inn7291-7490.
InitialPS test incorrectly rounded via integer Math.Max overload; corrected to
explicitdouble. Productionradiance unchanged. Inn native direction
(-.86602545,0,.50000006),radiance(.21475491,.24179018,.29938203). Keep this
legitimate template directional light indoors, doNOTblanketdisableallinteriors.

CheckCellRoundTrip1 passed inn3048-3277/808submitted -> Riverwood3309-3490/4465
in thirdperson. Subsequent pausedconsolecoc returnedinn. All9exteriornegative
membership groupspass;flames6/heads2 positive. Runtime dome registered1,
instanced0,active0 indoors at01:27:59-01:28:01. Effects native+import UV checks
InnerHaze2/2,flametriangles6/6;refraction3/3native+importedanimation pass. None
of these qualify GPU temporal/image parity or real doors/fasttravel.

Inspected012633-native-sun-riverwood.png,012733-native-sun-inn.png,
012750-native-sun-inn-above.png. Matching aboveceilingcamera(-210,-200,2000)
now black background, no formerlylitgreyplane. Hearth/UI rendercorrectly enough
to inspect, alchemyglass STILLfluorescentgreen/yellow (unfixed). Restoredcamera
tohearth and captured012821final. Exactpreviousplanehitidentity stillunproven.
Directional fix plausibly explains litoutsideplane but not every userreport.

Next: continue exact native/Remix material parity for fluorescent glass and
normals, plus test other interiors and transition frames for reported clouds/LOD.
Do not reclassify legitimate indoorCloud-textured dustbeams or ObjectLODRoot
children as exteriorcontent. Fullgoal still has many unresolved requirements.

## Continuation 01:13 — native lighting vertex RGBA restored and deployed

Previous turn classified as progress: interior/exterior membership evidence
changed the next inspection. This turn made a verified importer correction.
Goal remains active; glass glow, grey planes and full rendering parity NOT fixed.

Current PID46360 started01:09:10, in RiverwoodSleepingGiantInn. Normal Remix
scene, nativeReference=false, world suppression=true, debug0; freecam
(-210,-200,121), actual pitch-0.207930624485 yaw0.727235555649, HUD visible and
console closed, no held input. Final capture011215-vertex-colors-final-inn.png.
All build/setup/test sessions finished. Previous PIDs15924 and23772 quit via
paused console qqq. No game files deleted or saved settings edited.

Deployed plugin `20260920-native-lighting-vertex-color`, SHA256
`FACE1E91D439F8280F6929FA8DB7D36EA50D348414071C779F059E966CCCD595`.
Intermediate diagnostic buildD4B14F11... under material-input-diagnostics was
superseded. Backups and DLL/PDB archives in usual deployment-backups and
deployed-symbols directories. Runtime unchanged from effect-vertex-falloff-final
(D3D11 83E11F15..., DXGI066D7215...). Deploy-and-test-game skill followed.

RemixScene.cpp now imports generic lighting vertex RGB when native VertexColors
and descriptor VF_COLORS are enabled. Composes native material alpha with source
vertex alpha, packs BGRA and enables texture*vertex modulation. TREE_ANIM and
LOD object alpha is excluded from coverage; grass/hair/effect/native landscape
keep their separate channel handling. Mesh cache invalidates on colour mode or
material-alpha changes. This means animated material alpha currently rebuilds
vertex data; optimise later with explicit runtime scalar transport. Alpha is
quantized to8bit after composition, so not exact floating-point parity. No
authored intensity, texture sampler, normal or light changes in this build.

Completed uploadedColorSamples diagnostic. Inspector prefix `vertices:<filter>`
performs an explicit GPU readback of the cached native vertex buffer, reports
RGBA min/max/nonwhite counts and four native/uploaded corresponding samples,
plus blend stage state. No per-frame readback. Lighting entries now also report
native texture-set paths, material alpha, specular constants, emission and env
scale. Added CheckLightingVertexColors.ps1; script parser passed, then live test
passed11 records (one overlapping orb record), four samples each, zero channel
mismatches. Checked OuterGlass01/02, OrbOuterGlass, fiveFarmTable, twoMaleHeads.
Riverwood Farm/Rock query passed95 records, zero mismatches. Both tests prove
sampled transport/blend state, NOT GPU equation/image parity.

Native evidence before correction: OuterGlass01 allRGB77/255; OuterGlass02 RGB
min(0,222,0),max(18,255,255); OrbOuterGlass alpha12..255; FarmTable RGB150..255.
Importer formerly replaced all by white/opaque. MaleHead vertices allwhite;
this correction alone cannot resolve character-normal defects. Native shader
Lighting.hlsl copies VC at265, modulates RGB2637, alpha2814 excepttree/LOD.
Runtime API blend defaults isVertexColorBakedLighting=false (API>=0.5.2), so
native authored greys are not automatically normalized away.

BlackPlane01 materials actually reference textures/Black.dds and Default_n.dds,
with zero specularColor,zero emissiveColor,emissiveMult1,specularPower80. No
vertex colours. This rules out VC loss as their cause; SRV texels and exact
screen-hit identity still unverified. Generic importer ignores native specular
settings and uses roughness0.8; potentially relevant, not yet proven.

Inspected matching-view captures010609-before-native-lighting-colors.png and
011028-after-native-lighting-colors.png: alchemy vessels STILL overly luminous;
NPC/animation phases differ. Do not claim the entire material fixed. Inspected
011144-riverwood-native-lighting-colors.png: live third-person exterior scene.
Final inn membership passed all9 negative groups, flames/heads positive. Earlier
same process refraction3/3 native+importUV/strength pass; InnerHaze2/2 UV and
flames6/6 UV pass. These input checks don't qualify GPU temporal parity.

HIGH PRIORITY concrete next defect found: RemixScene::Initialize still creates
a hard-coded distant light direction(0.3,-0.4,-0.8660254),radiance(1,1,1),angle0.5,
and Submit unconditionally draws it in interiors too. No native sun updates.
This directly contradicts directional-light matching and may explain illumination
on surfaces above the inn. Do NOT merely disable all interior directional light:
native interior templates can provide one. Capture native shadowSceneNode[0]
sunLight->light as NiDirectionalLight during stable world capture, alongside
CaptureSceneLights (called from RemixNativeRender world hook). State.cpp1030
already derives native direction=-GetWorldDirection() for shader's toward-light
vector; Remix DistantEXT expects light's pointing direction (opposite shaderL).
Native colour=diffuse*fade*imageSpaceManager HDR sunlightScale. Need validate
photometric/gamma mapping, sun-vs-dome double contribution, light lifetime and
cell transitions. Existing hardcoded light is untouched, so fixing it is still
required. Use native source values and inspect both exterior and interior.

## Continuation 01:00 — reported interior clouds/LOD rechecked, not reproduced in settled inn

Latest user report is exterior clouds/LOD visible indoors. Investigated that
before continuing the prior vertex-colour work. No renderer code, build or DLL
deployment this turn; do NOT describe this as a newly fixed rendering defect.
Goal remains active. PID15924 is still running the 00:49 binaries.

Repeated inn -> Riverwood -> inn -> Riverwood -> inn in this process using
paused-console coc. CheckCellRoundTrip one round passed in third person:
inn frames73105-73360,809 submitted; Riverwood73395-73576,4451 submitted.
Later settled exterior had8348 instances. These are console transitions, not
walking/door/fast-travel or individual transition-frame visual qualification.

Expanded tools/remix/CheckInteriorMembership.ps1: negative checks now include
BSDistantTreeShaderProperty, BSSkyShaderProperty and lighting features9,15,17,18
alongside the previous three regression names. Positive controls remain flames
and characters. Script executed successfully: final inn835loaded809submitted,
all9 negative groups zero, sixflames/twoheads. Exterior controls demonstrated
the queries detect real objects: feature18=73, distant-tree=397, sky=41.
Runtime at00:57:49-51 reports809 instances, dome registered1/instanced0/active0,
textureIndex65535. No extra runtime instance count beyond plugin submissions.

Inspected captures:
- 005440-interior-current-membership.png: original alchemy view.
- 005529-interior-hearth-membership.png: hearth and indoor dust beams.
- 005531-interior-outside-shell.png: above roof looking down.
- 005748-interior-outward-after-transition.png: camera(-210,-200,2000),pitch0,
  yaw0; black background with a large grey rectangular plane at lower right.
- 005752-interior-restored-after-transition.png: final hearth view.
All in .research/captures. No exterior cloud/terrain background in inspected
views. This does not disprove the report in other cells/viewpoints or briefly
during transitions.

Important classification evidence: "LOD" matches831 legitimate inn objects
because ObjectLODRoot parents walls/furniture/actors. "Cloud" matches two
BeamMeshStatic01 under FXAmbBeamDust02, authored indoor dust beams, not weather.
Do not cull either by those substring names. The grey plane above the shell is
consistent with the two BlackPlane01:2 meshes at(1310.69,355.12,1362.32) and
(377.93,1446.64,1362.32), scale3.95, local bounds +/-256,2 triangles each.
Exact screen-hit identity/native colour not verified. Both shader flags
0000800182400300 lack vertex-colour bit37; thus the ordinary VC gap alone is NOT
a proven explanation for these planes. Diffuse/normal names empty in current
inspector, importedMaterial=true. Investigate native material/texture inputs
before deleting, hiding or recolouring them.

Current camera restored to freecam(-210,-200,121), actual pitch-0.207930624485,
yaw0.727235555649. HUD visible, console closed, no held input, normal Remix
scene/debug0. No pending tool sessions. Existing incomplete
Mesh::uploadedColorSamples field from the previous interrupted vertex audit
remains unfilled and unused; this turn did not extend or remove it.

## Continuation 00:49 — native effect vertex opacity implemented; glass still mismatches

Previous goal turn was progress (native comparison changed the next action).
This turn corrected one shader equation ordering, but did NOT fix the bright
alchemy glass or qualify the full goal. Keep goal active.

Current Skyrim PID15924 started00:47:17, alive frame5116 at00:48:50, in
RiverwoodSleepingGiantInn. Normal Remix scene mode, nativeReference=false,
world suppression=true, debug view0, HUD visible, console closed. Freecam
(-700,200,121), actual pitch-0.207930624485 yaw0.727235555649. No held input.
Final inspected capture `.research/captures/004840-alchemy-falloff-final.png`.
All build/test/setup/capture sessions finished; no pending handles.

Deployed runtime `20260920-effect-vertex-falloff-final`:
D3D11 SHA256 `83E11F1566035BFB00BA2FB7550C54C13DA173CDEFDE6E9717C2B87B3CC52A3D`.
DXGI remains `066D721569675B2172754F23A40DA3044E873B8B167A628A3534DA4780EF6788`.
Plugin unchanged, `20260920-native-refraction`, SHA271B7FAAFF2AB7C2EA87793DEC8C04F4D556A2762B2B264A2C55F898DD603D88.
Owned runtime DLLs backed up then overwritten only. No deletes or saved-setting
edits. PIDs53100 and42956 quit gracefully qqq. Intermediate runtime2CF4D811...
under `20260920-effect-vertex-falloff` was superseded to retain non-layer fallback.

Implementation details in remix-effects.md. New shared native_effect_math.h
evaluates native Effect VS angular opacity. surfaceInteractionCreate evaluates
it on each vertex normal/position, before hemisphere correction/bending, then
interpolates the scalar for native particle-layer effects. SurfaceInteraction
has a shader-local scalar, no GPU ABI/buffer-layout change. Primary camera is
used as angular reference even for secondary rays. Non-layer effects retain
their old hit-space fallback. No-normal/model-space-normal effect variants and
viewmodel-specific camera behaviour remain unqualified. No authored intensity
or sampler changed. No new per-frame CPU geometry upload.

Verification: build-remix, run-unit-tests and deploy-and-test-game skills used.
Both shader builds and subsequent C++ embedding/link succeeded (known missing
external PDB linker warnings only). Dedicated test_native_effect and
test_native_refraction passed2/2 at0.03s each; scalar CPU equations, not GPU parity.
Inspected intermediate004204 and final004840 images against native003011: glass
still too emissive; do NOT claim the material fixed. Interior membership final:
834loaded808submitted, zero exterior regression matches, sixflames/twoheads;
dome active0 textureIndex65535. InnerHaze02 native+importUV animated2/2.
Intermediate process also passed refraction3/3 strength/UV and flame animation
checks; refraction GPU mask was NOT recaptured this turn. No performance audit.

Next priority before guessing specular or brightness: ordinary lighting vertex
RGBA is currently lost. RemixScene::Upload sets white and restores native bytes
only for effect/landscape/grass/hair. Generic lighting blend selects texture RGB
and material alpha, without ordinary vertex modulation. Native Lighting.hlsl
uses input.Color.xyz at2637 and alpha*=input.Color.w at2814 (TREE_ANIM/LOD
exceptions). Inspect actual glass/static/character packed vertex inputs and
flags, then implement respecting special channel meanings. This source gap is
proven; its effect on the glass image is not yet measured.

Additional material gap: all14AlchemyWorkstation matches inventoried; glass
meshes OuterGlass02/InnerGlass02 are submitted, feature1(environment map),
PlainGlassTile01.dds/_n.dds, alphaFlags4333, BSOrderedNode. Importer gives them
generic roughness0.8, no native envMask/envScale/specular input transport.
CommonLib BSLightingShaderMaterialEnvmap has envTexture+0xA0,envMask+0xA8,
envMapScale+0xB0; base material specularColor+0x38,power+0x88,scale+0x8C.
Lighting.hlsl1883 onward uses mask*scale then native env sample; distinguish
path-traced material mapping from simply replaying cubemaps. LinearLighting
feature loaded=false in test. Also investigate colour-space/ordered attenuation.
Old GPU probe at744,312 hit flags304 smoke rather than the glass (later hits
overwrite earlier records), so it does NOT prove glass shader values. Probe
disabled again, requireCtrl/useMousePosition restored true before restarts.

## Continuation 00:33 — interior transition rechecked; native alchemy reference captured

Goal remains active. No renderer changes or new DLL deployment this turn.
The deployed native-refraction binaries/hashes below are unchanged.
Current Skyrim PID53100 started00:31:42, alive frame5005 at00:33:03 in
RiverwoodSleepingGiantInn. Normal Remix scene mode, nativeReference=false,
world suppression requested=true, debug view0, HUD visible, console closed.
Freecam (-700,200,121), reported pitch-0.207930624485 yaw0.727235555649;
Capture.ps1 requests pitch0.28 yaw-0.747 to reproduce this pose. No held input.
Final inspected capture `.research/captures/003254-alchemy-remix-restored.png`.
All script sessions completed; no build or analysis process pending.

Interior regression:
- PID37212 completed third-person inn/Riverwood round trip: inn frames78571–78824,
  810submitted; exterior78853–79039,4488submitted. This was console cell loading,
  NOT walking, doors, fast travel or a pacing qualification.
- Re-entered inn and checked membership: zero FXSplashLargeChurn/FXWaterfall/
  Tamriel matches, sixflames and twoMaleHeads submitted. Repeated after restart:
  834loaded808submitted, same negative/positive controls.
- Runtime dome evidence: outside00:32:27 instanced=1 active=1; inside00:33:01–03
  instanced=0 active=0 textureIndex65535. Retained registered dome is inactive.

Identified bright green/yellow object behind hearth as alchemy glass vessels:
InnerHaze02:8 and :13 under AlchemyWorkstation, not an editor marker or exterior
geometry. Native reference confirms an actual material/compositing mismatch.
Inspected images with identical reported camera pose:
- `002820-alchemy-remix-reference.png`: luminous cyan/yellow shells/dark centres.
- `003011-alchemy-native-reference.png`: subdued green/cyan glass and highlights.
- `003254-alchemy-remix-restored.png`: mismatch persists, Remix restored.
NPC positions and animation phases differ, so this is not pixel-perfect parity.

Added ConfigureRunningTest.ps1 -NativeReference, permitted only at Main Menu.
This session-only diagnostic is one-way until restart. Syntax check passed;
successfully selected before first world load in PID31988, loaded Riverwood,
declined Survival, then paused console coc to inn and captured. Loaded-world
guard tested and correctly rejected mode change. PID31988 quit gracefully qqq;
fresh PID53100 configured normally. PID37212 also exited gracefully. No deletes,
saved-setting edits, or binary overwrites. Deploy-and-test-game skill used.

Next effect work: do NOT arbitrarily reduce authored intensity. Existing native
SetupMaterial decompile at0x15569e0 confirms non-palette RGB is baseColor*scale;
these materials have scale3. Source PlainGlassTile01.dds BC3_UNORM77, repeat3,
no authored palette, flags112(VC RGB/alpha +falloff), alphaFlags4109(additive).
Both use falloff(.70710677,1,startAlpha,0), startAlpha1/yellow and.4/cyan.
Runtime UV/base/scale/falloff transport agrees with inspector, including active
UV index1 in restored process. External colourwhite, lightingInfluenceByte255.
Potential mismatches to investigate, NOT proven causes: native fade computed per
vertex then interpolated versus current per-hit bent-normal fade; blend/colour
space and surface ordering. Native PropertyColor/LightingInfluence applies even
without LIGHTING define; current native-effect shader gates RTX diffuse by128
and unconditionally multiplies external colour into emission. Reference mode
skips RemixScene::Submit, so remixScene inspector is empty in that mode.

## Continuation 00:22 — native refraction implemented, hearth mask animates on GPU

Goal remains active. This turn implemented and tested an initial native
refraction path; it does NOT qualify full image parity or the full goal.

Current Skyrim PID37212, launched00:17:43, alive at frame21779 at00:21:04 in
RiverwoodSleepingGiantInn. Freecam (-210,-200,121), reported pitch-0.207930624,
yaw0.727235556. HUD only, console closed, normal lit view0,
rtx.nativeRefraction.debugMask=false, no held input. Final capture
`.research/captures/002105-refraction-final-lit.png`.

Deployed runtime `20260920-native-refraction-bindless`:
D3D11 SHA256 `6D3622CA180EC5A6C24ACC3F6EC14C5D493BA41E38A4EAACD4ED39818C90F981`;
DXGI unchanged (`066D721569675B2172754F23A40DA3044E873B8B167A628A3534DA4780EF6788`).
Plugin `20260920-native-refraction` SHA256
`271B7FAAFF2AB7C2EA87793DEC8C04F4D556A2762B2B264A2C55F898DD603D88`.
Owned DLLs backed up and overwritten only. No deletes. Earlier PID44524 and
intermediate PID40980 exited gracefully with qqq. Initial runtime1091ECBD...
was superseded before inn testing to add required BINDLESS_ENABLED() metadata.

Read-only Ghidra audit completed using existing AE1.7 project (IDA had no open
database). New RunReadOnlyAudit.py opens getReadOnlyDomainObject and does not
reanalyze/edit/save it. Native functions confirm diffuse-slot source, active UV
index, material power and property+0x104 envmapLODFade as EyePos.w. Property bit16
selects angular falloff and bit13 selects clamp. BOTH vertex normal and texture
sample are clamped in that variant. See `remix-refraction.md` for source RVAs,
artifacts, equations, transport and explicit limitations.

Implementation:
- Refraction lighting meshes use animated NativeEffectStorage flag512. UV0..3,
  power8, propertyfade13, flags32(vertexalpha)/1024(falloff)/2048(clamp).
- The original diffuse SRV and native sampler addressing are imported; UVs
  stay unbaked and update every frame. Refraction is excluded from static probe.
- Meshes remain in unordered TLAS; ordinary light transport gives zero coverage.
  New native_refraction_mask.comp.slang ray-query pass evaluates native varyings,
  then native_refraction_composite.comp.slang distorts Remix's own scene color.
  Runs after upscaling/dust, before bloom/tonemap/UI; no native world draws added.
- Scene without refraction skips passes using flag derived in existing material
  packing loop. Mask image is reused. Debug option displays the mask.

Verification:
- Shader compilation and C++ runtime/plugin builds succeeded. First shader
  attempt lacked RayHitInfo declaration; corrected. First unit build lacked
  <string>; corrected. All build/test/setup sessions are complete; none pending.
- Dedicated `_Comp64UnitTest` test_native_refraction passed1/1 in0.04s. Shared
  scalar equations: distance, clamp, direction, edges, negative falloff. CPU test,
  NOT executed GPU image-parity test. Build/deploy/test skills used.
- Inspected baseline001523-before-native-refraction.png against matching
  001856-native-refraction-lit.png: tall opaque rainbow panels gone; hearth,
  flames and background visible. This alone would not prove refraction.
- Inspected001939-refraction-mask-a.png and001943-refraction-mask-b.png:
  all three masks present, foreground hearth clips lower bounds, texture pattern
  changes with identical camera pose. GPU mask animation is evidenced. Debug
  mask reset to false. Final normal capture002105-refraction-final-lit.png.
- CheckRefractionInputs -RequireImports -RequireImportedAnimation:3/3 submitted,
  native UV and imported UV animated; native/imported power0.1 and fade1 agree.
- CheckInteriorMembership:834loaded808submitted; zero exterior FXSplashLargeChurn,
  FXWaterfall or Tamriel matches; sixflames/twoMaleHeads positive controls.
- CheckEffectInputs Flames:6/6 native+imported UV animated, sources/palettes resident.

Limitations / next useful checks: nearest refraction layer is used; native overlap
draw order, target quantization, low-resolution/jittered depth edges, PSR/viewmodel
occlusion, skinned and temporary refraction remain unqualified. Current depth is
primary linear viewZ sampled at normalized screen UV (not jitter-corrected).
No native screenshot parity or performance qualification this turn. Other normal,
temporal, water, first-person and navigation requirements still apply. A bright
small green/yellow object behind the hearth is visible in lit capture and should
be identified rather than assumed to be correct or deleted speculatively.

## Continuation 23:52 — rainbow sheets are missing native refraction, not flame palette

Previous goal turn made verified progress (interior isolation). This continuation
identified the next concrete visual defect and deployed diagnostics; it did NOT
implement refraction or qualify the full goal.

PID44524 launched 23:48:19 and is alive in Sleeping Giant Inn. Setup62937,
shader5037, runtime9363, plugin91634 and capture/check96362 all completed.
First plugin3384 failed because BSTextureSet::GetTexturePath is non-const;
corrected the diagnostic getter and rebuilt successfully. No pending builds.

Deployed runtime `20260919-effect-gpu-probe` D3D11 SHA256
`BA540AEBFFA8191D92D5489F6B4C0E321C220CBCB603116229DA4C0F5529124B`;
DXGI unchanged. Plugin `20260919-refraction-inspector` SHA256
`BACD690BF4718D67650AF73D976F1C36ABC0E7AE1596BB6B742CC8CC454DFA85`.
Backups and symbols under those labels. PID51776 quit gracefully; only owned
DLLs overwritten. Build/deploy skills used; shader build precedes C++ embed.

Inspected captures `234332-fire-probe-before.png` (PID51776) and
`234941-fire-gpu-probe.png` (PID44524). Both show small orange flames beneath
three tall rainbow sheets. Camera (-210,-200,121); Capture.ps1 requested
pitch0.28 yaw-0.747 and reports actual pitch-0.207930624 yaw0.727235555.
Console closed, freecam on, debug view0, no held input. GPU print disabled again,
requireCtrl restored true and useMousePosition restored true.

New read-only `refraction` inspector block and CheckRefractionInputs.ps1 prove:
- Three `Plane03:0` meshes under FireplaceWood01Burning, shader flags
  0000803182418309 (kRefraction bit15), currently submitted.
- Diffuse AND normal paths are `textures\effects\VaporTileNormal_n.dds`.
- Each has refractionPower0.1, and BOTH native UV slots change.
- Current import path unconditionally creates opaque materials for these
  BSLightingShaderProperty meshes. No native refraction special-case exists.
  Thus the visible rainbow sheets are distortion data rendered as diffuse.
  Do NOT fix by deleting/skipping these meshes: that loses authored heat haze.

Relevant native shader equations, read this turn:
- Utility.hlsl RENDER_NORMAL VS around190: normal transformed to view space,
  vertex alpha passed; authored UV transform; distance denominator
  max(1,0.0013333333*clipZ+0.8); optional angular falloff; EyePos.w factor.
- Utility.hlsl around706: sample XY*2-1, optional clamp +-0.1; normal target XY
  = ((0.9*sampleXY + interpolatedViewNormalXY)/distanceDenominator)*0.5+0.5;
  targetZ = vertexAlpha*RefractionPower*EyePos.w*falloff, targetA=1.
- ISRefraction.hlsl: screen UV displacement (-1,1)*0.1*targetZ*(targetXY-0.5),
  authored edge adjustment, refracted-mask sample and colour lerp.
  Implement using Remix's rendered scene, not restored vanilla world draws.
  Audit native Utility SetupMaterial/Geometry for active UV slot, EyePos.w and
  refraction-power source before claiming exact transport. Ghidra tools were
  not found in current ALL_TOOLS search; prior local service/scripts may exist.

Runtime diagnostics added:
- D3D11CoreCreateDevice now initializes RtxFileSys and Logger::initRtxLog once,
  outside DllMain, matching D3D9's filesystem/log setup. Confirmed live log at
  `.research/testlogs/remix-dxvk.log`, honoring DXVK_LOG_PATH.
- rtx.debugView.gpuPrint.requireCtrl defaults true; false enables API-driven
  selected-pixel probes. Native-effect primary shader writes phase frame%4:
  0=(effect flags,palette index,material flags,intensity), 1=source RGBA,
  2=resolved RGBA, 3=UV offset/scale. Multiple hits can overwrite the same
  selected-pixel record; adjacent phases are NOT guaranteed the same surface.
- At internal pixel(550,347), samples include flags312, palette851,
  material32768, intensity1.5, changing coloured source and resolved alpha.
  At(550,300), flags304, palette21, intensity1 (smoke). This proves shader
  execution/live GPU input for those hits, NOT all palette or animation parity.
- Preserved log `.research/testlogs/20260919-refraction-inputs-and-flame-gpu.log`.
- `.research/effect-layout-probe.slang` compiled against actual shader structs
  to SPIR-V assembly. Inspected bit extraction preserves byte64 UV and
  byte110 flags; no simple reinterpret truncation found. This is compiler
  inspection, not executed GPU-layout unit testing. No unit suite run this turn.

Next implementation should address native animated refraction faithfully.
Full normal parity, temporal stability, water, first-person, traversal and
other goal requirements remain unproven/incomplete. Goal stays active.

## Continuation 23:31 — full model basis skinning and interior scene isolation

User reports exterior clouds/LOD visible inside. PID7996 while player was in
Sleeping Giant Inn had 5,988 retained / 4,474 submitted geometries, 1,870
`Cloud` matches, 52 `FXSplashLargeChurn` matches with submitted outdoor effects.
Freecam had moved to (21617.0762,-45140.4336,102.2094), pitch0.0181164,
yaw-0.4412174. Inspected `.research/captures/232942-before-interior-isolation.png`
showing exterior buildings, mountain and clouds despite the player's inn cell.

Plugin now avoids global WorldRoot traversal indoors; active-cell geometry and
references are gathered, plus the native temporary-node manager and player roots.
This also excludes the sky cell that TES::ForEachCell/Reference explicitly
enumerates even while an interior is active. Cell changes force immediate
rediscovery rather than waiting for the discovery cadence. Sky submission also
checks the active cell's explicit show-sky/sky-lighting flags. No global
AppCulled filter or blanket name-based LOD deletion was added.

Runtime now transforms all nine basis floats in the single skinning dispatch,
normalising each blended column at vertices and each interpolated column at
ray hits, matching native Lighting.hlsl's two normalisation stages. MSN meshes
always use GPU skinning. Shared CPU implementation supports nine output floats;
ordinary RGB/octahedral output writes retain their own sizes. SkinningArgs has
explicit scalar padding and compile-time offset/size checks in the unit test.

Dedicated `_Comp64UnitTest` configured using build-remix/run-unit-tests skills.
New `test_skinning_basis` passed (0.03s): indexed/unindexed, 0/0.25/1 bone
weights, rotated/translated bones, both vertex strides/offsets, guard values,
ordinary RGB/octahedral regressions. Uses the actual CPU/GPU shared skinning
function. Initial missing Logger instance link error fixed. This is CPU shared
math validation plus shader compilation, not GPU readback/native-image parity.
Shader49829, runtime84795, plugin88643, final layout/build/test42943 completed.
Test setup94475 and tests8484/41815 completed (8484 failed before logger fix).

Deployed runtime `20260919-model-normal-basis`:
D3D11 `32FCEB77F9E6E84503B5FACECCB73CC371B936E959A7FBA99708BE409EA2F42F`,
DXGI unchanged. Plugin `20260919-interior-membership`:
`292013262A6FEDA28CD5C328875066D04EC23BF33FD3D982CD58285DE211EDC1`.
Backups/symbols under those labels. PID7996 quit gracefully. PID51776 launched
23:30:26; setup handle76168 completed. No builds or setup remain pending.

Live validation at 23:32–23:37, same PID51776:
- Inn has 834 loaded / 808 submitted geometries initially (previously 5,988 /
  4,474). CheckInteriorMembership passed: zero FXSplashLargeChurn, FXWaterfall,
  and Tamriel matches; six flame and two MaleHead positive controls submitted.
- CheckEffectInputs passed six native and six imported animated flame UVs.
  This is update evidence, not proof of correct rendered fire.
- Inspected 233200-after-interior-isolation-outside.png at the former leaked
  exterior position: outdoor buildings, mountains and clouds are gone; only
  tiny distant inn geometry and HUD remain. Camera orientation differs from
  the before capture, so this is NOT a pixel-identical A/B.
- Inspected 233217-interior-isolation-positive.png and
  233219-model-basis-normal-view.png: inn geometry/NPCs still render. Normal
  detail is present, but native normal parity is not established. Malformed
  multicolour fire sheets and excessive lightglows remain visible.
- Inspected 233553-interior-isolation-riverwood-reload.png: exterior buildings,
  vegetation, mountains and sky return outside. This is a presence check, not
  full exterior image qualification.
- CheckCellRoundTrip -Rounds 1 passed third-person console teleports to inn
  (frame39566–39830, 809 submitted) and Riverwood (39860–40039, 4,468 submitted).
  Console was opened before all cell commands. This does NOT test doors,
  locomotion, fast travel, flicker, judder or continuous animation.
- Returned to Sleeping Giant Inn, closed console, restored debug view zero.
  Membership check passed again after the round trip (835 loaded / 809
  submitted; same zero exterior and six flame/two head controls). Freecam off;
  idle vanity camera observed and third-person requested again. Normal lit
  Remix active. No held input.

The build-remix/run-unit-tests skills dictated separate shader/runtime builds
and the dedicated unit-test build directory. Only the focused skinning basis
unit test ran, not the full suite. Interior fix remains scoped to the tested
inn; authored sky-lit interiors and temporary-node membership need broader
qualification. Overall goal remains active and incomplete.

## Latest verified state at 23:20

PID7996 is alive in Sleeping Giant Inn with runtime `20260919-native-rgb-normals`
and plugin `20260919-effect-live-updates` (hashes below). Setup65778 completed.
Shader89244/C++11292 completed. No build or setup handles remain pending.
Deployed DLL hash and live module path match the intended Remix runtime.
Console closed, HUD only, no modal, no held input. Debug view reset to zero;
normal lit Remix is active, native world suppressed, freecam remains parked at
(-54.6833801,-147.6012268,121), pitch0 yaw-2.04781485.

Inspected `.research/captures/231911-rgb-normals-after-parked.png` and
`231925-rgb-normals-lit.png`. Static framing matches the parked before capture;
NPC moved, so character pixels are not a direct A/B. Ordinary wood/stone normal
detail changes with RGB decoding. This proves a visible candidate change, NOT
agreement with native raster normals. Lit image still has malformed multicolour
fire sheets on the right. Input regression check again gives 6/6 native AND
imported flame UVs changing. Full animation/judder/lighting qualification remains.
No dedicated unit suite was run; its required `_Comp64UnitTest` directory is absent.

Next concrete defect to implement is the incomplete three-axis model-normal
skinning described below. Do not say characters, foliage, terrain, or all normals
are fixed. User's latest normal complaint remains unresolved in full.

## Continuation at 23:13 — effect updates verified; RGB normal candidate building

Update 23:17: shader89244 and C++11292 builds passed and completed (existing
dependency PDB warnings). Runtime `20260919-native-rgb-normals` deployed,
D3D11 SHA256 `E200DFAE6E0EF65BAFFA9FDDB54590BC15A32EC623ED9D4DE5E1E6D871A51692`;
DXGI unchanged. Backups/symbols under matching labels. PID25364 quit gracefully.
PID7996 launched 23:17:37, setup handle65778 pending at this writing.

Second concrete normal defect found, NOT yet changed: `ModelNormalVertex`
stores nine floats for its basis, but `RtxGeometryUtils::dispatchSkinning`
and `rtx/pass/skinning.h` still transform/write only THREE floats. Y and Z
basis columns remain in bind pose. No model-space special-case dispatch exists
in this fork. Fix all three columns: native Lighting.hlsl normalises each
column in the vertex shader (lines237–239), then normalises interpolated TBN
columns again in the pixel shader (lines925–930). The small-mesh CPU branch also has only
`float dstNormal[3]`, so it must be updated or explicitly route MSN to GPU.
The API's packed interleaved vertex layout is directly copied into the cache;
do not accidentally shrink it to a generic 3-float normal in a re-interleave.
Ordinary meshes also omit authored tangent/bitangent and use `genTangSpace`;
RGB decoding alone does not certify native normal parity.

Logging diagnosis: `RtxFileSys::init` and `Logger::initRtxLog` are called only
from d3d9_main.cpp in this fork, not D3D11. Its logs path is therefore not
initialised for our entry point. The path specification DOES honor
DXVK_LOG_PATH when initialised. Earlier suspicion that it ignores that variable
was wrong. No logging change is part of this RGB-normal candidate.

Current process PID25364, launched 23:05:37, is in Sleeping Giant Inn. Plugin
`20260919-effect-live-updates` SHA256
`7C9EE21D23CF5F1B2450D89F85E27C509147D6D8D1163A0B373D037140AD233F`.
Runtime is still `20260919-ordered-skinning-present` below.

Found an actual effect animation freeze: `RemixScene::Capture`'s static-probe
early-out precedes effect parameter capture/update. Stationary effect geometry
was eligible for that probe, despite UV animation. Effects are now excluded.
Before fix, all six `Flames:` triangle meshes had changing native UVs but ZERO
changing imported UVs. After fix `CheckEffectInputs -Filters 'Flames:'
-RequireImports -RequireImportedAnimation` reports 6/6 native AND imported UVs
changing. This is input/update proof only, not a particle or rendered-motion pass.
Malformed fireplace colours and broader intermittent motion remain unresolved.

Latest user report: normals still do not work. Found direct D3D11 SRV normal
imports enter the ordinary opaque path with no asset preprocessing, but that
path decodes XY as octahedral. Native `Lighting.hlsl::TransformNormal` instead
decodes RGB * 2 - 1. Added explicit native RGB normal flag (bit8, within uint16),
included in both material hashes, propagated through SceneManager and decoded
separately. Model-space and native landscape branches retain their own decoding.
Candidate is NOT deployed yet. Shader build handle89244 active at this writing.
Then run a separate C++ build to embed regenerated shader headers.
Parked before capture `.research/captures/231241-rgb-normals-before-parked.png`
was inspected. Debug view16 (shading normals), freecam at
(-54.6833801,-147.6012268,121), pitch0 yaw-2.04781485. Preserve this pose for after.
No held input. Setup handle41050 completed; do not poll it.

Expanded goal also explicitly includes RTX skin/hair/thin foliage subsurface,
animated refraction, TREE_ANIM, hardware skinning, camera-facing billboards,
lightglows, matching directional light, matching animated water reflection and
refraction, first-person model correctness, third-person traversal/doors/fast
travel, native depth/albedo parity, LOD and content/presentation stability, and
performance. None is newly certified by the checks above. Goal stays active.

## Continuation at 22:57 — presentation and skinning ordering defects

Latest user evidence: even the native mouse cursor jitters; interior lights
switch on/off; the player alternates between animation and T-pose; particles
advance a few frames then intermittently stop. These reports override the earlier
parked-camera tests. Do not call the problem fixed from FPS or still images.

Two concrete runtime ordering defects were found:

1. D3D11 permitted two outstanding presents (`DxvkLoader` calls
   `dxvkSetSyncPresent(0)`), but `vk::Presenter` owns only one pending acquisition.
   Its submission-thread `presentImage` advances `m_frameIndex`, selects the next
   semaphore pair, and pre-acquires `m_imageIndex`. A second host-thread acquire
   before the preceding present completes can reuse that shared image/semaphore
   state. Waiting for the status from TWO frames ago is insufficient.
   `D3D11SwapChain::SynchronizePresent` now always waits on slot zero; the exported
   depth setter remains ABI-compatible but logs an effective depth of one.
   This orders presenter CPU state, not a device-idle wait on every frame.
2. `csRemixRender` injected RTX without flushing geometry work. Skinning records
   on `RtxGeometryUtils::m_skinningContext`; `RtxContext::flushCommandList` submits
   the main upload list THEN the skinning list. Without a pre-injection flush,
   BLAS build/tracing can precede queued skinning, with implicit flush timing
   determining whether the current pose is available. D3D9's
   `triggerInjectRTX` explicitly flushes before injecting. The D3D11 API now
   calls `rtx->flushCommandList()` before `rtx->injectRTX()` on the CS thread.
   CPU/GPU idle waits are not added here either.

Current deployed runtime: `20260919-ordered-skinning-present`, D3D11 SHA256
`E5A8E0AF30AD94499F3FB830C45A3F26B355A0BD78EA75117F40197D75052629`.
DXGI unchanged `066D721569675B2172754F23A40DA3044E873B8B167A628A3534DA4780EF6788`.
DLL/PDB archives and overwritten-DLL backups are under the matching
`.research/deployed-symbols` and `.research/deployment-backups` labels.
CS remains `20260919-absolute-world-view`, hash recorded below. No plugin,
native-render-suppression, quality, saved-settings, or shader changes were made
for the two ordering fixes. PID23792 started 22:58:45; module enumeration and
deployed-file hash confirm the intended runtime path. Riverwood setup completed
with Survival declined and native world bypass true.

At 23:01 PID23792 is alive in the Sleeping Giant Inn, normal lit Remix, console
closed, HUD Menu only, no modal and no held input. Last sampled frame 12926.
The console-paused inn entry succeeded. Twelve spaced captures
`.research/flicker/23000*-ordered-skinning-inn-*.png` completed; 01/04/08/12 were
visually inspected. They show the character in posed states, candle sprites
changing, and working HUD; malformed fireplace sheets remain. The camera/player
framing differs from the previous candidate, so brightness is not an A/B result.
Global luminance range 11.58–12.31, max adjacent step 0.28 (HUD notifications
included). This is not a consecutive-frame proof that intermittent T-pose,
particle freezes, cursor judder, or lighting flicker are eliminated.

Separate screenshot-free 20-second PresentMon trace:
`.research/testlogs/20260919-ordered-skinning-inn.csv`, 2930 rows, one swapchain,
Hardware: Independent Flip. Display interval median 6.8492 ms, p99 12.991 ms,
max 16.6558 ms; present interval median 6.762 ms, p99 12.1586 ms, max 16.9592 ms.
No long present stall occurred in this short trace; scene content continuity
is a separate unresolved qualification. Plugin log snapshot:
`.research/testlogs/20260919-ordered-skinning-check.log`.
Build handles83426/33914, setup65528/34658, trace21086/95317 all completed.
No background test is pending. Do not poll these handles or repeat deployment.

Both isolated Release builds passed through Meson (existing missing dependency
PDB warnings only). Initial sandbox build could not read D:/packman-repo;
approved escalated build succeeded. Runtime diff whitespace check passed.
No dedicated `_Comp64UnitTest` suite was run. Both changes need continuous-motion
and scene-update qualification, not just compilation or source inspection.

### Presentation-only test (superseded by the skinning candidate)

Runtime `20260919-serialized-present`, D3D11 SHA256
`6F2D3F69A5D580DC5393E56662C42F549DDB30F32D150175FCE9BC88F82971AE`.
PID596 started 22:51:51, loaded Riverwood, then entered the inn with the console
explicitly open. Native world suppression and HUD remained active. It exited
gracefully via qqq for the next deployment; do not treat it as still running.

PresentMon `.research/testlogs/20260919-serialized-present-inn.csv` captured
2,819 rows in 20 seconds: one swapchain, Hardware: Independent Flip. A screenshot
burst overlapped the early trace; use only TimeInMs > 8000 for the quieter tail.
For 1,681 tail rows, displayed-frame interval median 6.879 ms, p99 13.803 ms,
max 17.634 ms. Present interval median 7.005 ms, p99 13.227 ms, max 16.354 ms.
This is no before/after causal comparison and cannot prove unique scene content.
Cast CSV time fields to double before filtering, not lexical string comparison.
The trace required execution outside the sandbox, but not a UAC/settings change.

Twelve snapshots `.research/flicker/225349-serialized-present-inn-*.png` had
global luminance 15.87–17.18/255, max adjacent step 0.46. Images 01/04/08/12 were
inspected; player poses differ and fire remains malformed bright multicolored
sheets. HUD screenshot notifications contaminate global luminance. These are
spaced snapshots, NOT consecutive presented frames or a temporal pass.
Preserved plugin log `.research/testlogs/20260919-present-only-final.log`.

### Prior material-update candidate and failed native-reference control

Runtime `20260919-native-material-updates`, D3D11 SHA256
`5E09EBF3D9919E107E731E171727DA31795857558CAC35C145467D7E5E5420B4`,
added a SceneManager external-material dirty flag so effect/water parameter
updates upload even without geometry changes. Simplified NEE material evaluation
now preserves native-effect flags/tint instead of clearing palette inputs.
Shader and C++ builds passed. These changes remain in the newer candidates.
They did NOT fix the malformed fire or the user-reported intermittent motion.

PID47684 (22:42:46) passed six parked third-person camera checks, max native
position difference 0.000184059 units. CheckCellRoundTrip aborted its exact-cell
precondition because spawn was Riverwood04, not Riverwood; no renderer failure
or round-trip pass should be inferred. Manual console-paused inn entry succeeded.
It exited gracefully for the presentation-only candidate. Pre-exit log:
`.research/testlogs/20260919-pre-present-serialization.log`.

Earlier PID8376 stalled after the one-way `cs.nativeReference=true` switch in
the inn. No valid native-reference image was obtained. Noninvasive CDB snapshot
`.research/testlogs/20260919-native-switch-hang-stacks.txt` showed main-thread
NVIDIA allocation work (NtGdiDdDDIDestroyAllocation2 / tryAllocDeviceMemory), not
a proven deadlock. After qqq failed to exit and exact process identity was
verified, it was force-closed; old crash dumps do not belong to this stall.

Other open findings (not changed): native-sRGB flag is defined at bit16 but
opaque flags are packed to uint16; fixing that blindly risks changing gamma
twice. Raw albedo debug32 runs before alpha rejection and is not resolved final
coverage. Motion debug21 showed zero vectors on stationary inn geometry and
nonzero on a moving NPC, so do not attribute all flicker to camera transforms.

## Continuation at 22:20 — stable-world camera candidate; flicker NOT qualified

The user reports objects jittering and lighting flickering on all objects. This
overrides any broad stability claims below. The active goal still includes
correct normals on **all** surfaces, correct characters, continuous movement,
door/interior/fast-travel stability, native-world suppression with working UI,
animated effects/volumetrics, raster albedo/depth parity in all scenarios, and
performance. None of the narrow tests below completes that goal.

Current deployed CS: `20260919-absolute-world-view`, Release, SHA256
`E8E05F8208DAE4547438E23097587A875B86C4D3E5BEF93F72F300A3428813CC`.
Exact DLL/PDB archived under `.research/deployed-symbols/20260919-absolute-world-view`.
Backup under the matching deployment-backups label contains the previous
`20260919-cpu-bypass-retest` DLL, SHA256
`FE7E13A94A3F2D40F30F8C5ED64D2B486B8B44CB9ED09FAAA1B86B472EAFFD23`.
Runtime is unchanged `20260919-blas-local-transform` (hashes below).
PID8376, started 22:14:57, is alive outside the Sleeping Giant Inn in Riverwood02,
third person, freecam off, normal lit Remix view, console closed. No held input
or background test remains. Builds 23297/59540 and test handles 29134/85974/5504
all completed. Do not restart them or infer they are still pending.

### Camera/temporal coordinates

The integration previously shifted every retained object and CPU camera history
by Skyrim's moving `posAdjust` origin. The runtime's full acceleration-structure
reuse path is gated by InstanceManager scene generation, but retained replay's
`RtInstance::rebaseBy` changes transforms/dirty flags without incrementing that
generation. Other dynamic objects may incidentally invalidate it; this is a
code-level invalidation gap, not a measured attribution of the reported jitter.
Rebasing CPU matrices also does not generally rebase persistent GPU lighting
data. NEE cache spatial hashing uses world positions (`nee_cache.h`).

`RemixCameraMath.h` now restores the absolute world-view translation:
`absoluteView[3][col] = relativeView[3][col] - dot(nativeOrigin, relativeView[0..2][col])`.
All instances, immediate grass, and point lights are submitted in the same
absolute coordinates; retained replay receives origin zero. This is not the
old invalid experiment of snapping an origin without compensating the view.
Projection jitter removal and the audited current-frame world-camera hook stay.
The renderer's own translated-world calculations remain available downstream.
No runtime code was modified in this continuation.

`tools/remix/CheckWorldViewMath.cpp` compiles/runs the actual shared helper with
MSVC: 540 rotated/origin/point comparisons passed, max error 0.0135177 units
through origins as large as (-200000,300000,7000). This is only coordinate
equivalence, not temporal or raster parity. Optimized plugin build passed.
Six first-person and six third-person in-game samples differed from native
camera position by at most 0.004368 units. Capture
`.research/captures/221556-absolute-world-third.png` was inspected: coherent
Riverwood/player/HUD, but very dark hair beneath the helmet remains.

### More discriminating capture tests; mixed result, not a jitter fix claim

Before restart, fixed-view PID36536 captures are
`.research/flicker/*-jitter-fixed-{lit,albedo}-*.png` (12 each). Lit global mean
118.76–119.15/255, largest adjacent step 0.32. Averages alone hide local flicker.
New `CompareTemporalFrames.ps1` uses HUD-free cliff/rock/building patches in the
documented fixed Riverwood pose. Lit mean-centered adjacent MAD was respectively
0.446 / 0.574 / 3.976, all best integer shifts (0,0). Raw-albedo debug captures
have expected unfiltered sampling jitter; do NOT call their 1–2 pixel shifts
proof that the final lit image jitters.

New `CheckCameraReturn.ps1` sweeps camera X +/-160 units and returns to the exact
same pose, without changing the player. Four cycles before/after survived:
`.research/captures/*-moving-origin-return-*.png` and
`*-absolute-origin-return-*.png`. Cliff MAD improved from ~2.164 to ~1.521;
foreground rock did not (~3.774 to ~3.898), building ~11.72 to ~10.07.
Lighting/weather/screenshot completion timing are not controlled enough for a
causal claim. These are return-to-pose samples, NOT consecutive presented-frame
motion measurements. Keep the candidate provisional and investigate remaining
judder/flicker with continuous-frame evidence. Baseline was also captured near
a full plugin build, so do not infer performance improvements from those runs.
Preserved baseline log `.research/testlogs/20260919-moving-origin-final.log`.

### CPU bypass and scene membership

The preceding CPU-bypass candidate passed six console-paused transitions in
PID36536: `.research/testlogs/20260919-cpu-bypass-paused-console-roundtrip.json`.
Restoring full native CPU world/shadow work was not required for that test;
conditional bypass remains. Native world phase fell from ~6.7 to ~1.5 ms in
those runs (not a controlled overall FPS comparison).

This deployed build also includes the previously built scene-membership work:
native TESObject::IsMarker predicate excludes authored editor objects; exact
exterior terrain roots are excluded indoors only when the engine marks them
hidden. Per-geometry AppCulled is still not used as a blanket filter.
Root globals were audited with Ghidra in `.research/scene-*-audit.json`:
trees +0x3203520, water +0x3203528, land +0x3203538, objects +0x3203540.
TES::objRoot named ObjectLODRoot is NOT one of those disposable exterior roots.

At 22:17, inside the inn, `remixScene` filters BSDistantTreeShaderProperty,
MarkerCOCHeading, marker_north, MarkerXHeading all matched **zero**. The baseline
had 397 exterior tree groups and the editor markers. Inn submissions were ~4470
on coc versus ~5127 before. HOWEVER inspected
`.research/captures/221724-absolute-world-inn.png` still has the malformed bright
pink effect at left. Marker exclusion did not fix that effect; identify its
actual geometry/material. Interior appearance is not correct yet.

### Transition and input coverage

New PID8376 passed three console-paused inn/Riverwood round trips, third person:
end frames 4330/4540, 4816/5037, 5316/5530. No crash. This is still only coc
coverage, not proof of general transition safety.

Additionally, native Papyrus ObjectReference.Activate(player,false) on the
observed exit door 0x13419 returned true and loaded Riverwood02. The observed
exterior door 0x13424 returned true and loaded the inn; activating 0x13419 again
returned to Riverwood02. These three actual door transitions are stronger than
coc, but were script-activated, not a crosshair/keyboard interaction test.
The final exit capture `.research/captures/221940-absolute-world-walk.png` shows
the player/HUD outside, with dark terrain patches still visible. Not a visual pass.

Input.GetMappedKey("Forward",0)=17 and Activate=18. HoldKey(17) was always
paired with ReleaseKey in finally; final IsKeyPressed returned false. A 600ms
attempt produced no immediate position change, with ~6.5 units of movement
visible later. Do not count it as a sustained-walking/pacing pass. Player
SetAngle in third person was immediately overwritten by camera behavior; it
did not reliably aim the exit door. Idle vanity can also override setPov.
Fast travel, continuous camera/lighting stability, all-surface normals,
character correctness, and raster albedo/depth parity remain outstanding.

## Continuation at 21:50 — transition control changes the diagnosis

Current deployed CS is `20260919-accumulator-lifecycle`, SHA256
`7A7D5A81DAB2A03EE68ED4406F9F0492836FC0ADE57C52E174103C13597B4F49`.
Runtime remains `20260919-blas-local-transform` below. This CS candidate restores
both native `Main_RenderShadowMaps` and `Main_RenderWorld` CPU calls while
retaining independent draw/compute suppression. It is an experiment, not a
proven necessary fix; suppressed-world CPU cost increased to about 6.47 ms.

The former live test handle 3323 completed with a crash: PID45460 entered
Riverwood, then failed on its first unpaused `coc RiverwoodSleepingGiantInn`.
Preserved `.research/testlogs/20260919-accumulator-lifecycle-crash.dmp/.log`.
Fault is now `SkyrimSE+0x154b9cb`: `BSLight+0x48` contains a null NiLight, then
native point-light constant setup reads `[rdi+0x134]`. Stack goes through native
world rendering, not the earlier shadow SetupTechnique failure. Ghidra export
`.research/transition-null-light-audit.json` matches this dispatch.

**Native control reproduced the exact same null-light failure.** PID45484 was
configured with `-Mode Triangle` (no imported scene, no native suppression), then
one-way `cs.nativeReference=true` before the round trip. Zero draw/compute
suppression was logged. Evidence `20260919-native-reference-crash.dmp/.log`.
Therefore this particular crash does not require retained scene import or native
GPU suppression. Do not attribute it to either without further evidence.

Installed DevBench 1.17 PDB/disassembly confirms ConsoleHandler queues
`RE::Console::ExecuteCommand` via `SKSE::TaskInterface::AddTask`. It does not
open the console for the command. Issuing coc after explicitly opening the
actual `Console` menu, then closing it once the destination loads, **passed
three native-control round trips** (six transitions) in PID43396, all third
person with advancing frames. Results
`.research/testlogs/20260919-native-paused-console-roundtrip.json`; final native
log archived alongside it. No game saves or persistent settings changed.

`tools/remix/CheckCellRoundTrip.ps1` now uses this console-open sequence, verifies
the console closes and world frames advance, and offers `-NativeReference` as
an explicitly labelled diagnostic control (requires process restart to return
to Remix). This is only console-load stability, NOT door navigation, fast
travel, locomotion, visual correctness, or pacing validation.

Remix repeat **passed all six transitions** in PID28008 (started 21:50:44).
Exec handle8414 completed successfully. Results:
`.research/testlogs/20260919-lifecycle-paused-console-roundtrip.json`.
Six parked third-person camera checks after returning matched within 0.000214
game units; about 8,347 submitted instances after streaming settled. Result
`20260919-lifecycle-post-transition-camera.json`. Native logs report 6,980 draw
batches / 78 compute submissions suppressed. Capture
`.research/captures/215215-lifecycle-post-transition-third.png` was visually
inspected: coherent Riverwood/third-person body and native compass, very bright
lighting and dark hair beneath the helmet. Not an albedo/depth or character
correctness pass. Process is left running in Riverwood, freecam off, console
closed, Remix on, for screenshots. No held input or background test remains.

Next isolate whether restoring the expensive CPU world/shadow calls is necessary
with the corrected console sequence before keeping that performance regression.
Then test real door navigation/fast travel and investigate interior scene
membership (around 5,126 instances indoors still includes exterior geometry).
The null-light unpaused-console failure is reproduced in the native control;
the earlier freed-pass/shadow failure is not yet proven to have the same cause.
Do not mark either broad transition stability or the goal complete.

## Continuation at 21:33 — supersedes deployment and floating-object claims below

Current CS is still `20260919-api-context-lock` (SHA unchanged below). Runtime
is now `20260919-blas-local-transform`, built from `.research/dxvk-remix`:

- D3D11 SHA256 `0C8A5F8D466D96B93C5436F792C82E582997EE3128691712B80C23A43F26FBB8`.
- DXGI SHA256 `066D721569675B2172754F23A40DA3044E873B8B167A628A3534DA4780EF6788`.
- Runtime backup and exact DLL/PDB archive use the new deployment label.
- Both `meson compile` invocations succeeded; no unit-suite run this continuation.

Final fresh default-settings check at 21:34: PID42880 (started 21:33:08) is
running Riverwood with Remix/native suppression and HUD, left in fixed freecam.
Six third-person camera-source samples matched exactly before freecam, with
8,377 instances submitted. Captures `213441-blas-local-fresh-third.png` and
`213444-blas-local-fresh-default.png` were visually inspected. The neck/hair
under the helmet remains very dark; not a character-correctness pass. Another
20 fresh captures completed without crash: mean luminance 118.78, range
118.51–119.12, largest adjacent step 0.59. Results
`.research/testlogs/20260919-blas-local-fresh-{camera,default-capture}.json`.
No BLAS routing or graphics-quality overrides were applied to this fresh run.

### Capture race regression now exercised

PID7988 survived three bursts of 20 genuinely new screenshots (60 total), plus
individual captures. Logs/results: `.research/testlogs/20260919-context-lock-*`.
The game had entered idle vanity mode; `setPov` was immediately overridden, so
these bursts used fixed freecam `(17224.77,-47204.45,30)`, pitch/yaw zero.
This is capture-race regression evidence, not locomotion validation.

### Floating geometry: actual additional defect and verified correction

The first burst visibly contradicted the old floating-object claim. Frames
`212332-api-context-lock-fixed-a-12/13.png` under `.research/flicker` show a
cluster appearing in the sky, plus a large displaced stump in the foreground.
Turning `rtx.enablePreservePath` off did not remove the stray pieces. Restored
it to true. Setting `rtx.maxPrimsInMergedBLAS=0` catastrophically displaced
large cliff meshes: `212616-unmerged-fixed.png` under `.research/captures`.

Cause in `rtx_accel_manager.cpp`: cached `BlasEntry::buildGeometries` retains
the merged build's `triangles.transformData.deviceAddress`. On promotion to a
standalone dynamic BLAS, the cached description may skip regeneration, so the
build applies an old per-frame transform slot (possibly now another object),
then TLAS applies the object's transform again. Dynamic build setup now sets
this address to zero: standalone geometry is object-local. The normal merged
path still supplies its own transform; no feature or optimization is disabled.

New PID45636, same fixed view: `212921-blas-local-default.png` puts the fallen
tree back across the stream with no giant stump/floating pieces visible.
Repeating promotion gives `212924-blas-local-promoted.png`, coherent instead
of the prior catastrophic cliff displacement. A further 20-frame burst passed
with luminance 118.74–119.28 (largest adjacent step 0.54), preserved under
`.research/flicker/*-blas-local-promoted-*` and result
`.research/testlogs/20260919-blas-local-promoted-capture.json`. Not proof of
all motion/flicker scenarios. The promotion test changes routing persistently
within a scene; its timing is NOT comparable to the default. Setting restored
to 50000, then fresh process launched to return to default routing.

### Interior round trip is still a failure

PID45636 successfully `coc RiverwoodSleepingGiantInn` from Riverwood in third
person. Six parked-camera samples matched exactly; 8 scene lights submitted,
native draw/compute suppression logged. `213132-blas-local-inn-third.png`
shows the room/HUD/player, but also a bright malformed pink effect at left;
do not call interior appearance correct. Thousands of exterior/LOD geometries
remain gathered indoors; investigate scene membership as well.

`coc riverwood` returning from the inn crashed at 21:32:07. Evidence:
`.research/testlogs/20260919-blas-local-return-crash.dmp/.log`. CDB confirms
native `SkyrimSE+0x156016a` indirect shader SetupTechnique dispatch, via shadow
map rendering. RCX/RDI=0x135349a1030, RSI=0x135349a2980, RDX=0xc046.
RSI's first word points to RCX; RCX's first word points to 0x135349a1080.
Both latter blocks look like linked render-pass records, not shader vtables:
techniques 0xc046/0xc066 at +0x18, `0xfefedead` at +0x40. Investigate stale
render-pass/pool lifetime rather than assuming a freed shader singleton.
This is evidence/a lead, not a confirmed cause or fix.

Door activation through console did not change cells and `prid` did not yield
a selected reference; do not count those attempts as door-navigation tests.
Console help capture also returned no markers. `coc` works. Actual locomotion,
door navigation, fast travel, motion shimmer, full character rendering, and
raster albedo/depth parity remain unverified/unresolved. Goal stays active.

Portable `tools/remix/runtime.patch` / `PrepareRuntime.ps1` still target the
older Titanfall base. They have NOT been regenerated for the active runtime;
do not apply them to the current source or claim reproducible fresh setup yet.

## Continuation at 21:18 — supersedes the deployment state below

Current deployed label: `20260919-api-context-lock` (Release CS + runtime).
Camera fix tested in both player POVs; context-lock stress retest is pending.
The runtime being built is **`.research/dxvk-remix`**, not the older
`.research/Titanfall-2-Remix`. Both repositories still exist.

- CS SHA256 `390C918C83958FAADFAE7B65760669FA97D7F604EB62FFED4DC61A2E27E41C4E`.
- D3D11 SHA256 `082661A3A56C4FB1BFA159771F3859F0A9C7EF0445DC82C28707400BB8AED7E3`.
- DXGI SHA256 `066D721569675B2172754F23A40DA3044E873B8B167A628A3534DA4780EF6788`.
- Previous binaries backed up under `.research/deployment-backups/20260919-api-context-lock`;
  exact DLL/PDB pairs under `.research/deployed-symbols/20260919-api-context-lock`.

### World camera: a definite source defect, now corrected

The `jitter-zeroed` build launched and produced a coherent third-person view,
but first-person after fresh `coc riverwood` rendered from `(0,0,120.5)` while
DevBench's player camera was `(17224.771,-47204.449,-7.733)`. The pass-state
camera at UI time was the **local first-person camera**, not the world camera.
This is independent of the projection jitter. A near-plane heuristic and a
60-frame hold cannot reliably identify the camera and can themselves stall it.

Read-only Ghidra audits `.research/camera-world-call-audit.json` and
`camera-state-native-audit.json` confirm unconditional call RVA `0x656fd4` ->
`BSGraphics::State::SetCameraData`, RVA `0x101c400`. RSI is WorldRootCamera and
R8D=1. An independent, byte/target-verified native call hook snapshots the
resulting ViewData AND posAdjust before first-person/image-space passes.
`ReadWorldCamera` rejects a snapshot not belonging to the current graphics
frame. The near-plane hold is removed. Projection `_31/_32` zeroing is retained;
**motion shimmer is not yet qualified**.

The first test exposed a bridge bootstrap bug: one missing world frame assigned
`ready=false`, permanently disabling Remix and its controls. Scene-frame
readiness is now separate from initialized-API readiness. Snapshot capture does
not depend on suppression being enabled. Loading clears camera-jump history.

`CheckWorldCamera.ps1` verifies parked camera position only. Six samples each
on PID24264: first-person error 0, third-person max 0.000458 game units; scene
frame advances and >8,300 instances submitted. Screenshots
`210845-world-camera-recovery-first-person.png` and
`210906-world-camera-recovery-third-person.png` under `.research/captures`
show the correct Riverwood view and third-person model/HUD. Suppression logs
report native draw/compute batches skipped. Not a full character/lighting pass.

### Two separately identified test crashes

1. `20260919-world-camera-recovery.dmp` and matching `*-crash.log` in
   `.research/testlogs`: invalid `getgs fAutoVanityModeDelay` diagnostic hit
   **GameFunc::handler::GetGameSetting**, RVA `0x34ed75`. Ghidra and CDB both show
   a literal read from address `0x8` on its setting-not-found error path. Do not
   issue that query again. This is not a movement or render-pass crash.
2. `20260919-world-camera-capture-crash.dmp/.log`: after five screenshots,
   `DxvkCsChunk::push` inlined into `setRetainedInstance`, D3D11+`0xe25f8`,
   dereferenced a null command chunk. Screenshot's worker Map can flush/move
   that chunk while private Remix API EmitCs bypasses the D3D11 context lock.
   The API accessor now locks EmitCs/RestoreState; API device registration
   enables context protection before worker traffic. Screenshot and Flowmap
   guards restore the prior protection flag rather than forcing it off.
   Both optimized builds succeeded. Re-run repeated capture before claiming fix.

The synthetic C key did not produce observed player movement; do not count it
as locomotion validation. Interior/fast-travel tests remain outstanding, as do
the previously documented transition crash and full albedo/depth parity.

### Corrected test tooling

DevBench 1.17 **does** dismiss Survival via `describe`, then `accept index=1`
(zero-based `No`). ConfigureRunningTest now uses this, checks the prompt text,
and waits for five quiet polls without mouse input. RunRiverwoodTest's prior
one-based index bug is corrected. MeasureFlicker requires the requested number
of genuinely new screenshots, a live advancing process and a stationary camera;
it no longer silently includes old captures if the worker falls behind.

Runtime builds in this continuation use the repo-prescribed `meson compile -C
_Comp64Release -j 8`, followed by a second invocation to verify no generated
shader consumers are stale. CS uses `cmake --build build/ALL --config Release
--target CommunityShaders --parallel 8`, not /Od Dev-Fast for measurement.

Branch `codex/remix-vulkan`. Everything below is from this session unless it
says otherwise. Measurements are quoted with the conditions that produced them,
because several of them are not comparable with numbers recorded earlier in
`remix-performance.md`.

## Read this first: the deployed build is untested

| Component | Label | State |
| --- | --- | --- |
| `CommunityShaders.dll` | `20260919-jitter-zeroed` | built and deployed, **never launched** |
| `dxvk_d3d11.dll` | `20260919-motion-theory-recorded` | deployed and verified |

The plugin currently installed zeroes the two TAA jitter terms in the camera
projection handed to Remix (`RemixScene.cpp`, the `SetupCamera` block). It
compiled and deployed, but the launch that would have verified it was stopped
before it ran. **Launch it and look at the image before trusting it.**

The immediately preceding attempt at the same fix — substituting
`frameBufferCached.GetCameraProjUnjittered()` for the whole matrix — produced a
badly smeared image with geometry floating in the sky
(`.research/captures/204817-unjittered-clean.png`). That approach is wrong and
is documented as such in the code comment; do not reinstate it. If the current
build is also bad, roll back to
`.research/deployment-backups/20260919-jitter-zeroed` (which holds the *previous*
plugin, i.e. the last known-good one) and treat the jitter work as unfinished.

## Fixed, with evidence

**Skyrim's lights reach Remix.** Previously the only light in the scene was a
hardcoded distant sun; every torch, hearth and forge was emissive geometry that
illuminated nothing. `RemixScene::CaptureSceneLights` walks
`ShadowSceneNode::activeLights`/`activeShadowLights` from inside the world frame
and submits sphere lights with `volumetricRadianceScale = 1`, so they feed the
volumetric froxel grid. 8 lights in the Sleeping Giant Inn, the forge light
outside. Details, including the `lodDimmer == 0` trap that deletes every
interior light, in `remix-effects.md`.

**Objects floating in the air.** Log-end discs hovering detached from any
woodpile, plus a cross-shaped structure in the sky
(`.research/captures/200249-forge-anim-a.png`), now render as a correct woodpile
on the ground (`200821-floating-after-fix.png`). Cause was a torn read of an
instanced-placement buffer being *latched*: per-row validation cannot see a tear
that lands between rows, because both halves are internally consistent. The read
now retries up to three times within the frame and a group that still will not
settle is left out for a frame rather than shown wrong.

**Random objects showing up for one frame.** 60-second camera orbit: **1**
object churning (`PCloudForgeSparks`, an emitter genuinely between bursts) and
**0 objects placed somewhere they had not been**, down from 5 objects / 649
events earlier in the session.

**LOD.** `.research/captures/200213-remix-horizon-lod.png` — full tree foliage to
the far treeline, detailed distant terrain and cliffs, no smearing, no
billboards at wrong angles.

**Effect shaders animate.** Water wheel and its falling-water effect differ by
**37.5 / 38.9** (mean absolute, 0–255) between captures 3 seconds apart, against
a frame mean of 7.5 and a static roof at 3.0. Note the trap that caught me: the
forge coals are a largely static emissive in vanilla, so testing there reads as
"no animation", and clouds, snow particles and ripples animate by particle and
vertex motion rather than material UV — their `texCoordOffset` reads zero with
world suppression both on *and* off, so a zero there means nothing.

**Grass face normals.** Bright and dark blades side by side within a single
clump, which a shared instance-up normal cannot produce. Shader branch is
`surface.useFaceNormals ? flippedTriangleNormal : getBentNormal(...)`.

**Remix at default graphics settings with RTXDI.** The four performance
overrides were removed from `ConfigureRunningTest.ps1`. `RtxOptions` now logs
what the preset resolved to:

```
[RTX.preset] resolved graphicsPreset=1 (0 Ultra 1 High 2 Medium 3 Low 4 Custom)
  rtxdi=1 rayReconstruction=1 neeCacheFirstBounce=1
  unorderedResolveInIndirectRays=1 postFx=1 volumetrics=1
```

Auto picks High. **This costs 49.75 fps settled against 86.4 under the old
forced-Medium overrides.** Every frame-rate figure recorded in
`remix-performance.md` before this change was taken under those overrides and is
not comparable.

**Flicker.** 1.75% luminance swing across 20 frames at a parked camera, no
dropped or blown frames.

## Not fixed: camera jitter under motion

This is the open item. Three hypotheses were tested; two are disproved and the
third is the untested deployed build.

1. **spdlog flushing every `info` line on the render thread.** Plausible — the
   periodic diagnostics emit ~25 lines once per 120 frames, exactly p99. Changed
   to flush on `warn` plus a 2-second periodic flush. The frame-period tail did
   not move (p99 24–30 ms either way). Kept as hygiene, **not** a fix.
2. **Instance-set placements reporting the camera's motion as their own.** The
   reasoning is in `remix-performance.md` and is sound on its face. Instrumented
   and measured: across 120 frames spanning a 60-second orbit, with the origin
   confirmed to follow the camera across ~4,000 units and the counter reaching
   all 22 batches per frame, the largest suppressed motion was **0 on every
   sample**. Disproved. The first implementation of this "fix" also hung the
   loader, because `RtInstance::rebaseBy` marks the BLAS dirty and for an
   instance set that means rebuilding every batch every frame.
3. **Skyrim's TAA jitter reaching Remix.** Measured and real: the game's
   projection carries `_31 = -0.000260, _32 = 0.000309`, about a quarter of a
   pixel at 1920 wide, varying per frame; Remix jitters the projection itself
   for the upscaler and can only unjitter what it applied. This is the deployed,
   unverified change. **Verify it, and if the image is good, confirm with the
   user whether the shimmer is gone — it cannot be settled from a still.**

Useful framing for whoever picks this up: the frame-period tail is ~1.8× the
median both parked and moving, so the tail is not caused by camera motion, and
with 10.3 of a parked 13.6 ms already spent waiting on the GPU it is GPU-side.
The retained preserve path does **not** collapse under motion (7918 preserved
during an orbit against 7947 parked) — an earlier note claiming otherwise is
stale.

## Other open defects

- **Cell transitions kill the process.** `0xc0000005` with `rdx = 0xc046`,
  faulting in `BSShader::SetupTechnique` (vtable slot 2) called through a
  `BSRenderPass::shader` with a corrupted vtable. **Not the scene lights** — it
  reproduces byte-identically with `CS_REMIX_NO_SCENE_LIGHTS=1` and no
  `[RemixScene.lights]` line in the log at all. Pre-existing.
- A genuine heap overflow was found and fixed on the way:
  `LightManager::prepareSceneData` sized `m_lightMappingData` to
  `current + previous` active lights and then indexed it with a buffer index
  that can be older than "previous" for any light that skips a frame. Fixed, but
  it is *not* the crash above.
- `22 instanced batches (0 retained)` — grass and trees never take the retained
  path.
- `3 failed uploads` every frame (the `Fish:1` meshes).
- Frame generation reports `fg=off method=FSR-FG`. `rtx.dlfg.enable` is back in
  the harness and matches Remix's default, but the method is chosen by the
  plugin's own Upscaling feature, not by Remix, so presented rate currently
  equals rendered rate.

## Tooling notes that will save time

- **The Survival Mode prompt fires on every fresh `coc` into an exterior, and a
  second one fires after the first is answered.** It is modal, and while it is up
  the world behind it renders through the game's own path — so any capture taken
  then is of vanilla Skyrim, not Remix. Several hours of confusing captures came
  from this. `ConfigureRunningTest.ps1` now clicks "No" and keeps checking until
  the box has stayed shut across five polls. DevBench cannot answer it and Skyrim
  ignores synthetic key events; a synthetic *click* does work while the cursor
  menu is up.
- Do not run a click-watchdog in the background *during* a capture — the clicks
  drag the free camera and you get a smeared frame at the wrong yaw.
- `CS_REMIX_NO_SCENE_LIGHTS`: `1` disables the light path entirely, `2` captures
  the game's light list but makes no Remix call. Use `1` to take the lights out
  of the picture when attributing a crash.
- The crash handler in `XSEPlugin.cpp` now logs module+offset for the faulting
  instruction and for the return addresses on the faulting stack, so a crash is
  symbolised straight into `CommunityShaders.log` without cdb.
- **Other sessions share this game install and this worktree.** A peer session
  was building the same plugin and driving the camera in the same process while
  measurements were running, which invalidated three separate A/B runs. Check
  `ListAgents` and the deployment backups before blaming your own change.
- Ghidra headless: project path is `ghidraprojects/BethesdaGhidraScripts`, the
  program folder argument is `BethesdaGhidraScripts/skyrim/ae1.7`, scripts must
  live in `ghidrascripts/` and be Java. Disassembly works; the decompiler
  returned nothing for the two functions tried.

## Suggested next steps

1. Launch the deployed build and check the image. That is the only thing
   standing between the jitter work and an answer.
2. If it is good, ask the user whether the shimmer under motion is gone. No
   still frame can settle it.
3. If it is bad, roll back and note that the jitter is real but that neither
   substituting the unjittered matrix nor zeroing `_31`/`_32` is the way to
   remove it — the next thing to try is telling Remix what jitter was applied
   rather than removing it.
4. The cell-transition crash is the largest remaining stability problem and is
   independent of everything above.
