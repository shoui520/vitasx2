// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"

#include "VUmicro.h"
#include "VUmicroFast.h"
#include "DebugTools/VuTrace.h"
#include "GS.h"
#include "Gif_Unit.h"
#include "MTVU.h"

#include <cfenv>

extern void _vuFlushAll(VURegs* VU);
extern void _vuXGKICKFlush(VURegs* VU);

#if defined(VITASX2_QEMU_VALIDATION)
extern u32 g_qemuVuUpperNopFastSteps;
extern u32 g_qemuVuLowerNopFastSteps;
extern u32 g_qemuVuNopPairBurstSteps;
extern u32 g_qemuVuLowerDirectFastSteps;
extern u32 g_qemuVuUpperDirectFastSteps;
extern bool g_qemuVuLowerDirectFastEnabled;
extern bool g_qemuVuUpperDirectFastEnabled;
#endif

void _vu1ExecUpper(VURegs* VU, u32* ptr)
{
	VU->code = ptr[1];
	IdebugUPPER(VU1);
	VU1_UPPER_OPCODE[VU->code & 0x3f]();
}

void _vu1ExecLower(VURegs* VU, u32* ptr)
{
	VU->code = ptr[0];
	IdebugLOWER(VU1);
	VU1_LOWER_OPCODE[VU->code >> 25]();
}

static void _vu1ExecUpperMaybeFast(VURegs* VU, u32* ptr, bool upper_fast)
{
	if (upper_fast)
	{
		VU->code = ptr[1];
		IdebugUPPER(VU1);
		VUInterpFast::ExecuteUpperNoLower(VU, ptr[1]);
#if defined(VITASX2_QEMU_VALIDATION)
		++g_qemuVuUpperDirectFastSteps;
#endif
		return;
	}

	_vu1ExecUpper(VU, ptr);
}

int vu1branch = 0;

static __fi bool _vu1IsUpperNop(u32 upper)
{
	return (upper & 0x07ffffffu) == 0x000002ffu;
}

static __fi bool _vu1IsLowerNop(u32 lower)
{
	return lower == 0x8000033cu;
}

static __fi bool _vu1IsPlainNopPair(u32 upper, u32 lower)
{
	return upper == 0x000002ffu && _vu1IsLowerNop(lower);
}

static u32 _vu1ExecNopPairBurst(VURegs* VU, u32 max_steps)
{
	if (max_steps == 0 || Pcsx2Trace::IsVuTraceEnabled() ||
		VU->branch != 0 || VU->ebit != 0 || VU->takedelaybranch)
	{
		return 0;
	}

	u32 steps = 0;
	while (steps < max_steps && (VU0.VI[REG_VPU_STAT].UL & 0x100))
	{
		VU->VI[REG_TPC].UL &= VU1_PROGMASK;
		const u32 pc = VU->VI[REG_TPC].UL;
		const u32* ptr = reinterpret_cast<const u32*>(&VU->Micro[pc]);
		if (!_vu1IsPlainNopPair(ptr[1], ptr[0]))
			break;

		// PCSX2 owners: VUops.cpp::_vuNOP(), _vuMOVE(Ft==0), and
		// VU1microInterp.cpp::vu1Exec(). Plain NOP pairs have no register
		// dependencies or writes, but still advance time and flush pending pipes.
		VU->cycle++;
		VU->VI[REG_TPC].UL = pc + 8;
		const u64 cyclesBeforeOp = VU->cycle - 1;
		_vuTestPipes(VU);
		if (VU->VIBackupCycles > 0)
			VU->VIBackupCycles -= std::min((u8)(VU->cycle - cyclesBeforeOp), VU->VIBackupCycles);
		VU->code = ptr[0];
		steps++;
	}

#if defined(VITASX2_QEMU_VALIDATION)
	g_qemuVuNopPairBurstSteps += steps;
#endif
	return steps;
}

