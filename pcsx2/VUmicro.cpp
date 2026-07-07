// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "VUmicro.h"
#include "VUmicroFast.h"
#include "MTVU.h"
#include "GS.h"
#include "Gif_Unit.h"

BaseVUmicroCPU* CpuVU0 = nullptr;
BaseVUmicroCPU* CpuVU1 = nullptr;

#if defined(VITASX2_QEMU_VALIDATION)
u32 g_qemuVuUpperNopFastSteps = 0;
u32 g_qemuVuLowerNopFastSteps = 0;
u32 g_qemuVuNopPairBurstSteps = 0;
u32 g_qemuVuNopPairFastForwardSteps = 0;
u32 g_qemuVuLowerDirectFastSteps = 0;
u32 g_qemuVuUpperDirectFastSteps = 0;
u32 g_qemuVuIbitFastSteps = 0;
u32 g_qemuVuIbitBurstSteps = 0;
u32 g_qemuVuLowerDirectBurstSteps = 0;
u32 g_qemuVuUpperDirectBurstSteps = 0;
u32 g_qemuVuPairedDirectBurstSteps = 0;
u32 g_qemuVuLowerNeonQwordOps = 0;
u32 g_qemuVuUpperNeonQwordOps = 0;
u32 g_qemuVuUpperScalarFullMaskOps = 0;
u32 g_qemuVuUpperScalarPartialMaskOps = 0;
u32 g_qemuVuLowerVfpSqrtOps = 0;
u32 g_qemuVuLowerVfpDivOps = 0;
u32 g_qemuVuLowerVfpDoubleDivOps = 0;
u32 g_qemuVuUpperVfpMaddScalarOps = 0;
u32 g_qemuVuDecodedUpperCacheHits = 0;
u32 g_qemuVuDecodedUpperCacheMisses = 0;
u32 g_qemuVuDecodedLowerCacheHits = 0;
u32 g_qemuVuDecodedLowerCacheMisses = 0;
bool g_qemuVuLowerDirectFastEnabled = true;
bool g_qemuVuUpperDirectFastEnabled = true;
bool g_qemuVuLowerDirectBurstEnabled = true;
bool g_qemuVuUpperDirectBurstEnabled = true;
#endif

namespace
{
	enum VuMicroDecodedFlags : u8
	{
		VU_DECODED_UPPER_VALID = 1u << 0,
		VU_DECODED_UPPER_FAST = 1u << 1,
		VU_DECODED_LOWER_VALID = 1u << 2,
		VU_DECODED_LOWER_FAST = 1u << 3,
	};

	struct VuMicroDecodedEntry
	{
		u32 upper = 0;
		u32 lower = 0;
		_VURegsNum upper_regs = {};
		_VURegsNum lower_regs = {};
		u8 flags = 0;
	};

	VuMicroDecodedEntry s_vu0_decoded[VU0_PROGSIZE / 8];
	VuMicroDecodedEntry s_vu1_decoded[VU1_PROGSIZE / 8];

	__fi VuMicroDecodedEntry* VuMicroDecodedEntries(int idx)
	{
		return idx ? s_vu1_decoded : s_vu0_decoded;
	}

	__fi u32 VuMicroProgramMask(int idx)
	{
		return idx ? VU1_PROGMASK : VU0_PROGMASK;
	}

	__fi u32 VuMicroProgramSize(int idx)
	{
		return idx ? VU1_PROGSIZE : VU0_PROGSIZE;
	}

	__fi VuMicroDecodedEntry& VuMicroDecodedEntryForPc(int idx, u32 pc)
	{
		return VuMicroDecodedEntries(idx)[(pc & VuMicroProgramMask(idx)) >> 3];
	}
} // namespace

bool VuMicroAnalyzeUpperNoLowerCached(int idx, u32 pc, u32 code, _VURegsNum* regs)
{
	VuMicroDecodedEntry& entry = VuMicroDecodedEntryForPc(idx, pc);
	if ((entry.flags & VU_DECODED_UPPER_VALID) != 0 && entry.upper == code)
	{
#if defined(VITASX2_QEMU_VALIDATION)
		++g_qemuVuDecodedUpperCacheHits;
#endif
		if ((entry.flags & VU_DECODED_UPPER_FAST) == 0)
			return false;

		*regs = entry.upper_regs;
		return true;
	}

#if defined(VITASX2_QEMU_VALIDATION)
	++g_qemuVuDecodedUpperCacheMisses;
#endif
	entry.upper = code;
	entry.flags = static_cast<u8>(entry.flags & ~VU_DECODED_UPPER_FAST);
	const bool fast = VUInterpFast::AnalyzeUpperNoLower(code, &entry.upper_regs);
	if (fast)
	{
		entry.flags |= VU_DECODED_UPPER_FAST;
		*regs = entry.upper_regs;
	}
	entry.flags |= VU_DECODED_UPPER_VALID;
	return fast;
}

