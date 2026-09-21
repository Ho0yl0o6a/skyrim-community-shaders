# Native water import checkpoint — 2026-09-12

## User acceptance — 2026-09-20

The user confirmed: "water is also working at this point, move on". Water is
accepted for the current integration; stop investigating historical water defects
unless a new regression is reported. The older evidence and limitations below
remain a historical record, not an instruction to reopen water work.

Branch: `codex/remix-vulkan`. This is **partial progress, not water parity or goal completion**.

## 2026-09-20 saved floating-sheet regression

**Movement superseded the initial hidden-only fix below.** The user immediately
reproduced visible sheets at Z=30/600 after moving. Native-visible WADING passes
are stencil-masked by each real water surface (SetupGeometry154db70), so importing
their whole quads as physical RT boundaries is wrong even when AppCulled=false.
CS6431F506 now excludes all property-bit0 raster overlays, preserving actual
ordinary water. Their ripple displacement still needs transfer to actual water;
excluding them is not a full ripple or water implementation.

CheckWaterOverlayMovement's8position/POV samples have0overlay submissions with
ordinary water preserved, but all source overlays remained hidden; its strict
native-visible coverage check FAILED/inconclusive. Screenshot060611 after these
tests has no ceiling, but does not prove the user's moving case fully resolved.
No full water correctness claim; original frozen-patch report remains open.

The user's save `Save1_2B23D269_0_73647364_Tamriel_000002_20260920044800_1_1`
reproduces an overhead water sheet near Riverwood, player Z=-234.44. Native
Water2048 wading overlays at Z=-250 and Z=30 were both AppCulled but retained by
Remix. The latter visibly covered the sky. Property bit0 identifies WADING;
the native water manager0x52a890 directly controls the geometry hidden bit.

The first attempt retired hidden WADING registrations only. Other water was not filtered
by AppCulled, and active wading remained eligible. CheckWaterVisibility.cpp tests
the current classification; CheckWaterSavedCase.ps1 records the live saved-case census and
parameter animation without claiming GPU motion. CS446DE6FA deployed with backups.
Matched screenshots055018(before) and055612(after) confirm the ceiling removed
while river/HUD remain; hidden-overlay submission goes to0.

The user also reports frozen patches on the real river in this save. This remains
OPEN. Native and imported scroll/flow clocks change for the nearby river material;
debug16 images055648/055652 show changing detail but cannot qualify every patch or
continuous animation. Active wading/river overlap, flow sampling, missing ripple
displacement and effect surfaces remain relevant. Do not mark water finished.

## Implemented and deployed

`RemixScene` now admits `BSWaterShaderProperty` geometry separately from lighting
materials. It does not cast water to `BSLightingShaderMaterialBase`, apply NIF
alpha-blending as transparency, or substitute the opaque fallback material.
Near-water meshes and active distant-water index segments use Remix's real
translucent transport in the existing Skyrim window. Native world draw/compute
suppression remains enabled; the game still draws the HUD.

The owned runtime exports `csRemixCreateWaterMaterialD3D11V3` and
`csRemixUpdateWaterMaterialV3`. Creation retains native Vulkan normal texture views,
not Skyrim's SRV wrappers. Changing scroll offsets/scales/amplitudes update a
shared material parameter block on the render command stream. They do not create
new material hashes, reupload mesh vertices, or rebuild acceleration structures.
Material destruction removes the API weak-map entry. Existing runtime material
cache ownership keeps in-flight data alive.

The first three native XYZ normal textures use repeat/linear sampling, authored
UV scales and live scroll offsets. They bypass Remix's octahedral normal decoder
and TBN multiplication because Skyrim water normals are world-space. Coordinates
come from absolute world XY, or object UV times 1000 when the native object-UV
flag applies. The sample gradients receive the same inverse scale as coordinates.
Camera rebasing therefore does not move the normal pattern. Meshes are invalidated
when their world transform changes, because these projected coordinates were
baked at upload.

The 112-byte GPU material stride is unchanged. Translucent padding stores normal
indices, 12 half-float base parameters, the wading flag at byte 62, six full-float
UV-affine coefficients at byte 64, four half-float cell offsets, signed grid size,
flow atlas/normal/sampler indices, and full-float time at byte 104. Byte 108 is
the sampler-feedback stamp, followed by two padding bytes. API parameter 24
(noise falloff) is currently not serialized; do not overwrite sampler feedback.
The API carries 26 floats; mismatched older exports are deliberately not accepted.
Base scroll and cell offsets still have half-float quantization.

