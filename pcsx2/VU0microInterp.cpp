// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"

#include "VUmicro.h"
#include "VUmicroFast.h"
#include "DebugTools/VuTrace.h"

#include <cfenv>

extern void _vuFlushAll(VURegs* VU);

#if defined(VITASX2_QEMU_VALIDATION)
extern u32 g_qemuVuUpperNopFastSteps;
extern u32 g_qemuVuLowerNopFastSteps;
extern u32 g_qemuVuNopPairBurstSteps;
extern u32 g_qemuVuLowerDirectFastSteps;
extern u32 g_qemuVuUpperDirectFastSteps;
extern u32 g_qemuVuIbitFastSteps;
extern u32 g_qemuVuIbitBurstSteps;
extern u32 g_qemuVuLowerDirectBurstSteps;
extern u32 g_qemuVuUpperDirectBurstSteps;
extern u32 g_qemuVuPairedDirectBurstSteps;
extern bool g_qemuVuLowerDirectFastEnabled;
extern bool g_qemuVuUpperDirectFastEnabled;
extern bool g_qemuVuLowerDirectBurstEnabled;
extern bool g_qemuVuUpperDirectBurstEnabled;
#endif

static void _vu0ExecUpper(VURegs* VU, u32* ptr)
{
	VU->code = ptr[1];
	IdebugUPPER(VU0);
	VU0_UPPER_OPCODE[VU->code & 0x3f]();
}

static void _vu0ExecLower(VURegs* VU, u32* ptr)
{
	VU->code = ptr[0];
	IdebugLOWER(VU0);
	VU0_LOWER_OPCODE[VU->code >> 25]();
}

static void _vu0ExecUpperMaybeFast(VURegs* VU, u32* ptr, bool upper_fast)
{
	if (upper_fast)
	{
		VU->code = ptr[1];
		IdebugUPPER(VU0);
		VUInterpFast::ExecuteUpperNoLower(VU, ptr[1]);
#if defined(VITASX2_QEMU_VALIDATION)
		++g_qemuVuUpperDirectFastSteps;
#endif
		return;
	}

	_vu0ExecUpper(VU, ptr);
}

int vu0branch = 0;

static __fi bool _vu0IsUpperNop(u32 upper)
{
	return (upper & 0x07ffffffu) == 0x000002ffu;
}

static __fi bool _vu0IsLowerNop(u32 lower)
{
	return lower == 0x8000033cu;
}

static __fi bool _vu0IsPlainNopPair(u32 upper, u32 lower)
{
	return upper == 0x000002ffu && _vu0IsLowerNop(lower);
}

static u32 _vu0ExecNopPairBurst(VURegs* VU, u32 max_steps)
{
	if (max_steps == 0 || Pcsx2Trace::IsVuTraceEnabled() ||
		VU->branch != 0 || VU->ebit != 0 || VU->takedelaybranch)
	{
		return 0;
	}

	u32 steps = 0;
	while (steps < max_steps &&
		   (VU0.VI[REG_VPU_STAT].UL & 0x1) &&
		   !(VU->flags & VUFLAG_MFLAGSET))
	{
		VU->VI[REG_TPC].UL &= VU0_PROGMASK;
		const u32 pc = VU->VI[REG_TPC].UL;
		const u32* ptr = reinterpret_cast<const u32*>(&VU->Micro[pc]);
		if (!_vu0IsPlainNopPair(ptr[1], ptr[0]))
			break;

		// PCSX2 owners: VUops.cpp::_vuNOP(), _vuMOVE(Ft==0), and
		// VU0microInterp.cpp::vu0Exec(). Plain NOP pairs have no register
		// dependencies or writes, but still advance time and flush pending pipes.
		VU->cycle++;
		VU->VI[REG_TPC].UL = pc + 8;
		const u64 cyclesBeforeOp = VU->cycle - 1;
		_vuTestPipes(VU);
		if (VU->VIBackupCycles > 0)
			VU->VIBackupCycles -= std::min((u8)(VU->cycle - cyclesBeforeOp), VU->VIBackupCycles);
		VU->code = ptr[0];
		vu0branch = false;
		steps++;
	}

#if defined(VITASX2_QEMU_VALIDATION)
	g_qemuVuNopPairBurstSteps += steps;
#endif
	return steps;
}

