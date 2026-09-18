// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "vita/GSDeviceGXM.h"

#if !defined(VITASX2_QEMU_VALIDATION)

#include "GS/GSRegs.h"
#include "GS/Renderers/Common/GSDevice.h"
#include "GS/Renderers/Common/GSVertex.h"
#include "Memory.h"
#include "MTVU.h"
#include "VU.h"
#include "common/Console.h"
#include "common/Threading.h"
#include "common/Timer.h"
#include "vita/VitaGpuVuDraw.h"
#include "vita/VitaGpuVuGeneratedUniversal.h"
#include "vita/VitaGpuVuHealthJournal.h"
#include "vita/VitaGpuVuUniversalEpoch.h"
#if defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
#include "vita/VitaGpuVuCommandEpoch.h"
#include "vita/VitaGpuVuMicroProgram.h"
#include "vita/VitaVuApproximateMath.h"
#endif
#include "vita/VitaGpuVuProgramRegistry.h"
#include "vita/VitaGpuVuShaderCompiler.h"
#include "vita/VitaGpuVuVifInput.h"
#include "vita/VitaGsMailbox.h"
#include "vita/VitaGxmArena.h"
#include "vita/VitaGxmDisplay.h"
#include "vita/VitaGxmMemory.h"
#include "vita/VitaGxmRenderStore.h"
#include "vita/VitaGxmTexture.h"
#include "vita/VitaPerformanceTelemetry.h"
#if defined(VITASX2_GXM_SCISSOR_VALIDATION) || defined(VITASX2_GXM_ATTACHMENT_CLEAR_VALIDATION)
#include "../../tests/gs/ScissorMaskCases.h"
#endif
#if defined(VITASX2_GS_DRAW_TRACE) && VITASX2_GS_DRAW_TRACE
#include "vita/VitaGsDrawTrace.h"
#endif

#include <psp2/gxm.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#if defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
#include <psp2/io/fcntl.h>
#endif

#include <algorithm>
#include <atomic>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

// VitaSDK's public compatibility header omits this documented libGXM query,
// although its SceGxm stub exports the official NID. Sony's SDK 3.550
// gxm/program.h defines it and the output bits used below.
extern "C" unsigned int sceGxmProgramGetVertexProgramOutputs(
	const SceGxmProgram* program);

namespace
{
	VitaGXM::RenderStoreTargetKey MakeRenderStoreTargetKey(
		const VitaGXM::GSTextureGXM& texture)
	{
		const VitaGXM::GuestRenderTargetIdentity& identity =
			texture.GuestTargetIdentity();
		return {identity.base_block, identity.buffer_width, identity.psm,
			identity.width, identity.height, identity.scale_bits, identity.depth};
	}

	constexpr u32 GXM_VERTEX_OUTPUT_POSITION_BIT = 1u << 0u;
	constexpr u32 GXM_VERTEX_OUTPUT_PSIZE_BIT = 1u << 14u;
	constexpr u32 GPU_VU_COMPUTE_REQUIRED_VERTEX_OUTPUTS =
		GXM_VERTEX_OUTPUT_POSITION_BIT | GXM_VERTEX_OUTPUT_PSIZE_BIT;

	bool GpuVuComputeVertexOutputsAreValid(
		const SceGxmProgram* program, u32* outputs = nullptr)
	{
		const u32 found = program ?
			sceGxmProgramGetVertexProgramOutputs(program) : 0u;
		if (outputs)
			*outputs = found;
		return (found & GPU_VU_COMPUTE_REQUIRED_VERTEX_OUTPUTS) ==
			GPU_VU_COMPUTE_REQUIRED_VERTEX_OUTPUTS;
	}

	void ConfigureGpuVuComputeRaster(SceGxmContext* context)
	{
		// Sony's sceGxmDraw contract requires POINTS to use a point polygon
		// mode and a vertex program which writes PSIZE. These GPU-VU roots use
		// the vertex stage only, so suppress fragment execution explicitly.
		sceGxmSetFrontPolygonMode(context, SCE_GXM_POLYGON_MODE_POINT);
		sceGxmSetBackPolygonMode(context, SCE_GXM_POLYGON_MODE_POINT);
		sceGxmSetFrontFragmentProgramEnable(
			context, SCE_GXM_FRAGMENT_PROGRAM_DISABLED);
		sceGxmSetBackFragmentProgramEnable(
			context, SCE_GXM_FRAGMENT_PROGRAM_DISABLED);
	}

	void RestoreGpuVuComputeRaster(SceGxmContext* context)
	{
		// The ordinary GS path starts from triangle-fill with fragment shading
		// enabled. Restore that canonical state when a compute draw shared its
		// scene rather than leaking POINT state into the next GS primitive.
		sceGxmSetFrontPolygonMode(context, SCE_GXM_POLYGON_MODE_TRIANGLE_FILL);
		sceGxmSetBackPolygonMode(context, SCE_GXM_POLYGON_MODE_TRIANGLE_FILL);
		sceGxmSetFrontFragmentProgramEnable(
			context, SCE_GXM_FRAGMENT_PROGRAM_ENABLED);
		sceGxmSetBackFragmentProgramEnable(
			context, SCE_GXM_FRAGMENT_PROGRAM_ENABLED);
	}

	void ReportGpuVuMemoryPhase(const char* phase)
	{
		SceKernelFreeMemorySizeInfo info{};
		info.size = sizeof(info);
		const s32 result = sceKernelGetFreeMemorySize(&info);
		constexpr s32 MaximumCrediblePoolBytes = 512 * 1024 * 1024;
		const bool credible = result >= 0 && info.size_user >= 0 &&
			info.size_user <= MaximumCrediblePoolBytes && info.size_cdram >= 0 &&
			info.size_cdram <= MaximumCrediblePoolBytes &&
			info.size_phycont >= 0 &&
			info.size_phycont <= MaximumCrediblePoolBytes;
		Console.WriteLn(
			"GPU-VU memory phase=%s result=%08x credible=%u "
			"user=%d cdram=%d phycont=%d.",
			phase, static_cast<u32>(result), credible ? 1u : 0u,
			info.size_user, info.size_cdram, info.size_phycont);
	}
}

extern "C"
{
	extern const SceGxmProgram _binary_vitasx2_color_v_gxp_start;
	extern const SceGxmProgram _binary_vitasx2_color_f_gxp_start;
	extern const SceGxmProgram _binary_vitasx2_present_v_gxp_start;
	extern const SceGxmProgram _binary_vitasx2_present_f_gxp_start;
	extern const SceGxmProgram _binary_vitasx2_merge_f_gxp_start;
	extern const SceGxmProgram _binary_vitasx2_copy_f_gxp_start;
	extern const SceGxmProgram _binary_vitasx2_rta_correction_f_gxp_start;
	extern const SceGxmProgram _binary_vitasx2_rta_decorrection_f_gxp_start;
	extern const SceGxmProgram _binary_vitasx2_mad_buffer_f_gxp_start;
	extern const SceGxmProgram _binary_vitasx2_mad_reconstruct_f_gxp_start;
	extern const SceGxmProgram _binary_vitasx2_tfx_v_gxp_start;
	extern const SceGxmProgram _binary_vitasx2_tfx_f_gxp_start;
	extern const SceGxmProgram _binary_vitasx2_tfx_manual_lod_f_gxp_start;
	extern const SceGxmProgram _binary_vitasx2_tfx_manual_lod_zfloor_f_gxp_start;
	extern const SceGxmProgram _binary_vitasx2_tfx_zfloor_f_gxp_start;
	extern const SceGxmProgram _binary_vitasx2_tfx_psm16_f_gxp_start;
	extern const SceGxmProgram _binary_vitasx2_tfx_psm16_zfloor_f_gxp_start;
	extern const SceGxmProgram _binary_vitasx2_tfx_zfloor_source_direct_decal_af_f_gxp_start;
	extern const SceGxmProgram _binary_vitasx2_tfx_fast_f_gxp_start;
	extern const SceGxmProgram _binary_vitasx2_tfx_fast_no_atst_f_gxp_start;
	extern const SceGxmProgram _binary_vitasx2_tfx_gs_blend_f_gxp_start;
	extern const SceGxmProgram _binary_vitasx2_tfx_gs_blend_zfloor_f_gxp_start;
	extern const SceGxmProgram _binary_vitasx2_tfx_gs_blend_no_atst_f_gxp_start;
	extern const SceGxmProgram _binary_vitasx2_tfx_gs_blend_no_atst_zfloor_f_gxp_start;
	extern const SceGxmProgram _binary_vitasx2_tfx_gs_blend_source_f_gxp_start;
	extern const SceGxmProgram _binary_vitasx2_tfx_gs_blend_source_zfloor_f_gxp_start;
	extern const SceGxmProgram _binary_vitasx2_tfx_gs_blend_source_no_atst_f_gxp_start;
	extern const SceGxmProgram _binary_vitasx2_tfx_gs_blend_source_no_atst_zfloor_f_gxp_start;
	extern const SceGxmProgram _binary_vitasx2_tfx_gs_blend_direct_modulate_stq_f_gxp_start;
	extern const SceGxmProgram _binary_vitasx2_tfx_gs_blend_direct_modulate_stq_zfloor_f_gxp_start;
	extern const SceGxmProgram _binary_vitasx2_tfx_uv_no_fog_f_gxp_start;
	extern const SceGxmProgram _binary_vitasx2_tfx_uv_no_fog_fast_f_gxp_start;
	extern const SceGxmProgram _binary_vitasx2_tfx_uv_no_fog_zfloor_f_gxp_start;
	extern const SceGxmProgram _binary_vitasx2_tfx_region_repeat_f_gxp_start;
	extern const SceGxmProgram _binary_vitasx2_tfx_region_repeat_fast_f_gxp_start;
	extern const SceGxmProgram _binary_vitasx2_tfx_untextured_f_gxp_start;
	extern const SceGxmProgram _binary_vitasx2_tfx_source_f_gxp_start;
	extern const SceGxmProgram _binary_vitasx2_tfx_programmable_add_f_gxp_start;
	extern const SceGxmProgram _binary_vitasx2_tfx_programmable_add_direct_f_gxp_start;
	extern const SceGxmProgram _binary_vitasx2_tfx_programmable_over_f_gxp_start;
	extern const SceGxmProgram _binary_vitasx2_tfx_source_direct_f_gxp_start;
	extern const SceGxmProgram _binary_vitasx2_tfx_source_direct_modulate_f_gxp_start;
	extern const SceGxmProgram _binary_vitasx2_tfx_source_direct_modulate_af_f_gxp_start;
	extern const SceGxmProgram _binary_vitasx2_tfx_source_untextured_f_gxp_start;
#if defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
	extern const SceGxmProgram _binary_vitasx2_gpu_vu_universal_f_gxp_start;
#endif
}

namespace
{
	VitaGxmPerformanceCounters s_gxm_worker_performance;
	// Identity comes from the submitted fragment program, never its admission
	// booleans. Ordering matches VitaGxmTfxProgram (Unknown has no embedded GXP).
	const std::array<const SceGxmProgram*, static_cast<size_t>(VitaGxmTfxProgram::Unknown)>
		s_tfx_gxp_programs = {
			&_binary_vitasx2_tfx_f_gxp_start,
			&_binary_vitasx2_tfx_manual_lod_f_gxp_start,
			&_binary_vitasx2_tfx_manual_lod_zfloor_f_gxp_start,
			&_binary_vitasx2_tfx_zfloor_f_gxp_start,
			&_binary_vitasx2_tfx_psm16_f_gxp_start,
			&_binary_vitasx2_tfx_psm16_zfloor_f_gxp_start,
			&_binary_vitasx2_tfx_zfloor_source_direct_decal_af_f_gxp_start,
			&_binary_vitasx2_tfx_fast_f_gxp_start,
			&_binary_vitasx2_tfx_uv_no_fog_f_gxp_start,
			&_binary_vitasx2_tfx_uv_no_fog_fast_f_gxp_start,
			&_binary_vitasx2_tfx_uv_no_fog_zfloor_f_gxp_start,
			&_binary_vitasx2_tfx_region_repeat_f_gxp_start,
			&_binary_vitasx2_tfx_region_repeat_fast_f_gxp_start,
			&_binary_vitasx2_tfx_untextured_f_gxp_start,
			&_binary_vitasx2_tfx_source_f_gxp_start,
			&_binary_vitasx2_tfx_programmable_add_f_gxp_start,
			&_binary_vitasx2_tfx_programmable_add_direct_f_gxp_start,
			&_binary_vitasx2_tfx_programmable_over_f_gxp_start,
			&_binary_vitasx2_tfx_source_direct_f_gxp_start,
			&_binary_vitasx2_tfx_source_direct_modulate_f_gxp_start,
			&_binary_vitasx2_tfx_source_direct_modulate_af_f_gxp_start,
			&_binary_vitasx2_tfx_source_untextured_f_gxp_start,
			&_binary_vitasx2_tfx_fast_no_atst_f_gxp_start,
			&_binary_vitasx2_tfx_gs_blend_f_gxp_start,
			&_binary_vitasx2_tfx_gs_blend_zfloor_f_gxp_start,
			&_binary_vitasx2_tfx_gs_blend_no_atst_f_gxp_start,
			&_binary_vitasx2_tfx_gs_blend_no_atst_zfloor_f_gxp_start,
			&_binary_vitasx2_tfx_gs_blend_source_f_gxp_start,
			&_binary_vitasx2_tfx_gs_blend_source_zfloor_f_gxp_start,
			&_binary_vitasx2_tfx_gs_blend_source_no_atst_f_gxp_start,
			&_binary_vitasx2_tfx_gs_blend_source_no_atst_zfloor_f_gxp_start,
			&_binary_vitasx2_tfx_gs_blend_direct_modulate_stq_f_gxp_start,
			&_binary_vitasx2_tfx_gs_blend_direct_modulate_stq_zfloor_f_gxp_start,
		};

	// Bounded native pixel-program capabilities, not desktop blend factors.
	// bit 0: depth replacement; bit 1: no alpha test; bit 2: no destination.
	constexpr u32 GS_BLEND_CAPABILITY_PROGRAM_COUNT = 8;
	constexpr u32 GS_BLEND_DIRECT_MODULATE_STQ = 8;
	constexpr u32 GS_BLEND_DIRECT_MODULATE_STQ_ZFLOOR = 9;
	constexpr u32 GS_BLEND_PROGRAM_COUNT = 10;
	bool CanUseGsBlendDirectModulateStq(const GSHWDrawConfig& config,
		const GSHWDrawConfig::PSSelector& ps)
	{
		const auto& op = config.gs_blend;
		return op.enabled && op.a == 0 && op.b == 2 && op.c == 1 && op.d == 1 &&
			!op.pabe && op.clamp && config.vs.tme && !ps.fst && ps.tfx == 0 &&
			ps.tcc && ps.aem_fmt == 0 && !ps.aem && !ps.ltf && !ps.region_rect &&
			ps.wms <= 1 && ps.wmt <= 1 && !ps.fog &&
			ps.atst == GSHWDrawConfig::PS_ATST::NONE && !ps.fixed_one_a &&
			!ps.fba && ps.dst_fmt == 0 && !ps.rta_source_correction &&
			!ps.rta_correction && !ps.date && !ps.fbmask && !ps.no_color &&
			config.colormask.wrgba == 15;
	}
	u32 SelectGsBlendProgram(const GSHWDrawConfig& config,
		const GSHWDrawConfig::PSSelector& ps)
	{
		if (CanUseGsBlendDirectModulateStq(config, ps))
			return ps.zfloor ? GS_BLEND_DIRECT_MODULATE_STQ_ZFLOOR :
				GS_BLEND_DIRECT_MODULATE_STQ;
		// PCSX2's scanline AlphaBlend reads Cd/Ad only for these operands.
		// Write-mask/DATE preservation can still require a destination read even
		// when the color equation does not. RGB24 preserves the high byte.
		const bool source_only = !config.gs_blend.ReadsDestination() &&
			!ps.date && !ps.fbmask && !ps.no_color && ps.dst_fmt == 0 &&
			config.colormask.wrgba == 15;
		return (ps.zfloor ? 1u : 0u) |
			(ps.atst == GSHWDrawConfig::PS_ATST::NONE ? 2u : 0u) |
			(source_only ? 4u : 0u);
	}

	size_t IdentifyTfxProgram(const SceGxmFragmentProgram* fragment)
	{
		// Sony libGXM Reference: this returns the underlying registered GXP,
		// including for shader-patcher-created blend/mask variants.
		const SceGxmProgram* program = sceGxmFragmentProgramGetProgram(fragment);
		for (size_t i = 0; i < s_tfx_gxp_programs.size(); i++)
		{
			if (s_tfx_gxp_programs[i] == program)
				return i;
		}
		return static_cast<size_t>(VitaGxmTfxProgram::Unknown);
	}

	void LogTfxProgramIdentities()
	{
		if (!VitaPerformanceTelemetry::IsEnabled())
			return;
		for (size_t i = 0; i < s_tfx_gxp_programs.size(); i++)
		{
			const SceGxmProgram* program = s_tfx_gxp_programs[i];
			const u32 bytes = sceGxmProgramGetSize(program);
			const u8* data = reinterpret_cast<const u8*>(program);
			u64 hash = UINT64_C(14695981039346656037);
			for (u32 j = 0; j < bytes; j++)
				hash = (hash ^ data[j]) * UINT64_C(1099511628211);
			Console.WriteLn("GXM TFX binary program=%s bytes=%u fnv1a64=%016llx "
				"discard=%u depth_replace=%u",
				VitaGxmTfxProgramNames[i], bytes, static_cast<unsigned long long>(hash),
				static_cast<unsigned>(sceGxmProgramIsDiscardUsed(program)),
				static_cast<unsigned>(sceGxmProgramIsDepthReplaceUsed(program)));
		}
	}
	struct PublishedGxmHostCall
	{
		std::atomic<u64> calls{0};
		std::atomic<u64> wall_us{0};
		std::atomic<u64> max_lifetime_us{0};
	};
	std::array<PublishedGxmHostCall, static_cast<size_t>(VitaGxmHostCall::Count)>
		s_gxm_published_host_calls;
	std::array<PublishedGxmHostCall, static_cast<size_t>(VitaGxmRendererStage::Count)>
		s_gxm_published_renderer_stages;
	struct PublishedTfxProgram
	{
		std::atomic<u64> draws{0}, indices{0}, draw_rect_pixels{0};
		std::atomic<u64> no_tests_draws{0}, no_tests_rect_pixels{0};
	};
	std::array<PublishedTfxProgram, static_cast<size_t>(VitaGxmTfxProgram::Count)>
		s_gxm_published_tfx_programs;
	struct PublishedSceneContent
	{
		std::atomic<u64> scenes{0}, end_scene_wall_us{0};
	};
	std::array<PublishedSceneContent, 16> s_gxm_published_scene_contents;
	struct PublishedSceneTransition
	{
		std::atomic<u64> transitions{0}, clear_calls{0}, clear_followups{0}, clear_only_followups{0};
	};
	std::array<PublishedSceneTransition, 16> s_gxm_published_scene_transitions;
	std::array<std::atomic<u64>, 16> s_gxm_published_unsupported_passes{};

	class ScopedGxmHostCall
	{
	public:
		explicit ScopedGxmHostCall(VitaGxmHostCall call)
			: m_call(call), m_enabled(VitaPerformanceTelemetry::IsEnabled()),
			  m_start(m_enabled ? Common::Timer::GetCurrentValue() : 0)
		{
		}

		~ScopedGxmHostCall()
		{
			if (!m_enabled)
				return;
			const u64 elapsed = static_cast<u64>(Common::Timer::ConvertValueToSeconds(
				Common::Timer::GetCurrentValue() - m_start) * 1000000.0);
			auto& counters = s_gxm_worker_performance.host_calls[static_cast<size_t>(m_call)];
			counters.calls++;
			counters.wall_us += elapsed;
			counters.max_lifetime_us = std::max(counters.max_lifetime_us, elapsed);
		}

	private:
		VitaGxmHostCall m_call;
		bool m_enabled;
		Common::Timer::Value m_start;
	};

	template <typename Callback>
	decltype(auto) TimeGxmHostCall(VitaGxmHostCall call, Callback&& callback)
	{
		// libGXM calls can block on internal queues. Observe existing submission
		// only: no Finish, notification, readback or changed synchronization.
		const ScopedGxmHostCall timer(call);
		return std::forward<Callback>(callback)();
	}
	std::atomic<u64> s_gxm_published_draw_calls{0};
	std::atomic<u64> s_gxm_published_renderer_draw_calls{0};
	std::atomic<u64> s_gxm_published_renderer_draw_wall_us{0};
	std::atomic<u64> s_gxm_published_device_render_calls{0};
	std::atomic<u64> s_gxm_published_device_render_wall_us{0};
	std::atomic<u64> s_gxm_published_draw_indices{0};
	std::atomic<u64> s_gxm_published_gpu_vu_draw_calls{0};
	std::atomic<u64> s_gxm_published_gpu_vu_draw_indices{0};
	std::atomic<u64> s_gxm_published_gpu_vu_descriptor_objects{0};
	std::atomic<u64> s_gxm_published_vertex_upload_bytes{0};
	std::atomic<u64> s_gxm_published_index_upload_bytes{0};
	std::atomic<u64> s_gxm_published_texture_uploads{0};
	std::atomic<u64> s_gxm_published_texture_upload_bytes{0};
	std::atomic<u64> s_gxm_published_render_store_acquisitions{0};
	std::atomic<u64> s_gxm_published_render_store_new_residencies{0};
	std::atomic<u64> s_gxm_published_render_store_resident_switches{0};
	std::atomic<u64> s_gxm_published_render_store_physical_scenes{0};
	std::atomic<u64> s_gxm_published_render_store_loads{0};
	std::atomic<u64> s_gxm_published_render_store_materializations{0};
	std::atomic<u64> s_gxm_published_render_store_failures{0};
	std::atomic<u64> s_gxm_published_texture_readbacks{0};
	std::atomic<u64> s_gxm_published_texture_readback_bytes{0};
	std::atomic<u64> s_gxm_published_tfx_draws{0};
	std::atomic<u64> s_gxm_published_textured_tfx_draws{0};
	std::atomic<u64> s_gxm_published_render_target_source_draws{0};
	std::atomic<u64> s_gxm_published_depth_source_draws{0};
	std::atomic<u64> s_gxm_published_rt_hazard_draws{0};
	std::atomic<u64> s_gxm_published_depth_hazard_draws{0};
	std::atomic<u64> s_gxm_published_feedback_rt_draws{0};
	std::atomic<u64> s_gxm_published_feedback_depth_draws{0};
	std::atomic<u64> s_gxm_published_software_blend_draws{0};
	std::atomic<u64> s_gxm_published_fixed_blend_draws{0};
	std::atomic<u64> s_gxm_published_alpha_test_draws{0};
	std::atomic<u64> s_gxm_published_partial_color_mask_draws{0};
	std::atomic<u64> s_gxm_published_feedback_snapshots{0};
	std::atomic<u64> s_gxm_published_feedback_snapshot_bytes{0};
	std::atomic<u64> s_gxm_published_merge_calls{0};
	std::atomic<u64> s_gxm_published_merge_rc1_draws{0};
	std::atomic<u64> s_gxm_published_merge_rc2_draws{0};
	std::atomic<u64> s_gxm_published_present_calls{0};
	std::atomic<u64> s_gxm_published_interlace_calls{0};
	std::atomic<u64> s_gxm_published_last_merge_pmode{0};
	std::atomic<u64> s_gxm_published_last_merge_extbuf{0};
	std::atomic<u32> s_gxm_published_last_merge_background{0};
	std::atomic<u32> s_gxm_published_last_merge_source_size_1{0};
	std::atomic<u32> s_gxm_published_last_merge_source_size_2{0};
	std::atomic<u32> s_gxm_published_last_merge_source_id_1{0};
	std::atomic<u32> s_gxm_published_last_merge_source_id_2{0};
	std::atomic<u8> s_gxm_published_last_merge_source_mask{0};
	std::atomic<u8> s_gxm_published_last_merge_source_states{0};
	std::atomic<u64> s_gxm_published_last_merge_writer_tfx_writes{0};
	std::atomic<u64> s_gxm_published_last_merge_writer_ps_lo{0};
	std::atomic<u64> s_gxm_published_last_merge_writer_ps_hi{0};
	std::atomic<u64> s_gxm_published_last_merge_writer_draw_area{0};
	std::atomic<u64> s_gxm_published_last_merge_writer_sample_area{0};
	std::atomic<u32> s_gxm_published_last_merge_writer_source_id{0};
	std::atomic<u32> s_gxm_published_last_merge_writer_source_size{0};
	std::atomic<u32> s_gxm_published_last_merge_writer_blend{0};
	std::atomic<u32> s_gxm_published_last_merge_writer_selector_keys{0};
	std::atomic<u8> s_gxm_published_last_merge_trace_circuit{0};
	std::atomic<u8> s_gxm_published_last_merge_writer_kind{0};
	std::atomic<u8> s_gxm_published_last_merge_writer_topology{0};
	std::atomic<u64> s_gxm_published_last_merge_parent_tfx_writes{0};
	std::atomic<u64> s_gxm_published_last_merge_parent_textured_tfx_writes{0};
	std::atomic<u64> s_gxm_published_last_merge_parent_untextured_tfx_writes{0};
	std::atomic<u64> s_gxm_published_last_merge_parent_rt_source_tfx_writes{0};
	std::atomic<u64> s_gxm_published_last_merge_parent_full_mask_tfx_writes{0};
	std::atomic<u64> s_gxm_published_last_merge_parent_rgb_only_tfx_writes{0};
	std::atomic<u64> s_gxm_published_last_merge_parent_alpha_only_tfx_writes{0};
	std::atomic<u64> s_gxm_published_last_merge_parent_other_mask_tfx_writes{0};
	std::atomic<u64> s_gxm_published_last_merge_parent_ps_lo{0};
	std::atomic<u64> s_gxm_published_last_merge_parent_ps_hi{0};
	std::atomic<u64> s_gxm_published_last_merge_parent_draw_area{0};
	std::atomic<u64> s_gxm_published_last_merge_parent_sample_area{0};
	std::atomic<u32> s_gxm_published_last_merge_parent_source_id{0};
	std::atomic<u32> s_gxm_published_last_merge_parent_source_size{0};
	std::atomic<u32> s_gxm_published_last_merge_parent_blend{0};
	std::atomic<u32> s_gxm_published_last_merge_parent_selector_keys{0};
	std::atomic<u64> s_gxm_published_last_merge_parent_last_rgb_ps_lo{0};
	std::atomic<u64> s_gxm_published_last_merge_parent_last_rgb_ps_hi{0};
	std::atomic<u64> s_gxm_published_last_merge_parent_last_rgb_draw_area{0};
	std::atomic<u64> s_gxm_published_last_merge_parent_last_rgb_sample_area{0};
	std::atomic<u32> s_gxm_published_last_merge_parent_last_rgb_source_id{0};
	std::atomic<u32> s_gxm_published_last_merge_parent_last_rgb_source_size{0};
	std::atomic<u32> s_gxm_published_last_merge_parent_last_rgb_blend{0};
	std::atomic<u32> s_gxm_published_last_merge_parent_last_rgb_selector_keys{0};
	std::atomic<u8> s_gxm_published_last_merge_parent_kind{0};
	std::atomic<u8> s_gxm_published_last_merge_parent_topology{0};
	std::atomic<u8> s_gxm_published_last_merge_parent_color_mask{0};
	std::atomic<u8> s_gxm_published_last_merge_parent_last_rgb_topology{0};
	std::atomic<u8> s_gxm_published_last_merge_parent_last_rgb_color_mask{0};
	std::atomic<u64> s_gxm_published_psm24_draws{0};
	std::atomic<u64> s_gxm_published_device_rejects{0};
	std::atomic<u32> s_gxm_published_last_device_reject_hash{0};
	std::atomic<u64> s_gxm_published_last_feedback_ps_lo{0};
	std::atomic<u64> s_gxm_published_last_feedback_ps_hi{0};
	std::atomic<u32> s_gxm_published_last_feedback_blend{0};
	std::atomic<u8> s_gxm_published_last_feedback_vs{0};
	std::atomic<u8> s_gxm_published_last_feedback_sampler{0};
	std::atomic<u8> s_gxm_published_last_feedback_depth{0};
	std::atomic<u8> s_gxm_published_last_feedback_colormask{0};
	std::atomic<u8> s_gxm_published_last_feedback_topology{0};
	std::atomic<u8> s_gxm_published_last_feedback_hazard{0};
	std::atomic<u64> s_gxm_published_rejected_tfx_draws{0};
	std::atomic<u64> s_gxm_published_last_rejected_tfx_features{0};
	std::atomic<u64> s_gxm_published_last_rejected_tfx_ps_lo{0};
	std::atomic<u64> s_gxm_published_last_rejected_tfx_ps_hi{0};
	std::atomic<u32> s_gxm_published_last_rejected_tfx_blend{0};
	std::atomic<u8> s_gxm_published_last_rejected_tfx_vs{0};
	std::atomic<u8> s_gxm_published_last_rejected_tfx_sampler{0};
	std::atomic<u8> s_gxm_published_last_rejected_tfx_depth{0};
	std::atomic<u8> s_gxm_published_last_rejected_tfx_colormask{0};
	std::atomic<u8> s_gxm_published_last_rejected_tfx_topology{0};

	enum GxmTfxRejectFeature : u64
	{
		GxmTfxRejectPaletteTexture = 1ull << 0,
		GxmTfxRejectPaletteFormat = 1ull << 1,
		GxmTfxRejectDepthFormat = 1ull << 2,
		GxmTfxRejectDestinationFormat = 1ull << 3,
		GxmTfxRejectDither = 1ull << 4,
		GxmTfxRejectAlphaFail = 1ull << 5,
		GxmTfxRejectZTest = 1ull << 6,
		GxmTfxRejectShuffle = 1ull << 7,
		GxmTfxRejectChannelFetch = 1ull << 8,
		GxmTfxRejectColorClip = 1ull << 9,
		GxmTfxRejectGameHle = 1ull << 10,
		GxmTfxRejectPointSampler = 1ull << 11,
		GxmTfxRejectSoftwareAnisotropy = 1ull << 12,
		GxmTfxRejectScanMask = 1ull << 13,
		GxmTfxRejectAa1 = 1ull << 14,
		GxmTfxRejectRov = 1ull << 15,
		GxmTfxRejectRegionClamp = 1ull << 16,
		GxmTfxRejectZClamp = 1ull << 17,
		GxmTfxRejectTextureOffset = 1ull << 18,
		GxmTfxRejectCoordinateAdjust = 1ull << 19,
		GxmTfxRejectTextureIsFramebuffer = 1ull << 20,
		GxmTfxRejectDateOrAbe = 1ull << 21,
		GxmTfxRejectDepthDate = 1ull << 22,
		GxmTfxRejectMipFilter = 1ull << 23,
	};

	// Keep the shader-registration capacity, but place these GPU buffers in
	// mapped CDRAM rather than competing with the CPU heap for LPDDR. Sony's
	// SceGxmShaderPatcherParams separates bufferMem (GPU buffers) from the host
	// allocation callbacks; the latter still use cached CPU memory. The old
	// fixed-interpreter providers are quarantined, not startup consumers.
	constexpr u32 PATCHER_BUFFER_BYTES = 12 * 1024 * 1024;
	constexpr u32 PATCHER_VERTEX_USSE_BYTES = 256 * 1024;
	constexpr u32 PATCHER_FRAGMENT_USSE_BYTES = 2 * 1024 * 1024;
	constexpr u32 PATCHER_GENERATED_VERTEX_USSE_HEADROOM_BYTES = 32 * 1024;
	// Sony SDK gxm/error.h names 0x805b0023 as
	// SCE_GXM_ERROR_OUT_OF_VERTEX_USSE_MEMORY. VitaSDK's public header omits
	// that symbolic extension, so keep the official value local to telemetry
	// and pre-effect generated-resource rejection.
	constexpr s32 GXM_ERROR_OUT_OF_VERTEX_USSE_MEMORY =
		static_cast<s32>(0x805b0023u);
	static_assert(PATCHER_GENERATED_VERTEX_USSE_HEADROOM_BYTES <
		PATCHER_VERTEX_USSE_BYTES);
	struct ShaderPatcherUsage
	{
		u32 host = 0;
		u32 buffer = 0;
		u32 vertex_usse = 0;
		u32 fragment_usse = 0;
	};

	ShaderPatcherUsage GetShaderPatcherUsage(
		const SceGxmShaderPatcher* patcher)
	{
		if (!patcher)
			return {};
		return {
			sceGxmShaderPatcherGetHostMemAllocated(patcher),
			sceGxmShaderPatcherGetBufferMemAllocated(patcher),
			sceGxmShaderPatcherGetVertexUsseMemAllocated(patcher),
			sceGxmShaderPatcherGetFragmentUsseMemAllocated(patcher),
		};
	}
	constexpr u32 GEOMETRY_VERTEX_BYTES = 6 * 1024 * 1024;
	constexpr u32 GEOMETRY_INDEX_BYTES = 512 * 1024;
	constexpr u32 GEOMETRY_VERTEX_MAPPED_BYTES =
		GEOMETRY_VERTEX_BYTES + VitaGXM::GpuMappedFetchGuardSize;
	constexpr u32 GEOMETRY_INDEX_MAPPED_BYTES =
		GEOMETRY_INDEX_BYTES + VitaGXM::GpuMappedFetchGuardSize;
	constexpr u32 MAX_STAGED_INDICES = 65532;
	constexpr u32 GPU_VU_SEQUENTIAL_INDEX_COUNT = 65536;
	constexpr u32 GPU_VU_SEQUENTIAL_INDEX_BYTES =
		GPU_VU_SEQUENTIAL_INDEX_COUNT * sizeof(u16);
	static_assert(VitaGXM::GpuMappedFetchGuardSize >= 256u,
		"mapped stream guard must cover Series5 VDM/PDS read-ahead");
	static_assert(GPU_VU_SEQUENTIAL_INDEX_BYTES <= GEOMETRY_INDEX_BYTES);
	constexpr u32 MAX_RENDER_TARGETS = 48;
	constexpr size_t MAX_TFX_PATCHED_PROGRAMS = 128;
	constexpr size_t GPU_VU_RETIREMENT_SLOT_COUNT = 4;
	constexpr size_t GPU_VU_FRAGMENT_PROGRESS_HISTORY_COUNT = 64;
	enum class GpuVuHealthRetirementStage : u32
	{
		Scan = 1u,
		Pending,
		Reached,
		OutputsPublished,
		ResourcesReleasedAndReset,
		SlotPrepare,
		SlotReuse,
		SlotPrepared,
	};
	enum class GpuVuHealthRetirementAction : u32
	{
		CompletedScan = 1u,
		InputRingReuse,
		RetirementSlotReuse,
		SubmissionPrepare,
		ReleaseAll,
	};
	enum class GpuVuHealthRetirementSlotStage : u32
	{
		Empty = 1u,
		Pending,
		Reached,
		InvalidNotification,
	};
	constexpr u32 GPU_VU_HEALTH_RETIREMENT_HAS_TRANSACTION = 1u;
	constexpr u32 GPU_VU_HEALTH_RETIREMENT_HAS_COMPLETION_ONLY = 1u << 1u;
	// A generated vertex job normally retires within one rendered frame. Two
	// seconds without notification progress is already far outside the playable
	// contract, yet remains long enough for clock-scaled diagnostic builds. The
	// timeout is a fail-stop safety boundary, never a CPU replay trigger.
	constexpr double GPU_VU_RETIREMENT_NO_PROGRESS_TIMEOUT_SECONDS = 2.0;
	// Retirement is part of the accepted generated-provider hot path.  It may
	// not grow newlib containers after GXM commands have been encoded.  The
	// transaction pool is the stronger product bound; retain a small allowance
	// for private attestation records which do not consume a transaction slot.
	constexpr size_t GPU_VU_RETIREMENT_PRIVATE_OUTPUT_CAPACITY =
		VitaGpuVu::GeneratedLoopKernelTransactionPoolCapacity + 8u;
	// One buffered input group can retain compact input, batch records, varying
	// live-ins and exact indices, plus one shared structured scratch allocation.
	// A mailbox publication contains at most 256 descriptors.
	constexpr size_t GPU_VU_RETIREMENT_BATCH_ALLOCATION_CAPACITY =
		1u + 4u * 256u;
	constexpr u32 GPU_VU_PRIVATE_STORE_GUARD_BYTES = 64u;
	constexpr u32 GPU_VU_PRIVATE_STORE_SENTINEL = 0xa56b39cdu;
	constexpr u32 GPU_VU_PRIVATE_STORE_GUARD = 0x6d4f21b7u;
#if defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
	constexpr u32 AlignUniversalBuffer(u32 value)
	{
		return (value + 63u) & ~63u;
	}
	constexpr u32 GPU_VU_UNIVERSAL_GXP_OFFSET = 0;
	constexpr u32 GPU_VU_UNIVERSAL_GXP_MAX_BYTES = 48u * 1024u;
	constexpr const char* GPU_VU_UNIVERSAL_GXP_PATH =
		"ux0:data/vitasx2/gpu-vu-universal-v.gxp";
	constexpr u32 GPU_VU_UNIVERSAL_COMPACT_GXP_OFFSET =
		AlignUniversalBuffer(GPU_VU_UNIVERSAL_GXP_OFFSET +
			GPU_VU_UNIVERSAL_GXP_MAX_BYTES);
	constexpr u32 GPU_VU_UNIVERSAL_COMPACT_GXP_MAX_BYTES = 48u * 1024u;
	constexpr const char* GPU_VU_UNIVERSAL_COMPACT_GXP_PATH =
		"ux0:data/vitasx2/gpu-vu-compact-v.gxp";
	constexpr u32 GPU_VU_VIF_UNPACK_GXP_OFFSET =
		AlignUniversalBuffer(GPU_VU_UNIVERSAL_COMPACT_GXP_OFFSET +
			GPU_VU_UNIVERSAL_COMPACT_GXP_MAX_BYTES);
	constexpr u32 GPU_VU_VIF_UNPACK_GXP_MAX_BYTES = 16u * 1024u;
	constexpr const char* GPU_VU_VIF_UNPACK_GXP_PATH =
		"ux0:data/vitasx2/gpu-vif-unpack-v.gxp";
	constexpr u32 GPU_VU_VIF_UNPACK_INDEPENDENT_GXP_OFFSET =
		AlignUniversalBuffer(GPU_VU_VIF_UNPACK_GXP_OFFSET +
			GPU_VU_VIF_UNPACK_GXP_MAX_BYTES);
	constexpr u32 GPU_VU_VIF_UNPACK_INDEPENDENT_GXP_MAX_BYTES = 16u * 1024u;
	constexpr const char* GPU_VU_VIF_UNPACK_INDEPENDENT_GXP_PATH =
		"ux0:data/vitasx2/gpu-vif-unpack-independent-v.gxp";
	constexpr u32 GPU_VU_STRUCTURED_CONTROL_GXP_OFFSET =
		AlignUniversalBuffer(GPU_VU_VIF_UNPACK_INDEPENDENT_GXP_OFFSET +
			GPU_VU_VIF_UNPACK_INDEPENDENT_GXP_MAX_BYTES);
	constexpr u32 GPU_VU_STRUCTURED_CONTROL_GXP_MAX_BYTES = 16u * 1024u;
	constexpr const char* GPU_VU_STRUCTURED_CONTROL_GXP_PATH =
		"ux0:data/vitasx2/gpu-vu-structured-control-v.gxp";
	constexpr u32 GPU_VU_STRUCTURED_STATE_GXP_OFFSET =
		AlignUniversalBuffer(GPU_VU_STRUCTURED_CONTROL_GXP_OFFSET +
			GPU_VU_STRUCTURED_CONTROL_GXP_MAX_BYTES);
	constexpr u32 GPU_VU_STRUCTURED_STATE_GXP_MAX_BYTES = 48u * 1024u;
	constexpr const char* GPU_VU_STRUCTURED_STATE_GXP_PATH =
		"ux0:data/vitasx2/gpu-vu-structured-state-v.gxp";
	constexpr u32 GPU_VU_STRUCTURED_PREFLIGHT_BUILD_GXP_OFFSET =
		AlignUniversalBuffer(GPU_VU_STRUCTURED_STATE_GXP_OFFSET +
			GPU_VU_STRUCTURED_STATE_GXP_MAX_BYTES);
	constexpr u32 GPU_VU_STRUCTURED_PREFLIGHT_BUILD_GXP_MAX_BYTES = 8u * 1024u;
	constexpr const char* GPU_VU_STRUCTURED_PREFLIGHT_BUILD_GXP_PATH =
		"ux0:data/vitasx2/gpu-vu-structured-preflight-build-v.gxp";
	constexpr u32 GPU_VU_STRUCTURED_PREFLIGHT_REDUCE_GXP_OFFSET =
		AlignUniversalBuffer(GPU_VU_STRUCTURED_PREFLIGHT_BUILD_GXP_OFFSET +
			GPU_VU_STRUCTURED_PREFLIGHT_BUILD_GXP_MAX_BYTES);
	constexpr u32 GPU_VU_STRUCTURED_PREFLIGHT_REDUCE_GXP_MAX_BYTES = 8u * 1024u;
	constexpr const char* GPU_VU_STRUCTURED_PREFLIGHT_REDUCE_GXP_PATH =
		"ux0:data/vitasx2/gpu-vu-structured-preflight-reduce-v.gxp";
	constexpr u32 GPU_VU_STRUCTURED_PREFLIGHT_FINALIZE_GXP_OFFSET =
		AlignUniversalBuffer(GPU_VU_STRUCTURED_PREFLIGHT_REDUCE_GXP_OFFSET +
			GPU_VU_STRUCTURED_PREFLIGHT_REDUCE_GXP_MAX_BYTES);
	constexpr u32 GPU_VU_STRUCTURED_PREFLIGHT_FINALIZE_GXP_MAX_BYTES = 8u * 1024u;
	constexpr const char* GPU_VU_STRUCTURED_PREFLIGHT_FINALIZE_GXP_PATH =
		"ux0:data/vitasx2/gpu-vu-structured-preflight-finalize-v.gxp";
	constexpr u32 GPU_VU_PATH1_COMMIT_GXP_OFFSET =
		AlignUniversalBuffer(GPU_VU_STRUCTURED_PREFLIGHT_FINALIZE_GXP_OFFSET +
			GPU_VU_STRUCTURED_PREFLIGHT_FINALIZE_GXP_MAX_BYTES);
	constexpr u32 GPU_VU_PATH1_COMMIT_GXP_MAX_BYTES = 8u * 1024u;
	constexpr const char* GPU_VU_PATH1_COMMIT_GXP_PATH =
		"ux0:data/vitasx2/gpu-vu-path1-commit-v.gxp";
	constexpr u32 GPU_VU_STRUCTURED_QP_NUMERIC_GXP_OFFSET =
		AlignUniversalBuffer(GPU_VU_PATH1_COMMIT_GXP_OFFSET +
			GPU_VU_PATH1_COMMIT_GXP_MAX_BYTES);
	constexpr u32 GPU_VU_STRUCTURED_QP_NUMERIC_GXP_MAX_BYTES = 16u * 1024u;
	constexpr const char* GPU_VU_STRUCTURED_QP_NUMERIC_GXP_PATH =
		"ux0:data/vitasx2/gpu-vu-structured-qp-numeric-v.gxp";
	constexpr u32 GPU_VU_STRUCTURED_FMAC_NUMERIC_GXP_OFFSET =
		AlignUniversalBuffer(GPU_VU_STRUCTURED_QP_NUMERIC_GXP_OFFSET +
			GPU_VU_STRUCTURED_QP_NUMERIC_GXP_MAX_BYTES);
	constexpr u32 GPU_VU_STRUCTURED_FMAC_NUMERIC_GXP_MAX_BYTES = 16u * 1024u;
	constexpr const char* GPU_VU_STRUCTURED_FMAC_NUMERIC_GXP_PATH =
		"ux0:data/vitasx2/gpu-vu-structured-fmac-numeric-v.gxp";
	constexpr u32 GPU_VU_STRUCTURED_FMAC_NATIVE_GXP_OFFSET =
		AlignUniversalBuffer(GPU_VU_STRUCTURED_FMAC_NUMERIC_GXP_OFFSET +
			GPU_VU_STRUCTURED_FMAC_NUMERIC_GXP_MAX_BYTES);
	constexpr u32 GPU_VU_STRUCTURED_FMAC_NATIVE_GXP_MAX_BYTES = 8u * 1024u;
	constexpr const char* GPU_VU_STRUCTURED_FMAC_NATIVE_GXP_PATH =
		"ux0:data/vitasx2/gpu-vu-structured-fmac-native-v.gxp";
	constexpr u32 GPU_VU_UNIVERSAL_MICRO_OFFSET =
		AlignUniversalBuffer(GPU_VU_UNIVERSAL_GXP_MAX_BYTES);
	constexpr u32 GPU_VU_UNIVERSAL_MICRO_BYTES =
		VitaGpuVu::UniversalMicroProgramPairCount *
		sizeof(VitaGpuVu::UniversalPairMicroOp);
	constexpr u32 GPU_VU_UNIVERSAL_VF_OFFSET =
		AlignUniversalBuffer(GPU_VU_UNIVERSAL_MICRO_OFFSET +
			GPU_VU_UNIVERSAL_MICRO_BYTES);
	constexpr u32 GPU_VU_UNIVERSAL_VF_BYTES = 33u * 4u * sizeof(u32);
	constexpr u32 GPU_VU_UNIVERSAL_STATE_OFFSET =
		AlignUniversalBuffer(GPU_VU_UNIVERSAL_VF_OFFSET +
			GPU_VU_UNIVERSAL_VF_BYTES);
	// Words 64..95 preserve delayed VU pipeline queues. Words 96..103 carry
	// transactional VIF1 row/column state for masked and MODE 2/3 UNPACKs.
	constexpr u32 GPU_VU_UNIVERSAL_STATE_WORDS =
		VitaGpuVu::UniversalGpuVuStateWordCount;
	constexpr u32 GPU_VU_UNIVERSAL_STATE_BYTES =
		GPU_VU_UNIVERSAL_STATE_WORDS * sizeof(u32);
	constexpr u32 GPU_VU_UNIVERSAL_MEMORY_OFFSET =
		AlignUniversalBuffer(GPU_VU_UNIVERSAL_STATE_OFFSET +
			GPU_VU_UNIVERSAL_STATE_BYTES);
	constexpr u32 GPU_VU_UNIVERSAL_MEMORY_BYTES = 1024u * 4u * sizeof(u32);
	constexpr u32 GPU_VU_UNIVERSAL_OUTPUT_VF_OFFSET =
		AlignUniversalBuffer(GPU_VU_UNIVERSAL_MEMORY_OFFSET +
			GPU_VU_UNIVERSAL_MEMORY_BYTES);
	constexpr u32 GPU_VU_UNIVERSAL_OUTPUT_STATE_OFFSET =
		AlignUniversalBuffer(GPU_VU_UNIVERSAL_OUTPUT_VF_OFFSET +
			GPU_VU_UNIVERSAL_VF_BYTES);
	constexpr u32 GPU_VU_UNIVERSAL_OUTPUT_MEMORY_OFFSET =
		AlignUniversalBuffer(GPU_VU_UNIVERSAL_OUTPUT_STATE_OFFSET +
			GPU_VU_UNIVERSAL_STATE_BYTES);
	constexpr u32 GPU_VU_UNIVERSAL_EPOCH_OFFSET =
		AlignUniversalBuffer(GPU_VU_UNIVERSAL_OUTPUT_MEMORY_OFFSET +
			GPU_VU_UNIVERSAL_MEMORY_BYTES);
	constexpr u32 GPU_VU_UNIVERSAL_EPOCH_BYTES =
		VitaGpuVu::UniversalCommandEpochMaximumCommands *
			sizeof(VitaGpuVu::UniversalEpochMicroOp);
	constexpr u32 GPU_VU_UNIVERSAL_RESUME_EPOCH_OFFSET =
		AlignUniversalBuffer(GPU_VU_UNIVERSAL_EPOCH_OFFSET +
			GPU_VU_UNIVERSAL_EPOCH_BYTES);
	constexpr u32 GPU_VU_UNIVERSAL_PAYLOAD_OFFSET =
		AlignUniversalBuffer(GPU_VU_UNIVERSAL_RESUME_EPOCH_OFFSET +
			GPU_VU_UNIVERSAL_EPOCH_BYTES);
	constexpr u32 GPU_VU_UNIVERSAL_PAYLOAD_BYTES =
		VitaGpuVu::UniversalGpuVuFixedPayloadWindowBytes;
	constexpr u32 GPU_VU_UNIVERSAL_PATH1_OFFSET =
		AlignUniversalBuffer(GPU_VU_UNIVERSAL_PAYLOAD_OFFSET +
			GPU_VU_UNIVERSAL_PAYLOAD_BYTES);
	constexpr u32 GPU_VU_UNIVERSAL_PATH1_BYTES =
		sizeof(VitaGpuVu::UniversalRawPath1Export);
	constexpr u32 GPU_VU_UNIVERSAL_ESTIMATE_OFFSET =
		AlignUniversalBuffer(GPU_VU_UNIVERSAL_PATH1_OFFSET +
			GPU_VU_UNIVERSAL_PATH1_BYTES);
	constexpr u32 GPU_VU_UNIVERSAL_ESTIMATE_WORDS = 640u;
	constexpr u32 GPU_VU_UNIVERSAL_ESTIMATE_BYTES =
		GPU_VU_UNIVERSAL_ESTIMATE_WORDS * sizeof(u32);
	constexpr u32 GPU_VU_UNIVERSAL_BUFFER_BYTES =
		GPU_VU_UNIVERSAL_ESTIMATE_OFFSET + GPU_VU_UNIVERSAL_ESTIMATE_BYTES;
	static_assert(GPU_VU_UNIVERSAL_GXP_OFFSET +
		GPU_VU_UNIVERSAL_GXP_MAX_BYTES <= GPU_VU_UNIVERSAL_MICRO_OFFSET);
	static_assert(GPU_VU_UNIVERSAL_MICRO_OFFSET +
		GPU_VU_UNIVERSAL_MICRO_BYTES <= GPU_VU_UNIVERSAL_VF_OFFSET);
	static_assert(GPU_VU_UNIVERSAL_VF_OFFSET +
		GPU_VU_UNIVERSAL_VF_BYTES <= GPU_VU_UNIVERSAL_STATE_OFFSET);
	static_assert(GPU_VU_UNIVERSAL_STATE_OFFSET +
		GPU_VU_UNIVERSAL_STATE_BYTES <= GPU_VU_UNIVERSAL_MEMORY_OFFSET);
	static_assert(GPU_VU_UNIVERSAL_MEMORY_OFFSET +
		GPU_VU_UNIVERSAL_MEMORY_BYTES <= GPU_VU_UNIVERSAL_OUTPUT_VF_OFFSET);
	static_assert(GPU_VU_UNIVERSAL_OUTPUT_VF_OFFSET +
		GPU_VU_UNIVERSAL_VF_BYTES <= GPU_VU_UNIVERSAL_OUTPUT_STATE_OFFSET);
	static_assert(GPU_VU_UNIVERSAL_OUTPUT_STATE_OFFSET +
		GPU_VU_UNIVERSAL_STATE_BYTES <= GPU_VU_UNIVERSAL_OUTPUT_MEMORY_OFFSET);
	static_assert(GPU_VU_UNIVERSAL_OUTPUT_MEMORY_OFFSET +
		GPU_VU_UNIVERSAL_MEMORY_BYTES <= GPU_VU_UNIVERSAL_EPOCH_OFFSET);
	static_assert(GPU_VU_UNIVERSAL_EPOCH_OFFSET +
		GPU_VU_UNIVERSAL_EPOCH_BYTES <=
		GPU_VU_UNIVERSAL_RESUME_EPOCH_OFFSET);
	static_assert(GPU_VU_UNIVERSAL_RESUME_EPOCH_OFFSET +
		GPU_VU_UNIVERSAL_EPOCH_BYTES <= GPU_VU_UNIVERSAL_PAYLOAD_OFFSET);
	static_assert(GPU_VU_UNIVERSAL_PAYLOAD_OFFSET +
		GPU_VU_UNIVERSAL_PAYLOAD_BYTES <= GPU_VU_UNIVERSAL_PATH1_OFFSET);
	static_assert(GPU_VU_UNIVERSAL_PATH1_OFFSET +
		GPU_VU_UNIVERSAL_PATH1_BYTES <= GPU_VU_UNIVERSAL_ESTIMATE_OFFSET);
	static_assert(GPU_VU_UNIVERSAL_ESTIMATE_OFFSET +
		GPU_VU_UNIVERSAL_ESTIMATE_BYTES <= GPU_VU_UNIVERSAL_BUFFER_BYTES);
	// Format v3 retains the complete PairPlan dependency record (88 KiB) next
	// to the VU state and command buffers. This one-shot oracle owner remains
	// bounded and is released before ordinary rendering; a product-resident
	// cache will separate immutable program records from per-epoch state.
	static_assert(GPU_VU_UNIVERSAL_BUFFER_BYTES < 3u * 1024u * 1024u);

	// Two private generations are the minimum transactional producer/consumer
	// pair. Keep the much larger immutable input journal at four generations;
	// the product slot ABI previously reserved three PATH1 pages the GXP could
	// not bind and four mostly idle state generations, starving that journal.
	constexpr u32 GPU_VU_UNIVERSAL_PRODUCT_SLOT_COUNT = 2;
	constexpr u32 GPU_VU_UNIVERSAL_PRODUCT_ESTIMATE_OFFSET =
		AlignUniversalBuffer(GPU_VU_STRUCTURED_FMAC_NATIVE_GXP_OFFSET +
			GPU_VU_STRUCTURED_FMAC_NATIVE_GXP_MAX_BYTES);
	constexpr u32 GPU_VU_UNIVERSAL_PRODUCT_ZERO_PAYLOAD_OFFSET =
		AlignUniversalBuffer(GPU_VU_UNIVERSAL_PRODUCT_ESTIMATE_OFFSET +
			GPU_VU_UNIVERSAL_ESTIMATE_BYTES);
	constexpr u32 GPU_VU_UNIVERSAL_PRODUCT_SHARED_BYTES =
		GPU_VU_UNIVERSAL_PRODUCT_ZERO_PAYLOAD_OFFSET +
			GPU_VU_UNIVERSAL_PAYLOAD_BYTES;
	constexpr u32 GPU_VU_UNIVERSAL_PRODUCT_MICRO_OFFSET = 0;
	constexpr u32 GPU_VU_UNIVERSAL_PRODUCT_VF_OFFSET =
		AlignUniversalBuffer(GPU_VU_UNIVERSAL_PRODUCT_MICRO_OFFSET +
			GPU_VU_UNIVERSAL_MICRO_BYTES);
	constexpr u32 GPU_VU_UNIVERSAL_PRODUCT_STATE_OFFSET =
		AlignUniversalBuffer(GPU_VU_UNIVERSAL_PRODUCT_VF_OFFSET +
			GPU_VU_UNIVERSAL_VF_BYTES);
	constexpr u32 GPU_VU_UNIVERSAL_PRODUCT_MEMORY_OFFSET =
		AlignUniversalBuffer(GPU_VU_UNIVERSAL_PRODUCT_STATE_OFFSET +
			GPU_VU_UNIVERSAL_STATE_BYTES);
	constexpr u32 GPU_VU_UNIVERSAL_PRODUCT_OUTPUT_VF_OFFSET =
		AlignUniversalBuffer(GPU_VU_UNIVERSAL_PRODUCT_MEMORY_OFFSET +
			GPU_VU_UNIVERSAL_MEMORY_BYTES);
	constexpr u32 GPU_VU_UNIVERSAL_PRODUCT_OUTPUT_STATE_OFFSET =
		AlignUniversalBuffer(GPU_VU_UNIVERSAL_PRODUCT_OUTPUT_VF_OFFSET +
			GPU_VU_UNIVERSAL_VF_BYTES);
	constexpr u32 GPU_VU_UNIVERSAL_PRODUCT_OUTPUT_MEMORY_OFFSET =
		AlignUniversalBuffer(GPU_VU_UNIVERSAL_PRODUCT_OUTPUT_STATE_OFFSET +
			GPU_VU_UNIVERSAL_STATE_BYTES);
	constexpr u32 GPU_VU_UNIVERSAL_PRODUCT_EPOCH_OFFSET =
		AlignUniversalBuffer(GPU_VU_UNIVERSAL_PRODUCT_OUTPUT_MEMORY_OFFSET +
			GPU_VU_UNIVERSAL_MEMORY_BYTES);
	constexpr u32 GPU_VU_UNIVERSAL_PRODUCT_PATH1_OFFSET =
		AlignUniversalBuffer(GPU_VU_UNIVERSAL_PRODUCT_EPOCH_OFFSET +
			GPU_VU_UNIVERSAL_EPOCH_BYTES);
	constexpr u32 GPU_VU_STRUCTURED_SNAPSHOT_BYTES =
		VitaGpuVu::StructuredGeneratedSnapshotWordCount * sizeof(u32);
	constexpr u32 GPU_VU_STRUCTURED_OUTER_STATE_BYTES =
		VitaGpuVu::StructuredGeneratedOuterStateWords * sizeof(u32);
	constexpr u32 GPU_VU_STRUCTURED_VI_SNAPSHOT_BYTES =
		VitaGpuVu::StructuredGeneratedViSnapshotWordCount * sizeof(u32);
	constexpr u32 GPU_VU_STRUCTURED_MAXIMUM_JOURNAL_ENTRIES =
		64u * 64u * 8u;
	constexpr u32 GPU_VU_STRUCTURED_JOURNAL_ADDRESS_BYTES =
		GPU_VU_STRUCTURED_MAXIMUM_JOURNAL_ENTRIES * sizeof(u32);
	static_assert(VitaGpuVu::FixedStructuredStateMaximumWords * sizeof(u32) <=
		GPU_VU_STRUCTURED_JOURNAL_ADDRESS_BYTES);
	constexpr u32 GPU_VU_STRUCTURED_JOURNAL_PAYLOAD_BYTES =
		GPU_VU_STRUCTURED_MAXIMUM_JOURNAL_ENTRIES * 4u * sizeof(u32);
	constexpr u32 GPU_VU_STRUCTURED_SCRATCH_BYTES =
		VitaGpuVu::StructuredGeneratedMaximumScratchInvocations *
		VitaGpuVu::StructuredGeneratedScratchSlots * sizeof(u32);
	// BUFFER10 is deliberately declared as a one-word Cg array and indexed over
	// the complete mapped transaction range. Sony documents that dynamic user-
	// buffer indexing may exceed the source declaration; the application owns
	// the physical bound. Keep one cache-line canary after the maximum legal
	// word so a bad generated operand or address expression rejects the private
	// generation before the following descriptor region can be corrupted.
	constexpr u32 GPU_VU_STRUCTURED_SCRATCH_GUARD_WORDS = 16u;
	constexpr u32 GPU_VU_STRUCTURED_SCRATCH_GUARD_BYTES =
		GPU_VU_STRUCTURED_SCRATCH_GUARD_WORDS * sizeof(u32);
	constexpr u32 GPU_VU_STRUCTURED_SCRATCH_ALLOCATION_BYTES =
		GPU_VU_STRUCTURED_SCRATCH_BYTES +
		GPU_VU_STRUCTURED_SCRATCH_GUARD_BYTES;
	constexpr u32 GPU_VU_STRUCTURED_SCRATCH_GUARD_VALUE = 0x6d4f21b7u;
	constexpr u32 GPU_VU_STRUCTURED_JOURNAL_VALUE_BYTES =
		std::max(GPU_VU_STRUCTURED_JOURNAL_PAYLOAD_BYTES,
			GPU_VU_STRUCTURED_SCRATCH_ALLOCATION_BYTES);
	constexpr u32 GPU_VU_STRUCTURED_QP_DESCRIPTOR_BYTES =
		VitaGpuVu::StructuredGeneratedMaximumPartitionModuleCount *
		sizeof(VitaGpuVu::StructuredFixedQpNumericDescriptor);
	constexpr u32 GPU_VU_STRUCTURED_FMAC_DESCRIPTOR_BYTES =
		VitaGpuVu::StructuredGeneratedMaximumPartitionModuleCount *
		sizeof(VitaGpuVu::StructuredFixedFmacNumericDescriptor);
	static_assert(sizeof(VitaGpuVu::StructuredMemoryPreflightData) <=
		GPU_VU_STRUCTURED_JOURNAL_VALUE_BYTES);
	static_assert(VitaGpuVu::StructuredGeneratedPreflightWorkspaceWords *
		sizeof(u32) <= GPU_VU_STRUCTURED_JOURNAL_VALUE_BYTES);
	static_assert(VitaGpuVu::StructuredGeneratedPreflightWorkspaceWords ==
		64u * 1024u);
	static_assert((VitaGpuVu::StructuredGeneratedPreflightWorkspaceWords +
		VitaGpuVu::StructuredGeneratedAuxiliaryWords) * sizeof(u32) <=
		GPU_VU_STRUCTURED_JOURNAL_VALUE_BYTES);
	static_assert(VitaGpuVu::StructuredGeneratedPreflightMetadataOffset *
		sizeof(u32) + sizeof(VitaGpuVu::StructuredMemoryPreflightData) <=
		VitaGpuVu::StructuredGeneratedAuxiliaryWords * sizeof(u32));
	static_assert(VitaGpuVu::StructuredGeneratedAuxiliaryWords <= 64u * 1024u);
	static_assert(VitaGpuVu::StructuredGeneratedStateScratchOffset +
		VitaGpuVu::FixedStructuredStateMaximumModules *
			VitaGpuVu::StructuredGeneratedStateScratchWordsPerModule ==
		VitaGpuVu::StructuredGeneratedAuxiliaryWords);
	constexpr u32 GPU_VU_UNIVERSAL_PRODUCT_SNAPSHOT_OFFSET =
		AlignUniversalBuffer(GPU_VU_UNIVERSAL_PRODUCT_PATH1_OFFSET +
			GPU_VU_UNIVERSAL_PATH1_BYTES);
	constexpr u32 GPU_VU_UNIVERSAL_PRODUCT_OUTER_STATE_OFFSET =
		AlignUniversalBuffer(GPU_VU_UNIVERSAL_PRODUCT_SNAPSHOT_OFFSET +
			GPU_VU_STRUCTURED_SNAPSHOT_BYTES);
	constexpr u32 GPU_VU_UNIVERSAL_PRODUCT_OUTER_STATE_OUTPUT_OFFSET =
		AlignUniversalBuffer(GPU_VU_UNIVERSAL_PRODUCT_OUTER_STATE_OFFSET +
			GPU_VU_STRUCTURED_OUTER_STATE_BYTES);
	constexpr u32 GPU_VU_UNIVERSAL_PRODUCT_VI_SNAPSHOT_OFFSET =
		AlignUniversalBuffer(GPU_VU_UNIVERSAL_PRODUCT_OUTER_STATE_OUTPUT_OFFSET +
			GPU_VU_STRUCTURED_OUTER_STATE_BYTES);
	constexpr u32 GPU_VU_UNIVERSAL_PRODUCT_JOURNAL_ADDRESS_OFFSET =
		AlignUniversalBuffer(GPU_VU_UNIVERSAL_PRODUCT_VI_SNAPSHOT_OFFSET +
			GPU_VU_STRUCTURED_VI_SNAPSHOT_BYTES);
	constexpr u32 GPU_VU_UNIVERSAL_PRODUCT_JOURNAL_VALUE_OFFSET =
		AlignUniversalBuffer(GPU_VU_UNIVERSAL_PRODUCT_JOURNAL_ADDRESS_OFFSET +
			GPU_VU_STRUCTURED_JOURNAL_ADDRESS_BYTES);
	constexpr u32 GPU_VU_UNIVERSAL_PRODUCT_QP_DESCRIPTOR_OFFSET =
		AlignUniversalBuffer(GPU_VU_UNIVERSAL_PRODUCT_JOURNAL_VALUE_OFFSET +
			GPU_VU_STRUCTURED_JOURNAL_VALUE_BYTES);
	constexpr u32 GPU_VU_UNIVERSAL_PRODUCT_FMAC_DESCRIPTOR_OFFSET =
		AlignUniversalBuffer(GPU_VU_UNIVERSAL_PRODUCT_QP_DESCRIPTOR_OFFSET +
			GPU_VU_STRUCTURED_QP_DESCRIPTOR_BYTES);
	constexpr u32 GPU_VU_UNIVERSAL_PRODUCT_SLOT_BYTES =
		GPU_VU_UNIVERSAL_PRODUCT_FMAC_DESCRIPTOR_OFFSET +
			GPU_VU_STRUCTURED_FMAC_DESCRIPTOR_BYTES;
	static_assert(GPU_VU_UNIVERSAL_PRODUCT_MICRO_OFFSET +
		GPU_VU_UNIVERSAL_MICRO_BYTES <=
		GPU_VU_UNIVERSAL_PRODUCT_VF_OFFSET);
	static_assert(GPU_VU_UNIVERSAL_PRODUCT_VF_OFFSET +
		GPU_VU_UNIVERSAL_VF_BYTES <=
		GPU_VU_UNIVERSAL_PRODUCT_STATE_OFFSET);
	static_assert(GPU_VU_UNIVERSAL_PRODUCT_STATE_OFFSET +
		GPU_VU_UNIVERSAL_STATE_BYTES <=
		GPU_VU_UNIVERSAL_PRODUCT_MEMORY_OFFSET);
	static_assert(GPU_VU_UNIVERSAL_PRODUCT_MEMORY_OFFSET +
		GPU_VU_UNIVERSAL_MEMORY_BYTES <=
		GPU_VU_UNIVERSAL_PRODUCT_OUTPUT_VF_OFFSET);
	static_assert(GPU_VU_UNIVERSAL_PRODUCT_OUTPUT_VF_OFFSET +
		GPU_VU_UNIVERSAL_VF_BYTES <=
		GPU_VU_UNIVERSAL_PRODUCT_OUTPUT_STATE_OFFSET);
	static_assert(GPU_VU_UNIVERSAL_PRODUCT_OUTPUT_STATE_OFFSET +
		GPU_VU_UNIVERSAL_STATE_BYTES <=
		GPU_VU_UNIVERSAL_PRODUCT_OUTPUT_MEMORY_OFFSET);
	static_assert(GPU_VU_UNIVERSAL_PRODUCT_OUTPUT_MEMORY_OFFSET +
		GPU_VU_UNIVERSAL_MEMORY_BYTES <=
		GPU_VU_UNIVERSAL_PRODUCT_EPOCH_OFFSET);
	static_assert(GPU_VU_UNIVERSAL_PRODUCT_EPOCH_OFFSET +
		GPU_VU_UNIVERSAL_EPOCH_BYTES <=
		GPU_VU_UNIVERSAL_PRODUCT_PATH1_OFFSET);
	static_assert(GPU_VU_UNIVERSAL_PRODUCT_PATH1_OFFSET +
		GPU_VU_UNIVERSAL_PATH1_BYTES <=
		GPU_VU_UNIVERSAL_PRODUCT_SNAPSHOT_OFFSET);
	static_assert(GPU_VU_UNIVERSAL_PRODUCT_SNAPSHOT_OFFSET +
		GPU_VU_STRUCTURED_SNAPSHOT_BYTES <=
		GPU_VU_UNIVERSAL_PRODUCT_OUTER_STATE_OFFSET);
	static_assert(GPU_VU_UNIVERSAL_PRODUCT_OUTER_STATE_OFFSET +
		GPU_VU_STRUCTURED_OUTER_STATE_BYTES <=
		GPU_VU_UNIVERSAL_PRODUCT_OUTER_STATE_OUTPUT_OFFSET);
	static_assert(GPU_VU_UNIVERSAL_PRODUCT_OUTER_STATE_OUTPUT_OFFSET +
		GPU_VU_STRUCTURED_OUTER_STATE_BYTES <=
		GPU_VU_UNIVERSAL_PRODUCT_VI_SNAPSHOT_OFFSET);
	static_assert(GPU_VU_UNIVERSAL_PRODUCT_VI_SNAPSHOT_OFFSET +
		GPU_VU_STRUCTURED_VI_SNAPSHOT_BYTES <=
		GPU_VU_UNIVERSAL_PRODUCT_JOURNAL_ADDRESS_OFFSET);
	static_assert(GPU_VU_UNIVERSAL_PRODUCT_JOURNAL_ADDRESS_OFFSET +
		GPU_VU_STRUCTURED_JOURNAL_ADDRESS_BYTES <=
		GPU_VU_UNIVERSAL_PRODUCT_JOURNAL_VALUE_OFFSET);
	static_assert(GPU_VU_UNIVERSAL_PRODUCT_JOURNAL_VALUE_OFFSET +
		GPU_VU_STRUCTURED_JOURNAL_VALUE_BYTES <=
		GPU_VU_UNIVERSAL_PRODUCT_QP_DESCRIPTOR_OFFSET);
	static_assert(GPU_VU_UNIVERSAL_PRODUCT_QP_DESCRIPTOR_OFFSET +
		GPU_VU_STRUCTURED_QP_DESCRIPTOR_BYTES <=
		GPU_VU_UNIVERSAL_PRODUCT_FMAC_DESCRIPTOR_OFFSET);
	static_assert(GPU_VU_UNIVERSAL_PRODUCT_FMAC_DESCRIPTOR_OFFSET +
		GPU_VU_STRUCTURED_FMAC_DESCRIPTOR_BYTES <=
		GPU_VU_UNIVERSAL_PRODUCT_SLOT_BYTES);
	static_assert(GPU_VU_UNIVERSAL_GXP_OFFSET +
		GPU_VU_UNIVERSAL_GXP_MAX_BYTES <=
		GPU_VU_UNIVERSAL_COMPACT_GXP_OFFSET);
	static_assert(GPU_VU_UNIVERSAL_COMPACT_GXP_OFFSET +
		GPU_VU_UNIVERSAL_COMPACT_GXP_MAX_BYTES <=
		GPU_VU_VIF_UNPACK_GXP_OFFSET);
	static_assert(GPU_VU_VIF_UNPACK_GXP_OFFSET +
		GPU_VU_VIF_UNPACK_GXP_MAX_BYTES <=
		GPU_VU_VIF_UNPACK_INDEPENDENT_GXP_OFFSET);
	static_assert(GPU_VU_VIF_UNPACK_INDEPENDENT_GXP_OFFSET +
		GPU_VU_VIF_UNPACK_INDEPENDENT_GXP_MAX_BYTES <=
		GPU_VU_STRUCTURED_CONTROL_GXP_OFFSET);
	static_assert(GPU_VU_STRUCTURED_CONTROL_GXP_OFFSET +
		GPU_VU_STRUCTURED_CONTROL_GXP_MAX_BYTES <=
		GPU_VU_STRUCTURED_STATE_GXP_OFFSET);
	static_assert(GPU_VU_STRUCTURED_STATE_GXP_OFFSET +
		GPU_VU_STRUCTURED_STATE_GXP_MAX_BYTES <=
		GPU_VU_STRUCTURED_PREFLIGHT_BUILD_GXP_OFFSET);
	static_assert(GPU_VU_STRUCTURED_PREFLIGHT_BUILD_GXP_OFFSET +
		GPU_VU_STRUCTURED_PREFLIGHT_BUILD_GXP_MAX_BYTES <=
		GPU_VU_STRUCTURED_PREFLIGHT_REDUCE_GXP_OFFSET);
	static_assert(GPU_VU_STRUCTURED_PREFLIGHT_REDUCE_GXP_OFFSET +
		GPU_VU_STRUCTURED_PREFLIGHT_REDUCE_GXP_MAX_BYTES <=
		GPU_VU_STRUCTURED_PREFLIGHT_FINALIZE_GXP_OFFSET);
	static_assert(GPU_VU_STRUCTURED_PREFLIGHT_FINALIZE_GXP_OFFSET +
		GPU_VU_STRUCTURED_PREFLIGHT_FINALIZE_GXP_MAX_BYTES <=
		GPU_VU_PATH1_COMMIT_GXP_OFFSET);
	static_assert(GPU_VU_PATH1_COMMIT_GXP_OFFSET +
		GPU_VU_PATH1_COMMIT_GXP_MAX_BYTES <=
		GPU_VU_STRUCTURED_QP_NUMERIC_GXP_OFFSET);
	static_assert(GPU_VU_STRUCTURED_QP_NUMERIC_GXP_OFFSET +
		GPU_VU_STRUCTURED_QP_NUMERIC_GXP_MAX_BYTES <=
		GPU_VU_STRUCTURED_FMAC_NUMERIC_GXP_OFFSET);
	static_assert(GPU_VU_STRUCTURED_FMAC_NUMERIC_GXP_OFFSET +
		GPU_VU_STRUCTURED_FMAC_NUMERIC_GXP_MAX_BYTES <=
		GPU_VU_STRUCTURED_FMAC_NATIVE_GXP_OFFSET);
	static_assert(GPU_VU_STRUCTURED_FMAC_NATIVE_GXP_OFFSET +
		GPU_VU_STRUCTURED_FMAC_NATIVE_GXP_MAX_BYTES <=
		GPU_VU_UNIVERSAL_PRODUCT_ESTIMATE_OFFSET);
	static_assert(GPU_VU_UNIVERSAL_PRODUCT_ESTIMATE_OFFSET +
		GPU_VU_UNIVERSAL_ESTIMATE_BYTES <=
		GPU_VU_UNIVERSAL_PRODUCT_ZERO_PAYLOAD_OFFSET);
	static_assert(GPU_VU_UNIVERSAL_PRODUCT_ZERO_PAYLOAD_OFFSET +
		GPU_VU_UNIVERSAL_PAYLOAD_BYTES <=
		GPU_VU_UNIVERSAL_PRODUCT_SHARED_BYTES);
	static_assert(GPU_VU_STRUCTURED_MAXIMUM_JOURNAL_ENTRIES == 64u * 64u * 8u);
	static_assert(GPU_VU_STRUCTURED_JOURNAL_PAYLOAD_BYTES == 512u * 1024u);
	static_assert(VitaGpuVu::StructuredGeneratedMaximumScratchInvocations ==
		64u * 64u);
	static_assert(GPU_VU_STRUCTURED_SCRATCH_BYTES == 2u * 1024u * 1024u);
	static_assert(GPU_VU_STRUCTURED_SCRATCH_BYTES +
		GPU_VU_STRUCTURED_SCRATCH_GUARD_BYTES <=
		GPU_VU_STRUCTURED_JOURNAL_VALUE_BYTES);
	static_assert((GPU_VU_STRUCTURED_SCRATCH_BYTES / sizeof(u32)) - 1u ==
		VitaGpuVu::StructuredGeneratedMaximumScratchInvocations *
			VitaGpuVu::StructuredGeneratedScratchSlots - 1u);
	static_assert(VitaGpuVu::StructuredGeneratedMaximumParallelInvocationsPerJob <=
		GPU_VU_SEQUENTIAL_INDEX_COUNT);
	static_assert(VitaGpuVu::StructuredGeneratedMaximumNumericInvocationsPerJob <=
		GPU_VU_SEQUENTIAL_INDEX_COUNT);
	static_assert(VitaGpuVu::StructuredGeneratedMaximumScratchInvocations <=
		GPU_VU_SEQUENTIAL_INDEX_COUNT);
	static_assert(GPU_VU_UNIVERSAL_PRODUCT_SLOT_BYTES < 4u * 1024u * 1024u);

	enum class UniversalGpuVuValidationStage : u8
	{
		Uninitialized,
		Ready,
		Submitted,
		Passed,
		Failed,
	};

	enum class StructuredNativeFmacAttestation : u8
	{
		Unavailable,
		NearestEven,
		FiniteMantissaChop,
	};

	constexpr const char* STRUCTURED_FMAC_ATTESTATION_RECEIPT_PATH =
		"ux0:data/vitasx2/gpu-vu-structured-fmac-attestation.txt";

	struct RawVuPair
	{
		u32 lower;
		u32 upper;
	};
	static_assert(sizeof(RawVuPair) == VitaGpuVu::UniversalMicroProgramPairBytes);

	constexpr RawVuPair UNIVERSAL_VALIDATION_NOP = {
		0x8000033cu, 0x000002ffu};
	constexpr std::array<std::array<u32, 4>, 4>
		UNIVERSAL_VALIDATION_FTOI_SOURCES = {{
			{{0x00000000u, 0x80000000u, 0x00000001u, 0x80000001u}},
			{{0x3f7fffffu, 0xbf7fffffu, 0x3f800000u, 0xbf800000u}},
			{{0x4effffffu, 0xceffffffu, 0x4f000000u, 0xcf000000u}},
			{{0x7f800000u, 0xff800000u, 0x7fc00000u, 0xffc00000u}},
		}};
	constexpr const char* UNIVERSAL_VALIDATION_RECEIPT_PATH =
		"ux0:data/vitasx2/gpu-vu-universal-validation.txt";
	constexpr u32 UNIVERSAL_VALIDATION_SEMANTIC_PAIR_COUNT = 86;
	constexpr u32 UNIVERSAL_VALIDATION_LONG_NOP_PAIRS = 56;
	constexpr u32 UNIVERSAL_VALIDATION_FIRST_EXECUTE_PAIRS =
		UNIVERSAL_VALIDATION_SEMANTIC_PAIR_COUNT +
		UNIVERSAL_VALIDATION_LONG_NOP_PAIRS;
	constexpr u32 UNIVERSAL_VALIDATION_RESUME_PAIRS = 3;
	constexpr u32 UNIVERSAL_VALIDATION_SOURCE_PAIR_COUNT =
		UNIVERSAL_VALIDATION_FIRST_EXECUTE_PAIRS +
		UNIVERSAL_VALIDATION_RESUME_PAIRS;
	static_assert(UNIVERSAL_VALIDATION_FIRST_EXECUTE_PAIRS > 128);
	static_assert(UNIVERSAL_VALIDATION_FIRST_EXECUTE_PAIRS <=
		VitaGpuVu::UniversalCommandEpochMaximumPairsPerExecute);

	bool WriteUniversalGpuVuValidationReceipt()
	{
		constexpr char receipt[] =
			"v=19 status=passed micro_format=3 record_bytes=44 "
			"source_pair_bytes=8 "
			"pipeline_timing_checked=1 vi_backup_checked=0 "
			"configuration_identity_checked=1 configuration_bits=0x00002c1c "
			"assume_scheduled=0 approximate_q=0 approximate_p=1 "
			"commands_per_invocation=8 execute_commands=2 "
			"unpack_commands_per_invocation=6 unpack_vectors_per_invocation=17 "
			"source_pairs=145 executed_pairs=145 first_execute_pairs=142 "
			"resume_pairs=3 maximum_pairs_per_execute=16384 "
			"long_execute_checked=1 resume_execute_checked=1 "
			"single_invocation_multi_execute=1 mid_scene_flushes=0 "
			"xtop_ops=1 xtop_checked=1 arm_estimate_words=640 "
			"itof_edge_vectors=4 ftoi_edge_vectors=16 "
			"accumulator_ops=13 mac_status_checked=1 numeric_mode=0x000000c1 "
			"qp_checked=1 noninstant_qp_checked=1 instant_qp_checked=0 "
			"fdiv_ops=5 sqrt_ops=2 rsqrt_ops=2 efu_ops=3 "
			"ersadd_ops=1 esqrt_ops=1 approximate_p_checked=1 "
			"integer_memory_ops=4 vi_transfer_ops=3 integer_family_checked=1 "
			"waitq_ops=3 waitp_ops=3 q_consumers=4 p_consumers=4 "
			"qp_old_value_exports=2 waitq_same_pair_consumers=3 "
			"status_snapshots=2 sqrt_status=0x0000 "
			"final_status_snapshot=0x0010 final_status=0x0031 "
			"mfp_checked=1 q_value=0x00000000 p_value=0x404a6244 "
			"status_value=0x0031 "
			"clip_ops=2 clip_checked=1 "
			"gpu_resident_vif_vu_chain=1 "
			"vu_memory_exports=1 "
			"raw_path1_packets=2 raw_path1_qwords=10 xgkick_waits=0 "
			"vu_execute_boundaries=2 completion_boundaries=1 "
			"resources_released=1\n";
		const SceUID fd = sceIoOpen(UNIVERSAL_VALIDATION_RECEIPT_PATH,
			SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
		if (fd < 0)
			return false;

		size_t written = 0;
		bool okay = true;
		while (written < sizeof(receipt) - 1)
		{
			const SceSSize result = sceIoWrite(fd, receipt + written,
				static_cast<SceSize>(sizeof(receipt) - 1 - written));
			if (result <= 0)
			{
				okay = false;
				break;
			}
			written += static_cast<size_t>(result);
		}
		if (okay && sceIoSyncByFd(fd, 0) < 0)
			okay = false;
		if (sceIoClose(fd) < 0)
			okay = false;
		return okay;
	}

	constexpr u32 UniversalValidationLowerImm15(u32 op, u32 it, u32 is,
		u32 immediate)
	{
		return ((op & 0x7fu) << 25) |
			((immediate & 0x7800u) << 10) |
			((it & 0x1fu) << 16) |
			((is & 0x1fu) << 11) |
			(immediate & 0x07ffu);
	}

	constexpr u32 UniversalValidationLowerFlagImm12(u32 op, u32 it,
		u32 immediate)
	{
		return ((op & 0x7fu) << 25) |
			((immediate & 0x0800u) << 10) |
			((it & 0x1fu) << 16) |
			(immediate & 0x07ffu);
	}

	constexpr u32 UniversalValidationUpper(u32 op, u32 mask, u32 ft,
		u32 fs, u32 fd)
	{
		return ((mask & 0x0fu) << 21) |
			((ft & 0x1fu) << 16) |
			((fs & 0x1fu) << 11) |
			((fd & 0x1fu) << 6) |
			(op & 0x3fu);
	}

	constexpr u32 UniversalValidationLowerMemory(u32 op, u32 mask, u32 it,
		u32 is, s32 immediate)
	{
		return ((op & 0x7fu) << 25) |
			((mask & 0x0fu) << 21) |
			((it & 0x1fu) << 16) |
			((is & 0x1fu) << 11) |
			(static_cast<u32>(immediate) & 0x07ffu);
	}

	constexpr u32 UniversalValidationLowerT3(u32 table, u32 index,
		u32 mask, u32 ft, u32 fs)
	{
		return (0x40u << 25) |
			((mask & 0x0fu) << 21) |
			((ft & 0x1fu) << 16) |
			((fs & 0x1fu) << 11) |
			((index & 0x1fu) << 6) |
			(table & 0x3fu);
	}

#endif
	// vitaGL's GXM owner records eight as libGXM's per-target maximum. Sony's
	// macrotile_sync and tutorial_postprocessing samples raise scenesPerFrame
	// for targets used by several ordered passes instead of leaving it at one.
	constexpr u16 MAX_GXM_SCENES_PER_RENDER_TARGET = 8;
	// Keep multiple fully provisioned firmware owners for hot geometries. One
	// object per size serializes the ninth scene on the first object's bounded
	// scene resources, which is pathological for the BIOS multi-pass workload.
	constexpr u32 MAX_GXM_RENDER_TARGET_OBJECTS = 24;
	constexpr u32 MAX_GXM_RENDER_TARGETS_PER_GEOMETRY = 6;

	struct TfxVertex
	{
		float st[2];
		u8 rgba[4];
		float q;
		float position[2];
		float depth;
		float uv[2];
		u8 fog[4];
	};
	static_assert(sizeof(TfxVertex) == 40);

	struct QuadVertex
	{
		float x;
		float y;
		float z;
		u8 r;
		u8 g;
		u8 b;
		u8 a;
		float u;
		float v;
	};
	static_assert(sizeof(QuadVertex) == 24);

	void* PatcherHostAlloc(void*, SceSize size)
	{
		return std::malloc(size);
	}

	void PatcherHostFree(void*, void* memory)
	{
		std::free(memory);
	}

	float PixelToNdc(float value, u32 extent)
	{
		return (2.0f * value / static_cast<float>(extent)) - 1.0f;
	}

	SceGxmDepthFunc TranslateDepthFunc(u8 ztst)
	{
		static constexpr std::array<SceGxmDepthFunc, 4> functions = {
			SCE_GXM_DEPTH_FUNC_NEVER,
			SCE_GXM_DEPTH_FUNC_ALWAYS,
			SCE_GXM_DEPTH_FUNC_GREATER_EQUAL,
			SCE_GXM_DEPTH_FUNC_GREATER,
		};
		return functions[std::min<u8>(ztst, 3)];
	}

	SceGxmPrimitiveType TranslateTopology(GSHWDrawConfig::Topology topology)
	{
		switch (topology)
		{
			case GSHWDrawConfig::Topology::Point:
				return SCE_GXM_PRIMITIVE_POINTS;
			case GSHWDrawConfig::Topology::Line:
				return SCE_GXM_PRIMITIVE_LINES;
			case GSHWDrawConfig::Topology::Triangle:
			default:
				return SCE_GXM_PRIMITIVE_TRIANGLES;
		}
	}

	u8 TranslateColorMask(u8 mask)
	{
		// PCSX2: R/G/B/A are bits 0/1/2/3. Sony GXM: A/R/G/B are
		// bits 0/1/2/3. Sony's api_libgxm/blending sample establishes that
		// colorMask is fixed when the fragment program is patched.
		u8 result = SCE_GXM_COLOR_MASK_NONE;
		if (mask & 0x1)
			result |= SCE_GXM_COLOR_MASK_R;
		if (mask & 0x2)
			result |= SCE_GXM_COLOR_MASK_G;
		if (mask & 0x4)
			result |= SCE_GXM_COLOR_MASK_B;
		if (mask & 0x8)
			result |= SCE_GXM_COLOR_MASK_A;
		return result;
	}

	bool TranslateBlendFactor(u8 factor, SceGxmBlendFactor* result)
	{
		if (!result)
			return false;
		switch (factor)
		{
			case GSDevice::SRC_COLOR: *result = SCE_GXM_BLEND_FACTOR_SRC_COLOR; break;
			case GSDevice::INV_SRC_COLOR: *result = SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_COLOR; break;
			case GSDevice::DST_COLOR: *result = SCE_GXM_BLEND_FACTOR_DST_COLOR; break;
			case GSDevice::INV_DST_COLOR: *result = SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_COLOR; break;
			case GSDevice::SRC_ALPHA: *result = SCE_GXM_BLEND_FACTOR_SRC_ALPHA; break;
			case GSDevice::INV_SRC_ALPHA: *result = SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; break;
			case GSDevice::DST_ALPHA: *result = SCE_GXM_BLEND_FACTOR_DST_ALPHA; break;
			case GSDevice::INV_DST_ALPHA: *result = SCE_GXM_BLEND_FACTOR_ONE_MINUS_DST_ALPHA; break;
			case GSDevice::CONST_ONE: *result = SCE_GXM_BLEND_FACTOR_ONE; break;
			case GSDevice::CONST_ZERO: *result = SCE_GXM_BLEND_FACTOR_ZERO; break;
			default: return false;
		}
		return true;
	}

	bool CanPatchTfxBlend(const GSHWDrawConfig& config)
	{
		const GSHWDrawConfig::ColorMaskSelector mask(
			config.ps.no_color ? 0 : config.colormask.wrgba);
		if (!config.blend.IsEffective(mask) || !config.blend.enable)
			return true;
		if (config.blend.constant_enable ||
			config.blend.op > GSDevice::OP_REV_SUBTRACT)
		{
			return false;
		}
		SceGxmBlendFactor ignored{};
		return TranslateBlendFactor(config.blend.src_factor, &ignored) &&
			TranslateBlendFactor(config.blend.dst_factor, &ignored) &&
			TranslateBlendFactor(config.blend.src_factor_alpha, &ignored) &&
			TranslateBlendFactor(config.blend.dst_factor_alpha, &ignored);
	}

	u64 GetUnsupportedTfxFeatureMask(const GSHWDrawConfig& config,
		bool invalid_destination_format, bool invalid_dither,
		bool invalid_mip_filter)
	{
		u64 features = 0;
		if (config.pal)
			features |= GxmTfxRejectPaletteTexture;
		if (config.ps.pal_fmt)
			features |= GxmTfxRejectPaletteFormat;
		if (config.ps.depth_fmt)
			features |= GxmTfxRejectDepthFormat;
		if (invalid_destination_format)
			features |= GxmTfxRejectDestinationFormat;
		if (invalid_dither)
			features |= GxmTfxRejectDither;
		if (config.ps.afail != GSHWDrawConfig::PS_AFAIL::KEEP)
			features |= GxmTfxRejectAlphaFail;
		if (config.ps.ztst)
			features |= GxmTfxRejectZTest;
		if (config.ps.shuffle || config.ps.shuffle_same || config.ps.real16src ||
			config.ps.process_ba || config.ps.process_rg ||
			config.ps.shuffle_across || config.ps.write_rg || config.ps.a_masked)
		{
			features |= GxmTfxRejectShuffle;
		}
		if (config.ps.channel)
			features |= GxmTfxRejectChannelFetch;
		if (config.ps.colclip_hw)
			features |= GxmTfxRejectColorClip;
		if (config.ps.urban_chaos_hle || config.ps.tales_of_abyss_hle)
			features |= GxmTfxRejectGameHle;
		if (config.ps.point_sampler)
			features |= GxmTfxRejectPointSampler;
		if (config.ps.sw_aniso)
			features |= GxmTfxRejectSoftwareAnisotropy;
		if (config.ps.scanmsk)
			features |= GxmTfxRejectScanMask;
		if (config.ps.aa1 != GSHWDrawConfig::PS_AA1::NONE)
			features |= GxmTfxRejectAa1;
		if (config.ps.rov_color ||
			config.ps.rov_depth != GSHWDrawConfig::PS_ROV_DEPTH::NONE)
		{
			features |= GxmTfxRejectRov;
		}
		if (config.ps.wms == 2 || config.ps.wmt == 2)
			features |= GxmTfxRejectRegionClamp;
		if (config.ps.zclamp)
			features |= GxmTfxRejectZClamp;
		if (config.ps.tcoffsethack)
			features |= GxmTfxRejectTextureOffset;
		if (config.ps.tex_is_fb)
			features |= GxmTfxRejectTextureIsFramebuffer;
		if (config.ps.date || config.ps.abe)
			features |= GxmTfxRejectDateOrAbe;
		if (config.depth.date || config.depth.date_one)
			features |= GxmTfxRejectDepthDate;
		if (invalid_mip_filter)
			features |= GxmTfxRejectMipFilter;
		return features;
	}

	u64 RecordRejectedTfxDraw(const GSHWDrawConfig& config, u64 features)
	{
		VitaGxmPerformanceCounters& counters = s_gxm_worker_performance;
		counters.rejected_tfx_draws++;
		counters.last_rejected_tfx_features = features;
		counters.last_rejected_tfx_ps_lo = config.ps.key_lo;
		counters.last_rejected_tfx_ps_hi = config.ps.key_hi;
		counters.last_rejected_tfx_blend = config.blend.key;
		counters.last_rejected_tfx_vs = config.vs.key;
		counters.last_rejected_tfx_sampler = config.sampler.key;
		counters.last_rejected_tfx_depth = config.depth.key;
		counters.last_rejected_tfx_colormask = config.colormask.key;
		counters.last_rejected_tfx_topology = static_cast<u8>(config.topology);
		return counters.rejected_tfx_draws;
	}

	u32 HashRejectReason(const char* reason)
	{
		// Stable FNV-1a lets the bounded record identify every non-TFX rejection
		// without copying strings or logging on the hot draw path. The literal
		// remains the authoritative decoder at each GSDeviceGXM::Reject() callsite.
		u32 hash = 2166136261u;
		for (const unsigned char* p =
			reinterpret_cast<const unsigned char*>(reason); p && *p; p++)
		{
			hash = (hash ^ *p) * 16777619u;
		}
		return hash;
	}

	u64 HashGpuVuDiagnosticBytes(const void* data, size_t size,
		u32* sampled_bytes)
	{
		// This is a breadcrumb identity, never an admission proof. Hash every byte
		// of the small generated record/index allocations and sixteen evenly spaced
		// cache lines of a large raw-input window. The latter bounds diagnostic CPU2
		// work while still distinguishing the guest-state phase which reached GXM.
		constexpr size_t SampleChunkBytes = 64u;
		constexpr size_t MaximumFullHashBytes = 4096u;
		constexpr size_t LargeSampleChunks = 16u;
		if (sampled_bytes)
			*sampled_bytes = 0u;
		if (!data || size == 0u)
			return 0u;

		const u8* const bytes = static_cast<const u8*>(data);
		u64 hash = 14695981039346656037ull;
		const auto include = [&hash](const u8* begin, size_t count) {
			for (size_t index = 0u; index < count; index++)
				hash = (hash ^ begin[index]) * 1099511628211ull;
		};
		if (size <= MaximumFullHashBytes)
		{
			include(bytes, size);
			if (sampled_bytes)
				*sampled_bytes = static_cast<u32>(size);
			return hash;
		}

		for (size_t sample = 0u; sample < LargeSampleChunks; sample++)
		{
			const size_t maximum_offset = size - SampleChunkBytes;
			const size_t offset = maximum_offset * sample /
				(LargeSampleChunks - 1u);
			include(bytes + offset, SampleChunkBytes);
		}
		if (sampled_bytes)
			*sampled_bytes = static_cast<u32>(
				SampleChunkBytes * LargeSampleChunks);
		return hash;
	}

	bool GpuVuDiagnosticRangesOverlap(uptr first, size_t first_size,
		uptr second, size_t second_size)
	{
		if (first == 0u || second == 0u || first_size == 0u || second_size == 0u)
			return false;
		return first < second ? second - first < first_size :
			first - second < second_size;
	}

	u64 PackTelemetryRect(const GSVector4i& rect)
	{
		// Preserve signed 16-bit GS coordinates in a compact, parser-friendly
		// x/y/z/w word. GSRendererHW's draw/sample bounds are within this range.
		return static_cast<u64>(static_cast<u16>(rect.x)) |
			(static_cast<u64>(static_cast<u16>(rect.y)) << 16) |
			(static_cast<u64>(static_cast<u16>(rect.z)) << 32) |
			(static_cast<u64>(static_cast<u16>(rect.w)) << 48);
	}

	void RecordAcceptedTfxDraw(const GSHWDrawConfig& config)
	{
		if (!VitaPerformanceTelemetry::IsEnabled())
			return;
		VitaGxmPerformanceCounters& counters = s_gxm_worker_performance;
		counters.tfx_draws++;
		if (config.vs.tme)
			counters.textured_tfx_draws++;
		if (config.tex && config.tex->IsRenderTarget())
			counters.render_target_source_draws++;
		if (config.tex && config.tex->IsDepthLike())
			counters.depth_source_draws++;
		if (config.tex_hazard == GSHWDrawConfig::TEX_HAZARD_RT)
			counters.rt_hazard_draws++;
		else if (config.tex_hazard == GSHWDrawConfig::TEX_HAZARD_DEPTH)
			counters.depth_hazard_draws++;
		const bool feedback_rt = config.IsFeedbackLoopRT(config.ps);
		const bool feedback_depth = config.IsFeedbackLoopDepth(config.ps);
		if (feedback_rt)
			counters.feedback_rt_draws++;
		if (feedback_depth)
			counters.feedback_depth_draws++;
		if (config.ps.IsSWBlending())
			counters.software_blend_draws++;
		if (config.blend.enable)
			counters.fixed_blend_draws++;
		if (config.ps.IsAlphaTesting())
			counters.alpha_test_draws++;
		if (config.colormask.wrgba != 0xf)
			counters.partial_color_mask_draws++;
		if (feedback_rt || feedback_depth ||
			config.tex_hazard != GSHWDrawConfig::TEX_HAZARD_NONE)
		{
			counters.last_feedback_ps_lo = config.ps.key_lo;
			counters.last_feedback_ps_hi = config.ps.key_hi;
			counters.last_feedback_blend = config.blend.key;
			counters.last_feedback_vs = config.vs.key;
			counters.last_feedback_sampler = config.sampler.key;
			counters.last_feedback_depth = config.depth.key;
			counters.last_feedback_colormask = config.colormask.key;
			counters.last_feedback_topology = static_cast<u8>(config.topology);
			counters.last_feedback_hazard = static_cast<u8>(config.tex_hazard);
		}
	}

	void SetQuadVertex(QuadVertex& vertex, float x, float y, u32 color,
		float u, float v)
	{
		vertex.x = x;
		vertex.y = y;
		vertex.z = 0.0f;
		vertex.r = static_cast<u8>(color);
		vertex.g = static_cast<u8>(color >> 8);
		vertex.b = static_cast<u8>(color >> 16);
		vertex.a = static_cast<u8>(color >> 24);
		vertex.u = u;
		vertex.v = v;
	}

	template <typename T>
	T* CheckedCast(GSTexture* texture)
	{
		return texture ? static_cast<T*>(texture) : nullptr;
	}
} // namespace

void VitaGxmRecordRendererDrawTime(u64 elapsed_us)
{
	if (!VitaPerformanceTelemetry::IsEnabled())
		return;
	s_gxm_worker_performance.renderer_draw_calls++;
	s_gxm_worker_performance.renderer_draw_wall_us += elapsed_us;
}

void VitaGxmRecordRendererStageTime(VitaGxmRendererStage stage, u64 elapsed_us)
{
	if (!VitaPerformanceTelemetry::IsEnabled())
		return;
	auto& counters = s_gxm_worker_performance.renderer_stages[static_cast<size_t>(stage)];
	counters.calls++;
	counters.wall_us += elapsed_us;
	counters.max_lifetime_us = std::max(counters.max_lifetime_us, elapsed_us);
}

void VitaGxmPublishPerformanceCounters()
{
	if (!VitaPerformanceTelemetry::IsEnabled())
		return;
	for (size_t i = 0; i < s_gxm_published_tfx_programs.size(); i++)
	{
		const auto& local = s_gxm_worker_performance.tfx_programs[i];
		auto& published = s_gxm_published_tfx_programs[i];
		published.draws.store(local.draws, std::memory_order_relaxed);
		published.indices.store(local.indices, std::memory_order_relaxed);
		published.draw_rect_pixels.store(local.draw_rect_pixels, std::memory_order_relaxed);
		published.no_tests_draws.store(local.no_tests_draws, std::memory_order_relaxed);
		published.no_tests_rect_pixels.store(local.no_tests_rect_pixels, std::memory_order_relaxed);
	}
	for (size_t i = 0; i < s_gxm_published_scene_transitions.size(); i++)
	{
		const auto& local = s_gxm_worker_performance.scene_transitions[i];
		auto& published = s_gxm_published_scene_transitions[i];
		published.transitions.store(local.transitions, std::memory_order_relaxed);
		published.clear_calls.store(local.clear_calls, std::memory_order_relaxed);
		published.clear_followups.store(local.clear_followups, std::memory_order_relaxed);
		published.clear_only_followups.store(local.clear_only_followups, std::memory_order_relaxed);
	}
	for (size_t i = 0; i < s_gxm_published_scene_contents.size(); i++)
	{
		const auto& local = s_gxm_worker_performance.scene_contents[i];
		auto& published = s_gxm_published_scene_contents[i];
		published.scenes.store(local.scenes, std::memory_order_relaxed);
		published.end_scene_wall_us.store(local.end_scene_wall_us, std::memory_order_relaxed);
		s_gxm_published_unsupported_passes[i].store(
			s_gxm_worker_performance.unsupported_passes[i], std::memory_order_relaxed);
	}
	for (size_t i = 0; i < s_gxm_published_host_calls.size(); i++)
	{
		const auto& local = s_gxm_worker_performance.host_calls[i];
		auto& published = s_gxm_published_host_calls[i];
		published.calls.store(local.calls, std::memory_order_relaxed);
		published.wall_us.store(local.wall_us, std::memory_order_relaxed);
		published.max_lifetime_us.store(local.max_lifetime_us, std::memory_order_relaxed);
	}
	for (size_t i = 0; i < s_gxm_published_renderer_stages.size(); i++)
	{
		const auto& local = s_gxm_worker_performance.renderer_stages[i];
		auto& published = s_gxm_published_renderer_stages[i];
		published.calls.store(local.calls, std::memory_order_relaxed);
		published.wall_us.store(local.wall_us, std::memory_order_relaxed);
		published.max_lifetime_us.store(local.max_lifetime_us, std::memory_order_relaxed);
	}
	s_gxm_published_draw_calls.store(s_gxm_worker_performance.draw_calls,
		std::memory_order_relaxed);
	s_gxm_published_renderer_draw_calls.store(
		s_gxm_worker_performance.renderer_draw_calls, std::memory_order_relaxed);
	s_gxm_published_renderer_draw_wall_us.store(
		s_gxm_worker_performance.renderer_draw_wall_us, std::memory_order_relaxed);
	s_gxm_published_device_render_calls.store(
		s_gxm_worker_performance.device_render_calls, std::memory_order_relaxed);
	s_gxm_published_device_render_wall_us.store(
		s_gxm_worker_performance.device_render_wall_us, std::memory_order_relaxed);
	s_gxm_published_draw_indices.store(s_gxm_worker_performance.draw_indices,
		std::memory_order_relaxed);
	s_gxm_published_gpu_vu_draw_calls.store(
		s_gxm_worker_performance.gpu_vu_draw_calls,
		std::memory_order_relaxed);
	s_gxm_published_gpu_vu_draw_indices.store(
		s_gxm_worker_performance.gpu_vu_draw_indices,
		std::memory_order_relaxed);
	s_gxm_published_gpu_vu_descriptor_objects.store(
		s_gxm_worker_performance.gpu_vu_descriptor_objects,
		std::memory_order_relaxed);
	s_gxm_published_vertex_upload_bytes.store(
		s_gxm_worker_performance.vertex_upload_bytes, std::memory_order_relaxed);
	s_gxm_published_index_upload_bytes.store(
		s_gxm_worker_performance.index_upload_bytes, std::memory_order_relaxed);
	s_gxm_published_texture_uploads.store(s_gxm_worker_performance.texture_uploads,
		std::memory_order_relaxed);
	s_gxm_published_texture_upload_bytes.store(
		s_gxm_worker_performance.texture_upload_bytes, std::memory_order_relaxed);
	s_gxm_published_render_store_acquisitions.store(
		s_gxm_worker_performance.render_store_acquisitions,
		std::memory_order_relaxed);
	s_gxm_published_render_store_new_residencies.store(
		s_gxm_worker_performance.render_store_new_residencies,
		std::memory_order_relaxed);
	s_gxm_published_render_store_resident_switches.store(
		s_gxm_worker_performance.render_store_resident_switches,
		std::memory_order_relaxed);
	s_gxm_published_render_store_physical_scenes.store(
		s_gxm_worker_performance.render_store_physical_scenes,
		std::memory_order_relaxed);
	s_gxm_published_render_store_loads.store(
		s_gxm_worker_performance.render_store_loads,
		std::memory_order_relaxed);
	s_gxm_published_render_store_materializations.store(
		s_gxm_worker_performance.render_store_materializations,
		std::memory_order_relaxed);
	s_gxm_published_render_store_failures.store(
		s_gxm_worker_performance.render_store_failures,
		std::memory_order_relaxed);
	s_gxm_published_texture_readbacks.store(
		s_gxm_worker_performance.texture_readbacks, std::memory_order_relaxed);
	s_gxm_published_texture_readback_bytes.store(
		s_gxm_worker_performance.texture_readback_bytes, std::memory_order_relaxed);
	s_gxm_published_tfx_draws.store(s_gxm_worker_performance.tfx_draws,
		std::memory_order_relaxed);
	s_gxm_published_textured_tfx_draws.store(
		s_gxm_worker_performance.textured_tfx_draws, std::memory_order_relaxed);
	s_gxm_published_render_target_source_draws.store(
		s_gxm_worker_performance.render_target_source_draws,
		std::memory_order_relaxed);
	s_gxm_published_depth_source_draws.store(
		s_gxm_worker_performance.depth_source_draws, std::memory_order_relaxed);
	s_gxm_published_rt_hazard_draws.store(
		s_gxm_worker_performance.rt_hazard_draws, std::memory_order_relaxed);
	s_gxm_published_depth_hazard_draws.store(
		s_gxm_worker_performance.depth_hazard_draws, std::memory_order_relaxed);
	s_gxm_published_feedback_rt_draws.store(
		s_gxm_worker_performance.feedback_rt_draws, std::memory_order_relaxed);
	s_gxm_published_feedback_depth_draws.store(
		s_gxm_worker_performance.feedback_depth_draws, std::memory_order_relaxed);
	s_gxm_published_software_blend_draws.store(
		s_gxm_worker_performance.software_blend_draws, std::memory_order_relaxed);
	s_gxm_published_fixed_blend_draws.store(
		s_gxm_worker_performance.fixed_blend_draws, std::memory_order_relaxed);
	s_gxm_published_alpha_test_draws.store(
		s_gxm_worker_performance.alpha_test_draws, std::memory_order_relaxed);
	s_gxm_published_partial_color_mask_draws.store(
		s_gxm_worker_performance.partial_color_mask_draws,
		std::memory_order_relaxed);
	s_gxm_published_feedback_snapshots.store(
		s_gxm_worker_performance.feedback_snapshots, std::memory_order_relaxed);
	s_gxm_published_feedback_snapshot_bytes.store(
		s_gxm_worker_performance.feedback_snapshot_bytes,
		std::memory_order_relaxed);
	s_gxm_published_merge_calls.store(s_gxm_worker_performance.merge_calls,
		std::memory_order_relaxed);
	s_gxm_published_merge_rc1_draws.store(
		s_gxm_worker_performance.merge_rc1_draws, std::memory_order_relaxed);
	s_gxm_published_merge_rc2_draws.store(
		s_gxm_worker_performance.merge_rc2_draws, std::memory_order_relaxed);
	s_gxm_published_present_calls.store(s_gxm_worker_performance.present_calls,
		std::memory_order_relaxed);
	s_gxm_published_interlace_calls.store(
		s_gxm_worker_performance.interlace_calls, std::memory_order_relaxed);
	s_gxm_published_last_merge_pmode.store(
		s_gxm_worker_performance.last_merge_pmode, std::memory_order_relaxed);
	s_gxm_published_last_merge_extbuf.store(
		s_gxm_worker_performance.last_merge_extbuf, std::memory_order_relaxed);
	s_gxm_published_last_merge_background.store(
		s_gxm_worker_performance.last_merge_background,
		std::memory_order_relaxed);
	s_gxm_published_last_merge_source_size_1.store(
		s_gxm_worker_performance.last_merge_source_sizes[0],
		std::memory_order_relaxed);
	s_gxm_published_last_merge_source_size_2.store(
		s_gxm_worker_performance.last_merge_source_sizes[1],
		std::memory_order_relaxed);
	s_gxm_published_last_merge_source_id_1.store(
		s_gxm_worker_performance.last_merge_source_ids[0],
		std::memory_order_relaxed);
	s_gxm_published_last_merge_source_id_2.store(
		s_gxm_worker_performance.last_merge_source_ids[1],
		std::memory_order_relaxed);
	s_gxm_published_last_merge_source_mask.store(
		s_gxm_worker_performance.last_merge_source_mask,
		std::memory_order_relaxed);
	s_gxm_published_last_merge_source_states.store(
		s_gxm_worker_performance.last_merge_source_states,
		std::memory_order_relaxed);
	s_gxm_published_last_merge_writer_tfx_writes.store(
		s_gxm_worker_performance.last_merge_writer_tfx_writes,
		std::memory_order_relaxed);
	s_gxm_published_last_merge_writer_ps_lo.store(
		s_gxm_worker_performance.last_merge_writer_ps_lo,
		std::memory_order_relaxed);
	s_gxm_published_last_merge_writer_ps_hi.store(
		s_gxm_worker_performance.last_merge_writer_ps_hi,
		std::memory_order_relaxed);
	s_gxm_published_last_merge_writer_draw_area.store(
		s_gxm_worker_performance.last_merge_writer_draw_area,
		std::memory_order_relaxed);
	s_gxm_published_last_merge_writer_sample_area.store(
		s_gxm_worker_performance.last_merge_writer_sample_area,
		std::memory_order_relaxed);
	s_gxm_published_last_merge_writer_source_id.store(
		s_gxm_worker_performance.last_merge_writer_source_id,
		std::memory_order_relaxed);
	s_gxm_published_last_merge_writer_source_size.store(
		s_gxm_worker_performance.last_merge_writer_source_size,
		std::memory_order_relaxed);
	s_gxm_published_last_merge_writer_blend.store(
		s_gxm_worker_performance.last_merge_writer_blend,
		std::memory_order_relaxed);
	s_gxm_published_last_merge_writer_selector_keys.store(
		s_gxm_worker_performance.last_merge_writer_selector_keys,
		std::memory_order_relaxed);
	s_gxm_published_last_merge_trace_circuit.store(
		s_gxm_worker_performance.last_merge_trace_circuit,
		std::memory_order_relaxed);
	s_gxm_published_last_merge_writer_kind.store(
		s_gxm_worker_performance.last_merge_writer_kind,
		std::memory_order_relaxed);
	s_gxm_published_last_merge_writer_topology.store(
		s_gxm_worker_performance.last_merge_writer_topology,
		std::memory_order_relaxed);
	s_gxm_published_last_merge_parent_tfx_writes.store(
		s_gxm_worker_performance.last_merge_parent_tfx_writes,
		std::memory_order_relaxed);
	s_gxm_published_last_merge_parent_textured_tfx_writes.store(
		s_gxm_worker_performance.last_merge_parent_textured_tfx_writes,
		std::memory_order_relaxed);
	s_gxm_published_last_merge_parent_untextured_tfx_writes.store(
		s_gxm_worker_performance.last_merge_parent_untextured_tfx_writes,
		std::memory_order_relaxed);
	s_gxm_published_last_merge_parent_rt_source_tfx_writes.store(
		s_gxm_worker_performance.last_merge_parent_render_target_source_tfx_writes,
		std::memory_order_relaxed);
	s_gxm_published_last_merge_parent_full_mask_tfx_writes.store(
		s_gxm_worker_performance.last_merge_parent_full_mask_tfx_writes,
		std::memory_order_relaxed);
	s_gxm_published_last_merge_parent_rgb_only_tfx_writes.store(
		s_gxm_worker_performance.last_merge_parent_rgb_only_tfx_writes,
		std::memory_order_relaxed);
	s_gxm_published_last_merge_parent_alpha_only_tfx_writes.store(
		s_gxm_worker_performance.last_merge_parent_alpha_only_tfx_writes,
		std::memory_order_relaxed);
	s_gxm_published_last_merge_parent_other_mask_tfx_writes.store(
		s_gxm_worker_performance.last_merge_parent_other_mask_tfx_writes,
		std::memory_order_relaxed);
	s_gxm_published_last_merge_parent_ps_lo.store(
		s_gxm_worker_performance.last_merge_parent_ps_lo,
		std::memory_order_relaxed);
	s_gxm_published_last_merge_parent_ps_hi.store(
		s_gxm_worker_performance.last_merge_parent_ps_hi,
		std::memory_order_relaxed);
	s_gxm_published_last_merge_parent_draw_area.store(
		s_gxm_worker_performance.last_merge_parent_draw_area,
		std::memory_order_relaxed);
	s_gxm_published_last_merge_parent_sample_area.store(
		s_gxm_worker_performance.last_merge_parent_sample_area,
		std::memory_order_relaxed);
	s_gxm_published_last_merge_parent_source_id.store(
		s_gxm_worker_performance.last_merge_parent_source_id,
		std::memory_order_relaxed);
	s_gxm_published_last_merge_parent_source_size.store(
		s_gxm_worker_performance.last_merge_parent_source_size,
		std::memory_order_relaxed);
	s_gxm_published_last_merge_parent_blend.store(
		s_gxm_worker_performance.last_merge_parent_blend,
		std::memory_order_relaxed);
	s_gxm_published_last_merge_parent_selector_keys.store(
		s_gxm_worker_performance.last_merge_parent_selector_keys,
		std::memory_order_relaxed);
	s_gxm_published_last_merge_parent_last_rgb_ps_lo.store(
		s_gxm_worker_performance.last_merge_parent_last_rgb_ps_lo,
		std::memory_order_relaxed);
	s_gxm_published_last_merge_parent_last_rgb_ps_hi.store(
		s_gxm_worker_performance.last_merge_parent_last_rgb_ps_hi,
		std::memory_order_relaxed);
	s_gxm_published_last_merge_parent_last_rgb_draw_area.store(
		s_gxm_worker_performance.last_merge_parent_last_rgb_draw_area,
		std::memory_order_relaxed);
	s_gxm_published_last_merge_parent_last_rgb_sample_area.store(
		s_gxm_worker_performance.last_merge_parent_last_rgb_sample_area,
		std::memory_order_relaxed);
	s_gxm_published_last_merge_parent_last_rgb_source_id.store(
		s_gxm_worker_performance.last_merge_parent_last_rgb_source_id,
		std::memory_order_relaxed);
	s_gxm_published_last_merge_parent_last_rgb_source_size.store(
		s_gxm_worker_performance.last_merge_parent_last_rgb_source_size,
		std::memory_order_relaxed);
	s_gxm_published_last_merge_parent_last_rgb_blend.store(
		s_gxm_worker_performance.last_merge_parent_last_rgb_blend,
		std::memory_order_relaxed);
	s_gxm_published_last_merge_parent_last_rgb_selector_keys.store(
		s_gxm_worker_performance.last_merge_parent_last_rgb_selector_keys,
		std::memory_order_relaxed);
	s_gxm_published_last_merge_parent_kind.store(
		s_gxm_worker_performance.last_merge_parent_kind,
		std::memory_order_relaxed);
	s_gxm_published_last_merge_parent_topology.store(
		s_gxm_worker_performance.last_merge_parent_topology,
		std::memory_order_relaxed);
	s_gxm_published_last_merge_parent_color_mask.store(
		s_gxm_worker_performance.last_merge_parent_color_mask,
		std::memory_order_relaxed);
	s_gxm_published_last_merge_parent_last_rgb_topology.store(
		s_gxm_worker_performance.last_merge_parent_last_rgb_topology,
		std::memory_order_relaxed);
	s_gxm_published_last_merge_parent_last_rgb_color_mask.store(
		s_gxm_worker_performance.last_merge_parent_last_rgb_color_mask,
		std::memory_order_relaxed);
	s_gxm_published_psm24_draws.store(s_gxm_worker_performance.psm24_draws,
		std::memory_order_relaxed);
	s_gxm_published_last_device_reject_hash.store(
		s_gxm_worker_performance.last_device_reject_hash,
		std::memory_order_relaxed);
	s_gxm_published_last_feedback_ps_lo.store(
		s_gxm_worker_performance.last_feedback_ps_lo, std::memory_order_relaxed);
	s_gxm_published_last_feedback_ps_hi.store(
		s_gxm_worker_performance.last_feedback_ps_hi, std::memory_order_relaxed);
	s_gxm_published_last_feedback_blend.store(
		s_gxm_worker_performance.last_feedback_blend, std::memory_order_relaxed);
	s_gxm_published_last_feedback_vs.store(
		s_gxm_worker_performance.last_feedback_vs, std::memory_order_relaxed);
	s_gxm_published_last_feedback_sampler.store(
		s_gxm_worker_performance.last_feedback_sampler, std::memory_order_relaxed);
	s_gxm_published_last_feedback_depth.store(
		s_gxm_worker_performance.last_feedback_depth, std::memory_order_relaxed);
	s_gxm_published_last_feedback_colormask.store(
		s_gxm_worker_performance.last_feedback_colormask,
		std::memory_order_relaxed);
	s_gxm_published_last_feedback_topology.store(
		s_gxm_worker_performance.last_feedback_topology,
		std::memory_order_relaxed);
	s_gxm_published_last_feedback_hazard.store(
		s_gxm_worker_performance.last_feedback_hazard, std::memory_order_relaxed);
	// Publish the monotonic device-rejection count after its identity.
	s_gxm_published_device_rejects.store(
		s_gxm_worker_performance.device_rejects, std::memory_order_release);
	s_gxm_published_last_rejected_tfx_features.store(
		s_gxm_worker_performance.last_rejected_tfx_features,
		std::memory_order_relaxed);
	s_gxm_published_last_rejected_tfx_ps_lo.store(
		s_gxm_worker_performance.last_rejected_tfx_ps_lo,
		std::memory_order_relaxed);
	s_gxm_published_last_rejected_tfx_ps_hi.store(
		s_gxm_worker_performance.last_rejected_tfx_ps_hi,
		std::memory_order_relaxed);
	s_gxm_published_last_rejected_tfx_blend.store(
		s_gxm_worker_performance.last_rejected_tfx_blend,
		std::memory_order_relaxed);
	s_gxm_published_last_rejected_tfx_vs.store(
		s_gxm_worker_performance.last_rejected_tfx_vs, std::memory_order_relaxed);
	s_gxm_published_last_rejected_tfx_sampler.store(
		s_gxm_worker_performance.last_rejected_tfx_sampler,
		std::memory_order_relaxed);
	s_gxm_published_last_rejected_tfx_depth.store(
		s_gxm_worker_performance.last_rejected_tfx_depth,
		std::memory_order_relaxed);
	s_gxm_published_last_rejected_tfx_colormask.store(
		s_gxm_worker_performance.last_rejected_tfx_colormask,
		std::memory_order_relaxed);
	s_gxm_published_last_rejected_tfx_topology.store(
		s_gxm_worker_performance.last_rejected_tfx_topology,
		std::memory_order_relaxed);
	// Publish the monotonic rejection count last. Readers acquire it before
	// consuming the associated last-rejection identity.
	s_gxm_published_rejected_tfx_draws.store(
		s_gxm_worker_performance.rejected_tfx_draws, std::memory_order_release);
}

VitaGxmPerformanceCounters VitaGxmGetPublishedPerformanceCounters()
{
	VitaGxmPerformanceCounters counters;
	for (size_t i = 0; i < s_gxm_published_tfx_programs.size(); i++)
	{
		const auto& published = s_gxm_published_tfx_programs[i];
		auto& local = counters.tfx_programs[i];
		local.draws = published.draws.load(std::memory_order_relaxed);
		local.indices = published.indices.load(std::memory_order_relaxed);
		local.draw_rect_pixels = published.draw_rect_pixels.load(std::memory_order_relaxed);
		local.no_tests_draws = published.no_tests_draws.load(std::memory_order_relaxed);
		local.no_tests_rect_pixels = published.no_tests_rect_pixels.load(std::memory_order_relaxed);
	}
	for (size_t i = 0; i < s_gxm_published_scene_transitions.size(); i++)
	{
		const auto& published = s_gxm_published_scene_transitions[i];
		auto& local = counters.scene_transitions[i];
		local.transitions = published.transitions.load(std::memory_order_relaxed);
		local.clear_calls = published.clear_calls.load(std::memory_order_relaxed);
		local.clear_followups = published.clear_followups.load(std::memory_order_relaxed);
		local.clear_only_followups = published.clear_only_followups.load(std::memory_order_relaxed);
	}
	for (size_t i = 0; i < s_gxm_published_scene_contents.size(); i++)
	{
		const auto& published = s_gxm_published_scene_contents[i];
		auto& local = counters.scene_contents[i];
		local.scenes = published.scenes.load(std::memory_order_relaxed);
		local.end_scene_wall_us = published.end_scene_wall_us.load(std::memory_order_relaxed);
		counters.unsupported_passes[i] =
			s_gxm_published_unsupported_passes[i].load(std::memory_order_relaxed);
	}
	counters.rejected_tfx_draws =
		s_gxm_published_rejected_tfx_draws.load(std::memory_order_acquire);
	for (size_t i = 0; i < s_gxm_published_host_calls.size(); i++)
	{
		const auto& published = s_gxm_published_host_calls[i];
		auto& local = counters.host_calls[i];
		local.calls = published.calls.load(std::memory_order_relaxed);
		local.wall_us = published.wall_us.load(std::memory_order_relaxed);
		local.max_lifetime_us = published.max_lifetime_us.load(std::memory_order_relaxed);
	}
	for (size_t i = 0; i < s_gxm_published_renderer_stages.size(); i++)
	{
		const auto& published = s_gxm_published_renderer_stages[i];
		auto& local = counters.renderer_stages[i];
		local.calls = published.calls.load(std::memory_order_relaxed);
		local.wall_us = published.wall_us.load(std::memory_order_relaxed);
		local.max_lifetime_us = published.max_lifetime_us.load(std::memory_order_relaxed);
	}
	counters.draw_calls = s_gxm_published_draw_calls.load(std::memory_order_relaxed);
	counters.renderer_draw_calls =
		s_gxm_published_renderer_draw_calls.load(std::memory_order_relaxed);
	counters.renderer_draw_wall_us =
		s_gxm_published_renderer_draw_wall_us.load(std::memory_order_relaxed);
	counters.device_render_calls =
		s_gxm_published_device_render_calls.load(std::memory_order_relaxed);
	counters.device_render_wall_us =
		s_gxm_published_device_render_wall_us.load(std::memory_order_relaxed);
	counters.draw_indices = s_gxm_published_draw_indices.load(std::memory_order_relaxed);
	counters.gpu_vu_draw_calls =
		s_gxm_published_gpu_vu_draw_calls.load(std::memory_order_relaxed);
	counters.gpu_vu_draw_indices =
		s_gxm_published_gpu_vu_draw_indices.load(std::memory_order_relaxed);
	counters.gpu_vu_descriptor_objects =
		s_gxm_published_gpu_vu_descriptor_objects.load(std::memory_order_relaxed);
	counters.vertex_upload_bytes =
		s_gxm_published_vertex_upload_bytes.load(std::memory_order_relaxed);
	counters.index_upload_bytes =
		s_gxm_published_index_upload_bytes.load(std::memory_order_relaxed);
	counters.texture_uploads =
		s_gxm_published_texture_uploads.load(std::memory_order_relaxed);
	counters.texture_upload_bytes =
		s_gxm_published_texture_upload_bytes.load(std::memory_order_relaxed);
	counters.render_store_acquisitions =
		s_gxm_published_render_store_acquisitions.load(std::memory_order_relaxed);
	counters.render_store_new_residencies =
		s_gxm_published_render_store_new_residencies.load(std::memory_order_relaxed);
	counters.render_store_resident_switches =
		s_gxm_published_render_store_resident_switches.load(std::memory_order_relaxed);
	counters.render_store_physical_scenes =
		s_gxm_published_render_store_physical_scenes.load(std::memory_order_relaxed);
	counters.render_store_loads =
		s_gxm_published_render_store_loads.load(std::memory_order_relaxed);
	counters.render_store_materializations =
		s_gxm_published_render_store_materializations.load(std::memory_order_relaxed);
	counters.render_store_failures =
		s_gxm_published_render_store_failures.load(std::memory_order_relaxed);
	counters.texture_readbacks =
		s_gxm_published_texture_readbacks.load(std::memory_order_relaxed);
	counters.texture_readback_bytes =
		s_gxm_published_texture_readback_bytes.load(std::memory_order_relaxed);
	counters.tfx_draws = s_gxm_published_tfx_draws.load(std::memory_order_relaxed);
	counters.textured_tfx_draws =
		s_gxm_published_textured_tfx_draws.load(std::memory_order_relaxed);
	counters.render_target_source_draws =
		s_gxm_published_render_target_source_draws.load(std::memory_order_relaxed);
	counters.depth_source_draws =
		s_gxm_published_depth_source_draws.load(std::memory_order_relaxed);
	counters.rt_hazard_draws =
		s_gxm_published_rt_hazard_draws.load(std::memory_order_relaxed);
	counters.depth_hazard_draws =
		s_gxm_published_depth_hazard_draws.load(std::memory_order_relaxed);
	counters.feedback_rt_draws =
		s_gxm_published_feedback_rt_draws.load(std::memory_order_relaxed);
	counters.feedback_depth_draws =
		s_gxm_published_feedback_depth_draws.load(std::memory_order_relaxed);
	counters.software_blend_draws =
		s_gxm_published_software_blend_draws.load(std::memory_order_relaxed);
	counters.fixed_blend_draws =
		s_gxm_published_fixed_blend_draws.load(std::memory_order_relaxed);
	counters.alpha_test_draws =
		s_gxm_published_alpha_test_draws.load(std::memory_order_relaxed);
	counters.partial_color_mask_draws =
		s_gxm_published_partial_color_mask_draws.load(std::memory_order_relaxed);
	counters.feedback_snapshots =
		s_gxm_published_feedback_snapshots.load(std::memory_order_relaxed);
	counters.feedback_snapshot_bytes =
		s_gxm_published_feedback_snapshot_bytes.load(std::memory_order_relaxed);
	counters.merge_calls =
		s_gxm_published_merge_calls.load(std::memory_order_relaxed);
	counters.merge_rc1_draws =
		s_gxm_published_merge_rc1_draws.load(std::memory_order_relaxed);
	counters.merge_rc2_draws =
		s_gxm_published_merge_rc2_draws.load(std::memory_order_relaxed);
	counters.present_calls =
		s_gxm_published_present_calls.load(std::memory_order_relaxed);
	counters.interlace_calls =
		s_gxm_published_interlace_calls.load(std::memory_order_relaxed);
	counters.last_merge_pmode =
		s_gxm_published_last_merge_pmode.load(std::memory_order_relaxed);
	counters.last_merge_extbuf =
		s_gxm_published_last_merge_extbuf.load(std::memory_order_relaxed);
	counters.last_merge_background =
		s_gxm_published_last_merge_background.load(std::memory_order_relaxed);
	counters.last_merge_source_sizes[0] =
		s_gxm_published_last_merge_source_size_1.load(std::memory_order_relaxed);
	counters.last_merge_source_sizes[1] =
		s_gxm_published_last_merge_source_size_2.load(std::memory_order_relaxed);
	counters.last_merge_source_ids[0] =
		s_gxm_published_last_merge_source_id_1.load(std::memory_order_relaxed);
	counters.last_merge_source_ids[1] =
		s_gxm_published_last_merge_source_id_2.load(std::memory_order_relaxed);
	counters.last_merge_source_mask =
		s_gxm_published_last_merge_source_mask.load(std::memory_order_relaxed);
	counters.last_merge_source_states =
		s_gxm_published_last_merge_source_states.load(std::memory_order_relaxed);
	counters.last_merge_writer_tfx_writes =
		s_gxm_published_last_merge_writer_tfx_writes.load(
			std::memory_order_relaxed);
	counters.last_merge_writer_ps_lo =
		s_gxm_published_last_merge_writer_ps_lo.load(std::memory_order_relaxed);
	counters.last_merge_writer_ps_hi =
		s_gxm_published_last_merge_writer_ps_hi.load(std::memory_order_relaxed);
	counters.last_merge_writer_draw_area =
		s_gxm_published_last_merge_writer_draw_area.load(
			std::memory_order_relaxed);
	counters.last_merge_writer_sample_area =
		s_gxm_published_last_merge_writer_sample_area.load(
			std::memory_order_relaxed);
	counters.last_merge_writer_source_id =
		s_gxm_published_last_merge_writer_source_id.load(
			std::memory_order_relaxed);
	counters.last_merge_writer_source_size =
		s_gxm_published_last_merge_writer_source_size.load(
			std::memory_order_relaxed);
	counters.last_merge_writer_blend =
		s_gxm_published_last_merge_writer_blend.load(std::memory_order_relaxed);
	counters.last_merge_writer_selector_keys =
		s_gxm_published_last_merge_writer_selector_keys.load(
			std::memory_order_relaxed);
	counters.last_merge_trace_circuit =
		s_gxm_published_last_merge_trace_circuit.load(std::memory_order_relaxed);
	counters.last_merge_writer_kind =
		s_gxm_published_last_merge_writer_kind.load(std::memory_order_relaxed);
	counters.last_merge_writer_topology =
		s_gxm_published_last_merge_writer_topology.load(
			std::memory_order_relaxed);
	counters.last_merge_parent_tfx_writes =
		s_gxm_published_last_merge_parent_tfx_writes.load(
			std::memory_order_relaxed);
	counters.last_merge_parent_textured_tfx_writes =
		s_gxm_published_last_merge_parent_textured_tfx_writes.load(
			std::memory_order_relaxed);
	counters.last_merge_parent_untextured_tfx_writes =
		s_gxm_published_last_merge_parent_untextured_tfx_writes.load(
			std::memory_order_relaxed);
	counters.last_merge_parent_render_target_source_tfx_writes =
		s_gxm_published_last_merge_parent_rt_source_tfx_writes.load(
			std::memory_order_relaxed);
	counters.last_merge_parent_full_mask_tfx_writes =
		s_gxm_published_last_merge_parent_full_mask_tfx_writes.load(
			std::memory_order_relaxed);
	counters.last_merge_parent_rgb_only_tfx_writes =
		s_gxm_published_last_merge_parent_rgb_only_tfx_writes.load(
			std::memory_order_relaxed);
	counters.last_merge_parent_alpha_only_tfx_writes =
		s_gxm_published_last_merge_parent_alpha_only_tfx_writes.load(
			std::memory_order_relaxed);
	counters.last_merge_parent_other_mask_tfx_writes =
		s_gxm_published_last_merge_parent_other_mask_tfx_writes.load(
			std::memory_order_relaxed);
	counters.last_merge_parent_ps_lo =
		s_gxm_published_last_merge_parent_ps_lo.load(std::memory_order_relaxed);
	counters.last_merge_parent_ps_hi =
		s_gxm_published_last_merge_parent_ps_hi.load(std::memory_order_relaxed);
	counters.last_merge_parent_draw_area =
		s_gxm_published_last_merge_parent_draw_area.load(
			std::memory_order_relaxed);
	counters.last_merge_parent_sample_area =
		s_gxm_published_last_merge_parent_sample_area.load(
			std::memory_order_relaxed);
	counters.last_merge_parent_source_id =
		s_gxm_published_last_merge_parent_source_id.load(
			std::memory_order_relaxed);
	counters.last_merge_parent_source_size =
		s_gxm_published_last_merge_parent_source_size.load(
			std::memory_order_relaxed);
	counters.last_merge_parent_blend =
		s_gxm_published_last_merge_parent_blend.load(std::memory_order_relaxed);
	counters.last_merge_parent_selector_keys =
		s_gxm_published_last_merge_parent_selector_keys.load(
			std::memory_order_relaxed);
	counters.last_merge_parent_last_rgb_ps_lo =
		s_gxm_published_last_merge_parent_last_rgb_ps_lo.load(
			std::memory_order_relaxed);
	counters.last_merge_parent_last_rgb_ps_hi =
		s_gxm_published_last_merge_parent_last_rgb_ps_hi.load(
			std::memory_order_relaxed);
	counters.last_merge_parent_last_rgb_draw_area =
		s_gxm_published_last_merge_parent_last_rgb_draw_area.load(
			std::memory_order_relaxed);
	counters.last_merge_parent_last_rgb_sample_area =
		s_gxm_published_last_merge_parent_last_rgb_sample_area.load(
			std::memory_order_relaxed);
	counters.last_merge_parent_last_rgb_source_id =
		s_gxm_published_last_merge_parent_last_rgb_source_id.load(
			std::memory_order_relaxed);
	counters.last_merge_parent_last_rgb_source_size =
		s_gxm_published_last_merge_parent_last_rgb_source_size.load(
			std::memory_order_relaxed);
	counters.last_merge_parent_last_rgb_blend =
		s_gxm_published_last_merge_parent_last_rgb_blend.load(
			std::memory_order_relaxed);
	counters.last_merge_parent_last_rgb_selector_keys =
		s_gxm_published_last_merge_parent_last_rgb_selector_keys.load(
			std::memory_order_relaxed);
	counters.last_merge_parent_kind =
		s_gxm_published_last_merge_parent_kind.load(std::memory_order_relaxed);
	counters.last_merge_parent_topology =
		s_gxm_published_last_merge_parent_topology.load(
			std::memory_order_relaxed);
	counters.last_merge_parent_color_mask =
		s_gxm_published_last_merge_parent_color_mask.load(
			std::memory_order_relaxed);
	counters.last_merge_parent_last_rgb_topology =
		s_gxm_published_last_merge_parent_last_rgb_topology.load(
			std::memory_order_relaxed);
	counters.last_merge_parent_last_rgb_color_mask =
		s_gxm_published_last_merge_parent_last_rgb_color_mask.load(
			std::memory_order_relaxed);
	counters.psm24_draws =
		s_gxm_published_psm24_draws.load(std::memory_order_relaxed);
	counters.device_rejects =
		s_gxm_published_device_rejects.load(std::memory_order_acquire);
	counters.last_device_reject_hash =
		s_gxm_published_last_device_reject_hash.load(std::memory_order_relaxed);
	counters.last_feedback_ps_lo =
		s_gxm_published_last_feedback_ps_lo.load(std::memory_order_relaxed);
	counters.last_feedback_ps_hi =
		s_gxm_published_last_feedback_ps_hi.load(std::memory_order_relaxed);
	counters.last_feedback_blend =
		s_gxm_published_last_feedback_blend.load(std::memory_order_relaxed);
	counters.last_feedback_vs =
		s_gxm_published_last_feedback_vs.load(std::memory_order_relaxed);
	counters.last_feedback_sampler =
		s_gxm_published_last_feedback_sampler.load(std::memory_order_relaxed);
	counters.last_feedback_depth =
		s_gxm_published_last_feedback_depth.load(std::memory_order_relaxed);
	counters.last_feedback_colormask =
		s_gxm_published_last_feedback_colormask.load(std::memory_order_relaxed);
	counters.last_feedback_topology =
		s_gxm_published_last_feedback_topology.load(std::memory_order_relaxed);
	counters.last_feedback_hazard =
		s_gxm_published_last_feedback_hazard.load(std::memory_order_relaxed);
	counters.last_rejected_tfx_features =
		s_gxm_published_last_rejected_tfx_features.load(std::memory_order_relaxed);
	counters.last_rejected_tfx_ps_lo =
		s_gxm_published_last_rejected_tfx_ps_lo.load(std::memory_order_relaxed);
	counters.last_rejected_tfx_ps_hi =
		s_gxm_published_last_rejected_tfx_ps_hi.load(std::memory_order_relaxed);
	counters.last_rejected_tfx_blend =
		s_gxm_published_last_rejected_tfx_blend.load(std::memory_order_relaxed);
	counters.last_rejected_tfx_vs =
		s_gxm_published_last_rejected_tfx_vs.load(std::memory_order_relaxed);
	counters.last_rejected_tfx_sampler =
		s_gxm_published_last_rejected_tfx_sampler.load(std::memory_order_relaxed);
	counters.last_rejected_tfx_depth =
		s_gxm_published_last_rejected_tfx_depth.load(std::memory_order_relaxed);
	counters.last_rejected_tfx_colormask =
		s_gxm_published_last_rejected_tfx_colormask.load(std::memory_order_relaxed);
	counters.last_rejected_tfx_topology =
		s_gxm_published_last_rejected_tfx_topology.load(std::memory_order_relaxed);
	return counters;
}

static void RecordGxmDraw(u64 vertices, size_t vertex_stride, u64 indices)
{
	if (!VitaPerformanceTelemetry::IsEnabled())
		return;
	s_gxm_worker_performance.draw_calls++;
	s_gxm_worker_performance.draw_indices += indices;
	s_gxm_worker_performance.vertex_upload_bytes += vertices * vertex_stride;
	s_gxm_worker_performance.index_upload_bytes += indices * sizeof(u16);
}

static void RecordGpuVuGxmDraw(u64 indices, u64 descriptor_objects = 1u)
{
	if (!VitaPerformanceTelemetry::IsEnabled())
		return;
	s_gxm_worker_performance.draw_calls++;
	s_gxm_worker_performance.draw_indices += indices;
	s_gxm_worker_performance.gpu_vu_draw_calls++;
	s_gxm_worker_performance.gpu_vu_draw_indices += indices;
	s_gxm_worker_performance.gpu_vu_descriptor_objects += descriptor_objects;
}

static bool GpuVuNotificationReached(
	const SceGxmNotification& notification)
{
	if (!notification.address)
		return false;
	// PhyreRenderInterfaceGXM::isVertexCompleted() uses this signed modular
	// comparison so a later completed value also retires an older resource.
	return VitaGpuVu::HasCompletedNotificationValue(
		*notification.address, notification.value);
}

using GpuVuInputRetentions =
	std::array<VitaGpuVu::RawVifPayloadRef, VitaGpuVu::InputRingSlotCount>;

static bool SameGpuVuInputSlot(const VitaGpuVu::RawVifPayloadRef& left,
	const VitaGpuVu::RawVifPayloadRef& right)
{
	return left.owner == right.owner && left.slot == right.slot &&
		left.generation == right.generation;
}

static bool ContainsGpuVuInputSlot(const GpuVuInputRetentions& retentions,
	u32 retention_count,
	const VitaGpuVu::RawVifPayloadRef& payload)
{
	for (u32 index = 0; index < retention_count; index++)
	{
		if (SameGpuVuInputSlot(retentions[index], payload))
			return true;
	}
	return false;
}

static bool CanRetainGpuVuInputSlots(
	const GpuVuInputRetentions& retained, u32 retained_count,
	const std::vector<std::unique_ptr<VitaGpuVu::GpuVuDraw>>& draws)
{
	if (retained_count > retained.size())
		return false;
	GpuVuInputRetentions combined = retained;
	u32 combined_count = retained_count;
	for (const auto& draw : draws)
	{
		if (!draw)
			return false;
		for (const VitaGpuVu::RawVifPayloadRef& payload :
			draw->InputPayloads())
		{
			if (!payload.IsValid())
				return false;
			if (ContainsGpuVuInputSlot(
					combined, combined_count, payload))
			{
				continue;
			}
			if (combined_count >= combined.size())
				return false;
			combined[combined_count++] = payload;
		}
	}
	return true;
}

static bool AdoptGpuVuInputSlot(VitaGpuVu::GpuVuDraw* draw,
	size_t input_index, GpuVuInputRetentions* retentions,
	u32* retention_count)
{
	if (!draw || !retentions || !retention_count ||
		input_index >= draw->InputPayloads().size())
		return false;
	const VitaGpuVu::RawVifPayloadRef& payload =
		draw->InputPayloads()[input_index];
	if (!payload.IsValid())
		return false;
	for (u32 index = 0; index < *retention_count; index++)
	{
		if (SameGpuVuInputSlot((*retentions)[index], payload))
			return true;
	}
	if (*retention_count >= retentions->size())
		return false;
	// The descriptor already owns this exact generation. Transfer that
	// reference after encoding instead of reacquiring it from the ring: Sony's
	// GXM lifetime contract permits release only after vertex completion, and a
	// failed post-draw RetainRawVifPayload() is too late for CPU fallback.
	if (!draw->TransferInputPayloadOwnership(
			input_index, &(*retentions)[*retention_count]))
	{
		return false;
	}
	(*retention_count)++;
	return true;
}

static void ReleaseGpuVuInputSlots(GpuVuInputRetentions* retentions,
	u32* retention_count, u32 keep_count = 0)
{
	if (!retentions || !retention_count)
		return;
	while (*retention_count > keep_count)
	{
		(*retention_count)--;
		VitaGpuVu::ReleaseRawVifPayload(
			&(*retentions)[*retention_count]);
	}
}

static void MoveGpuVuInputSlots(GpuVuInputRetentions* destination,
	u32* destination_count, GpuVuInputRetentions* source,
	u32* source_count)
{
	pxAssert(destination && destination_count && source && source_count);
	pxAssert(*destination_count == 0);
	for (u32 index = 0; index < *source_count; index++)
	{
		(*destination)[index] = (*source)[index];
		(*source)[index] = {};
	}
	*destination_count = *source_count;
	*source_count = 0;
}

	struct GSDeviceGXM::Impl final : public VitaGXM::TextureOwner
	{
		struct GeneratedVuProgram
	{
		struct Uniforms
		{
			const SceGxmProgramParameter* vertex_scale_offset = nullptr;
			const SceGxmProgramParameter* max_depth = nullptr;
			std::vector<const SceGxmProgramParameter*> constants;
			std::array<const SceGxmProgramParameter*, 32> vf{};
			const SceGxmProgramParameter* acc = nullptr;
			const SceGxmProgramParameter* q = nullptr;
			const SceGxmProgramParameter* p = nullptr;
			const SceGxmProgramParameter* i = nullptr;
			const SceGxmProgramParameter* gif_q = nullptr;
		};

		VitaGpuVu::ShaderKey key;
		VitaGpuVu::GeneratedCgProgram metadata;
		std::vector<u8> gxp;
		VitaGpuVu::GeneratedGxpResourceUsage gxp_resources;
		VitaGpuVu::GeneratedGxpResourceAttestation resource_attestation =
			VitaGpuVu::GeneratedGxpResourceAttestation::MissingInput;
		SceGxmShaderPatcherId id = nullptr;
		SceGxmVertexProgram* vertex_program = nullptr;
		SceGxmFragmentProgram* general_fragment_program = nullptr;
		SceGxmFragmentProgram* zfloor_fragment_program = nullptr;
		SceGxmFragmentProgram* opaque_fragment_program = nullptr;
		u32 patcher_buffer_bytes = 0;
		u32 patcher_vertex_usse_bytes = 0;
		u32 patcher_fragment_usse_bytes = 0;
		bool registration_complete = false;
		Uniforms uniforms;
		};

		struct GpuVuPrivateStoreOutput
		{
			GpuVuPrivateStoreOutput() = default;
			GpuVuPrivateStoreOutput(const GpuVuPrivateStoreOutput&) = delete;
			GpuVuPrivateStoreOutput& operator=(
				const GpuVuPrivateStoreOutput&) = delete;
			GpuVuPrivateStoreOutput(GpuVuPrivateStoreOutput&& other) noexcept
			{
				MoveFrom(std::move(other));
			}
			GpuVuPrivateStoreOutput& operator=(
				GpuVuPrivateStoreOutput&& other) noexcept
			{
				if (this != &other)
				{
					ReleasePendingOwnership();
					MoveFrom(std::move(other));
				}
				return *this;
			}
			~GpuVuPrivateStoreOutput()
			{
				ReleasePendingOwnership();
			}

			VitaGXM::ArenaAllocation allocation;
			u8* payload_data = nullptr;
			u8* ftoi_probe_data = nullptr;
			u8* guard_data = nullptr;
			std::vector<VitaGpuVu::PrivateStoreExpectation> expectations;
			std::vector<VitaGpuVu::FtoiProbeExpectation> ftoi_expectations;
			std::shared_ptr<VitaGpuVu::GeneratedLoopKernelTransaction>
				transaction;
			VitaGpuVu::GeneratedLoopKernelAttestationIdentity
				attestation_identity;
			u64 sequence = 0;
			u32 payload_bytes = 0;
			u32 entry_count = 0;
			u32 ftoi_probe_bytes = 0;
			u32 ftoi_probe_count = 0;
			u32 ftoi_probe_configuration_bits = 0;
			VitaGpuVu::GeneratedLoopKernelPrivateOutputWriteExtent
				write_extent{};
			u16 ftoi_probe_outer_count = 0;
			u16 ftoi_probe_child_count = 0;
			u8 stores_per_invocation = 0;
			u8 ftoi_probe_store_index = 0;
			u8 ftoi_probe_lane = 0;
			u8 ftoi_probe_scale_offset = 0;
			bool compare_with_cpu_oracle = false;
			bool completion_only = false;
			bool architectural_state_compared = false;
			bool architectural_state_exact = false;
			bool architectural_state_playable_profile_matches = false;
			u32 architectural_state_mismatch_lanes = 0u;
			u32 architectural_state_playable_mismatch_lanes = 0u;

			u32 TotalPayloadBytes() const
			{
				return payload_bytes + ftoi_probe_bytes;
			}

			u32 FtoiProbeOffset() const
			{
				return payload_bytes;
			}

			u8* PayloadData() const
			{
				return payload_data ? payload_data :
					static_cast<u8*>(allocation.Data());
			}

			u8* FtoiProbeData() const
			{
				if (!HasFtoiProbe())
					return nullptr;
				return ftoi_probe_data ? ftoi_probe_data :
					PayloadData() + FtoiProbeOffset();
			}

			u8* GuardData() const
			{
				return guard_data ? guard_data :
					(PayloadData() ? PayloadData() + TotalPayloadBytes() : nullptr);
			}

			bool HasOutputStorage() const
			{
				return PayloadData() != nullptr && GuardData() != nullptr;
			}

			bool HasFtoiProbe() const
			{
				return ftoi_probe_bytes != 0u && ftoi_probe_count != 0u;
			}

		private:
			void ReleasePendingOwnership()
			{
				if (transaction)
				{
					// A stack-local output record may be discarded while RenderHW is
					// still rejecting the descriptor before sceGxmDraw().  The outer
					// descriptor owner records that exact rejection and retains the
					// replay journal.  Once GXM has accepted the draw, however, losing
					// this record loses the only completion/output publication owner.
					if (transaction->Stage() == VitaGpuVu::
						GeneratedLoopKernelTransactionStage::GsAccepted)
					{
						transaction->MarkFailed(
							VitaGpuVu::GeneratedLoopKernelTransactionFailure::
								GsOutputRecordReleasedBeforeCompletion);
					}
					transaction.reset();
				}
				if (compare_with_cpu_oracle && attestation_identity.IsValid())
				{
					// Once GS has accepted cancellation ownership, losing the
					// private output record is a terminal failure for this key in
					// the current process. Reopening it as Unattested caused every
					// following BSpline epoch to launch another CPU+GPU canary when
					// an ordinary render-target allocation failed. CPU MTVU remains
					// the pre-effect owner; a later process may attest the cached GXP
					// again after the device resources have been rebuilt.
					VitaGpuVu::CompleteGeneratedLoopKernelAttestation(
						attestation_identity,
						VitaGpuVu::GeneratedLoopKernelNumericProfile::None,
						VitaGpuVu::GeneratedLoopKernelAttestationRejection::
							RetirementCancelled);
				}
				compare_with_cpu_oracle = false;
				attestation_identity = {};
			}

			void MoveFrom(GpuVuPrivateStoreOutput&& other) noexcept
			{
				allocation = std::move(other.allocation);
				payload_data = other.payload_data;
				ftoi_probe_data = other.ftoi_probe_data;
				guard_data = other.guard_data;
				expectations = std::move(other.expectations);
				ftoi_expectations = std::move(other.ftoi_expectations);
				transaction = std::move(other.transaction);
				attestation_identity = other.attestation_identity;
				sequence = other.sequence;
				payload_bytes = other.payload_bytes;
				entry_count = other.entry_count;
				ftoi_probe_bytes = other.ftoi_probe_bytes;
				ftoi_probe_count = other.ftoi_probe_count;
				ftoi_probe_configuration_bits =
					other.ftoi_probe_configuration_bits;
				write_extent = other.write_extent;
				ftoi_probe_outer_count = other.ftoi_probe_outer_count;
				ftoi_probe_child_count = other.ftoi_probe_child_count;
				stores_per_invocation = other.stores_per_invocation;
				ftoi_probe_store_index = other.ftoi_probe_store_index;
				ftoi_probe_lane = other.ftoi_probe_lane;
				ftoi_probe_scale_offset = other.ftoi_probe_scale_offset;
				compare_with_cpu_oracle = other.compare_with_cpu_oracle;
				completion_only = other.completion_only;
				architectural_state_compared =
					other.architectural_state_compared;
				architectural_state_exact = other.architectural_state_exact;
				architectural_state_playable_profile_matches =
					other.architectural_state_playable_profile_matches;
				architectural_state_mismatch_lanes =
					other.architectural_state_mismatch_lanes;
				architectural_state_playable_mismatch_lanes =
					other.architectural_state_playable_mismatch_lanes;

				// The identity and comparison bit are cancellation ownership. They
				// are trivially copyable, unlike the vectors and shared pointer, so
				// a defaulted move leaves the source armed. Vector relocation then
				// destroys that source and reopens PrivateTest while its GPU draw is
				// still in flight. Transfer that ownership exactly once.
				other.attestation_identity = {};
				other.compare_with_cpu_oracle = false;
				other.completion_only = false;
				other.payload_data = nullptr;
				other.ftoi_probe_data = nullptr;
				other.guard_data = nullptr;
			}
		};

	struct GpuVuRetirementSlot
	{
		SceGxmNotification notification{};
		SceGxmNotification fragment_notification{};
		GpuVuInputRetentions input_retentions{};
		u32 input_retention_count = 0;
		u64 draw_count = 0;
		Common::Timer::Value submission_wall = 0;
		u64 submission_scene_serial = 0;
		u64 retained_sequence_begin = 0;
		u64 retained_sequence_end = 0;
		u32 retained_sequence_flags = 0;
		std::vector<VitaGXM::ArenaAllocation> batch_allocations;
		std::vector<GpuVuPrivateStoreOutput> private_store_outputs;
		bool submitted = false;
	};

	struct GpuVuGeneratedSceneManifest
	{
		u64 scene_serial = 0;
		u64 sequence_begin = 0;
		u64 sequence_end = 0;
		u64 program_key_high = 0;
		u64 program_key_low = 0;
		u64 ps_selector_low = 0;
		u64 ps_selector_high = 0;
		uptr input_owner = 0;
		uptr output_first = 0;
		uptr output_last = 0;
		uptr vertex_program = 0;
		uptr fragment_program = 0;
		uptr source_texture_object = 0;
		uptr source_texture_descriptor = 0;
		uptr source_texture_data = 0;
		u32 input_slot = ~u32{0};
		u32 input_generation = 0;
		u32 input_first_qword = 0;
		u32 input_last_qword = 0;
		u32 program_abi = 0;
		u32 draw_call_count = 0;
		u32 index_count = 0;
		u32 index_minimum = 0;
		u32 index_maximum = 0;
		u32 object_count = 0;
		u32 private_transaction_count = 0;
		u32 output_allocation_bytes = 0;
		u32 output_maximum_write_word = 0;
		u32 output_payload_capacity_words = 0;
		u32 output_probe_maximum_write_word = 0;
		u32 output_probe_capacity_words = 0;
		u32 primitive_type = 0;
		u32 topology = 0;
		u32 sampler_key = 0;
		u32 blend_key = 0;
		u32 color_mask_key = 0;
		u32 depth_key = 0;
		u32 texture_type = 0;
		u32 texture_format = 0;
		u32 texture_width = 0;
		u32 texture_height = 0;
		u32 texture_stride = 0;
		u32 texture_mipmap_count = 0;
		u32 texture_sampler_state = 0;
		u32 flags = 0;
	};

	struct GpuVuFragmentProgressRecord
	{
		u32 value = 0;
		u64 scene_serial = 0;
		u64 last_generated_scene = 0;
		u64 last_generated_sequence = 0;
		GpuVuGeneratedSceneManifest generated_manifest{};
	};

#if defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
	struct UniversalGpuVuProductSlot
	{
		VitaGXM::MappedBlock buffer;
		SceGxmNotification notification{};
		std::array<SceGxmNotification,
			VitaGpuVu::UniversalGpuVuMaximumSubmissionsPerBatch>
			stage_notifications{};
		u64 sequence = 0;
		Common::Timer::Value submission_wall = 0;
		u32 submission_groups = 0;
		u32 continuation_groups = 0;
		u32 core_submissions = 0;
		u32 job_submissions = 0;
		u32 path1_commit_submissions = 0;
		u32 submission_pair_limit = 0;
		u32 source_bank = 0;
		u32 completed_bank = 0;
		u32 structured_submission_cursor = 0;
		u32 structured_submission_total = 0;
		u32 structured_workspace_generation = 0;
		bool generated_serial = false;
		bool generated_hot = false;
		bool generated_structured = false;
		bool compact_serial = false;
		bool submitted = false;
		bool committed = false;
		bool dedicated_scene = false;
	};

	struct UniversalGpuVuObservedPairBound
	{
		u64 program_identity = 0;
		u64 last_use = 0;
		u32 execute_count = 0;
		u32 unpack_submission_count = 0;
		u32 observed_pairs = 0;
		u32 minimum_fixed_firmware_jobs = 0;
	};
#endif

	struct RenderTargetEntry
	{
		u32 width = 0;
		u32 height = 0;
		u32 scenes_this_frame = 0;
		bool growth_disabled = false;
		std::vector<SceGxmRenderTarget*> targets;
	};

	struct ProgramUniforms
	{
		const SceGxmProgramParameter* vertex_scale_offset = nullptr;
		const SceGxmProgramParameter* selector[7]{};
		const SceGxmProgramParameter* selector7 = nullptr;
		const SceGxmProgramParameter* dither_matrix[4]{};
		const SceGxmProgramParameter* fog_color_aref = nullptr;
		const SceGxmProgramParameter* texture_size = nullptr;
		const SceGxmProgramParameter* native_texture_size = nullptr;
		const SceGxmProgramParameter* texture_alpha = nullptr;
		const SceGxmProgramParameter* half_texel = nullptr;
		const SceGxmProgramParameter* st_scale = nullptr;
		const SceGxmProgramParameter* st_range = nullptr;
		const SceGxmProgramParameter* fb_mask = nullptr;
		const SceGxmProgramParameter* hardware_blend[2]{};
		const SceGxmProgramParameter* color_mask = nullptr;
		const SceGxmProgramParameter* region_mask_fix = nullptr;
		const SceGxmProgramParameter* lod_params = nullptr;
		const SceGxmProgramParameter* interlace[2]{};
	};

	bool ready = false;
	bool gxm_initialized = false;
	bool scene_active = false;
	bool scene_is_display = false;
	u32 scene_ordinary_content = 0;
	u32 clear_commit_depth = 0;
#if defined(VITASX2_GXM_FUSE_ATTACHMENT_CLEARS)
	bool fuse_attachment_clears = true;
#else
	bool fuse_attachment_clears = false;
#endif
	bool present_active = false;
	SceGxmContext* context = nullptr;
	void* context_host = nullptr;
	SceGxmShaderPatcher* patcher = nullptr;
	SceGxmDepthStencilSurface disabled_depth{};

	VitaGXM::MappedBlock vdm_ring;
	VitaGXM::MappedBlock vertex_ring;
	VitaGXM::MappedBlock fragment_ring;
	VitaGXM::MappedBlock fragment_usse_ring;
	VitaGXM::MappedBlock patcher_buffer;
	VitaGXM::MappedBlock patcher_vertex_usse;
	VitaGXM::MappedBlock patcher_fragment_usse;
	VitaGXM::MappedBlock geometry_vertices;
	VitaGXM::MappedBlock geometry_indices;
	VitaGXM::Arena texture_arena;
	VitaGXM::Arena transfer_arena;
	VitaGXM::ArenaAllocation render_store_color;
	VitaGXM::ArenaAllocation render_store_depth;
	VitaGXM::ArenaAllocation render_store_stencil;
	SceGxmColorSurface render_store_color_surface{};
	SceGxmDepthStencilSurface render_store_depth_surface{};
	VitaGXM::RenderStoreResidencyPlanner render_store_planner;
	bool render_store_ready = false;
	VitaGXM::Display display;
	VitaGpuVu::ShaderCompiler gpu_vu_shader_compiler;
	VitaGpuVu::InputRing gpu_vu_input_ring;
#if defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
	VitaGXM::MappedBlock gpu_vu_universal_validation_buffer;
	SceGxmShaderPatcherId gpu_vu_universal_vertex_id = nullptr;
	SceGxmShaderPatcherId gpu_vu_universal_fragment_id = nullptr;
	SceGxmVertexProgram* gpu_vu_universal_vertex_program = nullptr;
	SceGxmFragmentProgram* gpu_vu_universal_fragment_program = nullptr;
	u32 gpu_vu_universal_expected_cycle = 0;
	u32 gpu_vu_universal_configuration_bits = 0;
	UniversalGpuVuValidationStage gpu_vu_universal_validation_stage =
		UniversalGpuVuValidationStage::Uninitialized;
	VitaGXM::MappedBlock gpu_vu_universal_product_shared;
	// The runtime-generated direct provider only needs the 640-word ARM Q/P
	// estimate table.  Keep it separate from the retired fixed-machine image so
	// direct GXPs do not reserve its offline programs, payload arena, private
	// state generations, or dedicated render target.
	VitaGXM::MappedBlock gpu_vu_generated_estimate_table;
	SceGxmShaderPatcherId gpu_vu_universal_product_vertex_id = nullptr;
	SceGxmShaderPatcherId gpu_vu_universal_compact_product_vertex_id = nullptr;
	SceGxmShaderPatcherId gpu_vu_vif_unpack_product_vertex_id = nullptr;
	SceGxmShaderPatcherId gpu_vu_vif_unpack_independent_product_vertex_id = nullptr;
	SceGxmShaderPatcherId gpu_vu_structured_control_product_vertex_id = nullptr;
	SceGxmShaderPatcherId gpu_vu_structured_state_product_vertex_id = nullptr;
	SceGxmShaderPatcherId gpu_vu_structured_preflight_build_product_vertex_id = nullptr;
	SceGxmShaderPatcherId gpu_vu_structured_preflight_reduce_product_vertex_id = nullptr;
	SceGxmShaderPatcherId gpu_vu_structured_preflight_finalize_product_vertex_id = nullptr;
	SceGxmShaderPatcherId gpu_vu_structured_qp_numeric_product_vertex_id = nullptr;
	SceGxmShaderPatcherId gpu_vu_structured_fmac_numeric_product_vertex_id = nullptr;
	SceGxmShaderPatcherId gpu_vu_structured_fmac_native_product_vertex_id = nullptr;
	SceGxmShaderPatcherId gpu_vu_path1_commit_product_vertex_id = nullptr;
	SceGxmShaderPatcherId gpu_vu_universal_product_fragment_id = nullptr;
	SceGxmVertexProgram* gpu_vu_universal_product_vertex_program = nullptr;
	SceGxmVertexProgram* gpu_vu_universal_compact_product_vertex_program = nullptr;
	SceGxmVertexProgram* gpu_vu_vif_unpack_product_vertex_program = nullptr;
	SceGxmVertexProgram* gpu_vu_vif_unpack_independent_product_vertex_program = nullptr;
	SceGxmVertexProgram* gpu_vu_structured_control_product_vertex_program = nullptr;
	SceGxmVertexProgram* gpu_vu_structured_state_product_vertex_program = nullptr;
	SceGxmVertexProgram* gpu_vu_structured_preflight_build_product_vertex_program = nullptr;
	SceGxmVertexProgram* gpu_vu_structured_preflight_reduce_product_vertex_program = nullptr;
	SceGxmVertexProgram* gpu_vu_structured_preflight_finalize_product_vertex_program = nullptr;
	SceGxmVertexProgram* gpu_vu_structured_qp_numeric_product_vertex_program = nullptr;
	SceGxmVertexProgram* gpu_vu_structured_fmac_numeric_product_vertex_program = nullptr;
	SceGxmVertexProgram* gpu_vu_structured_fmac_native_product_vertex_program = nullptr;
	SceGxmVertexProgram* gpu_vu_path1_commit_product_vertex_program = nullptr;
	SceGxmFragmentProgram* gpu_vu_universal_product_fragment_program = nullptr;
	SceGxmFragmentProgram* gpu_vu_universal_compact_product_fragment_program = nullptr;
	SceGxmFragmentProgram* gpu_vu_vif_unpack_product_fragment_program = nullptr;
	SceGxmFragmentProgram* gpu_vu_vif_unpack_independent_product_fragment_program = nullptr;
	SceGxmFragmentProgram* gpu_vu_structured_control_product_fragment_program = nullptr;
	SceGxmFragmentProgram* gpu_vu_structured_state_product_fragment_program = nullptr;
	SceGxmFragmentProgram* gpu_vu_structured_preflight_build_product_fragment_program = nullptr;
	SceGxmFragmentProgram* gpu_vu_structured_preflight_reduce_product_fragment_program = nullptr;
	SceGxmFragmentProgram* gpu_vu_structured_preflight_finalize_product_fragment_program = nullptr;
	SceGxmFragmentProgram* gpu_vu_structured_qp_numeric_product_fragment_program = nullptr;
	SceGxmFragmentProgram* gpu_vu_structured_fmac_numeric_product_fragment_program = nullptr;
	SceGxmFragmentProgram* gpu_vu_structured_fmac_native_product_fragment_program = nullptr;
	SceGxmFragmentProgram* gpu_vu_path1_commit_product_fragment_program = nullptr;
	std::array<UniversalGpuVuProductSlot,
		GPU_VU_UNIVERSAL_PRODUCT_SLOT_COUNT> gpu_vu_universal_product_slots;
	SceGxmRenderTarget* gpu_vu_universal_product_target = nullptr;
	std::unique_ptr<VitaGXM::GSTextureGXM>
		gpu_vu_universal_product_color_surface;
	s32 gpu_vu_universal_product_committed_slot = -1;
	u32 gpu_vu_universal_product_next_slot = 0;
	u64 gpu_vu_universal_product_accept_count = 0;
	u64 gpu_vu_universal_product_runtime_reject_count = 0;
	u64 gpu_vu_universal_product_dispatch_cost_reject_count = 0;
	u64 gpu_vu_universal_product_continuation_count = 0;
	u64 gpu_vu_universal_product_structured_submit_count = 0;
	u64 gpu_vu_universal_product_structured_owner_reject_count = 0;
	u64 gpu_vu_universal_product_structured_quarantine_reject_count = 0;
	u64 gpu_vu_universal_product_structured_bank_diagnostic_count = 0;
	bool gpu_vu_universal_generated_accept_reported = false;
	std::array<UniversalGpuVuObservedPairBound, 16>
		gpu_vu_universal_observed_pair_bounds{};
	u64 gpu_vu_universal_observed_pair_bound_clock = 0;
	bool gpu_vu_universal_product_ready = false;
	bool gpu_vu_canonical_vu_memory_mapped = false;
	bool gpu_vu_structured_fmac_exact_attested = false;
	bool gpu_vu_structured_scratch_range_attested = false;
	StructuredNativeFmacAttestation gpu_vu_structured_fmac_native_attestation =
		StructuredNativeFmacAttestation::Unavailable;
#endif
	std::array<GpuVuRetirementSlot, GPU_VU_RETIREMENT_SLOT_COUNT>
		gpu_vu_retirement_slots;
	GpuVuInputRetentions gpu_vu_scene_input_retentions{};
	u32 gpu_vu_scene_input_retention_count = 0;
	u64 gpu_vu_scene_draw_count = 0;
		std::vector<VitaGXM::ArenaAllocation>
			gpu_vu_scene_batch_allocations;
		std::vector<GpuVuPrivateStoreOutput>
			gpu_vu_scene_private_store_outputs;
	u64 gpu_vu_generated_pipeline_accept_count = 0;
	u64 gpu_vu_generated_pipeline_pair_count = 0;
	u64 gpu_vu_loop_kernel_accept_count = 0;
	u64 gpu_vu_loop_kernel_pair_count = 0;
	u64 gpu_vu_batch_live_in_variance_count = 0;
	u64 gpu_vu_scene_end_transaction_submit_count = 0;
	u32 next_gpu_vu_retirement_slot = 0;
	u32 gpu_vu_retirement_health_pass = 0;
	u32 gpu_vu_pre_notification_watchdog_ticket = 0;
	u64 gpu_vu_pre_notification_watchdog_sequence = 0;
	u64 gpu_vu_last_generated_sequence = 0;
	u64 gpu_vu_last_generated_scene_serial = 0;
	GpuVuGeneratedSceneManifest gpu_vu_generated_scene_manifest{};
	SceGxmNotification gpu_vu_fragment_progress_notification{};
	std::array<GpuVuFragmentProgressRecord,
		GPU_VU_FRAGMENT_PROGRESS_HISTORY_COUNT>
		gpu_vu_fragment_progress_history{};
	u32 gpu_vu_fragment_progress_last_submitted = 0;
	u32 gpu_vu_fragment_progress_last_completed = 0;
	u64 gpu_vu_fragment_progress_completed_scene = 0;
	u64 gpu_vu_fragment_progress_completed_generated_scene = 0;
	u64 gpu_vu_fragment_progress_completed_generated_sequence = 0;
	u64 gpu_vu_fragment_progress_history_overruns = 0;
	bool gpu_vu_retirements_ready = false;
	bool gpu_vu_device_faulted = false;
	bool reported_first_gpu_vu_draw = false;

	std::vector<RenderTargetEntry> render_targets;
	SceGxmRenderTarget* display_render_target = nullptr;
	VitaGXM::GSTextureGXM* scene_rt = nullptr;
	VitaGXM::GSTextureGXM* scene_ds = nullptr;
	GSVector4i scene_scissor{};
	GSVector2i scene_raster_origin{};
	u16 scene_store_residency =
		VitaGXM::RenderStoreResidencyPlanner::InvalidIndex;
	bool scene_uses_render_store = false;
	u64 scene_serial = 0;
	u64 completed_scene_serial = 0;
	u64 transfer_serial = 0;
	u64 completed_transfer_serial = 0;
	u32 vertex_offset = 0;
	u32 index_offset = 0;
	const u16* gpu_vu_sequential_indices = nullptr;
	const std::vector<std::unique_ptr<VitaGpuVu::GpuVuDraw>>*
		active_gpu_vu_draws = nullptr;
		bool active_gpu_vu_draw_encoded = false;
		const char* active_gpu_vu_draw_failure_reason = nullptr;

	SceGxmShaderPatcherId tfx_vertex_id = nullptr;
	SceGxmShaderPatcherId tfx_fragment_id = nullptr;
	SceGxmShaderPatcherId tfx_manual_lod_fragment_id = nullptr;
	SceGxmShaderPatcherId tfx_manual_lod_zfloor_fragment_id = nullptr;
	SceGxmShaderPatcherId tfx_zfloor_fragment_id = nullptr;
	SceGxmShaderPatcherId tfx_psm16_fragment_id = nullptr;
	SceGxmShaderPatcherId tfx_psm16_zfloor_fragment_id = nullptr;
	SceGxmShaderPatcherId tfx_zfloor_source_direct_decal_af_fragment_id = nullptr;
	SceGxmShaderPatcherId tfx_fast_fragment_id = nullptr;
	SceGxmShaderPatcherId tfx_fast_no_atst_fragment_id = nullptr;
	std::array<SceGxmShaderPatcherId, GS_BLEND_PROGRAM_COUNT> gs_blend_ids{};
	std::array<SceGxmFragmentProgram*, GS_BLEND_PROGRAM_COUNT> gs_blend_programs{};
	SceGxmShaderPatcherId tfx_uv_no_fog_fragment_id = nullptr;
	SceGxmShaderPatcherId tfx_uv_no_fog_fast_fragment_id = nullptr;
	SceGxmShaderPatcherId tfx_uv_no_fog_zfloor_fragment_id = nullptr;
	SceGxmShaderPatcherId tfx_region_repeat_fragment_id = nullptr;
	SceGxmShaderPatcherId tfx_region_repeat_fast_fragment_id = nullptr;
	SceGxmShaderPatcherId tfx_untextured_fragment_id = nullptr;
	SceGxmShaderPatcherId tfx_source_fragment_id = nullptr;
	SceGxmShaderPatcherId tfx_programmable_add_fragment_id = nullptr;
	SceGxmShaderPatcherId tfx_programmable_add_direct_fragment_id = nullptr;
	SceGxmShaderPatcherId tfx_programmable_over_fragment_id = nullptr;
	SceGxmShaderPatcherId tfx_source_direct_fragment_id = nullptr;
	SceGxmShaderPatcherId tfx_source_direct_modulate_fragment_id = nullptr;
	SceGxmShaderPatcherId tfx_source_direct_modulate_af_fragment_id = nullptr;
	SceGxmShaderPatcherId tfx_source_untextured_fragment_id = nullptr;
	SceGxmShaderPatcherId present_vertex_id = nullptr;
	SceGxmShaderPatcherId present_fragment_id = nullptr;
	SceGxmShaderPatcherId merge_fragment_id = nullptr;
	SceGxmShaderPatcherId copy_fragment_id = nullptr;
	SceGxmShaderPatcherId rta_correction_fragment_id = nullptr;
	SceGxmShaderPatcherId rta_decorrection_fragment_id = nullptr;
	SceGxmShaderPatcherId color_vertex_id = nullptr;
	SceGxmShaderPatcherId color_fragment_id = nullptr;
	SceGxmShaderPatcherId mad_buffer_fragment_id = nullptr;
	SceGxmShaderPatcherId mad_reconstruct_fragment_id = nullptr;
	SceGxmVertexProgram* tfx_vertex_program = nullptr;
	SceGxmFragmentProgram* tfx_fragment_program = nullptr;
	SceGxmFragmentProgram* tfx_manual_lod_fragment_program = nullptr;
	SceGxmFragmentProgram* tfx_manual_lod_zfloor_fragment_program = nullptr;
	SceGxmFragmentProgram* tfx_zfloor_fragment_program = nullptr;
	SceGxmFragmentProgram* tfx_psm16_fragment_program = nullptr;
	SceGxmFragmentProgram* tfx_psm16_zfloor_fragment_program = nullptr;
	SceGxmFragmentProgram* tfx_zfloor_source_direct_decal_af_program = nullptr;
	SceGxmFragmentProgram* tfx_opaque_program = nullptr;
	SceGxmFragmentProgram* tfx_fast_no_atst_program = nullptr;
	SceGxmFragmentProgram* tfx_region_repeat_program = nullptr;
	SceGxmFragmentProgram* tfx_region_repeat_opaque_program = nullptr;
	SceGxmFragmentProgram* tfx_programmable_add_program = nullptr;
	SceGxmFragmentProgram* tfx_programmable_add_direct_program = nullptr;
	SceGxmFragmentProgram* tfx_programmable_over_program = nullptr;
	SceGxmVertexProgram* present_vertex_program = nullptr;
	std::array<SceGxmFragmentProgram*, 16> copy_programs{};
	std::array<SceGxmFragmentProgram*, 16> rta_correction_programs{};
	SceGxmFragmentProgram* rta_decorrection_program = nullptr;
	SceGxmFragmentProgram* present_alpha_program = nullptr;
	SceGxmFragmentProgram* present_source_alpha_program = nullptr;
	SceGxmFragmentProgram* present_alpha_rgb_program = nullptr;
	SceGxmFragmentProgram* present_source_alpha_rgb_program = nullptr;
	SceGxmFragmentProgram* mad_buffer_program = nullptr;
	SceGxmFragmentProgram* mad_reconstruct_program = nullptr;
	SceGxmVertexProgram* color_vertex_program = nullptr;
	SceGxmFragmentProgram* color_fragment_program = nullptr;
	SceGxmFragmentProgram* mask_update_program = nullptr;
	ProgramUniforms uniforms;
	ProgramUniforms manual_lod_uniforms;
	ProgramUniforms manual_lod_zfloor_uniforms;
	ProgramUniforms zfloor_uniforms;
	ProgramUniforms psm16_uniforms;
	ProgramUniforms psm16_zfloor_uniforms;
	ProgramUniforms zfloor_source_direct_decal_af_uniforms;
	ProgramUniforms fast_uniforms;
	ProgramUniforms fast_no_atst_uniforms;
	std::array<ProgramUniforms, GS_BLEND_PROGRAM_COUNT> gs_blend_uniforms{};
	ProgramUniforms uv_no_fog_uniforms;
	ProgramUniforms uv_no_fog_fast_uniforms;
	ProgramUniforms uv_no_fog_zfloor_uniforms;
	ProgramUniforms region_repeat_uniforms;
	ProgramUniforms region_repeat_fast_uniforms;
	ProgramUniforms untextured_uniforms;
	ProgramUniforms source_uniforms;
	ProgramUniforms programmable_add_uniforms;
	ProgramUniforms programmable_add_direct_uniforms;
	ProgramUniforms programmable_over_uniforms;
	ProgramUniforms source_direct_uniforms;
	ProgramUniforms source_direct_modulate_uniforms;
	ProgramUniforms source_direct_modulate_af_uniforms;
	ProgramUniforms source_untextured_uniforms;
	std::map<u64, SceGxmFragmentProgram*> tfx_fast_programs;
	std::map<u64, SceGxmFragmentProgram*> tfx_untextured_programs;
	std::map<u64, SceGxmFragmentProgram*> tfx_source_programs;
	std::map<u64, SceGxmFragmentProgram*> tfx_source_direct_programs;
	std::map<u64, SceGxmFragmentProgram*> tfx_source_direct_modulate_programs;
	std::map<u64, SceGxmFragmentProgram*> tfx_source_direct_modulate_af_programs;
	std::map<u64, SceGxmFragmentProgram*> tfx_source_untextured_programs;
	std::map<std::pair<u64, u64>, GeneratedVuProgram> generated_vu_programs;
	bool tfx_patched_program_limit_logged = false;
	bool tfx_zfloor_source_direct_decal_af_logged = false;
	bool tfx_psm16_logged = false;

	std::map<std::pair<int, int>, std::unique_ptr<VitaGXM::GSTextureGXM>> feedback_textures;
	std::map<std::tuple<int, int, GSTexture::Format>, std::unique_ptr<VitaGXM::GSTextureGXM>> post_textures;
	std::unique_ptr<VitaGXM::GSTextureGXM> white_texture;
	std::vector<VitaGXM::ArenaAllocation> quarantined_allocations;

	bool Initialize();
	void Shutdown();
	bool Fail(const char* operation, int result);
	bool Reject(const char* reason);
	bool CreateContext();
	bool CreatePatcher();
	bool CreatePrograms();
#if defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
	bool InitializeUniversalGpuVuValidation();
	bool SubmitUniversalGpuVuValidation();
	bool CompleteUniversalGpuVuValidation();
	bool ReleaseUniversalGpuVuValidationResources();
	bool InitializeGeneratedGpuVuProduct();
	bool InitializeLegacyUniversalGpuVuValidationOwner();
	bool AttestUniversalGpuVuStructuredFmac();
	void ReleaseUniversalGpuVuProduct();
	void ServiceUniversalGpuVuEpoch(
		VitaGpuVu::UniversalGpuVuEpoch* epoch);
#endif
	bool RegisterGeneratedVuProgram(VitaGpuVu::CompileResult result,
		VitaGpuVu::GeneratedCgProgram metadata);
	void PollGeneratedVuPrograms();
	GeneratedVuProgram* FindGeneratedVuProgram(
		const VitaGpuVu::ShaderKey& key);
	bool DrawGpuVu(const GSHWDrawConfig& config,
		VitaGXM::GSTextureGXM* source, bool fast_fragment, bool psm16_fragment,
		bool region_repeat_fragment, bool manual_lod_fragment);
	bool InitializeGpuVuRetirements();
	bool RetainGpuVuDrawForScene(
		std::unique_ptr<VitaGpuVu::GpuVuDraw> draw);
	bool RetainGpuVuDrawsForScene(
		std::vector<std::unique_ptr<VitaGpuVu::GpuVuDraw>> draws);
	bool PrepareGpuVuRetirementNotification(
		const SceGxmNotification** vertex_notification,
		const SceGxmNotification** fragment_notification);
	bool WaitForGpuVuRetirementSlot(
		GpuVuRetirementSlot& slot, const char* owner,
		GpuVuHealthRetirementAction action);
	void PublishGpuVuRetirementHealth(
		GpuVuHealthRetirementStage stage,
		GpuVuHealthRetirementAction action,
		u32 current_slot = VitaGpuVu::HealthJournal::InvalidSlot);
	bool FailGpuVuRetirementOwner(
		const char* owner, uptr address, u32 required_value,
		u32 observed_value, u64 sequence, u64 elapsed_us);
	bool FlushGeneratedGpuVuTransaction();
	void AccumulateGpuVuGeneratedSceneManifest(
		VitaGS::GpuVuGxmCallKind kind, u64 sequence,
		const VitaGS::GpuVuGxmCallBreadcrumb& breadcrumb);
	bool WaitForGeneratedSceneFragmentCompletion(
		const SceGxmNotification& notification,
		const GpuVuGeneratedSceneManifest& manifest);
	bool ArmPendingGpuVuRetirementWake();
	u64 OldestPendingGpuVuSequence() const;
	bool ArmGpuVuPreNotificationWatchdog(u64 sequence);
	void UpdateGpuVuPreNotificationWatchdog(
		VitaGS::GpuVuGxmSubmissionStage stage, u64 sequence);
	void UpdateGpuVuPreNotificationOwnership(
		const VitaGS::GpuVuGxmOwnershipBreadcrumb& breadcrumb);
	void CompleteGpuVuPreNotificationWatchdog();
		bool RetireCompletedGpuVuDraws();
		void RetireGpuVuPrivateStoreOutputs(
			std::vector<GpuVuPrivateStoreOutput>* outputs,
			bool compare_results);
	bool WaitForGpuVuInputRetirement(
		const VitaGpuVu::RawVifPayloadRef& blocked_generation);
	void ReleaseAllGpuVuDraws();
	bool CreateGeometry();
	bool InitializeRenderStore();
	bool CanUseRenderStore(VitaGXM::GSTextureGXM* rt,
		VitaGXM::GSTextureGXM* ds) const;
	bool AcquireRenderStore(VitaGXM::GSTextureGXM* rt,
		VitaGXM::GSTextureGXM* ds,
		VitaGXM::RenderStoreResidencyPlanner::Acquisition* acquisition);
	bool LoadRenderStoreResidency(VitaGXM::GSTextureGXM* rt,
		VitaGXM::GSTextureGXM* ds,
		const VitaGXM::RenderStoreResidencyPlanner::Acquisition& acquisition);
	bool EnsureTextureBackingCurrent(VitaGXM::GSTextureGXM& texture);
	bool BindLatestRenderStoreView(VitaGXM::GSTextureGXM& texture);
	bool CopyTextureStoreRegion(VitaGXM::GSTextureGXM& texture,
		const VitaGXM::RenderStoreAllocator::Region& region, bool to_store);
	void MarkSceneStoreWrite(VitaGXM::GSTextureGXM* texture,
		VitaGXM::RenderStoreAllocator::Plane plane);
	void MarkTextureBackingWrite(VitaGXM::GSTextureGXM& texture);
	bool CreateRenderTarget(u32 width, u32 height, u16 scenes_per_frame,
		SceGxmRenderTarget** target, bool required = true);
	SceGxmRenderTarget* GetRenderTarget(u32 width, u32 height);
	void RetuneRenderTargets();
	bool EndScene(bool finish);
	bool HasPendingGpuVuSceneDraws() const
	{
		return gpu_vu_scene_draw_count != 0;
	}
	bool Finish();
	bool CommitClear(VitaGXM::GSTextureGXM& texture);
	bool CommitAttachmentClears(VitaGXM::GSTextureGXM* rt,
		VitaGXM::GSTextureGXM* ds, const GSVector4i& scissor);
	bool DrawTargetClear(u32 color, bool write_color, float depth,
		bool write_depth);
	bool EnsureScene(VitaGXM::GSTextureGXM* rt, VitaGXM::GSTextureGXM* ds,
		const GSVector4i& scissor);
	bool BeginDisplayScene();
	void ConfigureRaster(u32 width, u32 height,
		u32 origin_x = 0, u32 origin_y = 0);
	bool ConfigureScissor(const GSVector4i& scissor, u32 width, u32 height,
		bool exact_depth);
#if defined(VITASX2_GXM_SCISSOR_VALIDATION)
	bool ValidateScissorMaskStorage();
#endif
#if defined(VITASX2_GXM_TFX_ALPHA_VALIDATION)
	bool ValidateTfxAlphaSpecialization();
#endif
#if defined(VITASX2_GXM_GS_BLEND_VALIDATION)
	bool ValidateGsBlend();
#endif
#if defined(VITASX2_GXM_ATTACHMENT_CLEAR_VALIDATION)
	bool ValidateAttachmentClears();
#endif
	bool DrawMaskRect(const GSVector4i& rect, u32 width, u32 height,
		SceGxmStencilFunc operation);
	bool HasGeometryCapacity(u32 vertices, u32 indices) const;
	bool ReserveGeometry(u32 vertices, u32 indices, void** vertex_data,
		u16** index_data);
	bool DrawQuad(VitaGXM::GSTextureGXM* source, const GSVector4& source_rect,
		const GSVector4& destination_rect, u32 color, Filter filter,
		SceGxmFragmentProgram* fragment, const SceGxmProgramParameter* uniform = nullptr,
		const float* uniform_data = nullptr);
	bool StageAndDraw(const GSHWDrawConfig& config,
		const GSHWDrawConfig::PSSelector& ps, u32 first_index, u32 index_count,
		VitaGXM::GSTextureGXM* source, SceGxmFragmentProgram* fragment,
		bool region_repeat_fragment, bool fast_fragment,
		bool programmable_add_fragment,
		bool programmable_add_direct_fragment,
		bool programmable_over_fragment,
		bool psm16_fragment,
		bool source_only_fragment,
		bool source_direct_fragment,
		bool source_direct_modulate_fragment,
		bool source_direct_modulate_af_fragment,
		bool untextured_fragment);
	bool UploadTfxUniforms(const GSHWDrawConfig& config,
		const GSHWDrawConfig::PSSelector& ps, VitaGXM::GSTextureGXM* source,
		bool zfloor_programmable_constant_fragment,
		bool region_repeat_fragment, bool fast_fragment,
		bool programmable_add_fragment,
		bool programmable_add_direct_fragment,
		bool programmable_over_fragment,
		bool psm16_fragment,
		bool source_only_fragment,
		bool source_direct_fragment,
		bool source_direct_modulate_fragment,
		bool source_direct_modulate_af_fragment,
		bool untextured_fragment,
		const GeneratedVuProgram* generated_vu = nullptr,
		const VitaGpuVu::GpuVuDraw* gpu_vu_draw = nullptr,
		bool no_alpha_test_fragment = false);
	bool CanUseFastTfx(const GSHWDrawConfig& config) const;
	bool CanUseGsSourceOnlyTfx(const GSHWDrawConfig& config) const;
	bool CanUseSourceOnlyTfx(const GSHWDrawConfig& config) const;
	bool CanUseSourceDirectTfx(const GSHWDrawConfig& config) const;
	bool CanUseSourceDirectModulateTfx(const GSHWDrawConfig& config) const;
	bool CanUseSourceDirectModulateAfTfx(const GSHWDrawConfig& config) const;
	bool CanUseZfloorSourceDirectDecalAfTfx(
		const GSHWDrawConfig& config) const;
	SceGxmFragmentProgram* GetPatchedTfxProgram(const GSHWDrawConfig& config,
		bool source_only_fragment, bool source_direct_fragment,
		bool source_direct_modulate_fragment,
		bool source_direct_modulate_af_fragment,
		bool untextured_fragment);
	VitaGXM::GSTextureGXM* SnapshotTexture(VitaGXM::GSTextureGXM& source,
		const GSVector4i& area);

	VitaGXM::Arena& TextureArena() override { return texture_arena; }
	VitaGXM::Arena& TransferArena() override { return transfer_arena; }
	bool RetireTextureScene(VitaGXM::GSTextureGXM& texture,
		bool preserve_contents, u64* serial) override;
	bool QueueTextureUpload(VitaGXM::GSTextureGXM& texture, u32 level,
		const GSVector4i& destination, u32 source_pitch,
		VitaGXM::ArenaAllocation source, u64* serial) override;
	bool QueueTextureReadback(VitaGXM::GSTextureGXM& texture, u32 level,
		const GSVector4i& source, void* destination, u32 destination_pitch,
		const GSVector4i& destination_rect, u64* serial) override;
	bool QueueTextureMipmaps(VitaGXM::GSTextureGXM& texture,
		u64* serial) override;
	bool WaitForTextureTransfer(u64 serial) override;
	void RetireTextureAllocation(VitaGXM::ArenaAllocation allocation,
		const VitaGXM::TextureCompletionFence& fence) override;
};

bool GSDeviceGXM::Impl::Fail(const char* operation, int result)
{
	if (active_gpu_vu_draws && !active_gpu_vu_draw_failure_reason)
		active_gpu_vu_draw_failure_reason = operation;
	Console.Error("GXM GS: %s failed (%08x).", operation, static_cast<u32>(result));
	return false;
}

bool GSDeviceGXM::Impl::Reject(const char* reason)
{
	if (active_gpu_vu_draws && !active_gpu_vu_draw_failure_reason)
		active_gpu_vu_draw_failure_reason = reason;
	s_gxm_worker_performance.device_rejects++;
	s_gxm_worker_performance.last_device_reject_hash = HashRejectReason(reason);
	Console.Error("GXM GS: rejected unsupported PCSX2 device contract: %s.", reason);
	return false;
}

bool GSDeviceGXM::Impl::CreateContext()
{
	context_host = std::malloc(SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE);
	if (!context_host)
		return Fail("context host allocation", SCE_GXM_ERROR_OUT_OF_MEMORY);

	int result = VitaGXM::AllocateMappedBlock("VitaSX2 GXM VDM ring",
		SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_NC_RW,
		SCE_GXM_DEFAULT_VDM_RING_BUFFER_SIZE, SCE_GXM_MEMORY_ATTRIB_READ,
		&vdm_ring);
	if (result < 0)
		return Fail("VDM ring allocation", result);
	result = VitaGXM::AllocateMappedBlock("VitaSX2 GXM vertex ring",
		SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_NC_RW,
		SCE_GXM_DEFAULT_VERTEX_RING_BUFFER_SIZE, SCE_GXM_MEMORY_ATTRIB_READ,
		&vertex_ring);
	if (result < 0)
		return Fail("vertex ring allocation", result);
	result = VitaGXM::AllocateMappedBlock("VitaSX2 GXM fragment ring",
		SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_NC_RW,
		SCE_GXM_DEFAULT_FRAGMENT_RING_BUFFER_SIZE, SCE_GXM_MEMORY_ATTRIB_READ,
		&fragment_ring);
	if (result < 0)
		return Fail("fragment ring allocation", result);
	result = VitaGXM::AllocateFragmentUsseBlock("VitaSX2 GXM fragment USSE ring",
		SCE_GXM_DEFAULT_FRAGMENT_USSE_RING_BUFFER_SIZE, &fragment_usse_ring);
	if (result < 0)
		return Fail("fragment USSE ring allocation", result);

	SceGxmContextParams params{};
	params.hostMem = context_host;
	params.hostMemSize = SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE;
	params.vdmRingBufferMem = vdm_ring.base;
	params.vdmRingBufferMemSize = SCE_GXM_DEFAULT_VDM_RING_BUFFER_SIZE;
	params.vertexRingBufferMem = vertex_ring.base;
	params.vertexRingBufferMemSize = SCE_GXM_DEFAULT_VERTEX_RING_BUFFER_SIZE;
	params.fragmentRingBufferMem = fragment_ring.base;
	params.fragmentRingBufferMemSize = SCE_GXM_DEFAULT_FRAGMENT_RING_BUFFER_SIZE;
	params.fragmentUsseRingBufferMem = fragment_usse_ring.base;
	params.fragmentUsseRingBufferMemSize =
		SCE_GXM_DEFAULT_FRAGMENT_USSE_RING_BUFFER_SIZE;
	params.fragmentUsseRingBufferOffset = fragment_usse_ring.usse_offset;
	result = sceGxmCreateContext(&params, &context);
	return (result >= 0 && context) ? true :
		Fail("sceGxmCreateContext", result < 0 ? result : SCE_GXM_ERROR_INVALID_POINTER);
}

bool GSDeviceGXM::Impl::CreatePatcher()
{
	int result = VitaGXM::AllocateMappedBlock("VitaSX2 GXM shader patcher",
		SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, PATCHER_BUFFER_BYTES,
		SCE_GXM_MEMORY_ATTRIB_RW, &patcher_buffer);
	if (result < 0)
		return Fail("shader patcher buffer allocation", result);
	result = VitaGXM::AllocateVertexUsseBlock("VitaSX2 GXM vertex shader heap",
		PATCHER_VERTEX_USSE_BYTES, &patcher_vertex_usse);
	if (result < 0)
		return Fail("vertex shader heap allocation", result);
	result = VitaGXM::AllocateFragmentUsseBlock("VitaSX2 GXM fragment shader heap",
		PATCHER_FRAGMENT_USSE_BYTES, &patcher_fragment_usse);
	if (result < 0)
		return Fail("fragment shader heap allocation", result);

	SceGxmShaderPatcherParams params{};
	params.hostAllocCallback = PatcherHostAlloc;
	params.hostFreeCallback = PatcherHostFree;
	params.bufferMem = patcher_buffer.base;
	params.bufferMemSize = PATCHER_BUFFER_BYTES;
	params.vertexUsseMem = patcher_vertex_usse.base;
	params.vertexUsseMemSize = PATCHER_VERTEX_USSE_BYTES;
	params.vertexUsseOffset = patcher_vertex_usse.usse_offset;
	params.fragmentUsseMem = patcher_fragment_usse.base;
	params.fragmentUsseMemSize = PATCHER_FRAGMENT_USSE_BYTES;
	params.fragmentUsseOffset = patcher_fragment_usse.usse_offset;
	result = sceGxmShaderPatcherCreate(&params, &patcher);
	if (result < 0 || !patcher)
	{
		return Fail("sceGxmShaderPatcherCreate",
			result < 0 ? result : SCE_GXM_ERROR_INVALID_POINTER);
	}
	const ShaderPatcherUsage usage = GetShaderPatcherUsage(patcher);
	Console.WriteLn(
		"GXM shader patcher phase=create host=%u buffer=%u/%u "
		"vertex_usse=%u/%u fragment_usse=%u/%u.",
		usage.host, usage.buffer, PATCHER_BUFFER_BYTES,
		usage.vertex_usse, PATCHER_VERTEX_USSE_BYTES,
		usage.fragment_usse, PATCHER_FRAGMENT_USSE_BYTES);
	return true;
}

bool GSDeviceGXM::Impl::CreateGeometry()
{
	// CPU-to-GPU streams remain uncached and retain the same mapping, extent,
	// fetch guards and retirement ownership. Only their physical pool changes.
	// PCSX2's VKStreamBuffer::{Create,Destroy} owns the corresponding lifetime;
	// Sony's graphicsAlloc/sceGxmMapMemory contract permits mapped CDRAM here.
	int result = VitaGXM::AllocateMappedBlock("VitaSX2 GXM staged vertices",
		SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, GEOMETRY_VERTEX_MAPPED_BYTES,
		SCE_GXM_MEMORY_ATTRIB_READ, &geometry_vertices);
	if (result < 0)
		return Fail("staged vertex allocation", result);
	result = VitaGXM::AllocateMappedBlock("VitaSX2 GXM staged indices",
		SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, GEOMETRY_INDEX_MAPPED_BYTES,
		SCE_GXM_MEMORY_ATTRIB_READ, &geometry_indices);
	if (result < 0)
		return Fail("staged index allocation", result);

	// Direct VU+TFX draws must not construct one CPU index per generated
	// vertex. Reserve the tail of the already mapped index block and build the
	// identity sequence once. GIF NLOOP is 15 bits, but keeping the complete
	// u16 domain also covers future coalesced direct batches.
	std::memset(static_cast<u8*>(geometry_vertices.base) +
		GEOMETRY_VERTEX_BYTES, 0, VitaGXM::GpuMappedFetchGuardSize);
	std::memset(static_cast<u8*>(geometry_indices.base) +
		GEOMETRY_INDEX_BYTES, 0, VitaGXM::GpuMappedFetchGuardSize);
	u16* const indices = reinterpret_cast<u16*>(
		static_cast<u8*>(geometry_indices.base) +
		GEOMETRY_INDEX_BYTES - GPU_VU_SEQUENTIAL_INDEX_BYTES);
	for (u32 i = 0; i < GPU_VU_SEQUENTIAL_INDEX_COUNT; i++)
		indices[i] = static_cast<u16>(i);
	gpu_vu_sequential_indices = indices;
	return true;
}

bool GSDeviceGXM::Impl::InitializeRenderStore()
{
#if !defined(VITASX2_GXM_PERSISTENT_RENDER_STORE) || \
	!VITASX2_GXM_PERSISTENT_RENDER_STORE
	// The first physical BIOS experiment immediately materialized 688 of 693
	// planned scenes, did not reduce scene count, regressed wall time, and
	// produced visible corruption. Preserve the implementation for bounded
	// follow-up work, but never put it in an unrelated retail-game baseline.
	return true;
#else
	using Allocator = VitaGXM::RenderStoreAllocator;
	constexpr u32 width = Allocator::StoreWidth;
	constexpr u32 height = Allocator::StoreHeight;
	constexpr size_t pixels = static_cast<size_t>(width) * height;
	const VitaGXM::TextureFormatInfo* const color_format =
		VitaGXM::GetTextureFormatInfo(GSTexture::Format::Color);
	if (!color_format)
		return false;

	int result = texture_arena.Allocate(pixels * 4,
		SCE_GXM_COLOR_SURFACE_ALIGNMENT, &render_store_color);
	if (result < 0)
		return Fail("render-store color allocation", result);
	result = texture_arena.Allocate(pixels * 4,
		SCE_GXM_DEPTHSTENCIL_SURFACE_ALIGNMENT, &render_store_depth);
	if (result < 0)
		return Fail("render-store depth allocation", result);
	result = texture_arena.Allocate(pixels,
		SCE_GXM_DEPTHSTENCIL_SURFACE_ALIGNMENT, &render_store_stencil);
	if (result < 0)
		return Fail("render-store stencil allocation", result);

	// Linear storage is intentional. Unlike a tiled descriptor, it permits a
	// logical target to be exposed as a LINEAR_STRIDED subregion and permits
	// PTLA to copy between pair residencies at non-zero coordinates. SGX still
	// rasterizes through its on-chip 32x32 tiles; the allocator keeps every
	// logical region tile-aligned so unrelated targets never share a tile.
	result = sceGxmColorSurfaceInit(&render_store_color_surface,
		color_format->color_format, SCE_GXM_COLOR_SURFACE_LINEAR,
		SCE_GXM_COLOR_SURFACE_SCALE_NONE, color_format->output_register_size,
		width, height, width, render_store_color.Data());
	if (result < 0)
		return Fail("render-store color surface", result);
	result = sceGxmDepthStencilSurfaceInit(&render_store_depth_surface,
		SCE_GXM_DEPTH_STENCIL_FORMAT_DF32M_S8,
		SCE_GXM_DEPTH_STENCIL_SURFACE_LINEAR, width,
		render_store_depth.Data(), render_store_stencil.Data());
	if (result < 0)
		return Fail("render-store depth surface", result);
	sceGxmDepthStencilSurfaceSetForceLoadMode(&render_store_depth_surface,
		SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_ENABLED);
	sceGxmDepthStencilSurfaceSetForceStoreMode(&render_store_depth_surface,
		SCE_GXM_DEPTH_STENCIL_FORCE_STORE_ENABLED);
	sceGxmColorSurfaceSetClip(&render_store_color_surface, 0, 0,
		width - 1, height - 1);
	render_store_ready = true;
	Console.WriteLn(
		"GXM GS: persistent linear render store ready color=%ux%u depth=%ux%u bytes=%u.",
		width, height, width, height, static_cast<u32>(pixels * 9));
	return true;
#endif
}

bool GSDeviceGXM::Impl::CanUseRenderStore(VitaGXM::GSTextureGXM* rt,
	VitaGXM::GSTextureGXM* ds) const
{
	using Allocator = VitaGXM::RenderStoreAllocator;
	if (!render_store_ready || (!rt && !ds))
		return false;
	const VitaGXM::GSTextureGXM* const target = rt ? rt : ds;
	const GSVector2i size = target->GetSize();
	if (size.x <= 0 || size.y <= 0 ||
		static_cast<u32>(size.x) > Allocator::StoreWidth ||
		static_cast<u32>(size.y) > Allocator::StoreHeight ||
		(rt && (!rt->HasGuestRenderTargetIdentity() ||
			rt->GuestTargetIdentity().depth)) ||
		(ds && (!ds->HasGuestRenderTargetIdentity() ||
			!ds->GuestTargetIdentity().depth)))
	{
		return false;
	}
	return (!rt || rt->GetSize() == size) && (!ds || ds->GetSize() == size);
}

bool GSDeviceGXM::Impl::AcquireRenderStore(VitaGXM::GSTextureGXM* rt,
	VitaGXM::GSTextureGXM* ds,
	VitaGXM::RenderStoreResidencyPlanner::Acquisition* acquisition)
{
	if (!CanUseRenderStore(rt, ds) || !acquisition)
		return false;
	VitaGXM::RenderStoreResidencyPlanner::PairKey key;
	if (rt)
		key.color = MakeRenderStoreTargetKey(*rt);
	if (ds)
		key.depth = MakeRenderStoreTargetKey(*ds);
	const GSVector2i size = (rt ? rt : ds)->GetSize();
	const bool acquired = render_store_planner.Acquire(key,
		static_cast<u32>(size.x), static_cast<u32>(size.y), acquisition);
	if (VitaPerformanceTelemetry::IsEnabled())
	{
		s_gxm_worker_performance.render_store_acquisitions++;
		if (!acquired)
			s_gxm_worker_performance.render_store_failures++;
		else if (acquisition->new_residency)
			s_gxm_worker_performance.render_store_new_residencies++;
	}
	return acquired;
}

bool GSDeviceGXM::Impl::CopyTextureStoreRegion(
	VitaGXM::GSTextureGXM& texture,
	const VitaGXM::RenderStoreAllocator::Region& region, bool to_store)
{
	const VitaGXM::TextureLevelLayout* const level = texture.Level(0);
	if (!level || !region || region.width < static_cast<u32>(texture.GetWidth()) ||
		region.height < static_cast<u32>(texture.GetHeight()))
	{
		return false;
	}
	const bool depth = texture.IsDepthStencil();
	const SceGxmTransferType texture_type = level->tiled ?
		SCE_GXM_TRANSFER_TILED : SCE_GXM_TRANSFER_LINEAR;
	const u32 store_pitch = VitaGXM::RenderStoreAllocator::StoreWidth * 4u;
	const void* const source = to_store ? texture.LevelData(0) :
		static_cast<const u8*>(depth ? render_store_depth.Data() :
			render_store_color.Data());
	void* const destination = to_store ?
		(depth ? render_store_depth.Data() : render_store_color.Data()) :
		texture.LevelData(0);
	const u32 source_x = to_store ? 0u : region.x;
	const u32 source_y = to_store ? 0u : region.y;
	const u32 destination_x = to_store ? region.x : 0u;
	const u32 destination_y = to_store ? region.y : 0u;
	const s32 source_stride = to_store ? static_cast<s32>(level->pitch) :
		static_cast<s32>(store_pitch);
	const s32 destination_stride = to_store ? static_cast<s32>(store_pitch) :
		static_cast<s32>(level->pitch);
	const SceGxmTransferType source_type = to_store ? texture_type :
		SCE_GXM_TRANSFER_LINEAR;
	const SceGxmTransferType destination_type = to_store ?
		SCE_GXM_TRANSFER_LINEAR : texture_type;
	// Tiled GXM surfaces are allocated at the tile-rounded extent and PTLA
	// requires tile-sized copies. Small/linear surfaces are only allocated at
	// their logical extent, so copying the residency's padded rectangle would
	// walk beyond their backing allocation.
	const u32 copy_width = level->tiled ? region.width :
		static_cast<u32>(texture.GetWidth());
	const u32 copy_height = level->tiled ? region.height :
		static_cast<u32>(texture.GetHeight());
	const int result = sceGxmTransferCopy(copy_width, copy_height, 0, 0,
		SCE_GXM_TRANSFER_COLORKEY_NONE, texture.NativeFormat().transfer_format,
		source_type, source, source_x, source_y, source_stride,
		texture.NativeFormat().transfer_format, destination_type, destination,
		destination_x, destination_y, destination_stride, nullptr, 0, nullptr);
	if (result < 0)
		return Fail(to_store ? "load render-store target" :
			"materialize render-store target", result);

	if (depth)
	{
		const void* const stencil_source = to_store ? texture.StencilData() :
			render_store_stencil.Data();
		void* const stencil_destination = to_store ? render_store_stencil.Data() :
			texture.StencilData();
		const s32 store_stencil_pitch =
			static_cast<s32>(VitaGXM::RenderStoreAllocator::StoreWidth);
		const int stencil_result = sceGxmTransferCopy(copy_width, copy_height,
			0, 0, SCE_GXM_TRANSFER_COLORKEY_NONE,
			SCE_GXM_TRANSFER_FORMAT_U8_R, SCE_GXM_TRANSFER_LINEAR,
			stencil_source, source_x, source_y,
			to_store ? static_cast<s32>(texture.StencilPitch()) :
				store_stencil_pitch,
			SCE_GXM_TRANSFER_FORMAT_U8_R, SCE_GXM_TRANSFER_LINEAR,
			stencil_destination, destination_x, destination_y,
			to_store ? store_stencil_pitch :
				static_cast<s32>(texture.StencilPitch()), nullptr, 0, nullptr);
		if (stencil_result < 0)
			return Fail(to_store ? "load render-store stencil" :
				"materialize render-store stencil", stencil_result);
	}
	return true;
}

bool GSDeviceGXM::Impl::EnsureTextureBackingCurrent(
	VitaGXM::GSTextureGXM& texture)
{
	if (!render_store_ready || !texture.HasGuestRenderTargetIdentity())
		return true;
	const VitaGXM::RenderStoreTargetKey key = MakeRenderStoreTargetKey(texture);
	if (!render_store_planner.BackingNeedsStore(key))
		return true;
	if (!EndScene(false))
		return false;
	const u16 latest = render_store_planner.LatestResidency(key);
	const VitaGXM::RenderStoreAllocator::Region* const region =
		render_store_planner.Region(latest);
	if (!region || !CopyTextureStoreRegion(texture, *region, false))
		return false;
	if (VitaPerformanceTelemetry::IsEnabled())
		s_gxm_worker_performance.render_store_materializations++;
	const int finish_result = sceGxmTransferFinish();
	if (finish_result < 0)
		return Fail("finish render-store materialization", finish_result);
	texture.InvalidateDepthMaskScissor();
	return render_store_planner.MarkBackingCurrent(key);
}

bool GSDeviceGXM::Impl::BindLatestRenderStoreView(
	VitaGXM::GSTextureGXM& texture)
{
	if (!render_store_ready || !texture.IsRenderTarget() ||
		!texture.HasGuestRenderTargetIdentity())
	{
		return false;
	}
	const VitaGXM::RenderStoreTargetKey key = MakeRenderStoreTargetKey(texture);
	const u16 latest = render_store_planner.LatestResidency(key);
	const VitaGXM::RenderStoreAllocator::Region* const region =
		render_store_planner.Region(latest);
	if (!region)
		return false;
	constexpr u32 pitch = VitaGXM::RenderStoreAllocator::StoreWidth * 4u;
	u8* const data = static_cast<u8*>(render_store_color.Data()) +
		static_cast<size_t>(region->y) * pitch +
		static_cast<size_t>(region->x) * 4u;
	return texture.SetLinearSamplingOverride(data,
		static_cast<u32>(texture.GetWidth()),
		static_cast<u32>(texture.GetHeight()), pitch);
}

bool GSDeviceGXM::Impl::LoadRenderStoreResidency(
	VitaGXM::GSTextureGXM* rt, VitaGXM::GSTextureGXM* ds,
	const VitaGXM::RenderStoreResidencyPlanner::Acquisition& acquisition)
{
	using Plane = VitaGXM::RenderStoreAllocator::Plane;
	bool submitted = false;
	const auto load = [&](VitaGXM::GSTextureGXM* texture, Plane plane,
		bool needed) {
		if (!texture || !needed)
			return true;
		// A full lazy clear overwrites this plane; importing its undefined old
		// backing would only add a transfer and cannot affect GS-visible data.
		if (clear_commit_depth != 0)
			return render_store_planner.MarkLoaded(acquisition.residency, plane);
		if (!EnsureTextureBackingCurrent(*texture) ||
			!CopyTextureStoreRegion(*texture, acquisition.region, true))
		{
			return false;
		}
		submitted = true;
		if (VitaPerformanceTelemetry::IsEnabled())
			s_gxm_worker_performance.render_store_loads++;
		return render_store_planner.MarkLoaded(acquisition.residency, plane);
	};
	if (!load(rt, Plane::Color, acquisition.load_color) ||
		!load(ds, Plane::Depth, acquisition.load_depth))
	{
		return false;
	}
	if (!submitted)
		return true;
	const int result = sceGxmTransferFinish();
	return result >= 0 ? true : Fail("finish render-store load", result);
}

void GSDeviceGXM::Impl::MarkSceneStoreWrite(VitaGXM::GSTextureGXM* texture,
	VitaGXM::RenderStoreAllocator::Plane plane)
{
	if (!texture || !scene_uses_render_store ||
		scene_store_residency ==
			VitaGXM::RenderStoreResidencyPlanner::InvalidIndex)
	{
		return;
	}
	if (!render_store_planner.MarkWritten(scene_store_residency, plane))
		Reject("render-store write without a live residency");
}

void GSDeviceGXM::Impl::MarkTextureBackingWrite(
	VitaGXM::GSTextureGXM& texture)
{
	if (render_store_ready && texture.HasGuestRenderTargetIdentity() &&
		!render_store_planner.MarkBackingWritten(
			MakeRenderStoreTargetKey(texture)))
	{
		Reject("render-store target table exhausted by backing write");
	}
}

bool GSDeviceGXM::Impl::CreateRenderTarget(u32 width, u32 height,
	u16 scenes_per_frame, SceGxmRenderTarget** target, bool required)
{
	if (!target || width == 0 || height == 0 || width > 4096 || height > 4096 ||
		scenes_per_frame == 0 ||
		scenes_per_frame > MAX_GXM_SCENES_PER_RENDER_TARGET)
		return false;
	SceGxmRenderTargetParams params{};
	params.width = static_cast<u16>(width);
	params.height = static_cast<u16>(height);
	params.scenesPerFrame = scenes_per_frame;
	params.multisampleMode = SCE_GXM_MULTISAMPLE_NONE;
	params.driverMemBlock = static_cast<SceUID>(-1);
	const int result = sceGxmCreateRenderTarget(&params, target);
	if (result >= 0 && *target)
		return true;
	const int error = result < 0 ? result : SCE_GXM_ERROR_INVALID_POINTER;
	if (required)
		return Fail("sceGxmCreateRenderTarget", error);
	Console.Warning(
		"GXM GS: retaining smaller per-frame scene capacity after render-target upgrade failed (%08x).",
		static_cast<u32>(error));
	return false;
}

SceGxmRenderTarget* GSDeviceGXM::Impl::GetRenderTarget(u32 width, u32 height)
{
	for (RenderTargetEntry& entry : render_targets)
	{
		if (entry.width == width && entry.height == height)
		{
			if (entry.targets.empty())
				return nullptr;
			const u32 scene = entry.scenes_this_frame++;
			const u32 target_index =
				(scene / MAX_GXM_SCENES_PER_RENDER_TARGET) %
				static_cast<u32>(entry.targets.size());
			return entry.targets[target_index];
		}
	}
	if (render_targets.size() >= MAX_RENDER_TARGETS)
	{
		Reject("more than 48 live GXM render-target geometries");
		return nullptr;
	}
	SceGxmRenderTarget* target = nullptr;
	if (!CreateRenderTarget(width, height,
			MAX_GXM_SCENES_PER_RENDER_TARGET, &target))
		return nullptr;
	RenderTargetEntry entry;
	entry.width = width;
	entry.height = height;
	entry.scenes_this_frame = 1;
	entry.targets.push_back(target);
	render_targets.push_back(std::move(entry));
	return target;
}

void GSDeviceGXM::Impl::RetuneRenderTargets()
{
	u32 target_count = 0;
	for (const RenderTargetEntry& entry : render_targets)
		target_count += static_cast<u32>(entry.targets.size());
	for (RenderTargetEntry& entry : render_targets)
	{
		const u32 observed_scenes = entry.scenes_this_frame;
		entry.scenes_this_frame = 0;
		if (entry.growth_disabled || entry.targets.empty() ||
			target_count >= MAX_GXM_RENDER_TARGET_OBJECTS)
			continue;
		const u32 wanted = std::min<u32>(
			MAX_GXM_RENDER_TARGETS_PER_GEOMETRY,
			std::max<u32>(1u, (observed_scenes +
				MAX_GXM_SCENES_PER_RENDER_TARGET - 1u) /
				MAX_GXM_SCENES_PER_RENDER_TARGET));
		while (entry.targets.size() < wanted &&
			target_count < MAX_GXM_RENDER_TARGET_OBJECTS)
		{
			SceGxmRenderTarget* target = nullptr;
			if (!CreateRenderTarget(entry.width, entry.height,
					MAX_GXM_SCENES_PER_RENDER_TARGET, &target, false))
			{
				entry.growth_disabled = true;
				break;
			}
			entry.targets.push_back(target);
			target_count++;
			Console.WriteLn(
				"GXM GS: render-target ring geometry=%ux%u objects=%u observed_scenes=%u.",
				entry.width, entry.height,
				static_cast<u32>(entry.targets.size()), observed_scenes);
		}
	}
}

bool GSDeviceGXM::Impl::Initialize()
{
	SceGxmInitializeParams params{};
	params.flags = SCE_GXM_INITIALIZE_FLAG_DEFAULT;
	params.displayQueueMaxPendingCount = VitaGXM::Display::BufferCount;
	params.displayQueueCallback = VitaGXM::Display::DisplayQueueCallback;
	params.displayQueueCallbackDataSize = VitaGXM::Display::DisplayQueueCallbackDataSize();
	params.parameterBufferSize = SCE_GXM_DEFAULT_PARAMETER_BUFFER_SIZE;
	int result = sceGxmInitialize(&params);
	if (result < 0)
		return Fail("sceGxmInitialize", result);
	gxm_initialized = true;
	if (!InitializeGpuVuRetirements())
		Console.Warning("GPU-VU: four-way vertex retirement ring is unavailable.");
	if (!VitaGpuVu::AttachGeneratedProgramCompiler(
		&gpu_vu_shader_compiler))
	{
		Console.Warning(
			"GPU-VU: generated-program registry already has another compiler owner.");
	}
#if defined(VITASX2_GPU_VU_DIRECT_ADMISSION) && \
	VITASX2_GPU_VU_DIRECT_ADMISSION
	// The installed ShaccCg module needs physical pages of its own. Reserve it
	// before optional mapped arenas and the immutable VIF input generations;
	// otherwise a cold first VU program can find only the final 1 MiB page and
	// fail with SCE_KERNEL_ERROR_NO_FREE_PHYSICAL_PAGE. This waits only for
	// service initialization. Every generated program still compiles on the
	// low-priority worker while the fixed universal GXP owns cold epochs.
	if (!gpu_vu_shader_compiler.StartAndWaitUntilReady())
	{
		Console.Warning(
			"GPU-VU: runtime compiler could not be reserved before mapped GPU owners; fixed universal execution remains available.");
	}
	ReportGpuVuMemoryPhase("compiler-reserved");
#endif

	result = texture_arena.Initialize(VitaGXM::ArenaMemory::Cdram,
		"VitaSX2 GXM textures", 4 * 1024 * 1024, SCE_GXM_MEMORY_ATTRIB_RW);
	if (result < 0)
		return Fail("texture arena initialization", result);
	result = transfer_arena.Initialize(VitaGXM::ArenaMemory::MainNonCached,
		"VitaSX2 GXM transfers", 2 * 1024 * 1024, SCE_GXM_MEMORY_ATTRIB_RW);
	if (result < 0)
		return Fail("transfer arena initialization", result);

	if (!CreateContext() || !CreatePatcher() || !CreatePrograms() ||
		!CreateGeometry() || !InitializeRenderStore() ||
		!CreateRenderTarget(VitaGXM::Display::Width, VitaGXM::Display::Height, 1,
			&display_render_target))
	{
		return false;
	}
	ReportGpuVuMemoryPhase("base-renderer-ready");
#if defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
		if (!InitializeGeneratedGpuVuProduct())
		{
			// Every product allocation is transactional even during startup. A
			// missing/stale sidecar or patcher failure must not strand mapped
			// physical pages and then make the independent VIF input ring fail.
			ReleaseUniversalGpuVuProduct();
				Console.Warning(
					"GPU-VU: generated transaction owner is unavailable; "
					"epochs will remain pre-effect CPU MTVU fallbacks.");
		}
		else
		{
			// Do not register the same large GXP twice. The old one-shot owner has a
			// single-draw, in-place buffer contract and cannot validate the product's
			// source/destination continuation ABI. Product completion records and
			// PCSX2 differentials now own this gate.
			gpu_vu_universal_validation_stage =
				UniversalGpuVuValidationStage::Passed;
			Console.WriteLn(
				"GPU-VU: source/destination generation owner enabled; legacy "
				"single-draw fixture bypassed.");
		}
	#endif
	ReportGpuVuMemoryPhase("universal-owner-ready");
	result = sceGxmDepthStencilSurfaceInitDisabled(&disabled_depth);
	if (result < 0)
		return Fail("sceGxmDepthStencilSurfaceInitDisabled", result);
	result = display.Initialize();
	if (result < 0)
		return Fail("display initialization", result);

	int texture_error = 0;
	white_texture = VitaGXM::GSTextureGXM::Create(this, GSTexture::Texture,
		1, 1, 1, GSTexture::Format::Color, &texture_error);
	const u32 white = 0xffffffffu;
	if (!white_texture || !white_texture->Update(GSVector4i(0, 0, 1, 1),
			&white, sizeof(white)))
	{
		return Fail("create fallback white texture",
			texture_error < 0 ? texture_error : SCE_GXM_ERROR_INVALID_VALUE);
	}
	// Base-renderer allocations are mandatory. The optional raw-input ring is
	// deliberately last so a constrained Vita falls back to inline MTVU
	// payloads instead of starving the renderer during startup.
	if (!gpu_vu_input_ring.Initialize())
		Console.Warning("GPU-VU: immutable VIF input ring is unavailable.");
	ReportGpuVuMemoryPhase("vif-input-ring-attempted");
	ready = true;
#if defined(VITASX2_GXM_GS_BLEND_VALIDATION)
	if (!ValidateGsBlend())
		return Fail("native GS blend validation", SCE_GXM_ERROR_INVALID_VALUE);
#endif
#if defined(VITASX2_GXM_SCISSOR_VALIDATION)
	if (!ValidateScissorMaskStorage())
		return Fail("persistent scissor mask validation", SCE_GXM_ERROR_INVALID_VALUE);
#endif
#if defined(VITASX2_GXM_TFX_ALPHA_VALIDATION)
	if (!ValidateTfxAlphaSpecialization())
		return Fail("TFX alpha specialization validation", SCE_GXM_ERROR_INVALID_VALUE);
#endif
#if defined(VITASX2_GXM_ATTACHMENT_CLEAR_VALIDATION)
	if (!ValidateAttachmentClears())
		return Fail("attachment clear validation", SCE_GXM_ERROR_INVALID_VALUE);
#endif
	Console.WriteLn("GXM GS: direct GSRendererHW backend ready (960x544 display).");
	return true;
}

bool GSDeviceGXM::Impl::CreatePrograms()
{
	const SceGxmProgram* const tfx_v = &_binary_vitasx2_tfx_v_gxp_start;
	const SceGxmProgram* const tfx_f = &_binary_vitasx2_tfx_f_gxp_start;
	const SceGxmProgram* const tfx_manual_lod_f =
		&_binary_vitasx2_tfx_manual_lod_f_gxp_start;
	const SceGxmProgram* const tfx_manual_lod_zfloor_f =
		&_binary_vitasx2_tfx_manual_lod_zfloor_f_gxp_start;
	const SceGxmProgram* const tfx_zfloor_f =
		&_binary_vitasx2_tfx_zfloor_f_gxp_start;
	const SceGxmProgram* const tfx_psm16_f =
		&_binary_vitasx2_tfx_psm16_f_gxp_start;
	const SceGxmProgram* const tfx_psm16_zfloor_f =
		&_binary_vitasx2_tfx_psm16_zfloor_f_gxp_start;
	const SceGxmProgram* const tfx_zfloor_source_direct_decal_af_f =
		&_binary_vitasx2_tfx_zfloor_source_direct_decal_af_f_gxp_start;
	const SceGxmProgram* const tfx_fast_f =
		&_binary_vitasx2_tfx_fast_f_gxp_start;
	const SceGxmProgram* const tfx_fast_no_atst_f =
		&_binary_vitasx2_tfx_fast_no_atst_f_gxp_start;
	const SceGxmProgram* const tfx_uv_no_fog_f =
		&_binary_vitasx2_tfx_uv_no_fog_f_gxp_start;
	const SceGxmProgram* const tfx_uv_no_fog_fast_f =
		&_binary_vitasx2_tfx_uv_no_fog_fast_f_gxp_start;
	const SceGxmProgram* const tfx_uv_no_fog_zfloor_f =
		&_binary_vitasx2_tfx_uv_no_fog_zfloor_f_gxp_start;
	const SceGxmProgram* const tfx_region_repeat_f =
		&_binary_vitasx2_tfx_region_repeat_f_gxp_start;
	const SceGxmProgram* const tfx_region_repeat_fast_f =
		&_binary_vitasx2_tfx_region_repeat_fast_f_gxp_start;
	const SceGxmProgram* const tfx_untextured_f =
		&_binary_vitasx2_tfx_untextured_f_gxp_start;
	const SceGxmProgram* const tfx_source_f =
		&_binary_vitasx2_tfx_source_f_gxp_start;
	const SceGxmProgram* const tfx_programmable_add_f =
		&_binary_vitasx2_tfx_programmable_add_f_gxp_start;
	const SceGxmProgram* const tfx_programmable_add_direct_f =
		&_binary_vitasx2_tfx_programmable_add_direct_f_gxp_start;
	const SceGxmProgram* const tfx_programmable_over_f =
		&_binary_vitasx2_tfx_programmable_over_f_gxp_start;
	const SceGxmProgram* const tfx_source_direct_f =
		&_binary_vitasx2_tfx_source_direct_f_gxp_start;
	const SceGxmProgram* const tfx_source_direct_modulate_f =
		&_binary_vitasx2_tfx_source_direct_modulate_f_gxp_start;
	const SceGxmProgram* const tfx_source_direct_modulate_af_f =
		&_binary_vitasx2_tfx_source_direct_modulate_af_f_gxp_start;
	const SceGxmProgram* const tfx_source_untextured_f =
		&_binary_vitasx2_tfx_source_untextured_f_gxp_start;
	const SceGxmProgram* const present_v = &_binary_vitasx2_present_v_gxp_start;
	const SceGxmProgram* const present_f = &_binary_vitasx2_present_f_gxp_start;
	const SceGxmProgram* const merge_f = &_binary_vitasx2_merge_f_gxp_start;
	const SceGxmProgram* const copy_f = &_binary_vitasx2_copy_f_gxp_start;
	const SceGxmProgram* const rta_correction_f =
		&_binary_vitasx2_rta_correction_f_gxp_start;
	const SceGxmProgram* const rta_decorrection_f =
		&_binary_vitasx2_rta_decorrection_f_gxp_start;
	const SceGxmProgram* const color_v = &_binary_vitasx2_color_v_gxp_start;
	const SceGxmProgram* const color_f = &_binary_vitasx2_color_f_gxp_start;
	const SceGxmProgram* const mad_buffer_f =
		&_binary_vitasx2_mad_buffer_f_gxp_start;
	const SceGxmProgram* const mad_reconstruct_f =
		&_binary_vitasx2_mad_reconstruct_f_gxp_start;
	for (const SceGxmProgram* program : {tfx_v, tfx_f, tfx_manual_lod_f,
		tfx_manual_lod_zfloor_f, tfx_zfloor_f,
		tfx_psm16_f, tfx_psm16_zfloor_f,
		tfx_zfloor_source_direct_decal_af_f,
		tfx_fast_f, tfx_fast_no_atst_f, tfx_uv_no_fog_f, tfx_uv_no_fog_fast_f,
		tfx_uv_no_fog_zfloor_f,
		tfx_region_repeat_f, tfx_region_repeat_fast_f,
		tfx_untextured_f, tfx_source_f, tfx_programmable_add_f,
		tfx_programmable_add_direct_f,
		tfx_programmable_over_f,
		tfx_source_direct_f,
		tfx_source_direct_modulate_f,
		tfx_source_direct_modulate_af_f,
		tfx_source_untextured_f,
		present_v, present_f,
		merge_f, copy_f, rta_correction_f, rta_decorrection_f, color_v,
		color_f, mad_buffer_f, mad_reconstruct_f})
	{
		const int result = sceGxmProgramCheck(program);
		if (result < 0)
			return Fail("validate embedded GXP header/version", result);
	}

	LogTfxProgramIdentities();
	const auto register_program = [this](const SceGxmProgram* program,
		SceGxmShaderPatcherId* id, const char* name) {
		const int result = sceGxmShaderPatcherRegisterProgram(patcher, program, id);
		return result >= 0 ? true : Fail(name, result);
	};
	if (!register_program(tfx_v, &tfx_vertex_id, "register TFX vertex program") ||
		!register_program(tfx_f, &tfx_fragment_id, "register TFX fragment program") ||
		!register_program(tfx_manual_lod_f, &tfx_manual_lod_fragment_id,
			"register manual-LOD TFX fragment program") ||
		!register_program(tfx_manual_lod_zfloor_f,
			&tfx_manual_lod_zfloor_fragment_id,
			"register manual-LOD Z-floor TFX fragment program") ||
		!register_program(tfx_zfloor_f, &tfx_zfloor_fragment_id,
			"register Z-floor TFX fragment program") ||
		!register_program(tfx_psm16_f, &tfx_psm16_fragment_id,
			"register PSMCT16 TFX fragment program") ||
		!register_program(tfx_psm16_zfloor_f,
			&tfx_psm16_zfloor_fragment_id,
			"register PSMCT16 Z-floor TFX fragment program") ||
		!register_program(tfx_zfloor_source_direct_decal_af_f,
			&tfx_zfloor_source_direct_decal_af_fragment_id,
			"register Z-floor direct DECAL/RGB (Cs-0)*Af+0 TFX fragment program") ||
		!register_program(tfx_fast_f, &tfx_fast_fragment_id,
			"register fast TFX fragment program") ||
		!register_program(tfx_fast_no_atst_f, &tfx_fast_no_atst_fragment_id,
			"register no-alpha-test fast TFX fragment program") ||
		!register_program(tfx_uv_no_fog_f, &tfx_uv_no_fog_fragment_id,
			"register fixed-UV/no-fog TFX fragment program") ||
		!register_program(tfx_uv_no_fog_fast_f,
			&tfx_uv_no_fog_fast_fragment_id,
			"register fast fixed-UV/no-fog TFX fragment program") ||
		!register_program(tfx_uv_no_fog_zfloor_f,
			&tfx_uv_no_fog_zfloor_fragment_id,
			"register Z-floor fixed-UV/no-fog TFX fragment program") ||
		!register_program(tfx_region_repeat_f, &tfx_region_repeat_fragment_id,
			"register REGION_REPEAT TFX fragment program") ||
		!register_program(tfx_region_repeat_fast_f,
			&tfx_region_repeat_fast_fragment_id,
			"register fast REGION_REPEAT TFX fragment program") ||
		!register_program(tfx_untextured_f, &tfx_untextured_fragment_id,
			"register untextured TFX fragment program") ||
		!register_program(tfx_source_f, &tfx_source_fragment_id,
			"register source-only TFX fragment program") ||
		!register_program(tfx_programmable_add_f,
			&tfx_programmable_add_fragment_id,
			"register programmable additive TFX fragment program") ||
		!register_program(tfx_programmable_add_direct_f,
			&tfx_programmable_add_direct_fragment_id,
			"register direct-texture programmable additive TFX fragment program") ||
		!register_program(tfx_programmable_over_f,
			&tfx_programmable_over_fragment_id,
			"register programmable source-alpha TFX fragment program") ||
		!register_program(tfx_source_direct_f, &tfx_source_direct_fragment_id,
			"register source-only direct-texture TFX fragment program") ||
		!register_program(tfx_source_direct_modulate_f,
			&tfx_source_direct_modulate_fragment_id,
			"register source-only direct MODULATE/RGBA TFX fragment program") ||
		!register_program(tfx_source_direct_modulate_af_f,
			&tfx_source_direct_modulate_af_fragment_id,
			"register source-only direct MODULATE/RGBA (Cs-0)*Af+0 TFX fragment program") ||
		!register_program(tfx_source_untextured_f,
			&tfx_source_untextured_fragment_id,
			"register source-only untextured TFX fragment program") ||
		!register_program(present_v, &present_vertex_id, "register present vertex program") ||
		!register_program(present_f, &present_fragment_id, "register present fragment program") ||
		!register_program(merge_f, &merge_fragment_id, "register merge fragment program") ||
		!register_program(copy_f, &copy_fragment_id, "register copy fragment program") ||
		!register_program(rta_correction_f, &rta_correction_fragment_id,
			"register RTA correction fragment program") ||
		!register_program(rta_decorrection_f, &rta_decorrection_fragment_id,
			"register RTA decorrection fragment program") ||
		!register_program(color_v, &color_vertex_id, "register color vertex program") ||
		!register_program(color_f, &color_fragment_id, "register color fragment program") ||
		!register_program(mad_buffer_f, &mad_buffer_fragment_id, "register MAD buffer program") ||
		!register_program(mad_reconstruct_f, &mad_reconstruct_fragment_id,
			"register MAD reconstruct program"))
	{
		return false;
	}

	const auto parameter = [this](const SceGxmProgram* program, const char* name,
		SceGxmParameterCategory category) -> const SceGxmProgramParameter* {
		const SceGxmProgramParameter* value =
			sceGxmProgramFindParameterByName(program, name);
		if (!value || sceGxmProgramParameterGetCategory(value) != category)
		{
			Fail(name, SCE_GXM_ERROR_INVALID_VALUE);
			return nullptr;
		}
		return value;
	};

	const std::array<const char*, 7> selector_names = {
		"Selector0", "Selector1", "Selector2", "Selector3", "Selector4",
		"Selector5", "Selector6"};
	uniforms.vertex_scale_offset = parameter(tfx_v, "VertexScaleOffset",
		SCE_GXM_PARAMETER_CATEGORY_UNIFORM);
	const auto find_uniform = [](const SceGxmProgram* program,
		const char* name) {
		const SceGxmProgramParameter* value =
			sceGxmProgramFindParameterByName(program, name);
		return (value && sceGxmProgramParameterGetCategory(value) ==
			SCE_GXM_PARAMETER_CATEGORY_UNIFORM) ? value : nullptr;
	};
	const auto load_variant_uniforms = [&find_uniform, &selector_names](
		const SceGxmProgram* program, ProgramUniforms* destination) {
		for (u32 i = 0; i < selector_names.size(); i++)
			destination->selector[i] = find_uniform(program, selector_names[i]);
		destination->fog_color_aref = find_uniform(program, "FogColorAref");
		destination->texture_size = find_uniform(program, "TextureSize");
		destination->native_texture_size = find_uniform(program, "NativeTextureSize");
		destination->texture_alpha = find_uniform(program, "TextureAlpha");
		destination->half_texel = find_uniform(program, "HalfTexel");
		destination->st_scale = find_uniform(program, "STScale");
		destination->st_range = find_uniform(program, "STRange");
		destination->fb_mask = find_uniform(program, "FbMask");
		destination->hardware_blend[0] = find_uniform(program, "HardwareBlend0");
		destination->hardware_blend[1] = find_uniform(program, "HardwareBlend1");
		destination->color_mask = find_uniform(program, "ColorMask");
		destination->region_mask_fix = find_uniform(program, "RegionMaskFix");
		destination->lod_params = find_uniform(program, "LODParams");
		destination->selector7 = find_uniform(program, "Selector7");
		destination->dither_matrix[0] = find_uniform(program, "DitherMatrix0");
		destination->dither_matrix[1] = find_uniform(program, "DitherMatrix1");
		destination->dither_matrix[2] = find_uniform(program, "DitherMatrix2");
		destination->dither_matrix[3] = find_uniform(program, "DitherMatrix3");
	};
	for (u32 i = 0; i < selector_names.size(); i++)
	{
		uniforms.selector[i] = parameter(tfx_f, selector_names[i],
			SCE_GXM_PARAMETER_CATEGORY_UNIFORM);
	}
	load_variant_uniforms(tfx_manual_lod_f, &manual_lod_uniforms);
	load_variant_uniforms(tfx_manual_lod_zfloor_f,
		&manual_lod_zfloor_uniforms);
	if (!manual_lod_uniforms.lod_params ||
		!manual_lod_zfloor_uniforms.lod_params)
	{
		return Fail("validate manual-LOD TFX uniforms",
			SCE_GXM_ERROR_INVALID_VALUE);
	}
	load_variant_uniforms(tfx_zfloor_f, &zfloor_uniforms);
	load_variant_uniforms(tfx_psm16_f, &psm16_uniforms);
	load_variant_uniforms(tfx_psm16_zfloor_f, &psm16_zfloor_uniforms);
	if (!psm16_uniforms.selector7 || !psm16_zfloor_uniforms.selector7)
		return Fail("validate PSMCT16 TFX selector", SCE_GXM_ERROR_INVALID_VALUE);
	for (u32 i = 0; i < 4; i++)
	{
		if (!psm16_uniforms.dither_matrix[i] ||
			!psm16_zfloor_uniforms.dither_matrix[i])
		{
			return Fail("validate PSMCT16 TFX dither matrix",
				SCE_GXM_ERROR_INVALID_VALUE);
		}
	}
	load_variant_uniforms(tfx_zfloor_source_direct_decal_af_f,
		&zfloor_source_direct_decal_af_uniforms);
	if (!zfloor_source_direct_decal_af_uniforms.texture_alpha ||
		!zfloor_source_direct_decal_af_uniforms.st_scale ||
		!zfloor_source_direct_decal_af_uniforms.hardware_blend[1])
	{
		return Fail("validate Z-floor direct constant-blend uniforms",
			SCE_GXM_ERROR_INVALID_VALUE);
	}
	load_variant_uniforms(tfx_fast_f, &fast_uniforms);
	load_variant_uniforms(tfx_fast_no_atst_f, &fast_no_atst_uniforms);
#if defined(VITASX2_GXM_GS_BLEND) || defined(VITASX2_GXM_GS_BLEND_VALIDATION)
	const std::array<const SceGxmProgram*, GS_BLEND_PROGRAM_COUNT> gs_blend_sources = {
		&_binary_vitasx2_tfx_gs_blend_f_gxp_start,
		&_binary_vitasx2_tfx_gs_blend_zfloor_f_gxp_start,
		&_binary_vitasx2_tfx_gs_blend_no_atst_f_gxp_start,
		&_binary_vitasx2_tfx_gs_blend_no_atst_zfloor_f_gxp_start,
		&_binary_vitasx2_tfx_gs_blend_source_f_gxp_start,
		&_binary_vitasx2_tfx_gs_blend_source_zfloor_f_gxp_start,
			&_binary_vitasx2_tfx_gs_blend_source_no_atst_f_gxp_start,
			&_binary_vitasx2_tfx_gs_blend_source_no_atst_zfloor_f_gxp_start,
			&_binary_vitasx2_tfx_gs_blend_direct_modulate_stq_f_gxp_start,
			&_binary_vitasx2_tfx_gs_blend_direct_modulate_stq_zfloor_f_gxp_start};
	for (u32 i = 0; i < gs_blend_sources.size(); i++)
	{
		if (sceGxmProgramCheck(gs_blend_sources[i]) < 0 ||
			!register_program(gs_blend_sources[i], &gs_blend_ids[i],
				"register native GS blend program"))
			return false;
		load_variant_uniforms(gs_blend_sources[i], &gs_blend_uniforms[i]);
		// The native path must not accidentally retain desktop blend factors.
		if (gs_blend_uniforms[i].hardware_blend[0] || gs_blend_uniforms[i].hardware_blend[1])
			return Fail("native GS program retained desktop blending", SCE_GXM_ERROR_INVALID_VALUE);
		const bool expected_discard = i < GS_BLEND_CAPABILITY_PROGRAM_COUNT ?
			!(i & 2u) : false;
		const bool expected_depth_replace = i < GS_BLEND_CAPABILITY_PROGRAM_COUNT ?
			!!(i & 1u) : i == GS_BLEND_DIRECT_MODULATE_STQ_ZFLOOR;
		if (!!sceGxmProgramIsDiscardUsed(gs_blend_sources[i]) != expected_discard ||
			!!sceGxmProgramIsDepthReplaceUsed(gs_blend_sources[i]) != expected_depth_replace)
			return Fail("native GS program capability mismatch", SCE_GXM_ERROR_INVALID_VALUE);
	}
#endif
	load_variant_uniforms(tfx_uv_no_fog_f, &uv_no_fog_uniforms);
	load_variant_uniforms(tfx_uv_no_fog_fast_f,
		&uv_no_fog_fast_uniforms);
	load_variant_uniforms(tfx_uv_no_fog_zfloor_f,
		&uv_no_fog_zfloor_uniforms);
	load_variant_uniforms(tfx_region_repeat_f, &region_repeat_uniforms);
	load_variant_uniforms(tfx_region_repeat_fast_f,
		&region_repeat_fast_uniforms);
	if (!region_repeat_uniforms.region_mask_fix ||
		!region_repeat_fast_uniforms.region_mask_fix)
	{
		return Fail("validate REGION_REPEAT TFX uniforms",
			SCE_GXM_ERROR_INVALID_VALUE);
	}
	load_variant_uniforms(tfx_untextured_f, &untextured_uniforms);
	load_variant_uniforms(tfx_source_f, &source_uniforms);
	load_variant_uniforms(tfx_programmable_add_f, &programmable_add_uniforms);
	load_variant_uniforms(tfx_programmable_add_direct_f,
		&programmable_add_direct_uniforms);
	load_variant_uniforms(tfx_programmable_over_f, &programmable_over_uniforms);
	load_variant_uniforms(tfx_source_direct_f, &source_direct_uniforms);
	load_variant_uniforms(tfx_source_direct_modulate_f,
		&source_direct_modulate_uniforms);
	load_variant_uniforms(tfx_source_direct_modulate_af_f,
		&source_direct_modulate_af_uniforms);
	load_variant_uniforms(tfx_source_untextured_f,
		&source_untextured_uniforms);
	uniforms.fog_color_aref = parameter(tfx_f, "FogColorAref",
		SCE_GXM_PARAMETER_CATEGORY_UNIFORM);
	uniforms.texture_size = parameter(tfx_f, "TextureSize",
		SCE_GXM_PARAMETER_CATEGORY_UNIFORM);
	uniforms.native_texture_size = parameter(tfx_f, "NativeTextureSize",
		SCE_GXM_PARAMETER_CATEGORY_UNIFORM);
	uniforms.texture_alpha = parameter(tfx_f, "TextureAlpha",
		SCE_GXM_PARAMETER_CATEGORY_UNIFORM);
	uniforms.half_texel = parameter(tfx_f, "HalfTexel",
		SCE_GXM_PARAMETER_CATEGORY_UNIFORM);
	uniforms.st_scale = parameter(tfx_f, "STScale",
		SCE_GXM_PARAMETER_CATEGORY_UNIFORM);
	uniforms.st_range = parameter(tfx_f, "STRange",
		SCE_GXM_PARAMETER_CATEGORY_UNIFORM);
	uniforms.fb_mask = parameter(tfx_f, "FbMask",
		SCE_GXM_PARAMETER_CATEGORY_UNIFORM);
	uniforms.hardware_blend[0] = parameter(tfx_f, "HardwareBlend0",
		SCE_GXM_PARAMETER_CATEGORY_UNIFORM);
	uniforms.hardware_blend[1] = parameter(tfx_f, "HardwareBlend1",
		SCE_GXM_PARAMETER_CATEGORY_UNIFORM);
	uniforms.color_mask = parameter(tfx_f, "ColorMask",
		SCE_GXM_PARAMETER_CATEGORY_UNIFORM);
	uniforms.interlace[0] = parameter(mad_buffer_f, "ZrH",
		SCE_GXM_PARAMETER_CATEGORY_UNIFORM);
	uniforms.interlace[1] = parameter(mad_reconstruct_f, "ZrH",
		SCE_GXM_PARAMETER_CATEGORY_UNIFORM);
	if (!uniforms.vertex_scale_offset || !uniforms.fog_color_aref ||
		!uniforms.texture_size || !uniforms.native_texture_size ||
		!uniforms.texture_alpha || !uniforms.half_texel || !uniforms.st_scale ||
		!uniforms.st_range || !uniforms.fb_mask || !uniforms.hardware_blend[0] ||
		!uniforms.hardware_blend[1] || !uniforms.color_mask ||
		!uniforms.interlace[0] || !uniforms.interlace[1])
	{
		return false;
	}
	for (const SceGxmProgramParameter* selector : uniforms.selector)
	{
		if (!selector)
			return false;
	}

	const std::array<const char*, 7> tfx_attribute_names = {
		"aST", "aColor", "aQ", "aPosition", "aDepth", "aUV", "aFog"};
	const std::array<u16, 7> tfx_attribute_offsets = {
		offsetof(TfxVertex, st), offsetof(TfxVertex, rgba), offsetof(TfxVertex, q),
		offsetof(TfxVertex, position), offsetof(TfxVertex, depth),
		offsetof(TfxVertex, uv), offsetof(TfxVertex, fog)};
	const std::array<SceGxmAttributeFormat, 7> tfx_attribute_formats = {
		SCE_GXM_ATTRIBUTE_FORMAT_F32, SCE_GXM_ATTRIBUTE_FORMAT_U8,
		SCE_GXM_ATTRIBUTE_FORMAT_F32, SCE_GXM_ATTRIBUTE_FORMAT_F32,
		SCE_GXM_ATTRIBUTE_FORMAT_F32, SCE_GXM_ATTRIBUTE_FORMAT_F32,
		SCE_GXM_ATTRIBUTE_FORMAT_U8N};
	const std::array<u8, 7> tfx_attribute_components = {2, 4, 1, 2, 1, 2, 4};
	std::array<SceGxmVertexAttribute, 7> tfx_attributes{};
	for (u32 i = 0; i < tfx_attributes.size(); i++)
	{
		const SceGxmProgramParameter* attr = parameter(tfx_v,
			tfx_attribute_names[i], SCE_GXM_PARAMETER_CATEGORY_ATTRIBUTE);
		if (!attr)
			return false;
		tfx_attributes[i].streamIndex = 0;
		tfx_attributes[i].offset = tfx_attribute_offsets[i];
		tfx_attributes[i].format = tfx_attribute_formats[i];
		tfx_attributes[i].componentCount = tfx_attribute_components[i];
		tfx_attributes[i].regIndex = sceGxmProgramParameterGetResourceIndex(attr);
	}
	SceGxmVertexStream tfx_stream{};
	tfx_stream.stride = sizeof(TfxVertex);
	tfx_stream.indexSource = SCE_GXM_INDEX_SOURCE_INDEX_16BIT;
	int result = sceGxmShaderPatcherCreateVertexProgram(patcher, tfx_vertex_id,
		tfx_attributes.data(), tfx_attributes.size(), &tfx_stream, 1,
		&tfx_vertex_program);
	if (result < 0 || !tfx_vertex_program)
		return Fail("create TFX vertex program", result);

	result = sceGxmShaderPatcherCreateFragmentProgram(patcher, tfx_fragment_id,
		SCE_GXM_OUTPUT_REGISTER_FORMAT_DECLARED, SCE_GXM_MULTISAMPLE_NONE,
		nullptr, tfx_v, &tfx_fragment_program);
	if (result < 0 || !tfx_fragment_program)
		return Fail("create TFX fragment program", result);
#if defined(VITASX2_GXM_GS_BLEND) || defined(VITASX2_GXM_GS_BLEND_VALIDATION)
	for (u32 i = 0; i < gs_blend_programs.size(); i++)
	{
		result = sceGxmShaderPatcherCreateFragmentProgram(patcher, gs_blend_ids[i],
			SCE_GXM_OUTPUT_REGISTER_FORMAT_DECLARED, SCE_GXM_MULTISAMPLE_NONE,
			nullptr, tfx_v, &gs_blend_programs[i]);
		if (result < 0 || !gs_blend_programs[i])
			return Fail("create native GS blend program", result);
	}
#endif
	result = sceGxmShaderPatcherCreateFragmentProgram(patcher,
		tfx_manual_lod_fragment_id, SCE_GXM_OUTPUT_REGISTER_FORMAT_DECLARED,
		SCE_GXM_MULTISAMPLE_NONE, nullptr, tfx_v,
		&tfx_manual_lod_fragment_program);
	if (result < 0 || !tfx_manual_lod_fragment_program)
		return Fail("create manual-LOD TFX fragment program", result);
	result = sceGxmShaderPatcherCreateFragmentProgram(patcher,
		tfx_manual_lod_zfloor_fragment_id,
		SCE_GXM_OUTPUT_REGISTER_FORMAT_DECLARED, SCE_GXM_MULTISAMPLE_NONE,
		nullptr, tfx_v, &tfx_manual_lod_zfloor_fragment_program);
	if (result < 0 || !tfx_manual_lod_zfloor_fragment_program)
		return Fail("create manual-LOD Z-floor TFX fragment program", result);
	result = sceGxmShaderPatcherCreateFragmentProgram(patcher,
		tfx_zfloor_fragment_id, SCE_GXM_OUTPUT_REGISTER_FORMAT_DECLARED,
		SCE_GXM_MULTISAMPLE_NONE, nullptr, tfx_v,
		&tfx_zfloor_fragment_program);
	if (result < 0 || !tfx_zfloor_fragment_program)
		return Fail("create Z-floor TFX fragment program", result);
	result = sceGxmShaderPatcherCreateFragmentProgram(patcher,
		tfx_psm16_fragment_id, SCE_GXM_OUTPUT_REGISTER_FORMAT_DECLARED,
		SCE_GXM_MULTISAMPLE_NONE, nullptr, tfx_v,
		&tfx_psm16_fragment_program);
	if (result < 0 || !tfx_psm16_fragment_program)
		return Fail("create PSMCT16 TFX fragment program", result);
	result = sceGxmShaderPatcherCreateFragmentProgram(patcher,
		tfx_psm16_zfloor_fragment_id,
		SCE_GXM_OUTPUT_REGISTER_FORMAT_DECLARED, SCE_GXM_MULTISAMPLE_NONE,
		nullptr, tfx_v, &tfx_psm16_zfloor_fragment_program);
	if (result < 0 || !tfx_psm16_zfloor_fragment_program)
		return Fail("create PSMCT16 Z-floor TFX fragment program", result);
	// PCSX2's constant-blend split requires a destination value, but GXM has no
	// fixed constant-color blend factor. The specialized native-color program
	// performs that one proven equation through FRAGCOLOR, matching Sony's
	// programmable_blending sample without retaining the general TFX shader.
	result = sceGxmShaderPatcherCreateFragmentProgram(patcher,
		tfx_zfloor_source_direct_decal_af_fragment_id,
		SCE_GXM_OUTPUT_REGISTER_FORMAT_DECLARED, SCE_GXM_MULTISAMPLE_NONE,
		nullptr, tfx_v, &tfx_zfloor_source_direct_decal_af_program);
	if (result < 0 || !tfx_zfloor_source_direct_decal_af_program)
		return Fail("create Z-floor direct DECAL/RGB constant-blend program",
			result);

	// Full-channel opaque output requires neither FRAGCOLOR nor fixed blending.
	// Create it with the same null blend contract as Sony's non-blended samples;
	// partial masks remain on the general programmable path.
	result = sceGxmShaderPatcherCreateFragmentProgram(patcher,
		tfx_fast_fragment_id,
		SCE_GXM_OUTPUT_REGISTER_FORMAT_DECLARED, SCE_GXM_MULTISAMPLE_NONE,
		nullptr, tfx_v, &tfx_opaque_program);
	if (result < 0 || !tfx_opaque_program)
		return Fail("create opaque TFX fragment program", result);
	result = sceGxmShaderPatcherCreateFragmentProgram(patcher,
		tfx_fast_no_atst_fragment_id,
		SCE_GXM_OUTPUT_REGISTER_FORMAT_DECLARED, SCE_GXM_MULTISAMPLE_NONE,
		nullptr, tfx_v, &tfx_fast_no_atst_program);
	if (result < 0 || !tfx_fast_no_atst_program)
		return Fail("create no-alpha-test fast TFX fragment program", result);
	result = sceGxmShaderPatcherCreateFragmentProgram(patcher,
		tfx_region_repeat_fragment_id,
		SCE_GXM_OUTPUT_REGISTER_FORMAT_DECLARED, SCE_GXM_MULTISAMPLE_NONE,
		nullptr, tfx_v, &tfx_region_repeat_program);
	if (result < 0 || !tfx_region_repeat_program)
		return Fail("create REGION_REPEAT TFX fragment program", result);
	result = sceGxmShaderPatcherCreateFragmentProgram(patcher,
		tfx_region_repeat_fast_fragment_id,
		SCE_GXM_OUTPUT_REGISTER_FORMAT_DECLARED, SCE_GXM_MULTISAMPLE_NONE,
		nullptr, tfx_v, &tfx_region_repeat_opaque_program);
	if (result < 0 || !tfx_region_repeat_opaque_program)
		return Fail("create fast REGION_REPEAT TFX fragment program", result);

	// Sony's programmable- and masked-blending samples create FRAGCOLOR programs
	// without a patcher blend state. These programs preserve PCSX2's general TFX
	// path and compile out only one independently proven host blend equation.
	result = sceGxmShaderPatcherCreateFragmentProgram(patcher,
		tfx_programmable_add_fragment_id,
		SCE_GXM_OUTPUT_REGISTER_FORMAT_DECLARED, SCE_GXM_MULTISAMPLE_NONE,
		nullptr, tfx_v, &tfx_programmable_add_program);
	if (result < 0 || !tfx_programmable_add_program)
		return Fail("create programmable additive TFX fragment program", result);
	result = sceGxmShaderPatcherCreateFragmentProgram(patcher,
		tfx_programmable_add_direct_fragment_id,
		SCE_GXM_OUTPUT_REGISTER_FORMAT_DECLARED, SCE_GXM_MULTISAMPLE_NONE,
		nullptr, tfx_v, &tfx_programmable_add_direct_program);
	if (result < 0 || !tfx_programmable_add_direct_program)
		return Fail("create direct-texture programmable additive TFX fragment program", result);
	result = sceGxmShaderPatcherCreateFragmentProgram(patcher,
		tfx_programmable_over_fragment_id,
		SCE_GXM_OUTPUT_REGISTER_FORMAT_DECLARED, SCE_GXM_MULTISAMPLE_NONE,
		nullptr, tfx_v, &tfx_programmable_over_program);
	if (result < 0 || !tfx_programmable_over_program)
		return Fail("create programmable source-alpha TFX fragment program", result);

	const std::array<const char*, 3> quad_attribute_names = {
		"aPosition", "aColor", "aTexCoord"};
	const std::array<u16, 3> quad_attribute_offsets = {
		offsetof(QuadVertex, x), offsetof(QuadVertex, r), offsetof(QuadVertex, u)};
	const std::array<SceGxmAttributeFormat, 3> quad_attribute_formats = {
		SCE_GXM_ATTRIBUTE_FORMAT_F32, SCE_GXM_ATTRIBUTE_FORMAT_U8N,
		SCE_GXM_ATTRIBUTE_FORMAT_F32};
	const std::array<u8, 3> quad_attribute_components = {3, 4, 2};
	std::array<SceGxmVertexAttribute, 3> quad_attributes{};
	for (u32 i = 0; i < quad_attributes.size(); i++)
	{
		const SceGxmProgramParameter* attr = parameter(present_v,
			quad_attribute_names[i], SCE_GXM_PARAMETER_CATEGORY_ATTRIBUTE);
		if (!attr)
			return false;
		quad_attributes[i].streamIndex = 0;
		quad_attributes[i].offset = quad_attribute_offsets[i];
		quad_attributes[i].format = quad_attribute_formats[i];
		quad_attributes[i].componentCount = quad_attribute_components[i];
		quad_attributes[i].regIndex = sceGxmProgramParameterGetResourceIndex(attr);
	}
	SceGxmVertexStream quad_stream{};
	quad_stream.stride = sizeof(QuadVertex);
	quad_stream.indexSource = SCE_GXM_INDEX_SOURCE_INDEX_16BIT;
	result = sceGxmShaderPatcherCreateVertexProgram(patcher, present_vertex_id,
		quad_attributes.data(), quad_attributes.size(), &quad_stream, 1,
		&present_vertex_program);
	if (result < 0 || !present_vertex_program)
		return Fail("create present vertex program", result);

	SceGxmBlendInfo opaque{};
	opaque.colorFunc = SCE_GXM_BLEND_FUNC_NONE;
	opaque.alphaFunc = SCE_GXM_BLEND_FUNC_NONE;
	opaque.colorSrc = SCE_GXM_BLEND_FACTOR_ZERO;
	opaque.colorDst = SCE_GXM_BLEND_FACTOR_ZERO;
	opaque.alphaSrc = SCE_GXM_BLEND_FACTOR_ZERO;
	opaque.alphaDst = SCE_GXM_BLEND_FACTOR_ZERO;
	opaque.colorMask = SCE_GXM_COLOR_MASK_ALL;
	const auto create_fragment = [this, present_v](SceGxmShaderPatcherId id,
		const SceGxmBlendInfo* blend, SceGxmFragmentProgram** out,
		const char* name) {
		const int create_result = sceGxmShaderPatcherCreateFragmentProgram(patcher,
			id, SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4, SCE_GXM_MULTISAMPLE_NONE,
			blend, present_v, out);
		return (create_result >= 0 && *out) ? true : Fail(name, create_result);
	};
	for (u32 mask = 0; mask < copy_programs.size(); mask++)
	{
		SceGxmBlendInfo masked_copy = opaque;
		masked_copy.colorMask = TranslateColorMask(static_cast<u8>(mask));
		if (!create_fragment(copy_fragment_id, &masked_copy,
				&copy_programs[mask], "create masked copy program") ||
			!create_fragment(rta_correction_fragment_id, &masked_copy,
				&rta_correction_programs[mask],
				"create masked RTA-correction program"))
		{
			return false;
		}
	}
	if (!create_fragment(rta_decorrection_fragment_id, &opaque,
			&rta_decorrection_program, "create RTA-decorrection program"))
	{
		return false;
	}

	SceGxmBlendInfo source_over{};
	source_over.colorFunc = SCE_GXM_BLEND_FUNC_ADD;
	source_over.alphaFunc = SCE_GXM_BLEND_FUNC_ADD;
	source_over.colorSrc = SCE_GXM_BLEND_FACTOR_SRC_ALPHA;
	source_over.colorDst = SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	source_over.alphaSrc = SCE_GXM_BLEND_FACTOR_ONE;
	source_over.alphaDst = SCE_GXM_BLEND_FACTOR_ZERO;
	source_over.colorMask = SCE_GXM_COLOR_MASK_ALL;
	if (!create_fragment(present_fragment_id, &source_over, &present_alpha_program,
			"create constant-alpha merge program") ||
		!create_fragment(merge_fragment_id, &source_over,
			&present_source_alpha_program, "create source-alpha merge program"))
	{
		return false;
	}
	SceGxmBlendInfo source_over_rgb = source_over;
	source_over_rgb.colorMask = SCE_GXM_COLOR_MASK_R | SCE_GXM_COLOR_MASK_G |
		SCE_GXM_COLOR_MASK_B;
	if (!create_fragment(present_fragment_id, &source_over_rgb,
			&present_alpha_rgb_program, "create RGB-only constant-alpha merge program") ||
		!create_fragment(merge_fragment_id, &source_over_rgb,
			&present_source_alpha_rgb_program, "create RGB-only source-alpha merge program") ||
		!create_fragment(mad_buffer_fragment_id, &opaque, &mad_buffer_program,
			"create MAD-buffer program") ||
		!create_fragment(mad_reconstruct_fragment_id, &opaque,
			&mad_reconstruct_program, "create MAD-reconstruct program"))
	{
		return false;
	}

	const SceGxmProgramParameter* color_position = parameter(color_v, "aPosition",
		SCE_GXM_PARAMETER_CATEGORY_ATTRIBUTE);
	const SceGxmProgramParameter* color_color = parameter(color_v, "aColor",
		SCE_GXM_PARAMETER_CATEGORY_ATTRIBUTE);
	if (!color_position || !color_color)
		return false;
	std::array<SceGxmVertexAttribute, 2> color_attributes{};
	color_attributes[0] = quad_attributes[0];
	color_attributes[0].regIndex = sceGxmProgramParameterGetResourceIndex(color_position);
	color_attributes[1] = quad_attributes[1];
	color_attributes[1].regIndex = sceGxmProgramParameterGetResourceIndex(color_color);
	result = sceGxmShaderPatcherCreateVertexProgram(patcher, color_vertex_id,
		color_attributes.data(), color_attributes.size(), &quad_stream, 1,
		&color_vertex_program);
	if (result < 0 || !color_vertex_program)
		return Fail("create color vertex program", result);
	result = sceGxmShaderPatcherCreateFragmentProgram(patcher, color_fragment_id,
		SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4, SCE_GXM_MULTISAMPLE_NONE,
		&opaque, color_v, &color_fragment_program);
	if (result < 0 || !color_fragment_program)
		return Fail("create color fragment program", result);
	result = sceGxmShaderPatcherCreateMaskUpdateFragmentProgram(patcher,
		&mask_update_program);
	return (result >= 0 && mask_update_program) ? true :
		Fail("create mask-update fragment program", result);
}

#if defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
bool GSDeviceGXM::Impl::InitializeUniversalGpuVuValidation()
{
	if (!patcher || gpu_vu_universal_validation_stage !=
			UniversalGpuVuValidationStage::Uninitialized)
	{
		return false;
	}

	int result = VitaGXM::AllocateMappedBlock(
		"VitaSX2 universal GPU-VU state",
		SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_NC_RW,
		GPU_VU_UNIVERSAL_BUFFER_BYTES, SCE_GXM_MEMORY_ATTRIB_RW,
		&gpu_vu_universal_validation_buffer);
	if (result < 0)
		return Fail("allocate universal GPU-VU state", result);
	std::memset(gpu_vu_universal_validation_buffer.base, 0,
		gpu_vu_universal_validation_buffer.size);

	const SceUID gxp_fd = sceIoOpen(GPU_VU_UNIVERSAL_GXP_PATH,
		SCE_O_RDONLY, 0);
	if (gxp_fd < 0)
		return Fail("open universal GPU-VU GXP asset", gxp_fd);
	const SceOff gxp_size = sceIoLseek(gxp_fd, 0, SCE_SEEK_END);
	if (gxp_size <= 0 ||
		static_cast<u64>(gxp_size) > GPU_VU_UNIVERSAL_GXP_MAX_BYTES ||
		sceIoLseek(gxp_fd, 0, SCE_SEEK_SET) < 0)
	{
		sceIoClose(gxp_fd);
		return Fail("validate universal GPU-VU GXP asset",
			SCE_GXM_ERROR_INVALID_VALUE);
	}
	u8* const gxp_bytes = static_cast<u8*>(
		gpu_vu_universal_validation_buffer.base) + GPU_VU_UNIVERSAL_GXP_OFFSET;
	SceOff gxp_read = 0;
	while (gxp_read < gxp_size)
	{
		const SceSSize read_result = sceIoRead(gxp_fd, gxp_bytes + gxp_read,
			static_cast<SceSize>(gxp_size - gxp_read));
		if (read_result <= 0)
		{
			sceIoClose(gxp_fd);
			return Fail("read universal GPU-VU GXP asset",
				read_result < 0 ? static_cast<int>(read_result) :
					SCE_GXM_ERROR_INVALID_VALUE);
		}
		gxp_read += read_result;
	}
	const int gxp_close_result = sceIoClose(gxp_fd);
	if (gxp_close_result < 0)
		return Fail("close universal GPU-VU GXP asset", gxp_close_result);

	const SceGxmProgram* const vertex =
		reinterpret_cast<const SceGxmProgram*>(gxp_bytes);
	const SceGxmProgram* const fragment =
		&_binary_vitasx2_gpu_vu_universal_f_gxp_start;
	// The SDK contract checks GXP header/version compatibility only. Immutable
	// host preflight, private generations, and physical output attestation own
	// semantic, range, disjoint-write, and bounded-execution safety.
	result = sceGxmProgramCheck(vertex);
	if (result < 0)
		return Fail("validate universal GPU-VU vertex GXP header/version", result);
	u32 vertex_outputs = 0u;
	if (!GpuVuComputeVertexOutputsAreValid(vertex, &vertex_outputs))
	{
		Console.Error(
			"GPU-VU: universal validation GXP has invalid POINT output "
			"contract (outputs=%08x required=%08x).",
			vertex_outputs, GPU_VU_COMPUTE_REQUIRED_VERTEX_OUTPUTS);
		return Fail("validate universal GPU-VU POINT outputs",
			SCE_GXM_ERROR_INVALID_VALUE);
	}
	result = sceGxmProgramCheck(fragment);
	if (result < 0)
		return Fail("validate universal GPU-VU fragment GXP header/version", result);
	result = sceGxmShaderPatcherRegisterProgram(patcher, vertex,
		&gpu_vu_universal_vertex_id);
	if (result < 0 || !gpu_vu_universal_vertex_id)
	{
		return Fail("register universal GPU-VU vertex program",
			result < 0 ? result : SCE_GXM_ERROR_INVALID_POINTER);
	}
	result = sceGxmShaderPatcherRegisterProgram(patcher, fragment,
		&gpu_vu_universal_fragment_id);
	if (result < 0 || !gpu_vu_universal_fragment_id)
	{
		return Fail("register universal GPU-VU fragment program",
			result < 0 ? result : SCE_GXM_ERROR_INVALID_POINTER);
	}
	result = sceGxmShaderPatcherCreateVertexProgram(patcher,
		gpu_vu_universal_vertex_id, nullptr, 0, nullptr, 0,
		&gpu_vu_universal_vertex_program);
	if (result < 0 || !gpu_vu_universal_vertex_program)
	{
		return Fail("create universal GPU-VU vertex program",
			result < 0 ? result : SCE_GXM_ERROR_INVALID_POINTER);
	}
	SceGxmBlendInfo no_color{};
	no_color.colorFunc = SCE_GXM_BLEND_FUNC_NONE;
	no_color.alphaFunc = SCE_GXM_BLEND_FUNC_NONE;
	no_color.colorSrc = SCE_GXM_BLEND_FACTOR_ZERO;
	no_color.colorDst = SCE_GXM_BLEND_FACTOR_ZERO;
	no_color.alphaSrc = SCE_GXM_BLEND_FACTOR_ZERO;
	no_color.alphaDst = SCE_GXM_BLEND_FACTOR_ZERO;
	no_color.colorMask = SCE_GXM_COLOR_MASK_NONE;
	result = sceGxmShaderPatcherCreateFragmentProgram(patcher,
		gpu_vu_universal_fragment_id,
		SCE_GXM_OUTPUT_REGISTER_FORMAT_DECLARED,
		SCE_GXM_MULTISAMPLE_NONE, &no_color, vertex,
		&gpu_vu_universal_fragment_program);
	if (result < 0 || !gpu_vu_universal_fragment_program)
	{
		return Fail("create universal GPU-VU fragment program",
			result < 0 ? result : SCE_GXM_ERROR_INVALID_POINTER);
	}

	std::vector<u8> source(VitaGpuVu::UniversalMicroProgramSourceBytes);
	for (u32 pair = 0;
		pair < VitaGpuVu::UniversalMicroProgramPairCount; pair++)
	{
		std::memcpy(source.data() + pair * sizeof(RawVuPair),
			&UNIVERSAL_VALIDATION_NOP, sizeof(RawVuPair));
	}
	const std::array<RawVuPair, UNIVERSAL_VALIDATION_SEMANTIC_PAIR_COUNT>
		semantic_program = {{
		{UniversalValidationLowerImm15(0x08, 1, 0, 100),
			UNIVERSAL_VALIDATION_NOP.upper},
		{UniversalValidationLowerMemory(0x00, 0x0f, 1, 1, 0),
			UNIVERSAL_VALIDATION_NOP.upper},
		{UniversalValidationLowerMemory(0x00, 0x0f, 2, 1, 1),
			UNIVERSAL_VALIDATION_NOP.upper},
		{UNIVERSAL_VALIDATION_NOP.lower,
			UniversalValidationUpper(0x28, 0x0f, 2, 1, 3)},
		{UniversalValidationLowerImm15(0x08, 2, 0, 16),
			UNIVERSAL_VALIDATION_NOP.upper},
		{UniversalValidationLowerT3(0x3c, 0x1b, 0, 0, 2),
			UNIVERSAL_VALIDATION_NOP.upper},
		{UniversalValidationLowerMemory(0x01, 0x0f, 2, 3, 1),
			UNIVERSAL_VALIDATION_NOP.upper},
		{UniversalValidationLowerImm15(0x08, 3, 0, 1022),
			UNIVERSAL_VALIDATION_NOP.upper},
		{UniversalValidationLowerT3(0x3c, 0x1b, 0, 0, 3),
			UNIVERSAL_VALIDATION_NOP.upper},
		{UniversalValidationLowerT3(0x3c, 0x1a, 0, 7, 0),
			UNIVERSAL_VALIDATION_NOP.upper},
		{UniversalValidationLowerMemory(0x00, 0x0f, 4, 1, 2),
			UNIVERSAL_VALIDATION_NOP.upper},
		{UNIVERSAL_VALIDATION_NOP.lower,
			UniversalValidationUpper(0x3c, 0x0f, 5, 4, 4)},
		{UNIVERSAL_VALIDATION_NOP.lower,
			UniversalValidationUpper(0x3d, 0x0f, 6, 4, 4)},
		{UNIVERSAL_VALIDATION_NOP.lower,
			UniversalValidationUpper(0x3e, 0x0f, 7, 4, 4)},
		{UNIVERSAL_VALIDATION_NOP.lower,
			UniversalValidationUpper(0x3f, 0x0f, 8, 4, 4)},
		{UniversalValidationLowerMemory(0x00, 0x0f, 9, 1, 3),
			UNIVERSAL_VALIDATION_NOP.upper},
		{UNIVERSAL_VALIDATION_NOP.lower,
			UniversalValidationUpper(0x3c, 0x0f, 13, 9, 5)},
		{UNIVERSAL_VALIDATION_NOP.lower,
			UniversalValidationUpper(0x3d, 0x0f, 14, 9, 5)},
		{UNIVERSAL_VALIDATION_NOP.lower,
			UniversalValidationUpper(0x3e, 0x0f, 15, 9, 5)},
		{UNIVERSAL_VALIDATION_NOP.lower,
			UniversalValidationUpper(0x3f, 0x0f, 16, 9, 5)},
		{UniversalValidationLowerMemory(0x00, 0x0f, 10, 1, 4),
			UNIVERSAL_VALIDATION_NOP.upper},
		{UNIVERSAL_VALIDATION_NOP.lower,
			UniversalValidationUpper(0x3c, 0x0f, 17, 10, 5)},
		{UNIVERSAL_VALIDATION_NOP.lower,
			UniversalValidationUpper(0x3d, 0x0f, 18, 10, 5)},
		{UNIVERSAL_VALIDATION_NOP.lower,
			UniversalValidationUpper(0x3e, 0x0f, 19, 10, 5)},
		{UNIVERSAL_VALIDATION_NOP.lower,
			UniversalValidationUpper(0x3f, 0x0f, 20, 10, 5)},
		{UniversalValidationLowerMemory(0x00, 0x0f, 11, 1, 5),
			UNIVERSAL_VALIDATION_NOP.upper},
		{UNIVERSAL_VALIDATION_NOP.lower,
			UniversalValidationUpper(0x3c, 0x0f, 21, 11, 5)},
		{UNIVERSAL_VALIDATION_NOP.lower,
			UniversalValidationUpper(0x3d, 0x0f, 22, 11, 5)},
		{UNIVERSAL_VALIDATION_NOP.lower,
			UniversalValidationUpper(0x3e, 0x0f, 23, 11, 5)},
		{UNIVERSAL_VALIDATION_NOP.lower,
			UniversalValidationUpper(0x3f, 0x0f, 24, 11, 5)},
		{UniversalValidationLowerMemory(0x00, 0x0f, 12, 1, 6),
			UNIVERSAL_VALIDATION_NOP.upper},
		{UNIVERSAL_VALIDATION_NOP.lower,
			UniversalValidationUpper(0x3c, 0x0f, 25, 12, 5)},
		{UNIVERSAL_VALIDATION_NOP.lower,
			UniversalValidationUpper(0x3d, 0x0f, 26, 12, 5)},
		{UNIVERSAL_VALIDATION_NOP.lower,
			UniversalValidationUpper(0x3e, 0x0f, 27, 12, 5)},
		{UNIVERSAL_VALIDATION_NOP.lower,
			UniversalValidationUpper(0x3f, 0x0f, 28, 12, 5)},
		// PCSX2 VUops.cpp/VUflags.cpp oracle chain. Checkpoint ADDA,
		// SUBA, and MULA/MADDA/MSUBA through zero-product MADDs, then
		// retain OPMULA ACC and OPMSUB's forced-XYZ result/flag behavior.
		{UNIVERSAL_VALIDATION_NOP.lower,
			UniversalValidationUpper(0x3c, 0x0f, 2, 1, 0x0a)}, // ADDA
		{UNIVERSAL_VALIDATION_NOP.lower,
			UniversalValidationUpper(0x29, 0x0e, 0, 0, 29)},
		{UNIVERSAL_VALIDATION_NOP.lower,
			UniversalValidationUpper(0x3c, 0x0f, 2, 1, 0x0b)}, // SUBA
		{UNIVERSAL_VALIDATION_NOP.lower,
			UniversalValidationUpper(0x29, 0x0e, 0, 0, 30)},
		{UNIVERSAL_VALIDATION_NOP.lower,
			UniversalValidationUpper(0x3e, 0x0f, 2, 1, 0x0a)}, // MULA
		{UNIVERSAL_VALIDATION_NOP.lower,
			UniversalValidationUpper(0x3d, 0x0f, 2, 1, 0x0a)}, // MADDA
		{UNIVERSAL_VALIDATION_NOP.lower,
			UniversalValidationUpper(0x3d, 0x0f, 2, 1, 0x0b)}, // MSUBA
		{UNIVERSAL_VALIDATION_NOP.lower,
			UniversalValidationUpper(0x29, 0x0e, 0, 0, 31)},
		{UNIVERSAL_VALIDATION_NOP.lower,
			UniversalValidationUpper(0x3e, 0x0f, 2, 1, 0x0b)}, // OPMULA
			{UNIVERSAL_VALIDATION_NOP.lower,
				UniversalValidationUpper(0x2e, 0x0f, 2, 1, 30)}, // OPMSUB
			// PCSX2 VUops.cpp::_vuCLIP(): first emit +XYZ against VF1.W,
			// then shift that result by six while VF1 stays within VF2.W.
			{UNIVERSAL_VALIDATION_NOP.lower,
				UniversalValidationUpper(0x3f, 0x0f, 1, 2, 7)},
			{UNIVERSAL_VALIDATION_NOP.lower,
				UniversalValidationUpper(0x3f, 0x0f, 2, 1, 7)},
			// PCSX2 VUops.cpp::_vuDIV/_vuSQRT/_vuRSQRT/_vuWAITQ/
			// _vuESADD/_vuWAITP/_vuMFP.
			// Encode this one-shot owner against an immutable Accurate Q/P policy
			// without changing the live product/MTVU configuration. Unsynchronized
			// MULq/MFP consumers are exported before WAITQ/WAITP, while synchronized
			// DIV/SQRT/RSQRT consumers run in the upper half of the WAITQ pair as
			// specified by Sony VU User Manual 3.4.5/3.4.6.
			{UniversalValidationLowerT3(0x3c, 0x0e, 0x01, 2, 2),
				UNIVERSAL_VALIDATION_NOP.upper},
			{UNIVERSAL_VALIDATION_NOP.lower,
				UniversalValidationUpper(0x1c, 0x0f, 0, 1, 4)},
			{UniversalValidationLowerMemory(0x01, 0x0f, 1, 4, 7),
				UNIVERSAL_VALIDATION_NOP.upper},
			{UniversalValidationLowerT3(0x3f, 0x0e, 0, 0, 0),
				UniversalValidationUpper(0x1c, 0x0f, 0, 1, 4)},
			{UniversalValidationLowerMemory(0x01, 0x0f, 1, 4, 9),
				UNIVERSAL_VALIDATION_NOP.upper},
			{UniversalValidationLowerT3(0x3c, 0x1c, 0, 0, 1),
				UNIVERSAL_VALIDATION_NOP.upper},
			{UniversalValidationLowerT3(0x3c, 0x19, 0x0f, 4, 0),
				UNIVERSAL_VALIDATION_NOP.upper},
			{UniversalValidationLowerMemory(0x01, 0x0f, 1, 4, 8),
				UNIVERSAL_VALIDATION_NOP.upper},
			{UniversalValidationLowerT3(0x3f, 0x1e, 0, 0, 0),
				UNIVERSAL_VALIDATION_NOP.upper},
			{UniversalValidationLowerT3(0x3c, 0x19, 0x0f, 4, 0),
				UNIVERSAL_VALIDATION_NOP.upper},
			{UniversalValidationLowerT3(0x3d, 0x0e, 0x0c, 1, 0),
				UNIVERSAL_VALIDATION_NOP.upper},
			{UniversalValidationLowerT3(0x3f, 0x0e, 0, 0, 0),
				UniversalValidationUpper(0x1c, 0x0f, 0, 1, 5)},
			{UniversalValidationLowerT3(0x3e, 0x0e, 0x0f, 1, 2),
				UNIVERSAL_VALIDATION_NOP.upper},
			{UniversalValidationLowerT3(0x3f, 0x0e, 0, 0, 0),
				UniversalValidationUpper(0x1c, 0x0f, 0, 1, 6)},
			{UNIVERSAL_VALIDATION_NOP.lower,
				UniversalValidationUpper(0x2e, 0x0f, 2, 1, 30)},
			// PCSX2 VUops.cpp::_vuFSAND(): snapshot STATUS D/I bits after
			// edge Q producers. Before its WAIT/resource edge, SQRT(-1.0)'s I
			// bit is still pending. Issuing RSQRT(0/0) stalls on that producer,
			// publishes I, and leaves its own D+I update pending until terminal
			// drain. The two FSAND snapshots therefore prove old/current status
			// visibility rather than Instant-Q/P publication.
			{UniversalValidationLowerT3(0x3d, 0x0e, 0x0c, 10, 0),
				UNIVERSAL_VALIDATION_NOP.upper},
			{UniversalValidationLowerFlagImm12(0x16, 4, 0x30),
				UNIVERSAL_VALIDATION_NOP.upper},
			{UniversalValidationLowerT3(0x3e, 0x0e, 0x00, 0, 0),
				UNIVERSAL_VALIDATION_NOP.upper},
			{UniversalValidationLowerFlagImm12(0x16, 5, 0x30),
				UNIVERSAL_VALIDATION_NOP.upper},
			// Generic BSpline lower-body blockers. ERSADD and ESQRT use the
			// immutable Cortex-A9 approximate-P configuration and export both
			// results through WAITP/MFP/SQ for exact CPU/GPU comparison.
			{UniversalValidationLowerT3(0x3d, 0x1c, 0, 0, 1),
				UNIVERSAL_VALIDATION_NOP.upper},
			{UniversalValidationLowerT3(0x3f, 0x1e, 0, 0, 0),
				UNIVERSAL_VALIDATION_NOP.upper},
			{UniversalValidationLowerT3(0x3c, 0x19, 0x0f, 27, 0),
				UNIVERSAL_VALIDATION_NOP.upper},
			{UniversalValidationLowerMemory(0x01, 0x0f, 1, 27, 10),
				UNIVERSAL_VALIDATION_NOP.upper},
			{UniversalValidationLowerT3(0x3c, 0x1e, 0, 0, 2),
				UNIVERSAL_VALIDATION_NOP.upper},
			{UniversalValidationLowerT3(0x3f, 0x1e, 0, 0, 0),
				UNIVERSAL_VALIDATION_NOP.upper},
			{UniversalValidationLowerT3(0x3c, 0x19, 0x0f, 28, 0),
				UNIVERSAL_VALIDATION_NOP.upper},
			{UniversalValidationLowerMemory(0x01, 0x0f, 1, 28, 11),
				UNIVERSAL_VALIDATION_NOP.upper},
			// PCSX2 VUops.cpp::_vuILW/_vuISW/_vuILWR/_vuISWR and
			// _vuMFIR/_vuMTIR/_vuXITOP(). Exercise signed VI data,
			// selected-lane memory ordering, both addressing forms, MFIR sign
			// extension, VI/VF transfer, and serialized VIF ITOP ownership.
			{UniversalValidationLowerImm15(0x08, 8, 0, 112),
				UNIVERSAL_VALIDATION_NOP.upper},
			{UniversalValidationLowerImm15(0x09, 9, 0, 0x0123),
				UNIVERSAL_VALIDATION_NOP.upper},
			{UniversalValidationLowerMemory(0x05, 0x0a, 9, 8, 0),
				UNIVERSAL_VALIDATION_NOP.upper},
			{UniversalValidationLowerMemory(0x04, 0x0a, 10, 8, 0),
				UNIVERSAL_VALIDATION_NOP.upper},
			{UniversalValidationLowerImm15(0x08, 11, 0, 113),
				UNIVERSAL_VALIDATION_NOP.upper},
			{UniversalValidationLowerT3(0x3f, 0x0f, 0x05, 10, 11),
				UNIVERSAL_VALIDATION_NOP.upper},
			{UniversalValidationLowerT3(0x3e, 0x0f, 0x05, 12, 11),
				UNIVERSAL_VALIDATION_NOP.upper},
			{UniversalValidationLowerT3(0x3d, 0x0f, 0x0f, 3, 12),
				UNIVERSAL_VALIDATION_NOP.upper},
			{UniversalValidationLowerT3(0x3c, 0x0f, 0x02, 13, 3),
				UNIVERSAL_VALIDATION_NOP.upper},
			{UniversalValidationLowerT3(0x3d, 0x1a, 0, 14, 0),
				UNIVERSAL_VALIDATION_NOP.upper},
			{UNIVERSAL_VALIDATION_NOP.lower,
				UNIVERSAL_VALIDATION_NOP.upper | 0x40000000u},
			UNIVERSAL_VALIDATION_NOP,
		}};
	std::array<RawVuPair, UNIVERSAL_VALIDATION_SOURCE_PAIR_COUNT>
		dependent_program;
	dependent_program.fill(UNIVERSAL_VALIDATION_NOP);
	constexpr u32 semantic_terminal_pairs = 2;
	constexpr u32 semantic_body_pairs =
		UNIVERSAL_VALIDATION_SEMANTIC_PAIR_COUNT - semantic_terminal_pairs;
	std::copy_n(semantic_program.begin(), semantic_body_pairs,
		dependent_program.begin());
	std::copy_n(semantic_program.begin() + semantic_body_pairs,
		semantic_terminal_pairs,
		dependent_program.begin() + semantic_body_pairs +
			UNIVERSAL_VALIDATION_LONG_NOP_PAIRS);
	// The first execution now crosses the old 128-pair validation ceiling while
	// retaining the exact semantic body and terminal E-bit ordering.  TPC then
	// points at this independent MSCNT continuation, which writes a marker and
	// terminates in the normal E-bit plus delay-pair sequence.
	dependent_program[UNIVERSAL_VALIDATION_FIRST_EXECUTE_PAIRS] = {
		UniversalValidationLowerImm15(0x08, 6, 0, 0x0456),
		UNIVERSAL_VALIDATION_NOP.upper};
	dependent_program[UNIVERSAL_VALIDATION_FIRST_EXECUTE_PAIRS + 1] = {
		UNIVERSAL_VALIDATION_NOP.lower,
		UNIVERSAL_VALIDATION_NOP.upper | 0x40000000u};
	dependent_program[UNIVERSAL_VALIDATION_FIRST_EXECUTE_PAIRS + 2] =
		UNIVERSAL_VALIDATION_NOP;
	std::memcpy(source.data(), dependent_program.data(),
		dependent_program.size() * sizeof(RawVuPair));
	auto encoded = std::make_unique<VitaGpuVu::UniversalMicroProgram>();
	std::string error;
	const u32 validation_configuration_bits =
		VitaGpuVu::GetCurrentUniversalMicroProgramConfigurationBits() &
		~(VitaGpuVu::UniversalConfigurationAssumeScheduled |
			VitaGpuVu::UniversalConfigurationInstantQp);
	if (validation_configuration_bits != 0x00002c1cu)
	{
		Console.Error(
			"GPU-VU: universal validation configuration mismatch (%08x).",
			validation_configuration_bits);
		return false;
	}
	if ((validation_configuration_bits &
			VitaGpuVu::UniversalConfigurationApproximateQ) != 0)
	{
		Console.Error(
			"GPU-VU: universal validation requires exact Q arithmetic.");
		return false;
	}
	if (!VitaGpuVu::EncodeUniversalMicroProgramForConfiguration(
			source.data(), source.size(), 0, validation_configuration_bits,
			encoded.get(), &error))
	{
		Console.Error("GPU-VU: universal validation encode failed: %s",
			error.c_str());
		return false;
	}
	VitaGpuVu::UniversalFixedPipelineState expected_pipeline;
	for (u32 pair = 0;
		pair < UNIVERSAL_VALIDATION_FIRST_EXECUTE_PAIRS; pair++)
	{
		if (!VitaGpuVu::AdvanceUniversalFixedPipeline(encoded->pairs[pair],
				encoded->configuration_bits, &expected_pipeline, &error))
		{
			Console.Error("GPU-VU: universal pipeline oracle failed at pair %u: %s",
				pair, error.c_str());
			return false;
		}
	}
	VitaGpuVu::FinishUniversalFixedPipeline(&expected_pipeline);
	expected_pipeline = {};
	for (u32 pair = UNIVERSAL_VALIDATION_FIRST_EXECUTE_PAIRS;
		pair < dependent_program.size(); pair++)
	{
		if (!VitaGpuVu::AdvanceUniversalFixedPipeline(encoded->pairs[pair],
				encoded->configuration_bits, &expected_pipeline, &error))
		{
			Console.Error(
				"GPU-VU: universal resume pipeline oracle failed at pair %u: %s",
				pair, error.c_str());
			return false;
		}
	}
	VitaGpuVu::FinishUniversalFixedPipeline(&expected_pipeline);
	if (expected_pipeline.cycle > std::numeric_limits<u32>::max())
	{
		Console.Error("GPU-VU: universal pipeline oracle cycle overflow.");
		return false;
	}
	gpu_vu_universal_expected_cycle =
		static_cast<u32>(expected_pipeline.cycle);
	gpu_vu_universal_configuration_bits = encoded->configuration_bits;
	std::memcpy(static_cast<u8*>(gpu_vu_universal_validation_buffer.base) +
			GPU_VU_UNIVERSAL_MICRO_OFFSET, encoded->pairs.data(),
		GPU_VU_UNIVERSAL_MICRO_BYTES);

	u32* const vf = reinterpret_cast<u32*>(
		static_cast<u8*>(gpu_vu_universal_validation_buffer.base) +
		GPU_VU_UNIVERSAL_VF_OFFSET);
	u32* const state = reinterpret_cast<u32*>(
		static_cast<u8*>(gpu_vu_universal_validation_buffer.base) +
		GPU_VU_UNIVERSAL_STATE_OFFSET);
	auto* const epoch = reinterpret_cast<VitaGpuVu::UniversalEpochMicroOp*>(
		static_cast<u8*>(gpu_vu_universal_validation_buffer.base) +
			GPU_VU_UNIVERSAL_EPOCH_OFFSET);
	u32* const payload = reinterpret_cast<u32*>(
		static_cast<u8*>(gpu_vu_universal_validation_buffer.base) +
		GPU_VU_UNIVERSAL_PAYLOAD_OFFSET);
	auto* const path1 =
		reinterpret_cast<VitaGpuVu::UniversalRawPath1Export*>(
			static_cast<u8*>(gpu_vu_universal_validation_buffer.base) +
				GPU_VU_UNIVERSAL_PATH1_OFFSET);
	u32* const arm_estimates = reinterpret_cast<u32*>(
		static_cast<u8*>(gpu_vu_universal_validation_buffer.base) +
			GPU_VU_UNIVERSAL_ESTIMATE_OFFSET);
	// ARM ARM RecipEstimate()/RecipSqrtEstimate(), as implemented by the
	// Cortex-A9 VRECPE/VRSQRTE instructions used by VitaVuApproximateMath.h.
	// Store the architecturally consumed eight-bit estimate field rather than
	// asking the SGX program to reduce the wider intermediate after a dynamic
	// uniform-buffer load. The GXP then performs only indexed lookup plus the
	// architected non-fused refinement for every runtime operand.
	for (u32 scaled = 256; scaled < 512; scaled++)
	{
		const u32 a = scaled * 2u + 1u;
		const u32 b = (1u << 19u) / a;
		arm_estimates[scaled - 256u] = ((b + 1u) >> 1u) & 0xffu;
	}
	for (u32 scaled = 128; scaled < 512; scaled++)
	{
		u32 a;
		if (scaled < 256u)
			a = scaled * 2u + 1u;
		else
			a = ((((scaled >> 1u) << 1u) + 1u) * 2u);
		u32 b = 512u;
		while (a * (b + 1u) * (b + 1u) < (1u << 28u))
			b++;
		arm_estimates[256u + scaled - 128u] = ((b + 1u) >> 1u) & 0xffu;
	}
	const std::array<u32, 4> vf1 = {
		0x3f800000u, 0x40000000u, 0x40400000u, 0x40800000u};
	const std::array<u32, 4> vf2 = {
		0x41200000u, 0x41a00000u, 0x41f00000u, 0x42200000u};
	const std::array<u32, 4> itof_source = {
		0x00000000u, 0x00000001u, 0x7fffffffu, 0x80000000u};
	vf[3] = 0x3f800000u;
	std::copy(vf1.begin(), vf1.end(), payload);
	std::copy(vf2.begin(), vf2.end(), payload + 4);
	GIFTag packed_tag{};
	packed_tag.NLOOP = 1;
	packed_tag.EOP = 1;
	packed_tag.FLG = GIF_FLG_PACKED;
	packed_tag.NREG = 1;
	packed_tag.REGS = GIF_REG_A_D;
	std::memcpy(payload + 8, &packed_tag, sizeof(packed_tag));
	for (u32 word = 0; word < 4; word++)
		payload[12 + word] = 0xdeadc000u + word;
	GIFTag image_tag{};
	image_tag.NLOOP = 1;
	image_tag.FLG = GIF_FLG_IMAGE;
	std::memcpy(payload + 16, &image_tag, sizeof(image_tag));
	for (u32 word = 0; word < 4; word++)
		payload[20 + word] = 0x60000000u + word * 0x01010101u;
	GIFTag reglist_tag{};
	reglist_tag.NLOOP = 3;
	reglist_tag.EOP = 1;
	reglist_tag.FLG = GIF_FLG_REGLIST;
	reglist_tag.NREG = 3;
	reglist_tag.REGS = GIF_REG_PRIM | (static_cast<u64>(GIF_REG_RGBA) << 4) |
		(static_cast<u64>(GIF_REG_XYZ2) << 8);
	std::memcpy(payload + 24, &reglist_tag, sizeof(reglist_tag));
	for (u32 qword = 0; qword < 5; qword++)
	{
		for (u32 word = 0; word < 4; word++)
		{
			payload[28 + qword * 4 + word] =
				0x70000000u + (qword + 1u) * 0x100u +
				word * 0x01010101u;
		}
	}
	std::copy(itof_source.begin(), itof_source.end(), payload + 48);
	for (u32 vector = 0;
		vector < UNIVERSAL_VALIDATION_FTOI_SOURCES.size(); vector++)
	{
		std::copy(UNIVERSAL_VALIDATION_FTOI_SOURCES[vector].begin(),
			UNIVERSAL_VALIDATION_FTOI_SOURCES[vector].end(),
			payload + 52 + vector * 4);
	}
	path1->format_version =
		VitaGpuVu::UniversalRawPath1ExportFormatVersion;
	VitaGpuVu::VifUnpackSpan first_unpack;
	first_unpack.source_size = 16;
	first_unpack.tag_size_words = 4;
	first_unpack.destination_qword = 100;
	first_unpack.vector_count = 1;
	first_unpack.vif_top = 0x0123;
	first_unpack.vif_itop = 0x0234;
	first_unpack.command = 0x6c;
	first_unpack.cycle_cl = 1;
	first_unpack.cycle_wl = 1;
	VitaGpuVu::VifUnpackSpan second_unpack = first_unpack;
	second_unpack.destination_qword = 101;
	VitaGpuVu::VifUnpackSpan packed_unpack = first_unpack;
	packed_unpack.source_size = 2 * 16;
	packed_unpack.tag_size_words = 8;
	packed_unpack.destination_qword = 16;
	packed_unpack.vector_count = 2;
	VitaGpuVu::VifUnpackSpan wrapped_unpack = first_unpack;
	wrapped_unpack.source_size = 8 * 16;
	wrapped_unpack.tag_size_words = 32;
	wrapped_unpack.destination_qword = 1022;
	wrapped_unpack.vector_count = 8;
	VitaGpuVu::VifUnpackSpan itof_unpack = first_unpack;
	itof_unpack.destination_qword = 102;
	VitaGpuVu::VifUnpackSpan ftoi_unpack = first_unpack;
	ftoi_unpack.source_size = 4 * 16;
	ftoi_unpack.tag_size_words = 16;
	ftoi_unpack.destination_qword = 103;
	ftoi_unpack.vector_count = 4;
	if (!VitaGpuVu::EncodeUniversalVifUnpackCommand(
			first_unpack, 0, &epoch[0], &error) ||
		!VitaGpuVu::EncodeUniversalVifUnpackCommand(
			second_unpack, 16, &epoch[1], &error) ||
		!VitaGpuVu::EncodeUniversalVifUnpackCommand(
			packed_unpack, 32, &epoch[2], &error) ||
		!VitaGpuVu::EncodeUniversalVifUnpackCommand(
			wrapped_unpack, 64, &epoch[3], &error) ||
		!VitaGpuVu::EncodeUniversalVifUnpackCommand(
			itof_unpack, 192, &epoch[4], &error) ||
		!VitaGpuVu::EncodeUniversalVifUnpackCommand(
			ftoi_unpack, 208, &epoch[5], &error) ||
		!VitaGpuVu::EncodeUniversalVuExecuteCommand(
			0, 0, UNIVERSAL_VALIDATION_FIRST_EXECUTE_PAIRS,
			0x0123, 0x0234, 0,
			false, &epoch[6], &error))
	{
		Console.Error("GPU-VU: universal epoch encode failed: %s",
			error.c_str());
		return false;
	}
	if (!VitaGpuVu::EncodeUniversalVuExecuteCommand(
			0, 0, UNIVERSAL_VALIDATION_RESUME_PAIRS,
			0x0123, 0x0234, 0, true, &epoch[7], &error))
	{
		Console.Error("GPU-VU: universal resume epoch encode failed: %s",
			error.c_str());
		return false;
	}
	epoch[8] = VitaGpuVu::EncodeUniversalEpochEndCommand();
	state[1] = 0xeeeeeeeeu;
	state[2] = 0xfeedfaceu;
	// Non-instant consumers must observably retain the old architectural Q/P
	// values until WAITQ/WAITP or terminal drain publishes a pending result.
	state[25] = 0x40400000u; // Q = 3.0
	state[26] = 0x40000000u; // P = 2.0
	state[29] = 9;
	state[30] = 17u * 16u;
	state[44] = 0x000000c1u;
	state[46] = 0;
	state[47] = gpu_vu_universal_configuration_bits;

	gpu_vu_universal_validation_stage =
		UniversalGpuVuValidationStage::Ready;
	Console.WriteLn(
		"GPU-VU: universal fixed executor registered "
		"(encoded=%u bytes, epoch=%u bytes, payload=%u bytes, path1=%u bytes, "
		"one coarse validation boundary).",
		GPU_VU_UNIVERSAL_MICRO_BYTES,
		GPU_VU_UNIVERSAL_EPOCH_BYTES, GPU_VU_UNIVERSAL_PAYLOAD_BYTES,
		GPU_VU_UNIVERSAL_PATH1_BYTES);
	return true;
}

bool GSDeviceGXM::Impl::SubmitUniversalGpuVuValidation()
{
	if (!scene_active || !gpu_vu_sequential_indices ||
		gpu_vu_universal_validation_stage !=
			UniversalGpuVuValidationStage::Ready ||
		!gpu_vu_universal_validation_buffer.IsMapped())
	{
		return false;
	}

	u8* const base =
		static_cast<u8*>(gpu_vu_universal_validation_buffer.base);
	sceGxmSetVertexProgram(context, gpu_vu_universal_vertex_program);
	sceGxmSetFragmentProgram(context, gpu_vu_universal_fragment_program);
	int result = sceGxmSetVertexUniformBuffer(context, 0,
		base + GPU_VU_UNIVERSAL_MICRO_OFFSET);
	if (result >= 0)
	{
		result = sceGxmSetVertexUniformBuffer(context, 1,
			base + GPU_VU_UNIVERSAL_VF_OFFSET);
	}
	if (result >= 0)
	{
		result = sceGxmSetVertexUniformBuffer(context, 2,
			base + GPU_VU_UNIVERSAL_STATE_OFFSET);
	}
	if (result >= 0)
	{
		result = sceGxmSetVertexUniformBuffer(context, 3,
			base + GPU_VU_UNIVERSAL_MEMORY_OFFSET);
	}
	if (result >= 0)
	{
		result = sceGxmSetVertexUniformBuffer(context, 4,
			base + GPU_VU_UNIVERSAL_EPOCH_OFFSET);
	}
	if (result >= 0)
	{
		result = sceGxmSetVertexUniformBuffer(context, 5,
			base + GPU_VU_UNIVERSAL_PAYLOAD_OFFSET);
	}
	if (result >= 0)
	{
		result = sceGxmSetVertexUniformBuffer(context, 6,
			base + GPU_VU_UNIVERSAL_PATH1_OFFSET);
	}
	if (result >= 0)
	{
		result = sceGxmSetVertexUniformBuffer(context, 7,
			base + GPU_VU_UNIVERSAL_ESTIMATE_OFFSET);
	}
		for (u32 page = 1; page <= 3u &&
			result >= 0; page++)
	{
		result = sceGxmSetVertexUniformBuffer(context, 7u + page,
			base + GPU_VU_UNIVERSAL_PATH1_OFFSET +
					page * VitaGpuVu::UniversalRawPath1ExportPageWords * sizeof(u32));
		}
		if (result >= 0)
			result = sceGxmSetVertexUniformBuffer(context, 11,
				base + GPU_VU_UNIVERSAL_OUTPUT_VF_OFFSET);
		if (result >= 0)
			result = sceGxmSetVertexUniformBuffer(context, 12,
				base + GPU_VU_UNIVERSAL_OUTPUT_STATE_OFFSET);
		if (result >= 0)
			result = sceGxmSetVertexUniformBuffer(context, 13,
				base + GPU_VU_UNIVERSAL_OUTPUT_MEMORY_OFFSET);
	if (result < 0)
		return Fail("bind universal GPU-VU validation buffers", result);
	sceGxmSetFrontDepthFunc(context, SCE_GXM_DEPTH_FUNC_ALWAYS);
	sceGxmSetBackDepthFunc(context, SCE_GXM_DEPTH_FUNC_ALWAYS);
	sceGxmSetFrontDepthWriteEnable(context, SCE_GXM_DEPTH_WRITE_DISABLED);
	sceGxmSetBackDepthWriteEnable(context, SCE_GXM_DEPTH_WRITE_DISABLED);
	ConfigureGpuVuComputeRaster(context);
	result = sceGxmDraw(context, SCE_GXM_PRIMITIVE_POINTS,
		SCE_GXM_INDEX_FORMAT_U16, gpu_vu_sequential_indices, 1);
	RestoreGpuVuComputeRaster(context);
	if (result < 0)
		return Fail("sceGxmDraw(universal GPU-VU long validation)", result);
	gpu_vu_universal_validation_stage =
		UniversalGpuVuValidationStage::Submitted;
	Console.WriteLn(
		"GPU-VU: universal single-invocation long-execute plus resumed "
		"VIF/VU validation submitted.");
	return true;
}

bool GSDeviceGXM::Impl::CompleteUniversalGpuVuValidation()
{
	if (gpu_vu_universal_validation_stage !=
		UniversalGpuVuValidationStage::Submitted)
	{
		return false;
	}
	// Validation performs one coarse command-epoch drain after the submitted
	// scene, never one wait per pair or XGKICK. Product execution must replace
	// this readback with the existing asynchronous notification ownership.
	if (!Finish())
		return false;

	const u8* const base = static_cast<const u8*>(
		gpu_vu_universal_validation_buffer.base);
	const u32* const vf = reinterpret_cast<const u32*>(
			base + GPU_VU_UNIVERSAL_OUTPUT_VF_OFFSET);
	const u32* const state = reinterpret_cast<const u32*>(
			base + GPU_VU_UNIVERSAL_OUTPUT_STATE_OFFSET);
	const u32* const memory = reinterpret_cast<const u32*>(
			base + GPU_VU_UNIVERSAL_OUTPUT_MEMORY_OFFSET);
	const auto* const path1 =
		reinterpret_cast<const VitaGpuVu::UniversalRawPath1Export*>(
			base + GPU_VU_UNIVERSAL_PATH1_OFFSET);
	std::array<u32, 33 * 4> expected_vf{};
	expected_vf[3] = 0x3f800000u;
	const std::array<u32, 4> vf1 = {
		0x3f800000u, 0x40000000u, 0x40400000u, 0x40800000u};
	const std::array<u32, 4> vf2 = {
		0x41200000u, 0x41a00000u, 0x41f00000u, 0x42200000u};
	const std::array<u32, 4> vf3 = {
		0x41300000u, 0x41b00000u, 0x42040000u, 0x42300000u};
	const std::array<u32, 4> itof_source = {
		0x00000000u, 0x00000001u, 0x7fffffffu, 0x80000000u};
	const std::array<std::array<u32, 4>, 4> itof_expected = {{
		{{0x00000000u, 0x3f800000u, 0x4f000000u, 0xcf000000u}},
		{{0x00000000u, 0x3d800000u, 0x4d000000u, 0xcd000000u}},
		{{0x00000000u, 0x39800000u, 0x49000000u, 0xc9000000u}},
		{{0x00000000u, 0x38000000u, 0x47800000u, 0xc7800000u}},
	}};
	const std::array<std::array<std::array<u32, 4>, 4>, 4>
		ftoi_expected = {{
			{{
				{{0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
				{{0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
				{{0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
				{{0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u}},
			}},
			{{
				{{0x00000000u, 0x00000000u, 0x00000001u, 0xffffffffu}},
				{{0x0000000fu, 0xfffffff1u, 0x00000010u, 0xfffffff0u}},
				{{0x00000fffu, 0xfffff001u, 0x00001000u, 0xfffff000u}},
				{{0x00007fffu, 0xffff8001u, 0x00008000u, 0xffff8000u}},
			}},
			{{
				{{0x7fffff80u, 0x80000080u, 0x7fffffffu, 0x80000000u}},
				{{0x7fffffffu, 0x80000000u, 0x7fffffffu, 0x80000000u}},
				{{0x7fffffffu, 0x80000000u, 0x7fffffffu, 0x80000000u}},
				{{0x7fffffffu, 0x80000000u, 0x7fffffffu, 0x80000000u}},
			}},
			{{
				{{0x7fffffffu, 0x80000000u, 0x7fffffffu, 0x80000000u}},
				{{0x7fffffffu, 0x80000000u, 0x7fffffffu, 0x80000000u}},
				{{0x7fffffffu, 0x80000000u, 0x7fffffffu, 0x80000000u}},
				{{0x7fffffffu, 0x80000000u, 0x7fffffffu, 0x80000000u}},
			}},
		}};
	std::copy(vf1.begin(), vf1.end(), expected_vf.begin() + 4);
	std::copy(vf2.begin(), vf2.end(), expected_vf.begin() + 8);
	std::copy(vf3.begin(), vf3.end(), expected_vf.begin() + 12);
	std::copy(itof_source.begin(), itof_source.end(), expected_vf.begin() + 16);
	for (u32 variant = 0; variant < itof_expected.size(); variant++)
	{
		std::copy(itof_expected[variant].begin(), itof_expected[variant].end(),
			expected_vf.begin() + (5u + variant) * 4u);
	}
	for (u32 source = 0;
		source < UNIVERSAL_VALIDATION_FTOI_SOURCES.size(); source++)
	{
		std::copy(UNIVERSAL_VALIDATION_FTOI_SOURCES[source].begin(),
			UNIVERSAL_VALIDATION_FTOI_SOURCES[source].end(),
			expected_vf.begin() + (9u + source) * 4u);
		for (u32 variant = 0; variant < ftoi_expected[source].size(); variant++)
		{
			std::copy(ftoi_expected[source][variant].begin(),
				ftoi_expected[source][variant].end(),
				expected_vf.begin() + (13u + source * 4u + variant) * 4u);
		}
	}
		const std::array<u32, 4> adda_checkpoint = {
			0x41300000u, 0x41b00000u, 0x42040000u, 0x00000000u};
		const std::array<u32, 4> opmsub_checkpoint = {
			0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u};
		const std::array<u32, 4> multiply_accumulator_checkpoint = {
			0x41200000u, 0x42200000u, 0x42b40000u, 0x00000000u};
		const std::array<u32, 4> opmula_accumulator = {
			0x42700000u, 0x41f00000u, 0x41a00000u, 0x43200000u};
		const std::array<u32, 4> mfp_p_expected = {
			0x41600000u, 0x41600000u, 0x41600000u, 0x41600000u};
		const std::array<u32, 4> sqrt_q_mul_expected = {
			0x40000000u, 0x40800000u, 0x40c00000u, 0x41000000u};
		const std::array<u32, 4> rsqrt_q_mul_expected = {
			0x41a00000u, 0x42200000u, 0x42700000u, 0x42a00000u};
		const float ersadd_value = VitaVU::ApproximateReciprocal(14.0f);
		const float esqrt_value = VitaVU::ApproximateSqrt(10.0f);
		u32 ersadd_bits = 0;
		u32 esqrt_bits = 0;
		std::memcpy(&ersadd_bits, &ersadd_value, sizeof(ersadd_bits));
		std::memcpy(&esqrt_bits, &esqrt_value, sizeof(esqrt_bits));
		const std::array<u32, 4> ersadd_p_expected = {
			ersadd_bits, ersadd_bits, ersadd_bits, ersadd_bits};
		const std::array<u32, 4> esqrt_p_expected = {
			esqrt_bits, esqrt_bits, esqrt_bits, esqrt_bits};
		std::copy(mfp_p_expected.begin(), mfp_p_expected.end(),
			expected_vf.begin() + 4u * 4u);
		std::copy(sqrt_q_mul_expected.begin(), sqrt_q_mul_expected.end(),
			expected_vf.begin() + 5u * 4u);
		std::copy(rsqrt_q_mul_expected.begin(), rsqrt_q_mul_expected.end(),
			expected_vf.begin() + 6u * 4u);
		std::copy(adda_checkpoint.begin(), adda_checkpoint.end(),
			expected_vf.begin() + 29u * 4u);
	std::copy(opmsub_checkpoint.begin(), opmsub_checkpoint.end(),
		expected_vf.begin() + 30u * 4u);
	std::copy(multiply_accumulator_checkpoint.begin(),
		multiply_accumulator_checkpoint.end(), expected_vf.begin() + 31u * 4u);
	std::copy(opmula_accumulator.begin(), opmula_accumulator.end(),
		expected_vf.begin() + 32u * 4u);
	std::copy(ersadd_p_expected.begin(), ersadd_p_expected.end(),
		expected_vf.begin() + 27u * 4u);
	std::copy(esqrt_p_expected.begin(), esqrt_p_expected.end(),
		expected_vf.begin() + 28u * 4u);
	const std::array<u32, 4> mfir_expected = {
		0xfffffeddu, 0xfffffeddu, 0xfffffeddu, 0xfffffeddu};
	std::copy(mfir_expected.begin(), mfir_expected.end(),
		expected_vf.begin() + 3u * 4u);
	std::array<u32, GPU_VU_UNIVERSAL_STATE_WORDS> expected_state{};
	expected_state[0] = UNIVERSAL_VALIDATION_SOURCE_PAIR_COUNT *
		VitaGpuVu::UniversalMicroProgramPairBytes;
	expected_state[1] = UNIVERSAL_VALIDATION_RESUME_PAIRS;
	expected_state[2] = 1;
		expected_state[8 + 1] = 100;
		expected_state[8 + 2] = 16;
		expected_state[8 + 3] = 1022;
		expected_state[8 + 4] = 0x0000u;
		expected_state[8 + 5] = 0x0010u;
		expected_state[8 + 6] = 0x0456u;
		expected_state[8 + 7] = 0x0123u;
		expected_state[8 + 8] = 112u;
		expected_state[8 + 9] = 0xfeddu;
		expected_state[8 + 10] = 0xfeddu;
		expected_state[8 + 11] = 113u;
		expected_state[8 + 12] = 0xfeddu;
		expected_state[8 + 13] = 0xfeddu;
		expected_state[8 + 14] = 0x0234u;
		expected_state[25] = 0x00000000u;
		expected_state[26] = esqrt_bits;
		expected_state[27] = UNIVERSAL_VALIDATION_SOURCE_PAIR_COUNT;
	expected_state[29] = 9;
	expected_state[30] = 17 * 16;
	expected_state[31] = 8;
		expected_state[32] = 17;
		expected_state[40] = 0x0123;
	expected_state[41] = 0x0234;
	expected_state[42] = 0x000eu;
	expected_state[43] = 0x0031u;
	expected_state[44] = 0x000000c1u;
	expected_state[45] = 0x00000540u;
		expected_state[46] = gpu_vu_universal_expected_cycle;
		expected_state[47] = gpu_vu_universal_configuration_bits;
		expected_state[48] = 1u;
		expected_state[49] = 0x3f800000u;
		expected_state[50] = 0x40000000u;
		expected_state[51] = 0x40400000u;
		expected_state[52] = 0x41600000u;
		expected_state[53] = 192u;
		expected_state[54] = 36u;
		expected_state[55] = 0x3d920000u;
		expected_state[56] = 0x3f7f8000u;
		expected_state[57] = 0x3f804000u;
		expected_state[58] = ersadd_bits;
		expected_state[59] = 2u;

	std::array<u32, 1024 * 4> expected_memory{};
	std::copy(vf1.begin(), vf1.end(), expected_memory.begin() + 100 * 4);
	std::copy(vf2.begin(), vf2.end(), expected_memory.begin() + 101 * 4);
	std::copy(itof_source.begin(), itof_source.end(),
		expected_memory.begin() + 102 * 4);
	for (u32 vector = 0;
		vector < UNIVERSAL_VALIDATION_FTOI_SOURCES.size(); vector++)
	{
		std::copy(UNIVERSAL_VALIDATION_FTOI_SOURCES[vector].begin(),
			UNIVERSAL_VALIDATION_FTOI_SOURCES[vector].end(),
			expected_memory.begin() + (103u + vector) * 4u);
	}
	const std::array<u32, 4> old_q_consumer = {
		0x40400000u, 0x40c00000u, 0x41100000u, 0x41400000u};
	const std::array<u32, 4> old_p_consumer = {
		0x40000000u, 0x40000000u, 0x40000000u, 0x40000000u};
	const std::array<u32, 4> synchronized_div_consumer = {
		0x40000000u, 0x40800000u, 0x40c00000u, 0x41000000u};
	std::copy(old_q_consumer.begin(), old_q_consumer.end(),
		expected_memory.begin() + 107u * 4u);
	std::copy(old_p_consumer.begin(), old_p_consumer.end(),
		expected_memory.begin() + 108u * 4u);
	std::copy(synchronized_div_consumer.begin(),
		synchronized_div_consumer.end(),
		expected_memory.begin() + 109u * 4u);
	std::copy(ersadd_p_expected.begin(), ersadd_p_expected.end(),
		expected_memory.begin() + 110u * 4u);
	std::copy(esqrt_p_expected.begin(), esqrt_p_expected.end(),
		expected_memory.begin() + 111u * 4u);
	expected_memory[112u * 4u + 0u] = 0x0000feddu;
	expected_memory[112u * 4u + 2u] = 0x0000feddu;
	expected_memory[113u * 4u + 1u] = 0x0000feddu;
	expected_memory[113u * 4u + 3u] = 0x0000feddu;
	GIFTag packed_tag{};
	packed_tag.NLOOP = 1;
	packed_tag.EOP = 1;
	packed_tag.FLG = GIF_FLG_PACKED;
	packed_tag.NREG = 1;
	packed_tag.REGS = GIF_REG_A_D;
	std::memcpy(expected_memory.data() + 16 * 4, &packed_tag,
		sizeof(packed_tag));
	// The second command epoch deliberately repeats the same two-vector UNPACK
	// before MSCNT.  Its payload therefore replaces the first execution's SQ
	// result at qword 17 before the three-pair continuation runs.
	// Both Execute commands now share one invocation. The resume segment has
	// no repeated UNPACK, so the first segment's dependent SQ remains the final
	// VU-memory value as well as the already-exported PATH1 value.
	std::copy(vf3.begin(), vf3.end(), expected_memory.begin() + 17 * 4);
	GIFTag image_tag{};
	image_tag.NLOOP = 1;
	image_tag.FLG = GIF_FLG_IMAGE;
	std::memcpy(expected_memory.data() + 1022 * 4, &image_tag,
		sizeof(image_tag));
	for (u32 word = 0; word < 4; word++)
		expected_memory[1023 * 4 + word] =
			0x60000000u + word * 0x01010101u;
	GIFTag reglist_tag{};
	reglist_tag.NLOOP = 3;
	reglist_tag.EOP = 1;
	reglist_tag.FLG = GIF_FLG_REGLIST;
	reglist_tag.NREG = 3;
	reglist_tag.REGS = GIF_REG_PRIM |
		(static_cast<u64>(GIF_REG_RGBA) << 4) |
		(static_cast<u64>(GIF_REG_XYZ2) << 8);
	std::memcpy(expected_memory.data(), &reglist_tag,
		sizeof(reglist_tag));
	for (u32 qword = 1; qword <= 5; qword++)
	{
		for (u32 word = 0; word < 4; word++)
		{
			expected_memory[qword * 4 + word] =
				0x70000000u + qword * 0x100u +
				word * 0x01010101u;
		}
	}
	const bool memory_ok = std::memcmp(memory, expected_memory.data(),
		GPU_VU_UNIVERSAL_MEMORY_BYTES) == 0;
	auto expected_path1 =
		std::make_unique<VitaGpuVu::UniversalRawPath1Export>();
	auto expected_path1_memory = expected_memory;
	// PATH1 captured the first execution's complete packet before the resume
	// segment. Packet retirement remains immutable within the transaction.
	std::string path1_error;
	const bool expected_path1_ok =
		VitaGpuVu::AppendUniversalRawPath1Packet(
			reinterpret_cast<const u8*>(expected_path1_memory.data()), 16 * 16,
			expected_path1.get(), &path1_error) &&
		VitaGpuVu::AppendUniversalRawPath1Packet(
			reinterpret_cast<const u8*>(expected_path1_memory.data()), 1022 * 16,
			expected_path1.get(), &path1_error);
	const bool path1_ok = expected_path1_ok &&
		std::memcmp(path1, expected_path1.get(), sizeof(*expected_path1)) == 0;
	const bool vf_ok = std::memcmp(vf, expected_vf.data(),
		GPU_VU_UNIVERSAL_VF_BYTES) == 0;
	const bool state_ok = std::memcmp(state, expected_state.data(),
		GPU_VU_UNIVERSAL_STATE_BYTES) == 0;
	u32 first_state_mismatch = 64;
	u32 first_memory_mismatch = expected_memory.size();
	if (!state_ok)
	{
		for (u32 word = 0; word < expected_state.size(); word++)
		{
			if (state[word] != expected_state[word])
			{
				first_state_mismatch = word;
				break;
			}
		}
	}
	if (!memory_ok)
	{
		for (u32 word = 0; word < expected_memory.size(); word++)
		{
			if (memory[word] != expected_memory[word])
			{
				first_memory_mismatch = word;
				break;
			}
		}
	}
	if (!vf_ok || !state_ok || !memory_ok || !path1_ok)
	{
		gpu_vu_universal_validation_stage =
			UniversalGpuVuValidationStage::Failed;
		Console.Error(
				"GPU-VU: universal VIF/VU command-epoch validation failed "
				"(vf=%u state=%u memory=%u path1=%u pc=%u reason=%u pairs=%u "
				"commands=%u unpacks=%u vi=%04x:%04x:%04x:%04x:%04x "
				"vf3=%08x:%08x:%08x:%08x mem17=%08x:%08x:%08x:%08x "
				"q=%08x:%08x p=%08x:%08x vf4=%08x:%08x:%08x:%08x "
				"vf5=%08x:%08x:%08x:%08x vf6=%08x:%08x:%08x:%08x "
				"path1_packets=%u path1_qwords=%u path1_error=%u "
				"first_state_mismatch=%u actual=%08x expected=%08x "
				"cycle=%u:%u config=%08x:%08x).",
			vf_ok ? 1u : 0u, state_ok ? 1u : 0u, memory_ok ? 1u : 0u,
				path1_ok ? 1u : 0u,
				state[0], state[2], state[27], state[31], state[32],
				state[9] & 0xffffu, state[10] & 0xffffu,
				state[11] & 0xffffu, state[12] & 0xffffu,
				state[13] & 0xffffu, vf[12], vf[13], vf[14], vf[15],
				memory[68], memory[69], memory[70], memory[71],
				state[25], expected_state[25], state[26], expected_state[26],
				vf[16], vf[17], vf[18], vf[19],
				vf[20], vf[21], vf[22], vf[23],
				vf[24], vf[25], vf[26], vf[27],
				path1->packet_count, path1->data_qword_count,
				static_cast<u32>(path1->error), first_state_mismatch,
			first_state_mismatch < expected_state.size() ?
				state[first_state_mismatch] : 0u,
			first_state_mismatch < expected_state.size() ?
				expected_state[first_state_mismatch] : 0u,
				state[46], expected_state[46], state[47], expected_state[47]);
		Console.Error(
			"GPU-VU: universal memory mismatch "
			"first_word=%u actual=%08x expected=%08x "
			"qword107=%08x:%08x:%08x:%08x/%08x:%08x:%08x:%08x "
			"qword108=%08x:%08x:%08x:%08x/%08x:%08x:%08x:%08x "
			"qword109=%08x:%08x:%08x:%08x/%08x:%08x:%08x:%08x.",
			first_memory_mismatch,
			first_memory_mismatch < expected_memory.size() ?
				memory[first_memory_mismatch] : 0u,
			first_memory_mismatch < expected_memory.size() ?
				expected_memory[first_memory_mismatch] : 0u,
			memory[107u * 4u + 0u], memory[107u * 4u + 1u],
			memory[107u * 4u + 2u], memory[107u * 4u + 3u],
			expected_memory[107u * 4u + 0u],
			expected_memory[107u * 4u + 1u],
			expected_memory[107u * 4u + 2u],
			expected_memory[107u * 4u + 3u],
			memory[108u * 4u + 0u], memory[108u * 4u + 1u],
			memory[108u * 4u + 2u], memory[108u * 4u + 3u],
			expected_memory[108u * 4u + 0u],
			expected_memory[108u * 4u + 1u],
			expected_memory[108u * 4u + 2u],
			expected_memory[108u * 4u + 3u],
			memory[109u * 4u + 0u], memory[109u * 4u + 1u],
			memory[109u * 4u + 2u], memory[109u * 4u + 3u],
			expected_memory[109u * 4u + 0u],
			expected_memory[109u * 4u + 1u],
			expected_memory[109u * 4u + 2u],
			expected_memory[109u * 4u + 3u]);
		Console.Error(
			"GPU-VU: universal ERSADD diagnostics "
			"fs=%u source=%08x:%08x:%08x sum=%08x index=%u field=%u "
			"seed=%08x product=%08x step=%08x result=%08x.",
			state[48], state[49], state[50], state[51], state[52],
			state[53], state[54], state[55], state[56], state[57], state[58]);
		Console.Error(
			"GPU-VU: universal ITOF results "
			"itof0=%08x:%08x:%08x:%08x "
			"itof4=%08x:%08x:%08x:%08x "
			"itof12=%08x:%08x:%08x:%08x "
			"itof15=%08x:%08x:%08x:%08x.",
			vf[20], vf[21], vf[22], vf[23],
			vf[24], vf[25], vf[26], vf[27],
			vf[28], vf[29], vf[30], vf[31],
			vf[32], vf[33], vf[34], vf[35]);
		Console.Error(
			"GPU-VU: universal accumulator results "
			"vf29=%08x:%08x:%08x:%08x "
			"vf30=%08x:%08x:%08x:%08x "
			"vf31=%08x:%08x:%08x:%08x "
			"acc=%08x:%08x:%08x:%08x mac=%04x status=%04x mode=%08x "
			"clip=%06x.",
			vf[29u * 4u + 0u], vf[29u * 4u + 1u],
			vf[29u * 4u + 2u], vf[29u * 4u + 3u],
			vf[30u * 4u + 0u], vf[30u * 4u + 1u],
			vf[30u * 4u + 2u], vf[30u * 4u + 3u],
			vf[31u * 4u + 0u], vf[31u * 4u + 1u],
			vf[31u * 4u + 2u], vf[31u * 4u + 3u],
			vf[32u * 4u + 0u], vf[32u * 4u + 1u],
			vf[32u * 4u + 2u], vf[32u * 4u + 3u],
			state[42], state[43], state[44], state[45]);
		for (u32 source = 0;
			source < UNIVERSAL_VALIDATION_FTOI_SOURCES.size(); source++)
		{
			for (u32 variant = 0; variant < 4; variant++)
			{
				const u32 base_word =
					(13u + source * 4u + variant) * 4u;
				Console.Error(
					"GPU-VU: universal FTOI result source=%u variant=%u "
					"value=%08x:%08x:%08x:%08x.",
					source, variant, vf[base_word + 0], vf[base_word + 1],
					vf[base_word + 2], vf[base_word + 3]);
			}
		}
		// A failed one-shot comparison owns the same GXM lifetime as a passed
		// comparison.  The coarse Finish() above has drained the submission, so
		// release its patched programs and mapped buffers before ordinary GS work;
		// otherwise a useful diagnostic failure can also starve later render-target
		// creation and turn the continuation screen black.
		if (!ReleaseUniversalGpuVuValidationResources())
		{
			Console.Error(
				"GPU-VU: failed to release universal validation resources after "
				"a comparison mismatch.");
		}
		return false;
	}
	// This is a one-shot validation owner. The coarse Finish() above proves
	// that neither the patched programs nor their mapped uniform/output state
	// remain referenced by queued GPU work. Release them before ordinary GS
	// rendering starts so the oracle cannot reduce the product render-target
	// budget merely by having run once.
	if (!ReleaseUniversalGpuVuValidationResources())
	{
		gpu_vu_universal_validation_stage =
			UniversalGpuVuValidationStage::Failed;
		return false;
	}
	if (!WriteUniversalGpuVuValidationReceipt())
	{
		gpu_vu_universal_validation_stage =
			UniversalGpuVuValidationStage::Failed;
		Console.Error(
			"GPU-VU: universal validation passed GPU comparison but failed "
			"to publish the synchronized result receipt.");
		return false;
	}
		gpu_vu_universal_validation_stage =
			UniversalGpuVuValidationStage::Passed;
		Console.WriteLn(
			"GPU-VU: universal VIF/VU command-epoch validation passed "
			"(two ordered GPU executions, 142-pair explicit plus three-pair MSCNT "
			"resume, 145 executed/source pairs, XTOP/XITOP from serialized VIF state, "
			"six UNPACKs per invocation, "
			"four ITOF and 16 FTOI edge "
			"vectors, 13 ACC/FMAC/outer-product operations with MAC/STATUS, "
			"immutable non-instant Q/P config, old Q/P consumers exported before "
			"WAITQ/WAITP, finite DIV/SQRT/RSQRT Q consumed in three WAITQ upper "
			"pairs, delayed SQRT I and final RSQRT D+I STATUS snapshots through "
			"FSAND, terminal RSQRT Q publication, ESADD P=14.0, WAITP, and two "
			"MFP P observations, ARM-table ERSADD(14)=0x3d924900 and "
			"ESQRT(10)=0x404a6244 through WAITP/MFP/SQ, "
			"two raw-word CLIP operations with rolling history, one exact "
			"fixed-pipeline timing check, one delayed VU-memory "
			"export, two raw PATH1 packets, zero XGKICK waits, one coarse "
			"completion, one Sony-required mid-scene writable-buffer flush, "
			"validation resources released).");
	return true;
}

bool GSDeviceGXM::Impl::ReleaseUniversalGpuVuValidationResources()
{
	if (gpu_vu_universal_fragment_program)
	{
		const int result = sceGxmShaderPatcherReleaseFragmentProgram(
			patcher, gpu_vu_universal_fragment_program);
		if (result < 0)
			return Fail("release universal GPU-VU validation fragment program",
				result);
		gpu_vu_universal_fragment_program = nullptr;
	}
	if (gpu_vu_universal_vertex_program)
	{
		const int result = sceGxmShaderPatcherReleaseVertexProgram(
			patcher, gpu_vu_universal_vertex_program);
		if (result < 0)
			return Fail("release universal GPU-VU validation vertex program",
				result);
		gpu_vu_universal_vertex_program = nullptr;
	}
	if (gpu_vu_universal_fragment_id)
	{
		const int result = sceGxmShaderPatcherUnregisterProgram(
			patcher, gpu_vu_universal_fragment_id);
		if (result < 0)
			return Fail("unregister universal GPU-VU validation fragment program",
				result);
		gpu_vu_universal_fragment_id = nullptr;
	}
	if (gpu_vu_universal_vertex_id)
	{
		const int result = sceGxmShaderPatcherUnregisterProgram(
			patcher, gpu_vu_universal_vertex_id);
		if (result < 0)
			return Fail("unregister universal GPU-VU validation vertex program",
				result);
		gpu_vu_universal_vertex_id = nullptr;
	}
	if (gpu_vu_universal_validation_buffer.IsAllocated())
	{
		const int result = VitaGXM::ReleaseMappedBlock(
			&gpu_vu_universal_validation_buffer);
		if (result < 0)
			return Fail("release universal GPU-VU validation state", result);
	}
	return true;
}

bool GSDeviceGXM::Impl::AttestUniversalGpuVuStructuredFmac()
{
	constexpr u32 ActiveInvocations = 16u;
	constexpr u32 GuardInvocations = 1u;
	constexpr u32 OperationCount = 5u;
	constexpr u32 MaximumScratchInvocation =
		VitaGpuVu::StructuredGeneratedMaximumScratchInvocations - 1u;
	constexpr u32 MaximumScratchWord =
		VitaGpuVu::StructuredGeneratedMaximumScratchInvocations *
			VitaGpuVu::StructuredGeneratedScratchSlots - 1u;
	gpu_vu_structured_fmac_exact_attested = false;
	gpu_vu_structured_scratch_range_attested = false;
	gpu_vu_structured_fmac_native_attestation =
		StructuredNativeFmacAttestation::Unavailable;
	if (!context || !gpu_vu_sequential_indices ||
		!gpu_vu_universal_product_target ||
		!gpu_vu_universal_product_color_surface ||
		!gpu_vu_universal_product_color_surface->ColorSurface() ||
		gpu_vu_universal_product_slots.empty() ||
		!gpu_vu_universal_product_slots[0].buffer.IsMapped() ||
		!gpu_vu_structured_fmac_numeric_product_vertex_program ||
		!gpu_vu_structured_fmac_numeric_product_fragment_program ||
		!gpu_vu_structured_fmac_native_product_vertex_program ||
		!gpu_vu_structured_fmac_native_product_fragment_program)
	{
		return false;
	}

	const auto write_receipt = [=](const char* status, bool exact,
			const char* native_model, bool guard, bool scratch_range,
			u32 exact_mismatch,
			u32 native_nearest_mismatch, u32 native_finite_chop_mismatch,
			u32 nearest_actual, u32 nearest_expected,
			u32 finite_chop_actual, u32 finite_chop_expected,
			u32 range_actual, u32 range_expected) {
		char receipt[768]{};
		const int length = std::snprintf(receipt, sizeof(receipt),
			"v=3 status=%s exact=%u native=%u native_model=%s "
			"invocations=%u operations=%u guard_rows=%u guards=%u "
			"scratch_range=%u max_scratch_invocation=%u max_scratch_word=%u "
			"scratch_guard_words=%u "
			"exact_first_mismatch=%u "
			"native_nearest_first_mismatch=%u "
			"native_finite_chop_first_mismatch=%u "
			"nearest_actual=0x%08x nearest_expected=0x%08x "
			"finite_chop_actual=0x%08x finite_chop_expected=0x%08x "
			"range_actual=0x%08x range_expected=0x%08x "
			"private_transaction=1 "
			"completion_boundaries=1\n",
			status, exact ? 1u : 0u,
			std::strcmp(native_model, "unavailable") != 0 ? 1u : 0u,
			native_model, ActiveInvocations, OperationCount, GuardInvocations,
			guard ? 1u : 0u, scratch_range ? 1u : 0u,
			MaximumScratchInvocation, MaximumScratchWord,
			GPU_VU_STRUCTURED_SCRATCH_GUARD_WORDS,
			exact_mismatch, native_nearest_mismatch,
			native_finite_chop_mismatch, nearest_actual, nearest_expected,
			finite_chop_actual, finite_chop_expected,
			range_actual, range_expected);
		if (length <= 0 || static_cast<size_t>(length) >= sizeof(receipt))
			return false;
		const SceUID fd = sceIoOpen(STRUCTURED_FMAC_ATTESTATION_RECEIPT_PATH,
			SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
		if (fd < 0)
			return false;
		size_t written = 0;
		bool okay = true;
		while (written < static_cast<size_t>(length))
		{
			const SceSSize result = sceIoWrite(fd, receipt + written,
				static_cast<SceSize>(length - written));
			if (result <= 0)
			{
				okay = false;
				break;
			}
			written += static_cast<size_t>(result);
		}
		if (okay && sceIoSyncByFd(fd, 0) < 0)
			okay = false;
		if (sceIoClose(fd) < 0)
			okay = false;
		return okay;
	};

	const u32 exact_configuration =
		VitaGpuVu::GetCurrentUniversalMicroProgramConfigurationBits() &
		~VitaGpuVu::UniversalConfigurationApproximateFmac;
	const u32 native_configuration = exact_configuration |
		VitaGpuVu::UniversalConfigurationApproximateFmac;
	if (!VitaGpuVu::IsGeneratedCgSoftwareF32ConfigurationSupported(
			exact_configuration) ||
		!VitaGpuVu::IsGeneratedCgNativeF32ConfigurationSupported(
			native_configuration))
	{
		const bool receipt_written = write_receipt("unsupported-configuration",
			false, "unavailable", true, false, 0u, 0u, 0u, 0u, 0u,
			0u, 0u, 0u, 0u);
		Console.Warning(
			"GPU-VU: structured FMAC physical attestation skipped for unsupported "
			"numeric configuration exact=%08x native=%08x receipt=%u; "
			"fixed/CPU pre-effect fallback remains authoritative.",
			exact_configuration, native_configuration,
			receipt_written ? 1u : 0u);
		return receipt_written;
	}

	VitaGpuVu::StructuredFixedFmacNumericDescriptor exact_descriptor;
	exact_descriptor.configuration_bits = exact_configuration;
	exact_descriptor.operation_count = OperationCount;
	exact_descriptor.initial_scratch_mask[0] = 0x00000003u;
	const auto scratch_operand = [](u32 slot) {
		return VitaGpuVu::StructuredFixedFmacOperand{
			VitaGpuVu::StructuredFixedFmacOperandKind::Scratch, slot};
	};
	const auto immediate_operand = [](u32 bits) {
		return VitaGpuVu::StructuredFixedFmacOperand{
			VitaGpuVu::StructuredFixedFmacOperandKind::Immediate, bits};
	};
	const auto set_operation = [&](u32 index,
			VitaGpuVu::StructuredFixedFmacNumericOperation operation,
			u32 output_slot, VitaGpuVu::StructuredFixedFmacOperand left,
			VitaGpuVu::StructuredFixedFmacOperand right) {
		auto& record = exact_descriptor.operations[index];
		record.operation = operation;
		record.output_slot = output_slot;
		record.left = left;
		record.right = right;
	};
	set_operation(0u, VitaGpuVu::StructuredFixedFmacNumericOperation::Add,
		2u, scratch_operand(0u), scratch_operand(1u));
	set_operation(1u, VitaGpuVu::StructuredFixedFmacNumericOperation::Subtract,
		3u, scratch_operand(2u), immediate_operand(0x3f800000u));
	set_operation(2u, VitaGpuVu::StructuredFixedFmacNumericOperation::Multiply,
		4u, scratch_operand(0u), scratch_operand(1u));
	set_operation(3u, VitaGpuVu::StructuredFixedFmacNumericOperation::Multiply,
		5u, scratch_operand(2u), immediate_operand(0x40000000u));
	set_operation(4u, VitaGpuVu::StructuredFixedFmacNumericOperation::Add,
		6u, scratch_operand(4u), scratch_operand(5u));
	VitaGpuVu::StructuredFixedFmacNumericDescriptor native_descriptor =
		exact_descriptor;
	native_descriptor.configuration_bits = native_configuration;
	VitaGpuVu::StructuredFixedFmacNumericDescriptor range_descriptor;
	range_descriptor.configuration_bits = native_configuration;
	range_descriptor.operation_count = 1u;
	range_descriptor.operations[0].operation =
		VitaGpuVu::StructuredFixedFmacNumericOperation::Add;
	range_descriptor.operations[0].output_slot = 127u;
	range_descriptor.operations[0].left = immediate_operand(0x3f800000u);
	range_descriptor.operations[0].right = immediate_operand(0x40000000u);
	if (!exact_descriptor.IsValid() || !native_descriptor.IsValid() ||
		!range_descriptor.IsValid())
		return false;

	constexpr u32 ScratchSlots = VitaGpuVu::StructuredGeneratedScratchSlots;
	constexpr u32 AttestationScratchWords =
		(ActiveInvocations + GuardInvocations) * ScratchSlots;
	using AttestationScratch = std::array<u32, AttestationScratchWords>;
	static_assert(2u * sizeof(AttestationScratch) <=
		GPU_VU_STRUCTURED_JOURNAL_VALUE_BYTES);
	AttestationScratch seed{};
	for (u32 index = 0; index < seed.size(); index++)
		seed[index] = 0xa5000000u | index;
	// The first row distinguishes nearest-even from finite mantissa chop. The
	// max-minus-one rows distinguish finite SGX alignment from an idealized
	// infinite-precision round-toward-zero model. Remaining rows cover DAZ/FZ,
	// signed zero, cancellation, ordinary products, NaN/Inf input clamping,
	// negative operands, and positive/negative overflow.
	constexpr std::array<std::array<u32, 2>, ActiveInvocations> inputs = {{
		{{0x3f800000u, 0x34400000u}},
		{{0x00000001u, 0x3f800000u}},
		{{0x7f7fffffu, 0x3f800000u}},
		{{0xff7fffffu, 0x3f800000u}},
		{{0x3f800000u, 0x00800000u}},
		{{0xbf800000u, 0x34400000u}},
		{{0x3f000000u, 0x33800000u}},
		{{0x3f7fffffu, 0x33800000u}},
		{{0x00000000u, 0x80000000u}},
		{{0x7f800000u, 0x3f800000u}},
		{{0x7fc00001u, 0x3f800000u}},
		{{0xff800000u, 0x3f800000u}},
		{{0x3f8ccccdu, 0x3fa66666u}},
		{{0x3f800000u, 0xbf800000u}},
		{{0x00800000u, 0x3f000000u}},
		{{0x7f7fffffu, 0x3f800001u}},
	}};
	for (u32 invocation = 0; invocation < ActiveInvocations; invocation++)
	{
		seed[invocation * ScratchSlots + 0u] = inputs[invocation][0];
		seed[invocation * ScratchSlots + 1u] = inputs[invocation][1];
	}
	AttestationScratch expected_exact = seed;
	AttestationScratch expected_native_nearest = seed;
	AttestationScratch expected_native_finite_chop = seed;
	if (!VitaGpuVu::ApplyStructuredFixedFmacNumericReference(
			exact_descriptor, ActiveInvocations, expected_exact.data(),
			expected_exact.size()) ||
		!VitaGpuVu::ApplyStructuredFixedFmacNumericReference(
			exact_descriptor, ActiveInvocations, expected_native_nearest.data(),
			expected_native_nearest.size()) ||
		!VitaGpuVu::ApplyStructuredFixedFmacNumericReference(
			native_descriptor, ActiveInvocations,
			expected_native_finite_chop.data(),
			expected_native_finite_chop.size()))
	{
		return false;
	}

	u8* const base = static_cast<u8*>(
		gpu_vu_universal_product_slots[0].buffer.base);
	auto* const descriptors = reinterpret_cast<
		VitaGpuVu::StructuredFixedFmacNumericDescriptor*>(
			base + GPU_VU_UNIVERSAL_PRODUCT_FMAC_DESCRIPTOR_OFFSET);
	descriptors[0] = exact_descriptor;
	descriptors[1] = native_descriptor;
	descriptors[2] = range_descriptor;
	u32* const exact_actual = reinterpret_cast<u32*>(
		base + GPU_VU_UNIVERSAL_PRODUCT_JOURNAL_VALUE_OFFSET);
	u32* const native_actual = exact_actual + AttestationScratchWords;
	u32* const scratch_guard = reinterpret_cast<u32*>(
		base + GPU_VU_UNIVERSAL_PRODUCT_JOURNAL_VALUE_OFFSET +
			GPU_VU_STRUCTURED_SCRATCH_BYTES);
	std::memcpy(exact_actual, seed.data(), sizeof(seed));
	std::memcpy(native_actual, seed.data(), sizeof(seed));
	exact_actual[MaximumScratchWord] = 0xdead4f21u;
	std::fill_n(scratch_guard, GPU_VU_STRUCTURED_SCRATCH_GUARD_WORDS,
		GPU_VU_STRUCTURED_SCRATCH_GUARD_VALUE);

	SceGxmDepthStencilSurface attestation_depth{};
	int result = sceGxmDepthStencilSurfaceInitDisabled(&attestation_depth);
	if (result < 0)
		return Fail("initialize structured FMAC attestation depth", result);
	result = sceGxmBeginScene(context, 0,
		gpu_vu_universal_product_target, nullptr, nullptr, nullptr,
		gpu_vu_universal_product_color_surface->ColorSurface(),
		&attestation_depth);
	if (result < 0)
		return Fail("begin structured FMAC physical attestation", result);
	scene_active = true;
	scene_is_display = false;
	scene_rt = nullptr;
	scene_ds = nullptr;
	ConfigureRaster(1u, 1u);
	sceGxmSetFrontDepthFunc(context, SCE_GXM_DEPTH_FUNC_ALWAYS);
	sceGxmSetBackDepthFunc(context, SCE_GXM_DEPTH_FUNC_ALWAYS);
	sceGxmSetFrontDepthWriteEnable(context, SCE_GXM_DEPTH_WRITE_DISABLED);
	sceGxmSetBackDepthWriteEnable(context, SCE_GXM_DEPTH_WRITE_DISABLED);
	ConfigureGpuVuComputeRaster(context);

	const auto submit = [&](SceGxmVertexProgram* vertex,
			SceGxmFragmentProgram* fragment, const void* descriptor,
			void* scratch, const u16* indices, u32 invocation_count) {
		sceGxmSetVertexProgram(context, vertex);
		sceGxmSetFragmentProgram(context, fragment);
		int submit_result = sceGxmSetVertexUniformBuffer(context, 0, descriptor);
		if (submit_result >= 0)
			submit_result = sceGxmSetVertexUniformBuffer(context, 10, scratch);
		if (submit_result >= 0)
		{
			submit_result = sceGxmDraw(context, SCE_GXM_PRIMITIVE_POINTS,
				SCE_GXM_INDEX_FORMAT_U16, indices, invocation_count);
		}
		return submit_result;
	};
	result = submit(gpu_vu_structured_fmac_numeric_product_vertex_program,
		gpu_vu_structured_fmac_numeric_product_fragment_program,
		&descriptors[0], exact_actual, gpu_vu_sequential_indices,
		ActiveInvocations);
	if (result >= 0)
		result = sceGxmMidSceneFlush(context, 0, nullptr, nullptr);
	if (result >= 0)
	{
		result = submit(gpu_vu_structured_fmac_native_product_vertex_program,
			gpu_vu_structured_fmac_native_product_fragment_program,
			&descriptors[1], native_actual, gpu_vu_sequential_indices,
			ActiveInvocations);
	}
	if (result >= 0)
		result = sceGxmMidSceneFlush(context, 0, nullptr, nullptr);
	if (result >= 0)
	{
		// One invocation at the maximum legal INDEX exercises BUFFER10's
		// complete 2 MiB physical address range without paying for a 4096-point
		// arithmetic draw. The following cache-line canary detects an inclusive-
		// bound or slot-stride error before structured product admission opens.
		result = submit(gpu_vu_structured_fmac_native_product_vertex_program,
			gpu_vu_structured_fmac_native_product_fragment_program,
			&descriptors[2], exact_actual,
			gpu_vu_sequential_indices + MaximumScratchInvocation, 1u);
	}
	RestoreGpuVuComputeRaster(context);
	if (result < 0)
	{
		EndScene(false);
		sceGxmFinish(context);
		return Fail("submit structured FMAC physical attestation", result);
	}
	if (!Finish())
		return false;

	const auto first_mismatch = [](const u32* actual,
			const AttestationScratch& expected) {
		for (u32 index = 0; index < expected.size(); index++)
		{
			if (actual[index] != expected[index])
				return index;
		}
		return static_cast<u32>(expected.size());
	};
	const u32 exact_mismatch = first_mismatch(exact_actual, expected_exact);
	const u32 native_nearest_mismatch = first_mismatch(
		native_actual, expected_native_nearest);
	const u32 native_finite_chop_mismatch = first_mismatch(
		native_actual, expected_native_finite_chop);
	const bool exact_ok = exact_mismatch == AttestationScratchWords;
	const bool native_nearest_ok =
		native_nearest_mismatch == AttestationScratchWords;
	const bool native_finite_chop_ok =
		native_finite_chop_mismatch == AttestationScratchWords;
	const u32 guard_begin = ActiveInvocations * ScratchSlots;
	const bool guards_ok =
		std::equal(exact_actual + guard_begin,
			exact_actual + AttestationScratchWords, seed.data() + guard_begin) &&
		std::equal(native_actual + guard_begin,
			native_actual + AttestationScratchWords, seed.data() + guard_begin);
	constexpr u32 RangeExpected = 0x40400000u;
	const u32 range_actual = exact_actual[MaximumScratchWord];
	const bool scratch_guard_ok = std::all_of(scratch_guard,
		scratch_guard + GPU_VU_STRUCTURED_SCRATCH_GUARD_WORDS,
		[](u32 value) {
			return value == GPU_VU_STRUCTURED_SCRATCH_GUARD_VALUE;
		});
	gpu_vu_structured_scratch_range_attested =
		range_actual == RangeExpected && scratch_guard_ok;
	gpu_vu_structured_fmac_exact_attested = exact_ok && guards_ok;
	if (gpu_vu_structured_fmac_exact_attested && native_nearest_ok)
	{
		gpu_vu_structured_fmac_native_attestation =
			StructuredNativeFmacAttestation::NearestEven;
	}
	else if (gpu_vu_structured_fmac_exact_attested && native_finite_chop_ok)
	{
		gpu_vu_structured_fmac_native_attestation =
			StructuredNativeFmacAttestation::FiniteMantissaChop;
	}
	const char* const native_model =
		gpu_vu_structured_fmac_native_attestation ==
			StructuredNativeFmacAttestation::NearestEven ? "nearest-even" :
		gpu_vu_structured_fmac_native_attestation ==
			StructuredNativeFmacAttestation::FiniteMantissaChop ?
			"finite-mantissa-chop" :
		"unavailable";
	const bool native_ok = gpu_vu_structured_fmac_native_attestation !=
		StructuredNativeFmacAttestation::Unavailable;
	const u32 nearest_diagnostic_index =
		native_nearest_mismatch < AttestationScratchWords ?
			native_nearest_mismatch : 2u;
	const u32 finite_chop_diagnostic_index =
		native_finite_chop_mismatch < AttestationScratchWords ?
			native_finite_chop_mismatch : 2u;
	const bool receipt_written = write_receipt(
		gpu_vu_structured_fmac_exact_attested && native_ok &&
			gpu_vu_structured_scratch_range_attested ? "passed" : "failed",
		gpu_vu_structured_fmac_exact_attested, native_model, guards_ok,
		gpu_vu_structured_scratch_range_attested,
		exact_mismatch, native_nearest_mismatch,
		native_finite_chop_mismatch,
		native_actual[nearest_diagnostic_index],
		expected_native_nearest[nearest_diagnostic_index],
		native_actual[finite_chop_diagnostic_index],
		expected_native_finite_chop[finite_chop_diagnostic_index],
		range_actual, RangeExpected);
	Console.WriteLn(
		"GPU-VU: structured FMAC physical attestation exact=%u native=%u "
		"native_model=%s guards=%u scratch_range=%u max_scratch_word=%u "
		"range_actual=%08x range_expected=%08x invocations=%u operations=%u "
		"exact_mismatch=%u native_nearest_mismatch=%u "
		"native_finite_chop_mismatch=%u receipt=%u.",
		gpu_vu_structured_fmac_exact_attested ? 1u : 0u,
		native_ok ? 1u : 0u, native_model, guards_ok ? 1u : 0u,
		gpu_vu_structured_scratch_range_attested ? 1u : 0u,
		MaximumScratchWord, range_actual, RangeExpected,
		ActiveInvocations, exact_descriptor.operation_count, exact_mismatch,
		native_nearest_mismatch, native_finite_chop_mismatch,
		receipt_written ? 1u : 0u);
	return receipt_written;
}

bool GSDeviceGXM::Impl::InitializeGeneratedGpuVuProduct()
{
	if (!patcher || gpu_vu_universal_product_ready)
		return false;

	// HostSys owns the cached USER_RW data arena for the process lifetime.
	// libGXM permits one page-aligned read-only mapping of its VU slice and an
	// interior BUFFER pointer. Keep unchanged VU1 qwords in that canonical owner
	// instead of making CPU1 rebuild tens of MiB of duplicate input tables.
	static_assert((HostMemoryMap::VUmemSize & 4095u) == 0u);
	u8* const canonical_vu_base = SysMemory::GetVUMem();
	const uptr canonical_begin = reinterpret_cast<uptr>(canonical_vu_base);
	const uptr canonical_end = canonical_begin + HostMemoryMap::VUmemSize;
	const uptr vu1_begin = reinterpret_cast<uptr>(VU1.Mem);
	if (!canonical_vu_base || (canonical_begin & 4095u) != 0u ||
		!VU1.Mem || (vu1_begin & 4095u) != 0u ||
		vu1_begin < canonical_begin ||
		vu1_begin + VU1_MEMSIZE > canonical_end)
	{
		return Fail("validate canonical VU1 GXM mapping",
			SCE_GXM_ERROR_INVALID_ALIGNMENT);
	}
	int result = sceGxmMapMemory(canonical_vu_base,
		HostMemoryMap::VUmemSize, SCE_GXM_MEMORY_ATTRIB_READ);
	if (result < 0)
		return Fail("map canonical VU1 memory for generated GPU-VU", result);
	gpu_vu_canonical_vu_memory_mapped = true;
	Console.WriteLn(
		"GPU-VU: canonical VU1 input owner mapped once "
		"(base=%p bytes=%u vu1=%p bytes=%u access=read).",
		canonical_vu_base, HostMemoryMap::VUmemSize, VU1.Mem, VU1_MEMSIZE);

	// The only persistent data dependency of a runtime-generated direct root is
	// BUFFER7's Cortex-A9-compatible Q/P estimate table.  The fixed universal
	// machine is quarantined in ServiceUniversalGpuVuEpoch(), so reserving its
	// support GXPs, zero payload, two 2.8 MiB state generations, and a maximal
	// eight-scene render target here was pure cold-path cost.  On r201 it left
	// libGXM unable to allocate BSpline's first ordinary GS render target.
	result = VitaGXM::AllocateMappedBlock(
		"VitaSX2 generated GPU-VU QP estimates",
		SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_NC_RW,
		GPU_VU_UNIVERSAL_ESTIMATE_BYTES, SCE_GXM_MEMORY_ATTRIB_READ,
		&gpu_vu_generated_estimate_table);
	if (result < 0)
		return Fail("allocate generated GPU-VU Q/P estimate table", result);
	u32* const arm_estimates = static_cast<u32*>(
		gpu_vu_generated_estimate_table.base);
	for (u32 scaled = 256; scaled < 512; scaled++)
	{
		const u32 a = scaled * 2u + 1u;
		const u32 b = (1u << 19u) / a;
		arm_estimates[scaled - 256u] = ((b + 1u) >> 1u) & 0xffu;
	}
	for (u32 scaled = 128; scaled < 512; scaled++)
	{
		u32 a = scaled < 256u ? scaled * 2u + 1u :
			((((scaled >> 1u) << 1u) + 1u) * 2u);
		u32 b = 512u;
		while (a * (b + 1u) * (b + 1u) < (1u << 28u))
			b++;
		arm_estimates[256u + scaled - 128u] = ((b + 1u) >> 1u) & 0xffu;
	}

	gpu_vu_structured_fmac_exact_attested = false;
	gpu_vu_structured_scratch_range_attested = false;
	gpu_vu_structured_fmac_native_attestation =
		StructuredNativeFmacAttestation::Unavailable;
	gpu_vu_universal_product_ready = true;
	VitaGpuVu::SetUniversalGpuVuCompactProviderAvailable(false);
	VitaGpuVu::SetUniversalGpuVuDeviceAvailable(true);
	Console.WriteLn(
		"GPU-VU: generated direct owner ready (state_slots=0 "
		"offline_support_programs=0 dedicated_render_targets=0 "
		"estimate_bytes=%u "
		"cold_provider=cpu-mtvu fixed_interpreter=disabled "
		"structured_provider=disabled compiler=asynchronous-shacc "
		"output=direct-tfx transaction=bounded-cpu-journal).",
		GPU_VU_UNIVERSAL_ESTIMATE_BYTES);
	return true;
}

bool GSDeviceGXM::Impl::InitializeLegacyUniversalGpuVuValidationOwner()
{
	if (!patcher || gpu_vu_universal_product_ready)
		return false;

	int result = VitaGXM::AllocateMappedBlock(
		"VitaSX2 universal GPU-VU programs",
		SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_NC_RW,
		GPU_VU_UNIVERSAL_PRODUCT_SHARED_BYTES, SCE_GXM_MEMORY_ATTRIB_RW,
		&gpu_vu_universal_product_shared);
	if (result < 0)
		return Fail("allocate universal GPU-VU product programs", result);
	std::memset(gpu_vu_universal_product_shared.base, 0,
		gpu_vu_universal_product_shared.size);

	u8* const gxp_bytes = static_cast<u8*>(
		gpu_vu_universal_product_shared.base);
	const auto load_gxp = [&](const char* path, u32 offset, u32 capacity,
			const char* operation) -> bool {
		const SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
		if (fd < 0)
			return Fail(operation, fd);
		const SceOff size = sceIoLseek(fd, 0, SCE_SEEK_END);
		if (size <= 0 || static_cast<u64>(size) > capacity ||
			sceIoLseek(fd, 0, SCE_SEEK_SET) < 0)
		{
			sceIoClose(fd);
			return Fail(operation, SCE_GXM_ERROR_INVALID_VALUE);
		}
		SceOff total = 0;
		while (total < size)
		{
			const SceSSize read_result = sceIoRead(fd,
				gxp_bytes + offset + total,
				static_cast<SceSize>(size - total));
			if (read_result <= 0)
			{
				sceIoClose(fd);
				return Fail(operation, read_result < 0 ?
					static_cast<int>(read_result) :
					SCE_GXM_ERROR_INVALID_VALUE);
			}
			total += read_result;
		}
		const int close_result = sceIoClose(fd);
		return close_result >= 0 ? true : Fail(operation, close_result);
	};
	if (!load_gxp(GPU_VU_UNIVERSAL_GXP_PATH,
			GPU_VU_UNIVERSAL_GXP_OFFSET,
			GPU_VU_UNIVERSAL_GXP_MAX_BYTES,
			"load universal GPU-VU product GXP") ||
		!load_gxp(GPU_VU_UNIVERSAL_COMPACT_GXP_PATH,
			GPU_VU_UNIVERSAL_COMPACT_GXP_OFFSET,
			GPU_VU_UNIVERSAL_COMPACT_GXP_MAX_BYTES,
			"load compact universal GPU-VU product GXP") ||
		!load_gxp(GPU_VU_VIF_UNPACK_GXP_PATH,
				GPU_VU_VIF_UNPACK_GXP_OFFSET,
				GPU_VU_VIF_UNPACK_GXP_MAX_BYTES,
				"load universal GPU-VU VIF UNPACK GXP") ||
		!load_gxp(GPU_VU_VIF_UNPACK_INDEPENDENT_GXP_PATH,
				GPU_VU_VIF_UNPACK_INDEPENDENT_GXP_OFFSET,
				GPU_VU_VIF_UNPACK_INDEPENDENT_GXP_MAX_BYTES,
				"load independent GPU-VU VIF UNPACK GXP") ||
		!load_gxp(GPU_VU_STRUCTURED_CONTROL_GXP_PATH,
				GPU_VU_STRUCTURED_CONTROL_GXP_OFFSET,
				GPU_VU_STRUCTURED_CONTROL_GXP_MAX_BYTES,
				"load fixed structured GPU-VU control GXP") ||
		!load_gxp(GPU_VU_STRUCTURED_STATE_GXP_PATH,
				GPU_VU_STRUCTURED_STATE_GXP_OFFSET,
				GPU_VU_STRUCTURED_STATE_GXP_MAX_BYTES,
				"load fixed structured GPU-VU state GXP") ||
		!load_gxp(GPU_VU_STRUCTURED_PREFLIGHT_BUILD_GXP_PATH,
			GPU_VU_STRUCTURED_PREFLIGHT_BUILD_GXP_OFFSET,
			GPU_VU_STRUCTURED_PREFLIGHT_BUILD_GXP_MAX_BYTES,
			"load structured GPU-VU memory preflight build GXP") ||
		!load_gxp(GPU_VU_STRUCTURED_PREFLIGHT_REDUCE_GXP_PATH,
			GPU_VU_STRUCTURED_PREFLIGHT_REDUCE_GXP_OFFSET,
			GPU_VU_STRUCTURED_PREFLIGHT_REDUCE_GXP_MAX_BYTES,
			"load structured GPU-VU memory preflight reduce GXP") ||
		!load_gxp(GPU_VU_STRUCTURED_PREFLIGHT_FINALIZE_GXP_PATH,
			GPU_VU_STRUCTURED_PREFLIGHT_FINALIZE_GXP_OFFSET,
			GPU_VU_STRUCTURED_PREFLIGHT_FINALIZE_GXP_MAX_BYTES,
			"load structured GPU-VU memory preflight finalize GXP") ||
		!load_gxp(GPU_VU_PATH1_COMMIT_GXP_PATH,
			GPU_VU_PATH1_COMMIT_GXP_OFFSET,
			GPU_VU_PATH1_COMMIT_GXP_MAX_BYTES,
			"load universal GPU-VU terminal PATH1 commit GXP") ||
		!load_gxp(GPU_VU_STRUCTURED_QP_NUMERIC_GXP_PATH,
			GPU_VU_STRUCTURED_QP_NUMERIC_GXP_OFFSET,
			GPU_VU_STRUCTURED_QP_NUMERIC_GXP_MAX_BYTES,
			"load structured GPU-VU fixed Q/P numeric GXP") ||
		!load_gxp(GPU_VU_STRUCTURED_FMAC_NUMERIC_GXP_PATH,
			GPU_VU_STRUCTURED_FMAC_NUMERIC_GXP_OFFSET,
			GPU_VU_STRUCTURED_FMAC_NUMERIC_GXP_MAX_BYTES,
			"load structured GPU-VU exact FMAC numeric GXP") ||
		!load_gxp(GPU_VU_STRUCTURED_FMAC_NATIVE_GXP_PATH,
			GPU_VU_STRUCTURED_FMAC_NATIVE_GXP_OFFSET,
			GPU_VU_STRUCTURED_FMAC_NATIVE_GXP_MAX_BYTES,
			"load structured GPU-VU native FMAC numeric GXP"))
	{
		return false;
	}

	const SceGxmProgram* const vertex =
		reinterpret_cast<const SceGxmProgram*>(gxp_bytes);
	const SceGxmProgram* const compact_vertex =
		reinterpret_cast<const SceGxmProgram*>(
			gxp_bytes + GPU_VU_UNIVERSAL_COMPACT_GXP_OFFSET);
	const SceGxmProgram* const vif_unpack_vertex =
		reinterpret_cast<const SceGxmProgram*>(
			gxp_bytes + GPU_VU_VIF_UNPACK_GXP_OFFSET);
	const SceGxmProgram* const vif_unpack_independent_vertex =
		reinterpret_cast<const SceGxmProgram*>(
			gxp_bytes + GPU_VU_VIF_UNPACK_INDEPENDENT_GXP_OFFSET);
	const SceGxmProgram* const structured_control_vertex =
		reinterpret_cast<const SceGxmProgram*>(
			gxp_bytes + GPU_VU_STRUCTURED_CONTROL_GXP_OFFSET);
	const SceGxmProgram* const structured_state_vertex =
		reinterpret_cast<const SceGxmProgram*>(
			gxp_bytes + GPU_VU_STRUCTURED_STATE_GXP_OFFSET);
	const SceGxmProgram* const structured_preflight_build_vertex =
		reinterpret_cast<const SceGxmProgram*>(
			gxp_bytes + GPU_VU_STRUCTURED_PREFLIGHT_BUILD_GXP_OFFSET);
	const SceGxmProgram* const structured_preflight_reduce_vertex =
		reinterpret_cast<const SceGxmProgram*>(
			gxp_bytes + GPU_VU_STRUCTURED_PREFLIGHT_REDUCE_GXP_OFFSET);
	const SceGxmProgram* const structured_preflight_finalize_vertex =
		reinterpret_cast<const SceGxmProgram*>(
			gxp_bytes + GPU_VU_STRUCTURED_PREFLIGHT_FINALIZE_GXP_OFFSET);
	const SceGxmProgram* const path1_commit_vertex =
		reinterpret_cast<const SceGxmProgram*>(
			gxp_bytes + GPU_VU_PATH1_COMMIT_GXP_OFFSET);
	const SceGxmProgram* const structured_qp_numeric_vertex =
		reinterpret_cast<const SceGxmProgram*>(
			gxp_bytes + GPU_VU_STRUCTURED_QP_NUMERIC_GXP_OFFSET);
	const SceGxmProgram* const structured_fmac_numeric_vertex =
		reinterpret_cast<const SceGxmProgram*>(
			gxp_bytes + GPU_VU_STRUCTURED_FMAC_NUMERIC_GXP_OFFSET);
	const SceGxmProgram* const structured_fmac_native_vertex =
		reinterpret_cast<const SceGxmProgram*>(
			gxp_bytes + GPU_VU_STRUCTURED_FMAC_NATIVE_GXP_OFFSET);
	const SceGxmProgram* const fragment =
		&_binary_vitasx2_gpu_vu_universal_f_gxp_start;
	result = sceGxmProgramCheck(vertex);
	if (result < 0)
		return Fail("validate universal GPU-VU product GXP header/version", result);
	result = sceGxmProgramCheck(compact_vertex);
	if (result < 0)
		return Fail("validate compact GPU-VU product GXP header/version", result);
	result = sceGxmProgramCheck(vif_unpack_vertex);
	if (result < 0)
		return Fail("validate GPU-VU VIF UNPACK GXP header/version", result);
	result = sceGxmProgramCheck(vif_unpack_independent_vertex);
	if (result < 0)
		return Fail("validate independent GPU-VU VIF UNPACK GXP header/version",
			result);
	result = sceGxmProgramCheck(structured_control_vertex);
	if (result < 0)
		return Fail("validate structured GPU-VU control GXP header/version", result);
	result = sceGxmProgramCheck(structured_state_vertex);
	if (result < 0)
		return Fail("validate structured GPU-VU state GXP header/version", result);
	result = sceGxmProgramCheck(structured_preflight_build_vertex);
	if (result < 0)
		return Fail("validate GPU-VU preflight-build GXP header/version",
			result);
	result = sceGxmProgramCheck(structured_preflight_reduce_vertex);
	if (result < 0)
		return Fail("validate GPU-VU preflight-reduce GXP header/version",
			result);
	result = sceGxmProgramCheck(structured_preflight_finalize_vertex);
	if (result < 0)
		return Fail("validate GPU-VU preflight-finalize GXP header/version",
			result);
	result = sceGxmProgramCheck(path1_commit_vertex);
	if (result < 0)
		return Fail("validate GPU-VU PATH1 commit GXP header/version",
			result);
	result = sceGxmProgramCheck(structured_qp_numeric_vertex);
	if (result < 0)
		return Fail("validate structured GPU-VU Q/P GXP header/version",
			result);
	result = sceGxmProgramCheck(structured_fmac_numeric_vertex);
	if (result < 0)
		return Fail("validate structured GPU-VU exact FMAC GXP header/version",
			result);
	result = sceGxmProgramCheck(structured_fmac_native_vertex);
	if (result < 0)
		return Fail("validate structured GPU-VU native FMAC GXP header/version",
			result);
	result = sceGxmProgramCheck(fragment);
	if (result < 0)
		return Fail("validate GPU-VU product fragment GXP header/version", result);
	const std::array<std::pair<const char*, const SceGxmProgram*>, 13>
		compute_point_programs = {{
			{"universal", vertex},
			{"compact", compact_vertex},
			{"vif-unpack", vif_unpack_vertex},
			{"vif-unpack-independent", vif_unpack_independent_vertex},
			{"structured-control", structured_control_vertex},
			{"structured-state", structured_state_vertex},
			{"preflight-build", structured_preflight_build_vertex},
			{"preflight-reduce", structured_preflight_reduce_vertex},
			{"preflight-finalize", structured_preflight_finalize_vertex},
			{"path1-commit", path1_commit_vertex},
			{"structured-qp", structured_qp_numeric_vertex},
			{"structured-fmac-exact", structured_fmac_numeric_vertex},
			{"structured-fmac-native", structured_fmac_native_vertex},
		}};
	for (const auto& [name, point_program] : compute_point_programs)
	{
		u32 outputs = 0u;
		if (GpuVuComputeVertexOutputsAreValid(point_program, &outputs))
			continue;
		Console.Error(
			"GPU-VU: fixed compute GXP %s has invalid POINT output "
			"contract (outputs=%08x required=%08x).",
			name, outputs, GPU_VU_COMPUTE_REQUIRED_VERTEX_OUTPUTS);
		return Fail("validate fixed GPU-VU POINT outputs",
			SCE_GXM_ERROR_INVALID_VALUE);
	}
	result = sceGxmShaderPatcherRegisterProgram(patcher, vertex,
		&gpu_vu_universal_product_vertex_id);
	if (result < 0 || !gpu_vu_universal_product_vertex_id)
		return Fail("register universal GPU-VU product vertex program",
			result < 0 ? result : SCE_GXM_ERROR_INVALID_POINTER);
	result = sceGxmShaderPatcherRegisterProgram(patcher, compact_vertex,
		&gpu_vu_universal_compact_product_vertex_id);
	if (result < 0 || !gpu_vu_universal_compact_product_vertex_id)
		return Fail("register compact universal GPU-VU product vertex program",
			result < 0 ? result : SCE_GXM_ERROR_INVALID_POINTER);
	result = sceGxmShaderPatcherRegisterProgram(patcher, vif_unpack_vertex,
		&gpu_vu_vif_unpack_product_vertex_id);
	if (result < 0 || !gpu_vu_vif_unpack_product_vertex_id)
		return Fail("register universal GPU-VU VIF UNPACK vertex program",
			result < 0 ? result : SCE_GXM_ERROR_INVALID_POINTER);
	result = sceGxmShaderPatcherRegisterProgram(patcher,
		vif_unpack_independent_vertex,
		&gpu_vu_vif_unpack_independent_product_vertex_id);
	if (result < 0 || !gpu_vu_vif_unpack_independent_product_vertex_id)
		return Fail("register independent GPU-VU VIF UNPACK vertex program",
			result < 0 ? result : SCE_GXM_ERROR_INVALID_POINTER);
	result = sceGxmShaderPatcherRegisterProgram(patcher,
		structured_control_vertex, &gpu_vu_structured_control_product_vertex_id);
	if (result < 0 || !gpu_vu_structured_control_product_vertex_id)
		return Fail("register fixed structured GPU-VU control vertex program",
			result < 0 ? result : SCE_GXM_ERROR_INVALID_POINTER);
	result = sceGxmShaderPatcherRegisterProgram(patcher,
		structured_state_vertex, &gpu_vu_structured_state_product_vertex_id);
	if (result < 0 || !gpu_vu_structured_state_product_vertex_id)
		return Fail("register fixed structured GPU-VU state vertex program",
			result < 0 ? result : SCE_GXM_ERROR_INVALID_POINTER);
	result = sceGxmShaderPatcherRegisterProgram(patcher,
		structured_preflight_build_vertex,
		&gpu_vu_structured_preflight_build_product_vertex_id);
	if (result < 0 || !gpu_vu_structured_preflight_build_product_vertex_id)
		return Fail("register structured GPU-VU memory preflight build vertex program",
			result < 0 ? result : SCE_GXM_ERROR_INVALID_POINTER);
	result = sceGxmShaderPatcherRegisterProgram(patcher,
		structured_preflight_reduce_vertex,
		&gpu_vu_structured_preflight_reduce_product_vertex_id);
	if (result < 0 || !gpu_vu_structured_preflight_reduce_product_vertex_id)
		return Fail("register structured GPU-VU memory preflight reduce vertex program",
			result < 0 ? result : SCE_GXM_ERROR_INVALID_POINTER);
	result = sceGxmShaderPatcherRegisterProgram(patcher,
		structured_preflight_finalize_vertex,
		&gpu_vu_structured_preflight_finalize_product_vertex_id);
	if (result < 0 || !gpu_vu_structured_preflight_finalize_product_vertex_id)
		return Fail("register structured GPU-VU memory preflight finalize vertex program",
			result < 0 ? result : SCE_GXM_ERROR_INVALID_POINTER);
	result = sceGxmShaderPatcherRegisterProgram(patcher, path1_commit_vertex,
		&gpu_vu_path1_commit_product_vertex_id);
	if (result < 0 || !gpu_vu_path1_commit_product_vertex_id)
		return Fail("register universal GPU-VU terminal PATH1 commit vertex program",
			result < 0 ? result : SCE_GXM_ERROR_INVALID_POINTER);
	result = sceGxmShaderPatcherRegisterProgram(patcher,
		structured_qp_numeric_vertex,
		&gpu_vu_structured_qp_numeric_product_vertex_id);
	if (result < 0 || !gpu_vu_structured_qp_numeric_product_vertex_id)
		return Fail("register structured GPU-VU fixed Q/P numeric vertex program",
			result < 0 ? result : SCE_GXM_ERROR_INVALID_POINTER);
	result = sceGxmShaderPatcherRegisterProgram(patcher,
		structured_fmac_numeric_vertex,
		&gpu_vu_structured_fmac_numeric_product_vertex_id);
	if (result < 0 || !gpu_vu_structured_fmac_numeric_product_vertex_id)
		return Fail("register structured GPU-VU fixed FMAC numeric vertex program",
			result < 0 ? result : SCE_GXM_ERROR_INVALID_POINTER);
	result = sceGxmShaderPatcherRegisterProgram(patcher,
		structured_fmac_native_vertex,
		&gpu_vu_structured_fmac_native_product_vertex_id);
	if (result < 0 || !gpu_vu_structured_fmac_native_product_vertex_id)
		return Fail("register structured GPU-VU native FMAC vertex program",
			result < 0 ? result : SCE_GXM_ERROR_INVALID_POINTER);
	result = sceGxmShaderPatcherRegisterProgram(patcher, fragment,
		&gpu_vu_universal_product_fragment_id);
	if (result < 0 || !gpu_vu_universal_product_fragment_id)
		return Fail("register universal GPU-VU product fragment program",
			result < 0 ? result : SCE_GXM_ERROR_INVALID_POINTER);
	result = sceGxmShaderPatcherCreateVertexProgram(patcher,
		gpu_vu_universal_product_vertex_id, nullptr, 0, nullptr, 0,
		&gpu_vu_universal_product_vertex_program);
	if (result < 0 || !gpu_vu_universal_product_vertex_program)
		return Fail("create universal GPU-VU product vertex program",
			result < 0 ? result : SCE_GXM_ERROR_INVALID_POINTER);
	result = sceGxmShaderPatcherCreateVertexProgram(patcher,
		gpu_vu_universal_compact_product_vertex_id, nullptr, 0, nullptr, 0,
		&gpu_vu_universal_compact_product_vertex_program);
	if (result < 0 || !gpu_vu_universal_compact_product_vertex_program)
		return Fail("create compact universal GPU-VU product vertex program",
			result < 0 ? result : SCE_GXM_ERROR_INVALID_POINTER);
	result = sceGxmShaderPatcherCreateVertexProgram(patcher,
		gpu_vu_vif_unpack_product_vertex_id, nullptr, 0, nullptr, 0,
		&gpu_vu_vif_unpack_product_vertex_program);
	if (result < 0 || !gpu_vu_vif_unpack_product_vertex_program)
		return Fail("create universal GPU-VU VIF UNPACK vertex program",
			result < 0 ? result : SCE_GXM_ERROR_INVALID_POINTER);
	result = sceGxmShaderPatcherCreateVertexProgram(patcher,
		gpu_vu_vif_unpack_independent_product_vertex_id, nullptr, 0, nullptr, 0,
		&gpu_vu_vif_unpack_independent_product_vertex_program);
	if (result < 0 || !gpu_vu_vif_unpack_independent_product_vertex_program)
		return Fail("create independent GPU-VU VIF UNPACK vertex program",
			result < 0 ? result : SCE_GXM_ERROR_INVALID_POINTER);
	result = sceGxmShaderPatcherCreateVertexProgram(patcher,
		gpu_vu_structured_control_product_vertex_id, nullptr, 0, nullptr, 0,
		&gpu_vu_structured_control_product_vertex_program);
	if (result < 0 || !gpu_vu_structured_control_product_vertex_program)
		return Fail("create fixed structured GPU-VU control vertex program",
			result < 0 ? result : SCE_GXM_ERROR_INVALID_POINTER);
	result = sceGxmShaderPatcherCreateVertexProgram(patcher,
		gpu_vu_structured_state_product_vertex_id, nullptr, 0, nullptr, 0,
		&gpu_vu_structured_state_product_vertex_program);
	if (result < 0 || !gpu_vu_structured_state_product_vertex_program)
		return Fail("create fixed structured GPU-VU state vertex program",
			result < 0 ? result : SCE_GXM_ERROR_INVALID_POINTER);
	result = sceGxmShaderPatcherCreateVertexProgram(patcher,
		gpu_vu_structured_preflight_build_product_vertex_id, nullptr, 0, nullptr, 0,
		&gpu_vu_structured_preflight_build_product_vertex_program);
	if (result < 0 || !gpu_vu_structured_preflight_build_product_vertex_program)
		return Fail("create structured GPU-VU memory preflight build vertex program",
			result < 0 ? result : SCE_GXM_ERROR_INVALID_POINTER);
	result = sceGxmShaderPatcherCreateVertexProgram(patcher,
		gpu_vu_structured_preflight_reduce_product_vertex_id, nullptr, 0, nullptr, 0,
		&gpu_vu_structured_preflight_reduce_product_vertex_program);
	if (result < 0 || !gpu_vu_structured_preflight_reduce_product_vertex_program)
		return Fail("create structured GPU-VU memory preflight reduce vertex program",
			result < 0 ? result : SCE_GXM_ERROR_INVALID_POINTER);
	result = sceGxmShaderPatcherCreateVertexProgram(patcher,
		gpu_vu_structured_preflight_finalize_product_vertex_id, nullptr, 0, nullptr, 0,
		&gpu_vu_structured_preflight_finalize_product_vertex_program);
	if (result < 0 || !gpu_vu_structured_preflight_finalize_product_vertex_program)
		return Fail("create structured GPU-VU memory preflight finalize vertex program",
			result < 0 ? result : SCE_GXM_ERROR_INVALID_POINTER);
	result = sceGxmShaderPatcherCreateVertexProgram(patcher,
		gpu_vu_path1_commit_product_vertex_id, nullptr, 0, nullptr, 0,
		&gpu_vu_path1_commit_product_vertex_program);
	if (result < 0 || !gpu_vu_path1_commit_product_vertex_program)
		return Fail("create universal GPU-VU terminal PATH1 commit vertex program",
			result < 0 ? result : SCE_GXM_ERROR_INVALID_POINTER);
	result = sceGxmShaderPatcherCreateVertexProgram(patcher,
		gpu_vu_structured_qp_numeric_product_vertex_id,
		nullptr, 0, nullptr, 0,
		&gpu_vu_structured_qp_numeric_product_vertex_program);
	if (result < 0 || !gpu_vu_structured_qp_numeric_product_vertex_program)
		return Fail("create structured GPU-VU fixed Q/P numeric vertex program",
			result < 0 ? result : SCE_GXM_ERROR_INVALID_POINTER);
	result = sceGxmShaderPatcherCreateVertexProgram(patcher,
		gpu_vu_structured_fmac_numeric_product_vertex_id,
		nullptr, 0, nullptr, 0,
		&gpu_vu_structured_fmac_numeric_product_vertex_program);
	if (result < 0 || !gpu_vu_structured_fmac_numeric_product_vertex_program)
		return Fail("create structured GPU-VU fixed FMAC numeric vertex program",
			result < 0 ? result : SCE_GXM_ERROR_INVALID_POINTER);
	result = sceGxmShaderPatcherCreateVertexProgram(patcher,
		gpu_vu_structured_fmac_native_product_vertex_id,
		nullptr, 0, nullptr, 0,
		&gpu_vu_structured_fmac_native_product_vertex_program);
	if (result < 0 || !gpu_vu_structured_fmac_native_product_vertex_program)
		return Fail("create structured GPU-VU native FMAC vertex program",
			result < 0 ? result : SCE_GXM_ERROR_INVALID_POINTER);
	SceGxmBlendInfo no_color{};
	no_color.colorFunc = SCE_GXM_BLEND_FUNC_NONE;
	no_color.alphaFunc = SCE_GXM_BLEND_FUNC_NONE;
	no_color.colorSrc = SCE_GXM_BLEND_FACTOR_ZERO;
	no_color.colorDst = SCE_GXM_BLEND_FACTOR_ZERO;
	no_color.alphaSrc = SCE_GXM_BLEND_FACTOR_ZERO;
	no_color.alphaDst = SCE_GXM_BLEND_FACTOR_ZERO;
	no_color.colorMask = SCE_GXM_COLOR_MASK_NONE;
	result = sceGxmShaderPatcherCreateFragmentProgram(patcher,
		gpu_vu_universal_product_fragment_id,
		SCE_GXM_OUTPUT_REGISTER_FORMAT_DECLARED,
		SCE_GXM_MULTISAMPLE_NONE, &no_color, vertex,
		&gpu_vu_universal_product_fragment_program);
	if (result < 0 || !gpu_vu_universal_product_fragment_program)
		return Fail("create universal GPU-VU product fragment program",
			result < 0 ? result : SCE_GXM_ERROR_INVALID_POINTER);
	result = sceGxmShaderPatcherCreateFragmentProgram(patcher,
		gpu_vu_universal_product_fragment_id,
		SCE_GXM_OUTPUT_REGISTER_FORMAT_DECLARED,
		SCE_GXM_MULTISAMPLE_NONE, &no_color, compact_vertex,
		&gpu_vu_universal_compact_product_fragment_program);
	if (result < 0 || !gpu_vu_universal_compact_product_fragment_program)
		return Fail("create compact universal GPU-VU product fragment program",
			result < 0 ? result : SCE_GXM_ERROR_INVALID_POINTER);
	result = sceGxmShaderPatcherCreateFragmentProgram(patcher,
		gpu_vu_universal_product_fragment_id,
		SCE_GXM_OUTPUT_REGISTER_FORMAT_DECLARED,
		SCE_GXM_MULTISAMPLE_NONE, &no_color, vif_unpack_vertex,
		&gpu_vu_vif_unpack_product_fragment_program);
	if (result < 0 || !gpu_vu_vif_unpack_product_fragment_program)
		return Fail("create universal GPU-VU VIF UNPACK fragment program",
			result < 0 ? result : SCE_GXM_ERROR_INVALID_POINTER);
	result = sceGxmShaderPatcherCreateFragmentProgram(patcher,
		gpu_vu_universal_product_fragment_id,
		SCE_GXM_OUTPUT_REGISTER_FORMAT_DECLARED,
		SCE_GXM_MULTISAMPLE_NONE, &no_color, vif_unpack_independent_vertex,
		&gpu_vu_vif_unpack_independent_product_fragment_program);
	if (result < 0 || !gpu_vu_vif_unpack_independent_product_fragment_program)
		return Fail("create independent GPU-VU VIF UNPACK fragment program",
			result < 0 ? result : SCE_GXM_ERROR_INVALID_POINTER);
	result = sceGxmShaderPatcherCreateFragmentProgram(patcher,
		gpu_vu_universal_product_fragment_id,
		SCE_GXM_OUTPUT_REGISTER_FORMAT_DECLARED,
		SCE_GXM_MULTISAMPLE_NONE, &no_color, structured_control_vertex,
		&gpu_vu_structured_control_product_fragment_program);
	if (result < 0 || !gpu_vu_structured_control_product_fragment_program)
		return Fail("create fixed structured GPU-VU control fragment program",
			result < 0 ? result : SCE_GXM_ERROR_INVALID_POINTER);
	result = sceGxmShaderPatcherCreateFragmentProgram(patcher,
		gpu_vu_universal_product_fragment_id,
		SCE_GXM_OUTPUT_REGISTER_FORMAT_DECLARED,
		SCE_GXM_MULTISAMPLE_NONE, &no_color, structured_state_vertex,
		&gpu_vu_structured_state_product_fragment_program);
	if (result < 0 || !gpu_vu_structured_state_product_fragment_program)
		return Fail("create fixed structured GPU-VU state fragment program",
			result < 0 ? result : SCE_GXM_ERROR_INVALID_POINTER);
	result = sceGxmShaderPatcherCreateFragmentProgram(patcher,
		gpu_vu_universal_product_fragment_id,
		SCE_GXM_OUTPUT_REGISTER_FORMAT_DECLARED,
		SCE_GXM_MULTISAMPLE_NONE, &no_color, structured_preflight_build_vertex,
		&gpu_vu_structured_preflight_build_product_fragment_program);
	if (result < 0 || !gpu_vu_structured_preflight_build_product_fragment_program)
		return Fail("create structured GPU-VU memory preflight build fragment program",
			result < 0 ? result : SCE_GXM_ERROR_INVALID_POINTER);
	result = sceGxmShaderPatcherCreateFragmentProgram(patcher,
		gpu_vu_universal_product_fragment_id,
		SCE_GXM_OUTPUT_REGISTER_FORMAT_DECLARED,
		SCE_GXM_MULTISAMPLE_NONE, &no_color, structured_preflight_reduce_vertex,
		&gpu_vu_structured_preflight_reduce_product_fragment_program);
	if (result < 0 || !gpu_vu_structured_preflight_reduce_product_fragment_program)
		return Fail("create structured GPU-VU memory preflight reduce fragment program",
			result < 0 ? result : SCE_GXM_ERROR_INVALID_POINTER);
	result = sceGxmShaderPatcherCreateFragmentProgram(patcher,
		gpu_vu_universal_product_fragment_id,
		SCE_GXM_OUTPUT_REGISTER_FORMAT_DECLARED,
		SCE_GXM_MULTISAMPLE_NONE, &no_color, structured_preflight_finalize_vertex,
		&gpu_vu_structured_preflight_finalize_product_fragment_program);
	if (result < 0 || !gpu_vu_structured_preflight_finalize_product_fragment_program)
		return Fail("create structured GPU-VU memory preflight finalize fragment program",
			result < 0 ? result : SCE_GXM_ERROR_INVALID_POINTER);
	result = sceGxmShaderPatcherCreateFragmentProgram(patcher,
		gpu_vu_universal_product_fragment_id,
		SCE_GXM_OUTPUT_REGISTER_FORMAT_DECLARED,
		SCE_GXM_MULTISAMPLE_NONE, &no_color, path1_commit_vertex,
		&gpu_vu_path1_commit_product_fragment_program);
	if (result < 0 || !gpu_vu_path1_commit_product_fragment_program)
		return Fail("create universal GPU-VU terminal PATH1 commit fragment program",
			result < 0 ? result : SCE_GXM_ERROR_INVALID_POINTER);
	result = sceGxmShaderPatcherCreateFragmentProgram(patcher,
		gpu_vu_universal_product_fragment_id,
		SCE_GXM_OUTPUT_REGISTER_FORMAT_DECLARED,
		SCE_GXM_MULTISAMPLE_NONE, &no_color, structured_qp_numeric_vertex,
		&gpu_vu_structured_qp_numeric_product_fragment_program);
	if (result < 0 || !gpu_vu_structured_qp_numeric_product_fragment_program)
		return Fail("create structured GPU-VU fixed Q/P numeric fragment program",
			result < 0 ? result : SCE_GXM_ERROR_INVALID_POINTER);
	result = sceGxmShaderPatcherCreateFragmentProgram(patcher,
		gpu_vu_universal_product_fragment_id,
		SCE_GXM_OUTPUT_REGISTER_FORMAT_DECLARED,
		SCE_GXM_MULTISAMPLE_NONE, &no_color, structured_fmac_numeric_vertex,
		&gpu_vu_structured_fmac_numeric_product_fragment_program);
	if (result < 0 || !gpu_vu_structured_fmac_numeric_product_fragment_program)
		return Fail("create structured GPU-VU fixed FMAC numeric fragment program",
			result < 0 ? result : SCE_GXM_ERROR_INVALID_POINTER);
	result = sceGxmShaderPatcherCreateFragmentProgram(patcher,
		gpu_vu_universal_product_fragment_id,
		SCE_GXM_OUTPUT_REGISTER_FORMAT_DECLARED,
		SCE_GXM_MULTISAMPLE_NONE, &no_color, structured_fmac_native_vertex,
		&gpu_vu_structured_fmac_native_product_fragment_program);
	if (result < 0 || !gpu_vu_structured_fmac_native_product_fragment_program)
		return Fail("create structured GPU-VU native FMAC fragment program",
			result < 0 ? result : SCE_GXM_ERROR_INVALID_POINTER);

	u32* const arm_estimates = reinterpret_cast<u32*>(
		gxp_bytes + GPU_VU_UNIVERSAL_PRODUCT_ESTIMATE_OFFSET);
	for (u32 scaled = 256; scaled < 512; scaled++)
	{
		const u32 a = scaled * 2u + 1u;
		const u32 b = (1u << 19u) / a;
		arm_estimates[scaled - 256u] = ((b + 1u) >> 1u) & 0xffu;
	}
	for (u32 scaled = 128; scaled < 512; scaled++)
	{
		u32 a = scaled < 256u ? scaled * 2u + 1u :
			((((scaled >> 1u) << 1u) + 1u) * 2u);
		u32 b = 512u;
		while (a * (b + 1u) * (b + 1u) < (1u << 28u))
			b++;
		arm_estimates[256u + scaled - 128u] =
			((b + 1u) >> 1u) & 0xffu;
	}

	volatile unsigned int* const notification_region =
		sceGxmGetNotificationRegion();
	if (!notification_region)
		return false;
	for (u32 index = 0; index < gpu_vu_universal_product_slots.size(); index++)
	{
		UniversalGpuVuProductSlot& slot =
			gpu_vu_universal_product_slots[index];
		result = VitaGXM::AllocateMappedBlock(
			"VitaSX2 universal GPU-VU generation",
			SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_NC_RW,
			GPU_VU_UNIVERSAL_PRODUCT_SLOT_BYTES,
			SCE_GXM_MEMORY_ATTRIB_RW, &slot.buffer);
		if (result < 0)
			return Fail("allocate universal GPU-VU product generation", result);
		std::memset(slot.buffer.base, 0, slot.buffer.size);
			slot.notification.address = notification_region +
				2u * GPU_VU_RETIREMENT_SLOT_COUNT + index;
			slot.notification.value = 0;
			*slot.notification.address = 0;
			for (SceGxmNotification& stage_notification :
				slot.stage_notifications)
			{
				stage_notification.address = slot.notification.address;
				stage_notification.value = 0;
			}
	}
	int product_surface_error = 0;
	gpu_vu_universal_product_color_surface =
		VitaGXM::GSTextureGXM::Create(this, GSTexture::RenderTarget,
			1, 1, 1, GSTexture::Format::Color, &product_surface_error);
	if (!gpu_vu_universal_product_color_surface ||
		!gpu_vu_universal_product_color_surface->ColorSurface())
	{
		return Fail("create universal GPU-VU scene color surface",
			product_surface_error < 0 ? product_surface_error :
				SCE_GXM_ERROR_INVALID_POINTER);
	}
	if (!CreateRenderTarget(1, 1, MAX_GXM_SCENES_PER_RENDER_TARGET,
			&gpu_vu_universal_product_target))
	{
		return false;
	}
	if (!AttestUniversalGpuVuStructuredFmac())
	{
		return false;
	}
	gpu_vu_universal_product_ready = true;
	VitaGpuVu::SetUniversalGpuVuCompactProviderAvailable(true);
	VitaGpuVu::SetUniversalGpuVuDeviceAvailable(true);
	Console.WriteLn(
		"GPU-VU: persistent universal owner ready (slots=%u slot_bytes=%u "
		"providers=fixed,compact-scheduled "
		"vif-unpack=independent+serial-mirrored-offline-gxp "
		"structured-control-state=offline-gxp "
		"structured-preflight=generation-tagged-parallel-build/reduce/finalize-offline-gxp "
		"structured-qp-numeric=offline-gxp "
		"structured-fmac-numeric=exact-nearest:%u+native-normalized:%u:%s-offline-gxp "
		"structured-scratch-range=%u:%u-words+%u-guard "
		"path1-terminal-commit=offline-gxp input=immutable-mapped "
		"output=raw-path1 notification=vertex).",
		GPU_VU_UNIVERSAL_PRODUCT_SLOT_COUNT,
		GPU_VU_UNIVERSAL_PRODUCT_SLOT_BYTES,
		gpu_vu_structured_fmac_exact_attested ? 1u : 0u,
		gpu_vu_structured_fmac_native_attestation !=
			StructuredNativeFmacAttestation::Unavailable ? 1u : 0u,
		gpu_vu_structured_fmac_native_attestation ==
			StructuredNativeFmacAttestation::NearestEven ? "nearest-even" :
		gpu_vu_structured_fmac_native_attestation ==
			StructuredNativeFmacAttestation::FiniteMantissaChop ?
			"finite-mantissa-chop" :
			"unavailable",
		gpu_vu_structured_scratch_range_attested ? 1u : 0u,
		GPU_VU_STRUCTURED_SCRATCH_BYTES / sizeof(u32),
		GPU_VU_STRUCTURED_SCRATCH_GUARD_WORDS);
	return true;
}

void GSDeviceGXM::Impl::ReleaseUniversalGpuVuProduct()
{
	VitaGpuVu::SetUniversalGpuVuDeviceAvailable(false);
	VitaGpuVu::SetUniversalGpuVuCompactProviderAvailable(false);
	gpu_vu_universal_product_ready = false;
	if (gpu_vu_canonical_vu_memory_mapped)
	{
		const int unmap_result = sceGxmUnmapMemory(SysMemory::GetVUMem());
		if (unmap_result < 0)
		{
			Console.Error(
				"GPU-VU: canonical VU1 GXM unmap failed (%08x); "
				"retaining mapping state for a later drained teardown retry.",
				static_cast<u32>(unmap_result));
		}
		else
		{
			gpu_vu_canonical_vu_memory_mapped = false;
		}
	}
	gpu_vu_structured_scratch_range_attested = false;
	if (gpu_vu_structured_fmac_native_product_fragment_program)
	{
		sceGxmShaderPatcherReleaseFragmentProgram(patcher,
			gpu_vu_structured_fmac_native_product_fragment_program);
		gpu_vu_structured_fmac_native_product_fragment_program = nullptr;
	}
	if (gpu_vu_structured_fmac_numeric_product_fragment_program)
	{
		sceGxmShaderPatcherReleaseFragmentProgram(patcher,
			gpu_vu_structured_fmac_numeric_product_fragment_program);
		gpu_vu_structured_fmac_numeric_product_fragment_program = nullptr;
	}
	if (gpu_vu_structured_qp_numeric_product_fragment_program)
	{
		sceGxmShaderPatcherReleaseFragmentProgram(patcher,
			gpu_vu_structured_qp_numeric_product_fragment_program);
		gpu_vu_structured_qp_numeric_product_fragment_program = nullptr;
	}
	if (gpu_vu_path1_commit_product_fragment_program)
	{
		sceGxmShaderPatcherReleaseFragmentProgram(patcher,
			gpu_vu_path1_commit_product_fragment_program);
		gpu_vu_path1_commit_product_fragment_program = nullptr;
	}
	if (gpu_vu_structured_preflight_finalize_product_fragment_program)
	{
		sceGxmShaderPatcherReleaseFragmentProgram(patcher,
			gpu_vu_structured_preflight_finalize_product_fragment_program);
		gpu_vu_structured_preflight_finalize_product_fragment_program = nullptr;
	}
	if (gpu_vu_structured_preflight_reduce_product_fragment_program)
	{
		sceGxmShaderPatcherReleaseFragmentProgram(patcher,
			gpu_vu_structured_preflight_reduce_product_fragment_program);
		gpu_vu_structured_preflight_reduce_product_fragment_program = nullptr;
	}
	if (gpu_vu_structured_preflight_build_product_fragment_program)
	{
		sceGxmShaderPatcherReleaseFragmentProgram(patcher,
			gpu_vu_structured_preflight_build_product_fragment_program);
		gpu_vu_structured_preflight_build_product_fragment_program = nullptr;
	}
	if (gpu_vu_structured_state_product_fragment_program)
	{
		sceGxmShaderPatcherReleaseFragmentProgram(patcher,
			gpu_vu_structured_state_product_fragment_program);
		gpu_vu_structured_state_product_fragment_program = nullptr;
	}
	if (gpu_vu_structured_control_product_fragment_program)
	{
		sceGxmShaderPatcherReleaseFragmentProgram(patcher,
			gpu_vu_structured_control_product_fragment_program);
		gpu_vu_structured_control_product_fragment_program = nullptr;
	}
	if (gpu_vu_vif_unpack_product_fragment_program)
	{
		sceGxmShaderPatcherReleaseFragmentProgram(patcher,
			gpu_vu_vif_unpack_product_fragment_program);
		gpu_vu_vif_unpack_product_fragment_program = nullptr;
	}
	if (gpu_vu_vif_unpack_independent_product_fragment_program)
	{
		sceGxmShaderPatcherReleaseFragmentProgram(patcher,
			gpu_vu_vif_unpack_independent_product_fragment_program);
		gpu_vu_vif_unpack_independent_product_fragment_program = nullptr;
	}
	if (gpu_vu_universal_compact_product_fragment_program)
	{
		sceGxmShaderPatcherReleaseFragmentProgram(patcher,
			gpu_vu_universal_compact_product_fragment_program);
		gpu_vu_universal_compact_product_fragment_program = nullptr;
	}
	if (gpu_vu_universal_product_fragment_program)
	{
		sceGxmShaderPatcherReleaseFragmentProgram(patcher,
			gpu_vu_universal_product_fragment_program);
		gpu_vu_universal_product_fragment_program = nullptr;
	}
	if (gpu_vu_universal_product_vertex_program)
	{
		sceGxmShaderPatcherReleaseVertexProgram(patcher,
			gpu_vu_universal_product_vertex_program);
		gpu_vu_universal_product_vertex_program = nullptr;
	}
	if (gpu_vu_universal_compact_product_vertex_program)
	{
		sceGxmShaderPatcherReleaseVertexProgram(patcher,
			gpu_vu_universal_compact_product_vertex_program);
		gpu_vu_universal_compact_product_vertex_program = nullptr;
	}
	if (gpu_vu_vif_unpack_product_vertex_program)
	{
		sceGxmShaderPatcherReleaseVertexProgram(patcher,
			gpu_vu_vif_unpack_product_vertex_program);
		gpu_vu_vif_unpack_product_vertex_program = nullptr;
	}
	if (gpu_vu_vif_unpack_independent_product_vertex_program)
	{
		sceGxmShaderPatcherReleaseVertexProgram(patcher,
			gpu_vu_vif_unpack_independent_product_vertex_program);
		gpu_vu_vif_unpack_independent_product_vertex_program = nullptr;
	}
	if (gpu_vu_structured_preflight_finalize_product_vertex_program)
	{
		sceGxmShaderPatcherReleaseVertexProgram(patcher,
			gpu_vu_structured_preflight_finalize_product_vertex_program);
		gpu_vu_structured_preflight_finalize_product_vertex_program = nullptr;
	}
	if (gpu_vu_structured_preflight_reduce_product_vertex_program)
	{
		sceGxmShaderPatcherReleaseVertexProgram(patcher,
			gpu_vu_structured_preflight_reduce_product_vertex_program);
		gpu_vu_structured_preflight_reduce_product_vertex_program = nullptr;
	}
	if (gpu_vu_structured_preflight_build_product_vertex_program)
	{
		sceGxmShaderPatcherReleaseVertexProgram(patcher,
			gpu_vu_structured_preflight_build_product_vertex_program);
		gpu_vu_structured_preflight_build_product_vertex_program = nullptr;
	}
	if (gpu_vu_structured_state_product_vertex_program)
	{
		sceGxmShaderPatcherReleaseVertexProgram(patcher,
			gpu_vu_structured_state_product_vertex_program);
		gpu_vu_structured_state_product_vertex_program = nullptr;
	}
	if (gpu_vu_structured_control_product_vertex_program)
	{
		sceGxmShaderPatcherReleaseVertexProgram(patcher,
			gpu_vu_structured_control_product_vertex_program);
		gpu_vu_structured_control_product_vertex_program = nullptr;
	}
	if (gpu_vu_structured_qp_numeric_product_vertex_program)
	{
		sceGxmShaderPatcherReleaseVertexProgram(patcher,
			gpu_vu_structured_qp_numeric_product_vertex_program);
		gpu_vu_structured_qp_numeric_product_vertex_program = nullptr;
	}
	if (gpu_vu_structured_fmac_native_product_vertex_program)
	{
		sceGxmShaderPatcherReleaseVertexProgram(patcher,
			gpu_vu_structured_fmac_native_product_vertex_program);
		gpu_vu_structured_fmac_native_product_vertex_program = nullptr;
	}
	if (gpu_vu_structured_fmac_numeric_product_vertex_program)
	{
		sceGxmShaderPatcherReleaseVertexProgram(patcher,
			gpu_vu_structured_fmac_numeric_product_vertex_program);
		gpu_vu_structured_fmac_numeric_product_vertex_program = nullptr;
	}
	if (gpu_vu_path1_commit_product_vertex_program)
	{
		sceGxmShaderPatcherReleaseVertexProgram(patcher,
			gpu_vu_path1_commit_product_vertex_program);
		gpu_vu_path1_commit_product_vertex_program = nullptr;
	}
	if (gpu_vu_universal_product_fragment_id)
	{
		sceGxmShaderPatcherUnregisterProgram(patcher,
			gpu_vu_universal_product_fragment_id);
		gpu_vu_universal_product_fragment_id = nullptr;
	}
	if (gpu_vu_universal_product_vertex_id)
	{
		sceGxmShaderPatcherUnregisterProgram(patcher,
			gpu_vu_universal_product_vertex_id);
		gpu_vu_universal_product_vertex_id = nullptr;
	}
	if (gpu_vu_universal_compact_product_vertex_id)
	{
		sceGxmShaderPatcherUnregisterProgram(patcher,
			gpu_vu_universal_compact_product_vertex_id);
		gpu_vu_universal_compact_product_vertex_id = nullptr;
	}
	if (gpu_vu_vif_unpack_product_vertex_id)
	{
		sceGxmShaderPatcherUnregisterProgram(patcher,
			gpu_vu_vif_unpack_product_vertex_id);
		gpu_vu_vif_unpack_product_vertex_id = nullptr;
	}
	if (gpu_vu_vif_unpack_independent_product_vertex_id)
	{
		sceGxmShaderPatcherUnregisterProgram(patcher,
			gpu_vu_vif_unpack_independent_product_vertex_id);
		gpu_vu_vif_unpack_independent_product_vertex_id = nullptr;
	}
	if (gpu_vu_structured_preflight_finalize_product_vertex_id)
	{
		sceGxmShaderPatcherUnregisterProgram(patcher,
			gpu_vu_structured_preflight_finalize_product_vertex_id);
		gpu_vu_structured_preflight_finalize_product_vertex_id = nullptr;
	}
	if (gpu_vu_structured_preflight_reduce_product_vertex_id)
	{
		sceGxmShaderPatcherUnregisterProgram(patcher,
			gpu_vu_structured_preflight_reduce_product_vertex_id);
		gpu_vu_structured_preflight_reduce_product_vertex_id = nullptr;
	}
	if (gpu_vu_structured_preflight_build_product_vertex_id)
	{
		sceGxmShaderPatcherUnregisterProgram(patcher,
			gpu_vu_structured_preflight_build_product_vertex_id);
		gpu_vu_structured_preflight_build_product_vertex_id = nullptr;
	}
	if (gpu_vu_structured_state_product_vertex_id)
	{
		sceGxmShaderPatcherUnregisterProgram(patcher,
			gpu_vu_structured_state_product_vertex_id);
		gpu_vu_structured_state_product_vertex_id = nullptr;
	}
	if (gpu_vu_structured_control_product_vertex_id)
	{
		sceGxmShaderPatcherUnregisterProgram(patcher,
			gpu_vu_structured_control_product_vertex_id);
		gpu_vu_structured_control_product_vertex_id = nullptr;
	}
	if (gpu_vu_structured_qp_numeric_product_vertex_id)
	{
		sceGxmShaderPatcherUnregisterProgram(patcher,
			gpu_vu_structured_qp_numeric_product_vertex_id);
		gpu_vu_structured_qp_numeric_product_vertex_id = nullptr;
	}
	if (gpu_vu_structured_fmac_native_product_vertex_id)
	{
		sceGxmShaderPatcherUnregisterProgram(patcher,
			gpu_vu_structured_fmac_native_product_vertex_id);
		gpu_vu_structured_fmac_native_product_vertex_id = nullptr;
	}
	if (gpu_vu_structured_fmac_numeric_product_vertex_id)
	{
		sceGxmShaderPatcherUnregisterProgram(patcher,
			gpu_vu_structured_fmac_numeric_product_vertex_id);
		gpu_vu_structured_fmac_numeric_product_vertex_id = nullptr;
	}
	if (gpu_vu_path1_commit_product_vertex_id)
	{
		sceGxmShaderPatcherUnregisterProgram(patcher,
			gpu_vu_path1_commit_product_vertex_id);
		gpu_vu_path1_commit_product_vertex_id = nullptr;
	}
	for (UniversalGpuVuProductSlot& slot : gpu_vu_universal_product_slots)
	{
		if (slot.buffer.IsAllocated())
			VitaGXM::ReleaseMappedBlock(&slot.buffer);
		slot.notification = {};
		slot.sequence = 0;
		slot.submission_groups = 0;
		slot.continuation_groups = 0;
		slot.core_submissions = 0;
		slot.job_submissions = 0;
		slot.path1_commit_submissions = 0;
		slot.source_bank = 0;
		slot.completed_bank = 0;
		slot.structured_submission_cursor = 0;
		slot.structured_submission_total = 0;
		slot.structured_workspace_generation = 0;
		slot.generated_serial = false;
		slot.generated_hot = false;
		slot.generated_structured = false;
		slot.compact_serial = false;
		slot.submitted = false;
		slot.committed = false;
		slot.dedicated_scene = false;
	}
	if (gpu_vu_universal_product_shared.IsAllocated())
		VitaGXM::ReleaseMappedBlock(&gpu_vu_universal_product_shared);
	if (gpu_vu_generated_estimate_table.IsAllocated())
		VitaGXM::ReleaseMappedBlock(&gpu_vu_generated_estimate_table);
	if (gpu_vu_universal_product_target)
	{
		sceGxmDestroyRenderTarget(gpu_vu_universal_product_target);
		gpu_vu_universal_product_target = nullptr;
	}
	gpu_vu_universal_product_color_surface.reset();
	gpu_vu_universal_product_committed_slot = -1;
	gpu_vu_universal_product_next_slot = 0;
	gpu_vu_universal_product_accept_count = 0;
	gpu_vu_universal_product_runtime_reject_count = 0;
	gpu_vu_universal_product_dispatch_cost_reject_count = 0;
	gpu_vu_universal_product_continuation_count = 0;
	gpu_vu_universal_product_structured_submit_count = 0;
	gpu_vu_universal_product_structured_owner_reject_count = 0;
	gpu_vu_universal_product_structured_quarantine_reject_count = 0;
	gpu_vu_universal_product_structured_bank_diagnostic_count = 0;
	gpu_vu_universal_generated_accept_reported = false;
	gpu_vu_universal_observed_pair_bounds = {};
	gpu_vu_universal_observed_pair_bound_clock = 0;
	gpu_vu_structured_fmac_exact_attested = false;
	gpu_vu_structured_fmac_native_attestation =
		StructuredNativeFmacAttestation::Unavailable;
}

void GSDeviceGXM::Impl::ServiceUniversalGpuVuEpoch(
	VitaGpuVu::UniversalGpuVuEpoch* epoch)
{
	if (!epoch)
		return;
	const auto reject = [&](VitaGpuVu::UniversalGpuVuRejection reason,
			u32 pairs) {
		// The GS owner never writes MTVU's canonical VU1 mirror. On rejection
		// the preceding committed mapped generation remains authoritative and
		// the VU worker alone materializes it before replaying the retained
		// journal. The failed slot is private and is never published.
		epoch->MarkGpuRejected(reason, pairs);
	};
	// Hardware-driven GPU-VU Phase 0: no legacy universal epoch may acquire GXM
	// ownership. This independently contains stale mailbox descriptors even if
	// an admission regression bypasses MTVU's product gate.
	reject(VitaGpuVu::UniversalGpuVuRejection::
		GeneratedArchitectureQuarantined, epoch->PreflightPairCount());
	return;

	if (!gpu_vu_universal_product_ready)
	{
		reject(VitaGpuVu::UniversalGpuVuRejection::DeviceUnavailable,
			epoch->PreflightPairCount());
		return;
	}
	// Product policy is generated-or-MTVU.  The fixed interpreter and the
	// dependency-heavy structured prototype remain registered only for
	// validation/support-kernel work; neither is a legal fallback after an
	// epoch reaches the GS owner.  This is a defense-in-depth check behind the
	// MTVU pre-effect readiness gate.
	GeneratedVuProgram* const product_generated_program =
		FindGeneratedVuProgram(epoch->GeneratedProgramKey());
	const bool product_generated_ready = product_generated_program &&
		product_generated_program->metadata.execution_kind ==
			VitaGpuVu::GeneratedCgExecutionKind::UniversalDirectStateMachine &&
		product_generated_program->vertex_program &&
		product_generated_program->general_fragment_program;
	const VitaGpuVu::GeneratedHotBundle& product_hot_bundle =
		epoch->GeneratedHotBundleDescriptor();
	VitaGpuVu::GeneratedHotBundle live_hot_bundle;
	const VitaGpuVu::GeneratedUniversalRegionBundleState product_hot_state =
		product_hot_bundle.HasAtomicCompilerOwnership() ?
			VitaGpuVu::QueryUniversalControlFlowRegionPrograms(
				epoch->ProgramIdentity(), &live_hot_bundle) :
			VitaGpuVu::GeneratedUniversalRegionBundleState::Missing;
	const bool product_hot_owner_ready =
		product_hot_state ==
			VitaGpuVu::GeneratedUniversalRegionBundleState::Ready &&
		product_hot_bundle.MatchesOwner(epoch->ProgramIdentity(),
			epoch->AnalysisEntryPc(), epoch->ConfigurationBits()) &&
		product_hot_bundle.MatchesLiveGeneration(live_hot_bundle);
	std::array<GeneratedVuProgram*, VitaGpuVu::GeneratedHotBundleMaximumModules>
		product_hot_programs{};
		bool product_hot_ready = product_hot_owner_ready;
	for (u32 index = 0u;
		product_hot_ready && index < product_hot_bundle.module_count; index++)
	{
		GeneratedVuProgram* const program =
			FindGeneratedVuProgram(product_hot_bundle.modules[index].key);
		product_hot_programs[index] = program;
			product_hot_ready = program &&
			program->metadata.execution_kind ==
				product_hot_bundle.modules[index].kind &&
				program->vertex_program && program->general_fragment_program;
		}
		constexpr u32 NO_GENERATED_HOT_MODULE =
			std::numeric_limits<u32>::max();
		u32 product_hot_initial_module = NO_GENERATED_HOT_MODULE;
		if (product_hot_ready &&
			!product_hot_bundle.ModuleForPc(
				epoch->AnalysisEntryPc(), &product_hot_initial_module))
		{
			product_hot_ready = false;
		}
	const auto generated_hot_pair_limit = [&](u32 module_index) {
		if (!product_hot_ready ||
			module_index >= product_hot_bundle.module_count ||
			!product_hot_programs[module_index])
		{
			return 0u;
		}
		const VitaGpuVu::GeneratedHotBundleModule& module =
			product_hot_bundle.modules[module_index];
		return VitaGpuVu::UniversalGpuVuGeneratedHotPairLimit(
			module.semantic_pair_count,
			product_hot_programs[module_index]
				->gxp_resources.primary_instruction_count,
			module.maximum_dynamic_pairs_per_invocation);
	};
	if ((!product_generated_ready && !product_hot_ready) ||
		epoch->StructuredBundle().HasKeys())
	{
		const VitaGpuVu::GeneratedProgramState state =
			VitaGpuVu::QueryGeneratedProgram(epoch->GeneratedProgramKey());
		const bool hot_pending = product_hot_state ==
			VitaGpuVu::GeneratedUniversalRegionBundleState::Compiling;
		const VitaGpuVu::UniversalGpuVuRejection reason =
			VitaGpuVu::GeneratedProgramStateIsPending(state) || hot_pending ?
				VitaGpuVu::UniversalGpuVuRejection::GeneratedProgramPending :
				VitaGpuVu::UniversalGpuVuRejection::GeneratedProgramUnavailable;
		Console.Warning(
			"GPU-VU seq=%llu provider=cpu-mtvu accepted=0 pre_effect=1 "
			"reason=%s generated_state=%u hot_state=%u hot_modules=%u "
			"fixed_interpreter_submitted=0 structured_submitted=0 pairs=%u "
			"cpu_fallback=1.",
			static_cast<unsigned long long>(epoch->Sequence()),
			VitaGpuVu::UniversalGpuVuRejectionName(reason),
			static_cast<u32>(state), static_cast<u32>(product_hot_state),
			product_hot_bundle.module_count, epoch->PreflightPairCount());
		reject(reason, epoch->PreflightPairCount());
		return;
	}
	const bool product_plan_profitable = product_hot_ready ?
		product_hot_bundle.IsLowJobProductCandidate(
			epoch->DynamicPairUpperBound(), epoch->UnpackSubmissionCount()) :
		VitaGpuVu::UniversalGpuVuGeneratedEntryPlanIsProfitable(
			epoch->DynamicPairUpperBound(), epoch->UnpackSubmissionCount());
	if (!product_plan_profitable)
	{
		Console.WriteLn(
			"GPU-VU seq=%llu provider=cpu-mtvu accepted=0 pre_effect=1 "
			"reason=generated-program-unprofitable pairs=%u dynamic_bound=%u "
			"unpack_jobs=%u generated_vu_jobs=%u generated_modules=%u "
			"job_limit=%u cpu_fallback=1.",
			static_cast<unsigned long long>(epoch->Sequence()),
			epoch->PreflightPairCount(), epoch->DynamicPairUpperBound(),
			epoch->UnpackSubmissionCount(),
			product_hot_ready ? product_hot_bundle.stage_count : 1u,
			product_hot_ready ? product_hot_bundle.module_count : 1u,
			VitaGpuVu::UniversalGpuVuMaximumGeneratedEntryFirmwareJobs);
		reject(VitaGpuVu::UniversalGpuVuRejection::GeneratedProgramUnprofitable,
			epoch->PreflightPairCount());
		return;
	}
		const auto resolve_payload = [&]() -> const u8* {
		const u8* payload = static_cast<const u8*>(
			gpu_vu_universal_product_shared.base) +
			GPU_VU_UNIVERSAL_PRODUCT_ZERO_PAYLOAD_OFFSET;
		if (!epoch->Unpacks().empty())
		{
			VitaGpuVu::RawVifPayloadRef window =
				epoch->Unpacks().front().payload;
			window.offset = epoch->PayloadBaseOffset();
			window.size = epoch->PayloadSize();
			payload = VitaGpuVu::ResolveGpuRawVifPayload(window);
		}
			return payload;
		};
		const auto vf_offset = [](u32 bank) {
			return bank == 0u ? GPU_VU_UNIVERSAL_PRODUCT_VF_OFFSET :
				GPU_VU_UNIVERSAL_PRODUCT_OUTPUT_VF_OFFSET;
		};
		const auto state_offset = [](u32 bank) {
			return bank == 0u ? GPU_VU_UNIVERSAL_PRODUCT_STATE_OFFSET :
				GPU_VU_UNIVERSAL_PRODUCT_OUTPUT_STATE_OFFSET;
		};
		const auto memory_offset = [](u32 bank) {
			return bank == 0u ? GPU_VU_UNIVERSAL_PRODUCT_MEMORY_OFFSET :
				GPU_VU_UNIVERSAL_PRODUCT_OUTPUT_MEMORY_OFFSET;
		};
		const VitaGpuVu::StructuredGeneratedBundle& structured =
			epoch->StructuredBundle();
		const bool structured_owner_matches = structured.MatchesOwner(
			epoch->ProgramIdentity(), epoch->AnalysisEntryPc(),
			epoch->ConfigurationBits());
		VitaGpuVu::StructuredGeneratedBundle live_structured;
		VitaGpuVu::StructuredGeneratedBundleState live_structured_state =
			VitaGpuVu::StructuredGeneratedBundleState::Missing;
		bool live_structured_generation_ready = false;
		bool structured_bundle_stale_or_quarantined = false;
		if (structured.HasKeys())
		{
			live_structured_state = VitaGpuVu::QueryStructuredGeneratedBundle(
				epoch->ProgramIdentity(), &live_structured);
			live_structured_generation_ready =
				live_structured_state ==
					VitaGpuVu::StructuredGeneratedBundleState::Ready &&
				structured_owner_matches &&
				structured.MatchesLiveGeneration(live_structured);
			structured_bundle_stale_or_quarantined = !structured_owner_matches ||
				live_structured_state ==
					VitaGpuVu::StructuredGeneratedBundleState::Failed ||
				live_structured_state ==
					VitaGpuVu::StructuredGeneratedBundleState::Unavailable ||
				live_structured_state ==
					VitaGpuVu::StructuredGeneratedBundleState::Missing ||
				(live_structured_state ==
					VitaGpuVu::StructuredGeneratedBundleState::Ready &&
				 !live_structured_generation_ready);
			if (structured_bundle_stale_or_quarantined &&
				epoch->Stage() == VitaGpuVu::UniversalGpuVuEpochStage::Prepared)
			{
				const bool newly_quarantined =
					VitaGpuVu::RejectStructuredGeneratedBundle(
						epoch->ProgramIdentity());
				if (!structured_owner_matches)
					++gpu_vu_universal_product_structured_owner_reject_count;
				const u64 count =
					++gpu_vu_universal_product_structured_quarantine_reject_count;
				if (count <= 8 || (count & (count - 1)) == 0)
				{
				Console.Error(
					"GPU-VU seq=%llu provider=generated-structured accepted=0 "
					"pre_effect=1 reason=structured-bundle-quarantined "
					"cache_state=%u "
					"epoch_owner=%llu bundle_owner=%llu analysis_pc=%u "
					"bundle_analysis_pc=%u configuration=%08x "
					"bundle_configuration=%08x snapshot_generation=%llu "
					"live_generation=%llu live_match=%u quarantined=%u "
					"cpu_fallback=1 reject_count=%llu.",
					static_cast<unsigned long long>(epoch->Sequence()),
					static_cast<u32>(live_structured_state),
					static_cast<unsigned long long>(epoch->ProgramIdentity()),
					static_cast<unsigned long long>(structured.program_identity),
					epoch->AnalysisEntryPc(), structured.analysis_start_pc,
					epoch->ConfigurationBits(), structured.configuration_bits,
					static_cast<unsigned long long>(structured.cache_generation),
					static_cast<unsigned long long>(
						live_structured.cache_generation),
					static_cast<u32>(live_structured_generation_ready),
					static_cast<u32>(newly_quarantined),
					static_cast<unsigned long long>(count));
				}
				reject(VitaGpuVu::UniversalGpuVuRejection::
					StructuredBundleQuarantined, epoch->PreflightPairCount());
				return;
			}
		}
				std::array<GeneratedVuProgram*,
				VitaGpuVu::StructuredGeneratedMaximumPartitionModuleCount>
				structured_partition_programs{};
			for (u32 index = 0; index < structured.partition_module_count &&
				index < structured_partition_programs.size(); index++)
			{
				structured_partition_programs[index] =
					FindGeneratedVuProgram(structured.partition_keys[index]);
			}
		const auto structured_program_ready = [](const GeneratedVuProgram* program,
			VitaGpuVu::GeneratedCgExecutionKind kind) {
			return program && program->metadata.execution_kind == kind &&
				program->vertex_program && program->general_fragment_program;
		};
			bool generated_structured_ready = structured.HasKeys() &&
				live_structured_generation_ready &&
				structured_owner_matches &&
				gpu_vu_structured_scratch_range_attested &&
			structured.maximum_outer_iterations == 64u &&
			structured.maximum_child_iterations != 0u &&
			structured.maximum_child_iterations <= 64u &&
			structured.maximum_fixed_state_module_expressions != 0u &&
			structured.maximum_fixed_state_module_expressions <=
				VitaGpuVu::StructuredGeneratedMaximumStateModuleWork /
					structured.maximum_outer_iterations &&
			structured.pair_upper_bound != 0u &&
				structured.TransactionPairUpperBound() != 0u &&
				structured.store_count != 0u && structured.store_count <= 8u &&
				structured.partition_module_count != 0u &&
				structured.partition_module_count <=
					structured_partition_programs.size() &&
				structured.state_module_count != 0u &&
				structured.state_module_count <=
					VitaGpuVu::FixedStructuredStateMaximumModules &&
				structured.fixed_state_descriptor_words >=
					VitaGpuVu::FixedStructuredStateHeaderWords &&
				structured.fixed_state_descriptor_words <=
					VitaGpuVu::FixedStructuredStateMaximumWords &&
				structured.scratch_module_count +
					structured.fixed_qp_numeric_module_count +
					structured.fixed_fmac_numeric_module_count +
					structured.memory_store_module_count +
					structured.final_module_count ==
					structured.partition_module_count &&
				structured.memory_store_module_count != 0u &&
				structured.final_module_count != 0u &&
			structured.memory_preflight.header0[0] ==
				VitaGpuVu::StructuredMemoryPreflightFormatVersion &&
			structured.memory_preflight.header0[1] ==
				static_cast<u32>(structured.runtime_memory_preflight) &&
			structured.memory_preflight.header0[2] <=
				VitaGpuVu::StructuredMemoryPreflightMaximumAccesses &&
			(structured.memory_preflight.header0[2] == 0u ||
			 structured.maximum_child_iterations <=
				VitaGpuVu::StructuredGeneratedMaximumPreflightAccessWork /
					structured.memory_preflight.header0[2]) &&
			structured.memory_preflight.header0[3] +
				structured.memory_preflight.header1[0] ==
				structured.memory_preflight.header0[2] &&
			(!structured.runtime_memory_preflight ||
			 structured.memory_preflight.header1[0] == structured.store_count) &&
			structured.memory_preflight.header2[0] ==
				structured.maximum_outer_iterations &&
			structured.memory_preflight.header2[1] ==
				structured.maximum_child_iterations &&
			structured.parallel_child_memory_store &&
			structured.runtime_memory_preflight &&
				gpu_vu_structured_control_product_vertex_program &&
				gpu_vu_structured_control_product_fragment_program &&
				gpu_vu_structured_state_product_vertex_program &&
				gpu_vu_structured_state_product_fragment_program &&
				gpu_vu_structured_preflight_build_product_vertex_program &&
				gpu_vu_structured_preflight_build_product_fragment_program &&
				gpu_vu_structured_preflight_reduce_product_vertex_program &&
				gpu_vu_structured_preflight_reduce_product_fragment_program &&
				gpu_vu_structured_preflight_finalize_product_vertex_program &&
			gpu_vu_structured_preflight_finalize_product_fragment_program &&
				gpu_vu_structured_qp_numeric_product_vertex_program &&
					gpu_vu_structured_qp_numeric_product_fragment_program &&
				gpu_vu_structured_fmac_numeric_product_vertex_program &&
					gpu_vu_structured_fmac_numeric_product_fragment_program &&
				gpu_vu_structured_fmac_native_product_vertex_program &&
					gpu_vu_structured_fmac_native_product_fragment_program;
		const u32 structured_preflight_barrier_submission =
			epoch->UnpackSubmissionCount() +
			VitaGpuVu::StructuredGeneratedEntryJobCount +
			VitaGpuVu::StructuredGeneratedPartitionStageBase;
				u32 actual_scratch_modules = 0;
			u32 actual_fixed_qp_numeric_modules = 0;
			u32 actual_fixed_fmac_numeric_modules = 0;
			u32 actual_memory_store_modules = 0;
			u32 actual_final_modules = 0;
			for (u32 index = 0; index < structured.partition_module_count; index++)
			{
					const auto kind = structured.partition_kinds[index];
					generated_structured_ready &=
						VitaGpuVu::IsStructuredGeneratedPartitionKind(kind) &&
						kind != VitaGpuVu::GeneratedCgExecutionKind::
							StructuredStateSnapshots;
				if (kind == VitaGpuVu::GeneratedCgExecutionKind::
					StructuredFixedQpNumeric)
				{
					generated_structured_ready &=
						structured_partition_programs[index] == nullptr &&
						structured.partition_fixed_qp_numeric[index].IsValid();
				}
				else if (kind == VitaGpuVu::GeneratedCgExecutionKind::
					StructuredFixedFmacNumeric)
				{
					generated_structured_ready &=
						structured_partition_programs[index] == nullptr &&
						structured.partition_fixed_fmac_operation_counts[index] != 0u &&
						structured.partition_fixed_fmac_operation_counts[index] <=
							VitaGpuVu::StructuredFixedFmacNumericMaximumOperations;
				}
				else
				{
					generated_structured_ready &= structured_program_ready(
						structured_partition_programs[index], kind);
				}
				switch (kind)
				{
						case VitaGpuVu::GeneratedCgExecutionKind::
							StructuredStateSnapshots:
							break;
					case VitaGpuVu::GeneratedCgExecutionKind::
						StructuredExpressionScratch:
						actual_scratch_modules++;
						break;
					case VitaGpuVu::GeneratedCgExecutionKind::
						StructuredFixedQpNumeric:
						actual_fixed_qp_numeric_modules++;
						break;
					case VitaGpuVu::GeneratedCgExecutionKind::
						StructuredFixedFmacNumeric:
						actual_fixed_fmac_numeric_modules++;
						break;
					case VitaGpuVu::GeneratedCgExecutionKind::
						StructuredParallelChildMemoryStore:
						actual_memory_store_modules++;
						break;
					case VitaGpuVu::GeneratedCgExecutionKind::StructuredFinalState:
						actual_final_modules++;
						break;
					default:
						break;
				}
				}
				generated_structured_ready &=
					actual_scratch_modules == structured.scratch_module_count &&
				actual_fixed_qp_numeric_modules ==
					structured.fixed_qp_numeric_module_count &&
				actual_fixed_fmac_numeric_modules ==
					structured.fixed_fmac_numeric_module_count &&
				actual_memory_store_modules ==
					structured.memory_store_module_count &&
				actual_final_modules == structured.final_module_count;
		const bool structured_native_fmac =
			(epoch->ConfigurationBits() &
			 VitaGpuVu::UniversalConfigurationApproximateFmac) != 0u;
		generated_structured_ready &=
			actual_fixed_fmac_numeric_modules == 0u ||
			(structured_native_fmac ?
				gpu_vu_structured_fmac_native_attestation !=
					StructuredNativeFmacAttestation::Unavailable :
				gpu_vu_structured_fmac_exact_attested);
		u32 observed_pairs = 0u;
		u32 profiled_fixed_jobs = 0u;
		for (const UniversalGpuVuObservedPairBound& observed :
			gpu_vu_universal_observed_pair_bounds)
		{
			if (observed.program_identity == epoch->ProgramIdentity() &&
				observed.execute_count == epoch->ExecuteCount() &&
				observed.unpack_submission_count ==
					epoch->UnpackSubmissionCount())
			{
				observed_pairs = observed.observed_pairs;
				profiled_fixed_jobs =
					observed.minimum_fixed_firmware_jobs;
				break;
			}
		}
		const bool profiled_compact_fixed =
			epoch->CanUseCompactScheduledContinuation() &&
			gpu_vu_universal_compact_product_vertex_program &&
			gpu_vu_universal_compact_product_fragment_program;
		const u32 generated_jobs = structured.HasKeys() ?
			VitaGpuVu::StructuredGeneratedFirmwareJobCount(
				0u, structured.partition_firmware_job_count) : 0u;
		const bool profiled_generated_profitable = observed_pairs != 0u &&
			generated_jobs < profiled_fixed_jobs;
		if (generated_structured_ready && !profiled_generated_profitable)
		{
			generated_structured_ready = false;
			const u64 count =
				++gpu_vu_universal_product_dispatch_cost_reject_count;
			if (count <= 8u || (count & (count - 1u)) == 0u)
			{
				Console.WriteLn(
					"GPU-VU seq=%llu provider=generated-structured selected=0 "
					"reason=profiled-firmware-cost observed_pairs=%u "
					"fixed_jobs=%u generated_jobs=%u compact_fixed=%u "
					"fixed_universal=1 reject_count=%llu.",
					static_cast<unsigned long long>(epoch->Sequence()),
					observed_pairs, profiled_fixed_jobs, generated_jobs,
					static_cast<u32>(profiled_compact_fixed),
					static_cast<unsigned long long>(count));
			}
		}
			const auto submit_slot = [&](UniversalGpuVuProductSlot* slot,
				const u8* payload, bool continuation,
				u32 submission_count, u32 submission_base,
				u32 structured_submission_total,
				u32 hot_module_index) -> bool {
			const bool structured_transaction =
				structured_submission_total != 0u;
			const bool hot_transaction =
				!structured_transaction && product_hot_ready;
			const u32 hot_pair_limit = hot_transaction ?
				generated_hot_pair_limit(hot_module_index) : 0u;
				const u32 expected_hot_group = hot_transaction ?
					(continuation ? 0u : epoch->UnpackSubmissionCount()) + 1u : 0u;
			const u32 expected_structured_group = structured_transaction ?
				VitaGpuVu::UniversalGpuVuStructuredSubmissionGroupCount(
					structured_submission_total, submission_base,
					structured_preflight_barrier_submission) : 0u;
			if (!slot || !payload || submission_count == 0 ||
				submission_count > VitaGpuVu::UniversalGpuVuMaximumSubmissionsPerBatch ||
				(structured_transaction &&
					(!generated_structured_ready || expected_structured_group == 0u ||
					 submission_count != expected_structured_group ||
					 continuation != (submission_base != 0u))) ||
					(hot_transaction && (submission_count != expected_hot_group ||
						hot_pair_limit == 0u)) ||
					(hot_transaction &&
						hot_module_index >= product_hot_bundle.module_count) ||
					(!hot_transaction && hot_module_index != NO_GENERATED_HOT_MODULE) ||
				(!structured_transaction && submission_base != 0u))
			{
				Console.Error(
					"GPU-VU seq=%llu provider=universal submission_precondition=0 "
						"slot=%u payload=%u batch=%u base=%u total=%u structured=%u hot=%u "
						"hot_module=%u "
					"maximum=%u unpacks=%u; disabling "
					"admission before further PairPlan analysis.",
					static_cast<unsigned long long>(epoch->Sequence()), slot ? 1u : 0u,
					payload ? 1u : 0u, submission_count, submission_base,
					structured_submission_total,
						static_cast<u32>(structured_transaction),
						static_cast<u32>(hot_transaction),
						hot_module_index,
					VitaGpuVu::UniversalGpuVuMaximumSubmissionsPerBatch,
					static_cast<u32>(epoch->Unpacks().size()));
			VitaGpuVu::SetUniversalGpuVuDeviceAvailable(false);
			return false;
		}
		u8* const base = static_cast<u8*>(slot->buffer.base);
		if (hot_transaction)
		{
			// The preceding notification makes both non-cached mapped private
			// generations CPU-writable here. Select the work envelope from the
			// exact registered root which owns the current TPC, and publish the
			// same immutable limit to both ping-pong banks before rebinding either
			// one as BUFFER2/12.
			reinterpret_cast<u32*>(base + state_offset(0u))[63] = hot_pair_limit;
			reinterpret_cast<u32*>(base + state_offset(1u))[63] = hot_pair_limit;
		}
		const bool begin_dedicated_scene = !scene_active;
		if (begin_dedicated_scene)
		{
			const int begin_result = sceGxmBeginScene(context, 0,
				gpu_vu_universal_product_target, nullptr, nullptr, nullptr,
				gpu_vu_universal_product_color_surface->ColorSurface(),
				&disabled_depth);
			if (begin_result < 0)
			{
				Console.Error(
					"GPU-VU seq=%llu provider=universal begin_scene_error=%08x; "
					"disabling admission before further PairPlan analysis.",
					static_cast<unsigned long long>(epoch->Sequence()),
					static_cast<u32>(begin_result));
				VitaGpuVu::SetUniversalGpuVuDeviceAvailable(false);
				return false;
			}
			scene_active = true;
			scene_is_display = false;
			scene_rt = nullptr;
			scene_ds = nullptr;
			ConfigureRaster(1, 1);
		}
		sceGxmSetFrontDepthFunc(context, SCE_GXM_DEPTH_FUNC_ALWAYS);
		sceGxmSetBackDepthFunc(context, SCE_GXM_DEPTH_FUNC_ALWAYS);
		sceGxmSetFrontDepthWriteEnable(context, SCE_GXM_DEPTH_WRITE_DISABLED);
		sceGxmSetBackDepthWriteEnable(context, SCE_GXM_DEPTH_WRITE_DISABLED);
		sceGxmSetFrontStencilFunc(context, SCE_GXM_STENCIL_FUNC_ALWAYS,
			SCE_GXM_STENCIL_OP_KEEP, SCE_GXM_STENCIL_OP_KEEP,
			SCE_GXM_STENCIL_OP_KEEP, 0xff, 0xff);
		sceGxmSetBackStencilFunc(context, SCE_GXM_STENCIL_FUNC_ALWAYS,
			SCE_GXM_STENCIL_OP_KEEP, SCE_GXM_STENCIL_OP_KEEP,
			SCE_GXM_STENCIL_OP_KEEP, 0xff, 0xff);
		GeneratedVuProgram* const generated_program =
			FindGeneratedVuProgram(epoch->GeneratedProgramKey());
		const bool generated_state_machine = generated_program &&
			generated_program->metadata.execution_kind ==
				VitaGpuVu::GeneratedCgExecutionKind::UniversalStateMachine;
		const bool generated_direct_continuation = generated_program &&
			(generated_program->metadata.execution_kind ==
				VitaGpuVu::GeneratedCgExecutionKind::UniversalDirectStateMachine ||
			 generated_program->metadata.execution_kind ==
				VitaGpuVu::GeneratedCgExecutionKind::UniversalCompactContinuation);
		const bool generated_serial = generated_program &&
			(generated_state_machine || generated_direct_continuation) &&
			generated_program->vertex_program &&
			generated_program->general_fragment_program;
		const bool compact_serial =
			epoch->CanUseCompactScheduledContinuation() &&
			gpu_vu_universal_compact_product_vertex_program &&
			gpu_vu_universal_compact_product_fragment_program;
		const bool vif_unpack_kernel_ready =
			gpu_vu_vif_unpack_product_vertex_program &&
			gpu_vu_vif_unpack_product_fragment_program &&
			gpu_vu_vif_unpack_independent_product_vertex_program &&
			gpu_vu_vif_unpack_independent_product_fragment_program;
		const bool path1_commit_kernel_ready =
			gpu_vu_path1_commit_product_vertex_program &&
			gpu_vu_path1_commit_product_fragment_program;
		if (!path1_commit_kernel_ready)
		{
			Console.Error(
				"GPU-VU seq=%llu provider=universal "
				"missing-terminal-path1-commit-kernel=1; disabling admission.",
				static_cast<unsigned long long>(epoch->Sequence()));
			VitaGpuVu::SetUniversalGpuVuDeviceAvailable(false);
			if (begin_dedicated_scene)
				EndScene(false);
			return false;
		}
		ConfigureGpuVuComputeRaster(context);
		bool used_generated_serial = false;
		bool used_generated_hot = false;
			bool used_generated_structured = structured_transaction;
		bool used_compact_serial = false;
			const u8* const path1_base =
				base + GPU_VU_UNIVERSAL_PRODUCT_PATH1_OFFSET;
			int result = 0;
			const char* failed_operation = "none";
			u32 failed_submission = 0;
			u32 firmware_job_count = 0;
			u32 path1_commit_job_count = 0;
				const u64 sequence = epoch->Sequence();
				const bool trace_stage_notifications = structured_transaction &&
					VitaPerformanceTelemetry::IsEnabled() && submission_count > 1u &&
					(sequence <= 8u || (sequence & (sequence - 1u)) == 0u);
				u32 notification_start_value = slot->notification.value;
				if (trace_stage_notifications &&
					notification_start_value >
						std::numeric_limits<u32>::max() - submission_count)
				{
					// A product slot is reused only after its prior final notification.
					// Resetting the completed word here makes the rare wrap unambiguous.
					notification_start_value = 0u;
					*slot->notification.address = 0u;
				}
				u32 notification_value = notification_start_value +
					(trace_stage_notifications ? submission_count : 1u);
			if (notification_value == 0u)
			{
				notification_start_value = 0u;
				notification_value = 1u;
				*slot->notification.address = 0u;
			}
			if (trace_stage_notifications)
			{
				for (u32 index = 0; index < submission_count; index++)
				{
					slot->stage_notifications[index].value =
						notification_start_value + index + 1u;
				}
			}
			slot->notification.value = notification_value;
			if (result >= 0)
			{
				const bool notification_set = continuation ?
					epoch->SetContinuationSubmissionNotification(
						reinterpret_cast<uptr>(slot->notification.address),
						notification_value, notification_start_value,
						submission_base,
						trace_stage_notifications ? submission_count : 1u) :
					epoch->SetSubmissionNotification(
						reinterpret_cast<uptr>(slot->notification.address),
						notification_value, notification_start_value,
						submission_base,
						trace_stage_notifications ? submission_count : 1u);
			if (!notification_set)
			{
				failed_operation = "publish-notification";
					result = SCE_GXM_ERROR_INVALID_VALUE;
				}
			}
			u32 current_bank = slot->source_bank;
				for (u32 submission = 0; submission < submission_count && result >= 0;
					submission++)
				{
					const u32 logical_submission = structured_transaction ?
						submission_base + submission : submission;
					failed_submission = logical_submission;
				// BUFFER13 writes become visible at each mid-scene flush. The fixed
				// core therefore owns one job per UNPACK plus the first Execute job;
				// only then may a generated/compact continuation assume command input
				// has been fully published into the private generation.
					// An entry-capable generated root consumes BUFFER4 and owns the
					// first Execute pair itself.  Only the retired fixed/structured
					// paths need an additional fixed command-entry job.
					const u32 command_transition_jobs = structured_transaction ?
						epoch->UnpackSubmissionCount() + 1u : continuation ? 0u :
						epoch->UnpackSubmissionCount() +
							(generated_serial || hot_transaction ? 0u : 1u);
					const u32 structured_entry_end = command_transition_jobs +
						(structured_transaction ?
							VitaGpuVu::StructuredGeneratedEntryJobCount - 1u : 0u);
					const bool command_transition_job =
						logical_submission < command_transition_jobs;
					const bool vif_unpack_job =
						(structured_transaction || !continuation) &&
						logical_submission < epoch->UnpackSubmissionCount();
					const bool independent_vif_unpack_job = vif_unpack_job &&
						epoch->CanUseIndependentUnpackSubmission(
							logical_submission);
					const bool continuation_job = !command_transition_job;
					const u32 structured_stage =
						logical_submission >= structured_entry_end ?
						logical_submission - structured_entry_end :
						std::numeric_limits<u32>::max();
			const u32 structured_generated_stage_count =
				structured.GeneratedStageCount();
					const bool structured_job = structured_transaction &&
						VitaGpuVu::IsStructuredGeneratedModuleStage(
							structured_stage, structured_generated_stage_count);
					const bool structured_tail_job = structured_transaction &&
						VitaGpuVu::IsStructuredGeneratedTailStage(
						structured_stage, structured_generated_stage_count);
				const bool structured_transaction_job =
					structured_job || structured_tail_job;
					const bool hot_stage_job = hot_transaction &&
						logical_submission >= command_transition_jobs;
					const bool use_generated = generated_serial && continuation_job &&
						!structured_transaction_job;
					const bool use_generated_hot = hot_stage_job;
				const bool use_compact = !use_generated && compact_serial &&
					continuation_job && !structured_transaction_job &&
					!use_generated_hot;
				if (vif_unpack_job && !vif_unpack_kernel_ready)
				{
					failed_operation = "missing-vif-unpack-kernel";
					result = SCE_GXM_ERROR_INVALID_POINTER;
					break;
				}
					GeneratedVuProgram* structured_program = nullptr;
					const bool structured_control_job = structured_job &&
						structured_stage ==
							VitaGpuVu::StructuredGeneratedControlStage;
					const bool structured_fixed_state_job = structured_job &&
						VitaGpuVu::IsStructuredGeneratedFixedStateStage(
							structured_stage);
					const u32 structured_fixed_state_slice =
						VitaGpuVu::StructuredGeneratedFixedStateSlice(
							structured_stage);
					const bool structured_preflight_build_job = structured_job &&
						structured_stage ==
							VitaGpuVu::StructuredGeneratedPreflightBuildStage;
					const bool structured_preflight_reduce_job = structured_job &&
						structured_stage ==
							VitaGpuVu::StructuredGeneratedPreflightReduceStage;
					const bool structured_preflight_finalize_job = structured_job &&
						structured_stage ==
							VitaGpuVu::StructuredGeneratedPreflightFinalizeStage;
					VitaGpuVu::StructuredGeneratedPartitionStage
						structured_partition_stage;
					const bool structured_partition_job = structured_job &&
						structured.MapPartitionStage(
							structured_stage, &structured_partition_stage);
					u32 structured_partition_index =
						structured_partition_job ?
							structured_partition_stage.partition_index :
							std::numeric_limits<u32>::max();
					if (structured_partition_job)
					{
						if (structured_partition_index <
							structured.partition_module_count)
						{
							structured_program = structured_partition_programs[
								structured_partition_index];
						}
					}
					else if (structured_job &&
						structured_stage >=
							VitaGpuVu::StructuredGeneratedPartitionStageBase)
					{
						failed_operation = "structured-partition-stage-map";
						result = SCE_GXM_ERROR_INVALID_VALUE;
						break;
					}
					const bool structured_qp_numeric_job =
						structured_partition_index <
							structured.partition_module_count &&
						structured.partition_kinds[structured_partition_index] ==
							VitaGpuVu::GeneratedCgExecutionKind::
								StructuredFixedQpNumeric;
					const bool structured_fmac_numeric_job =
						structured_partition_index <
							structured.partition_module_count &&
						structured.partition_kinds[structured_partition_index] ==
							VitaGpuVu::GeneratedCgExecutionKind::
								StructuredFixedFmacNumeric;
					const u32 hot_first_module = hot_module_index;
					if (use_generated_hot &&
						(hot_first_module >= product_hot_bundle.module_count ||
						 !product_hot_programs[hot_first_module]))
				{
					failed_operation = "generated-hot-stage-map";
					result = SCE_GXM_ERROR_INVALID_VALUE;
					break;
				}
				GeneratedVuProgram* const hot_program = use_generated_hot ?
					product_hot_programs[hot_first_module] : nullptr;
				sceGxmSetVertexProgram(context, independent_vif_unpack_job ?
					gpu_vu_vif_unpack_independent_product_vertex_program : vif_unpack_job ?
					gpu_vu_vif_unpack_product_vertex_program : use_generated ?
					generated_program->vertex_program : use_generated_hot ?
					hot_program->vertex_program : structured_control_job ?
					gpu_vu_structured_control_product_vertex_program :
					structured_fixed_state_job ?
					gpu_vu_structured_state_product_vertex_program :
					structured_preflight_build_job ?
					gpu_vu_structured_preflight_build_product_vertex_program :
					structured_preflight_reduce_job ?
					gpu_vu_structured_preflight_reduce_product_vertex_program :
					structured_preflight_finalize_job ?
					gpu_vu_structured_preflight_finalize_product_vertex_program :
					structured_qp_numeric_job ?
					gpu_vu_structured_qp_numeric_product_vertex_program :
					structured_fmac_numeric_job ?
						(structured_native_fmac ?
							gpu_vu_structured_fmac_native_product_vertex_program :
							gpu_vu_structured_fmac_numeric_product_vertex_program) : structured_program ?
					structured_program->vertex_program : use_compact ?
					gpu_vu_universal_compact_product_vertex_program :
					gpu_vu_universal_product_vertex_program);
				sceGxmSetFragmentProgram(context, independent_vif_unpack_job ?
					gpu_vu_vif_unpack_independent_product_fragment_program : vif_unpack_job ?
					gpu_vu_vif_unpack_product_fragment_program : use_generated ?
					generated_program->general_fragment_program : use_generated_hot ?
					hot_program->general_fragment_program : structured_control_job ?
					gpu_vu_structured_control_product_fragment_program :
					structured_fixed_state_job ?
					gpu_vu_structured_state_product_fragment_program :
					structured_preflight_build_job ?
					gpu_vu_structured_preflight_build_product_fragment_program :
					structured_preflight_reduce_job ?
					gpu_vu_structured_preflight_reduce_product_fragment_program :
					structured_preflight_finalize_job ?
					gpu_vu_structured_preflight_finalize_product_fragment_program :
					structured_qp_numeric_job ?
					gpu_vu_structured_qp_numeric_product_fragment_program :
					structured_fmac_numeric_job ?
						(structured_native_fmac ?
							gpu_vu_structured_fmac_native_product_fragment_program :
							gpu_vu_structured_fmac_numeric_product_fragment_program) : structured_program ?
					structured_program->general_fragment_program : use_compact ?
					gpu_vu_universal_compact_product_fragment_program :
					gpu_vu_universal_product_fragment_program);
				used_generated_serial |= use_generated;
				used_generated_hot |= use_generated_hot;
				used_generated_structured |= structured_job;
				used_compact_serial |= use_compact;
					// Each dependency job reads one private generation and publishes a
					// complete successor to the other. Fixed/VIF jobs copy before sparse
					// writes; the fixed structured evaluator and preflight initialize
					// snapshots and scalar state before store/final roots update them. This avoids
					// undocumented same-address writable-uniform cache reuse across
					// sceGxmMidSceneFlush().
					const u32 source_bank = current_bank;
					const u32 output_bank = current_bank ^ 1u;
				const void* buffers[14] = {
					base + GPU_VU_UNIVERSAL_PRODUCT_MICRO_OFFSET,
					base + vf_offset(source_bank),
					base + state_offset(source_bank),
					base + memory_offset(source_bank),
					base + GPU_VU_UNIVERSAL_PRODUCT_EPOCH_OFFSET,
					payload,
					path1_base,
					static_cast<const u8*>(gpu_vu_universal_product_shared.base) +
						GPU_VU_UNIVERSAL_PRODUCT_ESTIMATE_OFFSET,
					path1_base + 1u * VitaGpuVu::UniversalRawPath1ExportPageWords * sizeof(u32),
					path1_base + 2u * VitaGpuVu::UniversalRawPath1ExportPageWords * sizeof(u32),
					path1_base + 3u * VitaGpuVu::UniversalRawPath1ExportPageWords * sizeof(u32),
					base + vf_offset(output_bank),
					base + state_offset(output_bank),
					base + memory_offset(output_bank),
				};
					if (structured_job)
					{
						buffers[9] = base +
							GPU_VU_UNIVERSAL_PRODUCT_JOURNAL_ADDRESS_OFFSET;
					buffers[10] = base +
						GPU_VU_UNIVERSAL_PRODUCT_JOURNAL_VALUE_OFFSET;
					buffers[11] = base +
						GPU_VU_UNIVERSAL_PRODUCT_SNAPSHOT_OFFSET;
						buffers[12] = base + (structured_stage >=
							VitaGpuVu::StructuredGeneratedPartitionStageBase ?
							GPU_VU_UNIVERSAL_PRODUCT_OUTER_STATE_OUTPUT_OFFSET :
							GPU_VU_UNIVERSAL_PRODUCT_OUTER_STATE_OFFSET);
						buffers[13] =
							base + GPU_VU_UNIVERSAL_PRODUCT_VI_SNAPSHOT_OFFSET;
							if (structured_control_job || structured_fixed_state_job ||
								structured_preflight_build_job ||
								structured_preflight_reduce_job ||
								structured_preflight_finalize_job)
							{
								buffers[9] = base +
									GPU_VU_UNIVERSAL_PRODUCT_JOURNAL_VALUE_OFFSET +
									VitaGpuVu::
										StructuredGeneratedPreflightWorkspaceWords *
											sizeof(u32);
							}
							if (structured_preflight_finalize_job)
							{
								buffers[6] = base +
									GPU_VU_UNIVERSAL_PRODUCT_OUTER_STATE_OUTPUT_OFFSET;
								buffers[8] = base + state_offset(output_bank);
								buffers[0] = base +
									GPU_VU_UNIVERSAL_PRODUCT_JOURNAL_ADDRESS_OFFSET;
							}
							else if (structured_control_job || structured_fixed_state_job)
							{
								buffers[0] = base +
									GPU_VU_UNIVERSAL_PRODUCT_JOURNAL_ADDRESS_OFFSET;
							}
						else if (structured_partition_index <
							structured.partition_module_count)
						{
							switch (structured.partition_kinds[
								structured_partition_index])
							{
								case VitaGpuVu::GeneratedCgExecutionKind::
									StructuredExpressionScratch:
									break;
								case VitaGpuVu::GeneratedCgExecutionKind::
									StructuredFixedQpNumeric:
									buffers[0] = base +
										GPU_VU_UNIVERSAL_PRODUCT_QP_DESCRIPTOR_OFFSET +
										structured_partition_index *
											sizeof(VitaGpuVu::
											StructuredFixedQpNumericDescriptor);
									break;
								case VitaGpuVu::GeneratedCgExecutionKind::
									StructuredFixedFmacNumeric:
									buffers[0] = base +
										GPU_VU_UNIVERSAL_PRODUCT_FMAC_DESCRIPTOR_OFFSET +
										structured_partition_index *
											sizeof(VitaGpuVu::
												StructuredFixedFmacNumericDescriptor);
									break;
								case VitaGpuVu::GeneratedCgExecutionKind::
									StructuredParallelChildMemoryStore:
									buffers[8] = base + memory_offset(output_bank);
									break;
								case VitaGpuVu::GeneratedCgExecutionKind::
									StructuredFinalState:
									buffers[8] = base + vf_offset(output_bank);
									buffers[9] = base + state_offset(output_bank);
									break;
								default:
									break;
							}
						}
						}
				for (u32 index = 0; index < std::size(buffers) && result >= 0;
					index++)
				{
					failed_operation = "set-uniform-buffer";
					result = sceGxmSetVertexUniformBuffer(context, index,
						buffers[index]);
				}
				if (result < 0)
					break;
				failed_operation = "draw";
					u32 draw_count = 1u;
					if (use_generated_hot)
						draw_count = product_hot_bundle.modules[
							hot_first_module].invocation_count;
					else if (structured_fixed_state_job)
						draw_count = structured.state_module_count;
					else if (structured_preflight_build_job)
						draw_count = VitaGpuVu::StructuredGeneratedPreflightOuterCount;
					else if (structured_preflight_reduce_job)
						draw_count = VitaGpuVu::StructuredGeneratedPreflightAddressCount;
					else if (structured_partition_index <
						structured.partition_module_count)
					{
						draw_count =
							structured_partition_stage.invocation_count;
					}
				const u16* draw_indices = gpu_vu_sequential_indices;
				if (structured_fixed_state_job)
				{
					static_assert(
						VitaGpuVu::StructuredGeneratedFixedStateJobCount *
							VitaGpuVu::StructuredGeneratedFixedStateIndexStride <=
						GPU_VU_SEQUENTIAL_INDEX_COUNT);
					draw_indices += structured_fixed_state_slice *
						VitaGpuVu::StructuredGeneratedFixedStateIndexStride;
				}
				else if (structured_partition_job)
				{
					if (structured_partition_stage.invocation_offset >=
							GPU_VU_SEQUENTIAL_INDEX_COUNT ||
						draw_count > GPU_VU_SEQUENTIAL_INDEX_COUNT -
							structured_partition_stage.invocation_offset)
					{
						failed_operation = "structured-partition-index-range";
						result = SCE_GXM_ERROR_INVALID_VALUE;
						break;
					}
					draw_indices += structured_partition_stage.invocation_offset;
				}
				result = sceGxmDraw(context, SCE_GXM_PRIMITIVE_POINTS,
					SCE_GXM_INDEX_FORMAT_U16, draw_indices, draw_count);
				const bool flush_after_draw = !structured_transaction_job ||
					VitaGpuVu::StructuredGeneratedStageNeedsFlush(
						structured_stage, structured_generated_stage_count);
				// A fixed/compact core publishes terminal phase 3 and every later
				// prequeued serial core preserves that private state. Scan for the
				// delayed terminal XGKICK once from the final bank in this bounded
				// group instead of submitting the same no-op helper after every core.
				// The structured entry transition retains its immediate dependency
				// boundary because the generated stages consume that successor.
					const bool path1_commit_job = flush_after_draw &&
						!vif_unpack_job && !use_generated && !use_generated_hot &&
						!structured_transaction_job &&
						!structured_transaction &&
						submission + 1u == submission_count;
			if (result >= 0 && flush_after_draw)
			{
				// Sony's libgxm reference defines a null notification as a valid
						// vertex-only job submission. Every compiler-bounded partition is an
						// explicit visibility boundary so a scratch consumer sees its producer
						// and the following producer may safely reuse the same slots. Traced
						// early transactions publish non-blocking progress breadcrumbs; the
						// final notification remains the only ownership boundary.
				const SceGxmNotification* const notification =
					trace_stage_notifications ?
						&slot->stage_notifications[submission] :
						submission + 1u == submission_count && !path1_commit_job ?
							&slot->notification : nullptr;
					failed_operation = "mid-scene-flush";
					result = sceGxmMidSceneFlush(context, 0, nullptr, notification);
					if (result >= 0)
					{
						firmware_job_count++;
						// Both VIF roots publish each sparse state/memory change to both
						// byte-identical private banks. Consume the opposite output bank
						// after their visibility boundary: rebinding the in-place source
						// address can refetch its pre-UNPACK writable-uniform cache image on
						// physical SGX even though the mapped bytes are correct afterward.
						// Every serial root likewise publishes dirty values to both its in-place
						// working bank and BUFFER11-13 in the opposite bank, so alternate
						// banks at every dependency boundary. Rebinding one writable-uniform
						// address let SGX reuse the preceding input generation and caused an
						// unbounded generated continuation storm on physical BSpline.
						const bool dual_generation_job = !vif_unpack_job &&
							(use_generated || use_generated_hot || use_compact ||
								(!use_generated && !use_generated_hot &&
								!structured_job));
						const bool structured_generation_complete = structured_job &&
							structured_stage + 1u == structured_generated_stage_count;
						if (vif_unpack_job || dual_generation_job ||
							structured_generation_complete)
							current_bank ^= 1u;
					}
				}
				if (result >= 0 && path1_commit_job)
				{
					// Architectural output boundary: a terminal E-bit XGKICK has no
					// following guest pair. The narrow fixed kernel appends that one
					// packet in-place after the final serial generation in this group is
					// visible. It no-ops for an ordinary continuation, so the complete
					// bounded dependency group remains free of an ARM decision or wait.
					sceGxmSetVertexProgram(context,
						gpu_vu_path1_commit_product_vertex_program);
					sceGxmSetFragmentProgram(context,
						gpu_vu_path1_commit_product_fragment_program);
					const void* const commit_buffers[6] = {
						base + state_offset(current_bank),
						base + memory_offset(current_bank),
						path1_base,
						path1_base + 1u *
							VitaGpuVu::UniversalRawPath1ExportPageWords * sizeof(u32),
						path1_base + 2u *
							VitaGpuVu::UniversalRawPath1ExportPageWords * sizeof(u32),
						path1_base + 3u *
							VitaGpuVu::UniversalRawPath1ExportPageWords * sizeof(u32),
					};
					for (u32 index = 0;
						index < std::size(commit_buffers) && result >= 0; index++)
					{
						failed_operation = "set-path1-commit-buffer";
						result = sceGxmSetVertexUniformBuffer(
							context, index, commit_buffers[index]);
					}
					if (result >= 0)
					{
						failed_operation = "draw-path1-commit";
						result = sceGxmDraw(context, SCE_GXM_PRIMITIVE_POINTS,
							SCE_GXM_INDEX_FORMAT_U16,
							gpu_vu_sequential_indices, 1u);
					}
					if (result >= 0)
					{
						const SceGxmNotification* const notification =
							submission + 1u == submission_count ?
								&slot->notification : nullptr;
						failed_operation = "flush-path1-commit";
						result = sceGxmMidSceneFlush(
							context, 0, nullptr, notification);
						if (result >= 0)
						{
							firmware_job_count++;
							path1_commit_job_count++;
						}
					}
				}
		}
		RestoreGpuVuComputeRaster(context);
		const bool stage_published = result >= 0 &&
			(continuation || epoch->MarkSubmitted());
		if (result < 0 || !stage_published)
		{
			if (result < 0)
			{
				Console.Error(
						"GPU-VU seq=%llu provider=universal submission_error=%08x "
						"operation=%s job=%u batch=%u base=%u total=%u "
						"continuation=%u; disabling "
					"admission before further PairPlan analysis.",
					static_cast<unsigned long long>(epoch->Sequence()),
					static_cast<u32>(result), failed_operation,
						failed_submission, submission_count, submission_base,
						structured_submission_total, continuation ? 1u : 0u);
				VitaGpuVu::SetUniversalGpuVuDeviceAvailable(false);
			}
			else
			{
				Console.Error(
					"GPU-VU seq=%llu provider=universal submission_stage_publish=0 "
					"stage=%u notification=%p:%u; disabling admission before "
					"further PairPlan analysis.",
					static_cast<unsigned long long>(epoch->Sequence()),
					static_cast<u32>(epoch->Stage()),
					reinterpret_cast<void*>(epoch->SubmissionNotificationAddress()),
					epoch->SubmissionNotificationValue());
				VitaGpuVu::SetUniversalGpuVuDeviceAvailable(false);
			}
			if (begin_dedicated_scene)
				EndScene(false);
			return false;
		}
		if (trace_stage_notifications)
		{
			const u32 structured_stage_logical_base =
				epoch->UnpackSubmissionCount() +
				VitaGpuVu::StructuredGeneratedEntryJobCount;
			const u32 first_logical_job = submission_base;
			const u32 last_logical_job =
				submission_base + submission_count - 1u;
			const u32 first_structured_stage =
				first_logical_job >= structured_stage_logical_base ?
					first_logical_job - structured_stage_logical_base :
					std::numeric_limits<u32>::max();
			const u32 last_structured_stage =
				last_logical_job >= structured_stage_logical_base ?
					last_logical_job - structured_stage_logical_base :
					std::numeric_limits<u32>::max();
			VitaGpuVu::StructuredGeneratedPartitionStage first_partition_stage;
			VitaGpuVu::StructuredGeneratedPartitionStage last_partition_stage;
			structured.MapPartitionStage(
				first_structured_stage, &first_partition_stage);
			structured.MapPartitionStage(
				last_structured_stage, &last_partition_stage);
			Console.WriteLn(
				"GPU-VU seq=%llu provider=generated-structured "
				"firmware_group_queued=1 logical_jobs=[%u,%u) "
				"structured_stages=[%u,%u] "
				"first_partition=%u:%u invocations=[%u,%u) work=%u "
				"last_partition=%u:%u invocations=[%u,%u) work=%u "
				"progress_notifications=%u final_notification=%u",
				static_cast<unsigned long long>(epoch->Sequence()),
				submission_base, submission_base + submission_count,
				first_structured_stage, last_structured_stage,
				first_partition_stage.partition_index,
				first_partition_stage.partition_job_index,
				first_partition_stage.invocation_offset,
				first_partition_stage.invocation_offset +
					first_partition_stage.invocation_count,
				first_partition_stage.invocation_work,
				last_partition_stage.partition_index,
				last_partition_stage.partition_job_index,
				last_partition_stage.invocation_offset,
				last_partition_stage.invocation_offset +
					last_partition_stage.invocation_count,
				last_partition_stage.invocation_work,
				submission_count, notification_value);
		}
			slot->sequence = epoch->Sequence();
			slot->submission_pair_limit = hot_transaction ? hot_pair_limit :
				compact_serial ?
					VitaGpuVu::UniversalGpuVuCompactPairsPerSubmission :
					VitaGpuVu::UniversalGpuVuWatchdogSafePairsPerSubmission;
			if (!continuation)
			{
				slot->submission_wall = VitaPerformanceTelemetry::IsEnabled() ?
					Common::Timer::GetCurrentValue() : 0;
			}
			if (continuation)
			{
				slot->submission_groups++;
				slot->continuation_groups++;
				slot->core_submissions += submission_count;
				slot->job_submissions += firmware_job_count;
			}
			else
			{
				slot->submission_groups = 1u;
				slot->continuation_groups = 0u;
				slot->core_submissions = submission_count;
				slot->job_submissions = firmware_job_count;
			}
			if (continuation)
				slot->path1_commit_submissions += path1_commit_job_count;
			else
				slot->path1_commit_submissions = path1_commit_job_count;
			if (structured_transaction)
			{
				slot->structured_submission_cursor =
					submission_base + submission_count;
				slot->structured_submission_total = structured_submission_total;
			}
			else if (!continuation)
			{
				slot->structured_submission_cursor = 0u;
				slot->structured_submission_total = 0u;
			}
				slot->completed_bank = current_bank;
		if (continuation)
		{
			slot->generated_serial |= used_generated_serial;
			slot->generated_hot |= used_generated_hot;
			slot->generated_structured |= used_generated_structured;
			slot->compact_serial |= used_compact_serial;
		}
		else
		{
			slot->generated_serial = used_generated_serial;
			slot->generated_hot = used_generated_hot;
			slot->generated_structured = used_generated_structured;
			slot->compact_serial = used_compact_serial;
		}
		slot->submitted = true;
		slot->committed = false;
		slot->dedicated_scene = begin_dedicated_scene;
		if (continuation)
		{
			VitaGpuVu::RecordUniversalGpuVuContinuationGroup(submission_count);
			gpu_vu_universal_product_continuation_count++;
		}
		return true;
	};
			const auto submission_batch_count = [&](u32 executed_pairs) {
				if (product_hot_ready)
				{
					return (executed_pairs == 0u ?
						epoch->UnpackSubmissionCount() : 0u) + 1u;
			}
			u32 bounded_pairs = epoch->DynamicPairUpperBound();
			const GeneratedVuProgram* const generated_program =
				FindGeneratedVuProgram(epoch->GeneratedProgramKey());
			const bool generated_serial = generated_program &&
				(generated_program->metadata.execution_kind ==
					VitaGpuVu::GeneratedCgExecutionKind::UniversalStateMachine ||
				 generated_program->metadata.execution_kind ==
					VitaGpuVu::GeneratedCgExecutionKind::UniversalDirectStateMachine ||
				 generated_program->metadata.execution_kind ==
					VitaGpuVu::GeneratedCgExecutionKind::UniversalCompactContinuation) &&
				generated_program->vertex_program &&
				generated_program->general_fragment_program;
			// Generated serial roots currently execute at most the conservative
			// 128-pair source loop even when the offline compact core is present.
			// Plan from the provider submit_slot() will actually select, rather than
			// undercounting the number of later notification groups.
			const bool compact_continuation = !generated_serial &&
				epoch->CanUseCompactScheduledContinuation() &&
				gpu_vu_universal_compact_product_vertex_program &&
				gpu_vu_universal_compact_product_fragment_program;
			const u32 continuation_slice = compact_continuation ?
				VitaGpuVu::UniversalGpuVuCompactPairsPerSubmission :
				VitaGpuVu::UniversalGpuVuWatchdogSafePairsPerSubmission;
			// A prior successful completion is only a scheduling hint. Retain one
			// complete watchdog slice of growth, and use it only for the initial
			// batch. If execution outgrows the hint, the existing notification-
			// driven continuation consumes the unchanged architectural bound from
			// the same private generation without committing partial state.
			if (executed_pairs == 0)
			{
				for (UniversalGpuVuObservedPairBound& observed :
					gpu_vu_universal_observed_pair_bounds)
				{
					if (observed.program_identity != epoch->ProgramIdentity() ||
						observed.execute_count != epoch->ExecuteCount() ||
						observed.unpack_submission_count !=
							epoch->UnpackSubmissionCount())
					{
						continue;
					}
					observed.last_use =
						++gpu_vu_universal_observed_pair_bound_clock;
					const u32 margin = continuation_slice;
					const u32 hinted = observed.observed_pairs <=
						std::numeric_limits<u32>::max() - margin ?
						observed.observed_pairs + margin :
						std::numeric_limits<u32>::max();
					bounded_pairs = std::min(bounded_pairs, hinted);
					break;
				}
			}
			return VitaGpuVu::UniversalGpuVuSerialSubmissionGroupCount(
				bounded_pairs, executed_pairs, epoch->UnpackSubmissionCount(),
				compact_continuation, executed_pairs == 0u);
		};

	if (epoch->Stage() == VitaGpuVu::UniversalGpuVuEpochStage::Submitted)
	{
		UniversalGpuVuProductSlot* submitted = nullptr;
		for (UniversalGpuVuProductSlot& slot :
			gpu_vu_universal_product_slots)
		{
			if (slot.submitted && slot.sequence == epoch->Sequence())
			{
				submitted = &slot;
				break;
			}
		}
		if (!submitted || !GpuVuNotificationReached(submitted->notification))
			return;
		if (submitted->dedicated_scene)
		{
			// A targetless scene exists only to host this compute-style vertex
			// invocation. The mid-scene notification proves its writes complete;
			// close it without waiting so later GS transfers cannot inherit a
			// scene whose attachment pointers are deliberately null.
			if (!EndScene(false))
			{
				submitted->submitted = false;
				submitted->dedicated_scene = false;
				reject(VitaGpuVu::UniversalGpuVuRejection::SubmissionFailed,
					epoch->PreflightPairCount());
				return;
			}
			submitted->dedicated_scene = false;
		}

			u8* const base = static_cast<u8*>(submitted->buffer.base);
			const u32 completed_bank = submitted->completed_bank;
			u32* const state = reinterpret_cast<u32*>(
				base + state_offset(completed_bank));
			// Generated roots summarize pairs from the same architectural Execute
			// budget as the fixed core. The proof-derived structured ceiling is an
			// implementation bound, never permission to exceed the VIF command's
			// maximum-pair contract.
			const u32 completion_pair_upper_bound = submitted->generated_structured ?
				std::min(structured.TransactionPairUpperBound(),
					epoch->DynamicPairUpperBound()) :
				epoch->DynamicPairUpperBound();
				const u32* const structured_outer_state =
					reinterpret_cast<const u32*>(
						base + GPU_VU_UNIVERSAL_PRODUCT_OUTER_STATE_OUTPUT_OFFSET);
		auto* const path1 =
			reinterpret_cast<VitaGpuVu::UniversalRawPath1Export*>(
				base + GPU_VU_UNIVERSAL_PRODUCT_PATH1_OFFSET);
		bool path1_valid = path1->format_version ==
				VitaGpuVu::UniversalRawPath1ExportFormatVersion &&
			path1->error == VitaGpuVu::UniversalRawPath1ExportError::None &&
			path1->packet_count <=
				VitaGpuVu::UniversalRawPath1ExportMaximumPackets &&
			path1->data_qword_count <=
				VitaGpuVu::UniversalRawPath1ExportDataQwords;
			for (u32 index = 0; path1_valid && index < path1->packet_count;
				index++)
		{
			const VitaGpuVu::UniversalRawPath1PacketDescriptor& packet =
				path1->packets[index];
			path1_valid = packet.qword_count != 0 &&
				packet.output_qword_offset <= path1->data_qword_count &&
					packet.qword_count <= path1->data_qword_count -
						packet.output_qword_offset;
			}
			VitaGpuVu::UniversalGpuVuRejection reason =
				VitaGpuVu::UniversalGpuVuRejection::None;
			if (!path1_valid)
			{
					reason = path1->error ==
						VitaGpuVu::UniversalRawPath1ExportError::PacketCapacity ||
					path1->error ==
						VitaGpuVu::UniversalRawPath1ExportError::DescriptorCapacity ?
					VitaGpuVu::UniversalGpuVuRejection::RuntimeOutputCapacity :
					VitaGpuVu::UniversalGpuVuRejection::RuntimeInvalidPath1;
			}
			if (reason == VitaGpuVu::UniversalGpuVuRejection::None &&
				submitted->generated_structured &&
				structured_bundle_stale_or_quarantined)
			{
				reason = VitaGpuVu::UniversalGpuVuRejection::
					StructuredBundleQuarantined;
			}
			const u32* const structured_scratch_guard =
				reinterpret_cast<const u32*>(base +
					GPU_VU_UNIVERSAL_PRODUCT_JOURNAL_VALUE_OFFSET +
					GPU_VU_STRUCTURED_SCRATCH_BYTES);
			const bool structured_scratch_guard_valid =
				!submitted->generated_structured ||
				std::all_of(structured_scratch_guard,
					structured_scratch_guard +
						GPU_VU_STRUCTURED_SCRATCH_GUARD_WORDS,
					[](u32 value) {
						return value == GPU_VU_STRUCTURED_SCRATCH_GUARD_VALUE;
					});
			if (reason == VitaGpuVu::UniversalGpuVuRejection::None &&
				!structured_scratch_guard_valid)
			{
				const u32 first_bad_guard = static_cast<u32>(
					std::find_if(structured_scratch_guard,
						structured_scratch_guard +
							GPU_VU_STRUCTURED_SCRATCH_GUARD_WORDS,
						[](u32 value) {
							return value != GPU_VU_STRUCTURED_SCRATCH_GUARD_VALUE;
						}) - structured_scratch_guard);
				const bool newly_quarantined =
					VitaGpuVu::RejectStructuredGeneratedBundle(
						epoch->ProgramIdentity());
				Console.Error(
					"GPU-VU seq=%llu provider=generated-structured accepted=0 "
					"pre_effect=1 reason=structured-scratch-range "
					"maximum_word=%u guard_word=%u actual=%08x expected=%08x "
					"completed_jobs=%u/%u quarantined=%u cpu_fallback=1.",
					static_cast<unsigned long long>(epoch->Sequence()),
					GPU_VU_STRUCTURED_SCRATCH_BYTES / sizeof(u32) - 1u,
					first_bad_guard,
					structured_scratch_guard[first_bad_guard],
					GPU_VU_STRUCTURED_SCRATCH_GUARD_VALUE,
					submitted->structured_submission_cursor,
					submitted->structured_submission_total,
					static_cast<u32>(newly_quarantined));
				reason = VitaGpuVu::UniversalGpuVuRejection::SubmissionFailed;
			}
			const u32 expected_structured_submission_total =
				submitted->generated_structured ?
					epoch->UnpackSubmissionCount() +
						VitaGpuVu::StructuredGeneratedEntryJobCount +
						structured.GeneratedStageCount() + 1u : 0u;
			const bool structured_schedule_valid =
				!submitted->generated_structured ||
				(generated_structured_ready &&
				 submitted->structured_submission_total ==
					expected_structured_submission_total &&
				 submitted->structured_submission_cursor != 0u &&
				 submitted->structured_submission_cursor <=
					submitted->structured_submission_total &&
				 VitaGpuVu::UniversalGpuVuStructuredSubmissionCursorIsValid(
					submitted->structured_submission_total,
					submitted->structured_submission_cursor,
					structured_preflight_barrier_submission));
			submitted->submitted = false;
			if (reason == VitaGpuVu::UniversalGpuVuRejection::None &&
				!structured_schedule_valid)
			{
				const bool newly_quarantined =
					VitaGpuVu::RejectStructuredGeneratedBundle(
						epoch->ProgramIdentity());
				Console.Error(
					"GPU-VU seq=%llu provider=generated-structured accepted=0 "
					"pre_effect=1 reason=structured-schedule-metadata "
					"ready=%u cursor=%u total=%u expected=%u quarantined=%u "
					"cpu_fallback=1.",
					static_cast<unsigned long long>(epoch->Sequence()),
					static_cast<u32>(generated_structured_ready),
					submitted->structured_submission_cursor,
					submitted->structured_submission_total,
					expected_structured_submission_total,
					static_cast<u32>(newly_quarantined));
				reason = VitaGpuVu::UniversalGpuVuRejection::SubmissionFailed;
			}
			if (reason == VitaGpuVu::UniversalGpuVuRejection::None &&
				submitted->generated_structured &&
				submitted->structured_submission_cursor ==
					structured_preflight_barrier_submission)
			{
				const u32 preflight_gate = structured_outer_state[12];
				if (preflight_gate == 2u)
				{
					reason = VitaGpuVu::UniversalGpuVuRejection::
						RuntimeStructuredPreflight;
				}
				else if (preflight_gate > 1u)
				{
					reason = VitaGpuVu::UniversalGpuVuRejection::SubmissionFailed;
				}
				if (reason != VitaGpuVu::UniversalGpuVuRejection::None)
				{
					Console.Warning(
						"GPU-VU seq=%llu provider=generated-structured "
						"preflight_barrier=1 accepted=0 pre_effect=1 gate=%u "
						"failure=%s(%u) completed_jobs=%u/%u "
						"generated_partition_jobs_submitted=0 cpu_fallback=1.",
						static_cast<unsigned long long>(epoch->Sequence()),
						preflight_gate,
						VitaGpuVu::StructuredGeneratedRuntimeFailureName(
							structured_outer_state[13]),
						structured_outer_state[13],
						submitted->structured_submission_cursor,
						submitted->structured_submission_total);
				}
			}
			if (reason == VitaGpuVu::UniversalGpuVuRejection::None &&
				submitted->generated_structured &&
				submitted->structured_submission_cursor <
					submitted->structured_submission_total)
			{
				const u32 submission_base =
					submitted->structured_submission_cursor;
				const u32 batch_count = VitaGpuVu::
					UniversalGpuVuStructuredSubmissionGroupCount(
						submitted->structured_submission_total, submission_base,
						structured_preflight_barrier_submission);
				const u8* const payload = resolve_payload();
				if (!payload)
				{
					reason = VitaGpuVu::UniversalGpuVuRejection::InputUnavailable;
				}
				else if (batch_count == 0u)
				{
					reason = VitaGpuVu::UniversalGpuVuRejection::SubmissionFailed;
				}
				else
				{
					submitted->source_bank = completed_bank;
					if (submit_slot(submitted, payload, true, batch_count,
							submission_base,
							submitted->structured_submission_total,
							NO_GENERATED_HOT_MODULE))
					{
						const u64 continuation_count =
							gpu_vu_universal_product_continuation_count;
						if (submitted->continuation_groups <= 32u ||
							(continuation_count & (continuation_count - 1u)) == 0u)
						{
							Console.WriteLn(
								"GPU-VU seq=%llu provider=generated-structured "
								"continuation=1 completed_jobs=%u/%u "
								"next_jobs=[%u,%u) group=%u "
								"group_core_submissions=%u total_core_submissions=%u "
								"total_firmware_jobs=%u output=raw-path1 "
								"private=1 canonical_commit=0 "
								"path1_commit_jobs=%u cpu_vu_calls=0",
								static_cast<unsigned long long>(epoch->Sequence()),
								submission_base,
								submitted->structured_submission_total,
								submission_base, submission_base + batch_count,
								submitted->submission_groups, batch_count,
								submitted->core_submissions,
								submitted->job_submissions,
								submitted->path1_commit_submissions);
						}
						return;
					}
					reason = VitaGpuVu::UniversalGpuVuRejection::SubmissionFailed;
				}
			}
			const bool continuation_requested = state[2] == 13u;
			if (reason == VitaGpuVu::UniversalGpuVuRejection::None)
			switch (state[2])
		{
			case 1: break;
			case 13: break;
			case 7:
				reason = VitaGpuVu::UniversalGpuVuRejection::RuntimePairBudget;
				break;
			case 9:
				switch (path1->error)
				{
					case VitaGpuVu::UniversalRawPath1ExportError::PacketCapacity:
					case VitaGpuVu::UniversalRawPath1ExportError::DescriptorCapacity:
						reason = VitaGpuVu::UniversalGpuVuRejection::RuntimeOutputCapacity;
						break;
					case VitaGpuVu::UniversalRawPath1ExportError::InvalidAddress:
					case VitaGpuVu::UniversalRawPath1ExportError::PacketExceedsVuMemory:
						reason = VitaGpuVu::UniversalGpuVuRejection::RuntimeInvalidPath1;
						break;
					default:
						reason = VitaGpuVu::UniversalGpuVuRejection::SubmissionFailed;
						break;
				}
				break;
			case 10:
				reason = VitaGpuVu::UniversalGpuVuRejection::RuntimeTerminalXgkick;
				break;
			case 14:
				reason = VitaGpuVu::UniversalGpuVuRejection::RuntimeStructuredPreflight;
				break;
			case 2:
				reason = VitaGpuVu::UniversalGpuVuRejection::RuntimeInvalidPair;
				break;
			default:
				reason = VitaGpuVu::UniversalGpuVuRejection::SubmissionFailed;
				break;
		}
		if (!path1_valid && reason == VitaGpuVu::UniversalGpuVuRejection::None)
		{
			reason = path1->error ==
					VitaGpuVu::UniversalRawPath1ExportError::PacketCapacity ||
				path1->error ==
					VitaGpuVu::UniversalRawPath1ExportError::DescriptorCapacity ?
				VitaGpuVu::UniversalGpuVuRejection::RuntimeOutputCapacity :
				VitaGpuVu::UniversalGpuVuRejection::RuntimeInvalidPath1;
		}
		if (reason == VitaGpuVu::UniversalGpuVuRejection::None &&
			(state[27] == 0u || completion_pair_upper_bound == 0u ||
			 state[27] > completion_pair_upper_bound))
		{
			reason = VitaGpuVu::UniversalGpuVuRejection::SubmissionFailed;
		}
			if (continuation_requested &&
			(reason == VitaGpuVu::UniversalGpuVuRejection::None))
		{
			const bool continuation_valid = VitaGpuVu::
				UniversalGpuVuSerialContinuationMetadataIsValid(
					state[59], state[60], state[61], state[27], state[62],
					state[63], epoch->CommandCount(),
					completion_pair_upper_bound,
					submitted->generated_hot ?
						submitted->submission_pair_limit : 0u);
			if (!continuation_valid)
			{
				reason = VitaGpuVu::UniversalGpuVuRejection::SubmissionFailed;
			}
				else
				{
					const u8* const payload = resolve_payload();
					u32 continuation_hot_module = NO_GENERATED_HOT_MODULE;
					if (product_hot_ready &&
						!product_hot_bundle.ModuleForPc(
							state[0], &continuation_hot_module))
					{
						reason = VitaGpuVu::UniversalGpuVuRejection::SubmissionFailed;
					}
					const u32 batch_count =
						reason == VitaGpuVu::UniversalGpuVuRejection::None ?
							submission_batch_count(state[27]) : 0u;
				if (!payload)
				{
					reason = VitaGpuVu::UniversalGpuVuRejection::InputUnavailable;
				}
				else if (batch_count == 0u)
				{
					reason = VitaGpuVu::UniversalGpuVuRejection::SubmissionFailed;
				}
				else
				{
					// The completed generation remains byte-identical until all
					// continuation metadata and retained input have validated. Phase 1
					// makes clearing state[2] unnecessary: every serial core begins
					// with a local zero stop reason and publishes only its successor.
					submitted->source_bank = completed_bank;
				}
					if (reason == VitaGpuVu::UniversalGpuVuRejection::None &&
						submit_slot(submitted, payload, true, batch_count, 0u, 0u,
							continuation_hot_module))
				{
					const u64 count =
						gpu_vu_universal_product_continuation_count;
					if (count <= 8 || (count & (count - 1)) == 0)
					{
						Console.WriteLn(
							"GPU-VU seq=%llu provider=%s continuation=1 "
								"pairs=%u remaining=%u pc=%u active_module=%u "
								"active_stage=%u group=%u "
							"group_core_submissions=%u total_core_submissions=%u "
							"total_firmware_jobs=%u output=raw-path1 "
							"path1_commit_jobs=%u cpu_vu_calls=0 "
							"continuation_group_count=%llu",
							static_cast<unsigned long long>(epoch->Sequence()),
							submitted->generated_structured ? "generated-structured" :
							submitted->generated_hot ? "generated-hot" :
							submitted->generated_serial ? "generated-serial" :
								submitted->compact_serial ? "universal-compact" :
								"universal-fixed",
								state[27], state[61], state[0] >> 3,
								continuation_hot_module,
								continuation_hot_module < product_hot_bundle.module_count ?
									product_hot_bundle.modules[continuation_hot_module].stage :
									NO_GENERATED_HOT_MODULE,
							submitted->submission_groups, batch_count,
							submitted->core_submissions, submitted->job_submissions,
							submitted->path1_commit_submissions,
							static_cast<unsigned long long>(count));
					}
					return;
				}
				if (reason == VitaGpuVu::UniversalGpuVuRejection::None)
					reason = VitaGpuVu::UniversalGpuVuRejection::SubmissionFailed;
			}
		}
		if (reason == VitaGpuVu::UniversalGpuVuRejection::None &&
			submitted->generated_structured)
		{
			// A structured transaction keeps its fixed-entry generation intact
			// until all generated stages have published the opposite bank. Keep a
			// bounded physical diagnostic of that exact seam while the provider is
			// quarantined: VF1 is sourced from predecessor VF5 in the current
			// BSpline root, while VF13 exposes its first VU-memory child input. No
			// value here participates in admission or semantic dispatch.
			const u64 bank_diagnostic =
				++gpu_vu_universal_product_structured_bank_diagnostic_count;
			if (bank_diagnostic <= 8u)
			{
				const u32 predecessor_bank = completed_bank ^ 1u;
				const u32* const predecessor_state =
					reinterpret_cast<const u32*>(
						base + state_offset(predecessor_bank));
				const u32* const predecessor_vf =
					reinterpret_cast<const u32*>(
						base + vf_offset(predecessor_bank));
				const u32* const completed_vf =
					reinterpret_cast<const u32*>(
						base + vf_offset(completed_bank));
				const u32 predecessor_vi10 = predecessor_state[18] & 0xffffu;
				const u32 predecessor_qword = predecessor_vi10 & 1023u;
				const u32* const predecessor_memory =
					reinterpret_cast<const u32*>(
						base + memory_offset(predecessor_bank)) +
					predecessor_qword * 4u;
				const u32* const completed_memory =
					reinterpret_cast<const u32*>(
						base + memory_offset(completed_bank)) +
					predecessor_qword * 4u;
				const u32* const snapshots = reinterpret_cast<const u32*>(
					base + GPU_VU_UNIVERSAL_PRODUCT_SNAPSHOT_OFFSET);
				const u32* const vi_snapshots = reinterpret_cast<const u32*>(
					base + GPU_VU_UNIVERSAL_PRODUCT_VI_SNAPSHOT_OFFSET);
				Console.Warning(
					"GPU-VU seq=%llu provider=generated-structured "
					"private-bank-diagnostic=%llu predecessor=%u completed=%u "
					"pre_state=%u,%u,%u,%u,%u out_state=%u,%u,%u,%u,%u "
					"pre_vf5=%08x:%08x:%08x:%08x "
					"out_vf5=%08x:%08x:%08x:%08x "
					"child_vf1=%08x:%08x:%08x:%08x "
					"child_vf13=%08x:%08x:%08x:%08x child_vi10=%u "
					"pre_vi10=%u pre_qword=%u "
					"pre_memory=%08x:%08x:%08x:%08x "
					"out_memory=%08x:%08x:%08x:%08x canonical=private",
					static_cast<unsigned long long>(epoch->Sequence()),
					static_cast<unsigned long long>(bank_diagnostic),
					predecessor_bank, completed_bank,
					predecessor_state[0], predecessor_state[2],
					predecessor_state[37], predecessor_state[38],
					predecessor_state[18], state[0], state[2], state[37],
					state[38], state[18], predecessor_vf[5u * 4u + 0u],
					predecessor_vf[5u * 4u + 1u],
					predecessor_vf[5u * 4u + 2u],
					predecessor_vf[5u * 4u + 3u],
					completed_vf[5u * 4u + 0u],
					completed_vf[5u * 4u + 1u],
					completed_vf[5u * 4u + 2u],
					completed_vf[5u * 4u + 3u],
					snapshots[1u * 4u + 0u], snapshots[1u * 4u + 1u],
					snapshots[1u * 4u + 2u], snapshots[1u * 4u + 3u],
					snapshots[13u * 4u + 0u], snapshots[13u * 4u + 1u],
					snapshots[13u * 4u + 2u], snapshots[13u * 4u + 3u],
					vi_snapshots[10], predecessor_vi10, predecessor_qword,
					predecessor_memory[0], predecessor_memory[1],
					predecessor_memory[2], predecessor_memory[3],
					completed_memory[0], completed_memory[1],
					completed_memory[2], completed_memory[3]);
			}
			// The structured generated tier has not yet passed a physical
			// PCSX2-state/PATH1 differential. Its transaction is private at this
			// point, so retain the completed bytes for comparison with the exact
			// MTVU replay and reject before either state or PATH1 is published.
			// A source/configuration may not regain admission merely because one
			// structural terminal check happened to pass.
			const bool diagnostic_captured =
				epoch->CapturePrivateStructuredResult(
					reinterpret_cast<const u32*>(
						base + vf_offset(completed_bank)),
					state, base + memory_offset(completed_bank), path1,
					reinterpret_cast<const u32*>(base +
						GPU_VU_UNIVERSAL_PRODUCT_SNAPSHOT_OFFSET),
					structured_outer_state,
					reinterpret_cast<const u32*>(base +
						GPU_VU_UNIVERSAL_PRODUCT_VI_SNAPSHOT_OFFSET));
			const bool newly_quarantined =
				VitaGpuVu::RejectStructuredGeneratedBundle(
					epoch->ProgramIdentity());
			reason = diagnostic_captured ?
				VitaGpuVu::UniversalGpuVuRejection::
					RuntimeStructuredAttestation :
				VitaGpuVu::UniversalGpuVuRejection::SubmissionFailed;
			Console.Warning(
				"GPU-VU seq=%llu provider=generated-structured accepted=0 "
				"pre_effect=1 reason=%s private_result=%u quarantined=%u "
				"pairs=%u path1_packets=%u path1_qwords=%u "
				"entry_pc=%u child_entry_pc=%u tail_pc=%u parent_prefix_pairs=%u "
				"child_pairs=%u suffix_pairs=%u "
				"state_pc=%u stop=%u target_pc=%u target_enable=%u "
				"phase=%u cursor=%u remaining=%u execute_ready=%u "
				"slice=%u outer=%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u "
				"cpu_fallback=1",
				static_cast<unsigned long long>(epoch->Sequence()),
				VitaGpuVu::UniversalGpuVuRejectionName(reason),
				static_cast<u32>(diagnostic_captured),
				static_cast<u32>(newly_quarantined), state[27],
				path1->packet_count, path1->data_qword_count,
				structured.entry_pc, structured.child_entry_pc,
				structured.tail_pc,
				structured.parent_prefix_pair_count,
				structured.child_pair_count, structured.suffix_pair_count,
				state[0], state[2], state[37], state[38], state[59], state[60],
				state[61], state[62], state[63],
				structured_outer_state[0], structured_outer_state[1],
				structured_outer_state[2], structured_outer_state[3],
				structured_outer_state[4], structured_outer_state[5],
				structured_outer_state[6], structured_outer_state[7],
				structured_outer_state[8], structured_outer_state[9],
				structured_outer_state[10], structured_outer_state[11],
				structured_outer_state[12], structured_outer_state[13],
				structured_outer_state[14], structured_outer_state[15]);
		}
			if (reason != VitaGpuVu::UniversalGpuVuRejection::None)
			{
				if (reason ==
						VitaGpuVu::UniversalGpuVuRejection::SubmissionFailed &&
					submitted->generated_structured &&
					gpu_vu_universal_product_runtime_reject_count < 8u)
				{
					Console.Warning(
						"GPU-VU seq=%llu structured-submission-state "
						"pc=%u stop=%u pairs=%u commands=%u unpacks=%u "
						"target_pc=%u target_enable=%u phase=%u cursor=%u "
						"remaining=%u execute_ready=%u slice=%u marker=%08x "
						"path1_format=%u path1_error=%u path1_packets=%u "
						"path1_qwords=%u path1_valid=%u pair_bound=%u "
						"outer=%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u",
						static_cast<unsigned long long>(epoch->Sequence()),
						state[0], state[2], state[27], state[31], state[32],
						state[37], state[38], state[59], state[60], state[61],
						state[62], state[63], state[35], path1->format_version,
						static_cast<u32>(path1->error), path1->packet_count,
						path1->data_qword_count, static_cast<u32>(path1_valid),
						completion_pair_upper_bound,
						structured_outer_state[0], structured_outer_state[1],
						structured_outer_state[2], structured_outer_state[3],
						structured_outer_state[4], structured_outer_state[5],
						structured_outer_state[6], structured_outer_state[7],
						structured_outer_state[8], structured_outer_state[9],
						structured_outer_state[10], structured_outer_state[11],
						structured_outer_state[12], structured_outer_state[13],
						structured_outer_state[14], structured_outer_state[15]);
				}
				if (reason ==
						VitaGpuVu::UniversalGpuVuRejection::RuntimeStructuredPreflight &&
				submitted->generated_structured)
			{
				const u32* const fixed_state_program =
					reinterpret_cast<const u32*>(base +
						GPU_VU_UNIVERSAL_PRODUCT_JOURNAL_ADDRESS_OFFSET);
				const u32* const auxiliary = reinterpret_cast<const u32*>(
					base + GPU_VU_UNIVERSAL_PRODUCT_JOURNAL_VALUE_OFFSET +
					VitaGpuVu::StructuredGeneratedPreflightWorkspaceWords *
						sizeof(u32));
				const u32 vi_offset =
					fixed_state_program[VitaGpuVu::FixedStateHeaderViOffset];
				const u32 child_counter_reg = fixed_state_program[
					VitaGpuVu::FixedStateHeaderChildCounterReg];
				const u32 child_limit_reg = fixed_state_program[
					VitaGpuVu::FixedStateHeaderChildLimitReg];
				const u32 counter_metadata =
					vi_offset + child_counter_reg *
						VitaGpuVu::FixedStructuredStateViEntryWords;
				const u32 limit_metadata =
					vi_offset + child_limit_reg *
						VitaGpuVu::FixedStructuredStateViEntryWords;
				const u32 counter_initial = auxiliary[
					VitaGpuVu::StructuredGeneratedControlInitialParentViOffset +
					child_counter_reg];
				const u32 limit_initial = auxiliary[
					VitaGpuVu::StructuredGeneratedControlInitialParentViOffset +
					child_limit_reg];
				const u32 counter_child = auxiliary[
					VitaGpuVu::StructuredGeneratedControlChildViOffset +
					child_counter_reg];
				const u32 limit_child = auxiliary[
					VitaGpuVu::StructuredGeneratedControlChildViOffset +
					child_limit_reg];
				Console.Warning(
					"GPU-VU seq=%llu structured-control-oracle "
					"child_control=%u,%u,%d initial=%u,%u child=%u,%u "
					"private=%u,%u counter_affine=%u,%u,%u,%u "
					"limit_affine=%u,%u,%u,%u",
					static_cast<unsigned long long>(epoch->Sequence()),
					child_counter_reg, child_limit_reg,
					static_cast<s32>(fixed_state_program[
						VitaGpuVu::FixedStateHeaderChildCounterStep]),
					counter_initial, limit_initial, counter_child, limit_child,
					state[8u + child_counter_reg] & 0xffffu,
					state[8u + child_limit_reg] & 0xffffu,
					fixed_state_program[counter_metadata + 0u],
					fixed_state_program[counter_metadata + 1u],
					fixed_state_program[counter_metadata + 2u],
					fixed_state_program[counter_metadata + 3u],
					fixed_state_program[limit_metadata + 0u],
					fixed_state_program[limit_metadata + 1u],
					fixed_state_program[limit_metadata + 2u],
					fixed_state_program[limit_metadata + 3u]);
				const bool newly_quarantined =
					VitaGpuVu::RejectStructuredGeneratedBundle(
						epoch->ProgramIdentity());
				Console.Warning(
					"GPU-VU seq=%llu structured-preflight-state "
					"outer_executed=%u outer_counter=%u outer_complete=%u "
					"outer_failure=%s(%u) structured_pairs=%u "
					"failure_duplicate=%u base_pairs=%u base_cycle=%u "
					"final_trip=%u final_failure=%s(%u) "
						"preflight_gate=%u preflight_failure=%s(%u) "
						"preflight_trip=%u "
						"remaining_pair_budget=%u handoff_continuation=%u "
						"transaction_gate=%u "
						"quarantined=%u",
					static_cast<unsigned long long>(epoch->Sequence()),
					structured_outer_state[0], structured_outer_state[1],
					structured_outer_state[2],
					VitaGpuVu::StructuredGeneratedRuntimeFailureName(
						structured_outer_state[3]), structured_outer_state[3],
					structured_outer_state[4], structured_outer_state[5],
					structured_outer_state[8], structured_outer_state[9],
					structured_outer_state[10],
					VitaGpuVu::StructuredGeneratedRuntimeFailureName(
						structured_outer_state[11]), structured_outer_state[11],
					structured_outer_state[12],
					VitaGpuVu::StructuredGeneratedRuntimeFailureName(
						structured_outer_state[13]), structured_outer_state[13],
					structured_outer_state[14],
						structured_outer_state[6], structured_outer_state[7],
						structured_outer_state[15],
						static_cast<u32>(newly_quarantined));
			}
			if (reason == VitaGpuVu::UniversalGpuVuRejection::RuntimeInvalidPath1)
			{
				// Generated roots stage a not-yet-committed XGKICK descriptor in
				// packets[packet_count].  The fixed core instead publishes its
				// rejected address/tag through reserved_header.  Preserve the
				// actual generated address so CPU replay compares the same VU-memory
				// qword rather than the zero-initialized fixed-core diagnostic.
				u32 rejected_address = path1->reserved_header[1] & 0x3ff0u;
				std::array<u32, 4> rejected_tag = {
					path1->reserved_header[2], path1->reserved_header[3],
					path1->reserved_header[4], path1->reserved_header[5]};
				const bool generated_path1 = submitted->generated_hot ||
					submitted->generated_serial;
				if (generated_path1 &&
					path1->packet_count <
						VitaGpuVu::UniversalRawPath1ExportMaximumPackets)
				{
					const auto& staged = path1->packets[path1->packet_count];
					if ((staged.source_byte_address & 0x0fu) == 0u &&
						staged.source_byte_address < GPU_VU_UNIVERSAL_MEMORY_BYTES)
					{
						rejected_address = staged.source_byte_address;
						const u32* const completed_memory =
							reinterpret_cast<const u32*>(
								base + memory_offset(completed_bank));
						const u32 source_word =
							(rejected_address >> 4u) * 4u;
						rejected_tag = {completed_memory[source_word + 0u],
							completed_memory[source_word + 1u],
							completed_memory[source_word + 2u],
							completed_memory[source_word + 3u]};
					}
				}
				if (gpu_vu_universal_product_runtime_reject_count < 8u)
				{
					const u32 source_qword = state[40] & 0x3ffu;
					const u32* const memory0 = reinterpret_cast<const u32*>(
						base + memory_offset(0u));
					const u32* const memory1 = reinterpret_cast<const u32*>(
						base + memory_offset(1u));
					Console.Warning(
						"GPU-VU seq=%llu generated-path1-memory completed_bank=%u "
						"vif_top=%u vi4=%u vi5=%u vi7=%u "
						"bank0_top=%08x:%08x:%08x:%08x "
						"bank1_top=%08x:%08x:%08x:%08x "
						"bank0_qw0=%08x:%08x:%08x:%08x "
						"bank1_qw0=%08x:%08x:%08x:%08x "
						"staged_address=%04x staged_tag=%08x:%08x:%08x:%08x",
						static_cast<unsigned long long>(epoch->Sequence()),
						completed_bank, source_qword, state[12] & 0xffffu,
						state[13] & 0xffffu, state[15] & 0xffffu,
						memory0[source_qword * 4u + 0u],
						memory0[source_qword * 4u + 1u],
						memory0[source_qword * 4u + 2u],
						memory0[source_qword * 4u + 3u],
						memory1[source_qword * 4u + 0u],
						memory1[source_qword * 4u + 1u],
						memory1[source_qword * 4u + 2u],
						memory1[source_qword * 4u + 3u], memory0[0], memory0[1],
						memory0[2], memory0[3], memory1[0], memory1[1],
						memory1[2], memory1[3], rejected_address,
						rejected_tag[0], rejected_tag[1], rejected_tag[2],
						rejected_tag[3]);
				}
				epoch->SetRejectedPath1Diagnostic(rejected_address, rejected_tag);
			}
			submitted->committed = false;
			reject(reason, state[27]);
			const u64 count = ++gpu_vu_universal_product_runtime_reject_count;
			if (count <= 8 || (count & (count - 1)) == 0)
			{
				Console.Warning(
					"GPU-VU seq=%llu provider=%s accepted=0 pre_effect=1 "
					"reason=%s executes=%u pairs=%u pair_bound=%u "
					"fixed_dynamic_bound=%u "
					"watchdog_budget=%u output=raw-path1 path1_error=%u "
					"path1_packets=%u path1_qwords=%u xgkick_cancelled=%u "
					"xgkick_address=%04x xgkick_tag=%08x:%08x:%08x:%08x "
					"cpu_fallback=1 "
					"runtime_reject_count=%llu",
					static_cast<unsigned long long>(epoch->Sequence()),
					submitted->generated_structured ? "generated-structured" :
					submitted->generated_hot ? "generated-hot" :
					submitted->generated_serial ? "generated-serial" :
						submitted->compact_serial ? "universal-compact" :
						"universal-fixed",
					VitaGpuVu::UniversalGpuVuRejectionName(reason),
					epoch->ExecuteCount(), state[27],
					completion_pair_upper_bound,
					epoch->DynamicPairUpperBound(),
					submitted->submission_pair_limit,
					static_cast<u32>(path1->error), path1->packet_count,
					path1->data_qword_count, path1->reserved_header[0],
					path1->reserved_header[1], path1->reserved_header[2],
					path1->reserved_header[3], path1->reserved_header[4],
					path1->reserved_header[5],
					static_cast<unsigned long long>(count));
			}
			return;
		}

		for (UniversalGpuVuProductSlot& slot :
			gpu_vu_universal_product_slots)
			slot.committed = false;
			submitted->committed = true;
			submitted->source_bank = completed_bank;
		gpu_vu_universal_product_committed_slot = static_cast<s32>(
			submitted - gpu_vu_universal_product_slots.data());
		epoch->MarkAccepted(state[0], state[27], path1->packet_count,
			path1->data_qword_count, VU_Thread::InterruptFlagVUEBit,
			state[46], path1,
				{reinterpret_cast<const u32*>(
					base + vf_offset(completed_bank)),
				 state,
				 base + memory_offset(completed_bank),
				 epoch->Sequence()});
		if (!submitted->generated_serial && !submitted->generated_hot &&
			!submitted->generated_structured)
		{
			const u32 unpack_jobs = epoch->UnpackSubmissionCount();
			const u32 fixed_firmware_jobs =
				submitted->job_submissions > unpack_jobs ?
					submitted->job_submissions - unpack_jobs : 0u;
			UniversalGpuVuObservedPairBound* observed_target = nullptr;
			for (UniversalGpuVuObservedPairBound& observed :
				gpu_vu_universal_observed_pair_bounds)
			{
				if (observed.program_identity == epoch->ProgramIdentity() &&
					observed.execute_count == epoch->ExecuteCount() &&
					observed.unpack_submission_count == unpack_jobs)
				{
					observed_target = &observed;
					break;
				}
				if (observed.program_identity == 0 || !observed_target ||
					observed.last_use < observed_target->last_use)
				{
					observed_target = &observed;
					if (observed.program_identity == 0)
						break;
				}
			}
			if (observed_target && fixed_firmware_jobs != 0u)
			{
				if (observed_target->program_identity != epoch->ProgramIdentity() ||
					observed_target->execute_count != epoch->ExecuteCount() ||
					observed_target->unpack_submission_count != unpack_jobs)
				{
					*observed_target = {};
					observed_target->program_identity = epoch->ProgramIdentity();
					observed_target->execute_count = epoch->ExecuteCount();
					observed_target->unpack_submission_count = unpack_jobs;
				}
				observed_target->observed_pairs =
					std::max(observed_target->observed_pairs, state[27]);
				if (observed_target->minimum_fixed_firmware_jobs == 0u)
				{
					observed_target->minimum_fixed_firmware_jobs =
						fixed_firmware_jobs;
				}
				else
				{
					observed_target->minimum_fixed_firmware_jobs = std::min(
						observed_target->minimum_fixed_firmware_jobs,
						fixed_firmware_jobs);
				}
				observed_target->last_use =
					++gpu_vu_universal_observed_pair_bound_clock;
			}
			VitaGpuVu::RecordStructuredGeneratedFixedCost(
				epoch->ProgramIdentity(), epoch->ExecuteCount(), unpack_jobs,
				state[27], fixed_firmware_jobs);
		}
		const u64 gpu_residency_wall_us = submitted->submission_wall != 0 ?
			static_cast<u64>(Common::Timer::ConvertValueToSeconds(
				Common::Timer::GetCurrentValue() - submitted->submission_wall) *
				1000000.0) : 0;
		const u64 gpu_job_average_us = submitted->job_submissions != 0 ?
			gpu_residency_wall_us / submitted->job_submissions : 0;
		const u64 gpu_pairs_per_second = gpu_residency_wall_us != 0 ?
			(static_cast<u64>(state[27]) * 1000000ull) /
				gpu_residency_wall_us : 0;
		VitaGpuVu::RecordUniversalGpuVuProviderCompletion(
			submitted->generated_serial || submitted->generated_hot ||
				submitted->generated_structured,
			submitted->job_submissions,
			gpu_residency_wall_us);
		submitted->submission_wall = 0;
		const u64 count = ++gpu_vu_universal_product_accept_count;
		const bool first_generated =
			(submitted->generated_serial || submitted->generated_hot ||
			 submitted->generated_structured) &&
			!gpu_vu_universal_generated_accept_reported;
		if (first_generated)
			gpu_vu_universal_generated_accept_reported = true;
		if (first_generated || count <= 8 || (count & (count - 1)) == 0)
		{
			const VitaGpuVu::GeneratedHotBundle& completed_hot_bundle =
				epoch->GeneratedHotBundleDescriptor();
			const u32 generated_modules = submitted->generated_hot ?
				completed_hot_bundle.module_count :
				submitted->generated_serial ? 1u : 0u;
			const u32 generated_stages = submitted->generated_hot ?
				completed_hot_bundle.stage_count :
				submitted->generated_serial ? 1u : 0u;
			const u32 generated_draws = submitted->generated_hot ?
				completed_hot_bundle.DrawCount() :
				submitted->generated_serial ? submitted->core_submissions : 0u;
			Console.WriteLn(
				"GPU-VU seq=%llu provider=%s accepted=1 executes=%u pairs=%u "
				"pair_bound=%u fixed_dynamic_bound=%u watchdog_budget=%u "
				"generated_modules=%u generated_stages=%u generated_draws=%u "
				"output=raw-path1 path1_packets=%u path1_qwords=%u "
				"xgkick_cancelled=%u vif_unpack_jobs=%u "
				"vif_independent_jobs=%u vif_vectors=%u "
				"gpu_groups=%u continuation_groups=%u core_submissions=%u "
				"gpu_jobs=%u path1_commit_jobs=%u gpu_residency_us=%llu "
				"gpu_job_avg_us=%llu gpu_pairs_per_second=%llu "
				"completion_boundaries=1 "
				"cpu_vu_calls=0 accepted_count=%llu",
				static_cast<unsigned long long>(epoch->Sequence()),
				submitted->generated_structured ? "generated-structured" :
				submitted->generated_hot ? "generated-hot" :
				submitted->generated_serial ? "generated-serial" :
					submitted->compact_serial ? "universal-compact" :
					"universal-fixed",
				epoch->ExecuteCount(), state[27],
				completion_pair_upper_bound,
				epoch->DynamicPairUpperBound(),
				submitted->submission_pair_limit,
				generated_modules, generated_stages, generated_draws,
				path1->packet_count, path1->data_qword_count,
				path1->reserved_header[0],
				epoch->UnpackSubmissionCount(),
				epoch->IndependentUnpackSubmissionCount(), state[32],
				submitted->submission_groups, submitted->continuation_groups,
				submitted->core_submissions,
				submitted->job_submissions,
				submitted->path1_commit_submissions,
				static_cast<unsigned long long>(gpu_residency_wall_us),
				static_cast<unsigned long long>(gpu_job_average_us),
				static_cast<unsigned long long>(gpu_pairs_per_second),
				static_cast<unsigned long long>(count));
		}
		return;
	}

	if (epoch->Stage() != VitaGpuVu::UniversalGpuVuEpochStage::Prepared)
		return;
	// A fixed-interpreter slice is a dependency-ordered firmware job, not a
	// cheap VU pair loop. Long Execute bounds are now retained as one private
	// transaction and advanced through small notification-driven groups. The
	// only serial scheduling shape that still has to fit at once is the initial
	// UNPACK-to-first-Execute transition; the epoch remains pristine here if it
	// does not fit.
	const GeneratedVuProgram* const ready_generated_program =
		FindGeneratedVuProgram(epoch->GeneratedProgramKey());
	const bool ready_generated_serial = ready_generated_program &&
		(ready_generated_program->metadata.execution_kind ==
			VitaGpuVu::GeneratedCgExecutionKind::UniversalStateMachine ||
		 ready_generated_program->metadata.execution_kind ==
			VitaGpuVu::GeneratedCgExecutionKind::UniversalDirectStateMachine ||
		 ready_generated_program->metadata.execution_kind ==
			VitaGpuVu::GeneratedCgExecutionKind::UniversalCompactContinuation) &&
		ready_generated_program->vertex_program &&
		ready_generated_program->general_fragment_program;
	const bool ready_compact_serial =
		epoch->CanUseCompactScheduledContinuation() &&
		gpu_vu_universal_compact_product_vertex_program &&
		gpu_vu_universal_compact_product_fragment_program;
	const u32 serial_submission_upper_bound = VitaGpuVu::
		UniversalGpuVuSerialSubmissionUpperBound(
			epoch->DynamicPairUpperBound(), epoch->UnpackSubmissionCount(),
			ready_compact_serial);
	const u32 initial_transition_submissions = VitaGpuVu::
		UniversalGpuVuInitialTransitionSubmissionCount(
			epoch->UnpackSubmissionCount());
	if (!generated_structured_ready && !product_hot_ready && !VitaGpuVu::
			UniversalGpuVuInitialTransitionFitsGroup(
				epoch->UnpackSubmissionCount()))
	{
		reject(VitaGpuVu::UniversalGpuVuRejection::SynchronousDispatchCost,
			epoch->PreflightPairCount());
		const u64 count =
			++gpu_vu_universal_product_dispatch_cost_reject_count;
		if (count <= 8 || (count & (count - 1)) == 0)
		{
			Console.WriteLn(
				"GPU-VU seq=%llu provider=universal accepted=0 pre_effect=1 "
				"reason=synchronous-dispatch-cost pairs=%u dynamic_bound=%u "
				"serial_jobs=%u initial_transition_jobs=%u group_core_limit=%u "
				"generated_ready=%u compact_ready=%u "
				"cpu_fallback=1 fallback_count=%llu",
				static_cast<unsigned long long>(epoch->Sequence()),
				epoch->PreflightPairCount(), epoch->DynamicPairUpperBound(),
				serial_submission_upper_bound,
				initial_transition_submissions,
				VitaGpuVu::UniversalGpuVuMaximumSerialCoreSubmissionsPerGroup,
				ready_generated_serial ? 1u : 0u,
				ready_compact_serial ? 1u : 0u,
				static_cast<unsigned long long>(count));
		}
		return;
	}
	UniversalGpuVuProductSlot* slot = nullptr;
	for (u32 attempt = 0; attempt < gpu_vu_universal_product_slots.size();
		attempt++)
	{
		const u32 index = (gpu_vu_universal_product_next_slot + attempt) %
			gpu_vu_universal_product_slots.size();
		if (!gpu_vu_universal_product_slots[index].submitted &&
			static_cast<s32>(index) != gpu_vu_universal_product_committed_slot)
		{
			slot = &gpu_vu_universal_product_slots[index];
			gpu_vu_universal_product_next_slot =
				(index + 1u) % gpu_vu_universal_product_slots.size();
			break;
		}
	}
		if (!slot)
	{
		reject(VitaGpuVu::UniversalGpuVuRejection::DeviceUnavailable,
			epoch->PreflightPairCount());
		return;
		}
		slot->source_bank = 0;
		slot->structured_submission_cursor = 0u;
		slot->structured_submission_total = 0u;
		u8* const base = static_cast<u8*>(slot->buffer.base);
	if (gpu_vu_universal_product_committed_slot >= 0 &&
		epoch->PredecessorSequence() == 0)
	{
		// A zero predecessor after a prior GPU chain is the worker's attestation
		// that it materialized the completion-published mapped generation before
		// a CPU provider/observer changed canonical VU1 state. Relinquish the old
		// private generation and import that canonical state below.
		gpu_vu_universal_product_slots[
			gpu_vu_universal_product_committed_slot].committed = false;
		gpu_vu_universal_product_committed_slot = -1;
	}
	if (gpu_vu_universal_product_committed_slot >= 0)
	{
			const UniversalGpuVuProductSlot& committed =
			gpu_vu_universal_product_slots[
				gpu_vu_universal_product_committed_slot];
			const u32* const committed_state = reinterpret_cast<const u32*>(
				static_cast<const u8*>(committed.buffer.base) +
				state_offset(committed.source_bank));
		if (epoch->PredecessorSequence() != committed.sequence ||
			committed_state[47] != epoch->ConfigurationBits())
		{
			reject(
				epoch->PredecessorSequence() != committed.sequence ?
					VitaGpuVu::UniversalGpuVuRejection::RuntimePredecessorFailed :
					VitaGpuVu::UniversalGpuVuRejection::UnsupportedConfiguration,
				epoch->PreflightPairCount());
			return;
		}
			const u8* const source = static_cast<const u8*>(committed.buffer.base);
			std::memcpy(base + GPU_VU_UNIVERSAL_PRODUCT_VF_OFFSET,
				source + vf_offset(committed.source_bank),
				GPU_VU_UNIVERSAL_VF_BYTES);
			std::memcpy(base + GPU_VU_UNIVERSAL_PRODUCT_STATE_OFFSET,
				source + state_offset(committed.source_bank),
				GPU_VU_UNIVERSAL_STATE_BYTES);
			std::memcpy(base + GPU_VU_UNIVERSAL_PRODUCT_MEMORY_OFFSET,
				source + memory_offset(committed.source_bank),
			GPU_VU_UNIVERSAL_MEMORY_BYTES);
	}
	else
	{
		if (epoch->PredecessorSequence() != 0)
		{
			reject(VitaGpuVu::UniversalGpuVuRejection::RuntimePredecessorFailed,
				epoch->PreflightPairCount());
			return;
		}
		if (VU1.branch != 0 || VU1.ebit != 0 || VU1.VIBackupCycles != 0 ||
			VU1.fmaccount != 0 || VU1.ialucount != 0 || VU1.fdiv.enable != 0 ||
			VU1.efu.enable != 0 || VU1.xgkickenable != 0)
		{
			reject(VitaGpuVu::UniversalGpuVuRejection::UnsupportedConfiguration,
				epoch->PreflightPairCount());
			return;
		}
		std::memcpy(base + GPU_VU_UNIVERSAL_PRODUCT_VF_OFFSET,
			&VU1.VF[0].UL[0], 32u * 4u * sizeof(u32));
		std::memcpy(base + GPU_VU_UNIVERSAL_PRODUCT_VF_OFFSET +
			32u * 4u * sizeof(u32), &VU1.ACC.UL[0], 4u * sizeof(u32));
		std::memcpy(base + GPU_VU_UNIVERSAL_PRODUCT_MEMORY_OFFSET,
			VU1.Mem, GPU_VU_UNIVERSAL_MEMORY_BYTES);
		u32* const initial_state = reinterpret_cast<u32*>(
			base + GPU_VU_UNIVERSAL_PRODUCT_STATE_OFFSET);
		std::memset(initial_state, 0, GPU_VU_UNIVERSAL_STATE_BYTES);
		for (u32 reg = 1; reg < 16; reg++)
			initial_state[8 + reg] = VU1.VI[reg].UL;
		initial_state[0] = VU1.VI[REG_TPC].UL << 3;
		initial_state[24] = VU1.VI[REG_I].UL;
		initial_state[25] = VU1.VI[REG_Q].UL;
		initial_state[26] = VU1.VI[REG_P].UL;
		initial_state[42] = VU1.macflag;
		initial_state[43] = VU1.statusflag;
		initial_state[45] = VU1.clipflag;
		initial_state[46] = static_cast<u32>(VU1.cycle);
		for (u32 lane = 0; lane < 4; lane++)
		{
			initial_state[VitaGpuVu::UniversalGpuVuStateVifRowWord + lane] =
				epoch->InitialVifRow()[lane];
			initial_state[VitaGpuVu::UniversalGpuVuStateVifColumnWord + lane] =
				epoch->InitialVifColumn()[lane];
		}
	}

	std::memcpy(base + GPU_VU_UNIVERSAL_PRODUCT_MICRO_OFFSET,
		epoch->Program().pairs.data(), GPU_VU_UNIVERSAL_MICRO_BYTES);
	std::memset(base + GPU_VU_UNIVERSAL_PRODUCT_EPOCH_OFFSET, 0,
		GPU_VU_UNIVERSAL_EPOCH_BYTES);
	std::memcpy(base + GPU_VU_UNIVERSAL_PRODUCT_EPOCH_OFFSET,
		epoch->Commands(), epoch->CommandCount() *
			sizeof(VitaGpuVu::UniversalEpochMicroOp));
	auto* const path1 =
		reinterpret_cast<VitaGpuVu::UniversalRawPath1Export*>(
			base + GPU_VU_UNIVERSAL_PRODUCT_PATH1_OFFSET);
	// A slot owns a 64 KiB immutable output record. Reset only the live
	// header; descriptors and payload are authoritative solely below the counts
	// published by the terminal GPU job, so clearing the whole slot on ARM would
	// add pure CPU1 bandwidth to every epoch.
	path1->format_version = VitaGpuVu::UniversalRawPath1ExportFormatVersion;
	path1->packet_count = 0;
	path1->data_qword_count = 0;
	path1->error = VitaGpuVu::UniversalRawPath1ExportError::None;
	path1->reserved_header.fill(0);
	u32* const state = reinterpret_cast<u32*>(
		base + GPU_VU_UNIVERSAL_PRODUCT_STATE_OFFSET);
	state[1] = 0;
	state[2] = 0;
	state[27] = 0;
	state[29] = epoch->CommandCount();
	state[30] = epoch->PayloadSize();
	state[31] = 0;
	state[32] = 0;
	state[35] = 0x47565531u;
	state[36] = epoch->CanUseCompactScheduledContinuation() ?
		VitaGpuVu::UniversalGpuVuCompactPairsPerSubmission : 0u;
	state[VitaGpuVu::UniversalGpuVuStateStructuredTargetPcWord] =
		generated_structured_ready ? structured.entry_pc : 0u;
	state[VitaGpuVu::UniversalGpuVuStateStructuredTargetEnableWord] =
		generated_structured_ready ? 1u : 0u;
	state[44] = 0x000000c1u;
	// PCSX2's MTVU worker resets its provider-local cycle counter at every
	// MSCAL/MSCNT dispatch. Architectural register/memory state persists, but
	// carrying the prior dispatch's terminal timing into this invocation would
	// change cycle-steal accounting and delayed-resource timestamps.
	state[46] = 0;
	state[47] = epoch->ConfigurationBits();
	std::memset(state + 59, 0,
		(VitaGpuVu::UniversalGpuVuStateVifRowWord - 59) * sizeof(u32));
		std::memset(state + VitaGpuVu::UniversalGpuVuStateUnpackVectorWord, 0,
			(GPU_VU_UNIVERSAL_STATE_WORDS -
			 VitaGpuVu::UniversalGpuVuStateUnpackVectorWord) * sizeof(u32));
		state[63] = product_hot_ready ?
			generated_hot_pair_limit(product_hot_initial_module) :
			VitaGpuVu::UniversalGpuVuWatchdogSafePairsPerSubmission;
		// Sparse dual-publication kernels assume both private generations begin
		// byte-identical. This one bounded initialization replaces a full VU-memory
		// copy inside every dependency-ordered GPU job.
		std::memcpy(base + vf_offset(1u), base + vf_offset(0u),
			GPU_VU_UNIVERSAL_VF_BYTES);
		std::memcpy(base + state_offset(1u), base + state_offset(0u),
			GPU_VU_UNIVERSAL_STATE_BYTES);
		std::memcpy(base + memory_offset(1u), base + memory_offset(0u),
			GPU_VU_UNIVERSAL_MEMORY_BYTES);
		if (generated_structured_ready)
		{
			std::fill_n(reinterpret_cast<u32*>(base +
				GPU_VU_UNIVERSAL_PRODUCT_JOURNAL_VALUE_OFFSET +
				GPU_VU_STRUCTURED_SCRATCH_BYTES),
				GPU_VU_STRUCTURED_SCRATCH_GUARD_WORDS,
				GPU_VU_STRUCTURED_SCRATCH_GUARD_VALUE);
			u32 fixed_state_words = 0u;
			if (!VitaGpuVu::CopyStructuredFixedStateProgram(
					epoch->ProgramIdentity(),
					reinterpret_cast<u32*>(base +
						GPU_VU_UNIVERSAL_PRODUCT_JOURNAL_ADDRESS_OFFSET),
					GPU_VU_STRUCTURED_JOURNAL_ADDRESS_BYTES / sizeof(u32),
					&fixed_state_words) ||
				fixed_state_words != structured.fixed_state_descriptor_words)
			{
				VitaGpuVu::RejectStructuredGeneratedBundle(
					epoch->ProgramIdentity());
				Console.Error(
					"GPU-VU seq=%llu provider=generated-structured accepted=0 "
					"pre_effect=1 reason=fixed-state-descriptor-unavailable "
					"expected_words=%u copied_words=%u cpu_fallback=1.",
					static_cast<unsigned long long>(epoch->Sequence()),
					structured.fixed_state_descriptor_words, fixed_state_words);
				reject(VitaGpuVu::UniversalGpuVuRejection::SubmissionFailed,
					epoch->PreflightPairCount());
				return;
			}
			std::memset(base + GPU_VU_UNIVERSAL_PRODUCT_OUTER_STATE_OFFSET, 0,
				GPU_VU_STRUCTURED_OUTER_STATE_BYTES);
			std::memset(base + GPU_VU_UNIVERSAL_PRODUCT_OUTER_STATE_OUTPUT_OFFSET, 0,
				GPU_VU_STRUCTURED_OUTER_STATE_BYTES);
			u32 workspace_generation =
				slot->structured_workspace_generation + 1u;
			if (workspace_generation == 0u || workspace_generation > 0x00ffffffu)
			{
				workspace_generation = 1u;
				std::memset(base + GPU_VU_UNIVERSAL_PRODUCT_JOURNAL_VALUE_OFFSET,
					0, VitaGpuVu::StructuredGeneratedPreflightWorkspaceWords *
						sizeof(u32));
			}
			slot->structured_workspace_generation = workspace_generation;
			reinterpret_cast<u32*>(base +
				GPU_VU_UNIVERSAL_PRODUCT_OUTER_STATE_OFFSET)[11] =
				workspace_generation;
			// During preflight, BUFFER10 is the generation-tagged 256 KiB
			// occupancy plane. Publish immutable PairPlan-derived metadata in the
			// disjoint BUFFER9 auxiliary plane so the preflight kernel itself never
			// indexes past word 65535. Later partition roots reuse BUFFER10 as the
			// separately bounded and physically attested 524288-word scratch plane.
		std::memcpy(base + GPU_VU_UNIVERSAL_PRODUCT_JOURNAL_VALUE_OFFSET +
				VitaGpuVu::StructuredGeneratedPreflightWorkspaceWords * sizeof(u32) +
				VitaGpuVu::StructuredGeneratedPreflightMetadataOffset * sizeof(u32),
			&structured.memory_preflight,
			sizeof(structured.memory_preflight));
		std::memcpy(base + GPU_VU_UNIVERSAL_PRODUCT_QP_DESCRIPTOR_OFFSET,
			structured.partition_fixed_qp_numeric.data(),
			structured.partition_module_count *
				sizeof(VitaGpuVu::StructuredFixedQpNumericDescriptor));
		u32 copied_fmac_descriptors = 0u;
		auto* const fmac_descriptors =
			reinterpret_cast<VitaGpuVu::StructuredFixedFmacNumericDescriptor*>(
				base + GPU_VU_UNIVERSAL_PRODUCT_FMAC_DESCRIPTOR_OFFSET);
		const bool copied_fmac =
			VitaGpuVu::CopyStructuredFixedFmacNumericDescriptors(
				epoch->ProgramIdentity(),
				fmac_descriptors,
				VitaGpuVu::StructuredGeneratedMaximumPartitionModuleCount,
				&copied_fmac_descriptors);
		bool fmac_configuration_matches = copied_fmac;
		for (u32 index = 0;
			fmac_configuration_matches && index < copied_fmac_descriptors; index++)
		{
			if (structured.partition_fixed_fmac_operation_counts[index] != 0u)
			{
				fmac_configuration_matches =
					fmac_descriptors[index].configuration_bits ==
						epoch->ConfigurationBits();
			}
		}
		if (!copied_fmac ||
			copied_fmac_descriptors != structured.partition_module_count ||
			!fmac_configuration_matches)
		{
			VitaGpuVu::RejectStructuredGeneratedBundle(
				epoch->ProgramIdentity());
			Console.Error(
				"GPU-VU seq=%llu provider=generated-structured accepted=0 "
				"pre_effect=1 reason=fixed-fmac-descriptor-unavailable "
				"expected_descriptors=%u copied_descriptors=%u config_match=%u "
				"cpu_fallback=1.",
				static_cast<unsigned long long>(epoch->Sequence()),
				structured.partition_module_count, copied_fmac_descriptors,
				static_cast<u32>(fmac_configuration_matches));
			reject(VitaGpuVu::UniversalGpuVuRejection::SubmissionFailed,
				epoch->PreflightPairCount());
			return;
		}
	}

	const u8* const payload = resolve_payload();
	if (!payload)
	{
		reject(VitaGpuVu::UniversalGpuVuRejection::InputUnavailable,
			epoch->PreflightPairCount());
		return;
	}
	const u32 structured_submission_total = generated_structured_ready ?
		epoch->UnpackSubmissionCount() +
			VitaGpuVu::StructuredGeneratedEntryJobCount +
			structured.GeneratedStageCount() + 1u : 0u;
	const u32 batch_count = generated_structured_ready ?
		VitaGpuVu::UniversalGpuVuStructuredSubmissionGroupCount(
			structured_submission_total, 0u,
			structured_preflight_barrier_submission) :
		submission_batch_count(0);
	const u32 structured_firmware_jobs = generated_structured_ready ?
		VitaGpuVu::StructuredGeneratedFirmwareJobCount(
			epoch->UnpackSubmissionCount(),
			structured.partition_firmware_job_count) :
		batch_count;
	if (generated_structured_ready)
	{
		const u64 count = ++gpu_vu_universal_product_structured_submit_count;
		if (count <= 8 || (count & (count - 1)) == 0)
		{
			Console.WriteLn(
				"GPU-VU seq=%llu provider=generated-structured submit=1 "
					"target_pc=%u analysis_pc=%u initial_pc=%u "
					"program_owner=%llu bundle_owner=%llu transaction_stages=%u "
				"serial_entry_jobs=%u fixed_tail_jobs=1 "
				"runtime_preflight=%u pair_bound=%u "
					"fixed_dynamic_bound=%u predicted_jobs=%u/%u "
					"partition_job_budget=%u fixed_state_words=%u "
					"fixed_state_expressions=%u state_modules=%u "
					"max_state_module_expressions=%u "
					"max_state_recurrence_slots=%u state_slice_work=%u "
					"state_total_work=%u "
					"preflight_accesses=%u preflight_invocation_work=%u "
					"fixed_control_state_stages=%u state_outer_per_job=%u "
					"fixed_preflight_stages=3 "
					"workspace_generation=%u occupancy_clear_stores=0 "
					"preflight_source_bytes=%u "
					"partition_source_bytes=%u partitions=%u partition_jobs=%u "
					"max_partition_invocations_per_job=%u "
					"max_partition_invocation_work=%u max_partition_job_work=%u "
					"dynamic_invocation_limit=%u numeric_invocation_limit=%u "
					"unique_programs=%u "
					"scratch=%u fixed_qp=%u fixed_fmac=%u "
					"stores=%u finals=%u max_source=%u "
					"private_store_shape=parallel-child-grid grid_invocations=%u "
						"child_bound=%u fmac_policy=%s unpacks=%u "
					"draws=%u gpu_jobs=%u initial_group=%u group_limit=%u "
					"submit_count=%llu",
				static_cast<unsigned long long>(epoch->Sequence()),
					structured.entry_pc, epoch->AnalysisEntryPc(), state[0],
					static_cast<unsigned long long>(epoch->ProgramIdentity()),
					static_cast<unsigned long long>(structured.program_identity),
				structured.GeneratedStageCount(),
				VitaGpuVu::StructuredGeneratedEntryJobCount,
				static_cast<u32>(structured.runtime_memory_preflight),
				structured.TransactionPairUpperBound(),
				epoch->DynamicPairUpperBound(),
					structured.predicted_generated_firmware_jobs,
					structured.predicted_fixed_firmware_jobs,
					structured.maximum_profitable_partition_jobs,
					structured.fixed_state_descriptor_words,
					structured.fixed_state_expression_count,
					structured.state_module_count,
					structured.maximum_fixed_state_module_expressions,
					structured.maximum_fixed_state_recurrence_slots,
					VitaGpuVu::StructuredGeneratedStateOuterIterationsPerJob *
						structured.maximum_fixed_state_module_expressions,
					structured.maximum_outer_iterations *
						structured.maximum_fixed_state_module_expressions,
					structured.memory_preflight.header0[2],
					structured.maximum_child_iterations *
						structured.memory_preflight.header0[2],
					1u + VitaGpuVu::StructuredGeneratedFixedStateJobCount,
					VitaGpuVu::StructuredGeneratedStateOuterIterationsPerJob,
					slot->structured_workspace_generation,
					structured.preflight_source_bytes,
					structured.partition_source_bytes,
					structured.partition_module_count,
					structured.partition_firmware_job_count,
					structured.maximum_partition_invocations_per_job,
					structured.maximum_partition_invocation_work,
					structured.maximum_partition_job_work,
					VitaGpuVu::StructuredGeneratedMaximumParallelInvocationsPerJob,
					VitaGpuVu::StructuredGeneratedMaximumNumericInvocationsPerJob,
					structured.unique_compiler_program_count,
					structured.scratch_module_count,
					structured.fixed_qp_numeric_module_count,
					structured.fixed_fmac_numeric_module_count,
					structured.memory_store_module_count,
					structured.final_module_count,
					structured.maximum_partition_source_bytes,
				structured.maximum_outer_iterations *
					structured.maximum_child_iterations,
					structured.maximum_child_iterations,
						structured_native_fmac ?
							(gpu_vu_structured_fmac_native_attestation ==
								StructuredNativeFmacAttestation::NearestEven ?
								"native-nearest-even" :
								"native-finite-mantissa-chop") :
							"exact-nearest-even",
						epoch->UnpackSubmissionCount(), structured_submission_total,
					structured_firmware_jobs,
					batch_count,
					VitaGpuVu::UniversalGpuVuMaximumStructuredSubmissionsPerGroup,
					static_cast<unsigned long long>(count));
		}
	}
		if (!submit_slot(slot, payload, false, batch_count, 0u,
				structured_submission_total,
				generated_structured_ready ? NO_GENERATED_HOT_MODULE :
					product_hot_ready ? product_hot_initial_module :
					NO_GENERATED_HOT_MODULE))
	{
		reject(VitaGpuVu::UniversalGpuVuRejection::SubmissionFailed,
			epoch->PreflightPairCount());
	}
}
#endif

bool GSDeviceGXM::Impl::RegisterGeneratedVuProgram(
	VitaGpuVu::CompileResult result,
	VitaGpuVu::GeneratedCgProgram metadata)
{
	const std::pair<u64, u64> map_key(result.key.high, result.key.low);
	const auto existing = generated_vu_programs.find(map_key);
	if (existing != generated_vu_programs.end())
	{
		const GeneratedVuProgram& stored = existing->second;
		const bool executable = stored.registration_complete && stored.id &&
			stored.vertex_program && stored.general_fragment_program &&
			stored.zfloor_fragment_program && stored.opaque_fragment_program;
		VitaGpuVu::CompleteGeneratedProgramRegistration(
			result.key, executable);
		return executable;
	}

	if (!result.succeeded || result.gxp.empty())
	{
		for (const VitaGpuVu::CompileDiagnostic& diagnostic : result.diagnostics)
		{
			Console.Error(
				"GPU-VU: generated shader compile %u at %u:%u: %s",
				diagnostic.code, diagnostic.line, diagnostic.column,
				diagnostic.message.c_str());
		}
		Console.Error(
			"GPU-VU: generated shader compilation failed (%016llx%016llx, "
			"source_bytes=%u source_pairs=%u unique_bodies=%u "
			"stage_bodies=%u/%u/%u/%u "
			"upper_families=%02x lower_families=%05x "
			"resource=%s gxp_bytes=%u primary_instructions=%u "
			"registers=%u/%u/%u scratch=%u thread=%u).",
			static_cast<unsigned long long>(result.key.high),
			static_cast<unsigned long long>(result.key.low),
			metadata.generated_source_bytes,
			metadata.semantic_pair_count,
			metadata.emitted_expression_count,
			metadata.generated_prelude_bodies,
			metadata.generated_upper_bodies,
			metadata.generated_lower_bodies,
			metadata.generated_terminal_bodies,
			metadata.semantic_upper_family_mask,
			metadata.semantic_lower_family_mask,
			VitaGpuVu::GeneratedGxpResourceAttestationName(
				result.gxp_resource_attestation),
			static_cast<u32>(result.gxp.size()),
			result.gxp_resources.primary_instruction_count,
			result.gxp_resources.primary_register_count,
			result.gxp_resources.temporary_register_count,
			result.gxp_resources.secondary_register_count,
			result.gxp_resources.scratch_buffer_size,
			result.gxp_resources.thread_buffer_size);
		VitaGpuVu::CompleteGeneratedProgramRegistration(result.key, false);
		return false;
	}

	GeneratedVuProgram entry;
	entry.key = result.key;
	entry.metadata = std::move(metadata);
	entry.metadata.source.clear();
	entry.gxp = std::move(result.gxp);
	auto [it, inserted] =
		generated_vu_programs.emplace(map_key, std::move(entry));
	if (!inserted)
	{
		VitaGpuVu::CompleteGeneratedProgramRegistration(result.key, true);
		return true;
	}

	GeneratedVuProgram& stored = it->second;
	const SceGxmProgram* const program =
		reinterpret_cast<const SceGxmProgram*>(stored.gxp.data());
	const ShaderPatcherUsage patcher_usage_before =
		GetShaderPatcherUsage(patcher);
	const auto fail_registration =
		[this, &stored, &it, &result, &patcher_usage_before](
		const char* operation, int error) {
		Fail(operation, error);
		bool may_erase = true;
		if (stored.opaque_fragment_program)
		{
			if (sceGxmShaderPatcherReleaseFragmentProgram(
				patcher, stored.opaque_fragment_program) >= 0)
			{
				stored.opaque_fragment_program = nullptr;
			}
			else
			{
				may_erase = false;
			}
		}
		if (stored.general_fragment_program)
		{
			if (sceGxmShaderPatcherReleaseFragmentProgram(
				patcher, stored.general_fragment_program) >= 0)
			{
				stored.general_fragment_program = nullptr;
			}
			else
			{
				may_erase = false;
			}
		}
		if (stored.zfloor_fragment_program)
		{
			if (sceGxmShaderPatcherReleaseFragmentProgram(
				patcher, stored.zfloor_fragment_program) >= 0)
			{
				stored.zfloor_fragment_program = nullptr;
			}
			else
			{
				may_erase = false;
			}
		}
		if (stored.vertex_program)
		{
			if (sceGxmShaderPatcherReleaseVertexProgram(
				patcher, stored.vertex_program) >= 0)
			{
				stored.vertex_program = nullptr;
			}
			else
			{
				may_erase = false;
			}
		}
		if (stored.id)
		{
			if (sceGxmShaderPatcherUnregisterProgram(patcher, stored.id) >= 0)
				stored.id = nullptr;
			else
				may_erase = false;
		}
		const ShaderPatcherUsage usage = GetShaderPatcherUsage(patcher);
		Console.Error(
			"GPU-VU: generated registration resource failure "
			"key=%016llx%016llx operation=%s error=%08x "
			"before_buffer=%u before_vertex_usse=%u before_fragment_usse=%u "
			"after_buffer=%u/%u after_vertex_usse=%u/%u "
			"after_fragment_usse=%u/%u host=%u registered_programs=%u.",
			static_cast<unsigned long long>(result.key.high),
			static_cast<unsigned long long>(result.key.low), operation,
			static_cast<u32>(error), patcher_usage_before.buffer,
			patcher_usage_before.vertex_usse,
			patcher_usage_before.fragment_usse, usage.buffer,
			PATCHER_BUFFER_BYTES, usage.vertex_usse,
			PATCHER_VERTEX_USSE_BYTES, usage.fragment_usse,
			PATCHER_FRAGMENT_USSE_BYTES, usage.host,
			static_cast<u32>(generated_vu_programs.size()));
		VitaGpuVu::CompleteGeneratedProgramRegistration(result.key, false);
		if (may_erase)
			generated_vu_programs.erase(it);
		return false;
	};

	int check_result = sceGxmProgramCheck(program);
	if (check_result < 0)
	{
		return fail_registration("validate generated VU1+TFX GXP header/version",
			check_result);
	}
	VitaGpuVu::GeneratedGxpResourceUsage generated_resources;
	const VitaGpuVu::GeneratedGxpResourceAttestation resource_attestation =
		VitaGpuVu::AttestRuntimeGeneratedGxpResources(
			stored.gxp.data(), stored.gxp.size(), &generated_resources);
#if defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
	const bool validation_scratch_spill =
		resource_attestation ==
			VitaGpuVu::GeneratedGxpResourceAttestation::ScratchSpill &&
		result.gxp_resource_attestation ==
			VitaGpuVu::GeneratedGxpResourceAttestation::ScratchSpill &&
		stored.metadata.execution_kind ==
			VitaGpuVu::GeneratedCgExecutionKind::
				GeneratedLoopKernelDirectVuTfx &&
		VitaGpuVu::IsBoundedParallelStaticValidationScratchSpill(
			generated_resources);
#else
	constexpr bool validation_scratch_spill = false;
#endif
	if (resource_attestation !=
			VitaGpuVu::GeneratedGxpResourceAttestation::Accepted &&
		!validation_scratch_spill)
	{
		Console.Error(
			"GPU-VU: generated GXP rejected before patcher mutation "
			"key=%016llx%016llx resource=%s format=%u.%u sdk=%03x "
			"gxp_bytes=%u primary_instructions=%u secondary_instructions=%u "
			"registers=%u/%u/%u scratch=%u thread=%u literal=%u; "
			"CPU MTVU remains authoritative.",
			static_cast<unsigned long long>(result.key.high),
			static_cast<unsigned long long>(result.key.low),
			VitaGpuVu::GeneratedGxpResourceAttestationName(
				resource_attestation),
			generated_resources.major_version,
			generated_resources.minor_version,
			generated_resources.sdk_version,
			static_cast<u32>(stored.gxp.size()),
			generated_resources.primary_instruction_count,
			generated_resources.secondary_instruction_count,
			generated_resources.primary_register_count,
			generated_resources.temporary_register_count,
			generated_resources.secondary_register_count,
			generated_resources.scratch_buffer_size,
			generated_resources.thread_buffer_size,
			generated_resources.literal_buffer_size);
		return fail_registration("attest generated GPU-VU GXP resources",
			SCE_GXM_ERROR_INVALID_VALUE);
	}
	if (validation_scratch_spill)
	{
		Console.Warning(
			"GPU-VU: validation-only bounded scratch-spill registration "
			"key=%016llx%016llx scratch=%u mode=parallel-static; only the "
			"single transactional canary may execute it.",
			static_cast<unsigned long long>(result.key.high),
			static_cast<unsigned long long>(result.key.low),
			generated_resources.scratch_buffer_size);
	}
	stored.gxp_resources = generated_resources;
	stored.resource_attestation = resource_attestation;
	const u32 generated_vertex_outputs =
		sceGxmProgramGetVertexProgramOutputs(program);
	if ((generated_vertex_outputs & GXM_VERTEX_OUTPUT_POSITION_BIT) == 0u ||
		(stored.metadata.uses_tfx_point_size &&
		 (generated_vertex_outputs & GXM_VERTEX_OUTPUT_PSIZE_BIT) == 0u))
	{
		Console.Error(
			"GPU-VU: generated GXP %016llx%016llx has invalid vertex "
			"outputs=%08x (position=1 point_size=%u required).",
			static_cast<unsigned long long>(result.key.high),
			static_cast<unsigned long long>(result.key.low),
			generated_vertex_outputs,
			stored.metadata.uses_tfx_point_size ? 1u : 0u);
		return fail_registration("validate generated GPU-VU vertex outputs",
			SCE_GXM_ERROR_INVALID_VALUE);
	}
	const bool generated_compute_program = stored.metadata.execution_kind ==
			VitaGpuVu::GeneratedCgExecutionKind::UniversalStateMachine ||
		stored.metadata.execution_kind ==
			VitaGpuVu::GeneratedCgExecutionKind::UniversalDirectStateMachine ||
		stored.metadata.execution_kind ==
			VitaGpuVu::GeneratedCgExecutionKind::UniversalCompactContinuation ||
		stored.metadata.execution_kind ==
			VitaGpuVu::GeneratedCgExecutionKind::StructuredStateSnapshots ||
		stored.metadata.execution_kind ==
			VitaGpuVu::GeneratedCgExecutionKind::StructuredMemoryPreflight ||
		stored.metadata.execution_kind ==
			VitaGpuVu::GeneratedCgExecutionKind::StructuredExpressionScratch ||
		stored.metadata.execution_kind ==
			VitaGpuVu::GeneratedCgExecutionKind::
				StructuredParallelChildMemoryStore ||
		stored.metadata.execution_kind ==
			VitaGpuVu::GeneratedCgExecutionKind::StructuredStoreCommit ||
		stored.metadata.execution_kind ==
			VitaGpuVu::GeneratedCgExecutionKind::StructuredFinalState;
	if (generated_compute_program)
	{
		if ((generated_vertex_outputs & GPU_VU_COMPUTE_REQUIRED_VERTEX_OUTPUTS) !=
			GPU_VU_COMPUTE_REQUIRED_VERTEX_OUTPUTS)
		{
			Console.Error(
				"GPU-VU: generated compute GXP %016llx%016llx has invalid "
				"POINT output contract (outputs=%08x required=%08x).",
				static_cast<unsigned long long>(result.key.high),
				static_cast<unsigned long long>(result.key.low),
				generated_vertex_outputs,
				GPU_VU_COMPUTE_REQUIRED_VERTEX_OUTPUTS);
			return fail_registration(
				"validate generated GPU-VU POINT outputs",
				SCE_GXM_ERROR_INVALID_VALUE);
		}
#if defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
		if (!gpu_vu_universal_product_fragment_id)
		{
			return fail_registration(
				"bind generated universal fragment owner",
				SCE_GXM_ERROR_INVALID_POINTER);
		}
		// r34 exhausted the shared vertex-USSE heap after compiling and
		// registering only part of a large structured bundle. Fail this optional
		// optimization before patcher mutation when one compiler-bounded root no
		// longer has conservative execution-cache headroom. CPU MTVU remains
		// authoritative and no guest effect has occurred.
		if (stored.gxp.size() >
				PATCHER_GENERATED_VERTEX_USSE_HEADROOM_BYTES ||
			patcher_usage_before.vertex_usse >
				PATCHER_VERTEX_USSE_BYTES -
					PATCHER_GENERATED_VERTEX_USSE_HEADROOM_BYTES)
		{
			return fail_registration(
				"generated universal vertex-USSE resource budget",
				GXM_ERROR_OUT_OF_VERTEX_USSE_MEMORY);
		}
		int patch_result = sceGxmShaderPatcherRegisterProgram(
			patcher, program, &stored.id);
		if (patch_result < 0 || !stored.id)
		{
			return fail_registration(
				"register generated universal GPU-VU vertex program",
				patch_result < 0 ? patch_result :
					SCE_GXM_ERROR_INVALID_POINTER);
		}
		patch_result = sceGxmShaderPatcherCreateVertexProgram(
			patcher, stored.id, nullptr, 0, nullptr, 0,
			&stored.vertex_program);
		if (patch_result < 0 || !stored.vertex_program)
		{
			return fail_registration(
				"create generated universal GPU-VU vertex program",
				patch_result < 0 ? patch_result :
					SCE_GXM_ERROR_INVALID_POINTER);
		}
		SceGxmBlendInfo no_color{};
		no_color.colorFunc = SCE_GXM_BLEND_FUNC_NONE;
		no_color.alphaFunc = SCE_GXM_BLEND_FUNC_NONE;
		no_color.colorSrc = SCE_GXM_BLEND_FACTOR_ZERO;
		no_color.colorDst = SCE_GXM_BLEND_FACTOR_ZERO;
		no_color.alphaSrc = SCE_GXM_BLEND_FACTOR_ZERO;
		no_color.alphaDst = SCE_GXM_BLEND_FACTOR_ZERO;
		no_color.colorMask = SCE_GXM_COLOR_MASK_NONE;
		patch_result = sceGxmShaderPatcherCreateFragmentProgram(
			patcher, gpu_vu_universal_product_fragment_id,
			SCE_GXM_OUTPUT_REGISTER_FORMAT_DECLARED,
			SCE_GXM_MULTISAMPLE_NONE, &no_color, program,
			&stored.general_fragment_program);
		if (patch_result < 0 || !stored.general_fragment_program)
		{
			return fail_registration(
				"create generated universal GPU-VU fragment program",
				patch_result < 0 ? patch_result :
					SCE_GXM_ERROR_INVALID_POINTER);
		}
		const ShaderPatcherUsage patcher_usage_after =
			GetShaderPatcherUsage(patcher);
		const auto allocated_delta = [](u32 after, u32 before) {
			return after >= before ? after - before : 0u;
		};
		stored.patcher_buffer_bytes = allocated_delta(
			patcher_usage_after.buffer, patcher_usage_before.buffer);
		stored.patcher_vertex_usse_bytes = allocated_delta(
			patcher_usage_after.vertex_usse,
			patcher_usage_before.vertex_usse);
		stored.patcher_fragment_usse_bytes = allocated_delta(
			patcher_usage_after.fragment_usse,
			patcher_usage_before.fragment_usse);
		VitaGpuVu::CompleteGeneratedProgramRegistration(result.key, true);
		const char* generated_kind = "state-machine";
		switch (stored.metadata.execution_kind)
		{
			case VitaGpuVu::GeneratedCgExecutionKind::
				GeneratedLoopKernelDirectVuTfx:
				generated_kind = "loop-kernel-direct-vu-tfx";
				break;
			case VitaGpuVu::GeneratedCgExecutionKind::UniversalDirectStateMachine:
				generated_kind = "direct-state-machine";
				break;
			case VitaGpuVu::GeneratedCgExecutionKind::UniversalCompactContinuation:
				generated_kind = "compact-continuation";
				break;
			case VitaGpuVu::GeneratedCgExecutionKind::StructuredMemoryPreflight:
				generated_kind = "structured-preflight";
				break;
			case VitaGpuVu::GeneratedCgExecutionKind::StructuredExpressionScratch:
				generated_kind = "structured-expression-scratch";
				break;
			case VitaGpuVu::GeneratedCgExecutionKind::
				StructuredParallelChildMemoryStore:
				generated_kind = "structured-parallel-child-store";
				break;
			case VitaGpuVu::GeneratedCgExecutionKind::StructuredStoreCommit:
				generated_kind = "structured-commit";
				break;
			case VitaGpuVu::GeneratedCgExecutionKind::StructuredFinalState:
				generated_kind = "structured-final";
				break;
			default:
				break;
		}
		Console.WriteLn(
			"GPU-VU: GS registered generated %s program "
			"%016llx%016llx (gxp_bytes=%u source_bytes=%u "
			"source_pairs=%u unique_bodies=%u stage_bodies=%u/%u/%u/%u).",
			generated_kind,
			static_cast<unsigned long long>(result.key.high),
			static_cast<unsigned long long>(result.key.low),
			static_cast<u32>(stored.gxp.size()),
			stored.metadata.generated_source_bytes,
			stored.metadata.semantic_pair_count,
			stored.metadata.emitted_expression_count,
			stored.metadata.generated_prelude_bodies,
			stored.metadata.generated_upper_bodies,
			stored.metadata.generated_lower_bodies,
			stored.metadata.generated_terminal_bodies);
			Console.WriteLn(
				"GPU-VU: generated GXP resource attestation "
				"key=%016llx%016llx status=accepted format=%u.%u sdk=%03x "
				"primary_instructions=%u secondary_instructions=%u "
				"registers=%u/%u/%u scratch=%u thread=%u literal=%u "
				"uniform_vectors=%u binding_vectors=%u dynamic_uniform=%u "
				"instance_live_ins=%u batch_varying_vectors=%u source_abi=%u.",
			static_cast<unsigned long long>(result.key.high),
			static_cast<unsigned long long>(result.key.low),
			stored.gxp_resources.major_version,
			stored.gxp_resources.minor_version,
			stored.gxp_resources.sdk_version,
			stored.gxp_resources.primary_instruction_count,
			stored.gxp_resources.secondary_instruction_count,
			stored.gxp_resources.primary_register_count,
			stored.gxp_resources.temporary_register_count,
			stored.gxp_resources.secondary_register_count,
				stored.gxp_resources.scratch_buffer_size,
				stored.gxp_resources.thread_buffer_size,
				stored.gxp_resources.literal_buffer_size,
				stored.metadata.BatchUniformVectorCount(),
				stored.metadata.BatchBindingVectorCount(),
				static_cast<u32>(
					stored.metadata.uses_dynamic_batch_uniform_index),
				static_cast<u32>(
					stored.metadata.uses_instance_indexed_batch_live_ins),
				stored.metadata.BatchVaryingLiveInVectorCount(),
				stored.metadata.loop_kernel_source_abi);
		Console.WriteLn(
			"GPU-VU: generated patcher usage key=%016llx%016llx "
			"buffer=%u/%u delta=%u vertex_usse=%u/%u delta=%u "
			"fragment_usse=%u/%u delta=%u host=%u "
			"registered_programs=%u.",
			static_cast<unsigned long long>(result.key.high),
			static_cast<unsigned long long>(result.key.low),
			patcher_usage_after.buffer, PATCHER_BUFFER_BYTES,
			stored.patcher_buffer_bytes, patcher_usage_after.vertex_usse,
			PATCHER_VERTEX_USSE_BYTES, stored.patcher_vertex_usse_bytes,
			patcher_usage_after.fragment_usse,
			PATCHER_FRAGMENT_USSE_BYTES,
			stored.patcher_fragment_usse_bytes, patcher_usage_after.host,
			static_cast<u32>(generated_vu_programs.size()));
		return true;
#else
		return fail_registration("universal GPU-VU owner is not compiled",
			SCE_GXM_ERROR_INVALID_VALUE);
#endif
	}
	const bool flat_instances = stored.metadata.uses_flat_instance_inputs;
	const bool buffered_batch =
		stored.metadata.uses_buffered_batch_inputs;
	const bool instance_batch_live_ins =
		stored.metadata.uses_instance_indexed_batch_live_ins;
	const bool structured_snapshot_inputs =
		stored.metadata.UsesStructuredSnapshotInputBuffers();
	if (!stored.metadata.HasValidDirectTfxInputMode())
	{
		return fail_registration("bind generated VU1+TFX vertex inputs",
			SCE_GXM_ERROR_INVALID_VALUE);
	}
	u32 generated_attribute_count = 0;
	const bool valid_attribute_masks =
		stored.metadata.GetDirectTfxVertexAttributeCount(&generated_attribute_count);
	const u32 generated_stream_count =
		static_cast<u32>(stored.metadata.memory_inputs.size());
	const u32 patch_attribute_count = instance_batch_live_ins ?
		stored.metadata.BatchInstanceLiveInVectorCount() :
		(buffered_batch ? 0u : generated_attribute_count);
	const u32 patch_stream_count = instance_batch_live_ins ? 1u :
		(buffered_batch ? 0u : generated_stream_count);
	if (!valid_attribute_masks ||
		patch_attribute_count > 16 || patch_stream_count > 16 ||
		(buffered_batch && stored.metadata.memory_inputs.empty()))
	{
		return fail_registration("bind generated VU1+TFX vertex inputs",
			SCE_GXM_ERROR_INVALID_VALUE);
	}
	const bool private_state_canary =
		VitaGpuVu::HasGeneratedLoopKernelStateCanaryContract(stored.metadata);
	if (stored.metadata.uses_loop_kernel_state_canary != private_state_canary ||
		(!stored.metadata.uses_tfx_uniforms && !private_state_canary))
	{
		return fail_registration("validate generated VU1+TFX root",
			SCE_GXM_ERROR_INVALID_VALUE);
	}

	const auto find_uniform = [program](const char* name) {
		const SceGxmProgramParameter* const parameter =
			sceGxmProgramFindParameterByName(program, name);
		return (parameter &&
			sceGxmProgramParameterGetCategory(parameter) ==
				SCE_GXM_PARAMETER_CATEGORY_UNIFORM) ?
			parameter : nullptr;
	};
	stored.uniforms.vertex_scale_offset =
		find_uniform("VertexScaleOffset");
	stored.uniforms.max_depth = find_uniform("MaxDepth");
	if (private_state_canary ?
		(stored.uniforms.vertex_scale_offset || stored.uniforms.max_depth) :
		(!stored.uniforms.vertex_scale_offset || !stored.uniforms.max_depth))
	{
		return fail_registration("find generated VU1+TFX state uniforms",
			SCE_GXM_ERROR_INVALID_VALUE);
	}
	stored.uniforms.constants.resize(stored.metadata.constant_inputs.size());
	for (u32 index = 0; index < stored.metadata.constant_inputs.size(); index++)
	{
		const VitaGpuVu::CgConstantInput& input =
			stored.metadata.constant_inputs[index];
		if (!input.address.valid ||
			input.address.invocation_coefficient != 0 ||
			input.address.outer_invocation_coefficient != 0 ||
			input.uniform_index != index)
		{
			return fail_registration(
				"validate generated VU1 constant input layout",
				SCE_GXM_ERROR_INVALID_VALUE);
		}
		char name[24];
		std::snprintf(name, sizeof(name), "VuConstant%u", index);
		if (!buffered_batch && !structured_snapshot_inputs)
			stored.uniforms.constants[index] = find_uniform(name);
		if (!buffered_batch && !structured_snapshot_inputs &&
			!stored.uniforms.constants[index])
		{
			return fail_registration("find generated VU1 constant uniform",
				SCE_GXM_ERROR_INVALID_VALUE);
		}
	}
	for (u32 reg = 1; reg < stored.uniforms.vf.size(); reg++)
	{
		if ((stored.metadata.vf_uniform_mask & (1u << reg)) == 0)
			continue;
		char name[8];
		std::snprintf(name, sizeof(name), "VF%02u", reg);
		if (!buffered_batch && !structured_snapshot_inputs)
			stored.uniforms.vf[reg] = find_uniform(name);
		if (!buffered_batch && !structured_snapshot_inputs &&
			!stored.uniforms.vf[reg])
		{
			return fail_registration("find generated VU1+TFX VF uniform",
				SCE_GXM_ERROR_INVALID_VALUE);
		}
	}
	const auto require_uniform = [&find_uniform](
		bool used, const char* name,
		const SceGxmProgramParameter** destination) {
		if (!used)
			return true;
		*destination = find_uniform(name);
		return *destination != nullptr;
	};
	if (!buffered_batch && !structured_snapshot_inputs &&
		(!require_uniform(stored.metadata.uses_acc_uniform, "ACC",
				&stored.uniforms.acc) ||
			!require_uniform(stored.metadata.uses_q_uniform, "Q",
				&stored.uniforms.q) ||
			!require_uniform(stored.metadata.uses_p_uniform, "P",
				&stored.uniforms.p) ||
			!require_uniform(stored.metadata.uses_i_uniform, "I",
				&stored.uniforms.i) ||
			!require_uniform(stored.metadata.uses_gif_q_uniform, "GifQ",
				&stored.uniforms.gif_q)))
	{
		return fail_registration("find generated VU1+TFX scalar uniform",
			SCE_GXM_ERROR_INVALID_VALUE);
	}

	std::vector<SceGxmVertexAttribute> attributes;
	attributes.reserve(patch_attribute_count);
	std::vector<SceGxmVertexStream> streams(patch_stream_count);
	if (instance_batch_live_ins)
	{
		u32 attribute_vector = 0u;
		const auto append_attribute =
			[&](const char* parameter_name) {
				const SceGxmProgramParameter* const parameter =
					sceGxmProgramFindParameterByName(program, parameter_name);
				if (!parameter ||
					sceGxmProgramParameterGetCategory(parameter) !=
						SCE_GXM_PARAMETER_CATEGORY_ATTRIBUTE)
				{
					return false;
				}
				SceGxmVertexAttribute attribute{};
				attribute.streamIndex = 0u;
				attribute.offset = static_cast<u16>(attribute_vector * 16u);
				attribute.format = SCE_GXM_ATTRIBUTE_FORMAT_UNTYPED;
				attribute.componentCount = 4u;
				attribute.regIndex =
					sceGxmProgramParameterGetResourceIndex(parameter);
				attributes.push_back(attribute);
				attribute_vector++;
				return true;
			};
		if (!append_attribute("VuBatchIdentity"))
		{
			return fail_registration(
				"find generated VU1 batch identity attribute",
				SCE_GXM_ERROR_INVALID_VALUE);
		}
		for (u32 index = 0u;
			index < stored.metadata.constant_inputs.size(); index++)
		{
			char name[24];
			std::snprintf(name, sizeof(name), "VuConstant%u", index);
			if (!append_attribute(name))
			{
				return fail_registration(
					"find generated VU1 constant live-in attribute",
					SCE_GXM_ERROR_INVALID_VALUE);
			}
		}
		for (u32 reg = 1u; reg < 32u; reg++)
		{
			if ((stored.metadata.vf_uniform_mask & (1u << reg)) == 0u)
				continue;
			char name[8];
			std::snprintf(name, sizeof(name), "VF%02u", reg);
			if (!append_attribute(name))
			{
				return fail_registration(
					"find generated VU1 VF live-in attribute",
					SCE_GXM_ERROR_INVALID_VALUE);
			}
		}
		if (stored.metadata.uses_acc_uniform && !append_attribute("ACC"))
		{
			return fail_registration(
				"find generated VU1 ACC live-in attribute",
				SCE_GXM_ERROR_INVALID_VALUE);
		}
		const bool uses_scalar_vector =
			stored.metadata.uses_q_uniform ||
			stored.metadata.uses_p_uniform ||
			stored.metadata.uses_i_uniform ||
			stored.metadata.uses_gif_q_uniform;
		if (uses_scalar_vector && !append_attribute("VuBatchScalars"))
		{
			return fail_registration(
				"find generated VU1 scalar live-in attribute",
				SCE_GXM_ERROR_INVALID_VALUE);
		}
		if (attribute_vector != patch_attribute_count)
		{
			return fail_registration(
				"validate generated VU1 batch live-in attributes",
				SCE_GXM_ERROR_INVALID_VALUE);
		}
		SceGxmVertexStream& stream = streams.front();
		stream.stride = static_cast<u16>(attribute_vector * 16u);
		stream.indexSource = SCE_GXM_INDEX_SOURCE_INSTANCE_16BIT;
	}
	for (u32 index = 0; index < stored.metadata.memory_inputs.size(); index++)
	{
		const VitaGpuVu::CgMemoryInput& input =
			stored.metadata.memory_inputs[index];
		if (input.attribute_index != index ||
			input.address.invocation_coefficient < 0 ||
			input.address.outer_invocation_coefficient < 0)
		{
			return fail_registration("validate generated VU1+TFX input layout",
				SCE_GXM_ERROR_INVALID_VALUE);
		}
		if (buffered_batch)
			continue;
		const u64 vertex_stride =
			static_cast<u64>(input.address.invocation_coefficient) * 16u;
		const u64 stream_stride = vertex_stride *
			(flat_instances ?
				stored.metadata.flat_instance_vertex_step : 1u);
		const u64 maximum_attribute_offset = vertex_stride *
			(flat_instances ?
				stored.metadata.flat_vertices_per_primitive - 1u : 0u);
		if (stream_stride > std::numeric_limits<u16>::max() ||
			maximum_attribute_offset > std::numeric_limits<u16>::max())
		{
			return fail_registration("validate generated VU1+TFX input stride",
				SCE_GXM_ERROR_INVALID_VALUE);
		}

		const u32 vertex_attributes = flat_instances ?
			stored.metadata.flat_vertices_per_primitive : 1u;
		for (u32 vertex = 0; vertex < vertex_attributes; vertex++)
		{
			if (flat_instances &&
				(input.flat_attribute_vertex_mask & (1u << vertex)) == 0)
			{
				continue;
			}
			char parameter_name[40];
			if (flat_instances)
			{
				std::snprintf(parameter_name, sizeof(parameter_name),
					"VuMemory%uVertex%u", input.attribute_index, vertex);
			}
			else
			{
				std::snprintf(parameter_name, sizeof(parameter_name),
					"VuMemory%u", input.attribute_index);
			}
			const SceGxmProgramParameter* const parameter =
				sceGxmProgramFindParameterByName(program, parameter_name);
			if (!parameter ||
				sceGxmProgramParameterGetCategory(parameter) !=
					SCE_GXM_PARAMETER_CATEGORY_ATTRIBUTE)
			{
				return fail_registration("find generated VU1+TFX input",
					SCE_GXM_ERROR_INVALID_VALUE);
			}

			SceGxmVertexAttribute attribute{};
			attribute.streamIndex = static_cast<u16>(index);
			attribute.offset = static_cast<u16>(vertex_stride * vertex);
			// Shader_Compiler-Users_Guide::Offline Vertex Unpacking: a
			// __regformat int4 attribute is four untyped 32-bit words.
			attribute.format = SCE_GXM_ATTRIBUTE_FORMAT_UNTYPED;
			attribute.componentCount = 4;
			attribute.regIndex =
				sceGxmProgramParameterGetResourceIndex(parameter);
			attributes.push_back(attribute);
		}
		SceGxmVertexStream& stream = streams[index];
		stream.stride = static_cast<u16>(stream_stride);
		// Sony api_libgxm/instancing: instance-indexed streams advance once
		// per indexWrap group, independently of primitive-local indices.
		stream.indexSource = flat_instances ?
			SCE_GXM_INDEX_SOURCE_INSTANCE_16BIT :
			SCE_GXM_INDEX_SOURCE_INDEX_16BIT;
	}
	int patch_result = sceGxmShaderPatcherRegisterProgram(
		patcher, program, &stored.id);
	if (patch_result < 0 || !stored.id)
	{
		return fail_registration("register generated VU1+TFX vertex program",
			patch_result < 0 ? patch_result : SCE_GXM_ERROR_INVALID_POINTER);
	}
	patch_result = sceGxmShaderPatcherCreateVertexProgram(
		patcher, stored.id,
		attributes.empty() ? nullptr : attributes.data(), attributes.size(),
		streams.empty() ? nullptr : streams.data(), streams.size(),
		&stored.vertex_program);
	if (patch_result < 0 || !stored.vertex_program)
	{
		return fail_registration("create generated VU1+TFX vertex program",
			patch_result < 0 ? patch_result : SCE_GXM_ERROR_INVALID_POINTER);
	}
	const bool uv_no_fog_interface =
		stored.metadata.uses_tfx_uv_no_fog_interface;
	const SceGxmShaderPatcherId general_fragment_id =
		uv_no_fog_interface ? tfx_uv_no_fog_fragment_id : tfx_fragment_id;
	const SceGxmShaderPatcherId zfloor_fragment_id =
		uv_no_fog_interface ?
			tfx_uv_no_fog_zfloor_fragment_id : tfx_zfloor_fragment_id;
	const SceGxmShaderPatcherId opaque_fragment_id =
		uv_no_fog_interface ?
			tfx_uv_no_fog_fast_fragment_id : tfx_fast_fragment_id;
	patch_result = sceGxmShaderPatcherCreateFragmentProgram(patcher,
		general_fragment_id, SCE_GXM_OUTPUT_REGISTER_FORMAT_DECLARED,
		SCE_GXM_MULTISAMPLE_NONE, nullptr, program,
		&stored.general_fragment_program);
	if (patch_result < 0 || !stored.general_fragment_program)
	{
		return fail_registration(
			"create generated VU1+TFX general fragment program",
			patch_result < 0 ? patch_result : SCE_GXM_ERROR_INVALID_POINTER);
	}
	patch_result = sceGxmShaderPatcherCreateFragmentProgram(patcher,
		zfloor_fragment_id, SCE_GXM_OUTPUT_REGISTER_FORMAT_DECLARED,
		SCE_GXM_MULTISAMPLE_NONE, nullptr, program,
		&stored.zfloor_fragment_program);
	if (patch_result < 0 || !stored.zfloor_fragment_program)
	{
		return fail_registration(
			"create generated VU1+TFX Z-floor fragment program",
			patch_result < 0 ? patch_result : SCE_GXM_ERROR_INVALID_POINTER);
	}
	patch_result = sceGxmShaderPatcherCreateFragmentProgram(patcher,
		opaque_fragment_id, SCE_GXM_OUTPUT_REGISTER_FORMAT_DECLARED,
		SCE_GXM_MULTISAMPLE_NONE, nullptr, program,
		&stored.opaque_fragment_program);
	if (patch_result < 0 || !stored.opaque_fragment_program)
	{
		return fail_registration(
			"create generated VU1+TFX opaque fragment program",
			patch_result < 0 ? patch_result : SCE_GXM_ERROR_INVALID_POINTER);
	}

	stored.registration_complete = true;
	VitaGpuVu::CompleteGeneratedProgramRegistration(result.key, true);
	Console.WriteLn(
		"GPU-VU: GS registered generated VU1+TFX program %016llx%016llx "
		"(%u raw inputs%s, %u expressions, "
		"general+Z-floor+opaque %sTFX links).",
		static_cast<unsigned long long>(result.key.high),
		static_cast<unsigned long long>(result.key.low),
		static_cast<u32>(stored.metadata.memory_inputs.size()),
		buffered_batch ? " through two user buffers" : " as streams",
		stored.metadata.emitted_expression_count,
		uv_no_fog_interface ? "fixed-UV/no-fog " : "");
	return true;
}

void GSDeviceGXM::Impl::PollGeneratedVuPrograms()
{
	RetireCompletedGpuVuDraws();
	VitaGpuVu::CompileResult result;
	VitaGpuVu::GeneratedCgProgram metadata;
	while (VitaGpuVu::PollGeneratedProgramCompile(&result, &metadata))
	{
		RegisterGeneratedVuProgram(std::move(result), std::move(metadata));
		result = {};
		metadata = {};
	}
}

GSDeviceGXM::Impl::GeneratedVuProgram*
GSDeviceGXM::Impl::FindGeneratedVuProgram(
	const VitaGpuVu::ShaderKey& key)
{
	const auto it = generated_vu_programs.find({key.high, key.low});
	return it != generated_vu_programs.end() ? &it->second : nullptr;
}

bool GSDeviceGXM::Impl::InitializeGpuVuRetirements()
{
	volatile unsigned int* const notification_region =
		sceGxmGetNotificationRegion();
	if (!notification_region)
		return false;

	// Reserve every owner before Shacc, mapped input rings and render targets
	// consume the remaining LPDDR.  Physical r176 reached this path with only
	// 8,224 newlib bytes free and later aborted while growing the scene private
	// output vector.  These buffers rotate by swap below, so no accepted scene
	// performs a retirement-container allocation.
	try
	{
		gpu_vu_scene_batch_allocations.reserve(
			GPU_VU_RETIREMENT_BATCH_ALLOCATION_CAPACITY);
		gpu_vu_scene_private_store_outputs.reserve(
			GPU_VU_RETIREMENT_PRIVATE_OUTPUT_CAPACITY);
		for (GpuVuRetirementSlot& slot : gpu_vu_retirement_slots)
		{
			slot.batch_allocations.reserve(
				GPU_VU_RETIREMENT_BATCH_ALLOCATION_CAPACITY);
			slot.private_store_outputs.reserve(
				GPU_VU_RETIREMENT_PRIVATE_OUTPUT_CAPACITY);
		}
	}
	catch (const std::bad_alloc&)
	{
		Console.Error(
			"GPU-VU: fixed retirement storage reservation failed before effects; "
			"generated admission remains disabled.");
		return false;
	}

	// No other VitaSX2 subsystem consumes notification words. Sony's
	// precomputation and instancing samples allocate linearly from this region;
	// reserve four vertex and four fragment words so a submitted value is never
	// overwritten before its slot is explicitly reused. Universal-product
	// validation owns the following two words. The next word is a Phyre-style
	// process-lifetime fragment progress notification: reusing one monotonically
	// increasing address is safe because one immediate context submits fragment
	// jobs in order, and it exposes fragment backlog without retaining another
	// large descriptor-owner ring.
	for (u32 i = 0; i < gpu_vu_retirement_slots.size(); i++)
	{
		GpuVuRetirementSlot& slot = gpu_vu_retirement_slots[i];
		slot.notification.address = notification_region + i;
		slot.notification.value = 0;
		slot.fragment_notification.address =
			notification_region + GPU_VU_RETIREMENT_SLOT_COUNT + i;
		slot.fragment_notification.value = 0;
		*slot.notification.address = 0;
		*slot.fragment_notification.address = 0;
		ReleaseGpuVuInputSlots(
			&slot.input_retentions, &slot.input_retention_count);
		slot.draw_count = 0;
		slot.submission_wall = 0;
		slot.submission_scene_serial = 0;
		slot.retained_sequence_begin = 0;
		slot.retained_sequence_end = 0;
		slot.retained_sequence_flags = 0;
		slot.batch_allocations.clear();
		slot.private_store_outputs.clear();
		slot.submitted = false;
	}
	constexpr u32 fragment_progress_notification_index =
		2u * GPU_VU_RETIREMENT_SLOT_COUNT +
		GPU_VU_UNIVERSAL_PRODUCT_SLOT_COUNT;
	// Sony's libGXM overview defines the notification region as 512 u32
	// entries.  The VitaSDK compatibility headers used by this target expose
	// sceGxmGetNotificationRegion() but omit SCE_GXM_NOTIFICATION_COUNT.
	constexpr u32 gxm_notification_region_count = 512u;
	static_assert(fragment_progress_notification_index <
		gxm_notification_region_count);
	gpu_vu_fragment_progress_notification.address =
		notification_region + fragment_progress_notification_index;
	gpu_vu_fragment_progress_notification.value = 0u;
	*gpu_vu_fragment_progress_notification.address = 0u;
	gpu_vu_fragment_progress_history.fill(GpuVuFragmentProgressRecord{});
	gpu_vu_fragment_progress_last_submitted = 0u;
	gpu_vu_fragment_progress_last_completed = 0u;
	gpu_vu_fragment_progress_completed_scene = 0u;
	gpu_vu_fragment_progress_completed_generated_scene = 0u;
	gpu_vu_fragment_progress_completed_generated_sequence = 0u;
	gpu_vu_fragment_progress_history_overruns = 0u;
	gpu_vu_last_generated_scene_serial = 0u;
	gpu_vu_generated_scene_manifest = {};
	ReleaseGpuVuInputSlots(&gpu_vu_scene_input_retentions,
		&gpu_vu_scene_input_retention_count);
	gpu_vu_scene_draw_count = 0;
	gpu_vu_scene_private_store_outputs.clear();
	next_gpu_vu_retirement_slot = 0;
	gpu_vu_retirement_health_pass = 0;
	gpu_vu_device_faulted = false;
	gpu_vu_retirements_ready = true;
	return true;
}

void GSDeviceGXM::Impl::PublishGpuVuRetirementHealth(
	GpuVuHealthRetirementStage stage,
	GpuVuHealthRetirementAction action, u32 current_slot)
{
#if defined(__vita__) && defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
	VitaGpuVu::HealthJournal::RetirementState health{};
	const Common::Timer::Value now = Common::Timer::GetCurrentValue();
	health.update_time_us = static_cast<u64>(
		Common::Timer::ConvertValueToSeconds(now) * 1000000.0);
	health.stage = static_cast<u32>(stage);
	health.current_slot = current_slot;
	health.action = static_cast<u32>(action);
	if (++gpu_vu_retirement_health_pass == 0u)
		gpu_vu_retirement_health_pass = 1u;
	health.pass = gpu_vu_retirement_health_pass;
	static_assert(GPU_VU_RETIREMENT_SLOT_COUNT ==
		std::tuple_size<decltype(health.slots)>::value);
	for (u32 index = 0u; index < gpu_vu_retirement_slots.size(); index++)
	{
		const GpuVuRetirementSlot& source = gpu_vu_retirement_slots[index];
		VitaGpuVu::HealthJournal::RetirementSlotState& destination =
			health.slots[index];
		destination.update_time_us = health.update_time_us;
		destination.scene = source.submission_scene_serial;
		destination.notification_address =
			reinterpret_cast<uptr>(source.notification.address);
		destination.notification_required = source.notification.value;
		destination.submitted = source.submitted ? 1u : 0u;
		const u32 observed = source.notification.address ?
			*source.notification.address : 0u;
		destination.notification_observed = observed;
		if (!source.submitted)
		{
			destination.stage = static_cast<u32>(
				GpuVuHealthRetirementSlotStage::Empty);
		}
		else if (!source.notification.address)
		{
			destination.stage = static_cast<u32>(
				GpuVuHealthRetirementSlotStage::InvalidNotification);
		}
		else
		{
			destination.stage = static_cast<u32>(
				VitaGpuVu::HasCompletedNotificationValue(
					observed, source.notification.value) ?
					GpuVuHealthRetirementSlotStage::Reached :
					GpuVuHealthRetirementSlotStage::Pending);
		}

		destination.sequence_begin = source.retained_sequence_begin;
		destination.sequence_end = source.retained_sequence_end;
		destination.flags = source.retained_sequence_flags;
		destination.draw_count = static_cast<u32>(std::min<u64>(
			source.draw_count, std::numeric_limits<u32>::max()));
		destination.private_output_count = static_cast<u32>(
			std::min<size_t>(source.private_store_outputs.size(),
				std::numeric_limits<u32>::max()));
		destination.input_retention_count = source.input_retention_count;
		destination.allocation_count = static_cast<u32>(
			std::min<size_t>(source.batch_allocations.size(),
				std::numeric_limits<u32>::max()));
	}
	VitaGpuVu::HealthJournal::PublishRetirement(health);
#else
	(void)stage;
	(void)action;
	(void)current_slot;
#endif
}

void GSDeviceGXM::Impl::RetireGpuVuPrivateStoreOutputs(
	std::vector<GpuVuPrivateStoreOutput>* outputs, bool compare_results)
{
	if (!outputs)
		return;
	bool completion_only_cpu1_progress = false;
	for (GpuVuPrivateStoreOutput& output : *outputs)
	{
		if (output.completion_only)
		{
			const bool had_transaction = static_cast<bool>(output.transaction);
			const bool completion_ok = compare_results && output.transaction &&
				output.transaction->PublishGpuCompletion();
			if (!completion_ok && output.transaction)
			{
				output.transaction->MarkFailed(
					VitaGpuVu::GeneratedLoopKernelTransactionFailure::
						InvalidGpuCompletionPublication);
			}
			if (!completion_ok || output.sequence <= 8u ||
				(output.sequence & (output.sequence - 1u)) == 0u)
			{
				Console.WriteLn(
					"GPU-VU seq=%llu provider=generated-loop-kernel "
					"transaction_commit=%s diagnostic_bytes=0 "
					"canonical_store_journal=pairplan-exact cpu_vu_calls=0.",
					static_cast<unsigned long long>(output.sequence),
					completion_ok ? "ready" : "FAILED");
			}
			output.transaction.reset();
			completion_only_cpu1_progress |= had_transaction;
			continue;
		}
		const u32 total_payload_bytes = output.TotalPayloadBytes();
		const u8* const payload_data = output.PayloadData();
		const u8* const guard_data = output.GuardData();
		bool guard_ok = payload_data && guard_data;
		if (guard_ok && output.allocation)
		{
			const u8* const allocation_begin =
				static_cast<const u8*>(output.allocation.Data());
			const u8* const allocation_end =
				allocation_begin + output.allocation.Size();
			guard_ok = payload_data >= allocation_begin &&
				guard_data >= allocation_begin &&
				guard_data + GPU_VU_PRIVATE_STORE_GUARD_BYTES <=
					allocation_end;
		}
		if (guard_ok)
		{
			const u32* const guard = reinterpret_cast<const u32*>(
				guard_data);
			for (u32 word = 0;
				word < GPU_VU_PRIVATE_STORE_GUARD_BYTES / sizeof(u32); word++)
			{
				if (guard[word] != GPU_VU_PRIVATE_STORE_GUARD)
				{
					guard_ok = false;
					break;
				}
			}
		}

		if (output.transaction)
		{
			const u32 transaction_bytes = output.payload_bytes;
			const bool transaction_ok = compare_results && guard_ok &&
				payload_data &&
				transaction_bytes == output.transaction->OutputBytes() &&
				output.transaction->PublishGpuOutput(
					reinterpret_cast<const u32*>(payload_data),
					transaction_bytes / sizeof(u32));
			if (!transaction_ok)
			{
				output.transaction->MarkFailed(
					VitaGpuVu::GeneratedLoopKernelTransactionFailure::
						InvalidGpuOutputPublication);
				Console.Error(
					"GPU-VU seq=%llu provider=generated-loop-kernel "
					"transaction_commit=FAILED guard=%u bytes=%u/%u.",
					static_cast<unsigned long long>(output.sequence),
					static_cast<u32>(guard_ok), transaction_bytes,
					output.transaction->OutputBytes());
			}
			else if (output.sequence <= 8u ||
				(output.sequence & (output.sequence - 1u)) == 0u)
			{
				Console.WriteLn(
					"GPU-VU seq=%llu provider=generated-loop-kernel "
					"transaction_commit=ready memory_entries=%u pre_loop_stores=%u "
					"output_profile=%s canonical_store_journal=%s "
					"state_formula=o1 cpu_vu_calls=0.",
					static_cast<unsigned long long>(output.sequence),
					output.entry_count,
					output.transaction->PreLoopStoreEntryCount(),
					VitaGpuVu::GeneratedLoopKernelNumericProfileName(
						output.transaction->OutputNumericProfile()),
					output.transaction->StoreCommitModeName());
			}
			output.transaction.reset();
			vu1Thread.NotifyUniversalGpuVuProgress();
		}

		if (!output.compare_with_cpu_oracle || !compare_results)
		{
			if (!guard_ok)
			{
				Console.Error(
					"GPU-VU seq=%llu private_store_guard=FAILED entries=%u "
					"stores=%u; generated BUFFER2 write escaped its allocation.",
					static_cast<unsigned long long>(output.sequence),
					output.entry_count, output.stores_per_invocation);
			}
			continue;
		}

		bool numeric_probe_ok = !output.HasFtoiProbe();
		if (output.HasFtoiProbe())
		{
			const bool probe_shape_ok = guard_ok && payload_data &&
				output.FtoiProbeData() &&
				output.ftoi_probe_bytes ==
					output.ftoi_probe_count * 8u * sizeof(u32) &&
				output.ftoi_probe_outer_count != 0u &&
				output.ftoi_probe_child_count != 0u &&
				static_cast<u64>(output.ftoi_probe_outer_count) *
					output.ftoi_probe_child_count == output.ftoi_probe_count &&
				output.FtoiProbeOffset() + output.ftoi_probe_bytes ==
					total_payload_bytes &&
				output.expectations.size() == output.entry_count &&
				output.stores_per_invocation != 0u &&
				output.entry_count ==
					output.ftoi_probe_count * output.stores_per_invocation &&
				output.ftoi_probe_store_index < output.stores_per_invocation &&
				output.ftoi_probe_lane < 4u &&
				output.ftoi_expectations.size() == output.ftoi_probe_count &&
				VitaGpuVu::IsGeneratedCgExactDivideConfigurationSupported(
					output.ftoi_probe_configuration_bits) &&
				VitaGpuVu::IsGeneratedCgNativeF32ConfigurationSupported(
					output.ftoi_probe_configuration_bits) &&
				(output.ftoi_probe_configuration_bits &
				 VitaGpuVu::UniversalConfigurationApproximateConversions) != 0u;
			u32 reference_failures = 0u;
			u32 gpu_exact_ftoi_mismatches = 0u;
			u32 gpu_playable_ftoi_mismatches = 0u;
			u32 product_exact_differences = 0u;
			u32 product_native_model_mismatches = 0u;
			u32 store_ftoi_cpu_differences = 0u;
			u32 exact_ftoi_cpu_differences = 0u;
			u32 playable_ftoi_cpu_differences = 0u;
			u32 store_conversion_only_differences = 0u;
			u32 upstream_differences = 0u;
			u32 store_approximation_cancellations = 0u;
			u32 graph_ftoi_cpu_differences = 0u;
			u32 graph_compact_outer_zero_cpu_differences = 0u;
			u32 graph_outer_memory_zero_cpu_differences = 0u;
			u32 graph_repeated_add_zero_cpu_differences = 0u;
			u32 graph_all_outer_zero_cpu_differences = 0u;
			u32 gpu_left_reference_mismatches = 0u;
			u32 gpu_right_reference_mismatches = 0u;
			u32 gpu_product_reference_mismatches = 0u;
			u32 upstream_expression_differences = 0u;
			u32 native_multiply_only_differences = 0u;
			u64 graph_mismatch_outer_mask = 0u;
			u64 graph_mismatch_child_mask = 0u;
			u32 first_graph_mismatch_invocation =
				std::numeric_limits<u32>::max();
			u32 first_graph_mismatch_expected = 0u;
			u32 first_graph_mismatch_cpu = 0u;
			u32 first_graph_value_match_invocation =
				std::numeric_limits<u32>::max();
			u32 first_graph_value_match_count = 0u;
			std::array<u32, 8> graph_outer_expected{};
			std::array<u32, 8> graph_outer_cpu{};
			std::array<u32, 8> graph_outer_gpu{};
			std::array<u16, 8> graph_outer_qword{};
			u32 first_difference_invocation =
				std::numeric_limits<u32>::max();
			u32 first_left = 0u;
			u32 first_right = 0u;
			u32 first_product = 0u;
			u32 first_exact_product = 0u;
			u32 first_store_ftoi = 0u;
			u32 first_exact_ftoi = 0u;
			u32 first_playable_ftoi = 0u;
			u32 first_cpu_ftoi = 0u;
			u32 first_pair_pc = 0u;
			u32 first_source_vf = 0u;
			u32 first_expected_left = 0u;
			u32 first_expected_right = 0u;
			u32 first_expected_product = 0u;
			u32 first_expected_ftoi = 0u;
			u32 first_left_node = 0u;
			u32 first_right_node = 0u;
			u32 first_product_node = 0u;
			u32 first_ftoi_node = 0u;
			u32 first_probe_mismatch_invocation =
				std::numeric_limits<u32>::max();
			u32 first_probe_mismatch_product = 0u;
			u32 first_probe_mismatch_gpu = 0u;
			u32 first_probe_mismatch_host = 0u;
			u32 first_playable_mismatch_invocation =
				std::numeric_limits<u32>::max();
			u32 first_playable_mismatch_product = 0u;
			u32 first_playable_mismatch_gpu = 0u;
			u32 first_playable_mismatch_host = 0u;
			if (probe_shape_ok)
			{
				const u32* const stores =
					reinterpret_cast<const u32*>(payload_data);
				const u32* const probes = reinterpret_cast<const u32*>(
					output.FtoiProbeData());
				for (u32 invocation = 0u;
					invocation < output.ftoi_probe_count; invocation++)
				{
					const u32 entry = invocation * output.stores_per_invocation +
						output.ftoi_probe_store_index;
					if (entry >= output.entry_count)
					{
						reference_failures++;
						continue;
					}
					const VitaGpuVu::PrivateStoreExpectation& expectation =
						output.expectations[entry];
					const u32 lane_bit = 0x8u >> output.ftoi_probe_lane;
					if ((expectation.lane_mask & lane_bit) == 0u)
					{
						reference_failures++;
						continue;
					}
					const u32 left = probes[invocation * 8u + 0u];
					const u32 right = probes[invocation * 8u + 1u];
					const u32 product = probes[invocation * 8u + 2u];
					const u32 gpu_exact_ftoi = probes[invocation * 8u + 3u];
					const u32 gpu_playable_ftoi = probes[invocation * 8u + 4u];
					const u32 store_ftoi =
						stores[entry * 4u + output.ftoi_probe_lane];
					const u32 cpu_ftoi = expectation.expected[output.ftoi_probe_lane];
					const VitaGpuVu::FtoiProbeExpectation& probe_expectation =
						output.ftoi_expectations[invocation];
					if ((invocation % output.ftoi_probe_child_count) == 0u)
					{
						const u32 outer =
							invocation / output.ftoi_probe_child_count;
						if (outer < graph_outer_expected.size())
						{
							graph_outer_expected[outer] =
								probe_expectation.converted;
							graph_outer_cpu[outer] = cpu_ftoi;
							graph_outer_gpu[outer] = gpu_exact_ftoi;
							graph_outer_qword[outer] = expectation.address_qword;
						}
					}
					u32 exact_product = 0u;
					u32 native_model_product = 0u;
					u32 host_exact_ftoi = 0u;
					const bool references_ok =
						VitaGpuVu::EvaluateGeneratedCgSoftwareF32Reference(
							VitaGpuVu::GeneratedCgF32BinaryOperation::Multiply,
							left, right, output.ftoi_probe_configuration_bits,
							&exact_product) &&
						VitaGpuVu::EvaluateGeneratedCgNativeF32Reference(
							VitaGpuVu::GeneratedCgF32BinaryOperation::Multiply,
							left, right, output.ftoi_probe_configuration_bits,
							&native_model_product) &&
						VitaGpuVu::EvaluateGeneratedCgExactFloatToIntReference(
							product, output.ftoi_probe_scale_offset,
							output.ftoi_probe_configuration_bits, &host_exact_ftoi);
					if (!references_ok)
					{
						reference_failures++;
						continue;
					}
					product_exact_differences += product != exact_product;
					product_native_model_mismatches +=
						product != native_model_product;
					gpu_exact_ftoi_mismatches += gpu_exact_ftoi != host_exact_ftoi;
					gpu_playable_ftoi_mismatches +=
						gpu_playable_ftoi != host_exact_ftoi;
					if (gpu_exact_ftoi != host_exact_ftoi &&
						first_probe_mismatch_invocation ==
							std::numeric_limits<u32>::max())
					{
						first_probe_mismatch_invocation = invocation;
						first_probe_mismatch_product = product;
						first_probe_mismatch_gpu = gpu_exact_ftoi;
						first_probe_mismatch_host = host_exact_ftoi;
					}
					if (gpu_playable_ftoi != host_exact_ftoi &&
						first_playable_mismatch_invocation ==
							std::numeric_limits<u32>::max())
					{
						first_playable_mismatch_invocation = invocation;
						first_playable_mismatch_product = product;
						first_playable_mismatch_gpu = gpu_playable_ftoi;
						first_playable_mismatch_host = host_exact_ftoi;
					}
					store_ftoi_cpu_differences += store_ftoi != cpu_ftoi;
					exact_ftoi_cpu_differences += gpu_exact_ftoi != cpu_ftoi;
					playable_ftoi_cpu_differences +=
						gpu_playable_ftoi != cpu_ftoi;
					store_conversion_only_differences +=
						store_ftoi != cpu_ftoi && gpu_exact_ftoi == cpu_ftoi;
					upstream_differences += gpu_exact_ftoi != cpu_ftoi;
					store_approximation_cancellations +=
						store_ftoi == cpu_ftoi && gpu_exact_ftoi != cpu_ftoi;
					const bool graph_differs =
						probe_expectation.converted != cpu_ftoi;
					graph_ftoi_cpu_differences += graph_differs;
					graph_compact_outer_zero_cpu_differences +=
						probe_expectation.compact_outer_zero_converted != cpu_ftoi;
					graph_outer_memory_zero_cpu_differences +=
						probe_expectation.outer_memory_zero_converted != cpu_ftoi;
					graph_repeated_add_zero_cpu_differences +=
						probe_expectation.repeated_add_zero_converted != cpu_ftoi;
					graph_all_outer_zero_cpu_differences +=
						probe_expectation.all_outer_zero_converted != cpu_ftoi;
					if (graph_differs)
					{
						const u32 outer =
							invocation / output.ftoi_probe_child_count;
						const u32 child =
							invocation % output.ftoi_probe_child_count;
						if (outer < 64u)
							graph_mismatch_outer_mask |= 1ull << outer;
						if (child < 64u)
							graph_mismatch_child_mask |= 1ull << child;
						if (first_graph_mismatch_invocation ==
							std::numeric_limits<u32>::max())
						{
							first_graph_mismatch_invocation = invocation;
							first_graph_mismatch_expected =
								probe_expectation.converted;
							first_graph_mismatch_cpu = cpu_ftoi;
						}
					}
					const bool left_differs = left != probe_expectation.left;
					const bool right_differs = right != probe_expectation.right;
					const bool product_differs =
						product != probe_expectation.product;
					gpu_left_reference_mismatches += left_differs;
					gpu_right_reference_mismatches += right_differs;
					gpu_product_reference_mismatches += product_differs;
					upstream_expression_differences +=
						left_differs || right_differs;
					native_multiply_only_differences +=
						!left_differs && !right_differs && product_differs;
					if ((store_ftoi != cpu_ftoi || gpu_exact_ftoi != cpu_ftoi ||
						 gpu_playable_ftoi != cpu_ftoi ||
						 product != exact_product || left_differs || right_differs ||
						 probe_expectation.converted != cpu_ftoi) &&
						first_difference_invocation ==
							std::numeric_limits<u32>::max())
					{
						first_difference_invocation = invocation;
						first_left = left;
						first_right = right;
						first_product = product;
						first_exact_product = exact_product;
						first_store_ftoi = store_ftoi;
						first_exact_ftoi = gpu_exact_ftoi;
						first_playable_ftoi = gpu_playable_ftoi;
						first_cpu_ftoi = cpu_ftoi;
						first_pair_pc = expectation.pair_pc;
						first_source_vf = expectation.source_vf;
						first_expected_left = probe_expectation.left;
						first_expected_right = probe_expectation.right;
						first_expected_product = probe_expectation.product;
						first_expected_ftoi = probe_expectation.converted;
						first_left_node = probe_expectation.left_node;
						first_right_node = probe_expectation.right_node;
						first_product_node = probe_expectation.product_node;
						first_ftoi_node = probe_expectation.ftoi_node;
					}
				}
			}
			if (probe_shape_ok && first_graph_mismatch_invocation !=
				std::numeric_limits<u32>::max())
			{
				for (u32 invocation = 0u;
					invocation < output.ftoi_probe_count; invocation++)
				{
					const u32 entry =
						invocation * output.stores_per_invocation +
						output.ftoi_probe_store_index;
					if (output.expectations[entry].expected[
							output.ftoi_probe_lane] !=
						first_graph_mismatch_expected)
					{
						continue;
					}
					if (first_graph_value_match_invocation ==
						std::numeric_limits<u32>::max())
					{
						first_graph_value_match_invocation = invocation;
					}
					first_graph_value_match_count++;
				}
			}
			if (probe_shape_ok && reference_failures == 0u &&
				gpu_exact_ftoi_mismatches == 0u &&
				gpu_playable_ftoi_mismatches == 0u)
			{
				numeric_probe_ok = true;
				// Keep physical attestation telemetry below a bounded AAPCS
				// variadic footprint.  A Vita coredump captured the MTGS worker
				// executing this record's read-only format literal after the old
				// 54-argument WriteLn call.  Preserve every diagnostic field, but
				// split the record into sequence-correlated parts so newlib's
				// vfprintf bridge never has to walk that oversized argument frame.
				Console.WriteLn(
					"GPU-VU seq=%llu provider=generated-loop-kernel "
					"ftoi_probe=classified part=summary invocations=%u store=%u lane=%u scale=%u "
					"product_exact_differences=%u "
					"product_native_model_mismatches=%u "
					"store_ftoi_cpu_differences=%u "
					"exact_ftoi_cpu_differences=%u "
					"playable_ftoi_cpu_differences=%u "
					"gpu_playable_ftoi_mismatches=%u store_conversion_only=%u upstream=%u "
					"store_approximation_cancellations=%u product_accepted=0.",
					static_cast<unsigned long long>(output.sequence),
					output.ftoi_probe_count, output.ftoi_probe_store_index,
					output.ftoi_probe_lane, output.ftoi_probe_scale_offset,
					product_exact_differences, product_native_model_mismatches,
					store_ftoi_cpu_differences, exact_ftoi_cpu_differences,
					playable_ftoi_cpu_differences, gpu_playable_ftoi_mismatches,
					store_conversion_only_differences, upstream_differences,
					store_approximation_cancellations);
				Console.WriteLn(
					"GPU-VU seq=%llu provider=generated-loop-kernel "
					"ftoi_probe=classified part=graph graph_ftoi_cpu_differences=%u "
					"graph_compact0_cpu_differences=%u "
					"graph_memory0_cpu_differences=%u "
					"graph_repeat0_cpu_differences=%u "
					"graph_all0_cpu_differences=%u "
					"gpu_left_reference_mismatches=%u "
					"gpu_right_reference_mismatches=%u "
					"gpu_product_reference_mismatches=%u "
					"upstream_expression_differences=%u "
					"native_multiply_only_differences=%u product_accepted=0.",
					static_cast<unsigned long long>(output.sequence),
					graph_ftoi_cpu_differences,
					graph_compact_outer_zero_cpu_differences,
					graph_outer_memory_zero_cpu_differences,
					graph_repeated_add_zero_cpu_differences,
					graph_all_outer_zero_cpu_differences,
					gpu_left_reference_mismatches,
					gpu_right_reference_mismatches,
					gpu_product_reference_mismatches,
					upstream_expression_differences,
					native_multiply_only_differences);
				Console.WriteLn(
					"GPU-VU seq=%llu provider=generated-loop-kernel "
					"ftoi_probe=classified part=first first_invocation=%u "
					"graph_first=%u graph_outer=%u graph_child=%u "
					"graph_expected=%08x graph_cpu=%08x "
					"graph_value_cpu_match=%u graph_value_cpu_matches=%u "
					"graph_outer_mask=%016llx graph_child_mask=%016llx product_accepted=0.",
					static_cast<unsigned long long>(output.sequence),
					first_difference_invocation,
					first_graph_mismatch_invocation,
					first_graph_mismatch_invocation ==
						std::numeric_limits<u32>::max() ? 0u :
						first_graph_mismatch_invocation /
							output.ftoi_probe_child_count,
					first_graph_mismatch_invocation ==
						std::numeric_limits<u32>::max() ? 0u :
						first_graph_mismatch_invocation %
							output.ftoi_probe_child_count,
					first_graph_mismatch_expected, first_graph_mismatch_cpu,
					first_graph_value_match_invocation,
					first_graph_value_match_count,
					static_cast<unsigned long long>(graph_mismatch_outer_mask),
					static_cast<unsigned long long>(graph_mismatch_child_mask));
				Console.WriteLn(
					"GPU-VU seq=%llu provider=generated-loop-kernel "
					"ftoi_probe=classified part=actual "
					"pair_pc=%04x source_vf=%u left=%08x right=%08x "
					"product=%08x exact_product=%08x store_ftoi=%08x "
					"playable_ftoi=%08x exact_ftoi=%08x cpu_ftoi=%08x product_accepted=0.",
					static_cast<unsigned long long>(output.sequence),
					first_pair_pc, first_source_vf, first_left, first_right,
					first_product, first_exact_product, first_store_ftoi,
					first_playable_ftoi, first_exact_ftoi, first_cpu_ftoi);
				Console.WriteLn(
					"GPU-VU seq=%llu provider=generated-loop-kernel "
					"ftoi_probe=classified part=expected expected_left=%08x "
					"expected_right=%08x expected_product=%08x expected_ftoi=%08x "
					"nodes=%u/%u/%u/%u canonical=cpu-shadow "
					"product_accepted=0.",
					static_cast<unsigned long long>(output.sequence),
					first_expected_left,
					first_expected_right, first_expected_product,
					first_expected_ftoi, first_left_node, first_right_node,
					first_product_node, first_ftoi_node);
				if (graph_ftoi_cpu_differences != 0u)
				{
					const u32 reported_outer = std::min<u32>(
						output.ftoi_probe_outer_count,
						static_cast<u32>(graph_outer_expected.size()));
					for (u32 outer = 0u; outer < reported_outer; outer++)
					{
						Console.WriteLn(
							"GPU-VU seq=%llu provider=generated-loop-kernel "
							"ftoi_graph_outer=%u child=0 qword=%u graph=%08x "
							"cpu=%08x gpu=%08x canonical=cpu-shadow "
							"product_accepted=0.",
							static_cast<unsigned long long>(output.sequence), outer,
							graph_outer_qword[outer], graph_outer_expected[outer],
							graph_outer_cpu[outer], graph_outer_gpu[outer]);
					}
				}
			}
			else
			{
				Console.Error(
					"GPU-VU seq=%llu provider=generated-loop-kernel "
					"ftoi_probe=FAILED shape=%u guard=%u invocations=%u "
					"reference_failures=%u gpu_exact_ftoi_mismatches=%u "
					"gpu_playable_ftoi_mismatches=%u "
					"first_probe_mismatch=%u product=%08x gpu=%08x host=%08x "
					"first_playable_mismatch=%u product=%08x gpu=%08x host=%08x "
					"canonical=cpu-shadow product_accepted=0.",
					static_cast<unsigned long long>(output.sequence),
					static_cast<u32>(probe_shape_ok), static_cast<u32>(guard_ok),
					output.ftoi_probe_count, reference_failures,
					gpu_exact_ftoi_mismatches, gpu_playable_ftoi_mismatches,
					first_probe_mismatch_invocation,
					first_probe_mismatch_product, first_probe_mismatch_gpu,
					first_probe_mismatch_host, first_playable_mismatch_invocation,
					first_playable_mismatch_product, first_playable_mismatch_gpu,
					first_playable_mismatch_host);
			}
		}

		const bool shape_ok = guard_ok && payload_data &&
			output.expectations.size() == output.entry_count &&
			output.payload_bytes == output.entry_count * 4u * sizeof(u32);
		u32 compared_lanes = 0u;
		u32 mismatch_lanes = 0u;
		u32 non_output_only_mismatch_lanes = 0u;
		u32 native_policy_unsupported_mismatch_lanes = 0u;
		u32 mismatch_entries = 0u;
		s64 minimum_signed_delta = 0;
		s64 maximum_signed_delta = 0;
		u64 maximum_absolute_signed_delta = 0u;
		u64 maximum_raw_delta = 0u;
		u32 maximum_float_ulp_delta = 0u;
		u32 maximum_fixed_integer_delta = 0u;
		// The first mismatch may be within policy while a later lane rejects
		// the whole canary. Retain peak locations during the existing compare;
		// no extra GPU readback, allocation or product-path work is required.
		u32 peak_float_entry = std::numeric_limits<u32>::max();
		u32 peak_float_lane = 0u;
		u32 peak_fixed_entry = std::numeric_limits<u32>::max();
		u32 peak_fixed_lane = 0u;
		u64 mismatch_kind_mask = 0u;
		u32 mismatch_domain_mask = 0u;
		u32 first_entry = std::numeric_limits<u32>::max();
		u32 first_lane = 0u;
		u32 gpu_word = 0u;
		u32 cpu_word = 0u;
		u32 first_qword = 0u;
		u32 first_store = 0u;
		u32 first_invocation = 0u;
		u32 first_pair_pc = 0u;
		u32 first_source_vf = 0u;
		u32 first_lane_mask = 0u;
		u32 first_value_kind = 0u;
		u32 first_value_domain = 0u;
                u32 profile_mismatch_lanes = 0u;
                u32 profile_mismatch_entries = 0u;
                u64 profile_mismatch_kind_mask = 0u;
                u32 profile_mismatch_domain_mask = 0u;
                u32 first_profile_entry = std::numeric_limits<u32>::max();
                u32 first_profile_lane = 0u;
                u32 first_profile_gpu = 0u;
                u32 first_profile_expected = 0u;
                u32 first_profile_node = 0u;
                u32 first_profile_kind = 0u;
                u32 first_profile_domain = 0u;
                u32 cpu_exact_mismatch_lanes = 0u;
                u32 cpu_playable_mismatch_lanes = 0u;
                struct CpuExactMismatchDetail final {
                  u32 entry = 0u;
                  u32 lane = 0u;
                  u32 qword = 0u;
                  u32 pair_pc = 0u;
                  u32 source_vf = 0u;
                  u32 lane_mask = 0u;
                  u32 value_node = 0u;
                  u32 value_kind = 0u;
                  u32 value_domain = 0u;
                  u32 cpu = 0u;
                  u32 exact = 0u;
                  u32 playable = 0u;
                  u32 gpu = 0u;
                };
                std::array<CpuExactMismatchDetail, 8>
                    cpu_exact_mismatch_details{};
                u32 cpu_exact_mismatch_details_count = 0u;
                if (shape_ok) {
                  const u32 *const words =
                      reinterpret_cast<const u32 *>(payload_data);
                  for (u32 entry = 0u; entry < output.entry_count; entry++) {
                    const VitaGpuVu::PrivateStoreExpectation &expectation =
                        output.expectations[entry];
                    bool entry_mismatch = false;
                    bool profile_entry_mismatch = false;
                    for (u32 lane = 0u; lane < 4u; lane++) {
                      if ((expectation.lane_mask & (0x8u >> lane)) == 0u)
                        continue;
                      compared_lanes++;
                      const u32 observed = words[entry * 4u + lane];
                      const u32 cpu_shadow = expectation.expected[lane];
                      const u32 expected =
                          expectation.exact_profile_expected[lane];
                      const u32 profile_expected =
                          expectation.playable_profile_expected[lane];
                      if (cpu_shadow != expected) {
                        cpu_exact_mismatch_lanes++;
                        if (cpu_exact_mismatch_details_count <
                            cpu_exact_mismatch_details.size()) {
                          CpuExactMismatchDetail &detail =
                              cpu_exact_mismatch_details
                                  [cpu_exact_mismatch_details_count++];
                          detail.entry = entry;
                          detail.lane = lane;
                          detail.qword = expectation.address_qword;
                          detail.pair_pc = expectation.pair_pc;
                          detail.source_vf = expectation.source_vf;
                          detail.lane_mask = expectation.lane_mask;
                          detail.value_node = expectation.value_nodes[lane];
                          detail.value_kind = expectation.value_kinds[lane];
                          detail.value_domain = expectation.value_domains[lane];
                          detail.cpu = cpu_shadow;
                          detail.exact = expected;
                          detail.playable = profile_expected;
                          detail.gpu = observed;
                        }
                      }
                      cpu_playable_mismatch_lanes +=
                          cpu_shadow != profile_expected;
                      if ((expectation.playable_profile_mask &
                           (0x8u >> lane)) == 0u ||
                          observed != profile_expected) {
                        profile_entry_mismatch = true;
                        profile_mismatch_lanes++;
                        const u32 profile_kind = expectation.value_kinds[lane];
                        const u32 profile_domain =
                            expectation.value_domains[lane];
                        if (profile_kind < 64u)
                          profile_mismatch_kind_mask |= 1ull << profile_kind;
                        if (profile_domain < 32u)
                          profile_mismatch_domain_mask |= 1u << profile_domain;
                        if (first_profile_entry ==
                            std::numeric_limits<u32>::max()) {
                          first_profile_entry = entry;
                          first_profile_lane = lane;
                          first_profile_gpu = observed;
                          first_profile_expected = profile_expected;
                          first_profile_node = expectation.value_nodes[lane];
                          first_profile_kind = profile_kind;
                          first_profile_domain = profile_domain;
                        }
                      }
                      if (observed != expected) {
                        entry_mismatch = true;
                        mismatch_lanes++;
                        if ((expectation.output_only_mask & (0x8u >> lane)) ==
                            0u) {
                          non_output_only_mismatch_lanes++;
                        }
                        const s64 signed_delta =
                            static_cast<s64>(static_cast<s32>(observed)) -
                            static_cast<s64>(static_cast<s32>(expected));
                        const u64 absolute_signed_delta =
                            signed_delta < 0 ? static_cast<u64>(-signed_delta)
                                             : static_cast<u64>(signed_delta);
                        const u64 raw_delta =
                            observed >= expected
                                ? static_cast<u64>(observed - expected)
                                : static_cast<u64>(expected - observed);
                        if (mismatch_lanes == 1u) {
                          minimum_signed_delta = signed_delta;
                          maximum_signed_delta = signed_delta;
                        } else {
                          minimum_signed_delta =
                              std::min(minimum_signed_delta, signed_delta);
                          maximum_signed_delta =
                              std::max(maximum_signed_delta, signed_delta);
                        }
                        maximum_absolute_signed_delta =
                            std::max(maximum_absolute_signed_delta,
                                     absolute_signed_delta);
                        maximum_raw_delta =
                            std::max(maximum_raw_delta, raw_delta);
                        const u32 value_kind = expectation.value_kinds[lane];
                        const u32 value_domain =
                            expectation.value_domains[lane];
                        if (value_kind < 64u)
                          mismatch_kind_mask |= 1ull << value_kind;
                        if (value_domain < 32u)
                          mismatch_domain_mask |= 1u << value_domain;
                        const bool native_float_output =
                            value_kind ==
                                static_cast<u32>(
                                    VitaGpuVu::ExpressionKind::Normalize) &&
                            value_domain == static_cast<u32>(
                                                VitaGpuVu::ScalarDomain::Float);
                        const bool native_fixed_output =
                            value_kind ==
                                static_cast<u32>(
                                    VitaGpuVu::ExpressionKind::FloatToInt) &&
                            value_domain ==
                                static_cast<u32>(
                                    VitaGpuVu::ScalarDomain::SignedInt);
                        if (!native_float_output && !native_fixed_output)
                          native_policy_unsupported_mismatch_lanes++;
                        if (native_float_output) {
                          const auto ordered_float = [](u32 bits) {
                            return (bits & 0x80000000u) != 0u
                                       ? ~bits
                                       : bits | 0x80000000u;
                          };
                          const u32 observed_key = ordered_float(observed);
                          const u32 expected_key = ordered_float(expected);
                          const u32 ulp_delta =
                              observed_key >= expected_key
                                  ? observed_key - expected_key
                                  : expected_key - observed_key;
                          if (ulp_delta > maximum_float_ulp_delta) {
                            maximum_float_ulp_delta = ulp_delta;
                            peak_float_entry = entry;
                            peak_float_lane = lane;
                          }
                        } else if (native_fixed_output) {
                          if (absolute_signed_delta > maximum_fixed_integer_delta) {
                            maximum_fixed_integer_delta =
                                static_cast<u32>(absolute_signed_delta);
                            peak_fixed_entry = entry;
                            peak_fixed_lane = lane;
                          }
                        }
                        if (first_entry == std::numeric_limits<u32>::max()) {
                          first_entry = entry;
                          first_lane = lane;
                          gpu_word = observed;
                          cpu_word = expected;
                          first_qword = expectation.address_qword;
                          first_store =
                              output.stores_per_invocation != 0u
                                  ? entry % output.stores_per_invocation
                                  : 0u;
                          first_invocation =
                              output.stores_per_invocation != 0u
                                  ? entry / output.stores_per_invocation
                                  : 0u;
                          first_pair_pc = expectation.pair_pc;
                          first_source_vf = expectation.source_vf;
                          first_lane_mask = expectation.lane_mask;
                          first_value_kind = expectation.value_kinds[lane];
                          first_value_domain = expectation.value_domains[lane];
                        }
                      }
                    }
                    if (entry_mismatch)
                      mismatch_entries++;
                    if (profile_entry_mismatch)
                      profile_mismatch_entries++;
                  }
                }
                for (u32 mismatch = 0u;
                     mismatch < cpu_exact_mismatch_details_count; mismatch++) {
                  const CpuExactMismatchDetail &detail =
                      cpu_exact_mismatch_details[mismatch];
                  Console.Warning(
                      "GPU-VU seq=%llu provider=generated-loop-kernel "
                      "cpu_exact_store_formula=MISMATCH detail=%u/%u "
                      "entry=%u invocation=%u store=%u qword=%u lane=%u "
                      "lane_mask=0x%x pair_pc=%04x source_vf=%u node=%u "
                      "value_kind=%u value_domain=%u cpu=%08x "
                      "exact_formula=%08x "
                      "playable_formula=%08x gpu=%08x product_accepted=0.",
                      static_cast<unsigned long long>(output.sequence),
                      mismatch + 1u, cpu_exact_mismatch_lanes, detail.entry,
                      output.stores_per_invocation != 0u
                          ? detail.entry / output.stores_per_invocation
                          : 0u,
                      output.stores_per_invocation != 0u
                          ? detail.entry % output.stores_per_invocation
                          : 0u,
                      detail.qword, detail.lane, detail.lane_mask,
                      detail.pair_pc, detail.source_vf, detail.value_node,
                      detail.value_kind, detail.value_domain, detail.cpu,
                      detail.exact, detail.playable, detail.gpu);
                }
                if (shape_ok && profile_mismatch_lanes == 0u)
		{
			Console.WriteLn(
				"GPU-VU seq=%llu provider=generated-loop-kernel "
					"playable_profile_compare=passed entries=%u lanes=%u "
					"profile=native-sgx-fmac+exact-conversion "
					"canonical=pairplan-exact cpu_shadow_exact_mismatches=%u "
					"cpu_shadow_playable_mismatches=%u product_accepted=0.",
					static_cast<unsigned long long>(output.sequence),
					output.entry_count, compared_lanes, cpu_exact_mismatch_lanes,
					cpu_playable_mismatch_lanes);
		}
		else
		{
			Console.Error(
				"GPU-VU seq=%llu provider=generated-loop-kernel "
				"playable_profile_compare=MISMATCH entries=%u lanes=%u "
				"mismatch_entries=%u mismatch_lanes=%u guard=%u shape=%u "
				"kind_mask=%016llx domain_mask=%08x "
				"first_entry=%u invocation=%u store=%u lane=%u "
				"node=%u value_kind=%u value_domain=%u "
					"gpu=%08x profile=%08x canonical=pairplan-exact "
					"cpu_shadow_exact_mismatches=%u "
					"cpu_shadow_playable_mismatches=%u "
					"product_accepted=0.",
				static_cast<unsigned long long>(output.sequence),
				output.entry_count, compared_lanes,
				profile_mismatch_entries, profile_mismatch_lanes,
				static_cast<u32>(guard_ok), static_cast<u32>(shape_ok),
				static_cast<unsigned long long>(profile_mismatch_kind_mask),
				profile_mismatch_domain_mask,
				first_profile_entry,
				output.stores_per_invocation != 0u ?
					first_profile_entry / output.stores_per_invocation : 0u,
				output.stores_per_invocation != 0u ?
					first_profile_entry % output.stores_per_invocation : 0u,
					first_profile_lane, first_profile_node, first_profile_kind,
					first_profile_domain, first_profile_gpu,
					first_profile_expected, cpu_exact_mismatch_lanes,
					cpu_playable_mismatch_lanes);
		}
		const bool exact = shape_ok && mismatch_lanes == 0u;
		if (exact)
		{
			Console.WriteLn(
				"GPU-VU seq=%llu provider=generated-loop-kernel "
					"private_store_compare=passed entries=%u lanes=%u guard=1 "
					"canonical=pairplan-exact cpu_shadow_exact_mismatches=%u "
					"cpu_shadow_playable_mismatches=%u product_accepted=0.",
					static_cast<unsigned long long>(output.sequence),
					output.entry_count, compared_lanes, cpu_exact_mismatch_lanes,
					cpu_playable_mismatch_lanes);
		}
		else
		{
			Console.Error(
				"GPU-VU seq=%llu provider=generated-loop-kernel "
				"private_store_compare=MISMATCH entries=%u lanes=%u "
				"mismatch_entries=%u mismatch_lanes=%u guard=%u shape=%u "
				"signed_delta=%lld:%lld abs_max=%llu raw_max=%llu "
				"float_ulp_max=%u kind_mask=%016llx domain_mask=%08x "
				"first_entry=%u invocation=%u store=%u qword=%u lane=%u "
				"lane_mask=0x%x pair_pc=%04x source_vf=%u "
					"value_kind=%u value_domain=%u gpu=%08x exact=%08x "
					"canonical=pairplan-exact cpu_shadow_exact_mismatches=%u "
					"cpu_shadow_playable_mismatches=%u product_accepted=0.",
				static_cast<unsigned long long>(output.sequence),
				output.entry_count, compared_lanes, mismatch_entries,
				mismatch_lanes, static_cast<u32>(guard_ok),
				static_cast<u32>(shape_ok),
				static_cast<long long>(minimum_signed_delta),
				static_cast<long long>(maximum_signed_delta),
				static_cast<unsigned long long>(maximum_absolute_signed_delta),
				static_cast<unsigned long long>(maximum_raw_delta),
				maximum_float_ulp_delta,
				static_cast<unsigned long long>(mismatch_kind_mask),
				mismatch_domain_mask,
				first_entry, first_invocation, first_store, first_qword,
				first_lane, first_lane_mask, first_pair_pc, first_source_vf,
						first_value_kind, first_value_domain, gpu_word, cpu_word,
						cpu_exact_mismatch_lanes, cpu_playable_mismatch_lanes);
			}

			// Private-only evidence, bounded to two records per canary. The
			// exact/profile words are the existing PCSX2-derived store oracle;
			// this diagnostic does not alter numeric admission or canonical state.
			const auto report_peak = [&](const char* metric, u32 delta, u32 limit,
				u32 entry, u32 lane) {
				if (!shape_ok || entry >= output.expectations.size() || lane >= 4u)
					return;
				const auto& expectation = output.expectations[entry];
				const u32 observed = reinterpret_cast<const u32*>(payload_data)[entry * 4u + lane];
				Console.WriteLn(
					"GPU-VU seq=%llu provider=generated-loop-kernel "
					"private_store_peak v=1 key=%016llx%016llx metric=%s "
					"delta=%u limit=%u exceeded=%u entry=%u invocation=%u "
					"store=%u qword=%u lane=%u lane_mask=0x%x pair_pc=%04x "
					"source_vf=%u node=%u value_kind=%u value_domain=%u "
					"gpu=%08x exact=%08x cpu=%08x playable=%08x output_only=%u "
					"canonical=pairplan-exact product_accepted=0.",
					static_cast<unsigned long long>(output.sequence),
					static_cast<unsigned long long>(output.attestation_identity.key.high),
					static_cast<unsigned long long>(output.attestation_identity.key.low),
					metric, delta, limit, static_cast<u32>(delta > limit), entry,
					output.stores_per_invocation ? entry / output.stores_per_invocation : 0u,
					output.stores_per_invocation ? entry % output.stores_per_invocation : 0u,
					expectation.address_qword, lane, expectation.lane_mask,
					expectation.pair_pc, expectation.source_vf, expectation.value_nodes[lane],
					expectation.value_kinds[lane], expectation.value_domains[lane], observed,
					expectation.exact_profile_expected[lane], expectation.expected[lane],
					expectation.playable_profile_expected[lane],
					static_cast<u32>((expectation.output_only_mask & (0x8u >> lane)) != 0u));
			};
			report_peak("float-ulp", maximum_float_ulp_delta,
				VitaGpuVu::NativeSgxOutputOnlyMaximumFloatUlp, peak_float_entry, peak_float_lane);
			report_peak("fixed-integer", maximum_fixed_integer_delta,
				VitaGpuVu::NativeSgxOutputOnlyMaximumFixedIntegerDelta, peak_fixed_entry, peak_fixed_lane);

			const bool architectural_state_exact =
				output.architectural_state_compared &&
				output.architectural_state_exact &&
				output.architectural_state_mismatch_lanes == 0u;
			const bool architectural_state_playable =
				output.architectural_state_compared &&
				output.architectural_state_playable_profile_matches &&
				output.architectural_state_playable_mismatch_lanes == 0u;
			const VitaGpuVu::GeneratedLoopKernelNativeOutputOnlyEvidence
				native_output_evidence{
					mismatch_lanes,
					non_output_only_mismatch_lanes,
					native_policy_unsupported_mismatch_lanes,
					cpu_exact_mismatch_lanes,
					maximum_float_ulp_delta,
					maximum_fixed_integer_delta,
					architectural_state_exact,
					numeric_probe_ok};
			const bool native_output_only =
				VitaGpuVu::GeneratedLoopKernelNativeOutputOnlyCanaryPasses(
					native_output_evidence);
			VitaGpuVu::GeneratedLoopKernelNumericProfile numeric_profile =
				VitaGpuVu::GeneratedLoopKernelNumericProfile::None;
			VitaGpuVu::GeneratedLoopKernelAttestationRejection rejection =
				VitaGpuVu::GeneratedLoopKernelAttestationRejection::None;
			if (!output.attestation_identity.IsValid())
				rejection = VitaGpuVu::GeneratedLoopKernelAttestationRejection::
					InvalidDescriptor;
			else if (!guard_ok)
				rejection = VitaGpuVu::GeneratedLoopKernelAttestationRejection::
					OutputGuard;
			else if (!shape_ok)
				rejection = VitaGpuVu::GeneratedLoopKernelAttestationRejection::
					InvalidDescriptor;
			else if (!architectural_state_exact)
				rejection = VitaGpuVu::GeneratedLoopKernelAttestationRejection::
					ArchitecturalStateMismatch;
			else if (!numeric_probe_ok)
				rejection = VitaGpuVu::GeneratedLoopKernelAttestationRejection::
					NumericProbeMismatch;
			else if (cpu_exact_mismatch_lanes != 0u)
				rejection = VitaGpuVu::GeneratedLoopKernelAttestationRejection::
					ExactCanonicalJournalMismatch;
			else if (exact)
				numeric_profile =
					VitaGpuVu::GeneratedLoopKernelNumericProfile::ExactVu;
			else if (non_output_only_mismatch_lanes != 0u)
				rejection = VitaGpuVu::GeneratedLoopKernelAttestationRejection::
					OutputOnlyClassificationMissing;
			else if (native_output_only)
				numeric_profile = VitaGpuVu::
					GeneratedLoopKernelNumericProfile::NativeSgxOutputOnly;
			else
				rejection = VitaGpuVu::GeneratedLoopKernelAttestationRejection::
					PlayableStoreMismatch;

			const bool attestation_published =
				VitaGpuVu::CompleteGeneratedLoopKernelAttestation(
					output.attestation_identity, numeric_profile, rejection);
			if (attestation_published)
			{
				VitaGpuVu::RefreshGeneratedLoopKernelExecutableState(
					output.attestation_identity.key);
			}
			if (attestation_published && rejection == VitaGpuVu::
					GeneratedLoopKernelAttestationRejection::None)
			{
				// Registration completion cannot discover a later physical
				// partial-batch pass. Pump the bounded source cache from retirement
				// so that a passed batch canary immediately requests its no-write
				// lean successor. CPU MTVU owns the pre-effect compile interval; the
				// canary is never promoted into sustained generated ownership.
				VitaGpuVu::RequestGeneratedLoopKernelBundlePump();
			}
			if (attestation_published && rejection != VitaGpuVu::
					GeneratedLoopKernelAttestationRejection::None &&
				VitaGpuVu::RejectGeneratedLoopKernelBatchExecutable(
					output.attestation_identity.key))
			{
				Console.Warning(
					"GPU-VU: rejected generated batch specialization retired "
					"key=%016llx%016llx; attested base executable resumes.",
					static_cast<unsigned long long>(
						output.attestation_identity.key.high),
					static_cast<unsigned long long>(
						output.attestation_identity.key.low));
			}
			if (attestation_published && rejection == VitaGpuVu::
					GeneratedLoopKernelAttestationRejection::None)
			{
				Console.WriteLn(
					"GPU-VU seq=%llu provider=generated-loop-kernel "
					"attestation=passed profile=%s architectural_state_formula=exact "
					"cpu_shadow_profile=%s state_exact_match=%u "
					"control=exact addresses=exact indices=exact gif=exact "
					"persistent_state=exact canonical_store_journal=pairplan-exact "
					"output_policy=%s product_accepted=0.",
					static_cast<unsigned long long>(output.sequence),
					VitaGpuVu::GeneratedLoopKernelNumericProfileName(
						numeric_profile),
					architectural_state_exact ? "exact" : "playable",
					static_cast<u32>(architectural_state_exact),
					numeric_profile == VitaGpuVu::
						GeneratedLoopKernelNumericProfile::NativeSgxOutputOnly ?
						VitaGpuVu::NativeSgxOutputOnlyPolicyName :
						"pairplan-exact-v1");
			}
			else
			{
				Console.Error(
					"GPU-VU seq=%llu provider=generated-loop-kernel "
					"attestation=%s reason=%s published=%u "
					"architectural_state_exact=%u state_mismatches=%u "
					"cpu_shadow_playable_match=%u playable_state_mismatches=%u "
					"exact_store_mismatches=%u non_output_only_mismatches=%u "
					"native_policy_unsupported=%u float_ulp_max=%u "
					"fixed_delta_max=%u cpu_exact_journal_mismatches=%u "
					"playable_profile_mismatches=%u numeric_probe_ok=%u "
					"product_accepted=0.",
					static_cast<unsigned long long>(output.sequence),
					rejection == VitaGpuVu::
						GeneratedLoopKernelAttestationRejection::None ?
						"publish-failed" : "rejected",
					VitaGpuVu::GeneratedLoopKernelAttestationRejectionName(
						rejection),
					static_cast<u32>(attestation_published),
					static_cast<u32>(architectural_state_exact),
					output.architectural_state_mismatch_lanes,
					static_cast<u32>(architectural_state_playable),
					output.architectural_state_playable_mismatch_lanes,
					mismatch_lanes, non_output_only_mismatch_lanes,
					native_policy_unsupported_mismatch_lanes,
					maximum_float_ulp_delta, maximum_fixed_integer_delta,
					cpu_exact_mismatch_lanes,
					profile_mismatch_lanes,
					static_cast<u32>(numeric_probe_ok));
			}
		}
	outputs->clear();
	// Completion-only generated products have no BUFFER2 publication branch.
	// Their successful GsAccepted -> GpuCompleted transition must still wake
	// CPU1 so it can adopt the private transaction.  Coalesce a whole firmware
	// batch into one semaphore notification instead of one wake per object.
	if (completion_only_cpu1_progress)
		vu1Thread.NotifyUniversalGpuVuProgress();
}

bool GSDeviceGXM::Impl::RetainGpuVuDrawForScene(
	std::unique_ptr<VitaGpuVu::GpuVuDraw> draw)
{
	if (!draw)
		return false;
	std::vector<std::unique_ptr<VitaGpuVu::GpuVuDraw>> draws;
	draws.push_back(std::move(draw));
	return RetainGpuVuDrawsForScene(std::move(draws));
}

bool GSDeviceGXM::Impl::RetainGpuVuDrawsForScene(
	std::vector<std::unique_ptr<VitaGpuVu::GpuVuDraw>> draws)
{
	if (draws.empty() || !scene_active || !gpu_vu_retirements_ready)
		return false;
	const u32 descriptor_count = static_cast<u32>(draws.size());
	for (u32 draw_index = 0u; draw_index < descriptor_count; draw_index++)
	{
		const auto& draw = draws[draw_index];
		if (!draw)
			return false;
		UpdateGpuVuPreNotificationWatchdog(
			VitaGS::GpuVuGxmSubmissionStage::SceneDrawAccounting,
			draw->ordering_sequence);
		VitaGS::GpuVuGxmOwnershipBreadcrumb progress;
		progress.draw_index = draw_index;
		progress.descriptor_count = descriptor_count;
		progress.retained_input_count = gpu_vu_scene_input_retention_count;
		progress.action = VitaGS::GpuVuGxmOwnershipAction::DrawAccounting;
		UpdateGpuVuPreNotificationOwnership(progress);
		VitaGpuVu::RecordGpuVuDrawExecuted(*draw);
		if (!reported_first_gpu_vu_draw)
		{
			reported_first_gpu_vu_draw = true;
			if (draw->precompute_program_count != 0u)
			{
				Console.WriteLn(
					"GPU-VU: first staged generated VU1+TFX batch "
					"encoded (%u vertices and %u primitives per object, "
					"%u producer modules in %u visibility stages); "
					"GPU BUFFER10 retained to scene completion, no readback.",
					draw->vertex_count, draw->primitive_count,
					draw->precompute_program_count,
					draw->precompute_stage_count);
			}
			else if (draw->HasPrivateStoreComparison())
			{
				Console.WriteLn(
					"GPU-VU: first generated VU1+TFX oracle batch encoded "
					"(%u vertices and %u primitives per object); private BUFFER2 "
					"journal retires asynchronously, no routine wait.",
					draw->vertex_count, draw->primitive_count);
			}
			else if (draw->HasGeneratedLoopKernelTransaction())
			{
				Console.WriteLn(
					"GPU-VU: first transactional generated VU1+TFX batch "
					"encoded (%u vertices and %u primitives per object); "
					"private BUFFER2 completion retires asynchronously.",
					draw->vertex_count, draw->primitive_count);
			}
			else
			{
				Console.WriteLn(
					"GPU-VU: first direct VU1+TFX batch encoded "
					"(%u vertices and %u primitives per object); "
					"no producer pass or readback.",
					draw->vertex_count, draw->primitive_count);
			}
		}
	}
	// CanRetainGpuVuInputSlots() proved this bounded union before RenderHW.
	// Transfer one descriptor-owned reference for each unique slot generation;
	// duplicate descriptor references are released normally by their owners.
	// This loop performs no allocation, ring lookup, or refcount acquisition.
	for (u32 draw_index = 0u; draw_index < descriptor_count; draw_index++)
	{
		const auto& draw = draws[draw_index];
		const size_t input_count = draw->InputPayloads().size();
		for (size_t input_index = 0; input_index < input_count; input_index++)
		{
			const VitaGpuVu::RawVifPayloadRef payload =
				draw->InputPayloads()[input_index];
			UpdateGpuVuPreNotificationWatchdog(
				VitaGS::GpuVuGxmSubmissionStage::SceneInputOwnership,
				draw->ordering_sequence);
			VitaGS::GpuVuGxmOwnershipBreadcrumb progress;
			progress.input_owner = payload.owner;
			progress.input_slot = payload.slot;
			progress.input_generation = payload.generation;
			progress.input_offset = payload.offset;
			progress.input_size = payload.size;
			progress.input_references =
				VitaGpuVu::GetRawVifPayloadGenerationReferenceCount(payload);
			progress.draw_index = draw_index;
			progress.input_index = static_cast<u32>(input_index);
			progress.retained_input_count = gpu_vu_scene_input_retention_count;
			progress.descriptor_count = descriptor_count;
			progress.action = VitaGS::GpuVuGxmOwnershipAction::InputInspect;
			UpdateGpuVuPreNotificationOwnership(progress);
			if (ContainsGpuVuInputSlot(gpu_vu_scene_input_retentions,
					gpu_vu_scene_input_retention_count, payload))
			{
				progress.action =
					VitaGS::GpuVuGxmOwnershipAction::InputAlreadyRetained;
				UpdateGpuVuPreNotificationOwnership(progress);
				continue;
			}
			if (!AdoptGpuVuInputSlot(draw.get(), input_index,
					&gpu_vu_scene_input_retentions,
					&gpu_vu_scene_input_retention_count))
			{
				return false;
			}
			progress.retained_input_count = gpu_vu_scene_input_retention_count;
			progress.input_references =
				VitaGpuVu::GetRawVifPayloadGenerationReferenceCount(payload);
			progress.action =
				VitaGS::GpuVuGxmOwnershipAction::InputTransferredToScene;
			UpdateGpuVuPreNotificationOwnership(progress);
		}
	}
	VitaGpuVu::RecordGpuVuObjectsEncoded(draws.size());
	gpu_vu_scene_draw_count += draws.size();
	// GXM has consumed every descriptor field at this point. Preserve one
	// reference per immutable ring slot until the scene's vertex notification,
	// then release the thousands of per-dispatch CPU objects immediately.
	// Release each remaining descriptor-owned input explicitly. This makes a
	// last-reference kernel semaphore signal distinguishable from transaction
	// and descriptor-pool destruction in the durable watchdog record.
	for (u32 draw_index = 0u; draw_index < descriptor_count; draw_index++)
	{
		auto& draw = draws[draw_index];
		const u64 sequence = draw->ordering_sequence;
		const size_t input_count = draw->InputPayloads().size();
		for (size_t input_index = 0u; input_index < input_count; input_index++)
		{
			const VitaGpuVu::RawVifPayloadRef payload =
				draw->InputPayloads()[input_index];
			if (!payload.IsValid())
				continue;
			VitaGpuVu::RawVifPayloadRef released;
			if (!draw->TransferInputPayloadOwnership(input_index, &released))
				return false;
			UpdateGpuVuPreNotificationWatchdog(
				VitaGS::GpuVuGxmSubmissionStage::DescriptorInputRelease,
				sequence);
			VitaGS::GpuVuGxmOwnershipBreadcrumb progress;
			progress.input_owner = payload.owner;
			progress.input_slot = payload.slot;
			progress.input_generation = payload.generation;
			progress.input_offset = payload.offset;
			progress.input_size = payload.size;
			progress.input_references =
				VitaGpuVu::GetRawVifPayloadGenerationReferenceCount(payload);
			progress.draw_index = draw_index;
			progress.input_index = static_cast<u32>(input_index);
			progress.retained_input_count = gpu_vu_scene_input_retention_count;
			progress.descriptor_count = descriptor_count;
			progress.action = VitaGS::GpuVuGxmOwnershipAction::
				DescriptorInputReleaseBefore;
			UpdateGpuVuPreNotificationOwnership(progress);
			VitaGpuVu::ReleaseRawVifPayload(&released);
			progress.input_references =
				VitaGpuVu::GetRawVifPayloadGenerationReferenceCount(payload);
			progress.action = VitaGS::GpuVuGxmOwnershipAction::
				DescriptorInputReleaseAfter;
			UpdateGpuVuPreNotificationOwnership(progress);
		}

		UpdateGpuVuPreNotificationWatchdog(
			VitaGS::GpuVuGxmSubmissionStage::DescriptorRelease, sequence);
		VitaGS::GpuVuGxmOwnershipBreadcrumb progress;
		progress.draw_index = draw_index;
		progress.retained_input_count = gpu_vu_scene_input_retention_count;
		progress.descriptor_count = descriptor_count;
		progress.action =
			VitaGS::GpuVuGxmOwnershipAction::DescriptorDestroyBefore;
		UpdateGpuVuPreNotificationOwnership(progress);
		draw.reset();
		progress.action =
			VitaGS::GpuVuGxmOwnershipAction::DescriptorDestroyAfter;
		UpdateGpuVuPreNotificationOwnership(progress);
	}
	UpdateGpuVuPreNotificationWatchdog(
		VitaGS::GpuVuGxmSubmissionStage::DrawsRetainedForScene,
		gpu_vu_pre_notification_watchdog_sequence);
	VitaGS::GpuVuGxmOwnershipBreadcrumb progress;
	progress.draw_index = descriptor_count;
	progress.retained_input_count = gpu_vu_scene_input_retention_count;
	progress.descriptor_count = descriptor_count;
	progress.action = VitaGS::GpuVuGxmOwnershipAction::Complete;
	UpdateGpuVuPreNotificationOwnership(progress);
	return true;
}

bool GSDeviceGXM::Impl::FailGpuVuRetirementOwner(
	const char* owner, uptr address, u32 required_value,
	u32 observed_value, u64 sequence, u64 elapsed_us)
{
	if (gpu_vu_device_faulted)
		return false;

	gpu_vu_device_faulted = true;
	ready = false;
	present_active = false;
	gpu_vu_retirements_ready = false;
	VitaGpuVu::SetUniversalGpuVuDeviceAvailable(false);
	VitaGpuVu::SetUniversalGpuVuCompactProviderAvailable(false);

	u32 matched_slot = std::numeric_limits<u32>::max();
	u32 submitted_slots = 0;
	u32 failed_transactions = 0;
	u64 retained_draws = gpu_vu_scene_draw_count;
	u64 first_sequence = std::numeric_limits<u64>::max();
	u64 last_sequence = 0;
	const auto fail_outputs = [&](std::vector<GpuVuPrivateStoreOutput>& outputs) {
		for (GpuVuPrivateStoreOutput& output : outputs)
		{
			if (!output.transaction)
				continue;
			first_sequence = std::min(first_sequence, output.sequence);
			last_sequence = std::max(last_sequence, output.sequence);
			output.transaction->MarkFailed(
				VitaGpuVu::GeneratedLoopKernelTransactionFailure::
					GpuRetirementOwnerFailure,
				owner);
			failed_transactions++;
		}
	};
	fail_outputs(gpu_vu_scene_private_store_outputs);
	for (u32 index = 0; index < gpu_vu_retirement_slots.size(); index++)
	{
		GpuVuRetirementSlot& slot = gpu_vu_retirement_slots[index];
		if (!slot.submitted)
			continue;
		submitted_slots++;
		retained_draws += slot.draw_count;
		if (reinterpret_cast<uptr>(slot.notification.address) == address &&
			slot.notification.value == required_value)
		{
			matched_slot = index;
		}
		fail_outputs(slot.private_store_outputs);
	}
	if (sequence != 0)
	{
		first_sequence = std::min(first_sequence, sequence);
		last_sequence = std::max(last_sequence, sequence);
	}
	if (first_sequence == std::numeric_limits<u64>::max())
		first_sequence = 0;

	Console.Error(
		"GPU-VU FATAL retirement_timeout=1 owner=%s address=%08x "
		"required=%u observed=%u elapsed_us=%llu matched_slot=%u "
		"submitted_slots=%u retained_draws=%llu failed_transactions=%u "
		"sequence=%llu..%llu scene=%llu; further GXM submission disabled, "
		"in-flight mappings retained, cpu_replay=0.",
		owner ? owner : "unknown", static_cast<u32>(address), required_value,
		observed_value, static_cast<unsigned long long>(elapsed_us),
		matched_slot, submitted_slots,
		static_cast<unsigned long long>(retained_draws), failed_transactions,
		static_cast<unsigned long long>(first_sequence),
		static_cast<unsigned long long>(last_sequence),
		static_cast<unsigned long long>(scene_serial));
	return false;
}

bool GSDeviceGXM::Impl::WaitForGpuVuRetirementSlot(
	GpuVuRetirementSlot& slot, const char* owner,
	GpuVuHealthRetirementAction action)
{
	u32 slot_index = VitaGpuVu::HealthJournal::InvalidSlot;
	for (u32 index = 0u; index < gpu_vu_retirement_slots.size(); index++)
	{
		if (&gpu_vu_retirement_slots[index] == &slot)
		{
			slot_index = index;
			break;
		}
	}
	if (!slot.submitted || !slot.notification.address)
	{
		PublishGpuVuRetirementHealth(
			GpuVuHealthRetirementStage::Pending, action, slot_index);
		return FailGpuVuRetirementOwner(owner,
			reinterpret_cast<uptr>(slot.notification.address),
			slot.notification.value, 0, 0, 0);
	}
	if (GpuVuNotificationReached(slot.notification))
	{
		PublishGpuVuRetirementHealth(
			GpuVuHealthRetirementStage::Reached, action, slot_index);
		return true;
	}

	VitaGpuVu::RecordGpuVuNotificationWait();
	PublishGpuVuRetirementHealth(
		GpuVuHealthRetirementStage::Pending, action, slot_index);
	const Common::Timer::Value started = slot.submission_wall != 0 ?
		slot.submission_wall : Common::Timer::GetCurrentValue();
	Common::Timer::Value last_progress = Common::Timer::GetCurrentValue();
	u32 last_observed = *slot.notification.address;
	for (;;)
	{
		const u32 observed = *slot.notification.address;
		if (VitaGpuVu::HasCompletedNotificationValue(
				observed, slot.notification.value))
		{
			PublishGpuVuRetirementHealth(
				GpuVuHealthRetirementStage::Reached, action, slot_index);
			return true;
		}
		const Common::Timer::Value now = Common::Timer::GetCurrentValue();
		if (observed != last_observed)
		{
			last_observed = observed;
			last_progress = now;
		}
		if (Common::Timer::ConvertValueToSeconds(now - last_progress) >=
			GPU_VU_RETIREMENT_NO_PROGRESS_TIMEOUT_SECONDS)
		{
			u64 sequence = 0;
			for (const GpuVuPrivateStoreOutput& output :
				slot.private_store_outputs)
			{
				if (output.transaction)
				{
					sequence = output.sequence;
					break;
				}
			}
			const u64 elapsed_us = static_cast<u64>(
				Common::Timer::ConvertValueToSeconds(now - started) * 1000000.0);
			return FailGpuVuRetirementOwner(owner,
				reinterpret_cast<uptr>(slot.notification.address),
				slot.notification.value, observed, sequence, elapsed_us);
		}
		Threading::Sleep(1);
	}
}

bool GSDeviceGXM::Impl::RetireCompletedGpuVuDraws()
{
	if (!gpu_vu_retirements_ready)
		return false;
	PublishGpuVuRetirementHealth(
		GpuVuHealthRetirementStage::Scan,
		GpuVuHealthRetirementAction::CompletedScan);
	bool retired = false;
	bool pending_published = false;
	for (u32 slot_index = 0u;
		slot_index < gpu_vu_retirement_slots.size(); slot_index++)
	{
		GpuVuRetirementSlot& slot = gpu_vu_retirement_slots[slot_index];
		if (!slot.submitted)
			continue;
		if (!GpuVuNotificationReached(slot.notification))
		{
			if (!pending_published)
			{
				PublishGpuVuRetirementHealth(
					GpuVuHealthRetirementStage::Pending,
					GpuVuHealthRetirementAction::CompletedScan,
					slot_index);
				pending_published = true;
			}
			continue;
		}
		PublishGpuVuRetirementHealth(
			GpuVuHealthRetirementStage::Reached,
			GpuVuHealthRetirementAction::CompletedScan, slot_index);
		const u64 count = slot.draw_count;
		RetireGpuVuPrivateStoreOutputs(&slot.private_store_outputs, true);
		PublishGpuVuRetirementHealth(
			GpuVuHealthRetirementStage::OutputsPublished,
			GpuVuHealthRetirementAction::CompletedScan, slot_index);
		ReleaseGpuVuInputSlots(
			&slot.input_retentions, &slot.input_retention_count);
		slot.draw_count = 0;
		slot.submission_wall = 0;
		slot.submission_scene_serial = 0;
		slot.retained_sequence_begin = 0;
		slot.retained_sequence_end = 0;
		slot.retained_sequence_flags = 0;
		slot.batch_allocations.clear();
		slot.submitted = false;
		PublishGpuVuRetirementHealth(
			GpuVuHealthRetirementStage::ResourcesReleasedAndReset,
			GpuVuHealthRetirementAction::CompletedScan, slot_index);
		retired = true;
		if (count != 0)
			VitaGpuVu::RecordGpuVuDrawsRetired(count);
	}
	ArmPendingGpuVuRetirementWake();
	return retired;
}

bool GSDeviceGXM::Impl::ArmPendingGpuVuRetirementWake()
{
	GpuVuRetirementSlot* oldest = nullptr;
	GpuVuRetirementSlot* oldest_without_transaction = nullptr;
	u64 oldest_sequence = std::numeric_limits<u64>::max();
	Common::Timer::Value oldest_submission =
		std::numeric_limits<Common::Timer::Value>::max();
	for (GpuVuRetirementSlot& slot : gpu_vu_retirement_slots)
	{
		if (!slot.submitted || GpuVuNotificationReached(slot.notification))
			continue;
		for (const GpuVuPrivateStoreOutput& output :
			slot.private_store_outputs)
		{
			if (output.transaction && output.sequence < oldest_sequence)
			{
				oldest = &slot;
				oldest_sequence = output.sequence;
			}
		}
		if (slot.submission_wall < oldest_submission)
		{
			oldest_without_transaction = &slot;
			oldest_submission = slot.submission_wall;
		}
	}
	if (!oldest && oldest_without_transaction)
	{
		oldest = oldest_without_transaction;
		oldest_sequence = oldest->submission_scene_serial;
	}
	if (!oldest)
		return true;
	return VitaGS::ArmGpuVuVertexNotification(
		reinterpret_cast<uptr>(oldest->notification.address),
		oldest->notification.value, oldest_sequence);
}

u64 GSDeviceGXM::Impl::OldestPendingGpuVuSequence() const
{
	u64 oldest = std::numeric_limits<u64>::max();
	const auto include_outputs = [&oldest](
		const std::vector<GpuVuPrivateStoreOutput>& outputs) {
		for (const GpuVuPrivateStoreOutput& output : outputs)
		{
			if (output.sequence != 0u)
				oldest = std::min(oldest, output.sequence);
		}
	};
	include_outputs(gpu_vu_scene_private_store_outputs);
	if (gpu_vu_scene_draw_count != 0u &&
		gpu_vu_scene_private_store_outputs.empty())
	{
		oldest = std::min(oldest, scene_serial != 0u ? scene_serial : 1u);
	}
	for (const GpuVuRetirementSlot& slot : gpu_vu_retirement_slots)
	{
		if (!slot.submitted)
			continue;
		const bool has_sequence = std::any_of(
			slot.private_store_outputs.begin(),
			slot.private_store_outputs.end(),
			[](const GpuVuPrivateStoreOutput& output) {
				return output.sequence != 0u;
			});
		include_outputs(slot.private_store_outputs);
		if (!has_sequence && slot.draw_count != 0u)
		{
			oldest = std::min(oldest,
				slot.submission_scene_serial != 0u ?
					slot.submission_scene_serial : 1u);
		}
	}
	return oldest != std::numeric_limits<u64>::max() ? oldest : 0u;
}

bool GSDeviceGXM::Impl::ArmGpuVuPreNotificationWatchdog(u64 sequence)
{
	if (gpu_vu_pre_notification_watchdog_ticket != 0u)
		return true;
#if defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
	const u32 ticket = VitaGS::ArmGpuVuGxmSubmissionWatchdog(
		VitaGS::GpuVuGxmSubmissionStage::DrawEncoding, sequence,
		scene_serial);
	if (ticket == 0u)
		return false;
	gpu_vu_pre_notification_watchdog_ticket = ticket;
	gpu_vu_pre_notification_watchdog_sequence = sequence;
#else
	(void)sequence;
#endif
	return true;
}

void GSDeviceGXM::Impl::UpdateGpuVuPreNotificationWatchdog(
	VitaGS::GpuVuGxmSubmissionStage stage, u64 sequence)
{
	if (gpu_vu_pre_notification_watchdog_ticket == 0u)
		return;
	gpu_vu_pre_notification_watchdog_sequence = sequence;
	VitaGS::UpdateGpuVuGxmSubmissionWatchdog(
		gpu_vu_pre_notification_watchdog_ticket, stage, sequence,
		scene_serial);
}

void GSDeviceGXM::Impl::UpdateGpuVuPreNotificationOwnership(
	const VitaGS::GpuVuGxmOwnershipBreadcrumb& breadcrumb)
{
	if (gpu_vu_pre_notification_watchdog_ticket == 0u)
		return;
	VitaGS::UpdateGpuVuGxmSubmissionWatchdogOwnership(
		gpu_vu_pre_notification_watchdog_ticket, breadcrumb);
}

void GSDeviceGXM::Impl::CompleteGpuVuPreNotificationWatchdog()
{
	if (gpu_vu_pre_notification_watchdog_ticket == 0u)
		return;
	VitaGS::CompleteGpuVuGxmSubmissionWatchdog(
		gpu_vu_pre_notification_watchdog_ticket);
	gpu_vu_pre_notification_watchdog_ticket = 0u;
	gpu_vu_pre_notification_watchdog_sequence = 0u;
}

bool GSDeviceGXM::Impl::FlushGeneratedGpuVuTransaction()
{
	if (!scene_active || gpu_vu_scene_draw_count == 0u)
		return false;
	const u64 scene_draw_count_before_prepare = gpu_vu_scene_draw_count;
	bool has_transaction = false;
	u64 transaction_sequence = std::numeric_limits<u64>::max();
	u64 transaction_sequence_end = 0u;
	u32 transaction_count = 0u;
	for (const GpuVuPrivateStoreOutput& output :
		gpu_vu_scene_private_store_outputs)
	{
		if (!output.transaction)
			continue;
		has_transaction = true;
		transaction_sequence = std::min(transaction_sequence, output.sequence);
		transaction_sequence_end =
			std::max(transaction_sequence_end, output.sequence);
		transaction_count++;
	}
	if (!has_transaction)
		return false;
	if (!ArmGpuVuPreNotificationWatchdog(transaction_sequence))
		return false;

	const SceGxmNotification* vertex_notification = nullptr;
	const SceGxmNotification* fragment_notification = nullptr;
	if (!PrepareGpuVuRetirementNotification(
			&vertex_notification, &fragment_notification) ||
		!vertex_notification)
	{
		return false;
	}

	// Sony's libGXM reference defines this as a vertex-processing-only
	// firmware job and explicitly permits its notification to synchronize
	// dynamic vertex resources.  BUFFER2 is written exclusively by the vertex
	// program, so this is the earliest exact architectural completion point.
	// Waiting for sceGxmEndScene() instead creates a cycle: CPU1 needs the VU
	// successor state to reach VSync, while VSync was the only scene boundary.
	UpdateGpuVuPreNotificationWatchdog(
		VitaGS::GpuVuGxmSubmissionStage::MidSceneFlush,
		transaction_sequence);
	VitaGS::GpuVuGxmCallBreadcrumb call_breadcrumb;
	call_breadcrumb.sequence_end = transaction_sequence_end;
	call_breadcrumb.index_count = transaction_count;
	call_breadcrumb.object_count = static_cast<u32>(
		std::min<u64>(scene_draw_count_before_prepare,
			std::numeric_limits<u32>::max()));
	call_breadcrumb.private_transaction_count = transaction_count;
	const u32 retirement_slot =
		(next_gpu_vu_retirement_slot + gpu_vu_retirement_slots.size() - 1u) %
		gpu_vu_retirement_slots.size();
	call_breadcrumb.retirement_slot = retirement_slot;
	const GpuVuRetirementSlot& submitted_slot =
		gpu_vu_retirement_slots[retirement_slot];
	call_breadcrumb.notification_address = reinterpret_cast<uptr>(
		submitted_slot.notification.address);
	call_breadcrumb.notification_value = submitted_slot.notification.value;
	call_breadcrumb.notification_observed =
		submitted_slot.notification.address ?
			*submitted_slot.notification.address : 0u;
	call_breadcrumb.fragment_notification_address = reinterpret_cast<uptr>(
		submitted_slot.fragment_notification.address);
	call_breadcrumb.fragment_notification_value =
		submitted_slot.fragment_notification.value;
	call_breadcrumb.fragment_notification_observed =
		submitted_slot.fragment_notification.address ?
			*submitted_slot.fragment_notification.address : 0u;
	if (submitted_slot.input_retention_count != 0u)
	{
		const VitaGpuVu::RawVifPayloadRef& input =
			submitted_slot.input_retentions[0];
		call_breadcrumb.input_owner = input.owner;
		call_breadcrumb.input_slot = input.slot;
		call_breadcrumb.input_generation = input.generation;
		call_breadcrumb.input_first_qword = input.offset / 16u;
		call_breadcrumb.input_last_qword =
			(input.offset + input.size - 1u) / 16u;
	}
	for (const GpuVuPrivateStoreOutput& output :
		submitted_slot.private_store_outputs)
	{
		if (!output.HasOutputStorage() || !output.allocation)
			continue;
		call_breadcrumb.output_address = reinterpret_cast<uptr>(
			output.allocation.Data());
		call_breadcrumb.output_bytes = output.TotalPayloadBytes();
		call_breadcrumb.output_allocation_bytes = static_cast<u32>(
			std::min<size_t>(output.allocation.Size(),
				std::numeric_limits<u32>::max()));
		const uptr guard_offset = reinterpret_cast<uptr>(output.GuardData()) -
			reinterpret_cast<uptr>(output.allocation.Data());
		call_breadcrumb.output_guard_offset_bytes = static_cast<u32>(
			std::min<uptr>(guard_offset, std::numeric_limits<u32>::max()));
		call_breadcrumb.index_minimum = output.write_extent.invocation_first;
		call_breadcrumb.index_maximum = output.write_extent.invocation_last;
		call_breadcrumb.output_maximum_write_word =
			output.write_extent.payload_maximum_write_word;
		call_breadcrumb.output_payload_capacity_words =
			output.write_extent.payload_capacity_words;
		call_breadcrumb.output_probe_maximum_write_word =
			output.write_extent.probe_maximum_write_word;
		call_breadcrumb.output_probe_capacity_words =
			output.write_extent.probe_capacity_words;
		break;
	}
	const u32 call_token = VitaGS::BeginGpuVuGxmCall(
		VitaGS::GpuVuGxmCallKind::MidSceneFlush, transaction_sequence,
		scene_serial, call_breadcrumb);
	const int result = sceGxmMidSceneFlush(context,
		SCE_GXM_MIDSCENE_PRESERVE_DEFAULT_UNIFORM_BUFFERS,
		nullptr, vertex_notification);
	VitaGS::CompleteGpuVuGxmCall(call_token, result);
	if (result < 0)
	{
		Fail("flush generated GPU-VU transaction", result);
		return FailGpuVuRetirementOwner("mid-scene-flush-return",
			reinterpret_cast<uptr>(vertex_notification->address),
			vertex_notification->value,
			*vertex_notification->address, transaction_sequence, 0u);
	}
	VitaGpuVu::RecordGpuVuRetirementBatch();
	VitaGpuVu::RecordGeneratedGpuVuFirmwareSubmission();
	if (!VitaGS::ArmGpuVuVertexNotification(
			reinterpret_cast<uptr>(vertex_notification->address),
			vertex_notification->value, transaction_sequence))
	{
		// Admission is normally gated on the process-lifetime bridge. If that
		// invariant is ever violated after commands are submitted, retain the
		// same bounded no-progress gate as every other retirement owner.
		Console.Error(
			"GPU-VU seq=%llu asynchronous transaction notification could not "
			"be armed; entering bounded retirement health gate.",
			static_cast<unsigned long long>(transaction_sequence));
		auto slot = std::find_if(gpu_vu_retirement_slots.begin(),
			gpu_vu_retirement_slots.end(),
			[vertex_notification](const GpuVuRetirementSlot& candidate) {
				return candidate.notification.address ==
					vertex_notification->address &&
					candidate.notification.value ==
					vertex_notification->value;
			});
		if (slot == gpu_vu_retirement_slots.end() ||
			!WaitForGpuVuRetirementSlot(
				*slot, "notification-bridge-arm",
				GpuVuHealthRetirementAction::SubmissionPrepare))
		{
			return false;
		}
		RetireCompletedGpuVuDraws();
	}
	CompleteGpuVuPreNotificationWatchdog();
	static u64 generated_submission_reports = 0u;
	const u64 submission_report = ++generated_submission_reports;
	if (submission_report <= 8u ||
		(submission_report & (submission_report - 1u)) == 0u)
	{
		Console.WriteLn(
			"GPU-VU report=%llu seq=%llu..%llu "
			"transaction_submit=mid-scene-vertex-job "
			"batched_executes=%u firmware_jobs=1 notification=%u "
			"routine_waits=0 scene_end_wait=0.",
			static_cast<unsigned long long>(submission_report),
			static_cast<unsigned long long>(transaction_sequence),
			static_cast<unsigned long long>(transaction_sequence_end),
			transaction_count,
			vertex_notification->value);
	}
	return true;
}

bool GSDeviceGXM::Impl::WaitForGpuVuInputRetirement(
	const VitaGpuVu::RawVifPayloadRef& blocked_generation)
{
	if (!gpu_vu_retirements_ready ||
		!blocked_generation.IsValid())
		return false;

	bool owned = ContainsGpuVuInputSlot(
		gpu_vu_scene_input_retentions,
		gpu_vu_scene_input_retention_count, blocked_generation);
	for (const GpuVuRetirementSlot& slot : gpu_vu_retirement_slots)
	{
		owned |= slot.submitted &&
			ContainsGpuVuInputSlot(slot.input_retentions,
				slot.input_retention_count, blocked_generation);
	}

	// Release every completion already visible, but do not mistake an
	// unrelated slot's completion for progress on the exact input generation
	// the producer is blocked on.
	RetireCompletedGpuVuDraws();

	// Input pressure can stop the producer before it reaches VSync. Submit the
	// already encoded final draws now so their vertex notification can release
	// the immutable source slots. This is not a producer pass and does not wait
	// unless all four input slots are genuinely exhausted.
	if (ContainsGpuVuInputSlot(gpu_vu_scene_input_retentions,
			gpu_vu_scene_input_retention_count, blocked_generation))
	{
		owned = true;
		if (gpu_vu_scene_draw_count == 0 || !EndScene(false))
			return false;
		RetireCompletedGpuVuDraws();
	}

	for (u32 slot_index = 0u;
		slot_index < gpu_vu_retirement_slots.size(); slot_index++)
	{
		GpuVuRetirementSlot& slot = gpu_vu_retirement_slots[slot_index];
		if (!slot.submitted ||
			!ContainsGpuVuInputSlot(slot.input_retentions,
				slot.input_retention_count, blocked_generation))
		{
			continue;
		}

		owned = true;
		VitaGpuVu::RecordGpuVuRetirementRingWait();
		PublishGpuVuRetirementHealth(
			GpuVuHealthRetirementStage::SlotReuse,
			GpuVuHealthRetirementAction::InputRingReuse, slot_index);
		if (!WaitForGpuVuRetirementSlot(slot, "input-ring-reuse",
				GpuVuHealthRetirementAction::InputRingReuse))
			return false;

		const u64 count = slot.draw_count;
		RetireGpuVuPrivateStoreOutputs(&slot.private_store_outputs, true);
		PublishGpuVuRetirementHealth(
			GpuVuHealthRetirementStage::OutputsPublished,
			GpuVuHealthRetirementAction::InputRingReuse, slot_index);
		ReleaseGpuVuInputSlots(
			&slot.input_retentions, &slot.input_retention_count);
		slot.draw_count = 0;
		slot.submission_wall = 0;
		slot.submission_scene_serial = 0;
		slot.retained_sequence_begin = 0;
		slot.retained_sequence_end = 0;
		slot.retained_sequence_flags = 0;
		slot.batch_allocations.clear();
		slot.submitted = false;
		PublishGpuVuRetirementHealth(
			GpuVuHealthRetirementStage::ResourcesReleasedAndReset,
			GpuVuHealthRetirementAction::InputRingReuse, slot_index);
		if (count != 0)
			VitaGpuVu::RecordGpuVuDrawsRetired(count);
	}
	return owned;
}

bool GSDeviceGXM::Impl::PrepareGpuVuRetirementNotification(
	const SceGxmNotification** vertex_notification,
	const SceGxmNotification** fragment_notification)
{
	if (!vertex_notification || !fragment_notification)
		return false;
	*vertex_notification = nullptr;
	*fragment_notification = nullptr;
	// Retire before deciding whether this scene submits new work. A scene which
	// carries no direct descriptors is still the point at which the GPU's
	// completion of an older batch becomes visible, and it is the common case:
	// the guest keeps issuing ordinary GS work after the last direct object.
	// Retiring only on the submitting path stranded the final batch forever,
	// holding its draws, batch bindings and immutable VIF ring slots. That
	// starved raw capture, which emptied the input spans every later dispatch
	// needs, which stopped direct admission, which meant no later scene ever
	// reached the retirement path again.
	if (gpu_vu_retirements_ready)
		RetireCompletedGpuVuDraws();
	if (gpu_vu_scene_draw_count == 0)
	{
		pxAssertRel(gpu_vu_scene_input_retention_count == 0,
			"GPU-VU input retention survived without its descriptors");
		pxAssertRel(gpu_vu_scene_batch_allocations.empty(),
			"GPU-VU batch input survived without its descriptors");
		pxAssertRel(gpu_vu_scene_private_store_outputs.empty(),
			"GPU-VU private output survived without its descriptors");
		return true;
	}
	if (!gpu_vu_retirements_ready)
		return false;

	const u32 slot_index = next_gpu_vu_retirement_slot;
	GpuVuRetirementSlot& slot = gpu_vu_retirement_slots[slot_index];
	PublishGpuVuRetirementHealth(
		GpuVuHealthRetirementStage::SlotPrepare,
		GpuVuHealthRetirementAction::SubmissionPrepare, slot_index);
	if (slot.submitted)
	{
		// Four newer scene batches have consumed the other slots. Reuse remains
		// bounded by the same no-progress health gate as input-ring pressure.
		VitaGpuVu::RecordGpuVuRetirementRingWait();
		PublishGpuVuRetirementHealth(
			GpuVuHealthRetirementStage::SlotReuse,
			GpuVuHealthRetirementAction::RetirementSlotReuse, slot_index);
		if (!WaitForGpuVuRetirementSlot(slot, "retirement-slot-reuse",
				GpuVuHealthRetirementAction::RetirementSlotReuse))
			return false;
		const u64 count = slot.draw_count;
		RetireGpuVuPrivateStoreOutputs(&slot.private_store_outputs, true);
		PublishGpuVuRetirementHealth(
			GpuVuHealthRetirementStage::OutputsPublished,
			GpuVuHealthRetirementAction::RetirementSlotReuse, slot_index);
		ReleaseGpuVuInputSlots(
			&slot.input_retentions, &slot.input_retention_count);
		slot.draw_count = 0;
		slot.submission_wall = 0;
		slot.submission_scene_serial = 0;
		slot.retained_sequence_begin = 0;
		slot.retained_sequence_end = 0;
		slot.retained_sequence_flags = 0;
		slot.batch_allocations.clear();
		slot.submitted = false;
		PublishGpuVuRetirementHealth(
			GpuVuHealthRetirementStage::ResourcesReleasedAndReset,
			GpuVuHealthRetirementAction::RetirementSlotReuse, slot_index);
		if (count != 0)
			VitaGpuVu::RecordGpuVuDrawsRetired(count);
	}

	u32 value = slot.notification.value + 1;
	if (value == 0)
		value = 1;
	slot.notification.value = value;
	slot.fragment_notification.value = value;
	MoveGpuVuInputSlots(&slot.input_retentions,
		&slot.input_retention_count, &gpu_vu_scene_input_retentions,
		&gpu_vu_scene_input_retention_count);
	slot.draw_count = gpu_vu_scene_draw_count;
	slot.submission_wall = Common::Timer::GetCurrentValue();
	slot.submission_scene_serial = scene_serial;
	gpu_vu_scene_draw_count = 0;
	// Both sides own equal process-lifetime reservations.  Swapping transfers
	// the live records while returning the retired slot's empty storage to the
	// next scene; move assignment would discard that storage and force another
	// newlib allocation on every submission.
	slot.batch_allocations.swap(gpu_vu_scene_batch_allocations);
	slot.private_store_outputs.swap(gpu_vu_scene_private_store_outputs);
	slot.retained_sequence_begin = 0u;
	slot.retained_sequence_end = 0u;
	slot.retained_sequence_flags = 0u;
	u64 sequence_begin = std::numeric_limits<u64>::max();
	for (const GpuVuPrivateStoreOutput& output : slot.private_store_outputs)
	{
		if (output.sequence != 0u)
		{
			sequence_begin = std::min(sequence_begin, output.sequence);
			slot.retained_sequence_end = std::max(
				slot.retained_sequence_end, output.sequence);
		}
		if (output.transaction)
			slot.retained_sequence_flags |=
				GPU_VU_HEALTH_RETIREMENT_HAS_TRANSACTION;
		if (output.completion_only)
			slot.retained_sequence_flags |=
				GPU_VU_HEALTH_RETIREMENT_HAS_COMPLETION_ONLY;
	}
	slot.retained_sequence_begin =
		sequence_begin != std::numeric_limits<u64>::max() ? sequence_begin : 0u;
	slot.submitted = true;
	PublishGpuVuRetirementHealth(
		GpuVuHealthRetirementStage::SlotPrepared,
		GpuVuHealthRetirementAction::SubmissionPrepare, slot_index);
	VitaGS::UpdateGpuVuGxmSubmissionWatchdogRetirementSlot(
		gpu_vu_pre_notification_watchdog_ticket,
		next_gpu_vu_retirement_slot);
	next_gpu_vu_retirement_slot =
		(next_gpu_vu_retirement_slot + 1) %
		gpu_vu_retirement_slots.size();
	*vertex_notification = &slot.notification;
	*fragment_notification = &slot.fragment_notification;
	return true;
}

void GSDeviceGXM::Impl::ReleaseAllGpuVuDraws()
{
	u64 count = gpu_vu_scene_draw_count;
	gpu_vu_scene_draw_count = 0;
	ReleaseGpuVuInputSlots(&gpu_vu_scene_input_retentions,
		&gpu_vu_scene_input_retention_count);
	gpu_vu_scene_batch_allocations.clear();
	RetireGpuVuPrivateStoreOutputs(
		&gpu_vu_scene_private_store_outputs, true);
	for (u32 slot_index = 0u;
		slot_index < gpu_vu_retirement_slots.size(); slot_index++)
	{
		GpuVuRetirementSlot& slot = gpu_vu_retirement_slots[slot_index];
		count += slot.draw_count;
		slot.draw_count = 0;
		slot.submission_wall = 0;
		slot.submission_scene_serial = 0;
		ReleaseGpuVuInputSlots(
			&slot.input_retentions, &slot.input_retention_count);
		slot.batch_allocations.clear();
		RetireGpuVuPrivateStoreOutputs(&slot.private_store_outputs, true);
		slot.retained_sequence_begin = 0;
		slot.retained_sequence_end = 0;
		slot.retained_sequence_flags = 0;
		slot.submitted = false;
		PublishGpuVuRetirementHealth(
			GpuVuHealthRetirementStage::ResourcesReleasedAndReset,
			GpuVuHealthRetirementAction::ReleaseAll, slot_index);
	}
	if (count != 0)
		VitaGpuVu::RecordGpuVuDrawsRetired(count);
}

void GSDeviceGXM::Impl::AccumulateGpuVuGeneratedSceneManifest(
	VitaGS::GpuVuGxmCallKind kind, u64 sequence,
	const VitaGS::GpuVuGxmCallBreadcrumb& breadcrumb)
{
	GpuVuGeneratedSceneManifest& manifest = gpu_vu_generated_scene_manifest;
	if (manifest.scene_serial != scene_serial)
	{
		manifest = {};
		manifest.scene_serial = scene_serial;
	}
	const u64 sequence_end = breadcrumb.sequence_end != 0u ?
		breadcrumb.sequence_end : sequence;
	manifest.sequence_begin = manifest.sequence_begin == 0u ? sequence :
		std::min(manifest.sequence_begin, sequence);
	manifest.sequence_end = std::max(manifest.sequence_end, sequence_end);
	if (manifest.draw_call_count == 0u)
	{
		manifest.program_key_high = breadcrumb.program_key_high;
		manifest.program_key_low = breadcrumb.program_key_low;
		manifest.program_abi = breadcrumb.program_abi;
		manifest.vertex_program = breadcrumb.vertex_program;
		manifest.fragment_program = breadcrumb.fragment_program;
		manifest.source_texture_object = breadcrumb.source_texture_object;
		manifest.source_texture_descriptor = breadcrumb.source_texture_descriptor;
		manifest.source_texture_data = breadcrumb.source_texture_data;
		manifest.ps_selector_low = breadcrumb.ps_selector_low;
		manifest.ps_selector_high = breadcrumb.ps_selector_high;
		manifest.primitive_type = breadcrumb.primitive_type;
		manifest.topology = breadcrumb.topology;
		manifest.sampler_key = breadcrumb.sampler_key;
		manifest.blend_key = breadcrumb.blend_key;
		manifest.color_mask_key = breadcrumb.color_mask_key;
		manifest.depth_key = breadcrumb.depth_key;
		manifest.texture_type = breadcrumb.texture_type;
		manifest.texture_format = breadcrumb.texture_format;
		manifest.texture_width = breadcrumb.texture_width;
		manifest.texture_height = breadcrumb.texture_height;
		manifest.texture_stride = breadcrumb.texture_stride;
		manifest.texture_mipmap_count = breadcrumb.texture_mipmap_count;
		manifest.texture_sampler_state = breadcrumb.texture_sampler_state;
	}
	else if (manifest.program_key_high != breadcrumb.program_key_high ||
		manifest.program_key_low != breadcrumb.program_key_low ||
		manifest.program_abi != breadcrumb.program_abi)
	{
		manifest.flags |= 1u << 0u;
	}
	if (manifest.draw_call_count != 0u &&
		(manifest.vertex_program != breadcrumb.vertex_program ||
			manifest.fragment_program != breadcrumb.fragment_program ||
			manifest.source_texture_object != breadcrumb.source_texture_object ||
			manifest.source_texture_descriptor !=
				breadcrumb.source_texture_descriptor ||
			manifest.source_texture_data != breadcrumb.source_texture_data ||
			manifest.ps_selector_low != breadcrumb.ps_selector_low ||
			manifest.ps_selector_high != breadcrumb.ps_selector_high ||
			manifest.primitive_type != breadcrumb.primitive_type ||
			manifest.topology != breadcrumb.topology ||
			manifest.sampler_key != breadcrumb.sampler_key ||
			manifest.blend_key != breadcrumb.blend_key ||
			manifest.color_mask_key != breadcrumb.color_mask_key ||
			manifest.depth_key != breadcrumb.depth_key ||
			manifest.texture_type != breadcrumb.texture_type ||
			manifest.texture_format != breadcrumb.texture_format ||
			manifest.texture_width != breadcrumb.texture_width ||
			manifest.texture_height != breadcrumb.texture_height ||
			manifest.texture_stride != breadcrumb.texture_stride ||
			manifest.texture_mipmap_count != breadcrumb.texture_mipmap_count ||
			manifest.texture_sampler_state != breadcrumb.texture_sampler_state))
	{
		manifest.flags |= 1u << 6u;
	}
	if (breadcrumb.input_owner != 0u)
	{
		if (manifest.input_owner == 0u)
		{
			manifest.input_owner = breadcrumb.input_owner;
			manifest.input_slot = breadcrumb.input_slot;
			manifest.input_generation = breadcrumb.input_generation;
			manifest.input_first_qword = breadcrumb.input_first_qword;
			manifest.input_last_qword = breadcrumb.input_last_qword;
		}
		else
		{
			if (manifest.input_owner != breadcrumb.input_owner ||
				manifest.input_slot != breadcrumb.input_slot ||
				manifest.input_generation != breadcrumb.input_generation)
			{
				manifest.flags |= 1u << 1u;
			}
			manifest.input_first_qword = std::min(
				manifest.input_first_qword, breadcrumb.input_first_qword);
			manifest.input_last_qword = std::max(
				manifest.input_last_qword, breadcrumb.input_last_qword);
		}
	}
	if (breadcrumb.output_address != 0u &&
		breadcrumb.output_allocation_bytes != 0u)
	{
		const uptr allocation_end = breadcrumb.output_address +
			breadcrumb.output_allocation_bytes;
		if (allocation_end < breadcrumb.output_address)
		{
			manifest.flags |= 1u << 2u;
		}
		else
		{
			manifest.output_first = manifest.output_first == 0u ?
				breadcrumb.output_address :
				std::min(manifest.output_first, breadcrumb.output_address);
			manifest.output_last = std::max(
				manifest.output_last, allocation_end - 1u);
		}
		const u64 allocation_bytes = static_cast<u64>(
			manifest.output_allocation_bytes) +
			breadcrumb.output_allocation_bytes;
		manifest.output_allocation_bytes = static_cast<u32>(std::min<u64>(
			allocation_bytes, std::numeric_limits<u32>::max()));
		manifest.output_maximum_write_word = std::max(
			manifest.output_maximum_write_word,
			breadcrumb.output_maximum_write_word);
		manifest.output_payload_capacity_words = std::max(
			manifest.output_payload_capacity_words,
			breadcrumb.output_payload_capacity_words);
		manifest.output_probe_maximum_write_word = std::max(
			manifest.output_probe_maximum_write_word,
			breadcrumb.output_probe_maximum_write_word);
		manifest.output_probe_capacity_words = std::max(
			manifest.output_probe_capacity_words,
			breadcrumb.output_probe_capacity_words);
	}
	if (manifest.draw_call_count == 0u)
	{
		manifest.index_minimum = breadcrumb.index_minimum;
		manifest.index_maximum = breadcrumb.index_maximum;
	}
	else
	{
		manifest.index_minimum = std::min(
			manifest.index_minimum, breadcrumb.index_minimum);
		manifest.index_maximum = std::max(
			manifest.index_maximum, breadcrumb.index_maximum);
	}
	manifest.draw_call_count++;
	manifest.index_count = static_cast<u32>(std::min<u64>(
		static_cast<u64>(manifest.index_count) + breadcrumb.index_count,
		std::numeric_limits<u32>::max()));
	manifest.object_count = static_cast<u32>(std::min<u64>(
		static_cast<u64>(manifest.object_count) + breadcrumb.object_count,
		std::numeric_limits<u32>::max()));
	manifest.private_transaction_count = static_cast<u32>(std::min<u64>(
		static_cast<u64>(manifest.private_transaction_count) +
			breadcrumb.private_transaction_count,
		std::numeric_limits<u32>::max()));
	if (kind == VitaGS::GpuVuGxmCallKind::GeneratedPrecomputeDraw)
		manifest.flags |= 1u << 3u;
	else if (kind == VitaGS::GpuVuGxmCallKind::GeneratedBatchDraw)
		manifest.flags |= 1u << 4u;
	else if (kind == VitaGS::GpuVuGxmCallKind::GeneratedDirectDraw)
		manifest.flags |= 1u << 5u;
}

bool GSDeviceGXM::Impl::WaitForGeneratedSceneFragmentCompletion(
	const SceGxmNotification& notification,
	const GpuVuGeneratedSceneManifest& manifest)
{
	if (!notification.address || notification.value == 0u)
	{
		return FailGpuVuRetirementOwner("generated-fragment-isolation-invalid",
			reinterpret_cast<uptr>(notification.address), notification.value, 0u,
			manifest.sequence_begin, 0u);
	}
	const Common::Timer::Value started = Common::Timer::GetCurrentValue();
	Common::Timer::Value last_report = started;
	Console.WriteLn(
		"GPU-VU fragment_isolation=wait scene=%llu sequence=%llu..%llu "
		"target=%u observed=%u calls=%u indices=%u:%u:%u objects=%u private=%u "
		"program=%016llx%016llx abi=%u input=%08x:%u:%u:%u:%u "
		"output=%08x:%08x:%u payload=%u/%u probe=%u/%u flags=%08x.",
		static_cast<unsigned long long>(manifest.scene_serial),
		static_cast<unsigned long long>(manifest.sequence_begin),
		static_cast<unsigned long long>(manifest.sequence_end),
		notification.value, *notification.address, manifest.draw_call_count,
		manifest.index_count, manifest.index_minimum, manifest.index_maximum,
		manifest.object_count,
		manifest.private_transaction_count,
		static_cast<unsigned long long>(manifest.program_key_high),
		static_cast<unsigned long long>(manifest.program_key_low),
		manifest.program_abi, static_cast<u32>(manifest.input_owner),
		manifest.input_slot, manifest.input_generation,
		manifest.input_first_qword, manifest.input_last_qword,
		static_cast<u32>(manifest.output_first),
		static_cast<u32>(manifest.output_last),
		manifest.output_allocation_bytes,
		manifest.output_maximum_write_word,
		manifest.output_payload_capacity_words,
		manifest.output_probe_maximum_write_word,
		manifest.output_probe_capacity_words, manifest.flags);
	for (;;)
	{
		const u32 observed = *notification.address;
		if (VitaGpuVu::HasCompletedNotificationValue(
				observed, notification.value))
		{
			const Common::Timer::Value completed = Common::Timer::GetCurrentValue();
			const u64 elapsed_us = completed >= started ?
				static_cast<u64>(Common::Timer::ConvertValueToSeconds(
					completed - started) * 1000000.0) : 0u;
			if (elapsed_us >= 250000u)
			{
				// Preserve a slow-but-returning fragment boundary immediately. It is
				// otherwise indistinguishable from the later call which absorbs GXM
				// backpressure if the title is suspended or closed soon afterward.
				Console.Warning(
					"GPU-VU fragment_isolation=complete scene=%llu "
					"sequence=%llu..%llu target=%u observed=%u elapsed_us=%llu slow=1.",
					static_cast<unsigned long long>(manifest.scene_serial),
					static_cast<unsigned long long>(manifest.sequence_begin),
					static_cast<unsigned long long>(manifest.sequence_end),
					notification.value, observed,
					static_cast<unsigned long long>(elapsed_us));
			}
			else
			{
				Console.WriteLn(
					"GPU-VU fragment_isolation=complete scene=%llu "
					"sequence=%llu..%llu target=%u observed=%u elapsed_us=%llu slow=0.",
					static_cast<unsigned long long>(manifest.scene_serial),
					static_cast<unsigned long long>(manifest.sequence_begin),
					static_cast<unsigned long long>(manifest.sequence_end),
					notification.value, observed,
					static_cast<unsigned long long>(elapsed_us));
			}
			return true;
		}
		const Common::Timer::Value now = Common::Timer::GetCurrentValue();
		const double elapsed = Common::Timer::ConvertValueToSeconds(now - started);
		if (elapsed >= GPU_VU_RETIREMENT_NO_PROGRESS_TIMEOUT_SECONDS)
		{
			const u64 elapsed_us = static_cast<u64>(elapsed * 1000000.0);
			Console.Error(
				"GPU-VU FATAL fragment_completion_timeout=1 scene=%llu "
				"sequence=%llu..%llu target=%u observed=%u calls=%u indices=%u "
				"objects=%u private=%u flags=%08x; no later GXM command encoded.",
				static_cast<unsigned long long>(manifest.scene_serial),
				static_cast<unsigned long long>(manifest.sequence_begin),
				static_cast<unsigned long long>(manifest.sequence_end),
				notification.value, observed, manifest.draw_call_count,
				manifest.index_count, manifest.object_count,
				manifest.private_transaction_count, manifest.flags);
			return FailGpuVuRetirementOwner("generated-fragment-isolation",
				reinterpret_cast<uptr>(notification.address), notification.value,
				observed, manifest.sequence_begin, elapsed_us);
		}
		if (Common::Timer::ConvertValueToSeconds(now - last_report) >= 0.25)
		{
			last_report = now;
			Console.Warning(
				"GPU-VU fragment_isolation=pending scene=%llu sequence=%llu..%llu "
				"target=%u observed=%u elapsed_us=%llu.",
				static_cast<unsigned long long>(manifest.scene_serial),
				static_cast<unsigned long long>(manifest.sequence_begin),
				static_cast<unsigned long long>(manifest.sequence_end),
				notification.value, observed,
				static_cast<unsigned long long>(elapsed * 1000000.0));
		}
		Threading::Sleep(1);
	}
}

bool GSDeviceGXM::Impl::EndScene(bool finish)
{
	if (!scene_active)
		return true;
	// Submit transactional generated vertex work at the vertex-only boundary
	// before ending the ordinary scene. Sony defines sceGxmEndScene() as the
	// point which creates and submits the complete firmware job; the r237 hard
	// hang entered that call with 44 generated private owners and never returned.
	// Using the already-established MidSceneFlush path gives those owners their
	// exact vertex-completion notification first and makes a later non-return
	// unambiguous: MidSceneFlush identifies generated command processing, while
	// an EndScene with no generated owners identifies the remaining scene/context.
	// Capture the manifest before FlushGeneratedGpuVuTransaction() transfers
	// and zeroes the scene-owned vectors. r239 captured these fields afterward,
	// making a generated EndScene falsely report zero objects/transactions.
	const u64 scene_draw_count_before_flush = gpu_vu_scene_draw_count;
	u32 generated_transaction_count = 0u;
	u64 generated_transaction_begin = std::numeric_limits<u64>::max();
	u64 generated_transaction_end = 0u;
	for (const GpuVuPrivateStoreOutput& output :
		gpu_vu_scene_private_store_outputs)
	{
		if (!output.transaction)
			continue;
		generated_transaction_count++;
		generated_transaction_begin = std::min(
			generated_transaction_begin, output.sequence);
		generated_transaction_end = std::max(
			generated_transaction_end, output.sequence);
	}
	const bool has_generated_transaction = generated_transaction_count != 0u;
	if (has_generated_transaction)
		gpu_vu_last_generated_scene_serial = scene_serial;
	if (has_generated_transaction && !FlushGeneratedGpuVuTransaction())
		return false;
	const SceGxmNotification* vertex_notification = nullptr;
	const SceGxmNotification* fragment_notification = nullptr;
	if (!PrepareGpuVuRetirementNotification(
			&vertex_notification, &fragment_notification))
		return false;
	bool uses_fragment_progress_notification = false;
	u32 fragment_progress_target = 0u;
	if (!fragment_notification && gpu_vu_last_generated_sequence != 0u &&
		gpu_vu_fragment_progress_notification.address)
	{
		const u32 observed =
			*gpu_vu_fragment_progress_notification.address;
		if (observed != gpu_vu_fragment_progress_last_completed &&
			VitaGpuVu::HasCompletedNotificationValue(
				observed, gpu_vu_fragment_progress_last_completed))
		{
			gpu_vu_fragment_progress_last_completed = observed;
			const GpuVuFragmentProgressRecord& completed =
				gpu_vu_fragment_progress_history[
					observed % gpu_vu_fragment_progress_history.size()];
			if (completed.value == observed)
			{
				gpu_vu_fragment_progress_completed_scene =
					completed.scene_serial;
				gpu_vu_fragment_progress_completed_generated_scene =
					completed.last_generated_scene;
				gpu_vu_fragment_progress_completed_generated_sequence =
					completed.last_generated_sequence;
			}
		}

		fragment_progress_target =
			gpu_vu_fragment_progress_last_submitted + 1u;
		if (fragment_progress_target == 0u)
			fragment_progress_target = 1u;
		GpuVuFragmentProgressRecord& record =
			gpu_vu_fragment_progress_history[
				fragment_progress_target %
				gpu_vu_fragment_progress_history.size()];
		if (record.value != 0u &&
			!VitaGpuVu::HasCompletedNotificationValue(observed, record.value))
		{
			const u64 report = ++gpu_vu_fragment_progress_history_overruns;
			if (report <= 8u || (report & (report - 1u)) == 0u)
			{
				Console.Warning(
					"GPU-VU fragment progress history overrun report=%llu "
					"target=%u observed=%u overwritten=%u scene=%llu; "
					"fragment backlog exceeded %u scenes.",
					static_cast<unsigned long long>(report),
					fragment_progress_target, observed, record.value,
					static_cast<unsigned long long>(scene_serial),
					static_cast<u32>(
						gpu_vu_fragment_progress_history.size()));
			}
		}
		record.value = fragment_progress_target;
		record.scene_serial = scene_serial;
		record.last_generated_scene =
			gpu_vu_last_generated_scene_serial;
		record.last_generated_sequence = gpu_vu_last_generated_sequence;
		record.generated_manifest = has_generated_transaction ?
			gpu_vu_generated_scene_manifest : GpuVuGeneratedSceneManifest{};
		gpu_vu_fragment_progress_notification.value =
			fragment_progress_target;
		fragment_notification = &gpu_vu_fragment_progress_notification;
		uses_fragment_progress_notification = true;
	}
	if (vertex_notification)
	{
		const u64 watchdog_sequence =
			generated_transaction_count != 0u ?
				generated_transaction_begin :
				gpu_vu_pre_notification_watchdog_sequence;
		if (!ArmGpuVuPreNotificationWatchdog(watchdog_sequence))
			return false;
		UpdateGpuVuPreNotificationWatchdog(
			VitaGS::GpuVuGxmSubmissionStage::EndScene,
			watchdog_sequence);
	}
	VitaGS::GpuVuGxmCallBreadcrumb call_breadcrumb;
	call_breadcrumb.sequence_end = generated_transaction_end;
	call_breadcrumb.object_count = static_cast<u32>(
		std::min<u64>(scene_draw_count_before_flush,
			std::numeric_limits<u32>::max()));
	call_breadcrumb.private_transaction_count = generated_transaction_count;
	if (has_generated_transaction &&
		gpu_vu_generated_scene_manifest.scene_serial == scene_serial)
	{
		const GpuVuGeneratedSceneManifest& manifest =
			gpu_vu_generated_scene_manifest;
		call_breadcrumb.program_key_high = manifest.program_key_high;
		call_breadcrumb.program_key_low = manifest.program_key_low;
		call_breadcrumb.program_abi = manifest.program_abi;
		call_breadcrumb.input_owner = manifest.input_owner;
		call_breadcrumb.input_slot = manifest.input_slot;
		call_breadcrumb.input_generation = manifest.input_generation;
		call_breadcrumb.input_first_qword = manifest.input_first_qword;
		call_breadcrumb.input_last_qword = manifest.input_last_qword;
		call_breadcrumb.index_count = manifest.index_count;
		call_breadcrumb.index_minimum = manifest.index_minimum;
		call_breadcrumb.index_maximum = manifest.index_maximum;
		call_breadcrumb.output_address = manifest.output_first;
		if (manifest.output_last >= manifest.output_first &&
			manifest.output_first != 0u)
		{
			call_breadcrumb.output_bytes = static_cast<u32>(std::min<uptr>(
				manifest.output_last - manifest.output_first + 1u,
				std::numeric_limits<u32>::max()));
		}
		call_breadcrumb.output_allocation_bytes =
			manifest.output_allocation_bytes;
		call_breadcrumb.output_maximum_write_word =
			manifest.output_maximum_write_word;
		call_breadcrumb.output_payload_capacity_words =
			manifest.output_payload_capacity_words;
		call_breadcrumb.output_probe_maximum_write_word =
			manifest.output_probe_maximum_write_word;
		call_breadcrumb.output_probe_capacity_words =
			manifest.output_probe_capacity_words;
		call_breadcrumb.vertex_program = manifest.vertex_program;
		call_breadcrumb.fragment_program = manifest.fragment_program;
		call_breadcrumb.source_texture_object = manifest.source_texture_object;
		call_breadcrumb.source_texture_descriptor =
			manifest.source_texture_descriptor;
		call_breadcrumb.source_texture_data = manifest.source_texture_data;
		call_breadcrumb.ps_selector_low = manifest.ps_selector_low;
		call_breadcrumb.ps_selector_high = manifest.ps_selector_high;
		call_breadcrumb.primitive_type = manifest.primitive_type;
		call_breadcrumb.topology = manifest.topology;
		call_breadcrumb.sampler_key = manifest.sampler_key;
		call_breadcrumb.blend_key = manifest.blend_key;
		call_breadcrumb.color_mask_key = manifest.color_mask_key;
		call_breadcrumb.depth_key = manifest.depth_key;
		call_breadcrumb.texture_type = manifest.texture_type;
		call_breadcrumb.texture_format = manifest.texture_format;
		call_breadcrumb.texture_width = manifest.texture_width;
		call_breadcrumb.texture_height = manifest.texture_height;
		call_breadcrumb.texture_stride = manifest.texture_stride;
		call_breadcrumb.texture_mipmap_count = manifest.texture_mipmap_count;
		call_breadcrumb.texture_sampler_state = manifest.texture_sampler_state;
	}
	call_breadcrumb.last_generated_scene =
		gpu_vu_last_generated_scene_serial;
	call_breadcrumb.last_generated_sequence =
		gpu_vu_last_generated_sequence;
	call_breadcrumb.fragment_completed_value =
		gpu_vu_fragment_progress_last_completed;
	call_breadcrumb.fragment_last_submitted_value =
		gpu_vu_fragment_progress_last_submitted;
	call_breadcrumb.fragment_completed_scene =
		gpu_vu_fragment_progress_completed_scene;
	call_breadcrumb.fragment_completed_generated_scene =
		gpu_vu_fragment_progress_completed_generated_scene;
	call_breadcrumb.fragment_completed_generated_sequence =
		gpu_vu_fragment_progress_completed_generated_sequence;
	u32 fragment_oldest_value = gpu_vu_fragment_progress_last_completed + 1u;
	if (fragment_oldest_value == 0u)
		fragment_oldest_value = 1u;
	const u32 fragment_newest_value = uses_fragment_progress_notification ?
		fragment_progress_target : gpu_vu_fragment_progress_last_submitted;
	if (fragment_newest_value != 0u &&
		VitaGpuVu::HasCompletedNotificationValue(
			fragment_newest_value, fragment_oldest_value))
	{
		const GpuVuFragmentProgressRecord& oldest =
			gpu_vu_fragment_progress_history[
				fragment_oldest_value %
					gpu_vu_fragment_progress_history.size()];
		if (oldest.value == fragment_oldest_value)
		{
			call_breadcrumb.fragment_oldest_value = oldest.value;
			call_breadcrumb.fragment_oldest_scene = oldest.scene_serial;
			call_breadcrumb.fragment_oldest_generated_scene =
				oldest.last_generated_scene;
			call_breadcrumb.fragment_oldest_generated_sequence =
				oldest.last_generated_sequence;
			call_breadcrumb.fragment_oldest_draw_call_count =
				oldest.generated_manifest.draw_call_count;
			call_breadcrumb.fragment_oldest_object_count =
				oldest.generated_manifest.object_count;
			call_breadcrumb.fragment_oldest_flags =
				oldest.generated_manifest.flags;
		}
	}
	call_breadcrumb.scene_render_target =
		reinterpret_cast<uptr>(scene_rt);
	call_breadcrumb.scene_depth_target =
		reinterpret_cast<uptr>(scene_ds);
	call_breadcrumb.scenes_since_generated =
		gpu_vu_last_generated_scene_serial != 0u &&
			scene_serial >= gpu_vu_last_generated_scene_serial ?
			static_cast<u32>(std::min<u64>(
				scene_serial - gpu_vu_last_generated_scene_serial,
				std::numeric_limits<u32>::max())) :
			std::numeric_limits<u32>::max();
	call_breadcrumb.scene_flags =
		(static_cast<u32>(scene_is_display) << 0u) |
		(static_cast<u32>(has_generated_transaction) << 1u) |
		(static_cast<u32>(uses_fragment_progress_notification) << 2u) |
		(static_cast<u32>(vertex_notification != nullptr) << 3u) |
		(static_cast<u32>(scene_rt != nullptr) << 4u) |
		(static_cast<u32>(scene_ds != nullptr) << 5u) |
		(static_cast<u32>(
			VitaGS::IsGpuVuFragmentCompletionIsolationEnabled()) << 6u) |
		(gpu_vu_generated_scene_manifest.flags << 8u);
	call_breadcrumb.retirement_slot = vertex_notification ?
		(next_gpu_vu_retirement_slot + gpu_vu_retirement_slots.size() - 1u) %
			gpu_vu_retirement_slots.size() : ~u32{0};
	if (vertex_notification)
	{
		call_breadcrumb.notification_address = reinterpret_cast<uptr>(
			vertex_notification->address);
		call_breadcrumb.notification_value = vertex_notification->value;
		call_breadcrumb.notification_observed = vertex_notification->address ?
			*vertex_notification->address : 0u;
	}
	if (fragment_notification)
	{
		call_breadcrumb.fragment_notification_address = reinterpret_cast<uptr>(
			fragment_notification->address);
		call_breadcrumb.fragment_notification_value =
			fragment_notification->value;
		call_breadcrumb.fragment_notification_observed =
			fragment_notification->address ?
				*fragment_notification->address : 0u;
	}
	const u64 call_sequence = generated_transaction_count != 0u ?
		generated_transaction_begin : gpu_vu_last_generated_sequence;
	const u32 call_token = call_sequence != 0u ?
		VitaGS::BeginGpuVuGxmCall(VitaGS::GpuVuGxmCallKind::EndScene,
			call_sequence, scene_serial, call_breadcrumb) : 0u;
	const u64 previous_end_wall = s_gxm_worker_performance.host_calls[
		static_cast<size_t>(VitaGxmHostCall::EndScene)].wall_us;
	const int end_result = TimeGxmHostCall(VitaGxmHostCall::EndScene, [&] {
		return sceGxmEndScene(context, vertex_notification, fragment_notification);
	});
	if (VitaPerformanceTelemetry::IsEnabled())
	{
		// Reuse the existing call timer. A scene's EndScene may block on earlier
		// work; this classifies host residence, never the scene's GPU duration.
		auto& stats = s_gxm_worker_performance.scene_contents[scene_ordinary_content];
		stats.scenes++;
		stats.end_scene_wall_us += s_gxm_worker_performance.host_calls[
			static_cast<size_t>(VitaGxmHostCall::EndScene)].wall_us - previous_end_wall;
	}
	VitaGS::CompleteGpuVuGxmCall(call_token, end_result);
	if (uses_fragment_progress_notification && end_result >= 0)
		gpu_vu_fragment_progress_last_submitted = fragment_progress_target;
	if (end_result < 0)
	{
		Fail("sceGxmEndScene", end_result);
		if (vertex_notification)
		{
			return FailGpuVuRetirementOwner("end-scene-return",
				reinterpret_cast<uptr>(vertex_notification->address),
				vertex_notification->value,
				*vertex_notification->address,
				gpu_vu_pre_notification_watchdog_sequence, 0u);
		}
		ready = false;
		return false;
	}
	scene_active = false;
	scene_is_display = false;
	scene_ordinary_content = 0;
	scene_rt = nullptr;
	scene_ds = nullptr;
	scene_raster_origin = {};
	scene_store_residency =
		VitaGXM::RenderStoreResidencyPlanner::InvalidIndex;
	scene_uses_render_store = false;
	if (has_generated_transaction && uses_fragment_progress_notification &&
		VitaGS::IsGpuVuFragmentCompletionIsolationEnabled() &&
		!WaitForGeneratedSceneFragmentCompletion(
			*fragment_notification, gpu_vu_generated_scene_manifest))
	{
		return false;
	}
	gpu_vu_generated_scene_manifest = {};
	if (vertex_notification)
	{
		VitaGpuVu::RecordGpuVuRetirementBatch();
		// EndScene can become the submission boundary before an explicit MTVU
		// transaction marker when immutable input generations or render targets
		// change. Arm the oldest transactional output here as well so CPU1 never
		// waits for a notification which was submitted but not bridged.
		if (!ArmPendingGpuVuRetirementWake())
		{
			return FailGpuVuRetirementOwner(
				"scene-end-notification-bridge",
				reinterpret_cast<uptr>(vertex_notification->address),
				vertex_notification->value,
				*vertex_notification->address,
				gpu_vu_pre_notification_watchdog_sequence, 0u);
		}
		CompleteGpuVuPreNotificationWatchdog();
	}
	if (generated_transaction_count != 0u)
	{
		const u64 report = ++gpu_vu_scene_end_transaction_submit_count;
		if (report <= 8u || (report & (report - 1u)) == 0u)
		{
			Console.WriteLn(
				"GPU-VU seq=%llu..%llu transaction_submit=scene-end "
				"batched_executes=%u additional_firmware_jobs=0 "
				"scene_jobs=1 notification=%u routine_waits=0.",
				static_cast<unsigned long long>(generated_transaction_begin),
				static_cast<unsigned long long>(generated_transaction_end),
				generated_transaction_count,
				vertex_notification ? vertex_notification->value : 0u);
		}
	}
	completed_scene_serial = finish ? scene_serial : completed_scene_serial;
	if (!finish)
		return true;
	u64 finish_sequence = std::numeric_limits<u64>::max();
	for (const GpuVuRetirementSlot& slot : gpu_vu_retirement_slots)
	{
		if (!slot.submitted)
			continue;
		bool found_transaction = false;
		for (const GpuVuPrivateStoreOutput& output :
			slot.private_store_outputs)
		{
			if (!output.transaction)
				continue;
			finish_sequence = std::min(finish_sequence, output.sequence);
			found_transaction = true;
		}
		if (!found_transaction)
			finish_sequence = std::min(
				finish_sequence, slot.submission_scene_serial);
	}
	if (finish_sequence != std::numeric_limits<u64>::max())
	{
		if (!ArmGpuVuPreNotificationWatchdog(finish_sequence))
			return false;
		UpdateGpuVuPreNotificationWatchdog(
			VitaGS::GpuVuGxmSubmissionStage::FinishDrain,
			finish_sequence);
	}
	const u32 finish_token = gpu_vu_last_generated_sequence != 0u ?
		VitaGS::BeginGpuVuGxmCall(VitaGS::GpuVuGxmCallKind::Finish,
			gpu_vu_last_generated_sequence, scene_serial) : 0u;
	TimeGxmHostCall(VitaGxmHostCall::Finish, [&] { return sceGxmFinish(context); });
	VitaGS::CompleteGpuVuGxmCall(finish_token, 0);
	CompleteGpuVuPreNotificationWatchdog();
	ReleaseAllGpuVuDraws();
	completed_scene_serial = scene_serial;
	vertex_offset = 0;
	index_offset = 0;
	return true;
}

bool GSDeviceGXM::Impl::Finish()
{
	if (!EndScene(false))
		return false;
	if (!context)
		return true;
	u64 finish_sequence = std::numeric_limits<u64>::max();
	for (const GpuVuRetirementSlot& slot : gpu_vu_retirement_slots)
	{
		if (!slot.submitted)
			continue;
		bool found_transaction = false;
		for (const GpuVuPrivateStoreOutput& output :
			slot.private_store_outputs)
		{
			if (!output.transaction)
				continue;
			finish_sequence = std::min(finish_sequence, output.sequence);
			found_transaction = true;
		}
		if (!found_transaction)
			finish_sequence = std::min(
				finish_sequence, slot.submission_scene_serial);
	}
	if (finish_sequence != std::numeric_limits<u64>::max())
	{
		if (!ArmGpuVuPreNotificationWatchdog(finish_sequence))
			return false;
		UpdateGpuVuPreNotificationWatchdog(
			VitaGS::GpuVuGxmSubmissionStage::FinishDrain,
			finish_sequence);
	}
	const u32 finish_token = gpu_vu_last_generated_sequence != 0u ?
		VitaGS::BeginGpuVuGxmCall(VitaGS::GpuVuGxmCallKind::Finish,
			gpu_vu_last_generated_sequence, scene_serial) : 0u;
	TimeGxmHostCall(VitaGxmHostCall::Finish, [&] { return sceGxmFinish(context); });
	VitaGS::CompleteGpuVuGxmCall(finish_token, 0);
	CompleteGpuVuPreNotificationWatchdog();
	ReleaseAllGpuVuDraws();
	completed_scene_serial = scene_serial;
	completed_transfer_serial = transfer_serial;
	vertex_offset = 0;
	index_offset = 0;
	return true;
}

void GSDeviceGXM::Impl::ConfigureRaster(u32 width, u32 height,
	u32 origin_x, u32 origin_y)
{
	sceGxmSetViewport(context,
		static_cast<float>(origin_x) + static_cast<float>(width) * 0.5f,
		static_cast<float>(width) * 0.5f,
		static_cast<float>(origin_y) + static_cast<float>(height) * 0.5f,
		static_cast<float>(height) * 0.5f, 0.0f, 1.0f);
	sceGxmSetViewportEnable(context, SCE_GXM_VIEWPORT_ENABLED);
	sceGxmSetCullMode(context, SCE_GXM_CULL_NONE);
	sceGxmSetFrontPolygonMode(context, SCE_GXM_POLYGON_MODE_TRIANGLE_FILL);
	sceGxmSetBackPolygonMode(context, SCE_GXM_POLYGON_MODE_TRIANGLE_FILL);
	sceGxmSetFrontFragmentProgramEnable(context, SCE_GXM_FRAGMENT_PROGRAM_ENABLED);
	sceGxmSetBackFragmentProgramEnable(context, SCE_GXM_FRAGMENT_PROGRAM_ENABLED);
	sceGxmSetFrontStencilFunc(context, SCE_GXM_STENCIL_FUNC_ALWAYS,
		SCE_GXM_STENCIL_OP_KEEP, SCE_GXM_STENCIL_OP_KEEP,
		SCE_GXM_STENCIL_OP_KEEP, 0xff, 0xff);
	sceGxmSetBackStencilFunc(context, SCE_GXM_STENCIL_FUNC_ALWAYS,
		SCE_GXM_STENCIL_OP_KEEP, SCE_GXM_STENCIL_OP_KEEP,
		SCE_GXM_STENCIL_OP_KEEP, 0xff, 0xff);
}

bool GSDeviceGXM::Impl::CommitClear(VitaGXM::GSTextureGXM& texture)
{
	if (texture.GetState() != GSTexture::State::Cleared)
		return true;
	struct ClearCommitScope
	{
		u32& depth;
		explicit ClearCommitScope(u32& value) : depth(value) { depth++; }
		~ClearCommitScope() { depth--; }
	} clear_scope(clear_commit_depth);

	// PCSX2 owner: GSDeviceOGL::CommitClear() materializes a lazy clear on the
	// GPU without waiting for earlier draws. Sony's api_libgxm/basic sample uses
	// the same ordered-scene clear triangle before ordinary rendering and calls
	// sceGxmFinish() only during teardown. Keep the lazy value out of recursive
	// EnsureScene() calls while its target-only clear scene is established.
	const bool depth_stencil = texture.IsDepthStencil();
	const u32 clear_color = depth_stencil ? 0 : texture.GetClearColor();
	const float clear_depth = depth_stencil ? texture.GetClearDepth() : 1.0f;
	const GSVector4i bounds = texture.GetRect();
	texture.SetState(GSTexture::State::Dirty);
	const bool gpu_clear = depth_stencil ?
		(texture.DepthStencilSurface() && EnsureScene(nullptr, &texture, bounds) &&
			DrawTargetClear(0, false, clear_depth, true)) :
		(texture.ColorSurface() && EnsureScene(&texture, nullptr, bounds) &&
			DrawTargetClear(clear_color, true, 1.0f, false));
	if (gpu_clear)
	{
		texture.RecordWriter(VitaGXM::TextureWriterKind::Clear, nullptr);
		return true;
	}

	// A failed target clear must remain lazy; claiming Dirty would expose
	// uninitialized storage to a later draw or readback.
	texture.SetState(GSTexture::State::Cleared);
	return false;
}

bool GSDeviceGXM::Impl::CommitAttachmentClears(VitaGXM::GSTextureGXM* rt,
	VitaGXM::GSTextureGXM* ds, const GSVector4i& scissor)
{
	// PCSX2 GSDeviceVK::{OMSetRenderTargets,RenderHW}: pending attachment
	// clears cover the full surface, independently of the following draw's
	// scissor. Keep both intended attachments in one scene; do not reorder
	// source materialization (RenderHW commits source clears before this call).
	const bool clear_color = rt && rt->GetState() == GSTexture::State::Cleared;
	const bool clear_depth = ds && ds->GetState() == GSTexture::State::Cleared;
	const u32 color = clear_color ? rt->GetClearColor() : 0;
	const float depth = clear_depth ? ds->GetClearDepth() : 1.0f;
	const GSVector4i bounds = (rt ? rt : ds)->GetRect();
	// Reserve for the full mask, clear triangle and subsequent partial mask
	// before entering this scene. No caller has staged its guest geometry yet.
	if (!HasGeometryCapacity(16, 24) && !Finish())
		return false;
	if (clear_color)
		rt->SetState(GSTexture::State::Dirty);
	if (clear_depth)
		ds->SetState(GSTexture::State::Dirty);
	// Marking only these pending clears Dirty makes this recursion establish
	// the attachment scene, without recursively materializing either clear.
	const bool cleared = EnsureScene(rt, ds, bounds) &&
		DrawTargetClear(color, clear_color, depth, clear_depth);
	if (!cleared)
	{
		if (clear_color)
			rt->SetState(GSTexture::State::Cleared);
		if (clear_depth)
			ds->SetState(GSTexture::State::Cleared);
		return false;
	}
	if (clear_color)
		rt->RecordWriter(VitaGXM::TextureWriterKind::Clear, nullptr);
	if (clear_depth)
		ds->RecordWriter(VitaGXM::TextureWriterKind::Clear, nullptr);
	// The color surface's immutable scene clip stays full-sized. A depth
	// attachment supplies the exact M-plane scissor for subsequent drawing.
	// Color-only partial scissors are deliberately kept on CommitClear's path.
	return ConfigureScissor(scissor, bounds.width(), bounds.height(), ds != nullptr);
}

bool GSDeviceGXM::Impl::DrawTargetClear(u32 color, bool write_color,
	float depth, bool write_depth)
{
	if (!scene_active || (!write_color && !write_depth) ||
		(write_color && !scene_rt) || (write_depth && !scene_ds))
	{
		return false;
	}
	void* vertex_memory = nullptr;
	u16* indices = nullptr;
	if (!ReserveGeometry(3, 3, &vertex_memory, &indices))
	{
		// Geometry is retained until the GPU completes it. The caller has not
		// staged guest geometry, so the ordinary rare arena-wrap drain can safely
		// rebuild this exact clear scene.
		VitaGXM::GSTextureGXM* const rt = scene_rt;
		VitaGXM::GSTextureGXM* const ds = scene_ds;
		const GSVector4i scissor = scene_scissor;
		if (!Finish() || !EnsureScene(rt, ds, scissor) ||
			!ReserveGeometry(3, 3, &vertex_memory, &indices))
		{
			return false;
		}
	}

	// Sony's clear_v.cg owns this oversized triangle. The color vertex program
	// accepts clip-space XYZ directly. ConfigureRaster() maps clip Z directly to
	// screen Z so low GS depth values retain their PCSX2 32-bit normalization.
	QuadVertex* vertices = static_cast<QuadVertex*>(vertex_memory);
	SetQuadVertex(vertices[0], -1.0f, -1.0f, color, 0.0f, 0.0f);
	SetQuadVertex(vertices[1], 3.0f, -1.0f, color, 0.0f, 0.0f);
	SetQuadVertex(vertices[2], -1.0f, 3.0f, color, 0.0f, 0.0f);
	const float clip_depth = depth;
	vertices[0].z = clip_depth;
	vertices[1].z = clip_depth;
	vertices[2].z = clip_depth;
	indices[0] = 0;
	indices[1] = 1;
	indices[2] = 2;

	sceGxmSetFrontDepthFunc(context, SCE_GXM_DEPTH_FUNC_ALWAYS);
	sceGxmSetBackDepthFunc(context, SCE_GXM_DEPTH_FUNC_ALWAYS);
	const SceGxmDepthWriteMode depth_write = write_depth ?
		SCE_GXM_DEPTH_WRITE_ENABLED : SCE_GXM_DEPTH_WRITE_DISABLED;
	sceGxmSetFrontDepthWriteEnable(context, depth_write);
	sceGxmSetBackDepthWriteEnable(context, depth_write);
	sceGxmSetVertexProgram(context, color_vertex_program);
	sceGxmSetFragmentProgram(context, color_fragment_program);
	sceGxmSetVertexStream(context, 0, vertices);
	// Sony GPU User's Guide p.172: disabling a fragment program with neither
	// discard nor depth replacement keeps ordinary depth rasterization while
	// suppressing color writes. Restore immediately after this depth-only clear.
	const bool suppress_color = (scene_rt || scene_uses_render_store) &&
		!write_color;
	if (suppress_color)
	{
		sceGxmSetFrontFragmentProgramEnable(context, SCE_GXM_FRAGMENT_PROGRAM_DISABLED);
		sceGxmSetBackFragmentProgramEnable(context, SCE_GXM_FRAGMENT_PROGRAM_DISABLED);
	}
	VitaGS::GpuVuGxmCallBreadcrumb call_breadcrumb;
	call_breadcrumb.index_count = 3u;
	const u32 call_token = gpu_vu_last_generated_sequence != 0u ?
		VitaGS::BeginGpuVuGxmCall(VitaGS::GpuVuGxmCallKind::ClearDraw,
			gpu_vu_last_generated_sequence, scene_serial, call_breadcrumb) : 0u;
	const int result = TimeGxmHostCall(VitaGxmHostCall::DrawClear, [&] {
		return sceGxmDraw(context, SCE_GXM_PRIMITIVE_TRIANGLES,
			SCE_GXM_INDEX_FORMAT_U16, indices, 3);
	});
	VitaGS::CompleteGpuVuGxmCall(call_token, result);
	if (suppress_color)
	{
		sceGxmSetFrontFragmentProgramEnable(context, SCE_GXM_FRAGMENT_PROGRAM_ENABLED);
		sceGxmSetBackFragmentProgramEnable(context, SCE_GXM_FRAGMENT_PROGRAM_ENABLED);
	}
	if (result < 0)
		return Fail("sceGxmDraw(lazy clear)", result);
	if (write_color)
		MarkSceneStoreWrite(scene_rt, VitaGXM::RenderStoreAllocator::Plane::Color);
	if (write_depth)
		MarkSceneStoreWrite(scene_ds, VitaGXM::RenderStoreAllocator::Plane::Depth);
	if (VitaPerformanceTelemetry::IsEnabled())
		scene_ordinary_content |= 4;
	RecordGxmDraw(3, sizeof(QuadVertex), 3);
	return true;
}

bool GSDeviceGXM::Impl::EnsureScene(VitaGXM::GSTextureGXM* rt,
	VitaGXM::GSTextureGXM* ds, const GSVector4i& scissor)
{
	if (!rt && !ds)
		return Reject("draw without a render target or depth target");
	const GSVector2i size = (rt ? rt : ds)->GetSize();
	if ((rt && rt->GetSize() != size) || (ds && ds->GetSize() != size))
		return Reject("mismatched color/depth target dimensions");

	// A malformed generated command can poison the immediate context and make
	// the *next* EndScene/BeginScene call the first blocking API. DrawGpuVu's
	// draw-local watchdog used to start after this function, leaving that exact
	// interval invisible in a no-coredump hard hang. Cover scene changes only
	// while generated mappings are still owned; ordinary GS-only scenes pay no
	// monitoring cost.
	u64 transition_sequence = OldestPendingGpuVuSequence();
	if (transition_sequence == 0u)
		transition_sequence = gpu_vu_last_generated_sequence;
	struct SceneTransitionWatchdogScope
	{
		Impl* owner = nullptr;
		bool inherited = false;
		bool owns = false;

		~SceneTransitionWatchdogScope()
		{
			if (owner && owns && !owner->gpu_vu_device_faulted)
				owner->CompleteGpuVuPreNotificationWatchdog();
		}
	} transition_watchdog{this,
		gpu_vu_pre_notification_watchdog_ticket != 0u, false};
	const auto arm_transition_watchdog = [&](
		VitaGS::GpuVuGxmSubmissionStage stage) {
		if (transition_sequence == 0u)
			return true;
		const bool already_armed =
			gpu_vu_pre_notification_watchdog_ticket != 0u;
		if (!ArmGpuVuPreNotificationWatchdog(transition_sequence))
			return false;
		if (!already_armed && !transition_watchdog.inherited)
			transition_watchdog.owns = true;
		UpdateGpuVuPreNotificationWatchdog(stage, transition_sequence);
		return true;
	};
	const bool commits_clear =
		(rt && rt->GetState() == GSTexture::State::Cleared) ||
		(ds && ds->GetState() == GSTexture::State::Cleared);
	if (commits_clear && !arm_transition_watchdog(
			VitaGS::GpuVuGxmSubmissionStage::SceneTransition))
	{
		return false;
	}
	const GSVector4i bounds(0, 0, size.x, size.y);
	const GSVector4i clipped_scissor = scissor.rintersect(bounds);
	if (clipped_scissor.rempty())
		return Reject("empty hardware draw scissor");
	const bool use_render_store = CanUseRenderStore(rt, ds) &&
		!active_gpu_vu_draws && gpu_vu_scene_draw_count == 0;
	if ((fuse_attachment_clears || use_render_store) && commits_clear &&
		transition_sequence == 0 &&
		!active_gpu_vu_draws && (ds || clipped_scissor.eq(bounds)))
	{
		return CommitAttachmentClears(rt, ds, clipped_scissor);
	}
	if ((rt && !CommitClear(*rt)) || (ds && !CommitClear(*ds)))
		return false;
	VitaGXM::RenderStoreResidencyPlanner::Acquisition store_acquisition;
	if (use_render_store && !AcquireRenderStore(rt, ds, &store_acquisition))
		return Reject("persistent render store exhausted");

	if (scene_active && !scene_is_display && scene_rt == rt && scene_ds == ds &&
		scene_scissor.eq(clipped_scissor))
	{
		return true;
	}
	const bool store_resident_switch = use_render_store && scene_active &&
		!scene_is_display && scene_uses_render_store &&
		!store_acquisition.load_color && !store_acquisition.load_depth;
	// Exact depth scissoring emits two mask rectangles while establishing the
	// new scene. Retire the staging arena before BeginScene when those quads
	// would cross its end; no caller has staged this draw yet.
	if (ds && !HasGeometryCapacity(8, 12) && !Finish())
		return false;
	if (VitaPerformanceTelemetry::IsEnabled())
	{
		// Count actual target-scene transitions after lazy-clear recursion, not
		// merely requests carrying a Cleared texture. A clear-only predecessor
		// is a fusion candidate, not proof that its full-surface clear can move.
		const u32 bits = scene_active ?
			((scene_rt != rt ? 1u : 0u) | (scene_ds != ds ? 2u : 0u) |
			 (!scene_scissor.eq(clipped_scissor) ? 4u : 0u) |
			 (scene_is_display ? 8u : 0u)) : 0u;
		auto& stats = s_gxm_worker_performance.scene_transitions[bits];
		stats.transitions++;
		stats.clear_calls += clear_commit_depth != 0;
		const bool follows_clear = scene_active && (scene_ordinary_content & 4u) != 0;
		stats.clear_followups += follows_clear;
		stats.clear_only_followups += follows_clear && (scene_ordinary_content & 3u) == 0;
	}
	if (!arm_transition_watchdog(
			VitaGS::GpuVuGxmSubmissionStage::SceneTransition))
	{
		return false;
	}
	if (store_resident_switch)
	{
		// The physical attachments do not change. Switch only the logical view:
		// viewport and region clip move to another tile-aligned resident pair.
		// No tile store/load or firmware scene is required.
		scene_rt = rt;
		scene_ds = ds;
		scene_store_residency = store_acquisition.residency;
		scene_raster_origin = GSVector2i(store_acquisition.region.x,
			store_acquisition.region.y);
		scene_scissor = {};
		if (VitaPerformanceTelemetry::IsEnabled())
			s_gxm_worker_performance.render_store_resident_switches++;
		if (rt)
			rt->MarkSceneUse(scene_serial);
		if (ds)
			ds->MarkSceneUse(scene_serial);
		ConfigureRaster(size.x, size.y, store_acquisition.region.x,
			store_acquisition.region.y);
		return ConfigureScissor(clipped_scissor, size.x, size.y, ds != nullptr);
	}
	if (!EndScene(false))
		return false;
	if (use_render_store && !LoadRenderStoreResidency(rt, ds, store_acquisition))
		return false;
	const u32 physical_width = use_render_store ?
		VitaGXM::RenderStoreAllocator::StoreWidth : static_cast<u32>(size.x);
	const u32 physical_height = use_render_store ?
		VitaGXM::RenderStoreAllocator::StoreHeight : static_cast<u32>(size.y);
	SceGxmRenderTarget* target = GetRenderTarget(physical_width, physical_height);
	if (!target)
		return false;
	SceGxmColorSurface* color = use_render_store ? &render_store_color_surface :
		(rt ? rt->ColorSurface() : nullptr);
	SceGxmDepthStencilSurface* depth = use_render_store ?
		&render_store_depth_surface :
		(ds ? ds->DepthStencilSurface() : &disabled_depth);
	if ((rt && !color) || (ds && !ds->DepthStencilSurface()))
		return Reject("texture lacks required GXM render surface");
	// Official libGXM contract: sceGxmBeginScene() copies the color-surface
	// descriptor. Program its exact pixel clip before the copy; changing the
	// descriptor during a scene affects only a later scene.
	if (color && !use_render_store)
	{
		sceGxmColorSurfaceSetClip(color, clipped_scissor.x, clipped_scissor.y,
			clipped_scissor.z - 1, clipped_scissor.w - 1);
	}
	const u64 reserved_serial = ++scene_serial;
	if (!arm_transition_watchdog(
			VitaGS::GpuVuGxmSubmissionStage::BeginScene))
	{
		return false;
	}
	const u32 call_token = transition_sequence != 0u ?
		VitaGS::BeginGpuVuGxmCall(VitaGS::GpuVuGxmCallKind::BeginScene,
			transition_sequence, reserved_serial) : 0u;
	const int result = TimeGxmHostCall(VitaGxmHostCall::BeginTarget, [&] {
		return sceGxmBeginScene(context, 0, target, nullptr, nullptr,
			nullptr, color, depth);
	});
	VitaGS::CompleteGpuVuGxmCall(call_token, result);
	if (result < 0)
		return Fail("sceGxmBeginScene(GS target)", result);
	scene_active = true;
	scene_is_display = false;
	scene_rt = rt;
	scene_ds = ds;
	scene_uses_render_store = use_render_store;
	if (use_render_store && VitaPerformanceTelemetry::IsEnabled())
		s_gxm_worker_performance.render_store_physical_scenes++;
	scene_store_residency = use_render_store ? store_acquisition.residency :
		VitaGXM::RenderStoreResidencyPlanner::InvalidIndex;
	scene_raster_origin = use_render_store ?
		GSVector2i(store_acquisition.region.x, store_acquisition.region.y) :
		GSVector2i();
	scene_scissor = {};
	if (rt)
		rt->MarkSceneUse(reserved_serial);
	if (ds)
		ds->MarkSceneUse(reserved_serial);
	ConfigureRaster(size.x, size.y, scene_raster_origin.x,
		scene_raster_origin.y);
	return ConfigureScissor(clipped_scissor, size.x, size.y, ds != nullptr);
}

bool GSDeviceGXM::Impl::BeginDisplayScene()
{
	u64 transition_sequence = OldestPendingGpuVuSequence();
	if (transition_sequence == 0u)
		transition_sequence = gpu_vu_last_generated_sequence;
	struct DisplayTransitionWatchdogScope
	{
		Impl* owner = nullptr;
		bool inherited = false;
		bool owns = false;

		~DisplayTransitionWatchdogScope()
		{
			if (owner && owns && !owner->gpu_vu_device_faulted)
				owner->CompleteGpuVuPreNotificationWatchdog();
		}
	} transition_watchdog{this,
		gpu_vu_pre_notification_watchdog_ticket != 0u, false};
	const auto arm_transition_watchdog = [&](
		VitaGS::GpuVuGxmSubmissionStage stage) {
		if (transition_sequence == 0u)
			return true;
		const bool already_armed =
			gpu_vu_pre_notification_watchdog_ticket != 0u;
		if (!ArmGpuVuPreNotificationWatchdog(transition_sequence))
			return false;
		if (!already_armed && !transition_watchdog.inherited)
			transition_watchdog.owns = true;
		UpdateGpuVuPreNotificationWatchdog(stage, transition_sequence);
		return true;
	};
	if (!arm_transition_watchdog(
			VitaGS::GpuVuGxmSubmissionStage::SceneTransition))
	{
		return false;
	}
	if (!EndScene(false))
		return false;
#if defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
	if (gpu_vu_universal_validation_stage ==
			UniversalGpuVuValidationStage::Submitted &&
		!CompleteUniversalGpuVuValidation())
	{
		return false;
	}
#endif
	const u64 reserved_serial = ++scene_serial;
	if (!arm_transition_watchdog(
			VitaGS::GpuVuGxmSubmissionStage::BeginScene))
	{
		return false;
	}
	const u32 call_token = transition_sequence != 0u ?
		VitaGS::BeginGpuVuGxmCall(VitaGS::GpuVuGxmCallKind::BeginScene,
			transition_sequence, reserved_serial) : 0u;
	const int result = TimeGxmHostCall(VitaGxmHostCall::BeginDisplay, [&] {
		return sceGxmBeginScene(context, 0, display_render_target,
			nullptr, nullptr, display.BackSyncObject(), display.BackColorSurface(),
			&disabled_depth);
	});
	VitaGS::CompleteGpuVuGxmCall(call_token, result);
	if (result < 0)
		return Fail("sceGxmBeginScene(display)", result);
	scene_active = true;
	scene_is_display = true;
	(void)reserved_serial;
	ConfigureRaster(VitaGXM::Display::Width, VitaGXM::Display::Height);
	sceGxmSetRegionClip(context, SCE_GXM_REGION_CLIP_OUTSIDE, 0, 0,
		VitaGXM::Display::Width - 1, VitaGXM::Display::Height - 1);
#if defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
	if (gpu_vu_universal_validation_stage ==
			UniversalGpuVuValidationStage::Ready &&
		!SubmitUniversalGpuVuValidation())
	{
		return false;
	}
#endif
	return true;
}

bool GSDeviceGXM::Impl::ReserveGeometry(u32 vertices, u32 indices,
	void** vertex_data, u16** index_data)
{
	if (!vertex_data || !index_data)
		return false;
	if (!HasGeometryCapacity(vertices, indices))
		return false;
	const u32 vertex_bytes = vertices * std::max<u32>(sizeof(TfxVertex), sizeof(QuadVertex));
	const u32 index_bytes = indices * sizeof(u16);
	vertex_offset = (vertex_offset + 15u) & ~15u;
	index_offset = (index_offset + 1u) & ~1u;
	*vertex_data = static_cast<u8*>(geometry_vertices.base) + vertex_offset;
	*index_data = reinterpret_cast<u16*>(
		static_cast<u8*>(geometry_indices.base) + index_offset);
	vertex_offset += vertex_bytes;
	index_offset += index_bytes;
	return true;
}

bool GSDeviceGXM::Impl::HasGeometryCapacity(u32 vertices, u32 indices) const
{
	const u64 aligned_vertex_offset = (static_cast<u64>(vertex_offset) + 15u) & ~15ull;
	const u64 aligned_index_offset = (static_cast<u64>(index_offset) + 1u) & ~1ull;
	const u64 vertex_bytes = static_cast<u64>(vertices) *
		std::max<u32>(sizeof(TfxVertex), sizeof(QuadVertex));
	const u64 index_bytes = static_cast<u64>(indices) * sizeof(u16);
	return aligned_vertex_offset + vertex_bytes <= GEOMETRY_VERTEX_BYTES &&
		aligned_index_offset + index_bytes <=
			GEOMETRY_INDEX_BYTES - GPU_VU_SEQUENTIAL_INDEX_BYTES;
}

bool GSDeviceGXM::Impl::DrawMaskRect(const GSVector4i& rect, u32 width,
	u32 height, SceGxmStencilFunc operation)
{
	void* vertex_memory = nullptr;
	u16* indices = nullptr;
	if (!ReserveGeometry(4, 6, &vertex_memory, &indices))
		return false;
	QuadVertex* vertices = static_cast<QuadVertex*>(vertex_memory);
	const float left = PixelToNdc(static_cast<float>(rect.x), width);
	const float right = PixelToNdc(static_cast<float>(rect.z), width);
	const float top = PixelToNdc(static_cast<float>(rect.y), height);
	const float bottom = PixelToNdc(static_cast<float>(rect.w), height);
	SetQuadVertex(vertices[0], left, top, 0xffffffffu, 0.0f, 0.0f);
	SetQuadVertex(vertices[1], right, top, 0xffffffffu, 0.0f, 0.0f);
	SetQuadVertex(vertices[2], left, bottom, 0xffffffffu, 0.0f, 0.0f);
	SetQuadVertex(vertices[3], right, bottom, 0xffffffffu, 0.0f, 0.0f);
	constexpr std::array<u16, 6> quad = {0, 1, 2, 2, 1, 3};
	std::copy(quad.begin(), quad.end(), indices);

	sceGxmSetFrontDepthFunc(context, SCE_GXM_DEPTH_FUNC_ALWAYS);
	sceGxmSetBackDepthFunc(context, SCE_GXM_DEPTH_FUNC_ALWAYS);
	sceGxmSetFrontDepthWriteEnable(context, SCE_GXM_DEPTH_WRITE_DISABLED);
	sceGxmSetBackDepthWriteEnable(context, SCE_GXM_DEPTH_WRITE_DISABLED);
	sceGxmSetFrontStencilFunc(context, operation, SCE_GXM_STENCIL_OP_KEEP,
		SCE_GXM_STENCIL_OP_KEEP, SCE_GXM_STENCIL_OP_KEEP, 0, 0);
	sceGxmSetBackStencilFunc(context, operation, SCE_GXM_STENCIL_OP_KEEP,
		SCE_GXM_STENCIL_OP_KEEP, SCE_GXM_STENCIL_OP_KEEP, 0, 0);
	sceGxmSetVertexProgram(context, present_vertex_program);
	sceGxmSetFragmentProgram(context, mask_update_program);
	sceGxmSetVertexStream(context, 0, vertices);
	VitaGS::GpuVuGxmCallBreadcrumb call_breadcrumb;
	call_breadcrumb.index_count = static_cast<u32>(quad.size());
	const u32 call_token = gpu_vu_last_generated_sequence != 0u ?
		VitaGS::BeginGpuVuGxmCall(VitaGS::GpuVuGxmCallKind::ScissorDraw,
			gpu_vu_last_generated_sequence, scene_serial, call_breadcrumb) : 0u;
	const int result = TimeGxmHostCall(VitaGxmHostCall::DrawScissor, [&] {
		return sceGxmDraw(context, SCE_GXM_PRIMITIVE_TRIANGLES,
			SCE_GXM_INDEX_FORMAT_U16, indices, quad.size());
	});
	VitaGS::CompleteGpuVuGxmCall(call_token, result);
	if (result < 0)
		return Fail("sceGxmDraw(scissor mask)", result);
	if (VitaPerformanceTelemetry::IsEnabled())
		scene_ordinary_content |= 8;
	RecordGxmDraw(4, sizeof(QuadVertex), quad.size());
	return true;
}

bool GSDeviceGXM::Impl::ConfigureScissor(const GSVector4i& requested,
	u32 width, u32 height, bool exact_depth)
{
	const GSVector4i bounds(0, 0, static_cast<int>(width), static_cast<int>(height));
	const GSVector4i scissor = requested.rintersect(bounds);
	if (scissor.rempty())
		return Reject("empty hardware draw scissor");
	if (scene_scissor.eq(scissor))
		return true;

	if (exact_depth)
	{
		// Sony graphics/api_libgxm/scissor owns this exact-pixel mask sequence.
		// Region clip alone rounds to tiles and would allow depth writes outside
		// the GS scissor. InitializeDepthStorage forces DF32M_S8 load/store, so
		// an unchanged mask survives scene/attachment changes on the same DS.
		// Ordinary depth/stencil writes do not modify M; raw copies invalidate
		// its per-texture rectangle. PCSX2 GSRasterizer::DrawPoint/DrawSprite
		// own the exact half-open pixel scissor, which this mask preserves.
#if defined(VITASX2_GXM_SCISSOR_REBUILD_CONTROL)
		constexpr bool reuse_mask = false;
#else
		// A logical depth target can have several pair residencies at different
		// atlas origins. Its legacy per-texture mask rectangle cannot prove which
		// physical copy owns those M bits; keep store masks residency-local until
		// that cache carries the residency generation as well.
		const bool reuse_mask = !scene_uses_render_store && scene_ds &&
			scene_ds->HasDepthMaskScissor(scissor);
#endif
		if (!reuse_mask)
		{
			if (scene_ds)
				scene_ds->InvalidateDepthMaskScissor();
			sceGxmSetRegionClip(context, SCE_GXM_REGION_CLIP_OUTSIDE,
				scene_raster_origin.x, scene_raster_origin.y,
				scene_raster_origin.x + width - 1,
				scene_raster_origin.y + height - 1);
			// Setting every mask bit overwrites a preceding full-surface clear.
			// Partial rectangles must still clear the outside pixels first.
			if ((!scissor.eq(bounds) &&
					!DrawMaskRect(bounds, width, height, SCE_GXM_STENCIL_FUNC_NEVER)) ||
				!DrawMaskRect(scissor, width, height, SCE_GXM_STENCIL_FUNC_ALWAYS))
			{
				return false;
			}
			if (scene_ds)
				scene_ds->SetDepthMaskScissor(scissor);
		}
		sceGxmSetFrontStencilFunc(context, SCE_GXM_STENCIL_FUNC_ALWAYS,
			SCE_GXM_STENCIL_OP_KEEP, SCE_GXM_STENCIL_OP_KEEP,
			SCE_GXM_STENCIL_OP_KEEP, 0xff, 0xff);
		sceGxmSetBackStencilFunc(context, SCE_GXM_STENCIL_FUNC_ALWAYS,
			SCE_GXM_STENCIL_OP_KEEP, SCE_GXM_STENCIL_OP_KEEP,
			SCE_GXM_STENCIL_OP_KEEP, 0xff, 0xff);
	}
	sceGxmSetRegionClip(context, SCE_GXM_REGION_CLIP_OUTSIDE,
		scene_raster_origin.x + scissor.x,
		scene_raster_origin.y + scissor.y,
		scene_raster_origin.x + scissor.z - 1,
		scene_raster_origin.y + scissor.w - 1);
	scene_scissor = scissor;
	return true;
}

#if defined(VITASX2_GXM_SCISSOR_VALIDATION)
bool GSDeviceGXM::Impl::ValidateScissorMaskStorage()
{
	using namespace ScissorMaskCases;
	int depth_error = 0, other_error = 0, color_error = 0;
	auto depth = VitaGXM::GSTextureGXM::Create(this, GSTexture::DepthStencil,
		Width, Height, 1, GSTexture::Format::DepthStencil, &depth_error);
	auto other = VitaGXM::GSTextureGXM::Create(this, GSTexture::DepthStencil,
		Width, Height, 1, GSTexture::Format::DepthStencil, &other_error);
	auto color = VitaGXM::GSTextureGXM::Create(this, GSTexture::RenderTarget,
		Width, Height, 1, GSTexture::Format::Color, &color_error);
	if (!depth || !other || !color)
	{
		Console.Error("GXM scissor validation: FAIL allocation depth=%08x other=%08x color=%08x.",
			depth_error, other_error, color_error);
		return false;
	}
	const GSVector4i bounds(0, 0, Width, Height);
	std::array<u32, Width * Height> initial{};
	std::array<u32, Width * Height> color_initial{};
	color_initial.fill(0x44332211u);
	for (size_t i = 0; i < initial.size(); i++)
		initial[i] = 0x3e800000u | ((i & 1u) << 31u); // Z=0.25, mixed M.
	const auto initialize = [&](VitaGXM::GSTextureGXM& texture) {
		std::memset(texture.StencilData(), 0x5a, texture.StencilStorageSize());
		texture.SetState(GSTexture::State::Dirty);
		return texture.CopyFromLinear(0, bounds, initial.data(), Width * 4);
	};
	if (!initialize(*other))
		return false;
	u64 mask_hash = 14695981039346656037ull;
	u64 control_mask_hash = 0;
	u32 checks = 0;
	bool force_update = true;
	const auto check = [&](const GSVector4i& scissor, u32 inside_depth,
		bool hash_mask, bool check_color, const GSVector4i* depth_scissor = nullptr) {
		const auto* z = static_cast<const u8*>(depth->LevelData(0));
		const auto* s = static_cast<const u8*>(depth->StencilData());
		std::array<u32, Width * Height> colors{};
		if (check_color && !color->CopyToLinear(0, bounds, colors.data(), Width * 4))
			return false;
		for (int y = 0; y < Height; y++)
		{
			for (int x = 0; x < Width; x++)
			{
				// Independent full-checkout GSRasterizer oracle produces the same
				// mask hash by recording DrawPoint's actual output spans.
				const bool inside = x >= scissor.x && x < scissor.z &&
					y >= scissor.y && y < scissor.w;
				u32 word = 0;
				std::memcpy(&word, z + y * depth->DepthPitch() + x * 4, 4);
				const GSVector4i& zr = depth_scissor ? *depth_scissor : scissor;
				const bool depth_inside = x >= zr.x && x < zr.z && y >= zr.y && y < zr.w;
				const u32 expected = (depth_inside ? inside_depth : 0x3e800000u) |
					(static_cast<u32>(inside) << 31u);
				u32 color_word = 0;
				if (check_color)
					color_word = colors[y * Width + x];
				if (word != expected || s[y * depth->StencilPitch() + x] != 0x5a ||
					(check_color && color_word != (inside ? 0xff765432u : 0x44332211u)))
				{
					Console.Error("GXM scissor validation: FAIL force_update=%u check=%u x=%d y=%d "
						"depth_mask=%08x expected=%08x stencil=%02x color=%08x.",
						static_cast<u32>(force_update), checks, x, y, word, expected,
						s[y * depth->StencilPitch() + x], color_word);
					return false;
				}
				if (hash_mask)
				{
					mask_hash ^= word >> 31u;
					mask_hash *= 1099511628211ull;
				}
			}
		}
		checks++;
		return true;
	};
	for (u32 mode = 0; mode < 2; mode++)
	{
		force_update = mode == 0;
		mask_hash = 14695981039346656037ull;
		for (const auto& rectangle : Rectangles)
		{
			const GSVector4i requested(rectangle[0], rectangle[1], rectangle[2], rectangle[3]);
			const GSVector4i clipped = requested.rintersect(bounds);
			if (!initialize(*depth) ||
				!color->CopyFromLinear(0, bounds, color_initial.data(), Width * 4))
				return false;
			color->SetState(GSTexture::State::Dirty);
			if (!EnsureScene(nullptr, depth.get(), requested) ||
				!DrawTargetClear(0, false, 0.5f, true) || !Finish() ||
				!check(clipped, 0x3f000000u, true, false))
				return false;
			// Switch DS, then return to the first DS with a new color attachment.
			// No finish between these scenes: exercise the product ordering itself.
			// The first sweep is the uncached mask-update control.
			if (force_update)
				depth->InvalidateDepthMaskScissor();
			if (!EnsureScene(nullptr, other.get(), bounds) ||
				!DrawTargetClear(0, false, 0.5f, true) ||
				!EnsureScene(color.get(), depth.get(), requested) ||
				!DrawTargetClear(0xff765432u, true, 0.5f, true) || !Finish() ||
				!check(clipped, 0x3f000000u, false, true))
				return false;
			// Raw copies must invalidate even when the requested rectangle is the
			// same. They carry arbitrary mask bits in the sign bit of DF32M.
			if (!initialize(*depth) || !EnsureScene(nullptr, depth.get(), requested) ||
				!DrawTargetClear(0, false, 0.5f, true) || !Finish() ||
				!check(clipped, 0x3f000000u, false, false))
				return false;
			// Change a known mask without a raw copy; no depth/stencil/color write
			// is allowed merely because ConfigureScissor changes the M plane.
			const GSVector4i alternate = clipped.eq(bounds) ? GSVector4i(3, 5, 19, 23) : bounds;
			if (!EnsureScene(nullptr, depth.get(), alternate) || !Finish() ||
				!check(alternate, 0x3f000000u, false, false, &clipped))
				return false;
		}
		if (force_update)
			control_mask_hash = mask_hash;
		else if (mask_hash != control_mask_hash)
			return false;
	}
	Console.WriteLn("GXM scissor validation: PASS cases=%u checks=%u mask_hash=%016llx.",
		static_cast<u32>(Rectangles.size()), checks, static_cast<unsigned long long>(mask_hash));
	return true;
}
#endif

#if defined(VITASX2_GXM_GS_BLEND_VALIDATION)
bool GSDeviceGXM::Impl::ValidateGsBlend()
{
	constexpr int width = 8, height = 8;
	const GSVector4i bounds(0, 0, width, height);
	int error = 0;
	auto rt = VitaGXM::GSTextureGXM::Create(this, GSTexture::RenderTarget,
		width, height, 1, GSTexture::Format::Color, &error);
	auto ds = VitaGXM::GSTextureGXM::Create(this, GSTexture::DepthStencil,
		width, height, 1, GSTexture::Format::DepthStencil, &error);
	auto tex = VitaGXM::GSTextureGXM::Create(this, GSTexture::Texture,
		4, 4, 1, GSTexture::Format::Color, &error);
	if (!rt || !ds || !tex)
		return false;
	std::array<GSVertex, 4> vertices{};
	const std::array<u16, 6> indices = {0, 1, 2, 2, 1, 3};
	std::array<u32, width * height> initial{}, colors{}, depth{};
	depth.fill(0x3e800000u);
	constexpr u8 alpha[] = {0, 1, 127, 128, 129, 254, 255, 64};
	u64 hash = UINT64_C(14695981039346656037);
	u32 cases = 0;
	std::array<u32, GS_BLEND_PROGRAM_COUNT> program_cases{};
	for (u32 tuple = 0; tuple < 81; tuple++)
		for (u32 pattern = 0; pattern < 32; pattern++)
		{
			GSHWDrawConfig cfg{};
			u32 key = tuple;
			auto& op = cfg.gs_blend;
			op.d = key % 3; key /= 3; op.c = key % 3; key /= 3;
			op.b = key % 3; op.a = key / 3;
			op.fix = alpha[(pattern + 5) & 7];
			op.enabled = true; op.pabe = (pattern & 1) != 0;
			op.clamp = (pattern & 2) != 0;
			const u32 source = ((pattern * 31 + 17) & 255) |
				(((pattern * 47 + 113) & 255) << 8) |
				(((pattern * 73 + 211) & 255) << 16) |
				(static_cast<u32>(alpha[pattern & 7]) << 24);
			const u32 destination = ((pattern * 59 + 239) & 255) |
				(((pattern * 23 + 31) & 255) << 8) |
				(((pattern * 41 + 5) & 255) << 16) |
				(static_cast<u32>(alpha[(pattern + 3) & 7]) << 24);
			initial.fill(destination);
			cfg.rt = rt.get(); cfg.ds = ds.get();
			cfg.verts = vertices.data(); cfg.indices = indices.data();
			cfg.nverts = vertices.size(); cfg.nindices = indices.size(); cfg.indices_per_prim = 3;
			cfg.topology = GSHWDrawConfig::Topology::Triangle;
			cfg.scissor = (pattern & 4) ? GSVector4i(1, 2, 7, 6) : bounds;
			cfg.drawarea = bounds;
			cfg.ps.tfx = 4; cfg.ps.tcc = 1;
			// Both compiled test capabilities: GEQUAL against zero always passes
			// this unsigned source matrix, but must retain the discard program.
			cfg.ps.atst = (pattern & 1) ? GSHWDrawConfig::PS_ATST::GEQUAL :
				GSHWDrawConfig::PS_ATST::NONE;
			cfg.ps.dst_fmt = (pattern >> 3) & 1;
			cfg.ps.zfloor = (pattern >> 4) & 1;
			cfg.ps.fba = (pattern >> 2) & 1;
			cfg.ps.blend_a = op.a; cfg.ps.blend_b = op.b;
			cfg.ps.blend_c = op.c; cfg.ps.blend_d = op.d;
			cfg.ps.pabe = op.pabe; cfg.ps.colclip = !op.clamp;
			cfg.ps.fbmask = (pattern & 4) != 0;
			const u32 keep_mask = (cfg.ps.fbmask ? 0x3ca55a0fu : 0u) |
				(cfg.ps.dst_fmt ? 0xff000000u : 0u);
			cfg.cb_ps.FbMask = GSVector4i(keep_mask & 255, (keep_mask >> 8) & 255,
				(keep_mask >> 16) & 255, keep_mask >> 24);
			cfg.colormask.wrgba = cfg.ps.dst_fmt ? 7 : 15;
			cfg.depth.ztst = ZTST_GEQUAL; cfg.depth.zwe = true;
			cfg.cb_vs.vertex_scale = GSVector2(2.0f / (16 * width), 2.0f / (16 * height));
			cfg.cb_vs.vertex_offset = GSVector2(1.0f, 1.0f);
			cfg.cb_vs.max_depth = 0xffffffffu;
			cfg.cb_ps.TA_MaxDepth_Af.a = op.fix / 128.0f;
			cfg.cb_ps.STScale = GSVector2(1.0f, 1.0f);
			for (u32 i = 0; i < vertices.size(); i++)
			{
				auto& v = vertices[i];
				v.XYZ.X = (i & 1) * width * 16; v.XYZ.Y = (i >> 1) * height * 16;
				v.XYZ.Z = 0x80000000u; v.RGBAQ.Q = 1.0f;
				v.RGBAQ.R = source; v.RGBAQ.G = source >> 8;
				v.RGBAQ.B = source >> 16; v.RGBAQ.A = source >> 24;
			}
			if (!Finish() || !rt->CopyFromLinear(0, bounds, initial.data(), width * 4) ||
				!ds->CopyFromLinear(0, bounds, depth.data(), width * 4))
				return false;
			std::memset(ds->StencilData(), 0x5a, ds->StencilStorageSize());
			rt->SetState(GSTexture::State::Dirty); ds->SetState(GSTexture::State::Dirty);
			u32 expected = destination;
			const u32 program_key = SelectGsBlendProgram(cfg, cfg.ps);
			program_cases[program_key]++;
			// Two overlapping draws in one scene must observe the preceding result.
			for (u32 draw = 0; draw < 2; draw++)
			{
				if (!StageAndDraw(cfg, cfg.ps, 0, indices.size(), nullptr,
						gs_blend_programs[program_key],
						false, false, false, false, false, false, false, false, false, false, false))
					return false;
				u32 next = 0;
				for (u32 channel = 0; channel < 3; channel++)
				{
					const int value = VitaGS::BlendComponent(op, (source >> (8 * channel)) & 255,
						(expected >> (8 * channel)) & 255, source >> 24,
						cfg.ps.dst_fmt ? 128 : expected >> 24);
					next |= static_cast<u32>(VitaGS::StoreBlendComponent(op, value)) << (8 * channel);
				}
				next |= (source | (cfg.ps.fba && !cfg.ps.dst_fmt ? 0x80000000u : 0u)) & 0xff000000u;
				expected = (next & ~keep_mask) | (expected & keep_mask);
			}
			if (!Finish() || !rt->CopyToLinear(0, bounds, colors.data(), width * 4))
				return false;
			for (int y = 0; y < height; y++)
				for (int x = 0; x < width; x++)
				{
					const bool inside = x >= cfg.scissor.x && x < cfg.scissor.z &&
						y >= cfg.scissor.y && y < cfg.scissor.w;
					u32 z;
					std::memcpy(&z, static_cast<const u8*>(ds->LevelData(0)) + y * ds->DepthPitch() + x * 4, 4);
					const u32 expected_z = inside ? 0xbf000000u : 0x3e800000u;
					const u32 expected_color = inside ? expected : destination;
					if (colors[y * width + x] != expected_color || z != expected_z ||
						static_cast<const u8*>(ds->StencilData())[y * ds->StencilPitch() + x] != 0x5a)
					{
						Console.Error("GXM GS blend validation: FAIL tuple=%u pattern=%u x=%d y=%d color=%08x expected=%08x z=%08x expected_z=%08x.",
							tuple, pattern, x, y, colors[y * width + x], expected_color, z, expected_z);
						return false;
					}
					hash = (hash ^ colors[y * width + x]) * UINT64_C(1099511628211);
					hash = (hash ^ z) * UINT64_C(1099511628211);
				}
			cases++;
		}
	// The two canonical direct MODULATE/STQ kernels share the exact blend
	// operation proven above, but also remove the generic source/sampling
	// selector tree. Differentially exercise their interpolated texture path on
	// the physical SGX, including the separate depth-replacement pass type.
	std::array<u32, 16> texels{};
	for (u32 i = 0; i < texels.size(); i++)
	{
		texels[i] = ((i * 29u + 17u) & 255u) |
			(((i * 47u + 31u) & 255u) << 8u) |
			(((i * 71u + 53u) & 255u) << 16u) |
			(((i * 19u + 97u) & 255u) << 24u);
	}
	if (!tex->Update(GSVector4i(0, 0, 4, 4), texels.data(), 4 * 4))
		return false;
	std::array<u32, width * height> control_color{}, control_depth{};
	for (u32 zfloor = 0; zfloor < 2; zfloor++)
	for (u32 pattern = 0; pattern < 16; pattern++)
	{
		GSHWDrawConfig cfg{};
		cfg.gs_blend = {0, 2, 1, 1, 0, false, true, true};
		const u32 source = ((pattern * 37u + 11u) & 255u) |
			(((pattern * 59u + 43u) & 255u) << 8u) |
			(((pattern * 83u + 79u) & 255u) << 16u) |
			(((pattern * 23u + 101u) & 255u) << 24u);
		const u32 destination = ((pattern * 61u + 211u) & 255u) |
			(((pattern * 41u + 157u) & 255u) << 8u) |
			(((pattern * 31u + 89u) & 255u) << 16u) |
			(((pattern * 17u + 3u) & 255u) << 24u);
		initial.fill(destination);
		cfg.rt = rt.get(); cfg.ds = ds.get(); cfg.tex = tex.get();
		cfg.verts = vertices.data(); cfg.indices = indices.data();
		cfg.nverts = vertices.size(); cfg.nindices = indices.size();
		cfg.indices_per_prim = 3;
		cfg.topology = GSHWDrawConfig::Topology::Triangle;
		cfg.scissor = (pattern & 1u) ? GSVector4i(1, 1, 7, 7) : bounds;
		cfg.drawarea = bounds;
		cfg.vs.tme = true;
		cfg.ps.tfx = 0; cfg.ps.tcc = true; cfg.ps.zfloor = zfloor;
		cfg.ps.blend_a = 0; cfg.ps.blend_b = 2;
		cfg.ps.blend_c = 1; cfg.ps.blend_d = 1;
		cfg.colormask.wrgba = 15;
		cfg.depth.ztst = ZTST_GEQUAL; cfg.depth.zwe = true;
		cfg.cb_vs.vertex_scale = GSVector2(2.0f / (16 * width),
			2.0f / (16 * height));
		cfg.cb_vs.vertex_offset = GSVector2(1.0f, 1.0f);
		cfg.cb_vs.texture_scale = GSVector2(1.0f, 1.0f);
		cfg.cb_vs.max_depth = 0xffffffffu;
		cfg.cb_ps.WH = GSVector4(4.0f, 4.0f, 4.0f, 4.0f);
		cfg.cb_ps.STScale = GSVector2(1.0f, 1.0f);
		for (u32 i = 0; i < vertices.size(); i++)
		{
			auto& v = vertices[i];
			v.XYZ.X = (i & 1u) * width * 16;
			v.XYZ.Y = (i >> 1u) * height * 16;
			v.XYZ.Z = 0x80000000u;
			v.ST.S = (i & 1u) ? 0.875f : 0.125f;
			v.ST.T = (i >> 1u) ? 0.875f : 0.125f;
			v.RGBAQ.Q = 1.0f;
			v.RGBAQ.R = source; v.RGBAQ.G = source >> 8;
			v.RGBAQ.B = source >> 16; v.RGBAQ.A = source >> 24;
		}
		const u32 specialized_key = SelectGsBlendProgram(cfg, cfg.ps);
		const u32 expected_key = zfloor ? GS_BLEND_DIRECT_MODULATE_STQ_ZFLOOR :
			GS_BLEND_DIRECT_MODULATE_STQ;
		if (specialized_key != expected_key)
			return Fail("native GS direct MODULATE key mismatch",
				SCE_GXM_ERROR_INVALID_VALUE);
		program_cases[specialized_key]++;
		for (u32 variant = 0; variant < 2; variant++)
		{
			GSHWDrawConfig draw_config = cfg;
			// FMT_32 ignores AEM in ApplyTextureAem(), so this is a
			// semantic no-op which deliberately keeps the control on the
			// generic no-ATST capability. StageAndDraw must see the same
			// selector which chose its GXP so it uploads that GXP's uniform
			// layout rather than the specialized layout.
			if (!variant)
				draw_config.ps.aem = true;
			const u32 draw_key = SelectGsBlendProgram(draw_config,
				draw_config.ps);
			if (draw_key != (variant ? specialized_key : (2u | zfloor)))
				return Fail("native GS direct MODULATE control key mismatch",
					SCE_GXM_ERROR_INVALID_VALUE);
			if (!Finish() ||
				!rt->CopyFromLinear(0, bounds, initial.data(), width * 4) ||
				!ds->CopyFromLinear(0, bounds, depth.data(), width * 4))
			{
				return false;
			}
			std::memset(ds->StencilData(), 0x5a, ds->StencilStorageSize());
			rt->SetState(GSTexture::State::Dirty);
			ds->SetState(GSTexture::State::Dirty);
			if (!StageAndDraw(draw_config, draw_config.ps, 0, indices.size(),
					tex.get(), gs_blend_programs[draw_key],
					false, false, false, false, false, false, false, false,
					false, false, false) || !Finish() ||
				!rt->CopyToLinear(0, bounds, colors.data(), width * 4))
			{
				return false;
			}
			for (int y = 0; y < height; y++)
			for (int x = 0; x < width; x++)
			{
				const size_t pixel = y * width + x;
				u32 observed_depth;
				std::memcpy(&observed_depth,
					static_cast<const u8*>(ds->LevelData(0)) +
						y * ds->DepthPitch() + x * 4, 4);
				const u8 stencil = static_cast<const u8*>(ds->StencilData())[
					y * ds->StencilPitch() + x];
				if (stencil != 0x5a || (variant &&
					(colors[pixel] != control_color[pixel] ||
					 observed_depth != control_depth[pixel])))
				{
					Console.Error("GXM GS direct MODULATE validation: FAIL zfloor=%u pattern=%u x=%d y=%d color=%08x control=%08x depth=%08x control_depth=%08x stencil=%02x.",
						zfloor, pattern, x, y, colors[pixel],
						control_color[pixel], observed_depth,
						control_depth[pixel], stencil);
					return false;
				}
				if (!variant)
				{
					control_color[pixel] = colors[pixel];
					control_depth[pixel] = observed_depth;
				}
				else
				{
					hash = (hash ^ colors[pixel]) * UINT64_C(1099511628211);
					hash = (hash ^ observed_depth) * UINT64_C(1099511628211);
				}
			}
		}
		cases++;
	}
	for (u32 key = 0; key < program_cases.size(); key++)
	{
		if (!program_cases[key])
			return Fail("native GS fixture missed program capability", SCE_GXM_ERROR_INVALID_VALUE);
		Console.WriteLn("GXM GS blend validation: program_key=%u cases=%u.", key, program_cases[key]);
	}
	Console.WriteLn("GXM GS blend validation: PASS cases=%u draws=%u color_depth_hash=%016llx.",
		cases, cases * 2, static_cast<unsigned long long>(hash));
	return true;
}
#endif

#if defined(VITASX2_GXM_TFX_ALPHA_VALIDATION)
bool GSDeviceGXM::Impl::ValidateTfxAlphaSpecialization()
{
	// A product-path differential, not a replacement GS oracle. PCSX2's actual
	// Vulkan atst() is exercised separately by run_pcsx2_alpha_test_oracle.py.
	// Here prove that removing its inactive test preserves GXM color, Z, M and
	// stencil, including fog and coverage alpha which must NOT be removed.
	constexpr int width = 16, height = 16;
	constexpr u32 cases = 512;
	const GSVector4i bounds(0, 0, width, height);
	int error = 0;
	auto rt = VitaGXM::GSTextureGXM::Create(this, GSTexture::RenderTarget,
		width, height, 1, GSTexture::Format::Color, &error);
	auto ds = VitaGXM::GSTextureGXM::Create(this, GSTexture::DepthStencil,
		width, height, 1, GSTexture::Format::DepthStencil, &error);
	auto tex = VitaGXM::GSTextureGXM::Create(this, GSTexture::Texture,
		8, 8, 1, GSTexture::Format::Color, &error);
	if (!rt || !ds || !tex)
		return false;
	const SceGxmProgram* program = sceGxmFragmentProgramGetProgram(tfx_fast_no_atst_program);
	if (sceGxmProgramIsDiscardUsed(program) || sceGxmProgramIsDepthReplaceUsed(program))
		return false;
	std::array<u32, 64> texels{};
	for (u32 i = 0; i < texels.size(); i++)
		texels[i] = (i * 37u & 255u) | ((i * 53u & 255u) << 8u) |
			((i * 71u & 255u) << 16u) | ((i * 97u & 255u) << 24u);
	if (!tex->Update(GSVector4i(0, 0, 8, 8), texels.data(), 8 * 4))
		return false;
	std::array<u32, width * height> initial_color{}, initial_depth{}, control_color{}, control_depth{};
	initial_color.fill(0x44332211u);
	initial_depth.fill(0x3e800000u); // Z=0.25, mask initially clear.
	std::array<GSVertex, 4> vertices{};
	const std::array<u16, 6> indices = {0, 1, 2, 2, 1, 3};
	u64 hash = UINT64_C(14695981039346656037);
	u64 written_pixels = 0;
	for (u32 test = 0; test < cases; test++)
	{
		GSHWDrawConfig cfg{};
		cfg.rt = rt.get(); cfg.ds = ds.get(); cfg.tex = tex.get();
		cfg.verts = vertices.data(); cfg.indices = indices.data();
		cfg.nverts = vertices.size(); cfg.nindices = indices.size(); cfg.indices_per_prim = 3;
		cfg.topology = GSHWDrawConfig::Topology::Triangle;
		cfg.scissor = (test & 1u) ? GSVector4i(1, 2, 15, 14) : bounds;
		cfg.drawarea = bounds;
		cfg.vs.tme = (test >> 2) & 1u;
		cfg.vs.iip = (test >> 7) & 1u;
		cfg.ps.tfx = test & 3u;
		cfg.ps.tcc = (test >> 3) & 1u;
		cfg.ps.fog = (test >> 4) & 1u;
		cfg.ps.fixed_one_a = (test >> 5) & 1u;
		cfg.ps.fba = (test >> 6) & 1u;
		cfg.ps.fst = (test >> 8) & 1u;
		cfg.ps.atst = GSHWDrawConfig::PS_ATST::NONE;
		cfg.colormask.wrgba = 15;
		cfg.depth.ztst = ZTST_GEQUAL;
		cfg.depth.zwe = (test >> 3) & 1u;
		cfg.cb_vs.vertex_scale = GSVector2(2.0f / (16 * width), 2.0f / (16 * height));
		cfg.cb_vs.vertex_offset = GSVector2(1.0f, 1.0f);
		cfg.cb_vs.texture_scale = GSVector2(1.0f / 128.0f, 1.0f / 128.0f);
		cfg.cb_vs.max_depth = 0xffffffffu;
		cfg.cb_ps.WH = GSVector4(8.0f, 8.0f, 8.0f, 8.0f);
		cfg.cb_ps.STScale = GSVector2(1.0f, 1.0f);
		cfg.cb_ps.FogColor_AREF = GSVector4(19.0f, 71.0f, 151.0f, static_cast<float>(test & 255u));
		for (u32 i = 0; i < vertices.size(); i++)
		{
			auto& v = vertices[i];
			v.XYZ.X = (i & 1u) * width * 16;
			v.XYZ.Y = (i >> 1) * height * 16;
			v.XYZ.Z = 0x80000000u; // Exactly representable Z=0.5.
			v.U = (i & 1u) * 8 * 16; v.V = (i >> 1) * 8 * 16;
			v.ST.S = static_cast<float>(i & 1u); v.ST.T = static_cast<float>(i >> 1);
			v.RGBAQ.Q = 1.0f;
			v.RGBAQ.R = 37u + i * 49u; v.RGBAQ.G = 211u - i * 31u;
			v.RGBAQ.B = 17u + i * 73u;
			constexpr u8 alpha[] = {0, 127, 128, 255};
			v.RGBAQ.A = alpha[(i + test) & 3u];
			v.FOG = 17u + i * 73u;
		}
		for (u32 variant = 0; variant < 2; variant++)
		{
			if (!Finish() || !rt->CopyFromLinear(0, bounds, initial_color.data(), width * 4) ||
				!ds->CopyFromLinear(0, bounds, initial_depth.data(), width * 4))
				return false;
			std::memset(ds->StencilData(), 0x5a, ds->StencilStorageSize());
			rt->SetState(GSTexture::State::Dirty); ds->SetState(GSTexture::State::Dirty);
			if (!StageAndDraw(cfg, cfg.ps, 0, indices.size(), cfg.vs.tme ? tex.get() : nullptr,
					variant ? tfx_fast_no_atst_program : tfx_opaque_program,
					false, true, false, false, false, false, false, false, false, false, false) || !Finish())
				return false;
			std::array<u32, width * height> colors{};
			if (!rt->CopyToLinear(0, bounds, colors.data(), width * 4))
				return false;
			const u8* depth = static_cast<const u8*>(ds->LevelData(0));
			const u8* stencil = static_cast<const u8*>(ds->StencilData());
			u32 changed = 0;
			for (int y = 0; y < height; y++)
				for (int x = 0; x < width; x++)
				{
					const size_t pixel = y * width + x;
					u32 z;
					std::memcpy(&z, depth + y * ds->DepthPitch() + x * 4, 4);
					changed += colors[pixel] != initial_color[pixel];
					if (stencil[y * ds->StencilPitch() + x] != 0x5a ||
						(variant && (colors[pixel] != control_color[pixel] || z != control_depth[pixel])))
					{
						Console.Error("GXM TFX alpha validation: FAIL case=%u variant=%u x=%d y=%d "
							"color=%08x control=%08x depth=%08x control_depth=%08x.",
							test, variant, x, y, colors[pixel], control_color[pixel], z, control_depth[pixel]);
						return false;
					}
					if (!variant)
					{
						control_color[pixel] = colors[pixel]; control_depth[pixel] = z;
					}
					else
					{
						hash = (hash ^ colors[pixel]) * UINT64_C(1099511628211);
						hash = (hash ^ z) * UINT64_C(1099511628211);
					}
				}
			if (!changed)
				return Fail("TFX alpha fixture produced no color", SCE_GXM_ERROR_INVALID_VALUE);
			written_pixels += changed;
		}
	}
	Console.WriteLn("GXM TFX alpha validation: PASS cases=%u draws=%u written_pixels=%llu "
		"color_depth_hash=%016llx discard=0 depth_replace=0.", cases, cases * 2,
		static_cast<unsigned long long>(written_pixels), static_cast<unsigned long long>(hash));
	return true;
}
#endif

#if defined(VITASX2_GXM_ATTACHMENT_CLEAR_VALIDATION)
bool GSDeviceGXM::Impl::ValidateAttachmentClears()
{
	using namespace ScissorMaskCases;
	struct RestoreMode
	{
		bool& mode;
		bool saved;
		~RestoreMode() { mode = saved; }
	} restore{fuse_attachment_clears, fuse_attachment_clears};
	int error = 0;
	auto rt = VitaGXM::GSTextureGXM::Create(this, GSTexture::RenderTarget,
		Width, Height, 1, GSTexture::Format::Color, &error);
	auto ds = VitaGXM::GSTextureGXM::Create(this, GSTexture::DepthStencil,
		Width, Height, 1, GSTexture::Format::DepthStencil, &error);
	auto other = VitaGXM::GSTextureGXM::Create(this, GSTexture::RenderTarget,
		Width, Height, 1, GSTexture::Format::Color, &error);
	if (!rt || !ds || !other)
		return false;
	const GSVector4i bounds(0, 0, Width, Height);
	constexpr u32 initial_color = 0x44332211u, clear_color = 0xa17935e7u;
	constexpr u32 draw_color = 0xff765432u, initial_z = 0x3e800000u;
	std::array<u32, Width * Height> colors{}, depths{}, control_color{}, control_depth{};
	colors.fill(initial_color);
	for (u32 i = 0; i < depths.size(); i++)
		depths[i] = initial_z | ((i & 1u) << 31u);
	if (!other->CopyFromLinear(0, bounds, colors.data(), Width * 4))
		return false;
	other->SetState(GSTexture::State::Dirty);
	u64 hash = UINT64_C(14695981039346656037);
	u64 mask_hash = UINT64_C(14695981039346656037);
	u64 scenes[2] = {};
	u32 cases = 0, checks = 0;
	for (u32 attachments = 1; attachments <= 3; attachments++)
	for (u32 clears = 1; clears <= 3; clears++)
	{
		if ((clears & attachments) != clears)
			continue;
		for (const auto& rectangle : Rectangles)
		for (u32 prior = 0; prior < 3; prior++)
		for (u32 rollover = 0; rollover < 2; rollover++)
		for (u32 draw = 0; draw < 2; draw++)
		{
			const GSVector4i requested(rectangle[0], rectangle[1], rectangle[2], rectangle[3]);
			const GSVector4i clipped = requested.rintersect(bounds);
			auto* color_target = (attachments & 1) ? rt.get() : nullptr;
			auto* depth_target = (attachments & 2) ? ds.get() : nullptr;
			for (u32 variant = 0; variant < 2; variant++)
			{
				fuse_attachment_clears = variant != 0;
				if (!Finish() || !rt->CopyFromLinear(0, bounds, colors.data(), Width * 4) ||
					!ds->CopyFromLinear(0, bounds, depths.data(), Width * 4))
					return false;
				rt->SetState(GSTexture::State::Dirty);
				ds->SetState(GSTexture::State::Dirty);
				std::memset(ds->StencilData(), 0x5a, ds->StencilStorageSize());
				// Exercise an existing scene with the same pair or a different RT.
				// Its writes precede the lazy clear without a completion fence.
				if (prior && (!EnsureScene(prior == 2 ? other.get() : color_target,
						depth_target, bounds) ||
					!DrawTargetClear(initial_color, prior == 2 || color_target != nullptr,
						0.25f, depth_target != nullptr)))
					return false;
				if (rollover)
				{
					// Skip unused arena space, never overwrite in-flight geometry.
					vertex_offset = GEOMETRY_VERTEX_BYTES;
					index_offset = GEOMETRY_INDEX_BYTES - GPU_VU_SEQUENTIAL_INDEX_BYTES;
				}
				if (clears & 1)
					rt->SetClearColor(clear_color);
				if (clears & 2)
					ds->SetClearDepth(0.5f);
				const u64 start_scene = scene_serial;
				if (!EnsureScene(color_target, depth_target, requested) ||
					(draw && !DrawTargetClear(draw_color, color_target != nullptr,
						0.0f, depth_target != nullptr)))
					return false;
				scenes[variant] += scene_serial - start_scene;
				if (!Finish())
					return false;
				std::array<u32, Width * Height> result_color{};
				if (!rt->CopyToLinear(0, bounds, result_color.data(), Width * 4))
					return false;
				for (int y = 0; y < Height; y++)
				for (int x = 0; x < Width; x++)
				{
					const u32 pixel = y * Width + x;
					const bool inside = x >= clipped.x && x < clipped.z &&
						y >= clipped.y && y < clipped.w;
					const u32 expected_color = draw && color_target && inside ? draw_color :
						((clears & 1) ? clear_color : initial_color);
					const u32 z = draw && depth_target && inside ? 0 :
						((clears & 2) ? 0x3f000000u : initial_z);
					const u32 expected_depth = z | (depth_target ?
						(static_cast<u32>(inside) << 31u) : (depths[pixel] & 0x80000000u));
					u32 result_depth;
					std::memcpy(&result_depth, static_cast<const u8*>(ds->LevelData(0)) +
						y * ds->DepthPitch() + x * 4, 4);
					const u8 stencil = static_cast<const u8*>(ds->StencilData())[y * ds->StencilPitch() + x];
					if (result_color[pixel] != expected_color || result_depth != expected_depth ||
						stencil != 0x5a || (variant && (result_color[pixel] != control_color[pixel] ||
							result_depth != control_depth[pixel])))
					{
						Console.Error("GXM attachment clear validation: FAIL case=%u variant=%u "
							"attachments=%u clears=%u prior=%u rollover=%u draw=%u x=%d y=%d "
							"color=%08x expected_color=%08x depth=%08x expected_depth=%08x stencil=%02x.",
							cases, variant, attachments, clears, prior, rollover, draw, x, y,
							result_color[pixel], expected_color, result_depth, expected_depth, stencil);
						return false;
					}
					if (!variant)
					{
						control_color[pixel] = result_color[pixel];
						control_depth[pixel] = result_depth;
					}
					else
					{
						hash = (hash ^ result_color[pixel]) * UINT64_C(1099511628211);
						hash = (hash ^ result_depth) * UINT64_C(1099511628211);
						if (attachments == 3 && clears == 3 && prior == 0 && rollover == 0 && draw == 0)
							mask_hash = (mask_hash ^ (result_depth >> 31u)) * UINT64_C(1099511628211);
					}
				}
				checks++;
			}
			cases++;
		}
	}
	if (scenes[1] >= scenes[0])
		return Fail("attachment clear fixture saved no scenes", SCE_GXM_ERROR_INVALID_VALUE);
	Console.WriteLn("GXM attachment clear validation: PASS cases=%u checks=%u "
		"color_depth_hash=%016llx mask_hash=%016llx separate_scenes=%llu fused_scenes=%llu.",
		cases, checks, static_cast<unsigned long long>(hash), static_cast<unsigned long long>(mask_hash),
		static_cast<unsigned long long>(scenes[0]), static_cast<unsigned long long>(scenes[1]));
	return true;
}
#endif

bool GSDeviceGXM::Impl::RetireTextureScene(VitaGXM::GSTextureGXM& texture,
	bool preserve_contents, u64* serial)
{
	if (!serial || (preserve_contents && !EnsureTextureBackingCurrent(texture)))
		return false;

	// QueueTextureUpload() writes the texture's mapped backing directly from
	// CPU2.  A texture referenced by an outstanding scene therefore has to be
	// retired before that write, but a newly-created or already-retired texture
	// has no GPU reader to wait for.  The completion fence is the owning GXM
	// dependency record; draining the complete context for fence.scene == 0 was
	// pure serialization and made ordinary texture creation wait for unrelated
	// rendering.  PCSX2's texture update contract requires only that this
	// texture's prior contents are no longer in use, not that the whole device
	// is idle.
	if (texture.CompletionFence().scene > completed_scene_serial && !Finish())
		return false;
	*serial = completed_scene_serial;
	return true;
}

bool GSDeviceGXM::Impl::QueueTextureUpload(VitaGXM::GSTextureGXM& texture,
	u32 level, const GSVector4i& destination, u32 source_pitch,
	VitaGXM::ArenaAllocation source, u64* serial)
{
	const VitaGXM::TextureLevelLayout* layout = texture.Level(level);
	if (!source || !serial || !layout || !texture.LevelData(level))
		return false;
	const u32 bpp = texture.NativeFormat().bytes_per_pixel;
	const u32 row_bytes = destination.width() * bpp;
	if (source_pitch < row_bytes)
		return false;
	if (!texture.CopyFromLinear(level, destination, source.Data(), source_pitch))
		return false;
	if (level == 0 && texture.HasGuestRenderTargetIdentity())
		MarkTextureBackingWrite(texture);
	#if defined(VITASX2_GS_DRAW_TRACE) && VITASX2_GS_DRAW_TRACE
	VitaGsDrawTraceRecordTextureContent(texture, level, destination);
	#endif
	if (VitaPerformanceTelemetry::IsEnabled())
	{
		s_gxm_worker_performance.texture_uploads++;
		s_gxm_worker_performance.texture_upload_bytes +=
			static_cast<u64>(row_bytes) * destination.height();
	}
	*serial = ++transfer_serial;
	completed_transfer_serial = transfer_serial;
	texture.MarkTransferUse(*serial);
	source.Reset();
	return true;
}

bool GSDeviceGXM::Impl::QueueTextureReadback(VitaGXM::GSTextureGXM& texture,
	u32 level, const GSVector4i& source, void* destination, u32 destination_pitch,
	const GSVector4i& destination_rect, u64* serial)
{
	const VitaGXM::TextureLevelLayout* layout = texture.Level(level);
	if (!serial || !layout || !texture.LevelData(level) || !destination)
		return false;
	const u32 bpp = texture.NativeFormat().bytes_per_pixel;
	const u32 row_bytes = source.width() * bpp;
	if (destination_pitch < row_bytes)
		return false;
	u8* output = static_cast<u8*>(destination) +
		static_cast<size_t>(destination_rect.y) * destination_pitch +
		static_cast<size_t>(destination_rect.x) * bpp;
	if (!texture.CopyToLinear(level, source, output, destination_pitch))
		return false;
	if (VitaPerformanceTelemetry::IsEnabled())
	{
		s_gxm_worker_performance.texture_readbacks++;
		s_gxm_worker_performance.texture_readback_bytes +=
			static_cast<u64>(row_bytes) * source.height();
	}
	*serial = ++transfer_serial;
	completed_transfer_serial = transfer_serial;
	texture.MarkTransferUse(*serial);
	return true;
}

bool GSDeviceGXM::Impl::QueueTextureMipmaps(VitaGXM::GSTextureGXM& texture,
	u64* serial)
{
	if (!serial || texture.GetMipmapLevels() <= 1 ||
		texture.NativeFormat().bytes_per_pixel != 4)
	{
		return Reject("mipmap generation outside RGBA8 texture path");
	}
	for (int level = 1; level < texture.GetMipmapLevels(); level++)
	{
		const VitaGXM::TextureLevelLayout* src = texture.Level(level - 1);
		const VitaGXM::TextureLevelLayout* dst = texture.Level(level);
		if (!src || !dst)
			return false;
		for (u32 y = 0; y < dst->height; y++)
		{
			u32* out = reinterpret_cast<u32*>(static_cast<u8*>(texture.LevelData(level)) +
				static_cast<size_t>(y) * dst->pitch);
			for (u32 x = 0; x < dst->width; x++)
			{
				const u32 sx = x * 2;
				const u32 sy = y * 2;
				std::array<u32, 4> samples{};
				for (u32 iy = 0; iy < 2; iy++)
				{
					const u32* row = reinterpret_cast<const u32*>(
						static_cast<const u8*>(texture.LevelData(level - 1)) +
						static_cast<size_t>(std::min(sy + iy, src->height - 1)) * src->pitch);
					for (u32 ix = 0; ix < 2; ix++)
						samples[iy * 2 + ix] = row[std::min(sx + ix, src->width - 1)];
				}
				u32 result = 0;
				for (u32 channel = 0; channel < 4; channel++)
				{
					u32 sum = 0;
					for (u32 sample : samples)
						sum += (sample >> (channel * 8)) & 0xff;
					result |= ((sum + 2) >> 2) << (channel * 8);
				}
				out[x] = result;
			}
		}
	}
	*serial = ++transfer_serial;
	completed_transfer_serial = transfer_serial;
	texture.MarkTransferUse(*serial);
	texture.ClearMipmapGenerationFlag();
	return true;
}

bool GSDeviceGXM::Impl::CanUseFastTfx(const GSHWDrawConfig& config) const
{
	// PCSX2's blend state is already the host-GPU lowering for this family.
	// Any surviving software blend is handled by CanUseSourceOnlyTfx() or by
	// the destination-reading general program.
	if (config.ps.IsFeedbackLoopRT() || config.ps.pabe || config.ps.blend_a ||
		config.ps.blend_b || config.ps.blend_d)
	{
		return false;
	}
	return CanPatchTfxBlend(config);
}

bool GSDeviceGXM::Impl::CanUseSourceOnlyTfx(
	const GSHWDrawConfig& config) const
{
	return CanUseGsSourceOnlyTfx(config) && CanPatchTfxBlend(config);
}

bool GSDeviceGXM::Impl::CanUseGsSourceOnlyTfx(
	const GSHWDrawConfig& config) const
{
	// PCSX2 owner: tfx_fs.glsl::SW_BLEND_NEEDS_RT and
	// PSSelector::IsFeedbackLoopRT(). A software blend is source-only exactly
	// when none of A, B, C, or D selects Cd/Ad and no other shader feature needs
	// the render target. PABE's dual-source contract is retained on the general
	// path until GXM has a proven secondary-color output.
	return config.ps.IsSWBlending() && !config.ps.IsFeedbackLoopRT() &&
		!config.ps.pabe;
}

bool GSDeviceGXM::Impl::CanUseSourceDirectTfx(
	const GSHWDrawConfig& config) const
{
	// PCSX2 owner: tfx_fs.glsl::sample_c. With a 32-bit source, no manual
	// linear filtering, and no target-region remap, sample_c is one ordinary
	// texture lookup. GXM's sampler owns repeat/clamp and host linear filtering.
	return config.vs.tme && config.ps.aem_fmt == 0 && !config.ps.ltf &&
		!config.ps.region_rect && !config.ps.adjs && !config.ps.adjt &&
		config.ps.wms <= 1 && config.ps.wmt <= 1;
}

bool GSDeviceGXM::Impl::CanUseSourceDirectModulateTfx(
	const GSHWDrawConfig& config) const
{
	// PCSX2 owner: tfx_fs.glsl compile-time PS_TFX/PS_TCC selector. This is
	// exactly MODULATE with texture alpha participating in the result.
	return CanUseSourceDirectTfx(config) && config.ps.tfx == 0 && config.ps.tcc;
}

bool GSDeviceGXM::Impl::CanUseSourceDirectModulateAfTfx(
	const GSHWDrawConfig& config) const
{
	// PCSX2 owner: tfx_fs.glsl's PS selector constants. Every skipped shader
	// branch is proven here; this is the general (Cs - 0) * Af + 0 STQ path,
	// independent of title, CRC, guest PC, texture address, and draw size.
	return CanUseSourceOnlyTfx(config) &&
		CanUseSourceDirectModulateTfx(config) && !config.ps.fst &&
		!config.ps.fog && config.ps.atst == GSHWDrawConfig::PS_ATST::NONE &&
		!config.ps.fba && !config.ps.rta_source_correction &&
		!config.ps.colclip && !config.ps.blend_mix && !config.ps.fixed_one_a &&
		config.ps.blend_a == 0 && config.ps.blend_b == 2 &&
		config.ps.blend_c == 2 && config.ps.blend_d == 2;
}

bool GSDeviceGXM::Impl::CanUseZfloorSourceDirectDecalAfTfx(
	const GSHWDrawConfig& config) const
{
	// PCSX2 owner: tfx_fs.glsl's compile-time PS selector. These are every
	// branch removed by vitasx2_tfx_zfloor_source_direct_decal_af_f.cg. This
	// recognizes a mechanism family, never a title, CRC, guest PC, or asset.
	return config.ps.zfloor && CanUseGsSourceOnlyTfx(config) &&
		CanUseSourceDirectTfx(config) && !config.ps.fst &&
		config.ps.tfx == 1 && !config.ps.tcc && !config.ps.fog &&
		config.ps.atst == GSHWDrawConfig::PS_ATST::NONE &&
		!config.ps.fixed_one_a && !config.ps.fba &&
		!config.ps.rta_source_correction && config.ps.rta_correction &&
		!config.ps.colclip && config.ps.blend_mix == 1 &&
		config.ps.blend_hw == 0 &&
		config.ps.blend_a == 0 && config.ps.blend_b == 2 &&
		config.ps.blend_c == 2 && config.ps.blend_d == 2 &&
		!config.ps.fbmask && !config.ps.no_color &&
		config.colormask.wrgba == 0xf && config.blend.enable &&
		config.blend.constant_enable && config.blend.op == GSDevice::OP_ADD &&
		config.blend.src_factor == GSDevice::CONST_ONE &&
		config.blend.dst_factor == GSDevice::INV_CONST_COLOR &&
		config.blend.src_factor_alpha == GSDevice::CONST_ONE &&
		config.blend.dst_factor_alpha == GSDevice::CONST_ZERO;
}

SceGxmFragmentProgram* GSDeviceGXM::Impl::GetPatchedTfxProgram(
	const GSHWDrawConfig& config, bool source_only_fragment,
	bool source_direct_fragment, bool source_direct_modulate_fragment,
	bool source_direct_modulate_af_fragment,
	bool untextured_fragment)
{
	const u8 color_mask = config.ps.no_color ? 0 : config.colormask.wrgba;
	const GSHWDrawConfig::ColorMaskSelector mask(color_mask);
	const bool enabled = config.blend.IsEffective(mask) && config.blend.enable;
	GSHWDrawConfig::BlendState canonical_blend = config.blend;
	if (!enabled)
		canonical_blend.key = 0;
	else if (!canonical_blend.constant_enable)
		canonical_blend.constant = 0;
	const u64 key = static_cast<u64>(canonical_blend.key) |
		(static_cast<u64>(color_mask) << 32);
	auto& programs = source_only_fragment ?
		(untextured_fragment ? tfx_source_untextured_programs :
			(source_direct_modulate_af_fragment ?
				tfx_source_direct_modulate_af_programs :
			(source_direct_modulate_fragment ?
				tfx_source_direct_modulate_programs :
			(source_direct_fragment ? tfx_source_direct_programs :
				tfx_source_programs)))) :
		(untextured_fragment ? tfx_untextured_programs : tfx_fast_programs);
	const auto existing = programs.find(key);
	if (existing != programs.end())
		return existing->second;
	if (tfx_fast_programs.size() + tfx_untextured_programs.size() +
		tfx_source_programs.size() + tfx_source_direct_programs.size() +
		tfx_source_direct_modulate_programs.size() +
		tfx_source_direct_modulate_af_programs.size() +
		tfx_source_untextured_programs.size() >=
		MAX_TFX_PATCHED_PROGRAMS)
	{
		if (!tfx_patched_program_limit_logged)
		{
			tfx_patched_program_limit_logged = true;
			Console.Warning(
				"GXM GS: patched TFX program cache full; retaining exact general fallback.");
		}
		return nullptr;
	}

	SceGxmBlendInfo blend{};
	blend.colorMask = TranslateColorMask(color_mask);
	if (!enabled)
	{
		blend.colorFunc = SCE_GXM_BLEND_FUNC_NONE;
		blend.alphaFunc = SCE_GXM_BLEND_FUNC_NONE;
		blend.colorSrc = SCE_GXM_BLEND_FACTOR_ZERO;
		blend.colorDst = SCE_GXM_BLEND_FACTOR_ZERO;
		blend.alphaSrc = SCE_GXM_BLEND_FACTOR_ZERO;
		blend.alphaDst = SCE_GXM_BLEND_FACTOR_ZERO;
	}
	else
	{
		static constexpr std::array<SceGxmBlendFunc, 3> functions = {
			SCE_GXM_BLEND_FUNC_ADD,
			SCE_GXM_BLEND_FUNC_SUBTRACT,
			SCE_GXM_BLEND_FUNC_REVERSE_SUBTRACT,
		};
		blend.colorFunc = functions[config.blend.op];
		// PCSX2's GL owner always uses ADD for the separate alpha equation.
		blend.alphaFunc = SCE_GXM_BLEND_FUNC_ADD;
		SceGxmBlendFactor color_src{};
		SceGxmBlendFactor color_dst{};
		SceGxmBlendFactor alpha_src{};
		SceGxmBlendFactor alpha_dst{};
		if (!TranslateBlendFactor(config.blend.src_factor, &color_src) ||
			!TranslateBlendFactor(config.blend.dst_factor, &color_dst) ||
			!TranslateBlendFactor(config.blend.src_factor_alpha, &alpha_src) ||
			!TranslateBlendFactor(config.blend.dst_factor_alpha, &alpha_dst))
		{
			programs.emplace(key, nullptr);
			return nullptr;
		}
		blend.colorSrc = color_src;
		blend.colorDst = color_dst;
		blend.alphaSrc = alpha_src;
		blend.alphaDst = alpha_dst;
	}

	SceGxmFragmentProgram* program = nullptr;
	const SceGxmShaderPatcherId fragment_id = source_only_fragment ?
		(untextured_fragment ? tfx_source_untextured_fragment_id :
			(source_direct_modulate_af_fragment ?
				tfx_source_direct_modulate_af_fragment_id :
			(source_direct_modulate_fragment ?
				tfx_source_direct_modulate_fragment_id :
			(source_direct_fragment ? tfx_source_direct_fragment_id :
				tfx_source_fragment_id)))) :
		(untextured_fragment ? tfx_untextured_fragment_id :
			tfx_fast_fragment_id);
	// Sony api_libgxm/blending patches normalized float4 output to the target
	// UCHAR4 format. DECLARED native-color output belongs to programmable
	// blending and caused real-hardware tile corruption when combined with a
	// patcher blend state.
	const SceGxmOutputRegisterFormat output_format = source_only_fragment ?
		SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4 :
		SCE_GXM_OUTPUT_REGISTER_FORMAT_DECLARED;
	const int result = sceGxmShaderPatcherCreateFragmentProgram(patcher,
		fragment_id,
		output_format,
		SCE_GXM_MULTISAMPLE_NONE, &blend,
		&_binary_vitasx2_tfx_v_gxp_start, &program);
	if (result < 0 || !program)
	{
		Fail("create fixed-function TFX blend program", result);
		program = nullptr;
	}
	programs.emplace(key, program);
	return program;
}

bool GSDeviceGXM::Impl::WaitForTextureTransfer(u64 serial)
{
	return serial <= completed_transfer_serial || Finish();
}

void GSDeviceGXM::Impl::RetireTextureAllocation(
	VitaGXM::ArenaAllocation allocation,
	const VitaGXM::TextureCompletionFence& fence)
{
	if (!allocation)
		return;
	if (fence.scene > completed_scene_serial ||
		fence.transfer > completed_transfer_serial)
	{
		if (!Finish())
		{
			// A failed finish makes freeing unsafe. Preserve the mapping until
			// process teardown rather than exposing recycled storage to the GPU.
			Console.Error("GXM GS: retaining texture allocation after failed GPU drain.");
			quarantined_allocations.push_back(std::move(allocation));
			return;
		}
	}
	allocation.Reset();
}

bool GSDeviceGXM::Impl::UploadTfxUniforms(const GSHWDrawConfig& config,
	const GSHWDrawConfig::PSSelector& ps, VitaGXM::GSTextureGXM* source,
	bool zfloor_programmable_constant_fragment,
	bool region_repeat_fragment, bool fast_fragment,
	bool programmable_add_fragment,
	bool programmable_add_direct_fragment,
	bool programmable_over_fragment,
	bool psm16_fragment,
	bool source_only_fragment, bool source_direct_fragment,
	bool source_direct_modulate_fragment,
	bool source_direct_modulate_af_fragment,
	bool untextured_fragment,
	const GeneratedVuProgram* generated_vu,
	const VitaGpuVu::GpuVuDraw* gpu_vu_draw,
	bool no_alpha_test_fragment)
{
	const ProgramUniforms* fragment_uniforms = &uniforms;
	if (config.gs_blend.enabled)
		fragment_uniforms = &gs_blend_uniforms[SelectGsBlendProgram(config, ps)];
	else if (generated_vu &&
		generated_vu->metadata.uses_tfx_uv_no_fog_interface)
	{
		fragment_uniforms = ps.zfloor ? &uv_no_fog_zfloor_uniforms :
			(fast_fragment ? &uv_no_fog_fast_uniforms :
				&uv_no_fog_uniforms);
	}
	else if (region_repeat_fragment)
	{
		fragment_uniforms = fast_fragment ? &region_repeat_fast_uniforms :
			&region_repeat_uniforms;
	}
	else if (ps.manual_lod)
	{
		fragment_uniforms = ps.zfloor ? &manual_lod_zfloor_uniforms :
			&manual_lod_uniforms;
	}
	else if (zfloor_programmable_constant_fragment)
		fragment_uniforms = &zfloor_source_direct_decal_af_uniforms;
	else if (psm16_fragment)
		fragment_uniforms = ps.zfloor ? &psm16_zfloor_uniforms :
			&psm16_uniforms;
	else if (ps.zfloor)
		fragment_uniforms = &zfloor_uniforms;
	else if (programmable_add_direct_fragment)
		fragment_uniforms = &programmable_add_direct_uniforms;
	else if (programmable_add_fragment)
		fragment_uniforms = &programmable_add_uniforms;
	else if (programmable_over_fragment)
		fragment_uniforms = &programmable_over_uniforms;
	else if (source_only_fragment)
	{
		fragment_uniforms = untextured_fragment ? &source_untextured_uniforms :
			(source_direct_modulate_af_fragment ?
				&source_direct_modulate_af_uniforms :
			(source_direct_modulate_fragment ? &source_direct_modulate_uniforms :
			(source_direct_fragment ? &source_direct_uniforms : &source_uniforms)));
	}
	else if (untextured_fragment)
		fragment_uniforms = &untextured_uniforms;
	else if (fast_fragment)
		fragment_uniforms = no_alpha_test_fragment ? &fast_no_atst_uniforms : &fast_uniforms;
	const u64 generated_sequence = gpu_vu_draw ?
		gpu_vu_draw->ordering_sequence : 0u;
	const bool private_state_canary = generated_vu && gpu_vu_draw &&
		gpu_vu_draw->IsPrivateStateCanary() &&
		VitaGpuVu::HasGeneratedLoopKernelStateCanaryContract(generated_vu->metadata);
	if ((generated_vu && generated_vu->metadata.uses_loop_kernel_state_canary !=
			private_state_canary) ||
		(gpu_vu_draw && gpu_vu_draw->IsPrivateStateCanary() != private_state_canary) ||
		(private_state_canary && (!gpu_vu_draw->HasPrivateStoreComparison() ||
			gpu_vu_draw->HasGeneratedLoopKernelTransaction())))
	{
		return Reject("generated VU1 uniform upload changed private state purpose");
	}
	if (generated_sequence != 0u)
	{
		UpdateGpuVuPreNotificationWatchdog(
			VitaGS::GpuVuGxmSubmissionStage::VertexDefaultUniformReservation,
			generated_sequence);
	}
	void* vertex_buffer = nullptr;
	int result = 0;
	// Dense state roots use only explicit BUFFER0/1/2 bindings. Reserving a
	// nonexistent default vertex buffer is neither required nor valid for them.
	if (!private_state_canary)
	{
		result = sceGxmReserveVertexDefaultUniformBuffer(context, &vertex_buffer);
		if (result < 0 || !vertex_buffer)
			return Fail("reserve TFX vertex uniforms",
				result < 0 ? result : SCE_GXM_ERROR_INVALID_POINTER);
	}
	if (generated_sequence != 0u)
	{
		UpdateGpuVuPreNotificationWatchdog(
			VitaGS::GpuVuGxmSubmissionStage::VertexDefaultUniformUpload,
			generated_sequence);
	}
	if (generated_vu || gpu_vu_draw)
	{
		if (!generated_vu || !gpu_vu_draw)
			return Reject("incomplete generated VU1 uniform contract");
		const auto upload_vertex = [this, vertex_buffer](
			const SceGxmProgramParameter* parameter, u32 count,
			const float* values, const char* operation) {
			if (!parameter || !values)
				return false;
			const int upload_result = sceGxmSetUniformDataF(
				vertex_buffer, parameter, 0, count, values);
			return upload_result >= 0 ? true : Fail(operation, upload_result);
		};
		const std::array<float, 12> generated_transform = {
			config.cb_vs.vertex_scale.x, config.cb_vs.vertex_scale.y,
			config.cb_vs.vertex_offset.x, config.cb_vs.vertex_offset.y,
			config.cb_vs.texture_scale.x, config.cb_vs.texture_scale.y,
			config.cb_vs.texture_offset.x, config.cb_vs.texture_offset.y,
			config.cb_vs.point_size.x, config.cb_vs.point_size.y, 0.0f, 0.0f};
		const float generated_max_depth =
			static_cast<float>(config.cb_vs.max_depth);
		const u32 transform_float_count =
			generated_vu->metadata.uses_tfx_point_size ? 12u : 8u;
		if (!private_state_canary &&
			(!upload_vertex(generated_vu->uniforms.vertex_scale_offset,
				transform_float_count,
				generated_transform.data(),
				"upload generated VU1+TFX transform") ||
			!upload_vertex(generated_vu->uniforms.max_depth, 1,
				&generated_max_depth,
				"upload generated VU1+TFX maximum depth")))
		{
			return false;
		}
		if (!generated_vu->metadata.uses_buffered_batch_inputs &&
			!generated_vu->metadata.UsesStructuredSnapshotInputBuffers())
		{
			for (const VitaGpuVu::ConstantUniform& uniform :
				gpu_vu_draw->ConstantUniforms())
			{
				std::array<float, 4> values;
				std::memcpy(values.data(), uniform.bits.data(), sizeof(values));
				if (uniform.input_index >=
						generated_vu->uniforms.constants.size() ||
					!upload_vertex(
						generated_vu->uniforms.constants[uniform.input_index],
						values.size(), values.data(),
						"upload generated VU1 constant uniform"))
				{
					return false;
				}
			}
			for (const VitaGpuVu::VectorUniform& uniform :
				gpu_vu_draw->VfUniforms())
			{
				std::array<float, 4> values;
				std::memcpy(values.data(), uniform.bits.data(), sizeof(values));
				if (!upload_vertex(
						generated_vu->uniforms.vf[uniform.register_index],
						values.size(), values.data(),
						"upload generated VU1 VF uniform"))
				{
					return false;
				}
			}
			if (generated_vu->metadata.uses_acc_uniform)
			{
				std::array<float, 4> values;
				std::memcpy(values.data(), gpu_vu_draw->acc_uniform.data(),
					sizeof(values));
				if (!upload_vertex(generated_vu->uniforms.acc, values.size(),
						values.data(), "upload generated VU1 ACC uniform"))
				{
					return false;
				}
			}
			const auto upload_scalar = [&upload_vertex](
				bool used, const SceGxmProgramParameter* parameter, u32 bits,
				const char* operation) {
				if (!used)
					return true;
				float value;
				std::memcpy(&value, &bits, sizeof(value));
				return upload_vertex(parameter, 1, &value, operation);
			};
			const u32 scalar_mask = gpu_vu_draw->scalar_uniforms.present;
			if (!upload_scalar(generated_vu->metadata.uses_q_uniform,
					generated_vu->uniforms.q, gpu_vu_draw->scalar_uniforms.q,
					"upload generated VU1 Q uniform") ||
				!upload_scalar(generated_vu->metadata.uses_p_uniform,
					generated_vu->uniforms.p, gpu_vu_draw->scalar_uniforms.p,
					"upload generated VU1 P uniform") ||
				!upload_scalar(generated_vu->metadata.uses_i_uniform,
					generated_vu->uniforms.i, gpu_vu_draw->scalar_uniforms.i,
					"upload generated VU1 I uniform") ||
				!upload_scalar(generated_vu->metadata.uses_gif_q_uniform,
					generated_vu->uniforms.gif_q,
					gpu_vu_draw->scalar_uniforms.gif_q,
					"upload generated GIF Q uniform") ||
				((scalar_mask & VitaGpuVu::ScalarUniformQ) != 0) !=
					generated_vu->metadata.uses_q_uniform ||
				((scalar_mask & VitaGpuVu::ScalarUniformP) != 0) !=
					generated_vu->metadata.uses_p_uniform ||
				((scalar_mask & VitaGpuVu::ScalarUniformI) != 0) !=
					generated_vu->metadata.uses_i_uniform ||
				((scalar_mask & VitaGpuVu::ScalarUniformGifQ) != 0) !=
					generated_vu->metadata.uses_gif_q_uniform)
			{
				return Reject("generated VU1 scalar-uniform mask mismatch");
			}
		}
	}
	else
	{
		// Keep PCSX2's point-size member in the same GXM uniform-array upload as
		// the ordinary transform. A separate scalar upload would add a libGXM
		// call to every triangle draw merely because the shared vertex program
		// emits PSIZE.
		const std::array<float, 12> vertex_values = {
			config.cb_vs.vertex_scale.x, config.cb_vs.vertex_scale.y,
			config.cb_vs.vertex_offset.x, config.cb_vs.vertex_offset.y,
			config.cb_vs.texture_scale.x, config.cb_vs.texture_scale.y,
			config.cb_vs.texture_offset.x, config.cb_vs.texture_offset.y,
			config.cb_vs.point_size.x, config.cb_vs.point_size.y, 0.0f, 0.0f};
		result = sceGxmSetUniformDataF(vertex_buffer,
			uniforms.vertex_scale_offset, 0, vertex_values.size(),
			vertex_values.data());
		if (result < 0)
			return Fail("upload TFX vertex uniforms", result);
	}
	if (generated_sequence != 0u)
	{
		UpdateGpuVuPreNotificationWatchdog(
			VitaGS::GpuVuGxmSubmissionStage::FragmentDefaultUniformReservation,
			generated_sequence);
	}
	void* fragment_buffer = nullptr;
	result = sceGxmReserveFragmentDefaultUniformBuffer(context, &fragment_buffer);
	if (result < 0 || !fragment_buffer)
		return Fail("reserve TFX fragment uniforms",
			result < 0 ? result : SCE_GXM_ERROR_INVALID_POINTER);
	if (generated_sequence != 0u)
	{
		UpdateGpuVuPreNotificationWatchdog(
			VitaGS::GpuVuGxmSubmissionStage::FragmentDefaultUniformUpload,
			generated_sequence);
	}
	const auto upload4 = [this, fragment_buffer](
		const SceGxmProgramParameter* parameter, const float* values,
		const char* name) {
		if (!parameter)
			return true;
		const int upload_result =
			sceGxmSetUniformDataF(fragment_buffer, parameter, 0, 4, values);
		return upload_result >= 0 ? true : Fail(name, upload_result);
	};
	if (zfloor_programmable_constant_fragment)
	{
		// Every other selector and constant is compiled out of this GXP. Avoid
		// rebuilding 28 selector floats and touching source dimensions/FBMASK on
		// the Cortex-A9 for each draw.
		const float st_scale[4] = {
			config.cb_ps.STScale.x, config.cb_ps.STScale.y, 0.0f, 0.0f};
		const float blend_constant[4] = {
			0.0f, 0.0f,
			std::min(static_cast<float>(config.blend.constant) / 128.0f, 1.0f),
			0.0f};
		return upload4(fragment_uniforms->texture_alpha,
				config.cb_ps.TA_MaxDepth_Af.F32, "upload texture alpha") &&
			upload4(fragment_uniforms->st_scale, st_scale, "upload ST scale") &&
			upload4(fragment_uniforms->hardware_blend[1], blend_constant,
				"upload constant blend");
	}
	float selectors[7][4] = {
		{static_cast<float>(config.vs.tme), static_cast<float>(ps.fst),
			static_cast<float>(ps.tfx), static_cast<float>(ps.tcc)},
		{static_cast<float>(ps.fog), static_cast<float>(ps.atst),
			static_cast<float>(ps.fba), static_cast<float>(ps.dst_fmt)},
		{static_cast<float>(ps.aem), static_cast<float>(ps.aem_fmt),
			static_cast<float>(ps.ltf), static_cast<float>(ps.rta_source_correction)},
		{static_cast<float>(ps.blend_a), static_cast<float>(ps.blend_b),
			static_cast<float>(ps.blend_c), static_cast<float>(ps.blend_d)},
		{static_cast<float>(ps.pabe), static_cast<float>(ps.rta_correction),
			static_cast<float>(ps.colclip), static_cast<float>(ps.fbmask)},
		{static_cast<float>(ps.date), static_cast<float>(ps.no_color),
			static_cast<float>(ps.blend_mix), static_cast<float>(ps.blend_hw)},
		{static_cast<float>(ps.region_rect), static_cast<float>(ps.wms),
			static_cast<float>(ps.wmt), static_cast<float>(ps.fixed_one_a)},
	};
	if (config.gs_blend.enabled)
	{
		// The native semantic value is authoritative, not any later desktop
		// selector rewrites. The existing upload shape is migration glue only.
		const auto& op = config.gs_blend;
		selectors[3][0] = op.a; selectors[3][1] = op.b;
		selectors[3][2] = op.c; selectors[3][3] = op.d;
		selectors[4][0] = op.pabe ? 1.0f : 0.0f;
		selectors[4][1] = 0.0f;
		selectors[4][2] = op.clamp ? 0.0f : 1.0f;
	}
	for (u32 i = 0; i < 7; i++)
	{
		if (!fragment_uniforms->selector[i])
			continue;
		result = sceGxmSetUniformDataF(fragment_buffer,
			fragment_uniforms->selector[i], 0, 4, selectors[i]);
		if (result < 0)
			return Fail("upload TFX selector", result);
	}
	if (psm16_fragment)
	{
		const float selector7[4] = {
			static_cast<float>(ps.dither),
			static_cast<float>(ps.dither_adjust),
			static_cast<float>(ps.round_inv), config.cb_ps.ScaleFactor.y};
		if (!upload4(fragment_uniforms->selector7, selector7,
				"upload PSMCT16 selector"))
		{
			return false;
		}
		for (u32 i = 0; i < 4; i++)
		{
			if (!upload4(fragment_uniforms->dither_matrix[i],
					config.cb_ps.DitherMatrix[i].F32,
					"upload PSMCT16 dither row"))
			{
				return false;
			}
		}
	}
	GSVector4 texture_alpha = config.cb_ps.TA_MaxDepth_Af;
	if (config.gs_blend.enabled)
		texture_alpha.a = config.gs_blend.fix / 128.0f;
	if (!upload4(fragment_uniforms->fog_color_aref,
			config.cb_ps.FogColor_AREF.F32,
			"upload FogColor_AREF") ||
		!upload4(fragment_uniforms->texture_size, config.cb_ps.WH.F32,
			"upload texture size") ||
		!upload4(fragment_uniforms->texture_alpha,
			texture_alpha.F32,
			"upload texture alpha") ||
		!upload4(fragment_uniforms->half_texel, config.cb_ps.HalfTexel.F32,
			"upload half texel") ||
		!upload4(fragment_uniforms->st_range, config.cb_ps.STRange.F32,
			"upload ST range"))
	{
		return false;
	}
	// STScale.zw are uniform equivalents of PCSX2's PS_ADJS/PS_ADJT compile
	// selectors. The general GXP implements tfx_fs.glsl::sample_c exactly;
	// direct-texture specializations are excluded by CanUseSourceDirectTfx().
	const float st_scale[4] = {config.cb_ps.STScale.x, config.cb_ps.STScale.y,
		static_cast<float>(ps.adjs), static_cast<float>(ps.adjt)};
	if (!upload4(fragment_uniforms->st_scale, st_scale, "upload ST scale"))
		return false;
	const float region_mask_fix[4] = {
		static_cast<float>(config.cb_ps.MinMax.U32[0]),
		static_cast<float>(config.cb_ps.MinMax.U32[1]),
		static_cast<float>(config.cb_ps.MinMax.U32[2]),
		static_cast<float>(config.cb_ps.MinMax.U32[3])};
	if (!upload4(fragment_uniforms->region_mask_fix, region_mask_fix,
			"upload REGION_REPEAT mask/fix"))
	{
		return false;
	}
	if (!upload4(fragment_uniforms->lod_params, config.cb_ps.LODParams.F32,
			"upload manual LOD parameters"))
	{
		return false;
	}
	const VitaGXM::GSTextureGXM* native_source = source ? source : white_texture.get();
	const float native_size[4] = {
		static_cast<float>(native_source->GetWidth()),
		static_cast<float>(native_source->GetHeight()),
		1.0f / static_cast<float>(native_source->GetWidth()),
		1.0f / static_cast<float>(native_source->GetHeight())};
	if (!upload4(fragment_uniforms->native_texture_size, native_size,
			"upload native texture size"))
	{
		return false;
	}
	const float fb_mask[4] = {
		static_cast<float>(config.cb_ps.FbMask.x),
		static_cast<float>(config.cb_ps.FbMask.y),
		static_cast<float>(config.cb_ps.FbMask.z),
		static_cast<float>(config.cb_ps.FbMask.w)};
	if (!upload4(fragment_uniforms->fb_mask, fb_mask,
			"upload framebuffer mask"))
		return false;
	const bool blend_enabled = config.blend.enable;
	const float blend0[4] = {blend_enabled ? 1.0f : 0.0f,
		static_cast<float>(config.blend.op),
		static_cast<float>(config.blend.src_factor),
		static_cast<float>(config.blend.dst_factor)};
	const float blend1[4] = {
		static_cast<float>(config.blend.src_factor_alpha),
		static_cast<float>(config.blend.dst_factor_alpha),
		std::min(static_cast<float>(config.blend.constant) / 128.0f, 1.0f),
		0.0f};
	if (!upload4(fragment_uniforms->hardware_blend[0], blend0,
			"upload hardware blend 0") ||
		!upload4(fragment_uniforms->hardware_blend[1], blend1,
			"upload hardware blend 1"))
	{
		return false;
	}
	const float color_mask[4] = {
		config.colormask.wr ? 1.0f : 0.0f,
		config.colormask.wg ? 1.0f : 0.0f,
		config.colormask.wb ? 1.0f : 0.0f,
		config.colormask.wa ? 1.0f : 0.0f};
	return upload4(fragment_uniforms->color_mask, color_mask,
		"upload color mask");
}

VitaGXM::GSTextureGXM* GSDeviceGXM::Impl::SnapshotTexture(
	VitaGXM::GSTextureGXM& source, const GSVector4i& requested_area)
{
	if (!CommitClear(source) || !Finish())
		return nullptr;
	const GSVector4i area = requested_area.rintersect(source.GetRect());
	if (area.rempty() || source.GetFormat() != GSTexture::Format::Color)
	{
		Reject("non-color or empty render-target texture snapshot");
		return nullptr;
	}
	const std::pair<int, int> key(source.GetWidth(), source.GetHeight());
	auto it = feedback_textures.find(key);
	if (it == feedback_textures.end())
	{
		int error = 0;
		auto texture = VitaGXM::GSTextureGXM::Create(this, GSTexture::Texture,
			source.GetWidth(), source.GetHeight(), 1, source.GetFormat(), &error);
		if (!texture)
		{
			Fail("create render-target feedback snapshot", error);
			return nullptr;
		}
		it = feedback_textures.emplace(key, std::move(texture)).first;
	}
	VitaGXM::GSTextureGXM& destination = *it->second;
	const VitaGXM::TextureLevelLayout* dst_layout = destination.Level(0);
	if (!source.Level(0) || !dst_layout)
		return nullptr;
	u8* dst = static_cast<u8*>(destination.LevelData(0)) +
		static_cast<size_t>(area.y) * dst_layout->pitch + area.x * 4;
	if (!source.CopyToLinear(0, area, dst, dst_layout->pitch))
		return nullptr;
	#if defined(VITASX2_GS_DRAW_TRACE) && VITASX2_GS_DRAW_TRACE
	VitaGsDrawTraceRecordTextureContent(destination, 0, area);
	#endif
	destination.SetState(GSTexture::State::Dirty);
	if (VitaPerformanceTelemetry::IsEnabled())
	{
		s_gxm_worker_performance.feedback_snapshots++;
		s_gxm_worker_performance.feedback_snapshot_bytes +=
			static_cast<u64>(area.width()) * static_cast<u64>(area.height()) * 4u;
	}
	return &destination;
}

bool GSDeviceGXM::Impl::StageAndDraw(const GSHWDrawConfig& config,
	const GSHWDrawConfig::PSSelector& ps, u32 first_index, u32 index_count,
	VitaGXM::GSTextureGXM* source, SceGxmFragmentProgram* fragment,
	bool region_repeat_fragment, bool fast_fragment,
	bool programmable_add_fragment,
	bool programmable_add_direct_fragment,
	bool programmable_over_fragment,
	bool psm16_fragment,
	bool source_only_fragment, bool source_direct_fragment,
	bool source_direct_modulate_fragment,
	bool source_direct_modulate_af_fragment,
	bool untextured_fragment)
{
	if (!config.verts || !config.indices || config.nverts == 0 || index_count == 0 ||
		first_index > config.nindices || index_count > config.nindices - first_index ||
		config.indices_per_prim == 0 ||
		(index_count % config.indices_per_prim) != 0 || index_count > MAX_STAGED_INDICES)
	{
		return Reject("invalid indexed hardware draw geometry");
	}
	VitaGXM::GSTextureGXM* const rt =
		CheckedCast<VitaGXM::GSTextureGXM>(config.rt);
	VitaGXM::GSTextureGXM* const ds =
		CheckedCast<VitaGXM::GSTextureGXM>(config.ds);
	VitaGXM::GSTextureGXM* draw_rt = rt;
	VitaGXM::GSTextureGXM* draw_ds = ds;

	// PCSX2 owner: GSDeviceOGL::RenderHW() keeps a size-compatible attachment
	// when the next draw omits only RT or DS, avoiding a framebuffer transition.
	// GXM scenes have immutable attachments, so this avoids an EndScene/BeginScene
	// pair. Unlike desktop GL, GXM cannot sample an attached surface; also require
	// the omitted attachment's writes to be disabled before retaining it.
	if (scene_active && !scene_is_display)
	{
		if (!draw_rt && draw_ds && scene_rt && source != scene_rt &&
			(config.ps.no_color || config.colormask.wrgba == 0) &&
			scene_rt->GetSize() == draw_ds->GetSize())
		{
			draw_rt = scene_rt;
		}
		else if (!draw_ds && draw_rt && scene_ds && source != scene_ds &&
			!config.depth.zwe && config.depth.ztst == ZTST_ALWAYS &&
			scene_ds->GetSize() == draw_rt->GetSize())
		{
			draw_ds = scene_ds;
		}
	}
	if (!EnsureScene(draw_rt, draw_ds, config.scissor))
		return false;
	void* vertex_memory = nullptr;
	u16* staged_indices = nullptr;
	if (!ReserveGeometry(index_count, index_count, &vertex_memory, &staged_indices))
	{
		if (!Finish() || !EnsureScene(draw_rt, draw_ds, config.scissor) ||
			!ReserveGeometry(index_count, index_count, &vertex_memory, &staged_indices))
		{
			return Reject("hardware draw exceeds GXM geometry staging capacity");
		}
	}
	TfxVertex* staged_vertices = static_cast<TfxVertex*>(vertex_memory);
	for (u32 i = 0; i < index_count; i++)
	{
		const u32 source_index = config.indices[first_index + i];
		if (source_index >= config.nverts)
			return Reject("hardware draw index exceeds vertex count");
		const u32 primitive_first = first_index +
			(i / config.indices_per_prim) * config.indices_per_prim;
		// PCSX2's GS uses the last vertex as the provoking vertex. GXM has no
		// selectable provoking-vertex state, so the device-owned staging path
		// makes flat shading explicit by assigning the last vertex's color to
		// every staged vertex in the primitive. This also keeps de-indexing inside
		// the bounded MAX_STAGED_INDICES chunks instead of invoking
		// GSRendererHW::HandleProvokingVertexFirst() on the complete draw.
		const u32 color_index = config.vs.iip ? source_index :
			static_cast<u32>(config.indices[primitive_first +
				config.indices_per_prim - 1]);
		if (color_index >= config.nverts)
			return Reject("flat-shading provoking index exceeds vertex count");
		const GSVertex& vertex = config.verts[source_index];
		const GSVertex& color_vertex = config.verts[color_index];
		TfxVertex& output = staged_vertices[i];
		output.st[0] = vertex.ST.S;
		output.st[1] = vertex.ST.T;
		output.rgba[0] = color_vertex.RGBAQ.R;
		output.rgba[1] = color_vertex.RGBAQ.G;
		output.rgba[2] = color_vertex.RGBAQ.B;
		output.rgba[3] = color_vertex.RGBAQ.A;
		output.q = vertex.RGBAQ.Q;
		output.position[0] = static_cast<float>(vertex.XYZ.X);
		output.position[1] = static_cast<float>(vertex.XYZ.Y);
		output.depth = static_cast<float>(
			std::min<u32>(vertex.XYZ.Z, config.cb_vs.max_depth));
		output.uv[0] = static_cast<float>(vertex.U);
		output.uv[1] = static_cast<float>(vertex.V);
		std::memcpy(output.fog, &vertex.FOG, sizeof(output.fog));
		staged_indices[i] = static_cast<u16>(i);
	}

	sceGxmSetVertexProgram(context, tfx_vertex_program);
	sceGxmSetFragmentProgram(context, fragment);
	sceGxmSetVertexStream(context, 0, staged_vertices);

	VitaGXM::GSTextureGXM* bound_source = source ? source : white_texture.get();
	if (!bound_source)
		return false;
	SceGxmTexture& native_texture = bound_source->Texture();
	const bool linear_strided =
		sceGxmTextureGetType(&native_texture) == SCE_GXM_TEXTURE_LINEAR_STRIDED;
	if (linear_strided && (config.sampler.tau || config.sampler.tav))
		return Reject("repeat addressing on a linear-strided GXM texture");
	if (bound_source->GetMipmapLevels() > 1 &&
		!ps.automatic_lod && !ps.manual_lod)
	{
		return Reject("multi-level texture without an explicit or implicit LOD contract");
	}
	if (bound_source->GetMipmapLevels() > 1 && config.sampler.lodclamp)
	{
		// GXM's mip-filter disable bit selects nearest-mip sampling; it does not
		// force level zero. PCSX2's lodclamp contract therefore needs an explicit
		// tex2Dlod(0) shader path before multi-level textures can use it.
		return Reject("LOD0 clamp on a multi-level GXM texture");
	}
	int result = sceGxmTextureSetUAddrMode(&native_texture,
		config.sampler.tau ? SCE_GXM_TEXTURE_ADDR_REPEAT : SCE_GXM_TEXTURE_ADDR_CLAMP);
	if (result >= 0)
	{
		result = sceGxmTextureSetVAddrMode(&native_texture,
			config.sampler.tav ? SCE_GXM_TEXTURE_ADDR_REPEAT : SCE_GXM_TEXTURE_ADDR_CLAMP);
	}
	const SceGxmTextureFilter min_filter = (!ps.ltf &&
		config.sampler.IsMinFilterLinear()) ? SCE_GXM_TEXTURE_FILTER_LINEAR :
		SCE_GXM_TEXTURE_FILTER_POINT;
	const SceGxmTextureFilter mag_filter = (!ps.ltf &&
		config.sampler.IsMagFilterLinear()) ? SCE_GXM_TEXTURE_FILTER_LINEAR :
		SCE_GXM_TEXTURE_FILTER_POINT;
	if (result >= 0 && !linear_strided)
		result = sceGxmTextureSetMinFilter(&native_texture, min_filter);
	if (result >= 0)
		result = sceGxmTextureSetMagFilter(&native_texture, mag_filter);
	if (result >= 0 && !linear_strided)
	{
		result = sceGxmTextureSetMipFilter(&native_texture,
			config.sampler.IsMipFilterLinear() ?
			SCE_GXM_TEXTURE_MIP_FILTER_ENABLED :
			SCE_GXM_TEXTURE_MIP_FILTER_DISABLED);
	}
	if (result < 0)
		return Fail("configure TFX sampler", result);
	result = sceGxmSetFragmentTexture(context, 0, &native_texture);
	if (result < 0)
		return Fail("bind TFX source texture", result);
	bound_source->MarkSceneUse(scene_serial);

	const SceGxmDepthFunc depth_func = TranslateDepthFunc(config.depth.ztst);
	sceGxmSetFrontDepthFunc(context, depth_func);
	sceGxmSetBackDepthFunc(context, depth_func);
	const SceGxmDepthWriteMode depth_write = config.depth.zwe ?
		SCE_GXM_DEPTH_WRITE_ENABLED : SCE_GXM_DEPTH_WRITE_DISABLED;
	sceGxmSetFrontDepthWriteEnable(context, depth_write);
	sceGxmSetBackDepthWriteEnable(context, depth_write);
	#if defined(VITASX2_GS_DRAW_TRACE) && VITASX2_GS_DRAW_TRACE
	const u32 backend_sampler =
		(static_cast<u32>(config.sampler.tau) << 0) |
		(static_cast<u32>(config.sampler.tav) << 1) |
		(static_cast<u32>(min_filter == SCE_GXM_TEXTURE_FILTER_LINEAR) << 2) |
		(static_cast<u32>(mag_filter == SCE_GXM_TEXTURE_FILTER_LINEAR) << 3) |
		(static_cast<u32>(config.sampler.IsMipFilterLinear()) << 4) |
		(static_cast<u32>(linear_strided) << 5) |
		(static_cast<u32>(config.sampler.lodclamp) << 6) |
		(static_cast<u32>(bound_source->GetMipmapLevels()) << 8);
	const u32 backend_depth = static_cast<u32>(depth_func) |
		(static_cast<u32>(depth_write) << 8);
	const u32 backend_raster = static_cast<u32>(TranslateTopology(config.topology)) |
		(static_cast<u32>(
			config.topology == GSHWDrawConfig::Topology::Point ? 1u :
			(config.topology == GSHWDrawConfig::Topology::Line ? 2u : 0u)) << 8);
	VitaGsDrawTraceRecordBackendState(config, *bound_source,
		staged_vertices, index_count * sizeof(TfxVertex),
		staged_indices, index_count * sizeof(u16), index_count,
		backend_sampler, backend_depth, backend_raster,
		TranslateColorMask(config.ps.no_color ? 0 : config.colormask.wrgba));
	#endif
	if (!UploadTfxUniforms(config, ps, source,
			fragment == tfx_zfloor_source_direct_decal_af_program,
			region_repeat_fragment, fast_fragment,
			programmable_add_fragment,
			programmable_add_direct_fragment,
			programmable_over_fragment,
			psm16_fragment,
			source_only_fragment, source_direct_fragment,
			source_direct_modulate_fragment,
			source_direct_modulate_af_fragment, untextured_fragment,
			nullptr, nullptr, fragment == tfx_fast_no_atst_program))
		return false;

	// PCSX2 owner: GSRendererHW::SetupIA() keeps native points as one index per
	// primitive and supplies cb_vs.point_size. Sony's libGXM draw contract
	// requires SCE_GXM_PRIMITIVE_POINTS to use a vertex program with PSIZE; its
	// point samples and vitaGL additionally select POINT_01UV polygon mode.
	// Lines retain their native PCSX2 list assembly and GXM LINE polygon mode.
	// Restore triangle fill after either draw so later triangles in this scene
	// cannot inherit the primitive-specific raster state.
	const bool point_topology =
		config.topology == GSHWDrawConfig::Topology::Point;
	const bool line_topology = config.topology == GSHWDrawConfig::Topology::Line;
	if (point_topology)
	{
		sceGxmSetFrontPolygonMode(context, SCE_GXM_POLYGON_MODE_POINT_01UV);
		sceGxmSetBackPolygonMode(context, SCE_GXM_POLYGON_MODE_POINT_01UV);
	}
	else if (line_topology)
	{
		sceGxmSetFrontPolygonMode(context, SCE_GXM_POLYGON_MODE_LINE);
		sceGxmSetBackPolygonMode(context, SCE_GXM_POLYGON_MODE_LINE);
		sceGxmSetFrontPointLineWidth(context, 1);
		sceGxmSetBackPointLineWidth(context, 1);
	}
	VitaGS::GpuVuGxmCallBreadcrumb call_breadcrumb;
	call_breadcrumb.index_count = index_count;
	const u32 call_token = gpu_vu_last_generated_sequence != 0u ?
		VitaGS::BeginGpuVuGxmCall(VitaGS::GpuVuGxmCallKind::OrdinaryTfxDraw,
			gpu_vu_last_generated_sequence, scene_serial, call_breadcrumb) : 0u;
	result = TimeGxmHostCall(VitaGxmHostCall::DrawTfx, [&] {
		return sceGxmDraw(context, TranslateTopology(config.topology),
			SCE_GXM_INDEX_FORMAT_U16, staged_indices, index_count);
	});
	VitaGS::CompleteGpuVuGxmCall(call_token, result);
	if (point_topology || line_topology)
	{
		sceGxmSetFrontPolygonMode(context, SCE_GXM_POLYGON_MODE_TRIANGLE_FILL);
		sceGxmSetBackPolygonMode(context, SCE_GXM_POLYGON_MODE_TRIANGLE_FILL);
	}
	if (result < 0)
		return Fail("sceGxmDraw(TFX)", result);
	if (VitaPerformanceTelemetry::IsEnabled())
	{
		scene_ordinary_content |= 1;
		auto& stats = s_gxm_worker_performance.tfx_programs[IdentifyTfxProgram(fragment)];
		stats.draws++;
		stats.indices += index_count;
		const GSVector4i clipped = config.drawarea.rintersect(scene_scissor);
		const u64 pixels = clipped.rempty() ? 0 :
			static_cast<u64>(clipped.width()) * clipped.height();
		stats.draw_rect_pixels += pixels;
		if (ps.atst == GSHWDrawConfig::PS_ATST::NONE && ps.date == 0 && !ps.zfloor)
		{
			stats.no_tests_draws++;
			stats.no_tests_rect_pixels += pixels;
		}
	}
	RecordGxmDraw(index_count, sizeof(TfxVertex), index_count);
	if (rt)
		rt->SetState(GSTexture::State::Dirty);
	if (ds && config.depth.zwe)
		ds->SetState(GSTexture::State::Dirty);
	return true;
}

bool GSDeviceGXM::Impl::DrawGpuVu(const GSHWDrawConfig& config,
	VitaGXM::GSTextureGXM* source, bool fast_fragment, bool psm16_fragment,
	bool region_repeat_fragment, bool manual_lod_fragment)
{
	if (!active_gpu_vu_draws || active_gpu_vu_draws->empty() ||
		active_gpu_vu_draw_encoded)
	{
		return Reject("missing or already encoded GPU-VU draw descriptor");
	}
	const VitaGpuVu::GpuVuDraw* const draw =
		active_gpu_vu_draws->front().get();
	if (!draw)
		return Reject("GPU-VU batch starts with a null descriptor");
	if (!draw->WasValidatedForQueue())
		return Reject("GPU-VU batch starts with an unvalidated descriptor");
	if (draw->UsesQuarantinedSnapshotArchitecture())
	{
		return Reject(
			"quarantined GPU-VU snapshot/precompute descriptor reached GXM");
	}
	if (draw->lowering != VitaGpuVu::OutputLowering::DirectTfx ||
		draw->execution != VitaGpuVu::ExecutionKind::GeneratedParallel)
	{
		return Reject("GPU-VU draw is not a generated direct-TFX job");
	}
	const bool generated_final_state_owner =
		draw->HasPrivateStoreComparison() ||
		draw->HasGeneratedLoopKernelTransaction();
	if (draw->final_state.IsRequired() && !generated_final_state_owner)
	{
		return Reject(
			"native GPU-VU draw requires unimplemented final-state publication");
	}
	if (psm16_fragment || region_repeat_fragment || manual_lod_fragment)
	{
		return Reject(
			"generated VU1 root lacks the selected specialized TFX fragment link");
	}
	if (!gpu_vu_retirements_ready || !gpu_vu_sequential_indices)
		return Reject("GPU-VU draw retirement or identity indices unavailable");

	// Preflight the complete CPU ownership footprint before changing GXM state.
	// Every accepted descriptor can contribute at most one private completion
	// record and one buffered input group can retain at most four allocations.
	// The vectors were reserved at initialization; crossing either bound must be
	// a pre-effect MTVU fallback, never an implicit std::vector growth after a
	// draw has been encoded.
	size_t required_private_outputs = 0u;
	for (const auto& candidate : *active_gpu_vu_draws)
	{
		if (candidate && (candidate->HasPrivateStoreJournal() ||
				candidate->HasGeneratedLoopKernelTransaction()))
		{
			required_private_outputs++;
		}
	}
	if (gpu_vu_scene_private_store_outputs.size() >
			GPU_VU_RETIREMENT_PRIVATE_OUTPUT_CAPACITY ||
		required_private_outputs >
			GPU_VU_RETIREMENT_PRIVATE_OUTPUT_CAPACITY -
				gpu_vu_scene_private_store_outputs.size())
	{
		return Reject(
			"GPU-VU private retirement fixed capacity would be exceeded");
	}
	const size_t required_batch_allocations =
		1u + 4u * active_gpu_vu_draws->size();
	if (gpu_vu_scene_batch_allocations.size() >
			GPU_VU_RETIREMENT_BATCH_ALLOCATION_CAPACITY ||
		required_batch_allocations >
			GPU_VU_RETIREMENT_BATCH_ALLOCATION_CAPACITY -
				gpu_vu_scene_batch_allocations.size())
	{
		return Reject(
			"GPU-VU batch retirement fixed capacity would be exceeded");
	}

	GeneratedVuProgram* const generated =
		FindGeneratedVuProgram(draw->program);
	if (!generated || !generated->registration_complete ||
		generated->key != draw->program || !generated->vertex_program ||
		!generated->general_fragment_program ||
		!generated->zfloor_fragment_program ||
		!generated->opaque_fragment_program)
	{
		return Reject("generated VU1+TFX program is not GS-ready");
	}
	const bool private_state_canary =
		VitaGpuVu::HasGeneratedLoopKernelStateCanaryContract(generated->metadata);
	if (generated->metadata.uses_loop_kernel_state_canary != private_state_canary ||
		draw->IsPrivateStateCanary() != private_state_canary ||
		(private_state_canary && (!draw->HasPrivateStoreComparison() ||
			draw->HasGeneratedLoopKernelTransaction() || draw->HasExactIndices())))
	{
		return Reject("generated VU1 state canary escaped its private dense domain");
	}
	if ((!generated->metadata.uses_tfx_uniforms && !private_state_canary) ||
		generated->metadata.memory_inputs.size() != draw->streams.size() ||
		generated->metadata.constant_inputs.size() !=
			draw->ConstantUniforms().size())
	{
		return Reject(
			"generated VU1 program metadata differs from draw inputs");
	}
	const bool structured_direct =
		generated->metadata.execution_kind ==
			VitaGpuVu::GeneratedCgExecutionKind::
				StructuredParallelDirectVuTfx;
	const bool loop_kernel_direct =
		generated->metadata.execution_kind ==
			VitaGpuVu::GeneratedCgExecutionKind::
				GeneratedLoopKernelDirectVuTfx;
	const bool validation_private_scratch = loop_kernel_direct &&
		draw->HasPrivateStoreComparison() &&
		generated->resource_attestation ==
			VitaGpuVu::GeneratedGxpResourceAttestation::ScratchSpill;
	if (generated->resource_attestation !=
			VitaGpuVu::GeneratedGxpResourceAttestation::Accepted &&
		!validation_private_scratch)
	{
		return Reject(
			"generated VU1 executable lacks its registered resource attestation");
	}
	if (loop_kernel_direct)
	{
		if (!draw->HasGeneratedLoopKernelAttestation())
			return Reject("generated loop-kernel has no semantic attestation key");
		const VitaGpuVu::GeneratedLoopKernelAttestationRecord attestation =
			VitaGpuVu::QueryGeneratedLoopKernelAttestation(
				draw->GeneratedLoopKernelAttestation());
		const VitaGpuVu::GeneratedLoopKernelAttestationState required_state =
			draw->HasPrivateStoreComparison() ?
				VitaGpuVu::GeneratedLoopKernelAttestationState::PrivateTest :
				VitaGpuVu::GeneratedLoopKernelAttestationState::Product;
		if (attestation.state != required_state)
			return Reject(
				"generated loop-kernel semantic attestation changed before GXM");
		if (!draw->HasPrivateStoreComparison() &&
			!draw->HasGeneratedLoopKernelTransaction())
		{
			return Reject(
				"generated loop-kernel product has no transactional state owner");
		}
	}
	if (structured_direct != (draw->StructuredDirectInput() != nullptr))
	{
		return Reject(
			"generated VU1 structured input differs from the registered root");
	}
	using StructuredScratchMask =
		VitaGpuVu::StructuredGeneratedScratchMask;
	const auto mask_contains = [](const StructuredScratchMask& superset,
			const StructuredScratchMask& subset) {
		for (u32 word = 0; word < superset.size(); word++)
		{
			if ((subset[word] & ~superset[word]) != 0u)
				return false;
		}
		return true;
	};
	const auto mask_intersects = [](const StructuredScratchMask& left,
			const StructuredScratchMask& right) {
		for (u32 word = 0; word < left.size(); word++)
		{
			if ((left[word] & right[word]) != 0u)
				return true;
		}
		return false;
	};
	const auto mask_merge = [](StructuredScratchMask* destination,
			const StructuredScratchMask& source) {
		for (u32 word = 0; word < destination->size(); word++)
			(*destination)[word] |= source[word];
	};
	const auto mask_nonempty = [](const StructuredScratchMask& mask) {
		return std::any_of(mask.begin(), mask.end(),
			[](u32 word) { return word != 0u; });
	};

	std::array<GeneratedVuProgram*,
		VitaGpuVu::GpuVuDirectPrecomputeMaximumPrograms>
		precompute_programs{};
	bool generated_pipeline_uses_arm_estimate_table =
		generated->metadata.uses_arm_estimate_table;
	const bool staged_generated_direct =
		draw->precompute_program_count != 0u;
	if (loop_kernel_direct && (structured_direct || staged_generated_direct ||
		generated->metadata.UsesStructuredSnapshotInputBuffers()))
	{
		return Reject(
			"generated loop-kernel escaped its one-root no-snapshot ABI");
	}
	if (staged_generated_direct)
	{
		if (!structured_direct || draw->precompute_stage_count == 0u ||
			!generated->metadata.uses_structured_parallel_direct_vu_tfx ||
			!generated->metadata.UsesStructuredSnapshotInputBuffers() ||
			!mask_nonempty(generated->metadata.structured_scratch_read_mask) ||
			generated->gxp_resources.scratch_buffer_size != 0u ||
			generated->gxp_resources.thread_buffer_size != 0u)
		{
			return Reject(
				"generated direct consumer escaped the staged scratch ABI");
		}

		StructuredScratchMask available{};
		StructuredScratchMask stage_reads{};
		StructuredScratchMask stage_writes{};
		u32 active_stage = 0u;
		for (u32 index = 0; index < draw->precompute_program_count; index++)
		{
			const u32 stage = draw->precompute_stages[index];
			if (stage > active_stage)
			{
				if (stage != active_stage + 1u ||
					!mask_nonempty(stage_writes))
				{
					return Reject(
						"generated direct precompute stage ordering is invalid");
				}
				mask_merge(&available, stage_writes);
				stage_reads = {};
				stage_writes = {};
				active_stage = stage;
			}

			GeneratedVuProgram* const producer =
				FindGeneratedVuProgram(draw->precompute_programs[index]);
			if (!producer || !producer->registration_complete ||
				producer->resource_attestation !=
					VitaGpuVu::GeneratedGxpResourceAttestation::Accepted ||
				!producer->vertex_program ||
				!producer->general_fragment_program ||
				producer->metadata.execution_kind !=
					VitaGpuVu::GeneratedCgExecutionKind::
						StructuredExpressionScratch ||
				!producer->metadata.uses_structured_expression_scratch ||
				!producer->metadata.uses_generated_direct_precompute ||
				!producer->metadata.UsesStructuredSnapshotInputBuffers() ||
				producer->metadata.uses_tfx_uniforms ||
				producer->metadata.structured_scratch_output_count == 0u ||
				!mask_nonempty(
					producer->metadata.structured_scratch_write_mask) ||
				producer->gxp_resources.scratch_buffer_size != 0u ||
				producer->gxp_resources.thread_buffer_size != 0u)
			{
				return Reject(
					"generated direct precompute program is not GS-ready");
			}
			if (!mask_contains(available,
					producer->metadata.structured_scratch_read_mask) ||
				mask_intersects(stage_writes,
					producer->metadata.structured_scratch_read_mask) ||
				mask_intersects(stage_reads,
					producer->metadata.structured_scratch_write_mask) ||
				mask_intersects(stage_writes,
					producer->metadata.structured_scratch_write_mask))
			{
				return Reject(
					"generated direct precompute dependency proof changed");
			}
			mask_merge(&stage_reads,
				producer->metadata.structured_scratch_read_mask);
			mask_merge(&stage_writes,
				producer->metadata.structured_scratch_write_mask);
			precompute_programs[index] = producer;
			generated_pipeline_uses_arm_estimate_table |=
				producer->metadata.uses_arm_estimate_table;
		}
		mask_merge(&available, stage_writes);
		if (active_stage + 1u != draw->precompute_stage_count ||
			!mask_contains(available,
				generated->metadata.structured_scratch_read_mask))
		{
			return Reject(
				"generated direct consumer reads unpublished scratch");
		}
	}
	const bool flat_instances =
		generated->metadata.uses_flat_instance_inputs;
	const bool flat_indices =
		generated->metadata.uses_flat_index_inputs;
	const bool expanded_flat = flat_instances || flat_indices;
	const bool nested_grid =
		generated->metadata.uses_nested_iteration_grid;
	const bool nested_batch_index =
		generated->metadata.uses_nested_batch_index_inputs;
	const bool buffered_batch =
		generated->metadata.uses_buffered_batch_inputs;
	const bool instance_batch_live_ins =
		generated->metadata.uses_instance_indexed_batch_live_ins;
	const bool variable_capacity_batch =
		loop_kernel_direct && nested_grid && buffered_batch &&
		draw->HasExactIndices() &&
		draw->HasGeneratedLoopKernelTransaction();
	const bool nested_flat_line_product = loop_kernel_direct && nested_grid &&
		flat_indices && !flat_instances && draw->HasExpandedExactIndices() &&
		draw->direct_tfx.primitive == GS_LINESTRIP && !draw->direct_tfx.gouraud &&
		!draw->HasPrivateStoreJournal() && !draw->HasPrivateStoreComparison() &&
		(generated->metadata.loop_kernel_source_abi ==
			VitaGpuVu::GeneratedLoopKernelNestedFlatProductCgAbiVersion ||
		 generated->metadata.loop_kernel_source_abi ==
			VitaGpuVu::GeneratedLoopKernelNestedFlatPartialProductCgAbiVersion);
	if (loop_kernel_direct)
	{
		const bool private_output =
			generated->metadata.uses_loop_kernel_private_store_output;
		if ((private_output &&
				(generated->metadata.loop_kernel_private_store_count == 0u ||
				 generated->metadata.loop_kernel_private_store_count !=
					draw->private_store_count ||
				 !draw->HasPrivateStoreJournal())) ||
			(!private_output &&
				(generated->metadata.loop_kernel_private_store_count != 0u ||
				 draw->HasPrivateStoreJournal() ||
				 !draw->HasGeneratedLoopKernelTransaction() ||
				 draw->private_store_count == 0u)) ||
			(generated->metadata.uses_loop_kernel_ftoi_probe_output &&
				(!generated_final_state_owner ||
				 !VitaGpuVu::IsGeneratedCgExactDivideConfigurationSupported(
					 generated->metadata.
						 loop_kernel_ftoi_probe_configuration_bits) ||
				 !VitaGpuVu::IsGeneratedCgNativeF32ConfigurationSupported(
					 generated->metadata.
						 loop_kernel_ftoi_probe_configuration_bits) ||
				 (generated->metadata.loop_kernel_ftoi_probe_configuration_bits &
					 VitaGpuVu::UniversalConfigurationApproximateConversions) == 0u ||
				 generated->metadata.loop_kernel_ftoi_probe_store_index >=
					 draw->private_store_count ||
				 generated->metadata.loop_kernel_ftoi_probe_lane >= 4u ||
				 (generated->metadata.loop_kernel_ftoi_probe_scale_offset != 0u &&
				  generated->metadata.loop_kernel_ftoi_probe_scale_offset != 4u &&
				  generated->metadata.loop_kernel_ftoi_probe_scale_offset != 12u &&
				  generated->metadata.loop_kernel_ftoi_probe_scale_offset != 15u))) ||
			(!generated->metadata.uses_loop_kernel_ftoi_probe_output &&
				(generated->metadata.
					 loop_kernel_ftoi_probe_configuration_bits != 0u ||
				 generated->metadata.loop_kernel_ftoi_probe_store_index != 0u ||
				 generated->metadata.loop_kernel_ftoi_probe_lane != 0u ||
				 generated->metadata.loop_kernel_ftoi_probe_scale_offset != 0u)))
		{
			return Reject(
				"generated loop-kernel private-output ABI differs from its descriptor");
		}
	}
	else if (generated->metadata.uses_loop_kernel_private_store_output ||
		generated->metadata.loop_kernel_private_store_count != 0u ||
		draw->HasPrivateStoreJournal())
	{
		return Reject(
			"non-loop generated root unexpectedly owns a private-store journal");
	}
		if ((flat_instances && flat_indices) ||
			(nested_grid && expanded_flat && !nested_flat_line_product) ||
			(nested_grid != nested_batch_index) ||
			instance_batch_live_ins ||
			(buffered_batch && !expanded_flat && !nested_grid) ||
		(nested_grid && !buffered_batch) ||
		(flat_indices && !buffered_batch))
	{
			return Reject(
				"GPU-VU buffered input root has an invalid expanded-index ABI");
		}
		if (draw->HasCompactRawInputs() && !buffered_batch)
			return Reject("compact GPU-VU input requires the BUFFER0 batch ABI");
	const VitaGpuVu::PrimitiveBoundary expected_boundary =
		private_state_canary ? VitaGpuVu::PrimitiveBoundary::PrivateStatePoints :
		draw->HasExactIndices() ?
			(flat_indices ? VitaGpuVu::PrimitiveBoundary::ExactPostLoopExpandedIndexed :
				VitaGpuVu::PrimitiveBoundary::ExactPostLoopIndexed) :
			(flat_instances ? VitaGpuVu::PrimitiveBoundary::InstanceIndexed :
				(flat_indices ? VitaGpuVu::PrimitiveBoundary::ExpandedIndexed :
					VitaGpuVu::PrimitiveBoundary::Native));
	if (draw->primitive_boundary != expected_boundary)
	{
		return Reject("GPU-VU primitive boundary differs from generated ABI");
	}

	u32 vf_mask = 0;
	for (const VitaGpuVu::VectorUniform& uniform : draw->VfUniforms())
		vf_mask |= 1u << uniform.register_index;
	if (!structured_direct &&
		vf_mask != generated->metadata.vf_uniform_mask)
		return Reject("generated VU1 VF-uniform mask mismatch");
	if (structured_direct &&
		(!draw->VfUniforms().empty() ||
			!draw->ConstantUniforms().empty() ||
			draw->scalar_uniforms.present != 0u))
	{
		return Reject(
			"generated structured VU1 state escaped its buffer ABI");
	}
	for (u32 index = 0; index < draw->ConstantUniforms().size(); index++)
	{
		if (draw->ConstantUniforms()[index].input_index != index ||
			generated->metadata.constant_inputs[index].uniform_index != index)
		{
			return Reject("generated VU1 constant-uniform layout mismatch");
		}
	}
	const u32 expected_scalar_mask =
		(generated->metadata.uses_q_uniform ?
			VitaGpuVu::ScalarUniformQ : 0u) |
		(generated->metadata.uses_p_uniform ?
			VitaGpuVu::ScalarUniformP : 0u) |
		(generated->metadata.uses_i_uniform ?
			VitaGpuVu::ScalarUniformI : 0u) |
		(generated->metadata.uses_gif_q_uniform ?
			VitaGpuVu::ScalarUniformGifQ : 0u);
	if (!structured_direct &&
		draw->scalar_uniforms.present != expected_scalar_mask)
		return Reject("generated VU1 scalar-uniform mask mismatch");

	std::vector<const u8*> structured_direct_inputs;
	if (structured_direct)
		structured_direct_inputs.reserve(active_gpu_vu_draws->size());
	for (const auto& candidate_owner : *active_gpu_vu_draws)
	{
		const VitaGpuVu::GpuVuDraw* const candidate =
			candidate_owner.get();
		if (!candidate ||
			!candidate->WasValidatedForQueue() ||
			candidate->program != draw->program ||
			candidate->precompute_program_count !=
				draw->precompute_program_count ||
			candidate->precompute_stage_count !=
				draw->precompute_stage_count ||
			!std::equal(candidate->precompute_programs.begin(),
				candidate->precompute_programs.begin() +
					candidate->precompute_program_count,
				draw->precompute_programs.begin()) ||
			!std::equal(candidate->precompute_stages.begin(),
				candidate->precompute_stages.begin() +
					candidate->precompute_program_count,
				draw->precompute_stages.begin()) ||
				candidate->streams.size() != draw->streams.size() ||
				!VitaGpuVu::HasSamePrivateStoreBufferBinding(
					*candidate, *draw) ||
					candidate->private_store_count != draw->private_store_count ||
				candidate->HasPrivateStoreComparison() !=
					draw->HasPrivateStoreComparison() ||
				candidate->HasGeneratedLoopKernelTransaction() !=
					draw->HasGeneratedLoopKernelTransaction() ||
				candidate->HasCompactRawInputs() !=
					draw->HasCompactRawInputs() ||
				candidate->HasExactIndices() != draw->HasExactIndices() ||
				candidate->HasExpandedExactIndices() != draw->HasExpandedExactIndices() ||
				candidate->IsPrivateStateCanary() != private_state_canary ||
			(candidate->HasExactIndices() && !variable_capacity_batch &&
				candidate->ExactIndices() != draw->ExactIndices()) ||
			structured_direct !=
				(candidate->StructuredDirectInput() != nullptr))
		{
			return Reject("GPU-VU descriptor escaped its derived batch ABI");
		}
		if (variable_capacity_batch)
		{
			const u64 capacity =
				static_cast<u64>(generated->metadata.nested_outer_iterations) *
				generated->metadata.nested_child_iterations;
			std::array<u32, 4> normalized_tag = candidate->gif_tag;
			std::array<u32, 4> normalized_first_tag = draw->gif_tag;
			normalized_tag[0] &= ~0x7fffu;
			normalized_first_tag[0] &= ~0x7fffu;
			GIFTag candidate_tag{};
			std::memcpy(&candidate_tag, candidate->gif_tag.data(),
				sizeof(candidate_tag));
			if (!candidate->HasGeneratedLoopKernelTransaction() ||
				candidate->invocation_count == 0u ||
				candidate->invocation_count > capacity ||
				(candidate->invocation_count %
					generated->metadata.nested_child_iterations) != 0u ||
				candidate->vertex_count != candidate->invocation_count ||
				candidate_tag.NLOOP != candidate->vertex_count ||
				normalized_tag != normalized_first_tag)
			{
				return Reject(
					"variable-capacity GPU-VU transaction changed its root contract");
			}
		}
		u32 active_outer_iterations = 1u;
		if (nested_grid)
		{
			const u32 child_iterations =
				generated->metadata.nested_child_iterations;
			if (child_iterations == 0u || candidate->invocation_count == 0u ||
				(candidate->invocation_count % child_iterations) != 0u)
			{
				return Reject(
					"GPU-VU nested input domain is not an exact child grid");
			}
			active_outer_iterations =
				candidate->invocation_count / child_iterations;
			if (active_outer_iterations == 0u ||
				active_outer_iterations >
					generated->metadata.nested_outer_iterations ||
				(private_state_canary && active_outer_iterations !=
					generated->metadata.nested_outer_iterations))
			{
				return Reject(
					"GPU-VU nested input domain exceeds compiled capacity");
			}
		}
		for (u32 index = 0; index < candidate->streams.size(); index++)
		{
			const VitaGpuVu::StreamBinding& binding =
				candidate->streams[index];
			const VitaGpuVu::CgMemoryInput& input =
				generated->metadata.memory_inputs[index];
			u64 expected_stride = 0u;
			u64 expected_outer_stride = 0u;
			u64 expected_extent = 0u;
			if (input.compact_outer_table ==
				VitaGpuVu::CgMemoryInput::PackedCompactOuterTables)
			{
				u64 expected_qwords = 0u;
				for (const VitaGpuVu::CompactOuterInputTable& table :
					generated->metadata.compact_outer_inputs)
				{
					if (table.sources.size() !=
							generated->metadata.nested_outer_iterations ||
						table.sources.size() >
							std::numeric_limits<u64>::max() - expected_qwords)
					{
						return Reject(
							"GPU-VU compact outer-input table is invalid");
					}
					expected_qwords += table.sources.size();
				}
				if (expected_qwords >
						std::numeric_limits<u64>::max() / 16u)
				{
					return Reject(
						"GPU-VU compact outer-input extent overflow");
				}
				expected_extent = expected_qwords * 16u;
				if (!nested_grid || expected_extent == 0u ||
					!input.address.valid ||
					input.address.base_vi != 0u ||
					input.address.qword_offset != 0 ||
					input.address.invocation_coefficient != 0 ||
					input.address.outer_invocation_coefficient != 0)
				{
					return Reject(
						"GPU-VU compact outer-input marker is invalid");
				}
			}
			else
			{
				if (input.compact_outer_table !=
					VitaGpuVu::CgMemoryInput::OrdinaryVuMemory)
				{
					return Reject("GPU-VU input has an unknown compact marker");
				}
				expected_stride = static_cast<u64>(
					input.address.invocation_coefficient) * 16u;
				expected_outer_stride = nested_grid ?
					static_cast<u64>(
						input.address.outer_invocation_coefficient) * 16u : 0u;
				expected_extent = nested_grid ?
					(static_cast<u64>(
						active_outer_iterations - 1u) *
						input.address.outer_invocation_coefficient +
					 static_cast<u64>(
						generated->metadata.nested_child_iterations - 1u) *
						input.address.invocation_coefficient + 1u) * 16u :
					(static_cast<u64>(candidate->invocation_count - 1u) *
						input.address.invocation_coefficient + 1u) * 16u;
			}
			if (binding.attribute_index != index ||
				input.attribute_index != index ||
				expected_stride != binding.byte_stride ||
				expected_outer_stride != binding.outer_byte_stride ||
				expected_extent != binding.payload_byte_extent ||
				(binding.payload_byte_offset &
					(buffered_batch ? 15u : 3u)) != 0)
			{
				return Reject(
					"generated VU1 input binding differs from GXP layout");
			}
		}
		if (structured_direct)
		{
			const VitaGpuVu::RawVifPayloadRef* const input_ref =
				candidate->StructuredDirectInput();
			const u8* const input = input_ref ?
				VitaGpuVu::ResolveGpuRawVifPayload(*input_ref) : nullptr;
			if (!input ||
				input_ref->size != VitaGpuVu::GeneratedNestedDirectInputBytes)
			{
				return Reject(
					"generated nested-direct input retired before preflight");
			}
			structured_direct_inputs.push_back(input);
		}
	}
	if (structured_direct_inputs.size() !=
			(structured_direct ? active_gpu_vu_draws->size() : 0u))
	{
		return Reject("generated nested-direct input preflight is incomplete");
	}

	VitaGXM::ArenaAllocation structured_scratch;
	u32 structured_scratch_stride = 0u;
	u32 structured_scratch_bytes = 0u;
	if (staged_generated_direct)
	{
		constexpr u64 scratch_bytes_per_invocation =
			static_cast<u64>(VitaGpuVu::StructuredGeneratedScratchSlots) *
			sizeof(u32);
		const u64 total_invocations =
			static_cast<u64>(draw->invocation_count) *
			active_gpu_vu_draws->size();
		if (total_invocations == 0u ||
			total_invocations >
				VitaGpuVu::StructuredGeneratedMaximumScratchInvocations)
		{
			return Reject(
				"generated direct scratch batch exceeds its mapped bound");
		}
		const u64 stride_bytes =
			static_cast<u64>(draw->invocation_count) *
			scratch_bytes_per_invocation;
		const u64 allocation_bytes =
			total_invocations * scratch_bytes_per_invocation;
		if (stride_bytes > std::numeric_limits<u32>::max() ||
			allocation_bytes > std::numeric_limits<u32>::max())
		{
			return Reject("generated direct scratch size overflow");
		}
		structured_scratch_stride = static_cast<u32>(stride_bytes);
		structured_scratch_bytes = static_cast<u32>(allocation_bytes);
		const int allocation_result = transfer_arena.Allocate(
			structured_scratch_bytes, 64u, &structured_scratch);
		if (allocation_result < 0 || !structured_scratch)
		{
			return Fail("allocate generated VU1 staged scratch",
				allocation_result < 0 ? allocation_result :
					SCE_GXM_ERROR_OUT_OF_MEMORY);
		}
	}

	GIFTag tag{};
	std::memcpy(&tag, draw->gif_tag.data(), sizeof(tag));
	GIFRegPRIM prim{};
	prim.U32[0] = tag.PRIM;
	if (!tag.PRE || tag.FLG != GIF_FLG_PACKED ||
		tag.NLOOP != draw->vertex_count ||
		prim.PRIM != draw->direct_tfx.primitive ||
		static_cast<bool>(prim.IIP) != draw->direct_tfx.gouraud ||
		static_cast<bool>(prim.TME) != draw->direct_tfx.textured ||
		static_cast<bool>(prim.FGE) != draw->direct_tfx.fog_enabled ||
		static_cast<bool>(prim.FST) !=
			draw->direct_tfx.fixed_texture_coordinates)
	{
		return Reject("GPU-VU static GIF tag differs from direct-TFX contract");
	}
	if (static_cast<bool>(config.vs.iip) != draw->direct_tfx.gouraud ||
		static_cast<bool>(config.vs.tme) != draw->direct_tfx.textured ||
		static_cast<bool>(config.vs.fst) !=
			draw->direct_tfx.fixed_texture_coordinates ||
		static_cast<bool>(config.ps.fst) !=
			draw->direct_tfx.fixed_texture_coordinates ||
		static_cast<bool>(config.ps.fog) != draw->direct_tfx.fog_enabled)
	{
		return Reject("PCSX2 TFX selectors differ from the GPU-VU GIF contract");
	}
	if (!private_state_canary &&
		generated->metadata.uses_tfx_uv_no_fog_interface !=
		(draw->direct_tfx.textured &&
			draw->direct_tfx.fixed_texture_coordinates &&
			!draw->direct_tfx.fog_enabled))
	{
		return Reject("generated VU1 TFX varying ABI differs from the GIF contract");
	}
	if (generated->metadata.uses_tfx_point_size !=
		(private_state_canary || prim.PRIM == GS_POINTLIST))
	{
		return Reject("generated VU1 point-size ABI differs from the GIF contract");
	}
	if (!draw->direct_tfx.gouraud && prim.PRIM != GS_POINTLIST &&
		!expanded_flat && !draw->HasExactIndices() && !private_state_canary)
	{
		return Reject(
			"flat GPU-VU primitive lacks exact expanded provoking color");
	}

	SceGxmPrimitiveType primitive_type{};
	GSHWDrawConfig::Topology topology{};
	u32 indices_per_primitive = 0;
	u32 primitive_count = 0;
	u32 flat_vertex_step = 0;
	bool flat_strip_winding = false;
	switch (prim.PRIM)
	{
		case GS_POINTLIST:
			if (expanded_flat)
				return Reject("point GPU-VU draw unexpectedly expands flat inputs");
			primitive_type = SCE_GXM_PRIMITIVE_POINTS;
			topology = GSHWDrawConfig::Topology::Point;
			indices_per_primitive = 1;
			primitive_count = draw->vertex_count;
			break;
		case GS_LINELIST:
			if ((draw->vertex_count & 1u) != 0)
				return Reject("odd native GPU-VU line-list vertex count");
			primitive_type = SCE_GXM_PRIMITIVE_LINES;
			topology = GSHWDrawConfig::Topology::Line;
			indices_per_primitive = 2;
			primitive_count = draw->vertex_count / 2;
			flat_vertex_step = 2;
			break;
		case GS_LINESTRIP:
			if ((!expanded_flat && !draw->HasExactIndices() && !private_state_canary) ||
				draw->vertex_count < 2)
				return Reject("line-strip GPU-VU draw lacks flat expansion");
			primitive_type = SCE_GXM_PRIMITIVE_LINES;
			topology = GSHWDrawConfig::Topology::Line;
			indices_per_primitive = 2;
			primitive_count = draw->HasExactIndices() ? draw->index_count / 2u :
				draw->vertex_count - 1;
			flat_vertex_step = 1;
			break;
		case GS_TRIANGLELIST:
			if ((draw->vertex_count % 3u) != 0)
				return Reject("non-integral native GPU-VU triangle-list count");
			primitive_type = SCE_GXM_PRIMITIVE_TRIANGLES;
			topology = GSHWDrawConfig::Topology::Triangle;
			indices_per_primitive = 3;
			primitive_count = draw->vertex_count / 3;
			flat_vertex_step = 3;
			break;
		case GS_TRIANGLESTRIP:
			if (draw->vertex_count < 3)
				return Reject("short native GPU-VU triangle strip");
			primitive_type = (expanded_flat || draw->HasExactIndices()) ?
				SCE_GXM_PRIMITIVE_TRIANGLES :
				SCE_GXM_PRIMITIVE_TRIANGLE_STRIP;
			topology = GSHWDrawConfig::Topology::Triangle;
			indices_per_primitive = 3;
			primitive_count = draw->HasExactIndices() ?
				static_cast<u32>(draw->ExactIndices().size() / 3u) :
				draw->vertex_count - 2;
			flat_vertex_step = 1;
			flat_strip_winding = true;
			break;
		case GS_TRIANGLEFAN:
			if (expanded_flat)
			{
				return Reject(
					"flat triangle fan requires a GPU export output lowering");
			}
			if (draw->vertex_count < 3)
				return Reject("short native GPU-VU triangle fan");
			primitive_type = SCE_GXM_PRIMITIVE_TRIANGLE_FAN;
			topology = GSHWDrawConfig::Topology::Triangle;
			indices_per_primitive = 3;
			primitive_count = draw->vertex_count - 2;
			break;
		case GS_SPRITE:
		default:
			return Reject("GPU-VU primitive requires a non-native output lowering");
	}
	if (expanded_flat &&
		(draw->direct_tfx.gouraud ||
			generated->metadata.flat_vertices_per_primitive !=
				indices_per_primitive ||
			generated->metadata.flat_instance_vertex_step !=
				flat_vertex_step ||
			generated->metadata.flat_strip_winding != flat_strip_winding ||
			(buffered_batch &&
				generated->metadata.batch_primitives_per_draw !=
					(nested_flat_line_product
						? static_cast<u32>(generated->metadata.nested_outer_iterations) *
							generated->metadata.nested_child_iterations - 1u
						: primitive_count))))
	{
		return Reject("GPU-VU flat expansion metadata differs from GIF topology");
	}
	// Preserve the original GIF/GSRendererHW contract above. Only the private
	// hardware dispatch becomes dense POINTS, so ADC-skipped VU iterations still
	// write their journal slots. This draw never supplies the visible geometry.
	const GSHWDrawConfig::Topology guest_topology = topology;
	const u32 guest_indices_per_primitive = indices_per_primitive;
	if (private_state_canary)
	{
		primitive_type = SCE_GXM_PRIMITIVE_POINTS;
		topology = GSHWDrawConfig::Topology::Point;
		indices_per_primitive = 1u;
		primitive_count = draw->invocation_count;
	}
	const u64 expected_index_count = draw->HasExactIndices() ?
		draw->ExactIndices().size() :
		(expanded_flat ?
			static_cast<u64>(primitive_count) * indices_per_primitive :
			draw->vertex_count);
	const u64 nested_invocations =
		static_cast<u64>(generated->metadata.nested_outer_iterations) *
		generated->metadata.nested_child_iterations;
	const bool nested_grid_dimensions_valid = !nested_grid ||
		(generated->metadata.nested_outer_iterations != 0u &&
		 generated->metadata.nested_child_iterations != 0u &&
		 draw->invocation_count %
			 generated->metadata.nested_child_iterations == 0u &&
		 draw->invocation_count <= nested_invocations &&
		 nested_invocations <= GPU_VU_SEQUENTIAL_INDEX_COUNT);
	const bool full_nested_batch_domain = nested_batch_index &&
		nested_invocations == draw->invocation_count;
	if (expected_index_count > std::numeric_limits<u32>::max() ||
		draw->index_count != expected_index_count ||
		(!expanded_flat && !draw->HasExactIndices() &&
			draw->index_count > GPU_VU_SEQUENTIAL_INDEX_COUNT) ||
		(flat_instances &&
			primitive_count > GPU_VU_SEQUENTIAL_INDEX_COUNT) ||
		(flat_indices &&
			draw->index_count > GPU_VU_SEQUENTIAL_INDEX_COUNT) ||
		draw->primitive_count != primitive_count ||
		!nested_grid_dimensions_valid ||
		(private_state_canary && !full_nested_batch_domain) ||
		config.topology != guest_topology ||
		config.indices_per_prim != guest_indices_per_primitive)
	{
		return Reject("GPU-VU geometry dimensions differ from PCSX2 draw state");
	}
	constexpr u32 maximum_gxm_draw_indices = (1u << 22u) - 1u;

	struct BatchInputGroup
	{
		const u8* raw_buffer_base = nullptr;
		uptr owner = 0;
		u32 slot = 0;
		u32 generation = 0;
		u32 first_qword = 0;
			u32 last_qword = 0;
			u32 first_draw = 0;
			u32 draw_count = 0;
			u32 index_count = 0;
			u32 index_minimum = 0;
			u32 index_maximum = 0;
			VitaGXM::ArenaAllocation compact_raw_input;
			VitaGXM::ArenaAllocation batch_data;
			VitaGXM::ArenaAllocation batch_live_ins;
		VitaGXM::ArenaAllocation exact_index_data;
			std::vector<GpuVuPrivateStoreOutput> private_store_outputs;
	};
	std::vector<BatchInputGroup> batch_input_groups;
	if (buffered_batch)
	{
#if defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
		if (!gpu_vu_canonical_vu_memory_mapped || !VU1.Mem)
			return Reject("generated VU1 canonical input owner is unavailable");
#else
		return Reject("generated VU1 canonical input owner is disabled");
#endif
		const u32 binding_vectors =
			generated->metadata.BatchBindingVectorCount();
		const u32 raw_binding_vectors =
			generated->metadata.BatchRawBindingVectorCount();
		const u32 record_vectors =
			generated->metadata.BatchRecordVectorCount();
		const u32 instance_live_in_vectors =
			generated->metadata.BatchInstanceLiveInVectorCount();
		const u32 varying_live_in_vectors =
			generated->metadata.BatchVaryingLiveInVectorCount();
		const bool inline_batch_varying_tail =
			generated->metadata.UsesInlineBatchVaryingTail();
		const bool dynamic_batch_uniform =
			generated->metadata.uses_dynamic_batch_uniform_index;
		const u32 expected_binding_vectors = raw_binding_vectors;
		const u32 expected_record_vectors = binding_vectors +
			(instance_batch_live_ins ? 0u :
				generated->metadata.BatchInvariantUniformVectorCount()) +
			(inline_batch_varying_tail ? varying_live_in_vectors : 0u);
		if (raw_binding_vectors == 0u ||
			binding_vectors != expected_binding_vectors ||
			dynamic_batch_uniform ||
			record_vectors != expected_record_vectors ||
			instance_live_in_vectors != 0u ||
			varying_live_in_vectors > 4u ||
			((varying_live_in_vectors != 0u) !=
			 generated->metadata.batch_varying_live_ins.Any()))
			return Reject("GPU-VU buffered root has no binding vectors");
		// psp2shaderperf shows the installed compiler consuming dynamic buffer
		// indices through a signed 16-bit path. Keep the last int4 record index
		// positive just as the raw VIF window below keeps its qword index
		// positive.
		constexpr u32 maximum_batch_data_vector =
			static_cast<u32>(std::numeric_limits<s16>::max());
		const u32 maximum_record_draws =
			(maximum_batch_data_vector + 1u) / record_vectors;
		if (maximum_record_draws == 0)
			return Reject("GPU-VU batch record exceeds its addressable buffer");
		// Flat-line INDEX addresses endpoint slots, not original VU iterations.
		// ADC changes the number of submitted indices, never this compiled stride.
		const u32 nested_batch_stride = nested_flat_line_product
			? VitaGpuVu::DirectTfxFlatLineIndexDomain(static_cast<u32>(nested_invocations))
			: static_cast<u32>(nested_invocations);
		const u32 maximum_nested_batch_draws =
			(full_nested_batch_domain || variable_capacity_batch) &&
			nested_batch_stride != 0u ?
			GPU_VU_SEQUENTIAL_INDEX_COUNT / nested_batch_stride : 0u;
		const u32 maximum_instance_batch_draws = instance_batch_live_ins ?
			std::min(GPU_VU_SEQUENTIAL_INDEX_COUNT - 1u,
				maximum_gxm_draw_indices / draw->index_count) : 0u;
		const u32 maximum_indexed_batch_draws = instance_batch_live_ins ?
			maximum_instance_batch_draws :
			((full_nested_batch_domain || variable_capacity_batch) ?
				maximum_nested_batch_draws :
				((nested_grid || draw->HasExactIndices() ||
				  draw->HasPrivateStoreJournal()) ? 1u :
				 (flat_indices ?
					GPU_VU_SEQUENTIAL_INDEX_COUNT / draw->index_count :
					VitaGpuVu::GeneratedCgProgram::MaximumBatchDraws)));
		if (maximum_indexed_batch_draws == 0u)
			return Reject("GPU-VU expanded index batch exceeds its U16 domain");

		// The installed PSP2 compiler lowers the dynamically indexed int4
		// loads in this root through signed 16-bit address arithmetic when the
		// shader keeps Sony's proven 64-vector declaration. psp2shaderperf
		// shows the binding value consumed by mad.i16 and then r*.lo16. The
		// capture ring is 2 MiB, so passing a slot-absolute qword index can
		// silently discard its upper bits. Bind a sub-range of the mapped slot
		// for each draw group and keep every final qword index in the positive
		// signed-16 range. gxm/memory.h explicitly permits one mapped capture
		// buffer to supply independently based uniform-buffer sub-ranges.
			constexpr u32 maximum_relative_qword =
				VitaGpuVu::GeneratedRawInputMaximumRelativeQword;
		struct ResolvedInputRange
		{
			const u8* slot_base = nullptr;
			uptr owner = 0;
			u32 slot = 0;
			u32 generation = 0;
			u32 first_qword = 0;
			u32 last_qword = 0;
			u32 compact_qword_count = 0;
			bool compact = false;
			bool uses_raw = false;
			bool uses_canonical = false;
		};
			const auto resolve_input_range =
				[&](const VitaGpuVu::GpuVuDraw& candidate,
					ResolvedInputRange* range) {
					if (!range)
						return false;
					*range = {};
				if (candidate.HasCompactRawInputs())
				{
					const auto& words = candidate.CompactRawInputWords();
					if (words.empty() || (words.size() & 3u) != 0u ||
						!candidate.InputPayloads().empty())
					{
						return false;
					}
					range->first_qword = 0u;
					range->last_qword =
						static_cast<u32>(words.size() / 4u - 1u);
					range->compact_qword_count =
						static_cast<u32>(words.size() / 4u);
					range->compact = true;
						range->uses_raw = true;
						return true;
					}

					VitaGpuVu::GeneratedInputWindow structural_window;
					VitaGpuVu::GeneratedInputWindowFailure structural_failure =
						VitaGpuVu::GeneratedInputWindowFailure::None;
					const VitaGpuVu::GeneratedInputWindow* const sealed_window =
						candidate.GeneratedInputWindowProof();
					if (sealed_window)
					{
						structural_window = *sealed_window;
					}
					else if (!VitaGpuVu::ResolveGeneratedInputWindow(
							candidate.InputPayloads().data(),
							candidate.InputPayloads().size(), candidate.streams.data(),
							candidate.streams.size(),
							VitaGpuVu::GeneratedRawInputBufferQwords,
							VU1_MEMSIZE / 16u,
							VitaGpuVu::GeneratedRawInputDeclaredQwords,
							maximum_relative_qword, &structural_window,
							&structural_failure))
					{
						static std::atomic<u64> structural_reports{0u};
						const u64 report = structural_reports.fetch_add(
							1u, std::memory_order_relaxed) + 1u;
						if (report <= 8u || (report & (report - 1u)) == 0u)
						{
							Console.Error(
								"GPU-VU: generated_input_window report=%llu seq=%llu "
								"key=%016llx%016llx streams=%u payloads=%u "
								"failure=%s sealed=0 pre_effect=1.",
								static_cast<unsigned long long>(report),
								static_cast<unsigned long long>(
									candidate.ordering_sequence),
								static_cast<unsigned long long>(candidate.program.high),
								static_cast<unsigned long long>(candidate.program.low),
								static_cast<u32>(candidate.streams.size()),
								static_cast<u32>(candidate.InputPayloads().size()),
								VitaGpuVu::GeneratedInputWindowFailureName(
									structural_failure));
						}
						return false;
					}

					range->uses_raw = structural_window.uses_raw;
					range->uses_canonical = structural_window.uses_canonical;
					range->first_qword = structural_window.bound_first_qword;
					range->last_qword = structural_window.required_last_qword;
					if (structural_window.uses_canonical)
					{
						if (!VU1.Mem)
							return false;
						range->slot_base = VU1.Mem;
						range->owner = reinterpret_cast<uptr>(VU1.Mem);
						return true;
					}
					if (!structural_window.uses_raw)
						return false;

					for (u32 payload_index = 0u;
						 payload_index < candidate.InputPayloads().size(); payload_index++)
					{
						const VitaGpuVu::RawVifPayloadRef& payload_ref =
							candidate.InputPayloads()[payload_index];
						VitaGpuVu::RawVifPayloadResolveFailure payload_failure =
							VitaGpuVu::RawVifPayloadResolveFailure::None;
						VitaGpuVu::RawVifPayloadResolveDiagnostics diagnostics;
						const u8* const payload = VitaGpuVu::ResolveGpuRawVifPayload(
							payload_ref, &payload_failure, &diagnostics);
						const u8* const base = payload ? payload - payload_ref.offset : nullptr;
						const bool identity_matches = payload &&
							payload_ref.owner == structural_window.owner &&
							payload_ref.slot == structural_window.slot &&
							payload_ref.generation == structural_window.generation &&
							(!range->slot_base || range->slot_base == base);
						if (!identity_matches)
						{
							static std::atomic<u64> publication_reports{0u};
							const u64 report = publication_reports.fetch_add(
								1u, std::memory_order_relaxed) + 1u;
							if (report <= 8u || (report & (report - 1u)) == 0u)
							{
								Console.Error(
									"GPU-VU: generated_input_publication report=%llu "
									"seq=%llu key=%016llx%016llx streams=%u payloads=%u "
									"payload=%u failure=%s owner=%08x slot=%u "
									"wanted_gen=%u actual_gen=%u offset=%u size=%u "
									"committed=%u published=%u refs=%u required=%u..%u "
									"bound=%u identity=%u pre_effect=1.",
									static_cast<unsigned long long>(report),
									static_cast<unsigned long long>(
										candidate.ordering_sequence),
									static_cast<unsigned long long>(candidate.program.high),
									static_cast<unsigned long long>(candidate.program.low),
									static_cast<u32>(candidate.streams.size()),
									static_cast<u32>(candidate.InputPayloads().size()),
									payload_index,
									VitaGpuVu::RawVifPayloadResolveFailureName(
										payload_failure),
									static_cast<u32>(diagnostics.owner), diagnostics.slot,
									diagnostics.wanted_generation,
									diagnostics.actual_generation, diagnostics.offset,
									diagnostics.size, diagnostics.committed_prefix,
									diagnostics.published_prefix, diagnostics.references,
									structural_window.required_first_qword,
									structural_window.required_last_qword,
									structural_window.bound_first_qword,
									identity_matches ? 1u : 0u);
							}
							return false;
						}
						if (!range->slot_base)
							range->slot_base = base;
					}
					range->owner = structural_window.owner;
					range->slot = structural_window.slot;
					range->generation = structural_window.generation;
					return true;
				};
		struct BatchLiveInVariance
		{
			u64 constant_vectors = 0u;
			u32 vf_registers = 0u;
			u8 acc_lanes = 0u;
			u8 scalar_lanes = 0u;
			u32 differing_vectors = 0u;
			bool layout_mismatch = false;

			bool Any() const
			{
				return layout_mismatch || differing_vectors != 0u;
			}
		};
		const auto compare_batch_live_ins =
			[generated](const VitaGpuVu::GpuVuDraw& left,
				const VitaGpuVu::GpuVuDraw& right) {
				BatchLiveInVariance variance;
				const auto lane_difference = [](const auto& first,
					const auto& second) {
					u8 lanes = 0u;
					for (u32 lane = 0u; lane < 4u; lane++)
						lanes |= first[lane] != second[lane] ? (1u << lane) : 0u;
					return lanes;
				};
				const auto count_bits = [](u64 bits) {
					u32 count = 0u;
					while (bits != 0u)
					{
						count += static_cast<u32>(bits & 1u);
						bits >>= 1u;
					}
					return count;
				};
				if (left.UniformBlock().Get() != right.UniformBlock().Get())
				{
					const auto& left_constants = left.ConstantUniforms();
					const auto& right_constants = right.ConstantUniforms();
					if (left_constants.size() != right_constants.size() ||
						left_constants.size() > 64u)
					{
						variance.layout_mismatch = true;
						return variance;
					}
					for (u32 index = 0u; index < left_constants.size(); index++)
					{
						if (left_constants[index].input_index !=
							right_constants[index].input_index)
						{
							variance.layout_mismatch = true;
							return variance;
						}
						if (left_constants[index].bits != right_constants[index].bits)
							variance.constant_vectors |= 1ull << index;
					}
					const auto& left_vf = left.VfUniforms();
					const auto& right_vf = right.VfUniforms();
					if (left_vf.size() != right_vf.size())
					{
						variance.layout_mismatch = true;
						return variance;
					}
					for (u32 index = 0u; index < left_vf.size(); index++)
					{
						if (left_vf[index].register_index !=
							right_vf[index].register_index ||
							left_vf[index].register_index >= 32u)
						{
							variance.layout_mismatch = true;
							return variance;
						}
						if (left_vf[index].bits != right_vf[index].bits)
							variance.vf_registers |=
								1u << left_vf[index].register_index;
					}
				}
				if (generated->metadata.uses_acc_uniform &&
					left.acc_uniform != right.acc_uniform)
				{
					variance.acc_lanes =
						lane_difference(left.acc_uniform, right.acc_uniform);
				}
				const bool uses_scalars = generated->metadata.uses_q_uniform ||
					generated->metadata.uses_p_uniform ||
					generated->metadata.uses_i_uniform ||
					generated->metadata.uses_gif_q_uniform;
				if (uses_scalars)
				{
					if (left.scalar_uniforms.present != right.scalar_uniforms.present)
						variance.layout_mismatch = true;
					if (generated->metadata.uses_q_uniform &&
						left.scalar_uniforms.q != right.scalar_uniforms.q)
						variance.scalar_lanes |= 1u << 0u;
					if (generated->metadata.uses_p_uniform &&
						left.scalar_uniforms.p != right.scalar_uniforms.p)
						variance.scalar_lanes |= 1u << 1u;
					if (generated->metadata.uses_i_uniform &&
						left.scalar_uniforms.i != right.scalar_uniforms.i)
						variance.scalar_lanes |= 1u << 2u;
					if (generated->metadata.uses_gif_q_uniform &&
						left.scalar_uniforms.gif_q != right.scalar_uniforms.gif_q)
						variance.scalar_lanes |= 1u << 3u;
				}
				variance.differing_vectors =
					count_bits(variance.constant_vectors) +
					count_bits(variance.vf_registers) +
					static_cast<u32>(variance.acc_lanes != 0u) +
					static_cast<u32>(variance.scalar_lanes != 0u);
				return variance;
			};

		for (u32 first = 0; first < active_gpu_vu_draws->size();)
		{
			ResolvedInputRange window;
			if (!resolve_input_range(*(*active_gpu_vu_draws)[first],
					&window) ||
				window.last_qword - window.first_qword >
					maximum_relative_qword)
			{
					return Reject(
						"GPU-VU generated input publication/window proof failed");
			}

			u32 end = first + 1;
				while (end < active_gpu_vu_draws->size() &&
				end - first <
					std::min(
						std::min(
							VitaGpuVu::GeneratedCgProgram::MaximumBatchDraws,
							maximum_record_draws),
						maximum_indexed_batch_draws))
			{
				ResolvedInputRange next;
					if (!resolve_input_range(*(*active_gpu_vu_draws)[end],
							&next) || next.compact != window.compact ||
						next.uses_raw != window.uses_raw ||
						next.uses_canonical != window.uses_canonical)
				{
					break;
				}
				// ABI 26 retains fixed record-zero live-ins. Physical ABI-27
				// evidence rejected streaming every live-in because it introduced
				// 17 KiB of shader scratch in the hot root.
				const BatchLiveInVariance variance = compare_batch_live_ins(
					*(*active_gpu_vu_draws)[first],
					*(*active_gpu_vu_draws)[end]);
				const VitaGpuVu::GeneratedBatchVaryingLiveIns observed_variance{
					variance.constant_vectors,
					variance.vf_registers,
					variance.acc_lanes != 0u,
					variance.scalar_lanes != 0u};
				if (nested_grid && variance.Any() && !variance.layout_mismatch)
				{
					const bool recorded =
						VitaGpuVu::RecordGeneratedLoopKernelBatchLiveInVariance(
							generated->key, observed_variance);
					if (recorded)
					{
						// Registration completion used to be the only bundle-pump
						// edge.  Live-in variance is learned later, during physical
						// batching, so a compiler-idle run otherwise logged the same
						// split forever and never built its title-neutral BUFFER4
						// specialization.  Pump after releasing the variance-cache
						// lock; source construction/submission is bounded and Shacc
						// remains asynchronous while the base GXP keeps executing.
						VitaGpuVu::RequestGeneratedLoopKernelBundlePump();
					}
				}
				const VitaGpuVu::GeneratedBatchVaryingLiveIns& covered_variance =
					generated->metadata.batch_varying_live_ins;
				const bool uncovered_variance = variance.layout_mismatch ||
					(variance.constant_vectors &
						~covered_variance.constant_mask) != 0u ||
					(variance.vf_registers & ~covered_variance.vf_mask) != 0u ||
					(variance.acc_lanes != 0u && !covered_variance.acc) ||
					(variance.scalar_lanes != 0u && !covered_variance.scalars);
				if (nested_grid && variance.Any() && uncovered_variance)
				{
					gpu_vu_batch_live_in_variance_count++;
					const u64 report = gpu_vu_batch_live_in_variance_count;
					if (report <= 8u || (report & (report - 1u)) == 0u)
					{
						Console.WriteLn(
							"GPU-VU: batch_live_in_variance report=%llu "
							"seq=%llu..%llu constants=%016llx vf=%08x "
							"acc_lanes=%x scalar_lanes=%x vectors=%u layout=%u "
							"action=split-pre-effect.",
							static_cast<unsigned long long>(report),
							static_cast<unsigned long long>(
								(*active_gpu_vu_draws)[first]->ordering_sequence),
							static_cast<unsigned long long>(
								(*active_gpu_vu_draws)[end]->ordering_sequence),
							static_cast<unsigned long long>(variance.constant_vectors),
							variance.vf_registers,
							static_cast<u32>(variance.acc_lanes),
							static_cast<u32>(variance.scalar_lanes),
							variance.differing_vectors,
							static_cast<u32>(variance.layout_mismatch));
					}
					break;
				}
				if (window.compact)
				{
					if (next.compact_qword_count >
							maximum_relative_qword + 1u -
							window.compact_qword_count)
					{
						break;
					}
					window.compact_qword_count += next.compact_qword_count;
					window.last_qword = window.compact_qword_count - 1u;
					end++;
					continue;
				}
				if (next.slot_base != window.slot_base ||
					next.owner != window.owner ||
					next.slot != window.slot ||
					next.generation != window.generation)
				{
					break;
				}
				const u32 combined_first =
					std::min(window.first_qword, next.first_qword);
				const u32 combined_last =
					std::max(window.last_qword, next.last_qword);
				if (combined_last - combined_first >
					maximum_relative_qword)
				{
					break;
				}
				window.first_qword = combined_first;
				window.last_qword = combined_last;
				end++;
			}

			const u32 draw_count = end - first;
			const u64 batch_data_bytes =
				static_cast<u64>(draw_count) * record_vectors *
				sizeof(std::array<u32, 4>);
			const u64 batch_live_in_bytes = inline_batch_varying_tail ? 0u :
				static_cast<u64>(draw_count) * varying_live_in_vectors *
					sizeof(std::array<u32, 4>);
			if (batch_data_bytes > std::numeric_limits<u32>::max() ||
				batch_live_in_bytes > std::numeric_limits<u32>::max())
				return Reject("GPU-VU batch data table is too large");
			VitaGXM::ArenaAllocation allocation;
			const int allocation_result = transfer_arena.Allocate(
				static_cast<u32>(batch_data_bytes), 16, &allocation);
			if (allocation_result < 0 || !allocation)
			{
				return Fail("allocate generated VU1 batch data",
					allocation_result < 0 ? allocation_result :
						SCE_GXM_ERROR_OUT_OF_MEMORY);
			}
			std::memset(allocation.Data(), 0, allocation.Size());
			u32* const batch_data =
				static_cast<u32*>(allocation.Data());
			VitaGXM::ArenaAllocation live_in_allocation;
			u32* batch_live_ins = nullptr;
			if (batch_live_in_bytes != 0u)
			{
				const int live_in_result = transfer_arena.Allocate(
					static_cast<u32>(batch_live_in_bytes), 16u,
					&live_in_allocation);
				if (live_in_result < 0 || !live_in_allocation)
				{
					return Fail("allocate generated VU1 batch live-ins",
						live_in_result < 0 ? live_in_result :
							SCE_GXM_ERROR_OUT_OF_MEMORY);
				}
				std::memset(live_in_allocation.Data(), 0,
					live_in_allocation.Size());
				batch_live_ins =
					static_cast<u32*>(live_in_allocation.Data());
			}
			u32 compact_base_qword = 0u;
			for (u32 object = 0; object < draw_count; object++)
			{
				const VitaGpuVu::GpuVuDraw& candidate =
					*(*active_gpu_vu_draws)[first + object];
				if (candidate.scalar_uniforms.present != expected_scalar_mask)
					return Reject("GPU-VU scalar live-in mask mismatch");
				u32* const record =
					batch_data + object * record_vectors * 4u;
				for (u32 input = 0;
					input < candidate.streams.size(); input++)
				{
					const VitaGpuVu::StreamBinding& binding =
							candidate.streams[input];
						u64 absolute_byte = 0u;
						if (binding.owner ==
							VitaGpuVu::StreamInputOwner::CanonicalVuMemory)
						{
							if (window.compact || !window.uses_canonical ||
								(binding.payload_byte_offset & 15u) != 0u ||
								binding.payload_byte_offset >= VU1_MEMSIZE)
							{
								return Reject(
									"GPU-VU canonical binding is outside VU1 memory");
							}
							absolute_byte = binding.payload_byte_offset;
						}
						else if (binding.owner ==
							VitaGpuVu::StreamInputOwner::RawInput)
						{
							absolute_byte = window.compact ?
								static_cast<u64>(compact_base_qword) * 16u +
									binding.payload_byte_offset :
								static_cast<u64>(candidate.InputPayloads()[
									binding.input_span].offset) +
									binding.payload_byte_offset;
						}
						else
						{
							return Reject("GPU-VU input binding owner is invalid");
						}
					// This is the exact dynamic BUFFER0 expression emitted by the
					// generated root: record_base + maximum affine/grid offset.
					// Prove it again at the final batched/rebased record boundary so
					// neither a malformed descriptor nor batching can turn the mapped
					// fetch guard into logical input capacity.
					u32 relative_qword = 0u;
					if (!VitaGpuVu::ValidateGeneratedBufferReadWindow(
							absolute_byte, binding.payload_byte_extent,
							window.first_qword, window.last_qword,
							maximum_relative_qword, &relative_qword))
					{
						return Reject(
							"GPU-VU generated read escapes its proven input window");
					}
						record[input] = relative_qword;
					}
				u32* const invariant_data = record + binding_vectors * 4u;
				u32* const varying_data =
					!inline_batch_varying_tail && varying_live_in_vectors != 0u ?
					batch_live_ins +
						object * varying_live_in_vectors * 4u : nullptr;
				u32 invariant_vector = 0u;
				u32 varying_vector = 0u;
				const auto copy_live_in = [&](const u32* words, bool varying) {
					u32* destination = nullptr;
					if (varying)
					{
						destination = inline_batch_varying_tail ?
							invariant_data +
								(generated->metadata.
									BatchInvariantUniformVectorCount() +
								 varying_vector++) * 4u :
							(varying_data ? varying_data + varying_vector++ * 4u :
							 nullptr);
					}
					else
					{
						destination = invariant_data + invariant_vector++ * 4u;
					}
					if (!destination)
						return false;
					std::memcpy(destination, words, 4u * sizeof(u32));
					return true;
				};
				const auto& constants = candidate.ConstantUniforms();
				if (constants.size() != generated->metadata.constant_inputs.size())
					return Reject("GPU-VU constant live-in layout mismatch");
				for (u32 index = 0u; index < constants.size(); index++)
				{
					const VitaGpuVu::ConstantUniform& uniform = constants[index];
					if (uniform.input_index != index ||
						generated->metadata.constant_inputs[index].uniform_index !=
							index)
					{
						return Reject("GPU-VU constant live-in index mismatch");
					}
					const bool varying = index < 64u &&
						(generated->metadata.batch_varying_live_ins.constant_mask &
						 (1ull << index)) != 0u;
					if (!copy_live_in(uniform.bits.data(), varying))
						return Reject("GPU-VU constant live-in table overflow");
				}
				const auto& vf_uniforms = candidate.VfUniforms();
				u32 vf_index = 0u;
				for (u32 reg = 1u; reg < 32u; reg++)
				{
					if ((generated->metadata.vf_uniform_mask & (1u << reg)) == 0u)
						continue;
					if (vf_index >= vf_uniforms.size() ||
						vf_uniforms[vf_index].register_index != reg)
					{
						return Reject("GPU-VU VF live-in layout mismatch");
					}
					const VitaGpuVu::VectorUniform& uniform = vf_uniforms[vf_index++];
					if (!copy_live_in(uniform.bits.data(),
							(generated->metadata.batch_varying_live_ins.vf_mask &
							 (1u << reg)) != 0u))
					{
						return Reject("GPU-VU VF live-in table overflow");
					}
				}
				if (vf_index != vf_uniforms.size())
					return Reject("GPU-VU VF live-in count mismatch");
				if (generated->metadata.uses_acc_uniform)
				{
					if (!copy_live_in(candidate.acc_uniform.data(),
							generated->metadata.batch_varying_live_ins.acc))
					{
						return Reject("GPU-VU ACC live-in table overflow");
					}
				}
				if (generated->metadata.uses_q_uniform ||
					generated->metadata.uses_p_uniform ||
					generated->metadata.uses_i_uniform ||
					generated->metadata.uses_gif_q_uniform)
				{
					const std::array<u32, 4> scalars{
						candidate.scalar_uniforms.q,
						candidate.scalar_uniforms.p,
						candidate.scalar_uniforms.i,
						candidate.scalar_uniforms.gif_q};
					if (!copy_live_in(
							scalars.data(),
							generated->metadata.batch_varying_live_ins.scalars))
					{
						return Reject("GPU-VU scalar live-in table overflow");
					}
				}
				if (invariant_vector !=
						generated->metadata.BatchInvariantUniformVectorCount() ||
					varying_vector != varying_live_in_vectors)
					return Reject("GPU-VU batch record layout mismatch");
				if (window.compact)
				{
					const auto& compact_words =
						candidate.CompactRawInputWords();
					compact_base_qword +=
						static_cast<u32>(compact_words.size() / 4u);
				}
				}
			if (window.compact &&
				compact_base_qword != window.compact_qword_count)
			{
				return Reject("compact GPU-VU batch extent changed");
			}

				BatchInputGroup group;
				if (window.compact)
				{
					// The compact owner must satisfy the same physical BUFFER0
					// declaration as a full input-ring slot. Its logical payload may
					// be shorter than 64 qwords, so retain zeroed mapped tail space
					// instead of exposing the following transfer-arena allocation.
					u32 compact_storage_qwords = 0u;
					if (!VitaGpuVu::ResolveGeneratedCompactStorageQwords(
							window.compact_qword_count,
							VitaGpuVu::GeneratedCgProgram::DeclaredBufferVectors,
							&compact_storage_qwords))
					{
						return Reject("compact GPU-VU batch allocation is invalid");
					}
					const u64 compact_bytes = compact_storage_qwords * 16u;
					if (compact_bytes == 0u ||
						compact_bytes > std::numeric_limits<u32>::max())
					{
						return Reject(
							"compact GPU-VU batch allocation overflow");
					}
					const int compact_result = transfer_arena.Allocate(
						static_cast<u32>(compact_bytes), 16u,
						&group.compact_raw_input);
					if (compact_result < 0 || !group.compact_raw_input)
					{
						return Fail("allocate compact generated VU1 input",
							compact_result < 0 ? compact_result :
								SCE_GXM_ERROR_OUT_OF_MEMORY);
					}
					u8* compact_destination = static_cast<u8*>(
						group.compact_raw_input.Data());
					std::memset(compact_destination, 0,
						group.compact_raw_input.Size());
					for (u32 object = 0u; object < draw_count; object++)
					{
						const auto& words = (*active_gpu_vu_draws)[
							first + object]->CompactRawInputWords();
						const size_t bytes = words.size() * sizeof(u32);
						std::memcpy(compact_destination, words.data(), bytes);
						compact_destination += bytes;
					}
					group.raw_buffer_base = static_cast<const u8*>(
						group.compact_raw_input.Data());
				}
				else
				{
					group.raw_buffer_base =
						window.slot_base + window.first_qword * 16u;
				}
				group.owner = window.owner;
			group.slot = window.slot;
			group.generation = window.generation;
			group.first_qword = window.first_qword;
			group.last_qword = window.last_qword;
			group.first_draw = first;
			group.draw_count = draw_count;
			group.batch_data = std::move(allocation);
			group.batch_live_ins = std::move(live_in_allocation);
			const VitaGpuVu::GpuVuDraw& candidate =
				*(*active_gpu_vu_draws)[first];
			if (candidate.HasExactIndices())
			{
				if (!instance_batch_live_ins &&
					!full_nested_batch_domain && !variable_capacity_batch &&
					draw_count != 1u)
					return Reject("exact GPU-VU indices require nested INDEX batching");
				u64 stored_index_count = 0u;
				if (instance_batch_live_ins)
				{
					stored_index_count = candidate.ExactIndices().size();
				}
				else
				{
					for (u32 object = 0u; object < draw_count; object++)
					{
						stored_index_count += (*active_gpu_vu_draws)[
							first + object]->ExactIndices().size();
					}
				}
				const u64 index_bytes = stored_index_count * sizeof(u16);
				if (index_bytes == 0u ||
					index_bytes > std::numeric_limits<u32>::max() ||
					stored_index_count > maximum_gxm_draw_indices)
				{
					return Reject("exact GPU-VU index allocation overflow");
				}
				const int index_result = transfer_arena.Allocate(
					static_cast<u32>(index_bytes), 16u,
					&group.exact_index_data);
				if (index_result < 0 || !group.exact_index_data)
				{
					return Fail("allocate exact generated VU1 indices",
						index_result < 0 ? index_result :
							SCE_GXM_ERROR_OUT_OF_MEMORY);
				}
				u16* const indices = static_cast<u16*>(group.exact_index_data.Data());
				if (instance_batch_live_ins)
				{
					std::memcpy(indices, candidate.ExactIndices().data(),
						static_cast<size_t>(index_bytes));
				}
				else
				{
					size_t destination_offset = 0u;
					for (u32 object = 0u; object < draw_count; object++)
					{
						const VitaGpuVu::GpuVuDraw& indexed_candidate =
							*(*active_gpu_vu_draws)[first + object];
						u16* const destination = indices + destination_offset;
						const bool rebased = variable_capacity_batch ?
							VitaGpuVu::RebaseExactIndicesForGpuVuVariableBatch(
								indexed_candidate.ExactIndices().data(),
								indexed_candidate.ExactIndices().size(),
								indexed_candidate.ShaderIndexDomainCount(),
								nested_batch_stride, object,
								destination) :
							VitaGpuVu::RebaseExactIndicesForGpuVuBatch(
								indexed_candidate.ExactIndices().data(),
								indexed_candidate.ExactIndices().size(),
								indexed_candidate.ShaderIndexDomainCount(), object,
								destination);
						if (!rebased)
						{
							return Reject(
								"exact GPU-VU index escaped its global U16 domain");
						}
						destination_offset +=
							indexed_candidate.ExactIndices().size();
					}
				}
				group.index_count = static_cast<u32>(stored_index_count);
				const u16* const submitted_indices = static_cast<const u16*>(
					group.exact_index_data.Data());
				const auto bounds = std::minmax_element(submitted_indices,
					submitted_indices + group.index_count);
				group.index_minimum = *bounds.first;
				group.index_maximum = *bounds.second;
			}
			else
			{
				const u64 implicit_index_count = static_cast<u64>(
					draw->index_count) * draw_count;
				if (implicit_index_count == 0u ||
					implicit_index_count > std::numeric_limits<u32>::max())
				{
					return Reject(
						"implicit GPU-VU INDEX domain is outside host telemetry bounds");
				}
				group.index_count = static_cast<u32>(implicit_index_count);
				group.index_minimum = 0u;
				group.index_maximum = group.index_count - 1u;
			}
			if (candidate.HasPrivateStoreJournal())
			{
				const u64 compiled_invocations = variable_capacity_batch ?
					nested_invocations : candidate.PrivateStoreInvocationCount();
				VitaGpuVu::GeneratedLoopKernelPrivateOutputBatchLayout
					output_layout;
				if ((!instance_batch_live_ins &&
						 !full_nested_batch_domain && !variable_capacity_batch &&
						 draw_count != 1u) ||
					compiled_invocations > std::numeric_limits<u32>::max() ||
					!VitaGpuVu::ComputeGeneratedLoopKernelPrivateOutputBatchLayout(
						draw_count, static_cast<u32>(compiled_invocations),
						candidate.private_store_count,
						generated->metadata.uses_loop_kernel_ftoi_probe_output,
						GPU_VU_PRIVATE_STORE_GUARD_BYTES, &output_layout))
				{
					return Reject("private GPU-VU output allocation overflow");
				}
				group.private_store_outputs.resize(draw_count);
				GpuVuPrivateStoreOutput& allocation_owner =
					group.private_store_outputs.front();
				const int output_result = transfer_arena.Allocate(
					output_layout.allocation_bytes, 64u,
					&allocation_owner.allocation);
				if (output_result < 0 || !allocation_owner.allocation)
				{
					return Fail("allocate generated VU1 private output",
						output_result < 0 ? output_result :
							SCE_GXM_ERROR_OUT_OF_MEMORY);
				}
				u8* const output_base = static_cast<u8*>(
					allocation_owner.allocation.Data());
				u8* const guard = output_base + output_layout.guard_offset_bytes;
				for (u32 object = 0u; object < draw_count; object++)
				{
					const VitaGpuVu::GpuVuDraw& output_candidate =
						*(*active_gpu_vu_draws)[first + object];
					VitaGpuVu::GeneratedLoopKernelPrivateOutputSlice output_slice;
					if (!VitaGpuVu::ResolveGeneratedLoopKernelPrivateOutputBatchSlice(
							output_layout, object,
							output_candidate.PrivateStoreInvocationCount(),
							&output_slice))
					{
						return Reject(
							"private GPU-VU output active prefix exceeds compiled capacity");
					}
					GpuVuPrivateStoreOutput& output =
						group.private_store_outputs[object];
					output.payload_data =
						output_base + output_slice.payload_offset_bytes;
					output.ftoi_probe_data = output_slice.probe_bytes != 0u ?
						output_base + output_slice.probe_offset_bytes : nullptr;
					output.guard_data = guard;
					output.sequence = output_candidate.ordering_sequence;
					output.payload_bytes = output_slice.payload_bytes;
					output.entry_count = output_slice.entry_count;
					output.ftoi_probe_bytes = output_slice.probe_bytes;
					output.ftoi_probe_count = output_slice.probe_count;
					output.ftoi_probe_outer_count = output_slice.probe_count != 0u ?
						output_candidate.invocation_count /
							generated->metadata.nested_child_iterations : 0u;
					output.ftoi_probe_child_count = output_slice.probe_count != 0u ?
						generated->metadata.nested_child_iterations : 0u;
					output.ftoi_probe_configuration_bits = output_slice.probe_count != 0u ?
						generated->metadata.loop_kernel_ftoi_probe_configuration_bits : 0u;
					output.ftoi_probe_store_index = output_slice.probe_count != 0u ?
						generated->metadata.loop_kernel_ftoi_probe_store_index : 0u;
					output.ftoi_probe_lane = output_slice.probe_count != 0u ?
						generated->metadata.loop_kernel_ftoi_probe_lane : 0u;
					output.ftoi_probe_scale_offset = output_slice.probe_count != 0u ?
						generated->metadata.loop_kernel_ftoi_probe_scale_offset : 0u;
					output.stores_per_invocation =
						output_candidate.private_store_count;
					output.compare_with_cpu_oracle =
						output_candidate.HasPrivateStoreComparison();
					if (output.compare_with_cpu_oracle)
					{
						output.expectations =
							output_candidate.PrivateStoreExpectations();
						output.ftoi_expectations =
							output_candidate.FtoiProbeExpectations();
						output.attestation_identity =
							output_candidate.GeneratedLoopKernelAttestation();
						output.architectural_state_compared =
							output_candidate.PrivateArchitecturalStateCompared();
						output.architectural_state_exact =
							output_candidate.PrivateArchitecturalStateExact();
						output.architectural_state_playable_profile_matches =
							output_candidate.
								PrivateArchitecturalStatePlayableProfileMatches();
						output.architectural_state_mismatch_lanes =
							output_candidate.PrivateArchitecturalStateMismatchLanes();
						output.architectural_state_playable_mismatch_lanes =
							output_candidate.
								PrivateArchitecturalStatePlayableMismatchLanes();
					}
					if (output_candidate.HasGeneratedLoopKernelTransaction())
					{
						output.transaction = output_candidate.
							GeneratedLoopKernelTransactionOwner();
						if (!output.transaction ||
							output.transaction->OutputBytes() !=
								output.payload_bytes ||
							!output.transaction->CanClaimGpuOutputOwner())
						{
							return Reject(
								"generated VU1 transaction output ownership preflight failed");
						}
					}
				}
				VitaGpuVu::GeneratedLoopKernelPrivateOutputWriteExtent
					write_extent;
				if (!VitaGpuVu::
						ResolveGeneratedLoopKernelPrivateOutputWriteExtent(
							group.index_minimum, group.index_maximum,
							candidate.private_store_count,
							output_layout.payload_total_bytes / sizeof(u32),
							output_layout.probe_total_bytes / sizeof(u32),
							&write_extent))
				{
					return Reject(
						"generated VU1 INDEX domain escapes BUFFER2/BUFFER3 output");
				}
				for (GpuVuPrivateStoreOutput& output :
					group.private_store_outputs)
				{
					output.write_extent = write_extent;
				}
				u32* const words = reinterpret_cast<u32*>(output_base);
				std::fill_n(words,
					output_layout.guard_offset_bytes / sizeof(u32),
					GPU_VU_PRIVATE_STORE_SENTINEL);
				std::fill_n(reinterpret_cast<u32*>(guard),
					GPU_VU_PRIVATE_STORE_GUARD_BYTES / sizeof(u32),
					GPU_VU_PRIVATE_STORE_GUARD);
			}
			else if (candidate.HasGeneratedLoopKernelTransaction())
			{
				group.private_store_outputs.resize(draw_count);
				for (u32 object = 0u; object < draw_count; object++)
				{
					const VitaGpuVu::GpuVuDraw& output_candidate =
						*(*active_gpu_vu_draws)[first + object];
					GpuVuPrivateStoreOutput& output =
						group.private_store_outputs[object];
					output.sequence = output_candidate.ordering_sequence;
					output.entry_count = output_candidate.
						GeneratedLoopKernelTransactionOwner()->StoreEntryCount();
					output.transaction = output_candidate.
						GeneratedLoopKernelTransactionOwner();
					output.completion_only = true;
					if (!output.transaction ||
						!output.transaction->CanClaimGpuOutputOwner())
					{
						return Reject(
							"lean generated VU1 completion ownership preflight failed");
					}
				}
			}
			batch_input_groups.push_back(std::move(group));
			first = end;
		}
	}
	std::vector<VitaGXM::ArenaAllocation> direct_exact_index_data;
	std::vector<GpuVuPrivateStoreOutput> direct_private_store_outputs;
	if (!buffered_batch &&
		(draw->HasExactIndices() || draw->HasPrivateStoreJournal() ||
		 draw->HasGeneratedLoopKernelTransaction()))
	{
		direct_exact_index_data.resize(active_gpu_vu_draws->size());
		direct_private_store_outputs.resize(active_gpu_vu_draws->size());
		for (u32 object = 0u; object < active_gpu_vu_draws->size(); object++)
		{
			const VitaGpuVu::GpuVuDraw& candidate =
				*(*active_gpu_vu_draws)[object];
			if (candidate.HasExactIndices())
			{
				const u64 index_bytes =
					static_cast<u64>(candidate.ExactIndices().size()) * sizeof(u16);
				if (index_bytes == 0u ||
					index_bytes > std::numeric_limits<u32>::max())
				{
					return Reject("exact GPU-VU index allocation overflow");
				}
				const int index_result = transfer_arena.Allocate(
					static_cast<u32>(index_bytes), 16u,
					&direct_exact_index_data[object]);
				if (index_result < 0 || !direct_exact_index_data[object])
				{
					return Fail("allocate exact generated VU1 indices",
						index_result < 0 ? index_result :
							SCE_GXM_ERROR_OUT_OF_MEMORY);
				}
				std::memcpy(direct_exact_index_data[object].Data(),
					candidate.ExactIndices().data(), static_cast<size_t>(index_bytes));
			}
			if (!candidate.HasPrivateStoreJournal())
			{
				if (candidate.HasGeneratedLoopKernelTransaction())
				{
					GpuVuPrivateStoreOutput& output =
						direct_private_store_outputs[object];
					output.sequence = candidate.ordering_sequence;
					output.entry_count = candidate.
						GeneratedLoopKernelTransactionOwner()->StoreEntryCount();
					output.transaction =
						candidate.GeneratedLoopKernelTransactionOwner();
					output.completion_only = true;
					if (!output.transaction ||
						!output.transaction->CanClaimGpuOutputOwner())
					{
						return Reject(
							"lean generated VU1 completion ownership preflight failed");
					}
				}
				continue;
			}
			const u64 entry_count =
				static_cast<u64>(candidate.PrivateStoreInvocationCount()) *
				candidate.private_store_count;
			const u64 payload_bytes = entry_count * 4u * sizeof(u32);
			const u64 ftoi_probe_count =
				generated->metadata.uses_loop_kernel_ftoi_probe_output ?
					candidate.PrivateStoreInvocationCount() : 0u;
			const u64 ftoi_probe_bytes =
				ftoi_probe_count * 8u * sizeof(u32);
			const u64 allocation_bytes =
				payload_bytes + ftoi_probe_bytes +
				GPU_VU_PRIVATE_STORE_GUARD_BYTES;
			if (entry_count == 0u ||
				entry_count > std::numeric_limits<u32>::max() ||
				payload_bytes > std::numeric_limits<u32>::max() ||
				ftoi_probe_count > std::numeric_limits<u32>::max() ||
				ftoi_probe_bytes > std::numeric_limits<u32>::max() ||
				allocation_bytes > std::numeric_limits<u32>::max())
			{
				return Reject("private GPU-VU output allocation overflow");
			}
			GpuVuPrivateStoreOutput& output =
				direct_private_store_outputs[object];
			const int output_result = transfer_arena.Allocate(
				static_cast<u32>(allocation_bytes), 64u, &output.allocation);
			if (output_result < 0 || !output.allocation)
			{
				return Fail("allocate generated VU1 private output",
					output_result < 0 ? output_result :
						SCE_GXM_ERROR_OUT_OF_MEMORY);
			}
			output.sequence = candidate.ordering_sequence;
			output.payload_bytes = static_cast<u32>(payload_bytes);
			output.entry_count = static_cast<u32>(entry_count);
			output.ftoi_probe_bytes = static_cast<u32>(ftoi_probe_bytes);
			output.ftoi_probe_count = static_cast<u32>(ftoi_probe_count);
			output.ftoi_probe_outer_count = ftoi_probe_count != 0u ?
				candidate.invocation_count /
					generated->metadata.nested_child_iterations : 0u;
			output.ftoi_probe_child_count = ftoi_probe_count != 0u ?
				generated->metadata.nested_child_iterations : 0u;
			output.ftoi_probe_configuration_bits = ftoi_probe_count != 0u ?
				generated->metadata.loop_kernel_ftoi_probe_configuration_bits : 0u;
			output.ftoi_probe_store_index = ftoi_probe_count != 0u ?
				generated->metadata.loop_kernel_ftoi_probe_store_index : 0u;
			output.ftoi_probe_lane = ftoi_probe_count != 0u ?
				generated->metadata.loop_kernel_ftoi_probe_lane : 0u;
			output.ftoi_probe_scale_offset = ftoi_probe_count != 0u ?
				generated->metadata.loop_kernel_ftoi_probe_scale_offset : 0u;
			output.stores_per_invocation = candidate.private_store_count;
			output.compare_with_cpu_oracle = candidate.HasPrivateStoreComparison();
			if (output.compare_with_cpu_oracle)
			{
				output.expectations = candidate.PrivateStoreExpectations();
				output.ftoi_expectations = candidate.FtoiProbeExpectations();
				output.attestation_identity =
					candidate.GeneratedLoopKernelAttestation();
				output.architectural_state_compared =
					candidate.PrivateArchitecturalStateCompared();
				output.architectural_state_exact =
					candidate.PrivateArchitecturalStateExact();
				output.architectural_state_playable_profile_matches =
					candidate.PrivateArchitecturalStatePlayableProfileMatches();
				output.architectural_state_mismatch_lanes =
					candidate.PrivateArchitecturalStateMismatchLanes();
				output.architectural_state_playable_mismatch_lanes =
					candidate.PrivateArchitecturalStatePlayableMismatchLanes();
			}
			if (candidate.HasGeneratedLoopKernelTransaction())
			{
				output.transaction =
					candidate.GeneratedLoopKernelTransactionOwner();
				if (!output.transaction ||
					output.transaction->OutputBytes() !=
						output.payload_bytes ||
					!output.transaction->CanClaimGpuOutputOwner())
				{
					return Reject(
						"generated VU1 transaction output ownership preflight failed");
				}
			}
			u32 index_minimum = 0u;
			u32 index_maximum = 0u;
			if (candidate.HasExactIndices())
			{
				const auto bounds = std::minmax_element(
					candidate.ExactIndices().begin(), candidate.ExactIndices().end());
				index_minimum = *bounds.first;
				index_maximum = *bounds.second;
			}
			else
			{
				if (candidate.index_count == 0u)
					return Reject("generated VU1 output has an empty INDEX domain");
				index_maximum = candidate.index_count - 1u;
			}
			if (!VitaGpuVu::ResolveGeneratedLoopKernelPrivateOutputWriteExtent(
					index_minimum, index_maximum, candidate.private_store_count,
					output.payload_bytes / sizeof(u32),
					output.ftoi_probe_bytes / sizeof(u32), &output.write_extent))
			{
				return Reject(
					"generated VU1 direct INDEX domain escapes BUFFER2/BUFFER3 output");
			}
			u32* const words = static_cast<u32*>(output.allocation.Data());
			std::fill_n(words, output.TotalPayloadBytes() / sizeof(u32),
				GPU_VU_PRIVATE_STORE_SENTINEL);
			std::fill_n(words + output.TotalPayloadBytes() / sizeof(u32),
				GPU_VU_PRIVATE_STORE_GUARD_BYTES / sizeof(u32),
				GPU_VU_PRIVATE_STORE_GUARD);
		}
	}

	VitaGXM::GSTextureGXM* const rt =
		CheckedCast<VitaGXM::GSTextureGXM>(config.rt);
	VitaGXM::GSTextureGXM* const ds =
		CheckedCast<VitaGXM::GSTextureGXM>(config.ds);
	VitaGXM::GSTextureGXM* draw_rt = rt;
	VitaGXM::GSTextureGXM* draw_ds = ds;
	if (scene_active && !scene_is_display)
	{
		if (!draw_rt && draw_ds && scene_rt && source != scene_rt &&
			(config.ps.no_color || config.colormask.wrgba == 0) &&
			scene_rt->GetSize() == draw_ds->GetSize())
		{
			draw_rt = scene_rt;
		}
		else if (!draw_ds && draw_rt && scene_ds && source != scene_ds &&
			!config.depth.zwe && config.depth.ztst == ZTST_ALWAYS &&
			scene_ds->GetSize() == draw_rt->GetSize())
		{
			draw_ds = scene_ds;
		}
	}
	if (!EnsureScene(draw_rt, draw_ds, config.scissor))
		return false;

	const bool watchdog_already_armed =
		gpu_vu_pre_notification_watchdog_ticket != 0u;
	const u64 first_submission_sequence =
		active_gpu_vu_draws->front()->ordering_sequence;
	gpu_vu_last_generated_sequence =
		active_gpu_vu_draws->back()->ordering_sequence;
	if (!ArmGpuVuPreNotificationWatchdog(first_submission_sequence))
	{
		return Reject(
			"generated GPU-VU pre-notification watchdog is unavailable");
	}
	VitaGS::GpuVuGxmSubmissionBreadcrumb submission_breadcrumb;
	submission_breadcrumb.object_count = static_cast<u32>(
		active_gpu_vu_draws->size());
	for (const auto& candidate : *active_gpu_vu_draws)
	{
		if (!candidate)
			continue;
		submission_breadcrumb.input_payload_count += static_cast<u32>(
			candidate->InputPayloads().size());
		submission_breadcrumb.stream_count += static_cast<u32>(
			candidate->streams.size());
		if (submission_breadcrumb.input_owner == 0u &&
			!candidate->InputPayloads().empty())
		{
			const VitaGpuVu::RawVifPayloadRef& payload =
				candidate->InputPayloads()[0];
			submission_breadcrumb.input_owner = payload.owner;
			submission_breadcrumb.input_slot = payload.slot;
			submission_breadcrumb.input_generation = payload.generation;
		}
	}
	const auto include_private_outputs = [&submission_breadcrumb](
		const std::vector<GpuVuPrivateStoreOutput>& outputs) {
		for (const GpuVuPrivateStoreOutput& output : outputs)
		{
			if (output.transaction)
				submission_breadcrumb.private_transaction_count++;
			if (submission_breadcrumb.output_address == 0u &&
				output.HasOutputStorage())
			{
				submission_breadcrumb.output_address =
					reinterpret_cast<uptr>(output.PayloadData());
				submission_breadcrumb.output_bytes =
					output.TotalPayloadBytes();
			}
		}
	};
	for (const BatchInputGroup& group : batch_input_groups)
		include_private_outputs(group.private_store_outputs);
	include_private_outputs(direct_private_store_outputs);
	VitaGS::UpdateGpuVuGxmSubmissionWatchdogBreadcrumb(
		gpu_vu_pre_notification_watchdog_ticket, submission_breadcrumb);
	bool encoded_any = false;
	struct GpuVuPreNotificationWatchdogScope
	{
		Impl* owner = nullptr;
		bool armed_here = false;
		bool retained = false;
		const bool* encoded_any = nullptr;

		~GpuVuPreNotificationWatchdogScope()
		{
			// A pre-effect rejection owns no firmware work and may cancel the
			// newly armed scene ticket. Once even one draw has been encoded,
			// however, a returning libGXM error does not prove that firmware
			// stopped consuming the command stream. Keep that ticket armed until
			// EndScene/MidSceneFlush bridges a completion notification, or until
			// the title-only watchdog terminates the process.
			if (owner && VitaGpuVu::CanCancelGpuVuPreNotificationWatchdog(
					armed_here, retained, encoded_any && *encoded_any))
			{
				owner->CompleteGpuVuPreNotificationWatchdog();
			}
		}
		} watchdog_scope{
			this, !watchdog_already_armed, false, &encoded_any};
	bool failed_submission_resources_retained = false;
	const auto retain_failed_submission_resources = [&]() {
		if (failed_submission_resources_retained)
			return true;
		UpdateGpuVuPreNotificationWatchdog(
			VitaGS::GpuVuGxmSubmissionStage::ResourceOwnershipTransfer,
			active_gpu_vu_draws->back()->ordering_sequence);

		// Sony requires every vertex stream, index buffer and vertex uniform
		// buffer referenced by an encoded draw to remain valid until vertex
		// completion.  The normal success path transfers these owners below after
		// the complete batch is encoded.  A later bind/draw/transaction failure
		// must perform the same transfer before stack unwinding can recycle an
		// ArenaAllocation or release an immutable VIF input generation.
		bool inputs_retained = true;
		for (const auto& candidate : *active_gpu_vu_draws)
		{
			if (!candidate)
			{
				inputs_retained = false;
				continue;
			}
			for (size_t input_index = 0;
				input_index < candidate->InputPayloads().size(); input_index++)
			{
				const VitaGpuVu::RawVifPayloadRef& payload =
					candidate->InputPayloads()[input_index];
				if (ContainsGpuVuInputSlot(gpu_vu_scene_input_retentions,
						gpu_vu_scene_input_retention_count, payload))
				{
					continue;
				}
				if (!AdoptGpuVuInputSlot(candidate.get(), input_index,
						&gpu_vu_scene_input_retentions,
						&gpu_vu_scene_input_retention_count))
				{
					inputs_retained = false;
				}
			}
		}

		const auto retain_allocation = [this](VitaGXM::ArenaAllocation* allocation) {
			if (allocation && *allocation)
			{
				gpu_vu_scene_batch_allocations.push_back(std::move(*allocation));
			}
		};
		retain_allocation(&structured_scratch);
		for (BatchInputGroup& group : batch_input_groups)
		{
			retain_allocation(&group.compact_raw_input);
			retain_allocation(&group.batch_data);
			retain_allocation(&group.batch_live_ins);
			retain_allocation(&group.exact_index_data);
			for (GpuVuPrivateStoreOutput& output : group.private_store_outputs)
			{
				if (output.HasOutputStorage() || output.completion_only)
				{
					gpu_vu_scene_private_store_outputs.push_back(
						std::move(output));
				}
			}
		}
		for (VitaGXM::ArenaAllocation& allocation : direct_exact_index_data)
			retain_allocation(&allocation);
		for (GpuVuPrivateStoreOutput& output : direct_private_store_outputs)
		{
			if (output.HasOutputStorage() || output.completion_only)
			{
				gpu_vu_scene_private_store_outputs.push_back(std::move(output));
			}
		}
		failed_submission_resources_retained = true;
		UpdateGpuVuPreNotificationWatchdog(
			VitaGS::GpuVuGxmSubmissionStage::ResourcesRetained,
			active_gpu_vu_draws->back()->ordering_sequence);
		return inputs_retained;
	};
	const auto reject_generated_submission =
			[this, &encoded_any, first_submission_sequence,
				&retain_failed_submission_resources](const char* reason) {
				const bool retained = !encoded_any ||
					retain_failed_submission_resources();
				Reject(reason);
				if (!encoded_any)
					return false;
				if (!retained)
				{
					Console.Error(
						"GPU-VU: post-encode input ownership transfer failed; "
						"all reachable mapped allocations remain quarantined.");
				}
				return FailGpuVuRetirementOwner(reason, 0u, 0u, 0u,
					first_submission_sequence, 0u);
			};
	const auto fail_generated_submission =
			[this, &encoded_any, first_submission_sequence,
				&retain_failed_submission_resources](
				const char* operation, int error) {
				const bool retained = !encoded_any ||
					retain_failed_submission_resources();
				Fail(operation, error);
				if (!encoded_any)
					return false;
				if (!retained)
				{
					Console.Error(
						"GPU-VU: post-encode input ownership transfer failed; "
						"all reachable mapped allocations remain quarantined.");
				}
				return FailGpuVuRetirementOwner(operation, 0u, 0u, 0u,
					first_submission_sequence, 0u);
			};

	SceGxmFragmentProgram* const generated_final_fragment = config.ps.zfloor ?
		generated->zfloor_fragment_program : (fast_fragment ?
			generated->opaque_fragment_program :
			generated->general_fragment_program);
	UpdateGpuVuPreNotificationWatchdog(
		VitaGS::GpuVuGxmSubmissionStage::VertexProgramBinding,
		first_submission_sequence);
	sceGxmSetVertexProgram(context, generated->vertex_program);
	UpdateGpuVuPreNotificationWatchdog(
		VitaGS::GpuVuGxmSubmissionStage::FragmentProgramBinding,
		first_submission_sequence);
	sceGxmSetFragmentProgram(context, generated_final_fragment);
	VitaGXM::GSTextureGXM* bound_source = source ? source : white_texture.get();
	if (!bound_source)
		return false;
	SceGxmTexture& native_texture = bound_source->Texture();
	const bool linear_strided =
		sceGxmTextureGetType(&native_texture) == SCE_GXM_TEXTURE_LINEAR_STRIDED;
	if (linear_strided && (config.sampler.tau || config.sampler.tav))
		return Reject("repeat addressing on a linear-strided GXM texture");
	if (bound_source->GetMipmapLevels() > 1 &&
		!config.ps.automatic_lod && !config.ps.manual_lod)
	{
		return Reject("multi-level texture without an explicit or implicit LOD contract");
	}
	if (bound_source->GetMipmapLevels() > 1 && config.sampler.lodclamp)
		return Reject("LOD0 clamp on a multi-level GXM texture");
	int result = sceGxmTextureSetUAddrMode(&native_texture,
		config.sampler.tau ? SCE_GXM_TEXTURE_ADDR_REPEAT :
			SCE_GXM_TEXTURE_ADDR_CLAMP);
	if (result >= 0)
	{
		result = sceGxmTextureSetVAddrMode(&native_texture,
			config.sampler.tav ? SCE_GXM_TEXTURE_ADDR_REPEAT :
				SCE_GXM_TEXTURE_ADDR_CLAMP);
	}
	const SceGxmTextureFilter min_filter = (!config.ps.ltf &&
		config.sampler.IsMinFilterLinear()) ? SCE_GXM_TEXTURE_FILTER_LINEAR :
		SCE_GXM_TEXTURE_FILTER_POINT;
	const SceGxmTextureFilter mag_filter = (!config.ps.ltf &&
		config.sampler.IsMagFilterLinear()) ? SCE_GXM_TEXTURE_FILTER_LINEAR :
		SCE_GXM_TEXTURE_FILTER_POINT;
	if (result >= 0 && !linear_strided)
		result = sceGxmTextureSetMinFilter(&native_texture, min_filter);
	if (result >= 0)
		result = sceGxmTextureSetMagFilter(&native_texture, mag_filter);
	if (result >= 0 && !linear_strided)
	{
		result = sceGxmTextureSetMipFilter(&native_texture,
			config.sampler.IsMipFilterLinear() ?
				SCE_GXM_TEXTURE_MIP_FILTER_ENABLED :
				SCE_GXM_TEXTURE_MIP_FILTER_DISABLED);
	}
	if (result < 0)
		return Fail("configure generated VU1+TFX sampler", result);
	UpdateGpuVuPreNotificationWatchdog(
		VitaGS::GpuVuGxmSubmissionStage::TextureBinding,
		first_submission_sequence);
	result = sceGxmSetFragmentTexture(context, 0, &native_texture);
	if (result < 0)
		return Fail("bind generated VU1+TFX source texture", result);
	bound_source->MarkSceneUse(scene_serial);
		const auto populate_generated_gxm_identity =
			[this, &config, bound_source, &native_texture](
				VitaGS::GpuVuGxmCallBreadcrumb& breadcrumb,
			const SceGxmVertexProgram* vertex_program,
			const SceGxmFragmentProgram* fragment_program,
			SceGxmPrimitiveType submitted_primitive) {
			breadcrumb.vertex_program = reinterpret_cast<uptr>(vertex_program);
			breadcrumb.fragment_program = reinterpret_cast<uptr>(fragment_program);
			breadcrumb.source_texture_object = reinterpret_cast<uptr>(bound_source);
			breadcrumb.source_texture_descriptor =
				reinterpret_cast<uptr>(&native_texture);
				breadcrumb.source_texture_data = reinterpret_cast<uptr>(
					sceGxmTextureGetData(&native_texture));
				breadcrumb.source_texture_storage_bytes = static_cast<u32>(
					std::min<size_t>(bound_source->StorageSize(),
						std::numeric_limits<u32>::max()));
				breadcrumb.scene_render_target = reinterpret_cast<uptr>(scene_rt);
				breadcrumb.scene_depth_target = reinterpret_cast<uptr>(scene_ds);
				if (scene_rt)
				{
					breadcrumb.render_target_data = reinterpret_cast<uptr>(
						scene_rt->LevelData(0u));
					breadcrumb.render_target_storage_bytes = static_cast<u32>(
						std::min<size_t>(scene_rt->StorageSize(),
							std::numeric_limits<u32>::max()));
				}
				if (scene_ds)
				{
					breadcrumb.depth_target_data = reinterpret_cast<uptr>(
						scene_ds->LevelData(0u));
					breadcrumb.depth_stencil_data = reinterpret_cast<uptr>(
						scene_ds->StencilData());
					breadcrumb.depth_target_storage_bytes = static_cast<u32>(
						std::min<size_t>(scene_ds->StorageSize(),
							std::numeric_limits<u32>::max()));
					breadcrumb.depth_stencil_storage_bytes = static_cast<u32>(
						std::min<size_t>(scene_ds->StencilStorageSize(),
							std::numeric_limits<u32>::max()));
				}
			breadcrumb.ps_selector_low = config.ps.key_lo;
			breadcrumb.ps_selector_high = config.ps.key_hi;
			breadcrumb.primitive_type = static_cast<u32>(submitted_primitive);
			breadcrumb.topology = static_cast<u32>(config.topology);
			breadcrumb.sampler_key = config.sampler.key;
			breadcrumb.blend_key = config.blend.key;
			breadcrumb.color_mask_key = config.colormask.key;
			breadcrumb.depth_key = config.depth.key;
			breadcrumb.texture_type = static_cast<u32>(
				sceGxmTextureGetType(&native_texture));
			breadcrumb.texture_format = static_cast<u32>(
				sceGxmTextureGetFormat(&native_texture));
			breadcrumb.texture_width = sceGxmTextureGetWidth(&native_texture);
			breadcrumb.texture_height = sceGxmTextureGetHeight(&native_texture);
			breadcrumb.texture_stride = sceGxmTextureGetStride(&native_texture);
			breadcrumb.texture_mipmap_count =
				sceGxmTextureGetMipmapCount(&native_texture);
				breadcrumb.texture_sampler_state =
				(static_cast<u32>(sceGxmTextureGetMinFilter(&native_texture)) & 0xfu) |
				((static_cast<u32>(sceGxmTextureGetMagFilter(&native_texture)) & 0xfu) << 4u) |
				((static_cast<u32>(sceGxmTextureGetMipFilter(&native_texture)) & 0xfu) << 8u) |
				((static_cast<u32>(sceGxmTextureGetUAddrMode(&native_texture)) & 0xffu) << 12u) |
					((static_cast<u32>(sceGxmTextureGetVAddrMode(&native_texture)) & 0xffu) << 20u);
			};
		const auto populate_generated_resource_identity =
			[bound_source, this](VitaGS::GpuVuGxmCallBreadcrumb& breadcrumb,
				const void* buffer0, u32 buffer0_bytes, u32 buffer0_read_bytes,
				const VitaGXM::ArenaAllocation* buffer1,
				const GpuVuPrivateStoreOutput* output,
				const VitaGXM::ArenaAllocation* buffer4,
				const void* index_data, u32 index_bytes, u32 draw_flags) {
				breadcrumb.buffer0_address = reinterpret_cast<uptr>(buffer0);
				breadcrumb.buffer0_bytes = buffer0_bytes;
				breadcrumb.buffer0_hash = HashGpuVuDiagnosticBytes(buffer0,
					std::min(buffer0_bytes, buffer0_read_bytes),
					&breadcrumb.buffer0_hash_bytes);
				if (buffer1 && *buffer1)
				{
					breadcrumb.buffer1_address = reinterpret_cast<uptr>(
						buffer1->Data());
					breadcrumb.buffer1_bytes = static_cast<u32>(std::min<size_t>(
						buffer1->Size(), std::numeric_limits<u32>::max()));
					breadcrumb.buffer1_hash = HashGpuVuDiagnosticBytes(buffer1->Data(),
						breadcrumb.buffer1_bytes, &breadcrumb.buffer1_hash_bytes);
				}
				if (output && output->HasOutputStorage())
				{
					breadcrumb.buffer2_address = reinterpret_cast<uptr>(
						output->PayloadData());
					breadcrumb.buffer2_bytes =
						output->write_extent.payload_capacity_words * sizeof(u32);
					if (output->HasFtoiProbe())
					{
						breadcrumb.buffer3_address = reinterpret_cast<uptr>(
							output->FtoiProbeData());
						breadcrumb.buffer3_bytes =
							output->write_extent.probe_capacity_words * sizeof(u32);
					}
				}
				if (buffer4 && *buffer4)
				{
					breadcrumb.buffer4_address = reinterpret_cast<uptr>(
						buffer4->Data());
					breadcrumb.buffer4_bytes = static_cast<u32>(std::min<size_t>(
						buffer4->Size(), std::numeric_limits<u32>::max()));
					breadcrumb.buffer4_hash = HashGpuVuDiagnosticBytes(buffer4->Data(),
						breadcrumb.buffer4_bytes, &breadcrumb.buffer4_hash_bytes);
				}
				breadcrumb.index_address = reinterpret_cast<uptr>(index_data);
				breadcrumb.index_bytes = index_bytes;
				breadcrumb.index_hash = HashGpuVuDiagnosticBytes(index_data, index_bytes,
					&breadcrumb.index_hash_bytes);
				breadcrumb.draw_flags = draw_flags;

				struct Range
				{
					uptr address = 0u;
					u32 bytes = 0u;
				};
				const std::array<Range, 6> vertex_ranges{{
					{breadcrumb.buffer0_address, breadcrumb.buffer0_bytes},
					{breadcrumb.buffer1_address, breadcrumb.buffer1_bytes},
					{breadcrumb.buffer2_address, breadcrumb.buffer2_bytes},
					{breadcrumb.buffer3_address, breadcrumb.buffer3_bytes},
					{breadcrumb.buffer4_address, breadcrumb.buffer4_bytes},
					{breadcrumb.index_address, breadcrumb.index_bytes},
				}};
				u32 overlap = 0u;
				u32 pair_bit = 0u;
				for (u32 first = 0u; first < vertex_ranges.size(); first++)
				{
					for (u32 second = first + 1u; second < vertex_ranges.size(); second++)
					{
						if (GpuVuDiagnosticRangesOverlap(vertex_ranges[first].address,
								vertex_ranges[first].bytes, vertex_ranges[second].address,
								vertex_ranges[second].bytes))
						{
							overlap |= 1u << pair_bit;
						}
						pair_bit++;
					}
				}
				const Range source{reinterpret_cast<uptr>(bound_source->LevelData(0u)),
					static_cast<u32>(std::min<size_t>(bound_source->StorageSize(),
						std::numeric_limits<u32>::max()))};
				const Range render{breadcrumb.render_target_data,
					breadcrumb.render_target_storage_bytes};
				const Range depth{breadcrumb.depth_target_data,
					breadcrumb.depth_target_storage_bytes};
				const Range stencil{breadcrumb.depth_stencil_data,
					breadcrumb.depth_stencil_storage_bytes};
				if (GpuVuDiagnosticRangesOverlap(source.address, source.bytes,
						render.address, render.bytes))
					overlap |= 1u << 16u;
				if (GpuVuDiagnosticRangesOverlap(source.address, source.bytes,
						depth.address, depth.bytes) ||
					GpuVuDiagnosticRangesOverlap(source.address, source.bytes,
						stencil.address, stencil.bytes))
					overlap |= 1u << 17u;
				for (const Range& range : vertex_ranges)
				{
					if (GpuVuDiagnosticRangesOverlap(range.address, range.bytes,
							source.address, source.bytes))
						overlap |= 1u << 18u;
					if (GpuVuDiagnosticRangesOverlap(range.address, range.bytes,
							render.address, render.bytes))
						overlap |= 1u << 19u;
					if (GpuVuDiagnosticRangesOverlap(range.address, range.bytes,
							depth.address, depth.bytes) ||
						GpuVuDiagnosticRangesOverlap(range.address, range.bytes,
							stencil.address, stencil.bytes))
						overlap |= 1u << 20u;
				}
				breadcrumb.resource_overlap_mask = overlap;
			};

		UpdateGpuVuPreNotificationWatchdog(
		VitaGS::GpuVuGxmSubmissionStage::RasterStateBinding,
		first_submission_sequence);
	const SceGxmDepthFunc depth_func = TranslateDepthFunc(config.depth.ztst);
	sceGxmSetFrontDepthFunc(context, depth_func);
	sceGxmSetBackDepthFunc(context, depth_func);
	const SceGxmDepthWriteMode depth_write = config.depth.zwe ?
		SCE_GXM_DEPTH_WRITE_ENABLED : SCE_GXM_DEPTH_WRITE_DISABLED;
	sceGxmSetFrontDepthWriteEnable(context, depth_write);
	sceGxmSetBackDepthWriteEnable(context, depth_write);
	if (!UploadTfxUniforms(config, config.ps, source, false, false,
			fast_fragment, false, false, false, false, false, false, false,
			false, false, generated, draw))
	{
		return false;
	}
	if (generated_pipeline_uses_arm_estimate_table)
	{
#if defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
		if (!gpu_vu_generated_estimate_table.IsMapped())
			return Reject("generated VU1 ARM estimate table is unavailable");
		UpdateGpuVuPreNotificationWatchdog(
			VitaGS::GpuVuGxmSubmissionStage::AuxiliaryUniformBinding,
			first_submission_sequence);
		const int estimate_result = sceGxmSetVertexUniformBuffer(
			context, 7, gpu_vu_generated_estimate_table.base);
		if (estimate_result < 0)
			return Fail("bind generated VU1 ARM estimate table", estimate_result);
#else
		return Reject("generated VU1 ARM estimate table owner is disabled");
#endif
	}

	u64 producer_draw_count = 0u;
	u64 final_draw_count = 0u;
	if (staged_generated_direct)
	{
		ConfigureGpuVuComputeRaster(context);
		const auto abandon_staged_submission = [this]() {
			RestoreGpuVuComputeRaster(context);
		};
		for (u32 stage = 0; stage < draw->precompute_stage_count; stage++)
		{
			bool stage_encoded = false;
			for (u32 module = 0;
				module < draw->precompute_program_count; module++)
			{
				if (draw->precompute_stages[module] != stage)
					continue;
				GeneratedVuProgram* const producer =
					precompute_programs[module];
				UpdateGpuVuPreNotificationWatchdog(
					VitaGS::GpuVuGxmSubmissionStage::PrecomputeProgramBinding,
					active_gpu_vu_draws->front()->ordering_sequence);
				sceGxmSetVertexProgram(context, producer->vertex_program);
				sceGxmSetFragmentProgram(
					context, producer->general_fragment_program);
				for (u32 object = 0;
					object < active_gpu_vu_draws->size(); object++)
				{
					const VitaGpuVu::GpuVuDraw& candidate =
						*(*active_gpu_vu_draws)[object];
					UpdateGpuVuPreNotificationWatchdog(
						VitaGS::GpuVuGxmSubmissionStage::DrawBinding,
						candidate.ordering_sequence);
					const u8* const input =
						structured_direct_inputs[object];
					u8* const scratch =
						static_cast<u8*>(structured_scratch.Data()) +
						object * structured_scratch_stride;
					result = sceGxmSetVertexUniformBuffer(context, 3,
						input +
							VitaGpuVu::GeneratedNestedDirectMemoryWordOffset *
								sizeof(u32));
					if (result >= 0)
						result = sceGxmSetVertexUniformBuffer(
							context, 10, scratch);
					if (result >= 0)
						result = sceGxmSetVertexUniformBuffer(context, 11,
							input + VitaGpuVu::
								GeneratedNestedDirectSnapshotWordOffset *
									sizeof(u32));
					if (result >= 0)
						result = sceGxmSetVertexUniformBuffer(context, 12,
							input + VitaGpuVu::
								GeneratedNestedDirectOuterStateWordOffset *
									sizeof(u32));
					if (result >= 0)
						result = sceGxmSetVertexUniformBuffer(context, 13,
							input + VitaGpuVu::
								GeneratedNestedDirectViSnapshotWordOffset *
									sizeof(u32));
					if (result >= 0)
					{
						// Once sceGxmDraw() is entered, a negative return does not
						// prove that firmware retained no reference to the bound
						// mappings. Treat ownership as uncertain before the call so
						// its failure path quarantines every reachable resource.
						UpdateGpuVuPreNotificationWatchdog(
							VitaGS::GpuVuGxmSubmissionStage::DrawCall,
							candidate.ordering_sequence);
						encoded_any = true;
						VitaGS::GpuVuGxmCallBreadcrumb call_breadcrumb;
						call_breadcrumb.sequence_end =
							active_gpu_vu_draws->back()->ordering_sequence;
						call_breadcrumb.program_key_high = producer->key.high;
						call_breadcrumb.program_key_low = producer->key.low;
						call_breadcrumb.program_abi =
							producer->metadata.loop_kernel_source_abi;
						call_breadcrumb.input_owner =
							submission_breadcrumb.input_owner;
						call_breadcrumb.input_slot =
							submission_breadcrumb.input_slot;
						call_breadcrumb.input_generation =
							submission_breadcrumb.input_generation;
						call_breadcrumb.phase_index = stage;
						call_breadcrumb.module_index = module;
						call_breadcrumb.object_index = object;
						call_breadcrumb.index_count = candidate.invocation_count;
						call_breadcrumb.object_count = static_cast<u32>(
							active_gpu_vu_draws->size());
						call_breadcrumb.private_transaction_count =
							submission_breadcrumb.private_transaction_count;
						call_breadcrumb.output_address =
							submission_breadcrumb.output_address;
						call_breadcrumb.output_bytes =
							submission_breadcrumb.output_bytes;
						populate_generated_gxm_identity(call_breadcrumb,
							producer->vertex_program,
							producer->general_fragment_program,
							SCE_GXM_PRIMITIVE_POINTS);
						AccumulateGpuVuGeneratedSceneManifest(
							VitaGS::GpuVuGxmCallKind::GeneratedPrecomputeDraw,
							candidate.ordering_sequence, call_breadcrumb);
						const u32 call_token = VitaGS::BeginGpuVuGxmCall(
							VitaGS::GpuVuGxmCallKind::GeneratedPrecomputeDraw,
							candidate.ordering_sequence, scene_serial,
							call_breadcrumb);
						result = sceGxmDraw(context,
							SCE_GXM_PRIMITIVE_POINTS,
							SCE_GXM_INDEX_FORMAT_U16,
							gpu_vu_sequential_indices,
							candidate.invocation_count);
						VitaGS::CompleteGpuVuGxmCall(call_token, result);
					}
					if (result < 0)
					{
						abandon_staged_submission();
						return fail_generated_submission(
							"submit generated VU1 precompute module", result);
					}
					UpdateGpuVuPreNotificationWatchdog(
						VitaGS::GpuVuGxmSubmissionStage::DrawReturned,
						candidate.ordering_sequence);
					stage_encoded = true;
					producer_draw_count++;
					RecordGpuVuGxmDraw(candidate.invocation_count);
				}
			}
			if (!stage_encoded)
			{
				abandon_staged_submission();
				return reject_generated_submission(
					"generated VU1 dependency stage encoded no work");
			}
			UpdateGpuVuPreNotificationWatchdog(
				VitaGS::GpuVuGxmSubmissionStage::MidSceneFlush,
				active_gpu_vu_draws->back()->ordering_sequence);
			VitaGS::GpuVuGxmCallBreadcrumb flush_breadcrumb;
			flush_breadcrumb.sequence_end =
				active_gpu_vu_draws->back()->ordering_sequence;
			flush_breadcrumb.phase_index = stage;
			flush_breadcrumb.object_count = static_cast<u32>(
				active_gpu_vu_draws->size());
			const u32 flush_token = VitaGS::BeginGpuVuGxmCall(
				VitaGS::GpuVuGxmCallKind::MidSceneFlush,
				active_gpu_vu_draws->front()->ordering_sequence,
				scene_serial, flush_breadcrumb);
			result = sceGxmMidSceneFlush(context,
				SCE_GXM_MIDSCENE_PRESERVE_DEFAULT_UNIFORM_BUFFERS,
				nullptr, nullptr);
			VitaGS::CompleteGpuVuGxmCall(flush_token, result);
			if (result < 0)
			{
				abandon_staged_submission();
				return fail_generated_submission(
					"flush generated VU1 precompute dependency stage",
					result);
			}
			UpdateGpuVuPreNotificationWatchdog(
				VitaGS::GpuVuGxmSubmissionStage::DrawEncoding,
				active_gpu_vu_draws->back()->ordering_sequence);
		}
		RestoreGpuVuComputeRaster(context);
		// The producer programs use a no-color POINT link. Restore the fused
		// VU+TFX link after the final BUFFER10 visibility boundary.
		UpdateGpuVuPreNotificationWatchdog(
			VitaGS::GpuVuGxmSubmissionStage::FinalProgramRestore,
			active_gpu_vu_draws->back()->ordering_sequence);
		sceGxmSetVertexProgram(context, generated->vertex_program);
		sceGxmSetFragmentProgram(context, generated_final_fragment);
	}

	UpdateGpuVuPreNotificationWatchdog(
		VitaGS::GpuVuGxmSubmissionStage::FinalRasterBinding,
		first_submission_sequence);
	// A candidate which still carries a CPU private-store oracle is not an
	// output provider. Sony's libGXM contract lets either front/back fragment
	// enable suppress fragment processing; pair that with disabled depth writes
	// so this draw can execute its vertex-stage BUFFER2 journal without touching
	// the guest-visible render/depth targets. The CPU PATH1 packet at this same
	// reservation remains authoritative until the comparison passes.
	struct PrivateAttestationRasterScope
	{
		SceGxmContext* context = nullptr;
		SceGxmDepthWriteMode restore_depth_write =
			SCE_GXM_DEPTH_WRITE_DISABLED;
		bool active = false;

		PrivateAttestationRasterScope(SceGxmContext* draw_context,
			SceGxmDepthWriteMode depth_write_mode, bool enabled)
			: context(draw_context), restore_depth_write(depth_write_mode),
			  active(enabled)
		{
			if (!active)
				return;
			sceGxmSetFrontFragmentProgramEnable(
				context, SCE_GXM_FRAGMENT_PROGRAM_DISABLED);
			sceGxmSetBackFragmentProgramEnable(
				context, SCE_GXM_FRAGMENT_PROGRAM_DISABLED);
			sceGxmSetFrontDepthWriteEnable(
				context, SCE_GXM_DEPTH_WRITE_DISABLED);
			sceGxmSetBackDepthWriteEnable(
				context, SCE_GXM_DEPTH_WRITE_DISABLED);
			// ConfigureRaster/ConfigureScissor establish ALWAYS/KEEP on both
			// faces. Explicit zero write masks isolate the private point draw.
			sceGxmSetFrontStencilFunc(context, SCE_GXM_STENCIL_FUNC_ALWAYS,
				SCE_GXM_STENCIL_OP_KEEP, SCE_GXM_STENCIL_OP_KEEP,
				SCE_GXM_STENCIL_OP_KEEP, 0xff, 0);
			sceGxmSetBackStencilFunc(context, SCE_GXM_STENCIL_FUNC_ALWAYS,
				SCE_GXM_STENCIL_OP_KEEP, SCE_GXM_STENCIL_OP_KEEP,
				SCE_GXM_STENCIL_OP_KEEP, 0xff, 0);
		}

		~PrivateAttestationRasterScope()
		{
			if (!active)
				return;
			sceGxmSetFrontPolygonMode(context, SCE_GXM_POLYGON_MODE_TRIANGLE_FILL);
			sceGxmSetBackPolygonMode(context, SCE_GXM_POLYGON_MODE_TRIANGLE_FILL);
			sceGxmSetFrontStencilFunc(context, SCE_GXM_STENCIL_FUNC_ALWAYS,
				SCE_GXM_STENCIL_OP_KEEP, SCE_GXM_STENCIL_OP_KEEP,
				SCE_GXM_STENCIL_OP_KEEP, 0xff, 0xff);
			sceGxmSetBackStencilFunc(context, SCE_GXM_STENCIL_FUNC_ALWAYS,
				SCE_GXM_STENCIL_OP_KEEP, SCE_GXM_STENCIL_OP_KEEP,
				SCE_GXM_STENCIL_OP_KEEP, 0xff, 0xff);
			sceGxmSetFrontDepthWriteEnable(context, restore_depth_write);
			sceGxmSetBackDepthWriteEnable(context, restore_depth_write);
			sceGxmSetFrontFragmentProgramEnable(
				context, SCE_GXM_FRAGMENT_PROGRAM_ENABLED);
			sceGxmSetBackFragmentProgramEnable(
				context, SCE_GXM_FRAGMENT_PROGRAM_ENABLED);
		}
	} private_attestation_raster(
		context, depth_write, draw->HasPrivateStoreComparison());

	const bool point_topology = topology == GSHWDrawConfig::Topology::Point;
	const bool line_topology = topology == GSHWDrawConfig::Topology::Line;
	if (point_topology)
	{
		sceGxmSetFrontPolygonMode(context, SCE_GXM_POLYGON_MODE_POINT_01UV);
		sceGxmSetBackPolygonMode(context, SCE_GXM_POLYGON_MODE_POINT_01UV);
	}
	else if (line_topology)
	{
		sceGxmSetFrontPolygonMode(context, SCE_GXM_POLYGON_MODE_LINE);
		sceGxmSetBackPolygonMode(context, SCE_GXM_POLYGON_MODE_LINE);
		sceGxmSetFrontPointLineWidth(context, 1);
		sceGxmSetBackPointLineWidth(context, 1);
	}
	if (buffered_batch)
	{
		// ABI 26 uses a rebased global INDEX domain. The dormant instance branch
		// is retained only so stale metadata is rejected cleanly; ABI-27 physical
		// resource evidence prevents product programs from selecting it.
		u32 batch_group_index = 0u;
		for (BatchInputGroup& group : batch_input_groups)
		{
			const u64 group_sequence = (*active_gpu_vu_draws)[
				group.first_draw]->ordering_sequence;
			UpdateGpuVuPreNotificationWatchdog(
				VitaGS::GpuVuGxmSubmissionStage::DrawBinding,
				group_sequence);
			result = sceGxmSetVertexUniformBuffer(
				context, 0, group.raw_buffer_base);
			if (result >= 0)
			{
				result = sceGxmSetVertexUniformBuffer(
					context, 1, group.batch_data.Data());
			}
			if (result >= 0 && !group.private_store_outputs.empty() &&
				group.private_store_outputs.front().HasOutputStorage())
			{
				result = sceGxmSetVertexUniformBuffer(
					context, 2,
					group.private_store_outputs.front().PayloadData());
			}
			if (result >= 0 && !group.private_store_outputs.empty() &&
				group.private_store_outputs.front().HasFtoiProbe())
			{
				result = sceGxmSetVertexUniformBuffer(
					context, 3,
					group.private_store_outputs.front().FtoiProbeData());
			}
			if (result >= 0 &&
				generated->metadata.BatchVaryingLiveInVectorCount() != 0u &&
				!generated->metadata.UsesInlineBatchVaryingTail())
			{
				result = sceGxmSetVertexUniformBuffer(
					context, 4u, group.batch_live_ins.Data());
			}
			if (result >= 0 && instance_batch_live_ins)
			{
				result = sceGxmSetVertexStream(
					context, 0u, group.batch_live_ins.Data());
			}
			if (result < 0)
			{
				return fail_generated_submission(
					"bind generated VU1 batch buffers", result);
			}
			const u64 group_indices = draw->HasExactIndices() ?
				group.index_count :
				static_cast<u64>(draw->index_count) * group.draw_count;
			if (group_indices > maximum_gxm_draw_indices ||
				(flat_indices &&
					group_indices > GPU_VU_SEQUENTIAL_INDEX_COUNT))
			{
				return reject_generated_submission(
					"generated VU1 batch index count overflow");
			}
			const void* const index_data = group.exact_index_data ?
				group.exact_index_data.Data() : gpu_vu_sequential_indices;
			// Crossing the libGXM call boundary transfers uncertain firmware
			// ownership even when the API reports an error. Preserve all bound
			// resources before handling that result.
			UpdateGpuVuPreNotificationWatchdog(
				VitaGS::GpuVuGxmSubmissionStage::DrawCall,
				group_sequence);
			encoded_any = true;
			VitaGS::GpuVuGxmCallBreadcrumb call_breadcrumb;
			call_breadcrumb.sequence_end =
				active_gpu_vu_draws->back()->ordering_sequence;
			call_breadcrumb.program_key_high = generated->key.high;
			call_breadcrumb.program_key_low = generated->key.low;
			call_breadcrumb.program_abi =
				generated->metadata.loop_kernel_source_abi;
			call_breadcrumb.input_owner =
				group.owner;
			call_breadcrumb.input_slot = group.slot;
			call_breadcrumb.input_generation = group.generation;
			call_breadcrumb.input_first_qword = group.first_qword;
			call_breadcrumb.input_last_qword = group.last_qword;
			call_breadcrumb.group_index = batch_group_index;
			call_breadcrumb.index_count = static_cast<u32>(group_indices);
			call_breadcrumb.index_minimum = group.index_minimum;
			call_breadcrumb.index_maximum = group.index_maximum;
			call_breadcrumb.object_count = group.draw_count;
			call_breadcrumb.private_transaction_count = static_cast<u32>(
				group.private_store_outputs.size());
			if (!group.private_store_outputs.empty())
			{
				const GpuVuPrivateStoreOutput& first_output =
					group.private_store_outputs.front();
				if (first_output.HasOutputStorage())
				{
					call_breadcrumb.output_address =
						reinterpret_cast<uptr>(first_output.PayloadData());
					call_breadcrumb.output_bytes = first_output.TotalPayloadBytes();
					call_breadcrumb.output_allocation_bytes = static_cast<u32>(
						std::min<size_t>(first_output.allocation.Size(),
							std::numeric_limits<u32>::max()));
					const uptr guard_offset = reinterpret_cast<uptr>(
						first_output.GuardData()) - reinterpret_cast<uptr>(
							first_output.PayloadData());
					call_breadcrumb.output_guard_offset_bytes = static_cast<u32>(
						std::min<uptr>(guard_offset,
							std::numeric_limits<u32>::max()));
					call_breadcrumb.output_maximum_write_word = first_output.
						write_extent.payload_maximum_write_word;
					call_breadcrumb.output_payload_capacity_words = first_output.
						write_extent.payload_capacity_words;
					call_breadcrumb.output_probe_maximum_write_word = first_output.
						write_extent.probe_maximum_write_word;
					call_breadcrumb.output_probe_capacity_words = first_output.
						write_extent.probe_capacity_words;
				}
			}
			populate_generated_gxm_identity(call_breadcrumb,
				generated->vertex_program, generated_final_fragment,
				primitive_type);
			const bool canonical_input = group.owner ==
				reinterpret_cast<uptr>(VU1.Mem);
			const u32 buffer0_owner_bytes = group.compact_raw_input ?
				static_cast<u32>(std::min<size_t>(group.compact_raw_input.Size(),
					std::numeric_limits<u32>::max())) :
				(canonical_input ? VU1_MEMSIZE : VitaGpuVu::InputRingSlotSize);
			const u32 buffer0_offset_bytes = group.compact_raw_input ? 0u :
				group.first_qword * 16u;
			const u32 buffer0_available_bytes =
				buffer0_offset_bytes <= buffer0_owner_bytes ?
					buffer0_owner_bytes - buffer0_offset_bytes : 0u;
			const u32 buffer0_read_bytes =
				group.last_qword >= group.first_qword ?
					(group.last_qword - group.first_qword + 1u) * 16u : 0u;
			const GpuVuPrivateStoreOutput* const diagnostic_output =
				!group.private_store_outputs.empty() ?
					&group.private_store_outputs.front() : nullptr;
			const VitaGXM::ArenaAllocation* const diagnostic_buffer4 =
				group.batch_live_ins ? &group.batch_live_ins : nullptr;
			// draw_flags: bit0 instanced-live-in, bit1 exact INDEX, bit2 flat,
			// bit3 nested-grid, bit4 compact BUFFER0, bit5 canonical BUFFER0,
			// bit6 raw-ring BUFFER0, bit7 private attestation.
			const u32 draw_flags =
				(static_cast<u32>(instance_batch_live_ins) << 0u) |
				(static_cast<u32>(draw->HasExactIndices()) << 1u) |
				(static_cast<u32>(flat_indices) << 2u) |
				(static_cast<u32>(nested_grid) << 3u) |
				(static_cast<u32>(static_cast<bool>(group.compact_raw_input)) << 4u) |
				(static_cast<u32>(canonical_input) << 5u) |
				(static_cast<u32>(!group.compact_raw_input && !canonical_input) << 6u) |
				(static_cast<u32>(draw->HasPrivateStoreComparison()) << 7u);
			populate_generated_resource_identity(call_breadcrumb,
				group.raw_buffer_base, buffer0_available_bytes, buffer0_read_bytes,
				&group.batch_data, diagnostic_output, diagnostic_buffer4,
				index_data, static_cast<u32>(group_indices * sizeof(u16)),
				draw_flags);
			AccumulateGpuVuGeneratedSceneManifest(
				VitaGS::GpuVuGxmCallKind::GeneratedBatchDraw,
				group_sequence, call_breadcrumb);
			const u32 call_token = VitaGS::BeginGpuVuGxmCall(
				VitaGS::GpuVuGxmCallKind::GeneratedBatchDraw,
				group_sequence, scene_serial, call_breadcrumb);
			if (instance_batch_live_ins)
			{
				result = sceGxmDrawInstanced(context, primitive_type,
					SCE_GXM_INDEX_FORMAT_U16, index_data,
					static_cast<u32>(group_indices), draw->index_count);
			}
			else
			{
				result = (flat_indices || nested_grid || draw->HasExactIndices()) ?
					sceGxmDraw(context, primitive_type,
						SCE_GXM_INDEX_FORMAT_U16, index_data,
						static_cast<u32>(group_indices)) :
					sceGxmDrawInstanced(context, primitive_type,
						SCE_GXM_INDEX_FORMAT_U16, gpu_vu_sequential_indices,
						static_cast<u32>(group_indices),
						indices_per_primitive);
			}
			VitaGS::CompleteGpuVuGxmCall(call_token, result);
			if (result < 0)
			{
				return fail_generated_submission(instance_batch_live_ins ?
					"sceGxmDrawInstanced(instance-batched VU1+TFX)" :
					(flat_indices || nested_grid || draw->HasExactIndices()) ?
					(nested_grid ?
						"sceGxmDraw(nested-grid VU1+TFX)" :
						"sceGxmDraw(batched expanded-index VU1+TFX)") :
					"sceGxmDrawInstanced(batched generated VU1+TFX)",
					result);
			}
			UpdateGpuVuPreNotificationWatchdog(
				VitaGS::GpuVuGxmSubmissionStage::DrawReturned,
				group_sequence);
			for (GpuVuPrivateStoreOutput& output :
				group.private_store_outputs)
			{
				if (!output.transaction)
					continue;
				UpdateGpuVuPreNotificationWatchdog(
					VitaGS::GpuVuGxmSubmissionStage::TransactionClaim,
					output.sequence);
				if (!output.transaction->ClaimGpuOutputOwner())
				{
					output.transaction->MarkFailed(
						VitaGpuVu::GeneratedLoopKernelTransactionFailure::
							GsSubmissionFailedAfterEffects,
						"claim generated batch transaction after sceGxmDraw");
					return reject_generated_submission(
						"generated VU1 batch transaction acceptance failed after draw");
				}
			}
			final_draw_count++;
			RecordGpuVuGxmDraw(group_indices, group.draw_count);
			batch_group_index++;
		}
	}
	else
	{
		// Non-buffered roots retain their exact legacy stream ABI. They may
		// share GS state derivation but each immutable payload still needs its
		// own stream bindings and draw.
			for (u32 object = 0; object < active_gpu_vu_draws->size(); object++)
		{
			const VitaGpuVu::GpuVuDraw& candidate =
				*(*active_gpu_vu_draws)[object];
			UpdateGpuVuPreNotificationWatchdog(
				VitaGS::GpuVuGxmSubmissionStage::DrawBinding,
				candidate.ordering_sequence);
			if (object < direct_private_store_outputs.size() &&
				direct_private_store_outputs[object].allocation)
			{
				result = sceGxmSetVertexUniformBuffer(
					context, 2,
					direct_private_store_outputs[object].allocation.Data());
				if (result < 0)
				{
					return fail_generated_submission(
						"bind generated VU1 private output", result);
				}
			}
			if (object < direct_private_store_outputs.size() &&
				direct_private_store_outputs[object].HasFtoiProbe())
			{
				result = sceGxmSetVertexUniformBuffer(
					context, 3,
					static_cast<u8*>(
						direct_private_store_outputs[object].allocation.Data()) +
						direct_private_store_outputs[object].FtoiProbeOffset());
				if (result < 0)
				{
					return fail_generated_submission(
						"bind generated VU1 DIV probe output", result);
				}
			}
			if (structured_direct)
			{
				const u8* const input = structured_direct_inputs[object];
				result = sceGxmSetVertexUniformBuffer(context, 3,
					input + VitaGpuVu::GeneratedNestedDirectMemoryWordOffset *
						sizeof(u32));
				if (result >= 0 && staged_generated_direct)
				{
					result = sceGxmSetVertexUniformBuffer(context, 10,
						static_cast<u8*>(structured_scratch.Data()) +
							object * structured_scratch_stride);
				}
				if (result >= 0)
					result = sceGxmSetVertexUniformBuffer(context, 11,
						input + VitaGpuVu::GeneratedNestedDirectSnapshotWordOffset *
							sizeof(u32));
				if (result >= 0)
					result = sceGxmSetVertexUniformBuffer(context, 12,
						input + VitaGpuVu::GeneratedNestedDirectOuterStateWordOffset *
							sizeof(u32));
				if (result >= 0)
					result = sceGxmSetVertexUniformBuffer(context, 13,
						input + VitaGpuVu::GeneratedNestedDirectViSnapshotWordOffset *
							sizeof(u32));
				if (result < 0)
				{
					return fail_generated_submission(
						"bind generated nested-direct state buffers", result);
				}
			}
			for (u32 index = 0; index < candidate.streams.size(); index++)
			{
				const VitaGpuVu::StreamBinding& binding =
					candidate.streams[index];
				if (binding.owner != VitaGpuVu::StreamInputOwner::RawInput)
					return reject_generated_submission(
						"non-buffered generated root cannot bind canonical VU memory");
				const VitaGpuVu::RawVifPayloadRef& payload_ref =
					candidate.InputPayloads()[binding.input_span];
				const u8* const payload =
					VitaGpuVu::ResolveGpuRawVifPayload(payload_ref);
				if (!payload)
				{
					return reject_generated_submission(
						"GPU-VU VIF input span retired before draw");
				}
				const int stream_result = sceGxmSetVertexStream(
					context, index,
					payload + binding.payload_byte_offset);
				if (stream_result < 0)
				{
					return fail_generated_submission(
						"bind generated VU1 raw VIF stream",
						stream_result);
				}
			}
			const void* const index_data =
				object < direct_exact_index_data.size() &&
					direct_exact_index_data[object] ?
					direct_exact_index_data[object].Data() :
					gpu_vu_sequential_indices;
			// As above, entry to sceGxmDraw* is the conservative ownership
			// boundary, not its return value.
			UpdateGpuVuPreNotificationWatchdog(
				VitaGS::GpuVuGxmSubmissionStage::DrawCall,
				candidate.ordering_sequence);
			encoded_any = true;
			VitaGS::GpuVuGxmCallBreadcrumb call_breadcrumb;
			call_breadcrumb.sequence_end =
				active_gpu_vu_draws->back()->ordering_sequence;
			call_breadcrumb.program_key_high = generated->key.high;
			call_breadcrumb.program_key_low = generated->key.low;
			call_breadcrumb.program_abi =
				generated->metadata.loop_kernel_source_abi;
			call_breadcrumb.input_owner =
				submission_breadcrumb.input_owner;
			call_breadcrumb.input_slot = submission_breadcrumb.input_slot;
			call_breadcrumb.input_generation =
				submission_breadcrumb.input_generation;
			call_breadcrumb.object_index = object;
			call_breadcrumb.index_count = candidate.index_count;
			if (candidate.HasExactIndices())
			{
				const auto bounds = std::minmax_element(
					candidate.ExactIndices().begin(), candidate.ExactIndices().end());
				call_breadcrumb.index_minimum = *bounds.first;
				call_breadcrumb.index_maximum = *bounds.second;
			}
			else if (candidate.index_count != 0u)
			{
				call_breadcrumb.index_maximum = candidate.index_count - 1u;
			}
			call_breadcrumb.object_count = static_cast<u32>(
				active_gpu_vu_draws->size());
			call_breadcrumb.private_transaction_count =
				submission_breadcrumb.private_transaction_count;
			call_breadcrumb.output_address =
				submission_breadcrumb.output_address;
			call_breadcrumb.output_bytes =
				submission_breadcrumb.output_bytes;
			if (const VitaGpuVu::GeneratedInputWindow* input_window =
					candidate.GeneratedInputWindowProof())
			{
				call_breadcrumb.input_first_qword =
					input_window->bound_first_qword;
				call_breadcrumb.input_last_qword =
					input_window->required_last_qword;
			}
			if (object < direct_private_store_outputs.size())
			{
				const GpuVuPrivateStoreOutput& output =
					direct_private_store_outputs[object];
				if (output.HasOutputStorage() && output.allocation)
				{
					call_breadcrumb.output_address =
						reinterpret_cast<uptr>(output.PayloadData());
					call_breadcrumb.output_bytes = output.TotalPayloadBytes();
					call_breadcrumb.output_allocation_bytes = static_cast<u32>(
						std::min<size_t>(output.allocation.Size(),
							std::numeric_limits<u32>::max()));
					const uptr guard_offset = reinterpret_cast<uptr>(
						output.GuardData()) - reinterpret_cast<uptr>(
							output.PayloadData());
					call_breadcrumb.output_guard_offset_bytes = static_cast<u32>(
						std::min<uptr>(guard_offset,
							std::numeric_limits<u32>::max()));
					call_breadcrumb.output_maximum_write_word = output.write_extent.
						payload_maximum_write_word;
					call_breadcrumb.output_payload_capacity_words = output.write_extent.
						payload_capacity_words;
					call_breadcrumb.output_probe_maximum_write_word = output.write_extent.
						probe_maximum_write_word;
					call_breadcrumb.output_probe_capacity_words = output.write_extent.
						probe_capacity_words;
				}
			}
			populate_generated_gxm_identity(call_breadcrumb,
				generated->vertex_program, generated_final_fragment,
				primitive_type);
			AccumulateGpuVuGeneratedSceneManifest(
				VitaGS::GpuVuGxmCallKind::GeneratedDirectDraw,
				candidate.ordering_sequence, call_breadcrumb);
			const u32 call_token = VitaGS::BeginGpuVuGxmCall(
				VitaGS::GpuVuGxmCallKind::GeneratedDirectDraw,
				candidate.ordering_sequence, scene_serial, call_breadcrumb);
			result = flat_instances ?
				sceGxmDrawInstanced(context, primitive_type,
					SCE_GXM_INDEX_FORMAT_U16,
					gpu_vu_sequential_indices,
					candidate.index_count,
					indices_per_primitive) :
				sceGxmDraw(context, primitive_type,
					SCE_GXM_INDEX_FORMAT_U16,
					index_data,
					candidate.index_count);
			VitaGS::CompleteGpuVuGxmCall(call_token, result);
			if (result < 0)
			{
				return fail_generated_submission(flat_instances ?
					"sceGxmDrawInstanced(generated VU1+TFX)" :
					"sceGxmDraw(generated VU1+TFX)", result);
			}
			UpdateGpuVuPreNotificationWatchdog(
				VitaGS::GpuVuGxmSubmissionStage::DrawReturned,
				candidate.ordering_sequence);
			if (object < direct_private_store_outputs.size() &&
				direct_private_store_outputs[object].transaction)
			{
				UpdateGpuVuPreNotificationWatchdog(
					VitaGS::GpuVuGxmSubmissionStage::TransactionClaim,
					candidate.ordering_sequence);
				if (!direct_private_store_outputs[object].transaction->
						ClaimGpuOutputOwner())
				{
					direct_private_store_outputs[object].transaction->MarkFailed(
						VitaGpuVu::GeneratedLoopKernelTransactionFailure::
							GsSubmissionFailedAfterEffects,
						"claim generated direct transaction after sceGxmDraw");
					return reject_generated_submission(
						"generated VU1 direct transaction acceptance failed after draw");
				}
			}
			final_draw_count++;
			RecordGpuVuGxmDraw(candidate.index_count);
		}
	}
	if (point_topology || line_topology)
	{
		sceGxmSetFrontPolygonMode(context, SCE_GXM_POLYGON_MODE_TRIANGLE_FILL);
		sceGxmSetBackPolygonMode(context, SCE_GXM_POLYGON_MODE_TRIANGLE_FILL);
	}
	if (!encoded_any || final_draw_count == 0u)
	{
		return reject_generated_submission(
			"GPU-VU batch encoded no final GXM work");
	}
	UpdateGpuVuPreNotificationWatchdog(
		VitaGS::GpuVuGxmSubmissionStage::ResourceOwnershipTransfer,
		active_gpu_vu_draws->back()->ordering_sequence);
	if (structured_scratch)
	{
		gpu_vu_scene_batch_allocations.push_back(
			std::move(structured_scratch));
	}
		for (BatchInputGroup& group : batch_input_groups)
		{
			if (group.compact_raw_input)
			{
				gpu_vu_scene_batch_allocations.push_back(
					std::move(group.compact_raw_input));
			}
		gpu_vu_scene_batch_allocations.push_back(
			std::move(group.batch_data));
		if (group.batch_live_ins)
		{
			gpu_vu_scene_batch_allocations.push_back(
				std::move(group.batch_live_ins));
		}
		if (group.exact_index_data)
		{
			gpu_vu_scene_batch_allocations.push_back(
				std::move(group.exact_index_data));
		}
		for (GpuVuPrivateStoreOutput& output : group.private_store_outputs)
		{
			if (output.HasOutputStorage() || output.completion_only)
			{
				gpu_vu_scene_private_store_outputs.push_back(
					std::move(output));
			}
		}
	}
	for (VitaGXM::ArenaAllocation& allocation : direct_exact_index_data)
	{
		if (allocation)
			gpu_vu_scene_batch_allocations.push_back(std::move(allocation));
	}
	for (GpuVuPrivateStoreOutput& output : direct_private_store_outputs)
	{
		if (output.HasOutputStorage() || output.completion_only)
			gpu_vu_scene_private_store_outputs.push_back(std::move(output));
	}
	UpdateGpuVuPreNotificationWatchdog(
		VitaGS::GpuVuGxmSubmissionStage::ResourcesRetained,
		active_gpu_vu_draws->back()->ordering_sequence);
	if (staged_generated_direct)
	{
		u64 accepted_pairs = 0u;
		for (const auto& candidate : *active_gpu_vu_draws)
			accepted_pairs += candidate->executed_pair_count;
		gpu_vu_generated_pipeline_accept_count +=
			active_gpu_vu_draws->size();
		gpu_vu_generated_pipeline_pair_count += accepted_pairs;
		const u64 accepted = gpu_vu_generated_pipeline_accept_count;
		if (accepted <= 8u || (accepted & (accepted - 1u)) == 0u)
		{
			const u64 first_sequence =
				active_gpu_vu_draws->front()->ordering_sequence;
			const u64 last_sequence =
				active_gpu_vu_draws->back()->ordering_sequence;
			Console.WriteLn(
				"GPU-VU: seq=%llu..%llu provider=generated-direct "
				"accepted=1 objects=%u pairs=%llu output=direct-tfx "
				"precompute_modules=%u precompute_stages=%u "
				"producer_draws=%llu final_draws=%llu firmware_jobs=%u "
				"scratch_bytes=%u cpu_vu_calls=0 "
				"accepted_total=%llu pairs_total=%llu.",
				static_cast<unsigned long long>(first_sequence),
				static_cast<unsigned long long>(last_sequence),
				static_cast<u32>(active_gpu_vu_draws->size()),
				static_cast<unsigned long long>(accepted_pairs),
				draw->precompute_program_count,
				draw->precompute_stage_count,
				static_cast<unsigned long long>(producer_draw_count),
				static_cast<unsigned long long>(final_draw_count),
				draw->precompute_stage_count + 1u,
				structured_scratch_bytes,
				static_cast<unsigned long long>(accepted),
				static_cast<unsigned long long>(
					gpu_vu_generated_pipeline_pair_count));
			}
		}
	else if (loop_kernel_direct)
	{
		u64 accepted_pairs = 0u;
		u64 private_store_entries = 0u;
		u64 ftoi_probe_entries = 0u;
		bool cpu_shadow = false;
		for (const auto& candidate : *active_gpu_vu_draws)
		{
			accepted_pairs += candidate->executed_pair_count;
			cpu_shadow |= candidate->HasPrivateStoreComparison();
			private_store_entries +=
				static_cast<u64>(candidate->PrivateStoreInvocationCount()) *
				candidate->private_store_count;
			if (generated->metadata.uses_loop_kernel_ftoi_probe_output)
				ftoi_probe_entries += candidate->PrivateStoreInvocationCount();
		}
		const u64 first_sequence =
			active_gpu_vu_draws->front()->ordering_sequence;
		const u64 last_sequence =
			active_gpu_vu_draws->back()->ordering_sequence;
		if (cpu_shadow)
		{
			Console.WriteLn(
				"GPU-VU: seq=%llu..%llu provider=generated-loop-kernel "
				"encoded=1 objects=%u pairs=%llu output=private-attestation "
				"roots=1 producer_draws=0 final_draws=%llu "
				"source_abi=%u batch_varying_vectors=%u "
				"firmware_jobs=1 snapshots=0 scratch_bytes=0 "
				"mid_scene_flushes=0 routine_waits=0 "
				"cpu_oracle_calls=%u cpu_oracle_pairs=%llu "
				"private_store_entries=%llu ftoi_probe_entries=%llu "
				"cpu_path1_preserved=1 "
				"fragments=disabled depth_writes=disabled product_accepted=0.",
				static_cast<unsigned long long>(first_sequence),
				static_cast<unsigned long long>(last_sequence),
				static_cast<u32>(active_gpu_vu_draws->size()),
				static_cast<unsigned long long>(accepted_pairs),
				static_cast<unsigned long long>(final_draw_count),
				generated->metadata.loop_kernel_source_abi,
				generated->metadata.BatchVaryingLiveInVectorCount(),
				static_cast<u32>(active_gpu_vu_draws->size()),
				static_cast<unsigned long long>(accepted_pairs),
				static_cast<unsigned long long>(private_store_entries),
				static_cast<unsigned long long>(ftoi_probe_entries));
		}
		else
		{
			gpu_vu_loop_kernel_accept_count += active_gpu_vu_draws->size();
			gpu_vu_loop_kernel_pair_count += accepted_pairs;
			const u64 accepted = gpu_vu_loop_kernel_accept_count;
			if (accepted <= 8u || (accepted & (accepted - 1u)) == 0u)
			{
				Console.WriteLn(
					"GPU-VU: seq=%llu..%llu provider=generated-loop-kernel "
					"encoded=1 objects=%u pairs=%llu output=direct-tfx "
					"roots=1 producer_draws=0 final_draws=%llu "
					"source_abi=%u batch_varying_vectors=%u "
					"firmware_jobs=1 snapshots=0 scratch_bytes=0 "
					"mid_scene_flushes=0 routine_waits=0 cpu_vu_calls=0 "
					"accepted_total=%llu pairs_total=%llu.",
					static_cast<unsigned long long>(first_sequence),
					static_cast<unsigned long long>(last_sequence),
					static_cast<u32>(active_gpu_vu_draws->size()),
					static_cast<unsigned long long>(accepted_pairs),
					static_cast<unsigned long long>(final_draw_count),
					generated->metadata.loop_kernel_source_abi,
					generated->metadata.BatchVaryingLiveInVectorCount(),
					static_cast<unsigned long long>(accepted),
					static_cast<unsigned long long>(
						gpu_vu_loop_kernel_pair_count));
			}
		}
	}
	if (rt)
		rt->SetState(GSTexture::State::Dirty);
	if (ds && config.depth.zwe)
		ds->SetState(GSTexture::State::Dirty);
	active_gpu_vu_draw_encoded = true;
	watchdog_scope.retained = true;
	return true;
}

bool GSDeviceGXM::Impl::DrawQuad(VitaGXM::GSTextureGXM* source,
	const GSVector4& source_rect, const GSVector4& destination_rect, u32 color,
	Filter filter, SceGxmFragmentProgram* fragment,
	const SceGxmProgramParameter* uniform, const float* uniform_data)
{
	if (!scene_active || destination_rect.z <= destination_rect.x ||
		destination_rect.w <= destination_rect.y)
	{
		return false;
	}
	const u32 width = scene_is_display ? VitaGXM::Display::Width :
		static_cast<u32>((scene_rt ? scene_rt : scene_ds)->GetWidth());
	const u32 height = scene_is_display ? VitaGXM::Display::Height :
		static_cast<u32>((scene_rt ? scene_rt : scene_ds)->GetHeight());
	void* vertex_memory = nullptr;
	u16* indices = nullptr;
	if (!ReserveGeometry(4, 6, &vertex_memory, &indices))
	{
		const bool restart_display_scene = scene_is_display;
		VitaGXM::GSTextureGXM* const restart_rt = scene_rt;
		VitaGXM::GSTextureGXM* const restart_ds = scene_ds;
		const GSVector4i restart_scissor = scene_scissor;
		if (!Finish())
			return false;
		const bool restarted = restart_display_scene ? BeginDisplayScene() :
			EnsureScene(restart_rt, restart_ds, restart_scissor);
		if (!restarted || !ReserveGeometry(4, 6, &vertex_memory, &indices))
			return Reject("quad geometry exceeds GXM staging capacity");
	}
	QuadVertex* vertices = static_cast<QuadVertex*>(vertex_memory);
	const float left = PixelToNdc(destination_rect.x, width);
	const float right = PixelToNdc(destination_rect.z, width);
	const float top = PixelToNdc(destination_rect.y, height);
	const float bottom = PixelToNdc(destination_rect.w, height);
	SetQuadVertex(vertices[0], left, top, color, source_rect.x, source_rect.y);
	SetQuadVertex(vertices[1], right, top, color, source_rect.z, source_rect.y);
	SetQuadVertex(vertices[2], left, bottom, color, source_rect.x, source_rect.w);
	SetQuadVertex(vertices[3], right, bottom, color, source_rect.z, source_rect.w);
	constexpr std::array<u16, 6> quad = {0, 1, 2, 2, 1, 3};
	std::copy(quad.begin(), quad.end(), indices);

	sceGxmSetFrontDepthFunc(context, SCE_GXM_DEPTH_FUNC_ALWAYS);
	sceGxmSetBackDepthFunc(context, SCE_GXM_DEPTH_FUNC_ALWAYS);
	sceGxmSetFrontDepthWriteEnable(context, SCE_GXM_DEPTH_WRITE_DISABLED);
	sceGxmSetBackDepthWriteEnable(context, SCE_GXM_DEPTH_WRITE_DISABLED);
	sceGxmSetVertexProgram(context, source ? present_vertex_program : color_vertex_program);
	sceGxmSetFragmentProgram(context, source ? fragment : color_fragment_program);
	sceGxmSetVertexStream(context, 0, vertices);
	if (source)
	{
		SceGxmTexture& texture = source->Texture();
		const bool strided =
			sceGxmTextureGetType(&texture) == SCE_GXM_TEXTURE_LINEAR_STRIDED;
		const SceGxmTextureFilter native_filter = filter == Biln ?
			SCE_GXM_TEXTURE_FILTER_LINEAR : SCE_GXM_TEXTURE_FILTER_POINT;
		int result = strided ? 0 : sceGxmTextureSetMinFilter(&texture, native_filter);
		if (result >= 0)
			result = sceGxmTextureSetMagFilter(&texture, native_filter);
		if (result >= 0)
			result = sceGxmSetFragmentTexture(context, 0, &texture);
		if (result < 0)
			return Fail("bind presentation texture", result);
		source->MarkSceneUse(scene_serial);
	}
	if (uniform || uniform_data)
	{
		if (!uniform || !uniform_data)
			return false;
		void* default_buffer = nullptr;
		int result = sceGxmReserveFragmentDefaultUniformBuffer(context,
			&default_buffer);
		if (result < 0 || !default_buffer)
			return Fail("reserve presentation uniforms",
				result < 0 ? result : SCE_GXM_ERROR_INVALID_POINTER);
		result = sceGxmSetUniformDataF(default_buffer, uniform, 0, 4,
			uniform_data);
		if (result < 0)
			return Fail("upload presentation uniforms", result);
	}
	VitaGS::GpuVuGxmCallBreadcrumb call_breadcrumb;
	call_breadcrumb.index_count = static_cast<u32>(quad.size());
	const u32 call_token = gpu_vu_last_generated_sequence != 0u ?
		VitaGS::BeginGpuVuGxmCall(
			VitaGS::GpuVuGxmCallKind::PresentationDraw,
			gpu_vu_last_generated_sequence, scene_serial, call_breadcrumb) : 0u;
	const int result = TimeGxmHostCall(VitaGxmHostCall::DrawQuad, [&] {
		return sceGxmDraw(context, SCE_GXM_PRIMITIVE_TRIANGLES,
			SCE_GXM_INDEX_FORMAT_U16, indices, quad.size());
	});
	VitaGS::CompleteGpuVuGxmCall(call_token, result);
	if (result < 0)
		return Fail("sceGxmDraw(presentation quad)", result);
	if (VitaPerformanceTelemetry::IsEnabled())
		scene_ordinary_content |= 2;
	RecordGxmDraw(4, sizeof(QuadVertex), quad.size());
	return true;
}

void GSDeviceGXM::PollGpuVuPrograms()
{
	if (m_impl && m_impl->ready)
		m_impl->PollGeneratedVuPrograms();
}

void GSDeviceGXM::ServiceUniversalGpuVuEpoch(
	VitaGpuVu::UniversalGpuVuEpoch* epoch)
{
#if defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
	if (m_impl && m_impl->ready)
	{
		m_impl->ServiceUniversalGpuVuEpoch(epoch);
		return;
	}
#endif
	if (epoch)
	{
		epoch->MarkGpuRejected(
			VitaGpuVu::UniversalGpuVuRejection::DeviceUnavailable,
			epoch->PreflightPairCount());
	}
}

bool GSDeviceGXM::RenderGpuVuDraw(GSHWDrawConfig& config,
	std::unique_ptr<VitaGpuVu::GpuVuDraw> draw)
{
	if (!draw)
		return false;
	std::vector<std::unique_ptr<VitaGpuVu::GpuVuDraw>> draws;
	draws.push_back(std::move(draw));
	return RenderGpuVuDraws(config, std::move(draws));
}

bool GSDeviceGXM::RenderGpuVuDraws(GSHWDrawConfig& config,
	std::vector<std::unique_ptr<VitaGpuVu::GpuVuDraw>> draws)
{
	if (!m_impl || !m_impl->ready || draws.empty() ||
		m_impl->active_gpu_vu_draws || !m_impl->gpu_vu_retirements_ready)
	{
		return false;
	}
	for (const auto& draw : draws)
	{
		if (!draw || !draw->WasValidatedForQueue())
		{
			return m_impl->Reject(draw ?
				"unvalidated GPU-VU descriptor in batch" :
				"null GPU-VU descriptor in batch");
		}
	}
	// Sony's skinning_reuse sample binds each reusable buffer lifetime to one
	// scene notification, and PhyreEngine records every ring range consumed by
	// that scene against the same sync value. Preserve that ownership while
	// allowing one generated transaction to span multiple immutable VIF input
	// generations. The fixed retention array is the exact input-ring capacity;
	// only end the scene when adding this draw would exceed that bounded union.
	// Splitting merely because the generation set changed turns every Execute
	// into its own firmware job and defeats transaction batching.
	if (!CanRetainGpuVuInputSlots(m_impl->gpu_vu_scene_input_retentions,
			m_impl->gpu_vu_scene_input_retention_count, draws))
	{
		if (!m_impl->HasPendingGpuVuSceneDraws() ||
			!m_impl->EndScene(false) ||
			!CanRetainGpuVuInputSlots(m_impl->gpu_vu_scene_input_retentions,
				m_impl->gpu_vu_scene_input_retention_count, draws))
		{
			return false;
		}
	}

	m_impl->active_gpu_vu_draws = &draws;
	m_impl->active_gpu_vu_draw_encoded = false;
	m_impl->active_gpu_vu_draw_failure_reason = nullptr;
	RenderHW(config);
	m_impl->UpdateGpuVuPreNotificationWatchdog(
		VitaGS::GpuVuGxmSubmissionStage::RenderReturnedToDevice,
		draws.back()->ordering_sequence);
	const bool encoded = m_impl->active_gpu_vu_draw_encoded;
	const char* const failure_reason =
		m_impl->active_gpu_vu_draw_failure_reason;
	m_impl->active_gpu_vu_draws = nullptr;
	m_impl->active_gpu_vu_draw_encoded = false;
	m_impl->active_gpu_vu_draw_failure_reason = nullptr;
	if (!encoded)
	{
		const char* const detail = failure_reason ? failure_reason :
			"RenderHW returned without accepting generated GPU-VU work";
		for (const auto& draw : draws)
		{
			if (!draw || !draw->HasGeneratedLoopKernelTransaction())
				continue;
			const auto transaction =
				draw->GeneratedLoopKernelTransactionOwner();
			if (!transaction)
				continue;
			const auto stage = transaction->Stage();
			transaction->MarkFailed(
				stage == VitaGpuVu::GeneratedLoopKernelTransactionStage::Prepared ?
					VitaGpuVu::GeneratedLoopKernelTransactionFailure::
						GsRejectedBeforeEffects :
					VitaGpuVu::GeneratedLoopKernelTransactionFailure::
						GsSubmissionFailedAfterEffects,
				detail);
		}
		return false;
	}
	if (m_impl->RetainGpuVuDrawsForScene(std::move(draws)))
	{
		m_impl->UpdateGpuVuPreNotificationWatchdog(
			VitaGS::GpuVuGxmSubmissionStage::DeviceReturn,
			m_impl->gpu_vu_pre_notification_watchdog_sequence);
		return true;
	}

	// All fallible capacity and generation checks ran before RenderHW; reaching
	// this branch means an internal ownership invariant failed after GXM
	// accepted commands. Never block in sceGxmFinish() or replay on CPU while
	// firmware may still consume the mappings. Keep the process-owned mappings
	// alive and enter the existing bounded title-only fail-stop path.
	return m_impl->FailGpuVuRetirementOwner("input-ownership-transfer",
		0u, 0u, 0u, 0u, 0u);
}

bool GSDeviceGXM::SubmitGeneratedGpuVuTransactions()
{
	if (!m_impl || !m_impl->ready)
		return false;
	if (!m_impl->HasPendingGpuVuSceneDraws())
		return m_impl->ArmPendingGpuVuRetirementWake();
	return m_impl->FlushGeneratedGpuVuTransaction();
}

bool GSDeviceGXM::RetainGpuVuDrawForVertexCompletion(
	std::unique_ptr<VitaGpuVu::GpuVuDraw> draw)
{
	return m_impl && m_impl->ready &&
		m_impl->RetainGpuVuDrawForScene(std::move(draw));
}

void GSDeviceGXM::RenderHW(GSHWDrawConfig& config)
{
	const bool profile_render = VitaPerformanceTelemetry::IsEnabled();
	const Common::Timer::Value render_started = profile_render ?
		Common::Timer::GetCurrentValue() : 0;
	struct RenderTimer final
	{
		bool enabled;
		Common::Timer::Value started;
		~RenderTimer()
		{
			if (!enabled)
				return;
			const u64 elapsed_us = static_cast<u64>(
				Common::Timer::ConvertValueToSeconds(
					Common::Timer::GetCurrentValue() - started) * 1000000.0);
			s_gxm_worker_performance.device_render_calls++;
			s_gxm_worker_performance.device_render_wall_us += elapsed_us;
		}
	} render_timer{profile_render, render_started};
	#if defined(VITASX2_GS_DRAW_TRACE) && VITASX2_GS_DRAW_TRACE
	VitaGsDrawTraceRecordDeviceOutcome(config,
		VitaGsDrawTraceOutcome::DeviceEntered);
	#endif
	if (!m_impl || !m_impl->ready)
		return;
	if (config.gs_blend.enabled && m_impl->active_gpu_vu_draws)
	{
		m_impl->Reject("native GS blend migration does not yet consume GPU-VU draws");
		return;
	}
	if (!config.rt && !config.ds)
	{
		m_impl->Reject("RenderHW without a target");
		return;
	}
	const bool point_topology =
		config.topology == GSHWDrawConfig::Topology::Point;
	if (config.vs.expand != GSHWDrawConfig::VSExpand::None ||
		(config.vs.point_size != point_topology) ||
		config.line_expand)
	{
		m_impl->Reject("unsupported point-size/vertex/line expansion contract");
		return;
	}
	const bool mip_lod = config.ps.manual_lod || config.ps.automatic_lod;
	const bool invalid_mip_filter =
		config.sampler.triln >
			static_cast<u8>(GS_MIN_FILTER::Linear_Mipmap_Linear) ||
		(config.sampler.triln != static_cast<u8>(GS_MIN_FILTER::Nearest) &&
			!mip_lod);
	const bool psm24_fragment = config.ps.dst_fmt == 1;
	const bool psm16_fragment = config.ps.dst_fmt == 2;
	// PCSX2 GSRendererHW::EmulateTextureShuffleAndFbmask() maps PSMCT24 to
	// dst_fmt=1 and disables alpha writes through the component mask. Its
	// desktop TFX shaders otherwise share the 32-bit RGB path and suppress FBA
	// for that destination. The GXM general program carries both contracts.
	const bool invalid_destination_format = config.ps.dst_fmt > 2;
	// GSRendererHW::EmulateBlending() also sets round_inv for every hardware
	// reverse-subtract blend. PCSX2's tfx_fs.glsl::ps_dither() observes it only
	// when dithering a PSMCT16 destination; GXM's blend operation already owns
	// the ordinary 32-bit reverse subtraction, so round_inv alone is not an
	// unsupported dither contract.
	const bool invalid_dither = config.ps.dither > 3 ||
		(config.ps.dither != 0 && !psm16_fragment) ||
		(config.ps.dither_adjust && !psm16_fragment);
	const bool unsupported_tfx = config.pal || config.ps.pal_fmt ||
		config.ps.depth_fmt || invalid_destination_format || invalid_dither ||
		config.ps.afail != GSHWDrawConfig::PS_AFAIL::KEEP || config.ps.ztst ||
		config.ps.shuffle || config.ps.shuffle_same || config.ps.real16src ||
		config.ps.process_ba || config.ps.process_rg || config.ps.shuffle_across ||
		config.ps.write_rg || config.ps.a_masked || config.ps.channel ||
		config.ps.colclip_hw || config.ps.urban_chaos_hle ||
		config.ps.tales_of_abyss_hle || config.ps.point_sampler ||
		config.ps.sw_aniso || config.ps.scanmsk ||
		config.ps.aa1 != GSHWDrawConfig::PS_AA1::NONE || config.ps.rov_color ||
		config.ps.rov_depth != GSHWDrawConfig::PS_ROV_DEPTH::NONE ||
		config.ps.wms == 2 || config.ps.wmt == 2 || config.ps.zclamp ||
		config.ps.tcoffsethack || config.ps.tex_is_fb || config.ps.date ||
		config.ps.abe ||
		config.depth.date || config.depth.date_one || invalid_mip_filter;
	if (unsupported_tfx)
	{
		const u64 unsupported_tfx_features = GetUnsupportedTfxFeatureMask(config,
			invalid_destination_format, invalid_dither, invalid_mip_filter);
		#if defined(VITASX2_GS_DRAW_TRACE) && VITASX2_GS_DRAW_TRACE
		VitaGsDrawTraceRecordDeviceOutcome(config,
			VitaGsDrawTraceOutcome::RejectedSelector, 0,
			static_cast<u32>(unsupported_tfx_features) ^
			static_cast<u32>(unsupported_tfx_features >> 32));
		#endif
		const u64 rejected = RecordRejectedTfxDraw(config,
			unsupported_tfx_features);
		// Keep the diagnostic observable without allowing a missing renderer
		// mechanism to turn into an unbounded Console.Error hot path.
		if ((rejected & (rejected - 1)) == 0)
		{
			Console.Error(
				"GXM GS: rejected TFX selector count=%llu features=%016llx "
				"ps=%08x%08x%08x%08x vs=%02x sampler=%02x depth=%02x "
				"blend=%08x mask=%02x topology=%u.",
				static_cast<unsigned long long>(rejected),
				static_cast<unsigned long long>(unsupported_tfx_features),
				static_cast<u32>(config.ps.key_hi >> 32),
				static_cast<u32>(config.ps.key_hi),
				static_cast<u32>(config.ps.key_lo >> 32),
				static_cast<u32>(config.ps.key_lo), config.vs.key,
				config.sampler.key, config.depth.key, config.blend.key,
				config.colormask.key, static_cast<u32>(config.topology));
		}
		return;
	}
	const bool region_repeat_fragment = config.ps.wms == 3 || config.ps.wmt == 3;
	if (psm16_fragment && (config.ps.manual_lod || region_repeat_fragment))
	{
		m_impl->Reject("PSMCT16 combined with manual LOD or REGION_REPEAT");
		return;
	}
	if (region_repeat_fragment &&
		(config.ps.region_rect || config.ps.manual_lod || config.ps.zfloor))
	{
		m_impl->Reject(
			"REGION_REPEAT combined with target-region, manual-LOD, or Z-floor");
		return;
	}
	if (config.destination_alpha != GSHWDrawConfig::DestinationAlphaMode::Off ||
		config.alpha_second_pass.enable || config.blend_multi_pass.enable ||
		config.colclip_mode != GSHWDrawConfig::ColClipMode::NoModify)
	{
		if (VitaPerformanceTelemetry::IsEnabled())
		{
			const u32 requirements =
				(config.destination_alpha != GSHWDrawConfig::DestinationAlphaMode::Off ? 1u : 0u) |
				(config.alpha_second_pass.enable ? 2u : 0u) |
				(config.blend_multi_pass.enable ? 4u : 0u) |
				(config.colclip_mode != GSHWDrawConfig::ColClipMode::NoModify ? 8u : 0u);
			s_gxm_worker_performance.unsupported_passes[requirements]++;
		}
		m_impl->Reject("DATE, alpha-second-pass, blend-multipass, or color-clip draw");
		return;
	}
	if (config.require_full_barrier)
	{
		m_impl->Reject("per-primitive feedback barrier draw");
		return;
	}
	if (config.ps.blend_hw != 0 && config.ps.blend_hw != 3)
	{
		m_impl->Reject("unimplemented PCSX2 PS_BLEND_HW mode");
		return;
	}

	RecordAcceptedTfxDraw(config);
	#if defined(VITASX2_GS_DRAW_TRACE) && VITASX2_GS_DRAW_TRACE
	VitaGsDrawTraceRecordDeviceOutcome(config,
		VitaGsDrawTraceOutcome::Accepted);
	#endif
	VitaGXM::GSTextureGXM* source =
		CheckedCast<VitaGXM::GSTextureGXM>(config.tex);
	if (mip_lod && !source)
	{
		m_impl->Reject("mip LOD draw without a source texture");
		return;
	}
	if (source && !m_impl->CommitClear(*source))
		return;
	if (source && source->IsRenderTargetOrDepthStencil() &&
		!m_impl->EnsureTextureBackingCurrent(*source))
	{
		return;
	}
	#if defined(VITASX2_GS_DRAW_TRACE) && VITASX2_GS_DRAW_TRACE
	if (source && source->IsRenderTarget())
	{
		const u64 generation =
			source->WriterTelemetry().content_generation;
		const bool needs_sync = VitaGsDrawTraceRenderTargetSourceNeedsSync(
			config, *source, generation);
		if ((!needs_sync || m_impl->Finish()))
		{
			VitaGsDrawTraceRecordRenderTargetSource(
				config, *source, generation);
		}
	}
	#endif
	VitaGXM::GSTextureGXM* rt =
		CheckedCast<VitaGXM::GSTextureGXM>(config.rt);
	VitaGXM::GSTextureGXM* ds =
		CheckedCast<VitaGXM::GSTextureGXM>(config.ds);
	if (source && ((config.tex == config.rt) ||
		config.tex_hazard == GSHWDrawConfig::TEX_HAZARD_RT))
	{
		// GXM permits same-pixel ordered FRAGCOLOR reads, but not arbitrary
		// texture sampling from the color surface currently being written. PCSX2's
		// sample-area analysis bounds the snapshot for this non-overlap route.
		if (!rt)
		{
			m_impl->Reject("RT texture hazard without a color render target");
			return;
		}
		source = m_impl->SnapshotTexture(*rt, config.samplearea);
		if (!source)
			return;
	}
	else if (config.tex_hazard == GSHWDrawConfig::TEX_HAZARD_DEPTH)
	{
		m_impl->Reject("depth-as-texture hazard");
		return;
	}

	const GSHWDrawConfig::ColorMaskSelector color_mask(
		config.ps.no_color ? 0 : config.colormask.wrgba);
	const bool blend_effective = config.blend.enable &&
		config.blend.IsEffective(color_mask);
	// PCSX2's opaque TFX lowering needs no destination value. GXM's fixed-patched
	// fragment programs are excluded: real-hardware A/B evidence showed tile
	// seams, while null-blend programs preserve the oracle image.
	const bool full_color_write = config.colormask.wrgba == 0xf &&
		!config.ps.no_color;
	// No patcher blend state is involved: this program is valid only when PCSX2
	// proves blending ineffective and every color component is overwritten.
	const bool manual_lod_fragment = config.ps.manual_lod;
	bool fast_fragment = !psm16_fragment && !manual_lod_fragment &&
		!blend_effective && full_color_write &&
		m_impl->CanUseFastTfx(config);
	const bool programmable_add = !psm16_fragment && !manual_lod_fragment &&
		blend_effective &&
		m_impl->CanUseSourceOnlyTfx(config) &&
		full_color_write &&
		!config.blend.constant_enable &&
		config.blend.op == GSDevice::OP_ADD &&
		config.blend.src_factor == GSDevice::CONST_ONE &&
		config.blend.dst_factor == GSDevice::CONST_ONE &&
		config.blend.src_factor_alpha == GSDevice::CONST_ONE &&
		config.blend.dst_factor_alpha == GSDevice::CONST_ZERO;
	const bool programmable_add_direct = programmable_add &&
		m_impl->CanUseSourceDirectTfx(config);
	const bool programmable_over = !psm16_fragment && !manual_lod_fragment &&
		blend_effective &&
		full_color_write && !config.ps.fbmask &&
		!config.blend.constant_enable &&
		config.blend.op == GSDevice::OP_ADD &&
		config.blend.src_factor == GSDevice::CONST_ONE &&
		config.blend.dst_factor == GSDevice::INV_SRC1_COLOR &&
		config.blend.src_factor_alpha == GSDevice::CONST_ONE &&
		config.blend.dst_factor_alpha == GSDevice::CONST_ZERO;
	// PCSX2 tfx_fs.glsl proves this family never observes Cd/Ad. Use Sony's
	// fixed-blend contract to avoid the SGX framebuffer fetch entirely.
	const bool source_only_contract = !psm16_fragment && !manual_lod_fragment &&
		blend_effective && full_color_write &&
		!config.ps.fbmask && m_impl->CanUseSourceOnlyTfx(config);
	const bool zfloor_programmable_constant_fragment =
		!psm16_fragment && !manual_lod_fragment && blend_effective &&
		m_impl->CanUseZfloorSourceDirectDecalAfTfx(config);
	if (zfloor_programmable_constant_fragment &&
		!m_impl->tfx_zfloor_source_direct_decal_af_logged)
	{
		m_impl->tfx_zfloor_source_direct_decal_af_logged = true;
		Console.WriteLn(
			"GXM GS: source-direct Z-floor DECAL/RGB constant-blend path active.");
	}
	bool source_only_fragment = !m_impl->active_gpu_vu_draws &&
		source_only_contract && !config.ps.zfloor && !region_repeat_fragment;
	bool untextured_fragment = source_only_fragment && !config.vs.tme;
	bool source_direct_fragment = source_only_fragment &&
		m_impl->CanUseSourceDirectTfx(config);
	bool source_direct_modulate_fragment = source_direct_fragment &&
		m_impl->CanUseSourceDirectModulateTfx(config);
	bool source_direct_modulate_af_fragment =
		source_direct_modulate_fragment &&
		m_impl->CanUseSourceDirectModulateAfTfx(config);
	SceGxmFragmentProgram* fixed_source_fragment =
		source_only_fragment ? m_impl->GetPatchedTfxProgram(config, true,
			source_direct_fragment, source_direct_modulate_fragment,
			source_direct_modulate_af_fragment, untextured_fragment) :
		nullptr;
	if (source_only_fragment && !fixed_source_fragment)
	{
		source_direct_modulate_af_fragment = false;
		source_only_fragment = false;
		source_direct_fragment = false;
		source_direct_modulate_fragment = false;
	}
	bool programmable_add_direct_fragment = programmable_add_direct &&
		!fixed_source_fragment;
	bool programmable_add_fragment = programmable_add &&
		!programmable_add_direct && !fixed_source_fragment;
	bool programmable_over_fragment = programmable_over;
	if (config.ps.zfloor)
	{
		// Fragment-depth replacement must remain after rasterization. The dedicated
		// constant-blend program below has that exact DEPTH output; every ordinary
		// non-Z-floor specialization remains excluded.
		fast_fragment = false;
		programmable_add_direct_fragment = false;
		programmable_add_fragment = false;
		programmable_over_fragment = false;
		source_only_fragment = false;
		source_direct_fragment = false;
		source_direct_modulate_fragment = false;
		source_direct_modulate_af_fragment = false;
		untextured_fragment = false;
	}
	if (region_repeat_fragment)
	{
		programmable_add_direct_fragment = false;
		programmable_add_fragment = false;
		programmable_over_fragment = false;
		source_only_fragment = false;
		source_direct_fragment = false;
		source_direct_modulate_fragment = false;
		source_direct_modulate_af_fragment = false;
		untextured_fragment = false;
	}
	SceGxmFragmentProgram* fragment = psm16_fragment ?
		(config.ps.zfloor ? m_impl->tfx_psm16_zfloor_fragment_program :
			m_impl->tfx_psm16_fragment_program) : (region_repeat_fragment ?
		(fast_fragment ? m_impl->tfx_region_repeat_opaque_program :
			m_impl->tfx_region_repeat_program) : (manual_lod_fragment ?
		(config.ps.zfloor ? m_impl->tfx_manual_lod_zfloor_fragment_program :
			m_impl->tfx_manual_lod_fragment_program) : (config.ps.zfloor ?
		(zfloor_programmable_constant_fragment ?
			m_impl->tfx_zfloor_source_direct_decal_af_program :
		(fixed_source_fragment ? fixed_source_fragment :
			m_impl->tfx_zfloor_fragment_program)) : (fixed_source_fragment ?
		fixed_source_fragment : (programmable_add_direct ?
		m_impl->tfx_programmable_add_direct_program :
		(programmable_add ?
		m_impl->tfx_programmable_add_program :
		(programmable_over ? m_impl->tfx_programmable_over_program :
		(fast_fragment ? m_impl->tfx_opaque_program :
			m_impl->tfx_fragment_program))))))));
	if (!fragment)
	{
		// Shader-patcher memory exhaustion must not change GS behavior. The
		// general FRAGCOLOR program remains the exact fallback for this draw.
		fast_fragment = false;
		programmable_add_direct_fragment = false;
		programmable_add_fragment = false;
		programmable_over_fragment = false;
		source_only_fragment = false;
		source_direct_fragment = false;
		source_direct_modulate_fragment = false;
		source_direct_modulate_af_fragment = false;
		untextured_fragment = false;
		fragment = psm16_fragment ?
			(config.ps.zfloor ? m_impl->tfx_psm16_zfloor_fragment_program :
				m_impl->tfx_psm16_fragment_program) : (manual_lod_fragment ?
			(config.ps.zfloor ? m_impl->tfx_manual_lod_zfloor_fragment_program :
				m_impl->tfx_manual_lod_fragment_program) :
			(config.ps.zfloor ? m_impl->tfx_zfloor_fragment_program :
				m_impl->tfx_fragment_program));
	}
	// PCSX2 GSDeviceVK::GetTFXFragmentShader and tfx.glsl::atst/main omit
	// the kill instruction for PS_ATST_NONE. Preserve the ordinary fast path's
	// native output/null-blend contract and every other selector. Generated
	// GPU-VU and region/Z-floor/manual-LOD programs are deliberately unchanged.
	if (!m_impl->active_gpu_vu_draws && fragment == m_impl->tfx_opaque_program &&
		config.ps.atst == GSHWDrawConfig::PS_ATST::NONE)
	{
		fragment = m_impl->tfx_fast_no_atst_program;
	}
	if (psm16_fragment && !m_impl->tfx_psm16_logged)
	{
		m_impl->tfx_psm16_logged = true;
		Console.WriteLn(
			"GXM GS: PCSX2 PSMCT16 dither/quantization path active.");
	}
	if (config.gs_blend.enabled)
	{
		fragment = m_impl->gs_blend_programs[SelectGsBlendProgram(config, config.ps)];
		fast_fragment = false;
		programmable_add_fragment = false;
		programmable_add_direct_fragment = false;
		programmable_over_fragment = false;
		source_only_fragment = false;
		source_direct_fragment = false;
		source_direct_modulate_fragment = false;
		source_direct_modulate_af_fragment = false;
		untextured_fragment = false;
	}
	if (m_impl->active_gpu_vu_draws)
	{
		if (!m_impl->DrawGpuVu(config, source, fast_fragment, psm16_fragment,
				region_repeat_fragment, manual_lod_fragment))
		{
			return;
		}
	}
	else
	{
		const u32 primitive = config.indices_per_prim;
		if (primitive == 0)
		{
			m_impl->Reject("zero indices per primitive");
			return;
		}
		const u32 max_chunk = (MAX_STAGED_INDICES / primitive) * primitive;
		for (u32 first = 0; first < config.nindices;)
		{
			const u32 count = std::min(max_chunk, config.nindices - first);
			if (count == 0 || !m_impl->StageAndDraw(config, config.ps, first,
				count, source, fragment, region_repeat_fragment, fast_fragment,
				programmable_add_fragment,
				programmable_add_direct_fragment,
				programmable_over_fragment,
				psm16_fragment,
				source_only_fragment,
				source_direct_fragment, source_direct_modulate_fragment,
				source_direct_modulate_af_fragment,
				untextured_fragment))
			{
				return;
			}
			first += count;
		}
	}
	if (psm24_fragment && VitaPerformanceTelemetry::IsEnabled())
		s_gxm_worker_performance.psm24_draws++;
	if (rt)
	{
		const u32 selector_keys = static_cast<u32>(config.vs.key) |
			(static_cast<u32>(config.sampler.key) << 8) |
			(static_cast<u32>(config.depth.key) << 16) |
			(static_cast<u32>(config.colormask.key) << 24);
		rt->RecordTfxWriter(source, config.ps.key_lo, config.ps.key_hi,
			config.blend.key, selector_keys,
			static_cast<u8>(config.topology), config.colormask.wrgba,
			PackTelemetryRect(config.drawarea),
			PackTelemetryRect(config.samplearea));
		#if defined(VITASX2_GS_DRAW_TRACE) && VITASX2_GS_DRAW_TRACE
		VitaGsDrawTraceRecordRenderTargetWrite(config, *rt,
			rt->WriterTelemetry().content_generation);
		if (VitaGsDrawTraceRenderTargetAfterDrawNeedsSync(config, *rt,
				rt->WriterTelemetry().content_generation) && m_impl->Finish())
		{
			VitaGsDrawTraceRecordRenderTargetAfterDraw(config, *rt,
				rt->WriterTelemetry().content_generation);
		}
		#endif
	}
	if (rt && !config.ps.no_color && config.colormask.wrgba != 0)
	{
		m_impl->MarkSceneStoreWrite(rt,
			VitaGXM::RenderStoreAllocator::Plane::Color);
	}
	if (ds && config.depth.zwe)
	{
		m_impl->MarkSceneStoreWrite(ds,
			VitaGXM::RenderStoreAllocator::Plane::Depth);
	}
	#if defined(VITASX2_GS_DRAW_TRACE) && VITASX2_GS_DRAW_TRACE
	u32 trace_paths = (psm24_fragment ? VITA_GS_DRAW_PATH_PSM24 : 0u) |
		(psm16_fragment ? VITA_GS_DRAW_PATH_PSM16 : 0u) |
		(mip_lod ? VITA_GS_DRAW_PATH_MIP_LOD : 0u) |
		(region_repeat_fragment ? VITA_GS_DRAW_PATH_REGION_REPEAT : 0u) |
		(fast_fragment ? VITA_GS_DRAW_PATH_FAST_FRAGMENT : 0u) |
		(programmable_add_fragment ? VITA_GS_DRAW_PATH_PROGRAMMABLE_ADD : 0u) |
		(programmable_add_direct_fragment ?
			VITA_GS_DRAW_PATH_PROGRAMMABLE_ADD_DIRECT : 0u) |
		(programmable_over_fragment ? VITA_GS_DRAW_PATH_PROGRAMMABLE_OVER : 0u) |
		(source_only_fragment ? VITA_GS_DRAW_PATH_SOURCE_ONLY : 0u) |
		(source_direct_fragment ? VITA_GS_DRAW_PATH_SOURCE_DIRECT : 0u) |
		(source_direct_modulate_fragment ?
			VITA_GS_DRAW_PATH_SOURCE_DIRECT_MODULATE : 0u) |
		(source_direct_modulate_af_fragment ?
			VITA_GS_DRAW_PATH_SOURCE_DIRECT_MODULATE_AF : 0u) |
		(untextured_fragment ? VITA_GS_DRAW_PATH_UNTEXTURED : 0u) |
		((config.tex && source != config.tex) ? VITA_GS_DRAW_PATH_RT_SNAPSHOT : 0u);
	VitaGsDrawTraceRecordDeviceOutcome(config,
		VitaGsDrawTraceOutcome::Drawn, trace_paths);
	#endif
}

GSTexture* GSDeviceGXM::CreateSurface(GSTexture::Usage usage, int width,
	int height, int levels, GSTexture::Format format)
{
	if (!m_impl || !m_impl->ready)
		return nullptr;
	int error = 0;
	std::unique_ptr<VitaGXM::GSTextureGXM> texture =
		VitaGXM::GSTextureGXM::Create(m_impl.get(), usage, width, height, levels,
			format, &error);
	if (!texture)
	{
		const VitaGXM::Arena::Statistics stats =
			m_impl->texture_arena.GetStatistics();
		Console.Error(
			"GXM GS: create texture failed (%08x): usage=%u format=%u size=%dx%d levels=%d arena=%llu/%llu bytes live=%llu slabs=%llu largest_free=%llu.",
			static_cast<u32>(error), static_cast<u32>(usage),
			static_cast<u32>(format), width, height, levels,
			static_cast<unsigned long long>(stats.allocated_bytes),
			static_cast<unsigned long long>(stats.mapped_capacity),
			static_cast<unsigned long long>(stats.live_allocations),
			static_cast<unsigned long long>(stats.slab_count),
			static_cast<unsigned long long>(stats.largest_free_range));
	}
	return texture.release();
}

void GSDeviceGXM::SetRenderTargetIdentity(GSTexture* texture,
	const GSRenderTargetIdentity& identity)
{
	auto* const native = CheckedCast<VitaGXM::GSTextureGXM>(texture);
	if (!native)
		return;
	static_assert(sizeof(identity.scale) == sizeof(u32));
	u32 scale_bits = 0;
	std::memcpy(&scale_bits, &identity.scale, sizeof(scale_bits));
	native->SetGuestRenderTargetIdentity({identity.base_block,
		identity.buffer_width, identity.psm,
		static_cast<u32>(identity.unscaled_size.x),
		static_cast<u32>(identity.unscaled_size.y), scale_bits,
		identity.depth});
}

std::unique_ptr<GSDownloadTexture> GSDeviceGXM::CreateDownloadTexture(
	u32 width, u32 height, GSTexture::Format format)
{
	if (!m_impl || !m_impl->ready)
		return {};
	int error = 0;
	auto texture = VitaGXM::GSDownloadTextureGXM::Create(m_impl.get(), width,
		height, format, &error);
	if (!texture)
		m_impl->Fail("create GXM download texture", error);
	return texture;
}

void GSDeviceGXM::CopyRect(GSTexture* source_texture,
	GSTexture* destination_texture, const GSVector4i& requested,
	u32 destination_x, u32 destination_y)
{
	if (!m_impl || !source_texture || !destination_texture)
		return;
	auto* source = CheckedCast<VitaGXM::GSTextureGXM>(source_texture);
	auto* destination = CheckedCast<VitaGXM::GSTextureGXM>(destination_texture);
	if (source->GetFormat() != destination->GetFormat() ||
		requested.rempty() || !source->GetRect().rcontains(requested) ||
		destination_x + static_cast<u32>(requested.width()) >
			static_cast<u32>(destination->GetWidth()) ||
		destination_y + static_cast<u32>(requested.height()) >
			static_cast<u32>(destination->GetHeight()))
	{
		m_impl->Reject("invalid or format-converting CopyRect");
		return;
	}
	const bool full_copy = requested.eq(source->GetRect()) && destination_x == 0 &&
		destination_y == 0 && source->GetSize() == destination->GetSize();
	if (source->GetState() == GSTexture::State::Cleared &&
		ProcessClearsBeforeCopy(source, destination, full_copy))
	{
		return;
	}
	if (!m_impl->CommitClear(*source) ||
		!m_impl->EnsureTextureBackingCurrent(*source) ||
		(destination->GetState() == GSTexture::State::Cleared &&
			!m_impl->CommitClear(*destination)) || !m_impl->Finish())
	{
		return;
	}
	const VitaGXM::TextureLevelLayout* src_layout = source->Level(0);
	const VitaGXM::TextureLevelLayout* dst_layout = destination->Level(0);
	if (!src_layout || !dst_layout)
		return;
	const u32 bpp = source->NativeFormat().bytes_per_pixel;
	const size_t row_bytes = static_cast<size_t>(requested.width()) * bpp;
	if (src_layout->tiled || dst_layout->tiled)
	{
		std::vector<u8> staging(row_bytes * static_cast<size_t>(requested.height()));
		const GSVector4i destination_rect(
			static_cast<int>(destination_x), static_cast<int>(destination_y),
			static_cast<int>(destination_x) + requested.width(),
			static_cast<int>(destination_y) + requested.height());
		if (!source->CopyToLinear(0, requested, staging.data(),
				static_cast<u32>(row_bytes)) ||
			!destination->CopyFromLinear(0, destination_rect, staging.data(),
				static_cast<u32>(row_bytes)))
		{
			m_impl->Reject("failed tiled CopyRect staging");
			return;
		}
		destination->SetState(GSTexture::State::Dirty);
		destination->RecordWriter(VitaGXM::TextureWriterKind::Copy, source);
		m_impl->MarkTextureBackingWrite(*destination);
		return;
	}
	const u8* src_base = static_cast<const u8*>(source->LevelData(0)) +
		static_cast<size_t>(requested.y) * src_layout->pitch +
		static_cast<size_t>(requested.x) * bpp;
	u8* dst_base = static_cast<u8*>(destination->LevelData(0)) +
		static_cast<size_t>(destination_y) * dst_layout->pitch +
		static_cast<size_t>(destination_x) * bpp;
	// A raw depth copy moves the host M plane along with Z. It need not
	// describe either the old destination scissor or the source's full mask.
	destination->InvalidateDepthMaskScissor();
	if (source == destination && dst_base > src_base)
	{
		for (int y = requested.height() - 1; y >= 0; y--)
		{
			std::memmove(dst_base + static_cast<size_t>(y) * dst_layout->pitch,
				src_base + static_cast<size_t>(y) * src_layout->pitch, row_bytes);
		}
	}
	else
	{
		for (int y = 0; y < requested.height(); y++)
		{
			std::memmove(dst_base + static_cast<size_t>(y) * dst_layout->pitch,
				src_base + static_cast<size_t>(y) * src_layout->pitch, row_bytes);
		}
	}
	if (source->IsDepthStencil() && source->StencilData() &&
		destination->StencilData())
	{
		const u8* src_stencil = static_cast<const u8*>(source->StencilData()) +
			static_cast<size_t>(requested.y) * source->StencilPitch() + requested.x;
		u8* dst_stencil = static_cast<u8*>(destination->StencilData()) +
			static_cast<size_t>(destination_y) * destination->StencilPitch() +
			destination_x;
		for (int y = 0; y < requested.height(); y++)
		{
			std::memmove(dst_stencil + static_cast<size_t>(y) * destination->StencilPitch(),
				src_stencil + static_cast<size_t>(y) * source->StencilPitch(),
				requested.width());
		}
	}
	destination->SetState(GSTexture::State::Dirty);
	destination->RecordWriter(VitaGXM::TextureWriterKind::Copy, source);
	m_impl->MarkTextureBackingWrite(*destination);
}

void GSDeviceGXM::ClearSamplerCache()
{
}

void GSDeviceGXM::UpdateCLUTTexture(GSTexture*, float, u32, u32, GSTexture*,
	u32, u32)
{
	if (m_impl)
		m_impl->Reject("GPU palette conversion");
}

void GSDeviceGXM::ConvertToIndexedTexture(GSTexture*, float, u32, u32, u32,
	u32, GSTexture*, u32, u32)
{
	if (m_impl)
		m_impl->Reject("GPU color-to-index conversion");
}

void GSDeviceGXM::FilteredDownsampleTexture(GSTexture*, GSTexture*, u32,
	const GSVector2i&, const GSVector4&)
{
	if (m_impl)
		m_impl->Reject("filtered downsample conversion");
}

GSDeviceGXM::GSDeviceGXM()
	: m_impl(std::make_unique<Impl>())
{
}

GSDeviceGXM::~GSDeviceGXM()
{
	Destroy();
}

RenderAPI GSDeviceGXM::GetRenderAPI() const
{
	return RenderAPI::GXM;
}

bool GSDeviceGXM::HasSurface() const
{
	return m_impl && m_impl->ready && m_impl->display.IsReady();
}

bool GSDeviceGXM::Create(GSVSyncMode vsync_mode, bool allow_present_throttle)
{
	if (!GSDevice::Create(vsync_mode, allow_present_throttle))
		return false;
	if (!AcquireWindow(true))
		return false;
	m_name = "Sony SGX543MP4+ / GXM";
	m_max_texture_size = 4096;
	m_features.framebuffer_fetch = true;
	// PCSX2 owner: GSRendererHW::SetupIA(). GXM natively consumes the shader's
	// PSIZE output for point lists, including scaled points, so no six-index
	// vertex-expansion fallback is needed.
	m_features.point_expand = true;
	// PCSX2 owner: GSRendererHW::HandleProvokingVertexFirst(). StageAndDraw()
	// implements the same last-provoking color contract while expanding each
	// primitive into bounded GXM staging chunks, so the generic whole-draw
	// de-index fallback must not run before the device sees the geometry.
	m_features.provoking_vertex_last = true;
	// FRAGCOLOR provides ordered same-pixel framebuffer fetch. Per-primitive
	// draw-list snapshots are not implemented yet, so do not advertise PCSX2's
	// separate multidraw_fb_copy contract.
	m_features.multidraw_fb_copy = false;
	m_features.stencil_buffer = true;
	m_features.prefer_new_textures = false;
	return m_impl && m_impl->Initialize();
}

void GSDeviceGXM::Destroy()
{
	if (!m_impl)
		return;
	if (m_impl->gpu_vu_device_faulted)
	{
		// A notification watchdog means firmware ownership is indeterminate.
		// Sony's normal teardown starts with sceGxmFinish(), then destroys every
		// texture and mapping; doing either here can turn a bounded provider fault
		// into the same system-level hang this gate is intended to contain. Exit
		// only this title and let the kernel revoke its GXM process resources.
		Console.Error(
			"GPU-VU FATAL process_exit=1 reason=indeterminate-gxm-ownership; "
			"skipping sceGxmFinish and user-space resource teardown.");
		(void)VitaGpuVu::HealthJournal::FlushForFatal();
		sceKernelExitProcess(1);
		return;
	}
	GSDevice::Destroy();
	m_impl->Shutdown();
}

void GSDeviceGXM::DestroySurface()
{
	// The fixed Vita display is owned for the complete GXM device lifetime.
}

bool GSDeviceGXM::UpdateWindow()
{
	return HasSurface();
}

void GSDeviceGXM::ResizeWindow(u32 new_width, u32 new_height, float scale)
{
	(void)new_width;
	(void)new_height;
	m_window_info.surface_width = VitaGXM::Display::Width;
	m_window_info.surface_height = VitaGXM::Display::Height;
	m_window_info.surface_scale = scale;
}

bool GSDeviceGXM::SupportsExclusiveFullscreen() const
{
	return false;
}

void GSDeviceGXM::EndGpuVuEpoch()
{
	if (!m_impl || !m_impl->ready || m_impl->present_active)
		return;
	// One ordered GS command epoch. Direct descriptors and their mapped input
	// storage belong to the guest frame that produced them, so the owning GXM
	// scene is submitted at the epoch boundary even when presentation is
	// skipped. Retention stays with the four-way notification ring; nothing
	// waits here.
	if (m_impl->HasPendingGpuVuSceneDraws())
		m_impl->EndScene(false);
}

bool GSDeviceGXM::WaitForGpuVuInputRetirement(
	const VitaGpuVu::RawVifPayloadRef& blocked_generation)
{
	return m_impl && m_impl->ready &&
		m_impl->WaitForGpuVuInputRetirement(blocked_generation);
}

void GSDeviceGXM::HandleGpuVuNotificationTimeout(
	uptr address, u32 required_value, u32 observed_value,
	u64 sequence, u64 elapsed_us)
{
	if (!m_impl)
		return;
	m_impl->FailGpuVuRetirementOwner("notification-watchdog", address,
		required_value, observed_value, sequence, elapsed_us);
}

bool GSDeviceGXM::HasFatalGpuFault() const
{
	return m_impl && m_impl->gpu_vu_device_faulted;
}

GSDevice::PresentResult GSDeviceGXM::BeginPresent(bool frame_skip)
{
	if (!m_impl || !m_impl->ready)
		return PresentResult::DeviceLost;
	if (frame_skip)
	{
		m_impl->EndScene(false);
		return PresentResult::FrameSkipped;
	}
	#if defined(VITASX2_GS_DRAW_TRACE) && VITASX2_GS_DRAW_TRACE
	// The merged output may still be owned by an in-flight GXM scene here.
	// Finish exactly once in the opt-in capture frame before the CPU reads its
	// tiled storage; normal presentation retains the asynchronous path below.
	if (VitaGsDrawTraceNeedsPresentSourceCapture())
	{
		if (!m_impl->Finish())
			return PresentResult::DeviceLost;
		VitaGsDrawTraceCapturePresentSource();
	}
	#endif
	// PCSX2's device backends submit presentation after the preceding render
	// pass without forcing the GPU idle. Sony's basic/display_queue samples use
	// the same ordered-context + display-sync contract: EndScene, begin the
	// back-buffer scene with its sync object, then queue the flip. Geometry is
	// retained monotonically and drained only when its staging arena wraps.
	if (!m_impl->BeginDisplayScene())
		return PresentResult::DeviceLost;
	const GSVector4 full(0.0f, 0.0f, static_cast<float>(VitaGXM::Display::Width),
		static_cast<float>(VitaGXM::Display::Height));
	if (!m_impl->DrawQuad(nullptr, GSVector4::zero(), full, 0xff000000u,
		Nearest, nullptr))
	{
		m_impl->EndScene(false);
		return PresentResult::DeviceLost;
	}
	m_impl->present_active = true;
	return PresentResult::OK;
}

void GSDeviceGXM::EndPresent()
{
	if (!m_impl || !m_impl->present_active)
		return;
	m_impl->present_active = false;
	if (!m_impl->EndScene(false))
		return;
#if defined(VITASX2_GS_DRAW_TRACE) && VITASX2_GS_DRAW_TRACE
	if (VitaGsDrawTraceNeedsDisplayCapture())
	{
		if (!m_impl->Finish())
			return;
		VitaGsDrawTraceRecordDisplay(m_impl->display.BackBufferAddress(),
			VitaGXM::Display::StrideInPixels * sizeof(u32),
			VitaGXM::Display::Width, VitaGXM::Display::Height, sizeof(u32));
	}
#endif
	m_impl->RetuneRenderTargets();
	int result = TimeGxmHostCall(VitaGxmHostCall::Heartbeat, [&] {
		return sceGxmPadHeartbeat(m_impl->display.BackColorSurface(),
			m_impl->display.BackSyncObject());
	});
	if (result < 0)
	{
		m_impl->Fail("sceGxmPadHeartbeat", result);
		return;
	}
	result = TimeGxmHostCall(VitaGxmHostCall::QueuePresent,
		[&] { return m_impl->display.QueuePresent(); });
	if (result < 0)
		m_impl->Fail("display queue present", result);
}

void GSDeviceGXM::SetVSyncMode(GSVSyncMode mode, bool allow_present_throttle)
{
	m_vsync_mode = mode;
	m_allow_present_throttle = allow_present_throttle;
}

std::string GSDeviceGXM::GetDriverInfo() const
{
	return "Sony libGXM; SGX543MP4+; direct PCSX2 GSRendererHW backend";
}

bool GSDeviceGXM::SetGPUTimingEnabled(bool enabled)
{
	return !enabled;
}

float GSDeviceGXM::GetAndResetAccumulatedGPUTime()
{
	return 0.0f;
}

bool GSDeviceGXM::SetGPUPipelineStatisticsEnabled(bool enabled)
{
	return !enabled;
}

GPUPipelineStatistics GSDeviceGXM::GetAndResetAccumulatedGPUPipelineStatistics()
{
	return {};
}

void GSDeviceGXM::PushDebugGroup(const char*, ...)
{
}

void GSDeviceGXM::PopDebugGroup()
{
}

void GSDeviceGXM::InsertDebugMessage(DebugMessageCategory, const char*, ...)
{
}

void GSDeviceGXM::PresentRect(GSTexture* source_texture,
	const GSVector4& source_rect, GSTexture* destination_texture,
	const GSVector4& destination_rect, PresentShader shader, float shader_time,
	Filter filter)
{
	(void)shader_time;
	if (!m_impl || shader != PresentShader::COPY || destination_texture)
	{
		if (m_impl)
			m_impl->Reject("non-copy or texture-targeted PresentRect");
		return;
	}
	auto* source = CheckedCast<VitaGXM::GSTextureGXM>(source_texture);
	if (!source || !m_impl->CommitClear(*source))
	{
		m_impl->Reject("failed final presentation source preparation");
		return;
	}
	const bool store_view = m_impl->BindLatestRenderStoreView(*source);
	if (!store_view && !m_impl->EnsureTextureBackingCurrent(*source))
	{
		m_impl->Reject("failed final presentation source materialization");
		return;
	}
	if (!m_impl->scene_active || !m_impl->scene_is_display)
	{
		if (!m_impl->EndScene(false) || !m_impl->BeginDisplayScene())
		{
			source->ClearSamplingOverride();
			return;
		}
		const GSVector4 full(0.0f, 0.0f,
			static_cast<float>(VitaGXM::Display::Width),
			static_cast<float>(VitaGXM::Display::Height));
		if (!m_impl->DrawQuad(nullptr, GSVector4::zero(), full, 0xff000000u,
				Nearest, nullptr))
		{
			source->ClearSamplingOverride();
			return;
		}
	}
	if (VitaPerformanceTelemetry::IsEnabled())
		s_gxm_worker_performance.present_calls++;
	const bool drawn = m_impl->DrawQuad(source, source_rect, destination_rect,
		0xffffffffu, filter, m_impl->copy_programs[0xf]);
	source->ClearSamplingOverride();
	if (!drawn)
	{
		m_impl->Reject("failed final presentation copy");
	}
}

void GSDeviceGXM::DoStretchRect(GSTexture* source_texture,
	const GSVector4& source_rect, GSTexture* destination_texture,
	const GSVector4& destination_rect, ShaderConvertSelector shader,
	Filter filter)
{
	if (!m_impl || !source_texture || !destination_texture)
		return;

	SceGxmFragmentProgram* fragment = nullptr;
	switch (shader.Shader())
	{
		case ShaderConvert::COPY:
			fragment = m_impl->copy_programs[shader.Mask() & 0xf];
			break;
		case ShaderConvert::RTA_CORRECTION:
			fragment = m_impl->rta_correction_programs[shader.Mask() & 0xf];
			break;
		case ShaderConvert::RTA_DECORRECTION:
			// PCSX2 declares decorrection as a fixed full-write conversion.
			if (shader.Mask() == 0xf)
				fragment = m_impl->rta_decorrection_program;
			break;
		default:
			break;
	}
	if (!fragment)
	{
		// A missing device conversion is a persistent capability fact, not a
		// per-frame event. Preserve one actionable record per conversion while
		// avoiding synchronous file/console traffic on every repeated draw.
		static u32 logged_conversions = 0;
		const u32 shader_index = static_cast<u32>(shader.Shader());
		const u32 shader_bit = shader_index < 32 ? (1u << shader_index) : 0;
		if (shader_bit == 0 || !(logged_conversions & shader_bit))
		{
			logged_conversions |= shader_bit;
			Console.Error(
				"GXM GS: rejected StretchRect conversion: shader=%s mask=%u src_format=%u dst_format=%u src=(%.3f,%.3f,%.3f,%.3f) dst=(%.1f,%.1f,%.1f,%.1f); further rejects for this conversion are suppressed.",
				shader.Name(), shader.Mask(),
				static_cast<u32>(source_texture->GetFormat()),
				static_cast<u32>(destination_texture->GetFormat()),
				source_rect.x, source_rect.y, source_rect.z, source_rect.w,
				destination_rect.x, destination_rect.y,
				destination_rect.z, destination_rect.w);
		}
		return;
	}
	auto* source = CheckedCast<VitaGXM::GSTextureGXM>(source_texture);
	auto* destination = CheckedCast<VitaGXM::GSTextureGXM>(destination_texture);
	if (source == destination)
	{
		source = m_impl->SnapshotTexture(*source, source->GetRect());
		if (!source)
			return;
	}
	if (!m_impl->CommitClear(*source) ||
		!m_impl->EnsureTextureBackingCurrent(*source) ||
		!m_impl->EnsureScene(destination, nullptr, destination->GetRect()) ||
		!m_impl->DrawQuad(source, source_rect, destination_rect, 0xffffffffu,
			filter, fragment))
	{
		m_impl->Reject("failed copy StretchRect");
		return;
	}
	destination->SetState(GSTexture::State::Dirty);
	destination->RecordWriter(VitaGXM::TextureWriterKind::Copy, source);
	m_impl->MarkSceneStoreWrite(destination,
		VitaGXM::RenderStoreAllocator::Plane::Color);
}

void GSDeviceGXM::DoStretchRect(GSTexture* source_texture,
	const GSVector4& source_rect, const GSVector4& destination_rect,
	PresentShader shader, Filter filter)
{
	PresentRect(source_texture, source_rect, nullptr, destination_rect, shader,
		0.0f, filter);
}

void GSDeviceGXM::DoMerge(GSTexture* sources[3], GSVector4* source_rects,
	GSTexture* destination_texture, GSVector4* destination_rects,
	const GSRegPMODE& pmode, const GSRegEXTBUF& extbuf, u32 background,
	Filter filter)
{
	if (!m_impl || !destination_texture)
		return;
	const bool performance_telemetry_enabled =
		VitaPerformanceTelemetry::IsEnabled();
	VitaGxmPerformanceCounters& performance = s_gxm_worker_performance;
	if (performance_telemetry_enabled)
	{
		performance.merge_calls++;
		performance.last_merge_pmode = pmode.U64;
		performance.last_merge_extbuf = extbuf.U64;
		performance.last_merge_background = background;
		performance.last_merge_source_mask =
			(sources[0] ? 1u : 0u) | (sources[1] ? 2u : 0u) |
			(sources[2] ? 4u : 0u);
		performance.last_merge_source_states =
			(sources[0] ? static_cast<u8>(sources[0]->GetState()) & 0xfu : 0u) |
			(sources[1] ?
				(static_cast<u8>(sources[1]->GetState()) & 0xfu) << 4 : 0u);
		for (u32 i = 0; i < 2; i++)
		{
			performance.last_merge_source_sizes[i] = sources[i] ?
				(static_cast<u32>(sources[i]->GetWidth()) & 0xffffu) |
				((static_cast<u32>(sources[i]->GetHeight()) & 0xffffu) << 16) : 0u;
			auto* source = CheckedCast<VitaGXM::GSTextureGXM>(sources[i]);
			performance.last_merge_source_ids[i] = source ? source->TelemetryId() : 0;
		}
	}
	const bool feedback_2 = pmode.EN2 && sources[2] && extbuf.FBIN == 1;
	const bool feedback_1 = pmode.EN1 && sources[2] && extbuf.FBIN == 0;
	if (feedback_1 || feedback_2)
	{
		m_impl->Reject("PCRTC EXTWRITE YUV feedback merge");
		return;
	}
	auto* destination = CheckedCast<VitaGXM::GSTextureGXM>(destination_texture);
	auto* source_2 = (sources[1] && pmode.SLBG == 0) ?
		CheckedCast<VitaGXM::GSTextureGXM>(sources[1]) : nullptr;
	auto* source_1 = sources[0] ?
		CheckedCast<VitaGXM::GSTextureGXM>(sources[0]) : nullptr;
	// Follow the frontmost enabled circuit, or RC2 when it is the only PCSX2
	// merge input. The texture's GS-worker provenance identifies the precise
	// operation which produced the image that PCRTC is about to consume.
	const VitaGXM::GSTextureGXM* trace_source = source_1 ? source_1 : source_2;
	if (performance_telemetry_enabled)
	{
		performance.last_merge_trace_circuit = source_1 ? 1u : (source_2 ? 2u : 0u);
		if (trace_source)
		{
			const VitaGXM::TextureWriterTelemetry& writer =
				trace_source->WriterTelemetry();
			performance.last_merge_writer_tfx_writes = writer.tfx_writes;
			performance.last_merge_writer_ps_lo = writer.ps_lo;
			performance.last_merge_writer_ps_hi = writer.ps_hi;
			performance.last_merge_writer_draw_area = writer.draw_area;
			performance.last_merge_writer_sample_area = writer.sample_area;
			performance.last_merge_writer_source_id = writer.source_id;
			performance.last_merge_writer_source_size = writer.source_size;
			performance.last_merge_writer_blend = writer.blend;
			performance.last_merge_writer_selector_keys = writer.selector_keys;
			performance.last_merge_writer_kind = static_cast<u8>(writer.kind);
			performance.last_merge_writer_topology = writer.topology;
			performance.last_merge_parent_tfx_writes =
				writer.source_writer_tfx_writes;
			performance.last_merge_parent_textured_tfx_writes =
				writer.source_writer_textured_tfx_writes;
			performance.last_merge_parent_untextured_tfx_writes =
				writer.source_writer_untextured_tfx_writes;
			performance.last_merge_parent_render_target_source_tfx_writes =
				writer.source_writer_render_target_source_tfx_writes;
			performance.last_merge_parent_full_mask_tfx_writes =
				writer.source_writer_full_mask_tfx_writes;
			performance.last_merge_parent_rgb_only_tfx_writes =
				writer.source_writer_rgb_only_tfx_writes;
			performance.last_merge_parent_alpha_only_tfx_writes =
				writer.source_writer_alpha_only_tfx_writes;
			performance.last_merge_parent_other_mask_tfx_writes =
				writer.source_writer_other_mask_tfx_writes;
			performance.last_merge_parent_ps_lo = writer.source_writer_ps_lo;
			performance.last_merge_parent_ps_hi = writer.source_writer_ps_hi;
			performance.last_merge_parent_draw_area = writer.source_writer_draw_area;
			performance.last_merge_parent_sample_area =
				writer.source_writer_sample_area;
			performance.last_merge_parent_source_id = writer.source_writer_source_id;
			performance.last_merge_parent_source_size =
				writer.source_writer_source_size;
			performance.last_merge_parent_blend = writer.source_writer_blend;
			performance.last_merge_parent_selector_keys =
				writer.source_writer_selector_keys;
			performance.last_merge_parent_last_rgb_ps_lo =
				writer.source_writer_last_rgb_ps_lo;
			performance.last_merge_parent_last_rgb_ps_hi =
				writer.source_writer_last_rgb_ps_hi;
			performance.last_merge_parent_last_rgb_draw_area =
				writer.source_writer_last_rgb_draw_area;
			performance.last_merge_parent_last_rgb_sample_area =
				writer.source_writer_last_rgb_sample_area;
			performance.last_merge_parent_last_rgb_source_id =
				writer.source_writer_last_rgb_source_id;
			performance.last_merge_parent_last_rgb_source_size =
				writer.source_writer_last_rgb_source_size;
			performance.last_merge_parent_last_rgb_blend =
				writer.source_writer_last_rgb_blend;
			performance.last_merge_parent_last_rgb_selector_keys =
				writer.source_writer_last_rgb_selector_keys;
			performance.last_merge_parent_kind =
				static_cast<u8>(writer.source_writer_kind);
			performance.last_merge_parent_topology = writer.source_writer_topology;
			performance.last_merge_parent_color_mask =
				writer.source_writer_color_mask;
			performance.last_merge_parent_last_rgb_topology =
				writer.source_writer_last_rgb_topology;
			performance.last_merge_parent_last_rgb_color_mask =
				writer.source_writer_last_rgb_color_mask;
		}
		else
		{
			performance.last_merge_writer_tfx_writes = 0;
			performance.last_merge_writer_ps_lo = 0;
			performance.last_merge_writer_ps_hi = 0;
			performance.last_merge_writer_draw_area = 0;
			performance.last_merge_writer_sample_area = 0;
			performance.last_merge_writer_source_id = 0;
			performance.last_merge_writer_source_size = 0;
			performance.last_merge_writer_blend = 0;
			performance.last_merge_writer_selector_keys = 0;
			performance.last_merge_writer_kind = 0;
			performance.last_merge_writer_topology = 0;
			performance.last_merge_parent_tfx_writes = 0;
			performance.last_merge_parent_textured_tfx_writes = 0;
			performance.last_merge_parent_untextured_tfx_writes = 0;
			performance.last_merge_parent_render_target_source_tfx_writes = 0;
			performance.last_merge_parent_full_mask_tfx_writes = 0;
			performance.last_merge_parent_rgb_only_tfx_writes = 0;
			performance.last_merge_parent_alpha_only_tfx_writes = 0;
			performance.last_merge_parent_other_mask_tfx_writes = 0;
			performance.last_merge_parent_ps_lo = 0;
			performance.last_merge_parent_ps_hi = 0;
			performance.last_merge_parent_draw_area = 0;
			performance.last_merge_parent_sample_area = 0;
			performance.last_merge_parent_source_id = 0;
			performance.last_merge_parent_source_size = 0;
			performance.last_merge_parent_blend = 0;
			performance.last_merge_parent_selector_keys = 0;
			performance.last_merge_parent_last_rgb_ps_lo = 0;
			performance.last_merge_parent_last_rgb_ps_hi = 0;
			performance.last_merge_parent_last_rgb_draw_area = 0;
			performance.last_merge_parent_last_rgb_sample_area = 0;
			performance.last_merge_parent_last_rgb_source_id = 0;
			performance.last_merge_parent_last_rgb_source_size = 0;
			performance.last_merge_parent_last_rgb_blend = 0;
			performance.last_merge_parent_last_rgb_selector_keys = 0;
			performance.last_merge_parent_kind = 0;
			performance.last_merge_parent_topology = 0;
			performance.last_merge_parent_color_mask = 0;
			performance.last_merge_parent_last_rgb_topology = 0;
			performance.last_merge_parent_last_rgb_color_mask = 0;
		}
	}
	// GSDeviceVK::DoMerge owns this ordering. A lazy source clear may retire the
	// current scene, so materialize both PCRTC inputs before opening dTex.
	if ((source_2 && (!m_impl->CommitClear(*source_2) ||
			!m_impl->EnsureTextureBackingCurrent(*source_2))) ||
		(source_1 && (!m_impl->CommitClear(*source_1) ||
			!m_impl->EnsureTextureBackingCurrent(*source_1))))
	{
		return;
	}
	ClearRenderTarget(destination, background);
	if (!m_impl->EnsureScene(destination, nullptr, destination->GetRect()))
		return;
	if (source_2)
	{
		if (!m_impl->DrawQuad(source_2, source_rects[1], destination_rects[1],
				0xffffffffu, filter, m_impl->copy_programs[0xf]))
		{
			Console.Error(
				"GXM GS: failed PCRTC RC2 copy: source_failed=%u src=(%.3f,%.3f,%.3f,%.3f) dst=(%.1f,%.1f,%.1f,%.1f).",
				static_cast<u32>(source_2->HadOperationFailure()),
				source_rects[1].x, source_rects[1].y,
				source_rects[1].z, source_rects[1].w,
				destination_rects[1].x, destination_rects[1].y,
				destination_rects[1].z, destination_rects[1].w);
			return;
		}
		if (performance_telemetry_enabled)
			performance.merge_rc2_draws++;
	}
	if (source_1)
	{
		const u32 color = 0x00ffffffu | (static_cast<u32>(pmode.ALP) << 24);
		SceGxmFragmentProgram* fragment = nullptr;
		if (pmode.MMOD == 1)
		{
			fragment = pmode.AMOD ? m_impl->present_alpha_rgb_program :
				m_impl->present_alpha_program;
		}
		else
		{
			fragment = pmode.AMOD ? m_impl->present_source_alpha_rgb_program :
				m_impl->present_source_alpha_program;
		}
		if (!m_impl->DrawQuad(source_1, source_rects[0], destination_rects[0],
				color, filter, fragment))
		{
			m_impl->Reject("failed PCRTC RC1 blend");
			return;
		}
		if (performance_telemetry_enabled)
			performance.merge_rc1_draws++;
	}
	destination->SetState(GSTexture::State::Dirty);
	destination->RecordWriter(VitaGXM::TextureWriterKind::Merge, trace_source);
	m_impl->MarkSceneStoreWrite(destination,
		VitaGXM::RenderStoreAllocator::Plane::Color);
}

void GSDeviceGXM::DoInterlace(GSTexture* source_texture,
	const GSVector4& source_rect, GSTexture* destination_texture,
	const GSVector4& destination_rect, ShaderInterlace shader, Filter filter,
	const InterlaceConstantBuffer& cb)
{
	if (!m_impl || !source_texture || !destination_texture)
		return;
	if (VitaPerformanceTelemetry::IsEnabled())
		s_gxm_worker_performance.interlace_calls++;
	SceGxmFragmentProgram* program = nullptr;
	const SceGxmProgramParameter* uniform = nullptr;
	if (shader == ShaderInterlace::MAD_BUFFER)
	{
		program = m_impl->mad_buffer_program;
		uniform = m_impl->uniforms.interlace[0];
	}
	else if (shader == ShaderInterlace::MAD_RECONSTRUCT)
	{
		program = m_impl->mad_reconstruct_program;
		uniform = m_impl->uniforms.interlace[1];
	}
	else
	{
		m_impl->Reject("weave/bob/blend interlace shader");
		return;
	}
	auto* source = CheckedCast<VitaGXM::GSTextureGXM>(source_texture);
	auto* destination = CheckedCast<VitaGXM::GSTextureGXM>(destination_texture);
	if (!m_impl->CommitClear(*source) ||
		!m_impl->EnsureTextureBackingCurrent(*source) ||
		!m_impl->EnsureScene(destination, nullptr, destination->GetRect()) ||
		!m_impl->DrawQuad(source, source_rect, destination_rect, 0xffffffffu,
			filter, program, uniform, cb.ZrH.F32))
	{
		m_impl->Reject("failed FastMAD interlace pass");
		return;
	}
	destination->SetState(GSTexture::State::Dirty);
	destination->RecordWriter(VitaGXM::TextureWriterKind::Interlace, source);
	m_impl->MarkSceneStoreWrite(destination,
		VitaGXM::RenderStoreAllocator::Plane::Color);
}

void GSDeviceGXM::DoFXAA(GSTexture*, GSTexture*)
{
}

void GSDeviceGXM::DoShadeBoost(GSTexture*, GSTexture*, const float[4])
{
}

bool GSDeviceGXM::DoCAS(GSTexture*, GSTexture*, bool,
	const std::array<u32, NUM_CAS_CONSTANTS>&)
{
	return false;
}

void GSDeviceGXM::Impl::Shutdown()
{
	ready = false;
	present_active = false;
	VitaGpuVu::DetachGeneratedProgramCompiler(&gpu_vu_shader_compiler);
	gpu_vu_shader_compiler.Stop();
	const auto succeeded = [this](const char* operation, int result) {
		return result >= 0 ? true : Fail(operation, result);
	};

	// Sony's api_libgxm/basic teardown owns this ordering. No GPU command or
	// display callback may retain a pointer when its program, surface, or mapped
	// storage is released.
	if (context)
	{
		if (!Finish())
			return;
	}
	else
	{
		ReleaseAllGpuVuDraws();
	}
	if (!gpu_vu_input_ring.Shutdown())
		return;
	if (!succeeded("sceGxmDisplayQueueFinish(shutdown)", display.Finish()) ||
		!succeeded("GXM display destroy", display.Destroy()))
	{
		return;
	}

	// Device-private textures use the same lifetime contract as PCSX2's pool.
	// Destroy them only after the context and display queue are drained, and
	// before their arenas are unmapped.
	white_texture.reset();
	feedback_textures.clear();
	post_textures.clear();
	quarantined_allocations.clear();
	render_store_planner.Reset();
	render_store_color.Reset();
	render_store_depth.Reset();
	render_store_stencil.Reset();
	render_store_color_surface = {};
	render_store_depth_surface = {};
	render_store_ready = false;

	bool group_ok = true;
	const auto release_fragment = [&](SceGxmFragmentProgram*& program,
		const char* operation) {
		if (!patcher || !program)
			return;
		if (succeeded(operation,
			sceGxmShaderPatcherReleaseFragmentProgram(patcher, program)))
			program = nullptr;
		else
			group_ok = false;
	};
	const auto release_vertex = [&](SceGxmVertexProgram*& program,
		const char* operation) {
		if (!patcher || !program)
			return;
		if (succeeded(operation,
			sceGxmShaderPatcherReleaseVertexProgram(patcher, program)))
			program = nullptr;
		else
			group_ok = false;
	};
	for (auto& entry : generated_vu_programs)
	{
		release_fragment(entry.second.opaque_fragment_program,
			"release generated VU1+TFX opaque fragment program");
		release_fragment(entry.second.zfloor_fragment_program,
			"release generated VU1+TFX Z-floor fragment program");
		release_fragment(entry.second.general_fragment_program,
			"release generated VU1+TFX general fragment program");
		release_vertex(entry.second.vertex_program,
			"release generated VU1+TFX vertex program");
	}
#if defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
	release_fragment(gpu_vu_universal_fragment_program,
		"release universal GPU-VU validation fragment program");
	release_vertex(gpu_vu_universal_vertex_program,
		"release universal GPU-VU validation vertex program");
#endif
	if (!group_ok)
		return;
	release_fragment(mask_update_program, "release mask-update fragment program");
	release_fragment(color_fragment_program, "release color fragment program");
	release_fragment(mad_reconstruct_program, "release MAD-reconstruct program");
	release_fragment(mad_buffer_program, "release MAD-buffer program");
	release_fragment(present_source_alpha_rgb_program,
		"release source-alpha RGB merge program");
	release_fragment(present_alpha_rgb_program,
		"release constant-alpha RGB merge program");
	release_fragment(present_source_alpha_program,
		"release source-alpha merge program");
	release_fragment(present_alpha_program,
		"release constant-alpha merge program");
	release_fragment(rta_decorrection_program,
		"release RTA-decorrection program");
	for (SceGxmFragmentProgram*& program : rta_correction_programs)
		release_fragment(program, "release masked RTA-correction program");
	for (SceGxmFragmentProgram*& program : copy_programs)
		release_fragment(program, "release masked copy program");
	for (auto& entry : tfx_fast_programs)
		release_fragment(entry.second, "release fixed-function TFX program");
	tfx_fast_programs.clear();
	for (auto& entry : tfx_untextured_programs)
		release_fragment(entry.second, "release untextured TFX program");
	tfx_untextured_programs.clear();
	for (auto& entry : tfx_source_programs)
		release_fragment(entry.second, "release source-only TFX program");
	tfx_source_programs.clear();
	for (auto& entry : tfx_source_direct_programs)
		release_fragment(entry.second,
			"release source-only direct-texture TFX program");
	tfx_source_direct_programs.clear();
	for (auto& entry : tfx_source_direct_modulate_programs)
		release_fragment(entry.second,
			"release source-only direct MODULATE/RGBA TFX program");
	tfx_source_direct_modulate_programs.clear();
	for (auto& entry : tfx_source_direct_modulate_af_programs)
		release_fragment(entry.second,
			"release source-only direct MODULATE/RGBA (Cs-0)*Af+0 TFX program");
	tfx_source_direct_modulate_af_programs.clear();
	for (auto& entry : tfx_source_untextured_programs)
		release_fragment(entry.second,
			"release source-only untextured TFX program");
	tfx_source_untextured_programs.clear();
	release_fragment(tfx_programmable_add_program,
		"release programmable additive TFX fragment program");
	release_fragment(tfx_programmable_add_direct_program,
		"release direct-texture programmable additive TFX fragment program");
	release_fragment(tfx_programmable_over_program,
		"release programmable source-alpha TFX fragment program");
	release_fragment(tfx_opaque_program, "release opaque TFX fragment program");
	release_fragment(tfx_fast_no_atst_program, "release no-alpha-test fast TFX fragment program");
	for (auto*& program : gs_blend_programs)
		release_fragment(program, "release native GS blend program");
	release_fragment(tfx_zfloor_source_direct_decal_af_program,
		"release Z-floor direct DECAL/RGB constant-blend program");
	release_fragment(tfx_psm16_zfloor_fragment_program,
		"release PSMCT16 Z-floor TFX fragment program");
	release_fragment(tfx_psm16_fragment_program,
		"release PSMCT16 TFX fragment program");
	release_fragment(tfx_zfloor_fragment_program,
		"release Z-floor TFX fragment program");
	release_fragment(tfx_manual_lod_zfloor_fragment_program,
		"release manual-LOD Z-floor TFX fragment program");
	release_fragment(tfx_manual_lod_fragment_program,
		"release manual-LOD TFX fragment program");
	release_fragment(tfx_fragment_program, "release TFX fragment program");
	release_vertex(color_vertex_program, "release color vertex program");
	release_vertex(present_vertex_program, "release presentation vertex program");
	release_vertex(tfx_vertex_program, "release TFX vertex program");
	if (!group_ok)
		return;

	const auto unregister = [&](SceGxmShaderPatcherId& id,
		const char* operation) {
		if (!patcher || !id)
			return;
		if (succeeded(operation, sceGxmShaderPatcherUnregisterProgram(patcher, id)))
			id = nullptr;
		else
			group_ok = false;
	};
	for (auto& entry : generated_vu_programs)
	{
		unregister(entry.second.id,
			"unregister generated VU1+TFX vertex program");
	}
#if defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
	unregister(gpu_vu_universal_fragment_id,
		"unregister universal GPU-VU validation fragment program");
	unregister(gpu_vu_universal_vertex_id,
		"unregister universal GPU-VU validation vertex program");
#endif
	if (!group_ok)
		return;
	generated_vu_programs.clear();
#if defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
	// Generated serial programs link against the universal fragment owner, so
	// release and unregister every generated link before this shared owner.
	ReleaseUniversalGpuVuProduct();
#endif
	VitaGpuVu::ClearUniversalStateMachineProgramCache();
	VitaGpuVu::ClearGeneratedProgramRegistry();
	unregister(mad_reconstruct_fragment_id, "unregister MAD-reconstruct fragment");
	unregister(mad_buffer_fragment_id, "unregister MAD-buffer fragment");
	unregister(color_fragment_id, "unregister color fragment");
	unregister(color_vertex_id, "unregister color vertex");
	unregister(rta_decorrection_fragment_id,
		"unregister RTA-decorrection fragment");
	unregister(rta_correction_fragment_id,
		"unregister RTA-correction fragment");
	unregister(copy_fragment_id, "unregister copy fragment");
	unregister(merge_fragment_id, "unregister merge fragment");
	unregister(present_fragment_id, "unregister present fragment");
	unregister(present_vertex_id, "unregister present vertex");
	unregister(tfx_source_untextured_fragment_id,
		"unregister source-only untextured TFX fragment");
	unregister(tfx_zfloor_source_direct_decal_af_fragment_id,
		"unregister Z-floor direct DECAL/RGB (Cs-0)*Af+0 TFX fragment");
	unregister(tfx_psm16_zfloor_fragment_id,
		"unregister PSMCT16 Z-floor TFX fragment");
	unregister(tfx_psm16_fragment_id,
		"unregister PSMCT16 TFX fragment");
	unregister(tfx_source_direct_fragment_id,
		"unregister source-only direct-texture TFX fragment");
	unregister(tfx_source_direct_modulate_fragment_id,
		"unregister source-only direct MODULATE/RGBA TFX fragment");
	unregister(tfx_source_direct_modulate_af_fragment_id,
		"unregister source-only direct MODULATE/RGBA (Cs-0)*Af+0 TFX fragment");
	unregister(tfx_programmable_add_fragment_id,
		"unregister programmable additive TFX fragment");
	unregister(tfx_programmable_add_direct_fragment_id,
		"unregister direct-texture programmable additive TFX fragment");
	unregister(tfx_programmable_over_fragment_id,
		"unregister programmable source-alpha TFX fragment");
	unregister(tfx_source_fragment_id,
		"unregister source-only TFX fragment");
	unregister(tfx_untextured_fragment_id,
		"unregister untextured TFX fragment");
	unregister(tfx_region_repeat_fast_fragment_id,
		"unregister fast REGION_REPEAT TFX fragment");
	unregister(tfx_region_repeat_fragment_id,
		"unregister REGION_REPEAT TFX fragment");
	unregister(tfx_uv_no_fog_zfloor_fragment_id,
		"unregister Z-floor fixed-UV/no-fog TFX fragment");
	unregister(tfx_uv_no_fog_fast_fragment_id,
		"unregister fast fixed-UV/no-fog TFX fragment");
	unregister(tfx_uv_no_fog_fragment_id,
		"unregister fixed-UV/no-fog TFX fragment");
	unregister(tfx_fast_fragment_id, "unregister fast TFX fragment");
	unregister(tfx_fast_no_atst_fragment_id, "unregister no-alpha-test fast TFX fragment");
	for (auto& id : gs_blend_ids)
		unregister(id, "unregister native GS blend fragment");
	unregister(tfx_zfloor_fragment_id, "unregister Z-floor TFX fragment");
	unregister(tfx_manual_lod_zfloor_fragment_id,
		"unregister manual-LOD Z-floor TFX fragment");
	unregister(tfx_manual_lod_fragment_id,
		"unregister manual-LOD TFX fragment");
	unregister(tfx_fragment_id, "unregister TFX fragment");
	unregister(tfx_vertex_id, "unregister TFX vertex");
	if (!group_ok)
		return;

	for (RenderTargetEntry& entry : render_targets)
	{
		for (SceGxmRenderTarget*& target : entry.targets)
		{
			if (target &&
				!succeeded("destroy cached render target",
					sceGxmDestroyRenderTarget(target)))
			{
				group_ok = false;
			}
			else
			{
				target = nullptr;
			}
		}
	}
	if (display_render_target &&
		!succeeded("destroy display render target",
			sceGxmDestroyRenderTarget(display_render_target)))
	{
		group_ok = false;
	}
	else
	{
		display_render_target = nullptr;
	}
	if (!group_ok)
		return;
	render_targets.clear();

	const auto release_block = [&](VitaGXM::MappedBlock& block,
		const char* operation) {
		if (block.IsAllocated() &&
			!succeeded(operation, VitaGXM::ReleaseMappedBlock(&block)))
		{
			group_ok = false;
		}
	};
	release_block(geometry_indices, "release staged indices");
	release_block(geometry_vertices, "release staged vertices");
#if defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
	release_block(gpu_vu_universal_validation_buffer,
		"release universal GPU-VU validation state");
#endif
	if (!group_ok ||
		!succeeded("destroy texture arena", texture_arena.Destroy()) ||
		!succeeded("destroy transfer arena", transfer_arena.Destroy()))
	{
		return;
	}

	if (patcher)
	{
		if (!succeeded("sceGxmShaderPatcherDestroy",
			sceGxmShaderPatcherDestroy(patcher)))
		{
			return;
		}
		patcher = nullptr;
	}
	release_block(patcher_fragment_usse, "release patcher fragment USSE");
	release_block(patcher_vertex_usse, "release patcher vertex USSE");
	release_block(patcher_buffer, "release patcher buffer");
	if (!group_ok)
		return;

	if (context)
	{
		if (!succeeded("sceGxmDestroyContext", sceGxmDestroyContext(context)))
			return;
		context = nullptr;
	}
	release_block(fragment_usse_ring, "release fragment USSE ring");
	release_block(fragment_ring, "release fragment ring");
	release_block(vertex_ring, "release vertex ring");
	release_block(vdm_ring, "release VDM ring");
	if (!group_ok)
		return;
	std::free(context_host);
	context_host = nullptr;

	if (gxm_initialized)
	{
		if (!succeeded("sceGxmTerminate", sceGxmTerminate()))
			return;
		gxm_initialized = false;
	}
	gpu_vu_retirements_ready = false;
	for (GpuVuRetirementSlot& slot : gpu_vu_retirement_slots)
	{
		slot.notification = {};
		slot.fragment_notification = {};
	}
	gpu_vu_fragment_progress_notification = {};
	scene_active = false;
	scene_is_display = false;
	scene_rt = nullptr;
	scene_ds = nullptr;
	vertex_offset = 0;
	index_offset = 0;
	gpu_vu_sequential_indices = nullptr;
	active_gpu_vu_draws = nullptr;
	active_gpu_vu_draw_encoded = false;
	active_gpu_vu_draw_failure_reason = nullptr;
#if defined(VITASX2_GPU_VU_UNIVERSAL_VALIDATION) && \
	VITASX2_GPU_VU_UNIVERSAL_VALIDATION
	gpu_vu_universal_validation_stage =
		UniversalGpuVuValidationStage::Uninitialized;
#endif
}

#endif
