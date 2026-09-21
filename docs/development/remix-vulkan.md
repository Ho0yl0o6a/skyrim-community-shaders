# RTX Remix / Vulkan integration

Work branch: `codex/remix-vulkan`.

Latest focused checkpoints: [Riverwood performance audit](remix-performance.md),
[native render boundary audit](remix-native-render-audit.md),
[six-layer landscape](remix-landscape.md), [tree atlas/placements](remix-trees.md),
and [native water import](remix-water.md).

The performance audit supersedes every frame-rate number below: the benchmark is
now the required 1920x1080, and the earlier conclusions drawn from A/B testing
Remix options are unreliable, because options owned by the graphics/DLSS presets
silently ignore anything set through the config API (see that note).
The chronological notes below include superseded limitations and measurements;
consult the focused checkpoints for their latest validation. The full goal is open.

Completion requirements include correct LOD, water, trees, and grass. All world
rendering and post-processing must belong to Remix. Only game menus, loading
screens, 3D menu objects, and UI may use the native renderer. The diagnostic
triangle and the current late-UI injection path are not the final menu policy.

Skyrim diffuse assets are sRGB-authored despite UNORM/linear format labels.
The current fork's opaque material shader already applies `gammaToLinear` to
UNORM diffuse samples; sRGB-format views bypass that software decode to avoid
double conversion. Do not apply an additional decode blindly. Verify the
material flags and final albedo separately from exposure/tonemapping.

## Current checkpoint

This is an opt-in API smoke test, **not a completed path-traced Skyrim renderer**.
Baseline Riverwood was visually validated before deployment. The Remix API triangle
is now visible in Skyrim's own window, including the in-game HUD after loading
Riverwood. Normal path-traced/emissive output also works after explicitly setting
the opaque material alpha comparison to ALWAYS (7); zero means NEVER, which rejects
all ray hits even though the raw-albedo debug view can show candidate surfaces.
The depth diagnostic reconstructs Riverwood silhouettes and surface normals through
Remix (about 27,720 vertices / 36,500 triangles). It has coarse edge gaps and only a
test material. Real static Riverwood geometry now renders in the normals diagnostic
(about 1,600 visible instances). The `BSBatchRenderer` world-pass bypass was tested
with this geometry: the Remix scene, HUD, and Survival dialog remain visible.
Textured path-traced static geometry is now visible with working alpha cutouts,
shadows, and physical-atmosphere sky. Skinned full-size trees now render. Dynamic
face positions use their separate CPU stream; bone hashes update the Remix BLAS.
Tangent-space native XYZ normal maps are imported and decoded explicitly. Model-space
character normals remain unsupported (not incorrectly fed to the tangent decoder).
Water, complete grass/LOD coverage, alpha effects, and full animation validation
remain open. The latest target also requires 60 FPS at 1080p; the earlier complete
tree test ran only 6–8 FPS. This is not goal completion.

Grass now submits loaded instance groups rather than the last raster pass's
`isVisible` subset (about 33,600 placements at the Riverwood spawn and 40,700 near
the eastern houses in the latest test). It uses unmodified native grass UVs,
`BSGrassShaderProperty::clampMode`, and the shader's captured `ScaleMask` with each
instance's size variation. The clamp fix removed repeated triangular texture tips
in the close-up comparison. API transform arrays now have shared lifetime with
retained Remix surfaces. Camera rebasing belongs to the outer batch transform so
previous-frame transforms remain meaningful; movement/flicker validation is still
pending. Exact native wind deformation for grass and trees remains a completion
requirement, not an implemented feature of the grass instance path.

The corrected native GPU gate suppresses roughly 3,800 world/post-processing
draws or dispatches per frame in Riverwood. Do not skip the whole engine
`Main_RenderShadowMaps` function: it also prepares CPU scene lists. Doing so left
only the independently traversed trees visible. Keep CPU setup and gate GPU work.

LOD meshes now use the visible `BSSubIndexTriShape` index ranges, merged to avoid
overlapping aggregate/child triangles. Landscape LOD reproduces the native
`LodLandscape::AdjustLodLandscapeVertexPositionMS` lowering inside the shader
manager's loaded range, invalidating the upload when that range changes. At the
Riverwood spawn the nearest block lowered 479 of 1,068 vertices; neighboring
blocks outside the high-detail rectangle remained unchanged. Six-layer terrain
material blending is still missing and must not be confused with this geometry fix.

