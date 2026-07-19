// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#include "VitaGxmTexture.h"

#if !defined(VITASX2_QEMU_VALIDATION)

#include "common/Console.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <utility>

namespace VitaGXM
{
	namespace
	{
		std::atomic<std::uint32_t> s_next_texture_telemetry_id{1};

		constexpr std::uint32_t TEXTURE_UPLOAD_ALIGNMENT = 64;
		constexpr std::uint32_t TEXTURE_UPLOAD_PITCH_ALIGNMENT = 64;
		constexpr std::uint32_t DOWNLOAD_PITCH_ALIGNMENT = 64;
		// Present in Sony's gxm/defs.h but omitted from VitaSDK's public header.
		constexpr std::uint32_t TEXTURE_IMPLICIT_STRIDE_ALIGNMENT = 8;

		constexpr TextureFormatInfo COLOR_FORMAT = {
			GSTexture::Format::Color,
			SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR,
			SCE_GXM_TRANSFER_FORMAT_U8U8U8U8_ABGR,
			SCE_GXM_COLOR_FORMAT_U8U8U8U8_ABGR,
			SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT,
			4,
			true,
			false};
		constexpr TextureFormatInfo UNORM8_FORMAT = {
			GSTexture::Format::UNorm8,
			// PCSX2's palette path consumes both the scalar/red and alpha view.
			// RRRR gives both without a shader-side backend special case.
			SCE_GXM_TEXTURE_FORMAT_U8_RRRR, SCE_GXM_TRANSFER_FORMAT_U8_R,
			SCE_GXM_COLOR_FORMAT_U8_R, SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT, 1, false,
			false};
		constexpr TextureFormatInfo UINT16_FORMAT = {GSTexture::Format::UInt16,
			SCE_GXM_TEXTURE_FORMAT_U16_R,
			SCE_GXM_TRANSFER_FORMAT_RAW16,
			SCE_GXM_COLOR_FORMAT_U16_R,
			SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT,
			2,
			true,
			false};
		constexpr TextureFormatInfo UINT32_FORMAT = {
			GSTexture::Format::UInt32, SCE_GXM_TEXTURE_FORMAT_U32_R,
			SCE_GXM_TRANSFER_FORMAT_RAW32,
			// GXM has no U32_R color surface. Routing arbitrary PS2 depth bits
			// through F32_R is not exact because NaN and denormal encodings may be
			// canonicalized. Keep UInt32 valid for upload/sample/readback, but do
			// not advertise it as renderable until a byte-packed shader path is
			// proven bit-exact on SGX543.
			SCE_GXM_COLOR_FORMAT_F32_R, SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT, 4, false,
			false};
		constexpr TextureFormatInfo DEPTH_FORMAT = {GSTexture::Format::DepthStencil,
			// DF32M reserves the sign bit for GXM's mask-update path. The F32M
			// texture view removes that implementation bit when sampling depth.
			SCE_GXM_TEXTURE_FORMAT_F32M_R,
			SCE_GXM_TRANSFER_FORMAT_RAW32,
			SCE_GXM_COLOR_FORMAT_F32_R,
			SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT,
			4,
			false,
			true};

		bool IsPowerOfTwo(std::size_t value)
		{
			return value != 0 && (value & (value - 1)) == 0;
		}

		bool AlignUp(std::size_t value, std::size_t alignment, std::size_t* aligned)
		{
			if (!aligned || !IsPowerOfTwo(alignment) ||
				value > std::numeric_limits<std::size_t>::max() - (alignment - 1))
			{
				return false;
			}
			*aligned = (value + alignment - 1) & ~(alignment - 1);
			return true;
		}

		bool Multiply(std::size_t lhs, std::size_t rhs, std::size_t* product)
		{
			if (!product ||
				(lhs != 0 && rhs > std::numeric_limits<std::size_t>::max() / lhs))
				return false;
			*product = lhs * rhs;
			return true;
		}

		bool NextPowerOfTwo(std::uint32_t value, std::uint32_t* result)
		{
			if (!result || value == 0 || value > (1u << 31))
				return false;

			value--;
			value |= value >> 1;
			value |= value >> 2;
			value |= value >> 4;
			value |= value >> 8;
			value |= value >> 16;
			*result = value + 1;
			return true;
		}

		void SetError(int* error, int value)
		{
			if (error)
				*error = value;
		}

		bool RectWithin(const GSVector4i& rect, std::uint32_t width,
			std::uint32_t height)
		{
			return rect.x >= 0 && rect.y >= 0 && rect.z > rect.x && rect.w > rect.y &&
			       static_cast<std::uint32_t>(rect.z) <= width &&
			       static_cast<std::uint32_t>(rect.w) <= height;
		}
	} // namespace

	const TextureFormatInfo* GetTextureFormatInfo(GSTexture::Format format)
	{
		switch (format)
		{
			case GSTexture::Format::Color:
				return &COLOR_FORMAT;
			case GSTexture::Format::UNorm8:
				return &UNORM8_FORMAT;
			case GSTexture::Format::UInt16:
				return &UINT16_FORMAT;
			case GSTexture::Format::UInt32:
				return &UINT32_FORMAT;
			case GSTexture::Format::DepthStencil:
				return &DEPTH_FORMAT;
			default:
				return nullptr;
		}
	}

	GSTextureGXM::GSTextureGXM(TextureOwner* owner)
		: m_owner(owner),
		  m_telemetry_id(s_next_texture_telemetry_id.fetch_add(
			  1, std::memory_order_relaxed))
	{
	}

