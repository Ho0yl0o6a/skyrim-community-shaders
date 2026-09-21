# Character visibility investigation — 2026-09-20

## Same-frame lighting diagnostic827

PAIRED_LIGHTING_FRAME displays TL final, TR primary diffuse luminance,
BL primary specular luminance, BR secondary plus shared luminance. The three
diagnostic panels use L/(1+L). Primary lobes include actual denoiser unpacking,
BSDF factors, albedo remodulation, roughness demodulation and primary attenuation
from loadPrimaryRadiance. They are measured BEFORE volume attenuation, fog,
alpha composition and reconstruction/postfx, unlike826. Secondary/shared are
combined in BR, not separated; direct/indirect are combined in each primary
lobe. Inactive/miss primary lobes initialize to zero. DebugView readback is
within the same invocation/pixel; no new descriptors or history. Dispatch is
post-tonemap only, native refraction remains enabled. Prefer NoUpscaling startup
for isolation; normal RR output includes subsequent reconstruction and is not a
pixelwise sum of these panels. PNG quantization is not raw float measurement.

## 2026-09-21 00:17 native MSN grazing correction

Latest user report shifted focus to grazing black on body/rock grass. Native
model-space and grass normal preservation now requires positive NdotV; existing
getBentNormal handles rejected inputs. Runtime65BC214D deployed, normalPID12692.
See remix-normals.md newest entry for evidence:64-frame warmed808 capture0–10
red pixels vs2589–2752 before; orbit0–7. Not matched poses/native parity.
Eye brightness is still unresolved, and this does not close face animation or
head normal correctness. Valid authored normals and SSS settings are unchanged.

## 2026-09-21 00:03 alpha composition excluded on captured bright eye

PROGRESS, no production eye fix. Added debug826 PAIRED_ALPHA_COMPOSITE_FRAME:
TL final, TR background contribution luminance (radianceOutput*backgroundAlpha),
BL alphaBlendOutput luminance, BR backgroundAlpha. Terms sampled at final
composite sum AFTER fog/volumes, BEFORE upscaling/postfx. Luminance displays
use L/(1+L), transparency linear0..1. DebugView carries the three terms from
composite; includes sparse-inactive pixels, no diagnostic history, same source
coordinates, post-tonemap dispatch only, native refraction path preserved.
No bindings added. This diagnostic separates alpha from background, NOT direct
from indirect/specular, nor raw floating-point export from PNG quantization.
Source confirms stochastic-alpha enable is not gated off for826.

Build first62865 failed on wrong CompositeArgs member debugView; corrected to
debugViewIdx, retry20518 exit0. build-paired-alpha-composite-retry.log.129 Python
CPU/source tests pass; composite and both debug_view SPIR-V variants validate
Vulkan1.3 scalar-block-layout. No GPU unit suite or performance acceptance.
Runtime deployed BD4A2208AB2ABB47E7228848BD7E66BD18FADCA51EF00FF60AA8F34DC7A9510F;
backup/symbols20260920-paired-alpha-composite preservesDCF07D06. DXGI066D7215 and
hostBB430485 unchanged. Followed runtime shader-first build/deploy workflow.

Native-resolution NoUpscaling PID6148 captures, both verified frozen (zero pose
changes/unsubmitted),64 consecutive frames integrity pass, sheets inspected:
.research/sequences/eyes-alpha-terms1-1789945214 (1066..1129),
eyes-alpha-terms2-1789945223 (1393..1456).
Full1067 bright vs1129 dark inspected. Eye brightening is in TR background
contribution, not BL alpha contribution. In both fullframes, ROI x500..680,
y230..310 in TL-source-panel coordinates has BL RGBmax0, BR RGBmin255. Those
are encoded PNG values (not a claim of exact zero HDR before quantization).
This supports main surface-lighting origin rather than alpha composition;
do not spend another turn tuning alpha neighbor search to fix this sample.
Next split main diffuse/specular and direct/indirect contributions on the SAME
frozen failing frame, with primary/secondary validity if needed. No eye fix.

Archive eyes-alpha-terms-complete.log. Diagnostic run closed normally, normal
PID43460 started00:02:11, Save1 loaded/playerLoadedtrue(frame1084), final third/
freeCamfalse verified, debug0 configured, RR1 verified, native22draw/75compute
suppressed after load. All jobs completed. No saves/deletes/persistent settings
writes. Accepted grass/water and shared-bone fix unchanged. Goal incomplete.

## 23:49 frozen-pose eye flash reproduced after shared-bone fix

PROGRESS by isolation, no new production fix. Revalidated normal PID38924 alive
at start and end; no second crash observed. Current eye census reports
EyeBrown_n.dds normalTexture, unlike an earlier empty-path observation: do not
assume eye normal input is missing. Actual diffuse remains EyesIceBlue.dds,
native feature16, alpha4333/128, one bone,186vertices, thin/skin false.

New .research/sequences captures on the unchanged BB430485 host/DCF07D06 runtime:
- eyes-shared-frozen-material-1789944407, view825,9672..9735.
- eyes-shared-frozen-surface-1789944417, view823,10013..10076.
- eyes-frozen-visibility-1789944509, view805,13324..13387.
All64-frame integrity passes and exact host pose joins: ZERO changed pose
frames, ZERO unsubmitted frames. Camera makes actual XY/yaw orbit; animation
frozen by tfc1, then restored by harness. Paired face sheets480,180,440,300;
visibility full-screen sheet960,360,880,600. All inspected.

Eyes visibly switch white/dark despite frozen host geometry/morph/bones. Full
material frames9672(bright) and9735(dark) inspected, camera returns approximately
to starting view. No obvious primary roughness/specular-albedo/normal jump;
this is visual, NOT numeric or alpha-layer identity proof. Frozen result rules
out a REQUIREMENT for host animation changes, not all GPU geometry faults.
The beard remains attached. Do not retune skin/lighting to mask this.

805 source semantics: red invalid light sample, blue opaque blocker, green
unoccluded; only primarySelectedIntegrationSurface. Eye region broadly red,
under-eye blue in this run, not an obvious eye-only green transition. This is
a separate run without simultaneous final output, so it does NOT attribute
the bright frame to a direct-light failure. Next useful diagnostic is paired
final plus direct/indirect/alpha lighting on the SAME frozen failing frame,
with coverage validity for the eye layer. Avoid claiming the primary material
view describes every blended layer. No shading/code/deploy changes this turn.
Harness restores debug0, auditfalse, animation and freeCamfalse. Final live
check frame17059: idle vanity camera active; setPov third acknowledged but
immediate get still reports vanity. Do not claim final third-person verified.

## 23:44 shared bone sampling fixes reproduced head/beard disagreement

PROGRESS, not full character/goal acceptance. Before-fix normal PID53148:
face-pose-original1-1789943485 (5398..5461), original2-1789943495
(5720..5783), original3-1789943504 (6047..6110), under .research/sequences.
All integrity and exact pose joins pass. Correct paired-view face crop is
260,120,220,240 (initial original1 crop450,150,450,350 missed the face).
Original2 full5745 and5769 show beard absent in BOTH final and albedo.
Mesh/retained/morph unchanged, no unsubmitted meshes. Crucially, head and beard
bone rotations agree exactly in the OTHER62 frames, but disagree in those TWO
failed frames. Composed world rotation max deltas .00196613 and .01810457.
This is positive failure-frame host evidence, not another negative sample.

