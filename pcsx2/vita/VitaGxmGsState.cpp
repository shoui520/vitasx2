// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "vita/VitaGxmGsState.h"
#include "vita/VitaGpuVuDraw.h"

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

#endif

void VitaGxmGsState::ConsumeGpuVuDraw(
	std::unique_ptr<VitaGpuVu::GpuVuDraw> draw)
{
	if (!draw)
		return;

	// Admission remains disconnected until GSRendererHW can derive its target,
	// texture, blend, depth, and fragment state without a CPU GSVertex array.
	// Reaching this command early would lose PATH1 output, so fail loudly
	// instead of silently treating a registered vertex root as executable.
	VitaGpuVu::RecordGpuVuDrawRejected();
	pxFailRel("GPU-VU draw reached GS before native geometry consumption");
}