	void GSTextureGXM::RecordWriter(TextureWriterKind kind,
		const GSTextureGXM* source)
	{
		m_writer_telemetry.content_generation++;
		const TextureWriterTelemetry* const source_writer =
			source ? &source->WriterTelemetry() : nullptr;
		m_writer_telemetry.kind = kind;
		m_writer_telemetry.source_id = source ? source->TelemetryId() : 0;
		m_writer_telemetry.source_size = source ?
			(static_cast<std::uint32_t>(source->GetWidth()) & 0xffffu) |
			((static_cast<std::uint32_t>(source->GetHeight()) & 0xffffu) << 16) : 0;
		m_writer_telemetry.ps_lo = 0;
		m_writer_telemetry.ps_hi = 0;
		m_writer_telemetry.blend = 0;
		m_writer_telemetry.selector_keys = 0;
		m_writer_telemetry.topology = 0;
		m_writer_telemetry.color_mask = 0;
		m_writer_telemetry.draw_area = 0;
		m_writer_telemetry.sample_area = 0;
		m_writer_telemetry.source_writer_tfx_writes =
			source_writer ? source_writer->tfx_writes : 0;
		m_writer_telemetry.source_writer_textured_tfx_writes =
			source_writer ? source_writer->textured_tfx_writes : 0;
		m_writer_telemetry.source_writer_untextured_tfx_writes =
			source_writer ? source_writer->untextured_tfx_writes : 0;
		m_writer_telemetry.source_writer_render_target_source_tfx_writes =
			source_writer ? source_writer->render_target_source_tfx_writes : 0;
		m_writer_telemetry.source_writer_full_mask_tfx_writes =
			source_writer ? source_writer->full_mask_tfx_writes : 0;
		m_writer_telemetry.source_writer_rgb_only_tfx_writes =
			source_writer ? source_writer->rgb_only_tfx_writes : 0;
		m_writer_telemetry.source_writer_alpha_only_tfx_writes =
			source_writer ? source_writer->alpha_only_tfx_writes : 0;
		m_writer_telemetry.source_writer_other_mask_tfx_writes =
			source_writer ? source_writer->other_mask_tfx_writes : 0;
		m_writer_telemetry.source_writer_ps_lo =
			source_writer ? source_writer->ps_lo : 0;
		m_writer_telemetry.source_writer_ps_hi =
			source_writer ? source_writer->ps_hi : 0;
		m_writer_telemetry.source_writer_draw_area =
			source_writer ? source_writer->draw_area : 0;
		m_writer_telemetry.source_writer_sample_area =
			source_writer ? source_writer->sample_area : 0;
		m_writer_telemetry.source_writer_source_id =
			source_writer ? source_writer->source_id : 0;
		m_writer_telemetry.source_writer_source_size =
			source_writer ? source_writer->source_size : 0;
		m_writer_telemetry.source_writer_blend =
			source_writer ? source_writer->blend : 0;
		m_writer_telemetry.source_writer_selector_keys =
			source_writer ? source_writer->selector_keys : 0;
		m_writer_telemetry.source_writer_last_rgb_ps_lo =
			source_writer ? source_writer->last_rgb_ps_lo : 0;
		m_writer_telemetry.source_writer_last_rgb_ps_hi =
			source_writer ? source_writer->last_rgb_ps_hi : 0;
		m_writer_telemetry.source_writer_last_rgb_draw_area =
			source_writer ? source_writer->last_rgb_draw_area : 0;
		m_writer_telemetry.source_writer_last_rgb_sample_area =
			source_writer ? source_writer->last_rgb_sample_area : 0;
		m_writer_telemetry.source_writer_last_rgb_source_id =
			source_writer ? source_writer->last_rgb_source_id : 0;
		m_writer_telemetry.source_writer_last_rgb_source_size =
			source_writer ? source_writer->last_rgb_source_size : 0;
		m_writer_telemetry.source_writer_last_rgb_blend =
			source_writer ? source_writer->last_rgb_blend : 0;
		m_writer_telemetry.source_writer_last_rgb_selector_keys =
			source_writer ? source_writer->last_rgb_selector_keys : 0;
		m_writer_telemetry.source_writer_kind = source_writer ?
			source_writer->kind : TextureWriterKind::None;
		m_writer_telemetry.source_writer_topology =
			source_writer ? source_writer->topology : 0;
		m_writer_telemetry.source_writer_color_mask =
			source_writer ? source_writer->color_mask : 0;
		m_writer_telemetry.source_writer_last_rgb_topology =
			source_writer ? source_writer->last_rgb_topology : 0;
		m_writer_telemetry.source_writer_last_rgb_color_mask =
			source_writer ? source_writer->last_rgb_color_mask : 0;
	}

