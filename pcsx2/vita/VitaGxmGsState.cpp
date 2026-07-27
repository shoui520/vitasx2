// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "vita/VitaGxmGsState.h"
#include "vita/VitaGpuVuDraw.h"
#include "common/Console.h"

#if defined(VITASX2_QEMU_VALIDATION) && VITASX2_QEMU_VALIDATION

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

VitaGxmGsState::VitaGxmGsState(bool enable_native_presenter)
	: GSRendererSW(0)
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
	// Match the hardware path's guest-VSync lifetime even though Linux/QEMU has
	// no display device.
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

#elif defined(VITASX2_VITA_SOFTWARE_GS_CONTROL) && \
	VITASX2_VITA_SOFTWARE_GS_CONTROL

#include "GS/Renderers/Common/GSDevice.h"

VitaGxmGsState::VitaGxmGsState(bool enable_native_presenter)
	: GSRendererSW(GSConfig.SWExtraThreads)
{
	(void)enable_native_presenter;
}

VitaGxmGsState::~VitaGxmGsState() = default;

bool VitaGxmGsState::IsNativePresenterReady() const
{
	return g_gs_device != nullptr;
}

void VitaGxmGsState::Reset(bool hardware_reset)
{
	GSRendererSW::Reset(hardware_reset);
}

void VitaGxmGsState::VSync(u32 field, bool registers_written, bool idle_frame)
{
	GSRendererSW::VSync(field, registers_written, idle_frame);
}

#else

