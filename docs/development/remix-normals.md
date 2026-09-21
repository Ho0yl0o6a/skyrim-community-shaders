# Native normal transport

## Native skin/grass grazing gate — September21 00:17

User reopened grazing black on the player body and rock grass. Native grass and
model-space normals bypassed getBentNormal unconditionally. They now preserve
authored normals only when dot(normal, viewDirection)>0; other inputs use the
existing correction. No SSS, brightness, geometry, water or valid-normal tuning.

Runtime build2315 succeeded; deployed D3D11 SHA256
65BC214DF08524F7C5552EFDF03086636F317B599E141E561E41FD34C8A5D03C,
backup/symbols20260921-native-normal-facing. Host BB430485 and DXGI unchanged.
130 CPU/source tests pass; gbuffer_debug_psr_raygen_nrc_wboit SPIR-V validates.

Normal PID12692, Riverwood Save1. Before64-frame
.research/sequences/grazing-body-facing-1789945629 has2589–2752 red808 pixels
per full image. After grazing-fixed-facing-warm-1789946131 has0–10, and
grazing-fixed-orbit-1789946197 has0–7 over64 frames each. Threshold is RGB
R>200,G<50,B<50; diagnostic red denotes NdotV<materialEpsilon, not strictly
negative. Whole-frame counts are not material masks. Warm stationary capture
passes integrity and all64 host pose records are unchanged/submitted. Orbit
passes integrity; camera trajectory is asynchronous, not per-frame telemetry.
These are different freezes/poses and NOT pixel-matched native/RTX parity.

Inspected warm frame3905 has clean body silhouette in808; lit frame1774 from
grazing-fixed-lit-1789946082 shows a rendered character/scene. Lit64 integrity
passes, but pose coverage is incomplete: reject its pose comparison. Initial
grazing-fixed-facing-1789946049 contains a black world during startup and is
rejected. Log reports NRC initialization failure at00:14:13; scene subsequently
renders, cause not resolved. No crash observed. Normal RR1, debug0, audit off,
third-person/freeCam false restored. Eye brightness/facial defects remain open;
this is evidence for the facing correction, not full visual acceptance.

## Grazing hemisphere correction — September20 19:15

User reported grazing-angle darkness. Matched native/RTX captures isolated a
normal-correction bug, not an import error, on foreground terrain. The custom
getBentNormal early return only checked whether reflection was above geometry.
Reflection is unchanged by negating its normal, so some backward-facing normals
escaped correction and opaque BSDF evaluation returned zero. The early return
now also requires positive dot(shadingNormal, incidentDirection); the existing
closed-form correction and fallback handle rejected inputs. No brightness,
foliage, grass/MSN bypass or material tuning changes.

Before Pair captures .research/buffers/grazing-pair-facing-1789927404 and
grazing-pair-facing-repeat-1789927409; after grazing-fixed-facing-1789927877 and
grazing-fixed-facing-repeat-1789927881. Exact captured camera and primary pixel
centres agree; full1920x1080 zero-jitter native/RTX same-host-frame pairs.
CompareGrazingBuffers.py reports raw whole-mask statistics and retains diagnostic
depth subsets separately. ROI x>=1000,y>=800 is a current-view probe, not a
material-ID mask or general acceptance gate. Both repetitions show449negative
normals before and zero after in this257600pixel foreground region. All original
449 normals now face the view (.10989 minimum dot). Native normals and depths
at those pixels remain unchanged, and their max absolute depth difference is
.00542gameunits. Native normal disagreement increases as an intentional BSDF
orientation correction; this is NOT a normal-buffer parity pass.

Lit captures190324-grazing-pair-lit /191129-grazing-fixed-lit in .research/captures
show the discrete black foreground patches removed. Scene-wide808 red count
4979 ->1877; followup804 labels1857ofthese ordinarygrass (separate frames),
20other. Geometric-normal debug15 faces the view on all1877positions. Grass
authored-normal behavior is user-accepted and untouched. Skin and other objects
are not declared fixed by this terrain test.

