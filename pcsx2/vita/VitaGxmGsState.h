// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/Renderers/SW/GSRendererSW.h"

#include <memory>

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
	void InvalidateVideoMem(const GIFRegBITBLTBUF& blit, const GSVector4i& rect) override;
	void InvalidateLocalMem(const GIFRegBITBLTBUF& blit, const GSVector4i& rect, bool clut = false) override;

private:
	bool HasPcsx2MergeOutput();

	struct Impl;
	std::unique_ptr<Impl> m_impl;
	u64 m_last_present_draw = 0;
	u64 m_last_present_transfer = 0;
};
