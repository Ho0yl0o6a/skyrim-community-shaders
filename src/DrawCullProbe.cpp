#include "DrawCullProbe.h"

namespace DrawCullProbe::detail
{
	uint32_t ResolvePercent()
	{
		char buf[8]{};
		const DWORD n = ::GetEnvironmentVariableA("CS_CULL_PROBE", buf, sizeof(buf));

		if (!n || n >= sizeof(buf))
			return 0;

		const int value = std::atoi(buf);
		const uint32_t percent = uint32_t(std::clamp(value, 0, 100));

		if (percent)
			logger::info("[DrawCullProbe] dropping {}% of render passes -- measurement only", percent);

		return percent;
	}
}