## Flow-map and wading follow-up

Authored cell flow maps now reach the atlas even with native world compute
suppressed. The specific call at `0x657912` to `0x1540b50` is replaced with an
owned tile-copy shader, only when targeting the native water atlas in Remix mode.
Native reference/off mode calls the original. It is **not** a blanket compute
exception. Native source tiles are BGRA8 (DXGI 87), destination RGBA8 (28), so an
ordinary resource copy is not format-compatible. The owned shader uses typed
Load/store, source/destination bounds checks, the native rectangle, a deferred
command list and restored immediate state. Eleven 64x64 tiles copied successfully
in the Riverwood test, with no copy errors.

Flow uses the atlas's point/clamp sampler and the authored flow-normal texture's
repeat/linear sampler. Four rotated/offset samples preserve native speeds
9.92/10.64/8/8.48 and phases 0/0.27/0/0.62, driven by water clock `0x20d68d0`.
Original mesh UVs are reconstructed from world XY using an affine fit verified
against every vertex; invalid fits disable flow rather than silently misproject.
All 12 live fits had zero reported error. The BLEND_NORMALS path uses atlas blue
to mix the base and flow normals. Native far-distance and shoreline attenuation
remain unimplemented.

`Water2048:0` is the player-centered **wading** surface, not a duplicate to remove.
Native `GetRenderPasses` at `0x152c4a0` maps property bit 0 to technique bit `0x40`.
Geometry setup then reads cell globals `0x34364b0..0x34364bc` and origin globals
`0x3436460/0x3436464`. Their producer `0x52a890` updates from player movement and
TES grid bounds, independently of individual geometry visibility. The Remix VS
equivalent preserves the wading atlas-UV scale 0.1, pattern-UV scale 0.5 and
corresponding pattern gradients. The generic river-cell path remains unchanged.
`CheckWaterCoordinates.ps1` passes 784 algebra cases across grid sizes 3/5/7/9,
maximum double error 4.44e-16, before GPU half packing. Live V3 validation follows
below; this arithmetic test alone does not prove rendered correctness.

Read-only exports additionally retained:
`.research/water-flow-atlas-{references,update,copy}.json`,
`.research/water-native-pass-selection.json`,
`.research/water-wading-{globals,source}.json`.

## Native evidence

Read-only Ghidra project: `/skyrim/ae1.7/SkyrimSE.exe.unpacked.exe`, SHA256
`0b473f0d6c42d0b2885266e78394a64c980480e9663dd1ea8b51731961d0c18a`.

- Water material setup `0x154d4a0` copies material offsets `0x100..0x118`
  directly into normal-scroll constants. Live snapshots prove these are already
  changing offsets, **not velocities to multiply by a second timer**.
- Geometry setup `0x154db70` selects object UV with water-property flag `0x100`.
  Flow-map water uses its own UV path. It binds normal textures at material
  `0x40/0x48/0x50`, with a default texture fallback at global `0x3336448`.
- Native water-form conversion `0x307460` maps record colors and depth/scale/
  amplitude parameters into the shader material. Deep RGB is authored RGB8/255;
  deep alpha is a specular parameter, not opacity.
- `package/Shaders/Water.hlsl` contains the water-coordinate projection and
  layered XYZ decode used by this checkpoint.

Evidence files: `.research/water-native-setup.json`,
`.research/water-native-animation.json`, `.research/water-animation-symbols.json`.
The native scroll-update producer is not fully traced yet; native shader setup
only copies the values. The flow follow-up adds the narrowly scoped owned atlas
copy call replacement described above, not a native world rendering dependency.

## Base-layer validation (superseded build, retained baseline)

Deployed symbols and DLLs: `.research/deployed-symbols/20260912-water-base`.
Prior owned DLLs/logs: `.research/deployment-backups/20260912-water-base`.

SHA256:

- CS: `C3715D8A0CFFECC83EA3858A77453605B5EE9B00C66444FF432B1DF985B25182`
- Remix D3D11: `58C438681A25DADC3DBB613F7D523D08B6D4240083EB0D8963286A3B2435EB3D`
- Remix DXGI: `3DC9C2F05D31B0869D5B77314BC030C1E21B9B3177268B32BE3F5E676508145E`

