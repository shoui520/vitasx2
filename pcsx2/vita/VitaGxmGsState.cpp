// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "vita/VitaGxmGsState.h"

#include "common/Console.h"

bool VitaGxmGsState::HasPcsx2MergeOutput()
{
	// PCSX2 owner: GSRenderer::Merge() and the common early rejection in
	// GSRenderer{HW,SW}::GetOutput(). SCANMSK ages only when Merge has at least
	// one RC texture; a background-only VSync is not enough.
	if (!m_regs ||
		(!PCRTCDisplays.PCRTCDisplays[0].enabled &&
		 !PCRTCDisplays.PCRTCDisplays[1].enabled))
	{
		return false;
	}

	const auto output_is_valid = [this](int display) {
		const int index = display >= 0 ? display : 1;
		const GSPCRTCRegs::PCRTCDisplay& circuit =
			PCRTCDisplays.PCRTCDisplays[index];
		const GSVector2i size = PCRTCDisplays.GetFramebufferSize(display);
		return circuit.FBW != 0 &&
			!PCRTCDisplays.GetFramebufferRect(display).rempty() &&
			size.x >= 0 && size.y >= 0;
	};

	const bool feedback_merge = m_regs->EXTWRITE.WRITE == 1;
	if (PCRTCDisplays.FrameRectMatch() && !PCRTCDisplays.FrameWrap() &&
		!feedback_merge)
	{
		return output_is_valid(-1);
	}

	const GSPCRTCRegs::PCRTCDisplay& rc1 = PCRTCDisplays.PCRTCDisplays[0];
	const GSPCRTCRegs::PCRTCDisplay& rc2 = PCRTCDisplays.PCRTCDisplays[1];
	const bool use_rc1 = rc1.enabled &&
		(!(m_regs->PMODE.MMOD == 1 && m_regs->PMODE.ALP == 0) ||
		 m_regs->PMODE.AMOD == 0 ||
		 (feedback_merge && m_regs->EXTBUF.FBIN == 0));
	const bool rc1_overwrites_rc2 = use_rc1 &&
		rc1.displayRect.rcontains(rc2.displayRect) &&
		m_regs->PMODE.MMOD == 1 && m_regs->PMODE.ALP == 255;
	const bool use_rc2 = rc2.enabled &&
		((m_regs->PMODE.SLBG == 0 && !rc1_overwrites_rc2) ||
		 m_regs->PMODE.AMOD == 1 ||
		 (feedback_merge && m_regs->EXTBUF.FBIN == 1));
	return (use_rc1 && output_is_valid(0)) ||
		(use_rc2 && output_is_valid(1));
}

#if defined(VITASX2_QEMU_VALIDATION)

struct VitaGxmGsState::Impl
{
	bool ready = false;
};

VitaGxmGsState::VitaGxmGsState(bool enable_native_presenter)
	: GSRendererSW(0)
	, m_impl(std::make_unique<Impl>())
{
	(void)enable_native_presenter;
}

VitaGxmGsState::~VitaGxmGsState() = default;

bool VitaGxmGsState::IsNativePresenterReady() const
{
	return false;
}

bool VitaGxmGsState::IsIdleFrame() const
{
	return m_last_present_draw == s_n &&
		m_last_present_transfer == s_transfer_n;
}

void VitaGxmGsState::Present()
{
}

void VitaGxmGsState::VSync(u32 field)
{
	(void)field;
	CompleteVSync();
	// Match the native path's guest-VSync lifetime even though the Linux/QEMU
	// validation build has no GXM presenter.
	const bool merge_output = HasPcsx2MergeOutput();
	if (merge_output)
		s_n++;
	if (merge_output && m_scanmask_used)
		m_scanmask_used--;
	m_last_present_draw = s_n;
	m_last_present_transfer = s_transfer_n;
}

void VitaGxmGsState::Reset(bool hardware_reset)
{
	GSRendererSW::Reset(hardware_reset);
}

void VitaGxmGsState::Draw()
{
	GSRendererSW::Draw();
}

void VitaGxmGsState::InvalidateVideoMem(const GIFRegBITBLTBUF& blit,
	const GSVector4i& rect)
{
	GSRendererSW::InvalidateVideoMem(blit, rect);
}

void VitaGxmGsState::InvalidateLocalMem(const GIFRegBITBLTBUF& blit,
	const GSVector4i& rect, bool clut)
{
	GSRendererSW::InvalidateLocalMem(blit, rect, clut);
}

#else

#include "Config.h"
#include "GS/Renderers/Common/GSVertex.h"
#include "vita/VitaGxmDisplay.h"
#include "vita/VitaGsMemory.h"
#include "vita/VitaGxmMemory.h"

#include <psp2/gxm.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>

extern "C"
{
	extern const SceGxmProgram _binary_vitasx2_color_v_gxp_start;
	extern const SceGxmProgram _binary_vitasx2_color_f_gxp_start;
	extern const SceGxmProgram _binary_vitasx2_present_v_gxp_start;
	extern const SceGxmProgram _binary_vitasx2_present_f_gxp_start;
	extern const SceGxmProgram _binary_vitasx2_merge_f_gxp_start;
	extern const SceGxmProgram _binary_vitasx2_copy_f_gxp_start;
	extern const SceGxmProgram _binary_vitasx2_mad_buffer_f_gxp_start;
	extern const SceGxmProgram _binary_vitasx2_mad_reconstruct_f_gxp_start;
}

namespace
{
	constexpr std::uint32_t GS_TARGET_WIDTH = 1024;
	constexpr std::uint32_t GS_TARGET_HEIGHT = 1024;
	constexpr std::uint32_t GS_TARGET_BYTES =
		GS_TARGET_WIDTH * GS_TARGET_HEIGHT * sizeof(std::uint32_t);
	constexpr std::uint32_t GS_MAD_TARGET_HEIGHT = GS_TARGET_HEIGHT * 2;
	constexpr std::uint32_t GS_MAD_TARGET_BYTES =
		GS_TARGET_WIDTH * GS_MAD_TARGET_HEIGHT * sizeof(std::uint32_t);
	constexpr std::uint32_t STAGING_VERTEX_CAPACITY = 65532;
	constexpr std::uint32_t STAGING_INDEX_CAPACITY = 98304;
	constexpr std::uint32_t PRESENT_VERTEX_CAPACITY = 8;
	constexpr std::uint32_t PRESENT_INDEX_CAPACITY = 12;
	constexpr std::uint32_t PATCHER_BUFFER_BYTES = 64 * 1024;
	constexpr std::uint32_t PATCHER_VERTEX_USSE_BYTES = 64 * 1024;
	constexpr std::uint32_t PATCHER_FRAGMENT_USSE_BYTES = 64 * 1024;

	struct GxmVertex
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
	static_assert(sizeof(GxmVertex) == 24);

	void* PatcherHostAlloc(void* user_data, SceSize size)
	{
		(void)user_data;
		return std::malloc(size);
	}

	void PatcherHostFree(void* user_data, void* memory)
	{
		(void)user_data;
		std::free(memory);
	}

	float NdcX(float x, u32 width)
	{
		return (2.0f * x / static_cast<float>(width)) - 1.0f;
	}

	float NdcY(float y, u32 height)
	{
		return 1.0f - (2.0f * y / static_cast<float>(height));
	}

	void SetVertex(GxmVertex* vertex, float x, float y, u32 color,
		float u = 0.0f, float v = 0.0f)
	{
		vertex->x = x;
		vertex->y = y;
		vertex->z = 0.0f;
		vertex->r = static_cast<u8>(color);
		vertex->g = static_cast<u8>(color >> 8);
		vertex->b = static_cast<u8>(color >> 16);
		vertex->a = static_cast<u8>(color >> 24);
		vertex->u = u;
		vertex->v = v;
	}

	float CurrentPresentationAspect(bool progressive)
	{
		switch (EmuConfig.CurrentAspectRatio)
		{
			case AspectRatioType::Stretch:
				return static_cast<float>(VitaGXM::Display::Width) /
					static_cast<float>(VitaGXM::Display::Height);
			case AspectRatioType::R4_3:
				return 4.0f / 3.0f;
			case AspectRatioType::R16_9:
				return 16.0f / 9.0f;
			case AspectRatioType::R10_7:
				return 10.0f / 7.0f;
			case AspectRatioType::RAuto4_3_3_2:
			default:
				if (EmuConfig.CurrentCustomAspectRatio > 0.0f)
					return EmuConfig.CurrentCustomAspectRatio;
				return progressive ? 3.0f / 2.0f : 4.0f / 3.0f;
		}
	}

	GSVector4 CalculatePresentationRect(bool progressive)
	{
		// Port of GSRenderer.cpp::CalculateDrawDstRect() for the bounded native
		// envelope: full uncropped merge texture, centered, no integer scaling.
		const float width = static_cast<float>(VitaGXM::Display::Width);
		const float height = static_cast<float>(VitaGXM::Display::Height);
		const float client_aspect = width / height;
		const float target_aspect = CurrentPresentationAspect(progressive);
		const double aspect_ratio = target_aspect / client_aspect;
		float target_width = width;
		float target_height = height;
		if (aspect_ratio < 1.0)
			target_width = std::floor(width * aspect_ratio + 0.5);
		else if (aspect_ratio > 1.0)
			target_height = std::floor(height / aspect_ratio + 0.5);
		const float left = (width - target_width) * 0.5f;
		const float top = (height - target_height) * 0.5f;
		return GSVector4(left, top, left + target_width, top + target_height);
	}

	bool BuildMergeQuad(const GSVector4i& source_rect,
		const GSVector4i& merge_rect, const GSVector2i& resolution,
		GSVector4* source_out, GSVector4* destination_out, bool* visible_out)
	{
		if (!source_out || !destination_out || !visible_out || source_rect.rempty() ||
			merge_rect.rempty() || resolution.x <= 0 || resolution.y <= 0)
		{
			return false;
		}
		*visible_out = false;

		// PCSX2 owner: GSRenderer::Merge(). Clip each circuit in merge-texture
		// coordinates while moving the corresponding source edge by the same
		// fraction, then map the clipped destination into the final aspect rect.
		GSVector4 source(source_rect);
		GSVector4 destination(merge_rect);
		const auto clip_low = [](float limit, float& dst_low, float dst_high,
			float& src_low, float src_high) {
			if (dst_low >= limit)
				return;
			const float span = dst_high - dst_low;
			if (span > 0.0f)
				src_low += (src_high - src_low) * ((limit - dst_low) / span);
			dst_low = limit;
		};
		const auto clip_high = [](float limit, float dst_low, float& dst_high,
			float src_low, float& src_high) {
			if (dst_high <= limit)
				return;
			const float span = dst_high - dst_low;
			if (span > 0.0f)
				src_high -= (src_high - src_low) * ((dst_high - limit) / span);
			dst_high = limit;
		};
		clip_low(0.0f, destination.x, destination.z, source.x, source.z);
		clip_low(0.0f, destination.y, destination.w, source.y, source.w);
		clip_high(static_cast<float>(resolution.x), destination.x,
			destination.z, source.x, source.z);
		clip_high(static_cast<float>(resolution.y), destination.y,
			destination.w, source.y, source.w);
		if (destination.z <= destination.x || destination.w <= destination.y)
			return true;
		if (source.z <= source.x || source.w <= source.y || source.x < 0.0f ||
			source.y < 0.0f || source.z > GS_TARGET_WIDTH ||
			source.w > GS_TARGET_HEIGHT)
		{
			return false;
		}

		*source_out = source;
		*destination_out = destination;
		*visible_out = true;
		return true;
	}
} // namespace

