# Native effect import audit — 2026-09-12

## Effect radiance encoding — 2026-09-20

Skyrim's `Color::RadianceToLinear` uses `pow(abs(color), 1.6)` when Linear
Lighting is disabled. Native effect RGB, including its authored scale and
vertex colour, is in that same gamma space. The native-effect runtime path
previously passed it through Remix's generic 2.2 decode. For HDR channel 3,
those mappings produce 5.799546 and 11.211578 respectively; authored intensity
must not be lowered to compensate for the wrong transfer function.

The host now sets NativeEffectStorage flag4096 for non-LL effects. Runtime
effect creation/update accepts this flag (maximum valid word8188, low two bits
reserved for SRV sRGB metadata). Only flagged native effects use the shared
`nativeEffectRadianceToLinear` helper, for albedo and resolved emission/external
colour. The 20-float API and 112-byte GPU material layout remain unchanged.
Refraction uses a separate flag path and is not assigned this colour flag.
This requires deploying the matching updated runtime and plugin together.

This is not complete native colour/compositing parity. In particular, blending
gamma-space native effects and then decoding the result is not equivalent to
linear-space blending of individually decoded layers. Native BSOrderedNode
layer ordering, environment-map glass inputs, soft intersections and the
enabled Linear Lighting effectGamma/multiplier path still need qualification.
The no-flag branch retains existing behaviour; it is not a verified LL path.

Unit coverage adds zero, unit, half, negative and HDR channels to the existing
angular-opacity test. Both native_effect and native_refraction tests pass.
Shader permutations and plugin compiled; live deployment results are recorded
in the latest handover (do not infer a visual pass from these scalar tests).

Live build `20260920-effect-native-radiance`: imported InnerHaze02 layers report
flags4208 (4096+112), scale3, both animated. Selected-pixel GPU diagnostics also
read flag4400 (4096+304) on a palette smoke effect, confirming the new encoding
bit survives GPU packing. The probe is last-hit-wins, so this does not identify
the glass layer or measure its final decoded radiance. Diagnostics were disabled
afterwards. Capture013943-alchemy-native-radiance remains clearly fluorescent
against native reference003011: do NOT call this a successful glass appearance
fix. It corrects the transfer equation only. Native/import effect UV, refraction
UV/strength, directional transport and inn membership checks pass after deploy.

## Angular-opacity correction — 2026-09-20

Native `Effect.hlsl` evaluates its smoothstep-shaped angular opacity in the
vertex shader, from each world-space vertex normal and the direction toward
the camera. It then interpolates the resulting scalar. Evaluating the curve
after interpolating/normalizing/bending the normal is not equivalent.

The runtime now evaluates that scalar while the three vertex normals and
positions are already available in `surfaceInteractionCreate`, before normal
hemisphere correction and bending. Native particle-layer effects carry the
interpolated value in a shader-local `SurfaceInteraction` field; no GPU buffer
layout, API ABI, CPU vertex upload or material colour changes are required.
The primary camera remains the angular reference for secondary rays too,
matching the authored camera-dependent effect. Non-layer effects retain their
previous hit-space fallback; no-normal and model-space-normal effect variants
are not qualified by this implementation.

`native_effect_math.h` is shared by the shader and dedicated unit test. The test
covers endpoints, saturation, absolute cosine, reversed ranges, the existing
degenerate-range fallback, and the difference between interpolation before and
after the nonlinear curve. Both `test_native_effect` and `test_native_refraction`
passed in `_Comp64UnitTest` (2/2, 0.03s each). These are scalar CPU tests, not GPU
image-parity tests. All shader permutations compiled successfully.

The inspected `004204-alchemy-vertex-falloff.png` still shows overly emissive
alchemy glass compared with `003011-alchemy-native-reference.png`. This
correction does NOT fix the overall material mismatch. The baseline/reference
camera and conditions are in the handover. Native RGB scale3 is confirmed by
SetupMaterial and must not be reduced arbitrarily to hide another error.

All 14 AlchemyWorkstation matches were inventoried: both InnerHaze02 effects
and the OuterGlass02/InnerGlass02 lighting meshes are submitted. OuterGlass02
uses native material feature1, PlainGlassTile01 diffuse/normal, alphaFlags4333,
and an ordered node. The importer currently assigns ordinary lighting materials
roughness0.8 and does not transport the native environment-map material inputs.
That is a further material gap, not yet proven to explain the emissive shells.
LinearLighting reports loaded=false in the Remix test session. Also audit
compositing/colour space and ordered surface attenuation against the reference.

A more basic input gap also needs priority: `RemixScene::Upload` initializes
ordinary lighting vertex colour to white and only restores authored bytes for
effects, landscape, grass and hair. Generic lighting blends also select texture
RGB alone and material alpha alone. Native `Lighting.hlsl` multiplies ordinary
RGB by `input.Color.xyz` and, except TREE_ANIM/LOD variants, opacity by
`input.Color.w`. Inspect actual glass and representative static/character vertex
inputs before changing the path; preserve the special hair, grass, terrain,
tree-wind and LOD channel semantics. Do not replace this with a global brightness
multiplier. This source discrepancy is proven; its contribution to this specific
glass image is not yet measured.

