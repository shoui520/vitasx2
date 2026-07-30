// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

// PCSX2 owner: Hw.h D0_CHCR..D9_CHCR. These are the ten architectural
// EE DMAC channel-control registers; bit 8 is tDMA_CHCR::STR.
constexpr bool VitaIsEeDmacChcrAddress(u32 address)
{
	switch (address)
	{
		case 0x10008000u:
		case 0x10009000u:
		case 0x1000a000u:
		case 0x1000b000u:
		case 0x1000b400u:
		case 0x1000c000u:
		case 0x1000c400u:
		case 0x1000c800u:
		case 0x1000d000u:
		case 0x1000d400u:
			return true;
		default:
			return false;
	}
}