struct VitaGxmGsState::Impl
{
	enum class Scene : u8
	{
		None,
		Merge,
		Mad,
		Interlace,
		Display,
	};
	enum class PresentBlend : u8
	{
		Opaque,
		ConstantAlpha,
		SourceAlphaTwice,
	};

	bool enabled = false;
	bool ready = false;
	bool gxm_initialized = false;
	bool frame_active = false;
	bool notification_pending = false;
	Scene scene = Scene::None;

	SceGxmContext* context = nullptr;
	void* context_host = nullptr;
	SceGxmShaderPatcher* patcher = nullptr;
	SceGxmRenderTarget* merge_render_target = nullptr;
	SceGxmRenderTarget* mad_render_target = nullptr;
	SceGxmRenderTarget* interlace_render_target = nullptr;
	SceGxmRenderTarget* display_render_target = nullptr;
	SceGxmDepthStencilSurface disabled_depth = {};
	SceGxmColorSurface merge_color_surface = {};
	SceGxmColorSurface mad_color_surface = {};
	SceGxmColorSurface interlace_color_surface = {};
	SceGxmTexture gs_texture = {};
	SceGxmTexture merge_texture = {};
	SceGxmTexture mad_texture = {};
	SceGxmTexture interlace_texture = {};

	SceGxmShaderPatcherId color_vertex_id = nullptr;
	SceGxmShaderPatcherId color_fragment_id = nullptr;
	SceGxmShaderPatcherId present_vertex_id = nullptr;
	SceGxmShaderPatcherId present_fragment_id = nullptr;
	SceGxmShaderPatcherId merge_fragment_id = nullptr;
	SceGxmShaderPatcherId copy_fragment_id = nullptr;
	SceGxmShaderPatcherId mad_buffer_fragment_id = nullptr;
	SceGxmShaderPatcherId mad_reconstruct_fragment_id = nullptr;
	SceGxmVertexProgram* color_vertex_program = nullptr;
	SceGxmFragmentProgram* color_fragment_program = nullptr;
	SceGxmVertexProgram* present_vertex_program = nullptr;
	SceGxmFragmentProgram* present_copy_fragment_program = nullptr;
	SceGxmFragmentProgram* present_blend_fragment_program = nullptr;
	SceGxmFragmentProgram* present_source_alpha_fragment_program = nullptr;
	SceGxmFragmentProgram* present_blend_rgb_fragment_program = nullptr;
	SceGxmFragmentProgram* present_source_alpha_rgb_fragment_program = nullptr;
	SceGxmFragmentProgram* mad_buffer_fragment_program = nullptr;
	SceGxmFragmentProgram* mad_reconstruct_fragment_program = nullptr;
	const SceGxmProgramParameter* mad_buffer_constants = nullptr;
	const SceGxmProgramParameter* mad_reconstruct_constants = nullptr;

	VitaGXM::MappedBlock vdm_ring;
	VitaGXM::MappedBlock vertex_ring;
	VitaGXM::MappedBlock fragment_ring;
	VitaGXM::MappedBlock fragment_usse_ring;
	VitaGXM::MappedBlock patcher_buffer;
	VitaGXM::MappedBlock patcher_vertex_usse;
	VitaGXM::MappedBlock patcher_fragment_usse;
	VitaGXM::MappedBlock gs_color_buffer;
	VitaGXM::MappedBlock merge_color_buffer;
	VitaGXM::MappedBlock mad_color_buffer;
	VitaGXM::MappedBlock interlace_color_buffer;
	VitaGXM::MappedBlock staging_vertices;
	VitaGXM::MappedBlock staging_indices;
	VitaGXM::Display display;

	SceGxmNotification vertex_notification = {};
	SceGxmNotification fragment_notification = {};
	u32 vertex_used = 0;
	u32 index_used = 0;
	u32 present_attempts = 0;
	u32 presented_frames = 0;
	bool source_contract_logged = false;
	bool unsupported_contract_logged = false;
	bool mad_history_valid = false;
	u32 mad_history_width = 0;
	u32 mad_history_height = 0;
	int mad_buffer_index = 0;

	explicit Impl(bool should_enable)
		: enabled(should_enable)
	{
		if (enabled)
			ready = Initialize();
	}

	~Impl()
	{
		Shutdown();
	}

	bool Fail(const char* operation, int result)
	{
		Console.Error("Vita GXM: %s failed (%08x).", operation,
			static_cast<u32>(result));
		return false;
	}

	bool Initialize();
	void Shutdown();
	bool CreateContext();
	bool CreatePatcher();
	bool CreatePrograms();
	bool CreateRenderTarget(u32 width, u32 height, u32 scenes_per_frame,
		SceGxmRenderTarget** target);
	bool CreateSurfacesAndBuffers();
	bool BeginFrame();
	bool WaitForNotifications();
	bool EndScene(bool notify_completion);
	bool DrainSceneForCpuWrite();
	void ConfigureRaster(u32 width, u32 height);
	bool PrepareMergeTexture(u32 width, u32 height);
	bool BeginMergeScene(u32 width, u32 height);
	bool PrepareInterlaceTextures(u32 width, u32 height);
	bool BeginMadScene(u32 width, u32 height);
	bool BeginInterlaceScene(u32 width, u32 height);
	bool PreloadTarget(GSLocalMemory& memory, u32 bp, u32 bw, u32 psm,
		const GSVector4i& framebuffer_rect, int framebuffer_height);
	bool ReserveGeometry(u32 vertices, u32 indices, GxmVertex** vertex_out,
		u16** index_out, u32* first_vertex, u32* first_index);
	bool BeginDisplayScene();
	bool SubmitQuad(const GSVector4* source_rect,
		const GSVector4& output_rect, u32 color, PresentBlend blend,
		bool preserve_destination_alpha, SceGxmTextureFilter filter,
		u32 target_width, u32 target_height, SceGxmTexture* texture,
		SceGxmFragmentProgram* fragment_override = nullptr,
		const SceGxmProgramParameter* uniform_parameter = nullptr,
		const float* uniform_data = nullptr);
	bool AbortFrame();
	bool Present(VitaGxmGsState& owner, u32 field);
	void ResetTarget();
};

bool VitaGxmGsState::Impl::Initialize()
{
	// Sony PSP2 SDK graphics/api_libgxm/basic owns this process-wide GXM
	// lifecycle. The renderer keeps one context and one display queue for the VM.
	SceGxmInitializeParams initialize = {};
	initialize.flags = SCE_GXM_INITIALIZE_FLAG_DEFAULT;
	initialize.displayQueueMaxPendingCount = VitaGXM::Display::BufferCount;
	initialize.displayQueueCallback = VitaGXM::Display::DisplayQueueCallback;
	initialize.displayQueueCallbackDataSize =
		VitaGXM::Display::DisplayQueueCallbackDataSize();
	initialize.parameterBufferSize = SCE_GXM_DEFAULT_PARAMETER_BUFFER_SIZE;
	const int initialize_result = sceGxmInitialize(&initialize);
	if (initialize_result < 0)
		return Fail("sceGxmInitialize", initialize_result);
	gxm_initialized = true;

	if (!CreateContext() || !CreatePatcher() || !CreatePrograms() ||
		!CreateSurfacesAndBuffers())
	{
		Shutdown();
		return false;
	}

	volatile u32* const notifications = sceGxmGetNotificationRegion();
	if (!notifications)
	{
		Fail("sceGxmGetNotificationRegion", SCE_GXM_ERROR_INVALID_POINTER);
		Shutdown();
		return false;
	}
	vertex_notification.address = notifications;
	vertex_notification.value = 0;
	fragment_notification.address = notifications + 1;
	fragment_notification.value = 0;
	*vertex_notification.address = 0;
	*fragment_notification.address = 0;

	Console.WriteLn(
		"Vita GXM: native presenter ready (GS target=%ux%u, display=%ux%u).",
		GS_TARGET_WIDTH, GS_TARGET_HEIGHT, VitaGXM::Display::Width,
		VitaGXM::Display::Height);
	return true;
}

bool VitaGxmGsState::Impl::CreateContext()
{
	context_host = std::malloc(SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE);
	if (!context_host)
		return Fail("GXM context host allocation", SCE_GXM_ERROR_OUT_OF_MEMORY);

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

	SceGxmContextParams params = {};
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
	return result >= 0 && context ? true :
		Fail("sceGxmCreateContext", result < 0 ? result : SCE_GXM_ERROR_INVALID_POINTER);
}

bool VitaGxmGsState::Impl::CreatePatcher()
{
	int result = VitaGXM::AllocateMappedBlock("VitaSX2 GXM patcher",
		SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_NC_RW, PATCHER_BUFFER_BYTES,
		SCE_GXM_MEMORY_ATTRIB_RW, &patcher_buffer);
	if (result < 0)
		return Fail("shader patcher buffer allocation", result);
	result = VitaGXM::AllocateVertexUsseBlock("VitaSX2 GXM patcher vertex USSE",
		PATCHER_VERTEX_USSE_BYTES, &patcher_vertex_usse);
	if (result < 0)
		return Fail("shader patcher vertex USSE allocation", result);
	result = VitaGXM::AllocateFragmentUsseBlock("VitaSX2 GXM patcher fragment USSE",
		PATCHER_FRAGMENT_USSE_BYTES, &patcher_fragment_usse);
	if (result < 0)
		return Fail("shader patcher fragment USSE allocation", result);

	SceGxmShaderPatcherParams params = {};
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
	return result >= 0 && patcher ? true :
		Fail("sceGxmShaderPatcherCreate",
			result < 0 ? result : SCE_GXM_ERROR_INVALID_POINTER);
}