Capture previously read each live bone separately during expensive mesh/material
work. Animation advances during that walk. Submit now snapshots each unique
boneWorldTransforms source once BEFORE capture, and all parts reuse that
frame-local value. No previous-frame cache, no bone remapping, no skin/alpha/
lighting retuning. Use instance.world for the inverse, matching the actual
submitted object transform. This ensures shared-source consistency, not an
atomic whole-skeleton/native morph snapshot; those broader guarantees remain
unverified. Changes apply to skinned geometry generally, not only this beard.

Host build39327 exit0,128 Python CPU/source tests pass. AnalyzeFacialAlignment
compares this save's matching head/beard bind rotations (not a universal skeleton
validator). Deployed BB430485173514DAACEB26E0AF555B1B85CF3A328F2F46B4DAD234C4DDA5E077;
backup/symbol label20260920-shared-bone-snapshot preserves883D28CF. Runtime/DXGI
unchanged. No GPU unit suite or performance acceptance.

Post-fix normalRR captures (all64 frames, integrity pass, sheets inspected):
- PID52056 face-shared-bones1-1789944037 (1255..1318),
  bones2-1789944046 (1567..1630), bones3-1789944056 (1884..1947).
- PID38924 face-shared-orbit1-1789944180 (1506..1569),
  orbit2-1789944192 (1863..1926): actual XY/yaw orbit; close crop480,180,440,300.
- PID38924 face-shared-normal-1789944207 (2177..2240): debug0/auditoff,
  default wider camera, crop520,240,440,480.
All FIVE audited sequences have63 changed-pose transitions, no unsubmitted
meshes, and zero head/beard rotation mismatches (320 totalframes). Beard stays
attached in inspected sheets, including normal-rendering run. Orbit eyes still
have sharp dark/bright changes (e.g.1552/1908); do not claim eye fix or total
flicker freedom. Some orbit frames move face out of crop; not all eye pixels
were compared numerically. Next isolate remaining eye visibility/lighting;
also validate longer play/NPC/first-person and skeleton/morph coherence.

Stability caveat: PID52056 crashed23:41:29 AFTER three successful captures,
before orbit command. Native fault SkyrimSE+100efa4, callers ef3bcc/fd5b4d;
cause unknown, not established unrelated to this build. Preserved
.research/testlogs/shared-bones-first-host.log, shared-bones-first-runtime.log,
shared-bones-first.dmp (1,594,947 bytes). Dump source is GAME ROOT, not SKSE logs.
No retry loop: restarted once, PID38924 normal started23:42:15, completed remaining
captures and still running at frame4524. Existing Save1 only, no saves/deletes.
Final third/freeCamfalse/auditfalse, debug0 restored, RR1 verified, native14draw/
75compute suppressed. Accepted grass/water untouched. All jobs finished.

## 23:28 frame-tagged host pose audit deployed; no character fix

PROGRESS: added opt-in 256-frame characterAudit history under cs.sceneAudit.
RecordAudit snapshots cached dynamic player meshes: geometry/mesh/retained
identity, vertex count, submitted flag, FNV64 morph-data hash, world rotation/
translation/scale and full submitted bone matrices. Seven player face classes
observed: brows88, eyes186, beard250, hairline217, hair1140, mouth141, head898.
These are HOST CACHED INPUTS, not native vertex parity, GPU readback or BLAS proof.
No hot-path collection when cs.sceneAudit is false. JSON work when enabled may
perturb timing; short negative captures do not prove absence of a race.

CaptureCharacterSequence -PoseAudit arms just before capture, disables and reads
history after capture, writes pose-audit.json and restores audit off in finally.
Rejects an already active audit or unsupported host. AnalyzeCharacterPoses joins
exact host frame IDs to PNG sidecars and rejects missing/duplicate frames and
empty/not-ready poses; writes pose-analysis.json. Run AnalyzeFrameSequence too:
pose coverage alone is not capture-integrity or visual acceptance.

Release host build32324 exit0,126 Python CPU/source tests pass. Deployed host
883D28CF79AFFAF890E4278AF3691FE6A72A6522521458F8B304DB2DC484412F;
backup/symbol label20260920-character-pose-audit preserves BB6D1676.
RuntimeDCF07D06/DXGI066D7215 unchanged. No production pose/shading changes.

PID50776 normalRR: all64-frame integrity and pose joins pass:
face-pose-animated1-1789942939(1866..1929), animated2-1789942949(2190..2253),
face-pose-frozen-1789942958(2520..2583),
face-pose-stationary1-1789943012(4548..4611), stationary2-1789943022(4921..4984),
stationary3-1789943032(5258..5321).
Frozen recorded poses have ZERO changes across64frames, validating this freeze
at host-input level. Animated changed-frame counts51/49; stationary53/47/52.
No facial mesh unsubmitted in any joined frame. Sheets inspected: no large
beard dropout reproduced. This is not proof the bug improved or was fixed.

PID1780 NoUpscaling/NRD all64 integrity/pose joins pass:
face-pose-nrd1-1789943195(1831..1894), nrd2-1789943205(2160..2223),
nrd3-1789943215(2496..2559). Changed-frame counts47/48/48, unsubmitted0.
Sheets inspected, no large dropout. Head/view angle differs from prior failing
captures; not a matched before/after. Archives face-pose-rr-complete.log and
face-pose-nrd-complete.log. Earlier unaudited frozen orbits PID47820:
face-frozen-orbit1-1789942471(7266..7329), orbit2-1789942481(7593..7656),
integrity pass/no large dropout in sheets, no independently verified freeze.

Next obtain a failing frame with -PoseAudit, preferably matched native-resolution
camera/head pose, then inspect its exact fields versus neighboring frames.
Do not repeat disproved upload-order suspicion: csRemixRender flushes uploads
then skinning after retained replay (documented below). Audit/source inspection
did not establish a new synchronization defect. Full goal ACTIVE/incomplete.
Normal restored PID53148 started23:29:01, Save1/playerLoadedtrue/third/freeCamfalse,
RR1/auditfalse verified, debug0 configured; native22draw/75compute suppressed.
All jobs complete, no saves/deletes/persistent settings changes.

## Paired material diagnostic 825

Built and tested 23:08-23:11. Build8667 exit0,122 CPU/source tests pass,
both debug_view SPIR-V variants validate (Vulkan1.3/scalar-block-layout).
Deployed D3D11 DCF07D0697B85C55C5FDEA13564C127F566F43786AD25F50C6BA0379310A5C55;
backup/symbol label20260920-paired-material-frame preserves83B2618E.
DXGI/host unchanged. No production shading fix or GPU unit suite.

NoUpscaling/NRD PID44876 three64-frame integrity passes:
eyes-paired-material1-1789942134(1153..1216),
eyes-paired-material2-1789942144(1466..1529),
eyes-paired-material3-1789942153(1766..1829).
Sheets inspected; full1767/1768 show dark-to-bright eye without an obvious
roughness/specular-albedo jump or wholesale normal flip. This is visual,
not a numeric material/instance-identity proof, and motion changes coordinates.
Full1492/1494 also inspected. Full1485 shows most beard disappearing; user
also confirms beard/face flicker persists. Eye/beard/face NOT accepted.
Next audit visibility/shadow traversal and animated-geometry lifecycle on
failing frames; don't retune brightness or claim source tests prove skinning
timing. Archive eyes-paired-material-complete.log. No saves/deletes/config writes.
Normal session restored PID47820 started23:11:04, Save1 loaded, third/freeCamfalse,
RR1 verified, debug0 configured, native22draw/75compute suppressed after load.
All build/capture jobs completed; overall goal remains ACTIVE.

