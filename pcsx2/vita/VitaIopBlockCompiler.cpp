// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#include "pcsx2/vita/VitaIopBlockCompiler.h"

#include "common/Vita/VitaJitMemory.h"
#include "pcsx2/IopMem.h"
#include "pcsx2/R3000A.h"
#include "pcsx2/R5900.h"
#include "pcsx2/vita/VitaCore.h"

#include <algorithm>
#include <new>

namespace
{
	using GeneratedBlock = u32 (*)();

	constexpr u16 REG_R4 = 1u << 4;
	constexpr u16 REG_LR = 1u << 14;
	constexpr u16 REG_PC = 1u << 15;

	constexpr unsigned HOST_TMP0 = 0;
	constexpr unsigned HOST_TMP1 = 1;
	constexpr unsigned HOST_TMP2 = 2;
	constexpr unsigned HOST_PSX_REGS = 4;
	constexpr unsigned HOST_CALL_SCRATCH = 12;

	constexpr size_t GPR_OFFSET = offsetof(psxRegisters, GPR);
	constexpr size_t HI_OFFSET = GPR_OFFSET + offsetof(GPRRegs, n.hi);
	constexpr size_t LO_OFFSET = GPR_OFFSET + offsetof(GPRRegs, n.lo);
	constexpr size_t PC_OFFSET = offsetof(psxRegisters, pc);
	constexpr size_t CODE_OFFSET = offsetof(psxRegisters, code);
	constexpr size_t CYCLE_OFFSET = offsetof(psxRegisters, cycle);
	constexpr size_t IOP_CYCLE_EE_OFFSET = offsetof(psxRegisters, iopCycleEE);

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

