# Independent Skyrim rendering boundary audit

## Sky policy correction — September 20

Read-only 1.7.99 audits `interior-sky-selection.json` and `interior-sky-mode.json`
establish the native rules. RVA `0x1a3f40` selects mode 1 for an interior without
CELL DATA bit 7 (Show Sky), irrespective of bit 8 (Use Sky Lighting). A sky-enabled
interior selects mode 2 without sky lighting or mode 3 with it. Worldspace rules
can additionally hide the sky. RVA `0x411660` makes the sky root visible only in
modes 2/3 and with Sky flags bit 7 clear. Remix's weather-cube path requires
mode 3, the active-cell Show Sky guard and no Hide Sky flag. Sky lighting alone
is not permission to display the last exterior cubemap. Mode 2 needs a separate
valid dome-only image source; it cannot use the existing weather cubemap.

A trial allowing mode 2 (`2CAAE030...`, PID24544) exposed exterior weather in the
inn after a native door return, despite clean host geometry membership. Capture
`052927-sky-policy-interior-shell.png` and `20260920-sky-policy-after-audit.json`
record that regression (3,900 interior dome samples). The mode-2 allowance was
removed. This is a controlled regression, not proof of the original leak's cause.

The reflection-settings implementation had an early return while already active,
contradicting the per-frame reconciliation described below. It now reapplies the
four overrides, preserving the original values once per ownership period, and
also runs before native world rendering can update the cubemap. Repeated restore
does nothing; a new ownership period snapshots anew. No saved INI is edited.

`CheckSkyPolicy.cpp` tests 96 visibility combinations and the override/reload/
restore lifecycle. `CheckReflectionOverride.ps1` temporarily perturbs each setting
in a loaded inn, verifies advancing frames, and restores all original in-memory
values in finally. The pre-fix B25E plugin failed all four settings across 136–141
rendered frames per perturbation; evidence is `20260920-sky-policy-negative.json`.

Final optimized plugin `19F306FD...` (label `20260920-sky-policy-final`) passes all
four live checks (`20260920-sky-policy-final-settings.json`). Native door entry
from Riverwood produced3,685 audit attempts/3,554 interior attempts, zero suspect
classes/dome and131 exterior dome submissions. Three missing-current-camera
attempts leave the strict overall audit failing; first failure was under Fader
Menu. Capture053323 shows black beyond the interior shell, with runtime dome
inactive. Capture053256 confirms the outdoor sky remains present. These are
limited tests, not whole-goal or all-interior qualification.

This establishes a settings-reconciliation defect, not reproduction of the user's
intermittent interior clouds/LOD. Before the fix, PID47604 had no suspect host
classes/dome during the observed inn run. Capture `052049-interior-leak-above-shell.png`
was black outside the shell, with runtime dome `active=0`. Do not label that report
resolved from these controls. The whole-scene reflection branch noted below is
also still unqualified.

Status: independent game-code suppression and UI composition were built and
deployed for the September 11 23:29 test. The old D3D11 draw/dispatch suppression
hooks and the existing CS pass early-return were removed. Scene capture is still
raster-pass-dependent and needs replacement; this is not full goal qualification.

## Exact binary provenance

The game executable reports 1.7.99.0. Its SHA-256 is
`0b473f0d6c42d0b2885266e78394a64c980480e9663dd1ea8b51731961d0c18a`.
This exactly matches the executable SHA-256 recorded by the existing Ghidra program
`/skyrim/ae1.7/SkyrimSE.exe.unpacked.exe` in
`G:/ghidrashit/BethesdaGhidraScripts-1.7.99/ghidraprojects/BethesdaGhidraScripts`.
The harness's packed executable has a different hash; the game uses the matching
unpacked executable. Do not reuse the earlier conversational 1.7.104 claim.

`tools/remix/ghidra/RenderBoundaryAudit.py` is read-only: it exports names, call
sites, callers, leading bytes and decompilation without changing functions or
saving the program. Analysis artifacts are under `.research/ghidra-*.json`.

## Findings

Addresses below are image-relative RVAs, with image base `0x140000000`.

- `0x656c00` (Main::Draw, 2751 bytes) contains culling jobs, water preparation,
  camera updates, world draws, first-person draws and end-of-frame image-space
  effects. It is not safe to return from this entire routine without replacing
  its necessary CPU work. Its decompiler prototype is incomplete: EDX/R8 inputs
  appear in the body despite the imported single-argument signature.
