#include "RemixSceneGraph.h"
#include "RemixNativeRender.h"
#include "RemixNodeChildren.h"

namespace RemixSceneGraph
{
	namespace
	{
		struct Pending
		{
			RE::NiAVObject* object;
			bool firstPerson;
			bool playerBody;
		};

		// Working buffers persist so a steady-state frame performs no allocation.
		// Their capacity, not their contents, is what is reused.
		Snapshot snapshot;
		std::vector<RE::NiAVObject*> roots;
		PointerSet excluded;
		std::vector<Pending> pending;
		PointerSet visited;
	}

	const Snapshot& Gather()
	{
		snapshot.geometry.Clear();
		snapshot.firstPerson.Clear();
		snapshot.playerBody.Clear();
		snapshot.references = snapshot.nodes = snapshot.appCulled = 0;
		roots.clear();
		excluded.Clear();
		pending.clear();
		visited.Clear();

		auto* tes = RE::TES::GetSingleton();
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!tes || !player || !player->GetParentCell())
			return snapshot;
		auto* activeCell = player->GetParentCell();
		const bool interior = activeCell->IsInteriorCell();

		// Reference/cell attachment is authoritative. No radius, render pass,
		// frustum, occlusion, shadow-view or transient AppCulled filters.
		// WorldRoot also retains exterior LOD and sky-cell objects indoors.
		// Interior membership comes from the active cell, not that global root.
		if (!interior) {
			if (auto* root = RE::Main::WorldRootNode())
				roots.emplace_back(root);
		} else if (tes->tempNodeManager) {
			roots.emplace_back(tes->tempNodeManager);
		}
		tes->ForEachCell([&](RE::TESObjectCELL* cell) {
			if (interior && cell != activeCell)
				return;
			if (const auto* data = cell->GetRuntimeData().loadedData) {
				if (data->cell3D)
					roots.emplace_back(data->cell3D.get());
				if (data->lightMarkerNode)
					excluded.Insert(data->lightMarkerNode.get());
				if (data->soundMarkerNode)
					excluded.Insert(data->soundMarkerNode.get());
			}
		});
		tes->ForEachReference([&](RE::TESObjectREFR* reference) {
			if (interior && reference && reference->GetParentCell() != activeCell)
				return RE::BSContainer::ForEachResult::kContinue;
			if (auto* root = reference ? reference->Get3D(false) : nullptr) {
				auto* base = reference->GetBaseObject();
				// IsMarker is the native authored-object predicate (0x275e10),
				// including heading/occlusion/light markers and marker-flagged forms.
				if (reference->IsDisabled() || reference->IsDeleted() || (base && base->IsMarker())) {
					excluded.Insert(root);
				} else {
					++snapshot.references;
					roots.emplace_back(root);
				}
			}
			return RE::BSContainer::ForEachResult::kContinue;
		});

		auto* firstPersonRoot = player->Get3D(true);
		auto* bodyRoot = player->Get3D(false);
		pending.reserve(roots.size() + 2);
		for (auto* root : roots)
			pending.push_back({ root, false, false });
		if (firstPersonRoot && firstPersonRoot != bodyRoot)
			pending.push_back({ firstPersonRoot, true, false });
		if (bodyRoot)
			pending.push_back({ bodyRoot, false, true });
		while (!pending.empty()) {
			auto item = pending.back();
			pending.pop_back();
			auto* object = item.object;
			if (!object || excluded.Contains(object) || !visited.Insert(object))
				continue;
			++snapshot.nodes;
			item.firstPerson |= object == firstPersonRoot && firstPersonRoot != bodyRoot;
			item.playerBody |= object == bodyRoot;
			if (auto* geometry = object->AsGeometry()) {
				snapshot.geometry.Insert(geometry);
				if (item.firstPerson)
					snapshot.firstPerson.Insert(geometry);
				if (item.playerBody)
					snapshot.playerBody.Insert(geometry);
				snapshot.appCulled += geometry->GetAppCulled();
			} else if (auto* node = object->AsNode()) {
				const auto children = ChildSlots(node->GetChildren());
				// A switch selects mutually exclusive authored states. Unlike
				// AppCulled, its active index is not produced by OnVisible's
				// frustum test (see the Ghidra NiSwitchNode::OnVisible audit).
				if (auto* selection = object->AsSwitchNode()) {
					const auto index = RemixNativeRender::ReadSwitchIndex(selection);
					if (index >= 0 && size_t(index) < children.size())
						pending.push_back({ children[size_t(index)].get(), item.firstPerson, item.playerBody });
				} else {
					for (const auto& child : children)
						if (child)
							pending.push_back({ child.get(), item.firstPerson, item.playerBody });
				}
			}
		}
		return snapshot;
	}
}