static __fi bool _vu0CanBurstLowerDirect(const _VURegsNum& lregs)
{
	return lregs.pipe == VUPIPE_FMAC ||
		lregs.pipe == VUPIPE_IALU ||
		lregs.pipe == VUPIPE_FDIV ||
		lregs.pipe == VUPIPE_EFU;
}

static __fi bool _vu0CanBurstUpperNopLowerDirect(const _VURegsNum& lregs)
{
	return _vu0CanBurstLowerDirect(lregs);
}

static __fi bool _vu0CanBurstUpperDirect(const _VURegsNum& uregs)
{
	return uregs.pipe == VUPIPE_FMAC;
}

static u32 _vu0ExecUpperNopLowerDirectBurst(VURegs* VU, u32 max_cycles)
{
	if (max_cycles == 0 || Pcsx2Trace::IsVuTraceEnabled() ||
		VU->branch != 0 || VU->ebit != 0 || VU->takedelaybranch ||
		(VU->flags & VUFLAG_MFLAGSET))
	{
		return 0;
	}

#if defined(VITASX2_QEMU_VALIDATION)
	if (!g_qemuVuLowerDirectFastEnabled || !g_qemuVuLowerDirectBurstEnabled)
		return 0;
#endif

	u32 steps = 0;
	const u64 start_cycle = VU->cycle;
	while ((VU->cycle - start_cycle) < max_cycles &&
		   (VU0.VI[REG_VPU_STAT].UL & 0x1) &&
		   !(VU->flags & VUFLAG_MFLAGSET))
	{
		VU->VI[REG_TPC].UL &= VU0_PROGMASK;
		const u32 pc = VU->VI[REG_TPC].UL;
		const u32* ptr = reinterpret_cast<const u32*>(&VU->Micro[pc]);
		const u32 lower = ptr[0];
		const u32 upper = ptr[1];
		if (upper != 0x000002ffu || _vu0IsLowerNop(lower))
			break;

		_VURegsNum lregs = {};
		if (!VUInterpFast::AnalyzeLowerNoUpper(lower, &lregs) || !_vu0CanBurstUpperNopLowerDirect(lregs))
			break;

		// PCSX2 owners: VU0microInterp.cpp::vu0Exec() upper-NOP path,
		// VUops.cpp lower-slot implementations, and VUops.cpp pipe/stall
		// helpers for FMAC/IALU/FDIV/EFU lower pipes. The burst keeps the same
		// per-op dependency/pipe sequence.
		VU->cycle++;
		VU->VI[REG_TPC].UL = pc + 8;
		VU->code = lower;

		const u64 cyclesBeforeOp = VU->cycle - 1;
		_vuTestLowerStalls(VU, &lregs);
		_vuTestPipes(VU);

		if (VU->VIBackupCycles > 0)
			VU->VIBackupCycles -= std::min((u8)(VU->cycle - cyclesBeforeOp), VU->VIBackupCycles);
		vu0branch = false;

		IdebugLOWER(VU0);
		VUInterpFast::ExecuteLowerNoUpper(VU, lower);

		if (lregs.pipe == VUPIPE_FMAC)
			_vuClearFMAC(VU);

		_vuAddLowerStalls(VU, &lregs);

		if (lregs.pipe == VUPIPE_FMAC)
			VU->fmacwritepos = (VU->fmacwritepos + 1) & 3;

		steps++;
	}

#if defined(VITASX2_QEMU_VALIDATION)
	g_qemuVuLowerDirectFastSteps += steps;
	g_qemuVuUpperNopFastSteps += steps;
	g_qemuVuLowerDirectBurstSteps += steps;
#endif
	return steps;
}

