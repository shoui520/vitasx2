// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#include "pcsx2/vita/VitaIopBlockCompiler.h"

#include "common/Vita/VitaJitMemory.h"
#include "pcsx2/Config.h"
#include "pcsx2/IopDma.h"
#include "pcsx2/IopGte.h"
#include "pcsx2/IopHw.h"
#include "pcsx2/IopMem.h"
#include "pcsx2/R3000A.h"
#include "pcsx2/R5900.h"
#include "pcsx2/vita/VitaCore.h"

#include <algorithm>
#include <new>

#if defined(VITASX2_QEMU_VALIDATION)
u32 g_qemuIopDivSignedHelperCalls = 0;
u32 g_qemuIopDivUnsignedHelperCalls = 0;
u32 g_qemuIopKnownRamScalarLoadFastPaths = 0;
u32 g_qemuIopKnownRamScalarStoreFastPaths = 0;
u32 g_qemuIopKnownRamUnalignedLoadFastPaths = 0;
u32 g_qemuIopKnownRamUnalignedStoreFastPaths = 0;
u32 g_qemuIopConstRamScalarLoadFastPaths = 0;
u32 g_qemuIopConstRamScalarStoreFastPaths = 0;
u32 g_qemuIopConstRamUnalignedLoadFastPaths = 0;
u32 g_qemuIopConstRamUnalignedStoreFastPaths = 0;
u32 g_qemuIopConstRamCop2LoadFastPaths = 0;
u32 g_qemuIopConstRamCop2StoreFastPaths = 0;
u32 g_qemuIopConstBranchCompareFastPaths = 0;
u32 g_qemuIopConstSignedBranchFastPaths = 0;
u32 g_qemuIopConstStoreValueFastPaths = 0;
u32 g_qemuIopConstStaticBranchTailFastPaths = 0;
u32 g_qemuIopConstImmediateFastPaths = 0;
u32 g_qemuIopConstRegisterOpFastPaths = 0;
u32 g_qemuIopConstShiftFastPaths = 0;
u32 g_qemuIopConstShiftAmountFastPaths = 0;
u32 g_qemuIopConstMultiplyFastPaths = 0;
u32 g_qemuIopConstDivideFastPaths = 0;
u32 g_qemuIopConstHiLoReadFastPaths = 0;
u32 g_qemuIopConstHiLoWriteFastPaths = 0;
u32 g_qemuIopConstRegisterOperandFastPaths = 0;
u32 g_qemuIopConstMultiplyOperandFastPaths = 0;
u32 g_qemuIopConstDivideOperandFastPaths = 0;
u32 g_qemuIopConstRegisterJumpFastPaths = 0;
u32 g_qemuIopConstCop0WriteFastPaths = 0;
u32 g_qemuIopConstCop2WriteFastPaths = 0;
static bool s_qemuIopTrustedSourceAuditEnabled = true;
static bool s_qemuIopPinnedGprResidencyEnabled = true;
static bool s_qemuIopClockModeSpecializationEnabled = true;
static bool s_qemuIopSavedRegisterNarrowingEnabled = true;
static bool s_qemuIopBlockCycleBatchingEnabled = true;
#endif

namespace
{
	using GeneratedBlock = u32 (*)();

	constexpr u16 REG_R4 = 1u << 4;
	constexpr u16 REG_R5 = 1u << 5;
	constexpr u16 REG_R6 = 1u << 6;
	constexpr u16 REG_R7 = 1u << 7;
	constexpr u16 REG_R8 = 1u << 8;
	constexpr u16 REG_R10 = 1u << 10;
	constexpr u16 REG_R11 = 1u << 11;
	constexpr u16 REG_LR = 1u << 14;
	constexpr u16 REG_PC = 1u << 15;
	constexpr unsigned HOST_SP = 13;

	constexpr unsigned HOST_TMP0 = 0;
	constexpr unsigned HOST_TMP1 = 1;
	constexpr unsigned HOST_TMP2 = 2;
	constexpr unsigned HOST_TMP3 = 3;
	constexpr unsigned HOST_PSX_REGS = 4;
	constexpr unsigned HOST_SAVED0 = 5;
	constexpr unsigned HOST_SAVED1 = 6;
	constexpr unsigned HOST_BRANCH_FLAG = 7;
	constexpr unsigned HOST_REGISTER_JUMP_TARGET = 8;
	// r10 is also HOST_IOP_RAM_MASK; the cycle base is only allocated in
	// non-trace blocks that do not reserve the direct IOP RAM fast-path pair.
	constexpr unsigned HOST_CYCLE_BASE = 10;
	constexpr unsigned HOST_IOP_RAM_MASK = 10;
	constexpr unsigned HOST_IOP_RAM_BASE = 11;
	constexpr unsigned HOST_CALL_SCRATCH = 12;

	constexpr u32 IOP_BRANCH_TARGET_ZERO = 0x00000000u;
	constexpr u32 IOP_BRANCH_TARGET_SYSMEM = 0x00000890u;
	constexpr u32 IOP_BRANCH_TARGET_IOPBOOT = 0xbfc4a000u;

	constexpr size_t GPR_OFFSET = offsetof(psxRegisters, GPR);
	constexpr size_t HI_OFFSET = GPR_OFFSET + offsetof(GPRRegs, n.hi);
	constexpr size_t LO_OFFSET = GPR_OFFSET + offsetof(GPRRegs, n.lo);
	constexpr size_t CP0_OFFSET = offsetof(psxRegisters, CP0);
	constexpr size_t CP0_STATUS_OFFSET = CP0_OFFSET + offsetof(CP0Regs, n.Status);
	constexpr size_t CP2D_OFFSET = offsetof(psxRegisters, CP2D);
	constexpr size_t CP2C_OFFSET = offsetof(psxRegisters, CP2C);
	constexpr size_t PC_OFFSET = offsetof(psxRegisters, pc);
	constexpr size_t CODE_OFFSET = offsetof(psxRegisters, code);
	constexpr size_t CYCLE_OFFSET = offsetof(psxRegisters, cycle);
	constexpr size_t INTERRUPT_OFFSET = offsetof(psxRegisters, interrupt);
	constexpr size_t IOP_NEXT_EVENT_CYCLE_OFFSET = offsetof(psxRegisters, iopNextEventCycle);
	constexpr size_t IOP_NEXT_EVENT_CYCLE_FROM_CYCLE_OFFSET = IOP_NEXT_EVENT_CYCLE_OFFSET - CYCLE_OFFSET;
	constexpr size_t IOP_CYCLE_EE_OFFSET = offsetof(psxRegisters, iopCycleEE);
	constexpr size_t IOP_CYCLE_EE_CARRY_OFFSET = offsetof(psxRegisters, iopCycleEECarry);
	constexpr u32 IOP_WAIT_CYCLES = 384;

	constexpr unsigned RS(u32 op)
	{
		return (op >> 21) & 0x1f;
	}

	constexpr unsigned RT(u32 op)
	{
		return (op >> 16) & 0x1f;
	}

	constexpr unsigned RD(u32 op)
	{
		return (op >> 11) & 0x1f;
	}

	constexpr unsigned SA(u32 op)
	{
		return (op >> 6) & 0x1f;
	}

	constexpr s16 IMM_S(u32 op)
	{
		return static_cast<s16>(op);
	}

	constexpr u16 IMM_U(u32 op)
	{
		return static_cast<u16>(op);
	}

	constexpr u32 BranchTarget(u32 pc, u32 op)
	{
		return pc + 4 + static_cast<u32>(static_cast<s32>(IMM_S(op)) * 4);
	}

	constexpr u32 JumpTarget(u32 pc, u32 op)
	{
		return ((pc + 4) & 0xf0000000u) | ((op & 0x03ffffffu) << 2);
	}

	bool IsIopWaitLoopShape(u32 start_pc, u32 instruction_count)
	{
		if (instruction_count < 2)
			return false;

		const u32 branch_index = instruction_count - 2;
		const u32 branch_pc = start_pc + branch_index * 4;
		const u32 branch_op = iopMemRead32(branch_pc);
		for (u32 i = 0; i < instruction_count; i++)
		{
			if (i != branch_index && iopMemRead32(start_pc + i * 4) != 0)
				return false;
		}

		const u32 primary = branch_op >> 26;
		if (primary == 0x02 || primary == 0x03)
			return JumpTarget(branch_pc, branch_op) == start_pc;
		if (primary >= 0x04 && primary <= 0x07)
			return BranchTarget(branch_pc, branch_op) == start_pc;
		if (primary != 0x01)
			return false;

		const u32 rt = RT(branch_op);
		return (rt == 0x00 || rt == 0x01 || rt == 0x10 || rt == 0x11) &&
			BranchTarget(branch_pc, branch_op) == start_pc;
	}

	constexpr size_t GprOffset(unsigned guest_reg)
	{
		return GPR_OFFSET + guest_reg * sizeof(u32);
	}

	void ComputeIopMultiplyResult(u32 lhs, u32 rhs, bool is_signed, u32* lo, u32* hi)
	{
		// PCSX2 owners: R3000AOpcodeTables.cpp::psxMULT()/psxMULTU().
		const u64 result = is_signed ?
			static_cast<u64>(static_cast<s64>(static_cast<s32>(lhs)) *
							 static_cast<s64>(static_cast<s32>(rhs))) :
			(static_cast<u64>(lhs) * static_cast<u64>(rhs));
		*lo = static_cast<u32>(result);
		*hi = static_cast<u32>(result >> 32);
	}

	void ComputeIopDivideResult(u32 lhs, u32 rhs, bool is_signed, u32* lo, u32* hi)
	{
		// PCSX2 owners: R3000AOpcodeTables.cpp::psxDIV()/psxDIVU().
		if (is_signed)
		{
			const s32 numerator = static_cast<s32>(lhs);
			const s32 denominator = static_cast<s32>(rhs);
			if (denominator == 0)
			{
				*lo = (numerator < 0) ? 1u : 0xffffffffu;
				*hi = lhs;
			}
			else if (lhs == 0x80000000u && rhs == 0xffffffffu)
			{
				*lo = 0x80000000u;
				*hi = 0;
			}
			else
			{
				*lo = static_cast<u32>(numerator / denominator);
				*hi = static_cast<u32>(numerator % denominator);
			}
			return;
		}

		if (rhs == 0)
		{
			*lo = 0xffffffffu;
			*hi = lhs;
		}
		else
		{
			*lo = lhs / rhs;
			*hi = lhs % rhs;
		}
	}

	bool IsPowerOfTwo(u32 value)
	{
		return value != 0 && (value & (value - 1)) == 0;
	}

	bool IsIopSpecialBranchTarget(u32 target)
	{
		return target == IOP_BRANCH_TARGET_ZERO ||
			   target == IOP_BRANCH_TARGET_SYSMEM ||
			   target == IOP_BRANCH_TARGET_IOPBOOT;
	}

	unsigned PowerOfTwoShift(u32 value)
	{
		unsigned shift = 0;
		while ((value & 1) == 0)
		{
			value >>= 1;
			shift++;
		}
		return shift;
	}

	constexpr size_t Cp0Offset(unsigned cop0_reg)
	{
		return CP0_OFFSET + cop0_reg * sizeof(u32);
	}

	constexpr size_t Cp2dOffset(unsigned cop2_reg)
	{
		return CP2D_OFFSET + cop2_reg * sizeof(u32);
	}

	constexpr size_t Cp2cOffset(unsigned cop2_reg)
	{
		return CP2C_OFFSET + cop2_reg * sizeof(u32);
	}

	constexpr bool IsNativeCop0Opcode(u32 op)
	{
		switch (RS(op))
		{
			case 0x00: // MFC0
			case 0x02: // CFC0
			case 0x04: // MTC0
			case 0x06: // CTC0
			case 0x10: // RFE
				return true;
			default:
				return false;
		}
	}

	constexpr bool IsNativeCop2Opcode(u32 op)
	{
		if ((op & 0x3f) == 0)
		{
			switch (RS(op))
			{
				case 0x00: // MFC2
				case 0x02: // CFC2
				case 0x04: // MTC2
				case 0x06: // CTC2
					return true;
				default:
					return false;
			}
		}

		switch (op & 0x3f)
		{
			case 0x01: // RTPS
			case 0x06: // NCLIP
			case 0x0c: // OP
			case 0x10: // DPCS
			case 0x11: // INTPL
			case 0x12: // MVMVA
			case 0x13: // NCDS
			case 0x14: // CDP
			case 0x16: // NCDT
			case 0x1b: // NCCS
			case 0x1c: // CC
			case 0x1e: // NCS
			case 0x20: // NCT
			case 0x28: // SQR
			case 0x29: // DCPL
			case 0x2a: // DPCT
			case 0x2d: // AVSZ3
			case 0x2e: // AVSZ4
			case 0x30: // RTPT
			case 0x3d: // GPF
			case 0x3e: // GPL
			case 0x3f: // NCCT
				return true;
			default:
				return false;
		}
	}

	constexpr bool IsNativeRegimmOpcode(u32 op)
	{
		switch (RT(op))
		{
			case 0x00: // BLTZ
			case 0x01: // BGEZ
			case 0x10: // BLTZAL
			case 0x11: // BGEZAL
				return true;
			default:
				return false;
		}
	}

	constexpr bool IsNativeSpecialOpcode(u32 op)
	{
		switch (op & 0x3f)
		{
			case 0x00: // SLL
			case 0x02: // SRL
			case 0x03: // SRA
			case 0x04: // SLLV
			case 0x06: // SRLV
			case 0x07: // SRAV
			case 0x08: // JR
			case 0x09: // JALR
			case 0x0c: // SYSCALL
			case 0x0d: // BREAK
			case 0x10: // MFHI
			case 0x11: // MTHI
			case 0x12: // MFLO
			case 0x13: // MTLO
			case 0x18: // MULT
			case 0x19: // MULTU
			case 0x1a: // DIV
			case 0x1b: // DIVU
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
				return true;
			default:
				return false;
		}
	}

	constexpr bool IsNativeOpcode(u32 op)
	{
		switch (op >> 26)
		{
			case 0x00: // SPECIAL
				return IsNativeSpecialOpcode(op);
			case 0x01: // REGIMM
				return IsNativeRegimmOpcode(op);
			case 0x02: // J
			case 0x03: // JAL
			case 0x04: // BEQ
			case 0x05: // BNE
			case 0x06: // BLEZ
			case 0x07: // BGTZ
			case 0x08: // ADDI
			case 0x09: // ADDIU
			case 0x0a: // SLTI
			case 0x0b: // SLTIU
			case 0x0c: // ANDI
			case 0x0d: // ORI
			case 0x0e: // XORI
			case 0x0f: // LUI
				return true;
			case 0x10: // COP0
				return IsNativeCop0Opcode(op);
			case 0x12: // COP2
				return IsNativeCop2Opcode(op);
			case 0x20: // LB
			case 0x21: // LH
			case 0x22: // LWL
			case 0x23: // LW
			case 0x24: // LBU
			case 0x25: // LHU
			case 0x26: // LWR
			case 0x28: // SB
			case 0x29: // SH
			case 0x2a: // SWL
			case 0x2b: // SW
			case 0x2e: // SWR
			case 0x32: // LWC2
			case 0x3a: // SWC2
				return true;
			default:
				return false;
		}
	}

	constexpr bool UsesDirectIopRamFastPath(u32 op)
	{
		switch (op >> 26)
		{
			case 0x20: // LB
			case 0x21: // LH
			case 0x23: // LW
			case 0x24: // LBU
			case 0x25: // LHU
				return RT(op) != 0;
			case 0x22: // LWL
			case 0x26: // LWR
			case 0x28: // SB
			case 0x29: // SH
			case 0x2a: // SWL
			case 0x2b: // SW
			case 0x2e: // SWR
			case 0x32: // LWC2
			case 0x3a: // SWC2
				return true;
			default:
				return false;
		}
	}

	constexpr bool IsIopBranchOrJumpOpcode(u32 op)
	{
		switch (op >> 26)
		{
			case 0x00: // SPECIAL
				return (op & 0x3f) == 0x08 || (op & 0x3f) == 0x09; // JR/JALR
			case 0x01: // REGIMM
				switch (RT(op))
				{
					case 0x00: // BLTZ
					case 0x01: // BGEZ
					case 0x10: // BLTZAL
					case 0x11: // BGEZAL
						return true;
					default:
						return false;
				}
			case 0x02: // J
			case 0x03: // JAL
			case 0x04: // BEQ
			case 0x05: // BNE
			case 0x06: // BLEZ
			case 0x07: // BGTZ
				return true;
			default:
				return false;
		}
	}

	constexpr bool IsIopExceptionOpcode(u32 op)
	{
		if ((op >> 26) != 0x00)
			return false;

		const u32 function = op & 0x3f;
		return function == 0x0c || function == 0x0d; // SYSCALL/BREAK
	}

	constexpr bool IsIopCop2CommandOpcode(u32 op)
	{
		return (op >> 26) == 0x12 && (op & 0x3f) != 0;
	}

	constexpr bool IopInstructionRequiresCodeState(u32 op)
	{
		// PCSX2 owner: x86/iR3000A.cpp only writes psxRegs.code for paths that
		// enter opcode-driven helpers, notably rpsxSYSCALL()/rpsxBREAK() and
		// COP2 GTE command calls. Native A32 templates decode other fields from
		// the compile-time opcode and do not need the architectural code slot.
		return IsIopExceptionOpcode(op) || IsIopCop2CommandOpcode(op);
	}

	constexpr bool IopInstructionRequiresPcState(u32 op)
	{
		// PCSX2 owner: x86/iR3000A.cpp keeps psxpc as compile-time state and
		// stores psxRegs.pc around helper/exit paths. Vita keeps the same rule
		// conservatively for branches, exceptions, event tests, and memory
		// operations that can reach helper-backed cold tails.
		switch (op >> 26)
		{
			case 0x00: // SPECIAL
				return (op & 0x3f) == 0x08 || (op & 0x3f) == 0x09 || // JR/JALR
					   (op & 0x3f) == 0x0c || (op & 0x3f) == 0x0d; // SYSCALL/BREAK
			case 0x01: // REGIMM
			case 0x02: // J
			case 0x03: // JAL
			case 0x04: // BEQ
			case 0x05: // BNE
			case 0x06: // BLEZ
			case 0x07: // BGTZ
				return true;
			case 0x10: // COP0
				return RS(op) == 0x10; // RFE calls iopTestIntc().
			case 0x20: // LB
			case 0x21: // LH
			case 0x22: // LWL
			case 0x23: // LW
			case 0x24: // LBU
			case 0x25: // LHU
			case 0x26: // LWR
			case 0x28: // SB
			case 0x29: // SH
			case 0x2a: // SWL
			case 0x2b: // SW
			case 0x2e: // SWR
			case 0x32: // LWC2
			case 0x3a: // SWC2
				return true;
			default:
				return false;
		}
	}

	constexpr bool IopInstructionCanDeferCycleState(u32 op)
	{
		// PCSX2 owner: x86/iR3000A.cpp batches s_psxBlockCycles and commits them
		// in iPsxBranchTest(). Keep this per-instruction predicate to opcodes
		// whose Vita A32 templates cannot call helpers, touch memory handlers,
		// redirect control flow, or read timing.
		switch (op >> 26)
		{
			case 0x00: // SPECIAL
				switch (op & 0x3f)
				{
					case 0x00: // SLL
					case 0x02: // SRL
					case 0x03: // SRA
					case 0x04: // SLLV
					case 0x06: // SRLV
					case 0x07: // SRAV
					case 0x10: // MFHI
					case 0x11: // MTHI
					case 0x12: // MFLO
					case 0x13: // MTLO
					case 0x18: // MULT
					case 0x19: // MULTU
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
						return true;
					default:
						return false;
				}

			case 0x08: // ADDI
			case 0x09: // ADDIU
			case 0x0a: // SLTI
			case 0x0b: // SLTIU
			case 0x0c: // ANDI
			case 0x0d: // ORI
			case 0x0e: // XORI
			case 0x0f: // LUI
				return true;

			default:
				return false;
		}
	}

	constexpr bool IsIopStaticConditionalBranchOpcode(u32 op)
	{
		switch (op >> 26)
		{
			case 0x01: // REGIMM
				return IsNativeRegimmOpcode(op);
			case 0x04: // BEQ
			case 0x05: // BNE
			case 0x06: // BLEZ
			case 0x07: // BGTZ
				return true;
			default:
				return false;
		}
	}

	constexpr bool IsIopStaticJumpOpcode(u32 op)
	{
		return (op >> 26) == 0x02 || (op >> 26) == 0x03; // J/JAL
	}

	constexpr bool IsIopRegisterJumpOpcode(u32 op)
	{
		return (op >> 26) == 0x00 && ((op & 0x3f) == 0x08 || (op & 0x3f) == 0x09); // JR/JALR
	}

	bool IopBlockCanDeferCycleUpdates(u32 start_pc, u32 instruction_count)
	{
		for (u32 i = 0; i < instruction_count; i++)
		{
			const u32 pc = start_pc + i * 4;
			const u32 op = iopMemRead32(pc);
			if (!IsNativeOpcode(op))
				return false;

			if (IopInstructionCanDeferCycleState(op))
				continue;

			const u32 delay_op = (i + 1 < instruction_count) ? iopMemRead32(pc + 4) : 0;
			const bool final_branch_pair = (i + 2 == instruction_count) &&
										   IopInstructionCanDeferCycleState(delay_op) &&
										   !IsIopBranchOrJumpOpcode(delay_op) &&
										   !IsIopExceptionOpcode(delay_op);
			if (IsIopStaticConditionalBranchOpcode(op) && final_branch_pair)
				continue;

			if (IsIopStaticJumpOpcode(op) && final_branch_pair &&
				((op >> 26) != 0x02 || (delay_op >> 16) != 0x2400))
			{
				continue;
			}

			return false;
		}

		return true;
	}

	bool IopBlockCanBatchCycleUpdates(u32 start_pc, u32 instruction_count)
	{
		// PCSX2's R3000A recompiler accumulates s_psxBlockCycles across native
		// ALU, memory, COP and helper-backed operations, then publishes it at the
		// actual branch/block exit. Vita retains per-instruction publication only
		// where the existing helper owns a non-uniform instruction path.
		for (u32 i = 0; i < instruction_count; i++)
		{
			const u32 op = iopMemRead32(start_pc + i * 4);
			if (!IsNativeOpcode(op) || IsIopExceptionOpcode(op))
				return false;
			// iopTestIntc() schedules from the architecturally current cycle.
			// Keep RFE on the dynamic path until a prefix-publication contract
			// handles both its IOP-running and EE-running interrupt arms.
			if ((op >> 26) == 0x10 && RS(op) == 0x10)
				return false;

			if (IsIopBranchOrJumpOpcode(op) && i + 1 < instruction_count)
			{
				const u32 delay_op = iopMemRead32(start_pc + (i + 1) * 4);
				if (IsIopBranchOrJumpOpcode(delay_op) || IsIopExceptionOpcode(delay_op))
					return false;

				// psxDoJump() may consume an import-dispatch delay word without
				// executing it, so its dynamic cycle delta remains the owner.
				if ((op >> 26) == 0x02 && (delay_op >> 16) == 0x2400)
					return false;
			}
		}

		return true;
	}

	u8 DirectIopRamAlignmentMask(u32 op)
	{
		switch (op >> 26)
		{
			case 0x20: // LB
			case 0x22: // LWL
			case 0x24: // LBU
			case 0x26: // LWR
			case 0x28: // SB
			case 0x2a: // SWL
			case 0x2e: // SWR
				return 0;
			case 0x21: // LH
			case 0x25: // LHU
			case 0x29: // SH
				return 1;
			case 0x23: // LW
			case 0x2b: // SW
			case 0x32: // LWC2
			case 0x3a: // SWC2
				return 3;
			default:
				return 0xff;
		}
	}

	bool CanEmitKnownDirectIopRamFastPath(u32 op)
	{
		switch (op >> 26)
		{
			case 0x20: // LB
			case 0x21: // LH
			case 0x22: // LWL
			case 0x23: // LW
			case 0x24: // LBU
			case 0x25: // LHU
			case 0x26: // LWR
			case 0x28: // SB
			case 0x29: // SH
			case 0x2a: // SWL
			case 0x2b: // SW
			case 0x2e: // SWR
			case 0x32: // LWC2
			case 0x3a: // SWC2
				return true;
			default:
				return false;
		}
	}

	bool TryDirectIopRamEffectiveAddress(u32 effective_address, u8 alignment_mask, u32* address)
	{
		if (alignment_mask == 0xff || (effective_address & alignment_mask) != 0 ||
			(effective_address & 0x10000000u) != 0)
		{
			return false;
		}

		if (address)
			*address = effective_address & (Ps2MemSize::ExposedIopRam - 1);
		return true;
	}

	static_assert(PC_OFFSET <= 4095);
	static_assert(CODE_OFFSET <= 4095);
	static_assert(CYCLE_OFFSET + sizeof(u64) <= 4095);
	static_assert((CYCLE_OFFSET % alignof(u64)) == 0);
	static_assert(IOP_NEXT_EVENT_CYCLE_OFFSET > CYCLE_OFFSET);
	static_assert(IOP_NEXT_EVENT_CYCLE_FROM_CYCLE_OFFSET <= 0xff);
	static_assert((IOP_NEXT_EVENT_CYCLE_FROM_CYCLE_OFFSET % alignof(u64)) == 0);
	static_assert(IOP_CYCLE_EE_OFFSET <= 4095);
	static_assert(IOP_CYCLE_EE_CARRY_OFFSET <= 4095);
	static_assert(GprOffset(33) + sizeof(u32) <= 4095);
	static_assert(HI_OFFSET + sizeof(u32) <= 4095);
	static_assert(LO_OFFSET + sizeof(u32) <= 4095);
	static_assert(LO_OFFSET == HI_OFFSET + sizeof(u32));
	static_assert(HI_OFFSET <= 0xff);
	static_assert(Cp0Offset(31) + sizeof(u32) <= 4095);
	static_assert(CP0_STATUS_OFFSET + sizeof(u32) <= 4095);
	static_assert(Cp2dOffset(31) + sizeof(u32) <= 4095);
	static_assert(Cp2cOffset(31) + sizeof(u32) <= 4095);

	extern "C" __attribute__((noinline)) u32 VitaIopA32DirectExit()
	{
		return static_cast<u32>(VitaIOP::BlockExitKind::Direct);
	}

	extern "C" __attribute__((noinline)) bool VitaIopA32TraceInstruction(u32 pc, u32 opcode)
	{
		// PCSX2 owner: R3000AInterpreter.cpp::execI() stores psxRegs.code,
		// records DebugTools/IopTrace.cpp::RecordIopPreInstruction, and only
		// then advances pc/cycle. Keep generated A32 blocks at that hook point.
		if (!VitaRecordIopPreInstruction(pc, opcode))
			return false;

		psxRegs.iopCycleEE = 0;
		if (Cpu)
			Cpu->ExitExecution();
		return true;
	}

	extern "C" __attribute__((noinline)) u32 VitaIopA32FastForwardWaitLoop(
		u32 loop_pc, u32 block_cycles)
	{
		// PCSX2 owner: x86/iR3000A.cpp::iPsxBranchTest(). The wait-loop form
		// advances the IOP clock to the earlier of the EE timeslice budget and
		// the next IOP event, charges exactly that dynamic delta, and tests the
		// event only when budget remains. The SF-only minimum deliberately
		// mirrors xCMP/xCMOVNS rather than a C++ unsigned comparison.
		const u64 old_cycle = psxRegs.cycle;
		const u32 budget = static_cast<u32>(psxRegs.iopCycleEE);
		const u64 budget_target = old_cycle + ((static_cast<u64>(budget) + 7) >> 3);
		const u64 next_event = psxRegs.iopNextEventCycle;
		const u64 candidate_minus_event = budget_target - next_event;
		const u64 target_cycle = (candidate_minus_event & (1ull << 63)) == 0 ?
			next_event : budget_target;

		psxRegs.pc = loop_pc;
		psxRegs.cycle = target_cycle;
		const u64 iop_cycles = target_cycle - old_cycle;
		const u32 ee_cycles = static_cast<u32>(iop_cycles << 3);
		if ((psxHu32(HW_ICFG) & (1u << 3)) == 0)
		{
			psxRegs.iopCycleEE = static_cast<s32>(
				static_cast<u32>(psxRegs.iopCycleEE) - ee_cycles);
		}
		else
		{
			// Dynamic iPsxAddEECycles(0xffffffff) receives delta << 3 in EAX.
			const u32 numerator = ee_cycles + psxRegs.iopCycleEECarry;
			psxRegs.iopCycleEECarry = numerator % 147u;
			psxRegs.iopCycleEE = static_cast<s32>(
				static_cast<u32>(psxRegs.iopCycleEE) - (numerator / 147u));
		}

		VitaRecordA32IopWaitLoopFastForward(iop_cycles, block_cycles);
		if (psxRegs.iopCycleEE > 0)
			iopEventTest();

		return static_cast<u32>(VitaIOP::BlockExitKind::Direct);
	}

	extern "C" __attribute__((noinline)) void VitaIopA32RaiseException(u32 pc, u32 code)
	{
		// PCSX2 owner: R3000AOpcodeTables.cpp::psxSYSCALL()/psxBREAK()
		// subtract the pre-incremented pc before entering R3000A.cpp::psxException().
		psxRegs.pc = pc;
		psxException(code, iopIsDelaySlot);
	}

	extern "C" __attribute__((noinline)) u64 VitaIopA32DivResult(u32 rs_value, u32 rt_value)
	{
		// PCSX2 owner: R3000AOpcodeTables.cpp::psxDIV().
#if defined(VITASX2_QEMU_VALIDATION)
		++g_qemuIopDivSignedHelperCalls;
#endif
		u32 lo = 0;
		u32 hi = 0;
		if (rt_value == 0)
		{
			lo = (static_cast<s32>(rs_value) < 0) ? 1 : 0xffffffffu;
			hi = rs_value;
		}
		else if (rs_value == 0x80000000u && rt_value == 0xffffffffu)
		{
			lo = 0x80000000u;
			hi = 0;
		}
		else
		{
			lo = static_cast<u32>(static_cast<s32>(rs_value) / static_cast<s32>(rt_value));
			hi = static_cast<u32>(static_cast<s32>(rs_value) % static_cast<s32>(rt_value));
		}

		return (static_cast<u64>(hi) << 32) | lo;
	}

	extern "C" __attribute__((noinline)) u64 VitaIopA32DivuResult(u32 rs_value, u32 rt_value)
	{
		// PCSX2 owner: R3000AOpcodeTables.cpp::psxDIVU().
#if defined(VITASX2_QEMU_VALIDATION)
		++g_qemuIopDivUnsignedHelperCalls;
#endif
		u32 lo = 0;
		u32 hi = 0;
		if (rt_value == 0)
		{
			lo = 0xffffffffu;
			hi = rs_value;
		}
		else
		{
			lo = rs_value / rt_value;
			hi = rs_value % rt_value;
		}

		return (static_cast<u64>(hi) << 32) | lo;
	}

	bool DecodeExitKind(u32 value, VitaIOP::BlockExitKind* exit)
	{
		if (value == static_cast<u32>(VitaIOP::BlockExitKind::Direct))
		{
			*exit = VitaIOP::BlockExitKind::Direct;
			return true;
		}

		return false;
	}

	size_t AlignUp(size_t value, size_t alignment)
	{
		return (value + alignment - 1) & ~(alignment - 1);
	}

} // namespace

namespace VitaIOP
{
	BlockCompiler::BlockCompiler(VitaA32::CodeBuffer& code, const u16* ram_source_page_live_counts)
		: m_code(code)
		, m_ram_source_page_live_counts(ram_source_page_live_counts)
	{
	}

	bool BlockCompiler::CanCompileOpcode(u32 op)
	{
		// PCSX2 owner: R3000AOpcodeTables.cpp::psxBSC plus the SPECIAL/REGIMM/
		// COP0/COP2 subtables. Every non-psxNULL R3000A slot has a native A32
		// path below; psxNULL slots are rejected at scan time and executed by the
		// interpreter fallback instead of a generated helper tail.
		return IsNativeOpcode(op);
	}

	bool BlockCompiler::BeginBlock()
	{
		m_scalar_load_cold_tails.clear();
		m_scalar_store_cold_tails.clear();
		m_unaligned_read_cold_tails.clear();
		m_unaligned_write_cold_tails.clear();
		m_cop2_load_cold_tails.clear();
		m_cop2_store_cold_tails.clear();
		// Trace blocks call out before every cycle increment. Pure production
		// blocks batch cycles once at the tail. Runtime IOP RAM masking also
		// uses r10, but known direct-RAM blocks only need the r11 base pointer.
		m_iop_cycle_base_register_available =
			!m_iop_ram_mask_register_available && !m_emit_trace_checks && !m_defer_cycle_updates;
		const u16 baseline_saved_registers = REG_R4 | REG_R5 | REG_R6 | REG_R7 | REG_R8 |
			((m_iop_cycle_base_register_available || m_iop_ram_mask_register_available) ? REG_R10 : 0) |
			(m_iop_ram_registers_available ? REG_R11 : 0);
		bool narrow_saved_registers = true;
#if defined(VITASX2_QEMU_VALIDATION)
		narrow_saved_registers = s_qemuIopSavedRegisterNarrowingEnabled;
#endif
		m_saved_registers = narrow_saved_registers ? m_required_saved_registers : baseline_saved_registers;
		if ((m_saved_registers & ~baseline_saved_registers) != 0)
			return false;
		m_saved_register_stack_words_removed = narrow_saved_registers ?
			2u * (static_cast<u32>(__builtin_popcount(static_cast<unsigned>(baseline_saved_registers))) -
				static_cast<u32>(__builtin_popcount(static_cast<unsigned>(m_saved_registers)))) : 0;

		const u16 pushed_registers = m_saved_registers | REG_LR;
		const u32 pushed_count = static_cast<u32>(__builtin_popcount(static_cast<unsigned>(pushed_registers)));
		const u32 baseline_pushed_count = static_cast<u32>(
			__builtin_popcount(static_cast<unsigned>(baseline_saved_registers | REG_LR)));
		// PCSX2's iPsxAddEECycles(blockCycles) charges an analysis-proven block
		// directly. Timing-safe blocks need no data slot. Handler-capable blocks
		// retain one word for the greatest guest-cycle prefix already published.
		const bool needs_cycle_stack_word = !m_defer_cycle_updates || m_track_published_cycle_prefix;
		if (!needs_cycle_stack_word)
			m_stack_frame_size = (pushed_count & 1u) ? 4 : 0;
		else
			m_stack_frame_size = (pushed_count & 1u) ? 4 : 8;
		const u8 baseline_stack_frame_size = !needs_cycle_stack_word ?
			((baseline_pushed_count & 1u) ? 4 : 0) :
			((baseline_pushed_count & 1u) ? 4 : 8);
		m_saved_register_frame_instructions_added =
			(baseline_stack_frame_size == 0 && m_stack_frame_size != 0) ? 2 : 0;
		m_saved_register_frame_instructions_removed =
			(baseline_stack_frame_size != 0 && m_stack_frame_size == 0) ? 2 : 0;
		if (!m_code.EmitPush(m_saved_registers | REG_LR) ||
			(m_stack_frame_size != 0 && !m_code.EmitSubImm8(HOST_SP, HOST_SP, m_stack_frame_size)) ||
			!m_code.EmitMovImm32(HOST_PSX_REGS, static_cast<u32>(reinterpret_cast<uptr>(&psxRegs))))
		{
			return false;
		}
		if (m_track_published_cycle_prefix &&
			(!m_code.EmitMovImm8(HOST_TMP0, 0) ||
				!m_code.EmitStrImm12(HOST_TMP0, HOST_SP, 0)))
		{
			return false;
		}
		if (!m_defer_cycle_updates &&
			(!m_code.EmitLdrImm12(HOST_TMP0, HOST_PSX_REGS, CYCLE_OFFSET) ||
				!m_code.EmitStrImm12(HOST_TMP0, HOST_SP, 0)))
		{
			return false;
		}

		if (m_iop_cycle_base_register_available &&
			!m_code.EmitAddImm32(HOST_CYCLE_BASE, HOST_PSX_REGS, static_cast<u32>(CYCLE_OFFSET)))
		{
			return false;
		}

		if (m_iop_ram_mask_register_available &&
			!m_code.EmitMovImm32(HOST_IOP_RAM_MASK, Ps2MemSize::ExposedIopRam - 1))
		{
			return false;
		}

		if (m_iop_ram_registers_available &&
			!m_code.EmitMovImm32(HOST_IOP_RAM_BASE,
				static_cast<u32>(reinterpret_cast<uptr>(iopMem->Main))))
		{
			return false;
		}

		for (u8 i = 0; i < m_pinned_gpr_count; i++)
		{
			const PinnedGpr& pin = m_pinned_gprs[i];
			if (pin.needs_initial_load)
			{
				if (!m_code.EmitLdrImm12(pin.host, HOST_PSX_REGS,
						static_cast<u16>(GprOffset(pin.guest))))
				{
					return false;
				}
				m_pinned_gpr_initial_loads++;
			}
		}
		return true;
	}

	bool BlockCompiler::EndBlockReturn(
		BlockExitKind exit, bool charge_budget, bool flush_pins, u32 known_cycle_count)
	{
		if ((flush_pins && !EmitFlushPinnedGprs()) ||
			(charge_budget && !EmitChargeEeBudget(known_cycle_count)))
			return false;

		return m_code.EmitMovImm32(HOST_TMP0, static_cast<u32>(exit)) &&
			   (m_stack_frame_size == 0 || m_code.EmitAddImm8(HOST_SP, HOST_SP, m_stack_frame_size)) &&
			   m_code.EmitPop(m_saved_registers | REG_PC);
	}

