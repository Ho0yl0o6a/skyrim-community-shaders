#pragma once

/**
 * @brief Drops redundant D3D11 state-setting calls before they reach the runtime.
 *
 * Skyrim's batch renderer re-binds the same vertex buffer, index buffer, shader
 * resources and constant buffers for draw after draw. The native NVIDIA D3D11 driver
 * discards those in a handful of nanoseconds; DXVK's D3D11 layer pays a cross-module
 * call and a walk of its (much larger) context state block before reaching the same
 * conclusion, which an ETW profile puts at roughly a millisecond of render-thread CPU
 * per frame in a CPU-bound scene.
 *
 * This keeps a compact shadow of the handful of slots the game actually uses and
 * returns without entering the runtime when a call would change nothing.
 *
 * **Measured result: this does not pay off, and the hooks are off unless asked for.**
 * In the Whiterun bench the game issues ~38,000 of these calls per frame, but only the
 * constant-buffer binds are meaningfully redundant (98% of `PSSetConstantBuffers`, 90%
 * of `VSSetConstantBuffers`) and those are the calls DXVK already answers in ~7 ns. The
 * expensive ones -- vertex buffers, index buffers, shader resources, at 50-90 ns each --
 * are 5-9% redundant. Worse, interception costs 44 ns per call on its own (1.67 ms per
 * frame with every call still forwarded), because the cost being measured is not the
 * comparison but touching *any* state between two draws with a cold cache. A second
 * shadow cannot beat that; the win has to come from DXVK touching fewer lines per draw.
 *
 * Kept because the counters are the cheapest way to get an exact per-call census of the
 * game's D3D11 traffic.
 *
 * Environment (read once, at install):
 *   - unset: the hooks are not installed at all.
 *   - `CS_D3D11_FILTER=0`: install and count, but forward every call.
 *   - `CS_D3D11_FILTER=1`: install, count, and drop redundant calls.
 *   - `CS_D3D11_FILTER_STATS=1`: log per-frame call and skip counts every 600 presents.
 */

struct ID3D11DeviceContext;

namespace D3D11CallFilter
{
	/** @brief Installs the state-setter detours. Call once, after the context exists. */
	void Install(ID3D11DeviceContext* a_context);

	/** @brief Advances the frame counter and emits the statistics line when due. */
	void OnPresent();
}