Opt-in view825 shows TL final image, TR primary specular albedo captured at
demodulation, BL virtual world shading normal (same mapping as view17), BR
perceptual roughness. Each panel uses the same current source pixel; debug
history is bypassed and dispatch is post-tonemap only. Native refraction remains
enabled. Use NoUpscaling for same-resolution source correspondence. This is a
diagnostic, not an eye shading fix. Primary resolved material data may describe
the surface behind a separately blended object; do not infer object identity
from screen position alone.

Before this addition, normal RR PID10472 captured view163 alpha membership
eyes-alpha-membership-1789941544 frames5260..5323, all64 integrity pass.
Inspected full5260 and sheet: magenta invalid alpha surfaces throughout the
view. This does not support the borrowed-alpha-light hypothesis for that run;
it is not same-frame evidence from a confirmed bright-eye frame.
View16 eyes-shading-normal-1789941653 frames8784..8847 also passes integrity.
Normals remain structured on inspected sheet, but absent same-frame beauty
prevents attributing an individual flash. These are not character acceptance.

## 22:57 transparency shader safety fix; eye flash still reproduces

PROGRESS, not an eye/character acceptance. Fixed divergent group barriers in
composite_alpha_blend.comp.slang: out-of-bounds/invalid lanes now participate
with safe input coordinates and masked output; shared-neighbor jitter is clamped;
an end-of-iteration barrier separates shared reads from subsequent writes.
No material/brightness/foliage/water/SSS retuning.

BuildRuntime completed exit0 (build-alpha-neighbor-safety.log, session21486).
121 CPU/source tests pass; compiled composite_alpha_blend.spv passes spirv-val
with Vulkan1.3/scalar-block-layout. No GPU unit suite or performance acceptance.
Deployed D3D11 SHA256
83B2618EEC46553CD5DC2746E74663AD7BDB921AC095FAA5A3C72FEF1E6AE7A4.
Owned backup/symbol label20260920-alpha-neighbor-safety preserves E34AAEE;
host BB6D1676 and DXGI066D7215 unchanged.

PID24676 NoUpscaling/NRD debug824 camera orbits, all64-frame integrity passes:
eyes-alpha-safe1-1789941140(1868..1931), safe2-1789941150(2206..2269),
safe3-1789941160(2565..2628). Sheets and full2610 inspected: eye still flashes
bright in final AND same-frame composite. Shader hazards are corrected, but
this does not resolve or establish the cause of the character issue. Do not
infer improvement from these small samples. Archive alpha-neighbor-safety-complete.log.
Next: isolate eye alpha-lighting/visibility inputs on failing frames rather
than repeat weak negative global-toggle comparisons.

Restored normal PID10472 started22:56:47, existing Riverwood Save1 loaded;
playerLoadedtrue, third-person and freeCamfalse verified. Normal launch clears
diagnostic startup overrides; native22draw/75compute suppression logged after load.
No live build/capture jobs, saves/deletes/persistent config changes. Goal ACTIVE.

## 22:45 paired pre-postfx composite catches eye brightening

PROGRESS, no production shading fix. Added opt-in debug824
DEBUG_VIEW_PAIRED_COMPOSITE_FRAME: TL final, TR current composite HDR mapped
with max(C,0)/(1+max(C,0)), BL depth, BR motion. Same source coordinates as823,
no debug accumulation, post-tonemap dispatch only, native-refraction final path
preserved. Existing Composite binding31 reads m_compositeOutput after composite,
before upscaling/dust/native refraction/bloom/motion blur/tonemap/lens effects.
Use NoUpscaling for same-resolution comparison. Diagnostic HDR display scale
differs from final; it is not a brightness-parity test or raw numeric export.

Built/deployed E34AAEE13555D0085403DB92D3D8DE150B190D296801D409F04E7C704FA80B0F.
Owned backup/symbol label20260920-paired-composite-frame preservesAA375669;
DXGI066D7215/hostBB6D1676 unchanged. build-paired-composite.log success,
118 CPU/source tests pass, no GPU unit suite. Normal debug0 unchanged.

PID43540 NoUpscaling/NRD three64-frame debug824 orbits pass integrity:
eyes-orbit-composite1-1789940491(1276..1339), composite2-1789940573(4623..4686),
composite3-1789940584(5001..5064). Full5002/5003 inspected: eye changes from
dark to bright in final AND a localized bright eye patch appears in the same
frame's composite panel. Final blurred while composite sharp. Therefore later
postfx/upscaling do not originate this brightening, though they may amplify it.
Composite already includes denoising/stochastic-alpha lighting; this capture
does not identify which earlier stage/input is wrong. Prior raw1849 confirms
denoising alone is insufficient explanation. No claim eye now correct.

Earlier this turn PID40896 stochastic alpha off/on two64-frame captures:
eyes-orbit-no-stochastic-1789939931(12892..12955) and
eyes-orbit-stochastic-restored-1789939986(14765..14828). Integrity passes, no
strong flash on inspected sheets; INCONCLUSIVE A/B. Restored optionTrue.
Archive eyes-stochastic-isolation-complete.log. Shader source lead:
composite_alpha_blend.comp.slang estimates transparent lighting from neighbors
or volume. Its shared-neighbor offset branch lacks coordinate clamp, and its
barrier is nested under per-pixel validity; these require a focused shader
correctness audit, not attribution to the current eye bug without evidence.

All sequences/sheets in .research/sequences and .research/testlogs; archive
eyes-paired-composite-complete.log. Next: instrument pre-composite eye alpha
lighting/visibility on failing frames. Avoid more isolated negative toggles.

Restored NORMAL PID48564 started22:45:16, existing Riverwood Save1 loaded;
22:45 playerLoadedtrue/freeCamfalse/RR1/debug0, native22draw/75compute suppressed
in initial first-person load; third-person requested afterward. No active jobs,
no saves/deletes/persistent settings changes. Full goal remains ACTIVE.

## 22:28 camera-position orbit reproduces bright eyes, including raw lighting

PROGRESS; no production fix. A yaw-only sweep changes orientation, not the
surface-to-camera vector. Added optional OrbitCenter to CaptureCharacterSequence:
rotate XY position clockwise around center together with yaw. Every capture now
writes capture-manifest.json with requested/observed24-step camera trajectory.
API movement is asynchronous: observed samples can lag commands; these are NOT
per-render-frame pose joins. Framebuffer sidecars remain the capture-integrity
authority. Some crops lose the eye at the sweep extreme; inspect full frames.

Reproduction: CameraX13700 CameraY-48192 CameraZ-150, default yaw-2.07738137,
OrbitCenter13665.155,-48229.410, YawSweep0.2, DebugView823. Three64-frame sequences
pass integrity and visually show the eye becoming bright:

- Normal RR PID44360: eyes-orbit-rr-1789939255 frames13071..13134.
  Inspected full13117(dark) and13118(bright). Beard/brow visibility also changes.
- NoUpscaling/NRD PID22932: eyes-orbit-nrd-1789939385 frames1781..1844.
  Full1826 confirms very bright sclera/iris in final, eye region dark in albedo.
