"""Structural checks for menu integration; actual input is tested in Skyrim."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
RUNTIME = ROOT / ".research/dxvk-remix/src"


class MenuInputContractTests(unittest.TestCase):
    def test_menu_is_composited_after_game_and_before_present(self):
        source = (RUNTIME / "d3d11/d3d11_swapchain.cpp").read_text()
        body = source.split("HRESULT D3D11SwapChain::PresentImage", 1)[1]
        self.assertLess(body.index("m_blitter->presentImage"), body.index("getImgui().render"))
        self.assertLess(body.index("getImgui().render"), body.index("m_rtx.OnPresent"))
        self.assertIn("render(m_context, info.imageExtent, true)", body)

    def test_game_input_is_fed_then_consumed_before_cs_menu(self):
        source = (ROOT / "src/Hooks.cpp").read_text()
        body = source.split("struct BSInputDeviceManager_PollInputDevices", 1)[1]
        route = body.split("bool blockedDevice", 1)[0]
        self.assertIn("RemixBridge::ProcessMenuInput(a_events)", route)
        self.assertIn("func(a_dispatcher, dummy)", route)
        self.assertLess(body.index("ProcessMenuInput"), body.index("menu->ProcessInputEvents"))

    def test_host_queue_is_bounded_and_consumed_on_gui_thread(self):
        source = (RUNTIME / "dxvk/imgui/dxvk_imgui.cpp").read_text()
        queue = source.split("bool ImGUI::queueHostInput", 1)[1].split("void ImGUI::processHostInput", 1)[0]
        self.assertIn("lock(m_hostInputMutex)", queue)
        self.assertIn("m_hostInputQueue.size() >= 512", queue)
        self.assertNotIn("ImGui::GetIO", queue)
        render = source.split("void ImGUI::render(", 1)[1].split("void ImGUI::createFontsTexture", 1)[0]
        self.assertLess(render.index("m_overlayWin = nullptr"), render.index("m_overlayWin->update"))
        self.assertLess(render.index("processHostInput()"), render.index("ImGui::NewFrame()"))
        self.assertIn("RtxOptions::blockInputToGameInUI()", render)


if __name__ == "__main__":
    unittest.main()
