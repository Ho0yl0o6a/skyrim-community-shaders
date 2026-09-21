# Native animated refraction

## Native evidence

Read-only Ghidra audit of AE1.7 executable SHA256
`0b473f0d6c42d0b2885266e78394a64c980480e9663dd1ea8b51731961d0c18a`:

- `BSUtilityShader::Func4`, RVA `0x1566db0`: RENDER_NORMAL samples the
  **diffuse** texture (material+0x48), using clamp mode+0x70, UV offsets/scales
  at+0x0c/+0x1c and active slot at global RVA0x20d69d0. Power is material+0x84.
- `BSUtilityShader::Func6`, RVA `0x1567190`: EyePos.w comes from
  BSLightingShaderProperty+0x104 (`envmapLODFade`). Angular falloff optionally
  transforms the camera position into object space.
- `BSLightingShaderProperty::GetRenderPasses`, RVA `0x1519f00`: property bit16
  selects Utility bit10 (normal falloff); property bit13 selects Utility bit11
  (normal clamp). Utility descriptor includes its base offset0x2b.
- `Utility.hlsl` clamps BOTH the per-vertex view normal and sampled distortion
  XY to +/-0.1 in the clamp variant. Distance attenuation is computed per vertex
  before interpolation. Vertex alpha, power, property fade and angular falloff
  multiply strength. Texture alpha does not control coverage.
- `ISRefraction.hlsl` displaces UV by (-1,+1)*0.1*strength*(normal.xy-0.5),
  applies strength-weighted edge compression, then uses the displaced mask's
  alpha to blend original and displaced scene color.

Artifacts: `.research/refraction-native-setup.json`,
`.research/refraction-native-technique.json`, `.research/refraction-native-passes.json`.
`tools/remix/ghidra/RunReadOnlyAudit.py` opens the existing analyzed program
read-only; it does not reanalyze, edit or save the database.

## Transport and Remix pass

Native refraction currently uses the existing 20-float animated effect material
transport, with explicit flag512. Indices0..3 carry active UV offset/scale,
index8 carries power, index13 carries property fade. Flag32 means vertex alpha,
1024 angular falloff, 2048 clamp. Low bits0/1 remain runtime-owned sRGB flags.
Refraction and palette-effect semantics are mutually exclusive. The source is
the native diffuse SRV, with its native wrap/clamp mode; normals are data, not
albedo. The static identity probe must not skip these animated materials.

The mesh remains registered in the unordered TLAS. Ordinary lighting resolves
zero coverage for its special material; a separate compute ray-query pass
evaluates its distortion mask and composites the path-traced image. The pass
runs after upscaling/dust and before bloom/tonemapping/UI. No native world draws
are re-enabled. Mask storage is reused and resized with the output. Scenes
without refraction skip both passes, based on the existing material-pack loop.

`rtx.nativeRefraction.debugMask` displays mask RGB through the postprocessing
chain for diagnostics. It must be restored to false after testing.

## Verification limits

`test_native_refraction` shares scalar equations with the actual shaders and
tests near/far distance, clamp, edge handling, displacement sign and negative
falloff. This is a CPU unit test, not a GPU parity test.

`CheckRefractionInputs.ps1 -RequireImports -RequireImportedAnimation` checks
live native/imported UV and strength transport. It does not prove GPU output.

Current implementation chooses the nearest refraction triangle and depth-tests
against Remix primary linear view Z. Overlapping native pass order, low-resolution
or jittered depth edges, PSR/viewmodel occlusion, skinned refraction, temporary
refraction and native render-target quantization need separate verification.
Do not describe this initial implementation as full native image parity.

2026-09-20 live check in Sleeping Giant Inn: all three hearth refraction meshes
submitted, power0.1/fade1 match native values, all three imported UVs animate.
Matching lit capture001856 removes opaque rainbow sheets; mask captures001939
and001943 show changing spatial patterns with a fixed camera. Interior membership
and six-flame UV regressions passed. Debug mode restored off. See handover for
deployed hashes and full capture paths.