static u32 _vu0ExecUpperDirectLowerNopBurst(VURegs* VU, u32 max_cycles)
{
	if (max_cycles == 0 || Pcsx2Trace::IsVuTraceEnabled() ||
		VU->branch != 0 || VU->ebit != 0 || VU->takedelaybranch ||
		(VU->flags & VUFLAG_MFLAGSET))
	{
		return 0;
	}

#if defined(VITASX2_QEMU_VALIDATION)
	if (!g_qemuVuUpperDirectFastEnabled || !g_qemuVuUpperDirectBurstEnabled)
		return 0;
#endif

	u32 steps = 0;
	const u64 start_cycle = VU->cycle;
	while ((VU->cycle - start_cycle) < max_cycles &&
		   (VU0.VI[REG_VPU_STAT].UL & 0x1) &&
		   !(VU->flags & VUFLAG_MFLAGSET))
	{
		VU->VI[REG_TPC].UL &= VU0_PROGMASK;
		const u32 pc = VU->VI[REG_TPC].UL;
		const u32* ptr = reinterpret_cast<const u32*>(&VU->Micro[pc]);
		const u32 lower = ptr[0];
		const u32 upper = ptr[1];
		if ((upper & 0xf8000000u) != 0 || !_vu0IsLowerNop(lower))
			break;

		_VURegsNum uregs = {};
		if (!VUInterpFast::AnalyzeUpperNoLower(upper, &uregs) || !_vu0CanBurstUpperDirect(uregs))
			break;

		// PCSX2 owners: VU0microInterp.cpp::vu0Exec() lower-NOP path,
		// VUops.cpp upper-slot implementations, and VUops.cpp pipe/stall
		// helpers. The burst keeps the same per-op dependency/pipe sequence.
		VU->cycle++;
		VU->VI[REG_TPC].UL = pc + 8;
		VU->code = upper;

		const u64 cyclesBeforeOp = VU->cycle - 1;
		_vuTestUpperStalls(VU, &uregs);
		_vuTestPipes(VU);

		if (VU->VIBackupCycles > 0)
			VU->VIBackupCycles -= std::min((u8)(VU->cycle - cyclesBeforeOp), VU->VIBackupCycles);
		vu0branch = false;

		IdebugUPPER(VU0);
		VUInterpFast::ExecuteUpperNoLower(VU, upper);
		VU->code = lower;

		if (uregs.pipe == VUPIPE_FMAC)
			_vuClearFMAC(VU);

		_vuAddUpperStalls(VU, &uregs);

		if (uregs.pipe == VUPIPE_FMAC)
			VU->fmacwritepos = (VU->fmacwritepos + 1) & 3;

		steps++;
	}

#if defined(VITASX2_QEMU_VALIDATION)
	g_qemuVuUpperDirectFastSteps += steps;
	g_qemuVuLowerNopFastSteps += steps;
	g_qemuVuUpperDirectBurstSteps += steps;
#endif
	return steps;
}

