# Thin foliage material transport

## Strength returned to 1× — 2026-09-20 (deployed)

After accepting the corrected transmission, the user requested 1× brightness.
The shared angular response now has no extra multiplier (previously 5×), for
both visible foliage SSS and intervening-card transmission. Linear soft/back
map weights, tint-only transmission and the RTXDI payload flag fix remain.
Grass colour/normal formulas, sunlight at one-third and tonemapping unchanged.
Runtime9150B1D2... deployed; build, dedicated1/1 unit test and67 Python checks
passed. This section supersedes the historical 5× settings below.

## Soft/back maps as linear lighting weights — 2026-09-20 (deployed)

Deployed9DFB920E... PID15944; build, dedicated1/1unit test and67Python checks
passed. Matched-camera170214/170218/170223 and close170329/170334/170338 show
clear canopy illumination with linear authored tint-only transmission; including
diffuse again on every intervening card restores excessive darkening. Kept tint
alone (foliageDiffuseTransmission=false) and5x response. Source-colour probe and
same-run visibility A/B support this change, but all-foliage/buffer parity and
full goal remain unverified. Details and current session in handover17:03.

Lighting.hlsl1322 converts diffuse through Color::Diffuse, but1516/1525 sample
back/rimSoft textures directly. LightingEval.hlsli138/146 uses them unchanged,
including when Linear Lighting is enabled. The candidate preserves that division:
linearize the derived diffuse colour, then multiply by the raw soft/back weight.
The earlier linear(base*weight) formulation also gamma-decoded the lighting weight
in LL-off imports, suppressing it relative to the LL-on path. Shared CPU/Slang
nativeFoliageLightingChannel covers both paths. Grass still uses its derived
albedo squared; there is no change to the5x angular multiplier, sunlight or tone.
This is a linear-renderer mapping of the native shader's lighting factors, not
a proof of exact equivalence to Skyrim's nonlinear gamma-space light summation.

GPU input probe809 (debug only) displays raw soft-map RGB for non-grass native
foliage. It also cycles selected-pixel GPU prints, w=0 raw diffuse,1 raw soft,
2 linear soft product,3 rolloff/gamma/diffuse scale. ProbeFoliageInputs.ps1 uses
internal render coordinates, restores printing off/debug0, and does not save
settings. Different phases occupy different jittered frames; do not treat them
as a single pixel-identical equation evaluation. Captures165600/165602 and GPU
records at(280,290),(310,330),(1130,170) established raw soft≈0.16–0.32 versus
prior linear products≈0.0002–0.0035 on green pine leaves. Both live SRVs were
DXGI77(BC3_UNORM), not hardware-sRGB. This rules out hardware double decode for
these materials, not every asset. Candidate build/deployment is in the handover.

## RTXDI reduced-reader repair — 2026-09-20

The custom RAB_GetGBufferSurface reader copied SharedSubsurfaceData but left
interaction flags zero. It consequently interpreted the native soft/back/rolloff
payload as generic thin optics. In particular, soft-only trees have zero in the
back-colour slot that generic optics treats as single-scattering albedo, giving
zero back-facing target PDF despite a nonzero native soft-light response.
The current-frame reader now reconstructs the native flag from the source
material with the same thin/maps gates as the payload producer. No new buffer
binding, texture read, layout change or multiplier change. Build and67 structural
tests pass; in-game qualification is recorded in the handover.

The transmission-only experiment is also currently in the runtime: non-grass
visibility uses authored soft/back tint without diffuse multiplication. Visible
shading and grass remain unchanged. Process-local diagnostic option
rtx.debugView.foliageDiffuseTransmission=true restores diffuse multiplication;
false is the experimental default. 163540/163544 matched-camera captures show
the dense cores still dark, with no compelling large improvement. Do not claim
that tint mapping alone fixes the problem; reassess after the reader repair.
No separate shadow term, tonemapping or sunlight adjustment was introduced.

Debug803 now previews native grazing plus full-back projected response times pi
(including5x), not just the old back-colour payload. Non-native thin materials
still show single-scattering albedo. It is not a raw native SSA diagnostic.

## Native angular response — 2026-09-20

The requested5x version is now built and deployed (handover16:23). Exact native
angular equations and5x scaling passed the shared CPU tests; runtime visual
qualification is ongoing. The1x native response visibly improved tree branches,
but did not fix the dense dark canopy. No separate soft-shadow path was added.