bool VitaGxmGsState::Impl::CreatePrograms()
{
	const SceGxmProgram* const color_v = &_binary_vitasx2_color_v_gxp_start;
	const SceGxmProgram* const color_f = &_binary_vitasx2_color_f_gxp_start;
	const SceGxmProgram* const present_v = &_binary_vitasx2_present_v_gxp_start;
	const SceGxmProgram* const present_f = &_binary_vitasx2_present_f_gxp_start;
	const SceGxmProgram* const merge_f = &_binary_vitasx2_merge_f_gxp_start;
	const SceGxmProgram* const copy_f = &_binary_vitasx2_copy_f_gxp_start;
	const SceGxmProgram* const mad_buffer_f =
		&_binary_vitasx2_mad_buffer_f_gxp_start;
	const SceGxmProgram* const mad_reconstruct_f =
		&_binary_vitasx2_mad_reconstruct_f_gxp_start;
	for (const SceGxmProgram* program :
		{color_v, color_f, present_v, present_f, merge_f, copy_f,
			mad_buffer_f, mad_reconstruct_f})
	{
		const int result = sceGxmProgramCheck(program);
		if (result < 0)
			return Fail("sceGxmProgramCheck", result);
	}

	int result = sceGxmShaderPatcherRegisterProgram(patcher, color_v,
		&color_vertex_id);
	if (result < 0)
		return Fail("register color vertex program", result);
	result = sceGxmShaderPatcherRegisterProgram(patcher, color_f,
		&color_fragment_id);
	if (result < 0)
		return Fail("register color fragment program", result);
	result = sceGxmShaderPatcherRegisterProgram(patcher, present_v,
		&present_vertex_id);
	if (result < 0)
		return Fail("register present vertex program", result);
	result = sceGxmShaderPatcherRegisterProgram(patcher, present_f,
		&present_fragment_id);
	if (result < 0)
		return Fail("register present fragment program", result);
	result = sceGxmShaderPatcherRegisterProgram(patcher, merge_f,
		&merge_fragment_id);
	if (result < 0)
		return Fail("register merge fragment program", result);
	result = sceGxmShaderPatcherRegisterProgram(patcher, copy_f,
		&copy_fragment_id);
	if (result < 0)
		return Fail("register copy fragment program", result);
	result = sceGxmShaderPatcherRegisterProgram(patcher, mad_buffer_f,
		&mad_buffer_fragment_id);
	if (result < 0)
		return Fail("register MAD buffer fragment program", result);
	result = sceGxmShaderPatcherRegisterProgram(patcher, mad_reconstruct_f,
		&mad_reconstruct_fragment_id);
	if (result < 0)
		return Fail("register MAD reconstruct fragment program", result);

	auto find_attribute = [this](const SceGxmProgram* program,
		const char* name) -> const SceGxmProgramParameter* {
		const SceGxmProgramParameter* parameter =
			sceGxmProgramFindParameterByName(program, name);
		if (!parameter || sceGxmProgramParameterGetCategory(parameter) !=
			SCE_GXM_PARAMETER_CATEGORY_ATTRIBUTE)
		{
			Fail(name, SCE_GXM_ERROR_INVALID_VALUE);
			return nullptr;
		}
		return parameter;
	};
	const SceGxmProgramParameter* color_position =
		find_attribute(color_v, "aPosition");
	const SceGxmProgramParameter* color_color = find_attribute(color_v, "aColor");
	const SceGxmProgramParameter* present_position =
		find_attribute(present_v, "aPosition");
	const SceGxmProgramParameter* present_color =
		find_attribute(present_v, "aColor");
	const SceGxmProgramParameter* present_uv =
		find_attribute(present_v, "aTexCoord");
	const auto find_uniform = [this](const SceGxmProgram* program,
		const char* name) -> const SceGxmProgramParameter* {
		const SceGxmProgramParameter* parameter =
			sceGxmProgramFindParameterByName(program, name);
		if (!parameter || sceGxmProgramParameterGetCategory(parameter) !=
			SCE_GXM_PARAMETER_CATEGORY_UNIFORM)
		{
			Fail(name, SCE_GXM_ERROR_INVALID_VALUE);
			return nullptr;
		}
		return parameter;
	};
	mad_buffer_constants = find_uniform(mad_buffer_f, "ZrH");
	mad_reconstruct_constants = find_uniform(mad_reconstruct_f, "ZrH");
	if (!color_position || !color_color || !present_position || !present_color ||
		!present_uv || !mad_buffer_constants || !mad_reconstruct_constants)
	{
		return false;
	}

	SceGxmVertexAttribute color_attributes[2] = {};
	color_attributes[0].streamIndex = 0;
	color_attributes[0].offset = offsetof(GxmVertex, x);
	color_attributes[0].format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
	color_attributes[0].componentCount = 3;
	color_attributes[0].regIndex =
		sceGxmProgramParameterGetResourceIndex(color_position);
	color_attributes[1].streamIndex = 0;
	color_attributes[1].offset = offsetof(GxmVertex, r);
	color_attributes[1].format = SCE_GXM_ATTRIBUTE_FORMAT_U8N;
	color_attributes[1].componentCount = 4;
	color_attributes[1].regIndex =
		sceGxmProgramParameterGetResourceIndex(color_color);
	SceGxmVertexStream stream = {};
	stream.stride = sizeof(GxmVertex);
	stream.indexSource = SCE_GXM_INDEX_SOURCE_INDEX_16BIT;
	result = sceGxmShaderPatcherCreateVertexProgram(patcher, color_vertex_id,
		color_attributes, 2, &stream, 1, &color_vertex_program);
	if (result < 0 || !color_vertex_program)
		return Fail("create color vertex program", result);

	SceGxmVertexAttribute present_attributes[3] = {};
	present_attributes[0] = color_attributes[0];
	present_attributes[0].regIndex =
		sceGxmProgramParameterGetResourceIndex(present_position);
	present_attributes[1] = color_attributes[1];
	present_attributes[1].regIndex =
		sceGxmProgramParameterGetResourceIndex(present_color);
	present_attributes[2].streamIndex = 0;
	present_attributes[2].offset = offsetof(GxmVertex, u);
	present_attributes[2].format = SCE_GXM_ATTRIBUTE_FORMAT_F32;
	present_attributes[2].componentCount = 2;
	present_attributes[2].regIndex =
		sceGxmProgramParameterGetResourceIndex(present_uv);
	result = sceGxmShaderPatcherCreateVertexProgram(patcher, present_vertex_id,
		present_attributes, 3, &stream, 1, &present_vertex_program);
	if (result < 0 || !present_vertex_program)
		return Fail("create present vertex program", result);

	SceGxmBlendInfo opaque = {};
	opaque.colorFunc = SCE_GXM_BLEND_FUNC_NONE;
	opaque.alphaFunc = SCE_GXM_BLEND_FUNC_NONE;
	opaque.colorSrc = SCE_GXM_BLEND_FACTOR_ZERO;
	opaque.colorDst = SCE_GXM_BLEND_FACTOR_ZERO;
	opaque.alphaSrc = SCE_GXM_BLEND_FACTOR_ZERO;
	opaque.alphaDst = SCE_GXM_BLEND_FACTOR_ZERO;
	opaque.colorMask = SCE_GXM_COLOR_MASK_ALL;
	result = sceGxmShaderPatcherCreateFragmentProgram(patcher, color_fragment_id,
		SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4, SCE_GXM_MULTISAMPLE_NONE,
		&opaque, color_v, &color_fragment_program);
	if (result < 0 || !color_fragment_program)
		return Fail("create color fragment program", result);
	result = sceGxmShaderPatcherCreateFragmentProgram(patcher, copy_fragment_id,
		SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4, SCE_GXM_MULTISAMPLE_NONE,
		&opaque, present_v, &present_copy_fragment_program);
	if (result < 0 || !present_copy_fragment_program)
		return Fail("create copy fragment program", result);
	result = sceGxmShaderPatcherCreateFragmentProgram(patcher,
		mad_buffer_fragment_id, SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
		SCE_GXM_MULTISAMPLE_NONE, &opaque, present_v,
		&mad_buffer_fragment_program);
	if (result < 0 || !mad_buffer_fragment_program)
		return Fail("create MAD buffer fragment program", result);
	result = sceGxmShaderPatcherCreateFragmentProgram(patcher,
		mad_reconstruct_fragment_id, SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4,
		SCE_GXM_MULTISAMPLE_NONE, &opaque, present_v,
		&mad_reconstruct_fragment_program);
	if (result < 0 || !mad_reconstruct_fragment_program)
		return Fail("create MAD reconstruct fragment program", result);

	// PCSX2 GSDevice::DoMerge() blends RC1 over the merge background. Sony's
	// api_libgxm/blending sample owns this matching source-alpha blend state.
	SceGxmBlendInfo source_over = {};
	source_over.colorFunc = SCE_GXM_BLEND_FUNC_ADD;
	source_over.alphaFunc = SCE_GXM_BLEND_FUNC_ADD;
	source_over.colorSrc = SCE_GXM_BLEND_FACTOR_SRC_ALPHA;
	source_over.colorDst = SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	source_over.alphaSrc = SCE_GXM_BLEND_FACTOR_ONE;
	source_over.alphaDst = SCE_GXM_BLEND_FACTOR_ZERO;
	source_over.colorMask = SCE_GXM_COLOR_MASK_ALL;
	result = sceGxmShaderPatcherCreateFragmentProgram(patcher, present_fragment_id,
		SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4, SCE_GXM_MULTISAMPLE_NONE,
		&source_over, present_v, &present_blend_fragment_program);
	if (result < 0 || !present_blend_fragment_program)
		return Fail("create blended present fragment program", result);
	result = sceGxmShaderPatcherCreateFragmentProgram(patcher, merge_fragment_id,
		SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4, SCE_GXM_MULTISAMPLE_NONE,
		&source_over, present_v, &present_source_alpha_fragment_program);
	if (result < 0 || !present_source_alpha_fragment_program)
		return Fail("create source-alpha merge fragment program", result);

	// OpenGL GSDeviceOGL::DoMerge() masks A when PMODE.AMOD keeps RC2 alpha.
	SceGxmBlendInfo source_over_rgb = source_over;
	source_over_rgb.colorMask = SCE_GXM_COLOR_MASK_R | SCE_GXM_COLOR_MASK_G |
		SCE_GXM_COLOR_MASK_B;
	result = sceGxmShaderPatcherCreateFragmentProgram(patcher, present_fragment_id,
		SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4, SCE_GXM_MULTISAMPLE_NONE,
		&source_over_rgb, present_v, &present_blend_rgb_fragment_program);
	if (result < 0 || !present_blend_rgb_fragment_program)
		return Fail("create RGB-only constant merge fragment program", result);
	result = sceGxmShaderPatcherCreateFragmentProgram(patcher, merge_fragment_id,
		SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4, SCE_GXM_MULTISAMPLE_NONE,
		&source_over_rgb, present_v,
		&present_source_alpha_rgb_fragment_program);
	if (result < 0 || !present_source_alpha_rgb_fragment_program)
		return Fail("create RGB-only source-alpha merge fragment program", result);
	return true;
}

bool VitaGxmGsState::Impl::CreateRenderTarget(u32 width, u32 height,
	u32 scenes_per_frame, SceGxmRenderTarget** target)
{
	SceGxmRenderTargetParams params = {};
	params.width = static_cast<u16>(width);
	params.height = static_cast<u16>(height);
	params.scenesPerFrame = static_cast<u16>(scenes_per_frame);
	params.multisampleMode = SCE_GXM_MULTISAMPLE_NONE;
	// VitaSDK's public headers omit Sony's SCE_UID_INVALID_UID spelling. The
	// official samples also use -1 here to request libgxm-owned driver memory.
	params.driverMemBlock = static_cast<SceUID>(-1);
	const int result = sceGxmCreateRenderTarget(&params, target);
	return result >= 0 && *target ? true :
		Fail("sceGxmCreateRenderTarget",
			result < 0 ? result : SCE_GXM_ERROR_INVALID_POINTER);
}