- RawLighting PID2180: eyes-orbit-raw-1789939501 frames1803..1866.
  Full1849 confirms bright eye even without RR/upscaling/denoising. Raw still
  includes postfx/motion blur and other temporal systems; not a fully history-free
  renderer. Thus RR/NRD alone cannot explain the flash. Same-frame resolved
  albedo stays dark, but transparency can prevent it representing final eye
  shading. Do NOT infer correct eye geometry from this albedo view alone.

Sheets/JSON in .research/testlogs/eyes-orbit-{rr,nrd,raw}-beauty.*;
runtime archives eyes-orbit-{rr,nrd,raw}-complete.log. Prior wider yaw-only
eyes-wide-rr-1789939181 and eyes-wide-paired-1789939191 are not orbit tests.

Normal PID40896 subsequent motion-blur-off capture eyes-orbit-no-motionblur-
1789939624(2017..2080), then motion-blur restored capture eyes-orbit-motionblur-
restored-1789939668(3697..3760), both integrity pass. Neither inspected sheet
reproduces the strong sustained white eye. This is an INCONCLUSIVE A/B, not
proof motion blur causes/fixes it. rtx.postfx.enableMotionBlur restoredTrue in
finally; do not leave it disabled as a workaround.

Source audit correction: VANILLA_EYE_NORMAL occurs only in Lighting.hlsl's
conditional override, no definition found in package/src. Its absence from
import is NOT established mismatch against current CS. Eye alpha4333 maps
standard SRC_ALPHA/ONE_MINUS_SRC_ALPHA (not emissive translation). Generic
import still usesroughness0.8 and does not map full native eye specular/env
parameters. These are fidelity gaps, not proven flash cause. Next investigate
eye visibility/shading before postfx at failing motion frames, and native eye
alpha/material inputs. Do not mask eye flash by global brightness/SSS changes.

Normal PID40896 started22:26:05, Riverwood Save1 loaded;22:28 playerLoadedtrue,
third/freeCamfalse, RR1, debug0 restored by finally, native14draw/75compute
suppressed. No binaries changed this turn(AA375669 remains), no saves/deletes/
persistent settings writes.117 CPU/source tests pass; no GPU unit suite.

## 22:16 selected beard GPU probe passes; eyes remain OPEN

Diagnostic-only runtime AA3756691F879B7FEC7AEA9348BFD4CEB6BB925EA4D6A74178121A74FBF88A17
built successfully (build-selected-face-probe.log), deployed with owned-DLL
backup/symbol label 20260920-selected-face-probe. DXGI/host unchanged.
117 Python CPU/source checks pass; no GPU unit suite run.

LaunchTest -SkinProbeVertices 250 selects a bounded vertex-count/material
class, implies FaceSkinProbe, and arms only during debug823. Default898 and
body32 modes retained. Alternate classes allow1..4 total bones and1..4096
vertices. CheckSkinProbe --face --vertices 250 validates the selected count.
Normal launches clear the selector. No production rendering behavior changed.

Live census confirmed player HumanBeard02 is250vertices/2bones/nativeTBN,
so it already takes GPU skinning (not the small-mesh CPU path). PID43788
NoUpscaling/NRD run captured64 GPU samples: maximum position error1.52588e-5,
normal error1.78814e-7, no nonfinite values, animated bone hashes. Report
.research/testlogs/beard-selected-probe.json; archived runtime log
selected-beard-probe-complete.log. This validates captured input/output math,
NOT native pose timing, actor uniqueness, every bad frame, BLAS or visibility.
Runtime and host frame numbers have not been explicitly joined.

All three paired64-frame captures pass integrity; inspected beauty sheets
show no unmistakable beard dropout: beard-probe1-1789938616(1402..1465),
beard-probe2-1789938625(1714..1777), beard-probe3-1789938634(2019..2082).
Negative observations do NOT override prior confirmed5298 failure.

CaptureCharacterSequence now accepts explicit CameraX/Y/Z/Pitch/Yaw to frame
eyes. Close-up attempts nrd1/4/5 were poorly framed; nrd2 rejected because yaw
changed; nrd3 returned unexpected yaw and is not a controlled comparison.
Do not attribute yaw mismatch to user motion without evidence. Working close-up:
x13700,y-48192,z-150,pitch0,yaw-2.07738137. Normal RR PID44360 capture
eyes-close-rr-1789938925(1374..1437),0.06rad sweep, debug823, integrity pass.
Beauty/albedo crops570,210,230,150 and1530,210,230,150 saved to testlogs.
Eye visible in inspected beauty sheet, but bright flash not reproduced.
Eye issue remains OPEN, not accepted. Live eye186vertices/1bone/nativeTBN,
feature16, empty imported normalTexture, alphaFlags4333/threshold128.
EYE shader special handling remains a source lead, not proven cause.

Restored normal startup/RR1 on PID44360 at22:14:41; Riverwood existing Save1.
22:16 playerLoadedtrue/third/freeCamfalse, debug0 restored in capture finally,
native14draw/75compute suppressed. No saves/deletes/config writes. No active
jobs. Next: reproduce eyes with a wider/orbit sweep or different angle; join
facial geometry/visibility evidence to an actual bad frame before changing shading.

## 21:58 paired diagnostic catches beard loss BEFORE denoising

22:00 follow-up: normal-RR sweep paired-eye-rr-yaw-1789937981 (1647..1710),
PID17144,0.08rad, debug823, integrity pass. Inspected beauty sheet has no
unmistakable white-eye flash; eye coverage is too small for acceptance. Need
closer camera/eye ROI, not a claim user issue is absent. Shader input modes and
same-frame panels now available for that work. Debug0/freeCamfalse restored by
capture finally; normal RR1 runtime remains live, third person, native14draw/
75compute suppressed at22:00:01. No live jobs.

PROGRESS, no visual fix claimed. New opt-in debug823 reads four panels from the
same frame: top-left post-tonemap final image, top-right resolved material albedo
written in geometry_resolver, bottom-left abs(linearZ)/(abs(linearZ)+100),
bottom-right screen motion encoded RG=0.5+0.05*motion. Nearest downsample by2,
no diagnostic history accumulation, always post-tonemap dispatch; native
refraction composition remains enabled. Other debug/production behavior unchanged.
This is resolved debug albedo, not a raw DDS/native-raster parity test. Primary
depth/motion may differ from resolved surfaces after PSR. Panels are best used
without upscaling; temporal upscalers inherently combine history in final output.

Built/deployed D3D11 DE9AB880A2C2C8CA88D9E858D34251E7F72E694DE1D9064444AD8EBAE192DB46;
DXGI066D7215 unchanged, hostBB6D1676 unchanged. Owned backup/symbol directories
20260920-paired-surface-frame preserve previous4B0C70B4 runtime. First build98321
failed misplaced switch case; corrected build69664 and final4158 both exit0.
Logs build-paired-surface*.log.116 CPU/source checks pass, including3diagnostic
contracts, not GPU/visual correctness. No GPU unit suite executed.

PID41820 (-NoUpscaling) sequences, all integrity pass unless noted:

- Initial paired-surface1 request REJECTED due camera pitch movement; not copied.
- paired-surface-motion1-1789937597,2464..2527: four panels visually verified;
  endpoints stationary despite opt-in AllowCameraMotion. No obvious dropout.
