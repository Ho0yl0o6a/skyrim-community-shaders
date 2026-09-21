#pragma once

namespace RemixCameraMath
{
	// Row-vector view matrix: (world - origin) * relativeView == world * result.
	// Double intermediates avoid losing low translation bits in the dot products.
	inline void RestoreWorldViewTranslation(float* view, const float* origin)
	{
		for (unsigned column = 0; column < 4; ++column) {
			double translation = view[12 + column];
			for (unsigned row = 0; row < 3; ++row)
				translation -= static_cast<double>(origin[row]) * view[row * 4 + column];
			view[12 + column] = static_cast<float>(translation);
		}
	}
}
