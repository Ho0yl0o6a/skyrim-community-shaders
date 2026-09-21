#pragma once

#include <array>
#include <cstdint>

namespace RemixTangentFrame
{
	// Lighting.hlsl packs T.x in position.w, T.y/z in normal/binormal.w.
	inline std::array<float, 6> Decode(float positionW, const uint8_t* normal, const uint8_t* binormal)
	{
		return { positionW, normal[3] / 127.5f - 1.0f, binormal[3] / 127.5f - 1.0f,
			binormal[0] / 127.5f - 1.0f, binormal[1] / 127.5f - 1.0f, binormal[2] / 127.5f - 1.0f };
	}
}
