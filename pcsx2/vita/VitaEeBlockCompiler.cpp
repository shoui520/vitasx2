// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "pcsx2/vita/VitaEeBlockCompiler.h"

#include "pcsx2/Config.h"
#include "pcsx2/Memory.h"
#include "pcsx2/R5900.h"
#include "pcsx2/R5900OpcodeTables.h"
#include "pcsx2/vita/A32Emitter.h"

#include <cstddef>

namespace VitaEE
{
	namespace
	{
		constexpr u16 REG_R4 = 1u << 4;
		constexpr u16 REG_LR = 1u << 14;
		constexpr u16 REG_PC = 1u << 15;

		constexpr unsigned HOST_CPU_REGS = 4;
		constexpr unsigned HOST_TMP0 = 0;
		constexpr unsigned HOST_TMP1 = 1;
		constexpr unsigned HOST_TMP2 = 2;
		constexpr unsigned HOST_TMP3 = 3;
		constexpr unsigned HOST_TMP4 = 12;

		constexpr size_t GPR_OFFSET = offsetof(cpuRegisters, GPR);
		constexpr size_t PC_OFFSET = offsetof(cpuRegisters, pc);
		constexpr size_t CYCLE_OFFSET = offsetof(cpuRegisters, cycle);
		constexpr size_t NEXT_EVENT_OFFSET = offsetof(cpuRegisters, nextEventCycle);

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

		constexpr u16 IMM_U(u32 op)
		{
			return static_cast<u16>(op);
		}

		constexpr s16 IMM_S(u32 op)
		{
			return static_cast<s16>(op);
		}

		constexpr size_t GprOffset(unsigned guest_reg)
		{
			return GPR_OFFSET + sizeof(GPR_reg) * guest_reg;
		}

		bool CanCompileSPECIAL(u32 op)
		{
			switch (op & 0x3f)
			{
				case 0x00: // SLL, owned by R5900OpcodeImpl.cpp::SLL().
				case 0x02: // SRL, owned by R5900OpcodeImpl.cpp::SRL().
				case 0x03: // SRA, owned by R5900OpcodeImpl.cpp::SRA().
				case 0x04: // SLLV, owned by R5900OpcodeImpl.cpp::SLLV().
				case 0x06: // SRLV, owned by R5900OpcodeImpl.cpp::SRLV().
				case 0x07: // SRAV, owned by R5900OpcodeImpl.cpp::SRAV().
				case 0x21: // ADDU, owned by R5900OpcodeImpl.cpp::ADDU().
				case 0x23: // SUBU, owned by R5900OpcodeImpl.cpp::SUBU().
				case 0x24: // AND, owned by R5900OpcodeImpl.cpp::AND().
				case 0x25: // OR, owned by R5900OpcodeImpl.cpp::OR().
				case 0x26: // XOR, owned by R5900OpcodeImpl.cpp::XOR().
				case 0x27: // NOR, owned by R5900OpcodeImpl.cpp::NOR().
				case 0x2a: // SLT, owned by R5900OpcodeImpl.cpp::SLT().
				case 0x2b: // SLTU, owned by R5900OpcodeImpl.cpp::SLTU().
				case 0x2d: // DADDU, owned by R5900OpcodeImpl.cpp::DADDU().
				case 0x2f: // DSUBU, owned by R5900OpcodeImpl.cpp::DSUBU().
				case 0x38: // DSLL, owned by R5900OpcodeImpl.cpp::DSLL().
				case 0x3a: // DSRL, owned by R5900OpcodeImpl.cpp::DSRL().
				case 0x3b: // DSRA, owned by R5900OpcodeImpl.cpp::DSRA().
				case 0x3c: // DSLL32, owned by R5900OpcodeImpl.cpp::DSLL32().
				case 0x3e: // DSRL32, owned by R5900OpcodeImpl.cpp::DSRL32().
				case 0x3f: // DSRA32, owned by R5900OpcodeImpl.cpp::DSRA32().
					return true;
				default:
					return false;
			}
		}

		u32 ScaleBlockCycles(u32 raw_cycles)
		{
			// Ported from PCSX2 x86/ix86-32/iR5900.cpp::scaleblockcycles_calculation()
			// and matched with Interpreter.cpp::intUpdateCPUCycles().
			const bool lowcycles = (raw_cycles <= 40);
			const s8 cyclerate = EmuConfig.Speedhacks.EECycleRate;
			u32 scale_cycles = 0;

			if (cyclerate == 0 || lowcycles || cyclerate < -99 || cyclerate > 3)
				scale_cycles = raw_cycles >> 3;
			else if (cyclerate > 1)
				scale_cycles = raw_cycles >> (2 + cyclerate);
			else if (cyclerate == 1)
				scale_cycles = (raw_cycles >> 3) / 1.3f;
			else if (cyclerate == -1)
				scale_cycles = (raw_cycles <= 80 || raw_cycles > 168 ? 5 : 7) * raw_cycles / 32;
			else
				scale_cycles = ((5 + (-2 * (cyclerate + 1))) * raw_cycles) >> 5;

			return (scale_cycles < 1) ? 1 : scale_cycles;
		}
	} // namespace

