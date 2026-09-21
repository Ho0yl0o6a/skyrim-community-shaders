#include "../../src/RemixDirectionalLightMath.h"
#include <iostream>
#include <limits>

int main()
{
	unsigned checked = 0;
	auto near = [&](float actual, float expected) {
		++checked;
		if (!std::isfinite(actual) || std::abs(actual - expected) > 2e-6f) {
			std::cerr << "Mismatch: " << actual << " != " << expected << '\n';
			return false;
		}
		return true;
	};
	using namespace RemixDirectionalLightMath;
	if (!near(DefaultRadianceScale, 1.0f / 3.0f) || !ValidRadianceScale(0) || !ValidRadianceScale(16) ||
		ValidRadianceScale(-1) || ValidRadianceScale(17) ||
		ValidRadianceScale(std::numeric_limits<float>::quiet_NaN()) ||
		ValidRadianceScale(std::numeric_limits<float>::infinity())) return 11;
	std::array<float, 3> direction{ 3, -4, 0 };
	if (!Normalize(direction) || !near(direction[0], .6f) || !near(direction[1], -.8f)) return 1;
	std::array<float, 3> zero{}, invalid{ std::numeric_limits<float>::quiet_NaN(), 1, 0 };
	if (Normalize(zero) || Normalize(invalid)) return 2;
	if (!near(Radiance(.25f, .5f, 2, false, false, 0, 0, 0, false), std::pow(.25f, 1.6f))) return 3;
	if (!near(Radiance(1, 0, 2, false, false, 0, 0, 0, false), 0)) return 4;
	if (!near(Radiance(1, 1, 0, false, false, 0, 0, 0, false), 0)) return 5;
	if (!near(Radiance(2, 1, 1, false, false, 0, 0, 0, false), std::pow(2.0f, 1.6f))) return 6;
	if (!near(Radiance(.5f, 1, 2, true, false, 2, 3, 2, false), 1.5f)) return 7;
	if (!near(Radiance(.5f, 1, 2, true, false, 2, 3, 2, true), 3.0f)) return 8;
	if (!near(Radiance(.5f, 1, 2, true, true, 2, 3, 2, false), 1.0f / std::numbers::pi_v<float>)) return 9;
	// Independent integral: Remix samples L/sin^2(a); a white Lambert surface
	// receives integral(L*cos(theta)/pi dOmega) == L at normal incidence.
	for (const double degrees : { .5, 1.0, 5.0 }) {
		const double a = degrees * std::numbers::pi / 360.0;
		const double width = (1.0 - std::cos(a)) / 10000.0;
		double response = 0;
		for (unsigned i = 0; i < 10000; ++i)
			response += (std::cos(a) + (i + .5) * width) * 2 * width / std::pow(std::sin(a), 2);
		if (!near(float(response), 1)) return 10;
	}
	std::cout << checked << " directional-light scalar checks passed; not a GPU parity test.\n";
}
