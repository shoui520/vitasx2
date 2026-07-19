// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

struct GSHWDrawConfig;
class GSTexture;
class GSVector4;
class GSVector4i;

namespace VitaGXM
{
	class GSTextureGXM;
}

enum class VitaGsDrawTraceOutcome : u8
{
	NotSubmitted = 0,
	DeviceEntered = 1,
	RejectedSelector = 2,
	RejectedContract = 3,
	Accepted = 4,
	Drawn = 5,
};

enum VitaGsDrawTracePath : u32
{
	VITA_GS_DRAW_PATH_PSM24 = 1u << 0,
	VITA_GS_DRAW_PATH_PSM16 = 1u << 1,
	VITA_GS_DRAW_PATH_MIP_LOD = 1u << 2,
	VITA_GS_DRAW_PATH_REGION_REPEAT = 1u << 3,
	VITA_GS_DRAW_PATH_FAST_FRAGMENT = 1u << 4,
	VITA_GS_DRAW_PATH_PROGRAMMABLE_ADD = 1u << 5,
	VITA_GS_DRAW_PATH_PROGRAMMABLE_ADD_DIRECT = 1u << 6,
	VITA_GS_DRAW_PATH_PROGRAMMABLE_OVER = 1u << 7,
	VITA_GS_DRAW_PATH_SOURCE_ONLY = 1u << 8,
	VITA_GS_DRAW_PATH_SOURCE_DIRECT = 1u << 9,
	VITA_GS_DRAW_PATH_SOURCE_DIRECT_MODULATE = 1u << 10,
	VITA_GS_DRAW_PATH_SOURCE_DIRECT_MODULATE_AF = 1u << 11,
	VITA_GS_DRAW_PATH_UNTEXTURED = 1u << 12,
	VITA_GS_DRAW_PATH_RT_SNAPSHOT = 1u << 13,
};

// PCSX2 owner: GSRendererHW::Draw() publishes GSHWDrawConfig immediately before
// GSDevice::RenderHW(), at the same point used by GSHWDrawConfig::DumpConfig().
// These hooks exist only in VITASX2_GS_DRAW_TRACE builds.
void VitaGsDrawTraceNotifyElfEntry();
void VitaGsDrawTraceVSync();
// PCSX2 owner: GSState::Transfer() records the exact raw transfer before GIF
// decoding. The Vita mailbox records that same byte stream at the worker
// boundary so a missing packet can be distinguished from a GS/GXM draw bug.
void VitaGsDrawTraceRecordPacket(u8 source, const void* data, u32 size,
	bool mtvu_packet);
// PCSX2 owner: GSRenderer::VSync() presents GSDevice::GetCurrent() only after
// Merge()/Interlace(). Capture that exact source before BeginPresent(), then
// capture the GXM scanout buffer after EndScene so merge and final-copy faults
// are distinguishable from draw faults.
void VitaGsDrawTraceRecordPresentSource(const GSTexture* texture,
	const GSVector4i& source_rect, const GSVector4& source_uv,
	const GSVector4& destination_rect);
bool VitaGsDrawTraceNeedsPresentSourceCapture();
void VitaGsDrawTraceCapturePresentSource();
bool VitaGsDrawTraceNeedsDisplayCapture();
void VitaGsDrawTraceRecordDisplay(const void* pixels, u32 pitch,
	u32 width, u32 height, u32 bytes_per_pixel);
void VitaGsDrawTraceRecordConfig(u32 draw_index, u32 frame, u8 source,
	const GSHWDrawConfig& config);
void VitaGsDrawTraceRecordTextureContent(
	const VitaGXM::GSTextureGXM& texture, u32 level,
	const GSVector4i& updated_rect);
void VitaGsDrawTraceRecordBackendState(const GSHWDrawConfig& config,
	const VitaGXM::GSTextureGXM& bound_texture,
	const void* staged_vertices, u32 staged_vertex_bytes,
	const void* staged_indices, u32 staged_index_bytes,
	u32 index_count, u32 sampler_state, u32 depth_state,
	u32 raster_state, u32 color_mask);
// PCSX2's hardware renderers resolve an existing render target before it is
// sampled by a later draw. The first call tells GXM whether a scene completion
// is needed; the second hashes the now-visible logical pixels and attaches the
// last trace-visible writer identity to the current draw.
bool VitaGsDrawTraceRenderTargetSourceNeedsSync(
	const GSHWDrawConfig& config, const VitaGXM::GSTextureGXM& texture,
	u64 content_generation);
void VitaGsDrawTraceRecordRenderTargetSource(
	const GSHWDrawConfig& config, const VitaGXM::GSTextureGXM& texture,
	u64 content_generation);
void VitaGsDrawTraceRecordRenderTargetWrite(const GSHWDrawConfig& config,
	const VitaGXM::GSTextureGXM& texture, u64 content_generation);
// Opt-in fine trace: synchronize only a bounded configured draw window and
// capture what the backend actually left in the color target. Normal builds
// contain neither hook, and ordinary draw traces default the budget to zero.
bool VitaGsDrawTraceRenderTargetAfterDrawNeedsSync(
	const GSHWDrawConfig& config, const VitaGXM::GSTextureGXM& texture,
	u64 content_generation);
void VitaGsDrawTraceRecordRenderTargetAfterDraw(
	const GSHWDrawConfig& config, const VitaGXM::GSTextureGXM& texture,
	u64 content_generation);
void VitaGsDrawTraceRecordDeviceOutcome(const GSHWDrawConfig& config,
	VitaGsDrawTraceOutcome outcome, u32 path_flags = 0, u32 detail = 0);