bool VitaGxmGsState::Impl::CreateSurfacesAndBuffers()
{
	if (!CreateRenderTarget(GS_TARGET_WIDTH, GS_TARGET_HEIGHT, 1,
			&merge_render_target) ||
		!CreateRenderTarget(GS_TARGET_WIDTH, GS_MAD_TARGET_HEIGHT, 1,
			&mad_render_target) ||
		!CreateRenderTarget(GS_TARGET_WIDTH, GS_TARGET_HEIGHT, 1,
			&interlace_render_target) ||
		!CreateRenderTarget(VitaGXM::Display::Width, VitaGXM::Display::Height, 1,
			&display_render_target))
	{
		return false;
	}

	int result = VitaGXM::AllocateMappedBlock("VitaSX2 GS color target",
		SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, GS_TARGET_BYTES,
		SCE_GXM_MEMORY_ATTRIB_RW, &gs_color_buffer);
	if (result < 0)
		return Fail("GS color target allocation", result);
	std::memset(gs_color_buffer.base, 0, GS_TARGET_BYTES);
	result = VitaGXM::AllocateMappedBlock("VitaSX2 GS merge target",
		SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, GS_TARGET_BYTES,
		SCE_GXM_MEMORY_ATTRIB_RW, &merge_color_buffer);
	if (result < 0)
		return Fail("GS merge target allocation", result);
	std::memset(merge_color_buffer.base, 0, GS_TARGET_BYTES);
	result = sceGxmColorSurfaceInit(&merge_color_surface,
		SCE_GXM_COLOR_FORMAT_U8U8U8U8_ABGR, SCE_GXM_COLOR_SURFACE_LINEAR,
		SCE_GXM_COLOR_SURFACE_SCALE_NONE, SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT,
		GS_TARGET_WIDTH, GS_TARGET_HEIGHT, GS_TARGET_WIDTH,
		merge_color_buffer.base);
	if (result < 0)
		return Fail("sceGxmColorSurfaceInit(GS merge)", result);
	sceGxmColorSurfaceSetClip(&merge_color_surface, 0, 0,
		GS_TARGET_WIDTH - 1, GS_TARGET_HEIGHT - 1);

	result = VitaGXM::AllocateMappedBlock("VitaSX2 GS MAD history target",
		SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, GS_MAD_TARGET_BYTES,
		SCE_GXM_MEMORY_ATTRIB_RW, &mad_color_buffer);
	if (result < 0)
		return Fail("GS MAD history target allocation", result);
	std::memset(mad_color_buffer.base, 0, GS_MAD_TARGET_BYTES);
	result = sceGxmColorSurfaceInit(&mad_color_surface,
		SCE_GXM_COLOR_FORMAT_U8U8U8U8_ABGR, SCE_GXM_COLOR_SURFACE_LINEAR,
		SCE_GXM_COLOR_SURFACE_SCALE_NONE, SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT,
		GS_TARGET_WIDTH, GS_MAD_TARGET_HEIGHT, GS_TARGET_WIDTH,
		mad_color_buffer.base);
	if (result < 0)
		return Fail("sceGxmColorSurfaceInit(GS MAD history)", result);
	sceGxmColorSurfaceSetClip(&mad_color_surface, 0, 0,
		GS_TARGET_WIDTH - 1, GS_MAD_TARGET_HEIGHT - 1);

	result = VitaGXM::AllocateMappedBlock("VitaSX2 GS interlace target",
		SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, GS_TARGET_BYTES,
		SCE_GXM_MEMORY_ATTRIB_RW, &interlace_color_buffer);
	if (result < 0)
		return Fail("GS interlace target allocation", result);
	std::memset(interlace_color_buffer.base, 0, GS_TARGET_BYTES);
	result = sceGxmColorSurfaceInit(&interlace_color_surface,
		SCE_GXM_COLOR_FORMAT_U8U8U8U8_ABGR, SCE_GXM_COLOR_SURFACE_LINEAR,
		SCE_GXM_COLOR_SURFACE_SCALE_NONE, SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT,
		GS_TARGET_WIDTH, GS_TARGET_HEIGHT, GS_TARGET_WIDTH,
		interlace_color_buffer.base);
	if (result < 0)
		return Fail("sceGxmColorSurfaceInit(GS interlace)", result);
	sceGxmColorSurfaceSetClip(&interlace_color_surface, 0, 0,
		GS_TARGET_WIDTH - 1, GS_TARGET_HEIGHT - 1);

	// Sony's texture sample owns sampling a linear CDRAM buffer. PCSX2's
	// software renderer is the sole writer; GXM only samples completed VSyncs.
	result = sceGxmTextureInitLinear(&gs_texture, gs_color_buffer.base,
		SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR, GS_TARGET_WIDTH,
		GS_TARGET_HEIGHT, 1);
	if (result < 0)
		return Fail("sceGxmTextureInitLinear(GS target)", result);
	result = sceGxmTextureInitLinearStrided(&merge_texture,
		merge_color_buffer.base,
		SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR, GS_TARGET_WIDTH,
		GS_TARGET_HEIGHT, GS_TARGET_WIDTH * sizeof(u32));
	if (result < 0)
		return Fail("sceGxmTextureInitLinear(GS merge)", result);
	result = sceGxmTextureInitLinearStrided(&mad_texture,
		mad_color_buffer.base, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR,
		GS_TARGET_WIDTH, GS_MAD_TARGET_HEIGHT,
		GS_TARGET_WIDTH * sizeof(u32));
	if (result < 0)
		return Fail("sceGxmTextureInitLinear(GS MAD history)", result);
	result = sceGxmTextureInitLinearStrided(&interlace_texture,
		interlace_color_buffer.base,
		SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR, GS_TARGET_WIDTH,
		GS_TARGET_HEIGHT, GS_TARGET_WIDTH * sizeof(u32));
	if (result < 0)
		return Fail("sceGxmTextureInitLinear(GS interlace)", result);
	const auto configure_texture = [](SceGxmTexture* texture) {
		int configure_result = sceGxmTextureSetUAddrMode(texture,
			SCE_GXM_TEXTURE_ADDR_CLAMP);
		if (configure_result >= 0)
			configure_result = sceGxmTextureSetVAddrMode(texture,
				SCE_GXM_TEXTURE_ADDR_CLAMP);
		if (configure_result >= 0 &&
			sceGxmTextureGetType(texture) != SCE_GXM_TEXTURE_LINEAR_STRIDED)
		{
			configure_result = sceGxmTextureSetMinFilter(texture,
				SCE_GXM_TEXTURE_FILTER_POINT);
		}
		if (configure_result >= 0)
			configure_result = sceGxmTextureSetMagFilter(texture,
				SCE_GXM_TEXTURE_FILTER_POINT);
		return configure_result;
	};
	result = configure_texture(&gs_texture);
	if (result >= 0)
		result = configure_texture(&merge_texture);
	if (result >= 0)
		result = configure_texture(&mad_texture);
	if (result >= 0)
		result = configure_texture(&interlace_texture);
	if (result < 0)
	{
		return Fail("configure GS presentation textures", result);
	}

	result = sceGxmDepthStencilSurfaceInitDisabled(&disabled_depth);
	if (result < 0)
		return Fail("sceGxmDepthStencilSurfaceInitDisabled", result);

	result = VitaGXM::AllocateMappedBlock("VitaSX2 GXM vertices",
		SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_NC_RW,
		STAGING_VERTEX_CAPACITY * sizeof(GxmVertex), SCE_GXM_MEMORY_ATTRIB_READ,
		&staging_vertices);
	if (result < 0)
		return Fail("GXM vertex staging allocation", result);
	result = VitaGXM::AllocateMappedBlock("VitaSX2 GXM indices",
		SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_NC_RW,
		STAGING_INDEX_CAPACITY * sizeof(u16), SCE_GXM_MEMORY_ATTRIB_READ,
		&staging_indices);
	if (result < 0)
		return Fail("GXM index staging allocation", result);

	result = display.Initialize();
	return result >= 0 ? true : Fail("Vita GXM display initialization", result);
}

bool VitaGxmGsState::Impl::WaitForNotifications()
{
	if (!notification_pending)
		return true;
	int result = sceGxmNotificationWait(&vertex_notification);
	if (result < 0)
		return Fail("vertex notification wait", result);
	result = sceGxmNotificationWait(&fragment_notification);
	if (result < 0)
		return Fail("fragment notification wait", result);
	notification_pending = false;
	return true;
}

bool VitaGxmGsState::Impl::BeginFrame()
{
	if (frame_active)
		return true;
	if (!WaitForNotifications())
		return false;
	vertex_used = 0;
	index_used = 0;
	frame_active = true;
	return true;
}

bool VitaGxmGsState::Impl::EndScene(bool notify_completion)
{
	if (scene == Scene::None)
		return true;
	SceGxmNotification* vertex = nullptr;
	SceGxmNotification* fragment = nullptr;
	if (notify_completion)
	{
		++vertex_notification.value;
		++fragment_notification.value;
		vertex = &vertex_notification;
		fragment = &fragment_notification;
	}
	const int result = sceGxmEndScene(context, vertex, fragment);
	if (result < 0)
	{
		// The command stream's scene state is indeterminate after a failed close.
		// Keep the logical scene active for bounded shutdown recovery, but never
		// allow another frame to continue through this context.
		ready = false;
		return Fail("sceGxmEndScene", result);
	}
	scene = Scene::None;
	if (notify_completion)
		notification_pending = true;
	return true;
}

bool VitaGxmGsState::Impl::DrainSceneForCpuWrite()
{
	if (scene == Scene::None)
		return true;
	if (!EndScene(true) || !WaitForNotifications())
		return false;
	// All staged geometry is retired after the notification. Reclaiming it here
	// avoids turning a necessary target-coherence seam into a capacity fallback.
	vertex_used = 0;
	index_used = 0;
	return true;
}

void VitaGxmGsState::Impl::ConfigureRaster(u32 width, u32 height)
{
	sceGxmSetRegionClip(context, SCE_GXM_REGION_CLIP_OUTSIDE,
		0, 0, width - 1, height - 1);
	sceGxmSetViewport(context,
		0.5f * static_cast<float>(width), 0.5f * static_cast<float>(width),
		0.5f * static_cast<float>(height), -0.5f * static_cast<float>(height),
		0.5f, 0.5f);
	sceGxmSetViewportEnable(context, SCE_GXM_VIEWPORT_ENABLED);
	sceGxmSetCullMode(context, SCE_GXM_CULL_NONE);
	sceGxmSetFrontPolygonMode(context, SCE_GXM_POLYGON_MODE_TRIANGLE_FILL);
	sceGxmSetBackPolygonMode(context, SCE_GXM_POLYGON_MODE_TRIANGLE_FILL);
	sceGxmSetFrontFragmentProgramEnable(context, SCE_GXM_FRAGMENT_PROGRAM_ENABLED);
	sceGxmSetBackFragmentProgramEnable(context, SCE_GXM_FRAGMENT_PROGRAM_ENABLED);
	sceGxmSetFrontDepthFunc(context, SCE_GXM_DEPTH_FUNC_ALWAYS);
	sceGxmSetBackDepthFunc(context, SCE_GXM_DEPTH_FUNC_ALWAYS);
	sceGxmSetFrontDepthWriteEnable(context, SCE_GXM_DEPTH_WRITE_DISABLED);
	sceGxmSetBackDepthWriteEnable(context, SCE_GXM_DEPTH_WRITE_DISABLED);
}

bool VitaGxmGsState::Impl::PrepareMergeTexture(u32 width, u32 height)
{
	if (width == 0 || height == 0 || width > GS_TARGET_WIDTH ||
		height > GS_TARGET_HEIGHT)
	{
		return false;
	}
	// Sony tutorial_postprocessing/renderbuffer.c owns the linear-strided
	// texture view: the render surface keeps a 1024-pixel pitch while the view
	// clamps at PCSX2's actual merge resolution.
	int result = sceGxmTextureInitLinearStrided(&merge_texture,
		merge_color_buffer.base, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR,
		width, height, GS_TARGET_WIDTH * sizeof(u32));
	if (result >= 0)
		result = sceGxmTextureSetUAddrMode(&merge_texture,
			SCE_GXM_TEXTURE_ADDR_CLAMP);
	if (result >= 0)
		result = sceGxmTextureSetVAddrMode(&merge_texture,
			SCE_GXM_TEXTURE_ADDR_CLAMP);
	return result >= 0 ? true : Fail("configure GS merge texture", result);
}

