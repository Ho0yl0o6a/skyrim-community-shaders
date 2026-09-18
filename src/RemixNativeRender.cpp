#include "RemixNativeRender.h"
#include "RemixBridge.h"
#include "RemixMaterial.h"
#include <RE/N/NiParticleSystem.h>
#include <RE/N/NiPSysData.h>

#include <atomic>
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

		// The game draws its own sky into the reflections cubemap, one face per
		// frame, and that image is what Remix is given as its sky. Suppressing
		// the world would take those draws with it and leave nothing to hand
		// over, so this one pass is allowed through. It renders into the game's
		// own cube, never the screen, so nothing of it reaches the display.
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
				draws = dispatches = 0;
				suppress = RemixBridge::SuppressWorld();
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
				if (!RemixBridge::SuppressWorld()) return func(effect, arg2, arg3);
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
		stl::write_thunk_jmp<ComputeCall>(base + 0x1589adc);
		stl::write_thunk_call<WaterFlowCopy>(base + 0x657912);
		stl::write_thunk_call<InterfaceCall<0x6566c1>>(base + 0x6566c1);
		stl::write_thunk_call<InterfaceCall<0x6e4b60>>(base + 0x6e4b60);
		installed = true;
		logger::info("[RemixNative] installed audited 1.7.99 game-code submission and UI boundaries; no D3D11/Vulkan suppression hooks");
	}
}
