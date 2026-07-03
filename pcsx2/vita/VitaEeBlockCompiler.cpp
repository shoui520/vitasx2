// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "pcsx2/vita/VitaEeBlockCompiler.h"
#include "pcsx2/COP0.h"
#include "pcsx2/MemoryTypes.h"
#include "pcsx2/vtlb.h"

#if defined(VITASX2_QEMU_VALIDATION) && !defined(VITASX2_QEMU_FULL_CORE)
#define VITASX2_QEMU_PROVIDER_FIXTURE 1
#endif

#if defined(VITASX2_QEMU_PROVIDER_FIXTURE)
#include "vita/qemu/VitaEeQemuStubs.h"
#else
#include "pcsx2/Config.h"
#include "pcsx2/Memory.h"
#include "pcsx2/R5900.h"
#include "pcsx2/R5900OpcodeTables.h"
#include "pcsx2/VUmicro.h"
#endif
#include "pcsx2/vita/A32Emitter.h"
#if !defined(VITASX2_QEMU_PROVIDER_FIXTURE)
#include "pcsx2/vita/VitaCore.h"
#include "pcsx2/DebugTools/GsTrace.h"

#include "common/Console.h"
#include "fmt/format.h"
#endif

#include <cstddef>
#if defined(VITASX2_QEMU_VALIDATION)
#include <cstdio>
#endif
#include <string>

#if !defined(VITASX2_QEMU_PROVIDER_FIXTURE)
extern void vu0Sync();
#endif

#if defined(VITASX2_QEMU_VALIDATION)
u32 g_qemuDivSignedHelperCalls = 0;
u32 g_qemuDivUnsignedHelperCalls = 0;
u32 g_qemuDivSigned1HelperCalls = 0;
u32 g_qemuDivUnsigned1HelperCalls = 0;
u32 g_qemuPackedDivSignedWordHelperCalls = 0;
u32 g_qemuPackedDivUnsignedWordHelperCalls = 0;
u32 g_qemuPackedDivWordByHalfwordHelperCalls = 0;
u32 g_qemuByteMemoryHelperCalls = 0;
u32 g_qemuHalfwordMemoryHelperCalls = 0;
u32 g_qemuPartialWordMemoryHelperCalls = 0;
u32 g_qemuWordMemoryHelperCalls = 0;
u32 g_qemuDwordMemoryHelperCalls = 0;
u32 g_qemuPartialDwordMemoryHelperCalls = 0;
u32 g_qemuCop1MemoryHelperCalls = 0;
u32 g_qemuQwordGprMemoryHelperCalls = 0;
u32 g_qemuQwordCop2MemoryHelperCalls = 0;
#endif

namespace VitaEE
{
	namespace
	{
		constexpr u16 REG_R4 = 1u << 4;
		constexpr u16 REG_R5 = 1u << 5;
		constexpr u16 REG_R6 = 1u << 6;
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
		constexpr unsigned HOST_TMP5 = 6;

		constexpr size_t GPR_OFFSET = offsetof(cpuRegisters, GPR);
		constexpr size_t HI_OFFSET = offsetof(cpuRegisters, HI);
		constexpr size_t LO_OFFSET = offsetof(cpuRegisters, LO);
		constexpr size_t CP0_OFFSET = offsetof(cpuRegisters, CP0);
		constexpr size_t FPU_OFFSET = offsetof(cpuRegistersPack, fpuRegs) - offsetof(cpuRegistersPack, cpuRegs);
		constexpr size_t FPR_OFFSET = FPU_OFFSET + offsetof(fpuRegisters, fpr);
		constexpr size_t FPRC_OFFSET = FPU_OFFSET + offsetof(fpuRegisters, fprc);
		constexpr size_t FPU_ACC_OFFSET = FPU_OFFSET + offsetof(fpuRegisters, ACC);
		constexpr size_t FPU_ACCFLAG_OFFSET = FPU_OFFSET + offsetof(fpuRegisters, ACCflag);
		constexpr size_t SA_OFFSET = offsetof(cpuRegisters, sa);
		constexpr size_t PC_OFFSET = offsetof(cpuRegisters, pc);
		constexpr size_t CODE_OFFSET = offsetof(cpuRegisters, code);
		constexpr size_t PERF_OFFSET = offsetof(cpuRegisters, PERF);
		constexpr size_t PERF_PCCR_OFFSET = PERF_OFFSET;
		constexpr size_t PERF_PCR0_OFFSET = PERF_OFFSET + sizeof(u32);
		constexpr size_t PERF_PCR1_OFFSET = PERF_OFFSET + 2 * sizeof(u32);
		constexpr size_t CYCLE_OFFSET = offsetof(cpuRegisters, cycle);
		constexpr size_t BRANCH_OFFSET = offsetof(cpuRegisters, branch);
		constexpr size_t NEXT_EVENT_OFFSET = offsetof(cpuRegisters, nextEventCycle);
		constexpr size_t LAST_COP0_CYCLE_OFFSET = offsetof(cpuRegisters, lastCOP0Cycle);
		constexpr size_t LAST_PERF_CYCLE_OFFSET = offsetof(cpuRegisters, lastPERFCycle);
		constexpr size_t TLB_ENTRY_COUNT = 48;
		constexpr size_t TLB_PAGE_MASK_OFFSET = offsetof(tlbs, PageMask);
		constexpr size_t TLB_ENTRY_HI_OFFSET = offsetof(tlbs, EntryHi);
		constexpr size_t TLB_ENTRY_LO0_OFFSET = offsetof(tlbs, EntryLo0);
		constexpr size_t TLB_ENTRY_LO1_OFFSET = offsetof(tlbs, EntryLo1);
		constexpr size_t TLB_ENTRY_SIZE = sizeof(tlbs);
		constexpr u32 TLB_PAGE_MASK_REGISTER_MASK = 0x01ffe000u;
		constexpr u32 TLB_TLBR_ENTRY_LO0_MASK = 0x03fffffeu;
		constexpr u32 TLB_TLBR_ENTRY_LO1_MASK = 0x83fffffeu;
		constexpr u32 TLB_ENTRY_HI32_VPN2_MASK = 0x0007ffffu;
		constexpr u32 TLB_MASK_FIELD_MASK = 0x00000fffu;

		constexpr u32 GOEMON_PRELOAD_RETURN_PC_0 = 0x0033ad48;
		constexpr u32 GOEMON_PRELOAD_RETURN_PC_1 = 0x0035060c;
		constexpr u32 GOEMON_UNLOAD_ENTRY_PC = 0x003563b8;
		constexpr u32 FPU_FCR31_CONDITION_FLAG = 0x00800000;
		constexpr u32 FPU_FCR31_INVALID_FLAG = 0x00020000;
		constexpr u32 FPU_FCR31_DIVIDE_BY_ZERO_FLAG = 0x00010000;
		constexpr u32 FPU_FCR31_OVERFLOW_FLAG = 0x00008000;
		constexpr u32 FPU_FCR31_UNDERFLOW_FLAG = 0x00004000;
		constexpr u32 FPU_FCR31_STICKY_INVALID_FLAG = 0x00000040;
		constexpr u32 FPU_FCR31_STICKY_DIVIDE_BY_ZERO_FLAG = 0x00000020;
		constexpr u32 FPU_FCR31_STICKY_OVERFLOW_FLAG = 0x00000010;
		constexpr u32 FPU_FCR31_STICKY_UNDERFLOW_FLAG = 0x00000008;
		constexpr u32 FPU_FCR31_INVALID_FLAGS =
			FPU_FCR31_INVALID_FLAG | FPU_FCR31_STICKY_INVALID_FLAG;
		constexpr u32 FPU_FCR31_DIVIDE_BY_ZERO_FLAGS =
			FPU_FCR31_DIVIDE_BY_ZERO_FLAG | FPU_FCR31_STICKY_DIVIDE_BY_ZERO_FLAG;
		constexpr u32 FPU_FCR31_ARITHMETIC_OVERFLOW_FLAGS =
			FPU_FCR31_OVERFLOW_FLAG | FPU_FCR31_STICKY_OVERFLOW_FLAG;
		constexpr u32 FPU_FCR31_ARITHMETIC_UNDERFLOW_FLAGS =
			FPU_FCR31_UNDERFLOW_FLAG | FPU_FCR31_STICKY_UNDERFLOW_FLAG;
		constexpr u32 FPU_FCR31_INVALID_DIVIDE_CAUSE_FLAGS =
			FPU_FCR31_INVALID_FLAG | FPU_FCR31_DIVIDE_BY_ZERO_FLAG;
		constexpr u32 FPU_FCR31_OVERFLOW_UNDERFLOW_FLAGS = 0x0000c000;
		constexpr u32 FPU_FCR31_CLEAR_OVERFLOW_UNDERFLOW_MASK = ~FPU_FCR31_OVERFLOW_UNDERFLOW_FLAGS;
		constexpr u32 FPU_FLOAT_SIGN_MASK = 0x80000000;
		constexpr u32 FPU_FLOAT_EXPONENT_MASK = 0x7f800000;
		constexpr u32 FPU_FLOAT_FRACTION_MASK = 0x007fffff;
		constexpr u32 FPU_FLOAT_IMPLICIT_MANTISSA = 0x00800000;
		constexpr u32 FPU_FLOAT_MAX_FINITE = 0x7f7fffff;
		constexpr u32 FPU_CVT_W_MAX_EXPONENT_MASK = 0x4e800000;
		constexpr u32 FPU_FLOAT_EXPONENT_BIAS = 127;
		constexpr u32 FPU_FLOAT_MANTISSA_BITS = 23;
		constexpr size_t DMAC_REGS_HW_OFFSET = 0xe000;
		constexpr u16 DMAC_STAT_DMAC_OFFSET = 0x10;
		constexpr u16 DMAC_PCR_DMAC_OFFSET = 0x20;
		constexpr u32 DMAC_CPCOND_MASK = 0x3ff;

		alignas(16) GPR_reg s_lq_zero_sink;

		constexpr u32 LWL_MASK[4] = {0x00ffffff, 0x0000ffff, 0x000000ff, 0x00000000};
		constexpr u32 LWR_MASK[4] = {0x00000000, 0xff000000, 0xffff0000, 0xffffff00};
		constexpr u8 LWL_SHIFT[4] = {24, 16, 8, 0};
		constexpr u8 LWR_SHIFT[4] = {0, 8, 16, 24};
		constexpr u64 LDL_MASK[8] = {
			0x00ffffffffffffffULL, 0x0000ffffffffffffULL, 0x000000ffffffffffULL, 0x00000000ffffffffULL,
			0x0000000000ffffffULL, 0x000000000000ffffULL, 0x00000000000000ffULL, 0x0000000000000000ULL};
		constexpr u64 LDR_MASK[8] = {
			0x0000000000000000ULL, 0xff00000000000000ULL, 0xffff000000000000ULL, 0xffffff0000000000ULL,
			0xffffffff00000000ULL, 0xffffffffff000000ULL, 0xffffffffffff0000ULL, 0xffffffffffffff00ULL};
		constexpr u8 LDL_SHIFT[8] = {56, 48, 40, 32, 24, 16, 8, 0};
		constexpr u8 LDR_SHIFT[8] = {0, 8, 16, 24, 32, 40, 48, 56};
		constexpr u32 SWL_MASK[4] = {0xffffff00, 0xffff0000, 0xff000000, 0x00000000};
		constexpr u32 SWR_MASK[4] = {0x00000000, 0x000000ff, 0x0000ffff, 0x00ffffff};
		constexpr u8 SWL_SHIFT[4] = {24, 16, 8, 0};
		constexpr u8 SWR_SHIFT[4] = {0, 8, 16, 24};
		constexpr u64 SDL_MASK[8] = {
			0xffffffffffffff00ULL, 0xffffffffffff0000ULL, 0xffffffffff000000ULL, 0xffffffff00000000ULL,
			0xffffff0000000000ULL, 0xffff000000000000ULL, 0xff00000000000000ULL, 0x0000000000000000ULL};
		constexpr u64 SDR_MASK[8] = {
			0x0000000000000000ULL, 0x00000000000000ffULL, 0x000000000000ffffULL, 0x0000000000ffffffULL,
			0x00000000ffffffffULL, 0x000000ffffffffffULL, 0x0000ffffffffffffULL, 0x00ffffffffffffffULL};
		constexpr u8 SDL_SHIFT[8] = {56, 48, 40, 32, 24, 16, 8, 0};
		constexpr u8 SDR_SHIFT[8] = {0, 8, 16, 24, 32, 40, 48, 56};

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

		constexpr size_t Cp0Offset(unsigned guest_reg)
		{
			return CP0_OFFSET + sizeof(u32) * guest_reg;
		}

		constexpr size_t FprOffset(unsigned guest_reg)
		{
			return FPR_OFFSET + sizeof(FPRreg) * guest_reg;
		}

		constexpr size_t FprcOffset(unsigned guest_reg)
		{
			return FPRC_OFFSET + sizeof(u32) * guest_reg;
		}

		constexpr size_t HiloLaneOffset(size_t hilo_offset, bool upper_pipeline)
		{
			return hilo_offset + (upper_pipeline ? sizeof(u64) : 0);
		}

		constexpr size_t PackedHalfwordAccumulatorOffset(unsigned lane)
		{
			const size_t base_offset = ((lane & 2u) != 0) ? HI_OFFSET : LO_OFFSET;
			const unsigned word = (lane & 1u) + (((lane & 4u) != 0) ? 2u : 0u);
			return base_offset + word * sizeof(u32);
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
				case 0x0d: // BREAK, owned by R5900OpcodeImpl.cpp::BREAK().
				case 0x0f: // SYNC, owned by R5900OpcodeImpl.cpp::SYNC().
				case 0x10: // MFHI, owned by R5900OpcodeImpl.cpp::MFHI().
				case 0x11: // MTHI, owned by R5900OpcodeImpl.cpp::MTHI().
				case 0x12: // MFLO, owned by R5900OpcodeImpl.cpp::MFLO().
				case 0x13: // MTLO, owned by R5900OpcodeImpl.cpp::MTLO().
				case 0x14: // DSLLV, owned by R5900OpcodeImpl.cpp::DSLLV().
				case 0x16: // DSRLV, owned by R5900OpcodeImpl.cpp::DSRLV().
				case 0x17: // DSRAV, owned by R5900OpcodeImpl.cpp::DSRAV().
				case 0x18: // MULT, owned by R5900OpcodeImpl.cpp::MULT().
				case 0x19: // MULTU, owned by R5900OpcodeImpl.cpp::MULTU().
				case 0x1a: // DIV, owned by R5900OpcodeImpl.cpp::DIV().
				case 0x1b: // DIVU, owned by R5900OpcodeImpl.cpp::DIVU().
				case 0x20: // ADD, owned by R5900OpcodeImpl.cpp::ADD().
				case 0x21: // ADDU, owned by R5900OpcodeImpl.cpp::ADDU().
				case 0x22: // SUB, owned by R5900OpcodeImpl.cpp::SUB().
				case 0x23: // SUBU, owned by R5900OpcodeImpl.cpp::SUBU().
				case 0x24: // AND, owned by R5900OpcodeImpl.cpp::AND().
				case 0x25: // OR, owned by R5900OpcodeImpl.cpp::OR().
				case 0x26: // XOR, owned by R5900OpcodeImpl.cpp::XOR().
				case 0x27: // NOR, owned by R5900OpcodeImpl.cpp::NOR().
				case 0x2a: // SLT, owned by R5900OpcodeImpl.cpp::SLT().
				case 0x2b: // SLTU, owned by R5900OpcodeImpl.cpp::SLTU().
				case 0x2c: // DADD, owned by R5900OpcodeImpl.cpp::DADD().
				case 0x2d: // DADDU, owned by R5900OpcodeImpl.cpp::DADDU().
				case 0x2e: // DSUB, owned by R5900OpcodeImpl.cpp::DSUB().
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

		bool IsBREAK(u32 op)
		{
			return (op >> 26) == 0x00 && (op & 0x3f) == 0x0d;
		}

		bool IsCounterReadLoad(u32 op)
		{
			switch (op >> 26)
			{
				case 0x20: // LB, owned by R5900OpcodeImpl.cpp::LB().
				case 0x21: // LH, owned by R5900OpcodeImpl.cpp::LH().
				case 0x23: // LW, owned by R5900OpcodeImpl.cpp::LW().
				case 0x24: // LBU, owned by R5900OpcodeImpl.cpp::LBU().
				case 0x25: // LHU, owned by R5900OpcodeImpl.cpp::LHU().
					return true;
				default:
					return false;
			}
		}

		bool CanCompileMMI0(u32 op);
		bool CanCompileMMI1(u32 op);
		bool CanCompileMMI2(u32 op);
		bool CanCompileMMI3(u32 op);
		bool CanCompileCOP0(u32 op);
		bool CanCompileCOP1(u32 op);

		bool IsFastMFC0(u32 op)
		{
			if ((op >> 26) != 0x10 || ((op >> 21) & 0x1f) != 0x00)
				return false;

			const unsigned rd = RD(op);
			// PCSX2 x86/iCOP0.cpp::recMFC0() keeps Count in-block by committing
			// cycles through scaleblockcycles_clear(). MFPS/PCCR also stays in-block,
			// but PCR0/PCR1 reads call COP0_UpdatePCCR() and keep the event path.
			if (rd != 25)
				return true;

			return RT(op) == 0 || (op & 1u) == 0;
		}

		bool IsFastMTC0(u32 op)
		{
			if ((op >> 26) != 0x10 || ((op >> 21) & 0x1f) != 0x04)
				return false;

			switch (RD(op))
			{
				case 0x09: // Count, owned by x86/iCOP0.cpp::recMTC0().
				case 0x10: // Config, owned by COP0.cpp::WriteCP0Config().
				case 0x18: // Breakpoint debug registers only log in PCSX2.
					return true;
				case 0x0c: // Status, owned by x86/iCOP0.cpp::recMTC0().
					return true;
				case 0x19: // Perf counters.
					if ((op & 1u) == 0)
						return (op & 0x3eu) != 0; // MTPS/PCCR sel 0 calls COP0_UpdatePCCR(); other even sels no-op.
					return true; // MTPC0/MTPC1, selected by sel bit 1.
				default:
					return true;
			}
		}

		bool IsDI(u32 op)
		{
			return (op >> 26) == 0x10 && ((op >> 21) & 0x1f) == 0x10 && (op & 0x3f) == 0x39;
		}

		bool IsCycleCommittingFastCOP0(u32 op)
		{
			if ((op >> 26) != 0x10)
				return false;

			switch ((op >> 21) & 0x1f)
			{
				case 0x00:
					return RD(op) == 9;
				case 0x04:
					return RD(op) == 9 || RD(op) == 12 || (RD(op) == 25 && (op & 1u) != 0);
				default:
					return false;
			}
		}

		bool IsFastCOP1MoveControl(u32 op)
		{
			if ((op >> 26) != 0x11)
				return false;

			switch ((op >> 21) & 0x1f)
			{
				case 0x00: // MFC1
				case 0x02: // CFC1
				case 0x04: // MTC1
				case 0x06: // CTC1
					return true;
				default:
					return false;
			}
		}

		bool IsFastCOP1ScalarWordOp(u32 op)
		{
			if ((op >> 26) != 0x11 || ((op >> 21) & 0x1f) != 0x10)
				return false;

			switch (op & 0x3f)
			{
				case 0x05: // ABS_S, owned by FPU.cpp::ABS_S().
				case 0x06: // MOV_S, owned by FPU.cpp::MOV_S().
				case 0x07: // NEG_S, owned by FPU.cpp::NEG_S().
				case 0x28: // MAX_S, owned by FPU.cpp::MAX_S().
				case 0x29: // MIN_S, owned by FPU.cpp::MIN_S().
					return true;
				default:
					return false;
			}
		}

		bool IsFastCOP1ArithmeticOp(u32 op)
		{
			if ((op >> 26) != 0x11 || ((op >> 21) & 0x1f) != 0x10)
				return false;

			switch (op & 0x3f)
			{
				case 0x00: // ADD_S, owned by FPU.cpp::ADD_S().
				case 0x01: // SUB_S, owned by FPU.cpp::SUB_S().
				case 0x02: // MUL_S, owned by FPU.cpp::MUL_S().
					return true;
				default:
					return false;
			}
		}

		bool IsFastCOP1DivSqrtOp(u32 op)
		{
			if ((op >> 26) != 0x11 || ((op >> 21) & 0x1f) != 0x10)
				return false;

			switch (op & 0x3f)
			{
				case 0x03: // DIV_S, owned by FPU.cpp::DIV_S().
				case 0x04: // SQRT_S, owned by FPU.cpp::SQRT_S().
				case 0x16: // RSQRT_S, owned by FPU.cpp::RSQRT_S().
					return true;
				default:
					return false;
			}
		}

		bool IsFastCOP1AccumulatorOp(u32 op)
		{
			if ((op >> 26) != 0x11 || ((op >> 21) & 0x1f) != 0x10)
				return false;

			switch (op & 0x3f)
			{
				case 0x18: // ADDA_S, owned by FPU.cpp::ADDA_S().
				case 0x19: // SUBA_S, owned by FPU.cpp::SUBA_S().
				case 0x1a: // MULA_S, owned by FPU.cpp::MULA_S().
				case 0x1c: // MADD_S, owned by FPU.cpp::MADD_S().
				case 0x1d: // MSUB_S, owned by FPU.cpp::MSUB_S().
				case 0x1e: // MADDA_S, owned by FPU.cpp::MADDA_S().
				case 0x1f: // MSUBA_S, owned by FPU.cpp::MSUBA_S().
					return true;
				default:
					return false;
			}
		}

		bool IsFastCOP1CompareOp(u32 op)
		{
			if ((op >> 26) != 0x11 || ((op >> 21) & 0x1f) != 0x10)
				return false;

			switch (op & 0x3f)
			{
				case 0x30: // C_F, owned by FPU.cpp::C_F().
				case 0x32: // C_EQ, owned by FPU.cpp::C_EQ().
				case 0x34: // C_LT, owned by FPU.cpp::C_LT().
				case 0x36: // C_LE, owned by FPU.cpp::C_LE().
					return true;
				default:
					return false;
			}
		}

		bool IsFastCOP1ConvertWordOp(u32 op)
		{
			return (op >> 26) == 0x11 && ((op >> 21) & 0x1f) == 0x10 &&
				   (op & 0x3f) == 0x24; // CVT_W, owned by FPU.cpp::CVT_W().
		}

		bool IsFastCOP1ConvertSingleOp(u32 op)
		{
			return (op >> 26) == 0x11 && ((op >> 21) & 0x1f) == 0x14 &&
				   (op & 0x3f) == 0x20; // CVT_S, owned by FPU.cpp::CVT_S().
		}

		bool IsFastCOP1InBlock(u32 op)
		{
			return IsFastCOP1MoveControl(op) || IsFastCOP1ArithmeticOp(op) ||
				   IsFastCOP1DivSqrtOp(op) ||
				   IsFastCOP1AccumulatorOp(op) ||
				   IsFastCOP1ScalarWordOp(op) ||
				   IsFastCOP1CompareOp(op) || IsFastCOP1ConvertWordOp(op) ||
				   IsFastCOP1ConvertSingleOp(op);
		}

		bool CanCompileMMI(u32 op)
		{
			switch (op & 0x3f)
			{
				case 0x00: // MADD, owned by MMI.cpp::MADD() and x86/ix86-32/iR5900MultDiv.cpp::recMADD().
				case 0x01: // MADDU, owned by MMI.cpp::MADDU() and x86/ix86-32/iR5900MultDiv.cpp::recMADDU().
				case 0x04: // PLZCW, owned by MMI.cpp::PLZCW().
				case 0x10: // MFHI1, owned by MMI.cpp::MFHI1().
				case 0x11: // MTHI1, owned by MMI.cpp::MTHI1().
				case 0x12: // MFLO1, owned by MMI.cpp::MFLO1().
				case 0x13: // MTLO1, owned by MMI.cpp::MTLO1().
				case 0x18: // MULT1, owned by MMI.cpp::MULT1() and x86/ix86-32/iR5900MultDiv.cpp::recMULT1().
				case 0x19: // MULTU1, owned by MMI.cpp::MULTU1() and x86/ix86-32/iR5900MultDiv.cpp::recMULTU1().
				case 0x1a: // DIV1, owned by MMI.cpp::DIV1() and x86/ix86-32/iR5900MultDiv.cpp::recDIV1().
				case 0x1b: // DIVU1, owned by MMI.cpp::DIVU1() and x86/ix86-32/iR5900MultDiv.cpp::recDIVU1().
				case 0x20: // MADD1, owned by MMI.cpp::MADD1() and x86/ix86-32/iR5900MultDiv.cpp::recMADD1().
				case 0x21: // MADDU1, owned by MMI.cpp::MADDU1() and x86/ix86-32/iR5900MultDiv.cpp::recMADDU1().
				case 0x30: // PMFHL, owned by MMI.cpp::PMFHL().
				case 0x31: // PMTHL, owned by MMI.cpp::PMTHL().
				case 0x34: // PSLLH, owned by MMI.cpp::PSLLH().
				case 0x36: // PSRLH, owned by MMI.cpp::PSRLH().
				case 0x37: // PSRAH, owned by MMI.cpp::PSRAH().
				case 0x3c: // PSLLW, owned by MMI.cpp::PSLLW().
				case 0x3e: // PSRLW, owned by MMI.cpp::PSRLW().
				case 0x3f: // PSRAW, owned by MMI.cpp::PSRAW().
					return true;
				case 0x08: // MMI0 class, owned by R5900OpcodeTables.cpp::Class_MMI0().
					return CanCompileMMI0(op);
				case 0x09: // MMI2 class, owned by R5900OpcodeTables.cpp::Class_MMI2().
					return CanCompileMMI2(op);
				case 0x28: // MMI1 class, owned by R5900OpcodeTables.cpp::Class_MMI1().
					return CanCompileMMI1(op);
				case 0x29: // MMI3 class, owned by R5900OpcodeTables.cpp::Class_MMI3().
					return CanCompileMMI3(op);
				default:
					return false;
			}
		}

		bool CanCompileMMI0(u32 op)
		{
			switch ((op >> 6) & 0x1f)
			{
				case 0x00: // PADDW, owned by MMI.cpp::PADDW().
				case 0x01: // PSUBW, owned by MMI.cpp::PSUBW().
				case 0x02: // PCGTW, owned by MMI.cpp::PCGTW().
				case 0x03: // PMAXW, owned by MMI.cpp::PMAXW().
				case 0x04: // PADDH, owned by MMI.cpp::PADDH().
				case 0x05: // PSUBH, owned by MMI.cpp::PSUBH().
				case 0x06: // PCGTH, owned by MMI.cpp::PCGTH().
				case 0x07: // PMAXH, owned by MMI.cpp::PMAXH().
				case 0x08: // PADDB, owned by MMI.cpp::PADDB().
				case 0x09: // PSUBB, owned by MMI.cpp::PSUBB().
				case 0x0a: // PCGTB, owned by MMI.cpp::PCGTB().
				case 0x10: // PADDSW, owned by MMI.cpp::PADDSW().
				case 0x11: // PSUBSW, owned by MMI.cpp::PSUBSW().
				case 0x12: // PEXTLW, owned by MMI.cpp::PEXTLW().
				case 0x13: // PPACW, owned by MMI.cpp::PPACW().
				case 0x14: // PADDSH, owned by MMI.cpp::PADDSH().
				case 0x15: // PSUBSH, owned by MMI.cpp::PSUBSH().
				case 0x16: // PEXTLH, owned by MMI.cpp::PEXTLH().
				case 0x17: // PPACH, owned by MMI.cpp::PPACH().
				case 0x18: // PADDSB, owned by MMI.cpp::PADDSB().
				case 0x19: // PSUBSB, owned by MMI.cpp::PSUBSB().
				case 0x1a: // PEXTLB, owned by MMI.cpp::PEXTLB().
				case 0x1b: // PPACB, owned by MMI.cpp::PPACB().
				case 0x1e: // PEXT5, owned by MMI.cpp::PEXT5().
				case 0x1f: // PPAC5, owned by MMI.cpp::PPAC5().
					return true;
				default:
					return false;
			}
		}

		bool CanCompileMMI1(u32 op)
		{
			switch ((op >> 6) & 0x1f)
			{
				case 0x01: // PABSW, owned by MMI.cpp::PABSW().
				case 0x02: // PCEQW, owned by MMI.cpp::PCEQW().
				case 0x03: // PMINW, owned by MMI.cpp::PMINW().
				case 0x04: // PADSBH, owned by MMI.cpp::PADSBH().
				case 0x05: // PABSH, owned by MMI.cpp::PABSH().
				case 0x06: // PCEQH, owned by MMI.cpp::PCEQH().
				case 0x07: // PMINH, owned by MMI.cpp::PMINH().
				case 0x0a: // PCEQB, owned by MMI.cpp::PCEQB().
				case 0x10: // PADDUW, owned by MMI.cpp::PADDUW().
				case 0x11: // PSUBUW, owned by MMI.cpp::PSUBUW().
				case 0x12: // PEXTUW, owned by MMI.cpp::PEXTUW().
				case 0x14: // PADDUH, owned by MMI.cpp::PADDUH().
				case 0x15: // PSUBUH, owned by MMI.cpp::PSUBUH().
				case 0x16: // PEXTUH, owned by MMI.cpp::PEXTUH().
				case 0x18: // PADDUB, owned by MMI.cpp::PADDUB().
				case 0x19: // PSUBUB, owned by MMI.cpp::PSUBUB().
				case 0x1a: // PEXTUB, owned by MMI.cpp::PEXTUB().
				case 0x1b: // QFSRV, owned by MMI.cpp::QFSRV().
					return true;
				default:
					return false;
			}
		}

		bool CanCompileMMI2(u32 op)
		{
			switch ((op >> 6) & 0x1f)
			{
				case 0x00: // PMADDW, owned by MMI.cpp::PMADDW().
				case 0x02: // PSLLVW, owned by MMI.cpp::PSLLVW().
				case 0x03: // PSRLVW, owned by MMI.cpp::PSRLVW().
				case 0x04: // PMSUBW, owned by MMI.cpp::PMSUBW().
				case 0x08: // PMFHI, owned by MMI.cpp::PMFHI().
				case 0x09: // PMFLO, owned by MMI.cpp::PMFLO().
				case 0x0a: // PINTH, owned by MMI.cpp::PINTH().
				case 0x0c: // PMULTW, owned by MMI.cpp::PMULTW().
				case 0x0d: // PDIVW, owned by MMI.cpp::PDIVW().
				case 0x0e: // PCPYLD, owned by MMI.cpp::PCPYLD().
				case 0x10: // PMADDH, owned by MMI.cpp::PMADDH().
				case 0x11: // PHMADH, owned by MMI.cpp::PHMADH().
				case 0x12: // PAND, owned by MMI.cpp::PAND().
				case 0x13: // PXOR, owned by MMI.cpp::PXOR().
				case 0x14: // PMSUBH, owned by MMI.cpp::PMSUBH().
				case 0x15: // PHMSBH, owned by MMI.cpp::PHMSBH().
				case 0x1a: // PEXEH, owned by MMI.cpp::PEXEH().
				case 0x1b: // PREVH, owned by MMI.cpp::PREVH().
				case 0x1c: // PMULTH, owned by MMI.cpp::PMULTH().
				case 0x1d: // PDIVBW, owned by MMI.cpp::PDIVBW().
				case 0x1e: // PEXEW, owned by MMI.cpp::PEXEW().
				case 0x1f: // PROT3W, owned by MMI.cpp::PROT3W().
					return true;
				default:
					return false;
			}
		}

		bool CanCompileMMI3(u32 op)
		{
			switch ((op >> 6) & 0x1f)
			{
				case 0x00: // PMADDUW, owned by MMI.cpp::PMADDUW().
				case 0x03: // PSRAVW, owned by MMI.cpp::PSRAVW().
				case 0x08: // PMTHI, owned by MMI.cpp::PMTHI().
				case 0x09: // PMTLO, owned by MMI.cpp::PMTLO().
				case 0x0a: // PINTEH, owned by MMI.cpp::PINTEH().
				case 0x0c: // PMULTUW, owned by MMI.cpp::PMULTUW().
				case 0x0d: // PDIVUW, owned by MMI.cpp::PDIVUW().
				case 0x0e: // PCPYUD, owned by MMI.cpp::PCPYUD().
				case 0x12: // POR, owned by MMI.cpp::POR().
				case 0x13: // PNOR, owned by MMI.cpp::PNOR().
				case 0x1a: // PEXCH, owned by MMI.cpp::PEXCH().
				case 0x1b: // PCPYH, owned by MMI.cpp::PCPYH().
				case 0x1e: // PEXCW, owned by MMI.cpp::PEXCW().
					return true;
				default:
					return false;
			}
		}

		bool CanCompileCOP0(u32 op)
		{
			switch ((op >> 21) & 0x1f)
			{
				case 0x00: // MFC0, owned by COP0.cpp::MFC0().
				case 0x04: // MTC0, owned by COP0.cpp::MTC0().
					return true;
				case 0x08: // COP0_BC0 branch forms, owned by COP0.cpp::BC0*().
					switch (RT(op))
					{
						case 0x00: // BC0F, owned by COP0.cpp::BC0F().
						case 0x01: // BC0T, owned by COP0.cpp::BC0T().
						case 0x02: // BC0FL, owned by COP0.cpp::BC0FL().
						case 0x03: // BC0TL, owned by COP0.cpp::BC0TL().
							return true;
						default:
							return false;
					}
				case 0x10: // COP0_C0 class, owned by R5900OpcodeTables.cpp::tbl_COP0_C0.
					switch (op & 0x3f)
					{
						case 0x01: // TLBR, owned by COP0.cpp::TLBR().
						case 0x02: // TLBWI, owned by COP0.cpp::TLBWI().
						case 0x06: // TLBWR, owned by COP0.cpp::TLBWR().
						case 0x08: // TLBP, owned by COP0.cpp::TLBP().
						case 0x18: // ERET, owned by COP0.cpp::ERET().
						case 0x38: // EI, owned by COP0.cpp::EI().
						case 0x39: // DI, owned by x86/iCOP0.cpp::recDI().
							return true;
						default:
							return false;
					}
				default:
					return false;
			}
		}

		bool CanCompileCOP1(u32 op)
		{
			switch ((op >> 21) & 0x1f)
			{
				case 0x00: // MFC1, owned by FPU.cpp::MFC1().
				case 0x02: // CFC1, owned by FPU.cpp::CFC1().
				case 0x04: // MTC1, owned by FPU.cpp::MTC1().
				case 0x06: // CTC1, owned by FPU.cpp::CTC1().
					return true;
				case 0x08: // COP1_BC1 class, owned by R5900OpcodeTables.cpp::tbl_COP1_BC1.
					switch (RT(op))
					{
						case 0x00: // BC1F, owned by FPU.cpp::BC1F().
						case 0x01: // BC1T, owned by FPU.cpp::BC1T().
						case 0x02: // BC1FL, owned by FPU.cpp::BC1FL().
						case 0x03: // BC1TL, owned by FPU.cpp::BC1TL().
							return true;
						default:
							return false;
					}
				case 0x10: // COP1_S class, owned by R5900OpcodeTables.cpp::tbl_COP1_S.
					switch (op & 0x3f)
					{
						case 0x00: // ADD_S, owned by FPU.cpp::ADD_S().
						case 0x01: // SUB_S, owned by FPU.cpp::SUB_S().
						case 0x02: // MUL_S, owned by FPU.cpp::MUL_S().
						case 0x03: // DIV_S, owned by FPU.cpp::DIV_S().
						case 0x04: // SQRT_S, owned by FPU.cpp::SQRT_S().
						case 0x05: // ABS_S, owned by FPU.cpp::ABS_S().
						case 0x06: // MOV_S, owned by FPU.cpp::MOV_S().
						case 0x07: // NEG_S, owned by FPU.cpp::NEG_S().
						case 0x16: // RSQRT_S, owned by FPU.cpp::RSQRT_S().
						case 0x18: // ADDA_S, owned by FPU.cpp::ADDA_S().
						case 0x19: // SUBA_S, owned by FPU.cpp::SUBA_S().
						case 0x1a: // MULA_S, owned by FPU.cpp::MULA_S().
						case 0x1c: // MADD_S, owned by FPU.cpp::MADD_S().
						case 0x1d: // MSUB_S, owned by FPU.cpp::MSUB_S().
						case 0x1e: // MADDA_S, owned by FPU.cpp::MADDA_S().
						case 0x1f: // MSUBA_S, owned by FPU.cpp::MSUBA_S().
						case 0x24: // CVT_W, owned by FPU.cpp::CVT_W().
						case 0x28: // MAX_S, owned by FPU.cpp::MAX_S().
						case 0x29: // MIN_S, owned by FPU.cpp::MIN_S().
						case 0x30: // C_F, owned by FPU.cpp::C_F().
						case 0x32: // C_EQ, owned by FPU.cpp::C_EQ().
						case 0x34: // C_LT, owned by FPU.cpp::C_LT().
						case 0x36: // C_LE, owned by FPU.cpp::C_LE().
							return true;
						default:
							return false;
					}
				case 0x14: // COP1_W class, owned by R5900OpcodeTables.cpp::tbl_COP1_W.
					return (op & 0x3f) == 0x20; // CVT_S, owned by FPU.cpp::CVT_S().
				default:
					return false;
			}
		}

		bool IsNoOpCACHE(u32 op)
		{
			switch (RT(op))
			{
				case 0x07: // IXIN, owned by Cache.cpp::CACHE(); PCSX2 no-ops instruction-cache invalidation.
				case 0x0c: // BFH, owned by Cache.cpp::CACHE(); PCSX2 no-ops BTAC flush.
					return true;
				default:
					return false;
			}
		}

		bool IsHelperCACHE(u32 op)
		{
			switch (RT(op))
			{
				case 0x10: // DXLTG, owned by Cache.cpp::CACHE().
				case 0x11: // DXLDT, owned by Cache.cpp::CACHE().
				case 0x12: // DXSTG, owned by Cache.cpp::CACHE().
				case 0x13: // DXSDT, owned by Cache.cpp::CACHE().
				case 0x14: // DXWBIN, owned by Cache.cpp::CACHE().
				case 0x16: // DXIN, owned by Cache.cpp::CACHE().
				case 0x18: // DHWBIN, owned by Cache.cpp::CACHE().
				case 0x1a: // DHIN, owned by Cache.cpp::CACHE().
				case 0x1c: // DHWOIN, owned by Cache.cpp::CACHE().
					return true;
				default:
					return false;
			}
		}

		bool CanCompileCACHE(u32 op)
		{
			return IsNoOpCACHE(op) || IsHelperCACHE(op);
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
				case 0x18: // MTSAB, owned by R5900OpcodeImpl.cpp::MTSAB().
				case 0x19: // MTSAH, owned by R5900OpcodeImpl.cpp::MTSAH().
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
				case 0x11:
					return ((op >> 21) & 0x1f) == 0x08 && (RT(op) == 0x02 || RT(op) == 0x03);
				case 0x10:
					return ((op >> 21) & 0x1f) == 0x08 && (RT(op) == 0x02 || RT(op) == 0x03);
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

		u32 RawCycleRemainderAfterClear(u32 raw_cycles)
		{
			// Ported from PCSX2 x86/ix86-32/iR5900.cpp::scaleblockcycles_clear().
			// The recompiler keeps the fixed-point remainder after an in-block
			// cycle commit, and the final block tail scales that remainder again.
			const bool lowcycles = (raw_cycles <= 40);
			const s8 cyclerate = EmuConfig.Speedhacks.EECycleRate;
			if (!lowcycles && cyclerate > 1)
				return raw_cycles & ((0x1u << (cyclerate + 2)) - 1);

			return raw_cycles & 0x7u;
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
#if defined(VITASX2_QEMU_VALIDATION)
			++g_qemuByteMemoryHelperCalls;
#endif
			return memRead8(addr);
		}

		__noinline u32 VitaEeMemRead16Checked(u32 addr)
		{
			// PCSX2 owners: R5900OpcodeImpl.cpp::LH() and LHU().
#if defined(VITASX2_QEMU_VALIDATION)
			++g_qemuHalfwordMemoryHelperCalls;
#endif
			if (addr & 1)
				VitaEeRaiseAddressError(addr, false);

			return memRead16(addr);
		}

		__noinline u32 VitaEeMemRead32Checked(u32 addr)
		{
			// PCSX2 owners: R5900OpcodeImpl.cpp::LW() and LWU().
#if defined(VITASX2_QEMU_VALIDATION)
			++g_qemuWordMemoryHelperCalls;
#endif
			if (addr & 3)
				VitaEeRaiseAddressError(addr, false);

			return memRead32(addr);
		}

		__noinline u64 VitaEeMemRead64Checked(u32 addr)
		{
			// PCSX2 owner: R5900OpcodeImpl.cpp::LD().
#if defined(VITASX2_QEMU_VALIDATION)
			++g_qemuDwordMemoryHelperCalls;
#endif
			if (addr & 7)
				VitaEeRaiseAddressError(addr, false);

			return memRead64(addr);
		}

		__noinline void VitaEeMemReadWordLeft(u32 addr, u32 guest_reg)
		{
			// PCSX2 owner: R5900OpcodeImpl.cpp::LWL().
#if defined(VITASX2_QEMU_VALIDATION)
			++g_qemuPartialWordMemoryHelperCalls;
#endif
			const u32 shift = addr & 3;
			const u32 mem = memRead32(addr & ~3u);
			if (guest_reg == 0)
				return;

			cpuRegs.GPR.r[guest_reg].SD[0] = static_cast<s32>((cpuRegs.GPR.r[guest_reg].UL[0] & LWL_MASK[shift]) |
															  (mem << LWL_SHIFT[shift]));
		}

		__noinline void VitaEeMemReadWordRight(u32 addr, u32 guest_reg)
		{
			// PCSX2 owner: R5900OpcodeImpl.cpp::LWR().
#if defined(VITASX2_QEMU_VALIDATION)
			++g_qemuPartialWordMemoryHelperCalls;
#endif
			const u32 shift = addr & 3;
			const u32 aligned_mem = memRead32(addr & ~3u);
			if (guest_reg == 0)
				return;

			const u32 mem = (cpuRegs.GPR.r[guest_reg].UL[0] & LWR_MASK[shift]) |
							(aligned_mem >> LWR_SHIFT[shift]);
			if (shift == 0)
				cpuRegs.GPR.r[guest_reg].SD[0] = static_cast<s32>(mem);
			else
				cpuRegs.GPR.r[guest_reg].UL[0] = mem;
		}

		__noinline void VitaEeMemReadDwordLeft(u32 addr, u32 guest_reg)
		{
			// PCSX2 owner: R5900OpcodeImpl.cpp::LDL().
#if defined(VITASX2_QEMU_VALIDATION)
			++g_qemuPartialDwordMemoryHelperCalls;
#endif
			const u32 shift = addr & 7;
			const u64 mem = memRead64(addr & ~7u);
			if (guest_reg == 0)
				return;

			cpuRegs.GPR.r[guest_reg].UD[0] = (cpuRegs.GPR.r[guest_reg].UD[0] & LDL_MASK[shift]) |
											 (mem << LDL_SHIFT[shift]);
		}

		__noinline void VitaEeMemReadDwordRight(u32 addr, u32 guest_reg)
		{
			// PCSX2 owner: R5900OpcodeImpl.cpp::LDR().
#if defined(VITASX2_QEMU_VALIDATION)
			++g_qemuPartialDwordMemoryHelperCalls;
#endif
			const u32 shift = addr & 7;
			const u64 mem = memRead64(addr & ~7u);
			if (guest_reg == 0)
				return;

			cpuRegs.GPR.r[guest_reg].UD[0] = (cpuRegs.GPR.r[guest_reg].UD[0] & LDR_MASK[shift]) |
											 (mem >> LDR_SHIFT[shift]);
		}

		__noinline void VitaEeMemRead128Aligned(u32 addr, u32 guest_reg)
		{
			// PCSX2 owner: R5900OpcodeImpl.cpp::LQ().
#if defined(VITASX2_QEMU_VALIDATION)
			++g_qemuQwordGprMemoryHelperCalls;
#endif
			GPR_reg* dest = (guest_reg == 0) ? &s_lq_zero_sink : &cpuRegs.GPR.r[guest_reg];
			memRead128(addr & ~0x0fu, dest->UQ);
		}

		__noinline void VitaEeMemReadCop1Word(u32 addr, u32 guest_reg)
		{
			// PCSX2 owner: FPU.cpp::LWC1().
#if defined(VITASX2_QEMU_VALIDATION)
			++g_qemuCop1MemoryHelperCalls;
#endif
			if (addr & 3)
			{
				Console.Error("FPU (LWC1 Opcode): Invalid Unaligned Memory Address");
				return;
			}

			fpuRegs.fpr[guest_reg].UL = memRead32(addr);
		}

		__noinline void VitaEeMemReadVu0Quad(u32 addr, u32 guest_reg)
		{
			// PCSX2 owner: VU0.cpp::LQC2().
#if defined(VITASX2_QEMU_VALIDATION)
			++g_qemuQwordCop2MemoryHelperCalls;
#endif
			vu0Sync();
			if (guest_reg != 0)
				memRead128(addr, VU0.VF[guest_reg].UQ);
			else
			{
				mem128_t sink;
				memRead128(addr, sink);
			}
		}

		__noinline void VitaEeMemWrite8(u32 addr, u32 value)
		{
			// PCSX2 owner: R5900OpcodeImpl.cpp::SB().
#if defined(VITASX2_QEMU_VALIDATION)
			++g_qemuByteMemoryHelperCalls;
#endif
			memWrite8(addr, static_cast<u8>(value));
		}

		__noinline void VitaEeMemWrite16Checked(u32 addr, u32 value)
		{
			// PCSX2 owner: R5900OpcodeImpl.cpp::SH().
#if defined(VITASX2_QEMU_VALIDATION)
			++g_qemuHalfwordMemoryHelperCalls;
#endif
			if (addr & 1)
				VitaEeRaiseAddressError(addr, true);

			memWrite16(addr, static_cast<u16>(value));
		}

		__noinline void VitaEeMemWrite32Checked(u32 addr, u32 value)
		{
			// PCSX2 owner: R5900OpcodeImpl.cpp::SW().
#if defined(VITASX2_QEMU_VALIDATION)
			++g_qemuWordMemoryHelperCalls;
#endif
			if (addr & 3)
				VitaEeRaiseAddressError(addr, true);

			memWrite32(addr, value);
		}

		__noinline void VitaEeMemWriteWordLeft(u32 addr, u32 guest_reg)
		{
			// PCSX2 owner: R5900OpcodeImpl.cpp::SWL().
#if defined(VITASX2_QEMU_VALIDATION)
			++g_qemuPartialWordMemoryHelperCalls;
#endif
			const u32 shift = addr & 3;
			const u32 aligned = addr & ~3u;
			const u32 mem = memRead32(aligned);
			memWrite32(aligned, (cpuRegs.GPR.r[guest_reg].UL[0] >> SWL_SHIFT[shift]) | (mem & SWL_MASK[shift]));
		}

		__noinline void VitaEeMemWriteWordRight(u32 addr, u32 guest_reg)
		{
			// PCSX2 owner: R5900OpcodeImpl.cpp::SWR().
#if defined(VITASX2_QEMU_VALIDATION)
			++g_qemuPartialWordMemoryHelperCalls;
#endif
			const u32 shift = addr & 3;
			const u32 aligned = addr & ~3u;
			const u32 mem = memRead32(aligned);
			memWrite32(aligned, (cpuRegs.GPR.r[guest_reg].UL[0] << SWR_SHIFT[shift]) | (mem & SWR_MASK[shift]));
		}

		__noinline void VitaEeMemWrite64Checked(u32 addr, u32 low, u32 high)
		{
			// PCSX2 owner: R5900OpcodeImpl.cpp::SD().
#if defined(VITASX2_QEMU_VALIDATION)
			++g_qemuDwordMemoryHelperCalls;
#endif
			if (addr & 7)
				VitaEeRaiseAddressError(addr, true);

			memWrite64(addr, (static_cast<u64>(high) << 32) | low);
		}

		__noinline void VitaEeMemWriteDwordLeft(u32 addr, u32 guest_reg)
		{
			// PCSX2 owner: R5900OpcodeImpl.cpp::SDL().
#if defined(VITASX2_QEMU_VALIDATION)
			++g_qemuPartialDwordMemoryHelperCalls;
#endif
			const u32 shift = addr & 7;
			const u32 aligned = addr & ~7u;
			const u64 mem = (cpuRegs.GPR.r[guest_reg].UD[0] >> SDL_SHIFT[shift]) |
							(memRead64(aligned) & SDL_MASK[shift]);
			memWrite64(aligned, mem);
		}

		__noinline void VitaEeMemWriteDwordRight(u32 addr, u32 guest_reg)
		{
			// PCSX2 owner: R5900OpcodeImpl.cpp::SDR().
#if defined(VITASX2_QEMU_VALIDATION)
			++g_qemuPartialDwordMemoryHelperCalls;
#endif
			const u32 shift = addr & 7;
			const u32 aligned = addr & ~7u;
			const u64 mem = (cpuRegs.GPR.r[guest_reg].UD[0] << SDR_SHIFT[shift]) |
							(memRead64(aligned) & SDR_MASK[shift]);
			memWrite64(aligned, mem);
		}

		__noinline void VitaEeMemWrite128Aligned(u32 addr, u32 guest_reg)
		{
			// PCSX2 owner: R5900OpcodeImpl.cpp::SQ().
#if defined(VITASX2_QEMU_VALIDATION)
			++g_qemuQwordGprMemoryHelperCalls;
#endif
			memWrite128(addr & ~0x0fu, cpuRegs.GPR.r[guest_reg].UQ);
		}

		__noinline void VitaEeMemWriteCop1Word(u32 addr, u32 guest_reg)
		{
			// PCSX2 owner: FPU.cpp::SWC1().
#if defined(VITASX2_QEMU_VALIDATION)
			++g_qemuCop1MemoryHelperCalls;
#endif
			if (addr & 3)
			{
				Console.Error("FPU (SWC1 Opcode): Invalid Unaligned Memory Address");
				return;
			}

			memWrite32(addr, fpuRegs.fpr[guest_reg].UL);
		}

		__noinline void VitaEeMemWriteVu0Quad(u32 addr, u32 guest_reg)
		{
			// PCSX2 owner: VU0.cpp::SQC2().
#if defined(VITASX2_QEMU_VALIDATION)
			++g_qemuQwordCop2MemoryHelperCalls;
#endif
			vu0Sync();
			memWrite128(addr, VU0.VF[guest_reg].UQ);
		}

		__noinline void VitaEeDivSigned(u32 rs, u32 rt)
		{
			// PCSX2 owner: R5900OpcodeImpl.cpp::DIV(). Cortex-A9 has no integer
			// divide instruction, so arbitrary division falls back here.
#if defined(VITASX2_QEMU_VALIDATION)
			++g_qemuDivSignedHelperCalls;
#endif
			if (rs == 0x80000000u && rt == 0xffffffffu)
			{
				cpuRegs.LO.SD[0] = static_cast<s32>(0x80000000);
				cpuRegs.HI.SD[0] = 0;
			}
			else if (static_cast<s32>(rt) != 0)
			{
				cpuRegs.LO.SD[0] = static_cast<s32>(rs) / static_cast<s32>(rt);
				cpuRegs.HI.SD[0] = static_cast<s32>(rs) % static_cast<s32>(rt);
			}
			else
			{
				cpuRegs.LO.SD[0] = (static_cast<s32>(rs) < 0) ? 1 : -1;
				cpuRegs.HI.SD[0] = static_cast<s32>(rs);
			}
		}

		__noinline void VitaEeDivUnsigned(u32 rs, u32 rt)
		{
			// PCSX2 owner: R5900OpcodeImpl.cpp::DIVU().
#if defined(VITASX2_QEMU_VALIDATION)
			++g_qemuDivUnsignedHelperCalls;
#endif
			if (rt != 0)
			{
				cpuRegs.LO.SD[0] = static_cast<s32>(rs / rt);
				cpuRegs.HI.SD[0] = static_cast<s32>(rs % rt);
			}
			else
			{
				cpuRegs.LO.SD[0] = -1;
				cpuRegs.HI.SD[0] = static_cast<s32>(rs);
			}
		}

		__noinline void VitaEeDivSigned1(u32 rs, u32 rt)
		{
			// PCSX2 owners: MMI.cpp::DIV1() and
			// x86/ix86-32/iR5900MultDiv.cpp::recDIV1().
#if defined(VITASX2_QEMU_VALIDATION)
			++g_qemuDivSigned1HelperCalls;
#endif
			if (rs == 0x80000000u && rt == 0xffffffffu)
			{
				cpuRegs.LO.SD[1] = static_cast<s32>(0x80000000);
				cpuRegs.HI.SD[1] = 0;
			}
			else if (static_cast<s32>(rt) != 0)
			{
				cpuRegs.LO.SD[1] = static_cast<s32>(rs) / static_cast<s32>(rt);
				cpuRegs.HI.SD[1] = static_cast<s32>(rs) % static_cast<s32>(rt);
			}
			else
			{
				cpuRegs.LO.SD[1] = (static_cast<s32>(rs) < 0) ? 1 : -1;
				cpuRegs.HI.SD[1] = static_cast<s32>(rs);
			}
		}

		__noinline void VitaEeDivUnsigned1(u32 rs, u32 rt)
		{
			// PCSX2 owners: MMI.cpp::DIVU1() and
			// x86/ix86-32/iR5900MultDiv.cpp::recDIVU1().
#if defined(VITASX2_QEMU_VALIDATION)
			++g_qemuDivUnsigned1HelperCalls;
#endif
			if (rt != 0)
			{
				cpuRegs.LO.SD[1] = static_cast<s32>(rs / rt);
				cpuRegs.HI.SD[1] = static_cast<s32>(rs % rt);
			}
			else
			{
				cpuRegs.LO.SD[1] = -1;
				cpuRegs.HI.SD[1] = static_cast<s32>(rs);
			}
		}

		__noinline void VitaEePackedDivSignedWords(u32 rs0, u32 rt0, u32 rs2, u32 rt2)
		{
			// PCSX2 owners: MMI.cpp::PDIVW() and x86/iMMI.cpp::recPDIVW().
#if defined(VITASX2_QEMU_VALIDATION)
			++g_qemuPackedDivSignedWordHelperCalls;
#endif
			const auto divide_lane = [](unsigned lane, u32 rs, u32 rt) {
				if (rs == 0x80000000u && rt == 0xffffffffu)
				{
					cpuRegs.LO.SD[lane] = static_cast<s32>(0x80000000);
					cpuRegs.HI.SD[lane] = 0;
				}
				else if (static_cast<s32>(rt) != 0)
				{
					cpuRegs.LO.SD[lane] = static_cast<s32>(rs) / static_cast<s32>(rt);
					cpuRegs.HI.SD[lane] = static_cast<s32>(rs) % static_cast<s32>(rt);
				}
				else
				{
					cpuRegs.LO.SD[lane] = (static_cast<s32>(rs) < 0) ? 1 : -1;
					cpuRegs.HI.SD[lane] = static_cast<s32>(rs);
				}
			};

			divide_lane(0, rs0, rt0);
			divide_lane(1, rs2, rt2);
		}

		__noinline void VitaEePackedDivUnsignedWords(u32 rs0, u32 rt0, u32 rs2, u32 rt2)
		{
			// PCSX2 owners: MMI.cpp::PDIVUW() and x86/iMMI.cpp::recPDIVUW().
#if defined(VITASX2_QEMU_VALIDATION)
			++g_qemuPackedDivUnsignedWordHelperCalls;
#endif
			const auto divide_lane = [](unsigned lane, u32 rs, u32 rt) {
				if (rt != 0)
				{
					cpuRegs.LO.SD[lane] = static_cast<s32>(rs / rt);
					cpuRegs.HI.SD[lane] = static_cast<s32>(rs % rt);
				}
				else
				{
					cpuRegs.LO.SD[lane] = -1;
					cpuRegs.HI.SD[lane] = static_cast<s32>(rs);
				}
			};

			divide_lane(0, rs0, rt0);
			divide_lane(1, rs2, rt2);
		}

		__noinline void VitaEePackedDivSignedWordsByHalfword(u32 op)
		{
			// PCSX2 owner: MMI.cpp::PDIVBW().
#if defined(VITASX2_QEMU_VALIDATION)
			++g_qemuPackedDivWordByHalfwordHelperCalls;
#endif
			const unsigned rs = RS(op);
			const unsigned rt = RT(op);
			const u16 raw_divisor = cpuRegs.GPR.r[rt].US[0];
			const s16 divisor = static_cast<s16>(raw_divisor);
			for (unsigned lane = 0; lane < 4; lane++)
			{
				const s32 dividend = cpuRegs.GPR.r[rs].SL[lane];
				if (static_cast<u32>(dividend) == 0x80000000u && raw_divisor == 0xffffu)
				{
					cpuRegs.LO.SL[lane] = static_cast<s32>(0x80000000);
					cpuRegs.HI.SL[lane] = 0;
				}
				else if (raw_divisor != 0)
				{
					cpuRegs.LO.SL[lane] = dividend / divisor;
					cpuRegs.HI.SL[lane] = dividend % divisor;
				}
				else
				{
					cpuRegs.LO.SL[lane] = (dividend < 0) ? 1 : -1;
					cpuRegs.HI.SL[lane] = dividend;
				}
			}
		}

	} // namespace

	static_assert(GprOffset(31) + sizeof(GPR_reg) <= 0x0fff);
	static_assert(HI_OFFSET + sizeof(GPR_reg) <= 0x0fff);
	static_assert(LO_OFFSET + sizeof(GPR_reg) <= 0x0fff);
	static_assert(Cp0Offset(31) + sizeof(u32) <= 0x0fff);
	static_assert(FprOffset(31) + sizeof(FPRreg) <= 0x0fff);
	static_assert(FprcOffset(31) + sizeof(u32) <= 0x0fff);
	static_assert(FPU_ACC_OFFSET + sizeof(FPRreg) <= 0x0fff);
	static_assert(FPU_ACCFLAG_OFFSET + sizeof(u32) <= 0x0fff);
	static_assert(SA_OFFSET + sizeof(u32) <= 0x0fff);
	static_assert(PC_OFFSET + sizeof(u32) <= 0x0fff);
	static_assert(CODE_OFFSET + sizeof(u32) <= 0x0fff);
	static_assert(PERF_OFFSET + sizeof(PERFregs) <= 0x0fff);
	static_assert(LAST_PERF_CYCLE_OFFSET + 2 * sizeof(u64) <= 0x0fff);
	static_assert(CYCLE_OFFSET + sizeof(u64) <= 0x0fff);
	static_assert(BRANCH_OFFSET + sizeof(int) <= 0x0fff);
	static_assert(NEXT_EVENT_OFFSET + sizeof(u64) <= 0x0fff);
	static_assert(LAST_COP0_CYCLE_OFFSET + sizeof(u64) <= 0x0fff);
	static_assert(TLB_ENTRY_COUNT == 48);
	static_assert(sizeof(vtlb_private::VTLBVirtual) == sizeof(u32));
	static_assert(TLB_ENTRY_SIZE == 16);
	static_assert(TLB_PAGE_MASK_OFFSET == 0);
	static_assert(TLB_ENTRY_HI_OFFSET == 4);
	static_assert(TLB_ENTRY_LO0_OFFSET == 8);
	static_assert(TLB_ENTRY_LO1_OFFSET == 12);

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
			case 0x08: // ADDI, owned by R5900OpcodeImpl.cpp::ADDI().
			case 0x09: // ADDIU, owned by R5900OpcodeImpl.cpp::ADDIU().
			case 0x0a: // SLTI, owned by R5900OpcodeImpl.cpp::SLTI().
			case 0x0b: // SLTIU, owned by R5900OpcodeImpl.cpp::SLTIU().
			case 0x0c: // ANDI, owned by R5900OpcodeImpl.cpp::ANDI().
			case 0x0d: // ORI, owned by R5900OpcodeImpl.cpp::ORI().
			case 0x0e: // XORI, owned by R5900OpcodeImpl.cpp::XORI().
			case 0x0f: // LUI, owned by R5900OpcodeImpl.cpp::LUI().
				return true;
			case 0x10: // COP0 helper-backed system ops, owned by COP0.cpp and x86/iCOP0.cpp.
				return CanCompileCOP0(op);
			case 0x11: // COP1 helper-backed scalar/control ops, owned by FPU.cpp and x86/iFPU.cpp.
				return CanCompileCOP1(op);
			case 0x18: // DADDI, owned by R5900OpcodeImpl.cpp::DADDI().
			case 0x19: // DADDIU, owned by R5900OpcodeImpl.cpp::DADDIU().
			case 0x1a: // LDL, owned by R5900OpcodeImpl.cpp::LDL().
			case 0x1b: // LDR, owned by R5900OpcodeImpl.cpp::LDR().
			case 0x1e: // LQ, owned by R5900OpcodeImpl.cpp::LQ().
			case 0x1f: // SQ, owned by R5900OpcodeImpl.cpp::SQ().
			case 0x20: // LB, owned by R5900OpcodeImpl.cpp::LB().
			case 0x21: // LH, owned by R5900OpcodeImpl.cpp::LH().
			case 0x22: // LWL, owned by R5900OpcodeImpl.cpp::LWL().
			case 0x23: // LW, owned by R5900OpcodeImpl.cpp::LW().
			case 0x24: // LBU, owned by R5900OpcodeImpl.cpp::LBU().
			case 0x25: // LHU, owned by R5900OpcodeImpl.cpp::LHU().
			case 0x26: // LWR, owned by R5900OpcodeImpl.cpp::LWR().
			case 0x27: // LWU, owned by R5900OpcodeImpl.cpp::LWU().
			case 0x28: // SB, owned by R5900OpcodeImpl.cpp::SB().
			case 0x29: // SH, owned by R5900OpcodeImpl.cpp::SH().
			case 0x2a: // SWL, owned by R5900OpcodeImpl.cpp::SWL().
			case 0x2b: // SW, owned by R5900OpcodeImpl.cpp::SW().
			case 0x2c: // SDL, owned by R5900OpcodeImpl.cpp::SDL().
			case 0x2d: // SDR, owned by R5900OpcodeImpl.cpp::SDR().
			case 0x2e: // SWR, owned by R5900OpcodeImpl.cpp::SWR().
				return true;
			case 0x2f: // CACHE known modes, owned by Cache.cpp::CACHE().
				return CanCompileCACHE(op);
			case 0x31: // LWC1, owned by FPU.cpp::LWC1().
			case 0x33: // PREF, owned by R5900OpcodeImpl.cpp::PREF().
			case 0x36: // LQC2, owned by VU0.cpp::LQC2().
			case 0x37: // LD, owned by R5900OpcodeImpl.cpp::LD().
			case 0x39: // SWC1, owned by FPU.cpp::SWC1().
			case 0x3e: // SQC2, owned by VU0.cpp::SQC2().
			case 0x3f: // SD, owned by R5900OpcodeImpl.cpp::SD().
				return true;
			case 0x1c: // MMI scalar mult/div extensions, owned by MMI.cpp and iR5900MultDiv.cpp.
				return CanCompileMMI(op);
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
			case 0x11: // COP1_BC1 branch forms, owned by FPU.cpp::BC1F()/BC1T()/BC1FL()/BC1TL().
				if (((op >> 21) & 0x1f) != 0x08)
					return false;
				switch (RT(op))
				{
					case 0x00:
					case 0x01:
					case 0x02:
					case 0x03:
						return true;
					default:
						return false;
				}
			case 0x10: // COP0_BC0 branch forms, owned by COP0.cpp::BC0F()/BC0T()/BC0FL()/BC0TL().
				if (((op >> 21) & 0x1f) != 0x08)
					return false;
				switch (RT(op))
				{
					case 0x00:
					case 0x01:
					case 0x02:
					case 0x03:
						return true;
					default:
						return false;
				}
			default:
				return false;
			}
		}

	bool BlockCompiler::IsBranchLikely(u32 op)
	{
		// PCSX2 owners: Interpreter.cpp::BEQL()/BNEL()/BLEZL()/BGTZL() and the
		// REGIMM likely forms cancel the delay slot on the not-taken path.
		return IsBranchLikelyOpcode(op);
	}

	bool BlockCompiler::CanCompileDelaySlotOpcode(u32 op)
	{
		// PCSX2 x86/ix86-32/iR5900.cpp::recRecompile() detects branches in
		// delay slots through recompileNextInstruction(true, ...): the delay
		// branch is skipped as generated work and the outer branch still owns
		// the block exit.
		// BREAK is different: R5900OpcodeImpl.cpp::BREAK() is a helper-backed
		// exception path, and Interpreter.cpp::_doBranch_shared() marks
		// cpuRegs.branch before executing it as a delay slot.
		return CanCompileOpcode(op) && !IsDI(op) &&
			   (!RequiresBlockEndAfterOpcode(op) || IsBREAK(op) || IsCounterReadLoad(op));
	}

	bool BlockCompiler::RequiresBlockEndAfterOpcode(u32 op)
	{
		// PCSX2 owners: R5900OpcodeImpl.cpp::LB()/LBU()/LH()/LHU()/LW()
		// force intUpdateCPUCycles() and intEventTest() for EE counter reads.
		// R5900OpcodeImpl.cpp::SYNC() is a no-op, but local EE docs still forbid
		// compiling it inside a branch delay slot, so make it a one-op tail.
		switch (op >> 26)
		{
			case 0x00:
				return (op & 0x3f) == 0x0d || (op & 0x3f) == 0x0f;
			case 0x10:
				return CanCompileCOP0(op) && !IsDI(op) && !IsFastMFC0(op) && !IsFastMTC0(op);
			case 0x11:
				return CanCompileCOP1(op) && !IsFastCOP1InBlock(op);
			case 0x2f:
				return IsHelperCACHE(op);
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
		return m_code.EmitPush(REG_R4 | REG_R5 | REG_R6 | REG_LR) &&
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
		u32 committed_scaled_cycles = 0;
		bool has_branch = false;
		bool has_register_branch_target = false;
		bool has_static_direct_link_target = false;
		bool has_static_conditional_direct_links = false;
		bool has_static_likely_direct_links = false;
		bool branch_is_likely = false;
		bool pending_di_clear = false;
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
				if (pending_di_clear)
					return false;

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
				if (!EmitGsTracePreInstruction(pc))
					return false;
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
					case 0x10:
						branch_target_pc = BranchTarget(pc, op);
						if (branch_is_likely)
							has_static_likely_direct_links = true;
						else
							has_static_conditional_direct_links = true;
						if (!EmitCop0Branch(op))
							return false;
						break;
					case 0x11:
						branch_target_pc = BranchTarget(pc, op);
						if (branch_is_likely)
							has_static_likely_direct_links = true;
						else
							has_static_conditional_direct_links = true;
						if (!EmitCop1Branch(op))
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
			const bool branch_delay_slot = has_branch && i == branch_instruction_index + 1;
			if (!EmitGsTracePreInstruction(pc))
				return false;
			if (IsDI(op))
			{
				// PCSX2 owner: x86/iCOP0.cpp::recDI() compiles the following
				// instruction first, then clears Status.EIE inline. Keep the
				// narrow native path to straight-line cases where the next
				// instruction is definitely emitted by this block.
				if (branch_delay_slot || pending_di_clear || i + 1 >= instruction_count)
					return false;

				const u32 next_op = memRead32(pc + 4);
				if (IsSupportedBranchOpcode(next_op) || RequiresBlockEndAfterOpcode(next_op) ||
					IsCycleCommittingFastCOP0(next_op))
				{
					return false;
				}

				pending_di_clear = true;
				continue;
			}
			if (!EmitOpcode(op, pc, raw_cycles, event_exit, branch_delay_slot))
			{
#if defined(VITASX2_QEMU_VALIDATION)
				std::printf("a32-block-compile-failed index=%u pc=%08x op=%08x code=%zu\n",
					i, pc, op, m_code.Size());
#endif
				return false;
			}

			if (pending_di_clear)
			{
				if (!EmitDIDelayedStatusClear())
					return false;
				pending_di_clear = false;
			}

			if (IsCycleCommittingFastCOP0(op))
			{
				const u32 committed = ScaleBlockCycles(raw_cycles);
				committed_scaled_cycles += committed;
				raw_cycles = RawCycleRemainderAfterClear(raw_cycles);
			}

			if (IsBREAK(op) && !branch_delay_slot)
			{
				if (scaled_cycles)
					*scaled_cycles = committed_scaled_cycles + ScaleBlockCycles(raw_cycles);
				return true;
			}

			if ((op >> 26) == 0x10 && CanCompileCOP0(op) && !IsFastMFC0(op) && !IsFastMTC0(op))
			{
				if (scaled_cycles)
					*scaled_cycles = committed_scaled_cycles + ScaleBlockCycles(raw_cycles);
				return true;
			}

			if ((op >> 26) == 0x11 && CanCompileCOP1(op) && !IsFastCOP1InBlock(op))
			{
				if (scaled_cycles)
					*scaled_cycles = committed_scaled_cycles + ScaleBlockCycles(raw_cycles);
				return true;
			}

			if ((op >> 26) == 0x2f && IsHelperCACHE(op))
			{
				if (scaled_cycles)
					*scaled_cycles = committed_scaled_cycles + ScaleBlockCycles(raw_cycles);
				return true;
			}

			if (has_branch && branch_is_likely && i == branch_instruction_index + 1)
			{
				if (!m_code.PatchBranch(branch_likely_skip_delay, m_code.Size(), VitaA32::Condition::EQ))
					return false;
			}
		}

		if (pending_di_clear)
			return false;

		const u32 next_pc = start_pc + instruction_count * 4;
		const u32 block_cycles = ScaleBlockCycles(raw_cycles);
		const u32 branch_likely_not_taken_cycles = ScaleBlockCycles(branch_likely_not_taken_raw_cycles);
		if (scaled_cycles)
			*scaled_cycles = committed_scaled_cycles + block_cycles;

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

	bool BlockCompiler::EmitOpcode(u32 op, u32 pc, u32 raw_cycles_through_instruction,
		const void* event_exit, bool branch_delay_slot)
	{
		switch (op >> 26)
		{
			case 0x00:
				return EmitSPECIAL(op, pc, raw_cycles_through_instruction, event_exit, branch_delay_slot);
			case 0x01: // REGIMM, including MTSAB/MTSAH from R5900OpcodeImpl.cpp.
				return EmitREGIMM(op, pc);
			case 0x08: // ADDI, owned by R5900OpcodeImpl.cpp::ADDI(). The PCSX2
				// recompiler owner x86/ix86-32/iR5900AritImm.cpp::recADDI_() drops
				// the integer-overflow exception, so ADDI compiles exactly like ADDIU.
				return EmitADDIU(op);
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
			case 0x10: // COP0 helper-backed system ops, owned by COP0.cpp and x86/iCOP0.cpp.
				return EmitCOP0(op, pc, raw_cycles_through_instruction, event_exit);
			case 0x11: // COP1 helper-backed scalar/control ops, owned by FPU.cpp and x86/iFPU.cpp.
				return EmitCOP1(op, pc, raw_cycles_through_instruction, event_exit);
			case 0x18: // DADDI, owned by R5900OpcodeImpl.cpp::DADDI(); overflow trap
				// dropped by x86/ix86-32/iR5900AritImm.cpp::recDADDI(), compiled as DADDIU.
				return EmitDADDIU(op);
			case 0x19: // DADDIU, owned by R5900OpcodeImpl.cpp::DADDIU().
				return EmitDADDIU(op);
			case 0x1a: // LDL, owned by R5900OpcodeImpl.cpp::LDL().
				return EmitLDL(op);
			case 0x1b: // LDR, owned by R5900OpcodeImpl.cpp::LDR().
				return EmitLDR(op);
			case 0x1c: // MMI scalar mult/div extensions, owned by MMI.cpp and iR5900MultDiv.cpp.
				return EmitMMI(op);
			case 0x1e: // LQ, owned by R5900OpcodeImpl.cpp::LQ().
				return EmitLQ(op);
			case 0x1f: // SQ, owned by R5900OpcodeImpl.cpp::SQ().
				return EmitSQ(op);
			case 0x20: // LB, owned by R5900OpcodeImpl.cpp::LB().
				return EmitLB(op, pc, raw_cycles_through_instruction, event_exit, branch_delay_slot);
			case 0x21: // LH, owned by R5900OpcodeImpl.cpp::LH().
				return EmitLH(op, pc, raw_cycles_through_instruction, event_exit, branch_delay_slot);
			case 0x22: // LWL, owned by R5900OpcodeImpl.cpp::LWL().
				return EmitLWL(op);
			case 0x23: // LW, owned by R5900OpcodeImpl.cpp::LW().
				return EmitLW(op, pc, raw_cycles_through_instruction, event_exit, branch_delay_slot);
			case 0x24: // LBU, owned by R5900OpcodeImpl.cpp::LBU().
				return EmitLBU(op, pc, raw_cycles_through_instruction, event_exit, branch_delay_slot);
			case 0x25: // LHU, owned by R5900OpcodeImpl.cpp::LHU().
				return EmitLHU(op, pc, raw_cycles_through_instruction, event_exit, branch_delay_slot);
			case 0x26: // LWR, owned by R5900OpcodeImpl.cpp::LWR().
				return EmitLWR(op);
			case 0x27: // LWU, owned by R5900OpcodeImpl.cpp::LWU().
				return EmitLWU(op);
			case 0x28: // SB, owned by R5900OpcodeImpl.cpp::SB().
				return EmitSB(op);
			case 0x29: // SH, owned by R5900OpcodeImpl.cpp::SH().
				return EmitSH(op);
			case 0x2a: // SWL, owned by R5900OpcodeImpl.cpp::SWL().
				return EmitSWL(op);
			case 0x2b: // SW, owned by R5900OpcodeImpl.cpp::SW().
				return EmitSW(op);
			case 0x2c: // SDL, owned by R5900OpcodeImpl.cpp::SDL().
				return EmitSDL(op);
			case 0x2d: // SDR, owned by R5900OpcodeImpl.cpp::SDR().
				return EmitSDR(op);
			case 0x2e: // SWR, owned by R5900OpcodeImpl.cpp::SWR().
				return EmitSWR(op);
			case 0x2f: // CACHE, owned by Cache.cpp::CACHE().
				return EmitCACHE(op, pc, raw_cycles_through_instruction, event_exit);
			case 0x31: // LWC1, owned by FPU.cpp::LWC1().
				return EmitLWC1(op);
			case 0x33: // PREF, owned by R5900OpcodeImpl.cpp::PREF(); PCSX2 no-ops it.
				return true;
			case 0x36: // LQC2, owned by VU0.cpp::LQC2().
				return EmitLQC2(op);
			case 0x37: // LD, owned by R5900OpcodeImpl.cpp::LD().
				return EmitLD(op);
			case 0x39: // SWC1, owned by FPU.cpp::SWC1().
				return EmitSWC1(op);
			case 0x3e: // SQC2, owned by VU0.cpp::SQC2().
				return EmitSQC2(op);
			case 0x3f: // SD, owned by R5900OpcodeImpl.cpp::SD().
				return EmitSD(op);
			default:
				return false;
		}
	}

	bool BlockCompiler::EndBlockReturn(u8 value)
	{
		return m_code.EmitMovImm8(0, value) &&
			   m_code.EmitPop(REG_R4 | REG_R5 | REG_R6 | REG_PC);
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
			!m_code.EmitPop(REG_R4 | REG_R5 | REG_R6 | REG_PC))
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

			if (!m_code.EmitPop(REG_R4 | REG_R5 | REG_R6 | REG_LR))
				return false;

			const size_t fallthrough_target_offset = m_code.Size();
			if (!m_code.EmitMovImm32(HOST_TMP4, static_cast<u32>(reinterpret_cast<uptr>(direct_exit))) ||
				!m_code.EmitBx(HOST_TMP4))
			{
				return false;
			}

			const size_t taken_tail_target = m_code.Size();
			if (!m_code.PatchBranch(taken_tail, taken_tail_target, VitaA32::Condition::NE) ||
				!m_code.EmitPop(REG_R4 | REG_R5 | REG_R6 | REG_LR))
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

		if (!m_code.EmitPop(REG_R4 | REG_R5 | REG_R6 | REG_LR))
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
			!m_code.EmitPop(REG_R4 | REG_R5 | REG_R6 | REG_PC))
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

			if (!m_code.EmitPop(REG_R4 | REG_R5 | REG_R6 | REG_LR))
				return false;

			const size_t not_taken_target_offset = m_code.Size();
			if (!m_code.EmitMovImm32(HOST_TMP4, static_cast<u32>(reinterpret_cast<uptr>(direct_exit))) ||
				!m_code.EmitBx(HOST_TMP4))
			{
				return false;
			}

			const size_t taken_tail_target = m_code.Size();
			if (!m_code.PatchBranch(taken_tail, taken_tail_target, VitaA32::Condition::NE) ||
				!m_code.EmitPop(REG_R4 | REG_R5 | REG_R6 | REG_LR))
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

		if (!m_code.EmitPop(REG_R4 | REG_R5 | REG_R6 | REG_LR))
			return false;

		return m_code.EmitMovImm32(HOST_TMP4, static_cast<u32>(reinterpret_cast<uptr>(direct_exit))) &&
			   m_code.EmitBx(HOST_TMP4) &&
			   m_code.PatchBranch(direct_branch, direct_target, VitaA32::Condition::MI);
	}

	bool BlockCompiler::EmitSPECIAL(u32 op, u32 pc, u32 raw_cycles_through_instruction,
		const void* event_exit, bool branch_delay_slot)
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
			case 0x0d: // BREAK, owned by R5900OpcodeImpl.cpp::BREAK().
				return EmitBREAK(op, pc, raw_cycles_through_instruction, event_exit, branch_delay_slot);
			case 0x0f: // SYNC, owned by R5900OpcodeImpl.cpp::SYNC(); PCSX2 no-ops it.
				return true;
			case 0x10: // MFHI, owned by R5900OpcodeImpl.cpp::MFHI().
				return EmitMFHI(op);
			case 0x11: // MTHI, owned by R5900OpcodeImpl.cpp::MTHI().
				return EmitMTHI(op);
			case 0x12: // MFLO, owned by R5900OpcodeImpl.cpp::MFLO().
				return EmitMFLO(op);
			case 0x13: // MTLO, owned by R5900OpcodeImpl.cpp::MTLO().
				return EmitMTLO(op);
			case 0x14: // DSLLV, owned by R5900OpcodeImpl.cpp::DSLLV().
				return EmitDSLLV(op);
			case 0x16: // DSRLV, owned by R5900OpcodeImpl.cpp::DSRLV().
				return EmitDSRLV(op);
			case 0x17: // DSRAV, owned by R5900OpcodeImpl.cpp::DSRAV().
				return EmitDSRAV(op);
			case 0x18: // MULT, owned by R5900OpcodeImpl.cpp::MULT().
				return EmitMULT(op);
			case 0x19: // MULTU, owned by R5900OpcodeImpl.cpp::MULTU().
				return EmitMULTU(op);
			case 0x1a: // DIV, owned by R5900OpcodeImpl.cpp::DIV().
				return EmitDIV(op);
			case 0x1b: // DIVU, owned by R5900OpcodeImpl.cpp::DIVU().
				return EmitDIVU(op);
			case 0x20: // ADD, owned by R5900OpcodeImpl.cpp::ADD(). The PCSX2
				// recompiler owner x86/ix86-32/iR5900Arit.cpp::recADD_() drops the
				// integer-overflow exception, so ADD compiles exactly like ADDU.
				return EmitADDU(op);
			case 0x21: // ADDU, owned by R5900OpcodeImpl.cpp::ADDU().
				return EmitADDU(op);
			case 0x22: // SUB, owned by R5900OpcodeImpl.cpp::SUB(); overflow trap
				// dropped by x86/ix86-32/iR5900Arit.cpp::recSUB_(), compiled as SUBU.
				return EmitSUBU(op);
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
			case 0x2c: // DADD, owned by R5900OpcodeImpl.cpp::DADD(); overflow trap
				// dropped by x86/ix86-32/iR5900Arit.cpp::recDADD_(), compiled as DADDU.
				return EmitDADDU(op);
			case 0x2d: // DADDU, owned by R5900OpcodeImpl.cpp::DADDU().
				return EmitDADDU(op);
			case 0x2e: // DSUB, owned by R5900OpcodeImpl.cpp::DSUB(); overflow trap
				// dropped by x86/ix86-32/iR5900Arit.cpp::recDSUB_(), compiled as DSUBU.
				return EmitDSUBU(op);
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

	bool BlockCompiler::EmitCOP0(u32 op, u32 pc, u32 raw_cycles_through_instruction, const void* event_exit)
	{
		// PCSX2 owner: x86/iCOP0.cpp. CP0_RECOMPILE keeps ordinary MFC0/MTC0,
		// Count, perf-counter direct cases, straight-line delayed DI, and TLB
		// read/probe ops inside generated code. MTC0 Status calls
		// WriteCP0Status() in-block after committing cycles, and EI emits its
		// Status.EIE/event scheduling directly before the required event tail.
		// TLB writes, ERET, and the remaining perf helpers keep the event tail
		// until their exact side effects are ported directly.
		using namespace R5900::Interpreter::OpcodeImpl::COP0;
			switch ((op >> 21) & 0x1f)
			{
				case 0x00: // MFC0, owned by COP0.cpp::MFC0().
					if (IsFastMFC0(op))
						return EmitMFC0Fast(op, raw_cycles_through_instruction);
					return EmitSystemHelperEventExit(op, pc + 4, raw_cycles_through_instruction,
						reinterpret_cast<const void*>(&MFC0), event_exit);
				case 0x04: // MTC0, owned by COP0.cpp::MTC0().
					if (IsFastMTC0(op))
						return EmitMTC0Fast(op, raw_cycles_through_instruction);
					return EmitSystemHelperEventExit(op, pc + 4, raw_cycles_through_instruction,
						reinterpret_cast<const void*>(&MTC0), event_exit);
			case 0x10: // COP0_C0 class, owned by R5900OpcodeTables.cpp::tbl_COP0_C0.
				switch (op & 0x3f)
				{
					case 0x01: // TLBR, owned by COP0.cpp::TLBR().
						return EmitTLBREventExit(op, pc + 4, raw_cycles_through_instruction, event_exit);
					case 0x02: // TLBWI, owned by COP0.cpp::TLBWI().
						return EmitSystemHelperEventExit(op, pc + 4, raw_cycles_through_instruction,
							reinterpret_cast<const void*>(&TLBWI), event_exit, true);
					case 0x06: // TLBWR, owned by COP0.cpp::TLBWR().
						return EmitSystemHelperEventExit(op, pc + 4, raw_cycles_through_instruction,
							reinterpret_cast<const void*>(&TLBWR), event_exit, true);
					case 0x08: // TLBP, owned by COP0.cpp::TLBP().
						return EmitTLBPEventExit(op, pc + 4, raw_cycles_through_instruction, event_exit);
					case 0x18: // ERET, owned by COP0.cpp::ERET().
						return EmitERETEventExit(op, raw_cycles_through_instruction, event_exit);
					case 0x38: // EI, owned by COP0.cpp::EI().
						return EmitEIEventExit(op, pc + 4, raw_cycles_through_instruction, event_exit);
					default:
						return false;
				}
			default:
				return false;
		}
	}

	bool BlockCompiler::EmitMFC0Fast(u32 op, u32 raw_cycles_through_instruction)
	{
		// PCSX2 owner: x86/iCOP0.cpp::recMFC0() under CP0_RECOMPILE. For all
		// ordinary CP0 registers, Count, and MFPS/PCCR, it stays inside the block.
		// PCR0/PCR1 reads still use the helper/event tail to run COP0_UpdatePCCR().
		// rd 24 only logs in PCSX2, so it is a no-op here.
		const unsigned rt = RT(op);
		const unsigned rd = RD(op);
		if (rd == 9)
			return EmitMFC0CountFast(op, ScaleBlockCycles(raw_cycles_through_instruction));

		if (rt == 0 || rd == 24)
			return true;

		if (rd == 25)
		{
			return m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(PERF_OFFSET)) &&
				   m_code.EmitMovRegShiftImm(HOST_TMP1, HOST_TMP0, VitaA32::ShiftType::ASR, 31) &&
				   EmitStoreGpr64(rt, HOST_TMP0, HOST_TMP1);
		}

		const size_t cp0_offset = Cp0Offset(rd);
		return m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(cp0_offset)) &&
			   m_code.EmitMovRegShiftImm(HOST_TMP1, HOST_TMP0, VitaA32::ShiftType::ASR, 31) &&
			   EmitStoreGpr64(rt, HOST_TMP0, HOST_TMP1);
	}

		bool BlockCompiler::EmitMFC0CountFast(u32 op, u32 scaled_cycles_through_instruction)
		{
		// PCSX2 owner: x86/iCOP0.cpp::recMFC0() rd 9. It commits cycles through
		// scaleblockcycles_clear(), updates CP0.Count from cycle-lastCOP0Cycle
		// even when RT is zero, then returns the sign-extended Count value.
		const unsigned rt = RT(op);
		if (scaled_cycles_through_instruction == 0)
			return false;

		if (!m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(CYCLE_OFFSET)) ||
			!m_code.EmitLdrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(CYCLE_OFFSET + sizeof(u32))))
		{
			return false;
		}

		if (scaled_cycles_through_instruction <= 255)
		{
			if (!m_code.EmitAddImm8(HOST_TMP0, HOST_TMP0, static_cast<u8>(scaled_cycles_through_instruction), true))
				return false;
		}
		else
		{
			if (!m_code.EmitMovImm32(HOST_TMP2, scaled_cycles_through_instruction) ||
				!m_code.EmitAddReg(HOST_TMP0, HOST_TMP0, HOST_TMP2, true))
			{
				return false;
			}
		}

		if (!m_code.EmitAdcImm8(HOST_TMP1, HOST_TMP1, 0) ||
			!m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(CYCLE_OFFSET)) ||
			!m_code.EmitStrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(CYCLE_OFFSET + sizeof(u32))) ||
			!m_code.EmitLdrImm12(HOST_TMP2, HOST_CPU_REGS, static_cast<u16>(LAST_COP0_CYCLE_OFFSET)) ||
			!m_code.EmitSubReg(HOST_TMP2, HOST_TMP0, HOST_TMP2) ||
			!m_code.EmitLdrImm12(HOST_TMP3, HOST_CPU_REGS, static_cast<u16>(Cp0Offset(9))) ||
			!m_code.EmitAddReg(HOST_TMP3, HOST_TMP3, HOST_TMP2) ||
			!m_code.EmitStrImm12(HOST_TMP3, HOST_CPU_REGS, static_cast<u16>(Cp0Offset(9))) ||
			!m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(LAST_COP0_CYCLE_OFFSET)) ||
			!m_code.EmitStrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(LAST_COP0_CYCLE_OFFSET + sizeof(u32))))
		{
			return false;
		}

		if (rt == 0)
			return true;

			return m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP3, VitaA32::ShiftType::ASR, 31) &&
				   EmitStoreGpr64(rt, HOST_TMP3, HOST_TMP2);
		}

		bool BlockCompiler::EmitMTC0Fast(u32 op, u32 raw_cycles_through_instruction)
		{
			// PCSX2 owner: x86/iCOP0.cpp::recMTC0() under CP0_RECOMPILE.
			// Status calls PCSX2's WriteCP0Status() in-block after committing
			// cycles, matching the x86 helper-call shape while avoiding an
			// artificial event-tail split. MTPS/PCCR still uses the helper/event
			// tail because it calls COP0_UpdatePCCR() and COP0_DiagnosticPCCR().
			// Count and MTPC0/MTPC1 commit cycles here using the same
			// scaleblockcycles_clear() cadence.
			const unsigned rt = RT(op);
			const unsigned rd = RD(op);
			const auto load_rt_low = [this, rt](unsigned host_reg) {
				if (rt == 0)
					return m_code.EmitMovImm8(host_reg, 0);
				return m_code.EmitLdrImm12(host_reg, HOST_CPU_REGS, static_cast<u16>(GprOffset(rt)));
			};

			switch (rd)
			{
				case 0x09: // Count
				{
					const u32 cycles = ScaleBlockCycles(raw_cycles_through_instruction);
					if (cycles == 0 ||
						!EmitAddScaledCyclesToCpu(cycles) ||
						!load_rt_low(HOST_TMP2))
					{
						return false;
					}

					return m_code.EmitStrImm12(HOST_TMP2, HOST_CPU_REGS, static_cast<u16>(Cp0Offset(9))) &&
						   m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(LAST_COP0_CYCLE_OFFSET)) &&
						   m_code.EmitStrImm12(HOST_TMP1, HOST_CPU_REGS,
							   static_cast<u16>(LAST_COP0_CYCLE_OFFSET + sizeof(u32)));
				}
				case 0x0c: // Status
				{
					const u32 cycles = ScaleBlockCycles(raw_cycles_through_instruction);
					if (cycles == 0 ||
						!EmitAddScaledCyclesToCpu(cycles) ||
						!load_rt_low(HOST_TMP0))
					{
						return false;
					}

					return m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&WriteCP0Status));
				}
				case 0x10: // Config
					return load_rt_low(HOST_TMP0) &&
						   m_code.EmitMovImm32(HOST_TMP1, 0xfffff03fu) &&
						   m_code.EmitAndReg(HOST_TMP0, HOST_TMP0, HOST_TMP1) &&
						   m_code.EmitMovImm32(HOST_TMP1, 0x00000440u) &&
						   m_code.EmitOrrReg(HOST_TMP0, HOST_TMP0, HOST_TMP1) &&
						   m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(Cp0Offset(16)));
				case 0x18: // Breakpoint debug registers
					return true;
				case 0x19: // Perf counters
					if ((op & 1u) == 0)
						return true; // Non-zero even sels are no-op; sel 0 is helper-backed MTPS/PCCR.

					{
						const u32 cycles = ScaleBlockCycles(raw_cycles_through_instruction);
						const bool pcr1 = (op & 2u) != 0;
						const size_t pcr_offset = pcr1 ? PERF_PCR1_OFFSET : PERF_PCR0_OFFSET;
						const size_t last_perf_offset = LAST_PERF_CYCLE_OFFSET + (pcr1 ? sizeof(u64) : 0);
						if (cycles == 0 ||
							!EmitAddScaledCyclesToCpu(cycles) ||
							!load_rt_low(HOST_TMP2))
						{
							return false;
						}

						return m_code.EmitStrImm12(HOST_TMP2, HOST_CPU_REGS, static_cast<u16>(pcr_offset)) &&
							   m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(last_perf_offset)) &&
							   m_code.EmitStrImm12(HOST_TMP1, HOST_CPU_REGS,
								   static_cast<u16>(last_perf_offset + sizeof(u32)));
					}
				default:
					return load_rt_low(HOST_TMP0) &&
						   m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(Cp0Offset(rd)));
			}
		}

		bool BlockCompiler::EmitSetNextEventDelta4FromCurrentCycle()
		{
			// PCSX2 owner: R5900.cpp::cpuSetNextEventDelta(4). HOST_TMP0/1
			// must hold the current committed cycle from EmitAddScaledCyclesToCpu().
			constexpr u32 EVENT_DELTA = 4;
			if (!m_code.EmitAddImm8(HOST_TMP2, HOST_TMP0, EVENT_DELTA, true) ||
				!m_code.EmitAdcImm8(HOST_TMP3, HOST_TMP1, 0) ||
				!m_code.EmitLdrImm12(HOST_TMP4, HOST_CPU_REGS, static_cast<u16>(NEXT_EVENT_OFFSET)) ||
				!m_code.EmitSubReg(HOST_TMP4, HOST_TMP4, HOST_TMP0, true) ||
				!m_code.EmitMovImm8(HOST_TMP5, EVENT_DELTA) ||
				!m_code.EmitCmpReg(HOST_TMP4, HOST_TMP5))
			{
				return false;
			}

			const size_t keep_event = m_code.EmitBranchPlaceholder(VitaA32::Condition::LE);
			if (keep_event == static_cast<size_t>(-1))
				return false;

			if (!m_code.EmitStrImm12(HOST_TMP2, HOST_CPU_REGS, static_cast<u16>(NEXT_EVENT_OFFSET)) ||
				!m_code.EmitStrImm12(HOST_TMP3, HOST_CPU_REGS, static_cast<u16>(NEXT_EVENT_OFFSET + sizeof(u32))))
			{
				return false;
			}

			return m_code.PatchBranch(keep_event, m_code.Size(), VitaA32::Condition::LE);
		}

		bool BlockCompiler::EmitEIEventExit(u32 op, u32 next_pc, u32 raw_cycles_through_instruction,
			const void* event_exit)
		{
			// PCSX2 owner: x86/iCOP0.cpp::recEI() must branch after
			// COP0.cpp::EI() so pending interrupts can be tested. Inline the
			// Status.EIE update and cpuSetNextEventDelta(4), but keep the event
			// tail instead of continuing through the block.
			constexpr u32 EI_ALLOWED_MASK = 0x00020006u; // Status._EDI | EXL | ERL
			constexpr u32 EI_KSU_MASK = 0x00000018u;
			constexpr u32 STATUS_EIE_SET_MASK = 0x00010000u;

			if (!event_exit || raw_cycles_through_instruction == 0)
				return false;

			const u32 cycles = ScaleBlockCycles(raw_cycles_through_instruction);
			if (!m_code.EmitMovImm32(HOST_TMP0, op) ||
				!m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(CODE_OFFSET)) ||
				!EmitStorePc(next_pc) ||
				!EmitAddScaledCyclesToCpu(cycles))
			{
				return false;
			}

			if (!m_code.EmitLdrImm12(HOST_TMP2, HOST_CPU_REGS, static_cast<u16>(Cp0Offset(12))) ||
				!m_code.EmitMovImm32(HOST_TMP3, EI_ALLOWED_MASK) ||
				!m_code.EmitAndReg(HOST_TMP4, HOST_TMP2, HOST_TMP3, true))
			{
				return false;
			}

			const size_t set_eie = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (set_eie == static_cast<size_t>(-1))
				return false;

			if (!m_code.EmitMovImm32(HOST_TMP3, EI_KSU_MASK) ||
				!m_code.EmitAndReg(HOST_TMP4, HOST_TMP2, HOST_TMP3, true))
			{
				return false;
			}

			const size_t skip_ei = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (skip_ei == static_cast<size_t>(-1))
				return false;

			const size_t set_target = m_code.Size();
			if (!m_code.PatchBranch(set_eie, set_target, VitaA32::Condition::NE) ||
				!m_code.EmitMovImm32(HOST_TMP3, STATUS_EIE_SET_MASK) ||
				!m_code.EmitOrrReg(HOST_TMP2, HOST_TMP2, HOST_TMP3) ||
				!m_code.EmitStrImm12(HOST_TMP2, HOST_CPU_REGS, static_cast<u16>(Cp0Offset(12))) ||
				!EmitSetNextEventDelta4FromCurrentCycle())
			{
				return false;
			}

			if (!m_code.PatchBranch(skip_ei, m_code.Size(), VitaA32::Condition::NE))
				return false;

			return m_code.EmitCallAbsolute(event_exit) &&
				   m_code.EmitPop(REG_R4 | REG_R5 | REG_R6 | REG_PC);
		}

		bool BlockCompiler::EmitTLBREventExit(u32 op, u32 next_pc, u32 raw_cycles_through_instruction,
			const void* event_exit)
		{
			// PCSX2 owners: COP0.cpp::TLBR(), x86/iCOP0.cpp::recTLBR().
			// TLBR reads the architectural TLB table only, so it can be emitted
			// directly while keeping the current event-test tail.
			if (!event_exit || raw_cycles_through_instruction == 0)
				return false;

			const u32 cycles = ScaleBlockCycles(raw_cycles_through_instruction);
			if (!m_code.EmitMovImm32(HOST_TMP0, op) ||
				!m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(CODE_OFFSET)) ||
				!EmitStorePc(next_pc) ||
				!EmitAddScaledCyclesToCpu(cycles) ||
				!m_code.EmitLdrImm12(HOST_TMP2, HOST_CPU_REGS, static_cast<u16>(Cp0Offset(0))) ||
				!m_code.EmitAndImm8(HOST_TMP2, HOST_TMP2, 0x3f) ||
				!m_code.EmitMovImm8(HOST_TMP3, static_cast<u8>(TLB_ENTRY_COUNT)) ||
				!m_code.EmitCmpReg(HOST_TMP2, HOST_TMP3))
			{
				return false;
			}

			const size_t invalid_index = m_code.EmitBranchPlaceholder(VitaA32::Condition::CS);
			if (invalid_index == static_cast<size_t>(-1))
				return false;

			if (!m_code.EmitMovImm32(HOST_TMP3, static_cast<u32>(reinterpret_cast<uptr>(&tlb[0]))) ||
				!m_code.EmitMovRegShiftImm(HOST_TMP4, HOST_TMP2, VitaA32::ShiftType::LSL, 4) ||
				!m_code.EmitAddReg(HOST_TMP3, HOST_TMP3, HOST_TMP4) ||
				!m_code.EmitLdrImm12(HOST_TMP4, HOST_TMP3, static_cast<u16>(TLB_PAGE_MASK_OFFSET)) ||
				!m_code.EmitMovImm32(HOST_TMP5, TLB_PAGE_MASK_REGISTER_MASK) ||
				!m_code.EmitAndReg(HOST_TMP4, HOST_TMP4, HOST_TMP5) ||
				!m_code.EmitStrImm12(HOST_TMP4, HOST_CPU_REGS, static_cast<u16>(Cp0Offset(5))) ||
				!m_code.EmitLdrImm12(HOST_TMP0, HOST_TMP3, static_cast<u16>(TLB_ENTRY_HI_OFFSET)) ||
				!m_code.EmitMovImm32(HOST_TMP5, 0x1f00u) ||
				!m_code.EmitOrrReg(HOST_TMP5, HOST_TMP5, HOST_TMP4) ||
				!m_code.EmitMvnReg(HOST_TMP5, HOST_TMP5) ||
				!m_code.EmitAndReg(HOST_TMP0, HOST_TMP0, HOST_TMP5) ||
				!m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(Cp0Offset(10))) ||
				!m_code.EmitLdrImm12(HOST_TMP0, HOST_TMP3, static_cast<u16>(TLB_ENTRY_LO0_OFFSET)) ||
				!m_code.EmitLdrImm12(HOST_TMP1, HOST_TMP3, static_cast<u16>(TLB_ENTRY_LO1_OFFSET)) ||
				!m_code.EmitAndReg(HOST_TMP2, HOST_TMP0, HOST_TMP1) ||
				!m_code.EmitAndImm8(HOST_TMP2, HOST_TMP2, 1) ||
				!m_code.EmitMovImm32(HOST_TMP5, TLB_TLBR_ENTRY_LO0_MASK) ||
				!m_code.EmitAndReg(HOST_TMP0, HOST_TMP0, HOST_TMP5) ||
				!m_code.EmitOrrReg(HOST_TMP0, HOST_TMP0, HOST_TMP2) ||
				!m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(Cp0Offset(2))) ||
				!m_code.EmitMovImm32(HOST_TMP5, TLB_TLBR_ENTRY_LO1_MASK) ||
				!m_code.EmitAndReg(HOST_TMP1, HOST_TMP1, HOST_TMP5) ||
				!m_code.EmitOrrReg(HOST_TMP1, HOST_TMP1, HOST_TMP2) ||
				!m_code.EmitStrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(Cp0Offset(3))))
			{
				return false;
			}

			if (!m_code.PatchBranch(invalid_index, m_code.Size(), VitaA32::Condition::CS))
				return false;

			return m_code.EmitCallAbsolute(event_exit) &&
				   m_code.EmitPop(REG_R4 | REG_R5 | REG_R6 | REG_PC);
		}

		bool BlockCompiler::EmitTLBPEventExit(u32 op, u32 next_pc, u32 raw_cycles_through_instruction,
			const void* event_exit)
		{
			// PCSX2 owners: COP0.cpp::TLBP(), x86/iCOP0.cpp::recTLBP().
			// Keep COP0.cpp's EntryHi32 bitfield view exactly: VPN2 is the
			// low 19 bits of EntryHi, while ASID is bits 24..31.
			if (!event_exit || raw_cycles_through_instruction == 0)
				return false;

			const u32 cycles = ScaleBlockCycles(raw_cycles_through_instruction);
			if (!m_code.EmitMovImm32(HOST_TMP0, op) ||
				!m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(CODE_OFFSET)) ||
				!EmitStorePc(next_pc) ||
				!EmitAddScaledCyclesToCpu(cycles) ||
				!m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(Cp0Offset(10))) ||
				!m_code.EmitMovImm32(HOST_TMP1, TLB_ENTRY_HI32_VPN2_MASK) ||
				!m_code.EmitAndReg(HOST_TMP3, HOST_TMP0, HOST_TMP1) ||
				!m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP0, VitaA32::ShiftType::LSR, 24) ||
				!m_code.EmitMovImm32(HOST_TMP5, static_cast<u32>(reinterpret_cast<uptr>(&tlb[0]))) ||
				!m_code.EmitMovImm8(HOST_TMP4, 0))
			{
				return false;
			}

			const size_t loop_start = m_code.Size();
			if (!m_code.EmitLdrImm12(HOST_TMP0, HOST_TMP5, static_cast<u16>(TLB_PAGE_MASK_OFFSET)) ||
				!m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_TMP0, VitaA32::ShiftType::LSR, 13) ||
				!m_code.EmitMovImm32(HOST_TMP1, TLB_MASK_FIELD_MASK) ||
				!m_code.EmitAndReg(HOST_TMP0, HOST_TMP0, HOST_TMP1) ||
				!m_code.EmitMvnReg(HOST_TMP0, HOST_TMP0) ||
				!m_code.EmitLdrImm12(HOST_TMP1, HOST_TMP5, static_cast<u16>(TLB_ENTRY_HI_OFFSET)) ||
				!m_code.EmitMovRegShiftImm(HOST_TMP1, HOST_TMP1, VitaA32::ShiftType::LSR, 13) ||
				!m_code.EmitAndReg(HOST_TMP1, HOST_TMP1, HOST_TMP0) ||
				!m_code.EmitMovRegShiftImm(HOST_TMP1, HOST_TMP1, VitaA32::ShiftType::LSL, 13) ||
				!m_code.EmitAndReg(HOST_TMP0, HOST_TMP3, HOST_TMP0) ||
				!m_code.EmitCmpReg(HOST_TMP1, HOST_TMP0))
			{
				return false;
			}

			const size_t vpn_mismatch = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (vpn_mismatch == static_cast<size_t>(-1))
				return false;

			if (!m_code.EmitLdrImm12(HOST_TMP0, HOST_TMP5, static_cast<u16>(TLB_ENTRY_LO0_OFFSET)) ||
				!m_code.EmitLdrImm12(HOST_TMP1, HOST_TMP5, static_cast<u16>(TLB_ENTRY_LO1_OFFSET)) ||
				!m_code.EmitAndReg(HOST_TMP0, HOST_TMP0, HOST_TMP1) ||
				!m_code.EmitAndImm8(HOST_TMP0, HOST_TMP0, 1, true))
			{
				return false;
			}

			const size_t global_match = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (global_match == static_cast<size_t>(-1))
				return false;

			if (!m_code.EmitLdrImm12(HOST_TMP0, HOST_TMP5, static_cast<u16>(TLB_ENTRY_HI_OFFSET)) ||
				!m_code.EmitAndImm8(HOST_TMP0, HOST_TMP0, 0xff) ||
				!m_code.EmitCmpReg(HOST_TMP0, HOST_TMP2))
			{
				return false;
			}

			const size_t asid_match = m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
			if (asid_match == static_cast<size_t>(-1))
				return false;

			const size_t next_entry = m_code.Size();
			if (!m_code.PatchBranch(vpn_mismatch, next_entry, VitaA32::Condition::NE) ||
				!m_code.EmitAddImm8(HOST_TMP5, HOST_TMP5, static_cast<u8>(TLB_ENTRY_SIZE)) ||
				!m_code.EmitAddImm8(HOST_TMP4, HOST_TMP4, 1) ||
				!m_code.EmitMovImm8(HOST_TMP0, static_cast<u8>(TLB_ENTRY_COUNT)) ||
				!m_code.EmitCmpReg(HOST_TMP4, HOST_TMP0))
			{
				return false;
			}

			const size_t continue_loop = m_code.EmitBranchPlaceholder(VitaA32::Condition::CC);
			if (continue_loop == static_cast<size_t>(-1))
				return false;

			if (!m_code.PatchBranch(continue_loop, loop_start, VitaA32::Condition::CC) ||
				!m_code.EmitMovImm32(HOST_TMP0, 0x80000000u) ||
				!m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(Cp0Offset(0))))
			{
				return false;
			}

			const size_t done = m_code.EmitBranchPlaceholder();
			if (done == static_cast<size_t>(-1))
				return false;

			const size_t match = m_code.Size();
			if (!m_code.PatchBranch(global_match, match, VitaA32::Condition::NE) ||
				!m_code.PatchBranch(asid_match, match, VitaA32::Condition::EQ) ||
				!m_code.EmitStrImm12(HOST_TMP4, HOST_CPU_REGS, static_cast<u16>(Cp0Offset(0))) ||
				!m_code.PatchBranch(done, m_code.Size()))
			{
				return false;
			}

			return m_code.EmitCallAbsolute(event_exit) &&
				   m_code.EmitPop(REG_R4 | REG_R5 | REG_R6 | REG_PC);
		}

		bool BlockCompiler::EmitERETEventExit(u32 op, u32 raw_cycles_through_instruction,
			const void* event_exit)
		{
			// PCSX2 owner: x86/iCOP0.cpp::recERET() branches after
			// COP0.cpp::ERET(). Inline the ERL/ErrorEPC versus EXL/EPC state
			// update and keep the required event-test tail.
			constexpr u32 STATUS_EXL_CLEAR_MASK = 0xfffffffdu;
			constexpr u32 STATUS_ERL_CLEAR_MASK = 0xfffffffbu;
			constexpr u32 STATUS_ERL_MASK = 0x00000004u;

			if (!event_exit || raw_cycles_through_instruction == 0)
				return false;

			const u32 cycles = ScaleBlockCycles(raw_cycles_through_instruction);
			if (!m_code.EmitMovImm32(HOST_TMP0, op) ||
				!m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(CODE_OFFSET)) ||
				!EmitAddScaledCyclesToCpu(cycles) ||
				!m_code.EmitLdrImm12(HOST_TMP2, HOST_CPU_REGS, static_cast<u16>(Cp0Offset(12))) ||
				!m_code.EmitMovImm8(HOST_TMP3, STATUS_ERL_MASK) ||
				!m_code.EmitAndReg(HOST_TMP4, HOST_TMP2, HOST_TMP3, true))
			{
				return false;
			}

			const size_t exl_path = m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
			if (exl_path == static_cast<size_t>(-1))
				return false;

			if (!m_code.EmitLdrImm12(HOST_TMP4, HOST_CPU_REGS, static_cast<u16>(Cp0Offset(30))) ||
				!m_code.EmitStrImm12(HOST_TMP4, HOST_CPU_REGS, static_cast<u16>(PC_OFFSET)) ||
				!m_code.EmitMovImm32(HOST_TMP3, STATUS_ERL_CLEAR_MASK) ||
				!m_code.EmitAndReg(HOST_TMP2, HOST_TMP2, HOST_TMP3) ||
				!m_code.EmitStrImm12(HOST_TMP2, HOST_CPU_REGS, static_cast<u16>(Cp0Offset(12))))
			{
				return false;
			}

			const size_t done = m_code.EmitBranchPlaceholder();
			if (done == static_cast<size_t>(-1))
				return false;

			const size_t exl_target = m_code.Size();
			if (!m_code.PatchBranch(exl_path, exl_target, VitaA32::Condition::EQ) ||
				!m_code.EmitLdrImm12(HOST_TMP4, HOST_CPU_REGS, static_cast<u16>(Cp0Offset(14))) ||
				!m_code.EmitStrImm12(HOST_TMP4, HOST_CPU_REGS, static_cast<u16>(PC_OFFSET)) ||
				!m_code.EmitMovImm32(HOST_TMP3, STATUS_EXL_CLEAR_MASK) ||
				!m_code.EmitAndReg(HOST_TMP2, HOST_TMP2, HOST_TMP3) ||
				!m_code.EmitStrImm12(HOST_TMP2, HOST_CPU_REGS, static_cast<u16>(Cp0Offset(12))))
			{
				return false;
			}

			if (!m_code.PatchBranch(done, m_code.Size()) ||
				!EmitSetNextEventDelta4FromCurrentCycle())
			{
				return false;
			}

			return m_code.EmitCallAbsolute(event_exit) &&
				   m_code.EmitPop(REG_R4 | REG_R5 | REG_R6 | REG_PC);
		}

		bool BlockCompiler::EmitDIDelayedStatusClear()
		{
			// PCSX2 owner: x86/iCOP0.cpp::recDI() inlines COP0.cpp::DI() after
			// recompiling the next instruction, so Status.EIE changes only after
			// that following instruction has observed the old Status value.
			constexpr u32 DI_ALLOWED_MASK = 0x00020006u; // Status._EDI | EXL | ERL
			constexpr u32 DI_KSU_MASK = 0x00000018u;
			constexpr u32 STATUS_EIE_CLEAR_MASK = 0xfffeffffu;

			if (!m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(Cp0Offset(12))) ||
				!m_code.EmitMovImm32(HOST_TMP1, DI_ALLOWED_MASK) ||
				!m_code.EmitAndReg(HOST_TMP2, HOST_TMP0, HOST_TMP1, true))
			{
				return false;
			}

			const size_t clear_eie = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (clear_eie == static_cast<size_t>(-1))
				return false;

			if (!m_code.EmitMovImm32(HOST_TMP1, DI_KSU_MASK) ||
				!m_code.EmitAndReg(HOST_TMP2, HOST_TMP0, HOST_TMP1, true))
			{
				return false;
			}

			const size_t done = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (done == static_cast<size_t>(-1))
				return false;

			const size_t clear_target = m_code.Size();
			if (!m_code.PatchBranch(clear_eie, clear_target, VitaA32::Condition::NE) ||
				!m_code.EmitMovImm32(HOST_TMP1, STATUS_EIE_CLEAR_MASK) ||
				!m_code.EmitAndReg(HOST_TMP0, HOST_TMP0, HOST_TMP1) ||
				!m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(Cp0Offset(12))))
			{
				return false;
			}

			return m_code.PatchBranch(done, m_code.Size(), VitaA32::Condition::NE);
		}

	bool BlockCompiler::EmitCOP1(u32 op, u32 pc, u32 raw_cycles_through_instruction, const void* event_exit)
	{
		if (IsFastCOP1MoveControl(op))
			return EmitCOP1MoveControlFast(op);
		if (IsFastCOP1ArithmeticOp(op))
			return EmitCOP1ArithmeticFast(op);
		if (IsFastCOP1DivSqrtOp(op))
			return EmitCOP1DivSqrtFast(op);
		if (IsFastCOP1AccumulatorOp(op))
			return EmitCOP1AccumulatorFast(op);
		if (IsFastCOP1ScalarWordOp(op))
			return EmitCOP1ScalarWordFast(op);
		if (IsFastCOP1CompareOp(op))
			return EmitCOP1CompareFast(op);
		if (IsFastCOP1ConvertWordOp(op))
			return EmitCOP1ConvertWordFast(op);
		if (IsFastCOP1ConvertSingleOp(op))
			return EmitCOP1ConvertSingleFast(op);

		// PCSX2 owner: x86/iFPU.cpp helper-calls FPU.cpp for arithmetic/control
		// fallbacks; the Vita native FPU arithmetic path will replace this
		// one-op event tail later.
		using namespace R5900::Interpreter::OpcodeImpl::COP1;
		const void* helper = nullptr;

		switch ((op >> 21) & 0x1f)
		{
			case 0x00: // MFC1, owned by FPU.cpp::MFC1().
				helper = reinterpret_cast<const void*>(&MFC1);
				break;
			case 0x02: // CFC1, owned by FPU.cpp::CFC1().
				helper = reinterpret_cast<const void*>(&CFC1);
				break;
			case 0x04: // MTC1, owned by FPU.cpp::MTC1().
				helper = reinterpret_cast<const void*>(&MTC1);
				break;
			case 0x06: // CTC1, owned by FPU.cpp::CTC1().
				helper = reinterpret_cast<const void*>(&CTC1);
				break;
			case 0x10: // COP1_S class, owned by R5900OpcodeTables.cpp::tbl_COP1_S.
				switch (op & 0x3f)
				{
					case 0x00: helper = reinterpret_cast<const void*>(&ADD_S); break;
					case 0x01: helper = reinterpret_cast<const void*>(&SUB_S); break;
					case 0x02: helper = reinterpret_cast<const void*>(&MUL_S); break;
					case 0x03: helper = reinterpret_cast<const void*>(&DIV_S); break;
					case 0x04: helper = reinterpret_cast<const void*>(&SQRT_S); break;
					case 0x05: helper = reinterpret_cast<const void*>(&ABS_S); break;
					case 0x06: helper = reinterpret_cast<const void*>(&MOV_S); break;
					case 0x07: helper = reinterpret_cast<const void*>(&NEG_S); break;
					case 0x16: helper = reinterpret_cast<const void*>(&RSQRT_S); break;
					case 0x18: helper = reinterpret_cast<const void*>(&ADDA_S); break;
					case 0x19: helper = reinterpret_cast<const void*>(&SUBA_S); break;
					case 0x1a: helper = reinterpret_cast<const void*>(&MULA_S); break;
					case 0x1c: helper = reinterpret_cast<const void*>(&MADD_S); break;
					case 0x1d: helper = reinterpret_cast<const void*>(&MSUB_S); break;
					case 0x1e: helper = reinterpret_cast<const void*>(&MADDA_S); break;
					case 0x1f: helper = reinterpret_cast<const void*>(&MSUBA_S); break;
					case 0x24: helper = reinterpret_cast<const void*>(&CVT_W); break;
					case 0x28: helper = reinterpret_cast<const void*>(&MAX_S); break;
					case 0x29: helper = reinterpret_cast<const void*>(&MIN_S); break;
					case 0x30: helper = reinterpret_cast<const void*>(&C_F); break;
					case 0x32: helper = reinterpret_cast<const void*>(&C_EQ); break;
					case 0x34: helper = reinterpret_cast<const void*>(&C_LT); break;
					case 0x36: helper = reinterpret_cast<const void*>(&C_LE); break;
					default:
						return false;
				}
				break;
			case 0x14: // COP1_W class, owned by R5900OpcodeTables.cpp::tbl_COP1_W.
				if ((op & 0x3f) == 0x20)
					helper = reinterpret_cast<const void*>(&CVT_S);
				else
					return false;
				break;
			default:
				return false;
		}

		return EmitSystemHelperEventExit(op, pc + 4, raw_cycles_through_instruction, helper, event_exit);
	}

	bool BlockCompiler::EmitCOP1MoveControlFast(u32 op)
	{
		// PCSX2 owners: FPU.cpp::MFC1()/MTC1()/CFC1()/CTC1() and
		// x86/iFPU.cpp::recMFC1()/recMTC1()/recCFC1()/recCTC1(). The ARMv7
		// validation oracle currently runs PCSX2's interpreter-owned FPU.cpp
		// path, so CFC1 keeps that helper's raw fs=31 and fs=0 behavior here.
		const unsigned rt = RT(op);
		const unsigned fs = RD(op);

		switch ((op >> 21) & 0x1f)
		{
			case 0x00: // MFC1
				if (rt == 0)
					return true;
				return m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(FprOffset(fs))) &&
					   m_code.EmitMovRegShiftImm(HOST_TMP1, HOST_TMP0, VitaA32::ShiftType::ASR, 31) &&
					   EmitStoreGpr64(rt, HOST_TMP0, HOST_TMP1);
			case 0x02: // CFC1
				if (rt == 0)
					return true;
				if (fs == 31)
				{
					if (!m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(FprcOffset(31))))
						return false;
				}
				else if (fs == 0)
				{
					if (!m_code.EmitMovImm32(HOST_TMP0, 0x00002e00u))
						return false;
				}
				else if (!m_code.EmitMovImm8(HOST_TMP0, 0))
				{
					return false;
				}

				return m_code.EmitMovRegShiftImm(HOST_TMP1, HOST_TMP0, VitaA32::ShiftType::ASR, 31) &&
					   EmitStoreGpr64(rt, HOST_TMP0, HOST_TMP1);
			case 0x04: // MTC1
				return EmitLoadGprLow(rt, HOST_TMP0) &&
					   m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(FprOffset(fs)));
			case 0x06: // CTC1
				if (fs != 31)
					return true;
				return EmitLoadGprLow(rt, HOST_TMP0) &&
					   m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(FprcOffset(31)));
			default:
				return false;
		}
	}

	bool BlockCompiler::EmitCOP1ArithmeticFast(u32 op)
	{
		// PCSX2 owners: FPU.cpp::ADD_S()/SUB_S()/MUL_S(), plus
		// FPU.cpp::fpuDouble(), checkOverflow(), and checkUnderflow(). The
		// native path preserves signed zero when normalizing exponent-0 inputs,
		// clamps exponent-0xff inputs to signed max finite, then applies the
		// same O/U cause and SO/SU sticky flag behavior after the VFP operation.
		const unsigned fs = (op >> 11) & 0x1f;
		const unsigned ft = (op >> 16) & 0x1f;
		const unsigned fd = (op >> 6) & 0x1f;
		const u32 function = op & 0x3f;
		constexpr unsigned VFP_FS_S0 = 0;
		constexpr unsigned VFP_FT_S1 = 1;
		constexpr unsigned VFP_FD_S2 = 2;

		const auto normalize_arithmetic_word = [&](unsigned reg) {
			if (!m_code.EmitMovImm32(HOST_TMP2, FPU_FLOAT_EXPONENT_MASK) ||
				!m_code.EmitAndReg(HOST_TMP3, reg, HOST_TMP2) ||
				!m_code.EmitCmpReg(HOST_TMP3, HOST_TMP2))
			{
				return false;
			}

			const size_t not_infinity_or_nan = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (not_infinity_or_nan == static_cast<size_t>(-1))
				return false;

			if (!m_code.EmitMovImm32(HOST_TMP4, FPU_FLOAT_SIGN_MASK) ||
				!m_code.EmitAndReg(reg, reg, HOST_TMP4) ||
				!m_code.EmitMovImm32(HOST_TMP4, FPU_FLOAT_MAX_FINITE) ||
				!m_code.EmitOrrReg(reg, reg, HOST_TMP4))
			{
				return false;
			}

			const size_t done_from_clamp = m_code.EmitBranchPlaceholder();
			if (done_from_clamp == static_cast<size_t>(-1))
				return false;

			const size_t finite_or_zero_target = m_code.Size();
			if (!m_code.PatchBranch(not_infinity_or_nan, finite_or_zero_target, VitaA32::Condition::NE) ||
				!m_code.EmitMovImm8(HOST_TMP4, 0) ||
				!m_code.EmitCmpReg(HOST_TMP3, HOST_TMP4))
			{
				return false;
			}

			const size_t done_from_finite = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (done_from_finite == static_cast<size_t>(-1))
				return false;

			if (!m_code.EmitMovImm32(HOST_TMP4, FPU_FLOAT_SIGN_MASK) ||
				!m_code.EmitAndReg(reg, reg, HOST_TMP4))
			{
				return false;
			}

			const size_t done_target = m_code.Size();
			return m_code.PatchBranch(done_from_clamp, done_target) &&
				   m_code.PatchBranch(done_from_finite, done_target, VitaA32::Condition::NE);
		};

		const auto apply_overflow_underflow_flags = [&]() {
			if (!m_code.EmitLdrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(FprcOffset(31))) ||
				!m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP0, VitaA32::ShiftType::LSL, 1) ||
				!m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP2, VitaA32::ShiftType::LSR, 1) ||
				!m_code.EmitMovImm32(HOST_TMP3, FPU_FLOAT_EXPONENT_MASK) ||
				!m_code.EmitCmpReg(HOST_TMP2, HOST_TMP3))
			{
				return false;
			}

			const size_t no_overflow = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (no_overflow == static_cast<size_t>(-1))
				return false;

			if (!m_code.EmitMovImm32(HOST_TMP2, FPU_FLOAT_SIGN_MASK) ||
				!m_code.EmitAndReg(HOST_TMP0, HOST_TMP0, HOST_TMP2) ||
				!m_code.EmitMovImm32(HOST_TMP2, FPU_FLOAT_MAX_FINITE) ||
				!m_code.EmitOrrReg(HOST_TMP0, HOST_TMP0, HOST_TMP2) ||
				!m_code.EmitMovImm32(HOST_TMP2, FPU_FCR31_ARITHMETIC_OVERFLOW_FLAGS) ||
				!m_code.EmitOrrReg(HOST_TMP1, HOST_TMP1, HOST_TMP2))
			{
				return false;
			}

			const size_t store_result = m_code.EmitBranchPlaceholder();
			if (store_result == static_cast<size_t>(-1))
				return false;

			const size_t no_overflow_target = m_code.Size();
			if (!m_code.PatchBranch(no_overflow, no_overflow_target, VitaA32::Condition::NE) ||
				!m_code.EmitMovImm32(HOST_TMP2, ~FPU_FCR31_OVERFLOW_FLAG) ||
				!m_code.EmitAndReg(HOST_TMP1, HOST_TMP1, HOST_TMP2) ||
				!m_code.EmitMovImm32(HOST_TMP2, FPU_FLOAT_EXPONENT_MASK) ||
				!m_code.EmitAndReg(HOST_TMP3, HOST_TMP0, HOST_TMP2) ||
				!m_code.EmitMovImm8(HOST_TMP4, 0) ||
				!m_code.EmitCmpReg(HOST_TMP3, HOST_TMP4))
			{
				return false;
			}

			const size_t no_underflow_from_exponent = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (no_underflow_from_exponent == static_cast<size_t>(-1))
				return false;

			if (!m_code.EmitMovImm32(HOST_TMP2, FPU_FLOAT_FRACTION_MASK) ||
				!m_code.EmitAndReg(HOST_TMP3, HOST_TMP0, HOST_TMP2) ||
				!m_code.EmitMovImm8(HOST_TMP4, 0) ||
				!m_code.EmitCmpReg(HOST_TMP3, HOST_TMP4))
			{
				return false;
			}

			const size_t no_underflow_from_fraction = m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
			if (no_underflow_from_fraction == static_cast<size_t>(-1))
				return false;

			if (!m_code.EmitMovImm32(HOST_TMP2, FPU_FLOAT_SIGN_MASK) ||
				!m_code.EmitAndReg(HOST_TMP0, HOST_TMP0, HOST_TMP2) ||
				!m_code.EmitMovImm32(HOST_TMP2, FPU_FCR31_ARITHMETIC_UNDERFLOW_FLAGS) ||
				!m_code.EmitOrrReg(HOST_TMP1, HOST_TMP1, HOST_TMP2))
			{
				return false;
			}

			const size_t underflow_store = m_code.EmitBranchPlaceholder();
			if (underflow_store == static_cast<size_t>(-1))
				return false;

			const size_t clear_underflow_target = m_code.Size();
			if (!m_code.PatchBranch(no_underflow_from_exponent, clear_underflow_target, VitaA32::Condition::NE) ||
				!m_code.PatchBranch(no_underflow_from_fraction, clear_underflow_target, VitaA32::Condition::EQ) ||
				!m_code.EmitMovImm32(HOST_TMP2, ~FPU_FCR31_UNDERFLOW_FLAG) ||
				!m_code.EmitAndReg(HOST_TMP1, HOST_TMP1, HOST_TMP2))
			{
				return false;
			}

			const size_t store_target = m_code.Size();
			return m_code.PatchBranch(store_result, store_target) &&
				   m_code.PatchBranch(underflow_store, store_target) &&
				   m_code.EmitStrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(FprcOffset(31))) &&
				   m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(FprOffset(fd)));
		};

		if (!m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(FprOffset(fs))) ||
			!m_code.EmitLdrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(FprOffset(ft))) ||
			!normalize_arithmetic_word(HOST_TMP0) ||
			!normalize_arithmetic_word(HOST_TMP1) ||
			!m_code.EmitVmovCoreToS(VFP_FS_S0, HOST_TMP0) ||
			!m_code.EmitVmovCoreToS(VFP_FT_S1, HOST_TMP1))
		{
			return false;
		}

		switch (function)
		{
			case 0x00: // ADD_S
				if (!m_code.EmitVaddF32(VFP_FD_S2, VFP_FS_S0, VFP_FT_S1))
					return false;
				break;
			case 0x01: // SUB_S
				if (!m_code.EmitVsubF32(VFP_FD_S2, VFP_FS_S0, VFP_FT_S1))
					return false;
				break;
			case 0x02: // MUL_S
				if (!m_code.EmitVmulF32(VFP_FD_S2, VFP_FS_S0, VFP_FT_S1))
					return false;
				break;
			default:
				return false;
		}

		return m_code.EmitVmovSToCore(HOST_TMP0, VFP_FD_S2) &&
			   apply_overflow_underflow_flags();
	}

	bool BlockCompiler::EmitCOP1DivSqrtFast(u32 op)
	{
		// PCSX2 owners: FPU.cpp::DIV_S()/SQRT_S()/RSQRT_S(), plus
		// FPU.cpp::checkDivideByZero(), fpuDouble(), checkOverflow(), and
		// checkUnderflow(). DIV/RSQRT clamp overflow/underflow results without
		// touching O/U flags because FPU.cpp passes cFlagsToSet=0. SQRT clears
		// only I/D cause flags and does not run overflow/underflow checks.
		const unsigned fs = (op >> 11) & 0x1f;
		const unsigned ft = (op >> 16) & 0x1f;
		const unsigned fd = (op >> 6) & 0x1f;
		const u32 function = op & 0x3f;
		constexpr unsigned VFP_FS_S0 = 0;
		constexpr unsigned VFP_FT_S1 = 1;
		constexpr unsigned VFP_FD_S2 = 2;

		const auto normalize_arithmetic_word = [&](unsigned reg) {
			if (!m_code.EmitMovImm32(HOST_TMP2, FPU_FLOAT_EXPONENT_MASK) ||
				!m_code.EmitAndReg(HOST_TMP3, reg, HOST_TMP2) ||
				!m_code.EmitCmpReg(HOST_TMP3, HOST_TMP2))
			{
				return false;
			}

			const size_t not_infinity_or_nan = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (not_infinity_or_nan == static_cast<size_t>(-1))
				return false;

			if (!m_code.EmitMovImm32(HOST_TMP4, FPU_FLOAT_SIGN_MASK) ||
				!m_code.EmitAndReg(reg, reg, HOST_TMP4) ||
				!m_code.EmitMovImm32(HOST_TMP4, FPU_FLOAT_MAX_FINITE) ||
				!m_code.EmitOrrReg(reg, reg, HOST_TMP4))
			{
				return false;
			}

			const size_t done_from_clamp = m_code.EmitBranchPlaceholder();
			if (done_from_clamp == static_cast<size_t>(-1))
				return false;

			const size_t finite_or_zero_target = m_code.Size();
			if (!m_code.PatchBranch(not_infinity_or_nan, finite_or_zero_target, VitaA32::Condition::NE) ||
				!m_code.EmitMovImm8(HOST_TMP4, 0) ||
				!m_code.EmitCmpReg(HOST_TMP3, HOST_TMP4))
			{
				return false;
			}

			const size_t done_from_finite = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (done_from_finite == static_cast<size_t>(-1))
				return false;

			if (!m_code.EmitMovImm32(HOST_TMP4, FPU_FLOAT_SIGN_MASK) ||
				!m_code.EmitAndReg(reg, reg, HOST_TMP4))
			{
				return false;
			}

			const size_t done_target = m_code.Size();
			return m_code.PatchBranch(done_from_clamp, done_target) &&
				   m_code.PatchBranch(done_from_finite, done_target, VitaA32::Condition::NE);
		};

		const auto store_result = [&](bool store_fcr31) {
			if (store_fcr31 &&
				!m_code.EmitStrImm12(HOST_TMP5, HOST_CPU_REGS, static_cast<u16>(FprcOffset(31))))
			{
				return false;
			}
			return m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(FprOffset(fd)));
		};

		const auto clamp_result_no_flags = [&](bool store_fcr31) {
			if (!m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP0, VitaA32::ShiftType::LSL, 1) ||
				!m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP2, VitaA32::ShiftType::LSR, 1) ||
				!m_code.EmitMovImm32(HOST_TMP3, FPU_FLOAT_EXPONENT_MASK) ||
				!m_code.EmitCmpReg(HOST_TMP2, HOST_TMP3))
			{
				return false;
			}

			const size_t no_overflow = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (no_overflow == static_cast<size_t>(-1))
				return false;

			if (!m_code.EmitMovImm32(HOST_TMP2, FPU_FLOAT_SIGN_MASK) ||
				!m_code.EmitAndReg(HOST_TMP0, HOST_TMP0, HOST_TMP2) ||
				!m_code.EmitMovImm32(HOST_TMP2, FPU_FLOAT_MAX_FINITE) ||
				!m_code.EmitOrrReg(HOST_TMP0, HOST_TMP0, HOST_TMP2))
			{
				return false;
			}

			const size_t store_after_overflow = m_code.EmitBranchPlaceholder();
			if (store_after_overflow == static_cast<size_t>(-1))
				return false;

			const size_t no_overflow_target = m_code.Size();
			if (!m_code.PatchBranch(no_overflow, no_overflow_target, VitaA32::Condition::NE) ||
				!m_code.EmitMovImm32(HOST_TMP2, FPU_FLOAT_EXPONENT_MASK) ||
				!m_code.EmitAndReg(HOST_TMP3, HOST_TMP0, HOST_TMP2) ||
				!m_code.EmitMovImm8(HOST_TMP4, 0) ||
				!m_code.EmitCmpReg(HOST_TMP3, HOST_TMP4))
			{
				return false;
			}

			const size_t no_underflow_from_exponent = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (no_underflow_from_exponent == static_cast<size_t>(-1))
				return false;

			if (!m_code.EmitMovImm32(HOST_TMP2, FPU_FLOAT_FRACTION_MASK) ||
				!m_code.EmitAndReg(HOST_TMP3, HOST_TMP0, HOST_TMP2) ||
				!m_code.EmitMovImm8(HOST_TMP4, 0) ||
				!m_code.EmitCmpReg(HOST_TMP3, HOST_TMP4))
			{
				return false;
			}

			const size_t no_underflow_from_fraction = m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
			if (no_underflow_from_fraction == static_cast<size_t>(-1))
				return false;

			if (!m_code.EmitMovImm32(HOST_TMP2, FPU_FLOAT_SIGN_MASK) ||
				!m_code.EmitAndReg(HOST_TMP0, HOST_TMP0, HOST_TMP2))
			{
				return false;
			}

			const size_t store_target = m_code.Size();
			return m_code.PatchBranch(store_after_overflow, store_target) &&
				   m_code.PatchBranch(no_underflow_from_exponent, store_target, VitaA32::Condition::NE) &&
				   m_code.PatchBranch(no_underflow_from_fraction, store_target, VitaA32::Condition::EQ) &&
				   store_result(store_fcr31);
		};

		const auto clear_invalid_divide_causes = [&]() {
			return m_code.EmitLdrImm12(HOST_TMP5, HOST_CPU_REGS, static_cast<u16>(FprcOffset(31))) &&
				   m_code.EmitMovImm32(HOST_TMP2, ~FPU_FCR31_INVALID_DIVIDE_CAUSE_FLAGS) &&
				   m_code.EmitAndReg(HOST_TMP5, HOST_TMP5, HOST_TMP2);
		};

		const auto emit_divide_by_zero_result = [&]() {
			if (!m_code.EmitLdrImm12(HOST_TMP5, HOST_CPU_REGS, static_cast<u16>(FprcOffset(31))) ||
				!m_code.EmitMovImm32(HOST_TMP2, FPU_FLOAT_EXPONENT_MASK) ||
				!m_code.EmitAndReg(HOST_TMP3, HOST_TMP0, HOST_TMP2) ||
				!m_code.EmitMovImm8(HOST_TMP4, 0) ||
				!m_code.EmitCmpReg(HOST_TMP3, HOST_TMP4))
			{
				return false;
			}

			const size_t dividend_nonzero = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (dividend_nonzero == static_cast<size_t>(-1))
				return false;

			if (!m_code.EmitMovImm32(HOST_TMP4, FPU_FCR31_INVALID_FLAGS))
				return false;

			const size_t flags_ready = m_code.EmitBranchPlaceholder();
			if (flags_ready == static_cast<size_t>(-1))
				return false;

			const size_t dividend_nonzero_target = m_code.Size();
			if (!m_code.PatchBranch(dividend_nonzero, dividend_nonzero_target, VitaA32::Condition::NE) ||
				!m_code.EmitMovImm32(HOST_TMP4, FPU_FCR31_DIVIDE_BY_ZERO_FLAGS))
			{
				return false;
			}

			const size_t flags_ready_target = m_code.Size();
			return m_code.PatchBranch(flags_ready, flags_ready_target) &&
				   m_code.EmitOrrReg(HOST_TMP5, HOST_TMP5, HOST_TMP4) &&
				   m_code.EmitEorReg(HOST_TMP0, HOST_TMP0, HOST_TMP1) &&
				   m_code.EmitMovImm32(HOST_TMP2, FPU_FLOAT_SIGN_MASK) &&
				   m_code.EmitAndReg(HOST_TMP0, HOST_TMP0, HOST_TMP2) &&
				   m_code.EmitMovImm32(HOST_TMP2, FPU_FLOAT_MAX_FINITE) &&
				   m_code.EmitOrrReg(HOST_TMP0, HOST_TMP0, HOST_TMP2) &&
				   store_result(true);
		};

		if (function == 0x03) // DIV_S
		{
			if (!m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(FprOffset(fs))) ||
				!m_code.EmitLdrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(FprOffset(ft))) ||
				!m_code.EmitMovImm32(HOST_TMP2, FPU_FLOAT_EXPONENT_MASK) ||
				!m_code.EmitAndReg(HOST_TMP3, HOST_TMP1, HOST_TMP2) ||
				!m_code.EmitMovImm8(HOST_TMP4, 0) ||
				!m_code.EmitCmpReg(HOST_TMP3, HOST_TMP4))
			{
				return false;
			}

			const size_t divisor_nonzero = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (divisor_nonzero == static_cast<size_t>(-1))
				return false;

			if (!emit_divide_by_zero_result())
				return false;

			const size_t done_from_zero = m_code.EmitBranchPlaceholder();
			if (done_from_zero == static_cast<size_t>(-1))
				return false;

			const size_t divisor_nonzero_target = m_code.Size();
			if (!m_code.PatchBranch(divisor_nonzero, divisor_nonzero_target, VitaA32::Condition::NE) ||
				!normalize_arithmetic_word(HOST_TMP0) ||
				!normalize_arithmetic_word(HOST_TMP1) ||
				!m_code.EmitVmovCoreToS(VFP_FS_S0, HOST_TMP0) ||
				!m_code.EmitVmovCoreToS(VFP_FT_S1, HOST_TMP1) ||
				!m_code.EmitVdivF32(VFP_FD_S2, VFP_FS_S0, VFP_FT_S1) ||
				!m_code.EmitVmovSToCore(HOST_TMP0, VFP_FD_S2) ||
				!clamp_result_no_flags(false))
			{
				return false;
			}

			return m_code.PatchBranch(done_from_zero, m_code.Size());
		}

		if (function == 0x04) // SQRT_S
		{
			if (!m_code.EmitLdrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(FprOffset(ft))) ||
				!clear_invalid_divide_causes() ||
				!m_code.EmitMovImm32(HOST_TMP2, FPU_FLOAT_EXPONENT_MASK) ||
				!m_code.EmitAndReg(HOST_TMP3, HOST_TMP1, HOST_TMP2) ||
				!m_code.EmitMovImm8(HOST_TMP4, 0) ||
				!m_code.EmitCmpReg(HOST_TMP3, HOST_TMP4))
			{
				return false;
			}

			const size_t operand_nonzero = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (operand_nonzero == static_cast<size_t>(-1))
				return false;

			if (!m_code.EmitMovImm32(HOST_TMP2, FPU_FLOAT_SIGN_MASK) ||
				!m_code.EmitAndReg(HOST_TMP0, HOST_TMP1, HOST_TMP2))
			{
				return false;
			}

			const size_t done_from_zero = m_code.EmitBranchPlaceholder();
			if (done_from_zero == static_cast<size_t>(-1))
				return false;

			const size_t operand_nonzero_target = m_code.Size();
			if (!m_code.PatchBranch(operand_nonzero, operand_nonzero_target, VitaA32::Condition::NE) ||
				!m_code.EmitMovImm32(HOST_TMP2, FPU_FLOAT_SIGN_MASK) ||
				!m_code.EmitAndReg(HOST_TMP3, HOST_TMP1, HOST_TMP2) ||
				!m_code.EmitMovImm8(HOST_TMP4, 0) ||
				!m_code.EmitCmpReg(HOST_TMP3, HOST_TMP4))
			{
				return false;
			}

			const size_t operand_positive = m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
			if (operand_positive == static_cast<size_t>(-1))
				return false;

			if (!m_code.EmitMovImm32(HOST_TMP2, FPU_FCR31_INVALID_FLAGS) ||
				!m_code.EmitOrrReg(HOST_TMP5, HOST_TMP5, HOST_TMP2) ||
				!normalize_arithmetic_word(HOST_TMP1) ||
				!m_code.EmitMovImm32(HOST_TMP2, ~FPU_FLOAT_SIGN_MASK) ||
				!m_code.EmitAndReg(HOST_TMP1, HOST_TMP1, HOST_TMP2))
			{
				return false;
			}

			const size_t operand_ready = m_code.EmitBranchPlaceholder();
			if (operand_ready == static_cast<size_t>(-1))
				return false;

			const size_t operand_positive_target = m_code.Size();
			if (!m_code.PatchBranch(operand_positive, operand_positive_target, VitaA32::Condition::EQ) ||
				!normalize_arithmetic_word(HOST_TMP1))
			{
				return false;
			}

			const size_t operand_ready_target = m_code.Size();
			return m_code.PatchBranch(operand_ready, operand_ready_target) &&
				   m_code.EmitVmovCoreToS(VFP_FT_S1, HOST_TMP1) &&
				   m_code.EmitVsqrtF32(VFP_FD_S2, VFP_FT_S1) &&
				   m_code.EmitVmovSToCore(HOST_TMP0, VFP_FD_S2) &&
				   m_code.PatchBranch(done_from_zero, m_code.Size()) &&
				   store_result(true);
		}

		if (function == 0x16) // RSQRT_S
		{
			if (!m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(FprOffset(fs))) ||
				!m_code.EmitLdrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(FprOffset(ft))) ||
				!clear_invalid_divide_causes() ||
				!m_code.EmitMovImm32(HOST_TMP2, FPU_FLOAT_EXPONENT_MASK) ||
				!m_code.EmitAndReg(HOST_TMP3, HOST_TMP1, HOST_TMP2) ||
				!m_code.EmitMovImm8(HOST_TMP4, 0) ||
				!m_code.EmitCmpReg(HOST_TMP3, HOST_TMP4))
			{
				return false;
			}

			const size_t operand_nonzero = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (operand_nonzero == static_cast<size_t>(-1))
				return false;

			if (!m_code.EmitMovImm32(HOST_TMP2, FPU_FCR31_DIVIDE_BY_ZERO_FLAGS) ||
				!m_code.EmitOrrReg(HOST_TMP5, HOST_TMP5, HOST_TMP2) ||
				!m_code.EmitMovImm32(HOST_TMP2, FPU_FLOAT_SIGN_MASK) ||
				!m_code.EmitAndReg(HOST_TMP0, HOST_TMP1, HOST_TMP2) ||
				!m_code.EmitMovImm32(HOST_TMP2, FPU_FLOAT_MAX_FINITE) ||
				!m_code.EmitOrrReg(HOST_TMP0, HOST_TMP0, HOST_TMP2) ||
				!store_result(true))
			{
				return false;
			}

			const size_t done_from_zero = m_code.EmitBranchPlaceholder();
			if (done_from_zero == static_cast<size_t>(-1))
				return false;

			const size_t operand_nonzero_target = m_code.Size();
			if (!m_code.PatchBranch(operand_nonzero, operand_nonzero_target, VitaA32::Condition::NE) ||
				!m_code.EmitMovImm32(HOST_TMP2, FPU_FLOAT_SIGN_MASK) ||
				!m_code.EmitAndReg(HOST_TMP3, HOST_TMP1, HOST_TMP2) ||
				!m_code.EmitMovImm8(HOST_TMP4, 0) ||
				!m_code.EmitCmpReg(HOST_TMP3, HOST_TMP4))
			{
				return false;
			}

			const size_t operand_positive = m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
			if (operand_positive == static_cast<size_t>(-1))
				return false;

			if (!m_code.EmitMovImm32(HOST_TMP2, FPU_FCR31_INVALID_FLAGS) ||
				!m_code.EmitOrrReg(HOST_TMP5, HOST_TMP5, HOST_TMP2) ||
				!normalize_arithmetic_word(HOST_TMP1) ||
				!m_code.EmitMovImm32(HOST_TMP2, ~FPU_FLOAT_SIGN_MASK) ||
				!m_code.EmitAndReg(HOST_TMP1, HOST_TMP1, HOST_TMP2))
			{
				return false;
			}

			const size_t operand_ready = m_code.EmitBranchPlaceholder();
			if (operand_ready == static_cast<size_t>(-1))
				return false;

			const size_t operand_positive_target = m_code.Size();
			if (!m_code.PatchBranch(operand_positive, operand_positive_target, VitaA32::Condition::EQ) ||
				!normalize_arithmetic_word(HOST_TMP1))
			{
				return false;
			}

			const size_t operand_ready_target = m_code.Size();
			if (!m_code.PatchBranch(operand_ready, operand_ready_target) ||
				!normalize_arithmetic_word(HOST_TMP0) ||
				!m_code.EmitVmovCoreToS(VFP_FS_S0, HOST_TMP0) ||
				!m_code.EmitVmovCoreToS(VFP_FT_S1, HOST_TMP1) ||
				!m_code.EmitVsqrtF32(VFP_FT_S1, VFP_FT_S1) ||
				!m_code.EmitVdivF32(VFP_FD_S2, VFP_FS_S0, VFP_FT_S1) ||
				!m_code.EmitVmovSToCore(HOST_TMP0, VFP_FD_S2) ||
				!clamp_result_no_flags(true))
			{
				return false;
			}

			return m_code.PatchBranch(done_from_zero, m_code.Size());
		}

		return false;
	}

	bool BlockCompiler::EmitCOP1AccumulatorFast(u32 op)
	{
		// PCSX2 owners: FPU.cpp::ADDA_S()/SUBA_S()/MULA_S()/MADD_S()/
		// MSUB_S()/MADDA_S()/MSUBA_S(), plus FPU.cpp::fpuDouble(),
		// checkOverflow(), and checkUnderflow(). MADD/MSUB use the
		// FPU.cpp temporary product then fpuDouble() both ACC and the product.
		// MADDA/MSUBA keep FPU.cpp's raw ACC compound-assignment behavior.
		const unsigned fs = (op >> 11) & 0x1f;
		const unsigned ft = (op >> 16) & 0x1f;
		const unsigned fd = (op >> 6) & 0x1f;
		const u32 function = op & 0x3f;
		constexpr unsigned VFP_FS_S0 = 0;
		constexpr unsigned VFP_FT_S1 = 1;
		constexpr unsigned VFP_FD_S2 = 2;

		const auto destination_offset = [&]() -> size_t {
			switch (function)
			{
				case 0x18: // ADDA_S
				case 0x19: // SUBA_S
				case 0x1a: // MULA_S
				case 0x1e: // MADDA_S
				case 0x1f: // MSUBA_S
					return FPU_ACC_OFFSET;
				default:
					return FprOffset(fd);
			}
		};

		const auto normalize_arithmetic_word = [&](unsigned reg) {
			if (!m_code.EmitMovImm32(HOST_TMP2, FPU_FLOAT_EXPONENT_MASK) ||
				!m_code.EmitAndReg(HOST_TMP3, reg, HOST_TMP2) ||
				!m_code.EmitCmpReg(HOST_TMP3, HOST_TMP2))
			{
				return false;
			}

			const size_t not_infinity_or_nan = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (not_infinity_or_nan == static_cast<size_t>(-1))
				return false;

			if (!m_code.EmitMovImm32(HOST_TMP4, FPU_FLOAT_SIGN_MASK) ||
				!m_code.EmitAndReg(reg, reg, HOST_TMP4) ||
				!m_code.EmitMovImm32(HOST_TMP4, FPU_FLOAT_MAX_FINITE) ||
				!m_code.EmitOrrReg(reg, reg, HOST_TMP4))
			{
				return false;
			}

			const size_t done_from_clamp = m_code.EmitBranchPlaceholder();
			if (done_from_clamp == static_cast<size_t>(-1))
				return false;

			const size_t finite_or_zero_target = m_code.Size();
			if (!m_code.PatchBranch(not_infinity_or_nan, finite_or_zero_target, VitaA32::Condition::NE) ||
				!m_code.EmitMovImm8(HOST_TMP4, 0) ||
				!m_code.EmitCmpReg(HOST_TMP3, HOST_TMP4))
			{
				return false;
			}

			const size_t done_from_finite = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (done_from_finite == static_cast<size_t>(-1))
				return false;

			if (!m_code.EmitMovImm32(HOST_TMP4, FPU_FLOAT_SIGN_MASK) ||
				!m_code.EmitAndReg(reg, reg, HOST_TMP4))
			{
				return false;
			}

			const size_t done_target = m_code.Size();
			return m_code.PatchBranch(done_from_clamp, done_target) &&
				   m_code.PatchBranch(done_from_finite, done_target, VitaA32::Condition::NE);
		};

		const auto apply_overflow_underflow_flags = [&](size_t dest_offset) {
			if (!m_code.EmitLdrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(FprcOffset(31))) ||
				!m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP0, VitaA32::ShiftType::LSL, 1) ||
				!m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP2, VitaA32::ShiftType::LSR, 1) ||
				!m_code.EmitMovImm32(HOST_TMP3, FPU_FLOAT_EXPONENT_MASK) ||
				!m_code.EmitCmpReg(HOST_TMP2, HOST_TMP3))
			{
				return false;
			}

			const size_t no_overflow = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (no_overflow == static_cast<size_t>(-1))
				return false;

			if (!m_code.EmitMovImm32(HOST_TMP2, FPU_FLOAT_SIGN_MASK) ||
				!m_code.EmitAndReg(HOST_TMP0, HOST_TMP0, HOST_TMP2) ||
				!m_code.EmitMovImm32(HOST_TMP2, FPU_FLOAT_MAX_FINITE) ||
				!m_code.EmitOrrReg(HOST_TMP0, HOST_TMP0, HOST_TMP2) ||
				!m_code.EmitMovImm32(HOST_TMP2, FPU_FCR31_ARITHMETIC_OVERFLOW_FLAGS) ||
				!m_code.EmitOrrReg(HOST_TMP1, HOST_TMP1, HOST_TMP2))
			{
				return false;
			}

			const size_t store_result = m_code.EmitBranchPlaceholder();
			if (store_result == static_cast<size_t>(-1))
				return false;

			const size_t no_overflow_target = m_code.Size();
			if (!m_code.PatchBranch(no_overflow, no_overflow_target, VitaA32::Condition::NE) ||
				!m_code.EmitMovImm32(HOST_TMP2, ~FPU_FCR31_OVERFLOW_FLAG) ||
				!m_code.EmitAndReg(HOST_TMP1, HOST_TMP1, HOST_TMP2) ||
				!m_code.EmitMovImm32(HOST_TMP2, FPU_FLOAT_EXPONENT_MASK) ||
				!m_code.EmitAndReg(HOST_TMP3, HOST_TMP0, HOST_TMP2) ||
				!m_code.EmitMovImm8(HOST_TMP4, 0) ||
				!m_code.EmitCmpReg(HOST_TMP3, HOST_TMP4))
			{
				return false;
			}

			const size_t no_underflow_from_exponent = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (no_underflow_from_exponent == static_cast<size_t>(-1))
				return false;

			if (!m_code.EmitMovImm32(HOST_TMP2, FPU_FLOAT_FRACTION_MASK) ||
				!m_code.EmitAndReg(HOST_TMP3, HOST_TMP0, HOST_TMP2) ||
				!m_code.EmitMovImm8(HOST_TMP4, 0) ||
				!m_code.EmitCmpReg(HOST_TMP3, HOST_TMP4))
			{
				return false;
			}

			const size_t no_underflow_from_fraction = m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
			if (no_underflow_from_fraction == static_cast<size_t>(-1))
				return false;

			if (!m_code.EmitMovImm32(HOST_TMP2, FPU_FLOAT_SIGN_MASK) ||
				!m_code.EmitAndReg(HOST_TMP0, HOST_TMP0, HOST_TMP2) ||
				!m_code.EmitMovImm32(HOST_TMP2, FPU_FCR31_ARITHMETIC_UNDERFLOW_FLAGS) ||
				!m_code.EmitOrrReg(HOST_TMP1, HOST_TMP1, HOST_TMP2))
			{
				return false;
			}

			const size_t underflow_store = m_code.EmitBranchPlaceholder();
			if (underflow_store == static_cast<size_t>(-1))
				return false;

			const size_t clear_underflow_target = m_code.Size();
			if (!m_code.PatchBranch(no_underflow_from_exponent, clear_underflow_target, VitaA32::Condition::NE) ||
				!m_code.PatchBranch(no_underflow_from_fraction, clear_underflow_target, VitaA32::Condition::EQ) ||
				!m_code.EmitMovImm32(HOST_TMP2, ~FPU_FCR31_UNDERFLOW_FLAG) ||
				!m_code.EmitAndReg(HOST_TMP1, HOST_TMP1, HOST_TMP2))
			{
				return false;
			}

			const size_t store_target = m_code.Size();
			return m_code.PatchBranch(store_result, store_target) &&
				   m_code.PatchBranch(underflow_store, store_target) &&
				   m_code.EmitStrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(FprcOffset(31))) &&
				   m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(dest_offset));
		};

		const auto load_normalized_operands = [&]() {
			return m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(FprOffset(fs))) &&
				   m_code.EmitLdrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(FprOffset(ft))) &&
				   normalize_arithmetic_word(HOST_TMP0) &&
				   normalize_arithmetic_word(HOST_TMP1) &&
				   m_code.EmitVmovCoreToS(VFP_FS_S0, HOST_TMP0) &&
				   m_code.EmitVmovCoreToS(VFP_FT_S1, HOST_TMP1);
		};

		if (!load_normalized_operands())
			return false;

		switch (function)
		{
			case 0x18: // ADDA_S
				if (!m_code.EmitVaddF32(VFP_FD_S2, VFP_FS_S0, VFP_FT_S1))
					return false;
				break;
			case 0x19: // SUBA_S
				if (!m_code.EmitVsubF32(VFP_FD_S2, VFP_FS_S0, VFP_FT_S1))
					return false;
				break;
			case 0x1a: // MULA_S
			case 0x1c: // MADD_S
			case 0x1d: // MSUB_S
			case 0x1e: // MADDA_S
			case 0x1f: // MSUBA_S
				if (!m_code.EmitVmulF32(VFP_FD_S2, VFP_FS_S0, VFP_FT_S1))
					return false;
				break;
			default:
				return false;
		}

		if (function == 0x1c || function == 0x1d) // MADD_S / MSUB_S
		{
			if (!m_code.EmitVmovSToCore(HOST_TMP0, VFP_FD_S2) ||
				!normalize_arithmetic_word(HOST_TMP0) ||
				!m_code.EmitLdrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(FPU_ACC_OFFSET)) ||
				!normalize_arithmetic_word(HOST_TMP1) ||
				!m_code.EmitVmovCoreToS(VFP_FS_S0, HOST_TMP1) ||
				!m_code.EmitVmovCoreToS(VFP_FT_S1, HOST_TMP0))
			{
				return false;
			}

			if (function == 0x1c)
			{
				if (!m_code.EmitVaddF32(VFP_FD_S2, VFP_FS_S0, VFP_FT_S1))
					return false;
			}
			else if (!m_code.EmitVsubF32(VFP_FD_S2, VFP_FS_S0, VFP_FT_S1))
			{
				return false;
			}
		}
		else if (function == 0x1e || function == 0x1f) // MADDA_S / MSUBA_S
		{
			if (!m_code.EmitLdrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(FPU_ACC_OFFSET)) ||
				!m_code.EmitVmovCoreToS(VFP_FS_S0, HOST_TMP1))
			{
				return false;
			}

			if (function == 0x1e)
			{
				if (!m_code.EmitVaddF32(VFP_FD_S2, VFP_FS_S0, VFP_FD_S2))
					return false;
			}
			else if (!m_code.EmitVsubF32(VFP_FD_S2, VFP_FS_S0, VFP_FD_S2))
			{
				return false;
			}
		}

		return m_code.EmitVmovSToCore(HOST_TMP0, VFP_FD_S2) &&
			   apply_overflow_underflow_flags(destination_offset());
	}

	bool BlockCompiler::EmitCOP1ScalarWordFast(u32 op)
	{
		// PCSX2 owners: FPU.cpp::MOV_S()/ABS_S()/NEG_S()/MAX_S()/MIN_S().
		// These operate on fpuRegs.fpr[] as 32-bit words; ABS_S, NEG_S, MAX_S,
		// and MIN_S also clear only FPUflagO | FPUflagU in fpuRegs.fprc[31].
		const unsigned fs = (op >> 11) & 0x1f;
		const unsigned ft = (op >> 16) & 0x1f;
		const unsigned fd = (op >> 6) & 0x1f;
		const u32 function = op & 0x3f;

		const auto clear_overflow_underflow = [&]() {
			return m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(FprcOffset(31))) &&
				   m_code.EmitMovImm32(HOST_TMP1, FPU_FCR31_CLEAR_OVERFLOW_UNDERFLOW_MASK) &&
				   m_code.EmitAndReg(HOST_TMP0, HOST_TMP0, HOST_TMP1) &&
				   m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(FprcOffset(31)));
		};

		if (!m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(FprOffset(fs))))
			return false;

		switch (function)
		{
			case 0x05: // ABS_S
				if (!m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_TMP0, VitaA32::ShiftType::LSL, 1) ||
					!m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_TMP0, VitaA32::ShiftType::LSR, 1))
				{
					return false;
				}
				break;
			case 0x06: // MOV_S
				break;
			case 0x07: // NEG_S
				if (!m_code.EmitMovImm32(HOST_TMP1, 0x80000000u) ||
					!m_code.EmitEorReg(HOST_TMP0, HOST_TMP0, HOST_TMP1))
				{
					return false;
				}
				break;
			case 0x28: // MAX_S
			case 0x29: // MIN_S
			{
				// PCSX2 FPU.cpp::fp_max()/fp_min() use signed word ordering and
				// reverse the comparison only when both operands are negative.
				const bool max_op = function == 0x28;
				const VitaA32::Condition normal_select_ft =
					max_op ? VitaA32::Condition::LT : VitaA32::Condition::GT;
				const VitaA32::Condition negative_select_ft =
					max_op ? VitaA32::Condition::GT : VitaA32::Condition::LT;

				if (!m_code.EmitLdrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(FprOffset(ft))) ||
					!m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP0, VitaA32::ShiftType::LSL, 0) ||
					!m_code.EmitCmpReg(HOST_TMP0, HOST_TMP1) ||
					!m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP1, VitaA32::ShiftType::LSL, 0,
						false, normal_select_ft) ||
					!m_code.EmitMovImm32(HOST_TMP3, 0x80000000u) ||
					!m_code.EmitAndReg(HOST_TMP4, HOST_TMP0, HOST_TMP1) ||
					!m_code.EmitAndReg(HOST_TMP4, HOST_TMP4, HOST_TMP3) ||
					!m_code.EmitCmpReg(HOST_TMP4, HOST_TMP3))
				{
					return false;
				}

				const size_t store_word = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
				if (store_word == static_cast<size_t>(-1))
					return false;

				if (!m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP0, VitaA32::ShiftType::LSL, 0) ||
					!m_code.EmitCmpReg(HOST_TMP0, HOST_TMP1) ||
					!m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP1, VitaA32::ShiftType::LSL, 0,
						false, negative_select_ft) ||
					!m_code.PatchBranch(store_word, m_code.Size(), VitaA32::Condition::NE) ||
					!m_code.EmitStrImm12(HOST_TMP2, HOST_CPU_REGS, static_cast<u16>(FprOffset(fd))))
				{
					return false;
				}

				return clear_overflow_underflow();
			}
			default:
				return false;
		}

		if (!m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(FprOffset(fd))))
			return false;

		if (function == 0x06)
			return true;

		return clear_overflow_underflow();
	}

	bool BlockCompiler::EmitCOP1CompareFast(u32 op)
	{
		// PCSX2 owners: FPU.cpp::C_F()/C_EQ()/C_LT()/C_LE() and
		// FPU.cpp::fpuDouble(). fpuDouble() clamps all exponent-0 values to
		// signed zero and all exponent-0xff values to signed max finite before
		// comparing, so this path does the same as integer transforms.
		const unsigned fs = (op >> 11) & 0x1f;
		const unsigned ft = (op >> 16) & 0x1f;
		const u32 function = op & 0x3f;

		const auto clear_condition_flag = [&]() {
			return m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(FprcOffset(31))) &&
				   m_code.EmitMovImm32(HOST_TMP1, FPU_FCR31_CONDITION_FLAG) &&
				   m_code.EmitMvnReg(HOST_TMP2, HOST_TMP1) &&
				   m_code.EmitAndReg(HOST_TMP0, HOST_TMP0, HOST_TMP2) &&
				   m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(FprcOffset(31)));
		};

		const auto normalize_compare_word = [&](unsigned reg) {
			if (!m_code.EmitMovImm32(HOST_TMP2, FPU_FLOAT_EXPONENT_MASK) ||
				!m_code.EmitAndReg(HOST_TMP3, reg, HOST_TMP2) ||
				!m_code.EmitCmpReg(HOST_TMP3, HOST_TMP2))
			{
				return false;
			}

			const size_t not_infinity_or_nan = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (not_infinity_or_nan == static_cast<size_t>(-1))
				return false;

			if (!m_code.EmitMovImm32(HOST_TMP4, FPU_FLOAT_SIGN_MASK) ||
				!m_code.EmitAndReg(reg, reg, HOST_TMP4) ||
				!m_code.EmitMovImm32(HOST_TMP4, FPU_FLOAT_MAX_FINITE) ||
				!m_code.EmitOrrReg(reg, reg, HOST_TMP4))
			{
				return false;
			}

			const size_t done_from_clamp = m_code.EmitBranchPlaceholder();
			if (done_from_clamp == static_cast<size_t>(-1))
				return false;

			const size_t finite_or_zero_target = m_code.Size();
			if (!m_code.PatchBranch(not_infinity_or_nan, finite_or_zero_target, VitaA32::Condition::NE) ||
				!m_code.EmitMovImm8(HOST_TMP4, 0) ||
				!m_code.EmitCmpReg(HOST_TMP3, HOST_TMP4))
			{
				return false;
			}

			const size_t done_from_finite = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (done_from_finite == static_cast<size_t>(-1))
				return false;

			if (!m_code.EmitMovImm8(reg, 0))
				return false;

			const size_t done_target = m_code.Size();
			return m_code.PatchBranch(done_from_clamp, done_target) &&
				   m_code.PatchBranch(done_from_finite, done_target, VitaA32::Condition::NE);
		};

		const auto make_sortable_compare_key = [&](unsigned reg) {
			return m_code.EmitMovRegShiftImm(HOST_TMP2, reg, VitaA32::ShiftType::ASR, 31) &&
				   m_code.EmitMovImm32(HOST_TMP3, FPU_FLOAT_SIGN_MASK) &&
				   m_code.EmitOrrReg(HOST_TMP2, HOST_TMP2, HOST_TMP3) &&
				   m_code.EmitEorReg(reg, reg, HOST_TMP2);
		};

		const auto apply_condition_flag = [&](VitaA32::Condition condition) {
			if (!m_code.EmitMovImm8(HOST_TMP4, 0) ||
				!m_code.EmitMovImm8(HOST_TMP4, 1, condition) ||
				!m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(FprcOffset(31))) ||
				!m_code.EmitMovImm32(HOST_TMP1, FPU_FCR31_CONDITION_FLAG) ||
				!m_code.EmitMvnReg(HOST_TMP2, HOST_TMP1) ||
				!m_code.EmitAndReg(HOST_TMP0, HOST_TMP0, HOST_TMP2) ||
				!m_code.EmitMovImm8(HOST_TMP2, 0) ||
				!m_code.EmitCmpReg(HOST_TMP4, HOST_TMP2))
			{
				return false;
			}

			const size_t clear_only = m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
			if (clear_only == static_cast<size_t>(-1))
				return false;

			if (!m_code.EmitOrrReg(HOST_TMP0, HOST_TMP0, HOST_TMP1))
				return false;

			const size_t store_target = m_code.Size();
			return m_code.PatchBranch(clear_only, store_target, VitaA32::Condition::EQ) &&
				   m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(FprcOffset(31)));
		};

		if (function == 0x30) // C_F
			return clear_condition_flag();

		if (!m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(FprOffset(fs))) ||
			!m_code.EmitLdrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(FprOffset(ft))) ||
			!normalize_compare_word(HOST_TMP0) ||
			!normalize_compare_word(HOST_TMP1))
		{
			return false;
		}

		switch (function)
		{
			case 0x32: // C_EQ
				return m_code.EmitCmpReg(HOST_TMP0, HOST_TMP1) &&
					   apply_condition_flag(VitaA32::Condition::EQ);
			case 0x34: // C_LT
				return make_sortable_compare_key(HOST_TMP0) &&
					   make_sortable_compare_key(HOST_TMP1) &&
					   m_code.EmitCmpReg(HOST_TMP0, HOST_TMP1) &&
					   apply_condition_flag(VitaA32::Condition::CC);
			case 0x36: // C_LE
				return make_sortable_compare_key(HOST_TMP0) &&
					   make_sortable_compare_key(HOST_TMP1) &&
					   m_code.EmitCmpReg(HOST_TMP0, HOST_TMP1) &&
					   apply_condition_flag(VitaA32::Condition::LS);
			default:
				return false;
		}
	}

	bool BlockCompiler::EmitCOP1ConvertWordFast(u32 op)
	{
		// PCSX2 owner: FPU.cpp::CVT_W(). PCSX2 saturates when the exponent mask
		// exceeds 0x4e800000, otherwise the float is in signed-int range and the
		// C++ cast truncates toward zero. This integer path mirrors that gate.
		const unsigned fs = (op >> 11) & 0x1f;
		const unsigned fd = (op >> 6) & 0x1f;

		if (!m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(FprOffset(fs))) ||
			!m_code.EmitMovImm32(HOST_TMP1, FPU_FLOAT_EXPONENT_MASK) ||
			!m_code.EmitAndReg(HOST_TMP2, HOST_TMP0, HOST_TMP1) ||
			!m_code.EmitMovImm32(HOST_TMP3, FPU_CVT_W_MAX_EXPONENT_MASK) ||
			!m_code.EmitCmpReg(HOST_TMP2, HOST_TMP3))
		{
			return false;
		}

		const size_t convert_path = m_code.EmitBranchPlaceholder(VitaA32::Condition::LS);
		if (convert_path == static_cast<size_t>(-1))
			return false;

		if (!m_code.EmitMovImm32(HOST_TMP3, FPU_FLOAT_SIGN_MASK) ||
			!m_code.EmitAndReg(HOST_TMP2, HOST_TMP0, HOST_TMP3) ||
			!m_code.EmitCmpReg(HOST_TMP2, HOST_TMP3) ||
			!m_code.EmitMovImm32(HOST_TMP0, 0x7fffffffu) ||
			!m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_TMP3, VitaA32::ShiftType::LSL, 0,
				false, VitaA32::Condition::EQ))
		{
			return false;
		}

		const size_t store_result = m_code.EmitBranchPlaceholder();
		if (store_result == static_cast<size_t>(-1))
			return false;

		const size_t convert_target = m_code.Size();
		if (!m_code.PatchBranch(convert_path, convert_target, VitaA32::Condition::LS) ||
			!m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP2, VitaA32::ShiftType::LSR, 23) ||
			!m_code.EmitMovImm8(HOST_TMP3, FPU_FLOAT_EXPONENT_BIAS) ||
			!m_code.EmitCmpReg(HOST_TMP2, HOST_TMP3))
		{
			return false;
		}

		const size_t nonzero_path = m_code.EmitBranchPlaceholder(VitaA32::Condition::CS);
		if (nonzero_path == static_cast<size_t>(-1))
			return false;

		if (!m_code.EmitMovImm8(HOST_TMP0, 0))
			return false;

		const size_t zero_done = m_code.EmitBranchPlaceholder();
		if (zero_done == static_cast<size_t>(-1))
			return false;

		const size_t nonzero_target = m_code.Size();
		if (!m_code.PatchBranch(nonzero_path, nonzero_target, VitaA32::Condition::CS) ||
			!m_code.EmitMovImm32(HOST_TMP3, FPU_FLOAT_FRACTION_MASK) ||
			!m_code.EmitAndReg(HOST_TMP3, HOST_TMP0, HOST_TMP3) ||
			!m_code.EmitMovImm32(HOST_TMP4, FPU_FLOAT_IMPLICIT_MANTISSA) ||
			!m_code.EmitOrrReg(HOST_TMP3, HOST_TMP3, HOST_TMP4) ||
			!m_code.EmitMovImm8(HOST_TMP4, FPU_FLOAT_EXPONENT_BIAS) ||
			!m_code.EmitSubReg(HOST_TMP2, HOST_TMP2, HOST_TMP4) ||
			!m_code.EmitMovImm8(HOST_TMP4, FPU_FLOAT_MANTISSA_BITS) ||
			!m_code.EmitCmpReg(HOST_TMP2, HOST_TMP4))
		{
			return false;
		}

		const size_t left_shift_path = m_code.EmitBranchPlaceholder(VitaA32::Condition::CS);
		if (left_shift_path == static_cast<size_t>(-1))
			return false;

		if (!m_code.EmitSubReg(HOST_TMP4, HOST_TMP4, HOST_TMP2) ||
			!m_code.EmitMovRegShiftReg(HOST_TMP3, HOST_TMP3, VitaA32::ShiftType::LSR, HOST_TMP4))
		{
			return false;
		}

		const size_t apply_sign = m_code.EmitBranchPlaceholder();
		if (apply_sign == static_cast<size_t>(-1))
			return false;

		const size_t left_shift_target = m_code.Size();
		if (!m_code.PatchBranch(left_shift_path, left_shift_target, VitaA32::Condition::CS) ||
			!m_code.EmitMovImm8(HOST_TMP4, FPU_FLOAT_MANTISSA_BITS) ||
			!m_code.EmitSubReg(HOST_TMP4, HOST_TMP2, HOST_TMP4) ||
			!m_code.EmitMovRegShiftReg(HOST_TMP3, HOST_TMP3, VitaA32::ShiftType::LSL, HOST_TMP4))
		{
			return false;
		}

		const size_t apply_sign_target = m_code.Size();
		if (!m_code.PatchBranch(apply_sign, apply_sign_target) ||
			!m_code.EmitMovImm32(HOST_TMP4, FPU_FLOAT_SIGN_MASK) ||
			!m_code.EmitAndReg(HOST_TMP4, HOST_TMP0, HOST_TMP4, true) ||
			!m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_TMP3, VitaA32::ShiftType::LSL, 0))
		{
			return false;
		}

		const size_t positive_done = m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
		if (positive_done == static_cast<size_t>(-1))
			return false;

		if (!m_code.EmitMovImm8(HOST_TMP1, 0) ||
			!m_code.EmitSubReg(HOST_TMP0, HOST_TMP1, HOST_TMP3))
		{
			return false;
		}

		const size_t final_store = m_code.Size();
		return m_code.PatchBranch(store_result, final_store) &&
			   m_code.PatchBranch(zero_done, final_store) &&
			   m_code.PatchBranch(positive_done, final_store, VitaA32::Condition::EQ) &&
			   m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(FprOffset(fd)));
	}

	bool BlockCompiler::EmitCOP1ConvertSingleFast(u32 op)
	{
		// PCSX2 owner: FPU.cpp::CVT_S(). PCSX2 casts the signed source FPR word
		// to float and does not update FCR31, so the Cortex-A9 path uses
		// call-clobbered VFP s0 for the same int-to-single conversion.
		const unsigned fs = (op >> 11) & 0x1f;
		const unsigned fd = (op >> 6) & 0x1f;
		constexpr unsigned VFP_TMP_S0 = 0;

		return m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(FprOffset(fs))) &&
			   m_code.EmitVmovCoreToS(VFP_TMP_S0, HOST_TMP0) &&
			   m_code.EmitVcvtF32S32(VFP_TMP_S0, VFP_TMP_S0) &&
			   m_code.EmitVmovSToCore(HOST_TMP0, VFP_TMP_S0) &&
			   m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(FprOffset(fd)));
	}

	bool BlockCompiler::EmitCACHE(u32 op, u32 pc, u32 raw_cycles_through_instruction, const void* event_exit)
	{
		if (IsNoOpCACHE(op))
			return true;

		if (!IsHelperCACHE(op))
			return false;

		using namespace R5900::Interpreter::OpcodeImpl;
		return EmitSystemHelperEventExit(op, pc + 4, raw_cycles_through_instruction,
			reinterpret_cast<const void*>(&CACHE), event_exit);
	}

	bool BlockCompiler::EmitBREAK(u32 op, u32 pc, u32 raw_cycles_through_instruction,
		const void* event_exit, bool branch_delay_slot)
	{
		if (!event_exit || raw_cycles_through_instruction == 0)
			return false;

		using namespace R5900::Interpreter::OpcodeImpl;
		const u32 cycles = ScaleBlockCycles(raw_cycles_through_instruction);
		if (!m_code.EmitMovImm32(HOST_TMP0, op) ||
			!m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(CODE_OFFSET)) ||
			!EmitStorePc(pc + 4) ||
			!m_code.EmitMovImm8(HOST_TMP0, branch_delay_slot ? 1 : 0) ||
			!m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(BRANCH_OFFSET)) ||
			!EmitAddScaledCyclesToCpu(cycles) ||
			!m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&BREAK)))
		{
			return false;
		}

		return m_code.EmitCallAbsolute(event_exit) &&
			   m_code.EmitPop(REG_R4 | REG_R5 | REG_R6 | REG_PC);
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

	bool BlockCompiler::EmitMULT(u32 op)
	{
		return EmitMultiply(op, true, false);
	}

	bool BlockCompiler::EmitMULTU(u32 op)
	{
		return EmitMultiply(op, false, false);
	}

	bool BlockCompiler::EmitMADD(u32 op)
	{
		return EmitMultiplyAdd(op, true, false);
	}

	bool BlockCompiler::EmitMADDU(u32 op)
	{
		return EmitMultiplyAdd(op, false, false);
	}

	bool BlockCompiler::EmitMADD1(u32 op)
	{
		return EmitMultiplyAdd(op, true, true);
	}

	bool BlockCompiler::EmitMADDU1(u32 op)
	{
		return EmitMultiplyAdd(op, false, true);
	}

	bool BlockCompiler::EmitMFHI1(u32 op)
	{
		// PCSX2 owner: MMI.cpp::MFHI1().
		return EmitMoveFromHiLo(op, HiloLaneOffset(HI_OFFSET, true));
	}

	bool BlockCompiler::EmitMFLO1(u32 op)
	{
		// PCSX2 owner: MMI.cpp::MFLO1().
		return EmitMoveFromHiLo(op, HiloLaneOffset(LO_OFFSET, true));
	}

	bool BlockCompiler::EmitMTHI1(u32 op)
	{
		// PCSX2 owner: MMI.cpp::MTHI1().
		return EmitMoveToHiLo(op, HiloLaneOffset(HI_OFFSET, true));
	}

	bool BlockCompiler::EmitMTLO1(u32 op)
	{
		// PCSX2 owner: MMI.cpp::MTLO1().
		return EmitMoveToHiLo(op, HiloLaneOffset(LO_OFFSET, true));
	}

	bool BlockCompiler::EmitMULT1(u32 op)
	{
		return EmitMultiply(op, true, true);
	}

	bool BlockCompiler::EmitMULTU1(u32 op)
	{
		return EmitMultiply(op, false, true);
	}

	bool BlockCompiler::EmitMultiply(u32 op, bool signed_multiply, bool upper_pipeline)
	{
		// PCSX2 owners: R5900OpcodeImpl.cpp::MULT()/MULTU(), MMI.cpp::MULT1()/MULTU1(),
		// and x86/ix86-32/iR5900MultDiv.cpp::recMULT*(). All forms sign-extend
		// the 32-bit LO/HI halves into their 64-bit lanes, and the EE
		// three-operand form writes the selected LO lane into rd as well.
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		const unsigned rd = RD(op);
		const size_t lo_offset = HiloLaneOffset(LO_OFFSET, upper_pipeline);
		const size_t hi_offset = HiloLaneOffset(HI_OFFSET, upper_pipeline);

		if (!EmitLoadGprLow(rs, HOST_TMP0) ||
			!EmitLoadGprLow(rt, HOST_TMP1))
		{
			return false;
		}

		if (signed_multiply)
		{
			if (!m_code.EmitSmull(HOST_TMP2, HOST_TMP3, HOST_TMP0, HOST_TMP1))
				return false;
		}
		else
		{
			if (!m_code.EmitUmull(HOST_TMP2, HOST_TMP3, HOST_TMP0, HOST_TMP1))
				return false;
		}

		if (!m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_TMP2, VitaA32::ShiftType::ASR, 31) ||
			!m_code.EmitStrImm12(HOST_TMP2, HOST_CPU_REGS, static_cast<u16>(lo_offset)) ||
			!m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(lo_offset + sizeof(u32))))
		{
			return false;
		}

		if (rd != 0 && !EmitStoreGpr64(rd, HOST_TMP2, HOST_TMP0))
			return false;

		return m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_TMP3, VitaA32::ShiftType::ASR, 31) &&
			   m_code.EmitStrImm12(HOST_TMP3, HOST_CPU_REGS, static_cast<u16>(hi_offset)) &&
			   m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(hi_offset + sizeof(u32)));
	}

	bool BlockCompiler::EmitMultiplyAdd(u32 op, bool signed_multiply, bool upper_pipeline)
	{
		// PCSX2 owners: MMI.cpp::MADD()/MADDU()/MADD1()/MADDU1() and
		// x86/ix86-32/iR5900MultDiv.cpp::recMADD*(). The accumulator is the
		// raw 64-bit value formed from LO.UL[lane*2] and HI.UL[lane*2]; the
		// writeback stores each 32-bit result half as a sign-extended 64-bit lane.
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		const unsigned rd = RD(op);
		const size_t lo_offset = HiloLaneOffset(LO_OFFSET, upper_pipeline);
		const size_t hi_offset = HiloLaneOffset(HI_OFFSET, upper_pipeline);

		if (!EmitLoadGprLow(rs, HOST_TMP0) ||
			!EmitLoadGprLow(rt, HOST_TMP1))
		{
			return false;
		}

		if (signed_multiply)
		{
			if (!m_code.EmitSmull(HOST_TMP2, HOST_TMP3, HOST_TMP0, HOST_TMP1))
				return false;
		}
		else
		{
			if (!m_code.EmitUmull(HOST_TMP2, HOST_TMP3, HOST_TMP0, HOST_TMP1))
				return false;
		}

		if (!m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(lo_offset)) ||
			!m_code.EmitLdrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(hi_offset)) ||
			!m_code.EmitAddReg(HOST_TMP2, HOST_TMP2, HOST_TMP0, true) ||
			!m_code.EmitAdcReg(HOST_TMP3, HOST_TMP3, HOST_TMP1))
		{
			return false;
		}

		if (!m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_TMP2, VitaA32::ShiftType::ASR, 31) ||
			!m_code.EmitStrImm12(HOST_TMP2, HOST_CPU_REGS, static_cast<u16>(lo_offset)) ||
			!m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(lo_offset + sizeof(u32))))
		{
			return false;
		}

		if (rd != 0 && !EmitStoreGpr64(rd, HOST_TMP2, HOST_TMP0))
			return false;

		return m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_TMP3, VitaA32::ShiftType::ASR, 31) &&
			   m_code.EmitStrImm12(HOST_TMP3, HOST_CPU_REGS, static_cast<u16>(hi_offset)) &&
			   m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(hi_offset + sizeof(u32)));
	}

	bool BlockCompiler::EmitDIV(u32 op)
	{
		// PCSX2 owner: R5900OpcodeImpl.cpp::DIV(). Cortex-A9 has no integer
		// divide, so no-divide and positive power-of-two cases are direct and
		// arbitrary division keeps the exact PCSX2 helper semantics.
		return EmitScalarDivide(op, true, false);
	}

	bool BlockCompiler::EmitDIVU(u32 op)
	{
		// PCSX2 owner: R5900OpcodeImpl.cpp::DIVU().
		return EmitScalarDivide(op, false, false);
	}

	bool BlockCompiler::EmitDIV1(u32 op)
	{
		// PCSX2 owners: MMI.cpp::DIV1() and x86/ix86-32/iR5900MultDiv.cpp::recDIV1().
		return EmitScalarDivide(op, true, true);
	}

	bool BlockCompiler::EmitDIVU1(u32 op)
	{
		// PCSX2 owners: MMI.cpp::DIVU1() and x86/ix86-32/iR5900MultDiv.cpp::recDIVU1().
		return EmitScalarDivide(op, false, true);
	}

	bool BlockCompiler::EmitScalarDivide(u32 op, bool signed_divide, bool upper_pipeline)
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

		const auto store_signed_word_as_doubleword = [this](unsigned value_reg, size_t offset) {
			return m_code.EmitMovRegShiftImm(HOST_TMP4, value_reg, VitaA32::ShiftType::ASR, 31) &&
				   m_code.EmitStrImm12(value_reg, HOST_CPU_REGS, static_cast<u16>(offset)) &&
				   m_code.EmitStrImm12(HOST_TMP4, HOST_CPU_REGS, static_cast<u16>(offset + sizeof(u32)));
		};

		const size_t lo_offset = HiloLaneOffset(LO_OFFSET, upper_pipeline);
		const size_t hi_offset = HiloLaneOffset(HI_OFFSET, upper_pipeline);
		const void* helper = nullptr;
		if (signed_divide)
		{
			helper = upper_pipeline ?
						 reinterpret_cast<const void*>(&VitaEeDivSigned1) :
						 reinterpret_cast<const void*>(&VitaEeDivSigned);
		}
		else
		{
			helper = upper_pipeline ?
						 reinterpret_cast<const void*>(&VitaEeDivUnsigned1) :
						 reinterpret_cast<const void*>(&VitaEeDivUnsigned);
		}

		if (!EmitLoadGprLow(RS(op), HOST_TMP0) ||
			!EmitLoadGprLow(RT(op), HOST_TMP1))
		{
			return false;
		}

		BranchPatch divzero_branch{};
		BranchPatch zero_branch{};
		BranchPatch divone_branch{};
		BranchPatch negone_or_below_branch{};
		BranchPatch equal_branch{};
		BranchPatch power_of_two_branch{};
		BranchPatch fallback_branch{};
		BranchPatch done_branches[6]{};
		unsigned done_branch_count = 0;

		if (!m_code.EmitMovImm8(HOST_TMP3, 0) ||
			!m_code.EmitCmpReg(HOST_TMP1, HOST_TMP3) ||
			!emit_branch(divzero_branch, VitaA32::Condition::EQ) ||
			!m_code.EmitCmpReg(HOST_TMP0, HOST_TMP3) ||
			!emit_branch(zero_branch, VitaA32::Condition::EQ) ||
			!m_code.EmitMovImm8(HOST_TMP3, 1) ||
			!m_code.EmitCmpReg(HOST_TMP1, HOST_TMP3) ||
			!emit_branch(divone_branch, VitaA32::Condition::EQ))
		{
			return false;
		}

		if (signed_divide)
		{
			if (!m_code.EmitMovImm32(HOST_TMP3, 0xffffffffu) ||
				!m_code.EmitCmpReg(HOST_TMP1, HOST_TMP3) ||
				!emit_branch(negone_or_below_branch, VitaA32::Condition::EQ) ||
				!m_code.EmitCmpReg(HOST_TMP0, HOST_TMP1) ||
				!emit_branch(equal_branch, VitaA32::Condition::EQ) ||
				!m_code.EmitMovRegShiftImm(HOST_TMP3, HOST_TMP1, VitaA32::ShiftType::ASR, 31) ||
				!m_code.EmitEorReg(HOST_TMP2, HOST_TMP1, HOST_TMP3) ||
				!m_code.EmitSubReg(HOST_TMP2, HOST_TMP2, HOST_TMP3) ||
				!m_code.EmitSubImm8(HOST_TMP3, HOST_TMP2, 1) ||
				!m_code.EmitAndReg(HOST_TMP2, HOST_TMP2, HOST_TMP3, true) ||
				!emit_branch(power_of_two_branch, VitaA32::Condition::EQ))
			{
				return false;
			}
		}
		else if (!m_code.EmitCmpReg(HOST_TMP0, HOST_TMP1) ||
				 !emit_branch(negone_or_below_branch, VitaA32::Condition::CC) ||
				 !emit_branch(equal_branch, VitaA32::Condition::EQ) ||
				 !m_code.EmitSubImm8(HOST_TMP2, HOST_TMP1, 1) ||
				 !m_code.EmitAndReg(HOST_TMP2, HOST_TMP1, HOST_TMP2, true) ||
				 !emit_branch(power_of_two_branch, VitaA32::Condition::EQ))
		{
			return false;
		}

		if (!emit_branch(fallback_branch, VitaA32::Condition::AL))
			return false;

		if (!patch_branch(divzero_branch, m_code.Size()) ||
			!m_code.EmitMovImm32(HOST_TMP2, 0xffffffffu))
		{
			return false;
		}

		if (signed_divide &&
			(!m_code.EmitMovImm8(HOST_TMP3, 0) ||
				!m_code.EmitCmpReg(HOST_TMP0, HOST_TMP3) ||
				!m_code.EmitMovImm8(HOST_TMP2, 1, VitaA32::Condition::LT)))
		{
			return false;
		}

		if (!store_signed_word_as_doubleword(HOST_TMP2, lo_offset) ||
			!store_signed_word_as_doubleword(HOST_TMP0, hi_offset) ||
			!emit_branch(done_branches[done_branch_count++], VitaA32::Condition::AL))
		{
			return false;
		}

		if (!patch_branch(zero_branch, m_code.Size()) ||
			!m_code.EmitMovImm8(HOST_TMP2, 0) ||
			!store_signed_word_as_doubleword(HOST_TMP2, lo_offset) ||
			!store_signed_word_as_doubleword(HOST_TMP2, hi_offset) ||
			!emit_branch(done_branches[done_branch_count++], VitaA32::Condition::AL))
		{
			return false;
		}

		if (!patch_branch(divone_branch, m_code.Size()) ||
			!m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP0, VitaA32::ShiftType::LSL, 0) ||
			!store_signed_word_as_doubleword(HOST_TMP2, lo_offset) ||
			!m_code.EmitMovImm8(HOST_TMP2, 0) ||
			!store_signed_word_as_doubleword(HOST_TMP2, hi_offset) ||
			!emit_branch(done_branches[done_branch_count++], VitaA32::Condition::AL))
		{
			return false;
		}

		if (signed_divide)
		{
			if (!patch_branch(negone_or_below_branch, m_code.Size()) ||
				!m_code.EmitMovImm8(HOST_TMP3, 0) ||
				!m_code.EmitSubReg(HOST_TMP2, HOST_TMP3, HOST_TMP0) ||
				!store_signed_word_as_doubleword(HOST_TMP2, lo_offset) ||
				!m_code.EmitMovImm8(HOST_TMP2, 0) ||
				!store_signed_word_as_doubleword(HOST_TMP2, hi_offset) ||
				!emit_branch(done_branches[done_branch_count++], VitaA32::Condition::AL) ||
				!patch_branch(equal_branch, m_code.Size()) ||
				!m_code.EmitMovImm8(HOST_TMP2, 1) ||
				!store_signed_word_as_doubleword(HOST_TMP2, lo_offset) ||
				!m_code.EmitMovImm8(HOST_TMP2, 0) ||
				!store_signed_word_as_doubleword(HOST_TMP2, hi_offset) ||
				!emit_branch(done_branches[done_branch_count++], VitaA32::Condition::AL) ||
				!patch_branch(power_of_two_branch, m_code.Size()) ||
				!m_code.EmitMovRegShiftImm(HOST_TMP3, HOST_TMP0, VitaA32::ShiftType::ASR, 31) ||
				!m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP1, VitaA32::ShiftType::ASR, 31) ||
				!m_code.EmitEorReg(HOST_TMP4, HOST_TMP1, HOST_TMP2) ||
				!m_code.EmitSubReg(HOST_TMP4, HOST_TMP4, HOST_TMP2) ||
				!m_code.EmitSubImm8(HOST_TMP2, HOST_TMP4, 1) ||
				!m_code.EmitAndReg(HOST_TMP3, HOST_TMP3, HOST_TMP2) ||
				!m_code.EmitAddReg(HOST_TMP3, HOST_TMP0, HOST_TMP3) ||
				!m_code.EmitClz(HOST_TMP4, HOST_TMP4) ||
				!m_code.EmitMovImm8(HOST_TMP2, 31) ||
				!m_code.EmitSubReg(HOST_TMP2, HOST_TMP2, HOST_TMP4) ||
				!m_code.EmitMovRegShiftReg(HOST_TMP4, HOST_TMP3, VitaA32::ShiftType::ASR, HOST_TMP2) ||
				!m_code.EmitMovRegShiftReg(HOST_TMP3, HOST_TMP4, VitaA32::ShiftType::LSL, HOST_TMP2) ||
				!m_code.EmitSubReg(HOST_TMP3, HOST_TMP0, HOST_TMP3) ||
				!m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP1, VitaA32::ShiftType::ASR, 31) ||
				!m_code.EmitEorReg(HOST_TMP4, HOST_TMP4, HOST_TMP2) ||
				!m_code.EmitSubReg(HOST_TMP2, HOST_TMP4, HOST_TMP2) ||
				!store_signed_word_as_doubleword(HOST_TMP2, lo_offset) ||
				!store_signed_word_as_doubleword(HOST_TMP3, hi_offset) ||
				!emit_branch(done_branches[done_branch_count++], VitaA32::Condition::AL))
			{
				return false;
			}
		}
		else
		{
			if (!patch_branch(negone_or_below_branch, m_code.Size()) ||
				!m_code.EmitMovImm8(HOST_TMP2, 0) ||
				!store_signed_word_as_doubleword(HOST_TMP2, lo_offset) ||
				!store_signed_word_as_doubleword(HOST_TMP0, hi_offset) ||
				!emit_branch(done_branches[done_branch_count++], VitaA32::Condition::AL) ||
				!patch_branch(equal_branch, m_code.Size()) ||
				!m_code.EmitMovImm8(HOST_TMP2, 1) ||
				!store_signed_word_as_doubleword(HOST_TMP2, lo_offset) ||
				!m_code.EmitMovImm8(HOST_TMP2, 0) ||
				!store_signed_word_as_doubleword(HOST_TMP2, hi_offset) ||
				!emit_branch(done_branches[done_branch_count++], VitaA32::Condition::AL) ||
				!patch_branch(power_of_two_branch, m_code.Size()) ||
				!m_code.EmitSubImm8(HOST_TMP2, HOST_TMP1, 1) ||
				!m_code.EmitAndReg(HOST_TMP3, HOST_TMP0, HOST_TMP2) ||
				!m_code.EmitClz(HOST_TMP2, HOST_TMP1) ||
				!m_code.EmitMovImm8(HOST_TMP4, 31) ||
				!m_code.EmitSubReg(HOST_TMP4, HOST_TMP4, HOST_TMP2) ||
				!m_code.EmitMovRegShiftReg(HOST_TMP2, HOST_TMP0, VitaA32::ShiftType::LSR, HOST_TMP4) ||
				!store_signed_word_as_doubleword(HOST_TMP2, lo_offset) ||
				!store_signed_word_as_doubleword(HOST_TMP3, hi_offset) ||
				!emit_branch(done_branches[done_branch_count++], VitaA32::Condition::AL))
			{
				return false;
			}
		}

		if (!patch_branch(fallback_branch, m_code.Size()) ||
			!m_code.EmitCallAbsolute(helper))
		{
			return false;
		}

		return patch_branches(done_branches, done_branch_count, m_code.Size());
	}

	bool BlockCompiler::EmitPLZCW(u32 op)
	{
		// PCSX2 owner: MMI.cpp::PLZCW(), which writes only RD.UL[0]/[1] with
		// the leading sign-bit count of RS.SL[0]/[1], excluding the sign bit.
		const unsigned rd = RD(op);
		if (rd == 0)
			return true;

		const size_t rd_offset = GprOffset(rd);
		const auto emit_lane = [this, rd_offset](unsigned word, unsigned value_reg) {
			return m_code.EmitMovRegShiftImm(HOST_TMP2, value_reg, VitaA32::ShiftType::ASR, 31) &&
				   m_code.EmitEorReg(value_reg, value_reg, HOST_TMP2) &&
				   m_code.EmitClz(value_reg, value_reg) &&
				   m_code.EmitSubImm8(value_reg, value_reg, 1) &&
				   m_code.EmitStrImm12(value_reg, HOST_CPU_REGS,
					   static_cast<u16>(rd_offset + word * sizeof(u32)));
		};

		return EmitLoadGpr64(RS(op), HOST_TMP0, HOST_TMP1) &&
			   emit_lane(0, HOST_TMP0) &&
			   emit_lane(1, HOST_TMP1);
	}

	bool BlockCompiler::EmitPMFHL(u32 op)
	{
		// PCSX2 owner: MMI.cpp::PMFHL(). Mode 2 saturates the signed 64-bit
		// LO/HI pair into a sign-extended signed 32-bit doubleword.
		const unsigned rd = RD(op);
		if (rd == 0)
			return true;

		const size_t rd_offset = GprOffset(rd);
		const auto emit_word_move = [this, rd_offset](size_t source_offset, unsigned dst_word) {
			return m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(source_offset)) &&
				   m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS,
					   static_cast<u16>(rd_offset + dst_word * sizeof(u32)));
		};
		const auto emit_halfword_pack = [this, rd_offset](size_t first_offset, size_t second_offset,
										  unsigned dst_word, bool saturate) {
			if (!m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(first_offset)) ||
				!m_code.EmitLdrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(second_offset)))
			{
				return false;
			}

			if (saturate &&
				(!m_code.EmitSsat(HOST_TMP0, 16, HOST_TMP0) ||
				 !m_code.EmitSsat(HOST_TMP1, 16, HOST_TMP1)))
			{
				return false;
			}

			return m_code.EmitPkhbt(HOST_TMP0, HOST_TMP0, HOST_TMP1, 16) &&
				   m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS,
					   static_cast<u16>(rd_offset + dst_word * sizeof(u32)));
		};
		const auto emit_saturating_word = [this, rd_offset](size_t lo_offset, size_t hi_offset,
											unsigned dst_doubleword) {
			if (!m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(lo_offset)) ||
				!m_code.EmitLdrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(hi_offset)) ||
				!m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP0, VitaA32::ShiftType::ASR, 31) ||
				!m_code.EmitCmpReg(HOST_TMP1, HOST_TMP2))
			{
				return false;
			}

			const size_t no_overflow = m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
			if (no_overflow == static_cast<size_t>(-1))
				return false;

			if (!m_code.EmitMovImm8(HOST_TMP3, 0) ||
				!m_code.EmitCmpReg(HOST_TMP1, HOST_TMP3))
			{
				return false;
			}

			const size_t negative_overflow = m_code.EmitBranchPlaceholder(VitaA32::Condition::LT);
			if (negative_overflow == static_cast<size_t>(-1))
				return false;

			if (!m_code.EmitMovImm32(HOST_TMP0, 0x7fffffffu) ||
				!m_code.EmitMovImm8(HOST_TMP1, 0))
			{
				return false;
			}

			const size_t positive_done = m_code.EmitBranchPlaceholder();
			if (positive_done == static_cast<size_t>(-1) ||
				!m_code.PatchBranch(negative_overflow, m_code.Size(), VitaA32::Condition::LT))
			{
				return false;
			}

			if (!m_code.EmitMovImm32(HOST_TMP0, 0x80000000u) ||
				!m_code.EmitMovImm32(HOST_TMP1, 0xffffffffu))
			{
				return false;
			}

			const size_t negative_done = m_code.EmitBranchPlaceholder();
			if (negative_done == static_cast<size_t>(-1) ||
				!m_code.PatchBranch(no_overflow, m_code.Size(), VitaA32::Condition::EQ))
			{
				return false;
			}

			if (!m_code.EmitMovRegShiftImm(HOST_TMP1, HOST_TMP2, VitaA32::ShiftType::LSL, 0) ||
				!m_code.PatchBranch(positive_done, m_code.Size()) ||
				!m_code.PatchBranch(negative_done, m_code.Size()))
			{
				return false;
			}

			return m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS,
					   static_cast<u16>(rd_offset + dst_doubleword * sizeof(u64))) &&
				   m_code.EmitStrImm12(HOST_TMP1, HOST_CPU_REGS,
					   static_cast<u16>(rd_offset + dst_doubleword * sizeof(u64) + sizeof(u32)));
		};

		switch (SA(op))
		{
			case 0x00: // LW
				return emit_word_move(LO_OFFSET, 0) &&
					   emit_word_move(HI_OFFSET, 1) &&
					   emit_word_move(LO_OFFSET + 2 * sizeof(u32), 2) &&
					   emit_word_move(HI_OFFSET + 2 * sizeof(u32), 3);
			case 0x01: // UW
				return emit_word_move(LO_OFFSET + sizeof(u32), 0) &&
					   emit_word_move(HI_OFFSET + sizeof(u32), 1) &&
					   emit_word_move(LO_OFFSET + 3 * sizeof(u32), 2) &&
					   emit_word_move(HI_OFFSET + 3 * sizeof(u32), 3);
			case 0x02: // SLW
				return emit_saturating_word(LO_OFFSET, HI_OFFSET, 0) &&
					   emit_saturating_word(LO_OFFSET + 2 * sizeof(u32), HI_OFFSET + 2 * sizeof(u32), 1);
			case 0x03: // LH
				return emit_halfword_pack(LO_OFFSET, LO_OFFSET + sizeof(u32), 0, false) &&
					   emit_halfword_pack(HI_OFFSET, HI_OFFSET + sizeof(u32), 1, false) &&
					   emit_halfword_pack(LO_OFFSET + 2 * sizeof(u32), LO_OFFSET + 3 * sizeof(u32), 2, false) &&
					   emit_halfword_pack(HI_OFFSET + 2 * sizeof(u32), HI_OFFSET + 3 * sizeof(u32), 3, false);
			case 0x04: // SH
				return emit_halfword_pack(LO_OFFSET, LO_OFFSET + sizeof(u32), 0, true) &&
					   emit_halfword_pack(HI_OFFSET, HI_OFFSET + sizeof(u32), 1, true) &&
					   emit_halfword_pack(LO_OFFSET + 2 * sizeof(u32), LO_OFFSET + 3 * sizeof(u32), 2, true) &&
					   emit_halfword_pack(HI_OFFSET + 2 * sizeof(u32), HI_OFFSET + 3 * sizeof(u32), 3, true);
			default:
				return true;
		}
	}

	bool BlockCompiler::EmitPMTHL(u32 op)
	{
		// PCSX2 owner: MMI.cpp::PMTHL(), which only changes LO/HI for SA == 0.
		if (SA(op) != 0)
			return true;

		const unsigned rs = RS(op);
		const auto emit_load_source_word = [this, rs](unsigned dst_reg, unsigned source_word) {
			if (rs == 0)
				return m_code.EmitMovImm8(dst_reg, 0);

			return m_code.EmitLdrImm12(dst_reg, HOST_CPU_REGS,
				static_cast<u16>(GprOffset(rs) + source_word * sizeof(u32)));
		};
		const auto emit_move_word = [this, &emit_load_source_word](unsigned source_word, size_t dst_offset) {
			return emit_load_source_word(HOST_TMP0, source_word) &&
				   m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(dst_offset));
		};

		return emit_move_word(0, LO_OFFSET) &&
			   emit_move_word(1, HI_OFFSET) &&
			   emit_move_word(2, LO_OFFSET + 2 * sizeof(u32)) &&
			   emit_move_word(3, HI_OFFSET + 2 * sizeof(u32));
	}

	bool BlockCompiler::EmitMFHI(u32 op)
	{
		// PCSX2 owner: R5900OpcodeImpl.cpp::MFHI().
		return EmitMoveFromHiLo(op, HI_OFFSET);
	}

	bool BlockCompiler::EmitMFLO(u32 op)
	{
		// PCSX2 owner: R5900OpcodeImpl.cpp::MFLO().
		return EmitMoveFromHiLo(op, LO_OFFSET);
	}

	bool BlockCompiler::EmitMTHI(u32 op)
	{
		// PCSX2 owner: R5900OpcodeImpl.cpp::MTHI().
		return EmitMoveToHiLo(op, HI_OFFSET);
	}

	bool BlockCompiler::EmitMTLO(u32 op)
	{
		// PCSX2 owner: R5900OpcodeImpl.cpp::MTLO().
		return EmitMoveToHiLo(op, LO_OFFSET);
	}

	bool BlockCompiler::EmitMoveFromHiLo(u32 op, size_t hilo_offset)
	{
		const unsigned rd = RD(op);
		if (rd == 0)
			return true;

		return m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(hilo_offset)) &&
			   m_code.EmitLdrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(hilo_offset + sizeof(u32))) &&
			   EmitStoreGpr64(rd, HOST_TMP0, HOST_TMP1);
	}

	bool BlockCompiler::EmitMoveToHiLo(u32 op, size_t hilo_offset)
	{
		return EmitLoadGpr64(RS(op), HOST_TMP0, HOST_TMP1) &&
			   m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(hilo_offset)) &&
			   m_code.EmitStrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(hilo_offset + sizeof(u32)));
	}

	bool BlockCompiler::EmitMoveFullFromHiLo(u32 op, size_t hilo_offset)
	{
		const unsigned rd = RD(op);
		if (rd == 0)
			return true;

		constexpr unsigned NEON_VALUE = 0;
		const auto emit_cpu_regs_address = [this](unsigned host_reg, size_t offset) {
			if (offset <= 255)
				return m_code.EmitAddImm8(host_reg, HOST_CPU_REGS, static_cast<u8>(offset));

			return m_code.EmitMovImm32(host_reg, static_cast<u32>(offset)) &&
				   m_code.EmitAddReg(host_reg, HOST_CPU_REGS, host_reg);
		};

		return emit_cpu_regs_address(HOST_TMP0, hilo_offset) &&
			   m_code.EmitVld1Q32(NEON_VALUE, HOST_TMP0) &&
			   emit_cpu_regs_address(HOST_TMP1, GprOffset(rd)) &&
			   m_code.EmitVst1Q32(NEON_VALUE, HOST_TMP1);
	}

	bool BlockCompiler::EmitMoveFullToHiLo(u32 op, size_t hilo_offset)
	{
		const unsigned rs = RS(op);

		constexpr unsigned NEON_VALUE = 0;
		const auto emit_cpu_regs_address = [this](unsigned host_reg, size_t offset) {
			if (offset <= 255)
				return m_code.EmitAddImm8(host_reg, HOST_CPU_REGS, static_cast<u8>(offset));

			return m_code.EmitMovImm32(host_reg, static_cast<u32>(offset)) &&
				   m_code.EmitAddReg(host_reg, HOST_CPU_REGS, host_reg);
		};

		if (rs == 0)
		{
			if (!m_code.EmitVeorQ(NEON_VALUE, NEON_VALUE, NEON_VALUE))
				return false;
		}
		else if (!emit_cpu_regs_address(HOST_TMP0, GprOffset(rs)) ||
				 !m_code.EmitVld1Q32(NEON_VALUE, HOST_TMP0))
		{
			return false;
		}

		return emit_cpu_regs_address(HOST_TMP1, hilo_offset) &&
			   m_code.EmitVst1Q32(NEON_VALUE, HOST_TMP1);
	}

	bool BlockCompiler::EmitMMI(u32 op)
	{
		switch (op & 0x3f)
		{
			case 0x00: // MADD, owned by MMI.cpp::MADD().
				return EmitMADD(op);
			case 0x01: // MADDU, owned by MMI.cpp::MADDU().
				return EmitMADDU(op);
			case 0x04: // PLZCW, owned by MMI.cpp::PLZCW().
				return EmitPLZCW(op);
			case 0x08: // MMI0 class, owned by R5900OpcodeTables.cpp::Class_MMI0().
				return EmitMMI0(op);
			case 0x09: // MMI2 class, owned by R5900OpcodeTables.cpp::Class_MMI2().
				return EmitMMI2(op);
			case 0x28: // MMI1 class, owned by R5900OpcodeTables.cpp::Class_MMI1().
				return EmitMMI1(op);
			case 0x29: // MMI3 class, owned by R5900OpcodeTables.cpp::Class_MMI3().
				return EmitMMI3(op);
			case 0x10: // MFHI1, owned by MMI.cpp::MFHI1().
				return EmitMFHI1(op);
			case 0x11: // MTHI1, owned by MMI.cpp::MTHI1().
				return EmitMTHI1(op);
			case 0x12: // MFLO1, owned by MMI.cpp::MFLO1().
				return EmitMFLO1(op);
			case 0x13: // MTLO1, owned by MMI.cpp::MTLO1().
				return EmitMTLO1(op);
			case 0x18: // MULT1, owned by MMI.cpp::MULT1().
				return EmitMULT1(op);
			case 0x19: // MULTU1, owned by MMI.cpp::MULTU1().
				return EmitMULTU1(op);
			case 0x1a: // DIV1, owned by MMI.cpp::DIV1().
				return EmitDIV1(op);
			case 0x1b: // DIVU1, owned by MMI.cpp::DIVU1().
				return EmitDIVU1(op);
			case 0x20: // MADD1, owned by MMI.cpp::MADD1().
				return EmitMADD1(op);
			case 0x21: // MADDU1, owned by MMI.cpp::MADDU1().
				return EmitMADDU1(op);
			case 0x30: // PMFHL, owned by MMI.cpp::PMFHL().
				return EmitPMFHL(op);
			case 0x31: // PMTHL, owned by MMI.cpp::PMTHL().
				return EmitPMTHL(op);
			case 0x34: // PSLLH, owned by MMI.cpp::PSLLH().
				return EmitPSLLH(op);
			case 0x36: // PSRLH, owned by MMI.cpp::PSRLH().
				return EmitPSRLH(op);
			case 0x37: // PSRAH, owned by MMI.cpp::PSRAH().
				return EmitPSRAH(op);
			case 0x3c: // PSLLW, owned by MMI.cpp::PSLLW().
				return EmitPSLLW(op);
			case 0x3e: // PSRLW, owned by MMI.cpp::PSRLW().
				return EmitPSRLW(op);
			case 0x3f: // PSRAW, owned by MMI.cpp::PSRAW().
				return EmitPSRAW(op);
			default:
				return false;
		}
	}

	bool BlockCompiler::EmitMMI0(u32 op)
	{
		switch ((op >> 6) & 0x1f)
		{
			case 0x00: // PADDW, owned by MMI.cpp::PADDW().
				return EmitPADDW(op);
			case 0x01: // PSUBW, owned by MMI.cpp::PSUBW().
				return EmitPSUBW(op);
			case 0x02: // PCGTW, owned by MMI.cpp::PCGTW().
				return EmitPCGTW(op);
			case 0x03: // PMAXW, owned by MMI.cpp::PMAXW().
				return EmitPMAXW(op);
			case 0x04: // PADDH, owned by MMI.cpp::PADDH().
				return EmitPADDH(op);
			case 0x05: // PSUBH, owned by MMI.cpp::PSUBH().
				return EmitPSUBH(op);
			case 0x06: // PCGTH, owned by MMI.cpp::PCGTH().
				return EmitPCGTH(op);
			case 0x07: // PMAXH, owned by MMI.cpp::PMAXH().
				return EmitPMAXH(op);
			case 0x08: // PADDB, owned by MMI.cpp::PADDB().
				return EmitPADDB(op);
			case 0x09: // PSUBB, owned by MMI.cpp::PSUBB().
				return EmitPSUBB(op);
			case 0x0a: // PCGTB, owned by MMI.cpp::PCGTB().
				return EmitPCGTB(op);
			case 0x10: // PADDSW, owned by MMI.cpp::PADDSW().
				return EmitPADDSW(op);
			case 0x11: // PSUBSW, owned by MMI.cpp::PSUBSW().
				return EmitPSUBSW(op);
			case 0x12: // PEXTLW, owned by MMI.cpp::PEXTLW().
				return EmitPEXTLW(op);
			case 0x13: // PPACW, owned by MMI.cpp::PPACW().
				return EmitPPACW(op);
			case 0x14: // PADDSH, owned by MMI.cpp::PADDSH().
				return EmitPADDSH(op);
			case 0x15: // PSUBSH, owned by MMI.cpp::PSUBSH().
				return EmitPSUBSH(op);
			case 0x16: // PEXTLH, owned by MMI.cpp::PEXTLH().
				return EmitPEXTLH(op);
			case 0x17: // PPACH, owned by MMI.cpp::PPACH().
				return EmitPPACH(op);
			case 0x18: // PADDSB, owned by MMI.cpp::PADDSB().
				return EmitPADDSB(op);
			case 0x19: // PSUBSB, owned by MMI.cpp::PSUBSB().
				return EmitPSUBSB(op);
			case 0x1a: // PEXTLB, owned by MMI.cpp::PEXTLB().
				return EmitPEXTLB(op);
			case 0x1b: // PPACB, owned by MMI.cpp::PPACB().
				return EmitPPACB(op);
			case 0x1e: // PEXT5, owned by MMI.cpp::PEXT5().
				return EmitPEXT5(op);
			case 0x1f: // PPAC5, owned by MMI.cpp::PPAC5().
				return EmitPPAC5(op);
			default:
				return false;
		}
	}

	bool BlockCompiler::EmitMMI1(u32 op)
	{
		switch ((op >> 6) & 0x1f)
		{
			case 0x01: // PABSW, owned by MMI.cpp::PABSW().
				return EmitPABSW(op);
			case 0x02: // PCEQW, owned by MMI.cpp::PCEQW().
				return EmitPCEQW(op);
			case 0x03: // PMINW, owned by MMI.cpp::PMINW().
				return EmitPMINW(op);
			case 0x04: // PADSBH, owned by MMI.cpp::PADSBH().
				return EmitPADSBH(op);
			case 0x05: // PABSH, owned by MMI.cpp::PABSH().
				return EmitPABSH(op);
			case 0x06: // PCEQH, owned by MMI.cpp::PCEQH().
				return EmitPCEQH(op);
			case 0x07: // PMINH, owned by MMI.cpp::PMINH().
				return EmitPMINH(op);
			case 0x0a: // PCEQB, owned by MMI.cpp::PCEQB().
				return EmitPCEQB(op);
			case 0x10: // PADDUW, owned by MMI.cpp::PADDUW().
				return EmitPADDUW(op);
			case 0x11: // PSUBUW, owned by MMI.cpp::PSUBUW().
				return EmitPSUBUW(op);
			case 0x12: // PEXTUW, owned by MMI.cpp::PEXTUW().
				return EmitPEXTUW(op);
			case 0x14: // PADDUH, owned by MMI.cpp::PADDUH().
				return EmitPADDUH(op);
			case 0x15: // PSUBUH, owned by MMI.cpp::PSUBUH().
				return EmitPSUBUH(op);
			case 0x16: // PEXTUH, owned by MMI.cpp::PEXTUH().
				return EmitPEXTUH(op);
			case 0x18: // PADDUB, owned by MMI.cpp::PADDUB().
				return EmitPADDUB(op);
			case 0x19: // PSUBUB, owned by MMI.cpp::PSUBUB().
				return EmitPSUBUB(op);
			case 0x1a: // PEXTUB, owned by MMI.cpp::PEXTUB().
				return EmitPEXTUB(op);
			case 0x1b: // QFSRV, owned by MMI.cpp::QFSRV().
				return EmitQFSRV(op);
			default:
				return false;
		}
	}

	bool BlockCompiler::EmitMMI2(u32 op)
	{
		switch ((op >> 6) & 0x1f)
		{
			case 0x00: // PMADDW, owned by MMI.cpp::PMADDW().
				return EmitPMADDW(op);
			case 0x02: // PSLLVW, owned by MMI.cpp::PSLLVW().
				return EmitPSLLVW(op);
			case 0x03: // PSRLVW, owned by MMI.cpp::PSRLVW().
				return EmitPSRLVW(op);
			case 0x04: // PMSUBW, owned by MMI.cpp::PMSUBW().
				return EmitPMSUBW(op);
			case 0x08: // PMFHI, owned by MMI.cpp::PMFHI().
				return EmitPMFHI(op);
			case 0x09: // PMFLO, owned by MMI.cpp::PMFLO().
				return EmitPMFLO(op);
			case 0x0a: // PINTH, owned by MMI.cpp::PINTH().
				return EmitPINTH(op);
			case 0x0c: // PMULTW, owned by MMI.cpp::PMULTW().
				return EmitPMULTW(op);
			case 0x0d: // PDIVW, owned by MMI.cpp::PDIVW().
				return EmitPDIVW(op);
			case 0x0e: // PCPYLD, owned by MMI.cpp::PCPYLD().
				return EmitPCPYLD(op);
			case 0x10: // PMADDH, owned by MMI.cpp::PMADDH().
				return EmitPMADDH(op);
			case 0x11: // PHMADH, owned by MMI.cpp::PHMADH().
				return EmitPHMADH(op);
			case 0x12: // PAND, owned by MMI.cpp::PAND().
				return EmitPAND(op);
			case 0x13: // PXOR, owned by MMI.cpp::PXOR().
				return EmitPXOR(op);
			case 0x14: // PMSUBH, owned by MMI.cpp::PMSUBH().
				return EmitPMSUBH(op);
			case 0x15: // PHMSBH, owned by MMI.cpp::PHMSBH().
				return EmitPHMSBH(op);
			case 0x1a: // PEXEH, owned by MMI.cpp::PEXEH().
				return EmitPEXEH(op);
			case 0x1b: // PREVH, owned by MMI.cpp::PREVH().
				return EmitPREVH(op);
			case 0x1c: // PMULTH, owned by MMI.cpp::PMULTH().
				return EmitPMULTH(op);
			case 0x1d: // PDIVBW, owned by MMI.cpp::PDIVBW().
				return EmitPDIVBW(op);
			case 0x1e: // PEXEW, owned by MMI.cpp::PEXEW().
				return EmitPEXEW(op);
			case 0x1f: // PROT3W, owned by MMI.cpp::PROT3W().
				return EmitPROT3W(op);
			default:
				return false;
		}
	}

	bool BlockCompiler::EmitMMI3(u32 op)
	{
		switch ((op >> 6) & 0x1f)
		{
			case 0x00: // PMADDUW, owned by MMI.cpp::PMADDUW().
				return EmitPMADDUW(op);
			case 0x03: // PSRAVW, owned by MMI.cpp::PSRAVW().
				return EmitPSRAVW(op);
			case 0x08: // PMTHI, owned by MMI.cpp::PMTHI().
				return EmitPMTHI(op);
			case 0x09: // PMTLO, owned by MMI.cpp::PMTLO().
				return EmitPMTLO(op);
			case 0x0a: // PINTEH, owned by MMI.cpp::PINTEH().
				return EmitPINTEH(op);
			case 0x0c: // PMULTUW, owned by MMI.cpp::PMULTUW().
				return EmitPMULTUW(op);
			case 0x0d: // PDIVUW, owned by MMI.cpp::PDIVUW().
				return EmitPDIVUW(op);
			case 0x0e: // PCPYUD, owned by MMI.cpp::PCPYUD().
				return EmitPCPYUD(op);
			case 0x12: // POR, owned by MMI.cpp::POR().
				return EmitPOR(op);
			case 0x13: // PNOR, owned by MMI.cpp::PNOR().
				return EmitPNOR(op);
			case 0x1a: // PEXCH, owned by MMI.cpp::PEXCH().
				return EmitPEXCH(op);
			case 0x1b: // PCPYH, owned by MMI.cpp::PCPYH().
				return EmitPCPYH(op);
			case 0x1e: // PEXCW, owned by MMI.cpp::PEXCW().
				return EmitPEXCW(op);
			default:
				return false;
		}
	}

	bool BlockCompiler::EmitPMADDW(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI2() dispatches this
		// opcode to MMI.cpp::PMADDW(); the lower lane has the PS2 high-word
		// adjustment documented in the interpreter.
		return EmitPackedSignedWordMultiplyAccumulate(op, false);
	}

	bool BlockCompiler::EmitPMSUBW(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI2() dispatches this
		// opcode to MMI.cpp::PMSUBW(); x86/iMMI.cpp::recPMSUBW() uses the same
		// signed product and LO/HI accumulator lanes.
		return EmitPackedSignedWordMultiplyAccumulate(op, true);
	}

	bool BlockCompiler::EmitPMADDH(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI2() dispatches this
		// opcode to MMI.cpp::PMADDH(), which adds eight signed 16x16 products
		// into the packed LO/HI word accumulators.
		return EmitPackedHalfwordMultiplyAccumulate(op, false);
	}

	bool BlockCompiler::EmitPMSUBH(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI2() dispatches this
		// opcode to MMI.cpp::PMSUBH(), which subtracts eight signed 16x16
		// products from the packed LO/HI word accumulators.
		return EmitPackedHalfwordMultiplyAccumulate(op, true);
	}

	bool BlockCompiler::EmitPHMADH(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI2() dispatches this
		// opcode to MMI.cpp::PHMADH(), which writes pairwise signed 16x16
		// sums into even LO/HI words and the second pair product into odd words.
		return EmitPackedHalfwordPairMultiply(op, false);
	}

	bool BlockCompiler::EmitPHMSBH(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI2() dispatches this
		// opcode to MMI.cpp::PHMSBH(), which writes pairwise signed 16x16
		// differences and the undocumented bitwise-not second product lanes.
		return EmitPackedHalfwordPairMultiply(op, true);
	}

	bool BlockCompiler::EmitPMULTW(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI2() dispatches this
		// opcode to MMI.cpp::PMULTW(), which multiplies RS.SL[0]/[2] by
		// RT.SL[0]/[2], writes the raw 64-bit products to RD, and writes the
		// sign-extended low/high 32-bit halves into LO/HI.
		return EmitPackedWordMultiply(op, true);
	}

	bool BlockCompiler::EmitPMULTUW(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI3() dispatches this
		// opcode to MMI.cpp::PMULTUW(), which multiplies RS.UL[0]/[2] by
		// RT.UL[0]/[2], writes unsigned 64-bit products to RD, and still
		// sign-extends the low/high 32-bit halves into LO/HI.
		return EmitPackedWordMultiply(op, false);
	}

	bool BlockCompiler::EmitPMULTH(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI2() dispatches this
		// opcode to MMI.cpp::PMULTH(), which multiplies all eight signed
		// halfword lanes into LO/HI words and exposes lanes 0/2/4/6 through RD.
		return EmitPackedHalfwordMultiply(op);
	}

	bool BlockCompiler::EmitPMADDUW(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI3() dispatches this
		// opcode to MMI.cpp::PMADDUW(), which adds each unsigned 32x32
		// product to the raw 64-bit LO.UL[0/2] + HI.UL[0/2] accumulator.
		return EmitPackedUnsignedWordMultiplyAdd(op);
	}

	bool BlockCompiler::EmitPDIVW(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI2() dispatches this
		// opcode to MMI.cpp::PDIVW(). Cortex-A9 has no integer divide, so the
		// no-divide edge cases are emitted directly and true division keeps the
		// exact PCSX2 helper semantics.
		return EmitPackedWordDivide(op, true);
	}

	bool BlockCompiler::EmitPDIVUW(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI3() dispatches this
		// opcode to MMI.cpp::PDIVUW(). Cortex-A9 has no integer divide, so the
		// no-divide edge cases are emitted directly and true division keeps the
		// exact PCSX2 helper semantics.
		return EmitPackedWordDivide(op, false);
	}

	bool BlockCompiler::EmitPDIVBW(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI2() dispatches this
		// opcode to MMI.cpp::PDIVBW(), which divides all four signed RS words
		// by RT.SS[0] and writes all four 32-bit LO/HI words directly. Divisor
		// 0, 1, and -1 are emitted directly; other divisors call the PCSX2-shaped
		// helper because Cortex-A9 has no integer divide.
		return EmitPackedWordByHalfwordDivide(op);
	}

	bool BlockCompiler::EmitPackedWordMultiply(u32 op, bool signed_multiply)
	{
		const unsigned rd = RD(op);
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);

		const auto load_word = [this](unsigned guest_reg, unsigned word, unsigned host_reg) {
			if (guest_reg == 0)
				return m_code.EmitMovImm8(host_reg, 0);

			return m_code.EmitLdrImm12(host_reg, HOST_CPU_REGS,
				static_cast<u16>(GprOffset(guest_reg) + word * sizeof(u32)));
		};

		const auto emit_lane = [this, rd, rs, rt, signed_multiply, load_word](unsigned source_word, unsigned dest_lane) {
			const size_t rd_offset = GprOffset(rd) + dest_lane * sizeof(u64);
			const size_t lo_offset = LO_OFFSET + dest_lane * sizeof(u64);
			const size_t hi_offset = HI_OFFSET + dest_lane * sizeof(u64);

			if (!load_word(rs, source_word, HOST_TMP0) ||
				!load_word(rt, source_word, HOST_TMP1))
			{
				return false;
			}

			if (signed_multiply)
			{
				if (!m_code.EmitSmull(HOST_TMP2, HOST_TMP3, HOST_TMP0, HOST_TMP1))
					return false;
			}
			else
			{
				if (!m_code.EmitUmull(HOST_TMP2, HOST_TMP3, HOST_TMP0, HOST_TMP1))
					return false;
			}

			if (rd != 0 &&
				(!m_code.EmitStrImm12(HOST_TMP2, HOST_CPU_REGS, static_cast<u16>(rd_offset)) ||
					!m_code.EmitStrImm12(HOST_TMP3, HOST_CPU_REGS, static_cast<u16>(rd_offset + sizeof(u32)))))
			{
				return false;
			}

			if (!m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_TMP2, VitaA32::ShiftType::ASR, 31) ||
				!m_code.EmitStrImm12(HOST_TMP2, HOST_CPU_REGS, static_cast<u16>(lo_offset)) ||
				!m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(lo_offset + sizeof(u32))) ||
				!m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_TMP3, VitaA32::ShiftType::ASR, 31) ||
				!m_code.EmitStrImm12(HOST_TMP3, HOST_CPU_REGS, static_cast<u16>(hi_offset)) ||
				!m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(hi_offset + sizeof(u32))))
			{
				return false;
			}

			return true;
		};

		return emit_lane(0, 0) && emit_lane(2, 1);
	}

	bool BlockCompiler::EmitPackedWordDivide(u32 op, bool signed_divide)
	{
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		struct BranchPatch
		{
			size_t offset = static_cast<size_t>(-1);
			VitaA32::Condition condition = VitaA32::Condition::AL;
		};

		const auto load_word = [this](unsigned guest_reg, unsigned word, unsigned host_reg) {
			if (guest_reg == 0)
				return m_code.EmitMovImm8(host_reg, 0);

			return m_code.EmitLdrImm12(host_reg, HOST_CPU_REGS,
				static_cast<u16>(GprOffset(guest_reg) + word * sizeof(u32)));
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

		const auto store_signed_word_as_doubleword = [this](unsigned value_reg, size_t offset) {
			return m_code.EmitMovRegShiftImm(HOST_TMP4, value_reg, VitaA32::ShiftType::ASR, 31) &&
				   m_code.EmitStrImm12(value_reg, HOST_CPU_REGS, static_cast<u16>(offset)) &&
				   m_code.EmitStrImm12(HOST_TMP4, HOST_CPU_REGS, static_cast<u16>(offset + sizeof(u32)));
		};

		BranchPatch fallback_branches[2]{};
		unsigned fallback_branch_count = 0;
		const auto emit_fallback = [&]() {
			if (fallback_branch_count >= 2)
				return false;
			return emit_branch(fallback_branches[fallback_branch_count++], VitaA32::Condition::AL);
		};

		const auto emit_signed_fast_guard = [&](unsigned dividend_reg, unsigned divisor_reg) {
			BranchPatch ok_branches[6]{};
			unsigned ok_branch_count = 0;
			if (!m_code.EmitMovImm8(HOST_TMP4, 0) ||
				!m_code.EmitCmpReg(divisor_reg, HOST_TMP4) ||
				!emit_branch(ok_branches[ok_branch_count++], VitaA32::Condition::EQ) ||
				!m_code.EmitCmpReg(dividend_reg, HOST_TMP4) ||
				!emit_branch(ok_branches[ok_branch_count++], VitaA32::Condition::EQ) ||
				!m_code.EmitMovImm8(HOST_TMP4, 1) ||
				!m_code.EmitCmpReg(divisor_reg, HOST_TMP4) ||
				!emit_branch(ok_branches[ok_branch_count++], VitaA32::Condition::EQ) ||
				!m_code.EmitMovImm32(HOST_TMP4, 0xffffffffu) ||
				!m_code.EmitCmpReg(divisor_reg, HOST_TMP4) ||
				!emit_branch(ok_branches[ok_branch_count++], VitaA32::Condition::EQ) ||
				!m_code.EmitCmpReg(dividend_reg, divisor_reg) ||
				!emit_branch(ok_branches[ok_branch_count++], VitaA32::Condition::EQ) ||
				!m_code.EmitMovRegShiftImm(HOST_TMP4, divisor_reg, VitaA32::ShiftType::ASR, 31) ||
				!m_code.EmitEorReg(HOST_TMP5, divisor_reg, HOST_TMP4) ||
				!m_code.EmitSubReg(HOST_TMP5, HOST_TMP5, HOST_TMP4) ||
				!m_code.EmitSubImm8(HOST_TMP4, HOST_TMP5, 1) ||
				!m_code.EmitAndReg(HOST_TMP4, HOST_TMP5, HOST_TMP4, true) ||
				!emit_branch(ok_branches[ok_branch_count++], VitaA32::Condition::EQ) ||
				!emit_fallback())
			{
				return false;
			}

			return patch_branches(ok_branches, ok_branch_count, m_code.Size());
		};

		const auto emit_unsigned_fast_guard = [&](unsigned dividend_reg, unsigned divisor_reg) {
			BranchPatch ok_branches[5]{};
			unsigned ok_branch_count = 0;
			if (!m_code.EmitMovImm8(HOST_TMP4, 0) ||
				!m_code.EmitCmpReg(divisor_reg, HOST_TMP4) ||
				!emit_branch(ok_branches[ok_branch_count++], VitaA32::Condition::EQ) ||
				!m_code.EmitCmpReg(dividend_reg, HOST_TMP4) ||
				!emit_branch(ok_branches[ok_branch_count++], VitaA32::Condition::EQ) ||
				!m_code.EmitMovImm8(HOST_TMP4, 1) ||
				!m_code.EmitCmpReg(divisor_reg, HOST_TMP4) ||
				!emit_branch(ok_branches[ok_branch_count++], VitaA32::Condition::EQ) ||
				!m_code.EmitCmpReg(dividend_reg, divisor_reg) ||
				!emit_branch(ok_branches[ok_branch_count++], VitaA32::Condition::LS) ||
				!m_code.EmitSubImm8(HOST_TMP4, divisor_reg, 1) ||
				!m_code.EmitAndReg(HOST_TMP4, divisor_reg, HOST_TMP4, true) ||
				!emit_branch(ok_branches[ok_branch_count++], VitaA32::Condition::EQ) ||
				!emit_fallback())
			{
				return false;
			}

			return patch_branches(ok_branches, ok_branch_count, m_code.Size());
		};

		const auto emit_signed_direct_lane = [&](unsigned source_word, unsigned dest_lane) {
			const size_t lo_offset = LO_OFFSET + dest_lane * sizeof(u64);
			const size_t hi_offset = HI_OFFSET + dest_lane * sizeof(u64);
			BranchPatch divzero_branch{};
			BranchPatch zero_branch{};
			BranchPatch divone_branch{};
			BranchPatch negone_branch{};
			BranchPatch equal_branch{};
			BranchPatch done_branches[6]{};
			unsigned done_branch_count = 0;

			if (!load_word(rs, source_word, HOST_TMP0) ||
				!load_word(rt, source_word, HOST_TMP1) ||
				!m_code.EmitMovImm8(HOST_TMP3, 0) ||
				!m_code.EmitCmpReg(HOST_TMP1, HOST_TMP3) ||
				!emit_branch(divzero_branch, VitaA32::Condition::EQ) ||
				!m_code.EmitCmpReg(HOST_TMP0, HOST_TMP3) ||
				!emit_branch(zero_branch, VitaA32::Condition::EQ) ||
				!m_code.EmitMovImm8(HOST_TMP3, 1) ||
				!m_code.EmitCmpReg(HOST_TMP1, HOST_TMP3) ||
				!emit_branch(divone_branch, VitaA32::Condition::EQ) ||
				!m_code.EmitMovImm32(HOST_TMP3, 0xffffffffu) ||
				!m_code.EmitCmpReg(HOST_TMP1, HOST_TMP3) ||
				!emit_branch(negone_branch, VitaA32::Condition::EQ) ||
				!m_code.EmitCmpReg(HOST_TMP0, HOST_TMP1) ||
				!emit_branch(equal_branch, VitaA32::Condition::EQ))
			{
				return false;
			}

			if (!m_code.EmitMovRegShiftImm(HOST_TMP3, HOST_TMP0, VitaA32::ShiftType::ASR, 31) ||
				!m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP1, VitaA32::ShiftType::ASR, 31) ||
				!m_code.EmitEorReg(HOST_TMP4, HOST_TMP1, HOST_TMP2) ||
				!m_code.EmitSubReg(HOST_TMP4, HOST_TMP4, HOST_TMP2) ||
				!m_code.EmitSubImm8(HOST_TMP2, HOST_TMP4, 1) ||
				!m_code.EmitAndReg(HOST_TMP3, HOST_TMP3, HOST_TMP2) ||
				!m_code.EmitAddReg(HOST_TMP3, HOST_TMP0, HOST_TMP3) ||
				!m_code.EmitClz(HOST_TMP4, HOST_TMP4) ||
				!m_code.EmitMovImm8(HOST_TMP2, 31) ||
				!m_code.EmitSubReg(HOST_TMP2, HOST_TMP2, HOST_TMP4) ||
				!m_code.EmitMovRegShiftReg(HOST_TMP4, HOST_TMP3, VitaA32::ShiftType::ASR, HOST_TMP2) ||
				!m_code.EmitMovRegShiftReg(HOST_TMP3, HOST_TMP4, VitaA32::ShiftType::LSL, HOST_TMP2) ||
				!m_code.EmitSubReg(HOST_TMP3, HOST_TMP0, HOST_TMP3) ||
				!m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP1, VitaA32::ShiftType::ASR, 31) ||
				!m_code.EmitEorReg(HOST_TMP4, HOST_TMP4, HOST_TMP2) ||
				!m_code.EmitSubReg(HOST_TMP2, HOST_TMP4, HOST_TMP2) ||
				!store_signed_word_as_doubleword(HOST_TMP2, lo_offset) ||
				!store_signed_word_as_doubleword(HOST_TMP3, hi_offset) ||
				!emit_branch(done_branches[done_branch_count++], VitaA32::Condition::AL))
			{
				return false;
			}

			if (!patch_branch(negone_branch, m_code.Size()) ||
				!m_code.EmitMovImm8(HOST_TMP3, 0) ||
				!m_code.EmitSubReg(HOST_TMP2, HOST_TMP3, HOST_TMP0) ||
				!store_signed_word_as_doubleword(HOST_TMP2, lo_offset) ||
				!m_code.EmitMovImm8(HOST_TMP2, 0) ||
				!store_signed_word_as_doubleword(HOST_TMP2, hi_offset) ||
				!emit_branch(done_branches[done_branch_count++], VitaA32::Condition::AL))
			{
				return false;
			}

			if (!patch_branch(equal_branch, m_code.Size()) ||
				!m_code.EmitMovImm8(HOST_TMP2, 1) ||
				!store_signed_word_as_doubleword(HOST_TMP2, lo_offset) ||
				!m_code.EmitMovImm8(HOST_TMP2, 0) ||
				!store_signed_word_as_doubleword(HOST_TMP2, hi_offset) ||
				!emit_branch(done_branches[done_branch_count++], VitaA32::Condition::AL))
			{
				return false;
			}

			if (!patch_branch(divone_branch, m_code.Size()) ||
				!m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP0, VitaA32::ShiftType::LSL, 0) ||
				!store_signed_word_as_doubleword(HOST_TMP2, lo_offset) ||
				!m_code.EmitMovImm8(HOST_TMP2, 0) ||
				!store_signed_word_as_doubleword(HOST_TMP2, hi_offset) ||
				!emit_branch(done_branches[done_branch_count++], VitaA32::Condition::AL))
			{
				return false;
			}

			if (!patch_branch(zero_branch, m_code.Size()) ||
				!m_code.EmitMovImm8(HOST_TMP2, 0) ||
				!store_signed_word_as_doubleword(HOST_TMP2, lo_offset) ||
				!store_signed_word_as_doubleword(HOST_TMP2, hi_offset) ||
				!emit_branch(done_branches[done_branch_count++], VitaA32::Condition::AL))
			{
				return false;
			}

			if (!patch_branch(divzero_branch, m_code.Size()) ||
				!m_code.EmitMovImm32(HOST_TMP2, 0xffffffffu) ||
				!m_code.EmitMovImm8(HOST_TMP3, 0) ||
				!m_code.EmitCmpReg(HOST_TMP0, HOST_TMP3) ||
				!m_code.EmitMovImm8(HOST_TMP2, 1, VitaA32::Condition::LT) ||
				!store_signed_word_as_doubleword(HOST_TMP2, lo_offset) ||
				!store_signed_word_as_doubleword(HOST_TMP0, hi_offset))
			{
				return false;
			}

			return patch_branches(done_branches, done_branch_count, m_code.Size());
		};

		const auto emit_unsigned_direct_lane = [&](unsigned source_word, unsigned dest_lane) {
			const size_t lo_offset = LO_OFFSET + dest_lane * sizeof(u64);
			const size_t hi_offset = HI_OFFSET + dest_lane * sizeof(u64);
			BranchPatch divzero_branch{};
			BranchPatch zero_branch{};
			BranchPatch divone_branch{};
			BranchPatch below_branch{};
			BranchPatch equal_branch{};
			BranchPatch done_branches[6]{};
			unsigned done_branch_count = 0;

			if (!load_word(rs, source_word, HOST_TMP0) ||
				!load_word(rt, source_word, HOST_TMP1) ||
				!m_code.EmitMovImm8(HOST_TMP3, 0) ||
				!m_code.EmitCmpReg(HOST_TMP1, HOST_TMP3) ||
				!emit_branch(divzero_branch, VitaA32::Condition::EQ) ||
				!m_code.EmitCmpReg(HOST_TMP0, HOST_TMP3) ||
				!emit_branch(zero_branch, VitaA32::Condition::EQ) ||
				!m_code.EmitMovImm8(HOST_TMP3, 1) ||
				!m_code.EmitCmpReg(HOST_TMP1, HOST_TMP3) ||
				!emit_branch(divone_branch, VitaA32::Condition::EQ) ||
				!m_code.EmitCmpReg(HOST_TMP0, HOST_TMP1) ||
				!emit_branch(below_branch, VitaA32::Condition::CC) ||
				!emit_branch(equal_branch, VitaA32::Condition::EQ))
			{
				return false;
			}

			if (!m_code.EmitSubImm8(HOST_TMP2, HOST_TMP1, 1) ||
				!m_code.EmitAndReg(HOST_TMP3, HOST_TMP0, HOST_TMP2) ||
				!m_code.EmitClz(HOST_TMP2, HOST_TMP1) ||
				!m_code.EmitMovImm8(HOST_TMP4, 31) ||
				!m_code.EmitSubReg(HOST_TMP4, HOST_TMP4, HOST_TMP2) ||
				!m_code.EmitMovRegShiftReg(HOST_TMP2, HOST_TMP0, VitaA32::ShiftType::LSR, HOST_TMP4) ||
				!store_signed_word_as_doubleword(HOST_TMP2, lo_offset) ||
				!store_signed_word_as_doubleword(HOST_TMP3, hi_offset) ||
				!emit_branch(done_branches[done_branch_count++], VitaA32::Condition::AL))
			{
				return false;
			}

			if (!patch_branch(equal_branch, m_code.Size()) ||
				!m_code.EmitMovImm8(HOST_TMP2, 1) ||
				!store_signed_word_as_doubleword(HOST_TMP2, lo_offset) ||
				!m_code.EmitMovImm8(HOST_TMP2, 0) ||
				!store_signed_word_as_doubleword(HOST_TMP2, hi_offset) ||
				!emit_branch(done_branches[done_branch_count++], VitaA32::Condition::AL))
			{
				return false;
			}

			if (!patch_branch(below_branch, m_code.Size()) ||
				!m_code.EmitMovImm8(HOST_TMP2, 0) ||
				!store_signed_word_as_doubleword(HOST_TMP2, lo_offset) ||
				!store_signed_word_as_doubleword(HOST_TMP0, hi_offset) ||
				!emit_branch(done_branches[done_branch_count++], VitaA32::Condition::AL))
			{
				return false;
			}

			if (!patch_branch(divone_branch, m_code.Size()) ||
				!m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP0, VitaA32::ShiftType::LSL, 0) ||
				!store_signed_word_as_doubleword(HOST_TMP2, lo_offset) ||
				!m_code.EmitMovImm8(HOST_TMP2, 0) ||
				!store_signed_word_as_doubleword(HOST_TMP2, hi_offset) ||
				!emit_branch(done_branches[done_branch_count++], VitaA32::Condition::AL))
			{
				return false;
			}

			if (!patch_branch(zero_branch, m_code.Size()) ||
				!m_code.EmitMovImm8(HOST_TMP2, 0) ||
				!store_signed_word_as_doubleword(HOST_TMP2, lo_offset) ||
				!store_signed_word_as_doubleword(HOST_TMP2, hi_offset) ||
				!emit_branch(done_branches[done_branch_count++], VitaA32::Condition::AL))
			{
				return false;
			}

			if (!patch_branch(divzero_branch, m_code.Size()) ||
				!m_code.EmitMovImm32(HOST_TMP2, 0xffffffffu) ||
				!store_signed_word_as_doubleword(HOST_TMP2, lo_offset) ||
				!store_signed_word_as_doubleword(HOST_TMP0, hi_offset))
			{
				return false;
			}

			return patch_branches(done_branches, done_branch_count, m_code.Size());
		};

		if (!load_word(rs, 0, HOST_TMP0) ||
			!load_word(rt, 0, HOST_TMP1) ||
			!load_word(rs, 2, HOST_TMP2) ||
			!load_word(rt, 2, HOST_TMP3))
		{
			return false;
		}

		if (signed_divide)
		{
			if (!emit_signed_fast_guard(HOST_TMP0, HOST_TMP1) ||
				!emit_signed_fast_guard(HOST_TMP2, HOST_TMP3))
			{
				return false;
			}
		}
		else if (!emit_unsigned_fast_guard(HOST_TMP0, HOST_TMP1) ||
				 !emit_unsigned_fast_guard(HOST_TMP2, HOST_TMP3))
		{
			return false;
		}

		if (signed_divide)
		{
			if (!emit_signed_direct_lane(0, 0) ||
				!emit_signed_direct_lane(2, 1))
			{
				return false;
			}
		}
		else if (!emit_unsigned_direct_lane(0, 0) ||
				 !emit_unsigned_direct_lane(2, 1))
		{
			return false;
		}

		BranchPatch done_branch{};
		if (!emit_branch(done_branch, VitaA32::Condition::AL))
			return false;

		if (!patch_branches(fallback_branches, fallback_branch_count, m_code.Size()) ||
			!m_code.EmitCallAbsolute(signed_divide ?
					reinterpret_cast<const void*>(&VitaEePackedDivSignedWords) :
					reinterpret_cast<const void*>(&VitaEePackedDivUnsignedWords)))
		{
			return false;
		}

		return patch_branch(done_branch, m_code.Size());
	}

	bool BlockCompiler::EmitPackedWordByHalfwordDivide(u32 op)
	{
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
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

		const auto load_word = [this](unsigned guest_reg, unsigned word, unsigned host_reg) {
			if (guest_reg == 0)
				return m_code.EmitMovImm8(host_reg, 0);

			return m_code.EmitLdrImm12(host_reg, HOST_CPU_REGS,
				static_cast<u16>(GprOffset(guest_reg) + word * sizeof(u32)));
		};

		const auto load_divisor = [&]() {
			if (rt == 0)
				return m_code.EmitMovImm8(HOST_TMP1, 0);

			return m_code.EmitLdrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(GprOffset(rt))) &&
				   m_code.EmitMovRegShiftImm(HOST_TMP1, HOST_TMP1, VitaA32::ShiftType::LSL, 16) &&
				   m_code.EmitMovRegShiftImm(HOST_TMP1, HOST_TMP1, VitaA32::ShiftType::LSR, 16);
		};

		const auto emit_divzero_lane = [&](unsigned lane) {
			return load_word(rs, lane, HOST_TMP0) &&
				   m_code.EmitMovImm32(HOST_TMP2, 0xffffffffu) &&
				   m_code.EmitMovImm8(HOST_TMP3, 0) &&
				   m_code.EmitCmpReg(HOST_TMP0, HOST_TMP3) &&
				   m_code.EmitMovImm8(HOST_TMP2, 1, VitaA32::Condition::LT) &&
				   m_code.EmitStrImm12(HOST_TMP2, HOST_CPU_REGS,
					   static_cast<u16>(LO_OFFSET + lane * sizeof(u32))) &&
				   m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS,
					   static_cast<u16>(HI_OFFSET + lane * sizeof(u32)));
		};

		const auto emit_divone_lane = [&](unsigned lane) {
			return load_word(rs, lane, HOST_TMP0) &&
				   m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS,
					   static_cast<u16>(LO_OFFSET + lane * sizeof(u32))) &&
				   m_code.EmitMovImm8(HOST_TMP2, 0) &&
				   m_code.EmitStrImm12(HOST_TMP2, HOST_CPU_REGS,
					   static_cast<u16>(HI_OFFSET + lane * sizeof(u32)));
		};

		const auto emit_negone_lane = [&](unsigned lane) {
			return load_word(rs, lane, HOST_TMP0) &&
				   m_code.EmitMovImm8(HOST_TMP2, 0) &&
				   m_code.EmitSubReg(HOST_TMP3, HOST_TMP2, HOST_TMP0) &&
				   m_code.EmitStrImm12(HOST_TMP3, HOST_CPU_REGS,
					   static_cast<u16>(LO_OFFSET + lane * sizeof(u32))) &&
				   m_code.EmitStrImm12(HOST_TMP2, HOST_CPU_REGS,
					   static_cast<u16>(HI_OFFSET + lane * sizeof(u32)));
		};

		BranchPatch divzero_branch{};
		BranchPatch divone_branch{};
		BranchPatch negone_branch{};
		BranchPatch fallback_branch{};
		BranchPatch done_branches[3]{};
		unsigned done_branch_count = 0;

		if (!load_divisor() ||
			!m_code.EmitMovImm8(HOST_TMP4, 0) ||
			!m_code.EmitCmpReg(HOST_TMP1, HOST_TMP4) ||
			!emit_branch(divzero_branch, VitaA32::Condition::EQ) ||
			!m_code.EmitMovImm8(HOST_TMP4, 1) ||
			!m_code.EmitCmpReg(HOST_TMP1, HOST_TMP4) ||
			!emit_branch(divone_branch, VitaA32::Condition::EQ) ||
			!m_code.EmitMovImm32(HOST_TMP4, 0xffffu) ||
			!m_code.EmitCmpReg(HOST_TMP1, HOST_TMP4) ||
			!emit_branch(negone_branch, VitaA32::Condition::EQ) ||
			!emit_branch(fallback_branch, VitaA32::Condition::AL))
		{
			return false;
		}

		if (!patch_branch(divzero_branch, m_code.Size()) ||
			!emit_divzero_lane(0) ||
			!emit_divzero_lane(1) ||
			!emit_divzero_lane(2) ||
			!emit_divzero_lane(3) ||
			!emit_branch(done_branches[done_branch_count++], VitaA32::Condition::AL))
		{
			return false;
		}

		if (!patch_branch(divone_branch, m_code.Size()) ||
			!emit_divone_lane(0) ||
			!emit_divone_lane(1) ||
			!emit_divone_lane(2) ||
			!emit_divone_lane(3) ||
			!emit_branch(done_branches[done_branch_count++], VitaA32::Condition::AL))
		{
			return false;
		}

		if (!patch_branch(negone_branch, m_code.Size()) ||
			!emit_negone_lane(0) ||
			!emit_negone_lane(1) ||
			!emit_negone_lane(2) ||
			!emit_negone_lane(3) ||
			!emit_branch(done_branches[done_branch_count++], VitaA32::Condition::AL))
		{
			return false;
		}

		if (!patch_branch(fallback_branch, m_code.Size()) ||
			!m_code.EmitMovImm32(HOST_TMP0, op) ||
			!m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&VitaEePackedDivSignedWordsByHalfword)))
		{
			return false;
		}

		return patch_branches(done_branches, done_branch_count, m_code.Size());
	}

	bool BlockCompiler::EmitPackedSignedWordMultiplyAccumulate(u32 op, bool subtract)
	{
		const unsigned rd = RD(op);
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);

		const auto load_word = [this](unsigned guest_reg, unsigned word, unsigned host_reg) {
			if (guest_reg == 0)
				return m_code.EmitMovImm8(host_reg, 0);

			return m_code.EmitLdrImm12(host_reg, HOST_CPU_REGS,
				static_cast<u16>(GprOffset(guest_reg) + word * sizeof(u32)));
		};

		const auto store_signed_word_as_doubleword = [this](unsigned value_reg, size_t offset) {
			return m_code.EmitMovRegShiftImm(HOST_TMP0, value_reg, VitaA32::ShiftType::ASR, 31) &&
				   m_code.EmitStrImm12(value_reg, HOST_CPU_REGS, static_cast<u16>(offset)) &&
				   m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(offset + sizeof(u32)));
		};

		const auto emit_divide_signed_64_by_u32_max = [this]() {
			if (!m_code.EmitMovImm8(HOST_TMP0, 0) ||
				!m_code.EmitCmpReg(HOST_TMP3, HOST_TMP0))
			{
				return false;
			}

			const size_t negative_branch = m_code.EmitBranchPlaceholder(VitaA32::Condition::LT);
			if (negative_branch == static_cast<size_t>(-1))
				return false;

			if (!m_code.EmitMvnReg(HOST_TMP0, HOST_TMP3) ||
				!m_code.EmitCmpReg(HOST_TMP4, HOST_TMP0))
			{
				return false;
			}

			const size_t positive_no_increment = m_code.EmitBranchPlaceholder(VitaA32::Condition::CC);
			if (positive_no_increment == static_cast<size_t>(-1))
				return false;

			if (!m_code.EmitAddImm8(HOST_TMP3, HOST_TMP3, 1))
				return false;

			const size_t positive_done = m_code.EmitBranchPlaceholder();
			if (positive_done == static_cast<size_t>(-1))
				return false;

			const size_t negative_target = m_code.Size();
			if (!m_code.PatchBranch(negative_branch, negative_target, VitaA32::Condition::LT) ||
				!m_code.PatchBranch(positive_no_increment, positive_done, VitaA32::Condition::CC))
			{
				return false;
			}

			if (!m_code.EmitMovImm8(HOST_TMP0, 0) ||
				!m_code.EmitSubReg(HOST_TMP0, HOST_TMP0, HOST_TMP3) ||
				!m_code.EmitCmpReg(HOST_TMP4, HOST_TMP0))
			{
				return false;
			}

			const size_t negative_no_increment = m_code.EmitBranchPlaceholder(VitaA32::Condition::LS);
			if (negative_no_increment == static_cast<size_t>(-1))
				return false;

			if (!m_code.EmitAddImm8(HOST_TMP3, HOST_TMP3, 1))
				return false;

			const size_t done_target = m_code.Size();
			return m_code.PatchBranch(positive_done, done_target) &&
				   m_code.PatchBranch(negative_no_increment, done_target, VitaA32::Condition::LS);
		};

		const auto emit_pmaddw_lower_quirk = [this, rs, load_word]() {
			if (!m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP1, VitaA32::ShiftType::LSL, 1) ||
				!m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP2, VitaA32::ShiftType::LSR, 1) ||
				!m_code.EmitMovImm8(HOST_TMP0, 0) ||
				!m_code.EmitCmpReg(HOST_TMP2, HOST_TMP0))
			{
				return false;
			}

			const size_t compare_registers_from_zero = m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
			if (compare_registers_from_zero == static_cast<size_t>(-1))
				return false;

			if (!m_code.EmitMovImm32(HOST_TMP0, 0x7fffffffu) ||
				!m_code.EmitCmpReg(HOST_TMP2, HOST_TMP0))
			{
				return false;
			}

			const size_t no_quirk_from_mask = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (no_quirk_from_mask == static_cast<size_t>(-1))
				return false;

			const size_t compare_registers_target = m_code.Size();
			if (!m_code.PatchBranch(compare_registers_from_zero, compare_registers_target, VitaA32::Condition::EQ) ||
				!load_word(rs, 0, HOST_TMP0) ||
				!m_code.EmitCmpReg(HOST_TMP0, HOST_TMP1))
			{
				return false;
			}

			const size_t no_quirk_from_equal_operands = m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
			if (no_quirk_from_equal_operands == static_cast<size_t>(-1))
				return false;

			if (!m_code.EmitMovImm32(HOST_TMP0, 0x70000000u) ||
				!m_code.EmitAddReg(HOST_TMP4, HOST_TMP4, HOST_TMP0, true) ||
				!m_code.EmitAdcImm8(HOST_TMP3, HOST_TMP3, 0))
			{
				return false;
			}

			const size_t done_target = m_code.Size();
			return m_code.PatchBranch(no_quirk_from_mask, done_target, VitaA32::Condition::NE) &&
				   m_code.PatchBranch(no_quirk_from_equal_operands, done_target, VitaA32::Condition::EQ);
		};

		const auto emit_lane = [this, rs, rt, subtract, load_word, store_signed_word_as_doubleword,
							   emit_divide_signed_64_by_u32_max, emit_pmaddw_lower_quirk]
			(unsigned source_word, unsigned dest_lane) {
			const size_t lo_source_offset = LO_OFFSET + source_word * sizeof(u32);
			const size_t hi_source_offset = HI_OFFSET + source_word * sizeof(u32);
			const size_t lo_dest_offset = LO_OFFSET + dest_lane * sizeof(u64);
			const size_t hi_dest_offset = HI_OFFSET + dest_lane * sizeof(u64);

			if (!load_word(rs, source_word, HOST_TMP0) ||
				!load_word(rt, source_word, HOST_TMP1) ||
				!m_code.EmitSmull(HOST_TMP2, HOST_TMP3, HOST_TMP0, HOST_TMP1) ||
				!m_code.EmitMovRegShiftImm(HOST_TMP4, HOST_TMP2, VitaA32::ShiftType::LSL, 0) ||
				!m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(lo_source_offset)) ||
				!(subtract ? m_code.EmitSubReg(HOST_TMP2, HOST_TMP0, HOST_TMP4) :
							  m_code.EmitAddReg(HOST_TMP2, HOST_TMP0, HOST_TMP4)) ||
				!store_signed_word_as_doubleword(HOST_TMP2, lo_dest_offset))
			{
				return false;
			}

			if (subtract)
			{
				if (!m_code.EmitMovImm8(HOST_TMP0, 0) ||
					!m_code.EmitSubReg(HOST_TMP4, HOST_TMP0, HOST_TMP4, true) ||
					!m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(hi_source_offset)) ||
					!m_code.EmitSbcReg(HOST_TMP3, HOST_TMP0, HOST_TMP3))
				{
					return false;
				}
			}
			else if (!m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(hi_source_offset)) ||
					 !m_code.EmitAddReg(HOST_TMP3, HOST_TMP3, HOST_TMP0) ||
					 (source_word == 0 && !emit_pmaddw_lower_quirk()))
			{
				return false;
			}

			return emit_divide_signed_64_by_u32_max() &&
				   store_signed_word_as_doubleword(HOST_TMP3, hi_dest_offset);
		};

		const auto copy_accumulator_to_rd = [this, rd](unsigned rd_word, size_t accumulator_offset) {
			const size_t rd_offset = GprOffset(rd) + rd_word * sizeof(u32);
			return m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(accumulator_offset)) &&
				   m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(rd_offset));
		};

		if (!emit_lane(0, 0) || !emit_lane(2, 1))
			return false;

		if (rd == 0)
			return true;

		return copy_accumulator_to_rd(0, LO_OFFSET) &&
			   copy_accumulator_to_rd(1, HI_OFFSET) &&
			   copy_accumulator_to_rd(2, LO_OFFSET + sizeof(u64)) &&
			   copy_accumulator_to_rd(3, HI_OFFSET + sizeof(u64));
	}

	bool BlockCompiler::EmitPackedHalfwordMultiplyAccumulate(u32 op, bool subtract)
	{
		const unsigned rd = RD(op);
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);

		const auto load_halfword = [this](unsigned guest_reg, unsigned lane, unsigned host_reg) {
			if (guest_reg == 0)
				return m_code.EmitMovImm8(host_reg, 0);

			const size_t source_offset = GprOffset(guest_reg) + (lane / 2) * sizeof(u32);
			if (!m_code.EmitLdrImm12(host_reg, HOST_CPU_REGS, static_cast<u16>(source_offset)))
				return false;

			if ((lane & 1u) != 0)
				return m_code.EmitMovRegShiftImm(host_reg, host_reg, VitaA32::ShiftType::ASR, 16);

			return m_code.EmitMovRegShiftImm(host_reg, host_reg, VitaA32::ShiftType::LSL, 16) &&
				   m_code.EmitMovRegShiftImm(host_reg, host_reg, VitaA32::ShiftType::ASR, 16);
		};

		const auto emit_lane = [this, rs, rt, subtract, load_halfword](unsigned lane) {
			const size_t accumulator_offset = PackedHalfwordAccumulatorOffset(lane);
			if (!load_halfword(rs, lane, HOST_TMP0) ||
				!load_halfword(rt, lane, HOST_TMP1) ||
				!m_code.EmitSmull(HOST_TMP2, HOST_TMP3, HOST_TMP0, HOST_TMP1) ||
				!m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(accumulator_offset)) ||
				!(subtract ? m_code.EmitSubReg(HOST_TMP2, HOST_TMP0, HOST_TMP2) :
							  m_code.EmitAddReg(HOST_TMP2, HOST_TMP0, HOST_TMP2)) ||
				!m_code.EmitStrImm12(HOST_TMP2, HOST_CPU_REGS, static_cast<u16>(accumulator_offset)))
			{
				return false;
			}

			return true;
		};

		const auto copy_accumulator_to_rd = [this, rd](unsigned rd_word, size_t accumulator_offset) {
			const size_t rd_offset = GprOffset(rd) + rd_word * sizeof(u32);
			return m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(accumulator_offset)) &&
				   m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(rd_offset));
		};

		if (!emit_lane(0) || !emit_lane(1) || !emit_lane(2) || !emit_lane(3) ||
			!emit_lane(4) || !emit_lane(5) || !emit_lane(6) || !emit_lane(7))
		{
			return false;
		}

		if (rd == 0)
			return true;

		return copy_accumulator_to_rd(0, PackedHalfwordAccumulatorOffset(0)) &&
			   copy_accumulator_to_rd(1, PackedHalfwordAccumulatorOffset(2)) &&
			   copy_accumulator_to_rd(2, PackedHalfwordAccumulatorOffset(4)) &&
			   copy_accumulator_to_rd(3, PackedHalfwordAccumulatorOffset(6));
	}

	bool BlockCompiler::EmitPackedHalfwordPairMultiply(u32 op, bool subtract)
	{
		const unsigned rd = RD(op);
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);

		const auto load_halfword = [this](unsigned guest_reg, unsigned lane, unsigned host_reg) {
			if (guest_reg == 0)
				return m_code.EmitMovImm8(host_reg, 0);

			const size_t source_offset = GprOffset(guest_reg) + (lane / 2) * sizeof(u32);
			if (!m_code.EmitLdrImm12(host_reg, HOST_CPU_REGS, static_cast<u16>(source_offset)))
				return false;

			if ((lane & 1u) != 0)
				return m_code.EmitMovRegShiftImm(host_reg, host_reg, VitaA32::ShiftType::ASR, 16);

			return m_code.EmitMovRegShiftImm(host_reg, host_reg, VitaA32::ShiftType::LSL, 16) &&
				   m_code.EmitMovRegShiftImm(host_reg, host_reg, VitaA32::ShiftType::ASR, 16);
		};

		const auto emit_product = [this, rs, rt, load_halfword](unsigned lane) {
			return load_halfword(rs, lane, HOST_TMP0) &&
				   load_halfword(rt, lane, HOST_TMP1) &&
				   m_code.EmitSmull(HOST_TMP2, HOST_TMP3, HOST_TMP0, HOST_TMP1);
		};

		const auto emit_pair = [this, subtract, emit_product](unsigned first_lane) {
			const unsigned second_lane = first_lane + 1;
			if (!emit_product(first_lane) ||
				!m_code.EmitMovRegShiftImm(HOST_TMP4, HOST_TMP2, VitaA32::ShiftType::LSL, 0) ||
				!emit_product(second_lane))
			{
				return false;
			}

			if (subtract)
			{
				if (!m_code.EmitMvnReg(HOST_TMP3, HOST_TMP2) ||
					!m_code.EmitSubReg(HOST_TMP2, HOST_TMP2, HOST_TMP4))
				{
					return false;
				}
			}
			else if (!m_code.EmitMovRegShiftImm(HOST_TMP3, HOST_TMP2, VitaA32::ShiftType::LSL, 0) ||
					 !m_code.EmitAddReg(HOST_TMP2, HOST_TMP2, HOST_TMP4))
			{
				return false;
			}

			return m_code.EmitStrImm12(HOST_TMP2, HOST_CPU_REGS,
					   static_cast<u16>(PackedHalfwordAccumulatorOffset(first_lane))) &&
				   m_code.EmitStrImm12(HOST_TMP3, HOST_CPU_REGS,
					   static_cast<u16>(PackedHalfwordAccumulatorOffset(second_lane)));
		};

		const auto copy_accumulator_to_rd = [this, rd](unsigned rd_word, size_t accumulator_offset) {
			const size_t rd_offset = GprOffset(rd) + rd_word * sizeof(u32);
			return m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(accumulator_offset)) &&
				   m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(rd_offset));
		};

		if (!emit_pair(0) || !emit_pair(2) || !emit_pair(4) || !emit_pair(6))
			return false;

		if (rd == 0)
			return true;

		return copy_accumulator_to_rd(0, PackedHalfwordAccumulatorOffset(0)) &&
			   copy_accumulator_to_rd(1, PackedHalfwordAccumulatorOffset(2)) &&
			   copy_accumulator_to_rd(2, PackedHalfwordAccumulatorOffset(4)) &&
			   copy_accumulator_to_rd(3, PackedHalfwordAccumulatorOffset(6));
	}

	bool BlockCompiler::EmitPackedHalfwordMultiply(u32 op)
	{
		const unsigned rd = RD(op);
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);

		const auto load_halfword = [this](unsigned guest_reg, unsigned lane, unsigned host_reg) {
			if (guest_reg == 0)
				return m_code.EmitMovImm8(host_reg, 0);

			const size_t source_offset = GprOffset(guest_reg) + (lane / 2) * sizeof(u32);
			if (!m_code.EmitLdrImm12(host_reg, HOST_CPU_REGS, static_cast<u16>(source_offset)))
				return false;

			if ((lane & 1u) != 0)
				return m_code.EmitMovRegShiftImm(host_reg, host_reg, VitaA32::ShiftType::ASR, 16);

			return m_code.EmitMovRegShiftImm(host_reg, host_reg, VitaA32::ShiftType::LSL, 16) &&
				   m_code.EmitMovRegShiftImm(host_reg, host_reg, VitaA32::ShiftType::ASR, 16);
		};

		const auto emit_lane = [this, rs, rt, load_halfword](unsigned lane) {
			if (!load_halfword(rs, lane, HOST_TMP0) ||
				!load_halfword(rt, lane, HOST_TMP1) ||
				!m_code.EmitSmull(HOST_TMP2, HOST_TMP3, HOST_TMP0, HOST_TMP1) ||
				!m_code.EmitStrImm12(HOST_TMP2, HOST_CPU_REGS,
					static_cast<u16>(PackedHalfwordAccumulatorOffset(lane))))
			{
				return false;
			}

			return true;
		};

		const auto copy_accumulator_to_rd = [this, rd](unsigned rd_word, size_t accumulator_offset) {
			const size_t rd_offset = GprOffset(rd) + rd_word * sizeof(u32);
			return m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(accumulator_offset)) &&
				   m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(rd_offset));
		};

		if (!emit_lane(0) || !emit_lane(1) || !emit_lane(2) || !emit_lane(3) ||
			!emit_lane(4) || !emit_lane(5) || !emit_lane(6) || !emit_lane(7))
		{
			return false;
		}

		if (rd == 0)
			return true;

		return copy_accumulator_to_rd(0, PackedHalfwordAccumulatorOffset(0)) &&
			   copy_accumulator_to_rd(1, PackedHalfwordAccumulatorOffset(2)) &&
			   copy_accumulator_to_rd(2, PackedHalfwordAccumulatorOffset(4)) &&
			   copy_accumulator_to_rd(3, PackedHalfwordAccumulatorOffset(6));
	}

	bool BlockCompiler::EmitPackedUnsignedWordMultiplyAdd(u32 op)
	{
		const unsigned rd = RD(op);
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);

		const auto load_word = [this](unsigned guest_reg, unsigned word, unsigned host_reg) {
			if (guest_reg == 0)
				return m_code.EmitMovImm8(host_reg, 0);

			return m_code.EmitLdrImm12(host_reg, HOST_CPU_REGS,
				static_cast<u16>(GprOffset(guest_reg) + word * sizeof(u32)));
		};

		const auto emit_lane = [this, rd, rs, rt, load_word](unsigned source_word, unsigned dest_lane) {
			const size_t rd_offset = GprOffset(rd) + dest_lane * sizeof(u64);
			const size_t lo_offset = LO_OFFSET + dest_lane * sizeof(u64);
			const size_t hi_offset = HI_OFFSET + dest_lane * sizeof(u64);

			if (!load_word(rs, source_word, HOST_TMP0) ||
				!load_word(rt, source_word, HOST_TMP1) ||
				!m_code.EmitUmull(HOST_TMP2, HOST_TMP3, HOST_TMP0, HOST_TMP1) ||
				!m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(lo_offset)) ||
				!m_code.EmitLdrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(hi_offset)) ||
				!m_code.EmitAddReg(HOST_TMP2, HOST_TMP2, HOST_TMP0, true) ||
				!m_code.EmitAdcReg(HOST_TMP3, HOST_TMP3, HOST_TMP1))
			{
				return false;
			}

			if (rd != 0 &&
				(!m_code.EmitStrImm12(HOST_TMP2, HOST_CPU_REGS, static_cast<u16>(rd_offset)) ||
					!m_code.EmitStrImm12(HOST_TMP3, HOST_CPU_REGS, static_cast<u16>(rd_offset + sizeof(u32)))))
			{
				return false;
			}

			if (!m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_TMP2, VitaA32::ShiftType::ASR, 31) ||
				!m_code.EmitStrImm12(HOST_TMP2, HOST_CPU_REGS, static_cast<u16>(lo_offset)) ||
				!m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(lo_offset + sizeof(u32))) ||
				!m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_TMP3, VitaA32::ShiftType::ASR, 31) ||
				!m_code.EmitStrImm12(HOST_TMP3, HOST_CPU_REGS, static_cast<u16>(hi_offset)) ||
				!m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(hi_offset + sizeof(u32))))
			{
				return false;
			}

			return true;
		};

		return emit_lane(0, 0) && emit_lane(2, 1);
	}

	bool BlockCompiler::EmitMmiVectorOp(u32 op, MmiVectorOp operation)
	{
		const unsigned rd = RD(op);
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		if (rd == 0)
			return true;

		constexpr unsigned NEON_RS = 0;
		constexpr unsigned NEON_RT = 1;
		constexpr unsigned NEON_RD = 2;

		const auto emit_gpr_address = [this](unsigned guest_reg, unsigned host_reg) {
			const size_t offset = GprOffset(guest_reg);
			if (offset <= 255)
				return m_code.EmitAddImm8(host_reg, HOST_CPU_REGS, static_cast<u8>(offset));

			return m_code.EmitMovImm32(host_reg, static_cast<u32>(offset)) &&
				   m_code.EmitAddReg(host_reg, HOST_CPU_REGS, host_reg);
		};

		if (rs == 0)
		{
			if (!m_code.EmitVeorQ(NEON_RS, NEON_RS, NEON_RS))
				return false;
		}
		else if (!emit_gpr_address(rs, HOST_TMP0) ||
				 !m_code.EmitVld1Q32(NEON_RS, HOST_TMP0))
		{
			return false;
		}

		if (rt == 0)
		{
			if (!m_code.EmitVeorQ(NEON_RT, NEON_RT, NEON_RT))
				return false;
		}
		else if (!emit_gpr_address(rt, HOST_TMP1) ||
				 !m_code.EmitVld1Q32(NEON_RT, HOST_TMP1))
		{
			return false;
		}

		bool vector_op_ok = false;
		switch (operation)
		{
			case MmiVectorOp::AddWord:
				vector_op_ok = m_code.EmitVaddI32Q(NEON_RD, NEON_RS, NEON_RT);
				break;
			case MmiVectorOp::SubtractWord:
				vector_op_ok = m_code.EmitVsubI32Q(NEON_RD, NEON_RS, NEON_RT);
				break;
			case MmiVectorOp::CompareGreaterSignedWord:
				vector_op_ok = m_code.EmitVcgtS32Q(NEON_RD, NEON_RS, NEON_RT);
				break;
			case MmiVectorOp::MaxSignedWord:
				vector_op_ok = m_code.EmitVmaxS32Q(NEON_RD, NEON_RS, NEON_RT);
				break;
			case MmiVectorOp::AddHalfword:
				vector_op_ok = m_code.EmitVaddI16Q(NEON_RD, NEON_RS, NEON_RT);
				break;
			case MmiVectorOp::SubtractHalfword:
				vector_op_ok = m_code.EmitVsubI16Q(NEON_RD, NEON_RS, NEON_RT);
				break;
			case MmiVectorOp::CompareGreaterSignedHalfword:
				vector_op_ok = m_code.EmitVcgtS16Q(NEON_RD, NEON_RS, NEON_RT);
				break;
			case MmiVectorOp::MaxSignedHalfword:
				vector_op_ok = m_code.EmitVmaxS16Q(NEON_RD, NEON_RS, NEON_RT);
				break;
			case MmiVectorOp::AddByte:
				vector_op_ok = m_code.EmitVaddI8Q(NEON_RD, NEON_RS, NEON_RT);
				break;
			case MmiVectorOp::SubtractByte:
				vector_op_ok = m_code.EmitVsubI8Q(NEON_RD, NEON_RS, NEON_RT);
				break;
			case MmiVectorOp::CompareGreaterSignedByte:
				vector_op_ok = m_code.EmitVcgtS8Q(NEON_RD, NEON_RS, NEON_RT);
				break;
			case MmiVectorOp::CompareEqualWord:
				vector_op_ok = m_code.EmitVceqI32Q(NEON_RD, NEON_RS, NEON_RT);
				break;
			case MmiVectorOp::MinSignedWord:
				vector_op_ok = m_code.EmitVminS32Q(NEON_RD, NEON_RS, NEON_RT);
				break;
			case MmiVectorOp::CompareEqualHalfword:
				vector_op_ok = m_code.EmitVceqI16Q(NEON_RD, NEON_RS, NEON_RT);
				break;
			case MmiVectorOp::MinSignedHalfword:
				vector_op_ok = m_code.EmitVminS16Q(NEON_RD, NEON_RS, NEON_RT);
				break;
			case MmiVectorOp::CompareEqualByte:
				vector_op_ok = m_code.EmitVceqI8Q(NEON_RD, NEON_RS, NEON_RT);
				break;
			case MmiVectorOp::SaturatingAddSignedWord:
				vector_op_ok = m_code.EmitVqaddS32Q(NEON_RD, NEON_RS, NEON_RT);
				break;
			case MmiVectorOp::SaturatingSubtractSignedWord:
				vector_op_ok = m_code.EmitVqsubS32Q(NEON_RD, NEON_RS, NEON_RT);
				break;
			case MmiVectorOp::SaturatingAddSignedHalfword:
				vector_op_ok = m_code.EmitVqaddS16Q(NEON_RD, NEON_RS, NEON_RT);
				break;
			case MmiVectorOp::SaturatingSubtractSignedHalfword:
				vector_op_ok = m_code.EmitVqsubS16Q(NEON_RD, NEON_RS, NEON_RT);
				break;
			case MmiVectorOp::SaturatingAddSignedByte:
				vector_op_ok = m_code.EmitVqaddS8Q(NEON_RD, NEON_RS, NEON_RT);
				break;
			case MmiVectorOp::SaturatingSubtractSignedByte:
				vector_op_ok = m_code.EmitVqsubS8Q(NEON_RD, NEON_RS, NEON_RT);
				break;
			case MmiVectorOp::SaturatingAddUnsignedWord:
				vector_op_ok = m_code.EmitVqaddU32Q(NEON_RD, NEON_RS, NEON_RT);
				break;
			case MmiVectorOp::SaturatingSubtractUnsignedWord:
				vector_op_ok = m_code.EmitVqsubU32Q(NEON_RD, NEON_RS, NEON_RT);
				break;
			case MmiVectorOp::SaturatingAddUnsignedHalfword:
				vector_op_ok = m_code.EmitVqaddU16Q(NEON_RD, NEON_RS, NEON_RT);
				break;
			case MmiVectorOp::SaturatingSubtractUnsignedHalfword:
				vector_op_ok = m_code.EmitVqsubU16Q(NEON_RD, NEON_RS, NEON_RT);
				break;
			case MmiVectorOp::SaturatingAddUnsignedByte:
				vector_op_ok = m_code.EmitVqaddU8Q(NEON_RD, NEON_RS, NEON_RT);
				break;
			case MmiVectorOp::SaturatingSubtractUnsignedByte:
				vector_op_ok = m_code.EmitVqsubU8Q(NEON_RD, NEON_RS, NEON_RT);
				break;
			case MmiVectorOp::BitwiseAnd:
				vector_op_ok = m_code.EmitVandQ(NEON_RD, NEON_RS, NEON_RT);
				break;
			case MmiVectorOp::BitwiseXor:
				vector_op_ok = m_code.EmitVeorQ(NEON_RD, NEON_RS, NEON_RT);
				break;
			case MmiVectorOp::BitwiseOr:
				vector_op_ok = m_code.EmitVorrQ(NEON_RD, NEON_RS, NEON_RT);
				break;
			case MmiVectorOp::BitwiseNor:
				vector_op_ok = m_code.EmitVorrQ(NEON_RD, NEON_RS, NEON_RT) &&
							   m_code.EmitVmvnQ(NEON_RD, NEON_RD);
				break;
		}

		if (!vector_op_ok ||
			!emit_gpr_address(rd, HOST_TMP2) ||
			!m_code.EmitVst1Q32(NEON_RD, HOST_TMP2))
		{
			return false;
		}

		return true;
	}

	bool BlockCompiler::EmitMmiUnaryRtVectorOp(u32 op, MmiUnaryVectorOp operation)
	{
		const unsigned rd = RD(op);
		const unsigned rt = RT(op);
		if (rd == 0)
			return true;

		constexpr unsigned NEON_RT = 0;

		const auto emit_gpr_address = [this](unsigned guest_reg, unsigned host_reg) {
			const size_t offset = GprOffset(guest_reg);
			if (offset <= 255)
				return m_code.EmitAddImm8(host_reg, HOST_CPU_REGS, static_cast<u8>(offset));

			return m_code.EmitMovImm32(host_reg, static_cast<u32>(offset)) &&
				   m_code.EmitAddReg(host_reg, HOST_CPU_REGS, host_reg);
		};

		if (rt == 0)
		{
			if (!m_code.EmitVeorQ(NEON_RT, NEON_RT, NEON_RT))
				return false;
		}
		else if (!emit_gpr_address(rt, HOST_TMP0) ||
				 !m_code.EmitVld1Q32(NEON_RT, HOST_TMP0))
		{
			return false;
		}

		bool vector_op_ok = false;
		switch (operation)
		{
			case MmiUnaryVectorOp::AbsoluteSignedWord:
				vector_op_ok = m_code.EmitVqabsS32Q(NEON_RT, NEON_RT);
				break;
			case MmiUnaryVectorOp::AbsoluteSignedHalfword:
				vector_op_ok = m_code.EmitVqabsS16Q(NEON_RT, NEON_RT);
				break;
		}

		if (!vector_op_ok ||
			!emit_gpr_address(rd, HOST_TMP2) ||
			!m_code.EmitVst1Q32(NEON_RT, HOST_TMP2))
		{
			return false;
		}

		return true;
	}

	bool BlockCompiler::EmitMmiImmediateShiftOp(u32 op, MmiImmediateShiftOp operation)
	{
		const unsigned rd = RD(op);
		const unsigned rt = RT(op);
		if (rd == 0)
			return true;

		constexpr unsigned NEON_RT = 0;

		const auto emit_gpr_address = [this](unsigned guest_reg, unsigned host_reg) {
			const size_t offset = GprOffset(guest_reg);
			if (offset <= 255)
				return m_code.EmitAddImm8(host_reg, HOST_CPU_REGS, static_cast<u8>(offset));

			return m_code.EmitMovImm32(host_reg, static_cast<u32>(offset)) &&
				   m_code.EmitAddReg(host_reg, HOST_CPU_REGS, host_reg);
		};

		if (rt == 0)
		{
			if (!m_code.EmitVeorQ(NEON_RT, NEON_RT, NEON_RT))
				return false;
		}
		else if (!emit_gpr_address(rt, HOST_TMP0) ||
				 !m_code.EmitVld1Q32(NEON_RT, HOST_TMP0))
		{
			return false;
		}

		const u8 amount = static_cast<u8>((operation == MmiImmediateShiftOp::ShiftLeftHalfword ||
			operation == MmiImmediateShiftOp::ShiftRightLogicalHalfword ||
			operation == MmiImmediateShiftOp::ShiftRightArithmeticHalfword) ?
			(SA(op) & 0x0f) : SA(op));

		bool vector_op_ok = true;
		if (amount != 0)
		{
			switch (operation)
			{
				case MmiImmediateShiftOp::ShiftLeftHalfword:
					vector_op_ok = m_code.EmitVshlI16Q(NEON_RT, NEON_RT, amount);
					break;
				case MmiImmediateShiftOp::ShiftRightLogicalHalfword:
					vector_op_ok = m_code.EmitVshrU16Q(NEON_RT, NEON_RT, amount);
					break;
				case MmiImmediateShiftOp::ShiftRightArithmeticHalfword:
					vector_op_ok = m_code.EmitVshrS16Q(NEON_RT, NEON_RT, amount);
					break;
				case MmiImmediateShiftOp::ShiftLeftWord:
					vector_op_ok = m_code.EmitVshlI32Q(NEON_RT, NEON_RT, amount);
					break;
				case MmiImmediateShiftOp::ShiftRightLogicalWord:
					vector_op_ok = m_code.EmitVshrU32Q(NEON_RT, NEON_RT, amount);
					break;
				case MmiImmediateShiftOp::ShiftRightArithmeticWord:
					vector_op_ok = m_code.EmitVshrS32Q(NEON_RT, NEON_RT, amount);
					break;
			}
		}

		if (!vector_op_ok ||
			!emit_gpr_address(rd, HOST_TMP2) ||
			!m_code.EmitVst1Q32(NEON_RT, HOST_TMP2))
		{
			return false;
		}

		return true;
	}

	bool BlockCompiler::EmitMmiVariableWordShiftOp(u32 op, MmiVariableWordShiftOp operation)
	{
		const unsigned rd = RD(op);
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		if (rd == 0)
			return true;

		const auto emit_load_gpr_word = [this](unsigned guest_reg, unsigned word, unsigned host_reg) {
			if (guest_reg == 0)
				return m_code.EmitMovImm8(host_reg, 0);

			return m_code.EmitLdrImm12(host_reg, HOST_CPU_REGS,
				static_cast<u16>(GprOffset(guest_reg) + word * sizeof(u32)));
		};

		const auto emit_store_rd_word = [this, rd](unsigned word, unsigned host_reg) {
			return m_code.EmitStrImm12(host_reg, HOST_CPU_REGS,
				static_cast<u16>(GprOffset(rd) + word * sizeof(u32)));
		};

		const auto emit_lane = [&](unsigned source_word, unsigned result_word, unsigned value_reg,
			unsigned amount_reg, unsigned sign_reg) {
			VitaA32::ShiftType shift = VitaA32::ShiftType::LSL;
			switch (operation)
			{
				case MmiVariableWordShiftOp::ShiftLeftLogical:
					shift = VitaA32::ShiftType::LSL;
					break;
				case MmiVariableWordShiftOp::ShiftRightLogical:
					shift = VitaA32::ShiftType::LSR;
					break;
				case MmiVariableWordShiftOp::ShiftRightArithmetic:
					shift = VitaA32::ShiftType::ASR;
					break;
			}

			return emit_load_gpr_word(rt, source_word, value_reg) &&
				   emit_load_gpr_word(rs, source_word, amount_reg) &&
				   m_code.EmitAndImm8(amount_reg, amount_reg, 0x1f) &&
				   m_code.EmitMovRegShiftReg(value_reg, value_reg, shift, amount_reg) &&
				   m_code.EmitMovRegShiftImm(sign_reg, value_reg, VitaA32::ShiftType::ASR, 31) &&
				   emit_store_rd_word(result_word, value_reg) &&
				   emit_store_rd_word(result_word + 1, sign_reg);
		};

		return emit_lane(0, 0, HOST_TMP0, HOST_TMP2, HOST_TMP1) &&
			   emit_lane(2, 2, HOST_TMP3, HOST_TMP2, HOST_TMP4);
	}

	bool BlockCompiler::EmitMmiWordShuffleOp(u32 op, MmiWordShuffleOp operation)
	{
		const unsigned rd = RD(op);
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		if (rd == 0)
			return true;

		const auto emit_load_gpr_word = [this](unsigned guest_reg, unsigned word, unsigned host_reg) {
			if (guest_reg == 0)
				return m_code.EmitMovImm8(host_reg, 0);

			return m_code.EmitLdrImm12(host_reg, HOST_CPU_REGS,
				static_cast<u16>(GprOffset(guest_reg) + word * sizeof(u32)));
		};

		const auto emit_store_rd_word = [this, rd](unsigned word, unsigned host_reg) {
			return m_code.EmitStrImm12(host_reg, HOST_CPU_REGS,
				static_cast<u16>(GprOffset(rd) + word * sizeof(u32)));
		};

		switch (operation)
		{
			case MmiWordShuffleOp::Pcpyld:
				return emit_load_gpr_word(rt, 0, HOST_TMP0) &&
					   emit_load_gpr_word(rt, 1, HOST_TMP1) &&
					   emit_load_gpr_word(rs, 0, HOST_TMP2) &&
					   emit_load_gpr_word(rs, 1, HOST_TMP3) &&
					   emit_store_rd_word(0, HOST_TMP0) &&
					   emit_store_rd_word(1, HOST_TMP1) &&
					   emit_store_rd_word(2, HOST_TMP2) &&
					   emit_store_rd_word(3, HOST_TMP3);
			case MmiWordShuffleOp::Pcpyud:
				return emit_load_gpr_word(rs, 2, HOST_TMP0) &&
					   emit_load_gpr_word(rs, 3, HOST_TMP1) &&
					   emit_load_gpr_word(rt, 2, HOST_TMP2) &&
					   emit_load_gpr_word(rt, 3, HOST_TMP3) &&
					   emit_store_rd_word(0, HOST_TMP0) &&
					   emit_store_rd_word(1, HOST_TMP1) &&
					   emit_store_rd_word(2, HOST_TMP2) &&
					   emit_store_rd_word(3, HOST_TMP3);
			case MmiWordShuffleOp::Pexew:
				return emit_load_gpr_word(rt, 0, HOST_TMP0) &&
					   emit_load_gpr_word(rt, 1, HOST_TMP1) &&
					   emit_load_gpr_word(rt, 2, HOST_TMP2) &&
					   emit_load_gpr_word(rt, 3, HOST_TMP3) &&
					   emit_store_rd_word(0, HOST_TMP2) &&
					   emit_store_rd_word(1, HOST_TMP1) &&
					   emit_store_rd_word(2, HOST_TMP0) &&
					   emit_store_rd_word(3, HOST_TMP3);
			case MmiWordShuffleOp::Prot3w:
				return emit_load_gpr_word(rt, 0, HOST_TMP0) &&
					   emit_load_gpr_word(rt, 1, HOST_TMP1) &&
					   emit_load_gpr_word(rt, 2, HOST_TMP2) &&
					   emit_load_gpr_word(rt, 3, HOST_TMP3) &&
					   emit_store_rd_word(0, HOST_TMP1) &&
					   emit_store_rd_word(1, HOST_TMP2) &&
					   emit_store_rd_word(2, HOST_TMP0) &&
					   emit_store_rd_word(3, HOST_TMP3);
			case MmiWordShuffleOp::Pexcw:
				return emit_load_gpr_word(rt, 0, HOST_TMP0) &&
					   emit_load_gpr_word(rt, 1, HOST_TMP1) &&
					   emit_load_gpr_word(rt, 2, HOST_TMP2) &&
					   emit_load_gpr_word(rt, 3, HOST_TMP3) &&
					   emit_store_rd_word(0, HOST_TMP0) &&
					   emit_store_rd_word(1, HOST_TMP2) &&
					   emit_store_rd_word(2, HOST_TMP1) &&
					   emit_store_rd_word(3, HOST_TMP3);
		}

		return false;
	}

	bool BlockCompiler::EmitMmiFiveBitOp(u32 op, MmiFiveBitOp operation)
	{
		const unsigned rd = RD(op);
		const unsigned rt = RT(op);
		if (rd == 0)
			return true;

		const auto emit_load_gpr_word = [this](unsigned guest_reg, unsigned word, unsigned host_reg) {
			if (guest_reg == 0)
				return m_code.EmitMovImm8(host_reg, 0);

			return m_code.EmitLdrImm12(host_reg, HOST_CPU_REGS,
				static_cast<u16>(GprOffset(guest_reg) + word * sizeof(u32)));
		};

		const auto emit_store_rd_word = [this, rd](unsigned word, unsigned host_reg) {
			return m_code.EmitStrImm12(host_reg, HOST_CPU_REGS,
				static_cast<u16>(GprOffset(rd) + word * sizeof(u32)));
		};

		const auto emit_expand_lane = [&](unsigned word, unsigned value_reg, unsigned result_reg,
			unsigned tmp_reg) {
			return emit_load_gpr_word(rt, word, value_reg) &&
				   m_code.EmitAndImm8(result_reg, value_reg, 0x1f) &&
				   m_code.EmitMovRegShiftImm(result_reg, result_reg, VitaA32::ShiftType::LSL, 3) &&
				   m_code.EmitMovRegShiftImm(tmp_reg, value_reg, VitaA32::ShiftType::LSR, 5) &&
				   m_code.EmitAndImm8(tmp_reg, tmp_reg, 0x1f) &&
				   m_code.EmitMovRegShiftImm(tmp_reg, tmp_reg, VitaA32::ShiftType::LSL, 11) &&
				   m_code.EmitOrrReg(result_reg, result_reg, tmp_reg) &&
				   m_code.EmitMovRegShiftImm(tmp_reg, value_reg, VitaA32::ShiftType::LSR, 10) &&
				   m_code.EmitAndImm8(tmp_reg, tmp_reg, 0x1f) &&
				   m_code.EmitMovRegShiftImm(tmp_reg, tmp_reg, VitaA32::ShiftType::LSL, 19) &&
				   m_code.EmitOrrReg(result_reg, result_reg, tmp_reg) &&
				   m_code.EmitMovRegShiftImm(tmp_reg, value_reg, VitaA32::ShiftType::LSR, 15) &&
				   m_code.EmitAndImm8(tmp_reg, tmp_reg, 0x01) &&
				   m_code.EmitMovRegShiftImm(tmp_reg, tmp_reg, VitaA32::ShiftType::LSL, 31) &&
				   m_code.EmitOrrReg(result_reg, result_reg, tmp_reg) &&
				   emit_store_rd_word(word, result_reg);
		};

		const auto emit_pack_lane = [&](unsigned word, unsigned value_reg, unsigned result_reg,
			unsigned tmp_reg) {
			return emit_load_gpr_word(rt, word, value_reg) &&
				   m_code.EmitMovRegShiftImm(result_reg, value_reg, VitaA32::ShiftType::LSR, 3) &&
				   m_code.EmitAndImm8(result_reg, result_reg, 0x1f) &&
				   m_code.EmitMovRegShiftImm(tmp_reg, value_reg, VitaA32::ShiftType::LSR, 11) &&
				   m_code.EmitAndImm8(tmp_reg, tmp_reg, 0x1f) &&
				   m_code.EmitMovRegShiftImm(tmp_reg, tmp_reg, VitaA32::ShiftType::LSL, 5) &&
				   m_code.EmitOrrReg(result_reg, result_reg, tmp_reg) &&
				   m_code.EmitMovRegShiftImm(tmp_reg, value_reg, VitaA32::ShiftType::LSR, 19) &&
				   m_code.EmitAndImm8(tmp_reg, tmp_reg, 0x1f) &&
				   m_code.EmitMovRegShiftImm(tmp_reg, tmp_reg, VitaA32::ShiftType::LSL, 10) &&
				   m_code.EmitOrrReg(result_reg, result_reg, tmp_reg) &&
				   m_code.EmitMovRegShiftImm(tmp_reg, value_reg, VitaA32::ShiftType::LSR, 31) &&
				   m_code.EmitMovRegShiftImm(tmp_reg, tmp_reg, VitaA32::ShiftType::LSL, 15) &&
				   m_code.EmitOrrReg(result_reg, result_reg, tmp_reg) &&
				   emit_store_rd_word(word, result_reg);
		};

		for (unsigned word = 0; word < 4; word++)
		{
			if (operation == MmiFiveBitOp::Expand)
			{
				if (!emit_expand_lane(word, HOST_TMP0, HOST_TMP1, HOST_TMP2))
					return false;
			}
			else if (!emit_pack_lane(word, HOST_TMP0, HOST_TMP1, HOST_TMP2))
			{
				return false;
			}
		}

		return true;
	}

	bool BlockCompiler::EmitMmiHalfwordShuffleOp(u32 op, MmiHalfwordShuffleOp operation)
	{
		const unsigned rd = RD(op);
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		if (rd == 0)
			return true;

		const auto emit_load_gpr_word = [this](unsigned guest_reg, unsigned word, unsigned host_reg) {
			if (guest_reg == 0)
				return m_code.EmitMovImm8(host_reg, 0);

			return m_code.EmitLdrImm12(host_reg, HOST_CPU_REGS,
				static_cast<u16>(GprOffset(guest_reg) + word * sizeof(u32)));
		};

		const auto emit_store_rd_word = [this, rd](unsigned word, unsigned host_reg) {
			return m_code.EmitStrImm12(host_reg, HOST_CPU_REGS,
				static_cast<u16>(GprOffset(rd) + word * sizeof(u32)));
		};

		const auto emit_pack = [this](unsigned low_reg, bool low_high_half, unsigned high_reg,
			bool high_high_half, unsigned out_reg) {
			if (low_high_half &&
				!m_code.EmitMovRegShiftImm(out_reg, low_reg, VitaA32::ShiftType::LSR, 16))
			{
				return false;
			}

			const unsigned rn = low_high_half ? out_reg : low_reg;
			const u8 high_shift = high_high_half ? 0 : 16;
			return m_code.EmitPkhbt(out_reg, rn, high_reg, high_shift);
		};

		const auto emit_pin_word = [&](unsigned out_word, unsigned rt_word, unsigned rs_word) {
			return emit_load_gpr_word(rt, rt_word, HOST_TMP0) &&
				   emit_load_gpr_word(rs, rs_word, HOST_TMP1) &&
				   emit_pack(HOST_TMP0, false, HOST_TMP1, false, HOST_TMP4) &&
				   emit_store_rd_word(out_word, HOST_TMP4);
		};

		switch (operation)
		{
			case MmiHalfwordShuffleOp::Pinth:
				if (!emit_load_gpr_word(rt, 0, HOST_TMP0) ||
					!emit_load_gpr_word(rs, 2, HOST_TMP1) ||
					!emit_load_gpr_word(rt, 1, HOST_TMP2) ||
					!emit_load_gpr_word(rs, 3, HOST_TMP3))
				{
					return false;
				}
				return emit_pack(HOST_TMP0, false, HOST_TMP1, false, HOST_TMP4) &&
					   emit_store_rd_word(0, HOST_TMP4) &&
					   emit_pack(HOST_TMP0, true, HOST_TMP1, true, HOST_TMP4) &&
					   emit_store_rd_word(1, HOST_TMP4) &&
					   emit_pack(HOST_TMP2, false, HOST_TMP3, false, HOST_TMP4) &&
					   emit_store_rd_word(2, HOST_TMP4) &&
					   emit_pack(HOST_TMP2, true, HOST_TMP3, true, HOST_TMP4) &&
					   emit_store_rd_word(3, HOST_TMP4);
			case MmiHalfwordShuffleOp::Pinteh:
				return emit_pin_word(0, 0, 0) &&
					   emit_pin_word(1, 1, 1) &&
					   emit_pin_word(2, 2, 2) &&
					   emit_pin_word(3, 3, 3);
			case MmiHalfwordShuffleOp::Pexeh:
				return emit_load_gpr_word(rt, 0, HOST_TMP0) &&
					   emit_load_gpr_word(rt, 1, HOST_TMP1) &&
					   emit_pack(HOST_TMP1, false, HOST_TMP0, true, HOST_TMP4) &&
					   emit_store_rd_word(0, HOST_TMP4) &&
					   emit_pack(HOST_TMP0, false, HOST_TMP1, true, HOST_TMP4) &&
					   emit_store_rd_word(1, HOST_TMP4) &&
					   emit_load_gpr_word(rt, 2, HOST_TMP0) &&
					   emit_load_gpr_word(rt, 3, HOST_TMP1) &&
					   emit_pack(HOST_TMP1, false, HOST_TMP0, true, HOST_TMP4) &&
					   emit_store_rd_word(2, HOST_TMP4) &&
					   emit_pack(HOST_TMP0, false, HOST_TMP1, true, HOST_TMP4) &&
					   emit_store_rd_word(3, HOST_TMP4);
			case MmiHalfwordShuffleOp::Prevh:
				return emit_load_gpr_word(rt, 0, HOST_TMP0) &&
					   emit_load_gpr_word(rt, 1, HOST_TMP1) &&
					   emit_pack(HOST_TMP1, true, HOST_TMP1, false, HOST_TMP4) &&
					   emit_store_rd_word(0, HOST_TMP4) &&
					   emit_pack(HOST_TMP0, true, HOST_TMP0, false, HOST_TMP4) &&
					   emit_store_rd_word(1, HOST_TMP4) &&
					   emit_load_gpr_word(rt, 2, HOST_TMP0) &&
					   emit_load_gpr_word(rt, 3, HOST_TMP1) &&
					   emit_pack(HOST_TMP1, true, HOST_TMP1, false, HOST_TMP4) &&
					   emit_store_rd_word(2, HOST_TMP4) &&
					   emit_pack(HOST_TMP0, true, HOST_TMP0, false, HOST_TMP4) &&
					   emit_store_rd_word(3, HOST_TMP4);
			case MmiHalfwordShuffleOp::Pexch:
				return emit_load_gpr_word(rt, 0, HOST_TMP0) &&
					   emit_load_gpr_word(rt, 1, HOST_TMP1) &&
					   emit_pack(HOST_TMP0, false, HOST_TMP1, false, HOST_TMP4) &&
					   emit_store_rd_word(0, HOST_TMP4) &&
					   emit_pack(HOST_TMP0, true, HOST_TMP1, true, HOST_TMP4) &&
					   emit_store_rd_word(1, HOST_TMP4) &&
					   emit_load_gpr_word(rt, 2, HOST_TMP0) &&
					   emit_load_gpr_word(rt, 3, HOST_TMP1) &&
					   emit_pack(HOST_TMP0, false, HOST_TMP1, false, HOST_TMP4) &&
					   emit_store_rd_word(2, HOST_TMP4) &&
					   emit_pack(HOST_TMP0, true, HOST_TMP1, true, HOST_TMP4) &&
					   emit_store_rd_word(3, HOST_TMP4);
			case MmiHalfwordShuffleOp::Pcpyh:
				return emit_load_gpr_word(rt, 0, HOST_TMP0) &&
					   emit_pack(HOST_TMP0, false, HOST_TMP0, false, HOST_TMP4) &&
					   emit_store_rd_word(0, HOST_TMP4) &&
					   emit_store_rd_word(1, HOST_TMP4) &&
					   emit_load_gpr_word(rt, 2, HOST_TMP0) &&
					   emit_pack(HOST_TMP0, false, HOST_TMP0, false, HOST_TMP4) &&
					   emit_store_rd_word(2, HOST_TMP4) &&
					   emit_store_rd_word(3, HOST_TMP4);
		}

		return false;
	}

	bool BlockCompiler::EmitMmiInterleaveOp(u32 op, MmiUpperInterleaveOp operation, bool upper_half)
	{
		const unsigned rd = RD(op);
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		if (rd == 0)
			return true;

		constexpr unsigned NEON_RS = 0;
		constexpr unsigned NEON_RT = 1;

		const auto emit_gpr_address = [this](unsigned guest_reg, unsigned host_reg) {
			const size_t offset = GprOffset(guest_reg);
			if (offset <= 255)
				return m_code.EmitAddImm8(host_reg, HOST_CPU_REGS, static_cast<u8>(offset));

			return m_code.EmitMovImm32(host_reg, static_cast<u32>(offset)) &&
				   m_code.EmitAddReg(host_reg, HOST_CPU_REGS, host_reg);
		};

		if (rs == 0)
		{
			if (!m_code.EmitVeorQ(NEON_RS, NEON_RS, NEON_RS))
				return false;
		}
		else if (!emit_gpr_address(rs, HOST_TMP0) ||
				 !m_code.EmitVld1Q32(NEON_RS, HOST_TMP0))
		{
			return false;
		}

		if (rt == 0)
		{
			if (!m_code.EmitVeorQ(NEON_RT, NEON_RT, NEON_RT))
				return false;
		}
		else if (!emit_gpr_address(rt, HOST_TMP1) ||
				 !m_code.EmitVld1Q32(NEON_RT, HOST_TMP1))
		{
			return false;
		}

		bool vector_op_ok = false;
		switch (operation)
		{
			case MmiUpperInterleaveOp::Word:
				vector_op_ok = m_code.EmitVzipI32Q(NEON_RT, NEON_RS);
				break;
			case MmiUpperInterleaveOp::Halfword:
				vector_op_ok = m_code.EmitVzipI16Q(NEON_RT, NEON_RS);
				break;
			case MmiUpperInterleaveOp::Byte:
				vector_op_ok = m_code.EmitVzipI8Q(NEON_RT, NEON_RS);
				break;
		}

		// vzip RT, RS leaves the lower interleave in RT and the upper
		// interleave in RS. PCSX2's PEXTL* and PEXTU* forms use those halves
		// directly.
		const unsigned result = upper_half ? NEON_RS : NEON_RT;
		if (!vector_op_ok ||
			!emit_gpr_address(rd, HOST_TMP2) ||
			!m_code.EmitVst1Q32(result, HOST_TMP2))
		{
			return false;
		}

		return true;
	}

	bool BlockCompiler::EmitMmiUpperInterleaveOp(u32 op, MmiUpperInterleaveOp operation)
	{
		return EmitMmiInterleaveOp(op, operation, true);
	}

	bool BlockCompiler::EmitMmiPackEvenOp(u32 op, MmiUpperInterleaveOp operation)
	{
		const unsigned rd = RD(op);
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		if (rd == 0)
			return true;

		constexpr unsigned NEON_RS = 0;
		constexpr unsigned NEON_RT = 1;

		const auto emit_gpr_address = [this](unsigned guest_reg, unsigned host_reg) {
			const size_t offset = GprOffset(guest_reg);
			if (offset <= 255)
				return m_code.EmitAddImm8(host_reg, HOST_CPU_REGS, static_cast<u8>(offset));

			return m_code.EmitMovImm32(host_reg, static_cast<u32>(offset)) &&
				   m_code.EmitAddReg(host_reg, HOST_CPU_REGS, host_reg);
		};

		if (rs == 0)
		{
			if (!m_code.EmitVeorQ(NEON_RS, NEON_RS, NEON_RS))
				return false;
		}
		else if (!emit_gpr_address(rs, HOST_TMP0) ||
				 !m_code.EmitVld1Q32(NEON_RS, HOST_TMP0))
		{
			return false;
		}

		if (rt == 0)
		{
			if (!m_code.EmitVeorQ(NEON_RT, NEON_RT, NEON_RT))
				return false;
		}
		else if (!emit_gpr_address(rt, HOST_TMP1) ||
				 !m_code.EmitVld1Q32(NEON_RT, HOST_TMP1))
		{
			return false;
		}

		bool vector_op_ok = false;
		switch (operation)
		{
			case MmiUpperInterleaveOp::Word:
				vector_op_ok = m_code.EmitVuzpI32Q(NEON_RT, NEON_RS);
				break;
			case MmiUpperInterleaveOp::Halfword:
				vector_op_ok = m_code.EmitVuzpI16Q(NEON_RT, NEON_RS);
				break;
			case MmiUpperInterleaveOp::Byte:
				vector_op_ok = m_code.EmitVuzpI8Q(NEON_RT, NEON_RS);
				break;
		}

		// vuzp RT, RS leaves the even RT lanes followed by the even RS lanes in
		// RT, matching PCSX2's PPACW/PPACH/PPACB definitions.
		if (!vector_op_ok ||
			!emit_gpr_address(rd, HOST_TMP2) ||
			!m_code.EmitVst1Q32(NEON_RT, HOST_TMP2))
		{
			return false;
		}

		return true;
	}

	bool BlockCompiler::EmitPABSW(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI1() dispatches this
		// opcode to MMI.cpp::PABSW(), which computes saturating absolute
		// values for four signed 32-bit lanes from RT and ignores RS.
		return EmitMmiUnaryRtVectorOp(op, MmiUnaryVectorOp::AbsoluteSignedWord);
	}

	bool BlockCompiler::EmitPSLLH(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::tbl_MMI dispatches this opcode
		// to MMI.cpp::PSLLH(), which left-shifts eight 16-bit lanes by SA & 0xf.
		return EmitMmiImmediateShiftOp(op, MmiImmediateShiftOp::ShiftLeftHalfword);
	}

	bool BlockCompiler::EmitPSRLH(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::tbl_MMI dispatches this opcode
		// to MMI.cpp::PSRLH(), which logical-right-shifts eight 16-bit lanes by SA & 0xf.
		return EmitMmiImmediateShiftOp(op, MmiImmediateShiftOp::ShiftRightLogicalHalfword);
	}

	bool BlockCompiler::EmitPSRAH(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::tbl_MMI dispatches this opcode
		// to MMI.cpp::PSRAH(), which arithmetic-right-shifts eight signed 16-bit lanes by SA & 0xf.
		return EmitMmiImmediateShiftOp(op, MmiImmediateShiftOp::ShiftRightArithmeticHalfword);
	}

	bool BlockCompiler::EmitPSLLW(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::tbl_MMI dispatches this opcode
		// to MMI.cpp::PSLLW(), which left-shifts four 32-bit lanes by SA.
		return EmitMmiImmediateShiftOp(op, MmiImmediateShiftOp::ShiftLeftWord);
	}

	bool BlockCompiler::EmitPSRLW(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::tbl_MMI dispatches this opcode
		// to MMI.cpp::PSRLW(), which logical-right-shifts four 32-bit lanes by SA.
		return EmitMmiImmediateShiftOp(op, MmiImmediateShiftOp::ShiftRightLogicalWord);
	}

	bool BlockCompiler::EmitPSRAW(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::tbl_MMI dispatches this opcode
		// to MMI.cpp::PSRAW(), which arithmetic-right-shifts four signed 32-bit lanes by SA.
		return EmitMmiImmediateShiftOp(op, MmiImmediateShiftOp::ShiftRightArithmeticWord);
	}

	bool BlockCompiler::EmitPADDW(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI0() dispatches this
		// opcode to MMI.cpp::PADDW(), which wraps four independent 32-bit lanes.
		// The Cortex-A9 path uses call-clobbered NEON q0-q2 as block-local
		// temporaries; this matches the full128 MMI promotion rule in the Vita
		// ARM optimization plan.
		return EmitMmiVectorOp(op, MmiVectorOp::AddWord);
	}

	bool BlockCompiler::EmitPSUBW(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI0() dispatches this
		// opcode to MMI.cpp::PSUBW(), which wraps four independent 32-bit lanes.
		return EmitMmiVectorOp(op, MmiVectorOp::SubtractWord);
	}

	bool BlockCompiler::EmitPCGTW(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI0() dispatches this
		// opcode to MMI.cpp::PCGTW(), signed-compares four independent 32-bit
		// lanes, and writes each result as 0xffffffff or 0x00000000.
		return EmitMmiVectorOp(op, MmiVectorOp::CompareGreaterSignedWord);
	}

	bool BlockCompiler::EmitPMAXW(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI0() dispatches this
		// opcode to MMI.cpp::PMAXW(), which writes the signed max of each
		// independent 32-bit lane.
		return EmitMmiVectorOp(op, MmiVectorOp::MaxSignedWord);
	}

	bool BlockCompiler::EmitPADDH(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI0() dispatches this
		// opcode to MMI.cpp::PADDH(), which wraps eight independent 16-bit lanes.
		return EmitMmiVectorOp(op, MmiVectorOp::AddHalfword);
	}

	bool BlockCompiler::EmitPSUBH(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI0() dispatches this
		// opcode to MMI.cpp::PSUBH(), which wraps eight independent 16-bit lanes.
		return EmitMmiVectorOp(op, MmiVectorOp::SubtractHalfword);
	}

	bool BlockCompiler::EmitPCGTH(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI0() dispatches this
		// opcode to MMI.cpp::PCGTH(), signed-compares eight independent 16-bit
		// lanes, and writes each result as 0xffff or 0x0000.
		return EmitMmiVectorOp(op, MmiVectorOp::CompareGreaterSignedHalfword);
	}

	bool BlockCompiler::EmitPMAXH(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI0() dispatches this
		// opcode to MMI.cpp::PMAXH(), which writes the signed max of each
		// independent 16-bit lane.
		return EmitMmiVectorOp(op, MmiVectorOp::MaxSignedHalfword);
	}

	bool BlockCompiler::EmitPADDB(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI0() dispatches this
		// opcode to MMI.cpp::PADDB(), which wraps sixteen independent 8-bit lanes.
		return EmitMmiVectorOp(op, MmiVectorOp::AddByte);
	}

	bool BlockCompiler::EmitPSUBB(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI0() dispatches this
		// opcode to MMI.cpp::PSUBB(), which wraps sixteen independent 8-bit lanes.
		return EmitMmiVectorOp(op, MmiVectorOp::SubtractByte);
	}

	bool BlockCompiler::EmitPCGTB(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI0() dispatches this
		// opcode to MMI.cpp::PCGTB(), signed-compares sixteen independent 8-bit
		// lanes, and writes each result as 0xff or 0x00.
		return EmitMmiVectorOp(op, MmiVectorOp::CompareGreaterSignedByte);
	}

	bool BlockCompiler::EmitPADDSW(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI0() dispatches this
		// opcode to MMI.cpp::PADDSW(), which signed-saturates four 32-bit sums.
		return EmitMmiVectorOp(op, MmiVectorOp::SaturatingAddSignedWord);
	}

	bool BlockCompiler::EmitPSUBSW(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI0() dispatches this
		// opcode to MMI.cpp::PSUBSW(), which signed-saturates four 32-bit differences.
		return EmitMmiVectorOp(op, MmiVectorOp::SaturatingSubtractSignedWord);
	}

	bool BlockCompiler::EmitPEXTLW(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI0() dispatches this
		// opcode to MMI.cpp::PEXTLW(), which interleaves the lower 32-bit lanes
		// of RT and RS as RT0, RS0, RT1, RS1.
		return EmitMmiInterleaveOp(op, MmiUpperInterleaveOp::Word, false);
	}

	bool BlockCompiler::EmitPPACW(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI0() dispatches this
		// opcode to MMI.cpp::PPACW(), which packs even 32-bit lanes as
		// RT0, RT2, RS0, RS2.
		return EmitMmiPackEvenOp(op, MmiUpperInterleaveOp::Word);
	}

	bool BlockCompiler::EmitPADDSH(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI0() dispatches this
		// opcode to MMI.cpp::PADDSH(), which signed-saturates eight 16-bit sums.
		return EmitMmiVectorOp(op, MmiVectorOp::SaturatingAddSignedHalfword);
	}

	bool BlockCompiler::EmitPSUBSH(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI0() dispatches this
		// opcode to MMI.cpp::PSUBSH(), which signed-saturates eight 16-bit differences.
		return EmitMmiVectorOp(op, MmiVectorOp::SaturatingSubtractSignedHalfword);
	}

	bool BlockCompiler::EmitPEXTLH(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI0() dispatches this
		// opcode to MMI.cpp::PEXTLH(), which interleaves the lower 16-bit lanes
		// of RT and RS as RT0, RS0, ... RT3, RS3.
		return EmitMmiInterleaveOp(op, MmiUpperInterleaveOp::Halfword, false);
	}

	bool BlockCompiler::EmitPPACH(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI0() dispatches this
		// opcode to MMI.cpp::PPACH(), which packs even 16-bit lanes as
		// RT0, RT2, RT4, RT6, RS0, RS2, RS4, RS6.
		return EmitMmiPackEvenOp(op, MmiUpperInterleaveOp::Halfword);
	}

	bool BlockCompiler::EmitPADDSB(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI0() dispatches this
		// opcode to MMI.cpp::PADDSB(), which signed-saturates sixteen 8-bit sums.
		return EmitMmiVectorOp(op, MmiVectorOp::SaturatingAddSignedByte);
	}

	bool BlockCompiler::EmitPSUBSB(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI0() dispatches this
		// opcode to MMI.cpp::PSUBSB(), which signed-saturates sixteen 8-bit differences.
		return EmitMmiVectorOp(op, MmiVectorOp::SaturatingSubtractSignedByte);
	}

	bool BlockCompiler::EmitPEXTLB(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI0() dispatches this
		// opcode to MMI.cpp::PEXTLB(), which interleaves the lower 8-bit lanes
		// of RT and RS as RT0, RS0, ... RT7, RS7.
		return EmitMmiInterleaveOp(op, MmiUpperInterleaveOp::Byte, false);
	}

	bool BlockCompiler::EmitPPACB(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI0() dispatches this
		// opcode to MMI.cpp::PPACB(), which packs even 8-bit lanes as
		// RT0, RT2, ... RT14, RS0, RS2, ... RS14.
		return EmitMmiPackEvenOp(op, MmiUpperInterleaveOp::Byte);
	}

	bool BlockCompiler::EmitPEXT5(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI0() dispatches this
		// opcode to MMI.cpp::PEXT5(), which expands each RT 5:5:5:1 word into
		// byte-aligned 8:8:8:8 fields.
		return EmitMmiFiveBitOp(op, MmiFiveBitOp::Expand);
	}

	bool BlockCompiler::EmitPPAC5(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI0() dispatches this
		// opcode to MMI.cpp::PPAC5(), which packs each RT 8:8:8:8 word back
		// into a 5:5:5:1 field layout.
		return EmitMmiFiveBitOp(op, MmiFiveBitOp::Pack);
	}

	bool BlockCompiler::EmitPCEQW(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI1() dispatches this
		// opcode to MMI.cpp::PCEQW(), which writes 0xffffffff for equal 32-bit lanes.
		return EmitMmiVectorOp(op, MmiVectorOp::CompareEqualWord);
	}

	bool BlockCompiler::EmitPMINW(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI1() dispatches this
		// opcode to MMI.cpp::PMINW(), which writes the signed min of each 32-bit lane.
		return EmitMmiVectorOp(op, MmiVectorOp::MinSignedWord);
	}

	bool BlockCompiler::EmitPADSBH(u32 op)
	{
		const unsigned rd = RD(op);
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		if (rd == 0)
			return true;

		constexpr unsigned NEON_RS = 0;
		constexpr unsigned NEON_RT = 1;
		constexpr unsigned NEON_SUB = 2;
		constexpr unsigned NEON_ADD = 3;
		constexpr unsigned NEON_SUB_LOW_D = NEON_SUB * 2;
		constexpr unsigned NEON_ADD_HIGH_D = NEON_ADD * 2 + 1;

		const auto emit_gpr_address = [this](unsigned guest_reg, unsigned host_reg) {
			const size_t offset = GprOffset(guest_reg);
			if (offset <= 255)
				return m_code.EmitAddImm8(host_reg, HOST_CPU_REGS, static_cast<u8>(offset));

			return m_code.EmitMovImm32(host_reg, static_cast<u32>(offset)) &&
				   m_code.EmitAddReg(host_reg, HOST_CPU_REGS, host_reg);
		};

		if (rs == 0)
		{
			if (!m_code.EmitVeorQ(NEON_RS, NEON_RS, NEON_RS))
				return false;
		}
		else if (!emit_gpr_address(rs, HOST_TMP0) ||
				 !m_code.EmitVld1Q32(NEON_RS, HOST_TMP0))
		{
			return false;
		}

		if (rt == 0)
		{
			if (!m_code.EmitVeorQ(NEON_RT, NEON_RT, NEON_RT))
				return false;
		}
		else if (!emit_gpr_address(rt, HOST_TMP1) ||
				 !m_code.EmitVld1Q32(NEON_RT, HOST_TMP1))
		{
			return false;
		}

		// PCSX2 owner: MMI.cpp::PADSBH() subtracts lanes 0..3 with PSUBH
		// semantics and adds lanes 4..7 with PADDH semantics.
		return m_code.EmitVsubI16Q(NEON_SUB, NEON_RS, NEON_RT) &&
			   m_code.EmitVaddI16Q(NEON_ADD, NEON_RS, NEON_RT) &&
			   emit_gpr_address(rd, HOST_TMP2) &&
			   m_code.EmitVst1D32(NEON_SUB_LOW_D, HOST_TMP2) &&
			   m_code.EmitAddImm8(HOST_TMP2, HOST_TMP2, 8) &&
			   m_code.EmitVst1D32(NEON_ADD_HIGH_D, HOST_TMP2);
	}

	bool BlockCompiler::EmitPABSH(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI1() dispatches this
		// opcode to MMI.cpp::PABSH(), which computes saturating absolute
		// values for eight signed 16-bit lanes from RT and ignores RS.
		return EmitMmiUnaryRtVectorOp(op, MmiUnaryVectorOp::AbsoluteSignedHalfword);
	}

	bool BlockCompiler::EmitPCEQH(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI1() dispatches this
		// opcode to MMI.cpp::PCEQH(), which writes 0xffff for equal 16-bit lanes.
		return EmitMmiVectorOp(op, MmiVectorOp::CompareEqualHalfword);
	}

	bool BlockCompiler::EmitPMINH(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI1() dispatches this
		// opcode to MMI.cpp::PMINH(), which writes the signed min of each 16-bit lane.
		return EmitMmiVectorOp(op, MmiVectorOp::MinSignedHalfword);
	}

	bool BlockCompiler::EmitPCEQB(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI1() dispatches this
		// opcode to MMI.cpp::PCEQB(), which writes 0xff for equal 8-bit lanes.
		return EmitMmiVectorOp(op, MmiVectorOp::CompareEqualByte);
	}

	bool BlockCompiler::EmitPADDUW(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI1() dispatches this
		// opcode to MMI.cpp::PADDUW(), which unsigned-saturates four 32-bit sums.
		return EmitMmiVectorOp(op, MmiVectorOp::SaturatingAddUnsignedWord);
	}

	bool BlockCompiler::EmitPSUBUW(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI1() dispatches this
		// opcode to MMI.cpp::PSUBUW(), which unsigned-saturates four 32-bit differences.
		return EmitMmiVectorOp(op, MmiVectorOp::SaturatingSubtractUnsignedWord);
	}

	bool BlockCompiler::EmitPEXTUW(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI1() dispatches this
		// opcode to MMI.cpp::PEXTUW(), which interleaves the upper 32-bit lanes
		// of RT and RS as RT2, RS2, RT3, RS3.
		return EmitMmiUpperInterleaveOp(op, MmiUpperInterleaveOp::Word);
	}

	bool BlockCompiler::EmitPADDUH(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI1() dispatches this
		// opcode to MMI.cpp::PADDUH(), which unsigned-saturates eight 16-bit sums.
		return EmitMmiVectorOp(op, MmiVectorOp::SaturatingAddUnsignedHalfword);
	}

	bool BlockCompiler::EmitPSUBUH(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI1() dispatches this
		// opcode to MMI.cpp::PSUBUH(), which unsigned-saturates eight 16-bit differences.
		return EmitMmiVectorOp(op, MmiVectorOp::SaturatingSubtractUnsignedHalfword);
	}

	bool BlockCompiler::EmitPEXTUH(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI1() dispatches this
		// opcode to MMI.cpp::PEXTUH(), which interleaves the upper 16-bit lanes
		// of RT and RS as RT4, RS4, ... RT7, RS7.
		return EmitMmiUpperInterleaveOp(op, MmiUpperInterleaveOp::Halfword);
	}

	bool BlockCompiler::EmitPADDUB(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI1() dispatches this
		// opcode to MMI.cpp::PADDUB(), which unsigned-saturates sixteen 8-bit sums.
		return EmitMmiVectorOp(op, MmiVectorOp::SaturatingAddUnsignedByte);
	}

	bool BlockCompiler::EmitPSUBUB(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI1() dispatches this
		// opcode to MMI.cpp::PSUBUB(), which unsigned-saturates sixteen 8-bit differences.
		return EmitMmiVectorOp(op, MmiVectorOp::SaturatingSubtractUnsignedByte);
	}

	bool BlockCompiler::EmitPEXTUB(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI1() dispatches this
		// opcode to MMI.cpp::PEXTUB(), which interleaves the upper 8-bit lanes
		// of RT and RS as RT8, RS8, ... RT15, RS15.
		return EmitMmiUpperInterleaveOp(op, MmiUpperInterleaveOp::Byte);
	}

	bool BlockCompiler::EmitQFSRV(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI1() dispatches this
		// opcode to MMI.cpp::QFSRV(); x86/iMMI.cpp::recQFSRV() implements it as
		// a 16-byte unaligned extract from RT||RS at byte offset cpuRegs.sa.
		const unsigned rd = RD(op);
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		if (rd == 0)
			return true;

		constexpr unsigned NEON_RS = 0;
		constexpr unsigned NEON_RT = 1;
		constexpr unsigned NEON_RD = 2;

		const auto emit_gpr_address = [this](unsigned guest_reg, unsigned host_reg) {
			const size_t offset = GprOffset(guest_reg);
			if (offset <= 255)
				return m_code.EmitAddImm8(host_reg, HOST_CPU_REGS, static_cast<u8>(offset));

			return m_code.EmitMovImm32(host_reg, static_cast<u32>(offset)) &&
				   m_code.EmitAddReg(host_reg, HOST_CPU_REGS, host_reg);
		};

		if (rs == 0)
		{
			if (!m_code.EmitVeorQ(NEON_RS, NEON_RS, NEON_RS))
				return false;
		}
		else if (!emit_gpr_address(rs, HOST_TMP0) ||
				 !m_code.EmitVld1Q32(NEON_RS, HOST_TMP0))
		{
			return false;
		}

		if (rt == 0)
		{
			if (!m_code.EmitVeorQ(NEON_RT, NEON_RT, NEON_RT))
				return false;
		}
		else if (!emit_gpr_address(rt, HOST_TMP1) ||
				 !m_code.EmitVld1Q32(NEON_RT, HOST_TMP1))
		{
			return false;
		}

		if (!emit_gpr_address(rd, HOST_TMP2) ||
			!m_code.EmitLdrImm12(HOST_TMP3, HOST_CPU_REGS, static_cast<u16>(SA_OFFSET)) ||
			!m_code.EmitAndImm8(HOST_TMP3, HOST_TMP3, 0x0f))
		{
			return false;
		}

		size_t done_branches[15]{};
		unsigned done_count = 0;
		for (u8 offset = 0; offset < 15; offset++)
		{
			if (!m_code.EmitMovImm8(HOST_TMP4, offset) ||
				!m_code.EmitCmpReg(HOST_TMP3, HOST_TMP4))
			{
				return false;
			}

			const size_t next_case = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (next_case == static_cast<size_t>(-1))
				return false;

			if (!m_code.EmitVextI8Q(NEON_RD, NEON_RT, NEON_RS, offset))
				return false;

			const size_t done = m_code.EmitBranchPlaceholder();
			if (done == static_cast<size_t>(-1))
				return false;
			done_branches[done_count++] = done;

			if (!m_code.PatchBranch(next_case, m_code.Size(), VitaA32::Condition::NE))
				return false;
		}

		if (!m_code.EmitVextI8Q(NEON_RD, NEON_RT, NEON_RS, 15))
			return false;

		const size_t store_target = m_code.Size();
		for (unsigned i = 0; i < done_count; i++)
		{
			if (!m_code.PatchBranch(done_branches[i], store_target))
				return false;
		}

		return m_code.EmitVst1Q32(NEON_RD, HOST_TMP2);
	}

	bool BlockCompiler::EmitPSLLVW(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI2() dispatches this
		// opcode to MMI.cpp::PSLLVW(), which shifts RT.UL[0]/[2] by
		// RS.UL[0]/[2] & 0x1f and sign-extends each 32-bit result to 64 bits.
		return EmitMmiVariableWordShiftOp(op, MmiVariableWordShiftOp::ShiftLeftLogical);
	}

	bool BlockCompiler::EmitPSRLVW(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI2() dispatches this
		// opcode to MMI.cpp::PSRLVW(), which logically shifts RT.UL[0]/[2] by
		// RS.UL[0]/[2] & 0x1f and sign-extends each 32-bit result to 64 bits.
		return EmitMmiVariableWordShiftOp(op, MmiVariableWordShiftOp::ShiftRightLogical);
	}

	bool BlockCompiler::EmitPSRAVW(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI3() dispatches this
		// opcode to MMI.cpp::PSRAVW(), which arithmetically shifts RT.SL[0]/[2]
		// by RS.UL[0]/[2] & 0x1f and sign-extends each result to 64 bits.
		return EmitMmiVariableWordShiftOp(op, MmiVariableWordShiftOp::ShiftRightArithmetic);
	}

	bool BlockCompiler::EmitPINTH(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI2() dispatches this
		// opcode to MMI.cpp::PINTH(), which interleaves RT low halfwords with
		// RS upper halfwords.
		return EmitMmiHalfwordShuffleOp(op, MmiHalfwordShuffleOp::Pinth);
	}

	bool BlockCompiler::EmitPCPYLD(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI2() dispatches this
		// opcode to MMI.cpp::PCPYLD(), which copies RT.UD[0] into RD.UD[0]
		// and RS.UD[0] into RD.UD[1].
		return EmitMmiWordShuffleOp(op, MmiWordShuffleOp::Pcpyld);
	}

	bool BlockCompiler::EmitPEXEH(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI2() dispatches this
		// opcode to MMI.cpp::PEXEH(), which swaps halfwords 0/2 inside each
		// 64-bit half while leaving halfwords 1/3 in place.
		return EmitMmiHalfwordShuffleOp(op, MmiHalfwordShuffleOp::Pexeh);
	}

	bool BlockCompiler::EmitPREVH(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI2() dispatches this
		// opcode to MMI.cpp::PREVH(), which reverses the four halfwords inside
		// each 64-bit half.
		return EmitMmiHalfwordShuffleOp(op, MmiHalfwordShuffleOp::Prevh);
	}

	bool BlockCompiler::EmitPEXEW(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI2() dispatches this
		// opcode to MMI.cpp::PEXEW(), which writes RT words as 2,1,0,3.
		return EmitMmiWordShuffleOp(op, MmiWordShuffleOp::Pexew);
	}

	bool BlockCompiler::EmitPROT3W(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI2() dispatches this
		// opcode to MMI.cpp::PROT3W(), which writes RT words as 1,2,0,3.
		return EmitMmiWordShuffleOp(op, MmiWordShuffleOp::Prot3w);
	}

	bool BlockCompiler::EmitPINTEH(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI3() dispatches this
		// opcode to MMI.cpp::PINTEH(), which interleaves even RT and RS
		// halfwords.
		return EmitMmiHalfwordShuffleOp(op, MmiHalfwordShuffleOp::Pinteh);
	}

	bool BlockCompiler::EmitPCPYUD(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI3() dispatches this
		// opcode to MMI.cpp::PCPYUD(), which copies RS.UD[1] into RD.UD[0]
		// and RT.UD[1] into RD.UD[1].
		return EmitMmiWordShuffleOp(op, MmiWordShuffleOp::Pcpyud);
	}

	bool BlockCompiler::EmitPMFHI(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI2() dispatches this
		// opcode to MMI.cpp::PMFHI(), which copies the full 128-bit HI value
		// into RD unless RD is zero.
		return EmitMoveFullFromHiLo(op, HI_OFFSET);
	}

	bool BlockCompiler::EmitPMFLO(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI2() dispatches this
		// opcode to MMI.cpp::PMFLO(), which copies the full 128-bit LO value
		// into RD unless RD is zero.
		return EmitMoveFullFromHiLo(op, LO_OFFSET);
	}

	bool BlockCompiler::EmitPMTHI(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI3() dispatches this
		// opcode to MMI.cpp::PMTHI(), which copies the full 128-bit RS value
		// into HI.
		return EmitMoveFullToHiLo(op, HI_OFFSET);
	}

	bool BlockCompiler::EmitPMTLO(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI3() dispatches this
		// opcode to MMI.cpp::PMTLO(), which copies the full 128-bit RS value
		// into LO.
		return EmitMoveFullToHiLo(op, LO_OFFSET);
	}

	bool BlockCompiler::EmitPEXCH(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI3() dispatches this
		// opcode to MMI.cpp::PEXCH(), which swaps halfwords 1/2 inside each
		// 64-bit half.
		return EmitMmiHalfwordShuffleOp(op, MmiHalfwordShuffleOp::Pexch);
	}

	bool BlockCompiler::EmitPCPYH(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI3() dispatches this
		// opcode to MMI.cpp::PCPYH(), which broadcasts RT.US[0] into the lower
		// 64-bit half and RT.US[4] into the upper 64-bit half.
		return EmitMmiHalfwordShuffleOp(op, MmiHalfwordShuffleOp::Pcpyh);
	}

	bool BlockCompiler::EmitPEXCW(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI3() dispatches this
		// opcode to MMI.cpp::PEXCW(), which writes RT words as 0,2,1,3.
		return EmitMmiWordShuffleOp(op, MmiWordShuffleOp::Pexcw);
	}

	bool BlockCompiler::EmitPAND(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI2() dispatches this
		// opcode to MMI.cpp::PAND(), which bitwise-ANDs both 64-bit halves of
		// the full 128-bit GPR value.
		return EmitMmiVectorOp(op, MmiVectorOp::BitwiseAnd);
	}

	bool BlockCompiler::EmitPXOR(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI2() dispatches this
		// opcode to MMI.cpp::PXOR(), which bitwise-XORs both 64-bit halves of
		// the full 128-bit GPR value.
		return EmitMmiVectorOp(op, MmiVectorOp::BitwiseXor);
	}

	bool BlockCompiler::EmitPOR(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI3() dispatches this
		// opcode to MMI.cpp::POR(), which bitwise-ORs both 64-bit halves of
		// the full 128-bit GPR value.
		return EmitMmiVectorOp(op, MmiVectorOp::BitwiseOr);
	}

	bool BlockCompiler::EmitPNOR(u32 op)
	{
		// PCSX2 owners: R5900OpcodeTables.cpp::Class_MMI3() dispatches this
		// opcode to MMI.cpp::PNOR(), which writes the full 128-bit bitwise-NOR
		// of RS and RT.
		return EmitMmiVectorOp(op, MmiVectorOp::BitwiseNor);
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
			case 0x18: // MTSAB, owned by R5900OpcodeImpl.cpp::MTSAB().
				return EmitMTSAB(op);
			case 0x19: // MTSAH, owned by R5900OpcodeImpl.cpp::MTSAH().
				return EmitMTSAH(op);
			default:
				return false;
		}
	}

	bool BlockCompiler::EmitMTSAB(u32 op)
	{
		// PCSX2 owner: R5900OpcodeImpl.cpp::MTSAB().
		const u8 imm = static_cast<u8>(IMM_U(op) & 0x0f);
		if (!EmitLoadGprLow(RS(op), HOST_TMP0) ||
			!m_code.EmitAndImm8(HOST_TMP0, HOST_TMP0, 0x0f))
		{
			return false;
		}

		if (imm != 0 && !m_code.EmitEorImm8(HOST_TMP0, HOST_TMP0, imm))
			return false;

		return m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(SA_OFFSET));
	}

	bool BlockCompiler::EmitMTSAH(u32 op)
	{
		// PCSX2 owner: R5900OpcodeImpl.cpp::MTSAH().
		const u8 imm = static_cast<u8>(IMM_U(op) & 0x07);
		if (!EmitLoadGprLow(RS(op), HOST_TMP0) ||
			!m_code.EmitAndImm8(HOST_TMP0, HOST_TMP0, 0x07))
		{
			return false;
		}

		if (imm != 0 && !m_code.EmitEorImm8(HOST_TMP0, HOST_TMP0, imm))
			return false;

		return m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_TMP0, VitaA32::ShiftType::LSL, 1) &&
			   m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(SA_OFFSET));
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

	bool BlockCompiler::EmitLB(u32 op, u32 pc, u32 raw_cycles_through_instruction,
		const void* event_exit, bool branch_delay_slot)
	{
		return EmitLoadWithCounterReadEvent(op, pc, raw_cycles_through_instruction, event_exit,
			reinterpret_cast<const void*>(&VitaEeMemRead8), true, 24, branch_delay_slot,
			ScalarLoadWidth::Byte, 0);
	}

	bool BlockCompiler::EmitLH(u32 op, u32 pc, u32 raw_cycles_through_instruction,
		const void* event_exit, bool branch_delay_slot)
	{
		return EmitLoadWithCounterReadEvent(op, pc, raw_cycles_through_instruction, event_exit,
			reinterpret_cast<const void*>(&VitaEeMemRead16Checked), true, 16, branch_delay_slot,
			ScalarLoadWidth::Halfword, 1);
	}

	bool BlockCompiler::EmitLW(u32 op, u32 pc, u32 raw_cycles_through_instruction,
		const void* event_exit, bool branch_delay_slot)
	{
		const unsigned rt = RT(op);

		size_t unaligned_fallback = static_cast<size_t>(-1);
		size_t handler_fallback = static_cast<size_t>(-1);
		if (!EmitEffectiveAddress(op, HOST_TMP0) ||
			(branch_delay_slot &&
			 !m_code.EmitMovRegShiftImm(HOST_TMP5, HOST_BRANCH_FLAG, VitaA32::ShiftType::LSL, 0)) ||
			!EmitCounterReadFlagFromAddress(HOST_TMP0) ||
			!m_code.EmitAndImm8(HOST_TMP1, HOST_TMP0, 3, true))
		{
			return false;
		}

		unaligned_fallback = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
		if (unaligned_fallback == static_cast<size_t>(-1))
			return false;

		if (!EmitVtlbNonHandlerHostAddress(HOST_TMP0, HOST_TMP1, HOST_TMP2, &handler_fallback) ||
			!m_code.EmitLdrImm12(HOST_TMP0, HOST_TMP0, 0))
		{
			return false;
		}

		const size_t value_ready = m_code.EmitBranchPlaceholder();
		if (value_ready == static_cast<size_t>(-1))
			return false;

		const size_t fallback_target = m_code.Size();
		if (!m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&VitaEeMemRead32Checked)))
			return false;

		const size_t value_ready_target = m_code.Size();
		auto emit_result_tail = [&]() -> bool {
			if (rt == 0)
			{
				return !branch_delay_slot ||
					   m_code.EmitMovRegShiftImm(HOST_BRANCH_FLAG, HOST_TMP5, VitaA32::ShiftType::LSL, 0);
			}

			return m_code.EmitMovRegShiftImm(HOST_TMP1, HOST_TMP0, VitaA32::ShiftType::ASR, 31) &&
				   EmitStoreGpr64(rt, HOST_TMP0, HOST_TMP1) &&
				   EmitCounterReadEventExit(pc + 4, raw_cycles_through_instruction, event_exit) &&
				   (!branch_delay_slot ||
					   m_code.EmitMovRegShiftImm(HOST_BRANCH_FLAG, HOST_TMP5, VitaA32::ShiftType::LSL, 0));
		};

		if (!emit_result_tail())
			return false;

		return m_code.PatchBranch(unaligned_fallback, fallback_target, VitaA32::Condition::NE) &&
			   m_code.PatchBranch(handler_fallback, fallback_target, VitaA32::Condition::MI) &&
			   m_code.PatchBranch(value_ready, value_ready_target);
	}

	bool BlockCompiler::EmitLBU(u32 op, u32 pc, u32 raw_cycles_through_instruction,
		const void* event_exit, bool branch_delay_slot)
	{
		return EmitLoadWithCounterReadEvent(op, pc, raw_cycles_through_instruction, event_exit,
			reinterpret_cast<const void*>(&VitaEeMemRead8), false, 0, branch_delay_slot,
			ScalarLoadWidth::Byte, 0);
	}

	bool BlockCompiler::EmitLHU(u32 op, u32 pc, u32 raw_cycles_through_instruction,
		const void* event_exit, bool branch_delay_slot)
	{
		return EmitLoadWithCounterReadEvent(op, pc, raw_cycles_through_instruction, event_exit,
			reinterpret_cast<const void*>(&VitaEeMemRead16Checked), false, 0, branch_delay_slot,
			ScalarLoadWidth::Halfword, 1);
	}

	bool BlockCompiler::EmitLWU(u32 op)
	{
		const unsigned rt = RT(op);

		size_t unaligned_fallback = static_cast<size_t>(-1);
		size_t handler_fallback = static_cast<size_t>(-1);
		if (!EmitEffectiveAddress(op, HOST_TMP0) ||
			!m_code.EmitAndImm8(HOST_TMP1, HOST_TMP0, 3, true))
		{
			return false;
		}

		unaligned_fallback = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
		if (unaligned_fallback == static_cast<size_t>(-1))
			return false;

		if (!EmitVtlbNonHandlerHostAddress(HOST_TMP0, HOST_TMP1, HOST_TMP2, &handler_fallback) ||
			!m_code.EmitLdrImm12(HOST_TMP0, HOST_TMP0, 0))
		{
			return false;
		}

		const size_t value_ready = m_code.EmitBranchPlaceholder();
		if (value_ready == static_cast<size_t>(-1))
			return false;

		const size_t fallback_target = m_code.Size();
		if (!m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&VitaEeMemRead32Checked)))
			return false;

		const size_t value_ready_target = m_code.Size();
		if (rt != 0 &&
			(!m_code.EmitMovImm8(HOST_TMP1, 0) ||
			 !EmitStoreGpr64(rt, HOST_TMP0, HOST_TMP1)))
		{
			return false;
		}

		return m_code.PatchBranch(unaligned_fallback, fallback_target, VitaA32::Condition::NE) &&
			   m_code.PatchBranch(handler_fallback, fallback_target, VitaA32::Condition::MI) &&
			   m_code.PatchBranch(value_ready, value_ready_target);
	}

	bool BlockCompiler::EmitLWL(u32 op)
	{
		return EmitPartialWordLoad(op, true);
	}

	bool BlockCompiler::EmitLWR(u32 op)
	{
		return EmitPartialWordLoad(op, false);
	}

	bool BlockCompiler::EmitLD(u32 op)
	{
		const unsigned rt = RT(op);

		size_t unaligned_fallback = static_cast<size_t>(-1);
		size_t handler_fallback = static_cast<size_t>(-1);
		if (!EmitEffectiveAddress(op, HOST_TMP0) ||
			!m_code.EmitAndImm8(HOST_TMP1, HOST_TMP0, 7, true))
		{
			return false;
		}

		unaligned_fallback = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
		if (unaligned_fallback == static_cast<size_t>(-1))
			return false;

		if (!EmitVtlbNonHandlerHostAddress(HOST_TMP0, HOST_TMP1, HOST_TMP2, &handler_fallback) ||
			!m_code.EmitLdrImm12(HOST_TMP1, HOST_TMP0, sizeof(u32)) ||
			!m_code.EmitLdrImm12(HOST_TMP0, HOST_TMP0, 0))
		{
			return false;
		}

		const size_t value_ready = m_code.EmitBranchPlaceholder();
		if (value_ready == static_cast<size_t>(-1))
			return false;

		const size_t fallback_target = m_code.Size();
		if (!m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&VitaEeMemRead64Checked)))
			return false;

		const size_t value_ready_target = m_code.Size();
		if (rt == 0)
		{
			// PCSX2 owner: R5900OpcodeImpl.cpp::LD() writes cpuRegs.GPR.r[_Rt_]
			// directly. This is intentionally different from LQ's gpr_GetWritePtr().
			const size_t offset = GprOffset(0);
			if (!m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(offset)) ||
				!m_code.EmitStrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(offset + sizeof(u32))))
			{
				return false;
			}
		}
		else if (!EmitStoreGpr64(rt, HOST_TMP0, HOST_TMP1))
		{
			return false;
		}

		return m_code.PatchBranch(unaligned_fallback, fallback_target, VitaA32::Condition::NE) &&
			   m_code.PatchBranch(handler_fallback, fallback_target, VitaA32::Condition::MI) &&
			   m_code.PatchBranch(value_ready, value_ready_target);
	}

	bool BlockCompiler::EmitLDL(u32 op)
	{
		return EmitPartialDwordLoad(op, true);
	}

	bool BlockCompiler::EmitLDR(u32 op)
	{
		return EmitPartialDwordLoad(op, false);
	}

	bool BlockCompiler::EmitLQ(u32 op)
	{
		const unsigned rt = RT(op);
		constexpr unsigned NEON_VALUE = 0;

		size_t handler_fallback = static_cast<size_t>(-1);
		if (!EmitEffectiveAddress(op, HOST_TMP0) ||
			!m_code.EmitMovRegShiftImm(HOST_TMP5, HOST_TMP0, VitaA32::ShiftType::LSL, 0) ||
			!EmitAlignQwordAddress(HOST_TMP0, HOST_TMP1) ||
			!EmitVtlbNonHandlerHostAddress128(HOST_TMP0, HOST_TMP1, HOST_TMP2, &handler_fallback))
		{
			return false;
		}

		if (rt != 0 &&
			(!m_code.EmitVld1Q32(NEON_VALUE, HOST_TMP0) ||
			 !EmitCpuRegsAddress(HOST_TMP1, GprOffset(rt)) ||
			 !m_code.EmitVst1Q32(NEON_VALUE, HOST_TMP1)))
		{
			return false;
		}

		const size_t done = m_code.EmitBranchPlaceholder();
		if (done == static_cast<size_t>(-1))
			return false;

		const size_t fallback_target = m_code.Size();
		if (!m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_TMP5, VitaA32::ShiftType::LSL, 0) ||
			!m_code.EmitMovImm8(HOST_TMP1, static_cast<u8>(rt)) ||
			!m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&VitaEeMemRead128Aligned)))
		{
			return false;
		}

		return m_code.PatchBranch(handler_fallback, fallback_target, VitaA32::Condition::MI) &&
			   m_code.PatchBranch(done, m_code.Size());
	}

	bool BlockCompiler::EmitLWC1(u32 op)
	{
		const unsigned rt = RT(op);

		size_t unaligned_fallback = static_cast<size_t>(-1);
		size_t handler_fallback = static_cast<size_t>(-1);
		if (!EmitEffectiveAddress(op, HOST_TMP0) ||
			!m_code.EmitMovRegShiftImm(HOST_TMP5, HOST_TMP0, VitaA32::ShiftType::LSL, 0) ||
			!m_code.EmitAndImm8(HOST_TMP1, HOST_TMP0, 3, true))
		{
			return false;
		}

		unaligned_fallback = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
		if (unaligned_fallback == static_cast<size_t>(-1))
			return false;

		if (!EmitVtlbNonHandlerHostAddress(HOST_TMP0, HOST_TMP1, HOST_TMP2, &handler_fallback) ||
			!m_code.EmitLdrImm12(HOST_TMP1, HOST_TMP0, 0) ||
			!m_code.EmitStrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(FprOffset(rt))))
		{
			return false;
		}

		const size_t done = m_code.EmitBranchPlaceholder();
		if (done == static_cast<size_t>(-1))
			return false;

		const size_t fallback_target = m_code.Size();
		if (!m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_TMP5, VitaA32::ShiftType::LSL, 0) ||
			!m_code.EmitMovImm8(HOST_TMP1, static_cast<u8>(rt)) ||
			!m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&VitaEeMemReadCop1Word)))
		{
			return false;
		}

		return m_code.PatchBranch(unaligned_fallback, fallback_target, VitaA32::Condition::NE) &&
			   m_code.PatchBranch(handler_fallback, fallback_target, VitaA32::Condition::MI) &&
			   m_code.PatchBranch(done, m_code.Size());
	}

	bool BlockCompiler::EmitLQC2(u32 op)
	{
		const unsigned rt = RT(op);
		constexpr unsigned NEON_VALUE = 0;

		size_t handler_fallback = static_cast<size_t>(-1);
		if (!EmitEffectiveAddress(op, HOST_TMP0) ||
			!m_code.EmitMovRegShiftImm(HOST_TMP5, HOST_TMP0, VitaA32::ShiftType::LSL, 0) ||
			!EmitVtlbNonHandlerHostAddress128(HOST_TMP0, HOST_TMP1, HOST_TMP2, &handler_fallback) ||
			!m_code.EmitMovRegShiftImm(HOST_TMP5, HOST_TMP0, VitaA32::ShiftType::LSL, 0) ||
			!m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&vu0Sync)) ||
			!m_code.EmitVld1Q32(NEON_VALUE, HOST_TMP5))
		{
			return false;
		}

		if (rt != 0 &&
			(!EmitVu0VfAddress(HOST_TMP0, rt) ||
			 !m_code.EmitVst1Q32(NEON_VALUE, HOST_TMP0)))
		{
			return false;
		}

		const size_t done = m_code.EmitBranchPlaceholder();
		if (done == static_cast<size_t>(-1))
			return false;

		const size_t fallback_target = m_code.Size();
		if (!m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_TMP5, VitaA32::ShiftType::LSL, 0) ||
			!m_code.EmitMovImm8(HOST_TMP1, static_cast<u8>(rt)) ||
			!m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&VitaEeMemReadVu0Quad)))
		{
			return false;
		}

		return m_code.PatchBranch(handler_fallback, fallback_target, VitaA32::Condition::MI) &&
			   m_code.PatchBranch(done, m_code.Size());
	}

	bool BlockCompiler::EmitSB(u32 op)
	{
		const unsigned rt = RT(op);

		size_t handler_fallback = static_cast<size_t>(-1);
		if (!EmitEffectiveAddress(op, HOST_TMP0) ||
			!EmitVtlbNonHandlerHostAddress(HOST_TMP0, HOST_TMP1, HOST_TMP2, &handler_fallback) ||
			!EmitLoadGprLow(rt, HOST_TMP1) ||
			!m_code.EmitStrbImm12(HOST_TMP1, HOST_TMP0, 0))
		{
			return false;
		}

		const size_t done = m_code.EmitBranchPlaceholder();
		if (done == static_cast<size_t>(-1))
			return false;

		const size_t fallback_target = m_code.Size();
		if (!EmitLoadGprLow(rt, HOST_TMP1) ||
			!m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&VitaEeMemWrite8)))
		{
			return false;
		}

		return m_code.PatchBranch(handler_fallback, fallback_target, VitaA32::Condition::MI) &&
			   m_code.PatchBranch(done, m_code.Size());
	}

	bool BlockCompiler::EmitSH(u32 op)
	{
		const unsigned rt = RT(op);

		size_t unaligned_fallback = static_cast<size_t>(-1);
		size_t handler_fallback = static_cast<size_t>(-1);
		if (!EmitEffectiveAddress(op, HOST_TMP0) ||
			!m_code.EmitAndImm8(HOST_TMP1, HOST_TMP0, 1, true))
		{
			return false;
		}

		unaligned_fallback = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
		if (unaligned_fallback == static_cast<size_t>(-1))
			return false;

		if (!EmitVtlbNonHandlerHostAddress(HOST_TMP0, HOST_TMP1, HOST_TMP2, &handler_fallback) ||
			!EmitLoadGprLow(rt, HOST_TMP1) ||
			!m_code.EmitStrhImm8(HOST_TMP1, HOST_TMP0, 0))
		{
			return false;
		}

		const size_t done = m_code.EmitBranchPlaceholder();
		if (done == static_cast<size_t>(-1))
			return false;

		const size_t fallback_target = m_code.Size();
		if (!EmitLoadGprLow(rt, HOST_TMP1) ||
			!m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&VitaEeMemWrite16Checked)))
		{
			return false;
		}

		return m_code.PatchBranch(unaligned_fallback, fallback_target, VitaA32::Condition::NE) &&
			   m_code.PatchBranch(handler_fallback, fallback_target, VitaA32::Condition::MI) &&
			   m_code.PatchBranch(done, m_code.Size());
	}

	bool BlockCompiler::EmitSW(u32 op)
	{
		const unsigned rt = RT(op);

		size_t unaligned_fallback = static_cast<size_t>(-1);
		size_t handler_fallback = static_cast<size_t>(-1);
		if (!EmitEffectiveAddress(op, HOST_TMP0) ||
			!m_code.EmitAndImm8(HOST_TMP1, HOST_TMP0, 3, true))
		{
			return false;
		}

		unaligned_fallback = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
		if (unaligned_fallback == static_cast<size_t>(-1))
			return false;

		if (!EmitVtlbNonHandlerHostAddress(HOST_TMP0, HOST_TMP1, HOST_TMP2, &handler_fallback) ||
			!EmitLoadGprLow(rt, HOST_TMP1) ||
			!m_code.EmitStrImm12(HOST_TMP1, HOST_TMP0, 0))
		{
			return false;
		}

		const size_t done = m_code.EmitBranchPlaceholder();
		if (done == static_cast<size_t>(-1))
			return false;

		const size_t fallback_target = m_code.Size();
		if (!EmitLoadGprLow(rt, HOST_TMP1) ||
			!m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&VitaEeMemWrite32Checked)))
		{
			return false;
		}

		return m_code.PatchBranch(unaligned_fallback, fallback_target, VitaA32::Condition::NE) &&
			   m_code.PatchBranch(handler_fallback, fallback_target, VitaA32::Condition::MI) &&
			   m_code.PatchBranch(done, m_code.Size());
	}

	bool BlockCompiler::EmitSWL(u32 op)
	{
		return EmitPartialWordStore(op, true);
	}

	bool BlockCompiler::EmitSWR(u32 op)
	{
		return EmitPartialWordStore(op, false);
	}

	bool BlockCompiler::EmitSD(u32 op)
	{
		const unsigned rt = RT(op);

		size_t unaligned_fallback = static_cast<size_t>(-1);
		size_t handler_fallback = static_cast<size_t>(-1);
		if (!EmitEffectiveAddress(op, HOST_TMP0) ||
			!m_code.EmitAndImm8(HOST_TMP1, HOST_TMP0, 7, true))
		{
			return false;
		}

		unaligned_fallback = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
		if (unaligned_fallback == static_cast<size_t>(-1))
			return false;

		if (!EmitVtlbNonHandlerHostAddress(HOST_TMP0, HOST_TMP1, HOST_TMP2, &handler_fallback) ||
			!EmitLoadGpr64(rt, HOST_TMP1, HOST_TMP2) ||
			!m_code.EmitStrImm12(HOST_TMP1, HOST_TMP0, 0) ||
			!m_code.EmitStrImm12(HOST_TMP2, HOST_TMP0, sizeof(u32)))
		{
			return false;
		}

		const size_t done = m_code.EmitBranchPlaceholder();
		if (done == static_cast<size_t>(-1))
			return false;

		const size_t fallback_target = m_code.Size();
		if (!EmitLoadGpr64(rt, HOST_TMP1, HOST_TMP2) ||
			!m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&VitaEeMemWrite64Checked)))
		{
			return false;
		}

		return m_code.PatchBranch(unaligned_fallback, fallback_target, VitaA32::Condition::NE) &&
			   m_code.PatchBranch(handler_fallback, fallback_target, VitaA32::Condition::MI) &&
			   m_code.PatchBranch(done, m_code.Size());
	}

	bool BlockCompiler::EmitSDL(u32 op)
	{
		return EmitPartialDwordStore(op, true);
	}

	bool BlockCompiler::EmitSDR(u32 op)
	{
		return EmitPartialDwordStore(op, false);
	}

	bool BlockCompiler::EmitSQ(u32 op)
	{
		const unsigned rt = RT(op);
		constexpr unsigned NEON_VALUE = 0;

		size_t handler_fallback = static_cast<size_t>(-1);
		if (!EmitEffectiveAddress(op, HOST_TMP0) ||
			!m_code.EmitMovRegShiftImm(HOST_TMP5, HOST_TMP0, VitaA32::ShiftType::LSL, 0) ||
			!EmitAlignQwordAddress(HOST_TMP0, HOST_TMP1) ||
			!EmitVtlbNonHandlerHostAddress128(HOST_TMP0, HOST_TMP1, HOST_TMP2, &handler_fallback) ||
			!EmitCpuRegsAddress(HOST_TMP1, GprOffset(rt)) ||
			!m_code.EmitVld1Q32(NEON_VALUE, HOST_TMP1) ||
			!m_code.EmitVst1Q32(NEON_VALUE, HOST_TMP0))
		{
			return false;
		}

		const size_t done = m_code.EmitBranchPlaceholder();
		if (done == static_cast<size_t>(-1))
			return false;

		const size_t fallback_target = m_code.Size();
		if (!m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_TMP5, VitaA32::ShiftType::LSL, 0) ||
			!m_code.EmitMovImm8(HOST_TMP1, static_cast<u8>(rt)) ||
			!m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&VitaEeMemWrite128Aligned)))
		{
			return false;
		}

		return m_code.PatchBranch(handler_fallback, fallback_target, VitaA32::Condition::MI) &&
			   m_code.PatchBranch(done, m_code.Size());
	}

	bool BlockCompiler::EmitSWC1(u32 op)
	{
		const unsigned rt = RT(op);

		size_t unaligned_fallback = static_cast<size_t>(-1);
		size_t handler_fallback = static_cast<size_t>(-1);
		if (!EmitEffectiveAddress(op, HOST_TMP0) ||
			!m_code.EmitMovRegShiftImm(HOST_TMP5, HOST_TMP0, VitaA32::ShiftType::LSL, 0) ||
			!m_code.EmitAndImm8(HOST_TMP1, HOST_TMP0, 3, true))
		{
			return false;
		}

		unaligned_fallback = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
		if (unaligned_fallback == static_cast<size_t>(-1))
			return false;

		if (!EmitVtlbNonHandlerHostAddress(HOST_TMP0, HOST_TMP1, HOST_TMP2, &handler_fallback) ||
			!m_code.EmitLdrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(FprOffset(rt))) ||
			!m_code.EmitStrImm12(HOST_TMP1, HOST_TMP0, 0))
		{
			return false;
		}

		const size_t done = m_code.EmitBranchPlaceholder();
		if (done == static_cast<size_t>(-1))
			return false;

		const size_t fallback_target = m_code.Size();
		if (!m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_TMP5, VitaA32::ShiftType::LSL, 0) ||
			!m_code.EmitMovImm8(HOST_TMP1, static_cast<u8>(rt)) ||
			!m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&VitaEeMemWriteCop1Word)))
		{
			return false;
		}

		return m_code.PatchBranch(unaligned_fallback, fallback_target, VitaA32::Condition::NE) &&
			   m_code.PatchBranch(handler_fallback, fallback_target, VitaA32::Condition::MI) &&
			   m_code.PatchBranch(done, m_code.Size());
	}

	bool BlockCompiler::EmitSQC2(u32 op)
	{
		const unsigned rt = RT(op);
		constexpr unsigned NEON_VALUE = 0;

		size_t handler_fallback = static_cast<size_t>(-1);
		if (!EmitEffectiveAddress(op, HOST_TMP0) ||
			!m_code.EmitMovRegShiftImm(HOST_TMP5, HOST_TMP0, VitaA32::ShiftType::LSL, 0) ||
			!EmitVtlbNonHandlerHostAddress128(HOST_TMP0, HOST_TMP1, HOST_TMP2, &handler_fallback) ||
			!m_code.EmitMovRegShiftImm(HOST_TMP5, HOST_TMP0, VitaA32::ShiftType::LSL, 0) ||
			!m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&vu0Sync)) ||
			!EmitVu0VfAddress(HOST_TMP0, rt) ||
			!m_code.EmitVld1Q32(NEON_VALUE, HOST_TMP0) ||
			!m_code.EmitVst1Q32(NEON_VALUE, HOST_TMP5))
		{
			return false;
		}

		const size_t done = m_code.EmitBranchPlaceholder();
		if (done == static_cast<size_t>(-1))
			return false;

		const size_t fallback_target = m_code.Size();
		if (!m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_TMP5, VitaA32::ShiftType::LSL, 0) ||
			!m_code.EmitMovImm8(HOST_TMP1, static_cast<u8>(rt)) ||
			!m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&VitaEeMemWriteVu0Quad)))
		{
			return false;
		}

		return m_code.PatchBranch(handler_fallback, fallback_target, VitaA32::Condition::MI) &&
			   m_code.PatchBranch(done, m_code.Size());
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

	bool BlockCompiler::EmitCop1Branch(u32 op)
	{
		// PCSX2 owner: FPU.cpp::BC1F()/BC1T()/BC1FL()/BC1TL() branch on
		// FCR31.C; x86/iFPU.cpp::REC_FPUBRANCH() flushes FPU state before
		// entering the shared branch path. Vita helper-backed scalar COP1 ops
		// already commit through fpuRegs before a BC1 block observes FCR31.
		const unsigned rt = RT(op);
		const bool branch_on_true = rt == 0x01 || rt == 0x03;
		const u32 fcr31_addr = static_cast<u32>(reinterpret_cast<uptr>(&fpuRegs.fprc[31]));

		return m_code.EmitMovImm32(HOST_TMP0, fcr31_addr) &&
			   m_code.EmitLdrImm12(HOST_TMP1, HOST_TMP0, 0) &&
			   m_code.EmitMovImm32(HOST_TMP2, FPU_FCR31_CONDITION_FLAG) &&
			   m_code.EmitAndReg(HOST_TMP1, HOST_TMP1, HOST_TMP2, true) &&
			   m_code.EmitMovImm8(HOST_BRANCH_FLAG, 0) &&
			   m_code.EmitMovImm8(HOST_BRANCH_FLAG, 1,
				   branch_on_true ? VitaA32::Condition::NE : VitaA32::Condition::EQ);
	}

	bool BlockCompiler::EmitCop0Branch(u32 op)
	{
		// PCSX2 owners: COP0.cpp::CPCOND0()/BC0F()/BC0T()/BC0FL()/BC0TL() and
		// x86/iCOP0.cpp::_setupBranchTest(). The branch condition is:
		// (((DMAC_STAT.CIS | ~DMAC_PCR.CPC) & 0x3ff) == 0x3ff).
		const unsigned rt = RT(op);
		const bool branch_on_true = rt == 0x01 || rt == 0x03;
		const u32 dmac_regs_addr = static_cast<u32>(reinterpret_cast<uptr>(&eeHw[DMAC_REGS_HW_OFFSET]));

		return m_code.EmitMovImm32(HOST_TMP0, dmac_regs_addr) &&
			   m_code.EmitLdrImm12(HOST_TMP1, HOST_TMP0, DMAC_PCR_DMAC_OFFSET) &&
			   m_code.EmitMvnReg(HOST_TMP1, HOST_TMP1) &&
			   m_code.EmitLdrImm12(HOST_TMP2, HOST_TMP0, DMAC_STAT_DMAC_OFFSET) &&
			   m_code.EmitOrrReg(HOST_TMP1, HOST_TMP1, HOST_TMP2) &&
			   m_code.EmitMovImm32(HOST_TMP2, DMAC_CPCOND_MASK) &&
			   m_code.EmitAndReg(HOST_TMP1, HOST_TMP1, HOST_TMP2) &&
			   m_code.EmitCmpReg(HOST_TMP1, HOST_TMP2) &&
			   m_code.EmitMovImm8(HOST_BRANCH_FLAG, 0) &&
			   m_code.EmitMovImm8(HOST_BRANCH_FLAG, 1,
				   branch_on_true ? VitaA32::Condition::EQ : VitaA32::Condition::NE);
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

	bool BlockCompiler::EmitPartialWordLoad(u32 op, bool left)
	{
		// PCSX2 owners: R5900OpcodeImpl.cpp::LWL() / LWR().
		const unsigned rt = RT(op);

		size_t handler_fallback = static_cast<size_t>(-1);
		if (!EmitEffectiveAddress(op, HOST_TMP0) ||
			!m_code.EmitMovRegShiftImm(HOST_TMP5, HOST_TMP0, VitaA32::ShiftType::LSL, 0) ||
			!m_code.EmitAndImm8(HOST_TMP3, HOST_TMP0, 3) ||
			!m_code.EmitMovRegShiftImm(HOST_TMP3, HOST_TMP3, VitaA32::ShiftType::LSL, 3))
		{
			return false;
		}

		if (left &&
			(!m_code.EmitMovImm8(HOST_TMP4, 24) ||
			 !m_code.EmitSubReg(HOST_TMP3, HOST_TMP4, HOST_TMP3)))
		{
			return false;
		}

		if (!m_code.EmitMovImm8(HOST_TMP4, 3) ||
			!m_code.EmitMvnReg(HOST_TMP4, HOST_TMP4) ||
			!m_code.EmitAndReg(HOST_TMP0, HOST_TMP0, HOST_TMP4) ||
			!EmitVtlbNonHandlerHostAddress(HOST_TMP0, HOST_TMP1, HOST_TMP2, &handler_fallback) ||
			!m_code.EmitLdrImm12(HOST_TMP0, HOST_TMP0, 0))
		{
			return false;
		}

		if (rt != 0)
		{
			if (left)
			{
				if (!EmitLoadGprLow(rt, HOST_TMP1) ||
					!m_code.EmitMovImm8(HOST_TMP4, 32) ||
					!m_code.EmitSubReg(HOST_TMP4, HOST_TMP4, HOST_TMP3) ||
					!m_code.EmitMovImm8(HOST_TMP2, 0) ||
					!m_code.EmitMvnReg(HOST_TMP2, HOST_TMP2) ||
					!m_code.EmitMovRegShiftReg(HOST_TMP2, HOST_TMP2, VitaA32::ShiftType::LSR, HOST_TMP4) ||
					!m_code.EmitAndReg(HOST_TMP1, HOST_TMP1, HOST_TMP2) ||
					!m_code.EmitMovRegShiftReg(HOST_TMP0, HOST_TMP0, VitaA32::ShiftType::LSL, HOST_TMP3) ||
					!m_code.EmitOrrReg(HOST_TMP0, HOST_TMP0, HOST_TMP1) ||
					!m_code.EmitMovRegShiftImm(HOST_TMP1, HOST_TMP0, VitaA32::ShiftType::ASR, 31) ||
					!EmitStoreGpr64(rt, HOST_TMP0, HOST_TMP1))
				{
					return false;
				}
			}
			else
			{
				if (!EmitLoadGprLow(rt, HOST_TMP1) ||
					!EmitLoadGprHigh(rt, HOST_TMP2) ||
					!m_code.EmitMovImm8(HOST_TMP4, 32) ||
					!m_code.EmitSubReg(HOST_TMP4, HOST_TMP4, HOST_TMP3) ||
					!m_code.EmitMovImm8(HOST_TMP5, 0) ||
					!m_code.EmitMvnReg(HOST_TMP5, HOST_TMP5) ||
					!m_code.EmitMovRegShiftReg(HOST_TMP5, HOST_TMP5, VitaA32::ShiftType::LSL, HOST_TMP4) ||
					!m_code.EmitAndReg(HOST_TMP1, HOST_TMP1, HOST_TMP5) ||
					!m_code.EmitMovRegShiftReg(HOST_TMP0, HOST_TMP0, VitaA32::ShiftType::LSR, HOST_TMP3) ||
					!m_code.EmitOrrReg(HOST_TMP0, HOST_TMP0, HOST_TMP1) ||
					!m_code.EmitMovImm8(HOST_TMP4, 0) ||
					!m_code.EmitCmpReg(HOST_TMP3, HOST_TMP4) ||
					!m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP0, VitaA32::ShiftType::ASR, 31, false,
						VitaA32::Condition::EQ) ||
					!EmitStoreGpr64(rt, HOST_TMP0, HOST_TMP2))
				{
					return false;
				}
			}
		}

		const size_t done = m_code.EmitBranchPlaceholder();
		if (done == static_cast<size_t>(-1))
			return false;

		const size_t fallback_target = m_code.Size();
		const void* fallback_helper = left ? reinterpret_cast<const void*>(&VitaEeMemReadWordLeft) :
											 reinterpret_cast<const void*>(&VitaEeMemReadWordRight);
		if (!m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_TMP5, VitaA32::ShiftType::LSL, 0) ||
			!m_code.EmitMovImm8(HOST_TMP1, static_cast<u8>(rt)) ||
			!m_code.EmitCallAbsolute(fallback_helper))
		{
			return false;
		}

		return m_code.PatchBranch(handler_fallback, fallback_target, VitaA32::Condition::MI) &&
			   m_code.PatchBranch(done, m_code.Size());
	}

	bool BlockCompiler::EmitPartialWordStore(u32 op, bool left)
	{
		// PCSX2 owners: R5900OpcodeImpl.cpp::SWL() / SWR().
		const unsigned rt = RT(op);

		size_t handler_fallback = static_cast<size_t>(-1);
		if (!EmitEffectiveAddress(op, HOST_TMP0) ||
			!m_code.EmitMovRegShiftImm(HOST_TMP5, HOST_TMP0, VitaA32::ShiftType::LSL, 0) ||
			!m_code.EmitAndImm8(HOST_TMP3, HOST_TMP0, 3) ||
			!m_code.EmitMovRegShiftImm(HOST_TMP3, HOST_TMP3, VitaA32::ShiftType::LSL, 3))
		{
			return false;
		}

		if (left &&
			(!m_code.EmitMovImm8(HOST_TMP4, 24) ||
			 !m_code.EmitSubReg(HOST_TMP3, HOST_TMP4, HOST_TMP3)))
		{
			return false;
		}

		if (!m_code.EmitMovImm8(HOST_TMP4, 3) ||
			!m_code.EmitMvnReg(HOST_TMP4, HOST_TMP4) ||
			!m_code.EmitAndReg(HOST_TMP0, HOST_TMP0, HOST_TMP4) ||
			!EmitVtlbNonHandlerHostAddress(HOST_TMP0, HOST_TMP1, HOST_TMP2, &handler_fallback) ||
			!m_code.EmitMovRegShiftImm(HOST_TMP5, HOST_TMP0, VitaA32::ShiftType::LSL, 0) ||
			!m_code.EmitLdrImm12(HOST_TMP0, HOST_TMP5, 0) ||
			!m_code.EmitLdrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(GprOffset(rt))) ||
			!m_code.EmitMovRegShiftReg(HOST_TMP1, HOST_TMP1,
				left ? VitaA32::ShiftType::LSR : VitaA32::ShiftType::LSL, HOST_TMP3) ||
			!m_code.EmitMovImm8(HOST_TMP4, 32) ||
			!m_code.EmitSubReg(HOST_TMP4, HOST_TMP4, HOST_TMP3) ||
			!m_code.EmitMovImm8(HOST_TMP2, 0) ||
			!m_code.EmitMvnReg(HOST_TMP2, HOST_TMP2) ||
			!m_code.EmitMovRegShiftReg(HOST_TMP2, HOST_TMP2,
				left ? VitaA32::ShiftType::LSL : VitaA32::ShiftType::LSR, HOST_TMP4) ||
			!m_code.EmitAndReg(HOST_TMP0, HOST_TMP0, HOST_TMP2) ||
			!m_code.EmitOrrReg(HOST_TMP0, HOST_TMP0, HOST_TMP1) ||
			!m_code.EmitStrImm12(HOST_TMP0, HOST_TMP5, 0))
		{
			return false;
		}

		const size_t done = m_code.EmitBranchPlaceholder();
		if (done == static_cast<size_t>(-1))
			return false;

		const size_t fallback_target = m_code.Size();
		const void* fallback_helper = left ? reinterpret_cast<const void*>(&VitaEeMemWriteWordLeft) :
											 reinterpret_cast<const void*>(&VitaEeMemWriteWordRight);
		if (!m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_TMP5, VitaA32::ShiftType::LSL, 0) ||
			!m_code.EmitMovImm8(HOST_TMP1, static_cast<u8>(rt)) ||
			!m_code.EmitCallAbsolute(fallback_helper))
		{
			return false;
		}

		return m_code.PatchBranch(handler_fallback, fallback_target, VitaA32::Condition::MI) &&
			   m_code.PatchBranch(done, m_code.Size());
	}

	bool BlockCompiler::EmitPartialDwordLoad(u32 op, bool left)
	{
		// PCSX2 owners: R5900OpcodeImpl.cpp::LDL() / LDR().
		const unsigned rt = RT(op);

		size_t handler_fallback = static_cast<size_t>(-1);
		if (!EmitEffectiveAddress(op, HOST_TMP0) ||
			!m_code.EmitMovRegShiftImm(HOST_TMP5, HOST_TMP0, VitaA32::ShiftType::LSL, 0) ||
			!m_code.EmitAndImm8(HOST_TMP3, HOST_TMP0, 7) ||
			!m_code.EmitMovImm8(HOST_TMP4, 7) ||
			!m_code.EmitMvnReg(HOST_TMP4, HOST_TMP4) ||
			!m_code.EmitAndReg(HOST_TMP0, HOST_TMP0, HOST_TMP4) ||
			!EmitVtlbNonHandlerHostAddress(HOST_TMP0, HOST_TMP1, HOST_TMP2, &handler_fallback))
		{
			return false;
		}

		const auto emit_byte = [this, rt](unsigned memory_byte, unsigned dest_byte) {
			return m_code.EmitLdrbImm12(HOST_TMP1, HOST_TMP0, static_cast<u16>(memory_byte)) &&
				   m_code.EmitStrbImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(GprOffset(rt) + dest_byte));
		};

		const auto emit_case_body = [&](unsigned shift) {
			if (rt == 0)
				return true;

			if (left)
			{
				for (unsigned memory_byte = 0; memory_byte <= shift; memory_byte++)
				{
					const unsigned dest_byte = 7 - shift + memory_byte;
					if (!emit_byte(memory_byte, dest_byte))
						return false;
				}
			}
			else
			{
				for (unsigned memory_byte = shift; memory_byte < 8; memory_byte++)
				{
					const unsigned dest_byte = memory_byte - shift;
					if (!emit_byte(memory_byte, dest_byte))
						return false;
				}
			}

			return true;
		};

		size_t case_branches[7]{};
		for (unsigned shift = 0; shift < 7; shift++)
		{
			if (!m_code.EmitMovImm8(HOST_TMP4, static_cast<u8>(shift)) ||
				!m_code.EmitCmpReg(HOST_TMP3, HOST_TMP4))
			{
				return false;
			}

			case_branches[shift] = m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
			if (case_branches[shift] == static_cast<size_t>(-1))
				return false;
		}

		size_t done_branches[8]{};
		if (!emit_case_body(7))
			return false;

		done_branches[7] = m_code.EmitBranchPlaceholder();
		if (done_branches[7] == static_cast<size_t>(-1))
			return false;

		for (unsigned shift = 0; shift < 7; shift++)
		{
			const size_t case_target = m_code.Size();
			if (!m_code.PatchBranch(case_branches[shift], case_target, VitaA32::Condition::EQ) ||
				!emit_case_body(shift))
			{
				return false;
			}

			done_branches[shift] = m_code.EmitBranchPlaceholder();
			if (done_branches[shift] == static_cast<size_t>(-1))
				return false;
		}

		const size_t fallback_target = m_code.Size();
		const void* fallback_helper = left ? reinterpret_cast<const void*>(&VitaEeMemReadDwordLeft) :
											 reinterpret_cast<const void*>(&VitaEeMemReadDwordRight);
		if (!m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_TMP5, VitaA32::ShiftType::LSL, 0) ||
			!m_code.EmitMovImm8(HOST_TMP1, static_cast<u8>(rt)) ||
			!m_code.EmitCallAbsolute(fallback_helper))
		{
			return false;
		}

		const size_t done_target = m_code.Size();
		if (!m_code.PatchBranch(handler_fallback, fallback_target, VitaA32::Condition::MI))
			return false;

		for (size_t branch : done_branches)
		{
			if (!m_code.PatchBranch(branch, done_target))
				return false;
		}

		return true;
	}

	bool BlockCompiler::EmitPartialDwordStore(u32 op, bool left)
	{
		// PCSX2 owners: R5900OpcodeImpl.cpp::SDL() / SDR().
		const unsigned rt = RT(op);

		size_t handler_fallback = static_cast<size_t>(-1);
		if (!EmitEffectiveAddress(op, HOST_TMP0) ||
			!m_code.EmitMovRegShiftImm(HOST_TMP5, HOST_TMP0, VitaA32::ShiftType::LSL, 0) ||
			!m_code.EmitAndImm8(HOST_TMP3, HOST_TMP0, 7) ||
			!m_code.EmitMovImm8(HOST_TMP4, 7) ||
			!m_code.EmitMvnReg(HOST_TMP4, HOST_TMP4) ||
			!m_code.EmitAndReg(HOST_TMP0, HOST_TMP0, HOST_TMP4) ||
			!EmitVtlbNonHandlerHostAddress(HOST_TMP0, HOST_TMP1, HOST_TMP2, &handler_fallback))
		{
			return false;
		}

		const auto emit_byte = [this, rt](unsigned source_byte, unsigned memory_byte) {
			return m_code.EmitLdrbImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(GprOffset(rt) + source_byte)) &&
				   m_code.EmitStrbImm12(HOST_TMP1, HOST_TMP0, static_cast<u16>(memory_byte));
		};

		const auto emit_case_body = [&](unsigned shift) {
			if (left)
			{
				for (unsigned memory_byte = 0; memory_byte <= shift; memory_byte++)
				{
					const unsigned source_byte = 7 - shift + memory_byte;
					if (!emit_byte(source_byte, memory_byte))
						return false;
				}
			}
			else
			{
				for (unsigned memory_byte = shift; memory_byte < 8; memory_byte++)
				{
					const unsigned source_byte = memory_byte - shift;
					if (!emit_byte(source_byte, memory_byte))
						return false;
				}
			}

			return true;
		};

		size_t case_branches[7]{};
		for (unsigned shift = 0; shift < 7; shift++)
		{
			if (!m_code.EmitMovImm8(HOST_TMP4, static_cast<u8>(shift)) ||
				!m_code.EmitCmpReg(HOST_TMP3, HOST_TMP4))
			{
				return false;
			}

			case_branches[shift] = m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
			if (case_branches[shift] == static_cast<size_t>(-1))
				return false;
		}

		size_t done_branches[8]{};
		if (!emit_case_body(7))
			return false;

		done_branches[7] = m_code.EmitBranchPlaceholder();
		if (done_branches[7] == static_cast<size_t>(-1))
			return false;

		for (unsigned shift = 0; shift < 7; shift++)
		{
			const size_t case_target = m_code.Size();
			if (!m_code.PatchBranch(case_branches[shift], case_target, VitaA32::Condition::EQ) ||
				!emit_case_body(shift))
			{
				return false;
			}

			done_branches[shift] = m_code.EmitBranchPlaceholder();
			if (done_branches[shift] == static_cast<size_t>(-1))
				return false;
		}

		const size_t fallback_target = m_code.Size();
		const void* fallback_helper = left ? reinterpret_cast<const void*>(&VitaEeMemWriteDwordLeft) :
											 reinterpret_cast<const void*>(&VitaEeMemWriteDwordRight);
		if (!m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_TMP5, VitaA32::ShiftType::LSL, 0) ||
			!m_code.EmitMovImm8(HOST_TMP1, static_cast<u8>(rt)) ||
			!m_code.EmitCallAbsolute(fallback_helper))
		{
			return false;
		}

		const size_t done_target = m_code.Size();
		if (!m_code.PatchBranch(handler_fallback, fallback_target, VitaA32::Condition::MI))
			return false;

		for (size_t branch : done_branches)
		{
			if (!m_code.PatchBranch(branch, done_target))
				return false;
		}

		return true;
	}

	bool BlockCompiler::EmitLoadWithCounterReadEvent(u32 op, u32 pc, u32 raw_cycles_through_instruction,
		const void* event_exit, const void* read_helper, bool sign_extend, unsigned sign_shift,
		bool branch_delay_slot, ScalarLoadWidth width, u8 alignment_mask)
	{
		const unsigned rt = RT(op);

		size_t unaligned_fallback = static_cast<size_t>(-1);
		size_t handler_fallback = static_cast<size_t>(-1);
		if (!EmitEffectiveAddress(op, HOST_TMP0) ||
			(branch_delay_slot &&
			 !m_code.EmitMovRegShiftImm(HOST_TMP5, HOST_BRANCH_FLAG, VitaA32::ShiftType::LSL, 0)) ||
			!EmitCounterReadFlagFromAddress(HOST_TMP0))
		{
			return false;
		}

		if (alignment_mask != 0)
		{
			if (!m_code.EmitAndImm8(HOST_TMP1, HOST_TMP0, alignment_mask, true))
				return false;

			unaligned_fallback = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (unaligned_fallback == static_cast<size_t>(-1))
				return false;
		}

		if (!EmitVtlbNonHandlerHostAddress(HOST_TMP0, HOST_TMP1, HOST_TMP2, &handler_fallback))
			return false;

		switch (width)
		{
			case ScalarLoadWidth::Byte:
				if (!m_code.EmitLdrbImm12(HOST_TMP0, HOST_TMP0, 0))
					return false;
				break;

			case ScalarLoadWidth::Halfword:
				if (!m_code.EmitLdrhImm8(HOST_TMP0, HOST_TMP0, 0))
					return false;
				break;
		}

		const size_t value_ready = m_code.EmitBranchPlaceholder();
		if (value_ready == static_cast<size_t>(-1))
			return false;

		const size_t fallback_target = m_code.Size();
		if (!m_code.EmitCallAbsolute(read_helper))
			return false;

		const size_t value_ready_target = m_code.Size();
		if (rt == 0)
		{
			if (branch_delay_slot &&
				!m_code.EmitMovRegShiftImm(HOST_BRANCH_FLAG, HOST_TMP5, VitaA32::ShiftType::LSL, 0))
			{
				return false;
			}

			return (unaligned_fallback == static_cast<size_t>(-1) ||
					   m_code.PatchBranch(unaligned_fallback, fallback_target, VitaA32::Condition::NE)) &&
				   m_code.PatchBranch(handler_fallback, fallback_target, VitaA32::Condition::MI) &&
				   m_code.PatchBranch(value_ready, value_ready_target);
		}

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
			   EmitCounterReadEventExit(pc + 4, raw_cycles_through_instruction, event_exit) &&
			   (!branch_delay_slot ||
				   m_code.EmitMovRegShiftImm(HOST_BRANCH_FLAG, HOST_TMP5, VitaA32::ShiftType::LSL, 0)) &&
			   (unaligned_fallback == static_cast<size_t>(-1) ||
				   m_code.PatchBranch(unaligned_fallback, fallback_target, VitaA32::Condition::NE)) &&
			   m_code.PatchBranch(handler_fallback, fallback_target, VitaA32::Condition::MI) &&
			   m_code.PatchBranch(value_ready, value_ready_target);
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
			!m_code.EmitPop(REG_R4 | REG_R5 | REG_R6 | REG_PC))
		{
			return false;
		}

		return m_code.PatchBranch(not_counter_read, m_code.Size(), VitaA32::Condition::EQ);
	}

	bool BlockCompiler::EmitSystemHelperEventExit(u32 op, u32 next_pc, u32 raw_cycles_through_instruction,
		const void* helper, const void* event_exit, bool request_cache_reset)
	{
		if (!helper || !event_exit || raw_cycles_through_instruction == 0)
			return false;

		const u32 cycles = ScaleBlockCycles(raw_cycles_through_instruction);
		if (!m_code.EmitMovImm32(HOST_TMP0, op) ||
			!m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(CODE_OFFSET)) ||
			!EmitStorePc(next_pc) ||
			!EmitAddScaledCyclesToCpu(cycles) ||
			!m_code.EmitCallAbsolute(helper))
		{
			return false;
		}

		if (request_cache_reset &&
			!m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&VitaRequestA32EeCacheReset)))
		{
			return false;
		}

		return m_code.EmitCallAbsolute(event_exit) &&
			   m_code.EmitPop(REG_R4 | REG_R5 | REG_R6 | REG_PC);
	}

	bool BlockCompiler::EmitGsTracePreInstruction(u32 pc)
	{
#if defined(VITASX2_QEMU_PROVIDER_FIXTURE)
		return true;
#else
		if (!Pcsx2Trace::IsGsTraceEnabled())
			return true;

		return m_code.EmitMovImm32(HOST_TMP0, pc) &&
			   m_code.EmitCallAbsolute(
				   reinterpret_cast<const void*>(
					   static_cast<bool (*)(u32)>(&Pcsx2Trace::RecordGsPreEeInstruction)));
#endif
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

	bool BlockCompiler::EmitCpuRegsAddress(unsigned host_reg, size_t offset)
	{
		if (offset <= 255)
			return m_code.EmitAddImm8(host_reg, HOST_CPU_REGS, static_cast<u8>(offset));

		return m_code.EmitMovImm32(host_reg, static_cast<u32>(offset)) &&
			   m_code.EmitAddReg(host_reg, HOST_CPU_REGS, host_reg);
	}

	bool BlockCompiler::EmitVu0VfAddress(unsigned host_reg, unsigned vf_reg)
	{
		return m_code.EmitMovImm32(host_reg,
			static_cast<u32>(reinterpret_cast<uptr>(&VU0.VF[vf_reg])));
	}

	bool BlockCompiler::EmitAlignQwordAddress(unsigned host_reg, unsigned scratch_reg)
	{
		return m_code.EmitMovImm8(scratch_reg, 0x0f) &&
			   m_code.EmitMvnReg(scratch_reg, scratch_reg) &&
			   m_code.EmitAndReg(host_reg, host_reg, scratch_reg);
	}

	bool BlockCompiler::EmitVtlbNonHandlerHostAddress(unsigned host_reg, unsigned vmap_reg,
		unsigned scratch_reg, size_t* handler_fallback_branch)
	{
		// PCSX2 owner: vtlb.cpp::vtlb_memRead*() / vtlb_memWrite*().
		// Fast path only mirrors the non-handler VTLB case. Handler-backed
		// cache/MMIO/unmapped pages branch to the existing PCSX2 helper path.
		constexpr u8 VTLB_VIRTUAL_ENTRY_SHIFT = 2;
		static_assert((sizeof(vtlb_private::VTLBVirtual) >> VTLB_VIRTUAL_ENTRY_SHIFT) == 1);

		if (!m_code.EmitMovImm32(vmap_reg,
				static_cast<u32>(reinterpret_cast<uptr>(&vtlb_private::vtlbdata.vmap))) ||
			!m_code.EmitLdrImm12(vmap_reg, vmap_reg, 0) ||
			!m_code.EmitMovRegShiftImm(scratch_reg, host_reg, VitaA32::ShiftType::LSR,
				vtlb_private::VTLB_PAGE_BITS) ||
			!m_code.EmitMovRegShiftImm(scratch_reg, scratch_reg, VitaA32::ShiftType::LSL,
				VTLB_VIRTUAL_ENTRY_SHIFT) ||
			!m_code.EmitAddReg(vmap_reg, vmap_reg, scratch_reg) ||
			!m_code.EmitLdrImm12(vmap_reg, vmap_reg, 0) ||
			!m_code.EmitAddReg(vmap_reg, vmap_reg, host_reg, true))
		{
			return false;
		}

		*handler_fallback_branch = m_code.EmitBranchPlaceholder(VitaA32::Condition::MI);
		if (*handler_fallback_branch == static_cast<size_t>(-1))
			return false;

		return m_code.EmitMovImm32(scratch_reg,
				   static_cast<u32>(reinterpret_cast<uptr>(&vtlb_private::vtlbdata.host_memory_base))) &&
			   m_code.EmitLdrImm12(scratch_reg, scratch_reg, 0) &&
			   m_code.EmitAddReg(host_reg, vmap_reg, scratch_reg);
	}

	bool BlockCompiler::EmitVtlbNonHandlerHostAddress128(unsigned host_reg, unsigned vmap_reg,
		unsigned scratch_reg, size_t* handler_fallback_branch)
	{
		return EmitVtlbNonHandlerHostAddress(host_reg, vmap_reg, scratch_reg, handler_fallback_branch);
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
