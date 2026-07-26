// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/Common/GSDevice.h"

#include <memory>

namespace VitaGpuVu
{
	class GpuVuDraw;
}

struct VitaGxmPerformanceCounters
{
	u64 draw_calls = 0;
	u64 draw_indices = 0;
	u64 vertex_upload_bytes = 0;
	u64 index_upload_bytes = 0;
	u64 texture_uploads = 0;
	u64 texture_upload_bytes = 0;
	u64 texture_readbacks = 0;
	u64 texture_readback_bytes = 0;
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