Branch `codex/remix-vulkan`. Retained triangle-effect materials now render, but
**foam/fire/particle correctness is not established**. The original diagnostic
checkpoint below is retained as the baseline, followed by implementation tests.

## Original diagnostic checkpoint (04:42 local)

`RemixScene::Capture` rejects non-lighting/non-water/non-distant-tree properties.
It also requires `AsTriShape()`, so allowing effect materials alone cannot import
particle systems. Loaded scene traversal does retain the missing effect objects.

The inspector now reports actual material type, property alpha, shader flags,
effect source/palette paths and resident resource formats, color/scale, UV
transform, falloff, soft depth, lighting-influence byte and external emittance.
It checks both property type and the material's virtual `GetType()` before reading
`BSEffectShaderMaterial`. Texture paths also participate in the inspector filter;
filters are currently case-sensitive. Many live effect texture `name` strings
are empty even though the material's authored paths and SRVs are valid.

Deployed CS SHA256:
`01E07E65C2FAE0E850DFA7946A3E1B4264984FBD1B091AA36C9365375CEDBE39`.
Symbols and logs: `.research/deployed-symbols/20260912-effect-audit`.
Prior CS/logs: `.research/deployment-backups/20260912-effect-audit`.
Only the owned CS DLL was overwritten; the tested V3 water runtime remains:
D3D11 `B21528CE529ECA9DC7EEB04602A13E4C835DD4CE40893C1B54D3B6783C0A0237`,
DXGI `ED199A47B09B40D257CB0121BA639C8E7616EC34941ECF56E95238A7D39DEF27`.
No files were deleted. Build succeeded, SKSE PID 5056 launched at 04:42:59 local,
fresh Riverwood load with survival disabled. Normal Remix mode/world suppression
and HUD remain active; camera restored to `(17400,-44200,600)`, pitch 0.65/yaw 0.
Water import/flow/wading regression checks still pass.

## Native evidence

Read-only Ghidra executable SHA256
`0b473f0d6c42d0b2885266e78394a64c980480e9663dd1ea8b51731961d0c18a`.
Exports: `.research/effect-native-{symbols,setup,technique}.json`.

- `0x1529a50`: effect GetRenderPasses; delegates technique construction to
  `0x152a050`. It distinguishes triangles, particle systems and strip particles,
  lighting, palettes, projected UV, falloff, soft intersection and blend variants.
- `0x152a050`: source-texture presence is selected from authored source path
  at material +0x70, not just a non-null fallback texture. Palette color/alpha
  require property bits 4/5 respectively and a nonempty palette path (+0x78).
  Technique bits are 0x80000/0x100000. Particle geometry selects different shader
  inputs, not ordinary triangle vertices.
- `0x15569e0`: source texture +0x58 binds t0 with address mode byte +0x80.
  Palette +0x60 binds t4 with clamp/linear sampling. UV offset/scale read the
  native active pair at +0x0c/+0x1c using index global 0x20d69d0.
  Non-palette color is base RGB (+0x48) times scale (+0x6c), alpha unchanged.
  Palette-color mode keeps base color unscaled and passes scale separately.
  Soft depth +0x68, falloff +0x38..0x44, lighting byte +0x81 are separate inputs.
- `0x1556e30`: external emittance is property +0x88 (white when null), and
  property alpha +0x30 is passed separately. Particle indexed texture coordinates
  are uploaded from particle data; the triangle importer cannot stand in for it.

`package/Shaders/Effect.hlsl` provides the matching equations, but includes CS
feature/color changes: audit vanilla shader variants where precise parity needs
more evidence. Important palette dependencies:

- Color lookup coordinates are `(source.g, baseColorMul.r)`; scale is applied to
  the palette result. `baseColorMul` includes vertex color when that variant uses it.
- Alpha lookup coordinates are `(source.a, combinedAlpha)`. Combined alpha
  includes base/vertex/property alpha, angular falloff and soft intersection.
- Consequently **a one-time RGBA texture bake cannot reproduce this material**:
  palette lookup depends on per-hit/per-vertex and changing state. Keep native
  material parameters and perform the palette lookup in the Remix shader.
- Source/palette scalar lookup channels must not be gamma-decoded as color.
  Decode the final authored RGB once when entering linear shading. Existing
  lighting-material normal decode and diffuse treatment cannot be reused blindly.

## Live inventory (not a renderer test)

`CheckEffectInputs.ps1` compares two snapshots with a two-second interval.
Each filter returns at most the nearest 48 matches; groups overlap and must not
be added together. Name/world matching is diagnostic, not a unique engine ID.

- `WhiteWater`: 267 matches. All 48 sampled are triangle shapes, source SRVs
  resident, 47 authored palettes, 48 changing UV transforms, **zero submitted**.
  Sources include FXWhiteWater01/02, FXwhiteWater and FXfluidTile01; palettes
  include GradWhiteWater, GradWhiteWaterMedSoft and GradWhiteWaterSoft.
- `Smoke`: 64 matches; sample 23 triangle shapes/25 other geometry, 41 authored
  palettes, 21 UV changes and one base-color change, zero submitted.
