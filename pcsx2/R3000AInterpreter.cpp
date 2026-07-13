// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "R3000A.h"
#include "Common.h"
#include "Config.h"
#include "VMManager.h"

#include "R5900OpcodeTables.h"
#include "DebugTools/Breakpoints.h"
#include "IopBios.h"
#include "IopDma.h"
#include "IopHw.h"
#include "IopMem.h"
#ifdef VITASX2_VITA
#include "vita/VitaCore.h"
#endif

#include <cstring>

using namespace R3000A;

// Used to flag delay slot instructions when throwig exceptions.
bool iopIsDelaySlot = false;

static bool branch2 = 0;
static u32 branchPC;

#ifdef VITASX2_VITA
// PCSX2's x86 IOP recompiler keeps s_psxBlockCycles private until the owning
// BaseBlock tail. A rare Vita compile fallback still has to execute the exact
// already-scanned window even when a preceding no-test link exhausted the EE
// budget. This mode borrows the interpreter's opcode semantics while leaving
// cycle publication and event ownership to the Vita recompiler dispatcher.
static bool s_vitaIopRecompilerFallbackActive = false;
static bool s_vitaIopRecompilerFallbackIrxHandled = false;
static bool s_vitaIopRecompilerFallbackExceptionHandled = false;
static bool s_vitaIopRecompilerFallbackUnalignedTarget = false;
static bool s_vitaIopRecompilerFallbackBreakpoint = false;
static u32 s_vitaIopRecompilerFallbackInstructions = 0;
static u32 s_vitaIopRecompilerFallbackInstructionCount = 0;
static const u32* s_vitaIopRecompilerFallbackOpcodes = nullptr;
static bool s_vitaIopRecompilerRegisterJumpTargetValid = false;
static u32 s_vitaIopRecompilerRegisterJumpTarget = 0;
static u32 s_vitaIopRecompilerConstGprs = 1;
static bool s_vitaIopRecompilerCompileBranch = false;
static bool s_vitaIopRecompilerDelayPending = false;
static u32 s_vitaIopRecompilerDelayInstruction = 0;
static bool s_vitaIopRecompilerDelayBranch = false;

static bool VitaIopRecompilerOpcodeIsBranch(u32 op)
{
	const u32 primary = op >> 26;
	if (primary == 0)
		return (op & 0x3fu) == 8u || (op & 0x3fu) == 9u;
	if (primary == 1)
	{
		const u32 rt = (op >> 16) & 0x1fu;
		return rt == 0u || rt == 1u || rt == 16u || rt == 17u;
	}
	return primary >= 2u && primary <= 7u;
}

static bool VitaIopRecompilerOpcodeIsException(u32 op)
{
	return (op >> 26) == 0 && ((op & 0x3fu) == 0x0cu || (op & 0x3fu) == 0x0du);
}

static bool VitaIopRecompilerInstructionRedirected(u32 op, u32 instruction_pc)
{
	// PCSX2 owner: x86/iR3000A.cpp::rpsxSYSCALL/rpsxBREAK compares the
	// helper's resulting PC with psxpc-4 (the exception instruction), not the
	// ordinary sequential PC. Every other nonbranch fallback instruction uses
	// the sequential continuation contract.
	const u32 expected_pc = VitaIopRecompilerOpcodeIsException(op) ?
		instruction_pc : instruction_pc + 4;
	return psxRegs.pc != expected_pc;
}

static bool VitaIopRecompilerGprIsConst(u32 reg)
{
	return (s_vitaIopRecompilerConstGprs & (1u << reg)) != 0;
}

static void VitaIopRecompilerSetGprConst(u32 reg, bool is_const)
{
	if (reg == 0)
		return;
	if (is_const)
		s_vitaIopRecompilerConstGprs |= 1u << reg;
	else
		s_vitaIopRecompilerConstGprs &= ~(1u << reg);
}

static void VitaIopRecompilerDeleteGprConst(u32 reg)
{
	if (reg < 32)
		s_vitaIopRecompilerConstGprs &= ~(1u << reg);
}

