#include "RemixNativeRender.h"
#include "State.h"
#include "RemixBridge.h"
#include "RemixMaterial.h"
#include "RemixScene.h"
#include "RemixSky.h"
#include <RE/N/NiParticleSystem.h>
#include <RE/N/NiPSysData.h>

#include <atomic>
#include <chrono>
#include <tuple>

namespace RemixNativeRender
{
	namespace
	{
		// Addresses and signatures recovered from the matching 1.7.99 executable.
		// See docs/development/remix-native-render-audit.md. World scope starts
		// outside Main::Draw and includes its trailing image-space composition.
		std::atomic<bool> suppress{ false };
		std::atomic<uint32_t> draws{ 0 }, dispatches{ 0 };
		uint32_t frames = 0;
		bool installed = false;
		RE::BSGraphics::ViewData worldEye{};
		RE::NiPoint3 worldOrigin{};
		RE::NiPoint3 worldCameraPosition{}, worldPlayerPosition{};
		uint32_t worldCameraFrame = 0;
		bool worldCameraValid = false;
		RE::BSGraphics::ViewData viewModelEye{};
		RE::NiPoint3 viewModelOrigin{}, viewModelRootPosition{};
		RE::NiPointer<RE::NiAVObject> viewModelRoot;
		uint32_t viewModelCameraFrame = 0;