- Lowercase `fire`: 31 particle objects using FXfireAnim04loop but **water-splash
  palettes**, not forge flames. Texture filename alone does not identify an effect.
- `Flames`: one triangle at `(20563.398,-45329,-54.275)`, VaporTile01 with
  GradFlame01; UV animates. Flags `00000020C0000078`, alpha flags 4333, scale 1.6,
  soft depth 10. Both base alpha and property alpha were **zero** in this sample.
  Respect native activation; do not force it visible to make a screenshot pass.
- `BlacksmithForgeGlow`: one triangle with GlowSlightFlash, no palette,
  alpha flags 4109 (additive blend). `JetBoost`: one triangle with CloudTile and
  GradHotCoals, also additive. Neither is submitted.

Editor markers with no authored textures are also retained in the scene graph.
Do not enable every effect indiscriminately or reinstate general AppCulled/
frustum filtering to hide them. Semantic visibility/activation needs its own
native evidence. In particular, non-null default normal SRVs do not make markers
valid authored effects.

## Next implementation

1. Add retained effect materials with live UV/color/alpha/scale parameters,
   resident source/palette Vulkan views, native address modes, palette color and
   alpha evaluation, and per-hit falloff. Update actual parameter changes without
   rehashing materials or uploading static mesh vertices each animation frame.
2. Preserve authored vertex RGB/alpha roles, two-sidedness, and native additive/
   alpha blending. Separate light-influenced whitewater/smoke from unlit glow/fire;
   avoid making every effect an opaque diffuse surface or every effect a light.
3. Implement soft-intersection fade using Remix scene depth/ray information, not
   a native world draw. Compare the fixed river native reference in remix-water.md.
4. Add native particle input/deformation and indexed sprite animation separately.
   Verify forge activation and wheel splashes, not merely all-resident counts.
5. Native semantic marker/activation filtering, full shader variants, weather/
   lights and performance gates remain necessary for the full goal.

## Retained material implementation (05:06 local)

Both CS and runtime builds succeeded; a second Ninja run confirmed current
sources and the exported runtime patch passed reverse-apply checking. Three
owned DLLs were overwritten, with previous binaries/logs in
`.research/deployment-backups/20260912-effect-base` and matching symbols in
`.research/deployed-symbols/20260912-effect-base`. No files were deleted.

- CS SHA256 `D7319BDF601F95010EDFC3EF6B78B14C844F9BC30DAF4A5ECCDF769A749D887D`.
- D3D11 SHA256 `DF8510D9F43EF43475EB1ACBB0C2A580A005D439F163FB7D85E4D5777A2F77CA`.
- DXGI SHA256 `EFAD9CDE775B28790BCAA553F26991F43879D85065101B3293C3747F1D47850E`.

Private V1 create/update exports retain same-device source/palette texture views,
native addressing and clamp/linear palette sampling. Twenty live parameters carry
UV transform, base color/scale, alpha, falloff, external emittance, and lighting
influence. Stable shared parameter ownership avoids rehashing/uploading static
geometry each animation frame. The 112-byte GPU material stride is unchanged;
effect-specific tail storage is distinguished by its own material flag.

Shader evaluation performs palette color/alpha lookup after native inputs,
per-hit angular falloff, final color decoding, and a preliminary diffuse/emission
lighting split. Soft intersection is transported but **not evaluated**. Native
engine lighting is not yet reproduced. Particle systems, projected UV and blood
variants remain unsupported. Empty authored source paths are rejected rather
than importing editor-marker fallback textures.

Fresh Riverwood import check: 48/48 sampled whitewater materials submitted and
48/48 retained UV transforms animated. Water regression: 65 imports, zero failures,
46 submitted samples, 12 flow materials, 11 copied tiles, zero failed tiles,
12 valid UV fits and one submitted wading material. These are CPU checks only.

Visual evidence in the game `screenshots` directory:

- `CS_2026-09-12_05-06-51_502.png`: same river camera as native reference; expected
  white foam is still missing. Import success is not visual success.
- `CS_2026-09-12_05-07-22_696.png`: diffuse debug view; water region black.
- `CS_2026-09-12_05-09-04_547.png`: forge glow visible, but excessively solid /
  incorrectly shaped; flame/particle parity not achieved.
- `CS_2026-09-12_05-09-45_995.png`: disabling runtime face culling did not restore
  the foam. Culling was restored before quitting this diagnostic process.

Review found that `Upload` still defaulted effect vertex RGBA to white. Corrected
to preserve authored RGBA, which feeds palette coordinates and edge opacity.
CS rebuilt and deployed at 05:11; SHA256
`1E154322A42E8BA9153CCB67C8FEF1CD8981A4A6B4EA1D74D8E8AF9980B918EE`, symbols
in `.research/deployed-symbols/20260912-effect-vertex`. Runtime unchanged.
`05-12-17_445` shows the circular ripple pattern removed but still lacks foam.
`05-15-11_354` shows reduced/varied forge glow after preserving vertex RGBA;
full flame and particle appearance remains unverified. Grass GPU wind probes
remain within 0.00390625 position error; 100 landscape imports (63 six-layer)
and 397 tree-atlas imports passed their nonvisual regression checks.

