#pragma once

namespace RemixFoliageMaterial
{
	// Whole-tree LOD atlases mix solid bark and leaves; they need a separate mask.
	constexpr bool IsThin(bool grass, bool lighting, bool treeAnimation, bool softLighting, bool backLighting, bool twoSided, bool alphaTest)
	{
		return grass || (lighting && (treeAnimation || softLighting || backLighting) && twoSided && alphaTest);
	}
}