- paired-surface2-1789937657,4967..5030: partial dropout5020.
- paired-surface3-1789937666,5288..5351: clear dropout5298 and5316.
  Full frame5297 versus5298 inspected: beard disappears in BOTH final and albedo
  on5298. This directly contradicts a denoiser-only explanation. Prior raw clean
  samples were weak negative evidence. Investigate pre-denoise geometry/pose/
  alpha-cutout visibility; do not fix by disabling NRD or changing brightness.
- paired-surface4-1789937675,5622..5685: no obvious dropout in inspected sheet.

All four accepted stationary sequences have matching endpoint camera coordinates.
Sheets under .research/testlogs, sequences under .research/sequences. Face crops
beauty295,145,135,180 and albedo1255,145,135,180 at1080p. Full paired frame retains
more information. Archive paired-surface-complete.log before normal restart.

Latest user: eyes go very bright WHEN MOVING CAMERA. Keep separate from beard
dropout. CaptureCharacterSequence now supports explicit -AllowCameraMotion and
-YawSweep up to0.2rad; reports cameraAfter/cameraStationary/yawSweep, rather than
mistaking identical endpoints for no commanded motion. Default camera guard
remains. paired-eye-yaw-1789937835 (11466..11529),0.08rad sweep, debug823,
NRD/noDLSS: no unmistakable white-eye flash in inspected beauty sheet. Insufficient
eye resolution and RR disabled: NOT validation or contradiction of user report.
Eye source audit: Lighting.hlsl EYE disables wetness/soft/back/rim lighting,
uses eye-center normals when VANILLA_EYE_NORMAL, and special vertex AO handling.
Remix generic material import does not implement the full EYE path; this is a
fidelity gap, not yet evidence of the motion-flash cause. Need normal-RR sweep
and closer eye/material/normal evidence independently of beard investigation.

## 21:42 native-resolution denoiser test — dropout persists without DLSS

PROGRESS, no character fix or production binary change. Added startup-only
LaunchTest -NoUpscaling: upscaler None, RR off, denoiser default preserved.
Normal launch removes overrides. PID21148 started21:36:35, RR0 preset logged;
visually denoised output contrasts with the preceding raw test. No getter for
actual upscaler/denoiser state yet; source env mappings confirmed.

All three fixed-camera sequences fail visibly (capture integrity passes):

- morph-nrd-native1-1789936646, frames1479..1542, dropout1515.
- morph-nrd-native2-1789936661, frames1989..2052, dropout1990.
- morph-nrd-native3-1789936676, frames2514..2577, dropout2536.

Frames show patchy/disappearing beard against a relatively stable hair/face;
do not turn this observation into a proven geometry or denoiser diagnosis.
No-upscaling removes DLSS as a necessary cause, but timing differences and
denoising visibility can confound the earlier negative raw samples. Need same
frame albedo/depth and beauty to distinguish geometry from composition.

Source findings for the next diagnostic: existing raw capture in
rtx_context.cpp ~800 preserves gbufferAlbedo before demodulate/denoise/composite,
and also exports noisy/denoised radiance and post-composite output. A bounded
same-frame burst/ROI capture would avoid comparing different animations.
DO NOT use the built-in FinalRenderWithMaterialProperties composite debug view
as same-frame proof: rtx_debug_view.cpp ~1671 cycles one tile per frame using
frameIndex modulo number of views, leaving other tiles from older frames.
Standalone debug23 similarly is not simultaneous beauty/albedo evidence.

Archive .research/testlogs/morph-nrd-native-complete.log. Face contact sheets
and JSON live there; sequences in .research/sequences. LaunchTest syntax and
113 CPU/source tests pass. No GPU unit suite. Returning to normal rendering;
eyes/brows/beard, skin leakage and full goal remain OPEN.

## 21:34 isolation checkpoint — beard, eyebrows and eyes remain OPEN

Latest user narrows remaining jumps to beard/brows and reports eyes buggy too.
Do not use head-only skinning validation as evidence for these accessories.
No production binaries changed in this checkpoint. 113 CPU/source tests pass;
LaunchTest syntax checked. No GPU unit suite or visual acceptance.

CaptureCharacterSequence now supports requested animation freeze (`tfc 1`) and
debug view selection with finally restoration. Camera coordinates are guarded;
this does not independently prove native pose freeze. All sequences below pass
64-frame capture integrity. Paths are under .research/sequences; corresponding
face contact sheets/JSON under .research/testlogs, crop590,290,270,360.

- morph-frozen-lit-1789935426,10724..10787: no obvious dropout in short sample.
- morph-animated-facing-1789935479,12988..13051: debug808 mostly green, no broad
  facing reversal. Not validation of beard placement.
- morph-animated-albedo-1789935520,14425..14488 and
  morph-animated-albedo-repeat-1789935600,17548..17611: no equally obvious isolated
  dropout; debug output may bypass reconstruction, not paired bad-frame proof.
- morph-no-rr-character-1789935757,1757..1820: FAIL1776 beard dropout. PID34544,
  startup log rayReconstruction=0; DLSS SR and NRD still enabled.
- RR-off plus live opacityMicromap binding disabled: morph-no-rr-no-omm-1789935847
  (5365..5428), morph-no-omm-repeat1-1789935932 (8556..8619) looked clean, but
  morph-no-omm-repeat2-1789935947 (9104..9167) FAIL9136. Setter accepted; source
  reacts to binding changes, but no per-frame GPU binding counts recorded.
- Binding restored true: morph-omm-restored-1789935962 (9660..9723) FAIL9704.
  Earlier clean 64-frame samples must not be called a fix.

LaunchTest -NoRayReconstruction is startup-only; -RawLighting additionally sets
DXVK_UPSCALER_TYPE=0 and DXVK_USE_DENOISER=0. Default launch clears all three
diagnostic overrides. Avoid live target-resizing options. Raw PID33536 produced
visibly noisy rendering, log RR0, and no obvious isolated beard dropout in:
morph-raw-character-1789936293 (34743..34806),
morph-raw-repeat1-1789936353 (37148..37211),
morph-raw-repeat2-1789936367 (37647..37710),
morph-raw-repeat3-1789936380 (38153..38216).
This is negative evidence only: no runtime upscaler/denoiser getter was logged,
noise masks subtle defects, and animation/pose differs between sequences.
Do not conclude denoising is the root cause or disable it as a production fix.
Next discriminating tests: separate NRD from DLSS SR at startup and capture
registered native/RTX accessory position/depth/motion on an actual bad frame.

Archives: morph-pre-rr-isolation.log, morph-rr-off-complete.log,
morph-raw-complete.log. Normal launch restoration follows these tests.
HairCard runtime forces alpha testing and disables blending: host NIF blend
bits do not mean these beard/brow surfaces are rendered alpha-blended by Remix.
Eyes require their own material/geometry evidence. All character and skin-leak
issues remain unresolved; foliage/water/SSS brightness left untouched.

## 21:15 compatible morph snapshots deployed; visible jump still unresolved

PROGRESS, not character acceptance. Host now attempts csRemixUpdateMeshV1 for
position-only dynamic facial changes when static mesh inputs match. Upload
rebuilds the authored vertex/basis snapshot, keeps the existing mesh handle, and
updates the saved dynamic positions only on API success. Failure restores them
and takes the existing destroy/recreate path. Static replacements and true
deletions retain their existing behavior. Applies to head, beard, brows and eyes,
including MSN and authored TBN; grass/water/effect/landscape excluded.