API mesh buffers now use device-local allocations and command-stream-ordered
staging uploads, including skin weights/indices. The same Riverwood view still
measures approximately 7–8 FPS after this change, with 100% reported GPU activity;
this did not establish a performance improvement. GPU-stage timing is needed to
locate the dominant cost. Output remains 1680x1050 during these diagnostics, not
the required final 1920x1080 benchmark.

`CS_REMIX_TEST=1` selects `CommunityShaders/bin/Remix/dxvk_d3d11.dll`
and `dxvk_dxgi.dll`. Normal launches continue using the existing runtime.
The runtime exposes `remixapi_InitializeLibrary`, `csRemixRender`, and the custom
`csRemixCreateMaterialD3D11V3` extension for importing native Vulkan-backed diffuse
and tangent-space normal SRVs with Skyrim's wrap/clamp mode. Native images need nonzero image hashes to
avoid aliasing in Remix's material cache. The extension retains Vulkan image views;
CS must not retain Skyrim's SRV wrapper across engine texture eviction.
CS registers its existing D3D11-on-Vulkan device; it never calls the standalone
Startup function or creates a D3D9 device. The temporary diagnostic submits an
emissive green triangle using CreateMaterial, CreateMesh, SetupCamera and
DrawInstance, followed by the explicit render extension immediately before the
first blended framebuffer draw inside UI rendering. The runtime must re-emit
cached D3D11 state after injecting RTX so the subsequent UI draws remain valid.

DevBench `communityshaders.remix` accepts `name` and string `value` for `rtx.*`
runtime options. `cs.depth=true` enables the experimental pre-water depth mesh
(8-pixel sampling, CPU readback, view-space reconstruction). This is a diagnostic,
not a replacement for scene extraction or evidence of vanilla-world suppression.
`cs.scene=true` selects mesh submission; `cs.suppressWorld=true` bypasses vanilla
world batch passes while still collecting meshes. A native GPU-command gate now
covers the shadow/world/post-processing interval, ending before UI rendering;
menu/3D-preview behavior still needs full regression testing. These switches
are process-local and default off. Landscape blending and full animation/material
coverage remain incomplete. Native CS world features are disabled only for the
test process, preserving Remote Control and Performance Overlay.
For the current exterior lighting test also set `rtx.zUp=true`, `rtx.skyMode=1`,
`rtx.enableRayReconstruction=false`, and `rtx.debugView.debugViewIdx=0`. Native
normal textures require `rtx.opaqueMaterial.normalMapsAreXYZ=true`. Requested
diffuse brightening is `rtx.opaqueMaterial.albedoScale=1.53846153846`, applied after
gamma decoding. The fork's default hybrid sky expects a
Titanfall skybox and is unsuitable for this API-only host.

`ConfigureRunningTest.ps1` sets eye-adaptation speed to `1.0` (upstream default
`5.0`) to reduce fast exposure pumping. This changes the response time, not the
albedo conversion or exposure range. Final exposure tuning must follow the
weather/sun/game-light integration; the current illumination is still a test.

Set process-local `CS_REMIX_GPU_TIMING=1` before launching to collect asynchronous
Vulkan timestamps every 30 frames. `[CSRemix.GPU]` records whole-frame stages,
G-buffer/RTXDI/integration, and direct/indirect integration in milliseconds.
The queries never flush or wait for the GPU. The inherited surface-coverage
shader atomics now follow `rtx.logSurfaceCoverage`, rather than running even
when diagnostic readback is disabled.

`tools/remix/MeasureRunningTest.ps1 -Seconds 10` measures the running process's
frame counter and summarizes newly appended GPU timestamps. It does not change
settings and explicitly does not certify resolution or visual correctness.
The September 11 22:14 Riverwood test after gating ordinary ray-hit coverage
atomics measured 15.48 FPS, 63.609 ms median Remix GPU time, 5.881 ms G-buffer,
and 37.138 ms indirect integration (five GPU samples). The preceding build was
about 9 FPS with 61–65 ms G-buffer time. These are diagnostic spawn-view samples,
not a controlled benchmark or the required 60 FPS at 1080p qualification.
Scene/HUD capture: game `screenshots/CS_2026-09-11_22-14-47_118.png`.
Output remains 1680x1050. Character model-space normals, terrain blending,
weather/game lights, complete alpha coverage and wind remain open requirements.

### Native model-space normal path