static u32 _vu0ExecUpperLowerDirectBurst(VURegs* VU, u32 max_cycles)
{
	if (max_cycles == 0 || Pcsx2Trace::IsVuTraceEnabled() ||
		VU->branch != 0 || VU->ebit != 0 || VU->takedelaybranch ||
		(VU->flags & VUFLAG_MFLAGSET))
	{
		return 0;
	}

#if defined(VITASX2_QEMU_VALIDATION)
	if (!g_qemuVuLowerDirectFastEnabled || !g_qemuVuUpperDirectFastEnabled ||
		!g_qemuVuLowerDirectBurstEnabled || !g_qemuVuUpperDirectBurstEnabled)
	{
		return 0;
	}
#endif

	u32 steps = 0;
	const u64 start_cycle = VU->cycle;
	while ((VU->cycle - start_cycle) < max_cycles &&
		   (VU0.VI[REG_VPU_STAT].UL & 0x1) &&
		   !(VU->flags & VUFLAG_MFLAGSET))
	{
		VU->VI[REG_TPC].UL &= VU0_PROGMASK;
		const u32 pc = VU->VI[REG_TPC].UL;
		const u32* ptr = reinterpret_cast<const u32*>(&VU->Micro[pc]);
		const u32 lower = ptr[0];
		const u32 upper = ptr[1];
		if ((upper & 0xf8000000u) != 0 || _vu0IsUpperNop(upper) || _vu0IsLowerNop(lower))
			break;

		_VURegsNum uregs = {};
		_VURegsNum lregs = {};
		if (!VUInterpFast::AnalyzeUpperNoLower(upper, &uregs) || !_vu0CanBurstUpperDirect(uregs) ||
			!VUInterpFast::AnalyzeLowerNoUpper(lower, &lregs) || !_vu0CanBurstLowerDirect(lregs))
		{
			break;
		}

		VECTOR _VF;
		VECTOR _VFc;
		REG_VI _VI;
		REG_VI _VIc;
		int vfreg = 0;
		int vireg = 0;
		int discard = 0;

		// PCSX2 owners: VU0microInterp.cpp::_vu0Exec() paired upper/lower
		// path and VUops.cpp pipe helpers for FMAC/IALU/FDIV/EFU lower pipes.
		// Keep the same upper-first issue, lower stale-read save/restore,
		// same-register discard, and stall order.
		VU->cycle++;
		VU->VI[REG_TPC].UL = pc + 8;
		VU->code = upper;

		const u64 cyclesBeforeOp = VU->cycle - 1;
		_vuTestUpperStalls(VU, &uregs);

		VU->code = lower;
		_vuTestLowerStalls(VU, &lregs);
		_vuTestPipes(VU);

		if (VU->VIBackupCycles > 0)
			VU->VIBackupCycles -= std::min((u8)(VU->cycle - cyclesBeforeOp), VU->VIBackupCycles);
		vu0branch = false;

		if (uregs.VFwrite)
		{
			if (lregs.VFwrite == uregs.VFwrite)
				discard = 1;
			if (lregs.VFread0 == uregs.VFwrite || lregs.VFread1 == uregs.VFwrite)
			{
				_VF = VU->VF[uregs.VFwrite];
				vfreg = uregs.VFwrite;
			}
		}
		if (uregs.VIread & (1 << REG_CLIP_FLAG))
		{
			if (lregs.VIwrite & (1 << REG_CLIP_FLAG))
				discard = 1;
			if (lregs.VIread & (1 << REG_CLIP_FLAG))
			{
				_VI = VU0.VI[REG_CLIP_FLAG];
				vireg = REG_CLIP_FLAG;
			}
		}

		VU->code = upper;
		IdebugUPPER(VU0);
		VUInterpFast::ExecuteUpperNoLower(VU, upper);

		if (discard == 0)
		{
			if (vfreg)
			{
				_VFc = VU->VF[vfreg];
				VU->VF[vfreg] = _VF;
			}
			if (vireg)
			{
				_VIc = VU->VI[vireg];
				VU->VI[vireg] = _VI;
			}

			IdebugLOWER(VU0);
			VUInterpFast::ExecuteLowerNoUpper(VU, lower);

			if (vfreg)
				VU->VF[vfreg] = _VFc;
			if (vireg)
				VU->VI[vireg] = _VIc;
		}

		if (uregs.pipe == VUPIPE_FMAC || lregs.pipe == VUPIPE_FMAC)
			_vuClearFMAC(VU);

		_vuAddUpperStalls(VU, &uregs);
		_vuAddLowerStalls(VU, &lregs);

		if (uregs.pipe == VUPIPE_FMAC || lregs.pipe == VUPIPE_FMAC)
			VU->fmacwritepos = (VU->fmacwritepos + 1) & 3;

		steps++;
	}

#if defined(VITASX2_QEMU_VALIDATION)
	g_qemuVuUpperDirectFastSteps += steps;
	g_qemuVuLowerDirectFastSteps += steps;
	g_qemuVuPairedDirectBurstSteps += steps;
#endif
	return steps;
}