static void VitaIopRecompilerUpdateConstState(u32 op)
{
	const u32 primary = op >> 26;
	const u32 rs = (op >> 21) & 0x1fu;
	const u32 rt = (op >> 16) & 0x1fu;
	const u32 rd = (op >> 11) & 0x1fu;
	if (primary == 0)
	{
		switch (op & 0x3fu)
		{
			case 0x00: // SLL
			case 0x02: // SRL
			case 0x03: // SRA
				VitaIopRecompilerSetGprConst(rd, VitaIopRecompilerGprIsConst(rt));
				return;
			case 0x04: // SLLV
			case 0x06: // SRLV
			case 0x07: // SRAV
			case 0x20: // ADD
			case 0x21: // ADDU
			case 0x22: // SUB
			case 0x23: // SUBU
			case 0x24: // AND
			case 0x25: // OR
			case 0x26: // XOR
			case 0x27: // NOR
			case 0x2a: // SLT
			case 0x2b: // SLTU
				VitaIopRecompilerSetGprConst(rd, VitaIopRecompilerGprIsConst(rs) &&
													 VitaIopRecompilerGprIsConst(rt));
				return;
			case 0x10: // MFHI
			case 0x12: // MFLO
				// The x86 recompiler does not represent HI/LO in its
				// 32-bit GPR constant mask.
				VitaIopRecompilerSetGprConst(rd, false);
				return;
			default:
				return;
		}
	}

	if (primary >= 0x08u && primary <= 0x0eu)
	{
		VitaIopRecompilerSetGprConst(rt, VitaIopRecompilerGprIsConst(rs));
		return;
	}
	if (primary == 0x0fu) // LUI
	{
		VitaIopRecompilerSetGprConst(rt, true);
		return;
	}
	if (primary == 0x10u && (rs == 0u || rs == 2u)) // MFC/CFC0
	{
		// x86 rpsxMFC0/rpsxCFC0 return before touching the constant mask
		// when Rt is zero. Ordinary writes still make their destination
		// dynamic.
		VitaIopRecompilerSetGprConst(rt, false);
		return;
	}
	if (primary == 0x12u)
	{
		// Every valid x86 REC_GTE_FUNC path invalidates _Rt_, including
		// MTC2/CTC2 and commands whose Rt field is not an architectural GPR.
		// Preserve that compiler topology without changing invalid psxNULL ops.
		constexpr u64 GTE_FUNCTIONS =
			(1ull << 1) | (1ull << 6) | (1ull << 12) | (1ull << 16) | (1ull << 17) |
			(1ull << 18) | (1ull << 19) | (1ull << 20) | (1ull << 22) |
			(1ull << 27) | (1ull << 28) | (1ull << 30) | (1ull << 32) |
			(1ull << 40) | (1ull << 41) | (1ull << 42) | (1ull << 45) |
			(1ull << 46) | (1ull << 48) | (1ull << 61) | (1ull << 62) |
			(1ull << 63);
		const u32 function = op & 0x3fu;
		const bool basic =
			function == 0u && (rs == 0u || rs == 2u || rs == 4u || rs == 6u);
		if (basic || (GTE_FUNCTIONS & (1ull << function)) != 0)
		{
			// REC_GTE_FUNC unconditionally applies PSX_DEL_CONST(_Rt_)
			// after the helper, including the encoded zero field of GTE
			// commands and MFC2/CFC2 with Rt==0.
			VitaIopRecompilerDeleteGprConst(rt);
		}
		return;
	}
	if (primary >= 0x20u && primary <= 0x26u) // Integer loads
	{
		if (primary == 0x22u || primary == 0x26u) // LWL/LWR REC_FUNC
			VitaIopRecompilerDeleteGprConst(rt);
		else
			VitaIopRecompilerSetGprConst(rt, false);
	}
	else if (primary == 0x2au || primary == 0x2eu || primary == 0x32u ||
			 primary == 0x3au)
	{
		// x86 REC_FUNC(SWL/SWR) and REC_GTE_FUNC(LWC2/SWC2) also
		// invalidate their encoded Rt field.
		VitaIopRecompilerDeleteGprConst(rt);
	}
}

