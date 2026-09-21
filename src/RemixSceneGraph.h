#pragma once

#include "RemixPointerSet.h"

namespace RemixSceneGraph
{
	/**
	 * @brief Iterable membership set of scene geometry.
	 *
	 * The retained scene needs both a stable iteration order and constant-time
	 * membership for tens of thousands of pointers every frame, so the list and
	 * the probe table are kept side by side rather than paying for a node-based
	 * hash container.
	 */
	class GeometrySet
	{
	public:
		void Clear()
		{
			entries.clear();
			table.Clear();
		}

		void Insert(RE::BSGeometry* geometry)
		{
			if (table.Insert(geometry))
				entries.push_back(geometry);
		}

		bool Contains(const RE::BSGeometry* geometry) const { return table.Contains(geometry); }
		size_t Size() const { return entries.size(); }
		auto begin() const { return entries.begin(); }
		auto end() const { return entries.end(); }

	private:
		std::vector<RE::BSGeometry*> entries;
		PointerSet table;
	};

	struct Snapshot
	{
		// Traversal results are raw pointers: the walk runs synchronously on the
		// render thread, and retained NiPointer ownership lives in RemixScene's
		// loadedGeometry map. Taking a reference on every node every frame cost
		// tens of thousands of atomic increments per frame for no added safety.
		GeometrySet geometry;
		GeometrySet firstPerson;
		GeometrySet playerBody;
		uint32_t references = 0;
		uint32_t nodes = 0;
		uint32_t appCulled = 0;  // Diagnostic only, never a lifetime predicate.
	};

	/// @brief Walks every attached cell/reference/player root and returns the
	///        retained-scene census. Reuses its working buffers across frames.
	const Snapshot& Gather();
}