static u32 _vu0ExecIbitDirectBurst(VURegs* VU, u32 max_cycles)
{
	if (max_cycles == 0 || Pcsx2Trace::IsVuTraceEnabled() ||
		VU->branch != 0 || VU->ebit != 0 || VU->takedelaybranch ||
		(VU->flags & VUFLAG_MFLAGSET))
	{
		return 0;
	}

#if defined(VITASX2_QEMU_VALIDATION)
	if (!g_qemuVuUpperDirectFastEnabled || !g_qemuVuUpperDirectBurstEnabled)
		return 0;
#endif

	u32 steps = 0;
	const u64 start_cycle = VU->cycle;
	while ((VU->cycle - start_cycle) < max_cycles &&
		   (VU0.VI[REG_VPU_STAT].UL & 0x1) &&
		   !(VU->flags & VUFLAG_MFLAGSET))
	{
		VU->VI[REG_TPC].UL &= VU0_PROGMASK;
		const u32 pc = VU->VI[REG_TPC].UL;
		const u32* ptr = reinterpret_cast<const u32*>(&VU->Micro[pc]);
		const u32 upper = ptr[1];
		if ((upper & 0xf8000000u) != 0x80000000u)
			break;

		_VURegsNum uregs = {};
		if (!VUInterpFast::AnalyzeUpperNoLower(upper, &uregs) ||
			(uregs.pipe != VUPIPE_NONE && uregs.pipe != VUPIPE_FMAC))
		{
			break;
		}

		// PCSX2 owners: VU0microInterp.cpp::_vu0Exec() I-bit path and
		// x86/microVU_Compile.inl::doIbit(). Execute upper with old REG_I,
		// then install the lower word as the new immediate.
		VU->cycle++;
		VU->VI[REG_TPC].UL = pc + 8;
		VU->code = upper;

		const u64 cyclesBeforeOp = VU->cycle - 1;
		_vuTestUpperStalls(VU, &uregs);
		_vuTestPipes(VU);

		if (VU->VIBackupCycles > 0)
			VU->VIBackupCycles -= std::min((u8)(VU->cycle - cyclesBeforeOp), VU->VIBackupCycles);

		IdebugUPPER(VU0);
		VUInterpFast::ExecuteUpperNoLower(VU, upper);
		VU->VI[REG_I].UL = ptr[0];

		if (uregs.pipe == VUPIPE_FMAC)
			_vuClearFMAC(VU);

		_vuAddUpperStalls(VU, &uregs);

		if (uregs.pipe == VUPIPE_FMAC)
			VU->fmacwritepos = (VU->fmacwritepos + 1) & 3;

		steps++;
	}

#if defined(VITASX2_QEMU_VALIDATION)
	g_qemuVuUpperDirectFastSteps += steps;
	g_qemuVuIbitFastSteps += steps;
	g_qemuVuIbitBurstSteps += steps;
#endif
	return steps;
}

