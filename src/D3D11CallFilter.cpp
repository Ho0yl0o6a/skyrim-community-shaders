#include "D3D11CallFilter.h"

#include <d3d11.h>

namespace D3D11CallFilter
{
	namespace
	{
		// Skyrim drives the immediate context from its render thread only, which is the
		// same assumption the Map/Unmap hooks in Globals.cpp already make, so the shadow
		// needs no synchronisation.
		bool g_filter = true;
		bool g_stats = false;

		struct Counter
		{
			uint64_t calls = 0;
			uint64_t skipped = 0;
		};

		struct Counters
		{
			Counter vbuffer;
			Counter ibuffer;
			Counter psSrv;
			Counter vsSrv;
			Counter psCbuffer;
			Counter vsCbuffer;
		};

		Counters g_counters;
		uint64_t g_frames = 0;

		/**
		 * @brief Shadow of one array of slot bindings.
		 *
		 * \c used is the high-water mark of slots the game has actually touched, so an
		 * invalidation only has to clear that prefix rather than the whole 128-slot array
		 * D3D11 allows. Skyrim never goes far past the first handful.
		 */
		template <class T, uint32_t N>
		struct SlotArray
		{
			T* slots[N]{};
			uint32_t used = 0;

			void invalidate()
			{
				std::fill_n(slots, used, nullptr);
				used = 0;
			}
		};

		struct ShadowState
		{
			SlotArray<ID3D11ShaderResourceView, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> psSrv;
			SlotArray<ID3D11ShaderResourceView, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> vsSrv;
			SlotArray<ID3D11Buffer, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT> psCbuffer;
			SlotArray<ID3D11Buffer, D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT> vsCbuffer;

			ID3D11Buffer* vbuffer[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT]{};
			UINT vbufferStride[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT]{};
			UINT vbufferOffset[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT]{};
			uint32_t vbufferUsed = 0;

			ID3D11Buffer* ibuffer = nullptr;
			DXGI_FORMAT ibufferFormat = DXGI_FORMAT_UNKNOWN;
			UINT ibufferOffset = 0;
		};

		alignas(64) ShadowState g_shadow;

		/** @brief Binding a render target can implicitly unbind a shader resource behind our back. */
		void InvalidateShaderResources()
		{
			g_shadow.psSrv.invalidate();
			g_shadow.vsSrv.invalidate();
		}

		void InvalidateAll()
		{
			InvalidateShaderResources();
			g_shadow.psCbuffer.invalidate();
			g_shadow.vsCbuffer.invalidate();

			std::fill_n(g_shadow.vbuffer, g_shadow.vbufferUsed, nullptr);
			g_shadow.vbufferUsed = 0;

			g_shadow.ibuffer = nullptr;
			g_shadow.ibufferFormat = DXGI_FORMAT_UNKNOWN;
			g_shadow.ibufferOffset = 0;
		}

		/**
		 * @brief Compares a slot binding against the shadow and updates it.
		 * @return \c true when the call changes nothing and may be dropped.
		 */
		template <class T, uint32_t N>
		bool TrackSlots(SlotArray<T, N>& a_shadow, Counter& a_counter,
			UINT a_startSlot, UINT a_count, T* const* a_values)
		{
			if (!a_count || !a_values || a_startSlot + a_count > N) [[unlikely]] {
				a_shadow.invalidate();
				return false;
			}

			bool redundant = true;

			for (UINT i = 0; i < a_count; i++) {
				if (a_shadow.slots[a_startSlot + i] != a_values[i]) {
					redundant = false;
					break;
				}
			}

			++a_counter.calls;

			if (redundant) {
				++a_counter.skipped;
				return g_filter;
			}

			for (UINT i = 0; i < a_count; i++)
				a_shadow.slots[a_startSlot + i] = a_values[i];

			a_shadow.used = std::max(a_shadow.used, a_startSlot + a_count);
			return false;
		}

