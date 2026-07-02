// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "pcsx2/vita/VitaEeBlockCompiler.h"

#include "pcsx2/Config.h"
#include "pcsx2/Memory.h"
#include "pcsx2/R5900.h"
#include "pcsx2/R5900OpcodeTables.h"
#include "pcsx2/vita/A32Emitter.h"
#include "pcsx2/vita/VitaCore.h"
#include "pcsx2/vtlb.h"

#include "common/Console.h"
#include "fmt/format.h"

#include <cstddef>
#include <string>

namespace VitaEE
{
	namespace
	{
		constexpr u16 REG_R4 = 1u << 4;
		constexpr u16 REG_R5 = 1u << 5;
		constexpr u16 REG_LR = 1u << 14;
		constexpr u16 REG_PC = 1u << 15;

		constexpr unsigned HOST_CPU_REGS = 4;
		constexpr unsigned HOST_BRANCH_STATE = 5;
		constexpr unsigned HOST_BRANCH_FLAG = HOST_BRANCH_STATE;
		constexpr unsigned HOST_BRANCH_TARGET = HOST_BRANCH_STATE;
		constexpr unsigned HOST_TMP0 = 0;
		constexpr unsigned HOST_TMP1 = 1;
		constexpr unsigned HOST_TMP2 = 2;
		constexpr unsigned HOST_TMP3 = 3;
		constexpr unsigned HOST_TMP4 = 12;

		constexpr size_t GPR_OFFSET = offsetof(cpuRegisters, GPR);
		constexpr size_t PC_OFFSET = offsetof(cpuRegisters, pc);
		constexpr size_t CYCLE_OFFSET = offsetof(cpuRegisters, cycle);
		constexpr size_t NEXT_EVENT_OFFSET = offsetof(cpuRegisters, nextEventCycle);

		constexpr u32 GOEMON_PRELOAD_RETURN_PC_0 = 0x0033ad48;
		constexpr u32 GOEMON_PRELOAD_RETURN_PC_1 = 0x0035060c;
		constexpr u32 GOEMON_UNLOAD_ENTRY_PC = 0x003563b8;

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

		constexpr u32 INSTRUC_TARGET(u32 op)
		{
			return op & 0x03ffffffu;
		}

		constexpr u32 BranchTarget(u32 pc, u32 op)
		{
			return pc + 4 + static_cast<s32>(IMM_S(op)) * 4;
		}

