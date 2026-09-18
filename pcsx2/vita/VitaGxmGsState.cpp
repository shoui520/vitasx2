// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "vita/VitaGxmGsState.h"
#include "vita/VitaGpuVuDraw.h"
#include "vita/VitaGpuVuUniversalEpoch.h"
#include "vita/VitaGsMailbox.h"
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

	bool SameGpuVuUniformLayout(const VitaGpuVu::GpuVuDraw& left,
		const VitaGpuVu::GpuVuDraw& right)
	{
		const auto& left_vf = left.VfUniforms();
		const auto& right_vf = right.VfUniforms();
		const auto& left_constants = left.ConstantUniforms();
		const auto& right_constants = right.ConstantUniforms();
		if (left_vf.size() != right_vf.size() ||
			left_constants.size() != right_constants.size() ||
			left.scalar_uniforms.present != right.scalar_uniforms.present)
		{
			return false;
		}
		for (u32 index = 0; index < left_vf.size(); index++)
		{
			if (left_vf[index].register_index !=
				right_vf[index].register_index)
			{
				return false;
			}
		}
		for (u32 index = 0; index < left_constants.size(); index++)
		{
			if (left_constants[index].input_index !=
				right_constants[index].input_index)
			{
				return false;
			}
		}
		return true;
	}

	bool SameGpuVuUniforms(const VitaGpuVu::GpuVuDraw& left,
		const VitaGpuVu::GpuVuDraw& right)
	{
		if (!SameGpuVuUniformLayout(left, right) ||
			left.acc_uniform != right.acc_uniform ||
			left.scalar_uniforms.q != right.scalar_uniforms.q ||
			left.scalar_uniforms.p != right.scalar_uniforms.p ||
			left.scalar_uniforms.i != right.scalar_uniforms.i ||
			left.scalar_uniforms.gif_q != right.scalar_uniforms.gif_q)
		{
			return false;
		}
		if (left.UniformBlock().Get() == right.UniformBlock().Get())
			return true;
		const auto& left_vf = left.VfUniforms();
		const auto& right_vf = right.VfUniforms();
		const auto& left_constants = left.ConstantUniforms();
		const auto& right_constants = right.ConstantUniforms();
		for (u32 index = 0; index < left_vf.size(); index++)
		{
			if (left_vf[index].bits != right_vf[index].bits)
				return false;
		}
		for (u32 index = 0; index < left_constants.size(); index++)
		{
			if (left_constants[index].bits != right_constants[index].bits)
				return false;
		}
		return true;
	}

	bool SameGpuVuGsStateContract(const VitaGpuVu::GpuVuDraw& left,
		const VitaGpuVu::GpuVuDraw& right)
	{
		if (!left.static_gs_writes.empty() || !right.static_gs_writes.empty())
			return false;

		GIFTag left_tag{};
		GIFTag right_tag{};
		std::memcpy(&left_tag, left.gif_tag.data(), sizeof(left_tag));
		std::memcpy(&right_tag, right.gif_tag.data(), sizeof(right_tag));
		// BuildGpuVuStatePacket() replaces NLOOP/EOP with its four conservative
		// proxy vertices. Every remaining GIFtag field and the current GS context
		// determine the PCSX2 hardware config. Geometry count and generated GXP
		// identity are device bindings, not GS state.
		return left_tag.PRE == right_tag.PRE &&
			left_tag.PRIM == right_tag.PRIM &&
			left_tag.FLG == right_tag.FLG &&
			left_tag.NREG == right_tag.NREG &&
			left_tag.REGS == right_tag.REGS &&
			left.direct_tfx.primitive == right.direct_tfx.primitive &&
			left.direct_tfx.gouraud == right.direct_tfx.gouraud &&
			left.direct_tfx.textured == right.direct_tfx.textured &&
			left.direct_tfx.fog_enabled == right.direct_tfx.fog_enabled &&
			left.direct_tfx.fixed_texture_coordinates ==
				right.direct_tfx.fixed_texture_coordinates;
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

        const char* DescribeGpuVuStateBatchIncompatibility(
            const VitaGpuVu::GpuVuDraw &left,
            const VitaGpuVu::GpuVuDraw &right) {
          // Expanded roots fetch each object's immutable VU seeds from their
          // batch record. Native roots still expose those seeds through the
          // one-draw default uniform buffer and therefore require exact
          // equality.
          const bool private_transactional_final_state =
              left.HasGeneratedLoopKernelTransaction() &&
              right.HasGeneratedLoopKernelTransaction() &&
              (left.primitive_boundary ==
                   VitaGpuVu::PrimitiveBoundary::ExactPostLoopIndexed ||
               left.HasExpandedExactIndices()) &&
              right.primitive_boundary == left.primitive_boundary;
          const bool variable_capacity_batch =
              private_transactional_final_state &&
              left.program == right.program;
          const auto same_tag_except_nloop = [](const auto& first,
                                                const auto& second) {
            auto normalized_first = first;
            auto normalized_second = second;
            normalized_first[0] &= ~0x7fffu;
            normalized_second[0] &= ~0x7fffu;
            return normalized_first == normalized_second;
          };
          if (left.program != right.program)
            return "program";
          if (!VitaGpuVu::HasSamePrivateStoreBufferBinding(left, right))
            return "private-store-buffer-binding";
          if (left.precompute_program_count != right.precompute_program_count ||
              left.precompute_stage_count != right.precompute_stage_count ||
              !std::equal(left.precompute_programs.begin(),
                          left.precompute_programs.begin() +
                              left.precompute_program_count,
                          right.precompute_programs.begin()) ||
              !std::equal(left.precompute_stages.begin(),
                          left.precompute_stages.begin() +
                              left.precompute_program_count,
                          right.precompute_stages.begin()))
            return "precompute";
          if (!(variable_capacity_batch ?
                    same_tag_except_nloop(left.gif_tag, right.gif_tag) :
                    left.gif_tag == right.gif_tag))
            return "gif-tag";
          if (!variable_capacity_batch &&
              left.direct_tfx.vertex_count != right.direct_tfx.vertex_count)
            return "contract-vertices";
          if (left.direct_tfx.primitive != right.direct_tfx.primitive ||
              left.direct_tfx.gouraud != right.direct_tfx.gouraud ||
              left.direct_tfx.textured != right.direct_tfx.textured ||
              left.direct_tfx.fog_enabled != right.direct_tfx.fog_enabled ||
              left.direct_tfx.fixed_texture_coordinates !=
                  right.direct_tfx.fixed_texture_coordinates)
            return "direct-tfx-state";
          if (!variable_capacity_batch &&
              (left.invocation_count != right.invocation_count ||
               left.vertex_count != right.vertex_count ||
               left.primitive_count != right.primitive_count ||
               left.index_count != right.index_count))
            return "geometry";
          if (left.lowering != right.lowering ||
              left.execution != right.execution ||
              left.primitive_boundary != right.primitive_boundary)
            return "execution";
          if (!left.static_gs_writes.empty() ||
              !right.static_gs_writes.empty())
            return "static-gs-write";
          if ((left.final_state.IsRequired() ||
               right.final_state.IsRequired()) &&
              !private_transactional_final_state)
            return "final-state";
                 // Exact-post-loop ABI-26 descriptors carry one independently
                 // retained raw-binding record per object.  Their VIF
                 // input-ring generation is therefore data ownership, not
                 // GS-state identity; GSDeviceGXM validates and groups each
                 // generation before encoding. Native/legacy roots still
                 // require one identical generation set.
          if (!private_transactional_final_state &&
              !SameGpuVuInputGenerations(left, right))
            return "input-generation";
          const bool indexed =
              left.primitive_boundary ==
                  VitaGpuVu::PrimitiveBoundary::InstanceIndexed ||
              left.primitive_boundary ==
                  VitaGpuVu::PrimitiveBoundary::ExpandedIndexed ||
              left.primitive_boundary ==
                  VitaGpuVu::PrimitiveBoundary::ExactPostLoopIndexed ||
              left.HasExpandedExactIndices();
          if (!(indexed ? SameGpuVuUniformLayout(left, right) :
                          SameGpuVuUniforms(left, right)))
            return "uniform-layout";
          return nullptr;
        }

        bool CanDeriveOneGpuVuState(const VitaGpuVu::GpuVuDraw &left,
                                    const VitaGpuVu::GpuVuDraw &right) {
          return DescribeGpuVuStateBatchIncompatibility(left, right) ==
                 nullptr;
        }
        } // namespace

        VitaGxmGsState::VitaGxmGsState(bool enable_native_presenter)
            : GSRendererHW() {
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
	// ConsumeGpuVuDraws() may have more generated-program groups with this
	// exact GS contract. Save the fully PCSX2-derived config before the first
	// group is moved into the device; transient proxy vertex/index pointers are
	// never consulted by GSDeviceGXM's active GPU-VU path.
	m_gpu_vu_derived_config = config;
	m_gpu_vu_derived_config_valid = true;
	m_gpu_vu_derived_submit_succeeded =
		device && device->RenderGpuVuDraws(config, std::move(draws));
	if (!m_gpu_vu_derived_submit_succeeded)
	{
		for (u32 index = 0; index < count; index++)
			VitaGpuVu::RecordGpuVuDrawRejected();
	}
}