bool VitaGxmGsState::Impl::BeginMergeScene(u32 width, u32 height)
{
	if (width == 0 || height == 0 || width > GS_TARGET_WIDTH ||
		height > GS_TARGET_HEIGHT)
	{
		return false;
	}
	if (scene != Scene::None)
		return Fail("begin merge scene with another scene active",
			SCE_GXM_ERROR_WITHIN_SCENE);
	// Sony graphics/api_libgxm/render_to_texture submits producer and consumer
	// scenes in order on one immediate context with flags=0. Explicit dependency
	// bits are for cross-pipeline cases such as water_simulation; using an
	// unconditional wait here left BGCOLOR-only frames waiting on no producer.
	const int result = sceGxmBeginScene(context, 0, merge_render_target, nullptr,
		nullptr, nullptr, &merge_color_surface, &disabled_depth);
	if (result < 0)
		return Fail("sceGxmBeginScene(GS merge)", result);
	scene = Scene::Merge;
	ConfigureRaster(width, height);
	return true;
}

bool VitaGxmGsState::Impl::PrepareInterlaceTextures(u32 width, u32 height)
{
	if (width == 0 || height == 0 || width > GS_TARGET_WIDTH ||
		height > GS_TARGET_HEIGHT)
	{
		return false;
	}

	if (mad_history_width != width || mad_history_height != height)
	{
		// PCSX2 GSDevice::ResizeRenderTarget(clear=true) starts a newly-sized MAD
		// target with zero history while preserving the four-field index.
		mad_history_valid = false;
		mad_history_width = width;
		mad_history_height = height;
	}

	int result = sceGxmTextureInitLinearStrided(&mad_texture,
		mad_color_buffer.base, SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR,
		width, height * 2, GS_TARGET_WIDTH * sizeof(u32));
	if (result >= 0)
		result = sceGxmTextureSetUAddrMode(&mad_texture,
			SCE_GXM_TEXTURE_ADDR_CLAMP);
	if (result >= 0)
		result = sceGxmTextureSetVAddrMode(&mad_texture,
			SCE_GXM_TEXTURE_ADDR_CLAMP);
	if (result >= 0)
		result = sceGxmTextureSetMagFilter(&mad_texture,
			SCE_GXM_TEXTURE_FILTER_POINT);
	if (result < 0)
		return Fail("configure GS MAD history texture", result);

	result = sceGxmTextureInitLinearStrided(&interlace_texture,
		interlace_color_buffer.base,
		SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR, width, height,
		GS_TARGET_WIDTH * sizeof(u32));
	if (result >= 0)
		result = sceGxmTextureSetUAddrMode(&interlace_texture,
			SCE_GXM_TEXTURE_ADDR_CLAMP);
	if (result >= 0)
		result = sceGxmTextureSetVAddrMode(&interlace_texture,
			SCE_GXM_TEXTURE_ADDR_CLAMP);
	if (result >= 0)
		result = sceGxmTextureSetMagFilter(&interlace_texture,
			SCE_GXM_TEXTURE_FILTER_POINT);
	return result >= 0 ? true :
		Fail("configure GS interlace texture", result);
}

bool VitaGxmGsState::Impl::BeginMadScene(u32 width, u32 height)
{
	if (width == 0 || height == 0 || width > GS_TARGET_WIDTH ||
		height > GS_TARGET_HEIGHT || scene != Scene::None)
	{
		return false;
	}
	const int result = sceGxmBeginScene(context, 0, mad_render_target,
		nullptr, nullptr, nullptr, &mad_color_surface, &disabled_depth);
	if (result < 0)
		return Fail("sceGxmBeginScene(GS MAD history)", result);
	scene = Scene::Mad;
	ConfigureRaster(width, height * 2);
	return true;
}

bool VitaGxmGsState::Impl::BeginInterlaceScene(u32 width, u32 height)
{
	if (width == 0 || height == 0 || width > GS_TARGET_WIDTH ||
		height > GS_TARGET_HEIGHT || scene != Scene::None)
	{
		return false;
	}
	const int result = sceGxmBeginScene(context, 0, interlace_render_target,
		nullptr, nullptr, nullptr, &interlace_color_surface, &disabled_depth);
	if (result < 0)
		return Fail("sceGxmBeginScene(GS interlace)", result);
	scene = Scene::Interlace;
	ConfigureRaster(width, height);
	return true;
}

bool VitaGxmGsState::Impl::PreloadTarget(GSLocalMemory& memory, u32 bp, u32 bw,
	u32 psm, const GSVector4i& framebuffer_rect, int framebuffer_height)
{
	if (bw == 0 || bw * 64 > GS_TARGET_WIDTH || psm >= 64 ||
		framebuffer_height <= 0 || framebuffer_height > GS_TARGET_HEIGHT)
		return false;
	if (!DrainSceneForCpuWrite())
		return false;

	// PCSX2 owner: GSRendererSW::GetOutput(). Convert the selected PCRTC
	// framebuffer from swizzled GSLocalMemory into the linear RGBA surface GXM
	// samples. This handles CT32/24/16 and the GS framebuffer wrap rules; it
	// replaces the old PSMCT32-only million-call ReadPixel32 walk.
	const GSLocalMemory::psm_t& format = GSLocalMemory::m_psm[psm];
	if (!format.rtx)
		return false;
	const int width = static_cast<int>(bw * 64);
	const int pitch = static_cast<int>(GS_TARGET_WIDTH * sizeof(u32));
	u8* const pixels = static_cast<u8*>(gs_color_buffer.base);
	std::memset(pixels, 0, GS_TARGET_BYTES);

	const int off_x = (framebuffer_rect.x & 0x7ff) & ~(format.bs.x - 1);
	const int off_x_end = ((framebuffer_rect.x & 0x7ff) +
		(format.bs.x - 1)) & ~(format.bs.x - 1);
	const int off_y = (framebuffer_rect.y & 0x7ff) & ~(format.bs.y - 1);
	const int off_y_end = ((framebuffer_rect.y & 0x7ff) +
		(format.bs.y - 1)) & ~(format.bs.y - 1);
	GSVector4i r(off_x, off_y, width + off_x_end,
		framebuffer_height + off_y_end);
	GSVector4i rh(off_x, off_y, width + off_x_end,
		(framebuffer_height + off_y_end) & 0x7ff);
	GSVector4i rw(off_x, off_y, (width + off_x_end) & 0x7ff,
		framebuffer_height + off_y_end);
	bool h_wrap = false;
	bool w_wrap = false;
	if (r.bottom >= 2048)
	{
		r.bottom = 2048;
		rw.bottom = 2048;
		rh.top = 0;
		h_wrap = true;
	}
	if (r.right >= 2048)
	{
		r.right = 2048;
		rh.right = 2048;
		rw.left = 0;
		w_wrap = true;
	}
	const GSVector4i aligned_r = r.ralign<Align_Outside>(format.bs);
	const GSVector4i aligned_rw = rw.ralign<Align_Outside>(format.bs);
	const GSVector4i aligned_rh = rh.ralign<Align_Outside>(format.bs);
	const int top = h_wrap ? (r.bottom - r.top) * pitch : 0;
	// Every display rtx expands to the RGBA32 destination representation,
	// regardless of the source PSM bit depth.
	const int left = w_wrap ?
		(r.right - r.left) * static_cast<int>(sizeof(u32)) : 0;
	const auto conversion_fits = [pitch](const GSVector4i& rect,
		int byte_offset) {
		if (rect.rempty() || byte_offset < 0 || byte_offset >= GS_TARGET_BYTES)
			return false;
		const int row_offset = byte_offset % pitch;
		const int first_row = byte_offset / pitch;
		return rect.width() <= static_cast<int>(GS_TARGET_WIDTH) &&
			rect.height() <= static_cast<int>(GS_TARGET_HEIGHT) &&
			row_offset + rect.width() * static_cast<int>(sizeof(u32)) <= pitch &&
			first_row + rect.height() <= static_cast<int>(GS_TARGET_HEIGHT);
	};
	if (!conversion_fits(aligned_r, 0) ||
		(w_wrap && !conversion_fits(aligned_rw, left)) ||
		(h_wrap && !conversion_fits(aligned_rh, top)))
	{
		return false;
	}
	GSVector4i aligned_rwh;
	if (h_wrap && w_wrap)
	{
		aligned_rwh = GSVector4i(rw.left, rh.top, rw.right, rh.bottom)
			.ralign<Align_Outside>(format.bs);
		if (!conversion_fits(aligned_rwh, top + left))
			return false;
	}

	GIFRegTEXA texa = {};
	texa.AEM = 0;
	texa.TA0 = (psm == PSMCT24 || psm == PSGPU24) ? 0x80 : 0;
	texa.TA1 = 0x80;
	const GSOffset offset = memory.GetOffset(bp, bw, psm);
	format.rtx(memory, offset, aligned_r, pixels, pitch, texa);
	if (w_wrap)
	{
		format.rtx(memory, offset, aligned_rw,
			pixels + left, pitch, texa);
	}
	if (h_wrap)
	{
		format.rtx(memory, offset, aligned_rh,
			pixels + top, pitch, texa);
	}
	if (h_wrap && w_wrap)
	{
		format.rtx(memory, offset, aligned_rwh,
			pixels + top + left, pitch, texa);
	}
	// PCSX2 GSRendererSW::GetOutput() exposes a logical w-by-h texture even
	// though m_output has a fixed 1024-pixel pitch. Sony's linear-strided API
	// preserves the same clamp edge without copying the rows.
	int texture_result = sceGxmTextureInitLinearStrided(&gs_texture, pixels,
		SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR, width, framebuffer_height, pitch);
	if (texture_result >= 0)
		texture_result = sceGxmTextureSetUAddrMode(&gs_texture,
			SCE_GXM_TEXTURE_ADDR_CLAMP);
	if (texture_result >= 0)
		texture_result = sceGxmTextureSetVAddrMode(&gs_texture,
			SCE_GXM_TEXTURE_ADDR_CLAMP);
	if (texture_result < 0)
		return Fail("configure GS output texture", texture_result);
	return true;
}

bool VitaGxmGsState::Impl::ReserveGeometry(u32 vertices, u32 indices,
	GxmVertex** vertex_out, u16** index_out, u32* first_vertex, u32* first_index)
{
	if (!vertex_out || !index_out || !first_vertex || !first_index ||
		vertices > STAGING_VERTEX_CAPACITY - vertex_used ||
		indices > STAGING_INDEX_CAPACITY - index_used)
	{
		return false;
	}
	*first_vertex = vertex_used;
	*first_index = index_used;
	*vertex_out = static_cast<GxmVertex*>(staging_vertices.base) + vertex_used;
	*index_out = static_cast<u16*>(staging_indices.base) + index_used;
	vertex_used += vertices;
	index_used += indices;
	return true;
}

bool VitaGxmGsState::Impl::BeginDisplayScene()
{
	if (scene != Scene::None)
		return Fail("begin display scene with another scene active",
			SCE_GXM_ERROR_WITHIN_SCENE);
	// Sony's render_to_texture sample relies on immediate-context submission
	// order here. A dependency wait is invalid for fallback frames which had no
	// merge producer and is unnecessary for the ordinary texture consumer.
	const int result = sceGxmBeginScene(context, 0, display_render_target,
		nullptr, nullptr, display.BackSyncObject(), display.BackColorSurface(),
		&disabled_depth);
	if (result < 0)
		return Fail("sceGxmBeginScene(display)", result);
	scene = Scene::Display;
	ConfigureRaster(VitaGXM::Display::Width, VitaGXM::Display::Height);
	return true;
}