Update16:17: native1x version built/deployed and its on/off/restored canopy test
shows stronger greenbranches, but densecore staysdark. Usernowrequests5x, after
an undeployed2x request. The nativeFoliageResponseChannel helper applies that5x
uniformly to soft/backresponse; visibility clampsscaledresponse0..1.5xbuild/test
are inprogress; seehandover. Userexplicitly saysnotto pursuea separate shadow
term; no suchpathhasbeenimplemented. Olddebug803showsbackcolouronly undernew
payloadandisNOTacomplete nativeSSSstrength diagnostic; correctbeforeusingit.

User explicitly prefers matching Lighting.hlsl/RunGrass.hlsl over physical
absorption. The intermediate Beer-Lambert candidate was built but never deployed;
its helper and native use were replaced. Generic non-native thin visibility keeps
its existing approximation. Do not describe this candidate as measured optics.

Native foliage now retains separate soft/back colours and rolloff. The shared
CPU/Slang nativeSoftLightMultiplier transcribes both smoothstep polynomials from
GetSoftLightMultiplier, evaluated at the actual N.L, not just grazing incidence.
Back lighting uses saturate(-N.L). Grass retains the derived albedo squared and
saturate(vertexAlpha*10)*SSSAmount*2 rolloff. Tree colour products remain native
base*soft and base*back. Conversion to linear occurs once before light evaluation.

Interaction flag bit2 identifies a native response payload in existing SSS data:
packedTransmittanceColor = R11G11B10 soft colour; packedSingleScatteringAlbedo =
R11G11B10 back colour; measurementDistance = rolloff+1; maxSampleRadius remains0.
The +1 preserves thin classification when rolloff is0. This is NOT an optical
distance. Flag survives the existing six-bit G-buffer flag mask; buffer strides
and CPU material records are unchanged. Only native thin/maps-enabled hits use
this encoding; generic thin and diffusion materials retain their usual payload.

Direct exact and approximate evaluation and indirect sampling use the same native
response. Front-hemisphere wrap is added to diffuse reflection; back-hemisphere
response uses diffuse transmission. The polynomial is already projected: no extra
cosine, Fresnel or Henyey-Greenstein/1/(4pi) factor is applied to this native term.
RTX's linear diffuse normalization is1/pi; this is not proof of complete LL-off
gamma-rendered raster parity or native shadow/ambient equivalence.

For shadow rays, the native back-facing response is extended to per-card coloured
visibility (clamped0..1, without1/pi), evaluated against the flat triangle plane.
This is an artistic ray-traced extension: native raster code has soft-shadow
visibility, not per-card coloured ray transport. It still requires visual testing,
especially stacked cards and soft-only foliage at normal incidence. Trunks remain
opaque. Accepted visible grass shading normals are unchanged. A local triangleNormal
field in SurfaceInteraction does not change any serialized G-buffer layout.

Traversal also forces thin instances nonopaque (including opaque micromap regions)
and requests no duplicate any-hit candidates. This was deployed separately first;
155538-thin-traversal-canopy.png still shows a very dark canopy, so traversal alone
is not a demonstrated canopy fix.

## Native colour flag contract — 2026-09-20

The imported-UNORM colour flag now occupies bit10 (type-relative offset8) within
the existing uint16 material flags. Bit9 remains native foliage; bits14/15 remain
landscape/effect. The material record is still112 bytes. A C++ static assertion
guards this flag's width; test_material_flag_width.py checks every shared opaque
flag fits and does not overlap. test_material_layout exercises the actual GPU
record write with the flag set/cleared and verifies the adjacent sampler index.
Its pre-fix run fails with "Native albedo flag lost in 16-bit GPU field".

This repair must be used with the single-decode cleanup below: activating the old
early-decode branch would introduce actual double decoding. Default native scale
is1, so packing the flag is not intended to brighten the canopy. The GPU test is
a reversible nativeAlbedoScale1/0.5/1 comparison via CompareNativeAlbedoScale.ps1.
Final build, unit and GPU results are recorded in the newest handover checkpoint.
Hardware-SRGB imported views and material-merge colour metadata still need a
separate audit; this flag specifically describes gamma-authored UNORM imports.

## Rejected tree-colour candidate — 2026-09-20 14:23

