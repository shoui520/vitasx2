// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "vita/GSDeviceGXM.h"

#if !defined(VITASX2_QEMU_VALIDATION)

#include "GS/GSRegs.h"
#include "GS/Renderers/Common/GSDevice.h"
#include "GS/Renderers/Common/GSVertex.h"
#include "common/Console.h"
#include "vita/VitaGpuVuDraw.h"
#include "vita/VitaGpuVuProgramRegistry.h"
#include "vita/VitaGpuVuShaderCompiler.h"
#include "vita/VitaGpuVuVifInput.h"
#include "vita/VitaGxmArena.h"
#include "vita/VitaGxmDisplay.h"
#include "vita/VitaGxmMemory.h"
#include "vita/VitaGxmTexture.h"
#include "vita/VitaPerformanceTelemetry.h"
#if defined(VITASX2_GS_DRAW_TRACE) && VITASX2_GS_DRAW_TRACE
#include "vita/VitaGsDrawTrace.h"
#endif

#include <psp2/gxm.h>

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
}

namespace
{
	VitaGxmPerformanceCounters s_gxm_worker_performance;
	std::atomic<u64> s_gxm_published_draw_calls{0};
	std::atomic<u64> s_gxm_published_draw_indices{0};
	std::atomic<u64> s_gxm_published_vertex_upload_bytes{0};
	std::atomic<u64> s_gxm_published_index_upload_bytes{0};
	std::atomic<u64> s_gxm_published_texture_uploads{0};
	std::atomic<u64> s_gxm_published_texture_upload_bytes{0};
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