static void VitaIopRecompilerPrepareBranch(u32 op)
{
	const u32 primary = op >> 26;
	const u32 rs = (op >> 21) & 0x1fu;
	const u32 rt = (op >> 16) & 0x1fu;
	bool delay_branch = s_vitaIopRecompilerCompileBranch;

	if (primary == 3u) // JAL
	{
		VitaIopRecompilerSetGprConst(31, true);
	}
	else if (primary == 0u && (op & 0x3fu) == 9u) // JALR
	{
		VitaIopRecompilerSetGprConst((op >> 11) & 0x1fu, true);
	}

	bool conditional = primary >= 4u && primary <= 7u;
	bool taken = false;
	bool single_path = false;
	if (primary == 1u)
	{
		conditional = rt == 0u || rt == 1u || rt == 16u || rt == 17u;
		if (rt == 16u || rt == 17u)
		{
			// rpsxBLTZAL/rpsxBGEZAL make r31 constant before testing Rs.
			VitaIopRecompilerSetGprConst(31, true);
		}
	}

	if (conditional)
	{
		u32 rs_value = psxRegs.GPR.r[rs];
		if (primary == 1u && (rt == 16u || rt == 17u) && rs == 31u)
			rs_value = psxRegs.pc + 4;

		switch (primary)
		{
			case 1:
				taken = rt == 0u || rt == 16u ? static_cast<s32>(rs_value) < 0 : static_cast<s32>(rs_value) >= 0;
				single_path = VitaIopRecompilerGprIsConst(rs);
				break;
			case 4: // BEQ
				taken = rs_value == psxRegs.GPR.r[rt];
				single_path = rs == rt || (VitaIopRecompilerGprIsConst(rs) &&
											  VitaIopRecompilerGprIsConst(rt));
				break;
			case 5: // BNE
				taken = rs_value != psxRegs.GPR.r[rt];
				single_path = rs == rt || (VitaIopRecompilerGprIsConst(rs) &&
											  VitaIopRecompilerGprIsConst(rt));
				break;
			case 6: // BLEZ
				taken = static_cast<s32>(rs_value) <= 0;
				single_path = VitaIopRecompilerGprIsConst(rs);
				break;
			case 7: // BGTZ
				taken = static_cast<s32>(rs_value) > 0;
				single_path = VitaIopRecompilerGprIsConst(rs);
				break;
		}

		// PCSX2 owner: x86/iR3000Atables.cpp::rpsx{BEQ,BNE,BLTZ,
		// BGEZ,BLTZAL,BGEZAL,BLEZ,BGTZ}. A dynamic branch emits two
		// delay copies. psxLoadBranchState() does not restore psxbranch,
		// so the second copy passes BD=true to rpsxSYSCALL/rpsxBREAK.
		// BEQ emits its not-taken copy second; every other family emits
		// its taken copy second. Constant-proven branches have one copy.
		const bool second_path = primary == 4u ? !taken : taken;
		if (!single_path && second_path)
			delay_branch = true;
	}

	s_vitaIopRecompilerDelayPending = true;
	s_vitaIopRecompilerDelayInstruction = s_vitaIopRecompilerFallbackInstructions;
	s_vitaIopRecompilerDelayBranch = delay_branch;
	s_vitaIopRecompilerCompileBranch = true;
}

static bool VitaExecuteIopRecompilerDirectLoad(u32 op)
{
	const u32 primary = op >> 26;
	if (primary != 0x20u && primary != 0x21u && primary != 0x23u &&
		primary != 0x24u && primary != 0x25u)
	{
		return false;
	}

	const u32 rs = (op >> 21) & 0x1fu;
	const u32 rt = (op >> 16) & 0x1fu;
	const u32 address = psxRegs.GPR.r[rs] +
	                    static_cast<u32>(static_cast<s32>(static_cast<s16>(op)));
	if ((address & 0x10000000u) != 0)
		return false;

	// PCSX2 owner: x86/iR3000Atables.cpp::rpsxLoad(). The recompiler's
	// ordinary-RAM test is bit 28, not the interpreter RLUT's 29-bit map.
	// A load to r0 also skips the direct RAM read entirely.
	if (rt == 0)
		return true;
	const u8* const source =
		&iopMem->Main[address & (Ps2MemSize::ExposedIopRam - 1)];
	switch (primary)
	{
		case 0x20: // LB
			psxRegs.GPR.r[rt] =
				static_cast<u32>(static_cast<s32>(static_cast<s8>(*source)));
			return true;
		case 0x24: // LBU
			psxRegs.GPR.r[rt] = *source;
			return true;
		case 0x21: // LH
		case 0x25: // LHU
		{
			u16 value = 0;
			std::memcpy(&value, source, sizeof(value));
			psxRegs.GPR.r[rt] =
				primary == 0x21u ? static_cast<u32>(static_cast<s32>(static_cast<s16>(value))) : value;
			return true;
		}
		case 0x23: // LW
			std::memcpy(&psxRegs.GPR.r[rt], source, sizeof(psxRegs.GPR.r[rt]));
			return true;
		default:
			return false;
	}
}
#endif

