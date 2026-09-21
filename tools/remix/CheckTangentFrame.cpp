#include "../../src/RemixTangentFrame.h"

#include <cmath>
#include <iostream>
#include <stdexcept>

int main()
{
	try {
		const uint8_t normal[]{ 128, 128, 255, 0 };
		const uint8_t binormal[]{ 255, 64, 0, 255 };
		const auto result = RemixTangentFrame::Decode(-0.25f, normal, binormal);
		const float expected[]{ -0.25f, -1.0f, 1.0f, 1.0f, -0.498039216f, -1.0f };
		for (size_t i = 0; i < result.size(); ++i)
			if (!std::isfinite(result[i]) || std::abs(result[i] - expected[i]) > 1e-6f)
				throw std::runtime_error("Authored tangent component mismatch");
		// Authored handedness is data; do not replace B with a cross product.
		const uint8_t opposite[]{ 0, 191, 255, 0 };
		const auto mirrored = RemixTangentFrame::Decode(0.25f, normal, opposite);
		if (mirrored[3] != -1 || mirrored[5] != 1 || mirrored[2] != -1)
			throw std::runtime_error("Mirrored basis channels lost");
		std::cout << "Native T/B packed channel decoding passed (CPU only).\n";
		return 0;
	} catch (const std::exception& error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