The host extension `csRemixCreateMeshMSN` keeps the standard Remix mesh ABI
unchanged. It converts marked meshes to a 60-byte interleaved vertex containing
position, three orientation columns, UV and color. The orientation starts as the
identity and skinning writes normalized blended bone-matrix columns, matching
Skyrim's `MODELSPACENORMALS` vertex shader. At a ray hit these are interpolated,
transformed into world space, and applied to `normalTexture.xzy * 2 - 1`.
The ordinary tangent-space path and geometric normals remain separate. Smooth
normal generation must not overwrite this orientation stream. Both DLLs must
be deployed together; CS reports whether the extension is available.

Both builds succeeded and were deployed for the September 11 22:27 test.
CS logged the extension as available and uploaded marked heads, hands, bodies
and landscape LOD meshes. `screenshots/CS_2026-09-11_22-28-48_917.png` shows
Alvor's face and arms in shading-normal debug view with detailed, smooth normals.
This is limited visual evidence, not full animated-character qualification.
The later stationary-camera pair (`22-32-13_630` full color and `22-32-14_319`
normal debug) is **not a valid matched comparison**: Alvor is absent from the
first image and present in the second. Investigate capture/submission continuity
before treating these as an A/B test. Debug view was reset to zero and freecam
was confirmed off after capture.

Character material conversion now includes a deferred-compute bake of the native
FaceGen tint/detail and RGB skin-tint equations. It preserves authored gamma space
until Remix's final decode; floating-point output avoids clipping the composition.
Hair preserves its vertex-green tint mask, and the native alpha-test/blend factors
are submitted through InstanceInfoBlendEXT. Eye-specific shading and native
specular inputs are still incomplete. These changes require visual qualification
of each facial feature, not just a successful bake log.

The first bake-cache implementation crashed at 22:58:02 while evicting retained
source COM resources. Dump `.research/remix-crash-225802.dmp` was preserved.
The original deployed PDB had already been superseded; attribution used unique
unchanged instruction-byte matches (MapCrashCode.py) to identify the resource-array
and CacheEntry move-assignment frames, not unverified mismatched-PDB stack names.
The source-resource cache was removed. Position-only facial morphs now retain the
owned Remix material, avoiding unnecessary texture rebakes. The corrected build
was deployed at 23:06 with matching CS symbols preserved under
`.research/deployed-symbols/20260911-character-grass`. Initial Riverwood rendering
passed; a long movement/streaming stability run remains necessary.

Vanilla RunGrass does not use grass mesh normals. The current Remix approximation
uses the instance's +Z direction for shading, with no separate normal map and no
vertex-stage backface flip. The native mesh extension carries the no-flip flag independently
of model-space normal maps. The 23:06 log confirms the instance-up path for grass
batches. This is a lighting surrogate, not a claim of exact vanilla BRDF matching.

The September 12 black-grass report exposed remaining oriented-surface assumptions:
the opaque material bent that direction again, diffuse evaluation rejected negative
view-dot-normal, and RTXDI rejected light directions behind the physical triangle.
An upward lighting proxy is not a blade's geometric hemisphere, so those tests can
black out blades above eye height or their backfaces. The new grass-only interaction
flag (bit 2, preserved by the existing 6-bit G-buffer encoding) uses a view-independent
Lambert response about instance-up, with matched cosine sampling/PDF and no specular
lobe. It bypasses the material bend and the triangle-hemisphere light rejection only
for grass. Actual triangle normals remain available for intersections; direct,
RTXDI and indirect rays offset onto the sampled side of the physical blade to
avoid immediately hitting the originating blade again. No medium state is changed;
alpha testing, visibility rays, albedo and exposure are unchanged. This is a
vanilla-inspired diffuse proxy, not an exact physical foliage model. Runtime and CS
builds passed and were deployed for the 00:14:45 test (PID 36140), with matching
artifacts in `.research/deployed-symbols/20260912-grass-proxy`. The previous runtime
is recoverable from `.research/deployment-backups/20260912-grass-proxy`.
Viewed `CS_2026-09-12_00-17-28_746.png` at the reported northern Riverwood position:
the large formerly black grass patches now receive light, with HUD and shadows
intact. `00-18-08_327` confirms stable upward grass directions in normal debug.
Thin grass is still visibly soft/pale; coverage, denoising and native vertex/instance
color modulation remain to be investigated. This is improvement, not finished grass
appearance. The near-matched camera has small pitch/yaw differences because DevBench
freecam drive/get conventions differ; these are not pixel-exact A/B images. Debug
was restored to zero and freecam was confirmed off, in first person, after testing.