	bool BlockCompiler::EndBlockDirectTail(const void* direct_exit, DirectLinkSlot* direct_link_slot)
	{
		if (!direct_exit)
			return false;

		if (!EmitFlushPinnedGprs() || !EmitChargeEeBudget() ||
			(m_stack_frame_size != 0 && !m_code.EmitAddImm8(HOST_SP, HOST_SP, m_stack_frame_size)) ||
			!m_code.EmitPop(m_saved_registers | REG_LR))
		{
			return false;
		}

		const size_t target_offset = m_code.Size();
		const size_t target_branch = m_code.EmitBranchPlaceholder();
		if (target_branch == static_cast<size_t>(-1))
			return false;

		const size_t fallback_offset = m_code.Size();
		if (!m_code.PatchBranch(target_branch, fallback_offset) ||
			!m_code.EmitMovImm32(HOST_CALL_SCRATCH, static_cast<u32>(reinterpret_cast<uptr>(direct_exit))) ||
			!m_code.EmitBx(HOST_CALL_SCRATCH))
		{
			return false;
		}

		if (direct_link_slot)
		{
			direct_link_slot->target_offset = target_offset;
			direct_link_slot->fallback_offset = fallback_offset;
		}
		return true;
	}

	bool BlockCompiler::EmitStoreCode(u32 op)
	{
		return m_code.EmitMovImm32(HOST_TMP0, op) &&
			   m_code.EmitStrImm12(HOST_TMP0, HOST_PSX_REGS, CODE_OFFSET);
	}

	bool BlockCompiler::EmitWaitLoopFastForwardBlock(u32 start_pc, u32 block_cycles)
	{
		// Keep SP 8-byte aligned across the one AAPCS helper call. No ordinary
		// block frame, architectural reloads, branch tail, or direct-link stub is
		// needed because iPsxBranchTest() owns the complete observable seam.
		m_native_instruction_count = block_cycles;
		m_helper_instruction_count = 0;
		return m_code.EmitPush(REG_R4 | REG_LR) &&
			   m_code.EmitMovImm32(HOST_TMP0, start_pc) &&
			   m_code.EmitMovImm32(HOST_TMP1, block_cycles) &&
			   m_code.EmitCallAbsolute(
				   reinterpret_cast<const void*>(&VitaIopA32FastForwardWaitLoop), HOST_CALL_SCRATCH) &&
			   m_code.EmitPop(REG_R4 | REG_PC);
	}

