// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#include "pcsx2/vita/VitaIopBlockCompiler.h"

#include "common/Assertions.h"
#include "common/Vita/VitaJitMemory.h"
#include "pcsx2/Config.h"
#include "pcsx2/DebugTools/Debug.h"
#if defined(VITASX2_QEMU_VALIDATION) || defined(VITASX2_PORTABLE_REPLAY_VALIDATION)
#include "pcsx2/DebugTools/CoreEventTrace.h"
#endif
#include "pcsx2/DebugTools/SymbolGuardian.h"
#include "pcsx2/Host.h"
#include "pcsx2/IopBios.h"
#include "pcsx2/IopDma.h"
#include "pcsx2/IopGte.h"
#include "pcsx2/IopHw.h"
#include "pcsx2/IopMem.h"
#include "pcsx2/R3000A.h"
#include "pcsx2/R5900.h"
#include "pcsx2/VMManager.h"
#include "pcsx2/vita/VitaCore.h"
#include "pcsx2/vita/VitaPerformanceTelemetry.h"

#include <algorithm>
#include <cstring>
#include <new>
#include <utility>

#if defined(__arm__) && defined(__GNUC__)
// ReturnFromPrivateProviderTimeslice() restores SP directly to the private
// caller's CFA, bypassing any compiler-generated VFP epilogue. Reserve only
// the AAPCS callee-saved VFP bank throughout this translation unit: private
// bodies then preserve d8-d15 structurally while GCC remains free to use the
// caller-clobbered banks, including d16 for paired IOP-state stores.
register double g_vita_iop_reserved_d8 asm("d8");
register double g_vita_iop_reserved_d9 asm("d9");
register double g_vita_iop_reserved_d10 asm("d10");
register double g_vita_iop_reserved_d11 asm("d11");
register double g_vita_iop_reserved_d12 asm("d12");
register double g_vita_iop_reserved_d13 asm("d13");
register double g_vita_iop_reserved_d14 asm("d14");
register double g_vita_iop_reserved_d15 asm("d15");
#endif

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
u32 g_qemuIopRetainedLoReadFastPaths = 0;
struct QemuIopLinkedFrameEvidence
{
	u32 entries = 0;
	u32 instructions_removed = 0;
	u32 stack_words_removed = 0;
};
static QemuIopLinkedFrameEvidence s_qemuIopLinkedFrameEvidence;
static u32 s_qemuIopSequentialQwordCopyFastPaths = 0;
static u32 s_qemuIopBranchEventCandidates = 0;
static u32 s_qemuIopBranchEventBudgetPositive = 0;
static u32 s_qemuIopBranchEventTestsEntered = 0;
static bool s_qemuIopTrustedSourceAuditEnabled = true;
static bool s_qemuIopPinnedGprResidencyEnabled = true;
static bool s_qemuIopRetainedLoForwardingEnabled = true;
static bool s_qemuIopPinnedBranchDirectCompareEnabled = true;
static bool s_qemuIopConditionCodeBranchEnabled = true;
static bool s_qemuIopProducerBranchFlagsEnabled = true;
static bool s_qemuIopRamProvenanceSpecializationEnabled = true;
static bool s_qemuIopSourcePageLiteralEnabled = true;
static bool s_qemuIopIsolateCacheSpecializationEnabled = true;
static bool s_qemuIopClockModeSpecializationEnabled = true;
static bool s_qemuIopSavedRegisterNarrowingEnabled = true;
static bool s_qemuIopBlockCycleBatchingEnabled = true;
static bool s_qemuIopLinkedFrameBypassEnabled = true;
static bool s_qemuIopResidentPreludeLinksEnabled = true;
static bool s_qemuIopResidentGprLinksEnabled = true;
static bool s_qemuIopPublishedEventDeadlineResidencyEnabled = true;
static bool s_qemuIopEeBudgetResidencyEnabled = true;
static bool s_qemuIopGeneratedInstrumentationEnabled = true;
static bool s_qemuIopSequentialQwordCopyEnabled = true;
static bool s_qemuIopBranchTestSchedulingEnabled = true;
static bool s_qemuIopPrivateDispatcherHotPathEnabled = true;
static bool s_qemuIopHotDispatchOwnershipEnabled = true;
static bool s_qemuIopCachedWaitDescriptorEnabled = true;
static bool s_qemuIopInlineWaitFastForwardEnabled = true;
static bool s_qemuIopWaitResumeCacheEnabled = true;
static bool s_qemuIopWaitResumeFirstEntryOwnershipEnabled = true;
static bool s_qemuIopWaitResumeKindEntryEnabled = true;
static bool s_qemuIopWaitResumeClockEntryEnabled = true;
static bool s_qemuIopWaitResumeNoLinkEntryEnabled = true;
static bool s_qemuIopWaitResumeDescriptorSpecializationEnabled = true;
static bool s_qemuIopCompiledPs1BiosGateEnabled = true;
static bool s_qemuIopSchedulerDirectResumeEnabled = true;
static bool s_qemuIopNullOpcodeCompilationEnabled = true;
static size_t s_qemuIopCodeCacheCapacityLimit = 0;
static u64 s_qemuIopInlineWaitFastForwards = 0;
extern "C"
{
	u32 g_vita_a32_iop_scheduler_pre_event_wait_advance_enabled = 1;
}
#endif

namespace
{
	using GeneratedBlock = u32 (*)();

#if defined(__arm__)
	inline __attribute__((always_inline)) u32
	RunGeneratedProviderEntry(const void* entry)
	{
		register u32 result asm("r0");
		register const void* target asm("r12") = entry;
		// A fixed private slot makes every linked body ABI-compatible, including
		// transitions from blocks whose own analysis needed no cycle scratch.
		// The IOP emitter's only vector temporary is q0. Declare it explicitly;
		// the private dispatcher reserves d8-d15 throughout this translation
		// unit, while this keeps the callable helper correct if that changes.
		asm volatile("sub sp, sp, #8\n\t"
			"adr r9, 1f\n\t"
			"bx %[target]\n\t"
			"1:\n\t"
			"add sp, sp, #8"
			: "=r"(result), [target] "+r"(target)
			:
			: "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10",
			  "r11", "lr", "d0", "d1", "cc", "memory");
		return result;
	}

	[[noreturn]] inline __attribute__((always_inline)) void
	ReturnFromPrivateProviderTimeslice(s32 result)
	{
		register s32 value asm("r0") = result;
		register void* caller_cfa asm("r1") = __builtin_dwarf_cfa();
		asm volatile(
			// The private entry stored its caller LR at CFA-4. Restore only
			// that value; r4-r11 are caller-owned at the EE scheduler seam.
			"ldr lr, [r1, #-4]\n"
			"mov sp, r1\n"
			"bx lr\n"
			:
			: "r"(value), "r"(caller_cfa)
			: "lr", "memory");
		__builtin_unreachable();
	}
#endif

	constexpr u16 REG_R4 = 1u << 4;
	constexpr u16 REG_R5 = 1u << 5;
	constexpr u16 REG_R6 = 1u << 6;
	constexpr u16 REG_R7 = 1u << 7;
	constexpr u16 REG_R8 = 1u << 8;
	constexpr u16 REG_R9 = 1u << 9;
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
	// PCSX2's _DynGen_EnterRecompiledCode() owns the return continuation for a
	// complete linked chain. r9 is otherwise unused by the IOP compiler and is
	// callee-saved across every AAPCS helper, so it carries that continuation.
	constexpr unsigned HOST_CHAIN_RETURN = 9;
	// r10 is also HOST_IOP_RAM_MASK; the cycle base is only allocated in
	// non-trace blocks that do not reserve the direct IOP RAM fast-path pair.
	constexpr unsigned HOST_CYCLE_BASE = 10;
	constexpr unsigned HOST_IOP_RAM_MASK = 10;
	constexpr unsigned HOST_IOP_RAM_BASE = 11;
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
	constexpr size_t INTERRUPT_OFFSET = offsetof(psxRegisters, interrupt);
	constexpr size_t IOP_NEXT_EVENT_CYCLE_OFFSET =
		offsetof(psxRegisters, iopNextEventCycle);
	constexpr size_t IOP_NEXT_EVENT_CYCLE_FROM_CYCLE_OFFSET =
		IOP_NEXT_EVENT_CYCLE_OFFSET - CYCLE_OFFSET;
	constexpr size_t IOP_CYCLE_EE_OFFSET = offsetof(psxRegisters, iopCycleEE);
	constexpr size_t IOP_CYCLE_EE_CARRY_OFFSET =
		offsetof(psxRegisters, iopCycleEECarry);
	constexpr size_t IOP_BREAK_OFFSET = offsetof(psxRegisters, iopBreak);
	constexpr u32 IOP_WAIT_CYCLES = 384;

	constexpr unsigned RS(u32 op) { return (op >> 21) & 0x1f; }

	constexpr unsigned RT(u32 op) { return (op >> 16) & 0x1f; }

	constexpr unsigned RD(u32 op) { return (op >> 11) & 0x1f; }

	constexpr unsigned SA(u32 op) { return (op >> 6) & 0x1f; }

	constexpr s16 IMM_S(u32 op) { return static_cast<s16>(op); }

	constexpr u16 IMM_U(u32 op) { return static_cast<u16>(op); }

	constexpr u32 BranchTarget(u32 pc, u32 op)
	{
		return pc + 4 + static_cast<u32>(static_cast<s32>(IMM_S(op)) * 4);
	}

	constexpr u32 JumpTarget(u32 pc, u32 op)
	{
		return ((pc + 4) & 0xf0000000u) | ((op & 0x03ffffffu) << 2);
	}

	constexpr u32 IopRecompilerCyclePenalty(u32 op)
	{
		// PCSX2 owners: x86/iR3000A.h::psxInstCycles_Mult /
		// psxInstCycles_Div, x86/iR3000A.h::PSXRECOMPILE_CONSTCODE3_PENALTY,
		// and x86/iR3000Atables.cpp::{rpsxMULT,rpsxMULTU,rpsxDIV,rpsxDIVU}.
		// The cost is opcode-based even when constant propagation replaces the
		// operation with a cheaper host sequence.
		if ((op >> 26) != 0)
			return 0;

		switch (op & 0x3f)
		{
			case 0x18: // MULT
			case 0x19: // MULTU
				return 7;
			case 0x1a: // DIV
			case 0x1b: // DIVU
				return 40;
			default:
				return 0;
		}
	}

	constexpr u32 IopRecompilerInstructionCycles(u32 op)
	{
		return 1 + IopRecompilerCyclePenalty(op);
	}

	constexpr u32 IopCompilerInstructionCycles(u32 op, bool interpreter_trace)
	{
		// PCSX2's IOP pre-instruction trace is necessarily interpreter-owned:
		// pcsx2-trace rejects --recompiler-iop with --iop-out. Keep that diagnostic
		// mode on the interpreter's one-cycle timeline while product execution uses
		// the x86 recompiler timing model above.
		return interpreter_trace ? 1u : IopRecompilerInstructionCycles(op);
	}

	constexpr bool IopInstructionPreservesRetainedLoHost(u32 op)
	{
		// r3 is caller-clobbered, but these native scalar templates use only
		// r0-r2. Keeping a just-produced LO there is therefore exact until an
		// instruction outside this deliberately narrow set is reached. Memory,
		// control, COP and helper paths fail closed because their cold arms or
		// lowering details can overwrite r3.
		switch (op >> 26)
		{
			case 0x00:
				switch (op & 0x3f)
				{
					case 0x00: // SLL (including NOP)
					case 0x02: // SRL
					case 0x03: // SRA
					case 0x04: // SLLV
					case 0x06: // SRLV
					case 0x07: // SRAV
					case 0x10: // MFHI
					case 0x11: // MTHI
					case 0x12: // MFLO
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

	bool RetainedLoForwardingEnabled()
	{
#if defined(VITASX2_IOP_RETAINED_LO_CONTROL)
		return false;
#elif defined(VITASX2_QEMU_VALIDATION)
		return s_qemuIopRetainedLoForwardingEnabled;
#else
		return true;
#endif
	}

	enum class IopRecSourceKind : u8
	{
		Ram,
		Rom,
	};

	bool GetIopRecExecutableSpan(u32 pc, u32* available_bytes,
		IopRecSourceKind* source_kind = nullptr)
	{
		// PCSX2 owner: x86/iR3000A.cpp::recResetIOP(). psxRecLUT maps only
		// kuseg plus the kseg0/kseg1 aliases of RAM, ROM1, the mapped ROM2
		// prefix, and the BIOS ROM. Handler-backed pages use
		// iopUnmappedRecLUTPage and must never be decoded or linked as code.
		const u32 segment = pc >> 29;
		if (segment != 0 && segment != 4 && segment != 5)
			return false;

		const u32 physical_pc = pc & 0x1fffffffu;
		u32 region_end = 0;
		IopRecSourceKind kind = IopRecSourceKind::Rom;
		if (physical_pc < Ps2MemSize::TotalIopRam)
		{
			region_end = Ps2MemSize::TotalIopRam;
			kind = IopRecSourceKind::Ram;
		}
		else if (physical_pc >= 0x1e000000u && physical_pc < 0x1e480000u)
		{
			// recResetIOP() makes ROM1 and its eight mapped ROM2 pages one
			// contiguous executable interval even though their backing arrays differ.
			region_end = 0x1e480000u;
		}
		else if (physical_pc >= 0x1fc00000u)
		{
			region_end = 0x20000000u;
		}
		else
		{
			return false;
		}

		if (available_bytes)
			*available_bytes = region_end - physical_pc;
		if (source_kind)
			*source_kind = kind;
		return true;
	}

	const u32* ResolveIopRecOpcode(u32 pc)
	{
		u32 available_bytes = 0;
		if (!GetIopRecExecutableSpan(pc, &available_bytes) ||
			available_bytes < sizeof(u32) || !psxMemRLUT)
		{
			return nullptr;
		}

		const u32 physical_pc = pc & 0x1fffffffu;
		const uptr page_base = psxMemRLUT[physical_pc >> 16];
		return page_base ? reinterpret_cast<const u32*>(page_base +
														(physical_pc & 0xffffu)) :
		                   nullptr;
	}

	u32 IopCompilerBlockCycles(u32 start_pc, u32 instruction_count,
		bool interpreter_trace)
	{
		if (interpreter_trace)
			return instruction_count;

		u32 cycles = 0;
		for (u32 i = 0; i < instruction_count; i++)
			cycles += IopRecompilerInstructionCycles(iopMemRead32(start_pc + i * 4));
		return cycles;
	}

	void ChargeIopRecompilerEeBudget(u32 block_cycles, bool ps1_clock)
	{
		// PCSX2 owner: x86/iR3000A.cpp::iPsxAddEECycles(). Preserve its
		// 32-bit arithmetic, including the PS1-clock numerator and carry.
		if (!ps1_clock)
		{
			psxRegs.iopCycleEE = static_cast<s32>(static_cast<u32>(psxRegs.iopCycleEE) -
												  block_cycles * 8u);
			return;
		}

		const u32 numerator = block_cycles * 1280u + psxRegs.iopCycleEECarry;
		psxRegs.iopCycleEECarry = numerator % 147u;
		psxRegs.iopCycleEE =
			static_cast<s32>(static_cast<u32>(psxRegs.iopCycleEE) - numerator / 147u);
	}

	void ApplyIopRecompilerEntrySideEffects(u32 start_pc)
	{
		// PCSX2 owner: x86/iR3000A.cpp::iopRecRecompile(). These effects belong
		// to a genuine target-block compilation, after JR/JALR's delay/event seam;
		// they must not run in psxDoBranch() before that seam or repeat on cache
		// hits.
		if (start_pc == 0x00000890u)
			R3000SymbolGuardian.ClearIrxModules();

		if (start_pc == 0x00001630u && EmuConfig.CurrentIRX.length() > 3 &&
			iopMemRead32(0x00020018u) == 0x1fu)
		{
			iopMemWrite32(0x00020094u, 0xbffc0000u);
		}

		if (start_pc == 0xbfc4a000u)
			psxRegs.GPR.n.a0 = Ps2MemSize::ExposedIopRam >> 20;
	}

	bool AnalyzeIopWaitLoopShape(u32 start_pc, u32 instruction_count,
		VitaIOP::WaitLoopDescriptor* descriptor)
	{
		if (!descriptor || instruction_count < 2)
			return false;
		*descriptor = {};

		const u32 branch_index = instruction_count - 2;
		const u32 branch_pc = start_pc + branch_index * 4;
		const u32 branch_op = iopMemRead32(branch_pc);
		for (u32 i = 0; i < instruction_count; i++)
		{
			if (i != branch_index && iopMemRead32(start_pc + i * 4) != 0)
				return false;
		}

		const u32 primary = branch_op >> 26;
		descriptor->cycles = instruction_count;
		descriptor->rs = static_cast<u8>(RS(branch_op));
		descriptor->rt = static_cast<u8>(RT(branch_op));
		switch (primary)
		{
			case 0x01:
				if (BranchTarget(branch_pc, branch_op) != start_pc)
					return false;
				switch (RT(branch_op))
				{
					case 0x00:
						descriptor->condition = VitaIOP::WaitLoopCondition::LessThanZero;
						break;
					case 0x01:
						descriptor->condition = VitaIOP::WaitLoopCondition::GreaterEqualZero;
						break;
					case 0x10:
						descriptor->condition = VitaIOP::WaitLoopCondition::LessThanZero;
						descriptor->writes_link = true;
						break;
					case 0x11:
						descriptor->condition = VitaIOP::WaitLoopCondition::GreaterEqualZero;
						descriptor->writes_link = true;
						break;
					default:
						return false;
				}
				break;

			case 0x02:
			case 0x03:
				if (JumpTarget(branch_pc, branch_op) != start_pc)
					return false;
				descriptor->condition = VitaIOP::WaitLoopCondition::Always;
				descriptor->writes_link = primary == 0x03;
				break;

			case 0x04:
			case 0x05:
			case 0x06:
			case 0x07:
				if (BranchTarget(branch_pc, branch_op) != start_pc)
					return false;
				descriptor->condition =
					primary == 0x04 ? VitaIOP::WaitLoopCondition::Equal : primary == 0x05 ? VitaIOP::WaitLoopCondition::NotEqual :
					primary == 0x06 ? VitaIOP::WaitLoopCondition::LessEqualZero :
					VitaIOP::WaitLoopCondition::GreaterThanZero;
				break;

			default:
				return false;
		}

		return true;
	}

	constexpr size_t GprOffset(unsigned guest_reg)
	{
		return GPR_OFFSET + guest_reg * sizeof(u32);
	}

	void ComputeIopMultiplyResult(u32 lhs, u32 rhs, bool is_signed, u32* lo,
		u32* hi)
	{
		// PCSX2 owners: R3000AOpcodeTables.cpp::psxMULT()/psxMULTU().
		const u64 result =
			is_signed ? static_cast<u64>(static_cast<s64>(static_cast<s32>(lhs)) *
							 static_cast<s64>(static_cast<s32>(rhs))) :
			(static_cast<u64>(lhs) * static_cast<u64>(rhs));
		*lo = static_cast<u32>(result);
		*hi = static_cast<u32>(result >> 32);
	}

	void ComputeIopDivideResult(u32 lhs, u32 rhs, bool is_signed, u32* lo,
		u32* hi)
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

	constexpr bool IsIopCop0StatusWriteOpcode(u32 op)
	{
		return (op >> 26) == 0x10 && (RS(op) == 0x04 || RS(op) == 0x06) &&
			RD(op) == 12;
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

	constexpr bool IsIopCop0RfeOpcode(u32 op)
	{
		return (op >> 26) == 0x10 && RS(op) == 0x10;
	}

	bool IopLogicalBlockHasPrivateCycleHelper(u32 start_pc,
		u32 instruction_count)
	{
		// PCSX2 owners: x86/iR3000A.cpp::rpsxSYSCALL/rpsxBREAK and
		// x86/iR3000Atables.cpp::rpsxRFE(). Their helpers run while
		// s_psxBlockCycles is still private. Native blocks containing one of
		// these operations therefore require deferred state. Nondeferred
		// validation controls retain the exact recompiler fallback.
		for (u32 i = 0; i < instruction_count; i++)
		{
			const u32* const opcode = ResolveIopRecOpcode(start_pc + i * 4);
			if (!opcode || IsIopExceptionOpcode(*opcode) ||
				IsIopCop0RfeOpcode(*opcode))
				return true;
		}
		return false;
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

	constexpr bool IopDelayInstructionPreservesHostFlags(u32 op)
	{
		// PCSX2 owners: x86/iR3000Atables.cpp::rpsxBEQ_process(),
		// rpsxBNE_process(), and the signed branch routines preserve separate
		// taken/fallthrough delay-slot state. On A32, a delay template proven not
		// to write CPSR can instead leave the branch comparison live until the
		// common delay slot has executed.
		// These native A32 templates use non-S data-processing operations (or
		// loads/stores) only. Compare-producing SLT/SLTU and helper/memory paths
		// are intentionally excluded. This lets a final branch consume CPSR after
		// its architectural delay slot without changing MIPS delay semantics.
		switch (op >> 26)
		{
			case 0x00: // SPECIAL
				switch (op & 0x3f)
				{
					case 0x00: // SLL (including NOP)
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
						return true;
					default:
						return false;
				}

			case 0x08: // ADDI
			case 0x09: // ADDIU
			case 0x0c: // ANDI
			case 0x0d: // ORI
			case 0x0e: // XORI
			case 0x0f: // LUI
				return true;

			default:
				return false;
		}
	}

	constexpr bool IopSetLessThanResultFeedsZeroBranch(u32 producer_op,
		u32 branch_op)
	{
		if ((producer_op >> 26) != 0x00)
			return false;

		const u32 funct = producer_op & 0x3f;
		if (funct != 0x2a && funct != 0x2b) // SLT/SLTU
			return false;

		const unsigned result = RD(producer_op);
		const unsigned branch_opcode = branch_op >> 26;
		return result != 0 && (branch_opcode == 0x04 || branch_opcode == 0x05) &&
			((RS(branch_op) == result && RT(branch_op) == 0) ||
				(RT(branch_op) == result && RS(branch_op) == 0));
	}

	constexpr bool IsIopStaticJumpOpcode(u32 op)
	{
		return (op >> 26) == 0x02 || (op >> 26) == 0x03; // J/JAL
	}

	constexpr bool IsIopRegisterJumpOpcode(u32 op)
	{
		return (op >> 26) == 0x00 &&
		       ((op & 0x3f) == 0x08 || (op & 0x3f) == 0x09); // JR/JALR
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

			if (IsIopStaticJumpOpcode(op) && final_branch_pair)
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
		// ALU, memory, COP, helper-backed, and rpsxNULL decode slots, then publishes
		// it at the actual branch/block exit. CanCompileOpcode() has already
		// established that every non-native word here is exactly that one-cycle,
		// no-runtime-semantics PCSX2 table entry.
		for (u32 i = 0; i < instruction_count; i++)
		{
			const u32 op = iopMemRead32(start_pc + i * 4);
			if (IsIopBranchOrJumpOpcode(op) && i + 1 < instruction_count)
			{
				const u32 delay_op = iopMemRead32(start_pc + (i + 1) * 4);
				if (IsIopBranchOrJumpOpcode(delay_op) || IsIopExceptionOpcode(delay_op))
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

	bool UsesIopRecompilerDirectLoadAlias(u32 op)
	{
		// PCSX2 owner: x86/iR3000Atables.cpp::rpsxLoad(). Only these aligned
		// scalar loads use its deliberately broad bit-28 RAM alias test. Every
		// other memory family reaches IopMem.cpp's 29-bit LUT mapping rules.
		switch (op >> 26)
		{
			case 0x20: // LB
			case 0x21: // LH
			case 0x23: // LW
			case 0x24: // LBU
			case 0x25: // LHU
				return true;
			default:
				return false;
		}
	}

	bool TryIopRecompilerDirectLoadAddress(u32 effective_address, u8 alignment_mask,
		u32* address)
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

	bool TryMappedIopRamEffectiveAddress(u32 effective_address, u8 alignment_mask,
		u32* address)
	{
		const u32 physical_address = effective_address & 0x1fffffffu;
		if (alignment_mask == 0xff || (effective_address & alignment_mask) != 0 ||
			physical_address >= Ps2MemSize::TotalIopRam)
		{
			return false;
		}

		if (address)
			*address = physical_address & (Ps2MemSize::ExposedIopRam - 1);
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

#if defined(VITASX2_QEMU_VALIDATION) || defined(VITASX2_PORTABLE_REPLAY_VALIDATION)
	extern "C" __attribute__((noinline)) bool
	VitaIopA32RunTracedEventTest()
	{
		// The product no-work path below is deliberately helper-free. CORE traces
		// also own iopEventTest()'s Enter/Exit records, so validation executes the
		// PCSX2 owner whenever that oracle is active instead of silently hiding an
		// otherwise-correct event boundary.
		if (!Pcsx2Trace::IsCoreEventTraceEnabled())
			return false;

		iopEventTest();
		return true;
	}
#endif

	inline __attribute__((always_inline)) u32 GetPublishedIopEventCountdown()
	{
		const u64 cycle = psxRegs.cycle;
		const u64 deadline = psxRegs.iopNextEventCycle;
		// PCSX2's x86 owner uses SUB/JS, so even an arbitrary restored 64-bit
		// horizon is ordered by the sign of this wrapping difference rather
		// than by an unsigned <= comparison.
		if (static_cast<s64>(cycle - deadline) >= 0)
			return 0;
		const u64 distance = deadline - cycle;
		return distance <= INT32_MAX ? static_cast<u32>(distance) : UINT32_MAX;
	}

	extern "C" __attribute__((noinline)) u32
	VitaIopA32LoadPublishedEventCountdown()
	{
		return GetPublishedIopEventCountdown();
	}

	extern "C" __attribute__((noinline)) u32
	VitaIopA32TestEventAndLoadPublishedCountdown()
	{
		if (static_cast<s64>(psxRegs.cycle - psxRegs.iopNextEventCycle) >= 0)
		{
#if defined(VITASX2_QEMU_VALIDATION)
			s_qemuIopBranchEventTestsEntered++;
#endif
			iopEventTest();
		}
		return GetPublishedIopEventCountdown();
	}

	extern "C" __attribute__((noinline)) bool
	VitaIopA32TraceInstruction(u32 pc, u32 opcode)
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

	template <int ClockMode>
	inline __attribute__((always_inline)) u32
	FastForwardCachedIopWaitLoopForClock(u32 loop_pc, u32 block_cycles)
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
		const u64 target_cycle =
			(candidate_minus_event & (1ull << 63)) == 0 ? next_event : budget_target;

		psxRegs.pc = loop_pc;
		psxRegs.cycle = target_cycle;
		const u64 iop_cycles = target_cycle - old_cycle;
		const u32 ee_cycles = static_cast<u32>(iop_cycles << 3);
		if constexpr (ClockMode == 0)
		{
			psxRegs.iopCycleEE =
				static_cast<s32>(static_cast<u32>(psxRegs.iopCycleEE) - ee_cycles);
		}
		else if constexpr (ClockMode == 1)
		{
			// Dynamic iPsxAddEECycles(0xffffffff) receives delta << 3 in EAX.
			const u32 numerator = ee_cycles + psxRegs.iopCycleEECarry;
			psxRegs.iopCycleEECarry = numerator % 147u;
			psxRegs.iopCycleEE = static_cast<s32>(static_cast<u32>(psxRegs.iopCycleEE) -
												  (numerator / 147u));
		}
		else if ((psxHu32(HW_ICFG) & (1u << 3)) == 0)
		{
			psxRegs.iopCycleEE =
				static_cast<s32>(static_cast<u32>(psxRegs.iopCycleEE) - ee_cycles);
		}
		else
		{
			const u32 numerator = ee_cycles + psxRegs.iopCycleEECarry;
			psxRegs.iopCycleEECarry = numerator % 147u;
			psxRegs.iopCycleEE = static_cast<s32>(static_cast<u32>(psxRegs.iopCycleEE) -
												  (numerator / 147u));
		}

#if defined(VITASX2_QEMU_VALIDATION)
		VitaRecordA32IopWaitLoopFastForward(iop_cycles, block_cycles);
#endif
		if (psxRegs.iopCycleEE > 0)
			iopEventTest();

		return static_cast<u32>(VitaIOP::BlockExitKind::Direct);
	}

	inline __attribute__((always_inline)) u32
	FastForwardCachedIopWaitLoop(u32 loop_pc, u32 block_cycles)
	{
		return FastForwardCachedIopWaitLoopForClock<-1>(loop_pc, block_cycles);
	}

	extern "C" __attribute__((noinline)) u32
	VitaIopA32FastForwardWaitLoop(u32 loop_pc, u32 block_cycles)
	{
		// Standalone generated diagnostics retain a callable AAPCS seam. Cached
		// provider entries inline the same PCSX2-owned body into the private
		// dispatcher so its already-saved r4-r11 frame owns the event call too.
		asm volatile("" ::: "memory");
		return FastForwardCachedIopWaitLoop(loop_pc, block_cycles);
	}

	inline __attribute__((always_inline)) u32
	FastForwardProviderIopWaitLoop(u32 loop_pc, u32 block_cycles)
	{
#if defined(VITASX2_QEMU_VALIDATION)
		if (!s_qemuIopInlineWaitFastForwardEnabled)
			return VitaIopA32FastForwardWaitLoop(loop_pc, block_cycles);
		s_qemuIopInlineWaitFastForwards++;
#endif
		return FastForwardCachedIopWaitLoop(loop_pc, block_cycles);
	}

	template <bool Ps1Clock>
	inline __attribute__((always_inline)) u32
	FastForwardProviderIopWaitLoopForClock(u32 loop_pc, u32 block_cycles)
	{
#if defined(VITASX2_QEMU_VALIDATION)
		if (!s_qemuIopInlineWaitFastForwardEnabled)
			return VitaIopA32FastForwardWaitLoop(loop_pc, block_cycles);
		s_qemuIopInlineWaitFastForwards++;
#endif
		return FastForwardCachedIopWaitLoopForClock < Ps1Clock ? 1 : 0 > (loop_pc, block_cycles);
	}

	extern "C" __attribute__((noinline)) void VitaIopA32RaiseException(u32 pc,
		u32 code)
	{
		// PCSX2 owner: R3000AOpcodeTables.cpp::psxSYSCALL()/psxBREAK()
		// subtract the pre-incremented pc before entering R3000A.cpp::psxException().
		// Native exception opcodes are admitted only outside a branch delay slot;
		// the path-dependent x86 delay copies retain their exact fallback.
		psxRegs.pc = pc;
		psxException(code, false);
	}

	extern "C" __attribute__((noinline)) u64 VitaIopA32DivResult(u32 rs_value,
		u32 rt_value)
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
			lo = static_cast<u32>(static_cast<s32>(rs_value) /
								  static_cast<s32>(rt_value));
			hi = static_cast<u32>(static_cast<s32>(rs_value) %
								  static_cast<s32>(rt_value));
		}

		return (static_cast<u64>(hi) << 32) | lo;
	}

	extern "C" __attribute__((noinline)) u64 VitaIopA32DivuResult(u32 rs_value,
		u32 rt_value)
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

	size_t AlignUp(size_t value, size_t alignment)
	{
		return (value + alignment - 1) & ~(alignment - 1);
	}

} // namespace

namespace VitaIOP
{
	BlockCompiler::BlockCompiler(VitaA32::CodeBuffer& code,
		const u32* ram_source_page_live_counts,
		const u8* ram_source_page_live_flags,
		const u8* ram_source_chunk_live_flags,
		bool source_page_literal_allowed)
		: m_code(code)
		, m_ram_source_page_live_counts(ram_source_page_live_counts)
		, m_ram_source_page_live_flags(ram_source_page_live_flags)
		, m_ram_source_chunk_live_flags(ram_source_chunk_live_flags)
		, m_source_page_literal_allowed(source_page_literal_allowed)
	{
	}

	bool BlockCompiler::CanCompileOpcode(u32 op)
	{
		// PCSX2 owner: R3000AOpcodeTables.cpp::psxBSC plus the SPECIAL/REGIMM/
		// COP0/COP2 subtables. Every architectural slot has a native A32 path
		// below. The remaining decoded slots are PCSX2 rpsxNULL(), which logs once
		// at compilation and emits no semantic host instruction.
#if defined(VITASX2_QEMU_VALIDATION)
		if (!s_qemuIopNullOpcodeCompilationEnabled && !IsNativeOpcode(op))
			return false;
#else
		(void)op;
#endif
		return true;
	}

	bool BlockCompiler::BeginBlock(u32 start_pc, size_t* linked_entry_offset,
		size_t* provider_entry_offset)
	{
		if (linked_entry_offset)
			*linked_entry_offset = 0;
		if (provider_entry_offset)
			*provider_entry_offset = 0;
		m_resident_contract = {};
		m_scalar_load_cold_tails.clear();
		m_scalar_store_cold_tails.clear();
		m_unaligned_read_cold_tails.clear();
		m_unaligned_write_cold_tails.clear();
		m_cop2_load_cold_tails.clear();
		m_cop2_store_cold_tails.clear();
		// Trace blocks call out before every cycle increment. Pure production
		// blocks batch cycles once at the tail. Runtime IOP RAM masking also
		// uses r10, but known direct-RAM blocks only need the r11 base pointer.
		m_iop_cycle_base_register_available = !m_iop_ram_mask_register_available &&
		                                      !m_emit_trace_checks &&
		                                      !m_defer_cycle_updates;
		// Nondeferred validation controls snapshot the target's entry cycle at
		// [sp, #0]. That value is block-local and cannot be inherited from a
		// predecessor, so they deliberately have no resident entry contract.
		m_resident_contract.base_entry.domain = m_defer_cycle_updates ?
			VitaRegion::GuestDomain::Iop : VitaRegion::GuestDomain::None;
		m_resident_contract.base_entry.Bind(HOST_PSX_REGS,
			VitaRegion::ResidentValue::CoreStateBase);
		if (m_resident_event_deadline)
		{
			m_resident_contract.base_entry.Bind(
				HOST_REGISTER_JUMP_TARGET,
				VitaRegion::ResidentValue::IopPublishedEventCountdown);
		}
		if (m_resident_ee_budget)
		{
			m_resident_contract.base_entry.Bind(
				HOST_SAVED0, VitaRegion::ResidentValue::IopEeBudget);
		}
		if (m_iop_cycle_base_register_available)
		{
			m_resident_contract.base_entry.Bind(HOST_CYCLE_BASE,
				VitaRegion::ResidentValue::CycleStateBase);
		}
		else if (m_iop_ram_mask_register_available)
		{
			m_resident_contract.base_entry.Bind(HOST_IOP_RAM_MASK,
				VitaRegion::ResidentValue::MainMemoryMask);
		}
		if (m_iop_ram_registers_available)
		{
			m_resident_contract.base_entry.Bind(HOST_IOP_RAM_BASE,
				VitaRegion::ResidentValue::MainMemoryBase);
		}
		m_resident_gpr_contract_safe =
			m_resident_contract.base_entry.domain != VitaRegion::GuestDomain::None &&
			!m_compiled_ps1_bios_gate &&
			!(m_irx_import_hle || m_irx_import_debug ||
				(m_irx_import_log && m_irx_import_funcname));
#if defined(VITASX2_QEMU_VALIDATION)
		m_resident_gpr_contract_safe =
			m_resident_gpr_contract_safe && s_qemuIopResidentGprLinksEnabled;
#endif
		// A GPR entry exists only when there is a guest value to inherit.
		// Publishing the base-only contract as a GPR entry leaves its offset at
		// zero, which is the callable adapter rather than the post-load body.
		// A dirty predecessor could then patch its first canonical store straight
		// into a second PUSH/SUB frame and bypass the store. This became common
		// when the published-event countdown reserved r8 and reduced the pin set.
		if (m_resident_gpr_contract_safe && m_pinned_gpr_count != 0)
		{
			m_resident_contract.gpr_entry = m_resident_contract.base_entry;
			for (u8 i = 0; i < m_pinned_gpr_count; i++)
			{
				const PinnedGpr& pin = m_pinned_gprs[i];
				m_resident_contract.gpr_entry.Bind(pin.host,
					VitaRegion::ResidentValue::GuestGprLow32, pin.guest);
			}
		}
		const u16 baseline_saved_registers =
			REG_R4 | REG_R5 | REG_R6 | REG_R7 | REG_R8 |
			((m_iop_cycle_base_register_available ||
				 m_iop_ram_mask_register_available) ?
					REG_R10 :
					0) |
			(m_iop_ram_registers_available ? REG_R11 : 0);
		bool narrow_saved_registers = true;
#if defined(VITASX2_QEMU_VALIDATION)
		narrow_saved_registers = s_qemuIopSavedRegisterNarrowingEnabled;
#endif
		m_saved_registers = narrow_saved_registers ? m_required_saved_registers : baseline_saved_registers;
		if ((m_saved_registers & ~baseline_saved_registers) != 0)
			return false;
		m_saved_register_stack_words_removed =
			narrow_saved_registers ? 2u * (static_cast<u32>(__builtin_popcount(
											   static_cast<unsigned>(baseline_saved_registers))) -
											  static_cast<u32>(__builtin_popcount(
												  static_cast<unsigned>(m_saved_registers)))) :
									 0;

		const u16 pushed_registers = m_saved_registers | REG_LR;
		const u32 pushed_count = static_cast<u32>(
			__builtin_popcount(static_cast<unsigned>(pushed_registers)));
		const u32 baseline_pushed_count = static_cast<u32>(__builtin_popcount(
			static_cast<unsigned>(baseline_saved_registers | REG_LR)));
		// PCSX2's s_psxBlockCycles remains private through the complete logical
		// BaseBlock, including every memory/helper call. Deferred blocks therefore
		// need no cycle-path stack word; only the nondeferred validation control
		// keeps its entry-cycle snapshot so the final tail can derive its published
		// delta for EE-budget charging.
		const bool needs_cycle_stack_word =
			!m_defer_cycle_updates;
		if (!needs_cycle_stack_word)
			m_stack_frame_size = (pushed_count & 1u) ? 4 : 0;
		else
			m_stack_frame_size = (pushed_count & 1u) ? 4 : 8;
		const u8 baseline_stack_frame_size =
			!needs_cycle_stack_word ? ((baseline_pushed_count & 1u) ? 4 : 0) : ((baseline_pushed_count & 1u) ? 4 : 8);
		m_saved_register_frame_instructions_added =
			(baseline_stack_frame_size == 0 && m_stack_frame_size != 0) ? 2 : 0;
		m_saved_register_frame_instructions_removed =
			(baseline_stack_frame_size != 0 && m_stack_frame_size == 0) ? 2 : 0;
		// A standalone BlockCompiler retains a correctness/diagnostic callable
		// adapter. Executor-managed blocks provide their private-entry output and
		// omit these cold 24 bytes from the product code cache entirely.
		// The adapter owns one conservative frame for the whole linked chain,
		// just like PCSX2's _DynGen_EnterRecompiledCode(). Nine pushed words plus
		// twelve private bytes preserve AAPCS alignment and leave [sp, #0]
		// available to every generated body.
		constexpr u16 callable_saved_registers =
			REG_R4 | REG_R5 | REG_R6 | REG_R7 | REG_R8 | REG_R9 | REG_R10 | REG_R11;
		constexpr u8 callable_stack_frame_size = 12;
		bool needs_callable_adapter = true;
#if defined(__arm__)
		needs_callable_adapter = (provider_entry_offset == nullptr);
#endif
		size_t callable_body = static_cast<size_t>(-1);
		if (needs_callable_adapter &&
			(!m_code.EmitPush(callable_saved_registers | REG_LR) ||
			 !m_code.EmitSubImm8(HOST_SP, HOST_SP, callable_stack_frame_size) ||
			 // At this instruction PC reads as the continuation two words ahead.
			 !m_code.EmitAddImm8(HOST_CHAIN_RETURN, 15, 0) ||
				(callable_body = m_code.EmitBranchPlaceholder()) ==
					static_cast<size_t>(-1) ||
			 !m_code.EmitAddImm8(HOST_SP, HOST_SP, callable_stack_frame_size) ||
			 !m_code.EmitPop(callable_saved_registers | REG_PC)))
		{
			return false;
		}

#if defined(VITASX2_QEMU_VALIDATION) || defined(VITASX2_CPU_PROFILER)
		// Internal entries need parallel diagnostic-only markers because their
		// link bypasses the ordinary entry. Product code points directly at each
		// body and emits none of this instrumentation.
		size_t resident_body_branch = static_cast<size_t>(-1);
		size_t resident_gpr_body_branch = static_cast<size_t>(-1);
		const auto emit_resident_marker =
			[&](size_t* entry_offset, size_t* body_branch) {
				*entry_offset = m_code.Size();
#if defined(VITASX2_QEMU_VALIDATION)
				const u32 frame_instructions =
					2u + (m_stack_frame_size != 0 ? 2u : 0u);
				const u32 stack_words =
					2u * static_cast<u32>(__builtin_popcount(
							 static_cast<unsigned>(m_saved_registers | REG_LR)));
				if (!m_code.EmitMovImm32(HOST_CALL_SCRATCH,
						static_cast<u32>(reinterpret_cast<uptr>(
							&s_qemuIopLinkedFrameEvidence))) ||
					!m_code.EmitLdrImm12(HOST_TMP0, HOST_CALL_SCRATCH, 0) ||
					!m_code.EmitAddImm8(HOST_TMP0, HOST_TMP0, 1) ||
					!m_code.EmitStrImm12(HOST_TMP0, HOST_CALL_SCRATCH, 0) ||
					!m_code.EmitLdrImm12(
						HOST_TMP0, HOST_CALL_SCRATCH, sizeof(u32)) ||
					!m_code.EmitAddImm8(HOST_TMP0, HOST_TMP0,
						static_cast<u8>(frame_instructions)) ||
					!m_code.EmitStrImm12(
						HOST_TMP0, HOST_CALL_SCRATCH, sizeof(u32)) ||
					!m_code.EmitLdrImm12(
						HOST_TMP0, HOST_CALL_SCRATCH, 2 * sizeof(u32)) ||
					!m_code.EmitAddImm8(HOST_TMP0, HOST_TMP0,
						static_cast<u8>(stack_words)) ||
					!m_code.EmitStrImm12(
						HOST_TMP0, HOST_CALL_SCRATCH, 2 * sizeof(u32)))
				{
					return false;
				}
#endif
#if defined(VITASX2_CPU_PROFILER)
				static_assert(sizeof(std::atomic<u32>) == sizeof(u32));
				static_assert(alignof(std::atomic<u32>) >= alignof(u32));
				if (!m_code.EmitMovImm32(HOST_TMP0,
						static_cast<u32>(reinterpret_cast<uptr>(
							&VitaPerformanceTelemetry::g_cpu_iop_statistical_pc))) ||
					!m_code.EmitMovImm32(HOST_TMP1, start_pc) ||
					!m_code.EmitStrImm12(HOST_TMP1, HOST_TMP0, 0))
				{
					return false;
				}
#endif
				*body_branch = m_code.EmitBranchPlaceholder();
				return *body_branch != static_cast<size_t>(-1);
			};
		if (!emit_resident_marker(
				&m_resident_contract.base_entry_offset,
				&resident_body_branch) ||
			(m_resident_gpr_contract_safe && m_pinned_gpr_count != 0 &&
				!emit_resident_marker(
					&m_resident_contract.gpr_entry_offset,
					&resident_gpr_body_branch)))
		{
			return false;
		}
#endif

		// PCSX2's _DynGen_EnterRecompiledCode() owns one private frame around a
		// linked chain. Vita keeps narrow callable frames, but an exact frame
		// signature can enter here after the PUSH/SUB and unwind only once at the
		// eventual event/budget exit.
#if defined(VITASX2_QEMU_VALIDATION)
		if (linked_entry_offset)
		{
			*linked_entry_offset = m_code.Size();
			const u32 frame_instructions = 2u + (m_stack_frame_size != 0 ? 2u : 0u);
			const u32 stack_words =
				2u * static_cast<u32>(__builtin_popcount(
						 static_cast<unsigned>(m_saved_registers | REG_LR)));
			if (!m_code.EmitMovImm32(HOST_CALL_SCRATCH,
					static_cast<u32>(reinterpret_cast<uptr>(
						&s_qemuIopLinkedFrameEvidence))) ||
				!m_code.EmitLdrImm12(HOST_TMP0, HOST_CALL_SCRATCH, 0) ||
				!m_code.EmitAddImm8(HOST_TMP0, HOST_TMP0, 1) ||
				!m_code.EmitStrImm12(HOST_TMP0, HOST_CALL_SCRATCH, 0) ||
				!m_code.EmitLdrImm12(HOST_TMP0, HOST_CALL_SCRATCH, sizeof(u32)) ||
				!m_code.EmitAddImm8(HOST_TMP0, HOST_TMP0,
					static_cast<u8>(frame_instructions)) ||
				!m_code.EmitStrImm12(HOST_TMP0, HOST_CALL_SCRATCH, sizeof(u32)) ||
				!m_code.EmitLdrImm12(HOST_TMP0, HOST_CALL_SCRATCH, 2 * sizeof(u32)) ||
				!m_code.EmitAddImm8(HOST_TMP0, HOST_TMP0,
					static_cast<u8>(stack_words)) ||
				!m_code.EmitStrImm12(HOST_TMP0, HOST_CALL_SCRATCH, 2 * sizeof(u32)))
			{
				return false;
			}
		}
#else
		if (linked_entry_offset)
			*linked_entry_offset = m_code.Size();
#endif
#if defined(VITASX2_CPU_PROFILER)
		if (linked_entry_offset)
		{
			// RunProviderBlockInline() publishes ordinary provider entries in
			// C++, but a patched A32 edge enters here without returning to it.
			// Mirror std::atomic<u32>::store(relaxed) exactly as the EE compiler
			// does, before provider_entry_offset, so only direct links pay for
			// this diagnostic marker. Normal product builds emit nothing.
			static_assert(sizeof(std::atomic<u32>) == sizeof(u32));
			static_assert(alignof(std::atomic<u32>) >= alignof(u32));
			if (!m_code.EmitMovImm32(HOST_TMP0,
					static_cast<u32>(reinterpret_cast<uptr>(
						&VitaPerformanceTelemetry::g_cpu_iop_statistical_pc))) ||
				!m_code.EmitMovImm32(HOST_TMP1, start_pc) ||
				!m_code.EmitStrImm12(HOST_TMP1, HOST_TMP0, 0))
			{
				return false;
			}
		}
#else
		(void)start_pc;
#endif
		if (provider_entry_offset)
			*provider_entry_offset = m_code.Size();
		if (needs_callable_adapter &&
			!m_code.PatchBranch(callable_body, m_code.Size()))
			return false;

		const size_t setup_start = m_code.Size();
		if (!m_code.EmitMovImm32(HOST_PSX_REGS,
				static_cast<u32>(reinterpret_cast<uptr>(&psxRegs))))
			return false;
		if (!m_defer_cycle_updates &&
			(!m_code.EmitLdrImm12(HOST_TMP0, HOST_PSX_REGS, CYCLE_OFFSET) ||
				!m_code.EmitStrImm12(HOST_TMP0, HOST_SP, 0)))
		{
			return false;
		}

		if (m_iop_cycle_base_register_available &&
			!m_code.EmitAddImm32(HOST_CYCLE_BASE, HOST_PSX_REGS,
				static_cast<u32>(CYCLE_OFFSET)))
		{
			return false;
		}

		if (m_iop_ram_mask_register_available &&
			!m_code.EmitMovImm32(HOST_IOP_RAM_MASK, Ps2MemSize::ExposedIopRam - 1))
		{
			return false;
		}

		if (m_iop_ram_registers_available &&
			!m_code.EmitMovImm32(
				HOST_IOP_RAM_BASE,
				static_cast<u32>(reinterpret_cast<uptr>(iopMem->Main))))
		{
			return false;
		}
		if (m_resident_event_deadline && !EmitReloadPublishedEventCountdown())
			return false;
		if (m_resident_ee_budget &&
			!m_code.EmitLdrImm12(HOST_SAVED0, HOST_PSX_REGS,
				static_cast<u16>(IOP_CYCLE_EE_OFFSET)))
		{
			return false;
		}
		const size_t resident_body = m_code.Size();
#if defined(VITASX2_QEMU_VALIDATION) || defined(VITASX2_CPU_PROFILER)
		if (resident_body_branch != static_cast<size_t>(-1) &&
			!m_code.PatchBranch(resident_body_branch, resident_body))
		{
			return false;
		}
#else
		m_resident_contract.base_entry_offset = resident_body;
#endif
		const size_t setup_bytes = resident_body - setup_start;
		if ((setup_bytes & 3u) != 0 || setup_bytes / sizeof(u32) > UINT8_MAX)
			return false;
		m_resident_contract.base_setup_instruction_count =
			static_cast<u8>(setup_bytes / sizeof(u32));
		for (u8 i = 0; i < m_pinned_gpr_count; i++)
		{
			const PinnedGpr& pin = m_pinned_gprs[i];
			// A compiled BIOS gate runs before guest state becomes private. Its
			// false arm performs this initial load after the helper; its true arm
			// returns without loading state which no guest instruction consumes.
			if (pin.needs_initial_load && !m_compiled_ps1_bios_gate)
			{
				if (!m_code.EmitLdrImm12(pin.host, HOST_PSX_REGS,
						static_cast<u16>(GprOffset(pin.guest))))
				{
					return false;
				}
				m_pinned_gpr_initial_loads++;
				m_resident_contract.gpr_entry_load_instruction_count++;
			}
		}
#if defined(VITASX2_QEMU_VALIDATION) || defined(VITASX2_CPU_PROFILER)
		if (resident_gpr_body_branch != static_cast<size_t>(-1) &&
			!m_code.PatchBranch(resident_gpr_body_branch, m_code.Size()))
		{
			return false;
		}
#else
		if (m_resident_gpr_contract_safe && m_pinned_gpr_count != 0)
			m_resident_contract.gpr_entry_offset = m_code.Size();
#endif
		return true;
	}

	void BlockCompiler::FinalizeResidentExitContract()
	{
		// Callee-saved core/RAM bases, and the countdown after its exact
		// block-cycle rebase, are valid at every ordinary linked exit whether or
		// not this block can also carry guest GPRs. Keep the base proof
		// independent from the stricter pinned-GPR proof so disabling or
		// rejecting GPR residency does not also disable safe prelude links.
		m_resident_contract.exit = m_resident_contract.base_entry;
		m_resident_contract.dirty_gpr_host_mask = 0;
		if (!m_resident_gpr_contract_safe)
			return;

		for (u8 i = 0; i < m_pinned_gpr_count; i++)
		{
			const PinnedGpr& pin = m_pinned_gprs[i];
			m_resident_contract.exit.Bind(pin.host,
				VitaRegion::ResidentValue::GuestGprLow32, pin.guest);
			if (pin.written)
				m_resident_contract.dirty_gpr_host_mask |=
					static_cast<u16>(1u << pin.host);
		}
	}

	bool BlockCompiler::EndBlockReturn(BlockExitKind exit, bool charge_budget,
		bool flush_pins, u32 known_cycle_count)
	{
		if ((flush_pins && !EmitFlushPinnedGprs()) ||
			(charge_budget && !EmitChargeEeBudget(known_cycle_count)) ||
			!EmitPublishResidentEeBudget())
			return false;

		return m_code.EmitMovImm32(HOST_TMP0, static_cast<u32>(exit)) &&
			   m_code.EmitBx(HOST_CHAIN_RETURN);
	}

	bool BlockCompiler::EmitCompiledPs1BiosGate()
	{
		// PCSX2 owner: x86/iR3000A.cpp::iopRecRecompile() emits this helper
		// only for PS1-clock blocks beginning at the A0/B0/C0 BIOS vectors. A
		// true return goes directly back to the dispatcher before guest work or
		// cycle charging; a false return continues with helper-observable GPRs
		// reloaded into the block's private pins.
		if (!m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&psxBiosCall),
				HOST_CALL_SCRATCH) ||
			!m_code.EmitCmpImm32(HOST_TMP0, 0))
		{
			return false;
		}

		const size_t continue_block =
			m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
		if (continue_block == static_cast<size_t>(-1) ||
			!EndBlockReturn(BlockExitKind::LogicalContinuation, false, false))
		{
			return false;
		}

		const size_t continue_offset = m_code.Size();
		if (!m_code.PatchBranch(continue_block, continue_offset,
				VitaA32::Condition::EQ))
		{
			return false;
		}

		for (u8 i = 0; i < m_pinned_gpr_count; i++)
		{
			const PinnedGpr& pin = m_pinned_gprs[i];
			if (pin.needs_initial_load &&
				!m_code.EmitLdrImm12(pin.host, HOST_PSX_REGS,
					static_cast<u16>(GprOffset(pin.guest))))
			{
				return false;
			}
			if (pin.needs_initial_load)
				m_pinned_gpr_initial_loads++;
		}
		return true;
	}

	bool BlockCompiler::EndBlockIsolateModeWriteReturn(bool charge_budget,
		bool flush_pins,
		u32 known_cycle_count)
	{
		if ((flush_pins && !EmitFlushPinnedGprs()) ||
			(charge_budget && !EmitChargeEeBudget(known_cycle_count)) ||
			!m_code.EmitLdrImm12(HOST_TMP0, HOST_PSX_REGS,
				static_cast<u16>(CP0_STATUS_OFFSET)) ||
			!m_code.EmitTstImm32(HOST_TMP0, 0x10000u))
		{
			return false;
		}

		const VitaA32::Condition changed_condition =
			m_isolate_cache_active ? VitaA32::Condition::EQ : VitaA32::Condition::NE;
		const size_t changed = m_code.EmitBranchPlaceholder(changed_condition);
		if (changed == static_cast<size_t>(-1) ||
			!EndBlockReturn(BlockExitKind::Direct, false, false))
		{
			return false;
		}

		const size_t changed_target = m_code.Size();
		return m_code.PatchBranch(changed, changed_target, changed_condition) &&
			EndBlockReturn(BlockExitKind::IsolateModeWrite, false, false);
	}

	bool BlockCompiler::EndBlockLogicalContinuationReturn(bool charge_budget,
		bool flush_pins, u32 known_cycle_count)
	{
		if ((flush_pins && !EmitFlushPinnedGprs()) ||
			(charge_budget && !EmitChargeEeBudget(
				known_cycle_count, true, UINT8_MAX, false)))
		{
			return false;
		}
		if (!m_writes_isolate_mode)
			return EndBlockReturn(BlockExitKind::LogicalContinuation, false, false);

		if (!m_code.EmitLdrImm12(HOST_TMP0, HOST_PSX_REGS,
				static_cast<u16>(CP0_STATUS_OFFSET)) ||
			!m_code.EmitTstImm32(HOST_TMP0, 0x10000u))
		{
			return false;
		}
		const VitaA32::Condition changed_condition =
			m_isolate_cache_active ? VitaA32::Condition::EQ : VitaA32::Condition::NE;
		const size_t changed = m_code.EmitBranchPlaceholder(changed_condition);
		if (changed == static_cast<size_t>(-1) ||
			!EndBlockReturn(BlockExitKind::LogicalContinuation, false, false))
		{
			return false;
		}
		const size_t changed_target = m_code.Size();
		return m_code.PatchBranch(changed, changed_target, changed_condition) &&
		       EndBlockReturn(BlockExitKind::LogicalContinuationIsolateModeWrite,
				   false, false);
	}

	bool BlockCompiler::EndBlockDirectTail(const void* direct_exit,
		DirectLinkSlot* direct_link_slot,
		u8 direct_link_slot_index,
		bool charge_budget, bool test_budget,
		BlockExitKind fallback_exit)
	{
		if (!direct_exit || direct_link_slot_index >= 2)
			return false;

		// Preserve dirty pins until the signed budget test. Its cold LE exit owns
		// the canonical flush, while a compatible linked edge can replace the
		// first ordinary STR with a direct branch and skip every source store.
		if (charge_budget &&
				!EmitChargeEeBudget(0, false,
					test_budget ? direct_link_slot_index : UINT8_MAX,
					test_budget))
		{
			return false;
		}

		const size_t flush_offset = m_code.Size();
		if (!EmitFlushPinnedGprs())
			return false;
		const u8 flush_count = static_cast<u8>(
			(m_code.Size() - flush_offset) / sizeof(u32));
		u32 flush_instruction = 0;
		if (flush_count != 0 &&
			!m_code.ReadInstruction(flush_offset, &flush_instruction))
		{
			return false;
		}

		const size_t budget_publish_offset = m_code.Size();
		if (!EmitPublishResidentEeBudget())
			return false;
		u32 budget_publish_instruction = 0;
		if (m_resident_ee_budget &&
			!m_code.ReadInstruction(
				budget_publish_offset, &budget_publish_instruction))
		{
			return false;
		}

		const size_t target_offset = m_code.Size();
		const size_t target_branch = m_code.EmitBranchPlaceholder();
		if (target_branch == static_cast<size_t>(-1))
			return false;

		const size_t fallback_offset = m_code.Size();
		if (!m_code.PatchBranch(target_branch, fallback_offset) ||
			!m_code.EmitMovImm32(HOST_TMP0, static_cast<u32>(fallback_exit)) ||
			!m_code.EmitBx(HOST_CHAIN_RETURN))
		{
			return false;
		}
		(void)direct_exit;

		if (direct_link_slot)
		{
			direct_link_slot->target_offset = target_offset;
			direct_link_slot->fallback_offset = fallback_offset;
			direct_link_slot->logical_continuation =
				fallback_exit == BlockExitKind::LogicalContinuation;
			direct_link_slot->resident_gpr_bypass_offset =
				flush_count != 0 ? flush_offset : static_cast<size_t>(-1);
			direct_link_slot->resident_gpr_bypass_instruction =
				flush_instruction;
			direct_link_slot->resident_gpr_stores_removed = flush_count;
			direct_link_slot->resident_budget_bypass_offset =
				m_resident_ee_budget ? budget_publish_offset :
					static_cast<size_t>(-1);
			direct_link_slot->resident_budget_bypass_instruction =
				budget_publish_instruction;
		}
		return true;
	}

	bool BlockCompiler::EmitStoreCode(u32 op)
	{
		return m_code.EmitMovImm32(HOST_TMP0, op) &&
			   m_code.EmitStrImm12(HOST_TMP0, HOST_PSX_REGS, CODE_OFFSET);
	}

	bool BlockCompiler::EmitIsolateCacheGuard(size_t* isolated_branch)
	{
		if (!isolated_branch)
			return false;

		if (m_isolate_cache_specialization && m_isolate_cache_guard_stable &&
			!m_isolate_cache_active)
		{
			*isolated_branch = static_cast<size_t>(-1);
			m_isolate_cache_guard_instructions_removed += 3;
			return true;
		}

		if (!m_code.EmitLdrImm12(HOST_TMP2, HOST_PSX_REGS,
				static_cast<u16>(CP0_STATUS_OFFSET)) ||
			!m_code.EmitTstImm32(HOST_TMP2, 0x10000u))
		{
			return false;
		}

		*isolated_branch = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
		return *isolated_branch != static_cast<size_t>(-1);
	}

	bool BlockCompiler::EmitWaitLoopFastForwardBlock(u32 start_pc,
		u32 block_cycles)
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
				   reinterpret_cast<const void*>(&VitaIopA32FastForwardWaitLoop),
				   HOST_CALL_SCRATCH) &&
			   m_code.EmitPop(REG_R4 | REG_PC);
	}

	bool BlockCompiler::EmitTraceCheck(u32 pc, u32 op,
		std::vector<size_t>& direct_exit_branches)
	{
		// Trace callbacks observe complete pre-instruction architectural state.
		// Publishing pins here keeps the diagnostic stream exact while production
		// blocks retain values until their real observable seam.
		if (!EmitFlushPinnedGprs() || !m_code.EmitMovImm32(HOST_TMP0, pc) ||
			!m_code.EmitMovImm32(HOST_TMP1, op) ||
			!m_code.EmitCallAbsolute(
				reinterpret_cast<const void*>(&VitaIopA32TraceInstruction)) ||
			!m_code.EmitCmpImm32(HOST_TMP0, 0))
		{
			return false;
		}

		direct_exit_branches.push_back(
			m_code.EmitBranchPlaceholder(VitaA32::Condition::NE));
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
		m_pinned_branch_operand_moves_removed = 0;
		m_pinned_gpr_min_exit_savings = UINT32_MAX;
#if defined(VITASX2_QEMU_VALIDATION)
		if (!s_qemuIopPinnedGprResidencyEnabled)
			return;
#endif
		std::array<u16, 32> scores{};
		u32 written_mask = 1;
		u32 needs_initial_mask = 0;
		u32 sequential_copy_result_mask = 0;
		bool supported = true;
		bool reserves_register_jump_host = false;
		bool reserve_sequential_copy_results =
			m_defer_cycle_updates &&
			!m_emit_trace_checks && m_isolate_cache_specialization &&
			m_isolate_cache_guard_stable && !m_isolate_cache_active &&
			m_iop_ram_registers_available && m_iop_ram_mask_register_available &&
			m_ram_source_page_live_flags &&
			Ps2MemSize::ExposedIopRam == Ps2MemSize::IopRam;
#if defined(VITASX2_QEMU_VALIDATION)
		reserve_sequential_copy_results = reserve_sequential_copy_results &&
			s_qemuIopSequentialQwordCopyEnabled &&
			s_qemuIopRamProvenanceSpecializationEnabled;
#endif
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
			SequentialQwordCopy sequential_copy;
			if (reserve_sequential_copy_results &&
				MatchSequentialQwordCopyShape(start_pc, i, instruction_count,
					&sequential_copy))
			{
				for (u32 result = 0; result < 4; result++)
					sequential_copy_result_mask |=
						1u << (sequential_copy.first_result + result);
			}
			if (IsIopBranchOrJumpOpcode(op) || IsIopExceptionOpcode(op))
			{
				const u32 delay_op =
					(i + 1 < instruction_count) ? iopMemRead32(pc + 4) : 0;
				const bool complete_delay_pair = i + 1 < instruction_count &&
					!IsIopBranchOrJumpOpcode(delay_op) &&
					!IsIopExceptionOpcode(delay_op);
				const bool final_delay_pair =
					i + 2 == instruction_count && complete_delay_pair;
				const bool path_specific_static_branch =
					IsIopStaticConditionalBranchOpcode(op) && complete_delay_pair;
				const bool native_static_jump =
					IsIopStaticJumpOpcode(op) && final_delay_pair;
				const bool native_register_jump =
					IsIopRegisterJumpOpcode(op) && final_delay_pair;
				if (native_register_jump)
					reserves_register_jump_host = true;
				if (!path_specific_static_branch && !native_static_jump &&
					!native_register_jump)
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
						case 0x00:
						case 0x02:
						case 0x03:
							read(RT(op));
							write(RD(op));
							break;
						case 0x04:
						case 0x06:
						case 0x07:
						case 0x20:
						case 0x21:
						case 0x22:
						case 0x23:
						case 0x24:
						case 0x25:
						case 0x26:
						case 0x27:
						case 0x2a:
						case 0x2b:
							read(RS(op));
							read(RT(op));
							write(RD(op));
							break;
						case 0x08: // JR
							read(RS(op));
							break;
						case 0x09: // JALR
							read(RS(op));
							write(RD(op));
							break;
						default:
							supported = false;
							break;
					}
					break;
				case 0x01:
					read(RS(op));
					supported =
						RT(op) == 0x00 || RT(op) == 0x01 || RT(op) == 0x10 || RT(op) == 0x11;
					break;
				case 0x02:
					break;
				case 0x03:
					write(31);
					break;
				case 0x04:
				case 0x05:
					read(RS(op));
					read(RT(op));
					break;
				case 0x06:
				case 0x07:
					read(RS(op));
					break;
				case 0x08:
				case 0x09:
				case 0x0a:
				case 0x0b:
				case 0x0c:
				case 0x0d:
				case 0x0e:
					read(RS(op));
					write(RT(op));
					break;
				case 0x0f:
					write(RT(op));
					break;
				case 0x20:
				case 0x21:
				case 0x23:
				case 0x24:
				case 0x25:
					read(RS(op));
					write(RT(op));
					break;
				case 0x28:
				case 0x29:
				case 0x2b:
					read(RS(op));
					read(RT(op));
					break;
				default:
					supported = false;
					break;
			}
		}

		if (!supported)
			return;

		constexpr std::array<u8, 2> pin_hosts = {HOST_SAVED1,
			HOST_REGISTER_JUMP_TARGET};
		const u8 pin_host_count =
			(reserves_register_jump_host || m_resident_event_deadline) ?
				1 : static_cast<u8>(pin_hosts.size());
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
				if (!already_pinned &&
					(sequential_copy_result_mask & (1u << guest)) == 0 &&
					scores[guest] > best_score)
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
			!m_iop_ram_mask_register_available && !m_emit_trace_checks &&
			!m_defer_cycle_updates;
		if (cycle_base_register_available || m_iop_ram_mask_register_available)
			m_required_saved_registers |= REG_R10;
		if (m_iop_ram_registers_available)
			m_required_saved_registers |= REG_R11;
		if (m_resident_event_deadline)
			m_required_saved_registers |= REG_R8;
		if (m_resident_ee_budget)
			m_required_saved_registers |= REG_R5;
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
			else if ((primary == 0x23 || primary == 0x28 || primary == 0x29 ||
						 primary == 0x2b) &&
				dynamic_address(primary == 0x28 ? 0 : (primary == 0x29 ? 1 : 3)))
			{
				m_required_saved_registers |= REG_R5;
			}
			else if ((primary == 0x22 || primary == 0x26 || primary == 0x2a ||
						 primary == 0x2e) &&
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
			const bool final_delay_pair = i + 2 == instruction_count &&
				!IsIopBranchOrJumpOpcode(delay_op) &&
				!IsIopExceptionOpcode(delay_op);
			const bool native_static_branch =
				IsIopStaticConditionalBranchOpcode(op) && final_delay_pair;
			const bool native_static_jump =
				IsIopStaticJumpOpcode(op) && final_delay_pair;
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
			if (m_isolate_cache_specialization && IsIopCop0StatusWriteOpcode(op))
			{
				m_writes_isolate_mode = true;
			}

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
		const u32 removed_memory_ops =
			m_pinned_gpr_load_hits + m_pinned_gpr_store_hits;
		const u32 added_memory_ops = m_pinned_gpr_initial_loads + dirty_pin_count;
		const u32 savings = removed_memory_ops > added_memory_ops ? removed_memory_ops - added_memory_ops : 0;
		m_pinned_gpr_min_exit_savings =
			std::min(m_pinned_gpr_min_exit_savings, savings);
	}

	bool BlockCompiler::EmitInterpreterTraceBranchHelperExit(const void* helper)
	{
		// The PCSX2 IOP instruction trace is interpreter-only. Its taken branch
		// executes the delay slot and event test inside psxDoBranch(), then charges
		// the EE budget when intExecuteBlock() regains control. Product blocks never
		// enter this helper: their scanner ends at the branch/delay pair and both
		// arms use the x86 recompiler's budget-before-event seam below.
		if (!m_emit_trace_checks)
			return false;

		RecordPinnedGprExitPathSavings();
		RecordBatchedCycleExitSavings(m_current_instruction_count, true);
		return EmitFlushPinnedGprs() &&
			m_code.EmitCallAbsolute(helper, HOST_CALL_SCRATCH) &&
		       (m_writes_isolate_mode ? EndBlockIsolateModeWriteReturn(true, false) : EndBlockReturn(BlockExitKind::Direct, true, false));
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

	void BlockCompiler::ClearKnownHiLo() { m_hilo_const_known_mask = 0; }

	bool BlockCompiler::TryKnownDirectIopRamAddress(u32 op, u8 alignment_mask,
		u32* address) const
	{
		u32 base = 0;
		if (!TryGetKnownGpr(RS(op), &base))
			return false;

		const u32 effective_address =
			base + static_cast<u32>(static_cast<s32>(IMM_S(op)));
		return UsesIopRecompilerDirectLoadAlias(op) ? TryIopRecompilerDirectLoadAddress(effective_address,
														  alignment_mask, address) :
		                                              TryMappedIopRamEffectiveAddress(effective_address,
														  alignment_mask, address);
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
						else if (rhs_known &&
								 (rhs == 1 || (signed_div && rhs == 0xffffffffu)))
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
					SetKnownGpr(
						rt, (static_cast<s32>(lhs) < static_cast<s32>(IMM_S(op))) ? 1u : 0u);
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

		const auto emit_add_to_loaded_cycle = [this, cycles](unsigned address_reg,
												  u8 offset) {
			const unsigned cycle_scratch =
				(address_reg == HOST_TMP2) ? HOST_TMP3 : HOST_TMP2;
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
			return emit_add_to_loaded_cycle(HOST_PSX_REGS,
				static_cast<u8>(CYCLE_OFFSET));

		if (m_code.EmitAddImm32(HOST_TMP2, HOST_PSX_REGS,
				static_cast<u32>(CYCLE_OFFSET)))
			return emit_add_to_loaded_cycle(HOST_TMP2, 0);

		if (!m_code.EmitLdrImm12(HOST_TMP0, HOST_PSX_REGS, CYCLE_OFFSET) ||
			!m_code.EmitLdrImm12(HOST_TMP1, HOST_PSX_REGS,
				CYCLE_OFFSET + sizeof(u32)))
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
		       m_code.EmitStrImm12(HOST_TMP1, HOST_PSX_REGS,
				   CYCLE_OFFSET + sizeof(u32));
	}

	bool BlockCompiler::EmitPublishCyclePrefix(u32 cycle_prefix)
	{
		// PCSX2 owner: x86/iR3000A.cpp::s_psxBlockCycles and
		// iPsxBranchTest(). Physical A32 fragments are one logical BaseBlock, so
		// only an owner exit commits the accumulated prefix. Memory helpers never
		// call this routine and continue to observe the BaseBlock-entry cycle.
		return EmitAddCycles(m_logical_cycle_prefix + cycle_prefix);
	}

	void BlockCompiler::RecordBatchedCycleExitSavings(u32 cycle_prefix,
		bool preserves_argument)
	{
		if (!m_expanded_cycle_batching)
			return;

		const bool ps1_clock_mode = (psxHu32(HW_ICFG) & (1u << 3)) != 0;
		s32 savings = 0;
		savings = static_cast<s32>(4u * cycle_prefix) +
			(ps1_clock_mode ? 0 : 1) - (preserves_argument ? 2 : 0);

		const u32 bounded_savings = savings > 0 ? static_cast<u32>(savings) : 0;
		if (m_batched_cycle_instructions_removed == UINT32_MAX)
			m_batched_cycle_instructions_removed = bounded_savings;
		else
			m_batched_cycle_instructions_removed =
				std::min(m_batched_cycle_instructions_removed, bounded_savings);
	}

	bool BlockCompiler::EmitChargeEeBudgetPs1(u32 known_block_cycles)
	{
		// PCSX2 owner: x86/iR3000A.cpp::iPsxAddEECycles(), PS1 clock mode.
		// high32(t * ceil(2^32 / 147)) is exact for every t below 72,796,163.
		// A logical block can be much longer than one 64-opcode A32 fragment, so
		// charge it in helper-free chunks of at most 56,871 IOP cycles. Carry is
		// threaded through canonical state and no guest/helper observation occurs
		// between chunks.
		constexpr u32 max_exact_cycle_chunk = 56871;
		const auto emit_chunk = [this](u32 cycles, bool dynamic_cycles) {
			return (dynamic_cycles || m_code.EmitMovImm32(HOST_TMP0, cycles)) &&
			       m_code.EmitMovRegShiftImm(HOST_TMP1, HOST_TMP0,
					   VitaA32::ShiftType::LSL, 10) &&
			       m_code.EmitAddRegShiftImm(HOST_TMP1, HOST_TMP1, HOST_TMP0,
					   VitaA32::ShiftType::LSL, 8) &&
			       m_code.EmitLdrImm12(HOST_TMP2, HOST_PSX_REGS,
					   static_cast<u16>(IOP_CYCLE_EE_CARRY_OFFSET)) &&
			m_code.EmitAddReg(HOST_TMP1, HOST_TMP1, HOST_TMP2) &&
			m_code.EmitMovImm32(HOST_TMP2, 0x01bdd2b9u) &&
			m_code.EmitUmull(HOST_TMP0, HOST_TMP3, HOST_TMP1, HOST_TMP2) &&
			       m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP3,
					   VitaA32::ShiftType::LSL, 7) &&
			       m_code.EmitAddRegShiftImm(HOST_TMP2, HOST_TMP2, HOST_TMP3,
					   VitaA32::ShiftType::LSL, 4) &&
			       m_code.EmitAddRegShiftImm(HOST_TMP2, HOST_TMP2, HOST_TMP3,
					   VitaA32::ShiftType::LSL, 1) &&
			m_code.EmitAddReg(HOST_TMP2, HOST_TMP2, HOST_TMP3) &&
			m_code.EmitSubReg(HOST_TMP1, HOST_TMP1, HOST_TMP2) &&
			       m_code.EmitStrImm12(HOST_TMP1, HOST_PSX_REGS,
					   static_cast<u16>(IOP_CYCLE_EE_CARRY_OFFSET)) &&
			       m_code.EmitLdrImm12(HOST_TMP0, HOST_PSX_REGS,
					   static_cast<u16>(IOP_CYCLE_EE_OFFSET)) &&
			m_code.EmitSubReg(HOST_TMP0, HOST_TMP0, HOST_TMP3, true) &&
			       m_code.EmitStrImm12(HOST_TMP0, HOST_PSX_REGS,
					   static_cast<u16>(IOP_CYCLE_EE_OFFSET));
		};

		if (known_block_cycles == 0)
			return emit_chunk(0, true);

		for (u32 remaining = known_block_cycles; remaining != 0;)
		{
			const u32 chunk = std::min(remaining, max_exact_cycle_chunk);
			if (!emit_chunk(chunk, false))
				return false;
			remaining -= chunk;
		}
		return true;
	}

	bool BlockCompiler::EmitChargeEeBudget(u32 known_cycle_count, bool pins_flushed,
		u8 scheduler_resume_slot,
		bool test_budget)
	{
		if (!m_direct_exit_branches || !m_budget_exit_branches ||
			!m_unflushed_budget_exit_branches)
			return false;
		if (scheduler_resume_slot != UINT8_MAX &&
			(scheduler_resume_slot >= 2 ||
			 !m_scheduler_budget_exit_branches[scheduler_resume_slot] ||
			 !m_unflushed_scheduler_budget_exit_branches[scheduler_resume_slot]))
		{
			return false;
		}
		m_has_budget_exit = m_has_budget_exit || test_budget;

		// PCSX2 owner: x86/iR3000A.cpp::iPsxAddEECycles() leaves the signed
		// budget subtraction flags live for iPsxBranchTest()'s xJLE. A32 STR
		// also preserves those flags, so branch on LE directly instead of
		// materializing and retesting a temporary Boolean.
		const auto emit_budget_exit_from_signed_flags = [this, pins_flushed,
															scheduler_resume_slot]() {
			std::vector<size_t>* exits = nullptr;
			if (scheduler_resume_slot != UINT8_MAX)
			{
				exits = pins_flushed ? m_scheduler_budget_exit_branches[scheduler_resume_slot] : m_unflushed_scheduler_budget_exit_branches[scheduler_resume_slot];
			}
			else
			{
				exits = pins_flushed ? m_budget_exit_branches : m_unflushed_budget_exit_branches;
			}
			exits->push_back(m_code.EmitBranchPlaceholder(VitaA32::Condition::LE));
			return exits->back() != static_cast<size_t>(-1);
		};

		const u32 known_block_cycles =
			known_cycle_count != 0 ? known_cycle_count : (m_defer_cycle_updates ? m_budget_cycle_count : 0);
		constexpr u32 max_exact_ps1_cycle_chunk = 56871;
		const auto finish_ps1_budget_test = [this, known_block_cycles, test_budget,
												&emit_budget_exit_from_signed_flags]() {
			if (!test_budget)
				return true;
			// Multiple exact reciprocal chunks leave flags for only the last
			// subtraction. Retest the modular final s32 value, matching x86's one
			// logical-block JLE rather than an artificial physical seam.
			return (known_block_cycles <= max_exact_ps1_cycle_chunk ||
					   m_code.EmitCmpImm32(HOST_TMP0, 0)) &&
			       emit_budget_exit_from_signed_flags();
		};
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
				       finish_ps1_budget_test();
			}

			if (m_resident_ee_budget)
			{
				const u32 ee_cycles = known_block_cycles * 8u;
				const bool subtracted =
					m_code.EmitSubImm32(
						HOST_SAVED0, HOST_SAVED0, ee_cycles, true) ||
					(m_code.EmitMovImm32(HOST_TMP2, ee_cycles) &&
					 m_code.EmitSubReg(
						 HOST_SAVED0, HOST_SAVED0, HOST_TMP2, true));
				return subtracted &&
				       (!test_budget || emit_budget_exit_from_signed_flags());
			}

			const bool emitted_ee_cycles =
				known_block_cycles != 0 ? m_code.EmitMovImm32(HOST_TMP2, known_block_cycles * 8) : m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP0, VitaA32::ShiftType::LSL, 3);
			return emitted_ee_cycles &&
			       m_code.EmitLdrImm12(HOST_TMP1, HOST_PSX_REGS,
					   static_cast<u16>(IOP_CYCLE_EE_OFFSET)) &&
				m_code.EmitSubReg(HOST_TMP1, HOST_TMP1, HOST_TMP2, true) &&
			       m_code.EmitStrImm12(HOST_TMP1, HOST_PSX_REGS,
					   static_cast<u16>(IOP_CYCLE_EE_OFFSET)) &&
			       (!test_budget || emit_budget_exit_from_signed_flags());
		}

		if (!m_code.EmitMovImm32(HOST_TMP2, static_cast<u32>(reinterpret_cast<uptr>(
												&iopHw[HW_ICFG & 0xffff]))) ||
			!m_code.EmitLdrImm12(HOST_TMP1, HOST_TMP2, 0) ||
			!m_code.EmitTstImm32(HOST_TMP1, 1u << 3))
		{
			return false;
		}

		const size_t ps1_clock_mode =
			m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
		if (ps1_clock_mode == static_cast<size_t>(-1))
			return false;

		const bool emitted_ee_cycles =
			known_block_cycles != 0 ? m_code.EmitMovImm32(HOST_TMP2, known_block_cycles * 8) : m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP0, VitaA32::ShiftType::LSL, 3);
		if (!emitted_ee_cycles ||
			!m_code.EmitLdrImm12(HOST_TMP1, HOST_PSX_REGS,
				static_cast<u16>(IOP_CYCLE_EE_OFFSET)) ||
			!m_code.EmitSubReg(HOST_TMP1, HOST_TMP1, HOST_TMP2, true) ||
			!m_code.EmitStrImm12(HOST_TMP1, HOST_PSX_REGS,
				static_cast<u16>(IOP_CYCLE_EE_OFFSET)) ||
			(test_budget && !emit_budget_exit_from_signed_flags()))
		{
			return false;
		}

		const size_t done = m_code.EmitBranchPlaceholder();
		if (done == static_cast<size_t>(-1))
			return false;

		const size_t ps1_clock_mode_target = m_code.Size();
		if (!m_code.PatchBranch(ps1_clock_mode, ps1_clock_mode_target,
				VitaA32::Condition::NE) ||
			!EmitChargeEeBudgetPs1(known_block_cycles) || !finish_ps1_budget_test())
		{
			return false;
		}

		return m_code.PatchBranch(done, m_code.Size());
	}

	bool BlockCompiler::EmitPublishResidentEeBudget()
	{
		// PCSX2 owner: x86/iR3000A.cpp::iPsxAddEECycles() keeps this value
		// canonical because the desktop dispatcher has no cross-block host
		// contract. Vita's private generated chain owns callee-saved r5 until a
		// real provider/observer exit. Compatible internal edges consume r5;
		// every generated fallback and exit publishes it exactly once.
		return !m_resident_ee_budget ||
		       m_code.EmitStrImm12(HOST_SAVED0, HOST_PSX_REGS,
				   static_cast<u16>(IOP_CYCLE_EE_OFFSET));
	}

	bool BlockCompiler::EmitPcChangedExitCheck(
		u32 expected_pc, std::vector<size_t>& direct_exit_branches)
	{
		if (!m_code.EmitLdrImm12(HOST_TMP0, HOST_PSX_REGS, PC_OFFSET))
			return false;

		if (!(m_code.EmitCmpImm32(HOST_TMP0, expected_pc) ||
				(m_code.EmitMovImm32(HOST_TMP1, expected_pc) &&
					m_code.EmitCmpReg(HOST_TMP0, HOST_TMP1))))
		{
			return false;
		}

		direct_exit_branches.push_back(
			m_code.EmitBranchPlaceholder(VitaA32::Condition::NE));
		return true;
	}

	bool BlockCompiler::EmitPcChangedExitCheckReg(
		unsigned expected_host_reg, std::vector<size_t>& direct_exit_branches)
	{
		if (!m_code.EmitLdrImm12(HOST_TMP0, HOST_PSX_REGS, PC_OFFSET) ||
			!m_code.EmitCmpReg(HOST_TMP0, expected_host_reg))
		{
			return false;
		}

		direct_exit_branches.push_back(
			m_code.EmitBranchPlaceholder(VitaA32::Condition::NE));
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
			       m_code.EmitMovRegShiftImm(host_reg,
					   static_cast<unsigned>(pinned_host),
					VitaA32::ShiftType::LSL, 0);
		}

		return m_code.EmitLdrImm12(host_reg, HOST_PSX_REGS,
			static_cast<u16>(GprOffset(guest_reg)));
	}

	bool BlockCompiler::EmitLoadGprValue(unsigned guest_reg, unsigned host_reg,
		bool* used_known_value)
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
			       m_code.EmitMovRegShiftImm(static_cast<unsigned>(pinned_host),
					   host_reg, VitaA32::ShiftType::LSL, 0);
		}

		return m_code.EmitStrImm12(host_reg, HOST_PSX_REGS,
			static_cast<u16>(GprOffset(guest_reg)));
	}

	bool BlockCompiler::EmitStoreGprZero(unsigned guest_reg)
	{
		if (guest_reg == 0)
			return true;

		return m_code.EmitMovImm8(HOST_TMP0, 0) && EmitStoreGpr(guest_reg, HOST_TMP0);
	}

	bool BlockCompiler::EmitMoveGpr(unsigned dst_guest_reg,
		unsigned src_guest_reg)
	{
		if (dst_guest_reg == 0)
			return true;

		if (dst_guest_reg == src_guest_reg)
			return true;

		return EmitLoadGpr(src_guest_reg, HOST_TMP0) &&
			   EmitStoreGpr(dst_guest_reg, HOST_TMP0);
	}

	bool BlockCompiler::EmitCompareGprs(unsigned lhs_guest_reg,
		unsigned rhs_guest_reg)
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
		const auto direct_pinned_host = [this](unsigned guest_reg) -> int {
			bool enabled = true;
#if defined(VITASX2_QEMU_VALIDATION)
			enabled = s_qemuIopPinnedBranchDirectCompareEnabled;
#endif
			return enabled ? PinnedHostForGuest(guest_reg) : -1;
		};

		if (lhs_known && rhs_known)
		{
#if defined(VITASX2_QEMU_VALIDATION)
			if (has_tracked_operand)
				++g_qemuIopConstBranchCompareFastPaths;
#endif
			return (lhs_value == rhs_value) ? m_code.EmitCmpReg(HOST_TMP0, HOST_TMP0) : emit_false_compare();
		}

		if (lhs_known)
		{
#if defined(VITASX2_QEMU_VALIDATION)
			if (has_tracked_operand)
				++g_qemuIopConstBranchCompareFastPaths;
#endif
			const int rhs_host = direct_pinned_host(rhs_guest_reg);
			if (rhs_host >= 0)
			{
				m_pinned_gpr_load_hits++;
				m_pinned_branch_operand_moves_removed++;
				return emit_cmp_reg_imm(static_cast<unsigned>(rhs_host), lhs_value);
			}
			return EmitLoadGpr(rhs_guest_reg, HOST_TMP0) &&
				emit_cmp_reg_imm(HOST_TMP0, lhs_value);
		}

		if (rhs_known)
		{
#if defined(VITASX2_QEMU_VALIDATION)
			if (has_tracked_operand)
				++g_qemuIopConstBranchCompareFastPaths;
#endif
			const int lhs_host = direct_pinned_host(lhs_guest_reg);
			if (lhs_host >= 0)
			{
				m_pinned_gpr_load_hits++;
				m_pinned_branch_operand_moves_removed++;
				return emit_cmp_reg_imm(static_cast<unsigned>(lhs_host), rhs_value);
			}
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

		const int lhs_host = direct_pinned_host(lhs_guest_reg);
		const int rhs_host = direct_pinned_host(rhs_guest_reg);
		if (lhs_host >= 0 && rhs_host >= 0)
		{
			m_pinned_gpr_load_hits += 2;
			m_pinned_branch_operand_moves_removed += 2;
			return m_code.EmitCmpReg(static_cast<unsigned>(lhs_host),
				static_cast<unsigned>(rhs_host));
		}
		if (lhs_host >= 0)
		{
			m_pinned_gpr_load_hits++;
			m_pinned_branch_operand_moves_removed++;
			return EmitLoadGpr(rhs_guest_reg, HOST_TMP1) &&
				m_code.EmitCmpReg(static_cast<unsigned>(lhs_host), HOST_TMP1);
		}
		if (rhs_host >= 0)
		{
			m_pinned_gpr_load_hits++;
			m_pinned_branch_operand_moves_removed++;
			return EmitLoadGpr(lhs_guest_reg, HOST_TMP0) &&
				m_code.EmitCmpReg(HOST_TMP0, static_cast<unsigned>(rhs_host));
		}
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
				return ((result <= 0xffu) ? m_code.EmitMovImm8(HOST_TMP0, static_cast<u8>(result)) : m_code.EmitMovImm32(HOST_TMP0, result)) &&
					   EmitStoreGpr(rd, HOST_TMP0);
			}
		}

		const auto store_zero = [this, rd]() { return EmitStoreGprZero(rd); };
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
		const auto emit_reg_imm_or_reg =
			[this](u32 known_value,
												bool (*emit_imm)(VitaA32::CodeBuffer&, unsigned, unsigned, u32),
				bool (*emit_reg)(VitaA32::CodeBuffer&, unsigned, unsigned,
					unsigned)) {
			return emit_imm(m_code, HOST_TMP2, HOST_TMP0, known_value) ||
				   (m_code.EmitMovImm32(HOST_TMP1, known_value) &&
					   emit_reg(m_code, HOST_TMP2, HOST_TMP0, HOST_TMP1));
		};
		const auto emit_add_imm = [](VitaA32::CodeBuffer& code, unsigned rd,
									  unsigned rn, u32 value) {
			return code.EmitAddImm32(rd, rn, value);
		};
		const auto emit_sub_imm = [](VitaA32::CodeBuffer& code, unsigned rd,
									  unsigned rn, u32 value) {
			return code.EmitSubImm32(rd, rn, value);
		};
		const auto emit_and_imm = [](VitaA32::CodeBuffer& code, unsigned rd,
									  unsigned rn, u32 value) {
			return code.EmitAndImm32(rd, rn, value);
		};
		const auto emit_orr_imm = [](VitaA32::CodeBuffer& code, unsigned rd,
									  unsigned rn, u32 value) {
			return code.EmitOrrImm32(rd, rn, value);
		};
		const auto emit_eor_imm = [](VitaA32::CodeBuffer& code, unsigned rd,
									  unsigned rn, u32 value) {
			return code.EmitEorImm32(rd, rn, value);
		};
		const auto emit_add_reg = [](VitaA32::CodeBuffer& code, unsigned rd,
									  unsigned rn, unsigned rm) {
			return code.EmitAddReg(rd, rn, rm);
		};
		const auto emit_sub_reg = [](VitaA32::CodeBuffer& code, unsigned rd,
									  unsigned rn, unsigned rm) {
			return code.EmitSubReg(rd, rn, rm);
		};
		const auto emit_and_reg = [](VitaA32::CodeBuffer& code, unsigned rd,
									  unsigned rn, unsigned rm) {
			return code.EmitAndReg(rd, rn, rm);
		};
		const auto emit_orr_reg = [](VitaA32::CodeBuffer& code, unsigned rd,
									  unsigned rn, unsigned rm) {
			return code.EmitOrrReg(rd, rn, rm);
		};
		const auto emit_eor_reg = [](VitaA32::CodeBuffer& code, unsigned rd,
									  unsigned rn, unsigned rm) {
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
					       m_code.EmitMvnReg(HOST_TMP2, HOST_TMP2) && emit_counted_store();
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
			return ((result <= 0xffu) ? m_code.EmitMovImm8(HOST_TMP0, static_cast<u8>(result)) : m_code.EmitMovImm32(HOST_TMP0, result)) &&
				   EmitStoreGpr(rd, HOST_TMP0);
		}

		if (sa == 0)
			return EmitMoveGpr(rd, rt);

		if (!EmitLoadGpr(rt, HOST_TMP0))
			return false;

		return m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP0, shift,
				   static_cast<u8>(sa)) &&
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
			return ((result <= 0xffu) ? m_code.EmitMovImm8(HOST_TMP0, static_cast<u8>(result)) : m_code.EmitMovImm32(HOST_TMP0, result)) &&
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
			       m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP0, shift,
					   static_cast<u8>(amount)) &&
				   EmitStoreGpr(rd, HOST_TMP2);
		}

		if (!EmitLoadGpr(rt, HOST_TMP0) || !EmitLoadGpr(rs, HOST_TMP1) ||
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
			const u32 result =
				is_signed ? ((static_cast<s32>(known_rs) < static_cast<s32>(known_rt)) ? 1u : 0u) : ((known_rs < known_rt) ? 1u : 0u);
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
				return EmitLoadGpr(rt, HOST_TMP0) && m_code.EmitCmpImm32(HOST_TMP0, 0) &&
					   m_code.EmitMovImm8(HOST_TMP2, 0) &&
					   m_code.EmitMovImm8(HOST_TMP2, 1, VitaA32::Condition::GT) &&
					   EmitStoreGpr(rd, HOST_TMP2);
			}

			if (rt == 0)
			{
				// PCSX2 owner: R3000AOpcodeTables.cpp::psxSLT(). Comparing a
				// signed 32-bit value with $zero is just its sign bit.
				return EmitLoadGpr(rs, HOST_TMP0) &&
				       m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP0,
						   VitaA32::ShiftType::LSR, 31) &&
					   EmitStoreGpr(rd, HOST_TMP2);
			}
		}
		else
		{
			if (rt == 0)
				return EmitStoreGprZero(rd);

			if (rs == 0)
			{
				return EmitLoadGpr(rt, HOST_TMP0) && m_code.EmitCmpImm32(HOST_TMP0, 0) &&
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
			const VitaA32::Condition set_condition =
				known_lhs ? (is_signed ? VitaA32::Condition::GT : VitaA32::Condition::HI) : (is_signed ? VitaA32::Condition::LT : VitaA32::Condition::CC);
#if defined(VITASX2_QEMU_VALIDATION)
			++g_qemuIopConstRegisterOperandFastPaths;
#endif
			return EmitLoadGpr(dynamic_reg, HOST_TMP0) &&
				   emit_cmp_reg_imm(HOST_TMP0, known_value) &&
				   m_code.EmitMovImm8(HOST_TMP2, 0) &&
				   m_code.EmitMovImm8(HOST_TMP2, 1, set_condition) &&
				   EmitStoreGpr(rd, HOST_TMP2);
		}

		const VitaA32::Condition set_condition =
			is_signed ? VitaA32::Condition::LT : VitaA32::Condition::CC;
		if (!EmitLoadGpr(rs, HOST_TMP0) || !EmitLoadGpr(rt, HOST_TMP1) ||
			!m_code.EmitCmpReg(HOST_TMP0, HOST_TMP1) ||
			!m_code.EmitMovImm8(HOST_TMP2, 0) ||
			!m_code.EmitMovImm8(HOST_TMP2, 1, set_condition) ||
			!EmitStoreGpr(rd, HOST_TMP2))
		{
			return false;
		}

		if (m_emit_branch_predicate_producer)
		{
			// PCSX2 owner: x86/iR3000Atables.cpp::rpsxSLTs_() materializes
			// SLT/SLTU directly from the compare flags, and the following
			// rpsxBEQ_process()/rpsxBNE_process() consumes the same 0/1 result.
			// A32 keeps that compare live through the non-S materialization/store.
			m_branch_predicate_producer_flags_live = true;
			m_branch_predicate_producer_guest = rd;
			m_branch_predicate_producer_true_condition = set_condition;
		}
		return true;
	}

	bool BlockCompiler::EmitMultiplyOp(u32 op, bool is_signed)
	{
		m_retained_lo_host_valid = false;
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
			       m_code.EmitStrImm12(HOST_TMP2, HOST_PSX_REGS,
					   static_cast<u16>(LO_OFFSET)) &&
			       m_code.EmitStrImm12(HOST_TMP3, HOST_PSX_REGS,
					   static_cast<u16>(HI_OFFSET));
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
				const bool retain_lo = RetainedLoForwardingEnabled();
				const unsigned zero_host = retain_lo ? HOST_TMP3 : HOST_TMP2;
				const bool emitted =
					m_code.EmitMovImm8(zero_host, 0) &&
					m_code.EmitStrImm12(zero_host, HOST_PSX_REGS,
						static_cast<u16>(LO_OFFSET)) &&
					m_code.EmitStrImm12(zero_host, HOST_PSX_REGS,
						static_cast<u16>(HI_OFFSET));
				m_retained_lo_host_valid = emitted && retain_lo;
				return emitted;
			}

			if (!EmitLoadGpr(dynamic_reg, HOST_TMP0) ||
				!m_code.EmitMovImm32(HOST_TMP1, known_value))
			{
				return false;
			}

			const bool retain_lo = RetainedLoForwardingEnabled();
			const unsigned lo_host = retain_lo ? HOST_TMP3 : HOST_TMP2;
			const unsigned hi_host = retain_lo ? HOST_TMP2 : HOST_TMP3;
			if (is_signed)
			{
				if (!m_code.EmitSmull(lo_host, hi_host, HOST_TMP0, HOST_TMP1))
					return false;
			}
			else
			{
				if (!m_code.EmitUmull(lo_host, hi_host, HOST_TMP0, HOST_TMP1))
					return false;
			}

			const bool emitted =
				m_code.EmitStrImm12(lo_host, HOST_PSX_REGS,
					static_cast<u16>(LO_OFFSET)) &&
				m_code.EmitStrImm12(hi_host, HOST_PSX_REGS,
					static_cast<u16>(HI_OFFSET));
			m_retained_lo_host_valid = emitted && retain_lo;
			return emitted;
		}

		if (!EmitLoadGpr(RS(op), HOST_TMP0) || !EmitLoadGpr(RT(op), HOST_TMP1))
		{
			return false;
		}

		const bool retain_lo = RetainedLoForwardingEnabled();
		const unsigned lo_host = retain_lo ? HOST_TMP3 : HOST_TMP2;
		const unsigned hi_host = retain_lo ? HOST_TMP2 : HOST_TMP3;
		if (is_signed)
		{
			if (!m_code.EmitSmull(lo_host, hi_host, HOST_TMP0, HOST_TMP1))
				return false;
		}
		else
		{
			if (!m_code.EmitUmull(lo_host, hi_host, HOST_TMP0, HOST_TMP1))
				return false;
		}

		const bool emitted =
			m_code.EmitStrImm12(lo_host, HOST_PSX_REGS,
				static_cast<u16>(LO_OFFSET)) &&
			m_code.EmitStrImm12(hi_host, HOST_PSX_REGS,
				static_cast<u16>(HI_OFFSET));
		m_retained_lo_host_valid = emitted && retain_lo;
		return emitted;
	}

	bool BlockCompiler::EmitDivideOp(u32 op, bool is_signed)
	{
		struct BranchPatch
		{
			size_t offset = static_cast<size_t>(-1);
			VitaA32::Condition condition = VitaA32::Condition::AL;
		};

		const auto emit_branch = [this](BranchPatch& patch,
									 VitaA32::Condition condition) {
			patch.offset = m_code.EmitBranchPlaceholder(condition);
			patch.condition = condition;
			return patch.offset != static_cast<size_t>(-1);
		};

		const auto patch_branch = [this](const BranchPatch& patch, size_t target) {
			return m_code.PatchBranch(patch.offset, target, patch.condition);
		};

		const auto patch_branches = [patch_branch](const BranchPatch* branches,
										unsigned count, size_t target) {
			for (unsigned i = 0; i < count; i++)
			{
				if (!patch_branch(branches[i], target))
					return false;
			}
			return true;
		};

		const auto store_hilo = [this](unsigned lo_reg, unsigned hi_reg) {
			return m_code.EmitStrImm12(lo_reg, HOST_PSX_REGS,
					   static_cast<u16>(LO_OFFSET)) &&
			       m_code.EmitStrImm12(hi_reg, HOST_PSX_REGS,
					   static_cast<u16>(HI_OFFSET));
		};
		const auto store_hilo_from_hi_lo_pair = [this]() {
			// PCSX2 owner: R3000A.h::GPRRegs lays out HI then LO; A32 STRD can
			// write the adjacent pair when HOST_TMP2=HI and HOST_TMP3=LO.
			return m_code.EmitStrdImm8(HOST_TMP2, HOST_TMP3, HOST_PSX_REGS,
				static_cast<u8>(HI_OFFSET));
		};
		const auto emit_and_mask = [this](unsigned rd, unsigned rn, u32 mask) {
			if (m_code.EmitAndImm32(rd, rn, mask))
				return true;
			return m_code.EmitMovImm32(HOST_TMP1, mask) &&
				   m_code.EmitAndReg(rd, rn, HOST_TMP1);
		};

		const void* helper =
			is_signed ? reinterpret_cast<const void*>(&VitaIopA32DivResult) : reinterpret_cast<const void*>(&VitaIopA32DivuResult);

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
			       m_code.EmitStrImm12(HOST_TMP2, HOST_PSX_REGS,
					   static_cast<u16>(LO_OFFSET)) &&
			       m_code.EmitStrImm12(HOST_TMP3, HOST_PSX_REGS,
					   static_cast<u16>(HI_OFFSET));
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
				return EmitLoadGpr(rs, HOST_TMP0) && m_code.EmitMovImm8(HOST_TMP2, 0) &&
					   store_hilo(HOST_TMP0, HOST_TMP2);
			}

			if (is_signed && known_rt == 0xffffffffu)
			{
#if defined(VITASX2_QEMU_VALIDATION)
				++g_qemuIopConstDivideOperandFastPaths;
#endif
				return EmitLoadGpr(rs, HOST_TMP0) && m_code.EmitMovImm8(HOST_TMP2, 0) &&
					   m_code.EmitRsbImm32(HOST_TMP3, HOST_TMP0, 0) &&
					   store_hilo_from_hi_lo_pair();
			}

			const bool signed_negative_power_of_two =
				is_signed && static_cast<s32>(known_rt) < 0 &&
				known_rt != 0x80000000u && IsPowerOfTwo(0u - known_rt);
			if ((!is_signed && IsPowerOfTwo(known_rt)) ||
				(is_signed && static_cast<s32>(known_rt) > 0 &&
					IsPowerOfTwo(known_rt)) ||
				signed_negative_power_of_two)
			{
#if defined(VITASX2_QEMU_VALIDATION)
				++g_qemuIopConstDivideOperandFastPaths;
#endif
				const u32 positive_divisor =
					signed_negative_power_of_two ? (0u - known_rt) : known_rt;
				const unsigned shift = PowerOfTwoShift(positive_divisor);
				const u32 mask = positive_divisor - 1;
				if (!EmitLoadGpr(rs, HOST_TMP0))
					return false;

				if (!is_signed)
				{
					return m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP0,
							   VitaA32::ShiftType::LSR,
							   static_cast<u8>(shift)) &&
						   emit_and_mask(HOST_TMP3, HOST_TMP0, mask) &&
						   store_hilo(HOST_TMP2, HOST_TMP3);
				}

				return m_code.EmitMovRegShiftImm(HOST_TMP3, HOST_TMP0,
						   VitaA32::ShiftType::ASR, 31) &&
					   emit_and_mask(HOST_TMP3, HOST_TMP3, mask) &&
					   m_code.EmitAddReg(HOST_TMP3, HOST_TMP0, HOST_TMP3) &&
				       m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP3,
						   VitaA32::ShiftType::ASR,
						   static_cast<u8>(shift)) &&
				       m_code.EmitMovRegShiftImm(HOST_TMP3, HOST_TMP2,
						   VitaA32::ShiftType::LSL,
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
			return EmitLoadGpr(rt, HOST_TMP1) && m_code.EmitMovImm8(HOST_TMP2, 0) &&
				   m_code.EmitMovImm8(HOST_TMP3, 0) &&
				   m_code.EmitCmpImm32(HOST_TMP1, 0) &&
			       m_code.EmitMovImm32(HOST_TMP2, 0xffffffffu,
					   VitaA32::Condition::EQ) &&
				   store_hilo(HOST_TMP2, HOST_TMP3);
		}

		if (!EmitLoadGpr(rs, HOST_TMP0) || !EmitLoadGpr(rt, HOST_TMP1))
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
			!emit_branch(done_branches[done_branch_count++],
				VitaA32::Condition::AL))
		{
			return false;
		}

		if (!patch_branch(zero_branch, m_code.Size()) ||
			!m_code.EmitMovImm8(HOST_TMP2, 0) || !store_hilo(HOST_TMP2, HOST_TMP2) ||
			!emit_branch(done_branches[done_branch_count++],
				VitaA32::Condition::AL))
		{
			return false;
		}

		if (!patch_branch(divone_branch, m_code.Size()) ||
			!m_code.EmitMovImm8(HOST_TMP2, 0) || !store_hilo(HOST_TMP0, HOST_TMP2) ||
			!emit_branch(done_branches[done_branch_count++],
				VitaA32::Condition::AL))
		{
			return false;
		}

		if (is_signed)
		{
			if (!patch_branch(negone_branch, m_code.Size()) ||
				!m_code.EmitMovImm8(HOST_TMP2, 0) ||
				!m_code.EmitRsbImm32(HOST_TMP3, HOST_TMP0, 0) ||
				!store_hilo_from_hi_lo_pair() ||
				!emit_branch(done_branches[done_branch_count++],
					VitaA32::Condition::AL) ||
				!patch_branch(equal_branch, m_code.Size()) ||
				!m_code.EmitMovImm8(HOST_TMP2, 0) ||
				!m_code.EmitMovImm8(HOST_TMP3, 1) || !store_hilo_from_hi_lo_pair() ||
				!emit_branch(done_branches[done_branch_count++],
					VitaA32::Condition::AL) ||
				!patch_branch(power_of_two_branch, m_code.Size()) ||
				!m_code.EmitMovRegShiftImm(HOST_TMP3, HOST_TMP0,
					VitaA32::ShiftType::ASR, 31) ||
				!m_code.EmitSubImm8(HOST_TMP2, HOST_TMP1, 1) ||
				!m_code.EmitAndReg(HOST_TMP3, HOST_TMP3, HOST_TMP2) ||
				!m_code.EmitAddReg(HOST_TMP3, HOST_TMP0, HOST_TMP3) ||
				!m_code.EmitClz(HOST_SAVED0, HOST_TMP1) ||
				!m_code.EmitRsbImm32(HOST_TMP2, HOST_SAVED0, 31) ||
				!m_code.EmitMovRegShiftReg(HOST_SAVED0, HOST_TMP3,
					VitaA32::ShiftType::ASR, HOST_TMP2) ||
				!m_code.EmitMovRegShiftReg(HOST_TMP3, HOST_SAVED0,
					VitaA32::ShiftType::LSL, HOST_TMP2) ||
				!m_code.EmitSubReg(HOST_TMP3, HOST_TMP0, HOST_TMP3) ||
				!store_hilo(HOST_SAVED0, HOST_TMP3) ||
				!emit_branch(done_branches[done_branch_count++],
					VitaA32::Condition::AL))
			{
				return false;
			}
		}
		else
		{
			if (!patch_branch(unsigned_less_branch, m_code.Size()) ||
				!m_code.EmitMovImm8(HOST_TMP2, 0) ||
				!store_hilo(HOST_TMP2, HOST_TMP0) ||
				!emit_branch(done_branches[done_branch_count++],
					VitaA32::Condition::AL) ||
				!patch_branch(equal_branch, m_code.Size()) ||
				!m_code.EmitMovImm8(HOST_TMP2, 0) ||
				!m_code.EmitMovImm8(HOST_TMP3, 1) || !store_hilo_from_hi_lo_pair() ||
				!emit_branch(done_branches[done_branch_count++],
					VitaA32::Condition::AL) ||
				!patch_branch(power_of_two_branch, m_code.Size()) ||
				!m_code.EmitSubImm8(HOST_TMP2, HOST_TMP1, 1) ||
				!m_code.EmitAndReg(HOST_TMP3, HOST_TMP0, HOST_TMP2) ||
				!m_code.EmitClz(HOST_TMP2, HOST_TMP1) ||
				!m_code.EmitRsbImm32(HOST_SAVED0, HOST_TMP2, 31) ||
				!m_code.EmitMovRegShiftReg(HOST_TMP2, HOST_TMP0,
					VitaA32::ShiftType::LSR, HOST_SAVED0) ||
				!store_hilo(HOST_TMP2, HOST_TMP3) ||
				!emit_branch(done_branches[done_branch_count++],
					VitaA32::Condition::AL))
			{
				return false;
			}
		}

		const size_t fallback_target = m_code.Size();
		if (!patch_branch(fallback_branch, fallback_target) ||
			(is_signed &&
				!patch_branch(non_positive_fallback_branch, fallback_target)) ||
			!m_code.EmitCallAbsolute(helper, HOST_CALL_SCRATCH) ||
			!store_hilo(HOST_TMP0, HOST_TMP1))
		{
			return false;
		}

		return patch_branches(done_branches, done_branch_count, m_code.Size());
	}

	bool BlockCompiler::EmitExceptionOp(u32 pc, u32 code)
	{
		// PCSX2 rpsxSYSCALL()/rpsxBREAK() use FLUSH_NODESTROY: dirty guest
		// values become helper-visible while their allocation remains valid on
		// the rare exception-vector equality arm.
		if (!EmitFlushPinnedGprs() ||
			!m_code.EmitMovImm32(HOST_TMP0, pc) ||
			!m_code.EmitMovImm32(HOST_TMP1, code) ||
			!m_code.EmitCallAbsolute(
				reinterpret_cast<const void*>(&VitaIopA32RaiseException),
				HOST_CALL_SCRATCH) ||
			!m_code.EmitLdrImm12(HOST_TMP0, HOST_PSX_REGS,
				static_cast<u16>(PC_OFFSET)) ||
			!(m_code.EmitCmpImm32(HOST_TMP0, pc) ||
				(m_code.EmitMovImm32(HOST_TMP1, pc) &&
					m_code.EmitCmpReg(HOST_TMP0, HOST_TMP1))))
		{
			return false;
		}

		// PCSX2 owner: x86/iR3000A.cpp::rpsxSYSCALL/rpsxBREAK. A normal
		// exception vector differs from the fault PC, charges the executed prefix,
		// and jumps straight back to the dispatcher. If the instruction itself is
		// at that vector, the equality arm keeps the private block-cycle state and
		// continues compiling/executing the same BaseBlock.
		const size_t continue_block =
			m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
		const u32 logical_cycle_prefix =
			m_logical_cycle_prefix + m_current_cycle_count;
		if (continue_block == static_cast<size_t>(-1) ||
			(m_defer_cycle_updates &&
				!EmitPublishCyclePrefix(m_current_cycle_count)) ||
			!EndBlockLogicalContinuationReturn(
				true, false, logical_cycle_prefix))
		{
			return false;
		}

		const size_t continue_offset = m_code.Size();
		// On the equality arm x86 keeps the fault/vector PC visible until the
		// logical BaseBlock's real tail. A later memory/helper call must not see an
		// artificial compile-time psxpc prefix.
		return m_code.PatchBranch(
			continue_block, continue_offset, VitaA32::Condition::EQ);
	}

	bool BlockCompiler::EmitPrivateCycleHelperCall(const void* helper)
	{
		// CompileStraightLineBlock rejects this opcode family whenever time is
		// nondeferred. Keep the invariant local too so a future caller cannot make
		// rpsxRFE() observe an already-published instruction prefix.
		if (!m_defer_cycle_updates)
			return false;
		return m_code.EmitCallAbsolute(helper, HOST_CALL_SCRATCH);
	}

	void BlockCompiler::ResolveIrxImport(u32 marker_pc, u32 marker_op)
	{
		m_emit_irx_import = (marker_op >> 16) == 0x2400;
		m_irx_import_log = false;
		m_irx_import_table = 0;
		m_irx_import_index = static_cast<u16>(marker_op);
		m_irx_import_hle = nullptr;
		m_irx_import_debug = nullptr;
		m_irx_import_funcname = nullptr;
		if (!m_emit_irx_import)
			return;

		// PCSX2 owner: x86/iR3000A.cpp::psxRecompileIrxImport(). Resolve
		// the import table and operation once at compile time. A marker without
		// a recognized table/callback remains the architectural ADDIU-to-$zero
		// delay NOP and reaches the ordinary static-J tail.
		m_irx_import_table = R3000A::irxImportTableAddr(marker_pc);
		if (m_irx_import_table == 0)
			return;

		const std::string libname = iopMemReadString(m_irx_import_table + 12, 8);
		m_irx_import_hle = reinterpret_cast<const void*>(
			R3000A::irxImportHLE(libname, m_irx_import_index));
#ifdef PCSX2_DEVBUILD
		m_irx_import_debug = reinterpret_cast<const void*>(
			R3000A::irxImportDebug(libname, m_irx_import_index));
		m_irx_import_funcname =
			R3000A::irxImportFuncname(libname, m_irx_import_index);
#endif
		m_irx_import_log = TraceActive(IOP.Bios);
	}

	bool BlockCompiler::EmitIrxImportMarker(u32 marker_pc, u32 marker_op)
	{
		if (!m_emit_irx_import)
			return false;

		// psxRecompileIrxImport() only materializes an otherwise inert marker
		// for tracing when the owner can name the import. An HLE/debug callback
		// still materializes it independently (and tracing then follows exactly
		// as the owner does, including release builds with no function name).
		const bool has_callback = m_irx_import_hle || m_irx_import_debug ||
			(m_irx_import_log && m_irx_import_funcname);
		if (!has_callback)
			return true;

		// PCSX2's _psxFlushCall(FLUSH_NODESTROY) publishes dirty mappings
		// before any import callback while leaving the old host allocation usable.
		// Mark those values clean after emitting the stores: a callback may update
		// canonical psxRegs, and the later ordinary-J tail must not overwrite it.
		if (!EmitStoreCode(marker_op) || !EmitStorePc(marker_pc + 4) ||
			!EmitFlushPinnedGprs())
		{
			return false;
		}
		for (u8 i = 0; i < m_pinned_gpr_count; i++)
			m_pinned_gprs[i].written = false;

		if (m_irx_import_log &&
			(!m_code.EmitMovImm32(HOST_TMP0, m_irx_import_table) ||
			 !m_code.EmitMovImm32(HOST_TMP1, m_irx_import_index) ||
				!m_code.EmitMovImm32(HOST_TMP2, static_cast<u32>(reinterpret_cast<uptr>(
													m_irx_import_funcname))) ||
			 !m_code.EmitCallAbsolute(
				reinterpret_cast<const void*>(&R3000A::irxImportLog_rec),
				HOST_CALL_SCRATCH)))
		{
			return false;
		}

		if (m_irx_import_debug &&
			!m_code.EmitCallAbsolute(m_irx_import_debug, HOST_CALL_SCRATCH))
		{
			return false;
		}

		if (!m_irx_import_hle)
			return true;
		if (!m_code.EmitCallAbsolute(m_irx_import_hle, HOST_CALL_SCRATCH) ||
			!m_code.EmitCmpImm32(HOST_TMP0, 0))
		{
			return false;
		}

		const size_t continue_jump =
			m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
		if (continue_jump == static_cast<size_t>(-1) ||
			!EndBlockLogicalContinuationReturn(false, false))
		{
			return false;
		}
		return m_code.PatchBranch(continue_jump, m_code.Size(),
			VitaA32::Condition::EQ);
	}

	bool BlockCompiler::EmitImmediateOp(u32 op, u32 pc)
	{
		const unsigned opcode = op >> 26;
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		if (rt == 0 && opcode == 0x09 && (op >> 16) == 0x2400 && m_emit_irx_import)
		{
			return EmitIrxImportMarker(pc, op);
		}
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
					result =
						(static_cast<s32>(known_rs) < static_cast<s32>(IMM_S(op))) ? 1u : 0u;
					break;
				case 0x0b: // SLTIU
					// PCSX2 owner: R3000AOpcodeTables.cpp::psxSLTIU().
					result =
						(known_rs < static_cast<u32>(static_cast<s32>(IMM_S(op)))) ? 1u : 0u;
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
				return ((result <= 0xffu) ? m_code.EmitMovImm8(HOST_TMP0, static_cast<u8>(result)) : m_code.EmitMovImm32(HOST_TMP0, result)) &&
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
			return m_code.EmitMovImm32(HOST_TMP0,
					   static_cast<u32>(static_cast<s32>(IMM_S(op)))) &&
				   EmitStoreGpr(rt, HOST_TMP0);
		}

		// PCSX2 owner: x86/iR3000Atables.cpp::rpsxLogicalOpI(). Preserve the
		// same zero/no-op folds before loading rs into a host register.
		const u16 logical_imm = IMM_U(op);
		if (opcode == 0x0c && (logical_imm == 0 || rs == 0)) // ANDI
		{
			return m_code.EmitMovImm8(HOST_TMP0, 0) && EmitStoreGpr(rt, HOST_TMP0);
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
			return m_code.EmitMovImm8(HOST_TMP0, result) && EmitStoreGpr(rt, HOST_TMP0);
		}
		if (opcode == 0x0b && rs == 0) // SLTIU
		{
			// PCSX2 owner: R3000AOpcodeTables.cpp::psxSLTIU(). The immediate is
			// sign-extended before the unsigned compare, so 0 < imm iff imm != 0.
			const u8 result = (IMM_S(op) != 0) ? 1 : 0;
			return m_code.EmitMovImm8(HOST_TMP0, result) && EmitStoreGpr(rt, HOST_TMP0);
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
				if (imm > 0 &&
					m_code.EmitAddImm32(HOST_TMP2, HOST_TMP0, static_cast<u32>(imm)))
					return EmitStoreGpr(rt, HOST_TMP2);
				if (imm < 0 &&
					m_code.EmitSubImm32(HOST_TMP2, HOST_TMP0, static_cast<u32>(-imm)))
					return EmitStoreGpr(rt, HOST_TMP2);
				return m_code.EmitMovImm32(HOST_TMP1, static_cast<u32>(imm)) &&
					   m_code.EmitAddReg(HOST_TMP2, HOST_TMP0, HOST_TMP1) &&
					   EmitStoreGpr(rt, HOST_TMP2);
			}
			case 0x0a: // SLTI
			{
				const u32 imm = static_cast<u32>(static_cast<s32>(IMM_S(op)));
				if (!(m_code.EmitCmpImm32(HOST_TMP0, imm) ||
						(m_code.EmitMovImm32(HOST_TMP1, imm) &&
							m_code.EmitCmpReg(HOST_TMP0, HOST_TMP1))))
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
						(m_code.EmitMovImm32(HOST_TMP1, imm) &&
							m_code.EmitCmpReg(HOST_TMP0, HOST_TMP1))))
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
		if (imm < 0 &&
			m_code.EmitSubImm32(host_reg, host_reg, static_cast<u32>(-imm)))
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
						return m_code.EmitLdrbImm12(HOST_TMP0, HOST_IOP_RAM_BASE,
							static_cast<u16>(address));
					return m_code.EmitMovImm32(HOST_TMP0, address) &&
						   m_code.EmitLdrbRegShift(HOST_TMP0, HOST_IOP_RAM_BASE, HOST_TMP0,
							   VitaA32::ShiftType::LSL, 0);
				case 0x20: // LB
					if (address <= 0xffu)
						return m_code.EmitLdrsbImm8(HOST_TMP0, HOST_IOP_RAM_BASE,
							static_cast<u8>(address));
					return m_code.EmitMovImm32(HOST_TMP0, address) &&
						   m_code.EmitLdrsbReg(HOST_TMP0, HOST_IOP_RAM_BASE, HOST_TMP0);
				case 0x21: // LH
					if (address <= 0xffu)
						return m_code.EmitLdrshImm8(HOST_TMP0, HOST_IOP_RAM_BASE,
							static_cast<u8>(address));
					return m_code.EmitMovImm32(HOST_TMP0, address) &&
						   m_code.EmitLdrshReg(HOST_TMP0, HOST_IOP_RAM_BASE, HOST_TMP0);
				case 0x25: // LHU
					if (address <= 0xffu)
						return m_code.EmitLdrhImm8(HOST_TMP0, HOST_IOP_RAM_BASE,
							static_cast<u8>(address));
					return m_code.EmitMovImm32(HOST_TMP0, address) &&
						   m_code.EmitLdrhReg(HOST_TMP0, HOST_IOP_RAM_BASE, HOST_TMP0);
				case 0x23: // LW
					if (address <= 0x0fffu)
						return m_code.EmitLdrImm12(HOST_TMP0, HOST_IOP_RAM_BASE,
							static_cast<u16>(address));
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

		bool ram_provenance_specialization = true;
#if defined(VITASX2_QEMU_VALIDATION)
		ram_provenance_specialization = s_qemuIopRamProvenanceSpecializationEnabled;
#endif
		const u32 ram_guard_mask =
			0x10000000u |
			((ram_provenance_specialization && rt != 0) ? alignment_mask : 0u);
		if (!EmitEffectiveAddress(op, HOST_SAVED0) ||
			!m_code.EmitTstImm32(HOST_SAVED0, ram_guard_mask))
		{
			return false;
		}

		// PCSX2 owner: x86/iR3000Atables.cpp::rpsxLoad() uses direct iopMem->Main
		// reads for ordinary IOP RAM aliases and iopMemRead* helpers for MMIO/ROM.
		const size_t fallback_branch =
			m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
		if (fallback_branch == static_cast<size_t>(-1))
			return false;

		size_t alignment_fallback_branch = static_cast<size_t>(-1);
		if (!ram_provenance_specialization && rt != 0 && alignment_mask != 0)
		{
			if (!m_code.EmitAndImm8(HOST_TMP2, HOST_SAVED0, alignment_mask, true))
				return false;

			alignment_fallback_branch =
				m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (alignment_fallback_branch == static_cast<size_t>(-1))
				return false;
		}
		else if (ram_provenance_specialization && rt != 0 && alignment_mask != 0)
		{
			// One TST/BNE now proves both PCSX2's ordinary-RAM alias and the
			// host alignment required by the direct A32 load.
			m_fused_ram_guard_instructions_removed += 2;
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
		if (!m_code.PatchBranch(tail.fallback_branch, fallback_target,
				VitaA32::Condition::NE))
			return false;
		if (tail.alignment_fallback_branch != static_cast<size_t>(-1) &&
			!m_code.PatchBranch(tail.alignment_fallback_branch, fallback_target,
				VitaA32::Condition::NE))
		{
			return false;
		}

		if (!m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_SAVED0,
				VitaA32::ShiftType::LSL, 0) ||
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
						return m_code.EmitStrbImm12(HOST_TMP1, HOST_IOP_RAM_BASE,
							static_cast<u16>(address));
					return m_code.EmitMovImm32(HOST_TMP0, address) &&
						   m_code.EmitStrbRegShift(HOST_TMP1, HOST_IOP_RAM_BASE, HOST_TMP0,
							   VitaA32::ShiftType::LSL, 0);
				case 0x29: // SH
					if (address <= 0xffu)
						return m_code.EmitStrhImm8(HOST_TMP1, HOST_IOP_RAM_BASE,
							static_cast<u8>(address));
					return m_code.EmitMovImm32(HOST_TMP0, address) &&
						   m_code.EmitStrhReg(HOST_TMP1, HOST_IOP_RAM_BASE, HOST_TMP0);
				case 0x2b: // SW
					if (address <= 0x0fffu)
						return m_code.EmitStrImm12(HOST_TMP1, HOST_IOP_RAM_BASE,
							static_cast<u16>(address));
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
			       m_code.EmitMovImm32(
					   HOST_CALL_SCRATCH,
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
		bool source_chunk_guard_enabled = true;
#if defined(VITASX2_QEMU_VALIDATION)
		source_chunk_guard_enabled = s_qemuIopRamProvenanceSpecializationEnabled;
#endif
		bool used_known_store_value = false;
		size_t isolated_skip = static_cast<size_t>(-1);
		if (!EmitIsolateCacheGuard(&isolated_skip) ||
			!EmitLoadGprValue(RT(op), HOST_TMP1, &used_known_store_value) ||
			!emit_store_value())
		{
			return false;
		}
		size_t no_source_chunk = static_cast<size_t>(-1);
		if (source_chunk_guard_enabled)
		{
			constexpr u32 source_chunk_shift = 6;
			if (!m_ram_source_chunk_live_flags ||
				!m_code.EmitMovImm32(HOST_TMP2,
					static_cast<u32>(reinterpret_cast<uptr>(
						m_ram_source_chunk_live_flags +
						(address >> source_chunk_shift)))) ||
				!m_code.EmitLdrbImm12(HOST_TMP0, HOST_TMP2, 0) ||
				!m_code.EmitCmpImm32(HOST_TMP0, 0))
			{
				return false;
			}
			no_source_chunk =
				m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
			if (no_source_chunk == static_cast<size_t>(-1))
				return false;
		}
		if (!emit_clear_stored_word() ||
			(no_source_chunk != static_cast<size_t>(-1) &&
				!m_code.PatchBranch(no_source_chunk, m_code.Size(),
					VitaA32::Condition::EQ)))
		{
			return false;
		}

		if (isolated_skip != static_cast<size_t>(-1) &&
			!m_code.PatchBranch(isolated_skip, m_code.Size(), VitaA32::Condition::NE))
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
		bool ram_provenance_specialization = true;
#if defined(VITASX2_QEMU_VALIDATION)
		ram_provenance_specialization = s_qemuIopRamProvenanceSpecializationEnabled;
#endif

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
			       m_code.EmitMovImm32(
					   HOST_CALL_SCRATCH,
					   static_cast<u32>(reinterpret_cast<uptr>(&psxCpu))) &&
				   m_code.EmitLdrImm12(HOST_CALL_SCRATCH, HOST_CALL_SCRATCH, 0) &&
				   m_code.EmitLdrImm12(HOST_CALL_SCRATCH, HOST_CALL_SCRATCH,
					   static_cast<u16>(offsetof(R3000Acpu, Clear))) &&
				   m_code.EmitBlx(HOST_CALL_SCRATCH);
		};
		const auto emit_source_chunk_guard = [&]() -> size_t {
			if (!ram_provenance_specialization ||
				!m_ram_source_chunk_live_flags)
			{
				return static_cast<size_t>(-1);
			}
			bool source_page_literal_enabled = m_source_page_literal_allowed;
#if defined(VITASX2_QEMU_VALIDATION)
			source_page_literal_enabled =
				source_page_literal_enabled && s_qemuIopSourcePageLiteralEnabled;
#endif
			if (source_page_literal_enabled)
			{
				const size_t load = m_code.EmitLdrLiteralPlaceholder(HOST_TMP2);
				if (load == static_cast<size_t>(-1))
					return static_cast<size_t>(-1);
				m_source_chunk_literal_loads.push_back(load);
			}
			else if (!m_code.EmitMovImm32(HOST_TMP2,
					static_cast<u32>(reinterpret_cast<uptr>(
						m_ram_source_chunk_live_flags))))
			{
				return static_cast<size_t>(-1);
			}
			if (!m_code.EmitAndReg(HOST_TMP0, HOST_SAVED0, HOST_IOP_RAM_MASK) ||
				!m_code.EmitLdrbRegShift(HOST_TMP0, HOST_TMP2, HOST_TMP0,
					VitaA32::ShiftType::LSR, 6) ||
				!m_code.EmitCmpImm32(HOST_TMP0, 0))
			{
				return static_cast<size_t>(-1);
			}
			return m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
		};
		const auto emit_source_page_guard = [&]() -> size_t {
			// PCSX2 owner: x86/iR3000A.cpp::PSXREC_CLEARM checks psxRecLUT
			// before entering recClearIOP(). The product path maintains an exact
			// byte Boolean beside the wider ownership count, so one shifted LDRB
			// replaces the halfword-index construction on every RAM store.
			if (ram_provenance_specialization)
			{
				bool source_page_literal_enabled = m_source_page_literal_allowed;
#if defined(VITASX2_QEMU_VALIDATION)
				source_page_literal_enabled =
					source_page_literal_enabled && s_qemuIopSourcePageLiteralEnabled;
#endif
				if (!m_ram_source_page_live_flags)
					return static_cast<size_t>(-1);
				if (source_page_literal_enabled)
				{
					const size_t load = m_code.EmitLdrLiteralPlaceholder(HOST_TMP2);
					if (load == static_cast<size_t>(-1))
						return static_cast<size_t>(-1);
					m_source_page_literal_loads.push_back(load);
				}
				else if (!m_code.EmitMovImm32(HOST_TMP2,
							 static_cast<u32>(reinterpret_cast<uptr>(
								 m_ram_source_page_live_flags))))
				{
					return static_cast<size_t>(-1);
				}
				if (!m_code.EmitLdrbRegShift(HOST_TMP0, HOST_TMP2, HOST_TMP0,
						VitaA32::ShiftType::LSR, 12) ||
					!m_code.EmitCmpImm32(HOST_TMP0, 0))
				{
					return static_cast<size_t>(-1);
				}
				m_source_page_guard_instructions_removed += 2;
				return m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
			}

			if (!m_ram_source_page_live_counts ||
				!m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_TMP0,
					VitaA32::ShiftType::LSR, 12) ||
				!m_code.EmitMovImm32(HOST_TMP2, static_cast<u32>(reinterpret_cast<uptr>(
												m_ram_source_page_live_counts))) ||
				!m_code.EmitLdrRegShift(HOST_TMP0, HOST_TMP2, HOST_TMP0,
					VitaA32::ShiftType::LSL, 2) ||
				!m_code.EmitCmpImm32(HOST_TMP0, 0))
			{
				return static_cast<size_t>(-1);
			}
			return m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
		};

		constexpr u32 RAM_MAPPING_GUARD_MASK = 0x1f800000u;
		if (!EmitEffectiveAddress(op, HOST_SAVED0) ||
			!m_code.EmitTstImm32(HOST_SAVED0, RAM_MAPPING_GUARD_MASK))
		{
			return false;
		}

		// PCSX2 owner: IopMem.cpp::iopMemWrite8/16/32 writes directly through
		// psxMemWLUT only for writable RAM and when CP0 isolate-cache is clear,
		// then invalidates the written word through psxCpu->Clear(mem & ~3, 1).
		const size_t fallback_branch =
			m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
		if (fallback_branch == static_cast<size_t>(-1))
			return false;

		size_t alignment_fallback_branch = static_cast<size_t>(-1);
		if (alignment_mask != 0)
		{
			if (!m_code.EmitAndImm8(HOST_TMP2, HOST_SAVED0, alignment_mask, true))
				return false;

			alignment_fallback_branch =
				m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (alignment_fallback_branch == static_cast<size_t>(-1))
				return false;
		}

		bool used_known_store_value = false;
		size_t isolated_fallback_branch = static_cast<size_t>(-1);
		if (!EmitIsolateCacheGuard(&isolated_fallback_branch) ||
			!EmitLoadGprValue(RT(op), HOST_TMP1, &used_known_store_value) ||
			!m_code.EmitAndReg(HOST_TMP0, HOST_SAVED0, HOST_IOP_RAM_MASK) ||
			!emit_store_value())
		{
			return false;
		}
		const size_t no_source_page_branch = emit_source_page_guard();
		const size_t no_source_chunk_branch = emit_source_chunk_guard();
		if (no_source_page_branch == static_cast<size_t>(-1) ||
			(ram_provenance_specialization &&
				no_source_chunk_branch == static_cast<size_t>(-1)) ||
			!emit_clear_stored_word() ||
			!m_code.PatchBranch(no_source_page_branch, m_code.Size(),
				VitaA32::Condition::EQ) ||
			(no_source_chunk_branch != static_cast<size_t>(-1) &&
				!m_code.PatchBranch(no_source_chunk_branch, m_code.Size(),
					VitaA32::Condition::EQ)))
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
#if defined(VITASX2_QEMU_VALIDATION)
		if (used_known_store_value)
			++g_qemuIopConstStoreValueFastPaths;
#endif
		return true;
	}

	bool BlockCompiler::EmitSourcePageLiteralPool()
	{
		if (m_source_page_literal_loads.empty() &&
			m_source_chunk_literal_loads.empty())
			return true;

		// The main hot path has returned before this pool. Cold/helper paths are
		// emitted later and their patched branches jump over the shared words.
		if (!m_source_page_literal_loads.empty())
		{
			const size_t literal_offset = m_code.Size();
			if (!m_code.EmitU32(static_cast<u32>(
					reinterpret_cast<uptr>(m_ram_source_page_live_flags))))
			{
				return false;
			}
			for (const size_t load : m_source_page_literal_loads)
			{
				if (!m_code.PatchLdrLiteral(load, literal_offset))
				{
					m_source_page_literal_out_of_range = true;
					return false;
				}
			}
		}
		if (!m_source_chunk_literal_loads.empty())
		{
			const size_t literal_offset = m_code.Size();
			if (!m_code.EmitU32(static_cast<u32>(
					reinterpret_cast<uptr>(m_ram_source_chunk_live_flags))))
			{
				return false;
			}
			for (const size_t load : m_source_chunk_literal_loads)
			{
				if (!m_code.PatchLdrLiteral(load, literal_offset))
				{
					m_source_page_literal_out_of_range = true;
					return false;
				}
			}
		}
		m_source_page_literal_instructions_removed +=
			static_cast<u32>(m_source_page_literal_loads.size() +
				m_source_chunk_literal_loads.size());
		return true;
	}

	bool BlockCompiler::EmitScalarStoreColdTail(const ScalarStoreColdTail& tail)
	{
		// PCSX2 owners: IopMem.cpp::iopMemWrite8/16/32 and
		// x86/iR3000Atables.cpp::rpsxStore(). MMIO/ROM, alignment, and
		// isolate-cache paths call the existing helper; writable RAM falls
		// through after the direct store and psxCpu->Clear() invalidation.
		const size_t fallback_target = m_code.Size();
		if (!m_code.PatchBranch(tail.fallback_branch, fallback_target,
				VitaA32::Condition::NE))
			return false;
		if (tail.alignment_fallback_branch != static_cast<size_t>(-1) &&
			!m_code.PatchBranch(tail.alignment_fallback_branch, fallback_target,
				VitaA32::Condition::NE))
		{
			return false;
		}
		if ((tail.isolated_fallback_branch != static_cast<size_t>(-1) &&
				!m_code.PatchBranch(tail.isolated_fallback_branch, fallback_target,
					VitaA32::Condition::NE)) ||
			!m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_SAVED0,
				VitaA32::ShiftType::LSL, 0) ||
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

	bool BlockCompiler::EmitUnalignedReadColdTail(
		const UnalignedReadColdTail& tail)
	{
		// PCSX2 owners: R3000AOpcodeTables.cpp::psxLWL/psxLWR/psxSWL/psxSWR
		// merge an aligned iopMemRead32() word. Handler-backed addresses call
		// the helper; ordinary IOP RAM falls through with HOST_TMP0 loaded.
		const size_t fallback_target = m_code.Size();
		if (!m_code.PatchBranch(tail.fallback_branch, fallback_target,
				VitaA32::Condition::NE) ||
			!m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_SAVED1,
				VitaA32::ShiftType::LSL, 0) ||
			!m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&iopMemRead32),
				HOST_CALL_SCRATCH))
		{
			return false;
		}

		const size_t tail_done = m_code.EmitBranchPlaceholder();
		return tail_done != static_cast<size_t>(-1) &&
			   m_code.PatchBranch(tail_done, tail.join_offset);
	}

	bool BlockCompiler::EmitUnalignedWriteColdTail(
		const UnalignedWriteColdTail& tail)
	{
		// PCSX2 owner: IopMem.cpp::iopMemWrite32(). The merged word is already
		// in HOST_TMP1 when the fallback branch fires; writable RAM falls
		// through after the direct write and invalidation.
		const size_t fallback_target = m_code.Size();
		if (!m_code.PatchBranch(tail.write_fallback_branch, fallback_target,
				VitaA32::Condition::NE) ||
			(tail.isolated_fallback_branch != static_cast<size_t>(-1) &&
				!m_code.PatchBranch(tail.isolated_fallback_branch, fallback_target,
					VitaA32::Condition::NE)) ||
			!m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_SAVED1,
				VitaA32::ShiftType::LSL, 0) ||
			!m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&iopMemWrite32),
				HOST_CALL_SCRATCH))
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
		if (!m_code.PatchBranch(tail.fallback_branch, fallback_target,
				VitaA32::Condition::NE) ||
			!m_code.PatchBranch(tail.alignment_fallback_branch, fallback_target,
				VitaA32::Condition::NE) ||
			!m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_SAVED0,
				VitaA32::ShiftType::LSL, 0) ||
			!m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&iopMemRead32),
				HOST_CALL_SCRATCH) ||
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
		if (!m_code.PatchBranch(tail.fallback_branch, fallback_target,
				VitaA32::Condition::NE) ||
			!m_code.PatchBranch(tail.alignment_fallback_branch, fallback_target,
				VitaA32::Condition::NE) ||
			(tail.isolated_fallback_branch != static_cast<size_t>(-1) &&
				!m_code.PatchBranch(tail.isolated_fallback_branch, fallback_target,
					VitaA32::Condition::NE)) ||
			!m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_SAVED1,
				VitaA32::ShiftType::LSL, 0) ||
			!m_code.EmitMovRegShiftImm(HOST_TMP1, HOST_SAVED0,
				VitaA32::ShiftType::LSL, 0) ||
			!m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&iopMemWrite32),
				HOST_CALL_SCRATCH))
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
			!m_code.EmitMovRegShiftImm(HOST_SAVED0, HOST_SAVED0,
				VitaA32::ShiftType::LSL, 3) ||
			!m_code.EmitBicImm32(HOST_SAVED1, HOST_TMP0, 3) ||
			!m_code.EmitTstImm32(HOST_SAVED1, 0x1f800000u))
		{
			return false;
		}

		// PCSX2 owner: R3000AOpcodeTables.cpp::psxLWL/psxLWR use iopMemRead32()
		// on the aligned address. Ordinary IOP RAM can read iopMem->Main directly.
		const size_t fallback_branch =
			m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
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
			       m_code.EmitMovImm32(
					   HOST_CALL_SCRATCH,
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
										VitaA32::ShiftType shift_type,
										u8 amount) -> bool {
			if (amount == 0)
				return m_code.EmitOrrRegShiftImm(rd, rn, rm, VitaA32::ShiftType::LSL, 0);
			return m_code.EmitOrrRegShiftImm(rd, rn, rm, shift_type, amount);
		};

		// PCSX2 owners: R3000AOpcodeTables.cpp::psxSWL()/psxSWR() merge an
		// aligned iopMemRead32() word, then IopMem.cpp::iopMemWrite32() applies
		// isolate-cache suppression and psxCpu->Clear() invalidation.
		bool used_known_store_value = false;
		if (!emit_load_aligned_word() ||
			!EmitLoadGprValue(RT(op), HOST_TMP1, &used_known_store_value))
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
					VitaA32::ShiftType::LSL,
					static_cast<u8>(shift)))
			{
				return false;
			}
		}

		size_t isolated_skip = static_cast<size_t>(-1);
		if (!EmitIsolateCacheGuard(&isolated_skip) || !emit_store_merged_word() ||
			!emit_clear_stored_word())
		{
			return false;
		}

		if (isolated_skip != static_cast<size_t>(-1) &&
			!m_code.PatchBranch(isolated_skip, m_code.Size(), VitaA32::Condition::NE))
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
			return m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_SAVED1,
					   VitaA32::ShiftType::LSL, 0) &&
				   m_code.EmitMovImm8(HOST_TMP1, 1) &&
			       m_code.EmitMovImm32(
					   HOST_CALL_SCRATCH,
					   static_cast<u32>(reinterpret_cast<uptr>(&psxCpu))) &&
				   m_code.EmitLdrImm12(HOST_CALL_SCRATCH, HOST_CALL_SCRATCH, 0) &&
				   m_code.EmitLdrImm12(HOST_CALL_SCRATCH, HOST_CALL_SCRATCH,
					   static_cast<u16>(offsetof(R3000Acpu, Clear))) &&
				   m_code.EmitBlx(HOST_CALL_SCRATCH);
		};

		if (!EmitEffectiveAddress(op) ||
			!m_code.EmitAndImm8(HOST_SAVED0, HOST_TMP0, 3) ||
			!m_code.EmitMovRegShiftImm(HOST_SAVED0, HOST_SAVED0,
				VitaA32::ShiftType::LSL, 3) ||
			!m_code.EmitBicImm32(HOST_SAVED1, HOST_TMP0, 3) ||
			!m_code.EmitTstImm32(HOST_SAVED1, 0x1f800000u))
		{
			return false;
		}

		// PCSX2 owners: R3000AOpcodeTables.cpp::psxSWL/psxSWR merge the aligned
		// word, while IopMem.cpp::iopMemWrite32() owns writable-RAM filtering,
		// isolate-cache suppression, and psxCpu->Clear() invalidation.
		const size_t fallback_branch =
			m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
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
						 VitaA32::ShiftType::LSL,
						 HOST_SAVED0)) ||
			!m_code.EmitTstImm32(HOST_SAVED1, 0x1f800000u))
		{
			return false;
		}

		const size_t write_fallback_branch =
			m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
		size_t isolated_fallback_branch = static_cast<size_t>(-1);
		if (write_fallback_branch == static_cast<size_t>(-1) ||
			!EmitIsolateCacheGuard(&isolated_fallback_branch))
		{
			return false;
		}

		if (!m_code.EmitAndReg(HOST_TMP0, HOST_SAVED1, HOST_IOP_RAM_MASK) ||
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
			   EmitInterpreterTraceBranchHelperExit(
				   reinterpret_cast<const void*>(&psxDoBranch)) &&
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

		VitaA32::Condition taken = VitaA32::Condition::AL;
		const bool producer_flags =
			m_branch_predicate_producer_flags_live &&
			((RS(op) == m_branch_predicate_producer_guest && RT(op) == 0) ||
				(RT(op) == m_branch_predicate_producer_guest && RS(op) == 0));
		if (producer_flags)
		{
			const VitaA32::Condition predicate_true =
				m_branch_predicate_producer_true_condition;
			if (predicate_true == VitaA32::Condition::LT)
				taken = branch_on_equal ? VitaA32::Condition::GE : VitaA32::Condition::LT;
			else if (predicate_true == VitaA32::Condition::CC)
				taken = branch_on_equal ? VitaA32::Condition::CS : VitaA32::Condition::CC;
			else
				return false;

			m_branch_predicate_producer_flags_live = false;
			m_producer_branch_compare_instructions_removed = 1;

			// Preserve the earlier pin-residency attribution when the producer
			// removes the branch operand read entirely. No load or move is emitted,
			// but the same resident mapping still owns that avoided architectural
			// access in enabled/disabled controls.
			bool direct_compare = true;
#if defined(VITASX2_QEMU_VALIDATION)
			direct_compare = s_qemuIopPinnedBranchDirectCompareEnabled;
#endif
			if (direct_compare &&
				PinnedHostForGuest(m_branch_predicate_producer_guest) >= 0)
			{
				m_pinned_gpr_load_hits++;
				m_pinned_branch_operand_moves_removed++;
			}
		}
		else
		{
			if (!EmitCompareGprs(RS(op), RT(op)))
				return false;
			taken = branch_on_equal ? VitaA32::Condition::EQ : VitaA32::Condition::NE;
		}
		if (m_emit_native_static_branch_flags)
		{
			m_static_branch_flags_live = true;
			m_static_branch_taken_condition = taken;
			m_condition_code_branch_instructions_removed = 3;
			return true;
		}

		if (!m_code.EmitMovImm8(HOST_BRANCH_FLAG, 0))
			return false;

		return m_code.EmitMovImm8(HOST_BRANCH_FLAG, 1, taken);
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
				   EmitInterpreterTraceBranchHelperExit(
					   reinterpret_cast<const void*>(&psxDoBranch));
		}

		if (!EmitLoadGpr(RS(op), HOST_TMP0) || !m_code.EmitCmpImm32(HOST_TMP0, 0))
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
			   EmitInterpreterTraceBranchHelperExit(
				   reinterpret_cast<const void*>(&psxDoBranch)) &&
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
			m_static_branch_taken =
				taken == VitaA32::Condition::GE || taken == VitaA32::Condition::LE;
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

		unsigned compare_host = HOST_TMP0;
		bool direct_compare = true;
#if defined(VITASX2_QEMU_VALIDATION)
		direct_compare = s_qemuIopPinnedBranchDirectCompareEnabled;
#endif
		const int pinned_host = direct_compare ? PinnedHostForGuest(RS(op)) : -1;
		if (pinned_host >= 0)
		{
			compare_host = static_cast<unsigned>(pinned_host);
			m_pinned_gpr_load_hits++;
			m_pinned_branch_operand_moves_removed++;
		}

		if (!(pinned_host >= 0 || EmitLoadGpr(RS(op), HOST_TMP0)) ||
			!m_code.EmitCmpImm32(compare_host, 0))
		{
			return false;
		}
		if (m_emit_native_static_branch_flags)
		{
			m_static_branch_flags_live = true;
			m_static_branch_taken_condition = taken;
			m_condition_code_branch_instructions_removed = 3;
			return true;
		}

		return m_code.EmitMovImm8(HOST_BRANCH_FLAG, 0) &&
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
				!m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&psxDoBranch),
					HOST_CALL_SCRATCH))
			{
				return false;
			}
		}
		else
		{
			if (!m_code.EmitMovImm32(HOST_TMP0, JumpTarget(pc, op)) ||
				!m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&psxDoJump),
					HOST_CALL_SCRATCH))
			{
				return false;
			}
		}

		return m_writes_isolate_mode ? EndBlockIsolateModeWriteReturn() : EndBlockReturn(BlockExitKind::Direct);
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
			   EmitInterpreterTraceBranchHelperExit(
				   reinterpret_cast<const void*>(&psxDoBranch));
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

		if (known_target_available)
		{
			// PCSX2 owners: R3000AInterpreter.cpp::psxJR()/psxJALR() and
			// x86/iR3000Atables.cpp::rpsxJR()/rpsxJALR(). Constant JR/JALR
			// targets can use the static direct-link tail after the delay slot.
			// iopRecRecompile()'s rare target-entry effects run on the target's
			// genuine cache miss rather than before this source branch's delay slot.
			m_register_jump_target_known = true;
			m_register_jump_target = known_target;
#if defined(VITASX2_QEMU_VALIDATION)
			++g_qemuIopConstRegisterJumpFastPaths;
#endif
			return true;
		}

		return EmitLoadGpr(RS(op), HOST_REGISTER_JUMP_TARGET);
	}

	bool BlockCompiler::BranchTestSchedulingEnabled() const
	{
#if defined(VITASX2_QEMU_VALIDATION)
		return s_qemuIopBranchTestSchedulingEnabled;
#else
		return true;
#endif
	}

	bool BlockCompiler::EmitReloadPublishedEventCountdown()
	{
		// The normal Vita owner publishes a near deadline, but savestates and
		// diagnostic fixtures may legally restore an arbitrary 64-bit value.
		// Represent only a non-negative distance <= INT32_MAX in r8. UINT32_MAX
		// is the fail-closed marker which selects the complete 64-bit comparison
		// at the next branch seam. This entry calculation runs once per linked
		// chain and remains out of every region backedge.
		return m_code.EmitCallAbsolute(
				   reinterpret_cast<const void*>(&VitaIopA32LoadPublishedEventCountdown),
				   HOST_CALL_SCRATCH) &&
		       m_code.EmitMovRegShiftImm(HOST_REGISTER_JUMP_TARGET, HOST_TMP0,
				   VitaA32::ShiftType::LSL, 0);
	}

	bool BlockCompiler::EmitConsumePublishedEventCountdown()
	{
		// A logical BaseBlock can end at an artificial fallthrough seam rather
		// than iPsxBranchTest(). It still publishes its accumulated IOP cycles
		// before a direct link, so carry the resident distance forward by the
		// same exact amount without dispatching an event at a seam PCSX2 does
		// not own. Preserve UINT32_MAX as the complete-comparison sentinel.
		if (!m_code.EmitCmpImm32(HOST_REGISTER_JUMP_TARGET, UINT32_MAX))
			return false;
		const size_t done =
			m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
		if (done == static_cast<size_t>(-1))
			return false;

		const bool subtracted =
			m_code.EmitSubImm32(HOST_REGISTER_JUMP_TARGET,
				HOST_REGISTER_JUMP_TARGET, m_budget_cycle_count) ||
			(m_code.EmitMovImm32(HOST_TMP0, m_budget_cycle_count) &&
			 m_code.EmitSubReg(HOST_REGISTER_JUMP_TARGET,
				 HOST_REGISTER_JUMP_TARGET, HOST_TMP0));
		return subtracted &&
		       m_code.PatchBranch(done, m_code.Size(), VitaA32::Condition::EQ);
	}

	bool BlockCompiler::EmitQemuCounterIncrement(u32* counter)
	{
#if defined(VITASX2_QEMU_VALIDATION)
		if (!s_qemuIopGeneratedInstrumentationEnabled)
			return true;
		return counter &&
		       m_code.EmitMovImm32(
				   HOST_TMP2, static_cast<u32>(reinterpret_cast<uptr>(counter))) &&
			m_code.EmitLdrImm12(HOST_TMP3, HOST_TMP2, 0) &&
			m_code.EmitAddImm8(HOST_TMP3, HOST_TMP3, 1) &&
			m_code.EmitStrImm12(HOST_TMP3, HOST_TMP2, 0);
#else
		(void)counter;
		return true;
#endif
	}

	bool BlockCompiler::EmitBranchEventTest(u8 scheduler_resume_slot)
	{
		// PCSX2 owner: x86/iR3000A.cpp::iPsxBranchTest() subtracts the
		// completed block from iopCycleEE and exits on <= 0 before checking any
		// IOP event. Keep dirty pins private until that decision; the dedicated
		// budget-exit tail publishes them only on the exiting path.
#if defined(VITASX2_QEMU_VALIDATION)
		if (!EmitQemuCounterIncrement(&s_qemuIopBranchEventCandidates))
			return false;
#endif
		if (BranchTestSchedulingEnabled() &&
			!EmitChargeEeBudget(0, false, scheduler_resume_slot))
			return false;
#if defined(VITASX2_QEMU_VALIDATION)
		if (!EmitQemuCounterIncrement(&s_qemuIopBranchEventBudgetPositive))
			return false;
#endif

		if (!BranchTestSchedulingEnabled())
			return EmitIopEventTestFastPath();

		// PCSX2's next iPsxBranchTest() seam compares the completed IOP cycle
		// against iopNextEventCycle and enters iopEventTest() only when due.
		// Helper-free scalar regions retain a bounded distance to the published
		// deadline in callee-saved r8. Each linked BaseBlock consumes its exact
		// private cycle total. UINT32_MAX marks an arbitrary restored horizon and
		// selects the complete PCSX2 comparison.
		if (m_resident_event_deadline)
		{
			if (!m_code.EmitCmpImm32(HOST_REGISTER_JUMP_TARGET, UINT32_MAX))
				return false;
			const size_t full_compare =
				m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
			if (full_compare == static_cast<size_t>(-1))
				return false;

			const bool subtracted =
				m_code.EmitSubImm32(HOST_REGISTER_JUMP_TARGET,
					HOST_REGISTER_JUMP_TARGET, m_budget_cycle_count, true) ||
				(m_code.EmitMovImm32(HOST_TMP0, m_budget_cycle_count) &&
				 m_code.EmitSubReg(HOST_REGISTER_JUMP_TARGET,
					 HOST_REGISTER_JUMP_TARGET, HOST_TMP0, true));
			if (!subtracted)
				return false;
			const size_t not_due =
				m_code.EmitBranchPlaceholder(VitaA32::Condition::GT);
			if (not_due == static_cast<size_t>(-1) ||
				!EmitIopEventTestFastPath() ||
				!EmitReloadPublishedEventCountdown())
			{
				return false;
			}
			const size_t skip_full = m_code.EmitBranchPlaceholder();
			const size_t full = m_code.Size();
			if (skip_full == static_cast<size_t>(-1) ||
				!m_code.PatchBranch(full_compare, full, VitaA32::Condition::EQ))
			{
				return false;
			}

			if (!m_code.EmitCallAbsolute(
					reinterpret_cast<const void*>(
						&VitaIopA32TestEventAndLoadPublishedCountdown),
					HOST_CALL_SCRATCH) ||
				!m_code.EmitMovRegShiftImm(HOST_REGISTER_JUMP_TARGET, HOST_TMP0,
					VitaA32::ShiftType::LSL, 0))
			{
				return false;
			}
			const size_t done = m_code.Size();
			return m_code.PatchBranch(not_due, done, VitaA32::Condition::GT) &&
			       m_code.PatchBranch(skip_full, done);
		}

		// SUBS/SBCS plus MI reproduces x86's signed 64-bit SUB/JS test.
		if (!(m_code.EmitAddImm32(HOST_CALL_SCRATCH, HOST_PSX_REGS,
				static_cast<u32>(CYCLE_OFFSET)) ||
				(m_code.EmitMovImm32(HOST_CALL_SCRATCH,
					static_cast<u32>(CYCLE_OFFSET)) &&
				 m_code.EmitAddReg(HOST_CALL_SCRATCH, HOST_PSX_REGS,
					HOST_CALL_SCRATCH))) ||
			!m_code.EmitLdrdImm8(HOST_TMP0, HOST_TMP1, HOST_CALL_SCRATCH, 0) ||
			!m_code.EmitLdrdImm8(
				HOST_TMP2, HOST_TMP3, HOST_CALL_SCRATCH,
				static_cast<u8>(IOP_NEXT_EVENT_CYCLE_FROM_CYCLE_OFFSET)) ||
			!m_code.EmitSubReg(HOST_TMP0, HOST_TMP0, HOST_TMP2, true) ||
			!m_code.EmitSbcReg(HOST_TMP1, HOST_TMP1, HOST_TMP3, true))
		{
			return false;
		}
		const size_t not_due = m_code.EmitBranchPlaceholder(VitaA32::Condition::MI);
		return not_due != static_cast<size_t>(-1) && EmitIopEventTestFastPath() &&
			m_code.PatchBranch(not_due, m_code.Size(), VitaA32::Condition::MI);
	}

	bool BlockCompiler::EmitIopEventTestFastPath()
	{
		// PCSX2 owner: x86/iR3000A.cpp::iPsxBranchTest() calls the one
		// R3000A.cpp::iopEventTest() owner only after the generated signed
		// 64-bit deadline check says work is due. Duplicating a second, partial
		// copy of that rare body in every branch block made two-instruction IOP
		// blocks exceed 130 A32 words and scattered their not-due continuation
		// across several Cortex-A9 I-cache lines. Keep the common deadline test
		// inline and put the due body behind its single authoritative function.
#if defined(VITASX2_QEMU_VALIDATION)
		if (!EmitQemuCounterIncrement(&s_qemuIopBranchEventTestsEntered))
			return false;
#endif
		return m_code.EmitCallAbsolute(
			reinterpret_cast<const void*>(&iopEventTest), HOST_CALL_SCRATCH);
	}

	bool BlockCompiler::EmitCop0TransferOp(u32 op, bool to_cop0)
	{
		if (to_cop0)
		{
			const bool isolate_mode_write =
				m_isolate_cache_specialization && IsIopCop0StatusWriteOpcode(op);
			if (isolate_mode_write)
				m_writes_isolate_mode = true;
			bool used_known_value = false;
			if (!EmitLoadGprValue(RT(op), HOST_TMP0, &used_known_value))
			{
				return false;
			}
			if (!m_code.EmitStrImm12(HOST_TMP0, HOST_PSX_REGS,
					static_cast<u16>(Cp0Offset(RD(op)))))
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

		return m_code.EmitLdrImm12(HOST_TMP0, HOST_PSX_REGS,
				   static_cast<u16>(Cp0Offset(RD(op)))) &&
			   EmitStoreGpr(RT(op), HOST_TMP0);
	}

	bool BlockCompiler::EmitCop0RfeOp()
	{
		return m_code.EmitLdrImm12(HOST_TMP0, HOST_PSX_REGS,
				   static_cast<u16>(CP0_STATUS_OFFSET)) &&
			   m_code.EmitBicImm32(HOST_TMP2, HOST_TMP0, 0x0f) &&
			   m_code.EmitAndImm32(HOST_TMP0, HOST_TMP0, 0x3cu) &&
		       m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_TMP0,
				   VitaA32::ShiftType::LSR, 2) &&
			   m_code.EmitOrrReg(HOST_TMP2, HOST_TMP2, HOST_TMP0) &&
		       m_code.EmitStrImm12(HOST_TMP2, HOST_PSX_REGS,
				   static_cast<u16>(CP0_STATUS_OFFSET)) &&
		       EmitPrivateCycleHelperCall(
				   reinterpret_cast<const void*>(&iopTestIntc));
	}

	bool BlockCompiler::EmitCop2CommandOp(u32 op)
	{
		// PCSX2 owner: R3000AOpcodeTables.cpp::psxCP2 dispatches by function
		// to IopGte.cpp. Direct calls remove the generic COP2 table tail while
		// preserving PCSX2's GTE command implementation.
		const void* helper = nullptr;
		switch (op & 0x3f)
		{
			case 0x01:
				helper = reinterpret_cast<const void*>(&gteRTPS);
				break;
			case 0x06:
				helper = reinterpret_cast<const void*>(&gteNCLIP);
				break;
			case 0x0c:
				helper = reinterpret_cast<const void*>(&gteOP);
				break;
			case 0x10:
				helper = reinterpret_cast<const void*>(&gteDPCS);
				break;
			case 0x11:
				helper = reinterpret_cast<const void*>(&gteINTPL);
				break;
			case 0x12:
				helper = reinterpret_cast<const void*>(&gteMVMVA);
				break;
			case 0x13:
				helper = reinterpret_cast<const void*>(&gteNCDS);
				break;
			case 0x14:
				helper = reinterpret_cast<const void*>(&gteCDP);
				break;
			case 0x16:
				helper = reinterpret_cast<const void*>(&gteNCDT);
				break;
			case 0x1b:
				helper = reinterpret_cast<const void*>(&gteNCCS);
				break;
			case 0x1c:
				helper = reinterpret_cast<const void*>(&gteCC);
				break;
			case 0x1e:
				helper = reinterpret_cast<const void*>(&gteNCS);
				break;
			case 0x20:
				helper = reinterpret_cast<const void*>(&gteNCT);
				break;
			case 0x28:
				helper = reinterpret_cast<const void*>(&gteSQR);
				break;
			case 0x29:
				helper = reinterpret_cast<const void*>(&gteDCPL);
				break;
			case 0x2a:
				helper = reinterpret_cast<const void*>(&gteDPCT);
				break;
			case 0x2d:
				helper = reinterpret_cast<const void*>(&gteAVSZ3);
				break;
			case 0x2e:
				helper = reinterpret_cast<const void*>(&gteAVSZ4);
				break;
			case 0x30:
				helper = reinterpret_cast<const void*>(&gteRTPT);
				break;
			case 0x3d:
				helper = reinterpret_cast<const void*>(&gteGPF);
				break;
			case 0x3e:
				helper = reinterpret_cast<const void*>(&gteGPL);
				break;
			case 0x3f:
				helper = reinterpret_cast<const void*>(&gteNCCT);
				break;
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
			return m_code.EmitLdrImm12(host_reg, HOST_PSX_REGS,
				static_cast<u16>(Cp2dOffset(cop2_reg)));

		if (!m_code.EmitLdrImm12(HOST_TMP0, HOST_PSX_REGS,
				static_cast<u16>(Cp2dOffset(9))) ||
			!m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_TMP0, VitaA32::ShiftType::LSR,
				7) ||
			!m_code.EmitAndImm8(HOST_TMP0, HOST_TMP0, 0x1f) ||
			!m_code.EmitLdrImm12(HOST_TMP1, HOST_PSX_REGS,
				static_cast<u16>(Cp2dOffset(10))) ||
			!m_code.EmitMovRegShiftImm(HOST_TMP1, HOST_TMP1, VitaA32::ShiftType::LSR,
				7) ||
			!m_code.EmitAndImm8(HOST_TMP1, HOST_TMP1, 0x1f) ||
			!m_code.EmitMovRegShiftImm(HOST_TMP1, HOST_TMP1, VitaA32::ShiftType::LSL,
				5) ||
			!m_code.EmitOrrReg(HOST_TMP0, HOST_TMP0, HOST_TMP1) ||
			!m_code.EmitLdrImm12(HOST_TMP1, HOST_PSX_REGS,
				static_cast<u16>(Cp2dOffset(11))) ||
			!m_code.EmitMovRegShiftImm(HOST_TMP1, HOST_TMP1, VitaA32::ShiftType::LSR,
				7) ||
			!m_code.EmitAndImm8(HOST_TMP1, HOST_TMP1, 0x1f) ||
			!m_code.EmitMovRegShiftImm(HOST_TMP1, HOST_TMP1, VitaA32::ShiftType::LSL,
				10) ||
			!m_code.EmitOrrReg(HOST_TMP0, HOST_TMP0, HOST_TMP1) ||
			!m_code.EmitStrImm12(HOST_TMP0, HOST_PSX_REGS,
				static_cast<u16>(Cp2dOffset(29))))
		{
			return false;
		}

		if (host_reg == HOST_TMP0)
			return true;

		return m_code.EmitMovRegShiftImm(host_reg, HOST_TMP0, VitaA32::ShiftType::LSL,
			0);
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
				       m_code.EmitStrImm12(HOST_TMP1, HOST_PSX_REGS,
						   static_cast<u16>(Cp2dOffset(cop2_reg)));

			case 15:
				return m_code.EmitLdrImm12(HOST_TMP1, HOST_PSX_REGS,
						   static_cast<u16>(Cp2dOffset(13))) &&
				       m_code.EmitLdrImm12(HOST_TMP2, HOST_PSX_REGS,
						   static_cast<u16>(Cp2dOffset(14))) &&
				       m_code.EmitStrImm12(HOST_TMP1, HOST_PSX_REGS,
						   static_cast<u16>(Cp2dOffset(12))) &&
				       m_code.EmitStrImm12(HOST_TMP2, HOST_PSX_REGS,
						   static_cast<u16>(Cp2dOffset(13))) &&
				       m_code.EmitStrImm12(host_reg, HOST_PSX_REGS,
						   static_cast<u16>(Cp2dOffset(14))) &&
				       m_code.EmitStrImm12(host_reg, HOST_PSX_REGS,
						   static_cast<u16>(Cp2dOffset(15)));

			case 16:
			case 17:
			case 18:
			case 19:
				return m_code.EmitUxth(HOST_TMP1, host_reg) &&
				       m_code.EmitStrImm12(HOST_TMP1, HOST_PSX_REGS,
						   static_cast<u16>(Cp2dOffset(cop2_reg)));

			case 28:
				return m_code.EmitStrImm12(host_reg, HOST_PSX_REGS,
						   static_cast<u16>(Cp2dOffset(28))) &&
					   m_code.EmitAndImm8(HOST_TMP1, host_reg, 0x1f) &&
				       m_code.EmitMovRegShiftImm(HOST_TMP1, HOST_TMP1,
						   VitaA32::ShiftType::LSL, 7) &&
				       m_code.EmitStrImm12(HOST_TMP1, HOST_PSX_REGS,
						   static_cast<u16>(Cp2dOffset(9))) &&
				       m_code.EmitMovRegShiftImm(HOST_TMP1, host_reg,
						   VitaA32::ShiftType::LSR, 5) &&
					   m_code.EmitAndImm8(HOST_TMP1, HOST_TMP1, 0x1f) &&
				       m_code.EmitMovRegShiftImm(HOST_TMP1, HOST_TMP1,
						   VitaA32::ShiftType::LSL, 7) &&
				       m_code.EmitStrImm12(HOST_TMP1, HOST_PSX_REGS,
						   static_cast<u16>(Cp2dOffset(10))) &&
				       m_code.EmitMovRegShiftImm(HOST_TMP1, host_reg,
						   VitaA32::ShiftType::LSR, 10) &&
					   m_code.EmitAndImm8(HOST_TMP1, HOST_TMP1, 0x1f) &&
				       m_code.EmitMovRegShiftImm(HOST_TMP1, HOST_TMP1,
						   VitaA32::ShiftType::LSL, 7) &&
				       m_code.EmitStrImm12(HOST_TMP1, HOST_PSX_REGS,
						   static_cast<u16>(Cp2dOffset(11)));

			case 30:
				return m_code.EmitStrImm12(host_reg, HOST_PSX_REGS,
						   static_cast<u16>(Cp2dOffset(30))) &&
				       m_code.EmitMovRegShiftImm(HOST_TMP1, host_reg,
						   VitaA32::ShiftType::ASR, 31) &&
					   m_code.EmitEorReg(HOST_TMP1, host_reg, HOST_TMP1) &&
					   m_code.EmitClz(HOST_TMP1, HOST_TMP1) &&
				       m_code.EmitStrImm12(HOST_TMP1, HOST_PSX_REGS,
						   static_cast<u16>(Cp2dOffset(31)));

			default:
				return m_code.EmitStrImm12(host_reg, HOST_PSX_REGS,
					static_cast<u16>(Cp2dOffset(cop2_reg)));
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
				return m_code.EmitLdrImm12(HOST_TMP0, HOST_PSX_REGS,
						   static_cast<u16>(Cp2cOffset(RD(op)))) &&
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
					!m_code.EmitStrImm12(HOST_TMP0, HOST_PSX_REGS,
						static_cast<u16>(Cp2cOffset(RD(op)))))
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
				return m_code.EmitLdrImm12(HOST_TMP0, HOST_IOP_RAM_BASE,
					static_cast<u16>(address));

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
				return m_code.EmitStrImm12(HOST_SAVED0, HOST_IOP_RAM_BASE,
					static_cast<u16>(address));

			return m_code.EmitMovImm32(HOST_TMP0, address) &&
				   m_code.EmitStrRegShift(HOST_SAVED0, HOST_IOP_RAM_BASE, HOST_TMP0,
					   VitaA32::ShiftType::LSL, 0);
		};
		const auto emit_clear_stored_word = [&]() -> bool {
			return m_code.EmitMovImm32(HOST_TMP0, address & ~3u) &&
				   m_code.EmitMovImm8(HOST_TMP1, 1) &&
			       m_code.EmitMovImm32(
					   HOST_CALL_SCRATCH,
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
		size_t isolated_skip = static_cast<size_t>(-1);
		if (!EmitReadCop2DataReg(RT(op), HOST_SAVED0) ||
			!EmitIsolateCacheGuard(&isolated_skip))
		{
			return false;
		}

		if (!emit_store_word() || !emit_clear_stored_word())
		{
			return false;
		}

		return isolated_skip == static_cast<size_t>(-1) ||
		       m_code.PatchBranch(isolated_skip, m_code.Size(),
				   VitaA32::Condition::NE);
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
				!m_code.EmitMovRegShiftImm(HOST_SAVED0, HOST_TMP0,
					VitaA32::ShiftType::LSL, 0) ||
				!m_code.EmitTstImm32(HOST_TMP0, 0x1f800000u))
			{
				return false;
			}

			// PCSX2 owner: IopGte.cpp::gteLWC2() reads with iopMemRead32() before
			// MTC2 side effects. Mirror rpsxLoad's ordinary-RAM fast split here.
			const size_t fallback_branch =
				m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (fallback_branch == static_cast<size_t>(-1) ||
				!m_code.EmitAndImm8(HOST_TMP2, HOST_SAVED0, 3, true))
			{
				return false;
			}

			const size_t alignment_fallback_branch =
				m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
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
				       m_code.EmitMovImm32(
						   HOST_CALL_SCRATCH,
						   static_cast<u32>(reinterpret_cast<uptr>(&psxCpu))) &&
					   m_code.EmitLdrImm12(HOST_CALL_SCRATCH, HOST_CALL_SCRATCH, 0) &&
				       m_code.EmitLdrImm12(
						   HOST_CALL_SCRATCH, HOST_CALL_SCRATCH,
						   static_cast<u16>(offsetof(R3000Acpu, Clear))) &&
					   m_code.EmitBlx(HOST_CALL_SCRATCH);
			};

			if (!EmitReadCop2DataReg(RT(op), HOST_SAVED0) ||
				!EmitEffectiveAddress(op) ||
				!m_code.EmitMovRegShiftImm(HOST_SAVED1, HOST_TMP0,
					VitaA32::ShiftType::LSL, 0) ||
				!m_code.EmitTstImm32(HOST_TMP0, 0x1f800000u))
			{
				return false;
			}

			// PCSX2 owner: IopGte.cpp::gteSWC2() writes MFC2(_Rt_) through
			// iopMemWrite32(). Ordinary writable RAM can store directly, but
			// must retain iopMemWrite32()'s isolate-cache and invalidation rules.
			const size_t fallback_branch =
				m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (fallback_branch == static_cast<size_t>(-1) ||
				!m_code.EmitAndImm8(HOST_TMP2, HOST_SAVED1, 3, true))
			{
				return false;
			}

			const size_t alignment_fallback_branch =
				m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			size_t isolated_fallback_branch = static_cast<size_t>(-1);
			if (alignment_fallback_branch == static_cast<size_t>(-1) ||
				!EmitIsolateCacheGuard(&isolated_fallback_branch))
			{
				return false;
			}

			if (!m_code.EmitAndReg(HOST_TMP0, HOST_SAVED1, HOST_IOP_RAM_MASK) ||
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
					!m_code.EmitStrImm12(HOST_TMP0, HOST_PSX_REGS,
						static_cast<u16>(HI_OFFSET)))
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
				if (RD(op) != 0 && m_retained_lo_host_valid)
				{
#if defined(VITASX2_QEMU_VALIDATION)
					++g_qemuIopRetainedLoReadFastPaths;
#endif
					return EmitStoreGpr(RD(op), HOST_TMP3);
				}
				return EmitMoveGpr(RD(op), 33);
			}
			case 0x13: // MTLO
			{
				bool used_known_value = false;
				if (!EmitLoadGprValue(RS(op), HOST_TMP0, &used_known_value) ||
					!m_code.EmitStrImm12(HOST_TMP0, HOST_PSX_REGS,
						static_cast<u16>(LO_OFFSET)))
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
				return EmitImmediateOp(op, pc);
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

	bool BlockCompiler::MatchSequentialQwordCopyShape(
		u32 start_pc, u32 instruction_index, u32 instruction_count,
		SequentialQwordCopy* copy) const
	{
		if (!copy || instruction_index + 8 > instruction_count)
			return false;

		SequentialQwordCopy candidate;
		for (u32 i = 0; i < 4; i++)
		{
			candidate.load_ops[i] =
				iopMemRead32(start_pc + (instruction_index + i) * sizeof(u32));
			candidate.store_ops[i] =
				iopMemRead32(start_pc + (instruction_index + 4 + i) * sizeof(u32));
		}
		const u32 first_load = candidate.load_ops[0];
		const u32 first_store = candidate.store_ops[0];
		if ((first_load >> 26) != 0x23 || (first_store >> 26) != 0x2b ||
			IMM_S(first_load) != 0 || IMM_S(first_store) != 0 ||
			RT(first_load) == 0 || RT(first_load) > 28)
		{
			return false;
		}

		const unsigned source_base = RS(first_load);
		const unsigned destination_base = RS(first_store);
		candidate.first_result = static_cast<u8>(RT(first_load));
		for (u32 i = 0; i < 4; i++)
		{
			const u32 load = candidate.load_ops[i];
			const u32 store = candidate.store_ops[i];
			const unsigned result = candidate.first_result + i;
			if ((load >> 26) != 0x23 || RS(load) != source_base || RT(load) != result ||
				IMM_S(load) != static_cast<s16>(i * 4) || (store >> 26) != 0x2b ||
				RS(store) != destination_base || RT(store) != result ||
				IMM_S(store) != static_cast<s16>(i * 4) || result == source_base ||
				result == destination_base)
			{
				return false;
			}
		}

		*copy = candidate;
		return true;
	}

	bool BlockCompiler::MatchSequentialQwordCopy(u32 start_pc,
		u32 instruction_index,
		u32 instruction_count,
		SequentialQwordCopy* copy) const
	{
		SequentialQwordCopy candidate;
		if (!MatchSequentialQwordCopyShape(start_pc, instruction_index,
				instruction_count, &candidate) ||
			m_emit_trace_checks || !m_defer_cycle_updates ||
			!m_iop_ram_registers_available ||
			!m_iop_ram_mask_register_available || !m_isolate_cache_specialization ||
			!m_isolate_cache_guard_stable || m_isolate_cache_active ||
			!m_ram_source_page_live_flags || !m_ram_source_chunk_live_flags ||
			Ps2MemSize::ExposedIopRam != Ps2MemSize::IopRam)
		{
			return false;
		}
#if defined(VITASX2_QEMU_VALIDATION)
		if (!s_qemuIopSequentialQwordCopyEnabled ||
			!s_qemuIopRamProvenanceSpecializationEnabled)
		{
			return false;
		}
#endif
		for (u32 i = 0; i < 4; i++)
		{
			if (PinnedHostForGuest(candidate.first_result + i) >= 0)
				return false;
		}
		*copy = candidate;
		return true;
	}

	bool BlockCompiler::EmitSequentialQwordCopy(const SequentialQwordCopy& copy,
		u32 start_pc,
		u32 instruction_index)
	{
		// PCSX2 owners: x86/iR3000Atables.cpp::rpsxLoad()/rpsxSW(),
		// IopMem.cpp::iopMemRead32()/iopMemWrite32(), and
		// x86/iR3000A.cpp::PSXREC_CLEARM. The fast arm coalesces the exact
		// directly mapped result of four adjacent reads followed by the same four
		// adjacent ordinary-RAM writes. Main RAM aliases and the immutable 4 MiB
		// BIOS ROM use their owning backing arrays; every rejected mapping,
		// alignment, wrap, or destination-page case executes the owner helpers in
		// original instruction order while the complete BaseBlock cycle delta
		// remains private. The owner tail publishes it once.
		constexpr u32 ram_mapping_guard_mask = 0x1f800000u;
		std::array<size_t, 8> fallback_branches{};
		fallback_branches.fill(static_cast<size_t>(-1));
		if (!EmitEffectiveAddress(copy.load_ops[0], HOST_SAVED0) ||
			!EmitEffectiveAddress(copy.store_ops[0], HOST_TMP3) ||
			!m_code.EmitTstImm32(HOST_TMP3, ram_mapping_guard_mask))
		{
			return false;
		}
		fallback_branches[0] = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
		if (fallback_branches[0] == static_cast<size_t>(-1) ||
			!m_code.EmitTstImm32(HOST_TMP3, 3u))
		{
			return false;
		}
		fallback_branches[1] = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
		if (fallback_branches[1] == static_cast<size_t>(-1) ||
			!m_code.EmitAddImm8(HOST_TMP0, HOST_TMP3, 15) ||
			!m_code.EmitEorReg(HOST_TMP0, HOST_TMP0, HOST_TMP3) ||
			!m_code.EmitTstImm32(HOST_TMP0, 0x00200000u))
		{
			return false;
		}
		fallback_branches[2] = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
		if (fallback_branches[2] == static_cast<size_t>(-1) ||
			!m_code.EmitTstImm32(HOST_TMP0, 0x00001000u))
		{
			return false;
		}
		fallback_branches[3] = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
		if (fallback_branches[3] == static_cast<size_t>(-1) ||
			!m_code.EmitTstImm32(HOST_SAVED0, 3u))
		{
			return false;
		}
		fallback_branches[4] = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
		if (fallback_branches[4] == static_cast<size_t>(-1) ||
			!m_code.EmitTstImm32(HOST_SAVED0, ram_mapping_guard_mask))
		{
			return false;
		}
		const size_t ram_source =
			m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
		if (ram_source == static_cast<size_t>(-1) ||
			!m_code.EmitEorImm32(HOST_TMP0, HOST_SAVED0, 0x1fc00000u) ||
			!m_code.EmitTstImm32(HOST_TMP0, 0x1fc00000u))
		{
			return false;
		}
		fallback_branches[5] = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
		if (fallback_branches[5] == static_cast<size_t>(-1) ||
			!m_code.EmitBicImm32(HOST_TMP0, HOST_TMP0, 0xe0000000u) ||
			!m_code.EmitAddImm8(HOST_TMP1, HOST_TMP0, 15) ||
			!m_code.EmitTstImm32(HOST_TMP1, Ps2MemSize::Rom))
		{
			return false;
		}
		fallback_branches[6] = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
		if (fallback_branches[6] == static_cast<size_t>(-1) ||
			!m_code.EmitMovImm32(HOST_TMP2,
				static_cast<u32>(reinterpret_cast<uptr>(eeMem->ROM))) ||
			!m_code.EmitAddReg(HOST_TMP0, HOST_TMP2, HOST_TMP0) ||
			!m_code.EmitVld1Q32(0, HOST_TMP0))
		{
			return false;
		}
		const size_t source_ready = m_code.EmitBranchPlaceholder();
		if (source_ready == static_cast<size_t>(-1) ||
			!m_code.PatchBranch(ram_source, m_code.Size(), VitaA32::Condition::EQ) ||
			!m_code.EmitAddImm8(HOST_TMP0, HOST_SAVED0, 15) ||
			!m_code.EmitEorReg(HOST_TMP0, HOST_TMP0, HOST_SAVED0) ||
			!m_code.EmitTstImm32(HOST_TMP0, 0x00200000u))
		{
			return false;
		}
		fallback_branches[7] = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
		if (fallback_branches[7] == static_cast<size_t>(-1) ||
			!m_code.EmitAndReg(HOST_TMP0, HOST_SAVED0, HOST_IOP_RAM_MASK) ||
			!m_code.EmitAddReg(HOST_TMP0, HOST_IOP_RAM_BASE, HOST_TMP0) ||
			!m_code.EmitVld1Q32(0, HOST_TMP0) ||
			!m_code.PatchBranch(source_ready, m_code.Size()))
		{
			return false;
		}

#if defined(VITASX2_QEMU_VALIDATION)
		if (!m_code.EmitMovImm32(HOST_TMP2,
				static_cast<u32>(reinterpret_cast<uptr>(
					&s_qemuIopSequentialQwordCopyFastPaths))) ||
			!m_code.EmitLdrImm12(HOST_TMP0, HOST_TMP2, 0) ||
			!m_code.EmitAddImm8(HOST_TMP0, HOST_TMP0, 1) ||
			!m_code.EmitStrImm12(HOST_TMP0, HOST_TMP2, 0))
		{
			return false;
		}
#endif

		const u32 result_offset = static_cast<u32>(GprOffset(copy.first_result));
		if (!(m_code.EmitAddImm32(HOST_TMP0, HOST_PSX_REGS, result_offset) ||
				(m_code.EmitMovImm32(HOST_TMP0, result_offset) &&
					m_code.EmitAddReg(HOST_TMP0, HOST_PSX_REGS, HOST_TMP0))) ||
			!m_code.EmitVst1Q32(0, HOST_TMP0) ||
			!m_code.EmitAndReg(HOST_TMP1, HOST_TMP3, HOST_IOP_RAM_MASK) ||
			!m_code.EmitAddReg(HOST_TMP0, HOST_IOP_RAM_BASE, HOST_TMP1) ||
			!m_code.EmitVst1Q32(0, HOST_TMP0))
		{
			return false;
		}

		bool source_page_literal_enabled = m_source_page_literal_allowed;
#if defined(VITASX2_QEMU_VALIDATION)
		source_page_literal_enabled =
			source_page_literal_enabled && s_qemuIopSourcePageLiteralEnabled;
#endif
		if (!m_ram_source_page_live_flags)
			return false;
		if (source_page_literal_enabled)
		{
			const size_t load = m_code.EmitLdrLiteralPlaceholder(HOST_TMP2);
			if (load == static_cast<size_t>(-1))
				return false;
			m_source_page_literal_loads.push_back(load);
		}
		else if (!m_code.EmitMovImm32(HOST_TMP2,
					 static_cast<u32>(reinterpret_cast<uptr>(
						 m_ram_source_page_live_flags))))
		{
			return false;
		}
		if (!m_code.EmitLdrbRegShift(HOST_TMP0, HOST_TMP2, HOST_TMP1,
				VitaA32::ShiftType::LSR, 12) ||
			!m_code.EmitCmpImm32(HOST_TMP0, 0))
		{
			return false;
		}
		const size_t no_source_page =
			m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
		if (no_source_page == static_cast<size_t>(-1))
			return false;
		if (source_page_literal_enabled)
		{
			const size_t load = m_code.EmitLdrLiteralPlaceholder(HOST_TMP2);
			if (load == static_cast<size_t>(-1))
				return false;
			m_source_chunk_literal_loads.push_back(load);
		}
		else if (!m_code.EmitMovImm32(HOST_TMP2,
				 static_cast<u32>(reinterpret_cast<uptr>(
					 m_ram_source_chunk_live_flags))))
		{
			return false;
		}
		if (!m_code.EmitLdrbRegShift(HOST_TMP0, HOST_TMP2, HOST_TMP1,
				VitaA32::ShiftType::LSR, 6) ||
			!m_code.EmitCmpImm32(HOST_TMP0, 0))
		{
			return false;
		}
		const size_t first_source_chunk =
			m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
		if (first_source_chunk == static_cast<size_t>(-1) ||
			!m_code.EmitAddImm8(HOST_TMP0, HOST_TMP1, 15) ||
			!m_code.EmitLdrbRegShift(HOST_TMP0, HOST_TMP2, HOST_TMP0,
				VitaA32::ShiftType::LSR, 6) ||
			!m_code.EmitCmpImm32(HOST_TMP0, 0))
		{
			return false;
		}
		const size_t no_source_chunk =
			m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
		const size_t clear_source_chunk = m_code.Size();
		if (no_source_chunk == static_cast<size_t>(-1) ||
			!m_code.PatchBranch(first_source_chunk, clear_source_chunk,
				VitaA32::Condition::NE) ||
			!m_code.EmitBicImm32(HOST_TMP0, HOST_TMP3, 3) ||
			!m_code.EmitMovImm8(HOST_TMP1, 4) ||
			!m_code.EmitMovImm32(HOST_CALL_SCRATCH,
				static_cast<u32>(reinterpret_cast<uptr>(&psxCpu))) ||
			!m_code.EmitLdrImm12(HOST_CALL_SCRATCH, HOST_CALL_SCRATCH, 0) ||
			!m_code.EmitLdrImm12(HOST_CALL_SCRATCH, HOST_CALL_SCRATCH,
				static_cast<u16>(offsetof(R3000Acpu, Clear))) ||
			!m_code.EmitBlx(HOST_CALL_SCRATCH) ||
			!m_code.PatchBranch(no_source_page, m_code.Size(),
				VitaA32::Condition::EQ) ||
			!m_code.PatchBranch(no_source_chunk, m_code.Size(),
				VitaA32::Condition::EQ))
		{
			return false;
		}
		m_source_page_guard_instructions_removed += 2;
		m_isolate_cache_guard_instructions_removed += 12;

		const size_t fast_done = m_code.EmitBranchPlaceholder();
		if (fast_done == static_cast<size_t>(-1))
			return false;
		const size_t fallback_target = m_code.Size();
		for (const size_t branch : fallback_branches)
		{
			if (!m_code.PatchBranch(branch, fallback_target, VitaA32::Condition::NE))
				return false;
		}

		for (u32 i = 0; i < 8; i++)
		{
			const bool load = i < 4;
			const u32 op = load ? copy.load_ops[i] : copy.store_ops[i - 4];
			const u32 pc = start_pc + (instruction_index + i) * sizeof(u32);
			m_current_instruction_count = instruction_index + i + 1;
			m_current_cycle_count = IopCompilerBlockCycles(
				start_pc, m_current_instruction_count, m_emit_trace_checks);
			if (!EmitEffectiveAddress(op, HOST_TMP0))
				return false;
			if (load)
			{
				if (!m_code.EmitCallAbsolute(
						reinterpret_cast<const void*>(&iopMemRead32),
						HOST_CALL_SCRATCH) ||
					!EmitStoreGpr(RT(op), HOST_TMP0))
				{
					return false;
				}
			}
			else if (!EmitLoadGpr(RT(op), HOST_TMP1) ||
				!m_code.EmitCallAbsolute(
						 reinterpret_cast<const void*>(&iopMemWrite32),
						 HOST_CALL_SCRATCH))
			{
				return false;
			}
			UpdateGprConstStateAfterOpcode(op, pc);
		}
		m_native_instruction_count += 8;
		m_current_instruction_count = instruction_index + 8;
		m_current_cycle_count = IopCompilerBlockCycles(
			start_pc, m_current_instruction_count, m_emit_trace_checks);
		return m_code.PatchBranch(fast_done, m_code.Size());
	}

	bool BlockCompiler::EmitInstruction(u32 op, u32 pc, bool store_pc,
		std::vector<size_t>& trace_exit_branches)
	{
		const u32 next_pc = pc + 4;
		if (((m_emit_trace_checks || IopInstructionRequiresCodeState(op)) &&
				!EmitStoreCode(op)) ||
			(m_emit_trace_checks && !EmitTraceCheck(pc, op, trace_exit_branches)) ||
			(store_pc && !EmitStorePc(next_pc)) ||
			(!m_defer_cycle_updates &&
				!EmitAddCycles(IopCompilerInstructionCycles(op, m_emit_trace_checks))))
		{
			return false;
		}

		// PCSX2 owner: x86/iR3000Atables.cpp::rpsxNULL() logs the decoded
		// unknown instruction while compiling and emits no runtime semantics.
		// All non-null table entries continue through their native A32 lowering.
		if (IsNativeOpcode(op))
		{
			if (!EmitNativeInstruction(op, pc))
			return false;
		}
		else
		{
			Console.WriteLn("psxUNK: %8.8x", op);
		}

		UpdateGprConstStateAfterOpcode(op, pc);
		m_native_instruction_count++;
		return true;
	}

	bool BlockCompiler::CompileStraightLineBlock(
		u32 start_pc, u32 instruction_count, const void* direct_exit,
		DirectLinkSlots* direct_links, size_t* linked_entry_offset,
		size_t* provider_entry_offset, bool test_fallthrough_budget,
		bool allow_entry_gate, bool inherited_isolate_write,
		u32 logical_cycle_prefix, u32 logical_cycle_total,
		bool fragmented_logical_block, bool logical_continuation_tail,
		ResidentFragmentContract* resident_contract)
	{
		if (instruction_count == 0 ||
			instruction_count > BlockExecutor::MAX_STRAIGHT_LINE_BLOCK_INSTRUCTIONS ||
			instruction_count > ((UINT32_MAX - start_pc) / 4))
		{
			return false;
		}

		if (direct_links)
			*direct_links = {};

		const bool interpreter_trace = VitaIsIopPreInstructionTraceEnabled();
		m_logical_cycle_prefix = logical_cycle_prefix;
		m_fragmented_logical_block = fragmented_logical_block;
		ResolveIrxImport(0, 0);
		if (!interpreter_trace && instruction_count >= 2)
		{
			const u32 jump_pc = start_pc + (instruction_count - 2) * 4;
			const u32 jump_op = iopMemRead32(jump_pc);
			const u32 marker_op = iopMemRead32(jump_pc + 4);
			if ((jump_op >> 26) == 0x02 && (marker_op >> 16) == 0x2400)
				ResolveIrxImport(jump_pc + 4, marker_op);
		}
		if (!interpreter_trace)
		{
			for (u32 i = 0; i < instruction_count; i++)
			{
				const u32 op = iopMemRead32(start_pc + i * 4);
				if (!IsIopBranchOrJumpOpcode(op))
					continue;

				// PCSX2's x86 recompiler owns one budget/event seam after every
				// branch and its delay slot. A branch embedded in a larger explicit
				// window can only use psxDoBranch(), whose event-before-budget order
				// is interpreter-specific, so reject that non-product block shape.
				if (i + 2 != instruction_count)
					return false;

				const u32 delay_op = iopMemRead32(start_pc + (i + 1) * 4);
				if (IsIopBranchOrJumpOpcode(delay_op) || IsIopExceptionOpcode(delay_op))
					return false;
				break;
			}
		}

		const u32 hardware_pc = start_pc & 0x1fffffffu;
		m_compiled_ps1_bios_gate =
			allow_entry_gate && BlockExecutor::CompiledPs1BiosGateEnabled() &&
			(psxHu32(HW_ICFG) & 8u) != 0 &&
			(hardware_pc == 0xa0u || hardware_pc == 0xb0u || hardware_pc == 0xc0u);

		m_isolate_cache_specialization = true;
#if defined(VITASX2_QEMU_VALIDATION)
		m_isolate_cache_specialization = s_qemuIopIsolateCacheSpecializationEnabled;
#endif
		m_isolate_cache_active = (psxRegs.CP0.n.Status & 0x10000u) != 0;
		m_isolate_cache_guard_stable =
			m_isolate_cache_specialization && !inherited_isolate_write;
		m_writes_isolate_mode = inherited_isolate_write;
		if (m_isolate_cache_specialization)
		{
			for (u32 i = 0; i + 1 < instruction_count; i++)
			{
				if (IsIopCop0StatusWriteOpcode(iopMemRead32(start_pc + i * 4)))
				{
					// Explicit fixed-window calls may include instructions after
					// Status. Retain their runtime guards; the pre-instruction trace
					// scanner ends the block at this ownership seam.
					m_isolate_cache_guard_stable = false;
					break;
				}
			}
		}

		if (!provider_entry_offset && allow_entry_gate && !m_compiled_ps1_bios_gate &&
			EmuConfig.Speedhacks.WaitLoop && !VitaIsIopPreInstructionTraceEnabled() &&
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
			if ((branch_op >> 26) == 0x02 &&
				JumpTarget(branch_pc, branch_op) == start_pc && only_nops)
				return EmitWaitLoopFastForwardBlock(start_pc, instruction_count);
		}

		m_iop_ram_registers_available = false;
		m_iop_ram_mask_register_available = false;
		m_static_branch_outcome_known = false;
		m_static_branch_taken = false;
		m_static_branch_flags_live = false;
		m_static_branch_taken_condition = VitaA32::Condition::AL;
		m_condition_code_branch_instructions_removed = 0;
		m_producer_branch_compare_instructions_removed = 0;
		m_fused_ram_guard_instructions_removed = 0;
		m_source_page_guard_instructions_removed = 0;
		m_source_page_literal_instructions_removed = 0;
		m_isolate_cache_guard_instructions_removed = 0;
		m_source_page_literal_loads.clear();
		m_source_chunk_literal_loads.clear();
		m_source_page_literal_out_of_range = false;
		m_branch_predicate_producer_flags_live = false;
		m_branch_predicate_producer_guest = 0;
		m_branch_predicate_producer_true_condition = VitaA32::Condition::AL;
		m_register_jump_target_known = false;
		m_register_jump_target = 0;
		m_retained_lo_host_valid = false;
		bool sequential_qword_copy_enabled =
			!VitaIsIopPreInstructionTraceEnabled() &&
			m_isolate_cache_specialization && m_isolate_cache_guard_stable &&
			!m_isolate_cache_active && m_ram_source_page_live_flags &&
			m_ram_source_chunk_live_flags &&
			Ps2MemSize::ExposedIopRam == Ps2MemSize::IopRam;
#if defined(VITASX2_QEMU_VALIDATION)
		sequential_qword_copy_enabled = sequential_qword_copy_enabled &&
			s_qemuIopSequentialQwordCopyEnabled &&
			s_qemuIopRamProvenanceSpecializationEnabled;
#endif
		ResetGprConstState();
		for (u32 i = 0; i < instruction_count; i++)
		{
			const u32 pc = start_pc + i * 4;
			const u32 op = iopMemRead32(pc);
			SequentialQwordCopy sequential_copy;
			if (sequential_qword_copy_enabled &&
				MatchSequentialQwordCopyShape(start_pc, i, instruction_count,
					&sequential_copy))
			{
				m_iop_ram_registers_available = true;
				m_iop_ram_mask_register_available = true;
				for (u32 j = 0; j < 8; j++)
				{
					const u32 sequential_op = j < 4 ? sequential_copy.load_ops[j] : sequential_copy.store_ops[j - 4];
					UpdateGprConstStateAfterOpcode(sequential_op,
						start_pc + (i + j) * sizeof(u32));
				}
				i += 7;
				continue;
			}
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
				}
			}
			UpdateGprConstStateAfterOpcode(op, pc);
		}
		ResetGprConstState();
		m_emit_trace_checks = interpreter_trace;
		const bool legacy_cycle_deferral =
			IopBlockCanDeferCycleUpdates(start_pc, instruction_count);
		bool block_cycle_batching = true;
#if defined(VITASX2_QEMU_VALIDATION)
		block_cycle_batching = s_qemuIopBlockCycleBatchingEnabled;
#endif
		// PCSX2's x86 JIT keeps s_psxBlockCycles private across all generated
		// memory calls, including one-op and handler-heavy blocks. Helper count is
		// therefore not a batching profitability term: deferral is both cheaper
		// and the owner-exact device/timer observation contract.
		const bool can_batch_cycle_updates =
			IopBlockCanBatchCycleUpdates(start_pc, instruction_count);
		if (m_fragmented_logical_block &&
			(interpreter_trace || !block_cycle_batching ||
				!can_batch_cycle_updates))
		{
			// An artificial A32 boundary may never expose or rebase PCSX2's
			// private s_psxBlockCycles state. Keep exception/RFE/non-batchable
			// shapes on the one logical interpreter path until their helper
			// contracts can carry the pending prefix.
			return false;
		}
		m_defer_cycle_updates = !m_emit_trace_checks &&
		                        (legacy_cycle_deferral ||
									(block_cycle_batching && can_batch_cycle_updates));
		// SYSCALL/BREAK/RFE helpers must see the BaseBlock-entry cycle. Product
		// blocks defer it; trace and QEMU attribution controls which disable
		// deferral retain the exact recompiler fallback instead of publishing an
		// instruction prefix before the helper.
		if (IopLogicalBlockHasPrivateCycleHelper(start_pc, instruction_count) &&
			!m_defer_cycle_updates)
		{
			return false;
		}
		m_expanded_cycle_batching = m_defer_cycle_updates && !legacy_cycle_deferral;
		// A physical A32 fragment is not a PCSX2 BaseBlock boundary. No generated
		// product helper sees or publishes a partial prefix; the final fragment or
		// an architectural early exit commits it exactly once.
		const bool active_irx_import = m_irx_import_hle || m_irx_import_debug ||
			(m_irx_import_log && m_irx_import_funcname);
		m_resident_event_deadline = m_defer_cycle_updates &&
			legacy_cycle_deferral && !active_irx_import &&
			BranchTestSchedulingEnabled();
#if defined(VITASX2_IOP_PUBLISHED_EVENT_DEADLINE_CONTROL)
		m_resident_event_deadline = false;
#endif
#if defined(VITASX2_QEMU_VALIDATION)
		m_resident_event_deadline = m_resident_event_deadline &&
			s_qemuIopPublishedEventDeadlineResidencyEnabled;
#endif
		bool specialize_clock_mode = true;
#if defined(VITASX2_QEMU_VALIDATION)
		specialize_clock_mode = s_qemuIopClockModeSpecializationEnabled;
#endif
		m_resident_ee_budget = m_defer_cycle_updates &&
			legacy_cycle_deferral && !active_irx_import &&
			BranchTestSchedulingEnabled() && specialize_clock_mode &&
			(psxHu32(HW_ICFG) & (1u << 3)) == 0;
#if defined(VITASX2_IOP_RESIDENT_EE_BUDGET_CONTROL)
		m_resident_ee_budget = false;
#endif
#if defined(VITASX2_QEMU_VALIDATION)
		m_resident_ee_budget =
			m_resident_ee_budget && s_qemuIopEeBudgetResidencyEnabled &&
			s_qemuIopResidentPreludeLinksEnabled;
#endif
		if (active_irx_import && !m_defer_cycle_updates)
		{
			// The x86 owner can leave the complete block-cycle delta private when
			// a handled HLE dispatches early. Keep the interpreter-trace/nondeferred
			// validation shape out of this early-exit path because it has already
			// advanced architectural time instruction by instruction.
			return false;
		}
		m_has_budget_exit = false;
		m_block_cycle_count =
			IopCompilerBlockCycles(start_pc, instruction_count, m_emit_trace_checks);
		m_budget_cycle_count =
			logical_cycle_total != 0 ? logical_cycle_total : m_block_cycle_count;
		m_batched_cycle_instructions_removed =
			m_expanded_cycle_batching ? UINT32_MAX : 0;
		m_batched_cycle_stack_words_removed =
			m_expanded_cycle_batching ? 2 : 0;
		m_current_instruction_count = 0;
		m_current_cycle_count = 0;
		AnalyzePinnedGprs(start_pc, instruction_count);
		AnalyzeSavedRegisters(start_pc, instruction_count);

		if (!BeginBlock(start_pc, linked_entry_offset, provider_entry_offset))
			return false;
		if (m_compiled_ps1_bios_gate && !EmitCompiledPs1BiosGate())
			return false;

		std::vector<size_t> direct_exit_branches;
		std::vector<size_t> trace_exit_branches;
		std::vector<size_t> budget_exit_branches;
		std::vector<size_t> unflushed_budget_exit_branches;
		std::array<std::vector<size_t>, 2> scheduler_budget_exit_branches;
		std::array<std::vector<size_t>, 2> unflushed_scheduler_budget_exit_branches;
		m_direct_exit_branches = &direct_exit_branches;
		m_budget_exit_branches = &budget_exit_branches;
		m_unflushed_budget_exit_branches = &unflushed_budget_exit_branches;
		for (u8 slot = 0; slot < 2; slot++)
		{
			m_scheduler_budget_exit_branches[slot] =
				&scheduler_budget_exit_branches[slot];
			m_unflushed_scheduler_budget_exit_branches[slot] =
				&unflushed_scheduler_budget_exit_branches[slot];
		}
		direct_exit_branches.reserve(instruction_count * 2);
		trace_exit_branches.reserve(instruction_count);
		budget_exit_branches.reserve(4);
		unflushed_budget_exit_branches.reserve(2);
		m_native_instruction_count = 0;
		m_helper_instruction_count = 0;
		bool can_direct_link_fallthrough = true;
		bool has_native_static_branch = false;
		bool has_native_static_jump = false;
		bool has_native_register_jump = false;
		u32 static_branch_target_pc = 0;
		u32 static_branch_fallthrough_pc = 0;
		u32 static_jump_target_pc = 0;
		const bool physical_continuation =
			!test_fallthrough_budget && !logical_continuation_tail;
		for (u32 i = 0; i < instruction_count; i++)
		{
			const u32 pc = start_pc + i * 4;
			const u32 op = iopMemRead32(pc);
			// A nondeferred instruction first calls EmitAddCycles(), whose
			// large-offset form may use r3 even when the opcode's own native
			// template does not. Trace callbacks are calls and therefore also
			// terminate the caller-clobbered lifetime.
			if (m_retained_lo_host_valid &&
				(!m_defer_cycle_updates || m_emit_trace_checks ||
					!IopInstructionPreservesRetainedLoHost(op)))
			{
				m_retained_lo_host_valid = false;
			}
			const u32 delay_op = (i + 1 < instruction_count) ? iopMemRead32(pc + 4) : 0;
			const u32 following_op =
				(i + 2 < instruction_count) ? iopMemRead32(pc + 8) : 0;
			m_current_instruction_count = i + 1;
			m_current_cycle_count +=
				IopCompilerInstructionCycles(op, m_emit_trace_checks);
			const bool can_native_static_branch =
				IsIopStaticConditionalBranchOpcode(op) && i + 2 == instruction_count &&
				!IsIopBranchOrJumpOpcode(delay_op) && !IsIopExceptionOpcode(delay_op);
			bool condition_code_branch_enabled = true;
			bool producer_branch_flags_enabled = true;
#if defined(VITASX2_QEMU_VALIDATION)
			condition_code_branch_enabled = s_qemuIopConditionCodeBranchEnabled;
			producer_branch_flags_enabled = s_qemuIopProducerBranchFlagsEnabled;
#endif
			const bool can_keep_static_branch_flags =
				can_native_static_branch && m_defer_cycle_updates &&
				condition_code_branch_enabled &&
				IopDelayInstructionPreservesHostFlags(delay_op);
			const bool can_native_static_jump =
				IsIopStaticJumpOpcode(op) && i + 2 == instruction_count &&
				!IsIopBranchOrJumpOpcode(delay_op) && !IsIopExceptionOpcode(delay_op) &&
				(!m_emit_trace_checks || (op >> 26) != 0x02 ||
					(delay_op >> 16) != 0x2400);
			const bool can_native_register_jump =
				IsIopRegisterJumpOpcode(op) && i + 2 == instruction_count &&
				!IsIopBranchOrJumpOpcode(delay_op) && !IsIopExceptionOpcode(delay_op);
			m_emit_native_static_branch = can_native_static_branch;
			m_emit_native_static_branch_flags = can_keep_static_branch_flags;
			m_emit_branch_predicate_producer =
				producer_branch_flags_enabled && m_defer_cycle_updates &&
				i + 3 == instruction_count &&
				IopSetLessThanResultFeedsZeroBranch(op, delay_op) &&
				!IsIopBranchOrJumpOpcode(following_op) &&
				!IsIopExceptionOpcode(following_op);
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
			SequentialQwordCopy sequential_copy;
			if (MatchSequentialQwordCopy(start_pc, i, instruction_count,
					&sequential_copy))
			{
				if (!EmitSequentialQwordCopy(sequential_copy, start_pc, i))
				{
					return false;
				}
				i += 7;
				continue;
			}
			if (IsIopBranchOrJumpOpcode(op))
				can_direct_link_fallthrough = false;
			// PCSX2 keeps psxpc private for the complete BaseBlock. Product helpers,
			// including branch-delay memory and RFE, observe the entry PC; their
			// owning tail publishes the final PC after the last opcode. The separate
			// preinstruction trace deliberately retains interpreter-style PC writes.
			const bool store_pc = m_emit_trace_checks;
			const bool emitted = CanCompileOpcode(op) &&
			                     EmitInstruction(op, pc, store_pc, trace_exit_branches);
			m_emit_native_static_branch = false;
			m_emit_native_static_branch_flags = false;
			m_emit_branch_predicate_producer = false;
			m_emit_native_static_jump = false;
			m_emit_native_register_jump = false;
			if (!emitted)
				return false;
		}

		const u32 next_pc = start_pc + instruction_count * 4;
		if (!physical_continuation && !has_native_static_branch &&
			!has_native_static_jump && !has_native_register_jump &&
			!EmitStorePc(next_pc))
		{
			return false;
		}
		if (!physical_continuation && m_defer_cycle_updates &&
			!m_static_branch_flags_live &&
			!EmitPublishCyclePrefix(m_block_cycle_count))
			return false;
		// The ordinary branch/jump paths below consume this logical block's
		// cycles as part of their event test. A pure fallthrough has no PCSX2
		// event seam, but a resident direct link still needs its countdown
		// rebased to the cycle value just published above.
		if (!physical_continuation && m_resident_event_deadline &&
			!has_native_static_branch && !has_native_static_jump &&
			!has_native_register_jump &&
			!EmitConsumePublishedEventCountdown())
		{
			return false;
		}
		if (m_expanded_cycle_batching)
			RecordBatchedCycleExitSavings(instruction_count, false);
		FinalizeResidentExitContract();

		const auto end_completion_return = [&](bool charge_budget = true,
			bool flush_pins = true) -> bool {
			if (logical_continuation_tail)
				return EndBlockLogicalContinuationReturn(charge_budget, flush_pins);
			return m_writes_isolate_mode && !physical_continuation ? EndBlockIsolateModeWriteReturn(charge_budget, flush_pins) : EndBlockReturn(BlockExitKind::Direct, charge_budget, flush_pins);
		};
		const bool branch_test_scheduling = BranchTestSchedulingEnabled();
		size_t direct_exit_offset = 0;
		const bool emit_link_tail = direct_exit && direct_links &&
		                            can_direct_link_fallthrough &&
		                            (!m_writes_isolate_mode || physical_continuation);
		const bool emit_branch_link_tails = direct_exit && direct_links &&
		                                    has_native_static_branch &&
			!m_writes_isolate_mode;
		const auto emit_direct_or_return_tail =
			[&](u32 target_pc, u8 slot_index, bool charge_budget = true) -> bool {
			if (emit_branch_link_tails)
			{
				DirectLinkSlot& link = direct_links->slots[slot_index];
				if (!EndBlockDirectTail(direct_exit, &link, slot_index, charge_budget))
					return false;

				link.target_pc = target_pc;
				link.valid = true;
				return true;
			}

			return end_completion_return(charge_budget);
		};

		if (has_native_static_branch)
		{
			const bool interpreter_trace_branch_semantics = m_emit_trace_checks;
			if (m_static_branch_outcome_known)
			{
				const u32 target_pc = m_static_branch_taken ? static_branch_target_pc : static_branch_fallthrough_pc;
				const u8 link_slot = m_static_branch_taken ? 1 : 0;
#if defined(VITASX2_QEMU_VALIDATION)
				++g_qemuIopConstStaticBranchTailFastPaths;
#endif
				if (!EmitStorePc(target_pc))
					return false;

				const bool branch_test_this_arm =
					m_static_branch_taken || !interpreter_trace_branch_semantics;
				if (branch_test_this_arm &&
					(!EmitBranchEventTest(emit_branch_link_tails ? link_slot : UINT8_MAX) ||
					 !EmitPcChangedExitCheck(target_pc, direct_exit_branches)))
				{
					return false;
				}

				if (!emit_direct_or_return_tail(target_pc, link_slot,
						!branch_test_this_arm ||
							!branch_test_scheduling))
					return false;

				direct_exit_offset = m_code.Size();
				if (!end_completion_return(false))
					return false;
			}
			else
			{
				// PCSX2 owners: x86/iR3000Atables.cpp's conditional branch arms
				// both call psxSetBranchImm(), which enters iPsxBranchTest(). Keep
				// the same budget-before-event seam on both outcomes. Publishing
				// after the conditional A32 branch keeps CPSR live through a delay
				// slot whose lowering deliberately preserves flags.
				const VitaA32::Condition taken_condition =
					m_static_branch_flags_live ? m_static_branch_taken_condition : VitaA32::Condition::NE;
				if (!m_static_branch_flags_live &&
					!m_code.EmitCmpImm32(HOST_BRANCH_FLAG, 0))
				{
					return false;
				}

				const size_t taken_path = m_code.EmitBranchPlaceholder(taken_condition);
				if (taken_path == static_cast<size_t>(-1))
					return false;

				if ((m_static_branch_flags_live &&
						!EmitPublishCyclePrefix(m_block_cycle_count)) ||
					!EmitStorePc(static_branch_fallthrough_pc) ||
					(!interpreter_trace_branch_semantics &&
						(!EmitBranchEventTest(emit_branch_link_tails ? 0 : UINT8_MAX) ||
						 !EmitPcChangedExitCheck(static_branch_fallthrough_pc,
							direct_exit_branches))) ||
					!emit_direct_or_return_tail(static_branch_fallthrough_pc, 0,
						interpreter_trace_branch_semantics ||
							!branch_test_scheduling))
				{
					return false;
				}

				const size_t taken_path_target = m_code.Size();
				if (!m_code.PatchBranch(taken_path, taken_path_target, taken_condition) ||
					(m_static_branch_flags_live &&
						!EmitPublishCyclePrefix(m_block_cycle_count)) ||
					!EmitStorePc(static_branch_target_pc) ||
					!EmitBranchEventTest(emit_branch_link_tails ? 1 : UINT8_MAX) ||
					!EmitPcChangedExitCheck(static_branch_target_pc,
						direct_exit_branches) ||
					!emit_direct_or_return_tail(static_branch_target_pc, 1,
						!branch_test_scheduling))
				{
					return false;
				}

				direct_exit_offset = m_code.Size();
				if (!end_completion_return(false))
					return false;
			}
		}
		else if (has_native_static_jump)
		{
			const bool can_link_static_jump =
				direct_exit && direct_links && !m_writes_isolate_mode;
			if (!EmitStorePc(static_jump_target_pc) ||
				!EmitBranchEventTest(can_link_static_jump ? 0 : UINT8_MAX) ||
				!EmitPcChangedExitCheck(static_jump_target_pc, direct_exit_branches))
			{
				return false;
			}

			if (can_link_static_jump)
			{
				DirectLinkSlot& link = direct_links->slots[0];
				if (!EndBlockDirectTail(direct_exit, &link, 0, !branch_test_scheduling))
					return false;

				link.target_pc = static_jump_target_pc;
				link.valid = true;
			}
			else if (!end_completion_return(!branch_test_scheduling))
			{
				return false;
			}

			direct_exit_offset = m_code.Size();
			if (!end_completion_return(false))
				return false;
		}
		else if (has_native_register_jump)
		{
			if (m_register_jump_target_known)
			{
				const bool can_link_register_jump =
					direct_exit && direct_links && !m_writes_isolate_mode;
				if (!EmitStorePc(m_register_jump_target) ||
					!EmitBranchEventTest(can_link_register_jump ? 0 : UINT8_MAX) ||
					!EmitPcChangedExitCheck(m_register_jump_target,
						direct_exit_branches))
				{
					return false;
				}

				if (can_link_register_jump)
				{
					DirectLinkSlot& link = direct_links->slots[0];
					if (!EndBlockDirectTail(direct_exit, &link, 0, !branch_test_scheduling))
						return false;

					link.target_pc = m_register_jump_target;
					link.valid = true;
				}
				else if (!end_completion_return(!branch_test_scheduling))
				{
					return false;
				}
			}
			else
			{
				if (!EmitStorePcReg(HOST_REGISTER_JUMP_TARGET) ||
					!EmitBranchEventTest() ||
					!EmitPcChangedExitCheckReg(HOST_REGISTER_JUMP_TARGET,
						direct_exit_branches) ||
					!end_completion_return(!branch_test_scheduling))
				{
					return false;
				}
			}

			direct_exit_offset = m_code.Size();
			if (!end_completion_return(false))
				return false;
		}
		else if (emit_link_tail)
		{
			DirectLinkSlot& link = direct_links->slots[0];
			if (!EndBlockDirectTail(direct_exit, &link, 0, !physical_continuation,
					test_fallthrough_budget,
					logical_continuation_tail ? BlockExitKind::LogicalContinuation : BlockExitKind::Direct))
				return false;

			link.target_pc = next_pc;
			link.valid = true;

			direct_exit_offset = m_code.Size();
			if (!end_completion_return(false))
				return false;
		}
		else
		{
			if (m_writes_isolate_mode && !end_completion_return())
				return false;
			direct_exit_offset = m_code.Size();
			if (!end_completion_return(false))
				return false;
		}

		u32 written_pin_count = 0;
		for (u8 i = 0; i < m_pinned_gpr_count; i++)
			written_pin_count += m_pinned_gprs[i].ever_written ? 1u : 0u;
		const u32 removed_gpr_memory_ops =
			m_pinned_gpr_load_hits + m_pinned_gpr_store_hits;
		const u32 added_gpr_memory_ops =
			m_pinned_gpr_initial_loads + written_pin_count;
		m_pinned_gpr_memory_ops_saved =
			removed_gpr_memory_ops > added_gpr_memory_ops ? removed_gpr_memory_ops - added_gpr_memory_ops : 0;
		if (m_pinned_gpr_min_exit_savings != UINT32_MAX)
		{
			m_pinned_gpr_memory_ops_saved =
				std::min(m_pinned_gpr_memory_ops_saved, m_pinned_gpr_min_exit_savings);
		}

		// Place the shared word at the first block-wide unreachable seam. Cold
		// fallback branches are patched afterwards and jump over it; their tails
		// branch back to their main-path joins. Keeping the pool before those
		// tails gives large blocks the same one-instruction hot guard.
		if (!EmitSourcePageLiteralPool() || !FlushColdTails())
			return false;

		// Budget-before-event exits and event-driven PC changes occur before the
		// ordinary direct-link tail publishes dirty pins. Give those paths one
		// shared state-owning return. The former code sent PC-change exits to a
		// post-flush return even though the skipped main tail had not flushed.
		const bool needs_unflushed_state_exit =
			!direct_exit_branches.empty() || !unflushed_budget_exit_branches.empty();
		const size_t unflushed_state_exit_offset =
			needs_unflushed_state_exit ? m_code.Size() : direct_exit_offset;
		if (needs_unflushed_state_exit &&
			(!EmitFlushPinnedGprs() || !end_completion_return(false, false)))
		{
			return false;
		}
		for (const size_t branch_offset : direct_exit_branches)
		{
			if (!m_code.PatchBranch(branch_offset, unflushed_state_exit_offset,
					VitaA32::Condition::NE))
			{
				return false;
			}
		}
		for (const size_t branch_offset : unflushed_budget_exit_branches)
		{
			if (!m_code.PatchBranch(branch_offset, unflushed_state_exit_offset,
					VitaA32::Condition::LE))
			{
				return false;
			}
		}

		// Trace callbacks and early branch-helper budget checks publish the state
		// belonging to their own control-flow path before branching. They must
		// bypass the ordinary final-state flush: a later fallthrough write-first
		// pin has no valid host value on an earlier exit. Ordinary blocks keep the
		// compact existing tail because their budget exit owns final-path state.
		const bool has_early_branch_helper =
			m_pinned_gpr_min_exit_savings != UINT32_MAX;
		const bool needs_published_state_exit =
			has_early_branch_helper || !trace_exit_branches.empty();
		const size_t published_state_exit_offset =
			needs_published_state_exit ? m_code.Size() : direct_exit_offset;
		if (needs_published_state_exit &&
			!EndBlockReturn(BlockExitKind::Direct, false, false))
			return false;
		for (const size_t branch_offset : trace_exit_branches)
		{
			if (!m_code.PatchBranch(branch_offset, published_state_exit_offset,
					VitaA32::Condition::NE))
				return false;
		}
		for (const size_t branch_offset : budget_exit_branches)
		{
			if (!m_code.PatchBranch(branch_offset, published_state_exit_offset,
					VitaA32::Condition::LE))
				return false;
		}

		// A linked chain can enter several blocks before the EE budget expires, so
		// its provider caller does not identify the final source block. Give each
		// static edge a cold budget-return tail whose patchable r0 value names the
		// already-resolved target. PatchDirectLink() updates this value atomically
		// with the owning PCSX2-style direct branch. Bit zero tags the aligned
		// CachedBlock pointer for the private scheduler dispatcher.
		for (u8 slot = 0; slot < 2; slot++)
		{
			auto& flushed_exits = scheduler_budget_exit_branches[slot];
			auto& unflushed_exits = unflushed_scheduler_budget_exit_branches[slot];
			if (flushed_exits.empty() && unflushed_exits.empty())
				continue;
			if (!direct_links)
				return false;

			const size_t unflushed_target = m_code.Size();
			if (!unflushed_exits.empty() && !EmitFlushPinnedGprs())
				return false;
			const size_t resume_return_target = m_code.Size();
			if (!EmitPublishResidentEeBudget())
				return false;
			DirectLinkSlot& link = direct_links->slots[slot];
			link.scheduler_resume_offset = m_code.Size();
			if (!m_code.EmitMovImm32Patchable(
					HOST_TMP0, static_cast<u32>(BlockExitKind::Direct)) ||
				!m_code.EmitBx(HOST_CHAIN_RETURN))
			{
				return false;
			}

			for (const size_t branch_offset : unflushed_exits)
			{
				if (!m_code.PatchBranch(branch_offset, unflushed_target,
						VitaA32::Condition::LE))
				{
					return false;
				}
			}
			for (const size_t branch_offset : flushed_exits)
			{
				if (!m_code.PatchBranch(branch_offset, resume_return_target,
						VitaA32::Condition::LE))
				{
					return false;
				}
			}
		}
		m_direct_exit_branches = nullptr;
		m_budget_exit_branches = nullptr;
		m_unflushed_budget_exit_branches = nullptr;
		m_scheduler_budget_exit_branches = {};
		m_unflushed_scheduler_budget_exit_branches = {};
		if (resident_contract)
			*resident_contract = m_resident_contract;
		return true;
	}

	BlockExecutor::BlockExecutor(bool owns_ee_event_entry)
		: m_ram_source_chunks(std::make_unique<RamSourceChunkOwnership>())
		, m_scheduler_direct_resume_event_context{this, nullptr}
		, m_wait_resume_event_context{this, nullptr, WaitResumeKind::Invalid}
		, m_owns_ee_event_entry(owns_ee_event_entry)
	{
		bool isolate_variants_enabled = true;
#if defined(VITASX2_QEMU_VALIDATION)
		isolate_variants_enabled = s_qemuIopIsolateCacheSpecializationEnabled;
#endif
		m_active_isolate_cache_mode =
			isolate_variants_enabled && (psxRegs.CP0.n.Status & 0x10000u) != 0;
		m_cache.reserve(INITIAL_CACHE_CAPACITY);
		m_block_records.reserve(INITIAL_CACHE_CAPACITY);
		m_incoming_links.reserve(INITIAL_CACHE_CAPACITY * DIRECT_LINK_SLOT_COUNT);
#if defined(__arm__)
		if (m_owns_ee_event_entry)
		{
			VitaSetA32IopSchedulerDirectEventContext(
				reinterpret_cast<uptr>(&m_scheduler_direct_resume_event_context));
		}
#endif
	}

	void BlockExecutor::SetWaitResumeBlock(CachedBlock* block)
	{
		if (!block)
			return;

		ClearSchedulerDirectResume();
		if (m_wait_resume_block == block)
			return;
		m_wait_resume_block = block;
		m_wait_resume_event_context.block = block;
		m_wait_resume_event_context.kind =
			block->HasRamPollWaitLoop() ? WaitResumeKind::PollCall : (block->wait_loop_descriptor.condition == WaitLoopCondition::Always ? WaitResumeKind::Unconditional : WaitResumeKind::Conditional);
#if defined(__arm__)
		if (m_owns_ee_event_entry)
		{
#if defined(VITASX2_QEMU_VALIDATION)
			m_wait_resume_event_installs++;
#endif
			uptr target = 0;
#if defined(VITASX2_IOP_WAIT_RESUME_KIND_ENTRY_CONTROL)
			target =
				reinterpret_cast<uptr>(&VitaIopA32ExecuteProviderWaitResumePrivate);
#elif defined(VITASX2_QEMU_VALIDATION)
			if (!s_qemuIopWaitResumeKindEntryEnabled)
				target =
					reinterpret_cast<uptr>(&VitaIopA32ExecuteProviderWaitResumePrivate);
#endif
			if (target == 0)
			{
#if defined(VITASX2_QEMU_VALIDATION) || \
	defined(VITASX2_IOP_WAIT_RESUME_CLOCK_ENTRY_CONTROL)
#if defined(VITASX2_IOP_WAIT_RESUME_CLOCK_ENTRY_CONTROL)
				constexpr bool use_dynamic_clock = true;
#else
				const bool use_dynamic_clock = !s_qemuIopWaitResumeClockEntryEnabled;
#endif
				if (use_dynamic_clock)
				{
					switch (m_wait_resume_event_context.kind)
					{
						case WaitResumeKind::Unconditional:
							target = reinterpret_cast<uptr>(
								&VitaIopA32ExecuteProviderWaitResumeUnconditionalPrivate);
							break;
						case WaitResumeKind::PollCall:
							target = reinterpret_cast<uptr>(
								&VitaIopA32ExecuteProviderWaitResumePollPrivate);
							break;
						case WaitResumeKind::Conditional:
							target = reinterpret_cast<uptr>(
								&VitaIopA32ExecuteProviderWaitResumeConditionalPrivate);
							break;
						case WaitResumeKind::Invalid:
							break;
					}
				}
				else
#endif
				{
					const bool ps1_clock = (psxHu32(HW_ICFG) & (1u << 3)) != 0;
					switch (m_wait_resume_event_context.kind)
					{
						case WaitResumeKind::Unconditional:
						{
#if defined(VITASX2_QEMU_VALIDATION) || \
	defined(VITASX2_IOP_WAIT_RESUME_NO_LINK_ENTRY_CONTROL)
#if defined(VITASX2_IOP_WAIT_RESUME_NO_LINK_ENTRY_CONTROL)
							constexpr bool use_no_link_entry = false;
#else
							const bool use_no_link_entry = s_qemuIopWaitResumeNoLinkEntryEnabled;
#endif
#else
							constexpr bool use_no_link_entry = true;
#endif
							const bool writes_link = block->wait_loop_descriptor.writes_link;
							if (use_no_link_entry && !writes_link)
							{
								target =
									ps1_clock ? reinterpret_cast<uptr>(
										&VitaIopA32ExecuteProviderWaitResumeUnconditionalNoLinkPs1Private) :
									reinterpret_cast<uptr>(
										&VitaIopA32ExecuteProviderWaitResumeUnconditionalNoLinkNormalPrivate);
							}
							else
							{
								target =
									ps1_clock ? reinterpret_cast<uptr>(
										&VitaIopA32ExecuteProviderWaitResumeUnconditionalPs1Private) :
									reinterpret_cast<uptr>(
										&VitaIopA32ExecuteProviderWaitResumeUnconditionalNormalPrivate);
							}
							break;
						}
						case WaitResumeKind::PollCall:
							target =
								ps1_clock ? reinterpret_cast<uptr>(
									&VitaIopA32ExecuteProviderWaitResumePollPs1Private) :
								reinterpret_cast<uptr>(
									&VitaIopA32ExecuteProviderWaitResumePollNormalPrivate);
							break;
						case WaitResumeKind::Conditional:
							target =
								ps1_clock ? reinterpret_cast<uptr>(
									&VitaIopA32ExecuteProviderWaitResumeConditionalPs1Private) :
								reinterpret_cast<uptr>(
									&VitaIopA32ExecuteProviderWaitResumeConditionalNormalPrivate);
							break;
						case WaitResumeKind::Invalid:
							break;
					}
				}
			}
			const bool retained_unconditional_normal_wait =
				m_wait_resume_event_context.kind == WaitResumeKind::Unconditional &&
				!block->wait_loop_descriptor.writes_link &&
				(psxHu32(HW_ICFG) & (1u << 3)) == 0;
			VitaSetA32IopWaitResumeEventEntry(
				reinterpret_cast<uptr>(&m_wait_resume_event_context), target,
				retained_unconditional_normal_wait);
		}
#endif
	}

	void BlockExecutor::ClearWaitResumeBlock()
	{
		if (!m_wait_resume_block)
			return;

		m_wait_resume_block = nullptr;
		m_wait_resume_event_context.block = nullptr;
		m_wait_resume_event_context.kind = WaitResumeKind::Invalid;
#if defined(__arm__)
		if (m_owns_ee_event_entry)
		{
#if defined(VITASX2_QEMU_VALIDATION)
			m_wait_resume_event_clears++;
#endif
			VitaSetA32IopWaitResumeEventEntry(0, 0, false);
		}
#endif
	}

	inline __attribute__((always_inline)) void
	BlockExecutor::SetSchedulerDirectResumeBlock(CachedBlock* block)
	{
		if (!block)
		{
			ClearSchedulerDirectResume();
			return;
		}

#if defined(VITASX2_QEMU_VALIDATION)
		const bool install_event_entry =
			!m_scheduler_direct_resume_event_context.block;
#endif
		m_scheduler_direct_resume_event_context.block = block;
#if defined(VITASX2_QEMU_VALIDATION)
		if (install_event_entry && m_owns_ee_event_entry)
			m_scheduler_direct_event_installs++;
#endif
	}

	void BlockExecutor::ClearSchedulerDirectResume()
	{
#if defined(VITASX2_QEMU_VALIDATION)
		if (!m_scheduler_direct_resume_event_context.block)
			return;
		if (m_owns_ee_event_entry)
			m_scheduler_direct_event_clears++;
#endif
		m_scheduler_direct_resume_event_context.block = nullptr;
	}

	inline __attribute__((always_inline)) void
	BlockExecutor::SetSchedulerPredictedResumeBlock(CachedBlock* block)
	{
		// PCSX2 owner: x86/iR3000A.cpp::_DynGen_DispatcherReg() resolves a
		// register-selected PC through the recompiler LUT and immediately enters
		// its BaseBlock. Keep the last two dynamically dispatched BaseBlocks at
		// the EE scheduler boundary; the A32 entry compares architectural PC
		// before using either and falls through to the complete dispatcher on a
		// miss. Two ways capture the measured alternating pair without paying for
		// the negligible four-way tail.
#if defined(VITASX2_IOP_SCHEDULER_PREDICTION_CONTROL)
		(void)block;
		return;
#elif defined(VITASX2_QEMU_VALIDATION) && \
	!defined(VITASX2_IOP_SCHEDULER_DISPATCH_CACHE_CODEGEN)
		if (!g_vita_a32_iop_scheduler_prediction_event_entry_enabled)
			return;
#endif
		// UINT32_MAX is the empty-key sentinel consumed by the product thunk.
		// Architectural IOP PCs are word aligned, so a matching key proves that
		// the paired block pointer was installed as well.
		if (!block)
			return;
		const u32 guest_pc = psxRegs.pc;
		if (m_scheduler_direct_resume_event_context.predicted_block != block ||
			m_scheduler_direct_resume_event_context.predicted_pc != guest_pc)
		{
			m_scheduler_direct_resume_event_context.predicted_block_second =
				m_scheduler_direct_resume_event_context.predicted_block;
			m_scheduler_direct_resume_event_context.predicted_pc_second =
				m_scheduler_direct_resume_event_context.predicted_pc;
			m_scheduler_direct_resume_event_context.predicted_block = block;
			m_scheduler_direct_resume_event_context.predicted_pc = guest_pc;
		}
		const u32 cache_index = (guest_pc >> 2) &
		                        (SchedulerDispatchCacheEntryCount() - 1);
		auto& cache_entry =
			m_scheduler_direct_resume_event_context.dispatch_cache[cache_index];
		if (cache_entry.block != block || cache_entry.guest_pc != guest_pc)
		{
			cache_entry.block = block;
			cache_entry.guest_pc = guest_pc;
#if defined(VITASX2_QEMU_VALIDATION) && \
	!defined(VITASX2_IOP_SCHEDULER_DISPATCH_CACHE_CODEGEN)
			m_scheduler_dispatch_cache_installs++;
#endif
		}
#if defined(VITASX2_QEMU_VALIDATION) && \
	!defined(VITASX2_IOP_SCHEDULER_DISPATCH_CACHE_CODEGEN)
		for (u32 i = 0; i < m_scheduler_prediction_shadow.size(); i++)
		{
			if (m_scheduler_prediction_shadow[i] != block)
				continue;
			for (; i > 0; i--)
				m_scheduler_prediction_shadow[i] = m_scheduler_prediction_shadow[i - 1];
			m_scheduler_prediction_shadow[0] = block;
			return;
		}
		for (u32 i = m_scheduler_prediction_shadow.size() - 1; i > 0; i--)
			m_scheduler_prediction_shadow[i] = m_scheduler_prediction_shadow[i - 1];
		m_scheduler_prediction_shadow[0] = block;
#endif
	}

	void BlockExecutor::ClearSchedulerPredictedResume()
	{
		m_scheduler_direct_resume_event_context.predicted_block = nullptr;
		m_scheduler_direct_resume_event_context.predicted_block_second = nullptr;
		m_scheduler_direct_resume_event_context.predicted_pc = UINT32_MAX;
		m_scheduler_direct_resume_event_context.predicted_pc_second = UINT32_MAX;
		for (auto& entry : m_scheduler_direct_resume_event_context.dispatch_cache)
		{
			entry.block = nullptr;
			entry.guest_pc = UINT32_MAX;
		}
#if defined(VITASX2_QEMU_VALIDATION)
		m_scheduler_prediction_shadow.fill(nullptr);
#endif
	}

	BlockExecutor::~BlockExecutor()
	{
		Shutdown();
		ReleaseLookupPages();
	}

	void BlockExecutor::NotifyPcDiscontinuity()
	{
		// PCSX2 owner: R3000A.cpp::psxException() is the asynchronous event
		// path which replaces the current IOP PC. Retire the scheduler target at
		// that owner so a retained wait can never mask the exception vector.
		ClearWaitResumeBlock();
		ClearSchedulerDirectResume();
	}

	void BlockExecutor::ResetInstrumentationCounters()
	{
#if defined(VITASX2_PORTABLE_REPLAY_VALIDATION) || \
	defined(VITASX2_PRODUCT_BOOT_VALIDATION)
		m_portable_validation_stats = {};
#endif
#if defined(VITASX2_QEMU_VALIDATION)
		m_hot_dispatch_cache_hits = 0;
		m_hot_dispatch_cache_misses = 0;
		m_hot_dispatch_cache_way_probes = 0;
		m_hot_dispatch_cache_64_set_hits = 0;
		m_hot_dispatch_cache_64_set_misses = 0;
		m_hot_dispatch_cache_64_set_way_probes = 0;
		m_scheduler_direct_resume_candidates = 0;
		m_scheduler_direct_resume_installs = 0;
		m_scheduler_direct_resume_attempts = 0;
		m_scheduler_direct_resume_hits = 0;
		m_scheduler_direct_resume_misses = 0;
		m_scheduler_direct_resume_no_target = 0;
		m_scheduler_direct_resume_target_mismatch = 0;
		m_scheduler_direct_event_entries = 0;
		m_scheduler_direct_event_forwards = 0;
		m_scheduler_direct_event_fallbacks = 0;
		m_scheduler_direct_event_remainders = 0;
		m_scheduler_direct_event_installs = 0;
		m_scheduler_direct_event_clears = 0;
		m_scheduler_prediction_attempts = 0;
		m_scheduler_prediction_hits = 0;
		m_scheduler_prediction_misses = 0;
		m_scheduler_prediction_two_way_hits = 0;
		m_scheduler_prediction_four_way_hits = 0;
		m_scheduler_prediction_forwards = 0;
		m_scheduler_prediction_fallbacks = 0;
		m_scheduler_prediction_remainders = 0;
		m_scheduler_dispatch_cache_attempts = 0;
		m_scheduler_dispatch_cache_hits = 0;
		m_scheduler_dispatch_cache_misses = 0;
		m_scheduler_dispatch_cache_forwards = 0;
		m_scheduler_dispatch_cache_fallbacks = 0;
		m_scheduler_dispatch_cache_remainders = 0;
		m_scheduler_dispatch_cache_installs = 0;
		m_hot_dispatch_trusted_raw_hits = 0;
		m_hot_dispatch_owned_hits = 0;
		m_hot_dispatch_hit_pc_profile.clear();
		m_interpreter_fallback_pc_profile.clear();
		m_wait_resume_cache_attempts = 0;
		m_wait_resume_cache_hits = 0;
		m_wait_resume_cache_misses = 0;
		m_wait_resume_event_entries = 0;
		m_wait_resume_event_forwards = 0;
		m_wait_resume_event_fallbacks = 0;
		m_wait_resume_event_installs = 0;
		m_wait_resume_event_clears = 0;
		m_wait_resume_first_entry_owned = 0;
		m_wait_resume_post_event_identity_checks = 0;
		m_wait_resume_kind_specific_entries = 0;
		m_wait_resume_kind_specific_unconditional_forwards = 0;
		m_wait_resume_kind_specific_poll_forwards = 0;
		m_wait_resume_kind_specific_conditional_forwards = 0;
		m_wait_resume_clock_specific_entries = 0;
		m_wait_resume_clock_specific_forwards = 0;
		m_wait_resume_no_link_specific_entries = 0;
		m_wait_resume_no_link_specific_forwards = 0;
		m_wait_resume_descriptor_forwards = 0;
		m_wait_resume_unconditional_forwards = 0;
		m_wait_resume_poll_forwards = 0;
		m_wait_resume_conditional_forwards = 0;
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
		m_pinned_gpr_memory_ops_saved = 0;
		m_pinned_branch_operand_moves_removed = 0;
		m_condition_code_branch_instructions_removed = 0;
		m_producer_branch_compare_instructions_removed = 0;
		m_fused_ram_guard_instructions_removed = 0;
		m_source_page_guard_instructions_removed = 0;
		m_source_page_literal_instructions_removed = 0;
		m_isolate_cache_guard_instructions_removed = 0;
		m_private_dispatcher_calls = 0;
		m_private_dispatcher_provider_entries = 0;
		m_private_dispatcher_wait_forwards = 0;
		m_private_dispatcher_generated_entries = 0;
		m_private_dispatcher_fallbacks = 0;
		m_private_dispatcher_inlined_hot_entries = 0;
		m_private_frame_provider_entries = 0;
		m_private_frame_stack_words_removed = 0;
		m_private_frame_zero_scratch_entries = 0;
		m_cached_wait_descriptor_checks = 0;
		m_cached_wait_descriptor_forwards = 0;
		m_cached_wait_descriptor_opcode_reads_removed = 0;
		m_cached_wait_descriptor_unconditional_checks = 0;
		m_compiled_ps1_bios_gate_blocks = 0;
		m_compiled_ps1_bios_gate_entries = 0;
		m_dispatcher_ps1_bios_gate_checks_removed = 0;
		s_qemuIopInlineWaitFastForwards = 0;
		s_qemuIopLinkedFrameEvidence = {};
		s_qemuIopSequentialQwordCopyFastPaths = 0;
		s_qemuIopBranchEventCandidates = 0;
		s_qemuIopBranchEventBudgetPositive = 0;
		s_qemuIopBranchEventTestsEntered = 0;
#endif
	}

#if defined(VITASX2_PORTABLE_REPLAY_VALIDATION) || \
	defined(VITASX2_PRODUCT_BOOT_VALIDATION)
	PortableValidationStats BlockExecutor::GetPortableValidationStats() const
	{
		return m_portable_validation_stats;
	}
#endif

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

	void BlockExecutor::SetRetainedLoForwardingEnabled(bool enabled)
	{
#if defined(VITASX2_QEMU_VALIDATION)
		s_qemuIopRetainedLoForwardingEnabled = enabled;
#else
		(void)enabled;
#endif
	}

	void BlockExecutor::SetPinnedBranchDirectCompareEnabled(bool enabled)
	{
#if defined(VITASX2_QEMU_VALIDATION)
		s_qemuIopPinnedBranchDirectCompareEnabled = enabled;
#else
		(void)enabled;
#endif
	}

	void BlockExecutor::SetConditionCodeBranchEnabled(bool enabled)
	{
#if defined(VITASX2_QEMU_VALIDATION)
		s_qemuIopConditionCodeBranchEnabled = enabled;
#else
		(void)enabled;
#endif
	}

	void BlockExecutor::SetProducerBranchFlagsEnabled(bool enabled)
	{
#if defined(VITASX2_QEMU_VALIDATION)
		s_qemuIopProducerBranchFlagsEnabled = enabled;
#else
		(void)enabled;
#endif
	}

	void BlockExecutor::SetRamProvenanceSpecializationEnabled(bool enabled)
	{
#if defined(VITASX2_QEMU_VALIDATION)
		s_qemuIopRamProvenanceSpecializationEnabled = enabled;
#else
		(void)enabled;
#endif
	}

	void BlockExecutor::SetSourcePageLiteralEnabled(bool enabled)
	{
#if defined(VITASX2_QEMU_VALIDATION)
		s_qemuIopSourcePageLiteralEnabled = enabled;
#else
		(void)enabled;
#endif
	}

	void BlockExecutor::SetIsolateCacheSpecializationEnabled(bool enabled)
	{
#if defined(VITASX2_QEMU_VALIDATION)
		s_qemuIopIsolateCacheSpecializationEnabled = enabled;
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

	void BlockExecutor::SetLinkedFrameBypassEnabled(bool enabled)
	{
#if defined(VITASX2_QEMU_VALIDATION)
		s_qemuIopLinkedFrameBypassEnabled = enabled;
#else
		(void)enabled;
#endif
	}

	void BlockExecutor::SetResidentPreludeLinksEnabled(bool enabled)
	{
#if defined(VITASX2_QEMU_VALIDATION)
		s_qemuIopResidentPreludeLinksEnabled = enabled;
#else
		(void)enabled;
#endif
	}

	void BlockExecutor::SetResidentGprLinksEnabled(bool enabled)
	{
#if defined(VITASX2_QEMU_VALIDATION)
		s_qemuIopResidentGprLinksEnabled = enabled;
#else
		(void)enabled;
#endif
	}

	void BlockExecutor::SetPublishedEventDeadlineResidencyEnabled(bool enabled)
	{
#if defined(VITASX2_QEMU_VALIDATION)
		s_qemuIopPublishedEventDeadlineResidencyEnabled = enabled;
#else
		(void)enabled;
#endif
	}

	void BlockExecutor::SetEeBudgetResidencyEnabled(bool enabled)
	{
#if defined(VITASX2_QEMU_VALIDATION)
		s_qemuIopEeBudgetResidencyEnabled = enabled;
#else
		(void)enabled;
#endif
	}

	void BlockExecutor::SetGeneratedInstrumentationEnabled(bool enabled)
	{
#if defined(VITASX2_QEMU_VALIDATION)
		s_qemuIopGeneratedInstrumentationEnabled = enabled;
#else
		(void)enabled;
#endif
	}

	void BlockExecutor::SetSequentialQwordCopyEnabled(bool enabled)
	{
#if defined(VITASX2_QEMU_VALIDATION)
		s_qemuIopSequentialQwordCopyEnabled = enabled;
#else
		(void)enabled;
#endif
	}

	void BlockExecutor::SetBranchTestSchedulingEnabled(bool enabled)
	{
#if defined(VITASX2_QEMU_VALIDATION)
		s_qemuIopBranchTestSchedulingEnabled = enabled;
#else
		(void)enabled;
#endif
	}

	void BlockExecutor::SetPrivateDispatcherHotPathEnabled(bool enabled)
	{
#if defined(VITASX2_QEMU_VALIDATION)
		s_qemuIopPrivateDispatcherHotPathEnabled = enabled;
#else
		(void)enabled;
#endif
	}

	void BlockExecutor::SetHotDispatchOwnershipEnabled(bool enabled)
	{
#if defined(VITASX2_QEMU_VALIDATION)
		s_qemuIopHotDispatchOwnershipEnabled = enabled;
#else
		(void)enabled;
#endif
	}

	void BlockExecutor::SetCachedWaitDescriptorEnabled(bool enabled)
	{
#if defined(VITASX2_QEMU_VALIDATION)
		s_qemuIopCachedWaitDescriptorEnabled = enabled;
#else
		(void)enabled;
#endif
	}

	void BlockExecutor::SetInlineWaitFastForwardEnabled(bool enabled)
	{
#if defined(VITASX2_QEMU_VALIDATION)
		s_qemuIopInlineWaitFastForwardEnabled = enabled;
#else
		(void)enabled;
#endif
	}

	void BlockExecutor::SetWaitResumeCacheEnabled(bool enabled)
	{
#if defined(VITASX2_QEMU_VALIDATION)
		s_qemuIopWaitResumeCacheEnabled = enabled;
#else
		(void)enabled;
#endif
	}

	void BlockExecutor::SetWaitResumeFirstEntryOwnershipEnabled(bool enabled)
	{
#if defined(VITASX2_QEMU_VALIDATION)
		s_qemuIopWaitResumeFirstEntryOwnershipEnabled = enabled;
#else
		(void)enabled;
#endif
	}

	void BlockExecutor::SetWaitResumeKindEntryEnabled(bool enabled)
	{
#if defined(VITASX2_QEMU_VALIDATION)
		s_qemuIopWaitResumeKindEntryEnabled = enabled;
#else
		(void)enabled;
#endif
	}

	void BlockExecutor::SetWaitResumeClockEntryEnabled(bool enabled)
	{
#if defined(VITASX2_QEMU_VALIDATION)
		s_qemuIopWaitResumeClockEntryEnabled = enabled;
#else
		(void)enabled;
#endif
	}

	void BlockExecutor::SetWaitResumeNoLinkEntryEnabled(bool enabled)
	{
#if defined(VITASX2_QEMU_VALIDATION)
		s_qemuIopWaitResumeNoLinkEntryEnabled = enabled;
#else
		(void)enabled;
#endif
	}

	void BlockExecutor::SetWaitResumeDescriptorSpecializationEnabled(bool enabled)
	{
#if defined(VITASX2_QEMU_VALIDATION)
		s_qemuIopWaitResumeDescriptorSpecializationEnabled = enabled;
#else
		(void)enabled;
#endif
	}

	void BlockExecutor::SetCompiledPs1BiosGateEnabled(bool enabled)
	{
#if defined(VITASX2_QEMU_VALIDATION)
		s_qemuIopCompiledPs1BiosGateEnabled = enabled;
#else
		(void)enabled;
#endif
	}

	bool BlockExecutor::CompiledPs1BiosGateEnabled()
	{
#if defined(VITASX2_IOP_COMPILED_PS1_BIOS_GATE_CONTROL)
		return false;
#elif defined(VITASX2_IOP_COMPILED_PS1_BIOS_GATE_PRODUCT)
		return true;
#elif defined(VITASX2_QEMU_VALIDATION)
		return s_qemuIopCompiledPs1BiosGateEnabled;
#else
		return true;
#endif
	}

	void BlockExecutor::SetSchedulerDirectResumeEnabled(bool enabled)
	{
#if defined(VITASX2_QEMU_VALIDATION)
		s_qemuIopSchedulerDirectResumeEnabled = enabled;
#else
		(void)enabled;
#endif
	}

	void BlockExecutor::SetNullOpcodeCompilationEnabled(bool enabled)
	{
#if defined(VITASX2_QEMU_VALIDATION)
		s_qemuIopNullOpcodeCompilationEnabled = enabled;
#else
		(void)enabled;
#endif
	}

	bool BlockExecutor::SchedulerDirectResumeEnabled()
	{
#if defined(VITASX2_IOP_SCHEDULER_DIRECT_RESUME_CONTROL)
		return false;
#elif defined(VITASX2_IOP_SCHEDULER_DIRECT_RESUME_PRODUCT)
		return true;
#elif defined(VITASX2_QEMU_VALIDATION)
		return s_qemuIopSchedulerDirectResumeEnabled;
#else
		return true;
#endif
	}

	void BlockExecutor::SetCodeCacheCapacityLimit(size_t capacity)
	{
#if defined(VITASX2_QEMU_VALIDATION)
		s_qemuIopCodeCacheCapacityLimit = capacity;
#else
		(void)capacity;
#endif
	}

	inline __attribute__((always_inline)) u32
	BlockExecutor::RecLookupIdentity(u32 pc)
	{
		// PCSX2 owners: x86/BaseblockEx.h::recLUT_SetPage() and
		// x86/iR3000A.cpp::recResetIOP(). The recompiler LUT shares one slot
		// across kuseg/kseg0/kseg1, and its 0..8 MiB RAM window repeats modulo
		// the exposed 2/8 MiB backing size. Keep the compile-owner PC separately:
		// generated static targets and one-shot iopRecRecompile() effects retain it.
		const u32 segment = pc >> 29;
		if (segment != 0 && segment != 4 && segment != 5)
			return UINT32_MAX;

		const u32 physical_pc = pc & 0x1fffffffu;
		if (physical_pc < Ps2MemSize::TotalIopRam)
			return physical_pc & (Ps2MemSize::ExposedIopRam - 1);
		if ((physical_pc >= 0x1e000000u && physical_pc < 0x1e480000u) ||
			physical_pc >= 0x1fc00000u)
		{
			return physical_pc;
		}
		return UINT32_MAX;
	}

	inline __attribute__((always_inline)) u32
	BlockExecutor::RecLinkIdentity(u32 pc)
	{
		// PCSX2's BaseBlocks::New()/Link() key is HWADDR(), not the recLUT
		// backing slot. It strips the KSEG page base but deliberately retains the
		// 0..8 MiB physical RAM mirror number.
		const u32 segment = pc >> 29;
		if (segment != 0 && segment != 4 && segment != 5)
			return UINT32_MAX;
		const u32 physical_pc = pc & 0x1fffffffu;
		if (physical_pc < Ps2MemSize::TotalIopRam ||
			(physical_pc >= 0x1e000000u && physical_pc < 0x1e480000u) ||
			physical_pc >= 0x1fc00000u)
		{
			return physical_pc;
		}
		return UINT32_MAX;
	}

	u32 BlockExecutor::LookupPageIndex(u32 rec_lookup_identity)
	{
		return rec_lookup_identity >> 16;
	}

	u32 BlockExecutor::LookupEntryIndex(u32 rec_lookup_identity)
	{
		return (rec_lookup_identity & 0xffffu) >> 2;
	}

	u32 BlockExecutor::HotDispatchCacheIndex(u32 rec_lookup_identity)
	{
		static_assert(
			(HOT_DISPATCH_CACHE_SET_COUNT & (HOT_DISPATCH_CACHE_SET_COUNT - 1)) == 0);
		return (rec_lookup_identity >> 2) & (HOT_DISPATCH_CACHE_SET_COUNT - 1);
	}

	const u32* BlockExecutor::ResolveRawOpcodeSpan(u32 start_pc,
		u32 instruction_count,
		u32* ram_source_start)
	{
		// PCSX2 owner: x86/BaseblockEx.h::recLUT_SetPage() and
		// x86/iR3000A.cpp::recResetIOP()/psxhwLUT map the
		// R3000A RAM and ROM aliases to stable backing pages. Cache that resolved
		// source span just as VitaEeExecutor.cpp::ValidateCachedBlock() does for
		// page-bounded EE code. RAM invalidation provenance is independent of that
		// raw pointer: a logical block crossing a 64 KiB LUT window still owns all
		// of its 4 KiB backing pages and must be unlinked on SMC.
		if (ram_source_start)
			*ram_source_start = INVALID_RAM_SOURCE;
		if (instruction_count == 0 ||
			instruction_count > ((UINT32_MAX - start_pc) / 4))
			return nullptr;

		const u32 byte_count = instruction_count * 4;
		u32 executable_bytes = 0;
		IopRecSourceKind source_kind;
		if (!GetIopRecExecutableSpan(start_pc, &executable_bytes, &source_kind) ||
			byte_count > executable_bytes)
		{
			return nullptr;
		}

		const u32 physical_pc = start_pc & 0x1fffffffu;
		const u32 page = physical_pc >> 16;
		if (source_kind == IopRecSourceKind::Ram)
		{
			if (ram_source_start)
			*ram_source_start = physical_pc & (Ps2MemSize::ExposedIopRam - 1);
		}

		if (byte_count > 0x10000u ||
			(physical_pc & 0xffffu) > (0x10000u - byte_count))
		{
			return nullptr;
		}

		const uptr page_base = psxMemRLUT[page];
		return page_base ? reinterpret_cast<const u32*>(page_base +
														(physical_pc & 0xffffu)) :
		                   nullptr;
	}

	bool BlockExecutor::EnsureLookupDirectory()
	{
		if (m_lookup_pages)
			return true;

		m_lookup_pages =
			new (std::nothrow) LookupPage* [LOOKUP_DIRECTORY_ENTRY_COUNT] {};
		return (m_lookup_pages != nullptr);
	}

	BlockExecutor::LookupPage* BlockExecutor::GetLookupPage(u32 rec_lookup_identity,
		bool allocate)
	{
		if (rec_lookup_identity == UINT32_MAX)
			return nullptr;
		if (!m_lookup_pages && (!allocate || !EnsureLookupDirectory()))
			return nullptr;

		const u32 page = LookupPageIndex(rec_lookup_identity);
		if (!m_lookup_pages[page] && allocate)
			m_lookup_pages[page] = new (std::nothrow) LookupPage();

		return m_lookup_pages[page];
	}

	void BlockExecutor::RegisterBlockLookup(CachedBlock& block)
	{
		if (!block.valid || block.rec_lookup_identity == UINT32_MAX)
			return;

		RegisterHotDispatchCache(block);

		// PCSX2 owner: x86/BaseblockEx.h::PC_GETBLOCK_()/recLUT_SetPage().
		// Vita keeps the same 64 KiB identity-page lookup granularity, allocated
		// lazily, while recLUT aliases share the same entry.
		if (LookupPage* page = GetLookupPage(block.rec_lookup_identity, true))
			page->blocks[block.isolate_cache_active ? 1 : 0]
						[LookupEntryIndex(block.rec_lookup_identity)] = &block;
	}

	void BlockExecutor::UnregisterBlockLookup(CachedBlock& block)
	{
		if (block.rec_lookup_identity == UINT32_MAX)
			return;

		UnregisterHotDispatchCache(block);

		if (LookupPage* page = GetLookupPage(block.rec_lookup_identity, false))
		{
			CachedBlock*& entry =
				page->blocks[block.isolate_cache_active ? 1 : 0]
							[LookupEntryIndex(block.rec_lookup_identity)];
			if (entry == &block)
				entry = nullptr;
		}
	}

	void BlockExecutor::RegisterHotDispatchCache(CachedBlock& block)
	{
		const auto register_block = [&block](auto& set) {
			for (HotDispatchCacheEntry& entry : set)
			{
				if (entry.block == &block ||
					entry.rec_lookup_identity == block.rec_lookup_identity)
				{
					entry.block = &block;
					entry.rec_lookup_identity = block.rec_lookup_identity;
					return;
				}
			}
			set[1] = set[0];
			set[0].block = &block;
			set[0].rec_lookup_identity = block.rec_lookup_identity;
		};
		auto& set =
			m_hot_dispatch_cache[block.isolate_cache_active ? 1 : 0]
								[HotDispatchCacheIndex(block.rec_lookup_identity)];
		// Keep the two most recently promoted PCs when a third PC aliases this
		// set. Hits do not reorder the ways, so a stable two-PC call chain keeps
		// both translations without per-dispatch cache writes.
		register_block(set);
#if defined(VITASX2_QEMU_VALIDATION)
		auto& control_set = m_hot_dispatch_cache_64_set_control
			[block.isolate_cache_active ? 1 : 0]
			[(block.rec_lookup_identity >> 2) &
				(HOT_DISPATCH_CACHE_CONTROL_SET_COUNT - 1)];
		register_block(control_set);
#endif
	}

	void BlockExecutor::UnregisterHotDispatchCache(CachedBlock& block)
	{
		const auto unregister_block = [&block](auto& set) {
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
		};
		auto& set =
			m_hot_dispatch_cache[block.isolate_cache_active ? 1 : 0]
								[HotDispatchCacheIndex(block.rec_lookup_identity)];
		unregister_block(set);
#if defined(VITASX2_QEMU_VALIDATION)
		auto& control_set = m_hot_dispatch_cache_64_set_control
			[block.isolate_cache_active ? 1 : 0]
			[(block.rec_lookup_identity >> 2) &
				(HOT_DISPATCH_CACHE_CONTROL_SET_COUNT - 1)];
		unregister_block(control_set);
#endif
	}

	inline __attribute__((always_inline)) BlockExecutor::CachedBlock*
	BlockExecutor::FindHotDispatchCacheBlockInline(u32 start_pc)
	{
		const u32 rec_lookup_identity = RecLookupIdentity(start_pc);
		if (rec_lookup_identity == UINT32_MAX)
			return nullptr;
#if defined(VITASX2_QEMU_VALIDATION)
		auto& control_set = m_hot_dispatch_cache_64_set_control
			[m_active_isolate_cache_mode ? 1 : 0]
			[(rec_lookup_identity >> 2) & (HOT_DISPATCH_CACHE_CONTROL_SET_COUNT - 1)];
		bool control_hit = false;
		for (HotDispatchCacheEntry& control_entry : control_set)
		{
			m_hot_dispatch_cache_64_set_way_probes++;
			if (control_entry.rec_lookup_identity == rec_lookup_identity &&
				control_entry.block)
			{
				control_hit = true;
				break;
			}
		}
		if (control_hit)
			m_hot_dispatch_cache_64_set_hits++;
		else
			m_hot_dispatch_cache_64_set_misses++;
#endif
		auto& set = m_hot_dispatch_cache[m_active_isolate_cache_mode ? 1 : 0]
										[HotDispatchCacheIndex(rec_lookup_identity)];
		for (HotDispatchCacheEntry& entry : set)
		{
#if defined(VITASX2_QEMU_VALIDATION)
			m_hot_dispatch_cache_way_probes++;
#endif
			if (entry.rec_lookup_identity != rec_lookup_identity || !entry.block)
				continue;

			// PCSX2 owner: x86/iR3000A.cpp::psxRecClearMem() owns recorded-block
			// removal and clearing the corresponding psxRecLUT range as one
			// invalidation operation. Vita's pointer-bearing first level requires
			// the stricter UnregisterBlockLookup()-before-invalidate/reuse ordering,
			// so a matching non-null record proves block identity and lifetime.
#if defined(VITASX2_IOP_HOT_DISPATCH_STALE_GUARD_CONTROL)
			constexpr bool trust_cache_ownership = false;
#elif defined(VITASX2_QEMU_VALIDATION)
			const bool trust_cache_ownership = s_qemuIopHotDispatchOwnershipEnabled;
#else
			constexpr bool trust_cache_ownership = true;
#endif
			if (!trust_cache_ownership &&
				(!entry.block->valid ||
					entry.block->rec_lookup_identity != rec_lookup_identity))
			{
				entry = {};
				return nullptr;
			}
#if defined(VITASX2_QEMU_VALIDATION)
			if (trust_cache_ownership)
				m_hot_dispatch_owned_hits++;
			// A miss in the retired geometry would fall through to the exact lazy
			// page table and promote this same block before returning. The selected
			// larger cache can hit first, so reproduce that promotion in the shadow
			// to keep every later control lookup faithful to the retired product.
			if (!control_hit)
			{
				control_set[1] = control_set[0];
				control_set[0].block = entry.block;
				control_set[0].rec_lookup_identity = rec_lookup_identity;
			}
#endif

			return entry.block;
		}
		return nullptr;
	}

	BlockExecutor::CachedBlock*
	BlockExecutor::FindHotDispatchCacheBlock(u32 start_pc)
	{
		return FindHotDispatchCacheBlockInline(start_pc);
	}

	void BlockExecutor::ClearHotDispatchCache()
	{
		m_hot_dispatch_cache = {};
#if defined(VITASX2_QEMU_VALIDATION)
		m_hot_dispatch_cache_64_set_control = {};
#endif
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

	void BlockExecutor::RegisterRamSourceChunks(u32 source_start, u32 source_size)
	{
		if (source_start == INVALID_RAM_SOURCE || source_size == 0)
			return;

		source_start &= Ps2MemSize::ExposedIopRam - 1;
		u32 remaining = source_size;
		while (remaining != 0)
		{
			const u32 span =
				std::min(remaining, Ps2MemSize::ExposedIopRam - source_start);
			const u32 source_end = source_start + span;
			for (u32 chunk_index = source_start >> RAM_SOURCE_CHUNK_SHIFT;
				chunk_index <= ((source_end - 1) >> RAM_SOURCE_CHUNK_SHIFT);
				chunk_index++)
			{
				pxAssertRel(m_ram_source_chunks->live_counts[chunk_index] != UINT32_MAX,
					"IOP source-chunk ownership overflow");
				if (m_ram_source_chunks->live_counts[chunk_index] != UINT32_MAX)
					m_ram_source_chunks->live_counts[chunk_index]++;
				m_ram_source_chunks->live_flags[chunk_index] = 1;
			}
			remaining -= span;
			source_start = 0;
		}
	}

	void BlockExecutor::UnregisterRamSourceChunks(u32 source_start,
		u32 source_size)
	{
		if (source_start == INVALID_RAM_SOURCE || source_size == 0)
			return;

		source_start &= Ps2MemSize::ExposedIopRam - 1;
		u32 remaining = source_size;
		while (remaining != 0)
		{
			const u32 span =
				std::min(remaining, Ps2MemSize::ExposedIopRam - source_start);
			const u32 source_end = source_start + span;
			for (u32 chunk_index = source_start >> RAM_SOURCE_CHUNK_SHIFT;
				chunk_index <= ((source_end - 1) >> RAM_SOURCE_CHUNK_SHIFT);
				chunk_index++)
			{
				pxAssertRel(m_ram_source_chunks->live_counts[chunk_index] != 0,
					"IOP source-chunk ownership underflow");
				if (m_ram_source_chunks->live_counts[chunk_index] != 0)
					m_ram_source_chunks->live_counts[chunk_index]--;
				m_ram_source_chunks->live_flags[chunk_index] =
					m_ram_source_chunks->live_counts[chunk_index] != 0 ? 1 : 0;
			}
			remaining -= span;
			source_start = 0;
		}
	}

	void BlockExecutor::RegisterRamSource(CachedBlock& block)
	{
		if (!block.valid || block.ram_source_start == INVALID_RAM_SOURCE)
			return;

		block.source_serial = m_next_source_serial++;
		if (m_next_source_serial == 0)
			m_next_source_serial = 1;

		std::bitset<RAM_SOURCE_PAGE_COUNT> registered_pages;
		const auto register_range = [&](u32 source_start, u32 source_size) {
			if (source_start == INVALID_RAM_SOURCE || source_size == 0)
				return;
			RegisterRamSourceChunks(source_start, source_size);
			source_start &= Ps2MemSize::ExposedIopRam - 1;
			u32 remaining = source_size;
			while (remaining != 0)
			{
				const u32 chunk =
					std::min(remaining, Ps2MemSize::ExposedIopRam - source_start);
				const u32 source_end = source_start + chunk;
			for (u32 page_index = source_start >> RAM_SOURCE_PAGE_SHIFT;
					 page_index <= ((source_end - 1) >> RAM_SOURCE_PAGE_SHIFT);
					 page_index++)
			{
					if (registered_pages.test(page_index))
					continue;

					m_ram_source_pages[page_index].push_back(
						{&block, nullptr, block.source_serial});
				m_ram_source_page_live_counts[page_index]++;
				m_ram_source_page_live_flags[page_index] = 1;
					registered_pages.set(page_index);
				}
				remaining -= chunk;
				source_start = 0;
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

		std::bitset<RAM_SOURCE_PAGE_COUNT> unregistered_pages;
		const auto unregister_range = [&](u32 source_start, u32 source_size) {
			if (source_start == INVALID_RAM_SOURCE || source_size == 0)
				return;
			UnregisterRamSourceChunks(source_start, source_size);
			source_start &= Ps2MemSize::ExposedIopRam - 1;
			u32 remaining = source_size;
			while (remaining != 0)
			{
				const u32 chunk =
					std::min(remaining, Ps2MemSize::ExposedIopRam - source_start);
				const u32 source_end = source_start + chunk;
			for (u32 page_index = source_start >> RAM_SOURCE_PAGE_SHIFT;
					 page_index <= ((source_end - 1) >> RAM_SOURCE_PAGE_SHIFT);
					 page_index++)
			{
					if (unregistered_pages.test(page_index))
					continue;

				if (m_ram_source_page_live_counts[page_index] != 0)
					m_ram_source_page_live_counts[page_index]--;
				m_ram_source_page_live_flags[page_index] =
					m_ram_source_page_live_counts[page_index] != 0 ? 1 : 0;
					unregistered_pages.set(page_index);
				}
				remaining -= chunk;
				source_start = 0;
			}
		};

		unregister_range(block.ram_source_start,
			block.instruction_count * sizeof(u32));
		if (block.poll_call_wait_loop)
		{
			unregister_range(block.poll_branch_source_start, 2 * sizeof(u32));
			unregister_range(block.poll_leaf_source_start, 5 * sizeof(u32));
		}
	}

	void BlockExecutor::RegisterRamSource(InterpreterFallbackBlock& block)
	{
		if (!block.valid || block.ram_source_start == INVALID_RAM_SOURCE ||
			block.instruction_count == 0)
		{
			return;
		}

		block.source_serial = m_next_source_serial++;
		if (m_next_source_serial == 0)
			m_next_source_serial = 1;
		RegisterRamSourceChunks(block.ram_source_start,
			block.instruction_count * sizeof(u32));

		std::bitset<RAM_SOURCE_PAGE_COUNT> registered_pages;
		u32 source_start = block.ram_source_start & (Ps2MemSize::ExposedIopRam - 1);
		u32 remaining = block.instruction_count * sizeof(u32);
		while (remaining != 0)
		{
			const u32 chunk =
				std::min(remaining, Ps2MemSize::ExposedIopRam - source_start);
			const u32 source_end = source_start + chunk;
			for (u32 page_index = source_start >> RAM_SOURCE_PAGE_SHIFT;
				 page_index <= ((source_end - 1) >> RAM_SOURCE_PAGE_SHIFT);
				 page_index++)
			{
				if (registered_pages.test(page_index))
					continue;
				m_ram_source_pages[page_index].push_back(
					{nullptr, &block, block.source_serial});
				m_ram_source_page_live_counts[page_index]++;
				m_ram_source_page_live_flags[page_index] = 1;
				registered_pages.set(page_index);
			}
			remaining -= chunk;
			source_start = 0;
		}
	}

	void BlockExecutor::UnregisterRamSource(const InterpreterFallbackBlock& block)
	{
		if (!block.valid || block.ram_source_start == INVALID_RAM_SOURCE ||
			block.instruction_count == 0)
		{
			return;
		}
		UnregisterRamSourceChunks(block.ram_source_start,
			block.instruction_count * sizeof(u32));

		std::bitset<RAM_SOURCE_PAGE_COUNT> unregistered_pages;
		u32 source_start = block.ram_source_start & (Ps2MemSize::ExposedIopRam - 1);
		u32 remaining = block.instruction_count * sizeof(u32);
		while (remaining != 0)
		{
			const u32 chunk =
				std::min(remaining, Ps2MemSize::ExposedIopRam - source_start);
			const u32 source_end = source_start + chunk;
			for (u32 page_index = source_start >> RAM_SOURCE_PAGE_SHIFT;
				 page_index <= ((source_end - 1) >> RAM_SOURCE_PAGE_SHIFT);
				 page_index++)
			{
				if (unregistered_pages.test(page_index))
					continue;
				if (m_ram_source_page_live_counts[page_index] != 0)
					m_ram_source_page_live_counts[page_index]--;
				m_ram_source_page_live_flags[page_index] =
					m_ram_source_page_live_counts[page_index] != 0 ? 1 : 0;
				unregistered_pages.set(page_index);
			}
			remaining -= chunk;
			source_start = 0;
		}
	}

	bool BlockExecutor::AnalyzePollCallWaitLoop(CachedBlock& block, u32 start_pc,
		u32 instruction_count)
	{
		// PCSX2 owners: x86/iR3000A.cpp::iPsxBranchTest() supplies the IOP
		// deadline/event fast-forward contract. The pure-load proof is the
		// R3000A adaptation of the load-aware loop analysis in
		// x86/ix86-32/iR5900.cpp::recRecompile()/recSkipTimeoutLoop().
		if (instruction_count != 2 || (block.Opcode(0) >> 26) != 0x03 ||
			block.Opcode(1) != 0)
			return false;
		if ((start_pc & 0x1fffffffu) >= Ps2MemSize::TotalIopRam)
			return false;

		const u32 branch_pc = start_pc + 2 * sizeof(u32);
		u32 branch_source_start = INVALID_RAM_SOURCE;
		const u32* const branch_opcodes =
			ResolveRawOpcodeSpan(branch_pc, 2, &branch_source_start);
		if (!branch_opcodes || branch_source_start == INVALID_RAM_SOURCE)
			return false;

		const u32 branch_op = branch_opcodes[0];
		if ((branch_op >> 26) != 0x04 ||
			BranchTarget(branch_pc, branch_op) != start_pc)
			return false;

		unsigned result_register = 0;
		if (RS(branch_op) == 0 && RT(branch_op) != 0)
			result_register = RT(branch_op);
		else if (RT(branch_op) == 0 && RS(branch_op) != 0)
			result_register = RS(branch_op);
		else
			return false;

		const u32 branch_delay = branch_opcodes[1];
		const u32 clear_result_delay =
			(result_register << 11) | 0x21u; // ADDU result,$zero,$zero
		if (branch_delay != 0 && branch_delay != clear_result_delay)
			return false;

		const u32 leaf_pc = JumpTarget(start_pc, block.Opcode(0));
		u32 leaf_source_start = INVALID_RAM_SOURCE;
		const u32* const leaf_opcodes =
			ResolveRawOpcodeSpan(leaf_pc, 5, &leaf_source_start);
		if (!leaf_opcodes || leaf_source_start == INVALID_RAM_SOURCE)
			return false;

		const u32 lui = leaf_opcodes[0];
		const u32 addiu = leaf_opcodes[1];
		const u32 load = leaf_opcodes[2];
		if ((lui >> 26) != 0x0f || RS(lui) != 0 || (addiu >> 26) != 0x09 ||
			RS(addiu) != RT(lui) || RT(addiu) != RT(lui) || (load >> 26) != 0x23 ||
			RS(load) != RT(lui) || RT(load) != result_register ||
			leaf_opcodes[3] != 0x03e00008u || leaf_opcodes[4] != 0)
		{
			return false;
		}

		u32 poll_address = IMM_U(lui) << 16;
		poll_address += static_cast<u32>(static_cast<s32>(IMM_S(addiu)));
		poll_address += static_cast<u32>(static_cast<s32>(IMM_S(load)));
		const u32 physical_address = poll_address & 0x1fffffffu;
		if ((physical_address & 3u) != 0 ||
			physical_address >= Ps2MemSize::TotalIopRam)
			return false;

		block.poll_call_wait_loop = true;
		block.inline_ram_poll_wait_loop = false;
		block.poll_result_register = static_cast<u8>(result_register);
		block.poll_load_opcode = 0x23; // LW
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

	bool BlockExecutor::AnalyzeInlineRamPollWaitLoop(CachedBlock& block,
		u32 start_pc, u32 instruction_count)
	{
		// PCSX2 owners: x86/ix86-32/iR5900.cpp::recRecompile() permits a
		// self-loop whose only machine-state input is a load to use s_nBlockFF;
		// x86/iR3000A.cpp::iPsxBranchTest() owns the IOP deadline/event advance.
		// Keep this first R3000A adaptation deliberately narrower: one constant
		// address, one ordinary-RAM scalar load, its required load-delay NOP, and
		// a BEQ-zero backedge with a NOP delay slot.
		if (instruction_count != 5)
			return false;

		const u32 lui = block.Opcode(0);
		const u32 load = block.Opcode(1);
		const u32 load_delay = block.Opcode(2);
		const u32 branch = block.Opcode(3);
		const u32 branch_delay = block.Opcode(4);
		if ((lui >> 26) != 0x0f || RS(lui) != 0 || RT(lui) == 0 ||
			load_delay != 0 || branch_delay != 0 || (branch >> 26) != 0x04 ||
			BranchTarget(start_pc + 3 * sizeof(u32), branch) != start_pc)
		{
			return false;
		}

		const u32 load_opcode = load >> 26;
		switch (load_opcode)
		{
			case 0x20: // LB
			case 0x21: // LH
			case 0x23: // LW
			case 0x24: // LBU
			case 0x25: // LHU
				break;
			default:
				return false;
		}

		const unsigned base_register = RT(lui);
		const unsigned result_register = RT(load);
		if (RS(load) != base_register || result_register != base_register ||
			!((RS(branch) == result_register && RT(branch) == 0) ||
				(RT(branch) == result_register && RS(branch) == 0)))
		{
			return false;
		}

		const u32 effective_address =
			(IMM_U(lui) << 16) + static_cast<u32>(static_cast<s32>(IMM_S(load)));
		u32 poll_address = 0;
		if (!TryMappedIopRamEffectiveAddress(effective_address,
				DirectIopRamAlignmentMask(load), &poll_address))
		{
			return false;
		}

		block.inline_ram_poll_wait_loop = true;
		block.poll_call_wait_loop = false;
		block.poll_result_register = static_cast<u8>(result_register);
		block.poll_load_opcode = static_cast<u8>(load_opcode);
		block.poll_word_address = poll_address;
		return true;
	}

	u32 BlockExecutor::InvalidateRamSourceRange(u32 start, u32 size)
	{
		if (size == 0 || start >= Ps2MemSize::ExposedIopRam ||
			size > Ps2MemSize::ExposedIopRam - start)
		{
			return 0;
		}

		struct SourceRange
		{
			u32 start;
			u32 end;
		};
		// A connected source extent on circular IOP RAM has at most two linear
		// components. Keep them on the stack: psxRecClearMem() is reached from a
		// generated store and must not allocate or sort heap storage.
		std::array<SourceRange, 2> closure{};
		u32 closure_count = 0;
		const auto add_range = [&closure, &closure_count](
			u32 range_start, u32 range_end) {
			if (range_start >= range_end)
				return false;
			for (u32 i = 0; i < closure_count; i++)
			{
				const SourceRange& range = closure[i];
				if (range_start >= range.start && range_end <= range.end)
					return false;
			}

			std::array<SourceRange, 3> candidates{};
			for (u32 i = 0; i < closure_count; i++)
				candidates[i] = closure[i];
			candidates[closure_count] = {range_start, range_end};
			const u32 candidate_count = closure_count + 1;
			for (u32 i = 1; i < candidate_count; i++)
			{
				const SourceRange candidate = candidates[i];
				u32 j = i;
				while (j != 0 && candidates[j - 1].start > candidate.start)
				{
					candidates[j] = candidates[j - 1];
					j--;
				}
				candidates[j] = candidate;
			}
			u32 write_index = 0;
			closure[0] = candidates[0];
			for (u32 read_index = 1; read_index < candidate_count; read_index++)
			{
				if (candidates[read_index].start <= closure[write_index].end)
				{
					closure[write_index].end =
						std::max(closure[write_index].end, candidates[read_index].end);
					continue;
				}
				pxAssertRel(write_index + 1 < closure.size(),
					"IOP SMC closure exceeded its two circular-RAM components");
				if (write_index + 1 < closure.size())
					closure[++write_index] = candidates[read_index];
			}
			closure_count = write_index + 1;
			return true;
		};
		const auto visit_source_ranges = [](u32 source_start, u32 source_size,
			const auto& visitor) {
			if (source_start == INVALID_RAM_SOURCE || source_size == 0)
				return;
			source_start &= Ps2MemSize::ExposedIopRam - 1;
			u32 remaining = source_size;
			while (remaining != 0)
			{
				const u32 chunk =
					std::min(remaining, Ps2MemSize::ExposedIopRam - source_start);
				visitor(source_start, source_start + chunk);
				remaining -= chunk;
				source_start = 0;
			}
		};
		const auto range_overlaps =
			[&closure, &closure_count, &visit_source_ranges](
			u32 source_start, u32 source_size) {
			bool overlaps = false;
			visit_source_ranges(source_start, source_size,
				[&](u32 range_start, u32 range_end) {
					for (u32 i = 0; i < closure_count; i++)
					{
						const SourceRange& invalidation = closure[i];
						if (range_start < invalidation.end &&
							invalidation.start < range_end)
						{
							overlaps = true;
							return;
						}
					}
				});
			return overlaps;
		};

		add_range(start, start + size);
		const auto first_descriptor_at_or_after = [&](u32 identity) {
			return static_cast<u32>(std::lower_bound(
				m_semantic_block_descriptors.begin(),
				m_semantic_block_descriptors.end(), identity,
				[](const SemanticBlockDescriptor& descriptor, u32 value) {
					return descriptor.rec_lookup_identity < value;
				}) - m_semantic_block_descriptors.begin());
		};
		const auto expand_descriptor = [&](
			const SemanticBlockDescriptor& descriptor) {
			const u32 source_size = descriptor.instruction_count * sizeof(u32);
			if (!range_overlaps(descriptor.ram_source_start, source_size))
				return false;
			bool expanded = false;
			visit_source_ranges(descriptor.ram_source_start, source_size,
				[&](u32 range_start, u32 range_end) {
					expanded |= add_range(range_start, range_end);
				});
			return expanded;
		};
		for (;;)
		{
			bool expanded = false;
			if (closure_count == 1)
			{
				constexpr u32 max_source_bytes =
					MAX_LOGICAL_BLOCK_INSTRUCTIONS * sizeof(u32);
				const u32 ram_size = Ps2MemSize::ExposedIopRam;

				// A low interval can overlap the low half of a source which wraps
				// from the end of exposed RAM. Probe only the maximum legal block
				// window at that end; ordinary invalidations stay on the local path.
				if (closure[0].start < max_source_bytes)
				{
					const u32 tail_begin =
						first_descriptor_at_or_after(ram_size - max_source_bytes);
					const u32 tail_end = first_descriptor_at_or_after(ram_size);
					for (u32 i = tail_end; i > tail_begin; i--)
						expanded |= expand_descriptor(
							m_semantic_block_descriptors[i - 1]);
				}

				if (closure_count == 1)
				{
					// PCSX2 psxRecClearMem() finds the containing block, grows the
					// lower extent backwards, then consumes starts below the dynamic
					// upper extent forwards. recLUT identity equals RAM source offset,
					// and the maximum block size supplies a safe early lower bound.
					u32 i = first_descriptor_at_or_after(closure[0].end);
					while (i != 0)
					{
						const SemanticBlockDescriptor& descriptor =
							m_semantic_block_descriptors[i - 1];
						if (descriptor.rec_lookup_identity >= ram_size)
						{
							i--;
							continue;
						}
						if (static_cast<u64>(descriptor.rec_lookup_identity) +
								max_source_bytes <= closure[0].start)
						{
							break;
						}
						expanded |= expand_descriptor(descriptor);
						i--;
					}

					i = first_descriptor_at_or_after(closure[0].start);
					while (i < m_semantic_block_descriptors.size() &&
						m_semantic_block_descriptors[i].rec_lookup_identity <
							closure[0].end)
					{
						expanded |=
							expand_descriptor(m_semantic_block_descriptors[i]);
						i++;
					}
				}
			}
			else
			{
				// Crossing exposed-RAM wrap produces two linear components. It is
				// rare; retain the allocation-free exhaustive owner walk so neither
				// side can miss a transitive overlap.
				for (u32 i = static_cast<u32>(m_semantic_block_descriptors.size());
					i != 0; i--)
				{
					expanded |=
						expand_descriptor(m_semantic_block_descriptors[i - 1]);
				}
				for (const SemanticBlockDescriptor& descriptor :
					m_semantic_block_descriptors)
				{
					expanded |= expand_descriptor(descriptor);
				}
			}
			if (!expanded)
				break;
		}

		// PCSX2 owner: x86/iR3000A.cpp::psxRecClearMem() expands lowerextent
		// and upperextent through every overlapping BaseBlock before removing
		// recBlocks and clearing recLUT. The compact descriptors are the Vita
		// equivalent after physical code eviction, including wraparound RAM spans.
		u32 descriptor_write_index = 0;
		for (u32 descriptor_read_index = 0;
			descriptor_read_index < m_semantic_block_descriptors.size();
			descriptor_read_index++)
		{
			const SemanticBlockDescriptor& descriptor =
				m_semantic_block_descriptors[descriptor_read_index];
			if (!range_overlaps(descriptor.ram_source_start,
					descriptor.instruction_count * sizeof(u32)))
			{
				if (descriptor_write_index != descriptor_read_index)
				{
					m_semantic_block_descriptors[descriptor_write_index] = descriptor;
				}
				descriptor_write_index++;
				continue;
			}
			UnregisterSemanticRamSource(descriptor);
		}
		m_semantic_block_descriptors.resize(descriptor_write_index);

		u32 invalidated = 0;
		std::bitset<RAM_SOURCE_PAGE_COUNT> visited_pages;
		for (u32 closure_index = 0; closure_index < closure_count; closure_index++)
		{
			const SourceRange& invalidation = closure[closure_index];
			for (u32 page_index = invalidation.start >> RAM_SOURCE_PAGE_SHIFT;
				page_index <= ((invalidation.end - 1) >> RAM_SOURCE_PAGE_SHIFT);
				page_index++)
			{
				if (visited_pages.test(page_index))
					continue;
				visited_pages.set(page_index);
				std::vector<RamSourceRecord>& records =
					m_ram_source_pages[page_index];
				u32 write_index = 0;
				for (u32 read_index = 0; read_index < records.size(); read_index++)
				{
#if defined(VITASX2_QEMU_VALIDATION)
					m_ram_invalidation_record_visits++;
#endif
					const RamSourceRecord record = records[read_index];
					CachedBlock* block = record.block;
					InterpreterFallbackBlock* fallback = record.fallback;
					const bool cached_owner = block && block->valid &&
						block->source_serial == record.serial &&
						block->ram_source_start != INVALID_RAM_SOURCE;
					const bool fallback_owner = fallback && fallback->valid &&
						fallback->source_serial == record.serial &&
						fallback->ram_source_start != INVALID_RAM_SOURCE;
					if (!cached_owner && !fallback_owner)
						continue;

					const u32 owner_source_start = cached_owner ?
						block->ram_source_start : fallback->ram_source_start;
					const u32 owner_source_size = (cached_owner ?
						block->instruction_count : fallback->instruction_count) * sizeof(u32);
					const bool overlaps =
						range_overlaps(owner_source_start, owner_source_size) ||
						(cached_owner && block->poll_call_wait_loop &&
							(range_overlaps(block->poll_branch_source_start, 2 * sizeof(u32)) ||
							 range_overlaps(block->poll_leaf_source_start, 5 * sizeof(u32))));
					if (overlaps)
					{
						if (cached_owner)
							InvalidateCachedBlock(*block);
						else
							InvalidateInterpreterFallbackBlock(*fallback);
						invalidated++;
						continue;
					}

					if (write_index != read_index)
						records[write_index] = record;
					write_index++;
				}
				records.resize(write_index);
			}
		}
		return invalidated;
	}

	void BlockExecutor::ClearRamSourcePages()
	{
		for (std::vector<RamSourceRecord>& page : m_ram_source_pages)
			page.clear();
		m_ram_source_page_live_counts.fill(0);
		m_ram_source_page_live_flags.fill(0);
		m_ram_source_chunks->live_counts.fill(0);
		m_ram_source_chunks->live_flags.fill(0);
		m_next_source_serial = 1;
	}

	BlockExecutor::InterpreterFallbackBlock*
	BlockExecutor::FindInterpreterFallbackBlock(u32 start_pc) const
	{
		const u32 rec_lookup_identity = RecLookupIdentity(start_pc);
		if (rec_lookup_identity == UINT32_MAX)
			return nullptr;

		const auto it = std::lower_bound(
			m_interpreter_fallback_blocks.begin(),
			m_interpreter_fallback_blocks.end(), rec_lookup_identity,
			[](const std::unique_ptr<InterpreterFallbackBlock>& block, u32 identity) {
				return block->rec_lookup_identity < identity;
			});
		return it != m_interpreter_fallback_blocks.end() &&
		               (*it)->rec_lookup_identity == rec_lookup_identity &&
		               (*it)->valid ?
		           it->get() :
		           nullptr;
	}

	bool BlockExecutor::RegisterInterpreterFallbackBlock(u32 start_pc)
	{
		u32 rec_lookup_identity = RecLookupIdentity(start_pc);
		const u32 rec_link_identity = RecLinkIdentity(start_pc);
		if (rec_lookup_identity == UINT32_MAX || rec_link_identity == UINT32_MAX)
			return false;
		if (FindInterpreterFallbackBlock(start_pc))
			return true;

		auto lower = std::lower_bound(
			m_interpreter_fallback_blocks.begin(),
			m_interpreter_fallback_blocks.end(), rec_lookup_identity,
			[](const std::unique_ptr<InterpreterFallbackBlock>& block, u32 identity) {
				return block->rec_lookup_identity < identity;
			});
		InterpreterFallbackBlock* block = nullptr;
		if (lower != m_interpreter_fallback_blocks.end() &&
			(*lower)->rec_lookup_identity == rec_lookup_identity)
		{
			block = lower->get();
		}
		else
		{
			if (m_interpreter_fallback_blocks.size() >= MAX_CACHE_CAPACITY)
			{
				// PCSX2's recResetIOP() retires BaseBlock metadata together with
				// its code arena. Re-scan after the same whole-cache ownership seam.
				ResetForCachePressure();
				rec_lookup_identity = RecLookupIdentity(start_pc);
				lower = m_interpreter_fallback_blocks.begin();
			}
			std::unique_ptr<InterpreterFallbackBlock> entry(
				new (std::nothrow) InterpreterFallbackBlock());
			if (!entry)
				return false;
			block = entry.get();
			lower = m_interpreter_fallback_blocks.insert(lower, std::move(entry));
		}
		block->start_pc = start_pc;
		block->rec_lookup_identity = rec_lookup_identity;
		block->rec_link_identity = rec_link_identity;

		BlockScanResult scan;
		if (ScanProviderLogicalBlock(start_pc, &scan) != BlockScanStatus::Success ||
			scan.instruction_count == 0)
		{
			return false;
		}

		std::vector<u32> opcodes;
		opcodes.resize(scan.instruction_count);
		for (u32 i = 0; i < scan.instruction_count; i++)
		{
			const u32* const opcode = ResolveIopRecOpcode(start_pc + i * sizeof(u32));
			if (!opcode)
				return false;
			opcodes[i] = *opcode;
		}

		u32 ram_source_start = INVALID_RAM_SOURCE;
		ResolveRawOpcodeSpan(start_pc, scan.instruction_count, &ram_source_start);
		block->opcodes = std::move(opcodes);
		block->instruction_count = scan.instruction_count;
		block->stop_pc = scan.stop_pc;
		block->ram_source_start = ram_source_start;
		block->logical_continuation = scan.logical_continuation;
		block->valid = true;
		RegisterRamSource(*block);
		RememberSemanticBlockDescriptor(block->rec_lookup_identity,
			block->ram_source_start, block->instruction_count,
			block->logical_continuation);
		return true;
	}

	void BlockExecutor::InvalidateInterpreterFallbackBlock(
		InterpreterFallbackBlock& block)
	{
		if (!block.valid)
			return;
		UnregisterRamSource(block);
		block.valid = false;
		block.source_serial = 0;
		block.instruction_count = 0;
		block.stop_pc = block.start_pc;
		block.ram_source_start = INVALID_RAM_SOURCE;
		block.logical_continuation = false;
		block.opcodes.clear();
	}

	s32 BlockExecutor::LastBlockRecordIndex(u32 rec_lookup_identity) const
	{
		if (m_block_records.empty())
			return -1;

		s32 min = 0;
		s32 max = static_cast<s32>(m_block_records.size() - 1);
		while (min != max)
		{
			const s32 mid = (min + max + 1) >> 1;
			if (m_block_records[mid].rec_lookup_identity > rec_lookup_identity)
				max = mid - 1;
			else
				min = mid;
		}

		return min;
	}

	bool BlockExecutor::RegisterBlockRecord(CachedBlock& block)
	{
		if (!block.valid || block.rec_lookup_identity == UINT32_MAX)
			return false;

		// CompileIntoCacheEntry() retires the CachedBlock before generating its
		// replacement. Registration has one caller, so repeating the sorted
		// lookup/erase here only searches for a record which cannot exist.
#if defined(VITASX2_QEMU_VALIDATION)
		for (const BlockRecord& record : m_block_records)
		{
			pxAssertRel(record.block != &block,
				"IOP block registration retained its previous record");
		}
#endif
		if (m_block_records.size() >= MAX_CACHE_CAPACITY)
			return false;

		// PCSX2 owners: BaseBlockArray::insert() supplies the sorted metadata,
		// while recLUT_SetPage() supplies dispatch/scanner identity. Sort Vita's
		// secondary lookup records by the latter so all aliases share one run.
		u32 insert_index = 0;
		u32 insert_limit = static_cast<u32>(m_block_records.size());
		while (insert_index < insert_limit)
		{
			const u32 mid = (insert_index + insert_limit) >> 1;
			if (m_block_records[mid].rec_lookup_identity <= block.rec_lookup_identity)
				insert_index = mid + 1;
			else
				insert_limit = mid;
		}

		m_block_records.insert(
			m_block_records.begin() + insert_index,
			{&block, block.rec_lookup_identity});
		return true;
	}

	void BlockExecutor::UnregisterBlockRecord(CachedBlock& block)
	{
		// PCSX2 owner: x86/BaseblockEx.cpp::BaseBlocks::LastIndex() plus
		// BaseBlocks::Remove(). Records are sorted by recLUT identity, so only
		// the same-identity run can contain this block.
		s32 index = LastBlockRecordIndex(block.rec_lookup_identity);
		while (index >= 0 && m_block_records[index].rec_lookup_identity ==
								 block.rec_lookup_identity)
			index--;
		index++;

		for (;
			 index >= 0 && static_cast<u32>(index) < m_block_records.size() &&
			 m_block_records[index].rec_lookup_identity == block.rec_lookup_identity;
			 index++)
		{
			if (m_block_records[index].block == &block)
			{
				m_block_records.erase(m_block_records.begin() + index);
				return;
			}
		}
	}

	void BlockExecutor::ClearBlockRecords() { m_block_records.clear(); }

	const BlockExecutor::SemanticBlockDescriptor*
	BlockExecutor::FindSemanticBlockDescriptor(u32 rec_lookup_identity) const
	{
		const auto it = std::lower_bound(m_semantic_block_descriptors.begin(),
			m_semantic_block_descriptors.end(), rec_lookup_identity,
			[](const SemanticBlockDescriptor& descriptor, u32 identity) {
				return descriptor.rec_lookup_identity < identity;
			});
		return it != m_semantic_block_descriptors.end() &&
			it->rec_lookup_identity == rec_lookup_identity ?
			&*it : nullptr;
	}

	void BlockExecutor::RegisterSemanticRamSource(
		const SemanticBlockDescriptor& descriptor)
	{
		if (descriptor.ram_source_start == INVALID_RAM_SOURCE ||
			descriptor.instruction_count == 0)
		{
			return;
		}
		RegisterRamSourceChunks(descriptor.ram_source_start,
			descriptor.instruction_count * sizeof(u32));

		std::bitset<RAM_SOURCE_PAGE_COUNT> registered_pages;
		u32 source_start =
			descriptor.ram_source_start & (Ps2MemSize::ExposedIopRam - 1);
		u32 remaining = descriptor.instruction_count * sizeof(u32);
		while (remaining != 0)
		{
			const u32 chunk =
				std::min(remaining, Ps2MemSize::ExposedIopRam - source_start);
			const u32 source_end = source_start + chunk;
			for (u32 page_index = source_start >> RAM_SOURCE_PAGE_SHIFT;
				 page_index <= ((source_end - 1) >> RAM_SOURCE_PAGE_SHIFT);
				 page_index++)
			{
				if (registered_pages.test(page_index))
					continue;
				pxAssertRel(m_ram_source_page_live_counts[page_index] != UINT32_MAX,
					"IOP semantic source-page ownership overflow");
				if (m_ram_source_page_live_counts[page_index] != UINT32_MAX)
					m_ram_source_page_live_counts[page_index]++;
				m_ram_source_page_live_flags[page_index] = 1;
				registered_pages.set(page_index);
			}
			remaining -= chunk;
			source_start = 0;
		}
	}

	void BlockExecutor::UnregisterSemanticRamSource(
		const SemanticBlockDescriptor& descriptor)
	{
		if (descriptor.ram_source_start == INVALID_RAM_SOURCE ||
			descriptor.instruction_count == 0)
		{
			return;
		}
		UnregisterRamSourceChunks(descriptor.ram_source_start,
			descriptor.instruction_count * sizeof(u32));

		std::bitset<RAM_SOURCE_PAGE_COUNT> unregistered_pages;
		u32 source_start =
			descriptor.ram_source_start & (Ps2MemSize::ExposedIopRam - 1);
		u32 remaining = descriptor.instruction_count * sizeof(u32);
		while (remaining != 0)
		{
			const u32 chunk =
				std::min(remaining, Ps2MemSize::ExposedIopRam - source_start);
			const u32 source_end = source_start + chunk;
			for (u32 page_index = source_start >> RAM_SOURCE_PAGE_SHIFT;
				 page_index <= ((source_end - 1) >> RAM_SOURCE_PAGE_SHIFT);
				 page_index++)
			{
				if (unregistered_pages.test(page_index))
					continue;
				pxAssertRel(m_ram_source_page_live_counts[page_index] != 0,
					"IOP semantic source-page ownership underflow");
				if (m_ram_source_page_live_counts[page_index] != 0)
					m_ram_source_page_live_counts[page_index]--;
				m_ram_source_page_live_flags[page_index] =
					m_ram_source_page_live_counts[page_index] != 0 ? 1 : 0;
				unregistered_pages.set(page_index);
			}
			remaining -= chunk;
			source_start = 0;
		}
	}

	void BlockExecutor::RememberSemanticBlockDescriptor(u32 rec_lookup_identity,
		u32 ram_source_start, u32 instruction_count, bool logical_continuation)
	{
		if (rec_lookup_identity == UINT32_MAX || instruction_count == 0)
			return;

		auto it = std::lower_bound(m_semantic_block_descriptors.begin(),
			m_semantic_block_descriptors.end(), rec_lookup_identity,
			[](const SemanticBlockDescriptor& descriptor, u32 identity) {
				return descriptor.rec_lookup_identity < identity;
			});
		SemanticBlockDescriptor replacement = {
			rec_lookup_identity,
			ram_source_start,
			instruction_count,
			logical_continuation,
		};
		if (it != m_semantic_block_descriptors.end() &&
			it->rec_lookup_identity == rec_lookup_identity)
		{
			// PCSX2 leaves an already-published BASEBLOCKEX and recLUT entry in
			// place until psxRecClearMem() or a true recompiler reset removes it.
			// An overlapping block compiled later cannot reshape this start.
			return;
		}

		it = m_semantic_block_descriptors.insert(it, replacement);
		RegisterSemanticRamSource(*it);
	}

	void BlockExecutor::ForgetSemanticBlockDescriptorsForLookupRange(
		u32 start, u32 size)
	{
		if (size == 0)
			return;
		const u64 end = static_cast<u64>(start) + size;
		for (u32 i = 0; i < m_semantic_block_descriptors.size();)
		{
			const SemanticBlockDescriptor& descriptor =
				m_semantic_block_descriptors[i];
			const u64 descriptor_start = descriptor.rec_lookup_identity;
			const u64 descriptor_end = descriptor_start +
				static_cast<u64>(descriptor.instruction_count) * sizeof(u32);
			if (static_cast<u64>(start) >= descriptor_end || descriptor_start >= end)
			{
				i++;
				continue;
			}
			UnregisterSemanticRamSource(descriptor);
			m_semantic_block_descriptors.erase(
				m_semantic_block_descriptors.begin() + i);
		}
	}

	BlockExecutor::CachedBlock* BlockExecutor::FindRecordedBlockByStartPc(
		u32 start_pc, u32 instruction_count, bool match_instruction_count,
		bool isolate_cache_active, bool discovered_topology_only)
	{
		const u32 rec_lookup_identity = RecLookupIdentity(start_pc);
		if (rec_lookup_identity == UINT32_MAX)
			return nullptr;
		s32 index = LastBlockRecordIndex(rec_lookup_identity);
		while (index >= 0 &&
			   m_block_records[index].rec_lookup_identity == rec_lookup_identity)
		{
			CachedBlock* block = m_block_records[index].block;
			if (block && block->valid &&
				(!discovered_topology_only || block->discovered_topology) &&
				block->isolate_cache_active == isolate_cache_active &&
				(!match_instruction_count ||
					block->instruction_count == instruction_count))
			{
				if (ValidateCachedBlock(*block))
					return block;

				return nullptr;
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
		block.next_free = m_free_cache_head;
		m_free_cache_head = &block;
	}

	BlockExecutor::CachedBlock* BlockExecutor::TakeFreeCacheEntry()
	{
		while (m_free_cache_head)
		{
			CachedBlock* block = m_free_cache_head;
			m_free_cache_head = block->next_free;
			block->next_free = nullptr;
			block->queued_free = false;
			if (!block->valid)
				return block;
		}

		return nullptr;
	}

	void BlockExecutor::RemoveFreeCacheEntry(CachedBlock& block)
	{
		if (!block.queued_free)
			return;

		CachedBlock** link = &m_free_cache_head;
		while (*link && *link != &block)
			link = &(*link)->next_free;
		if (*link == &block)
			*link = block.next_free;
		block.next_free = nullptr;
		block.queued_free = false;
	}

	DirectLinkSlot*
	BlockExecutor::GetRecordedDirectLink(IncomingLinkRecord& record)
	{
		if (!record.source || !record.source->valid ||
			record.slot_index >= DIRECT_LINK_SLOT_COUNT)
			return nullptr;

		DirectLinkSlot& link = record.source->direct_links.slots[record.slot_index];
		if (!link.valid ||
			RecLookupIdentity(link.target_pc) != record.target_lookup_identity ||
			RecLinkIdentity(link.target_pc) != record.target_link_identity)
			return nullptr;

		return &link;
	}

	s32 BlockExecutor::LastIncomingLinkIndex(u32 target_lookup_identity) const
	{
		if (m_incoming_links.empty())
			return -1;

		s32 min = 0;
		s32 max = static_cast<s32>(m_incoming_links.size() - 1);
		while (min != max)
		{
			const s32 mid = (min + max + 1) >> 1;
			if (m_incoming_links[mid].target_lookup_identity > target_lookup_identity)
				max = mid - 1;
			else
				min = mid;
		}

		return min;
	}

	void BlockExecutor::ClearIncomingLinks() { m_incoming_links.clear(); }

	void BlockExecutor::RegisterIncomingLinks(CachedBlock& block)
	{
		// The common compile seam already retired all records owned by this
		// metadata object. Avoid a second full traversal of the incoming-link
		// table on every cold compile.
#if defined(VITASX2_QEMU_VALIDATION)
		for (const IncomingLinkRecord& record : m_incoming_links)
		{
			pxAssertRel(record.source != &block,
				"IOP link registration retained its previous source record");
		}
#endif

		// PCSX2 owner: x86/BaseblockEx.cpp::BaseBlocks::Link(). Keep target-PC
		// -> source patch-site records so invalidating a block only repairs its
		// incoming edges. recLUT identity owns scanner/topology dependencies;
		// HWADDR identity owns BaseBlocks::Link patching. They differ for retail
		// 2 MiB RAM mirrors, so retain both while sorting by the former. Gather
		// the bounded zero-to-two edges and merge them into the sorted table in
		// one backwards pass instead of shifting the table once per edge.
		std::array<IncomingLinkRecord, DIRECT_LINK_SLOT_COUNT> pending;
		size_t pending_count = 0;
		for (u8 i = 0; i < DIRECT_LINK_SLOT_COUNT; i++)
		{
			const DirectLinkSlot& link = block.direct_links.slots[i];
			const u32 target_lookup_identity = RecLookupIdentity(link.target_pc);
			const u32 target_link_identity = RecLinkIdentity(link.target_pc);
			if (!link.valid || target_lookup_identity == UINT32_MAX ||
				target_link_identity == UINT32_MAX ||
				m_incoming_links.size() + pending_count >= MAX_INCOMING_LINKS)
			{
				continue;
			}
			pending[pending_count++] = {
				&block, target_lookup_identity, target_link_identity, i};
		}
		if (pending_count == 0)
			return;
		if (pending_count == 2 &&
			pending[1].target_lookup_identity <
				pending[0].target_lookup_identity)
		{
			std::swap(pending[0], pending[1]);
		}

		const size_t old_size = m_incoming_links.size();
		size_t old_index = old_size;
		size_t pending_index = pending_count;
		size_t write_index = old_size + pending_count;
		m_incoming_links.resize(write_index);
		while (old_index != 0 && pending_index != 0)
		{
			if (m_incoming_links[old_index - 1].target_lookup_identity >
				pending[pending_index - 1].target_lookup_identity)
			{
				m_incoming_links[--write_index] =
					m_incoming_links[--old_index];
			}
			else
			{
				m_incoming_links[--write_index] =
					pending[--pending_index];
			}
		}
		while (pending_index != 0)
			m_incoming_links[--write_index] = pending[--pending_index];
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
		ClearWaitResumeBlock();
		ClearSchedulerDirectResume();
		ClearSchedulerPredictedResume();
		m_force_logical_continuation = false;
		ClearHotDispatchCache();
		m_free_cache_head = nullptr;
		for (const std::unique_ptr<CachedBlock>& entry : m_cache)
		{
			if (entry->valid)
				invalidated++;

			entry->valid = false;
			entry->queued_free = false;
			entry->next_free = nullptr;
			entry->rec_lookup_identity = UINT32_MAX;
			entry->rec_link_identity = UINT32_MAX;
			entry->logical_continuation = false;
			entry->discovered_topology = false;
			entry->trusted_source = false;
			entry->direct_links = {};
			entry->ClearFragments();
			entry->ClearOpcodes();
			entry->ReleaseOversizedMetadata();
			RememberFreeCacheEntry(*entry);
		}
		for (const std::unique_ptr<InterpreterFallbackBlock>& entry :
			m_interpreter_fallback_blocks)
		{
			if (entry->valid)
				invalidated++;
		}

		ClearBlockRecords();
		ClearIncomingLinks();
		m_semantic_block_descriptors.clear();
		ClearRamSourcePages();
		m_interpreter_fallback_blocks.clear();
		ReleaseLookupPages();
		const u32 previous_resets = m_code_cache_resets;
		// PCSX2 owner: x86/iR3000A.cpp::recResetIOP() rewinds recPtr inside
		// the one recReserve()-owned arena.  Keep the Vita VM slice as well;
		// freeing and reallocating it at ordinary cache pressure both changes
		// that ownership and can fail after VM-domain publication has begun.
		m_code_cache_used = 0;
		bool isolate_variants_enabled = true;
#if defined(VITASX2_QEMU_VALIDATION)
		isolate_variants_enabled = s_qemuIopIsolateCacheSpecializationEnabled;
#endif
		m_active_isolate_cache_mode =
			isolate_variants_enabled && (psxRegs.CP0.n.Status & 0x10000u) != 0;
		m_code_cache_resets = previous_resets;
		return invalidated;
	}

	u32 BlockExecutor::Shutdown()
	{
		const u32 invalidated = Reset();
		ReleaseCodeCache();
		return invalidated;
	}

	void BlockExecutor::InvalidateCachedBlock(CachedBlock& block)
	{
		if (!block.valid)
			return;
		// PCSX2 owner: psxRecClearMem() and BaseBlocks::Remove(). A predecessor
		// ending exactly at this block is not part of the overlapping removal
		// extent. Keep its already-compiled no-test seam and only unlink its tail;
		// the forced dispatcher recompiles this target before continuing.
		if (m_wait_resume_block == &block)
			ClearWaitResumeBlock();
		if (m_scheduler_direct_resume_event_context.block == &block)
			ClearSchedulerDirectResume();
		if (m_scheduler_direct_resume_event_context.predicted_block == &block)
		{
			if (m_scheduler_direct_resume_event_context.predicted_block_second ==
				&block)
			{
				// Exact architectural aliases may occupy both tuples while sharing
				// one recLUT block. Retiring that block must not compact its other
				// alias back into the first way.
				m_scheduler_direct_resume_event_context.predicted_block = nullptr;
				m_scheduler_direct_resume_event_context.predicted_pc = UINT32_MAX;
			}
			else
			{
				m_scheduler_direct_resume_event_context.predicted_block =
					m_scheduler_direct_resume_event_context.predicted_block_second;
				m_scheduler_direct_resume_event_context.predicted_pc =
					m_scheduler_direct_resume_event_context.predicted_pc_second;
			}
			m_scheduler_direct_resume_event_context.predicted_block_second = nullptr;
			m_scheduler_direct_resume_event_context.predicted_pc_second = UINT32_MAX;
		}
		else if (m_scheduler_direct_resume_event_context.predicted_block_second ==
				 &block)
		{
			m_scheduler_direct_resume_event_context.predicted_block_second = nullptr;
			m_scheduler_direct_resume_event_context.predicted_pc_second = UINT32_MAX;
		}
		// RecLookupIdentity strips only segment/RAM-mirror bits above this
		// cache's PC[7:2] index, so every exact architectural alias of this
		// canonical block occupies the same direct-mapped slot.
		const u32 dispatch_cache_index = (block.rec_lookup_identity >> 2) &
		                                 (SchedulerDispatchCacheEntryCount() - 1);
		auto& dispatch_cache_entry = m_scheduler_direct_resume_event_context
		                                 .dispatch_cache[dispatch_cache_index];
		if (dispatch_cache_entry.block == &block)
		{
			dispatch_cache_entry.block = nullptr;
			dispatch_cache_entry.guest_pc = UINT32_MAX;
		}
#if defined(VITASX2_QEMU_VALIDATION)
		for (u32 i = 0; i < m_scheduler_prediction_shadow.size();)
		{
			if (m_scheduler_prediction_shadow[i] != &block)
			{
				i++;
				continue;
			}
			for (u32 j = i + 1; j < m_scheduler_prediction_shadow.size(); j++)
				m_scheduler_prediction_shadow[j - 1] =
					m_scheduler_prediction_shadow[j];
			m_scheduler_prediction_shadow.back() = nullptr;
		}
#endif

		UnregisterRamSource(block);
		UnlinkIncomingLinks(block.rec_link_identity,
			block.isolate_cache_active ? 1 : 0);
		// PCSX2 owner: x86/BaseblockEx.cpp::BaseBlocks::Link()/New() retains
		// every emitted source patch site in its target-keyed link multimap until
		// the complete recompiler arena is reset. Vita releases a source's
		// IncomingLinkRecords when its CachedBlock metadata is retired, so first
		// restore each emitted outgoing site to its dispatcher fallback. This is
		// observable when an SMC helper invalidates the currently executing source
		// and a later victim in the same range is one of its linked targets: the
		// source's A32 tail can still execute after the helper returns, but it must
		// not retain a branch or scheduler-resume pointer to the retired target.
		for (DirectLinkSlot& outgoing_link : block.direct_links.slots)
		{
			if (outgoing_link.valid)
				PatchDirectLink(block, outgoing_link, nullptr);
		}
		UnregisterIncomingLinks(block);
		UnregisterBlockLookup(block);
		UnregisterBlockRecord(block);
		block.valid = false;
		block.rec_lookup_identity = UINT32_MAX;
		block.rec_link_identity = UINT32_MAX;
		block.raw_opcodes = nullptr;
		block.logical_continuation = false;
		block.discovered_topology = false;
		block.trusted_source = false;
		block.ram_source_start = INVALID_RAM_SOURCE;
		block.poll_branch_opcodes = nullptr;
		block.poll_leaf_opcodes = nullptr;
		block.poll_branch_source_start = INVALID_RAM_SOURCE;
		block.poll_leaf_source_start = INVALID_RAM_SOURCE;
		block.poll_call_wait_loop = false;
		block.inline_ram_poll_wait_loop = false;
		block.poll_word_address = 0;
		block.poll_result_register = 0;
		block.poll_load_opcode = 0;
		block.direct_budget_exit = false;
		block.constant_cycle_budget = false;
		block.clock_mode_check_instructions_removed = 0;
		block.saved_register_stack_words_removed = 0;
		block.saved_register_frame_instructions_added = 0;
		block.saved_register_frame_instructions_removed = 0;
		block.batched_cycle_instructions_removed = 0;
		block.batched_cycle_stack_words_removed = 0;
		block.expanded_cycle_batching = false;
		block.saved_registers = 0;
		block.stack_frame_size = 0;
		block.direct_links = {};
		block.ClearFragments();
		block.ClearOpcodes();
		RememberFreeCacheEntry(block);
	}

	bool BlockExecutor::MayInvalidateRange(u32 start_pc,
		u32 instruction_count) const
	{
		if (instruction_count == 0)
			return false;
		if (instruction_count > ((UINT32_MAX - start_pc) / 4))
			return true;

		const u32 physical_start = start_pc & 0x1fffffffu;
		const u32 byte_count = instruction_count * sizeof(u32);
		if (physical_start >= Ps2MemSize::TotalIopRam ||
			byte_count > Ps2MemSize::TotalIopRam - physical_start)
		{
			// ROM and hardware lookup identities retain the existing exact-range
			// invalidation path. The compact chunk map describes writable RAM only.
			return true;
		}

		u32 backing_start = physical_start & (Ps2MemSize::ExposedIopRam - 1);
		u32 remaining = byte_count;
		while (remaining != 0)
		{
			const u32 span =
				std::min(remaining, Ps2MemSize::ExposedIopRam - backing_start);
			const u32 backing_end = backing_start + span;
			for (u32 chunk_index = backing_start >> RAM_SOURCE_CHUNK_SHIFT;
				chunk_index <= ((backing_end - 1) >> RAM_SOURCE_CHUNK_SHIFT);
				chunk_index++)
			{
				if (m_ram_source_chunks->live_flags[chunk_index] != 0)
					return true;
			}
			remaining -= span;
			backing_start = 0;
		}
		return false;
	}

	u32 BlockExecutor::InvalidateRange(u32 start_pc, u32 instruction_count)
	{
		if (instruction_count == 0 ||
			instruction_count > ((UINT32_MAX - start_pc) / 4))
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
				const u32 chunk =
					std::min(remaining, Ps2MemSize::ExposedIopRam - backing_start);
				invalidated += InvalidateRamSourceRange(backing_start, chunk);
				remaining -= chunk;
				backing_start = 0;
			}
			return invalidated;
		}

		const u32 rec_lookup_start = RecLookupIdentity(start_pc);
		if (rec_lookup_start == UINT32_MAX ||
			byte_count > UINT32_MAX - rec_lookup_start)
		{
			return 0;
		}
		const u32 rec_lookup_end = rec_lookup_start + byte_count;
		ForgetSemanticBlockDescriptorsForLookupRange(rec_lookup_start, byte_count);
		u32 invalidated = 0;
		for (const std::unique_ptr<InterpreterFallbackBlock>& block :
			m_interpreter_fallback_blocks)
		{
			if (!block->valid || block->rec_lookup_identity >= rec_lookup_end)
			{
				continue;
			}
			const u32 block_end =
				block->rec_lookup_identity + block->instruction_count * sizeof(u32);
			if (rec_lookup_start < block_end)
			{
				InvalidateInterpreterFallbackBlock(*block);
				invalidated++;
			}
		}
		constexpr u32 max_block_bytes = MAX_LOGICAL_BLOCK_INSTRUCTIONS * 4;
		const u32 first_candidate_pc = rec_lookup_start > max_block_bytes ? rec_lookup_start - max_block_bytes : 0;
		// PCSX2's recLUT aliases select the same translation. Since Vita blocks
		// are bounded and records use that identity, entries before this lower
		// bound cannot overlap the cleared word range.
		u32 i = 0;
		u32 limit = static_cast<u32>(m_block_records.size());
		while (i < limit)
		{
			const u32 mid = (i + limit) >> 1;
			if (m_block_records[mid].rec_lookup_identity < first_candidate_pc)
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

			if (block->rec_lookup_identity >= rec_lookup_end)
				break;

			const u32 block_end =
				block->rec_lookup_identity + block->instruction_count * 4;
			if (rec_lookup_start < block_end)
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

		ClearSchedulerDirectResume();
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
					const u32 rt = RT(op);
					link = rt == 0x10u || rt == 0x11u;
					// PCSX2 owner: rpsxBLTZAL()/rpsxBGEZAL() make r31
					// constant before evaluating Rs. If the branch itself reads
					// r31, its condition therefore observes the new link, not the
					// architectural value on entry.
					const u32 link_value = pc + 8;
					const s32 value = static_cast<s32>(
						link && RS(op) == 31u ? link_value : psxRegs.GPR.r[RS(op)]);
					switch (rt)
					{
						case 0x00:
							taken = value < 0;
							break; // BLTZ
						case 0x01:
							taken = value >= 0;
							break; // BGEZ
						case 0x10:
							taken = value < 0;
							break; // BLTZAL
						case 0x11:
							taken = value >= 0;
							break; // BGEZAL
						default:
							return false;
					}
					target_pc = BranchTarget(pc, op);
					break;
				}
				case 0x02:
					taken = true;
					target_pc = JumpTarget(pc, op);
					break; // J
				case 0x03:
					taken = true;
					link = true;
					target_pc = JumpTarget(pc, op);
					break; // JAL
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

		// RunValidatedBlock calls this only after the ordinary cache lookup and
		// source validation. This is the Cortex-A9 adaptation of PCSX2's generated
		// s_nBlockFF tail: it removes the generated-block call, self-link, and
		// return for an otherwise empty wait loop while retaining the exact owner
		// helper and the existing cache/SMC ownership.
		VitaIopA32FastForwardWaitLoop(start_pc, block_cycles);
#if defined(VITASX2_QEMU_VALIDATION)
		VitaRecordA32IopWaitLoopDispatchElision();
#endif
		return true;
	}

	template <int ClockMode>
	__attribute__((noinline, cold)) bool
	BlockExecutor::TryFastForwardCachedWaitLoopForClock(CachedBlock& block)
	{
		const WaitLoopDescriptor& descriptor = block.wait_loop_descriptor;
		if (descriptor.condition == WaitLoopCondition::Invalid ||
			descriptor.cycles == 0)
			return false;

#if defined(VITASX2_QEMU_VALIDATION)
		m_cached_wait_descriptor_checks++;
		m_cached_wait_descriptor_opcode_reads_removed += descriptor.cycles;
		if (descriptor.condition == WaitLoopCondition::Always)
			m_cached_wait_descriptor_unconditional_checks++;
#endif
		// PCSX2 owner: rpsxBLTZAL()/rpsxBGEZAL() publish the link before
		// evaluating the branch. Preserve that ordering when r31 is also Rs;
		// the not-taken path still enters generated code, which owns the write.
		const u32 link_value = block.start_pc + descriptor.cycles * sizeof(u32);
		const u32 rs_value = descriptor.writes_link && descriptor.rs == 31u ? link_value : psxRegs.GPR.r[descriptor.rs];
		const u32 rt_value = psxRegs.GPR.r[descriptor.rt];
		bool taken = false;
		switch (descriptor.condition)
		{
			case WaitLoopCondition::Always:
				taken = true;
				break;
			case WaitLoopCondition::Equal:
				taken = rs_value == rt_value;
				break;
			case WaitLoopCondition::NotEqual:
				taken = rs_value != rt_value;
				break;
			case WaitLoopCondition::LessThanZero:
				taken = static_cast<s32>(rs_value) < 0;
				break;
			case WaitLoopCondition::GreaterEqualZero:
				taken = static_cast<s32>(rs_value) >= 0;
				break;
			case WaitLoopCondition::LessEqualZero:
				taken = static_cast<s32>(rs_value) <= 0;
				break;
			case WaitLoopCondition::GreaterThanZero:
				taken = static_cast<s32>(rs_value) > 0;
				break;
			case WaitLoopCondition::Invalid:
				return false;
		}

		if (!taken)
			return false;
		if (descriptor.writes_link) [[unlikely]]
			psxRegs.GPR.r[31] = link_value;

		// PCSX2's s_nBlockFF and s_psxBlockCycles are compile-time facts consumed
		// directly by iPsxBranchTest(). The descriptor is protected by the same
		// source/SMC invalidation as the generated block, so no opcode translation
		// or branch decode belongs on this cached-entry path.
		if constexpr (ClockMode == 0)
			FastForwardProviderIopWaitLoopForClock<false>(block.start_pc,
				descriptor.cycles);
		else if constexpr (ClockMode == 1)
			FastForwardProviderIopWaitLoopForClock<true>(block.start_pc,
				descriptor.cycles);
		else
			FastForwardProviderIopWaitLoop(block.start_pc, descriptor.cycles);
#if defined(VITASX2_QEMU_VALIDATION)
		m_cached_wait_descriptor_forwards++;
		VitaRecordA32IopWaitLoopDispatchElision();
#endif
		return true;
	}

	__attribute__((noinline, cold)) bool
	BlockExecutor::TryFastForwardCachedWaitLoop(CachedBlock& block)
	{
		return TryFastForwardCachedWaitLoopForClock<-1>(block);
	}

	inline __attribute__((always_inline)) bool
	BlockExecutor::TryFastForwardCachedUnconditionalWaitLoop(CachedBlock& block)
	{
		const WaitLoopDescriptor& descriptor = block.wait_loop_descriptor;
		if (descriptor.condition != WaitLoopCondition::Always ||
			descriptor.cycles == 0)
			return false;

#if defined(VITASX2_QEMU_VALIDATION)
		m_cached_wait_descriptor_checks++;
		m_cached_wait_descriptor_forwards++;
		m_cached_wait_descriptor_unconditional_checks++;
		m_cached_wait_descriptor_opcode_reads_removed += descriptor.cycles;
#endif
		if (descriptor.writes_link) [[unlikely]]
			psxRegs.GPR.r[31] = block.start_pc + descriptor.cycles * sizeof(u32);
		FastForwardProviderIopWaitLoop(block.start_pc, descriptor.cycles);
#if defined(VITASX2_QEMU_VALIDATION)
		VitaRecordA32IopWaitLoopDispatchElision();
#endif
		return true;
	}

	inline __attribute__((always_inline)) void
	BlockExecutor::FastForwardRetainedUnconditionalWaitLoop(CachedBlock& block)
	{
		// SetWaitResumeBlock() records this kind only after the same immutable,
		// raw-backed descriptor has forwarded once. Source invalidation and every
		// cache reset clear the scheduler context before the block can be reused,
		// so repeating the condition/cycle classification here is redundant.
		const WaitLoopDescriptor& descriptor = block.wait_loop_descriptor;
#if defined(VITASX2_QEMU_VALIDATION)
		m_cached_wait_descriptor_checks++;
		m_cached_wait_descriptor_forwards++;
		m_cached_wait_descriptor_unconditional_checks++;
		m_cached_wait_descriptor_opcode_reads_removed += descriptor.cycles;
#endif
		if (descriptor.writes_link) [[unlikely]]
			psxRegs.GPR.r[31] = block.start_pc + descriptor.cycles * sizeof(u32);
		FastForwardProviderIopWaitLoop(block.start_pc, descriptor.cycles);
#if defined(VITASX2_QEMU_VALIDATION)
		VitaRecordA32IopWaitLoopDispatchElision();
#endif
	}

	template <bool Ps1Clock>
	inline __attribute__((always_inline)) void
	BlockExecutor::FastForwardRetainedUnconditionalWaitLoopForClock(
		CachedBlock& block)
	{
		const WaitLoopDescriptor& descriptor = block.wait_loop_descriptor;
#if defined(VITASX2_QEMU_VALIDATION)
		m_cached_wait_descriptor_checks++;
		m_cached_wait_descriptor_forwards++;
		m_cached_wait_descriptor_unconditional_checks++;
		m_cached_wait_descriptor_opcode_reads_removed += descriptor.cycles;
#endif
		if (descriptor.writes_link) [[unlikely]]
			psxRegs.GPR.r[31] = block.start_pc + descriptor.cycles * sizeof(u32);
		FastForwardProviderIopWaitLoopForClock<Ps1Clock>(block.start_pc,
			descriptor.cycles);
#if defined(VITASX2_QEMU_VALIDATION)
		VitaRecordA32IopWaitLoopDispatchElision();
#endif
	}

	template <bool Ps1Clock>
	inline __attribute__((always_inline)) void
	BlockExecutor::FastForwardRetainedUnconditionalNoLinkWaitLoopForClock(
		CachedBlock& block)
	{
		const WaitLoopDescriptor& descriptor = block.wait_loop_descriptor;
#if defined(VITASX2_QEMU_VALIDATION)
		m_cached_wait_descriptor_checks++;
		m_cached_wait_descriptor_forwards++;
		m_cached_wait_descriptor_unconditional_checks++;
		m_cached_wait_descriptor_opcode_reads_removed += descriptor.cycles;
#endif
		FastForwardProviderIopWaitLoopForClock<Ps1Clock>(block.start_pc,
			descriptor.cycles);
#if defined(VITASX2_QEMU_VALIDATION)
		VitaRecordA32IopWaitLoopDispatchElision();
#endif
	}

	inline __attribute__((always_inline)) bool
	BlockExecutor::TryFastForwardRetainedWaitLoop(CachedBlock& block,
		WaitResumeKind kind)
	{
		if (kind == WaitResumeKind::Unconditional) [[likely]]
		{
			FastForwardRetainedUnconditionalWaitLoop(block);
			return true;
		}
		if (kind == WaitResumeKind::PollCall)
			return TryFastForwardPollCallWaitLoop(block);
		return kind == WaitResumeKind::Conditional &&
		       TryFastForwardCachedWaitLoop(block);
	}

	bool BlockExecutor::ScanStraightLineBlock(u32 start_pc,
		u32 max_instruction_count,
		BlockScanResult* result)
	{
		if (!result || max_instruction_count == 0)
			return false;

		*result = {};
		result->start_pc = start_pc;
		result->stop_pc = start_pc;
		bool isolate_mode_boundary = VitaIsIopPreInstructionTraceEnabled();
#if defined(VITASX2_QEMU_VALIDATION)
		isolate_mode_boundary =
			isolate_mode_boundary && s_qemuIopIsolateCacheSpecializationEnabled;
#endif

		const auto add_instruction = [&](u32 pc) {
			result->instruction_count++;
			result->stop_pc = pc + 4;
		};

		for (u32 i = 0; i < max_instruction_count; i++)
		{
			if (i > ((UINT32_MAX - start_pc) / 4))
				return true;

			const u32 pc = start_pc + i * 4;

			const u32* const op_ptr = ResolveIopRecOpcode(pc);
			if (!op_ptr)
				return result->instruction_count != 0;
			const u32 op = *op_ptr;
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
				const u32* const delay_op_ptr = ResolveIopRecOpcode(delay_pc);
				if (!delay_op_ptr)
					return result->instruction_count != 0;
				const u32 delay_op = *delay_op_ptr;
				if (!BlockCompiler::CanCompileOpcode(delay_op))
					return result->instruction_count != 0;

				// PCSX2 owner: R3000AInterpreter.cpp::psxBNE()/psxJAL()
				// dispatch through doBranch(), whose delay slot belongs to the
				// branch. Keep the pair together even when the delay slot is the
				// first word of the next guest page.
				add_instruction(pc);
				add_instruction(delay_pc);
				if (isolate_mode_boundary && IsIopCop0StatusWriteOpcode(delay_op))
					return true;
				if (VitaIsIopPreInstructionTraceEnabled() &&
					IsIopStaticConditionalBranchOpcode(op) &&
					!IsIopBranchOrJumpOpcode(delay_op) &&
					!IsIopExceptionOpcode(delay_op) && (delay_pc & 0xffcu) != 0)
				{
					// The only available PCSX2 IOP instruction trace is interpreter-
					// owned. Preserve its not-taken fallthrough stream in diagnostic
					// mode; product blocks below follow the x86 recompiler boundary.
					if (BranchTarget(pc, op) != start_pc)
					{
						i++;
						continue;
					}
				}

				// PCSX2 owner: x86/iR3000A.cpp::iopRecRecompile() sets the end
				// of every product branch block to branch PC + 8. Both conditional
				// arms pass through psxSetBranchImm()/iPsxBranchTest().
				return true;
			}

			add_instruction(pc);
			if (isolate_mode_boundary && IsIopCop0StatusWriteOpcode(op))
				return true;
		}

		return true;
	}

	BlockScanStatus
	BlockExecutor::ScanProviderLogicalBlock(u32 start_pc, BlockScanResult* result)
	{
		if (!result || (start_pc & 3u) != 0)
			return BlockScanStatus::InvalidStart;

		// Diagnostic pre-instruction streams deliberately use interpreter-shaped
		// fallthrough windows. Product execution below follows the exact order of
		// PCSX2 iopRecRecompile(): existing BaseBlock lookup precedes opcode fetch,
		// then an interior conditional target can end a no-test prefix before the
		// branch and its delay slot.
		if (VitaIsIopPreInstructionTraceEnabled())
		{
			if (ScanStraightLineBlock(start_pc, MAX_LOGICAL_BLOCK_INSTRUCTIONS,
					result) &&
				result->instruction_count != 0)
			{
				return BlockScanStatus::Success;
			}
			return ResolveIopRecOpcode(start_pc) ? BlockScanStatus::AnalysisCeiling : BlockScanStatus::UnmappedStart;
		}

		*result = {};
		result->start_pc = start_pc;
		result->stop_pc = start_pc;
		const u32 start_identity = RecLookupIdentity(start_pc);
		if (start_identity != UINT32_MAX)
		{
			if (const SemanticBlockDescriptor* descriptor =
					FindSemanticBlockDescriptor(start_identity))
			{
				// A compiled PCSX2 BASEBLOCKEX keeps its first published shape even
				// if another entry is later compiled inside that source extent. Code
				// eviction must recreate that shape instead of rescanning against the
				// newer interior recLUT entry.
				result->instruction_count = descriptor->instruction_count;
				result->stop_pc = start_pc + descriptor->instruction_count * sizeof(u32);
				result->logical_continuation = descriptor->logical_continuation;
				return BlockScanStatus::Success;
			}
		}
		const auto add_instruction = [&](u32 pc) {
			result->instruction_count++;
			result->stop_pc = pc + 4;
		};
		const auto has_discovered_successor = [&](u32 pc) {
			const u32 rec_lookup_identity = RecLookupIdentity(pc);
			if (rec_lookup_identity == UINT32_MAX)
				return false;
			if (FindSemanticBlockDescriptor(rec_lookup_identity))
				return true;
			if (FindInterpreterFallbackBlock(pc))
				return true;
			s32 index = LastBlockRecordIndex(rec_lookup_identity);
			while (index >= 0 &&
				   m_block_records[index].rec_lookup_identity == rec_lookup_identity)
			{
				const CachedBlock* const block = m_block_records[index--].block;
				if (block && block->valid && block->discovered_topology)
					return true;
			}
			return false;
		};

		for (u32 i = 0; i < MAX_LOGICAL_BLOCK_INSTRUCTIONS; i++)
		{
			if (i > ((UINT32_MAX - start_pc) / 4))
				return BlockScanStatus::AnalysisCeiling;
			const u32 pc = start_pc + i * 4;
			if (i != 0 && has_discovered_successor(pc))
			{
				result->logical_continuation = true;
				result->stop_pc = pc;
				return BlockScanStatus::Success;
			}

			const u32* const op_ptr = ResolveIopRecOpcode(pc);
			if (!op_ptr)
				return i == 0 ? BlockScanStatus::UnmappedStart : BlockScanStatus::SourceBoundary;
			const u32 op = *op_ptr;

			if (IsIopBranchOrJumpOpcode(op))
			{
				if (IsIopStaticConditionalBranchOpcode(op))
				{
					const u32 target_pc = BranchTarget(pc, op);
					if (target_pc > start_pc && target_pc < pc)
					{
						result->instruction_count = (target_pc - start_pc) / 4;
						result->stop_pc = target_pc;
						result->logical_continuation = true;
						return BlockScanStatus::Success;
					}
				}

				add_instruction(pc);
				u32 delay_pc = pc;
				for (;;)
				{
					if (result->instruction_count >= MAX_LOGICAL_BLOCK_INSTRUCTIONS ||
						delay_pc > UINT32_MAX - 4)
					{
						return BlockScanStatus::AnalysisCeiling;
					}
					delay_pc += 4;
					const u32* const delay_op_ptr = ResolveIopRecOpcode(delay_pc);
					if (!delay_op_ptr)
						return BlockScanStatus::SourceBoundary;
					const u32 delay_op = *delay_op_ptr;
					add_instruction(delay_pc);
					if (!IsIopBranchOrJumpOpcode(delay_op))
						return BlockScanStatus::Success;

					// Branch handlers recursively compile their sequential delay
					// before setting psxbranch. The deepest nested branch therefore
					// emits the first reachable tail. Do not apply successor or
					// interior-backedge discovery inside this owned recursion.
				}
			}

			add_instruction(pc);
		}

		// The 0xffff analysis ceiling and a mapped-source boundary are not PCSX2
		// scheduling seams. Fall back for the complete candidate rather than
		// publishing an artificial executable prefix.
		return BlockScanStatus::AnalysisCeiling;
	}

	bool BlockExecutor::ReadRecompilerOwnedOpcode(u32 pc, u32* opcode)
	{
		if (!opcode)
			return false;
		const u32* const source = ResolveIopRecOpcode(pc);
		if (!source)
			return false;
		*opcode = *source;
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
		if (block.trusted_source)
		{
			m_trusted_source_hits++;
			if (!s_qemuIopTrustedSourceAuditEnabled)
				return true;
		}
#else
		if (block.trusted_source)
			return true;
#endif
		bool matches = true;
		u32 source_mismatch_pc = UINT32_MAX;
		if (block.raw_opcodes) [[likely]]
		{
#if defined(VITASX2_QEMU_VALIDATION)
			m_raw_validation_calls++;
			for (u32 i = 0; matches && i < block.instruction_count; i++)
			{
				m_validation_words++;
				m_raw_validation_words++;
				m_trusted_source_audit_words++;
				matches = (block.Opcode(i) == block.raw_opcodes[i]);
				if (!matches)
					source_mismatch_pc = block.start_pc + i * sizeof(u32);
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
#else
			return true;
#endif
		}
		else
		{
			for (u32 i = 0; matches && i < block.instruction_count; i++)
			{
				const u32* const opcode = ResolveIopRecOpcode(block.start_pc + i * 4);
#if defined(VITASX2_QEMU_VALIDATION)
				m_validation_words++;
				m_translated_validation_words++;
				if (block.trusted_source)
					m_trusted_source_audit_words++;
#endif
				matches = opcode && block.Opcode(i) == *opcode;
				if (!matches)
					source_mismatch_pc = block.start_pc + i * sizeof(u32);
			}
		}
#if defined(VITASX2_QEMU_VALIDATION)
		if (!matches && block.trusted_source)
			m_trusted_source_audit_failures++;
#endif

		if (matches)
			return true;

		// PCSX2 owner: x86/iR3000A.cpp::psxRecClearMem() invalidates changed
		// translated ranges. Raw RAM sources normally return above under the
		// explicit Vita invalidation contract; this mismatch path remains for the
		// QEMU trust audit and handler-backed/cross-page fallback sources.
		if (source_mismatch_pc != UINT32_MAX)
		{
			InvalidateRange(source_mismatch_pc, 1);
			return false;
		}
		InvalidateCachedBlock(block);
		return false;
	}

	inline __attribute__((always_inline)) bool
	BlockExecutor::TryFastForwardPollCallWaitLoop(CachedBlock& block)
	{
		if (block.inline_ram_poll_wait_loop)
			return TryFastForwardInlineRamPollWaitLoop(block);

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
		FastForwardProviderIopWaitLoop(block.start_pc, poll_loop_cycles);
#if defined(VITASX2_QEMU_VALIDATION)
		VitaRecordA32IopWaitLoopDispatchElision();
		VitaRecordA32IopPollCallWaitLoopDispatchElision();
#endif
		return true;
	}

	template <bool Ps1Clock>
	inline __attribute__((always_inline)) bool
	BlockExecutor::TryFastForwardPollCallWaitLoopForClock(CachedBlock& block)
	{
		if (block.inline_ram_poll_wait_loop)
			return TryFastForwardInlineRamPollWaitLoopForClock<Ps1Clock>(block);

		const u32 value =
			*reinterpret_cast<const u32*>(&iopMem->Main[block.poll_word_address]);
		if (value != 0)
			return false;

		psxRegs.GPR.r[block.poll_result_register] = 0;
		psxRegs.GPR.r[31] = block.start_pc + 2 * sizeof(u32);
		constexpr u32 poll_loop_cycles = 2 + 5 + 2;
		FastForwardProviderIopWaitLoopForClock<Ps1Clock>(block.start_pc,
			poll_loop_cycles);
#if defined(VITASX2_QEMU_VALIDATION)
		VitaRecordA32IopWaitLoopDispatchElision();
		VitaRecordA32IopPollCallWaitLoopDispatchElision();
#endif
		return true;
	}

	inline __attribute__((always_inline)) u32
	BlockExecutor::ReadInlineRamPollValue(const CachedBlock& block)
	{
		const u8* const address = &iopMem->Main[block.poll_word_address];
		switch (block.poll_load_opcode)
		{
			case 0x20: // LB
				return static_cast<u32>(static_cast<s32>(
					static_cast<s8>(*address)));
			case 0x21: // LH
			{
				u16 value = 0;
				std::memcpy(&value, address, sizeof(value));
				return static_cast<u32>(static_cast<s32>(static_cast<s16>(value)));
			}
			case 0x23: // LW
			{
				u32 value = 0;
				std::memcpy(&value, address, sizeof(value));
				return value;
			}
			case 0x24: // LBU
				return *address;
			case 0x25: // LHU
			{
				u16 value = 0;
				std::memcpy(&value, address, sizeof(value));
				return value;
			}
			default:
				// Invalid metadata must take the generated path, never suppress a load.
				return 1;
		}
	}

	inline __attribute__((always_inline)) bool
	BlockExecutor::TryFastForwardInlineRamPollWaitLoop(CachedBlock& block)
	{
		const u32 value = ReadInlineRamPollValue(block);
		if (value != 0)
			return false;

		// LUI and the load deliberately share a destination in the recognized
		// shape. Publishing the loaded zero therefore owns every architectural GPR
		// effect of the taken iteration; both delay slots are proven NOPs.
		psxRegs.GPR.r[block.poll_result_register] = value;
		constexpr u32 poll_loop_cycles = 5;
		FastForwardProviderIopWaitLoop(block.start_pc, poll_loop_cycles);
#if defined(VITASX2_QEMU_VALIDATION)
		VitaRecordA32IopWaitLoopDispatchElision();
#endif
		return true;
	}

	template <bool Ps1Clock>
	inline __attribute__((always_inline)) bool
	BlockExecutor::TryFastForwardInlineRamPollWaitLoopForClock(CachedBlock& block)
	{
		const u32 value = ReadInlineRamPollValue(block);
		if (value != 0)
			return false;

		psxRegs.GPR.r[block.poll_result_register] = value;
		constexpr u32 poll_loop_cycles = 5;
		FastForwardProviderIopWaitLoopForClock<Ps1Clock>(block.start_pc,
			poll_loop_cycles);
#if defined(VITASX2_QEMU_VALIDATION)
		VitaRecordA32IopWaitLoopDispatchElision();
#endif
		return true;
	}

	BlockExecutor::CachedBlock*
	BlockExecutor::FindLookupBlockByStartPc(u32 start_pc,
		bool isolate_cache_active)
	{
		if ((start_pc & 0x3u) != 0)
			return nullptr;

		const u32 rec_lookup_identity = RecLookupIdentity(start_pc);
		LookupPage* page = GetLookupPage(rec_lookup_identity, false);
		return page ? page->blocks[isolate_cache_active ? 1 : 0]
		                          [LookupEntryIndex(rec_lookup_identity)] :
		              nullptr;
	}

	bool BlockExecutor::FindCachedBlock(u32 start_pc, u32 instruction_count,
		CachedBlock** block, bool* lookup_hit)
	{
		if (!block || instruction_count == 0 ||
			instruction_count > MAX_LOGICAL_BLOCK_INSTRUCTIONS ||
			instruction_count > ((UINT32_MAX - start_pc) / 4))
		{
			return false;
		}

		*block = nullptr;
		if (lookup_hit)
			*lookup_hit = false;

		if (CachedBlock* entry =
				FindLookupBlockByStartPc(start_pc, m_active_isolate_cache_mode))
		{
			if (entry->valid && entry->instruction_count == instruction_count &&
				ValidateCachedBlock(*entry))
			{
				*block = entry;
				if (lookup_hit)
					*lookup_hit = true;
				return true;
			}
		}

		if (CachedBlock* entry = FindRecordedBlockByStartPc(
				start_pc, instruction_count, true, m_active_isolate_cache_mode))
		{
			*block = entry;
			return true;
		}

		return false;
	}

	BlockExecutor::CachedBlock*
	BlockExecutor::FindCachedBlockByStartPc(u32 start_pc,
		bool isolate_cache_active)
	{
		if (CachedBlock* entry =
				FindLookupBlockByStartPc(start_pc, isolate_cache_active))
		{
			if (entry->valid && ValidateCachedBlock(*entry))
				return entry;
		}

		return FindRecordedBlockByStartPc(start_pc, 0, false, isolate_cache_active);
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
		// with one ARM code arena for cached R3000A blocks. PES proved that the
		// former 1 MiB slice repeatedly discarded live work while EE still had
		// over 5 MiB of measured headroom, so the preferred exact 22 MiB
		// kuBridge layout assigns IOP 3 MiB and EE 14 MiB. The official VM
		// backend retains the prior 1 MiB IOP slice.
		size_t capacity = IOP_CODE_CACHE_CAPACITY;
#if defined(VITASX2_QEMU_VALIDATION)
		if (s_qemuIopCodeCacheCapacityLimit != 0)
			capacity = std::min(capacity, s_qemuIopCodeCacheCapacityLimit);
#endif
		m_code_cache =
			static_cast<u8*>(VitaVM::AllocLargeJitMemory(capacity));
		if (m_code_cache)
		{
			m_code_cache_capacity = capacity;
		}
		else
		{
			size_t fallback_capacity = IOP_FALLBACK_CODE_CACHE_CAPACITY;
#if defined(VITASX2_QEMU_VALIDATION)
			if (s_qemuIopCodeCacheCapacityLimit != 0)
			{
				fallback_capacity =
					std::min(fallback_capacity,
						s_qemuIopCodeCacheCapacityLimit);
			}
#endif
			m_code_cache = static_cast<u8*>(
				VitaVM::AllocJitMemory(fallback_capacity));
			m_code_cache_capacity =
				m_code_cache ? fallback_capacity : 0;
		}
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

		const size_t aligned_offset =
			AlignUp(m_code_cache_used, CODE_CACHE_ALIGNMENT);
		if (capacity > m_code_cache_capacity ||
			aligned_offset > (m_code_cache_capacity - capacity))
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
		if (slice_offset > m_code_cache_used ||
			code_size > m_code_cache_used - slice_offset)
			return;

		const size_t committed_size = AlignUp(code_size, CODE_CACHE_ALIGNMENT);
		if (committed_size > m_code_cache_used - slice_offset)
			return;

		m_code_cache_used = slice_offset + committed_size;
	}

	u32 BlockExecutor::ResetForCachePressure()
	{
		// PCSX2 couples recLUT's semantic starts to its 32 MiB x86 code arena.
		// Vita's 1 MiB A32 arena recycles much earlier, so retain the semantic
		// descriptors while retiring every physical code/link/dispatch pointer.
		// True Reset() and psxRecClearMem()-owned range invalidation still retire
		// descriptors at their architectural ownership seams.
		std::vector<SemanticBlockDescriptor> semantic_descriptors =
			std::move(m_semantic_block_descriptors);
		const u32 previous_resets = m_code_cache_resets;
		const u32 invalidated = Reset();
		m_semantic_block_descriptors = std::move(semantic_descriptors);
		// Rebuild both ownership levels from the retained semantic descriptors.
		// This rare cache-pressure seam avoids copying a 512 KiB count table onto
		// the Vita's bounded thread stack.
		for (const SemanticBlockDescriptor& descriptor :
			m_semantic_block_descriptors)
		{
			RegisterSemanticRamSource(descriptor);
		}
		m_code_cache_resets = previous_resets + 1;
		return invalidated;
	}

	bool BlockExecutor::CompileIntoCacheEntry(CachedBlock& block, u32 start_pc,
		u32 instruction_count,
		bool entry_effects_already_applied,
		bool allow_cache_pressure_retry,
		bool logical_continuation,
		bool discovered_topology)
	{
		const auto reject_unowned_entry = [&]() {
			if (!block.valid)
				RememberFreeCacheEntry(block);
			return false;
		};
		if (instruction_count == 0 ||
			instruction_count > MAX_LOGICAL_BLOCK_INSTRUCTIONS ||
			instruction_count > ((UINT32_MAX - start_pc) / 4))
		{
			return reject_unowned_entry();
		}
		const u32 rec_lookup_identity = RecLookupIdentity(start_pc);
		const u32 rec_link_identity = RecLinkIdentity(start_pc);
		if (rec_lookup_identity == UINT32_MAX || rec_link_identity == UINT32_MAX)
			return reject_unowned_entry();
		const u32 source_bytes = instruction_count * sizeof(u32);
		u32 executable_bytes = 0;
		if (!GetIopRecExecutableSpan(start_pc, &executable_bytes) ||
			source_bytes > executable_bytes)
		{
			// PCSX2's psxRecLUT sends every handler-backed/unmapped instruction
			// source to iopUnmappedRecLUTPage. Never decode it speculatively or
			// publish an unowned block which a warm link could execute after a write.
			return reject_unowned_entry();
		}

		// This is the one genuine cache-miss compilation seam. Keep PCSX2's
		// iopRecRecompile() entry effects outside code-generation retries and off
		// every cache-hit/direct-link path.
		if (!entry_effects_already_applied)
			ApplyIopRecompilerEntrySideEffects(start_pc);
		InvalidateCachedBlock(block);
		bool isolate_variants_enabled = true;
#if defined(VITASX2_QEMU_VALIDATION)
		isolate_variants_enabled = s_qemuIopIsolateCacheSpecializationEnabled;
#endif
		block.isolate_cache_active =
			isolate_variants_enabled && m_active_isolate_cache_mode;
		block.logical_continuation = logical_continuation;
		block.discovered_topology = discovered_topology;
		block.trusted_source = true;
		block.PrepareOpcodes(instruction_count);
		for (u32 i = 0; i < instruction_count; i++)
		{
			const u32 op = iopMemRead32(start_pc + i * 4);
			if (!BlockCompiler::CanCompileOpcode(op))
			{
				block.ClearOpcodes();
				RememberFreeCacheEntry(block);
				return false;
			}

			block.Opcode(i) = op;
		}
		block.poll_call_wait_loop = false;
		block.inline_ram_poll_wait_loop = false;
		block.poll_word_address = 0;
		block.poll_result_register = 0;
		block.poll_load_opcode = 0;
		block.direct_budget_exit = false;
		block.constant_cycle_budget = false;
		block.poll_branch_opcodes = nullptr;
		block.poll_leaf_opcodes = nullptr;
		block.poll_branch_source_start = INVALID_RAM_SOURCE;
		block.poll_leaf_source_start = INVALID_RAM_SOURCE;
		block.wait_loop_descriptor = {};
		block.wait_loop_shape =
			AnalyzePollCallWaitLoop(block, start_pc, instruction_count);
		if (!block.wait_loop_shape)
		{
			block.wait_loop_shape = AnalyzeInlineRamPollWaitLoop(
				block, start_pc, instruction_count);
		}
		if (!block.wait_loop_shape)
		{
			block.wait_loop_shape = AnalyzeIopWaitLoopShape(
				start_pc, instruction_count, &block.wait_loop_descriptor);
		}
		block.wait_loop_enabled_at_compile =
			EmuConfig.Speedhacks.WaitLoop && !VitaIsIopPreInstructionTraceEnabled();

		const size_t logical_code_slice_offset = m_code_cache_used;
		u32 native_instruction_count = 0;
		u32 helper_instruction_count = 0;
		bool compiled_ps1_bios_gate = false;
		u32 pinned_gpr_memory_ops_saved = 0;
		u32 pinned_branch_operand_moves_removed = 0;
		u32 condition_code_branch_instructions_removed = 0;
		u32 producer_branch_compare_instructions_removed = 0;
		u32 fused_ram_guard_instructions_removed = 0;
		u32 source_page_guard_instructions_removed = 0;
		u32 source_page_literal_instructions_removed = 0;
		u32 isolate_cache_guard_instructions_removed = 0;
		u32 clock_mode_check_instructions_removed = 0;
		u32 saved_register_stack_words_removed = 0;
		u32 saved_register_frame_instructions_added = 0;
		u32 saved_register_frame_instructions_removed = 0;
		u32 batched_cycle_instructions_removed = 0;
		u32 batched_cycle_stack_words_removed = 0;
		bool expanded_cycle_batching = false;
		u16 saved_registers = 0;
		u8 stack_frame_size = 0;
		bool direct_budget_exit = false;
		bool constant_cycle_budget = true;
		DirectLinkSlots direct_links;
		std::vector<DirectLinkSlot> continuation_links;
		const u32 reserved_fragment_count =
			(instruction_count + MAX_STRAIGHT_LINE_BLOCK_INSTRUCTIONS - 1) /
			MAX_STRAIGHT_LINE_BLOCK_INSTRUCTIONS;
		const bool fragmented_logical_block = reserved_fragment_count > 1;
		const u32 logical_cycle_total =
			IopCompilerBlockCycles(start_pc, instruction_count, false);
		block.ClearFragments();
		block.ReserveFragments(reserved_fragment_count);
		continuation_links.reserve(
			reserved_fragment_count > 0 ? reserved_fragment_count - 1 : 0);
		bool logical_writes_isolate_mode = false;

		const auto abandon_compilation = [&]() {
			block.ClearFragments();
			block.direct_links = {};
			block.ClearOpcodes();
			RewindCodeCache(logical_code_slice_offset);
			RememberFreeCacheEntry(block);
			return false;
		};

		for (u32 fragment_index = 0, compiled_instructions = 0,
				 logical_cycle_prefix = 0;
			 compiled_instructions < instruction_count; fragment_index++)
		{
			u32 fragment_instruction_count =
				std::min(MAX_STRAIGHT_LINE_BLOCK_INSTRUCTIONS,
					instruction_count - compiled_instructions);
			const u32 fragment_start_pc = start_pc + compiled_instructions * 4;
			while (fragment_instruction_count != 0 &&
				compiled_instructions + fragment_instruction_count <
					instruction_count &&
				IsIopBranchOrJumpOpcode(
					iopMemRead32(fragment_start_pc +
								 (fragment_instruction_count - 1) * sizeof(u32))))
			{
				// A MIPS branch and its delay slot are one indivisible execution
				// unit. Move an artificial A32 boundary behind the complete nested
				// branch chain rather than splitting any owner/delay pair at logical
				// lengths 65, 129, ... .
				fragment_instruction_count--;
			}
			if (fragment_instruction_count == 0)
			{
				// A branch chain longer than one physical A32 fragment remains one
				// PCSX2 BaseBlock and is handled by the exact retained fallback.
				return abandon_compilation();
			}
			const bool continuation_fragment =
				compiled_instructions + fragment_instruction_count < instruction_count;
			CachedBlock::CodeFragment& fragment = block.AddFragment();
			fragment.start_pc = fragment_start_pc;
			fragment.instruction_count = fragment_instruction_count;

			size_t block_code_capacity = STRAIGHT_LINE_BLOCK_CODE_CAPACITY;
		bool source_page_literal_allowed = true;
			bool fragment_compiled = false;
		for (;;)
		{
			size_t code_slice_offset = 0;
				u8* code_slice =
					AllocateCodeSlice(block_code_capacity, &code_slice_offset);
			if (!code_slice)
			{
					const bool retry_from_clean_cache =
						allow_cache_pressure_retry && logical_code_slice_offset != 0;
					abandon_compilation();
					if (!retry_from_clean_cache)
					return false;

					// PCSX2 recResetIOP() retries the cache-miss compilation after
					// reclaiming its arena. Restart the complete logical transaction;
					// no provisional fragment or entry effect may survive the reset.
					ResetForCachePressure();
					RemoveFreeCacheEntry(block);
					if (discovered_topology)
					{
						BlockScanResult retry_scan;
						if (ScanProviderLogicalBlock(start_pc, &retry_scan) !=
								BlockScanStatus::Success ||
							retry_scan.instruction_count == 0)
			{
							RememberFreeCacheEntry(block);
				return false;
			}
						return CompileIntoCacheEntry(
							block, start_pc, retry_scan.instruction_count, true, false,
							retry_scan.logical_continuation, true);
					}
					return CompileIntoCacheEntry(block, start_pc, instruction_count, true,
						false, logical_continuation,
						discovered_topology);
				}

				if (!fragment.code.Attach(code_slice, block_code_capacity))
					return abandon_compilation();

				BlockCompiler compiler(
					fragment.code, m_ram_source_page_live_counts.data(),
					m_ram_source_page_live_flags.data(),
					m_ram_source_chunks->live_flags.data(),
					source_page_literal_allowed);
			DirectLinkSlots attempt_direct_links;
			size_t attempt_linked_entry_offset = 0;
			size_t attempt_provider_entry_offset = 0;
			ResidentFragmentContract attempt_resident_contract{};
				const bool compiled = compiler.CompileStraightLineBlock(
					fragment_start_pc, fragment_instruction_count,
					reinterpret_cast<const void*>(&VitaIopA32DirectExit),
					&attempt_direct_links, &attempt_linked_entry_offset,
					&attempt_provider_entry_offset,
					!continuation_fragment && !logical_continuation,
					fragment_index == 0,
					logical_writes_isolate_mode, logical_cycle_prefix,
					logical_cycle_total, fragmented_logical_block,
					logical_continuation && !continuation_fragment,
					&attempt_resident_contract);
				const bool out_of_block_space =
					!compiled && fragment.code.Size() >= fragment.code.Capacity();
				const bool source_page_literal_out_of_range =
					compiler.SourcePageLiteralOutOfRange();
				if (compiled)
				{
					// A VM-domain write lease is process-global on PSP2.  Publish
					// this physical fragment before the next fragment opens its
					// own CodeBuffer; otherwise a >64-opcode PCSX2 logical block
					// attempts a nested sceKernelOpenVMDomain() on fragment two.
					if (!fragment.code.Flush())
						return abandon_compilation();
					CommitCodeSlice(code_slice_offset, fragment.code.Size());
					fragment.linked_entry_offset = attempt_linked_entry_offset;
					fragment.provider_entry_offset = attempt_provider_entry_offset;
					fragment.resident = attempt_resident_contract;
					native_instruction_count += compiler.NativeInstructionCount();
					helper_instruction_count += compiler.HelperInstructionCount();
					compiled_ps1_bios_gate =
						compiled_ps1_bios_gate || compiler.UsesCompiledPs1BiosGate();
					pinned_gpr_memory_ops_saved += compiler.PinnedGprMemoryOpsSaved();
					pinned_branch_operand_moves_removed +=
					compiler.PinnedBranchOperandMovesRemoved();
					condition_code_branch_instructions_removed +=
					compiler.ConditionCodeBranchInstructionsRemoved();
					producer_branch_compare_instructions_removed +=
					compiler.ProducerBranchCompareInstructionsRemoved();
					fused_ram_guard_instructions_removed +=
					compiler.FusedRamGuardInstructionsRemoved();
					source_page_guard_instructions_removed +=
					compiler.SourcePageGuardInstructionsRemoved();
					source_page_literal_instructions_removed +=
					compiler.SourcePageLiteralInstructionsRemoved();
					isolate_cache_guard_instructions_removed +=
					compiler.IsolateCacheGuardInstructionsRemoved();
					clock_mode_check_instructions_removed +=
						compiler.ClockModeCheckInstructionsRemoved();
					saved_register_stack_words_removed +=
						compiler.SavedRegisterStackWordsRemoved();
					saved_register_frame_instructions_added +=
						compiler.SavedRegisterFrameInstructionsAdded();
					saved_register_frame_instructions_removed +=
						compiler.SavedRegisterFrameInstructionsRemoved();
					batched_cycle_instructions_removed +=
						compiler.BatchedCycleInstructionsRemoved();
					batched_cycle_stack_words_removed +=
						compiler.BatchedCycleStackWordsRemoved();
					expanded_cycle_batching =
						expanded_cycle_batching || compiler.UsesExpandedCycleBatching();
					saved_registers |= compiler.SavedRegisters();
					stack_frame_size =
						std::max(stack_frame_size, compiler.StackFrameSize());
					direct_budget_exit =
						direct_budget_exit || compiler.UsesDirectBudgetExit();
					constant_cycle_budget =
						constant_cycle_budget && compiler.UsesConstantCycleBudget();
					logical_writes_isolate_mode = compiler.WritesIsolateMode();
					if (continuation_fragment)
					{
						DirectLinkSlot continuation = attempt_direct_links.slots[0];
						if (!continuation.valid ||
							continuation.target_pc !=
								fragment_start_pc + fragment_instruction_count * 4)
						{
							return abandon_compilation();
						}
						continuation.fragment_index = static_cast<u16>(fragment_index);
						continuation_links.push_back(continuation);
					}
					else
					{
				direct_links = attempt_direct_links;
						for (DirectLinkSlot& link : direct_links.slots)
							link.fragment_index = static_cast<u16>(fragment_index);
					}
					fragment_compiled = true;
				break;
			}

				fragment.code.Release();
			RewindCodeCache(code_slice_offset);
			if (source_page_literal_out_of_range && source_page_literal_allowed)
			{
				source_page_literal_allowed = false;
				continue;
			}
				if (!out_of_block_space ||
					block_code_capacity >= MAX_STRAIGHT_LINE_BLOCK_CODE_CAPACITY)
				{
					return abandon_compilation();
				}

			block_code_capacity *= 2;
		}

			if (!fragment_compiled)
				return abandon_compilation();
			logical_cycle_prefix += IopCompilerBlockCycles(
				fragment_start_pc, fragment_instruction_count, false);
			compiled_instructions += fragment_instruction_count;
		}

		for (u32 i = 0; i < continuation_links.size(); i++)
		{
			CachedBlock::CodeFragment& source = block.Fragment(i);
			const CachedBlock::CodeFragment& target = block.Fragment(i + 1);
			const DirectLinkSlot& link = continuation_links[i];
			const void* const target_entry =
				static_cast<const u8*>(target.code.EntryPoint()) +
				target.provider_entry_offset;
			if (!source.code.PatchBranchToAddress(link.target_offset, target_entry))
			{
				return abandon_compilation();
			}
			// The source was already published to release the process-global
			// write lease before compiling its successor.  Republish only the
			// patched continuation word now that the target address is known.
			if (!source.code.Flush())
				return abandon_compilation();
		}

		block.start_pc = start_pc;
		block.rec_lookup_identity = rec_lookup_identity;
		block.rec_link_identity = rec_link_identity;
		block.instruction_count = instruction_count;
		block.raw_opcodes = ResolveRawOpcodeSpan(start_pc, instruction_count,
			&block.ram_source_start);
		block.native_instruction_count = native_instruction_count;
		block.helper_instruction_count = helper_instruction_count;
		block.compiled_ps1_bios_gate = compiled_ps1_bios_gate;
#if defined(VITASX2_QEMU_VALIDATION)
		if (compiled_ps1_bios_gate)
			m_compiled_ps1_bios_gate_blocks++;
#endif
		block.pinned_gpr_memory_ops_saved = pinned_gpr_memory_ops_saved;
		block.pinned_branch_operand_moves_removed =
			pinned_branch_operand_moves_removed;
		block.condition_code_branch_instructions_removed =
			condition_code_branch_instructions_removed;
		block.producer_branch_compare_instructions_removed =
			producer_branch_compare_instructions_removed;
		block.fused_ram_guard_instructions_removed =
			fused_ram_guard_instructions_removed;
		block.source_page_guard_instructions_removed =
			source_page_guard_instructions_removed;
		block.source_page_literal_instructions_removed =
			source_page_literal_instructions_removed;
		block.isolate_cache_guard_instructions_removed =
			isolate_cache_guard_instructions_removed;
		block.clock_mode_check_instructions_removed =
			clock_mode_check_instructions_removed;
		// Per-fragment frame counterfactuals are not additive: the private
		// dispatcher owns one frame for the complete logical block. Until this
		// diagnostic is derived from the union signature, underclaim rather than
		// report fictitious frame removals at internal continuations.
		block.saved_register_stack_words_removed =
			fragmented_logical_block ? 0 : saved_register_stack_words_removed;
		block.saved_register_frame_instructions_added =
			fragmented_logical_block ? 0 : saved_register_frame_instructions_added;
		block.saved_register_frame_instructions_removed =
			fragmented_logical_block ? 0 : saved_register_frame_instructions_removed;
		block.batched_cycle_instructions_removed = batched_cycle_instructions_removed;
		block.batched_cycle_stack_words_removed = batched_cycle_stack_words_removed;
		block.expanded_cycle_batching = expanded_cycle_batching;
		block.saved_registers = saved_registers;
		block.stack_frame_size = stack_frame_size;
		block.direct_budget_exit = direct_budget_exit;
		block.constant_cycle_budget = constant_cycle_budget;
		block.direct_links = direct_links;
		block.logical_continuation = logical_continuation;
		block.discovered_topology = discovered_topology;
		block.trusted_source = true;
		block.valid = true;
		if (!RegisterBlockRecord(block))
		{
			block.valid = false;
			block.direct_links = {};
			block.ClearFragments();
			block.ClearOpcodes();
			RewindCodeCache(logical_code_slice_offset);
			RememberFreeCacheEntry(block);
			return false;
		}
		RegisterBlockLookup(block);
		RegisterRamSource(block);
		if (block.discovered_topology)
		{
			RememberSemanticBlockDescriptor(block.rec_lookup_identity,
				block.ram_source_start, block.instruction_count,
				block.logical_continuation);
		}
		RegisterIncomingLinks(block);

		if (m_direct_linking_enabled)
		{
			PatchIncomingLinks(block);
			for (DirectLinkSlot& link : block.direct_links.slots)
			{
				if (link.valid)
				{
					if (CachedBlock* target = FindCachedBlockByStartPc(
							link.target_pc, block.isolate_cache_active))
						PatchDirectLink(block, link, target);
				}
			}
		}
		VitaPerformanceTelemetry::RegisterIopGeneratedBlockCode(
			block.start_pc, block.opcodes.data(), block.instruction_count);

		return true;
	}

	size_t BlockExecutor::TotalCodeSize(const CachedBlock& block)
	{
		size_t total = 0;
		for (u32 i = 0; i < block.fragment_count; i++)
			total += block.Fragment(i).code.Size();
		return total;
	}

	size_t BlockExecutor::TotalCodeCacheFootprint(const CachedBlock& block)
	{
		size_t total = 0;
		for (u32 i = 0; i < block.fragment_count; i++)
			total += AlignUp(block.Fragment(i).code.Size(), CODE_CACHE_ALIGNMENT);
		return total;
	}

	VitaA32::CodeBuffer* BlockExecutor::DirectLinkCode(CachedBlock& block,
		const DirectLinkSlot& link)
	{
		return link.fragment_index < block.fragment_count ? &block.Fragment(link.fragment_index).code : nullptr;
	}

	const void* BlockExecutor::LinkedEntryPoint(const CachedBlock& block) const
	{
		if (!block.HasFragments())
			return nullptr;
		const CachedBlock::CodeFragment& fragment = block.Fragment(0);
		if (!fragment.code.EntryPoint() ||
			fragment.linked_entry_offset >= fragment.code.Size())
		{
			return fragment.code.EntryPoint();
		}

		return static_cast<const u8*>(fragment.code.EntryPoint()) +
		       fragment.linked_entry_offset;
	}

	const void* BlockExecutor::ResidentEntryPoint(const CachedBlock& block) const
	{
		if (!block.HasFragments())
			return nullptr;
		const CachedBlock::CodeFragment& fragment = block.Fragment(0);
		if (!fragment.code.EntryPoint() ||
			fragment.resident.base_setup_instruction_count == 0 ||
			fragment.resident.base_entry_offset >= fragment.code.Size())
		{
			return nullptr;
		}

		return static_cast<const u8*>(fragment.code.EntryPoint()) +
		       fragment.resident.base_entry_offset;
	}

	const void* BlockExecutor::ResidentGprEntryPoint(
		const CachedBlock& block) const
	{
		if (!block.HasFragments())
			return nullptr;
		const CachedBlock::CodeFragment& fragment = block.Fragment(0);
		if (!fragment.code.EntryPoint() ||
			fragment.resident.gpr_entry.domain == VitaRegion::GuestDomain::None ||
			fragment.resident.gpr_entry_offset >= fragment.code.Size())
		{
			return nullptr;
		}

		return static_cast<const u8*>(fragment.code.EntryPoint()) +
		       fragment.resident.gpr_entry_offset;
	}

	const void* BlockExecutor::ProviderEntryPoint(const CachedBlock& block) const
	{
		if (!block.HasFragments())
			return nullptr;
		const CachedBlock::CodeFragment& fragment = block.Fragment(0);
		if (!fragment.code.EntryPoint() ||
			fragment.provider_entry_offset >= fragment.code.Size())
		{
			return fragment.code.EntryPoint();
		}

		return static_cast<const u8*>(fragment.code.EntryPoint()) +
		       fragment.provider_entry_offset;
	}

	bool BlockExecutor::PatchDirectLink(CachedBlock& block, DirectLinkSlot& link,
		CachedBlock* target)
	{
		if (!block.valid || !link.valid ||
			link.target_offset == static_cast<size_t>(-1) ||
			link.fallback_offset == static_cast<size_t>(-1) ||
			(!link.logical_continuation &&
				link.scheduler_resume_offset == static_cast<size_t>(-1)))
		{
			return false;
		}
		if (target && block.isolate_cache_active != target->isolate_cache_active)
			target = nullptr;
		if (target &&
			(target->rec_link_identity == UINT32_MAX ||
				RecLinkIdentity(link.target_pc) != target->rec_link_identity))
		{
			target = nullptr;
		}
		if (target && block.discovered_topology && !target->discovered_topology)
			target = nullptr;
		// An enabled cached wait descriptor owns its dispatcher entry. A warm link
		// would bypass PCSX2's s_nBlockFF fast-forward decision. Shape recognition
		// alone is not enough: with WaitLoop disabled this is an ordinary generated
		// block and must retain normal direct-link/scheduler-resume behavior.
		if (target && target->wait_loop_shape && target->wait_loop_enabled_at_compile)
			target = nullptr;

		bool use_chain = target != nullptr;
#if defined(VITASX2_QEMU_VALIDATION)
		use_chain = use_chain && s_qemuIopLinkedFrameBypassEnabled;
#endif
		VitaA32::CodeBuffer* const link_code = DirectLinkCode(block, link);
		if (!link_code)
			return false;
		bool use_resident_entry = false;
		bool use_resident_gpr_entry = false;
		const void* target_entry = nullptr;
		if (use_chain)
		{
			const CachedBlock::CodeFragment& source_fragment =
				block.Fragment(link.fragment_index);
			const CachedBlock::CodeFragment& target_fragment =
				target->Fragment(0);
			// Entry contracts describe what a body may consume, not what every
			// path through that body preserves. Mutable residents such as the
			// published event countdown must be admitted from the source's
			// proven normal-exit contract.
			use_resident_entry =
				source_fragment.resident.exit.Provides(
					target_fragment.resident.base_entry) &&
				ResidentEntryPoint(*target) != nullptr;
			use_resident_gpr_entry =
				source_fragment.resident.exit.Provides(
					target_fragment.resident.gpr_entry) &&
				ResidentGprEntryPoint(*target) != nullptr;
			// The published event horizon is mutable cross-block state, while
			// the current GPR bypass rewrites an earlier canonical guest-store
			// seam after the ordinary target branch has already been selected.
			// Keep the proven base/countdown entry, but do not compose that
			// mutation with pinned-GPR store/load elision until the combined
			// exit proof has its own adversarial retail-oracle gate. This is
			// deliberately scoped to countdown contracts; existing immutable
			// base/GPR links remain unchanged.
			if (target_fragment.resident.gpr_entry.Contains(
					VitaRegion::ResidentValue::IopPublishedEventCountdown) ||
				target_fragment.resident.gpr_entry.Contains(
					VitaRegion::ResidentValue::IopEeBudget))
			{
				use_resident_gpr_entry = false;
			}
			for (unsigned host = 0;
				use_resident_gpr_entry &&
					host < VitaRegion::EntryContract::HostRegisterCount;
				host++)
			{
				if ((source_fragment.resident.dirty_gpr_host_mask &
						(1u << host)) != 0 &&
					!(source_fragment.resident.exit.host_values[host] ==
						target_fragment.resident.gpr_entry.host_values[host]))
				{
					use_resident_gpr_entry = false;
				}
			}
#if defined(VITASX2_IOP_RESIDENT_PRELUDE_CONTROL)
			use_resident_entry = false;
			use_resident_gpr_entry = false;
#endif
#if defined(VITASX2_IOP_RESIDENT_GPR_CONTROL)
			use_resident_gpr_entry = false;
#endif
#if defined(VITASX2_QEMU_VALIDATION)
			use_resident_entry =
				use_resident_entry && s_qemuIopResidentPreludeLinksEnabled;
			use_resident_gpr_entry = use_resident_gpr_entry &&
				s_qemuIopResidentPreludeLinksEnabled &&
				s_qemuIopResidentGprLinksEnabled;
#endif
			const bool bypasses_source_stores =
				link.resident_gpr_bypass_offset != static_cast<size_t>(-1);
			target_entry = use_resident_gpr_entry && !bypasses_source_stores ?
				ResidentGprEntryPoint(*target) : (use_resident_entry ?
				ResidentEntryPoint(*target) : LinkedEntryPoint(*target));
		}
		const bool has_gpr_bypass =
			link.resident_gpr_bypass_offset != static_cast<size_t>(-1);
		const bool bypass_restored = !has_gpr_bypass ||
			link_code->PatchInstruction(link.resident_gpr_bypass_offset,
				link.resident_gpr_bypass_instruction);
		const bool has_budget_bypass =
			link.resident_budget_bypass_offset != static_cast<size_t>(-1);
		const bool budget_bypass_restored = !has_budget_bypass ||
			link_code->PatchInstruction(link.resident_budget_bypass_offset,
				link.resident_budget_bypass_instruction);
		const bool target_patched =
			use_chain ? link_code->PatchBranchToAddress(link.target_offset,
							target_entry) :
						link_code->PatchBranch(link.target_offset, link.fallback_offset);
		const bool gpr_bypass_patched =
			!use_resident_gpr_entry || !has_gpr_bypass ||
			link_code->PatchBranchToAddress(link.resident_gpr_bypass_offset,
				ResidentGprEntryPoint(*target));
		const bool budget_bypass_patched =
			!(use_resident_entry || use_resident_gpr_entry) ||
			!has_budget_bypass ||
			link_code->PatchBranchToAddress(link.resident_budget_bypass_offset,
				use_resident_gpr_entry ? ResidentGprEntryPoint(*target) :
					ResidentEntryPoint(*target));
		link.resident_entry_active =
			use_resident_entry || use_resident_gpr_entry;
		link.resident_gpr_entry_active = use_resident_gpr_entry;
		link.resident_ee_budget_entry_active =
			(use_resident_entry || use_resident_gpr_entry) &&
			has_budget_bypass &&
			target->Fragment(0).resident.base_entry.Contains(
				VitaRegion::ResidentValue::IopEeBudget);
		link.resident_setup_instructions_removed =
			(use_resident_entry || use_resident_gpr_entry) ?
				target->Fragment(0).resident.base_setup_instruction_count :
				0;
		link.resident_gpr_loads_removed =
			use_resident_gpr_entry ?
				target->Fragment(0).resident.gpr_entry_load_instruction_count :
				0;
		if (link.logical_continuation)
			return bypass_restored && budget_bypass_restored &&
				target_patched && gpr_bypass_patched &&
				budget_bypass_patched && link_code->Flush();

		const u32 scheduler_resume =
			use_chain ? (static_cast<u32>(reinterpret_cast<uptr>(target)) |
				SCHEDULER_DIRECT_RESUME_TAG) :
			static_cast<u32>(BlockExitKind::Direct);
		if (!bypass_restored || !budget_bypass_restored || !target_patched ||
			!gpr_bypass_patched || !budget_bypass_patched ||
			!link_code->PatchMovImm32(link.scheduler_resume_offset, HOST_TMP0,
				scheduler_resume) ||
			!link_code->Flush())
		{
			return false;
		}

		return true;
	}

	void BlockExecutor::PatchIncomingLinks(CachedBlock& target)
	{
		if (!m_direct_linking_enabled || !target.valid)
			return;

		s32 index = LastIncomingLinkIndex(target.rec_lookup_identity);
		while (index >= 0 && m_incoming_links[index].target_lookup_identity ==
								 target.rec_lookup_identity)
		{
			IncomingLinkRecord& record = m_incoming_links[index--];
			if (record.target_link_identity == target.rec_link_identity &&
				record.source &&
				record.source->isolate_cache_active == target.isolate_cache_active)
			{
				if (DirectLinkSlot* link = GetRecordedDirectLink(record))
					PatchDirectLink(*record.source, *link, &target);
			}
		}
	}

	void BlockExecutor::UnlinkIncomingLinks(u32 target_link_identity,
		int isolate_cache_mode)
	{
		if (target_link_identity == UINT32_MAX)
		{
			for (u32 i = 0; i < m_incoming_links.size(); i++)
			{
				IncomingLinkRecord& record = m_incoming_links[i];
				if (DirectLinkSlot* link = GetRecordedDirectLink(record))
					PatchDirectLink(*record.source, *link, nullptr);
			}
			return;
		}

		const u32 target_lookup_identity = RecLookupIdentity(target_link_identity);
		if (target_lookup_identity == UINT32_MAX)
			return;
		s32 index = LastIncomingLinkIndex(target_lookup_identity);
		while (index >= 0 && m_incoming_links[index].target_lookup_identity ==
								 target_lookup_identity)
		{
			IncomingLinkRecord& record = m_incoming_links[index--];
			if (record.target_link_identity != target_link_identity)
				continue;
			if (isolate_cache_mode >= 0 && record.source &&
				static_cast<int>(record.source->isolate_cache_active) !=
					isolate_cache_mode)
			{
				continue;
			}
			if (DirectLinkSlot* link = GetRecordedDirectLink(record))
				PatchDirectLink(*record.source, *link, nullptr);
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

			CachedBlock* target = FindCachedBlockByStartPc(
				record.target_lookup_identity, record.source->isolate_cache_active);
			if (target && target->rec_link_identity != record.target_link_identity)
				target = nullptr;
			PatchDirectLink(*record.source, *link, target);
		}
	}

	void BlockExecutor::PublishExecutionDetails(
		const CachedBlock& block, BlockExecutionResult* result) const
	{
		result->instruction_count = block.instruction_count;
		result->native_instruction_count = block.native_instruction_count;
		result->helper_instruction_count = block.helper_instruction_count;
		result->code_size = TotalCodeSize(block);
		result->code_cache_footprint = TotalCodeCacheFootprint(block);
		result->physical_fragment_count = block.fragment_count;
		result->block_records = static_cast<u32>(m_block_records.size());
		result->link_records = static_cast<u32>(m_incoming_links.size());
		result->cache_slots = static_cast<u32>(m_cache.size());
		result->code_cache_resets = m_code_cache_resets;
		result->code_cache_used = m_code_cache_used;
		result->code_cache_capacity = m_code_cache_capacity;
#if defined(VITASX2_QEMU_VALIDATION)
		result->pinned_gpr_memory_ops_saved = block.pinned_gpr_memory_ops_saved;
		result->pinned_branch_operand_moves_removed =
			block.pinned_branch_operand_moves_removed;
		result->condition_code_branch_instructions_removed =
			block.condition_code_branch_instructions_removed;
		result->producer_branch_compare_instructions_removed =
			block.producer_branch_compare_instructions_removed;
		result->fused_ram_guard_instructions_removed =
			block.fused_ram_guard_instructions_removed;
		result->source_page_guard_instructions_removed =
			block.source_page_guard_instructions_removed;
		result->source_page_literal_instructions_removed =
			block.source_page_literal_instructions_removed;
		result->isolate_cache_guard_instructions_removed =
			block.isolate_cache_guard_instructions_removed;
		SnapshotInstrumentation(result);
#endif
	}

#if defined(VITASX2_QEMU_VALIDATION)
	void BlockExecutor::SnapshotInstrumentation(
		BlockExecutionResult* result) const
	{
		if (!result)
			return;

		result->block_records = static_cast<u32>(m_block_records.size());
		result->link_records = static_cast<u32>(m_incoming_links.size());
		result->cache_slots = static_cast<u32>(m_cache.size());
		result->code_cache_resets = m_code_cache_resets;
		result->code_cache_used = m_code_cache_used;
		result->code_cache_capacity = m_code_cache_capacity;
		result->hot_dispatch_cache_hits = m_hot_dispatch_cache_hits;
		result->hot_dispatch_cache_misses = m_hot_dispatch_cache_misses;
		result->hot_dispatch_cache_way_probes = m_hot_dispatch_cache_way_probes;
		result->hot_dispatch_cache_64_set_hits = m_hot_dispatch_cache_64_set_hits;
		result->hot_dispatch_cache_64_set_misses = m_hot_dispatch_cache_64_set_misses;
		result->hot_dispatch_cache_64_set_way_probes =
			m_hot_dispatch_cache_64_set_way_probes;
		result->scheduler_direct_resume_candidates =
			m_scheduler_direct_resume_candidates;
		result->scheduler_direct_resume_installs = m_scheduler_direct_resume_installs;
		result->scheduler_direct_resume_attempts = m_scheduler_direct_resume_attempts;
		result->scheduler_direct_resume_hits = m_scheduler_direct_resume_hits;
		result->scheduler_direct_resume_misses = m_scheduler_direct_resume_misses;
		result->scheduler_direct_resume_no_target =
			m_scheduler_direct_resume_no_target;
		result->scheduler_direct_resume_target_mismatch =
			m_scheduler_direct_resume_target_mismatch;
		result->scheduler_direct_event_entries = m_scheduler_direct_event_entries;
		result->scheduler_direct_event_forwards = m_scheduler_direct_event_forwards;
		result->scheduler_direct_event_fallbacks = m_scheduler_direct_event_fallbacks;
		result->scheduler_direct_event_remainders =
			m_scheduler_direct_event_remainders;
		result->scheduler_direct_event_installs = m_scheduler_direct_event_installs;
		result->scheduler_direct_event_clears = m_scheduler_direct_event_clears;
		result->scheduler_prediction_attempts = m_scheduler_prediction_attempts;
		result->scheduler_prediction_hits = m_scheduler_prediction_hits;
		result->scheduler_prediction_misses = m_scheduler_prediction_misses;
		result->scheduler_prediction_two_way_hits =
			m_scheduler_prediction_two_way_hits;
		result->scheduler_prediction_four_way_hits =
			m_scheduler_prediction_four_way_hits;
		result->scheduler_prediction_forwards = m_scheduler_prediction_forwards;
		result->scheduler_prediction_fallbacks = m_scheduler_prediction_fallbacks;
		result->scheduler_prediction_remainders = m_scheduler_prediction_remainders;
		result->scheduler_dispatch_cache_attempts =
			m_scheduler_dispatch_cache_attempts;
		result->scheduler_dispatch_cache_hits = m_scheduler_dispatch_cache_hits;
		result->scheduler_dispatch_cache_misses = m_scheduler_dispatch_cache_misses;
		result->scheduler_dispatch_cache_forwards =
			m_scheduler_dispatch_cache_forwards;
		result->scheduler_dispatch_cache_fallbacks =
			m_scheduler_dispatch_cache_fallbacks;
		result->scheduler_dispatch_cache_remainders =
			m_scheduler_dispatch_cache_remainders;
		result->scheduler_dispatch_cache_installs =
			m_scheduler_dispatch_cache_installs;
		result->hot_dispatch_trusted_raw_hits = m_hot_dispatch_trusted_raw_hits;
		result->hot_dispatch_owned_hits = m_hot_dispatch_owned_hits;
		std::vector<std::pair<u32, u64>> hot_hit_pcs;
		hot_hit_pcs.reserve(m_hot_dispatch_hit_pc_profile.size());
		for (const auto& entry : m_hot_dispatch_hit_pc_profile)
			hot_hit_pcs.push_back(entry);
		std::sort(hot_hit_pcs.begin(), hot_hit_pcs.end(),
			[](const auto& lhs, const auto& rhs) {
			return lhs.second != rhs.second ? lhs.second > rhs.second : lhs.first < rhs.first;
		});
		result->hot_dispatch_hit_pc_count =
			std::min<u32>(static_cast<u32>(hot_hit_pcs.size()), 16);
		for (u32 i = 0; i < result->hot_dispatch_hit_pc_count; i++)
		{
			result->hot_dispatch_hit_pcs[i] = hot_hit_pcs[i].first;
			result->hot_dispatch_hit_pc_hits[i] = hot_hit_pcs[i].second;
		}
		std::vector<std::pair<u64, InterpreterFallbackProfileEntry>> fallback_pcs;
		fallback_pcs.reserve(m_interpreter_fallback_pc_profile.size());
		for (const auto& entry : m_interpreter_fallback_pc_profile)
			fallback_pcs.push_back(entry);
		std::sort(fallback_pcs.begin(), fallback_pcs.end(),
			[](const auto& lhs, const auto& rhs) {
				return lhs.second.hits != rhs.second.hits ?
					lhs.second.hits > rhs.second.hits : lhs.first < rhs.first;
			});
		result->interpreter_fallback_pc_count =
			std::min<u32>(static_cast<u32>(fallback_pcs.size()), 16);
		for (u32 i = 0; i < result->interpreter_fallback_pc_count; i++)
		{
			result->interpreter_fallback_pcs[i] =
				fallback_pcs[i].second.start_pc;
			result->interpreter_fallback_owner_pcs[i] =
				fallback_pcs[i].second.owner_pc;
			result->interpreter_fallback_owner_opcodes[i] =
				fallback_pcs[i].second.owner_opcode;
			result->interpreter_fallback_instruction_counts[i] =
				fallback_pcs[i].second.instruction_count;
			result->interpreter_fallback_source_hashes[i] =
				fallback_pcs[i].second.source_hash;
			result->interpreter_fallback_hits[i] = fallback_pcs[i].second.hits;
		}
		result->wait_resume_cache_attempts = m_wait_resume_cache_attempts;
		result->wait_resume_cache_hits = m_wait_resume_cache_hits;
		result->wait_resume_cache_misses = m_wait_resume_cache_misses;
		result->wait_resume_event_entries = m_wait_resume_event_entries;
		result->wait_resume_event_forwards = m_wait_resume_event_forwards;
		result->wait_resume_event_fallbacks = m_wait_resume_event_fallbacks;
		result->wait_resume_event_installs = m_wait_resume_event_installs;
		result->wait_resume_event_clears = m_wait_resume_event_clears;
		result->wait_resume_first_entry_owned = m_wait_resume_first_entry_owned;
		result->wait_resume_post_event_identity_checks =
			m_wait_resume_post_event_identity_checks;
		result->wait_resume_kind_specific_entries =
			m_wait_resume_kind_specific_entries;
		result->wait_resume_kind_specific_unconditional_forwards =
			m_wait_resume_kind_specific_unconditional_forwards;
		result->wait_resume_kind_specific_poll_forwards =
			m_wait_resume_kind_specific_poll_forwards;
		result->wait_resume_kind_specific_conditional_forwards =
			m_wait_resume_kind_specific_conditional_forwards;
		result->wait_resume_clock_specific_entries =
			m_wait_resume_clock_specific_entries;
		result->wait_resume_clock_specific_forwards =
			m_wait_resume_clock_specific_forwards;
		result->wait_resume_no_link_specific_entries =
			m_wait_resume_no_link_specific_entries;
		result->wait_resume_no_link_specific_forwards =
			m_wait_resume_no_link_specific_forwards;
		result->wait_resume_descriptor_forwards = m_wait_resume_descriptor_forwards;
		result->wait_resume_unconditional_forwards =
			m_wait_resume_unconditional_forwards;
		result->wait_resume_poll_forwards = m_wait_resume_poll_forwards;
		result->wait_resume_conditional_forwards = m_wait_resume_conditional_forwards;
		result->direct_budget_exit_provider_entries =
			m_direct_budget_exit_provider_entries;
		result->constant_cycle_budget_provider_entries =
			m_constant_cycle_budget_provider_entries;
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
		result->clock_mode_check_instructions_removed =
			m_clock_mode_check_instructions_removed;
		result->saved_register_stack_words_removed =
			m_saved_register_stack_words_removed;
		result->saved_register_frame_instructions_added =
			m_saved_register_frame_instructions_added;
		result->saved_register_frame_instructions_removed =
			m_saved_register_frame_instructions_removed;
		result->batched_cycle_instructions_removed =
			m_batched_cycle_instructions_removed;
		result->batched_cycle_stack_words_removed =
			m_batched_cycle_stack_words_removed;
		result->expanded_cycle_batching_provider_entries =
			m_expanded_cycle_batching_provider_entries;
		result->total_pinned_gpr_memory_ops_saved = m_pinned_gpr_memory_ops_saved;
		result->total_pinned_branch_operand_moves_removed =
			m_pinned_branch_operand_moves_removed;
		result->total_condition_code_branch_instructions_removed =
			m_condition_code_branch_instructions_removed;
		result->total_producer_branch_compare_instructions_removed =
			m_producer_branch_compare_instructions_removed;
		result->total_fused_ram_guard_instructions_removed =
			m_fused_ram_guard_instructions_removed;
		result->total_source_page_guard_instructions_removed =
			m_source_page_guard_instructions_removed;
		result->total_source_page_literal_instructions_removed =
			m_source_page_literal_instructions_removed;
		result->total_isolate_cache_guard_instructions_removed =
			m_isolate_cache_guard_instructions_removed;
		result->linked_frame_bypass_entries = s_qemuIopLinkedFrameEvidence.entries;
		result->linked_frame_instructions_removed =
			s_qemuIopLinkedFrameEvidence.instructions_removed;
		result->linked_frame_stack_words_removed =
			s_qemuIopLinkedFrameEvidence.stack_words_removed;
		result->resident_prelude_links = 0;
		result->resident_prelude_setup_instructions_removed = 0;
		result->resident_gpr_links = 0;
		result->resident_gpr_stores_removed = 0;
		result->resident_gpr_loads_removed = 0;
		result->resident_ee_budget_links = 0;
		result->resident_ee_budget_stores_removed = 0;
		result->resident_ee_budget_loads_removed = 0;
		for (const std::unique_ptr<CachedBlock>& cached : m_cache)
		{
			if (!cached || !cached->valid)
				continue;
			for (const DirectLinkSlot& link : cached->direct_links.slots)
			{
				if (!link.valid || !link.resident_entry_active)
					continue;
				result->resident_prelude_links++;
				result->resident_prelude_setup_instructions_removed +=
					link.resident_setup_instructions_removed;
				if (link.resident_gpr_entry_active)
				{
					result->resident_gpr_links++;
					result->resident_gpr_stores_removed +=
						link.resident_gpr_stores_removed;
					result->resident_gpr_loads_removed +=
						link.resident_gpr_loads_removed;
				}
				if (link.resident_ee_budget_entry_active)
				{
					result->resident_ee_budget_links++;
					result->resident_ee_budget_stores_removed++;
					result->resident_ee_budget_loads_removed++;
				}
			}
		}
		result->sequential_qword_copy_fast_paths =
			s_qemuIopSequentialQwordCopyFastPaths;
		// The focused control is 62 product A32 instructions larger even though
		// the enabled QEMU block also carries a four-instruction dynamic counter.
		// Attribute only 36: the lower bound excludes that counter, block-frame
		// differences, the rare source-page Clear call, and every cold fallback.
		result->sequential_qword_copy_instructions_removed =
			static_cast<u64>(s_qemuIopSequentialQwordCopyFastPaths) * 36u;
		result->private_dispatcher_calls = m_private_dispatcher_calls;
		result->private_dispatcher_provider_entries =
			m_private_dispatcher_provider_entries;
		result->private_dispatcher_wait_forwards = m_private_dispatcher_wait_forwards;
		result->private_dispatcher_generated_entries =
			m_private_dispatcher_generated_entries;
		result->private_dispatcher_fallbacks = m_private_dispatcher_fallbacks;
		result->private_dispatcher_inlined_hot_entries =
			m_private_dispatcher_inlined_hot_entries;
		result->private_frame_provider_entries = m_private_frame_provider_entries;
		result->private_frame_stack_words_removed =
			m_private_frame_stack_words_removed;
		result->private_frame_zero_scratch_entries =
			m_private_frame_zero_scratch_entries;
		result->cached_wait_descriptor_checks = m_cached_wait_descriptor_checks;
		result->cached_wait_descriptor_forwards = m_cached_wait_descriptor_forwards;
		result->cached_wait_descriptor_opcode_reads_removed =
			m_cached_wait_descriptor_opcode_reads_removed;
		result->cached_wait_descriptor_unconditional_checks =
			m_cached_wait_descriptor_unconditional_checks;
		result->compiled_ps1_bios_gate_blocks = m_compiled_ps1_bios_gate_blocks;
		result->compiled_ps1_bios_gate_entries = m_compiled_ps1_bios_gate_entries;
		result->dispatcher_ps1_bios_gate_checks_removed =
			m_dispatcher_ps1_bios_gate_checks_removed;
		result->inline_wait_fast_forwards = s_qemuIopInlineWaitFastForwards;
		result->branch_event_candidates = s_qemuIopBranchEventCandidates;
		result->branch_event_budget_positive = s_qemuIopBranchEventBudgetPositive;
		result->branch_event_tests_entered = s_qemuIopBranchEventTestsEntered;
		result->budget_before_event_fast_exits =
			s_qemuIopBranchEventCandidates >= s_qemuIopBranchEventBudgetPositive ? s_qemuIopBranchEventCandidates - s_qemuIopBranchEventBudgetPositive : 0;
		// Before its first possible helper branch, the old product event path
		// always scheduled iopNextEventCycle and tested the counter deadline.
		// Twelve A32 instructions is a lower bound for that removed prefix and
		// excludes all later INTC/device work and QEMU-only counters.
		result->budget_before_event_instructions_removed =
			result->budget_before_event_fast_exits * 12u;
		result->event_deadline_fast_skips =
			s_qemuIopBranchEventBudgetPositive >= s_qemuIopBranchEventTestsEntered ? s_qemuIopBranchEventBudgetPositive -
																						 s_qemuIopBranchEventTestsEntered :
																					 0;
		// The focused control emits a six-instruction due guard. Charge a
		// conservative seven-instruction allowance against the twelve-instruction
		// event prefix and attribute only the five-instruction net lower bound.
		result->event_deadline_instructions_removed =
			result->event_deadline_fast_skips * 5u;
	}
#endif

	bool BlockExecutor::RunValidatedBlock(CachedBlock& block,
		BlockExecutionResult* result,
		bool publish_details,
		bool forced_continuation)
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
		const u32 dispatch_flags = RunProviderBlock(block, 0, forced_continuation);
		if ((dispatch_flags & ProviderDispatchSuccess) == 0)
			return false;

		const bool wait_forward = (dispatch_flags & ProviderDispatchWaitForward) != 0;
		if ((dispatch_flags & ProviderDispatchLogicalContinuation) != 0)
		{
			result->exit = (dispatch_flags & ProviderDispatchIsolateWrite) != 0 ? BlockExitKind::LogicalContinuationIsolateModeWrite : BlockExitKind::LogicalContinuation;
		}
		else
		{
			result->exit = (dispatch_flags & ProviderDispatchIsolateWrite) != 0 ? BlockExitKind::IsolateModeWrite : BlockExitKind::Direct;
		}
		if (publish_details)
		{
			PublishExecutionDetails(block, result);
		}
#if defined(VITASX2_QEMU_VALIDATION)
		else
		{
			result->instruction_count = block.instruction_count;
			result->pinned_gpr_memory_ops_saved = block.pinned_gpr_memory_ops_saved;
			result->pinned_branch_operand_moves_removed =
				block.pinned_branch_operand_moves_removed;
			result->condition_code_branch_instructions_removed =
				block.condition_code_branch_instructions_removed;
			result->producer_branch_compare_instructions_removed =
				block.producer_branch_compare_instructions_removed;
			result->fused_ram_guard_instructions_removed =
				block.fused_ram_guard_instructions_removed;
			result->source_page_guard_instructions_removed =
				block.source_page_guard_instructions_removed;
			result->source_page_literal_instructions_removed =
				block.source_page_literal_instructions_removed;
			result->isolate_cache_guard_instructions_removed =
				block.isolate_cache_guard_instructions_removed;
		}
#endif
		if (wait_forward)
		{
			ClearSchedulerDirectResume();
			// The generated block did not execute, so none of its memory or
			// producer-to-branch fast paths was dynamically traversed.
#if defined(VITASX2_QEMU_VALIDATION)
			result->producer_branch_compare_instructions_removed = 0;
			result->fused_ram_guard_instructions_removed = 0;
			result->source_page_guard_instructions_removed = 0;
			result->source_page_literal_instructions_removed = 0;
			result->isolate_cache_guard_instructions_removed = 0;
#endif
		}
		result->wait_loop_fast_forward = wait_forward;
		result->isolate_mode_switched =
			(dispatch_flags & ProviderDispatchIsolateSwitch) != 0;
		return true;
	}

	inline __attribute__((always_inline)) u32
	BlockExecutor::RunProviderBlockInline(CachedBlock& block, u32 dispatch_flags,
		bool forced_continuation)
	{
		if (!block.valid)
			return 0;
		VitaPerformanceTelemetry::PublishIopGeneratedPcIfProfiling(
			block.start_pc);

		// PCSX2 owner: x86/iR3000A.cpp::_DynGen_EnterRecompiledCode() enters
		// the selected BaseBlock directly and returns only dispatcher control.
		// psxRecExecuteBlock() passes the architectural PC it just read, so the
		// detailed API's redundant psxRegs.pc publication and result aggregate
		// are unnecessary on this provider-only path.
		bool wait_forward = false;
		if ((!block.HasRamPollWaitLoop() ||
				!(forced_continuation && psxRegs.iopCycleEE <= 0)) &&
			!block.compiled_ps1_bios_gate && block.wait_loop_shape &&
			block.wait_loop_enabled_at_compile)
		{
			if (block.HasRamPollWaitLoop())
			{
				wait_forward = TryFastForwardPollCallWaitLoop(block);
			}
			else
			{
#if defined(VITASX2_QEMU_VALIDATION)
				wait_forward =
					s_qemuIopCachedWaitDescriptorEnabled ? (block.wait_loop_descriptor.condition ==
																	   WaitLoopCondition::Always ?
						TryFastForwardCachedUnconditionalWaitLoop(block) :
						TryFastForwardCachedWaitLoop(block)) :
					TryFastForwardTrustedWaitLoopAtPc(block.start_pc);
#else
				wait_forward =
					block.wait_loop_descriptor.condition == WaitLoopCondition::Always ? TryFastForwardCachedUnconditionalWaitLoop(block) : TryFastForwardCachedWaitLoop(block);
#endif
			}
		}
		if (wait_forward)
		{
			ClearSchedulerDirectResume();
			// PCSX2's generated wait block remains the current dispatcher target
			// until an event changes PC. Retain that exact BaseBlock identity across
			// EE scheduler calls; RAM source invalidation clears the pointer.
			if (block.raw_opcodes &&
				RecLookupIdentity(psxRegs.pc) == block.rec_lookup_identity)
			{
#if defined(VITASX2_QEMU_VALIDATION)
				if (s_qemuIopWaitResumeCacheEnabled)
#endif
					SetWaitResumeBlock(&block);
			}
			else if (m_wait_resume_block == &block)
			{
				ClearWaitResumeBlock();
			}
#if defined(VITASX2_QEMU_VALIDATION)
			m_pinned_gpr_memory_ops_saved += block.pinned_gpr_memory_ops_saved;
			m_pinned_branch_operand_moves_removed +=
				block.pinned_branch_operand_moves_removed;
			m_condition_code_branch_instructions_removed +=
				block.condition_code_branch_instructions_removed;
#endif
#if defined(VITASX2_PORTABLE_REPLAY_VALIDATION) || \
	defined(VITASX2_PRODUCT_BOOT_VALIDATION)
			if (m_portable_validation_stats.native_provider_entries != UINT32_MAX)
				m_portable_validation_stats.native_provider_entries++;
#endif
			return dispatch_flags | ProviderDispatchSuccess |
			       ProviderDispatchWaitForward;
		}
		if (m_wait_resume_block == &block)
			ClearWaitResumeBlock();

#if defined(VITASX2_QEMU_VALIDATION)
		if (block.compiled_ps1_bios_gate)
			m_compiled_ps1_bios_gate_entries++;
		if (block.direct_budget_exit)
			m_direct_budget_exit_provider_entries++;
		if (block.constant_cycle_budget)
			m_constant_cycle_budget_provider_entries++;
		m_clock_mode_check_instructions_removed +=
			block.clock_mode_check_instructions_removed;
		m_saved_register_stack_words_removed +=
			block.saved_register_stack_words_removed;
		m_saved_register_frame_instructions_added +=
			block.saved_register_frame_instructions_added;
		m_saved_register_frame_instructions_removed +=
			block.saved_register_frame_instructions_removed;
		m_batched_cycle_instructions_removed +=
			block.batched_cycle_instructions_removed;
		m_batched_cycle_stack_words_removed +=
			block.batched_cycle_stack_words_removed;
		if (block.expanded_cycle_batching)
			m_expanded_cycle_batching_provider_entries++;
		m_private_frame_provider_entries++;
		m_private_frame_stack_words_removed +=
			2u * static_cast<u64>(__builtin_popcount(
					 static_cast<unsigned>(block.saved_registers | REG_LR)));
		if (block.stack_frame_size == 0)
			m_private_frame_zero_scratch_entries++;
#endif

#if defined(__arm__)
		const u32 exit_value = RunGeneratedProviderEntry(ProviderEntryPoint(block));
#else
		const u32 exit_value =
			reinterpret_cast<GeneratedBlock>(block.Fragment(0).code.EntryPoint())();
#endif
		const bool scheduler_resume_exit =
			(exit_value & SCHEDULER_DIRECT_RESUME_TAG) != 0;
		const bool logical_continuation_exit =
			exit_value == static_cast<u32>(BlockExitKind::LogicalContinuation) ||
			exit_value ==
				static_cast<u32>(BlockExitKind::LogicalContinuationIsolateModeWrite);
		const bool isolate_write_exit =
			exit_value == static_cast<u32>(BlockExitKind::IsolateModeWrite) ||
			exit_value ==
				static_cast<u32>(BlockExitKind::LogicalContinuationIsolateModeWrite);
		if (!scheduler_resume_exit &&
			exit_value != static_cast<u32>(BlockExitKind::Direct) &&
			exit_value != static_cast<u32>(BlockExitKind::IsolateModeWrite) &&
			!logical_continuation_exit)
		{
			// No PCSX2 BaseBlock owns any other dispatcher result. Retire the
			// failed translation while its identity is still known so every lookup,
			// direct link, scheduler cache, and RAM-source registration is removed
			// before the exact recompiler fallback executes this PC.
			ClearSchedulerDirectResume();
			InvalidateCachedBlock(block);
#if defined(VITASX2_PORTABLE_REPLAY_VALIDATION) || \
	defined(VITASX2_PRODUCT_BOOT_VALIDATION)
			if (m_portable_validation_stats.invalid_provider_results != UINT32_MAX)
				m_portable_validation_stats.invalid_provider_results++;
#endif
			return 0;
		}
		if (scheduler_resume_exit && SchedulerDirectResumeEnabled())
		{
			CachedBlock* const target = reinterpret_cast<CachedBlock*>(
				static_cast<uptr>(exit_value & ~SCHEDULER_DIRECT_RESUME_TAG));
#if defined(VITASX2_QEMU_VALIDATION) && \
	!defined(VITASX2_IOP_SCHEDULER_DIRECT_RESUME_CODEGEN)
			m_scheduler_direct_resume_candidates++;
			const bool target_matches =
				target && target->valid &&
				target->rec_lookup_identity == RecLookupIdentity(psxRegs.pc) &&
				target->isolate_cache_active == m_active_isolate_cache_mode;
			if (!target_matches)
			{
				m_scheduler_direct_resume_target_mismatch++;
				ClearSchedulerDirectResume();
			}
			else
			{
				SetSchedulerDirectResumeBlock(target);
				m_scheduler_direct_resume_installs++;
			}
#else
			SetSchedulerDirectResumeBlock(target);
#endif
		}
		else
		{
			ClearSchedulerDirectResume();
		}

#if defined(VITASX2_QEMU_VALIDATION)
		m_pinned_gpr_memory_ops_saved += block.pinned_gpr_memory_ops_saved;
		m_pinned_branch_operand_moves_removed +=
			block.pinned_branch_operand_moves_removed;
		m_condition_code_branch_instructions_removed +=
			block.condition_code_branch_instructions_removed;
		m_producer_branch_compare_instructions_removed +=
			block.producer_branch_compare_instructions_removed;
		m_fused_ram_guard_instructions_removed +=
			block.fused_ram_guard_instructions_removed;
		m_source_page_guard_instructions_removed +=
			block.source_page_guard_instructions_removed;
		m_source_page_literal_instructions_removed +=
			block.source_page_literal_instructions_removed;
		m_isolate_cache_guard_instructions_removed +=
			block.isolate_cache_guard_instructions_removed;
#endif

		if (logical_continuation_exit)
		{
			ClearSchedulerDirectResume();
			dispatch_flags |= ProviderDispatchLogicalContinuation;
		}

		if (isolate_write_exit)
		{
			ClearSchedulerDirectResume();
			ClearSchedulerPredictedResume();
			dispatch_flags |= ProviderDispatchIsolateWrite;
			const bool new_mode = (psxRegs.CP0.n.Status & 0x10000u) != 0;
			if (new_mode != m_active_isolate_cache_mode)
				dispatch_flags |= ProviderDispatchIsolateSwitch;
			m_active_isolate_cache_mode = new_mode;
		}

#if defined(VITASX2_PORTABLE_REPLAY_VALIDATION) || \
	defined(VITASX2_PRODUCT_BOOT_VALIDATION)
		if (m_portable_validation_stats.native_provider_entries != UINT32_MAX)
			m_portable_validation_stats.native_provider_entries++;
#endif
		return dispatch_flags | ProviderDispatchSuccess;
	}

	u32 BlockExecutor::RunProviderBlock(CachedBlock& block, u32 dispatch_flags,
		bool forced_continuation)
	{
		return RunProviderBlockInline(block, dispatch_flags, forced_continuation);
	}

	inline __attribute__((always_inline)) BlockExecutor::CachedBlock*
	BlockExecutor::FindSchedulerDirectResumeBlock(u32* dispatch_flags)
	{
		if (!dispatch_flags)
			return nullptr;
		CachedBlock* const block = m_scheduler_direct_resume_event_context.block;
		if (!block)
			return nullptr;

#if defined(VITASX2_QEMU_VALIDATION) && \
	!defined(VITASX2_IOP_SCHEDULER_DIRECT_RESUME_CODEGEN)
		m_scheduler_direct_resume_attempts++;
#endif
		bool match = SchedulerDirectResumeEnabled() &&
		             RecLookupIdentity(psxRegs.pc) == block->rec_lookup_identity;
#if defined(VITASX2_QEMU_VALIDATION) && \
	!defined(VITASX2_IOP_SCHEDULER_DIRECT_RESUME_CODEGEN)
		match = match && block->valid &&
			block->isolate_cache_active == m_active_isolate_cache_mode;
		if (match && s_qemuIopTrustedSourceAuditEnabled && block->trusted_source)
		{
			m_hot_dispatch_trusted_raw_hits++;
			match = ValidateCachedBlock(*block);
		}
#endif
		if (!match)
		{
#if defined(VITASX2_QEMU_VALIDATION) && \
	!defined(VITASX2_IOP_SCHEDULER_DIRECT_RESUME_CODEGEN)
			m_scheduler_direct_resume_misses++;
#endif
			ClearSchedulerDirectResume();
			return nullptr;
		}

		*dispatch_flags = ProviderDispatchCacheHit | ProviderDispatchLookupHit |
			ProviderDispatchFastHit;
#if defined(VITASX2_QEMU_VALIDATION) && \
	!defined(VITASX2_IOP_SCHEDULER_DIRECT_RESUME_CODEGEN)
		m_scheduler_direct_resume_hits++;
#endif
#if defined(VITASX2_QEMU_VALIDATION)
		m_private_dispatcher_inlined_hot_entries++;
#endif
		return block;
	}

	bool BlockExecutor::ExecuteCompiledBlock(u32 start_pc, u32 instruction_count,
		BlockExecutionResult* result,
		bool publish_details)
	{
		return ExecuteCompiledBlockInternal(start_pc, instruction_count, result,
			publish_details, false);
	}

	bool BlockExecutor::ExecuteCompiledBlockInternal(
		u32 start_pc, u32 instruction_count, BlockExecutionResult* result,
		bool publish_details, bool entry_effects_already_applied,
		bool logical_continuation, bool discovered_topology,
		bool forced_continuation)
	{
		if (!result || instruction_count == 0 ||
			instruction_count > MAX_LOGICAL_BLOCK_INSTRUCTIONS ||
			instruction_count > ((UINT32_MAX - start_pc) / 4))
		{
			return false;
		}

		result->cache_hit = false;
		result->lookup_hit = false;
		result->fast_dispatch_hit = false;
		result->wait_loop_fast_forward = false;
		result->isolate_mode_switched = false;
		CachedBlock* block = nullptr;
		bool lookup_hit = false;
		if (FindCachedBlock(start_pc, instruction_count, &block, &lookup_hit) &&
			(!discovered_topology || block->discovered_topology))
		{
			result->cache_hit = true;
			result->lookup_hit = lookup_hit;
			return RunValidatedBlock(*block, result, publish_details,
				forced_continuation);
		}

		if (!entry_effects_already_applied)
		{
			ApplyIopRecompilerEntrySideEffects(start_pc);
			entry_effects_already_applied = true;
		}
		block = AllocateCacheEntry();
		if (!block ||
			!CompileIntoCacheEntry(*block, start_pc, instruction_count,
				entry_effects_already_applied, true,
				logical_continuation, discovered_topology))
			return false;

		result->cache_hit = false;
		result->lookup_hit = false;
		const bool ran =
			RunValidatedBlock(*block, result, publish_details, forced_continuation);
		if (ran && !publish_details)
		{
			result->instruction_count = block->instruction_count;
			result->native_instruction_count = block->native_instruction_count;
			result->helper_instruction_count = block->helper_instruction_count;
			result->code_cache_resets = m_code_cache_resets;
		}
		return ran;
	}

	bool BlockExecutor::ExecuteCompiledBlockAtPc(u32 start_pc,
		BlockExecutionResult* result,
		bool publish_details,
		bool forced_continuation)
	{
		if (!result || (start_pc & 0x3u) != 0)
			return false;

		result->cache_hit = false;
		result->lookup_hit = false;
		result->fast_dispatch_hit = false;
		result->wait_loop_fast_forward = false;
		result->isolate_mode_switched = false;

		// PCSX2 owner: x86/BaseblockEx.h::PC_GETBLOCK_() looks up the
		// translated BaseBlock by guest PC before doing any decode work. Keep
		// the Vita IOP hot path on the same shape. A 512-set, two-way exact
		// first-level cache adapts psxRecLUT's direct lookup to Vita's smaller
		// memory budget;
		// the lazy two-level table remains the collision and cold fallback.
		if (CachedBlock* entry = FindHotDispatchCacheBlock(start_pc);
			entry && entry->discovered_topology)
		{
			// PCSX2's psxRecLUT dispatcher trusts a live exact translation because
			// psxRecClearMem() removes stale RAM blocks before dispatch. Raw Vita
			// sources have the same explicit-invalidation contract; immutable ROM
			// also needs no per-hit comparison. Handler-backed and cross-page
			// sources retain ValidateCachedBlock(), as does the opt-in QEMU audit.
			bool trust_source = entry->trusted_source;
#if defined(VITASX2_QEMU_VALIDATION)
			trust_source &= !s_qemuIopTrustedSourceAuditEnabled;
			if (trust_source)
				m_hot_dispatch_trusted_raw_hits++;
#endif
			if (trust_source || ValidateCachedBlock(*entry))
			{
#if defined(VITASX2_QEMU_VALIDATION)
				m_hot_dispatch_cache_hits++;
				m_hot_dispatch_hit_pc_profile[start_pc]++;
#endif
				result->cache_hit = true;
				result->lookup_hit = true;
				result->fast_dispatch_hit = true;
				return RunValidatedBlock(*entry, result, publish_details,
					forced_continuation);
			}
		}
#if defined(VITASX2_QEMU_VALIDATION)
		m_hot_dispatch_cache_misses++;
#endif

		// Validate the page-table fallback before execution, then promote it.
		if (CachedBlock* entry =
				FindLookupBlockByStartPc(start_pc, m_active_isolate_cache_mode))
		{
			if (entry->valid && entry->discovered_topology &&
				ValidateCachedBlock(*entry))
			{
				RegisterHotDispatchCache(*entry);
				result->cache_hit = true;
				result->lookup_hit = true;
				result->fast_dispatch_hit = true;
				return RunValidatedBlock(*entry, result, publish_details,
					forced_continuation);
			}
		}

		if (CachedBlock* entry = FindRecordedBlockByStartPc(
				start_pc, 0, false, m_active_isolate_cache_mode, true))
		{
			RegisterBlockLookup(*entry);
			result->cache_hit = true;
			result->fast_dispatch_hit = true;
			return RunValidatedBlock(*entry, result, publish_details,
				forced_continuation);
		}
		if (FindInterpreterFallbackBlock(start_pc))
			return false;

		u32 executable_bytes = 0;
		if (!GetIopRecExecutableSpan(start_pc, &executable_bytes) ||
			executable_bytes < sizeof(u32))
			return false;

		// PCSX2 iopRecRecompile() performs its one-shot target effects, then
		// resets an exhausted arena, and only then discovers BaseBlock seams.
		// Allocate first so a metadata-pressure reset cannot delete a successor
		// after we have already baked that successor into this block's shape.
		ApplyIopRecompilerEntrySideEffects(start_pc);
		CachedBlock* const block = AllocateCacheEntry();
		if (!block)
		{
			RegisterInterpreterFallbackBlock(start_pc);
			return false;
		}

		BlockScanResult scan;
		if (ScanProviderLogicalBlock(start_pc, &scan) != BlockScanStatus::Success ||
			scan.instruction_count == 0)
		{
			RememberFreeCacheEntry(*block);
			return false;
		}
		if (!CompileIntoCacheEntry(*block, start_pc, scan.instruction_count, true,
				true, scan.logical_continuation, true))
		{
			RegisterInterpreterFallbackBlock(start_pc);
			return false;
		}

		const bool ran =
			RunValidatedBlock(*block, result, publish_details, forced_continuation);
		if (ran && !publish_details)
		{
			result->instruction_count = block->instruction_count;
			result->native_instruction_count = block->native_instruction_count;
			result->helper_instruction_count = block->helper_instruction_count;
			result->code_cache_resets = m_code_cache_resets;
		}
		return ran;
	}

	__attribute__((noinline, cold)) BlockExecutor::CachedBlock*
	BlockExecutor::FindProviderBlockAtPcSlow(u32 start_pc,
		ProviderCompileResult* compile_result,
		u32* dispatch_flags)
	{
		const VitaPerformanceTelemetry::ScopedCpuStage provider_profile(
			VitaPerformanceTelemetry::CpuStage::IopProvider);
#if defined(VITASX2_QEMU_VALIDATION)
		const auto publish_profile_metadata = [compile_result](
												  const CachedBlock& block) {
			if (!compile_result)
				return;
			compile_result->instruction_count = block.instruction_count;
			compile_result->native_instruction_count = block.native_instruction_count;
			compile_result->helper_instruction_count = block.helper_instruction_count;
			compile_result->code_cache_resets = 0;
		};
#endif

		if (CachedBlock* entry =
				FindLookupBlockByStartPc(start_pc, m_active_isolate_cache_mode))
		{
			if (entry->valid && entry->discovered_topology &&
				ValidateCachedBlock(*entry))
			{
				RegisterHotDispatchCache(*entry);
#if defined(VITASX2_QEMU_VALIDATION)
				publish_profile_metadata(*entry);
#endif
				*dispatch_flags = ProviderDispatchCacheHit | ProviderDispatchLookupHit |
					ProviderDispatchFastHit;
				return entry;
			}
		}

		if (CachedBlock* entry = FindRecordedBlockByStartPc(
				start_pc, 0, false, m_active_isolate_cache_mode, true))
		{
			RegisterBlockLookup(*entry);
#if defined(VITASX2_QEMU_VALIDATION)
			publish_profile_metadata(*entry);
#endif
			*dispatch_flags = ProviderDispatchCacheHit | ProviderDispatchFastHit;
			return entry;
		}
		if (FindInterpreterFallbackBlock(start_pc))
			return nullptr;

		u32 executable_bytes = 0;
		if (!GetIopRecExecutableSpan(start_pc, &executable_bytes) ||
			executable_bytes < sizeof(u32))
			return nullptr;

		const VitaPerformanceTelemetry::ScopedCpuStage compile_profile(
			VitaPerformanceTelemetry::CpuStage::IopCompile);
		const VitaPerformanceTelemetry::ScopedExactIopCompileMeasurement
			exact_compile_profile;
		ApplyIopRecompilerEntrySideEffects(start_pc);
		CachedBlock* block = AllocateCacheEntry();
		if (!block)
		{
			RegisterInterpreterFallbackBlock(start_pc);
			return nullptr;
		}

		BlockScanResult scan;
		if (ScanProviderLogicalBlock(start_pc, &scan) != BlockScanStatus::Success ||
			scan.instruction_count == 0)
		{
			RememberFreeCacheEntry(*block);
			return nullptr;
		}
		if (!CompileIntoCacheEntry(*block, start_pc, scan.instruction_count, true,
				true, scan.logical_continuation, true))
		{
			RegisterInterpreterFallbackBlock(start_pc);
			return nullptr;
		}

		if (compile_result)
		{
			compile_result->instruction_count = block->instruction_count;
			compile_result->native_instruction_count = block->native_instruction_count;
			compile_result->helper_instruction_count = block->helper_instruction_count;
			compile_result->code_cache_resets = m_code_cache_resets;
		}
		*dispatch_flags = 0;
		return block;
	}

	inline __attribute__((always_inline)) BlockExecutor::CachedBlock*
	BlockExecutor::FindProviderBlockAtPcInline(
		u32 start_pc, ProviderCompileResult* compile_result, u32* dispatch_flags)
	{
		if ((start_pc & 0x3u) != 0 || !dispatch_flags)
			return nullptr;

#if defined(VITASX2_QEMU_VALIDATION)
		const bool inline_hot_path = s_qemuIopPrivateDispatcherHotPathEnabled;
		const auto publish_profile_metadata = [compile_result](
												  const CachedBlock& block) {
			if (!compile_result)
				return;
			compile_result->instruction_count = block.instruction_count;
			compile_result->native_instruction_count = block.native_instruction_count;
			compile_result->helper_instruction_count = block.helper_instruction_count;
			compile_result->code_cache_resets = 0;
		};
		CachedBlock* entry = inline_hot_path ? FindHotDispatchCacheBlockInline(start_pc) : FindHotDispatchCacheBlock(start_pc);
#else
		CachedBlock* entry = FindHotDispatchCacheBlockInline(start_pc);
#endif
		*dispatch_flags = 0;
		if (entry && !entry->discovered_topology)
			entry = nullptr;

		// PCSX2's PSX_GETBLOCK()/DispatcherReg pair performs one exact LUT lookup
		// and enters generated code without crossing a C ABI. Keep the analogous
		// two-way Vita lookup and the one shared execution body in this private
		// dispatcher; every page/search/scan/compile operation is cold.
		if (entry)
		{
			bool trust_source = entry->trusted_source;
#if defined(VITASX2_QEMU_VALIDATION)
			trust_source &= !s_qemuIopTrustedSourceAuditEnabled;
			if (trust_source)
				m_hot_dispatch_trusted_raw_hits++;
#endif
			if (trust_source || ValidateCachedBlock(*entry))
			{
#if defined(VITASX2_QEMU_VALIDATION)
				m_hot_dispatch_cache_hits++;
				m_hot_dispatch_hit_pc_profile[start_pc]++;
				publish_profile_metadata(*entry);
				if (inline_hot_path)
					m_private_dispatcher_inlined_hot_entries++;
#endif
				*dispatch_flags = ProviderDispatchCacheHit | ProviderDispatchLookupHit |
				                  ProviderDispatchFastHit;
			}
			else
			{
				entry = nullptr;
			}
		}

		if (!entry)
		{
#if defined(VITASX2_QEMU_VALIDATION)
			m_hot_dispatch_cache_misses++;
#endif
			entry = FindProviderBlockAtPcSlow(start_pc, compile_result, dispatch_flags);
			if (!entry)
				return nullptr;
		}
		return entry;
	}

	inline __attribute__((always_inline)) u32
	BlockExecutor::ExecuteProviderBlockAtPcInline(
		u32 start_pc, ProviderCompileResult* compile_result,
		bool forced_continuation)
	{
		u32 dispatch_flags = 0;
		CachedBlock* const entry =
			FindProviderBlockAtPcInline(start_pc, compile_result, &dispatch_flags);
		if (!entry)
			return 0;

#if defined(VITASX2_QEMU_VALIDATION)
		if (!s_qemuIopPrivateDispatcherHotPathEnabled)
			return RunProviderBlock(*entry, dispatch_flags, forced_continuation);
#endif
		return RunProviderBlockInline(*entry, dispatch_flags, forced_continuation);
	}

	u32 BlockExecutor::ExecuteProviderBlockAtPc(
		u32 start_pc, ProviderCompileResult* compile_result,
		bool forced_continuation)
	{
		return ExecuteProviderBlockAtPcInline(start_pc, compile_result,
			forced_continuation);
	}

	s32 BlockExecutor::ExecuteInterpreterFallbackTimeslice(s32 ee_cycles)
	{
		VitaPerformanceTelemetry::ScopedCpuStage stage(
			VitaPerformanceTelemetry::CpuStage::IopInterpreter);
#if defined(VITASX2_PORTABLE_REPLAY_VALIDATION) || \
	defined(VITASX2_PRODUCT_BOOT_VALIDATION)
		if (m_portable_validation_stats.interpreter_fallback_entries != UINT32_MAX)
			m_portable_validation_stats.interpreter_fallback_entries++;
#endif
		const s32 result = psxInt.ExecuteBlock(ee_cycles);
		bool isolate_variants_enabled = true;
#if defined(VITASX2_QEMU_VALIDATION)
		isolate_variants_enabled = s_qemuIopIsolateCacheSpecializationEnabled;
#endif
		const bool new_mode =
			isolate_variants_enabled && (psxRegs.CP0.n.Status & 0x10000u) != 0;
		if (new_mode != m_active_isolate_cache_mode)
		{
			// The interpreter owns CP0 while a rejected block runs. Mirror a
			// generated IsolateModeWrite exit before native lookup resumes so no
			// retained normal/isolate entry can execute under the opposite mode.
			ClearWaitResumeBlock();
			ClearSchedulerDirectResume();
			ClearSchedulerPredictedResume();
			m_active_isolate_cache_mode = new_mode;
		}
		return result;
	}

	s32 BlockExecutor::ExecuteInterpreterRecompilerBlock(
		s32 ee_cycles, bool* logical_continuation, bool* execution_terminated)
	{
		VitaPerformanceTelemetry::ScopedCpuStage stage(
			VitaPerformanceTelemetry::CpuStage::IopInterpreter);
#if defined(VITASX2_PORTABLE_REPLAY_VALIDATION) || \
	defined(VITASX2_PRODUCT_BOOT_VALIDATION)
		if (m_portable_validation_stats.interpreter_fallback_entries != UINT32_MAX)
			m_portable_validation_stats.interpreter_fallback_entries++;
#endif
		if (logical_continuation)
			*logical_continuation = false;
		if (execution_terminated)
			*execution_terminated = false;
		psxRegs.iopCycleEE = ee_cycles;

		// A PCSX2 no-test BaseBlock link enters its target even after the prefix
		// exhausted the EE budget. psxInt cannot own this seam: its branch helper
		// runs iopEventTest() before intExecuteBlock() charges the EE budget, and
		// it assigns one cycle to MULT/DIV instead of the x86 recompiler's 8/41.
		// A rejected A32 lowering still owns the BaseBlock PCSX2 discovered on
		// its first compilation attempt. Copy the retained opcode snapshot before
		// executing it: an instruction in this block can write its own source and
		// retire the descriptor, just as PCSX2 can unlink metadata while already-
		// emitted host bytes continue to execute.
		const u32 requested_pc = psxRegs.pc;
		BlockScanResult scan{};
		std::vector<u32> opcodes;
		u32 start_pc = requested_pc;
		if (const InterpreterFallbackBlock* const retained =
				FindInterpreterFallbackBlock(requested_pc))
		{
			start_pc = retained->start_pc;
			scan.start_pc = retained->start_pc;
			scan.instruction_count = retained->instruction_count;
			scan.stop_pc = retained->stop_pc;
			scan.logical_continuation = retained->logical_continuation;
			opcodes = retained->opcodes;
		}
		else
		{
			const BlockScanStatus scan_status =
				ScanProviderLogicalBlock(requested_pc, &scan);
			if (scan_status != BlockScanStatus::Success ||
				scan.instruction_count == 0)
			{
				// PCSX2 owner: x86/iR3000A.cpp::_DynGen_UnmappedRecLUTPage()
				// reports only an invalid dispatcher target as unmapped. A mapped block
				// which reaches Vita's owner-parity analysis ceiling or crosses the
				// bounded executable-source proof is a different, explicitly tracked
				// backend limitation and must never masquerade as that guest target.
				const char* message = "Invalid IOP recompiler analysis request";
				switch (scan_status)
				{
					case BlockScanStatus::InvalidStart:
						message = (requested_pc & 3u) != 0 ? "Jump to unaligned IOP recompiler address" : message;
						break;
					case BlockScanStatus::UnmappedStart:
						message = "Jump to unmapped IOP recompiler page";
						break;
					case BlockScanStatus::AnalysisCeiling:
						message = "IOP recompiler block exceeded the PCSX2 BaseBlock analysis "
								  "ceiling";
						break;
					case BlockScanStatus::SourceBoundary:
						message = "IOP recompiler block crossed an unowned executable-source "
								  "boundary";
						break;
					case BlockScanStatus::Success:
						break;
				}
				Host::ReportErrorAsync("R3000A Exception", message);
				VMManager::SetPaused(true);
				Cpu->ExitExecution();
				if (execution_terminated)
					*execution_terminated = true;
				return psxRegs.iopBreak + psxRegs.iopCycleEE;
			}
			opcodes.resize(scan.instruction_count);
			for (u32 i = 0; i < scan.instruction_count; i++)
			{
				const u32* const op = ResolveIopRecOpcode(start_pc + i * sizeof(u32));
				if (!op)
					pxFailRel("Forced IOP recompiler continuation lost source ownership");
				opcodes[i] = *op;
			}
		}
		if (opcodes.size() != scan.instruction_count)
			pxFailRel("Retained IOP BaseBlock snapshot size mismatch");
#if defined(VITASX2_QEMU_VALIDATION)
		// The private dispatcher bypasses VitaCpuProviders.cpp's legacy detailed
		// result path, so its exact recompiler fallbacks used to publish only one
		// aggregate count. Keep a cold per-source-descriptor profile at this seam
		// and publish only its bounded top 16; the source hash prevents an SMC/IRX
		// overlay at the same PC from inheriting stale owner metadata. It names the
		// owning SYSCALL/BREAK/RFE word when present and leaves a zero owner for
		// allocation, scan, or compile failures. This makes an all-native failure
		// actionable without adding work to generated IOP blocks.
		u64 source_hash = 1469598103934665603ull;
		const auto mix_source_word = [&source_hash](u32 word) {
			source_hash ^= word;
			source_hash *= 1099511628211ull;
		};
		mix_source_word(start_pc);
		mix_source_word(scan.instruction_count);
		for (const u32 opcode : opcodes)
			mix_source_word(opcode);
		InterpreterFallbackProfileEntry& fallback_profile =
			m_interpreter_fallback_pc_profile[source_hash];
		if (fallback_profile.hits == 0)
		{
			fallback_profile.start_pc = start_pc;
			fallback_profile.instruction_count = scan.instruction_count;
			fallback_profile.source_hash = source_hash;
			for (u32 i = 0; i < scan.instruction_count; i++)
			{
				if (!IsIopExceptionOpcode(opcodes[i]) &&
					!IsIopCop0RfeOpcode(opcodes[i]))
				{
					continue;
				}
				fallback_profile.owner_pc = start_pc + i * sizeof(u32);
				fallback_profile.owner_opcode = opcodes[i];
				break;
			}
		}
		fallback_profile.hits++;
#endif
		psxRegs.pc = start_pc;
		const bool ps1_clock = (psxHu32(HW_ICFG) & (1u << 3)) != 0;

		const u32 hardware_pc = start_pc & 0x1fffffffu;
		if (CompiledPs1BiosGateEnabled() && (psxHu32(HW_ICFG) & 8u) != 0 &&
			(hardware_pc == 0xa0u || hardware_pc == 0xb0u || hardware_pc == 0xc0u) &&
			psxBiosCall())
		{
			if (logical_continuation)
				*logical_continuation = true;
			return psxRegs.iopBreak + psxRegs.iopCycleEE;
		}

		u32 executed_instruction_count = 0;
		const VitaIopInterpreterRecompilerExit interpreter_exit =
			VitaExecuteIopInterpreterRecompilerBlock(
				opcodes.data(), scan.instruction_count, &executed_instruction_count);
		if (interpreter_exit == VitaIopInterpreterRecompilerExit::Failed)
			pxFailRel("Exact IOP recompiler interpreter fallback violated its scan");

		bool isolate_variants_enabled = true;
#if defined(VITASX2_QEMU_VALIDATION)
		isolate_variants_enabled = s_qemuIopIsolateCacheSpecializationEnabled;
#endif
		const bool new_mode =
			isolate_variants_enabled && (psxRegs.CP0.n.Status & 0x10000u) != 0;
		if (new_mode != m_active_isolate_cache_mode)
		{
			ClearWaitResumeBlock();
			ClearSchedulerDirectResume();
			ClearSchedulerPredictedResume();
			m_active_isolate_cache_mode = new_mode;
		}

		if (interpreter_exit == VitaIopInterpreterRecompilerExit::IrxHandled)
		{
			// psxRecompileIrxImport() returns directly to the dispatcher without
			// publishing or charging the private block-cycle accumulator.
			if (logical_continuation)
				*logical_continuation = true;
			return psxRegs.iopBreak + psxRegs.iopCycleEE;
		}
		if (interpreter_exit == VitaIopInterpreterRecompilerExit::Breakpoint)
		{
			if (execution_terminated)
				*execution_terminated = true;
			return psxRegs.iopBreak + psxRegs.iopCycleEE;
		}
		if (interpreter_exit == VitaIopInterpreterRecompilerExit::UnalignedTarget)
		{
			// PCSX2 owner: x86/iR3000A.cpp::psxSetBranchReg() reports and
			// exits before flushing its private block cycles or charging budget.
			Host::ReportErrorAsync("R3000A Exception",
				"Jump to unaligned IOP recompiler address");
			VMManager::SetPaused(true);
			Cpu->ExitExecution();
			if (execution_terminated)
				*execution_terminated = true;
			return psxRegs.iopBreak + psxRegs.iopCycleEE;
		}

		u32 block_cycles = 0;
		for (u32 i = 0; i < executed_instruction_count; i++)
			block_cycles += IopRecompilerInstructionCycles(opcodes[i]);
		psxRegs.cycle += block_cycles;
		ChargeIopRecompilerEeBudget(block_cycles, ps1_clock);

		const bool exception_exit =
			interpreter_exit == VitaIopInterpreterRecompilerExit::ExceptionHandled;
		if (scan.logical_continuation || exception_exit)
		{
			if (logical_continuation)
				*logical_continuation = true;
			return psxRegs.iopBreak + psxRegs.iopCycleEE;
		}

#if defined(VITASX2_QEMU_VALIDATION)
		s_qemuIopBranchEventCandidates++;
#endif
		// PCSX2 iPsxBranchTest() performs the signed budget exit before even
		// looking at the event deadline. This is the ordering which motivated
		// the exact forced path in the first place.
		if (psxRegs.iopCycleEE > 0)
		{
#if defined(VITASX2_QEMU_VALIDATION)
			s_qemuIopBranchEventBudgetPositive++;
#endif
			if (static_cast<s64>(psxRegs.cycle - psxRegs.iopNextEventCycle) >= 0)
			{
#if defined(VITASX2_QEMU_VALIDATION)
				s_qemuIopBranchEventTestsEntered++;
#endif
				iopEventTest();
			}
		}

		return psxRegs.iopBreak + psxRegs.iopCycleEE;
	}

	inline __attribute__((always_inline)) s32
	BlockExecutor::ExecuteProviderTimesliceLoop()
	{
		const bool compiled_ps1_bios_gate = CompiledPs1BiosGateEnabled();
		bool first_dispatch = true;
		for (;;)
		{
			const bool forced_continuation =
				std::exchange(m_force_logical_continuation, false);
			// PCSX2 owner: x86/iR3000A.cpp::recExecuteBlock() enters
			// _DynGen_EnterRecompiledCode() unconditionally. The generated
			// dispatcher has no budget gate before its first BaseBlock; only the
			// completed block's iPsxBranchTest() may return to the EE scheduler.
			if ((!first_dispatch || VitaIsIopPreInstructionTraceEnabled()) &&
				!forced_continuation && psxRegs.iopCycleEE <= 0)
				break;
			first_dispatch = false;
			if (!compiled_ps1_bios_gate && (psxHu32(HW_ICFG) & 8) &&
				((psxRegs.pc & 0x1fffffffu) == 0xa0 ||
				 (psxRegs.pc & 0x1fffffffu) == 0xb0 ||
				 (psxRegs.pc & 0x1fffffffu) == 0xc0))
			{
				psxBiosCall();
			}
#if defined(VITASX2_QEMU_VALIDATION) && \
	!defined(VITASX2_IOP_COMPILED_PS1_BIOS_GATE_PRODUCT)
			if (compiled_ps1_bios_gate)
				m_dispatcher_ps1_bios_gate_checks_removed++;
#endif

			u32 dispatch_flags = 0;
#if defined(VITASX2_QEMU_VALIDATION)
			// The direct scheduler entry is the product path. Keep the preceding
			// in-body resume cache as the matched attribution control.
			CachedBlock* const wait_resume = m_wait_resume_block;
			bool resume_match =
				wait_resume &&
				RecLookupIdentity(psxRegs.pc) == wait_resume->rec_lookup_identity &&
				wait_resume->isolate_cache_active == m_active_isolate_cache_mode;
			if (!s_qemuIopWaitResumeCacheEnabled)
				resume_match = false;
			if (wait_resume && s_qemuIopWaitResumeCacheEnabled)
				m_wait_resume_cache_attempts++;
			if (resume_match)
			{
				resume_match = wait_resume->valid && wait_resume->raw_opcodes;
				if (resume_match)
				{
					m_hot_dispatch_trusted_raw_hits++;
					resume_match = ValidateCachedBlock(*wait_resume);
				}
			}
			if (resume_match)
			{
				m_wait_resume_cache_hits++;
				m_private_dispatcher_inlined_hot_entries++;
				dispatch_flags = RunProviderBlockInline(*wait_resume,
					ProviderDispatchCacheHit |
						ProviderDispatchLookupHit |
						ProviderDispatchFastHit,
					forced_continuation);
			}
			else
			{
				if (wait_resume)
				{
					if (s_qemuIopWaitResumeCacheEnabled)
						m_wait_resume_cache_misses++;
					ClearWaitResumeBlock();
				}
				CachedBlock* entry = FindSchedulerDirectResumeBlock(&dispatch_flags);
				if (!entry)
				{
					entry =
						FindProviderBlockAtPcInline(psxRegs.pc, nullptr, &dispatch_flags);
					if (entry)
						SetSchedulerPredictedResumeBlock(entry);
				}
				if (entry)
				{
					dispatch_flags =
						s_qemuIopPrivateDispatcherHotPathEnabled ? RunProviderBlockInline(*entry, dispatch_flags,
																	   forced_continuation) :
																   RunProviderBlock(*entry, dispatch_flags, forced_continuation);
				}
			}
#else
			CachedBlock* entry = FindSchedulerDirectResumeBlock(&dispatch_flags);
			if (!entry)
			{
				entry = FindProviderBlockAtPcInline(psxRegs.pc, nullptr, &dispatch_flags);
				if (entry)
					SetSchedulerPredictedResumeBlock(entry);
			}
			if (entry)
			{
				dispatch_flags =
					RunProviderBlockInline(*entry, dispatch_flags, forced_continuation);
			}
#endif
			if ((dispatch_flags & ProviderDispatchSuccess) == 0)
			{
#if defined(VITASX2_QEMU_VALIDATION)
				m_private_dispatcher_fallbacks++;
#endif
				ClearSchedulerDirectResume();
				if (VitaIsIopPreInstructionTraceEnabled())
				return ExecuteInterpreterFallbackTimeslice(psxRegs.iopCycleEE);

				bool interpreter_logical_continuation = false;
				bool interpreter_execution_terminated = false;
				const s32 result = ExecuteInterpreterRecompilerBlock(
					psxRegs.iopCycleEE, &interpreter_logical_continuation,
					&interpreter_execution_terminated);
				if (interpreter_execution_terminated)
					return result;
				if (interpreter_logical_continuation)
				{
					m_force_logical_continuation = true;
					continue;
			}
				if (psxRegs.iopCycleEE > 0)
					continue;
				return result;
			}
			if ((dispatch_flags & ProviderDispatchLogicalContinuation) != 0)
				m_force_logical_continuation = true;
#if defined(VITASX2_QEMU_VALIDATION)
			m_private_dispatcher_provider_entries++;
#endif
			if ((dispatch_flags & ProviderDispatchWaitForward) != 0)
			{
#if defined(VITASX2_QEMU_VALIDATION)
				m_private_dispatcher_wait_forwards++;
#endif
				continue;
			}
#if defined(VITASX2_QEMU_VALIDATION)
			m_private_dispatcher_generated_entries++;
#endif
		}

		return psxRegs.iopBreak + psxRegs.iopCycleEE;
	}

	s32 BlockExecutor::ExecuteProviderTimesliceRemainder()
	{
		return ExecuteProviderTimesliceLoop();
	}

#if defined(__arm__)
	s32 BlockExecutor::ExecuteProviderTimeslice(s32)
	{
		asm volatile(
			// Cold AAPCS adapter for diagnostics and non-private callers.
			// This translation unit reserves d8-d15, generated IOP code uses q0,
			// and nested helpers obey AAPCS. The target therefore preserves the
			// callee-saved bank without an adapter-local 128-byte stack transfer.
			"push {r4-r11, lr}\n"
			"sub sp, sp, #4\n"
			"bl VitaIopA32ExecuteProviderTimeslicePrivate\n"
			"add sp, sp, #4\n"
			"pop {r4-r11, pc}\n");
	}

	s32 BlockExecutor::ExecuteProviderTimeslicePrivateBody(s32 ee_cycles)
#else
	s32 BlockExecutor::ExecuteProviderTimeslice(s32 ee_cycles)
#endif
	{
		// Force one stable first instruction for the verified private entry.
		// -fno-pie prevents a GOT literal setup from preceding this AAPCS save.
#if defined(__arm__)
		asm volatile("" ::: "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "lr");
#endif
		// PCSX2 owner: x86/iR3000A.cpp::_DynGen_EnterRecompiledCode() keeps
		// lookup, generated entry, wait forwarding, and the timeslice return in
		// one private dispatcher. Keeping this loop beside the cache implementation
		// lets Cortex-A9 inline the provider lookup instead of crossing AAPCS once
		// per generated block or wait forward.
		psxRegs.iopBreak = 0;
		psxRegs.iopCycleEE = ee_cycles;
		m_force_logical_continuation = false;
#if defined(VITASX2_QEMU_VALIDATION)
		m_private_dispatcher_calls++;
#endif
		const s32 result = ExecuteProviderTimesliceLoop();
#if defined(__arm__)
		ReturnFromPrivateProviderTimeslice(result);
#else
		return result;
#endif
	}

#if defined(__arm__)
	s32 BlockExecutor::ExecuteProviderSchedulerDirectResumePrivateBody(
		s32 ee_cycles, CachedBlock* block)
	{
		asm volatile("" ::: "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "lr");
		psxRegs.iopBreak = 0;
		psxRegs.iopCycleEE = ee_cycles;
		m_force_logical_continuation = false;
#if defined(VITASX2_QEMU_VALIDATION)
		m_private_dispatcher_calls++;
		m_scheduler_direct_event_entries++;
#endif

		bool resume_match = true;
#if defined(VITASX2_QEMU_VALIDATION) && \
	!defined(VITASX2_IOP_SCHEDULER_DIRECT_EVENT_CODEGEN)
		m_scheduler_direct_resume_attempts++;
		resume_match = SchedulerDirectResumeEnabled() && block &&
		               block == m_scheduler_direct_resume_event_context.block &&
		               block->valid &&
		               block->rec_lookup_identity == RecLookupIdentity(psxRegs.pc) &&
			block->isolate_cache_active == m_active_isolate_cache_mode;
		if (resume_match && s_qemuIopTrustedSourceAuditEnabled &&
			block->trusted_source)
		{
			m_hot_dispatch_trusted_raw_hits++;
			resume_match = ValidateCachedBlock(*block);
		}
		if (resume_match)
			m_scheduler_direct_resume_hits++;
		else
			m_scheduler_direct_resume_misses++;
#else
		if (!block)
			__builtin_unreachable();
#endif
		if (!resume_match)
		{
#if defined(VITASX2_QEMU_VALIDATION)
			m_scheduler_direct_event_fallbacks++;
#endif
			ClearSchedulerDirectResume();
			const s32 result = ExecuteProviderTimesliceRemainder();
			ReturnFromPrivateProviderTimeslice(result);
		}

#if defined(VITASX2_QEMU_VALIDATION)
		m_private_dispatcher_inlined_hot_entries++;
		if (CompiledPs1BiosGateEnabled())
			m_dispatcher_ps1_bios_gate_checks_removed++;
#endif
		const u32 dispatch_flags = RunProviderBlockInline(
			*block, ProviderDispatchCacheHit | ProviderDispatchLookupHit |
				ProviderDispatchFastHit);
		if ((dispatch_flags & ProviderDispatchSuccess) == 0)
		{
#if defined(VITASX2_QEMU_VALIDATION)
			m_private_dispatcher_fallbacks++;
			m_scheduler_direct_event_fallbacks++;
#endif
			ClearSchedulerDirectResume();
			ClearSchedulerPredictedResume();
			const s32 result = ExecuteProviderTimesliceRemainder();
			ReturnFromPrivateProviderTimeslice(result);
		}

#if defined(VITASX2_QEMU_VALIDATION)
		m_private_dispatcher_provider_entries++;
		m_scheduler_direct_event_forwards++;
		if ((dispatch_flags & ProviderDispatchWaitForward) != 0)
			m_private_dispatcher_wait_forwards++;
		else
			m_private_dispatcher_generated_entries++;
#endif
		if ((dispatch_flags & ProviderDispatchLogicalContinuation) != 0)
			m_force_logical_continuation = true;
		if (m_force_logical_continuation || psxRegs.iopCycleEE > 0)
		{
#if defined(VITASX2_QEMU_VALIDATION)
			m_scheduler_direct_event_remainders++;
#endif
			const s32 result = ExecuteProviderTimesliceRemainder();
			ReturnFromPrivateProviderTimeslice(result);
		}

		ReturnFromPrivateProviderTimeslice(psxRegs.iopBreak + psxRegs.iopCycleEE);
	}

	s32 BlockExecutor::ExecuteProviderSchedulerPredictedResumePrivateBody(
		s32 ee_cycles, CachedBlock* block)
	{
		asm volatile("" ::: "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "lr");
		psxRegs.iopBreak = 0;
		psxRegs.iopCycleEE = ee_cycles;
		m_force_logical_continuation = false;
#if defined(VITASX2_QEMU_VALIDATION)
		m_private_dispatcher_calls++;
		m_scheduler_prediction_attempts++;
#endif

#if defined(VITASX2_QEMU_VALIDATION)
		const u32 current_guest_pc = psxRegs.pc;
		const bool first_way_match =
			block == m_scheduler_direct_resume_event_context.predicted_block &&
			current_guest_pc == m_scheduler_direct_resume_event_context.predicted_pc;
		const bool second_way_match =
			block == m_scheduler_direct_resume_event_context.predicted_block_second &&
			current_guest_pc ==
				m_scheduler_direct_resume_event_context.predicted_pc_second;
		const u32 current_rec_lookup_identity = RecLookupIdentity(current_guest_pc);
		bool prediction_match =
			block && block->valid && (first_way_match || second_way_match) &&
			block->rec_lookup_identity == current_rec_lookup_identity &&
			block->isolate_cache_active == m_active_isolate_cache_mode;
		if (prediction_match && s_qemuIopTrustedSourceAuditEnabled &&
			block->trusted_source)
		{
			m_hot_dispatch_trusted_raw_hits++;
			prediction_match = ValidateCachedBlock(*block);
		}
		bool four_way_match = prediction_match;
		for (u32 i = 2; i < m_scheduler_prediction_shadow.size(); i++)
		{
			CachedBlock* const predicted = m_scheduler_prediction_shadow[i];
			const bool match =
				predicted && predicted->valid &&
				predicted->rec_lookup_identity == current_rec_lookup_identity &&
				predicted->isolate_cache_active == m_active_isolate_cache_mode;
			four_way_match = four_way_match || match;
		}
		if (prediction_match)
			m_scheduler_prediction_two_way_hits++;
		if (four_way_match)
			m_scheduler_prediction_four_way_hits++;
#else
		if (!block)
			__builtin_unreachable();
		constexpr bool prediction_match = true;
#endif
		if (!prediction_match)
		{
#if defined(VITASX2_QEMU_VALIDATION)
			m_scheduler_prediction_misses++;
			m_scheduler_prediction_fallbacks++;
#endif
			const s32 result = ExecuteProviderTimesliceRemainder();
			ReturnFromPrivateProviderTimeslice(result);
		}

#if defined(VITASX2_QEMU_VALIDATION)
		if (first_way_match)
			m_scheduler_prediction_hits++;
		m_private_dispatcher_inlined_hot_entries++;
		if (CompiledPs1BiosGateEnabled())
			m_dispatcher_ps1_bios_gate_checks_removed++;
#endif
		const u32 dispatch_flags = RunProviderBlockInline(
			*block, ProviderDispatchCacheHit | ProviderDispatchLookupHit |
				ProviderDispatchFastHit);
		if ((dispatch_flags & ProviderDispatchSuccess) == 0)
		{
#if defined(VITASX2_QEMU_VALIDATION)
			m_private_dispatcher_fallbacks++;
			m_scheduler_prediction_fallbacks++;
#endif
			ClearSchedulerDirectResume();
			ClearSchedulerPredictedResume();
			const s32 result = ExecuteProviderTimesliceRemainder();
			ReturnFromPrivateProviderTimeslice(result);
		}

#if defined(VITASX2_QEMU_VALIDATION)
		m_private_dispatcher_provider_entries++;
		m_scheduler_prediction_forwards++;
		if ((dispatch_flags & ProviderDispatchWaitForward) != 0)
			m_private_dispatcher_wait_forwards++;
		else
			m_private_dispatcher_generated_entries++;
#endif
		if ((dispatch_flags & ProviderDispatchLogicalContinuation) != 0)
			m_force_logical_continuation = true;
		if (m_force_logical_continuation || psxRegs.iopCycleEE > 0)
		{
#if defined(VITASX2_QEMU_VALIDATION)
			m_scheduler_prediction_remainders++;
#endif
			const s32 result = ExecuteProviderTimesliceRemainder();
			ReturnFromPrivateProviderTimeslice(result);
		}

		ReturnFromPrivateProviderTimeslice(psxRegs.iopBreak + psxRegs.iopCycleEE);
	}

	s32 BlockExecutor::ExecuteProviderSchedulerDispatchCachedResumePrivateBody(
		s32 ee_cycles, CachedBlock* block)
	{
		asm volatile("" ::: "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "lr");
		psxRegs.iopBreak = 0;
		psxRegs.iopCycleEE = ee_cycles;
		m_force_logical_continuation = false;
#if defined(VITASX2_QEMU_VALIDATION) && \
	!defined(VITASX2_IOP_SCHEDULER_DISPATCH_CACHE_CODEGEN)
		m_private_dispatcher_calls++;
		m_scheduler_dispatch_cache_attempts++;
#endif

#if defined(VITASX2_QEMU_VALIDATION) && \
	!defined(VITASX2_IOP_SCHEDULER_DISPATCH_CACHE_CODEGEN)
		const u32 current_guest_pc = psxRegs.pc;
		const u32 current_rec_lookup_identity = RecLookupIdentity(current_guest_pc);
		const u32 cache_index = (current_guest_pc >> 2) &
		                        (SchedulerDispatchCacheEntryCount() - 1);
		const auto& cache_entry =
			m_scheduler_direct_resume_event_context.dispatch_cache[cache_index];
		bool cache_match =
			block && block == cache_entry.block && block->valid &&
			cache_entry.guest_pc == current_guest_pc &&
			block->rec_lookup_identity == current_rec_lookup_identity &&
			block->isolate_cache_active == m_active_isolate_cache_mode;
		if (cache_match && s_qemuIopTrustedSourceAuditEnabled &&
			block->trusted_source)
		{
			m_hot_dispatch_trusted_raw_hits++;
			cache_match = ValidateCachedBlock(*block);
		}
		if (cache_match)
			m_scheduler_dispatch_cache_hits++;
		else
			m_scheduler_dispatch_cache_misses++;
#else
		if (!block)
			__builtin_unreachable();
		constexpr bool cache_match = true;
#endif
		if (!cache_match)
		{
#if defined(VITASX2_QEMU_VALIDATION) && \
	!defined(VITASX2_IOP_SCHEDULER_DISPATCH_CACHE_CODEGEN)
			m_scheduler_dispatch_cache_fallbacks++;
#endif
			const s32 result = ExecuteProviderTimesliceRemainder();
			ReturnFromPrivateProviderTimeslice(result);
		}

		// A PC-indexed hit is also the newest register-dispatch target. Refresh
		// the cheaper two-way front so alternating/local patterns return to its
		// shorter route instead of repeatedly probing the indexed tier.
		SetSchedulerPredictedResumeBlock(block);

#if defined(VITASX2_QEMU_VALIDATION) && \
	!defined(VITASX2_IOP_SCHEDULER_DISPATCH_CACHE_CODEGEN)
		m_private_dispatcher_inlined_hot_entries++;
		if (CompiledPs1BiosGateEnabled())
			m_dispatcher_ps1_bios_gate_checks_removed++;
#endif
		const u32 dispatch_flags = RunProviderBlockInline(
			*block, ProviderDispatchCacheHit | ProviderDispatchLookupHit |
				ProviderDispatchFastHit);
		if ((dispatch_flags & ProviderDispatchSuccess) == 0)
		{
#if defined(VITASX2_QEMU_VALIDATION) && \
	!defined(VITASX2_IOP_SCHEDULER_DISPATCH_CACHE_CODEGEN)
			m_private_dispatcher_fallbacks++;
			m_scheduler_dispatch_cache_fallbacks++;
#endif
			ClearSchedulerDirectResume();
			ClearSchedulerPredictedResume();
			const s32 result = ExecuteProviderTimesliceRemainder();
			ReturnFromPrivateProviderTimeslice(result);
		}

#if defined(VITASX2_QEMU_VALIDATION) && \
	!defined(VITASX2_IOP_SCHEDULER_DISPATCH_CACHE_CODEGEN)
		m_private_dispatcher_provider_entries++;
		m_scheduler_dispatch_cache_forwards++;
		if ((dispatch_flags & ProviderDispatchWaitForward) != 0)
			m_private_dispatcher_wait_forwards++;
		else
			m_private_dispatcher_generated_entries++;
#endif
		if ((dispatch_flags & ProviderDispatchLogicalContinuation) != 0)
			m_force_logical_continuation = true;
		if (m_force_logical_continuation || psxRegs.iopCycleEE > 0)
		{
#if defined(VITASX2_QEMU_VALIDATION) && \
	!defined(VITASX2_IOP_SCHEDULER_DISPATCH_CACHE_CODEGEN)
			m_scheduler_dispatch_cache_remainders++;
#endif
			const s32 result = ExecuteProviderTimesliceRemainder();
			ReturnFromPrivateProviderTimeslice(result);
		}

		ReturnFromPrivateProviderTimeslice(psxRegs.iopBreak + psxRegs.iopCycleEE);
	}

	inline __attribute__((always_inline)) s32
	BlockExecutor::ExecuteProviderWaitResumePrivateBodyCore(
		s32 ee_cycles, CachedBlock* block, WaitResumeKind kind, bool kind_specific,
		bool clock_specific, bool ps1_clock, bool no_link_specific)
	{
		asm volatile("" ::: "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "lr");
		psxRegs.iopBreak = 0;
		psxRegs.iopCycleEE = ee_cycles;
		m_force_logical_continuation = false;
#if defined(VITASX2_QEMU_VALIDATION)
		m_private_dispatcher_calls++;
		m_wait_resume_event_entries++;
		if (kind_specific)
			m_wait_resume_kind_specific_entries++;
		if (clock_specific)
			m_wait_resume_clock_specific_entries++;
		if (no_link_specific)
			m_wait_resume_no_link_specific_entries++;
#endif

		const auto resume_context_matches = [&](bool identity_owned) {
			bool resume_match =
				identity_owned ||
				(block && block == m_wait_resume_block &&
					RecLookupIdentity(psxRegs.pc) == block->rec_lookup_identity &&
					block->isolate_cache_active == m_active_isolate_cache_mode);
#if defined(VITASX2_QEMU_VALIDATION)
			if (!s_qemuIopWaitResumeCacheEnabled)
				resume_match = false;
			m_wait_resume_cache_attempts++;
			if (resume_match)
			{
				// Product ownership makes the first identity exact. Keep QEMU's
				// deliberate raw-source audit without reintroducing the product
				// block/PC/isolate comparisons being measured here.
				resume_match = block && block->valid && block->raw_opcodes;
				if (resume_match)
				{
					m_hot_dispatch_trusted_raw_hits++;
					resume_match = ValidateCachedBlock(*block);
				}
			}
			if (resume_match)
				m_wait_resume_cache_hits++;
			else
				m_wait_resume_cache_misses++;
#endif
			return resume_match;
		};

#if defined(VITASX2_IOP_WAIT_RESUME_FIRST_ENTRY_CONTROL)
		constexpr bool trust_first_entry = false;
#elif defined(VITASX2_QEMU_VALIDATION)
		const bool trust_first_entry = s_qemuIopWaitResumeFirstEntryOwnershipEnabled;
#else
		constexpr bool trust_first_entry = true;
#endif
#if !defined(VITASX2_QEMU_VALIDATION) && \
	!defined(VITASX2_IOP_WAIT_RESUME_FIRST_ENTRY_CONTROL)
		// The selected scheduler context cannot name the wait entry without its
		// retained block. Preserve that fact for register allocation after the
		// product path deliberately removes the redundant null test.
		if (!block)
			__builtin_unreachable();
#endif
		if (!trust_first_entry && !resume_context_matches(false))
		{
#if defined(VITASX2_QEMU_VALIDATION)
			m_wait_resume_event_fallbacks++;
#endif
			ClearWaitResumeBlock();
			const s32 result = ExecuteProviderTimesliceRemainder();
			ReturnFromPrivateProviderTimeslice(result);
		}
#if defined(VITASX2_QEMU_VALIDATION)
		if (trust_first_entry)
		{
			m_wait_resume_first_entry_owned++;
			if (!resume_context_matches(true))
			{
				m_wait_resume_event_fallbacks++;
				ClearWaitResumeBlock();
				const s32 result = ExecuteProviderTimesliceRemainder();
				ReturnFromPrivateProviderTimeslice(result);
			}
		}
#endif

		for (;;)
		{
#if defined(VITASX2_QEMU_VALIDATION)
			m_private_dispatcher_inlined_hot_entries++;
#endif
			bool wait_forward = false;
#if defined(VITASX2_QEMU_VALIDATION)
			bool control_provider_executed = false;
#endif
#if defined(VITASX2_IOP_WAIT_RESUME_DESCRIPTOR_CONTROL)
			constexpr bool use_descriptor_specialization = false;
#elif defined(VITASX2_QEMU_VALIDATION)
			const bool use_descriptor_specialization =
				s_qemuIopWaitResumeDescriptorSpecializationEnabled;
#else
			constexpr bool use_descriptor_specialization = true;
#endif
			if (!use_descriptor_specialization)
			{
				const u32 dispatch_flags = RunProviderBlockInline(
					*block, ProviderDispatchCacheHit | ProviderDispatchLookupHit |
					ProviderDispatchFastHit);
#if defined(VITASX2_QEMU_VALIDATION)
				control_provider_executed =
					(dispatch_flags & ProviderDispatchSuccess) != 0;
#endif
				wait_forward = (dispatch_flags & (ProviderDispatchSuccess |
													 ProviderDispatchWaitForward)) ==
					(ProviderDispatchSuccess | ProviderDispatchWaitForward);
			}
			else
			{
				if (kind_specific)
				{
					switch (kind)
					{
						case WaitResumeKind::Unconditional:
							if (clock_specific)
							{
								if (no_link_specific)
								{
									if (ps1_clock)
										FastForwardRetainedUnconditionalNoLinkWaitLoopForClock<true>(
											*block);
									else
										FastForwardRetainedUnconditionalNoLinkWaitLoopForClock<false>(
											*block);
								}
								else if (ps1_clock)
								{
									FastForwardRetainedUnconditionalWaitLoopForClock<true>(*block);
								}
								else
								{
									FastForwardRetainedUnconditionalWaitLoopForClock<false>(*block);
								}
							}
							else
							{
								FastForwardRetainedUnconditionalWaitLoop(*block);
							}
							wait_forward = true;
							break;
						case WaitResumeKind::PollCall:
							if (clock_specific)
							{
								wait_forward =
									ps1_clock ? TryFastForwardPollCallWaitLoopForClock<true>(*block) : TryFastForwardPollCallWaitLoopForClock<false>(*block);
							}
							else
							{
								wait_forward = TryFastForwardPollCallWaitLoop(*block);
							}
							break;
						case WaitResumeKind::Conditional:
							if (clock_specific)
							{
								wait_forward =
									ps1_clock ? TryFastForwardCachedWaitLoopForClock<1>(*block) : TryFastForwardCachedWaitLoopForClock<0>(*block);
							}
							else
							{
								wait_forward = TryFastForwardCachedWaitLoop(*block);
							}
							break;
						case WaitResumeKind::Invalid:
							break;
					}
				}
				else
				{
					wait_forward = TryFastForwardRetainedWaitLoop(*block, kind);
				}
				if (wait_forward)
				{
#if defined(VITASX2_PORTABLE_REPLAY_VALIDATION) || \
	defined(VITASX2_PRODUCT_BOOT_VALIDATION)
					if (m_portable_validation_stats.native_provider_entries != UINT32_MAX)
						m_portable_validation_stats.native_provider_entries++;
#endif
#if defined(VITASX2_QEMU_VALIDATION)
					m_wait_resume_descriptor_forwards++;
					if (clock_specific)
						m_wait_resume_clock_specific_forwards++;
					if (no_link_specific)
						m_wait_resume_no_link_specific_forwards++;
					switch (kind)
					{
						case WaitResumeKind::Unconditional:
							m_wait_resume_unconditional_forwards++;
							if (kind_specific)
								m_wait_resume_kind_specific_unconditional_forwards++;
							break;
						case WaitResumeKind::PollCall:
							m_wait_resume_poll_forwards++;
							if (kind_specific)
								m_wait_resume_kind_specific_poll_forwards++;
							break;
						case WaitResumeKind::Conditional:
							m_wait_resume_conditional_forwards++;
							if (kind_specific)
								m_wait_resume_kind_specific_conditional_forwards++;
							break;
						case WaitResumeKind::Invalid:
							break;
					}
					m_pinned_gpr_memory_ops_saved += block->pinned_gpr_memory_ops_saved;
					m_pinned_branch_operand_moves_removed +=
						block->pinned_branch_operand_moves_removed;
					m_condition_code_branch_instructions_removed +=
						block->condition_code_branch_instructions_removed;
#endif
				}
			}
			if (!wait_forward)
			{
#if defined(VITASX2_QEMU_VALIDATION)
				if (control_provider_executed)
				{
					m_private_dispatcher_provider_entries++;
					m_private_dispatcher_generated_entries++;
				}
				m_wait_resume_event_fallbacks++;
#endif
				ClearWaitResumeBlock();
				const s32 result = ExecuteProviderTimesliceRemainder();
				ReturnFromPrivateProviderTimeslice(result);
			}
#if defined(VITASX2_QEMU_VALIDATION)
			m_private_dispatcher_provider_entries++;
			m_private_dispatcher_wait_forwards++;
			m_wait_resume_event_forwards++;
#endif
			if (psxRegs.iopCycleEE <= 0)
				break;

			// FastForwardCachedIopWaitLoop() reaches a positive remaining budget
			// only after calling PCSX2's iopEventTest(). That event can change PC,
			// isolate mode, source ownership, or the retained scheduler context, so
			// preserve the complete identity check before another descriptor use.
#if defined(VITASX2_QEMU_VALIDATION)
			m_wait_resume_post_event_identity_checks++;
#endif
			if (!resume_context_matches(false))
			{
#if defined(VITASX2_QEMU_VALIDATION)
				m_wait_resume_event_fallbacks++;
#endif
				ClearWaitResumeBlock();
				const s32 result = ExecuteProviderTimesliceRemainder();
				ReturnFromPrivateProviderTimeslice(result);
			}
		}

		ReturnFromPrivateProviderTimeslice(psxRegs.iopBreak + psxRegs.iopCycleEE);
	}

#if defined(VITASX2_QEMU_VALIDATION) || \
	defined(VITASX2_IOP_WAIT_RESUME_KIND_ENTRY_CONTROL)
	s32 BlockExecutor::ExecuteProviderWaitResumePrivateBody(s32 ee_cycles,
		CachedBlock* block,
		WaitResumeKind kind)
	{
		return ExecuteProviderWaitResumePrivateBodyCore(ee_cycles, block, kind, false,
			false, false, false);
	}
#endif

#if defined(VITASX2_QEMU_VALIDATION) || \
	defined(VITASX2_IOP_WAIT_RESUME_CLOCK_ENTRY_CONTROL)
	s32 BlockExecutor::ExecuteProviderWaitResumeUnconditionalPrivateBody(
		s32 ee_cycles, CachedBlock* block)
	{
		return ExecuteProviderWaitResumePrivateBodyCore(ee_cycles, block,
			WaitResumeKind::Unconditional,
			true, false, false, false);
	}

	s32 BlockExecutor::ExecuteProviderWaitResumePollPrivateBody(
		s32 ee_cycles, CachedBlock* block)
	{
		return ExecuteProviderWaitResumePrivateBodyCore(
			ee_cycles, block, WaitResumeKind::PollCall, true, false, false, false);
	}

	s32 BlockExecutor::ExecuteProviderWaitResumeConditionalPrivateBody(
		s32 ee_cycles, CachedBlock* block)
	{
		return ExecuteProviderWaitResumePrivateBodyCore(
			ee_cycles, block, WaitResumeKind::Conditional, true, false, false, false);
	}
#endif

	s32 BlockExecutor::ExecuteProviderWaitResumeUnconditionalNormalPrivateBody(
		s32 ee_cycles, CachedBlock* block)
	{
		return ExecuteProviderWaitResumePrivateBodyCore(ee_cycles, block,
			WaitResumeKind::Unconditional,
			true, true, false, false);
	}

	s32 BlockExecutor::ExecuteProviderWaitResumeUnconditionalPs1PrivateBody(
		s32 ee_cycles, CachedBlock* block)
	{
		return ExecuteProviderWaitResumePrivateBodyCore(
			ee_cycles, block, WaitResumeKind::Unconditional, true, true, true, false);
	}

	s32 BlockExecutor::
		ExecuteProviderWaitResumeUnconditionalNoLinkNormalPrivateBody(
		s32 ee_cycles, CachedBlock* block)
	{
		return ExecuteProviderWaitResumePrivateBodyCore(
			ee_cycles, block, WaitResumeKind::Unconditional, true, true, false, true);
	}

	s32 BlockExecutor::ExecuteProviderWaitResumeUnconditionalNoLinkPs1PrivateBody(
		s32 ee_cycles, CachedBlock* block)
	{
		return ExecuteProviderWaitResumePrivateBodyCore(
			ee_cycles, block, WaitResumeKind::Unconditional, true, true, true, true);
	}

	s32 BlockExecutor::ExecuteProviderWaitResumePollNormalPrivateBody(
		s32 ee_cycles, CachedBlock* block)
	{
		return ExecuteProviderWaitResumePrivateBodyCore(
			ee_cycles, block, WaitResumeKind::PollCall, true, true, false, false);
	}

	s32 BlockExecutor::ExecuteProviderWaitResumePollPs1PrivateBody(
		s32 ee_cycles, CachedBlock* block)
	{
		return ExecuteProviderWaitResumePrivateBodyCore(
			ee_cycles, block, WaitResumeKind::PollCall, true, true, true, false);
	}

	s32 BlockExecutor::ExecuteProviderWaitResumeConditionalNormalPrivateBody(
		s32 ee_cycles, CachedBlock* block)
	{
		return ExecuteProviderWaitResumePrivateBodyCore(
			ee_cycles, block, WaitResumeKind::Conditional, true, true, false, false);
	}

	s32 BlockExecutor::ExecuteProviderWaitResumeConditionalPs1PrivateBody(
		s32 ee_cycles, CachedBlock* block)
	{
		return ExecuteProviderWaitResumePrivateBodyCore(
			ee_cycles, block, WaitResumeKind::Conditional, true, true, true, false);
	}
#endif

#if defined(__arm__)
	extern "C" void VitaIopA32ProviderTimesliceBodySymbol() __asm__(
		"VitaIopA32ProviderTimesliceBody");
	extern "C" void VitaIopA32ProviderSchedulerDirectResumeBodySymbol() __asm__(
		"VitaIopA32ProviderSchedulerDirectResumeBody");
	extern "C" void VitaIopA32ProviderSchedulerPredictedResumeBodySymbol() __asm__(
		"VitaIopA32ProviderSchedulerPredictedResumeBody");
	extern "C" void
	VitaIopA32ProviderSchedulerDispatchCachedResumeBodySymbol() __asm__(
		"VitaIopA32ProviderSchedulerDispatchCachedResumeBody");
#if defined(VITASX2_QEMU_VALIDATION) || \
	defined(VITASX2_IOP_WAIT_RESUME_KIND_ENTRY_CONTROL)
	extern "C" void VitaIopA32ProviderWaitResumeBodySymbol() __asm__(
		"VitaIopA32ProviderWaitResumeBody");
#endif
#if defined(VITASX2_QEMU_VALIDATION) || \
	defined(VITASX2_IOP_WAIT_RESUME_CLOCK_ENTRY_CONTROL)
	extern "C" void VitaIopA32ProviderWaitResumeUnconditionalBodySymbol() __asm__(
		"VitaIopA32ProviderWaitResumeUnconditionalBody");
	extern "C" void VitaIopA32ProviderWaitResumePollBodySymbol() __asm__(
		"VitaIopA32ProviderWaitResumePollBody");
	extern "C" void VitaIopA32ProviderWaitResumeConditionalBodySymbol() __asm__(
		"VitaIopA32ProviderWaitResumeConditionalBody");
#endif
	extern "C" void
	VitaIopA32ProviderWaitResumeUnconditionalNormalBodySymbol() __asm__(
		"VitaIopA32ProviderWaitResumeUnconditionalNormalBody");
	extern "C" void
	VitaIopA32ProviderWaitResumeUnconditionalPs1BodySymbol() __asm__(
		"VitaIopA32ProviderWaitResumeUnconditionalPs1Body");
	extern "C" void
	VitaIopA32ProviderWaitResumeUnconditionalNoLinkNormalBodySymbol() __asm__(
		"VitaIopA32ProviderWaitResumeUnconditionalNoLinkNormalBody");
	extern "C" void
	VitaIopA32ProviderWaitResumeUnconditionalNoLinkPs1BodySymbol() __asm__(
		"VitaIopA32ProviderWaitResumeUnconditionalNoLinkPs1Body");
	extern "C" void VitaIopA32ProviderWaitResumePollNormalBodySymbol() __asm__(
		"VitaIopA32ProviderWaitResumePollNormalBody");
	extern "C" void VitaIopA32ProviderWaitResumePollPs1BodySymbol() __asm__(
		"VitaIopA32ProviderWaitResumePollPs1Body");
	extern "C" void
	VitaIopA32ProviderWaitResumeConditionalNormalBodySymbol() __asm__(
		"VitaIopA32ProviderWaitResumeConditionalNormalBody");
	extern "C" void VitaIopA32ProviderWaitResumeConditionalPs1BodySymbol() __asm__(
		"VitaIopA32ProviderWaitResumeConditionalPs1Body");

	bool VitaIopA32PrivateTimesliceEntrySupported()
	{
		// The private entry skips exactly one forced A32 PUSH. Keep the public
		// AAPCS path if a future compiler/toolchain moves anything ahead of it.
		constexpr u32 EXPECTED_PUSH_R4_R11_LR = 0xe92d4ff0u;
		const auto* const body = reinterpret_cast<const u32*>(
			reinterpret_cast<uptr>(&VitaIopA32ProviderTimesliceBodySymbol));
		return body[0] == EXPECTED_PUSH_R4_R11_LR;
	}

	bool VitaIopA32PrivateSchedulerResumeEntrySupported()
	{
		constexpr u32 EXPECTED_PUSH_R4_R11_LR = 0xe92d4ff0u;
		const auto* const body = reinterpret_cast<const u32*>(reinterpret_cast<uptr>(
				&VitaIopA32ProviderSchedulerDirectResumeBodySymbol));
		return body[0] == EXPECTED_PUSH_R4_R11_LR;
	}

	bool VitaIopA32PrivateSchedulerPredictionEntrySupported()
	{
		constexpr u32 EXPECTED_PUSH_R4_R11_LR = 0xe92d4ff0u;
		const auto* const body = reinterpret_cast<const u32*>(reinterpret_cast<uptr>(
				&VitaIopA32ProviderSchedulerPredictedResumeBodySymbol));
		return body[0] == EXPECTED_PUSH_R4_R11_LR;
	}

	bool VitaIopA32PrivateSchedulerDispatchCacheEntrySupported()
	{
		constexpr u32 EXPECTED_PUSH_R4_R11_LR = 0xe92d4ff0u;
		const auto* const body = reinterpret_cast<const u32*>(reinterpret_cast<uptr>(
				&VitaIopA32ProviderSchedulerDispatchCachedResumeBodySymbol));
		return body[0] == EXPECTED_PUSH_R4_R11_LR;
	}

	bool VitaIopA32PrivateWaitResumeEntrySupported()
	{
		// Skip only the stable core PUSH. The translation-unit reservation makes
		// product and validation bodies preserve d8-d15 without a VFP prologue.
		constexpr u32 EXPECTED_PUSH_R4_R11_LR = 0xe92d4ff0u;
		const auto has_private_push = [&](const void* body) {
			return *reinterpret_cast<const u32*>(body) == EXPECTED_PUSH_R4_R11_LR;
		};
		bool supported =
			has_private_push(reinterpret_cast<const void*>(
				&VitaIopA32ProviderWaitResumeUnconditionalNoLinkNormalBodySymbol)) &&
			has_private_push(reinterpret_cast<const void*>(
				&VitaIopA32ProviderWaitResumeUnconditionalNoLinkPs1BodySymbol)) &&
			has_private_push(reinterpret_cast<const void*>(
				&VitaIopA32ProviderWaitResumeUnconditionalNormalBodySymbol)) &&
			has_private_push(reinterpret_cast<const void*>(
				&VitaIopA32ProviderWaitResumeUnconditionalPs1BodySymbol)) &&
			has_private_push(reinterpret_cast<const void*>(
				&VitaIopA32ProviderWaitResumePollNormalBodySymbol)) &&
			has_private_push(reinterpret_cast<const void*>(
				&VitaIopA32ProviderWaitResumePollPs1BodySymbol)) &&
			has_private_push(reinterpret_cast<const void*>(
				&VitaIopA32ProviderWaitResumeConditionalNormalBodySymbol)) &&
			has_private_push(reinterpret_cast<const void*>(
				&VitaIopA32ProviderWaitResumeConditionalPs1BodySymbol));
#if defined(VITASX2_QEMU_VALIDATION) || \
	defined(VITASX2_IOP_WAIT_RESUME_CLOCK_ENTRY_CONTROL)
		supported = supported &&
			has_private_push(reinterpret_cast<const void*>(
				&VitaIopA32ProviderWaitResumeUnconditionalBodySymbol)) &&
			has_private_push(reinterpret_cast<const void*>(
				&VitaIopA32ProviderWaitResumePollBodySymbol)) &&
			has_private_push(reinterpret_cast<const void*>(
				&VitaIopA32ProviderWaitResumeConditionalBodySymbol));
#endif
#if defined(VITASX2_QEMU_VALIDATION) || \
	defined(VITASX2_IOP_WAIT_RESUME_KIND_ENTRY_CONTROL)
		supported = supported && has_private_push(reinterpret_cast<const void*>(
			&VitaIopA32ProviderWaitResumeBodySymbol));
#endif
		return supported;
	}

	extern "C" __attribute__((naked, noinline)) s32
	VitaIopA32ExecuteProviderTimeslicePrivate(void*, s32)
	{
		asm volatile(
			// Reserve the standard nine-word save area, but publish only LR.
			// The body reconstructs its CFA and returns without reloading the
			// caller-owned r4-r11 values at the private EE scheduler seam.
			"sub sp, sp, #36\n"
			"str lr, [sp, #32]\n"
			"b VitaIopA32ProviderTimesliceBody + 4\n");
	}

	extern "C" __attribute__((naked, noinline)) s32
	VitaIopA32ExecuteProviderSchedulerDirectResumePrivate(void*, s32)
	{
		asm volatile(
			// Keep one scheduler-selected entry installed for the provider's
			// lifetime. A null block takes the ordinary private body; an exact
			// retained target enters the shorter known-block body.
			"ldmia r0, {r0, r2}\n"
			"cmp r2, #0\n"
			"sub sp, sp, #36\n"
			"str lr, [sp, #32]\n"
			"beq VitaIopA32ProviderTimesliceBody + 4\n"
			"b VitaIopA32ProviderSchedulerDirectResumeBody + 4\n");
	}

	extern "C" __attribute__((naked, noinline)) s32
	VitaIopA32ExecuteProviderSchedulerPredictedResumePrivate(void*, s32)
	{
		static_assert(BlockExecutor::SchedulerPredictionSecondOffset() < 65536);
		asm volatile(
			// Exact scheduler identity wins. Otherwise match the two cached
			// register-dispatch targets against the exact architectural PC. An
			// alias miss falls through to the complete recLUT dispatcher.
			"ldmia r0, {r0, r2, r3, r12}\n"
			"sub sp, sp, #36\n"
			"str lr, [sp, #32]\n"
			"cmp r2, #0\n"
			"bne VitaIopA32ProviderSchedulerDirectResumeBody + 4\n"
			"movw lr, #:lower16:psxRegs\n"
			"movt lr, #:upper16:psxRegs\n"
			"ldr lr, [lr, #%c1]\n"
			"cmp r12, lr\n"
			"moveq r2, r3\n"
			"beq VitaIopA32ProviderSchedulerPredictedResumeBody + 4\n"
			"movw r2, #%c0\n"
			"add r2, r0, r2\n"
			"ldmia r2, {r2, r3}\n"
			"cmp r3, lr\n"
			"beq VitaIopA32ProviderSchedulerPredictedResumeBody + 4\n"
			"b VitaIopA32ProviderTimesliceBody + 4\n"
			:
			: "i"(BlockExecutor::SchedulerPredictionSecondOffset()),
			  "i"(offsetof(psxRegisters, pc)));
	}

	extern "C" __attribute__((naked, noinline, aligned(32))) s32
	VitaIopA32ExecuteProviderSchedulerDispatchCachedResumePrivate(void*, s32)
	{
		static_assert(BlockExecutor::SchedulerPredictionSecondOffset() < 65536);
		static_assert(BlockExecutor::SchedulerDispatchCacheOffset() < 65536);
		static_assert(BlockExecutor::SchedulerDispatchCacheEntryCount() == 64);
		asm volatile(
			// Preserve the exact route. Key both predictor ways and the indexed
			// tier with the exact architectural PC, so their hits do not normalize
			// a recLUT alias. A mismatch reaches the complete generic dispatcher.
			"ldmia r0, {r0, r2, r3, r12}\n"
			"sub sp, sp, #36\n"
			"str lr, [sp, #32]\n"
			"cmp r2, #0\n"
			"bne VitaIopA32ProviderSchedulerDirectResumeBody + 4\n"
			"movw lr, #:lower16:psxRegs\n"
			"movt lr, #:upper16:psxRegs\n"
			"ldr lr, [lr, #%c2]\n"
			"cmp r12, lr\n"
			"moveq r2, r3\n"
			"beq VitaIopA32ProviderSchedulerPredictedResumeBody + 4\n"
			"movw r2, #%c0\n"
			"add r2, r0, r2\n"
			"ldmia r2, {r2, r3}\n"
			"cmp r3, lr\n"
			"beq VitaIopA32ProviderSchedulerPredictedResumeBody + 4\n"
			"movw r3, #%c1\n"
			"add r3, r0, r3\n"
			"ubfx r12, lr, #2, #6\n"
			"add r3, r3, r12, lsl #3\n"
			"ldmia r3, {r2, r3}\n"
			"cmp r3, lr\n"
			"beq VitaIopA32ProviderSchedulerDispatchCachedResumeBody + 4\n"
			"b VitaIopA32ProviderTimesliceBody + 4\n"
			:
			: "i"(BlockExecutor::SchedulerPredictionSecondOffset()),
			"i"(BlockExecutor::SchedulerDispatchCacheOffset()),
			"i"(offsetof(psxRegisters, pc)));
	}

#if defined(VITASX2_QEMU_VALIDATION) || \
	defined(VITASX2_IOP_WAIT_RESUME_KIND_ENTRY_CONTROL)
	extern "C" __attribute__((naked, noinline)) s32
	VitaIopA32ExecuteProviderWaitResumePrivate(void*, s32)
	{
		asm volatile(
			// The scheduler context supplies {executor, retained block, proven
			// wait kind}. Convert it to the private member ABI and reserve the
			// verified core CFA.
			"ldr r2, [r0, #4]\n"
			"ldr r3, [r0, #8]\n"
			"ldr r0, [r0, #0]\n"
			"sub sp, sp, #36\n"
			"str lr, [sp, #32]\n"
			"b VitaIopA32ProviderWaitResumeBody + 4\n");
	}
#endif

#if defined(VITASX2_QEMU_VALIDATION) || \
	defined(VITASX2_IOP_WAIT_RESUME_CLOCK_ENTRY_CONTROL)
	extern "C" __attribute__((naked, noinline)) s32
	VitaIopA32ExecuteProviderWaitResumeUnconditionalPrivate(void*, s32)
	{
		asm volatile("ldr r2, [r0, #4]\n"
			"ldr r0, [r0, #0]\n"
			"sub sp, sp, #36\n"
			"str lr, [sp, #32]\n"
			"b VitaIopA32ProviderWaitResumeUnconditionalBody + 4\n");
	}

	extern "C" __attribute__((naked, noinline)) s32
	VitaIopA32ExecuteProviderWaitResumePollPrivate(void*, s32)
	{
		asm volatile("ldr r2, [r0, #4]\n"
			"ldr r0, [r0, #0]\n"
			"sub sp, sp, #36\n"
			"str lr, [sp, #32]\n"
			"b VitaIopA32ProviderWaitResumePollBody + 4\n");
	}

	extern "C" __attribute__((naked, noinline)) s32
	VitaIopA32ExecuteProviderWaitResumeConditionalPrivate(void*, s32)
	{
		asm volatile("ldr r2, [r0, #4]\n"
			"ldr r0, [r0, #0]\n"
			"sub sp, sp, #36\n"
			"str lr, [sp, #32]\n"
			"b VitaIopA32ProviderWaitResumeConditionalBody + 4\n");
	}
#endif

#define VITA_IOP_DEFINE_CLOCK_WAIT_RESUME_THUNK(Name, Body) \
	extern "C" __attribute__((naked, noinline)) s32 Name(void*, s32) \
	{ \
		asm volatile("ldr r2, [r0, #4]\n" \
			"ldr r0, [r0, #0]\n" \
			"sub sp, sp, #36\n" \
			"str lr, [sp, #32]\n" \
			"b " Body " + 4\n"); \
	}

	VITA_IOP_DEFINE_CLOCK_WAIT_RESUME_THUNK(
		VitaIopA32ExecuteProviderWaitResumeUnconditionalNormalPrivate,
		"VitaIopA32ProviderWaitResumeUnconditionalNormalBody")
	VITA_IOP_DEFINE_CLOCK_WAIT_RESUME_THUNK(
		VitaIopA32ExecuteProviderWaitResumeUnconditionalPs1Private,
		"VitaIopA32ProviderWaitResumeUnconditionalPs1Body")

#if defined(VITASX2_IOP_SCHEDULER_PRE_EVENT_WAIT_ADVANCE_CONTROL)
	VITA_IOP_DEFINE_CLOCK_WAIT_RESUME_THUNK(
		VitaIopA32ExecuteProviderWaitResumeUnconditionalNoLinkNormalPrivate,
		"VitaIopA32ProviderWaitResumeUnconditionalNoLinkNormalBody")
#else
#if defined(VITASX2_QEMU_VALIDATION)
#define VITA_IOP_PRE_EVENT_WAIT_VALIDATION_GATE \
			"movw r12, #:lower16:g_vita_a32_iop_scheduler_pre_event_wait_advance_enabled\n" \
			"movt r12, #:upper16:g_vita_a32_iop_scheduler_pre_event_wait_advance_enabled\n" \
			"ldr r12, [r12]\n" \
			"cmp r12, #0\n" \
			"beq 1f\n"
#else
#define VITA_IOP_PRE_EVENT_WAIT_VALIDATION_GATE
#endif

	extern "C" __attribute__((naked, noinline, aligned(32))) s32
	VitaIopA32ExecuteProviderWaitResumeUnconditionalNoLinkNormalPrivate(
		void*, s32)
	{
		static_assert(CYCLE_OFFSET + sizeof(u64) <= 4095);
		static_assert(IOP_NEXT_EVENT_CYCLE_OFFSET + sizeof(u64) <= 4095);
		static_assert(IOP_CYCLE_EE_OFFSET <= 4095);
		static_assert(IOP_BREAK_OFFSET <= 4095);
		asm volatile(
			// PCSX2 owner: x86/iR3000A.cpp::iPsxBranchTest(). A retained
			// unconditional no-link wait has no architectural work before the
			// next IOP event. When this EE budget ends no later than that event,
			// advance the exact 8:1 clock and return directly from the scheduler
			// seam. Event crossings retain the complete provider body below,
			// including iopEventTest() and post-event source/PC identity checks.
			VITA_IOP_PRE_EVENT_WAIT_VALIDATION_GATE
			"cmp r1, #0\n"
			"ble 1f\n"
			"movw r12, #:lower16:psxRegs\n"
			"movt r12, #:upper16:psxRegs\n"
			"ldr r2, [r12, #%c0]\n"
			"ldr r3, [r12, #%c1]\n"
			"add r3, r1, #7\n"
			"mov r3, r3, lsr #3\n"
			"adds r2, r2, r3\n"
			"ldr r3, [r12, #%c1]\n"
			"adc r3, r3, #0\n"
			"ldr r12, [r12, #%c3]\n"
			"cmp r3, r12\n"
			"blo 2f\n"
			"bhi 1f\n"
			"movw r12, #:lower16:psxRegs\n"
			"movt r12, #:upper16:psxRegs\n"
			"ldr r12, [r12, #%c2]\n"
			"cmp r2, r12\n"
			"bls 2f\n"
			"1:\n"
			"ldr r2, [r0, #4]\n"
			"ldr r0, [r0, #0]\n"
			"sub sp, sp, #36\n"
			"str lr, [sp, #32]\n"
			"b VitaIopA32ProviderWaitResumeUnconditionalNoLinkNormalBody + 4\n"
			"2:\n"
			"movw r12, #:lower16:psxRegs\n"
			"movt r12, #:upper16:psxRegs\n"
			"str r2, [r12, #%c0]\n"
			"str r3, [r12, #%c1]\n"
			"add r3, r1, #7\n"
			"mov r3, r3, lsr #3\n"
			"sub r0, r1, r3, lsl #3\n"
			"mov r2, #0\n"
			"str r2, [r12, #%c4]\n"
			"str r0, [r12, #%c5]\n"
			"bx lr\n"
			:
			: "i"(CYCLE_OFFSET), "i"(CYCLE_OFFSET + sizeof(u32)),
			  "i"(IOP_NEXT_EVENT_CYCLE_OFFSET),
			  "i"(IOP_NEXT_EVENT_CYCLE_OFFSET + sizeof(u32)),
			  "i"(IOP_BREAK_OFFSET), "i"(IOP_CYCLE_EE_OFFSET)
			: "memory", "cc");
	}

#undef VITA_IOP_PRE_EVENT_WAIT_VALIDATION_GATE
#endif
	VITA_IOP_DEFINE_CLOCK_WAIT_RESUME_THUNK(
		VitaIopA32ExecuteProviderWaitResumeUnconditionalNoLinkPs1Private,
		"VitaIopA32ProviderWaitResumeUnconditionalNoLinkPs1Body")
	VITA_IOP_DEFINE_CLOCK_WAIT_RESUME_THUNK(
		VitaIopA32ExecuteProviderWaitResumePollNormalPrivate,
		"VitaIopA32ProviderWaitResumePollNormalBody")
	VITA_IOP_DEFINE_CLOCK_WAIT_RESUME_THUNK(
		VitaIopA32ExecuteProviderWaitResumePollPs1Private,
		"VitaIopA32ProviderWaitResumePollPs1Body")
	VITA_IOP_DEFINE_CLOCK_WAIT_RESUME_THUNK(
		VitaIopA32ExecuteProviderWaitResumeConditionalNormalPrivate,
		"VitaIopA32ProviderWaitResumeConditionalNormalBody")
	VITA_IOP_DEFINE_CLOCK_WAIT_RESUME_THUNK(
		VitaIopA32ExecuteProviderWaitResumeConditionalPs1Private,
		"VitaIopA32ProviderWaitResumeConditionalPs1Body")

#undef VITA_IOP_DEFINE_CLOCK_WAIT_RESUME_THUNK
#endif
} // namespace VitaIOP
