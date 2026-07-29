// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "GS/GSLocalMemory.h"

namespace VitaGS
{
	// GSLocalMemory owns one canonical 4 MiB store on Vita instead of PCSX2's
	// four virtual aliases. Reserve it before GXM's optional arenas so renderer
	// construction cannot lose a mandatory allocation to later fragmentation.
	inline constexpr size_t CanonicalLocalMemoryBytes = 4 * 1024 * 1024;
	bool ReserveCanonicalLocalMemory();
	void ReleaseUnclaimedCanonicalLocalMemory();

	// Commit an unmasked PSMCT32 source-replacement rectangle to canonical GS
	// memory. The offset may be reused across every rectangle in one GS draw.
	void FillPsmct32Rect(GSLocalMemory& memory, const GSOffset& offset,
		const GSVector4i& rect, u32 color);
}