	bool BlockCompiler::EmitTraceCheck(u32 pc, u32 op, std::vector<size_t>& direct_exit_branches)
	{
		// Trace callbacks observe complete pre-instruction architectural state.
		// Publishing pins here keeps the diagnostic stream exact while production
		// blocks retain values until their real observable seam.
		if (!EmitFlushPinnedGprs() ||
			!m_code.EmitMovImm32(HOST_TMP0, pc) ||
			!m_code.EmitMovImm32(HOST_TMP1, op) ||
			!m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&VitaIopA32TraceInstruction)) ||
			!m_code.EmitCmpImm32(HOST_TMP0, 0))
		{
			return false;
		}

		direct_exit_branches.push_back(m_code.EmitBranchPlaceholder(VitaA32::Condition::NE));
		return true;
	}

	void BlockCompiler::AnalyzePinnedGprs(u32 start_pc, u32 instruction_count)
	{
		// PCSX2 owner: x86/iR3000A.cpp::rpsxpropBSC() computes block-local
		// register use before emission, and iR3000Atables.cpp keeps allocated
		// R3000A values live until its flush boundary. Start with the two
		// callee-saved hosts which scalar/aligned-memory blocks do not otherwise
		// consume; unusual instruction families retain the existing memory-backed
		// path until their clobber contracts are described.
		m_pinned_gprs = {};
		m_pinned_gpr_count = 0;
		m_pinned_gpr_load_hits = 0;
		m_pinned_gpr_store_hits = 0;
		m_pinned_gpr_initial_loads = 0;
		m_pinned_gpr_memory_ops_saved = 0;
		m_pinned_gpr_min_exit_savings = UINT32_MAX;
#if defined(VITASX2_QEMU_VALIDATION)
		if (!s_qemuIopPinnedGprResidencyEnabled)
			return;
#endif
		std::array<u16, 32> scores{};
		u32 written_mask = 1;
		u32 needs_initial_mask = 0;
		bool supported = true;
		bool reserves_register_jump_host = false;
		const auto read = [&](unsigned reg) {
			if (reg == 0 || reg >= scores.size())
				return;
			scores[reg] += 2;
			if ((written_mask & (1u << reg)) == 0)
				needs_initial_mask |= 1u << reg;
		};
		const auto write = [&](unsigned reg) {
			if (reg == 0 || reg >= scores.size())
				return;
			scores[reg] += 1;
			written_mask |= 1u << reg;
		};

		for (u32 i = 0; supported && i < instruction_count; i++)
		{
			const u32 pc = start_pc + i * 4;
			const u32 op = iopMemRead32(pc);
			if (IsIopBranchOrJumpOpcode(op) || IsIopExceptionOpcode(op))
			{
				const u32 delay_op = (i + 1 < instruction_count) ? iopMemRead32(pc + 4) : 0;
				const bool complete_delay_pair =
					i + 1 < instruction_count &&
					!IsIopBranchOrJumpOpcode(delay_op) &&
					!IsIopExceptionOpcode(delay_op);
				const bool final_delay_pair =
					i + 2 == instruction_count && complete_delay_pair;
				const bool path_specific_static_branch =
					IsIopStaticConditionalBranchOpcode(op) && complete_delay_pair;
				const bool native_static_jump =
					IsIopStaticJumpOpcode(op) && final_delay_pair &&
					((op >> 26) != 0x02 || (delay_op >> 16) != 0x2400);
				const bool native_register_jump =
					IsIopRegisterJumpOpcode(op) && final_delay_pair;
				if (native_register_jump)
					reserves_register_jump_host = true;
				if (!path_specific_static_branch && !native_static_jump && !native_register_jump)
				{
					supported = false;
					continue;
				}
			}
			switch (op >> 26)
			{
				case 0x00:
					switch (op & 0x3fu)
					{
						case 0x00: case 0x02: case 0x03:
							read(RT(op)); write(RD(op)); break;
						case 0x04: case 0x06: case 0x07:
						case 0x20: case 0x21: case 0x22: case 0x23:
						case 0x24: case 0x25: case 0x26: case 0x27:
						case 0x2a: case 0x2b:
							read(RS(op)); read(RT(op)); write(RD(op)); break;
						case 0x08: // JR
							read(RS(op)); break;
						case 0x09: // JALR
							read(RS(op)); write(RD(op)); break;
						default:
							supported = false; break;
					}
					break;
				case 0x01:
					read(RS(op));
					supported = RT(op) == 0x00 || RT(op) == 0x01 ||
						RT(op) == 0x10 || RT(op) == 0x11;
					break;
				case 0x02: break;
				case 0x03: write(31); break;
				case 0x04: case 0x05:
					read(RS(op)); read(RT(op)); break;
				case 0x06: case 0x07:
					read(RS(op)); break;
				case 0x08: case 0x09: case 0x0a: case 0x0b:
				case 0x0c: case 0x0d: case 0x0e:
					read(RS(op)); write(RT(op)); break;
				case 0x0f:
					write(RT(op)); break;
				case 0x20: case 0x21: case 0x23: case 0x24: case 0x25:
					read(RS(op)); write(RT(op)); break;
				case 0x28: case 0x29: case 0x2b:
					read(RS(op)); read(RT(op)); break;
				default:
					supported = false; break;
			}
		}

		if (!supported)
			return;

		constexpr std::array<u8, 2> pin_hosts = {HOST_SAVED1, HOST_REGISTER_JUMP_TARGET};
		const u8 pin_host_count = reserves_register_jump_host ? 1 : static_cast<u8>(pin_hosts.size());
		for (u8 host_index = 0; host_index < pin_host_count; host_index++)
		{
			const u8 host = pin_hosts[host_index];
			unsigned best_guest = 0;
			u16 best_score = 2;
			for (unsigned guest = 1; guest < 31; guest++)
			{
				bool already_pinned = false;
				for (u8 p = 0; p < m_pinned_gpr_count; p++)
					already_pinned |= m_pinned_gprs[p].guest == guest;
				if (!already_pinned && scores[guest] > best_score)
				{
					best_guest = guest;
					best_score = scores[guest];
				}
			}

			if (best_guest == 0)
				break;
			PinnedGpr& pin = m_pinned_gprs[m_pinned_gpr_count++];
			pin.guest = static_cast<u8>(best_guest);
			pin.host = host;
			pin.needs_initial_load = (needs_initial_mask & (1u << best_guest)) != 0;
		}
	}

	void BlockCompiler::AnalyzeSavedRegisters(u32 start_pc, u32 instruction_count)
	{
		// PCSX2's _DynGen_EnterRecompiledCode() establishes one persistent host
		// frame. Vita returns through AAPCS at each EE timeslice, so preserve only
		// callee-saved hosts the emitted block can actually write.
		m_required_saved_registers = REG_R4;
		const bool cycle_base_register_available =
			!m_iop_ram_mask_register_available && !m_emit_trace_checks && !m_defer_cycle_updates;
		if (cycle_base_register_available || m_iop_ram_mask_register_available)
			m_required_saved_registers |= REG_R10;
		if (m_iop_ram_registers_available)
			m_required_saved_registers |= REG_R11;
		for (u8 i = 0; i < m_pinned_gpr_count; i++)
			m_required_saved_registers |= static_cast<u16>(1u << m_pinned_gprs[i].host);

		ResetGprConstState();
		for (u32 i = 0; i < instruction_count; i++)
		{
			const u32 pc = start_pc + i * 4;
			const u32 op = iopMemRead32(pc);
			const u32 primary = op >> 26;
			const auto dynamic_address = [&](u8 alignment_mask) {
				u32 address = 0;
				return !TryKnownDirectIopRamAddress(op, alignment_mask, &address);
			};

			if (primary == 0 && ((op & 0x3f) == 0x1a || (op & 0x3f) == 0x1b))
				m_required_saved_registers |= REG_R5;
			else if ((primary == 0x20 || primary == 0x24) && dynamic_address(0))
				m_required_saved_registers |= REG_R5;
			else if ((primary == 0x21 || primary == 0x25) && dynamic_address(1))
				m_required_saved_registers |= REG_R5;
			else if ((primary == 0x23 || primary == 0x28 || primary == 0x29 || primary == 0x2b) &&
				dynamic_address(primary == 0x28 ? 0 : (primary == 0x29 ? 1 : 3)))
			{
				m_required_saved_registers |= REG_R5;
			}
			else if ((primary == 0x22 || primary == 0x26 || primary == 0x2a || primary == 0x2e) &&
				dynamic_address(0))
			{
				m_required_saved_registers |= REG_R5 | REG_R6;
			}
			else if (primary == 0x32 && dynamic_address(3))
			{
				m_required_saved_registers |= REG_R5;
			}
			else if (primary == 0x3a)
			{
				m_required_saved_registers |= REG_R5;
				if (dynamic_address(3))
					m_required_saved_registers |= REG_R6;
			}

			const u32 delay_op = (i + 1 < instruction_count) ? iopMemRead32(pc + 4) : 0;
			const bool final_delay_pair =
				i + 2 == instruction_count &&
				!IsIopBranchOrJumpOpcode(delay_op) &&
				!IsIopExceptionOpcode(delay_op);
			const bool native_static_branch =
				IsIopStaticConditionalBranchOpcode(op) && final_delay_pair;
			const bool native_static_jump =
				IsIopStaticJumpOpcode(op) && final_delay_pair &&
				((op >> 26) != 0x02 || (delay_op >> 16) != 0x2400);
			const bool native_register_jump =
				IsIopRegisterJumpOpcode(op) && final_delay_pair;
			if (native_static_branch)
				m_required_saved_registers |= REG_R5 | REG_R7;
			else if (native_static_jump)
				m_required_saved_registers |= REG_R5;
			else if (native_register_jump)
				m_required_saved_registers |= REG_R5 | REG_R8;
			else if (m_defer_cycle_updates && IsIopBranchOrJumpOpcode(op))
				m_required_saved_registers |= REG_R5;

			UpdateGprConstStateAfterOpcode(op, pc);
		}
		ResetGprConstState();
	}

	int BlockCompiler::PinnedHostForGuest(unsigned guest_reg) const
	{
		for (u8 i = 0; i < m_pinned_gpr_count; i++)
		{
			if (m_pinned_gprs[i].guest == guest_reg)
				return m_pinned_gprs[i].host;
		}
		return -1;
	}

	bool BlockCompiler::EmitFlushPinnedGprs()
	{
		for (u8 i = 0; i < m_pinned_gpr_count; i++)
		{
			const PinnedGpr& pin = m_pinned_gprs[i];
			if (pin.written &&
				!m_code.EmitStrImm12(pin.host, HOST_PSX_REGS,
					static_cast<u16>(GprOffset(pin.guest))))
			{
				return false;
			}
		}
		return true;
	}

	void BlockCompiler::RecordPinnedGprExitPathSavings()
	{
		u32 dirty_pin_count = 0;
		for (u8 i = 0; i < m_pinned_gpr_count; i++)
			dirty_pin_count += m_pinned_gprs[i].written ? 1u : 0u;
		const u32 removed_memory_ops = m_pinned_gpr_load_hits + m_pinned_gpr_store_hits;
		const u32 added_memory_ops = m_pinned_gpr_initial_loads + dirty_pin_count;
		const u32 savings = removed_memory_ops > added_memory_ops ?
			removed_memory_ops - added_memory_ops : 0;
		m_pinned_gpr_min_exit_savings = std::min(m_pinned_gpr_min_exit_savings, savings);
	}

	bool BlockCompiler::EmitBranchHelperExit(const void* helper)
	{
		// PCSX2 owners: x86/iR3000Atables.cpp::rpsxBEQ_process() /
		// rpsxBNE_process() flush dirty state before saving the taken branch arm,
		// then restore the allocator snapshot for generated fallthrough. Vita's
		// helper executes the taken delay slot against canonical psxRegs, so
		// publish the current dirty pins before it and never overwrite the
		// helper-produced delay-slot state on this exiting arm.
		RecordPinnedGprExitPathSavings();
		RecordBatchedCycleExitSavings(m_current_instruction_count, true);
		const u32 known_cycle_count = m_defer_cycle_updates ? m_current_instruction_count + 1 : 0;
		return (!m_defer_cycle_updates ||
			(m_code.EmitMovRegShiftImm(HOST_SAVED0, HOST_TMP0, VitaA32::ShiftType::LSL, 0) &&
				EmitPublishCyclePrefix(m_current_instruction_count) &&
				m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_SAVED0, VitaA32::ShiftType::LSL, 0))) &&
			EmitFlushPinnedGprs() &&
			m_code.EmitCallAbsolute(helper, HOST_CALL_SCRATCH) &&
			EndBlockReturn(BlockExitKind::Direct, true, false, known_cycle_count);
	}

	void BlockCompiler::ResetGprConstState()
	{
		m_gpr_const_values.fill(0);
		m_gpr_const_known_mask = 1u;
		m_hilo_const_values.fill(0);
		m_hilo_const_known_mask = 0;
	}

	bool BlockCompiler::TryGetKnownGpr(unsigned guest_reg, u32* value) const
	{
		if (guest_reg >= m_gpr_const_values.size())
			return false;

		if ((m_gpr_const_known_mask & (1u << guest_reg)) == 0)
			return false;

		if (value)
			*value = (guest_reg == 0) ? 0 : m_gpr_const_values[guest_reg];
		return true;
	}

	void BlockCompiler::SetKnownGpr(unsigned guest_reg, u32 value)
	{
		if (guest_reg == 0 || guest_reg >= m_gpr_const_values.size())
			return;

		m_gpr_const_values[guest_reg] = value;
		m_gpr_const_known_mask |= (1u << guest_reg);
	}

	void BlockCompiler::ClearKnownGpr(unsigned guest_reg)
	{
		if (guest_reg == 0 || guest_reg >= m_gpr_const_values.size())
			return;

		m_gpr_const_known_mask &= ~(1u << guest_reg);
	}

	bool BlockCompiler::TryGetKnownHiLo(bool lo, u32* value) const
	{
		const u8 bit = lo ? 0x2u : 0x1u;
		if ((m_hilo_const_known_mask & bit) == 0)
			return false;

		if (value)
			*value = m_hilo_const_values[lo ? 1 : 0];
		return true;
	}

	void BlockCompiler::SetKnownHiLo(bool lo, u32 value)
	{
		const u8 bit = lo ? 0x2u : 0x1u;
		m_hilo_const_values[lo ? 1 : 0] = value;
		m_hilo_const_known_mask |= bit;
	}

	void BlockCompiler::ClearKnownHiLo(bool lo)
	{
		const u8 bit = lo ? 0x2u : 0x1u;
		m_hilo_const_known_mask &= static_cast<u8>(~bit);
	}

	void BlockCompiler::ClearKnownHiLo()
	{
		m_hilo_const_known_mask = 0;
	}

	bool BlockCompiler::TryKnownDirectIopRamAddress(u32 op, u8 alignment_mask, u32* address) const
	{
		u32 base = 0;
		if (!TryGetKnownGpr(RS(op), &base))
			return false;

		const u32 effective_address = base + static_cast<u32>(static_cast<s32>(IMM_S(op)));
		return TryDirectIopRamEffectiveAddress(effective_address, alignment_mask, address);
	}

	void BlockCompiler::UpdateGprConstStateAfterOpcode(u32 op, u32 pc)
	{
		const unsigned opcode = op >> 26;
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		const unsigned rd = RD(op);
		u32 lhs = 0;
		u32 rhs = 0;
		u32 lo = 0;
		u32 hi = 0;

		const auto set_binary_reg = [&](u32 (*func)(u32, u32)) {
			if (TryGetKnownGpr(rs, &lhs) && TryGetKnownGpr(rt, &rhs))
				SetKnownGpr(rd, func(lhs, rhs));
			else
				ClearKnownGpr(rd);
		};
		const auto set_shift_imm = [&](VitaA32::ShiftType shift, unsigned amount) {
			if (!TryGetKnownGpr(rt, &rhs))
			{
				ClearKnownGpr(rd);
				return;
			}

			switch (shift)
			{
				case VitaA32::ShiftType::LSL:
					SetKnownGpr(rd, rhs << amount);
					break;
				case VitaA32::ShiftType::LSR:
					SetKnownGpr(rd, rhs >> amount);
					break;
				case VitaA32::ShiftType::ASR:
					SetKnownGpr(rd, static_cast<u32>(static_cast<s32>(rhs) >> amount));
					break;
				default:
					ClearKnownGpr(rd);
					break;
			}
		};
		const auto set_shift_reg = [&](VitaA32::ShiftType shift) {
			if (!TryGetKnownGpr(rt, &rhs) || !TryGetKnownGpr(rs, &lhs))
			{
				ClearKnownGpr(rd);
				return;
			}

			const unsigned amount = lhs & 0x1f;
			switch (shift)
			{
				case VitaA32::ShiftType::LSL:
					SetKnownGpr(rd, rhs << amount);
					break;
				case VitaA32::ShiftType::LSR:
					SetKnownGpr(rd, rhs >> amount);
					break;
				case VitaA32::ShiftType::ASR:
					SetKnownGpr(rd, static_cast<u32>(static_cast<s32>(rhs) >> amount));
					break;
				default:
					ClearKnownGpr(rd);
					break;
			}
		};

		switch (opcode)
		{
			case 0x00: // SPECIAL
				switch (op & 0x3f)
				{
					case 0x00: // SLL
						set_shift_imm(VitaA32::ShiftType::LSL, SA(op));
						break;
					case 0x02: // SRL
						set_shift_imm(VitaA32::ShiftType::LSR, SA(op));
						break;
					case 0x03: // SRA
						set_shift_imm(VitaA32::ShiftType::ASR, SA(op));
						break;
					case 0x04: // SLLV
						set_shift_reg(VitaA32::ShiftType::LSL);
						break;
					case 0x06: // SRLV
						set_shift_reg(VitaA32::ShiftType::LSR);
						break;
					case 0x07: // SRAV
						set_shift_reg(VitaA32::ShiftType::ASR);
						break;
					case 0x09: // JALR
						SetKnownGpr(rd, pc + 8);
						break;
					case 0x10: // MFHI
						if (TryGetKnownHiLo(false, &lhs))
							SetKnownGpr(rd, lhs);
						else
							ClearKnownGpr(rd);
						break;
					case 0x12: // MFLO
						if (TryGetKnownHiLo(true, &lhs))
							SetKnownGpr(rd, lhs);
						else
							ClearKnownGpr(rd);
						break;
					case 0x11: // MTHI
						if (TryGetKnownGpr(rs, &lhs))
							SetKnownHiLo(false, lhs);
						else
							ClearKnownHiLo(false);
						break;
					case 0x13: // MTLO
						if (TryGetKnownGpr(rs, &lhs))
							SetKnownHiLo(true, lhs);
						else
							ClearKnownHiLo(true);
						break;
					case 0x18: // MULT
					case 0x19: // MULTU
						if (TryGetKnownGpr(rs, &lhs) && TryGetKnownGpr(rt, &rhs))
						{
							ComputeIopMultiplyResult(lhs, rhs, (op & 0x3f) == 0x18, &lo, &hi);
							SetKnownHiLo(true, lo);
							SetKnownHiLo(false, hi);
						}
						else
						{
							ClearKnownHiLo();
						}
						break;
					case 0x1a: // DIV
					case 0x1b: // DIVU
					{
						const bool signed_div = (op & 0x3f) == 0x1a;
						const bool lhs_known = TryGetKnownGpr(rs, &lhs);
						const bool rhs_known = TryGetKnownGpr(rt, &rhs);
						if (lhs_known && rhs_known)
						{
							ComputeIopDivideResult(lhs, rhs, signed_div, &lo, &hi);
							SetKnownHiLo(true, lo);
							SetKnownHiLo(false, hi);
						}
						else if (rhs_known && (rhs == 1 || (signed_div && rhs == 0xffffffffu)))
						{
							ClearKnownHiLo(true);
							SetKnownHiLo(false, 0);
						}
						else if (!signed_div && rhs_known && rhs == 0)
						{
							SetKnownHiLo(true, 0xffffffffu);
							ClearKnownHiLo(false);
						}
						else if (lhs_known && lhs == 0)
						{
							ClearKnownHiLo(true);
							SetKnownHiLo(false, 0);
						}
						else
						{
							ClearKnownHiLo();
						}
						break;
					}
					case 0x20: // ADD
					case 0x21: // ADDU
						set_binary_reg([](u32 a, u32 b) { return a + b; });
						break;
					case 0x22: // SUB
					case 0x23: // SUBU
						set_binary_reg([](u32 a, u32 b) { return a - b; });
						break;
					case 0x24: // AND
						set_binary_reg([](u32 a, u32 b) { return a & b; });
						break;
					case 0x25: // OR
						set_binary_reg([](u32 a, u32 b) { return a | b; });
						break;
					case 0x26: // XOR
						set_binary_reg([](u32 a, u32 b) { return a ^ b; });
						break;
					case 0x27: // NOR
						set_binary_reg([](u32 a, u32 b) { return ~(a | b); });
						break;
					case 0x2a: // SLT
						set_binary_reg([](u32 a, u32 b) {
							return static_cast<s32>(a) < static_cast<s32>(b) ? 1u : 0u;
						});
						break;
					case 0x2b: // SLTU
						set_binary_reg([](u32 a, u32 b) { return a < b ? 1u : 0u; });
						break;
					default:
						break;
				}
				break;

			case 0x01: // REGIMM
				if (rt == 0x10 || rt == 0x11) // BLTZAL/BGEZAL
					SetKnownGpr(31, pc + 8);
				break;

			case 0x03: // JAL
				SetKnownGpr(31, pc + 8);
				break;

			case 0x08: // ADDI
			case 0x09: // ADDIU
				if (TryGetKnownGpr(rs, &lhs))
					SetKnownGpr(rt, lhs + static_cast<u32>(static_cast<s32>(IMM_S(op))));
				else
					ClearKnownGpr(rt);
				break;
			case 0x0a: // SLTI
				if (TryGetKnownGpr(rs, &lhs))
				{
					SetKnownGpr(rt,
						(static_cast<s32>(lhs) < static_cast<s32>(IMM_S(op))) ? 1u : 0u);
				}
				else
				{
					ClearKnownGpr(rt);
				}
				break;
			case 0x0b: // SLTIU
				if (TryGetKnownGpr(rs, &lhs))
					SetKnownGpr(rt, lhs < static_cast<u32>(static_cast<s32>(IMM_S(op))) ? 1u : 0u);
				else
					ClearKnownGpr(rt);
				break;
			case 0x0c: // ANDI
				if (TryGetKnownGpr(rs, &lhs))
					SetKnownGpr(rt, lhs & IMM_U(op));
				else
					ClearKnownGpr(rt);
				break;
			case 0x0d: // ORI
				if (TryGetKnownGpr(rs, &lhs))
					SetKnownGpr(rt, lhs | IMM_U(op));
				else
					ClearKnownGpr(rt);
				break;
			case 0x0e: // XORI
				if (TryGetKnownGpr(rs, &lhs))
					SetKnownGpr(rt, lhs ^ IMM_U(op));
				else
					ClearKnownGpr(rt);
				break;
			case 0x0f: // LUI
				SetKnownGpr(rt, static_cast<u32>(IMM_U(op)) << 16);
				break;

			case 0x10: // COP0
				if ((rs == 0x00 || rs == 0x02) && rt != 0) // MFC0/CFC0
					ClearKnownGpr(rt);
				break;
			case 0x12: // COP2
				if ((op & 0x3f) == 0 && (rs == 0x00 || rs == 0x02) && rt != 0) // MFC2/CFC2
					ClearKnownGpr(rt);
				break;

			case 0x20: // LB
			case 0x21: // LH
			case 0x22: // LWL
			case 0x23: // LW
			case 0x24: // LBU
			case 0x25: // LHU
			case 0x26: // LWR
				ClearKnownGpr(rt);
				break;

			default:
				break;
		}
	}

	bool BlockCompiler::EmitStorePc(u32 pc)
	{
		return m_code.EmitMovImm32(HOST_TMP0, pc) &&
			   m_code.EmitStrImm12(HOST_TMP0, HOST_PSX_REGS, PC_OFFSET);
	}

	bool BlockCompiler::EmitStorePcReg(unsigned host_reg)
	{
		return m_code.EmitStrImm12(host_reg, HOST_PSX_REGS, PC_OFFSET);
	}

	bool BlockCompiler::EmitAddCycles(u32 cycles)
	{
		if (cycles == 0)
			return true;

		const auto emit_add_to_loaded_cycle = [this, cycles](unsigned address_reg, u8 offset) {
			const unsigned cycle_scratch = (address_reg == HOST_TMP2) ? HOST_TMP3 : HOST_TMP2;
			if (!m_code.EmitLdrdImm8(HOST_TMP0, HOST_TMP1, address_reg, offset))
				return false;

			if (!m_code.EmitAddImm32(HOST_TMP0, HOST_TMP0, cycles, true))
			{
				if (!m_code.EmitMovImm32(cycle_scratch, cycles) ||
					!m_code.EmitAddReg(HOST_TMP0, HOST_TMP0, cycle_scratch, true))
				{
					return false;
				}
			}

			return m_code.EmitAdcImm8(HOST_TMP1, HOST_TMP1, 0) &&
				   m_code.EmitStrdImm8(HOST_TMP0, HOST_TMP1, address_reg, offset);
		};

		if (m_iop_cycle_base_register_available)
			return emit_add_to_loaded_cycle(HOST_CYCLE_BASE, 0);

		if (CYCLE_OFFSET <= 0xff)
			return emit_add_to_loaded_cycle(HOST_PSX_REGS, static_cast<u8>(CYCLE_OFFSET));

		if (m_code.EmitAddImm32(HOST_TMP2, HOST_PSX_REGS, static_cast<u32>(CYCLE_OFFSET)))
			return emit_add_to_loaded_cycle(HOST_TMP2, 0);

		if (!m_code.EmitLdrImm12(HOST_TMP0, HOST_PSX_REGS, CYCLE_OFFSET) ||
			!m_code.EmitLdrImm12(HOST_TMP1, HOST_PSX_REGS, CYCLE_OFFSET + sizeof(u32)))
		{
			return false;
		}

		if (!m_code.EmitAddImm32(HOST_TMP0, HOST_TMP0, cycles, true))
		{
			if (!m_code.EmitMovImm32(HOST_TMP2, cycles) ||
				!m_code.EmitAddReg(HOST_TMP0, HOST_TMP0, HOST_TMP2, true))
			{
				return false;
			}
		}

		return m_code.EmitAdcImm8(HOST_TMP1, HOST_TMP1, 0) &&
			   m_code.EmitStrImm12(HOST_TMP0, HOST_PSX_REGS, CYCLE_OFFSET) &&
			   m_code.EmitStrImm12(HOST_TMP1, HOST_PSX_REGS, CYCLE_OFFSET + sizeof(u32));
	}

	bool BlockCompiler::EmitPublishCyclePrefix(u32 cycle_prefix)
	{
		if (!m_track_published_cycle_prefix)
			return EmitAddCycles(cycle_prefix);

		// The stack word is path state, not architectural state: it records how
		// many guest cycles this callable has already made visible. Every handler
		// path publishes only the missing delta, while direct RAM paths leave it at
		// zero until the final block exit.
		return m_code.EmitLdrImm12(HOST_TMP0, HOST_SP, 0) &&
			m_code.EmitMovImm32(HOST_TMP1, cycle_prefix) &&
			m_code.EmitSubReg(HOST_TMP0, HOST_TMP1, HOST_TMP0) &&
			m_code.EmitStrImm12(HOST_TMP1, HOST_SP, 0) &&
			m_code.EmitAddImm32(HOST_CALL_SCRATCH, HOST_PSX_REGS,
				static_cast<u32>(CYCLE_OFFSET)) &&
			m_code.EmitLdrdImm8(HOST_TMP2, HOST_TMP3, HOST_CALL_SCRATCH, 0) &&
			m_code.EmitAddReg(HOST_TMP2, HOST_TMP2, HOST_TMP0, true) &&
			m_code.EmitAdcImm8(HOST_TMP3, HOST_TMP3, 0) &&
			m_code.EmitStrdImm8(HOST_TMP2, HOST_TMP3, HOST_CALL_SCRATCH, 0);
	}

	u32 BlockCompiler::CurrentTimingHelperSeamCount() const
	{
		return static_cast<u32>(m_scalar_load_cold_tails.size() +
			m_scalar_store_cold_tails.size() +
			m_unaligned_read_cold_tails.size() +
			m_unaligned_write_cold_tails.size() +
			m_cop2_load_cold_tails.size() +
			m_cop2_store_cold_tails.size());
	}

	void BlockCompiler::RecordBatchedCycleExitSavings(
		u32 cycle_prefix, bool preserves_argument)
	{
		if (!m_expanded_cycle_batching)
			return;

		const bool ps1_clock_mode = (psxHu32(HW_ICFG) & (1u << 3)) != 0;
		s32 savings = 0;
		if (m_track_published_cycle_prefix)
		{
			const u32 helper_seams = CurrentTimingHelperSeamCount();
			savings = static_cast<s32>(4u * cycle_prefix) -
				static_cast<s32>(9u * helper_seams) -
				static_cast<s32>(m_unaligned_write_cold_tails.size()) -
				static_cast<s32>(ps1_clock_mode ? 7u : 6u) -
				(preserves_argument ? 2 : 0);
		}
		else
		{
			savings = static_cast<s32>(4u * cycle_prefix) +
				(ps1_clock_mode ? 0 : 1) - (preserves_argument ? 2 : 0);
		}

		const u32 bounded_savings = savings > 0 ? static_cast<u32>(savings) : 0;
		if (m_batched_cycle_instructions_removed == UINT32_MAX)
			m_batched_cycle_instructions_removed = bounded_savings;
		else
			m_batched_cycle_instructions_removed =
				std::min(m_batched_cycle_instructions_removed, bounded_savings);
	}

	bool BlockCompiler::EmitIncrementCycle()
	{
		return EmitAddCycles(1);
	}

	bool BlockCompiler::EmitChargeEeBudgetPs1(u32 known_block_cycles)
	{
		// PCSX2 owner: x86/iR3000A.cpp::iPsxAddEECycles(), PS1 clock mode.
		// Blocks are bounded to 64 IOP instructions, so t = delta * 1280 + carry
		// is <= 82066.  floor(t / 147) is exact as high32(t * 0x01bdd2b9)
		// for that range, and the remainder is reconstructed as t - q * 147.
		return
			(known_block_cycles == 0 || m_code.EmitMovImm32(HOST_TMP0, known_block_cycles)) &&
			m_code.EmitMovRegShiftImm(HOST_TMP1, HOST_TMP0, VitaA32::ShiftType::LSL, 10) &&
			m_code.EmitAddRegShiftImm(HOST_TMP1, HOST_TMP1, HOST_TMP0, VitaA32::ShiftType::LSL, 8) &&
			m_code.EmitLdrImm12(HOST_TMP2, HOST_PSX_REGS, static_cast<u16>(IOP_CYCLE_EE_CARRY_OFFSET)) &&
			m_code.EmitAddReg(HOST_TMP1, HOST_TMP1, HOST_TMP2) &&
			m_code.EmitMovImm32(HOST_TMP2, 0x01bdd2b9u) &&
			m_code.EmitUmull(HOST_TMP0, HOST_TMP3, HOST_TMP1, HOST_TMP2) &&
			m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP3, VitaA32::ShiftType::LSL, 7) &&
			m_code.EmitAddRegShiftImm(HOST_TMP2, HOST_TMP2, HOST_TMP3, VitaA32::ShiftType::LSL, 4) &&
			m_code.EmitAddRegShiftImm(HOST_TMP2, HOST_TMP2, HOST_TMP3, VitaA32::ShiftType::LSL, 1) &&
			m_code.EmitAddReg(HOST_TMP2, HOST_TMP2, HOST_TMP3) &&
			m_code.EmitSubReg(HOST_TMP1, HOST_TMP1, HOST_TMP2) &&
			m_code.EmitStrImm12(HOST_TMP1, HOST_PSX_REGS, static_cast<u16>(IOP_CYCLE_EE_CARRY_OFFSET)) &&
			m_code.EmitLdrImm12(HOST_TMP0, HOST_PSX_REGS, static_cast<u16>(IOP_CYCLE_EE_OFFSET)) &&
			m_code.EmitSubReg(HOST_TMP0, HOST_TMP0, HOST_TMP3, true) &&
			m_code.EmitStrImm12(HOST_TMP0, HOST_PSX_REGS, static_cast<u16>(IOP_CYCLE_EE_OFFSET));
	}

	bool BlockCompiler::EmitChargeEeBudget(u32 known_cycle_count)
	{
		if (!m_direct_exit_branches || !m_budget_exit_branches)
			return false;
		m_has_budget_exit = true;

		// PCSX2 owner: x86/iR3000A.cpp::iPsxAddEECycles() leaves the signed
		// budget subtraction flags live for iPsxBranchTest()'s xJLE. A32 STR
		// also preserves those flags, so branch on LE directly instead of
		// materializing and retesting a temporary Boolean.
		const auto emit_budget_exit_from_signed_flags = [this]() {
			m_budget_exit_branches->push_back(
				m_code.EmitBranchPlaceholder(VitaA32::Condition::LE));
			return m_budget_exit_branches->back() != static_cast<size_t>(-1);
		};

		const u32 known_block_cycles = known_cycle_count != 0 ? known_cycle_count :
			(m_defer_cycle_updates ? m_block_cycle_count : 0);
		if (known_block_cycles == 0 &&
			(!m_code.EmitLdrImm12(HOST_TMP0, HOST_PSX_REGS, CYCLE_OFFSET) ||
				!m_code.EmitLdrImm12(HOST_TMP1, HOST_SP, 0) ||
				!m_code.EmitSubReg(HOST_TMP0, HOST_TMP0, HOST_TMP1)))
		{
			return false;
		}

		bool specialize_clock_mode = true;
#if defined(VITASX2_QEMU_VALIDATION)
		specialize_clock_mode = s_qemuIopClockModeSpecializationEnabled;
#endif
		if (specialize_clock_mode)
		{
			// PCSX2 owner: x86/iR3000A.cpp::iPsxAddEECycles() reads HW_ICFG
			// while recompiling and emits only the active PS2 or PS1 clock formula.
			// HwWrite.cpp resets the IOP CPU cache at the sole PS1-mode transition,
			// so no generated block can outlive this compile-time fact.
			const bool ps1_clock_mode = (psxHu32(HW_ICFG) & (1u << 3)) != 0;
			m_clock_mode_check_instructions_removed = ps1_clock_mode ? 5 : 6;
			if (ps1_clock_mode)
			{
				return EmitChargeEeBudgetPs1(known_block_cycles) &&
					emit_budget_exit_from_signed_flags();
			}

			const bool emitted_ee_cycles = known_block_cycles != 0 ?
				m_code.EmitMovImm32(HOST_TMP2, known_block_cycles * 8) :
				m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP0, VitaA32::ShiftType::LSL, 3);
			return emitted_ee_cycles &&
				m_code.EmitLdrImm12(HOST_TMP1, HOST_PSX_REGS, static_cast<u16>(IOP_CYCLE_EE_OFFSET)) &&
				m_code.EmitSubReg(HOST_TMP1, HOST_TMP1, HOST_TMP2, true) &&
				m_code.EmitStrImm12(HOST_TMP1, HOST_PSX_REGS, static_cast<u16>(IOP_CYCLE_EE_OFFSET)) &&
				emit_budget_exit_from_signed_flags();
		}

		if (!m_code.EmitMovImm32(HOST_TMP2,
				static_cast<u32>(reinterpret_cast<uptr>(&iopHw[HW_ICFG & 0xffff]))) ||
			!m_code.EmitLdrImm12(HOST_TMP1, HOST_TMP2, 0) ||
			!m_code.EmitTstImm32(HOST_TMP1, 1u << 3))
		{
			return false;
		}

		const size_t ps1_clock_mode = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
		if (ps1_clock_mode == static_cast<size_t>(-1))
			return false;

		const bool emitted_ee_cycles = known_block_cycles != 0 ?
			m_code.EmitMovImm32(HOST_TMP2, known_block_cycles * 8) :
			m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP0, VitaA32::ShiftType::LSL, 3);
		if (!emitted_ee_cycles ||
			!m_code.EmitLdrImm12(HOST_TMP1, HOST_PSX_REGS, static_cast<u16>(IOP_CYCLE_EE_OFFSET)) ||
			!m_code.EmitSubReg(HOST_TMP1, HOST_TMP1, HOST_TMP2, true) ||
			!m_code.EmitStrImm12(HOST_TMP1, HOST_PSX_REGS, static_cast<u16>(IOP_CYCLE_EE_OFFSET)) ||
			!emit_budget_exit_from_signed_flags())
		{
			return false;
		}

		const size_t done = m_code.EmitBranchPlaceholder();
		if (done == static_cast<size_t>(-1))
			return false;

		const size_t ps1_clock_mode_target = m_code.Size();
		if (!m_code.PatchBranch(ps1_clock_mode, ps1_clock_mode_target, VitaA32::Condition::NE) ||
			!EmitChargeEeBudgetPs1(known_block_cycles) ||
			!emit_budget_exit_from_signed_flags())
		{
			return false;
		}

		return m_code.PatchBranch(done, m_code.Size());
	}

	bool BlockCompiler::EmitPcChangedExitCheck(u32 expected_pc, std::vector<size_t>& direct_exit_branches)
	{
		if (!m_code.EmitLdrImm12(HOST_TMP0, HOST_PSX_REGS, PC_OFFSET))
			return false;

		if (!(m_code.EmitCmpImm32(HOST_TMP0, expected_pc) ||
			  (m_code.EmitMovImm32(HOST_TMP1, expected_pc) && m_code.EmitCmpReg(HOST_TMP0, HOST_TMP1))))
		{
			return false;
		}

		direct_exit_branches.push_back(m_code.EmitBranchPlaceholder(VitaA32::Condition::NE));
		return true;
	}

	bool BlockCompiler::EmitPcChangedExitCheckReg(unsigned expected_host_reg, std::vector<size_t>& direct_exit_branches)
	{
		if (!m_code.EmitLdrImm12(HOST_TMP0, HOST_PSX_REGS, PC_OFFSET) ||
			!m_code.EmitCmpReg(HOST_TMP0, expected_host_reg))
		{
			return false;
		}

		direct_exit_branches.push_back(m_code.EmitBranchPlaceholder(VitaA32::Condition::NE));
		return true;
	}

	bool BlockCompiler::EmitLoadGpr(unsigned guest_reg, unsigned host_reg)
	{
		if (guest_reg == 0)
			return m_code.EmitMovImm8(host_reg, 0);
		if (const int pinned_host = PinnedHostForGuest(guest_reg); pinned_host >= 0)
		{
			m_pinned_gpr_load_hits++;
			return static_cast<unsigned>(pinned_host) == host_reg ||
				m_code.EmitMovRegShiftImm(host_reg, static_cast<unsigned>(pinned_host),
					VitaA32::ShiftType::LSL, 0);
		}

		return m_code.EmitLdrImm12(host_reg, HOST_PSX_REGS, static_cast<u16>(GprOffset(guest_reg)));
	}

	bool BlockCompiler::EmitLoadGprValue(unsigned guest_reg, unsigned host_reg, bool* used_known_value)
	{
		// PCSX2 owners read guest GPR operands from psxRegs.GPR. When this block
		// already proves that value, keep Cortex-A9 off the load path and
		// materialize it directly.
		u32 known_value = 0;
		if (TryGetKnownGpr(guest_reg, &known_value))
		{
			if (used_known_value)
				*used_known_value = guest_reg != 0;
			return m_code.EmitMovImm32(host_reg, known_value);
		}

		if (used_known_value)
			*used_known_value = false;
		return EmitLoadGpr(guest_reg, host_reg);
	}

	bool BlockCompiler::EmitStoreGpr(unsigned guest_reg, unsigned host_reg)
	{
		if (guest_reg == 0)
			return true;
		if (const int pinned_host = PinnedHostForGuest(guest_reg); pinned_host >= 0)
		{
			m_pinned_gpr_store_hits++;
			for (u8 i = 0; i < m_pinned_gpr_count; i++)
			{
				if (m_pinned_gprs[i].guest == guest_reg)
				{
					m_pinned_gprs[i].written = true;
					m_pinned_gprs[i].ever_written = true;
				}
			}
			return static_cast<unsigned>(pinned_host) == host_reg ||
				m_code.EmitMovRegShiftImm(static_cast<unsigned>(pinned_host), host_reg,
					VitaA32::ShiftType::LSL, 0);
		}

		return m_code.EmitStrImm12(host_reg, HOST_PSX_REGS, static_cast<u16>(GprOffset(guest_reg)));
	}

	bool BlockCompiler::EmitStoreGprZero(unsigned guest_reg)
	{
		if (guest_reg == 0)
			return true;

		return m_code.EmitMovImm8(HOST_TMP0, 0) &&
			   EmitStoreGpr(guest_reg, HOST_TMP0);
	}

	bool BlockCompiler::EmitMoveGpr(unsigned dst_guest_reg, unsigned src_guest_reg)
	{
		if (dst_guest_reg == 0)
			return true;

		if (dst_guest_reg == src_guest_reg)
			return true;

		return EmitLoadGpr(src_guest_reg, HOST_TMP0) &&
			   EmitStoreGpr(dst_guest_reg, HOST_TMP0);
	}

	bool BlockCompiler::EmitCompareGprs(unsigned lhs_guest_reg, unsigned rhs_guest_reg)
	{
		// PCSX2 owner: x86/iR3000Atables.cpp::rpsxBEQ()/rpsxBNE(). This only
		// changes the Vita compare sequence; DuckStation's ARM32 R3000A
		// recompiler proves this codegen shape for constants, but not PS2 IOP
		// semantics.
		u32 lhs_value = 0;
		u32 rhs_value = 0;
		const bool lhs_known = TryGetKnownGpr(lhs_guest_reg, &lhs_value);
		const bool rhs_known = TryGetKnownGpr(rhs_guest_reg, &rhs_value);
		const bool has_tracked_operand =
			(lhs_guest_reg != 0 && lhs_known) || (rhs_guest_reg != 0 && rhs_known);
		const auto emit_false_compare = [this]() {
			return m_code.EmitMovImm8(HOST_TMP0, 0) &&
				   m_code.EmitCmpImm32(HOST_TMP0, 1);
		};
		const auto emit_cmp_reg_imm = [this](unsigned host_reg, u32 value) {
			return m_code.EmitCmpImm32(host_reg, value) ||
				   (m_code.EmitMovImm32(HOST_TMP1, value) &&
					   m_code.EmitCmpReg(host_reg, HOST_TMP1));
		};

		if (lhs_known && rhs_known)
		{
#if defined(VITASX2_QEMU_VALIDATION)
			if (has_tracked_operand)
				++g_qemuIopConstBranchCompareFastPaths;
#endif
			return (lhs_value == rhs_value) ?
					   m_code.EmitCmpReg(HOST_TMP0, HOST_TMP0) :
					   emit_false_compare();
		}

		if (lhs_known)
		{
#if defined(VITASX2_QEMU_VALIDATION)
			if (has_tracked_operand)
				++g_qemuIopConstBranchCompareFastPaths;
#endif
			return EmitLoadGpr(rhs_guest_reg, HOST_TMP0) &&
				   emit_cmp_reg_imm(HOST_TMP0, lhs_value);
		}

		if (rhs_known)
		{
#if defined(VITASX2_QEMU_VALIDATION)
			if (has_tracked_operand)
				++g_qemuIopConstBranchCompareFastPaths;
#endif
			return EmitLoadGpr(lhs_guest_reg, HOST_TMP0) &&
				   emit_cmp_reg_imm(HOST_TMP0, rhs_value);
		}

		if (lhs_guest_reg == 0 && rhs_guest_reg == 0)
			return m_code.EmitCmpReg(HOST_TMP0, HOST_TMP0);
		if (lhs_guest_reg == 0)
			return EmitLoadGpr(rhs_guest_reg, HOST_TMP0) &&
				   m_code.EmitCmpImm32(HOST_TMP0, 0);
		if (rhs_guest_reg == 0)
			return EmitLoadGpr(lhs_guest_reg, HOST_TMP0) &&
				   m_code.EmitCmpImm32(HOST_TMP0, 0);

		return EmitLoadGpr(lhs_guest_reg, HOST_TMP0) &&
			   EmitLoadGpr(rhs_guest_reg, HOST_TMP1) &&
			   m_code.EmitCmpReg(HOST_TMP0, HOST_TMP1);
	}

	bool BlockCompiler::EmitBinaryRegOp(u32 op)
	{
		const unsigned rd = RD(op);
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		const u32 funct = op & 0x3f;

		if (rd == 0)
			return true;

		u32 known_rs = 0;
		u32 known_rt = 0;
		if (TryGetKnownGpr(rs, &known_rs) && TryGetKnownGpr(rt, &known_rt))
		{
			u32 result = 0;
			bool can_fold = true;
			switch (funct)
			{
				case 0x20: // ADD
				case 0x21: // ADDU
					// PCSX2 owners: R3000AOpcodeTables.cpp::psxADD() /
					// psxADDU() and x86/iR3000Atables.cpp::rpsxADDU_const().
					// The IOP ADD path in this tree wraps just like ADDU.
					result = known_rs + known_rt;
					break;
				case 0x22: // SUB
				case 0x23: // SUBU
					// PCSX2 owners: R3000AOpcodeTables.cpp::psxSUB() /
					// psxSUBU() and x86/iR3000Atables.cpp::rpsxSUBU_const().
					result = known_rs - known_rt;
					break;
				case 0x24: // AND
					// PCSX2 owners: R3000AOpcodeTables.cpp::psxAND() and
					// x86/iR3000Atables.cpp::rpsxAND_const().
					result = known_rs & known_rt;
					break;
				case 0x25: // OR
					// PCSX2 owners: R3000AOpcodeTables.cpp::psxOR() and
					// x86/iR3000Atables.cpp::rpsxOR_const().
					result = known_rs | known_rt;
					break;
				case 0x26: // XOR
					// PCSX2 owners: R3000AOpcodeTables.cpp::psxXOR() and
					// x86/iR3000Atables.cpp::rpsxXOR_const().
					result = known_rs ^ known_rt;
					break;
				case 0x27: // NOR
					// PCSX2 owners: R3000AOpcodeTables.cpp::psxNOR() and
					// x86/iR3000Atables.cpp::rpsxNOR_const().
					result = ~(known_rs | known_rt);
					break;
				default:
					can_fold = false;
					break;
			}

			if (can_fold)
			{
#if defined(VITASX2_QEMU_VALIDATION)
				++g_qemuIopConstRegisterOpFastPaths;
#endif
				return ((result <= 0xffu) ? m_code.EmitMovImm8(HOST_TMP0, static_cast<u8>(result)) :
											m_code.EmitMovImm32(HOST_TMP0, result)) &&
					   EmitStoreGpr(rd, HOST_TMP0);
			}
		}

		const auto store_zero = [this, rd]() {
			return EmitStoreGprZero(rd);
		};
		const auto store_not = [this, rd](unsigned guest_reg) {
			return EmitLoadGpr(guest_reg, HOST_TMP0) &&
				   m_code.EmitMvnReg(HOST_TMP2, HOST_TMP0) &&
				   EmitStoreGpr(rd, HOST_TMP2);
		};

		// PCSX2 owners: x86/iR3000Atables.cpp::rpsxADDU_(),
		// rpsxSUBU_(), and rpsxLogicalOp(). Fold the same $zero/no-op
		// identities before paying for both operand loads.
		switch (funct)
		{
			case 0x20: // ADD
			case 0x21: // ADDU
				if (rs == 0)
					return EmitMoveGpr(rd, rt);
				if (rt == 0)
					return EmitMoveGpr(rd, rs);
				break;
			case 0x22: // SUB
			case 0x23: // SUBU
				if (rs == rt)
					return store_zero();
				if (rt == 0)
					return EmitMoveGpr(rd, rs);
				if (rs == 0)
				{
					return EmitLoadGpr(rt, HOST_TMP0) &&
						   m_code.EmitRsbImm32(HOST_TMP2, HOST_TMP0, 0) &&
						   EmitStoreGpr(rd, HOST_TMP2);
				}
				break;
			case 0x24: // AND
				if (rs == 0 || rt == 0)
					return store_zero();
				if (rs == rt)
					return EmitMoveGpr(rd, rs);
				break;
			case 0x25: // OR
				if (rs == 0)
					return EmitMoveGpr(rd, rt);
				if (rt == 0 || rs == rt)
					return EmitMoveGpr(rd, rs);
				break;
			case 0x26: // XOR
				if (rs == rt)
					return store_zero();
				if (rs == 0)
					return EmitMoveGpr(rd, rt);
				if (rt == 0)
					return EmitMoveGpr(rd, rs);
				break;
			case 0x27: // NOR
				if (rs == 0 && rt == 0)
				{
					return m_code.EmitMovImm32(HOST_TMP0, 0xffffffffu) &&
						   EmitStoreGpr(rd, HOST_TMP0);
				}
				if (rs == 0)
					return store_not(rt);
				if (rt == 0 || rs == rt)
					return store_not(rs);
				break;
			default:
				break;
		}

		const bool rs_tracked = rs != 0 && TryGetKnownGpr(rs, &known_rs);
		const bool rt_tracked = rt != 0 && TryGetKnownGpr(rt, &known_rt);
		const auto emit_reg_imm_or_reg = [this](u32 known_value,
												bool (*emit_imm)(VitaA32::CodeBuffer&, unsigned, unsigned, u32),
												bool (*emit_reg)(VitaA32::CodeBuffer&, unsigned, unsigned, unsigned)) {
			return emit_imm(m_code, HOST_TMP2, HOST_TMP0, known_value) ||
				   (m_code.EmitMovImm32(HOST_TMP1, known_value) &&
					   emit_reg(m_code, HOST_TMP2, HOST_TMP0, HOST_TMP1));
		};
		const auto emit_add_imm = [](VitaA32::CodeBuffer& code, unsigned rd, unsigned rn, u32 value) {
			return code.EmitAddImm32(rd, rn, value);
		};
		const auto emit_sub_imm = [](VitaA32::CodeBuffer& code, unsigned rd, unsigned rn, u32 value) {
			return code.EmitSubImm32(rd, rn, value);
		};
		const auto emit_and_imm = [](VitaA32::CodeBuffer& code, unsigned rd, unsigned rn, u32 value) {
			return code.EmitAndImm32(rd, rn, value);
		};
		const auto emit_orr_imm = [](VitaA32::CodeBuffer& code, unsigned rd, unsigned rn, u32 value) {
			return code.EmitOrrImm32(rd, rn, value);
		};
		const auto emit_eor_imm = [](VitaA32::CodeBuffer& code, unsigned rd, unsigned rn, u32 value) {
			return code.EmitEorImm32(rd, rn, value);
		};
		const auto emit_add_reg = [](VitaA32::CodeBuffer& code, unsigned rd, unsigned rn, unsigned rm) {
			return code.EmitAddReg(rd, rn, rm);
		};
		const auto emit_sub_reg = [](VitaA32::CodeBuffer& code, unsigned rd, unsigned rn, unsigned rm) {
			return code.EmitSubReg(rd, rn, rm);
		};
		const auto emit_and_reg = [](VitaA32::CodeBuffer& code, unsigned rd, unsigned rn, unsigned rm) {
			return code.EmitAndReg(rd, rn, rm);
		};
		const auto emit_orr_reg = [](VitaA32::CodeBuffer& code, unsigned rd, unsigned rn, unsigned rm) {
			return code.EmitOrrReg(rd, rn, rm);
		};
		const auto emit_eor_reg = [](VitaA32::CodeBuffer& code, unsigned rd, unsigned rn, unsigned rm) {
			return code.EmitEorReg(rd, rn, rm);
		};
		const auto emit_counted_store = [this, rd]() {
#if defined(VITASX2_QEMU_VALIDATION)
			++g_qemuIopConstRegisterOperandFastPaths;
#endif
			return EmitStoreGpr(rd, HOST_TMP2);
		};
		const auto emit_known_operand = [&](bool known_lhs, u32 known_value) -> bool {
			const unsigned dynamic_reg = known_lhs ? rt : rs;
			switch (funct)
			{
				case 0x20: // ADD
				case 0x21: // ADDU
					if (known_value == 0)
					{
#if defined(VITASX2_QEMU_VALIDATION)
						++g_qemuIopConstRegisterOperandFastPaths;
#endif
						return EmitMoveGpr(rd, dynamic_reg);
					}
					return EmitLoadGpr(dynamic_reg, HOST_TMP0) &&
						   emit_reg_imm_or_reg(known_value, emit_add_imm, emit_add_reg) &&
						   emit_counted_store();
				case 0x22: // SUB
				case 0x23: // SUBU
					if (known_lhs)
					{
						return EmitLoadGpr(dynamic_reg, HOST_TMP0) &&
							   (m_code.EmitRsbImm32(HOST_TMP2, HOST_TMP0, known_value) ||
								   (m_code.EmitMovImm32(HOST_TMP1, known_value) &&
									   m_code.EmitSubReg(HOST_TMP2, HOST_TMP1, HOST_TMP0))) &&
							   emit_counted_store();
					}
					if (known_value == 0)
					{
#if defined(VITASX2_QEMU_VALIDATION)
						++g_qemuIopConstRegisterOperandFastPaths;
#endif
						return EmitMoveGpr(rd, dynamic_reg);
					}
					return EmitLoadGpr(dynamic_reg, HOST_TMP0) &&
						   emit_reg_imm_or_reg(known_value, emit_sub_imm, emit_sub_reg) &&
						   emit_counted_store();
				case 0x24: // AND
					if (known_value == 0)
					{
#if defined(VITASX2_QEMU_VALIDATION)
						++g_qemuIopConstRegisterOperandFastPaths;
#endif
						return store_zero();
					}
					if (known_value == 0xffffffffu)
					{
#if defined(VITASX2_QEMU_VALIDATION)
						++g_qemuIopConstRegisterOperandFastPaths;
#endif
						return EmitMoveGpr(rd, dynamic_reg);
					}
					return EmitLoadGpr(dynamic_reg, HOST_TMP0) &&
						   emit_reg_imm_or_reg(known_value, emit_and_imm, emit_and_reg) &&
						   emit_counted_store();
				case 0x25: // OR
					if (known_value == 0)
					{
#if defined(VITASX2_QEMU_VALIDATION)
						++g_qemuIopConstRegisterOperandFastPaths;
#endif
						return EmitMoveGpr(rd, dynamic_reg);
					}
					if (known_value == 0xffffffffu)
					{
						return m_code.EmitMovImm32(HOST_TMP2, 0xffffffffu) &&
							   emit_counted_store();
					}
					return EmitLoadGpr(dynamic_reg, HOST_TMP0) &&
						   emit_reg_imm_or_reg(known_value, emit_orr_imm, emit_orr_reg) &&
						   emit_counted_store();
				case 0x26: // XOR
					if (known_value == 0)
					{
#if defined(VITASX2_QEMU_VALIDATION)
						++g_qemuIopConstRegisterOperandFastPaths;
#endif
						return EmitMoveGpr(rd, dynamic_reg);
					}
					return EmitLoadGpr(dynamic_reg, HOST_TMP0) &&
						   emit_reg_imm_or_reg(known_value, emit_eor_imm, emit_eor_reg) &&
						   emit_counted_store();
				case 0x27: // NOR
					if (known_value == 0xffffffffu)
					{
#if defined(VITASX2_QEMU_VALIDATION)
						++g_qemuIopConstRegisterOperandFastPaths;
#endif
						return store_zero();
					}
					if (known_value == 0)
					{
#if defined(VITASX2_QEMU_VALIDATION)
						++g_qemuIopConstRegisterOperandFastPaths;
#endif
						return store_not(dynamic_reg);
					}
					return EmitLoadGpr(dynamic_reg, HOST_TMP0) &&
						   emit_reg_imm_or_reg(known_value, emit_orr_imm, emit_orr_reg) &&
						   m_code.EmitMvnReg(HOST_TMP2, HOST_TMP2) &&
						   emit_counted_store();
				default:
					return false;
			}
		};

		// PCSX2 owners: x86/iR3000Atables.cpp::rpsxADDU_consts(),
		// rpsxSUBU_const{t,s}(), rpsxLogicalOp_constv(). Fold one known
		// operand so Cortex-A9 avoids the second psxRegs.GPR load.
		if (rs_tracked)
			return emit_known_operand(true, known_rs);
		if (rt_tracked)
			return emit_known_operand(false, known_rt);

		if (!EmitLoadGpr(rs, HOST_TMP0) || !EmitLoadGpr(rt, HOST_TMP1))
			return false;

		switch (funct)
		{
			case 0x20: // ADD
			case 0x21: // ADDU
				if (!m_code.EmitAddReg(HOST_TMP2, HOST_TMP0, HOST_TMP1))
					return false;
				break;
			case 0x22: // SUB
			case 0x23: // SUBU
				if (!m_code.EmitSubReg(HOST_TMP2, HOST_TMP0, HOST_TMP1))
					return false;
				break;
			case 0x24: // AND
				if (!m_code.EmitAndReg(HOST_TMP2, HOST_TMP0, HOST_TMP1))
					return false;
				break;
			case 0x25: // OR
				if (!m_code.EmitOrrReg(HOST_TMP2, HOST_TMP0, HOST_TMP1))
					return false;
				break;
			case 0x26: // XOR
				if (!m_code.EmitEorReg(HOST_TMP2, HOST_TMP0, HOST_TMP1))
					return false;
				break;
			case 0x27: // NOR
				if (!m_code.EmitOrrReg(HOST_TMP2, HOST_TMP0, HOST_TMP1) ||
					!m_code.EmitMvnReg(HOST_TMP2, HOST_TMP2))
				{
					return false;
				}
				break;
			default:
				return false;
		}

		return EmitStoreGpr(rd, HOST_TMP2);
	}

	bool BlockCompiler::EmitShiftImmOp(u32 op)
	{
		const unsigned rd = RD(op);
		const unsigned rt = RT(op);
		const unsigned sa = SA(op);
		const u32 funct = op & 0x3f;
		if (rd == 0)
			return true;

		VitaA32::ShiftType shift = VitaA32::ShiftType::LSL;
		switch (funct)
		{
			case 0x00: // SLL
				shift = VitaA32::ShiftType::LSL;
				break;
			case 0x02: // SRL
				shift = VitaA32::ShiftType::LSR;
				break;
			case 0x03: // SRA
				shift = VitaA32::ShiftType::ASR;
				break;
			default:
				return false;
		}

		// PCSX2 owner: R3000AOpcodeTables.cpp::psxSLL()/psxSRL()/psxSRA().
		// Shifting register zero writes zero for every immediate amount.
		if (rt == 0)
			return EmitStoreGprZero(rd);

		u32 known_rt = 0;
		if (TryGetKnownGpr(rt, &known_rt))
		{
			u32 result = 0;
			switch (shift)
			{
				case VitaA32::ShiftType::LSL:
					result = known_rt << sa;
					break;
				case VitaA32::ShiftType::LSR:
					result = known_rt >> sa;
					break;
				case VitaA32::ShiftType::ASR:
					result = static_cast<u32>(static_cast<s32>(known_rt) >> sa);
					break;
				default:
					return false;
			}

#if defined(VITASX2_QEMU_VALIDATION)
			++g_qemuIopConstShiftFastPaths;
#endif
			return ((result <= 0xffu) ? m_code.EmitMovImm8(HOST_TMP0, static_cast<u8>(result)) :
										m_code.EmitMovImm32(HOST_TMP0, result)) &&
				   EmitStoreGpr(rd, HOST_TMP0);
		}

		if (sa == 0)
			return EmitMoveGpr(rd, rt);

		if (!EmitLoadGpr(rt, HOST_TMP0))
			return false;

		return m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP0, shift, static_cast<u8>(sa)) &&
			   EmitStoreGpr(rd, HOST_TMP2);
	}

	bool BlockCompiler::EmitShiftRegOp(u32 op)
	{
		const unsigned rd = RD(op);
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		const u32 funct = op & 0x3f;
		if (rd == 0)
			return true;

		VitaA32::ShiftType shift = VitaA32::ShiftType::LSL;
		switch (funct)
		{
			case 0x04: // SLLV
				shift = VitaA32::ShiftType::LSL;
				break;
			case 0x06: // SRLV
				shift = VitaA32::ShiftType::LSR;
				break;
			case 0x07: // SRAV
				shift = VitaA32::ShiftType::ASR;
				break;
			default:
				return false;
		}

		// PCSX2 owner: R3000AOpcodeTables.cpp::psxSLLV()/psxSRLV()/psxSRAV().
		// The shift amount is irrelevant when the source register is zero.
		if (rt == 0)
			return EmitStoreGprZero(rd);

		u32 known_rs = 0;
		u32 known_rt = 0;
		if (TryGetKnownGpr(rt, &known_rt) && TryGetKnownGpr(rs, &known_rs))
		{
			const unsigned amount = known_rs & 0x1f;
			u32 result = 0;
			switch (shift)
			{
				case VitaA32::ShiftType::LSL:
					result = known_rt << amount;
					break;
				case VitaA32::ShiftType::LSR:
					result = known_rt >> amount;
					break;
				case VitaA32::ShiftType::ASR:
					result = static_cast<u32>(static_cast<s32>(known_rt) >> amount);
					break;
				default:
					return false;
			}

#if defined(VITASX2_QEMU_VALIDATION)
			++g_qemuIopConstShiftFastPaths;
#endif
			return ((result <= 0xffu) ? m_code.EmitMovImm8(HOST_TMP0, static_cast<u8>(result)) :
										m_code.EmitMovImm32(HOST_TMP0, result)) &&
				   EmitStoreGpr(rd, HOST_TMP0);
		}

		// Register zero supplies a shift amount of 0.
		if (rs == 0)
			return EmitMoveGpr(rd, rt);

		if (TryGetKnownGpr(rs, &known_rs))
		{
			const unsigned amount = known_rs & 0x1f;
#if defined(VITASX2_QEMU_VALIDATION)
			++g_qemuIopConstShiftAmountFastPaths;
#endif
			if (amount == 0)
				return EmitMoveGpr(rd, rt);
			return EmitLoadGpr(rt, HOST_TMP0) &&
				   m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP0, shift, static_cast<u8>(amount)) &&
				   EmitStoreGpr(rd, HOST_TMP2);
		}

		if (!EmitLoadGpr(rt, HOST_TMP0) ||
			!EmitLoadGpr(rs, HOST_TMP1) ||
			!m_code.EmitAndImm8(HOST_TMP1, HOST_TMP1, 0x1f))
		{
			return false;
		}

		return m_code.EmitMovRegShiftReg(HOST_TMP2, HOST_TMP0, shift, HOST_TMP1) &&
			   EmitStoreGpr(rd, HOST_TMP2);
	}

	bool BlockCompiler::EmitSetLessThanRegOp(u32 op, bool is_signed)
	{
		const unsigned rd = RD(op);
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		if (rd == 0)
			return true;

		u32 known_rs = 0;
		u32 known_rt = 0;
		if (TryGetKnownGpr(rs, &known_rs) && TryGetKnownGpr(rt, &known_rt))
		{
			// PCSX2 owners: R3000AOpcodeTables.cpp::psxSLT()/psxSLTU() and
			// x86/iR3000Atables.cpp::rpsxSLT_const()/rpsxSLTU_const().
			const u32 result = is_signed ?
				((static_cast<s32>(known_rs) < static_cast<s32>(known_rt)) ? 1u : 0u) :
				((known_rs < known_rt) ? 1u : 0u);
#if defined(VITASX2_QEMU_VALIDATION)
			++g_qemuIopConstRegisterOpFastPaths;
#endif
			return m_code.EmitMovImm8(HOST_TMP0, static_cast<u8>(result)) &&
				   EmitStoreGpr(rd, HOST_TMP0);
		}

		// PCSX2 owner: R3000AOpcodeTables.cpp::psxSLT()/psxSLTU(). Fold
		// architectural-zero compare identities before loading both operands.
		if (rs == rt)
			return EmitStoreGprZero(rd);

		if (is_signed)
		{
			if (rs == 0)
			{
				return EmitLoadGpr(rt, HOST_TMP0) &&
					   m_code.EmitCmpImm32(HOST_TMP0, 0) &&
					   m_code.EmitMovImm8(HOST_TMP2, 0) &&
					   m_code.EmitMovImm8(HOST_TMP2, 1, VitaA32::Condition::GT) &&
					   EmitStoreGpr(rd, HOST_TMP2);
			}

			if (rt == 0)
			{
				// PCSX2 owner: R3000AOpcodeTables.cpp::psxSLT(). Comparing a
				// signed 32-bit value with $zero is just its sign bit.
				return EmitLoadGpr(rs, HOST_TMP0) &&
					   m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP0, VitaA32::ShiftType::LSR, 31) &&
					   EmitStoreGpr(rd, HOST_TMP2);
			}
		}
		else
		{
			if (rt == 0)
				return EmitStoreGprZero(rd);

			if (rs == 0)
			{
				return EmitLoadGpr(rt, HOST_TMP0) &&
					   m_code.EmitCmpImm32(HOST_TMP0, 0) &&
					   m_code.EmitMovImm8(HOST_TMP2, 0) &&
					   m_code.EmitMovImm8(HOST_TMP2, 1, VitaA32::Condition::NE) &&
					   EmitStoreGpr(rd, HOST_TMP2);
			}
		}

		const bool rs_tracked = rs != 0 && TryGetKnownGpr(rs, &known_rs);
		const bool rt_tracked = rt != 0 && TryGetKnownGpr(rt, &known_rt);
		const auto emit_cmp_reg_imm = [this](unsigned host_reg, u32 value) {
			return m_code.EmitCmpImm32(host_reg, value) ||
				   (m_code.EmitMovImm32(HOST_TMP1, value) &&
					   m_code.EmitCmpReg(host_reg, HOST_TMP1));
		};
		if (rs_tracked || rt_tracked)
		{
			// PCSX2 owners: x86/iR3000Atables.cpp::rpsxSLT_consts() /
			// rpsxSLT_constt() and rpsxSLTU_consts()/rpsxSLTU_constt().
			const bool known_lhs = rs_tracked;
			const u32 known_value = known_lhs ? known_rs : known_rt;
			const unsigned dynamic_reg = known_lhs ? rt : rs;
			const VitaA32::Condition set_condition = known_lhs ?
				(is_signed ? VitaA32::Condition::GT : VitaA32::Condition::HI) :
				(is_signed ? VitaA32::Condition::LT : VitaA32::Condition::CC);
#if defined(VITASX2_QEMU_VALIDATION)
			++g_qemuIopConstRegisterOperandFastPaths;
#endif
			return EmitLoadGpr(dynamic_reg, HOST_TMP0) &&
				   emit_cmp_reg_imm(HOST_TMP0, known_value) &&
				   m_code.EmitMovImm8(HOST_TMP2, 0) &&
				   m_code.EmitMovImm8(HOST_TMP2, 1, set_condition) &&
				   EmitStoreGpr(rd, HOST_TMP2);
		}

		return EmitLoadGpr(rs, HOST_TMP0) &&
			   EmitLoadGpr(rt, HOST_TMP1) &&
			   m_code.EmitCmpReg(HOST_TMP0, HOST_TMP1) &&
			   m_code.EmitMovImm8(HOST_TMP2, 0) &&
			   m_code.EmitMovImm8(HOST_TMP2, 1, is_signed ? VitaA32::Condition::LT : VitaA32::Condition::CC) &&
			   EmitStoreGpr(rd, HOST_TMP2);
	}

	bool BlockCompiler::EmitMultiplyOp(u32 op, bool is_signed)
	{
		u32 known_rs = 0;
		u32 known_rt = 0;
		if (TryGetKnownGpr(RS(op), &known_rs) && TryGetKnownGpr(RT(op), &known_rt))
		{
			// PCSX2 owners: R3000AOpcodeTables.cpp::psxMULT()/psxMULTU()
			// and x86/iR3000Atables.cpp::rpsxMULT_const()/rpsxMULTU_const().
			u32 lo = 0;
			u32 hi = 0;
			ComputeIopMultiplyResult(known_rs, known_rt, is_signed, &lo, &hi);
#if defined(VITASX2_QEMU_VALIDATION)
			++g_qemuIopConstMultiplyFastPaths;
#endif
			return m_code.EmitMovImm32(HOST_TMP2, lo) &&
				   m_code.EmitMovImm32(HOST_TMP3, hi) &&
				   m_code.EmitStrImm12(HOST_TMP2, HOST_PSX_REGS, static_cast<u16>(LO_OFFSET)) &&
				   m_code.EmitStrImm12(HOST_TMP3, HOST_PSX_REGS, static_cast<u16>(HI_OFFSET));
		}

		const bool rs_known = TryGetKnownGpr(RS(op), &known_rs);
		const bool rt_known = TryGetKnownGpr(RT(op), &known_rt);
		if (rs_known || rt_known)
		{
			// PCSX2 owners: x86/iR3000Atables.cpp::rpsxMULT_consts() /
			// rpsxMULT_constt() and rpsxMULTU_consts()/rpsxMULTU_constt().
			const u32 known_value = rs_known ? known_rs : known_rt;
			const unsigned dynamic_reg = rs_known ? RT(op) : RS(op);
#if defined(VITASX2_QEMU_VALIDATION)
			++g_qemuIopConstMultiplyOperandFastPaths;
#endif
			if (known_value == 0)
			{
				return m_code.EmitMovImm8(HOST_TMP2, 0) &&
					   m_code.EmitStrImm12(HOST_TMP2, HOST_PSX_REGS, static_cast<u16>(LO_OFFSET)) &&
					   m_code.EmitStrImm12(HOST_TMP2, HOST_PSX_REGS, static_cast<u16>(HI_OFFSET));
			}

			if (!EmitLoadGpr(dynamic_reg, HOST_TMP0) ||
				!m_code.EmitMovImm32(HOST_TMP1, known_value))
			{
				return false;
			}

			if (is_signed)
			{
				if (!m_code.EmitSmull(HOST_TMP2, HOST_TMP3, HOST_TMP0, HOST_TMP1))
					return false;
			}
			else
			{
				if (!m_code.EmitUmull(HOST_TMP2, HOST_TMP3, HOST_TMP0, HOST_TMP1))
					return false;
			}

			return m_code.EmitStrImm12(HOST_TMP2, HOST_PSX_REGS, static_cast<u16>(LO_OFFSET)) &&
				   m_code.EmitStrImm12(HOST_TMP3, HOST_PSX_REGS, static_cast<u16>(HI_OFFSET));
		}

		if (!EmitLoadGpr(RS(op), HOST_TMP0) ||
			!EmitLoadGpr(RT(op), HOST_TMP1))
		{
			return false;
		}

		if (is_signed)
		{
			if (!m_code.EmitSmull(HOST_TMP2, HOST_TMP3, HOST_TMP0, HOST_TMP1))
				return false;
		}
		else
		{
			if (!m_code.EmitUmull(HOST_TMP2, HOST_TMP3, HOST_TMP0, HOST_TMP1))
				return false;
		}

		return m_code.EmitStrImm12(HOST_TMP2, HOST_PSX_REGS, static_cast<u16>(LO_OFFSET)) &&
			   m_code.EmitStrImm12(HOST_TMP3, HOST_PSX_REGS, static_cast<u16>(HI_OFFSET));
	}

	bool BlockCompiler::EmitDivideOp(u32 op, bool is_signed)
	{
		struct BranchPatch
		{
			size_t offset = static_cast<size_t>(-1);
			VitaA32::Condition condition = VitaA32::Condition::AL;
		};

		const auto emit_branch = [this](BranchPatch& patch, VitaA32::Condition condition) {
			patch.offset = m_code.EmitBranchPlaceholder(condition);
			patch.condition = condition;
			return patch.offset != static_cast<size_t>(-1);
		};

		const auto patch_branch = [this](const BranchPatch& patch, size_t target) {
			return m_code.PatchBranch(patch.offset, target, patch.condition);
		};

		const auto patch_branches = [patch_branch](const BranchPatch* branches, unsigned count, size_t target) {
			for (unsigned i = 0; i < count; i++)
			{
				if (!patch_branch(branches[i], target))
					return false;
			}
			return true;
		};

		const auto store_hilo = [this](unsigned lo_reg, unsigned hi_reg) {
			return m_code.EmitStrImm12(lo_reg, HOST_PSX_REGS, static_cast<u16>(LO_OFFSET)) &&
				   m_code.EmitStrImm12(hi_reg, HOST_PSX_REGS, static_cast<u16>(HI_OFFSET));
		};
		const auto store_hilo_from_hi_lo_pair = [this]() {
			// PCSX2 owner: R3000A.h::GPRRegs lays out HI then LO; A32 STRD can
			// write the adjacent pair when HOST_TMP2=HI and HOST_TMP3=LO.
			return m_code.EmitStrdImm8(HOST_TMP2, HOST_TMP3, HOST_PSX_REGS, static_cast<u8>(HI_OFFSET));
		};
		const auto emit_and_mask = [this](unsigned rd, unsigned rn, u32 mask) {
			if (m_code.EmitAndImm32(rd, rn, mask))
				return true;
			return m_code.EmitMovImm32(HOST_TMP1, mask) &&
				   m_code.EmitAndReg(rd, rn, HOST_TMP1);
		};

		const void* helper = is_signed ?
			reinterpret_cast<const void*>(&VitaIopA32DivResult) :
			reinterpret_cast<const void*>(&VitaIopA32DivuResult);

		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		u32 known_rs = 0;
		u32 known_rt = 0;
		const bool rs_known = TryGetKnownGpr(rs, &known_rs);
		const bool rt_known = TryGetKnownGpr(rt, &known_rt);
		if (rs_known && rt_known)
		{
			// PCSX2 owners: R3000AOpcodeTables.cpp::psxDIV()/psxDIVU() and
			// x86/iR3000Atables.cpp::rpsxDIV_const()/rpsxDIVU_const().
			u32 lo = 0;
			u32 hi = 0;
			ComputeIopDivideResult(known_rs, known_rt, is_signed, &lo, &hi);

#if defined(VITASX2_QEMU_VALIDATION)
			++g_qemuIopConstDivideFastPaths;
#endif
			return m_code.EmitMovImm32(HOST_TMP2, lo) &&
				   m_code.EmitMovImm32(HOST_TMP3, hi) &&
				   m_code.EmitStrImm12(HOST_TMP2, HOST_PSX_REGS, static_cast<u16>(LO_OFFSET)) &&
				   m_code.EmitStrImm12(HOST_TMP3, HOST_PSX_REGS, static_cast<u16>(HI_OFFSET));
		}

		// PCSX2 owners: x86/iR3000Atables.cpp::rpsxDIV_consts() /
		// rpsxDIV_constt() / rpsxDIVU_consts() / rpsxDIVU_constt(). Keep the
		// Cortex-A9 off the full divide branch/helper tree for one-known
		// operands whose quotient/remainder needs only the dynamic register.
		if (rt_known)
		{
			if (known_rt == 0)
			{
#if defined(VITASX2_QEMU_VALIDATION)
				++g_qemuIopConstDivideOperandFastPaths;
#endif
				if (!EmitLoadGpr(rs, HOST_TMP0) ||
					!m_code.EmitMovImm32(HOST_TMP2, 0xffffffffu))
				{
					return false;
				}

				if (is_signed &&
					(!m_code.EmitCmpImm32(HOST_TMP0, 0) ||
						!m_code.EmitMovImm8(HOST_TMP2, 1, VitaA32::Condition::LT)))
				{
					return false;
				}

				return store_hilo(HOST_TMP2, HOST_TMP0);
			}

			if (known_rt == 1)
			{
#if defined(VITASX2_QEMU_VALIDATION)
				++g_qemuIopConstDivideOperandFastPaths;
#endif
				return EmitLoadGpr(rs, HOST_TMP0) &&
					   m_code.EmitMovImm8(HOST_TMP2, 0) &&
					   store_hilo(HOST_TMP0, HOST_TMP2);
			}

			if (is_signed && known_rt == 0xffffffffu)
			{
#if defined(VITASX2_QEMU_VALIDATION)
				++g_qemuIopConstDivideOperandFastPaths;
#endif
				return EmitLoadGpr(rs, HOST_TMP0) &&
					   m_code.EmitMovImm8(HOST_TMP2, 0) &&
					   m_code.EmitRsbImm32(HOST_TMP3, HOST_TMP0, 0) &&
					   store_hilo_from_hi_lo_pair();
			}

			const bool signed_negative_power_of_two =
				is_signed && static_cast<s32>(known_rt) < 0 && known_rt != 0x80000000u &&
				IsPowerOfTwo(0u - known_rt);
			if ((!is_signed && IsPowerOfTwo(known_rt)) ||
				(is_signed && static_cast<s32>(known_rt) > 0 && IsPowerOfTwo(known_rt)) ||
				signed_negative_power_of_two)
			{
#if defined(VITASX2_QEMU_VALIDATION)
				++g_qemuIopConstDivideOperandFastPaths;
#endif
				const u32 positive_divisor = signed_negative_power_of_two ? (0u - known_rt) : known_rt;
				const unsigned shift = PowerOfTwoShift(positive_divisor);
				const u32 mask = positive_divisor - 1;
				if (!EmitLoadGpr(rs, HOST_TMP0))
					return false;

				if (!is_signed)
				{
					return m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP0, VitaA32::ShiftType::LSR,
							   static_cast<u8>(shift)) &&
						   emit_and_mask(HOST_TMP3, HOST_TMP0, mask) &&
						   store_hilo(HOST_TMP2, HOST_TMP3);
				}

				return m_code.EmitMovRegShiftImm(HOST_TMP3, HOST_TMP0, VitaA32::ShiftType::ASR, 31) &&
					   emit_and_mask(HOST_TMP3, HOST_TMP3, mask) &&
					   m_code.EmitAddReg(HOST_TMP3, HOST_TMP0, HOST_TMP3) &&
					   m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP3, VitaA32::ShiftType::ASR,
						   static_cast<u8>(shift)) &&
					   m_code.EmitMovRegShiftImm(HOST_TMP3, HOST_TMP2, VitaA32::ShiftType::LSL,
						   static_cast<u8>(shift)) &&
					   m_code.EmitSubReg(HOST_TMP3, HOST_TMP0, HOST_TMP3) &&
					   (!signed_negative_power_of_two ||
						   m_code.EmitRsbImm32(HOST_TMP2, HOST_TMP2, 0)) &&
					   store_hilo(HOST_TMP2, HOST_TMP3);
			}
		}

		if (rs_known && known_rs == 0)
		{
#if defined(VITASX2_QEMU_VALIDATION)
			++g_qemuIopConstDivideOperandFastPaths;
#endif
			return EmitLoadGpr(rt, HOST_TMP1) &&
				   m_code.EmitMovImm8(HOST_TMP2, 0) &&
				   m_code.EmitMovImm8(HOST_TMP3, 0) &&
				   m_code.EmitCmpImm32(HOST_TMP1, 0) &&
				   m_code.EmitMovImm32(HOST_TMP2, 0xffffffffu, VitaA32::Condition::EQ) &&
				   store_hilo(HOST_TMP2, HOST_TMP3);
		}

		if (!EmitLoadGpr(rs, HOST_TMP0) ||
			!EmitLoadGpr(rt, HOST_TMP1))
		{
			return false;
		}

		BranchPatch divzero_branch{};
		BranchPatch zero_branch{};
		BranchPatch divone_branch{};
		BranchPatch negone_branch{};
		BranchPatch equal_branch{};
		BranchPatch unsigned_less_branch{};
		BranchPatch non_positive_fallback_branch{};
		BranchPatch power_of_two_branch{};
		BranchPatch fallback_branch{};
		BranchPatch done_branches[7]{};
		unsigned done_branch_count = 0;

		if (!m_code.EmitCmpImm32(HOST_TMP1, 0) ||
			!emit_branch(divzero_branch, VitaA32::Condition::EQ) ||
			!m_code.EmitCmpImm32(HOST_TMP0, 0) ||
			!emit_branch(zero_branch, VitaA32::Condition::EQ) ||
			!m_code.EmitCmpImm32(HOST_TMP1, 1) ||
			!emit_branch(divone_branch, VitaA32::Condition::EQ))
		{
			return false;
		}

		if (is_signed)
		{
			if (!m_code.EmitCmpImm32(HOST_TMP1, 0xffffffffu) ||
				!emit_branch(negone_branch, VitaA32::Condition::EQ) ||
				!m_code.EmitCmpReg(HOST_TMP0, HOST_TMP1) ||
				!emit_branch(equal_branch, VitaA32::Condition::EQ) ||
				!m_code.EmitCmpImm32(HOST_TMP1, 0) ||
				!emit_branch(non_positive_fallback_branch, VitaA32::Condition::LE) ||
				!m_code.EmitSubImm8(HOST_TMP2, HOST_TMP1, 1) ||
				!m_code.EmitAndReg(HOST_TMP2, HOST_TMP1, HOST_TMP2, true) ||
				!emit_branch(power_of_two_branch, VitaA32::Condition::EQ))
			{
				return false;
			}
		}
		else
		{
			if (!m_code.EmitCmpReg(HOST_TMP0, HOST_TMP1) ||
				!emit_branch(unsigned_less_branch, VitaA32::Condition::CC) ||
				!emit_branch(equal_branch, VitaA32::Condition::EQ) ||
				!m_code.EmitSubImm8(HOST_TMP2, HOST_TMP1, 1) ||
				!m_code.EmitAndReg(HOST_TMP2, HOST_TMP1, HOST_TMP2, true) ||
				!emit_branch(power_of_two_branch, VitaA32::Condition::EQ))
			{
				return false;
			}
		}

		if (!emit_branch(fallback_branch, VitaA32::Condition::AL))
			return false;

		// PCSX2 owner: R3000AOpcodeTables.cpp::psxDIV()/psxDIVU().
		if (!patch_branch(divzero_branch, m_code.Size()) ||
			!m_code.EmitMovImm32(HOST_TMP2, 0xffffffffu))
		{
			return false;
		}

		if (is_signed &&
			(!m_code.EmitCmpImm32(HOST_TMP0, 0) ||
				!m_code.EmitMovImm8(HOST_TMP2, 1, VitaA32::Condition::LT)))
		{
			return false;
		}

		if (!store_hilo(HOST_TMP2, HOST_TMP0) ||
			!emit_branch(done_branches[done_branch_count++], VitaA32::Condition::AL))
		{
			return false;
		}

		if (!patch_branch(zero_branch, m_code.Size()) ||
			!m_code.EmitMovImm8(HOST_TMP2, 0) ||
			!store_hilo(HOST_TMP2, HOST_TMP2) ||
			!emit_branch(done_branches[done_branch_count++], VitaA32::Condition::AL))
		{
			return false;
		}

		if (!patch_branch(divone_branch, m_code.Size()) ||
			!m_code.EmitMovImm8(HOST_TMP2, 0) ||
			!store_hilo(HOST_TMP0, HOST_TMP2) ||
			!emit_branch(done_branches[done_branch_count++], VitaA32::Condition::AL))
		{
			return false;
		}

		if (is_signed)
		{
			if (!patch_branch(negone_branch, m_code.Size()) ||
				!m_code.EmitMovImm8(HOST_TMP2, 0) ||
				!m_code.EmitRsbImm32(HOST_TMP3, HOST_TMP0, 0) ||
				!store_hilo_from_hi_lo_pair() ||
				!emit_branch(done_branches[done_branch_count++], VitaA32::Condition::AL) ||
				!patch_branch(equal_branch, m_code.Size()) ||
				!m_code.EmitMovImm8(HOST_TMP2, 0) ||
				!m_code.EmitMovImm8(HOST_TMP3, 1) ||
				!store_hilo_from_hi_lo_pair() ||
				!emit_branch(done_branches[done_branch_count++], VitaA32::Condition::AL) ||
				!patch_branch(power_of_two_branch, m_code.Size()) ||
				!m_code.EmitMovRegShiftImm(HOST_TMP3, HOST_TMP0, VitaA32::ShiftType::ASR, 31) ||
				!m_code.EmitSubImm8(HOST_TMP2, HOST_TMP1, 1) ||
				!m_code.EmitAndReg(HOST_TMP3, HOST_TMP3, HOST_TMP2) ||
				!m_code.EmitAddReg(HOST_TMP3, HOST_TMP0, HOST_TMP3) ||
				!m_code.EmitClz(HOST_SAVED0, HOST_TMP1) ||
				!m_code.EmitRsbImm32(HOST_TMP2, HOST_SAVED0, 31) ||
				!m_code.EmitMovRegShiftReg(HOST_SAVED0, HOST_TMP3, VitaA32::ShiftType::ASR, HOST_TMP2) ||
				!m_code.EmitMovRegShiftReg(HOST_TMP3, HOST_SAVED0, VitaA32::ShiftType::LSL, HOST_TMP2) ||
				!m_code.EmitSubReg(HOST_TMP3, HOST_TMP0, HOST_TMP3) ||
				!store_hilo(HOST_SAVED0, HOST_TMP3) ||
				!emit_branch(done_branches[done_branch_count++], VitaA32::Condition::AL))
			{
				return false;
			}
		}
		else
		{
			if (!patch_branch(unsigned_less_branch, m_code.Size()) ||
				!m_code.EmitMovImm8(HOST_TMP2, 0) ||
				!store_hilo(HOST_TMP2, HOST_TMP0) ||
				!emit_branch(done_branches[done_branch_count++], VitaA32::Condition::AL) ||
				!patch_branch(equal_branch, m_code.Size()) ||
				!m_code.EmitMovImm8(HOST_TMP2, 0) ||
				!m_code.EmitMovImm8(HOST_TMP3, 1) ||
				!store_hilo_from_hi_lo_pair() ||
				!emit_branch(done_branches[done_branch_count++], VitaA32::Condition::AL) ||
				!patch_branch(power_of_two_branch, m_code.Size()) ||
				!m_code.EmitSubImm8(HOST_TMP2, HOST_TMP1, 1) ||
				!m_code.EmitAndReg(HOST_TMP3, HOST_TMP0, HOST_TMP2) ||
				!m_code.EmitClz(HOST_TMP2, HOST_TMP1) ||
				!m_code.EmitRsbImm32(HOST_SAVED0, HOST_TMP2, 31) ||
				!m_code.EmitMovRegShiftReg(HOST_TMP2, HOST_TMP0, VitaA32::ShiftType::LSR, HOST_SAVED0) ||
				!store_hilo(HOST_TMP2, HOST_TMP3) ||
				!emit_branch(done_branches[done_branch_count++], VitaA32::Condition::AL))
			{
				return false;
			}
		}

		const size_t fallback_target = m_code.Size();
		if (!patch_branch(fallback_branch, fallback_target) ||
			(is_signed && !patch_branch(non_positive_fallback_branch, fallback_target)) ||
			!m_code.EmitCallAbsolute(helper, HOST_CALL_SCRATCH) ||
			!store_hilo(HOST_TMP0, HOST_TMP1))
		{
			return false;
		}

		return patch_branches(done_branches, done_branch_count, m_code.Size());
	}

	bool BlockCompiler::EmitExceptionOp(u32 pc, u32 code)
	{
		return m_code.EmitMovImm32(HOST_TMP0, pc) &&
			   m_code.EmitMovImm32(HOST_TMP1, code) &&
			   m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&VitaIopA32RaiseException), HOST_CALL_SCRATCH) &&
			   EndBlockReturn(BlockExitKind::Direct);
	}

	bool BlockCompiler::EmitImmediateOp(u32 op)
	{
		const unsigned opcode = op >> 26;
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		if (rt == 0)
			return true;

		if (opcode == 0x0f) // LUI
		{
			return m_code.EmitMovImm32(HOST_TMP0, op << 16) &&
				   EmitStoreGpr(rt, HOST_TMP0);
		}

		u32 known_rs = 0;
		if (rs != 0 && TryGetKnownGpr(rs, &known_rs))
		{
			u32 result = 0;
			bool can_fold = true;
			switch (opcode)
			{
				case 0x08: // ADDI
				case 0x09: // ADDIU
					// PCSX2 owners: R3000AOpcodeTables.cpp::psxADDI() /
					// psxADDIU() and x86/iR3000Atables.cpp::rpsxADDIU_const().
					// This tree's IOP ADDI is wrapping, so a known source can be
					// folded without emitting a Cortex-A9 load/add pair.
					result = known_rs + static_cast<u32>(static_cast<s32>(IMM_S(op)));
					break;
				case 0x0a: // SLTI
					// PCSX2 owner: R3000AOpcodeTables.cpp::psxSLTI().
					result = (static_cast<s32>(known_rs) < static_cast<s32>(IMM_S(op))) ? 1u : 0u;
					break;
				case 0x0b: // SLTIU
					// PCSX2 owner: R3000AOpcodeTables.cpp::psxSLTIU().
					result = (known_rs < static_cast<u32>(static_cast<s32>(IMM_S(op)))) ? 1u : 0u;
					break;
				case 0x0c: // ANDI
					// PCSX2 owners: R3000AOpcodeTables.cpp::psxANDI() and
					// x86/iR3000Atables.cpp::rpsxANDI_const().
					result = known_rs & IMM_U(op);
					break;
				case 0x0d: // ORI
					// PCSX2 owners: R3000AOpcodeTables.cpp::psxORI() and
					// x86/iR3000Atables.cpp::rpsxORI_const().
					result = known_rs | IMM_U(op);
					break;
				case 0x0e: // XORI
					// PCSX2 owners: R3000AOpcodeTables.cpp::psxXORI() and
					// x86/iR3000Atables.cpp::rpsxXORI_const().
					result = known_rs ^ IMM_U(op);
					break;
				default:
					can_fold = false;
					break;
			}

			if (can_fold)
			{
#if defined(VITASX2_QEMU_VALIDATION)
				++g_qemuIopConstImmediateFastPaths;
#endif
				return ((result <= 0xffu) ? m_code.EmitMovImm8(HOST_TMP0, static_cast<u8>(result)) :
											m_code.EmitMovImm32(HOST_TMP0, result)) &&
					   EmitStoreGpr(rt, HOST_TMP0);
			}
		}

		if ((opcode == 0x08 || opcode == 0x09) && IMM_S(op) == 0)
			return EmitMoveGpr(rt, rs);
		if ((opcode == 0x08 || opcode == 0x09) && rs == 0)
		{
			// PCSX2 owner: x86/iR3000Atables.cpp::rpsxADDI() reuses rpsxADDIU().
			// The architectural zero register lets the Vita template skip the
			// otherwise-dead load/add pair and materialize the sign-extended
			// immediate directly.
			return m_code.EmitMovImm32(HOST_TMP0, static_cast<u32>(static_cast<s32>(IMM_S(op)))) &&
				   EmitStoreGpr(rt, HOST_TMP0);
		}

		// PCSX2 owner: x86/iR3000Atables.cpp::rpsxLogicalOpI(). Preserve the
		// same zero/no-op folds before loading rs into a host register.
		const u16 logical_imm = IMM_U(op);
		if (opcode == 0x0c && (logical_imm == 0 || rs == 0)) // ANDI
		{
			return m_code.EmitMovImm8(HOST_TMP0, 0) &&
				   EmitStoreGpr(rt, HOST_TMP0);
		}
		if ((opcode == 0x0d || opcode == 0x0e) && logical_imm == 0) // ORI/XORI
			return EmitMoveGpr(rt, rs);
		if ((opcode == 0x0d || opcode == 0x0e) && rs == 0) // ORI/XORI
		{
			return m_code.EmitMovImm32(HOST_TMP0, logical_imm) &&
				   EmitStoreGpr(rt, HOST_TMP0);
		}
		if (opcode == 0x0a && rs == 0) // SLTI
		{
			// PCSX2 owner: R3000AOpcodeTables.cpp::psxSLTI(). With the
			// architectural zero register, the signed predicate is constant.
			const u8 result = (IMM_S(op) > 0) ? 1 : 0;
			return m_code.EmitMovImm8(HOST_TMP0, result) &&
				   EmitStoreGpr(rt, HOST_TMP0);
		}
		if (opcode == 0x0b && rs == 0) // SLTIU
		{
			// PCSX2 owner: R3000AOpcodeTables.cpp::psxSLTIU(). The immediate is
			// sign-extended before the unsigned compare, so 0 < imm iff imm != 0.
			const u8 result = (IMM_S(op) != 0) ? 1 : 0;
			return m_code.EmitMovImm8(HOST_TMP0, result) &&
				   EmitStoreGpr(rt, HOST_TMP0);
		}

		if (!EmitLoadGpr(rs, HOST_TMP0))
			return false;

		switch (opcode)
		{
			case 0x08: // ADDI
			case 0x09: // ADDIU
			{
				// PCSX2 owner: x86/iR3000Atables.cpp::rpsxADDI() reuses rpsxADDIU().
				const s32 imm = static_cast<s32>(IMM_S(op));
				if (imm == 0)
					return EmitStoreGpr(rt, HOST_TMP0);
				if (imm > 0 && m_code.EmitAddImm32(HOST_TMP2, HOST_TMP0, static_cast<u32>(imm)))
					return EmitStoreGpr(rt, HOST_TMP2);
				if (imm < 0 && m_code.EmitSubImm32(HOST_TMP2, HOST_TMP0, static_cast<u32>(-imm)))
					return EmitStoreGpr(rt, HOST_TMP2);
				return m_code.EmitMovImm32(HOST_TMP1, static_cast<u32>(imm)) &&
					   m_code.EmitAddReg(HOST_TMP2, HOST_TMP0, HOST_TMP1) &&
					   EmitStoreGpr(rt, HOST_TMP2);
			}
			case 0x0a: // SLTI
			{
				const u32 imm = static_cast<u32>(static_cast<s32>(IMM_S(op)));
				if (!(m_code.EmitCmpImm32(HOST_TMP0, imm) ||
					  (m_code.EmitMovImm32(HOST_TMP1, imm) && m_code.EmitCmpReg(HOST_TMP0, HOST_TMP1))))
				{
					return false;
				}
				return m_code.EmitMovImm8(HOST_TMP2, 0) &&
					   m_code.EmitMovImm8(HOST_TMP2, 1, VitaA32::Condition::LT) &&
					   EmitStoreGpr(rt, HOST_TMP2);
			}
			case 0x0b: // SLTIU
			{
				const u32 imm = static_cast<u32>(static_cast<s32>(IMM_S(op)));
				if (!(m_code.EmitCmpImm32(HOST_TMP0, imm) ||
					  (m_code.EmitMovImm32(HOST_TMP1, imm) && m_code.EmitCmpReg(HOST_TMP0, HOST_TMP1))))
				{
					return false;
				}
				return m_code.EmitMovImm8(HOST_TMP2, 0) &&
					   m_code.EmitMovImm8(HOST_TMP2, 1, VitaA32::Condition::CC) &&
					   EmitStoreGpr(rt, HOST_TMP2);
			}
			case 0x0c: // ANDI
				return (m_code.EmitAndImm32(HOST_TMP2, HOST_TMP0, IMM_U(op)) ||
						  (m_code.EmitMovImm32(HOST_TMP1, IMM_U(op)) &&
						   m_code.EmitAndReg(HOST_TMP2, HOST_TMP0, HOST_TMP1))) &&
					   EmitStoreGpr(rt, HOST_TMP2);
			case 0x0d: // ORI
				return (m_code.EmitOrrImm32(HOST_TMP2, HOST_TMP0, IMM_U(op)) ||
						  (m_code.EmitMovImm32(HOST_TMP1, IMM_U(op)) &&
						   m_code.EmitOrrReg(HOST_TMP2, HOST_TMP0, HOST_TMP1))) &&
					   EmitStoreGpr(rt, HOST_TMP2);
			case 0x0e: // XORI
				return (m_code.EmitEorImm32(HOST_TMP2, HOST_TMP0, IMM_U(op)) ||
						  (m_code.EmitMovImm32(HOST_TMP1, IMM_U(op)) &&
						   m_code.EmitEorReg(HOST_TMP2, HOST_TMP0, HOST_TMP1))) &&
					   EmitStoreGpr(rt, HOST_TMP2);
			default:
				return false;
		}
	}

	bool BlockCompiler::EmitEffectiveAddress(u32 op)
	{
		return EmitEffectiveAddress(op, HOST_TMP0);
	}

	bool BlockCompiler::EmitEffectiveAddress(u32 op, unsigned host_reg)
	{
		const s32 imm = static_cast<s32>(IMM_S(op));
		const unsigned scratch_reg = (host_reg == HOST_TMP1) ? HOST_TMP2 : HOST_TMP1;
		u32 known_base = 0;
		if (RS(op) == 0)
			return m_code.EmitMovImm32(host_reg, static_cast<u32>(imm));
		if (TryGetKnownGpr(RS(op), &known_base))
			return m_code.EmitMovImm32(host_reg, known_base + static_cast<u32>(imm));

		if (!EmitLoadGpr(RS(op), host_reg))
			return false;

		if (imm == 0)
			return true;

		if (imm > 0 && m_code.EmitAddImm32(host_reg, host_reg, static_cast<u32>(imm)))
			return true;
		if (imm < 0 && m_code.EmitSubImm32(host_reg, host_reg, static_cast<u32>(-imm)))
			return true;

		return m_code.EmitMovImm32(scratch_reg, static_cast<u32>(imm)) &&
			   m_code.EmitAddReg(host_reg, host_reg, scratch_reg);
	}

	bool BlockCompiler::EmitKnownDirectRamLoadOp(u32 op, u32 address)
	{
		const unsigned opcode = op >> 26;
		const unsigned rt = RT(op);
#if defined(VITASX2_QEMU_VALIDATION)
		++g_qemuIopKnownRamScalarLoadFastPaths;
		if (RS(op) != 0)
			++g_qemuIopConstRamScalarLoadFastPaths;
#endif

		if (rt == 0)
			return true;

		// PCSX2 owner: x86/iR3000Atables.cpp::rpsxLoad() reads ordinary IOP
		// RAM directly through iopMem->Main. When the block-local address
		// tracker proves an aligned main-RAM address, the MMIO/ROM helper split
		// is impossible.
		const auto emit_load_value = [&]() -> bool {
			switch (opcode)
			{
				case 0x24: // LBU
					if (address <= 0x0fffu)
						return m_code.EmitLdrbImm12(HOST_TMP0, HOST_IOP_RAM_BASE, static_cast<u16>(address));
					return m_code.EmitMovImm32(HOST_TMP0, address) &&
						   m_code.EmitLdrbRegShift(HOST_TMP0, HOST_IOP_RAM_BASE, HOST_TMP0,
							   VitaA32::ShiftType::LSL, 0);
				case 0x20: // LB
					if (address <= 0xffu)
						return m_code.EmitLdrsbImm8(HOST_TMP0, HOST_IOP_RAM_BASE, static_cast<u8>(address));
					return m_code.EmitMovImm32(HOST_TMP0, address) &&
						   m_code.EmitLdrsbReg(HOST_TMP0, HOST_IOP_RAM_BASE, HOST_TMP0);
				case 0x21: // LH
					if (address <= 0xffu)
						return m_code.EmitLdrshImm8(HOST_TMP0, HOST_IOP_RAM_BASE, static_cast<u8>(address));
					return m_code.EmitMovImm32(HOST_TMP0, address) &&
						   m_code.EmitLdrshReg(HOST_TMP0, HOST_IOP_RAM_BASE, HOST_TMP0);
				case 0x25: // LHU
					if (address <= 0xffu)
						return m_code.EmitLdrhImm8(HOST_TMP0, HOST_IOP_RAM_BASE, static_cast<u8>(address));
					return m_code.EmitMovImm32(HOST_TMP0, address) &&
						   m_code.EmitLdrhReg(HOST_TMP0, HOST_IOP_RAM_BASE, HOST_TMP0);
				case 0x23: // LW
					if (address <= 0x0fffu)
						return m_code.EmitLdrImm12(HOST_TMP0, HOST_IOP_RAM_BASE, static_cast<u16>(address));
					return m_code.EmitMovImm32(HOST_TMP0, address) &&
						   m_code.EmitLdrRegShift(HOST_TMP0, HOST_IOP_RAM_BASE, HOST_TMP0,
							   VitaA32::ShiftType::LSL, 0);
				default:
					return false;
			}
		};

		return emit_load_value() && EmitStoreGpr(rt, HOST_TMP0);
	}

	bool BlockCompiler::EmitLoadOp(u32 op)
	{
		const unsigned opcode = op >> 26;
		const unsigned rt = RT(op);
		const void* helper = nullptr;
		u8 alignment_mask = 0;

		switch (opcode)
		{
			case 0x20: // LB
			case 0x24: // LBU
				helper = reinterpret_cast<const void*>(&iopMemRead8);
				break;
			case 0x21: // LH
			case 0x25: // LHU
				helper = reinterpret_cast<const void*>(&iopMemRead16);
				alignment_mask = 1;
				break;
			case 0x23: // LW
				helper = reinterpret_cast<const void*>(&iopMemRead32);
				alignment_mask = 3;
				break;
			default:
				return false;
		}

		u32 known_ram_address = 0;
		if (TryKnownDirectIopRamAddress(op, alignment_mask, &known_ram_address))
			return EmitKnownDirectRamLoadOp(op, known_ram_address);

		if (!EmitEffectiveAddress(op, HOST_SAVED0) ||
			!m_code.EmitTstImm32(HOST_SAVED0, 0x10000000u))
		{
			return false;
		}

		// PCSX2 owner: x86/iR3000Atables.cpp::rpsxLoad() uses direct iopMem->Main
		// reads for ordinary IOP RAM aliases and iopMemRead* helpers for MMIO/ROM.
		const size_t fallback_branch = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
		if (fallback_branch == static_cast<size_t>(-1))
			return false;

		size_t alignment_fallback_branch = static_cast<size_t>(-1);
		if (rt != 0 && alignment_mask != 0)
		{
			if (!m_code.EmitAndImm8(HOST_TMP2, HOST_SAVED0, alignment_mask, true))
				return false;

			alignment_fallback_branch = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (alignment_fallback_branch == static_cast<size_t>(-1))
				return false;
		}

		if (rt != 0)
		{
			if (!m_code.EmitAndReg(HOST_TMP0, HOST_SAVED0, HOST_IOP_RAM_MASK))
				return false;

			switch (opcode)
			{
				case 0x24: // LBU
					if (!m_code.EmitLdrbRegShift(HOST_TMP0, HOST_IOP_RAM_BASE, HOST_TMP0,
							VitaA32::ShiftType::LSL, 0))
						return false;
					break;
				case 0x20: // LB
					if (!m_code.EmitLdrsbReg(HOST_TMP0, HOST_IOP_RAM_BASE, HOST_TMP0))
						return false;
					break;
				case 0x21: // LH
					if (!m_code.EmitLdrshReg(HOST_TMP0, HOST_IOP_RAM_BASE, HOST_TMP0))
						return false;
					break;
				case 0x25: // LHU
					if (!m_code.EmitLdrhReg(HOST_TMP0, HOST_IOP_RAM_BASE, HOST_TMP0))
						return false;
					break;
				case 0x23: // LW
					if (!m_code.EmitLdrRegShift(HOST_TMP0, HOST_IOP_RAM_BASE, HOST_TMP0,
							VitaA32::ShiftType::LSL, 0))
						return false;
					break;
				default:
					return false;
			}

			if (!EmitStoreGpr(rt, HOST_TMP0))
				return false;
		}

		m_scalar_load_cold_tails.push_back({
			fallback_branch,
			alignment_fallback_branch,
			m_code.Size(),
			helper,
			rt,
			opcode,
			m_current_instruction_count,
		});
		return true;
	}

	bool BlockCompiler::EmitScalarLoadColdTail(const ScalarLoadColdTail& tail)
	{
		// PCSX2 owners: x86/iR3000Atables.cpp::rpsxLoad() and
		// IopMem.cpp::iopMemRead8/16/32. Non-RAM aliases and aligned-helper
		// cases keep the existing helper semantics; ordinary IOP RAM falls
		// through after the direct load.
		const size_t fallback_target = m_code.Size();
		if (!m_code.PatchBranch(tail.fallback_branch, fallback_target, VitaA32::Condition::NE))
			return false;
		if (tail.alignment_fallback_branch != static_cast<size_t>(-1) &&
			!m_code.PatchBranch(tail.alignment_fallback_branch, fallback_target, VitaA32::Condition::NE))
		{
			return false;
		}

		if ((m_track_published_cycle_prefix && !EmitPublishCyclePrefix(tail.cycle_prefix)) ||
			!m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_SAVED0, VitaA32::ShiftType::LSL, 0) ||
			!m_code.EmitCallAbsolute(tail.helper, HOST_CALL_SCRATCH))
		{
			return false;
		}

		if (tail.rt != 0)
		{
			switch (tail.opcode)
			{
				case 0x20: // LB
					if (!m_code.EmitSxtb(HOST_TMP0, HOST_TMP0))
						return false;
					break;
				case 0x21: // LH
					if (!m_code.EmitSxth(HOST_TMP0, HOST_TMP0))
						return false;
					break;
				default:
					break;
			}

			if (!EmitStoreGpr(tail.rt, HOST_TMP0))
				return false;
		}

		const size_t tail_done = m_code.EmitBranchPlaceholder();
		return tail_done != static_cast<size_t>(-1) &&
			   m_code.PatchBranch(tail_done, tail.join_offset);
	}

	bool BlockCompiler::EmitKnownDirectRamStoreOp(u32 op, u32 address)
	{
		const unsigned opcode = op >> 26;
#if defined(VITASX2_QEMU_VALIDATION)
		++g_qemuIopKnownRamScalarStoreFastPaths;
		if (RS(op) != 0)
			++g_qemuIopConstRamScalarStoreFastPaths;
#endif

		const auto emit_store_value = [&]() -> bool {
			switch (opcode)
			{
				case 0x28: // SB
					if (address <= 0x0fffu)
						return m_code.EmitStrbImm12(HOST_TMP1, HOST_IOP_RAM_BASE, static_cast<u16>(address));
					return m_code.EmitMovImm32(HOST_TMP0, address) &&
						   m_code.EmitStrbRegShift(HOST_TMP1, HOST_IOP_RAM_BASE, HOST_TMP0,
							   VitaA32::ShiftType::LSL, 0);
				case 0x29: // SH
					if (address <= 0xffu)
						return m_code.EmitStrhImm8(HOST_TMP1, HOST_IOP_RAM_BASE, static_cast<u8>(address));
					return m_code.EmitMovImm32(HOST_TMP0, address) &&
						   m_code.EmitStrhReg(HOST_TMP1, HOST_IOP_RAM_BASE, HOST_TMP0);
				case 0x2b: // SW
					if (address <= 0x0fffu)
						return m_code.EmitStrImm12(HOST_TMP1, HOST_IOP_RAM_BASE, static_cast<u16>(address));
					return m_code.EmitMovImm32(HOST_TMP0, address) &&
						   m_code.EmitStrRegShift(HOST_TMP1, HOST_IOP_RAM_BASE, HOST_TMP0,
							   VitaA32::ShiftType::LSL, 0);
				default:
					return false;
			}
		};

		const auto emit_clear_stored_word = [&]() -> bool {
			return m_code.EmitMovImm32(HOST_TMP0, address & ~3u) &&
				   m_code.EmitMovImm8(HOST_TMP1, 1) &&
				   m_code.EmitMovImm32(HOST_CALL_SCRATCH,
					   static_cast<u32>(reinterpret_cast<uptr>(&psxCpu))) &&
				   m_code.EmitLdrImm12(HOST_CALL_SCRATCH, HOST_CALL_SCRATCH, 0) &&
				   m_code.EmitLdrImm12(HOST_CALL_SCRATCH, HOST_CALL_SCRATCH,
					   static_cast<u16>(offsetof(R3000Acpu, Clear))) &&
				   m_code.EmitBlx(HOST_CALL_SCRATCH);
		};

		// PCSX2 owner: IopMem.cpp::iopMemWrite8/16/32 writes ordinary RAM
		// directly when isolate-cache is clear and invalidates the written word.
		// A compile-time-known main-RAM address cannot hit the MMIO/ROM helper
		// arm.
		if (!m_code.EmitLdrImm12(HOST_TMP2, HOST_PSX_REGS, static_cast<u16>(CP0_STATUS_OFFSET)) ||
			!m_code.EmitTstImm32(HOST_TMP2, 0x10000u))
		{
			return false;
		}

		bool used_known_store_value = false;
		const size_t isolated_skip = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
		if (isolated_skip == static_cast<size_t>(-1) ||
			!EmitLoadGprValue(RT(op), HOST_TMP1, &used_known_store_value) ||
			!emit_store_value() ||
			!emit_clear_stored_word())
		{
			return false;
		}

		if (!m_code.PatchBranch(isolated_skip, m_code.Size(), VitaA32::Condition::NE))
			return false;
#if defined(VITASX2_QEMU_VALIDATION)
		if (used_known_store_value)
			++g_qemuIopConstStoreValueFastPaths;
#endif
		return true;
	}

	bool BlockCompiler::EmitStoreOp(u32 op)
	{
		const unsigned opcode = op >> 26;
		const void* helper = nullptr;
		u8 alignment_mask = 0;

		switch (opcode)
		{
			case 0x28: // SB
				helper = reinterpret_cast<const void*>(&iopMemWrite8);
				break;
			case 0x29: // SH
				helper = reinterpret_cast<const void*>(&iopMemWrite16);
				alignment_mask = 1;
				break;
			case 0x2b: // SW
				helper = reinterpret_cast<const void*>(&iopMemWrite32);
				alignment_mask = 3;
				break;
			default:
				return false;
		}

		u32 known_ram_address = 0;
		if (TryKnownDirectIopRamAddress(op, alignment_mask, &known_ram_address))
			return EmitKnownDirectRamStoreOp(op, known_ram_address);

		const auto emit_store_value = [&]() -> bool {
			switch (opcode)
			{
				case 0x28: // SB
					return m_code.EmitStrbRegShift(HOST_TMP1, HOST_IOP_RAM_BASE, HOST_TMP0,
						VitaA32::ShiftType::LSL, 0);
				case 0x29: // SH
					return m_code.EmitStrhReg(HOST_TMP1, HOST_IOP_RAM_BASE, HOST_TMP0);
				case 0x2b: // SW
					return m_code.EmitStrRegShift(HOST_TMP1, HOST_IOP_RAM_BASE, HOST_TMP0,
						VitaA32::ShiftType::LSL, 0);
				default:
					return false;
			}
		};

		const auto emit_clear_stored_word = [&]() -> bool {
			return m_code.EmitBicImm32(HOST_TMP0, HOST_SAVED0, 3) &&
				   m_code.EmitMovImm8(HOST_TMP1, 1) &&
				   m_code.EmitMovImm32(HOST_CALL_SCRATCH,
					   static_cast<u32>(reinterpret_cast<uptr>(&psxCpu))) &&
				   m_code.EmitLdrImm12(HOST_CALL_SCRATCH, HOST_CALL_SCRATCH, 0) &&
				   m_code.EmitLdrImm12(HOST_CALL_SCRATCH, HOST_CALL_SCRATCH,
					   static_cast<u16>(offsetof(R3000Acpu, Clear))) &&
				   m_code.EmitBlx(HOST_CALL_SCRATCH);
		};
		const auto emit_source_page_guard = [&]() -> size_t {
			// PCSX2 owner: x86/iR3000A.cpp::PSXREC_CLEARM checks psxRecLUT
			// before entering recClearIOP(). Vita's compact equivalent counts
			// live source blocks per 4 KiB IOP RAM page. Convert the masked byte
			// address to a page index, then to the halfword-table byte offset.
			if (!m_ram_source_page_live_counts ||
				!m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_TMP0,
					VitaA32::ShiftType::LSR, 12) ||
				!m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_TMP0,
					VitaA32::ShiftType::LSL, 1) ||
				!m_code.EmitMovImm32(HOST_TMP2,
					static_cast<u32>(reinterpret_cast<uptr>(m_ram_source_page_live_counts))) ||
				!m_code.EmitLdrhReg(HOST_TMP0, HOST_TMP2, HOST_TMP0) ||
				!m_code.EmitCmpImm32(HOST_TMP0, 0))
			{
				return static_cast<size_t>(-1);
			}
			return m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
		};

		if (!EmitEffectiveAddress(op, HOST_SAVED0) ||
			!m_code.EmitTstImm32(HOST_SAVED0, 0x10000000u))
		{
			return false;
		}

		// PCSX2 owner: IopMem.cpp::iopMemWrite8/16/32 writes directly through
		// psxMemWLUT only for writable RAM and when CP0 isolate-cache is clear,
		// then invalidates the written word through psxCpu->Clear(mem & ~3, 1).
		const size_t fallback_branch = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
		if (fallback_branch == static_cast<size_t>(-1))
			return false;

		size_t alignment_fallback_branch = static_cast<size_t>(-1);
		if (alignment_mask != 0)
		{
			if (!m_code.EmitAndImm8(HOST_TMP2, HOST_SAVED0, alignment_mask, true))
				return false;

			alignment_fallback_branch = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (alignment_fallback_branch == static_cast<size_t>(-1))
				return false;
		}

		if (!m_code.EmitLdrImm12(HOST_TMP2, HOST_PSX_REGS, static_cast<u16>(CP0_STATUS_OFFSET)) ||
			!m_code.EmitTstImm32(HOST_TMP2, 0x10000u))
		{
			return false;
		}

		bool used_known_store_value = false;
		const size_t isolated_fallback_branch = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
		if (isolated_fallback_branch == static_cast<size_t>(-1) ||
			!EmitLoadGprValue(RT(op), HOST_TMP1, &used_known_store_value) ||
			!m_code.EmitAndReg(HOST_TMP0, HOST_SAVED0, HOST_IOP_RAM_MASK) ||
			!emit_store_value())
		{
			return false;
		}
		const size_t no_source_page_branch = emit_source_page_guard();
		if (no_source_page_branch == static_cast<size_t>(-1) ||
			!emit_clear_stored_word() ||
			!m_code.PatchBranch(no_source_page_branch, m_code.Size(), VitaA32::Condition::EQ))
		{
			return false;
		}

		m_scalar_store_cold_tails.push_back({
			fallback_branch,
			alignment_fallback_branch,
			isolated_fallback_branch,
			m_code.Size(),
			helper,
			RT(op),
			m_current_instruction_count,
		});
