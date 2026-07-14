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

static bool IsValidPortableSifFifo(const sifFifo& fifo)
{
	if (fifo.readPos < 0 || fifo.readPos >= FIFO_SIF_W ||
		fifo.writePos < 0 || fifo.writePos >= FIFO_SIF_W ||
		fifo.size < 0 || fifo.size > FIFO_SIF_W)
	{
		return false;
	}

	return fifo.writePos == ((fifo.readPos + fifo.size) & (FIFO_SIF_W - 1));
}

static bool IsCanonicalPortableSifBool(const bool& value)
{
	static_assert(sizeof(bool) == sizeof(u8));
	u8 representation = 0;
	std::memcpy(&representation, &value, sizeof(representation));
	return representation <= 1u;
}

static bool IsValidPortableSif(const _sif& sif)
{
	return IsValidPortableSifFifo(sif.fifo) &&
		sif.iop.writeJunk >= 0 && sif.iop.writeJunk <= 3 &&
		IsCanonicalPortableSifBool(sif.ee.end) &&
		IsCanonicalPortableSifBool(sif.ee.busy) &&
		IsCanonicalPortableSifBool(sif.iop.end) &&
		IsCanonicalPortableSifBool(sif.iop.busy);
}

bool SaveStateBase::sifFreeze()
{
	if (IsPortableReplay() && IsSaving() &&
		(!IsValidPortableSif(sif0) || !IsValidPortableSif(sif1)))
	{
		Console.Error("Portable SIF replay capture found an invalid FIFO ring state.");
		m_error = true;
		return false;
	}
	if (!FreezeTag("SIFdma"))
		return false;

	Freeze(sif0);
	if (IsPortableReplay() && !IsValidPortableSif(sif0))
	{
		Console.Error("Portable SIF replay state contains an invalid SIF0 FIFO ring.");
		m_error = true;
		return false;
	}
	Freeze(sif1);
	if (IsPortableReplay() && !IsValidPortableSif(sif1))
	{
		Console.Error("Portable SIF replay state contains an invalid SIF1 FIFO ring.");
		m_error = true;
		return false;
	}
	return IsOkay();
}
