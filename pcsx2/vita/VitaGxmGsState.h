// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <memory>

namespace VitaGpuVu
{
	class GpuVuDraw;
}

#if defined(VITASX2_QEMU_VALIDATION) && VITASX2_QEMU_VALIDATION

#include "GS/Renderers/SW/GSRendererSW.h"

// Linux/QEMU has no GXM device. Keep PCSX2's software renderer here so the
// portable GS-state and local-memory oracle remains available while the Vita
// product uses the hardware renderer below.
class VitaGxmGsState final : public GSRendererSW
{
public:
	explicit VitaGxmGsState(bool enable_native_presenter);
	~VitaGxmGsState() override;

	bool IsNativePresenterReady() const;
	bool IsIdleFrame() const;
	void Present();
	void VSync(u32 field);
	void ConsumeGpuVuDraw(std::unique_ptr<VitaGpuVu::GpuVuDraw> draw);

	void Reset(bool hardware_reset) override;
	void Draw() override;
	void InvalidateVideoMem(const GIFRegBITBLTBUF& blit,
		const GSVector4i& rect) override;
	void InvalidateLocalMem(const GIFRegBITBLTBUF& blit,
		const GSVector4i& rect, bool clut = false) override;

private:
	bool HasPcsx2MergeOutput();

	u64 m_last_present_draw = 0;
	u64 m_last_present_transfer = 0;
};

#elif defined(VITASX2_VITA_SOFTWARE_GS_CONTROL) && \
	VITASX2_VITA_SOFTWARE_GS_CONTROL

#include "GS/Renderers/SW/GSRendererSW.h"

// PCSX2 owner: GS.cpp::OpenGSRenderer(). This bounded physical-Vita control
// keeps the current core, MTGS, GSDeviceGXM, and presentation path, changing
// only the renderer which consumes decoded GS state.
class VitaGxmGsState final : public GSRendererSW
{
public:
	explicit VitaGxmGsState(bool enable_native_presenter);
	~VitaGxmGsState() override;

	bool IsNativePresenterReady() const;
	void ConsumeGpuVuDraw(std::unique_ptr<VitaGpuVu::GpuVuDraw> draw);
	void Reset(bool hardware_reset) override;
	void VSync(u32 field, bool registers_written, bool idle_frame) override;
};

#else

#include "GS/Renderers/HW/GSRendererHW.h"

// PCSX2's GSRendererHW is the Vita renderer. GSDeviceGXM translates its device
// contract to Sony GXM; this wrapper only keeps the mailbox's platform-neutral
// renderer type and does not own a second graphics context.
class VitaGxmGsState final : public GSRendererHW
{
public:
	explicit VitaGxmGsState(bool enable_native_presenter);
	~VitaGxmGsState() override;

	bool IsNativePresenterReady() const;
	void ConsumeGpuVuDraw(std::unique_ptr<VitaGpuVu::GpuVuDraw> draw);
};

#endif