	void GSTextureGXM::RecordTfxWriter(const GSTextureGXM* source,
		std::uint64_t ps_lo, std::uint64_t ps_hi, std::uint32_t blend,
		std::uint32_t selector_keys, std::uint8_t topology,
		std::uint8_t color_mask,
		std::uint64_t draw_area, std::uint64_t sample_area)
	{
		m_writer_telemetry.content_generation++;
		const TextureWriterTelemetry* const source_writer =
			source ? &source->WriterTelemetry() : nullptr;
		m_writer_telemetry.tfx_writes++;
		if (source)
		{
			m_writer_telemetry.textured_tfx_writes++;
			if (source->IsRenderTarget())
				m_writer_telemetry.render_target_source_tfx_writes++;
		}
		else
		{
			m_writer_telemetry.untextured_tfx_writes++;
		}
		switch (color_mask & 0xfu)
		{
			case 0xf: m_writer_telemetry.full_mask_tfx_writes++; break;
			case 0x7: m_writer_telemetry.rgb_only_tfx_writes++; break;
			case 0x8: m_writer_telemetry.alpha_only_tfx_writes++; break;
			default: m_writer_telemetry.other_mask_tfx_writes++; break;
		}
		m_writer_telemetry.kind = TextureWriterKind::Tfx;
		m_writer_telemetry.source_id = source ? source->TelemetryId() : 0;
		m_writer_telemetry.source_size = source ?
			(static_cast<std::uint32_t>(source->GetWidth()) & 0xffffu) |
			((static_cast<std::uint32_t>(source->GetHeight()) & 0xffffu) << 16) : 0;
		m_writer_telemetry.ps_lo = ps_lo;
		m_writer_telemetry.ps_hi = ps_hi;
		m_writer_telemetry.blend = blend;
		m_writer_telemetry.selector_keys = selector_keys;
		m_writer_telemetry.topology = topology;
		m_writer_telemetry.color_mask = color_mask & 0xfu;
		m_writer_telemetry.draw_area = draw_area;
		m_writer_telemetry.sample_area = sample_area;
		if (color_mask & 0x7u)
		{
			m_writer_telemetry.last_rgb_ps_lo = ps_lo;
			m_writer_telemetry.last_rgb_ps_hi = ps_hi;
			m_writer_telemetry.last_rgb_draw_area = draw_area;
			m_writer_telemetry.last_rgb_sample_area = sample_area;
			m_writer_telemetry.last_rgb_source_id =
				source ? source->TelemetryId() : 0;
			m_writer_telemetry.last_rgb_source_size = source ?
				(static_cast<std::uint32_t>(source->GetWidth()) & 0xffffu) |
				((static_cast<std::uint32_t>(source->GetHeight()) & 0xffffu) << 16) : 0;
			m_writer_telemetry.last_rgb_blend = blend;
			m_writer_telemetry.last_rgb_selector_keys = selector_keys;
			m_writer_telemetry.last_rgb_topology = topology;
			m_writer_telemetry.last_rgb_color_mask = color_mask & 0xfu;
		}
		m_writer_telemetry.source_writer_tfx_writes =
			source_writer ? source_writer->tfx_writes : 0;
		m_writer_telemetry.source_writer_textured_tfx_writes =
			source_writer ? source_writer->textured_tfx_writes : 0;
		m_writer_telemetry.source_writer_untextured_tfx_writes =
			source_writer ? source_writer->untextured_tfx_writes : 0;
		m_writer_telemetry.source_writer_render_target_source_tfx_writes =
			source_writer ? source_writer->render_target_source_tfx_writes : 0;
		m_writer_telemetry.source_writer_full_mask_tfx_writes =
			source_writer ? source_writer->full_mask_tfx_writes : 0;
		m_writer_telemetry.source_writer_rgb_only_tfx_writes =
			source_writer ? source_writer->rgb_only_tfx_writes : 0;
		m_writer_telemetry.source_writer_alpha_only_tfx_writes =
			source_writer ? source_writer->alpha_only_tfx_writes : 0;
		m_writer_telemetry.source_writer_other_mask_tfx_writes =
			source_writer ? source_writer->other_mask_tfx_writes : 0;
		m_writer_telemetry.source_writer_ps_lo =
			source_writer ? source_writer->ps_lo : 0;
		m_writer_telemetry.source_writer_ps_hi =
			source_writer ? source_writer->ps_hi : 0;
		m_writer_telemetry.source_writer_draw_area =
			source_writer ? source_writer->draw_area : 0;
		m_writer_telemetry.source_writer_sample_area =
			source_writer ? source_writer->sample_area : 0;
		m_writer_telemetry.source_writer_source_id =
			source_writer ? source_writer->source_id : 0;
		m_writer_telemetry.source_writer_source_size =
			source_writer ? source_writer->source_size : 0;
		m_writer_telemetry.source_writer_blend =
			source_writer ? source_writer->blend : 0;
		m_writer_telemetry.source_writer_selector_keys =
			source_writer ? source_writer->selector_keys : 0;
		m_writer_telemetry.source_writer_last_rgb_ps_lo =
			source_writer ? source_writer->last_rgb_ps_lo : 0;
		m_writer_telemetry.source_writer_last_rgb_ps_hi =
			source_writer ? source_writer->last_rgb_ps_hi : 0;
		m_writer_telemetry.source_writer_last_rgb_draw_area =
			source_writer ? source_writer->last_rgb_draw_area : 0;
		m_writer_telemetry.source_writer_last_rgb_sample_area =
			source_writer ? source_writer->last_rgb_sample_area : 0;
		m_writer_telemetry.source_writer_last_rgb_source_id =
			source_writer ? source_writer->last_rgb_source_id : 0;
		m_writer_telemetry.source_writer_last_rgb_source_size =
			source_writer ? source_writer->last_rgb_source_size : 0;
		m_writer_telemetry.source_writer_last_rgb_blend =
			source_writer ? source_writer->last_rgb_blend : 0;
		m_writer_telemetry.source_writer_last_rgb_selector_keys =
			source_writer ? source_writer->last_rgb_selector_keys : 0;
		m_writer_telemetry.source_writer_kind = source_writer ?
			source_writer->kind : TextureWriterKind::None;
		m_writer_telemetry.source_writer_topology =
			source_writer ? source_writer->topology : 0;
		m_writer_telemetry.source_writer_color_mask =
			source_writer ? source_writer->color_mask : 0;
		m_writer_telemetry.source_writer_last_rgb_topology =
			source_writer ? source_writer->last_rgb_topology : 0;
		m_writer_telemetry.source_writer_last_rgb_color_mask =
			source_writer ? source_writer->last_rgb_color_mask : 0;
	}

	std::unique_ptr<GSTextureGXM> GSTextureGXM::Create(TextureOwner* owner,
		Usage usage, int width,
		int height, int levels,
		Format format, int* error)
	{
		SetError(error, 0);
		if (!owner)
		{
			SetError(error, SCE_GXM_ERROR_INVALID_POINTER);
			return {};
		}

		std::unique_ptr<GSTextureGXM> texture(new GSTextureGXM(owner));
		const int result = texture->Initialize(usage, width, height, levels, format);
		if (result < 0)
		{
			SetError(error, result);
			return {};
		}
		return texture;
	}

	GSTextureGXM::~GSTextureGXM()
	{
		m_pending_map.staging.Reset();
		RetireStorage();
	}

	int GSTextureGXM::Initialize(Usage usage, int width, int height, int levels,
		Format format)
	{
		const TextureFormatInfo* const native_format = GetTextureFormatInfo(format);
		if (!native_format)
			return SCE_GXM_ERROR_UNSUPPORTED;
		if (width < 1 || height < 1 || width > 4096 || height > 4096 || levels < 1 ||
			levels > 13)
		{
			return SCE_GXM_ERROR_INVALID_VALUE;
		}
		if (!GSTexture::IsTexture(usage) && !GSTexture::IsRenderTarget(usage) &&
			!GSTexture::IsDepthStencil(usage))
		{
			return SCE_GXM_ERROR_INVALID_VALUE;
		}
		if (GSTexture::IsDepthStencil(usage) &&
			(usage & (Usage::ShaderWrite | Usage::RenderTarget)))
		{
			return SCE_GXM_ERROR_INVALID_VALUE;
		}
		if (GSTexture::IsFeedback(usage) &&
			!(GSTexture::IsRenderTarget(usage) || GSTexture::IsDepthStencil(usage)))
		{
			return SCE_GXM_ERROR_INVALID_VALUE;
		}
		// SGX543 has no PCSX2-style image load/store path. Advertising a
		// ShaderWrite texture would make CAS and ROV select unsupported behavior.
		if (GSTexture::IsShaderWrite(usage))
			return SCE_GXM_ERROR_UNSUPPORTED;
		if (GSTexture::IsRenderTargetOrDepthStencil(usage) && levels != 1)
			return SCE_GXM_ERROR_INVALID_VALUE;
		if (native_format->depth_stencil != GSTexture::IsDepthStencil(usage))
			return SCE_GXM_ERROR_INVALID_VALUE;
		if (GSTexture::IsFeedback(usage) && !GSTexture::IsFeedbackFormat(format))
			return SCE_GXM_ERROR_UNSUPPORTED;
		if (format == Format::UNorm8 && usage != Usage::Texture)
			return SCE_GXM_ERROR_UNSUPPORTED;
		if (GSTexture::IsRenderTarget(usage) && !native_format->color_renderable)
			return SCE_GXM_ERROR_UNSUPPORTED;

		m_size = GSVector2i(width, height);
		m_mipmap_levels = levels;
		m_usage = usage;
		m_format = format;
		m_native_format = *native_format;
		return native_format->depth_stencil ? InitializeDepthStorage() : InitializeColorStorage();
	}

