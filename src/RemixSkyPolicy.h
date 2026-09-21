#pragma once

#include <cstdint>
#include <optional>

namespace RemixSkyPolicy
{
	// Mode 2 has a native dome, but the reflection cube can still hold exterior
	// weather there. Only full-sky mode supplies this weather-image path.
	constexpr bool CanSubmitWeatherCube(bool hasCell, bool interior, bool showSky, uint32_t mode, bool hidden)
	{
		return hasCell && (!interior || showSky) && mode == 3 && !hidden;
	}

	struct SettingOverride
	{
		std::optional<bool> original;

		void Apply(bool& setting, bool value)
		{
			if (!original)
				original = setting;
			setting = value;
		}

		void Restore(bool& setting)
		{
			if (original) {
				setting = *original;
				original.reset();
			}
		}
	};
}
