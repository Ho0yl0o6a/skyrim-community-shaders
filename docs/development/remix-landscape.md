# Native landscape audit — September 12

## Authored terrain basis transport — September 20

`csRemixCreateMeshTBNV2` flag8 combines the authored N/T/B basis with the six-layer
landscape material. Its 68-byte vertex stores position, nine basis floats, UV,
color and the original two packed weight words. The existing hit decoder reads
weights relative to the color offset, preserving interpolation-before-normalize.
The same API keeps ordinary/MSN layouts at 60 bytes without terrain overhead.
No material record, texture identity or per-frame CPU work changes. The terrain
normal branch now uses the same full authored N/T/B evaluation as ordinary
meshes, rather than substituting the adjusted geometric normal for authored N.

The API rejects skinned terrain and unknown flags; the V1 TBN export remains
available as a flag0 wrapper. Meshes without complete native basis attributes
retain the existing terrain import. Static API geometry takes the exact
interleaved GPU-copy path, not the generic three-component normal reinterleaver.

`test_native_basis_vertex` tests the actual packing helper for 60/68-byte strides,
consecutive vertices, MSN identity/authored basis, UV/color/position, and raw
weights (including sums over255 and the last two reserved bytes). Run
`CheckLandscapeRuntime.ps1 -RequireLayeredImports -RequireTangentFrames` for
material-import and bounded live plugin-basis checks. Neither is GPU/native
buffer-parity evidence; visual and GPU qualification remains required.

Six-layer near-landscape albedo/normal blending is now implemented and deployed.
The entire landscape is **not complete**: overlay/noise/distant blending,
specular semantics, LOD coverage and tree coverage still need work.

## Live evidence

`20260912-landscape-audit`, PID 32952 (started 02:13:29), loaded Riverwood through
SKSE/DevBench with native world suppression. Bounded diagnostics in
`RemixScene::Upload` captured twelve landscape uploads. Snapshot:
`.research/deployed-symbols/20260912-landscape-audit/CommunityShaders-audit.log`.

- All twelve have feature 19 (`kMultiTexLandLODBlend`), 289 vertices, 40-byte
  stride, and `VA_LANDDATA` at byte 32. Land data is eight packed bytes; the
  first six are blend weights. Several sampled patches use all six channels.
- Weight sums are not always 255: observed ranges include 255..330, 255..321,
  255..310, 255..571, 255..397, and 255..332. Preserve raw weights, interpolate,
  **then normalize**, as in the native fragment shader. Per-vertex normalization
  before interpolation would change these blends.
- All twelve have `MSN=false`. These near-landscape normal maps need the terrain
  tangent basis, not the animated character model-space basis. This does not
  prove the encoding of all LOD assets.
- `numLandscapeTextures` values include 3, 4, 5. The material has a base
  diffuse/normal plus five extra slots. Empty `NiSourceTexture::name` does not
  prove a missing texture: check the actual texture/resource view. Some named
  entries were `Textures\Landscape\Dirt02.dds`, others unnamed.
- UV scale/offset were (1,1)/(0,0) in these samples; preserve authored transforms
  rather than hardcoding this for other patches.

The pre-fix API importer dropped the extra textures and all six weights. This was
a concrete material defect independent of exposure; it does not explain every
missing tree, LOD seam, water surface or other unfinished scene feature.

## Source contracts inspected

CommonLib `RE/V/VertexDesc.h` and `RE/B/BSLightingShaderMaterialLandscape.h`
describe packed offsets and layer texture slots. `package/Shaders/Lighting.hlsl`
passes the six weights through the vertex shader and normalizes them after
interpolation. `Common/LightingLandscape.hlsli` blends the texture samples.

CERT's AGENTS.md was read completely. Its `shaders/GBufferRaster.hlsl`,
`shaders/raytracing/include/Geometry.hlsli`,
`src/Core/Material/Skyrim/LandscapeMaterial.cpp`, and
`shaders/include/Material/Skyrim/LandMaterial.hlsli` corroborate byte-packed
weights, texture ownership and weighted normal/albedo evaluation.

## Implemented direct-layer path

`csRemixCreateLandscapeMaterialD3D11` imports six diffuse and six normal views
without readback. A retained native payload holds the extra five pairs; its
ordered texture hashes and per-view sRGB mask participate in material identity.
All extra views enter Remix's normal bindless texture tracking. Null extra views
explicitly fall back to the base layer instead of reading an invalid descriptor.
This fallback is defensive; it does not prove missing authored assets are correct.

Native mesh flag 8 carries packed weights in `HardcodedVertex._pad0/_pad1`.
Both the fast geometry copy and compact CPU/GPU interleaver preserve these eight
bytes immediately after BGRA vertex color. The surface decoder interpolates the
six byte values, and the material evaluator normalizes the sum at each ray hit.
Terrain uses authored vertex RGB modulation and the native UV transform.

GPU material records are 112 bytes (seven uint4s), with ten additional uint16
texture indices and the sRGB-view mask. All polymorphic shader structures and
CPU writers, including translucent/portal/subsurface padding, use this stride.
Hardware-decoded sRGB layers are returned to authored space before blending;
the existing common albedo decode and /0.65 scale then apply once. Encoded XYZ
normal samples blend before the ordinary tangent-to-world normal path.

