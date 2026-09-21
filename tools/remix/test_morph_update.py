"""Source-contract checks, not GPU history or animation acceptance."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
RUNTIME = ROOT / '.research/dxvk-remix/src'


class MorphUpdateContracts(unittest.TestCase):
    def test_compatibility_checked_before_gpu_allocation(self):
        code = (RUNTIME / 'd3d11/d3d11_remix_api.cpp').read_text()
        begin = code.index('remixapi_ErrorCode createMeshInternal(')
        body = code[begin:code.index('// A texture the host', begin)]
        self.assertLess(body.index('previousLayout->second != layouts'),
                        body.index('allocMeshBuffer(vertexDataSize)'))
        for field in ('blendWeights_values', 'blendIndices_values', 'indices_values'):
            self.assertIn(f'XXH64({"skin" if field.startswith("blend") else "surface"}.{field}', body)

    def test_new_positions_keep_topology_and_refresh_draws(self):
        code = (RUNTIME / 'd3d11/d3d11_remix_api.cpp').read_text()
        for field in ('Indices', 'GeometryDescriptor', 'VertexLayout'):
            self.assertIn(f'next.hashes[dxvk::HashComponents::{field}] = old.hashes', code)
        self.assertNotIn('next.hashes[dxvk::HashComponents::VertexPosition] = old.hashes', code)
        self.assertIn('invalidateExternalMesh(cHandle)', code)

    def test_host_uses_same_handle_before_rebuild_fallback(self):
        code = (ROOT / 'src/RemixScene.cpp').read_text()
        self.assertIn('updateExisting ? reinterpret_cast<uintptr_t>(mesh.handle) : ++nextHash', code)
        update = code.index('if (found != meshes.end() && !staticMeshChanged && updateMesh')
        self.assertLess(code.index('rendererData, true)', update), code.index('api->DestroyMesh', update))
        self.assertIn('mesh.dynamicPositions = std::move(previousPositions)', code[update:])

    def test_invalidation_clears_cached_snapshots_without_destroying_nodes(self):
        code = (RUNTIME / 'dxvk/rtx_render/rtx_scene_manager.cpp').read_text()
        begin = code.index('void SceneManager::invalidateExternalMesh(')
        body = code[begin:code.index('\n  namespace {', begin)]
        self.assertIn('++entry.revision', body)
        self.assertIn('entry.cachedSubmeshes.reset()', body)
        self.assertNotIn('destroy', body)


if __name__ == '__main__':
    unittest.main()