Runtime617DEB8314D9031A6ACF5A833EDBB09835F993884BC426A0EB26B3298AB65DDB,
backup20260920-grazing-hemisphere.99Python tests pass, including six new
float32-reference/source tests; not shader execution tests. Normal gameplay
launch restored after diagnostics. See handover checkpoint for live state.

## User acceptance — 2026-09-20

The user confirmed grass normals are fine and requested moving on. Anisotropic
filtering is also explicitly accepted. Keep the existing 8x anisotropic importer;
do not reopen grass-normal work based on the diagnostic residuals below. These
measurements remain evidence, not a claim of exact buffer parity. Character
rendering and animation are the next focus.

## Coincident primary samples (September20 10:55)

`LaunchTest.ps1 -MatchCaptureSamples` enables startup native preparation,
full-resolution DLSS profile5, and diagnostic zero jitter in both renderers.
Runtime `RtCamera::calcPixelJitter` checks CS_REMIX_TEST and
CS_REMIX_MATCH_CAPTURE_SAMPLES. Native zeroing occurs ONLY while the audited
world cache constructor at Main::Draw RVA0x656ee4 calls RVA0x101c8f0. Its original
camera/useJitter/alternate arguments are preserved; State+0x44..0x53 is saved,
zeroed for construction and immediately restored. Default launches install no
extra native cache hook. Version1.7.99 and call target are checked before patching.

Read-only Ghidra exports: `.research/native-camera-upsert-audit.json` and
`native-jitter-cache-audit.json`. The constructor locates the camera/useJitter
record and invokes RVA0x101e280, which uses either State+0x44/+0x48 or+0x4c/+0x50
for jitter. The latter pair is NOT CommonLib's frameCount/flags on1.7.99.
Earlier zeroing at the later SetCameraData hook, including calling the global
Upscaling UpdateCameraData relocation, left native jitter nonzero. Those attempts
FAILED the matching gate and have been replaced, not credited as fixes.

PID53496, same Riverwood save and bankcamera, GrassLighting temporarily enabled:
`grass-bank-pair-cache-construction-1789898111` (host/runtime48129) and
`grass-bank-pair-cache-construction-repeat-1789898151` (48926), both under
`.research/buffers`,23artifacts each. Both native/RTX resolutions1920x1080,
native projection _31/_32=0, uploaded RTX pixelJitter=[0,0]. The analytic
projection test finds maximum displacement2.486e-5 native pixels horizontally
and6.851e-6 vertically. Both pass `--require-matched-samples`. This validates
geometric primary sample centres, NOT texture footprints, alpha coverage,
post-refraction boundaries, normal/albedo parity or jitter in ordinary play.

Reports: `grass-bank-pair-cache-construction-registration.json` and
`grass-bank-pair-cache-construction-registration-repeat.json`. Fixed RTX grass
masks150204/150231 pixels: normal p50/p90=.099/69.931deg and.099/69.986deg.
Depth-relative<1% subsets135172/135128 still have p90=63.539/63.556deg.
No parity pass. Diagnostic absolute-depth<2 subsets109885/109442 have normal
p90=.151/.152deg, but p99~69deg; excluding difficult pixels cannot certify
correctness. Over30deg pixels29989/29903 show large signed depth differences
(first p10/p90=-74.57/+162.14 units); only~67.4% have native gloss1. This supports
investigating visible-surface/alpha coverage before further normal transforms.
It does not establish that all remaining normal transport is correct.

Independent rock check: native raw depth minus reprojected RTX geometric depth
is approximately -34 D24 steps (p10/p90=-35/-33), median native depth4101.915.
Native raster bias/viewport must be inspected before interpreting that as an
actual geometry displacement. No fitted bias correction is applied to reports.

## Same-host-frame reference capture

