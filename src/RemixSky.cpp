#include "RemixSky.h"

#include "Globals.h"
#include "Util.h"

#include <remix/remix_c.h>

namespace
{
	// Equirectangular, matching what Remix's dome light samples. Wide enough
	// that the horizon -- where the eye actually looks -- is not softer than the
	// 256-pixel cube face it came from, without paying for a resolve the sky
	// cannot fill with detail.
	constexpr UINT kLatLongWidth = 2048;
	constexpr UINT kLatLongHeight = 1024;

	using SetSkyDomeFn = remixapi_ErrorCode(REMIXAPI_CALL*)(ID3D11ShaderResourceView*, const remixapi_Transform*, const float*);

	eastl::unique_ptr<Texture2D> g_latLong;
	winrt::com_ptr<ID3D11ComputeShader> g_resolveCS;
	winrt::com_ptr<ID3D11SamplerState> g_sampler;
	SetSkyDomeFn g_setSkyDome = nullptr;
	bool g_warnedMissingExport = false;

	ID3D11ComputeShader* GetResolveCS()
	{
		if (!g_resolveCS) {
			std::vector<std::pair<const char*, const char*>> defines;
			// Replaces the resolved sky with a known gradient, to tell a dome that
			// never reaches the screen from a cubemap that holds nothing.
			if (GetEnvironmentVariableW(L"CS_REMIX_SKY_CONSTANT", nullptr, 0) != 0) {
				defines.push_back({ "SKY_PROBE_CONSTANT", nullptr });
			}
			g_resolveCS.attach(static_cast<ID3D11ComputeShader*>(
				Util::CompileShader(L"Data\\Shaders\\Remix\\SkyLatLongCS.hlsl", defines, "cs_5_0")));
		}
		return g_resolveCS.get();
	}

	bool EnsureResources()
	{
		if (!g_setSkyDome) {
			g_setSkyDome = reinterpret_cast<SetSkyDomeFn>(
				GetProcAddress(GetModuleHandleW(L"dxvk_d3d11.dll"), "csRemixSetSkyDome"));
			if (!g_setSkyDome) {
				if (!g_warnedMissingExport) {
					g_warnedMissingExport = true;
					logger::warn("[RemixSky] runtime has no csRemixSetSkyDome; sky will not be submitted");
				}
				return false;
			}
		}

		if (!g_latLong) {
			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = kLatLongWidth;
			desc.Height = kLatLongHeight;
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			// Half float: the sky carries the sun's own brightness, which does not
			// fit the 0-1 the game's cube would be read back at in 8 bits.
			desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
			desc.SampleDesc.Count = 1;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
			g_latLong = eastl::make_unique<Texture2D>(desc, "RemixSky::LatLong");

			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
			srvDesc.Format = desc.Format;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			srvDesc.Texture2D.MipLevels = 1;
			g_latLong->CreateSRV(srvDesc);

			D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
			uavDesc.Format = desc.Format;
			uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
			g_latLong->CreateUAV(uavDesc);
		}

		if (!g_sampler) {
			D3D11_SAMPLER_DESC desc{};
			desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
			// A cube lookup never runs off the edge of a face, so the address
			// mode only matters at the seams, where clamping is what the
			// hardware's own cube filtering expects.
			desc.AddressU = desc.AddressV = desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
			desc.MaxLOD = D3D11_FLOAT32_MAX;
			if (FAILED(globals::d3d::device->CreateSamplerState(&desc, g_sampler.put()))) {
				return false;
			}
		}

		return GetResolveCS() != nullptr;
	}

	// True where the game is showing its sky at all. Interiors and the sky-less
	// modes leave Remix's dome inactive, so an interior is lit by its own lights
	// rather than by whatever the cube happened to hold outside.
	bool HasVisibleSky(const RE::Sky* sky)
	{
		return sky && sky->mode.get() == RE::Sky::Mode::kFull;
	}
}

namespace RemixSky
{
	void Submit()
	{
		static uint32_t frame = 0;
		const bool report = (frame++ % 120) == 0;
		auto skipped = [&](const char* why) {
			if (report) {
				logger::info("[RemixSky] no sky submitted: {}", why);
			}
		};

		auto* sky = globals::game::sky;
		if (!sky) {
			skipped("no sky singleton");
			return;
		}
		if (!HasVisibleSky(sky)) {
			skipped(std::format("sky mode {}", static_cast<int>(sky->mode.get())).c_str());
			return;
		}

		auto* renderer = globals::game::renderer;
		if (!renderer) {
			skipped("no renderer");
			return;
		}

		auto& reflections = renderer->GetRendererData().cubemapRenderTargets[RE::RENDER_TARGETS_CUBEMAP::kREFLECTIONS];
		if (!reflections.SRV) {
			skipped("reflections cubemap has no shader resource view");
			return;
		}

		if (!EnsureResources()) {
			skipped("resolve resources unavailable");
			return;
		}

		if (report) {
			logger::info("[RemixSky] submitting {}x{} sky resolved from the game's reflections cubemap",
				kLatLongWidth, kLatLongHeight);
		}

		auto* context = globals::d3d::context;

		ID3D11ShaderResourceView* srv = reflections.SRV;
		ID3D11UnorderedAccessView* uav = g_latLong->uav.get();
		ID3D11SamplerState* samplers[] = { g_sampler.get() };

		context->CSSetShader(GetResolveCS(), nullptr, 0);
		context->CSSetShaderResources(0, 1, &srv);
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetSamplers(0, 1, samplers);
		context->Dispatch(kLatLongWidth / 8, kLatLongHeight / 8, 1);

		ID3D11ShaderResourceView* nullSRV = nullptr;
		ID3D11UnorderedAccessView* nullUAV = nullptr;
		context->CSSetShaderResources(0, 1, &nullSRV);
		context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
		context->CSSetShader(nullptr, nullptr, 0);

		// Remix's equirectangular mapping is Z-up and so is Skyrim's world, so the
		// image needs no reorientation. The colour is entirely in the texture; the
		// radiance here is the one brightness knob over it.
		remixapi_Transform identity{};
		identity.matrix[0][0] = 1.f;
		identity.matrix[1][1] = 1.f;
		identity.matrix[2][2] = 1.f;

		const float radiance[3] = { 1.f, 1.f, 1.f };

		// Registers and instances in one call: the runtime clears its active dome
		// at the end of every frame, so this has to happen each frame regardless.
		g_setSkyDome(g_latLong->srv.get(), &identity, radiance);
	}

	void Release()
	{
		g_latLong.reset();
		g_resolveCS = nullptr;
		g_sampler = nullptr;
	}
}
