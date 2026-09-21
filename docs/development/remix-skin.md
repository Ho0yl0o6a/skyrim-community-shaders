# Character skin diffusion — 2026-09-20

## Diffusion samples need actual surface separation, 18:53

Units were not sufficient. User stationary isolation183723/183726/183730/183733
showed shoulder patch survives transmission-off but vanishes with diffusion-off.
This isolates the visible patch to diffusion, not the through-volume transmission
term, in that view. Other big polygonal lighting boundaries remain.

RTXCR_EvalBurleyDiffusionProfile produces BSSRDF/pdf using sampled disk radius;
adapter previously reused that weight even if its ray hit a distant piece of
skin along the disk normal. Same-material acceptance did not constrain actual
separation beyond the much larger probe segment. Corrected adapter evaluates
the spatial profile after intersection, with actual3D separation, and combines
the two projection area densities. Sampling disk basis is now orthonormal to
the chosen shading/view axis. Reference principle: [PBRT spatial BSSRDF sampling](https://www.pbr-book.org/3ed-2018/Light_Transport_II_Volume_Rendering/Sampling_Subsurface_Reflection_Functions)
and [spatial BSSRDF definition](https://www.pbr-book.org/3ed-2018/Volume_Scattering/The_BSSRDF).

Using RTXCR's coefficient/albedo model, d=sigma_t*S(albedo), radial density
q(r)=.25*d*(exp(-r*d)+exp(-r*d/3)). Area density is q(r)/(2*pi*r).
Numerator is albedo*q(actualRadius)/actualRadius (minus RTXCR's optional single
scattering term when enabled). Denominator is the .5/.5 axis mixture of channel-
weighted q(projectedRadius)/projectedRadius times abs(NgHit.axis). Common2pi
cancels. A1micrometer world-scaled radius floor avoids the zero-distance
singularity. Bound remains local16gameunits from prior units fix.

Analytic Python tests verify flat identity, oblique-plane Monte Carlo energy,
world-unit invariance, and attenuation of10unit separation at.1unit projection.
Those are not GPU execution tests. Runtime build8541exit0;93totalPythonpass.
RuntimeF99FC94F5D814A60A15F04F5F6A547344B56CFAE562958B85AC3AA35649F156F
deployed, normalPID28764/hostA7AEA2A7. Details and capture paths in handover.
No complete/all-pose correctness claim: closest-hit/material-only selection,
alternative-axis occlusion PDF, cross-mesh diffusion still approximate.

Post-fix184754/184757/184800/184803 frozen comparison inspected: broad angular
shadows persist with diffusion off too. Not a matched old/new view; cannot claim
the reported shoulder leak eliminated in all views. Debug816185050 shows distant
underarm intersections with small projected radius; diagnostic printed pixels
missed skin, so no scalar probe results yet. Default debug0/printoff/unfrozen
restored. New grazing-angle darkening report pending object identification.

## Consistent physical distances, 18:32

User suspected Skyrim-to-meter conversion. Audit confirmed a dimensional bug:
RTXCR's SSS_METERS_UNIT=.01 produces meter-based radius/coefficients, while
the adapter supplied world-unit positions, traced thickness and integration
step sizes. Only maxSampleRadius was converted. Changed the shared RTXCR
material scale to include metersToWorldUnitScale, yielding coefficients in
inverse world units. Thus sampled diffusion radius, Beer-Lambert optical depth,
and sigma_s*stepSize all use the same units. No SDK modifications or geometric
rescaling. Host now sets rtx.sceneScale from bhkWorld::GetWorldScaleInverse/100.
Live log18:26:34 reports69.99125units/m, sceneScale0.699912.

Imported .5 scattering color and scale1 correspond to5mm (~.34996gameunits),
not the previous .005gameunits (~.0714mm). Parameters are still generic, not a
calibrated skin model. The 16-game-unit disk bound is now passed in meters via
GetWorldScale (~.2286m); previously16 exceeded normalized8-bit packing's input
range. That helper narrows to uint8 before min, so don't assert old saturation.
Decoded bound is about15.92gameunits. Disk basis/material-only hit acceptance
still need investigation; this change does not establish correct thickness rays.

Runtime213529A9F92FA7A11E37DAD2919B299251D0832F311A2E591F7C6650AD684C1D,
hostA7AEA2A7C059DF6AFD7A194766CF1210B483D11E43C11024799AC56977E3D0E6,
normal PID40260; build/deploy details in latest handover.88Python tests pass.
New test_skin_units.py checks optical-depth, sampled-distance and integrated
single-scattering-weight scale invariance plus source contracts. These are
analytic/source tests, not GPU parity tests. No shader unit layout changes.

CompareSkinUnits.ps1 captures corrected / coefficient-conversion-cancelled /
corrected-repeat with current actor frozen, then restores scale1 and unfreezes.
Actual useful body comparison in .research/captures:
-183010-skin-units-body-corrected.png
-183015-skin-units-body-legacy-coefficients.png
-183019-skin-units-body-corrected-repeat.png
All visually inspected; camera13735,-48155,-158,pitch0,yaw-2.07738137 identical.
Corrected hair/fern shadow edges on forehead/neck/arms visibly soften and repeat.
Black eyebrow band and larger polygonal shoulder shadows remain. This control
does NOT recreate the previous disk bound/global scale. Early comparison sets
missed skin; do not use182729/182822series as skin evidence.

No direct foliage/water/sun/tone changes. sceneScale is global and has NEE-cache,
particle and terrain consumers; those still need regression coverage. Native
albedo/depth/normal all-scenario parity and full character rendering unresolved.

## Native MSN preserved; arm transmission and eyebrow defects remain, 18:14

Runtime E5C2EC8F202152D61AC06B475E85F782DE05AB18D4E6F1FDE00826B910A5C6BC
preserves a loaded native model-space shading normal instead of getBentNormal,
like the already accepted native grass path. Geometry/ray-offset normals are
unchanged; generic materials retain bending. Build81928 exit0, log
build-skin-preserve-msn.log; backup/symbol label20260920-skin-preserve-msn.
82Python tests passed; no production setting/foliage/water/sun changes.

Matched-sample PID11228 captured pairs skin-msn-preserved-authored-1789923936
and-repeat-1789923940 at hostframes1079/1153. Authored-to-final normal max .0807deg
(buffer quantization), confirming the actual lighting normal now preserves the
authored result. Across393674skin-mask pixels, all-mask native anglep99=97.167deg
and depthp99=17.330units: NOT a parity pass.7995selected pixels differ in depth
by>1unit; native generally has foreground depth≈39–43 versus skin≈58–67.
Examples(1093,256),(1131,322),(1147,383),(1183,450) lie along the foreground fern.
This is a separate surface/coverage mismatch, not grounds to discard outliers.
Interior/depth<.1 diagnostic subset375865pixels has anglep99=4.595deg, versus
23.393deg in the earlier bent-normal pose. Restarts change pose, not strict A/B.
Reports testlogs/skin-msn-preserved{,-repeat}.json retain all results.

Second camera(13690,-48295,-148),pitch/yaw0, captures skin-msn-clear-authored-
1789924094 and-repeat-1789924098.242424pixels, native normalp50/p90/p99
.1446/.8333/3.1582deg; depthp99=.019737units. Authored-to-final max.0801deg.
Repeat agrees; reports testlogs/skin-msn-clear{,-repeat}.json. Extreme normal
outliers remain, no all-pose or all-surface certification. CaptureSkinBuffers
now accepts position/pitch/yaw, and -AuthoredNormals captures815twice.

Lit180146before and180604after inspected: angular skin shadows persist. Both
full-resolution diagnostic runs, but restarted pose differs. User supplied
I:/SteamLibrary/steamapps/common/Skyrim Special Edition/ScreenShot11.png showing
a black eyebrow band and angular torso/cheek shadows, then reported shadows
passing through arms / skin looking too thin. Do NOT call either defect fixed.

CompareSkinTransmission.ps1 temporarily tests defaults, transmissionfalse,
then diffusionProfileScale.1 (transmission stillfalse), restores1/true and
unfreezes. Use only freshly configured known-default test sessions; no getter.
Captures181217/181220/181223-skin-arm-transmission-* have identical camera
(13709.2051,-48170.6797,-126.988),pitch-.01791695,yaw-2.07738686. All inspected:
no clear removal of angular/leaking arm/chest patches in either test. Not proof
that transmission is correct everywhere, but does not support simply lowering
its strength as the fix. Session37341 exit0, freecamfalse/third restored.

Source findings to pursue separately:
- RTXCR_CreateSubsurfaceInteraction just copies its supplied basis. Current
  diffusion picks diskNormal=shadingNormal OR viewDirection, but supplies old
  geometry tangents unchanged. EvalBurleyDiffusionProfile places the disk using
  those tangents and offsets/raycasts using the new normal. Basis can be skewed.
  calcOrthonormalBasis already exists in utility/math.slangh. NOT FIXED YET.
- Initial imported skin parameters are generic radius(.5,.5,.5), scale1,
  maxSampleRadius16, albedo-derived transmission. RTXCR SSS_METERS_UNIT=.01;
  scattering coefficients use that, while disk bound multiplies normalized
  interaction maxSampleRadius by100*sceneScale. The interaction packs the bound
  through f16ToUnorm8. Audit units/range and actual sampled-hit distances before
  attributing the leak to thickness. No scene-scale changes were made.
- Census filter is case-sensitive: lowercase'brow' matched brown dust, not brows.
  Use'BrowsMaleHumanoid01' or'BSFaceGenNiNodeSkinned'. Player brows feature6Hair,
  alphaFlags4333(0x10ED),threshold128, importedMaterialtrue,hairCardsRequestedtrue,
  normalSpacefalse/nativeTangentFrametrue,1bone,88vertices,BSDynamicTriShape.
  Diffuse resident SRVformat77 exists despite empty texture.name; actual path
  textures/actors/character/malebrows/MaleBrow_1.dds,normalMaleBrow1_n.dds.
  Do not mistake an empty debug name for missing texture. Later transmission
  captures show brows without the large black band: investigate view-dependent
  alpha/hair-card/retained behavior, not just a permanent missing-map assumption.

Normal relaunch requested after tests; see newest handover for current process.

## Authored versus bent normal isolation, 17:59–18:01

Debug815 samples the actual MSN diffusion material normal map with its sampler,
same SurfaceInteraction UV/gradients and animated basis, before getBentNormal.
It encodes world normal * .5 + .5 in RGB and1 in alpha; other surfaces are0.
No production shading changed in this diagnostic build1757298020091F498334F8A1CB19C9908AAF00A0C3E329690D8D1E9295B4C722.
Build57168 exit0, backup/symbol label20260920-skin-authored-normal. PID40164
started17:58:53 with MatchCaptureSamples (fullres, zerojitter, native prep).

Paired captures skin-normal-unbent-authored-1789923590 and-repeat-1789923594,
hostframes1779/1859, same frozen pose/camera,23artifacts each. CompareSkinBuffers
accepts debug815's actual sampled-MSN diffusion mask and reports authored and
final normals separately. It retains nonunit/error outliers, rejects invalid
selected normals and requires exact analytic registration. Reports under
testlogs/skin-normal-unbent{,-repeat}.json. Both repeat results agree.

All393086mask pixels: final normal p50/p90/p99=.1431/2.3298/24.5942deg;
authored .1436/1.1627/4.5009deg. Over5deg fraction final5.768%, authored.771%.
Authored-to-final p99=24.0445deg. Authored encoded lengths p1/p50/p99
.999215/.999920/1.000312 (debug buffer quantization). Interior/depth<.1 subset
finalp99=23.393deg versus authored4.286deg. Extreme mismatches remain in both:
no full normal parity pass, no all-pose claim. Depth99th=.03063gameunits.
Known-albedo-scale corrected MAE(.002411,.001538,.001583), no saturated channels.

This isolates reflection-normal bending as a major source of native normal
disagreement, distinct from previously failed texture-derived vertex normals or
geometry-normal smoothing. A candidate now preserves loaded native MSN shading
normals as it already does grass; ray-offset geometric normals remain unchanged.
It also applies to diffusion hit shading, which previously bent authored normals
against the sampling-ray direction. Lit/shadow/specular behavior still requires
verification: unbent shading normals can direct samples below coarse triangles.

## Known albedo boost and invalid surface-light safety, 17:46

The earlier colour discrepancy is largely explained by the explicit initialization
setting `rtx.opaqueMaterial.albedoScale=1.53846153846` in RemixBridge.cpp (the
previously requested 1/0.65 brightness boost, documented in remix-vulkan.md).
This is an artistic adjustment, not evidence that diffuse assets are linear data
in sRGB views. Corrected that misleading source comment; retained the setting.
Grass has its own resolved colour path and bypasses this generic scale.

CompareSkinBuffers now accepts an explicit `--configured-albedo-scale`, never
fits it, preserves raw metrics and reports clipped channels. On the existing
same-frame393698-pixel capture skin-matched-normal-1789921681, scale1.53846153846
reduces pow2.2-native RGB MAE from(.10209,.06815,.05537) to
(.003773,.002386,.002331). No selected RTX channels are saturated. Report:
testlogs/skin-matched-normal-known-scale.json. Remaining differences/outliers
are real; this is not a native/Remix parity pass or all-material certification.

Source audit found evalNEEPrimary is called with an invalid surface light sample
when skin diffusion is enabled. Diffusion samples its own lights and can make
visibility.hasOpaqueHit false, continuing into computations using uninitialized
visibility attenuation/direction/hit distance and LightSample fields. Zero-weight
multiplication does not make those reads safe. The fix initializes absent data
and, after evaluating diffusion, accumulates only its scattering contribution
and returns when the surface sample is invalid. Valid-sample shading is unchanged.
It does not disable diffusion in shadowed/invalid-reservoir regions.

Two source-contract tests guard this branch and initialization; four numeric
analyzer tests cover the configured scale, no inferred scale, invalid scales,
and saturation. All77Python tests pass. These are not GPU execution coverage.
Runtime build16216 exit0, logbuild-skin-invalid-light.log. Built/deployed D3D11
DABF35382F2335147004CE9D5BBFC4A542A0EBDC2735AF776C867C98660438C2,
backup/symbol label20260920-skin-invalid-light.
Baseline captures174423..174440-skin-invalid-light-before use a fixed camera;
174440lit was inspected and angular shoulder/back shadows remain before the fix.
No assertion that the undefined-data fix resolves all angular shadows.

Post-fix PID38388 captures174751..174807 have the same camera transform, but
restart changed pose/lighting;174751 inspected and angular shadows still remain.
CaptureSkinBuffers produced classification1789922946 andvisibility1789922950
(19artifacts each). Approximately67.39% of372737interior display-mask pixels show
invalid surface lights in the next frozen-pose frame. Debugs are1080p while raw
Gbuffers are720p; no direct mask indexing across those extents is valid. Captured
noisy/denoised diffuse/specular, albedo/depth are finite globally, but output
sanitization prevents using that to certify internal math. Tests restored debug0,
freecamfalse and requested first-person. Idle vanity engaged by final175035
capture; game remains running, normalLaunchTest, no frozen state or active jobs.

## Sun-normal isolation and matched native buffers, 17:22–17:28

Diagnostic-only runtime807E2D36659228CA23391A64D65550EE39E1E8F54AB49B61317B9F9F01E2ECDC
retains accepted 1× foliage and all production lighting. Build75357 exit0,
backup/symbol label20260920-skin-sun-diagnostic. No skin shading fix claimed.
Debug813 compares geometry/shading normals with the first distant light:
red geometry rejects/shading faces; blue reverse; green both face; yellow both
face away; magenta no distant light. Debug814 stores signed cosines mapped to
RG[0,1]. GPU print emits geometryDotSun,shadingDotSun,geometryDotShading,count.
Extra light reads execute only for these two views.

PID39744 captures172204lit/172207debug813/172210debug814/172212debug15/
172215debug16/172217debug806/172220lit have identical frozen camera metadata.
Lit,813,15,16 inspected. Large dark regions include both-away normals, with
only narrow mismatched strips. Do not attribute the whole artifact solely to
the geometric-normal rejection gate. CaptureSkinSun.ps1 restores debug0,
freecam off and requests first-person. Prior shadow self-blocker evidence stands.

ProbeSkinSun.ps1, logtestlogs/skin-sun-probe.log, mask172511, internal1280x720:
at(760,140) geometryDotSun .267, shadingDotSun .299–.347; at(800,180) -.122/- .064;
at(830,140) geometry -.076 or -.117 (jittered triangle boundary), shading about
-.25; at(970,120) -.651/-.79. One distant light; geometryDotShading .86–.995.
No broad axis inversion at these points. Freeze/printing/debug restored.

PID50728 launched with -MatchCaptureSamples (native CPU preparation, full
resolution, zero jitter). Pair captures skin-matched-normal-1789921681 and
skin-matched-normal-repeat-1789921685 under .research/buffers,23artifacts each,
hostframes1981/2073. Both frozen at13655,-48295,-170.8372,pitch-.156536758,
yaw.079100654. Capture46557 exit0 and released freeze. CompareSkinBuffers.py
requires same-frame identity, unchanged camera, actual coincident primary ray
centres, matching extents and debug801's GPU diffusion mask. It does not fit or
discard outliers to obtain a pass. Four helper tests added;71Python tests pass.

Both reports (testlogs/skin-matched-normal{,-repeat}.json) agree:393698skin pixels,
normal angle p50/p90/p99=.1805/2.5266/30.7015deg,6.24%over5deg,1.04%over30deg.
Absolute depth p50/p90/p99=.00810/.01847/.04635 game units. Analytic primary-ray
displacement max2.486e-5px X/6.850e-6px Y, jitter[0,0]. This argues against a
global MSN axis/skinning-basis mismatch in this pose, not all-pose correctness.
Even interior/depth<.1 subset has p99=21.47deg; no normal parity certification.
Albedo raw-native MAE≈(.174,.191,.192), power2.2-native≈(.102,.068,.055).
Those are separate colour-domain hypotheses, neither assumed correct; a real
albedo/material mapping investigation remains. RTX-only material mask cannot
establish native material identity at every boundary. Restore normal launch
after this diagnostic; next-session details in handover.

## Direct-light isolation, 12:57–13:06 (diagnosis in progress)

PID9608, same save and frozen `tfc 1` pose/camera as the previous experiments.
Temporary options were restored after each group. Captures in .research/captures:

- 125705-skin-light-no-rtxdi: angular dark patches persist with fallback RIS.
- 125709-skin-light-no-direct: large polygonal patches disappear without direct light.
- 125903-skin-direct-diffuse and125906-skin-direct-specular: raw/noisy direct buffers
  both contain matching black polygonal regions. This is not solely denoising.

This narrows the investigation to direct lighting, but does not yet distinguish
invalid light samples, shadow visibility, or BRDF evaluation. Nearby feature5
scene census showed third-person body/hands/feet submitted, first-person parts
app-culled and NOT submitted. It does not establish GPU retirement correctness.

PID9608 exited on VK_ERROR_DEVICE_LOST at13:05:45, BEFORE any new deployment.
Original deployed D3D11 was46A77FF560575E8CCC2DE091C2A20456DA6984076C3BB0726B438B761E9378B5.
Frozen game had debug0, RTXDI/direct-light/diffusion defaults restored. Runtime
and CS logs archived under .research/testlogs/20260920-pre-visibility-device-lost-*
and gpu-crash-2026-09-20-13-05-45.nv-gpudmp copied there. Cause undetermined;
do not attribute to diagnostic source that was not yet deployed.

Opt-in debug views805–807 distinguish invalid/blocked/visible samples, blocker
distance and same/other surface. Not a lighting fix. Build89722 exit0; initial
build75275 failed on a duplicate declaration from the committed-hit macro,
corrected before deployment. D3D11 deployed13366B93F8F6508DD5C3629EB16D1CAB0E203D73DF8C6598BFFE583E6E6B8033,
backup/symbol label20260920-direct-visibility. PID11236 started13:08:43.

131021-visibility-ris-806 shows the angular body patches as hits within0.1–1
game units (red/yellow), some1–10(cyan).131303-visibility-blocker-surface-body
shows matching large patches red: committed blocker surface equals primary
body surface. This establishes self-occlusion in this test, not its numerical
cause. View807 comparison is valid for diffusion/POM surfaces where primary
surface ID is retained; ordinary non-SSS surfaces have no such comparison.
Camera moved between131020/131021 and131023; the latter misses most of the body
and is NOT a useful body comparison.131303 reset the camera to the known pose.
Direct/RTXDI/debug restored to defaults, `tfc 1` toggled off, third-person restored.
PID11236 then exited normally through verified Console+qqq for host deployment.
43 Python regression tests pass; shader compile success is not a visual fix.

## Tested and reverted MSN shadow experiments, 12:52

Neither candidate fixed the conspicuous angular skin lighting. Both shader edits
were removed and the exact pre-experiment deployed DLLs restored. Do not repeat
these candidates as untested ideas, or cite their debug buffers as a visual pass.

1. Added three mip0 normal-map reads at triangle vertices inside
   surfaceInteractionCreate, using transformed UVs, RGB.xzy decode and each
   vertex's animated orientation columns. Normalized/sanitized the resulting
   directions and oriented them to the visible triangle hemisphere. Applied
   existing area/length-bounded calcShadowTerminatorOffset. Also carried the
   computed offset separately through SssTracingResult into the diffusion
   visibility ray only; transmission retained its previous origin handling.
   This latter transfer is absent in the original implementation. The source
   omission is real, but this experiment did not establish a useful visual fix.
2. Extended the above to use the interpolated texture-derived vertex normal
   through getBentNormal as geometryNormal, matching the ordinary smooth-mesh
   path. Debug15 confirmed smooth character interaction normals on the GPU.
   Large angular patches still visible in lit124818. Removed this candidate too.

Candidate1 D3D11 SHA25640D3229559654CE9A0D36AC174A1A3F302C5E3051A26194025A7F98A343155D2,
backup/symbol label20260920-msn-terminator. Build22627 then26314 exit0 (second build
embeds the diffusion-transfer edit). PID53468 started12:36:36. Candidate2 D3D11
9490B53E1BED0F537595F6E98322C27BFAA46D6F6D6A5D02CA4EA5C491300473,
label20260920-msn-smooth-normal, build57050 exit0, PID15792 started12:46:31.
Build logs and CS logs are in .research/testlogs/20260920-msn-*.log.

Existing Riverwood save, third-person then `tfc 1` to freeze actor pose. First
camera(13689.9033,-48295.2461,-170.8372), then unobstructed camera
(13655,-48295,-170.8372). Drive inputs pitch.157043/yaw-.0800914 read back as
pitch-.156537/yaw.079101: do not assume the tool's input Euler convention matches
camera.get. All following capture names are in .research/captures:

- 123747-msn-offset-on / 123751-msn-offset-off: same-pose toggle of existing
  rtx.shadowTerminator.enableOffset. No clear improvement in angular patches.
- 123824-msn-offset-debug: debug279 shows nonzero character offsets (GPU evidence).
- 124007-msn-clear-on / 124045-msn-clear-off: unobstructed comparison; patch persists.
- 124048-msn-clear-no-diffusion: disabling diffusion changes shading in the back
  crease, but large polygonal boundaries remain. Diffusion restored immediately.
- 124130-msn-clear-shading-normal / 124132-msn-clear-albedo: actual debug16/23 buffers.
  Per-pixel normals appear detailed and smoothly varying over much of the body;
  no proof of full native normal parity. Large lit boundaries are absent in albedo.
- 124818-msn-smooth-lit / 124820-msn-smooth-geometry-normal: candidate2 lit/debug15.
  Normal field changes as intended; lit artifact remains. Restart changed the
  idle pose, so candidate1/2 are NOT exact pixel-matched comparisons.

The three captures named msn-side-* missed the character and are NOT character
test evidence. Ordinary screenshot notifications appear in some diagnostic images;
do not treat those overlays as renderer corruption. Existing offset toggles also
affect non-character objects, so scene-wide A/B differences are not MSN-only.

Both test processes exited normally via Console+qqq. Restored D3D11 SHA256
46A77FF560575E8CCC2DE091C2A20456DA6984076C3BB0726B438B761E9378B5 and unchanged DXGI
066D721569675B2172754F23A40DA3044E873B8B167A628A3534DA4780EF6788. No host changes.
Current PID9608 started12:51:39 with normal launch/configuration and same save;
no freeze, quality override or native reference. No saves or persistent settings
written; water/grass/aniso untouched. Revert build5019 subsequently finished exit0
at12:53; local build outputs now reflect restored source. The game runs the exact
verified pre-experiment backup, not either candidate. Capture125326 inspected:
third-person/freecamfalse, world and HUD visible. Input.IsKeyPressed17=false,
native14draw/75compute suppressed at12:53:52. No active builds or tool sessions.

43Python tooling tests and two existing dedicated unit tests (skinning_basis,
native_basis_vertex) passed. These are regressions, not tests proving the new
shader math or appearance. Full runtime shader builds succeeded for both candidates.

Next: isolate lighting/visibility versus normal bending with actual buffers/rays.
RtxdiApplicationBridge gates light candidates by geometryNormal, but the failed
smooth-normal experiment does not establish that this gate caused the artifact.
Likewise it does not rule out getBentNormal or self-intersection more generally.
Do not keep extra per-hit texture reads solely because the hypothesis sounds good.

## Remaining angular shadows: investigation 12:29

Native comparison captures show smoother-looking shoulders than Remix, but are
not a lighting/pose-controlled pixel comparison. Current restored Remix capture
`122813-native-comparison-restored-remix.png` still shows angular dark skin areas.
Do not claim a fix or attribute every dark patch to the same cause.

Source audit: `surface_interaction.slangh` modelSpaceNormals branch reads three
deformed orientation columns, assigns the triangle geometry normal, and skips
the ordinary smooth-vertex shadowTerminatorOffset calculation. This is a candidate
for self-shadow artifacts, not a confirmed cause from an A/B test.

Live feature5 census in PID40220 found nearby body/hands/feet MSN meshes (including
MaleUnderwearBody:0) with vertexDescriptor `0006300065000409`: flags0x63 contain
position/UV/colour/skinning, but NO VF_NORMAL or VF_TANGENT. Thus simply preserving
the public hardcoded vertex.normal would pass the host's default(0,0,1), not an
authored smooth normal. Runtime createMeshInternal writes the MSN orientation
basis into NativeBasisVertex; its columns must not be treated as vertex normals.

A valid correction needs independently obtained smooth normals (e.g. carefully
validated texture-derived normals or geometric reconstruction), including skinning
and seam handling, before reusing the existing terminator-offset formula. No
production edit/deployment was made on this hypothesis. Accepted grass/water/aniso
paths remain unchanged. Character shading and animation remain open.

## Implemented, not visually complete

FaceGen and FaceGenRGBTint now use the runtime's RTXCR diffusion-profile material.
Model-space normals alone do not classify skin. Existing FaceGen colour baking,
RGB normal import and animated model-space bases are preserved. Hair remains a
hair-card category, not thin foliage or skin. Dynamic facial mesh replacements
retain both the material and its successful diffusion-import diagnostic.

`csRemixCreateSkinMaterialD3D11V1` imports a resident, authored-space transmission
texture. The host supplies its composed FaceGen albedo. The runtime rejects sRGB
transmission views because diffusion sampling performs a shader gamma decode;
otherwise the colour would decode twice. Existing V3/V4/foliage contracts and
8x anisotropic sampling are unchanged. Radii/scales are checked finite, positive
and half-representable. Vulkan views are retained by the runtime material, not
native engine SRV wrappers across streaming.

Initial radius RGB=(0.5,0.5,0.5), scale=1, max sample radius=16 are the existing
Remix/Aperture material defaults. They are NOT measured Skyrim skin parameters.
CS `Skin.hlsli` uses a normalized thickness/transmittance approximation from
`1 - skinsk.x`; `_sk` RGB is not a physical mean-free-path map. Its mask, physical
radius calibration, skin-specific roughness/specular response and surface scar
overlays remain to be evaluated. Do not claim native skin lighting parity.

## Secondary-hit normal defect

Initial lit capture112715 showed conspicuous low-poly shading after diffusion
was enabled. `traceSssRay` in `algorithm/rtxcr/rtxcr_material.slangh` used
`surfaceInteraction.geometryNormal` for secondary-hit lighting, bypassing normal
textures and the animated MSN basis. It now evaluates the opaque material at
the hit and uses its shading normal. Geometric normals remain in the minimal
interaction for ray offsets. The shared result also supplies normals to the
transmission boundary calculation. A miss now returns before indexing surface
and material buffers. All four direct-integration shader variants compiled.

113252-skin-diffusion-normal-fix.png shows smoother shoulder shading; angular
shadow details remain. Different idle poses/light time mean this is qualitative
evidence, not a matched pixel or complete character-normal pass.

## Live evidence

Current test PID39504, launched11:32:06, normal quality/sampling. Existing Riverwood
save, third-person/freecamfalse, nativeReferencefalse/suppressWorldtrue. No saved
settings changed. Native suppression14draw/75compute at11:33; UI visible.

- `.research/captures/112716-skin-diffusion-gpu-mask.png` (initial runtime) and
  `113345-skin-normal-fix-gpu-mask.png` (corrected runtime), debug801, inspected:
  exposed player skin is green (diffusion), hair/nearby scenery red (not diffusion).
  These are actual GPU classification, not a host-only flag. Normal debug0 restored.
- `.research/testlogs/20260920-skin-normal-fix-routing.json`:18 query samples over
  frames1092..1148. FaceGen15matched/15active, RGBTint41/39active, Hair29/26active.
  Complete bounded censuses; active skin imported as diffusion, hair excluded.
- `.research/testlogs/20260920-skin-normal-fix-hair.json`: hair routing regression.
- `.research/testlogs/20260920-skin-viewmodel-routing.json`:12samples, first/third
  camera/category lifetime passes. Does NOT prove animation or first-person image.

Deployed CS A71A0444F14BA4BF030D548638CEB89B071084360EB7FB868F3D7CC9BE063C36,
backup/symbol label20260920-skin-diffusion. Runtime D3D11
2683F2CD4049447D1D89E232FDEBE995F41401FF717D1737B0BC5ACA42823FB6,
label20260920-skin-diffusion-normals. DXGI remains
066D721569675B2172754F23A40DA3044E873B8B167A628A3534DA4780EF6788.

Animation jitter, calibrated skin appearance and the full goal remain open.
Grass normals, water and anisotropic filtering remain user-accepted; do not
reopen them as a consequence of character experiments.
