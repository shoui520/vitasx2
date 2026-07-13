// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

namespace Vu0FlagConversion
{
	// PCSX2 owner: x86/microVU_Alloc.inl::mVUallocSFLAGd(). Convert the
	// architectural normalized Status layout into microVU's lane-local layout.
	constexpr u32 DenormalizeStatus(u32 status)
	{
		return ((status >> 3) & 0x18u) |
		       ((status << 11) & 0x1800u) |
		       ((status << 14) & 0x03cf0000u);
	}

	// VU0 program start and CTC2 both require every pipeline instance to see
	// the same value before micro execution can consume it.
	inline void Broadcast(u32* flags, u32 value)
	{
		flags[0] = value;
		flags[1] = value;
		flags[2] = value;
		flags[3] = value;
	}

	static_assert(DenormalizeStatus(0x001u) == 0x00000800u);
	static_assert(DenormalizeStatus(0xfffu) == 0x03cf1818u);
}