Tested using linear(softTexture)*softWeight + linear(backTexture), omitting the
native diffuse-albedo factor for non-grass foliage only. Runtime built/deployed
successfully; debug803 showed stronger coloured scattering, but the lit canopy
remained very dark. Same-camera within-run maps-on/off and thin-on/off captures
are 142230 through142245-tree-scattering-colour-* in .research/captures.
Compared with the prior runtime's141344/141352 images, the camera transform is
identical but restart/wind timing differs; not pixel-identical A/B evidence.
Candidate REVERTED: no compelling canopy fix and it changes CS's authored colour
product. Grass equation was untouched. Do not reinstate simply as a brightness
adjustment. The optical mapping remains an approximation requiring qualification.

Source lead, subsequently DISPROVEN as the current darkness cause:
opaque_surface_material_interaction.slangh first applied
gammaToLinear at the NATIVE_SRGB_ALBEDO sampling branch (around930), then applies
gammaToLinear again in the ordinary non-grass/non-effect path (around1042).
Both calls are pow(x,2.2), not identities. An imported unorm0.5 channel therefore
becomes about0.035 before final scale, versus0.218 for one conversion, absent
other operations, IF that branch were active. SRGB imports skip the first software call but can still be
hardware-decoded before the later call. Need live view-format and raw-buffer
evidence; do not fix this with a global multiplier or assume every import has
the same domain. Terrain blending, baked skin, vertex colours, legacy/replacement
materials and native grass/effects must be considered separately.

14:35 correction: live pine diffuse/soft views are BC3_UNORM77, bark BC1_UNORM71.
However, NATIVE_SRGB_ALBEDO is 1<<(2+14), bit16, while BOTH CPU writeGPUData's
flags and OpaqueSurfaceMaterial.flags are uint16_t. It is discarded before GPU
sampling. Thus the apparent first software decode NEVER ran for these records.
Images143113-before/143411-after confirm no material albedo lift from removing
it. Source now retains one final decode, with nativeAlbedoScale after conversion;
that scale is also inactive until the flag is corrected. This is latent-code
cleanup, NOT a demonstrated visual correction. Do not promote the duplicate
decode hypothesis to a diagnosed cause. Next audit/fix colour flag packing with
CPU/GPU tests and explicit native UNORM versus SRGB-view handling. Never widen
the shader's flags field without a complete material-layout audit.

## Tree baked AO diagnostic — 2026-09-20 13:42

CS avoids multiplying two AO estimates: Lighting.hlsl extracts vertexAO from
max RGB, Skylighting.hlsli divides skylighting visibility by it, and
DeferredCompositeCS divides SSGI AO by it using Masks2. TruePBR also exposes
vertex-brightness normalization strength. These raster compensations are not
implemented by multiplying raw native vertex RGB into Remix albedo.

Process-local cs.treeVertexBakedLighting (default false) enables Remix's existing
baked-lighting normalization for eligible full-tree lighting shapes beneath a
BSTreeNode, including bark. It does not change source buffers or tree animation
alpha. Selection is cached at mesh creation; toggling dirties retained instance
descriptions even if static-probe capture would otherwise skip them. Inspector
vertices: reports treeVertexColor and vertexColorBakedLighting.

Matched lit/albedo and near-canopy A/Bs confirm colour lift but persistent black
leaf undersides. This is NOT a full tree-lighting fix. Tested tint-strength1
(normalized chroma retained) and Remix default0.6 (mix toward white); restored
diagnostic false and strength0.6. No shader/runtime change. Exact captures,
deployment hashes and restoration are in remix-handover.md13:42. Next isolate
transmission/visibility; do not assume vertex AO alone explains the darkness.

## Grass reflection colour — 2026-09-20

Grass reflected albedo now uses the same `nativeGrassAlbedo` derivation as its
scattering tint. It reads the original
SRV sample and saved vertex colour: Color::Diffuse with the active native gamma
and multiplier, normalized ColorToLinear(vertexRGB), and BasicGrassBrightness
unless the complex atlas opts out. It does not apply the generic imported-albedo
gamma conversions or multiply in vertex AO again. Alpha tests and opacity remain
on the existing path; no normal, exposure, optical or animation change is implied.

The shared C++/Slang `native_foliage_math.h` channel equation is covered by
`test_native_foliage`: active/inactive gamma, uniform AO invariance, complex atlas
brightness opt-out/override, zero/near-zero vertex colour and white override.
Dedicated unit build passed1/1. This proves arithmetic, not native raster parity.
The host's existing generic albedoScale1.53846/bias compensation is bypassed for
native grass: applying it to already-resolved native colour clipped bright blades
in the first in-game test. The final material remains energy-clamped to0..1;
raw CS buffer parity is still unverified. Complex normals/sphere orientation now
have source paths and CPU tests (see remix-normals.md), but positive GPU coverage,
native AO lighting and GRASS_OPTIMIZATIONS LOD brightness remain
open. The earlier grass input transport alone did not fix reflected albedo.

