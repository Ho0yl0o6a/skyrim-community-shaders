#pragma once

/**
 * @brief Sizes what occlusion culling could be worth, before any culling exists.
 *
 * The DXVK-vs-native penalty on NVIDIA is per-draw D3D11 CPU on the render thread, so
 * anything that removes draws removes it too — and removes it faster than it removes native
 * frame time, because DXVK pays roughly 2.5x per draw. Before porting a real occluder
 * rasteriser it is worth knowing the shape of that curve.
 *
 * This drops a fixed fraction of render passes, deterministically, from inside the batch
 * renderer hook. The picture is wrong while it is on — it is a measurement, not a feature.
 *
 * `CS_CULL_PROBE=<percent>` (0-100), read once. Absent or 0 means every pass is drawn and the
 * check is a single predictable branch.
 */

#include <cstdint>

namespace DrawCullProbe
{
	namespace detail
	{
		inline uint32_t g_percent = UINT32_MAX;  // UINT32_MAX = not yet resolved
		inline uint32_t g_accumulator = 0;

		uint32_t ResolvePercent();
	}

	/** @brief True when this render pass should be dropped for the measurement. */
	inline bool ShouldSkip()
	{
		if (detail::g_percent == UINT32_MAX) [[unlikely]]
			detail::g_percent = detail::ResolvePercent();

		if (!detail::g_percent) [[likely]]
			return false;

		// Spread evenly and deterministically, so two runs drop the same passes.
		detail::g_accumulator += detail::g_percent;

		if (detail::g_accumulator >= 100u) {
			detail::g_accumulator -= 100u;
			return true;
		}

		return false;
	}
}