Runtime tracks per-handle surface-layout signatures under s_mutex: counts,
triangle indices, skin weights/indices, format flags and material identity.
Updates require the exact signature; deleted/unknown handles are rejected.
Fresh immutable GPU snapshots retain the old Indices/GeometryDescriptor/layout
hashes but use new vertex hashes. AssetReplacer replaces the shared snapshot,
keeping in-flight/BLAS references alive. SceneManager invalidates matching
retained revisions and cached submeshes so unchanged bones/world transforms
cannot preserve stale vertex data. Same mesh handle avoids both node destruction
and trackRetainedDraw's mesh-change clear. No deletion suppression or stale
instance retention workaround. Original scoped sourceMaterial compile error
corrected before deployment; no failed build deployed.

Validation:113 Python CPU/source tests pass (four new source contracts, NOT GPU
unit tests or exhaustive API rejection tests). Runtime build20237 plus final46302
exit0; optimized host build92754 exit0. Logs build-morph-update.log,
build-morph-update-final.log, build-morph-plugin-v2.log. Backup/symbol label
20260920-morph-snapshot. Deployed D3D11 SHA256
4B0C70B4A9335F0F0B32143A572780006544986A70FD9E583E42E5CE32C8ABC9;
host BB6D1676EAD5D9993954A85C6038C066597D37BCFD56F8A0BA7AFAC0E22A7A13;
DXGI066D7215 unchanged.

Diagnostic PID33920: host log proves repeated same-handle morph updates for
MaleHeadNord, HumanBeard02, BrowsMaleHumanoid01 and MaleEyesHumanIceBlue.
Archive .research/testlogs/morph-snapshot-plugin.log. GPU readback report
morph-snapshot-skin.json and source morph-snapshot-probe-complete.log:64samples,
all898head vertices, ONE stable topology key, max position error7.62939e-6,
normal error1.78814e-7. This checks submitted pose evaluation, not native timing,
actual RtInstance/BLAS pointer continuity, every frame, or beard GPU output.

Front-facing sequence .research/sequences/morph-snapshot-character-1789935046
(PID33920,1398..1461) passes capture/camera integrity but STILL visibly fails:
frame1454 shows head/beard misalignment. Contact sheet
.research/testlogs/morph-snapshot-character-face.png. Thus fixing this history
gap does NOT explain/remove all visible jumps. No before/after frequency claim.

Normal PID47304 started21:11:50 without diagnostics; configured Scene and loaded
existingSave1. Sequence morph-normal-character-1789935190,2389..2452, camera
integrity passes but character is turned away and the old face ROI shows shoulder.
Inspected full frame2389: reject as a beard/face test. Do not call the normal run
a visual pass. Controls restored/freeCamfalse, thirdperson, playerLoadedtrue.

Next: capture matched native/Remix geometry/depth or poses on a failing animated
frame, distinguish actual pose/surface visibility mismatch from temporal shading.
Current readbacks occur before the bad-frame captures and cannot settle this.
In-place path still allocates/uploads complete snapshots and invalidates by
scanning retained entries; no measured performance improvement claim. No UI,
SSS, accepted foliage/water or persistent settings changes. Full goal ACTIVE.

## 21:00 head GPU readback passes its limited scope; morph lifetime lead

PROGRESS, NOT a character fix. Added opt-in LaunchTest.ps1 -FaceSkinProbe:
64 sparse samples of all898 vertices of the two-bone MSN head class. Runtime
probe compares captured GPU positions/normals against CPU evaluation of the
captured input vertices, weights, indices and submitted bone matrices. It follows
vertex-count/material class, NOT unique actor identity or native pose timing.
No production geometry/material/lighting algorithm changed this turn.

First probe version incorrectly used TopologicalHash for stable identity: imports
assign fresh synthetic Indices/GeometryDescriptor hashes on every morph rebuild
(d3d11_remix_api.cpp ~292), so it stopped at18/64. Archived incomplete log
`.research/testlogs/face-skin-probe-v1-incomplete.log`; do NOT count as pass.
V2 uses material hash with898vertices/two-bones and logs changing geometryKey.

V2 PID15632 report `.research/testlogs/face-skin-probe-v2.json`, archived source
`face-skin-probe-v2-complete.log`:64samples frames410..745, four geometry keys,
max position error7.62939e-6, max normal error1.78814e-7, zero nonfinite. Animated
bone hashes and vertex movement observed. This validates GPU evaluation ONLY,
not correct native pose, every frame, current BLAS contents, beard, or head/body
alignment. Probe samples do NOT overlap the subsequent captured sequences.

Sequences: head-probe-character-1789934062 (PID51804,21392..21455) shows visible
head/beard jump at21414; head-probe-v2-character-1789934270 (PID15632,2223..2286)
does not show an equally obvious isolated dropout in the inspected sheet. Both
under .research/sequences; corresponding *-face.png/json under .research/testlogs;
all integrity/fixed-camera guards passed. Short clean sequence NOT acceptance.

Concrete source lifetime contradiction to investigate next:
- Host RemixScene.cpp ~2042 retains its registration but calls DestroyMesh before
  recreating a morphed head/eyes mesh.
- SceneManager::destroyExternalMesh (~3190) calls tracker removal by spatial hash.
- DrawCallTracker::removeReplacementInstancesWithSpatialMapHash (~160) destroys
  matching nodes unconditionally, including hostOwned nodes. Thus retaining the
  host handle does NOT preserve that runtime node/history through mesh rebuild.
- Even if removal were bypassed, trackRetainedDraw (~186) calls pNode->clear()
  when the mesh-derived spatialMapHash changes. Both paths need consideration.

This is a real history-preservation gap, NOT yet proved to explain the visible
pose jumps. Do not just skip DestroyMesh/removal: true deletion must still retire
geometry and must not replay a missing mesh. A robust dynamic-morph update or
transactional mesh replacement must preserve compatible topology/history while
properly retiring genuinely removed parts; then correlate failed frames with
node/BLAS changes and compare native poses. Unique synthetic topology hashes also
force new BLAS buckets per rebuilt morph, relevant to history/performance.

Build64353/98368 exit0 (build-face-skin-probe*.log),109 CPU/source tests pass,
LaunchTest parses. No GPU unit suite run. Deployed D3D11 C13ECB2037C28B127B2C738D0D7D5987F517D39D14CD293A15D246F26C114F8C,
backup/symbol label20260920-face-skin-probe-v2; DXGI/host unchanged. Normal launch
restored (no probes) PID33016 started20:58:47. Full goal stays ACTIVE/incomplete.

## 20:49 ordered-ray continuation deployed; normal animation still FAILS

User confirms "still flickering" and suspects the black band was a symptom.
Do NOT mark character animation/visibility complete. The ordered-ray correction
removes the broad black brow band in 203311/203319/203327 screenshots, but
independent consecutive-frame evidence below proves remaining intermittent
facial failure. Moving head/body separation still needs native/API/GPU pose
comparison, not another brightness or SSS adjustment.

Build15636 exit0, build-ordered-continuation.log. D3D11 SHA256
BF1D1EF0F1FE44715F87B44D5CE2DD39E08D822A0E5C1C382BF674C45118A7E5;
DXGI066D7215 / hostA7AEA2A7 unchanged. Owned-DLL backup/symbol label
20260920-ordered-continuation.107 CPU/source tests pass; no GPU unit tests run.