#if defined(VITASX2_QEMU_VALIDATION)
		if (used_known_store_value)
			++g_qemuIopConstStoreValueFastPaths;
#endif
		return true;
	}

	bool BlockCompiler::EmitScalarStoreColdTail(const ScalarStoreColdTail& tail)
	{
		// PCSX2 owners: IopMem.cpp::iopMemWrite8/16/32 and
		// x86/iR3000Atables.cpp::rpsxStore(). MMIO/ROM, alignment, and
		// isolate-cache paths call the existing helper; writable RAM falls
		// through after the direct store and psxCpu->Clear() invalidation.
		const size_t fallback_target = m_code.Size();
		if (!m_code.PatchBranch(tail.fallback_branch, fallback_target, VitaA32::Condition::NE))
			return false;
		if (tail.alignment_fallback_branch != static_cast<size_t>(-1) &&
			!m_code.PatchBranch(tail.alignment_fallback_branch, fallback_target, VitaA32::Condition::NE))
		{
			return false;
		}
		if (!m_code.PatchBranch(tail.isolated_fallback_branch, fallback_target, VitaA32::Condition::NE) ||
			(m_track_published_cycle_prefix && !EmitPublishCyclePrefix(tail.cycle_prefix)) ||
			!m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_SAVED0, VitaA32::ShiftType::LSL, 0) ||
			!EmitLoadGpr(tail.rt, HOST_TMP1) ||
			!m_code.EmitCallAbsolute(tail.helper, HOST_CALL_SCRATCH))
		{
			return false;
		}

		const size_t tail_done = m_code.EmitBranchPlaceholder();
		return tail_done != static_cast<size_t>(-1) &&
			   m_code.PatchBranch(tail_done, tail.join_offset);
	}

	bool BlockCompiler::FlushColdTails()
	{
		for (const ScalarLoadColdTail& tail : m_scalar_load_cold_tails)
		{
			if (!EmitScalarLoadColdTail(tail))
				return false;
		}

		for (const ScalarStoreColdTail& tail : m_scalar_store_cold_tails)
		{
			if (!EmitScalarStoreColdTail(tail))
				return false;
		}

		for (const UnalignedReadColdTail& tail : m_unaligned_read_cold_tails)
		{
			if (!EmitUnalignedReadColdTail(tail))
				return false;
		}

		for (const UnalignedWriteColdTail& tail : m_unaligned_write_cold_tails)
		{
			if (!EmitUnalignedWriteColdTail(tail))
				return false;
		}

		for (const Cop2LoadColdTail& tail : m_cop2_load_cold_tails)
		{
			if (!EmitCop2LoadColdTail(tail))
				return false;
		}

		for (const Cop2StoreColdTail& tail : m_cop2_store_cold_tails)
		{
			if (!EmitCop2StoreColdTail(tail))
				return false;
		}

		m_scalar_load_cold_tails.clear();
		m_scalar_store_cold_tails.clear();
		m_unaligned_read_cold_tails.clear();
		m_unaligned_write_cold_tails.clear();
		m_cop2_load_cold_tails.clear();
		m_cop2_store_cold_tails.clear();
		return true;
	}

	bool BlockCompiler::EmitUnalignedReadColdTail(const UnalignedReadColdTail& tail)
	{
		// PCSX2 owners: R3000AOpcodeTables.cpp::psxLWL/psxLWR/psxSWL/psxSWR
		// merge an aligned iopMemRead32() word. Handler-backed addresses call
		// the helper; ordinary IOP RAM falls through with HOST_TMP0 loaded.
		const size_t fallback_target = m_code.Size();
		if (!m_code.PatchBranch(tail.fallback_branch, fallback_target, VitaA32::Condition::NE) ||
			(m_track_published_cycle_prefix && !EmitPublishCyclePrefix(tail.cycle_prefix)) ||
			!m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_SAVED1, VitaA32::ShiftType::LSL, 0) ||
			!m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&iopMemRead32), HOST_CALL_SCRATCH))
		{
			return false;
		}

		const size_t tail_done = m_code.EmitBranchPlaceholder();
		return tail_done != static_cast<size_t>(-1) &&
			   m_code.PatchBranch(tail_done, tail.join_offset);
	}

	bool BlockCompiler::EmitUnalignedWriteColdTail(const UnalignedWriteColdTail& tail)
	{
		// PCSX2 owner: IopMem.cpp::iopMemWrite32(). The merged word is already
		// in HOST_TMP1 when the fallback branch fires; writable RAM falls
		// through after the direct write and invalidation.
		const size_t fallback_target = m_code.Size();
		if (!m_code.PatchBranch(tail.write_fallback_branch, fallback_target, VitaA32::Condition::NE) ||
			!m_code.PatchBranch(tail.isolated_fallback_branch, fallback_target, VitaA32::Condition::NE) ||
			(m_track_published_cycle_prefix && !EmitPublishCyclePrefix(tail.cycle_prefix)) ||
			!m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_SAVED1, VitaA32::ShiftType::LSL, 0) ||
			(m_track_published_cycle_prefix &&
				!m_code.EmitMovRegShiftImm(HOST_TMP1, HOST_SAVED0, VitaA32::ShiftType::LSL, 0)) ||
			!m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&iopMemWrite32), HOST_CALL_SCRATCH))
		{
			return false;
		}

		const size_t tail_done = m_code.EmitBranchPlaceholder();
		return tail_done != static_cast<size_t>(-1) &&
			   m_code.PatchBranch(tail_done, tail.join_offset);
	}

	bool BlockCompiler::EmitCop2LoadColdTail(const Cop2LoadColdTail& tail)
	{
		// PCSX2 owners: IopGte.cpp::gteLWC2()/MTC2() and
		// IopMem.cpp::iopMemRead32(). Helper-backed reads still feed the same
		// GTE data-register side effects as the direct RAM path.
		const size_t fallback_target = m_code.Size();
		if (!m_code.PatchBranch(tail.fallback_branch, fallback_target, VitaA32::Condition::NE) ||
			!m_code.PatchBranch(tail.alignment_fallback_branch, fallback_target, VitaA32::Condition::NE) ||
			(m_track_published_cycle_prefix && !EmitPublishCyclePrefix(tail.cycle_prefix)) ||
			!m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_SAVED0, VitaA32::ShiftType::LSL, 0) ||
			!m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&iopMemRead32), HOST_CALL_SCRATCH) ||
			!EmitWriteCop2DataReg(tail.cop2_reg, HOST_TMP0))
		{
			return false;
		}

		const size_t tail_done = m_code.EmitBranchPlaceholder();
		return tail_done != static_cast<size_t>(-1) &&
			   m_code.PatchBranch(tail_done, tail.join_offset);
	}

	bool BlockCompiler::EmitCop2StoreColdTail(const Cop2StoreColdTail& tail)
	{
		// PCSX2 owners: IopGte.cpp::gteSWC2()/MFC2() and
		// IopMem.cpp::iopMemWrite32(). EmitReadCop2DataReg() has already
		// produced the MFC2 value in HOST_SAVED0 before these branches fire.
		const size_t fallback_target = m_code.Size();
		if (!m_code.PatchBranch(tail.fallback_branch, fallback_target, VitaA32::Condition::NE) ||
			!m_code.PatchBranch(tail.alignment_fallback_branch, fallback_target, VitaA32::Condition::NE) ||
			!m_code.PatchBranch(tail.isolated_fallback_branch, fallback_target, VitaA32::Condition::NE) ||
			(m_track_published_cycle_prefix && !EmitPublishCyclePrefix(tail.cycle_prefix)) ||
			!m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_SAVED1, VitaA32::ShiftType::LSL, 0) ||
			!m_code.EmitMovRegShiftImm(HOST_TMP1, HOST_SAVED0, VitaA32::ShiftType::LSL, 0) ||
			!m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&iopMemWrite32), HOST_CALL_SCRATCH))
		{
			return false;
		}

		const size_t tail_done = m_code.EmitBranchPlaceholder();
		return tail_done != static_cast<size_t>(-1) &&
			   m_code.PatchBranch(tail_done, tail.join_offset);
	}

	bool BlockCompiler::EmitKnownDirectRamUnalignedLoadOp(u32 op, u32 address)
	{
		const bool left = ((op >> 26) == 0x22);
		const unsigned rt = RT(op);
#if defined(VITASX2_QEMU_VALIDATION)
		++g_qemuIopKnownRamUnalignedLoadFastPaths;
		if (RS(op) != 0)
			++g_qemuIopConstRamUnalignedLoadFastPaths;
#endif

		if (rt == 0)
			return true;

		const u32 aligned_address = address & ~3u;
		const u32 shift = (address & 3u) << 3;
		const auto emit_load_aligned_word = [&]() -> bool {
			if (aligned_address <= 0x0fffu)
				return m_code.EmitLdrImm12(HOST_TMP0, HOST_IOP_RAM_BASE,
					static_cast<u16>(aligned_address));

			return m_code.EmitMovImm32(HOST_TMP0, aligned_address) &&
				   m_code.EmitLdrRegShift(HOST_TMP0, HOST_IOP_RAM_BASE, HOST_TMP0,
					   VitaA32::ShiftType::LSL, 0);
		};

		if (!emit_load_aligned_word())
			return false;

		// PCSX2 owner: R3000AOpcodeTables.cpp::psxLWL()/psxLWR() merge an
		// aligned iopMemRead32() word. A compile-time-known main-RAM address
		// cannot hit the MMIO/ROM helper arm, so emit the fixed merge directly.
		if ((left && shift == 24) || (!left && shift == 0))
			return EmitStoreGpr(rt, HOST_TMP0);

		const auto emit_and_mask = [&](unsigned host_reg, u32 mask) -> bool {
			if (mask == 0)
				return m_code.EmitMovImm8(host_reg, 0);
			if (mask == 0xffffffffu)
				return true;
			if (m_code.EmitAndImm32(host_reg, host_reg, mask))
				return true;
			return m_code.EmitMovImm32(HOST_TMP2, mask) &&
				   m_code.EmitAndReg(host_reg, host_reg, HOST_TMP2);
		};

		if (!EmitLoadGpr(rt, HOST_TMP1))
			return false;

		if (left)
		{
			const u32 old_mask = 0x00ffffffu >> shift;
			const u8 mem_shift = static_cast<u8>(24 - shift);
			return emit_and_mask(HOST_TMP1, old_mask) &&
				   m_code.EmitOrrRegShiftImm(HOST_TMP0, HOST_TMP1, HOST_TMP0,
					   VitaA32::ShiftType::LSL, mem_shift) &&
				   EmitStoreGpr(rt, HOST_TMP0);
		}

		const u32 old_mask = 0xffffff00u << (24 - shift);
		const u8 mem_shift = static_cast<u8>(shift);
		return emit_and_mask(HOST_TMP1, old_mask) &&
			   m_code.EmitOrrRegShiftImm(HOST_TMP0, HOST_TMP1, HOST_TMP0,
				   VitaA32::ShiftType::LSR, mem_shift) &&
			   EmitStoreGpr(rt, HOST_TMP0);
	}

	bool BlockCompiler::EmitUnalignedLoadOp(u32 op)
	{
		u32 known_ram_address = 0;
		if (TryKnownDirectIopRamAddress(op, 0, &known_ram_address))
			return EmitKnownDirectRamUnalignedLoadOp(op, known_ram_address);

		const bool left = ((op >> 26) == 0x22);
		if (!EmitEffectiveAddress(op) ||
			!m_code.EmitAndImm8(HOST_SAVED0, HOST_TMP0, 3) ||
			!m_code.EmitMovRegShiftImm(HOST_SAVED0, HOST_SAVED0, VitaA32::ShiftType::LSL, 3) ||
			!m_code.EmitBicImm32(HOST_SAVED1, HOST_TMP0, 3) ||
			!m_code.EmitTstImm32(HOST_SAVED1, 0x10000000u))
		{
			return false;
		}

		// PCSX2 owner: R3000AOpcodeTables.cpp::psxLWL/psxLWR use iopMemRead32()
		// on the aligned address. Ordinary IOP RAM can read iopMem->Main directly.
		const size_t fallback_branch = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
		if (fallback_branch == static_cast<size_t>(-1) ||
			!m_code.EmitAndReg(HOST_TMP0, HOST_SAVED1, HOST_IOP_RAM_MASK) ||
			!m_code.EmitLdrRegShift(HOST_TMP0, HOST_IOP_RAM_BASE, HOST_TMP0,
				VitaA32::ShiftType::LSL, 0))
		{
			return false;
		}

		m_unaligned_read_cold_tails.push_back({
			fallback_branch,
			m_code.Size(),
			m_current_instruction_count,
		});

		if (RT(op) == 0)
			return true;

		if (!EmitLoadGpr(RT(op), HOST_TMP1))
			return false;

		if (left)
		{
			return m_code.EmitMovImm32(HOST_TMP2, 0x00ffffffu) &&
				   m_code.EmitAndRegShiftReg(HOST_TMP1, HOST_TMP1, HOST_TMP2,
					   VitaA32::ShiftType::LSR, HOST_SAVED0) &&
				   m_code.EmitRsbImm32(HOST_TMP3, HOST_SAVED0, 24) &&
				   m_code.EmitOrrRegShiftReg(HOST_TMP0, HOST_TMP1, HOST_TMP0,
					   VitaA32::ShiftType::LSL, HOST_TMP3) &&
				   EmitStoreGpr(RT(op), HOST_TMP0);
		}

		return m_code.EmitMovImm32(HOST_TMP2, 0xffffff00u) &&
			   m_code.EmitRsbImm32(HOST_TMP3, HOST_SAVED0, 24) &&
			   m_code.EmitAndRegShiftReg(HOST_TMP1, HOST_TMP1, HOST_TMP2,
				   VitaA32::ShiftType::LSL, HOST_TMP3) &&
			   m_code.EmitOrrRegShiftReg(HOST_TMP0, HOST_TMP1, HOST_TMP0,
				   VitaA32::ShiftType::LSR, HOST_SAVED0) &&
			   EmitStoreGpr(RT(op), HOST_TMP0);
	}

	bool BlockCompiler::EmitKnownDirectRamUnalignedStoreOp(u32 op, u32 address)
	{
		const bool left = ((op >> 26) == 0x2a);
#if defined(VITASX2_QEMU_VALIDATION)
		++g_qemuIopKnownRamUnalignedStoreFastPaths;
		if (RS(op) != 0)
			++g_qemuIopConstRamUnalignedStoreFastPaths;
#endif

		const u32 aligned_address = address & ~3u;
		const u32 shift = (address & 3u) << 3;
		const auto emit_load_aligned_word = [&]() -> bool {
			if (aligned_address <= 0x0fffu)
				return m_code.EmitLdrImm12(HOST_TMP0, HOST_IOP_RAM_BASE,
					static_cast<u16>(aligned_address));

			return m_code.EmitMovImm32(HOST_TMP0, aligned_address) &&
				   m_code.EmitLdrRegShift(HOST_TMP0, HOST_IOP_RAM_BASE, HOST_TMP0,
					   VitaA32::ShiftType::LSL, 0);
		};
		const auto emit_store_merged_word = [&]() -> bool {
			if (aligned_address <= 0x0fffu)
				return m_code.EmitStrImm12(HOST_TMP1, HOST_IOP_RAM_BASE,
					static_cast<u16>(aligned_address));

			return m_code.EmitMovImm32(HOST_TMP0, aligned_address) &&
				   m_code.EmitStrRegShift(HOST_TMP1, HOST_IOP_RAM_BASE, HOST_TMP0,
					   VitaA32::ShiftType::LSL, 0);
		};
		const auto emit_clear_stored_word = [&]() -> bool {
			return m_code.EmitMovImm32(HOST_TMP0, aligned_address) &&
				   m_code.EmitMovImm8(HOST_TMP1, 1) &&
				   m_code.EmitMovImm32(HOST_CALL_SCRATCH,
					   static_cast<u32>(reinterpret_cast<uptr>(&psxCpu))) &&
				   m_code.EmitLdrImm12(HOST_CALL_SCRATCH, HOST_CALL_SCRATCH, 0) &&
				   m_code.EmitLdrImm12(HOST_CALL_SCRATCH, HOST_CALL_SCRATCH,
					   static_cast<u16>(offsetof(R3000Acpu, Clear))) &&
				   m_code.EmitBlx(HOST_CALL_SCRATCH);
		};
		const auto emit_and_mask = [&](unsigned host_reg, u32 mask) -> bool {
			if (mask == 0)
				return m_code.EmitMovImm8(host_reg, 0);
			if (mask == 0xffffffffu)
				return true;
			if (m_code.EmitAndImm32(host_reg, host_reg, mask))
				return true;
			return m_code.EmitMovImm32(HOST_TMP2, mask) &&
				   m_code.EmitAndReg(host_reg, host_reg, HOST_TMP2);
		};
		const auto emit_orr_shift = [&](unsigned rd, unsigned rn, unsigned rm,
										VitaA32::ShiftType shift_type, u8 amount) -> bool {
			if (amount == 0)
				return m_code.EmitOrrRegShiftImm(rd, rn, rm, VitaA32::ShiftType::LSL, 0);
			return m_code.EmitOrrRegShiftImm(rd, rn, rm, shift_type, amount);
		};

		// PCSX2 owners: R3000AOpcodeTables.cpp::psxSWL()/psxSWR() merge an
		// aligned iopMemRead32() word, then IopMem.cpp::iopMemWrite32() applies
		// isolate-cache suppression and psxCpu->Clear() invalidation.
		bool used_known_store_value = false;
		if (!emit_load_aligned_word() || !EmitLoadGprValue(RT(op), HOST_TMP1, &used_known_store_value))
			return false;

		if (left)
		{
			const u32 old_mask = 0xffffff00u << shift;
			const u8 rt_shift = static_cast<u8>(24 - shift);
			if (!emit_and_mask(HOST_TMP0, old_mask) ||
				!emit_orr_shift(HOST_TMP1, HOST_TMP0, HOST_TMP1,
					VitaA32::ShiftType::LSR, rt_shift))
			{
				return false;
			}
		}
		else
		{
			const u8 mem_shift = static_cast<u8>(24 - shift);
			const u32 old_mask = 0x00ffffffu >> mem_shift;
			if (!emit_and_mask(HOST_TMP0, old_mask) ||
				!m_code.EmitOrrRegShiftImm(HOST_TMP1, HOST_TMP0, HOST_TMP1,
					VitaA32::ShiftType::LSL, static_cast<u8>(shift)))
			{
				return false;
			}
		}

		if (!m_code.EmitLdrImm12(HOST_TMP2, HOST_PSX_REGS, static_cast<u16>(CP0_STATUS_OFFSET)) ||
			!m_code.EmitTstImm32(HOST_TMP2, 0x10000u))
		{
			return false;
		}

		const size_t isolated_skip = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
		if (isolated_skip == static_cast<size_t>(-1) ||
			!emit_store_merged_word() ||
			!emit_clear_stored_word())
		{
			return false;
		}

		if (!m_code.PatchBranch(isolated_skip, m_code.Size(), VitaA32::Condition::NE))
			return false;
#if defined(VITASX2_QEMU_VALIDATION)
		if (used_known_store_value)
			++g_qemuIopConstStoreValueFastPaths;
#endif
		return true;
	}

	bool BlockCompiler::EmitUnalignedStoreOp(u32 op)
	{
		u32 known_ram_address = 0;
		if (TryKnownDirectIopRamAddress(op, 0, &known_ram_address))
			return EmitKnownDirectRamUnalignedStoreOp(op, known_ram_address);

		const bool left = ((op >> 26) == 0x2a);
		const auto emit_clear_stored_word = [&]() -> bool {
			return m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_SAVED1, VitaA32::ShiftType::LSL, 0) &&
				   m_code.EmitMovImm8(HOST_TMP1, 1) &&
				   m_code.EmitMovImm32(HOST_CALL_SCRATCH,
					   static_cast<u32>(reinterpret_cast<uptr>(&psxCpu))) &&
				   m_code.EmitLdrImm12(HOST_CALL_SCRATCH, HOST_CALL_SCRATCH, 0) &&
				   m_code.EmitLdrImm12(HOST_CALL_SCRATCH, HOST_CALL_SCRATCH,
					   static_cast<u16>(offsetof(R3000Acpu, Clear))) &&
				   m_code.EmitBlx(HOST_CALL_SCRATCH);
		};

		if (!EmitEffectiveAddress(op) ||
			!m_code.EmitAndImm8(HOST_SAVED0, HOST_TMP0, 3) ||
			!m_code.EmitMovRegShiftImm(HOST_SAVED0, HOST_SAVED0, VitaA32::ShiftType::LSL, 3) ||
			!m_code.EmitBicImm32(HOST_SAVED1, HOST_TMP0, 3) ||
			!m_code.EmitTstImm32(HOST_SAVED1, 0x10000000u))
		{
			return false;
		}

		// PCSX2 owners: R3000AOpcodeTables.cpp::psxSWL/psxSWR merge the aligned
		// word, while IopMem.cpp::iopMemWrite32() owns writable-RAM filtering,
		// isolate-cache suppression, and psxCpu->Clear() invalidation.
		const size_t fallback_branch = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
		if (fallback_branch == static_cast<size_t>(-1) ||
			!m_code.EmitAndReg(HOST_TMP0, HOST_SAVED1, HOST_IOP_RAM_MASK) ||
			!m_code.EmitLdrRegShift(HOST_TMP0, HOST_IOP_RAM_BASE, HOST_TMP0,
				VitaA32::ShiftType::LSL, 0))
		{
			return false;
		}

		m_unaligned_read_cold_tails.push_back({
			fallback_branch,
			m_code.Size(),
			m_current_instruction_count,
		});

		bool used_known_store_value = false;
		if (!EmitLoadGprValue(RT(op), HOST_TMP1, &used_known_store_value))
			return false;

		if (left)
		{
			if (!m_code.EmitRsbImm32(HOST_TMP3, HOST_SAVED0, 24) ||
				!m_code.EmitMovImm32(HOST_TMP2, 0xffffff00u))
			{
				return false;
			}
		}
		else
		{
			if (!m_code.EmitRsbImm32(HOST_TMP3, HOST_SAVED0, 24) ||
				!m_code.EmitMovImm32(HOST_TMP2, 0x00ffffffu))
			{
				return false;
			}
		}

		if (!(left ? m_code.EmitAndRegShiftReg(HOST_TMP0, HOST_TMP0, HOST_TMP2,
					   VitaA32::ShiftType::LSL, HOST_SAVED0) :
					 m_code.EmitAndRegShiftReg(HOST_TMP0, HOST_TMP0, HOST_TMP2,
					   VitaA32::ShiftType::LSR, HOST_TMP3)) ||
			!(left ? m_code.EmitOrrRegShiftReg(HOST_TMP1, HOST_TMP0, HOST_TMP1,
					   VitaA32::ShiftType::LSR, HOST_TMP3) :
					 m_code.EmitOrrRegShiftReg(HOST_TMP1, HOST_TMP0, HOST_TMP1,
					   VitaA32::ShiftType::LSL, HOST_SAVED0)) ||
			(m_track_published_cycle_prefix &&
				!m_code.EmitMovRegShiftImm(HOST_SAVED0, HOST_TMP1, VitaA32::ShiftType::LSL, 0)) ||
			!m_code.EmitTstImm32(HOST_SAVED1, 0x10000000u))
		{
			return false;
		}

		const size_t write_fallback_branch = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
		if (write_fallback_branch == static_cast<size_t>(-1) ||
			!m_code.EmitLdrImm12(HOST_TMP2, HOST_PSX_REGS, static_cast<u16>(CP0_STATUS_OFFSET)) ||
			!m_code.EmitTstImm32(HOST_TMP2, 0x10000u))
		{
			return false;
		}

		const size_t isolated_fallback_branch = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
		if (isolated_fallback_branch == static_cast<size_t>(-1) ||
			!m_code.EmitAndReg(HOST_TMP0, HOST_SAVED1, HOST_IOP_RAM_MASK) ||
			!m_code.EmitStrRegShift(HOST_TMP1, HOST_IOP_RAM_BASE, HOST_TMP0,
				VitaA32::ShiftType::LSL, 0) ||
			!emit_clear_stored_word())
		{
			return false;
		}

		m_unaligned_write_cold_tails.push_back({
			write_fallback_branch,
			isolated_fallback_branch,
			m_code.Size(),
			m_current_instruction_count,
		});
