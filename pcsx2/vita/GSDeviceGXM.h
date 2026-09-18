// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/Common/GSDevice.h"

#include <memory>
#include <array>
#include <vector>

namespace VitaGpuVu
{
	class GpuVuDraw;
	class UniversalGpuVuEpoch;
	struct RawVifPayloadRef;
}

// Host call residence, not GPU execution time. Ordinary GS submission only;
// generated GPU-VU calls have their own telemetry. All owners are on MTGS.
enum class VitaGxmHostCall : size_t
{
	BeginTarget,
	BeginDisplay,
	EndScene,
	Finish,
	DrawTfx,
	DrawQuad,
	DrawClear,
	DrawScissor,
	Heartbeat,
	QueuePresent,
	Count
};

inline constexpr std::array<const char*, static_cast<size_t>(VitaGxmHostCall::Count)>
	VitaGxmHostCallNames = {"begin_target", "begin_display", "end_scene", "finish",
		"draw_tfx", "draw_quad", "draw_clear", "draw_scissor", "heartbeat", "queue_present"};

struct VitaGxmHostCallCounters
{
	u64 calls = 0;
	u64 wall_us = 0;
	u64 max_lifetime_us = 0;
};

// CPU2 residence inside PCSX2's host-independent hardware-renderer analysis.
// These stages deliberately exclude GXM submission, which VitaGxmHostCall owns.
enum class VitaGxmRendererStage : size_t
{
	TextureSourceLookup,
	TextureDepthSourceLookup,
	TextureTargetLookup,
	TextureTargetCreate,
	DrawPrims,
	SourceMapFind,
	SourceCreate,
	SourceHashCacheLookup,
	SourceHashMemoHit,
	SourceHashMemoMiss,
	SourceUpdate,
	SourcePreload,
	Count
};

inline constexpr std::array<const char*, static_cast<size_t>(VitaGxmRendererStage::Count)>
	VitaGxmRendererStageNames = {"source_lookup", "depth_source_lookup",
		"target_lookup", "target_create", "draw_prims", "source_map_find",
		"source_create", "source_hash_cache_lookup", "source_hash_memo_hit",
		"source_hash_memo_miss", "source_update", "source_preload"};

enum class VitaGxmTfxProgram : size_t
{
	General, ManualLod, ManualLodZfloor, Zfloor, Psm16, Psm16Zfloor,
	ZfloorSourceDecalAf, Fast, UvNoFog, UvNoFogFast, UvNoFogZfloor,
	RegionRepeat, RegionRepeatFast, Untextured, SourceOnly, Add, AddDirect,
	Over, SourceDirect, SourceModulate, SourceModulateAf, SourceUntextured,
	FastNoAtst, GsBlend, GsBlendZfloor, GsBlendNoAtst, GsBlendNoAtstZfloor,
	GsBlendSource, GsBlendSourceZfloor, GsBlendSourceNoAtst, GsBlendSourceNoAtstZfloor,
	GsBlendDirectModulateStq, GsBlendDirectModulateStqZfloor,
	Unknown, Count
};
inline constexpr std::array<const char*, static_cast<size_t>(VitaGxmTfxProgram::Count)>
	VitaGxmTfxProgramNames = {"general", "manual_lod", "manual_lod_zfloor", "zfloor",
		"psm16", "psm16_zfloor", "zfloor_source_decal_af", "fast", "uv_no_fog",
		"uv_no_fog_fast", "uv_no_fog_zfloor", "region_repeat", "region_repeat_fast",
		"untextured", "source_only", "add", "add_direct", "over", "source_direct",
		"source_modulate", "source_modulate_af", "source_untextured", "fast_no_atst",
		"gs_blend", "gs_blend_zfloor", "gs_blend_no_atst", "gs_blend_no_atst_zfloor",
		"gs_blend_source", "gs_blend_source_zfloor", "gs_blend_source_no_atst",
		"gs_blend_source_no_atst_zfloor", "gs_blend_direct_modulate_stq",
		"gs_blend_direct_modulate_stq_zfloor", "unknown"};

struct VitaGxmTfxProgramCounters
{
	u64 draws = 0;
	u64 indices = 0;
	// Sum of clipped GS draw bounds, once per submitted chunk. Not fragments,
	// covered pixels or GPU execution time; triangles can overlap or leave gaps.
	u64 draw_rect_pixels = 0;
	// Joint PCSX2 selector condition: ATST=0, DATE=0, Z-floor=0. This does
	// not imply an opaque pass: blending and the compiled shader still matter.
	u64 no_tests_draws = 0;
	u64 no_tests_rect_pixels = 0;
};

struct VitaGxmSceneTransitionCounters
{
	u64 transitions = 0;
	u64 clear_calls = 0;
	u64 clear_followups = 0;
	u64 clear_only_followups = 0;
};