static void doBranch(s32 tar);	// forward declared prototype

/*********************************************************
* Register branch logic                                  *
* Format:  OP rs, offset                                 *
*********************************************************/

void psxBGEZ()         // Branch if Rs >= 0
{
	if (_i32(_rRs_) >= 0)
		doBranch(_BranchTarget_);
}

void psxBGEZAL()   // Branch if Rs >= 0 and link
{
	_SetLink(31);
	if (_i32(_rRs_) >= 0)
	{
		doBranch(_BranchTarget_);
	}
}

void psxBGTZ()          // Branch if Rs >  0
{
	if (_i32(_rRs_) > 0)
		doBranch(_BranchTarget_);
}

void psxBLEZ()         // Branch if Rs <= 0
{
	if (_i32(_rRs_) <= 0)
		doBranch(_BranchTarget_);
}
void psxBLTZ()          // Branch if Rs <  0
{
	if (_i32(_rRs_) < 0)
		doBranch(_BranchTarget_);
}

void psxBLTZAL()    // Branch if Rs <  0 and link
{
	_SetLink(31);
	if (_i32(_rRs_) < 0)
		{
			doBranch(_BranchTarget_);
		}
}

/*********************************************************
* Register branch logic                                  *
* Format:  OP rs, rt, offset                             *
*********************************************************/

void psxBEQ()   // Branch if Rs == Rt
{
	if (_i32(_rRs_) == _i32(_rRt_)) doBranch(_BranchTarget_);
}

void psxBNE()   // Branch if Rs != Rt
{
	if (_i32(_rRs_) != _i32(_rRt_)) doBranch(_BranchTarget_);
}

/*********************************************************
* Jump to target                                         *
* Format:  OP target                                     *
*********************************************************/
void psxJ()
{
	psxDoJump(_JumpTarget_);
}

void psxJAL()
{
	_SetLink(31);
	doBranch(_JumpTarget_);
}

/*********************************************************
* Register jump                                          *
* Format:  OP rs, rd                                     *
*********************************************************/
void psxJR() { doBranch(_u32(_rRs_)); }

void psxJALR()
{
	if (_Rd_)
	{
		_SetLink(_Rd_);
	}
	doBranch(_u32(_rRs_));
}

void psxBreakpoint(bool memcheck)
{
	u32 pc = psxRegs.pc;
	if (CBreakPoints::CheckSkipFirst(BREAKPOINT_IOP, pc) != 0)
	{
		CBreakPoints::ClearSkipFirst(BREAKPOINT_IOP);
		return;
	}

	if (!memcheck)
	{
		auto cond = CBreakPoints::GetBreakPointCondition(BREAKPOINT_IOP, pc);
		if (cond && !cond->Evaluate())
			return;
	}

	CBreakPoints::SetBreakpointTriggered(true, BREAKPOINT_IOP);
	VMManager::SetPaused(true);
	Cpu->ExitExecution();
#ifdef VITASX2_VITA
	if (s_vitaIopRecompilerFallbackActive)
		s_vitaIopRecompilerFallbackBreakpoint = true;
#endif
}

void psxMemcheck(u32 op, u32 bits, bool store)
{
	// compute accessed address
	u32 start = psxRegs.GPR.r[(op >> 21) & 0x1F];
	if ((s16)op != 0)
		start += (s16)op;

	u32 end = start + bits / 8;

	auto checks = CBreakPoints::GetMemChecks(BREAKPOINT_IOP);
	for (size_t i = 0; i < checks.size(); i++)
	{
		auto& check = checks[i];

		if (check.result == 0)
			continue;
		if ((check.memCond & MEMCHECK_WRITE) == 0 && store)
			continue;
		if ((check.memCond & MEMCHECK_READ) == 0 && !store)
			continue;

		if (check.hasCond)
		{
			if (!check.cond.Evaluate())
				continue;
		}

		if (start < check.end && check.start < end)
			psxBreakpoint(true);
	}
}

void psxCheckMemcheck()
{
	u32 pc = psxRegs.pc;
	int needed = psxIsMemcheckNeeded(pc);
	if (needed == 0)
		return;

	u32 op = iopMemRead32(needed == 2 ? pc + 4 : pc);
	// Yeah, we use the R5900 opcode table for the R3000
	const R5900::OPCODE& opcode = R5900::GetInstruction(op);

	bool store = (opcode.flags & IS_STORE) != 0;
	switch (opcode.flags & MEMTYPE_MASK)
	{
	case MEMTYPE_BYTE:
		psxMemcheck(op, 8, store);
		break;
	case MEMTYPE_HALF:
		psxMemcheck(op, 16, store);
		break;
	case MEMTYPE_WORD:
		psxMemcheck(op, 32, store);
		break;
	case MEMTYPE_DWORD:
		psxMemcheck(op, 64, store);
		break;
	}
}