	int GSTextureGXM::InitializeColorStorage()
	{
		// Sony's GPU guide and tutorial_postprocessing/renderbuffer.c define the
		// same 32x32 scanline-ordered tile layout for a color surface and its
		// texture view. Render targets are the only VitaSX2 color allocations
		// which are both PBE outputs and subsequent texture inputs, so use that
		// matched layout there. Upload/source textures stay linear: the guide
		// explicitly makes the choice workload-dependent, and linear storage is
		// the better CPU/upload contract.
		const bool tiled_render_target = GSTexture::IsRenderTarget(m_usage) &&
			m_mipmap_levels == 1 && m_size.x >= 32 && m_size.y >= 32;
		if (tiled_render_target)
		{
			std::size_t storage_width = 0;
			std::size_t storage_height = 0;
			std::size_t pitch = 0;
			std::size_t total_size = 0;
			if (!AlignUp(static_cast<std::size_t>(m_size.x), 32, &storage_width) ||
				!AlignUp(static_cast<std::size_t>(m_size.y), 32, &storage_height) ||
				!Multiply(storage_width, m_native_format.bytes_per_pixel, &pitch) ||
				!Multiply(pitch, storage_height, &total_size) ||
				pitch > std::numeric_limits<std::uint32_t>::max())
			{
				return SCE_GXM_ERROR_INVALID_VALUE;
			}

			m_levels.push_back(TextureLevelLayout{0,
				static_cast<std::uint32_t>(pitch),
				static_cast<std::uint32_t>(m_size.x),
				static_cast<std::uint32_t>(m_size.y), true});
			int result = m_owner->TextureArena().Allocate(
				total_size, SCE_GXM_TEXTURE_ALIGNMENT, &m_storage);
			if (result < 0)
				return result;

			result = sceGxmTextureInitTiled(&m_texture, m_storage.Data(),
				m_native_format.texture_format,
				static_cast<std::uint32_t>(m_size.x),
				static_cast<std::uint32_t>(m_size.y), 0);
			if (result < 0)
				return result;
			result = sceGxmTextureSetUAddrMode(
				&m_texture, SCE_GXM_TEXTURE_ADDR_CLAMP);
			if (result >= 0)
				result = sceGxmTextureSetVAddrMode(
					&m_texture, SCE_GXM_TEXTURE_ADDR_CLAMP);
			if (result >= 0)
				result = sceGxmTextureSetMinFilter(
					&m_texture, SCE_GXM_TEXTURE_FILTER_POINT);
			if (result >= 0)
				result = sceGxmTextureSetMagFilter(
					&m_texture, SCE_GXM_TEXTURE_FILTER_POINT);
			if (result >= 0)
				result = sceGxmTextureSetMipFilter(
					&m_texture, SCE_GXM_TEXTURE_MIP_FILTER_DISABLED);
			if (result >= 0)
				result = sceGxmTextureValidate(&m_texture);
			if (result < 0)
				return result;

			// The official renderbuffer sample passes the logical surface width as
			// the stride; GXM derives the whole-tile footprint internally.
			result = sceGxmColorSurfaceInit(&m_color_surface,
				m_native_format.color_format, SCE_GXM_COLOR_SURFACE_TILED,
				SCE_GXM_COLOR_SURFACE_SCALE_NONE,
				m_native_format.output_register_size,
				static_cast<std::uint32_t>(m_size.x),
				static_cast<std::uint32_t>(m_size.y),
				static_cast<std::uint32_t>(m_size.x), m_storage.Data());
			if (result < 0)
				return result;
			m_has_color_surface = true;
			return 0;
		}

		std::size_t total_size = 0;
		std::uint32_t level_width = static_cast<std::uint32_t>(m_size.x);
		std::uint32_t level_height = static_cast<std::uint32_t>(m_size.y);
		std::uint32_t storage_width = 0;
		std::uint32_t storage_height = 0;
		// Sony's linear-mipmap layout derives only the offset to the next
		// level from the enclosing power-of-two image. Each level's own rows
		// still use the actual width rounded to eight texels.
		if (!NextPowerOfTwo(level_width, &storage_width) ||
			!NextPowerOfTwo(level_height, &storage_height))
		{
			return SCE_GXM_ERROR_INVALID_VALUE;
		}
		storage_width = std::max<std::uint32_t>(
			storage_width, TEXTURE_IMPLICIT_STRIDE_ALIGNMENT);

		m_levels.reserve(m_mipmap_levels);
		for (int level = 0; level < m_mipmap_levels; level++)
		{
			std::size_t stride_pixels = 0;
			if (!AlignUp(level_width, TEXTURE_IMPLICIT_STRIDE_ALIGNMENT,
					&stride_pixels))
			{
				return SCE_GXM_ERROR_INVALID_VALUE;
			}

			std::size_t pitch = 0;
			std::size_t storage_pitch = 0;
			std::size_t level_size = 0;
			if (!Multiply(stride_pixels, m_native_format.bytes_per_pixel, &pitch) ||
				!Multiply(storage_width, m_native_format.bytes_per_pixel,
					&storage_pitch) ||
				!Multiply(level + 1 < m_mipmap_levels ? storage_pitch : pitch,
					level + 1 < m_mipmap_levels ? storage_height : level_height,
					&level_size) ||
				total_size > std::numeric_limits<std::uint32_t>::max() - level_size ||
				pitch > std::numeric_limits<std::uint32_t>::max())
			{
				return SCE_GXM_ERROR_INVALID_VALUE;
			}

			m_levels.push_back(TextureLevelLayout{
				static_cast<std::uint32_t>(total_size),
				static_cast<std::uint32_t>(pitch), level_width, level_height, false});
			total_size += level_size;
			level_width = std::max<std::uint32_t>(1, level_width >> 1);
			level_height = std::max<std::uint32_t>(1, level_height >> 1);
			storage_width = std::max<std::uint32_t>(
				TEXTURE_IMPLICIT_STRIDE_ALIGNMENT, storage_width >> 1);
			storage_height = std::max<std::uint32_t>(1, storage_height >> 1);
		}

		const int allocation_result = m_owner->TextureArena().Allocate(
			total_size, SCE_GXM_TEXTURE_ALIGNMENT, &m_storage);
		if (allocation_result < 0)
			return allocation_result;

		const std::uint32_t gxm_mip_count =
			(m_mipmap_levels > 1) ? static_cast<std::uint32_t>(m_mipmap_levels) : 0;
		int result = sceGxmTextureInitLinear(
			&m_texture, m_storage.Data(), m_native_format.texture_format,
			static_cast<std::uint32_t>(m_size.x),
			static_cast<std::uint32_t>(m_size.y), gxm_mip_count);
		if (result < 0)
			return result;

		result = sceGxmTextureSetUAddrMode(&m_texture, SCE_GXM_TEXTURE_ADDR_CLAMP);
		if (result >= 0)
			result = sceGxmTextureSetVAddrMode(&m_texture, SCE_GXM_TEXTURE_ADDR_CLAMP);
		if (result >= 0)
			result =
				sceGxmTextureSetMinFilter(&m_texture, SCE_GXM_TEXTURE_FILTER_POINT);
		if (result >= 0)
			result =
				sceGxmTextureSetMagFilter(&m_texture, SCE_GXM_TEXTURE_FILTER_POINT);
		if (result >= 0)
		{
			result = sceGxmTextureSetMipFilter(
				&m_texture, m_mipmap_levels > 1 ? SCE_GXM_TEXTURE_MIP_FILTER_ENABLED : SCE_GXM_TEXTURE_MIP_FILTER_DISABLED);
		}
		if (result >= 0)
			result = sceGxmTextureValidate(&m_texture);
		if (result < 0)
			return result;

		if (GSTexture::IsRenderTarget(m_usage))
		{
			result = sceGxmColorSurfaceInit(
				&m_color_surface, m_native_format.color_format,
				SCE_GXM_COLOR_SURFACE_LINEAR, SCE_GXM_COLOR_SURFACE_SCALE_NONE,
				m_native_format.output_register_size,
				static_cast<std::uint32_t>(m_size.x),
				static_cast<std::uint32_t>(m_size.y),
				m_levels[0].pitch / m_native_format.bytes_per_pixel, m_storage.Data());
			if (result < 0)
				return result;
			m_has_color_surface = true;
		}
		return 0;
	}