	static_assert(GprOffset(31) + sizeof(u64) <= 0x0fff);
	static_assert(PC_OFFSET + sizeof(u32) <= 0x0fff);
	static_assert(CYCLE_OFFSET + sizeof(u64) <= 0x0fff);
	static_assert(NEXT_EVENT_OFFSET + sizeof(u64) <= 0x0fff);

	BlockCompiler::BlockCompiler(VitaA32::CodeBuffer& code)
		: m_code(code)
	{
	}

	bool BlockCompiler::CanCompileOpcode(u32 op)
	{
		switch (op >> 26)
		{
			case 0x00:
				return CanCompileSPECIAL(op);
			case 0x09: // ADDIU, owned by R5900OpcodeImpl.cpp::ADDIU().
			case 0x0a: // SLTI, owned by R5900OpcodeImpl.cpp::SLTI().
			case 0x0b: // SLTIU, owned by R5900OpcodeImpl.cpp::SLTIU().
			case 0x0c: // ANDI, owned by R5900OpcodeImpl.cpp::ANDI().
			case 0x0d: // ORI, owned by R5900OpcodeImpl.cpp::ORI().
			case 0x0e: // XORI, owned by R5900OpcodeImpl.cpp::XORI().
			case 0x0f: // LUI, owned by R5900OpcodeImpl.cpp::LUI().
			case 0x19: // DADDIU, owned by R5900OpcodeImpl.cpp::DADDIU().
				return true;
			default:
				return false;
		}
	}

	bool BlockCompiler::BeginBlock()
	{
		return m_code.EmitPush(REG_R4 | REG_LR) &&
			   m_code.EmitMovImm32(HOST_CPU_REGS, static_cast<u32>(reinterpret_cast<uptr>(&cpuRegs)));
	}

	bool BlockCompiler::CompileStraightLineBlock(u32 start_pc, u32 instruction_count, const void* direct_exit,
		const void* event_exit, u32* scaled_cycles)
	{
		if (instruction_count == 0 || instruction_count > ((UINT32_MAX - start_pc) / 4))
			return false;

		if (!BeginBlock())
			return false;

		u32 raw_cycles = 0;
		for (u32 i = 0; i < instruction_count; i++)
		{
			const u32 pc = start_pc + i * 4;
			const u32 op = memRead32(pc);
			if (!CanCompileOpcode(op))
				return false;

			// PCSX2's x86 recRecompile() gives NOP a fixed 9-cycle raw cost before
			// scaling; all other op costs come from the R5900 opcode table.
			if (op == 0)
				raw_cycles += 9 * (2 - ((cpuRegs.CP0.n.Config >> 18) & 0x1));
			else
				raw_cycles += R5900::GetInstruction(op).cycles * (2 - ((cpuRegs.CP0.n.Config >> 18) & 0x1));

			if (!EmitOpcode(op))
				return false;
		}

		const u32 next_pc = start_pc + instruction_count * 4;
		const u32 block_cycles = ScaleBlockCycles(raw_cycles);
		if (scaled_cycles)
			*scaled_cycles = block_cycles;

		// Matches the fall-through writeback in x86/ix86-32/iR5900.cpp::recRecompile()
		// after compiling a non-branching block.
		return EmitStorePc(next_pc) &&
			   EndBlockWithCycleTest(block_cycles, direct_exit, event_exit);
	}

	bool BlockCompiler::EmitOpcode(u32 op)
	{
		switch (op >> 26)
		{
			case 0x00:
				return EmitSPECIAL(op);
			case 0x09: // ADDIU, owned by R5900OpcodeImpl.cpp::ADDIU().
				return EmitADDIU(op);
			case 0x0a: // SLTI, owned by R5900OpcodeImpl.cpp::SLTI().
				return EmitSLTI(op);
			case 0x0b: // SLTIU, owned by R5900OpcodeImpl.cpp::SLTIU().
				return EmitSLTIU(op);
			case 0x0c: // ANDI, owned by R5900OpcodeImpl.cpp::ANDI().
				return EmitANDI(op);
			case 0x0d: // ORI, owned by R5900OpcodeImpl.cpp::ORI().
				return EmitORI(op);
			case 0x0e: // XORI, owned by R5900OpcodeImpl.cpp::XORI().
				return EmitXORI(op);
			case 0x0f: // LUI, owned by R5900OpcodeImpl.cpp::LUI().
				return EmitLUI(op);
			case 0x19: // DADDIU, owned by R5900OpcodeImpl.cpp::DADDIU().
				return EmitDADDIU(op);
			default:
				return false;
		}
	}