**Experimental; populated pairs now work with startup native preparation.**
September20 initial live tests resumed native rendering into a stale shadow
batch and crashed. Keeping that routine skipped avoided the crash but produced
entirely cleared native buffers in two attempts. Those pairs remain unusable.
Launching with `LaunchTest.ps1 -NativePreparation` preserves native CPU batch
preparation/cleanup from startup while independent low-level GPU suppression
continues on ordinary frames. Two populated pairs succeeded in PID6116 at
frames2050 and5981. This does not establish safe general native resumption,
all-traversal stability, or rendering parity. The diagnostic adds CPU cost.

`CaptureBuffers.ps1 -Mode Pair -Label <label> -DebugView 804` requests one native
world frame followed by the normal Remix submission in the same game frame.
This is diagnostic only: normal rendering continues to suppress native world
draws. The existing one-way cs.nativeReference guard is unchanged. The new
cs.captureBufferPair request is accepted only in a loaded, suppressed Remix
scene launched with -NativePreparation; it reserves the shared capture slot
through both halves. The default process rejects it. A request that
arrives after the world latch waits for the next frame. An unfinished active
request is discarded at the next world latch, not silently paired with it.

Native albedo/normals/depth are saved at the end-deferred/pre-water boundary.
The startup preparation mode runs the native shadow/world CPU routines rather
than skipping their coupled preparation/cleanup. Independent native draws and
dispatches remain suppressed on normal frames; the capture frame allows them.
The CS direct-compute deferred compositor explicitly skips normal suppressed
frames too. These raw captures do not establish native shaded-image parity.
RTX capture is queued only if the native saves succeeded in that same host
frame. Both metadata files carry request/PID/frame/pairedCapture/nativePreparation and native
Grass Lighting load state. The harness rejects missing or mismatched halves
and restores debug0/capturefalse. The comparison accepts the same pair directory
for both inputs and validates its identity; sameHostFramePair is evidence of host
frame pairing, NOT equal ray footprints or a rendering-fidelity verdict.

For Grass Lighting reference normals, explicitly enable GrassLighting through
DevBench before testing and restore its prior loaded state afterward. Load-state
metadata does not prove shader compilation/selection: verify native buffer gloss
and normal outputs too. Native/RTX resolution, TAA jitter, texture derivatives,
alpha coverage and pre-water versus RTX PSR boundaries can still differ.

Successful captures: `.research/buffers/grass-bank-pair-prepared-1789893796`
and `grass-bank-pair-prepared-repeat-1789893908`, with comparison reports beside
them. Both have actual nonclear native albedo/normals and varying depth. Ordinary
grass depth-matched normal p50/p90 is1.232/81.551deg then0.599/70.278deg. Large
tails persist even with same-host-frame pairing. Continuous-normal subsets are
diagnostics, not replacement pass gates. Actual RTX jitter/sample registration,
alpha coverage and overlapping blades remain unresolved; do not attribute all
remaining errors to different capture times or claim normal parity.

## Sampling-registration diagnostic (September20 10:00)

### Uploaded-camera registration (10:17)

Runtime captures now include `gbufferLinearZ_<stamp>.dds.camera.json`, schema1,
source uploaded-raytrace-args, runtime/rng frame, depth filename, resolution,
extent, pixel jitter, camera flags and six column-major float32 camera matrices.
These are read from RaytracingOutput::m_raytraceArgs.camera at the G-buffer
capture boundary; no per-frame metadata work or readback in normal rendering.
`gbufferWorldNormals` is now captured at the same pre-composite boundary too.
The original final worldNormals is retained. First live pair had zero differing
packed normal values between these boundaries, so stage mutation wasn't the
cause in that sample. CaptureBuffers requires/copies the depth-associated sidecar.

