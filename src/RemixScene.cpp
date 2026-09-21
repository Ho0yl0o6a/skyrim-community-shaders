#include "RemixScene.h"

#include "Globals.h"
#include "State.h"
#include "RemixCameraMath.h"
#include "RemixDirectionalLightMath.h"
#include "RemixTangentFrame.h"
#include "Features/LinearLighting.h"
#include "Features/GrassLighting.h"
#include "Features/LodBlending.h"
#include "RemixMaterial.h"
#include "RemixFoliageMaterial.h"
#include "RemixWaterVisibility.h"
#include "RemixNativeRender.h"
#include "RemixSceneGraph.h"
#include "RemixSceneAudit.h"
#include "Util.h"
#include <RE/N/NiParticleSystem.h>
#include <RE/N/NiPSysData.h>
#include <RE/B/BSTreeNode.h>
#include <DirectXPackedVector.h>
#include <nlohmann/json.hpp>
#include <optional>
#include <atomic>
#include <deque>

namespace RemixScene
{
	namespace
	{
		using Microsoft::WRL::ComPtr;
		remixapi_Interface* api = nullptr;
		using ImportMaterialFn = remixapi_ErrorCode(REMIXAPI_CALL*)(const remixapi_MaterialInfo*, ID3D11ShaderResourceView*, ID3D11ShaderResourceView*, uint32_t, remixapi_MaterialHandle*);
		ImportMaterialFn importMaterial = nullptr;
		using ImportThinMaterialFn = remixapi_ErrorCode(REMIXAPI_CALL*)(const remixapi_MaterialInfo*, ID3D11ShaderResourceView*, ID3D11ShaderResourceView*, ID3D11ShaderResourceView*, uint32_t, remixapi_MaterialHandle*);
		ImportThinMaterialFn importThinMaterial = nullptr;
		ImportThinMaterialFn importSkinMaterial = nullptr;
		using ImportFoliageFn = remixapi_ErrorCode(REMIXAPI_CALL*)(const remixapi_MaterialInfo*, ID3D11ShaderResourceView*, ID3D11ShaderResourceView*,
			ID3D11ShaderResourceView*, ID3D11ShaderResourceView*, uint32_t, uint32_t, const float*, remixapi_MaterialHandle*);
		ImportFoliageFn importFoliage = nullptr;
		using ImportLandscapeFn = remixapi_ErrorCode(REMIXAPI_CALL*)(const remixapi_MaterialInfo*, ID3D11ShaderResourceView* const*, ID3D11ShaderResourceView* const*, uint32_t, remixapi_MaterialHandle*);
		ImportLandscapeFn importLandscape = nullptr;
		using ImportWaterFn = remixapi_ErrorCode(REMIXAPI_CALL*)(const remixapi_MaterialInfo*, ID3D11ShaderResourceView* const*, const float*, remixapi_MaterialHandle*);
		using UpdateWaterFn = remixapi_ErrorCode(REMIXAPI_CALL*)(remixapi_MaterialHandle, const float*);
		ImportWaterFn importWater = nullptr;
		UpdateWaterFn updateWater = nullptr;
		using ImportEffectFn = remixapi_ErrorCode(REMIXAPI_CALL*)(const remixapi_MaterialInfo*, ID3D11ShaderResourceView*, ID3D11ShaderResourceView*, uint32_t, const float*, remixapi_MaterialHandle*);
		ImportEffectFn importEffect = nullptr;
		UpdateWaterFn updateEffect = nullptr;
		using CreateModelNormalMeshFn = remixapi_ErrorCode(REMIXAPI_CALL*)(const remixapi_MeshInfo*, remixapi_MeshHandle*);
		CreateModelNormalMeshFn createModelNormalMesh = nullptr;
		using UpdateMeshFn = remixapi_ErrorCode(REMIXAPI_CALL*)(const remixapi_MeshInfo*, const float*, uint32_t);
		UpdateMeshFn updateMesh = nullptr;
		using CreateTangentMeshFn = remixapi_ErrorCode(REMIXAPI_CALL*)(const remixapi_MeshInfo*, const float*, uint32_t, uint32_t, remixapi_MeshHandle*);
		CreateTangentMeshFn createTangentMesh = nullptr;
		using CreateNativeMeshFn = remixapi_ErrorCode(REMIXAPI_CALL*)(const remixapi_MeshInfo*, uint32_t, remixapi_MeshHandle*);
		CreateNativeMeshFn createNativeMesh = nullptr;
		using CreateInstanceSetFn = remixapi_ErrorCode(REMIXAPI_CALL*)(const remixapi_Transform*, const float*, uint32_t, uint64_t*);
		using DestroyInstanceSetFn = remixapi_ErrorCode(REMIXAPI_CALL*)(uint64_t);
		using DrawInstanceSetFn = remixapi_ErrorCode(REMIXAPI_CALL*)(const remixapi_InstanceInfo*, uint64_t);
		CreateInstanceSetFn createInstanceSet = nullptr;
		DestroyInstanceSetFn destroyInstanceSet = nullptr;
		DrawInstanceSetFn drawInstanceSet = nullptr;
		using CreateGrassInstanceSetFn = remixapi_ErrorCode(REMIXAPI_CALL*)(const remixapi_Transform*, const float*, const float*, const float*, uint32_t, uint64_t*);
		using DrawGrassInstanceSetFn = remixapi_ErrorCode(REMIXAPI_CALL*)(const remixapi_InstanceInfo*, uint64_t, const float*);
		CreateGrassInstanceSetFn createGrassInstanceSet = nullptr;
		DrawGrassInstanceSetFn drawGrassInstanceSet = nullptr;
		// Retained instances: register once, report only genuine changes, and
		// replay the whole table with a single call per frame. Transforms are
		// absolute; the runtime applies the camera-origin rebasing, so a static
		// object is never resubmitted merely because the camera moved.
		using CreateRetainedFn = remixapi_ErrorCode(REMIXAPI_CALL*)(const remixapi_InstanceInfo*, uint64_t*);
		using UpdateRetainedFn = remixapi_ErrorCode(REMIXAPI_CALL*)(const remixapi_InstanceInfo*, uint64_t, uint64_t);
		using DestroyRetainedFn = remixapi_ErrorCode(REMIXAPI_CALL*)(uint64_t);
		using DrawRetainedFn = remixapi_ErrorCode(REMIXAPI_CALL*)(const float*);
		// An object that only moved needs no new description: re-sending one costs
		// the runtime a full draw-state rebuild (geometry hash, material resolve,
		// instance update), which for the few hundred objects that move each frame
		// was the largest single cost of replaying the retained table.
		using UpdateRetainedTransformFn = remixapi_ErrorCode(REMIXAPI_CALL*)(uint64_t, const remixapi_Transform*);
		using CreateRetainedSetFn = remixapi_ErrorCode(REMIXAPI_CALL*)(const remixapi_InstanceInfo*, uint64_t, uint64_t*);
		CreateRetainedFn createRetainedInstance = nullptr;
		CreateRetainedSetFn createRetainedInstanceSet = nullptr;
		UpdateRetainedFn updateRetainedInstance = nullptr;
		UpdateRetainedTransformFn updateRetainedTransform = nullptr;
		DestroyRetainedFn destroyRetainedInstance = nullptr;
		DrawRetainedFn drawRetainedInstances = nullptr;
		std::unordered_set<uint64_t> instanceSetHandles;
		remixapi_MaterialHandle material = nullptr;
		remixapi_LightHandle sun = nullptr;
		std::atomic<float> directionalRadianceScale{ RemixDirectionalLightMath::DefaultRadianceScale };
		struct DirectionalLight
		{
			bool valid = false;
			bool interior = false;
			bool linearLighting = false;
			bool alreadyLinear = false;
			uint32_t engineFrame = 0;
			RE::FormID cell = 0;
			std::array<float, 3> nativeDirection{}, diffuse{}, direction{}, radiance{};
			float fade = 0, sunlightScale = 1, gamma = 0, directionalMultiplier = 0, gammaScale = 1;
			float radianceScale = 1;
		};
		DirectionalLight capturedDirectional, submittedDirectional;
		std::array<float, 3> registeredSunDirection{}, registeredSunRadiance{};
		bool directionalSubmitted = false;
		uint32_t directionalSubmitEngineFrame = 0;
		// Skyrim's own point and spot lights. Remix lights the froxel volume from
		// lights alone -- an emissive surface contributes nothing to it -- so a
		// torch only lights the fog around it if the game's light reaches Remix.
		struct SceneLight
		{
			remixapi_Float3D position{};
			remixapi_Float3D radiance{};
			uint64_t lastSeenFrame = 0;
		};
		std::unordered_map<uint64_t, SceneLight> sceneLights;
		// Captured inside the world frame, consumed at submit. Plain values only:
		// nothing here keeps a game object alive or outlives the frame it came
		// from. See CaptureSceneLights for why it cannot be read at submit time.
		struct CapturedLight
		{
			uint64_t handle = 0;
			remixapi_Float3D position{};
			remixapi_Float3D radiance{};
		};
		std::vector<CapturedLight> capturedLights;
		std::mutex capturedLightsMutex;
		uint32_t capturedOffered = 0, capturedShadow = 0;
		uint32_t capturedRejectedHidden = 0, capturedRejectedMaskless = 0, capturedRejectedDark = 0;
		uint32_t sceneLightsThisFrame = 0;
		uint32_t sceneLightsUpdatedThisFrame = 0;
		uint64_t nextHash = 0x43534d4553480000ull;
		uint64_t frame = 0;
		uint32_t createdThisFrame = 0;
		uint32_t invalidatedThisFrame = 0;
		uint32_t failedUploadsThisFrame = 0;
		uint32_t changedInstancesThisFrame = 0;
		uint32_t movedInstancesThisFrame = 0;
		uint32_t rebuiltPlacementsThisFrame = 0;
		uint32_t instanceReadbacksThisFrame = 0;
		std::mutex mutex;
		// Process-local diagnostic; only full-tree vertex colour is normalized.
		bool treeVertexBakedLighting = false;
		std::atomic<bool> auditEnabled = false;
		RemixSceneAudit::History<> audit;
		std::deque<nlohmann::json> characterAudit;
		nlohmann::json bonePhaseAudit;
		std::unordered_map<RE::BSGeometry*, RE::NiTransform> entryViewModelTransforms;
		std::unordered_map<RE::BSGeometry*, RE::NiTransform> attachmentAuditTransforms;
		std::unordered_map<RE::BSGeometry*, std::string> attachmentAuditAnchors;
		// Rolling CPU cost of each submission phase. The frame rate is well below
		// the measured Remix GPU time, so the host-side cost needs its own
		// evidence rather than being inferred from the GPU profiler.
		struct PhaseTiming
		{
			std::array<double, 120> samples{};
			uint32_t count = 0;
			void Add(double ms) { samples[count++ % samples.size()] = ms; }
			double Median() const { return Percentile(0.5); }
			/// @brief The frame-to-frame period's tail, not its middle.
			///
			/// Smoothness is a question about the worst frames, and a median
			/// answers a different one: a run that alternates 14 ms and 34 ms has
			/// the same median as a steady 24 ms and does not look remotely alike.
			double Percentile(double fraction) const
			{
				const auto used = std::min<size_t>(count, samples.size());
				if (!used)
					return 0;
				std::array<double, 120> sorted = samples;
				std::sort(sorted.begin(), sorted.begin() + used);
				const auto index = std::min<size_t>(used - 1, static_cast<size_t>(fraction * used));
				return sorted[index];
			}
		};
		PhaseTiming gatherTiming, captureTiming, submitTiming, restTiming, periodTiming;
		// The game thread, not the command stream, sets the frame rate here: the
		// command-stream thread idles waiting for work. period is the wallclock
		// between successive Submit calls and rest is period minus the phases
		// below, i.e. everything the game does with the thread outside this hook.
		std::optional<std::chrono::steady_clock::time_point> lastSubmitEnd, lastGatherStart;
		bool previousFirstPersonView = false;
		RE::NiPoint3 lastEyeWorld{};
		bool haveLastEyeWorld = false;
		uint64_t impossibleEyeJumps = 0;
		// Sprinting covers roughly 20 units a frame and fast travel is a load, so
		// a few hundred is comfortably beyond anything real while still catching
		// a jump to somewhere else in the worldspace.
		constexpr float kImpossibleEyeJump = 400.0f;
		// Frames between full scene-graph walks. Membership changes when cells
		// stream, not when objects move, and everything a retained object does
		// is detected by probing it every frame regardless.
		constexpr uint64_t kSceneDiscoveryInterval = 4;
		std::vector<RE::BSGeometry*> captureOrder;
		std::unordered_set<RE::BSGeometry*> playerBody;
		std::unordered_set<RE::BSGeometry*> viewModelGeometry;
		uint32_t submittedViewModels = 0;
		bool submittedViewModelCamera = false;
		RE::NiPoint3 submittedViewModelEye{};
		uint32_t sceneNodes = 0, sceneReferences = 0, sceneAppCulled = 0, sceneFirstPerson = 0;
		uint32_t probeHitsThisFrame = 0;
		uint32_t probeMissesThisFrame = 0;
		uint32_t probeReachedArmThisFrame = 0;
		uint32_t probeArmedThisFrame = 0;
		uint32_t probeChangedThisFrame = 0;
		uint32_t probeStaggeredThisFrame = 0;
		uint32_t probeMaxStableFrames = 0;
		std::array<uint32_t, 11> probeIneligibleReasons{};
		// Instances that took the full re-describe path this frame, by the same
		// categories. A parked camera with the clock stopped still re-describes a
		// few hundred of them, and whether that is fire and NPCs genuinely
		// animating or work that could be avoided is not answerable from the
		// total alone.
		std::array<uint32_t, 11> changedReasons{};
		constexpr const char* probeReasonNames[]{ "notTriShape", "water", "effect", "grass", "distantTree",
			"hairTint", "skinned", "lodLandscape", "nativeLandscape", "multiStream", "dynamic" };
		struct InstanceGroupCache
		{
			ComPtr<ID3D11Buffer> buffer;
			uint32_t count = 0;
			uint32_t stride = 0;
			// The cell the placements in here belong to. An immutable buffer with
			// no CPU copy is reused on its COM pointer alone -- the contents are
			// never re-read -- and Skyrim hands the same pointer back for another
			// cell's trees once the first one has streamed out. The group's own
			// bound is what says which cell this is.
			RE::NiPoint3 center{};
			RE::NiPoint3 size{};
			std::vector<RE::NiTransform> transforms;
			std::vector<float> scaleVariations;
			std::vector<float> brightness;
			std::vector<uint8_t> sourceBytes;
		};
		struct PlacementSet
		{
			std::vector<remixapi_Transform> transforms;
			std::vector<float> brightness;
			std::vector<float> grassPhases;
			std::vector<float> grassNormalRows;
			uint64_t handle = 0;
		};
		struct Mesh
		{
			ComPtr<ID3D11Buffer> vertices;
			ComPtr<ID3D11Buffer> indices;
			remixapi_MeshHandle handle = nullptr;
			remixapi_MaterialHandle materialHandle = nullptr;
			uint64_t descriptor = 0;
			uint32_t vertexCount = 0;
			uint32_t indexCount = 0;
			uint32_t sourceIndexCount = 0;
			std::vector<std::pair<uint32_t, uint32_t>> indexRanges;
			bool lodLandscape = false;
			bool nativeLandscape = false;
			bool water = false;
			bool effect = false;
			bool particles = false;
			std::vector<RemixNativeRender::ParticleVertex> particleVertices;
			RE::NiPoint3 particleCenter{};
			uint64_t particleRevision = 0;
			std::array<float, 20> effectParameters{};
			std::array<uintptr_t, 5> effectTextures{};
			bool waterObjectUV = false;
			std::array<uintptr_t, 10> waterTextures{};
			std::array<float, 4> waterTransport{};
			std::array<float, 26> waterParameters{};
			bool waterFlowUVValid = false;
			std::array<uintptr_t, 24> landscapeTextures{};
			std::array<float, 4> landscapeUv{};
			RE::NiTransform lodWorld;
			// Where this mesh was last submitted, for recycled-node detection.
			RE::NiTransform lastWorld;
			std::array<float, 4> lodRange{};
			uint32_t boneCount = 0;
			bool modelSpaceNormals = false;
			bool nativeTangentFrame = false;
			bool useFaceNormals = false;
			bool grass = false;
			bool thinFoliage = false;
			uint32_t foliageFlags = 0;
			std::array<float, 6> foliageParameters{};
			std::array<uintptr_t, 2> foliageTextures{};
			std::array<uintptr_t, 2> treeAtlas{};
			bool hairTint = false;
			bool skinDiffusion = false;
			bool skinDiffusionImported = false;
			bool lightingVertexColor = false;
			bool treeVertexColor = false;
			bool lightingVertexAlpha = false;
			float vertexAlphaScale = 1;
			std::array<uint32_t, 4> uploadedColorSamples{};
			std::array<float, 3> tint{ 1, 1, 1 };
			RE::NiPoint3 boundsMin{ FLT_MAX, FLT_MAX, FLT_MAX };
			RE::NiPoint3 boundsMax{ -FLT_MAX, -FLT_MAX, -FLT_MAX };
			std::array<float, 4> uvBounds{ FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX };
			std::vector<float> dynamicPositions;
			std::vector<uint8_t> partitionVisibility;
			std::vector<InstanceGroupCache> groups;
			RE::NiPoint3 grassScale{};
			RE::NiTransform placementWorld;
			RE::NiPoint3 placementScale{};
			std::vector<ID3D11Buffer*> placementGroups;
			// The last placement set that had anything in it, and how many frames
			// running the rebuild has produced nothing. A group whose buffer or
			// instance count is momentarily unavailable is skipped, so a rebuild
			// can come back empty for a frame while the object is still there --
			// retiring on that destroys and recreates it, which is what grass and
			// distant trees flickering in and out of view actually is.
			std::shared_ptr<const PlacementSet> lastGoodPlacements;
			uint32_t emptyPlacementFrames = 0;
			std::shared_ptr<const PlacementSet> placements;
		};
		struct Instance
		{
			remixapi_MeshHandle mesh;
			RE::NiTransform world;
			std::vector<remixapi_Transform> bones;
			std::shared_ptr<const PlacementSet> placements;
			remixapi_InstanceInfoBlendEXT blend{};
			// Handle of this instance's registration in the runtime's retained
			// table. Zero until it has been registered; preserved across
			// in-place updates so the runtime keeps the same entry.
			uint64_t retained = 0;
		};
		std::unordered_map<RE::BSGeometry*, Mesh> meshes;
		std::unordered_map<RE::BSGeometry*, Instance> visible;
		// Instances whose submitted description genuinely changed since the last
		// frame. Everything else stays registered in the runtime untouched.
		std::unordered_set<RE::BSGeometry*> changedInstances;

		// Objects reported as flickering in and out would be retired and then
		// registered again shortly after. Record which ones churn, by name, so
		// the cause can be named rather than guessed at.
		struct InstanceChurn
		{
			std::string name;
			uint64_t lastRetiredFrame = 0;
			uint32_t retires = 0;
			uint32_t quickReturns = 0;
			uint32_t teleports = 0;
			float largestTeleport = 0.0f;
			const char* lastRetireReason = "";
		};
		std::unordered_map<RE::BSGeometry*, InstanceChurn> instanceChurn;

		// Distant-tree LOD census, and the switch that acts on it.
		std::atomic<bool> respectDistantTreeCulling{ false };
		uint32_t distantTreeGroupsSeen = 0, distantTreeGroupsVisible = 0;
		uint32_t distantTreeInstancesSeen = 0, distantTreeInstancesVisible = 0;
		// Instance groups read while the game was writing into them.
		uint32_t tornInstanceGroups = 0;
		// Instance groups whose buffer was handed back for a different cell.
		uint32_t recycledInstanceGroups = 0;
		// How far the furthest distant-tree placement has ever sat outside the
		// bound of the group it was read from, in world units.
		float distantTreeWorstEscape = 0.0f;
		// Groups rejected for holding placements that belong to another cell.
		uint32_t strayInstanceGroups = 0;
		// The guard itself, and a way to manufacture the fault it guards against.
		// Misplaced placements cannot be produced on demand by playing the game --
		// they come of a bad first read that is then cached for the session -- so
		// the only way to watch the guard do its job is to inject one.
		std::atomic<bool> strayGuardEnabled{ true };
		std::atomic<bool> injectStrayTreeGroups{ false };
		// Paired samples of the rebasing origin and the world-space eye it is
		// meant to follow, so a step in one without the other is visible.
		struct OriginSample
		{
			RE::NiPoint3 origin;
			RE::NiPoint3 eye;
		};
		std::vector<OriginSample> originTrail;

		// The frame an object was first found to have nothing to draw. A system
		// that produces no geometry for one frame has not gone away, so this
		// distinguishes a gap from an ending.
		std::unordered_map<RE::BSGeometry*, uint64_t> emptySince;
		// Six frames is under 50 ms at the rates this runs at: long enough to
		// cover the fade gaps measured on the weather cloud systems, short
		// enough that an emitter that really has stopped is not left standing.
		static constexpr uint64_t kEmptyGraceFrames = 6;



		/// @brief Replaces an instance's description, keeping its retained
		///        registration so the runtime updates rather than re-adds it.
		void StoreInstance(RE::BSGeometry* geometry, Instance&& instance)
		{
			// An object that appears somewhere impossible for a single frame is
			// an instance submitted with a transform that is not its own. Name
			// the ones whose placement jumps, so the cause can be found rather
			// than inferred from a screenshot.
			if (const auto previous = visible.find(geometry); previous != visible.end()) {
				const auto& a = previous->second.world.translate;
				const auto& b = instance.world.translate;
				const float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
				const float jump = std::sqrt(dx * dx + dy * dy + dz * dz);
				if (jump > 400.0f) {
					auto& record = instanceChurn[geometry];
					if (record.name.empty())
						record.name = geometry->name.c_str();
					++record.teleports;
					record.largestTeleport = std::max(record.largestTeleport, jump);
				}
			}
			if (const auto churn = instanceChurn.find(geometry);
				churn != instanceChurn.end() && churn->second.retires != 0 &&
				frame - churn->second.lastRetiredFrame <= 30)
				++churn->second.quickReturns;
			emptySince.erase(geometry);
			const auto found = visible.find(geometry);
			if (found != visible.end())
				instance.retained = found->second.retained;
			visible.insert_or_assign(geometry, std::move(instance));
			changedInstances.insert(geometry);
		}

		/// @brief Drops an instance and its retained registration together.
		void RetireInstance(RE::BSGeometry* geometry, const char* reason = "unspecified")
		{
			emptySince.erase(geometry);
			const auto found = visible.find(geometry);
			if (found == visible.end())
				return;
			{
				auto& record = instanceChurn[geometry];
				if (record.name.empty())
					record.name = geometry->name.c_str();
				++record.retires;
				record.lastRetireReason = reason;
				record.lastRetiredFrame = frame;
			}
			if (found->second.retained && destroyRetainedInstance)
				destroyRetainedInstance(found->second.retained);
			visible.erase(found);
			changedInstances.erase(geometry);
		}

		/// @brief Retires an object that has had nothing to draw for long enough
		///        that it is not simply between frames of its own animation.
		///
		/// A particle system's quads collapse to zero area as the native fade
		/// takes each particle out, and a system where that happens to all of
		/// them at once produces no geometry for a frame or two before the next
		/// spawn. Retiring on the spot destroys the mesh, the material and the
		/// registration and rebuilds all three when it returns, which is both a
		/// visible pop and a full re-upload. The weather cloud systems over
		/// Riverwood did that six to twelve times every two seconds.
		void RetireInstanceIfStillEmpty(RE::BSGeometry* geometry, const char* reason)
		{
			const auto [entry, inserted] = emptySince.try_emplace(geometry, frame);
			if (frame - entry->second < kEmptyGraceFrames)
				return;
			RetireInstance(geometry, reason);
		}
		std::unordered_map<RE::BSGeometry*, RE::NiPointer<RE::BSGeometry>> loadedGeometry;
		struct GrassScaleHint
		{
			RE::NiPointer<RE::BSGeometry> owner;
			RE::NiPoint3 scale;
			RE::NiPoint3 wind{};
			float timer = 0, previousTimer = 0;
			bool hasWind = false;
		};
		std::unordered_map<RE::BSGeometry*, GrassScaleHint> grassScales;
		// Skyrim's netimmerse_cast walks the NiRTTI chain on every call. The
		// object's own runtime type never changes, and the shader-property
		// derived kinds only change when the property itself is swapped, so
		// both are resolved once and revalidated by pointer identity.
		struct Classification
		{
			const RE::NiObject* property = nullptr;
			RE::BSSubIndexTriShape* subIndex = nullptr;
			RE::BSMultiStreamInstanceTriShape* multiStream = nullptr;
			RE::NiParticleSystem* particles = nullptr;
			bool resolvedShape = false;
			bool distantTree = false;
			bool grass = false;
			bool water = false;
			bool effect = false;
			// Visible LOD segment coverage, rebuilt only when the native segment
			// table genuinely changes rather than re-sorted and re-merged every
			// frame for every segmented world mesh.
			uint64_t segmentSignature = 0;
			bool segmentsValid = false;
			bool segmentsRejected = false;
			uint32_t segmentIndexCount = 0;
			std::vector<std::pair<uint32_t, uint32_t>> segmentRanges;
			// Identity probe for plain opaque static shapes. Re-deriving every
			// mesh/material input just to discover nothing changed dominated the
			// host frame cost, so eligible geometry compares these inputs first.
			struct StaticProbe
			{
				RE::NiTransform world;
				const void* rendererData = nullptr;
				const void* shaderProperty = nullptr;
				const void* materialPointer = nullptr;
				const void* alphaProperty = nullptr;
				uint64_t segmentSignature = 0;
				uint64_t propertyFlags = 0;
				uint64_t descriptor = 0;
				uint32_t alphaState = 0;
				uint32_t counts = 0;
				float materialAlpha = 0;
				bool operator==(const StaticProbe& other) const
				{
					return world == other.world && rendererData == other.rendererData &&
					       shaderProperty == other.shaderProperty && materialPointer == other.materialPointer &&
					       alphaProperty == other.alphaProperty && segmentSignature == other.segmentSignature &&
					       propertyFlags == other.propertyFlags && descriptor == other.descriptor &&
					       alphaState == other.alphaState && counts == other.counts && materialAlpha == other.materialAlpha;
				}
			};
			bool probeEligible = false;
			StaticProbe probe;
			// Consecutive frames this object's probe has come back unchanged.
			// Once an object has been still for a while its probe is only read
			// on a stagger, because reading it is itself the cost: the inputs
			// live in scattered game objects and every one is a cache miss.
			uint32_t stableFrames = 0;
		};
		// Frames an object must be unchanged before its probe is staggered, and
		// how far apart the staggered reads are. An object that starts moving is
		// noticed within the interval, which is well inside a frame of latency
		// at any rate this scene runs at.
		constexpr uint32_t kProbeStaggerThreshold = 30;
		constexpr uint64_t kProbeStaggerInterval = 4;

		/// @brief Hashes a segmented shape's native LOD coverage table.
		uint64_t SegmentSignature(const RE::BSSubIndexTriShape::SUB_INDEX_TRISHAPE_RUNTIME_DATA& segments, uint32_t sourceIndexCount)
		{
			uint64_t signature = 0xcbf29ce484222325ull ^ (uint64_t(segments.numSegments) << 32) ^ sourceIndexCount;
			if (!segments.segmentData)
				return signature;
			for (uint32_t i = 0; i < segments.numSegments; ++i) {
				const auto& segment = segments.segmentData[i];
				const uint64_t fields[]{ segment.index, i == 0 ? segment.numTris : segment.unkTriCount, i == 0 ? segment.flags : segment.unkFlags };
				for (const auto field : fields)
					signature = (signature ^ field) * 0x100000001b3ull;
			}
			return signature;
		}

		/// @brief Reads every per-frame input a plain static shape's capture uses.
		Classification::StaticProbe BuildStaticProbe(RE::BSGeometry* geometry, const RE::BSGeometry::GEOMETRY_RUNTIME_DATA& data,
			RE::BSTriShape* shape, const Classification& kinds)
		{
			Classification::StaticProbe probe;
			probe.world = geometry->world;
			probe.rendererData = data.rendererData;
			probe.shaderProperty = data.shaderProperty.get();
			if (data.rendererData)
				std::memcpy(&probe.descriptor, &data.rendererData->vertexDesc, sizeof(probe.descriptor));
			if (data.shaderProperty) {
				probe.materialPointer = data.shaderProperty->material;
				probe.propertyFlags = data.shaderProperty->flags.underlying();
				if (const auto* lighting = static_cast<const RE::BSLightingShaderMaterialBase*>(data.shaderProperty->material))
					probe.materialAlpha = lighting->materialAlpha;
			}
			if (const auto* alpha = data.alphaProperty.get()) {
				probe.alphaProperty = alpha;
				probe.alphaState = (uint32_t(alpha->alphaFlags) << 8) | alpha->alphaThreshold;
			}
			if (shape) {
				const auto& trishape = shape->GetTrishapeRuntimeData();
				probe.counts = (uint32_t(trishape.vertexCount) << 16) | trishape.triangleCount;
				if (kinds.subIndex)
					probe.segmentSignature = SegmentSignature(kinds.subIndex->GetSubIndexedTrishapeRuntimeData(), uint32_t(trishape.triangleCount) * 3);
			}
			return probe;
		}
		std::unordered_map<RE::BSGeometry*, Classification> classifications;
		const std::vector<std::pair<uint32_t, uint32_t>> noIndexRanges;