static void _vu0Exec(VURegs* VU)
{
	_VURegsNum lregs;
	_VURegsNum uregs;
	u32* ptr;

	ptr = (u32*)&VU->Micro[VU->VI[REG_TPC].UL];
	VU->VI[REG_TPC].UL += 8;

	if (ptr[1] & 0x40000000) // E flag
	{
		VU->ebit = 2;
	}
	if (ptr[1] & 0x20000000 && VU == &VU0) // M flag
	{
		VU->flags |= VUFLAG_MFLAGSET;
		//		Console.WriteLn("fixme: M flag set");
	}
	if (ptr[1] & 0x10000000) // D flag
	{
		if (VU0.VI[REG_FBRST].UL & 0x4)
		{
			VU0.VI[REG_VPU_STAT].UL |= 0x2;
			hwIntcIrq(INTC_VU0);
			VU->ebit = 1;
		}
	}
	if (ptr[1] & 0x08000000) // T flag
	{
		if (VU0.VI[REG_FBRST].UL & 0x8)
		{
			VU0.VI[REG_VPU_STAT].UL |= 0x4;
			hwIntcIrq(INTC_VU0);
			VU->ebit = 1;
		}
	}

	if ((ptr[1] & 0x80000000u) == 0 && _vu0IsUpperNop(ptr[1]))
	{
		// PCSX2 owner: VUops.cpp::_vuNOP() / _vuRegsNOP(). A NOP upper has
		// no reads, writes, or pipe work, so skip its dependency and execute
		// dispatch while keeping the lower opcode on the normal interpreter path.
		VU->code = ptr[0];
		const bool lower_nop = _vu0IsLowerNop(ptr[0]);
		const bool lower_fast = !lower_nop
#if defined(VITASX2_QEMU_VALIDATION)
			&& g_qemuVuLowerDirectFastEnabled
#endif
			&& VUInterpFast::AnalyzeLowerNoUpper(ptr[0], &lregs);
		if (lower_nop)
		{
			// PCSX2 owners: VUops.cpp::_vuMOVE() returns immediately for
			// Ft==0, and x86/microVU_Tables.inl accepts 0x8000033c as NOP.
			memset(&lregs, 0, sizeof(lregs));
		}
		else if (!lower_fast)
		{
			lregs.cycles = 0;
			VU0regs_LOWER_OPCODE[VU->code >> 25](&lregs);
		}
		const u64 cyclesBeforeOp = VU0.cycle - 1;
		_vuTestLowerStalls(VU, &lregs);

		_vuTestPipes(VU);
		if (VU->VIBackupCycles > 0)
			VU->VIBackupCycles -= std::min((u8)(VU0.cycle - cyclesBeforeOp), VU->VIBackupCycles);
		vu0branch = lregs.pipe == VUPIPE_BRANCH;

		if (lower_fast)
		{
			IdebugLOWER(VU0);
			VUInterpFast::ExecuteLowerNoUpper(VU, ptr[0]);
		}
		else if (!lower_nop)
			_vu0ExecLower(VU, ptr);

#if defined(VITASX2_QEMU_VALIDATION)
		++g_qemuVuUpperNopFastSteps;
		if (lower_nop)
			++g_qemuVuLowerNopFastSteps;
		if (lower_fast)
			++g_qemuVuLowerDirectFastSteps;
#endif

		if (lregs.pipe == VUPIPE_FMAC)
			_vuClearFMAC(VU);

		_vuAddLowerStalls(VU, &lregs);

		if (VU->branch > 0)
		{
			if (VU->branch-- == 1)
			{
				VU->VI[REG_TPC].UL = VU->branchpc;

				if (VU->takedelaybranch)
				{
					DevCon.Warning("VU0 - Branch/Jump in Delay Slot");
					VU->branch = 1;
					VU->branchpc = VU->delaybranchpc;
					VU->takedelaybranch = false;
				}
			}
		}

		if (VU->ebit > 0)
		{
			if (VU->ebit-- == 1)
			{
				VU->VIBackupCycles = 0;
				_vuFlushAll(VU);
				VU0.VI[REG_VPU_STAT].UL &= ~0x1; /* E flag */
				vif0Regs.stat.VEW = false;
			}
		}

		if (lregs.pipe == VUPIPE_FMAC)
			VU->fmacwritepos = (VU->fmacwritepos + 1) & 3;

		return;
	}

	VU->code = ptr[1];
	const bool upper_fast =
#if defined(VITASX2_QEMU_VALIDATION)
		g_qemuVuUpperDirectFastEnabled &&
#endif
		VUInterpFast::AnalyzeUpperNoLower(ptr[1], &uregs);
	if (!upper_fast)
		VU0regs_UPPER_OPCODE[VU->code & 0x3f](&uregs);

	u64 cyclesBeforeOp = VU0.cycle - 1;

	_vuTestUpperStalls(VU, &uregs);

	/* check upper flags */
	if (ptr[1] & 0x80000000) // I flag
	{
		_vuTestPipes(VU);

		if (VU->VIBackupCycles > 0)
			VU->VIBackupCycles -= std::min((u8)(VU0.cycle - cyclesBeforeOp), VU->VIBackupCycles);

		_vu0ExecUpperMaybeFast(VU, ptr, upper_fast);

#if defined(VITASX2_QEMU_VALIDATION)
		if (upper_fast)
			++g_qemuVuIbitFastSteps;
#endif

		VU->VI[REG_I].UL = ptr[0];
		memset(&lregs, 0, sizeof(lregs));
	}
	else
	{
		if (_vu0IsLowerNop(ptr[0]))
		{
			// PCSX2 owners: VUops.cpp::_vuMOVE() Ft==0 and
			// x86/microVU_Compile.inl's lower-op NOP handling. The lower slot
			// cannot create dependencies, stalls, branch state, or writes.
			memset(&lregs, 0, sizeof(lregs));

			_vuTestPipes(VU);
			if (VU->VIBackupCycles > 0)
				VU->VIBackupCycles -= std::min((u8)(VU0.cycle - cyclesBeforeOp), VU->VIBackupCycles);
			vu0branch = false;

			_vu0ExecUpperMaybeFast(VU, ptr, upper_fast);
			VU->code = ptr[0];

#if defined(VITASX2_QEMU_VALIDATION)
			++g_qemuVuLowerNopFastSteps;
#endif
		}
		else
		{
			VECTOR _VF;
			VECTOR _VFc;
			REG_VI _VI;
			REG_VI _VIc;
			int vfreg = 0;
			int vireg = 0;
			int discard = 0;

			VU->code = ptr[0];
			const bool lower_fast =
#if defined(VITASX2_QEMU_VALIDATION)
				g_qemuVuLowerDirectFastEnabled &&
#endif
				VUInterpFast::AnalyzeLowerNoUpper(ptr[0], &lregs);
			if (!lower_fast)
			{
				lregs.cycles = 0;
				VU0regs_LOWER_OPCODE[VU->code >> 25](&lregs);
			}
			_vuTestLowerStalls(VU, &lregs);

			_vuTestPipes(VU);
			if (VU->VIBackupCycles > 0)
				VU->VIBackupCycles -= std::min((u8)(VU0.cycle - cyclesBeforeOp), VU->VIBackupCycles);
			vu0branch = lregs.pipe == VUPIPE_BRANCH;

			if (uregs.VFwrite)
			{
				if (lregs.VFwrite == uregs.VFwrite)
				{
					//				Console.Warning("*PCSX2*: Warning, VF write to the same reg in both lower/upper cycle");
					discard = 1;
				}
				if (lregs.VFread0 == uregs.VFwrite ||
					lregs.VFread1 == uregs.VFwrite)
				{
					//				Console.WriteLn("saving reg %d at pc=%x", i, VU->VI[REG_TPC].UL);
					_VF = VU->VF[uregs.VFwrite];
					vfreg = uregs.VFwrite;
				}
			}
			if (uregs.VIread & (1 << REG_CLIP_FLAG))
			{
				if (lregs.VIwrite & (1 << REG_CLIP_FLAG))
				{
					//Console.Warning("*PCSX2*: Warning, VI write to the same reg in both lower/upper cycle");
					discard = 1;
				}
				if (lregs.VIread & (1 << REG_CLIP_FLAG))
				{
					_VI = VU0.VI[REG_CLIP_FLAG];
					vireg = REG_CLIP_FLAG;
				}
			}

			_vu0ExecUpperMaybeFast(VU, ptr, upper_fast);

			if (discard == 0)
			{
				if (vfreg)
				{
					_VFc = VU->VF[vfreg];
					VU->VF[vfreg] = _VF;
				}
				if (vireg)
				{
					_VIc = VU->VI[vireg];
					VU->VI[vireg] = _VI;
				}

				if (lower_fast)
				{
					IdebugLOWER(VU0);
					VUInterpFast::ExecuteLowerNoUpper(VU, ptr[0]);
				}
				else
					_vu0ExecLower(VU, ptr);

				if (vfreg)
				{
					VU->VF[vfreg] = _VFc;
				}
				if (vireg)
				{
					VU->VI[vireg] = _VIc;
				}
			}

#if defined(VITASX2_QEMU_VALIDATION)
			if (lower_fast)
				++g_qemuVuLowerDirectFastSteps;
#endif
		}
	}

	if (uregs.pipe == VUPIPE_FMAC || lregs.pipe == VUPIPE_FMAC)
		_vuClearFMAC(VU);

	_vuAddUpperStalls(VU, &uregs);
	_vuAddLowerStalls(VU, &lregs);

	if (VU->branch > 0)
	{
		if (VU->branch-- == 1)
		{
			VU->VI[REG_TPC].UL = VU->branchpc;

			if (VU->takedelaybranch)
			{
				DevCon.Warning("VU0 - Branch/Jump in Delay Slot");
				VU->branch = 1;
				VU->branchpc = VU->delaybranchpc;
				VU->takedelaybranch = false;
			}
		}
	}

	if (VU->ebit > 0)
	{
		if (VU->ebit-- == 1)
		{
			VU->VIBackupCycles = 0;
			_vuFlushAll(VU);
			VU0.VI[REG_VPU_STAT].UL &= ~0x1; /* E flag */
			vif0Regs.stat.VEW = false;
		}
	}

	// Progress the write position of the FMAC pipeline by one place
	if (uregs.pipe == VUPIPE_FMAC || lregs.pipe == VUPIPE_FMAC)
		VU->fmacwritepos = (VU->fmacwritepos + 1) & 3;
}

