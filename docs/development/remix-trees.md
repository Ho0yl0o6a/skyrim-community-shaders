# Distant-tree import checkpoint (2026-09-12)

The distant-tree placement and shared-atlas paths now render textured cutout trees
in the Riverwood comparison view. This does **not** establish correct full-size
tree animation, LOD transitions, or completion of the renderer.

## Native evidence

Read-only Ghidra analysis of Skyrim 1.7.99.0, executable SHA256
`0b473f0d6c42d0b2885266e78394a64c980480e9663dd1ea8b51731961d0c18a`:

- `0x50c6d0`: distant-tree upload writes 32-byte instance records and calls
  `BSMultiStreamInstanceTriShape::AddGroup` with 16 halfwords per record.
- `0xfe2a20`: AddGroup allocates count * halfwords * 2 bytes without setting the
  shape's `instanceSize` scratch field. Live distant-tree shapes have size zero
  despite valid groups. Using that field rejected all distant-tree placements.
- `0xfe36e0`: OnVisible writes group frustum visibility, not placement data. The
  importer ignores that visibility flag and retains all genuinely active records.
- `0x50c100`: worldspace loading assigns the atlas at `0x31f4e00` and copies it to
  the shader-global atlas at `0x3486718`.
- `0x1558d70`: distant-tree technique setup binds the shader-global atlas renderer
  texture's SRV to PS slot zero. The per-property base-texture accessor is empty
  in the live scene and must not be used for this shader.

Audit outputs are `.research/tree-native-layout.json`, `tree-native-upload.json`,
`tree-atlas-symbols.json`, and `tree-atlas-native.json`. These are local evidence,
not distribution dependencies. The accessor is restricted to the audited version.

## Import and lifetime

Records decode XYZ, scale, cosine/sine rotation, and active alpha from their first
eight halfwords. Zero-scale/zero-alpha records are inactive. Existing placement
retention and source-byte comparison apply; stationary groups do not require GPU
readback or placement reconstruction every frame. Native frustum flags are ignored.

The material uses the shared atlas, native UVs, clamp sampling, and the alpha
property's cutout test (Riverwood: GREATER, reference 128). It waits for a valid
atlas instead of caching opaque fallback quads. Atlas/SRV identity changes invalidate
the owned Remix material and mesh. The runtime owns the imported Vulkan view; CS
does not retain an engine SRV wrapper. In-place image-content revisions are not yet
tracked independently of resource identity.

`communityshaders.inspect {"kind":"remixScene","filter":"LOD Trees"}` returns
up to 48 nearest matches, with ancestor types, cached/submitted state, imported
material, base texture, shared atlas, and instance groups. This is read-only and
does not change native culling. Match counts include entries beyond the bounded list.

## Deployed and observed

CS DLL SHA256 `6135605C48416C0736C46326FFFDC7F704A8B8419B42CF57D69F73AFD8DACA5D`;
symbols in `.research/deployed-symbols/20260912-tree-atlas`. The runtime remains the
six-layer landscape build. Previous CS and logs are backed up under
`.research/deployment-backups/20260912-tree-atlas`.

Live PID 38908, started 03:02:42 local. Atlas imports report
`Textures\Terrain\Tamriel\Trees\TamrielTreeLOD.DDS`, successful, reference 128.
At one sample: 9,714 loaded geometry, 6,290 cached, 6,272 submitted, 375 tree groups.
The per-property base texture is empty while the shared atlas is populated.

Viewed capture:
`I:/SteamLibrary/steamapps/common/Skyrim Special Edition/screenshots/CS_2026-09-12_03-04-32_344.png`.
Camera XYZ `(21055.85546875, -43099.51953125, 39.4857177734375)`, pitch
`0.0865935311`, yaw `0.2872291803`. Textured tree silhouettes replace the previous
opaque crossed quads. Near grass, ground, bridge, and HUD remain visible. Distant
terrain is still visibly incorrect. This is 1680x1050, not the final 1080p test.

Grass GPU probe: two samples, maximum position error 0.00390625 and motion 0.707031.
Landscape: 100 successful imports, 63 with all six diffuse/normal layers. These
checks are import/animation evidence, not proof of full visual correctness.

