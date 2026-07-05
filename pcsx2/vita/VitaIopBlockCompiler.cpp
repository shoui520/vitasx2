// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#include "pcsx2/vita/VitaIopBlockCompiler.h"

#include "common/Vita/VitaJitMemory.h"
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

	static_assert(PC_OFFSET <= 4095);
	static_assert(CODE_OFFSET <= 4095);
	static_assert(CYCLE_OFFSET + sizeof(u64) <= 4095);
	static_assert((CYCLE_OFFSET % alignof(u64)) == 0);
	static_assert(IOP_NEXT_EVENT_CYCLE_OFFSET > CYCLE_OFFSET);
	static_assert(IOP_NEXT_EVENT_CYCLE_FROM_CYCLE_OFFSET <= 0xff);
	static_assert((IOP_NEXT_EVENT_CYCLE_FROM_CYCLE_OFFSET % alignof(u64)) == 0);
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
		m_scalar_load_cold_tails.clear();
		m_scalar_store_cold_tails.clear();
		m_unaligned_read_cold_tails.clear();
		m_unaligned_write_cold_tails.clear();
		m_cop2_load_cold_tails.clear();
		m_cop2_store_cold_tails.clear();
		// Trace blocks call out before every cycle increment. Pure production
		// blocks batch cycles once at the tail, so r10 is only useful for the
		// remaining per-instruction cycle path.
		m_iop_cycle_base_register_available =
			!m_iop_ram_registers_available && !m_emit_trace_checks && !m_defer_cycle_updates;
		m_saved_registers = REG_R4 | REG_R5 | REG_R6 | REG_R7 | REG_R8;
		if (m_iop_ram_registers_available)
			m_saved_registers |= REG_R10 | REG_R11;
		else if (m_iop_cycle_base_register_available)
			m_saved_registers |= REG_R10;

		if (!m_code.EmitPush(m_saved_registers | REG_LR) ||
			!m_code.EmitMovImm32(HOST_PSX_REGS, static_cast<u32>(reinterpret_cast<uptr>(&psxRegs))))
		{
			return false;
		}

		if (m_iop_cycle_base_register_available)
			return m_code.EmitAddImm32(HOST_CYCLE_BASE, HOST_PSX_REGS, static_cast<u32>(CYCLE_OFFSET));

		return !m_iop_ram_registers_available ||
			   (m_code.EmitMovImm32(HOST_IOP_RAM_MASK, Ps2MemSize::ExposedIopRam - 1) &&
				   m_code.EmitMovImm32(HOST_IOP_RAM_BASE, static_cast<u32>(reinterpret_cast<uptr>(iopMem->Main))));
	}

	bool BlockCompiler::EndBlockReturn(BlockExitKind exit)
	{
		return m_code.EmitMovImm32(HOST_TMP0, static_cast<u32>(exit)) &&
			   m_code.EmitPop(m_saved_registers | REG_PC);
	}

	bool BlockCompiler::EndBlockDirectTail(const void* direct_exit, size_t* direct_link_target_offset)
	{
		if (!direct_exit)
			return false;

		if (!m_code.EmitPop(m_saved_registers | REG_LR))
			return false;

		const size_t target_offset = m_code.Size();
		if (!m_code.EmitMovImm32Patchable(HOST_CALL_SCRATCH, static_cast<u32>(reinterpret_cast<uptr>(direct_exit))) ||
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
			!m_code.EmitCmpImm32(HOST_TMP0, 0))
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

	bool BlockCompiler::EmitIncrementCycle()
	{
		return EmitAddCycles(1);
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
		if (dst_guest_reg == src_guest_reg)
			return true;

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

		const auto store_zero = [this, rd]() {
			return m_code.EmitMovImm8(HOST_TMP0, 0) &&
				   EmitStoreGpr(rd, HOST_TMP0);
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

		if (sa == 0)
			return EmitMoveGpr(rd, rt);

		if (!EmitLoadGpr(rt, HOST_TMP0))
			return false;

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

		const void* helper = is_signed ?
			reinterpret_cast<const void*>(&VitaIopA32DivResult) :
			reinterpret_cast<const void*>(&VitaIopA32DivuResult);

		if (!EmitLoadGpr(RS(op), HOST_TMP0) ||
			!EmitLoadGpr(RT(op), HOST_TMP1))
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
				!m_code.EmitMovImm8(HOST_TMP3, 0) ||
				!m_code.EmitSubReg(HOST_TMP2, HOST_TMP3, HOST_TMP0) ||
				!store_hilo(HOST_TMP2, HOST_TMP3) ||
				!emit_branch(done_branches[done_branch_count++], VitaA32::Condition::AL) ||
				!patch_branch(equal_branch, m_code.Size()) ||
				!m_code.EmitMovImm8(HOST_TMP2, 1) ||
				!m_code.EmitMovImm8(HOST_TMP3, 0) ||
				!store_hilo(HOST_TMP2, HOST_TMP3) ||
				!emit_branch(done_branches[done_branch_count++], VitaA32::Condition::AL) ||
				!patch_branch(power_of_two_branch, m_code.Size()) ||
				!m_code.EmitMovRegShiftImm(HOST_TMP3, HOST_TMP0, VitaA32::ShiftType::ASR, 31) ||
				!m_code.EmitSubImm8(HOST_TMP2, HOST_TMP1, 1) ||
				!m_code.EmitAndReg(HOST_TMP3, HOST_TMP3, HOST_TMP2) ||
				!m_code.EmitAddReg(HOST_TMP3, HOST_TMP0, HOST_TMP3) ||
				!m_code.EmitClz(HOST_SAVED0, HOST_TMP1) ||
				!m_code.EmitMovImm8(HOST_TMP2, 31) ||
				!m_code.EmitSubReg(HOST_TMP2, HOST_TMP2, HOST_SAVED0) ||
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
				!m_code.EmitMovImm8(HOST_TMP2, 1) ||
				!m_code.EmitMovImm8(HOST_TMP3, 0) ||
				!store_hilo(HOST_TMP2, HOST_TMP3) ||
				!emit_branch(done_branches[done_branch_count++], VitaA32::Condition::AL) ||
				!patch_branch(power_of_two_branch, m_code.Size()) ||
				!m_code.EmitSubImm8(HOST_TMP2, HOST_TMP1, 1) ||
				!m_code.EmitAndReg(HOST_TMP3, HOST_TMP0, HOST_TMP2) ||
				!m_code.EmitClz(HOST_TMP2, HOST_TMP1) ||
				!m_code.EmitMovImm8(HOST_SAVED0, 31) ||
				!m_code.EmitSubReg(HOST_SAVED0, HOST_SAVED0, HOST_TMP2) ||
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

		if ((opcode == 0x08 || opcode == 0x09) && IMM_S(op) == 0)
			return EmitMoveGpr(rt, rs);

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

		if (!m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_SAVED0, VitaA32::ShiftType::LSL, 0) ||
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

		const size_t isolated_fallback_branch = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
		if (isolated_fallback_branch == static_cast<size_t>(-1) ||
			!EmitLoadGpr(RT(op), HOST_TMP1) ||
			!m_code.EmitAndReg(HOST_TMP0, HOST_SAVED0, HOST_IOP_RAM_MASK) ||
			!emit_store_value() ||
			!emit_clear_stored_word())
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
		});
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
		// PCSX2 owners: R3000AInterpreter.cpp::psxLWL/psxLWR/psxSWL/psxSWR
		// merge an aligned iopMemRead32() word. Handler-backed addresses call
		// the helper; ordinary IOP RAM falls through with HOST_TMP0 loaded.
		const size_t fallback_target = m_code.Size();
		if (!m_code.PatchBranch(tail.fallback_branch, fallback_target, VitaA32::Condition::NE) ||
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
			!m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_SAVED1, VitaA32::ShiftType::LSL, 0) ||
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

	bool BlockCompiler::EmitUnalignedLoadOp(u32 op)
	{
		const bool left = ((op >> 26) == 0x22);
		if (!EmitEffectiveAddress(op) ||
			!m_code.EmitAndImm8(HOST_SAVED0, HOST_TMP0, 3) ||
			!m_code.EmitMovRegShiftImm(HOST_SAVED0, HOST_SAVED0, VitaA32::ShiftType::LSL, 3) ||
			!m_code.EmitBicImm32(HOST_SAVED1, HOST_TMP0, 3) ||
			!m_code.EmitTstImm32(HOST_SAVED1, 0x10000000u))
		{
			return false;
		}

		// PCSX2 owner: R3000AInterpreter.cpp::psxLWL/psxLWR use iopMemRead32()
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
				   m_code.EmitMovImm8(HOST_TMP3, 24) &&
				   m_code.EmitSubReg(HOST_TMP3, HOST_TMP3, HOST_SAVED0) &&
				   m_code.EmitOrrRegShiftReg(HOST_TMP0, HOST_TMP1, HOST_TMP0,
					   VitaA32::ShiftType::LSL, HOST_TMP3) &&
				   EmitStoreGpr(RT(op), HOST_TMP0);
		}

		return m_code.EmitMovImm32(HOST_TMP2, 0xffffff00u) &&
			   m_code.EmitMovImm8(HOST_TMP3, 24) &&
			   m_code.EmitSubReg(HOST_TMP3, HOST_TMP3, HOST_SAVED0) &&
			   m_code.EmitAndRegShiftReg(HOST_TMP1, HOST_TMP1, HOST_TMP2,
				   VitaA32::ShiftType::LSL, HOST_TMP3) &&
			   m_code.EmitOrrRegShiftReg(HOST_TMP0, HOST_TMP1, HOST_TMP0,
				   VitaA32::ShiftType::LSR, HOST_SAVED0) &&
			   EmitStoreGpr(RT(op), HOST_TMP0);
	}

	bool BlockCompiler::EmitUnalignedStoreOp(u32 op)
	{
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

		// PCSX2 owners: R3000AInterpreter.cpp::psxSWL/psxSWR merge the aligned
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
		});

		if (!EmitLoadGpr(RT(op), HOST_TMP1))
			return false;

		if (left)
		{
			if (!m_code.EmitMovImm8(HOST_TMP3, 24) ||
				!m_code.EmitSubReg(HOST_TMP3, HOST_TMP3, HOST_SAVED0) ||
				!m_code.EmitMovImm32(HOST_TMP2, 0xffffff00u))
			{
				return false;
			}
		}
		else
		{
			if (!m_code.EmitMovImm8(HOST_TMP3, 24) ||
				!m_code.EmitSubReg(HOST_TMP3, HOST_TMP3, HOST_SAVED0) ||
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
		});
		return true;
	}

	bool BlockCompiler::EmitConditionalBranchOp(u32 op, u32 pc)
	{
		if (m_emit_native_static_branch)
			return EmitConditionalBranchFlag(op);

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

	bool BlockCompiler::EmitConditionalBranchFlag(u32 op)
	{
		if (!EmitLoadGpr(RS(op), HOST_TMP0) ||
			!EmitLoadGpr(RT(op), HOST_TMP1) ||
			!m_code.EmitCmpReg(HOST_TMP0, HOST_TMP1) ||
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
			   m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&psxDoBranch), HOST_CALL_SCRATCH) &&
			   EndBlockReturn(BlockExitKind::Direct) &&
			   m_code.PatchBranch(not_taken, m_code.Size(), skip_taken);
	}

	bool BlockCompiler::EmitSignedBranchFlag(u32 op)
	{
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
			   m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&psxDoBranch), HOST_CALL_SCRATCH) &&
			   EndBlockReturn(BlockExitKind::Direct);
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
			!m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&psxDoBranch), HOST_CALL_SCRATCH) ||
			!EndBlockReturn(BlockExitKind::Direct))
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
			!m_code.EmitLdrImm12(HOST_SAVED1, HOST_TMP3, 0) ||
			!m_code.EmitSubReg(HOST_TMP3, HOST_TMP0, HOST_SAVED0) ||
			!m_code.EmitCmpReg(HOST_TMP3, HOST_SAVED1))
		{
			return false;
		}
		helper_branches.push_back({m_code.EmitBranchPlaceholder(VitaA32::Condition::GE),
			VitaA32::Condition::GE});

		// HOST_TMP2 still holds iopNextEventCycle.low, while HOST_SAVED0/1 keep
		// psxNextStartCounter.low and psxNextDeltaCounter for the second
		// no-work test.
		if (!m_code.EmitSubReg(HOST_TMP2, HOST_TMP2, HOST_SAVED0) ||
			!m_code.EmitCmpReg(HOST_SAVED1, HOST_TMP2))
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

	bool BlockCompiler::EmitInstruction(u32 op, u32 pc, bool store_pc, std::vector<size_t>& direct_exit_branches)
	{
		const u32 next_pc = pc + 4;
		if (((m_emit_trace_checks || IopInstructionRequiresCodeState(op)) && !EmitStoreCode(op)) ||
			(m_emit_trace_checks && !EmitTraceCheck(pc, op, direct_exit_branches)) ||
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

		m_iop_ram_registers_available = false;
		for (u32 i = 0; i < instruction_count; i++)
		{
			if (UsesDirectIopRamFastPath(iopMemRead32(start_pc + i * 4)))
			{
				m_iop_ram_registers_available = true;
				break;
			}
		}
		m_emit_trace_checks = VitaIsIopPreInstructionTraceEnabled();
		m_defer_cycle_updates = !m_emit_trace_checks && IopBlockCanDeferCycleUpdates(start_pc, instruction_count);

		if (!BeginBlock())
			return false;

		std::vector<size_t> direct_exit_branches;
		direct_exit_branches.reserve(instruction_count * 2);
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
			const bool emitted = CanCompileOpcode(op) && EmitInstruction(op, pc, store_pc, direct_exit_branches);
			m_emit_native_static_branch = false;
			m_emit_native_static_jump = false;
			m_emit_native_register_jump = false;
			if (!emitted)
				return false;
		}

		const u32 next_pc = start_pc + instruction_count * 4;
		if (m_defer_cycle_updates && !EmitAddCycles(instruction_count))
			return false;

		size_t direct_exit_offset = 0;
		const bool emit_link_tail = direct_exit && direct_links && can_direct_link_fallthrough;
		const bool emit_branch_link_tails = direct_exit && direct_links && has_native_static_branch;
		const auto emit_direct_or_return_tail = [&](u32 target_pc, u8 slot_index) -> bool {
			if (emit_branch_link_tails)
			{
				size_t target_offset = 0;
				if (!EndBlockDirectTail(direct_exit, &target_offset))
					return false;

				direct_links->slots[slot_index].target_pc = target_pc;
				direct_links->slots[slot_index].target_offset = target_offset;
				direct_links->slots[slot_index].valid = true;
				return true;
			}

			return EndBlockReturn(BlockExitKind::Direct);
		};

		if (has_native_static_branch)
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
			if (!EndBlockReturn(BlockExitKind::Direct))
				return false;
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
				size_t target_offset = 0;
				if (!EndBlockDirectTail(direct_exit, &target_offset))
					return false;

				direct_links->slots[0].target_pc = static_jump_target_pc;
				direct_links->slots[0].target_offset = target_offset;
				direct_links->slots[0].valid = true;
			}
			else if (!EndBlockReturn(BlockExitKind::Direct))
			{
				return false;
			}

			direct_exit_offset = m_code.Size();
			if (!EndBlockReturn(BlockExitKind::Direct))
				return false;
		}
		else if (has_native_register_jump)
		{
			if (!EmitStorePcReg(HOST_REGISTER_JUMP_TARGET) ||
				!EmitIopEventTestFastPath() ||
				!EmitPcChangedExitCheckReg(HOST_REGISTER_JUMP_TARGET, direct_exit_branches) ||
				!EndBlockReturn(BlockExitKind::Direct))
			{
				return false;
			}

			direct_exit_offset = m_code.Size();
			if (!EndBlockReturn(BlockExitKind::Direct))
				return false;
		}
		else if (emit_link_tail)
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

		if (!FlushColdTails())
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
		RememberFreeCacheEntry(block);
	}

	u32 BlockExecutor::InvalidateRange(u32 start_pc, u32 instruction_count)
	{
		if (instruction_count == 0 || instruction_count > ((UINT32_MAX - start_pc) / 4))
			return 0;

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
		// translated ranges. Vita also validates the dispatcher entry block
		// because it cannot rely on x86 protected-page repair.
		InvalidateCachedBlock(block);
		return false;
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
				CommitCodeSlice(code_slice_offset, block.code.Size());
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

	bool BlockExecutor::RunCachedBlock(CachedBlock& block, BlockExecutionResult* result)
	{
		if (!result || !block.valid)
			return false;

		if (!ValidateCachedBlock(block))
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