	int GSTextureGXM::InitializeDepthStorage()
	{
		std::size_t aligned_width = 0;
		std::size_t aligned_height = 0;
		if (!AlignUp(static_cast<std::size_t>(m_size.x), SCE_GXM_TILE_SIZEX,
				&aligned_width) ||
			!AlignUp(static_cast<std::size_t>(m_size.y), SCE_GXM_TILE_SIZEY,
				&aligned_height))
		{
			return SCE_GXM_ERROR_INVALID_VALUE;
		}

		std::size_t depth_pitch = 0;
		std::size_t stencil_pitch = aligned_width;
		std::size_t depth_size = 0;
		std::size_t stencil_size = 0;
		if (!Multiply(aligned_width, 4, &depth_pitch) ||
			!Multiply(depth_pitch, aligned_height, &depth_size) ||
			!Multiply(stencil_pitch, aligned_height, &stencil_size) ||
			depth_pitch > std::numeric_limits<std::uint32_t>::max() ||
			stencil_pitch > std::numeric_limits<std::uint32_t>::max())
		{
			return SCE_GXM_ERROR_INVALID_VALUE;
		}

		int result = m_owner->TextureArena().Allocate(
			depth_size, SCE_GXM_DEPTHSTENCIL_SURFACE_ALIGNMENT, &m_storage);
		if (result < 0)
			return result;
		result = m_owner->TextureArena().Allocate(
			stencil_size, SCE_GXM_DEPTHSTENCIL_SURFACE_ALIGNMENT, &m_stencil_storage);
		if (result < 0)
			return result;

		m_depth_pitch = static_cast<std::uint32_t>(depth_pitch);
		m_stencil_pitch = static_cast<std::uint32_t>(stencil_pitch);
		m_levels.push_back(TextureLevelLayout{0, m_depth_pitch,
			static_cast<std::uint32_t>(m_size.x),
			static_cast<std::uint32_t>(m_size.y), false});

		// Depth surfaces require a 32-sample stride. A linear-strided texture
		// view preserves that pitch for PCSX2's depth conversion passes.
		result = sceGxmTextureInitLinearStrided(
			&m_texture, m_storage.Data(), m_native_format.texture_format,
			static_cast<std::uint32_t>(m_size.x),
			static_cast<std::uint32_t>(m_size.y), m_depth_pitch);
		if (result < 0)
			return result;
		result = sceGxmTextureSetUAddrMode(&m_texture, SCE_GXM_TEXTURE_ADDR_CLAMP);
		if (result >= 0)
			result = sceGxmTextureSetVAddrMode(&m_texture, SCE_GXM_TEXTURE_ADDR_CLAMP);
		// Official PSP2 gxm/texture.h makes minification state unavailable for
		// LINEAR_STRIDED textures: sceGxmTextureSetMinFilter() must return
		// SCE_GXM_ERROR_UNSUPPORTED for this descriptor type. The hardware's
		// fixed minification behavior is the only legal state; magnification is
		// still programmable and PCSX2's depth sampling requires point filtering.
		if (result >= 0)
			result =
				sceGxmTextureSetMagFilter(&m_texture, SCE_GXM_TEXTURE_FILTER_POINT);
		if (result >= 0)
			result = sceGxmTextureValidate(&m_texture);
		if (result < 0)
			return result;

		result = sceGxmDepthStencilSurfaceInit(
			&m_depth_surface,
			// PCSX2 DATE/scissor emulation needs GXM's mask-update plane as well
			// as separate stencil storage; plain DF32_S8 cannot represent it.
			SCE_GXM_DEPTH_STENCIL_FORMAT_DF32M_S8,
			SCE_GXM_DEPTH_STENCIL_SURFACE_LINEAR,
			static_cast<std::uint32_t>(aligned_width), m_storage.Data(),
			m_stencil_storage.Data());
		if (result < 0)
			return result;

		// PCSX2 targets survive target switches and feedback/copy scenes. Sony's
		// documented defaults discard both planes, so persistent load/store is
		// mandatory for the general texture-cache object.
		sceGxmDepthStencilSurfaceSetForceLoadMode(
			&m_depth_surface, SCE_GXM_DEPTH_STENCIL_FORCE_LOAD_ENABLED);
		sceGxmDepthStencilSurfaceSetForceStoreMode(
			&m_depth_surface, SCE_GXM_DEPTH_STENCIL_FORCE_STORE_ENABLED);
		m_has_depth_surface = true;
		return 0;
	}