Diagnostic captures `23-58-46_555` (full), `23-59-14_394` (albedo) and
`23-59-39_349` (shading normals) show the reported northern Riverwood grass view.
The raw-albedo capture `00-00-08_169` was taken after camera movement and must not
be treated as a matched pixel comparison. Debug view was restored to zero.

**Architecture requirement added by the user:** rendering suppression must move
to independently reverse-engineered game-code hooks, not graphics-API hooks or
existing CS hooks. Scene ownership must also stop depending on raster culling.
The September 11 23:29 build replaces the GPU gate with independently audited
game-code draw/compute and UI boundaries; no D3D11 draw/dispatch suppression hooks
remain. Riverwood, shading-normal debug, HUD, a modal dialog and the inventory
panel were visually checked. The subsequent retained-scene implementation is
described below; its change detection and unsupported geometry paths still do
NOT fully satisfy the scene requirements. Performance
is still around 15–16 FPS at 1680x1050, not the requested 60 FPS at 1920x1080.
Exact findings, captures and remaining gates are in `remix-native-render-audit.md`.

Triangle + Riverwood HUD capture: game `screenshots/CS_2026-09-11_19-45-51_361.png`.
Depth silhouette capture: game `screenshots/CS_2026-09-11_19-51-28_304.png`.
The conflicting game-root DLSS-to-FSR `nvngx.dll` was moved with user approval to
`.research/deployment-backups/20260911-191840-992/dlssg-to-fsr3-nvngx.dll`;
it was not a Remix component. That single file is recoverable; no other unowned
game files were deleted.

Static BSTriShape buffers and transforms are now discovered from loaded scene
roots, not the BSBatchRenderer pass (following CreationEngineRaytracing's Mesh and
Adapter buffer layout). The old generic render-pass capture call is removed;
grass setup currently supplies only ScaleMask metadata. Skyrim positions are float32, index buffers uint16, stored
stride is `(vertexDesc & 15) * 4`. Use the main camera view/projection plus posAdjust
for relative-world transforms. Do not treat the depth diagnostic as goal completion.

### Retained scene prototype (September 11, 23:44)

`RemixSceneGraph::Gather` traverses the world root, attached cell roots, loaded
reference roots, and player roots while retaining NiPointer ownership. It does
not filter AppCulled or grass-group isVisible. Explicit disabled/deleted references
and mutually exclusive NiSwitchNode states remain excluded. Meshes and instances
persist until real detachment; unchanged transforms/bones/placements do not replace
the retained instance. API placement arrays are cached in absolute coordinates,
with camera rebasing applied to the batch transform. Native API/runtime per-frame
placement uploads still need separate change tracking.

The initial test retained about 5,920 instances and 54,147 grass/tree placements;
the loaded census included about 1,416 AppCulled geometries. Paused-scene samples
reported zero changed instances. Viewed fixed-position captures
`CS_2026-09-11_23-48-17_672.png` (opposite heading) and
`CS_2026-09-11_23-50-09_452.png` (returned heading) render both directions with HUD.
Counts and screenshots are not exhaustive proof of culling independence.

Retaining the player's third-person body exposed first-person head occlusion.
The correction assigns the third-person-player category only in first person,
preserving secondary/shadow participation while excluding primary world rays.
Separate first-person geometry is identified but still pending correct projection.
The 23:54 body-mask build was tested from a normal first-person camera (freecam off);
viewed capture `CS_2026-09-11_23-55-28_159.png` shows Riverwood and HUD without head
occlusion. This does not establish correct first-person weapon/hand projection.
Grass ScaleMask fallback, material invalidation,
three rejected fish mesh uploads, effects/water/particles and streaming tests remain
open. No full-scene or performance completion is claimed.

The following CS build additionally snapshots instance-buffer bytes, detects
in-place edits (rather than treating COM identity as a revision), and publishes
shared immutable placement arrays only when groups/transforms/scale change.
Removed groups are retired from that cache. CPU-backed sources are compared
without GPU synchronization; mutable GPU-only sources require readback until a
reliable native revision mechanism is available. Rebuild/readback counters were
added for live verification. The 00:14:45 deployment includes this change. Its
paused scene sample at 00:16:09 retained 54,147 placements with zero placement
rebuilds and zero GPU source readbacks that frame. Mutable-buffer edit/streaming
stress coverage remains open.

### Native grass color inputs (September 12, 00:22)