void vu0Exec(VURegs* VU)
{
	VU0.VI[REG_TPC].UL &= VU0_PROGMASK;
	const u16 trace_pc = static_cast<u16>(VU0.VI[REG_TPC].UL);
	const u32* trace_ops = reinterpret_cast<const u32*>(&VU->Micro[trace_pc]);
	const u32 trace_lower = trace_ops[0];
	const u32 trace_upper = trace_ops[1];
	VU->cycle++;
	_vu0Exec(VU);
	Pcsx2Trace::RecordVuMicroStep(0, trace_pc, trace_upper, trace_lower, *VU);

	if (VU->VI[0].UL != 0)
		DbgCon.Error("VI[0] != 0!!!!\n");
	if (VU->VF[0].f.x != 0.0f)
		DbgCon.Error("VF[0].x != 0.0!!!!\n");
	if (VU->VF[0].f.y != 0.0f)
		DbgCon.Error("VF[0].y != 0.0!!!!\n");
	if (VU->VF[0].f.z != 0.0f)
		DbgCon.Error("VF[0].z != 0.0!!!!\n");
	if (VU->VF[0].f.w != 1.0f)
		DbgCon.Error("VF[0].w != 1.0!!!!\n");
}

// --------------------------------------------------------------------------------------
//  VU0microInterpreter
// --------------------------------------------------------------------------------------