///////////////////////////////////////////
// These macros are used to assemble the repassembler functions

static __fi void execI()
{
#ifdef VITASX2_VITA
	const bool recompiler_delay_instruction =
		s_vitaIopRecompilerFallbackActive && s_vitaIopRecompilerDelayPending &&
		s_vitaIopRecompilerDelayInstruction == s_vitaIopRecompilerFallbackInstructions;
	if (recompiler_delay_instruction)
	{
		s_vitaIopRecompilerDelayPending = false;
		s_vitaIopRecompilerCompileBranch = s_vitaIopRecompilerDelayBranch;
		iopIsDelaySlot = s_vitaIopRecompilerDelayBranch;
	}
#endif

	// This function is called for every instruction.
	// Enabling the define below will probably, no, will cause the interpretor to be slower.
//#define EXTRA_DEBUG
#if defined(EXTRA_DEBUG) || defined(PCSX2_DEVBUILD)
	if (psxIsBreakpointNeeded(psxRegs.pc))
		psxBreakpoint(false);
#ifdef VITASX2_VITA
	if (s_vitaIopRecompilerFallbackBreakpoint)
		return;
#endif

	psxCheckMemcheck();
#ifdef VITASX2_VITA
	if (s_vitaIopRecompilerFallbackBreakpoint)
		return;
#endif
	
	CBreakPoints::CommitClearSkipFirst(BREAKPOINT_IOP);
#endif

	// Inject IRX hack
	if (
#ifdef VITASX2_VITA
		!s_vitaIopRecompilerFallbackActive &&
#endif
		psxRegs.pc == 0x1630 && EmuConfig.CurrentIRX.length() > 3) {
		if (iopMemRead32(0x20018) == 0x1F) {
			// FIXME do I need to increase the module count (0x1F -> 0x20)
			iopMemWrite32(0x20094, 0xbffc0000);
		}
	}

	psxRegs.code =
#ifdef VITASX2_VITA
		s_vitaIopRecompilerFallbackActive ?
			s_vitaIopRecompilerFallbackOpcodes[s_vitaIopRecompilerFallbackInstructions] :
#endif
			iopMemRead32(psxRegs.pc);
#ifdef VITASX2_VITA
	if (s_vitaIopRecompilerFallbackActive && (psxRegs.code >> 26) == 0 &&
		((psxRegs.code & 0x3fu) == 8u || (psxRegs.code & 0x3fu) == 9u))
	{
		// x86 rpsxJR/rpsxJALR captures Rs in PCWRITEBACK before JALR
		// writes Rd. The interpreter's JALR helper does the writes in the
		// opposite order when Rd==Rs, so retain the owner target here.
		s_vitaIopRecompilerRegisterJumpTarget =
			psxRegs.GPR.r[(psxRegs.code >> 21) & 0x1fu];
		s_vitaIopRecompilerRegisterJumpTargetValid = true;
	}
	if (VitaRecordIopPreInstruction(psxRegs.pc, psxRegs.code))
	{
		psxRegs.iopCycleEE = 0;
		branch2 = 1;
		Cpu->ExitExecution();
		return;
	}
#endif

		PSXCPU_LOG("%s", disR3000AF(psxRegs.code, psxRegs.pc));

	psxRegs.pc+= 4;
#ifdef VITASX2_VITA
	if (s_vitaIopRecompilerFallbackActive)
		s_vitaIopRecompilerFallbackInstructions++;
	else
#endif
	psxRegs.cycle++;

#ifdef VITASX2_VITA
	const u32 recompiler_op = psxRegs.code;
	if (s_vitaIopRecompilerFallbackActive &&
		VitaIopRecompilerOpcodeIsBranch(recompiler_op))
	{
		VitaIopRecompilerPrepareBranch(recompiler_op);
	}
#endif

	const bool recompiler_rfe =
#ifdef VITASX2_VITA
		s_vitaIopRecompilerFallbackActive &&
#else
		false &&
#endif
		(psxRegs.code >> 26) == 0x10u && ((psxRegs.code >> 21) & 0x1fu) == 0x10u;
	const bool recompiler_direct_load =
#ifdef VITASX2_VITA
		s_vitaIopRecompilerFallbackActive &&
		VitaExecuteIopRecompilerDirectLoad(psxRegs.code);
#else
		false;
#endif
	if (!recompiler_direct_load)
	psxBSC[psxRegs.code >> 26]();
	if (recompiler_rfe)
	{
		// PCSX2 owner: x86/iR3000Atables.cpp::rpsxRFE() tests pending
		// IOP INTC state immediately after restoring Status. psxRFE() is
		// interpreter-owned and normally relies on its later event seam.
		iopTestIntc();
	}
#ifdef VITASX2_VITA
	if (s_vitaIopRecompilerFallbackActive)
	{
		if (!VitaIopRecompilerOpcodeIsBranch(recompiler_op))
			VitaIopRecompilerUpdateConstState(recompiler_op);
		if (recompiler_delay_instruction)
		{
			s_vitaIopRecompilerCompileBranch = true;
			iopIsDelaySlot = false;
		}
	}
#endif
}

