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
				case 0x21: // ADDU, owned by R5900OpcodeImpl.cpp::ADDU().
				case 0x23: // SUBU, owned by R5900OpcodeImpl.cpp::SUBU().
				case 0x24: // AND, owned by R5900OpcodeImpl.cpp::AND().
				case 0x25: // OR, owned by R5900OpcodeImpl.cpp::OR().
				case 0x26: // XOR, owned by R5900OpcodeImpl.cpp::XOR().
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
			case 0x0c: // ANDI, owned by R5900OpcodeImpl.cpp::ANDI().
			case 0x0d: // ORI, owned by R5900OpcodeImpl.cpp::ORI().
			case 0x0e: // XORI, owned by R5900OpcodeImpl.cpp::XORI().
			case 0x0f: // LUI, owned by R5900OpcodeImpl.cpp::LUI().
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
			case 0x0c: // ANDI, owned by R5900OpcodeImpl.cpp::ANDI().
				return EmitANDI(op);
			case 0x0d: // ORI, owned by R5900OpcodeImpl.cpp::ORI().
				return EmitORI(op);
			case 0x0e: // XORI, owned by R5900OpcodeImpl.cpp::XORI().
				return EmitXORI(op);
			case 0x0f: // LUI, owned by R5900OpcodeImpl.cpp::LUI().
				return EmitLUI(op);
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
		const unsigned rt = RT(op);
		const unsigned rd = RD(op);
		const unsigned sa = SA(op);

		if (rd == 0)
			return true;

		return EmitLoadGprLow(rt, HOST_TMP0) &&
			   m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_TMP0, VitaA32::ShiftType::LSL, static_cast<u8>(sa)) &&
			   m_code.EmitMovRegShiftImm(HOST_TMP1, HOST_TMP0, VitaA32::ShiftType::ASR, 31) &&
			   EmitStoreGpr64(rd, HOST_TMP0, HOST_TMP1);
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