#include "GS/Renderers/Common/GSDevice.h"
#include "vita/GSDeviceGXM.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace
{
	constexpr u32 ProxyVertexCount = 4;

	u16 ProxyCoordinate(s32 pixel, u32 offset)
	{
		const s64 fixed = static_cast<s64>(pixel) * 16 +
			static_cast<s64>(offset & 0xffffu);
		return static_cast<u16>(std::clamp<s64>(fixed, 0, 0xffff));
	}

	bool BuildGpuVuStatePacket(const VitaGpuVu::GpuVuDraw& draw,
		const GSDrawingContext& context, std::vector<u128>* packet)
	{
		if (!packet || !draw.static_gs_writes.empty())
			return false;

		GIFTag tag{};
		std::memcpy(&tag, draw.gif_tag.data(), sizeof(tag));
		const u32 nreg = tag.NREG == 0 ? 16u : tag.NREG;
		if (!tag.PRE || tag.FLG != GIF_FLG_PACKED || nreg == 0 ||
			nreg > 16)
		{
			return false;
		}
		tag.NLOOP = ProxyVertexCount;
		tag.EOP = 1;

		packet->assign(1 + ProxyVertexCount * nreg, {});
		std::memcpy(&(*packet)[0], &tag, sizeof(tag));
		const GSVector4i scissor = context.scissor.in;
		const s32 right = std::max(scissor.x, scissor.z - 1);
		const s32 bottom = std::max(scissor.y, scissor.w - 1);
		const std::array<s32, ProxyVertexCount> x = {
			scissor.x, right, scissor.x, right};
		const std::array<s32, ProxyVertexCount> y = {
			scissor.y, scissor.y, bottom, bottom};

		for (u32 vertex = 0; vertex < ProxyVertexCount; vertex++)
		{
			for (u32 reg_index = 0; reg_index < nreg; reg_index++)
			{
				const u32 reg = (tag.REGS >> (reg_index * 4)) & 0x0f;
				GIFPackedReg packed{};
				const bool high = (vertex & 1u) != 0;
				switch (reg)
				{
					case GIF_REG_RGBA:
						packed.RGBA.R = high ? 0xff : 0;
						packed.RGBA.G = high ? 0xff : 0;
						packed.RGBA.B = high ? 0xff : 0;
						packed.RGBA.A = high ? 0xff : 0;
						break;
					case GIF_REG_STQ:
						packed.STQ.S = high ? 1.0f : 0.0f;
						packed.STQ.T = vertex >= 2 ? 1.0f : 0.0f;
						packed.STQ.Q = 1.0f;
						break;
					case GIF_REG_UV:
						packed.UV.U = high ? 0x3fff : 0;
						packed.UV.V = vertex >= 2 ? 0x3fff : 0;
						break;
					case GIF_REG_XYZF2:
						packed.XYZF2.X = ProxyCoordinate(
							x[vertex], context.XYOFFSET.OFX);
						packed.XYZF2.Y = ProxyCoordinate(
							y[vertex], context.XYOFFSET.OFY);
						packed.XYZF2.Z = high ? 0x00ffffffu : 0;
						packed.XYZF2.F = high ? 0xff : 0;
						packed.XYZF2.ADC = 0;
						break;
					case GIF_REG_XYZ2:
						packed.XYZ2.X = ProxyCoordinate(
							x[vertex], context.XYOFFSET.OFX);
						packed.XYZ2.Y = ProxyCoordinate(
							y[vertex], context.XYOFFSET.OFY);
						packed.XYZ2.Z = high ? 0xffffffffu : 0;
						packed.XYZ2.ADC = 0;
						break;
					case GIF_REG_FOG:
						packed.FOG.F = high ? 0xff : 0;
						break;
					case GIF_REG_NOP:
						break;
					default:
						return false;
				}
				std::memcpy(&(*packet)[1 + vertex * nreg + reg_index],
					&packed, sizeof(packed));
			}
		}
		return true;
	}

	bool SameGpuVuUniforms(const VitaGpuVu::GpuVuDraw& left,
		const VitaGpuVu::GpuVuDraw& right)
	{
		const auto& left_vf = left.VfUniforms();
		const auto& right_vf = right.VfUniforms();
		const auto& left_constants = left.ConstantUniforms();
		const auto& right_constants = right.ConstantUniforms();
		if (left_vf.size() != right_vf.size() ||
			left_constants.size() != right_constants.size() ||
			left.acc_uniform != right.acc_uniform ||
			left.scalar_uniforms.present != right.scalar_uniforms.present ||
			left.scalar_uniforms.q != right.scalar_uniforms.q ||
			left.scalar_uniforms.p != right.scalar_uniforms.p ||
			left.scalar_uniforms.i != right.scalar_uniforms.i ||
			left.scalar_uniforms.gif_q != right.scalar_uniforms.gif_q)
		{
			return false;
		}
		if (left.UniformBlock().Get() == right.UniformBlock().Get())
			return true;
		for (u32 index = 0; index < left_vf.size(); index++)
		{
			if (left_vf[index].register_index !=
					right_vf[index].register_index ||
				left_vf[index].bits != right_vf[index].bits)
			{
				return false;
			}
		}
		for (u32 index = 0; index < left_constants.size(); index++)
		{
			if (left_constants[index].input_index !=
					right_constants[index].input_index ||
				left_constants[index].bits != right_constants[index].bits)
			{
				return false;
			}
		}
		return true;
	}

	using GpuVuInputGenerations =
		std::array<VitaGpuVu::RawVifPayloadRef,
			VitaGpuVu::InputRingSlotCount>;

	bool CollectGpuVuInputGenerations(const VitaGpuVu::GpuVuDraw& draw,
		GpuVuInputGenerations* generations, u32* generation_count)
	{
		if (!generations || !generation_count)
			return false;
		*generation_count = 0;
		for (const VitaGpuVu::RawVifPayloadRef& payload :
			draw.InputPayloads())
		{
			if (!payload.IsValid())
				return false;
			bool seen = false;
			for (u32 index = 0; index < *generation_count; index++)
			{
				const VitaGpuVu::RawVifPayloadRef& existing =
					(*generations)[index];
				if (existing.owner == payload.owner &&
					existing.slot == payload.slot &&
					existing.generation == payload.generation)
				{
					seen = true;
					break;
				}
			}
			if (seen)
				continue;
			if (*generation_count >= generations->size())
				return false;
			(*generations)[(*generation_count)++] = payload;
		}
		return *generation_count != 0;
	}

	bool SameGpuVuInputGenerations(const VitaGpuVu::GpuVuDraw& left,
		const VitaGpuVu::GpuVuDraw& right)
	{
		GpuVuInputGenerations left_generations{};
		GpuVuInputGenerations right_generations{};
		u32 left_count = 0;
		u32 right_count = 0;
		if (!CollectGpuVuInputGenerations(
				left, &left_generations, &left_count) ||
			!CollectGpuVuInputGenerations(
				right, &right_generations, &right_count) ||
			left_count != right_count)
		{
			return false;
		}
		for (u32 left_index = 0; left_index < left_count; left_index++)
		{
			bool found = false;
			for (u32 right_index = 0; right_index < right_count; right_index++)
			{
				const VitaGpuVu::RawVifPayloadRef& left_payload =
					left_generations[left_index];
				const VitaGpuVu::RawVifPayloadRef& right_payload =
					right_generations[right_index];
				if (left_payload.owner == right_payload.owner &&
					left_payload.slot == right_payload.slot &&
					left_payload.generation == right_payload.generation)
				{
					found = true;
					break;
				}
			}
			if (!found)
				return false;
		}
		return true;
	}

	bool CanDeriveOneGpuVuState(const VitaGpuVu::GpuVuDraw& left,
		const VitaGpuVu::GpuVuDraw& right)
	{
		return left.program == right.program &&
			left.gif_tag == right.gif_tag &&
			left.direct_tfx.vertex_count == right.direct_tfx.vertex_count &&
			left.direct_tfx.primitive == right.direct_tfx.primitive &&
			left.direct_tfx.gouraud == right.direct_tfx.gouraud &&
			left.direct_tfx.textured == right.direct_tfx.textured &&
			left.direct_tfx.fog_enabled == right.direct_tfx.fog_enabled &&
			left.direct_tfx.fixed_texture_coordinates ==
				right.direct_tfx.fixed_texture_coordinates &&
			left.invocation_count == right.invocation_count &&
			left.vertex_count == right.vertex_count &&
			left.primitive_count == right.primitive_count &&
			left.index_count == right.index_count &&
			left.lowering == right.lowering &&
			left.execution == right.execution &&
			left.primitive_boundary == right.primitive_boundary &&
				left.static_gs_writes.empty() && right.static_gs_writes.empty() &&
				!left.final_state.IsRequired() &&
				!right.final_state.IsRequired() &&
				SameGpuVuInputGenerations(left, right) &&
				SameGpuVuUniforms(left, right);
	}
}