void psxDoBranch(u32 tar)
{
#ifdef VITASX2_VITA
	if (s_vitaIopRecompilerFallbackActive &&
		s_vitaIopRecompilerRegisterJumpTargetValid)
	{
		tar = s_vitaIopRecompilerRegisterJumpTarget;
		s_vitaIopRecompilerRegisterJumpTargetValid = false;
	}
#endif
	if (tar == 0x0)
		DevCon.Warning("[R3000 Interpreter] Warning: Branch to 0x0!");

	// When upgrading the IOP, there are two resets, the second of which is a
	// 'fake' reset This second 'reset' involves UDNL calling SYSMEM and LOADCORE
	// directly, resetting LOADCORE's modules This detects when SYSMEM is called
	// and clears the modules then
	if (
#ifdef VITASX2_VITA
		!s_vitaIopRecompilerFallbackActive &&
#endif
		tar == 0x890)
	{
		DevCon.WriteLn(
			Color_Gray,
			"R3000 Debugger: Branch to 0x890 (SYSMEM). Clearing modules.");
		R3000SymbolGuardian.ClearIrxModules();
	}

	// Override the memory size argument to IOPBOOT
	if (
#ifdef VITASX2_VITA
		!s_vitaIopRecompilerFallbackActive &&
#endif
		tar == 0xbfc4a000)
	{
		psxRegs.GPR.n.a0 = Ps2MemSize::ExposedIopRam >> 20;
	}

#ifdef VITASX2_VITA
	if (s_vitaIopRecompilerFallbackActive)
	{
		branch2 = true;
		iopIsDelaySlot = false;
		if (s_vitaIopRecompilerFallbackInstructions >=
			s_vitaIopRecompilerFallbackInstructionCount)
		{
			s_vitaIopRecompilerFallbackExceptionHandled = true;
			return;
		}

		const u32 first_delay_op = s_vitaIopRecompilerFallbackOpcodes
			[s_vitaIopRecompilerFallbackInstructions];
		if (VitaIopRecompilerOpcodeIsBranch(first_delay_op))
		{
			// x86 compiles nested delay-slot branches recursively. Their first
			// emitted terminal tail is the only dynamically reachable one, so
			// the outer target/event tail must not overwrite it. A not-taken
			// nested conditional still owns and executes its sequential delay.
			while (s_vitaIopRecompilerFallbackInstructions <
				   s_vitaIopRecompilerFallbackInstructionCount)
			{
				const u32 index = s_vitaIopRecompilerFallbackInstructions;
				const u32 delay_pc = psxRegs.pc;
				const u32 delay_op = s_vitaIopRecompilerFallbackOpcodes[index];
				execI();
				if (s_vitaIopRecompilerFallbackIrxHandled ||
					s_vitaIopRecompilerFallbackExceptionHandled ||
					s_vitaIopRecompilerFallbackUnalignedTarget ||
					s_vitaIopRecompilerFallbackBreakpoint)
				{
					return;
				}
				if (!VitaIopRecompilerOpcodeIsBranch(delay_op))
				{
					if (VitaIopRecompilerInstructionRedirected(delay_op, delay_pc))
						s_vitaIopRecompilerFallbackExceptionHandled = true;
					else if (VitaIopRecompilerOpcodeIsException(delay_op))
						psxRegs.pc = delay_pc + 4;
					return;
				}
				if (s_vitaIopRecompilerFallbackInstructions > index + 1)
					return;
			}
			return;
		}

		const u32 delay_pc = psxRegs.pc;
		execI();
		PSXCPU_LOG("\n");
		iopIsDelaySlot = false;
		if (s_vitaIopRecompilerFallbackIrxHandled ||
			s_vitaIopRecompilerFallbackBreakpoint)
			return;
		if (VitaIopRecompilerInstructionRedirected(first_delay_op, delay_pc))
		{
			s_vitaIopRecompilerFallbackExceptionHandled = true;
			return;
		}
		psxRegs.pc = tar;
		if ((tar & 3u) != 0)
			s_vitaIopRecompilerFallbackUnalignedTarget = true;
		return;
	}
#endif

	branch2 = iopIsDelaySlot = true;
	branchPC = tar;
	execI();
	PSXCPU_LOG( "\n" );
	iopIsDelaySlot = false;
	psxRegs.pc = branchPC;

#ifdef VITASX2_VITA
	if (!s_vitaIopRecompilerFallbackActive)
#endif
	iopEventTest();
}