		constexpr u32 JumpTarget(u32 pc, u32 op)
		{
			return (INSTRUC_TARGET(op) << 2) | ((pc + 4) & 0xf0000000u);
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
				case 0x08: // JR, owned by Interpreter.cpp::JR().
				case 0x09: // JALR, owned by Interpreter.cpp::JALR().
				case 0x0a: // MOVZ, owned by R5900OpcodeImpl.cpp::MOVZ().
				case 0x0b: // MOVN, owned by R5900OpcodeImpl.cpp::MOVN().
				case 0x14: // DSLLV, owned by R5900OpcodeImpl.cpp::DSLLV().
				case 0x16: // DSRLV, owned by R5900OpcodeImpl.cpp::DSRLV().
				case 0x17: // DSRAV, owned by R5900OpcodeImpl.cpp::DSRAV().
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

		bool CanCompileREGIMM(u32 op)
		{
			switch (RT(op))
			{
				case 0x00: // BLTZ, owned by Interpreter.cpp::BLTZ().
				case 0x01: // BGEZ, owned by Interpreter.cpp::BGEZ().
				case 0x02: // BLTZL, owned by Interpreter.cpp::BLTZL().
				case 0x03: // BGEZL, owned by Interpreter.cpp::BGEZL().
				case 0x10: // BLTZAL, owned by Interpreter.cpp::BLTZAL().
				case 0x11: // BGEZAL, owned by Interpreter.cpp::BGEZAL().
				case 0x12: // BLTZALL, owned by Interpreter.cpp::BLTZALL().
				case 0x13: // BGEZALL, owned by Interpreter.cpp::BGEZALL().
					return true;
				default:
					return false;
			}
		}

		bool IsBranchLikelyOpcode(u32 op)
		{
			switch (op >> 26)
			{
				case 0x01:
					switch (RT(op))
					{
						case 0x02: // BLTZL, owned by Interpreter.cpp::BLTZL().
						case 0x03: // BGEZL, owned by Interpreter.cpp::BGEZL().
						case 0x12: // BLTZALL, owned by Interpreter.cpp::BLTZALL().
						case 0x13: // BGEZALL, owned by Interpreter.cpp::BGEZALL().
							return true;
						default:
							return false;
					}
				case 0x14: // BEQL, owned by Interpreter.cpp::BEQL().
				case 0x15: // BNEL, owned by Interpreter.cpp::BNEL().
				case 0x16: // BLEZL, owned by Interpreter.cpp::BLEZL().
				case 0x17: // BGTZL, owned by Interpreter.cpp::BGTZL().
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

		__noinline void VitaEeRaiseAddressError(u32 addr, bool store)
		{
			// PCSX2 owner: R5900OpcodeImpl.cpp::RaiseAddressError().
			const std::string message(
				fmt::format("Address Error, addr=0x{:x} [{}]", addr, store ? "store" : "load"));
			Console.Error(message);
			Cpu->CancelInstruction();
		}

		__noinline u32 VitaEeMemRead8(u32 addr)
		{
			// PCSX2 owners: R5900OpcodeImpl.cpp::LB() and LBU().
			return memRead8(addr);
		}

		__noinline u32 VitaEeMemRead16Checked(u32 addr)
		{
			// PCSX2 owners: R5900OpcodeImpl.cpp::LH() and LHU().
			if (addr & 1)
				VitaEeRaiseAddressError(addr, false);

			return memRead16(addr);
		}

		__noinline u32 VitaEeMemRead32Checked(u32 addr)
		{
			// PCSX2 owners: R5900OpcodeImpl.cpp::LW() and LWU().
			if (addr & 3)
				VitaEeRaiseAddressError(addr, false);

			return memRead32(addr);
		}

		__noinline void VitaEeMemWrite8(u32 addr, u32 value)
		{
			// PCSX2 owner: R5900OpcodeImpl.cpp::SB().
			memWrite8(addr, static_cast<u8>(value));
		}

		__noinline void VitaEeMemWrite16Checked(u32 addr, u32 value)
		{
			// PCSX2 owner: R5900OpcodeImpl.cpp::SH().
			if (addr & 1)
				VitaEeRaiseAddressError(addr, true);

			memWrite16(addr, static_cast<u16>(value));
		}

		__noinline void VitaEeMemWrite32Checked(u32 addr, u32 value)
		{
			// PCSX2 owner: R5900OpcodeImpl.cpp::SW().
			if (addr & 3)
				VitaEeRaiseAddressError(addr, true);

			memWrite32(addr, value);
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
			case 0x01:
				return CanCompileREGIMM(op);
			case 0x02: // J, owned by Interpreter.cpp::J().
			case 0x03: // JAL, owned by Interpreter.cpp::JAL().
				return true;
			case 0x04: // BEQ, owned by Interpreter.cpp::BEQ().
			case 0x05: // BNE, owned by Interpreter.cpp::BNE().
			case 0x06: // BLEZ, owned by Interpreter.cpp::BLEZ().
			case 0x07: // BGTZ, owned by Interpreter.cpp::BGTZ().
			case 0x14: // BEQL, owned by Interpreter.cpp::BEQL().
			case 0x15: // BNEL, owned by Interpreter.cpp::BNEL().
			case 0x16: // BLEZL, owned by Interpreter.cpp::BLEZL().
			case 0x17: // BGTZL, owned by Interpreter.cpp::BGTZL().
				return true;
			case 0x09: // ADDIU, owned by R5900OpcodeImpl.cpp::ADDIU().
			case 0x0a: // SLTI, owned by R5900OpcodeImpl.cpp::SLTI().
			case 0x0b: // SLTIU, owned by R5900OpcodeImpl.cpp::SLTIU().
			case 0x0c: // ANDI, owned by R5900OpcodeImpl.cpp::ANDI().
			case 0x0d: // ORI, owned by R5900OpcodeImpl.cpp::ORI().
			case 0x0e: // XORI, owned by R5900OpcodeImpl.cpp::XORI().
			case 0x0f: // LUI, owned by R5900OpcodeImpl.cpp::LUI().
			case 0x19: // DADDIU, owned by R5900OpcodeImpl.cpp::DADDIU().
			case 0x20: // LB, owned by R5900OpcodeImpl.cpp::LB().
			case 0x21: // LH, owned by R5900OpcodeImpl.cpp::LH().
			case 0x23: // LW, owned by R5900OpcodeImpl.cpp::LW().
			case 0x24: // LBU, owned by R5900OpcodeImpl.cpp::LBU().
			case 0x25: // LHU, owned by R5900OpcodeImpl.cpp::LHU().
			case 0x27: // LWU, owned by R5900OpcodeImpl.cpp::LWU().
			case 0x28: // SB, owned by R5900OpcodeImpl.cpp::SB().
			case 0x29: // SH, owned by R5900OpcodeImpl.cpp::SH().
			case 0x2b: // SW, owned by R5900OpcodeImpl.cpp::SW().
				return true;
			default:
				return false;
		}
	}

	bool BlockCompiler::IsSupportedBranchOpcode(u32 op)
	{
		switch (op >> 26)
		{
			case 0x00:
				switch (op & 0x3f)
				{
					case 0x08: // JR, owned by Interpreter.cpp::JR().
					case 0x09: // JALR, owned by Interpreter.cpp::JALR().
						return true;
					default:
						return false;
				}
			case 0x01:
				return CanCompileREGIMM(op);
			case 0x02: // J, owned by Interpreter.cpp::J().
			case 0x03: // JAL, owned by Interpreter.cpp::JAL().
				return true;
			case 0x04: // BEQ, owned by Interpreter.cpp::BEQ().
			case 0x05: // BNE, owned by Interpreter.cpp::BNE().
			case 0x06: // BLEZ, owned by Interpreter.cpp::BLEZ().
			case 0x07: // BGTZ, owned by Interpreter.cpp::BGTZ().
			case 0x14: // BEQL, owned by Interpreter.cpp::BEQL().
			case 0x15: // BNEL, owned by Interpreter.cpp::BNEL().
			case 0x16: // BLEZL, owned by Interpreter.cpp::BLEZL().
			case 0x17: // BGTZL, owned by Interpreter.cpp::BGTZL().
				return true;
			default:
				return false;
		}
	}

	bool BlockCompiler::CanCompileDelaySlotOpcode(u32 op)
	{
		// PCSX2 x86/ix86-32/iR5900.cpp::recRecompile() detects branches in
		// delay slots through recompileNextInstruction(true, ...): the delay
		// branch is skipped as generated work and the outer branch still owns
		// the block exit.
		return CanCompileOpcode(op) && !RequiresBlockEndAfterOpcode(op);
	}

	bool BlockCompiler::RequiresBlockEndAfterOpcode(u32 op)
	{
		// PCSX2 owners: R5900OpcodeImpl.cpp::LB()/LBU()/LH()/LHU()/LW()
		// force intUpdateCPUCycles() and intEventTest() for EE counter reads.
		// Ending the Vita block after these loads keeps trace-window recording
		// aligned with executed instructions.
		switch (op >> 26)
		{
			case 0x20:
			case 0x21:
			case 0x23:
			case 0x24:
			case 0x25:
				return true;
			default:
				return false;
		}
	}

	bool BlockCompiler::BeginBlock()
	{
		return m_code.EmitPush(REG_R4 | REG_R5 | REG_LR) &&
			   m_code.EmitMovImm32(HOST_CPU_REGS, static_cast<u32>(reinterpret_cast<uptr>(&cpuRegs)));
	}

	bool BlockCompiler::CompileStraightLineBlock(u32 start_pc, u32 instruction_count, const void* direct_exit,
		const void* event_exit, u32* scaled_cycles, DirectLinkSlots* direct_links)
	{
		if (instruction_count == 0 || instruction_count > ((UINT32_MAX - start_pc) / 4))
			return false;

		if (direct_links)
			*direct_links = {};

		if (!BeginBlock())
			return false;
		if (!EmitGoemonBlockStartHook(start_pc))
			return false;

		u32 raw_cycles = 0;
		bool has_branch = false;
		bool has_register_branch_target = false;
		bool has_static_direct_link_target = false;
		bool has_static_conditional_direct_links = false;
		bool has_static_likely_direct_links = false;
		bool branch_is_likely = false;
		u32 branch_instruction_index = 0;
		u32 branch_target_pc = 0;
		u32 static_direct_link_target_pc = 0;
		u32 branch_likely_not_taken_raw_cycles = 0;
		size_t branch_likely_skip_delay = static_cast<size_t>(-1);
		const auto add_raw_cycles = [&raw_cycles](u32 op) {
			// PCSX2's x86 recRecompile() gives NOP a fixed 9-cycle raw cost before
			// scaling; all other op costs come from the R5900 opcode table.
			if (op == 0)
				raw_cycles += 9 * (2 - ((cpuRegs.CP0.n.Config >> 18) & 0x1));
			else
				raw_cycles += R5900::GetInstruction(op).cycles * (2 - ((cpuRegs.CP0.n.Config >> 18) & 0x1));
		};

		for (u32 i = 0; i < instruction_count; i++)
		{
			const u32 pc = start_pc + i * 4;
			const u32 op = memRead32(pc);

			if (has_branch && i > branch_instruction_index + 1)
				return false;

			if (IsSupportedBranchOpcode(op))
			{
				if (has_branch && i == branch_instruction_index + 1)
				{
					// PCSX2 owner: x86/ix86-32/iR5900.cpp::recompileNextInstruction()
					// detects a branch while compiling the outer branch delay slot,
					// advances PC past it, and emits no side effects or cycles for
					// the delay-slot branch itself.
					if (branch_is_likely &&
						!m_code.PatchBranch(branch_likely_skip_delay, m_code.Size(), VitaA32::Condition::EQ))
					{
						return false;
					}

					continue;
				}

				if (has_branch || i + 1 >= instruction_count)
					return false;

				const u32 delay_op = memRead32(pc + 4);
				if (!CanCompileDelaySlotOpcode(delay_op))
					return false;

				add_raw_cycles(op);
				branch_instruction_index = i;
				has_branch = true;
				branch_is_likely = IsBranchLikelyOpcode(op);
				if (branch_is_likely)
					branch_likely_not_taken_raw_cycles = raw_cycles;

				switch (op >> 26)
				{
					case 0x00:
						switch (op & 0x3f)
						{
							case 0x08:
								has_register_branch_target = true;
								if (!EmitJR(op, pc))
									return false;
								break;
							case 0x09:
								has_register_branch_target = true;
								if (!EmitJALR(op, pc))
									return false;
								break;
							default:
								return false;
						}
						break;
					case 0x01:
						branch_target_pc = BranchTarget(pc, op);
						if (branch_is_likely)
							has_static_likely_direct_links = true;
						else
							has_static_conditional_direct_links = true;
						if (!EmitREGIMM(op, pc))
							return false;
						break;
					case 0x02:
						branch_target_pc = JumpTarget(pc, op);
						if (EmuConfig.Gamefixes.GoemonTlbHack)
							branch_target_pc = vtlb_V2P(branch_target_pc);
						has_static_direct_link_target = true;
						static_direct_link_target_pc = branch_target_pc;
						if (!EmitJ(op, pc))
							return false;
						break;
					case 0x03:
						branch_target_pc = JumpTarget(pc, op);
						if (EmuConfig.Gamefixes.GoemonTlbHack)
							branch_target_pc = vtlb_V2P(branch_target_pc);
						has_static_direct_link_target = true;
						static_direct_link_target_pc = branch_target_pc;
						if (!EmitJAL(op, pc))
							return false;
						break;
					case 0x04:
						branch_target_pc = BranchTarget(pc, op);
						has_static_conditional_direct_links = true;
						if (!EmitBEQ(op))
							return false;
						break;
					case 0x05:
						branch_target_pc = BranchTarget(pc, op);
						has_static_conditional_direct_links = true;
						if (!EmitBNE(op))
							return false;
						break;
					case 0x06:
						branch_target_pc = BranchTarget(pc, op);
						has_static_conditional_direct_links = true;
						if (!EmitBLEZ(op))
							return false;
						break;
					case 0x07:
						branch_target_pc = BranchTarget(pc, op);
						has_static_conditional_direct_links = true;
						if (!EmitBGTZ(op))
							return false;
						break;
					case 0x14:
						branch_target_pc = BranchTarget(pc, op);
						has_static_likely_direct_links = true;
						if (!EmitBEQL(op))
							return false;
						break;
					case 0x15:
						branch_target_pc = BranchTarget(pc, op);
						has_static_likely_direct_links = true;
						if (!EmitBNEL(op))
							return false;
						break;
					case 0x16:
						branch_target_pc = BranchTarget(pc, op);
						has_static_likely_direct_links = true;
						if (!EmitBLEZL(op))
							return false;
						break;
					case 0x17:
						branch_target_pc = BranchTarget(pc, op);
						has_static_likely_direct_links = true;
						if (!EmitBGTZL(op))
							return false;
						break;
					default:
						return false;
				}

				if (branch_is_likely)
				{
					// PCSX2 owners: Interpreter.cpp::BEQL()/BNEL()/BLEZL()/BGTZL()
					// and REGIMM likely forms cancel the delay slot when the
					// condition is false; x86/ix86-32/iR5900Branch.cpp emits a
					// separate not-taken path without recompileNextInstruction().
					if (!m_code.EmitMovImm8(HOST_TMP0, 0) ||
						!m_code.EmitCmpReg(HOST_BRANCH_FLAG, HOST_TMP0))
					{
						return false;
					}

					branch_likely_skip_delay = m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
					if (branch_likely_skip_delay == static_cast<size_t>(-1))
						return false;
				}

				continue;
			}

			if ((has_branch && i != branch_instruction_index + 1) || !CanCompileOpcode(op))
				return false;
			if (RequiresBlockEndAfterOpcode(op) && i + 1 != instruction_count)
				return false;

			add_raw_cycles(op);
			if (!EmitOpcode(op, pc, raw_cycles, event_exit))
				return false;

			if (has_branch && branch_is_likely && i == branch_instruction_index + 1)
			{
				if (!m_code.PatchBranch(branch_likely_skip_delay, m_code.Size(), VitaA32::Condition::EQ))
					return false;
			}
		}

		const u32 next_pc = start_pc + instruction_count * 4;
		const u32 block_cycles = ScaleBlockCycles(raw_cycles);
		const u32 branch_likely_not_taken_cycles = ScaleBlockCycles(branch_likely_not_taken_raw_cycles);
		if (scaled_cycles)
			*scaled_cycles = block_cycles;

		// Matches the fall-through/branch writeback in x86/ix86-32/iR5900.cpp,
		// after compiling either a non-branching block or a branch plus delay slot.
		if (has_branch)
		{
			if (has_register_branch_target)
			{
				if (!EmitStorePcFromHostReg(HOST_BRANCH_TARGET))
					return false;
			}
			else if (!EmitStoreBranchPc(branch_target_pc, next_pc))
			{
				return false;
			}
		}
		else if (!EmitStorePc(next_pc))
		{
			return false;
		}

		if (has_branch && branch_is_likely)
		{
			size_t not_taken_link_target_offset = 0;
			size_t taken_link_target_offset = 0;
			if (!EndBlockWithLikelyCycleTest(block_cycles, branch_likely_not_taken_cycles, direct_exit, event_exit,
					direct_links && has_static_likely_direct_links ? &not_taken_link_target_offset : nullptr,
					direct_links && has_static_likely_direct_links ? &taken_link_target_offset : nullptr))
			{
				return false;
			}

			if (direct_links && has_static_likely_direct_links)
			{
				direct_links->slots[0].target_pc = next_pc;
				direct_links->slots[0].target_offset = not_taken_link_target_offset;
				direct_links->slots[0].valid = true;

				direct_links->slots[1].target_pc = branch_target_pc;
				direct_links->slots[1].target_offset = taken_link_target_offset;
				direct_links->slots[1].valid = true;
			}

			return true;
		}

		size_t direct_link_target_offset = 0;
		size_t taken_link_target_offset = 0;
		const bool can_direct_link = !has_branch || has_static_direct_link_target ||
									 has_static_conditional_direct_links;
		if (!EndBlockWithCycleTest(block_cycles, direct_exit, event_exit,
				direct_links && can_direct_link ? &direct_link_target_offset : nullptr,
				direct_links && has_static_conditional_direct_links ? &taken_link_target_offset : nullptr))
		{
			return false;
		}

		if (direct_links && can_direct_link)
		{
			direct_links->slots[0].target_pc = has_static_direct_link_target ? static_direct_link_target_pc : next_pc;
			direct_links->slots[0].target_offset = direct_link_target_offset;
			direct_links->slots[0].valid = true;

			if (has_static_conditional_direct_links)
			{
				direct_links->slots[1].target_pc = branch_target_pc;
				direct_links->slots[1].target_offset = taken_link_target_offset;
				direct_links->slots[1].valid = true;
			}
		}

		return true;
	}

	bool BlockCompiler::EmitOpcode(u32 op, u32 pc, u32 raw_cycles_through_instruction, const void* event_exit)
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
			case 0x20: // LB, owned by R5900OpcodeImpl.cpp::LB().
				return EmitLB(op, pc, raw_cycles_through_instruction, event_exit);
			case 0x21: // LH, owned by R5900OpcodeImpl.cpp::LH().
				return EmitLH(op, pc, raw_cycles_through_instruction, event_exit);
			case 0x23: // LW, owned by R5900OpcodeImpl.cpp::LW().
				return EmitLW(op, pc, raw_cycles_through_instruction, event_exit);
			case 0x24: // LBU, owned by R5900OpcodeImpl.cpp::LBU().
				return EmitLBU(op, pc, raw_cycles_through_instruction, event_exit);
			case 0x25: // LHU, owned by R5900OpcodeImpl.cpp::LHU().
				return EmitLHU(op, pc, raw_cycles_through_instruction, event_exit);
			case 0x27: // LWU, owned by R5900OpcodeImpl.cpp::LWU().
				return EmitLWU(op);
			case 0x28: // SB, owned by R5900OpcodeImpl.cpp::SB().
				return EmitSB(op);
			case 0x29: // SH, owned by R5900OpcodeImpl.cpp::SH().
				return EmitSH(op);
			case 0x2b: // SW, owned by R5900OpcodeImpl.cpp::SW().
				return EmitSW(op);
			default:
				return false;
		}
	}