		/// @brief Resolves (and caches) the geometry and shader-property kinds.
		Classification& Classify(RE::BSGeometry* geometry, const RE::BSGeometry::GEOMETRY_RUNTIME_DATA& data, bool isTriShape)
		{
			auto& entry = classifications[geometry];
			if (!entry.resolvedShape) {
				entry.resolvedShape = true;
				entry.subIndex = isTriShape ? netimmerse_cast<RE::BSSubIndexTriShape*>(geometry) : nullptr;
				entry.multiStream = isTriShape ? netimmerse_cast<RE::BSMultiStreamInstanceTriShape*>(geometry) : nullptr;
				entry.particles = isTriShape ? nullptr : netimmerse_cast<RE::NiParticleSystem*>(geometry);
			}
			auto* property = data.shaderProperty.get();
			if (entry.property != property) {
				entry.property = property;
				entry.distantTree = property && netimmerse_cast<RE::BSDistantTreeShaderProperty*>(property) != nullptr;
				entry.grass = property && netimmerse_cast<RE::BSGrassShaderProperty*>(property) != nullptr;
				entry.water = property && netimmerse_cast<RE::BSWaterShaderProperty*>(property) != nullptr;
				entry.effect = property && netimmerse_cast<RE::BSEffectShaderProperty*>(property) != nullptr;
			}
			return entry;
		}
		uint64_t grassWindRevision = 0;
		uint64_t grassWindAuditSamples = 0;
		float grassWindAuditMaxError = 0;