## Native colour transport — 2026-09-20

`csRemixCreateFoliageMaterialD3D11V1` now carries separate native soft/back-light
SRVs, flags, rolloff, Grass Lighting brightness/complex threshold/SSS amount, and
the active Linear Lighting colour gamma/diffuse multiplier. Existing V3/V4 stay
ABI-compatible. Grass always qualifies; eligible two-sided alpha-tested lighting
cards qualify with TREE_ANIM, SOFT or BACK. Face/hair/MSN character materials,
terrain and whole-tree LOD atlases are excluded. TREE_ANIM cards without SOFT/BACK
keep the earlier V4 optical proxy rather than silently losing thin scattering.

Native flags: grass1, soft2, back4, disable terrain vertex colour8, override complex
grass brightness16, gamma-space CS colour32, grass sphere-normal64. Sphere-normal
is sourced from kEffectLighting exactly as Hooks' GrassSphereNormal permutation;
the V1 API rejects it without grass. Opaque record flag bit9 selects the new block, mutually
exclusive with effect/terrain storage. GPU offsets: soft/back indices64/66,
flags68, six float parameters72..95; record stride remains112. Parameters and
texture identities participate in material hashing/merge and CS invalidation.
Both authored views participate in material retain/release traversal.
No native COM SRV wrapper is retained, no per-frame texture readback/bake added.

Grass derives the CS colour using its complex-atlas diffuse half, Color::Diffuse,
normalized ColorToLinear(vertexRGB), and BasicGrassBrightness when appropriate.
The native vertex alpha now survives GPU grass deformation for
`saturate(alpha*10)*SSSAmount*2`; cutout opacity still uses texture alpha, not this
wind/rolloff channel. Atlas coordinates AND gradients are adjusted before the
opaque material read, including alpha tests. The bottom-left complex marker
uses the same source SRV read as RunGrass. Complex grass normal-map transport now
uses the lower half and CS cotangent frame; positive GPU sampling qualification,
GRASS_OPTIMIZATIONS mid/far LOD brightness and full raster parity remain open.

The native colour weight is applied ONCE to RTX's single-scattering albedo:
grass albedo squared; lighting base colour times sampled soft/back-light colours.
RTX's Hanrahan thin transmission does not additionally multiply by diffuse albedo.
Keeping colour in single-scattering albedo avoids treating the authored tint as
a nonlinear extinction coefficient. Neutral transmission0.5 at distance1 remains
an optical approximation, not measured leaf thickness. Soft rolloff is currently
mapped to its native grazing response `smoothstep(rolloff/(1+rolloff))`; BACK uses
unit strength. The actual angular lobe is RTX thin SSS, NOT the native polynomial.
Combined soft/back colour is clamped to an energy-bounded scattering albedo.
Thus native input transport is improved, but angular equivalence, foliage colour
under all lighting, coloured shadows and full visual fidelity are NOT established.