bool VuMicroAnalyzeLowerNoUpperCached(int idx, u32 pc, u32 code, _VURegsNum* regs)
{
	VuMicroDecodedEntry& entry = VuMicroDecodedEntryForPc(idx, pc);
	if ((entry.flags & VU_DECODED_LOWER_VALID) != 0 && entry.lower == code)
	{
#if defined(VITASX2_QEMU_VALIDATION)
		++g_qemuVuDecodedLowerCacheHits;
#endif
		if ((entry.flags & VU_DECODED_LOWER_FAST) == 0)
			return false;

		*regs = entry.lower_regs;
		return true;
	}

#if defined(VITASX2_QEMU_VALIDATION)
	++g_qemuVuDecodedLowerCacheMisses;
#endif
	entry.lower = code;
	entry.flags = static_cast<u8>(entry.flags & ~VU_DECODED_LOWER_FAST);
	const bool fast = VUInterpFast::AnalyzeLowerNoUpper(code, &entry.lower_regs);
	if (fast)
	{
		entry.flags |= VU_DECODED_LOWER_FAST;
		*regs = entry.lower_regs;
	}
	entry.flags |= VU_DECODED_LOWER_VALID;
	return fast;
}

void VuMicroInvalidateDecodedCache(int idx, u32 addr, u32 size)
{
	if (size == 0)
		return;

	const u32 program_size = VuMicroProgramSize(idx);
	VuMicroDecodedEntry* const entries = VuMicroDecodedEntries(idx);
	if (size >= program_size)
	{
		for (u32 i = 0; i < program_size / 8; i++)
			entries[i].flags = 0;
		return;
	}

	const u32 mask = VuMicroProgramMask(idx);
	const u32 start = addr & mask;
	const u32 first = start & ~7u;
	const u32 bytes = size + (start & 7u) + 7u;
	for (u32 offset = 0; offset < bytes; offset += 8)
		entries[((first + offset) & mask) >> 3].flags = 0;
}

__inline u32 CalculateMinRunCycles(u32 cycles, bool requiresAccurateCycles)
{
	// If we're running an interlocked COP2 operation
	// run for an exact amount of cycles
	if(requiresAccurateCycles)
		return cycles;

	// Allow a minimum of 16 cycles to avoid running small blocks
	// Running a block of like 3 cycles is highly inefficient
	// so while sync isn't tight, it's okay to run ahead a little bit.
	return std::max(16U, cycles);
}

// Executes a Block based on EE delta time
void BaseVUmicroCPU::ExecuteBlock(bool startUp)
{
	const u32& stat = VU0.VI[REG_VPU_STAT].UL;
	const int test = m_Idx ? 0x100 : 1;

	if (m_Idx && THREAD_VU1)
	{
		vu1Thread.Get_MTVUChanges();
		return;
	}

	if (!(stat & test))
	{
		// VU currently flushes XGKICK on VU1 end so no need for this, yet
		/*if (m_Idx == 1 && VU1.xgkickenable)
		{
			_vuXGKICKTransfer((cpuRegs.cycle - VU1.xgkicklastcycle), false);
		}*/
		return;
	}

	if (startUp)
	{
		Execute(CalculateMinRunCycles(0, false));
	}
	else // Continue Executing
	{
		u64 cycle = m_Idx ? VU1.cycle : VU0.cycle;
		s32 delta = (s32)(u32)(cpuRegs.cycle - cycle);

		if (delta > 0)
			Execute(CalculateMinRunCycles(delta, false));
	}
}

// This function is called by VU0 Macro (COP2) after transferring some
// EE data to VU0's registers. We want to run VU0 Micro right after this
// to ensure that the register is used at the correct time.
// This fixes spinning/hanging in some games like Ratchet and Clank's Intro.
void BaseVUmicroCPU::ExecuteBlockJIT(BaseVUmicroCPU* cpu, bool interlocked)
{
	const u32& stat = VU0.VI[REG_VPU_STAT].UL;
	constexpr int test = 1;

	if (stat & test)
	{ // VU is running
		s64 delta = (s64)(u64)(cpuRegs.cycle - VU0.cycle);

		if (delta > 0)
		{
			cpu->Execute(CalculateMinRunCycles(delta, interlocked)); // Execute the time since the last call
		}
	}
}