void psxDoJump(u32 tar)
{
	// check for iop module import table magic
	u32 delayslot =
#ifdef VITASX2_VITA
		(s_vitaIopRecompilerFallbackActive &&
			s_vitaIopRecompilerFallbackInstructions <
				s_vitaIopRecompilerFallbackInstructionCount) ?
			s_vitaIopRecompilerFallbackOpcodes
				[s_vitaIopRecompilerFallbackInstructions] :
#endif
			iopMemRead32(psxRegs.pc);
	if (delayslot >> 16 == 0x2400)
	{
#ifdef VITASX2_VITA
		if (s_vitaIopRecompilerFallbackActive)
		{
			// x86/iR3000A.cpp::psxRecompileIrxImport() materializes the
			// delay-marker code and post-delay PC before invoking the HLE.
			// The ordinary interpreter invokes it one instruction earlier.
			// Reproduce the recompiler-visible state for this exact fallback.
			const u32 marker_pc = psxRegs.pc;
			const u32 jump_code = psxRegs.code;
			const u32 import_table = irxImportTableAddr(marker_pc);
			psxRegs.code = delayslot;
			psxRegs.pc = marker_pc + 4;
			if (irxImportExec(import_table, delayslot & 0xffff))
			{
				s_vitaIopRecompilerFallbackIrxHandled = true;
		return;
			}
			psxRegs.code = jump_code;
			psxRegs.pc = marker_pc;
		}
		else
#endif
			if (irxImportExec(irxImportTableAddr(psxRegs.pc), delayslot & 0xffff))
			return;
	}

	psxDoBranch(tar);
}

static void doBranch(s32 tar) { psxDoBranch(static_cast<u32>(tar)); }

static void intReserve() {}

static void intAlloc() {}

static void intReset() { intAlloc(); }

static s32 intExecuteBlock( s32 eeCycles )
{
	psxRegs.iopBreak = 0;
	psxRegs.iopCycleEE = eeCycles;
	u64 lastIOPCycle = 0;

	while (psxRegs.iopCycleEE > 0)
	{
		lastIOPCycle = psxRegs.cycle;
		if ((psxHu32(HW_ICFG) & 8) && ((psxRegs.pc & 0x1fffffffU) == 0xa0 ||
										  (psxRegs.pc & 0x1fffffffU) == 0xb0 ||
										  (psxRegs.pc & 0x1fffffffU) == 0xc0))
			psxBiosCall();

		branch2 = 0;
		while (!branch2)
			execI();

		if ((psxHu32(HW_ICFG) & (1 << 3)))
		{
			// F = gcd(PS2CLK, PSXCLK) = 230400
			const u32 cnum = 1280; // PS2CLK / F
			const u32 cdenom = 147; // PSXCLK / F

			//One of the Iop to EE delta clocks to be set in PS1 mode.
			const u32 t =
				((cnum * (psxRegs.cycle - lastIOPCycle)) + psxRegs.iopCycleEECarry);
			psxRegs.iopCycleEE -= t / cdenom;
			psxRegs.iopCycleEECarry = t % cdenom;
		}
		else
		{ 
			//default ps2 mode value
			psxRegs.iopCycleEE -= (psxRegs.cycle - lastIOPCycle) * 8;
		}
	}

	return psxRegs.iopBreak + psxRegs.iopCycleEE;
}