Debug view803 displays resolved thin single-scattering RGB (native-derived colour
or the V4 fallback's neutral0.5);800 shows thin selection. View803 is written in
geometryResolverVertexOutputDebugView after resolving the material interaction,
not conditionally inside material sampling where the primary-ray macro was absent.
Restore view0 after testing. Runtime thin-opaque enable provides a separate A/B.
Dedicated `test_material_layout` passes1/1, including112-byte guards, foliage
offsets/flags/parameter packing, identity and merge preservation. Standalone
CheckFoliageMaterial and optimized CS/runtime builds pass. Visual results belong
in the handover; compile/layout tests do not prove appearance.

## Native source equations

The host now marks native gamma-space colour with foliage flag32
(`NATIVE_FOLIAGE_GAMMA_COLOR`) when GetCommonBufferData reports Linear Lighting
inactive. Shader colour derivation still mirrors the CS shader using its sampled
SRV values, active gamma/diffuse multiplier, vertex normalization and brightness.
The fully derived material colour then crosses ONE explicit boundary into RTX's
linear working space: pow(abs(colour),2.2) for gamma-space CS, identity for LL-on.
This is not another unconditional decode of the texture. It applies to reflected
grass and to the grass/tree SSS colour products; soft/back angular strengths stay
linear and outside the colour conversion. The native buffer remains in its own
working space, so raw albedo parity comparisons must name that domain explicitly.
Flag32 affects material identity and GPU packing; old flag combinations retain
their previous interpretation. The V1 import accepts the additional bit while
requiring at least one actual grass/soft/back kind. No GPU stride/ABI size change.

This removes the implicit assumption that CS LL-off grass values are already
linear reflectance. It does not establish angular SSS equivalence, full native
image parity, SRV-format coverage or correct lit appearance. Live results and
deployment status are recorded in remix-handover.md; do not infer them from this
implementation description.

Lighting.hlsl's rimSoftLightColor comes from rimSoftLightingTexture(+0x60) under
RIM/SOFT; BACK samples specularBackLightingTexture(+0x68) separately. Preserve that
distinction and avoid classifying skin/hair as generic thin foliage. Not yet
visually qualified in all scenarios.

User further requires grass SSS inputs to follow CS Grass Lighting. Source audit:
RunGrass.hlsl377 and Lighting.hlsl583 use the SAME soft-light function:
`saturate(smoothstepPolynomial(saturate((rolloff+NdotL)/(1+rolloff))) -
smoothstepPolynomial(saturate(NdotL)))`, where the polynomial is x*x*(3-2*x).
Grass rolloff is `saturate(vertexAlpha*10)*SubsurfaceScatteringAmount*2`;
tree/lighting rolloff is LightingEffectParams.x. Grass's scattering tint is its
derived albedo (brightness/complex atlas selection and normalized linear vertex
colour included), then the final diffuse multiplication applies albedo again.
LightingEval.hlsli138 uses rimSoftLightColor for SOFT, and144..146 uses the separate
backLightColor with saturate(-NdotL) for BACK; Lighting.hlsl2631 applies BaseColor.
Thus the final grass SSS colour weight is albedo squared, while ordinary lighting
is rimSoftLightColor*BaseColor (or backLightColor*BaseColor). This describes the
native equations, NOT an instruction to blindly square the Remix transmission
texture: first account for Remix's own albedo/scattering factors to avoid applying
the colour twice again. RIM_LIGHTING alone is not transmission. User's expanded
goal also explicitly requires realistic tree SSS without dark uncoloured shadows.

## Earlier V4 optical proxy (retained only for unauthored leaf fallback)

Detailed leaf cards and native grass initially used RTX's thin-opaque material extension.
The host selects grass directly, or lighting geometry with TREE_ANIM, TwoSided
and alpha testing together. It does not infer vegetation from filenames. Live
Riverwood pine branch/shrub materials have these flags; bark lacks them. Whole-tree
LOD atlases mix bark and foliage and still require a separate classification mask.

`csRemixCreateMaterialD3D11V4` adds a resident transmission SRV to the V3 import.
V3 remains an ABI-compatible wrapper with no transmission SRV. A non-null
transmission view requires a thin-opaque extension with finite positive measurement
distance, not a diffusion profile. Deferred material ownership retains the Vulkan
image view; the game-owned COM wrapper is not cached. No texture copy/readback or
per-frame material resubmission is added. The normal/albedo/sampler and alpha-test
paths are unchanged.

The foliage proxy uses the source texture RGB as transmission coefficients,
reference distance1 game unit, isotropic scattering and single-scattering albedo0.5.
These are an explicit optical approximation: Skyrim does not provide measured
leaf thickness or volumetric optical coefficients. The thin-material texture path
reads the SRV's values directly (no additional gamma transform); this mapping is
not a physical measurement or native lighting-equation match. Fallback transmission
is neutral0.5 when texture maps are disabled. Diffuse albedo keeps its existing
independent colour conversion. No exposure, emissive boost or quality override.

`CheckFoliageMaterial.cpp` tests grass/leaf positives and solid bark, non-TREE_ANIM
cutouts, atlas/effect negatives. Dedicated runtime `test_material_layout` now also
checks API pNext conversion preserves thin thickness/colour and keeps ordinary
opaque materials non-thin. The test needs `remix_api_include_path`; it is run only
from `_Comp64UnitTest`. Build/CPU tests are not pixel proof.

GPU debug view800 is green for thin opaque and red for other hit materials. Use
it to verify leaf/grass selection independently of appearance, then restore view0.
Runtime `rtx.subsurface.enableThinOpaque` allows same-camera response A/B testing;
restore true afterwards. Animation/weather/exposure can vary between captures.

Still unqualified: TREE_ANIM camera-facing/distance normal correction, normal/depth
native image parity, authored soft/backlight texture mapping, general non-TREE_ANIM
vegetation, bark/leaf separation in distant atlases, and the optical proxy's fidelity.