The active effect UV slot now reads audited native global `0x20d69d0`; live
Riverwood sample selected slot 0. CS and a read-only GPU hit probe were rebuilt
and deployed to `.research/deployed-symbols/20260912-effect-hit`:

- CS `C9AA78574011A276850F2727A6C49AA022A03C87D7C93CEDA6A79CEE0AD0B08B`.
- D3D11 `BA5D55AEDFDBD071C99FD91432F0002221FF184BEB1AD45E1C73A94B1C2F1216`.
- DXGI `A82CC32BB687ECAB02CAA3E2447A1B103CD4641DB4230FB9480CBA9EC92A61FE`.

The initial selected-pixel probe shared the legacy GPU print ring with other
probes, which continually overwrite it. No printed hit is **not evidence of no
hit**. An isolated frame-ring version is being built. Raw-albedo view at
`05-20-16_372` still shows no foam. Disabling WBOIT for a comparison at
`05-22-36_815` did not restore foam; WBOIT was restored immediately afterward.

An 18.71 FPS forge sample was collected **while shader compilation ran**;
it is a contended diagnostic, not an uncontended performance benchmark or a
60 FPS/1080p pass.

## Isolated GPU hit probe (05:25 local)

Runtime built successfully and deployed; symbols in
`.research/deployed-symbols/20260912-effect-ring`. CS remains the active-UV build
above. D3D11 SHA256
`ED243D97D6430018DE5DD1B040D5A6A294218E4940741D24AA72DDF6EA12418B`,
DXGI `477BD32C4AECFB93C1223248BC2E7BF739DA77963A37206DB474B64E011F9CD6`.
The runtime patch was regenerated and reverse-apply checked. No files deleted.

At the fixed river camera, the isolated selected-pixel probe `(560,350)` reports
native effect flags 508 and palette alpha equal to resolved opacity, varying
from zero through approximately 0.25. Captured frame is consistently CPU frame
minus three. This proves actual shader effect hits and nonzero palette alpha,
not merely successful CPU import. This first isolated version also runs in
secondary-ray shaders: it does **not** yet prove primary-camera coverage.

Disabling `enableSeparateUnorderedApproximations` made the river region black
(`05-27-27_992`) and did not restore correct foam. The option was restored.
The next probe revision adds separate authored-color / outgoing-radiance rings
and restricts writes to primary-ray shaders. Until that evidence is collected,
do not attribute the missing foam to a particular lighting or compositing cause.

## Embedded shader dependency correction and current checkpoint (05:43)

Important: the initial single-invocation runtime build left some C++ shader
consumers stale. The new color probe was absent despite updated generated shader
headers. A second build at 05:36 recompiled `rtx_pathtracer_gbuffer.cpp`, indirect
integration and other consumers; a subsequent no-change build compiled no C++.
The new `BuildRuntime.ps1` first builds `src/dxvk/_built_shaders.txt`, then invokes
Ninja separately for the DLL. This sequence was tested successfully. Earlier
diagnostic captures are not exact matched-source shader comparisons.

Fully embedded runtime deployed at 05:37, symbols in
`.research/deployed-symbols/20260912-effect-embedded`:

- D3D11 `3CF8D1B7F6D94F21B6144BEE4CA0EF7046C6552FF886660D77B6626B89CA8D23`.
- DXGI `2DAB5C944D19CDCD4E84765FF883E97F19B5B600E93B14024264D820510220F5`.

Primary-ray probe now reports both alpha and matching color/radiance records.
For example flags 508, palette alpha/resolved opacity 0.286133, authored RGB
`(0.49707,0.510742,0.519043)`, lighting influence 0.749023, premultiplied albedo
luminance 0.0745239 and emission luminance 0.0162506. This confirms functioning
primary shader palette/color evaluation; **it does not explain the remaining
loss of visible foam**. `05-38-45_005` still lacks the native foam pattern.

Blended native effect triangles now carry Remix's Particle category, selecting
effect-layer compositing rather than solid-surface transparency. Static triangles
are retained; this does not import native particle-system geometry. CS rebuilt
and deployed: `6BD81991E7E559398F61A10AD506718138B9F59AB7BB3486DE9BF397F113CDFA`,
symbols in `.research/deployed-symbols/20260912-effect-layer`. Runtime above is
unchanged. `05-41-53_524` shows changed/brighter water reflections but still not
the expected native foam. `05-43-16_483` shows forge glow, not complete flames.
Do not call either an appearance pass. Full native lighting coupling, soft fade,
particle geometry/activation and compositing need further work.

Current process PID 22028, launched 05:40:58. Riverwood, survival off, HUD working,
Remix scene on/world suppressed/nativeReference false. Debug view 0; GPU print
disabled after diagnostics. WBOIT, separate unordered approximations and
stochastic alpha blend are restored to their default true settings. Freecam
currently at forge `(20480,-45670,160)`, pitch 0.65/yaw 0.2. No deletions.