	bool BlockCompiler::EndBlockReturn(u8 value)
	{
		return m_code.EmitMovImm8(0, value) &&
			   m_code.EmitPop(REG_R4 | REG_R5 | REG_PC);
	}

	bool BlockCompiler::EndBlockWithCycleTest(u32 block_cycles, const void* direct_exit, const void* event_exit,
		size_t* direct_link_target_offset, size_t* taken_link_target_offset)
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
			!m_code.EmitPop(REG_R4 | REG_R5 | REG_PC))
		{
			return false;
		}

		const size_t direct_target = m_code.Size();
		if (taken_link_target_offset)
		{
			if (!m_code.EmitMovImm8(HOST_TMP0, 0) ||
				!m_code.EmitCmpReg(HOST_BRANCH_FLAG, HOST_TMP0))
			{
				return false;
			}

			const size_t taken_tail = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (taken_tail == static_cast<size_t>(-1))
				return false;

			if (!m_code.EmitPop(REG_R4 | REG_R5 | REG_LR))
				return false;

			const size_t fallthrough_target_offset = m_code.Size();
			if (!m_code.EmitMovImm32(HOST_TMP4, static_cast<u32>(reinterpret_cast<uptr>(direct_exit))) ||
				!m_code.EmitBx(HOST_TMP4))
			{
				return false;
			}

			const size_t taken_tail_target = m_code.Size();
			if (!m_code.PatchBranch(taken_tail, taken_tail_target, VitaA32::Condition::NE) ||
				!m_code.EmitPop(REG_R4 | REG_R5 | REG_LR))
			{
				return false;
			}

			const size_t taken_target_offset = m_code.Size();
			if (!m_code.EmitMovImm32(HOST_TMP4, static_cast<u32>(reinterpret_cast<uptr>(direct_exit))) ||
				!m_code.EmitBx(HOST_TMP4) ||
				!m_code.PatchBranch(direct_branch, direct_target, VitaA32::Condition::MI))
			{
				return false;
			}

			if (direct_link_target_offset)
				*direct_link_target_offset = fallthrough_target_offset;
			*taken_link_target_offset = taken_target_offset;
			return true;
		}

