// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "vita/VitaGxmGsState.h"

#include "common/Console.h"

bool VitaGxmGsState::IsCoverageAlphaSupported()
{
	// PCSX2 owners: GSRendererSW::IsCoverageAlphaSupported() and
	// GSRendererHW::IsCoverageAlphaSupported(). The bounded GXM draw contract
	// rejects PRIM.AA1, so it cannot provide coverage-alpha rasterization yet.
	return false;
}

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
	: m_impl(std::make_unique<Impl>())
{
	(void)enable_native_presenter;
}

VitaGxmGsState::~VitaGxmGsState() = default;

bool VitaGxmGsState::IsNativePresenterReady() const
{
	return false;
}

void VitaGxmGsState::Present()
{
}

void VitaGxmGsState::VSync()
{
	// Match the native path's guest-VSync lifetime even though the Linux/QEMU
	// validation build has no GXM presenter.
	if (HasPcsx2MergeOutput() && m_scanmask_used)
		m_scanmask_used--;
}

void VitaGxmGsState::Reset(bool hardware_reset)
{
	GSState::Reset(hardware_reset);
}

void VitaGxmGsState::Draw()
{
}

void VitaGxmGsState::InvalidateVideoMem(const GIFRegBITBLTBUF& blit,
	const GSVector4i& rect)
{
	(void)blit;
	(void)rect;
}

void VitaGxmGsState::InvalidateLocalMem(const GIFRegBITBLTBUF& blit,
	const GSVector4i& rect, bool clut)
{
	(void)blit;
	(void)rect;
	(void)clut;
}

#else

#include "Config.h"
#include "GS/Renderers/Common/GSVertex.h"
#include "vita/VitaGxmDisplay.h"
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
}

namespace
{
	constexpr std::uint32_t GS_TARGET_WIDTH = 1024;
	constexpr std::uint32_t GS_TARGET_HEIGHT = 1024;
	constexpr std::uint32_t GS_TARGET_BYTES =
		GS_TARGET_WIDTH * GS_TARGET_HEIGHT * sizeof(std::uint32_t);
	constexpr std::uint32_t STAGING_VERTEX_CAPACITY = 65532;
	constexpr std::uint32_t STAGING_INDEX_CAPACITY = 98304;
	constexpr std::uint32_t PRESENT_VERTEX_CAPACITY = 8;
	constexpr std::uint32_t PRESENT_INDEX_CAPACITY = 12;
	constexpr std::uint32_t GS_VERTEX_CAPACITY =
		STAGING_VERTEX_CAPACITY - PRESENT_VERTEX_CAPACITY;
	constexpr std::uint32_t GS_INDEX_CAPACITY =
		STAGING_INDEX_CAPACITY - PRESENT_INDEX_CAPACITY;
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

	float TargetNdcX(int x)
	{
		return (2.0f * static_cast<float>(x) /
			static_cast<float>(GS_TARGET_WIDTH)) - 1.0f;
	}

	float TargetNdcY(int y)
	{
		return 1.0f - (2.0f * static_cast<float>(y) /
			static_cast<float>(GS_TARGET_HEIGHT));
	}

	float DisplayNdcX(float x)
	{
		return (2.0f * static_cast<float>(x) /
			static_cast<float>(VitaGXM::Display::Width)) - 1.0f;
	}

	float DisplayNdcY(float y)
	{
		return 1.0f - (2.0f * static_cast<float>(y) /
			static_cast<float>(VitaGXM::Display::Height));
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
} // namespace

struct VitaGxmGsState::Impl
{
	enum class Scene : u8
	{
		None,
		GsTarget,
		Display,
	};

	bool enabled = false;
	bool ready = false;
	bool gxm_initialized = false;
	bool frame_active = false;
	bool notification_pending = false;
	bool target_valid = false;
	bool target_stale = true;
	Scene scene = Scene::None;

	SceGxmContext* context = nullptr;
	void* context_host = nullptr;
	SceGxmShaderPatcher* patcher = nullptr;
	SceGxmRenderTarget* gs_render_target = nullptr;
	SceGxmRenderTarget* display_render_target = nullptr;
	SceGxmDepthStencilSurface disabled_depth = {};
	SceGxmColorSurface gs_color_surface = {};
	SceGxmTexture gs_texture = {};

	SceGxmShaderPatcherId color_vertex_id = nullptr;
	SceGxmShaderPatcherId color_fragment_id = nullptr;
	SceGxmShaderPatcherId present_vertex_id = nullptr;
	SceGxmShaderPatcherId present_fragment_id = nullptr;
	SceGxmVertexProgram* color_vertex_program = nullptr;
	SceGxmFragmentProgram* color_fragment_program = nullptr;
	SceGxmVertexProgram* present_vertex_program = nullptr;
	SceGxmFragmentProgram* present_opaque_fragment_program = nullptr;
	SceGxmFragmentProgram* present_blend_fragment_program = nullptr;