Latest nonvisual checks still pass: 48/48 whitewater imports/UV animations;
Flames/BlacksmithForgeGlow/JetBoost all submitted and Flames UV animated;
65 water imports, 12 flow materials, 11 copied tiles, zero failures;
grass 22 allocations/54147 placements, max GPU position error 0.00390625;
100 landscape imports including 63 six-layer; 397 tree-atlas imports.
Uncontended 10-second forge test, GPU print off: **21.36 FPS at 1680x1050**,
median GPU total 24.926 ms, path tracing 14.818 ms, scene 7.549 ms.
Still far short of the goal's 60 FPS at 1080p.

## Native effect depth-boundary correction (05:55)

The live A/B test at the fixed river camera isolated a cause of missing foam:
lowering `rtx.particleSoftnessFactor` from 0.05 to 0.00001 restored broad foam
streaks (`05-46-55_135`). The option was restored to 0.05 immediately afterward.
Remix's generic fade uses the nearest ray surface, which is water only a few
units below the whitewater. Skyrim's SOFT shader uses opaque-world depth before
water/effect draws, and applies its factor before the palette-alpha lookup.

`resolveVertexUnordered` now bypasses this incorrect additional post-material
fade for NativeEffect materials only. This is not an implementation of native
SOFT fading: acquiring the correct opaque depth and applying authored softDepth
before palette lookup remain necessary. Other particle materials keep their
existing fade.

The staged runtime build succeeded, including all affected C++ shader consumers;
the runtime patch was regenerated and reverse-apply checked. Deployed symbols
and matching DLLs are in `.research/deployed-symbols/20260912-effect-fade`:

- CS `6BD81991E7E559398F61A10AD506718138B9F59AB7BB3486DE9BF397F113CDFA` (unchanged).
- D3D11 `AC2A56729595EB2B0893DF35A3E98A196A7727EB0BD161738A59518417A8EF83`.
- DXGI `E01A6AAC50EFE20E29545821257596540D769B40E77C924C45A1CBB3ED851181`.

Process 39400 launched 05:53:59. Captures `05-55-07_727` and `05-55-25_325`
at `(17400,-44200,600)`, pitch 0.65/yaw 0, show visible whitewater at the default
particle fade. The foam texture changes between the stationary-camera frames;
48/48 sampled submitted whitewater materials also report animated UVs. This
confirms visible animation, not exact native timing/direction or full appearance
parity. The water remains excessively reflective/bright compared with the native
reference. Grass wind error remains 0.00390625; 100 landscape imports (63 six-layer),
397 tree-atlas imports, and 65 water imports/12 animated flow materials pass.
No files were deleted. UI remains native and world rendering suppressed.

## Native alpha compositing and particle inventory (06:07)

Native standard-alpha materials already opacity-weight their diffuse and
emissive radiance. WBOIT multiplied this by alpha again, then applied its legacy
4x energy compensation. A single native layer therefore resolved to
`4 * C * alpha^2`, not `C * alpha`. Native `BlendType::kAlpha` contributions now
use a separate premultiplied sum, sharing the coverage/weight denominator but
not legacy energy compensation. Other blend modes retain their existing
behavior. This does not make unordered blending an exact depth-sorted solution.
`CheckEffectBlendMath.ps1` passes 195 CPU reference assertions covering zero,
small, fractional and full alpha; varying weights and compensation; equal-color
overlapping layers; and preservation of legacy math. These are not GPU tests.

Runtime and CS builds succeeded. Runtime patch regenerated/reverse-apply checked.
Deployed matching binaries/symbols: `.research/deployed-symbols/20260912-effect-alpha`:

- CS `47E38C703F33FDFA8ECB9E9FFBBD1895EF66C5D62F0F0BDFBFC0AD01F756880F`.
- D3D11 `D55F25F67A067C8982F347665EC06911256F9F42DAD0567248EB5B930DA51586`.
- DXGI `97A02BC235E7D5336A334669E4DC5F4104A1A2D7A3EF97A1F25CB9CAB5212052`.

Process 38924 launched 06:04:30. `06-06-02_209` confirms foam remains visible;
`06-06-57_762` confirms forge glow, but not complete forge/fire appearance.
All five import/wind checks still pass. No compiler or Ghidra processes ran
during the 10-second measurements: fixed river camera 18.75 FPS, GPU median
36.534 ms (path tracing 25.767, scene 7.882); fixed forge camera 20.94 FPS, GPU
median 24.910 ms (path tracing 14.793, scene 7.561). Both captures are 1680x1050,
not the required 1080p/60 FPS pass. The forge result is close to the preceding
21.36 FPS checkpoint; the different river/forge views are not directly comparable.

The test configurator now explicitly disables GPU print so fresh tests do not
inherit the fork's enabled diagnostic default. Current state: debug view 0,
GPU print off, WBOIT and separate unordered approximations enabled, particle
softness 0.05, nativeReference false, Remix scene enabled and native world
suppressed, survival off, HUD visible. Camera is at the forge
`(20480,-45670,160)`, pitch 0.65/yaw 0.2. No deletions.

### Native particle systems: audited but not yet imported