VitaGxmGsState::VitaGxmGsState(bool enable_native_presenter)
	: GSRendererHW()
{
	(void)enable_native_presenter;
}

VitaGxmGsState::~VitaGxmGsState() = default;

bool VitaGxmGsState::IsNativePresenterReady() const
{
	return g_gs_device != nullptr;
}

void VitaGxmGsState::SubmitDrawConfig(GSHWDrawConfig& config)
{
	if (m_gpu_vu_draws.empty())
	{
		GSRendererHW::SubmitDrawConfig(config);
		return;
	}

	auto* const device = static_cast<GSDeviceGXM*>(g_gs_device.get());
	std::vector<std::unique_ptr<VitaGpuVu::GpuVuDraw>> draws =
		std::move(m_gpu_vu_draws);
	const u32 count = static_cast<u32>(draws.size());
	if (!device || !device->RenderGpuVuDraws(config, std::move(draws)))
	{
		for (u32 index = 0; index < count; index++)
			VitaGpuVu::RecordGpuVuDrawRejected();
	}
}

#endif

void VitaGxmGsState::ConsumeGpuVuDraw(
	std::unique_ptr<VitaGpuVu::GpuVuDraw> draw)
{
	if (!draw)
		return;
	std::vector<std::unique_ptr<VitaGpuVu::GpuVuDraw>> draws;
	draws.push_back(std::move(draw));
	ConsumeGpuVuDraws(std::move(draws));
}

void VitaGxmGsState::ConsumeGpuVuDraws(
	std::vector<std::unique_ptr<VitaGpuVu::GpuVuDraw>> draws)
{
	if (draws.empty())
		return;
	#if !defined(VITASX2_QEMU_VALIDATION) && \
		!(defined(VITASX2_VITA_SOFTWARE_GS_CONTROL) && \
			VITASX2_VITA_SOFTWARE_GS_CONTROL)
	if (!m_gpu_vu_draws.empty())
	{
		for (const auto& draw : draws)
			VitaGpuVu::RecordGpuVuDrawRejected();
		pxFailRel("nested GPU-VU draw reached GS state");
		return;
	}

	for (u32 first = 0; first < draws.size();)
	{
		if (!draws[first])
		{
			first++;
			continue;
		}
		if (!draws[first]->WasValidatedForQueue())
		{
			VitaGpuVu::RecordGpuVuDrawRejected();
			Console.Error(
				"GPU-VU: GS received a descriptor outside the validated queue");
			draws[first].reset();
			first++;
			continue;
		}

		u32 end = first + 1;
		while (end < draws.size() && draws[end] &&
			draws[end]->WasValidatedForQueue() &&
			CanDeriveOneGpuVuState(*draws[first], *draws[end]))
		{
			end++;
		}

		// Complete any older PATH output, then feed one descriptor-scale proxy
		// through PCSX2's owning GS state derivation for the whole consecutive
		// compatible run. The generated root consumes every immutable VIF span;
		// none of the four conservative proxy vertices reaches GXM.
		Flush(GSFlushReason::CONTEXTCHANGE);
		std::vector<u128> packet;
		if (!BuildGpuVuStatePacket(*draws[first], *m_context, &packet))
		{
			for (u32 index = first; index < end; index++)
			{
				VitaGpuVu::RecordGpuVuDrawRejected();
				draws[index].reset();
			}
			first = end;
			continue;
		}
		m_gpu_vu_draws.reserve(end - first);
		for (u32 index = first; index < end; index++)
			m_gpu_vu_draws.push_back(std::move(draws[index]));
		Transfer<3>(reinterpret_cast<const u8*>(packet.data()),
			static_cast<u32>(packet.size()));
		Flush(GSFlushReason::CONTEXTCHANGE);
		if (!m_gpu_vu_draws.empty())
		{
			for (const auto& pending : m_gpu_vu_draws)
				VitaGpuVu::RecordGpuVuDrawRejected();
			m_gpu_vu_draws.clear();
			pxFailRel("GPU-VU proxy geometry produced no PCSX2 HW draw");
		}
		first = end;
	}
	return;
	#endif

	for (const auto& draw : draws)
		VitaGpuVu::RecordGpuVuDrawRejected();
#if !defined(VITASX2_QEMU_VALIDATION) || !VITASX2_QEMU_VALIDATION
	pxFailRel("GPU-VU draw reached GS before native geometry consumption");
#endif
}
