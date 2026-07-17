// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "vita/GSDeviceGXM.h"

#if !defined(VITASX2_QEMU_VALIDATION)

#include "GS/Renderers/Common/GSDevice.h"
#include "GS/Renderers/Common/GSVertex.h"
#include "common/Console.h"
#include "vita/VitaGxmArena.h"
#include "vita/VitaGxmDisplay.h"
#include "vita/VitaGxmMemory.h"
#include "vita/VitaGxmTexture.h"

#include <psp2/gxm.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
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
	extern const SceGxmProgram _binary_vitasx2_tfx_fast_f_gxp_start;
	extern const SceGxmProgram _binary_vitasx2_tfx_untextured_f_gxp_start;
	extern const SceGxmProgram _binary_vitasx2_tfx_source_f_gxp_start;
	extern const SceGxmProgram _binary_vitasx2_tfx_source_direct_f_gxp_start;
	extern const SceGxmProgram _binary_vitasx2_tfx_source_direct_modulate_f_gxp_start;
	extern const SceGxmProgram _binary_vitasx2_tfx_source_direct_modulate_af_f_gxp_start;
	extern const SceGxmProgram _binary_vitasx2_tfx_source_untextured_f_gxp_start;
}

namespace
{
	constexpr u32 PATCHER_BUFFER_BYTES = 512 * 1024;
	constexpr u32 PATCHER_VERTEX_USSE_BYTES = 256 * 1024;
	constexpr u32 PATCHER_FRAGMENT_USSE_BYTES = 2 * 1024 * 1024;
	constexpr u32 GEOMETRY_VERTEX_BYTES = 6 * 1024 * 1024;
	constexpr u32 GEOMETRY_INDEX_BYTES = 512 * 1024;
	constexpr u32 MAX_STAGED_INDICES = 65532;
	constexpr u32 MAX_RENDER_TARGETS = 48;
	constexpr size_t MAX_TFX_PATCHED_PROGRAMS = 128;

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

struct GSDeviceGXM::Impl final : public VitaGXM::TextureOwner
{
	struct RenderTargetEntry
	{
		u32 width = 0;
		u32 height = 0;
		SceGxmRenderTarget* target = nullptr;
	};

	struct ProgramUniforms
	{
		const SceGxmProgramParameter* vertex_scale_offset = nullptr;
		const SceGxmProgramParameter* selector[7]{};
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

