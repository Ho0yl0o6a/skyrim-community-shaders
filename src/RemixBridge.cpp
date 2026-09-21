#include "RemixBridge.h"

#include "DxvkLoader.h"
#include "Deferred.h"
#include "Globals.h"
#include "Features/GrassLighting.h"
#include "RemixScene.h"
#include "RemixSky.h"
#include "State.h"
#include "Util.h"
#include "Utils/D3D.h"
#include <fstream>
#include <nlohmann/json.hpp>
#include <remix/remix_c.h>

namespace RemixBridge
{
	namespace
	{
		using GuiInputFn = uint32_t(REMIXAPI_CALL*)(uint32_t, uint32_t, float);
		GuiInputFn GetGuiInput()
		{
			if (!IsRequested())
				return nullptr;
			const auto module = GetModuleHandleW(L"dxvk_d3d11.dll");
			return module ? reinterpret_cast<GuiInputFn>(GetProcAddress(module, "csRemixGuiInput")) : nullptr;
		}
		remixapi_Interface api{};
		using RenderFn = remixapi_ErrorCode(REMIXAPI_CALL*)(ID3D11Texture2D*);
		RenderFn render = nullptr;
		remixapi_MaterialHandle material = nullptr;
		remixapi_MeshHandle mesh = nullptr;
		remixapi_LightHandle sunlight = nullptr;
		bool attempted = false;
		bool ready = false;
		uint32_t diagnosticFrame = 0;
		bool drawingUI = false;
		bool pendingRender = false;
		std::atomic<bool> useDepth{ false };
		std::atomic<bool> useScene{ false };
		std::atomic<bool> suppressWorld{ false };
		// Session-only reference capture. Never enabled by the test setup or saved.
		std::atomic<bool> nativeReference{ false };
		constexpr uint64_t pairedCaptureBit = uint64_t{ 1 } << 32;
		std::atomic<uint64_t> bufferCaptureRequest{ 0 };
		std::atomic<uint32_t> pairedCaptureActive{ 0 };
		uint32_t pairedCaptureFrame = 0;
		bool pairedNativeCaptured = false;
		remixapi_MeshHandle depthMesh = nullptr;
		Microsoft::WRL::ComPtr<ID3D11Texture2D> depthReadback;

		std::filesystem::path BufferCaptureStem(uint32_t request, bool native)
		{
			const auto directory = std::filesystem::path("Screenshots") / "RemixBuffers";
			std::filesystem::create_directories(directory);
			return directory / std::format("{}-{}-{}-{}", native ? "native" : "rtx",
				GetCurrentProcessId(), request, globals::state->frameCountAtomic.load());
		}

		nlohmann::json BufferCaptureMetadata(uint32_t request, bool native)
		{
			const auto eye = globals::game::shadowState->GetRuntimeData().cameraData.getEye();
			std::array<float, 16> view{}, projection{};
			std::memcpy(view.data(), &eye.viewMat, sizeof(view));
			std::memcpy(projection.data(), &eye.projMat, sizeof(projection));
			const auto origin = globals::game::shadowState->GetRuntimeData().posAdjust.getEye();
			const auto viewport = globals::game::shadowState->GetRuntimeData().viewPort;
			return { { "request", request }, { "pid", GetCurrentProcessId() },
				{ "frame", globals::state->frameCountAtomic.load() }, { "nativeReference", native },
				{ "pairedCapture", pairedCaptureActive.load() == request },
				{ "grassLightingLoaded", globals::features::grassLighting.loaded },
				{ "nativePreparation", KeepNativePreparation() },
				{ "boundary", native ? "end-deferred-before-water" : "before-ui-remix-submit" },
				{ "shadowView", view }, { "shadowProjection", projection },
				{ "shadowOrigin", { origin.x, origin.y, origin.z } },
				{ "shadowViewport", { viewport.TopLeftX, viewport.TopLeftY, viewport.Width, viewport.Height, viewport.MinDepth, viewport.MaxDepth } },
				{ "note", "Raw DDS values, no display conversion. Record DevBench camera separately; RTX shadow matrices here are not its submitted camera." } };
		}

		void WriteBufferMetadata(const std::filesystem::path& stem, const nlohmann::json& metadata)
		{
			std::ofstream stream(stem.string() + ".json");
			stream << metadata.dump(2);
			if (!stream)
				throw std::runtime_error("Could not write buffer capture metadata");
			logger::info("[Remix.buffers] {}", stem.string());
		}

		bool HasWorldScene()
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			return player && player->GetParentCell() &&
				!globals::state->IsMainOrLoadingMenuOpen(RE::UI::GetSingleton());
		}

