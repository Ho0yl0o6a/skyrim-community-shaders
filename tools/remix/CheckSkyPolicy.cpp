#include "../../src/RemixSkyPolicy.h"
#include <iostream>

int main()
{
	using namespace RemixSkyPolicy;
	uint32_t cases = 0;
	for (uint32_t mode = 0; mode < 6; ++mode) {
		for (uint32_t bits = 0; bits < 16; ++bits) {
			const bool cell = bits & 1, interior = bits & 2, show = bits & 4, hidden = bits & 8;
			const bool expected = cell && !(interior && !show) && !hidden && mode == 3;
			if (CanSubmitWeatherCube(cell, interior, show, mode, hidden) != expected)
				return 1;
			++cases;
		}
	}
	for (bool initial : { false, true }) {
		for (bool forced : { false, true }) {
			SettingOverride state;
			bool setting = initial;
			state.Restore(setting); // No captured value must not alter the setting.
			if (setting != initial) return 2;
			state.Apply(setting, forced);
			if (setting != forced) return 3;
			setting = !forced; // Simulate an INI reload while Remix owns the sky.
			state.Apply(setting, forced);
			if (setting != forced) return 4;
			state.Restore(setting);
			if (setting != initial || state.original) return 5;
			setting = !initial;
			state.Restore(setting);
			if (setting != !initial) return 6;
			state.Apply(setting, forced); // A new ownership period captures anew.
			state.Restore(setting);
			if (setting != !initial) return 7;
		}
	}
	std::cout << cases << " sky visibility cases and reflection override/reload/restore cases passed.\n";
}