- Its sole code caller is at `0x656bcd`, inside `0x656ab0`. That wrapper establishes
  camera origin state and renderer frame state before calling Main::Draw.
- `0x657a30` updates sky/weather nodes and performs other frame preparation,
  including high-actor culling. A replacement retained scene cannot inherit
  this culling decision as object lifetime/visibility.
- `0x6576d0` handles water preparation and drains BGSAutoWater queues, including
  image-space work. Skipping it wholesale risks breaking water simulation/state.
- Independent game submission candidates include `0x100ba70`, `0x100bb70`,
  `0x100bc70`, `0x100c4c0`, `0x100c5f0`, `0x100c720`, `0x100c9c0`,
  `0x100ce90`, `0x100d190`, and `0x100d2c0`. Ghidra shows these loading the
  context global at VA `0x143331f30`, configuring buffers, then issuing indexed
  or instanced draws. Do not confuse device methods at the separate device
  global `0x143330190` with draw submission merely because a vtable offset matches.

## Implemented game-code boundary

`src/RemixNativeRender.cpp` installs only for 1.7.99.0, verifies every site's
instruction bytes/relative target before any patching, and checks Detours results.
It does not intercept D3D11/Vulkan exports or context methods.

- The independent wrapper at `0x656ab0` receives `(Main*, uint32 imageSpaceTarget)`.
  Its instructions save EDX, then pass that value and the menu-test result to
  Main::Draw. It starts suppression without skipping CPU scene/camera/water work.
- The ten listed draw wrappers are independently detoured before their dirty-state
  flush/buffer binding/submission. `0x100d190` still writes its consumed dynamic
  offset sentinel at data+0x18. `0x100c720`/`0x100c9c0` contain temporary draw-buffer
  allocation/copies, not persistent animation updates; these transient uploads are
  skipped together with their native draw.
- `0x1589adc` is a **tail JMP**, not CALL, into `0x100f280`. The instruction export
  proves this despite Ghidra's reference metadata calling it UNCONDITIONAL_CALL.
  This independent branch is replaced, not the existing CS compute entry hook.
- Independent calls at `0x6566c1` and `0x6e4b60` wrap interface rendering, ending
  suppression and submitting Remix. Existing CS menu TAA code no longer calls
  the bridge. `0x117baf0` calls Scaleform BeginFrame; its new independent detour
  composes Remix into Skyrim's framebuffer **after** setup and **before** menus.
  No intercepted first-D3D-draw timing remains.

Additional artifacts: `ghidra-submission-and-ui.json`,
`ghidra-ui-compute-boundary.json`, `ghidra-ui-begin.json` under `.research`.
The first two guarded launches stopped before patching because the handwritten
checks omitted the REX prefix on the Scaleform tail jump and misclassified the
compute tail jump. Both checks were corrected to match exported instructions;
they were not relaxed. The subsequent launch passed all checks.

## Initial validation (not completion)

The 23:29 build and its matching PDB are archived at
`.research/deployed-symbols/20260911-native-game-hooks`. Test process 20428 loaded
Riverwood through SKSE/DevBench. Logs report about 6,300 suppressed native draw
batches and 98 suppressed compute submissions per frame. Viewed captures in the
game's `screenshots` directory:

- `CS_2026-09-11_23-30-04_708.png`: Riverwood with intact HUD and Survival dialog.
- `CS_2026-09-11_23-30-41_168.png`: Remix shading-normal debug covers the visible
  world while the native compass remains normal UI.
- `CS_2026-09-11_23-31-05_162.png`: inventory panel over the Remix scene. This tests
  panel composition, **not** a selected 3D item preview or all menu types.

Debug view was restored to zero and the test-opened inventory was closed. Output
remains 1680x1050; observed full-shading performance is around 15–16 FPS, far below
the required 60 FPS at 1920x1080. These captures/counters do not prove that every
possible submission path is covered; streaming, effects, selected 3D previews,
first-person models, paused-menu transitions and camera-rotation tests remain.