		if (!m_code.EmitPop(REG_R4 | REG_R5 | REG_LR))
			return false;

		const size_t target_offset = m_code.Size();
		if (!m_code.EmitMovImm32(HOST_TMP4, static_cast<u32>(reinterpret_cast<uptr>(direct_exit))) ||
			!m_code.EmitBx(HOST_TMP4) ||
			!m_code.PatchBranch(direct_branch, direct_target, VitaA32::Condition::MI))
		{
			return false;
		}

		if (direct_link_target_offset)
			*direct_link_target_offset = target_offset;

		return true;
	}

	bool BlockCompiler::EndBlockWithLikelyCycleTest(u32 taken_cycles, u32 not_taken_cycles,
		const void* direct_exit, const void* event_exit, size_t* not_taken_link_target_offset,
		size_t* taken_link_target_offset)
	{
		if (!direct_exit || !event_exit)
			return false;

		if (!m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(CYCLE_OFFSET)) ||
			!m_code.EmitLdrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(CYCLE_OFFSET + sizeof(u32))) ||
			!m_code.EmitMovImm8(HOST_TMP2, 0) ||
			!m_code.EmitCmpReg(HOST_BRANCH_FLAG, HOST_TMP2))
		{
			return false;
		}

		const size_t taken_path = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
		if (taken_path == static_cast<size_t>(-1))
			return false;

		const auto add_cycles = [this](u32 cycles) {
			if (cycles <= 255)
			{
				if (!m_code.EmitAddImm8(HOST_TMP0, HOST_TMP0, static_cast<u8>(cycles), true))
					return false;
			}
			else
			{
				if (!m_code.EmitMovImm32(HOST_TMP2, cycles) ||
					!m_code.EmitAddReg(HOST_TMP0, HOST_TMP0, HOST_TMP2, true))
				{
					return false;
				}
			}

			return m_code.EmitAdcImm8(HOST_TMP1, HOST_TMP1, 0) &&
				   m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(CYCLE_OFFSET)) &&
				   m_code.EmitStrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(CYCLE_OFFSET + sizeof(u32)));
		};

		if (!add_cycles(not_taken_cycles))
			return false;

		const size_t cycles_done = m_code.EmitBranchPlaceholder();
		if (cycles_done == static_cast<size_t>(-1))
			return false;

		const size_t taken_target = m_code.Size();
		if (!m_code.PatchBranch(taken_path, taken_target, VitaA32::Condition::NE) ||
			!add_cycles(taken_cycles))
		{
			return false;
		}

		const size_t cycles_done_target = m_code.Size();
		if (!m_code.PatchBranch(cycles_done, cycles_done_target))
			return false;

		if (!m_code.EmitLdrImm12(HOST_TMP2, HOST_CPU_REGS, static_cast<u16>(NEXT_EVENT_OFFSET)) ||
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
			!m_code.EmitPop(REG_R4 | REG_R5 | REG_PC))
		{
			return false;
		}

		const size_t direct_target = m_code.Size();
		if (not_taken_link_target_offset || taken_link_target_offset)
		{
			if (!m_code.EmitMovImm8(HOST_TMP0, 0) ||
				!m_code.EmitCmpReg(HOST_BRANCH_FLAG, HOST_TMP0))
			{
				return false;
			}

			const size_t taken_tail = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (taken_tail == static_cast<size_t>(-1))
				return false;

			if (!m_code.EmitPop(REG_R4 | REG_R5 | REG_LR))
				return false;

			const size_t not_taken_target_offset = m_code.Size();
			if (!m_code.EmitMovImm32(HOST_TMP4, static_cast<u32>(reinterpret_cast<uptr>(direct_exit))) ||
				!m_code.EmitBx(HOST_TMP4))
			{
				return false;
			}

			const size_t taken_tail_target = m_code.Size();
			if (!m_code.PatchBranch(taken_tail, taken_tail_target, VitaA32::Condition::NE) ||
				!m_code.EmitPop(REG_R4 | REG_R5 | REG_LR))
			{
				return false;
			}

			const size_t taken_target_offset = m_code.Size();
			if (!m_code.EmitMovImm32(HOST_TMP4, static_cast<u32>(reinterpret_cast<uptr>(direct_exit))) ||
				!m_code.EmitBx(HOST_TMP4) ||
				!m_code.PatchBranch(direct_branch, direct_target, VitaA32::Condition::MI))
			{
				return false;
			}

			if (not_taken_link_target_offset)
				*not_taken_link_target_offset = not_taken_target_offset;
			if (taken_link_target_offset)
				*taken_link_target_offset = taken_target_offset;
			return true;
		}

		if (!m_code.EmitPop(REG_R4 | REG_R5 | REG_LR))
			return false;

		return m_code.EmitMovImm32(HOST_TMP4, static_cast<u32>(reinterpret_cast<uptr>(direct_exit))) &&
			   m_code.EmitBx(HOST_TMP4) &&
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
			case 0x0a: // MOVZ, owned by R5900OpcodeImpl.cpp::MOVZ().
				return EmitMOVZ(op);
			case 0x0b: // MOVN, owned by R5900OpcodeImpl.cpp::MOVN().
				return EmitMOVN(op);
			case 0x14: // DSLLV, owned by R5900OpcodeImpl.cpp::DSLLV().
				return EmitDSLLV(op);
			case 0x16: // DSRLV, owned by R5900OpcodeImpl.cpp::DSRLV().
				return EmitDSRLV(op);
			case 0x17: // DSRAV, owned by R5900OpcodeImpl.cpp::DSRAV().
				return EmitDSRAV(op);
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

	bool BlockCompiler::EmitMOVZ(u32 op)
	{
		return EmitConditionalMove(op, true);
	}

	bool BlockCompiler::EmitMOVN(u32 op)
	{
		return EmitConditionalMove(op, false);
	}

	bool BlockCompiler::EmitREGIMM(u32 op, u32 pc)
	{
		const unsigned rt = RT(op);
		const bool link = (rt == 0x10 || rt == 0x11 || rt == 0x12 || rt == 0x13);
		if (link)
		{
			// PCSX2 owners: Interpreter.cpp::BLTZAL()/BGEZAL()/BLTZALL()/BGEZALL() apply
			// R5900.h::_SetLink(31) before testing the branch condition.
			if (!EmitLink(31, pc))
				return false;
		}

		switch (rt)
		{
			case 0x00: // BLTZ, owned by Interpreter.cpp::BLTZ().
			case 0x02: // BLTZL, owned by Interpreter.cpp::BLTZL().
			case 0x10: // BLTZAL, owned by Interpreter.cpp::BLTZAL().
			case 0x12: // BLTZALL, owned by Interpreter.cpp::BLTZALL().
				return EmitBranchSigned(op, SignedBranchCondition::LessThanZero);
			case 0x01: // BGEZ, owned by Interpreter.cpp::BGEZ().
			case 0x03: // BGEZL, owned by Interpreter.cpp::BGEZL().
			case 0x11: // BGEZAL, owned by Interpreter.cpp::BGEZAL().
			case 0x13: // BGEZALL, owned by Interpreter.cpp::BGEZALL().
				return EmitBranchSigned(op, SignedBranchCondition::GreaterEqualZero);
			default:
				return false;
		}
	}

	bool BlockCompiler::EmitJ(u32, u32 pc)
	{
		return EmitJump(pc, false);
	}

	bool BlockCompiler::EmitJAL(u32, u32 pc)
	{
		return EmitJump(pc, true);
	}

	bool BlockCompiler::EmitJR(u32 op, u32 pc)
	{
		return EmitRegisterJump(op, pc, false);
	}

	bool BlockCompiler::EmitJALR(u32 op, u32 pc)
	{
		return EmitRegisterJump(op, pc, true);
	}

	bool BlockCompiler::EmitBEQ(u32 op)
	{
		return EmitBranchEqual(op, true);
	}

	bool BlockCompiler::EmitBNE(u32 op)
	{
		return EmitBranchEqual(op, false);
	}

	bool BlockCompiler::EmitBLEZ(u32 op)
	{
		return EmitBranchSigned(op, SignedBranchCondition::LessEqualZero);
	}

	bool BlockCompiler::EmitBGTZ(u32 op)
	{
		return EmitBranchSigned(op, SignedBranchCondition::GreaterThanZero);
	}

	bool BlockCompiler::EmitBEQL(u32 op)
	{
		return EmitBranchEqual(op, true);
	}

	bool BlockCompiler::EmitBNEL(u32 op)
	{
		return EmitBranchEqual(op, false);
	}

	bool BlockCompiler::EmitBLEZL(u32 op)
	{
		return EmitBranchSigned(op, SignedBranchCondition::LessEqualZero);
	}

	bool BlockCompiler::EmitBGTZL(u32 op)
	{
		return EmitBranchSigned(op, SignedBranchCondition::GreaterThanZero);
	}

	bool BlockCompiler::EmitLB(u32 op, u32 pc, u32 raw_cycles_through_instruction, const void* event_exit)
	{
		return EmitLoadWithCounterReadEvent(op, pc, raw_cycles_through_instruction, event_exit,
			reinterpret_cast<const void*>(&VitaEeMemRead8), true, 24);
	}

	bool BlockCompiler::EmitLH(u32 op, u32 pc, u32 raw_cycles_through_instruction, const void* event_exit)
	{
		return EmitLoadWithCounterReadEvent(op, pc, raw_cycles_through_instruction, event_exit,
			reinterpret_cast<const void*>(&VitaEeMemRead16Checked), true, 16);
	}

	bool BlockCompiler::EmitLW(u32 op, u32 pc, u32 raw_cycles_through_instruction, const void* event_exit)
	{
		const unsigned rt = RT(op);

		if (!EmitEffectiveAddress(op, HOST_TMP0) ||
			!EmitCounterReadFlagFromAddress(HOST_TMP0) ||
			!m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&VitaEeMemRead32Checked)))
		{
			return false;
		}

		if (rt != 0)
		{
			if (!m_code.EmitMovRegShiftImm(HOST_TMP1, HOST_TMP0, VitaA32::ShiftType::ASR, 31) ||
				!EmitStoreGpr64(rt, HOST_TMP0, HOST_TMP1))
			{
				return false;
			}

			return EmitCounterReadEventExit(pc + 4, raw_cycles_through_instruction, event_exit);
		}

		return true;
	}

	bool BlockCompiler::EmitLBU(u32 op, u32 pc, u32 raw_cycles_through_instruction, const void* event_exit)
	{
		return EmitLoadWithCounterReadEvent(op, pc, raw_cycles_through_instruction, event_exit,
			reinterpret_cast<const void*>(&VitaEeMemRead8), false, 0);
	}

	bool BlockCompiler::EmitLHU(u32 op, u32 pc, u32 raw_cycles_through_instruction, const void* event_exit)
	{
		return EmitLoadWithCounterReadEvent(op, pc, raw_cycles_through_instruction, event_exit,
			reinterpret_cast<const void*>(&VitaEeMemRead16Checked), false, 0);
	}

	bool BlockCompiler::EmitLWU(u32 op)
	{
		const unsigned rt = RT(op);

		if (!EmitEffectiveAddress(op, HOST_TMP0) ||
			!m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&VitaEeMemRead32Checked)))
		{
			return false;
		}

		if (rt == 0)
			return true;

		return m_code.EmitMovImm8(HOST_TMP1, 0) &&
			   EmitStoreGpr64(rt, HOST_TMP0, HOST_TMP1);
	}

	bool BlockCompiler::EmitSB(u32 op)
	{
		const unsigned rt = RT(op);

		return EmitEffectiveAddress(op, HOST_TMP0) &&
			   EmitLoadGprLow(rt, HOST_TMP1) &&
			   m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&VitaEeMemWrite8));
	}

	bool BlockCompiler::EmitSH(u32 op)
	{
		const unsigned rt = RT(op);

		return EmitEffectiveAddress(op, HOST_TMP0) &&
			   EmitLoadGprLow(rt, HOST_TMP1) &&
			   m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&VitaEeMemWrite16Checked));
	}

	bool BlockCompiler::EmitSW(u32 op)
	{
		const unsigned rt = RT(op);

		return EmitEffectiveAddress(op, HOST_TMP0) &&
			   EmitLoadGprLow(rt, HOST_TMP1) &&
			   m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&VitaEeMemWrite32Checked));
	}

	bool BlockCompiler::EmitDSLLV(u32 op)
	{
		return EmitShift64LeftVariable(op);
	}

	bool BlockCompiler::EmitDSRLV(u32 op)
	{
		return EmitShift64RightVariable(op, false);
	}

	bool BlockCompiler::EmitDSRAV(u32 op)
	{
		return EmitShift64RightVariable(op, true);
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

	bool BlockCompiler::EmitShift64LeftVariable(u32 op)
	{
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		const unsigned rd = RD(op);

		if (rd == 0)
			return true;

		if (!EmitLoadGpr64(rt, HOST_TMP0, HOST_TMP1) ||
			!EmitLoadGprLow(rs, HOST_TMP2) ||
			!m_code.EmitAndImm8(HOST_TMP2, HOST_TMP2, 0x3f) ||
			!m_code.EmitMovImm8(HOST_TMP3, 32) ||
			!m_code.EmitCmpReg(HOST_TMP2, HOST_TMP3))
		{
			return false;
		}

		const size_t ge32_branch = m_code.EmitBranchPlaceholder(VitaA32::Condition::CS);
		if (ge32_branch == static_cast<size_t>(-1))
			return false;

		if (!m_code.EmitMovImm8(HOST_TMP3, 0) ||
			!m_code.EmitCmpReg(HOST_TMP2, HOST_TMP3))
		{
			return false;
		}

		const size_t zero_branch = m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
		if (zero_branch == static_cast<size_t>(-1))
			return false;

		if (!m_code.EmitMovImm8(HOST_TMP3, 32) ||
			!m_code.EmitSubReg(HOST_TMP3, HOST_TMP3, HOST_TMP2) ||
			!m_code.EmitMovRegShiftReg(HOST_TMP4, HOST_TMP0, VitaA32::ShiftType::LSR, HOST_TMP3) ||
			!m_code.EmitMovRegShiftReg(HOST_TMP1, HOST_TMP1, VitaA32::ShiftType::LSL, HOST_TMP2) ||
			!m_code.EmitOrrReg(HOST_TMP1, HOST_TMP1, HOST_TMP4) ||
			!m_code.EmitMovRegShiftReg(HOST_TMP0, HOST_TMP0, VitaA32::ShiftType::LSL, HOST_TMP2))
		{
			return false;
		}

		const size_t done_branch = m_code.EmitBranchPlaceholder();
		if (done_branch == static_cast<size_t>(-1))
			return false;

		const size_t ge32_target = m_code.Size();
		if (!m_code.EmitAndImm8(HOST_TMP2, HOST_TMP2, 0x1f) ||
			!m_code.EmitMovRegShiftReg(HOST_TMP1, HOST_TMP0, VitaA32::ShiftType::LSL, HOST_TMP2) ||
			!m_code.EmitMovImm8(HOST_TMP0, 0))
		{
			return false;
		}

		const size_t store_target = m_code.Size();
		return m_code.PatchBranch(ge32_branch, ge32_target, VitaA32::Condition::CS) &&
			   m_code.PatchBranch(zero_branch, store_target, VitaA32::Condition::EQ) &&
			   m_code.PatchBranch(done_branch, store_target) &&
			   EmitStoreGpr64(rd, HOST_TMP0, HOST_TMP1);
	}

	bool BlockCompiler::EmitShift64RightVariable(u32 op, bool arithmetic)
	{
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		const unsigned rd = RD(op);

		if (rd == 0)
			return true;

		if (!EmitLoadGpr64(rt, HOST_TMP0, HOST_TMP1) ||
			!EmitLoadGprLow(rs, HOST_TMP2) ||
			!m_code.EmitAndImm8(HOST_TMP2, HOST_TMP2, 0x3f) ||
			!m_code.EmitMovImm8(HOST_TMP3, 32) ||
			!m_code.EmitCmpReg(HOST_TMP2, HOST_TMP3))
		{
			return false;
		}

		const size_t ge32_branch = m_code.EmitBranchPlaceholder(VitaA32::Condition::CS);
		if (ge32_branch == static_cast<size_t>(-1))
			return false;

		if (!m_code.EmitMovImm8(HOST_TMP3, 0) ||
			!m_code.EmitCmpReg(HOST_TMP2, HOST_TMP3))
		{
			return false;
		}

		const size_t zero_branch = m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
		if (zero_branch == static_cast<size_t>(-1))
			return false;

		const VitaA32::ShiftType high_shift = arithmetic ? VitaA32::ShiftType::ASR : VitaA32::ShiftType::LSR;
		if (!m_code.EmitMovImm8(HOST_TMP3, 32) ||
			!m_code.EmitSubReg(HOST_TMP3, HOST_TMP3, HOST_TMP2) ||
			!m_code.EmitMovRegShiftReg(HOST_TMP4, HOST_TMP1, VitaA32::ShiftType::LSL, HOST_TMP3) ||
			!m_code.EmitMovRegShiftReg(HOST_TMP0, HOST_TMP0, VitaA32::ShiftType::LSR, HOST_TMP2) ||
			!m_code.EmitOrrReg(HOST_TMP0, HOST_TMP0, HOST_TMP4) ||
			!m_code.EmitMovRegShiftReg(HOST_TMP1, HOST_TMP1, high_shift, HOST_TMP2))
		{
			return false;
		}

		const size_t done_branch = m_code.EmitBranchPlaceholder();
		if (done_branch == static_cast<size_t>(-1))
			return false;

		const size_t ge32_target = m_code.Size();
		if (!m_code.EmitAndImm8(HOST_TMP2, HOST_TMP2, 0x1f) ||
			!m_code.EmitMovRegShiftReg(HOST_TMP0, HOST_TMP1, high_shift, HOST_TMP2) ||
			!(arithmetic ?
				 m_code.EmitMovRegShiftImm(HOST_TMP1, HOST_TMP1, VitaA32::ShiftType::ASR, 31) :
				 m_code.EmitMovImm8(HOST_TMP1, 0)))
		{
			return false;
		}

		const size_t store_target = m_code.Size();
		return m_code.PatchBranch(ge32_branch, ge32_target, VitaA32::Condition::CS) &&
			   m_code.PatchBranch(zero_branch, store_target, VitaA32::Condition::EQ) &&
			   m_code.PatchBranch(done_branch, store_target) &&
			   EmitStoreGpr64(rd, HOST_TMP0, HOST_TMP1);
	}

	bool BlockCompiler::EmitConditionalMove(u32 op, bool move_on_zero)
	{
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		const unsigned rd = RD(op);

		if (rd == 0)
			return true;

		if (!EmitLoadGpr64(rt, HOST_TMP2, HOST_TMP3) ||
			!m_code.EmitOrrReg(HOST_TMP4, HOST_TMP2, HOST_TMP3, true))
		{
			return false;
		}

		const VitaA32::Condition skip_condition = move_on_zero ? VitaA32::Condition::NE : VitaA32::Condition::EQ;
		const size_t skip_store = m_code.EmitBranchPlaceholder(skip_condition);
		if (skip_store == static_cast<size_t>(-1))
			return false;

		if (!EmitLoadGpr64(rs, HOST_TMP0, HOST_TMP1) ||
			!EmitStoreGpr64(rd, HOST_TMP0, HOST_TMP1))
		{
			return false;
		}

		return m_code.PatchBranch(skip_store, m_code.Size(), skip_condition);
	}

	bool BlockCompiler::EmitJump(u32 pc, bool link)
	{
		if (!m_code.EmitMovImm8(HOST_BRANCH_FLAG, 1))
			return false;

		if (!link)
			return true;

		// PCSX2 owner: Interpreter.cpp::JAL() applies _SetLink(31) before
		// doBranch() executes the delay slot, so a delay-slot write to ra wins.
		return EmitLink(31, pc);
	}

	bool BlockCompiler::EmitRegisterJump(u32 op, u32 pc, bool link)
	{
		const unsigned rs = RS(op);
		const unsigned rd = RD(op);

		// PCSX2 owners: Interpreter.cpp::JR()/JALR() and
		// x86/ix86-32/iR5900Jump.cpp::recJR()/recJALR(). The target is snapped
		// before the delay slot, and JALR links before the delay slot.
		if (!EmitLoadGprLow(rs, HOST_BRANCH_TARGET))
			return false;
		if (EmuConfig.Gamefixes.GoemonTlbHack && !EmitGoemonTranslateHostReg(HOST_BRANCH_TARGET))
			return false;

		if (!link || rd == 0)
			return true;

		return EmitLink(rd, pc);
	}

	bool BlockCompiler::EmitGoemonBlockStartHook(u32 start_pc)
	{
		if (!EmuConfig.Gamefixes.GoemonTlbHack)
			return true;

		if (start_pc == GOEMON_PRELOAD_RETURN_PC_0 || start_pc == GOEMON_PRELOAD_RETURN_PC_1)
		{
			// PCSX2 owners: Interpreter.cpp::JR() and
			// x86/ix86-32/iR5900.cpp::recRecompile() preload Goemon's TLB cache
			// when execution reaches either return PC of the TLB-populating
			// function at 0x356250.
			return m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&GoemonPreloadTlb));
		}

		if (start_pc == GOEMON_UNLOAD_ENTRY_PC)
		{
			// PCSX2 owners: Interpreter.cpp::JAL() and
			// x86/ix86-32/iR5900.cpp::recRecompile() unload a Goemon TLB cache
			// entry at function 0x3563b8. The x86 path also marks the rec cache
			// for reset; Vita requests the same reset and performs it after this
			// generated block returns to the provider loop.
			return m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&VitaRequestA32EeCacheReset)) &&
				   EmitLoadGprLow(4, HOST_TMP0) &&
				   m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&GoemonUnloadTlb));
		}

		return true;
	}

	bool BlockCompiler::EmitGoemonTranslateHostReg(unsigned host_reg)
	{
		// PCSX2 owner: x86/ix86-32/iR5900Jump.cpp::recJR()/recJALR() snapshot
		// the register target, translate it with vtlb_DynV2P(), and only then
		// compile the delay slot.
		return m_code.EmitMovRegShiftImm(HOST_TMP0, host_reg, VitaA32::ShiftType::LSL, 0) &&
			   m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&vtlb_V2P)) &&
			   m_code.EmitMovRegShiftImm(host_reg, HOST_TMP0, VitaA32::ShiftType::LSL, 0);
	}

	bool BlockCompiler::EmitLink(unsigned guest_reg, u32 pc)
	{
		return m_code.EmitMovImm32(HOST_TMP0, pc + 8) &&
			   m_code.EmitMovImm8(HOST_TMP1, 0) &&
			   EmitStoreGpr64(guest_reg, HOST_TMP0, HOST_TMP1);
	}

	bool BlockCompiler::EmitBranchEqual(u32 op, bool branch_on_equal)
	{
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);

		return EmitLoadGpr64(rs, HOST_TMP0, HOST_TMP1) &&
			   EmitLoadGpr64(rt, HOST_TMP2, HOST_TMP3) &&
			   m_code.EmitEorReg(HOST_TMP0, HOST_TMP0, HOST_TMP2) &&
			   m_code.EmitEorReg(HOST_TMP1, HOST_TMP1, HOST_TMP3) &&
			   m_code.EmitOrrReg(HOST_TMP0, HOST_TMP0, HOST_TMP1, true) &&
			   m_code.EmitMovImm8(HOST_BRANCH_FLAG, 0) &&
			   m_code.EmitMovImm8(HOST_BRANCH_FLAG, 1, branch_on_equal ? VitaA32::Condition::EQ : VitaA32::Condition::NE);
	}

	bool BlockCompiler::EmitBranchSigned(u32 op, SignedBranchCondition condition)
	{
		const unsigned rs = RS(op);

		if (condition == SignedBranchCondition::LessThanZero ||
			condition == SignedBranchCondition::GreaterEqualZero)
		{
			return EmitLoadGprHigh(rs, HOST_TMP1) &&
				   m_code.EmitMovImm8(HOST_TMP2, 0) &&
				   m_code.EmitCmpReg(HOST_TMP1, HOST_TMP2) &&
				   m_code.EmitMovImm8(HOST_BRANCH_FLAG, 0) &&
				   m_code.EmitMovImm8(HOST_BRANCH_FLAG, 1,
					   condition == SignedBranchCondition::LessThanZero ? VitaA32::Condition::LT : VitaA32::Condition::GE);
		}

		if (!EmitLoadGpr64(rs, HOST_TMP0, HOST_TMP1) ||
			!m_code.EmitMovImm8(HOST_TMP2, 0) ||
			!m_code.EmitCmpReg(HOST_TMP1, HOST_TMP2) ||
			!m_code.EmitMovImm8(HOST_BRANCH_FLAG, 0))
		{
			return false;
		}

		if (condition == SignedBranchCondition::LessEqualZero)
		{
			if (!m_code.EmitMovImm8(HOST_BRANCH_FLAG, 1, VitaA32::Condition::LT))
				return false;
		}
		else
		{
			if (!m_code.EmitMovImm8(HOST_BRANCH_FLAG, 1, VitaA32::Condition::GT))
				return false;
		}

		const size_t high_nonzero = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
		if (high_nonzero == static_cast<size_t>(-1))
			return false;

		const VitaA32::Condition low_condition =
			(condition == SignedBranchCondition::LessEqualZero) ? VitaA32::Condition::EQ : VitaA32::Condition::NE;
		if (!m_code.EmitCmpReg(HOST_TMP0, HOST_TMP2) ||
			!m_code.EmitMovImm8(HOST_BRANCH_FLAG, 1, low_condition))
		{
			return false;
		}

		return m_code.PatchBranch(high_nonzero, m_code.Size(), VitaA32::Condition::NE);
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

	bool BlockCompiler::EmitLoadWithCounterReadEvent(u32 op, u32 pc, u32 raw_cycles_through_instruction,
		const void* event_exit, const void* read_helper, bool sign_extend, unsigned sign_shift)
	{
		const unsigned rt = RT(op);

		if (!EmitEffectiveAddress(op, HOST_TMP0) ||
			!EmitCounterReadFlagFromAddress(HOST_TMP0) ||
			!m_code.EmitCallAbsolute(read_helper))
		{
			return false;
		}

		if (rt == 0)
			return true;

		if (sign_extend)
		{
			if (!m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_TMP0, VitaA32::ShiftType::LSL,
					static_cast<u8>(sign_shift)) ||
				!m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_TMP0, VitaA32::ShiftType::ASR,
					static_cast<u8>(sign_shift)) ||
				!m_code.EmitMovRegShiftImm(HOST_TMP1, HOST_TMP0, VitaA32::ShiftType::ASR, 31))
			{
				return false;
			}
		}
		else if (!m_code.EmitMovImm8(HOST_TMP1, 0))
		{
			return false;
		}

		return EmitStoreGpr64(rt, HOST_TMP0, HOST_TMP1) &&
			   EmitCounterReadEventExit(pc + 4, raw_cycles_through_instruction, event_exit);
	}

	bool BlockCompiler::EmitCounterReadFlagFromAddress(unsigned host_reg)
	{
		// PCSX2 owners: R5900OpcodeImpl.cpp::LB()/LBU()/LH()/LHU()/LW()
		// check (addr & 0xffffe000) == 0x10000000 after the load to force
		// an EE counter-read event test.
		return m_code.EmitMovImm32(HOST_TMP2, 0xffffe000u) &&
			   m_code.EmitAndReg(HOST_TMP2, host_reg, HOST_TMP2) &&
			   m_code.EmitMovImm32(HOST_TMP3, 0x10000000u) &&
			   m_code.EmitCmpReg(HOST_TMP2, HOST_TMP3) &&
			   m_code.EmitMovImm8(HOST_BRANCH_FLAG, 0) &&
			   m_code.EmitMovImm8(HOST_BRANCH_FLAG, 1, VitaA32::Condition::EQ);
	}

	bool BlockCompiler::EmitCounterReadEventExit(u32 next_pc, u32 raw_cycles_through_instruction, const void* event_exit)
	{
		if (!event_exit || raw_cycles_through_instruction == 0)
			return false;

		if (!m_code.EmitMovImm8(HOST_TMP2, 0) ||
			!m_code.EmitCmpReg(HOST_BRANCH_FLAG, HOST_TMP2))
		{
			return false;
		}

		const size_t not_counter_read = m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
		if (not_counter_read == static_cast<size_t>(-1))
			return false;

		const u32 cycles = ScaleBlockCycles(raw_cycles_through_instruction);
		if (!EmitStorePc(next_pc) ||
			!EmitAddScaledCyclesToCpu(cycles) ||
			!m_code.EmitCallAbsolute(event_exit) ||
			!m_code.EmitPop(REG_R4 | REG_R5 | REG_PC))
		{
			return false;
		}

		return m_code.PatchBranch(not_counter_read, m_code.Size(), VitaA32::Condition::EQ);
	}

	bool BlockCompiler::EmitAddScaledCyclesToCpu(u32 cycles)
	{
		if (!m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(CYCLE_OFFSET)) ||
			!m_code.EmitLdrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(CYCLE_OFFSET + sizeof(u32))))
		{
			return false;
		}

		if (cycles <= 255)
		{
			if (!m_code.EmitAddImm8(HOST_TMP0, HOST_TMP0, static_cast<u8>(cycles), true))
				return false;
		}
		else
		{
			if (!m_code.EmitMovImm32(HOST_TMP2, cycles) ||
				!m_code.EmitAddReg(HOST_TMP0, HOST_TMP0, HOST_TMP2, true))
			{
				return false;
			}
		}

		return m_code.EmitAdcImm8(HOST_TMP1, HOST_TMP1, 0) &&
			   m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(CYCLE_OFFSET)) &&
			   m_code.EmitStrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(CYCLE_OFFSET + sizeof(u32)));
	}

	bool BlockCompiler::EmitEffectiveAddress(u32 op, unsigned host_reg)
	{
		const unsigned rs = RS(op);
		const s32 imm = static_cast<s32>(IMM_S(op));

		if (!EmitLoadGprLow(rs, host_reg))
			return false;

		if (imm == 0)
			return true;

		if (imm > 0 && imm <= 255)
			return m_code.EmitAddImm8(host_reg, host_reg, static_cast<u8>(imm));

		if (imm < 0 && imm >= -255)
			return m_code.EmitSubImm8(host_reg, host_reg, static_cast<u8>(-imm));

		return m_code.EmitMovImm32(HOST_TMP2, static_cast<u32>(imm)) &&
			   m_code.EmitAddReg(host_reg, host_reg, HOST_TMP2);
	}

	bool BlockCompiler::EmitLoadGprLow(unsigned guest_reg, unsigned host_reg)
	{
		if (guest_reg == 0)
			return m_code.EmitMovImm8(host_reg, 0);

		return m_code.EmitLdrImm12(host_reg, HOST_CPU_REGS, static_cast<u16>(GprOffset(guest_reg)));
	}

	bool BlockCompiler::EmitLoadGprHigh(unsigned guest_reg, unsigned host_reg)
	{
		if (guest_reg == 0)
			return m_code.EmitMovImm8(host_reg, 0);

		return m_code.EmitLdrImm12(host_reg, HOST_CPU_REGS, static_cast<u16>(GprOffset(guest_reg) + sizeof(u32)));
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

	bool BlockCompiler::EmitStorePcFromHostReg(unsigned host_reg)
	{
		return m_code.EmitStrImm12(host_reg, HOST_CPU_REGS, static_cast<u16>(PC_OFFSET));
	}

	bool BlockCompiler::EmitStoreBranchPc(u32 target_pc, u32 fallthrough_pc)
	{
		if (!EmitStorePc(fallthrough_pc) ||
			!m_code.EmitMovImm8(HOST_TMP1, 0) ||
			!m_code.EmitCmpReg(HOST_BRANCH_FLAG, HOST_TMP1))
		{
			return false;
		}

		const size_t not_taken = m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
		if (not_taken == static_cast<size_t>(-1))
			return false;

		if (!EmitStorePc(target_pc))
			return false;

		return m_code.PatchBranch(not_taken, m_code.Size(), VitaA32::Condition::EQ);
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