	VitaGXM::MappedBlock vdm_ring;
	VitaGXM::MappedBlock vertex_ring;
	VitaGXM::MappedBlock fragment_ring;
	VitaGXM::MappedBlock fragment_usse_ring;
	VitaGXM::MappedBlock patcher_buffer;
	VitaGXM::MappedBlock patcher_vertex_usse;
	VitaGXM::MappedBlock patcher_fragment_usse;
	VitaGXM::MappedBlock gs_color_buffer;
	VitaGXM::MappedBlock staging_vertices;
	VitaGXM::MappedBlock staging_indices;
	VitaGXM::Display display;

	SceGxmNotification vertex_notification = {};
	SceGxmNotification fragment_notification = {};
	u32 vertex_used = 0;
	u32 index_used = 0;
	u32 target_bp = 0;
	u32 target_bw = 0;

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
	bool BeginGsScene();
	bool EnsureTarget(GSLocalMemory& memory, u32 bp, u32 bw);
	bool PreloadTarget(GSLocalMemory& memory, u32 bp, u32 bw);
	bool ReserveGeometry(u32 vertices, u32 indices, GxmVertex** vertex_out,
		u16** index_out, u32* first_vertex, u32* first_index);
	bool ReserveGsGeometry(u32 vertices, u32 indices, GxmVertex** vertex_out,
		u16** index_out, u32* first_vertex, u32* first_index);
	bool SubmitSprites(u32 first_vertex, u32 first_index, u32 index_count,
		const GSVector4i& clip);
	bool BeginDisplayScene();
	bool SubmitDisplayQuad(const GSVector4i* source_rect,
		const GSVector4& output_rect, u32 color, bool blend,
		SceGxmTextureFilter filter);
	bool AbortFrame();
	bool Present(VitaGxmGsState& owner);
	void ResetTarget();
	void InvalidateVideo();
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
	for (const SceGxmProgram* program : {color_v, color_f, present_v, present_f})
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
	if (!color_position || !color_color || !present_position || !present_color ||
		!present_uv)
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
	result = sceGxmShaderPatcherCreateFragmentProgram(patcher, present_fragment_id,
		SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4, SCE_GXM_MULTISAMPLE_NONE,
		&opaque, present_v, &present_opaque_fragment_program);
	if (result < 0 || !present_opaque_fragment_program)
		return Fail("create opaque present fragment program", result);