A subsequent read-only 10.185-second sample measured 165 frames (16.2 FPS).
Six GPU samples gave median total 61.262 ms, path tracing 54.966 ms, indirect
integration 37.596 ms, RTXDI 7.959 ms, and G-buffer 4.463 ms. The dominant measured
cost is now inside Remix's tracing, not native raster submission. Investigate
the runtime's unconditional FP64 perspective-derivative path without reducing
the requested rendering quality; this is a hypothesis, not yet a proven cause.

## Retained scene and LOD ownership audit

The 23:44 prototype moves object discovery out of raster-pass hooks into
`RemixSceneGraph`. Loaded NiPointer owners and instances survive camera culling;
AppCulled is counted but not filtered. The generic BSBatchRenderer capture call is
removed. Source grass setup supplies ScaleMask only, not scene membership.

Additional read-only exports: `ghidra-segment-culling.json`,
`ghidra-segment-methods.json`, and `ghidra-segment-visibility-owners.json`.
BSSubIndexTriShape segment masks must **not** simply be ignored:

- `0xff5e40` loads original triangle ranges at segment+4 and merged ranges at +0xc.
- `0xff65a0`/`0xff65d0` enable/disable input segments; `0xff65f0` merges ranges.
- Their callers `0x518d40`/`0x518fa0` consult terrain block coordinates and loaded
  cell state to suppress LOD covered by full cells. No camera/frustum test occurs
  in these examined owners. Keep this coverage union to avoid duplicate geometry.
- `0xff6730` (OnVisible) reads segment count; it does not change segment masks.
- NiSwitchNode OnVisible at `0xeedea0` reads the selected-child index, not writing
  it from a visibility test. Preserve this mutually exclusive authored selection.

Paused Riverwood samples retained approximately 9,753 loaded geometries (including
1,416 AppCulled), 5,920 instances and 54,147 placements, with zero changed instances.
Fixed-position 180-degree and return captures at 23:48:17 and 23:50:09 were viewed.
This is limited rotation evidence, not full streaming/change-detection validation.
The correction built at 23:50 also caches API placement conversions and excludes
the retained third-person player body from primary rays in first person; correct
view-model projection remains unimplemented.

## Grass input and alpha audit (September 12)

Same executable hash as above. Read-only Ghidra exports are retained in
`.research/grass-native-{symbols,timing,constants}.json`.

`BSGrassShader::Func6` / SetupGeometry, RVA `0x1522610`, computes:

- Timer = float at `0x20d68e0` × float constant `0x197f0e8`
  (`0.0016666667070239782`) × `0x1b3f734` (`6.283180236816406`)
  × grass property float `+0x184` (wavePeriod). The clock is written by
  Main::Update (`0x658610`), not by grass visibility. Property `+0x188`
  is the previous timer as a float despite CommonLib's uint32 declaration;
  native setup conditionally updates it on the appropriate pass.
- Wind amplitude = min(float `0x33d4c38`, `60.0f`) × float `0x20d6d30`.
  The latter multiplier defaults to 1. The amplitude input is written by
  ShadowSceneNode setup, not by individual grass geometry visibility.
- Wind XY = XY of normalized inverse-world transform of world direction +Y.
  The shader uses `(WindVector.xy, 0)` for displacement, and WindVector.z
  stores amplitude rather than a direction component.
- ScaleMask = `(1,1,1)` with property flag `kUniformScale` (bit 43),
  otherwise `(0,0,1)`. This can be derived for off-camera loaded grass too.

`RemixNativeRender::ReadGrassWind` is gated by successful installation of the
exact-version audited hooks. It only reads these inputs; it does not invoke a
render pass, use wall time, or modify engine state. `RemixScene` compares its
results against native constant-table inputs and updates retained grass metadata
independently of AppCulled/native pass traversal. GPU deformation was pending at
this clock-audit stage; the subsequent implementation and bounded GPU validation
are recorded in `remix-vulkan.md` under retained GPU grass wind.

The CPU-authored blend table at relocation `(524749,411364)` was inspected via
GetDesc using the shadow state's selected indices, not stale bound D3D11 state.
Both observed grass passes `0x5C00005C` and `0x5C000032` use blend mode 0,
BlendEnable=false, AlphaToCoverage=false, alpha test enabled, and NIF threshold
divided by 255. NIF flags nevertheless include the blend bit (`0x12ED`/`0x1201`).
The bridge now disables blending for grass specifically while keeping its cutout
test. Same-camera captures verify that this removes the cloudy transparent grass.
This diagnostic uses the existing grass metadata callback only; the independent
game-code hooks above remain the sole native-world suppression mechanism.