		struct IASetVertexBuffers_Hook
		{
			static void WINAPI thunk(ID3D11DeviceContext* This, UINT StartSlot, UINT NumBuffers,
				ID3D11Buffer* const* ppVertexBuffers, const UINT* pStrides, const UINT* pOffsets)
			{
				if (NumBuffers && ppVertexBuffers && pStrides && pOffsets &&
					StartSlot + NumBuffers <= D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT) [[likely]] {
					bool redundant = true;

					for (UINT i = 0; i < NumBuffers; i++) {
						const UINT slot = StartSlot + i;

						if (g_shadow.vbuffer[slot] != ppVertexBuffers[i] ||
							g_shadow.vbufferStride[slot] != pStrides[i] ||
							g_shadow.vbufferOffset[slot] != pOffsets[i]) {
							redundant = false;
							break;
						}
					}

					++g_counters.vbuffer.calls;

					if (redundant) {
						++g_counters.vbuffer.skipped;

						if (g_filter)
							return;
					} else {
						for (UINT i = 0; i < NumBuffers; i++) {
							const UINT slot = StartSlot + i;
							g_shadow.vbuffer[slot] = ppVertexBuffers[i];
							g_shadow.vbufferStride[slot] = pStrides[i];
							g_shadow.vbufferOffset[slot] = pOffsets[i];
						}

						g_shadow.vbufferUsed = std::max(g_shadow.vbufferUsed, StartSlot + NumBuffers);
					}
				} else {
					std::fill_n(g_shadow.vbuffer, g_shadow.vbufferUsed, nullptr);
					g_shadow.vbufferUsed = 0;
				}

				func(This, StartSlot, NumBuffers, ppVertexBuffers, pStrides, pOffsets);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct IASetIndexBuffer_Hook
		{
			static void WINAPI thunk(ID3D11DeviceContext* This, ID3D11Buffer* pIndexBuffer,
				DXGI_FORMAT Format, UINT Offset)
			{
				const bool redundant = g_shadow.ibuffer == pIndexBuffer &&
				                       g_shadow.ibufferFormat == Format &&
				                       g_shadow.ibufferOffset == Offset;

				++g_counters.ibuffer.calls;

				if (redundant) {
					++g_counters.ibuffer.skipped;

					if (g_filter)
						return;
				} else {
					g_shadow.ibuffer = pIndexBuffer;
					g_shadow.ibufferFormat = Format;
					g_shadow.ibufferOffset = Offset;
				}

				func(This, pIndexBuffer, Format, Offset);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct PSSetShaderResources_Hook
		{
			static void WINAPI thunk(ID3D11DeviceContext* This, UINT StartSlot, UINT NumViews,
				ID3D11ShaderResourceView* const* ppShaderResourceViews)
			{
				if (TrackSlots(g_shadow.psSrv, g_counters.psSrv, StartSlot, NumViews, ppShaderResourceViews))
					return;

				func(This, StartSlot, NumViews, ppShaderResourceViews);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct VSSetShaderResources_Hook
		{
			static void WINAPI thunk(ID3D11DeviceContext* This, UINT StartSlot, UINT NumViews,
				ID3D11ShaderResourceView* const* ppShaderResourceViews)
			{
				if (TrackSlots(g_shadow.vsSrv, g_counters.vsSrv, StartSlot, NumViews, ppShaderResourceViews))
					return;

				func(This, StartSlot, NumViews, ppShaderResourceViews);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct PSSetConstantBuffers_Hook
		{
			static void WINAPI thunk(ID3D11DeviceContext* This, UINT StartSlot, UINT NumBuffers,
				ID3D11Buffer* const* ppConstantBuffers)
			{
				if (TrackSlots(g_shadow.psCbuffer, g_counters.psCbuffer, StartSlot, NumBuffers, ppConstantBuffers))
					return;

				func(This, StartSlot, NumBuffers, ppConstantBuffers);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct VSSetConstantBuffers_Hook
		{
			static void WINAPI thunk(ID3D11DeviceContext* This, UINT StartSlot, UINT NumBuffers,
				ID3D11Buffer* const* ppConstantBuffers)
			{
				if (TrackSlots(g_shadow.vsCbuffer, g_counters.vsCbuffer, StartSlot, NumBuffers, ppConstantBuffers))
					return;

				func(This, StartSlot, NumBuffers, ppConstantBuffers);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct OMSetRenderTargets_Hook
		{
			static void WINAPI thunk(ID3D11DeviceContext* This, UINT NumViews,
				ID3D11RenderTargetView* const* ppRenderTargetViews, ID3D11DepthStencilView* pDepthStencilView)
			{
				InvalidateShaderResources();
				func(This, NumViews, ppRenderTargetViews, pDepthStencilView);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct OMSetRenderTargetsAndUnorderedAccessViews_Hook
		{
			static void WINAPI thunk(ID3D11DeviceContext* This, UINT NumRTVs,
				ID3D11RenderTargetView* const* ppRenderTargetViews, ID3D11DepthStencilView* pDepthStencilView,
				UINT UAVStartSlot, UINT NumUAVs, ID3D11UnorderedAccessView* const* ppUnorderedAccessViews,
				const UINT* pUAVInitialCounts)
			{
				InvalidateShaderResources();
				func(This, NumRTVs, ppRenderTargetViews, pDepthStencilView,
					UAVStartSlot, NumUAVs, ppUnorderedAccessViews, pUAVInitialCounts);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct ClearState_Hook
		{
			static void WINAPI thunk(ID3D11DeviceContext* This)
			{
				InvalidateAll();
				func(This);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct ExecuteCommandList_Hook
		{
			static void WINAPI thunk(ID3D11DeviceContext* This, ID3D11CommandList* pCommandList,
				BOOL RestoreContextState)
			{
				InvalidateAll();
				func(This, pCommandList, RestoreContextState);
				InvalidateAll();
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		bool EnvFlag(const char* a_name, bool a_default)
		{
			char buf[8]{};
			if (!::GetEnvironmentVariableA(a_name, buf, sizeof(buf)))
				return a_default;
			return buf[0] == '1';
		}
	}

	void Install(ID3D11DeviceContext* a_context)
	{
		// Opt-in. Interception is not free: each hooked call costs about as much as the
		// DXVK call it is trying to avoid (see the header), so the hooks are only worth
		// installing when something is deliberately measuring them.
		char request[8]{};
		if (!a_context || !::GetEnvironmentVariableA("CS_D3D11_FILTER", request, sizeof(request)))
			return;

		g_filter = request[0] == '1';
		g_stats = EnvFlag("CS_D3D11_FILTER_STATS", false);

		stl::detour_vfunc<7, VSSetConstantBuffers_Hook>(a_context);
		stl::detour_vfunc<8, PSSetShaderResources_Hook>(a_context);
		stl::detour_vfunc<16, PSSetConstantBuffers_Hook>(a_context);
		stl::detour_vfunc<18, IASetVertexBuffers_Hook>(a_context);
		stl::detour_vfunc<19, IASetIndexBuffer_Hook>(a_context);
		stl::detour_vfunc<25, VSSetShaderResources_Hook>(a_context);
		stl::detour_vfunc<33, OMSetRenderTargets_Hook>(a_context);
		stl::detour_vfunc<34, OMSetRenderTargetsAndUnorderedAccessViews_Hook>(a_context);
		stl::detour_vfunc<58, ExecuteCommandList_Hook>(a_context);
		stl::detour_vfunc<110, ClearState_Hook>(a_context);

		logger::info("[D3D11Filter] installed (filter={}, stats={})", g_filter, g_stats);
	}

	void OnPresent()
	{
		if (!g_stats)
			return;

		constexpr uint64_t window = 600;

		if (++g_frames < window)
			return;

		const auto line = [](const char* a_name, const Counter& a_counter) {
			const double calls = double(a_counter.calls) / double(window);
			const double skipped = double(a_counter.skipped) / double(window);
			logger::info("[D3D11Filter]   {:<22} {:8.1f} calls/frame  {:8.1f} redundant ({:.1f}%)",
				a_name, calls, skipped, calls > 0.0 ? 100.0 * skipped / calls : 0.0);
		};

		logger::info("[D3D11Filter] over {} frames:", window);
		line("IASetVertexBuffers", g_counters.vbuffer);
		line("IASetIndexBuffer", g_counters.ibuffer);
		line("PSSetShaderResources", g_counters.psSrv);
		line("VSSetShaderResources", g_counters.vsSrv);
		line("PSSetConstantBuffers", g_counters.psCbuffer);
		line("VSSetConstantBuffers", g_counters.vsCbuffer);

		g_counters = Counters{};
		g_frames = 0;
	}
}