Read-only Ghidra artifacts: `.research/particle-native-symbols.json`,
`particle-native-layout.json`, `particle-native-update.json`; same audited
1.7.99 executable hash as above. Key RVAs:

- `f992b0`: native particle update. Instructions confirm data pointer at +158,
  lastUpdate +18c, reset +190 and modifiers +168. This is simulation, not a
  geometry builder; `f053d0` only increments a particle census counter.
- `f99ce0`: worldspace flag +192. World-space emitters deliberately have identity
  rotation/zero translation, retaining inherited scale. An inspector distance
  based on geometry world translation does not locate their actual particles.
- `f13550`: capacity +12, positions +30 (float3), colors +38 (float4), radius +40,
  size +48, angles +50, axes +58, indexed UV rectangles +60/count +68,
  aspect +6c, active count +7c, texture indices +80. `f14f70` reads active +7c;
  `f14f90` clamps it to capacity. `faab90` allocates particle-info +90/stride32.
- Existing effect setup `1556e30` uploads indexed UV rectangles directly from
  particle data +60/+68. Effect VS maps `u = rect.y * u + rect.x`,
  `v = rect.w * v + rect.z` before the material UV transform. These are
  offset/scale pairs, **not** UV min/max coordinates.

`remixScene` inspection now exposes a read-only `particles` object on native
NiParticleSystem geometry, guarded to this exact executable version. It includes
counts, simulation time, worldspace state, and up to four position/color/size/
angle/texture-frame samples. Counts are bounds-checked before reading arrays.
This does not force simulation or change native culling/activation.

Live `FXSmokeRiverwoodSmith01/smoke03` has 53 then 65 active particles out of130;
simulation time advances from 13.84272 to 43.89157. Sample positions and opacity
change, and its 16-frame texture atlas is resident. Thus this emitter is already
simulating: its absence is the triangle-only importer, not stopped simulation.
WaterWheel emitters `PArray07`/`PArray08` also have 86/67 active particles (capacity
102), world-space coordinates, and resident `FXfireAnim04loop.dds` source; inspect
their palette/material semantics rather than classifying by that filename.
The separate forge `Flames` triangle is imported but currently has authored base
alpha zero. Do not override activation merely to make a flame visible.

Next: implement native particle geometry using these live inputs and audited
billboard orientation/size rules, preserving texture frame, RGBA and activation.
Also still needed: true opaque-depth SOFT fade/pre-palette discard, additive
blend parity, native effect lighting and full in-game appearance validation.

## Skyrim's own lights reach Remix — 2026-09-19

Until now the only light submitted to Remix was a hardcoded distant sun
(`RemixScene::Initialize`). Nothing else in the game illuminated anything: a
torch, a hearth, a forge or a magic effect was emissive geometry and no more.

That matters specifically for volumetrics. Remix's volumetric integrator is a
froxel grid that samples **lights**; an emissive surface contributes to a
surface's indirect lighting through the path tracer but never to the froxel
grid. So "effects use RTX volumetrics" cannot be satisfied by making effect
quads brighter — it needs the lights that accompany those effects to exist as
Remix lights.

`RemixScene::CaptureSceneLights` walks `ShadowSceneNode::activeLights` and
`activeShadowLights` (the same registry `LightLimitFix::UpdateLights` reads) and
`SubmitSceneLights` submits each as a Remix sphere light with
`volumetricRadianceScale = 1`.

The walk runs from inside `RemixNativeRender::WorldFrame::thunk`, not from the
submit path, and copies only plain values out — no `NiPointer`, no raw
`BSLight*`/`NiLight*` survives the call. Reading the arrays at submit time is
wrong twice over: after the world render they are no longer the render thread's
alone, and a loading screen has no world frame at all, so the capture must
simply not run rather than race the teardown. Taking `NiPointer` copies to
survive that was worse still, because the last reference then dropped on the
render thread and ran a `BSLight` destructor the game had not asked for.

### Radiance conversion

Taken from Remix's own legacy-light math in `rtx_light_utils.cpp` so Skyrim's
lights sit on the same scale as anything Remix converts itself:

```
radiance = (endDistance^2 * kNewLightEndValue) / (pi * r^2)
```