## Required implementation and verification

### Water atlas resource update exception (September 12)

Read-only native audit found pending water flow tiles are populated through the
compute effect `0x1540b50`, called from the water queue drain at `0x657912`.
Native world compute suppression also blocked that data-production work. The
bridge now replaces **only this callsite**, only for mode-zero copies whose
destination is the native flow atlas (`NiSourceTexture*` global `0x33d3e60`).
It runs an owned BGRA8-to-RGBA8 typed tile copy on a deferred context. Rectangle
and resource bounds are checked, state is restored, and native-reference/off
mode calls the original. Other effect callsites remain unchanged. This does not
allow native water shading, post-processing or arbitrary compute through the
world gate. See `remix-water.md` for atlas and live-copy evidence.

Wading flow globals are read from their player/grid-driven producer at
`0x52a890`, not from the most recent visible water pass's constant buffer.

### Remaining verification

1. Extend the submission audit beyond the identified wrappers where necessary,
   including dynamically dispatched/other-module work; verify complete coverage
   without reintroducing API interception.
2. Validate native menu/loading/selected 3D preview rendering across transitions,
   and verify essential CPU state is preserved under the new world gate.
3. Give Remix retained ownership of loaded scene objects. Frustum, occlusion,
   shadow-pass visibility and transient AppCulled state must not remove objects.
   Update geometry/material/transform data only on genuine changes; handle real
   unloads and destruction explicitly. Avoid reuploading unchanged instance arrays.
4. Extend validation of the new game UI composition boundary. Verify no native
   world/post-processing survives while all required UI paths remain working.
5. Test camera rotation and movement, character animation, cell streaming,
   terrain/grass/LOD continuity, loading and 3D menu previews, then measure the
   requested 1920x1080 output and 60 FPS gate. Existing captures are not that proof.

## Reflection cubemap LOD gating (September 18)

`RemixSky` hands Remix's dome light the image Skyrim draws into
`RENDER_TARGETS_CUBEMAP::kREFLECTIONS`, so whatever the game renders there
becomes the sky. By default that includes LOD terrain, LOD objects and LOD
trees, which appear in the dome as ground and treelines swimming with the
camera and popping as LOD swaps.

`TESWaterReflections::Update` (RVA `0x5282a0`) is the **only** consumer of the
four settings below anywhere in the 1.7.99 image. Each gates one call to the
root-submit helper at RVA `0x52f310`, which appends an `NiAVObject*` to the
array at `reflector+0x188` and takes a reference:

| Setting | `Setting` object | `data.b` | read at | root submitted |
| --- | --- | --- | --- | --- |
| `bReflectLODLand:Water` | `0x1420b3878` | `0x1420b3880` | `0x140528529` | `BGSTerrainManager::GetLODLandRoot` |
| `bReflectLODObjects:Water` | `0x1420b3890` | `0x1420b3898` | `0x140528542` | `BGSTerrainManager::GetLODObjectRoot` |
| `bReflectLODTrees:Water` | `0x1420b38a8` | `0x1420b38b0` | `0x14052855b` | `BGSTerrainManager::GetTreeNode` |
| `bReflectSky:Water` | `0x1420b38c0` | `0x1420b38c8` | `0x140528574`, `0x1405286d1` | sky root |

The bytes are read fresh on every update and nothing caches them, so no code is
patched: `RemixSky::SetReflectionLodEnabled` clears the three LOD settings and
forces the sky setting on while Remix owns the world, and replays the player's
own values when it does not. `RE::GetINISetting` resolves them by name, which
searches both the pref and non-pref collections, so this is not version-locked
the way the addresses above are. It is reconciled every frame rather than only
on the transition, because a graphics settings change or a save load can rewrite
them underneath.

`bReflectSky` also selects the fifth argument to the per-face render virtual at
`+0x1a8`: with the sky submitted the face is not cleared first, on the
assumption the sky dome covers it.

One branch bypasses all four checks. When bit 12 of `TESWaterReflections+0x10`
is set, `Update` submits `*(*(TES+0x100)+8)` — a whole-scene root — instead of
consulting the settings at all. Riverwood exteriors were observed taking the
settings path, but nothing here forces bit 12 clear, so a configuration that
sets it would put the entire world back into the dome.
