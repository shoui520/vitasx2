// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "common/Assertions.h"
#include "common/Console.h"

#include "Vif_Dynarec.h"

void VifUnpackSSE_Init()
{
}

void dVifReset(int idx)
{
}

void dVifRelease(int idx)
{
}

template <int idx>
void dVifUnpack(const u8* data, bool isFill)
{
	// PCSX2 owner: Vif_Unpack.cpp::_nVifUnpack(). The Vita ARM32 target keeps
	// the dynarec entry point valid while the A32/NEON unpack generator is
	// brought up, so any accidental dVifUnpack() route still executes the
	// interpreter-owned unpack semantics instead of aborting.
	_nVifUnpack(idx, data, vifXRegs.mode, isFill);
}

template void dVifUnpack<0>(const u8* data, bool isFill);
template void dVifUnpack<1>(const u8* data, bool isFill);
