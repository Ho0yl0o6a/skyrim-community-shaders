#include "../../src/RemixFoliageMaterial.h"
#include <iostream>

int main()
{
	using RemixFoliageMaterial::IsThin;
	if (!IsThin(true, false, false, false, false, false, false)) return 1;
	if (!IsThin(false, true, false, true, false, true, true)) return 2; // SOFT card.
	if (!IsThin(false, true, false, false, true, true, true)) return 3; // BACK card, no TREE_ANIM required.
	if (IsThin(false, true, false, false, false, true, true)) return 4; // RIM alone is not transmission.
	if (IsThin(false, false, true, true, true, true, true)) return 5; // Ineligible skin/hair/atlas/effect.
	if (IsThin(false, true, true, true, true, false, true)) return 6;
	if (IsThin(false, true, true, true, true, true, false)) return 7;
	if (!IsThin(false, true, true, false, false, true, true)) return 8; // Leaf without native SSS map retains thin fallback.
	std::cout << "Foliage classification: grass and leaf cards selected; bark, hair, atlas and effect controls excluded.\n";
}