InterpVU0 CpuIntVU0;

InterpVU0::InterpVU0()
{
	m_Idx = 0;
	IsInterpreter = true;
}

void InterpVU0::Reset()
{
	DevCon.Warning("VU0 Int Reset");
	VU0.fmacwritepos = 0;
	VU0.fmacreadpos = 0;
	VU0.fmaccount = 0;
	VU0.ialuwritepos = 0;
	VU0.ialureadpos = 0;
	VU0.ialucount = 0;
}
void InterpVU0::SetStartPC(u32 startPC)
{
	VU0.start_pc = startPC;
}

void InterpVU0::Step()
{
	vu0Exec(&VU0);
}

void InterpVU0::Execute(u32 cycles)
{
	const FPControlRegisterBackup fpcr_backup(EmuConfig.Cpu.VU0FPCR);

	VU0.VI[REG_TPC].UL <<= 3;
	VU0.flags &= ~VUFLAG_MFLAGSET;
	u64 startcycles = VU0.cycle;
	while ((VU0.cycle - startcycles) < cycles)
	{
		if (!(VU0.VI[REG_VPU_STAT].UL & 0x1))
		{
			// Branches advance the PC to the new location if there was a branch in the E-Bit delay slot
			if (VU0.branch)
			{
				VU0.VI[REG_TPC].UL = VU0.branchpc;
				VU0.branch = 0;
			}
			break;
		}
		if (VU0.flags & VUFLAG_MFLAGSET)
			break;

		const u32 remaining_cycles = static_cast<u32>(cycles - (VU0.cycle - startcycles));
		if (_vu0ExecNopPairBurst(&VU0, remaining_cycles) != 0)
			continue;
		if (_vu0ExecUpperNopLowerDirectBurst(&VU0, remaining_cycles) != 0)
			continue;
		if (_vu0ExecUpperDirectLowerNopBurst(&VU0, remaining_cycles) != 0)
			continue;
		if (_vu0ExecUpperLowerDirectBurst(&VU0, remaining_cycles) != 0)
			continue;
		if (_vu0ExecIbitDirectBurst(&VU0, remaining_cycles) != 0)
			continue;

		vu0Exec(&VU0);
	}
	VU0.VI[REG_TPC].UL >>= 3;

	if (EmuConfig.Speedhacks.EECycleRate != 0 && (!EmuConfig.Gamefixes.VUSyncHack || EmuConfig.Speedhacks.EECycleRate < 0))
	{
		u64 cycle_change = VU0.cycle - startcycles;
		VU0.cycle -= cycle_change;
		switch (std::min(static_cast<int>(EmuConfig.Speedhacks.EECycleRate), static_cast<int>(cycle_change)))
		{
			case -3: // 50%
				cycle_change *= 2.0f;
				break;
			case -2: // 60%
				cycle_change *= 1.6666667f;
				break;
			case -1: // 75%
				cycle_change *= 1.3333333f;
				break;
			case 1: // 130%
				cycle_change /= 1.3f;
				break;
			case 2: // 180%
				cycle_change /= 1.8f;
				break;
			case 3: // 300%
				cycle_change /= 3.0f;
				break;
			default:
				break;
		}
		VU0.cycle += cycle_change;
	}

	VU0.nextBlockCycles = (VU0.cycle - cpuRegs.cycle) + 1;
}