#if defined(VITASX2_QEMU_VALIDATION)
		if (used_known_store_value)
			++g_qemuIopConstStoreValueFastPaths;
#endif
		return true;
	}

	bool BlockCompiler::EmitConditionalBranchOp(u32 op, u32 pc)
	{
		if (m_emit_native_static_branch)
			return EmitConditionalBranchFlag(op);

		if (!EmitCompareGprs(RS(op), RT(op)))
		{
			return false;
		}

		const VitaA32::Condition skip_taken =
			((op >> 26) == 0x04) ? VitaA32::Condition::NE : VitaA32::Condition::EQ;
		const size_t not_taken = m_code.EmitBranchPlaceholder(skip_taken);
		return m_code.EmitMovImm32(HOST_TMP0, BranchTarget(pc, op)) &&
			   EmitBranchHelperExit(reinterpret_cast<const void*>(&psxDoBranch)) &&
			   m_code.PatchBranch(not_taken, m_code.Size(), skip_taken);
	}

	bool BlockCompiler::EmitConditionalBranchFlag(u32 op)
	{
		u32 lhs_value = 0;
		u32 rhs_value = 0;
		const bool lhs_known = TryGetKnownGpr(RS(op), &lhs_value);
		const bool rhs_known = TryGetKnownGpr(RT(op), &rhs_value);
		const bool branch_on_equal = (op >> 26) == 0x04;
		if (RS(op) == RT(op) || (lhs_known && rhs_known))
		{
			const bool equal = RS(op) == RT(op) || lhs_value == rhs_value;
			m_static_branch_outcome_known = true;
			m_static_branch_taken = (equal == branch_on_equal);
#if defined(VITASX2_QEMU_VALIDATION)
			if ((RS(op) != 0 && lhs_known) || (RT(op) != 0 && rhs_known))
				++g_qemuIopConstBranchCompareFastPaths;
#endif
			return true;
		}

		if (!EmitCompareGprs(RS(op), RT(op)) ||
			!m_code.EmitMovImm8(HOST_BRANCH_FLAG, 0))
		{
			return false;
		}

		return m_code.EmitMovImm8(HOST_BRANCH_FLAG, 1,
			((op >> 26) == 0x04) ? VitaA32::Condition::EQ : VitaA32::Condition::NE);
	}

	bool BlockCompiler::EmitSignedBranchOp(u32 op, u32 pc)
	{
		const unsigned opcode = op >> 26;
		const unsigned rt = RT(op);
		const bool link = (opcode == 0x01 && (rt == 0x10 || rt == 0x11));
		if (link)
		{
			if (!m_code.EmitMovImm32(HOST_TMP0, pc + 8) ||
				!EmitStoreGpr(31, HOST_TMP0))
			{
				return false;
			}
		}

		if (m_emit_native_static_branch)
			return EmitSignedBranchFlag(op);

		u32 known_value = 0;
		if (TryGetKnownGpr(RS(op), &known_value))
		{
			const s32 signed_value = static_cast<s32>(known_value);
			bool taken = false;
			if (opcode == 0x01)
			{
				switch (rt)
				{
					case 0x00: // BLTZ
					case 0x10: // BLTZAL
						taken = signed_value < 0;
						break;
					case 0x01: // BGEZ
					case 0x11: // BGEZAL
						taken = signed_value >= 0;
						break;
					default:
						return false;
				}
			}
			else if (opcode == 0x06) // BLEZ
			{
				taken = signed_value <= 0;
			}
			else if (opcode == 0x07) // BGTZ
			{
				taken = signed_value > 0;
			}
			else
			{
				return false;
			}

			if (!taken)
				return true;
			return m_code.EmitMovImm32(HOST_TMP0, BranchTarget(pc, op)) &&
				   EmitBranchHelperExit(reinterpret_cast<const void*>(&psxDoBranch));
		}

		if (!EmitLoadGpr(RS(op), HOST_TMP0) ||
			!m_code.EmitCmpImm32(HOST_TMP0, 0))
		{
			return false;
		}

		VitaA32::Condition skip_taken = VitaA32::Condition::AL;
		if (opcode == 0x01)
		{
			switch (rt)
			{
				case 0x00: // BLTZ
				case 0x10: // BLTZAL
					skip_taken = VitaA32::Condition::GE;
					break;
				case 0x01: // BGEZ
				case 0x11: // BGEZAL
					skip_taken = VitaA32::Condition::LT;
					break;
				default:
					return false;
			}
		}
		else if (opcode == 0x06) // BLEZ
		{
			skip_taken = VitaA32::Condition::GT;
		}
		else if (opcode == 0x07) // BGTZ
		{
			skip_taken = VitaA32::Condition::LE;
		}
		else
		{
			return false;
		}

		const size_t not_taken = m_code.EmitBranchPlaceholder(skip_taken);
		return m_code.EmitMovImm32(HOST_TMP0, BranchTarget(pc, op)) &&
			   EmitBranchHelperExit(reinterpret_cast<const void*>(&psxDoBranch)) &&
			   m_code.PatchBranch(not_taken, m_code.Size(), skip_taken);
	}

	bool BlockCompiler::EmitSignedBranchFlag(u32 op)
	{
		// PCSX2 owner: x86/iR3000Atables.cpp signed REGIMM/BLEZ/BGTZ lowering.
		// Keep the same signed-zero predicate; known static branches also collapse
		// the tail to one PC store/direct-link path, matching the EE A32 branch fold.
		const unsigned opcode = op >> 26;
		const unsigned rt = RT(op);
		VitaA32::Condition taken = VitaA32::Condition::AL;
		if (opcode == 0x01)
		{
			switch (rt)
			{
				case 0x00: // BLTZ
				case 0x10: // BLTZAL
					taken = VitaA32::Condition::LT;
					break;
				case 0x01: // BGEZ
				case 0x11: // BGEZAL
					taken = VitaA32::Condition::GE;
					break;
				default:
					return false;
			}
		}
		else if (opcode == 0x06) // BLEZ
		{
			taken = VitaA32::Condition::LE;
		}
		else if (opcode == 0x07) // BGTZ
		{
			taken = VitaA32::Condition::GT;
		}
		else
		{
			return false;
		}

		if (RS(op) == 0)
		{
			m_static_branch_outcome_known = true;
			m_static_branch_taken = taken == VitaA32::Condition::GE || taken == VitaA32::Condition::LE;
			return true;
		}

		u32 known_value = 0;
		if (TryGetKnownGpr(RS(op), &known_value))
		{
			const s32 signed_value = static_cast<s32>(known_value);
			bool is_taken = false;
			switch (taken)
			{
				case VitaA32::Condition::LT:
					is_taken = signed_value < 0;
					break;
				case VitaA32::Condition::GE:
					is_taken = signed_value >= 0;
					break;
				case VitaA32::Condition::LE:
					is_taken = signed_value <= 0;
					break;
				case VitaA32::Condition::GT:
					is_taken = signed_value > 0;
					break;
				default:
					return false;
			}
#if defined(VITASX2_QEMU_VALIDATION)
				++g_qemuIopConstSignedBranchFastPaths;
#endif
			m_static_branch_outcome_known = true;
			m_static_branch_taken = is_taken;
			return true;
		}

		return EmitLoadGpr(RS(op), HOST_TMP0) &&
			   m_code.EmitCmpImm32(HOST_TMP0, 0) &&
			   m_code.EmitMovImm8(HOST_BRANCH_FLAG, 0) &&
			   m_code.EmitMovImm8(HOST_BRANCH_FLAG, 1, taken);
	}

	bool BlockCompiler::EmitJumpOp(u32 op, u32 pc)
	{
		if (m_emit_native_static_jump)
			return EmitStaticJumpOp(op, pc);

		if ((op >> 26) == 0x03) // JAL
		{
			if (!m_code.EmitMovImm32(HOST_TMP0, pc + 8) ||
				!EmitStoreGpr(31, HOST_TMP0) ||
				!m_code.EmitMovImm32(HOST_TMP0, JumpTarget(pc, op)) ||
				!m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&psxDoBranch), HOST_CALL_SCRATCH))
			{
				return false;
			}
		}
		else
		{
			if (!m_code.EmitMovImm32(HOST_TMP0, JumpTarget(pc, op)) ||
				!m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&psxDoJump), HOST_CALL_SCRATCH))
			{
				return false;
			}
		}

		return EndBlockReturn(BlockExitKind::Direct);
	}

	bool BlockCompiler::EmitStaticJumpOp(u32 op, u32 pc)
	{
		if ((op >> 26) == 0x03) // JAL
		{
			return m_code.EmitMovImm32(HOST_TMP0, pc + 8) &&
				   EmitStoreGpr(31, HOST_TMP0);
		}

		return (op >> 26) == 0x02; // J
	}

	bool BlockCompiler::EmitRegisterJumpOp(u32 op, u32 pc)
	{
		if (m_emit_native_register_jump)
			return EmitRegisterJumpCaptureOp(op, pc);

		if ((op & 0x3f) == 0x09 && RD(op) != 0) // JALR
		{
			if (!m_code.EmitMovImm32(HOST_TMP0, pc + 8) ||
				!EmitStoreGpr(RD(op), HOST_TMP0))
			{
				return false;
			}
		}

		return EmitLoadGpr(RS(op), HOST_TMP0) &&
			   EmitBranchHelperExit(reinterpret_cast<const void*>(&psxDoBranch));
	}

	bool BlockCompiler::EmitRegisterJumpCaptureOp(u32 op, u32 pc)
	{
		if ((op & 0x3f) == 0x09 && RD(op) != 0) // JALR
		{
			if (!m_code.EmitMovImm32(HOST_TMP0, pc + 8) ||
				!EmitStoreGpr(RD(op), HOST_TMP0))
			{
				return false;
			}
		}

		u32 known_target = 0;
		const bool known_jalr_self_link =
			(op & 0x3f) == 0x09 && RD(op) != 0 && RD(op) == RS(op);
		const bool known_target_available =
			known_jalr_self_link || TryGetKnownGpr(RS(op), &known_target);
		if (known_jalr_self_link)
			known_target = pc + 8;

		if (known_target_available && !IsIopSpecialBranchTarget(known_target))
		{
			// PCSX2 owners: R3000AInterpreter.cpp::psxJR()/psxJALR() and
			// x86/iR3000Atables.cpp::rpsxJR()/rpsxJALR(). Constant JR/JALR
			// targets can use the static direct-link tail after the delay slot,
			// but special psxDoBranch() targets must keep their helper side effects.
			m_register_jump_target_known = true;
			m_register_jump_target = known_target;
#if defined(VITASX2_QEMU_VALIDATION)
			++g_qemuIopConstRegisterJumpFastPaths;
#endif
			return true;
		}

		if (!EmitLoadGpr(RS(op), HOST_REGISTER_JUMP_TARGET))
			return false;

		// psxDoBranch() owns these diagnostic/module/IOPBOOT side effects.
		const auto emit_special_target_check = [this](u32 target) -> size_t {
			if (!(m_code.EmitCmpImm32(HOST_REGISTER_JUMP_TARGET, target) ||
				  (m_code.EmitMovImm32(HOST_TMP0, target) &&
				   m_code.EmitCmpReg(HOST_REGISTER_JUMP_TARGET, HOST_TMP0))))
			{
				return static_cast<size_t>(-1);
			}

			return m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
		};

		const size_t zero_target = emit_special_target_check(IOP_BRANCH_TARGET_ZERO);
		const size_t sysmem_target = emit_special_target_check(IOP_BRANCH_TARGET_SYSMEM);
		const size_t iopboot_target = emit_special_target_check(IOP_BRANCH_TARGET_IOPBOOT);
		if (zero_target == static_cast<size_t>(-1) ||
			sysmem_target == static_cast<size_t>(-1) ||
			iopboot_target == static_cast<size_t>(-1))
		{
			return false;
		}

		const size_t skip_helper = m_code.EmitBranchPlaceholder();
		if (skip_helper == static_cast<size_t>(-1))
			return false;

		const size_t helper_path = m_code.Size();
		if (!m_code.PatchBranch(zero_target, helper_path, VitaA32::Condition::EQ) ||
			!m_code.PatchBranch(sysmem_target, helper_path, VitaA32::Condition::EQ) ||
			!m_code.PatchBranch(iopboot_target, helper_path, VitaA32::Condition::EQ) ||
			!m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_REGISTER_JUMP_TARGET, VitaA32::ShiftType::LSL, 0) ||
			!EmitBranchHelperExit(reinterpret_cast<const void*>(&psxDoBranch)))
		{
			return false;
		}

		return m_code.PatchBranch(skip_helper, m_code.Size());
	}

	bool BlockCompiler::EmitIopEventTestFastPath()
	{
		struct HelperBranch
		{
			size_t offset;
			VitaA32::Condition condition;
		};
		std::vector<HelperBranch> helper_branches;
		helper_branches.reserve(5);

		const auto emit_add_wait_cycles = [this]() {
			if (m_code.EmitAddImm32(HOST_TMP2, HOST_TMP0, IOP_WAIT_CYCLES, true))
				return true;

			return m_code.EmitMovImm32(HOST_TMP2, IOP_WAIT_CYCLES) &&
				   m_code.EmitAddReg(HOST_TMP2, HOST_TMP0, HOST_TMP2, true);
		};
		const auto emit_schedule_next_event_from_cycle_base = [this, &emit_add_wait_cycles](unsigned cycle_base_reg) {
			return m_code.EmitLdrdImm8(HOST_TMP0, HOST_TMP1, cycle_base_reg, 0) &&
				   emit_add_wait_cycles() &&
				   m_code.EmitAdcImm8(HOST_TMP3, HOST_TMP1, 0) &&
				   m_code.EmitStrdImm8(HOST_TMP2, HOST_TMP3, cycle_base_reg,
					   static_cast<u8>(IOP_NEXT_EVENT_CYCLE_FROM_CYCLE_OFFSET));
		};

		// PCSX2 owner: R3000A.cpp::iopEventTest() writes
		// psxRegs.iopNextEventCycle = psxRegs.cycle + iopWaitCycles. Keep the
		// 64-bit fields paired so Cortex-A9 can issue one load/store each.
		const bool emitted_schedule =
			m_iop_cycle_base_register_available ?
				emit_schedule_next_event_from_cycle_base(HOST_CYCLE_BASE) :
				((m_code.EmitAddImm32(HOST_CALL_SCRATCH, HOST_PSX_REGS, static_cast<u32>(CYCLE_OFFSET)) ||
					 (m_code.EmitMovImm32(HOST_CALL_SCRATCH, static_cast<u32>(CYCLE_OFFSET)) &&
						 m_code.EmitAddReg(HOST_CALL_SCRATCH, HOST_PSX_REGS, HOST_CALL_SCRATCH))) &&
					emit_schedule_next_event_from_cycle_base(HOST_CALL_SCRATCH));
		if (!emitted_schedule)
		{
			return false;
		}

		// PCSX2 owner: R3000A.cpp::iopEventTest(). The generated path handles
		// only the no-work case; due/near counters, scheduled interrupts, and
		// pending IOP INTC all branch to the owner function.
		if (!m_code.EmitMovImm32(HOST_TMP3, static_cast<u32>(reinterpret_cast<uptr>(&psxNextStartCounter))) ||
			!m_code.EmitLdrImm12(HOST_SAVED0, HOST_TMP3, 0) ||
			!m_code.EmitMovImm32(HOST_TMP3, static_cast<u32>(reinterpret_cast<uptr>(&psxNextDeltaCounter))) ||
			!m_code.EmitLdrImm12(HOST_CALL_SCRATCH, HOST_TMP3, 0) ||
			!m_code.EmitSubReg(HOST_TMP3, HOST_TMP0, HOST_SAVED0) ||
			!m_code.EmitCmpReg(HOST_TMP3, HOST_CALL_SCRATCH))
		{
			return false;
		}
		helper_branches.push_back({m_code.EmitBranchPlaceholder(VitaA32::Condition::GE),
			VitaA32::Condition::GE});

		// HOST_TMP2 still holds iopNextEventCycle.low, while r5/r12 keep
		// psxNextStartCounter.low and psxNextDeltaCounter for the second
		// no-work test.
		if (!m_code.EmitSubReg(HOST_TMP2, HOST_TMP2, HOST_SAVED0) ||
			!m_code.EmitCmpReg(HOST_CALL_SCRATCH, HOST_TMP2))
		{
			return false;
		}
		helper_branches.push_back({m_code.EmitBranchPlaceholder(VitaA32::Condition::LT),
			VitaA32::Condition::LT});

		if (!m_code.EmitLdrImm12(HOST_TMP2, HOST_PSX_REGS, static_cast<u16>(INTERRUPT_OFFSET)) ||
			!m_code.EmitCmpImm32(HOST_TMP2, 0))
		{
			return false;
		}
		helper_branches.push_back({m_code.EmitBranchPlaceholder(VitaA32::Condition::NE),
			VitaA32::Condition::NE});

		if (!m_code.EmitMovImm32(HOST_TMP3,
				static_cast<u32>(reinterpret_cast<uptr>(&iopHw[HW_ICTRL & 0xffff]))) ||
			!m_code.EmitLdrImm12(HOST_TMP2, HOST_TMP3, 0) ||
			!m_code.EmitCmpImm32(HOST_TMP2, 0))
		{
			return false;
		}
		const size_t skip_intc = m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);

		if (!m_code.EmitMovImm32(HOST_TMP3,
				static_cast<u32>(reinterpret_cast<uptr>(&iopHw[HW_ISTAT & 0xffff]))) ||
			!m_code.EmitLdrImm12(HOST_TMP2, HOST_TMP3, 0) ||
			!m_code.EmitMovImm32(HOST_TMP3,
				static_cast<u32>(reinterpret_cast<uptr>(&iopHw[HW_IMASK & 0xffff]))) ||
			!m_code.EmitLdrImm12(HOST_TMP3, HOST_TMP3, 0) ||
			!m_code.EmitAndReg(HOST_TMP2, HOST_TMP2, HOST_TMP3, true))
		{
			return false;
		}
		helper_branches.push_back({m_code.EmitBranchPlaceholder(VitaA32::Condition::NE),
			VitaA32::Condition::NE});

		const size_t skip_helper = m_code.EmitBranchPlaceholder();
		if (skip_intc == static_cast<size_t>(-1) || skip_helper == static_cast<size_t>(-1))
			return false;

		const size_t helper_target = m_code.Size();
		// R3000A.cpp::iopEventTest() reads timing, CP0, interrupt, and device
		// state but never observes or mutates GPR words. AAPCS preserves the r6/r8
		// pin hosts through it (including counter/device callbacks), so this
		// operation-specific seam needs neither publication nor reload.
		if (!m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&iopEventTest), HOST_CALL_SCRATCH))
			return false;

		const size_t done_target = m_code.Size();
		if (!m_code.PatchBranch(skip_intc, done_target, VitaA32::Condition::EQ))
			return false;

		for (const HelperBranch& branch : helper_branches)
		{
			if (branch.offset == static_cast<size_t>(-1))
				return false;

			if (!m_code.PatchBranch(branch.offset, helper_target, branch.condition))
				return false;
		}

		return m_code.PatchBranch(skip_helper, done_target);
	}

	bool BlockCompiler::EmitCop0TransferOp(u32 op, bool to_cop0)
	{
		if (to_cop0)
		{
			bool used_known_value = false;
			if (!EmitLoadGprValue(RT(op), HOST_TMP0, &used_known_value) ||
				!m_code.EmitStrImm12(HOST_TMP0, HOST_PSX_REGS, static_cast<u16>(Cp0Offset(RD(op)))))
			{
				return false;
			}
#if defined(VITASX2_QEMU_VALIDATION)
			if (used_known_value)
				++g_qemuIopConstCop0WriteFastPaths;
#endif
			return true;
		}

		if (RT(op) == 0)
			return true;

		return m_code.EmitLdrImm12(HOST_TMP0, HOST_PSX_REGS, static_cast<u16>(Cp0Offset(RD(op)))) &&
			   EmitStoreGpr(RT(op), HOST_TMP0);
	}

	bool BlockCompiler::EmitCop0RfeOp()
	{
		return m_code.EmitLdrImm12(HOST_TMP0, HOST_PSX_REGS, static_cast<u16>(CP0_STATUS_OFFSET)) &&
			   m_code.EmitBicImm32(HOST_TMP2, HOST_TMP0, 0x0f) &&
			   m_code.EmitAndImm32(HOST_TMP0, HOST_TMP0, 0x3cu) &&
			   m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_TMP0, VitaA32::ShiftType::LSR, 2) &&
			   m_code.EmitOrrReg(HOST_TMP2, HOST_TMP2, HOST_TMP0) &&
			   m_code.EmitStrImm12(HOST_TMP2, HOST_PSX_REGS, static_cast<u16>(CP0_STATUS_OFFSET)) &&
			   m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&iopTestIntc), HOST_CALL_SCRATCH);
	}

	bool BlockCompiler::EmitCop2CommandOp(u32 op)
	{
		// PCSX2 owner: R3000AOpcodeTables.cpp::psxCP2 dispatches by function
		// to IopGte.cpp. Direct calls remove the generic COP2 table tail while
		// preserving PCSX2's GTE command implementation.
		const void* helper = nullptr;
		switch (op & 0x3f)
		{
			case 0x01: helper = reinterpret_cast<const void*>(&gteRTPS); break;
			case 0x06: helper = reinterpret_cast<const void*>(&gteNCLIP); break;
			case 0x0c: helper = reinterpret_cast<const void*>(&gteOP); break;
			case 0x10: helper = reinterpret_cast<const void*>(&gteDPCS); break;
			case 0x11: helper = reinterpret_cast<const void*>(&gteINTPL); break;
			case 0x12: helper = reinterpret_cast<const void*>(&gteMVMVA); break;
			case 0x13: helper = reinterpret_cast<const void*>(&gteNCDS); break;
			case 0x14: helper = reinterpret_cast<const void*>(&gteCDP); break;
			case 0x16: helper = reinterpret_cast<const void*>(&gteNCDT); break;
			case 0x1b: helper = reinterpret_cast<const void*>(&gteNCCS); break;
			case 0x1c: helper = reinterpret_cast<const void*>(&gteCC); break;
			case 0x1e: helper = reinterpret_cast<const void*>(&gteNCS); break;
			case 0x20: helper = reinterpret_cast<const void*>(&gteNCT); break;
			case 0x28: helper = reinterpret_cast<const void*>(&gteSQR); break;
			case 0x29: helper = reinterpret_cast<const void*>(&gteDCPL); break;
			case 0x2a: helper = reinterpret_cast<const void*>(&gteDPCT); break;
			case 0x2d: helper = reinterpret_cast<const void*>(&gteAVSZ3); break;
			case 0x2e: helper = reinterpret_cast<const void*>(&gteAVSZ4); break;
			case 0x30: helper = reinterpret_cast<const void*>(&gteRTPT); break;
			case 0x3d: helper = reinterpret_cast<const void*>(&gteGPF); break;
			case 0x3e: helper = reinterpret_cast<const void*>(&gteGPL); break;
			case 0x3f: helper = reinterpret_cast<const void*>(&gteNCCT); break;
			default:
				return false;
		}

		return m_code.EmitCallAbsolute(helper, HOST_CALL_SCRATCH);
	}

	bool BlockCompiler::EmitReadCop2DataReg(unsigned cop2_reg, unsigned host_reg)
	{
		// PCSX2 owner: IopGte.cpp::MFC2(). Register 29 synthesizes ORGB from
		// IR1/IR2/IR3 and stores the synthesized value back into CP2D[29].
		if (cop2_reg != 29)
			return m_code.EmitLdrImm12(host_reg, HOST_PSX_REGS, static_cast<u16>(Cp2dOffset(cop2_reg)));

		if (!m_code.EmitLdrImm12(HOST_TMP0, HOST_PSX_REGS, static_cast<u16>(Cp2dOffset(9))) ||
			!m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_TMP0, VitaA32::ShiftType::LSR, 7) ||
			!m_code.EmitAndImm8(HOST_TMP0, HOST_TMP0, 0x1f) ||
			!m_code.EmitLdrImm12(HOST_TMP1, HOST_PSX_REGS, static_cast<u16>(Cp2dOffset(10))) ||
			!m_code.EmitMovRegShiftImm(HOST_TMP1, HOST_TMP1, VitaA32::ShiftType::LSR, 7) ||
			!m_code.EmitAndImm8(HOST_TMP1, HOST_TMP1, 0x1f) ||
			!m_code.EmitMovRegShiftImm(HOST_TMP1, HOST_TMP1, VitaA32::ShiftType::LSL, 5) ||
			!m_code.EmitOrrReg(HOST_TMP0, HOST_TMP0, HOST_TMP1) ||
			!m_code.EmitLdrImm12(HOST_TMP1, HOST_PSX_REGS, static_cast<u16>(Cp2dOffset(11))) ||
			!m_code.EmitMovRegShiftImm(HOST_TMP1, HOST_TMP1, VitaA32::ShiftType::LSR, 7) ||
			!m_code.EmitAndImm8(HOST_TMP1, HOST_TMP1, 0x1f) ||
			!m_code.EmitMovRegShiftImm(HOST_TMP1, HOST_TMP1, VitaA32::ShiftType::LSL, 10) ||
			!m_code.EmitOrrReg(HOST_TMP0, HOST_TMP0, HOST_TMP1) ||
			!m_code.EmitStrImm12(HOST_TMP0, HOST_PSX_REGS, static_cast<u16>(Cp2dOffset(29))))
		{
			return false;
		}

		if (host_reg == HOST_TMP0)
			return true;

		return m_code.EmitMovRegShiftImm(host_reg, HOST_TMP0, VitaA32::ShiftType::LSL, 0);
	}

	bool BlockCompiler::EmitWriteCop2DataReg(unsigned cop2_reg, unsigned host_reg)
	{
		// PCSX2 owner: IopGte.cpp::MTC2(). These are GTE data-register side
		// effects, not generic coprocessor transfers.
		switch (cop2_reg)
		{
			case 8:
			case 9:
			case 10:
			case 11:
				return m_code.EmitSxth(HOST_TMP1, host_reg) &&
					   m_code.EmitStrImm12(HOST_TMP1, HOST_PSX_REGS, static_cast<u16>(Cp2dOffset(cop2_reg)));

			case 15:
				return m_code.EmitLdrImm12(HOST_TMP1, HOST_PSX_REGS, static_cast<u16>(Cp2dOffset(13))) &&
					   m_code.EmitLdrImm12(HOST_TMP2, HOST_PSX_REGS, static_cast<u16>(Cp2dOffset(14))) &&
					   m_code.EmitStrImm12(HOST_TMP1, HOST_PSX_REGS, static_cast<u16>(Cp2dOffset(12))) &&
					   m_code.EmitStrImm12(HOST_TMP2, HOST_PSX_REGS, static_cast<u16>(Cp2dOffset(13))) &&
					   m_code.EmitStrImm12(host_reg, HOST_PSX_REGS, static_cast<u16>(Cp2dOffset(14))) &&
					   m_code.EmitStrImm12(host_reg, HOST_PSX_REGS, static_cast<u16>(Cp2dOffset(15)));

			case 16:
			case 17:
			case 18:
			case 19:
				return m_code.EmitUxth(HOST_TMP1, host_reg) &&
					   m_code.EmitStrImm12(HOST_TMP1, HOST_PSX_REGS, static_cast<u16>(Cp2dOffset(cop2_reg)));

			case 28:
				return m_code.EmitStrImm12(host_reg, HOST_PSX_REGS, static_cast<u16>(Cp2dOffset(28))) &&
					   m_code.EmitAndImm8(HOST_TMP1, host_reg, 0x1f) &&
					   m_code.EmitMovRegShiftImm(HOST_TMP1, HOST_TMP1, VitaA32::ShiftType::LSL, 7) &&
					   m_code.EmitStrImm12(HOST_TMP1, HOST_PSX_REGS, static_cast<u16>(Cp2dOffset(9))) &&
					   m_code.EmitMovRegShiftImm(HOST_TMP1, host_reg, VitaA32::ShiftType::LSR, 5) &&
					   m_code.EmitAndImm8(HOST_TMP1, HOST_TMP1, 0x1f) &&
					   m_code.EmitMovRegShiftImm(HOST_TMP1, HOST_TMP1, VitaA32::ShiftType::LSL, 7) &&
					   m_code.EmitStrImm12(HOST_TMP1, HOST_PSX_REGS, static_cast<u16>(Cp2dOffset(10))) &&
					   m_code.EmitMovRegShiftImm(HOST_TMP1, host_reg, VitaA32::ShiftType::LSR, 10) &&
					   m_code.EmitAndImm8(HOST_TMP1, HOST_TMP1, 0x1f) &&
					   m_code.EmitMovRegShiftImm(HOST_TMP1, HOST_TMP1, VitaA32::ShiftType::LSL, 7) &&
					   m_code.EmitStrImm12(HOST_TMP1, HOST_PSX_REGS, static_cast<u16>(Cp2dOffset(11)));

			case 30:
				return m_code.EmitStrImm12(host_reg, HOST_PSX_REGS, static_cast<u16>(Cp2dOffset(30))) &&
					   m_code.EmitMovRegShiftImm(HOST_TMP1, host_reg, VitaA32::ShiftType::ASR, 31) &&
					   m_code.EmitEorReg(HOST_TMP1, host_reg, HOST_TMP1) &&
					   m_code.EmitClz(HOST_TMP1, HOST_TMP1) &&
					   m_code.EmitStrImm12(HOST_TMP1, HOST_PSX_REGS, static_cast<u16>(Cp2dOffset(31)));

			default:
				return m_code.EmitStrImm12(host_reg, HOST_PSX_REGS, static_cast<u16>(Cp2dOffset(cop2_reg)));
		}
	}

	bool BlockCompiler::EmitNativeCOP2(u32 op)
	{
		if ((op & 0x3f) != 0)
			return EmitCop2CommandOp(op);

		switch (RS(op))
		{
			case 0x00: // MFC2
				if (RT(op) == 0)
					return true;
				return EmitReadCop2DataReg(RD(op), HOST_TMP0) &&
					   EmitStoreGpr(RT(op), HOST_TMP0);

			case 0x02: // CFC2
				if (RT(op) == 0)
					return true;
				return m_code.EmitLdrImm12(HOST_TMP0, HOST_PSX_REGS, static_cast<u16>(Cp2cOffset(RD(op)))) &&
					   EmitStoreGpr(RT(op), HOST_TMP0);

			case 0x04: // MTC2
			{
				bool used_known_value = false;
				if (!EmitLoadGprValue(RT(op), HOST_TMP0, &used_known_value) ||
					!EmitWriteCop2DataReg(RD(op), HOST_TMP0))
				{
					return false;
				}
#if defined(VITASX2_QEMU_VALIDATION)
				if (used_known_value)
					++g_qemuIopConstCop2WriteFastPaths;
#endif
				return true;
			}

			case 0x06: // CTC2
			{
				bool used_known_value = false;
				if (!EmitLoadGprValue(RT(op), HOST_TMP0, &used_known_value) ||
					!m_code.EmitStrImm12(HOST_TMP0, HOST_PSX_REGS, static_cast<u16>(Cp2cOffset(RD(op)))))
				{
					return false;
				}
#if defined(VITASX2_QEMU_VALIDATION)
				if (used_known_value)
					++g_qemuIopConstCop2WriteFastPaths;
#endif
				return true;
			}

			default:
				return false;
		}
	}

	bool BlockCompiler::EmitKnownDirectRamCop2LoadOp(u32 op, u32 address)
	{
#if defined(VITASX2_QEMU_VALIDATION)
		++g_qemuIopConstRamCop2LoadFastPaths;
#endif

		const auto emit_load_word = [&]() -> bool {
			if (address <= 0x0fffu)
				return m_code.EmitLdrImm12(HOST_TMP0, HOST_IOP_RAM_BASE, static_cast<u16>(address));

			return m_code.EmitMovImm32(HOST_TMP0, address) &&
				   m_code.EmitLdrRegShift(HOST_TMP0, HOST_IOP_RAM_BASE, HOST_TMP0,
					   VitaA32::ShiftType::LSL, 0);
		};

		// PCSX2 owners: IopGte.cpp::gteLWC2()/MTC2() and
		// IopMem.cpp::iopMemRead32(). A proven aligned main-RAM address can
		// skip the runtime alias/MMIO split before applying MTC2 side effects.
		return emit_load_word() && EmitWriteCop2DataReg(RT(op), HOST_TMP0);
	}

	bool BlockCompiler::EmitKnownDirectRamCop2StoreOp(u32 op, u32 address)
	{
#if defined(VITASX2_QEMU_VALIDATION)
		++g_qemuIopConstRamCop2StoreFastPaths;
#endif

		const auto emit_store_word = [&]() -> bool {
			if (address <= 0x0fffu)
				return m_code.EmitStrImm12(HOST_SAVED0, HOST_IOP_RAM_BASE, static_cast<u16>(address));

			return m_code.EmitMovImm32(HOST_TMP0, address) &&
				   m_code.EmitStrRegShift(HOST_SAVED0, HOST_IOP_RAM_BASE, HOST_TMP0,
					   VitaA32::ShiftType::LSL, 0);
		};
		const auto emit_clear_stored_word = [&]() -> bool {
			return m_code.EmitMovImm32(HOST_TMP0, address & ~3u) &&
				   m_code.EmitMovImm8(HOST_TMP1, 1) &&
				   m_code.EmitMovImm32(HOST_CALL_SCRATCH,
					   static_cast<u32>(reinterpret_cast<uptr>(&psxCpu))) &&
				   m_code.EmitLdrImm12(HOST_CALL_SCRATCH, HOST_CALL_SCRATCH, 0) &&
				   m_code.EmitLdrImm12(HOST_CALL_SCRATCH, HOST_CALL_SCRATCH,
					   static_cast<u16>(offsetof(R3000Acpu, Clear))) &&
				   m_code.EmitBlx(HOST_CALL_SCRATCH);
		};

		// PCSX2 owners: IopGte.cpp::gteSWC2()/MFC2() and
		// IopMem.cpp::iopMemWrite32(). Keep MFC2 synthesis, isolate-cache
		// suppression, and code invalidation while skipping the runtime RAM
		// address/mask path.
		if (!EmitReadCop2DataReg(RT(op), HOST_SAVED0) ||
			!m_code.EmitLdrImm12(HOST_TMP2, HOST_PSX_REGS, static_cast<u16>(CP0_STATUS_OFFSET)) ||
			!m_code.EmitTstImm32(HOST_TMP2, 0x10000u))
		{
			return false;
		}

		const size_t isolated_skip = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
		if (isolated_skip == static_cast<size_t>(-1) ||
			!emit_store_word() ||
			!emit_clear_stored_word())
		{
			return false;
		}

		return m_code.PatchBranch(isolated_skip, m_code.Size(), VitaA32::Condition::NE);
	}

	bool BlockCompiler::EmitCop2LoadStoreOp(u32 op)
	{
		u32 known_ram_address = 0;
		if (TryKnownDirectIopRamAddress(op, 3, &known_ram_address))
		{
			if ((op >> 26) == 0x32) // LWC2
				return EmitKnownDirectRamCop2LoadOp(op, known_ram_address);
			if ((op >> 26) == 0x3a) // SWC2
				return EmitKnownDirectRamCop2StoreOp(op, known_ram_address);
		}

		if ((op >> 26) == 0x32) // LWC2
		{
			if (!EmitEffectiveAddress(op) ||
				!m_code.EmitMovRegShiftImm(HOST_SAVED0, HOST_TMP0, VitaA32::ShiftType::LSL, 0) ||
				!m_code.EmitTstImm32(HOST_TMP0, 0x10000000u))
			{
				return false;
			}

			// PCSX2 owner: IopGte.cpp::gteLWC2() reads with iopMemRead32() before
			// MTC2 side effects. Mirror rpsxLoad's ordinary-RAM fast split here.
			const size_t fallback_branch = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (fallback_branch == static_cast<size_t>(-1) ||
				!m_code.EmitAndImm8(HOST_TMP2, HOST_SAVED0, 3, true))
			{
				return false;
			}

			const size_t alignment_fallback_branch = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (alignment_fallback_branch == static_cast<size_t>(-1) ||
				!m_code.EmitAndReg(HOST_TMP0, HOST_SAVED0, HOST_IOP_RAM_MASK) ||
				!m_code.EmitLdrRegShift(HOST_TMP0, HOST_IOP_RAM_BASE, HOST_TMP0,
					VitaA32::ShiftType::LSL, 0))
			{
				return false;
			}

			if (!EmitWriteCop2DataReg(RT(op), HOST_TMP0))
				return false;

			m_cop2_load_cold_tails.push_back({
				fallback_branch,
				alignment_fallback_branch,
				m_code.Size(),
				RT(op),
				m_current_instruction_count,
			});
			return true;
		}

		if ((op >> 26) == 0x3a) // SWC2
		{
			const auto emit_clear_stored_word = [&]() -> bool {
				return m_code.EmitBicImm32(HOST_TMP0, HOST_SAVED1, 3) &&
					   m_code.EmitMovImm8(HOST_TMP1, 1) &&
					   m_code.EmitMovImm32(HOST_CALL_SCRATCH,
						   static_cast<u32>(reinterpret_cast<uptr>(&psxCpu))) &&
					   m_code.EmitLdrImm12(HOST_CALL_SCRATCH, HOST_CALL_SCRATCH, 0) &&
					   m_code.EmitLdrImm12(HOST_CALL_SCRATCH, HOST_CALL_SCRATCH,
						   static_cast<u16>(offsetof(R3000Acpu, Clear))) &&
					   m_code.EmitBlx(HOST_CALL_SCRATCH);
			};

			if (!EmitReadCop2DataReg(RT(op), HOST_SAVED0) ||
				!EmitEffectiveAddress(op) ||
				!m_code.EmitMovRegShiftImm(HOST_SAVED1, HOST_TMP0, VitaA32::ShiftType::LSL, 0) ||
				!m_code.EmitTstImm32(HOST_TMP0, 0x10000000u))
			{
				return false;
			}

			// PCSX2 owner: IopGte.cpp::gteSWC2() writes MFC2(_Rt_) through
			// iopMemWrite32(). Ordinary writable RAM can store directly, but
			// must retain iopMemWrite32()'s isolate-cache and invalidation rules.
			const size_t fallback_branch = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (fallback_branch == static_cast<size_t>(-1) ||
				!m_code.EmitAndImm8(HOST_TMP2, HOST_SAVED1, 3, true))
			{
				return false;
			}

			const size_t alignment_fallback_branch = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (alignment_fallback_branch == static_cast<size_t>(-1) ||
				!m_code.EmitLdrImm12(HOST_TMP2, HOST_PSX_REGS, static_cast<u16>(CP0_STATUS_OFFSET)) ||
				!m_code.EmitTstImm32(HOST_TMP2, 0x10000u))
			{
				return false;
			}

			const size_t isolated_fallback_branch = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (isolated_fallback_branch == static_cast<size_t>(-1) ||
				!m_code.EmitAndReg(HOST_TMP0, HOST_SAVED1, HOST_IOP_RAM_MASK) ||
				!m_code.EmitStrRegShift(HOST_SAVED0, HOST_IOP_RAM_BASE, HOST_TMP0,
					VitaA32::ShiftType::LSL, 0) ||
				!emit_clear_stored_word())
			{
				return false;
			}

			m_cop2_store_cold_tails.push_back({
				fallback_branch,
				alignment_fallback_branch,
				isolated_fallback_branch,
				m_code.Size(),
				m_current_instruction_count,
			});
			return true;
		}

		return false;
	}

	bool BlockCompiler::EmitNativeSPECIAL(u32 op, u32 pc)
	{
		switch (op & 0x3f)
		{
			case 0x00: // SLL
			case 0x02: // SRL
			case 0x03: // SRA
				return EmitShiftImmOp(op);
			case 0x04: // SLLV
			case 0x06: // SRLV
			case 0x07: // SRAV
				return EmitShiftRegOp(op);
			case 0x08: // JR
			case 0x09: // JALR
				return EmitRegisterJumpOp(op, pc);
			case 0x0c: // SYSCALL
				return EmitExceptionOp(pc, 0x20);
			case 0x0d: // BREAK
				return EmitExceptionOp(pc, 0x24);
			case 0x10: // MFHI
			{
				u32 known_hi = 0;
				if (RD(op) != 0 && TryGetKnownHiLo(false, &known_hi))
				{
#if defined(VITASX2_QEMU_VALIDATION)
					++g_qemuIopConstHiLoReadFastPaths;
#endif
					return m_code.EmitMovImm32(HOST_TMP0, known_hi) &&
						   EmitStoreGpr(RD(op), HOST_TMP0);
				}
				return EmitMoveGpr(RD(op), 32);
			}
			case 0x11: // MTHI
			{
				bool used_known_value = false;
				if (!EmitLoadGprValue(RS(op), HOST_TMP0, &used_known_value) ||
					!m_code.EmitStrImm12(HOST_TMP0, HOST_PSX_REGS, static_cast<u16>(HI_OFFSET)))
				{
					return false;
				}
#if defined(VITASX2_QEMU_VALIDATION)
				if (used_known_value)
					++g_qemuIopConstHiLoWriteFastPaths;
#endif
				return true;
			}
			case 0x12: // MFLO
			{
				u32 known_lo = 0;
				if (RD(op) != 0 && TryGetKnownHiLo(true, &known_lo))
				{
#if defined(VITASX2_QEMU_VALIDATION)
					++g_qemuIopConstHiLoReadFastPaths;
#endif
					return m_code.EmitMovImm32(HOST_TMP0, known_lo) &&
						   EmitStoreGpr(RD(op), HOST_TMP0);
				}
				return EmitMoveGpr(RD(op), 33);
			}
			case 0x13: // MTLO
			{
				bool used_known_value = false;
				if (!EmitLoadGprValue(RS(op), HOST_TMP0, &used_known_value) ||
					!m_code.EmitStrImm12(HOST_TMP0, HOST_PSX_REGS, static_cast<u16>(LO_OFFSET)))
				{
					return false;
				}
#if defined(VITASX2_QEMU_VALIDATION)
				if (used_known_value)
					++g_qemuIopConstHiLoWriteFastPaths;
#endif
				return true;
			}
			case 0x18: // MULT
				return EmitMultiplyOp(op, true);
			case 0x19: // MULTU
				return EmitMultiplyOp(op, false);
			case 0x1a: // DIV
				return EmitDivideOp(op, true);
			case 0x1b: // DIVU
				return EmitDivideOp(op, false);
			case 0x20: // ADD
			case 0x21: // ADDU
			case 0x22: // SUB
			case 0x23: // SUBU
			case 0x24: // AND
			case 0x25: // OR
			case 0x26: // XOR
			case 0x27: // NOR
				return EmitBinaryRegOp(op);
			case 0x2a: // SLT
				return EmitSetLessThanRegOp(op, true);
			case 0x2b: // SLTU
				return EmitSetLessThanRegOp(op, false);
			default:
				return false;
		}
	}

	bool BlockCompiler::EmitNativeCOP0(u32 op)
	{
		switch (RS(op))
		{
			case 0x00: // MFC0
			case 0x02: // CFC0
				return EmitCop0TransferOp(op, false);
			case 0x04: // MTC0
			case 0x06: // CTC0
				return EmitCop0TransferOp(op, true);
			case 0x10: // RFE
				return EmitCop0RfeOp();
			default:
				return false;
		}
	}

	bool BlockCompiler::EmitNativeInstruction(u32 op, u32 pc)
	{
		switch (op >> 26)
		{
			case 0x00: // SPECIAL
				return EmitNativeSPECIAL(op, pc);
			case 0x01: // REGIMM
				return EmitSignedBranchOp(op, pc);
			case 0x02: // J
			case 0x03: // JAL
				return EmitJumpOp(op, pc);
			case 0x04: // BEQ
			case 0x05: // BNE
				return EmitConditionalBranchOp(op, pc);
			case 0x06: // BLEZ
			case 0x07: // BGTZ
				return EmitSignedBranchOp(op, pc);
			case 0x08: // ADDI
			case 0x09: // ADDIU
			case 0x0a: // SLTI
			case 0x0b: // SLTIU
			case 0x0c: // ANDI
			case 0x0d: // ORI
			case 0x0e: // XORI
			case 0x0f: // LUI
				return EmitImmediateOp(op);
			case 0x10: // COP0
				return EmitNativeCOP0(op);
			case 0x12: // COP2
				return EmitNativeCOP2(op);
			case 0x20: // LB
			case 0x21: // LH
			case 0x23: // LW
			case 0x24: // LBU
			case 0x25: // LHU
				return EmitLoadOp(op);
			case 0x22: // LWL
			case 0x26: // LWR
				return EmitUnalignedLoadOp(op);
			case 0x28: // SB
			case 0x29: // SH
			case 0x2b: // SW
				return EmitStoreOp(op);
			case 0x2a: // SWL
			case 0x2e: // SWR
				return EmitUnalignedStoreOp(op);
			case 0x32: // LWC2
			case 0x3a: // SWC2
				return EmitCop2LoadStoreOp(op);
			default:
				return false;
		}
	}

	bool BlockCompiler::EmitInstruction(u32 op, u32 pc, bool store_pc, std::vector<size_t>& trace_exit_branches)
	{
		const u32 next_pc = pc + 4;
		if (((m_emit_trace_checks || IopInstructionRequiresCodeState(op)) && !EmitStoreCode(op)) ||
			(m_emit_trace_checks && !EmitTraceCheck(pc, op, trace_exit_branches)) ||
			(store_pc && !EmitStorePc(next_pc)) ||
			(!m_defer_cycle_updates && !EmitIncrementCycle()))
		{
			return false;
		}

		// PCSX2 owner: R3000AOpcodeTables.cpp implements the full valid IOP
		// table; x86/iR3000Atables.cpp lowers the same table to native host
		// operations. CanCompileOpcode() has already rejected psxNULL slots.
		if (!IsNativeOpcode(op) || !EmitNativeInstruction(op, pc))
			return false;

		UpdateGprConstStateAfterOpcode(op, pc);
		m_native_instruction_count++;
		return true;
	}

	bool BlockCompiler::CompileStraightLineBlock(u32 start_pc, u32 instruction_count,
		const void* direct_exit, DirectLinkSlots* direct_links)
	{
		if (instruction_count == 0 ||
			instruction_count > BlockExecutor::MAX_STRAIGHT_LINE_BLOCK_INSTRUCTIONS ||
			instruction_count > ((UINT32_MAX - start_pc) / 4))
		{
			return false;
		}

		if (direct_links)
			*direct_links = {};

		if (EmuConfig.Speedhacks.WaitLoop && !VitaIsIopPreInstructionTraceEnabled() &&
			instruction_count >= 2)
		{
			const u32 branch_index = instruction_count - 2;
			const u32 branch_pc = start_pc + branch_index * 4;
			const u32 branch_op = iopMemRead32(branch_pc);
			bool only_nops = true;
			for (u32 i = 0; i < instruction_count; i++)
			{
				if (i != branch_index && iopMemRead32(start_pc + i * 4) != 0)
				{
					only_nops = false;
					break;
				}
			}

			// The retail dominant unconditional form can use the compact callable
			// too. Conditional shapes keep their ordinary callable for the not-taken
			// path; BlockExecutor bypasses it only when the cached taken edge is live.
			if ((branch_op >> 26) == 0x02 && JumpTarget(branch_pc, branch_op) == start_pc && only_nops)
				return EmitWaitLoopFastForwardBlock(start_pc, instruction_count);
		}

		m_iop_ram_registers_available = false;
		m_iop_ram_mask_register_available = false;
		m_static_branch_outcome_known = false;
		m_static_branch_taken = false;
		m_register_jump_target_known = false;
		m_register_jump_target = 0;
		u32 runtime_memory_helper_seams = 0;
		u32 runtime_unaligned_store_saves = 0;
		ResetGprConstState();
		for (u32 i = 0; i < instruction_count; i++)
		{
			const u32 pc = start_pc + i * 4;
			const u32 op = iopMemRead32(pc);
			if (UsesDirectIopRamFastPath(op))
			{
				m_iop_ram_registers_available = true;
				const u8 alignment_mask = DirectIopRamAlignmentMask(op);
				u32 known_address = 0;
				const bool known_direct =
					CanEmitKnownDirectIopRamFastPath(op) &&
					TryKnownDirectIopRamAddress(op, alignment_mask, &known_address);
				if (!known_direct)
				{
					m_iop_ram_mask_register_available = true;
					const bool unaligned_store = (op >> 26) == 0x2a || (op >> 26) == 0x2e;
					runtime_memory_helper_seams += unaligned_store ? 2 : 1;
					runtime_unaligned_store_saves += unaligned_store ? 1 : 0;
				}
			}
			UpdateGprConstStateAfterOpcode(op, pc);
		}
		ResetGprConstState();
		m_emit_trace_checks = VitaIsIopPreInstructionTraceEnabled();
		const bool legacy_cycle_deferral = IopBlockCanDeferCycleUpdates(start_pc, instruction_count);
		bool block_cycle_batching = true;
#if defined(VITASX2_QEMU_VALIDATION)
		block_cycle_batching = s_qemuIopBlockCycleBatchingEnabled;
#endif
		// Runtime-address memory paths track how much of the compile-time block
		// delta has already been published, so MMIO/timer handlers observe the
		// same instruction boundary while direct RAM stays private until exit.
		const u32 runtime_cycle_budget_instructions =
			(psxHu32(HW_ICFG) & (1u << 3)) != 0 ? 7u : 6u;
		const bool runtime_memory_batching_profitable =
			4u * instruction_count >=
				9u * runtime_memory_helper_seams + runtime_cycle_budget_instructions +
					runtime_unaligned_store_saves;
		const bool can_batch_cycle_updates =
			(!m_iop_ram_mask_register_available ||
				(instruction_count >= 2 && runtime_memory_batching_profitable)) &&
			IopBlockCanBatchCycleUpdates(start_pc, instruction_count);
		m_defer_cycle_updates = !m_emit_trace_checks &&
			(legacy_cycle_deferral || (block_cycle_batching && can_batch_cycle_updates));
		m_expanded_cycle_batching = m_defer_cycle_updates && !legacy_cycle_deferral;
		m_track_published_cycle_prefix =
			m_expanded_cycle_batching && m_iop_ram_mask_register_available;
		m_has_budget_exit = false;
		m_block_cycle_count = instruction_count;
		m_batched_cycle_instructions_removed =
			m_expanded_cycle_batching ? UINT32_MAX : 0;
		m_batched_cycle_stack_words_removed =
			(m_expanded_cycle_batching && !m_track_published_cycle_prefix) ? 2 : 0;
		m_current_instruction_count = 0;
		AnalyzePinnedGprs(start_pc, instruction_count);
		AnalyzeSavedRegisters(start_pc, instruction_count);

		if (!BeginBlock())
			return false;

		std::vector<size_t> direct_exit_branches;
		std::vector<size_t> trace_exit_branches;
		std::vector<size_t> budget_exit_branches;
		m_direct_exit_branches = &direct_exit_branches;
		m_budget_exit_branches = &budget_exit_branches;
		direct_exit_branches.reserve(instruction_count * 2);
		trace_exit_branches.reserve(instruction_count);
		budget_exit_branches.reserve(4);
		m_native_instruction_count = 0;
		m_helper_instruction_count = 0;
		bool can_direct_link_fallthrough = true;
		bool has_native_static_branch = false;
		bool has_native_static_jump = false;
		bool has_native_register_jump = false;
		u32 static_branch_target_pc = 0;
		u32 static_branch_fallthrough_pc = 0;
		u32 static_jump_target_pc = 0;
		for (u32 i = 0; i < instruction_count; i++)
		{
			const u32 pc = start_pc + i * 4;
			const u32 op = iopMemRead32(pc);
			const u32 delay_op = (i + 1 < instruction_count) ? iopMemRead32(pc + 4) : 0;
			const bool can_native_static_branch =
				IsIopStaticConditionalBranchOpcode(op) &&
				i + 2 == instruction_count &&
				!IsIopBranchOrJumpOpcode(delay_op) &&
				!IsIopExceptionOpcode(delay_op);
			const bool can_native_static_jump =
				IsIopStaticJumpOpcode(op) &&
				i + 2 == instruction_count &&
				!IsIopBranchOrJumpOpcode(delay_op) &&
				!IsIopExceptionOpcode(delay_op) &&
				((op >> 26) != 0x02 || (delay_op >> 16) != 0x2400);
			const bool can_native_register_jump =
				IsIopRegisterJumpOpcode(op) &&
				i + 2 == instruction_count &&
				!IsIopBranchOrJumpOpcode(delay_op) &&
				!IsIopExceptionOpcode(delay_op);
			m_emit_native_static_branch = can_native_static_branch;
			m_emit_native_static_jump = can_native_static_jump;
			m_emit_native_register_jump = can_native_register_jump;
			if (can_native_static_branch)
			{
				has_native_static_branch = true;
				static_branch_target_pc = BranchTarget(pc, op);
				static_branch_fallthrough_pc = pc + 8;
			}
			if (can_native_static_jump)
			{
				has_native_static_jump = true;
				static_jump_target_pc = JumpTarget(pc, op);
			}
			if (can_native_register_jump)
				has_native_register_jump = true;
			if (IsIopBranchOrJumpOpcode(op) || IsIopExceptionOpcode(op))
				can_direct_link_fallthrough = false;
			const bool store_pc =
				m_emit_trace_checks || (i + 1 == instruction_count) || IopInstructionRequiresPcState(op);
			m_current_instruction_count = i + 1;
			const bool emitted = CanCompileOpcode(op) && EmitInstruction(op, pc, store_pc, trace_exit_branches);
			m_emit_native_static_branch = false;
			m_emit_native_static_jump = false;
			m_emit_native_register_jump = false;
			if (!emitted)
				return false;
		}

		const u32 next_pc = start_pc + instruction_count * 4;
		if (m_defer_cycle_updates && !EmitPublishCyclePrefix(instruction_count))
			return false;
		if (m_expanded_cycle_batching)
			RecordBatchedCycleExitSavings(instruction_count, false);

		size_t direct_exit_offset = 0;
		const bool emit_link_tail = direct_exit && direct_links && can_direct_link_fallthrough;
		const bool emit_branch_link_tails = direct_exit && direct_links && has_native_static_branch;
		const auto emit_direct_or_return_tail = [&](u32 target_pc, u8 slot_index) -> bool {
			if (emit_branch_link_tails)
			{
				DirectLinkSlot& link = direct_links->slots[slot_index];
				if (!EndBlockDirectTail(direct_exit, &link))
					return false;

				link.target_pc = target_pc;
				link.valid = true;
				return true;
			}

			return EndBlockReturn(BlockExitKind::Direct);
		};

		if (has_native_static_branch)
		{
			if (m_static_branch_outcome_known)
			{
				const u32 target_pc = m_static_branch_taken ? static_branch_target_pc : static_branch_fallthrough_pc;
				const u8 link_slot = m_static_branch_taken ? 1 : 0;
#if defined(VITASX2_QEMU_VALIDATION)
				++g_qemuIopConstStaticBranchTailFastPaths;
#endif
				if (!EmitStorePc(target_pc))
					return false;

				if (m_static_branch_taken &&
					(!EmitIopEventTestFastPath() ||
						!EmitPcChangedExitCheck(target_pc, direct_exit_branches)))
				{
					return false;
				}

				if (!emit_direct_or_return_tail(target_pc, link_slot))
					return false;

				direct_exit_offset = m_code.Size();
				if (!EndBlockReturn(BlockExitKind::Direct, false))
					return false;
			}
			else
			{
				if (!m_code.EmitCmpImm32(HOST_BRANCH_FLAG, 0))
				{
					return false;
				}

				const size_t taken_path = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
				if (taken_path == static_cast<size_t>(-1))
					return false;

				if (!EmitStorePc(static_branch_fallthrough_pc) ||
					!emit_direct_or_return_tail(static_branch_fallthrough_pc, 0))
				{
					return false;
				}

				const size_t taken_path_target = m_code.Size();
				if (!m_code.PatchBranch(taken_path, taken_path_target, VitaA32::Condition::NE) ||
					!EmitStorePc(static_branch_target_pc) ||
					!EmitIopEventTestFastPath() ||
					!EmitPcChangedExitCheck(static_branch_target_pc, direct_exit_branches) ||
					!emit_direct_or_return_tail(static_branch_target_pc, 1))
				{
					return false;
				}

				direct_exit_offset = m_code.Size();
				if (!EndBlockReturn(BlockExitKind::Direct, false))
					return false;
			}
		}
		else if (has_native_static_jump)
		{
			if (!EmitStorePc(static_jump_target_pc) ||
				!EmitIopEventTestFastPath() ||
				!EmitPcChangedExitCheck(static_jump_target_pc, direct_exit_branches))
			{
				return false;
			}

			if (direct_exit && direct_links)
			{
				DirectLinkSlot& link = direct_links->slots[0];
				if (!EndBlockDirectTail(direct_exit, &link))
					return false;

				link.target_pc = static_jump_target_pc;
				link.valid = true;
			}
			else if (!EndBlockReturn(BlockExitKind::Direct))
			{
				return false;
			}

			direct_exit_offset = m_code.Size();
			if (!EndBlockReturn(BlockExitKind::Direct, false))
				return false;
		}
		else if (has_native_register_jump)
		{
			if (m_register_jump_target_known)
			{
				if (!EmitStorePc(m_register_jump_target) ||
					!EmitIopEventTestFastPath() ||
					!EmitPcChangedExitCheck(m_register_jump_target, direct_exit_branches))
				{
					return false;
				}

				if (direct_exit && direct_links)
				{
					DirectLinkSlot& link = direct_links->slots[0];
					if (!EndBlockDirectTail(direct_exit, &link))
						return false;

					link.target_pc = m_register_jump_target;
					link.valid = true;
				}
				else if (!EndBlockReturn(BlockExitKind::Direct))
				{
					return false;
				}
			}
			else
			{
				if (!EmitStorePcReg(HOST_REGISTER_JUMP_TARGET) ||
					!EmitIopEventTestFastPath() ||
					!EmitPcChangedExitCheckReg(HOST_REGISTER_JUMP_TARGET, direct_exit_branches) ||
					!EndBlockReturn(BlockExitKind::Direct))
				{
					return false;
				}
			}

			direct_exit_offset = m_code.Size();
			if (!EndBlockReturn(BlockExitKind::Direct, false))
				return false;
		}
		else if (emit_link_tail)
		{
			DirectLinkSlot& link = direct_links->slots[0];
			if (!EndBlockDirectTail(direct_exit, &link))
				return false;

			link.target_pc = next_pc;
			link.valid = true;

			direct_exit_offset = m_code.Size();
			if (!EndBlockReturn(BlockExitKind::Direct, false))
				return false;
		}
		else
		{
			direct_exit_offset = m_code.Size();
			if (!EndBlockReturn(BlockExitKind::Direct, false))
				return false;
		}

		u32 written_pin_count = 0;
		for (u8 i = 0; i < m_pinned_gpr_count; i++)
			written_pin_count += m_pinned_gprs[i].ever_written ? 1u : 0u;
		const u32 removed_gpr_memory_ops = m_pinned_gpr_load_hits + m_pinned_gpr_store_hits;
		const u32 added_gpr_memory_ops = m_pinned_gpr_initial_loads + written_pin_count;
		m_pinned_gpr_memory_ops_saved =
			removed_gpr_memory_ops > added_gpr_memory_ops ?
				removed_gpr_memory_ops - added_gpr_memory_ops : 0;
		if (m_pinned_gpr_min_exit_savings != UINT32_MAX)
		{
			m_pinned_gpr_memory_ops_saved =
				std::min(m_pinned_gpr_memory_ops_saved, m_pinned_gpr_min_exit_savings);
		}

		if (!FlushColdTails())
			return false;

		for (const size_t branch_offset : direct_exit_branches)
		{
			if (!m_code.PatchBranch(branch_offset, direct_exit_offset, VitaA32::Condition::NE))
				return false;
		}

		// Trace callbacks and early branch-helper budget checks publish the state
		// belonging to their own control-flow path before branching. They must
		// bypass the ordinary final-state flush: a later fallthrough write-first
		// pin has no valid host value on an earlier exit. Ordinary blocks keep the
		// compact existing tail because their budget exit owns final-path state.
		const bool has_early_branch_helper = m_pinned_gpr_min_exit_savings != UINT32_MAX;
		const bool needs_published_state_exit =
			has_early_branch_helper || !trace_exit_branches.empty();
		const size_t published_state_exit_offset = needs_published_state_exit ? m_code.Size() : direct_exit_offset;
		if (needs_published_state_exit && !EndBlockReturn(BlockExitKind::Direct, false, false))
			return false;
		for (const size_t branch_offset : trace_exit_branches)
		{
			if (!m_code.PatchBranch(branch_offset, published_state_exit_offset, VitaA32::Condition::NE))
				return false;
		}
		for (const size_t branch_offset : budget_exit_branches)
		{
			if (!m_code.PatchBranch(branch_offset, published_state_exit_offset, VitaA32::Condition::LE))
				return false;
		}

		m_direct_exit_branches = nullptr;
		m_budget_exit_branches = nullptr;
		return true;
	}

	BlockExecutor::BlockExecutor()
	{
		m_cache.reserve(INITIAL_CACHE_CAPACITY);
		m_free_cache_entries.reserve(INITIAL_CACHE_CAPACITY);
		m_block_records.reserve(INITIAL_CACHE_CAPACITY);
		m_incoming_links.reserve(INITIAL_CACHE_CAPACITY * DIRECT_LINK_SLOT_COUNT);
	}

	BlockExecutor::~BlockExecutor()
	{
		Reset();
		ReleaseLookupPages();
		ReleaseCodeCache();
	}

	void BlockExecutor::ResetInstrumentationCounters()
	{
#if defined(VITASX2_QEMU_VALIDATION)
		m_hot_dispatch_cache_hits = 0;
		m_hot_dispatch_cache_misses = 0;
		m_hot_dispatch_trusted_raw_hits = 0;
		m_direct_budget_exit_provider_entries = 0;
		m_constant_cycle_budget_provider_entries = 0;
		m_validation_calls = 0;
		m_validation_words = 0;
		m_raw_validation_calls = 0;
		m_raw_validation_words = 0;
		m_translated_validation_words = 0;
		m_wait_loop_configuration_checks = 0;
		m_trusted_source_hits = 0;
		m_trusted_source_audit_words = 0;
		m_trusted_source_audit_failures = 0;
		m_ram_invalidation_calls = 0;
		m_ram_invalidation_record_visits = 0;
		m_clock_mode_check_instructions_removed = 0;
		m_saved_register_stack_words_removed = 0;
		m_saved_register_frame_instructions_added = 0;
		m_saved_register_frame_instructions_removed = 0;
		m_batched_cycle_instructions_removed = 0;
		m_batched_cycle_stack_words_removed = 0;
		m_expanded_cycle_batching_provider_entries = 0;
#endif
	}

	void BlockExecutor::SetTrustedSourceAuditEnabled(bool enabled)
	{
#if defined(VITASX2_QEMU_VALIDATION)
		s_qemuIopTrustedSourceAuditEnabled = enabled;
#else
		(void)enabled;
#endif
	}

	void BlockExecutor::SetPinnedGprResidencyEnabled(bool enabled)
	{
#if defined(VITASX2_QEMU_VALIDATION)
		s_qemuIopPinnedGprResidencyEnabled = enabled;
#else
		(void)enabled;
#endif
	}

	void BlockExecutor::SetClockModeSpecializationEnabled(bool enabled)
	{
#if defined(VITASX2_QEMU_VALIDATION)
		s_qemuIopClockModeSpecializationEnabled = enabled;
#else
		(void)enabled;
#endif
	}

	void BlockExecutor::SetSavedRegisterNarrowingEnabled(bool enabled)
	{
#if defined(VITASX2_QEMU_VALIDATION)
		s_qemuIopSavedRegisterNarrowingEnabled = enabled;
#else
		(void)enabled;
#endif
	}

	void BlockExecutor::SetBlockCycleBatchingEnabled(bool enabled)
	{
#if defined(VITASX2_QEMU_VALIDATION)
		s_qemuIopBlockCycleBatchingEnabled = enabled;
#else
		(void)enabled;
#endif
	}

	u32 BlockExecutor::LookupPageIndex(u32 start_pc)
	{
		return start_pc >> 16;
	}

	u32 BlockExecutor::LookupEntryIndex(u32 start_pc)
	{
		return (start_pc & 0xffffu) >> 2;
	}

	u32 BlockExecutor::HotDispatchCacheIndex(u32 start_pc)
	{
		static_assert((HOT_DISPATCH_CACHE_SET_COUNT & (HOT_DISPATCH_CACHE_SET_COUNT - 1)) == 0);
		return (start_pc >> 2) & (HOT_DISPATCH_CACHE_SET_COUNT - 1);
	}

	const u32* BlockExecutor::ResolveRawOpcodeSpan(
		u32 start_pc, u32 instruction_count, u32* ram_source_start)
	{
		// PCSX2 owner: x86/BaseblockEx.h::recLUT_SetPage() and
		// x86/iR3000A.cpp::recResetIOP()/psxhwLUT map the
		// R3000A RAM and ROM aliases to stable backing pages. Cache that resolved
		// source span just as VitaEeExecutor.cpp::ValidateCachedBlock() does for
		// page-bounded EE code, while retaining iopMemRead32() for handler-backed
		// or 64 KiB-crossing windows whose reads are not one raw contiguous span.
		if (ram_source_start)
			*ram_source_start = INVALID_RAM_SOURCE;
		if (instruction_count == 0 || instruction_count > ((UINT32_MAX - start_pc) / 4))
			return nullptr;

		const u32 physical_pc = start_pc & 0x1fffffffu;
		const u32 byte_count = instruction_count * 4;
		if ((physical_pc & 0xffffu) > (0x10000u - byte_count))
			return nullptr;

		const u32 page = physical_pc >> 16;
		const bool direct_ram = page < 0x80u;
		const bool direct_rom =
			(page >= 0x1fc0u && page < 0x2000u) ||
			(page >= 0x1e00u && page < 0x1e48u);
		if (!direct_ram && !direct_rom)
			return nullptr;
		if (direct_ram && ram_source_start)
			*ram_source_start = physical_pc & (Ps2MemSize::ExposedIopRam - 1);

		const uptr page_base = psxMemRLUT[page];
		return page_base ? reinterpret_cast<const u32*>(page_base + (physical_pc & 0xffffu)) : nullptr;
	}

	bool BlockExecutor::EnsureLookupDirectory()
	{
		if (m_lookup_pages)
			return true;

		m_lookup_pages = new (std::nothrow) LookupPage*[LOOKUP_DIRECTORY_ENTRY_COUNT] {};
		return (m_lookup_pages != nullptr);
	}

	BlockExecutor::LookupPage* BlockExecutor::GetLookupPage(u32 start_pc, bool allocate)
	{
		if (!m_lookup_pages && (!allocate || !EnsureLookupDirectory()))
			return nullptr;

		const u32 page = LookupPageIndex(start_pc);
		if (!m_lookup_pages[page] && allocate)
			m_lookup_pages[page] = new (std::nothrow) LookupPage();

		return m_lookup_pages[page];
	}

	void BlockExecutor::RegisterBlockLookup(CachedBlock& block)
	{
		if (!block.valid || (block.start_pc & 0x3u) != 0)
			return;

		RegisterHotDispatchCache(block);

		// PCSX2 owner: x86/BaseblockEx.h::PC_GETBLOCK_()/recLUT_SetPage().
		// Vita keeps the same 64 KiB guest-page lookup granularity, allocated
		// lazily for the R3000A address space.
		if (LookupPage* page = GetLookupPage(block.start_pc, true))
			page->blocks[LookupEntryIndex(block.start_pc)] = &block;
	}

	void BlockExecutor::UnregisterBlockLookup(CachedBlock& block)
	{
		if ((block.start_pc & 0x3u) != 0)
			return;

		UnregisterHotDispatchCache(block);

		if (LookupPage* page = GetLookupPage(block.start_pc, false))
		{
			CachedBlock*& entry = page->blocks[LookupEntryIndex(block.start_pc)];
			if (entry == &block)
				entry = nullptr;
		}
	}

	void BlockExecutor::RegisterHotDispatchCache(CachedBlock& block)
	{
		auto& set = m_hot_dispatch_cache[HotDispatchCacheIndex(block.start_pc)];
		for (HotDispatchCacheEntry& entry : set)
		{
			if (entry.block == &block || entry.start_pc == block.start_pc)
			{
				entry.block = &block;
				entry.start_pc = block.start_pc;
				return;
			}
		}
		// Keep the two most recently promoted PCs when a third PC aliases this
		// set. Hits do not reorder the ways, so a stable two-PC call chain keeps
		// both translations without per-dispatch cache writes.
		set[1] = set[0];
		set[0].block = &block;
		set[0].start_pc = block.start_pc;
	}

	void BlockExecutor::UnregisterHotDispatchCache(CachedBlock& block)
	{
		auto& set = m_hot_dispatch_cache[HotDispatchCacheIndex(block.start_pc)];
		for (HotDispatchCacheEntry& entry : set)
		{
			if (entry.block == &block)
				entry = {};
		}
		if (!set[0].block && set[1].block)
		{
			set[0] = set[1];
			set[1] = {};
		}
	}

	BlockExecutor::CachedBlock* BlockExecutor::FindHotDispatchCacheBlock(u32 start_pc)
	{
		auto& set = m_hot_dispatch_cache[HotDispatchCacheIndex(start_pc)];
		for (HotDispatchCacheEntry& entry : set)
		{
			if (entry.start_pc != start_pc || !entry.block)
				continue;

			if (!entry.block->valid || entry.block->start_pc != start_pc)
			{
				entry = {};
				return nullptr;
			}

			return entry.block;
		}
		return nullptr;
	}

	void BlockExecutor::ClearHotDispatchCache()
	{
		m_hot_dispatch_cache = {};
	}

	void BlockExecutor::ReleaseLookupPages()
	{
		if (!m_lookup_pages)
			return;

		for (u32 i = 0; i < LOOKUP_DIRECTORY_ENTRY_COUNT; i++)
			delete m_lookup_pages[i];

		delete[] m_lookup_pages;
		m_lookup_pages = nullptr;
	}

	void BlockExecutor::RegisterRamSource(CachedBlock& block)
	{
		if (!block.valid || block.ram_source_start == INVALID_RAM_SOURCE)
			return;

		block.source_serial = m_next_source_serial++;
		if (m_next_source_serial == 0)
			m_next_source_serial = 1;

		std::array<u32, 6> registered_pages{};
		u32 registered_page_count = 0;
		const auto register_range = [&](u32 source_start, u32 source_size) {
			if (source_start == INVALID_RAM_SOURCE || source_size == 0)
				return;
			const u32 source_end = source_start + source_size;
			for (u32 page_index = source_start >> RAM_SOURCE_PAGE_SHIFT;
				page_index <= ((source_end - 1) >> RAM_SOURCE_PAGE_SHIFT); page_index++)
			{
				bool already_registered = false;
				for (u32 i = 0; i < registered_page_count; i++)
					already_registered |= registered_pages[i] == page_index;
				if (already_registered)
					continue;

				m_ram_source_pages[page_index].push_back({&block, block.source_serial});
				m_ram_source_page_live_counts[page_index]++;
				registered_pages[registered_page_count++] = page_index;
			}
		};

		register_range(block.ram_source_start, block.instruction_count * sizeof(u32));
		if (block.poll_call_wait_loop)
		{
			register_range(block.poll_branch_source_start, 2 * sizeof(u32));
			register_range(block.poll_leaf_source_start, 5 * sizeof(u32));
		}
	}

	void BlockExecutor::UnregisterRamSource(const CachedBlock& block)
	{
		if (!block.valid || block.ram_source_start == INVALID_RAM_SOURCE)
			return;

		std::array<u32, 6> unregistered_pages{};
		u32 unregistered_page_count = 0;
		const auto unregister_range = [&](u32 source_start, u32 source_size) {
			if (source_start == INVALID_RAM_SOURCE || source_size == 0)
				return;
			const u32 source_end = source_start + source_size;
			for (u32 page_index = source_start >> RAM_SOURCE_PAGE_SHIFT;
				page_index <= ((source_end - 1) >> RAM_SOURCE_PAGE_SHIFT); page_index++)
			{
				bool already_unregistered = false;
				for (u32 i = 0; i < unregistered_page_count; i++)
					already_unregistered |= unregistered_pages[i] == page_index;
				if (already_unregistered)
					continue;

				if (m_ram_source_page_live_counts[page_index] != 0)
					m_ram_source_page_live_counts[page_index]--;
				unregistered_pages[unregistered_page_count++] = page_index;
			}
		};

		unregister_range(block.ram_source_start, block.instruction_count * sizeof(u32));
		if (block.poll_call_wait_loop)
		{
			unregister_range(block.poll_branch_source_start, 2 * sizeof(u32));
			unregister_range(block.poll_leaf_source_start, 5 * sizeof(u32));
		}
	}

	bool BlockExecutor::AnalyzePollCallWaitLoop(
		CachedBlock& block, u32 start_pc, u32 instruction_count)
	{
		// PCSX2 owners: x86/iR3000A.cpp::iPsxBranchTest() supplies the IOP
		// deadline/event fast-forward contract. The pure-load proof is the
		// R3000A adaptation of the load-aware loop analysis in
		// x86/ix86-32/iR5900.cpp::recRecompile()/recSkipTimeoutLoop().
		if (instruction_count != 2 || (block.opcodes[0] >> 26) != 0x03 || block.opcodes[1] != 0)
			return false;
		if ((start_pc & 0x1fffffffu) >= Ps2MemSize::TotalIopRam)
			return false;

		const u32 branch_pc = start_pc + 2 * sizeof(u32);
		u32 branch_source_start = INVALID_RAM_SOURCE;
		const u32* const branch_opcodes = ResolveRawOpcodeSpan(branch_pc, 2, &branch_source_start);
		if (!branch_opcodes || branch_source_start == INVALID_RAM_SOURCE)
			return false;

		const u32 branch_op = branch_opcodes[0];
		if ((branch_op >> 26) != 0x04 || BranchTarget(branch_pc, branch_op) != start_pc)
			return false;

		unsigned result_register = 0;
		if (RS(branch_op) == 0 && RT(branch_op) != 0)
			result_register = RT(branch_op);
		else if (RT(branch_op) == 0 && RS(branch_op) != 0)
			result_register = RS(branch_op);
		else
			return false;

		const u32 branch_delay = branch_opcodes[1];
		const u32 clear_result_delay = (result_register << 11) | 0x21u; // ADDU result,$zero,$zero
		if (branch_delay != 0 && branch_delay != clear_result_delay)
			return false;

		const u32 leaf_pc = JumpTarget(start_pc, block.opcodes[0]);
		u32 leaf_source_start = INVALID_RAM_SOURCE;
		const u32* const leaf_opcodes = ResolveRawOpcodeSpan(leaf_pc, 5, &leaf_source_start);
		if (!leaf_opcodes || leaf_source_start == INVALID_RAM_SOURCE)
			return false;

		const u32 lui = leaf_opcodes[0];
		const u32 addiu = leaf_opcodes[1];
		const u32 load = leaf_opcodes[2];
		if ((lui >> 26) != 0x0f || RS(lui) != 0 ||
			(addiu >> 26) != 0x09 || RS(addiu) != RT(lui) || RT(addiu) != RT(lui) ||
			(load >> 26) != 0x23 || RS(load) != RT(lui) || RT(load) != result_register ||
			leaf_opcodes[3] != 0x03e00008u || leaf_opcodes[4] != 0)
		{
			return false;
		}

		u32 poll_address = IMM_U(lui) << 16;
		poll_address += static_cast<u32>(static_cast<s32>(IMM_S(addiu)));
		poll_address += static_cast<u32>(static_cast<s32>(IMM_S(load)));
		const u32 physical_address = poll_address & 0x1fffffffu;
		if ((physical_address & 3u) != 0 || physical_address >= Ps2MemSize::TotalIopRam)
			return false;

		block.poll_call_wait_loop = true;
		block.poll_result_register = static_cast<u8>(result_register);
		block.poll_word_address = physical_address & (Ps2MemSize::ExposedIopRam - 1);
		block.poll_branch_opcodes = branch_opcodes;
		block.poll_leaf_opcodes = leaf_opcodes;
		block.poll_branch_source_start = branch_source_start;
		block.poll_leaf_source_start = leaf_source_start;
		for (u32 i = 0; i < block.poll_branch_expected.size(); i++)
			block.poll_branch_expected[i] = branch_opcodes[i];
		for (u32 i = 0; i < block.poll_leaf_expected.size(); i++)
			block.poll_leaf_expected[i] = leaf_opcodes[i];
		return true;
	}

	u32 BlockExecutor::InvalidateRamSourceRange(u32 start, u32 size)
	{
		if (size == 0 || start >= Ps2MemSize::ExposedIopRam ||
			size > Ps2MemSize::ExposedIopRam - start)
		{
			return 0;
		}

		const u32 end = start + size;
		u32 invalidated = 0;
		const auto range_touches_page = [](u32 source_start, u32 source_size, u32 page_index) {
			if (source_start == INVALID_RAM_SOURCE || source_size == 0)
				return false;
			const u32 source_end = source_start + source_size;
			return (source_start >> RAM_SOURCE_PAGE_SHIFT) <= page_index &&
				((source_end - 1) >> RAM_SOURCE_PAGE_SHIFT) >= page_index;
		};
		const auto range_overlaps = [start, end](u32 source_start, u32 source_size) {
			return source_start != INVALID_RAM_SOURCE && source_size != 0 &&
				start < source_start + source_size && source_start < end;
		};
		for (u32 page_index = start >> RAM_SOURCE_PAGE_SHIFT;
			page_index <= ((end - 1) >> RAM_SOURCE_PAGE_SHIFT); page_index++)
		{
			std::vector<RamSourceRecord>& records = m_ram_source_pages[page_index];
			u32 write_index = 0;
			for (u32 read_index = 0; read_index < records.size(); read_index++)
			{
#if defined(VITASX2_QEMU_VALIDATION)
				m_ram_invalidation_record_visits++;
#endif
				const RamSourceRecord record = records[read_index];
				CachedBlock* block = record.block;
				if (!block || !block->valid || block->source_serial != record.serial ||
					block->ram_source_start == INVALID_RAM_SOURCE)
				{
					continue;
				}

				const bool source_touches_page =
					range_touches_page(block->ram_source_start,
						block->instruction_count * sizeof(u32), page_index) ||
					(block->poll_call_wait_loop &&
						(range_touches_page(block->poll_branch_source_start, 2 * sizeof(u32), page_index) ||
						 range_touches_page(block->poll_leaf_source_start, 5 * sizeof(u32), page_index)));
				if (!source_touches_page)
				{
					continue;
				}

				const bool overlaps =
					range_overlaps(block->ram_source_start, block->instruction_count * sizeof(u32)) ||
					(block->poll_call_wait_loop &&
						(range_overlaps(block->poll_branch_source_start, 2 * sizeof(u32)) ||
						 range_overlaps(block->poll_leaf_source_start, 5 * sizeof(u32))));
				if (overlaps)
				{
					InvalidateCachedBlock(*block);
					invalidated++;
					continue;
				}

				if (write_index != read_index)
					records[write_index] = record;
				write_index++;
			}
			records.resize(write_index);
		}
		return invalidated;
	}

	void BlockExecutor::ClearRamSourcePages()
	{
		for (std::vector<RamSourceRecord>& page : m_ram_source_pages)
			page.clear();
		m_ram_source_page_live_counts.fill(0);
		m_next_source_serial = 1;
	}

	s32 BlockExecutor::LastBlockRecordIndex(u32 pc) const
	{
		if (m_block_records.empty())
			return -1;

		s32 min = 0;
		s32 max = static_cast<s32>(m_block_records.size() - 1);
		while (min != max)
		{
			const s32 mid = (min + max + 1) >> 1;
			if (m_block_records[mid].start_pc > pc)
				max = mid - 1;
			else
				min = mid;
		}

		return min;
	}

	bool BlockExecutor::RegisterBlockRecord(CachedBlock& block)
	{
		if (!block.valid)
			return false;

		UnregisterBlockRecord(block);
		if (m_block_records.size() >= MAX_CACHE_CAPACITY)
			return false;

		// PCSX2 owner: x86/BaseblockEx.h::BaseBlockArray::insert().
		// Sorted records let invalidation find overlapped R3000A blocks without
		// depending on cache-vector storage order.
		u32 insert_index = 0;
		u32 insert_limit = static_cast<u32>(m_block_records.size());
		while (insert_index < insert_limit)
		{
			const u32 mid = (insert_index + insert_limit) >> 1;
			if (m_block_records[mid].start_pc <= block.start_pc)
				insert_index = mid + 1;
			else
				insert_limit = mid;
		}

		m_block_records.insert(m_block_records.begin() + insert_index, {
			&block,
			block.code.EntryPoint(),
			block.start_pc,
			block.instruction_count,
			block.code.Size(),
		});
		return true;
	}

	void BlockExecutor::UnregisterBlockRecord(CachedBlock& block)
	{
		// PCSX2 owner: x86/BaseblockEx.cpp::BaseBlocks::LastIndex() plus
		// BaseBlocks::Remove(). Records are sorted by start PC, so only the
		// same-PC run can contain this block.
		s32 index = LastBlockRecordIndex(block.start_pc);
		while (index >= 0 && m_block_records[index].start_pc == block.start_pc)
			index--;
		index++;

		for (; index >= 0 && static_cast<u32>(index) < m_block_records.size() &&
			   m_block_records[index].start_pc == block.start_pc;
			 index++)
		{
			if (m_block_records[index].block == &block)
			{
				m_block_records.erase(m_block_records.begin() + index);
				return;
			}
		}
	}

	void BlockExecutor::ClearBlockRecords()
	{
		m_block_records.clear();
	}

	BlockExecutor::CachedBlock* BlockExecutor::FindRecordedBlockByStartPc(
		u32 start_pc, u32 instruction_count, bool match_instruction_count)
	{
		s32 index = LastBlockRecordIndex(start_pc);
		while (index >= 0 && m_block_records[index].start_pc == start_pc)
		{
			CachedBlock* block = m_block_records[index].block;
			if (block && block->valid &&
				(!match_instruction_count || block->instruction_count == instruction_count))
			{
				if (ValidateCachedBlock(*block))
					return block;

				break;
			}

			index--;
		}

		return nullptr;
	}

	void BlockExecutor::RememberFreeCacheEntry(CachedBlock& block)
	{
		// PCSX2 owner: x86/BaseblockEx.cpp::BaseBlocks::Remove()/New().
		// Removed BaseBlock records become reusable metadata; keep Vita's
		// CachedBlock object reuse O(1) instead of scanning m_cache.
		if (block.queued_free)
			return;

		block.queued_free = true;
		m_free_cache_entries.push_back(&block);
	}

	BlockExecutor::CachedBlock* BlockExecutor::TakeFreeCacheEntry()
	{
		while (!m_free_cache_entries.empty())
		{
			CachedBlock* block = m_free_cache_entries.back();
			m_free_cache_entries.pop_back();
			if (block)
				block->queued_free = false;
			if (block && !block->valid)
				return block;
		}

		return nullptr;
	}

	DirectLinkSlot* BlockExecutor::GetRecordedDirectLink(IncomingLinkRecord& record)
	{
		if (!record.source || !record.source->valid || record.slot_index >= DIRECT_LINK_SLOT_COUNT)
			return nullptr;

		DirectLinkSlot& link = record.source->direct_links.slots[record.slot_index];
		if (!link.valid || link.target_pc != record.target_pc)
			return nullptr;

		return &link;
	}

	s32 BlockExecutor::LastIncomingLinkIndex(u32 target_pc) const
	{
		if (m_incoming_links.empty())
			return -1;

		s32 min = 0;
		s32 max = static_cast<s32>(m_incoming_links.size() - 1);
		while (min != max)
		{
			const s32 mid = (min + max + 1) >> 1;
			if (m_incoming_links[mid].target_pc > target_pc)
				max = mid - 1;
			else
				min = mid;
		}

		return min;
	}

	void BlockExecutor::ClearIncomingLinks()
	{
		m_incoming_links.clear();
	}

	void BlockExecutor::RegisterIncomingLink(CachedBlock& block, u8 slot_index, const DirectLinkSlot& link)
	{
		if (!link.valid || m_incoming_links.size() >= MAX_INCOMING_LINKS)
			return;

		u32 insert_index = 0;
		u32 insert_limit = static_cast<u32>(m_incoming_links.size());
		while (insert_index < insert_limit)
		{
			const u32 mid = (insert_index + insert_limit) >> 1;
			if (m_incoming_links[mid].target_pc <= link.target_pc)
				insert_index = mid + 1;
			else
				insert_limit = mid;
		}
		m_incoming_links.insert(m_incoming_links.begin() + insert_index, {&block, link.target_pc, slot_index});
	}

	void BlockExecutor::RegisterIncomingLinks(CachedBlock& block)
	{
		UnregisterIncomingLinks(block);

		// PCSX2 owner: x86/BaseblockEx.cpp::BaseBlocks::Link(). Keep target-PC
		// -> source patch-site records so invalidating a block only repairs its
		// incoming edges. Keep Vita's vector sorted by target PC so direct-link
		// patching only visits matching records.
		for (u8 i = 0; i < DIRECT_LINK_SLOT_COUNT; i++)
		{
			const DirectLinkSlot& link = block.direct_links.slots[i];
			RegisterIncomingLink(block, i, link);
		}
	}

	void BlockExecutor::UnregisterIncomingLinks(CachedBlock& block)
	{
		u32 write_index = 0;
		for (u32 read_index = 0; read_index < m_incoming_links.size(); read_index++)
		{
			if (m_incoming_links[read_index].source == &block)
				continue;

			if (write_index != read_index)
				m_incoming_links[write_index] = m_incoming_links[read_index];
			write_index++;
		}

		m_incoming_links.resize(write_index);
	}

	u32 BlockExecutor::Reset()
	{
		u32 invalidated = 0;
		ClearHotDispatchCache();
		m_free_cache_entries.clear();
		for (const std::unique_ptr<CachedBlock>& entry : m_cache)
		{
			if (entry->valid)
				invalidated++;

			entry->valid = false;
			entry->queued_free = false;
			entry->direct_links = {};
			entry->code.Release();
			RememberFreeCacheEntry(*entry);
		}

		ClearBlockRecords();
		ClearIncomingLinks();
		ClearRamSourcePages();
		ReleaseLookupPages();
		const u32 previous_resets = m_code_cache_resets;
		ReleaseCodeCache();
		m_code_cache_resets = previous_resets;
		return invalidated;
	}

	void BlockExecutor::InvalidateCachedBlock(CachedBlock& block)
	{
		if (!block.valid)
			return;

		UnregisterRamSource(block);
		UnlinkIncomingLinks(block.start_pc);
		UnregisterIncomingLinks(block);
		UnregisterBlockLookup(block);
		UnregisterBlockRecord(block);
		block.valid = false;
		block.raw_opcodes = nullptr;
		block.ram_source_start = INVALID_RAM_SOURCE;
		block.poll_branch_opcodes = nullptr;
		block.poll_leaf_opcodes = nullptr;
		block.poll_branch_source_start = INVALID_RAM_SOURCE;
		block.poll_leaf_source_start = INVALID_RAM_SOURCE;
		block.poll_call_wait_loop = false;
		block.direct_budget_exit = false;
		block.constant_cycle_budget = false;
		block.clock_mode_check_instructions_removed = 0;
		block.saved_register_stack_words_removed = 0;
		block.saved_register_frame_instructions_added = 0;
		block.saved_register_frame_instructions_removed = 0;
		block.batched_cycle_instructions_removed = 0;
		block.batched_cycle_stack_words_removed = 0;
		block.expanded_cycle_batching = false;
		block.direct_links = {};
		block.code.Release();
		RememberFreeCacheEntry(block);
	}

	u32 BlockExecutor::InvalidateRange(u32 start_pc, u32 instruction_count)
	{
		if (instruction_count == 0 || instruction_count > ((UINT32_MAX - start_pc) / 4))
			return 0;

		const u32 physical_start = start_pc & 0x1fffffffu;
		const u32 byte_count = instruction_count * sizeof(u32);
		if (physical_start < Ps2MemSize::TotalIopRam &&
			byte_count <= Ps2MemSize::TotalIopRam - physical_start)
		{
#if defined(VITASX2_QEMU_VALIDATION)
			m_ram_invalidation_calls++;
#endif
			u32 backing_start = physical_start & (Ps2MemSize::ExposedIopRam - 1);
			u32 remaining = byte_count;
			u32 invalidated = 0;
			while (remaining != 0)
			{
				const u32 chunk = std::min(remaining, Ps2MemSize::ExposedIopRam - backing_start);
				invalidated += InvalidateRamSourceRange(backing_start, chunk);
				remaining -= chunk;
				backing_start = 0;
			}
			return invalidated;
		}

		const u32 end_pc = start_pc + instruction_count * 4;
		u32 invalidated = 0;
		constexpr u32 max_block_bytes = MAX_STRAIGHT_LINE_BLOCK_INSTRUCTIONS * 4;
		const u32 first_candidate_pc = (start_pc > max_block_bytes) ? (start_pc - max_block_bytes) : 0;
		// PCSX2 keeps BaseBlocks sorted by guest start PC. Since Vita blocks are
		// bounded, entries before this lower bound cannot overlap the cleared
		// word range.
		u32 i = 0;
		u32 limit = static_cast<u32>(m_block_records.size());
		while (i < limit)
		{
			const u32 mid = (i + limit) >> 1;
			if (m_block_records[mid].start_pc < first_candidate_pc)
				i = mid + 1;
			else
				limit = mid;
		}

		for (; i < m_block_records.size();)
		{
			CachedBlock* block = m_block_records[i].block;
			if (!block || !block->valid)
			{
				if (block)
					UnregisterBlockRecord(*block);
				else
					i++;
				continue;
			}

			if (block->start_pc >= end_pc)
				break;

			const u32 block_end = block->start_pc + block->instruction_count * 4;
			if (start_pc < block_end)
			{
				InvalidateCachedBlock(*block);
				invalidated++;
				continue;
			}

			i++;
		}

		return invalidated;
	}

	void BlockExecutor::SetDirectLinkingEnabled(bool enabled)
	{
		if (m_direct_linking_enabled == enabled)
			return;

		m_direct_linking_enabled = enabled;
		if (enabled)
			RelinkDirectLinks();
		else
			UnlinkIncomingLinks(UINT32_MAX);
	}

	bool BlockExecutor::TryFastForwardWaitLoopAtPc(u32 start_pc)
	{
		if (!EmuConfig.Speedhacks.WaitLoop || VitaIsIopPreInstructionTraceEnabled())
			return false;
		return TryFastForwardTrustedWaitLoopAtPc(start_pc);
	}

	bool BlockExecutor::TryFastForwardTrustedWaitLoopAtPc(u32 start_pc)
	{
		// PCSX2 owner: VMManager::CheckForCPUConfigChanges() resets psxCpu when
		// Speedhacks changes. VitaSetIopPreInstructionTraceCallback() likewise
		// resets this executor when trace enablement changes. RunValidatedBlock()
		// reaches this path only through a descriptor compiled under that reset
		// contract; the public standalone helper above retains its live guard.
		u32 block_cycles = 0;
		bool link = false;
		bool taken = false;
		for (u32 i = 0; i + 1 < MAX_STRAIGHT_LINE_BLOCK_INSTRUCTIONS; i++)
		{
			const u32 pc = start_pc + i * 4;
			const u32 op = iopMemRead32(pc);
			if (op == 0)
				continue;

			const u32 primary = op >> 26;
			u32 target_pc = UINT32_MAX;
			switch (primary)
			{
				case 0x01:
				{
					const s32 value = static_cast<s32>(psxRegs.GPR.r[RS(op)]);
					switch (RT(op))
					{
						case 0x00: taken = value < 0; break; // BLTZ
						case 0x01: taken = value >= 0; break; // BGEZ
						case 0x10: taken = value < 0; link = true; break; // BLTZAL
						case 0x11: taken = value >= 0; link = true; break; // BGEZAL
						default: return false;
					}
					target_pc = BranchTarget(pc, op);
					break;
				}
				case 0x02: taken = true; target_pc = JumpTarget(pc, op); break; // J
				case 0x03: taken = true; link = true; target_pc = JumpTarget(pc, op); break; // JAL
				case 0x04: // BEQ
					taken = psxRegs.GPR.r[RS(op)] == psxRegs.GPR.r[RT(op)];
					target_pc = BranchTarget(pc, op);
					break;
				case 0x05: // BNE
					taken = psxRegs.GPR.r[RS(op)] != psxRegs.GPR.r[RT(op)];
					target_pc = BranchTarget(pc, op);
					break;
				case 0x06: // BLEZ
					taken = static_cast<s32>(psxRegs.GPR.r[RS(op)]) <= 0;
					target_pc = BranchTarget(pc, op);
					break;
				case 0x07: // BGTZ
					taken = static_cast<s32>(psxRegs.GPR.r[RS(op)]) > 0;
					target_pc = BranchTarget(pc, op);
					break;
				default:
					return false;
			}

			if (!taken || target_pc != start_pc || iopMemRead32(pc + 4) != 0)
				return false;

			block_cycles = i + 2;
			if (link)
				psxRegs.GPR.r[31] = pc + 8;
			break;
		}

		if (block_cycles == 0)
			return false;

		// RunValidatedBlock calls this only after the ordinary cache lookup and source
		// validation. This is the Cortex-A9 adaptation of PCSX2's generated
		// s_nBlockFF tail: it removes the generated-block call, self-link, and
		// return for an otherwise empty wait loop while retaining the exact owner
		// helper and the existing cache/SMC ownership.
		VitaIopA32FastForwardWaitLoop(start_pc, block_cycles);
		VitaRecordA32IopWaitLoopDispatchElision();
		return true;
	}

	bool BlockExecutor::ScanStraightLineBlock(u32 start_pc, u32 max_instruction_count, BlockScanResult* result)
	{
		if (!result || max_instruction_count == 0)
			return false;

		*result = {};
		result->start_pc = start_pc;
		result->stop_pc = start_pc;

		const auto add_instruction = [&](u32 pc) {
			result->instruction_count++;
			result->stop_pc = pc + 4;
		};

		for (u32 i = 0; i < max_instruction_count; i++)
		{
			if (i > ((UINT32_MAX - start_pc) / 4))
				return true;

			const u32 pc = start_pc + i * 4;
			if (i != 0 && (pc & 0xffcu) == 0)
				return true;

			const u32 op = iopMemRead32(pc);
			if (!BlockCompiler::CanCompileOpcode(op))
				return true;

			if (IsIopBranchOrJumpOpcode(op))
			{
				if (i + 1 >= max_instruction_count ||
					i >= ((UINT32_MAX - start_pc) / 4))
				{
					return result->instruction_count != 0;
				}

				const u32 delay_pc = pc + 4;
				const u32 delay_op = iopMemRead32(delay_pc);
				if (!BlockCompiler::CanCompileOpcode(delay_op))
					return result->instruction_count != 0;

				// PCSX2 owner: R3000AInterpreter.cpp::psxBNE()/psxJAL()
				// dispatch through doBranch(), whose delay slot belongs to the
				// branch. Keep the pair together even when the delay slot is the
				// first word of the next guest page.
				add_instruction(pc);
				add_instruction(delay_pc);
				bool wait_loop_candidate = false;
				if (EmuConfig.Speedhacks.WaitLoop && !VitaIsIopPreInstructionTraceEnabled() &&
					IsIopStaticConditionalBranchOpcode(op) && BranchTarget(pc, op) == start_pc &&
					delay_op == 0)
				{
					wait_loop_candidate = true;
					for (u32 prefix_pc = start_pc; prefix_pc < pc; prefix_pc += 4)
					{
						if (iopMemRead32(prefix_pc) != 0)
						{
							wait_loop_candidate = false;
							break;
						}
					}
				}
				if (wait_loop_candidate)
					return true;

				if (IsIopStaticConditionalBranchOpcode(op) &&
					BranchTarget(pc, op) == start_pc &&
					!IsIopBranchOrJumpOpcode(delay_op) &&
					!IsIopExceptionOpcode(delay_op))
				{
					// PCSX2 owner: x86/iR3000A.cpp ends a hot loop block at its
					// branch/delay pair. Give a back-edge to this block's own entry
					// the same native two-successor tail so local register residency
					// covers the taken loop; the one-time exit keeps its fallthrough
					// link. Other conditionals retain the compact interpreter-style
					// fallthrough block below.
					return true;
				}

				if (IsIopStaticConditionalBranchOpcode(op) &&
					!IsIopBranchOrJumpOpcode(delay_op) &&
					!IsIopExceptionOpcode(delay_op) &&
					(delay_pc & 0xffcu) != 0)
				{
					i++;
					continue;
				}
				return true;
			}

			add_instruction(pc);
			if (IsIopExceptionOpcode(op))
				return true;
		}

		return true;
	}

	bool BlockExecutor::ValidateCachedBlock(CachedBlock& block)
	{
		if (!block.valid)
			return false;

		// Configuration-dependent wait descriptors are invalidated by the same
		// PCSX2 CPU-cache reset ownership described by the trusted helper above.
		// Do not poll immutable-within-cache-lifetime configuration on every hit.

#if defined(VITASX2_QEMU_VALIDATION)
		m_validation_calls++;
#endif
		bool matches = true;
		if (block.raw_opcodes) [[likely]]
		{
#if defined(VITASX2_QEMU_VALIDATION)
			m_trusted_source_hits++;
			if (!s_qemuIopTrustedSourceAuditEnabled)
				return true;
			m_raw_validation_calls++;
			for (u32 i = 0; matches && i < block.instruction_count; i++)
			{
				m_validation_words++;
				m_raw_validation_words++;
				m_trusted_source_audit_words++;
				matches = (block.opcodes[i] == block.raw_opcodes[i]);
			}
			if (block.poll_call_wait_loop)
			{
				for (u32 i = 0; matches && i < block.poll_branch_expected.size(); i++)
				{
					m_validation_words++;
					m_raw_validation_words++;
					m_trusted_source_audit_words++;
					matches = block.poll_branch_expected[i] == block.poll_branch_opcodes[i];
				}
				for (u32 i = 0; matches && i < block.poll_leaf_expected.size(); i++)
				{
					m_validation_words++;
					m_raw_validation_words++;
					m_trusted_source_audit_words++;
					matches = block.poll_leaf_expected[i] == block.poll_leaf_opcodes[i];
				}
			}
			if (!matches)
				m_trusted_source_audit_failures++;
#else
			return true;
#endif
		}
		else
		{
			for (u32 i = 0; matches && i < block.instruction_count; i++)
			{
#if defined(VITASX2_QEMU_VALIDATION)
				m_validation_words++;
				m_translated_validation_words++;
#endif
				matches = (block.opcodes[i] == iopMemRead32(block.start_pc + i * 4));
			}
		}

		if (matches)
			return true;

		// PCSX2 owner: x86/iR3000A.cpp::psxRecClearMem() invalidates changed
		// translated ranges. Raw RAM sources normally return above under the
		// explicit Vita invalidation contract; this mismatch path remains for the
		// QEMU trust audit and handler-backed/cross-page fallback sources.
		InvalidateCachedBlock(block);
		return false;
	}

	bool BlockExecutor::TryFastForwardPollCallWaitLoop(CachedBlock& block)
	{
		// RunValidatedBlock reaches this only after ValidateCachedBlock proved the
		// cached WaitLoop/trace configuration still matches compilation.
		const u32 value =
			*reinterpret_cast<const u32*>(&iopMem->Main[block.poll_word_address]);
		if (value != 0)
			return false;

		// Execute the architecturally visible results of the proven JAL/leaf-load/
		// taken-BEQ iteration before applying PCSX2's IOP deadline skip. The
		// branch delay is either NOP or the same zero assignment.
		psxRegs.GPR.r[block.poll_result_register] = 0;
		psxRegs.GPR.r[31] = block.start_pc + 2 * sizeof(u32);
		constexpr u32 poll_loop_cycles = 2 + 5 + 2;
		VitaIopA32FastForwardWaitLoop(block.start_pc, poll_loop_cycles);
		VitaRecordA32IopWaitLoopDispatchElision();
		VitaRecordA32IopPollCallWaitLoopDispatchElision();
		return true;
	}

	BlockExecutor::CachedBlock* BlockExecutor::FindLookupBlockByStartPc(u32 start_pc)
	{
		if ((start_pc & 0x3u) != 0)
			return nullptr;

		LookupPage* page = GetLookupPage(start_pc, false);
		return page ? page->blocks[LookupEntryIndex(start_pc)] : nullptr;
	}

	bool BlockExecutor::FindCachedBlock(u32 start_pc, u32 instruction_count, CachedBlock** block, bool* lookup_hit)
	{
		if (!block || instruction_count == 0 ||
			instruction_count > MAX_STRAIGHT_LINE_BLOCK_INSTRUCTIONS ||
			instruction_count > ((UINT32_MAX - start_pc) / 4))
		{
			return false;
		}

		*block = nullptr;
		if (lookup_hit)
			*lookup_hit = false;

		if (CachedBlock* entry = FindLookupBlockByStartPc(start_pc))
		{
			if (entry->valid && entry->instruction_count == instruction_count && ValidateCachedBlock(*entry))
			{
				*block = entry;
				if (lookup_hit)
					*lookup_hit = true;
				return true;
			}
		}

		if (CachedBlock* entry = FindRecordedBlockByStartPc(start_pc, instruction_count, true))
		{
			*block = entry;
			return true;
		}

		return false;
	}

	BlockExecutor::CachedBlock* BlockExecutor::FindCachedBlockByStartPc(u32 start_pc)
	{
		if (CachedBlock* entry = FindLookupBlockByStartPc(start_pc))
		{
			if (entry->valid && ValidateCachedBlock(*entry))
				return entry;
		}

		return FindRecordedBlockByStartPc(start_pc, 0, false);
	}

	BlockExecutor::CachedBlock* BlockExecutor::AllocateCacheEntry()
	{
		const auto append_entry = [this]() -> CachedBlock* {
			if (m_cache.size() >= MAX_CACHE_CAPACITY)
				return nullptr;

			std::unique_ptr<CachedBlock> entry(new (std::nothrow) CachedBlock());
			if (!entry)
				return nullptr;

			CachedBlock* block = entry.get();
			m_cache.push_back(std::move(entry));
			return block;
		};

		if (CachedBlock* block = TakeFreeCacheEntry())
			return block;

		if (CachedBlock* block = append_entry())
			return block;

		ResetForCachePressure();
		return TakeFreeCacheEntry();
	}

	bool BlockExecutor::EnsureCodeCache()
	{
		if (m_code_cache)
			return true;

		// PCSX2 owner: x86/iR3000A.cpp::recReserve()/recResetIOP() use one IOP
		// recompiler arena and BASEBLOCK records inside it. Vita mirrors that
		// with one ARM code arena for cached R3000A blocks.
		m_code_cache = static_cast<u8*>(VitaVM::AllocJitMemory(IOP_CODE_CACHE_CAPACITY));
		m_code_cache_capacity = m_code_cache ? IOP_CODE_CACHE_CAPACITY : 0;
		m_code_cache_used = 0;
		return (m_code_cache != nullptr);
	}

	void BlockExecutor::ReleaseCodeCache()
	{
		if (!m_code_cache)
			return;

		VitaVM::FreeJitMemory(m_code_cache);
		m_code_cache = nullptr;
		m_code_cache_capacity = 0;
		m_code_cache_used = 0;
	}

	u8* BlockExecutor::AllocateCodeSlice(size_t capacity, size_t* slice_offset)
	{
		if (!EnsureCodeCache())
			return nullptr;

		const size_t aligned_offset = AlignUp(m_code_cache_used, CODE_CACHE_ALIGNMENT);
		if (capacity > m_code_cache_capacity || aligned_offset > (m_code_cache_capacity - capacity))
			return nullptr;

		if (slice_offset)
			*slice_offset = aligned_offset;

		m_code_cache_used = aligned_offset + capacity;
		return m_code_cache + aligned_offset;
	}

	void BlockExecutor::RewindCodeCache(size_t slice_offset)
	{
		if (slice_offset <= m_code_cache_used)
			m_code_cache_used = slice_offset;
	}

	void BlockExecutor::CommitCodeSlice(size_t slice_offset, size_t code_size)
	{
		if (slice_offset > m_code_cache_used || code_size > m_code_cache_used - slice_offset)
			return;

		const size_t committed_size = AlignUp(code_size, CODE_CACHE_ALIGNMENT);
		if (committed_size > m_code_cache_used - slice_offset)
			return;

		m_code_cache_used = slice_offset + committed_size;
	}

	u32 BlockExecutor::ResetForCachePressure()
	{
		const u32 previous_resets = m_code_cache_resets;
		const u32 invalidated = Reset();
		m_code_cache_resets = previous_resets + 1;
		return invalidated;
	}

	bool BlockExecutor::CompileIntoCacheEntry(CachedBlock& block, u32 start_pc, u32 instruction_count)
	{
		if (instruction_count == 0 ||
			instruction_count > MAX_STRAIGHT_LINE_BLOCK_INSTRUCTIONS ||
			instruction_count > ((UINT32_MAX - start_pc) / 4))
		{
			return false;
		}

		InvalidateCachedBlock(block);
		for (u32 i = 0; i < instruction_count; i++)
		{
			const u32 op = iopMemRead32(start_pc + i * 4);
			if (!BlockCompiler::CanCompileOpcode(op))
				return false;

			block.opcodes[i] = op;
		}
		block.poll_call_wait_loop = false;
		block.direct_budget_exit = false;
		block.constant_cycle_budget = false;
		block.poll_branch_opcodes = nullptr;
		block.poll_leaf_opcodes = nullptr;
		block.poll_branch_source_start = INVALID_RAM_SOURCE;
		block.poll_leaf_source_start = INVALID_RAM_SOURCE;
		block.wait_loop_shape =
			AnalyzePollCallWaitLoop(block, start_pc, instruction_count) ||
			IsIopWaitLoopShape(start_pc, instruction_count);
		block.wait_loop_enabled_at_compile =
			EmuConfig.Speedhacks.WaitLoop && !VitaIsIopPreInstructionTraceEnabled();

		size_t block_code_capacity = STRAIGHT_LINE_BLOCK_CODE_CAPACITY;
		size_t block_code_slice_offset = 0;
		u32 native_instruction_count = 0;
		u32 helper_instruction_count = 0;
		u32 pinned_gpr_memory_ops_saved = 0;
		u32 clock_mode_check_instructions_removed = 0;
		u32 saved_register_stack_words_removed = 0;
		u32 saved_register_frame_instructions_added = 0;
		u32 saved_register_frame_instructions_removed = 0;
		u32 batched_cycle_instructions_removed = 0;
		u32 batched_cycle_stack_words_removed = 0;
		bool expanded_cycle_batching = false;
		bool direct_budget_exit = false;
		bool constant_cycle_budget = false;
		DirectLinkSlots direct_links;
		for (;;)
		{
			size_t code_slice_offset = 0;
			u8* code_slice = AllocateCodeSlice(block_code_capacity, &code_slice_offset);
			if (!code_slice)
			{
				ResetForCachePressure();
				code_slice = AllocateCodeSlice(block_code_capacity, &code_slice_offset);
				if (!code_slice)
					return false;
			}

			if (!block.code.Attach(code_slice, block_code_capacity))
			{
				RewindCodeCache(code_slice_offset);
				return false;
			}

			BlockCompiler compiler(block.code, m_ram_source_page_live_counts.data());
			DirectLinkSlots attempt_direct_links;
			const bool compiled = compiler.CompileStraightLineBlock(start_pc, instruction_count,
				reinterpret_cast<const void*>(&VitaIopA32DirectExit), &attempt_direct_links);
			const bool out_of_block_space = !compiled && block.code.Size() >= block.code.Capacity();
			if (compiled && block.code.Flush())
			{
				block_code_slice_offset = code_slice_offset;
				CommitCodeSlice(code_slice_offset, block.code.Size());
				native_instruction_count = compiler.NativeInstructionCount();
				helper_instruction_count = compiler.HelperInstructionCount();
				pinned_gpr_memory_ops_saved = compiler.PinnedGprMemoryOpsSaved();
				clock_mode_check_instructions_removed = compiler.ClockModeCheckInstructionsRemoved();
				saved_register_stack_words_removed = compiler.SavedRegisterStackWordsRemoved();
				saved_register_frame_instructions_added = compiler.SavedRegisterFrameInstructionsAdded();
				saved_register_frame_instructions_removed = compiler.SavedRegisterFrameInstructionsRemoved();
				batched_cycle_instructions_removed = compiler.BatchedCycleInstructionsRemoved();
				batched_cycle_stack_words_removed = compiler.BatchedCycleStackWordsRemoved();
				expanded_cycle_batching = compiler.UsesExpandedCycleBatching();
				direct_budget_exit = compiler.UsesDirectBudgetExit();
				constant_cycle_budget = compiler.UsesConstantCycleBudget();
				direct_links = attempt_direct_links;
				break;
			}

			block.code.Release();
			RewindCodeCache(code_slice_offset);
			if (!out_of_block_space || block_code_capacity >= MAX_STRAIGHT_LINE_BLOCK_CODE_CAPACITY)
				return false;

			block_code_capacity *= 2;
		}

		block.start_pc = start_pc;
		block.instruction_count = instruction_count;
		block.raw_opcodes = ResolveRawOpcodeSpan(
			start_pc, instruction_count, &block.ram_source_start);
		block.native_instruction_count = native_instruction_count;
		block.helper_instruction_count = helper_instruction_count;
		block.pinned_gpr_memory_ops_saved = pinned_gpr_memory_ops_saved;
		block.clock_mode_check_instructions_removed = clock_mode_check_instructions_removed;
		block.saved_register_stack_words_removed = saved_register_stack_words_removed;
		block.saved_register_frame_instructions_added = saved_register_frame_instructions_added;
		block.saved_register_frame_instructions_removed = saved_register_frame_instructions_removed;
		block.batched_cycle_instructions_removed = batched_cycle_instructions_removed;
		block.batched_cycle_stack_words_removed = batched_cycle_stack_words_removed;
		block.expanded_cycle_batching = expanded_cycle_batching;
		block.direct_budget_exit = direct_budget_exit;
		block.constant_cycle_budget = constant_cycle_budget;
		block.direct_links = direct_links;
		block.valid = true;
		if (!RegisterBlockRecord(block))
		{
			block.valid = false;
			block.direct_links = {};
			block.code.Release();
			RewindCodeCache(block_code_slice_offset);
			return false;
		}
		RegisterBlockLookup(block);
		RegisterRamSource(block);
		RegisterIncomingLinks(block);

		if (m_direct_linking_enabled)
		{
			PatchIncomingLinks(block.start_pc, block.code.EntryPoint());
			for (DirectLinkSlot& link : block.direct_links.slots)
			{
				if (link.valid)
				{
					if (CachedBlock* target = FindCachedBlockByStartPc(link.target_pc))
						PatchDirectLink(block, link, target->code.EntryPoint());
				}
			}
		}

		return true;
	}

	bool BlockExecutor::PatchDirectLink(CachedBlock& block, DirectLinkSlot& link, const void* target)
	{
		if (!target || !block.valid || !link.valid ||
			link.target_offset == static_cast<size_t>(-1) ||
			link.fallback_offset == static_cast<size_t>(-1))
		{
			return false;
		}

		const bool target_is_direct_exit =
			target == reinterpret_cast<const void*>(&VitaIopA32DirectExit);
		const bool patched = target_is_direct_exit ?
			block.code.PatchBranch(link.target_offset, link.fallback_offset) :
			block.code.PatchBranchToAddress(link.target_offset, target);
		return patched && block.code.Flush();
	}

	void BlockExecutor::PatchIncomingLinks(u32 target_pc, const void* target)
	{
		if (!m_direct_linking_enabled || !target)
			return;

		s32 index = LastIncomingLinkIndex(target_pc);
		while (index >= 0 && m_incoming_links[index].target_pc == target_pc)
		{
			IncomingLinkRecord& record = m_incoming_links[index--];
			if (DirectLinkSlot* link = GetRecordedDirectLink(record))
				PatchDirectLink(*record.source, *link, target);
		}
	}

	void BlockExecutor::UnlinkIncomingLinks(u32 target_pc)
	{
		if (target_pc == UINT32_MAX)
		{
			for (u32 i = 0; i < m_incoming_links.size(); i++)
			{
				IncomingLinkRecord& record = m_incoming_links[i];
				if (DirectLinkSlot* link = GetRecordedDirectLink(record))
					PatchDirectLink(*record.source, *link, reinterpret_cast<const void*>(&VitaIopA32DirectExit));
			}
			return;
		}

		s32 index = LastIncomingLinkIndex(target_pc);
		while (index >= 0 && m_incoming_links[index].target_pc == target_pc)
		{
			IncomingLinkRecord& record = m_incoming_links[index--];
			if (DirectLinkSlot* link = GetRecordedDirectLink(record))
				PatchDirectLink(*record.source, *link, reinterpret_cast<const void*>(&VitaIopA32DirectExit));
		}
	}

	void BlockExecutor::RelinkDirectLinks()
	{
		for (u32 i = 0; i < m_incoming_links.size(); i++)
		{
			IncomingLinkRecord& record = m_incoming_links[i];
			DirectLinkSlot* link = GetRecordedDirectLink(record);
			if (!link)
				continue;

			const CachedBlock* target = FindCachedBlockByStartPc(record.target_pc);
			PatchDirectLink(*record.source, *link, target ? target->code.EntryPoint() :
															reinterpret_cast<const void*>(&VitaIopA32DirectExit));
		}
	}

	void BlockExecutor::PublishExecutionDetails(
		const CachedBlock& block, BlockExecutionResult* result) const
	{
		result->instruction_count = block.instruction_count;
		result->native_instruction_count = block.native_instruction_count;
		result->helper_instruction_count = block.helper_instruction_count;
		result->code_size = block.code.Size();
		result->block_records = static_cast<u32>(m_block_records.size());
		result->link_records = static_cast<u32>(m_incoming_links.size());
		result->cache_slots = static_cast<u32>(m_cache.size());
		result->code_cache_resets = m_code_cache_resets;
		result->code_cache_used = m_code_cache_used;
		result->code_cache_capacity = m_code_cache_capacity;
#if defined(VITASX2_QEMU_VALIDATION)
		result->pinned_gpr_memory_ops_saved = block.pinned_gpr_memory_ops_saved;
		SnapshotInstrumentation(result);
#endif
	}