## Native reference and remaining full-size-tree omission

A same-camera native reference was viewed in
`CS_2026-09-12_03-18-36_779.png`. The hillside behind the stone markers is also
low-detail in native rendering. Do not assume every broad LOD color band is a UV
decoding failure. Native full-size trees, however, are clearly present where the
Remix capture has no trunk or canopy. Their scene ownership/import remains open.

The LOD audit (`filter: "feature:18"`) found valid 0–1 UV bounds, identity material
UV transforms, the corresponding `Tamriel.4.*.DDS` diffuse/normal textures, and
model-space normals enabled. A temporary `rtx.nativeMipBias=-4` test did not remove
the broad bands and was restored to zero. LOD noise/fade parity remains unfinished.

`cs.nativeReference=true` pauses Remix and enables native world submission for a
reference capture. **It is deliberately one-way per process. Restart the test to
return to Remix.** An attempted live resume produced `VK_ERROR_DEVICE_LOST` and
an Aftermath dump at 03:18:48; its resource-lifetime cause is unresolved. Logs and
dump are preserved in `.research/deployment-backups/20260912-native-reference-device-loss`.
The hung test was terminated and relaunched. This switch is session-only, defaults
off, and `ConfigureRunningTest.ps1` explicitly requests off on a fresh process.

`filter: "refs:tree"` adds a bounded read-only reference/child-tree census to help
locate full-size trees omitted by the current geometry traversal. This diagnostic
does not add raster-visible objects to the retained scene or alter culling.

## Full-size-tree traversal correction

The reference census exposed real `BSTreeNode` roots with `NiSwitchNode` children,
but their active indices read as 32759 and their descendant meshes were absent
from the gathered scene. This was a C++ layout error, not native culling:
`SKYRIM_CROSS_VR` removes NiNode's explicit children member from the compiled
base layout. Direct access to the derived NiSwitchNode `index` therefore reads
the high DWORD of the children-array vtable instead of the native field.

The existing Ghidra export `ghidra-segment-culling.json` confirms that native
NiSwitchNode::OnVisible, RVA `0xeedea0`, reads its index at **+0x12c**, child-array
data at +0x118, and revision data at +0x134/+0x140. `ReadSwitchIndex` now uses the
audited index offset with an exact-version guard, alongside the already
runtime-aware `GetChildren()` accessor. Traversal still selects only the active
child rather than duplicating both tree LOD representations.

Correction build: CS SHA256
`83C1B9BD130C3F74CAEA41DC91B153FA4BBFB64E53859637BA5BD87AEF4147A6`,
symbols `.research/deployed-symbols/20260912-tree-switch`.

Live validation: PID 8860, started 03:27:11 local. The matching screenshot
`CS_2026-09-12_03-28-57_230.png` was viewed: the large near trunk/canopy and other
full-size trees now render alongside the distant cutout trees. At frame 2431,
the census reports 10,339 loaded geometries, 6,888 cached meshes, 6,872 submitted,
and 452 geometries under BSTreeNode. All 48 returned nearest tree geometries are
submitted with imported materials. Reference switch indices now read zero and
the selected pair of trunk/canopy geometries is gathered.

The native-world gate remains active (sample: 3,389 draw batches and 98 compute
submissions suppressed); placement arrays and GPU source readbacks remain zero
in stationary samples. Grass probe passed with two samples, maximum position
error 0.00390625, motion 0.253906. Landscape imports remain 100 successful, with
63 six-layer pairs. Three pre-existing Fish mesh upload failures remain.

With the restored near trees, a ten-second sample is **17.85 FPS at 1680x1050**,
median GPU 50.96 ms, path tracing 41.081 ms, scene 7.778 ms. This fails the 60 FPS
1080p requirement and must not be compared to the missing-tree build as equivalent
workload. Correct full native tree wind, native weather/light sources, water and
effects, character/first-person parity, and streaming validation remain unfinished.

Current process stays in Remix scene mode with native reference off, native world
suppressed, final debug view zero and native mip bias zero. No user game assets
were deleted. Only the owned CS DLL was overwritten for these tests.