The CS build imports native vertex RGB (RGBA source to Remix's BGRA vertex
layout) and enables texture/vertex-color modulation for grass. Vertex alpha stays
opaque: RunGrass uses the source alpha as a wind weight, not cutout opacity.
The build passed and was deployed at 00:22:48 (PID 18284); matching CS symbols are
in `.research/deployed-symbols/20260912-grass-vertex-color`. Runtime is unchanged
from `20260912-grass-proxy`. Native vertex components range as low as 34/255 in
sampled meshes, so replacing all vertex RGB with white was not faithful.

Diagnostics also prove that the missing `InstanceData1.w` is significant:
SnowGrass02 groups have mean brightness about 0.65–0.80 (individual values as low
as 0.38), and FieldGrassFlowers17 includes values near 0.29. This multiplier was
not applied in the vertex-color-only build; no average/global brightness
adjustment was substituted. Capture `CS_2026-09-12_00-28-22_699.png` confirms the
large black patches remain improved, but thin grass still looks pale/soft. This
is a near-matched view, not a pixel-exact A/B. Camera control was restored.

The subsequent implementation adds `csRemixCreateInstanceSet`,
`csRemixDrawInstanceSet`, and `csRemixDestroyInstanceSet`. Creation copies immutable
absolute transforms and optional native brightness once. Draw submits a retained
handle; queued draws and retained surfaces own the snapshot even after handle
retirement. A render-stream-owned GPU buffer is uploaded once per revision. CS
retires orphan handles after capture and explicitly releases all handles on scene
discard; no cross-DLL calls occur from static destructors. Legacy/USD inputs still
use their existing per-frame upload path, converted to the new record layout.

The GPU point-instancer copies the surface template and assigns an absolute
per-instance texture factor, never multiplying the mutable template (thread zero
can already have overwritten it). Native grass multiplies texture by vertex RGB
and instance brightness; alpha remains one. Brightness uses Remix's existing
UNORM8 texture factor (round-to-nearest, max error 0.5/255); values outside [0,1]
or nonfinite matrices are rejected, rather than silently clipped. Other native
instance sets omit brightness and preserve their material color/alpha behavior.
The record is 80 bytes with a complete Matrix4 and four uints; SPIR-V validation
passed and reflection confirmed color/enable/padding offsets 64/68/72 and stride
80. The RtInstance copy constructor copies its surface including the new shared
owner; its size guard was updated from 792 to 808 after checking that constructor.
Both builds passed and were deployed at 00:41:44 (PID 15488), archived in
`.research/deployed-symbols/20260912-native-instance-sets`. CS SHA256:
`E36B9FA6EC13632E4CAE0C0CF940C9D5986BE7D83C31EEFA683E8B2DD0192EED`;
runtime d3d11 SHA256:
`C1C628791BAC67B95FBA7BBD63C92FC669A28791DC6CCDFD924EBCF1B4DC73F9`.
Riverwood submitted 22 batches / 54,182 placements. Total native GPU uploads
rose from 22 to 24 while moving to the comparison location, then remained 24
through 00:44, including active animation of other scene objects. CS reported
zero unchanged placement rebuilds/readbacks. The runtime patch was regenerated
with normal repository line-ending rules and reverse-checked.

Visual evidence is **not a pass**: `00-42-44_909` includes the Survival modal;
`00-43-41_208` is albedo; `00-44-01_627` is normals (grass remains instance-up);
`00-44-23_840` moved during capture and is blurred, not a matched full-render A/B.
All names have prefix `CS_2026-09-12_`. The user explicitly reports grass still
looks badly broken and does not animate. Do not describe this as fixed. Debug 0,
free camera off and first person were restored. A 10-second sample measured
14.69 FPS, median GPU frame 51.145 ms, scene 7.207 ms, path tracing 41.699 ms;
it overlapped the modal and is not a gameplay benchmark or evidence of 60 FPS.

The next CS-only diagnostic build reads native WindVector/WindTimer/PreviousWindTimer
from grass shader constant-table entries 5/6/8 (metadata only, not a render-blocking
hook). It logs native vertex wind weights and the error of fitting weight squared
to height. This tests whether an exact affine deformation is possible; no guessed
height-based animation has been enabled. Native RunGrass uses instance phase and
the squared painted vertex weight, so rigid clump motion would be incorrect.
The existing radius/density culling is explicitly disabled in
`rtx_point_instancer_system.cpp` (cullingEnabled=false), independent of game culling.

### September 12: grass cutout correction and wind audit

