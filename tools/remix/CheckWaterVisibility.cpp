#include "../../src/RemixWaterVisibility.h"
#include <iostream>

int main()
{
	using RemixWaterVisibility::IsRasterOverlay;
	static_assert(IsRasterOverlay(49349));  // All wading heights, hidden or active.
	static_assert(!IsRasterOverlay(49348));  // Ordinary procedural water.
	static_assert(!IsRasterOverlay(16450));  // Distant water remains available.
	static_assert(!IsRasterOverlay(16580));  // Authored trough water.
	for (uint32_t flags = 0; flags <= 0x1ffff; ++flags) {
		if (IsRasterOverlay(flags) != ((flags & 1u) != 0))
			return 1;
	}
	std::cout << "Water pass classification: 131072 flag combinations passed.\n";
}
