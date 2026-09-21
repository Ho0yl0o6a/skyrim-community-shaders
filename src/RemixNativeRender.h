#pragma once

namespace RemixNativeRender
{
	// Independent, exact-build game-code hooks; no graphics API interception.
	void Install();
	// The exact world SetCameraData call, before first-person/image-space passes.
	// False unless the snapshot belongs to the current graphics frame.
	bool ReadWorldCamera(RE::BSGraphics::ViewData& eye, RE::NiPoint3& origin);
	// Independent native scene-camera/player positions copied at that call.
	bool ReadWorldCameraPositions(RE::NiPoint3& camera, RE::NiPoint3& player);
	// First-person pass camera, adjusted to the model's restored coordinates.
	bool ReadViewModelCamera(RE::BSGraphics::ViewData& eye, RE::NiPoint3& origin, RE::NiPoint3* modelTranslation = nullptr);
	// Distant trees bind a shader-global atlas, not a property-local texture.
	RE::NiSourceTexture* ReadDistantTreeAtlas();
	// Terrain manager roots, not TES::objRoot (which also owns indoor objects).
	std::array<RE::NiAVObject*, 4> ReadExteriorLodRoots();
	int32_t ReadSwitchIndex(const RE::NiSwitchNode* node);
	struct WaterGlobals {
		RE::NiSourceTexture* flowAtlas = nullptr;
		RE::NiSourceTexture* defaultNormal = nullptr;
		uint32_t gridDimension = 0;
		float time = 0;
		std::array<float, 4> wadingCell{};
		std::array<float, 2> wadingOrigin{};
	};
	WaterGlobals ReadWaterGlobals();
	uint32_t ReadEffectUVIndex();
	struct ParticleVertex {
		RE::NiPoint3 position;
		float textureIndex;
		uint32_t rgba;
	};
	// Exact native CPU billboard construction; never maps or draws a GPU buffer.
	bool BuildParticleVertices(RE::NiParticleSystem* system, std::vector<ParticleVertex>& vertices);
	// Read the audited grass shader inputs without invoking visibility/render passes.
	bool ReadGrassWind(const RE::BSGeometry* geometry, const RE::BSGrassShaderProperty* property, RE::NiPoint3& wind, float& timer);
}