with `r = 4` world units (`rtx.lightConversionSphereLightFixedRadius`'s default)
and `kNewLightEndValue = 0.01`. Remix has to solve `endDistance` out of a D3D9
attenuation curve; Skyrim states it directly as the light's `radius.x`, so the
conversion is closed-form. `fade` scales the intensity and not the distance —
a dimmed torch still reaches as far — so it multiplies the radiance afterwards.

### lodDimmer is zero for interior lights

`BSLight::lodDimmer` is the renderer's distance fade and is only maintained for
lights the exterior LOD pass touches. Interior lights sit at exactly `0`, so
multiplying by it unconditionally — which is what `LightLimitFix` does, reading
it from inside the render pass — deletes every torch and hearth in the game.
Measured in the Sleeping Giant Inn: 6 active + 2 shadow lights offered, **0
submitted**. Treating `0` as "not LOD managed" gives 8 submitted and an interior
that is lit. Outside, the Riverwood forge light reports `lodDimmer` settling to
`1`, so exterior fading still applies.

### Identity

`NiLight*` is the only stable per-light identity available; the handle is a
64-bit mix of it tagged with `0x4C` in the top byte, which cannot collide with
the sun's `0x4353...`. Re-describing a light under the same handle preserves
Remix's `stableIdentity`, so RTXDI reservoirs and temporal reuse survive the
update; a light whose position and radiance are unchanged is not re-described at
all. A `NiLight` address reused after a cell change costs at most one frame of
temporal reuse.

### Measured

Riverwood exterior (the canonical pose) offers one light — the forge — which is
correct: Skyrim exteriors in daylight are lit by the sun and ambient, and the
count is identical with world suppression off, so this is the game's list and
not an artefact of the integration. The Sleeping Giant Inn offers 8, of which 5
are re-described per frame (the fires flicker through `NiLight::fade`).

### A heap overflow in the runtime's light mapping

`LightManager::prepareSceneData` sizes `m_lightMappingData` to
`currentActiveCount + previousActiveCount` and then, for every light whose
buffer index is not `kNewLightIdx`, writes
`m_lightMappingData[currentActiveCount + light.getBufferIdx()]`. That index is
only in range when it came from *last* frame. Any light that skips a frame keeps
an older one, and the write lands past the end of the vector.

External lights hit this constantly, because the host stops submitting them for
the duration of a loading screen and resumes afterwards. The overflow is a CRT
heap write inside the Remix DLL, and Engine Fixes routes Bethesda's allocations
through the same heap, so it surfaces nowhere near the light code: the observed
crash is the game's shadow-map pass calling `BSShader::SetupTechnique` (vtable
slot 2, technique `0xc046`) through a `BSRenderPass::shader` whose vtable
pointer has been overwritten.

The fix bounds the write against `previousLightActiveCount` and treats an index
older than that as new, in
`.research/dxvk-remix/src/dxvk/rtx_render/rtx_light_manager.cpp`. On the plugin
side `DiscardFrame` now clears the capture and destroys the tracked lights, so
the old cell's lights are never resubmitted under handles Remix still holds a
buffer index for.

**This crash is not the scene lights.** With `CS_REMIX_NO_SCENE_LIGHTS=1`, which
disables both the capture and the submission and leaves no `[RemixScene.lights]`
line in the log at all, `coc` between the Riverwood exterior and the Sleeping
Giant Inn reproduces it with the identical signature: `0xc0000005`, `rdx =
0xc046`, and the same `SkyrimSE+0x1520588 / +0x1515042 / +0x155d17c` stack. A
40-second free-camera orbit at the Riverwood pose survives with lights on, which
is the other reported trigger.

Earlier A/B runs that appeared to implicate the light path were confounded: a
second session was building the same plugin from this worktree, deploying it,
and driving the camera in the same game process, so the runs compared different
binaries under different load. They are recorded here only so the conclusion is
not re-derived from them:

| `CS_REMIX_NO_SCENE_LIGHTS` | Cell transitions survived | Clean? |
| --- | --- | --- |
| `1` | 4 / 4, then 4 / 4, then 1 / 2 | no, no, yes |
| `2` (capture, no Remix call) | 4 / 4 | no |
| unset | crashed on the 2nd, then the 5th, then the 1st | no, no, yes |

The two clean runs at the bottom of each column are the ones that count, and they
agree: the transition kills the process either way.

The `m_lightMappingData` overflow above is still a real out-of-bounds write and
the fix stays, but it is not what this crash is.

## The forge fire animates and lights the volumetric medium

Both halves of "effect shaders animate correctly and use rtx volumetrics" were
measured at Alvor's forge in Riverwood, camera at (19900, -45200, 180), pitch
0.35, yaw 1.85, which puts the coal bed in the middle of the frame.

**Animating.** Four captures about three and a half seconds apart show the small
yellow flame at the front of the coal bed changing height and shape between
every one of them. A frame-to-frame pixel diff is *not* a usable test here: over
the same interval a control region of ferns and cobbles with nothing animating
in it changed more than the fire did (10.7 versus 5.6 mean absolute difference),
because path-traced sampling noise dominates. Look at the flame, not at a
number.

**Lighting the medium.** At Remix's defaults the medium is nearly transparent --
`rtx.volumetrics.transmittanceMeasurementDistanceMeters` is 200 m against a
transmittance of 0.999 -- so nothing it scatters is visible, which is why this
looked unanswered for so long. Setting that distance to 6 m makes the medium
dense enough to read, and then:

- `rtx.volumetrics.enable = True`: a warm halo stands above the coals and spills
  onto the posts and the underside of the eave.
- `rtx.volumetrics.enable = False`: the coals are a flat orange disc with
  nothing above them.

The difference is warm and local. Over the forge the medium gains
**+0.57 R / +0.34 G / +0.15 B**; over a control region away from the fire it
gains **+0.25 / +0.22 / +0.22**, neutral. A red-biased gain concentrated at the
fire is the fire's own light scattering, not a global brightening.

Both options were restored afterwards (`enable = True`, distance 200). The
density change is a diagnostic; nothing here ships with a modified medium.

## Effect shaders do not animate — 2026-09-19

