# Native directional light transport

## Revised balance, 2026-09-20 15:03

Deployed15:14 with backup label20260920-directional-one-third; PID49528 confirms
the one-third default without a process-local override. Plugin SHA256
5908DA4745DB86D3EF0ABE04CEE29525679A8778AA7139FD2DABC4A4317959F3.

User reports 0.2 is now too dark and requests one third of the original.
`DefaultRadianceScale` is now `1.0f / 3.0f`; the same value is applied live in
PID54228 through `cs.directionalLightScale`. Three CPU/API samples confirm
scale0.333333343 with fresh native direction and expected radiance. The
standalone scalar test passes13 checks. Exposure, sky, materials and point
lights are unchanged. This supersedes the0.2 balance below, not the underlying
native conversion or its parity limitations.

Before this request, frozen captures150157-sun-rebalance-02 and150202-...-04
compared0.2/0.4 at camera15133,-46900,650,pitch0.14998868,yaw0. Both inspected:
sunlit surfaces change, dense canopy remains dark. Normal auto-exposure was
left enabled; this is not a fixed-exposure radiometric measurement. Camera was
released afterward. One-third was applied after that pair, not part of that
frozen comparison. No claim that this fixes tree transmission.

## User-requested balance adjustment, 2026-09-20 13:15

Directional radiance now has a0.2 default multiplier AFTER the native conversion
below:80% reduction. It preserves native direction, RGB ratio, weather/time
variation and0.5-degree disk size. This applies to native directional lights
indoors too, not point/spot lights. Dome radiance, exposure and material inputs
are unchanged. This is an artistic sun/ambient calibration, not a newly derived
physical unit conversion or proof of vanilla parity.

Process-local test control `cs.directionalLightScale` accepts finite0..16;
malformed, negative, nonfinite and out-of-range values are rejected. No saved
settings are written. `directional.radianceScale` records the applied scalar;
reported radiance is the effective API input. CheckDirectionalLight.ps1 now
checks the native conversion times that scalar, and still checks provenance.

BuildDev24209 exit0, standalone CheckDirectionalLightMath13 scalar checks plus
scale boundary/nonfinite checks pass. Plugin88B7D119623CE2C982B607A0A3B404F99793CF8BCECCD8BCB0B8100F4CA6542D,
backup/symbol label20260920-directional-balance. PID28552 started13:14:45.
Three Riverwood CPU/API samples at0.2 passed. Frozen camera captures:
131542-directional-scale-1,131546-directional-scale-0.2,131550-directional-scale-0.1
in .research/captures, all identical reported camera. Normal exposure adaptation
remained enabled and allowed3.5seconds per capture; not a fixed-exposure ratio
measurement. Retained0.2. User reports "maybe better", but trees still dark.
Tree scattering/ambient balance remains open; do not claim trees or skin fixed.

The scene importer captures `shadowSceneNode[0]->sunLight->light` during the
native world-render boundary, alongside the point-light snapshot. It copies
plain values, never retains engine light objects across a cell teardown.
Capture includes the CS Present frame and cell ID. Submission refuses stale captures,
destroys the registered light when inactive, and uses a stable API hash when
updating direction or intensity. Loading-screen discard also destroys it.
Unchanged values do not recreate the API light description.

## Direction and intensity

`NiDirectionalLight::GetWorldDirection()` points along travelling light rays.
Remix's DistantEXT expects that same direction. Skyrim's shader light vector is
its negative, as demonstrated by `State::UpdateSharedData`. Normalize once,
without camera translation, handedness flips, or an interior-only override.
Interior templates may author a nonzero directional light without visible sky.

Source colour is `NiLight.diffuse * fade * HDR.sunlightScale`, matching the
shared shader-data producer in State.cpp. The conversion helper models a
unit-white Lambert surface at normal incidence:

- Linear Lighting disabled: `pow(max(source, 0), 1.6)`, from native
  `Color::IrradianceToLinear` / `SkyrimGammaToLinear`.
- Linear Lighting enabled and light already linear: `source / pi`.
- Linear Lighting enabled, gamma light: mirrors Lighting.hlsl's
  `llDirLightMult` and `Color::DirectionalLight`, with its pi cancelled by the
  diffuse BRDF normalization. Interior light does not divide out the exterior
  HDR multiplier before gamma conversion.

Remix `distant_light.slangh` samples radiance divided by `sin(halfAngle)^2`.
Integrating its projected solid angle on a normal-facing Lambert surface gives
pi times the supplied value; Lambert's 1/pi cancels it. Do not divide by the
sun's solid angle a second time. The 0.5-degree emitter diameter is retained
as an approximation: the native directional shader itself has no finite disk.

## Limitations and verification

Native gamma-space lighting applies its final power to the sum of lights,
albedo and angular response. Converting lights individually is not identical
for arbitrary normals, multiple lights, or shader-specific PBR compensation.
The conversion above is an explicit normal-incidence reference, not proof of
full image parity. Native fog attenuation and atmospheric disk/dome energy
separation remain to be qualified. The explicit balance multiplier documented
above is separate from this native conversion.

`communityshaders.inspect` with `kind=remixScene,filter=directional` reports the
captured native inputs, mapped API values, source/submit frame IDs and successful
submission. This checks CPU transport, not GPU buffer contents or shadow shape.
The CS frame counter advances at Present after capture/submission. Do not use
the direct CommonLib `BSGraphics::State::frameCount` field for 1.7.99: live
sampling found repeated/alternating float-like bit patterns, not frame numbers.
The world-camera freshness check uses the same corrected counter.
`tools/remix/CheckDirectionalLightMath.cpp` exercises the shared scalar helper
and independently integrates Remix's cone normalization.

## Live check, 2026-09-20

Release plugin B01EE281993E7A5A65520185D2F0E2156AF099C44865A5FE06F4169179011B00
was deployed overwrite-only with a backup, following deploy-and-test-game.
`CheckDirectionalLightMath.exe` passed 12 scalar checks. The new read-only
`CheckDirectionalLight.ps1` passed six Riverwood samples, three after an
inn/riverwood round trip, and six inn samples. It independently checks the
normalization and scalar conversion, same-frame/cell provenance, positive-light
submission and advancing Present counter. The first script run exposed a test
bug: PowerShell selected integer Math.Max overloads for a literal zero; explicit
double arguments corrected the test. No production brightness change resulted.

Riverwood direction and radiance changed with game time. Inn template direction
was (-0.86602545, 0, 0.50000006), radiance (0.21475491, 0.24179018, 0.29938203),
stable across samples. This is legitimately nonzero indoors, not a leaked sun.
The runtime logged dome active=0, instanced=0 indoors. All nine exterior-geometry
negative filters passed, with flames and heads present as positive controls.
One paused-console third-person cell round trip passed; this is not a door,
fast-travel, locomotion, frame-pacing or transient-loading-frame qualification.

Inspected captures 012633-native-sun-riverwood, 012733-native-sun-inn and
012750-native-sun-inn-above. Above-ceiling view now has black background without
the formerly lit grey plane. Hearth scene remains rendered and UI intact;
alchemy vessels still glow incorrectly. No claim of full image/lighting parity.
