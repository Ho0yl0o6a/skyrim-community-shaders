#pragma once

#include "Globals.h"
#include "RemixBridge.h"
#include "Util.h"
#include <d3dcompiler.h>
#include <wrl/client.h>

namespace RemixMaterial
{
	// Owned resource update replacing the native ISCopySubRegionCS atlas fill.
	inline bool CopyWaterFlowTile(RE::BSGraphics::Texture* source, RE::BSGraphics::Texture* destination,
		const std::array<uint32_t, 8>& rectangle)
	{
		using Microsoft::WRL::ComPtr;
		if (!source || !destination || !source->resourceView || !source->texture || !destination->texture || !globals::d3d::device) return false;
		D3D11_TEXTURE2D_DESC sourceDesc{}, destinationDesc{};
		ComPtr<ID3D11Texture2D> source2D, destination2D;
		if (FAILED(source->texture->QueryInterface(IID_PPV_ARGS(source2D.GetAddressOf()))) ||
			FAILED(destination->texture->QueryInterface(IID_PPV_ARGS(destination2D.GetAddressOf())))) return false;
		source2D->GetDesc(&sourceDesc); destination2D->GetDesc(&destinationDesc);
		D3D11_SHADER_RESOURCE_VIEW_DESC sourceView{};
		source->resourceView->GetDesc(&sourceView);
		if (sourceView.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D || sourceDesc.SampleDesc.Count != 1 || destinationDesc.SampleDesc.Count != 1 ||
			!rectangle[2] || !rectangle[3] || rectangle[6] || rectangle[7] ||
			uint64_t(rectangle[0]) + rectangle[2] > std::max(1u, sourceDesc.Width >> sourceView.Texture2D.MostDetailedMip) ||
			uint64_t(rectangle[1]) + rectangle[3] > std::max(1u, sourceDesc.Height >> sourceView.Texture2D.MostDetailedMip) ||
			uint64_t(rectangle[4]) + rectangle[2] > destinationDesc.Width || uint64_t(rectangle[5]) + rectangle[3] > destinationDesc.Height) return false;
		static ComPtr<ID3D11ComputeShader> shader;
		static ComPtr<ID3D11DeviceContext> deferred;
		if (!shader) {
			constexpr char code[] = R"hlsl(
Texture2D<float4> Source : register(t0);
RWTexture2D<float4> Destination : register(u0);
cbuffer Rectangle : register(b0) { uint2 sourceXY; uint2 size; uint2 destinationXY; uint2 padding; };
[numthreads(8,8,1)] void main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= size)) return;
    Destination[destinationXY + id.xy] = Source.Load(int3(sourceXY + id.xy, 0));
}
)hlsl";
			ComPtr<ID3DBlob> binary, errors;
			if (FAILED(D3DCompile(code, sizeof(code) - 1, "RemixWaterFlowTile", nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
					binary.GetAddressOf(), errors.GetAddressOf())) || FAILED(globals::d3d::device->CreateDeferredContext(0, deferred.GetAddressOf())) ||
				FAILED(globals::d3d::device->CreateComputeShader(binary->GetBufferPointer(), binary->GetBufferSize(), nullptr, shader.GetAddressOf()))) return false;
			Util::SetResourceName(shader.Get(), "Remix Water Flow Atlas Copy");
		}
		ComPtr<ID3D11UnorderedAccessView> output;
		if (FAILED(globals::d3d::device->CreateUnorderedAccessView(destination->texture, nullptr, output.GetAddressOf()))) return false;
		D3D11_BUFFER_DESC bufferDesc{ sizeof(rectangle), D3D11_USAGE_IMMUTABLE, D3D11_BIND_CONSTANT_BUFFER };
		D3D11_SUBRESOURCE_DATA initial{ rectangle.data() };
		ComPtr<ID3D11Buffer> constants;
		if (FAILED(globals::d3d::device->CreateBuffer(&bufferDesc, &initial, constants.GetAddressOf()))) return false;
		deferred->CSSetShader(shader.Get(), nullptr, 0);
		deferred->CSSetShaderResources(0, 1, &source->resourceView);
		deferred->CSSetUnorderedAccessViews(0, 1, output.GetAddressOf(), nullptr);
		deferred->CSSetConstantBuffers(0, 1, constants.GetAddressOf());
		if (!RemixBridge::DispatchMaterialBake(deferred.Get(), (rectangle[2] + 7) / 8, (rectangle[3] + 7) / 8)) return false;
		deferred->ClearState();
		ComPtr<ID3D11CommandList> commands;
		if (FAILED(deferred->FinishCommandList(FALSE, commands.GetAddressOf()))) return false;
		globals::d3d::context->ExecuteCommandList(commands.Get(), TRUE);
		logger::info("[RemixMaterial.waterFlowTile] copied {}x{} ({},{}) -> ({},{}) sourceFormat={} destinationFormat={}", rectangle[2], rectangle[3],
			rectangle[0], rectangle[1], rectangle[4], rectangle[5], static_cast<uint32_t>(sourceView.Format), static_cast<uint32_t>(destinationDesc.Format));
		return true;
	}

	// All calls are serialized by RemixScene's mutex. Do not retain native
	// texture COM wrappers across frames: Skyrim can retire those wrappers
	// during streaming. The caller retains the owned Remix material instead.
	inline Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> BakeCharacterAlbedo(
		const RE::BSLightingShaderMaterialBase* material, ID3D11ShaderResourceView* diffuse)
	{
		using Microsoft::WRL::ComPtr;
		const auto feature = material->GetFeature();
		if (feature != RE::BSShaderMaterial::Feature::kFaceGen && feature != RE::BSShaderMaterial::Feature::kFaceGenRGBTint &&
			feature != RE::BSShaderMaterial::Feature::kHairTint)
			return {};
		struct Parameters
		{
			float tint[3];
			uint32_t mode;
			uint32_t width, height, srgbMask, padding;
		};
		Parameters parameters{ { 1, 1, 1 }, feature == RE::BSShaderMaterial::Feature::kFaceGen ? 1u :
			feature == RE::BSShaderMaterial::Feature::kFaceGenRGBTint ? 2u : 3u };
		std::array<ID3D11ShaderResourceView*, 3> inputs{ diffuse, nullptr, nullptr };
		auto viewOf = [](const RE::NiPointer<RE::NiSourceTexture>& texture) {
			return texture && texture->rendererTexture ? reinterpret_cast<ID3D11ShaderResourceView*>(texture->rendererTexture->resourceView) : nullptr;
		};
		if (parameters.mode == 1) {
			const auto* face = static_cast<const RE::BSLightingShaderMaterialFacegen*>(material);
			inputs[1] = viewOf(face->tintTexture);
			inputs[2] = viewOf(face->detailTexture);
			if (!inputs[1] || !inputs[2])
				return {};
		} else if (parameters.mode == 2) {
			const auto& tint = static_cast<const RE::BSLightingShaderMaterialFacegenTint*>(material)->tintColor;
			parameters.tint[0] = tint.red;
			parameters.tint[1] = tint.green;
			parameters.tint[2] = tint.blue;
		}
		std::array<D3D11_SHADER_RESOURCE_VIEW_DESC, 3> views{};
		const auto addressMode = static_cast<uint32_t>(material->textureClampMode);
		for (uint32_t i = 0; i < inputs.size(); ++i) {
			if (!inputs[i])
				continue;
			inputs[i]->GetDesc(&views[i]);
			if (views[i].ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D)
				return {};
			switch (views[i].Format) {
			case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
			case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
			case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
			case DXGI_FORMAT_BC1_UNORM_SRGB:
			case DXGI_FORMAT_BC2_UNORM_SRGB:
			case DXGI_FORMAT_BC3_UNORM_SRGB:
			case DXGI_FORMAT_BC7_UNORM_SRGB:
				parameters.srgbMask |= 1u << i;
				break;
			default: break;
			}
		}
		ComPtr<ID3D11Texture2D> source;
		ComPtr<ID3D11Resource> sourceResource;
		diffuse->GetResource(sourceResource.GetAddressOf());
		if (FAILED(sourceResource.As(&source)))
			return {};
		D3D11_TEXTURE2D_DESC desc{};
		source->GetDesc(&desc);
		parameters.width = std::max(1u, desc.Width >> views[0].Texture2D.MostDetailedMip);
		parameters.height = std::max(1u, desc.Height >> views[0].Texture2D.MostDetailedMip);
		source.Reset();
		sourceResource.Reset();
		static ComPtr<ID3D11ComputeShader> shader;
		static ComPtr<ID3D11DeviceContext> deferred;
		if (!shader) {
			// Native GetFacegen[RGBTint]BaseColor in Lighting.hlsl operates in
			// authored gamma space. Remix performs the single final decode.
			constexpr char code[] = R"hlsl(
Texture2D<float4> Diffuse : register(t0);
Texture2D<float4> Tint : register(t1);
Texture2D<float4> Detail : register(t2);
SamplerState LinearSampler : register(s0);
RWTexture2D<float4> Output : register(u0);
cbuffer Params : register(b0) { float3 tint; uint mode; uint width; uint height; uint srgbMask; uint padding; };
float3 authored(float3 c, uint bit) {
    if ((srgbMask & bit) == 0) return c;
    return float3(c.r <= 0.0031308 ? 12.92*c.r : 1.055*pow(max(c.r,0),1.0/2.4)-0.055,
                  c.g <= 0.0031308 ? 12.92*c.g : 1.055*pow(max(c.g,0),1.0/2.4)-0.055,
                  c.b <= 0.0031308 ? 12.92*c.b : 1.055*pow(max(c.b,0),1.0/2.4)-0.055);
}
[numthreads(8,8,1)] void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= width || id.y >= height) return;
    float2 uv = (float2(id.xy) + 0.5) / float2(width,height);
    float4 base = Diffuse.SampleLevel(LinearSampler, uv, 0);
    base.rgb = authored(base.rgb, 1);
    // Hair is kept in authored space for its per-vertex tint operation,
    // including when the original view would have decoded sRGB in hardware.
    if (mode == 3) { Output[id.xy] = base; return; }
    float3 tintValue = tint;
    float3 detailValue = float3(1.01171875, 0.99609375, 1.01171875);
    if (mode == 1) {
        tintValue = authored(Tint.SampleLevel(LinearSampler, uv, 0).rgb, 2);
        detailValue = 3.984375 * (float3(0.00392156886,0,0.00392156886) + authored(Detail.SampleLevel(LinearSampler, uv, 0).rgb, 4));
    }
    float3 tinted = tintValue * base.rgb * 2;
    Output[id.xy] = float4((base.rgb * base.rgb + tinted - tinted * base.rgb) * detailValue, base.a);
}
)hlsl";
			ComPtr<ID3DBlob> binary, errors;
			if (FAILED(D3DCompile(code, sizeof(code) - 1, "RemixFaceColor", nullptr, nullptr, "main", "cs_5_0",
					D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, binary.GetAddressOf(), errors.GetAddressOf()))) {
				logger::error("[RemixMaterial] FaceGen shader failed: {}", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no diagnostic");
				return {};
			}
			if (FAILED(globals::d3d::device->CreateDeferredContext(0, deferred.GetAddressOf())) ||
				FAILED(globals::d3d::device->CreateComputeShader(binary->GetBufferPointer(), binary->GetBufferSize(), nullptr, shader.GetAddressOf())))
				return {};
			Util::SetResourceName(shader.Get(), "Remix FaceGen Color Bake");
		}
		D3D11_TEXTURE2D_DESC outputDesc{};
		outputDesc.Width = parameters.width;
		outputDesc.Height = parameters.height;
		outputDesc.ArraySize = 1;
		outputDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		outputDesc.SampleDesc.Count = 1;
		outputDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET;
		outputDesc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
		ComPtr<ID3D11Texture2D> output;
		ComPtr<ID3D11ShaderResourceView> outputView;
		ComPtr<ID3D11UnorderedAccessView> uav;
		if (FAILED(globals::d3d::device->CreateTexture2D(&outputDesc, nullptr, output.GetAddressOf())) ||
			FAILED(globals::d3d::device->CreateShaderResourceView(output.Get(), nullptr, outputView.GetAddressOf())) ||
			FAILED(globals::d3d::device->CreateUnorderedAccessView(output.Get(), nullptr, uav.GetAddressOf())))
			return {};
		Util::SetResourceName(output.Get(), "Remix Composed FaceGen Albedo (Gamma)");
		Util::SetResourceName(outputView.Get(), "Remix Composed FaceGen Albedo SRV");
		Util::SetResourceName(uav.Get(), "Remix Composed FaceGen Albedo UAV");
		D3D11_BUFFER_DESC bufferDesc{ sizeof(parameters), D3D11_USAGE_IMMUTABLE, D3D11_BIND_CONSTANT_BUFFER };
		D3D11_SUBRESOURCE_DATA initial{ &parameters };
		ComPtr<ID3D11Buffer> constants;
		if (FAILED(globals::d3d::device->CreateBuffer(&bufferDesc, &initial, constants.GetAddressOf())))
			return {};
		Util::SetResourceName(constants.Get(), "Remix FaceGen Bake Parameters");
		D3D11_SAMPLER_DESC samplerDesc{};
		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		samplerDesc.AddressU = (addressMode & 2) ? D3D11_TEXTURE_ADDRESS_WRAP : D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.AddressV = (addressMode & 1) ? D3D11_TEXTURE_ADDRESS_WRAP : D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
		ComPtr<ID3D11SamplerState> sampler;
		if (FAILED(globals::d3d::device->CreateSamplerState(&samplerDesc, sampler.GetAddressOf())))
			return {};
		Util::SetResourceName(sampler.Get(), "Remix FaceGen Bake Sampler");
		deferred->CSSetShader(shader.Get(), nullptr, 0);
		deferred->CSSetShaderResources(0, 3, inputs.data());
		deferred->CSSetUnorderedAccessViews(0, 1, uav.GetAddressOf(), nullptr);
		deferred->CSSetConstantBuffers(0, 1, constants.GetAddressOf());
		deferred->CSSetSamplers(0, 1, sampler.GetAddressOf());
		// DXVK shares Dispatch's implementation between context types. Permit
		// only this owned deferred-context dispatch, never native draw work.
		if (!RemixBridge::DispatchMaterialBake(deferred.Get(), (parameters.width + 7) / 8, (parameters.height + 7) / 8))
			return {};
		ID3D11UnorderedAccessView* nullUav = nullptr;
		deferred->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
		deferred->GenerateMips(outputView.Get());
		ComPtr<ID3D11CommandList> commands;
		if (FAILED(deferred->FinishCommandList(FALSE, commands.GetAddressOf())))
			return {};
		globals::d3d::context->ExecuteCommandList(commands.Get(), TRUE);
		logger::info("[RemixMaterial] baked FaceGen mode={} {}x{} tint=({}, {}, {})", parameters.mode,
			parameters.width, parameters.height, parameters.tint[0], parameters.tint[1], parameters.tint[2]);
		return outputView;
	}
}