Native metadata additionally records shadowOrigin. CameraBuffers.py restores
native world-view translation using the same double dot products/float result
as RestoreWorldViewTranslation, checks full view equality, projection/inverse
consistency and resolutions, then projects each RTX pixel ray into native pixel
coordinates. JSON matrices must first round-trip through float32 before double
analysis: the nine-digit serialization of19988.578125 is19988.5781, not a real
camera displacement. No view-equality gate was relaxed. Tests cover centre and
resolution conventions, jitter signs, rebase/serialization, invalid matrices and
different views. Full reader/registration test suite29/29 passes.

PID52084 same-frame pairs (both runtime and host frame IDs agree in these tests):
`grass-bank-pair-ray-origin-1789895624`, frame876; repeat1789895761, frame5847.
Both23artifacts, 1280x720 RTX/1920x1080 native, same bank camera/GrassLightingtrue.
Analytic shifts respectively[-.4296875,.8981481] and[.609375,.4722222] native px.
First fixed67597grass pixels normal p90 improves92.459->72.881deg; depth-matched
p90 improves81.688->66.004deg, median1.249->.269deg. Repeat67499fixed pixels:
registered p50/p90=.331/73.116deg; 61016depth-matched p50/p90=.274/66.059deg.
Large tails persist despite measured camera registration; normals aren't solved.
Nearest native sampling still quantizes to different subpixel rays and alpha
coverage. Matching actual sample locations (not another inferred shift), material
coverage/derivatives and residual transport errors remain to be investigated.
Reports: grass-bank-pair-ray-origin-registration.json and -registration-repeat.json.
The first new-runtime/old-host capture1789895380 lacks shadowOrigin and correctly
fails analytic view matching; do not retrofit guessed origins into evidence.

`ProbeBufferRegistration.py` fits ONE global translation from the fixed bank
rock region's depth alone (bilinear depth, +/-2 native pixels,1/8pixel steps),
then evaluates held-out grass/ground/stump and rock normals with nearest raw
samples. No normal values enter the fit. All calibration pixels are fixed;
invalid depth aborts rather than changing candidate masks. Reports retain all
fixed grass pixels AND separate depth-matched subsets. The tool checks paired
identity, Grass Lighting enabled, exact bank camera, and supported dimensions.
This is an inferred sampling diagnostic, not measured jitter or a parity gate.

For prepared pairs2050/5981, shifts[-.5,.5]/[.625,.25] reduce depth-matched grass
normal p90 from81.550/70.287 to66.279/66.041deg. Fixed grass p90 likewise drops
92.226/84.085 to75.126/73.082deg, so improvement is not just subset selection.
Large residuals persist; this does not establish correct grass normals.

Two further actual same-host-frame pairs at1920x1080 native AND RTX, launched
with -NativePreparation -DlssProfile5, also retain large tails. PID18100 frames
1570/2917, captures grass-bank-pair-native-resolution-1789894649 and
grass-bank-pair-native-resolution-repeat-1789894714. Each21artifacts. First fit
[.125,-.5] changes NO nearest native pixels (documented half-pixel tie), so its
depth-matched grass p90 stays78.216deg. Repeat fit[1,-.5] changes every sampled
native pixel, grass p90 changes74.090->73.667deg; fixed grass p90 worsens slightly
86.464->86.935deg. Native-resolution alone is not a solution; inferred shift is
not sufficient proof of correspondence. Do not call this a normal-fidelity pass.

Reports: grass-bank-pair-registration.json, -registration-repeat.json,
grass-bank-pair-native-resolution-registration.json and -registration-repeat.json
under .research/buffers. Five synthetic registration tests plus the existing
19 reader/comparison tests pass. Need actual RTX ray camera/jitter/frame metadata
for analytic sample registration, then coverage/derivative/normal investigations.
Current host shadow matrices are explicitly NOT the RTX camera. Runtime uses
RtCamera::calcPixelJitter with Halton sequence; do not infer runtime frame IDs
from host frame counts or capture filename timestamps.

## Complex grass atlas normals (2026-09-20)

