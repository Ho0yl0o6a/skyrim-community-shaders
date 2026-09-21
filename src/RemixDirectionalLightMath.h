#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace RemixDirectionalLightMath
{
	inline constexpr float DefaultRadianceScale = 1.0f / 3.0f;

	inline bool ValidRadianceScale(float scale)
	{
		return std::isfinite(scale) && scale >= 0.0f && scale <= 16.0f;
	}

	inline bool Normalize(std::array<float, 3>& direction)
	{
		float lengthSquared = 0;
		for (const float component : direction) {
			if (!std::isfinite(component)) return false;
			lengthSquared += component * component;
		}
		if (!std::isfinite(lengthSquared) || lengthSquared < 1e-12f) return false;
		const float inverseLength = 1.0f / std::sqrt(lengthSquared);
		for (auto& component : direction) component *= inverseLength;
		return true;
	}

	// Unit-white, normal-incidence Lambert response. See remix-directional-light.md
	// for the native gamma-lighting limitation and Remix's cone normalization.
	inline float Radiance(float diffuse, float fade, float sunlightScale, bool linearLighting,
		bool alreadyLinear, float gamma, float directionalMultiplier, float gammaScale, bool interior)
	{
		const float coefficient = std::max(0.0f, diffuse) * std::max(0.0f, fade) * std::max(0.0f, sunlightScale);
		if (!linearLighting) return std::pow(coefficient, 1.6f);
		if (alreadyLinear) return coefficient / std::numbers::pi_v<float>;
		const float multiplier = interior ? 1.0f : gammaScale;
		return std::pow(coefficient / std::max(multiplier, 1e-5f), gamma) * directionalMultiplier * multiplier;
	}
}