#if defined(VITASX2_QEMU_VALIDATION)
	void BlockExecutor::SnapshotInstrumentation(BlockExecutionResult* result) const
	{
		if (!result)
			return;

		result->hot_dispatch_cache_hits = m_hot_dispatch_cache_hits;
		result->hot_dispatch_cache_misses = m_hot_dispatch_cache_misses;
		result->hot_dispatch_trusted_raw_hits = m_hot_dispatch_trusted_raw_hits;
		result->direct_budget_exit_provider_entries = m_direct_budget_exit_provider_entries;
		result->constant_cycle_budget_provider_entries = m_constant_cycle_budget_provider_entries;
		result->validation_calls = m_validation_calls;
		result->validation_words = m_validation_words;
		result->raw_validation_calls = m_raw_validation_calls;
		result->raw_validation_words = m_raw_validation_words;
		result->translated_validation_words = m_translated_validation_words;
		result->wait_loop_configuration_checks = m_wait_loop_configuration_checks;
		result->trusted_source_hits = m_trusted_source_hits;
		result->trusted_source_audit_words = m_trusted_source_audit_words;
		result->trusted_source_audit_failures = m_trusted_source_audit_failures;
		result->ram_invalidation_calls = m_ram_invalidation_calls;
		result->ram_invalidation_record_visits = m_ram_invalidation_record_visits;
		result->clock_mode_check_instructions_removed = m_clock_mode_check_instructions_removed;
		result->saved_register_stack_words_removed = m_saved_register_stack_words_removed;
		result->saved_register_frame_instructions_added = m_saved_register_frame_instructions_added;
		result->saved_register_frame_instructions_removed = m_saved_register_frame_instructions_removed;
		result->batched_cycle_instructions_removed = m_batched_cycle_instructions_removed;
		result->batched_cycle_stack_words_removed = m_batched_cycle_stack_words_removed;
		result->expanded_cycle_batching_provider_entries = m_expanded_cycle_batching_provider_entries;
	}
