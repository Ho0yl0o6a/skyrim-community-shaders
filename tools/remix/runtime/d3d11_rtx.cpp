#include "d3d11_rtx.h"
#include "d3d11_context.h"
#include "../dxvk/rtx_render/rtx_context.h"

namespace dxvk {
  void D3D11Rtx::Initialize() {
    Logger::info("[CS Remix] API-only D3D11/Vulkan host; no automatic scene capture");
  }

  void D3D11Rtx::EndFrame(const Rc<DxvkImage>& backbuffer) {
    // Rendering is explicitly requested before Skyrim's UI. Never overwrite it at Present.
    m_context->EmitCs([backbuffer](DxvkContext* ctx) {
      static_cast<RtxContext*>(ctx)->endFrame(0, backbuffer, false);
    });
    m_drawCallID = 0;
  }

  void D3D11Rtx::OnPresent(const Rc<DxvkImage>& image) {
    m_context->EmitCs([image](DxvkContext* ctx) {
      static_cast<RtxContext*>(ctx)->onPresent(image);
    });
  }
}