static void _vu1Exec(VURegs* VU)
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
	if (ptr[1] & 0x10000000) // D flag
	{
		if (VU0.VI[REG_FBRST].UL & 0x400)
		{
			VU0.VI[REG_VPU_STAT].UL |= 0x200;
			hwIntcIrq(INTC_VU1);
			VU->ebit = 1;
		}
	}
	if (ptr[1] & 0x08000000) // T flag
	{
		if (VU0.VI[REG_FBRST].UL & 0x800)
		{
			VU0.VI[REG_VPU_STAT].UL |= 0x400;
			hwIntcIrq(INTC_VU1);
			VU->ebit = 1;
		}
	}

	//VUM_LOG("VU->cycle = %d (flags st=%x;mac=%x;clip=%x,q=%f)", VU->cycle, VU->statusflag, VU->macflag, VU->clipflag, VU->q.F);

	if ((ptr[1] & 0x80000000u) == 0 && _vu1IsUpperNop(ptr[1]))
	{
		// PCSX2 owner: VUops.cpp::_vuNOP() / _vuRegsNOP(). A NOP upper has
		// no reads, writes, or pipe work, so skip its dependency and execute
		// dispatch while keeping the lower opcode on the normal interpreter path.
		VU->code = ptr[0];
		const bool lower_nop = _vu1IsLowerNop(ptr[0]);
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
			VU1regs_LOWER_OPCODE[VU->code >> 25](&lregs);
		}

		const u32 cyclesBeforeOp = VU1.cycle - 1;
		_vuTestLowerStalls(VU, &lregs);
		_vuTestPipes(VU);

		if (VU->VIBackupCycles > 0)
			VU->VIBackupCycles -= std::min((u8)(VU1.cycle - cyclesBeforeOp), VU->VIBackupCycles);

		if (lower_fast)
		{
			IdebugLOWER(VU1);
			VUInterpFast::ExecuteLowerNoUpper(VU, ptr[0]);
		}
		else if (!lower_nop)
			_vu1ExecLower(VU, ptr);

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
					//DevCon.Warning("VU1 - Branch/Jump in Delay Slot");
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
				VU0.VI[REG_VPU_STAT].UL &= ~0x100;
				vif1Regs.stat.VEW = false;

				if(VU1.xgkickenable)
					_vuXGKICKTransfer(0, true);
				if (INSTANT_VU1)
					VU1.xgkicklastcycle = cpuRegs.cycle;
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
		VU1regs_UPPER_OPCODE[VU->code & 0x3f](&uregs);

	u32 cyclesBeforeOp = VU1.cycle-1;

	_vuTestUpperStalls(VU, &uregs);

	/* check upper flags */
	if (ptr[1] & 0x80000000) // I Flag (Lower op is a float)
	{
		_vuTestPipes(VU);

		if (VU->VIBackupCycles > 0)
			VU->VIBackupCycles -= std::min((u8)(VU1.cycle - cyclesBeforeOp), VU->VIBackupCycles);

		_vu1ExecUpperMaybeFast(VU, ptr, upper_fast);

		VU->VI[REG_I].UL = ptr[0];
		//Lower not used, set to 0 to fill in the FMAC stall gap
		//Could probably get away with just running upper stalls, but lets not tempt fate.
		memset(&lregs, 0, sizeof(lregs));
	}
	else
	{
		if (_vu1IsLowerNop(ptr[0]))
		{
			// PCSX2 owners: VUops.cpp::_vuMOVE() Ft==0 and
			// x86/microVU_Compile.inl's lower-op NOP handling. The lower slot
			// cannot create dependencies, stalls, branch state, or writes.
			memset(&lregs, 0, sizeof(lregs));

			_vuTestPipes(VU);
			if (VU->VIBackupCycles > 0)
				VU->VIBackupCycles-= std::min((u8)(VU1.cycle- cyclesBeforeOp), VU->VIBackupCycles);

			_vu1ExecUpperMaybeFast(VU, ptr, upper_fast);
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
				VU1regs_LOWER_OPCODE[VU->code >> 25](&lregs);
			}

			_vuTestLowerStalls(VU, &lregs);
			_vuTestPipes(VU);

			if (VU->VIBackupCycles > 0)
				VU->VIBackupCycles-= std::min((u8)(VU1.cycle- cyclesBeforeOp), VU->VIBackupCycles);

			if (uregs.VFwrite)
			{
				if (lregs.VFwrite == uregs.VFwrite)
				{
					//Console.Warning("*PCSX2*: Warning, VF write to the same reg in both lower/upper cycle pc=%x", VU->VI[REG_TPC].UL);
					discard = 1;
				}
				if (lregs.VFread0 == uregs.VFwrite ||
					lregs.VFread1 == uregs.VFwrite)
				{
					//Console.WriteLn("saving reg %d at pc=%x", uregs.VFwrite, VU->VI[REG_TPC].UL);
					_VF = VU->VF[uregs.VFwrite];
					vfreg = uregs.VFwrite;
				}
			}
			if (uregs.VIwrite & (1 << REG_CLIP_FLAG))
			{
				if (lregs.VIwrite & (1 << REG_CLIP_FLAG))
				{
					//Console.Warning("*PCSX2*: Warning, VI write to the same reg in both lower/upper cyclepc=%x", VU->VI[REG_TPC].UL);
					discard = 1;
				}
				if (lregs.VIread & (1 << REG_CLIP_FLAG))
				{
					//Console.Warning("*PCSX2*: Warning, VI read same cycle as write pc=%x", VU->VI[REG_TPC].UL);
					_VI = VU->VI[REG_CLIP_FLAG];
					vireg = REG_CLIP_FLAG;
				}
			}

			_vu1ExecUpperMaybeFast(VU, ptr, upper_fast);

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
					IdebugLOWER(VU1);
					VUInterpFast::ExecuteLowerNoUpper(VU, ptr[0]);
				}
				else
					_vu1ExecLower(VU, ptr);

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
	// Clear an FMAC read for use
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
				//DevCon.Warning("VU1 - Branch/Jump in Delay Slot");
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
			VU0.VI[REG_VPU_STAT].UL &= ~0x100;
			vif1Regs.stat.VEW = false;

			if(VU1.xgkickenable)
				_vuXGKICKTransfer(0, true);
			// In instant VU mode, VU1 goes WAY ahead of the CPU, making the XGKick fall way behind
			// We also have some code to update it in VIF Unpacks too, since in some games (Aggressive Inline) overwrite the XGKick data
			// VU currently flushes XGKICK on end, so this isn't needed, yet
			if (INSTANT_VU1)
				VU1.xgkicklastcycle = cpuRegs.cycle;
		}
	}

	// Progress the write position of the FMAC pipeline by one place
	if (uregs.pipe == VUPIPE_FMAC || lregs.pipe == VUPIPE_FMAC)
		VU->fmacwritepos = (VU->fmacwritepos + 1) & 3;
}

