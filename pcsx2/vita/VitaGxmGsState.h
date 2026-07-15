// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/GSState.h"

#include <memory>

class VitaGxmGsState final : public GSState
{
public:
	explicit VitaGxmGsState(bool enable_native_presenter);
	~VitaGxmGsState() override;

	bool IsNativePresenterReady() const;
	void Present();
	void VSync();

	void Reset(bool hardware_reset) override;
	void Draw() override;
	void InvalidateVideoMem(const GIFRegBITBLTBUF& blit, const GSVector4i& rect) override;
	void InvalidateLocalMem(const GIFRegBITBLTBUF& blit, const GSVector4i& rect, bool clut = false) override;

private:
	bool IsCoverageAlphaSupported() override;
	bool HasPcsx2MergeOutput();

	struct Impl;
	std::unique_ptr<Impl> m_impl;
};
