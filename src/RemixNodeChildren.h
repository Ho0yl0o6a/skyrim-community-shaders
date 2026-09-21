#pragma once

#include <algorithm>
#include <span>

namespace RemixSceneGraph
{
	/// @brief Visits allocated child slots, including holes below the high-water index.
	template <class Array>
	auto ChildSlots(const Array& children)
	{
		using Value = typename Array::value_type;
		const auto* data = children.begin();
		// NiTArray::end() uses capacity, even when the backing allocation is null.
		const size_t count = data ? std::min(children.free_idx(), children.capacity()) : 0;
		return std::span<const Value>(data, count);
	}
}