void vu1Exec(VURegs* VU)
{
	const u16 trace_pc = static_cast<u16>(VU1.VI[REG_TPC].UL);
	const u32* trace_ops = reinterpret_cast<const u32*>(&VU->Micro[trace_pc]);
	const u32 trace_lower = trace_ops[0];
	const u32 trace_upper = trace_ops[1];
	VU->cycle++;
	_vu1Exec(VU);
	Pcsx2Trace::RecordVuMicroStep(1, trace_pc, trace_upper, trace_lower, *VU);

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

InterpVU1 CpuIntVU1;

InterpVU1::InterpVU1()
{
	m_Idx = 1;
	IsInterpreter = true;
}

void InterpVU1::Reset()
{
	DevCon.Warning("VU1 Int Reset");
	VU1.fmacwritepos = 0;
	VU1.fmacreadpos = 0;
	VU1.fmaccount = 0;
	VU1.ialuwritepos = 0;
	VU1.ialureadpos = 0;
	VU1.ialucount = 0;
}

void InterpVU1::SetStartPC(u32 startPC)
{
	VU1.start_pc = startPC;
}

void InterpVU1::Step()
{
	VU1.VI[REG_TPC].UL &= VU1_PROGMASK;
	vu1Exec(&VU1);
}

void InterpVU1::Execute(u32 cycles)
{
	const FPControlRegisterBackup fpcr_backup(EmuConfig.Cpu.VU1FPCR);

	VU1.VI[REG_TPC].UL <<= 3;
	u64 startcycles = VU1.cycle;

	while ((VU1.cycle - startcycles) < cycles)
	{
		if (!(VU0.VI[REG_VPU_STAT].UL & 0x100))
		{
			if (VU1.branch == 1)
			{
				VU1.VI[REG_TPC].UL = VU1.branchpc;
				VU1.branch = 0;
			}
			break;
		}
		const u32 remaining_cycles = static_cast<u32>(cycles - (VU1.cycle - startcycles));
		if (_vu1ExecNopPairBurst(&VU1, remaining_cycles) != 0)
			continue;
		Step();
	}
	VU1.VI[REG_TPC].UL >>= 3;
	VU1.nextBlockCycles = (VU1.cycle - cpuRegs.cycle) + 1;
}