#endif

	bool BlockExecutor::RunValidatedBlock(
		CachedBlock& block, BlockExecutionResult* result, bool publish_details)
	{
		if (!result || !block.valid)
			return false;

		// PCSX2 owner: x86/BaseblockEx.h::PC_GETBLOCK_() trusts the BaseBlock
		// selected by the dispatcher; x86/iR3000A.cpp::psxRecClearMem() removes
		// stale translations before they can run. Vita's explicit source-range
		// dependencies provide the same trust contract for raw RAM/ROM blocks;
		// handler-backed sources and the QEMU audit validate in FindCachedBlock().
		// A newly compiled block is source-proven by CompileIntoCacheEntry().

		psxRegs.pc = block.start_pc;
		if (block.wait_loop_shape && block.wait_loop_enabled_at_compile &&
			(block.poll_call_wait_loop ? TryFastForwardPollCallWaitLoop(block) :
									TryFastForwardTrustedWaitLoopAtPc(block.start_pc)))
		{
			if (publish_details)
			{
				result->exit = BlockExitKind::Direct;
				PublishExecutionDetails(block, result);
			}
#if defined(VITASX2_QEMU_VALIDATION)
			else
			{
				result->instruction_count = block.instruction_count;
				result->pinned_gpr_memory_ops_saved = block.pinned_gpr_memory_ops_saved;
			}
#endif
			result->wait_loop_fast_forward = true;
			return true;
		}

#if defined(VITASX2_QEMU_VALIDATION)
		if (block.direct_budget_exit)
			m_direct_budget_exit_provider_entries++;
		if (block.constant_cycle_budget)
			m_constant_cycle_budget_provider_entries++;
		m_clock_mode_check_instructions_removed += block.clock_mode_check_instructions_removed;
		m_saved_register_stack_words_removed += block.saved_register_stack_words_removed;
		m_saved_register_frame_instructions_added += block.saved_register_frame_instructions_added;
		m_saved_register_frame_instructions_removed += block.saved_register_frame_instructions_removed;
		m_batched_cycle_instructions_removed += block.batched_cycle_instructions_removed;
		m_batched_cycle_stack_words_removed += block.batched_cycle_stack_words_removed;
		if (block.expanded_cycle_batching)
			m_expanded_cycle_batching_provider_entries++;
#endif
		const u32 exit_value = reinterpret_cast<GeneratedBlock>(block.code.EntryPoint())();

		BlockExitKind exit = BlockExitKind::Direct;
		if (!DecodeExitKind(exit_value, &exit))
			return false;

		if (publish_details)
		{
			result->exit = exit;
			PublishExecutionDetails(block, result);
		}
#if defined(VITASX2_QEMU_VALIDATION)
		else
		{
			result->instruction_count = block.instruction_count;
			result->pinned_gpr_memory_ops_saved = block.pinned_gpr_memory_ops_saved;
		}
#endif
		return true;
	}

	bool BlockExecutor::ExecuteCompiledBlock(u32 start_pc, u32 instruction_count,
		BlockExecutionResult* result, bool publish_details)
	{
		if (!result || instruction_count == 0 ||
			instruction_count > MAX_STRAIGHT_LINE_BLOCK_INSTRUCTIONS ||
			instruction_count > ((UINT32_MAX - start_pc) / 4))
		{
			return false;
		}

		result->cache_hit = false;
		result->lookup_hit = false;
		result->fast_dispatch_hit = false;
		result->wait_loop_fast_forward = false;
		CachedBlock* block = nullptr;
		bool lookup_hit = false;
		if (FindCachedBlock(start_pc, instruction_count, &block, &lookup_hit))
		{
			result->cache_hit = true;
			result->lookup_hit = lookup_hit;
			return RunValidatedBlock(*block, result, publish_details);
		}

		block = AllocateCacheEntry();
		if (!block || !CompileIntoCacheEntry(*block, start_pc, instruction_count))
			return false;

		result->cache_hit = false;
		result->lookup_hit = false;
		const bool ran = RunValidatedBlock(*block, result, publish_details);
		if (ran && !publish_details)
		{
			result->instruction_count = block->instruction_count;
			result->native_instruction_count = block->native_instruction_count;
			result->helper_instruction_count = block->helper_instruction_count;
			result->code_cache_resets = m_code_cache_resets;
		}
		return ran;
	}

	bool BlockExecutor::ExecuteCompiledBlockAtPc(
		u32 start_pc, BlockExecutionResult* result, bool publish_details)
	{
		if (!result || (start_pc & 0x3u) != 0)
			return false;

		result->cache_hit = false;
		result->lookup_hit = false;
		result->fast_dispatch_hit = false;
		result->wait_loop_fast_forward = false;

		// PCSX2 owner: x86/BaseblockEx.h::PC_GETBLOCK_() looks up the
		// translated BaseBlock by guest PC before doing any decode work. Keep
		// the Vita IOP hot path on the same shape. A 64-set, two-way exact
		// first-level cache adapts psxRecLUT's direct lookup to Vita's smaller
		// memory budget;
		// the lazy two-level table remains the collision and cold fallback.
		if (CachedBlock* entry = FindHotDispatchCacheBlock(start_pc))
		{
			// PCSX2's psxRecLUT dispatcher trusts a live exact translation because
			// psxRecClearMem() removes stale RAM blocks before dispatch. Raw Vita
			// sources have the same explicit-invalidation contract; immutable ROM
			// also needs no per-hit comparison. Handler-backed and cross-page
			// sources retain ValidateCachedBlock(), as does the opt-in QEMU audit.
			bool trust_raw_source = (entry->raw_opcodes != nullptr);
#if defined(VITASX2_QEMU_VALIDATION)
			trust_raw_source &= !s_qemuIopTrustedSourceAuditEnabled;
			if (trust_raw_source)
				m_hot_dispatch_trusted_raw_hits++;
#endif
			if (trust_raw_source || ValidateCachedBlock(*entry))
			{
#if defined(VITASX2_QEMU_VALIDATION)
				m_hot_dispatch_cache_hits++;
#endif
				result->cache_hit = true;
				result->lookup_hit = true;
				result->fast_dispatch_hit = true;
				return RunValidatedBlock(*entry, result, publish_details);
			}
		}
#if defined(VITASX2_QEMU_VALIDATION)
		m_hot_dispatch_cache_misses++;
#endif

		// Validate the page-table fallback before execution, then promote it.
		if (CachedBlock* entry = FindLookupBlockByStartPc(start_pc))
		{
			if (entry->valid && ValidateCachedBlock(*entry))
			{
				RegisterHotDispatchCache(*entry);
				result->cache_hit = true;
				result->lookup_hit = true;
				result->fast_dispatch_hit = true;
				return RunValidatedBlock(*entry, result, publish_details);
			}
		}

		if (CachedBlock* entry = FindRecordedBlockByStartPc(start_pc, 0, false))
		{
			RegisterHotDispatchCache(*entry);
			result->cache_hit = true;
			result->fast_dispatch_hit = true;
			return RunValidatedBlock(*entry, result, publish_details);
		}

		BlockScanResult scan;
		if (!ScanStraightLineBlock(start_pc, MAX_STRAIGHT_LINE_BLOCK_INSTRUCTIONS, &scan) ||
			scan.instruction_count == 0)
		{
			return false;
		}

		return ExecuteCompiledBlock(start_pc, scan.instruction_count, result, publish_details);
	}
} // namespace VitaIOP