Measured rather than inferred. `[RemixScene.effectAnim]` logs the numbers that
decide whether an effect moves at all: parameters 0/1 are the scrolling UV
offset, 2/3 the UV scale. Six effects sampled at Riverwood, including
`L2_whiteCrossStream04`, which is a waterfall and must scroll to look like
flowing water:

```
'INV_CloudDistant03_O:0'  uvOffset=(0.00000, 0.00000) uvScale=(1.000, 1.000)
'L2_whiteCrossStream04'   uvOffset=(0.00000, 0.00000) uvScale=(1.000, 1.000)
'CloudShape03_O:0'        uvOffset=(0.00000, 0.00000) uvScale=(1.000, 1.000)
'INV_CloudShape02_O_50:0' uvOffset=(0.00000, 0.00000) uvScale=(1.000, 1.000)
'INV_CloudShape03:0'      uvOffset=(0.00000, 0.00000) uvScale=(1.000, 1.000)
'CloudDistant02:0'        uvOffset=(0.00000, 0.00000) uvScale=(1.000, 1.000)
```

Those are exactly `BSShaderMaterial`'s declared defaults -- `texCoordOffset[2]`
at 0x0C initialised to zero, `texCoordScale[2]` to one. Nothing is writing an
animated value into the material by the time `Capture` reads it.

Confirmed independently from the image: two captures of Alvor's forge 3 seconds
apart differ by 2.8 (mean absolute, 0-255) in the cell containing the fire,
against 30.4 in the cell containing wind-blown tree canopy. The flame is a still
image; the trees move.

The likely cause is where the scroll is computed. `ReadEffectUVIndex` already
reads a global at `0x20d69d0` that `BSEffectShader::SetupMaterial` (0x15569e0)
writes, so the UV pair selection was already known to be produced by the game's
shader setup rather than by the scene graph. If the animated offset is computed
there too, then suppressing the world draws stops it being produced, and the
material keeps its authored defaults for ever. Settling that needs
`BSEffectShader::SetupMaterial` decompiled to find where the offset it uses
comes from; if it is derived from a controller plus a global clock, the host can
compute the same value itself without depending on the suppressed path.

Note that this is separate from whether effects reach volumetrics, which they
now do -- see the scene-lights section above. An unanimated flame still lights
the fog.
## Authored soft-intersection opacity — 2026-09-20

`Effect.hlsl` computes `saturate((sceneViewDepth - effectViewDepth) /
softFalloffDepth)` for SOFT permutations. SetupMaterial transports the reciprocal
depth scale in LightingInfluence.y; VS transports clip w / softFalloffDepth.
This fade multiplies alpha BEFORE PropertyColor.w and the grayscale-alpha palette
lookup. The authored distance already travels as native effect parameter14 and
SOFT as flag256, but the runtime previously did not evaluate them.

Both unordered resolvers now set a shader-local `nativeEffectSoftOpacity` before
material evaluation. Camera-ray gaps are projected using worldToView; secondary
rays use a ray-local gap as an approximation. Native-effect particles bypass the
generic camera-distance-based particle fade, including effects without SOFT.
Non-native particles retain the existing generic fade. No host API or packed
material layout changes, new textures, CPU readbacks or scene traversals.

Dedicated scalar tests cover the clamp endpoints, oblique projection, invalid
nonpositive authored range, and independence from absolute camera distance.
These do not prove GPU/native depth parity. Native SOFT's 0.003 discard (with its
FALLOFF/MULTBLEND exception), portal/PSR rays, ordered blending, native engine
lighting and gamma-space composition remain unqualified. The nearest RTX opaque
hit is used instead of sampling Skyrim's native depth texture; any disagreement
in their geometry/depth also changes this fade. Do not describe this as a complete
fix for bright dust beams, glass, or the user's exterior-cloud/LOD report.

# Live controller correction — 2026-09-19 23:13

The earlier suspicion of frozen native UV controllers does not hold for the
Sleeping Giant Inn flame meshes. PID23792 showed native UV changes on all six
`Flames:` shapes while imported parameters stayed frozen. `Capture`'s static
probe returned before the material-update code. Excluding effect meshes from
that probe in plugin `20260919-effect-live-updates` gives 6/6 changing imported
UVs in PID25364. This does not certify correct palette sampling, particle
movement, or consecutive-frame animation. Those remain separate defects/tests.

## Rainbow hearth sheets are refraction — 2026-09-19 23:52

Close captures 234332-fire-probe-before and 234941-fire-gpu-probe show actual
orange flames under three tall multicoloured sheets. The new native refraction
inspector identifies three Plane03:0 meshes with kRefraction, power0.1 and
VaporTileNormal_n.dds in both diffuse and normal slots. Both UV slots animate.
The importer treats these as ordinary opaque lighting materials, displaying
distortion data as diffuse colour. This is NOT evidence of broken flame palettes.
Implement Utility.hlsl's RENDER_NORMAL and ISRefraction.hlsl's displacement
semantics using Remix scene output; do not hide the meshes to remove the defect.
No refraction rendering fix has been deployed yet. See latest handover for
live hashes, GPU probe evidence and exact test scope.