Diagnostic PID53092, frozen face-ordered-continuation captures:
lit1789932791, facing1789932799, Pair8171789932807, Pair8181789932811,
Remix8171789932816, Remix8181789932820, first-hit8171789932826,
first-hit8181789932830, first-hit8191789932835. All camera guards passed.
Report .research/testlogs/face-ordered-continuation-layers.json. Native-foreground
ROI32714pixels: raw float32 depth error median-.0039795, p99+.469618,
105samples deeper by>5 and441absolute error>1. These include silhouette/alpha
disagreements and misses, NOT classified or accepted. Debug817 counts only83
deep-hit samples because misses don't run the hit diagnostic. The script now
reports raw-depth counts too; don't report83as all remaining failures. Different
pose across launches, so counts are NOT a matched A/B improvement percentage.

Animated diagnostic sequence .research/sequences/ordered-character-1789932946:
64consecutive frames3957..4020, fixed freecam but NO tfc1 freeze. Integrity passed.
Contact sheet .research/testlogs/ordered-character-face.png visibly shows beard
missing/displaced at3964,3970,3973,3981 and4014 (examples, not exhaustive count).
The broad black brow band is absent but head/hair/beard alignment flickers.
This is pre-Present framebuffer evidence, not WSI display/pacing certification.
New CaptureCharacterSequence.ps1 creates this capture and restores freecamfalse.

NORMAL PID47244 started20:41:43, existingSave1/Scene, no matched-sample/native
preparation overrides. Sequence ordered-normal-character-1789933352, frames
1326..1389 also passes integrity; inspected contact sheet has no equally obvious
clean-shaven single-frame failure. A short clean run does NOT override user
report or prove correctness. Diagnostic-vs-normal differences include native
preparation, resolution/jitter and capture pose; isolate before attribution.
10s normal timing:396frames/10.113s=39.16renderedfps, no GPU timing records;
not a matched baseline and not a performance acceptance test.

Second NORMAL sequence confirms the defect without diagnostic launch overrides:
`.research/sequences/ordered-normal-repeat-1789933477`, PID47244, consecutive
frames6098..6161. Integrity and fixed-camera guards passed. Inspected contact
sheet `.research/testlogs/ordered-normal-repeat-face.png` shows abrupt beard/head
misalignment at6102,6109,6133,6134,6142,6158 (examples, not exhaustive count).
The first short normal run was a false negative for reproduction, NOT a fix.
This excludes a requirement for matched-sample/native-preparation overrides;
it does not identify the cause or establish native/API/GPU pose equivalence.

Source audit: API bone conversion deep-copies matrices, so host vector lifetime
is not an evident bug. dispatchSkinning uses geometry.numBonesPerVertex, not the
prototype's zero numBonesPerVertex. Retained geometry is committed by the scene
origin API BEFORE csRemixRender flushes uploads then skinning; it is NOT replayed
inside prepareSceneData after that flush. Don't repeat those disproved source
suspicions. DrawCallCache heuristic assignment, dynamic morph/history and native
pose synchronization remain candidates, none established. Existing bounded
skinning probe in rtx_geometry_utils.cpp only samples24-bone MSN body geometry;
it does NOT validate this2-bone dynamic head or1-bone face accessories. Next
useful probe: frame-tag head/accessory matrices/positions and GPU skin output
against native counterparts at failed frames, including membership/retirement.

### Ordered-ray continuation implementation

The resolver now separates the hardware ray from the interval being shaded.
Hardware origin/direction stay unchanged through ordered transparent hits;
TMin advances to nextFloat(last absolute hit T). `resolveRayT` is the previous
absolute hit before a callback and the current absolute hit after it.
`beginResolveSegment` converts hit distance to the current interval BEFORE
unordered particles, material resolution, cone growth, volume attenuation or
accumulated depth use it. The payload shading origin advances along the original
line, but does not become the hardware origin. Barycentric geometry reconstruction
is unchanged. No material opacity, SSS or normal adjustments hide skipped geometry.

Portal continuation explicitly clears `continueOriginalRay`, installs the
teleported origin/direction, subtracts the total absolute distance from the ray
limit and resets the absolute-T bookkeeping. Each new primary/PSR/indirect
resolve initializes the bookkeeping. RayQuery and TraceRay/SER use the same
advance function. Exhausted finite intervals invoke the miss callback rather
than submitting an invalid TMin/TMax range. Direct visibility/SSS secondary-ray
spawn offsets are not changed by this patch. Current payload overhead: float T
and bool per resolver; performance and animation require GPU validation.

107 CPU/source tests pass including close layers at Riverwood coordinates,
distance/cone/volume conservation, portal reset and finite-range exhaustion.
These tests do not establish GPU correctness or visual fidelity.

## 20:21 update: ordered continuation skips nearby facial geometry

User reports beard and head/hair blend flicker too, plus whole head separating
from body. Latest feedback: less frequent, still occurs. Keep ALL unresolved;
the stationary visibility evidence below does not explain the head/body motion.

Production precision correction: GeometryResolverState and
GeometryPSRResolverState retain vec3 directions instead of f16vec3. Synthetic
tests show f16 direction changes a hit point by >.005 units at distance65 and
can cross a layer .002 away. PathState still uses f16 direction. This change
does NOT fix the black brow band and has not passed animation/performance gates.
All102 Python CPU/source tests pass; these are not GPU unit tests.

New GPU diagnostics817 = ray-evaluated vs barycentric-position error, ray-depth,
barycentric-depth;818 = surface/primitive/buffer identities. Exported debug
images are FLOAT16 and force alpha1: do not interpret A as frontHit or indexBuffer,
nor large IDs as exact. Old reports were regenerated without the invalid A field.
Pre-precision-fix817 reconstructed its ray from f16 direction, so it was not an
exact reconstruction of the initial full-float hardware trace.

Stationary PID52796 sequence (all .research/buffers):
face-layers-stationary-Pair-817-1789930603, first-hit-817-1789930620 and
Remix-817-1789930611. Manual face ROI x[670,810),y[310,560), nativeZ<100,
finalZ-nativeZ>5 selects2262 failures. Native depth matches exactly at these
pixels across frozen first/final captures. Final depth error median+11.7683;
first-hit error median-.03499; hit/reconstructed position error median.003906.
Pair817 and Remix-only817 RGB match exactly at ALL selected pixels, excluding
the native-reference GPU draw as the cause of this particular frozen defect.

Post-precision PID4920 sequence face-float-direction-Pair-817-1789931084,
first-hit-817-1789931115, Remix-817-1789931095 still has2449 failing samples,
median depth error+11.5661, first error-.03314. Different pose across launches:
do NOT compare failed counts as a regression/improvement metric. Native-reference
draw again changes none of the failed-pixel diagnostic values. Registered lit
1789931066 forehead control agrees within.00305units (median-.000934), while
band median depth error remains+8.50084. 200426 screenshot still visibly fails.

New819 diagnostic traces from the SAME incoming ray origin/direction, with
TMin=nextFloat(hitDistance), ordinary primary ray mask/cull flags, FORCE_OPAQUE.
It reports next view-depth, nextT-firstT, and next surface; -2 surface sentinel
means repeated primitive, -1 miss. Use ONLY with showFirstGBufferHit=true for
this investigation; it is a nearest-geometric-hit probe, NOT alpha resolution.
Diagnostic output alone does not certify a production continuation algorithm.