bool VitaGxmGsState::Impl::SubmitQuad(const GSVector4* source_rect,
	const GSVector4& output_rect, u32 color, PresentBlend blend,
	bool preserve_destination_alpha, SceGxmTextureFilter filter,
	u32 target_width, u32 target_height, SceGxmTexture* texture,
	SceGxmFragmentProgram* fragment_override,
	const SceGxmProgramParameter* uniform_parameter,
	const float* uniform_data)
{
	const bool textured = source_rect != nullptr;
	const u32 source_width = textured && texture ?
		sceGxmTextureGetWidth(texture) : 0;
	const u32 source_height = textured && texture ?
		sceGxmTextureGetHeight(texture) : 0;
	if (output_rect.z <= output_rect.x || output_rect.w <= output_rect.y ||
		output_rect.x < 0.0f || output_rect.y < 0.0f ||
		output_rect.z > static_cast<float>(target_width) ||
		output_rect.w > static_cast<float>(target_height) ||
		(textured && (source_rect->z <= source_rect->x ||
			source_rect->w <= source_rect->y || source_rect->x < 0.0f ||
			source_rect->y < 0.0f ||
			source_rect->z > static_cast<float>(source_width) ||
			source_rect->w > static_cast<float>(source_height))) ||
		(textured && !texture))
	{
		return false;
	}

	constexpr u32 VERTICES = 4;
	constexpr u32 INDICES = 6;
	GxmVertex* vertices = nullptr;
	u16* indices = nullptr;
	u32 first_vertex = 0;
	u32 first_index = 0;
	if (!ReserveGeometry(VERTICES, INDICES, &vertices, &indices,
			&first_vertex, &first_index))
	{
		return false;
	}

	const float left = output_rect.x;
	const float top = output_rect.y;
	const float right = output_rect.z;
	const float bottom = output_rect.w;
	float u0 = 0.0f;
	float v0 = 0.0f;
	float u1 = 1.0f;
	float v1 = 1.0f;
	if (textured)
	{
		u0 = source_rect->x / static_cast<float>(source_width);
		v0 = source_rect->y / static_cast<float>(source_height);
		u1 = source_rect->z / static_cast<float>(source_width);
		v1 = source_rect->w / static_cast<float>(source_height);
	}

	SetVertex(&vertices[0], NdcX(left, target_width),
		NdcY(top, target_height), color, u0, v0);
	SetVertex(&vertices[1], NdcX(right, target_width),
		NdcY(top, target_height), color, u1, v0);
	SetVertex(&vertices[2], NdcX(left, target_width),
		NdcY(bottom, target_height), color, u0, v1);
	SetVertex(&vertices[3], NdcX(right, target_width),
		NdcY(bottom, target_height), color, u1, v1);
	constexpr std::array<u16, INDICES> QUAD = {0, 1, 2, 2, 1, 3};
	for (u32 i = 0; i < INDICES; i++)
		indices[i] = QUAD[i];

	sceGxmSetRegionClip(context, SCE_GXM_REGION_CLIP_OUTSIDE, 0, 0,
		target_width - 1, target_height - 1);
	sceGxmSetVertexStream(context, 0,
		static_cast<GxmVertex*>(staging_vertices.base) + first_vertex);
	if (textured)
	{
		sceGxmSetVertexProgram(context, present_vertex_program);
		SceGxmFragmentProgram* fragment = present_copy_fragment_program;
		if (blend == PresentBlend::ConstantAlpha)
		{
			fragment = preserve_destination_alpha ?
				present_blend_rgb_fragment_program :
				present_blend_fragment_program;
		}
		else if (blend == PresentBlend::SourceAlphaTwice)
		{
			fragment = preserve_destination_alpha ?
				present_source_alpha_rgb_fragment_program :
				present_source_alpha_fragment_program;
		}
		if (fragment_override)
			fragment = fragment_override;
		sceGxmSetFragmentProgram(context, fragment);
		if (uniform_parameter || uniform_data)
		{
			if (!uniform_parameter || !uniform_data)
				return false;
			// Sony tutorial_postprocessing owns the default-uniform lifetime:
			// bind the fragment program, reserve from this context/ring, then upload
			// immediately before the draw. Never retain the returned pointer.
			void* default_buffer = nullptr;
			const int reserve_result =
				sceGxmReserveFragmentDefaultUniformBuffer(context, &default_buffer);
			if (reserve_result < 0 || !default_buffer)
			{
				return Fail("reserve GS interlace fragment constants",
					reserve_result < 0 ? reserve_result :
						SCE_GXM_ERROR_INVALID_POINTER);
			}
			const int uniform_result = sceGxmSetUniformDataF(default_buffer,
				uniform_parameter, 0, 4, uniform_data);
			if (uniform_result < 0)
				return Fail("upload GS interlace fragment constants", uniform_result);
		}
		int filter_result = 0;
		if (sceGxmTextureGetType(texture) != SCE_GXM_TEXTURE_LINEAR_STRIDED)
			filter_result = sceGxmTextureSetMinFilter(texture, filter);
		if (filter_result >= 0)
			filter_result = sceGxmTextureSetMagFilter(texture, filter);
		if (filter_result < 0)
			return Fail("configure GS presentation filter", filter_result);
		const int bind_result = sceGxmSetFragmentTexture(context, 0, texture);
		if (bind_result < 0)
			return Fail("sceGxmSetFragmentTexture(GS target)", bind_result);
	}
	else
	{
		sceGxmSetVertexProgram(context, color_vertex_program);
		sceGxmSetFragmentProgram(context, color_fragment_program);
	}
	const int result = sceGxmDraw(context, SCE_GXM_PRIMITIVE_TRIANGLES,
		SCE_GXM_INDEX_FORMAT_U16,
		static_cast<u16*>(staging_indices.base) + first_index, INDICES);
	return result >= 0 ? true : Fail("sceGxmDraw(display quad)", result);
}

bool VitaGxmGsState::Impl::AbortFrame()
{
	if (scene != Scene::None && !EndScene(true))
		ready = false;
	if (notification_pending && !WaitForNotifications())
		ready = false;
	// A merge scene may have ended without a notification immediately before a
	// display-scene setup failure. This error-only drain guarantees neither CPU
	// buffer is overwritten while that orphaned work is still in flight.
	if (context)
		sceGxmFinish(context);
	vertex_used = 0;
	index_used = 0;
	frame_active = false;
	return false;
}

