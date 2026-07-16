// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/Common/GSDevice.h"

#include <memory>

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