PID14148 valid frozen sequence:

- face-next-unshifted-Pair-817-1789931932 (final)
- face-next-unshifted-Remix-817-1789931944 (no native-reference draw)
- face-next-unshifted-first-hit-817-1789931955
- face-next-unshifted-first-hit-818-1789931959
- face-next-unshifted-first-hit-819-1789931964
- final identity Pair818-1789931938; lit1789931914, facing1789931923.

Same manual mask selects1683 failures, final depth error median+12.3976.
All1683 next-ray probes hit geometry, zero misses/repeated primitives. Next-hit
depth-native median-.02635, gap from first median.0108185 (min.001854,
p99.904736). Next surface quantized5616 at1657samples, also the final back-head
surface at1519; first surface quantized11024 at1655. Native depth exactly matches
across first/final/next selected pixels; Pair/Remix-only RGB again identical.
201833/201843/201852 screenshots retained; black band visibly persists.

Strong lead: resolveVertexFinalContinue offsets the barycentric world position
along -Ng by roughly.023 gameunits at |worldY|48155. This exceeds the median
near-layer separation. The unshifted diagnostic finds intervening head geometry
the ordinary resolver skips. Do NOT globally reduce self-hit bias or flip skin
normals to hide this. A robust continuation should preserve the original ray and
advance TMin, or reject alpha hits in traversal. Account for per-segment distance,
cone radius, volumetric/unordered attenuation, portal/direction changes, PSR and
both RayQuery/TraceRay paths; simply editing the macro TMin double-counts distance.

Reproduce reports with tools/remix/CompareFaceLayers.py final first
--remix-only remix --next-hit next --output report. Reports:
.research/testlogs/face-layers-stationary-layers.json,
face-float-direction-layers.json, face-next-unshifted-layers.json.
CompareBrowBuffers report face-float-direction-brow.json; CompareHitBuffers
reports face-layers-hit-consistency.json and face-float-direction-hit-consistency.json.

Rejected moved sequences: brows-first-hit (cameraZ-137.4/yaw-2.3504) and
face-layers-first-hit (cameraZ-160.933). Neither is evidence. The explicit
user-stationary sequence above passed guards. CaptureBrowBuffers restores debug0,
firstHitfalse and unfreezes in finally; it now includes819 in first-hit probes.

Head/body motion source leads remain UNPROVEN: dynamic morph replacement and
history, capture pose synchronization, skin weights/bone palette, static mesh
upload-budget early return. Native dynamic positions are copied under lock;
pure morph replacement retains material and bypasses the new-material budget.
No evidence yet that the static-mesh budget actually stalls the head. Do not
attribute motion to the visibility offset solely because both affect the face.

## Confirmed: black brow band is not just a normal or SSS setting

Earlier checkpoint, before the precision update above. Terrain grazing-normal correction
remains deployed. Skin diffusion leakage, eyebrows and other character defects
are NOT considered fixed.

Diagnostic PID35672, full-resolution/zero-jitter LaunchTest MatchCaptureSamples.
Existing Riverwood Save1, frozen third-person character at camera
(13735,-48155,-158), actual yaw -2.077376127243042, pitch0.

Same-frame native/RTX pair `.research/buffers/brows-fresh-pair-lit-1789928760`
(frame1723, request1789928760) reproduced the visible black band. Screenshots
192600-brows-fresh-pair-before.png, 192608-brows-fresh-pair-after-lit.png and
192615-brows-fresh-pair-after-facing.png all show it. Pair808 companion is
brows-fresh-pair-facing-1789928768. All files are under .research.

`tools/remix/CompareBrowBuffers.py` validates paired identity, matching cameras,
camera sidecar, full-resolution coincident primary rays and buffer extents.
It reports unfiltered manually chosen probes, NOT whole-character parity.
Reproduce with:

```
python tools/remix/CompareBrowBuffers.py .research/buffers/brows-fresh-pair-lit-1789928760 --output .research/testlogs/brows-failed-pair.json
```

Band ROI x[725,770),y[378,386), 360pixels:

- Native depth median63.93568, Remix75.50635; median signed error+11.57602units,
  maximum+13.14435. Native Ns.V median+.82919, RTX-.67813.
- Native/RTX normal angle median114.50976deg. RTX albedo is skin-colored
  (.266862,.167155,.139785), not a black albedo card.
- Noisy/denoised diffuse and specular channel medians allzero; postComposite
  RGB median approximately1e-6. Darkness exists before denoising.

Forehead control x[725,770),y[350,360), 450pixels:

- Signed depth error median-.0018135, range[-.004116,+.000254].
- Native Ns.V median+.89734, RTX+.89176; normal angle median2.0904deg.

Interpretation: Remix resolves a different/deeper surface in the band, consistent
with seeing the inside/back of the head. Do not conceal this by bending/flipping
skin shading normals. Need distinguish missing/displaced geometry, incorrect
BVH/surface mapping, and resolver skipping an intervening surface.

## Isolation and limits

- Frozen191933fullyOpaque5/191936albedo23/191938geometryhash277: thin eyebrow
  geometry exists; no broad black albedo card. Brow texture SRV is resident.
- 192028default/192031transmissionOff/192034scale.1/192037bothSSSOff: black band
  remains with diffusion and transmission disabled. This is a different issue
  from the previously isolated underarm diffusion leakage.
- Later Pair3 front-hit and277geometryhash captures at1928 show opposite hit
  side in band and same head geometry hash. Actor unfrozen between sequences;
  not an exact pixel/pose match to the original failed pair.
- OMM192147/192150/192153 screenshots all lacked band even BEFORE toggling:
  inconclusive. Do not attribute disappearance to OMM.
- Alpha-test toggle193742..193810 retained black band in screenshots. However
  cutout geometry did not visibly disappear, so this does NOT prove an effective
  cutout-removal isolation. Option acceptance alone is insufficient.
- OMM193922..193950 screenshots already lacked band BEFORE toggling again.
  Pair lit1789929562 and off1789929576 nevertheless contain bad raw band pixels
  (and zero radiance) at same registered native pose. Report JSONs brows-omm-on
  and brows-omm-off preserved. Screenshots following Pair are NOT necessarily
  representative of the raw captured frame; inspect raw composite itself.
  No OMM cause established. New script waits2.5s after toggles for future tests.

All options restored to fresh-test defaults (SSS bothtrue, scale1, OMMbindingtrue,
alphaTesttrue), debug0 and freecam disabled. No persisted setting/save writes.

## Source leads, not findings

Host BSDynamicTriShape reads locked dynamic float4 positions, static attributes
and skin influences, concatenates visible NiSkinPartition index buffers, and
applies worldInverse*boneWorld*skinToBone. Head census: MaleHeadNord898verts,
2bones, dynamicType4, MSNtrue, alphaFlags0, skin diffusion imported. Brow:
BrowsMaleHumanoid0188verts/1bone, HairTint, alphaFlags4333, threshold128;
runtime HairCards forces cutout/double-sided. No missing brow texture evidence.

Potential next diagnostics: compare native uploaded head vertices/indices/bone
palette with API data; test raw captured frame versus immediately adjacent frame
without native-reference GPU draws; instrument selected primary hit primitive,
surface index and ray hit position versus reconstructed position. Fast-path
BLAS/surface mapping and dynamic mesh replacement remain unproven candidates.
Avoid broad normal/SSS/material changes without isolating this depth defect.
