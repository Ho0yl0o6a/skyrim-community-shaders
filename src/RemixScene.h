#pragma once

#include <remix/remix_c.h>
#include <string>

namespace RE
{
	class BSGeometry;
	class NiPoint3;
}

namespace RemixScene
{
	bool Initialize(remixapi_Interface* api);
	void RecordGrassScale(RE::BSGeometry* geometry, const RE::NiPoint3& scale);
	void RecordGrassWind(RE::BSGeometry* geometry, const RE::NiPoint3& wind, float timer, float previousTimer);

	/// Keeps the tree wind the game bound for this technique. See RemixBridge.
	void RecordTreeWind(const float* treeParams, const float* windTimers);
	// Called from inside the game's world frame. The light arrays it reads are
	// only stable there; see CaptureSceneLights in RemixScene.cpp.
	void CaptureSceneLights();
	// Freeze view-model pose beside its native camera before later updates.
	void CaptureViewModelPose();
	// Opt-in pose timing evidence at the native first-person camera call.
	void AuditNativeViewModelPose();
	bool SetDirectionalLightScale(float scale);
	void SetTreeVertexBakedLighting(bool enabled);
	bool Submit();
	/// @brief Whether distant-tree LOD groups the game has culled are submitted.
	///
	/// Grass groups are kept regardless of the engine's culling, because
	/// off-screen grass still casts shadows and shows in reflections. Distant
	/// trees are different: the same flag also carries the engine's decision to
	/// stop drawing a group of billboards because the full trees for that cell
	/// are loaded, and resurrecting those puts a second, differently oriented
	/// copy of every tree into the scene.
	void SetDistantTreeCullingRespected(bool respected);

	/// @brief Whether a distant-tree group whose placements lie outside its own
	///        bound is rejected. On by default; only a test turns it off.
	void SetStrayTreeGroupGuard(bool enabled);
	/// @brief Displaces every distant-tree group's placements into the next cell.
	///
	/// Manufactures the fault the guard exists for. A group that has picked up
	/// another cell's placements cannot be produced by playing the game -- it
	/// comes of a bad first read that is then cached for the session -- so this
	/// is how the guard is watched doing its job.
	void SetStrayTreeGroupInjection(bool enabled);
	void DiscardFrame();
	/// @brief Enable/reset or freeze opt-in per-world-frame membership evidence.
	void SetAuditEnabled(bool enabled);
	void RecordAudit(bool ready, bool domeSubmitted);
	std::string Inspect(const std::string& filter);
}
