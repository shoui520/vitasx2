// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

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
};

#endif