#endif

void VitaGxmGsState::ServiceUniversalGpuVuEpoch(
	VitaGpuVu::UniversalGpuVuEpoch* epoch)
{
	if (!epoch)
		return;
#if (!defined(VITASX2_QEMU_VALIDATION) || !VITASX2_QEMU_VALIDATION) && \
	(!defined(VITASX2_VITA_SOFTWARE_GS_CONTROL) || \
	 !VITASX2_VITA_SOFTWARE_GS_CONTROL)
	auto* const device = static_cast<GSDeviceGXM*>(g_gs_device.get());
	if (device)
	{
		device->ServiceUniversalGpuVuEpoch(epoch);
		return;
	}
#endif
	if (epoch->Stage() == VitaGpuVu::UniversalGpuVuEpochStage::Prepared &&
		epoch->MarkSubmitted())
	{
		epoch->MarkGpuRejected(
			VitaGpuVu::UniversalGpuVuRejection::DeviceUnavailable, 0);
	}
}


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

	auto* const device = static_cast<GSDeviceGXM*>(g_gs_device.get());
	const u32 consumer_total_draws = static_cast<u32>(draws.size());
	u32 consumer_group_index = 0u;
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

		u32 state_end = first + 1;
		while (state_end < draws.size() && draws[state_end] &&
			draws[state_end]->WasValidatedForQueue() &&
			SameGpuVuGsStateContract(*draws[first], *draws[state_end]))
		{
			state_end++;
		}

		// Complete older PATH output once, then feed exactly one conservative
		// proxy through PCSX2's owning GS state derivation. Subsequent generated
		// program groups in this contiguous run reuse that immutable config;
		// there is no intervening GIF packet or GS register write which could
		// change it. Each group still reaches GSDeviceGXM in original order and
		// receives its full program/input/target validation there.
		Flush(GSFlushReason::CONTEXTCHANGE);
		m_gpu_vu_derived_config_valid = false;
		m_gpu_vu_derived_submit_succeeded = false;
		u32 group_count = 0;
		u32 reused_group_count = 0;
		u32 object_count = 0;
		for (u32 group_first = first; group_first < state_end;)
		{
			u32 group_end = group_first + 1;
			while (group_end < state_end && draws[group_end] &&
				draws[group_end]->WasValidatedForQueue() &&
				CanDeriveOneGpuVuState(
					*draws[group_first], *draws[group_end]))
			{
				group_end++;
			}
			if (group_end < state_end && draws[group_end] &&
				draws[group_end]->WasValidatedForQueue())
			{
				const char* const reason =
					DescribeGpuVuStateBatchIncompatibility(
						*draws[group_first], *draws[group_end]);
				static u64 s_gpu_vu_state_batch_splits = 0;
				const u64 split = ++s_gpu_vu_state_batch_splits;
				if (reason && (split <= 8u || (split & (split - 1u)) == 0u))
				{
					Console.WriteLn(
						"GPU-VU gs_state_batch_split=%llu reason=%s "
						"left_vertices=%u right_vertices=%u "
						"left_program=%016llx%016llx "
						"right_program=%016llx%016llx pre_effect=0.",
						static_cast<unsigned long long>(split), reason,
						draws[group_first]->vertex_count,
						draws[group_end]->vertex_count,
						static_cast<unsigned long long>(
							draws[group_first]->program.high),
						static_cast<unsigned long long>(
							draws[group_first]->program.low),
						static_cast<unsigned long long>(
							draws[group_end]->program.high),
						static_cast<unsigned long long>(
							draws[group_end]->program.low));
				}
			}
			const u32 group_objects = group_end - group_first;
			object_count += group_objects;
			group_count++;
			// flags: bit 0 identifies the proxy-derived first group; bit 1
			// records a successful device return; bit 2 records a reusable
			// PCSX2-derived draw config. The notification thread owns only this
			// pointer-free copy and can report it while MTGS is stalled.
			VitaGS::GpuVuGxmConsumerBreadcrumb consumer{
				consumer_total_draws, first, state_end, group_first, group_end,
				consumer_group_index,
				group_first == first ? 1u : 0u};
			VitaGS::UpdateActiveGpuVuGxmSubmissionWatchdog(
				VitaGS::GpuVuGxmSubmissionStage::GsStateGroupBegin,
				consumer);

			if (group_first == first)
			{
				std::vector<u128> packet;
				if (!BuildGpuVuStatePacket(
						*draws[group_first], *m_context, &packet))
				{
					for (u32 index = group_first; index < state_end; index++)
					{
						if (draws[index])
							VitaGpuVu::RecordGpuVuDrawRejected();
						draws[index].reset();
					}
					group_first = state_end;
					continue;
				}
				m_gpu_vu_draws.reserve(group_objects);
				for (u32 index = group_first; index < group_end; index++)
					m_gpu_vu_draws.push_back(std::move(draws[index]));
				Transfer<3>(reinterpret_cast<const u8*>(packet.data()),
					static_cast<u32>(packet.size()));
				Flush(GSFlushReason::CONTEXTCHANGE);
				if (m_gpu_vu_derived_submit_succeeded)
					consumer.flags |= 1u << 1;
				if (m_gpu_vu_derived_config_valid)
					consumer.flags |= 1u << 2;
				VitaGS::UpdateActiveGpuVuGxmSubmissionWatchdog(
					VitaGS::GpuVuGxmSubmissionStage::GsStateGroupReturned,
					consumer);
				if (!m_gpu_vu_draws.empty())
				{
					for (const auto& pending : m_gpu_vu_draws)
						VitaGpuVu::RecordGpuVuDrawRejected();
					m_gpu_vu_draws.clear();
					pxFailRel("GPU-VU proxy geometry produced no PCSX2 HW draw");
				}
				if (!m_gpu_vu_derived_config_valid ||
					!m_gpu_vu_derived_submit_succeeded)
				{
					// SubmitDrawConfig() already accounted for the first group's
					// rejection. No later group may reuse an unaccepted config.
					for (u32 index = group_end; index < state_end; index++)
					{
						if (draws[index])
							VitaGpuVu::RecordGpuVuDrawRejected();
						draws[index].reset();
					}
					group_first = state_end;
					continue;
				}
			}
			else
			{
				std::vector<std::unique_ptr<VitaGpuVu::GpuVuDraw>> group;
				group.reserve(group_objects);
				for (u32 index = group_first; index < group_end; index++)
					group.push_back(std::move(draws[index]));
				GSHWDrawConfig config = m_gpu_vu_derived_config;
				const bool rendered = device &&
					device->RenderGpuVuDraws(config, std::move(group));
				if (rendered)
					consumer.flags |= 1u << 1;
				if (m_gpu_vu_derived_config_valid)
					consumer.flags |= 1u << 2;
				VitaGS::UpdateActiveGpuVuGxmSubmissionWatchdog(
					VitaGS::GpuVuGxmSubmissionStage::GsStateGroupReturned,
					consumer);
				if (!rendered)
				{
					for (u32 index = 0; index < group_objects; index++)
						VitaGpuVu::RecordGpuVuDrawRejected();
				}
				else
				{
					reused_group_count++;
				}
			}
			group_first = group_end;
			VitaGS::UpdateActiveGpuVuGxmSubmissionWatchdog(
				VitaGS::GpuVuGxmSubmissionStage::GsStateGroupAdvanced,
				consumer);
			consumer_group_index++;
		}

		if (reused_group_count != 0u)
		{
			static u64 s_gpu_vu_state_template_batches = 0;
			const u64 batch = ++s_gpu_vu_state_template_batches;
			if (batch <= 8u || (batch & (batch - 1u)) == 0u)
			{
				Console.WriteLn(
					"GPU-VU gs_state_template_batch=%llu objects=%u "
					"generated_groups=%u gs_derivations=1 reused_groups=%u "
					"synthetic_path1_packets=1 order=preserved.",
					static_cast<unsigned long long>(batch), object_count,
					group_count, reused_group_count);
			}
		}
		first = state_end;
		VitaGS::UpdateActiveGpuVuGxmSubmissionWatchdog(
			VitaGS::GpuVuGxmSubmissionStage::GsStateContractAdvanced);
	}
	VitaGS::UpdateActiveGpuVuGxmSubmissionWatchdog(
		VitaGS::GpuVuGxmSubmissionStage::GsStateConsumeReturned);
	return;
	#endif

	for (const auto& draw : draws)
		VitaGpuVu::RecordGpuVuDrawRejected();
#if !defined(VITASX2_QEMU_VALIDATION) || !VITASX2_QEMU_VALIDATION
	pxFailRel("GPU-VU draw reached GS before native geometry consumption");
#endif
}