	// PCSX2 GSDevice::DoMerge() blends RC1 over the merge background. Sony's
	// api_libgxm/blending sample owns this matching source-alpha blend state.
	SceGxmBlendInfo source_over = {};
	source_over.colorFunc = SCE_GXM_BLEND_FUNC_ADD;
	source_over.alphaFunc = SCE_GXM_BLEND_FUNC_ADD;
	source_over.colorSrc = SCE_GXM_BLEND_FACTOR_SRC_ALPHA;
	source_over.colorDst = SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	source_over.alphaSrc = SCE_GXM_BLEND_FACTOR_SRC_ALPHA;
	source_over.alphaDst = SCE_GXM_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	source_over.colorMask = SCE_GXM_COLOR_MASK_ALL;
	result = sceGxmShaderPatcherCreateFragmentProgram(patcher, present_fragment_id,
		SCE_GXM_OUTPUT_REGISTER_FORMAT_UCHAR4, SCE_GXM_MULTISAMPLE_NONE,
		&source_over, present_v, &present_blend_fragment_program);
	if (result < 0 || !present_blend_fragment_program)
		return Fail("create blended present fragment program", result);
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
	if (!CreateRenderTarget(GS_TARGET_WIDTH, GS_TARGET_HEIGHT, 8,
			&gs_render_target) ||
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
	result = sceGxmColorSurfaceInit(&gs_color_surface,
		SCE_GXM_COLOR_FORMAT_U8U8U8U8_ABGR, SCE_GXM_COLOR_SURFACE_LINEAR,
		SCE_GXM_COLOR_SURFACE_SCALE_NONE, SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT,
		GS_TARGET_WIDTH, GS_TARGET_HEIGHT, GS_TARGET_WIDTH,
		gs_color_buffer.base);
	if (result < 0)
		return Fail("sceGxmColorSurfaceInit(GS target)", result);
	sceGxmColorSurfaceSetClip(&gs_color_surface, 0, 0,
		GS_TARGET_WIDTH - 1, GS_TARGET_HEIGHT - 1);

	// Sony's render_to_texture sample owns using a linear color surface as the
	// texture input of a subsequent scene in the same context.
	result = sceGxmTextureInitLinear(&gs_texture, gs_color_buffer.base,
		SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR, GS_TARGET_WIDTH,
		GS_TARGET_HEIGHT, 1);
	if (result < 0)
		return Fail("sceGxmTextureInitLinear(GS target)", result);
	if ((result = sceGxmTextureSetUAddrMode(&gs_texture,
			 SCE_GXM_TEXTURE_ADDR_CLAMP)) < 0 ||
		(result = sceGxmTextureSetVAddrMode(&gs_texture,
			 SCE_GXM_TEXTURE_ADDR_CLAMP)) < 0 ||
		(result = sceGxmTextureSetMinFilter(&gs_texture,
			 SCE_GXM_TEXTURE_FILTER_POINT)) < 0 ||
		(result = sceGxmTextureSetMagFilter(&gs_texture,
			 SCE_GXM_TEXTURE_FILTER_POINT)) < 0)
	{
		return Fail("configure GS target texture", result);
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

bool VitaGxmGsState::Impl::BeginGsScene()
{
	if (scene == Scene::GsTarget)
		return true;
	if (scene != Scene::None)
		return Fail("begin GS scene with another scene active",
			SCE_GXM_ERROR_WITHIN_SCENE);
	// Sony's scene dependency contract makes the following presentation scene
	// wait for all target fragment writes, even when that presentation falls
	// back to a background-only frame and does not sample the target.
	const int result = sceGxmBeginScene(context,
		SCE_GXM_SCENE_FRAGMENT_SET_DEPENDENCY, gs_render_target, nullptr,
		nullptr, nullptr, &gs_color_surface, &disabled_depth);
	if (result < 0)
		return Fail("sceGxmBeginScene(GS target)", result);
	scene = Scene::GsTarget;
	ConfigureRaster(GS_TARGET_WIDTH, GS_TARGET_HEIGHT);
	return true;
}

bool VitaGxmGsState::Impl::PreloadTarget(GSLocalMemory& memory, u32 bp, u32 bw)
{
	if (bw == 0 || bw * 64 > GS_TARGET_WIDTH)
		return false;
	if (!DrainSceneForCpuWrite())
		return false;

	u32* const pixels = static_cast<u32*>(gs_color_buffer.base);
	std::memset(pixels, 0, GS_TARGET_BYTES);
	const u32 width = bw * 64;
	for (u32 y = 0; y < GS_TARGET_HEIGHT; y++)
	{
		u32* const row = pixels + y * GS_TARGET_WIDTH;
		for (u32 x = 0; x < width; x++)
			row[x] = memory.ReadPixel32(static_cast<int>(x), static_cast<int>(y), bp, bw);
	}
	target_bp = bp;
	target_bw = bw;
	target_valid = true;
	target_stale = false;
	return true;
}

bool VitaGxmGsState::Impl::EnsureTarget(GSLocalMemory& memory, u32 bp, u32 bw)
{
	if (!BeginFrame())
		return false;
	if (!target_valid || target_stale || target_bp != bp || target_bw != bw)
	{
		if (!PreloadTarget(memory, bp, bw))
			return false;
	}
	return BeginGsScene();
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

bool VitaGxmGsState::Impl::ReserveGsGeometry(u32 vertices, u32 indices,
	GxmVertex** vertex_out, u16** index_out, u32* first_vertex, u32* first_index)
{
	if (vertices > GS_VERTEX_CAPACITY || indices > GS_INDEX_CAPACITY)
		return false;

	// Keep an exclusive tail for the background and PCRTC quads. If several
	// accepted GS draws fill the staging ring, retire that batch and continue
	// the same target instead of misclassifying a supported draw as fallback.
	if (vertices > GS_VERTEX_CAPACITY - vertex_used ||
		indices > GS_INDEX_CAPACITY - index_used)
	{
		if (!DrainSceneForCpuWrite() || !BeginGsScene())
			return false;
	}
	return ReserveGeometry(vertices, indices, vertex_out, index_out,
		first_vertex, first_index);
}

bool VitaGxmGsState::Impl::SubmitSprites(u32 first_vertex, u32 first_index,
	u32 index_count, const GSVector4i& clip)
{
	if (scene != Scene::GsTarget || clip.rempty())
		return false;
	const u32 left = static_cast<u32>(std::max(clip.x, 0));
	const u32 top = static_cast<u32>(std::max(clip.y, 0));
	const u32 right = static_cast<u32>(std::min(clip.z, static_cast<int>(GS_TARGET_WIDTH)));
	const u32 bottom = static_cast<u32>(std::min(clip.w, static_cast<int>(GS_TARGET_HEIGHT)));
	if (right <= left || bottom <= top)
		return true;
	sceGxmSetRegionClip(context, SCE_GXM_REGION_CLIP_OUTSIDE,
		left, top, right - 1, bottom - 1);
	sceGxmSetVertexProgram(context, color_vertex_program);
	sceGxmSetFragmentProgram(context, color_fragment_program);
	sceGxmSetVertexStream(context, 0,
		static_cast<GxmVertex*>(staging_vertices.base) + first_vertex);
	const int result = sceGxmDraw(context, SCE_GXM_PRIMITIVE_TRIANGLES,
		SCE_GXM_INDEX_FORMAT_U16,
		static_cast<u16*>(staging_indices.base) + first_index, index_count);
	return result >= 0 ? true : Fail("sceGxmDraw(GS sprites)", result);
}

bool VitaGxmGsState::Impl::BeginDisplayScene()
{
	if (scene != Scene::None)
		return Fail("begin display scene with another scene active",
			SCE_GXM_ERROR_WITHIN_SCENE);
	const int result = sceGxmBeginScene(context,
		SCE_GXM_SCENE_VERTEX_WAIT_FOR_DEPENDENCY, display_render_target,
		nullptr, nullptr, display.BackSyncObject(), display.BackColorSurface(),
		&disabled_depth);
	if (result < 0)
		return Fail("sceGxmBeginScene(display)", result);
	scene = Scene::Display;
	ConfigureRaster(VitaGXM::Display::Width, VitaGXM::Display::Height);
	return true;
}

bool VitaGxmGsState::Impl::SubmitDisplayQuad(const GSVector4i* source_rect,
	const GSVector4& output_rect, u32 color, bool blend,
	SceGxmTextureFilter filter)
{
	const bool textured = source_rect != nullptr;
	if (output_rect.z <= output_rect.x || output_rect.w <= output_rect.y ||
		output_rect.x < 0.0f || output_rect.y < 0.0f ||
		output_rect.z > static_cast<int>(VitaGXM::Display::Width) ||
		output_rect.w > static_cast<int>(VitaGXM::Display::Height) ||
		(textured && (source_rect->rempty() || source_rect->x < 0 ||
			source_rect->y < 0 ||
			source_rect->z > static_cast<int>(GS_TARGET_WIDTH) ||
			source_rect->w > static_cast<int>(GS_TARGET_HEIGHT))))
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
		u0 = static_cast<float>(source_rect->x) / GS_TARGET_WIDTH;
		v0 = static_cast<float>(source_rect->y) / GS_TARGET_HEIGHT;
		u1 = static_cast<float>(source_rect->z) / GS_TARGET_WIDTH;
		v1 = static_cast<float>(source_rect->w) / GS_TARGET_HEIGHT;
	}

	SetVertex(&vertices[0], DisplayNdcX(left), DisplayNdcY(top), color, u0, v0);
	SetVertex(&vertices[1], DisplayNdcX(right), DisplayNdcY(top), color, u1, v0);
	SetVertex(&vertices[2], DisplayNdcX(left), DisplayNdcY(bottom), color, u0, v1);
	SetVertex(&vertices[3], DisplayNdcX(right), DisplayNdcY(bottom), color, u1, v1);
	constexpr std::array<u16, INDICES> QUAD = {0, 1, 2, 2, 1, 3};
	for (u32 i = 0; i < INDICES; i++)
		indices[i] = QUAD[i];

	sceGxmSetRegionClip(context, SCE_GXM_REGION_CLIP_OUTSIDE, 0, 0,
		VitaGXM::Display::Width - 1, VitaGXM::Display::Height - 1);
	sceGxmSetVertexStream(context, 0,
		static_cast<GxmVertex*>(staging_vertices.base) + first_vertex);
	if (textured)
	{
		sceGxmSetVertexProgram(context, present_vertex_program);
		sceGxmSetFragmentProgram(context,
			blend ? present_blend_fragment_program : present_opaque_fragment_program);
		int filter_result = sceGxmTextureSetMinFilter(&gs_texture, filter);
		if (filter_result >= 0)
			filter_result = sceGxmTextureSetMagFilter(&gs_texture, filter);
		if (filter_result < 0)
			return Fail("configure GS presentation filter", filter_result);
		const int bind_result = sceGxmSetFragmentTexture(context, 0, &gs_texture);
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
	target_stale = true;
	if (scene != Scene::None && !EndScene(true))
		ready = false;
	if (notification_pending && !WaitForNotifications())
		ready = false;
	vertex_used = 0;
	index_used = 0;
	frame_active = false;
	return false;
}

bool VitaGxmGsState::Impl::Present(VitaGxmGsState& owner)
{
	if (!ready || !owner.m_regs || !BeginFrame())
		return false;

	// PCSX2 owners: GSRenderer::Merge() and CalculateDrawDstRect(). This first
	// native envelope implements one RC1 PSMCT32 output, constant-alpha merge,
	// full-frame FIELD-mode input, and the standard final aspect/filter step.
	// Other PCRTC composition is shown as a background-only fallback.
	GSVector4i source_rect = GSVector4i::zero();
	GSVector4 output_rect(0.0f, 0.0f,
		static_cast<float>(VitaGXM::Display::Width),
		static_cast<float>(VitaGXM::Display::Height));
	bool source_ready = false;
	const GSState::GSPCRTCRegs::PCRTCDisplay& circuit =
		owner.PCRTCDisplays.PCRTCDisplays[0];
	const GSVector2i resolution = owner.PCRTCDisplays.GetResolution();
	const bool offset_changed_0 =
		owner.PCRTCDisplays.PCRTCDisplays[0].prevFramebufferOffsets.y !=
		owner.PCRTCDisplays.PCRTCDisplays[0].framebufferOffsets.y;
	const bool offset_changed_1 =
		owner.PCRTCDisplays.PCRTCDisplays[1].prevFramebufferOffsets.y !=
		owner.PCRTCDisplays.PCRTCDisplays[1].framebufferOffsets.y;
	const bool game_deinterlacing = offset_changed_0 != offset_changed_1;
	const bool no_crop = GSConfig.Crop[0] == 0 && GSConfig.Crop[1] == 0 &&
		GSConfig.Crop[2] == 0 && GSConfig.Crop[3] == 0;
	const bool standard_postprocess = no_crop && !GSConfig.IntegerScaling &&
		GSConfig.StretchY == 100.0f && !GSConfig.FXAA && !GSConfig.ShadeBoost &&
		GSConfig.CASMode == GSCASMode::Disabled && GSConfig.TVShader == 0 &&
		(GSConfig.LinearPresent == GSPostBilinearMode::Off ||
		 GSConfig.LinearPresent == GSPostBilinearMode::BilinearSmooth);
	// GSRenderer::Merge() resolves Automatic + full-frame input + no game
	// deinterlacing/SCANMSK to mode -1. GSDevice::Interlace() deliberately leaves
	// m_merge untouched for that default mode, even when the physical output is
	// interlaced and PCRTC toggles fields. Off is the other exact raw-merge case.
	// FFMD and observed per-circuit/game field motion still require a real
	// deinterlacer and remain outside this envelope.
	const bool automatic_raw_merge =
		GSConfig.InterlaceMode == GSInterlaceMode::Automatic &&
		!owner.PCRTCDisplays.FFMD && !game_deinterlacing &&
		owner.m_scanmask_used == 0;
	const bool interlace_supported = !owner.PCRTCDisplays.FFMD &&
		!game_deinterlacing && owner.m_scanmask_used == 0 &&
		(automatic_raw_merge || GSConfig.InterlaceMode == GSInterlaceMode::Off);
	const bool circuit_fills_merge = resolution.x > 0 && resolution.y > 0 &&
		circuit.displayRect.x == 0 && circuit.displayRect.y == 0 &&
		circuit.displayRect.width() == resolution.x &&
		circuit.displayRect.height() == resolution.y;
	const bool rc1_selected = circuit.enabled &&
		(!(owner.m_regs->PMODE.MMOD == 1 && owner.m_regs->PMODE.ALP == 0) ||
		 owner.m_regs->PMODE.AMOD == 0);
	const bool present_contract_supported = owner.m_regs->PMODE.EN1 &&
		!owner.m_regs->PMODE.EN2 && owner.m_regs->PMODE.MMOD == 1 &&
		owner.m_regs->EXTWRITE.WRITE == 0 && circuit.PSM == PSMCT32 &&
		circuit.FBW > 0 &&
		circuit.FBW * 64 <= static_cast<int>(GS_TARGET_WIDTH) &&
		circuit_fills_merge && interlace_supported && standard_postprocess;
	if (present_contract_supported && rc1_selected)
	{
		source_rect = circuit.framebufferRect;
		const bool valid = circuit.PSM == PSMCT32 && circuit.FBW > 0 &&
			circuit.FBW * 64 <= static_cast<int>(GS_TARGET_WIDTH) &&
			source_rect.width() == resolution.x &&
			source_rect.height() == resolution.y &&
			source_rect.x >= 0 && source_rect.y >= 0 &&
			source_rect.z <= static_cast<int>(GS_TARGET_WIDTH) &&
			source_rect.w <= static_cast<int>(GS_TARGET_HEIGHT) &&
			!source_rect.rempty() &&
			GSLocalMemory::GetStartBlockAddress(circuit.Block(), circuit.FBW,
				circuit.PSM, source_rect) >= static_cast<u32>(circuit.Block()) &&
			GSLocalMemory::GetUnwrappedEndBlockAddress(circuit.Block(), circuit.FBW,
				circuit.PSM, source_rect) < GS_MAX_BLOCKS;
		if (valid)
		{
			if (scene == Scene::GsTarget &&
				(target_bp != static_cast<u32>(circuit.Block()) ||
				 target_bw != static_cast<u32>(circuit.FBW)))
			{
				if (!DrainSceneForCpuWrite())
					return AbortFrame();
			}
			if (!target_valid || target_stale ||
				target_bp != static_cast<u32>(circuit.Block()) ||
				target_bw != static_cast<u32>(circuit.FBW))
			{
				if (!PreloadTarget(owner.m_mem,
						static_cast<u32>(circuit.Block()),
						static_cast<u32>(circuit.FBW)))
				{
					return AbortFrame();
				}
			}
			source_ready = true;
		}
	}
	if (present_contract_supported)
	{
		const bool progressive = owner.GetVideoMode() == GSVideoMode::SDTV_480P;
		output_rect = CalculatePresentationRect(progressive);
	}
	const GSVector4 full_display(0.0f, 0.0f,
		static_cast<float>(VitaGXM::Display::Width),
		static_cast<float>(VitaGXM::Display::Height));
	const u32 background_color =
		(owner.m_regs->BGCOLOR.U32[0] & 0x00ffffffu) |
		(static_cast<u32>(owner.m_regs->PMODE.ALP) << 24);
	const u32 rc1_color = 0x00ffffffu |
		(static_cast<u32>(owner.m_regs->PMODE.ALP) << 24);
	const SceGxmTextureFilter filter =
		GSConfig.LinearPresent == GSPostBilinearMode::Off ?
			SCE_GXM_TEXTURE_FILTER_POINT : SCE_GXM_TEXTURE_FILTER_LINEAR;

	if (!EndScene(false) || !BeginDisplayScene() ||
		!SubmitDisplayQuad(nullptr, full_display, background_color, false,
			SCE_GXM_TEXTURE_FILTER_POINT) ||
		(source_ready && !SubmitDisplayQuad(&source_rect, output_rect, rc1_color,
			true, filter)) || !EndScene(true))
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
	frame_active = false;
	return true;
}

void VitaGxmGsState::Impl::InvalidateVideo()
{
	// GSState calls this before committing the EE->GS or local move. End an
	// active target scene now; the subsequent Draw()/Present() reload occurs
	// after canonical memory contains the new bytes.
	if (scene == Scene::GsTarget)
	{
		if (!DrainSceneForCpuWrite())
			ready = false;
	}
	target_stale = true;
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
	target_valid = false;
	target_stale = true;
	target_bp = 0;
	target_bw = 0;
	vertex_used = 0;
	index_used = 0;
	frame_active = false;
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
	release_fragment(present_blend_fragment_program,
		"sceGxmShaderPatcherReleaseFragmentProgram(present blend)");
	release_fragment(present_opaque_fragment_program,
		"sceGxmShaderPatcherReleaseFragmentProgram(present opaque)");
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
	destroy_target(gs_render_target, "sceGxmDestroyRenderTarget(GS)");
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
	: m_impl(std::make_unique<Impl>(enable_native_presenter))
{
}

VitaGxmGsState::~VitaGxmGsState() = default;

bool VitaGxmGsState::IsNativePresenterReady() const
{
	return m_impl && m_impl->ready;
}

void VitaGxmGsState::Present()
{
	if (m_impl && m_impl->ready)
		m_impl->Present(*this);
}

void VitaGxmGsState::VSync()
{
	// PCSX2 owner: GSRenderer::VSync() -> Merge(). SCANMSK influences the
	// current merge and then ages once per processed VSync. Unsupported GXM
	// presentation still processes a background frame, so it must age too or a
	// single SCANMSK write would permanently suppress otherwise-supported output.
	const bool merge_output = HasPcsx2MergeOutput();
	const bool processed = m_impl &&
		(!m_impl->enabled || (m_impl->ready && m_impl->Present(*this)));
	if (processed && merge_output && m_scanmask_used)
		m_scanmask_used--;
}

void VitaGxmGsState::Reset(bool hardware_reset)
{
	GSState::Reset(hardware_reset);
	if (m_impl && m_impl->ready)
		m_impl->ResetTarget();
}

void VitaGxmGsState::Draw()
{
	if (!m_impl || !m_impl->ready || !PRIM || !m_context || !m_draw_env ||
		!m_vertex || !m_index)
	{
		return;
	}

	const GIFRegFRAME& frame = m_context->FRAME;
	const GIFRegTEST& test = m_context->TEST;
	u32 alpha_test_frame_mask = frame.FBMSK;
	u32 alpha_test_depth_mask =
		(m_context->ZBUF.ZMSK || !test.ZTE) ? 0xffffffffu : 0u;
	// PCSX2 owner: GSState::TryAlphaTest(). For a flat, untextured sprite
	// batch, its vertex-alpha range can prove the test has no effect on the
	// framebuffer even when ATE is set. A partial RGB_ONLY result remains out
	// of this envelope because it needs destination-alpha preservation.
	const bool alpha_test_resolved = !test.ATE ||
		TryAlphaTest(alpha_test_frame_mask, alpha_test_depth_mask);
	const bool alpha_test_keeps_full_frame = alpha_test_frame_mask == 0;
	// ALPHA computes (A - B) * C + D. A == B and D == Cs is exactly
	// source replacement. The common (Cs - Cd) * As + Cd form is also source
	// replacement for a flat GS alpha of 0x80 because GS alpha factors use 7
	// fractional bits. Both cases therefore keep GXM blending disabled; other
	// factors remain outside the exact native envelope.
	const bool blend_register_is_source = !PRIM->ABE ||
		(m_context->ALPHA.A == m_context->ALPHA.B &&
		 m_context->ALPHA.D == 0);
	const bool blend_register_is_source_over = PRIM->ABE &&
		m_context->ALPHA.A == 0 && m_context->ALPHA.B == 1 &&
		m_context->ALPHA.C == 0 && m_context->ALPHA.D == 1;
	// GSRendererSW::GetScanlineGlobalData() forces the depth mask to all ones
	// when ZTE is clear, regardless of ZMSK. With ZTE enabled, ALWAYS is still
	// side-effect-free only when the depth write is masked.
	const bool depth_is_noop = !test.ZTE ||
		(test.ZTST == ZTST_ALWAYS && m_context->ZBUF.ZMSK);
	const bool state_supported =
		PRIM->PRIM == GS_SPRITE && !PRIM->TME && !PRIM->FGE &&
		(blend_register_is_source || blend_register_is_source_over) &&
		!PRIM->AA1 && frame.PSM == PSMCT32 && frame.FBW > 0 &&
		frame.FBW * 64 <= GS_TARGET_WIDTH && frame.FBMSK == 0 &&
		alpha_test_resolved && alpha_test_keeps_full_frame && !test.DATE &&
		depth_is_noop &&
		m_draw_env->SCANMSK.MSK == 0 && !m_draw_env->DTHE.DTHE &&
		!m_draw_env->PABE.PABE && !m_context->FBA.FBA &&
		(m_index->tail % 2) == 0;
	const u32 sprite_count = m_index->tail / 2;
	if (!state_supported || sprite_count == 0)
	{
		return;
	}

	const GSVector4i clip = m_context->scissor.in;
	u32 visible_count = 0;
	for (u32 sprite = 0; sprite < sprite_count; sprite++)
	{
		const u16 first_index = m_index->buff[sprite * 2];
		const u16 second_index = m_index->buff[sprite * 2 + 1];
		if (first_index >= m_vertex->next || second_index >= m_vertex->next)
		{
			return;
		}
		const GSVertex& first = m_vertex->buff[first_index];
		const GSVertex& second = m_vertex->buff[second_index];
		if (blend_register_is_source_over && second.RGBAQ.A != 0x80)
		{
			return;
		}
		const int x0_fixed = static_cast<int>(first.XYZ.X) -
			static_cast<int>(m_context->XYOFFSET.OFX);
		const int y0_fixed = static_cast<int>(first.XYZ.Y) -
			static_cast<int>(m_context->XYOFFSET.OFY);
		const int x1_fixed = static_cast<int>(second.XYZ.X) -
			static_cast<int>(m_context->XYOFFSET.OFX);
		const int y1_fixed = static_cast<int>(second.XYZ.Y) -
			static_cast<int>(m_context->XYOFFSET.OFY);
		if (((x0_fixed | y0_fixed | x1_fixed | y1_fixed) & 0xf) != 0 ||
			x0_fixed < 0 || y0_fixed < 0 || x1_fixed < 0 || y1_fixed < 0 ||
			x0_fixed > static_cast<int>(GS_TARGET_WIDTH * 16) ||
			x1_fixed > static_cast<int>(GS_TARGET_WIDTH * 16) ||
			y0_fixed > static_cast<int>(GS_TARGET_HEIGHT * 16) ||
			y1_fixed > static_cast<int>(GS_TARGET_HEIGHT * 16))
		{
			return;
		}
		GSVector4i rect(std::min(x0_fixed, x1_fixed) / 16,
			std::min(y0_fixed, y1_fixed) / 16,
			std::max(x0_fixed, x1_fixed) / 16,
			std::max(y0_fixed, y1_fixed) / 16);
		rect = rect.rintersect(clip);
		if (rect.rempty())
			continue;
		const u32 start_block = GSLocalMemory::GetStartBlockAddress(
			frame.Block(), frame.FBW, frame.PSM, rect);
		const u32 end_block = GSLocalMemory::GetUnwrappedEndBlockAddress(
			frame.Block(), frame.FBW, frame.PSM, rect);
		if (start_block < frame.Block() || end_block >= GS_MAX_BLOCKS)
		{
			return;
		}
		visible_count++;
	}
	if (visible_count > GS_VERTEX_CAPACITY / 4 ||
		visible_count > GS_INDEX_CAPACITY / 6)
	{
		return;
	}
	if (visible_count == 0)
	{
		return;
	}

	if (!m_impl->EnsureTarget(m_mem, frame.Block(), frame.FBW))
	{
		m_impl->AbortFrame();
		return;
	}
	GxmVertex* vertices = nullptr;
	u16* indices = nullptr;
	u32 first_vertex = 0;
	u32 first_index = 0;
	if (!m_impl->ReserveGsGeometry(visible_count * 4, visible_count * 6,
			&vertices, &indices, &first_vertex, &first_index))
	{
		m_impl->AbortFrame();
		return;
	}

	u32 visible_sprite = 0;
	for (u32 sprite = 0; sprite < sprite_count; sprite++)
	{
		const GSVertex& first = m_vertex->buff[m_index->buff[sprite * 2]];
		const GSVertex& second = m_vertex->buff[m_index->buff[sprite * 2 + 1]];
		const int x0 = (static_cast<int>(first.XYZ.X) -
			static_cast<int>(m_context->XYOFFSET.OFX)) / 16;
		const int y0 = (static_cast<int>(first.XYZ.Y) -
			static_cast<int>(m_context->XYOFFSET.OFY)) / 16;
		const int x1 = (static_cast<int>(second.XYZ.X) -
			static_cast<int>(m_context->XYOFFSET.OFX)) / 16;
		const int y1 = (static_cast<int>(second.XYZ.Y) -
			static_cast<int>(m_context->XYOFFSET.OFY)) / 16;
		const GSVector4i rect = GSVector4i(std::min(x0, x1), std::min(y0, y1),
			std::max(x0, x1), std::max(y0, y1)).rintersect(clip);
		if (rect.rempty())
			continue;
		const u32 color = static_cast<u32>(second.RGBAQ.R) |
			(static_cast<u32>(second.RGBAQ.G) << 8) |
			(static_cast<u32>(second.RGBAQ.B) << 16) |
			(static_cast<u32>(second.RGBAQ.A) << 24);
		GxmVertex* const quad = vertices + visible_sprite * 4;
		SetVertex(&quad[0], TargetNdcX(rect.left), TargetNdcY(rect.top), color);
		SetVertex(&quad[1], TargetNdcX(rect.right), TargetNdcY(rect.top), color);
		SetVertex(&quad[2], TargetNdcX(rect.left), TargetNdcY(rect.bottom), color);
		SetVertex(&quad[3], TargetNdcX(rect.right), TargetNdcY(rect.bottom), color);
		const u16 base = static_cast<u16>(visible_sprite * 4);
		u16* const quad_indices = indices + visible_sprite * 6;
		quad_indices[0] = base;
		quad_indices[1] = base + 1;
		quad_indices[2] = base + 2;
		quad_indices[3] = base + 2;
		quad_indices[4] = base + 1;
		quad_indices[5] = base + 3;
		visible_sprite++;
	}
	if (!m_impl->SubmitSprites(first_vertex, first_index, visible_count * 6, clip))
	{
		m_impl->AbortFrame();
		return;
	}

	// Commit canonical GS memory only after libgxm accepted the matching draw.
	// This preserves the GSState read/download oracle on submission failures.
	for (u32 sprite = 0; sprite < sprite_count; sprite++)
	{
		const GSVertex& first = m_vertex->buff[m_index->buff[sprite * 2]];
		const GSVertex& second = m_vertex->buff[m_index->buff[sprite * 2 + 1]];
		const int x0 = (static_cast<int>(first.XYZ.X) -
			static_cast<int>(m_context->XYOFFSET.OFX)) / 16;
		const int y0 = (static_cast<int>(first.XYZ.Y) -
			static_cast<int>(m_context->XYOFFSET.OFY)) / 16;
		const int x1 = (static_cast<int>(second.XYZ.X) -
			static_cast<int>(m_context->XYOFFSET.OFX)) / 16;
		const int y1 = (static_cast<int>(second.XYZ.Y) -
			static_cast<int>(m_context->XYOFFSET.OFY)) / 16;
		const GSVector4i rect = GSVector4i(std::min(x0, x1), std::min(y0, y1),
			std::max(x0, x1), std::max(y0, y1)).rintersect(clip);
		if (rect.rempty())
			continue;
		const u32 color = static_cast<u32>(second.RGBAQ.R) |
			(static_cast<u32>(second.RGBAQ.G) << 8) |
			(static_cast<u32>(second.RGBAQ.B) << 16) |
			(static_cast<u32>(second.RGBAQ.A) << 24);
		for (int y = rect.top; y < rect.bottom; y++)
		{
			for (int x = rect.left; x < rect.right; x++)
				m_mem.WritePixel32(x, y, color, frame.Block(), frame.FBW);
		}
	}
}

void VitaGxmGsState::InvalidateVideoMem(const GIFRegBITBLTBUF& blit,
	const GSVector4i& rect)
{
	(void)blit;
	(void)rect;
	if (m_impl && m_impl->ready)
		m_impl->InvalidateVideo();
}

void VitaGxmGsState::InvalidateLocalMem(const GIFRegBITBLTBUF& blit,
	const GSVector4i& rect, bool clut)
{
	// Supported GXM sprites are mirrored immediately through GSLocalMemory, so
	// downloads and local moves already observe canonical bytes. This override
	// intentionally performs no GPU readback.
	(void)blit;
	(void)rect;
	(void)clut;
}

#endif