	void* GSTextureGXM::GetNativeHandle() const
	{
		return const_cast<SceGxmTexture*>(&m_texture);
	}

	bool GSTextureGXM::Update(const GSVector4i& rect, const void* data, int pitch,
		int layer)
	{
		if (!data || pitch <= 0 || layer < 0 ||
			!ValidateRect(rect, static_cast<std::uint32_t>(layer)) ||
			m_native_format.depth_stencil)
		{
			m_operation_failed = true;
			return false;
		}

		const std::size_t row_bytes =
			static_cast<std::size_t>(rect.width()) * m_native_format.bytes_per_pixel;
		if (static_cast<std::size_t>(pitch) < row_bytes)
		{
			m_operation_failed = true;
			return false;
		}

		std::size_t upload_pitch = 0;
		std::size_t upload_size = 0;
		if (!AlignUp(row_bytes, TEXTURE_UPLOAD_PITCH_ALIGNMENT, &upload_pitch) ||
			!Multiply(upload_pitch, static_cast<std::size_t>(rect.height()),
				&upload_size) ||
			upload_pitch > std::numeric_limits<std::uint32_t>::max())
		{
			m_operation_failed = true;
			return false;
		}

		ArenaAllocation staging;
		const int result = m_owner->TransferArena().Allocate(
			upload_size, TEXTURE_UPLOAD_ALIGNMENT, &staging);
		if (result < 0)
		{
			m_operation_failed = true;
			return false;
		}

		const std::uint8_t* source = static_cast<const std::uint8_t*>(data);
		std::uint8_t* destination = static_cast<std::uint8_t*>(staging.Data());
		for (int y = 0; y < rect.height(); y++)
		{
			std::memcpy(destination + static_cast<std::size_t>(y) * upload_pitch,
				source + static_cast<std::size_t>(y) * pitch, row_bytes);
		}

		return QueueUpload(std::move(staging),
			static_cast<std::uint32_t>(upload_pitch), rect,
			static_cast<std::uint32_t>(layer));
	}

	bool GSTextureGXM::Map(GSMap& map, const GSVector4i* rect, int layer)
	{
		map = {};
		if (m_pending_map.staging || layer < 0 || m_native_format.depth_stencil)
		{
			m_operation_failed = true;
			return false;
		}

		const std::uint32_t level = static_cast<std::uint32_t>(layer);
		const TextureLevelLayout* const layout = Level(level);
		if (!layout)
		{
			m_operation_failed = true;
			return false;
		}

		const GSVector4i upload_rect =
			rect ? *rect : GSVector4i(0, 0, static_cast<int>(layout->width), static_cast<int>(layout->height));
		if (!ValidateRect(upload_rect, level))
		{
			m_operation_failed = true;
			return false;
		}

		std::size_t pitch = 0;
		std::size_t size = 0;
		const std::size_t row_bytes = static_cast<std::size_t>(upload_rect.width()) *
		                              m_native_format.bytes_per_pixel;
		if (!AlignUp(row_bytes, TEXTURE_UPLOAD_PITCH_ALIGNMENT, &pitch) ||
			!Multiply(pitch, static_cast<std::size_t>(upload_rect.height()), &size) ||
			pitch > std::numeric_limits<std::uint32_t>::max())
		{
			m_operation_failed = true;
			return false;
		}

		const int result = m_owner->TransferArena().Allocate(
			size, TEXTURE_UPLOAD_ALIGNMENT, &m_pending_map.staging);
		if (result < 0)
		{
			m_operation_failed = true;
			return false;
		}

		m_pending_map.rect = upload_rect;
		m_pending_map.pitch = static_cast<std::uint32_t>(pitch);
		m_pending_map.level = level;
		map.bits = static_cast<u8*>(m_pending_map.staging.Data());
		map.pitch = static_cast<int>(pitch);
		return true;
	}

	void GSTextureGXM::Unmap()
	{
		if (!m_pending_map.staging)
		{
			m_operation_failed = true;
			Console.Error("GXM texture Unmap called without a matching Map");
			return;
		}

		ArenaAllocation staging = std::move(m_pending_map.staging);
		const GSVector4i rect = m_pending_map.rect;
		const std::uint32_t pitch = m_pending_map.pitch;
		const std::uint32_t level = m_pending_map.level;
		m_pending_map = {};
		if (!QueueUpload(std::move(staging), pitch, rect, level))
			Console.Error("GXM texture mapped upload could not be queued");
	}

	bool GSTextureGXM::QueueUpload(ArenaAllocation staging, std::uint32_t pitch,
		const GSVector4i& rect, std::uint32_t level)
	{
		const TextureLevelLayout* const layout = Level(level);
		if (!layout || !staging)
		{
			m_operation_failed = true;
			return false;
		}

		const bool preserve_contents =
			rect.x != 0 || rect.y != 0 ||
			static_cast<std::uint32_t>(rect.z) != layout->width ||
			static_cast<std::uint32_t>(rect.w) != layout->height;
		std::uint64_t scene_serial = 0;
		if (!m_owner->RetireTextureScene(*this, preserve_contents, &scene_serial))
		{
			m_operation_failed = true;
			return false;
		}
		MarkSceneUse(scene_serial);

		std::uint64_t transfer_serial = 0;
		if (!m_owner->QueueTextureUpload(*this, level, rect, pitch,
				std::move(staging), &transfer_serial))
		{
			m_operation_failed = true;
			return false;
		}
		MarkTransferUse(transfer_serial);
		m_needs_mipmaps_generated = true;
		m_state = State::Dirty;
		return true;
	}

	void GSTextureGXM::GenerateMipmap()
	{
		if (m_mipmap_levels <= 1 || m_native_format.depth_stencil)
		{
			m_operation_failed = true;
			m_needs_mipmaps_generated = true;
			Console.Error("GXM mip generation requested for an incompatible texture");
			return;
		}

		std::uint64_t scene_serial = 0;
		if (!m_owner->RetireTextureScene(*this, true, &scene_serial))
		{
			m_operation_failed = true;
			m_needs_mipmaps_generated = true;
			return;
		}
		MarkSceneUse(scene_serial);

		std::uint64_t transfer_serial = 0;
		if (!m_owner->QueueTextureMipmaps(*this, &transfer_serial))
		{
			m_operation_failed = true;
			m_needs_mipmaps_generated = true;
			Console.Error("GXM texture mip generation could not be queued");
			return;
		}
		MarkTransferUse(transfer_serial);
	}

#ifdef PCSX2_DEVBUILD
	void GSTextureGXM::SetDebugName(std::string_view name)
	{
		m_debug_name.assign(name.data(), name.size());
	}
#endif