The cloudy/transparent grass was a confirmed pass-state bug, not solved by
changing normal orientation or global exposure. Native CPU shadow state and its
authored blend descriptor show **blending disabled**, alpha test enabled, and
the NIF's cutout threshold (40/50/60/65/80/100 depending on grass). The same NIFs
have alpha flags `0x12ED` or `0x1201`, which include the blend bit. The bridge had
incorrectly interpreted that bit as grass transparency. `RemixScene` now ignores
the NIF blend bit for grass only, preserving alpha testing and authored thresholds.
No change to exposure, albedo brightness, lighting proxy, or texture sampler was
needed for this correction. Native blocking and the UI composition path are unchanged.

Same-camera captures, both actually viewed:

- Before: `CS_2026-09-12_01-06-40_558.png`, cloudy yellow/white translucent tufts.
- After: `CS_2026-09-12_01-17-55_026.png`, solid cutout blades and dense grass.

Both are in the game's `screenshots` directory. The matching free-camera position
is `(21055.85546875, -43099.51953125, 39.4857177734375)`, returned pitch/yaw
`(-0.0830531493, -0.2862145603)`. Game hour was set to 11.49 for the comparison.
The cutout build (PID 8400) measured 23.78 FPS over 10.134 seconds, median GPU
frame 33.154 ms, scene 7.331 ms and path tracing 23.715 ms. Output remains
1680x1050; this is **not** the required 60 FPS/1080p result. Terrain, other scene
categories, and grass animation remain incomplete.

Earlier same-camera tests with denoising off, bloom off, or motion blur off did
not remove the haze. The native mip-bias test was inconclusive because imported
materials use explicit sampler overrides; do not claim it ruled out sampling bugs.

The runtime deployed at 01:04 also fixes temporal mapping for all surfaces in a
retained placement set (previously only its first surface was mapped). Runtime
logs confirmed `native placement history mapped=54147 new=0` on consecutive
stable frames. This valid history repair alone did not solve the grass haze.

At the native-clock stage, grass was **not animated yet**. The mesh wind audit disproved an exact simple
height-based affine sway: squared painted wind weights have nonzero fit residuals
on most grass assets. Correct animation needs per-vertex deformation with each
instance's native phase, not a rigid transform. Ghidra at `0x1522610` establishes
the native clock and wind inputs; see `remix-native-render-audit.md`. The current
CS build derives inputs for every retained grass geometry independently of native
visibility, and compares them with native constant-buffer inputs. This is input
preparation/validation, not GPU deformation or an animation pass.

Native-clock build deployed as PID 19656 at 01:23:14, CS DLL SHA256
`4946AF8DFD2E9300261C6664838F89899E7258875FCBDB2C9623DE74F36FDC05`.
At 01:24:41 the native/independent wind comparison had 34,512 samples and
maximum error **0**, including paused Survival-modal frames. The modal was then
dismissed through DevBench; by 01:25:05 unpaused comparisons reached 46,032 with
maximum error still 0. Stable frames still rebuilt no placement arrays and did
no GPU source readbacks. Matching DLL/PDB are archived under
`.research/deployed-symbols/20260912-grass-native-clock`; the overwritten DLL is
in `.research/deployment-backups/20260912-grass-native-clock`. Runtime DLLs were
not changed after the 01:04 temporal-history build.

### September 12: retained GPU grass wind (01:35 onward)

The current implementation adds real per-vertex grass deformation, replacing the
previous static clumps. New native API entry points `csRemixCreateGrassInstanceSet`
and `csRemixDrawGrassInstanceSet` retain original instance-local wind phase and
submit just world-space wind amplitude/direction plus the native timer. Public
Remix API structs are unchanged. `csRemixCreateMeshNative` flag bit 2 marks a
native grass source whose vertex alpha is a painted wind weight, not opacity.

The compute shader `native_grass.comp.slang` expands each retained placement set
to fixed vertex/index buffers. Its source is Remix's **64-byte padded**
HardcodedVertex, checked by static_assert; output is compact 36-byte geometry.
The first implementation's 36-byte source-stride guard rejected the input; this
was corrected and redeployed before visual validation. Source placements/meshes
remain GPU-resident and are uploaded only on genuine changes. Static indices are
generated once. Position updates reproduce RunGrass's native sine wave, original
instance-local phase and squared vertex-alpha weight. Output alpha is always 255,
with texture alpha retaining the cutout. Native instance brightness modulates
vertex RGB, and instance-up remains the lighting direction.

Position hashes change with wind; topology hashes include the **ordered** immutable
placement revision. The normal geometry cache preserves previous positions for
motion and selects BVH updates. Camera-origin rebasing remains in the outer
object transform, never the placement arrays. Zero-weight meshes and zero wind
do not update vertices just because time advances. Grass batches use their own
retained BLAS, never the per-frame merged bucket, with a trace-optimized initial
build and subsequent refits. This is not a rigid sway or CPU per-frame mesh rebuild.

