#pragma once

#include <vector>

namespace RemixSceneGraph
{
	/**
	 * @brief Open-addressed pointer set sized for per-frame scene traversal.
	 *
	 * The retained-scene walk inserts and probes tens of thousands of node
	 * pointers every frame. std::unordered_set spends most of that time chasing
	 * bucket lists; linear probing over one contiguous table keeps the whole
	 * working set in cache. Capacity is retained across frames so a steady-state
	 * frame performs no allocation.
	 */
	class PointerSet
	{
	public:
		void Clear()
		{
			// Rehashing an empty table is cheaper than clearing a large one only
			// when the table is oversized; in practice the scene size is stable.
			std::fill(slots.begin(), slots.end(), nullptr);
			used = 0;
		}

		void Reserve(size_t expected)
		{
			size_t capacity = 64;
			while (capacity < expected * 2)
				capacity <<= 1;
			if (capacity > slots.size()) {
				slots.assign(capacity, nullptr);
				used = 0;
			}
		}

		/// @brief Inserts a pointer, returning false when it was already present.
		bool Insert(const void* key)
		{
			if ((used + 1) * 2 >= slots.size())
				Grow();
			auto index = Hash(key) & (slots.size() - 1);
			while (slots[index]) {
				if (slots[index] == key)
					return false;
				index = (index + 1) & (slots.size() - 1);
			}
			slots[index] = key;
			++used;
			return true;
		}

		bool Contains(const void* key) const
		{
			if (!used)
				return false;
			auto index = Hash(key) & (slots.size() - 1);
			while (slots[index]) {
				if (slots[index] == key)
					return true;
				index = (index + 1) & (slots.size() - 1);
			}
			return false;
		}

		bool Empty() const { return used == 0; }
		size_t Size() const { return used; }

	private:
		static size_t Hash(const void* key)
		{
			// Fibonacci hashing spreads the low-entropy low bits of aligned
			// allocations across the table without a multiply-heavy finalizer.
			return static_cast<size_t>((reinterpret_cast<uintptr_t>(key) >> 4) * 0x9e3779b97f4a7c15ull >> 32);
		}

		void Grow()
		{
			std::vector<const void*> previous(slots.size() ? slots.size() * 2 : 64, nullptr);
			previous.swap(slots);
			used = 0;
			for (const auto* key : previous)
				if (key)
					Insert(key);
		}

		std::vector<const void*> slots = std::vector<const void*>(1024, nullptr);
		size_t used = 0;
	};
}
