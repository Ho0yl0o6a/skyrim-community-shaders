#pragma once

#include <cstdint>

namespace RemixWaterVisibility
{
	// Bit 0 selects the stencil-masked player-centred WADING raster pass.
	// It is not a physical boundary: ripples belong on the actual water surface.
	constexpr bool IsRasterOverlay(uint32_t waterFlags)
	{
		return (waterFlags & 1u) != 0;
	}
}
