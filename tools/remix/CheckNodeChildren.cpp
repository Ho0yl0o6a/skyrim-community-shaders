#include "../../src/RemixNodeChildren.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>

struct ChildArray
{
	using value_type = int*;
	int** data;
	uint16_t allocated;
	uint16_t highWater;
	const value_type* begin() const { return data; }
	uint16_t capacity() const { return allocated; }
	uint16_t free_idx() const { return highWater; }
};

int main()
{
	try {
		if (!RemixSceneGraph::ChildSlots(ChildArray{ nullptr, 1, 1 }).empty())
			throw std::runtime_error("Unallocated node exposed a child slot");
		int a = 1, b = 2;
		std::array<int*, 6> data{ &a, nullptr, nullptr, &b, nullptr, nullptr };
		const auto slots = RemixSceneGraph::ChildSlots(ChildArray{ data.data(), 6, 4 });
		if (slots.size() != 4 || slots[0] != &a || slots[1] || slots[2] || slots[3] != &b)
			throw std::runtime_error("Sparse switch indices or high-water bound changed");
		if (RemixSceneGraph::ChildSlots(ChildArray{ data.data(), 6, 9 }).size() != 6)
			throw std::runtime_error("Traversal exceeds allocation");
		if (!RemixSceneGraph::ChildSlots(ChildArray{ data.data(), 6, 0 }).empty())
			throw std::runtime_error("Empty node exposes reserved capacity");
		std::cout << "Node child allocation and sparse-slot bounds passed.\n";
		return 0;
	} catch (const std::exception& error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
