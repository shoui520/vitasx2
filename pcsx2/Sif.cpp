// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#define _PC_	// disables MIPS opcode macros.

#include "R3000A.h"
#include "Common.h"
#include "Sif.h"

#if defined(VITASX2_QEMU_VALIDATION)
u32 g_qemuSifFifoContiguousWrites = 0;
u32 g_qemuSifFifoContiguousReads = 0;
u32 g_qemuSifFifoWrappedWrites = 0;
u32 g_qemuSifFifoWrappedReads = 0;
u32 g_qemuSifFifoJunkWrites = 0;
u32 g_qemuSifFifoJunkScalarWords = 0;
u32 g_qemuSifFifoNeonQwords = 0;
u32 g_qemuSifFifoNeon64ByteGroups = 0;
u32 g_qemuSifFifoNeon128ByteGroups = 0;
u32 g_qemuSifFifoNeon256ByteGroups = 0;
u32 g_qemuSifFifoNeon512ByteGroups = 0;
u32 g_qemuSifFifoExactSpanCopies = 0;
u32 g_qemuSifFifoExact384ByteCopies = 0;
u32 g_qemuSifFifoExact512ByteCopies = 0;
#endif

void sifReset()
{
	std::memset(&sif0, 0, sizeof(sif0));
	std::memset(&sif1, 0, sizeof(sif1));
}

bool SaveStateBase::sifFreeze()
{
	if (!FreezeTag("SIFdma"))
		return false;

	Freeze(sif0);
	Freeze(sif1);
	return IsOkay();
}