When the native complex marker is detected, grass shading samples RGB normals
from the lower half of the SAME resident albedo SRV and sampler. Diffuse UV.y and
gradients have already been halved; the normal read adds0.5 to UV.y without
halving again. Decode RGB*2-1, without colour gamma or generic normal intensity.
The final combined normal is normalized. No CPU readback or texture bake.

The shared nativeGrassMapNormal reproduces GrassLighting::CalculateTBN using
the existing raw dP/du,dP/dv triangle frame. T=cross(rawB,N), B=cross(N,rawT),
both multiplied by sign(dot(cross(rawT,rawB),view)). Normalize both by the SAME
maximum length. This retains UV handedness and relative scale; it is not the
orthonormal Remix basis. The sign accounts for CS's negative world-position
input and screen-down Y convention. The screen Jacobian's common determinant
magnitude cancels under that scale. Degenerate/nonfinite frames or mapped
vectors return the oriented authored normal. It does not reproduce coarse
quad finite-difference errors, mip/footprint differences or far-detail cutoff.

test_native_foliage compares the production function against the CS derivative
formula with non-axis-aligned edges, skewed UVs, mirrored UVs, both faces, sphere
normals, common-scale invariance, neutral maps and invalid/zero cases. CPU pass
does not prove GPU atlas sampling. Native specColor.w/gloss response and
GRASS_OPTIMIZATIONS far-detail selection are not implemented by this change.

Debug804 Native Grass Atlas Kind writes green=ordinary, blue=complex,
black=non-grass at actual hits. Riverwood bank capture1789890603 had ZERO complex
pixels, so the new positive branch remains unqualified in-game. Ordinary-grass
median normal error~0.3deg but p90~66deg persists, including eroded mask interiors.
Do not attribute the changing mixed-region metrics to this inactive branch.

## Grass authored normal transport (2026-09-20)

Grass with VF_NORMAL now preserves its decoded authored normal instead of always
using the face-normal proxy. This follows CS Grass Lighting, not vanilla's
instance-up lighting convention. Native mesh flag4 marks GPU-expanded grass;
flag6 additionally requests the fallback for assets without normals. Existing
GPU expansion transforms the normal alongside the animated position.

The subsequent orientation change carries kEffectLighting as foliage flag64
(NATIVE_FOLIAGE_SPHERE_NORMAL). Native grass material shading now uses the
interpolated authored normal before per-vertex hemisphere correction, flipped
only for backfaces unless sphere-normal is set. The non-TBN ordinary-vertex path
preserves that normal in modelNormalZ and the unflipped triangle direction in
modelNormalX; grass alone consumes them as this orientation pair. MSN and
authored TBN retain their separate meanings. The geometric normal used for ray
offsets still follows Remix's correction; the native grass shading normal is
not bent into it. Missing-normal assets retain the flat fallback.

Complex-atlas GPU qualification remains incomplete. The later V2 native grass
matrix path below corrects placement-only scaling and premature per-vertex
normalization; that correction does not establish full pixel parity.
Sphere-normal import/sign/packing have CPU coverage, but the sampled Riverwood
assets have no sphere flag, so that branch lacks in-game coverage. The ordinary
orientation change did NOT resolve the mixed-grass upper-tail error; see the
08:38 handover. Do not call this normal parity.
Inspector useFaceNormals and CheckFoliageRuntime's optional ExpectedGrassNormals
assertion identify the imported source only, not the resulting GPU shading normal.

## Authored tangent basis (2026-09-20)

Skyrim Lighting VS constructs tangent as `(Position.w, Normal.w * 2 - 1,
Bitangent.w * 2 - 1)`, bitangent as `Bitangent.xyz * 2 - 1`, and normal as
`Normal.xyz * 2 - 1`. It transforms and normalizes each column at the vertex,
then independently normalizes the interpolated columns in the pixel shader.
Reconstructing T/B from a triangle's UV derivatives is not equivalent.

