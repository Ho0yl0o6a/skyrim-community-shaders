# Remix material record stride

The fork's material and material-extension buffers use 112-byte records.
The subsurface and ray-portal CPU packers still advanced only 96 bytes. The
extension upload concatenates records using the returned offset, while shaders
index a `MemoryPolymorphicSurfaceMaterial` array with a 112-byte stride. Thus
subsurface entries after index zero decoded another part of the buffer. This
is a concrete serialization defect affecting skin/thin foliage, not evidence
that it caused every reported lighting/normal or interior-membership problem.

`SURFACE_MATERIAL_GPU_SIZE` now defines the common stride for host and shader.
Host/shader padding is derived from each payload size (opaque 76, translucent
110, portal 16, subsurface 26 bytes). Subsurface serialization also asserts its
final offset in debug builds. Existing payload fields and rendering equations
are unchanged. The native-effect payload continues to occupy all 112 bytes.

`tests/rtx/unit/test_material_layout.cpp` calls the production packers through
`RtSurfaceMaterial` for consecutive records. It checks stride, selected field
offsets, leading/trailing guards and padding boundaries. Release deliberately
leaves padding to caller initialization; debug fills it with 0xff. Tests cover
skin and thin-opaque subsurface, portals, ordinary/thin glass, opaque, landscape,
native effects and water. Run only from the dedicated `_Comp64UnitTest` build.

The initial negative control failed with `First material does not advance one
GPU record` before the packer edit. Evidence is archived in
`.research/testlogs/20260920-material-layout-negative-test.txt`.
CPU serialization tests do not by themselves certify in-game visual parity.

Final validation on 2026-09-20 passed all three dedicated unit targets:
`test_material_layout`, `test_skinning_basis`, `test_native_basis_vertex`.
Positive output is `.research/testlogs/20260920-material-layout-positive-test.txt`.
Shaders and the full optimized runtime built successfully and were deployed as
`20260920-material-stride`. Eight console cell transitions and three native door
activations subsequently passed in one process. Matching-camera inn screenshots
still show incorrect fluorescent glass; character/normal and water parity are
not established. See the latest handover for exact deployed hashes and evidence.