bool VitaGxmGsState::Impl::Present(VitaGxmGsState& owner, u32 field)
{
	const u32 attempt = ++present_attempts;
	if (attempt <= 64 && (attempt <= 4 || (attempt & (attempt - 1)) == 0))
	{
		Console.WriteLn("Vita GS flow: present_attempt=%u ready=%u regs=%u scene=%u",
			attempt, ready ? 1u : 0u, owner.m_regs ? 1u : 0u,
			static_cast<u32>(scene));
	}
	if (!ready || !owner.m_regs || !BeginFrame())
		return false;

	// PCSX2 owners: GSRendererSW::GetOutput(), GSRenderer::Merge(), and
	// GSDeviceOGL::DoMerge(). A same-source RC1/RC2 pair is unswizzled once,
	// RC2 is copied over BGCOLOR, and RC1 is blended with either PMODE.ALP or
	// twice its framebuffer alpha. A lone circuit uses the same path. Distinct
	// simultaneous framebuffer sources still need a second linear target.
	const GSState::GSPCRTCRegs::PCRTCDisplay& rc1 =
		owner.PCRTCDisplays.PCRTCDisplays[0];
	const GSState::GSPCRTCRegs::PCRTCDisplay& rc2 =
		owner.PCRTCDisplays.PCRTCDisplays[1];
	const GSVector2i resolution = owner.PCRTCDisplays.GetResolution();
	const bool offset_changed_0 =
		rc1.prevFramebufferOffsets.y != rc1.framebufferOffsets.y;
	const bool offset_changed_1 =
		rc2.prevFramebufferOffsets.y != rc2.framebufferOffsets.y;
	const bool game_deinterlacing = offset_changed_0 != offset_changed_1;
	const bool no_crop = GSConfig.Crop[0] == 0 && GSConfig.Crop[1] == 0 &&
		GSConfig.Crop[2] == 0 && GSConfig.Crop[3] == 0;
	const bool standard_postprocess = no_crop && !GSConfig.IntegerScaling &&
		GSConfig.StretchY == 100.0f && !GSConfig.FXAA && !GSConfig.ShadeBoost &&
		GSConfig.CASMode == GSCASMode::Disabled && GSConfig.TVShader == 0 &&
		(GSConfig.LinearPresent == GSPostBilinearMode::Off ||
		 GSConfig.LinearPresent == GSPostBilinearMode::BilinearSmooth);
	// Port GSRenderer::Merge()'s mode and field-order selection exactly. The
	// common owner leaves Automatic full-frame input as raw merge (mode -1), but
	// selects FastMAD (mode 3) for FFMD, game field motion, and scanmask-frame
	// input. Explicit AdaptiveTFF/BFF also select mode 3.
	const bool scanmask_frame = owner.m_scanmask_used != 0 &&
		std::abs(rc1.displayRect.y - rc2.displayRect.y) != 1;
	int field_order = 0;
	int interlace_mode = 3;
	if (GSConfig.InterlaceMode == GSInterlaceMode::Automatic)
	{
		if (!game_deinterlacing && !owner.m_regs->SMODE2.FFMD &&
			!scanmask_frame)
		{
			interlace_mode = -1;
		}
	}
	else if (GSConfig.InterlaceMode != GSInterlaceMode::Off)
	{
		const int configured = static_cast<int>(GSConfig.InterlaceMode) - 2;
		field_order = configured & 1;
		interlace_mode = configured >> 1;
	}
	const u32 interlace_field = (field ^ static_cast<u32>(field_order)) & 1u;
	const bool really_interlaced = owner.isReallyInterlaced();
	const bool raw_interlace = !really_interlaced ||
		GSConfig.InterlaceMode == GSInterlaceMode::Off || interlace_mode < 0;
	const bool fast_mad_interlace = really_interlaced &&
		GSConfig.InterlaceMode != GSInterlaceMode::Off && interlace_mode == 3 &&
		owner.m_scanmask_used == 0;
	const bool interlace_supported = raw_interlace || fast_mad_interlace;
	const bool bob_interlace =
		GSConfig.InterlaceMode == GSInterlaceMode::BobTFF ||
		GSConfig.InterlaceMode == GSInterlaceMode::BobBFF;
	const int ffmd_destination_offset = really_interlaced &&
		owner.m_regs->SMODE2.FFMD && !bob_interlace &&
		!GSConfig.DisableInterlaceOffset &&
		GSConfig.InterlaceMode != GSInterlaceMode::Off ?
		static_cast<int>(interlace_field) : 0;
	const bool feedback_merge = owner.m_regs->EXTWRITE.WRITE == 1;
	const bool same_source = owner.PCRTCDisplays.FrameRectMatch() &&
		!owner.PCRTCDisplays.FrameWrap() && !feedback_merge;
	bool use_rc1 = false;
	bool use_rc2 = false;
	if (same_source)
	{
		// GSRenderer::Merge() shares GetOutput(-1) between both enabled
		// circuits before applying the device merge.
		use_rc1 = rc1.enabled;
		use_rc2 = rc2.enabled;
	}
	else
	{
		use_rc1 = rc1.enabled &&
			(!(owner.m_regs->PMODE.MMOD == 1 && owner.m_regs->PMODE.ALP == 0) ||
			 owner.m_regs->PMODE.AMOD == 0 ||
			 (feedback_merge && owner.m_regs->EXTBUF.FBIN == 0));
		const bool rc1_overwrites_rc2 = use_rc1 &&
			rc1.displayRect.rcontains(rc2.displayRect) &&
			owner.m_regs->PMODE.MMOD == 1 && owner.m_regs->PMODE.ALP == 255;
		use_rc2 = rc2.enabled &&
			((owner.m_regs->PMODE.SLBG == 0 && !rc1_overwrites_rc2) ||
			 owner.m_regs->PMODE.AMOD == 1 ||
			 (feedback_merge && owner.m_regs->EXTBUF.FBIN == 1));
	}

	bool draw_rc1 = use_rc1;
	const bool draw_rc2 = use_rc2 && owner.m_regs->PMODE.SLBG == 0;
	const bool needs_two_sources = draw_rc1 && draw_rc2 && !same_source;
	const int source_index = same_source ? -1 :
		(draw_rc1 ? 0 : (draw_rc2 ? 1 : -2));
	// GSRenderer::Merge() rejects a VSync with no selected, valid RC texture
	// before GSDeviceOGL::DoMerge() can paint BGCOLOR. Keep that distinct from
	// SLBG/AMOD selecting a valid RC2 output which DoMerge intentionally does
	// not draw: that case really is a BGCOLOR-only merge.
	const bool has_merge_output = owner.HasPcsx2MergeOutput();
	if (!has_merge_output &&
		(GSConfig.InterlaceMode == GSInterlaceMode::Automatic ||
		 GSConfig.InterlaceMode >= GSInterlaceMode::AdaptiveTFF))
	{
		// GSRenderer::Merge() clears m_mad on a no-output adaptive frame so
		// stale history cannot flash when a display circuit returns. Deferring
		// the actual clear draw until the next MAD scene is host-internal and
		// preserves that observable result without submitting a useless scene.
		mad_history_valid = false;
	}
	bool present_contract_supported = has_merge_output &&
		resolution.x > 0 && resolution.y > 0 &&
		resolution.x <= static_cast<int>(GS_TARGET_WIDTH) &&
		resolution.y <= static_cast<int>(GS_TARGET_HEIGHT) &&
		!feedback_merge && !needs_two_sources && interlace_supported &&
		standard_postprocess;
	bool source_ready = false;
	bool rc1_quad_ready = false;
	bool rc2_quad_ready = false;
	bool rc1_quad_valid = true;
	bool rc2_quad_valid = true;
	GSVector4 rc1_source = GSVector4::zero();
	GSVector4 rc2_source = GSVector4::zero();
	GSVector4 rc1_destination = GSVector4::zero();
	GSVector4 rc2_destination = GSVector4::zero();
	const bool progressive = owner.GetVideoMode() == GSVideoMode::SDTV_480P;
	const GSVector4 presentation_rect = CalculatePresentationRect(progressive);
	const auto display_psm_supported = [](u32 psm) {
		return psm == PSMCT32 || psm == PSMCT24 || psm == PSMCT16 ||
			psm == PSMCT16S || psm == PSGPU24;
	};
	if (present_contract_supported && source_index != -2)
	{
		const int circuit_index = source_index < 0 ? 1 : source_index;
		const GSState::GSPCRTCRegs::PCRTCDisplay& source_circuit =
			owner.PCRTCDisplays.PCRTCDisplays[circuit_index];
		const GSVector2i framebuffer_size =
			owner.PCRTCDisplays.GetFramebufferSize(source_index);
		const GSVector4i preload_rect =
			owner.PCRTCDisplays.GetFramebufferRect(source_index);
		const bool source_valid = display_psm_supported(source_circuit.PSM) &&
			source_circuit.FBW > 0 &&
			source_circuit.FBW * 64 <= static_cast<int>(GS_TARGET_WIDTH) &&
			framebuffer_size.x >= 0 &&
			framebuffer_size.y > 0 &&
			framebuffer_size.y <= static_cast<int>(GS_TARGET_HEIGHT) &&
			!preload_rect.rempty() && source_circuit.Block() < GS_MAX_BLOCKS;
		if (!source_valid ||
			!PreloadTarget(owner.m_mem,
				static_cast<u32>(source_circuit.Block()),
				static_cast<u32>(source_circuit.FBW),
				static_cast<u32>(source_circuit.PSM), preload_rect,
				framebuffer_size.y))
		{
			present_contract_supported = false;
		}
		else
		{
			owner.PCRTCDisplays.RemoveFramebufferOffset(source_index);
			source_ready = true;
			// GSRenderer::Merge() drops RC1 when both shared-output quads are
			// identical; blending a texture over itself adds no visible color.
			if (same_source && !owner.m_regs->PMODE.SLBG &&
				(rc1.displayRect == rc2.displayRect).alltrue() &&
				(rc1.framebufferRect == rc2.framebufferRect).alltrue())
			{
				draw_rc1 = false;
			}
			if (draw_rc1)
			{
				GSVector4i destination = rc1.displayRect;
				destination.y += ffmd_destination_offset;
				destination.w += ffmd_destination_offset;
				rc1_quad_valid = BuildMergeQuad(rc1.framebufferRect,
					destination, resolution,
					&rc1_source, &rc1_destination, &rc1_quad_ready);
			}
			if (draw_rc2)
			{
				GSVector4i destination = rc2.displayRect;
				destination.y += ffmd_destination_offset;
				destination.w += ffmd_destination_offset;
				rc2_quad_valid = BuildMergeQuad(rc2.framebufferRect,
					destination, resolution,
					&rc2_source, &rc2_destination, &rc2_quad_ready);
			}
			const float source_width =
				static_cast<float>(sceGxmTextureGetWidth(&gs_texture));
			const float source_height =
				static_cast<float>(sceGxmTextureGetHeight(&gs_texture));
			const auto source_fits = [source_width, source_height](
				const GSVector4& source) {
				return source.x >= 0.0f && source.y >= 0.0f &&
					source.z <= source_width && source.w <= source_height;
			};
			if ((rc1_quad_ready && !source_fits(rc1_source)) ||
				(rc2_quad_ready && !source_fits(rc2_source)))
			{
				rc1_quad_ready = false;
				rc2_quad_ready = false;
				rc1_quad_valid = false;
				rc2_quad_valid = false;
			}
			if (!rc1_quad_valid || !rc2_quad_valid)
			{
				present_contract_supported = false;
				source_ready = false;
			}
		}
	}
	if (!present_contract_supported)
	{
		rc1_quad_ready = false;
		rc2_quad_ready = false;
		source_ready = false;
	}
	if (source_ready && !source_contract_logged)
	{
		source_contract_logged = true;
		Console.WriteLn(
			"Vita GS merge active: same_source=%u rc1=%u rc2=%u MMOD=%u ALP=%u interlace=%s field=%u resolution=%dx%d",
			same_source ? 1u : 0u, draw_rc1 ? 1u : 0u,
			draw_rc2 ? 1u : 0u, static_cast<u32>(owner.m_regs->PMODE.MMOD),
			static_cast<u32>(owner.m_regs->PMODE.ALP),
			fast_mad_interlace ? "fast-mad" : "raw", interlace_field,
			resolution.x, resolution.y);
	}
	else if (!present_contract_supported && resolution.x > 0 &&
		resolution.y > 0 && !unsupported_contract_logged)
	{
		unsupported_contract_logged = true;
		Console.Warning(
			"Vita GS merge fallback: feedback=%u distinct_dual=%u interlace=%u postprocess=%u source=%d PMODE=%08x",
			feedback_merge ? 1u : 0u, needs_two_sources ? 1u : 0u,
			interlace_supported ? 1u : 0u, standard_postprocess ? 1u : 0u,
			source_index, static_cast<u32>(owner.m_regs->PMODE.U32[0]));
	}
	const GSVector4 full_display(0.0f, 0.0f,
		static_cast<float>(VitaGXM::Display::Width),
		static_cast<float>(VitaGXM::Display::Height));
	const u32 background_color =
		(owner.m_regs->BGCOLOR.U32[0] & 0x00ffffffu) |
		(static_cast<u32>(owner.m_regs->PMODE.ALP) << 24);
	const u32 rc1_color = 0x00ffffffu |
		(static_cast<u32>(owner.m_regs->PMODE.ALP) << 24);
	const PresentBlend rc1_blend = owner.m_regs->PMODE.MMOD == 1 ?
		PresentBlend::ConstantAlpha : PresentBlend::SourceAlphaTwice;
	const SceGxmTextureFilter merge_filter = GSConfig.PCRTCOffsets ?
		SCE_GXM_TEXTURE_FILTER_LINEAR : SCE_GXM_TEXTURE_FILTER_POINT;
	const SceGxmTextureFilter present_filter =
		GSConfig.LinearPresent == GSPostBilinearMode::Off ?
			SCE_GXM_TEXTURE_FILTER_POINT : SCE_GXM_TEXTURE_FILTER_LINEAR;
	bool submitted = EndScene(false);
	if (submitted && present_contract_supported)
	{
		const u32 output_width = static_cast<u32>(resolution.x);
		const u32 output_height = static_cast<u32>(resolution.y);
		const GSVector4 merge_rect(0.0f, 0.0f,
			static_cast<float>(resolution.x), static_cast<float>(resolution.y));
		submitted = PrepareMergeTexture(output_width, output_height) &&
			BeginMergeScene(output_width, output_height) &&
			SubmitQuad(nullptr, merge_rect, background_color,
				PresentBlend::Opaque, false, SCE_GXM_TEXTURE_FILTER_POINT,
				output_width, output_height,
				nullptr) &&
			(!rc2_quad_ready || SubmitQuad(&rc2_source, rc2_destination,
				0xffffffffu, PresentBlend::Opaque, false, merge_filter,
				output_width, output_height,
				&gs_texture)) &&
			(!rc1_quad_ready || SubmitQuad(&rc1_source, rc1_destination,
				rc1_color, rc1_blend, owner.m_regs->PMODE.AMOD == 1,
				merge_filter, output_width, output_height, &gs_texture)) &&
			EndScene(false);

		SceGxmTexture* completed_texture = &merge_texture;
		if (submitted && fast_mad_interlace)
		{
			submitted = PrepareInterlaceTextures(output_width, output_height);
			if (submitted)
			{
				// PCSX2 GSDevice::Interlace(mode=3) owns this four-field index.
				mad_buffer_index++;
				mad_buffer_index &= ~1;
				mad_buffer_index |= static_cast<int>(interlace_field);
				mad_buffer_index &= 3;

				const float mad_height = static_cast<float>(output_height * 2);
				const int bank = mad_buffer_index >> 1;
				const GSVector4 mad_full_rect(0.0f, 0.0f,
					static_cast<float>(output_width), mad_height);
				const GSVector4 mad_bank_rect(0.0f,
					bank ? static_cast<float>(output_height) : 0.0f,
					static_cast<float>(output_width),
					bank ? mad_height : static_cast<float>(output_height));
				const float mad_constants[4] = {
					static_cast<float>(mad_buffer_index), 1.0f / mad_height,
					mad_height, 0.08f};
				submitted = BeginMadScene(output_width, output_height) &&
					(mad_history_valid || SubmitQuad(nullptr, mad_full_rect,
						0x00000000u, PresentBlend::Opaque, false,
						SCE_GXM_TEXTURE_FILTER_POINT, output_width,
						output_height * 2, nullptr)) &&
					SubmitQuad(&merge_rect, mad_bank_rect, 0xffffffffu,
						PresentBlend::Opaque, false,
						SCE_GXM_TEXTURE_FILTER_POINT, output_width,
						output_height * 2, &merge_texture,
						mad_buffer_fragment_program, mad_buffer_constants,
						mad_constants) &&
					EndScene(false);
				if (submitted)
					mad_history_valid = true;
			}

			if (submitted)
			{
				const float reconstruct_constants[4] = {
					static_cast<float>(mad_buffer_index),
					1.0f / static_cast<float>(output_height),
					static_cast<float>(output_height), 0.08f};
				const GSVector4 mad_source_rect(0.0f, 0.0f,
					static_cast<float>(output_width),
					static_cast<float>(output_height * 2));
				submitted = BeginInterlaceScene(output_width, output_height) &&
					SubmitQuad(&mad_source_rect, merge_rect, 0xffffffffu,
						PresentBlend::Opaque, false,
						SCE_GXM_TEXTURE_FILTER_POINT, output_width,
						output_height, &mad_texture,
						mad_reconstruct_fragment_program,
						mad_reconstruct_constants, reconstruct_constants) &&
					EndScene(false);
				completed_texture = &interlace_texture;
			}
		}

		if (submitted)
		{
			submitted = BeginDisplayScene() &&
			SubmitQuad(nullptr, full_display, 0xff000000u,
				PresentBlend::Opaque, false, SCE_GXM_TEXTURE_FILTER_POINT,
				VitaGXM::Display::Width, VitaGXM::Display::Height, nullptr) &&
			SubmitQuad(&merge_rect, presentation_rect, 0xffffffffu,
				PresentBlend::Opaque, false, present_filter,
				VitaGXM::Display::Width, VitaGXM::Display::Height,
				completed_texture) && EndScene(true);
		}
	}
	else if (submitted)
	{
		const u32 fallback_color = has_merge_output ?
			background_color : 0xff000000u;
		submitted = BeginDisplayScene() &&
			SubmitQuad(nullptr, full_display, fallback_color,
				PresentBlend::Opaque, false, SCE_GXM_TEXTURE_FILTER_POINT,
				VitaGXM::Display::Width, VitaGXM::Display::Height, nullptr) &&
			EndScene(true);
	}
	if (!submitted)
	{
		return AbortFrame();
	}

	const int heartbeat_result =
		sceGxmPadHeartbeat(display.BackColorSurface(), display.BackSyncObject());
	if (heartbeat_result < 0)
	{
		Fail("sceGxmPadHeartbeat", heartbeat_result);
		return AbortFrame();
	}
	const int queue_result = display.QueuePresent();
	if (queue_result < 0)
	{
		Fail("sceGxmDisplayQueueAddEntry", queue_result);
		return AbortFrame();
	}
	const u32 frame = ++presented_frames;
	if (frame <= 64 && (frame <= 4 || (frame & (frame - 1)) == 0))
	{
		Console.WriteLn("Vita GS flow: queued_present=%u source_ready=%u contract=%u rc1=%u rc2=%u resolution=%dx%d",
			frame, source_ready ? 1u : 0u,
			present_contract_supported ? 1u : 0u, draw_rc1 ? 1u : 0u,
			draw_rc2 ? 1u : 0u, resolution.x, resolution.y);
	}
	frame_active = false;
	return true;
}