static void intClear(u32 Addr, u32 Size) {
}

static void intShutdown() {
}

#ifdef VITASX2_VITA
VitaIopInterpreterRecompilerExit VitaExecuteIopInterpreterRecompilerBlock(
	const u32* opcodes, u32 instruction_count, u32* executed_instruction_count)
{
	if (executed_instruction_count)
		*executed_instruction_count = 0;
	if (!opcodes || instruction_count == 0 || !executed_instruction_count ||
		s_vitaIopRecompilerFallbackActive)
	{
		return VitaIopInterpreterRecompilerExit::Failed;
	}

	branch2 = false;
	iopIsDelaySlot = false;
	s_vitaIopRecompilerFallbackIrxHandled = false;
	s_vitaIopRecompilerFallbackExceptionHandled = false;
	s_vitaIopRecompilerFallbackUnalignedTarget = false;
	s_vitaIopRecompilerFallbackBreakpoint = false;
	s_vitaIopRecompilerFallbackInstructions = 0;
	s_vitaIopRecompilerFallbackInstructionCount = instruction_count;
	s_vitaIopRecompilerFallbackOpcodes = opcodes;
	s_vitaIopRecompilerRegisterJumpTargetValid = false;
	s_vitaIopRecompilerConstGprs = 1;
	s_vitaIopRecompilerCompileBranch = false;
	s_vitaIopRecompilerDelayPending = false;
	s_vitaIopRecompilerFallbackActive = true;

	bool failed = false;
	while (s_vitaIopRecompilerFallbackInstructions < instruction_count &&
		!s_vitaIopRecompilerFallbackIrxHandled &&
		!s_vitaIopRecompilerFallbackExceptionHandled &&
		!s_vitaIopRecompilerFallbackUnalignedTarget &&
		!s_vitaIopRecompilerFallbackBreakpoint)
	{
		const u32 before = s_vitaIopRecompilerFallbackInstructions;
		const u32 before_pc = psxRegs.pc;
		const u32 op = opcodes[before];
		execI();
		if ((!s_vitaIopRecompilerFallbackBreakpoint &&
				s_vitaIopRecompilerFallbackInstructions <= before) ||
			s_vitaIopRecompilerFallbackInstructions > instruction_count)
		{
			failed = true;
			break;
		}
		if (!s_vitaIopRecompilerFallbackIrxHandled &&
			!s_vitaIopRecompilerFallbackExceptionHandled &&
			!s_vitaIopRecompilerFallbackUnalignedTarget &&
			!VitaIopRecompilerOpcodeIsBranch(op))
		{
			if (VitaIopRecompilerInstructionRedirected(op, before_pc))
				s_vitaIopRecompilerFallbackExceptionHandled = true;
			else if (VitaIopRecompilerOpcodeIsException(op))
				psxRegs.pc = before_pc + 4;
		}
	}

	*executed_instruction_count = s_vitaIopRecompilerFallbackInstructions;
	s_vitaIopRecompilerFallbackActive = false;
	s_vitaIopRecompilerFallbackInstructionCount = 0;
	s_vitaIopRecompilerFallbackOpcodes = nullptr;
	s_vitaIopRecompilerRegisterJumpTargetValid = false;
	s_vitaIopRecompilerConstGprs = 1;
	s_vitaIopRecompilerCompileBranch = false;
	s_vitaIopRecompilerDelayPending = false;
	iopIsDelaySlot = false;
	if (failed)
		return VitaIopInterpreterRecompilerExit::Failed;
	if (s_vitaIopRecompilerFallbackIrxHandled)
		return VitaIopInterpreterRecompilerExit::IrxHandled;
	if (s_vitaIopRecompilerFallbackUnalignedTarget)
		return VitaIopInterpreterRecompilerExit::UnalignedTarget;
	if (s_vitaIopRecompilerFallbackBreakpoint)
		return VitaIopInterpreterRecompilerExit::Breakpoint;
	if (s_vitaIopRecompilerFallbackExceptionHandled)
		return VitaIopInterpreterRecompilerExit::ExceptionHandled;
	return s_vitaIopRecompilerFallbackInstructions == instruction_count ?
		VitaIopInterpreterRecompilerExit::Completed : VitaIopInterpreterRecompilerExit::Failed;
}
#endif

R3000Acpu psxInt = {
	intReserve,
	intReset,
	intExecuteBlock,
	intClear,
	intShutdown
};