	SceGxmShaderPatcherId tfx_vertex_id = nullptr;
	SceGxmShaderPatcherId tfx_fragment_id = nullptr;
	SceGxmShaderPatcherId tfx_fast_fragment_id = nullptr;
	SceGxmShaderPatcherId tfx_untextured_fragment_id = nullptr;
	SceGxmShaderPatcherId tfx_source_fragment_id = nullptr;
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
	ProgramUniforms fast_uniforms;
	ProgramUniforms untextured_uniforms;
	ProgramUniforms source_uniforms;
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
	bool tfx_patched_program_limit_logged = false;

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
	bool CreateGeometry();
	bool CreateRenderTarget(u32 width, u32 height, SceGxmRenderTarget** target);
	SceGxmRenderTarget* GetRenderTarget(u32 width, u32 height);
	bool EndScene(bool finish);
	bool Finish();
	bool CommitClear(VitaGXM::GSTextureGXM& texture);
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
		bool fast_fragment, bool source_only_fragment,
		bool source_direct_fragment,
		bool source_direct_modulate_fragment,
		bool source_direct_modulate_af_fragment,
		bool untextured_fragment);
	bool UploadTfxUniforms(const GSHWDrawConfig& config,
		const GSHWDrawConfig::PSSelector& ps, VitaGXM::GSTextureGXM* source,
		bool fast_fragment, bool source_only_fragment,
		bool source_direct_fragment,
		bool source_direct_modulate_fragment,
		bool source_direct_modulate_af_fragment,
		bool untextured_fragment);
	bool CanUseFastTfx(const GSHWDrawConfig& config) const;
	bool CanUseSourceOnlyTfx(const GSHWDrawConfig& config) const;
	bool CanUseSourceDirectTfx(const GSHWDrawConfig& config) const;
	bool CanUseSourceDirectModulateTfx(const GSHWDrawConfig& config) const;
	bool CanUseSourceDirectModulateAfTfx(const GSHWDrawConfig& config) const;
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
	return result >= 0 ? true : Fail("staged index allocation", result);
}

bool GSDeviceGXM::Impl::CreateRenderTarget(u32 width, u32 height,
	SceGxmRenderTarget** target)
{
	if (!target || width == 0 || height == 0 || width > 4096 || height > 4096)
		return false;
	SceGxmRenderTargetParams params{};
	params.width = static_cast<u16>(width);
	params.height = static_cast<u16>(height);
	params.scenesPerFrame = 1;
	params.multisampleMode = SCE_GXM_MULTISAMPLE_NONE;
	params.driverMemBlock = static_cast<SceUID>(-1);
	const int result = sceGxmCreateRenderTarget(&params, target);
	return (result >= 0 && *target) ? true : Fail("sceGxmCreateRenderTarget",
		result < 0 ? result : SCE_GXM_ERROR_INVALID_POINTER);
}

SceGxmRenderTarget* GSDeviceGXM::Impl::GetRenderTarget(u32 width, u32 height)
{
	for (const RenderTargetEntry& entry : render_targets)
	{
		if (entry.width == width && entry.height == height)
			return entry.target;
	}
	if (render_targets.size() >= MAX_RENDER_TARGETS)
	{
		Reject("more than 48 live GXM render-target geometries");
		return nullptr;
	}
	SceGxmRenderTarget* target = nullptr;
	if (!CreateRenderTarget(width, height, &target))
		return nullptr;
	render_targets.push_back({width, height, target});
	return target;
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
		!CreateRenderTarget(VitaGXM::Display::Width, VitaGXM::Display::Height,
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
	ready = true;
	Console.WriteLn("GXM GS: direct GSRendererHW backend ready (960x544 display).");
	return true;
}

bool GSDeviceGXM::Impl::CreatePrograms()
{
	const SceGxmProgram* const tfx_v = &_binary_vitasx2_tfx_v_gxp_start;
	const SceGxmProgram* const tfx_f = &_binary_vitasx2_tfx_f_gxp_start;
	const SceGxmProgram* const tfx_fast_f =
		&_binary_vitasx2_tfx_fast_f_gxp_start;
	const SceGxmProgram* const tfx_untextured_f =
		&_binary_vitasx2_tfx_untextured_f_gxp_start;
	const SceGxmProgram* const tfx_source_f =
		&_binary_vitasx2_tfx_source_f_gxp_start;
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
	for (const SceGxmProgram* program : {tfx_v, tfx_f, tfx_fast_f,
		tfx_untextured_f, tfx_source_f, tfx_source_direct_f,
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
		!register_program(tfx_fast_f, &tfx_fast_fragment_id,
			"register fast TFX fragment program") ||
		!register_program(tfx_untextured_f, &tfx_untextured_fragment_id,
			"register untextured TFX fragment program") ||
		!register_program(tfx_source_f, &tfx_source_fragment_id,
			"register source-only TFX fragment program") ||
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
	};
	for (u32 i = 0; i < selector_names.size(); i++)
	{
		uniforms.selector[i] = parameter(tfx_f, selector_names[i],
			SCE_GXM_PARAMETER_CATEGORY_UNIFORM);
	}
	load_variant_uniforms(tfx_fast_f, &fast_uniforms);
	load_variant_uniforms(tfx_untextured_f, &untextured_uniforms);
	load_variant_uniforms(tfx_source_f, &source_uniforms);
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

bool GSDeviceGXM::Impl::EndScene(bool finish)
{
	if (!scene_active)
		return true;
	const int end_result = sceGxmEndScene(context, nullptr, nullptr);
	if (end_result < 0)
	{
		ready = false;
		return Fail("sceGxmEndScene", end_result);
	}
	scene_active = false;
	scene_is_display = false;
	scene_rt = nullptr;
	scene_ds = nullptr;
	completed_scene_serial = finish ? scene_serial : completed_scene_serial;
	if (!finish)
		return true;
	sceGxmFinish(context);
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
		static_cast<float>(height) * 0.5f, 0.5f, 0.5f);
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
	if (!Finish())
		return false;

	if (texture.IsDepthStencil())
	{
		const VitaGXM::TextureLevelLayout* layout = texture.Level(0);
		if (!layout || !texture.LevelData(0))
			return false;
		const float depth = texture.GetClearDepth();
		for (u32 y = 0; y < layout->height; y++)
		{
			float* row = reinterpret_cast<float*>(
				static_cast<u8*>(texture.LevelData(0)) +
				static_cast<size_t>(y) * layout->pitch);
			std::fill_n(row, layout->width, depth);
		}
		if (texture.StencilData())
		{
			for (u32 y = 0; y < layout->height; y++)
				std::memset(static_cast<u8*>(texture.StencilData()) +
					static_cast<size_t>(y) * texture.StencilPitch(), 0, layout->width);
		}
	}
	else
	{
		const VitaGXM::TextureLevelLayout* layout = texture.Level(0);
		if (!layout || !texture.LevelData(0))
			return false;
		const VitaGXM::TextureFormatInfo& format = texture.NativeFormat();
		if (format.bytes_per_pixel != 4)
			return Reject("lazy clear of non-32-bit color target");
		for (u32 y = 0; y < layout->height; y++)
		{
			u32* row = reinterpret_cast<u32*>(
				static_cast<u8*>(texture.LevelData(0)) +
				static_cast<size_t>(y) * layout->pitch);
			std::fill_n(row, layout->width, texture.GetClearColor());
		}
	}
	texture.SetState(GSTexture::State::Dirty);
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
		aligned_index_offset + index_bytes <= geometry_indices.size;
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
	return result >= 0 ? true : Fail("sceGxmDraw(scissor mask)", result);
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
	u8* target = static_cast<u8*>(texture.LevelData(level)) +
		static_cast<size_t>(destination.y) * layout->pitch +
		static_cast<size_t>(destination.x) * bpp;
	const u8* input = static_cast<const u8*>(source.Data());
	for (int y = 0; y < destination.height(); y++)
	{
		std::memcpy(target + static_cast<size_t>(y) * layout->pitch,
			input + static_cast<size_t>(y) * source_pitch, row_bytes);
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
	const u8* input = static_cast<const u8*>(texture.LevelData(level)) +
		static_cast<size_t>(source.y) * layout->pitch +
		static_cast<size_t>(source.x) * bpp;
	u8* output = static_cast<u8*>(destination) +
		static_cast<size_t>(destination_rect.y) * destination_pitch +
		static_cast<size_t>(destination_rect.x) * bpp;
	for (int y = 0; y < source.height(); y++)
	{
		std::memcpy(output + static_cast<size_t>(y) * destination_pitch,
			input + static_cast<size_t>(y) * layout->pitch, row_bytes);
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
	// PCSX2 owner: tfx_fs.glsl::SW_BLEND_NEEDS_RT and
	// PSSelector::IsFeedbackLoopRT(). A software blend is source-only exactly
	// when none of A, B, C, or D selects Cd/Ad and no other shader feature needs
	// the render target. PABE's dual-source contract is retained on the general
	// path until GXM has a proven secondary-color output.
	return config.ps.IsSWBlending() && !config.ps.IsFeedbackLoopRT() &&
		!config.ps.pabe && CanPatchTfxBlend(config);
}

bool GSDeviceGXM::Impl::CanUseSourceDirectTfx(
	const GSHWDrawConfig& config) const
{
	// PCSX2 owner: tfx_fs.glsl::sample_c. With a 32-bit source, no manual
	// linear filtering, and no target-region remap, sample_c is one ordinary
	// texture lookup. GXM's sampler owns repeat/clamp and host linear filtering.
	return config.vs.tme && config.ps.aem_fmt == 0 && !config.ps.ltf &&
		!config.ps.region_rect;
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
	return CanUseSourceDirectModulateTfx(config) && !config.ps.fst &&
		!config.ps.fog && config.ps.atst == GSHWDrawConfig::PS_ATST::NONE &&
		!config.ps.fba && !config.ps.rta_source_correction &&
		!config.ps.colclip && !config.ps.blend_mix && !config.ps.fixed_one_a &&
		config.ps.blend_a == 0 && config.ps.blend_b == 2 &&
		config.ps.blend_c == 2 && config.ps.blend_d == 2;
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
	const int result = sceGxmShaderPatcherCreateFragmentProgram(patcher,
		fragment_id,
		SCE_GXM_OUTPUT_REGISTER_FORMAT_DECLARED,
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
	bool fast_fragment, bool source_only_fragment, bool source_direct_fragment,
	bool source_direct_modulate_fragment,
	bool source_direct_modulate_af_fragment,
	bool untextured_fragment)
{
	const ProgramUniforms& fragment_uniforms = source_only_fragment ?
		(untextured_fragment ? source_untextured_uniforms :
			(source_direct_modulate_af_fragment ?
				source_direct_modulate_af_uniforms :
			(source_direct_modulate_fragment ?
				source_direct_modulate_uniforms :
			(source_direct_fragment ? source_direct_uniforms : source_uniforms)))) :
		(untextured_fragment ? untextured_uniforms :
			(fast_fragment ? fast_uniforms : uniforms));
	void* vertex_buffer = nullptr;
	int result = sceGxmReserveVertexDefaultUniformBuffer(context, &vertex_buffer);
	if (result < 0 || !vertex_buffer)
		return Fail("reserve TFX vertex uniforms",
			result < 0 ? result : SCE_GXM_ERROR_INVALID_POINTER);
	const std::array<float, 8> vertex_values = {
		config.cb_vs.vertex_scale.x, config.cb_vs.vertex_scale.y,
		config.cb_vs.vertex_offset.x, config.cb_vs.vertex_offset.y,
		config.cb_vs.texture_scale.x, config.cb_vs.texture_scale.y,
		config.cb_vs.texture_offset.x, config.cb_vs.texture_offset.y};
	result = sceGxmSetUniformDataF(vertex_buffer, uniforms.vertex_scale_offset,
		0, vertex_values.size(), vertex_values.data());
	if (result < 0)
		return Fail("upload TFX vertex uniforms", result);

	void* fragment_buffer = nullptr;
	result = sceGxmReserveFragmentDefaultUniformBuffer(context, &fragment_buffer);
	if (result < 0 || !fragment_buffer)
		return Fail("reserve TFX fragment uniforms",
			result < 0 ? result : SCE_GXM_ERROR_INVALID_POINTER);
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
		if (!fragment_uniforms.selector[i])
			continue;
		result = sceGxmSetUniformDataF(fragment_buffer,
			fragment_uniforms.selector[i], 0, 4, selectors[i]);
		if (result < 0)
			return Fail("upload TFX selector", result);
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
	if (!upload4(fragment_uniforms.fog_color_aref,
			config.cb_ps.FogColor_AREF.F32,
			"upload FogColor_AREF") ||
		!upload4(fragment_uniforms.texture_size, config.cb_ps.WH.F32,
			"upload texture size") ||
		!upload4(fragment_uniforms.texture_alpha,
			config.cb_ps.TA_MaxDepth_Af.F32,
			"upload texture alpha") ||
		!upload4(fragment_uniforms.half_texel, config.cb_ps.HalfTexel.F32,
			"upload half texel") ||
		!upload4(fragment_uniforms.st_range, config.cb_ps.STRange.F32,
			"upload ST range"))
	{
		return false;
	}
	const float st_scale[4] = {config.cb_ps.STScale.x, config.cb_ps.STScale.y,
		0.0f, 0.0f};
	if (!upload4(fragment_uniforms.st_scale, st_scale, "upload ST scale"))
		return false;
	const VitaGXM::GSTextureGXM* native_source = source ? source : white_texture.get();
	const float native_size[4] = {
		static_cast<float>(native_source->GetWidth()),
		static_cast<float>(native_source->GetHeight()),
		1.0f / static_cast<float>(native_source->GetWidth()),
		1.0f / static_cast<float>(native_source->GetHeight())};
	if (!upload4(fragment_uniforms.native_texture_size, native_size,
			"upload native texture size"))
	{
		return false;
	}
	const float fb_mask[4] = {
		static_cast<float>(config.cb_ps.FbMask.x),
		static_cast<float>(config.cb_ps.FbMask.y),
		static_cast<float>(config.cb_ps.FbMask.z),
		static_cast<float>(config.cb_ps.FbMask.w)};
	if (!upload4(fragment_uniforms.fb_mask, fb_mask,
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
	if (!upload4(fragment_uniforms.hardware_blend[0], blend0,
			"upload hardware blend 0") ||
		!upload4(fragment_uniforms.hardware_blend[1], blend1,
			"upload hardware blend 1"))
	{
		return false;
	}
	const float color_mask[4] = {
		config.colormask.wr ? 1.0f : 0.0f,
		config.colormask.wg ? 1.0f : 0.0f,
		config.colormask.wb ? 1.0f : 0.0f,
		config.colormask.wa ? 1.0f : 0.0f};
	return upload4(fragment_uniforms.color_mask, color_mask,
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
	const VitaGXM::TextureLevelLayout* src_layout = source.Level(0);
	const VitaGXM::TextureLevelLayout* dst_layout = destination.Level(0);
	if (!src_layout || !dst_layout)
		return nullptr;
	const u32 row_bytes = area.width() * 4;
	const u8* src = static_cast<const u8*>(source.LevelData(0)) +
		static_cast<size_t>(area.y) * src_layout->pitch + area.x * 4;
	u8* dst = static_cast<u8*>(destination.LevelData(0)) +
		static_cast<size_t>(area.y) * dst_layout->pitch + area.x * 4;
	for (int y = 0; y < area.height(); y++)
	{
		std::memcpy(dst + static_cast<size_t>(y) * dst_layout->pitch,
			src + static_cast<size_t>(y) * src_layout->pitch, row_bytes);
	}
	destination.SetState(GSTexture::State::Dirty);
	return &destination;
}

bool GSDeviceGXM::Impl::StageAndDraw(const GSHWDrawConfig& config,
	const GSHWDrawConfig::PSSelector& ps, u32 first_index, u32 index_count,
	VitaGXM::GSTextureGXM* source, SceGxmFragmentProgram* fragment,
	bool fast_fragment, bool source_only_fragment, bool source_direct_fragment,
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
	VitaGXM::GSTextureGXM* rt = CheckedCast<VitaGXM::GSTextureGXM>(config.rt);
	VitaGXM::GSTextureGXM* ds = CheckedCast<VitaGXM::GSTextureGXM>(config.ds);
	if (!EnsureScene(rt, ds, config.scissor))
		return false;
	void* vertex_memory = nullptr;
	u16* staged_indices = nullptr;
	if (!ReserveGeometry(index_count, index_count, &vertex_memory, &staged_indices))
	{
		if (!Finish() || !EnsureScene(rt, ds, config.scissor) ||
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
		const u32 color_index = config.vs.iip ? source_index :
			static_cast<u32>(config.indices[primitive_first]);
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
	if (bound_source->GetMipmapLevels() > 1 && !ps.automatic_lod)
		return Reject("multi-level texture without implicit GXM LOD");
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
	if (!UploadTfxUniforms(config, ps, source, fast_fragment,
			source_only_fragment, source_direct_fragment,
			source_direct_modulate_fragment,
			source_direct_modulate_af_fragment, untextured_fragment))
		return false;

	result = sceGxmDraw(context, TranslateTopology(config.topology),
		SCE_GXM_INDEX_FORMAT_U16, staged_indices, index_count);
	if (result < 0)
		return Fail("sceGxmDraw(TFX)", result);
	if (rt)
		rt->SetState(GSTexture::State::Dirty);
	if (ds && config.depth.zwe)
		ds->SetState(GSTexture::State::Dirty);
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
	return result >= 0 ? true : Fail("sceGxmDraw(presentation quad)", result);
}

void GSDeviceGXM::RenderHW(GSHWDrawConfig& config)
{
	if (!m_impl || !m_impl->ready)
		return;
	if (!config.rt && !config.ds)
	{
		m_impl->Reject("RenderHW without a target");
		return;
	}
	if (config.topology == GSHWDrawConfig::Topology::Point ||
		config.vs.expand != GSHWDrawConfig::VSExpand::None || config.vs.point_size ||
		config.line_expand)
	{
		m_impl->Reject("point/vertex/line expansion not lowered by GSRendererHW");
		return;
	}
	if (config.pal || config.ps.pal_fmt || config.ps.depth_fmt || config.ps.dst_fmt ||
		config.ps.afail != GSHWDrawConfig::PS_AFAIL::KEEP || config.ps.ztst ||
		config.ps.shuffle || config.ps.shuffle_same || config.ps.real16src ||
		config.ps.process_ba || config.ps.process_rg || config.ps.shuffle_across ||
		config.ps.write_rg || config.ps.a_masked ||
		config.ps.channel || config.ps.dither || config.ps.dither_adjust ||
		config.ps.colclip_hw || config.ps.urban_chaos_hle ||
		config.ps.tales_of_abyss_hle || config.ps.point_sampler ||
		config.ps.sw_aniso ||
		config.ps.scanmsk || config.ps.aa1 != GSHWDrawConfig::PS_AA1::NONE ||
		config.ps.rov_color ||
		config.ps.rov_depth != GSHWDrawConfig::PS_ROV_DEPTH::NONE ||
		config.ps.wms > 1 || config.ps.wmt > 1 || config.ps.zclamp ||
		config.ps.zfloor || config.ps.tcoffsethack || config.ps.adjs ||
		config.ps.adjt || config.ps.tex_is_fb ||
		config.ps.manual_lod || config.ps.date || config.ps.abe ||
		config.depth.date || config.depth.date_one ||
		config.sampler.triln != static_cast<u8>(GS_MIN_FILTER::Nearest))
	{
		Console.Error(
			"GXM GS: rejected TFX selector: ps=%08x%08x%08x%08x vs=%02x sampler=%02x depth=%02x blend=%08x mask=%02x topology=%u.",
			static_cast<u32>(config.ps.key_hi >> 32),
			static_cast<u32>(config.ps.key_hi),
			static_cast<u32>(config.ps.key_lo >> 32),
			static_cast<u32>(config.ps.key_lo), config.vs.key,
			config.sampler.key, config.depth.key, config.blend.key,
			config.colormask.key, static_cast<u32>(config.topology));
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

	VitaGXM::GSTextureGXM* source =
		CheckedCast<VitaGXM::GSTextureGXM>(config.tex);
	if (config.ps.automatic_lod && !source)
	{
		m_impl->Reject("automatic LOD draw without a source texture");
		return;
	}
	if (source && !m_impl->CommitClear(*source))
		return;
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

	bool fast_fragment = m_impl->CanUseFastTfx(config);
	bool source_only_fragment = !fast_fragment &&
		m_impl->CanUseSourceOnlyTfx(config);
	bool untextured_fragment = (fast_fragment || source_only_fragment) &&
		!config.vs.tme;
	bool source_direct_fragment = source_only_fragment &&
		!untextured_fragment && m_impl->CanUseSourceDirectTfx(config);
	bool source_direct_modulate_fragment = source_direct_fragment &&
		m_impl->CanUseSourceDirectModulateTfx(config);
	bool source_direct_modulate_af_fragment = source_direct_modulate_fragment &&
		m_impl->CanUseSourceDirectModulateAfTfx(config);
	SceGxmFragmentProgram* fragment =
		(fast_fragment || source_only_fragment) ?
		m_impl->GetPatchedTfxProgram(config, source_only_fragment,
			source_direct_fragment, source_direct_modulate_fragment,
			source_direct_modulate_af_fragment,
			untextured_fragment) : m_impl->tfx_fragment_program;
	if (!fragment)
	{
		// Shader-patcher memory exhaustion must not change GS behavior. The
		// general FRAGCOLOR program remains the exact fallback for this draw.
		fast_fragment = false;
		source_only_fragment = false;
		source_direct_fragment = false;
		source_direct_modulate_fragment = false;
		source_direct_modulate_af_fragment = false;
		untextured_fragment = false;
		fragment = m_impl->tfx_fragment_program;
	}
	const u32 primitive = config.indices_per_prim;
	const u32 max_chunk = (MAX_STAGED_INDICES / primitive) * primitive;
	for (u32 first = 0; first < config.nindices;)
	{
		const u32 count = std::min(max_chunk, config.nindices - first);
		if (count == 0 || !m_impl->StageAndDraw(config, config.ps, first, count,
			source, fragment, fast_fragment, source_only_fragment,
			source_direct_fragment, source_direct_modulate_fragment,
			source_direct_modulate_af_fragment,
			untextured_fragment))
		{
			return;
		}
		first += count;
	}
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
	}
	destination->SetState(GSTexture::State::Dirty);
}

void GSDeviceGXM::DoInterlace(GSTexture* source_texture,
	const GSVector4& source_rect, GSTexture* destination_texture,
	const GSVector4& destination_rect, ShaderInterlace shader, Filter filter,
	const InterlaceConstantBuffer& cb)
{
	if (!m_impl || !source_texture || !destination_texture)
		return;
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
	unregister(tfx_source_direct_fragment_id,
		"unregister source-only direct-texture TFX fragment");
	unregister(tfx_source_direct_modulate_fragment_id,
		"unregister source-only direct MODULATE/RGBA TFX fragment");
	unregister(tfx_source_direct_modulate_af_fragment_id,
		"unregister source-only direct MODULATE/RGBA (Cs-0)*Af+0 TFX fragment");
	unregister(tfx_source_fragment_id,
		"unregister source-only TFX fragment");
	unregister(tfx_untextured_fragment_id,
		"unregister untextured TFX fragment");
	unregister(tfx_fast_fragment_id, "unregister fast TFX fragment");
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
	scene_active = false;
	scene_is_display = false;
	scene_rt = nullptr;
	scene_ds = nullptr;
	vertex_offset = 0;
	index_offset = 0;
}

#endif