Both builds passed. `runtime.patch` was regenerated and reverse-apply checked.
SKSE process 35748 launched at 03:53:17 local time. `coc riverwood`, no survival
mode, free camera at `(17400,-44200,600)`, drive pitch `0.65`, yaw `0`.
Configured game hour 11.49 (time continues advancing). Final debug view restored
to zero. No persistent quality/INI settings changed.

Screenshots in the game's `screenshots` directory, all visually inspected:

- `CS_2026-09-12_03-50-50_986.png`: before import, dry visible riverbed.
- `CS_2026-09-12_03-54-33_968.png`: water surface, reflections/transmission, HUD.
- `CS_2026-09-12_03-56-16_859.png` and
  `CS_2026-09-12_03-57-01_409.png`: matching normal views; the water pattern moves
  while rocks and camera remain fixed. This verifies actual shader animation,
  not just changing CPU parameters.

`CheckWaterRuntime.ps1 -RequireImports -RequireAnimation` reports 65 imports,
zero import failures, 67 loaded water shapes, 46/48 nearest shapes submitted and
all 46 imported parameter sets changing. The two unsubmitted near-list entries
are distant segment meshes; investigate coverage rather than forcing them on.
The script deliberately does not claim visual correctness.

Regression checks passed: 22 grass allocations/54,147 placements, two GPU wind
probes (maximum position error 0.00195312), 22 OMM builds using 403 unique rather
than 876,844 expanded triangles; 100 landscape imports including 63 six-layer;
397 distant-tree atlas imports with no failures.

Ten-second final-mode sample: 20.38 FPS at **1680x1050**, median GPU 39.566 ms,
path tracing 28.532 ms and scene work 8.011 ms. This is a different view from the
tree benchmark and is not an apples-to-apples performance comparison. It does
not meet 60 FPS at 1080p. Native-boundary logs continue reporting roughly
1,000 blocked draw batches and 98 blocked compute submissions per sample.

## Flow validation and native comparison

V2 flow checkpoint: `.research/deployed-symbols/20260912-water-flow`, with captured
logs and symbols. SHA256:

- CS: `F769BCD184A40AC5AD0F22107EFE149EE223CDE9E807F5EB76F1EF22DF631A00`
- D3D11: `53C717547A9573E446CA179FDFEB09F2B5628276140F91FD0B3FAE5FFF7F5B22`
- DXGI: `801280850E06497687FFC23E1BFA6D20A985B4B6BA6D25BCB4BBADDCB554A916`

At the same free-camera pose, `CS_2026-09-12_04-16-11_654.png` shows finer,
directional surface waves. Debug-normal captures `04-18-28_221` and
`04-19-22_877` show moving water normals with camera and rocks fixed. Both were
visually inspected. `CheckWaterRuntime -RequireImports -RequireAnimation
-RequireFlow` reported 65 imports, zero failures, 67 loaded water shapes, 46
submitted in the nearest list, 46 animated base materials, 12 animated flow
materials, 11 copied tiles and no UV-fit rejections.

The native reference `CS_2026-09-12_04-20-05_558.png` still shows much darker,
blue-green water, stronger foam streaks and different shoreline/scene lighting.
Native-reference mode was followed by a process restart to restore Remix; it
cannot safely be toggled off in the same process. No claim of water parity.

## Open work / next checkpoint

### V3 deployment — wading coordinates

Built CS and the full matching Remix runtime, regenerated `runtime.patch` and
successfully reverse-apply checked it. Deployed only the three owned DLLs;
no game files were deleted. Previous DLLs/logs are in
`.research/deployment-backups/20260912-water-wading`; current symbols/DLLs are in
`.research/deployed-symbols/20260912-water-wading`.

- CS: `276BCC4501F8754816CCE9EC40862B5F8516D7CCA8E90D5EAAE0D5DE976A27B3`
- D3D11: `B21528CE529ECA9DC7EEB04602A13E4C835DD4CE40893C1B54D3B6783C0A0237`
- DXGI: `ED199A47B09B40D257CB0121BA639C8E7616EC34941ECF56E95238A7D39DEF27`

SKSE PID 26724 launched at 04:34:09 local. Fresh Riverwood load, survival disabled,
same fixed river camera, game hour set to 11.49 (advancing). The water runtime
check including `-RequireWading` passes with the same flow counts as V2 and one
submitted wading material carrying the new flag. The material's folded cell
parameters are `(1.955078125,7.275390625,3.955078125,12.275390625)`. This is CPU
evidence, not proof of correct wading visibility or ripple rendering.