Bounded validation is available only when `CS_REMIX_GRASS_PROBE` is present in
the test process environment. It makes **two** small asynchronous readbacks and
then releases its buffer. It compares real GPU output against the native formula,
checks UV/alpha preservation, and measures movement of the same vertices. On
PID 8588 at 01:44:18, both 9-vertex samples had maximum position error
0.00195312 game units and zero attribute errors; the second sample moved by up to
0.144531 units. Timers were 0.700644 and 1.77995. This validates sampled GPU
deformation, not every grass asset/weather/streaming case.

Actually viewed matching-camera grass captures with deformation enabled:
`CS_2026-09-12_01-40-20_440.png` and `CS_2026-09-12_01-40-37_536.png`.
The dense cutout appearance remains intact. The initial animated build measured
20.87 FPS, median GPU 46.989 ms and path tracing 37.254 ms; this regression led
to the retained trace-optimized BLAS change. These numbers precede that change
and are not the final performance result or the required 1080p/60 FPS validation.

Latest CS/runtime build is archived in
`.research/deployed-symbols/20260912-grass-trace-refit`, with overwritten runtime
DLLs preserved in `.research/deployment-backups/20260912-grass-trace-refit`.
CS SHA256 `04C95666EFFF6339C6DAFF38F14B2A07CC7704A2B106B4B2661331EE1A47717E`;
d3d11 `6D34FDF247C7DF231F4B36B16446FC258CADE9D57BF3E326F3A80EAE03A66D81`;
dxgi `C1BABCAED83143CA06D9DDD1B02BC2495D137572094E11A4331D2BE4C02EE192`.
Both shader files are included in `runtime.patch`; the patch was reverse-checked
against the runtime source, and SPIR-V validation passed after the stride fix.
Full goal completion remains unproven: terrain, water/effects, other animation,
characters, first person, lighting/weather, UI preview coverage, and performance
still require implementation or broader verification.

The trace-refit build (PID 41544) did **not** improve the grass-facing result:
01:48 measurement 18.89 FPS / 50.763 ms median GPU; repeat at 01:53 was
18.79 FPS / 50.789 ms, with 41.672 ms in path tracing and 6.881 ms in scene work.
Both were 1680x1050, not the target 1080p. The GPU wind probe again completed:
9 vertices, maximum error 0.00390625 units, movement 0.589844 units, no UV/alpha
errors. The larger absolute-coordinate float rounding remains below 0.004 units.
A 180-degree camera turn and return retained coverage in actually viewed
`CS_2026-09-12_01-52-47_184.png` and `CS_2026-09-12_01-53-26_091.png`.
Freecam was then restored to first person (and subsequently used for the next
matching-camera performance test). Enabling the general animated-instance OMM
option alone did not help: 18.72 FPS / 50.696 ms median GPU.

### Repeated grass opacity maps (implemented and tested)

