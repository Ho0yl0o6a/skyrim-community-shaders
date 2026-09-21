#pragma once

#include "d3d11_include.h"
#include "../dxvk/dxvk_image.h"

namespace dxvk {
  class D3D11DeviceContext;

  // API-only host: Skyrim supplies camera, meshes, materials and lights explicitly.
  // Never infer transforms from arbitrary game shaders or patch game modules.
  class D3D11Rtx {
  public:
    explicit D3D11Rtx(D3D11DeviceContext* context) : m_context(context) { }
    void Initialize();
    bool OnDraw(UINT, UINT) { ++m_drawCallID; return false; }
    bool OnDrawIndexed(UINT, UINT, INT) { ++m_drawCallID; return false; }
    bool OnDrawInstanced(UINT, UINT, UINT, UINT) { ++m_drawCallID; return false; }
    bool OnDrawIndexedInstanced(UINT, UINT, UINT, INT, UINT) { ++m_drawCallID; return false; }
    void OnUpdateSubresource(ID3D11Resource*, const void*, UINT, UINT = 0, UINT = 0) { }
    void EndFrame(const Rc<DxvkImage>& backbuffer);
    void OnPresent(const Rc<DxvkImage>& swapchainImage);
    void SetSwapchainBackbuffer(const Rc<DxvkImage>&) { }
    uint32_t getDrawCallID() const { return m_drawCallID; }
    void resetDrawCallID() { m_drawCallID = 0; }
    void addDrawCallID(uint32_t count) { m_drawCallID += count; }
  private:
    D3D11DeviceContext* m_context;
    uint32_t m_drawCallID = 0;
  };
}