	bool BlockCompiler::EndBlockReturn(u8 value)
	{
		return m_code.EmitMovImm8(0, value) &&
			   m_code.EmitPop(REG_R4 | REG_PC);
	}

	bool BlockCompiler::EndBlockWithCycleTest(u32 block_cycles, const void* direct_exit, const void* event_exit)
	{
		if (!direct_exit || !event_exit)
			return false;

		if (!m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(CYCLE_OFFSET)) ||
			!m_code.EmitLdrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(CYCLE_OFFSET + sizeof(u32))))
		{
			return false;
		}

		if (block_cycles <= 255)
		{
			if (!m_code.EmitAddImm8(HOST_TMP0, HOST_TMP0, static_cast<u8>(block_cycles), true))
				return false;
		}
		else
		{
			if (!m_code.EmitMovImm32(HOST_TMP2, block_cycles) ||
				!m_code.EmitAddReg(HOST_TMP0, HOST_TMP0, HOST_TMP2, true))
			{
				return false;
			}
		}

		// Mirrors PCSX2's normal x86/ix86-32/iR5900.cpp::iBranchTest() path.
		// The signed-negative branch is the direct continuation/link path owned by
		// x86/BaseblockEx.cpp::BaseBlocks::Link() once Vita block linking exists.
		if (!m_code.EmitAdcImm8(HOST_TMP1, HOST_TMP1, 0) ||
			!m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(CYCLE_OFFSET)) ||
			!m_code.EmitStrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(CYCLE_OFFSET + sizeof(u32))) ||
			!m_code.EmitLdrImm12(HOST_TMP2, HOST_CPU_REGS, static_cast<u16>(NEXT_EVENT_OFFSET)) ||
			!m_code.EmitLdrImm12(HOST_TMP3, HOST_CPU_REGS, static_cast<u16>(NEXT_EVENT_OFFSET + sizeof(u32))) ||
			!m_code.EmitSubReg(HOST_TMP2, HOST_TMP0, HOST_TMP2, true) ||
			!m_code.EmitSbcReg(HOST_TMP3, HOST_TMP1, HOST_TMP3, true))
		{
			return false;
		}

		const size_t direct_branch = m_code.EmitBranchPlaceholder(VitaA32::Condition::MI);
		if (direct_branch == static_cast<size_t>(-1))
			return false;

		if (!m_code.EmitCallAbsolute(event_exit) ||
			!m_code.EmitPop(REG_R4 | REG_PC))
		{
			return false;
		}

		const size_t direct_target = m_code.Size();
		return m_code.EmitCallAbsolute(direct_exit) &&
			   m_code.EmitPop(REG_R4 | REG_PC) &&
			   m_code.PatchBranch(direct_branch, direct_target, VitaA32::Condition::MI);
	}

	bool BlockCompiler::EmitSPECIAL(u32 op)
	{
		switch (op & 0x3f)
		{
			case 0x00: // SLL, owned by R5900OpcodeImpl.cpp::SLL().
				return EmitSLL(op);
			case 0x02: // SRL, owned by R5900OpcodeImpl.cpp::SRL().
				return EmitSRL(op);
			case 0x03: // SRA, owned by R5900OpcodeImpl.cpp::SRA().
				return EmitSRA(op);
			case 0x04: // SLLV, owned by R5900OpcodeImpl.cpp::SLLV().
				return EmitSLLV(op);
			case 0x06: // SRLV, owned by R5900OpcodeImpl.cpp::SRLV().
				return EmitSRLV(op);
			case 0x07: // SRAV, owned by R5900OpcodeImpl.cpp::SRAV().
				return EmitSRAV(op);
			case 0x21: // ADDU, owned by R5900OpcodeImpl.cpp::ADDU().
				return EmitADDU(op);
			case 0x23: // SUBU, owned by R5900OpcodeImpl.cpp::SUBU().
				return EmitSUBU(op);
			case 0x24: // AND, owned by R5900OpcodeImpl.cpp::AND().
				return EmitAND(op);
			case 0x25: // OR, owned by R5900OpcodeImpl.cpp::OR().
				return EmitOR(op);
			case 0x26: // XOR, owned by R5900OpcodeImpl.cpp::XOR().
				return EmitXOR(op);
			case 0x27: // NOR, owned by R5900OpcodeImpl.cpp::NOR().
				return EmitNOR(op);
			case 0x2a: // SLT, owned by R5900OpcodeImpl.cpp::SLT().
				return EmitSLT(op);
			case 0x2b: // SLTU, owned by R5900OpcodeImpl.cpp::SLTU().
				return EmitSLTU(op);
			case 0x2d: // DADDU, owned by R5900OpcodeImpl.cpp::DADDU().
				return EmitDADDU(op);
			case 0x2f: // DSUBU, owned by R5900OpcodeImpl.cpp::DSUBU().
				return EmitDSUBU(op);
			case 0x38: // DSLL, owned by R5900OpcodeImpl.cpp::DSLL().
				return EmitDSLL(op);
			case 0x3a: // DSRL, owned by R5900OpcodeImpl.cpp::DSRL().
				return EmitDSRL(op);
			case 0x3b: // DSRA, owned by R5900OpcodeImpl.cpp::DSRA().
				return EmitDSRA(op);
			case 0x3c: // DSLL32, owned by R5900OpcodeImpl.cpp::DSLL32().
				return EmitDSLL32(op);
			case 0x3e: // DSRL32, owned by R5900OpcodeImpl.cpp::DSRL32().
				return EmitDSRL32(op);
			case 0x3f: // DSRA32, owned by R5900OpcodeImpl.cpp::DSRA32().
				return EmitDSRA32(op);
			default:
				return false;
		}
	}

	bool BlockCompiler::EmitADDIU(u32 op)
	{
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		const s32 imm = static_cast<s32>(IMM_S(op));

		if (rt == 0)
			return true;

		if (!EmitLoadGprLow(rs, HOST_TMP0))
			return false;

		if (imm >= 0 && imm <= 255)
		{
			if (!m_code.EmitAddImm8(HOST_TMP0, HOST_TMP0, static_cast<u8>(imm)))
				return false;
		}
		else if (imm < 0 && imm >= -255)
		{
			if (!m_code.EmitSubImm8(HOST_TMP0, HOST_TMP0, static_cast<u8>(-imm)))
				return false;
		}
		else
		{
			if (!m_code.EmitMovImm32(HOST_TMP2, static_cast<u32>(imm)) ||
				!m_code.EmitAddReg(HOST_TMP0, HOST_TMP0, HOST_TMP2))
			{
				return false;
			}
		}

		return m_code.EmitMovRegShiftImm(HOST_TMP1, HOST_TMP0, VitaA32::ShiftType::ASR, 31) &&
			   EmitStoreGpr64(rt, HOST_TMP0, HOST_TMP1);
	}

	bool BlockCompiler::EmitDADDIU(u32 op)
	{
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		const s32 imm = static_cast<s32>(IMM_S(op));

		if (rt == 0)
			return true;

		return EmitLoadGpr64(rs, HOST_TMP0, HOST_TMP1) &&
			   m_code.EmitMovImm32(HOST_TMP2, static_cast<u32>(imm)) &&
			   m_code.EmitAddReg(HOST_TMP0, HOST_TMP0, HOST_TMP2, true) &&
			   m_code.EmitMovImm32(HOST_TMP2, (imm < 0) ? 0xffffffffu : 0) &&
			   m_code.EmitAdcReg(HOST_TMP1, HOST_TMP1, HOST_TMP2) &&
			   EmitStoreGpr64(rt, HOST_TMP0, HOST_TMP1);
	}

	bool BlockCompiler::EmitSLTI(u32 op)
	{
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		const s32 imm = static_cast<s32>(IMM_S(op));

		if (rt == 0)
			return true;

		return EmitLoadGpr64(rs, HOST_TMP0, HOST_TMP1) &&
			   m_code.EmitMovImm32(HOST_TMP2, static_cast<u32>(imm)) &&
			   m_code.EmitMovImm32(HOST_TMP3, (imm < 0) ? 0xffffffffu : 0) &&
			   EmitSetLessThan64(rt, true);
	}

	bool BlockCompiler::EmitSLTIU(u32 op)
	{
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		const s32 imm = static_cast<s32>(IMM_S(op));

		if (rt == 0)
			return true;

		return EmitLoadGpr64(rs, HOST_TMP0, HOST_TMP1) &&
			   m_code.EmitMovImm32(HOST_TMP2, static_cast<u32>(imm)) &&
			   m_code.EmitMovImm32(HOST_TMP3, (imm < 0) ? 0xffffffffu : 0) &&
			   EmitSetLessThan64(rt, false);
	}

	bool BlockCompiler::EmitANDI(u32 op)
	{
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		const u16 imm = IMM_U(op);

		if (rt == 0)
			return true;

		if (!EmitLoadGprLow(rs, HOST_TMP0))
			return false;

		if (imm <= 255)
		{
			if (!m_code.EmitAndImm8(HOST_TMP0, HOST_TMP0, static_cast<u8>(imm)))
				return false;
		}
		else
		{
			if (!m_code.EmitMovImm32(HOST_TMP2, imm) ||
				!m_code.EmitAndReg(HOST_TMP0, HOST_TMP0, HOST_TMP2))
			{
				return false;
			}
		}

		return m_code.EmitMovImm8(HOST_TMP1, 0) &&
			   EmitStoreGpr64(rt, HOST_TMP0, HOST_TMP1);
	}

	bool BlockCompiler::EmitORI(u32 op)
	{
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		const u16 imm = IMM_U(op);

		if (rt == 0)
			return true;

		if (!EmitLoadGpr64(rs, HOST_TMP0, HOST_TMP1))
			return false;

		if (imm <= 255)
		{
			if (!m_code.EmitOrrImm8(HOST_TMP0, HOST_TMP0, static_cast<u8>(imm)))
				return false;
		}
		else
		{
			if (!m_code.EmitMovImm32(HOST_TMP2, imm) ||
				!m_code.EmitOrrReg(HOST_TMP0, HOST_TMP0, HOST_TMP2))
			{
				return false;
			}
		}

		return EmitStoreGpr64(rt, HOST_TMP0, HOST_TMP1);
	}

	bool BlockCompiler::EmitXORI(u32 op)
	{
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		const u16 imm = IMM_U(op);

		if (rt == 0)
			return true;

		if (!EmitLoadGpr64(rs, HOST_TMP0, HOST_TMP1))
			return false;

		if (imm <= 255)
		{
			if (!m_code.EmitEorImm8(HOST_TMP0, HOST_TMP0, static_cast<u8>(imm)))
				return false;
		}
		else
		{
			if (!m_code.EmitMovImm32(HOST_TMP2, imm) ||
				!m_code.EmitEorReg(HOST_TMP0, HOST_TMP0, HOST_TMP2))
			{
				return false;
			}
		}

		return EmitStoreGpr64(rt, HOST_TMP0, HOST_TMP1);
	}

	bool BlockCompiler::EmitLUI(u32 op)
	{
		const unsigned rt = RT(op);
		if (rt == 0)
			return true;

		const u32 value = op << 16;
		return m_code.EmitMovImm32(HOST_TMP0, value) &&
			   m_code.EmitMovRegShiftImm(HOST_TMP1, HOST_TMP0, VitaA32::ShiftType::ASR, 31) &&
			   EmitStoreGpr64(rt, HOST_TMP0, HOST_TMP1);
	}

	bool BlockCompiler::EmitSLL(u32 op)
	{
		return EmitShift32Immediate(op, VitaA32::ShiftType::LSL);
	}

	bool BlockCompiler::EmitSRL(u32 op)
	{
		return EmitShift32Immediate(op, VitaA32::ShiftType::LSR);
	}

	bool BlockCompiler::EmitSRA(u32 op)
	{
		return EmitShift32Immediate(op, VitaA32::ShiftType::ASR);
	}

	bool BlockCompiler::EmitSLLV(u32 op)
	{
		return EmitShift32Variable(op, VitaA32::ShiftType::LSL);
	}

	bool BlockCompiler::EmitSRLV(u32 op)
	{
		return EmitShift32Variable(op, VitaA32::ShiftType::LSR);
	}

	bool BlockCompiler::EmitSRAV(u32 op)
	{
		return EmitShift32Variable(op, VitaA32::ShiftType::ASR);
	}

	bool BlockCompiler::EmitDSLL(u32 op)
	{
		return EmitShift64LeftImmediate(op, SA(op));
	}

	bool BlockCompiler::EmitDSRL(u32 op)
	{
		return EmitShift64RightImmediate(op, SA(op), false);
	}

	bool BlockCompiler::EmitDSRA(u32 op)
	{
		return EmitShift64RightImmediate(op, SA(op), true);
	}

	bool BlockCompiler::EmitDSLL32(u32 op)
	{
		return EmitShift64LeftImmediate(op, SA(op) + 32);
	}

	bool BlockCompiler::EmitDSRL32(u32 op)
	{
		return EmitShift64RightImmediate(op, SA(op) + 32, false);
	}

	bool BlockCompiler::EmitDSRA32(u32 op)
	{
		return EmitShift64RightImmediate(op, SA(op) + 32, true);
	}

	bool BlockCompiler::EmitADDU(u32 op)
	{
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		const unsigned rd = RD(op);

		if (rd == 0)
			return true;

		return EmitLoadGprLow(rs, HOST_TMP0) &&
			   EmitLoadGprLow(rt, HOST_TMP1) &&
			   m_code.EmitAddReg(HOST_TMP0, HOST_TMP0, HOST_TMP1) &&
			   m_code.EmitMovRegShiftImm(HOST_TMP1, HOST_TMP0, VitaA32::ShiftType::ASR, 31) &&
			   EmitStoreGpr64(rd, HOST_TMP0, HOST_TMP1);
	}

	bool BlockCompiler::EmitSUBU(u32 op)
	{
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		const unsigned rd = RD(op);

		if (rd == 0)
			return true;

		return EmitLoadGprLow(rs, HOST_TMP0) &&
			   EmitLoadGprLow(rt, HOST_TMP1) &&
			   m_code.EmitSubReg(HOST_TMP0, HOST_TMP0, HOST_TMP1) &&
			   m_code.EmitMovRegShiftImm(HOST_TMP1, HOST_TMP0, VitaA32::ShiftType::ASR, 31) &&
			   EmitStoreGpr64(rd, HOST_TMP0, HOST_TMP1);
	}

	bool BlockCompiler::EmitDADDU(u32 op)
	{
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		const unsigned rd = RD(op);

		if (rd == 0)
			return true;

		return EmitLoadGpr64(rs, HOST_TMP0, HOST_TMP1) &&
			   EmitLoadGpr64(rt, HOST_TMP2, HOST_TMP3) &&
			   m_code.EmitAddReg(HOST_TMP0, HOST_TMP0, HOST_TMP2, true) &&
			   m_code.EmitAdcReg(HOST_TMP1, HOST_TMP1, HOST_TMP3) &&
			   EmitStoreGpr64(rd, HOST_TMP0, HOST_TMP1);
	}

	bool BlockCompiler::EmitDSUBU(u32 op)
	{
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		const unsigned rd = RD(op);

		if (rd == 0)
			return true;

		return EmitLoadGpr64(rs, HOST_TMP0, HOST_TMP1) &&
			   EmitLoadGpr64(rt, HOST_TMP2, HOST_TMP3) &&
			   m_code.EmitSubReg(HOST_TMP0, HOST_TMP0, HOST_TMP2, true) &&
			   m_code.EmitSbcReg(HOST_TMP1, HOST_TMP1, HOST_TMP3) &&
			   EmitStoreGpr64(rd, HOST_TMP0, HOST_TMP1);
	}

	bool BlockCompiler::EmitAND(u32 op)
	{
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		const unsigned rd = RD(op);

		if (rd == 0)
			return true;

		return EmitLoadGpr64(rs, HOST_TMP0, HOST_TMP1) &&
			   EmitLoadGpr64(rt, HOST_TMP2, HOST_TMP3) &&
			   m_code.EmitAndReg(HOST_TMP0, HOST_TMP0, HOST_TMP2) &&
			   m_code.EmitAndReg(HOST_TMP1, HOST_TMP1, HOST_TMP3) &&
			   EmitStoreGpr64(rd, HOST_TMP0, HOST_TMP1);
	}

	bool BlockCompiler::EmitOR(u32 op)
	{
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		const unsigned rd = RD(op);

		if (rd == 0)
			return true;

		return EmitLoadGpr64(rs, HOST_TMP0, HOST_TMP1) &&
			   EmitLoadGpr64(rt, HOST_TMP2, HOST_TMP3) &&
			   m_code.EmitOrrReg(HOST_TMP0, HOST_TMP0, HOST_TMP2) &&
			   m_code.EmitOrrReg(HOST_TMP1, HOST_TMP1, HOST_TMP3) &&
			   EmitStoreGpr64(rd, HOST_TMP0, HOST_TMP1);
	}

	bool BlockCompiler::EmitXOR(u32 op)
	{
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		const unsigned rd = RD(op);

		if (rd == 0)
			return true;

		return EmitLoadGpr64(rs, HOST_TMP0, HOST_TMP1) &&
			   EmitLoadGpr64(rt, HOST_TMP2, HOST_TMP3) &&
			   m_code.EmitEorReg(HOST_TMP0, HOST_TMP0, HOST_TMP2) &&
			   m_code.EmitEorReg(HOST_TMP1, HOST_TMP1, HOST_TMP3) &&
			   EmitStoreGpr64(rd, HOST_TMP0, HOST_TMP1);
	}

	bool BlockCompiler::EmitNOR(u32 op)
	{
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		const unsigned rd = RD(op);

		if (rd == 0)
			return true;

		return EmitLoadGpr64(rs, HOST_TMP0, HOST_TMP1) &&
			   EmitLoadGpr64(rt, HOST_TMP2, HOST_TMP3) &&
			   m_code.EmitOrrReg(HOST_TMP0, HOST_TMP0, HOST_TMP2) &&
			   m_code.EmitOrrReg(HOST_TMP1, HOST_TMP1, HOST_TMP3) &&
			   m_code.EmitMvnReg(HOST_TMP0, HOST_TMP0) &&
			   m_code.EmitMvnReg(HOST_TMP1, HOST_TMP1) &&
			   EmitStoreGpr64(rd, HOST_TMP0, HOST_TMP1);
	}

	bool BlockCompiler::EmitSLT(u32 op)
	{
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		const unsigned rd = RD(op);

		if (rd == 0)
			return true;

		return EmitLoadGpr64(rs, HOST_TMP0, HOST_TMP1) &&
			   EmitLoadGpr64(rt, HOST_TMP2, HOST_TMP3) &&
			   EmitSetLessThan64(rd, true);
	}

	bool BlockCompiler::EmitSLTU(u32 op)
	{
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		const unsigned rd = RD(op);

		if (rd == 0)
			return true;

		return EmitLoadGpr64(rs, HOST_TMP0, HOST_TMP1) &&
			   EmitLoadGpr64(rt, HOST_TMP2, HOST_TMP3) &&
			   EmitSetLessThan64(rd, false);
	}

	bool BlockCompiler::EmitShift32Immediate(u32 op, VitaA32::ShiftType shift)
	{
		const unsigned rt = RT(op);
		const unsigned rd = RD(op);
		const unsigned sa = SA(op);

		if (rd == 0)
			return true;

		if (!EmitLoadGprLow(rt, HOST_TMP0))
			return false;

		// ARM immediate LSR/ASR with amount 0 encodes a shift of 32, while
		// R5900 SRL/SRA with sa=0 is a no-op on the low word.
		if (sa == 0)
		{
			if (!m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_TMP0, VitaA32::ShiftType::LSL, 0))
				return false;
		}
		else if (!m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_TMP0, shift, static_cast<u8>(sa)))
		{
			return false;
		}

		return m_code.EmitMovRegShiftImm(HOST_TMP1, HOST_TMP0, VitaA32::ShiftType::ASR, 31) &&
			   EmitStoreGpr64(rd, HOST_TMP0, HOST_TMP1);
	}

	bool BlockCompiler::EmitShift32Variable(u32 op, VitaA32::ShiftType shift)
	{
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		const unsigned rd = RD(op);

		if (rd == 0)
			return true;

		return EmitLoadGprLow(rt, HOST_TMP0) &&
			   EmitLoadGprLow(rs, HOST_TMP2) &&
			   m_code.EmitAndImm8(HOST_TMP2, HOST_TMP2, 0x1f) &&
			   m_code.EmitMovRegShiftReg(HOST_TMP0, HOST_TMP0, shift, HOST_TMP2) &&
			   m_code.EmitMovRegShiftImm(HOST_TMP1, HOST_TMP0, VitaA32::ShiftType::ASR, 31) &&
			   EmitStoreGpr64(rd, HOST_TMP0, HOST_TMP1);
	}

	bool BlockCompiler::EmitShift64LeftImmediate(u32 op, unsigned amount)
	{
		const unsigned rt = RT(op);
		const unsigned rd = RD(op);

		if (rd == 0)
			return true;

		if (!EmitLoadGpr64(rt, HOST_TMP0, HOST_TMP1))
			return false;

		if (amount == 0)
		{
			return EmitStoreGpr64(rd, HOST_TMP0, HOST_TMP1);
		}
		else if (amount < 32)
		{
			return m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP0, VitaA32::ShiftType::LSR, static_cast<u8>(32 - amount)) &&
				   m_code.EmitMovRegShiftImm(HOST_TMP1, HOST_TMP1, VitaA32::ShiftType::LSL, static_cast<u8>(amount)) &&
				   m_code.EmitOrrReg(HOST_TMP1, HOST_TMP1, HOST_TMP2) &&
				   m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_TMP0, VitaA32::ShiftType::LSL, static_cast<u8>(amount)) &&
				   EmitStoreGpr64(rd, HOST_TMP0, HOST_TMP1);
		}
		else if (amount == 32)
		{
			return m_code.EmitMovRegShiftImm(HOST_TMP1, HOST_TMP0, VitaA32::ShiftType::LSL, 0) &&
				   m_code.EmitMovImm8(HOST_TMP0, 0) &&
				   EmitStoreGpr64(rd, HOST_TMP0, HOST_TMP1);
		}

		return m_code.EmitMovRegShiftImm(HOST_TMP1, HOST_TMP0, VitaA32::ShiftType::LSL, static_cast<u8>(amount - 32)) &&
			   m_code.EmitMovImm8(HOST_TMP0, 0) &&
			   EmitStoreGpr64(rd, HOST_TMP0, HOST_TMP1);
	}

	bool BlockCompiler::EmitShift64RightImmediate(u32 op, unsigned amount, bool arithmetic)
	{
		const unsigned rt = RT(op);
		const unsigned rd = RD(op);

		if (rd == 0)
			return true;

		if (!EmitLoadGpr64(rt, HOST_TMP0, HOST_TMP1))
			return false;

		const VitaA32::ShiftType high_shift = arithmetic ? VitaA32::ShiftType::ASR : VitaA32::ShiftType::LSR;
		if (amount == 0)
		{
			return EmitStoreGpr64(rd, HOST_TMP0, HOST_TMP1);
		}
		else if (amount < 32)
		{
			return m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP1, VitaA32::ShiftType::LSL, static_cast<u8>(32 - amount)) &&
				   m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_TMP0, VitaA32::ShiftType::LSR, static_cast<u8>(amount)) &&
				   m_code.EmitOrrReg(HOST_TMP0, HOST_TMP0, HOST_TMP2) &&
				   m_code.EmitMovRegShiftImm(HOST_TMP1, HOST_TMP1, high_shift, static_cast<u8>(amount)) &&
				   EmitStoreGpr64(rd, HOST_TMP0, HOST_TMP1);
		}
		else if (amount == 32)
		{
			return m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_TMP1, VitaA32::ShiftType::LSL, 0) &&
				   (arithmetic ?
					   m_code.EmitMovRegShiftImm(HOST_TMP1, HOST_TMP1, VitaA32::ShiftType::ASR, 31) :
					   m_code.EmitMovImm8(HOST_TMP1, 0)) &&
				   EmitStoreGpr64(rd, HOST_TMP0, HOST_TMP1);
		}

		return m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_TMP1, high_shift, static_cast<u8>(amount - 32)) &&
			   (arithmetic ?
				   m_code.EmitMovRegShiftImm(HOST_TMP1, HOST_TMP1, VitaA32::ShiftType::ASR, 31) :
				   m_code.EmitMovImm8(HOST_TMP1, 0)) &&
			   EmitStoreGpr64(rd, HOST_TMP0, HOST_TMP1);
	}

	bool BlockCompiler::EmitSetLessThan64(unsigned guest_reg, bool signed_compare)
	{
		if (guest_reg == 0)
			return true;

		if (!m_code.EmitMovImm8(HOST_TMP4, 0) ||
			!m_code.EmitCmpReg(HOST_TMP1, HOST_TMP3))
		{
			return false;
		}

		const size_t high_equal = m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
		if (high_equal == static_cast<size_t>(-1))
			return false;

		if (!m_code.EmitMovImm8(HOST_TMP4, 1, signed_compare ? VitaA32::Condition::LT : VitaA32::Condition::CC))
			return false;

		const size_t done = m_code.EmitBranchPlaceholder();
		if (done == static_cast<size_t>(-1))
			return false;

		const size_t low_compare = m_code.Size();
		if (!m_code.EmitCmpReg(HOST_TMP0, HOST_TMP2) ||
			!m_code.EmitMovImm8(HOST_TMP4, 1, VitaA32::Condition::CC))
		{
			return false;
		}

		const size_t done_target = m_code.Size();
		return m_code.PatchBranch(high_equal, low_compare, VitaA32::Condition::EQ) &&
			   m_code.PatchBranch(done, done_target) &&
			   m_code.EmitMovImm8(HOST_TMP1, 0) &&
			   EmitStoreGpr64(guest_reg, HOST_TMP4, HOST_TMP1);
	}

	bool BlockCompiler::EmitLoadGprLow(unsigned guest_reg, unsigned host_reg)
	{
		if (guest_reg == 0)
			return m_code.EmitMovImm8(host_reg, 0);

		return m_code.EmitLdrImm12(host_reg, HOST_CPU_REGS, static_cast<u16>(GprOffset(guest_reg)));
	}

	bool BlockCompiler::EmitLoadGpr64(unsigned guest_reg, unsigned host_low, unsigned host_high)
	{
		if (guest_reg == 0)
			return m_code.EmitMovImm8(host_low, 0) &&
				   m_code.EmitMovImm8(host_high, 0);

		const size_t offset = GprOffset(guest_reg);
		return m_code.EmitLdrImm12(host_low, HOST_CPU_REGS, static_cast<u16>(offset)) &&
			   m_code.EmitLdrImm12(host_high, HOST_CPU_REGS, static_cast<u16>(offset + sizeof(u32)));
	}

	bool BlockCompiler::EmitStorePc(u32 pc)
	{
		return m_code.EmitMovImm32(HOST_TMP0, pc) &&
			   m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(PC_OFFSET));
	}

	bool BlockCompiler::EmitStoreGpr64(unsigned guest_reg, unsigned host_low, unsigned host_high)
	{
		if (guest_reg == 0)
			return true;

		const size_t offset = GprOffset(guest_reg);
		return m_code.EmitStrImm12(host_low, HOST_CPU_REGS, static_cast<u16>(offset)) &&
			   m_code.EmitStrImm12(host_high, HOST_CPU_REGS, static_cast<u16>(offset + sizeof(u32)));
	}
} // namespace VitaEE