struct VitaGxmSceneContentCounters
{
	u64 scenes = 0;
	u64 end_scene_wall_us = 0;
};

struct VitaGxmPerformanceCounters
{
	std::array<VitaGxmHostCallCounters, static_cast<size_t>(VitaGxmHostCall::Count)> host_calls{};
	std::array<VitaGxmHostCallCounters, static_cast<size_t>(VitaGxmRendererStage::Count)> renderer_stages{};
	std::array<VitaGxmTfxProgramCounters, static_cast<size_t>(VitaGxmTfxProgram::Count)> tfx_programs{};
	// Ordinary submission bits: TFX=1, quad=2, clear=4, scissor-mask=8.
	// Zero means none of these observed, not necessarily an empty GPU scene.
	std::array<VitaGxmSceneContentCounters, 16> scene_contents{};
	// EnsureScene changes only: RT=1, DS=2, scissor=4, from display=8.
	// Zero means no active scene. These are opportunities, not safe-fusion proof.
	std::array<VitaGxmSceneTransitionCounters, 16> scene_transitions{};
	// Combined RenderHW gate: DATE=1, second alpha=2, multi blend=4, colclip=8.
	std::array<u64, 16> unsupported_passes{};
	// CPU2 wall residence for the PCSX2 hardware-renderer analysis and the
	// Vita device translation nested inside it. These are host intervals, not
	// SGX execution time. The worker owns the plain totals and publishes them
	// once per guest VSync with the rest of this structure.
	u64 renderer_draw_calls = 0;
	u64 renderer_draw_wall_us = 0;
	u64 device_render_calls = 0;
	u64 device_render_wall_us = 0;
	u64 draw_calls = 0;
	u64 draw_indices = 0;
	u64 gpu_vu_draw_calls = 0;
	u64 gpu_vu_draw_indices = 0;
	u64 gpu_vu_descriptor_objects = 0;
	u64 vertex_upload_bytes = 0;
	u64 index_upload_bytes = 0;
	u64 texture_uploads = 0;
	u64 texture_upload_bytes = 0;
	u64 texture_readbacks = 0;
	u64 texture_readback_bytes = 0;
	u64 render_store_acquisitions = 0;
	u64 render_store_new_residencies = 0;
	u64 render_store_resident_switches = 0;
	u64 render_store_physical_scenes = 0;
	u64 render_store_loads = 0;
	u64 render_store_materializations = 0;
	u64 render_store_failures = 0;
	u64 tfx_draws = 0;
	u64 textured_tfx_draws = 0;
	u64 render_target_source_draws = 0;
	u64 depth_source_draws = 0;
	u64 rt_hazard_draws = 0;
	u64 depth_hazard_draws = 0;
	u64 feedback_rt_draws = 0;
	u64 feedback_depth_draws = 0;
	u64 software_blend_draws = 0;
	u64 fixed_blend_draws = 0;
	u64 alpha_test_draws = 0;
	u64 partial_color_mask_draws = 0;
	u64 feedback_snapshots = 0;
	u64 feedback_snapshot_bytes = 0;
	u64 merge_calls = 0;
	u64 merge_rc1_draws = 0;
	u64 merge_rc2_draws = 0;
	u64 present_calls = 0;
	u64 interlace_calls = 0;
	u64 last_merge_pmode = 0;
	u64 last_merge_extbuf = 0;
	u32 last_merge_background = 0;
	u32 last_merge_source_sizes[2]{};
	u32 last_merge_source_ids[2]{};
	u8 last_merge_source_mask = 0;
	u8 last_merge_source_states = 0;
	u64 last_merge_writer_tfx_writes = 0;
	u64 last_merge_writer_ps_lo = 0;
	u64 last_merge_writer_ps_hi = 0;
	u64 last_merge_writer_draw_area = 0;
	u64 last_merge_writer_sample_area = 0;
	u32 last_merge_writer_source_id = 0;
	u32 last_merge_writer_source_size = 0;
	u32 last_merge_writer_blend = 0;
	u32 last_merge_writer_selector_keys = 0;
	u8 last_merge_trace_circuit = 0;
	u8 last_merge_writer_kind = 0;
	u8 last_merge_writer_topology = 0;
	u64 last_merge_parent_tfx_writes = 0;
	u64 last_merge_parent_textured_tfx_writes = 0;
	u64 last_merge_parent_untextured_tfx_writes = 0;
	u64 last_merge_parent_render_target_source_tfx_writes = 0;
	u64 last_merge_parent_full_mask_tfx_writes = 0;
	u64 last_merge_parent_rgb_only_tfx_writes = 0;
	u64 last_merge_parent_alpha_only_tfx_writes = 0;
	u64 last_merge_parent_other_mask_tfx_writes = 0;
	u64 last_merge_parent_ps_lo = 0;
	u64 last_merge_parent_ps_hi = 0;
	u64 last_merge_parent_draw_area = 0;
	u64 last_merge_parent_sample_area = 0;
	u32 last_merge_parent_source_id = 0;
	u32 last_merge_parent_source_size = 0;
	u32 last_merge_parent_blend = 0;
	u32 last_merge_parent_selector_keys = 0;
	u64 last_merge_parent_last_rgb_ps_lo = 0;
	u64 last_merge_parent_last_rgb_ps_hi = 0;
	u64 last_merge_parent_last_rgb_draw_area = 0;
	u64 last_merge_parent_last_rgb_sample_area = 0;
	u32 last_merge_parent_last_rgb_source_id = 0;
	u32 last_merge_parent_last_rgb_source_size = 0;
	u32 last_merge_parent_last_rgb_blend = 0;
	u32 last_merge_parent_last_rgb_selector_keys = 0;
	u8 last_merge_parent_kind = 0;
	u8 last_merge_parent_topology = 0;
	u8 last_merge_parent_color_mask = 0;
	u8 last_merge_parent_last_rgb_topology = 0;
	u8 last_merge_parent_last_rgb_color_mask = 0;
	u64 psm24_draws = 0;
	u64 device_rejects = 0;
	u32 last_device_reject_hash = 0;
	u64 last_feedback_ps_lo = 0;
	u64 last_feedback_ps_hi = 0;
	u32 last_feedback_blend = 0;
	u8 last_feedback_vs = 0;
	u8 last_feedback_sampler = 0;
	u8 last_feedback_depth = 0;
	u8 last_feedback_colormask = 0;
	u8 last_feedback_topology = 0;
	u8 last_feedback_hazard = 0;
	u64 rejected_tfx_draws = 0;
	u64 last_rejected_tfx_features = 0;
	u64 last_rejected_tfx_ps_lo = 0;
	u64 last_rejected_tfx_ps_hi = 0;
	u32 last_rejected_tfx_blend = 0;
	u8 last_rejected_tfx_vs = 0;
	u8 last_rejected_tfx_sampler = 0;
	u8 last_rejected_tfx_depth = 0;
	u8 last_rejected_tfx_colormask = 0;
	u8 last_rejected_tfx_topology = 0;
};