	const SceGxmColorSurface* GSTextureGXM::ColorSurface() const
	{
		return m_has_color_surface ? &m_color_surface : nullptr;
	}

	SceGxmColorSurface* GSTextureGXM::ColorSurface()
	{
		return m_has_color_surface ? &m_color_surface : nullptr;
	}

	const SceGxmDepthStencilSurface* GSTextureGXM::DepthStencilSurface() const
	{
		return m_has_depth_surface ? &m_depth_surface : nullptr;
	}

	SceGxmDepthStencilSurface* GSTextureGXM::DepthStencilSurface()
	{
		return m_has_depth_surface ? &m_depth_surface : nullptr;
	}

	const TextureLevelLayout* GSTextureGXM::Level(std::uint32_t level) const
	{
		return level < m_levels.size() ? &m_levels[level] : nullptr;
	}

	void* GSTextureGXM::LevelData(std::uint32_t level) const
	{
		const TextureLevelLayout* const layout = Level(level);
		return (layout && m_storage) ? static_cast<std::uint8_t*>(m_storage.Data()) + layout->offset : nullptr;
	}

	bool GSTextureGXM::CopyFromLinear(std::uint32_t level,
		const GSVector4i& destination, const void* source,
		std::uint32_t source_pitch)
	{
		const TextureLevelLayout* const layout = Level(level);
		if (!layout || !source || !ValidateRect(destination, level))
			return false;
		const std::size_t row_bytes = static_cast<std::size_t>(destination.width()) *
			m_native_format.bytes_per_pixel;
		if (source_pitch < row_bytes)
			return false;

		const auto* input = static_cast<const std::uint8_t*>(source);
		auto* output = static_cast<std::uint8_t*>(LevelData(level));
		if (!layout->tiled)
		{
			output += static_cast<std::size_t>(destination.y) * layout->pitch +
				static_cast<std::size_t>(destination.x) *
				m_native_format.bytes_per_pixel;
			for (int y = 0; y < destination.height(); y++)
			{
				std::memcpy(output + static_cast<std::size_t>(y) * layout->pitch,
					input + static_cast<std::size_t>(y) * source_pitch, row_bytes);
			}
			return true;
		}

		const std::size_t width_in_tiles =
			(layout->pitch / m_native_format.bytes_per_pixel) / 32;
		for (int row = 0; row < destination.height(); row++)
		{
			const std::uint32_t y = static_cast<std::uint32_t>(destination.y + row);
			std::uint32_t x = static_cast<std::uint32_t>(destination.x);
			const std::uint32_t end_x = static_cast<std::uint32_t>(destination.z);
			const std::uint8_t* row_input =
				input + static_cast<std::size_t>(row) * source_pitch;
			while (x < end_x)
			{
				const std::uint32_t run = std::min<std::uint32_t>(
					end_x - x, 32 - (x & 31));
				const std::size_t pixel =
					(((static_cast<std::size_t>(y >> 5) * width_in_tiles) +
						(x >> 5)) * 1024) +
					(static_cast<std::size_t>(y & 31) * 32) + (x & 31);
				std::memcpy(output + pixel * m_native_format.bytes_per_pixel,
					row_input + static_cast<std::size_t>(x - destination.x) *
						m_native_format.bytes_per_pixel,
					static_cast<std::size_t>(run) * m_native_format.bytes_per_pixel);
				x += run;
			}
		}
		return true;
	}

	bool GSTextureGXM::CopyToLinear(std::uint32_t level,
		const GSVector4i& source, void* destination,
		std::uint32_t destination_pitch) const
	{
		const TextureLevelLayout* const layout = Level(level);
		if (!layout || !destination || !ValidateRect(source, level))
			return false;
		const std::size_t row_bytes = static_cast<std::size_t>(source.width()) *
			m_native_format.bytes_per_pixel;
		if (destination_pitch < row_bytes)
			return false;

		const auto* input = static_cast<const std::uint8_t*>(LevelData(level));
		auto* output = static_cast<std::uint8_t*>(destination);
		if (!layout->tiled)
		{
			input += static_cast<std::size_t>(source.y) * layout->pitch +
				static_cast<std::size_t>(source.x) *
				m_native_format.bytes_per_pixel;
			for (int y = 0; y < source.height(); y++)
			{
				std::memcpy(output + static_cast<std::size_t>(y) * destination_pitch,
					input + static_cast<std::size_t>(y) * layout->pitch, row_bytes);
			}
			return true;
		}

		const std::size_t width_in_tiles =
			(layout->pitch / m_native_format.bytes_per_pixel) / 32;
		for (int row = 0; row < source.height(); row++)
		{
			const std::uint32_t y = static_cast<std::uint32_t>(source.y + row);
			std::uint32_t x = static_cast<std::uint32_t>(source.x);
			const std::uint32_t end_x = static_cast<std::uint32_t>(source.z);
			std::uint8_t* row_output =
				output + static_cast<std::size_t>(row) * destination_pitch;
			while (x < end_x)
			{
				const std::uint32_t run = std::min<std::uint32_t>(
					end_x - x, 32 - (x & 31));
				const std::size_t pixel =
					(((static_cast<std::size_t>(y >> 5) * width_in_tiles) +
						(x >> 5)) * 1024) +
					(static_cast<std::size_t>(y & 31) * 32) + (x & 31);
				std::memcpy(row_output + static_cast<std::size_t>(x - source.x) *
						m_native_format.bytes_per_pixel,
					input + pixel * m_native_format.bytes_per_pixel,
					static_cast<std::size_t>(run) * m_native_format.bytes_per_pixel);
				x += run;
			}
		}
		return true;
	}

	void GSTextureGXM::MarkSceneUse(std::uint64_t serial)
	{
		m_fence.scene = std::max(m_fence.scene, serial);
	}

	void GSTextureGXM::MarkTransferUse(std::uint64_t serial)
	{
		m_fence.transfer = std::max(m_fence.transfer, serial);
	}

	bool GSTextureGXM::ValidateRect(const GSVector4i& rect,
		std::uint32_t level) const
	{
		const TextureLevelLayout* const layout = Level(level);
		return layout && RectWithin(rect, layout->width, layout->height);
	}

	void GSTextureGXM::RetireStorage()
	{
		if (m_storage)
			m_owner->RetireTextureAllocation(std::move(m_storage), m_fence);
		if (m_stencil_storage)
			m_owner->RetireTextureAllocation(std::move(m_stencil_storage), m_fence);
	}