`RemixTangentFrame::Decode` preserves these channels, including authored
handedness. The importer calls `csRemixCreateMeshTBNV2` for ordinary meshes and
landscape with complete attributes. This single-surface extension accepts six floats per vertex
(T then B), validates the count and finite components, and synchronously copies
them into the same nine-vector-component layout used by model-space skinning.
For this layout the order is N/T/B; position/UV/color remain intact. Flag8 adds
the original two packed landscape-weight words after color (68-byte stride).
Other meshes retain the 60-byte stride. The V1 export remains a flag0 wrapper.
The host
vectors may be released when the API call returns because deferred uploads own
their bytes.

`nativeTangentFrame` is propagated through RasterGeometry, RaytraceGeometry and
RtSurface, packed at surface flags0 bit2. The GPU skinning dispatch enables its
existing three-vector path (legacy argument name `modelSpaceNormals`) for either
nine-float layout. Static and skinned API buffers use the GPU-friendly interleaved
copy path, preserving all 60/68 bytes per vertex. A future re-interleaving path must
also preserve all three vectors; generic interleaving supports only one normal.

The hit shader transforms/normalizes per-vertex columns, interpolates, then
normalizes the columns. It supplies authored T/B to the material basis and
resolves the sampled RGB normal against all three authored columns. Backface
orientation follows the existing Remix geometric hemisphere convention.
No new per-frame CPU skinning or readbacks.

This is not a claim of native normal-buffer parity. Remaining qualification:

- Water, grass, effect meshes and model-space maps retain their separate paths.
- Terrain combines authored basis with its six-layer material path. Overlay,
  distant blending and native image parity remain unqualified.
- Native TREE_ANIM camera-facing normal manipulation and distance-dependent
  double-sided normal correction are not reproduced by this change.
- Remix's geometric/shading normal bending, optional smoothing, nonuniform
  instance transforms, and second normal maps require separate verification.
- Texture transforms, mip selection, and image-space reconstruction can differ
  even when the transported basis is correct.

Tests: `tools/remix/CheckTangentFrame.cpp` exercises packed channel order and
handedness. Runtime `test_skinning_basis` evaluates the shared CPU/GPU function
with arbitrary nonorthogonal, mirrored N/T/B under blended bones, in addition
to its existing axis/stride/guard tests. These are CPU equation checks, not
GPU execution, native image comparison, or a full character-material test.
`test_native_basis_vertex` exercises the production packing function at both
strides, two consecutive vertices, MSN identity, mirrored authored basis,
position/UV/color, unnormalized terrain weights and surrounding byte guards.
# Grass placement normal matrix and interpolation — 2026-09-20

`RunGrass.hlsl` transforms decoded normals with the original three instance rows.
The per-axis `1 + variation * ScaleMask` factors affect positions only. The VS
does not apply World or normalize its normal output; the pixel shader normalizes
after interpolation. Position transforms therefore cannot substitute for normal
transforms, even when most placements happen to be near-uniform.

CS now calls `csRemixCreateGrassInstanceSetV2` with separate row-major 3x3 native
normal matrices, copied before size variation and World composition. Original
V1 remains exported; V2 is required by the updated host. Immutable grass records
are 128 bytes: original80-byte placement plus three16-byte normal rows. Word19
selects native unnormalized VS output, words20/24/28 start normal rows. The
non-grass placement record stays80 bytes. Normal rows enter content identity.
The GPU upload is once per rebuilt placement set, not per frame; wind and position
equations are untouched.

The grass material's saved authored normal is now interpolated from raw vertex
normals before normalization. Generic geometric hemisphere/ray-offset handling
and model-space/native-TBN paths remain separate. CPU tests cover row order,
nonuniform native matrices and detection of premature normalization, plus GPU
record offsets. `LaunchTest.ps1 -GrassProbe` enables the existing bounded two
readbacks, now also comparing actual GPU normal outputs against CPU row products.
Its alpha check compares preserved source alpha rather than assuming255.

Build and live results must be recorded in remix-handover.md; source changes and
CPU tests alone do not establish native pixel correspondence or visual fidelity.