The expanded grass batches repeat identical prototype UVs and opaque vertex
alpha, but an ordinary OMM request bakes every expanded triangle separately.
The new native-grass metadata distinguishes prototype triangles from BLAS
triangles: bake only the first copy and repeat its micromap indices for the
remaining copies. The cache key includes both counts and the placement topology;
its memory budget includes the full index buffer. Animated grass alone bypasses
the general animated-OMM exclusion because its opacity inputs remain immutable.
BLAS usage counts describe the full expanded triangle count, while micromap
build usage describes the unique prototype count, as required by the
[Vulkan opacity micromap specification](https://registry.khronos.org/vulkan/specs/latest-ratified/pdf/vkspec.pdf).
This does not remove geometry, alter alpha thresholds, or quantize wind.

The build passed and was deployed (PID 40620, 02:02:11). All 22 grass batches
built repeated OMMs: **403 unique triangles for 876,844 expanded triangles**, with
11,282,032 bytes charged to the OMM cache. The bounded wind probe sampled 32
vertices twice: maximum position error 0.00390625, measured motion 0.242188,
zero attribute errors. `tools/remix/CheckGrassRuntime.ps1 -RequireWindProbe
-RequireOpacityReuse` checks these reports without changing the game.

Same-process north-facing A/B/A (10-second samples, native world suppressed):

| Opacity binding | FPS | Median GPU ms | Path tracing ms | Scene ms |
| --- | ---: | ---: | ---: | ---: |
| On | 23.99 | 35.149 | 26.148 | 6.616 |
| Off | 19.29 | 49.856 | 46.383 | 1.228 |
| On again | 23.51 | 35.818 | 26.656 | 6.585 |

Actually viewed captures `CS_2026-09-12_02-04-07_666.png` (on) and
`CS_2026-09-12_02-04-37_134.png` (off) preserve the dense cutout appearance;
wind timing and temporal noise differ. The experiment toggled all OMM binding,
not only grass, so the timing delta is not exclusively attributable to grass.
Binding was restored on and freecam restored off / first person afterward.
This recovers much of the animation regression, but does not meet 60 FPS or
validate the other missing scene features. Captures remain 1680x1050.

Archive `.research/deployed-symbols/20260912-grass-omm-reuse`:
CS unchanged `04C95666EFFF6339C6DAFF38F14B2A07CC7704A2B106B4B2661331EE1A47717E`;
d3d11 `598E591B78BEC5CEBA7381643F6C26079F1307CA618FF80CD39AFFFA35649C57`;
dxgi `61BBD9C5EAA1C692060EB762E9BA37030B790EAB4336E381514282F406D08077`.
Only owned runtime DLLs were overwritten, with previous DLLs/log preserved in
`.research/deployment-backups/20260912-grass-omm-reuse`. Runtime patch refreshed
and reverse-check passed. No goal completion claim.

A brief existing `rtx.logSurfaceCoverage` diagnostic was enabled and then disabled
at 02:06. `Perf.Merge` frame 5647 reported **0 BLAS builds, 51 updates, 23 reuses**
among 74 unique BLASes, rather than a continuous rebuild loop. This count covers
the whole scene, not just grass. The greater scene cost with OMMs attached needs
further profiling; do not describe the remaining performance issue as solved.

The subsequent near-landscape audit and current test-plugin state are recorded
in [remix-landscape.md](remix-landscape.md). Six-layer landscape blending remains
unimplemented; the audit is not a terrain completion claim.

The runtime worktree is `.research/Titanfall-2-Remix`, branch
`codex/skyrim-api`, based on Frissj/Titanfall-2-Remix commit
`62378bcf` (API 0.6.2). Its D3D11 automatic capture implementation is replaced
with a small API-only host. Scene extraction and executable patching from
Titanfall are not used. The fork's public header is vendored with its license
under `include/remix`.

## Build

CS: `BuildDevFast.bat` (compiled successfully on Visual Studio 2026).

Runtime: initialize the pinned repository's git submodules, use the VS x64
environment, set `PM_PACKAGES_ROOT` to `.research/packman`, and run:

First apply `tools/remix/PrepareRuntime.ps1 -Source <clean dedicated clone>`.
The tracked `runtime.patch` and `runtime/d3d11_rtx.*` reproduce the source changes.
The patch was reverse-checked against the development runtime worktree.

```
meson setup _Comp64Release --buildtype release --backend ninja -Ddownload_apics=false -Denable_rtxio=false -Denable_tracy=false
ninja -C _Comp64Release -j 8 src/dxvk/_built_shaders.txt
ninja -C _Comp64Release -j 8
```

For the configured local clone, `tools/remix/BuildRuntime.ps1` runs the two
build stages in the VS environment. **Do not combine shader generation and DLL
compilation into one initial Ninja invocation after shader edits.** This fork
generates shader headers as side effects; Ninja can scan their C++ consumers
before generation and leave embedded shaders one revision behind. On 2026-09-12
this was confirmed by missing new primary-ray probe fields until a second build
recompiled `rtx_pathtracer_gbuffer.cpp` and other shader consumers. A final no-change
verification should rebuild no C++ objects (version/shader-generator checks may
still run).

The pre-existing D: dependency cache is full; use the workspace cache on J:.
The runtime currently includes repairs for switch case scopes, instance copying,
and the no-RTXIO texture worker configuration in the source fork.

## Validation gates

1. Runtime compiles, loads, and registers the same D3D11/Vulkan device as Skyrim.
2. The API diagnostic appears in Skyrim's own window and UI remains interactive.
3. Skyrim camera/depth geometry appears through Remix, with matching projection.
4. Submit real Riverwood meshes/materials and disable vanilla world shading.
5. Capture Riverwood and UI evidence, and inspect runtime logs for actual Remix
   scene submission/path tracing. A successful build or a triangle is not enough.

Deployment must only copy/overwrite explicitly selected files. Back up any
pre-existing DLL before overwriting it. Never mirror with deletion or remove
unowned game/Data files.
