#pragma once

#include <string>
#include <cstdint>

struct ID3D11DeviceContext;

namespace RE
{
	class BSGeometry;
	class NiPoint3;
	class InputEvent;
}

namespace RemixBridge
{
	/** Whether this process explicitly requested the isolated Remix diagnostic runtime. */
	bool IsRequested();

	/** Submit the explicitly configured Remix diagnostic scene before Skyrim's UI. */
	void BeforeUI();
	void AfterUI();
	void BeforePresent();
	bool SetConfig(const std::string& name, const std::string& value);
	/** Feed Remix from Skyrim's input stream; returns whether to consume game input. */
	bool ProcessMenuInput(RE::InputEvent* const* events);
	/** Queue focus changes without touching the render-thread ImGui context. */
	void SetMenuFocus(bool focused);
	/** One-shot native G-buffer capture at the pre-water deferred boundary. */
	void CaptureReferenceBuffers();
	void ComposeBeforeMenus();
	void CaptureGeometry(RE::BSGeometry* geometry, const RE::NiPoint3* grassScale = nullptr);
	void CaptureGrassWind(RE::BSGeometry* geometry, const RE::NiPoint3& wind, float timer, float previousTimer);

	/** Records the tree wind the game just bound.
	 *
	 * TreeParams (wind magnitude in y, amplitude in z, leaf frequency in w) and
	 * WindTimers are ordinary vertex shader constants, so they are only valid
	 * while a technique is bound. Geometry is discovered by walking the
	 * scenegraph, where no shader is bound and the constants do not exist yet,
	 * so they have to be taken here and kept for the submission that follows.
	 */
	void CaptureTreeWind(const float* treeParams, const float* windTimers);
	bool SuppressWorld();
	/**
	 * @brief Whether the world render in progress is being suppressed.
	 *
	 * SuppressWorld() reads live game state and options that can change at any
	 * moment. The passes inside one world render have to agree: skipping
	 * Main::RenderWorld and then letting the shadow pass run over the batches it
	 * never built crashes inside the game's shadow-caster lights, calling through
	 * a pointer that was never written. The decision is therefore taken once, at
	 * the top of the world frame, and every hook inside it reads that.
	 */
	bool SuppressWorldThisFrame();
	/// @brief Records the decision for the world render about to run.
	bool LatchSuppressWorld(bool suppress);
	bool CapturingReferencePairThisFrame();
	bool KeepNativePreparation();
	bool DispatchMaterialBake(ID3D11DeviceContext* context, uint32_t x, uint32_t y);
}
