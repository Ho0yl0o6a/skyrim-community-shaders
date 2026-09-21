#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>

namespace RemixSceneAudit
{
	struct Sample
	{
		uint64_t frame = 0;
		uint32_t cell = 0, submitted = 0;
		bool interior = false, ready = false, dome = false;
		bool cameraCurrent = false, fader = false;
		// Sky property, distant trees, terrain LOD, object LOD, cloud feature,
		// terrain LOD noise, and submitted geometry absent from host membership.
		std::array<uint32_t, 7> classes{};

		bool Suspect() const
		{
			return classes.back() || (interior && (dome ||
				std::any_of(classes.begin(), classes.end(), [](auto n) { return n != 0; })));
		}
	};

	/// @brief Bounded frame evidence with cumulative counters and a preserved
	/// first suspect, so a one-frame event survives recent-history rollover.
	template <size_t Capacity = 256>
	struct History
	{
		static_assert(Capacity > 0);
		std::array<Sample, Capacity> recent{};
		uint64_t samples = 0, interiorSamples = 0, suspectSamples = 0, failedSamples = 0;
		uint64_t cellChanges = 0;
		uint64_t missingCameraSamples = 0, cameraReadyFailures = 0, emptyReadySamples = 0;
		std::array<uint32_t, 7> interiorMax{}, exteriorMax{};
		uint64_t interiorDomeSamples = 0, exteriorDomeSamples = 0;
		std::optional<Sample> first, firstSuspect, firstFailure;

		void Record(const Sample& sample)
		{
			if (samples && recent[(samples - 1) % Capacity].cell != sample.cell)
				++cellChanges;
			if (!first) first = sample;
			if (sample.Suspect()) {
				++suspectSamples;
				if (!firstSuspect) firstSuspect = sample;
			}
			interiorSamples += sample.interior;
			auto& maxima = sample.interior ? interiorMax : exteriorMax;
			for (size_t i = 0; i < maxima.size(); ++i)
				maxima[i] = std::max(maxima[i], sample.classes[i]);
			if (sample.dome) ++(sample.interior ? interiorDomeSamples : exteriorDomeSamples);
			failedSamples += !sample.ready;
			missingCameraSamples += !sample.cameraCurrent;
			cameraReadyFailures += sample.cameraCurrent && !sample.ready;
			emptyReadySamples += sample.ready && sample.submitted == 0;
			if (!sample.ready && !firstFailure) firstFailure = sample;
			recent[samples++ % Capacity] = sample;
		}

		size_t Size() const { return static_cast<size_t>(std::min<uint64_t>(samples, Capacity)); }
		const Sample& Oldest(size_t index) const { return recent[(samples - Size() + index) % Capacity]; }
	};
}