		bool ReadBuffer(ID3D11Buffer* source, size_t bytes, std::vector<uint8_t>& result, const void* cpuData = nullptr)
		{
			D3D11_BUFFER_DESC desc{};
			source->GetDesc(&desc);
			if (bytes > desc.ByteWidth || bytes == 0)
				return false;
			if (cpuData) {
				result.resize(bytes);
				std::memcpy(result.data(), cpuData, bytes);
				return true;
			}
			desc.ByteWidth = static_cast<UINT>(bytes);
			desc.Usage = D3D11_USAGE_STAGING;
			desc.BindFlags = desc.MiscFlags = desc.StructureByteStride = 0;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			ComPtr<ID3D11Buffer> staging;
			if (FAILED(globals::d3d::device->CreateBuffer(&desc, nullptr, staging.GetAddressOf())))
				return false;
			Util::SetResourceName(staging.Get(), "Remix Mesh Upload Readback");
			D3D11_BOX box{ 0, 0, 0, static_cast<UINT>(bytes), 1, 1 };
			globals::d3d::context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, source, 0, &box);
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (FAILED(globals::d3d::context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
				return false;
			result.resize(bytes);
			std::memcpy(result.data(), mapped.pData, bytes);
			globals::d3d::context->Unmap(staging.Get(), 0);
			return true;
		}

		bool CaptureInstances(RE::BSMultiStreamInstanceTriShape* shape, Mesh& mesh, Instance& instance, bool grass, bool distantTree)
		{
			const auto& runtime = shape->GetMultiStreamTrishapeRuntimeData();
			// BGSDistantTreeBlock's native upload (1.7.99 RVA 0x50c6d0)
			// calls AddGroup with 16 half words per instance, but never sets
			// the shape's BeginAddingInstances scratch field instanceSize.
			// AddGroup (0xfe2a20) allocates count * words * 2 bytes directly.
			const uint32_t stride = distantTree ? 32u : runtime.instanceSize * 2;
			if (stride < (grass ? 32u : 16u) || stride > 128)
				return false;
			std::vector<size_t> activeGroups;
			std::vector<ID3D11Buffer*> groupOrder;
			for (const auto* group : runtime.instanceGroups) {
				// isVisible belongs to the engine's most recent raster culling pass
				// (which may be a shadow view). Keep loaded groups for ray tracing;
				// off-screen grass can still cast shadows and appear in reflections.
				//
				// Distant trees carry a second meaning in the same flag: a group
				// of billboards is turned off when the full trees for that cell
				// are loaded. Submitting those anyway puts a billboard copy of
				// every near tree into the scene at whatever yaw it was last
				// baked with, which is what reads as sprites scattered over the
				// cliffs. See SetDistantTreeCullingRespected.
				if (!group || !group->vertexBuffer || !group->vertexBuffer->buffer || !group->instanceCount || group->instanceCount > 100000)
					continue;
				if (distantTree) {
					++distantTreeGroupsSeen;
					distantTreeGroupsVisible += group->isVisible ? 1u : 0u;
					distantTreeInstancesSeen += group->instanceCount;
					distantTreeInstancesVisible += group->isVisible ? group->instanceCount : 0u;
					if (respectDistantTreeCulling.load(std::memory_order_relaxed) && !group->isVisible)
						continue;
				}
				auto* buffer = reinterpret_cast<ID3D11Buffer*>(group->vertexBuffer->buffer);
				auto cached = std::ranges::find_if(mesh.groups, [&](const auto& value) {
					return value.buffer.Get() == buffer && value.count == group->instanceCount && value.stride == stride &&
						value.center == group->center && value.size == group->size;
				});
				// Count the ones the old identity would have accepted: a matching
				// buffer serving a different cell. Those are placements drawn where
				// they do not belong until the group next changes.
				if (cached == mesh.groups.end() &&
					std::ranges::any_of(mesh.groups, [&](const auto& value) {
						return value.buffer.Get() == buffer && value.count == group->instanceCount && value.stride == stride;
					}))
					++recycledInstanceGroups;
				const size_t byteCount = size_t(stride) * group->instanceCount;
				D3D11_BUFFER_DESC bufferDesc{};
				buffer->GetDesc(&bufferDesc);
				if (byteCount > bufferDesc.ByteWidth)
					return false;
				std::vector<uint8_t> readback;
				const auto* source = static_cast<const uint8_t*>(group->vertexBuffer->m_data);
				if (!source && (cached == mesh.groups.end() || bufferDesc.Usage != D3D11_USAGE_IMMUTABLE)) {
					// Mutable GPU-only buffers need content verification too. A stable
					// COM pointer is not a revision number. CPU-backed groups avoid
					// GPU synchronization and compare directly against our snapshot.
					if (!ReadBuffer(buffer, byteCount, readback))
						return false;
					++instanceReadbacksThisFrame;
					source = readback.data();
				}
				if (cached == mesh.groups.end() || (source && (cached->sourceBytes.size() != byteCount ||
					std::memcmp(cached->sourceBytes.data(), source, byteCount) != 0))) {
					if (!source)
						return false;
					InstanceGroupCache entry;
					entry.buffer = buffer;
					entry.count = group->instanceCount;
					entry.stride = stride;
					entry.center = group->center;
					entry.size = group->size;
					entry.sourceBytes.assign(source, source + byteCount);
					// Per-row validation cannot see a tear that lands between rows.
					// Both halves are internally consistent -- each satisfies the
					// unit-rotation test below -- they just belong to different
					// uploads, so a tree ends up at the yaw its neighbour was baked
					// with and the cliff fills with branches at angles no billboard
					// takes. Only requiring two reads to agree separates "these
					// placements changed" from "these placements are being written".
					// The second read only happens on a frame the bytes changed,
					// which is the uncommon one.
					// Per-row validation cannot see a tear that lands between rows:
					// both halves satisfy the unit-rotation test below, they just
					// belong to different uploads, so an object ends up wearing a
					// neighbour's placement -- log ends hanging in mid-air, tree
					// billboards across a cliff face. Only requiring two reads to
					// agree separates "these placements changed" from "these
					// placements are being written".
					//
					// An upload is in flight for a frame or two, not indefinitely,
					// so a disagreement is retried within the frame before giving
					// up. That matters most for a group with no cached snapshot:
					// latching a torn first read leaves the object wrong until its
					// bytes change again, which may be never.
					bool stable = false;
					for (uint32_t attempt = 0; attempt < 3 && !stable; ++attempt) {
						if (const auto* live = static_cast<const uint8_t*>(group->vertexBuffer->m_data)) {
							stable = std::memcmp(entry.sourceBytes.data(), live, byteCount) == 0;
							if (!stable)
								entry.sourceBytes.assign(live, live + byteCount);
						} else {
							std::vector<uint8_t> verify;
							if (!ReadBuffer(buffer, byteCount, verify) || verify.size() != byteCount)
								break;
							++instanceReadbacksThisFrame;
							stable = std::memcmp(entry.sourceBytes.data(), verify.data(), byteCount) == 0;
							if (!stable)
								entry.sourceBytes = std::move(verify);
						}
					}
					if (!stable) {
						++tornInstanceGroups;
						// A group with a good snapshot keeps it and is read again
						// next frame. One without is left out of this frame
						// entirely rather than shown wrong: it appears as soon as
						// its upload settles, which is a frame or two.
						if (cached == mesh.groups.end())
							continue;
						activeGroups.push_back(static_cast<size_t>(cached - mesh.groups.begin()));
						groupOrder.push_back(buffer);
						continue;
					}
					float brightnessMin = FLT_MAX, brightnessMax = -FLT_MAX;
					double brightnessSum = 0;
					bool torn = false;
					for (uint32_t i = 0; i < entry.count; ++i) {
						float values[16]{};
						bool valid = true;
						for (uint32_t j = 0; j < (grass ? 16u : 8u); ++j) {
							uint16_t half;
							std::memcpy(&half, entry.sourceBytes.data() + size_t(i) * stride + j * 2, sizeof(half));
							values[j] = DirectX::PackedVector::XMConvertHalfToFloat(half);
							valid = valid && std::isfinite(values[j]);
						}
						if (!valid)
							continue;
						RE::NiTransform transform;
						transform.translate = { values[0], values[1], values[2] };
						if (grass) {
							for (uint32_t axis = 0; axis < 3; ++axis) {
								transform.rotate.entry[0][axis] = values[4 + axis];
								transform.rotate.entry[1][axis] = values[8 + axis];
							}
							transform.rotate.entry[2][0] = values[12];
							transform.rotate.entry[2][1] = values[7];
							transform.rotate.entry[2][2] = values[11];
						} else {
							if (values[3] == 0 || values[6] <= 0)
								continue;
							// The game writes a unit 2D rotation here, so cos^2 + sin^2
							// is 1 for every instance it ever produced. A read that
							// caught the buffer mid-upload splices two instances
							// together and almost never satisfies that, which is what
							// puts tree billboards across a cliff at angles no
							// billboard takes. Half floats carry about three decimal
							// digits, hence the tolerance.
							const float unitLength = values[4] * values[4] + values[5] * values[5];
							if (!std::isfinite(unitLength) || std::abs(unitLength - 1.0f) > 0.01f ||
								!(values[3] > 0.0f) || values[3] > 100.0f) {
								torn = true;
								break;
							}
							transform.scale = values[3];
							transform.rotate.entry[0][0] = values[4];
							transform.rotate.entry[0][1] = -values[5];
							transform.rotate.entry[1][0] = values[5];
							transform.rotate.entry[1][1] = values[4];
						}
						entry.transforms.push_back(transform);
						entry.scaleVariations.push_back(grass ? values[13] : 0);
						entry.brightness.push_back(grass ? values[3] : 1);
						brightnessMin = std::min(brightnessMin, values[3]);
						brightnessMax = std::max(brightnessMax, values[3]);
						brightnessSum += values[3];
					}
					// A group whose placements cannot be trusted keeps the last
					// snapshot that could be, and is read again next frame. Dropping
					// it instead would take every tree in that cell out of the scene
					// for as long as the upload lasts, which is the flicker this is
					// here to avoid.
					if (torn) {
						++tornInstanceGroups;
						if (cached == mesh.groups.end())
							continue;
						activeGroups.push_back(static_cast<size_t>(cached - mesh.groups.begin()));
						groupOrder.push_back(buffer);
						continue;
					}
					logger::info("[RemixScene.instances] '{}' grass={} count={} stride={} world=({}, {}, {}) local=({}, {}, {})", shape->name.c_str(), grass, entry.transforms.size(), stride,
						shape->world.translate.x, shape->world.translate.y, shape->world.translate.z, shape->local.translate.x, shape->local.translate.y, shape->local.translate.z);
					if (!entry.transforms.empty())
						logger::info("[RemixScene.instances] first=({}, {}, {}) groupCenter=({}, {}, {}) groupSize=({}, {}, {})",
							entry.transforms[0].translate.x, entry.transforms[0].translate.y, entry.transforms[0].translate.z,
							group->center.x, group->center.y, group->center.z, group->size.x, group->size.y, group->size.z);
					// A distant tree's placements are in the same frame as the
					// group's own bound (verified against the logged pairs), so how
					// far the furthest one sits outside that bound says whether
					// these placements belong to this cell at all. Reported before
					// any threshold is applied, so the threshold can be set from
					// what healthy data actually does.
					// Every distant-tree placement the game has ever produced sits
					// inside the bound of the group it came from: measured over the
					// whole loaded scene, the furthest any of them escaped that
					// bound was zero units. Placements are relative to the shape,
					// which is what the submission composes them with, while the
					// bound is in world space, so the two are compared in world
					// space. A group whose placements are somewhere else is holding
					// another cell's trees, which is what puts tree sprites across a
					// cliff face. The tolerance is far above the ~32 unit step of a
					// half float at these magnitudes and far below a cell.
					// Test injection: displace this group's trees into the next
					// cell over, which is what a group holding another cell's
					// placements looks like.
					if (distantTree && injectStrayTreeGroups.load(std::memory_order_relaxed))
						for (auto& transform : entry.transforms) {
							transform.translate.x += 4096.0f;
							transform.translate.y += 4096.0f;
						}
					if (distantTree && !entry.transforms.empty()) {
						constexpr float kBoundTolerance = 512.0f;
						float worst = 0.0f;
						for (const auto& transform : entry.transforms) {
							const auto t = shape->local * transform.translate;
							worst = std::max({ worst,
								std::abs(t.x - group->center.x) - std::abs(group->size.x),
								std::abs(t.y - group->center.y) - std::abs(group->size.y),
								std::abs(t.z - group->center.z) - std::abs(group->size.z) });
						}
						distantTreeWorstEscape = std::max(distantTreeWorstEscape, worst);
						if (worst > kBoundTolerance && strayGuardEnabled.load(std::memory_order_relaxed)) {
							torn = true;
							++strayInstanceGroups;
							if (strayInstanceGroups <= 8)
								logger::warn("[RemixScene.instances] a distant-tree group at ({}, {}, {}) held placements {} units outside its own bound and was rejected",
									group->center.x, group->center.y, group->center.z, worst);
						}
					}
					if (grass && !entry.transforms.empty())
						logger::info("[RemixScene.grassColor] '{}' instance brightness min={} max={} mean={} count={}", shape->name.c_str(), brightnessMin, brightnessMax, brightnessSum / entry.transforms.size(), entry.transforms.size());
					if (cached == mesh.groups.end()) {
						// A buffer serves one group at a time, so an entry still
						// holding this pointer describes a cell that has streamed
						// out. Overwrite it rather than erasing: activeGroups holds
						// indices into this vector for groups already handled.
						auto stale = std::ranges::find_if(mesh.groups, [&](const auto& value) { return value.buffer.Get() == buffer; });
						if (stale != mesh.groups.end()) {
							*stale = std::move(entry);
							cached = stale;
						} else {
							mesh.groups.push_back(std::move(entry));
							cached = std::prev(mesh.groups.end());
						}
					} else {
						*cached = std::move(entry);
					}
					// Instances keep shared ownership of their last valid snapshot
					// until a complete replacement has been built below.
					mesh.placements.reset();
				}
				activeGroups.push_back(static_cast<size_t>(cached - mesh.groups.begin()));
				groupOrder.push_back(buffer);
			}
			const auto& world = grass ? shape->world : shape->local;
			if (mesh.placements && mesh.placementWorld == world && mesh.placementScale == mesh.grassScale && mesh.placementGroups == groupOrder) {
				instance.placements = mesh.placements;
				return true;
			}
			auto placements = std::make_shared<PlacementSet>();
			size_t placementCount = 0;
			for (const auto index : activeGroups)
				placementCount += mesh.groups[index].transforms.size();
			placements->transforms.reserve(placementCount);
			if (grass) {
				placements->brightness.reserve(placementCount);
				placements->grassPhases.reserve(placementCount);
				placements->grassNormalRows.reserve(placementCount * 9);
			}
			for (const auto index : activeGroups) {
				const auto* cached = &mesh.groups[index];
				for (size_t i = 0; i < cached->transforms.size(); ++i) {
					auto transform = cached->transforms[i];
					if (grass) {
						// RunGrass transforms normals with the original instance rows,
						// without position variation, World or per-vertex normalization.
						for (uint32_t row = 0; row < 3; ++row)
							for (uint32_t col = 0; col < 3; ++col)
								placements->grassNormalRows.push_back(transform.rotate.entry[row][col]);
						// RunGrass scales the input vertex before instance rotation.
						const float variation = cached->scaleVariations[i];
						for (uint32_t row = 0; row < 3; ++row) {
							transform.rotate.entry[row][0] *= 1 + variation * mesh.grassScale.x;
							transform.rotate.entry[row][1] *= 1 + variation * mesh.grassScale.y;
							transform.rotate.entry[row][2] *= 1 + variation * mesh.grassScale.z;
						}
					}
					const auto placement = world * transform;
					remixapi_Transform output{};
					for (uint32_t row = 0; row < 3; ++row)
						for (uint32_t col = 0; col < 3; ++col)
							output.matrix[row][col] = placement.rotate.entry[row][col] * placement.scale;
					output.matrix[0][3] = placement.translate.x;
					output.matrix[1][3] = placement.translate.y;
					output.matrix[2][3] = placement.translate.z;
					placements->transforms.push_back(output);
					if (grass) {
						placements->brightness.push_back(cached->brightness[i]);
						// Native shader phase is instance-local, before World and rebasing.
						const auto& local = cached->transforms[i].translate;
						placements->grassPhases.push_back((local.x + local.y) * -0.0078125f);
					}
				}
			}
			if (!placements->transforms.empty()) {
				const auto result = grass ? createGrassInstanceSet(placements->transforms.data(), placements->brightness.data(), placements->grassPhases.data(), placements->grassNormalRows.data(),
					static_cast<uint32_t>(placements->transforms.size()), &placements->handle) :
					createInstanceSet(placements->transforms.data(), nullptr, static_cast<uint32_t>(placements->transforms.size()), &placements->handle);
				if (result != REMIXAPI_ERROR_CODE_SUCCESS) {
					logger::error("[RemixScene] native instance-set creation failed for '{}'", shape->name.c_str());
					return false;
				}
				instanceSetHandles.insert(placements->handle);
			}
			// Hold the previous set over a rebuild that produced nothing, rather
			// than reporting the object as gone. Only a run of empty rebuilds is
			// treated as the object genuinely having been removed.
			constexpr uint32_t kEmptyPlacementFramesBeforeRetiring = 4;
			if (placements->transforms.empty() && mesh.lastGoodPlacements &&
				++mesh.emptyPlacementFrames <= kEmptyPlacementFramesBeforeRetiring) {
				instance.placements = mesh.lastGoodPlacements;
				return true;
			}
			if (!placements->transforms.empty()) {
				mesh.emptyPlacementFrames = 0;
			}

			mesh.placementWorld = world;
			mesh.placementScale = mesh.grassScale;
			mesh.placementGroups = std::move(groupOrder);
			mesh.placements = std::move(placements);
			if (mesh.placements && !mesh.placements->transforms.empty()) {
				mesh.lastGoodPlacements = mesh.placements;
			}
			++rebuiltPlacementsThisFrame;
			instance.placements = mesh.placements;
			// Release removed groups after all indices into the cache are consumed.
			std::erase_if(mesh.groups, [&](const auto& entry) {
				return std::ranges::find(mesh.placementGroups, entry.buffer.Get()) == mesh.placementGroups.end() || entry.stride != stride ||
					std::ranges::none_of(runtime.instanceGroups, [&](const auto* group) {
						return group && group->vertexBuffer && reinterpret_cast<ID3D11Buffer*>(group->vertexBuffer->buffer) == entry.buffer.Get() && group->instanceCount == entry.count;
					});
			});
			return true;
		}

		bool Upload(Mesh& mesh, RE::BSGraphics::VertexDesc descriptor, const RE::BSLightingShaderMaterialBase* sourceMaterial, const RE::NiSkinPartition* partition, const RE::BSGraphics::TriShape* rendererData, bool updateExisting = false)
		{
			const uint32_t stride = static_cast<uint32_t>((mesh.descriptor & 15) * 4);
			if (stride < 12 || stride > 256)
				return false;
			std::vector<uint8_t> vertexBytes, indexBytes;
			if (!ReadBuffer(mesh.vertices.Get(), size_t(stride) * mesh.vertexCount, vertexBytes, rendererData->rawVertexData))
				return false;
			if (descriptor.HasFlag(RE::BSGraphics::Vertex::VF_LANDDATA)) {
				static uint32_t landAudits = 0;
				const auto offset = descriptor.GetAttributeOffset(RE::BSGraphics::Vertex::VA_LANDDATA);
				if (landAudits < 12 && offset + 8 <= stride && sourceMaterial) {
					++landAudits;
					std::array<uint32_t, 6> nonzero{};
					uint32_t minSum = UINT32_MAX, maxSum = 0;
					for (uint32_t i = 0; i < mesh.vertexCount; ++i) {
						const auto* packed = vertexBytes.data() + size_t(i) * stride + offset;
						uint32_t sum = 0;
						for (uint32_t layer = 0; layer < 6; ++layer) {
							sum += packed[layer];
							nonzero[layer] += packed[layer] != 0;
						}
						minSum = std::min(minSum, sum);
						maxSum = std::max(maxSum, sum);
					}
					logger::info("[RemixScene.landAudit] feature={} vertices={} stride={} landOffset={} MSN={} weightSum={}..{} nonzero=({},{},{},{},{},{})",
						static_cast<uint32_t>(sourceMaterial->GetFeature()), mesh.vertexCount, stride, offset, mesh.modelSpaceNormals,
						minSum, maxSum, nonzero[0], nonzero[1], nonzero[2], nonzero[3], nonzero[4], nonzero[5]);
					if (sourceMaterial->GetFeature() == RE::BSShaderMaterial::Feature::kMultiTexLandLODBlend) {
						const auto* land = static_cast<const RE::BSLightingShaderMaterialLandscape*>(sourceMaterial);
						logger::info("[RemixScene.landAudit] layers={} base='{}' extra=('{}','{}','{}','{}','{}') uvScale=({},{}) uvOffset=({},{})",
							land->numLandscapeTextures, land->diffuseTexture ? land->diffuseTexture->name.c_str() : "",
							land->landscapeDiffuseTexture[0] ? land->landscapeDiffuseTexture[0]->name.c_str() : "",
							land->landscapeDiffuseTexture[1] ? land->landscapeDiffuseTexture[1]->name.c_str() : "",
							land->landscapeDiffuseTexture[2] ? land->landscapeDiffuseTexture[2]->name.c_str() : "",
							land->landscapeDiffuseTexture[3] ? land->landscapeDiffuseTexture[3]->name.c_str() : "",
							land->landscapeDiffuseTexture[4] ? land->landscapeDiffuseTexture[4]->name.c_str() : "",
							land->texCoordScale[0].x, land->texCoordScale[0].y, land->texCoordOffset[0].x, land->texCoordOffset[0].y);
					}
				}
			}
			if (partition) {
				for (uint32_t i = 0; i < partition->numPartitions; ++i) {
					const auto& part = partition->partitions[i];
					if (!mesh.partitionVisibility[i] || !part.triangles)
						continue;
					std::vector<uint8_t> bytes;
					if (!part.buffData || !part.buffData->indexBuffer ||
						!ReadBuffer(reinterpret_cast<ID3D11Buffer*>(part.buffData->indexBuffer), size_t(part.triangles) * 6, bytes, part.buffData->rawIndexData))
						return false;
					indexBytes.insert(indexBytes.end(), bytes.begin(), bytes.end());
				}
			} else if (!ReadBuffer(mesh.indices.Get(), size_t(mesh.sourceIndexCount) * sizeof(uint16_t), indexBytes, rendererData->rawIndexData)) {
				return false;
			}
			if (!mesh.indexRanges.empty()) {
				std::vector<uint8_t> selected;
				selected.reserve(size_t(mesh.indexCount) * sizeof(uint16_t));
				for (const auto& [start, count] : mesh.indexRanges) {
					const auto begin = indexBytes.begin() + size_t(start) * sizeof(uint16_t);
					selected.insert(selected.end(), begin, begin + size_t(count) * sizeof(uint16_t));
				}
				indexBytes = std::move(selected);
			}
			uint32_t loweredVertices = 0;
			uint32_t grassColorMin = 255, grassColorMax = 0;
			std::vector<remixapi_HardcodedVertex> vertices(mesh.vertexCount);
			const auto normalAttribute = descriptor.GetAttributeOffset(RE::BSGraphics::Vertex::VA_NORMAL);
			const auto binormalAttribute = descriptor.GetAttributeOffset(RE::BSGraphics::Vertex::VA_BINORMAL);
			const bool authoredFrame = !mesh.modelSpaceNormals && !mesh.grass && !mesh.water && !mesh.effect &&
				descriptor.HasFlag(RE::BSGraphics::Vertex::VF_NORMAL) && descriptor.HasFlag(RE::BSGraphics::Vertex::VF_TANGENT) &&
				normalAttribute + 4 <= stride && binormalAttribute + 4 <= stride &&
				(!mesh.dynamicPositions.empty() || (descriptor.HasFlag(RE::BSGraphics::Vertex::VF_VERTEX) && stride >= 16));
			std::vector<float> tangentFrame(authoredFrame ? size_t(mesh.vertexCount) * 6 : 0);
			std::vector<std::array<float, 2>> waterSourceUV(mesh.water ? mesh.vertexCount : 0);
			std::vector<float> grassWindWeights(mesh.grass ? mesh.vertexCount : 0);
			std::vector<float> weights(mesh.boneCount ? mesh.vertexCount * 4 : 0);
			std::vector<uint32_t> boneIndices(weights.size());
			for (size_t i = 0; i < vertices.size(); ++i) {
				auto& vertex = vertices[i];
				const auto* source = vertexBytes.data() + stride * i;
				if (authoredFrame) {
					float positionW;
					if (!mesh.dynamicPositions.empty()) positionW = mesh.dynamicPositions[i * 4 + 3];
					else std::memcpy(&positionW, source + 12, sizeof(positionW));
					const auto decodedBasis = RemixTangentFrame::Decode(positionW, source + normalAttribute, source + binormalAttribute);
					std::copy(decodedBasis.begin(), decodedBasis.end(), tangentFrame.begin() + i * 6);
				}
				if (!mesh.dynamicPositions.empty())
					std::memcpy(vertex.position, mesh.dynamicPositions.data() + i * 4, sizeof(vertex.position));
				else if (descriptor.HasFlag(RE::BSGraphics::Vertex::VF_VERTEX))
					std::memcpy(vertex.position, source, sizeof(vertex.position));
				else
					return false;
				for (float coordinate : vertex.position)
					if (!std::isfinite(coordinate))
						return false;
				if (mesh.lodLandscape) {
					const auto world = mesh.lodWorld * RE::NiPoint3{ vertex.position[0], vertex.position[1], vertex.position[2] };
					if (std::abs(world.x - mesh.lodRange[0]) < mesh.lodRange[2] &&
						std::abs(world.y - mesh.lodRange[1]) < mesh.lodRange[3]) {
						// Match Common/LodLandscape.hlsli, before object-to-world.
						vertex.position[2] -= 230.0f + world.z / 1e9f;
						++loweredVertices;
					}
				}
				mesh.boundsMin.x = std::min(mesh.boundsMin.x, vertex.position[0]);
				mesh.boundsMin.y = std::min(mesh.boundsMin.y, vertex.position[1]);
				mesh.boundsMin.z = std::min(mesh.boundsMin.z, vertex.position[2]);
				mesh.boundsMax.x = std::max(mesh.boundsMax.x, vertex.position[0]);
				mesh.boundsMax.y = std::max(mesh.boundsMax.y, vertex.position[1]);
				mesh.boundsMax.z = std::max(mesh.boundsMax.z, vertex.position[2]);
				vertex.normal[2] = 1;
				const auto normalOffset = descriptor.GetAttributeOffset(RE::BSGraphics::Vertex::VA_NORMAL);
				// CS Grass Lighting consumes the authored normal, unlike vanilla's
				// instance-up light angle. The GPU grass path rotates this attribute.
				if (descriptor.HasFlag(RE::BSGraphics::Vertex::VF_NORMAL) && normalOffset + 3 <= stride)
					for (uint32_t axis = 0; axis < 3; ++axis)
						vertex.normal[axis] = source[normalOffset + axis] / 127.5f - 1.0f;
				const auto uvOffset = descriptor.GetAttributeOffset(RE::BSGraphics::Vertex::VA_TEXCOORD0);
				if (descriptor.HasFlag(RE::BSGraphics::Vertex::VF_UV) && uvOffset + 4 <= stride) {
					uint16_t uv[2];
					std::memcpy(uv, source + uvOffset, sizeof(uv));
					vertex.texcoord[0] = DirectX::PackedVector::XMConvertHalfToFloat(uv[0]);
					vertex.texcoord[1] = DirectX::PackedVector::XMConvertHalfToFloat(uv[1]);
				}
				vertex.color = mesh.grass ? 0x00ffffff : 0xffffffff;
				if (mesh.lightingVertexColor) {
					const auto offset = descriptor.GetAttributeOffset(RE::BSGraphics::Vertex::VA_COLOR);
					if (descriptor.HasFlag(RE::BSGraphics::Vertex::VF_COLORS) && offset + 4 <= stride) {
						// Lighting's VC permutation modulates RGB; tree/LOD alpha is
						// animation data. Compose material alpha only for coverage.
						const float alpha = (mesh.lightingVertexAlpha ? source[offset + 3] / 255.0f : 1.0f) * mesh.vertexAlphaScale;
						vertex.color = (uint32_t(std::lround(std::clamp(alpha, 0.0f, 1.0f) * 255)) << 24) |
							(uint32_t(source[offset]) << 16) | (uint32_t(source[offset + 1]) << 8) | uint32_t(source[offset + 2]);
					}
				}
				if (mesh.effect) {
					const auto offset = descriptor.GetAttributeOffset(RE::BSGraphics::Vertex::VA_COLOR);
					if (descriptor.HasFlag(RE::BSGraphics::Vertex::VF_COLORS) && offset + 4 <= stride) {
						// Effect vertex RGBA participates in palette coordinates and
						// coverage. White defaults discard the authored edge fades.
						vertex.color = (uint32_t(source[offset + 3]) << 24) | (uint32_t(source[offset]) << 16) |
							(uint32_t(source[offset + 1]) << 8) | uint32_t(source[offset + 2]);
					}
				}
				if (mesh.nativeLandscape) {
					const auto landOffset = descriptor.GetAttributeOffset(RE::BSGraphics::Vertex::VA_LANDDATA);
					if (!descriptor.HasFlag(RE::BSGraphics::Vertex::VF_LANDDATA) || landOffset + 8 > stride) return false;
					// Preserve all packed bytes in the public vertex's reserved words.
					// Native weights do not sum to 255; normalize after interpolation.
					std::memcpy(&vertex._pad0, source + landOffset, 8);
					const auto colorOffset = descriptor.GetAttributeOffset(RE::BSGraphics::Vertex::VA_COLOR);
					if (descriptor.HasFlag(RE::BSGraphics::Vertex::VF_COLORS) && colorOffset + 4 <= stride)
						vertex.color = 0xff000000u | (uint32_t(source[colorOffset]) << 16) |
							(uint32_t(source[colorOffset + 1]) << 8) | uint32_t(source[colorOffset + 2]);
				}
				if (mesh.grass) {
					const auto offset = descriptor.GetAttributeOffset(RE::BSGraphics::Vertex::VA_COLOR);
					if (descriptor.HasFlag(RE::BSGraphics::Vertex::VF_COLORS) && offset + 4 <= stride) {
						// RunGrass multiplies diffuse by vertex RGB. Its vertex alpha
						// controls wind displacement, NOT opacity. The native grass
						// GPU expansion consumes it then writes opaque output alpha.
						vertex.color = (uint32_t(source[offset + 3]) << 24) | (uint32_t(source[offset]) << 16) |
							(uint32_t(source[offset + 1]) << 8) | uint32_t(source[offset + 2]);
						grassWindWeights[i] = source[offset + 3] / 255.0f;
						for (uint32_t axis = 0; axis < 3; ++axis) {
							grassColorMin = std::min(grassColorMin, uint32_t(source[offset + axis]));
							grassColorMax = std::max(grassColorMax, uint32_t(source[offset + axis]));
						}
					}
				}
				if (mesh.hairTint) {
					const auto colorOffset = descriptor.GetAttributeOffset(RE::BSGraphics::Vertex::VA_COLOR);
					float mask = 1;
					float alpha = sourceMaterial ? sourceMaterial->materialAlpha : 1;
					if (descriptor.HasFlag(RE::BSGraphics::Vertex::VF_COLORS) && colorOffset + 4 <= stride) {
						mask = source[colorOffset + 1] / 255.0f;
						alpha *= source[colorOffset + 3] / 255.0f;
					}
					auto byte = [](float value) { return static_cast<uint32_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255)); };
					// Remix's hardcoded vertex uses BGRA. Hair's green component
					// is a tint weight, not a literal green albedo multiplier.
					vertex.color = (byte(alpha) << 24) | (byte(std::lerp(1.0f, mesh.tint[0], mask)) << 16) |
						(byte(std::lerp(1.0f, mesh.tint[1], mask)) << 8) | byte(std::lerp(1.0f, mesh.tint[2], mask));
				}
				if (mesh.boneCount) {
					const auto offset = descriptor.GetAttributeOffset(RE::BSGraphics::Vertex::VA_SKINNING);
					if (offset + 12 > stride)
						return false;
					float sum = 0;
					for (uint32_t bone = 0; bone < 4; ++bone) {
						uint16_t half;
						std::memcpy(&half, source + offset + bone * 2, sizeof(half));
						weights[i * 4 + bone] = DirectX::PackedVector::XMConvertHalfToFloat(half);
						boneIndices[i * 4 + bone] = source[offset + 8 + bone];
						if (!std::isfinite(weights[i * 4 + bone]) || weights[i * 4 + bone] < 0)
							return false;
						// Unused influences can contain a sentinel index. The GPU
						// still fetches all four matrices, so canonicalize these.
						if (weights[i * 4 + bone] == 0)
							boneIndices[i * 4 + bone] = 0;
						else if (boneIndices[i * 4 + bone] >= mesh.boneCount)
							return false;
						sum += weights[i * 4 + bone];
					}
					if (sum <= 0)
						return false;
					for (uint32_t bone = 0; bone < 4; ++bone)
						weights[i * 4 + bone] /= sum;
				}
				if (sourceMaterial) {
					vertex.texcoord[0] = vertex.texcoord[0] * sourceMaterial->texCoordScale[0].x + sourceMaterial->texCoordOffset[0].x;
					vertex.texcoord[1] = vertex.texcoord[1] * sourceMaterial->texCoordScale[0].y + sourceMaterial->texCoordOffset[0].y;
				}
				if (mesh.water) {
					waterSourceUV[i] = { vertex.texcoord[0], vertex.texcoord[1] };
					// Native water maps are projected using absolute world XY, or
					// authored object UV * 1000. Camera rebasing must not move waves.
					const auto pos = mesh.lodWorld * RE::NiPoint3{ vertex.position[0], vertex.position[1], vertex.position[2] };
					vertex.texcoord[0] = mesh.waterObjectUV ? vertex.texcoord[0] * 1000 : pos.x;
					vertex.texcoord[1] = mesh.waterObjectUV ? vertex.texcoord[1] * 1000 : pos.y;
				}
				for (uint32_t axis = 0; axis < 2; ++axis) {
					mesh.uvBounds[axis] = std::min(mesh.uvBounds[axis], vertex.texcoord[axis]);
					mesh.uvBounds[axis + 2] = std::max(mesh.uvBounds[axis + 2], vertex.texcoord[axis]);
				}
			}
			if (!vertices.empty())
				for (size_t i = 0; i < mesh.uploadedColorSamples.size(); ++i)
					mesh.uploadedColorSamples[i] = vertices[i * (vertices.size() - 1) / (mesh.uploadedColorSamples.size() - 1)].color;
			std::vector<uint32_t> indices(mesh.indexCount);
			if (mesh.grass)
				logger::info("[RemixScene.grassColor] vertex RGB range={}..{} (8-bit native RGB; alpha reserved for wind)", grassColorMin, grassColorMax);
			for (size_t i = 0; i < indices.size(); ++i) {
				uint16_t value;
				std::memcpy(&value, indexBytes.data() + i * sizeof(value), sizeof(value));
				if (value >= mesh.vertexCount)
					return false;
				indices[i] = value;
			}
			remixapi_MeshInfoSurfaceTriangles surface{};
			if (mesh.lodLandscape)
				logger::info("[RemixScene.landLOD] lowered {}/{} vertices; range=({}, {}, {}, {}) world=({}, {}, {}) scale={}", loweredVertices, mesh.vertexCount,
					mesh.lodRange[0], mesh.lodRange[1], mesh.lodRange[2], mesh.lodRange[3], mesh.lodWorld.translate.x, mesh.lodWorld.translate.y, mesh.lodWorld.translate.z, mesh.lodWorld.scale);
			if (mesh.water && mesh.waterParameters[22] != 0 && vertices.size() >= 3) {
				// Fit original vertex UV from the projected coordinates once. This
				// retains exact native flow UVs without extending every RT vertex.
				// Reject non-affine mappings rather than silently distorting them.
				const double x0 = vertices[0].texcoord[0], y0 = vertices[0].texcoord[1];
				size_t first = 0, second = 0;
				double farthest = 0, largestArea = 0;
				for (size_t i = 1; i < vertices.size(); ++i) {
					const double x = vertices[i].texcoord[0] - x0, y = vertices[i].texcoord[1] - y0;
					if (x * x + y * y > farthest) { first = i; farthest = x * x + y * y; }
				}
				const double dx1 = vertices[first].texcoord[0] - x0, dy1 = vertices[first].texcoord[1] - y0;
				for (size_t i = 1; i < vertices.size(); ++i) {
					const double area = dx1 * (vertices[i].texcoord[1] - y0) - dy1 * (vertices[i].texcoord[0] - x0);
					if (std::abs(area) > std::abs(largestArea)) { second = i; largestArea = area; }
				}
				float maxError = FLT_MAX;
				if (std::abs(largestArea) > 1e-8) {
					const double dx2 = vertices[second].texcoord[0] - x0, dy2 = vertices[second].texcoord[1] - y0;
					for (uint32_t axis = 0; axis < 2; ++axis) {
						const double v0 = waterSourceUV[0][axis];
						const double dv1 = waterSourceUV[first][axis] - v0, dv2 = waterSourceUV[second][axis] - v0;
						const double a = (dv1 * dy2 - dv2 * dy1) / largestArea;
						const double b = (dx1 * dv2 - dx2 * dv1) / largestArea;
						mesh.waterParameters[12 + axis * 3] = static_cast<float>(a);
						mesh.waterParameters[13 + axis * 3] = static_cast<float>(b);
						mesh.waterParameters[14 + axis * 3] = static_cast<float>(v0 - a * x0 - b * y0);
					}
					maxError = 0;
					for (size_t i = 0; i < vertices.size(); ++i)
						for (uint32_t axis = 0; axis < 2; ++axis) {
							const auto* row = mesh.waterParameters.data() + 12 + axis * 3;
							const float value = row[0] * vertices[i].texcoord[0] + row[1] * vertices[i].texcoord[1] + row[2];
							maxError = std::max(maxError, std::abs(value - waterSourceUV[i][axis]));
						}
					mesh.waterFlowUVValid = std::isfinite(maxError) && maxError < 0.0002f;
				}
				if (!mesh.waterFlowUVValid) mesh.waterParameters[22] = 0;
				logger::info("[RemixScene.waterFlowUV] vertices={} affine={} maxError={} rows=({},{},{}) ({},{},{})", vertices.size(), mesh.waterFlowUVValid, maxError,
					mesh.waterParameters[12], mesh.waterParameters[13], mesh.waterParameters[14], mesh.waterParameters[15], mesh.waterParameters[16], mesh.waterParameters[17]);
				if (updateWater(mesh.materialHandle, mesh.waterParameters.data()) != REMIXAPI_ERROR_CODE_SUCCESS) return false;
			}
			if (mesh.grass) {
				// Determine whether an affine height shear can reproduce ALL native
				// vertex wind weights exactly. Do not animate with an assumed height ramp.
				double sumZ = 0, sumZZ = 0, sumW = 0, sumZW = 0;
				for (size_t i = 0; i < vertices.size(); ++i) {
					const double z = vertices[i].position[2];
					const double weight = double(grassWindWeights[i]) * grassWindWeights[i];
					sumZ += z; sumZZ += z * z; sumW += weight; sumZW += z * weight;
				}
				const double n = double(vertices.size());
				const double denominator = n * sumZZ - sumZ * sumZ;
				const double slope = std::abs(denominator) > 1e-12 ? (n * sumZW - sumZ * sumW) / denominator : 0;
				const double intercept = (sumW - slope * sumZ) / n;
				double maxError = 0;
				for (size_t i = 0; i < vertices.size(); ++i)
					maxError = std::max(maxError, std::abs(slope * vertices[i].position[2] + intercept - double(grassWindWeights[i]) * grassWindWeights[i]));
				logger::info("[RemixScene.grassWindMesh] vertices={} slope={} intercept={} maxWeightSquaredError={}", vertices.size(), slope, intercept, maxError);
				for (size_t i = 0; i < std::min<size_t>(vertices.size(), 32); ++i)
					logger::info("[RemixScene.grassWindVertex] i={} xyz=({},{},{}) uv=({},{}) weight={}", i,
						vertices[i].position[0], vertices[i].position[1], vertices[i].position[2], vertices[i].texcoord[0], vertices[i].texcoord[1], grassWindWeights[i]);
			}
			surface.vertices_values = vertices.data();
			surface.vertices_count = mesh.vertexCount;
			surface.indices_values = indices.data();
			surface.indices_count = mesh.indexCount;
			surface.material = mesh.materialHandle ? mesh.materialHandle : material;
			if (mesh.boneCount) {
				surface.skinning_hasvalue = true;
				surface.skinning_value = { 4, weights.data(), static_cast<uint32_t>(weights.size()), boneIndices.data(), static_cast<uint32_t>(boneIndices.size()) };
			}
			remixapi_MeshInfo info{};
			info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
			info.hash = updateExisting ? reinterpret_cast<uintptr_t>(mesh.handle) : ++nextHash;
			info.surfaces_values = &surface;
			info.surfaces_count = 1;
			if (updateExisting)
				return updateMesh && updateMesh(&info, authoredFrame ? tangentFrame.data() : nullptr,
					(mesh.modelSpaceNormals ? 1u : 0u) | (mesh.useFaceNormals ? 2u : 0u)) == REMIXAPI_ERROR_CODE_SUCCESS;
			if (mesh.useFaceNormals || mesh.grass)
				return createNativeMesh && createNativeMesh(&info, (mesh.modelSpaceNormals ? 1u : 0u) | (mesh.useFaceNormals ? 2u : 0u) | (mesh.grass ? 4u : 0u), &mesh.handle) == REMIXAPI_ERROR_CODE_SUCCESS;
			if (mesh.modelSpaceNormals)
				return createModelNormalMesh && createModelNormalMesh(&info, &mesh.handle) == REMIXAPI_ERROR_CODE_SUCCESS;
			if (authoredFrame) {
				mesh.nativeTangentFrame = createTangentMesh && createTangentMesh(&info, tangentFrame.data(), mesh.vertexCount,
					mesh.nativeLandscape ? 8u : 0u, &mesh.handle) == REMIXAPI_ERROR_CODE_SUCCESS;
				return mesh.nativeTangentFrame;
			}
			if (mesh.nativeLandscape)
				return createNativeMesh && createNativeMesh(&info, 8u, &mesh.handle) == REMIXAPI_ERROR_CODE_SUCCESS;
			return api->CreateMesh(&info, &mesh.handle) == REMIXAPI_ERROR_CODE_SUCCESS;
		}
	}

	bool Initialize(remixapi_Interface* remixInterface)
	{
		std::lock_guard lock(mutex);
		api = remixInterface;
		importMaterial = reinterpret_cast<ImportMaterialFn>(GetProcAddress(GetModuleHandleW(L"dxvk_d3d11.dll"), "csRemixCreateMaterialD3D11V3"));
		importThinMaterial = reinterpret_cast<ImportThinMaterialFn>(GetProcAddress(GetModuleHandleW(L"dxvk_d3d11.dll"), "csRemixCreateMaterialD3D11V4"));
		importSkinMaterial = reinterpret_cast<ImportThinMaterialFn>(GetProcAddress(GetModuleHandleW(L"dxvk_d3d11.dll"), "csRemixCreateSkinMaterialD3D11V1"));
		importFoliage = reinterpret_cast<ImportFoliageFn>(GetProcAddress(GetModuleHandleW(L"dxvk_d3d11.dll"), "csRemixCreateFoliageMaterialD3D11V1"));
		importLandscape = reinterpret_cast<ImportLandscapeFn>(GetProcAddress(GetModuleHandleW(L"dxvk_d3d11.dll"), "csRemixCreateLandscapeMaterialD3D11"));
		importWater = reinterpret_cast<ImportWaterFn>(GetProcAddress(GetModuleHandleW(L"dxvk_d3d11.dll"), "csRemixCreateWaterMaterialD3D11V3"));
		updateWater = reinterpret_cast<UpdateWaterFn>(GetProcAddress(GetModuleHandleW(L"dxvk_d3d11.dll"), "csRemixUpdateWaterMaterialV3"));
		importEffect = reinterpret_cast<ImportEffectFn>(GetProcAddress(GetModuleHandleW(L"dxvk_d3d11.dll"), "csRemixCreateEffectMaterialD3D11V1"));
		updateEffect = reinterpret_cast<UpdateWaterFn>(GetProcAddress(GetModuleHandleW(L"dxvk_d3d11.dll"), "csRemixUpdateEffectMaterialV1"));
		createModelNormalMesh = reinterpret_cast<CreateModelNormalMeshFn>(GetProcAddress(GetModuleHandleW(L"dxvk_d3d11.dll"), "csRemixCreateMeshMSN"));
		updateMesh = reinterpret_cast<UpdateMeshFn>(GetProcAddress(GetModuleHandleW(L"dxvk_d3d11.dll"), "csRemixUpdateMeshV1"));
		createTangentMesh = reinterpret_cast<CreateTangentMeshFn>(GetProcAddress(GetModuleHandleW(L"dxvk_d3d11.dll"), "csRemixCreateMeshTBNV2"));
		createNativeMesh = reinterpret_cast<CreateNativeMeshFn>(GetProcAddress(GetModuleHandleW(L"dxvk_d3d11.dll"), "csRemixCreateMeshNative"));
		createInstanceSet = reinterpret_cast<CreateInstanceSetFn>(GetProcAddress(GetModuleHandleW(L"dxvk_d3d11.dll"), "csRemixCreateInstanceSet"));
		destroyInstanceSet = reinterpret_cast<DestroyInstanceSetFn>(GetProcAddress(GetModuleHandleW(L"dxvk_d3d11.dll"), "csRemixDestroyInstanceSet"));
		drawInstanceSet = reinterpret_cast<DrawInstanceSetFn>(GetProcAddress(GetModuleHandleW(L"dxvk_d3d11.dll"), "csRemixDrawInstanceSet"));
		createGrassInstanceSet = reinterpret_cast<CreateGrassInstanceSetFn>(GetProcAddress(GetModuleHandleW(L"dxvk_d3d11.dll"), "csRemixCreateGrassInstanceSetV2"));
		drawGrassInstanceSet = reinterpret_cast<DrawGrassInstanceSetFn>(GetProcAddress(GetModuleHandleW(L"dxvk_d3d11.dll"), "csRemixDrawGrassInstanceSet"));
		createRetainedInstance = reinterpret_cast<CreateRetainedFn>(GetProcAddress(GetModuleHandleW(L"dxvk_d3d11.dll"), "csRemixCreateRetainedInstance"));
		updateRetainedInstance = reinterpret_cast<UpdateRetainedFn>(GetProcAddress(GetModuleHandleW(L"dxvk_d3d11.dll"), "csRemixUpdateRetainedInstance"));
		updateRetainedTransform = reinterpret_cast<UpdateRetainedTransformFn>(GetProcAddress(GetModuleHandleW(L"dxvk_d3d11.dll"), "csRemixUpdateRetainedInstanceTransform"));
		createRetainedInstanceSet = reinterpret_cast<CreateRetainedSetFn>(GetProcAddress(GetModuleHandleW(L"dxvk_d3d11.dll"), "csRemixCreateRetainedInstanceSet"));
		destroyRetainedInstance = reinterpret_cast<DestroyRetainedFn>(GetProcAddress(GetModuleHandleW(L"dxvk_d3d11.dll"), "csRemixDestroyRetainedInstance"));
		drawRetainedInstances = reinterpret_cast<DrawRetainedFn>(GetProcAddress(GetModuleHandleW(L"dxvk_d3d11.dll"), "csRemixDrawRetainedInstances"));
		logger::info("[RemixScene] retained instance API {}",
			createRetainedInstance && updateRetainedInstance && destroyRetainedInstance && drawRetainedInstances ? "available" : "unavailable");
		if (!createInstanceSet || !destroyInstanceSet || !drawInstanceSet || !createGrassInstanceSet || !drawGrassInstanceSet) {
			logger::error("[RemixScene] required retained native instance-set API unavailable");
			return false;
		}
		logger::info("[RemixScene] model-space normal mesh extension: {}", createModelNormalMesh != nullptr);
		logger::info("[RemixScene] native diffuse texture import: {}", importMaterial != nullptr);
		remixapi_MaterialInfoOpaqueEXT opaque{};
		opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
		opaque.albedoConstant = { 0.5f, 0.5f, 0.5f };
		opaque.opacityConstant = 1;
		opaque.roughnessConstant = 0.8f;
		opaque.alphaTestType = 7;
		remixapi_MaterialInfo info{};
		info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
		info.pNext = &opaque;
		info.hash = 0x43535343454e4501ull;
		info.spriteSheetRow = info.spriteSheetCol = 1;
		if (api->CreateMaterial(&info, &material) != REMIXAPI_ERROR_CODE_SUCCESS)
			return false;
		return true;
	}

	void RecordGrassScale(RE::BSGeometry* geometry, const RE::NiPoint3& scale)
	{
		if (!geometry)
			return;
		std::lock_guard lock(mutex);
		auto found = grassScales.find(geometry);
		if (found == grassScales.end())
			grassScales.emplace(geometry, GrassScaleHint{ RE::NiPointer<RE::BSGeometry>(geometry), scale });
		else if (found->second.scale != scale)
			found->second.scale = scale;
	}

	void RecordTreeWind(const float* treeParams, const float* windTimers)
	{
		if (!treeParams || !windTimers)
			return;
		std::lock_guard lock(mutex);
		static std::array<float, 6> lastTreeWind{};
		const std::array<float, 6> current{ treeParams[0], treeParams[1], treeParams[2], treeParams[3],
			windTimers[0], windTimers[1] };
		if (current == lastTreeWind)
			return;
		lastTreeWind = current;
		static uint32_t reports = 0;
		if (reports < 6) {
			++reports;
			logger::info("[RemixScene.treeWind] TreeParams=({}, {}, {}, {}) WindTimers=({}, {})",
				current[0], current[1], current[2], current[3], current[4], current[5]);
		}
	}

	void RecordGrassWind(RE::BSGeometry* geometry, const RE::NiPoint3& wind, float timer, float previousTimer)
	{
		if (!geometry)
			return;
		std::lock_guard lock(mutex);
		RE::NiPoint3 independentWind;
		float independentTimer;
		const auto* property = netimmerse_cast<RE::BSGrassShaderProperty*>(geometry->GetGeometryRuntimeData().shaderProperty.get());
		if (RemixNativeRender::ReadGrassWind(geometry, property, independentWind, independentTimer)) {
			++grassWindAuditSamples;
			grassWindAuditMaxError = std::max({ grassWindAuditMaxError, std::abs(wind.x - independentWind.x),
				std::abs(wind.y - independentWind.y), std::abs(wind.z - independentWind.z), std::abs(timer - independentTimer) });
		}
		auto [it, _] = grassScales.try_emplace(geometry, GrassScaleHint{ RE::NiPointer<RE::BSGeometry>(geometry), {} });
		auto& hint = it->second;
		if (!hint.hasWind || hint.wind != wind || hint.timer != timer || hint.previousTimer != previousTimer) {
			hint.wind = wind;
			hint.timer = timer;
			hint.previousTimer = previousTimer;
			hint.hasWind = true;
			++grassWindRevision;
		}
	}

	static void CaptureParticles(RE::NiParticleSystem* system, const RE::BSEffectShaderMaterial* source,
		const std::array<ID3D11ShaderResourceView*, 2>& views, const std::array<uintptr_t, 5>& textures,
		const std::array<float, 20>& parameters)
	{
		std::vector<RemixNativeRender::ParticleVertex> native;
		if (!RemixNativeRender::BuildParticleVertices(system, native)) {
			RetireInstanceIfStillEmpty(system, "particles: unreadable");
			return;
		}
		if (native.empty()) {
			// The emitter is between bursts. Nothing should be drawn, so the
			// retire is correct; only the tear-down and rebuild of the retained
			// registration is waste, which the grace window absorbs.
			RetireInstanceIfStillEmpty(system, "particles: none alive");
			return;
		}
		const auto& inputs = system->GetParticlesRuntimeData().particleData->GetParticlesRuntimeData();
		const auto center = system->GetModelData().modelBound.center;
		auto found = meshes.find(system);
		const bool newMaterial = found == meshes.end() || found->second.effectTextures != textures;
		const bool changed = newMaterial || found->second.particleCenter != center || found->second.particleVertices.size() != native.size() ||
			std::memcmp(found->second.particleVertices.data(), native.data(), native.size() * sizeof(native[0])) != 0;
		if (changed) {
			if (found == meshes.end() && createdThisFrame >= 24) return;
			std::vector<remixapi_HardcodedVertex> vertices;
			vertices.reserve(native.size());
			std::vector<uint32_t> indices;
			indices.reserve(native.size() / 4 * 6);
			// Native static stream at 0x1012850: these UVs, and indices 012/023.
			constexpr float corners[4][2]{ { 0, 1 }, { 1, 1 }, { 1, 0 }, { 0, 0 } };
			for (size_t q = 0; q < native.size(); q += 4) {
				const auto edge0 = native[q + 1].position - native[q].position;
				const auto edge1 = native[q + 2].position - native[q].position;
				auto normal = edge0.Cross(edge1);
				const float length = normal.Length();
				if (!std::isfinite(length) || length < 1e-7f) continue; // Native fade can collapse a quad.
				normal = normal / length;
				const auto firstVertex = static_cast<uint32_t>(vertices.size());
				for (size_t corner = 0; corner < 4; ++corner) {
					const auto& v = native[q + corner];
					remixapi_HardcodedVertex out{};
					const auto p = v.position + center; // Undo native model-bound rebasing once.
					if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) ||
						!std::isfinite(v.textureIndex) || v.textureIndex < 0 || v.textureIndex > 255) return;
					out.position[0] = p.x; out.position[1] = p.y; out.position[2] = p.z;
					out.normal[0] = normal.x; out.normal[1] = normal.y; out.normal[2] = normal.z;
					out.texcoord[0] = corners[corner][0]; out.texcoord[1] = corners[corner][1];
					const auto index = static_cast<uint32_t>(v.textureIndex);
					if (inputs.subtextureOffsets && index < inputs.subTextureOffsetsCount && inputs.subTextureOffsetsCount <= 128) {
						const auto& rect = inputs.subtextureOffsets[index];
						out.texcoord[0] = rect.red + rect.green * out.texcoord[0];
						out.texcoord[1] = rect.blue + rect.alpha * out.texcoord[1];
					}
					// Engine stream is RGBA8; Remix's HardcodedVertex is BGRA8.
					out.color = (v.rgba & 0xff00ff00u) | ((v.rgba & 0xffu) << 16) | ((v.rgba >> 16) & 0xffu);
					vertices.push_back(out);
				}
				for (const uint32_t i : { 0u, 1u, 2u, 0u, 2u, 3u }) indices.push_back(firstVertex + i);
			}
			if (indices.empty()) { RetireInstanceIfStillEmpty(system, "particles: no indices"); return; }
			remixapi_MaterialHandle handle = newMaterial ? nullptr : found->second.materialHandle;
			if (newMaterial) {
				remixapi_MaterialInfoOpaqueEXT opaque{};
				opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
				opaque.albedoConstant = { 1, 1, 1 }; opaque.opacityConstant = 1; opaque.roughnessConstant = 1;
				opaque.useDrawCallAlphaState = true; opaque.alphaTestType = 7;
				remixapi_MaterialInfo info{}; info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
				info.pNext = &opaque; info.hash = ++nextHash; info.spriteSheetRow = info.spriteSheetCol = 1;
				if (importEffect(&info, views[0], views[1], static_cast<uint32_t>(textures[4]), parameters.data(), &handle) != REMIXAPI_ERROR_CODE_SUCCESS) return;
			}
			remixapi_MeshInfoSurfaceTriangles surface{};
			surface.vertices_values = vertices.data(); surface.vertices_count = vertices.size();
			surface.indices_values = indices.data(); surface.indices_count = indices.size(); surface.material = handle;
			remixapi_MeshInfo info{}; info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
			info.hash = ++nextHash; info.surfaces_values = &surface; info.surfaces_count = 1;
			remixapi_MeshHandle meshHandle = nullptr;
			if (api->CreateMesh(&info, &meshHandle) != REMIXAPI_ERROR_CODE_SUCCESS) {
				if (newMaterial) api->DestroyMaterial(handle);
				++failedUploadsThisFrame;
				return;
			}
			if (found != meshes.end()) {
				api->DestroyMesh(found->second.handle);
				if (newMaterial) api->DestroyMaterial(found->second.materialHandle);
			} else {
				++createdThisFrame;
				found = meshes.emplace(system, Mesh{}).first;
				logger::info("[RemixScene.particleMaterial] '{}' source='{}' particles={} atlas={}", system->name.c_str(), source->sourceTexturePath.c_str(), native.size() / 4, inputs.subTextureOffsetsCount);
			}
			auto& mesh = found->second;
			mesh.handle = meshHandle; mesh.materialHandle = handle; mesh.effect = mesh.particles = true;
			mesh.effectTextures = textures;
			if (newMaterial) mesh.effectParameters = parameters;
			mesh.particleVertices = std::move(native); mesh.particleCenter = center;
			mesh.vertexCount = static_cast<uint32_t>(vertices.size()); mesh.indexCount = static_cast<uint32_t>(indices.size());
			++mesh.particleRevision;
			mesh.boundsMin = { FLT_MAX, FLT_MAX, FLT_MAX }; mesh.boundsMax = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
			for (const auto& v : vertices) {
				mesh.boundsMin.x = std::min(mesh.boundsMin.x, v.position[0]); mesh.boundsMax.x = std::max(mesh.boundsMax.x, v.position[0]);
				mesh.boundsMin.y = std::min(mesh.boundsMin.y, v.position[1]); mesh.boundsMax.y = std::max(mesh.boundsMax.y, v.position[1]);
				mesh.boundsMin.z = std::min(mesh.boundsMin.z, v.position[2]); mesh.boundsMax.z = std::max(mesh.boundsMax.z, v.position[2]);
			}
		}
		auto& mesh = found->second;
		if (mesh.effectParameters != parameters && updateEffect(mesh.materialHandle, parameters.data()) == REMIXAPI_ERROR_CODE_SUCCESS)
			mesh.effectParameters = parameters;
		Instance instance{ mesh.handle, system->world, {}, {} };
		auto& blend = instance.blend;
		blend.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT;
		blend.textureColorArg1Source = blend.textureAlphaArg1Source = 1;
		blend.textureColorArg2Source = 2; blend.textureAlphaArg2Source = 3;
		blend.textureColorOperation = 1; blend.textureAlphaOperation = 3;
		blend.tFactor = 0xffffffff; blend.writeMask = 15;
		blend.srcColorBlendFactor = blend.srcAlphaBlendFactor = 1;
		blend.alphaTestCompareOp = 7;
		if (const auto* alpha = system->GetGeometryRuntimeData().alphaProperty.get()) {
			constexpr uint32_t compares[]{ 7, 1, 2, 3, 4, 5, 6, 0 };
			blend.alphaTestEnabled = alpha->GetAlphaTesting();
			blend.alphaTestCompareOp = compares[(alpha->alphaFlags >> 10) & 7];
			blend.alphaTestReferenceValue = alpha->alphaThreshold;
			constexpr uint32_t factors[]{ 1, 0, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
			const auto src = static_cast<uint32_t>(alpha->GetSrcBlendMode());
			const auto dst = static_cast<uint32_t>(alpha->GetDestBlendMode());
			if (alpha->GetAlphaBlending() && src < std::size(factors) && dst < std::size(factors)) {
				blend.alphaBlendEnabled = true;
				blend.srcColorBlendFactor = blend.srcAlphaBlendFactor = factors[src];
				blend.dstColorBlendFactor = blend.dstAlphaBlendFactor = factors[dst];
			}
		}
		StoreInstance(system, std::move(instance));
	}

	using BoneSnapshot = std::unordered_map<const RE::NiTransform*, RE::NiTransform>;
	static BoneSnapshot nativeViewModelBones;
	static std::vector<RE::NiPointer<RE::BSGeometry>> nativeViewModelOwners;
	static std::unordered_map<RE::BSGeometry*, RE::NiTransform> nativeViewModelWorlds, nativeViewModelAttachments;
	static std::unordered_map<RE::BSGeometry*, std::string> nativeViewModelAnchors;
	static uint32_t nativeViewModelFrame = 0;
	static nlohmann::json nativeViewModelPoseAudit;
	static uint32_t nativeViewModelPoseFrame = 0;

	void AuditNativeViewModelPose()
	{
		if (!auditEnabled.load(std::memory_order_relaxed)) return;
		nativeViewModelPoseAudit = nlohmann::json::array();
		nativeViewModelPoseFrame = globals::state->frameCount;
		// Copy values here, not pointers for deferred inspection. Native model
		// coordinates are temporarily rebased; rotations are translation invariant.
		for (auto* geometry : viewModelGeometry) {
			const auto* skin = geometry->GetGeometryRuntimeData().skinInstance.get();
			if (!skin || !skin->skinData || !skin->boneWorldTransforms) continue;
			const auto count = skin->skinData->GetBoneCount();
			if (count > REMIXAPI_INSTANCE_INFO_MAX_BONES_COUNT) continue;
			auto rotations = nlohmann::json::array();
			for (uint32_t bone = 0; bone < count; ++bone) {
				const auto* source = skin->boneWorldTransforms[bone];
				auto rotation = nlohmann::json::array();
				if (source) {
					const auto value = *source;
					for (const auto& row : value.rotate.entry)
						for (float entry : row) rotation.push_back(value.scale * entry);
				}
				rotations.push_back(std::move(rotation));
			}
			nativeViewModelPoseAudit.push_back({ { "geometry", reinterpret_cast<uintptr_t>(geometry) },
				{ "name", geometry->name.c_str() }, { "rotations", std::move(rotations) } });
		}
	}

	static void SnapshotBones(const std::vector<RE::BSGeometry*>& geometries, BoneSnapshot& snapshot)
	{
		for (auto* geometry : geometries) {
			const auto* skin = geometry->GetGeometryRuntimeData().skinInstance.get();
			if (!skin || !skin->skinData || !skin->boneWorldTransforms) continue;
			const auto count = skin->skinData->GetBoneCount();
			if (count > REMIXAPI_INSTANCE_INFO_MAX_BONES_COUNT) continue;
			for (uint32_t bone = 0; bone < count; ++bone) {
				const auto* source = skin->boneWorldTransforms[bone];
				if (source && !snapshot.contains(source)) snapshot.emplace(source, *source);
			}
		}
	}

	static void SnapshotViewModelAttachments(const BoneSnapshot& snapshot,
		std::unordered_map<RE::BSGeometry*, RE::NiTransform>& transforms = attachmentAuditTransforms,
		std::unordered_map<RE::BSGeometry*, std::string>& anchors = attachmentAuditAnchors,
		const std::unordered_set<RE::BSGeometry*>& geometries = viewModelGeometry)
	{
		std::unordered_map<const RE::NiAVObject*, const RE::NiTransform*> nodeBones;
		for (auto* geometry : geometries) {
			const auto* skin = geometry->GetGeometryRuntimeData().skinInstance.get();
			if (!skin || !skin->skinData || !skin->bones || !skin->boneWorldTransforms) continue;
			const auto count = skin->skinData->GetBoneCount();
			if (count > REMIXAPI_INSTANCE_INFO_MAX_BONES_COUNT) continue;
			for (uint32_t bone = 0; bone < count; ++bone) {
				const auto sampled = snapshot.find(skin->boneWorldTransforms[bone]);
				if (skin->bones[bone] && sampled != snapshot.end()) nodeBones.emplace(skin->bones[bone], &sampled->second);
			}
		}
		for (auto* geometry : geometries) {
			if (geometry->GetGeometryRuntimeData().skinInstance || transforms.contains(geometry)) continue;
			RE::NiTransform localChain;
			auto* node = static_cast<RE::NiAVObject*>(geometry);
			for (uint32_t depth = 0; node && depth < 64; ++depth, node = node->parent) {
				if (const auto anchor = nodeBones.find(node); anchor != nodeBones.end()) {
					transforms.emplace(geometry, *anchor->second * localChain);
					anchors.emplace(geometry, node->name.c_str());
					break;
				}
				localChain = node->local * localChain;
			}
		}
	}

	void CaptureViewModelPose()
	{
		nativeViewModelFrame = globals::state->frameCount;
		nativeViewModelBones.clear();
		nativeViewModelWorlds.clear();
		nativeViewModelAttachments.clear();
		nativeViewModelAnchors.clear();
		nativeViewModelOwners.clear();
		const std::vector<RE::BSGeometry*> geometries(viewModelGeometry.begin(), viewModelGeometry.end());
		// Gather may detach a previously captured mesh before Submit consumes
		// this snapshot. Keep its identity alive across that discovery boundary.
		for (auto* geometry : geometries) nativeViewModelOwners.emplace_back(geometry);
		SnapshotBones(geometries, nativeViewModelBones);
		for (auto* geometry : geometries) nativeViewModelWorlds.emplace(geometry, geometry->world);
		SnapshotViewModelAttachments(nativeViewModelBones, nativeViewModelAttachments, nativeViewModelAnchors);
		AuditNativeViewModelPose();
	}

	static void Capture(RE::BSGeometry* geometry, const BoneSnapshot& boneSnapshot)
	{
		if (!geometry)
			return;
		std::lock_guard lock(mutex);
		if (!api || !material)
			return;
		auto* shape = geometry->AsTriShape();
		const auto& data = geometry->GetGeometryRuntimeData();
		// Projected-size cull. The frame cost is 2.7 microseconds per registered
		// instance per frame, spread across the retained replay, prepareSceneData
		// and the GPU's TLAS build, and it does not care whether the instance is
		// a house or a pebble two hundred metres away. Retiring the ones whose
		// bounding sphere subtends less than a threshold takes them out of all
		// three at once.
		//
		// Size rather than distance: a distance cut removes a mountain and a
		// twig at the same range, and the twig was the one costing the same as
		// the mountain for nothing. Zero disables it.
		static const float minProjectedSize = [] {
			char value[32]{};
			if (!GetEnvironmentVariableA("CS_REMIX_MIN_PROJECTED_SIZE", value, sizeof(value)))
				return 0.0f;
			return std::strtof(value, nullptr);
		}();
		if (minProjectedSize > 0.0f && !viewModelGeometry.contains(geometry) && !playerBody.contains(geometry)) {
			const auto& bound = geometry->worldBound;
			if (bound.radius > 0.0f) {
				const auto eye = globals::game::shadowState->GetRuntimeData().posAdjust.getEye();
				const float dx = bound.center.x - eye.x, dy = bound.center.y - eye.y, dz = bound.center.z - eye.z;
				const float distanceSquared = dx * dx + dy * dy + dz * dz;
				// radius / distance, compared without the square root.
				if (distanceSquared > 0.0f &&
					bound.radius * bound.radius < minProjectedSize * minProjectedSize * distanceSquared) {
					RetireInstance(geometry, "below projected size");
					return;
				}
			}
		}
		// These placements come from sampled bones, not geometry->world. The
		// static probe does not cover that input and must never retain its pose.
		const bool animatedAttachment = attachmentAuditTransforms.contains(geometry);
		if (animatedAttachment) {
			if (auto cached = classifications.find(geometry); cached != classifications.end())
				cached->second.probeEligible = false;
		}
		// Fast path: a plain opaque static shape that captured successfully and
		// whose per-frame inputs are all unchanged cannot alter its mesh,
		// material or instance. Skipping its full derivation is what keeps the
		// retained scene from re-deriving thousands of unchanged objects a frame.
		if (const auto cached = classifications.find(geometry); cached != classifications.end() && cached->second.probeEligible) {
			auto& kinds = cached->second;
			// Spread the settled objects over the interval so each frame reads a
			// quarter of them rather than all of them on one frame in four.
			// These objects are cache-line aligned, so the low pointer bits carry
			// no entropy and taking them directly put every object in the same
			// slot. Mix before reducing.
			const uint64_t stagger = ((reinterpret_cast<uintptr_t>(geometry) >> 6) * 0x9e3779b97f4a7c15ull >> 33) % kProbeStaggerInterval;
			if (kinds.stableFrames >= kProbeStaggerThreshold && (frame % kProbeStaggerInterval) != stagger) {
				++probeStaggeredThisFrame;
				return;
			}
			const auto probe = BuildStaticProbe(geometry, data, shape, kinds);
			if (probe == kinds.probe) {
				++kinds.stableFrames;
				probeMaxStableFrames = std::max(probeMaxStableFrames, kinds.stableFrames);
				++probeHitsThisFrame;
				return;
			}
			kinds.stableFrames = 0;
			kinds.probeEligible = false;
			++probeChangedThisFrame;
		}
		++probeMissesThisFrame;
		static std::unordered_set<RE::BSGeometry*> audited;
		static std::unordered_map<uint32_t, uint32_t> auditCounts;
		const auto materialType = data.shaderProperty ? static_cast<int>(data.shaderProperty->GetMaterialType()) : -1;
		const uint32_t category = static_cast<uint32_t>(geometry->GetType().get()) | (uint32_t(bool(data.skinInstance)) << 8) | (uint32_t(materialType + 1) << 16);
		if (audited.size() < 10000 && audited.insert(geometry).second && auditCounts[category]++ < 6) {
			logger::info("[RemixScene.audit] '{}' parent='{}' type={} skin={} renderer={} materialType={} vertices={} triangles={}",
				geometry->name.c_str(), geometry->parent ? geometry->parent->name.c_str() : "", static_cast<int>(geometry->GetType().get()), bool(data.skinInstance),
				data.rendererData != nullptr, materialType, shape ? shape->GetTrishapeRuntimeData().vertexCount : 0, shape ? shape->GetTrishapeRuntimeData().triangleCount : 0);
		}
		auto& kinds = Classify(geometry, data, shape != nullptr);
		auto* particles = kinds.particles;
		if (!shape && !particles)
			return;
		const bool distantTree = kinds.distantTree;
		const bool grass = kinds.grass;
		const auto* waterProperty = kinds.water ? static_cast<const RE::BSWaterShaderProperty*>(data.shaderProperty.get()) : nullptr;
		const bool water = waterProperty && waterProperty->material && materialType == static_cast<int>(RE::BSShaderMaterial::Type::kWater);
		if (water && RemixWaterVisibility::IsRasterOverlay(waterProperty->waterFlags.underlying())) {
			// Native SetupGeometry (0x154db70) masks WADING by the corresponding
			// water's stencil bit. AppCulled can be false for multiple heights, so
			// these quads cannot be independent RT medium boundaries. Ripple data
			// must instead modify the real water surface; that transport is pending.
			RetireInstance(geometry, "stencil-only wading overlay");
			return;
		}
		const auto* effectProperty = kinds.effect ? static_cast<const RE::BSEffectShaderProperty*>(data.shaderProperty.get()) : nullptr;
		const auto* effectMaterial = effectProperty && effectProperty->material && effectProperty->material->GetType() == RE::BSShaderMaterial::Type::kEffect ?
			static_cast<const RE::BSEffectShaderMaterial*>(effectProperty->material) : nullptr;
		const auto* refractionProperty = materialType == static_cast<int>(RE::BSShaderMaterial::Type::kLighting) &&
			data.shaderProperty->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kRefraction) ?
			static_cast<const RE::BSLightingShaderProperty*>(data.shaderProperty.get()) : nullptr;
		const bool refraction = refractionProperty && refractionProperty->material;
		const bool effect = effectMaterial != nullptr || refraction;
		std::array<ID3D11ShaderResourceView*, 2> effectViews{};
		std::array<uintptr_t, 5> effectTextures{};
		std::array<float, 20> effectParameters{};
		if (effectMaterial) {
			// Untextured editor geometry, blood's special channel semantics and
			// projected UV need independent native paths; never import defaults.
			if (!importEffect || !updateEffect || effectMaterial->sourceTexturePath.empty() ||
				effectProperty->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kWeaponBlood, RE::BSShaderProperty::EShaderPropertyFlag::kProjectedUV)) return;
			const RE::NiSourceTexture* textures[]{ effectMaterial->sourceTexture.get(), effectMaterial->greyscaleTexture.get() };
			for (uint32_t i = 0; i < 2; ++i) {
				effectViews[i] = textures[i] && textures[i]->rendererTexture ? reinterpret_cast<ID3D11ShaderResourceView*>(textures[i]->rendererTexture->resourceView) : nullptr;
				effectTextures[i * 2] = reinterpret_cast<uintptr_t>(textures[i]);
				effectTextures[i * 2 + 1] = reinterpret_cast<uintptr_t>(effectViews[i]);
			}
			if (!effectViews[0]) return;
			effectTextures[4] = effectMaterial->effectClampMode.underlying();
			const auto flags = effectProperty->flags.underlying();
			uint32_t shaderFlags = 0;
			if (!effectMaterial->greyscaleTexturePath.empty() && effectViews[1]) {
				if (flags & (1ull << 4)) shaderFlags |= 4;
				if (flags & (1ull << 5)) shaderFlags |= 8;
			}
			if (flags & (1ull << 37)) shaderFlags |= 16 | 32; // Native VC multiplies RGBA.
			if (flags & (1ull << 6)) shaderFlags |= 64;
			if (flags & (1ull << 62)) shaderFlags |= 128;
			if (flags & (1ull << 30)) shaderFlags |= 256;
			// Effect output is in Skyrim gamma space when Linear Lighting is off.
			if (!globals::features::linearLighting.GetCommonBufferData().enableLinearLighting) shaderFlags |= 4096;
			const auto& color = effectMaterial->baseColor;
			const auto external = effectProperty->emittanceColor ? *effectProperty->emittanceColor : RE::NiColor{ 1, 1, 1 };
			const auto uvIndex = RemixNativeRender::ReadEffectUVIndex();
			effectParameters = { effectMaterial->texCoordOffset[uvIndex].x, effectMaterial->texCoordOffset[uvIndex].y,
				effectMaterial->texCoordScale[uvIndex].x, effectMaterial->texCoordScale[uvIndex].y,
				color.red, color.green, color.blue, color.alpha,
				effectMaterial->falloffStartAngle, effectMaterial->falloffStopAngle, effectMaterial->falloffStartOpacity, effectMaterial->falloffStopOpacity,
				effectMaterial->baseColorScale, effectProperty->alpha, effectMaterial->softFalloffDepth, float(effectMaterial->unk81) / 255.0f,
				external.red, external.green, external.blue, static_cast<float>(shaderFlags) };
		}
		if (refraction) {
			if (!importEffect || !updateEffect) return;
			const auto* source = static_cast<const RE::BSLightingShaderMaterialBase*>(refractionProperty->material);
			const auto* texture = source->diffuseTexture.get();
			effectViews[0] = texture && texture->rendererTexture ? reinterpret_cast<ID3D11ShaderResourceView*>(texture->rendererTexture->resourceView) : nullptr;
			if (!effectViews[0]) return;
			effectTextures = { reinterpret_cast<uintptr_t>(texture), reinterpret_cast<uintptr_t>(effectViews[0]), 0, 0,
				static_cast<uintptr_t>(source->textureClampMode) };
			const auto flags = refractionProperty->flags.underlying();
			uint32_t shaderFlags = 512;
			if (flags & (1ull << 37)) shaderFlags |= 32;
			if (flags & (1ull << 16)) shaderFlags |= 1024;
			if (flags & (1ull << 13)) shaderFlags |= 2048;
			const auto uvIndex = RemixNativeRender::ReadEffectUVIndex();
			// Utility SetupGeometry's EyePos.w is property+0x104; the field also
			// serves as envmap LOD fade for ordinary lighting materials.
			effectParameters = { source->texCoordOffset[uvIndex].x, source->texCoordOffset[uvIndex].y,
				source->texCoordScale[uvIndex].x, source->texCoordScale[uvIndex].y,
				1, 1, 1, 1, source->refractionPower, 0, 0, 0, 0, refractionProperty->envmapLODFade,
				0, 0, 0, 0, 0, static_cast<float>(shaderFlags) };
		}
		// Whether an effect animates at all is a question about these numbers:
		// 0/1 are the scrolling UV offset the game's controllers drive, 12 is the
		// emissive scale and 13 the property alpha. A flame whose offsets never
		// move is a still image no matter how the material is shaded.
		if (effect && frame % 120 == 0) {
			static uint32_t reported = 0;
			if (reported < 6) {
				++reported;
				logger::info("[RemixScene.effectAnim] '{}' uvOffset=({:.5f}, {:.5f}) uvScale=({:.3f}, {:.3f}) colorScale={:.3f} alpha={:.3f} flags={:X}",
					geometry->name.c_str(), effectParameters[0], effectParameters[1], effectParameters[2], effectParameters[3],
					effectParameters[12], effectParameters[13], static_cast<uint32_t>(effectParameters[19]));
			}
		}
		if (particles) {
			if (effect) CaptureParticles(particles, effectMaterial, effectViews, effectTextures, effectParameters);
			return;
		}
		if (water && (!importWater || !updateWater)) return;
		if (grass) {
			const auto* property = static_cast<RE::BSGrassShaderProperty*>(data.shaderProperty.get());
			const bool uniformScale = property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kUniformScale);
			const RE::NiPoint3 scale{ uniformScale ? 1.0f : 0.0f, uniformScale ? 1.0f : 0.0f, 1.0f };
			auto [it, _] = grassScales.try_emplace(geometry, GrassScaleHint{ RE::NiPointer<RE::BSGeometry>(geometry), scale });
			auto& hint = it->second;
			hint.scale = scale;
			RE::NiPoint3 wind;
			float timer;
			if (RemixNativeRender::ReadGrassWind(geometry, property, wind, timer)) {
				if (!hint.hasWind || hint.wind != wind || hint.timer != timer) {
					hint.previousTimer = hint.hasWind ? hint.timer : timer;
					hint.wind = wind;
					hint.timer = timer;
					hint.hasWind = true;
					++grassWindRevision;
				}
			}
		}
		const auto scaleHint = grassScales.find(geometry);
		const auto* grassScale = scaleHint != grassScales.end() ? &scaleHint->second.scale : nullptr;
		if (!data.shaderProperty ||
			(data.shaderProperty->GetMaterialType() != RE::BSShaderMaterial::Type::kLighting && !distantTree && !water && !effect))
			return;
		auto* rendererData = data.rendererData;
		auto* skin = data.skinInstance.get();
		const auto* partition = skin ? skin->skinPartition.get() : nullptr;
		uint32_t vertexCount = shape->GetTrishapeRuntimeData().vertexCount;
		uint32_t indexCount = uint32_t(shape->GetTrishapeRuntimeData().triangleCount) * 3;
		uint32_t boneCount = 0;
		std::vector<uint8_t> partitionVisibility;
		if (skin) {
			if (!partition || !partition->numPartitions || partition->numPartitions > 128 || !skin->skinData || !skin->boneWorldTransforms)
				return;
			boneCount = skin->skinData->GetBoneCount();
			if (!boneCount || boneCount > REMIXAPI_INSTANCE_INFO_MAX_BONES_COUNT)
				return;
			rendererData = partition->partitions[0].buffData;
			vertexCount = partition->vertexCount;
			indexCount = 0;
			partitionVisibility.resize(partition->numPartitions, 1);
			if (auto* dismember = netimmerse_cast<RE::BSDismemberSkinInstance*>(skin)) {
				const auto& parts = dismember->GetRuntimeData();
				if (!parts.partitions || parts.numPartitions != static_cast<int32_t>(partition->numPartitions))
					return;
				for (uint32_t i = 0; i < partition->numPartitions; ++i)
					partitionVisibility[i] = parts.partitions[i].editorVisible;
			}
			for (uint32_t i = 0; i < partition->numPartitions; ++i)
				if (partitionVisibility[i])
					indexCount += uint32_t(partition->partitions[i].triangles) * 3;
		}
		if (!rendererData || !vertexCount || !indexCount)
			return;
		const auto sourceIndexCount = indexCount;
		const std::vector<std::pair<uint32_t, uint32_t>>* indexRangesSource = &noIndexRanges;
		if (!skin) {
			if (auto* segmented = kinds.subIndex) {
				// Ghidra: 0x518d40/0x518fa0 enable these segments according to
				// loaded-cell coverage. OnVisible (0xff6730) only reads the count;
				// it does not write frustum visibility into this array. Preserve
				// genuine LOD coverage changes to avoid overlapping near/far meshes.
				const auto& segments = segmented->GetSubIndexedTrishapeRuntimeData();
				if (!segments.nonSegmented && segments.numSegments) {
					if (!segments.segmentData || segments.numSegments > 65536)
						return;
					// Hash the native table rather than rebuilding the merged
					// ranges every frame; coverage changes only when the engine
					// rewrites these counts/flags.
					const auto signature = SegmentSignature(segments, sourceIndexCount);
					if (!kinds.segmentsValid || kinds.segmentSignature != signature) {
						kinds.segmentSignature = signature;
						kinds.segmentsValid = true;
						kinds.segmentsRejected = false;
						kinds.segmentRanges.clear();
						uint32_t covered = 0;
						for (uint32_t i = 0; i < segments.numSegments; ++i) {
							const auto& segment = segments.segmentData[i];
							const auto triangles = i == 0 ? segment.numTris : segment.unkTriCount;
							const auto flags = i == 0 ? segment.flags : segment.unkFlags;
							if (!flags || !triangles)
								continue;
							if (uint64_t(segment.index) + uint64_t(triangles) * 3 > sourceIndexCount) {
								kinds.segmentsRejected = true;
								break;
							}
							kinds.segmentRanges.emplace_back(segment.index, triangles * 3);
							covered += triangles * 3;
						}
						// Aggregate and child ranges can overlap. Their union is the
						// visible index set; submitting duplicates creates coplanar hits.
						if (!kinds.segmentsRejected && covered) {
							std::ranges::sort(kinds.segmentRanges);
							size_t write = 0;
							for (size_t read = 0; read < kinds.segmentRanges.size(); ++read) {
								const auto& range = kinds.segmentRanges[read];
								if (write && range.first <= kinds.segmentRanges[write - 1].first + kinds.segmentRanges[write - 1].second) {
									auto& previous = kinds.segmentRanges[write - 1];
									previous.second = std::max(previous.first + previous.second, range.first + range.second) - previous.first;
								} else {
									kinds.segmentRanges[write++] = range;
								}
							}
							kinds.segmentRanges.resize(write);
						}
						kinds.segmentIndexCount = 0;
						for (const auto& range : kinds.segmentRanges)
							kinds.segmentIndexCount += range.second;
					}
					if (kinds.segmentsRejected)
						return;
					if (!kinds.segmentIndexCount) {
						RetireInstanceIfStillEmpty(geometry, "distant tree: atlas not ready");
						return;
					}
					indexRangesSource = &kinds.segmentRanges;
					indexCount = kinds.segmentIndexCount;
				}
			}
		}
		const auto& indexRanges = *indexRangesSource;
		const auto feature = !water && data.shaderProperty->material ? data.shaderProperty->material->GetFeature() : RE::BSShaderMaterial::Feature::kDefault;
		std::array<ID3D11ShaderResourceView*, 5> waterNormals{};
		std::array<uintptr_t, 10> waterTextures{};
		std::array<float, 4> waterTransport{};
		std::array<float, 26> waterParameters{};
		const bool waterObjectUV = water && (waterProperty->waterFlags.underlying() & 0x100) && !(waterProperty->waterFlags.underlying() & 0x8000);
		if (water) {
			const auto* source = static_cast<const RE::BSWaterShaderMaterial*>(waterProperty->material);
			const auto native = RemixNativeRender::ReadWaterGlobals();
			const bool flow = (waterProperty->waterFlags.underlying() & 0x8000) != 0;
			const bool blendNormals = (waterProperty->waterFlags.underlying() & 0x10000) != 0;
			const RE::NiSourceTexture* normals[]{ source->normalTexture1 ? source->normalTexture1.get() : native.defaultNormal,
				source->normalTexture2 ? source->normalTexture2.get() : native.defaultNormal,
				source->normalTexture3 ? source->normalTexture3.get() : native.defaultNormal,
				flow ? native.flowAtlas : nullptr, flow ? (source->normalTexture4 ? source->normalTexture4.get() : native.defaultNormal) : nullptr };
			for (uint32_t i = 0; i < 5; ++i) {
				waterNormals[i] = normals[i] && normals[i]->rendererTexture ? reinterpret_cast<ID3D11ShaderResourceView*>(normals[i]->rendererTexture->resourceView) : nullptr;
				waterTextures[i * 2] = reinterpret_cast<uintptr_t>(normals[i]);
				waterTextures[i * 2 + 1] = reinterpret_cast<uintptr_t>(waterNormals[i]);
				if (i < 3) {
					waterParameters[i] = source->uvScaleA[i];
					waterParameters[i + 3] = source->amplitudeA[i];
				}
			}
			waterParameters[6] = source->normalScroll1.x; waterParameters[7] = source->normalScroll1.y;
			waterParameters[8] = source->normalScroll2.x; waterParameters[9] = source->normalScroll2.y;
			waterParameters[10] = source->normalScroll3.x; waterParameters[11] = source->normalScroll3.y;
			// Native LOD and flow without BLEND_NORMALS only use the base layer.
			if ((waterProperty->waterFlags.underlying() & 2) || (flow && !blendNormals)) {
				waterParameters[4] = waterParameters[5] = 0;
			}
			if (flow && native.gridDimension > 0 && native.gridDimension < 1024 && waterNormals[3] && waterNormals[4]) {
				waterParameters[18] = static_cast<float>(waterProperty->flowX);
				waterParameters[19] = static_cast<float>(static_cast<int32_t>(native.gridDimension) - waterProperty->flowY - 1);
				waterParameters[20] = static_cast<float>(waterProperty->cellX);
				waterParameters[21] = -static_cast<float>(waterProperty->cellY);
				waterParameters[22] = static_cast<float>(native.gridDimension) * (blendNormals ? -1.0f : 1.0f);
				waterParameters[23] = native.time;
				waterParameters[24] = source->noiseFalloff;
				// GetRenderPasses (0x152c4a0) maps property bit 0 to WADING
				// technique bit 0x40. SetupGeometry (0x154db70) then uses the
				// water-manager globals, not this moving mesh's cell fields.
				if (waterProperty->waterFlags.underlying() & 1) {
					const float dimension = static_cast<float>(native.gridDimension);
					waterParameters[18] = (native.wadingCell[0] - 0.05f) * dimension + native.wadingCell[2];
					waterParameters[19] = (native.wadingCell[1] - 0.05f) * dimension - native.wadingCell[3] + dimension;
					waterParameters[20] = native.wadingOrigin[0] - 0.25f;
					waterParameters[21] = native.wadingOrigin[1] - 0.25f;
					waterParameters[25] = 1;
				}
			}
			waterTransport = { source->deepWaterColor.red, source->deepWaterColor.green, source->deepWaterColor.blue,
				std::clamp(source->underwaterFogDistFar, 1.0f, 65000.0f) };
		}
		const bool lodLandscape = feature == RE::BSShaderMaterial::Feature::kLODLand || feature == RE::BSShaderMaterial::Feature::kLODLandNoise;
		const bool nativeLandscape = feature == RE::BSShaderMaterial::Feature::kMultiTexLandLODBlend;
		std::array<ID3D11ShaderResourceView*, 6> landscapeAlbedos{}, landscapeNormals{};
		std::array<uintptr_t, 24> landscapeTextures{};
		std::array<float, 4> landscapeUv{};
		if (nativeLandscape) {
			const auto* land = static_cast<const RE::BSLightingShaderMaterialLandscape*>(data.shaderProperty->material);
			for (uint32_t i = 0; i < 6; ++i) {
				for (uint32_t kind = 0; kind < 2; ++kind) {
					const auto* texture = kind == 0 ? (i ? land->landscapeDiffuseTexture[i - 1].get() : land->diffuseTexture.get()) :
						(i ? land->landscapeNormalTexture[i - 1].get() : land->normalTexture.get());
					auto* view = texture && texture->rendererTexture ? reinterpret_cast<ID3D11ShaderResourceView*>(texture->rendererTexture->resourceView) : nullptr;
					(kind == 0 ? landscapeAlbedos[i] : landscapeNormals[i]) = view;
					landscapeTextures[(kind * 6 + i) * 2] = reinterpret_cast<uintptr_t>(texture);
					landscapeTextures[(kind * 6 + i) * 2 + 1] = reinterpret_cast<uintptr_t>(view);
				}
			}
			landscapeUv = { land->texCoordScale[0].x, land->texCoordScale[0].y, land->texCoordOffset[0].x, land->texCoordOffset[0].y };
		}
		std::array<float, 4> lodRange{};
		if (lodLandscape) {
			const auto& range = RE::BSShaderManager::State::GetSingleton().loadedRange;
			lodRange = { range.red, range.green, range.blue, range.alpha };
		}
		auto* vb = reinterpret_cast<ID3D11Buffer*>(rendererData->vertexBuffer);
		auto* ib = reinterpret_cast<ID3D11Buffer*>(rendererData->indexBuffer);
		if (!vb || !ib)
			return;
		uint64_t descriptor;
		std::memcpy(&descriptor, &rendererData->vertexDesc, sizeof(descriptor));
		std::vector<float> dynamicPositions;
		if (auto* dynamic = geometry->AsDynamicTriShape()) {
			auto& dynamicData = dynamic->GetDynamicTrishapeRuntimeData();
			RE::BSSpinLockGuard dynamicLock(dynamicData.lock);
			if (!dynamicData.dynamicData || dynamicData.dataSize < size_t(vertexCount) * 4 * sizeof(float))
				return;
			dynamicPositions.resize(size_t(vertexCount) * 4);
			std::memcpy(dynamicPositions.data(), dynamicData.dynamicData, dynamicPositions.size() * sizeof(float));
		}
		const bool modelSpaceNormals = !grass && !water && !effect && data.shaderProperty->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kModelSpaceNormals);
		// Missing authored normals retain the geometric fallback.
		const bool useFaceNormals = grass && !rendererData->vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_NORMAL);
		const bool hairTint = feature == RE::BSShaderMaterial::Feature::kHairTint;
		const bool skinDiffusion = feature == RE::BSShaderMaterial::Feature::kFaceGen || feature == RE::BSShaderMaterial::Feature::kFaceGenRGBTint;
		auto* treeAtlas = distantTree ? RemixNativeRender::ReadDistantTreeAtlas() : nullptr;
		auto* treeAtlasView = treeAtlas && treeAtlas->rendererTexture ? treeAtlas->rendererTexture->resourceView : nullptr;
		const std::array<uintptr_t, 2> treeAtlasIdentity{ reinterpret_cast<uintptr_t>(treeAtlas), reinterpret_cast<uintptr_t>(treeAtlasView) };
		// Wait for the real atlas instead of caching an opaque fallback billboard.
		// Skipping the frame is enough: an instance already registered keeps its
		// registration and is replayed as it was, and one that has not been
		// registered yet has nothing to retire. Retiring here destroyed every
		// distant tree in the scene on any frame the atlas was briefly absent,
		// and they all came back the next one -- trees flickering in and out.
		if (distantTree && !treeAtlasView) {
			return;
		}
		std::array<float, 3> tint{ 1, 1, 1 };
		const auto* colorMaterial = distantTree || water || effect || grass ? nullptr : static_cast<const RE::BSLightingShaderMaterialBase*>(data.shaderProperty->material);
		const bool softLighting = data.shaderProperty->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kSoftLighting);
		const bool backLighting = data.shaderProperty->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kBackLighting);
		const bool skinMaterial = skinDiffusion || modelSpaceNormals;
		const bool thinFoliage = RemixFoliageMaterial::IsThin(grass, colorMaterial && !hairTint && !skinMaterial && !nativeLandscape && !lodLandscape,
			data.shaderProperty->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kTreeAnim),
			softLighting, backLighting,
			data.shaderProperty->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kTwoSided),
			data.alphaProperty && data.alphaProperty->GetAlphaTesting());
		uint32_t foliageFlags = 0;
		std::array<float, 6> foliageParameters{};
		std::array<ID3D11ShaderResourceView*, 2> foliageViews{};
		std::array<uintptr_t, 2> foliageTextures{};
		if (thinFoliage && (grass || softLighting || backLighting)) {
			const auto& settings = globals::features::grassLighting.settings;
			const auto linear = globals::features::linearLighting.GetCommonBufferData();
			foliageFlags = grass ? 1u : (softLighting ? 2u : 0u) | (backLighting ? 4u : 0u);
			if (grass && globals::features::lodBlending.settings.DisableTerrainVertexColors) foliageFlags |= 8u;
			if (grass && settings.OverrideComplexGrassSettings) foliageFlags |= 16u;
			if (grass && data.shaderProperty->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kEffectLighting)) foliageFlags |= 64u;
			// CS without Linear Lighting evaluates these colours in Skyrim gamma space.
			if (!linear.enableLinearLighting) foliageFlags |= 32u;
			foliageParameters = { colorMaterial ? colorMaterial->subSurfaceLightRolloff : 0.0f,
				settings.BasicGrassBrightness, settings.ComplexGrassThreshold,
				linear.enableLinearLighting ? linear.colorGamma : 1.0f,
				linear.enableLinearLighting ? linear.vanillaDiffuseColorMult : 1.0f, settings.SubsurfaceScatteringAmount };
			auto viewOf = [](const RE::NiPointer<RE::NiSourceTexture>& texture) {
				return texture && texture->rendererTexture ? reinterpret_cast<ID3D11ShaderResourceView*>(texture->rendererTexture->resourceView) : nullptr;
			};
			if (colorMaterial) {
				foliageViews = { softLighting ? viewOf(colorMaterial->rimSoftLightingTexture) : nullptr,
					backLighting ? viewOf(colorMaterial->specularBackLightingTexture) : nullptr };
				for (uint32_t i = 0; i < foliageViews.size(); ++i) foliageTextures[i] = reinterpret_cast<uintptr_t>(foliageViews[i]);
			}
		}
		const bool lightingVertexColor = colorMaterial && !hairTint && !nativeLandscape && !lodLandscape &&
			data.shaderProperty->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kVertexColors) &&
			rendererData->vertexDesc.HasFlag(RE::BSGraphics::Vertex::VF_COLORS);
		const bool lightingVertexAlpha = lightingVertexColor &&
			!data.shaderProperty->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kTreeAnim, RE::BSShaderProperty::EShaderPropertyFlag::kLODObjects) &&
			colorMaterial->GetFeature() != RE::BSShaderMaterial::Feature::kTreeAnim &&
			colorMaterial->GetFeature() != RE::BSShaderMaterial::Feature::kLODObjectsHD;
		const float vertexAlphaScale = lightingVertexColor ? colorMaterial->materialAlpha : 1.0f;
		if (hairTint) {
			const auto& color = static_cast<const RE::BSLightingShaderMaterialHairTint*>(data.shaderProperty->material)->tintColor;
			tint = { color.red, color.green, color.blue };
		}
		auto found = meshes.find(geometry);
		remixapi_MaterialHandle retainedMaterial = nullptr;
		bool retainedSkinDiffusion = false;
		// The game recycles terrain and object LOD block nodes: rather than
		// allocate a node per cell, it moves an existing one to the new cell and
		// rewrites its vertex buffer in place. The buffer pointer, the counts and
		// the descriptor all stay the same, so nothing below notices, and the
		// block keeps the previous cell's geometry at the new cell's position --
		// a piece of landscape hanging in the air for the frame before anything
		// else catches up. Measured jumps of 98,304 units, exactly 24 cells.
		//
		// A placement that moves by more than a cell is not an object moving; it
		// is a different object in the same node. Rebuild it.
		const bool recycledIntoAnotherCell = found != meshes.end() && [&] {
			const auto& was = found->second.lastWorld.translate;
			const auto& now = geometry->world.translate;
			const float dx = now.x - was.x, dy = now.y - was.y, dz = now.z - was.z;
			return (dx * dx + dy * dy + dz * dz) > (4096.0f * 4096.0f);
		}();
		const bool staticMeshChanged = found != meshes.end() && (found->second.vertices.Get() != vb || found->second.indices.Get() != ib ||
			found->second.descriptor != descriptor || found->second.vertexCount != vertexCount ||
			found->second.indexCount != indexCount || found->second.sourceIndexCount != sourceIndexCount || found->second.indexRanges != indexRanges ||
			found->second.lodLandscape != lodLandscape || (lodLandscape && (found->second.lodRange != lodRange || found->second.lodWorld != geometry->world)) ||
			found->second.nativeLandscape != nativeLandscape || found->second.landscapeTextures != landscapeTextures || found->second.landscapeUv != landscapeUv ||
			found->second.boneCount != boneCount || found->second.modelSpaceNormals != modelSpaceNormals ||
			found->second.useFaceNormals != useFaceNormals ||
			found->second.thinFoliage != thinFoliage ||
			found->second.foliageFlags != foliageFlags || found->second.foliageParameters != foliageParameters || found->second.foliageTextures != foliageTextures ||
			found->second.treeAtlas != treeAtlasIdentity ||
			found->second.water != water || found->second.waterObjectUV != waterObjectUV || found->second.waterTextures != waterTextures ||
			found->second.effect != effect || found->second.effectTextures != effectTextures ||
			found->second.waterTransport != waterTransport || (water && found->second.lodWorld != geometry->world) ||
			found->second.hairTint != hairTint || found->second.tint != tint ||
			found->second.skinDiffusion != skinDiffusion ||
			found->second.lightingVertexColor != lightingVertexColor || found->second.lightingVertexAlpha != lightingVertexAlpha ||
			found->second.vertexAlphaScale != vertexAlphaScale ||
			found->second.partitionVisibility != partitionVisibility ||
			recycledIntoAnotherCell);
		if (found != meshes.end() && !staticMeshChanged && updateMesh && !dynamicPositions.empty() &&
			!grass && !water && !effect && !nativeLandscape && found->second.dynamicPositions != dynamicPositions) {
			auto& mesh = found->second;
			auto previousPositions = std::move(mesh.dynamicPositions);
			mesh.dynamicPositions = dynamicPositions;
			const auto* morphMaterial = static_cast<const RE::BSLightingShaderMaterialBase*>(data.shaderProperty->material);
			if (!Upload(mesh, rendererData->vertexDesc, morphMaterial, partition, rendererData, true)) {
				mesh.dynamicPositions = std::move(previousPositions);
			} else {
				static uint32_t morphUpdates = 0;
				if (++morphUpdates <= 12)
					logger::info("[RemixScene.morph] updated '{}' vertices={} retained mesh={}", geometry->name.c_str(), mesh.vertexCount, reinterpret_cast<uintptr_t>(mesh.handle));
			}
		}
		if (found != meshes.end() && (staticMeshChanged || found->second.dynamicPositions != dynamicPositions)) {
			// Keep the previous valid representation until its replacement can
			// be scheduled. Upload-budget exhaustion is not object disappearance.
			// The test has to match the one guarding creation below exactly: this
			// block destroys the old mesh, so returning between the two would
			// leave the object with none.
			if (staticMeshChanged && createdThisFrame >= 24)
				return;
			// A facial morph changes positions, not its textures. Keep our owned
			// Remix material; never pin native texture wrappers to cache the bake.
			//
			// Water's world transform updates every frame, which forces a mesh
			// rebuild but does not change the material: the transform-derived UV
			// basis reaches Remix through the per-frame water update instead.
			// Destroying and recreating the material here left quads without one
			// until the creation reached the command stream, which showed as
			// water that randomly went missing.
			const bool waterMaterialUnchanged = water &&
				found->second.water == water && found->second.waterTextures == waterTextures &&
				found->second.waterObjectUV == waterObjectUV && found->second.waterTransport == waterTransport;
			if (!staticMeshChanged || waterMaterialUnchanged) {
				retainedMaterial = std::exchange(found->second.materialHandle, nullptr);
				retainedSkinDiffusion = found->second.skinDiffusionImported;
			}
			++invalidatedThisFrame;
			// The instance is NOT retired here. Rebuilding an object's mesh is not
			// the object leaving: StoreInstance below keeps the retained handle
			// and the submission updates that registration in place. Destroying
			// it instead gave the replacement a brand new instance with no
			// temporal history, which the denoiser shows as a one frame flash --
			// measured on NPC heads and eyes, whose facial morphs rebuild their
			// mesh constantly (MaleEyesHumanLightBlue retired 20 times and
			// MaleHeadNord 14 times in a single minute standing still).
			api->DestroyMesh(found->second.handle);
			if (found->second.materialHandle)
				api->DestroyMaterial(found->second.materialHandle);
			meshes.erase(found);
			found = meshes.end();
		}
		if (found == meshes.end()) {
			if (!retainedMaterial && createdThisFrame >= 24)
				return;
			++createdThisFrame;
			Mesh mesh;
			mesh.materialHandle = retainedMaterial;
			mesh.skinDiffusionImported = retainedSkinDiffusion;
			mesh.vertices = vb;
			mesh.indices = ib;
			mesh.descriptor = descriptor;
			mesh.vertexCount = vertexCount;
			mesh.indexCount = indexCount;
			mesh.sourceIndexCount = sourceIndexCount;
			mesh.indexRanges = indexRanges;
			mesh.lodLandscape = lodLandscape;
			mesh.nativeLandscape = nativeLandscape;
			mesh.water = water;
			mesh.effect = effect;
			mesh.effectParameters = effectParameters;
			mesh.effectTextures = effectTextures;
			mesh.waterObjectUV = waterObjectUV;
			mesh.waterTextures = waterTextures;
			mesh.waterTransport = waterTransport;
			mesh.waterParameters = waterParameters;
			mesh.landscapeTextures = landscapeTextures;
			mesh.landscapeUv = landscapeUv;
			mesh.lodWorld = geometry->world;
			mesh.lastWorld = geometry->world;
			mesh.lodRange = lodRange;
			mesh.boneCount = boneCount;
			mesh.modelSpaceNormals = modelSpaceNormals;
			mesh.useFaceNormals = useFaceNormals;
			mesh.grass = grass;
			mesh.thinFoliage = thinFoliage;
			mesh.foliageFlags = foliageFlags;
			mesh.foliageParameters = foliageParameters;
			mesh.foliageTextures = foliageTextures;
			mesh.treeAtlas = treeAtlasIdentity;
			mesh.hairTint = hairTint;
			mesh.skinDiffusion = skinDiffusion;
			if (lightingVertexColor && !grass && !effect && !water && !distantTree) {
				// Include solid bark, which need not carry TREE_ANIM. Never infer
				// baked lighting from a texture filename or from all vertex colours.
				uint32_t depth = 0;
				for (auto* node = geometry->parent; node && depth++ < 32; node = node->parent) {
					if (netimmerse_cast<RE::BSTreeNode*>(node)) {
						mesh.treeVertexColor = true;
						break;
					}
				}
			}
			mesh.lightingVertexColor = lightingVertexColor;
			mesh.lightingVertexAlpha = lightingVertexAlpha;
			mesh.vertexAlphaScale = vertexAlphaScale;
			mesh.tint = tint;
			mesh.dynamicPositions = std::move(dynamicPositions);
			mesh.partitionVisibility = std::move(partitionVisibility);
			const auto* sourceMaterial = distantTree || water || effect ? nullptr : static_cast<const RE::BSLightingShaderMaterialBase*>(data.shaderProperty->material);
			auto* diffuse = distantTree ? treeAtlas : water ? nullptr : data.shaderProperty->GetBaseTexture();
			if (water && !mesh.materialHandle) {
				remixapi_MaterialInfoTranslucentEXT translucent{};
				translucent.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_TRANSLUCENT_EXT;
				translucent.refractiveIndex = 1.333f;
				translucent.transmittanceColor = { waterTransport[0], waterTransport[1], waterTransport[2] };
				translucent.transmittanceMeasurementDistance = waterTransport[3];
				remixapi_MaterialInfo info{};
				info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
				info.pNext = &translucent;
				info.hash = ++nextHash;
				info.spriteSheetRow = info.spriteSheetCol = 1;
				const auto result = importWater(&info, waterNormals.data(), waterParameters.data(), &mesh.materialHandle);
				logger::info("[RemixScene.waterMaterial] '{}' import={} flags={:X} layers={} distance={} objectUV={}", geometry->name.c_str(),
					result == REMIXAPI_ERROR_CODE_SUCCESS, waterProperty->waterFlags.underlying(), std::ranges::count_if(waterNormals, [](auto* v) { return v != nullptr; }), waterTransport[3], waterObjectUV);
				if (result != REMIXAPI_ERROR_CODE_SUCCESS) return;
			}
			if (effect && !mesh.materialHandle) {
				remixapi_MaterialInfoOpaqueEXT opaque{};
				opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
				opaque.albedoConstant = { 1, 1, 1 }; opaque.opacityConstant = 1; opaque.roughnessConstant = 1;
				opaque.useDrawCallAlphaState = true; opaque.alphaTestType = 7;
				remixapi_MaterialInfo info{}; info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
				info.pNext = &opaque; info.hash = ++nextHash; info.spriteSheetRow = info.spriteSheetCol = 1;
				const auto result = importEffect(&info, effectViews[0], effectViews[1], static_cast<uint32_t>(effectTextures[4]), effectParameters.data(), &mesh.materialHandle);
				logger::info("[RemixScene.effectMaterial] '{}' import={} source='{}' palette='{}' flags={:X}", geometry->name.c_str(),
					result == REMIXAPI_ERROR_CODE_SUCCESS, effectMaterial ? effectMaterial->sourceTexturePath.c_str() : "native-refraction",
					effectMaterial ? effectMaterial->greyscaleTexturePath.c_str() : "", static_cast<uint32_t>(effectParameters[19]));
				if (result != REMIXAPI_ERROR_CODE_SUCCESS) return;
			}
			if (!mesh.materialHandle && diffuse && diffuse->rendererTexture && importMaterial) {
				// The import retains the Vulkan image view, not the engine's SRV wrapper.
				// Do not cache an engine-owned COM view across Skyrim texture eviction.
				auto* albedo = reinterpret_cast<ID3D11ShaderResourceView*>(diffuse->rendererTexture->resourceView);
				if (albedo) {
					auto composed = sourceMaterial ? RemixMaterial::BakeCharacterAlbedo(sourceMaterial, albedo) : ComPtr<ID3D11ShaderResourceView>{};
					if (composed)
						albedo = composed.Get();
					remixapi_MaterialInfoOpaqueEXT opaque{};
					opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
					opaque.albedoConstant = { 1, 1, 1 };
					opaque.opacityConstant = sourceMaterial && !grass ? sourceMaterial->materialAlpha : 1;
					opaque.roughnessConstant = 0.8f;
					opaque.alphaTestType = 7;
					opaque.useDrawCallAlphaState = true;
					remixapi_MaterialInfoOpaqueSubsurfaceEXT subsurface{};
					const bool useSkinDiffusion = skinDiffusion && importSkinMaterial && composed;
					if (useSkinDiffusion) {
						subsurface.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_SUBSURFACE_EXT;
						subsurface.subsurfaceDiffusionProfile = true;
						// Initial Remix diffusion defaults; native _sk is not a radius map.
						subsurface.subsurfaceRadius = { 0.5f, 0.5f, 0.5f };
						subsurface.subsurfaceRadiusScale = 1.0f;
						// API disk bounds are meters; retain a local 16-game-unit search extent.
						subsurface.subsurfaceMaxSampleRadius = 16.0f * RE::bhkWorld::GetWorldScale();
						subsurface.subsurfaceTransmittanceColor = { 0.5f, 0.5f, 0.5f };
						opaque.pNext = &subsurface;
					} else if (thinFoliage && (importFoliage || importThinMaterial)) {
						subsurface.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_SUBSURFACE_EXT;
						// Optical thickness is not authored. Native scattering colour is
						// derived separately by the foliage importer, once per surface hit.
						subsurface.subsurfaceTransmittanceColor = { 0.5f, 0.5f, 0.5f };
						subsurface.subsurfaceMeasurementDistance = 1.0f;
						subsurface.subsurfaceSingleScatteringAlbedo = { 0.5f, 0.5f, 0.5f };
						opaque.pNext = &subsurface;
					}
					if (grass || distantTree) {
						opaque.alphaTestType = 4;
						opaque.alphaReferenceValue = 128;
					}
					if (data.alphaProperty && data.alphaProperty->GetAlphaTesting()) {
						constexpr int testFunctions[] = { 7, 1, 2, 3, 4, 5, 6, 0 };
						opaque.alphaTestType = testFunctions[(data.alphaProperty->alphaFlags >> 10) & 7];
						opaque.alphaReferenceValue = data.alphaProperty->alphaThreshold;
					}
					remixapi_MaterialInfo info{};
					info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
					info.pNext = &opaque;
					info.hash = ++nextHash;
					info.spriteSheetRow = info.spriteSheetCol = 1;
					ID3D11ShaderResourceView* normal = nullptr;
					// MSN meshes carry a separate animated model-to-world basis.
					if (sourceMaterial && !grass &&
						sourceMaterial->normalTexture && sourceMaterial->normalTexture->rendererTexture)
						normal = reinterpret_cast<ID3D11ShaderResourceView*>(sourceMaterial->normalTexture->rendererTexture->resourceView);
					const auto addressMode = grass ? static_cast<uint32_t>(static_cast<RE::BSGrassShaderProperty*>(data.shaderProperty.get())->clampMode) :
						sourceMaterial ? static_cast<uint32_t>(sourceMaterial->textureClampMode) : 0;
					if (grass)
						logger::info("[RemixScene.grass] '{}' native sampler address mode={}", geometry->name.c_str(), addressMode);
					const auto result = nativeLandscape && importLandscape ?
						importLandscape(&info, landscapeAlbedos.data(), landscapeNormals.data(), addressMode, &mesh.materialHandle) :
						useSkinDiffusion ? importSkinMaterial(&info, albedo, normal, albedo, addressMode, &mesh.materialHandle) :
						foliageFlags && importFoliage ? importFoliage(&info, albedo, normal, foliageViews[0], foliageViews[1], addressMode,
							foliageFlags, foliageParameters.data(), &mesh.materialHandle) :
						thinFoliage && importThinMaterial ? importThinMaterial(&info, albedo, normal, albedo, addressMode, &mesh.materialHandle) :
						importMaterial(&info, albedo, normal, addressMode, &mesh.materialHandle);
					if (skinDiffusion)
						logger::info("[RemixScene.skin] '{}' diffusion={} composed={} import={}", geometry->name.c_str(),
							useSkinDiffusion, composed.Get() != nullptr, result == REMIXAPI_ERROR_CODE_SUCCESS);
					mesh.skinDiffusionImported = useSkinDiffusion && result == REMIXAPI_ERROR_CODE_SUCCESS;
					if (thinFoliage)
						logger::info("[RemixScene.foliage] '{}' native={} flags={} soft={} back={} rolloff={} gamma={} import={}", geometry->name.c_str(),
							foliageFlags && importFoliage != nullptr, foliageFlags, foliageViews[0] != nullptr, foliageViews[1] != nullptr,
							foliageParameters[0], foliageParameters[3], result == REMIXAPI_ERROR_CODE_SUCCESS);
					if (nativeLandscape)
						logger::info("[RemixScene.landMaterial] six-layer import={} diffuse={} normal={}", importLandscape && result == REMIXAPI_ERROR_CODE_SUCCESS,
							std::ranges::count_if(landscapeAlbedos, [](auto* v) { return v != nullptr; }), std::ranges::count_if(landscapeNormals, [](auto* v) { return v != nullptr; }));
					if (distantTree)
						logger::info("[RemixScene.treeMaterial] atlas='{}' import={} alphaTest={} threshold={} descriptor={:016X}",
							diffuse->name.c_str(), result == REMIXAPI_ERROR_CODE_SUCCESS, opaque.alphaTestType, opaque.alphaReferenceValue, descriptor);
					if (result != REMIXAPI_ERROR_CODE_SUCCESS)
						logger::warn("[RemixScene] diffuse import failed: {}", static_cast<int>(result));
				}
			}
			// RunGrass passes mesh UVs through unchanged; lighting material UV
			// transforms are not part of the native grass vertex shader.
			if (!Upload(mesh, rendererData->vertexDesc, grass ? nullptr : sourceMaterial, partition, rendererData)) {
				++failedUploadsThisFrame;
				static std::unordered_set<RE::BSGeometry*> reportedFailures;
				if (reportedFailures.size() < 64 && reportedFailures.insert(geometry).second)
					logger::warn("[RemixScene.upload] failed '{}' vertices={} indices={} bones={} descriptor={:016X} partitions={}",
						geometry->name.c_str(), vertexCount, indexCount, boneCount, descriptor, partition ? partition->numPartitions : 0);
				if (mesh.materialHandle)
					api->DestroyMaterial(mesh.materialHandle);
				return;
			}
			found = meshes.emplace(geometry, std::move(mesh)).first;
			if (modelSpaceNormals)
				logger::info("[RemixScene.MSN] '{}' vertices={} bones={} dynamic={}", geometry->name.c_str(), vertexCount, boneCount, !found->second.dynamicPositions.empty());
			if (!found->second.indexRanges.empty())
				logger::info("[RemixScene.segments] '{}' {}/{} indices in {} visible ranges", geometry->name.c_str(), indexCount, sourceIndexCount, found->second.indexRanges.size());
			if (grass) {
				logger::info("[RemixScene.grass] '{}' uses vanilla instance-up lighting surrogate (no mesh normals)", geometry->name.c_str());
				logger::info("[RemixScene.grass] mesh '{}' vertices={} indices={} descriptor={:016X} texture='{}'", geometry->name.c_str(), vertexCount, indexCount, descriptor, diffuse ? diffuse->name.c_str() : "");
				const auto& uploaded = found->second;
				logger::info("[RemixScene.grass] bounds=({}, {}, {})..({}, {}, {})", uploaded.boundsMin.x, uploaded.boundsMin.y, uploaded.boundsMin.z, uploaded.boundsMax.x, uploaded.boundsMax.y, uploaded.boundsMax.z);
			}
		}
		// Remember where this mesh was submitted from, so a node the game recycles
		// into a different cell is compared against last frame rather than
		// against wherever it was first created.
		found->second.lastWorld = geometry->world;
		if (grass && grassScale && found->second.grassScale != *grassScale) {
			found->second.grassScale = *grassScale;
			logger::info("[RemixScene.grass] '{}' native ScaleMask=({}, {}, {})", geometry->name.c_str(), grassScale->x, grassScale->y, grassScale->z);
		}
		if (water) {
			std::copy_n(found->second.waterParameters.begin() + 12, 6, waterParameters.begin() + 12);
			if (!found->second.waterFlowUVValid) waterParameters[22] = 0;
		}
		if (water && found->second.waterParameters != waterParameters) {
			if (updateWater(found->second.materialHandle, waterParameters.data()) == REMIXAPI_ERROR_CODE_SUCCESS)
				found->second.waterParameters = waterParameters;
		}
		Instance instance{ found->second.handle, geometry->world, {}, {} };
		// Rigid player equipment shares its skinned skeleton's sampled pose,
		// in both first and third person. Unmapped attachments keep native world.
		if (!boneCount && (viewModelGeometry.contains(geometry) || playerBody.contains(geometry))) {
			if (const auto attachment = attachmentAuditTransforms.find(geometry); attachment != attachmentAuditTransforms.end())
				instance.world = attachment->second;
		}
		auto& blend = instance.blend;
		blend.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT;
		blend.textureColorArg1Source = 1;  // Texture
		blend.textureColorArg2Source = 2;  // VertexColor0
		blend.textureColorOperation = (hairTint || grass || nativeLandscape || lightingVertexColor) ? 3 : 1;  // Modulate / SelectArg1
		blend.isTextureFactorBlend = grass;
		blend.isVertexColorBakedLighting = treeVertexBakedLighting && found->second.treeVertexColor;
		blend.textureAlphaArg1Source = 1;
		blend.textureAlphaArg2Source = (hairTint || lightingVertexColor) ? 2 : 3;  // Composed vertex alpha / material alpha
		blend.textureAlphaOperation = 3;
		if (effect && found->second.effectParameters != effectParameters) {
			if (updateEffect(found->second.materialHandle, effectParameters.data()) == REMIXAPI_ERROR_CODE_SUCCESS)
				found->second.effectParameters = effectParameters;
		}
		const auto* liveMaterial = distantTree || water || effect ? nullptr : static_cast<const RE::BSLightingShaderMaterialBase*>(data.shaderProperty->material);
		const float materialAlpha = liveMaterial && !grass ? liveMaterial->materialAlpha : 1;
		blend.tFactor = (static_cast<uint32_t>(std::lround(std::clamp(materialAlpha, 0.0f, 1.0f) * 255)) << 24) | 0xffffff;
		blend.writeMask = 15;
		blend.srcColorBlendFactor = blend.srcAlphaBlendFactor = 1;  // VK_BLEND_FACTOR_ONE
		blend.alphaTestCompareOp = 7;  // VK_COMPARE_OP_ALWAYS
		if (grass || distantTree) {
			blend.alphaTestEnabled = true;
			blend.alphaTestCompareOp = 4;
			blend.alphaTestReferenceValue = 128;
		}
		if (data.alphaProperty && !water && !refraction) {
			if (data.alphaProperty->GetAlphaTesting()) {
				constexpr uint32_t compares[] = { 7, 1, 2, 3, 4, 5, 6, 0 };
				blend.alphaTestEnabled = true;
				blend.alphaTestCompareOp = compares[(data.alphaProperty->alphaFlags >> 10) & 7];
				blend.alphaTestReferenceValue = data.alphaProperty->alphaThreshold;
			}
			// Grass NIFs commonly retain the blend bit (0x12ED), but the
			// native grass pass explicitly disables blending. Honor that pass:
			// diffuse alpha is a cutout, not a translucent billboard/OIT layer.
			// Keep the NIF alpha-test threshold above, as the native pass does.
			if (!grass && data.alphaProperty->GetAlphaBlending()) {
				constexpr uint32_t factors[] = { 1, 0, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
				const auto src = static_cast<uint32_t>(data.alphaProperty->GetSrcBlendMode());
				const auto dst = static_cast<uint32_t>(data.alphaProperty->GetDestBlendMode());
				if (src < std::size(factors) && dst < std::size(factors)) {
					blend.alphaBlendEnabled = true;
					blend.srcColorBlendFactor = blend.srcAlphaBlendFactor = factors[src];
					blend.dstColorBlendFactor = blend.dstAlphaBlendFactor = factors[dst];
				}
			}
		}
		if (refraction) {
			blend.alphaBlendEnabled = true;
			blend.srcColorBlendFactor = blend.srcAlphaBlendFactor = 6;
			blend.dstColorBlendFactor = blend.dstAlphaBlendFactor = 7;
		}
		if (auto* instanced = kinds.multiStream) {
			if (!CaptureInstances(instanced, found->second, instance, grass, distantTree))
				return;
			if (instance.placements->transforms.empty()) {
				RetireInstance(geometry, "instanced: no placements");
				return;
			}
		}
		if (boneCount) {
			if (!std::isfinite(geometry->world.scale) || std::abs(geometry->world.scale) < 1e-8f)
				return;
			const auto inverse = instance.world.Invert();
			instance.bones.resize(boneCount);
			for (uint32_t bone = 0; bone < boneCount; ++bone) {
				if (!skin->boneWorldTransforms[bone])
					return;
				const auto sampled = boneSnapshot.find(skin->boneWorldTransforms[bone]);
				if (sampled == boneSnapshot.end())
					return;
				const auto transform = inverse * sampled->second * skin->skinData->GetBoneDataSkinToBone(bone);
				auto& matrix = instance.bones[bone].matrix;
				for (uint32_t row = 0; row < 3; ++row)
					for (uint32_t col = 0; col < 3; ++col)
						matrix[row][col] = transform.rotate.entry[row][col] * transform.scale;
				matrix[0][3] = transform.translate.x;
				matrix[1][3] = transform.translate.y;
				matrix[2][3] = transform.translate.z;
			}
		}
		// Arm the identity probe only for plain opaque static shapes, whose every
		// per-frame capture input the probe actually covers. Anything animated,
		// skinned, instanced, layered or category-specific keeps the full path.
		++probeReachedArmThisFrame;
		{
			// Attribute ineligibility so the remaining per-frame full captures can
			// be reduced by category rather than guessed at.
			const bool reasons[]{ !shape, water, effect, grass, distantTree, hairTint, boneCount != 0,
				lodLandscape, nativeLandscape, kinds.multiStream != nullptr, !found->second.dynamicPositions.empty() };
			for (uint32_t i = 0; i < std::size(reasons); ++i)
				probeIneligibleReasons[i] += reasons[i];
		}
		// Effect UV/color/alpha updates run after the probe early-out, so even
		// a stationary effect mesh must visit the full material capture path.
		//
		// Arming the probe for effects was tried, with StaticProbe::effectState
		// carrying the UV transform, colours, falloffs and alpha so a still
		// effect could take the early-out. It does not pay: 1361 effects armed
		// and all 1361 reported changed every frame, because the sky rotates and
		// their world transforms move even when their material state does not.
		// The probe build cost host capture 3.9 -> 4.45 ms for no hits.
		if (shape && !animatedAttachment && !water && !effect && !grass && !distantTree && !hairTint && !boneCount &&
			!lodLandscape && !nativeLandscape && !kinds.multiStream && found->second.dynamicPositions.empty()) {
			kinds.probe = BuildStaticProbe(geometry, data, shape, kinds);
			kinds.probeEligible = true;
			++probeArmedThisFrame;
		}
		const auto existing = visible.find(geometry);
		if (existing != visible.end()) {
			const auto& previous = existing->second;
			const bool sameBones = previous.bones.size() == instance.bones.size() &&
				(previous.bones.empty() || std::memcmp(previous.bones.data(), instance.bones.data(), instance.bones.size() * sizeof(remixapi_Transform)) == 0);
			const bool sameDescription = previous.mesh == instance.mesh && sameBones &&
				previous.placements == instance.placements &&
				std::memcmp(&previous.blend, &instance.blend, sizeof(instance.blend)) == 0;
			// Measured 2026-09-19: 294 geometries re-describe here, every one of
			// them because its bones changed -- NPC body parts, hair, brows, a
			// rabbit. Re-describing makes the runtime destroy and rebuild the
			// RtInstance, which is what keeps the merged BLAS bucket cache and
			// the full-skip path in AccelManager from ever settling. Avoiding it
			// needs a way to update bones on a retained instance, the way
			// updateRetainedTransform already handles a pure move.
			if (sameDescription) {
				if (previous.world == instance.world)
					return;
				// Only the transform moved. The runtime already holds everything
				// else about this instance, and re-describing it would make it
				// rebuild the whole draw state; hand it the new transform instead
				// and leave the registration alone. Batched placements carry their
				// transforms in the placement array rather than the instance, so
				// they are not eligible.
				if (updateRetainedTransform && previous.retained &&
					!(previous.placements && !previous.placements->transforms.empty())) {
					remixapi_Transform transform{};
					for (uint32_t row = 0; row < 3; ++row)
						for (uint32_t col = 0; col < 3; ++col)
							transform.matrix[row][col] = instance.world.rotate.entry[row][col] * instance.world.scale;
					transform.matrix[0][3] = instance.world.translate.x;
					transform.matrix[1][3] = instance.world.translate.y;
					transform.matrix[2][3] = instance.world.translate.z;
					if (updateRetainedTransform(previous.retained, &transform) == REMIXAPI_ERROR_CODE_SUCCESS) {
						existing->second.world = instance.world;
						++movedInstancesThisFrame;
						return;
					}
				}
			}
		}
		{
			const bool changedCategories[]{ !shape, water, effect, grass, distantTree, hairTint, boneCount != 0,
				lodLandscape, nativeLandscape, kinds.multiStream != nullptr, !found->second.dynamicPositions.empty() };
			for (uint32_t i = 0; i < std::size(changedCategories); ++i)
				changedReasons[i] += changedCategories[i];
		}
		StoreInstance(geometry, std::move(instance));
		++changedInstancesThisFrame;
	}


	void SetAuditEnabled(bool enabled)
	{
		std::lock_guard lock(mutex);
		if (enabled) {
			audit = {};
			characterAudit.clear();
		}
		auditEnabled.store(enabled, std::memory_order_relaxed);
	}

	void RecordAudit(bool ready, bool domeSubmitted)
	{
		if (!auditEnabled.load(std::memory_order_relaxed)) return;
		std::lock_guard lock(mutex);
		RemixSceneAudit::Sample sample;
		sample.frame = globals::state->frameCount;
		const auto* player = RE::PlayerCharacter::GetSingleton();
		const auto* cell = player ? player->GetParentCell() : nullptr;
		sample.cell = cell ? cell->GetFormID() : 0;
		sample.interior = cell && cell->IsInteriorCell();
		sample.ready = ready;
		sample.dome = domeSubmitted;
		RE::BSGraphics::ViewData auditEye{};
		RE::NiPoint3 auditOrigin{};
		sample.cameraCurrent = RemixNativeRender::ReadWorldCamera(auditEye, auditOrigin);
		RE::NiPoint3 nativeCameraPosition{}, nativePlayerPosition{};
		const bool nativePositionsValid = RemixNativeRender::ReadWorldCameraPositions(nativeCameraPosition, nativePlayerPosition);
		const auto nativeInverse = auditEye.viewMat.Invert();
		auto* ui = RE::UI::GetSingleton();
		sample.fader = ui && ui->IsMenuOpen("Fader Menu");
		sample.submitted = static_cast<uint32_t>(visible.size());
		auto poses = nlohmann::json::array();
		// Captured host inputs, not a readback or proof of GPU/native pose parity.
		std::unordered_set<RE::BSGeometry*> auditedPlayerGeometry = playerBody;
		auditedPlayerGeometry.insert(viewModelGeometry.begin(), viewModelGeometry.end());
		for (auto* geometry : auditedPlayerGeometry) {
			const auto cached = meshes.find(geometry);
			if (cached == meshes.end()) continue;
			const auto& mesh = cached->second;
			const auto submitted = visible.find(geometry);
			nlohmann::json pose = { { "name", geometry->name.c_str() },
				{ "viewModel", viewModelGeometry.contains(geometry) },
				{ "geometry", reinterpret_cast<uintptr_t>(geometry) },
				{ "mesh", reinterpret_cast<uintptr_t>(mesh.handle) },
				{ "vertices", mesh.vertexCount }, { "submitted", submitted != visible.end() } };
			uint64_t morphHash = 14695981039346656037ull;
			const auto* bytes = reinterpret_cast<const unsigned char*>(mesh.dynamicPositions.data());
			for (size_t i = 0; i < mesh.dynamicPositions.size() * sizeof(float); ++i)
				morphHash = (morphHash ^ bytes[i]) * 1099511628211ull;
			pose["morphHash"] = morphHash;
			if (const auto attachment = attachmentAuditTransforms.find(geometry); attachment != attachmentAuditTransforms.end()) {
				pose["attachmentAnchor"] = attachmentAuditAnchors.at(geometry);
				pose["attachmentWorld"] = { attachment->second.translate.x, attachment->second.translate.y, attachment->second.translate.z, attachment->second.scale };
				pose["attachmentRotation"] = nlohmann::json::array();
				for (const auto& row : attachment->second.rotate.entry)
					for (float value : row) pose["attachmentRotation"].push_back(value);
			}
			if (const auto entry = entryViewModelTransforms.find(geometry); entry != entryViewModelTransforms.end()) {
				pose["entryWorld"] = { entry->second.translate.x, entry->second.translate.y, entry->second.translate.z, entry->second.scale };
				pose["entryRotation"] = nlohmann::json::array();
				for (const auto& row : entry->second.rotate.entry)
					for (float value : row) pose["entryRotation"].push_back(value);
			}
			if (submitted != visible.end()) {
				const auto& object = submitted->second;
				pose["retained"] = object.retained;
				pose["world"] = { object.world.translate.x, object.world.translate.y, object.world.translate.z, object.world.scale };
				pose["rotation"] = nlohmann::json::array();
				for (const auto& row : object.world.rotate.entry)
					for (float value : row) pose["rotation"].push_back(value);
				pose["bones"] = nlohmann::json::array();
				for (const auto& bone : object.bones) {
					auto matrix = nlohmann::json::array();
					for (const auto& row : bone.matrix)
						for (float value : row) matrix.push_back(value);
					pose["bones"].push_back(std::move(matrix));
				}
			}
			poses.push_back(std::move(pose));
		}
		characterAudit.push_back({ { "frame", sample.frame }, { "ready", ready },
			{ "nativeCameraPositionsValid", nativePositionsValid },
			{ "nativeSceneCameraPosition", { nativeCameraPosition.x, nativeCameraPosition.y, nativeCameraPosition.z } },
			{ "nativePlayerPosition", { nativePlayerPosition.x, nativePlayerPosition.y, nativePlayerPosition.z } },
			{ "nativeCameraOrigin", { auditOrigin.x, auditOrigin.y, auditOrigin.z } },
			{ "nativeRelativeEye", { nativeInverse._41, nativeInverse._42, nativeInverse._43 } },
			{ "nativeViewModelPoses", nativeViewModelPoseFrame == sample.frame ? nativeViewModelPoseAudit : nlohmann::json::array() },
			{ "eyeWorld", { lastEyeWorld.x, lastEyeWorld.y, lastEyeWorld.z } },
			{ "viewModelEyeWorld", { submittedViewModelEye.x, submittedViewModelEye.y, submittedViewModelEye.z } },
			{ "viewModelCameraValid", submittedViewModelCamera },
			{ "cameraValid", haveLastEyeWorld }, { "bonePhases", bonePhaseAudit }, { "poses", std::move(poses) } });
		if (characterAudit.size() > 256) characterAudit.pop_front();
		for (const auto& [geometry, object] : visible) {
			const auto* property = geometry->GetGeometryRuntimeData().shaderProperty.get();
			sample.classes[6] += !loadedGeometry.contains(geometry);
			if (!property) continue;
			sample.classes[0] += netimmerse_cast<const RE::BSSkyShaderProperty*>(property) != nullptr;
			sample.classes[1] += netimmerse_cast<const RE::BSDistantTreeShaderProperty*>(property) != nullptr;
			using Feature = RE::BSShaderMaterial::Feature;
			const auto feature = property->material ? property->material->GetFeature() : Feature::kNone;
			sample.classes[2] += feature == Feature::kLODLand;
			sample.classes[3] += feature == Feature::kLODObjectsHD || property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kLODObjects);
			sample.classes[4] += feature == Feature::kCloud;
			sample.classes[5] += feature == Feature::kLODLandNoise;
		}
		audit.Record(sample);
	}

	std::string Inspect(const std::string& filter)
	{
		std::lock_guard lock(mutex);
		using json = nlohmann::json;
		if (filter == "characterAudit") {
			return json{ { "enabled", auditEnabled.load() }, { "frames", characterAudit },
				{ "scope", "Host cached player geometry and submitted transforms, not GPU/native pose parity" } }.dump();
		}
		if (filter == "audit") {
			auto encode = [](const RemixSceneAudit::Sample& s) -> json {
				return { { "frame", s.frame }, { "cell", s.cell }, { "interior", s.interior },
					{ "ready", s.ready }, { "dome", s.dome }, { "submitted", s.submitted }, { "classes", s.classes },
					{ "cameraCurrent", s.cameraCurrent }, { "fader", s.fader } };
			};
			json recent = json::array();
			for (size_t i = 0; i < audit.Size(); ++i) recent.push_back(encode(audit.Oldest(i)));
			return json{ { "enabled", auditEnabled.load() }, { "samples", audit.samples },
				{ "interiorSamples", audit.interiorSamples }, { "suspectSamples", audit.suspectSamples },
				{ "failedSamples", audit.failedSamples }, { "cellChanges", audit.cellChanges },
				{ "missingCameraSamples", audit.missingCameraSamples }, { "cameraReadyFailures", audit.cameraReadyFailures },
				{ "emptyReadySamples", audit.emptyReadySamples }, { "firstFailure", audit.firstFailure ? encode(*audit.firstFailure) : json(nullptr) },
				{ "interiorMax", audit.interiorMax }, { "exteriorMax", audit.exteriorMax },
				{ "interiorDomeSamples", audit.interiorDomeSamples }, { "exteriorDomeSamples", audit.exteriorDomeSamples },
				{ "first", audit.first ? encode(*audit.first) : json(nullptr) },
				{ "firstSuspect", audit.firstSuspect ? encode(*audit.firstSuspect) : json(nullptr) },
				{ "recent", recent }, { "classNames", { "sky", "distantTree", "landLOD", "objectLOD", "cloud", "landLODNoise", "notLoaded" } },
				{ "scope", "Every BeforeUI world submission while enabled; host membership and accepted dome API call, not GPU/TLAS or pixel evidence. Interior sky can be authored intentionally. Menus/loading and native-reference frames are excluded." } }.dump();
		}
		const bool inspectVertices = filter.starts_with("vertices:");
		const auto matchFilter = inspectVertices ? filter.substr(9) : filter;
		json result = { { "frame", frame }, { "loaded", loadedGeometry.size() }, { "cached", meshes.size() }, { "submitted", visible.size() } };
		result["submittedCamera"] = { { "valid", haveLastEyeWorld },
			{ "eyeWorld", { lastEyeWorld.x, lastEyeWorld.y, lastEyeWorld.z } },
			{ "impossibleJumps", impossibleEyeJumps }, { "source", "native-world-call" } };
		result["viewModel"] = { { "cameraValid", submittedViewModelCamera },
			{ "geometries", viewModelGeometry.size() }, { "submitted", submittedViewModels },
			{ "eyeWorld", { submittedViewModelEye.x, submittedViewModelEye.y, submittedViewModelEye.z } } };
		if (filter == "camera")
			return result.dump();
		if (filter == "directional") {
			const auto& light = submittedDirectional;
			result["directional"] = { { "valid", light.valid }, { "submitted", directionalSubmitted },
				{ "cell", light.cell }, { "interior", light.interior }, { "capturedFrame", light.engineFrame },
				{ "submitFrame", directionalSubmitEngineFrame }, { "frameSource", "CS-present" }, { "nativeDirection", light.nativeDirection },
				{ "direction", light.direction }, { "diffuse", light.diffuse }, { "fade", light.fade },
				{ "sunlightScale", light.sunlightScale }, { "radiance", light.radiance },
				{ "radianceScale", light.radianceScale },
				{ "linearLighting", light.linearLighting }, { "alreadyLinear", light.alreadyLinear },
				{ "gamma", light.gamma }, { "directionalMultiplier", light.directionalMultiplier }, { "gammaScale", light.gammaScale },
				{ "angularDiameterDegrees", 0.5 }, { "registered", sun != nullptr }, { "source", "native-world-directional-light" } };
			return result.dump();
		}
		if (filter == "refs:tree") {
			json refs = json::array();
			uint32_t count = 0;
			RE::TES::GetSingleton()->ForEachReference([&](RE::TESObjectREFR* ref) {
				if (!ref || !ref->GetBaseObject() || ref->GetBaseObject()->GetFormType() != RE::FormType::Tree)
					return RE::BSContainer::ForEachResult::kContinue;
				++count;
				if (refs.size() >= 48) return RE::BSContainer::ForEachResult::kContinue;
				auto* root = ref->Get3D(false);
				json entry = { { "id", ref->GetFormID() }, { "base", ref->GetBaseObject()->GetFormID() },
					{ "disabled", ref->IsDisabled() }, { "deleted", ref->IsDeleted() }, { "has3D", root != nullptr },
					{ "position", { ref->GetPositionX(), ref->GetPositionY(), ref->GetPositionZ() } } };
				json nodes = json::array();
				std::vector<RE::NiAVObject*> pending{ root };
				std::unordered_set<RE::NiAVObject*> visited;
				while (!pending.empty() && nodes.size() < 48) {
					auto* object = pending.back(); pending.pop_back();
					if (!object || !visited.insert(object).second) continue;
					json node = { { "name", object->name.c_str() }, { "type", object->GetRTTI() ? object->GetRTTI()->GetName() : "" },
						{ "geometry", object->AsGeometry() != nullptr }, { "node", object->AsNode() != nullptr }, { "switch", object->AsSwitchNode() != nullptr } };
					if (auto* selection = object->AsSwitchNode()) node["index"] = RemixNativeRender::ReadSwitchIndex(selection);
					if (auto* geometry = object->AsGeometry()) node["gathered"] = loadedGeometry.contains(geometry);
					if (auto* parent = object->AsNode()) {
						node["children"] = parent->GetChildren().size();
						for (const auto& child : parent->GetChildren()) pending.push_back(child.get());
					}
					nodes.push_back(std::move(node));
				}
				entry["nodes"] = std::move(nodes);
				refs.push_back(std::move(entry));
				return RE::BSContainer::ForEachResult::kContinue;
			});
			result["matched"] = count;
			result["references"] = std::move(refs);
			return result.dump();
		}
		const auto* player = RE::PlayerCharacter::GetSingleton();
		const auto origin = player ? player->GetPosition() : RE::NiPoint3{};
		std::vector<std::pair<float, RE::BSGeometry*>> ordered;
		for (const auto& [geometry, _] : loadedGeometry)
			ordered.emplace_back((geometry->world.translate - origin).SqrLength(), geometry);
		std::ranges::sort(ordered, {}, &std::pair<float, RE::BSGeometry*>::first);
		json entries = json::array();
		uint32_t matched = 0;
		for (const auto& [distance, geometry] : ordered) {
			json ancestors = json::array();
			std::string description;
			for (auto* node = static_cast<RE::NiAVObject*>(geometry); node && ancestors.size() < 12; node = node->parent) {
				const std::string type = node->GetRTTI() ? node->GetRTTI()->GetName() : "";
				description += type + ":" + node->name.c_str() + "/";
				ancestors.push_back({ { "name", node->name.c_str() }, { "type", type }, { "localScale", node->local.scale }, { "worldScale", node->world.scale }, { "appCulled", node->GetAppCulled() } });
			}
			const auto& data = geometry->GetGeometryRuntimeData();
			auto* property = data.shaderProperty.get();
			if (property && property->GetRTTI()) description += property->GetRTTI()->GetName();
			const auto feature = property && property->material ? static_cast<int>(property->material->GetFeature()) : -1;
			// Texture paths are useful for locating authored effects whose mesh
			// names are generic ("Shape", "Plane", etc.). This is inspection only.
			if (property && property->material && property->GetMaterialType() == RE::BSShaderMaterial::Type::kEffect && property->material->GetType() == RE::BSShaderMaterial::Type::kEffect) {
				const auto* effect = static_cast<const RE::BSEffectShaderMaterial*>(property->material);
				description += std::string("/") + effect->sourceTexturePath.c_str() + "/" + effect->greyscaleTexturePath.c_str();
			}
			if (!matchFilter.empty() && description.find(matchFilter) == std::string::npos && matchFilter != "feature:" + std::to_string(feature)) continue;
			++matched;
			if (entries.size() >= 48) continue;
			auto* shape = geometry->AsTriShape();
			const auto cached = meshes.find(geometry);
			const auto submitted = visible.find(geometry);
			json entry = { { "name", geometry->name.c_str() }, { "distance", std::sqrt(distance) }, { "ancestors", ancestors },
				{ "world", { geometry->world.translate.x, geometry->world.translate.y, geometry->world.translate.z } },
				{ "geometryType", static_cast<uint32_t>(geometry->GetType().get()) }, { "shape", shape != nullptr },
				{ "vertices", shape ? shape->GetTrishapeRuntimeData().vertexCount : 0 },
				{ "triangles", shape ? shape->GetTrishapeRuntimeData().triangleCount : 0 }, { "renderer", data.rendererData != nullptr },
				{ "property", property && property->GetRTTI() ? property->GetRTTI()->GetName() : "" },
				{ "distantTreeCast", property && netimmerse_cast<RE::BSDistantTreeShaderProperty*>(property) != nullptr },
				{ "materialType", property ? static_cast<int>(property->GetMaterialType()) : -1 },
				{ "feature", property && property->material ? static_cast<int>(property->material->GetFeature()) : -1 },
				{ "cached", cached != meshes.end() }, { "submitted", submitted != visible.end() },
				{ "alphaFlags", data.alphaProperty ? data.alphaProperty->alphaFlags : 0 },
				{ "alphaThreshold", data.alphaProperty ? data.alphaProperty->alphaThreshold : 0 } };
			if (submitted != visible.end()) entry["placements"] = submitted->second.placements ? submitted->second.placements->transforms.size() : 0;
			entry["importedMaterial"] = cached != meshes.end() && cached->second.materialHandle != nullptr;
			// Read-only native particle inventory. Layouts verified in 1.7.99:
			// f992b0 uses data +158 / lastUpdate +18c; f99ce0 uses worldspace
			// +192; f13550 and f14f70 verify arrays and active count +7c.
			// Never infer particle positions from the object's world translation:
			// world-space emitters deliberately have an identity world transform.
			if (REL::Module::get().version() == REL::Version{ 1, 7, 99, 0 }) {
				if (auto* system = netimmerse_cast<RE::NiParticleSystem*>(geometry)) {
					const auto& state = system->GetParticleSystemRuntimeData();
					const auto* particleData = system->GetParticlesRuntimeData().particleData.get();
					json particles = { { "hasData", particleData != nullptr }, { "worldspace", state.isWorldspace },
						{ "lastUpdate", state.lastUpdate }, { "reset", state.resetSystem }, { "samples", json::array() } };
					if (particleData) {
						const auto& inputs = particleData->GetParticlesRuntimeData();
						particles["active"] = inputs.numVertices;
						particles["capacity"] = inputs.maxNumVertices;
						particles["aspectRatio"] = inputs.aspectRatio;
						particles["subtextureCount"] = inputs.subTextureOffsetsCount;
						particles["hasPositions"] = inputs.positions != nullptr;
						particles["hasColors"] = inputs.color != nullptr;
						particles["hasRotations"] = inputs.rotations != nullptr;
						if (inputs.positions && inputs.numVertices <= inputs.maxNumVertices) {
							for (uint32_t i = 0; i < std::min<uint32_t>(inputs.numVertices, 4); ++i) {
								const auto& p = inputs.positions[i];
								const auto c = inputs.color ? inputs.color[i] : RE::NiColorA{ 1, 1, 1, 1 };
								json sample = { { "position", { p.x, p.y, p.z } }, { "color", { c.red, c.green, c.blue, c.alpha } },
									{ "radius", inputs.radii ? inputs.radii[i] : 1.0f }, { "size", inputs.sizes ? inputs.sizes[i] : 1.0f },
									{ "angle", inputs.rotationAngles ? inputs.rotationAngles[i] : 0.0f } };
								const uint32_t textureIndex = inputs.textureIndices ? inputs.textureIndices[i] : 0;
								sample["textureIndex"] = textureIndex;
								if (inputs.subtextureOffsets && inputs.subTextureOffsetsCount <= 256 && textureIndex < inputs.subTextureOffsetsCount) {
									const auto& uv = inputs.subtextureOffsets[textureIndex];
									sample["subtexture"] = { uv.red, uv.green, uv.blue, uv.alpha };
								}
								particles["samples"].push_back(std::move(sample));
							}
						}
					}
					entry["particles"] = std::move(particles);
				}
			}
			if (cached != meshes.end()) {
				const auto& mesh = cached->second;
				entry["vertexDescriptor"] = mesh.descriptor;
				entry["uvBounds"] = mesh.uvBounds;
				entry["modelSpaceNormals"] = mesh.modelSpaceNormals;
				entry["hairCardsRequested"] = mesh.hairTint;
				entry["skinDiffusionImported"] = mesh.skinDiffusionImported;
				entry["submittedBones"] = submitted != visible.end() ? submitted->second.bones.size() : 0;
				entry["nativeTangentFrame"] = mesh.nativeTangentFrame;
				entry["thinFoliage"] = mesh.thinFoliage && importThinMaterial != nullptr;
				entry["nativeFoliage"] = mesh.foliageFlags && importFoliage != nullptr;
				entry["useFaceNormals"] = mesh.useFaceNormals;
				if (mesh.thinFoliage) {
					entry["foliageFlags"] = mesh.foliageFlags;
					entry["foliageParameters"] = mesh.foliageParameters;
					entry["foliageSoftTexture"] = mesh.foliageTextures[0] != 0;
					entry["foliageBackTexture"] = mesh.foliageTextures[1] != 0;
				}
				if (inspectVertices && !mesh.particles) {
					RE::BSGraphics::VertexDesc descriptor{};
					std::memcpy(&descriptor, &mesh.descriptor, sizeof(descriptor));
					const auto stride = uint32_t((mesh.descriptor & 15) * 4);
					const auto offset = descriptor.GetAttributeOffset(RE::BSGraphics::Vertex::VA_COLOR);
					const bool hasColors = descriptor.HasFlag(RE::BSGraphics::Vertex::VF_COLORS);
					json colors = { { "hasColors", hasColors }, { "stride", stride }, { "offset", offset }, { "count", mesh.vertexCount } };
					colors["uploadedSamplesRGBA"] = json::array();
					for (const auto packed : mesh.uploadedColorSamples)
						colors["uploadedSamplesRGBA"].push_back({ (packed >> 16) & 255, (packed >> 8) & 255, packed & 255, packed >> 24 });
					// Explicit inspector requests only; never introduce per-frame readbacks.
					std::vector<uint8_t> bytes;
					if (hasColors && mesh.vertices && stride >= 12 && stride <= 256 && offset + 4 <= stride &&
						ReadBuffer(mesh.vertices.Get(), size_t(stride) * mesh.vertexCount, bytes)) {
						std::array<uint32_t, 4> minimum{ 255, 255, 255, 255 }, maximum{}, nonWhite{};
						for (uint32_t i = 0; i < mesh.vertexCount; ++i)
							for (uint32_t c = 0; c < 4; ++c) {
								const uint32_t value = bytes[size_t(i) * stride + offset + c];
								minimum[c] = std::min(minimum[c], value);
								maximum[c] = std::max(maximum[c], value);
								nonWhite[c] += value != 255;
							}
						colors["nativeMinRGBA"] = minimum;
						colors["nativeMaxRGBA"] = maximum;
						colors["nativeNonWhiteRGBA"] = nonWhite;
						colors["nativeSamplesRGBA"] = json::array();
						for (size_t i = 0; i < mesh.uploadedColorSamples.size(); ++i) {
							const auto* sample = bytes.data() + (i * (mesh.vertexCount - 1) / (mesh.uploadedColorSamples.size() - 1)) * stride + offset;
							colors["nativeSamplesRGBA"].push_back({ sample[0], sample[1], sample[2], sample[3] });
						}
					}
					entry["vertexColors"] = std::move(colors);
					if (submitted != visible.end()) {
						const auto& blend = submitted->second.blend;
						entry["colorBlend"] = { { "rgbOperation", blend.textureColorOperation },
							{ "vertexColorBakedLighting", bool(blend.isVertexColorBakedLighting) },
							{ "treeVertexColor", mesh.treeVertexColor },
							{ "alphaOperation", blend.textureAlphaOperation }, { "alphaArg2", blend.textureAlphaArg2Source }, { "tFactor", blend.tFactor } };
					}
				}
				if (mesh.effect) entry["effectParameters"] = mesh.effectParameters;
				if (mesh.particles) {
					entry["particleVerticesSubmitted"] = mesh.vertexCount;
					entry["particleTrianglesSubmitted"] = mesh.indexCount / 3;
					entry["particleRevision"] = mesh.particleRevision;
					if (!mesh.particleVertices.empty()) {
						const auto& v = mesh.particleVertices[0];
						const auto p = v.position + mesh.particleCenter;
						entry["particleImportedSample"] = { { "position", { p.x, p.y, p.z } }, { "textureIndex", v.textureIndex },
							{ "rgba", v.rgba }, { "alpha", float(v.rgba >> 24) / 255.0f } };
					}
				}
				if (mesh.water) {
					entry["waterParameters"] = mesh.waterParameters;
					entry["waterObjectUV"] = mesh.waterObjectUV;
					entry["waterFlowUVValid"] = mesh.waterFlowUVValid;
				}
				entry["boundsMin"] = { mesh.boundsMin.x, mesh.boundsMin.y, mesh.boundsMin.z };
				entry["boundsMax"] = { mesh.boundsMax.x, mesh.boundsMax.y, mesh.boundsMax.z };
			}
			if (property) {
				const auto* base = property->GetBaseTexture();
				entry["baseTexture"] = base ? base->name.c_str() : "";
				entry["shaderFlags"] = std::format("{:016X}", property->flags.underlying());
				entry["propertyAlpha"] = property->alpha;
				entry["actualMaterialType"] = property->material ? static_cast<int>(property->material->GetType()) : -1;
				if (property->material && property->GetMaterialType() == RE::BSShaderMaterial::Type::kEffect && property->material->GetType() == RE::BSShaderMaterial::Type::kEffect) {
					const auto* effect = static_cast<const RE::BSEffectShaderMaterial*>(property->material);
					const auto* effectProperty = netimmerse_cast<RE::BSEffectShaderProperty*>(property);
					auto textureInfo = [](const RE::NiPointer<RE::NiSourceTexture>& texture) {
						json value = { { "name", texture ? texture->name.c_str() : "" }, { "present", bool(texture) },
							{ "renderer", texture && texture->rendererTexture != nullptr } };
						if (texture && texture->rendererTexture && texture->rendererTexture->resourceView) {
							D3D11_SHADER_RESOURCE_VIEW_DESC view{};
							reinterpret_cast<ID3D11ShaderResourceView*>(texture->rendererTexture->resourceView)->GetDesc(&view);
							value["format"] = static_cast<uint32_t>(view.Format);
							value["dimension"] = static_cast<uint32_t>(view.ViewDimension);
						}
						return value;
					};
					const auto uvIndex = RemixNativeRender::ReadEffectUVIndex();
					entry["effect"] = {
						{ "uvIndex", uvIndex },
						{ "sourcePath", effect->sourceTexturePath.c_str() }, { "palettePath", effect->greyscaleTexturePath.c_str() },
						{ "source", textureInfo(effect->sourceTexture) }, { "palette", textureInfo(effect->greyscaleTexture) },
						{ "baseColor", { effect->baseColor.red, effect->baseColor.green, effect->baseColor.blue, effect->baseColor.alpha } },
						{ "baseColorScale", effect->baseColorScale }, { "clampMode", effect->effectClampMode.underlying() },
						{ "lightingInfluenceByte", effect->unk81 },
						{ "softDepth", effect->softFalloffDepth },
						{ "falloff", { effect->falloffStartAngle, effect->falloffStopAngle, effect->falloffStartOpacity, effect->falloffStopOpacity } },
						{ "uvTransform", { effect->texCoordOffset[uvIndex].x, effect->texCoordOffset[uvIndex].y, effect->texCoordScale[uvIndex].x, effect->texCoordScale[uvIndex].y } } };
					if (effectProperty && effectProperty->emittanceColor) {
						const auto& color = *effectProperty->emittanceColor;
						entry["effect"]["externalEmittance"] = { color.red, color.green, color.blue };
					}
				}
				if (property->material && property->GetMaterialType() == RE::BSShaderMaterial::Type::kWater) {
					const auto* water = static_cast<const RE::BSWaterShaderMaterial*>(property->material);
					const auto* waterProperty = netimmerse_cast<RE::BSWaterShaderProperty*>(property);
					entry["water"] = {
						{ "flags", waterProperty ? waterProperty->waterFlags.underlying() : 0 },
						{ "normalTextures", { water->normalTexture1 ? water->normalTexture1->name.c_str() : "", water->normalTexture2 ? water->normalTexture2->name.c_str() : "",
							water->normalTexture3 ? water->normalTexture3->name.c_str() : "", water->normalTexture4 ? water->normalTexture4->name.c_str() : "" } },
						{ "shallow", { water->shallowWaterColor.red, water->shallowWaterColor.green, water->shallowWaterColor.blue } },
						{ "deep", { water->deepWaterColor.red, water->deepWaterColor.green, water->deepWaterColor.blue, water->deepWaterColor.alpha } },
						{ "scroll", { water->normalScroll1.x, water->normalScroll1.y, water->normalScroll2.x, water->normalScroll2.y, water->normalScroll3.x, water->normalScroll3.y } },
						{ "scale", { water->uvScaleA[0], water->uvScaleA[1], water->uvScaleA[2] } },
						{ "amplitude", { water->amplitudeA[0], water->amplitudeA[1], water->amplitudeA[2] } },
						{ "alpha", water->alpha }, { "refractionMagnitude", water->refractionMagnitude }, { "fresnel", water->fresnelAmount },
						{ "fogFar", water->underwaterFogDistFar }, { "aboveFogFar", water->aboveWaterFogDistFar }, { "flowmapScale", water->flowmapScale } };
				}
				if (property->material && property->GetMaterialType() == RE::BSShaderMaterial::Type::kLighting) {
					const auto* nativeMaterial = static_cast<const RE::BSLightingShaderMaterialBase*>(property->material);
					const auto describeTextureView = [](const RE::NiPointer<RE::NiSourceTexture>& texture) {
						json value = { { "name", texture ? texture->name.c_str() : "" } };
						if (texture && texture->rendererTexture && texture->rendererTexture->resourceView) {
							D3D11_SHADER_RESOURCE_VIEW_DESC description{};
							static_cast<ID3D11ShaderResourceView*>(texture->rendererTexture->resourceView)->GetDesc(&description);
							value["format"] = static_cast<uint32_t>(description.Format);
							value["dimension"] = static_cast<uint32_t>(description.ViewDimension);
						}
						return value;
					};
					entry["lightingTextureViews"] = {
						{ "diffuse", describeTextureView(nativeMaterial->diffuseTexture) },
						{ "soft", describeTextureView(nativeMaterial->rimSoftLightingTexture) },
						{ "back", describeTextureView(nativeMaterial->specularBackLightingTexture) } };
					auto* textureSet = nativeMaterial->textureSet.get();
					const auto* diffusePath = textureSet ? textureSet->GetTexturePath(RE::BSTextureSet::Texture::kDiffuse) : nullptr;
					const auto* normalPath = textureSet ? textureSet->GetTexturePath(RE::BSTextureSet::Texture::kNormal) : nullptr;
					const auto* lightingProperty = netimmerse_cast<RE::BSLightingShaderProperty*>(property);
					entry["lightingMaterial"] = { { "diffusePath", diffusePath ? diffusePath : "" }, { "normalPath", normalPath ? normalPath : "" },
						{ "diffuseRenderTarget", nativeMaterial->diffuseRenderTargetSourceIndex }, { "alpha", nativeMaterial->materialAlpha },
						{ "specularColor", { nativeMaterial->specularColor.red, nativeMaterial->specularColor.green, nativeMaterial->specularColor.blue } },
						{ "specularPower", nativeMaterial->specularPower }, { "specularScale", nativeMaterial->specularColorScale } };
					if (lightingProperty) {
						const auto* color = lightingProperty->emissiveColor;
						entry["lightingMaterial"]["emissiveColor"] = color ? json{ color->red, color->green, color->blue } : json();
						entry["lightingMaterial"]["emissiveMult"] = lightingProperty->emissiveMult;
					}
					if (feature == static_cast<int>(RE::BSShaderMaterial::Feature::kEnvironmentMap)) {
						const auto* env = static_cast<const RE::BSLightingShaderMaterialEnvmap*>(nativeMaterial);
						entry["lightingMaterial"]["envMapScale"] = env->envMapScale;
					}
					entry["normalTexture"] = nativeMaterial->normalTexture ? nativeMaterial->normalTexture->name.c_str() : "";
					entry["uvTransform"] = { nativeMaterial->texCoordOffset[0].x, nativeMaterial->texCoordOffset[0].y, nativeMaterial->texCoordScale[0].x, nativeMaterial->texCoordScale[0].y };
					if (property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kRefraction)) {
						auto* textures = nativeMaterial->textureSet.get();
						const auto texturePath = [textures](RE::BSTextureSet::Texture slot) {
							const auto* path = textures ? textures->GetTexturePath(slot) : nullptr;
							return path ? path : "";
						};
						entry["refraction"] = {
							{ "power", nativeMaterial->refractionPower }, { "materialAlpha", nativeMaterial->materialAlpha },
							{ "fade", static_cast<RE::BSLightingShaderProperty*>(property)->envmapLODFade },
							{ "activeUVIndex", RemixNativeRender::ReadEffectUVIndex() },
							{ "falloff", property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kRefractionFalloff) },
							{ "diffusePath", texturePath(RE::BSTextureSet::Texture::kDiffuse) },
							{ "normalPath", texturePath(RE::BSTextureSet::Texture::kNormal) },
							{ "uvTransforms", {
								{ nativeMaterial->texCoordOffset[0].x, nativeMaterial->texCoordOffset[0].y, nativeMaterial->texCoordScale[0].x, nativeMaterial->texCoordScale[0].y },
								{ nativeMaterial->texCoordOffset[1].x, nativeMaterial->texCoordOffset[1].y, nativeMaterial->texCoordScale[1].x, nativeMaterial->texCoordScale[1].y } } } };
					}
				}
				if (netimmerse_cast<RE::BSDistantTreeShaderProperty*>(property)) {
					const auto* atlas = RemixNativeRender::ReadDistantTreeAtlas();
					entry["treeAtlas"] = atlas ? atlas->name.c_str() : "";
				}
			}
			if (auto* instances = netimmerse_cast<RE::BSMultiStreamInstanceTriShape*>(geometry)) {
				const auto& runtime = instances->GetMultiStreamTrishapeRuntimeData();
				entry["instanceSize"] = runtime.instanceSize;
				entry["instanceCount"] = runtime.instanceCount;
				entry["groups"] = json::array();
				for (const auto* group : runtime.instanceGroups) {
					if (!group || entry["groups"].size() >= 16) continue;
					entry["groups"].push_back({ { "count", group->instanceCount }, { "visible", group->isVisible },
						{ "hasBuffer", group->vertexBuffer && group->vertexBuffer->buffer }, { "hasCpuData", group->vertexBuffer && group->vertexBuffer->m_data },
						{ "bytes", group->vertexBuffer ? static_cast<uint32_t>(group->vertexBuffer->byteWidth) : 0 } });
				}
			}
			entries.push_back(std::move(entry));
		}
		result["matched"] = matched;
		result["entries"] = std::move(entries);
		return result.dump();
	}

	void SetDistantTreeCullingRespected(bool respected)
	{
		respectDistantTreeCulling.store(respected, std::memory_order_relaxed);
		// The placement snapshots are keyed by which groups went into them, so a
		// change of policy has to make them rebuild rather than be reused.
		std::lock_guard lock(mutex);
		for (auto& [_, mesh] : meshes)
			mesh.placements.reset();
	}

	namespace
	{
		/// @brief Forces every instance group to be read and decoded again.
		void RereadInstanceGroups()
		{
			std::lock_guard lock(mutex);
			for (auto& [_, mesh] : meshes) {
				mesh.groups.clear();
				mesh.placements.reset();
			}
		}
	}

	void SetStrayTreeGroupGuard(bool enabled)
	{
		strayGuardEnabled.store(enabled, std::memory_order_relaxed);
		strayInstanceGroups = 0;
		RereadInstanceGroups();
	}

	void SetStrayTreeGroupInjection(bool enabled)
	{
		injectStrayTreeGroups.store(enabled, std::memory_order_relaxed);
		distantTreeWorstEscape = 0.0f;
		strayInstanceGroups = 0;
		RereadInstanceGroups();
	}

	void DiscardFrame()
	{
		std::lock_guard lock(mutex);
		haveLastEyeWorld = false;
		originTrail.clear();
		if (destroyRetainedInstance)
			for (const auto& [_, object] : visible)
				if (object.retained)
					destroyRetainedInstance(object.retained);
		visible.clear();
		changedInstances.clear();
		for (const auto handle : instanceSetHandles)
			destroyInstanceSet(handle);
		instanceSetHandles.clear();
		for (const auto& [_, mesh] : meshes) {
			api->DestroyMesh(mesh.handle);
			if (mesh.materialHandle)
				api->DestroyMaterial(mesh.materialHandle);
		}
		meshes.clear();
		loadedGeometry.clear();
		// The loading screen has no world frame, so nothing refreshes the capture
		// and nothing sweeps the tracked set. Leaving them would resubmit the old
		// cell's lights on the first frame of the new one, under handles Remix
		// still holds a buffer index for.
		{
			std::lock_guard capturedLock(capturedLightsMutex);
			capturedLights.clear();
			capturedDirectional = {};
		}
		if (sun) api->DestroyLight(sun);
		sun = nullptr;
		directionalSubmitted = false;
		submittedDirectional = {};
		for (const auto& [handle, _] : sceneLights)
			api->DestroyLight(reinterpret_cast<remixapi_LightHandle>(handle));
		sceneLights.clear();
		captureOrder.clear();
		playerBody.clear();
		viewModelGeometry.clear();
		nativeViewModelFrame = 0;
		nativeViewModelBones.clear();
		nativeViewModelWorlds.clear();
		nativeViewModelAttachments.clear();
		nativeViewModelAnchors.clear();
		nativeViewModelOwners.clear();
		nativeViewModelPoseAudit = nlohmann::json::array();
		nativeViewModelPoseFrame = 0;
		attachmentAuditTransforms.clear();
		attachmentAuditAnchors.clear();
		entryViewModelTransforms.clear();
		submittedViewModels = 0;
		submittedViewModelCamera = false;
		grassScales.clear();
		classifications.clear();
		createdThisFrame = 0;
		invalidatedThisFrame = failedUploadsThisFrame = changedInstancesThisFrame = movedInstancesThisFrame = 0;
		rebuiltPlacementsThisFrame = instanceReadbacksThisFrame = 0;
	}

	// 0 submits, 1 disables the whole path, 2 captures but makes no Remix call.
	static uint32_t SceneLightMode()
	{
		static const uint32_t mode = [] {
			char value[8]{};
			if (!GetEnvironmentVariableA("CS_REMIX_NO_SCENE_LIGHTS", value, sizeof(value)))
				return 0u;
			return value[0] == '1' ? 1u : value[0] == '2' ? 2u : 0u;
		}();
		return mode;
	}

	// Skyrim's active light list, converted to Remix sphere lights. Without this
	// the only light in the scene is the sun: torches, fires, candles and magic
	// are all geometry with no illumination, and the volumetric froxel grid --
	// which samples lights, never emissive surfaces -- stays empty indoors.
	//
	// The radiance conversion is Remix's own legacy-light math (rtx_light_utils):
	// a sphere light of fixed radius r whose inverse-square falloff reaches the
	// perceptible threshold at the original light's cutoff distance d has
	// radiance (d^2 * kNewLightEndValue) / (pi * r^2). Skyrim states that cutoff
	// directly as the light's radius, so no attenuation curve has to be solved.
	//
	// This runs from inside the game's world frame, not from the submit path.
	// After the world render the light arrays are no longer the render thread's
	// alone: a cell load tears the old cell's lights out from underneath. Inside
	// the world frame they are stable by the game's own contract, and a loading
	// screen has no world frame at all, so the capture simply does not run rather
	// than racing the teardown. Taking NiPointer copies to survive that instead
	// is worse, not better -- the last reference then drops on the render thread
	// and runs a BSLight destructor the game has not asked for.
	void CaptureSceneLights()
	{
		DirectionalLight directional{};
		directional.engineFrame = globals::state->frameCount;
		const auto* player = RE::PlayerCharacter::GetSingleton();
		const auto* cell = player ? player->GetParentCell() : nullptr;
		directional.cell = cell ? cell->GetFormID() : 0;
		directional.interior = cell && cell->IsInteriorCell();
		auto* manager = globals::game::smState;
		auto* root = manager ? manager->shadowSceneNode[0] : nullptr;
		auto* nativeSun = root ? root->GetRuntimeData().sunLight : nullptr;
		auto* native = nativeSun ? netimmerse_cast<RE::NiDirectionalLight*>(nativeSun->light.get()) : nullptr;
		if (native && cell) {
			const auto& direction = native->GetWorldDirection();
			const auto& light = native->GetLightRuntimeData();
			directional.nativeDirection = { direction.x, direction.y, direction.z };
			directional.direction = directional.nativeDirection;
			directional.diffuse = { light.diffuse.red, light.diffuse.green, light.diffuse.blue };
			directional.fade = light.fade;
			if (auto* imageSpace = globals::game::imageSpaceManager)
				directional.sunlightScale = imageSpace->GetRuntimeData().data.baseData.hdr.sunlightScale;
			const auto linear = globals::features::linearLighting.GetCommonBufferData();
			directional.linearLighting = linear.enableLinearLighting;
			directional.alreadyLinear = linear.isDirLightLinear;
			directional.gamma = linear.lightGamma;
			directional.directionalMultiplier = linear.directionalLightMult;
			directional.gammaScale = linear.dirLightMult;
			directional.valid = RemixDirectionalLightMath::Normalize(directional.direction) &&
				std::isfinite(directional.fade) && std::isfinite(directional.sunlightScale);
			for (size_t i = 0; i < 3; ++i) {
				directional.valid &= std::isfinite(directional.diffuse[i]);
				directional.radiance[i] = RemixDirectionalLightMath::Radiance(directional.diffuse[i], directional.fade,
					directional.sunlightScale, directional.linearLighting, directional.alreadyLinear, directional.gamma,
					directional.directionalMultiplier, directional.gammaScale, directional.interior);
				directional.valid &= std::isfinite(directional.radiance[i]) && directional.radiance[i] >= 0;
			}
		}
		{
			std::lock_guard lock(capturedLightsMutex);
			capturedDirectional = directional;
		}
		if (SceneLightMode() == 1)
			return;
		auto* smState = globals::game::smState;
		auto* shadowSceneNode = smState ? smState->shadowSceneNode[0] : nullptr;
		if (!shadowSceneNode)
			return;
		// Matches Remix's rtx.lightConversionSphereLightFixedRadius default and
		// its kNewLightEndValue; keeping them in step keeps Skyrim's lights
		// consistent with anything Remix converts itself.
		constexpr float kEmitterRadius = 4.0f;
		constexpr float kEndValue = 0.01f;
		const float distanceSquaredToRadiance = kEndValue / (3.14159265358979f * kEmitterRadius * kEmitterRadius);
		std::vector<CapturedLight> captured;
		uint32_t rejectedHidden = 0, rejectedMaskless = 0, rejectedDark = 0;
		auto addLight = [&](RE::BSLight* bsLight) {
			if (!bsLight || !bsLight->light || bsLight->light->GetFlags().any(RE::NiAVObject::Flag::kHidden)) {
				++rejectedHidden;
				return;
			}
			// A shadow light with no mask slot was dropped by the game this frame.
			if (bsLight->IsShadowLight() && static_cast<RE::BSShadowLight*>(bsLight)->GetRuntimeData().maskIndex == 255) {
				++rejectedMaskless;
				return;
			}
			auto* niLight = bsLight->light.get();
			const auto& runtimeData = niLight->GetLightRuntimeData();
			// lodDimmer is the renderer's distance fade for exterior lights. It is
			// only maintained for lights the LOD pass touches: interior lights sit
			// at exactly zero, so multiplying by it unconditionally deletes every
			// torch and hearth in the game. Zero means "not LOD managed" here.
			const float dimmer = bsLight->lodDimmer > 0.f ? bsLight->lodDimmer : 1.f;
			const float fade = runtimeData.fade * dimmer;
			const float radius = runtimeData.radius.x;
			const float brightest = std::max({ runtimeData.diffuse.red, runtimeData.diffuse.green, runtimeData.diffuse.blue });
			if (!(fade > 1e-4f) || !(radius > 1e-4f) || !(brightest > 1e-4f) ||
				!std::isfinite(radius) || !std::isfinite(fade) || !std::isfinite(brightest)) {
				++rejectedDark;
				return;
			}
			const auto& translate = niLight->world.translate;
			if (!std::isfinite(translate.x) || !std::isfinite(translate.y) || !std::isfinite(translate.z))
				return;
			// Fade scales the intensity, not the cutoff: a dimmed torch still
			// reaches as far, so it belongs outside the distance term.
			const float intensity = distanceSquaredToRadiance * radius * radius * fade;
			// The light list is rebuilt as cells load and NiLight allocations are
			// reused, so the pointer is the only stable identity available. The
			// 0x4C tag keeps the handle space clear of the sun's 0x4353... A
			// reused address costs at most one frame of temporal light reuse.
			const uint64_t mixed = std::hash<const void*>{}(niLight) * 0x9e3779b97f4a7c15ull;
			CapturedLight light{};
			light.handle = 0x4c00000000000000ull | (mixed >> 8);
			// Positions are absolute here; the submit path rebases them onto the
			// same origin every instance is rebased around.
			light.position = { translate.x, translate.y, translate.z };
			light.radiance = { runtimeData.diffuse.red / brightest * intensity,
				runtimeData.diffuse.green / brightest * intensity,
				runtimeData.diffuse.blue / brightest * intensity };
			captured.push_back(light);
		};
		auto& runtime = shadowSceneNode->GetRuntimeData();
		for (auto& entry : runtime.activeLights)
			addLight(entry.get());
		for (auto& entry : runtime.activeShadowLights)
			addLight(entry.get());
		std::lock_guard lock(capturedLightsMutex);
		capturedLights = std::move(captured);
		capturedOffered = static_cast<uint32_t>(runtime.activeLights.size() + runtime.activeShadowLights.size());
		capturedShadow = static_cast<uint32_t>(runtime.activeShadowLights.size());
		capturedRejectedHidden = rejectedHidden;
		capturedRejectedMaskless = rejectedMaskless;
		capturedRejectedDark = rejectedDark;
	}

	bool SetDirectionalLightScale(float scale)
	{
		if (!RemixDirectionalLightMath::ValidRadianceScale(scale)) return false;
		directionalRadianceScale.store(scale, std::memory_order_relaxed);
		return true;
	}

	void SetTreeVertexBakedLighting(bool enabled)
	{
		std::lock_guard lock(mutex);
		if (treeVertexBakedLighting == enabled) return;
		treeVertexBakedLighting = enabled;
		// Static probe hits skip Capture's blend construction. Update retained
		// descriptions too, without reuploading geometry or changing its identity.
		for (auto& [geometry, instance] : visible) {
			const auto mesh = meshes.find(geometry);
			if (mesh == meshes.end() || !mesh->second.treeVertexColor) continue;
			instance.blend.isVertexColorBakedLighting = enabled;
			changedInstances.insert(geometry);
		}
	}

	void SubmitDirectionalLight(bool& success)
	{
		{
			std::lock_guard lock(capturedLightsMutex);
			submittedDirectional = capturedDirectional;
		}
		auto& light = submittedDirectional;
		light.radianceScale = directionalRadianceScale.load(std::memory_order_relaxed);
		for (auto& channel : light.radiance) channel *= light.radianceScale;
		directionalSubmitEngineFrame = globals::state->frameCount;
		const auto* player = RE::PlayerCharacter::GetSingleton();
		const auto* cell = player ? player->GetParentCell() : nullptr;
		directionalSubmitted = false;
		const bool active = light.valid && light.engineFrame == directionalSubmitEngineFrame &&
			cell && light.cell == cell->GetFormID() && std::ranges::any_of(light.radiance, [](float v) { return v > 0; });
		if (!active) {
			if (sun) success = api->DestroyLight(sun) == REMIXAPI_ERROR_CODE_SUCCESS && success;
			sun = nullptr;
			return;
		}
		if (!sun || registeredSunDirection != light.direction || registeredSunRadiance != light.radiance) {
			remixapi_LightInfoDistantEXT distant{};
			distant.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT;
			distant.direction = { light.direction[0], light.direction[1], light.direction[2] };
			distant.angularDiameterDegrees = 0.5f;
			distant.volumetricRadianceScale = 1;
			remixapi_LightInfo info{};
			info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
			info.pNext = &distant;
			info.hash = 0x43535343454e4502ull;
			info.radiance = { light.radiance[0], light.radiance[1], light.radiance[2] };
			remixapi_LightHandle updated = nullptr;
			if (api->CreateLight(&info, &updated) != REMIXAPI_ERROR_CODE_SUCCESS) {
				success = false;
				if (sun) api->DestroyLight(sun);
				sun = nullptr;
				return;
			}
			sun = updated;
			registeredSunDirection = light.direction;
			registeredSunRadiance = light.radiance;
		}
		directionalSubmitted = api->DrawLightInstance(sun) == REMIXAPI_ERROR_CODE_SUCCESS;
		success = directionalSubmitted && success;
	}

	void SubmitSceneLights(const RE::NiPoint3& origin, bool& success)
	{
		sceneLightsThisFrame = sceneLightsUpdatedThisFrame = 0;
		if (SceneLightMode())
			return;
		std::vector<CapturedLight> captured;
		uint32_t offered = 0, offeredShadow = 0, rejectedHidden = 0, rejectedMaskless = 0, rejectedDark = 0;
		{
			std::lock_guard lock(capturedLightsMutex);
			captured = capturedLights;
			offered = capturedOffered;
			offeredShadow = capturedShadow;
			rejectedHidden = capturedRejectedHidden;
			rejectedMaskless = capturedRejectedMaskless;
			rejectedDark = capturedRejectedDark;
		}
		for (const auto& light : captured) {
			SceneLight state{};
			state.position = { light.position.x - origin.x, light.position.y - origin.y, light.position.z - origin.z };
			state.radiance = light.radiance;
			state.lastSeenFrame = frame;
			auto [found, inserted] = sceneLights.try_emplace(light.handle, state);
			// Re-describing a light that has not moved or changed colour costs a
			// command-stream lambda per light per frame for nothing. Re-describing
			// one that has keeps Remix's stable identity, so temporal reuse and
			// RTXDI reservoirs survive the update.
			const bool changed = inserted ||
				std::memcmp(&found->second.position, &state.position, sizeof(state.position)) != 0 ||
				std::memcmp(&found->second.radiance, &state.radiance, sizeof(state.radiance)) != 0;
			found->second = state;
			if (changed) {
				remixapi_LightInfoSphereEXT sphere{};
				sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
				sphere.position = state.position;
				sphere.radius = 4.0f;
				sphere.volumetricRadianceScale = 1.0f;
				remixapi_LightInfo info{};
				info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
				info.pNext = &sphere;
				info.hash = light.handle;
				info.radiance = state.radiance;
				remixapi_LightHandle created = nullptr;
				if (api->CreateLight(&info, &created) != REMIXAPI_ERROR_CODE_SUCCESS) {
					sceneLights.erase(light.handle);
					continue;
				}
				++sceneLightsUpdatedThisFrame;
			}
			success = api->DrawLightInstance(reinterpret_cast<remixapi_LightHandle>(light.handle)) == REMIXAPI_ERROR_CODE_SUCCESS && success;
			++sceneLightsThisFrame;
		}
		if (frame % 120 == 0) {
			logger::info("[RemixScene.lights] {} submitted, {} re-described, {} tracked; offered {} ({} shadow); rejected hidden={} maskless={} dark={}",
				sceneLightsThisFrame, sceneLightsUpdatedThisFrame, sceneLights.size(), offered, offeredShadow,
				rejectedHidden, rejectedMaskless, rejectedDark);
		}
		std::erase_if(sceneLights, [&](const auto& pair) {
			if (pair.second.lastSeenFrame == frame)
				return false;
			api->DestroyLight(reinterpret_cast<remixapi_LightHandle>(pair.first));
			return true;
		});
	}

	bool Submit()
	{
		RE::BSGraphics::ViewData eye{};
		RE::NiPoint3 origin{};
		if (!RemixNativeRender::ReadWorldCamera(eye, origin)) {
			static uint64_t missingWorldCamera = 0;
			if (++missingWorldCamera == 1 || missingWorldCamera % 120 == 0)
				logger::warn("[RemixScene.camera] missing current-frame world-camera snapshot ({})", missingWorldCamera);
			return false;
		}
		const bool firstPersonView = RE::PlayerCamera::GetSingleton() && RE::PlayerCamera::GetSingleton()->IsInFirstPerson();
		RE::BSGraphics::ViewData viewModelEye{};
		RE::NiPoint3 viewModelOrigin{}, viewModelTranslation{};
		const bool haveViewModelCamera = firstPersonView && nativeViewModelFrame == globals::state->frameCount &&
			RemixNativeRender::ReadViewModelCamera(viewModelEye, viewModelOrigin, &viewModelTranslation);
		// Remix's temporal lighting caches and acceleration structures use a
		// persistent world space. Undo Skyrim's per-frame camera-relative origin
		// in the view matrix; meshes, lights and instance placements stay absolute.
		const float nativeOrigin[3]{ origin.x, origin.y, origin.z };
		RemixCameraMath::RestoreWorldViewTranslation(&eye.viewMat._11, nativeOrigin);
		origin = {};
		using Clock = std::chrono::steady_clock;
		const auto gatherStart = Clock::now();
		// Scene discovery can overlap the next animation update. Keep existing
		// skeletons at this frame's entry pose, independent of discovery cadence.
		BoneSnapshot boneSnapshot;
		SnapshotBones(captureOrder, boneSnapshot);
		attachmentAuditTransforms.clear();
		attachmentAuditAnchors.clear();
		SnapshotViewModelAttachments(boneSnapshot, attachmentAuditTransforms, attachmentAuditAnchors, playerBody);
		if (firstPersonView) SnapshotViewModelAttachments(boneSnapshot);
		BoneSnapshot earlyPlayerBones;
		const bool auditBones = auditEnabled.load(std::memory_order_relaxed);
		if (auditBones) {
			entryViewModelTransforms.clear();
			for (auto* geometry : viewModelGeometry) entryViewModelTransforms.emplace(geometry, geometry->world);
			for (auto* geometry : playerBody) {
				const auto* skin = geometry->GetGeometryRuntimeData().skinInstance.get();
				if (!skin || !skin->skinData || !skin->boneWorldTransforms) continue;
				const auto count = skin->skinData->GetBoneCount();
				if (count > REMIXAPI_INSTANCE_INFO_MAX_BONES_COUNT) continue;
				for (uint32_t bone = 0; bone < count; ++bone) {
					const auto* source = skin->boneWorldTransforms[bone];
					if (source && !earlyPlayerBones.contains(source)) earlyPlayerBones.emplace(source, *source);
				}
			}
		}
		if (lastSubmitEnd)
			restTiming.Add(std::chrono::duration<double, std::milli>(gatherStart - *lastSubmitEnd).count());
		if (lastGatherStart)
			periodTiming.Add(std::chrono::duration<double, std::milli>(gatherStart - *lastGatherStart).count());
		lastGatherStart = gatherStart;
		// Walking every attached cell, reference and node finds objects entering
		// and leaving the scene. That membership changes as cells stream, not as
		// objects move, so it is discovered on a cadence rather than every frame;
		// what an object is doing is detected separately, by probing the retained
		// set below. Retention holds NiPointer references, so an object the game
		// unloads stays valid until the next walk notices it.
		static RE::FormID lastSceneCell = 0;
		const auto* scenePlayer = RE::PlayerCharacter::GetSingleton();
		const auto* sceneCell = scenePlayer ? scenePlayer->GetParentCell() : nullptr;
		const auto sceneCellID = sceneCell ? sceneCell->GetFormID() : 0;
		const bool rediscover = sceneCellID != lastSceneCell || (frame % kSceneDiscoveryInterval) == 0;
		uint32_t added = 0, removed = 0;
		if (rediscover) {
			const auto& snapshot = RemixSceneGraph::Gather();
			std::lock_guard lock(mutex);
			if (!api)
				return false;
			lastSceneCell = sceneCellID;
			for (auto* geometry : snapshot.geometry)
				added += !loadedGeometry.contains(geometry);
			for (auto it = meshes.begin(); it != meshes.end();) {
				if (!snapshot.geometry.Contains(it->first)) {
					RetireInstance(it->first, "left the scene graph");
					api->DestroyMesh(it->second.handle);
					if (it->second.materialHandle)
						api->DestroyMaterial(it->second.materialHandle);
					it = meshes.erase(it);
					++removed;
				} else {
					++it;
				}
			}
			std::erase_if(grassScales, [&](const auto& hint) { return !snapshot.geometry.Contains(hint.first); });
			std::erase_if(emptySince, [&](const auto& entry) { return !snapshot.geometry.Contains(entry.first); });
			std::erase_if(classifications, [&](const auto& entry) { return !snapshot.geometry.Contains(entry.first); });
			// Hold Ni references until real detachment/unload, preventing pointer
			// reuse from aliasing the mesh/material cache to another object.
			std::erase_if(loadedGeometry, [&](const auto& entry) { return !snapshot.geometry.Contains(entry.first); });
			for (auto* geometry : snapshot.geometry)
				if (!loadedGeometry.contains(geometry))
					loadedGeometry.emplace(geometry, RE::NiPointer<RE::BSGeometry>(geometry));

			// Capture iterates a contiguous list rather than the retention map:
			// the probe is memory-bound, so the order it walks the scene in is
			// worth as much as the work it skips. View models are tagged separately
			// and captured only when their native camera ran this frame.
			captureOrder.clear();
			captureOrder.reserve(loadedGeometry.size());
			for (const auto& [geometry, _] : loadedGeometry)
				captureOrder.push_back(geometry);
			viewModelGeometry.clear();
			for (auto* geometry : snapshot.firstPerson)
				viewModelGeometry.insert(geometry);
			playerBody.clear();
			for (auto* geometry : snapshot.playerBody)
				playerBody.insert(geometry);
			sceneNodes = snapshot.nodes;
			sceneReferences = snapshot.references;
			sceneAppCulled = snapshot.appCulled;
			sceneFirstPerson = static_cast<uint32_t>(snapshot.firstPerson.Size());
		}
		const auto captureStart = Clock::now();
		gatherTiming.Add(std::chrono::duration<double, std::milli>(captureStart - gatherStart).count());
		// New skeletons discovered this frame have no entry sample. Existing
		// shared sources retain their entry sample across all meshes.
		if (rediscover) {
			SnapshotBones(captureOrder, boneSnapshot);
			SnapshotViewModelAttachments(boneSnapshot, attachmentAuditTransforms, attachmentAuditAnchors, playerBody);
			if (firstPersonView) SnapshotViewModelAttachments(boneSnapshot);
		}
		if (haveViewModelCamera) {
			// Apply exactly the camera's native-to-restored translation to frozen
			// bones and rigid attachments. No later skeleton/local-chain reads.
			for (const auto& [source, native] : nativeViewModelBones) {
				auto restored = native;
				restored.translate += viewModelTranslation;
				boneSnapshot.insert_or_assign(source, restored);
			}
			for (const auto& [geometry, native] : nativeViewModelWorlds) {
				if (!viewModelGeometry.contains(geometry)) continue;
				if (geometry->GetGeometryRuntimeData().skinInstance) continue;
				const auto attached = nativeViewModelAttachments.find(geometry);
				auto restored = attached != nativeViewModelAttachments.end() ? attached->second : native;
				restored.translate += viewModelTranslation;
				attachmentAuditTransforms.insert_or_assign(geometry, restored);
				const auto anchor = nativeViewModelAnchors.find(geometry);
				attachmentAuditAnchors.insert_or_assign(geometry, anchor != nativeViewModelAnchors.end() ? anchor->second : "native rigid world");
			}
		}
		if (auditBones) {
			uint32_t compared = 0, changed = 0;
			float maxTranslation = 0, maxRotation = 0;
			for (const auto& [source, early] : earlyPlayerBones) {
				if (!boneSnapshot.contains(source)) continue;
				const auto late = *source;
				++compared;
				changed += early != late;
				maxTranslation = std::max(maxTranslation, (early.translate - late.translate).Length());
				for (uint32_t row = 0; row < 3; ++row)
					for (uint32_t col = 0; col < 3; ++col)
						maxRotation = std::max(maxRotation, std::abs(early.rotate.entry[row][col] - late.rotate.entry[row][col]));
			}
			bonePhaseAudit = { { "sceneFrame", frame }, { "rediscovered", rediscover },
				{ "compared", compared }, { "changed", changed }, { "maxTranslation", maxTranslation },
				{ "maxRotation", maxRotation },
				{ "elapsedMs", std::chrono::duration<double, std::milli>(Clock::now() - gatherStart).count() } };
		}
		for (auto* geometry : captureOrder) {
			if (viewModelGeometry.contains(geometry)) {
				std::lock_guard lock(mutex);
				if (!haveViewModelCamera || !nativeViewModelWorlds.contains(geometry) || geometry->GetAppCulled()) {
					// View-model overlays use authored visibility (for example a
					// clean weapon hides its blood layer). They are not retained
					// as off-screen world shadow/reflection casters.
					RetireInstance(geometry, "view-model camera inactive or geometry hidden");
					continue;
				}
				// A weapon can be rigid but still follows the camera each frame.
				if (auto found = classifications.find(geometry); found != classifications.end())
					found->second.probeEligible = false;
			}
			Capture(geometry, boneSnapshot);
		}
		const auto submitStart = Clock::now();
		captureTiming.Add(std::chrono::duration<double, std::milli>(submitStart - captureStart).count());
		std::lock_guard lock(mutex);
		// Source is the unconditional world-camera call in Main::Draw, not the
		// shared pass state at UI time. First-person rendering replaces that
		// state's origin with local view-model coordinates (observed 0,0,120.5).
		// Near-plane similarity cannot establish a camera's identity and holding
		// a previous frame creates camera judder, so neither heuristic is used.
		{
			const auto inverseView = eye.viewMat.Invert();
			OriginSample sample{ origin, { origin.x + inverseView._41, origin.y + inverseView._42, origin.z + inverseView._43 } };
			if (originTrail.size() >= 24)
				originTrail.erase(originTrail.begin());
			originTrail.push_back(sample);
		}
		// What actually goes wrong is that a frame gets rendered from somewhere
		// other than where the player is, and the flash is only how that looks
		// when the elsewhere happens to be lit terrain. Measuring the look is
		// unreliable -- it depends on which way the camera faces inside the wall,
		// and most directions hide it completely -- so measure the cause: the
		// world-space eye position the frame will be traced from. The camera is
		// submitted rebased around the origin, so that position is the origin
		// plus the inverse view translation. A player cannot cross a few hundred
		// units between two frames; anything that does is the defect, whatever it
		// renders as.
		{
			const auto inverseView = eye.viewMat.Invert();
			const RE::NiPoint3 eyeWorld{
				origin.x + inverseView._41,
				origin.y + inverseView._42,
				origin.z + inverseView._43
			};
			if (haveLastEyeWorld) {
				const float dx = eyeWorld.x - lastEyeWorld.x;
				const float dy = eyeWorld.y - lastEyeWorld.y;
				const float dz = eyeWorld.z - lastEyeWorld.z;
				const float jump = std::sqrt(dx * dx + dy * dy + dz * dz);
				if (jump > kImpossibleEyeJump) {
					++impossibleEyeJumps;
					if (impossibleEyeJumps <= 12 || (impossibleEyeJumps & 0xFF) == 0) {
						logger::info("[RemixScene.camera] eye jumped {} units in one frame (#{}): ({}, {}, {}) from ({}, {}, {}); origin ({}, {}, {})",
							jump, impossibleEyeJumps,
							eyeWorld.x, eyeWorld.y, eyeWorld.z,
							lastEyeWorld.x, lastEyeWorld.y, lastEyeWorld.z,
							origin.x, origin.y, origin.z);
					}
				}
			}
			lastEyeWorld = eyeWorld;
			haveLastEyeWorld = true;
		}
		remixapi_CameraInfo camera{};
		camera.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
		camera.type = REMIXAPI_CAMERA_TYPE_WORLD;
		std::memcpy(camera.view, &eye.viewMat, sizeof(camera.view));
		// Skyrim's own projection carries its TAA jitter: a sub-pixel offset the
		// game varies every frame, which is why the engine keeps an unjittered
		// copy of its own alongside it. Remix jitters the projection itself for
		// the upscaler and can only unjitter what it applied, so handing it the
		// game's jittered matrix adds a second offset it cannot account for, and
		// the accumulated image shakes by a fraction of a pixel -- worst while
		// the camera moves, because that is when reprojection matters most.
		// Measured here: _31 = -0.000260, _32 = 0.000309, which at 1920 wide is
		// about a quarter of a pixel, exactly the scale of a TAA jitter.
		//
		// The jitter is removed from the game's own matrix rather than replaced
		// with globals::game::frameBufferCached.GetCameraProjUnjittered(). That
		// buffer is written per render pass, so with the world draws suppressed
		// it does not reliably hold this frame's world camera: substituting it
		// produced a visibly smeared image with geometry in the wrong places.
		// Zeroing the two jitter terms leaves every other element of the frame's
		// real projection untouched.
		auto projection = eye.projMat;
		projection._31 = 0.0f;
		projection._32 = 0.0f;
		std::memcpy(camera.projection, &projection, sizeof(camera.projection));
		if (frame % 120 == 0) {
			logger::info("[RemixScene.camera] removed projection jitter _31={:.6f} _32={:.6f}",
				eye.projMat._31, eye.projMat._32);
		}
		bool success = api->SetupCamera(&camera) == REMIXAPI_ERROR_CODE_SUCCESS;
		submittedViewModels = 0;
		submittedViewModelCamera = false;
		if (haveViewModelCamera) {
			const float modelOrigin[]{ viewModelOrigin.x, viewModelOrigin.y, viewModelOrigin.z };
			RemixCameraMath::RestoreWorldViewTranslation(&viewModelEye.viewMat._11, modelOrigin);
			const auto modelInverse = viewModelEye.viewMat.Invert();
			submittedViewModelEye = { modelInverse._41, modelInverse._42, modelInverse._43 };
			remixapi_CameraInfo modelCamera{};
			modelCamera.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
			modelCamera.type = REMIXAPI_CAMERA_TYPE_VIEW_MODEL;
			std::memcpy(modelCamera.view, &viewModelEye.viewMat, sizeof(modelCamera.view));
			viewModelEye.projMat._31 = viewModelEye.projMat._32 = 0;
			std::memcpy(modelCamera.projection, &viewModelEye.projMat, sizeof(modelCamera.projection));
			submittedViewModelCamera = api->SetupCamera(&modelCamera) == REMIXAPI_ERROR_CODE_SUCCESS;
			success = submittedViewModelCamera && success;
		}
		uint32_t instancedBatches = 0;
		uint32_t instancedPlacements = 0;
		uint32_t retainedBatches = 0;
		// The player-body category depends on the point of view, not on the
		// instance itself, so a view change is a genuine change for those.
		if (firstPersonView != previousFirstPersonView) {
			previousFirstPersonView = firstPersonView;
			for (const auto& [geometry, _] : visible)
				if (playerBody.contains(geometry))
					changedInstances.insert(geometry);
		}
		const bool retainedPath = createRetainedInstance && updateRetainedInstance && destroyRetainedInstance && drawRetainedInstances;
		uint32_t submittedThisFrame = 0;
		for (const auto& [geometry, constObject] : visible) {
			submittedViewModels += viewModelGeometry.contains(geometry);
			// Only genuinely changed instances are described again. Everything
			// else stays registered in the runtime from an earlier frame.
			if (retainedPath && constObject.retained && !changedInstances.contains(geometry))
				continue;
			auto& object = const_cast<Instance&>(constObject);
			++submittedThisFrame;
			remixapi_InstanceInfo instance{};
			instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
			instance.mesh = object.mesh;
			instance.doubleSided = true;
			if (viewModelGeometry.contains(geometry))
				instance.categoryFlags |= REMIXAPI_INSTANCE_CATEGORY_BIT_VIEW_MODEL;
			if (const auto mesh = meshes.find(geometry); mesh != meshes.end() && mesh->second.hairTint)
				instance.categoryFlags |= REMIXAPI_INSTANCE_CATEGORY_BIT_HAIR_CARDS;
			if (const auto mesh = meshes.find(geometry); mesh != meshes.end() && mesh->second.effect) {
				instance.categoryFlags |= REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_OPACITY_MICROMAP;
				// Native alpha effects are composited layers, not partially
				// transmitting solid BRDF surfaces. Keep their triangle geometry
				// but use Remix's effect-layer resolve and lighting approximation.
				if (object.blend.alphaBlendEnabled)
					instance.categoryFlags |= REMIXAPI_INSTANCE_CATEGORY_BIT_PARTICLE;
				const auto* property = geometry->GetGeometryRuntimeData().shaderProperty.get();
				instance.doubleSided = property && property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kTwoSided);
			}
			// First-person cameras sit inside the third-person head. Retain
			// that body for secondary/shadow rays, not primary world rays.
			if (firstPersonView && playerBody.contains(geometry))
				instance.categoryFlags |= REMIXAPI_INSTANCE_CATEGORY_BIT_THIRD_PERSON_PLAYER_MODEL;
			auto blend = object.blend;
			instance.pNext = &blend;
			remixapi_InstanceInfoBoneTransformsEXT bones{};
			if (!object.bones.empty()) {
				bones.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BONE_TRANSFORMS_EXT;
				bones.boneTransforms_values = object.bones.data();
				bones.boneTransforms_count = static_cast<uint32_t>(object.bones.size());
				bones.pNext = instance.pNext;
				instance.pNext = &bones;
			}
			for (uint32_t row = 0; row < 3; ++row)
				for (uint32_t col = 0; col < 3; ++col)
					instance.transform.matrix[row][col] = object.world.rotate.entry[row][col] * object.world.scale;
			// Absolute world translation on the retained path: the runtime applies
			// the camera-origin rebasing, so a stationary object's description
			// never changes just because the camera moved. The immediate path
			// still rebases here because nothing downstream does it for it.
			const auto rebase = retainedPath ? RE::NiPoint3{} : origin;
			instance.transform.matrix[0][3] = object.world.translate.x - rebase.x;
			instance.transform.matrix[1][3] = object.world.translate.y - rebase.y;
			instance.transform.matrix[2][3] = object.world.translate.z - rebase.z;
			if (object.placements && !object.placements->transforms.empty()) {
				const auto& placements = object.placements->transforms;
				++instancedBatches;
				instancedPlacements += static_cast<uint32_t>(placements.size());
				instance.transform = {};
				instance.transform.matrix[0][0] = instance.transform.matrix[1][1] = instance.transform.matrix[2][2] = 1;
				// Keep placements in stable absolute coordinates. Remix retains the
				// batch's previous object transform, not a previous placement array;
				// putting camera rebasing here preserves correct grass motion vectors.
				instance.transform.matrix[0][3] = -origin.x;
				instance.transform.matrix[1][3] = -origin.y;
				instance.transform.matrix[2][3] = -origin.z;
				if (!object.placements->grassPhases.empty()) {
					float worldWindAndTimer[4]{};
					const auto hint = grassScales.find(geometry);
					if (hint != grassScales.end() && hint->second.hasWind) {
						const auto& wind = hint->second.wind;
						const auto worldWind = object.world.rotate * RE::NiPoint3{ wind.x, wind.y, 0 } * (object.world.scale * wind.z * 0.5f);
						worldWindAndTimer[0] = worldWind.x;
						worldWindAndTimer[1] = worldWind.y;
						worldWindAndTimer[2] = worldWind.z;
						worldWindAndTimer[3] = hint->second.timer;
					}
					success = drawGrassInstanceSet(&instance, object.placements->handle, worldWindAndTimer) == REMIXAPI_ERROR_CODE_SUCCESS && success;
				} else if (retainedPath && createRetainedInstanceSet) {
					// A batch of static placements has no more reason to be
					// re-described every frame than a static single instance does.
					// A placement array is rebuilt as a new object when it changes,
					// so the identity comparison in Capture already reports the
					// change, and the skip at the top of this loop keeps an
					// unchanged batch out of here entirely. Grass above keeps the
					// per-frame path: its vertices are deformed by the wind each
					// frame, which is what that call does.
					//
					// Absolute translation, like every other retained transform:
					// the runtime subtracts the scene origin, arriving at the same
					// -origin the immediate path writes here.
					instance.transform.matrix[0][3] = 0;
					instance.transform.matrix[1][3] = 0;
					instance.transform.matrix[2][3] = 0;
					const auto result = object.retained ?
						updateRetainedInstance(&instance, object.retained, object.placements->handle) :
						createRetainedInstanceSet(&instance, object.placements->handle, &object.retained);
					success = result == REMIXAPI_ERROR_CODE_SUCCESS && success;
					++retainedBatches;
				} else {
					success = drawInstanceSet(&instance, object.placements->handle) == REMIXAPI_ERROR_CODE_SUCCESS && success;
				}
			} else if (retainedPath) {
				const auto result = object.retained ?
					updateRetainedInstance(&instance, object.retained, 0) :
					createRetainedInstance(&instance, &object.retained);
				success = result == REMIXAPI_ERROR_CODE_SUCCESS && success;
			} else {
				success = api->DrawInstance(&instance) == REMIXAPI_ERROR_CODE_SUCCESS && success;
			}
		}
		changedInstances.clear();
		if (retainedPath) {
			// The restored world-view matrix and all submitted geometry share the
			// absolute world origin, including immediate grass and lighting caches.
			const float sceneOrigin[3]{ origin.x, origin.y, origin.z };
			success = drawRetainedInstances(sceneOrigin) == REMIXAPI_ERROR_CODE_SUCCESS && success;
		}
		// Explicit resource retirement avoids calling another DLL from static
		// destructors during process shutdown. Queued Remix draws retain owners.
		std::unordered_set<uint64_t> liveSets;
		for (const auto& [_, object] : visible)
			if (object.placements && object.placements->handle)
				liveSets.insert(object.placements->handle);
		for (const auto& [_, mesh] : meshes) {
			if (mesh.placements && mesh.placements->handle)
				liveSets.insert(mesh.placements->handle);
			// The set held over an empty rebuild is still handed to instances, so
			// it has to count as live. Without this the sweep below destroys the
			// handle while it is in use, which is worse than the flicker holding
			// it was meant to fix.
			if (mesh.lastGoodPlacements && mesh.lastGoodPlacements->handle)
				liveSets.insert(mesh.lastGoodPlacements->handle);
		}
		std::erase_if(instanceSetHandles, [&](uint64_t handle) {
			if (liveSets.contains(handle))
				return false;
			destroyInstanceSet(handle);
			return true;
		});
		SubmitDirectionalLight(success);
		SubmitSceneLights(origin, success);
		const auto submitEnd = Clock::now();
		submitTiming.Add(std::chrono::duration<double, std::milli>(submitEnd - submitStart).count());
		lastSubmitEnd = submitEnd;
		if (frame % 120 == 0) {
			logger::info("[RemixScene.cpu] median ms period={:.3f} (p95 {:.3f}, p99 {:.3f}) rest={:.3f} gather={:.3f} capture={:.3f} submit={:.3f} (rolling 120 frames); {} probe hits / {} full captures / {} staggered ({} reached arm, {} armed, {} changed, max stable {})",
				periodTiming.Median(), periodTiming.Percentile(0.95), periodTiming.Percentile(0.99),
				restTiming.Median(), gatherTiming.Median(), captureTiming.Median(), submitTiming.Median(), probeHitsThisFrame, probeMissesThisFrame, probeStaggeredThisFrame,
				probeReachedArmThisFrame, probeArmedThisFrame, probeChangedThisFrame, probeMaxStableFrames);
			std::string reasons;
			for (uint32_t i = 0; i < probeIneligibleReasons.size(); ++i)
				if (probeIneligibleReasons[i])
					reasons += std::format("{}={} ", probeReasonNames[i], probeIneligibleReasons[i]);
			logger::info("[RemixScene.cpu] ineligible categories: {}", reasons.empty() ? "none" : reasons);
			std::string changedBy;
			for (uint32_t i = 0; i < changedReasons.size(); ++i)
				if (changedReasons[i])
					changedBy += std::format("{}={} ", probeReasonNames[i], changedReasons[i]);
			logger::info("[RemixScene.cpu] re-described by category: {}", changedBy.empty() ? "none" : changedBy);
			logger::info("[RemixScene] {} retained instances, {} cached meshes, {} uploaded this frame; {} instance descriptions submitted (retained path {})",
				visible.size(), meshes.size(), createdThisFrame, submittedThisFrame, retainedPath ? "on" : "off");
			logger::info("[RemixScene] {} instanced batches ({} retained) / {} placements; {} invalidated meshes, {} failed uploads", instancedBatches, retainedBatches, instancedPlacements, invalidatedThisFrame, failedUploadsThisFrame);
			logger::info("[RemixScene.graph] {} loaded geometries / {} nodes / {} references; {} added, {} retired meshes, {} changed instances, {} moved only; {} app-culled geometries retained; {} view-model geometries, {} submitted (camera {})", loadedGeometry.size(), sceneNodes, sceneReferences, added, removed, changedInstancesThisFrame, movedInstancesThisFrame, sceneAppCulled, sceneFirstPerson, submittedViewModels, submittedViewModelCamera);
			logger::info("[RemixScene.instances] {} placement arrays rebuilt / {} GPU source readbacks this frame", rebuiltPlacementsThisFrame, instanceReadbacksThisFrame);
			logger::info("[RemixScene.grassWindAudit] independent/native comparison samples={} maxError={}", grassWindAuditSamples, grassWindAuditMaxError);
			logger::info("[RemixScene.distantTree] {} groups this frame, {} the engine still wants; {} instances, {} of them wanted; culled groups {}; {} groups read mid-upload and held",
				distantTreeGroupsSeen, distantTreeGroupsVisible, distantTreeInstancesSeen, distantTreeInstancesVisible,
				respectDistantTreeCulling.load(std::memory_order_relaxed) ? "dropped" : "kept", tornInstanceGroups);
			logger::info("[RemixScene.instances] furthest a distant-tree placement has sat outside its group bound: {} units; {} groups rejected for it since load",
				distantTreeWorstEscape, strayInstanceGroups);
			if (recycledInstanceGroups)
				logger::info("[RemixScene.instances] {} groups since load had their buffer handed back for another cell and were re-read", recycledInstanceGroups);
			{
				// An object that is retired and re-registered again within half a
				// second was never really gone: that is what reads as flickering.
				std::vector<const InstanceChurn*> churning;
				for (const auto& [_, record] : instanceChurn)
					if (record.quickReturns > 0)
						churning.push_back(&record);
				std::ranges::sort(churning, [](const auto* a, const auto* b) { return a->quickReturns > b->quickReturns; });
				uint32_t totalQuickReturns = 0;
				for (const auto* record : churning)
					totalQuickReturns += record->quickReturns;
				logger::info("[RemixScene.churn] {} objects returned within 30 frames of being retired, {} times in total",
					churning.size(), totalQuickReturns);
				std::vector<const InstanceChurn*> jumping;
				for (const auto& [_, record] : instanceChurn)
					if (record.teleports > 0)
						jumping.push_back(&record);
				std::ranges::sort(jumping, [](const auto* a, const auto* b) { return a->teleports > b->teleports; });
				logger::info("[RemixScene.churn] {} objects placed somewhere they had not been", jumping.size());
				for (size_t i = 0; i < jumping.size() && i < 10; ++i)
					logger::info("[RemixScene.churn]   '{}' jumped {}x, largest {} units",
						jumping[i]->name, jumping[i]->teleports, jumping[i]->largestTeleport);
				for (size_t i = 0; i < churning.size() && i < 10; ++i)
					logger::info("[RemixScene.churn]   '{}' retired {}x ({}), returned quickly {}x",
						churning[i]->name, churning[i]->retires, churning[i]->lastRetireReason, churning[i]->quickReturns);
			}
			for (const auto& [geometry, hint] : grassScales)
				if (hint.hasWind)
					logger::info("[RemixScene.grassWind] '{}' native vector=({},{},{}) timer={} previous={} totalRevision={} (GPU deformation submitted)",
						geometry->name.c_str(), hint.wind.x, hint.wind.y, hint.wind.z, hint.timer, hint.previousTimer, grassWindRevision);
			const auto inverse = eye.viewMat.Invert();
			logger::info("[RemixScene] origin ({}, {}, {}), inverse view translation ({}, {}, {})", origin.x, origin.y, origin.z, inverse._41, inverse._42, inverse._43);
			// The world is submitted rebased around the origin and the camera is
			// submitted relative to the same origin. If those two do not step
			// together the whole scene slides under the camera by the difference,
			// which is the shape a smoothly moving camera's jitter takes.
			logger::info("[RemixScene.rebase] origin steps over the last {} frames: {}", originTrail.size(), [] {
				std::string out;
				for (size_t i = 1; i < originTrail.size(); ++i) {
					const auto& a = originTrail[i - 1];
					const auto& b = originTrail[i];
					out += std::format("{:.3f}/{:.3f} ", std::sqrt((b.origin.x - a.origin.x) * (b.origin.x - a.origin.x) +
						(b.origin.y - a.origin.y) * (b.origin.y - a.origin.y) + (b.origin.z - a.origin.z) * (b.origin.z - a.origin.z)),
						std::sqrt((b.eye.x - a.eye.x) * (b.eye.x - a.eye.x) + (b.eye.y - a.eye.y) * (b.eye.y - a.eye.y) +
						(b.eye.z - a.eye.z) * (b.eye.z - a.eye.z)));
				}
				return out;
			}());
		}
		createdThisFrame = 0;
		invalidatedThisFrame = failedUploadsThisFrame = changedInstancesThisFrame = movedInstancesThisFrame = 0;
		rebuiltPlacementsThisFrame = instanceReadbacksThisFrame = 0;
		probeHitsThisFrame = probeMissesThisFrame = 0;
		probeReachedArmThisFrame = probeArmedThisFrame = probeChangedThisFrame = probeStaggeredThisFrame = 0;
		probeIneligibleReasons.fill(0);
		changedReasons.fill(0);
		distantTreeGroupsSeen = distantTreeGroupsVisible = 0;
		distantTreeInstancesSeen = distantTreeInstancesVisible = 0;
		tornInstanceGroups = 0;
		++frame;
		return success;
	}
}