	constexpr u32 PATCHER_BUFFER_BYTES = 512 * 1024;
	constexpr u32 PATCHER_VERTEX_USSE_BYTES = 256 * 1024;
	constexpr u32 PATCHER_FRAGMENT_USSE_BYTES = 2 * 1024 * 1024;
	constexpr u32 GEOMETRY_VERTEX_BYTES = 6 * 1024 * 1024;
	constexpr u32 GEOMETRY_INDEX_BYTES = 512 * 1024;
	constexpr u32 MAX_STAGED_INDICES = 65532;
	constexpr u32 GPU_VU_SEQUENTIAL_INDEX_COUNT = 65536;
	constexpr u32 GPU_VU_SEQUENTIAL_INDEX_BYTES =
		GPU_VU_SEQUENTIAL_INDEX_COUNT * sizeof(u16);
	constexpr u32 MAX_RENDER_TARGETS = 48;
	constexpr size_t MAX_TFX_PATCHED_PROGRAMS = 128;
	constexpr size_t GPU_VU_RETIREMENT_SLOT_COUNT = 4;
	// vitaGL's GXM owner records eight as libGXM's per-target maximum. Sony's
	// macrotile_sync and tutorial_postprocessing samples raise scenesPerFrame
	// for targets used by several ordered passes instead of leaving it at one.
	constexpr u16 MAX_GXM_SCENES_PER_RENDER_TARGET = 8;

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
		if (config.ps.adjs || config.ps.adjt)
			features |= GxmTfxRejectCoordinateAdjust;
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

void VitaGxmPublishPerformanceCounters()
{
	if (!VitaPerformanceTelemetry::IsEnabled())
		return;
	s_gxm_published_draw_calls.store(s_gxm_worker_performance.draw_calls,
		std::memory_order_relaxed);
	s_gxm_published_draw_indices.store(s_gxm_worker_performance.draw_indices,
		std::memory_order_relaxed);
	s_gxm_published_vertex_upload_bytes.store(
		s_gxm_worker_performance.vertex_upload_bytes, std::memory_order_relaxed);
	s_gxm_published_index_upload_bytes.store(
		s_gxm_worker_performance.index_upload_bytes, std::memory_order_relaxed);
	s_gxm_published_texture_uploads.store(s_gxm_worker_performance.texture_uploads,
		std::memory_order_relaxed);
	s_gxm_published_texture_upload_bytes.store(
		s_gxm_worker_performance.texture_upload_bytes, std::memory_order_relaxed);
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
	counters.rejected_tfx_draws =
		s_gxm_published_rejected_tfx_draws.load(std::memory_order_acquire);
	counters.draw_calls = s_gxm_published_draw_calls.load(std::memory_order_relaxed);
	counters.draw_indices = s_gxm_published_draw_indices.load(std::memory_order_relaxed);
	counters.vertex_upload_bytes =
		s_gxm_published_vertex_upload_bytes.load(std::memory_order_relaxed);
	counters.index_upload_bytes =
		s_gxm_published_index_upload_bytes.load(std::memory_order_relaxed);
	counters.texture_uploads =
		s_gxm_published_texture_uploads.load(std::memory_order_relaxed);
	counters.texture_upload_bytes =
		s_gxm_published_texture_upload_bytes.load(std::memory_order_relaxed);
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

static void RecordGpuVuGxmDraw(u64 indices)
{
	if (!VitaPerformanceTelemetry::IsEnabled())
		return;
	s_gxm_worker_performance.draw_calls++;
	s_gxm_worker_performance.draw_indices += indices;
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

struct GSDeviceGXM::Impl final : public VitaGXM::TextureOwner
{
	struct GeneratedVuProgram
	{
		struct Uniforms
		{
			const SceGxmProgramParameter* vertex_scale_offset = nullptr;
			const SceGxmProgramParameter* max_depth = nullptr;
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
		SceGxmShaderPatcherId id = nullptr;
		SceGxmVertexProgram* vertex_program = nullptr;
		SceGxmFragmentProgram* general_fragment_program = nullptr;
		SceGxmFragmentProgram* zfloor_fragment_program = nullptr;
		SceGxmFragmentProgram* opaque_fragment_program = nullptr;
		Uniforms uniforms;
	};

	struct GpuVuRetirementSlot
	{
		SceGxmNotification notification{};
		std::vector<std::unique_ptr<VitaGpuVu::GpuVuDraw>> draws;
		bool submitted = false;
	};

	struct RenderTargetEntry
	{
		u32 width = 0;
		u32 height = 0;
		u16 scenes_per_frame = 1;
		u16 scenes_this_frame = 0;
		u16 requested_scenes_per_frame = 1;
		bool upgrade_disabled = false;
		SceGxmRenderTarget* target = nullptr;
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
	VitaGXM::Display display;
	VitaGpuVu::ShaderCompiler gpu_vu_shader_compiler;
	VitaGpuVu::InputRing gpu_vu_input_ring;
	std::array<GpuVuRetirementSlot, GPU_VU_RETIREMENT_SLOT_COUNT>
		gpu_vu_retirement_slots;
	std::vector<std::unique_ptr<VitaGpuVu::GpuVuDraw>>
		gpu_vu_scene_draws;
	u32 next_gpu_vu_retirement_slot = 0;
	bool gpu_vu_retirements_ready = false;

	std::vector<RenderTargetEntry> render_targets;
	SceGxmRenderTarget* display_render_target = nullptr;
	VitaGXM::GSTextureGXM* scene_rt = nullptr;
	VitaGXM::GSTextureGXM* scene_ds = nullptr;
	GSVector4i scene_scissor{};
	u64 scene_serial = 0;
	u64 completed_scene_serial = 0;
	u64 transfer_serial = 0;
	u64 completed_transfer_serial = 0;
	u32 vertex_offset = 0;
	u32 index_offset = 0;
	const u16* gpu_vu_sequential_indices = nullptr;
	const VitaGpuVu::GpuVuDraw* active_gpu_vu_draw = nullptr;
	bool active_gpu_vu_draw_encoded = false;

	SceGxmShaderPatcherId tfx_vertex_id = nullptr;
	SceGxmShaderPatcherId tfx_fragment_id = nullptr;
	SceGxmShaderPatcherId tfx_manual_lod_fragment_id = nullptr;
	SceGxmShaderPatcherId tfx_manual_lod_zfloor_fragment_id = nullptr;
	SceGxmShaderPatcherId tfx_zfloor_fragment_id = nullptr;
	SceGxmShaderPatcherId tfx_psm16_fragment_id = nullptr;
	SceGxmShaderPatcherId tfx_psm16_zfloor_fragment_id = nullptr;
	SceGxmShaderPatcherId tfx_zfloor_source_direct_decal_af_fragment_id = nullptr;
	SceGxmShaderPatcherId tfx_fast_fragment_id = nullptr;
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
	bool PrepareGpuVuRetirementNotification(
		const SceGxmNotification** notification);
	void RetireCompletedGpuVuDraws();
	void ReleaseAllGpuVuDraws();
	bool CreateGeometry();
	bool CreateRenderTarget(u32 width, u32 height, u16 scenes_per_frame,
		SceGxmRenderTarget** target, bool required = true);
	SceGxmRenderTarget* GetRenderTarget(u32 width, u32 height);
	void RetuneRenderTargets();
	bool EndScene(bool finish);
	bool Finish();
	bool CommitClear(VitaGXM::GSTextureGXM& texture);
	bool DrawTargetClear(u32 color, bool write_color, float depth,
		bool write_depth);
	bool EnsureScene(VitaGXM::GSTextureGXM* rt, VitaGXM::GSTextureGXM* ds,
		const GSVector4i& scissor);
	bool BeginDisplayScene();
	void ConfigureRaster(u32 width, u32 height);
	bool ConfigureScissor(const GSVector4i& scissor, u32 width, u32 height,
		bool exact_depth);
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
		const VitaGpuVu::GpuVuDraw* gpu_vu_draw = nullptr);
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
	Console.Error("GXM GS: %s failed (%08x).", operation, static_cast<u32>(result));
	return false;
}

bool GSDeviceGXM::Impl::Reject(const char* reason)
{
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
		SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_NC_RW, PATCHER_BUFFER_BYTES,
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
	return (result >= 0 && patcher) ? true : Fail("sceGxmShaderPatcherCreate",
		result < 0 ? result : SCE_GXM_ERROR_INVALID_POINTER);
}

bool GSDeviceGXM::Impl::CreateGeometry()
{
	int result = VitaGXM::AllocateMappedBlock("VitaSX2 GXM staged vertices",
		SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_NC_RW, GEOMETRY_VERTEX_BYTES,
		SCE_GXM_MEMORY_ATTRIB_READ, &geometry_vertices);
	if (result < 0)
		return Fail("staged vertex allocation", result);
	result = VitaGXM::AllocateMappedBlock("VitaSX2 GXM staged indices",
		SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_NC_RW, GEOMETRY_INDEX_BYTES,
		SCE_GXM_MEMORY_ATTRIB_READ, &geometry_indices);
	if (result < 0)
		return Fail("staged index allocation", result);

	// Direct VU+TFX draws must not construct one CPU index per generated
	// vertex. Reserve the tail of the already mapped index block and build the
	// identity sequence once. GIF NLOOP is 15 bits, but keeping the complete
	// u16 domain also covers future coalesced direct batches.
	u16* const indices = reinterpret_cast<u16*>(
		static_cast<u8*>(geometry_indices.base) +
		geometry_indices.size - GPU_VU_SEQUENTIAL_INDEX_BYTES);
	for (u32 i = 0; i < GPU_VU_SEQUENTIAL_INDEX_COUNT; i++)
		indices[i] = static_cast<u16>(i);
	gpu_vu_sequential_indices = indices;
	return true;
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
			// Eight is libGXM's maximum, so a saturated entry has nothing
			// left to learn. This is the common hot geometry in multi-pass
			// workloads; keep its steady-state lookup identical to the old
			// dimension cache instead of accounting every scene forever.
			if (entry.scenes_per_frame ==
				MAX_GXM_SCENES_PER_RENDER_TARGET)
			{
				return entry.target;
			}
			entry.scenes_this_frame = std::min<u16>(
				MAX_GXM_SCENES_PER_RENDER_TARGET,
				static_cast<u16>(entry.scenes_this_frame + 1));
			if (!entry.upgrade_disabled)
			{
				while (entry.requested_scenes_per_frame <
						entry.scenes_this_frame &&
					entry.requested_scenes_per_frame <
						MAX_GXM_SCENES_PER_RENDER_TARGET)
				{
					entry.requested_scenes_per_frame *= 2;
				}
			}
			return entry.target;
		}
	}
	if (render_targets.size() >= MAX_RENDER_TARGETS)
	{
		Reject("more than 48 live GXM render-target geometries");
		return nullptr;
	}
	SceGxmRenderTarget* target = nullptr;
	if (!CreateRenderTarget(width, height, 1, &target))
		return nullptr;
	render_targets.push_back({width, height, 1, 1, 1, false, target});
	return target;
}

void GSDeviceGXM::Impl::RetuneRenderTargets()
{
	struct PendingUpgrade
	{
		RenderTargetEntry* entry;
		SceGxmRenderTarget* replacement;
	};
	std::array<PendingUpgrade, MAX_RENDER_TARGETS> pending{};
	size_t pending_count = 0;

	for (RenderTargetEntry& entry : render_targets)
	{
		entry.scenes_this_frame = 0;
		if (entry.upgrade_disabled ||
			entry.requested_scenes_per_frame <= entry.scenes_per_frame)
		{
			continue;
		}
		SceGxmRenderTarget* upgraded = nullptr;
		if (!CreateRenderTarget(entry.width, entry.height,
				entry.requested_scenes_per_frame, &upgraded, false))
		{
			entry.upgrade_disabled = true;
			continue;
		}
		pending[pending_count++] = {&entry, upgraded};
	}

	if (pending_count == 0)
		return;

	// Sony's libGXM teardown contract and vitaGL's render-target recycler both
	// finish the context before destroying a handle which may still be owned by
	// queued GPU work. Retuning is a one-time event for each geometry, so take
	// that bounded synchronization here rather than leaking old handles or
	// adding a per-frame wait.
	if (!Finish())
	{
		for (size_t i = 0; i < pending_count; i++)
			sceGxmDestroyRenderTarget(pending[i].replacement);
		return;
	}

	for (size_t i = 0; i < pending_count; i++)
	{
		PendingUpgrade& upgrade = pending[i];
		RenderTargetEntry& entry = *upgrade.entry;
		const int result = sceGxmDestroyRenderTarget(entry.target);
		if (result < 0)
		{
			Console.Warning(
				"GXM GS: retaining smaller per-frame scene capacity after old render-target destruction failed (%08x).",
				static_cast<u32>(result));
			sceGxmDestroyRenderTarget(upgrade.replacement);
			entry.upgrade_disabled = true;
			continue;
		}
		entry.target = upgrade.replacement;
		entry.scenes_per_frame = entry.requested_scenes_per_frame;
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
	if (!gpu_vu_shader_compiler.Start())
		Console.Warning("GPU-VU: asynchronous compiler service did not start.");
	else if (!VitaGpuVu::AttachGeneratedProgramCompiler(
		&gpu_vu_shader_compiler))
	{
		Console.Warning(
			"GPU-VU: generated-program registry already has another compiler owner.");
		gpu_vu_shader_compiler.Stop();
	}

	result = texture_arena.Initialize(VitaGXM::ArenaMemory::Cdram,
		"VitaSX2 GXM textures", 4 * 1024 * 1024, SCE_GXM_MEMORY_ATTRIB_RW);
	if (result < 0)
		return Fail("texture arena initialization", result);
	result = transfer_arena.Initialize(VitaGXM::ArenaMemory::MainNonCached,
		"VitaSX2 GXM transfers", 2 * 1024 * 1024, SCE_GXM_MEMORY_ATTRIB_RW);
	if (result < 0)
		return Fail("transfer arena initialization", result);

	if (!CreateContext() || !CreatePatcher() || !CreatePrograms() ||
		!CreateGeometry() ||
		!CreateRenderTarget(VitaGXM::Display::Width, VitaGXM::Display::Height, 1,
			&display_render_target))
	{
		return false;
	}
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
	ready = true;
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
		tfx_fast_f, tfx_region_repeat_f, tfx_region_repeat_fast_f,
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
			return Fail("sceGxmProgramCheck", result);
	}

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

bool GSDeviceGXM::Impl::RegisterGeneratedVuProgram(
	VitaGpuVu::CompileResult result,
	VitaGpuVu::GeneratedCgProgram metadata)
{
	const std::pair<u64, u64> map_key(result.key.high, result.key.low);
	if (generated_vu_programs.find(map_key) != generated_vu_programs.end())
	{
		VitaGpuVu::CompleteGeneratedProgramRegistration(result.key, true);
		return true;
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
			"GPU-VU: generated shader compilation failed (%016llx%016llx).",
			static_cast<unsigned long long>(result.key.high),
			static_cast<unsigned long long>(result.key.low));
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
	const auto fail_registration = [this, &stored, &it, &result](
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
		VitaGpuVu::CompleteGeneratedProgramRegistration(result.key, false);
		if (may_erase)
			generated_vu_programs.erase(it);
		return false;
	};

	int check_result = sceGxmProgramCheck(program);
	if (check_result < 0)
	{
		return fail_registration("check generated VU1+TFX vertex program",
			check_result);
	}
	if (stored.metadata.memory_inputs.size() > 16)
	{
		return fail_registration("bind generated VU1+TFX vertex inputs",
			SCE_GXM_ERROR_INVALID_VALUE);
	}
	if (!stored.metadata.uses_tfx_uniforms)
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
	if (!stored.uniforms.vertex_scale_offset || !stored.uniforms.max_depth)
	{
		return fail_registration("find generated VU1+TFX state uniforms",
			SCE_GXM_ERROR_INVALID_VALUE);
	}
	for (u32 reg = 1; reg < stored.uniforms.vf.size(); reg++)
	{
		if ((stored.metadata.vf_uniform_mask & (1u << reg)) == 0)
			continue;
		char name[8];
		std::snprintf(name, sizeof(name), "VF%02u", reg);
		stored.uniforms.vf[reg] = find_uniform(name);
		if (!stored.uniforms.vf[reg])
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
	if (!require_uniform(stored.metadata.uses_acc_uniform, "ACC",
			&stored.uniforms.acc) ||
		!require_uniform(stored.metadata.uses_q_uniform, "Q",
			&stored.uniforms.q) ||
		!require_uniform(stored.metadata.uses_p_uniform, "P",
			&stored.uniforms.p) ||
		!require_uniform(stored.metadata.uses_i_uniform, "I",
			&stored.uniforms.i) ||
		!require_uniform(stored.metadata.uses_gif_q_uniform, "GifQ",
			&stored.uniforms.gif_q))
	{
		return fail_registration("find generated VU1+TFX scalar uniform",
			SCE_GXM_ERROR_INVALID_VALUE);
	}

	std::vector<SceGxmVertexAttribute> attributes(
		stored.metadata.memory_inputs.size());
	std::vector<SceGxmVertexStream> streams(
		stored.metadata.memory_inputs.size());
	for (u32 index = 0; index < stored.metadata.memory_inputs.size(); index++)
	{
		const VitaGpuVu::CgMemoryInput& input =
			stored.metadata.memory_inputs[index];
		if (input.attribute_index != index ||
			input.address.invocation_coefficient < 0)
		{
			return fail_registration("validate generated VU1+TFX input layout",
				SCE_GXM_ERROR_INVALID_VALUE);
		}
		const u64 stride =
			static_cast<u64>(input.address.invocation_coefficient) * 16u;
		if (stride > std::numeric_limits<u16>::max())
		{
			return fail_registration("validate generated VU1+TFX input stride",
				SCE_GXM_ERROR_INVALID_VALUE);
		}

		char parameter_name[24];
		std::snprintf(parameter_name, sizeof(parameter_name), "VuMemory%u",
			input.attribute_index);
		const SceGxmProgramParameter* const parameter =
			sceGxmProgramFindParameterByName(program, parameter_name);
		if (!parameter ||
			sceGxmProgramParameterGetCategory(parameter) !=
				SCE_GXM_PARAMETER_CATEGORY_ATTRIBUTE)
		{
			return fail_registration("find generated VU1+TFX input",
				SCE_GXM_ERROR_INVALID_VALUE);
		}

		SceGxmVertexAttribute& attribute = attributes[index];
		attribute.streamIndex = static_cast<u16>(index);
		attribute.offset = 0;
		// Shader_Compiler-Users_Guide::Offline Vertex Unpacking: a
		// __regformat int4 attribute is four untyped 32-bit words.
		attribute.format = SCE_GXM_ATTRIBUTE_FORMAT_UNTYPED;
		attribute.componentCount = 4;
		attribute.regIndex =
			sceGxmProgramParameterGetResourceIndex(parameter);

		SceGxmVertexStream& stream = streams[index];
		stream.stride = static_cast<u16>(stride);
		stream.indexSource = SCE_GXM_INDEX_SOURCE_INDEX_16BIT;
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
	patch_result = sceGxmShaderPatcherCreateFragmentProgram(patcher,
		tfx_fragment_id, SCE_GXM_OUTPUT_REGISTER_FORMAT_DECLARED,
		SCE_GXM_MULTISAMPLE_NONE, nullptr, program,
		&stored.general_fragment_program);
	if (patch_result < 0 || !stored.general_fragment_program)
	{
		return fail_registration(
			"create generated VU1+TFX general fragment program",
			patch_result < 0 ? patch_result : SCE_GXM_ERROR_INVALID_POINTER);
	}
	patch_result = sceGxmShaderPatcherCreateFragmentProgram(patcher,
		tfx_zfloor_fragment_id, SCE_GXM_OUTPUT_REGISTER_FORMAT_DECLARED,
		SCE_GXM_MULTISAMPLE_NONE, nullptr, program,
		&stored.zfloor_fragment_program);
	if (patch_result < 0 || !stored.zfloor_fragment_program)
	{
		return fail_registration(
			"create generated VU1+TFX Z-floor fragment program",
			patch_result < 0 ? patch_result : SCE_GXM_ERROR_INVALID_POINTER);
	}
	patch_result = sceGxmShaderPatcherCreateFragmentProgram(patcher,
		tfx_fast_fragment_id, SCE_GXM_OUTPUT_REGISTER_FORMAT_DECLARED,
		SCE_GXM_MULTISAMPLE_NONE, nullptr, program,
		&stored.opaque_fragment_program);
	if (patch_result < 0 || !stored.opaque_fragment_program)
	{
		return fail_registration(
			"create generated VU1+TFX opaque fragment program",
			patch_result < 0 ? patch_result : SCE_GXM_ERROR_INVALID_POINTER);
	}

	VitaGpuVu::CompleteGeneratedProgramRegistration(result.key, true);
	Console.WriteLn(
		"GPU-VU: GS registered generated VU1+TFX program %016llx%016llx "
		"(%u raw streams, %u expressions, general+Z-floor+opaque TFX links).",
		static_cast<unsigned long long>(result.key.high),
		static_cast<unsigned long long>(result.key.low),
		static_cast<u32>(stored.metadata.memory_inputs.size()),
		stored.metadata.emitted_expression_count);
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

	// No other VitaSX2 subsystem consumes notification words. Sony's
	// precomputation and instancing samples allocate linearly from this region;
	// reserve four distinct words so a submitted value is never overwritten
	// before its slot is explicitly reused.
	for (u32 i = 0; i < gpu_vu_retirement_slots.size(); i++)
	{
		GpuVuRetirementSlot& slot = gpu_vu_retirement_slots[i];
		slot.notification.address = notification_region + i;
		slot.notification.value = 0;
		*slot.notification.address = 0;
		slot.draws.clear();
		slot.submitted = false;
	}
	next_gpu_vu_retirement_slot = 0;
	gpu_vu_retirements_ready = true;
	return true;
}

bool GSDeviceGXM::Impl::RetainGpuVuDrawForScene(
	std::unique_ptr<VitaGpuVu::GpuVuDraw> draw)
{
	if (!draw || !scene_active || !gpu_vu_retirements_ready)
		return false;
	VitaGpuVu::RecordGpuVuDrawExecuted(*draw);
	gpu_vu_scene_draws.push_back(std::move(draw));
	return true;
}

void GSDeviceGXM::Impl::RetireCompletedGpuVuDraws()
{
	if (!gpu_vu_retirements_ready)
		return;
	for (GpuVuRetirementSlot& slot : gpu_vu_retirement_slots)
	{
		if (!slot.submitted ||
			!GpuVuNotificationReached(slot.notification))
		{
			continue;
		}
		const u64 count = slot.draws.size();
		slot.draws.clear();
		slot.submitted = false;
		if (count != 0)
			VitaGpuVu::RecordGpuVuDrawsRetired(count);
	}
}

bool GSDeviceGXM::Impl::PrepareGpuVuRetirementNotification(
	const SceGxmNotification** notification)
{
	if (!notification)
		return false;
	*notification = nullptr;
	if (gpu_vu_scene_draws.empty())
		return true;
	if (!gpu_vu_retirements_ready)
		return false;

	RetireCompletedGpuVuDraws();
	GpuVuRetirementSlot& slot =
		gpu_vu_retirement_slots[next_gpu_vu_retirement_slot];
	if (slot.submitted)
	{
		// This is the only routine GPU-VU notification wait: four newer scene
		// batches have already consumed the other slots and this older slot is
		// now being reused. Never wait when the completion word is already
		// visible.
		VitaGpuVu::RecordGpuVuRetirementRingWait();
		if (!GpuVuNotificationReached(slot.notification))
		{
			VitaGpuVu::RecordGpuVuNotificationWait();
			const int wait_result =
				sceGxmNotificationWait(&slot.notification);
			if (wait_result < 0)
				return Fail("wait for GPU-VU retirement slot", wait_result);
		}
		const u64 count = slot.draws.size();
		slot.draws.clear();
		slot.submitted = false;
		if (count != 0)
			VitaGpuVu::RecordGpuVuDrawsRetired(count);
	}

	u32 value = slot.notification.value + 1;
	if (value == 0)
		value = 1;
	slot.notification.value = value;
	slot.draws = std::move(gpu_vu_scene_draws);
	gpu_vu_scene_draws.clear();
	slot.submitted = true;
	next_gpu_vu_retirement_slot =
		(next_gpu_vu_retirement_slot + 1) %
		gpu_vu_retirement_slots.size();
	*notification = &slot.notification;
	return true;
}

void GSDeviceGXM::Impl::ReleaseAllGpuVuDraws()
{
	u64 count = gpu_vu_scene_draws.size();
	gpu_vu_scene_draws.clear();
	for (GpuVuRetirementSlot& slot : gpu_vu_retirement_slots)
	{
		count += slot.draws.size();
		slot.draws.clear();
		slot.submitted = false;
	}
	if (count != 0)
		VitaGpuVu::RecordGpuVuDrawsRetired(count);
}

bool GSDeviceGXM::Impl::EndScene(bool finish)
{
	if (!scene_active)
		return true;
	const SceGxmNotification* vertex_notification = nullptr;
	if (!PrepareGpuVuRetirementNotification(&vertex_notification))
		return false;
	const int end_result =
		sceGxmEndScene(context, vertex_notification, nullptr);
	if (end_result < 0)
	{
		ready = false;
		return Fail("sceGxmEndScene", end_result);
	}
	scene_active = false;
	scene_is_display = false;
	scene_rt = nullptr;
	scene_ds = nullptr;
	if (vertex_notification)
		VitaGpuVu::RecordGpuVuRetirementBatch();
	completed_scene_serial = finish ? scene_serial : completed_scene_serial;
	if (!finish)
		return true;
	sceGxmFinish(context);
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
	sceGxmFinish(context);
	ReleaseAllGpuVuDraws();
	completed_scene_serial = scene_serial;
	completed_transfer_serial = transfer_serial;
	vertex_offset = 0;
	index_offset = 0;
	return true;
}

void GSDeviceGXM::Impl::ConfigureRaster(u32 width, u32 height)
{
	sceGxmSetViewport(context, static_cast<float>(width) * 0.5f,
		static_cast<float>(width) * 0.5f, static_cast<float>(height) * 0.5f,
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

bool GSDeviceGXM::Impl::DrawTargetClear(u32 color, bool write_color,
	float depth, bool write_depth)
{
	if (!scene_active || write_color != (scene_rt != nullptr) ||
		write_depth != (scene_ds != nullptr))
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
	const int result = sceGxmDraw(context, SCE_GXM_PRIMITIVE_TRIANGLES,
		SCE_GXM_INDEX_FORMAT_U16, indices, 3);
	if (result < 0)
		return Fail("sceGxmDraw(lazy clear)", result);
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
	if ((rt && !CommitClear(*rt)) || (ds && !CommitClear(*ds)))
		return false;
	const GSVector4i bounds(0, 0, size.x, size.y);
	const GSVector4i clipped_scissor = scissor.rintersect(bounds);
	if (clipped_scissor.rempty())
		return Reject("empty hardware draw scissor");

	if (scene_active && !scene_is_display && scene_rt == rt && scene_ds == ds &&
		scene_scissor.eq(clipped_scissor))
	{
		return true;
	}
	// Exact depth scissoring emits two mask rectangles while establishing the
	// new scene. Retire the staging arena before BeginScene when those quads
	// would cross its end; no caller has staged this draw yet.
	if (ds && !HasGeometryCapacity(8, 12) && !Finish())
		return false;
	if (!EndScene(false))
		return false;
	SceGxmRenderTarget* target = GetRenderTarget(size.x, size.y);
	if (!target)
		return false;
	SceGxmColorSurface* color = rt ? rt->ColorSurface() : nullptr;
	SceGxmDepthStencilSurface* depth = ds ? ds->DepthStencilSurface() :
		&disabled_depth;
	if ((rt && !color) || (ds && !ds->DepthStencilSurface()))
		return Reject("texture lacks required GXM render surface");
	// Official libGXM contract: sceGxmBeginScene() copies the color-surface
	// descriptor. Program its exact pixel clip before the copy; changing the
	// descriptor during a scene affects only a later scene.
	if (color)
	{
		sceGxmColorSurfaceSetClip(color, clipped_scissor.x, clipped_scissor.y,
			clipped_scissor.z - 1, clipped_scissor.w - 1);
	}
	const u64 reserved_serial = ++scene_serial;
	const int result = sceGxmBeginScene(context, 0, target, nullptr, nullptr,
		nullptr, color, depth);
	if (result < 0)
		return Fail("sceGxmBeginScene(GS target)", result);
	scene_active = true;
	scene_is_display = false;
	scene_rt = rt;
	scene_ds = ds;
	scene_scissor = {};
	if (rt)
		rt->MarkSceneUse(reserved_serial);
	if (ds)
		ds->MarkSceneUse(reserved_serial);
	ConfigureRaster(size.x, size.y);
	return ConfigureScissor(clipped_scissor, size.x, size.y, ds != nullptr);
}

bool GSDeviceGXM::Impl::BeginDisplayScene()
{
	if (!EndScene(false))
		return false;
	const u64 reserved_serial = ++scene_serial;
	const int result = sceGxmBeginScene(context, 0, display_render_target,
		nullptr, nullptr, display.BackSyncObject(), display.BackColorSurface(),
		&disabled_depth);
	if (result < 0)
		return Fail("sceGxmBeginScene(display)", result);
	scene_active = true;
	scene_is_display = true;
	(void)reserved_serial;
	ConfigureRaster(VitaGXM::Display::Width, VitaGXM::Display::Height);
	sceGxmSetRegionClip(context, SCE_GXM_REGION_CLIP_OUTSIDE, 0, 0,
		VitaGXM::Display::Width - 1, VitaGXM::Display::Height - 1);
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
	return aligned_vertex_offset + vertex_bytes <= geometry_vertices.size &&
		aligned_index_offset + index_bytes <=
			geometry_indices.size - GPU_VU_SEQUENTIAL_INDEX_BYTES;
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
	const int result = sceGxmDraw(context, SCE_GXM_PRIMITIVE_TRIANGLES,
		SCE_GXM_INDEX_FORMAT_U16, indices, quad.size());
	if (result < 0)
		return Fail("sceGxmDraw(scissor mask)", result);
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
		// the GS scissor.
		sceGxmSetRegionClip(context, SCE_GXM_REGION_CLIP_OUTSIDE, 0, 0,
			width - 1, height - 1);
		if (!DrawMaskRect(bounds, width, height, SCE_GXM_STENCIL_FUNC_NEVER) ||
			!DrawMaskRect(scissor, width, height, SCE_GXM_STENCIL_FUNC_ALWAYS))
		{
			return false;
		}
		sceGxmSetFrontStencilFunc(context, SCE_GXM_STENCIL_FUNC_ALWAYS,
			SCE_GXM_STENCIL_OP_KEEP, SCE_GXM_STENCIL_OP_KEEP,
			SCE_GXM_STENCIL_OP_KEEP, 0xff, 0xff);
		sceGxmSetBackStencilFunc(context, SCE_GXM_STENCIL_FUNC_ALWAYS,
			SCE_GXM_STENCIL_OP_KEEP, SCE_GXM_STENCIL_OP_KEEP,
			SCE_GXM_STENCIL_OP_KEEP, 0xff, 0xff);
	}
	sceGxmSetRegionClip(context, SCE_GXM_REGION_CLIP_OUTSIDE, scissor.x,
		scissor.y, scissor.z - 1, scissor.w - 1);
	scene_scissor = scissor;
	return true;
}

bool GSDeviceGXM::Impl::RetireTextureScene(VitaGXM::GSTextureGXM& texture,
	bool preserve_contents, u64* serial)
{
	(void)texture;
	(void)preserve_contents;
	if (!serial || !Finish())
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
		!config.ps.region_rect && config.ps.wms <= 1 && config.ps.wmt <= 1;
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
	const VitaGpuVu::GpuVuDraw* gpu_vu_draw)
{
	const ProgramUniforms* fragment_uniforms = &uniforms;
	if (region_repeat_fragment)
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
		fragment_uniforms = &fast_uniforms;
	void* vertex_buffer = nullptr;
	int result = sceGxmReserveVertexDefaultUniformBuffer(context, &vertex_buffer);
	if (result < 0 || !vertex_buffer)
		return Fail("reserve TFX vertex uniforms",
			result < 0 ? result : SCE_GXM_ERROR_INVALID_POINTER);
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
		if (!upload_vertex(generated_vu->uniforms.vertex_scale_offset, 12,
				gpu_vu_draw->vertex_scale_offset[0].data(),
				"upload generated VU1+TFX transform") ||
			!upload_vertex(generated_vu->uniforms.max_depth, 1,
				&gpu_vu_draw->max_depth,
				"upload generated VU1+TFX maximum depth"))
		{
			return false;
		}
		for (const VitaGpuVu::VectorUniform& uniform :
			gpu_vu_draw->vf_uniforms)
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
	void* fragment_buffer = nullptr;
	result = sceGxmReserveFragmentDefaultUniformBuffer(context, &fragment_buffer);
	if (result < 0 || !fragment_buffer)
		return Fail("reserve TFX fragment uniforms",
			result < 0 ? result : SCE_GXM_ERROR_INVALID_POINTER);
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
	const float selectors[7][4] = {
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
	if (!upload4(fragment_uniforms->fog_color_aref,
			config.cb_ps.FogColor_AREF.F32,
			"upload FogColor_AREF") ||
		!upload4(fragment_uniforms->texture_size, config.cb_ps.WH.F32,
			"upload texture size") ||
		!upload4(fragment_uniforms->texture_alpha,
			config.cb_ps.TA_MaxDepth_Af.F32,
			"upload texture alpha") ||
		!upload4(fragment_uniforms->half_texel, config.cb_ps.HalfTexel.F32,
			"upload half texel") ||
		!upload4(fragment_uniforms->st_range, config.cb_ps.STRange.F32,
			"upload ST range"))
	{
		return false;
	}
	const float st_scale[4] = {config.cb_ps.STScale.x, config.cb_ps.STScale.y,
		0.0f, 0.0f};
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
			source_direct_modulate_af_fragment, untextured_fragment))
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
	result = sceGxmDraw(context, TranslateTopology(config.topology),
		SCE_GXM_INDEX_FORMAT_U16, staged_indices, index_count);
	if (point_topology || line_topology)
	{
		sceGxmSetFrontPolygonMode(context, SCE_GXM_POLYGON_MODE_TRIANGLE_FILL);
		sceGxmSetBackPolygonMode(context, SCE_GXM_POLYGON_MODE_TRIANGLE_FILL);
	}
	if (result < 0)
		return Fail("sceGxmDraw(TFX)", result);
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
	const VitaGpuVu::GpuVuDraw* const draw = active_gpu_vu_draw;
	if (!draw || active_gpu_vu_draw_encoded)
		return Reject("missing or already encoded GPU-VU draw descriptor");
	std::string validation_error;
	if (!draw->Validate(&validation_error))
		return Reject(validation_error.c_str());
	if (draw->lowering != VitaGpuVu::OutputLowering::DirectTfx ||
		draw->execution != VitaGpuVu::ExecutionKind::GeneratedParallel ||
		draw->primitive_boundary != VitaGpuVu::PrimitiveBoundary::Native)
	{
		return Reject("GPU-VU draw is not a native generated direct-TFX job");
	}
	if (draw->final_state.IsRequired())
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

	GeneratedVuProgram* const generated =
		FindGeneratedVuProgram(draw->program);
	if (!generated || !generated->vertex_program ||
		!generated->general_fragment_program ||
		!generated->zfloor_fragment_program ||
		!generated->opaque_fragment_program)
	{
		return Reject("generated VU1+TFX program is not GS-ready");
	}
	if (!generated->metadata.uses_tfx_uniforms ||
		generated->metadata.memory_inputs.size() != draw->streams.size())
	{
		return Reject("generated VU1 program metadata differs from draw streams");
	}

	u32 vf_mask = 0;
	for (const VitaGpuVu::VectorUniform& uniform : draw->vf_uniforms)
		vf_mask |= 1u << uniform.register_index;
	if (vf_mask != generated->metadata.vf_uniform_mask)
		return Reject("generated VU1 VF-uniform mask mismatch");
	const u32 expected_scalar_mask =
		(generated->metadata.uses_q_uniform ?
			VitaGpuVu::ScalarUniformQ : 0u) |
		(generated->metadata.uses_p_uniform ?
			VitaGpuVu::ScalarUniformP : 0u) |
		(generated->metadata.uses_i_uniform ?
			VitaGpuVu::ScalarUniformI : 0u) |
		(generated->metadata.uses_gif_q_uniform ?
			VitaGpuVu::ScalarUniformGifQ : 0u);
	if (draw->scalar_uniforms.present != expected_scalar_mask)
		return Reject("generated VU1 scalar-uniform mask mismatch");

	for (u32 index = 0; index < draw->streams.size(); index++)
	{
		const VitaGpuVu::StreamBinding& binding = draw->streams[index];
		const VitaGpuVu::CgMemoryInput& input =
			generated->metadata.memory_inputs[index];
		const u64 expected_stride =
			static_cast<u64>(input.address.invocation_coefficient) * 16u;
		if (binding.attribute_index != index ||
			input.attribute_index != index ||
			expected_stride != binding.byte_stride ||
			(binding.payload_byte_offset & 3u) != 0)
		{
			return Reject("generated VU1 stream binding differs from GXP layout");
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
	if (!draw->direct_tfx.gouraud && prim.PRIM != GS_POINTLIST)
	{
		// PCSX2's GS uses the last vertex as the provoking vertex. Public GXM
		// exposes neither a provoking-vertex selector nor flat interpolation;
		// the exact instance-indexed lowering remains a separate boundary.
		return Reject("native GPU-VU draw cannot preserve flat provoking color");
	}

	SceGxmPrimitiveType primitive_type{};
	GSHWDrawConfig::Topology topology{};
	u32 indices_per_primitive = 0;
	u32 primitive_count = 0;
	switch (prim.PRIM)
	{
		case GS_POINTLIST:
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
			break;
		case GS_TRIANGLELIST:
			if ((draw->vertex_count % 3u) != 0)
				return Reject("non-integral native GPU-VU triangle-list count");
			primitive_type = SCE_GXM_PRIMITIVE_TRIANGLES;
			topology = GSHWDrawConfig::Topology::Triangle;
			indices_per_primitive = 3;
			primitive_count = draw->vertex_count / 3;
			break;
		case GS_TRIANGLESTRIP:
			if (draw->vertex_count < 3)
				return Reject("short native GPU-VU triangle strip");
			primitive_type = SCE_GXM_PRIMITIVE_TRIANGLE_STRIP;
			topology = GSHWDrawConfig::Topology::Triangle;
			indices_per_primitive = 3;
			primitive_count = draw->vertex_count - 2;
			break;
		case GS_TRIANGLEFAN:
			if (draw->vertex_count < 3)
				return Reject("short native GPU-VU triangle fan");
			primitive_type = SCE_GXM_PRIMITIVE_TRIANGLE_FAN;
			topology = GSHWDrawConfig::Topology::Triangle;
			indices_per_primitive = 3;
			primitive_count = draw->vertex_count - 2;
			break;
		case GS_LINESTRIP:
		case GS_SPRITE:
		default:
			return Reject("GPU-VU primitive requires a non-native output lowering");
	}
	if (draw->index_count != draw->vertex_count ||
		draw->index_count > GPU_VU_SEQUENTIAL_INDEX_COUNT ||
		draw->primitive_count != primitive_count ||
		config.topology != topology ||
		config.indices_per_prim != indices_per_primitive ||
		config.nverts != draw->vertex_count ||
		config.nindices != draw->index_count)
	{
		return Reject("GPU-VU geometry dimensions differ from PCSX2 draw state");
	}

	const std::array<float, 12> expected_transform = {
		config.cb_vs.vertex_scale.x, config.cb_vs.vertex_scale.y,
		config.cb_vs.vertex_offset.x, config.cb_vs.vertex_offset.y,
		config.cb_vs.texture_scale.x, config.cb_vs.texture_scale.y,
		config.cb_vs.texture_offset.x, config.cb_vs.texture_offset.y,
		config.cb_vs.point_size.x, config.cb_vs.point_size.y, 0.0f, 0.0f};
	if (std::memcmp(draw->vertex_scale_offset[0].data(),
			expected_transform.data(), sizeof(expected_transform)) != 0 ||
		draw->max_depth != static_cast<float>(config.cb_vs.max_depth))
	{
		return Reject("GPU-VU transform uniforms differ from PCSX2 draw state");
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

	sceGxmSetVertexProgram(context, generated->vertex_program);
	sceGxmSetFragmentProgram(context, config.ps.zfloor ?
		generated->zfloor_fragment_program : (fast_fragment ?
			generated->opaque_fragment_program :
			generated->general_fragment_program));
	for (u32 index = 0; index < draw->streams.size(); index++)
	{
		const VitaGpuVu::StreamBinding& binding = draw->streams[index];
		const VitaGpuVu::VifUnpackSpan& span =
			draw->InputSpans()[binding.input_span];
		const u8* const payload = VitaGpuVu::ResolveRawVifPayload(span.payload);
		if (!payload)
			return Reject("GPU-VU VIF input span retired before draw");
		const int stream_result = sceGxmSetVertexStream(context, index,
			payload + binding.payload_byte_offset);
		if (stream_result < 0)
			return Fail("bind generated VU1 raw VIF stream", stream_result);
	}

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
	result = sceGxmSetFragmentTexture(context, 0, &native_texture);
	if (result < 0)
		return Fail("bind generated VU1+TFX source texture", result);
	bound_source->MarkSceneUse(scene_serial);

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
	result = sceGxmDraw(context, primitive_type, SCE_GXM_INDEX_FORMAT_U16,
		gpu_vu_sequential_indices, draw->index_count);
	if (point_topology || line_topology)
	{
		sceGxmSetFrontPolygonMode(context, SCE_GXM_POLYGON_MODE_TRIANGLE_FILL);
		sceGxmSetBackPolygonMode(context, SCE_GXM_POLYGON_MODE_TRIANGLE_FILL);
	}
	if (result < 0)
		return Fail("sceGxmDraw(generated VU1+TFX)", result);

	RecordGpuVuGxmDraw(draw->index_count);
	if (rt)
		rt->SetState(GSTexture::State::Dirty);
	if (ds && config.depth.zwe)
		ds->SetState(GSTexture::State::Dirty);
	active_gpu_vu_draw_encoded = true;
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
	const int result = sceGxmDraw(context, SCE_GXM_PRIMITIVE_TRIANGLES,
		SCE_GXM_INDEX_FORMAT_U16, indices, quad.size());
	if (result < 0)
		return Fail("sceGxmDraw(presentation quad)", result);
	RecordGxmDraw(4, sizeof(QuadVertex), quad.size());
	return true;
}

void GSDeviceGXM::PollGpuVuPrograms()
{
	if (m_impl && m_impl->ready)
		m_impl->PollGeneratedVuPrograms();
}

bool GSDeviceGXM::RenderGpuVuDraw(GSHWDrawConfig& config,
	std::unique_ptr<VitaGpuVu::GpuVuDraw> draw)
{
	if (!m_impl || !m_impl->ready || !draw ||
		m_impl->active_gpu_vu_draw)
	{
		return false;
	}
	std::string validation_error;
	if (!draw->Validate(&validation_error))
		return m_impl->Reject(validation_error.c_str());

	m_impl->active_gpu_vu_draw = draw.get();
	m_impl->active_gpu_vu_draw_encoded = false;
	RenderHW(config);
	const bool encoded = m_impl->active_gpu_vu_draw_encoded;
	m_impl->active_gpu_vu_draw = nullptr;
	m_impl->active_gpu_vu_draw_encoded = false;
	if (!encoded)
		return false;
	if (m_impl->RetainGpuVuDrawForScene(std::move(draw)))
		return true;

	// This is an exceptional ownership failure after commands were encoded.
	// Drain before releasing immutable VIF spans; the normal path retains them
	// behind a four-scene vertex notification and never reaches this branch.
	m_impl->Finish();
	return false;
}

bool GSDeviceGXM::RetainGpuVuDrawForVertexCompletion(
	std::unique_ptr<VitaGpuVu::GpuVuDraw> draw)
{
	return m_impl && m_impl->ready &&
		m_impl->RetainGpuVuDrawForScene(std::move(draw));
}

void GSDeviceGXM::RenderHW(GSHWDrawConfig& config)
{
	#if defined(VITASX2_GS_DRAW_TRACE) && VITASX2_GS_DRAW_TRACE
	VitaGsDrawTraceRecordDeviceOutcome(config,
		VitaGsDrawTraceOutcome::DeviceEntered);
	#endif
	if (!m_impl || !m_impl->ready)
		return;
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
		config.ps.tcoffsethack || config.ps.adjs || config.ps.adjt ||
		config.ps.tex_is_fb || config.ps.date || config.ps.abe ||
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
	bool source_only_fragment = !m_impl->active_gpu_vu_draw &&
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
	if (psm16_fragment && !m_impl->tfx_psm16_logged)
	{
		m_impl->tfx_psm16_logged = true;
		Console.WriteLn(
			"GXM GS: PCSX2 PSMCT16 dither/quantization path active.");
	}
	if (m_impl->active_gpu_vu_draw)
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
		return;
	}
	const u8* src_base = static_cast<const u8*>(source->LevelData(0)) +
		static_cast<size_t>(requested.y) * src_layout->pitch +
		static_cast<size_t>(requested.x) * bpp;
	u8* dst_base = static_cast<u8*>(destination->LevelData(0)) +
		static_cast<size_t>(destination_y) * dst_layout->pitch +
		static_cast<size_t>(destination_x) * bpp;
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
	int result = sceGxmPadHeartbeat(m_impl->display.BackColorSurface(),
		m_impl->display.BackSyncObject());
	if (result < 0)
	{
		m_impl->Fail("sceGxmPadHeartbeat", result);
		return;
	}
	result = m_impl->display.QueuePresent();
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
	if (VitaPerformanceTelemetry::IsEnabled())
		s_gxm_worker_performance.present_calls++;
	if (!m_impl->DrawQuad(source, source_rect, destination_rect, 0xffffffffu,
			filter, m_impl->copy_programs[0xf]))
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
		Console.Error(
			"GXM GS: rejected StretchRect conversion: shader=%s mask=%u src_format=%u dst_format=%u src=(%.3f,%.3f,%.3f,%.3f) dst=(%.1f,%.1f,%.1f,%.1f).",
			shader.Name(), shader.Mask(),
			static_cast<u32>(source_texture->GetFormat()),
			static_cast<u32>(destination_texture->GetFormat()),
			source_rect.x, source_rect.y, source_rect.z, source_rect.w,
			destination_rect.x, destination_rect.y,
			destination_rect.z, destination_rect.w);
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
		!m_impl->EnsureScene(destination, nullptr, destination->GetRect()) ||
		!m_impl->DrawQuad(source, source_rect, destination_rect, 0xffffffffu,
			filter, fragment))
	{
		m_impl->Reject("failed copy StretchRect");
		return;
	}
	destination->SetState(GSTexture::State::Dirty);
	destination->RecordWriter(VitaGXM::TextureWriterKind::Copy, source);
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
	if ((source_2 && !m_impl->CommitClear(*source_2)) ||
		(source_1 && !m_impl->CommitClear(*source_1)))
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
		!m_impl->EnsureScene(destination, nullptr, destination->GetRect()) ||
		!m_impl->DrawQuad(source, source_rect, destination_rect, 0xffffffffu,
			filter, program, uniform, cb.ZrH.F32))
	{
		m_impl->Reject("failed FastMAD interlace pass");
		return;
	}
	destination->SetState(GSTexture::State::Dirty);
	destination->RecordWriter(VitaGXM::TextureWriterKind::Interlace, source);
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
	if (scene_active && context && !EndScene(false))
		return;
	if (context)
	{
		sceGxmFinish(context);
		completed_scene_serial = scene_serial;
		completed_transfer_serial = transfer_serial;
	}
	ReleaseAllGpuVuDraws();
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
	if (!group_ok)
		return;
	generated_vu_programs.clear();
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
	unregister(tfx_fast_fragment_id, "unregister fast TFX fragment");
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
		if (entry.target &&
			!succeeded("destroy cached render target",
				sceGxmDestroyRenderTarget(entry.target)))
		{
			group_ok = false;
		}
		else
		{
			entry.target = nullptr;
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
		slot.notification = {};
	scene_active = false;
	scene_is_display = false;
	scene_rt = nullptr;
	scene_ds = nullptr;
	vertex_offset = 0;
	index_offset = 0;
	gpu_vu_sequential_indices = nullptr;
	active_gpu_vu_draw = nullptr;
	active_gpu_vu_draw_encoded = false;
}

#endif