CS invalidates terrain material/geometry when its layer texture identities,
resource views or UV transforms change, not when the camera turns. This does not
yet cover every possible in-place content mutation or overlay/material parameter.

## Remaining requirements

Keep the implemented direct-layer path rather than replacing it with a
low-resolution baked atlas or duplicate translucent terrain. Native normal-alpha
and specular parameters are not yet translated into the PBR specular response.
Overlay/noise/LOD blending remains separate even after six base layers work.
Audit any in-place texture/material mutations not covered by the identity/UV
invalidation above, and verify the defensive missing-view fallback against
authored native behavior.

Verify multi-layer patches, matching albedo/normal/full-render views, continuity,
camera turns, movement/streaming and material-cache stability. Compilation alone
does not validate terrain.

## Previous diagnostic deployment

CS SHA256 `587119FF37D15B01EF2B97D6B930B7EE6D67034C61130C4FF65DE05D6C725D7B`.
Only the owned CS DLL was overwritten; prior DLL/logs are in
`.research/deployment-backups/20260912-landscape-audit`.
Runtime remains `20260912-grass-omm-reuse` from remix-vulkan.md. Its check passed:
22 batches / 54,147 placements, two wind samples (max error 0.00390625 units,
movement 1.57422 units, zero UV/alpha errors), 403 unique opacity triangles for
876,844 expanded triangles.

Actually viewed matched-camera diagnostics:
`CS_2026-09-12_02-17-12_097.png` (Diffuse Albedo, 23) and
`CS_2026-09-12_02-17-53_507.png` (Shading Normal, 16). Indices verified against
`rtx/utility/debug_view_indices.h`; raw diagnostic colors are not exposure-matched
final color references. Restored full render (0), freecam off and first person.
The test process's auto-vanity delay was increased through the console for
unattended testing; no saved INI was edited.

## Six-layer deployment and verification

`20260912-landscape-six-layer`, PID 43800, started 02:37:23 on September 12.
CS and all runtime/shader targets built successfully. Runtime patch regenerated
and reverse-checked. Only the three owned DLLs were overwritten, after backups
to `.research/deployment-backups/20260912-landscape-six-layer`; matching DLL/PDB
artifacts are in `.research/deployed-symbols/20260912-landscape-six-layer`.

- CS SHA256: `3966F5ED37DC6678051F58CED2BDBB0A80E75BBC7BD5A56C7C73D62A0270EDBA`
- d3d11: `A875E1EA4ACA8D511B837A2B2A8972E873129E90D2A0FD5AF4385AE1A86F3F6B`
- dxgi: `DFF19414A0DAA8FF9DC5151FE0A4422EF8C7E6D18D449270C9632BD3438EBA92`

Riverwood loaded through SKSE/DevBench. 100 landscape imports succeeded: 63 had
six diffuse/normal pairs, 20 five, 8 four, 5 three, 2 two and 2 one. These are
cumulative import counts, not an independent GPU census. Check with
`tools/remix/CheckLandscapeRuntime.ps1 -RequireLayeredImports`.

Actually viewed matching camera captures in the game's screenshots directory:

- `CS_2026-09-12_02-35-01_128.png`: old build, diffuse albedo (23).
- `CS_2026-09-12_02-38-46_761.png`: new build, diffuse albedo (23).
- `CS_2026-09-12_02-39-06_749.png`: new shading normals (16).
- `CS_2026-09-12_02-39-39_168.png`: new final rendering (0).
- `CS_2026-09-12_02-40-01_966.png`: final rendering after 180-degree turn.

Near terrain texture/normal detail changes with the new layered path. Distant
blurred terrain strips and missing tree crowns remain plainly visible; these
captures are **not** evidence of overall scene correctness. The 180-degree turn
kept zero mesh invalidations in sampled logs. A short 1,900-unit move within
Riverwood remained running, but the selected free-camera position produced a
black image (`CS_2026-09-12_02-40-59_580.png`), possibly inside geometry; its cause
was not established. Do not count that as a passed movement/streaming test.

HUD and Survival prompt remained functional; the described prompt was answered
No. Native game-code blocking logged 3,282–5,849 draw batches and 98 compute
submissions per sampled frame. Three previously known upload failures remain.
Grass regression passed: two GPU samples, maximum position error 0.00195312,
movement 0.792969, no UV/alpha mismatches; 22 opacity builds, 403 unique versus
876,844 expanded triangles, 11,282,032 cache bytes.

Ten-second same-camera sample: 23.19 FPS at current 1680x1050, median GPU
34.799 ms (path tracing 26.064, scene 6.647). This is not 1080p and is far below
the 60 FPS requirement. No exposure or quality settings were changed to claim
an improvement.

Restored normal first-person camera/freecam off at the original Riverwood
location. Recovery was visually verified in `CS_2026-09-12_02-43-19_912.png`:
world rendering and HUD are present. The short-move black capture therefore
did not leave the process persistently black; its exact cause remains open.
