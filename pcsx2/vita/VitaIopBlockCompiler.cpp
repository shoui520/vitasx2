// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#include "pcsx2/vita/VitaIopBlockCompiler.h"

#include "common/Vita/VitaJitMemory.h"
#include "pcsx2/IopGte.h"
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
	constexpr u16 REG_R5 = 1u << 5;
	constexpr u16 REG_R6 = 1u << 6;
	constexpr u16 REG_LR = 1u << 14;
	constexpr u16 REG_PC = 1u << 15;

	constexpr unsigned HOST_TMP0 = 0;
	constexpr unsigned HOST_TMP1 = 1;
	constexpr unsigned HOST_TMP2 = 2;
	constexpr unsigned HOST_TMP3 = 3;
	constexpr unsigned HOST_PSX_REGS = 4;
	constexpr unsigned HOST_SAVED0 = 5;
	constexpr unsigned HOST_SAVED1 = 6;
	constexpr unsigned HOST_CALL_SCRATCH = 12;

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

	constexpr u32 JumpTarget(u32 pc, u32 op)
	{
		return ((pc + 4) & 0xf0000000u) | ((op & 0x03ffffffu) << 2);
	}

	constexpr size_t GprOffset(unsigned guest_reg)
	{
		return GPR_OFFSET + guest_reg * sizeof(u32);
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

	static_assert(PC_OFFSET <= 4095);
	static_assert(CODE_OFFSET <= 4095);
	static_assert(CYCLE_OFFSET + sizeof(u32) <= 4095);
	static_assert(IOP_CYCLE_EE_OFFSET <= 4095);
	static_assert(GprOffset(33) + sizeof(u32) <= 4095);
	static_assert(HI_OFFSET + sizeof(u32) <= 4095);
	static_assert(LO_OFFSET + sizeof(u32) <= 4095);
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
	BlockCompiler::BlockCompiler(VitaA32::CodeBuffer& code)
		: m_code(code)
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
		return m_code.EmitPush(REG_R4 | REG_R5 | REG_R6 | REG_LR) &&
			   m_code.EmitMovImm32(HOST_PSX_REGS, static_cast<u32>(reinterpret_cast<uptr>(&psxRegs)));
	}

	bool BlockCompiler::EndBlockReturn(BlockExitKind exit)
	{
		return m_code.EmitMovImm32(HOST_TMP0, static_cast<u32>(exit)) &&
			   m_code.EmitPop(REG_R4 | REG_R5 | REG_R6 | REG_PC);
	}

	bool BlockCompiler::EndBlockDirectTail(const void* direct_exit, size_t* direct_link_target_offset)
	{
		if (!direct_exit)
			return false;

		if (!m_code.EmitPop(REG_R4 | REG_R5 | REG_R6 | REG_LR))
			return false;

		const size_t target_offset = m_code.Size();
		if (!m_code.EmitMovImm32(HOST_CALL_SCRATCH, static_cast<u32>(reinterpret_cast<uptr>(direct_exit))) ||
			!m_code.EmitBx(HOST_CALL_SCRATCH))
		{
			return false;
		}

		if (direct_link_target_offset)
			*direct_link_target_offset = target_offset;
		return true;
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

	bool BlockCompiler::EmitMultiplyOp(u32 op, bool is_signed)
	{
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
		const void* helper = is_signed ?
			reinterpret_cast<const void*>(&VitaIopA32DivResult) :
			reinterpret_cast<const void*>(&VitaIopA32DivuResult);

		return EmitLoadGpr(RS(op), HOST_TMP0) &&
			   EmitLoadGpr(RT(op), HOST_TMP1) &&
			   m_code.EmitCallAbsolute(helper, HOST_CALL_SCRATCH) &&
			   m_code.EmitStrImm12(HOST_TMP0, HOST_PSX_REGS, static_cast<u16>(LO_OFFSET)) &&
			   m_code.EmitStrImm12(HOST_TMP1, HOST_PSX_REGS, static_cast<u16>(HI_OFFSET));
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

	bool BlockCompiler::EmitUnalignedLoadOp(u32 op)
	{
		const bool left = ((op >> 26) == 0x22);
		if (!EmitEffectiveAddress(op) ||
			!m_code.EmitAndImm8(HOST_SAVED0, HOST_TMP0, 3) ||
			!m_code.EmitMovRegShiftImm(HOST_SAVED0, HOST_SAVED0, VitaA32::ShiftType::LSL, 3) ||
			!m_code.EmitMovImm32(HOST_TMP1, 0xfffffffcu) ||
			!m_code.EmitAndReg(HOST_TMP0, HOST_TMP0, HOST_TMP1) ||
			!m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&iopMemRead32), HOST_CALL_SCRATCH))
		{
			return false;
		}

		if (RT(op) == 0)
			return true;

		if (!EmitLoadGpr(RT(op), HOST_TMP1))
			return false;

		if (left)
		{
			return m_code.EmitMovImm32(HOST_TMP2, 0x00ffffffu) &&
				   m_code.EmitMovRegShiftReg(HOST_TMP2, HOST_TMP2, VitaA32::ShiftType::LSR, HOST_SAVED0) &&
				   m_code.EmitAndReg(HOST_TMP1, HOST_TMP1, HOST_TMP2) &&
				   m_code.EmitMovImm8(HOST_TMP3, 24) &&
				   m_code.EmitSubReg(HOST_TMP3, HOST_TMP3, HOST_SAVED0) &&
				   m_code.EmitMovRegShiftReg(HOST_TMP0, HOST_TMP0, VitaA32::ShiftType::LSL, HOST_TMP3) &&
				   m_code.EmitOrrReg(HOST_TMP0, HOST_TMP0, HOST_TMP1) &&
				   EmitStoreGpr(RT(op), HOST_TMP0);
		}

		return m_code.EmitMovImm32(HOST_TMP2, 0xffffff00u) &&
			   m_code.EmitMovImm8(HOST_TMP3, 24) &&
			   m_code.EmitSubReg(HOST_TMP3, HOST_TMP3, HOST_SAVED0) &&
			   m_code.EmitMovRegShiftReg(HOST_TMP2, HOST_TMP2, VitaA32::ShiftType::LSL, HOST_TMP3) &&
			   m_code.EmitAndReg(HOST_TMP1, HOST_TMP1, HOST_TMP2) &&
			   m_code.EmitMovRegShiftReg(HOST_TMP0, HOST_TMP0, VitaA32::ShiftType::LSR, HOST_SAVED0) &&
			   m_code.EmitOrrReg(HOST_TMP0, HOST_TMP0, HOST_TMP1) &&
			   EmitStoreGpr(RT(op), HOST_TMP0);
	}

	bool BlockCompiler::EmitUnalignedStoreOp(u32 op)
	{
		const bool left = ((op >> 26) == 0x2a);
		if (!EmitEffectiveAddress(op) ||
			!m_code.EmitAndImm8(HOST_SAVED0, HOST_TMP0, 3) ||
			!m_code.EmitMovRegShiftImm(HOST_SAVED0, HOST_SAVED0, VitaA32::ShiftType::LSL, 3) ||
			!m_code.EmitMovImm32(HOST_TMP1, 0xfffffffcu) ||
			!m_code.EmitAndReg(HOST_SAVED1, HOST_TMP0, HOST_TMP1) ||
			!m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_SAVED1, VitaA32::ShiftType::LSL, 0) ||
			!m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&iopMemRead32), HOST_CALL_SCRATCH) ||
			!EmitLoadGpr(RT(op), HOST_TMP1))
		{
			return false;
		}

		if (left)
		{
			if (!m_code.EmitMovImm8(HOST_TMP3, 24) ||
				!m_code.EmitSubReg(HOST_TMP3, HOST_TMP3, HOST_SAVED0) ||
				!m_code.EmitMovRegShiftReg(HOST_TMP1, HOST_TMP1, VitaA32::ShiftType::LSR, HOST_TMP3) ||
				!m_code.EmitMovImm32(HOST_TMP2, 0xffffff00u) ||
				!m_code.EmitMovRegShiftReg(HOST_TMP2, HOST_TMP2, VitaA32::ShiftType::LSL, HOST_SAVED0))
			{
				return false;
			}
		}
		else
		{
			if (!m_code.EmitMovRegShiftReg(HOST_TMP1, HOST_TMP1, VitaA32::ShiftType::LSL, HOST_SAVED0) ||
				!m_code.EmitMovImm8(HOST_TMP3, 24) ||
				!m_code.EmitSubReg(HOST_TMP3, HOST_TMP3, HOST_SAVED0) ||
				!m_code.EmitMovImm32(HOST_TMP2, 0x00ffffffu) ||
				!m_code.EmitMovRegShiftReg(HOST_TMP2, HOST_TMP2, VitaA32::ShiftType::LSR, HOST_TMP3))
			{
				return false;
			}
		}

		return m_code.EmitAndReg(HOST_TMP0, HOST_TMP0, HOST_TMP2) &&
			   m_code.EmitOrrReg(HOST_TMP1, HOST_TMP1, HOST_TMP0) &&
			   m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_SAVED1, VitaA32::ShiftType::LSL, 0) &&
			   m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&iopMemWrite32), HOST_CALL_SCRATCH);
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

		if (!EmitLoadGpr(RS(op), HOST_TMP0) ||
			!m_code.EmitMovImm8(HOST_TMP1, 0) ||
			!m_code.EmitCmpReg(HOST_TMP0, HOST_TMP1))
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
			   m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&psxDoBranch), HOST_CALL_SCRATCH) &&
			   EndBlockReturn(BlockExitKind::Direct) &&
			   m_code.PatchBranch(not_taken, m_code.Size(), skip_taken);
	}

	bool BlockCompiler::EmitJumpOp(u32 op, u32 pc)
	{
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

	bool BlockCompiler::EmitRegisterJumpOp(u32 op, u32 pc)
	{
		if ((op & 0x3f) == 0x09 && RD(op) != 0) // JALR
		{
			if (!m_code.EmitMovImm32(HOST_TMP0, pc + 8) ||
				!EmitStoreGpr(RD(op), HOST_TMP0))
			{
				return false;
			}
		}

		return EmitLoadGpr(RS(op), HOST_TMP0) &&
			   m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&psxDoBranch), HOST_CALL_SCRATCH) &&
			   EndBlockReturn(BlockExitKind::Direct);
	}

	bool BlockCompiler::EmitCop0TransferOp(u32 op, bool to_cop0)
	{
		if (to_cop0)
		{
			return EmitLoadGpr(RT(op), HOST_TMP0) &&
				   m_code.EmitStrImm12(HOST_TMP0, HOST_PSX_REGS, static_cast<u16>(Cp0Offset(RD(op))));
		}

		if (RT(op) == 0)
			return true;

		return m_code.EmitLdrImm12(HOST_TMP0, HOST_PSX_REGS, static_cast<u16>(Cp0Offset(RD(op)))) &&
			   EmitStoreGpr(RT(op), HOST_TMP0);
	}

	bool BlockCompiler::EmitCop0RfeOp()
	{
		return m_code.EmitLdrImm12(HOST_TMP0, HOST_PSX_REGS, static_cast<u16>(CP0_STATUS_OFFSET)) &&
			   m_code.EmitMovImm32(HOST_TMP1, 0xfffffff0u) &&
			   m_code.EmitAndReg(HOST_TMP2, HOST_TMP0, HOST_TMP1) &&
			   m_code.EmitMovImm32(HOST_TMP1, 0x3cu) &&
			   m_code.EmitAndReg(HOST_TMP0, HOST_TMP0, HOST_TMP1) &&
			   m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_TMP0, VitaA32::ShiftType::LSR, 2) &&
			   m_code.EmitOrrReg(HOST_TMP2, HOST_TMP2, HOST_TMP0) &&
			   m_code.EmitStrImm12(HOST_TMP2, HOST_PSX_REGS, static_cast<u16>(CP0_STATUS_OFFSET));
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
				return m_code.EmitMovRegShiftImm(HOST_TMP1, host_reg, VitaA32::ShiftType::LSL, 16) &&
					   m_code.EmitMovRegShiftImm(HOST_TMP1, HOST_TMP1, VitaA32::ShiftType::ASR, 16) &&
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
				return m_code.EmitMovImm32(HOST_TMP1, 0xffffu) &&
					   m_code.EmitAndReg(HOST_TMP1, host_reg, HOST_TMP1) &&
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
				return EmitLoadGpr(RT(op), HOST_TMP0) &&
					   EmitWriteCop2DataReg(RD(op), HOST_TMP0);

			case 0x06: // CTC2
				return EmitLoadGpr(RT(op), HOST_TMP0) &&
					   m_code.EmitStrImm12(HOST_TMP0, HOST_PSX_REGS, static_cast<u16>(Cp2cOffset(RD(op))));

			default:
				return false;
		}
	}

	bool BlockCompiler::EmitCop2LoadStoreOp(u32 op)
	{
		if ((op >> 26) == 0x32) // LWC2
		{
			return EmitEffectiveAddress(op) &&
				   m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&iopMemRead32), HOST_CALL_SCRATCH) &&
				   EmitWriteCop2DataReg(RT(op), HOST_TMP0);
		}

		if ((op >> 26) == 0x3a) // SWC2
		{
			return EmitReadCop2DataReg(RT(op), HOST_SAVED0) &&
				   EmitEffectiveAddress(op) &&
				   m_code.EmitMovRegShiftImm(HOST_TMP1, HOST_SAVED0, VitaA32::ShiftType::LSL, 0) &&
				   m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&iopMemWrite32), HOST_CALL_SCRATCH);
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
				return EmitMoveGpr(RD(op), 32);
			case 0x11: // MTHI
				return EmitLoadGpr(RS(op), HOST_TMP0) &&
					   m_code.EmitStrImm12(HOST_TMP0, HOST_PSX_REGS, static_cast<u16>(HI_OFFSET));
			case 0x12: // MFLO
				return EmitMoveGpr(RD(op), 33);
			case 0x13: // MTLO
				return EmitLoadGpr(RS(op), HOST_TMP0) &&
					   m_code.EmitStrImm12(HOST_TMP0, HOST_PSX_REGS, static_cast<u16>(LO_OFFSET));
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

		// PCSX2 owner: R3000AOpcodeTables.cpp implements the full valid IOP
		// table; x86/iR3000Atables.cpp lowers the same table to native host
		// operations. CanCompileOpcode() has already rejected psxNULL slots.
		if (!IsNativeOpcode(op) || !EmitNativeInstruction(op, pc))
			return false;

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

		if (!BeginBlock())
			return false;

		std::vector<size_t> direct_exit_branches;
		direct_exit_branches.reserve(instruction_count * 2);
		m_native_instruction_count = 0;
		m_helper_instruction_count = 0;
		bool can_direct_link_fallthrough = true;
		for (u32 i = 0; i < instruction_count; i++)
		{
			const u32 pc = start_pc + i * 4;
			const u32 op = iopMemRead32(pc);
			if (IsIopBranchOrJumpOpcode(op) || IsIopExceptionOpcode(op))
				can_direct_link_fallthrough = false;
			if (!CanCompileOpcode(op) || !EmitInstruction(op, pc, direct_exit_branches))
				return false;
		}

		const u32 next_pc = start_pc + instruction_count * 4;
		size_t direct_exit_offset = 0;
		const bool emit_link_tail = direct_exit && direct_links && can_direct_link_fallthrough;
		if (emit_link_tail)
		{
			size_t target_offset = 0;
			if (!EndBlockDirectTail(direct_exit, &target_offset))
				return false;

			direct_links->slots[0].target_pc = next_pc;
			direct_links->slots[0].target_offset = target_offset;
			direct_links->slots[0].valid = true;

			direct_exit_offset = m_code.Size();
			if (!EndBlockReturn(BlockExitKind::Direct))
				return false;
		}
		else
		{
			direct_exit_offset = m_code.Size();
			if (!EndBlockReturn(BlockExitKind::Direct))
				return false;
		}

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
		m_block_records.reserve(INITIAL_CACHE_CAPACITY);
		m_incoming_links.reserve(INITIAL_CACHE_CAPACITY * DIRECT_LINK_SLOT_COUNT);
	}

	BlockExecutor::~BlockExecutor()
	{
		Reset();
		ReleaseLookupPages();
		ReleaseCodeCache();
	}

	u32 BlockExecutor::LookupPageIndex(u32 start_pc)
	{
		return start_pc >> 16;
	}

	u32 BlockExecutor::LookupEntryIndex(u32 start_pc)
	{
		return (start_pc & 0xffffu) >> 2;
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

		if (LookupPage* page = GetLookupPage(block.start_pc, false))
		{
			CachedBlock*& entry = page->blocks[LookupEntryIndex(block.start_pc)];
			if (entry == &block)
				entry = nullptr;
		}
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
		while (insert_index < m_block_records.size() &&
			   m_block_records[insert_index].start_pc <= block.start_pc)
		{
			insert_index++;
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
		u32 write_index = 0;
		for (u32 read_index = 0; read_index < m_block_records.size(); read_index++)
		{
			if (m_block_records[read_index].block == &block)
				continue;

			if (write_index != read_index)
				m_block_records[write_index] = m_block_records[read_index];
			write_index++;
		}

		m_block_records.resize(write_index);
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

	DirectLinkSlot* BlockExecutor::GetRecordedDirectLink(IncomingLinkRecord& record)
	{
		if (!record.source || !record.source->valid || record.slot_index >= DIRECT_LINK_SLOT_COUNT)
			return nullptr;

		DirectLinkSlot& link = record.source->direct_links.slots[record.slot_index];
		if (!link.valid || link.target_pc != record.target_pc)
			return nullptr;

		return &link;
	}

	void BlockExecutor::ClearIncomingLinks()
	{
		m_incoming_links.clear();
	}

	void BlockExecutor::RegisterIncomingLinks(CachedBlock& block)
	{
		UnregisterIncomingLinks(block);

		// PCSX2 owner: x86/BaseblockEx.cpp::BaseBlocks::Link(). Keep target-PC
		// -> source patch-site records so invalidating a block only repairs its
		// incoming edges.
		for (u8 i = 0; i < DIRECT_LINK_SLOT_COUNT; i++)
		{
			const DirectLinkSlot& link = block.direct_links.slots[i];
			if (!link.valid || m_incoming_links.size() >= MAX_INCOMING_LINKS)
				continue;

			m_incoming_links.push_back({&block, link.target_pc, i});
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
		for (const std::unique_ptr<CachedBlock>& entry : m_cache)
		{
			if (entry->valid)
				invalidated++;

			entry->valid = false;
			entry->direct_links = {};
			entry->code.Release();
		}

		ClearBlockRecords();
		ClearIncomingLinks();
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

		UnlinkIncomingLinks(block.start_pc);
		UnregisterIncomingLinks(block);
		UnregisterBlockLookup(block);
		UnregisterBlockRecord(block);
		block.valid = false;
		block.direct_links = {};
		block.code.Release();
	}

	u32 BlockExecutor::InvalidateRange(u32 start_pc, u32 instruction_count)
	{
		if (instruction_count == 0 || instruction_count > ((UINT32_MAX - start_pc) / 4))
			return 0;

		const u32 end_pc = start_pc + instruction_count * 4;
		u32 invalidated = 0;
		for (u32 i = 0; i < m_block_records.size();)
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
					i >= ((UINT32_MAX - start_pc) / 4) ||
					((pc + 4) & 0xffcu) == 0)
				{
					return result->instruction_count != 0;
				}

				const u32 delay_pc = pc + 4;
				const u32 delay_op = iopMemRead32(delay_pc);
				if (!BlockCompiler::CanCompileOpcode(delay_op))
					return result->instruction_count != 0;

				add_instruction(pc);
				add_instruction(delay_pc);
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

	void BlockExecutor::ValidateCachedBlocks()
	{
		for (const std::unique_ptr<CachedBlock>& block : m_cache)
			ValidateCachedBlock(*block);
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

		size_t block_code_capacity = STRAIGHT_LINE_BLOCK_CODE_CAPACITY;
		size_t block_code_slice_offset = 0;
		u32 native_instruction_count = 0;
		u32 helper_instruction_count = 0;
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

			BlockCompiler compiler(block.code);
			DirectLinkSlots attempt_direct_links;
			const bool compiled = compiler.CompileStraightLineBlock(start_pc, instruction_count,
				reinterpret_cast<const void*>(&VitaIopA32DirectExit), &attempt_direct_links);
			const bool out_of_block_space = !compiled && block.code.Size() >= block.code.Capacity();
			if (compiled && block.code.Flush())
			{
				block_code_slice_offset = code_slice_offset;
				native_instruction_count = compiler.NativeInstructionCount();
				helper_instruction_count = compiler.HelperInstructionCount();
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
		block.native_instruction_count = native_instruction_count;
		block.helper_instruction_count = helper_instruction_count;
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
		if (!target || !block.valid || !link.valid)
			return false;

		return block.code.PatchMovImm32(link.target_offset, HOST_CALL_SCRATCH,
				   static_cast<u32>(reinterpret_cast<uptr>(target))) &&
			   block.code.Flush();
	}

	void BlockExecutor::PatchIncomingLinks(u32 target_pc, const void* target)
	{
		if (!m_direct_linking_enabled || !target)
			return;

		for (u32 i = 0; i < m_incoming_links.size(); i++)
		{
			IncomingLinkRecord& record = m_incoming_links[i];
			if (record.target_pc != target_pc)
				continue;

			if (DirectLinkSlot* link = GetRecordedDirectLink(record))
				PatchDirectLink(*record.source, *link, target);
		}
	}

	void BlockExecutor::UnlinkIncomingLinks(u32 target_pc)
	{
		for (u32 i = 0; i < m_incoming_links.size(); i++)
		{
			IncomingLinkRecord& record = m_incoming_links[i];
			if (target_pc != UINT32_MAX && record.target_pc != target_pc)
				continue;

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

	bool BlockExecutor::RunCachedBlock(CachedBlock& block, BlockExecutionResult* result)
	{
		if (!result || !block.valid)
			return false;

		ValidateCachedBlocks();
		if (!block.valid)
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
		result->block_records = static_cast<u32>(m_block_records.size());
		result->link_records = static_cast<u32>(m_incoming_links.size());
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
		CachedBlock* block = nullptr;
		bool lookup_hit = false;
		if (FindCachedBlock(start_pc, instruction_count, &block, &lookup_hit))
		{
			result->cache_hit = true;
			result->lookup_hit = lookup_hit;
			return RunCachedBlock(*block, result);
		}

		block = AllocateCacheEntry();
		if (!block || !CompileIntoCacheEntry(*block, start_pc, instruction_count))
			return false;

		result->cache_hit = false;
		result->lookup_hit = false;
		return RunCachedBlock(*block, result);
	}
} // namespace VitaIOP