void VitaGxmGsState::Impl::ResetTarget()
{
	// GSState::Reset()/Defrost() mutate the canonical GS ring. Retire any scene
	// which still references staging or target CDRAM, then force the next draw
	// or presentation to preload the newly reset/restored canonical contents.
	bool retired = true;
	if (scene != Scene::None)
		retired = DrainSceneForCpuWrite();
	else if (notification_pending)
		retired = WaitForNotifications();
	if (!retired)
		ready = false;
	vertex_used = 0;
	index_used = 0;
	frame_active = false;
	mad_history_valid = false;
}

void VitaGxmGsState::Impl::Shutdown()
{
	ready = false;
	const auto succeeded = [this](const char* operation, int result) {
		return result >= 0 ? true : Fail(operation, result);
	};

	// Sony's api_libgxm/basic teardown and gxm/display_queue.h require all GPU
	// work and display callbacks to be complete before any referenced resource is
	// released. On any failure, deliberately retain the remaining ownership and
	// do not call sceGxmTerminate(); leaking until process exit is safer than a
	// callback or GPU job observing freed memory.
	if (scene != Scene::None && context && !EndScene(false))
		return;
	if (context)
		sceGxmFinish(context);
	if (!succeeded("sceGxmDisplayQueueFinish(shutdown)", display.Finish()) ||
		!succeeded("Vita GXM display destroy", display.Destroy()))
	{
		return;
	}

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
	release_fragment(mad_reconstruct_fragment_program,
		"sceGxmShaderPatcherReleaseFragmentProgram(MAD reconstruct)");
	release_fragment(mad_buffer_fragment_program,
		"sceGxmShaderPatcherReleaseFragmentProgram(MAD buffer)");
	release_fragment(present_source_alpha_rgb_fragment_program,
		"sceGxmShaderPatcherReleaseFragmentProgram(present source alpha RGB)");
	release_fragment(present_blend_rgb_fragment_program,
		"sceGxmShaderPatcherReleaseFragmentProgram(present blend RGB)");
	release_fragment(present_source_alpha_fragment_program,
		"sceGxmShaderPatcherReleaseFragmentProgram(present source alpha)");
	release_fragment(present_blend_fragment_program,
		"sceGxmShaderPatcherReleaseFragmentProgram(present blend)");
	release_fragment(present_copy_fragment_program,
		"sceGxmShaderPatcherReleaseFragmentProgram(present copy)");
	release_vertex(present_vertex_program,
		"sceGxmShaderPatcherReleaseVertexProgram(present)");
	release_fragment(color_fragment_program,
		"sceGxmShaderPatcherReleaseFragmentProgram(color)");
	release_vertex(color_vertex_program,
		"sceGxmShaderPatcherReleaseVertexProgram(color)");
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
	unregister(mad_reconstruct_fragment_id,
		"sceGxmShaderPatcherUnregisterProgram(MAD reconstruct fragment)");
	unregister(mad_buffer_fragment_id,
		"sceGxmShaderPatcherUnregisterProgram(MAD buffer fragment)");
	unregister(copy_fragment_id,
		"sceGxmShaderPatcherUnregisterProgram(copy fragment)");
	unregister(merge_fragment_id,
		"sceGxmShaderPatcherUnregisterProgram(merge fragment)");
	unregister(present_fragment_id,
		"sceGxmShaderPatcherUnregisterProgram(present fragment)");
	unregister(present_vertex_id,
		"sceGxmShaderPatcherUnregisterProgram(present vertex)");
	unregister(color_fragment_id,
		"sceGxmShaderPatcherUnregisterProgram(color fragment)");
	unregister(color_vertex_id,
		"sceGxmShaderPatcherUnregisterProgram(color vertex)");
	if (!group_ok)
		return;

	const auto destroy_target = [&](SceGxmRenderTarget*& target,
		const char* operation) {
		if (!target)
			return;
		if (succeeded(operation, sceGxmDestroyRenderTarget(target)))
			target = nullptr;
		else
			group_ok = false;
	};
	destroy_target(interlace_render_target,
		"sceGxmDestroyRenderTarget(interlace)");
	destroy_target(mad_render_target, "sceGxmDestroyRenderTarget(MAD history)");
	destroy_target(merge_render_target, "sceGxmDestroyRenderTarget(merge)");
	destroy_target(display_render_target, "sceGxmDestroyRenderTarget(display)");
	if (!group_ok)
		return;

	const auto release_block = [&](VitaGXM::MappedBlock& block,
		const char* operation) {
		if (!block.IsAllocated())
			return;
		if (!succeeded(operation, VitaGXM::ReleaseMappedBlock(&block)))
			group_ok = false;
	};
	release_block(staging_indices, "release staging indices");
	release_block(staging_vertices, "release staging vertices");
	release_block(interlace_color_buffer, "release GS interlace color buffer");
	release_block(mad_color_buffer, "release GS MAD history color buffer");
	release_block(merge_color_buffer, "release GS merge color buffer");
	release_block(gs_color_buffer, "release GS color buffer");
	if (!group_ok)
		return;

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
	frame_active = false;
	notification_pending = false;
	scene = Scene::None;
}

VitaGxmGsState::VitaGxmGsState(bool enable_native_presenter)
	: GSRendererSW(GSConfig.SWExtraThreads)
	, m_impl(std::make_unique<Impl>(enable_native_presenter))
{
}

VitaGxmGsState::~VitaGxmGsState() = default;

bool VitaGxmGsState::IsNativePresenterReady() const
{
	return m_impl && m_impl->ready;
}

bool VitaGxmGsState::IsIdleFrame() const
{
	return m_last_present_draw == s_n &&
		m_last_present_transfer == s_transfer_n;
}

void VitaGxmGsState::Present()
{
	// PCSX2's PresentCurrentFrame() reuses its cached completed merge texture.
	// Vita's fixed display is already scanning out that completed queue entry;
	// rereading mutable GSLocalMemory here would mix post-VSync bytes with old
	// PCRTC state. Leave the retained scanout untouched until the next VSync.
}

void VitaGxmGsState::VSync(u32 field)
{
	// PCSX2 owner: GSRendererSW::VSync(). Quiesce every raster worker before
	// GXM reads canonical GSLocalMemory for the display circuit.
	CompleteVSync();
	// PCSX2 owner: GSRenderer::VSync() -> Merge(). SCANMSK influences the
	// current merge and then ages once per processed VSync. Unsupported GXM
	// presentation still processes a background frame, so it must age too or a
	// single SCANMSK write would permanently suppress otherwise-supported output.
	const bool merge_output = HasPcsx2MergeOutput();
	if (merge_output)
		s_n++;
	const bool processed = m_impl &&
		(!m_impl->enabled || (m_impl->ready && m_impl->Present(*this, field)));
	if (processed && merge_output && m_scanmask_used)
		m_scanmask_used--;
	m_last_present_draw = s_n;
	m_last_present_transfer = s_transfer_n;
}

void VitaGxmGsState::Reset(bool hardware_reset)
{
	GSRendererSW::Reset(hardware_reset);
	if (m_impl && m_impl->ready)
		m_impl->ResetTarget();
}

void VitaGxmGsState::Draw()
{
	// PCSX2 owner: GSRendererSW::Draw(). Until GSRendererHW/GSTextureCache have
	// a complete GXM device contract, every primitive goes through PCSX2's
	// general software GS semantics and updates canonical GSLocalMemory. GXM is
	// deliberately presentation-only, avoiding mixed CPU/GPU ownership.
	GSRendererSW::Draw();
}

void VitaGxmGsState::InvalidateVideoMem(const GIFRegBITBLTBUF& blit,
	const GSVector4i& rect)
{
	GSRendererSW::InvalidateVideoMem(blit, rect);
}

void VitaGxmGsState::InvalidateLocalMem(const GIFRegBITBLTBUF& blit,
	const GSVector4i& rect, bool clut)
{
	// PCSX2's software renderer owns every GS write, so its synchronization and
	// texture-cache invalidation are the complete local-memory contract. GXM
	// never owns newer pixels and therefore needs no GPU readback.
	GSRendererSW::InvalidateLocalMem(blit, rect, clut);
}

#endif