	constexpr size_t GprOffset(unsigned guest_reg)
	{
		return GPR_OFFSET + guest_reg * sizeof(u32);
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
			case 0x10: // MFHI
			case 0x11: // MTHI
			case 0x12: // MFLO
			case 0x13: // MTLO
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
			case 0x04: // BEQ
			case 0x05: // BNE
			case 0x08: // ADDI
			case 0x09: // ADDIU
			case 0x0a: // SLTI
			case 0x0b: // SLTIU
			case 0x0c: // ANDI
			case 0x0d: // ORI
			case 0x0e: // XORI
			case 0x0f: // LUI
			case 0x20: // LB
			case 0x21: // LH
			case 0x23: // LW
			case 0x24: // LBU
			case 0x25: // LHU
			case 0x28: // SB
			case 0x29: // SH
			case 0x2b: // SW
				return true;
			default:
				return false;
		}
	}

	static_assert(PC_OFFSET <= 4095);
	static_assert(CODE_OFFSET <= 4095);
	static_assert(CYCLE_OFFSET + sizeof(u32) <= 4095);
	static_assert(IOP_CYCLE_EE_OFFSET <= 4095);
	static_assert(GprOffset(33) + sizeof(u32) <= 4095);
	static_assert(HI_OFFSET + sizeof(u32) <= 4095);
	static_assert(LO_OFFSET + sizeof(u32) <= 4095);

	extern "C" __attribute__((noinline)) bool VitaIopA32TraceInstruction(u32 pc, u32 opcode)
	{
		// PCSX2 owner: R3000AInterpreter.cpp::execI() stores psxRegs.code,
		// records DebugTools/IopTrace.cpp::RecordIopPreInstruction, and only
		// then advances pc/cycle. Keep the generated helper-tail path at that
		// same hook point.
		if (!VitaRecordIopPreInstruction(pc, opcode))
			return false;

		psxRegs.iopCycleEE = 0;
		if (Cpu)
			Cpu->ExitExecution();
		return true;
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
	BlockCompiler::BlockCompiler(VitaA32::CodeBuffer& code)
		: m_code(code)
	{
	}

	bool BlockCompiler::CanCompileOpcode(u32)
	{
		// PCSX2 owner: R3000AInterpreter.cpp::execI() dispatches every fetched
		// word through R3000AOpcodeTables.cpp::psxBSC, where invalid slots route
		// to psxNULL(). The first IOP A32 provider preserves that table as the
		// semantic oracle instead of maintaining a second validity table.
		return true;
	}

	bool BlockCompiler::BeginBlock()
	{
		return m_code.EmitPush(REG_R4 | REG_LR) &&
			   m_code.EmitMovImm32(HOST_PSX_REGS, static_cast<u32>(reinterpret_cast<uptr>(&psxRegs)));
	}

	bool BlockCompiler::EndBlockReturn(BlockExitKind exit)
	{
		return m_code.EmitMovImm32(HOST_TMP0, static_cast<u32>(exit)) &&
			   m_code.EmitPop(REG_R4 | REG_PC);
	}

	bool BlockCompiler::EmitStoreCode(u32 op)
	{
		return m_code.EmitMovImm32(HOST_TMP0, op) &&
			   m_code.EmitStrImm12(HOST_TMP0, HOST_PSX_REGS, CODE_OFFSET);
	}

	bool BlockCompiler::EmitTraceCheck(u32 pc, u32 op, std::vector<size_t>& direct_exit_branches)
	{
		if (!m_code.EmitMovImm32(HOST_TMP0, pc) ||
			!m_code.EmitMovImm32(HOST_TMP1, op) ||
			!m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&VitaIopA32TraceInstruction)) ||
			!m_code.EmitMovImm8(HOST_TMP1, 0) ||
			!m_code.EmitCmpReg(HOST_TMP0, HOST_TMP1))
		{
			return false;
		}

		direct_exit_branches.push_back(m_code.EmitBranchPlaceholder(VitaA32::Condition::NE));
		return true;
	}

	bool BlockCompiler::EmitStorePc(u32 pc)
	{
		return m_code.EmitMovImm32(HOST_TMP0, pc) &&
			   m_code.EmitStrImm12(HOST_TMP0, HOST_PSX_REGS, PC_OFFSET);
	}

	bool BlockCompiler::EmitIncrementCycle()
	{
		return m_code.EmitLdrImm12(HOST_TMP0, HOST_PSX_REGS, CYCLE_OFFSET) &&
			   m_code.EmitLdrImm12(HOST_TMP1, HOST_PSX_REGS, CYCLE_OFFSET + sizeof(u32)) &&
			   m_code.EmitAddImm8(HOST_TMP0, HOST_TMP0, 1, true) &&
			   m_code.EmitAdcImm8(HOST_TMP1, HOST_TMP1, 0) &&
			   m_code.EmitStrImm12(HOST_TMP0, HOST_PSX_REGS, CYCLE_OFFSET) &&
			   m_code.EmitStrImm12(HOST_TMP1, HOST_PSX_REGS, CYCLE_OFFSET + sizeof(u32));
	}

	bool BlockCompiler::EmitPcChangedExitCheck(u32 expected_pc, std::vector<size_t>& direct_exit_branches)
	{
		if (!m_code.EmitLdrImm12(HOST_TMP0, HOST_PSX_REGS, PC_OFFSET) ||
			!m_code.EmitMovImm32(HOST_TMP1, expected_pc) ||
			!m_code.EmitCmpReg(HOST_TMP0, HOST_TMP1))
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

		return m_code.EmitLdrImm12(host_reg, HOST_PSX_REGS, static_cast<u16>(GprOffset(guest_reg)));
	}

	bool BlockCompiler::EmitStoreGpr(unsigned guest_reg, unsigned host_reg)
	{
		if (guest_reg == 0)
			return true;

		return m_code.EmitStrImm12(host_reg, HOST_PSX_REGS, static_cast<u16>(GprOffset(guest_reg)));
	}

	bool BlockCompiler::EmitMoveGpr(unsigned dst_guest_reg, unsigned src_guest_reg)
	{
		return EmitLoadGpr(src_guest_reg, HOST_TMP0) &&
			   EmitStoreGpr(dst_guest_reg, HOST_TMP0);
	}

	bool BlockCompiler::EmitBinaryRegOp(u32 op)
	{
		const unsigned rd = RD(op);
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		const u32 funct = op & 0x3f;

		if (rd == 0)
			return true;

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

		if (!EmitLoadGpr(rt, HOST_TMP0))
			return false;
		if (sa == 0)
			return EmitStoreGpr(rd, HOST_TMP0);

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

		if (!EmitLoadGpr(rt, HOST_TMP0) ||
			!EmitLoadGpr(rs, HOST_TMP1) ||
			!m_code.EmitAndImm8(HOST_TMP1, HOST_TMP1, 0x1f))
		{
			return false;
		}

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

		return EmitLoadGpr(rs, HOST_TMP0) &&
			   EmitLoadGpr(rt, HOST_TMP1) &&
			   m_code.EmitCmpReg(HOST_TMP0, HOST_TMP1) &&
			   m_code.EmitMovImm8(HOST_TMP2, 0) &&
			   m_code.EmitMovImm8(HOST_TMP2, 1, is_signed ? VitaA32::Condition::LT : VitaA32::Condition::CC) &&
			   EmitStoreGpr(rd, HOST_TMP2);
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

		if (!EmitLoadGpr(rs, HOST_TMP0))
			return false;

		switch (opcode)
		{
			case 0x08: // ADDI
			case 0x09: // ADDIU
				return m_code.EmitMovImm32(HOST_TMP1, static_cast<u32>(static_cast<s32>(IMM_S(op)))) &&
					   m_code.EmitAddReg(HOST_TMP2, HOST_TMP0, HOST_TMP1) &&
					   EmitStoreGpr(rt, HOST_TMP2);
			case 0x0a: // SLTI
				return m_code.EmitMovImm32(HOST_TMP1, static_cast<u32>(static_cast<s32>(IMM_S(op)))) &&
					   m_code.EmitCmpReg(HOST_TMP0, HOST_TMP1) &&
					   m_code.EmitMovImm8(HOST_TMP2, 0) &&
					   m_code.EmitMovImm8(HOST_TMP2, 1, VitaA32::Condition::LT) &&
					   EmitStoreGpr(rt, HOST_TMP2);
			case 0x0b: // SLTIU
				return m_code.EmitMovImm32(HOST_TMP1, static_cast<u32>(static_cast<s32>(IMM_S(op)))) &&
					   m_code.EmitCmpReg(HOST_TMP0, HOST_TMP1) &&
					   m_code.EmitMovImm8(HOST_TMP2, 0) &&
					   m_code.EmitMovImm8(HOST_TMP2, 1, VitaA32::Condition::CC) &&
					   EmitStoreGpr(rt, HOST_TMP2);
			case 0x0c: // ANDI
				return m_code.EmitMovImm32(HOST_TMP1, IMM_U(op)) &&
					   m_code.EmitAndReg(HOST_TMP2, HOST_TMP0, HOST_TMP1) &&
					   EmitStoreGpr(rt, HOST_TMP2);
			case 0x0d: // ORI
				return m_code.EmitMovImm32(HOST_TMP1, IMM_U(op)) &&
					   m_code.EmitOrrReg(HOST_TMP2, HOST_TMP0, HOST_TMP1) &&
					   EmitStoreGpr(rt, HOST_TMP2);
			case 0x0e: // XORI
				return m_code.EmitMovImm32(HOST_TMP1, IMM_U(op)) &&
					   m_code.EmitEorReg(HOST_TMP2, HOST_TMP0, HOST_TMP1) &&
					   EmitStoreGpr(rt, HOST_TMP2);
			default:
				return false;
		}
	}

	bool BlockCompiler::EmitEffectiveAddress(u32 op)
	{
		return EmitLoadGpr(RS(op), HOST_TMP0) &&
			   m_code.EmitMovImm32(HOST_TMP1, static_cast<u32>(static_cast<s32>(IMM_S(op)))) &&
			   m_code.EmitAddReg(HOST_TMP0, HOST_TMP0, HOST_TMP1);
	}

	bool BlockCompiler::EmitLoadOp(u32 op)
	{
		const unsigned opcode = op >> 26;
		const unsigned rt = RT(op);
		const void* helper = nullptr;

		switch (opcode)
		{
			case 0x20: // LB
			case 0x24: // LBU
				helper = reinterpret_cast<const void*>(&iopMemRead8);
				break;
			case 0x21: // LH
			case 0x25: // LHU
				helper = reinterpret_cast<const void*>(&iopMemRead16);
				break;
			case 0x23: // LW
				helper = reinterpret_cast<const void*>(&iopMemRead32);
				break;
			default:
				return false;
		}

		if (!EmitEffectiveAddress(op) ||
			!m_code.EmitCallAbsolute(helper, HOST_CALL_SCRATCH))
		{
			return false;
		}

		if (rt == 0)
			return true;

		switch (opcode)
		{
			case 0x20: // LB
				if (!m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_TMP0, VitaA32::ShiftType::LSL, 24) ||
					!m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_TMP0, VitaA32::ShiftType::ASR, 24))
				{
					return false;
				}
				break;
			case 0x21: // LH
				if (!m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_TMP0, VitaA32::ShiftType::LSL, 16) ||
					!m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_TMP0, VitaA32::ShiftType::ASR, 16))
				{
					return false;
				}
				break;
			default:
				break;
		}

		return EmitStoreGpr(rt, HOST_TMP0);
	}

	bool BlockCompiler::EmitStoreOp(u32 op)
	{
		const unsigned opcode = op >> 26;
		const void* helper = nullptr;

		switch (opcode)
		{
			case 0x28: // SB
				helper = reinterpret_cast<const void*>(&iopMemWrite8);
				break;
			case 0x29: // SH
				helper = reinterpret_cast<const void*>(&iopMemWrite16);
				break;
			case 0x2b: // SW
				helper = reinterpret_cast<const void*>(&iopMemWrite32);
				break;
			default:
				return false;
		}

		return EmitEffectiveAddress(op) &&
			   EmitLoadGpr(RT(op), HOST_TMP1) &&
			   m_code.EmitCallAbsolute(helper, HOST_CALL_SCRATCH);
	}

	bool BlockCompiler::EmitConditionalBranchOp(u32 op, u32 pc)
	{
		if (!EmitLoadGpr(RS(op), HOST_TMP0) ||
			!EmitLoadGpr(RT(op), HOST_TMP1) ||
			!m_code.EmitCmpReg(HOST_TMP0, HOST_TMP1))
		{
			return false;
		}

		const VitaA32::Condition skip_taken =
			((op >> 26) == 0x04) ? VitaA32::Condition::NE : VitaA32::Condition::EQ;
		const size_t not_taken = m_code.EmitBranchPlaceholder(skip_taken);
		return m_code.EmitMovImm32(HOST_TMP0, BranchTarget(pc, op)) &&
			   m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&psxDoBranch), HOST_CALL_SCRATCH) &&
			   EndBlockReturn(BlockExitKind::Direct) &&
			   m_code.PatchBranch(not_taken, m_code.Size(), skip_taken);
	}

	bool BlockCompiler::EmitNativeSPECIAL(u32 op)
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
			case 0x10: // MFHI
				return EmitMoveGpr(RD(op), 32);
			case 0x11: // MTHI
				return EmitLoadGpr(RS(op), HOST_TMP0) &&
					   m_code.EmitStrImm12(HOST_TMP0, HOST_PSX_REGS, static_cast<u16>(HI_OFFSET));
			case 0x12: // MFLO
				return EmitMoveGpr(RD(op), 33);
			case 0x13: // MTLO
				return EmitLoadGpr(RS(op), HOST_TMP0) &&
					   m_code.EmitStrImm12(HOST_TMP0, HOST_PSX_REGS, static_cast<u16>(LO_OFFSET));
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

	bool BlockCompiler::EmitNativeInstruction(u32 op, u32 pc)
	{
		switch (op >> 26)
		{
			case 0x00: // SPECIAL
				return EmitNativeSPECIAL(op);
			case 0x04: // BEQ
			case 0x05: // BNE
				return EmitConditionalBranchOp(op, pc);
			case 0x08: // ADDI
			case 0x09: // ADDIU
			case 0x0a: // SLTI
			case 0x0b: // SLTIU
			case 0x0c: // ANDI
			case 0x0d: // ORI
			case 0x0e: // XORI
			case 0x0f: // LUI
				return EmitImmediateOp(op);
			case 0x20: // LB
			case 0x21: // LH
			case 0x23: // LW
			case 0x24: // LBU
			case 0x25: // LHU
				return EmitLoadOp(op);
			case 0x28: // SB
			case 0x29: // SH
			case 0x2b: // SW
				return EmitStoreOp(op);
			default:
				return false;
		}
	}

	bool BlockCompiler::EmitInstruction(u32 op, u32 pc, std::vector<size_t>& direct_exit_branches)
	{
		const u32 next_pc = pc + 4;
		if (!EmitStoreCode(op) ||
			!EmitTraceCheck(pc, op, direct_exit_branches) ||
			!EmitStorePc(next_pc) ||
			!EmitIncrementCycle())
		{
			return false;
		}

		// PCSX2 owner: R3000AOpcodeTables.cpp implements these pure R3000A
		// integer operations directly; x86/iR3000Atables.cpp lowers the same
		// batch to native host ALU/shifter instructions. Keep every other opcode
		// on the PCSX2 helper-tail path until its owner is ported in a verified
		// batch.
		if (IsNativeOpcode(op))
		{
			if (!EmitNativeInstruction(op, pc))
				return false;

			m_native_instruction_count++;
			return true;
		}

		void (*helper)() = psxBSC[op >> 26];
		if (!m_code.EmitCallAbsolute(reinterpret_cast<const void*>(helper), HOST_CALL_SCRATCH) ||
			!EmitPcChangedExitCheck(next_pc, direct_exit_branches))
		{
			return false;
		}

		m_helper_instruction_count++;
		return true;
	}

	bool BlockCompiler::EmitHelperInstruction(u32 op, u32 pc, std::vector<size_t>& direct_exit_branches)
	{
		const u32 next_pc = pc + 4;
		void (*helper)() = psxBSC[op >> 26];

		return EmitStoreCode(op) &&
			   EmitTraceCheck(pc, op, direct_exit_branches) &&
			   EmitStorePc(next_pc) &&
			   EmitIncrementCycle() &&
			   m_code.EmitCallAbsolute(reinterpret_cast<const void*>(helper), HOST_CALL_SCRATCH) &&
			   EmitPcChangedExitCheck(next_pc, direct_exit_branches);
	}

	bool BlockCompiler::CompileStraightLineBlock(u32 start_pc, u32 instruction_count)
	{
		if (instruction_count == 0 ||
			instruction_count > BlockExecutor::MAX_STRAIGHT_LINE_BLOCK_INSTRUCTIONS ||
			instruction_count > ((UINT32_MAX - start_pc) / 4))
		{
			return false;
		}

		if (!BeginBlock())
			return false;

		std::vector<size_t> direct_exit_branches;
		direct_exit_branches.reserve(instruction_count * 2);
		m_native_instruction_count = 0;
		m_helper_instruction_count = 0;
		for (u32 i = 0; i < instruction_count; i++)
		{
			const u32 pc = start_pc + i * 4;
			const u32 op = iopMemRead32(pc);
			if (!CanCompileOpcode(op) || !EmitInstruction(op, pc, direct_exit_branches))
				return false;
		}

		const size_t direct_exit_offset = m_code.Size();
		if (!EndBlockReturn(BlockExitKind::Direct))
			return false;

		for (const size_t branch_offset : direct_exit_branches)
		{
			if (!m_code.PatchBranch(branch_offset, direct_exit_offset, VitaA32::Condition::NE))
				return false;
		}

		return true;
	}

	BlockExecutor::BlockExecutor()
	{
		m_cache.reserve(INITIAL_CACHE_CAPACITY);
	}

	BlockExecutor::~BlockExecutor()
	{
		Reset();
		ReleaseCodeCache();
	}

	u32 BlockExecutor::Reset()
	{
		u32 invalidated = 0;
		for (const std::unique_ptr<CachedBlock>& entry : m_cache)
		{
			if (entry->valid)
				invalidated++;

			entry->valid = false;
			entry->code.Release();
		}

		const u32 previous_resets = m_code_cache_resets;
		ReleaseCodeCache();
		m_code_cache_resets = previous_resets;
		return invalidated;
	}

	void BlockExecutor::InvalidateCachedBlock(CachedBlock& block)
	{
		if (!block.valid)
			return;

		block.valid = false;
		block.code.Release();
	}

	u32 BlockExecutor::InvalidateRange(u32 start_pc, u32 instruction_count)
	{
		if (instruction_count == 0 || instruction_count > ((UINT32_MAX - start_pc) / 4))
			return 0;

		const u32 end_pc = start_pc + instruction_count * 4;
		u32 invalidated = 0;
		for (const std::unique_ptr<CachedBlock>& entry : m_cache)
		{
			CachedBlock& block = *entry;
			if (!block.valid)
				continue;

			const u32 block_end = block.start_pc + block.instruction_count * 4;
			if (start_pc < block_end && block.start_pc < end_pc)
			{
				InvalidateCachedBlock(block);
				invalidated++;
			}
		}

		return invalidated;
	}

	bool BlockExecutor::ScanStraightLineBlock(u32 start_pc, u32 max_instruction_count, BlockScanResult* result)
	{
		if (!result || max_instruction_count == 0)
			return false;

		*result = {};
		result->start_pc = start_pc;
		result->stop_pc = start_pc;

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

			result->instruction_count++;
			result->stop_pc = pc + 4;
		}

		return true;
	}

	bool BlockExecutor::ValidateCachedBlock(CachedBlock& block)
	{
		if (!block.valid)
			return false;

		bool matches = true;
		for (u32 i = 0; matches && i < block.instruction_count; i++)
			matches = (block.opcodes[i] == iopMemRead32(block.start_pc + i * 4));

		if (matches)
			return true;

		// PCSX2 owner: x86/iR3000A.cpp::psxRecClearMem() invalidates changed
		// translated ranges. The Vita path also validates cached opcodes before
		// dispatch because it cannot rely on x86 protected-page repair.
		InvalidateCachedBlock(block);
		return false;
	}

	BlockExecutor::CachedBlock* BlockExecutor::FindCachedBlock(u32 start_pc, u32 instruction_count)
	{
		for (const std::unique_ptr<CachedBlock>& entry : m_cache)
		{
			CachedBlock& block = *entry;
			if (block.valid && block.start_pc == start_pc && block.instruction_count == instruction_count &&
				ValidateCachedBlock(block))
			{
				return &block;
			}
		}

		return nullptr;
	}

	BlockExecutor::CachedBlock* BlockExecutor::AllocateCacheEntry()
	{
		for (const std::unique_ptr<CachedBlock>& entry : m_cache)
		{
			if (!entry->valid)
				return entry.get();
		}

		if (m_cache.size() < MAX_CACHE_CAPACITY)
		{
			std::unique_ptr<CachedBlock> entry(new (std::nothrow) CachedBlock());
			if (!entry)
				return nullptr;

			CachedBlock* block = entry.get();
			m_cache.push_back(std::move(entry));
			return block;
		}

		ResetForCachePressure();
		for (const std::unique_ptr<CachedBlock>& entry : m_cache)
		{
			if (!entry->valid)
				return entry.get();
		}

		return nullptr;
	}

	bool BlockExecutor::EnsureCodeCache()
	{
		if (m_code_cache)
			return true;

		// PCSX2 owner: x86/iR3000A.cpp::recReserve()/recResetIOP() use one IOP
		// recompiler arena and BASEBLOCK records inside it. Vita mirrors that
		// with one ARM code arena for cached R3000A helper-tail blocks.
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
			block.opcodes[i] = iopMemRead32(start_pc + i * 4);

		size_t block_code_capacity = STRAIGHT_LINE_BLOCK_CODE_CAPACITY;
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

			BlockCompiler compiler(block.code);
			const bool compiled = compiler.CompileStraightLineBlock(start_pc, instruction_count);
			const bool out_of_block_space = !compiled && block.code.Size() >= block.code.Capacity();
			if (compiled && block.code.Flush())
			{
				block.start_pc = start_pc;
				block.instruction_count = instruction_count;
				block.native_instruction_count = compiler.NativeInstructionCount();
				block.helper_instruction_count = compiler.HelperInstructionCount();
				block.valid = true;
				return true;
			}

			block.code.Release();
			RewindCodeCache(code_slice_offset);
			if (!out_of_block_space || block_code_capacity >= MAX_STRAIGHT_LINE_BLOCK_CODE_CAPACITY)
				return false;

			block_code_capacity *= 2;
		}
	}

	bool BlockExecutor::RunCachedBlock(CachedBlock& block, BlockExecutionResult* result)
	{
		if (!result || !ValidateCachedBlock(block))
			return false;

		psxRegs.pc = block.start_pc;
		const u32 exit_value = reinterpret_cast<GeneratedBlock>(block.code.EntryPoint())();

		BlockExitKind exit = BlockExitKind::Direct;
		if (!DecodeExitKind(exit_value, &exit))
			return false;

		result->exit = exit;
		result->instruction_count = block.instruction_count;
		result->native_instruction_count = block.native_instruction_count;
		result->helper_instruction_count = block.helper_instruction_count;
		result->code_size = block.code.Size();
		result->cache_slots = static_cast<u32>(m_cache.size());
		result->code_cache_resets = m_code_cache_resets;
		result->code_cache_used = m_code_cache_used;
		result->code_cache_capacity = m_code_cache_capacity;
		return true;
	}

	bool BlockExecutor::ExecuteCompiledBlock(u32 start_pc, u32 instruction_count, BlockExecutionResult* result)
	{
		if (!result || instruction_count == 0 ||
			instruction_count > MAX_STRAIGHT_LINE_BLOCK_INSTRUCTIONS ||
			instruction_count > ((UINT32_MAX - start_pc) / 4))
		{
			return false;
		}

		*result = {};
		if (CachedBlock* block = FindCachedBlock(start_pc, instruction_count))
		{
			result->cache_hit = true;
			return RunCachedBlock(*block, result);
		}

		CachedBlock* block = AllocateCacheEntry();
		if (!block || !CompileIntoCacheEntry(*block, start_pc, instruction_count))
			return false;

		result->cache_hit = false;
		return RunCachedBlock(*block, result);
	}
} // namespace VitaIOP