	GSDownloadTextureGXM::GSDownloadTextureGXM(TextureOwner* owner,
		std::uint32_t width,
		std::uint32_t height,
		GSTexture::Format format)
		: GSDownloadTexture(width, height, format)
		, m_owner(owner)
	{
	}

	std::unique_ptr<GSDownloadTextureGXM>
	GSDownloadTextureGXM::Create(TextureOwner* owner, std::uint32_t width,
		std::uint32_t height, GSTexture::Format format,
		int* error)
	{
		SetError(error, 0);
		if (!owner)
		{
			SetError(error, SCE_GXM_ERROR_INVALID_POINTER);
			return {};
		}

		std::unique_ptr<GSDownloadTextureGXM> texture(
			new GSDownloadTextureGXM(owner, width, height, format));
		const int result = texture->Initialize();
		if (result < 0)
		{
			SetError(error, result);
			return {};
		}
		return texture;
	}

	GSDownloadTextureGXM::~GSDownloadTextureGXM()
	{
		m_map_pointer = nullptr;
		if (m_storage)
			m_owner->RetireTextureAllocation(std::move(m_storage), m_fence);
	}

	int GSDownloadTextureGXM::Initialize()
	{
		const TextureFormatInfo* const native_format = GetTextureFormatInfo(m_format);
		if (!native_format)
			return SCE_GXM_ERROR_UNSUPPORTED;
		if (m_width == 0 || m_height == 0 || m_width > 4096 || m_height > 4096)
			return SCE_GXM_ERROR_INVALID_VALUE;

		const std::uint64_t unaligned_pitch =
			static_cast<std::uint64_t>(m_width) * native_format->bytes_per_pixel;
		if (unaligned_pitch > std::numeric_limits<std::size_t>::max())
			return SCE_GXM_ERROR_INVALID_VALUE;
		std::size_t pitch = 0;
		std::size_t size = 0;
		if (!AlignUp(static_cast<std::size_t>(unaligned_pitch),
				DOWNLOAD_PITCH_ALIGNMENT, &pitch) ||
			!Multiply(pitch, m_height, &size) ||
			pitch > std::numeric_limits<std::uint32_t>::max())
		{
			return SCE_GXM_ERROR_INVALID_VALUE;
		}

		const int result = m_owner->TransferArena().Allocate(
			size, DOWNLOAD_PITCH_ALIGNMENT, &m_storage);
		if (result < 0)
			return result;

		m_storage_pitch = static_cast<std::uint32_t>(pitch);
		m_current_pitch = m_storage_pitch;
		m_map_pointer = static_cast<const u8*>(m_storage.Data());
		return 0;
	}

	void GSDownloadTextureGXM::CopyFromTexture(const GSVector4i& destination,
		GSTexture* source_texture,
		const GSVector4i& source,
		std::uint32_t source_level,
		bool use_transfer_pitch)
	{
		if (m_needs_flush)
			Flush();
		if (m_needs_flush || !source_texture)
		{
			m_operation_failed = true;
			return;
		}

		GSTextureGXM* const source_gxm = static_cast<GSTextureGXM*>(source_texture);
		const TextureLevelLayout* const source_layout =
			source_gxm->Level(source_level);
		if (source_gxm->GetFormat() != m_format || !source_layout ||
			destination.width() != source.width() ||
			destination.height() != source.height() ||
			!RectWithin(source, source_layout->width, source_layout->height) ||
			!ValidateRect(destination) ||
			(use_transfer_pitch && (destination.x != 0 || destination.y != 0)))
		{
			m_operation_failed = true;
			Console.Error("Invalid GXM texture readback request");
			return;
		}

		m_current_pitch = GetTransferPitch(
			use_transfer_pitch ? static_cast<std::uint32_t>(destination.width()) : m_width,
			DOWNLOAD_PITCH_ALIGNMENT);
		std::uint32_t copy_offset = 0;
		std::uint32_t copy_size = 0;
		std::uint32_t copy_rows = 0;
		GetTransferSize(destination, &copy_offset, &copy_size, &copy_rows);
		const std::uint64_t copy_end =
			static_cast<std::uint64_t>(copy_offset) +
			(copy_rows == 0 ? 0 : static_cast<std::uint64_t>(copy_rows - 1) * m_current_pitch) +
			copy_size;
		if (copy_end > m_storage.Size())
		{
			m_operation_failed = true;
			Console.Error("GXM texture readback exceeds its download allocation");
			return;
		}

		std::uint64_t scene_serial = 0;
		if (!m_owner->RetireTextureScene(*source_gxm, true, &scene_serial))
		{
			m_operation_failed = true;
			return;
		}
		source_gxm->MarkSceneUse(scene_serial);

		std::uint64_t transfer_serial = 0;
		if (!m_owner->QueueTextureReadback(*source_gxm, source_level, source,
				m_storage.Data(), m_current_pitch,
				destination, &transfer_serial))
		{
			m_operation_failed = true;
			Console.Error("GXM texture readback could not be queued");
			return;
		}

		source_gxm->MarkTransferUse(transfer_serial);
		m_pending_transfer = transfer_serial;
		m_fence.transfer = std::max(m_fence.transfer, transfer_serial);
		m_needs_flush = true;
	}

	bool GSDownloadTextureGXM::Map(const GSVector4i& read_rect)
	{
		return !m_operation_failed && !m_needs_flush && ValidateRect(read_rect) &&
		       m_map_pointer != nullptr;
	}

	void GSDownloadTextureGXM::Unmap()
	{
		// MAIN_NC download memory is persistently CPU-addressable. There is no
		// mapping state to tear down; synchronization is owned exclusively by
		// Flush() and the transfer serial.
	}

	void GSDownloadTextureGXM::Flush()
	{
		if (!m_needs_flush)
			return;
		if (!m_owner->WaitForTextureTransfer(m_pending_transfer))
		{
			m_operation_failed = true;
			Console.Error("Waiting for GXM texture readback failed");
			return;
		}

		m_pending_transfer = 0;
		m_needs_flush = false;
	}

#ifdef PCSX2_DEVBUILD
	void GSDownloadTextureGXM::SetDebugName(std::string_view name)
	{
		m_debug_name.assign(name.data(), name.size());
	}
#endif

	bool GSDownloadTextureGXM::ValidateRect(const GSVector4i& rect) const
	{
		return RectWithin(rect, m_width, m_height);
	}
} // namespace VitaGXM

#endif