		bool BuildDepthMesh(remixapi_CameraInfo& camera)
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!player || !player->GetParentCell())
				return false;
			auto* source = globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY].texture;
			if (!source)
				return false;
			D3D11_TEXTURE2D_DESC desc{};
			source->GetDesc(&desc);
			const bool floatDepth = desc.Format == DXGI_FORMAT_R32_TYPELESS || desc.Format == DXGI_FORMAT_D32_FLOAT;
			const bool packedDepth = desc.Format == DXGI_FORMAT_R24G8_TYPELESS || desc.Format == DXGI_FORMAT_D24_UNORM_S8_UINT;
			if ((!floatDepth && !packedDepth) || desc.SampleDesc.Count != 1 || desc.ArraySize != 1)
				return false;
			if (depthReadback) {
				D3D11_TEXTURE2D_DESC old{};
				depthReadback->GetDesc(&old);
				if (old.Width != desc.Width || old.Height != desc.Height || old.Format != desc.Format)
					depthReadback.Reset();
			}
			if (!depthReadback) {
				desc.BindFlags = 0;
				desc.MiscFlags = 0;
				desc.Usage = D3D11_USAGE_STAGING;
				desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
				if (FAILED(globals::d3d::device->CreateTexture2D(&desc, nullptr, depthReadback.GetAddressOf())))
					return false;
				Util::SetResourceName(depthReadback.Get(), "Remix Depth Reconstruction Readback");
				logger::info("[Remix] depth reconstruction {}x{} format {}", desc.Width, desc.Height, static_cast<int>(desc.Format));
			}
			globals::d3d::context->CopyResource(depthReadback.Get(), source);
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (FAILED(globals::d3d::context->Map(depthReadback.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
				return false;
			const auto eye = globals::game::shadowState->GetRuntimeData().cameraData.getEye();
			const auto inverseProjection = eye.projMat.Invert();
			constexpr uint32_t step = 8;
			const uint32_t cols = (desc.Width - 1) / step + 1;
			const uint32_t rows = (desc.Height - 1) / step + 1;
			std::vector<remixapi_HardcodedVertex> vertices(cols * rows);
			std::vector<bool> valid(vertices.size());
			for (uint32_t y = 0; y < rows; ++y) {
				for (uint32_t x = 0; x < cols; ++x) {
					const auto* pixel = static_cast<const uint8_t*>(mapped.pData) + y * step * mapped.RowPitch + x * step * 4;
					float depth;
					if (floatDepth) {
						std::memcpy(&depth, pixel, sizeof(depth));
					} else {
						uint32_t packed;
						std::memcpy(&packed, pixel, sizeof(packed));
						depth = float(packed & 0xffffffu) / 16777215.0f;
					}
					const auto index = y * cols + x;
					if (!std::isfinite(depth) || depth <= 0 || depth >= 0.9999999f)
						continue;
					const float4 clip{ 2.0f * (x * step + 0.5f) / desc.Width - 1.0f, 1.0f - 2.0f * (y * step + 0.5f) / desc.Height, depth, 1.0f };
					const auto p = float4::Transform(clip, inverseProjection);
					if (!std::isfinite(p.w) || std::abs(p.w) < 1e-8f)
						continue;
					auto& vertex = vertices[index];
					vertex.position[0] = p.x / p.w;
					vertex.position[1] = p.y / p.w;
					vertex.position[2] = p.z / p.w;
					vertex.normal[2] = -1.0f;
					vertex.color = 0xffffffff;
					valid[index] = std::isfinite(vertex.position[2]);
				}
			}
			globals::d3d::context->Unmap(depthReadback.Get(), 0);
			std::vector<uint32_t> indices;
			indices.reserve((cols - 1) * (rows - 1) * 6);
			auto triangle = [&](uint32_t a, uint32_t b, uint32_t c) {
				if (!valid[a] || !valid[b] || !valid[c])
					return;
				const float za = std::abs(vertices[a].position[2]), zb = std::abs(vertices[b].position[2]), zc = std::abs(vertices[c].position[2]);
				if (std::max({ za, zb, zc }) > std::min({ za, zb, zc }) * 1.2f)
					return;
				indices.insert(indices.end(), { a, b, c });
			};
			for (uint32_t y = 0; y + 1 < rows; ++y)
				for (uint32_t x = 0; x + 1 < cols; ++x) {
					const uint32_t a = y * cols + x;
					triangle(a, a + cols, a + 1);
					triangle(a + 1, a + cols, a + cols + 1);
				}
			if (indices.empty())
				return false;
			if (depthMesh)
				api.DestroyMesh(depthMesh);
			depthMesh = nullptr;
			remixapi_MeshInfoSurfaceTriangles surface{};
			surface.vertices_values = vertices.data();
			surface.vertices_count = static_cast<uint32_t>(vertices.size());
			surface.indices_values = indices.data();
			surface.indices_count = static_cast<uint32_t>(indices.size());
			surface.material = material;
			remixapi_MeshInfo info{};
			info.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
			info.hash = 0x4353444550544800ull + diagnosticFrame;
			info.surfaces_count = 1;
			info.surfaces_values = &surface;
			if (api.CreateMesh(&info, &depthMesh) != REMIXAPI_ERROR_CODE_SUCCESS)
				return false;
			camera.pNext = nullptr;
			std::memcpy(camera.projection, &eye.projMat, sizeof(camera.projection));
			camera.view[0][0] = camera.view[1][1] = camera.view[2][2] = camera.view[3][3] = 1.0f;
			if (diagnosticFrame % 120 == 0)
				logger::info("[Remix] depth mesh: {} vertices, {} triangles", vertices.size(), indices.size() / 3);
			return true;
		}

		void Probe(ID3D11Texture2D* source)
		{
			if (!source) {
				logger::info("[Remix] probe target is null");
				return;
			}
			D3D11_TEXTURE2D_DESC desc{};
			source->GetDesc(&desc);
			logger::info("[Remix] probe target {}x{} format {}", desc.Width, desc.Height, static_cast<int>(desc.Format));
			Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
			globals::d3d::context->OMGetRenderTargets(1, rtv.GetAddressOf(), nullptr);
			if (rtv) {
				Microsoft::WRL::ComPtr<ID3D11Resource> resource;
				rtv->GetResource(resource.GetAddressOf());
				logger::info("[Remix] bound UI target matches backbuffer: {}", resource.Get() == source);
			}
			desc.Usage = D3D11_USAGE_STAGING;
			desc.BindFlags = 0;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			desc.MiscFlags = 0;
			Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
			if (FAILED(globals::d3d::device->CreateTexture2D(&desc, nullptr, staging.GetAddressOf())))
				return;
			Util::SetResourceName(staging.Get(), "Remix Diagnostic Readback");
			globals::d3d::context->CopyResource(staging.Get(), source);
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (SUCCEEDED(globals::d3d::context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
				const auto* center = static_cast<const uint8_t*>(mapped.pData) + mapped.RowPitch * (desc.Height / 2) + (desc.Width / 2) * 4;
				logger::info("[Remix] post-render center bytes: {} {} {} {}", center[0], center[1], center[2], center[3]);
				globals::d3d::context->Unmap(staging.Get(), 0);
			}
		}

		bool Check(remixapi_ErrorCode code, const char* operation)
		{
			if (code == REMIXAPI_ERROR_CODE_SUCCESS)
				return true;
			logger::error("[Remix] {} failed ({})", operation, static_cast<int>(code));
			return false;
		}

		bool Initialize()
		{
			const auto module = GetModuleHandleW(L"dxvk_d3d11.dll");
			if (!module || !DxvkLoader::IsLoaded())
				return false;
			const auto initialize = reinterpret_cast<PFN_remixapi_InitializeLibrary>(GetProcAddress(module, "remixapi_InitializeLibrary"));
			render = reinterpret_cast<RenderFn>(GetProcAddress(module, "csRemixRender"));
			if (!initialize || !render) {
				logger::error("[Remix] API-only Vulkan runtime exports missing; diagnostic disabled");
				return false;
			}
			remixapi_InitializeLibraryInfo library{};
			library.sType = REMIXAPI_STRUCT_TYPE_INITIALIZE_LIBRARY_INFO;
			library.version = REMIXAPI_VERSION_MAKE(0, 6, 2);
			if (!Check(initialize(&library, &api), "InitializeLibrary") ||
				!Check(api.dxvk_RegisterD3D11Device(globals::d3d::device), "RegisterD3D11Device"))
				return false;

			// Only what this integration genuinely needs differs from Remix's
			// defaults: Skyrim's normal maps are XYZ. The requested 1/0.65 diffuse
			// brightness boost is artistic, not a texture colour-space correction.
			// Everything else -- RTXDI, ReSTIR GI, volumetrics,
			// bounce counts, the denoiser -- is left at the defaults.
			//
			// rtx.debugView.debugViewIdx used to be set to 32 (DEBUG_VIEW_RAW_ALBEDO)
			// here, left over from bringing the integration up against a known
			// material. A non-zero index is what turns the debug view on.
			api.SetConfigVariable("rtx.enableNearPlaneOverride", "False");
			api.SetConfigVariable("rtx.enableRaytracing", "True");
			api.SetConfigVariable("rtx.viewModel.enable", "True");
			api.SetConfigVariable("rtx.viewModel.scale", "1.0");
			// Remix sceneScale is game units per centimeter; geometry stays in Skyrim units.
			const float metersToWorldUnits = RE::bhkWorld::GetWorldScaleInverse();
			if (!std::isfinite(metersToWorldUnits) || metersToWorldUnits <= 0.0f) {
				logger::error("[Remix] Invalid engine world scale: {}", metersToWorldUnits);
				return false;
			}
			const auto sceneScale = std::to_string(metersToWorldUnits * 0.01f);
			if (!Check(api.SetConfigVariable("rtx.sceneScale", sceneScale.c_str()), "SetSceneScale"))
				return false;
			logger::info("[Remix] World units per meter: {}; sceneScale: {}", metersToWorldUnits, sceneScale);
			api.SetConfigVariable("rtx.opaqueMaterial.normalMapsAreXYZ", "True");
			api.SetConfigVariable("rtx.opaqueMaterial.albedoScale", "1.53846153846");
			remixapi_MaterialInfoOpaqueEXT opaque{};
			opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
			opaque.albedoConstant = { 0.1f, 0.8f, 0.3f };
			opaque.opacityConstant = 1.0f;
			// Remix uses Vulkan compare-op values: zero means NEVER, seven ALWAYS.
			opaque.alphaTestType = 7;
			opaque.roughnessConstant = 0.8f;
			remixapi_MaterialInfo info{};
			info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
			info.pNext = &opaque;
			info.hash = 0x435352454d495801ull;
			info.spriteSheetRow = info.spriteSheetCol = 1;
			info.emissiveIntensity = 1.0f;
			info.emissiveColorConstant = { 0.1f, 0.8f, 0.3f };
			if (!Check(api.CreateMaterial(&info, &material), "CreateMaterial"))
				return false;
			remixapi_HardcodedVertex vertices[3]{};
			vertices[0].position[0] = -1.0f; vertices[0].position[1] = -1.0f;
			vertices[1].position[0] = 1.0f; vertices[1].position[1] = -1.0f;
			vertices[2].position[1] = 1.0f;
			for (auto& vertex : vertices) {
				vertex.position[2] = 3.0f;
				vertex.normal[2] = -1.0f;
				vertex.color = 0xffffffff;
			}
			const uint32_t indices[]{ 0, 2, 1 };
			remixapi_MeshInfoSurfaceTriangles surface{};
			surface.vertices_values = vertices;
			surface.vertices_count = 3;
			surface.indices_values = indices;
			surface.indices_count = 3;
			surface.material = material;
			remixapi_MeshInfo meshInfo{};
			meshInfo.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
			meshInfo.hash = 0x435352454d495802ull;
			meshInfo.surfaces_values = &surface;
			meshInfo.surfaces_count = 1;
			if (!Check(api.CreateMesh(&meshInfo, &mesh), "CreateMesh")) {
				api.DestroyMaterial(material);
				material = nullptr;
				return false;
			}
			remixapi_LightInfoDistantEXT distant{};
			distant.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT;
			distant.direction = { 0, 0, 1 };
			distant.angularDiameterDegrees = 0.5f;
			remixapi_LightInfo light{};
			light.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
			light.pNext = &distant;
			light.hash = 0x435353554e000001ull;
			light.radiance = { 5, 5, 5 };
			if (!Check(api.CreateLight(&light, &sunlight), "CreateLight"))
				return false;
			if (!RemixScene::Initialize(&api))
				return false;
			logger::info("[Remix] API registered on Skyrim's D3D11/Vulkan device (no D3D9)");
			return true;
		}
	}

	bool IsRequested()
	{
		static const bool enabled = [] {
			char value[8]{};
			return GetEnvironmentVariableA("CS_REMIX_TEST", value, sizeof(value)) && value[0] == '1';
		}();
		return enabled;
	}

	void SetMenuFocus(bool focused)
	{
		if (const auto input = GetGuiInput())
			input(5, 0, focused ? 1.0f : 0.0f);
	}

	bool ProcessMenuInput(RE::InputEvent* const* events)
	{
		const auto input = GetGuiInput();
		if (!input)
			return false;
		for (auto* event = events ? *events : nullptr; event; event = event->next) {
			if (const auto* character = event->AsCharEvent()) {
				input(4, character->keyCode, 0);
				continue;
			}
			const auto* button = event->AsButtonEvent();
			if (!button || (!button->IsDown() && !button->IsUp()))
				continue;
			const uint32_t code = button->GetIDCode();
			const float pressed = button->Value() > 0 ? 1.0f : 0.0f;
			if (button->GetDevice() == RE::INPUT_DEVICE::kKeyboard) {
				uint32_t key = Util::Input::DIKToVK(code);
				if (key == code)
					key = MapVirtualKeyEx(code, MAPVK_VSC_TO_VK_EX, GetKeyboardLayout(0));
				input(1, key, pressed);
			} else if (button->GetDevice() == RE::INPUT_DEVICE::kMouse) {
				if (code < 5)
					input(2, code, pressed);
				else if ((code == 8 || code == 9) && pressed > 0)
					input(3, 0, code == 8 ? button->Value() : -button->Value());
			}
		}
		return input(0, 0, 0) != 0;
	}

	bool SetConfig(const std::string& name, const std::string& value)
	{
		if (!ready)
			return false;
		if (name == "cs.treeVertexBakedLighting") {
			const bool enabled = value == "1" || _stricmp(value.c_str(), "true") == 0;
			if (!enabled && value != "0" && _stricmp(value.c_str(), "false") != 0)
				return false;
			RemixScene::SetTreeVertexBakedLighting(enabled);
			logger::info("[Remix] tree vertex baked lighting={}", enabled);
			return true;
		}
		if (name == "cs.directionalLightScale") {
			float scale = 0;
			const auto parsed = std::from_chars(value.data(), value.data() + value.size(), scale);
			if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || !RemixScene::SetDirectionalLightScale(scale))
				return false;
			logger::info("[Remix] directional radiance scale={}", scale);
			return true;
		}
		if (name == "cs.captureBuffers" || name == "cs.captureBufferPair") {
			const bool pair = name == "cs.captureBufferPair";
			uint32_t request = 0;
			const auto parsed = std::from_chars(value.data(), value.data() + value.size(), request);
			if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || !request || !HasWorldScene())
				return false;
			if (!nativeReference.load() && !GetProcAddress(GetModuleHandleW(L"dxvk_d3d11.dll"), "csRemixCaptureBuffers"))
				return false;
			if (pair && (!SuppressWorld() || !KeepNativePreparation()))
				return false;
			uint64_t expected = 0;
			return bufferCaptureRequest.compare_exchange_strong(expected, uint64_t(request) | (pair ? pairedCaptureBit : 0));
		}
		if (name == "cs.sceneAudit") {
			const bool enabled = value == "1" || _stricmp(value.c_str(), "true") == 0;
			if (!enabled && value != "0" && _stricmp(value.c_str(), "false") != 0)
				return false;
			RemixScene::SetAuditEnabled(enabled);
			return true;
		}
		if (name == "cs.strayTreeGroupGuard" || name == "cs.injectStrayTreeGroups") {
			const bool enabled = value == "1" || _stricmp(value.c_str(), "true") == 0;
			if (!enabled && value != "0" && _stricmp(value.c_str(), "false") != 0)
				return false;
			if (name == "cs.strayTreeGroupGuard")
				RemixScene::SetStrayTreeGroupGuard(enabled);
			else
				RemixScene::SetStrayTreeGroupInjection(enabled);
			logger::info("[Remix] {}={}", name, enabled);
			return true;
		}
		if (name == "cs.respectDistantTreeCulling") {
			const bool enabled = value == "1" || _stricmp(value.c_str(), "true") == 0;
			if (!enabled && value != "0" && _stricmp(value.c_str(), "false") != 0)
				return false;
			RemixScene::SetDistantTreeCullingRespected(enabled);
			logger::info("[Remix] {}={}", name, enabled);
			return true;
		}
		auto* toggle = name == "cs.suppressWorld" ? &suppressWorld : name == "cs.scene" ? &useScene : name == "cs.depth" ? &useDepth : name == "cs.nativeReference" ? &nativeReference : nullptr;
		if (toggle) {
			const bool enabled = value == "1" || _stricmp(value.c_str(), "true") == 0;
			if (!enabled && value != "0" && _stricmp(value.c_str(), "false") != 0)
				return false;
			// Native rasterization can replace resources while retained RT work is
			// paused. Until the resume lifetime is validated, reference mode is
			// one-way for this process; restart to return to Remix safely.
			if (toggle == &nativeReference && !enabled && nativeReference.load()) {
				logger::warn("[Remix] native reference mode requires a test-process restart to resume Remix");
				return false;
			}
			toggle->store(enabled);
			logger::info("[Remix] {}={}", name, enabled);
			return true;
		}
		if (name == "cs.graphicsPreset") {
			// The preset drives bounce counts, denoiser separation, neural cache
			// quality and volumetrics, and Remix only applies it during its own
			// initialization; setting the option alone has no effect here.
			static const auto applyPreset = reinterpret_cast<remixapi_ErrorCode(REMIXAPI_CALL*)(uint32_t)>(
				GetProcAddress(GetModuleHandleW(L"dxvk_d3d11.dll"), "csRemixApplyGraphicsPreset"));
			uint32_t preset = 0;
			const auto parsed = std::from_chars(value.data(), value.data() + value.size(), preset);
			if (!applyPreset || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
				return false;
			return Check(applyPreset(preset), "csRemixApplyGraphicsPreset");
		}
		return ready && name.starts_with("rtx.") && Check(api.SetConfigVariable(name.c_str(), value.c_str()), "SetConfigVariable");
	}

	bool SuppressWorld()
	{
		return ready && !nativeReference.load() && useScene.load() && suppressWorld.load() && HasWorldScene();
	}

	namespace
	{
		std::atomic<bool> suppressThisFrame{ false };
	}

	bool LatchSuppressWorld(bool suppress)
	{
		if (const auto abandoned = pairedCaptureActive.exchange(0)) {
			uint64_t expected = uint64_t(abandoned) | pairedCaptureBit;
			bufferCaptureRequest.compare_exchange_strong(expected, 0);
			logger::warn("[Remix.buffers] Paired request {} missed its UI boundary; discarded", abandoned);
		}
		pairedNativeCaptured = false;
		const auto request = bufferCaptureRequest.load();
		if (suppress && (request & pairedCaptureBit)) {
			pairedCaptureFrame = globals::state->frameCountAtomic.load();
			pairedCaptureActive.store(static_cast<uint32_t>(request));
			suppress = false;
			logger::info("[Remix.buffers] Paired request {} native world frame {}", static_cast<uint32_t>(request), pairedCaptureFrame);
		}
		suppressThisFrame.store(suppress, std::memory_order_relaxed);
		return suppress;
	}

	bool KeepNativePreparation()
	{
		static const bool enabled = [] {
			char value[8]{};
			return IsRequested() && GetEnvironmentVariableA("CS_REMIX_KEEP_NATIVE_PREPARATION", value, sizeof(value)) && value[0] == '1';
		}();
		return enabled;
	}

	bool CapturingReferencePairThisFrame()
	{
		return pairedCaptureActive.load() != 0;
	}

	bool SuppressWorldThisFrame()
	{
		return suppressThisFrame.load(std::memory_order_relaxed);
	}

	bool DispatchMaterialBake(ID3D11DeviceContext* context, uint32_t x, uint32_t y)
	{
		if (!context || context == globals::d3d::context || context->GetType() != D3D11_DEVICE_CONTEXT_DEFERRED)
			return false;
		context->Dispatch(x, y, 1);
		return true;
	}

	void CaptureReferenceBuffers()
	{
		const auto paired = pairedCaptureActive.load();
		if ((!nativeReference.load() && !paired) || !globals::state->inWorld || !bufferCaptureRequest.load())
			return;
		if (paired && (pairedNativeCaptured || pairedCaptureFrame != globals::state->frameCountAtomic.load()))
			return;
		const auto request = paired ? paired : static_cast<uint32_t>(bufferCaptureRequest.exchange(0));
		if (!request)
			return;
		try {
			const auto stem = BufferCaptureStem(request, true);
			auto metadata = BufferCaptureMetadata(request, true);
			auto& targets = globals::game::renderer->GetRuntimeData().renderTargets;
			auto* depth = globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN].texture;
			const std::array<std::pair<const char*, ID3D11Texture2D*>, 3> sources{ {
				{ "albedo", targets[ALBEDO].texture }, { "normalRoughness", targets[NORMALROUGHNESS].texture }, { "depth", depth } } };
			for (const auto& [name, texture] : sources) {
				if (!texture)
					throw std::runtime_error("Native reference buffer missing");
				D3D11_TEXTURE2D_DESC desc{};
				texture->GetDesc(&desc);
				const auto path = stem.string() + "-" + name + ".dds";
				const auto hr = Util::SaveTextureToFile(globals::d3d::device, globals::d3d::context, path, texture);
				metadata["buffers"][name] = { { "path", path }, { "width", desc.Width }, { "height", desc.Height },
					{ "format", static_cast<uint32_t>(desc.Format) }, { "hresult", static_cast<int32_t>(hr) } };
				if (FAILED(hr))
					throw std::runtime_error("Native reference buffer readback failed");
			}
			WriteBufferMetadata(stem, metadata);
			pairedNativeCaptured = paired != 0;
		} catch (const std::exception& error) {
			logger::error("[Remix.buffers] Native capture failed: {}", error.what());
		}
	}

	void CaptureGeometry(RE::BSGeometry* geometry, const RE::NiPoint3* grassScale)
	{
		// Raster passes supply only the native grass ScaleMask hint. They no
		// longer discover objects or determine the retained scene's lifetime.
		if (grassScale && ready && useScene.load() && HasWorldScene())
			RemixScene::RecordGrassScale(geometry, *grassScale);
	}

	void CaptureGrassWind(RE::BSGeometry* geometry, const RE::NiPoint3& wind, float timer, float previousTimer)
	{
		if (ready && useScene.load() && HasWorldScene())
			RemixScene::RecordGrassWind(geometry, wind, timer, previousTimer);
	}

	void CaptureTreeWind(const float* treeParams, const float* windTimers)
	{
		if (ready && useScene.load() && HasWorldScene())
			RemixScene::RecordTreeWind(treeParams, windTimers);
	}

	void ComposeBeforeMenus()
	{
		if (!ready || !drawingUI || !pendingRender)
			return;
		Microsoft::WRL::ComPtr<ID3D11RenderTargetView> view;
		view = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kFRAMEBUFFER].RTV;
		if (!view)
			return;
		Microsoft::WRL::ComPtr<ID3D11Resource> resource;
		Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
		view->GetResource(resource.GetAddressOf());
		resource.As(&texture);
		pendingRender = false;
		// The host's own work and the runtime's are on different threads, and the
		// frame is close to their sum rather than their maximum. Time the call
		// that hands the frame over, and the interval between handovers, to see
		// how much of the game thread is spent waiting rather than working.
		using Clock = std::chrono::steady_clock;
		static Clock::time_point previousCompose;
		static double composeTotalMs = 0, intervalTotalMs = 0;
		static uint32_t composeSamples = 0;
		const auto composeStart = Clock::now();
		if (previousCompose.time_since_epoch().count())
			intervalTotalMs += std::chrono::duration<double, std::milli>(composeStart - previousCompose).count();
		previousCompose = composeStart;

		ready = texture && Check(render(texture.Get()), "Render at game UI boundary");

		composeTotalMs += std::chrono::duration<double, std::milli>(Clock::now() - composeStart).count();
		if (++composeSamples >= 120) {
			logger::info("[Remix.compose] mean ms inside the render handover {:.3f}, mean frame interval {:.3f} (120 frames)",
				composeTotalMs / composeSamples, intervalTotalMs / composeSamples);
			composeTotalMs = intervalTotalMs = 0;
			composeSamples = 0;
		}
		if (diagnosticFrame <= 3)
			logger::info("[Remix] composed after native Scaleform setup, before menu rendering");
		if (diagnosticFrame == 120 || diagnosticFrame == 240)
			Probe(texture.Get());
	}

	void BeforeUI()
	{
		drawingUI = pendingRender = false;
		if (!IsRequested() || !globals::d3d::device || !globals::d3d::swapChain)
			return;
		if (!attempted) {
			attempted = true;
			ready = Initialize();
		}
		if (!ready)
			return;
		// Reconciled every frame rather than only on the transition, so that the
		// game rewriting these -- a graphics settings change, a save load -- is
		// corrected on the next one. It costs a compare while already in state.
		RemixSky::SetReflectionLodEnabled(!SuppressWorld());
		// Main/loading menus and their 3D previews remain entirely native.
		// Never carry a loading-screen geometry list into the next world frame.
		if (!HasWorldScene()) {
			RemixScene::DiscardFrame();
			return;
		}
		if (nativeReference.load())
			return;
		if (useScene.load()) {
			// A loading/menu transition may have no world-camera snapshot yet.
			// This frame's readiness is not the lifetime of the initialized API:
			// clearing `ready` here permanently disabled Remix after one miss.
			const bool sceneReady = RemixScene::Submit();
			// After the scene, so a frame that fails to submit geometry does not
			// leave a sky standing over nothing.
			const bool domeSubmitted = sceneReady && RemixSky::Submit();
			if (sceneReady) {
				const auto ticket = bufferCaptureRequest.load();
				const bool paired = (ticket & pairedCaptureBit) != 0;
				// A pair queued after the world latch belongs to the NEXT world frame.
				if (ticket && (!paired || pairedCaptureActive.load() == static_cast<uint32_t>(ticket))) {
					const auto request = static_cast<uint32_t>(ticket);
					try {
						using CaptureFn = remixapi_ErrorCode(REMIXAPI_CALL*)();
						const auto capture = reinterpret_cast<CaptureFn>(GetProcAddress(GetModuleHandleW(L"dxvk_d3d11.dll"), "csRemixCaptureBuffers"));
						auto metadata = BufferCaptureMetadata(request, false);
						const bool nativeValid = !paired || (pairedNativeCaptured && pairedCaptureFrame == globals::state->frameCountAtomic.load());
						metadata["pairedNativeCaptured"] = paired && nativeValid;
						metadata["queued"] = nativeValid && capture && Check(capture(), "Queue raw Remix buffer capture");
						WriteBufferMetadata(BufferCaptureStem(request, false), metadata);
					} catch (const std::exception& error) {
						logger::error("[Remix.buffers] RTX capture failed: {}", error.what());
					}
					bufferCaptureRequest.store(0);
					pairedCaptureActive.store(0);
				}
			}
			RemixScene::RecordAudit(sceneReady, domeSubmitted);
			++diagnosticFrame;
			drawingUI = pendingRender = sceneReady;
			return;
		}
		Microsoft::WRL::ComPtr<ID3D11Texture2D> backbuffer;
		if (FAILED(globals::d3d::swapChain->GetBuffer(0, IID_PPV_ARGS(backbuffer.GetAddressOf()))))
			return;
		// Skyrim composes UI into its framebuffer texture before the final swapchain copy.
		auto& framebuffer = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kFRAMEBUFFER];
		if (framebuffer.RTV) {
			Microsoft::WRL::ComPtr<ID3D11Resource> resource;
			framebuffer.RTV->GetResource(resource.GetAddressOf());
			resource.As(&backbuffer);
		}
		D3D11_TEXTURE2D_DESC desc{};
		backbuffer->GetDesc(&desc);
		if (!desc.Height)
			return;
		remixapi_CameraInfoParameterizedEXT parameters{};
		parameters.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO_PARAMETERIZED_EXT;
		parameters.forward = { 0, 0, 1 };
		parameters.up = { 0, 1, 0 };
		parameters.right = { 1, 0, 0 };
		parameters.fovYInDegrees = 60;
		parameters.aspect = static_cast<float>(desc.Width) / desc.Height;
		parameters.nearPlane = 0.1f;
		parameters.farPlane = 100.0f;
		remixapi_CameraInfo camera{};
		camera.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
		camera.pNext = &parameters;
		camera.type = REMIXAPI_CAMERA_TYPE_WORLD;
		remixapi_InstanceInfo instance{};
		instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
		instance.mesh = mesh;
		if (useDepth.load() && BuildDepthMesh(camera))
			instance.mesh = depthMesh;
		instance.doubleSided = true;
		instance.transform.matrix[0][0] = instance.transform.matrix[1][1] = instance.transform.matrix[2][2] = 1.0f;
		ready = Check(api.SetupCamera(&camera), "SetupCamera") &&
			Check(api.DrawInstance(&instance), "DrawInstance") &&
			Check(api.DrawLightInstance(sunlight), "DrawLightInstance");
		++diagnosticFrame;
		drawingUI = pendingRender = ready;
	}

	void AfterUI()
	{
		drawingUI = false;
		if (!ready || (diagnosticFrame != 120 && diagnosticFrame != 240))
			return;
		logger::info("[Remix] after UI probe");
		auto* rtv = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kFRAMEBUFFER].RTV;
		if (rtv) {
			Microsoft::WRL::ComPtr<ID3D11Resource> resource;
			Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
			rtv->GetResource(resource.GetAddressOf());
			resource.As(&texture);
			Probe(texture.Get());
		}
	}

	void BeforePresent()
	{
		if (!ready || (diagnosticFrame != 120 && diagnosticFrame != 240))
			return;
		Microsoft::WRL::ComPtr<ID3D11Texture2D> backbuffer;
		if (SUCCEEDED(globals::d3d::swapChain->GetBuffer(0, IID_PPV_ARGS(backbuffer.GetAddressOf())))) {
			logger::info("[Remix] before Present probe");
			Probe(backbuffer.Get());
		}
	}
}