`CS_2026-09-12_04-35-55_717.png` was visually inspected: directional water remains
visible with HUD, but reflections/transmission still differ substantially from
the native reference. Ten-second sample, after compilation completed: 21.74 FPS
at the captured 1680x1050 output; median total GPU 34.393 ms, path tracing
23.891 ms, scene 7.558 ms. Not the requested 60 FPS/1080p gate and not a controlled
performance A/B test. Grass wind probes passed (maximum position error
0.00390625); landscape 100 imports/63 six-layer and distant trees 397 atlas
imports passed with zero import failures.

Normal capture `CS_2026-09-12_04-36-49_071.png` was inspected and retains the
fine directional river pattern. A second view above the player's wading mesh,
`04-37-15_269`, is mostly terrain/vegetation and does **not** establish visual
wading parity. Debug view was restored to zero and the original river camera
restored afterward. Current-run logs were copied to the symbols archive.
Native-boundary samples still report approximately 987-1136 blocked draw batches
and 98 blocked compute submissions. Normal game UI remains visible in captures.

### Remaining water work

Do not claim the forge trough, shoreline behavior, underwater rendering or all
water correct yet. The foam visible in the native river reference is missing;
effect/particle materials remain unsupported and need native path identification.
The wading displacement texture and ripple perturbation are not yet imported.
Also audit wading's **semantic visibility** independently of frustum culling:
the native producer updates its activation and AppCulled state as well as its
position. Merely importing every loaded wading mesh may expose an inactive
overlay. Do not solve this by reinstating a general native visibility filter.

Native flow inputs already identified in `0x154db70`:

- Flow atlas NiSourceTexture global `0x33d3e60`, point/clamp sampling; default
  normal fallback global `0x3336448`.
- Flow normal material `0x58` (RiverFlow.dds in Riverwood), repeat/linear sampling.
- Atlas grid dimension global `0x33d3d98`.
- Non-wading cell constants: `(flowX, gridDimension-flowY-1, cellX, -cellY)`
  from property `0x98/0x9c/0x90/0x94`.
- Flow time is global `0x20d68d0` (native shader multiplies by 0.001), not the
  grass timer at `0x20d68e0`.
- Shader flow UVs need original vertex UV in addition to the base normal's
  world projection. Four offset/rotated flow samples blend with authored cell
  weights. Preserve this rather than replacing it with generic scrolling.

Default normal fallbacks are now implemented. Still missing:
shore-depth normal attenuation, full far-distance blending, displacement/ripples,
native weather/light color coupling, transport calibration. The current mapping
uses physical IOR 1.333, deep RGB as transmittance and native underwater fog-far
as measurement distance. **That is an initial transport approximation**, not an
equivalence to Skyrim's screen-space shallow/deep fog equation. It must be
calibrated against native references. The rest of the full renderer goal remains
open as recorded in the focused tree/landscape/native-render documents.

### 2026-09-20 transport source audit (not implemented)

The matched-camera native reference at `030013-land-tbn-native-lit.png` has
green depth-faded river water. Remix `030141-land-tbn-final-lit.png` retains a
darker reflective surface and stronger wave detail. These are visual references,
not quantitative parity measurements; lighting/time/exposure were not normalized.

`Water.hlsl::GetWaterDiffuseColor` uses above-water fog far/range/power and
blends refraction toward shallow/deep diffuse colour. It is not equivalent to
deep RGB Beer-Lambert absorption over `underwaterFogDistFar`, the current import.
The saved native setup audit (`.research/water-native-setup.json`, function
`0x154d4a0`) confirms above-water fields at material offsets `0x158/0x15c/0x160`
and native light/weather colour multiplication of shallow/deep RGB. These
inputs, depth controls and that lighting coupling still need transport.

Remix currently evaluates medium transmittance in `geometry_resolver.slangh`
(primary start-in-medium and PSR medium segments) and `integrator_indirect.slangh`.
An additive water-colour contribution must be placed along the refracted segment
with the correct incoming throughput, not approximated as constant surface
emission or a global exposure change. The existing material-extension buffer is
a possible location for extra water fields without growing every GPU record;
no extension or fog implementation was added in this audit.

Native flow-map sampling deliberately uses unrotated continuous UV gradients
even though sample coordinates rotate per flow tile. Rotating those gradients
as an assumed correction would diverge from the reference shader. Shore depth,
far-distance blend and Unified Water permutations need explicit treatment.