		struct ViewModelCameraCall
		{
			static void thunk(RE::BSGraphics::State* state, const RE::NiCamera* camera, uint32_t flags)
			{
				func(state, camera, flags);
				const auto* nativeCamera = *reinterpret_cast<RE::NiCamera**>(REL::Module::get().base() + 0x3436220);
				if (!camera || camera != nativeCamera)
					return;
				auto* player = RE::PlayerCharacter::GetSingleton();
				auto* root = player ? player->Get3D(true) : nullptr;
				if (!root || root == player->Get3D(false))
					return;
				auto& shadow = globals::game::shadowState->GetRuntimeData();
				viewModelEye = shadow.cameraData.getEye();
				viewModelOrigin = shadow.posAdjust.getEye();
				viewModelRoot.reset(root);
				viewModelRootPosition = root->world.translate;
				viewModelCameraFrame = globals::state->frameCount;
				RemixScene::CaptureViewModelPose();
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// Startup-only paired-buffer diagnostic. Preserve the native cache key
		// and all camera bookkeeping, but generate this world's projection without
		// sample jitter. Both state pairs are restored before other cameras run.
		struct MatchedWorldCameraCacheCall
		{
			static void thunk(RE::BSGraphics::State* state, const RE::NiCamera* camera, bool useJitter, bool alternate)
			{
				auto* jitter = reinterpret_cast<std::byte*>(state) + 0x44;
				std::array<float, 4> originalJitter;
				std::memcpy(originalJitter.data(), jitter, sizeof(originalJitter));
				const std::array<float, 4> zeroJitter{};
				std::memcpy(jitter, zeroJitter.data(), sizeof(zeroJitter));
				func(state, camera, useJitter, alternate);
				std::memcpy(jitter, originalJitter.data(), sizeof(originalJitter));
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct WorldCameraCall
		{
			static void thunk(RE::BSGraphics::State* state, const RE::NiCamera* camera, uint32_t flags)
			{
				func(state, camera, flags);
				// Audited unconditional call at Main::Draw+0x3d4: RSI is the
				// WorldRootCamera, R8D=1. Snapshot the resulting view AND origin
				// before first-person setup overwrites the shared shadow state.
				if (camera == RE::Main::WorldRootCamera()) {
					auto& shadow = globals::game::shadowState->GetRuntimeData();
					worldEye = shadow.cameraData.getEye();
					worldOrigin = shadow.posAdjust.getEye();
					worldCameraPosition = camera->world.translate;
					const auto* player = RE::PlayerCharacter::GetSingleton();
					worldPlayerPosition = player ? player->GetPosition() : RE::NiPoint3{};
					// CS owns this counter and advances it once at Present, after
					// world capture and UI submission. The direct CommonLib field
					// at State+0x4c is not a frame counter on 1.7.99 (live values
					// alternate between float-like bit patterns).
					worldCameraFrame = globals::state->frameCount;
					worldCameraValid = true;
				}
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// The game draws its own sky into the reflections cubemap, one face per
		// frame, and that image is what Remix is given as its sky. Suppressing
		// the world would take those draws with it and leave nothing to hand
		// over, so sky draws into that cube are allowed through. They are the
		// only geometry the game still renders for itself, and they land in the
		// game's own cube, never on the screen.
		//
		// The pass is let through whole rather than filtered by shader: the game
		// no longer submits its LOD terrain, object and tree roots into that cube
		// at all, because RemixSky::SetReflectionLodEnabled clears the settings
		// that gate them. Nothing is left to draw there but the sky.
		bool RenderingSkyCubemap()
		{
			auto* shadowState = globals::game::shadowState;
			return shadowState &&
			       shadowState->GetRuntimeData().cubeMapRenderTarget == RE::RENDER_TARGETS_CUBEMAP::kREFLECTIONS;
		}

		bool Suppressing()
		{
			return suppress.load(std::memory_order_relaxed) && !RenderingSkyCubemap();
		}

		struct WorldFrame
		{
			static void thunk(RE::Main* main, uint32_t imageSpaceTarget)
			{
				worldCameraValid = false;
				viewModelRoot.reset();
				draws = dispatches = 0;
				suppress = RemixBridge::SuppressWorld();
				// Reapply before this frame can populate the reflection cubemap.
				RemixSky::SetReflectionLodEnabled(!suppress.load(std::memory_order_relaxed));
				// Every pass inside this world render reads the same answer; see
				// RemixBridge::SuppressWorldThisFrame.
				suppress = RemixBridge::LatchSuppressWorld(suppress.load(std::memory_order_relaxed));
				// The draws below are suppressed, but everything that leads to
				// them -- scenegraph traversal, culling, batch building, shader
				// and constant setup -- still runs. This measures what that
				// costs, because the frame is CPU bound outside the plugin's
				// own gather/capture/submit and this is the prime suspect.
				const auto worldStart = std::chrono::steady_clock::now();
				struct Timed
				{
					std::chrono::steady_clock::time_point start;
					~Timed()
					{
						static double total = 0.0;
						static uint32_t samples = 0;
						total += std::chrono::duration<double, std::milli>(
							std::chrono::steady_clock::now() - start).count();
						if (++samples % 120 == 0) {
							logger::info("[RemixNativeRender] suppressed world render costs {:.3f} ms/frame (mean of 120)",
								total / 120.0);
							total = 0.0;
						}
					}
				} timed { worldStart };
				// The game's light lists are the render thread's own only inside
				// the world frame. Capturing here rather than at submit time is
				// what keeps a cell load from tearing them out mid-walk.
				RemixScene::CaptureSceneLights();
				func(main, imageSpaceTarget);
				// Do not end suppression here: the caller still has world image-
				// space work before it enters the separately hooked UI call.
			}
			static inline decltype(thunk)* func;
		};

		template <uintptr_t Rva, class... Args>
		struct Draw
		{
			static void thunk(Args... args)
			{
				if (Suppressing()) {
					++draws;
					if constexpr (Rva == 0x100d190) {
						// This wrapper consumes a dynamic-stream offset even when
						// no GPU work is emitted. Preserve its post-draw sentinel.
						auto data = std::get<1>(std::tuple{ args... });
						*reinterpret_cast<uint32_t*>(data + 0x18) = 0xffffffff;
					}
					return;
				}
				func(args...);
			}
			static inline decltype(thunk)* func;
		};
		using Word = uintptr_t;
		template <uintptr_t Rva> using Draw3 = Draw<Rva, Word, Word, uint32_t>;
		template <uintptr_t Rva> using Draw4 = Draw<Rva, Word, Word, uint32_t, uint32_t>;

		struct ComputeCall
		{
			static void thunk(Word renderer, Word shader, uint32_t x, uint32_t y, uint32_t z)
			{
				if (Suppressing()) {
					++dispatches;
					return;
				}
				func(renderer, shader, x, y, z);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct WaterFlowCopy
		{
			static uintptr_t thunk(uintptr_t effect, uintptr_t arg2, uintptr_t arg3)
			{
				if (!RemixBridge::SuppressWorldThisFrame()) return func(effect, arg2, arg3);
				const auto* bytes = reinterpret_cast<const uint8_t*>(effect);
				const auto source = *reinterpret_cast<RE::BSGraphics::Texture* const*>(bytes + 0x10);
				const auto destination = *reinterpret_cast<RE::BSGraphics::Texture* const*>(bytes + 0x18);
				const auto atlas = ReadWaterGlobals().flowAtlas;
				// Only the audited terrain-flow tile call, mode 0, actual atlas.
				if (*reinterpret_cast<const uint32_t*>(bytes + 0x60) != 0 || !atlas || destination != atlas->rendererTexture)
					return func(effect, arg2, arg3);
				std::array<uint32_t, 8> rectangle;
				std::memcpy(rectangle.data(), bytes + 0x40, sizeof(rectangle));
				if (!RemixMaterial::CopyWaterFlowTile(source, destination, rectangle))
					logger::error("[RemixMaterial.waterFlowTile] owned atlas copy failed");
				return 0;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		template <uintptr_t CallRva>
		struct InterfaceCall
		{
			static void thunk(Word menuManager)
			{
				suppress = false;
				if (++frames % 120 == 0)
					logger::info("[RemixNative] game-code boundary suppressed {} draw batches / {} compute submissions", draws.load(), dispatches.load());
				RemixBridge::BeforeUI();
				func(menuManager);
				RemixBridge::AfterUI();
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct ScaleformBegin
		{
			static void thunk(Word renderer)
			{
				func(renderer);
				// UI render-target/clear setup has completed, but no menu has
				// rendered yet. Remix writes Skyrim's framebuffer directly.
				RemixBridge::ComposeBeforeMenus();
			}
			static inline decltype(thunk)* func;
		};

		// Measures one call site inside Main::Draw without changing behaviour.
		// The suppressed world frame costs about 4 ms with every draw already
		// stubbed, and that time has to be attributed before any of it can be
		// cut; the phases are named in docs/development/remix-performance.md.
		void Verify(uintptr_t rva, std::initializer_list<uint8_t> bytes)
		{
			const auto address = REL::Module::get().base() + rva;
			if (std::memcmp(reinterpret_cast<const void*>(address), bytes.begin(), bytes.size()) != 0)
				stl::report_and_fail(std::format("Remix native hook bytes mismatch at RVA {:X}; refusing to patch this executable.", rva));
		}

		void VerifyBranch(uintptr_t rva, uintptr_t target, uint8_t opcode)
		{
			const auto address = REL::Module::get().base() + rva;
			int32_t displacement;
			std::memcpy(&displacement, reinterpret_cast<const void*>(address + 1), sizeof(displacement));
			if (*reinterpret_cast<const uint8_t*>(address) != opcode || rva + 5 + displacement != target)
				stl::report_and_fail(std::format("Remix native call mismatch at RVA {:X}; refusing to patch this executable.", rva));
		}

		template <class Hook>
		void Attach(uintptr_t rva)
		{
			Hook::func = reinterpret_cast<decltype(Hook::func)>(REL::Module::get().base() + rva);
			if (DetourAttach(reinterpret_cast<PVOID*>(&Hook::func), reinterpret_cast<PVOID>(Hook::thunk)) != NO_ERROR)
				stl::report_and_fail("Failed to prepare independent Remix game-code hook.");
		}
	}

	bool ReadWorldCamera(RE::BSGraphics::ViewData& eye, RE::NiPoint3& origin)
	{
		// Both producer and consumer run on the render thread. Never substitute
		// a previous frame or an unrelated pass when the world call is absent.
		if (!worldCameraValid || worldCameraFrame != globals::state->frameCount)
			return false;
		eye = worldEye;
		origin = worldOrigin;
		return true;
	}

	bool ReadWorldCameraPositions(RE::NiPoint3& camera, RE::NiPoint3& player)
	{
		if (!worldCameraValid || worldCameraFrame != globals::state->frameCount) return false;
		camera = worldCameraPosition;
		player = worldPlayerPosition;
		return true;
	}

	bool ReadViewModelCamera(RE::BSGraphics::ViewData& eye, RE::NiPoint3& origin, RE::NiPoint3* modelTranslation)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!viewModelRoot || viewModelCameraFrame != globals::state->frameCount ||
			!player || player->Get3D(true) != viewModelRoot.get())
			return false;
		eye = viewModelEye;
		// Main::Draw restores the recursively rebased model after its pass.
		// Move the captured camera by the same translation before importing
		// those restored transforms and bones. See remix-first-person.md.
		const auto translation = viewModelRoot->world.translate - viewModelRootPosition;
		origin = viewModelOrigin + translation;
		if (modelTranslation) *modelTranslation = translation;
		return true;
	}

	int32_t ReadSwitchIndex(const RE::NiSwitchNode* node)
	{
		if (!node || REL::Module::get().version() != REL::Version{ 1, 7, 99, 0 })
			return -1;
		// NiSwitchNode::OnVisible (0xeedea0) reads +0x12c. In the cross-VR
		// CommonLib layout NiNode omits its children member, so direct access
		// to the derived `index` field instead reads the children vtable's
		// high DWORD. GetChildren() already uses a runtime-aware accessor.
		return *reinterpret_cast<const int32_t*>(reinterpret_cast<const uint8_t*>(node) + 0x12c);
	}

	RE::NiSourceTexture* ReadDistantTreeAtlas()
	{
		if (REL::Module::get().version() != REL::Version{ 1, 7, 99, 0 })
			return nullptr;
		// 0x50c100 loads the worldspace atlas and assigns this shader global.
		// BSDistantTreeShader::SetupTechnique (0x1558d70) binds its renderer
		// texture's SRV to PS slot 0. The property-local slot at +0x90 is empty.
		return *reinterpret_cast<RE::NiSourceTexture**>(REL::Module::get().base() + 0x3486718);
	}

	std::array<RE::NiAVObject*, 4> ReadExteriorLodRoots()
	{
		if (!installed || REL::Module::get().version() != REL::Version{ 1, 7, 99, 0 })
			return {};
		const auto base = REL::Module::get().base();
		// Audited terrain attach/detach and SetCullState use these owning globals.
		// See .research/scene-membership-native-audit.json.
		return { *reinterpret_cast<RE::NiAVObject**>(base + 0x3203520),
			*reinterpret_cast<RE::NiAVObject**>(base + 0x3203528),
			*reinterpret_cast<RE::NiAVObject**>(base + 0x3203538),
			*reinterpret_cast<RE::NiAVObject**>(base + 0x3203540) };
	}

	WaterGlobals ReadWaterGlobals()
	{
		if (REL::Module::get().version() != REL::Version{ 1, 7, 99, 0 }) return {};
		const auto base = REL::Module::get().base();
		// BSWaterShader material/geometry setup: independent source globals,
		// not the last visible pass's constant buffer or bound resources.
		return { *reinterpret_cast<RE::NiSourceTexture**>(base + 0x33d3e60),
			*reinterpret_cast<RE::NiSourceTexture**>(base + 0x3336448),
			*reinterpret_cast<const uint32_t*>(base + 0x33d3d98),
			*reinterpret_cast<const float*>(base + 0x20d68d0),
			{ *reinterpret_cast<const float*>(base + 0x34364b0), *reinterpret_cast<const float*>(base + 0x34364b4),
				*reinterpret_cast<const float*>(base + 0x34364b8), *reinterpret_cast<const float*>(base + 0x34364bc) },
			{ *reinterpret_cast<const float*>(base + 0x3436460), 1.0f - *reinterpret_cast<const float*>(base + 0x3436464) } };
	}

	bool BuildParticleVertices(RE::NiParticleSystem* system, std::vector<ParticleVertex>& vertices)
	{
		static_assert(sizeof(ParticleVertex) == 20);
		if (!system || REL::Module::get().version() != REL::Version{ 1, 7, 99, 0 }) return false;
		const auto* data = system->GetParticlesRuntimeData().particleData.get();
		if (!data) return false;
		const auto& inputs = data->GetParticlesRuntimeData();
		if (inputs.numVertices > inputs.maxNumVertices) return false;
		// Same count limit as the audited caller at 0x155edf0. Unlike that
		// caller, this is invoked for retained emitters irrespective of frustum.
		const uint32_t count = std::min<uint32_t>(inputs.numVertices, 2048);
		vertices.resize(count * 4);
		// An emitter between bursts reports zero live particles. That is not a
		// failure to read it, and the caller distinguishes the two so a churn
		// report does not blame the import for the game's own spawn cadence.
		if (!count) return true;
		if (!inputs.positions || !inputs.radii || !inputs.sizes ||
			((inputs.unk88 || inputs.unk89) && !data->GetPSysRuntimeData().particleInfo)) return false;
		const auto address = REL::Module::get().base() + 0x1016930;
		static constexpr uint8_t signature[]{ 0x48, 0x8b, 0xc4, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56 };
		if (std::memcmp(reinterpret_cast<const void*>(address), signature, sizeof(signature)) != 0) return false;
		using Build = void (*)(uint32_t, RE::NiParticleSystem*, ParticleVertex*);
		// Audited callees only sort indices, allocate scratch memory, calculate
		// native fades and write four 20-byte vertices per particle. No renderer
		// state changes, GPU submissions, simulation updates or visibility calls.
		reinterpret_cast<Build>(address)(count, system, vertices.data());
		return true;
	}

	uint32_t ReadEffectUVIndex()
	{
		if (REL::Module::get().version() != REL::Version{ 1, 7, 99, 0 }) return 0;
		// BSEffectShader::SetupMaterial at 0x15569e0 selects this UV pair.
		const auto index = *reinterpret_cast<const uint32_t*>(REL::Module::get().base() + 0x20d69d0);
		return index < 2 ? index : 0;
	}

	bool ReadGrassWind(const RE::BSGeometry* geometry, const RE::BSGrassShaderProperty* property, RE::NiPoint3& wind, float& timer)
	{
		if (!installed || !geometry || !property || !std::isfinite(geometry->world.scale) || std::abs(geometry->world.scale) < 1e-8f)
			return false;
		const auto base = REL::Module::get().base();
		auto read = [base](uintptr_t rva) { return *reinterpret_cast<const float*>(base + rva); };
		// BSGrassShader::SetupGeometry at RVA 0x1522610. Preserve its float
		// constants and multiplication order; this is the game's clock, not wall time.
		timer = read(0x20d68e0) * read(0x197f0e8) * read(0x1b3f734) * property->wavePeriod;
		const float amplitude = std::min(read(0x33d4c38), read(0x1b5bea8)) * read(0x20d6d30);
		const auto inverse = geometry->world.Invert();
		RE::NiPoint3 direction{ inverse.rotate.entry[0][1] * inverse.scale,
			inverse.rotate.entry[1][1] * inverse.scale, inverse.rotate.entry[2][1] * inverse.scale };
		const float length = std::sqrt(direction.x * direction.x + direction.y * direction.y + direction.z * direction.z);
		if (!std::isfinite(length) || length < 1e-8f || !std::isfinite(timer) || !std::isfinite(amplitude))
			return false;
		// Native wind stores the first two components of normalized inverse-
		// world +Y and replaces Z with amplitude. RunGrass uses (wind.xy, 0).
		wind = { direction.x / length, direction.y / length, amplitude };
		return true;
	}

	void Install()
	{
		if (!RemixBridge::IsRequested())
			return;
		if (REL::Module::get().version() != REL::Version{ 1, 7, 99, 0 })
			stl::report_and_fail("The independently audited Remix game hooks currently require Skyrim 1.7.99.0.");
		char test[8]{}, match[8]{};
		const bool matchCaptureSamples = GetEnvironmentVariableA("CS_REMIX_TEST", test, sizeof(test)) == 1 && test[0] == '1' &&
			GetEnvironmentVariableA("CS_REMIX_MATCH_CAPTURE_SAMPLES", match, sizeof(match)) == 1 && match[0] == '1';
		if (matchCaptureSamples)
			VerifyBranch(0x656ee4, 0x101c8f0, 0xe8);
		// Validate every site before changing any game code. These are not the
		// existing CS pass/dirty-state/Main::Draw/compute/UI entry hooks.
		Verify(0x656ab0, { 0x40, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xec, 0x40 });
		Verify(0x117baf0, { 0x48, 0x8b, 0x01, 0x48, 0x8b, 0x48, 0x18, 0x48, 0x8b, 0x01, 0x48, 0xff, 0x60, 0x20 });
		for (auto rva : { 0x100ba70, 0x100bb70, 0x100d190, 0x100d2c0 })
			Verify(rva, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57 });
		Verify(0x100bc70, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x18, 0x48, 0x89 });
		Verify(0x100c4c0, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x18, 0x56, 0x57 });
		Verify(0x100ce90, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x30 });
		Verify(0x100c5f0, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x83, 0xec, 0x50 });
		Verify(0x100c720, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x55, 0x57 });
		Verify(0x100c9c0, { 0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48, 0x89 });
		VerifyBranch(0x1589adc, 0x100f280, 0xe9);  // tail jump, not CALL
		VerifyBranch(0x657912, 0x1540b50, 0xe8);
		VerifyBranch(0x6566c1, 0x116aa20, 0xe8);
		VerifyBranch(0x656fd4, 0x101c400, 0xe8);
		VerifyBranch(0x1514ff7, 0x101c400, 0xe8);
		VerifyBranch(0x6e4b60, 0x116aa20, 0xe8);
		if (DetourTransactionBegin() != NO_ERROR || DetourUpdateThread(GetCurrentThread()) != NO_ERROR)
			stl::report_and_fail("Failed to begin independent Remix game-code hook transaction.");
		Attach<WorldFrame>(0x656ab0);
		Attach<ScaleformBegin>(0x117baf0);
		Attach<Draw4<0x100ba70>>(0x100ba70);
		Attach<Draw<0x100bb70, Word, Word, uint32_t, uint32_t, Word>>(0x100bb70);
		Attach<Draw<0x100bc70, Word, Word, uint32_t, uint32_t, uint32_t, Word, Word>>(0x100bc70);
		Attach<Draw<0x100c4c0, Word, Word, Word, uint32_t, uint32_t>>(0x100c4c0);
		Attach<Draw<0x100c5f0, Word, Word, Word, uint32_t, uint32_t, uint32_t>>(0x100c5f0);
		Attach<Draw3<0x100c720>>(0x100c720);
		Attach<Draw<0x100c9c0, Word, Word, uint32_t, Word, uint32_t>>(0x100c9c0);
		Attach<Draw3<0x100ce90>>(0x100ce90);
		Attach<Draw4<0x100d190>>(0x100d190);
		Attach<Draw<0x100d2c0, Word, Word, uint32_t, uint32_t, uint32_t>>(0x100d2c0);
		if (DetourTransactionCommit() != NO_ERROR)
			stl::report_and_fail("Failed to commit independent Remix game-code hooks.");
		const auto base = REL::Module::get().base();
		if (matchCaptureSamples) {
			stl::write_thunk_call<MatchedWorldCameraCacheCall>(base + 0x656ee4);
			logger::info("[RemixNative] paired-buffer diagnostic: world camera cache generated without jitter");
		}
		stl::write_thunk_jmp<ComputeCall>(base + 0x1589adc);
		stl::write_thunk_call<WaterFlowCopy>(base + 0x657912);
		stl::write_thunk_call<WorldCameraCall>(base + 0x656fd4);
		stl::write_thunk_call<ViewModelCameraCall>(base + 0x1514ff7);
		stl::write_thunk_call<InterfaceCall<0x6566c1>>(base + 0x6566c1);
		stl::write_thunk_call<InterfaceCall<0x6e4b60>>(base + 0x6e4b60);
		installed = true;
		logger::info("[RemixNative] installed audited 1.7.99 game-code submission and UI boundaries; no D3D11/Vulkan suppression hooks");
	}
}