// The GS worker updates plain local totals and publishes them once per VSync.
// This avoids one cross-core atomic RMW per draw/upload on Cortex-A9.
void VitaGxmPublishPerformanceCounters();
VitaGxmPerformanceCounters VitaGxmGetPublishedPerformanceCounters();
void VitaGxmRecordRendererDrawTime(u64 elapsed_us);
void VitaGxmRecordRendererStageTime(VitaGxmRendererStage stage, u64 elapsed_us);

// Direct PCSX2 hardware-GS backend for the Vita's SGX543MP4+.  This class is
// intentionally a GSDevice rather than an OpenGL compatibility layer: PCSX2's
// GSRendererHW and GSTextureCache continue to own PS2 behavior, while this
// class translates their device contract to Sony GXM.
class GSDeviceGXM final : public GSDevice
{
public:
	GSDeviceGXM();
	~GSDeviceGXM() override;

	RenderAPI GetRenderAPI() const override;
	bool HasSurface() const override;
	bool Create(GSVSyncMode vsync_mode, bool allow_present_throttle) override;
	void Destroy() override;
	void DestroySurface() override;
	bool UpdateWindow() override;
	void ResizeWindow(u32 new_window_width, u32 new_window_height,
		float new_window_scale) override;
	bool SupportsExclusiveFullscreen() const override;
	PresentResult BeginPresent(bool frame_skip) override;
	void EndPresent() override;
	void SetVSyncMode(GSVSyncMode mode, bool allow_present_throttle) override;
	std::string GetDriverInfo() const override;

	bool SetGPUTimingEnabled(bool enabled) override;
	float GetAndResetAccumulatedGPUTime() override;
	bool SetGPUPipelineStatisticsEnabled(bool enabled) override;
	GPUPipelineStatistics GetAndResetAccumulatedGPUPipelineStatistics() override;
	void SetRenderTargetIdentity(GSTexture* texture,
		const GSRenderTargetIdentity& identity) override;

	void PushDebugGroup(const char* fmt, ...) override;
	void PopDebugGroup() override;
	void InsertDebugMessage(DebugMessageCategory category, const char* fmt, ...) override;

	std::unique_ptr<GSDownloadTexture> CreateDownloadTexture(
		u32 width, u32 height, GSTexture::Format format) override;
	void CopyRect(GSTexture* sTex, GSTexture* dTex, const GSVector4i& r,
		u32 destX, u32 destY) override;
	void PresentRect(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex,
		const GSVector4& dRect, PresentShader shader, float shaderTime,
		Filter filter) override;
	void UpdateCLUTTexture(GSTexture* sTex, float sScale, u32 offsetX,
		u32 offsetY, GSTexture* dTex, u32 dOffset, u32 dSize) override;
	void ConvertToIndexedTexture(GSTexture* sTex, float sScale, u32 offsetX,
		u32 offsetY, u32 SBW, u32 SPSM, GSTexture* dTex, u32 DBW,
		u32 DPSM) override;
	void FilteredDownsampleTexture(GSTexture* sTex, GSTexture* dTex,
		u32 downsample_factor, const GSVector2i& clamp_min,
		const GSVector4& dRect) override;
	void RenderHW(GSHWDrawConfig& config) override;
	void ClearSamplerCache() override;

	// GS-worker-only. Drains asynchronous ShaccCg results and performs every
	// libGXM registration/patcher operation on the context-owning thread.
	void PollGpuVuPrograms();

	// GS-worker-only transactional fixed-GXP service. The first call submits
	// private state/output and publishes a notification; a later call after
	// that notification validates and commits or requests retained-journal CPU
	// replay. It never calls sceGxmFinish().
	void ServiceUniversalGpuVuEpoch(VitaGpuVu::UniversalGpuVuEpoch* epoch);

	// Submits any direct GPU-VU descriptors accumulated during this guest frame
	// so a skipped presentation cannot merge many frames into one GXM scene.
	void EndGpuVuEpoch();

	// GS-worker-only. On immutable-input ring pressure, submits any current
	// direct scene and retires the oldest scene which still owns raw VIF input.
	// This may wait only because the producer is reusing exhausted ring storage.
	bool WaitForGpuVuInputRetirement(
		const VitaGpuVu::RawVifPayloadRef& blocked_generation);
	// Notification-watchdog handoff. The sleeping bridge owns no GXM state; it
	// reports a bounded no-progress interval here so the GS owner can fail every
	// private transaction without releasing storage still visible to the GPU.
	void HandleGpuVuNotificationTimeout(
		uptr address, u32 required_value, u32 observed_value,
		u64 sequence, u64 elapsed_us);
	bool HasFatalGpuFault() const;

	// GS-worker-only. Encodes one already-validated direct VU+TFX descriptor
	// with the supplied PCSX2 draw state. No GSVertex/TfxVertex staging occurs.
	bool RenderGpuVuDraw(GSHWDrawConfig& config,
		std::unique_ptr<VitaGpuVu::GpuVuDraw> draw);
	bool RenderGpuVuDraws(GSHWDrawConfig& config,
		std::vector<std::unique_ptr<VitaGpuVu::GpuVuDraw>> draws);

	// GS-worker-only. Submits all transactional generated draws encoded since
	// the preceding architectural boundary as one vertex-processing firmware
	// job. No CPU wait is performed; completion remains notification-driven.
	bool SubmitGeneratedGpuVuTransactions();

	// GS-worker-only. Called immediately after a native GPU-VU draw has been
	// encoded; ownership is held until its scene's vertex notification retires.
	bool RetainGpuVuDrawForVertexCompletion(
		std::unique_ptr<VitaGpuVu::GpuVuDraw> draw);

protected:
	using GSDevice::DoStretchRect;

	GSTexture* CreateSurface(GSTexture::Usage usage, int width, int height,
		int levels, GSTexture::Format format) override;
	void DoMerge(GSTexture* sTex[3], GSVector4* sRect, GSTexture* dTex,
		GSVector4* dRect, const GSRegPMODE& PMODE, const GSRegEXTBUF& EXTBUF,
		u32 c, Filter filter) override;
	void DoInterlace(GSTexture* sTex, const GSVector4& sRect, GSTexture* dTex,
		const GSVector4& dRect, ShaderInterlace shader, Filter filter,
		const InterlaceConstantBuffer& cb) override;
	void DoFXAA(GSTexture* sTex, GSTexture* dTex) override;
	void DoShadeBoost(GSTexture* sTex, GSTexture* dTex,
		const float params[4]) override;
	bool DoCAS(GSTexture* sTex, GSTexture* dTex, bool sharpen_only,
		const std::array<u32, NUM_CAS_CONSTANTS>& constants) override;
	void DoStretchRect(GSTexture* sTex, const GSVector4& sRect,
		GSTexture* dTex, const GSVector4& dRect,
		ShaderConvertSelector shader, Filter filter) override;
	void DoStretchRect(GSTexture* sTex, const GSVector4& sRect,
		const GSVector4& dRect, PresentShader shader, Filter filter) override;

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};
