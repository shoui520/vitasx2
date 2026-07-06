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
#include "pcsx2/DebugTools/GsTrace.h"
#include "pcsx2/DebugTools/VuTrace.h"
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
#include <type_traits>

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
u32 g_qemuCpuCancelInstructionCalls = 0;
u32 g_qemuVu0SyncCalls = 0;
u32 g_qemuGprPinnedBlocks = 0;
u32 g_qemuGprPinnedRegisters = 0;
u32 g_qemuGprPinnedDwordRegisters = 0;
u32 g_qemuGprPinnedVtlbFreeHostRegisters = 0;
u32 g_qemuGprPinnedHighWordHits = 0;
u32 g_qemuGprQCacheBlocks = 0;
u32 g_qemuGprQCacheHits = 0;
u32 g_qemuGprQCacheMisses = 0;
u32 g_qemuGprQCacheWordHits = 0;
u32 g_qemuGprQCacheDwordHits = 0;
u32 g_qemuMmiFiveBitVectorOps = 0;
u32 g_qemuMmiVariableWordShiftVectorOps = 0;
u32 g_qemuMmiPackedWordMultiplyVectorOps = 0;
u32 g_qemuMmiPackedWordMultiplyAddVectorOps = 0;
u32 g_qemuMmiPackedWordDivideVectorOps = 0;
u32 g_qemuMmiPackedHalfwordMultiplyAccumulateVectorOps = 0;
u32 g_qemuMmiPackedHalfwordPairMultiplyVectorOps = 0;
u32 g_qemuMmiPackedHalfwordMultiplyVectorOps = 0;
u32 g_qemuMmiPackedWordByHalfwordDivideVectorOps = 0;
u32 g_qemuMmiHalfwordShuffleVectorOps = 0;
u32 g_qemuMmiWordShuffleVectorOps = 0;
#endif

namespace VitaEE
{
	namespace
	{
		constexpr u16 REG_R3 = 1u << 3;
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

		constexpr unsigned HOST_CPU_REGS = 4;
		constexpr unsigned HOST_BRANCH_STATE = 5;
		constexpr unsigned HOST_BRANCH_FLAG = HOST_BRANCH_STATE;
		constexpr unsigned HOST_BRANCH_TARGET = HOST_BRANCH_STATE;
		constexpr unsigned HOST_SP = 13;
		constexpr unsigned HOST_TMP0 = 0;
		constexpr unsigned HOST_TMP1 = 1;
		constexpr unsigned HOST_TMP2 = 2;
		constexpr unsigned HOST_TMP3 = 3;
		constexpr unsigned HOST_TMP4 = 12;
		constexpr unsigned HOST_TMP5 = 6;
		constexpr unsigned HOST_VTLB_VMAP = 7;
		constexpr unsigned HOST_VTLB_HOST_MEMORY_BASE = 8;
		constexpr unsigned HOST_GPR_PIN0 = 9;
		constexpr unsigned HOST_COP1_EXPONENT_MASK = 10;
		constexpr unsigned HOST_VU0_BASE = 11;
		constexpr u8 NO_GPR_PIN_HOST = 0xff;

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
		using Vu0State = std::remove_reference_t<decltype(VU0)>;
		constexpr size_t VU0_VF_OFFSET = offsetof(Vu0State, VF);
		constexpr size_t VU0_VI_OFFSET = offsetof(Vu0State, VI);
		constexpr size_t VU0_Q_OFFSET = offsetof(Vu0State, q);
		constexpr size_t VU0_STATUSFLAG_OFFSET = offsetof(Vu0State, statusflag);
#if !defined(VITASX2_QEMU_PROVIDER_FIXTURE)
		constexpr size_t VU0_MEM_OFFSET = offsetof(Vu0State, Mem);
		constexpr size_t VU0_CODE_OFFSET = offsetof(Vu0State, code);
		constexpr size_t VU0_VI_BACKUP_CYCLES_OFFSET = offsetof(Vu0State, VIBackupCycles);
		constexpr size_t VU0_VI_OLD_VALUE_OFFSET = offsetof(Vu0State, VIOldValue);
		constexpr size_t VU0_VI_REG_NUMBER_OFFSET = offsetof(Vu0State, VIRegNumber);
#endif
		constexpr size_t VU0_VF_STRIDE = sizeof(VU0.VF[0]);
		constexpr size_t VU0_VI_STRIDE = sizeof(VU0.VI[0]);
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
		constexpr u32 FPU_FLOAT_IMPLICIT_MANTISSA = 0x00800000;
		constexpr u32 FPU_FLOAT_ONE = 0x3f800000;
		constexpr u32 FPU_FLOAT_MAX_FINITE = 0x7f7fffff;
		constexpr u32 FPU_CVT_W_MAX_EXPONENT_MASK = 0x4e800000;
		constexpr u32 FPU_FLOAT_EXPONENT_BIAS = 127;
		constexpr u32 FPU_FLOAT_MANTISSA_BITS = 23;
		constexpr size_t DMAC_REGS_HW_OFFSET = 0xe000;
		constexpr u16 DMAC_STAT_DMAC_OFFSET = 0x10;
		constexpr u16 DMAC_PCR_DMAC_OFFSET = 0x20;
		constexpr u32 DMAC_CPCOND_MASK = 0x3ff;
		constexpr unsigned VU0_REG_STATUS_FLAG = 16;
		constexpr unsigned VU0_REG_MAC_FLAG = 17;
		constexpr unsigned VU0_REG_CLIP_FLAG = 18;
		constexpr unsigned VU0_REG_R = 20;
		constexpr unsigned VU0_REG_I = 21;
		constexpr unsigned VU0_REG_Q = 22;
		constexpr unsigned VU0_REG_TPC = 26;
		constexpr unsigned VU0_REG_FBRST = 28;
		constexpr unsigned VU0_REG_VPU_STAT = 29;
		constexpr unsigned VU0_REG_CMSAR1 = 31;

		alignas(16) GPR_reg s_lq_zero_sink;
		alignas(4) u32 s_raw_gpr0_known_zero = 1;

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

		struct Cop2MacroMinMaxOp
		{
			bool valid = false;
			bool take_max = false;
			bool vector_operand = false;
			bool immediate_operand = false;
			unsigned broadcast_lane = 0;
		};

		struct Cop2MacroMoveOp
		{
			bool valid = false;
			bool rotate32 = false;
		};

		enum class Cop2MacroViKind : u8
		{
			Add,
			Sub,
			AddImmediate,
			And,
			Or,
		};

		struct Cop2MacroViOp
		{
			bool valid = false;
			Cop2MacroViKind kind = Cop2MacroViKind::Add;
		};

		struct Cop2MacroViTransferOp
		{
			bool valid = false;
			bool vi_to_vf = false;
		};

		enum class Cop2MacroRandomKind : u8
		{
			WaitQ,
			RNext,
			RGet,
			RInit,
			RXor,
		};

		struct Cop2MacroRandomOp
		{
			bool valid = false;
			Cop2MacroRandomKind kind = Cop2MacroRandomKind::WaitQ;
		};

		struct Cop2MacroIndexedViMemoryOp
		{
			bool valid = false;
			bool load = false;
		};

		enum class Cop2MacroIndexedVectorMemoryKind : u8
		{
			LoadIncrement,
			StoreIncrement,
			LoadDecrement,
			StoreDecrement,
		};

		struct Cop2MacroIndexedVectorMemoryOp
		{
			bool valid = false;
			Cop2MacroIndexedVectorMemoryKind kind = Cop2MacroIndexedVectorMemoryKind::LoadIncrement;
		};

		struct Cop2MacroItofOp
		{
			bool valid = false;
			unsigned offset = 0;
		};

		struct Cop2MacroFtoiOp
		{
			bool valid = false;
			unsigned offset = 0;
		};

		enum class Cop2MacroFdivKind
		{
			Div,
			Sqrt,
			Rsqrt,
		};

		struct Cop2MacroFdivOp
		{
			bool valid = false;
			Cop2MacroFdivKind kind = Cop2MacroFdivKind::Div;
		};

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

		constexpr Cop2MacroMinMaxOp DecodeCop2MacroMinMax(u32 op)
		{
			const unsigned function = op & 0x3f;
			if (function >= 0x10 && function <= 0x13)
				return {true, true, false, false, function - 0x10};
			if (function >= 0x14 && function <= 0x17)
				return {true, false, false, false, function - 0x14};
			if (function == 0x1d)
				return {true, true, false, true, 0};
			if (function == 0x1f)
				return {true, false, false, true, 0};
			if (function == 0x2b)
				return {true, true, true, false, 0};
			if (function == 0x2f)
				return {true, false, true, false, 0};
			return {};
		}

		constexpr Cop2MacroMoveOp DecodeCop2MacroMove(u32 op)
		{
			if ((op & 0x3c) != 0x3c)
				return {};

			const u32 special2_index = (op & 0x3) | ((op >> 4) & 0x7c);
			if (special2_index == 0x30)
				return {true, false};
			if (special2_index == 0x31)
				return {true, true};
			return {};
		}

		constexpr Cop2MacroViOp DecodeCop2MacroVi(u32 op)
		{
			switch (op & 0x3f)
			{
				case 0x30:
					return {true, Cop2MacroViKind::Add};
				case 0x31:
					return {true, Cop2MacroViKind::Sub};
				case 0x32:
					return {true, Cop2MacroViKind::AddImmediate};
				case 0x34:
					return {true, Cop2MacroViKind::And};
				case 0x35:
					return {true, Cop2MacroViKind::Or};
				default:
					return {};
			}
		}

		constexpr Cop2MacroViTransferOp DecodeCop2MacroViTransfer(u32 op)
		{
			if ((op & 0x3c) != 0x3c)
				return {};

			const u32 special2_index = (op & 0x3) | ((op >> 4) & 0x7c);
			if (special2_index == 0x3c)
				return {true, false};
			if (special2_index == 0x3d)
				return {true, true};
			return {};
		}

		constexpr Cop2MacroRandomOp DecodeCop2MacroRandom(u32 op)
		{
			if ((op & 0x3c) != 0x3c)
				return {};

			const u32 special2_index = (op & 0x3) | ((op >> 4) & 0x7c);
			switch (special2_index)
			{
				case 0x3b:
					return {true, Cop2MacroRandomKind::WaitQ};
				case 0x40:
					return {true, Cop2MacroRandomKind::RNext};
				case 0x41:
					return {true, Cop2MacroRandomKind::RGet};
				case 0x42:
					return {true, Cop2MacroRandomKind::RInit};
				case 0x43:
					return {true, Cop2MacroRandomKind::RXor};
				default:
					return {};
			}
		}

		constexpr Cop2MacroIndexedViMemoryOp DecodeCop2MacroIndexedViMemory(u32 op)
		{
#if defined(VITASX2_QEMU_PROVIDER_FIXTURE)
			(void)op;
			return {};
#else
			if ((op & 0x3c) != 0x3c)
				return {};

			const u32 special2_index = (op & 0x3) | ((op >> 4) & 0x7c);
			if (special2_index == 0x3e)
				return {true, true};
			if (special2_index == 0x3f)
				return {true, false};
			return {};
#endif
		}

		constexpr Cop2MacroIndexedVectorMemoryOp DecodeCop2MacroIndexedVectorMemory(u32 op)
		{
#if defined(VITASX2_QEMU_PROVIDER_FIXTURE)
			(void)op;
			return {};
#else
			if ((op & 0x3c) != 0x3c)
				return {};

			const u32 special2_index = (op & 0x3) | ((op >> 4) & 0x7c);
			switch (special2_index)
			{
				case 0x34:
					return {true, Cop2MacroIndexedVectorMemoryKind::LoadIncrement};
				case 0x35:
					return {true, Cop2MacroIndexedVectorMemoryKind::StoreIncrement};
				case 0x36:
					return {true, Cop2MacroIndexedVectorMemoryKind::LoadDecrement};
				case 0x37:
					return {true, Cop2MacroIndexedVectorMemoryKind::StoreDecrement};
				default:
					return {};
			}
#endif
		}

		constexpr bool IsCop2MacroClip(u32 op)
		{
			if ((op & 0x3c) != 0x3c)
				return false;

			const u32 special2_index = (op & 0x3) | ((op >> 4) & 0x7c);
			return special2_index == 0x1f;
		}

		constexpr Cop2MacroItofOp DecodeCop2MacroItof(u32 op)
		{
			if ((op & 0x3c) != 0x3c)
				return {};

			const u32 special2_index = (op & 0x3) | ((op >> 4) & 0x7c);
			switch (special2_index)
			{
				case 0x10:
					return {true, 0};
				case 0x11:
					return {true, 4};
				case 0x12:
					return {true, 12};
				case 0x13:
					return {true, 15};
				default:
					return {};
			}
		}

		constexpr Cop2MacroFtoiOp DecodeCop2MacroFtoi(u32 op)
		{
			if ((op & 0x3c) != 0x3c)
				return {};

			const u32 special2_index = (op & 0x3) | ((op >> 4) & 0x7c);
			switch (special2_index)
			{
				case 0x14:
					return {true, 0};
				case 0x15:
					return {true, 4};
				case 0x16:
					return {true, 12};
				case 0x17:
					return {true, 15};
				default:
					return {};
			}
		}

		constexpr Cop2MacroFdivOp DecodeCop2MacroFdiv(u32 op)
		{
			if ((op & 0x3c) != 0x3c)
				return {};

			const u32 special2_index = (op & 0x3) | ((op >> 4) & 0x7c);
			switch (special2_index)
			{
				case 0x38:
					return {true, Cop2MacroFdivKind::Div};
				case 0x39:
					return {true, Cop2MacroFdivKind::Sqrt};
				case 0x3a:
					return {true, Cop2MacroFdivKind::Rsqrt};
				default:
					return {};
			}
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

		constexpr bool CanUseA32DualTransferPair(unsigned low_reg, unsigned high_reg)
		{
			return low_reg < 14 && high_reg == low_reg + 1 && ((low_reg & 1u) == 0);
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
				case 0x0c: // SYSCALL, owned by R5900OpcodeImpl.cpp::SYSCALL().
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
				case 0x28: // MFSA, owned by R5900OpcodeImpl.cpp::MFSA().
				case 0x29: // MTSA, owned by R5900OpcodeImpl.cpp::MTSA().
				case 0x2a: // SLT, owned by R5900OpcodeImpl.cpp::SLT().
				case 0x2b: // SLTU, owned by R5900OpcodeImpl.cpp::SLTU().
				case 0x2c: // DADD, owned by R5900OpcodeImpl.cpp::DADD().
				case 0x2d: // DADDU, owned by R5900OpcodeImpl.cpp::DADDU().
				case 0x2e: // DSUB, owned by R5900OpcodeImpl.cpp::DSUB().
				case 0x2f: // DSUBU, owned by R5900OpcodeImpl.cpp::DSUBU().
				case 0x30: // TGE, owned by R5900OpcodeImpl.cpp::TGE().
				case 0x31: // TGEU, owned by R5900OpcodeImpl.cpp::TGEU().
				case 0x32: // TLT, owned by R5900OpcodeImpl.cpp::TLT().
				case 0x33: // TLTU, owned by R5900OpcodeImpl.cpp::TLTU().
				case 0x34: // TEQ, owned by R5900OpcodeImpl.cpp::TEQ().
				case 0x36: // TNE, owned by R5900OpcodeImpl.cpp::TNE().
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

		bool IsSYSCALL(u32 op)
		{
			return (op >> 26) == 0x00 && (op & 0x3f) == 0x0c;
		}

		bool IsSYNC(u32 op)
		{
			return (op >> 26) == 0x00 && (op & 0x3f) == 0x0f;
		}

		bool IsSpecialTrap(u32 op)
		{
			if ((op >> 26) != 0x00)
				return false;

			switch (op & 0x3f)
			{
				case 0x30: // TGE
				case 0x31: // TGEU
				case 0x32: // TLT
				case 0x33: // TLTU
				case 0x34: // TEQ
				case 0x36: // TNE
					return true;
				default:
					return false;
			}
		}

		bool IsRegImmTrap(u32 op)
		{
			if ((op >> 26) != 0x01)
				return false;

			switch (RT(op))
			{
				case 0x08: // TGEI
				case 0x09: // TGEIU
				case 0x0a: // TLTI
				case 0x0b: // TLTIU
				case 0x0c: // TEQI
				case 0x0e: // TNEI
					return true;
				default:
					return false;
			}
		}

		bool IsTrapOpcode(u32 op)
		{
			return IsSpecialTrap(op) || IsRegImmTrap(op);
		}

		const void* TrapHelperForOpcode(u32 op)
		{
			using namespace R5900::Interpreter::OpcodeImpl;

			if ((op >> 26) == 0x00)
			{
				switch (op & 0x3f)
				{
					case 0x30: return reinterpret_cast<const void*>(&TGE);
					case 0x31: return reinterpret_cast<const void*>(&TGEU);
					case 0x32: return reinterpret_cast<const void*>(&TLT);
					case 0x33: return reinterpret_cast<const void*>(&TLTU);
					case 0x34: return reinterpret_cast<const void*>(&TEQ);
					case 0x36: return reinterpret_cast<const void*>(&TNE);
					default: return nullptr;
				}
			}

			if ((op >> 26) == 0x01)
			{
				switch (RT(op))
				{
					case 0x08: return reinterpret_cast<const void*>(&TGEI);
					case 0x09: return reinterpret_cast<const void*>(&TGEIU);
					case 0x0a: return reinterpret_cast<const void*>(&TLTI);
					case 0x0b: return reinterpret_cast<const void*>(&TLTIU);
					case 0x0c: return reinterpret_cast<const void*>(&TEQI);
					case 0x0e: return reinterpret_cast<const void*>(&TNEI);
					default: return nullptr;
				}
			}

			return nullptr;
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
		bool CanCompileCOP2(u32 op);

		bool IsFastMFC0(u32 op)
		{
			if ((op >> 26) != 0x10 || ((op >> 21) & 0x1f) != 0x00)
				return false;

			const unsigned rd = RD(op);
			// PCSX2 x86/iCOP0.cpp::recMFC0() keeps Count in-block by committing
			// cycles through scaleblockcycles_clear(). MFPS/PCCR stays in-block,
			// and PCR0/PCR1 reads call COP0_UpdatePCCR() before reading the
			// selected counter without forcing an event-tail split.
			if (rd != 25)
				return true;

			return true;
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
					return true;
				default:
					return true;
			}
		}

		bool IsDI(u32 op)
		{
			return (op >> 26) == 0x10 && ((op >> 21) & 0x1f) == 0x10 && (op & 0x3f) == 0x39;
		}

		bool IsInBlockTLBReadProbe(u32 op)
		{
			if ((op >> 26) != 0x10 || ((op >> 21) & 0x1f) != 0x10)
				return false;

			switch (op & 0x3f)
			{
				case 0x01: // TLBR, owned by COP0.cpp::TLBR().
				case 0x08: // TLBP, owned by COP0.cpp::TLBP().
					return true;
				default:
					return false;
			}
		}

		bool IsCycleCommittingFastCOP0(u32 op)
		{
			if ((op >> 26) != 0x10)
				return false;

			switch ((op >> 21) & 0x1f)
			{
				case 0x00:
					return RD(op) == 9 || (RD(op) == 25 && RT(op) != 0 && (op & 1u) != 0);
				case 0x04:
					return RD(op) == 9 || RD(op) == 12 ||
						   (RD(op) == 25 && ((op & 1u) != 0 || (op & 0x3fu) == 0));
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

		bool FastCOP1UsesExponentMask(u32 op)
		{
			if (IsFastCOP1ArithmeticOp(op) || IsFastCOP1DivSqrtOp(op) ||
				IsFastCOP1AccumulatorOp(op) || IsFastCOP1ConvertWordOp(op))
			{
				return true;
			}

			return IsFastCOP1CompareOp(op) && (op & 0x3f) != 0x30; // C_F only clears FCR31.C.
		}

		bool IsCOP2Special2Supported(u32 index)
		{
			// PCSX2 owner: R5900OpcodeTables.cpp::Int_COP2SPECIAL2PrintTable.
			// Keep unknown slots out of the A32 provider so they remain visible
			// as scan fallbacks instead of helper-calling COP2_Unknown().
			return (index <= 42) || (index >= 44 && index <= 49) ||
				   (index >= 52 && index <= 67);
		}

		bool IsCOP2Special1Supported(u32 op)
		{
			// PCSX2 owner: R5900OpcodeTables.cpp::Int_COP2SPECIAL1PrintTable.
			const u32 function = op & 0x3f;
			if (function <= 50 || function == 52 || function == 53 ||
				function == 56 || function == 57)
			{
				return true;
			}

			if (function >= 60)
			{
				const u32 special2_index = (op & 0x3) | ((op >> 4) & 0x7c);
				return IsCOP2Special2Supported(special2_index);
			}

			return false;
		}

		bool IsCOP2BranchOpcode(u32 op)
		{
			if ((op >> 26) != 0x12 || ((op >> 21) & 0x1f) != 0x08)
				return false;

			switch (RT(op))
			{
				case 0x00: // BC2F, owned by COP2.cpp::BC2F().
				case 0x01: // BC2T, owned by COP2.cpp::BC2T().
				case 0x02: // BC2FL, owned by COP2.cpp::BC2FL().
				case 0x03: // BC2TL, owned by COP2.cpp::BC2TL().
					return true;
				default:
					return false;
			}
		}

		bool IsFastCOP2VectorTransfer(u32 op)
		{
			if ((op >> 26) != 0x12 || (op & 1u) != 0)
				return false;

			switch ((op >> 21) & 0x1f)
			{
				case 0x01: // QMFC2, owned by VU0.cpp::QMFC2().
				case 0x05: // QMTC2, owned by VU0.cpp::QMTC2().
					return true;
				default:
					return false;
			}
		}

		bool IsFastCOP2ControlRead(u32 op)
		{
			return (op >> 26) == 0x12 && ((op >> 21) & 0x1f) == 0x02 &&
				   (op & 1u) == 0; // CFC2, owned by VU0.cpp::CFC2().
		}

		bool IsFastCOP2ControlWrite(u32 op)
		{
			if ((op >> 26) != 0x12 || ((op >> 21) & 0x1f) != 0x06 ||
				(op & 1u) != 0)
			{
				return false;
			}

			const unsigned fs = RD(op);
			return fs != VU0_REG_FBRST && fs != VU0_REG_CMSAR1;
		}

		bool IsFastCOP2MacroInBlock(u32 op)
		{
			// PCSX2 owners: VU0.cpp::COP2_SPECIAL(), VUops.cpp VMAX/VMINI/
			// VABS/VCLIP/VDIV/VFTOI/VITOF/VNOP/VMOVE/VMR32/VI*/VMFIR/VMTIR/VWAITQ/VR*/VILWR/VISWR/
			// VLQI/VSQI/VLQD/VSQD, and x86/microVU_Macro.inl
			// recVMAX/recVMINI/recVABS/recVNOP/recVMOVE/recVMR32/recVI*/
			// recVMFIR/recVMTIR/recVWAITQ/recVR*. These macro ops either have
			// no MAC/status/clip synchronization side effects or synchronize
			// their flags directly, so idle VU0 can execute them inline while
			// running VU0 stays on the helper tail.
			if ((op >> 26) != 0x12 || (((op >> 21) & 0x10) == 0) ||
				!IsCOP2Special1Supported(op))
			{
				return false;
			}

			if (DecodeCop2MacroMinMax(op).valid)
				return true;
			if (DecodeCop2MacroMove(op).valid)
				return true;
			if (DecodeCop2MacroVi(op).valid)
				return true;
			if (DecodeCop2MacroViTransfer(op).valid)
				return true;
			if (DecodeCop2MacroRandom(op).valid)
				return true;
			if (DecodeCop2MacroIndexedViMemory(op).valid)
				return true;
			if (DecodeCop2MacroIndexedVectorMemory(op).valid)
				return true;
			if (IsCop2MacroClip(op))
				return true;
			if (DecodeCop2MacroItof(op).valid)
				return true;
			if (DecodeCop2MacroFtoi(op).valid)
				return true;
			if (DecodeCop2MacroFdiv(op).valid)
				return true;

			if ((op & 0x3c) != 0x3c)
				return false;

			const u32 special2_index = (op & 0x3) | ((op >> 4) & 0x7c);
			return special2_index == 0x1d || // VABS
				   special2_index == 0x2f;   // VNOP
		}

		bool IsFastCOP2InBlock(u32 op)
		{
			return IsFastCOP2VectorTransfer(op) || IsFastCOP2ControlRead(op) ||
				   IsFastCOP2ControlWrite(op) || IsFastCOP2MacroInBlock(op);
		}

		unsigned FastVu0AddressUses(u32 op)
		{
			if (IsCOP2BranchOpcode(op))
				return 1;

			if (IsFastCOP2VectorTransfer(op))
			{
				switch ((op >> 21) & 0x1f)
				{
					case 0x01: // QMFC2 reads VF when rt is writable.
						return RT(op) != 0 ? 1 : 0;
					case 0x05: // QMTC2 writes VF when fs is writable.
						return RD(op) != 0 ? 1 : 0;
					default:
						return 0;
				}
			}

			if (IsFastCOP2ControlRead(op))
				return RT(op) != 0 ? 1 : 0;

			if (IsFastCOP2ControlWrite(op))
			{
				switch (RD(op))
				{
					case 0:
					case VU0_REG_MAC_FLAG:
					case VU0_REG_TPC:
					case VU0_REG_VPU_STAT:
						return 0;
					default:
						return 1;
				}
			}

			if (IsFastCOP2MacroInBlock(op))
			{
				if (DecodeCop2MacroMinMax(op).valid)
					return 4; // code + source VF + operand VF/VI + destination VF.
				if (DecodeCop2MacroMove(op).valid)
					return 3; // code + source VF + destination VF.
				if (DecodeCop2MacroVi(op).valid)
					return 4; // code + source VI + operand/imm + destination VI.
				if (DecodeCop2MacroViTransfer(op).valid)
					return 3; // code + source VF/VI + destination VF/VI.
				if (DecodeCop2MacroRandom(op).valid)
				{
					const Cop2MacroRandomOp random = DecodeCop2MacroRandom(op);
					return random.kind == Cop2MacroRandomKind::WaitQ ? 1 : 3;
				}
				if (DecodeCop2MacroIndexedViMemory(op).valid)
					return 4; // code + address VI + data VI + VU memory/register window.
				if (DecodeCop2MacroIndexedVectorMemory(op).valid)
					return 5; // code + VI backup/update + VF + VU memory/register window.
				if (IsCop2MacroClip(op))
					return 4; // code + Fs/Ft VF + clipflag VI mirror.
				if (DecodeCop2MacroItof(op).valid)
					return 3; // code + source VF + destination VF.
				if (DecodeCop2MacroFtoi(op).valid)
					return 3; // code + source VF + destination VF.
				if (DecodeCop2MacroFdiv(op).valid)
					return 5; // code + source VF(s) + Q/status mirrors.

				const u32 special2_index = (op & 0x3) | ((op >> 4) & 0x7c);
				return special2_index == 0x1d ? 3 : 1; // VABS uses code + source/dest VF; VNOP uses code only.
			}

			switch (op >> 26)
			{
				case 0x36: // LQC2 checks VU0 run state and may write VF.
					return RT(op) != 0 ? 2 : 1;
				case 0x3e: // SQC2 checks VU0 run state and reads VF.
					return 2;
				default:
					return 0;
			}
		}

		unsigned RegisterCount(u16 registers)
		{
			unsigned count = 0;
			while (registers != 0)
			{
				count += registers & 1u;
				registers >>= 1;
			}

			return count;
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

		bool CanCompileCOP2(u32 op)
		{
#if defined(VITASX2_QEMU_PROVIDER_FIXTURE)
			return IsCOP2BranchOpcode(op) || IsFastCOP2VectorTransfer(op) ||
				   IsFastCOP2ControlRead(op) || IsFastCOP2ControlWrite(op);
#else
			// PCSX2 owners: COP2.cpp, VU0.cpp, VUops.cpp, and
			// R5900OpcodeTables.cpp::Int_COP2*PrintTable. The first A32 full-core
			// path helper-calls the same interpreter owner for valid transfer and
			// VU0 macro forms, while BC2 branches use a native branch test below.
			switch ((op >> 21) & 0x1f)
			{
				case 0x01: // QMFC2, owned by VU0.cpp::QMFC2().
				case 0x02: // CFC2, owned by VU0.cpp::CFC2().
				case 0x05: // QMTC2, owned by VU0.cpp::QMTC2().
				case 0x06: // CTC2, owned by VU0.cpp::CTC2().
					return true;
				case 0x08: // COP2_BC2 branch forms, owned by COP2.cpp::BC2*().
					return IsCOP2BranchOpcode(op);
				default:
					if (((op >> 21) & 0x10) == 0)
						return false;
					return IsCOP2Special1Supported(op);
			}
#endif
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
				case 0x08: // TGEI, owned by R5900OpcodeImpl.cpp::TGEI().
				case 0x09: // TGEIU, owned by R5900OpcodeImpl.cpp::TGEIU().
				case 0x0a: // TLTI, owned by R5900OpcodeImpl.cpp::TLTI().
				case 0x0b: // TLTIU, owned by R5900OpcodeImpl.cpp::TLTIU().
				case 0x0c: // TEQI, owned by R5900OpcodeImpl.cpp::TEQI().
				case 0x0e: // TNEI, owned by R5900OpcodeImpl.cpp::TNEI().
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
				case 0x12:
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
	static_assert((GprOffset(0) % 16) == 0);
	static_assert((GprOffset(0) % alignof(u64)) == 0);
	static_assert(GprOffset(31) + sizeof(u64) <= 0x3fc);
	static_assert(HI_OFFSET + sizeof(GPR_reg) <= 0x0fff);
	static_assert(LO_OFFSET + sizeof(GPR_reg) <= 0x0fff);
	static_assert((HI_OFFSET % alignof(u64)) == 0);
	static_assert((LO_OFFSET % alignof(u64)) == 0);
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
	static_assert((LAST_PERF_CYCLE_OFFSET % alignof(u64)) == 0);
	static_assert(CYCLE_OFFSET + sizeof(u64) <= 0x0fff);
	static_assert((CYCLE_OFFSET % alignof(u64)) == 0);
	static_assert(BRANCH_OFFSET + sizeof(int) <= 0x0fff);
	static_assert(NEXT_EVENT_OFFSET + sizeof(u64) <= 0x0fff);
	static_assert((NEXT_EVENT_OFFSET % alignof(u64)) == 0);
	static_assert(LAST_COP0_CYCLE_OFFSET + sizeof(u64) <= 0x0fff);
	static_assert((LAST_COP0_CYCLE_OFFSET % alignof(u64)) == 0);
	static_assert(TLB_ENTRY_COUNT == 48);
	static_assert(sizeof(vtlb_private::VTLBVirtual) == sizeof(u32));
	static_assert(TLB_ENTRY_SIZE == 16);
	static_assert(TLB_PAGE_MASK_OFFSET == 0);
	static_assert(TLB_ENTRY_HI_OFFSET == 4);
	static_assert(TLB_ENTRY_LO0_OFFSET == 8);
	static_assert(TLB_ENTRY_LO1_OFFSET == 12);
	static_assert(VU0_VF_OFFSET == 0);
	static_assert(VU0_VI_OFFSET + VU0_VI_STRIDE * 32 <= 0x0fff);
#if !defined(VITASX2_QEMU_PROVIDER_FIXTURE)
	static_assert(VU0_MEM_OFFSET <= 0x0fff);
#endif

	void RefreshRawGpr0KnownZero()
	{
		s_raw_gpr0_known_zero = (cpuRegs.GPR.r[0].UD[0] == 0 && cpuRegs.GPR.r[0].UD[1] == 0) ? 1u : 0u;
	}

	BlockCompiler::BlockCompiler(VitaA32::CodeBuffer& code)
		: m_code(code)
	{
	}

	bool BlockCompiler::EmitAndImm32OrReg(unsigned rd, unsigned rn, u32 value, unsigned scratch, bool set_flags)
	{
		return m_code.EmitAndImm32(rd, rn, value, set_flags) ||
			   (!set_flags && m_code.EmitBicImm32(rd, rn, ~value)) ||
			   (m_code.EmitMovImm32(scratch, value) && m_code.EmitAndReg(rd, rn, scratch, set_flags));
	}

	bool BlockCompiler::EmitOrrImm32OrReg(unsigned rd, unsigned rn, u32 value, unsigned scratch, bool set_flags)
	{
		return m_code.EmitOrrImm32(rd, rn, value, set_flags) ||
			   (m_code.EmitMovImm32(scratch, value) && m_code.EmitOrrReg(rd, rn, scratch, set_flags));
	}

	bool BlockCompiler::EmitEorImm32OrReg(unsigned rd, unsigned rn, u32 value, unsigned scratch, bool set_flags)
	{
		return m_code.EmitEorImm32(rd, rn, value, set_flags) ||
			   (m_code.EmitMovImm32(scratch, value) && m_code.EmitEorReg(rd, rn, scratch, set_flags));
	}

	bool BlockCompiler::EmitBicImm32OrReg(unsigned rd, unsigned rn, u32 value, unsigned scratch, bool set_flags)
	{
		return m_code.EmitBicImm32(rd, rn, value, set_flags) ||
			   (!set_flags && m_code.EmitAndImm32(rd, rn, ~value)) ||
			   (m_code.EmitMovImm32(scratch, ~value) && m_code.EmitAndReg(rd, rn, scratch, set_flags));
	}

	bool BlockCompiler::EmitCmpImm32OrReg(unsigned rn, u32 value, unsigned scratch)
	{
		return m_code.EmitCmpImm32(rn, value) ||
			   (m_code.EmitMovImm32(scratch, value) && m_code.EmitCmpReg(rn, scratch));
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
			case 0x12: // COP2/VU0 macro interface, owned by COP2.cpp and VU0.cpp.
				return CanCompileCOP2(op);
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
			case 0x12: // COP2_BC2 branch forms, owned by COP2.cpp::BC2F()/BC2T()/BC2FL()/BC2TL().
				return IsCOP2BranchOpcode(op);
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
		// SYSCALL/BREAK are different: R5900OpcodeImpl.cpp::SYSCALL()/BREAK()
		// are helper-backed exception paths, and Interpreter.cpp::_doBranch_shared()
		// marks cpuRegs.branch before executing them as delay slots.
		// Trap ops use the same exception-shaped helper/event tail.
		return CanCompileOpcode(op) && (op >> 26) != 0x12 && !IsDI(op) && !IsInBlockTLBReadProbe(op) &&
			   (!RequiresBlockEndAfterOpcode(op) || IsSYSCALL(op) || IsBREAK(op) ||
				   IsTrapOpcode(op) || IsCounterReadLoad(op));
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
				return (op & 0x3f) == 0x0c || (op & 0x3f) == 0x0d ||
					   (op & 0x3f) == 0x0f || IsSpecialTrap(op);
			case 0x01:
				return IsRegImmTrap(op);
			case 0x10:
				return CanCompileCOP0(op) && !IsDI(op) && !IsFastMFC0(op) && !IsFastMTC0(op) &&
					   !IsInBlockTLBReadProbe(op);
			case 0x11:
				return CanCompileCOP1(op) && !IsFastCOP1InBlock(op);
			case 0x12:
				return CanCompileCOP2(op) && !IsFastCOP2InBlock(op);
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

	bool OpcodeMayUseVtlbFastPath(u32 op)
	{
		switch (op >> 26)
		{
			case 0x1a: // LDL
			case 0x1b: // LDR
			case 0x1e: // LQ
			case 0x1f: // SQ
			case 0x20: // LB
			case 0x21: // LH
			case 0x22: // LWL
			case 0x23: // LW
			case 0x24: // LBU
			case 0x25: // LHU
			case 0x26: // LWR
			case 0x27: // LWU
			case 0x28: // SB
			case 0x29: // SH
			case 0x2a: // SWL
			case 0x2b: // SW
			case 0x2c: // SDL
			case 0x2d: // SDR
			case 0x2e: // SWR
			case 0x31: // LWC1
			case 0x36: // LQC2
			case 0x37: // LD
			case 0x39: // SWC1
			case 0x3e: // SQC2
			case 0x3f: // SD
				return true;
			default:
				return false;
		}
	}

	bool BlockMayUseVtlbFastPath(u32 start_pc, u32 instruction_count)
	{
		for (u32 i = 0; i < instruction_count; i++)
		{
			if (OpcodeMayUseVtlbFastPath(memRead32(start_pc + i * 4)))
				return true;
		}

		return false;
	}

	namespace
	{
		struct GprPinOpInfo
		{
			u8 low_reads[4]{};
			unsigned low_read_count = 0;
			u8 dword_reads[4]{};
			unsigned dword_read_count = 0;
		};
	} // namespace

	// Classifies one accepted EE opcode for the write-through GPR pin cache.
	// Returns true when every GPR-file write the op can perform goes through
	// EmitStoreGpr64()/EmitStoreGprZero64()/EmitStoreGprLowPreserveHigh(),
	// EmitStoreGprWord(), EmitStoreGprQ128(), EmitStoreGprQ128ToAddress(), or
	// EmitStoreGprDwordPair().
	// Returns false for op classes whose write paths are not certified yet;
	// those blocks compile without pins. The read lists score low32 and
	// low64 sources that reach EmitLoadGprLow()/EmitLoadGpr64().
	static bool ClassifyOpcodeForGprPinning(u32 op, GprPinOpInfo* info)
	{
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		const unsigned rd = RD(op);
		const auto add_low_read = [info](unsigned guest_reg) {
			if (guest_reg != 0 && info->low_read_count < 4)
				info->low_reads[info->low_read_count++] = static_cast<u8>(guest_reg);
		};
		const auto add_dword_read = [info](unsigned guest_reg) {
			if (guest_reg != 0 && info->dword_read_count < 4)
				info->dword_reads[info->dword_read_count++] = static_cast<u8>(guest_reg);
		};

		switch (op >> 26)
		{
			case 0x00:
				switch (op & 0x3f)
				{
					case 0x00: // SLL
					case 0x02: // SRL
					case 0x03: // SRA
						add_low_read(rt);
						return true;
					case 0x38: // DSLL
					case 0x3a: // DSRL
					case 0x3b: // DSRA
					case 0x3c: // DSLL32
					case 0x3e: // DSRL32
					case 0x3f: // DSRA32
						add_dword_read(rt);
						return true;
					case 0x04: // SLLV
					case 0x06: // SRLV
					case 0x07: // SRAV
						add_low_read(rt);
						add_low_read(rs);
						return true;
					case 0x14: // DSLLV
					case 0x16: // DSRLV
					case 0x17: // DSRAV
						add_dword_read(rt);
						add_low_read(rs);
						return true;
					case 0x08: // JR
					case 0x09: // JALR links through the seam
						add_low_read(rs);
						return true;
					case 0x18: // MULT
					case 0x19: // MULTU
					case 0x1a: // DIV
					case 0x1b: // DIVU
					case 0x20: // ADD
					case 0x21: // ADDU
					case 0x22: // SUB
					case 0x23: // SUBU
					case 0x30: // TGE
					case 0x31: // TGEU
					case 0x32: // TLT
					case 0x33: // TLTU
					case 0x34: // TEQ
					case 0x36: // TNE
						add_low_read(rs);
						add_low_read(rt);
						return true;
					case 0x0a: // MOVZ
					case 0x0b: // MOVN
					case 0x24: // AND
					case 0x25: // OR
					case 0x26: // XOR
					case 0x27: // NOR
					case 0x2a: // SLT
					case 0x2b: // SLTU
					case 0x2c: // DADD
					case 0x2d: // DADDU
					case 0x2e: // DSUB
					case 0x2f: // DSUBU
						add_dword_read(rs);
						add_dword_read(rt);
						return true;
					case 0x0c: // SYSCALL exits the block through the event helper
					case 0x0d: // BREAK exits the block through the event helper
					case 0x0f: // SYNC
					case 0x10: // MFHI writes rd through the seam
					case 0x12: // MFLO writes rd through the seam
					case 0x28: // MFSA writes rd through the seam
						return true;
					case 0x11: // MTHI
					case 0x13: // MTLO
						add_dword_read(rs);
						return true;
					case 0x29: // MTSA
						add_low_read(rs);
						return true;
					default:
						return false;
				}
			case 0x01:
				switch (rt)
				{
					case 0x00: // BLTZ
					case 0x01: // BGEZ
					case 0x02: // BLTZL
					case 0x03: // BGEZL
					case 0x08: // TGEI
					case 0x09: // TGEIU
					case 0x0a: // TLTI
					case 0x0b: // TLTIU
					case 0x0c: // TEQI
					case 0x0e: // TNEI
					case 0x10: // BLTZAL links through the seam
					case 0x11: // BGEZAL links through the seam
					case 0x12: // BLTZALL links through the seam
					case 0x13: // BGEZALL links through the seam
						add_dword_read(rs);
						return true;
					case 0x18: // MTSAB
					case 0x19: // MTSAH
						add_low_read(rs);
						return true;
					default:
						return false;
				}
			case 0x02: // J
			case 0x03: // JAL links through the seam
			case 0x0f: // LUI
			case 0x2f: // CACHE writes no GPR
			case 0x33: // PREF
				return true;
			case 0x10: // COP0, owned by COP0.cpp and x86/iCOP0.cpp.
				switch ((op >> 21) & 0x1f)
				{
					case 0x00: // MFC0 writes rt through EmitStoreGpr64().
						return IsFastMFC0(op);
					case 0x04: // MTC0 writes CP0; active stores read rt through EmitLoadGprLow().
						if (!IsFastMTC0(op))
							return false;
						if (rd == 0x18 || (rd == 0x19 && (op & 1u) == 0 && (op & 0x3eu) != 0))
							return true;
						add_low_read(rt);
						return true;
					case 0x08: // COP0_BC0 branch forms read only CPCOND0.
						return CanCompileCOP0(op);
					case 0x10: // COP0_C0 system/TLB ops write no GPR; block-ending helpers exit immediately.
						return CanCompileCOP0(op);
					default:
						return false;
				}
			case 0x11: // COP1, owned by FPU.cpp and x86/iFPU.cpp.
				if (((op >> 21) & 0x1f) == 0x08)
					return CanCompileCOP1(op); // COP1_BC1 branches read only FCR31.C.

				if (!IsFastCOP1InBlock(op))
					return CanCompileCOP1(op); // Accepted helper fallbacks are block-ending event tails.

				if (((op >> 21) & 0x1f) == 0x04 ||
					(((op >> 21) & 0x1f) == 0x06 && rd == 31))
				{
					add_low_read(rt); // MTC1/CTC1 read rt through EmitLoadGprLow().
				}
				return true;
			case 0x12: // COP2/VU0, owned by VU0.cpp and x86/microVU_Macro.inl.
				if (IsCOP2BranchOpcode(op))
					return true;

				if (IsFastCOP2VectorTransfer(op))
				{
					return true;
				}

				if (IsFastCOP2ControlRead(op))
					return true; // CFC2 uses EmitStoreGpr64() or EmitStoreGprLowPreserveHigh().

				if (IsFastCOP2ControlWrite(op))
				{
					switch (rd)
					{
						case 0:
						case VU0_REG_MAC_FLAG:
						case VU0_REG_TPC:
						case VU0_REG_VPU_STAT:
							return true;
						default:
							add_low_read(rt); // CTC2 active writes read nonzero rt through the pin-aware raw-zero loader.
							return true;
					}
				}

				return CanCompileCOP2(op); // Helper-backed macro/interlocked forms are block-ending event tails.
			case 0x04: // BEQ
			case 0x05: // BNE
			case 0x14: // BEQL
			case 0x15: // BNEL
				add_dword_read(rs);
				add_dword_read(rt);
				return true;
			case 0x06: // BLEZ
			case 0x07: // BGTZ
			case 0x16: // BLEZL
			case 0x17: // BGTZL
				add_dword_read(rs);
				return true;
			case 0x08: // ADDI
			case 0x09: // ADDIU
			case 0x0c: // ANDI
				add_low_read(rs);
				return true;
			case 0x0a: // SLTI
			case 0x0b: // SLTIU
			case 0x0d: // ORI
			case 0x0e: // XORI
			case 0x18: // DADDI
			case 0x19: // DADDIU
				add_dword_read(rs);
				return true;
			case 0x1a: // LDL writes byte lanes then refreshes the low-word pin from backing.
			case 0x1b: // LDR writes byte lanes then refreshes the low-word pin from backing.
				add_low_read(rs);
				return true;
			case 0x1e: // LQ writes rt through EmitStoreGprQ128() and refreshes the cold-tail pin.
				add_low_read(rs);
				return true;
			case 0x22: // LWL merges rt through EmitStoreGpr64() and refreshes the cold-tail pin.
			case 0x26: // LWR merges rt through EmitStoreGpr64() and refreshes the cold-tail pin.
				add_low_read(rs);
				add_low_read(rt);
				return true;
			case 0x1c:
				switch (op & 0x3f)
				{
					case 0x00: // MADD
					case 0x01: // MADDU
					case 0x18: // MULT1
					case 0x19: // MULTU1
					case 0x1a: // DIV1
					case 0x1b: // DIVU1
					case 0x20: // MADD1
					case 0x21: // MADDU1
						add_low_read(rs);
						add_low_read(rt);
						return true;
					case 0x04: // PLZCW reads RS.UL[0]/[1] and writes rd through EmitStoreGprWord().
						add_dword_read(rs);
						return true;
					case 0x10: // MFHI1 writes rd through the seam
					case 0x12: // MFLO1 writes rd through the seam
						return true;
					case 0x11: // MTHI1
					case 0x13: // MTLO1
						add_dword_read(rs);
						return true;
					case 0x30: // PMFHL writes rd from HI/LO through EmitStoreGprWord().
						return true;
					case 0x31: // PMTHL reads RS words into HI/LO
						add_low_read(rs);
						return true;
					case 0x34: // PSLLH
					case 0x36: // PSRLH
					case 0x37: // PSRAH
					case 0x3c: // PSLLW
					case 0x3e: // PSRLW
					case 0x3f: // PSRAW
						return true; // Packed immediate shifts write rd through EmitStoreGprQ128().
					case 0x08: // MMI0 class, owned by R5900OpcodeTables.cpp::Class_MMI0().
					{
						if (!CanCompileMMI0(op))
							return false;

						switch ((op >> 6) & 0x1f)
						{
							default:
								break;
						}
						return true;
					}
					case 0x09: // MMI2 class, owned by R5900OpcodeTables.cpp::Class_MMI2().
					{
						if (!CanCompileMMI2(op))
							return false;

							switch ((op >> 6) & 0x1f)
							{
								case 0x02: // PSLLVW reads RT through NEON and RS word shift counts.
								case 0x03: // PSRLVW reads RT through NEON and RS word shift counts.
									add_low_read(rs);
									break;
							case 0x0c: // PMULTW reads full RS/RT qwords through NEON.
								break;
							case 0x10: // PMADDH reads full RS/RT qwords through NEON.
							case 0x14: // PMSUBH reads full RS/RT qwords through NEON.
								break;
							case 0x11: // PHMADH reads full RS/RT qwords through NEON.
							case 0x15: // PHMSBH reads full RS/RT qwords through NEON.
								break;
							case 0x1c: // PMULTH reads full RS/RT qwords through NEON.
								break;
							case 0x00: // PMADDW
								case 0x04: // PMSUBW
								case 0x0d: // PDIVW
								case 0x1d: // PDIVBW
									add_low_read(rs);
									add_low_read(rt);
									break;
								case 0x1a: // PEXEH
								case 0x1e: // PEXEW
								case 0x1f: // PROT3W
									add_low_read(rt);
									break;
							default:
								break;
						}
						return true;
					}
					case 0x28: // MMI1 class, owned by R5900OpcodeTables.cpp::Class_MMI1().
						if (!CanCompileMMI1(op))
							return false;

						return true;
					case 0x29: // MMI3 class, owned by R5900OpcodeTables.cpp::Class_MMI3().
					{
						if (!CanCompileMMI3(op))
							return false;

						switch ((op >> 6) & 0x1f)
						{
							case 0x08: // PMTHI reads RS into HI
							case 0x09: // PMTLO reads RS into LO
								// Full-qword NEON copies still read authoritative memory;
								// don't score them as low32 pin users until q-register
								// materialization is certified.
								return true;
							default:
								break;
						}

							switch ((op >> 6) & 0x1f)
							{
								case 0x03: // PSRAVW reads RT through NEON and RS word shift counts.
									add_low_read(rs);
									break;
							case 0x00: // PMADDUW reads full RS/RT qwords through NEON.
								break;
							case 0x0c: // PMULTUW reads full RS/RT qwords through NEON.
								break;
								case 0x0d: // PDIVUW
									add_low_read(rs);
									add_low_read(rt);
									break;
								case 0x1e: // PEXCW
									add_low_read(rt);
									break;
							default:
								break;
						}
						return true;
					}
					default:
						return false;
				}
			case 0x20: // LB
			case 0x21: // LH
			case 0x23: // LW
			case 0x24: // LBU
			case 0x25: // LHU
			case 0x27: // LWU
			case 0x37: // LD writes rt through the seam in hot and cold paths
			case 0x31: // LWC1 writes an FPR only
			case 0x39: // SWC1
			case 0x36: // LQC2 writes a VU0 register only
			case 0x3e: // SQC2
				add_low_read(rs);
				return true;
			case 0x28: // SB
			case 0x29: // SH
			case 0x2b: // SW
			case 0x3f: // SD
			case 0x2a: // SWL writes guest RAM only
			case 0x2c: // SDL writes guest RAM only
			case 0x2d: // SDR writes guest RAM only
			case 0x2e: // SWR writes guest RAM only
			case 0x1f: // SQ writes guest RAM only
				add_low_read(rs);
				add_low_read(rt);
				return true;
			default:
				return false;
		}
	}

	bool BlockShouldUseCop1ExponentMaskRegister(u32 start_pc, u32 instruction_count)
	{
		unsigned mask_users = 0;
		for (u32 i = 0; i < instruction_count; i++)
		{
			if (FastCOP1UsesExponentMask(memRead32(start_pc + i * 4)) && ++mask_users >= 2)
				return true;
		}

		return false;
	}

	bool BlockShouldUseVu0BaseRegister(u32 start_pc, u32 instruction_count)
	{
		unsigned address_users = 0;
		for (u32 i = 0; i < instruction_count; i++)
		{
			address_users += FastVu0AddressUses(memRead32(start_pc + i * 4));
			if (address_users >= 3)
				return true;
		}

		return false;
	}

	bool MmiOpcodeKeepsGprQCacheLocal(u32 op)
	{
		if ((op >> 26) != 0x1c)
			return false;

		switch (op & 0x3f)
		{
			case 0x00: // MADD
			case 0x01: // MADDU
			case 0x04: // PLZCW
			case 0x10: // MFHI1
			case 0x11: // MTHI1
			case 0x12: // MFLO1
			case 0x13: // MTLO1
			case 0x18: // MULT1
			case 0x19: // MULTU1
			case 0x20: // MADD1
			case 0x21: // MADDU1
			case 0x30: // PMFHL
			case 0x31: // PMTHL
			case 0x34: // PSLLH
			case 0x36: // PSRLH
			case 0x37: // PSRAH
			case 0x3c: // PSLLW
			case 0x3e: // PSRLW
			case 0x3f: // PSRAW
				return CanCompileMMI(op);
			case 0x08: // MMI0 class; every accepted op is pure A32/NEON here.
				return CanCompileMMI0(op);
			case 0x28: // MMI1 class; every accepted op is pure A32/NEON here.
				return CanCompileMMI1(op);
			case 0x09: // MMI2 class.
			{
				const unsigned sub = (op >> 6) & 0x1f;
				if (sub == 0x0d || sub == 0x1d) // PDIVW/PDIVBW can call C helpers.
					return false;
				return CanCompileMMI2(op);
			}
			case 0x29: // MMI3 class.
			{
				const unsigned sub = (op >> 6) & 0x1f;
				if (sub == 0x0d) // PDIVUW can call a C helper.
					return false;
				return CanCompileMMI3(op);
			}
			default:
				return false;
		}
	}

	bool BlockShouldUseGprQCache(u32 start_pc, u32 instruction_count)
	{
		if (instruction_count == 0)
			return false;

		for (u32 i = 0; i < instruction_count; i++)
		{
			if (!MmiOpcodeKeepsGprQCacheLocal(memRead32(start_pc + i * 4)))
				return false;
		}

		return true;
	}

	void BlockCompiler::StageGprPinsForBlock(u32 start_pc, u32 instruction_count, bool allow_r7, bool allow_r8,
		bool allow_r10, bool allow_r11)
	{
		m_staged_pin_count = 0;

		u16 read_counts[32]{};
		u16 dword_read_counts[32]{};
		for (u32 i = 0; i < instruction_count; i++)
		{
			GprPinOpInfo info;
			if (!ClassifyOpcodeForGprPinning(memRead32(start_pc + i * 4), &info))
				return;

			for (unsigned read = 0; read < info.low_read_count; read++)
				read_counts[info.low_reads[read]]++;
			for (unsigned read = 0; read < info.dword_read_count; read++)
			{
				const unsigned guest_reg = info.dword_reads[read];
				read_counts[guest_reg]++;
				dword_read_counts[guest_reg]++;
			}
		}

		u8 hosts[MAX_GPR_PINS];
		unsigned host_count = 0;
		hosts[host_count++] = HOST_GPR_PIN0;
		if (allow_r10)
			hosts[host_count++] = HOST_COP1_EXPONENT_MASK;
		if (allow_r11)
			hosts[host_count++] = HOST_VU0_BASE;
		if (allow_r7)
			hosts[host_count++] = HOST_VTLB_VMAP;
		if (allow_r8)
			hosts[host_count++] = HOST_VTLB_HOST_MEMORY_BASE;

		bool host_used[MAX_GPR_PINS]{};
		unsigned used_hosts = 0;
		const auto next_free_host_slot = [&]() {
			for (unsigned slot = 0; slot < host_count; slot++)
			{
				if (!host_used[slot])
					return slot;
			}
			return host_count;
		};

		while (used_hosts + 1 < host_count)
		{
			unsigned best_reg = 0;
			u16 best_count = 1; // one low64 read only trades the entry loads for moves
			for (unsigned reg = 1; reg < 32; reg++)
			{
				if (dword_read_counts[reg] > best_count)
				{
					best_count = dword_read_counts[reg];
					best_reg = reg;
				}
			}

			if (best_reg != 0)
			{
				unsigned low_slot = next_free_host_slot();
				if (low_slot >= host_count)
					break;
				host_used[low_slot] = true;
				unsigned high_slot = next_free_host_slot();
				if (high_slot >= host_count)
					break;
				m_staged_pin_guest[m_staged_pin_count] = static_cast<u8>(best_reg);
				m_staged_pin_host[m_staged_pin_count] = hosts[low_slot];
				m_staged_pin_high_host[m_staged_pin_count] = hosts[high_slot];
				m_staged_pin_count++;
				host_used[high_slot] = true;
				used_hosts += 2;
				read_counts[best_reg] = 0;
				dword_read_counts[best_reg] = 0;
			}
			else
			{
				break;
			}
		}

		for (unsigned slot = 0; slot < host_count && used_hosts < host_count; slot++)
		{
			if (host_used[slot])
				continue;

			unsigned best_reg = 0;
			u16 best_count = 1; // a single read would only trade the entry load for the read
			for (unsigned reg = 1; reg < 32; reg++)
			{
				if (read_counts[reg] > best_count)
				{
					best_count = read_counts[reg];
					best_reg = reg;
				}
			}

			if (best_reg == 0)
				break;

			read_counts[best_reg] = 0;
			m_staged_pin_guest[m_staged_pin_count] = static_cast<u8>(best_reg);
			m_staged_pin_host[m_staged_pin_count] = hosts[slot];
			m_staged_pin_high_host[m_staged_pin_count] = NO_GPR_PIN_HOST;
			m_staged_pin_count++;
			host_used[slot] = true;
			used_hosts++;
		}
	}

	int BlockCompiler::FindGprPinHost(unsigned guest_reg) const
	{
		for (unsigned i = 0; i < m_pin_count; i++)
		{
			if (m_pin_guest[i] == guest_reg)
				return static_cast<int>(m_pin_host[i]);
		}

		return -1;
	}

	int BlockCompiler::FindGprPinHighHost(unsigned guest_reg) const
	{
		for (unsigned i = 0; i < m_pin_count; i++)
		{
			if (m_pin_guest[i] == guest_reg && m_pin_high_host[i] != NO_GPR_PIN_HOST)
				return static_cast<int>(m_pin_high_host[i]);
		}

		return -1;
	}

	bool BlockCompiler::EmitGprPinLoads()
	{
		for (unsigned i = 0; i < m_pin_count; i++)
		{
			if (!m_code.EmitLdrImm12(m_pin_host[i], HOST_CPU_REGS,
					static_cast<u16>(GprOffset(m_pin_guest[i]))))
			{
				return false;
			}

			if (m_pin_high_host[i] != NO_GPR_PIN_HOST &&
				!m_code.EmitLdrImm12(m_pin_high_host[i], HOST_CPU_REGS,
					static_cast<u16>(GprOffset(m_pin_guest[i]) + sizeof(u32))))
			{
				return false;
			}
		}

		return true;
	}

	void BlockCompiler::ClearGprQCache()
	{
		m_gpr_q_cache_count = 0;
	}

	void BlockCompiler::InvalidateGprQCacheForGuest(unsigned guest_reg)
	{
		if (guest_reg == 0 || m_gpr_q_cache_count == 0)
			return;

		for (unsigned i = 0; i < m_gpr_q_cache_count;)
		{
			if (m_gpr_q_cache_guest[i] == guest_reg)
			{
				const unsigned last = --m_gpr_q_cache_count;
				m_gpr_q_cache_guest[i] = m_gpr_q_cache_guest[last];
				m_gpr_q_cache_qreg[i] = m_gpr_q_cache_qreg[last];
				continue;
			}
			i++;
		}
	}

	void BlockCompiler::InvalidateGprQCacheForQreg(unsigned qreg)
	{
		if (m_gpr_q_cache_count == 0)
			return;

		for (unsigned i = 0; i < m_gpr_q_cache_count;)
		{
			if (m_gpr_q_cache_qreg[i] == qreg)
			{
				const unsigned last = --m_gpr_q_cache_count;
				m_gpr_q_cache_guest[i] = m_gpr_q_cache_guest[last];
				m_gpr_q_cache_qreg[i] = m_gpr_q_cache_qreg[last];
				continue;
			}
			i++;
		}
	}

	void BlockCompiler::MarkGprQCache(unsigned guest_reg, unsigned qreg)
	{
		if (!m_gpr_q_cache_enabled || guest_reg == 0 || qreg >= 4)
			return;

		InvalidateGprQCacheForGuest(guest_reg);
		InvalidateGprQCacheForQreg(qreg);
		if (m_gpr_q_cache_count >= 4)
			return;

		m_gpr_q_cache_guest[m_gpr_q_cache_count] = static_cast<u8>(guest_reg);
		m_gpr_q_cache_qreg[m_gpr_q_cache_count] = static_cast<u8>(qreg);
		m_gpr_q_cache_count++;
	}

	int BlockCompiler::FindGprQCache(unsigned guest_reg) const
	{
		if (!m_gpr_q_cache_enabled || guest_reg == 0)
			return -1;

		for (unsigned i = 0; i < m_gpr_q_cache_count; i++)
		{
			if (m_gpr_q_cache_guest[i] == guest_reg)
				return static_cast<int>(m_gpr_q_cache_qreg[i]);
		}

		return -1;
	}

	bool BlockCompiler::BeginBlock(bool use_vtlb_registers, bool use_cop1_exponent_mask_register,
		bool use_vu0_base_register)
	{
		m_scalar_load_cold_tails.clear();
		m_scalar_store_cold_tails.clear();
		m_qword_load_cold_tails.clear();
		m_qword_store_cold_tails.clear();
		m_cop1_word_memory_cold_tails.clear();
		m_cop2_qword_memory_cold_tails.clear();
		m_vu0_sync_cold_tails.clear();
		m_partial_memory_cold_tails.clear();
		m_vtlb_registers_available = use_vtlb_registers;
		m_cop1_exponent_mask_available = use_cop1_exponent_mask_register;
		m_vu0_base_available = use_vu0_base_register;
		ClearGprQCache();
#if defined(VITASX2_QEMU_VALIDATION)
		if (m_gpr_q_cache_enabled)
			g_qemuGprQCacheBlocks++;
#endif
		m_saved_registers = REG_R4 | REG_R5 | REG_R6;
		if (use_vtlb_registers)
			m_saved_registers |= REG_R7 | REG_R8;
		if (use_cop1_exponent_mask_register)
			m_saved_registers |= REG_R10;
		if (use_vu0_base_register)
			m_saved_registers |= REG_R11;

		// Adopt the staged GPR pins, dropping any whose host register a block
		// feature claimed after staging.
		m_pin_count = 0;
		for (unsigned i = 0; i < m_staged_pin_count; i++)
		{
			const unsigned host = m_staged_pin_host[i];
			const unsigned high_host = m_staged_pin_high_host[i];
			if ((host == HOST_VTLB_VMAP && use_vtlb_registers) ||
				(host == HOST_VTLB_HOST_MEMORY_BASE && use_vtlb_registers) ||
				(host == HOST_COP1_EXPONENT_MASK && use_cop1_exponent_mask_register) ||
				(host == HOST_VU0_BASE && use_vu0_base_register) ||
				(high_host == HOST_VTLB_VMAP && use_vtlb_registers) ||
				(high_host == HOST_VTLB_HOST_MEMORY_BASE && use_vtlb_registers) ||
				(high_host == HOST_COP1_EXPONENT_MASK && use_cop1_exponent_mask_register) ||
				(high_host == HOST_VU0_BASE && use_vu0_base_register))
			{
				continue;
			}

			m_pin_guest[m_pin_count] = m_staged_pin_guest[i];
			m_pin_host[m_pin_count] = static_cast<u8>(host);
			m_pin_high_host[m_pin_count] = static_cast<u8>(high_host);
			m_pin_count++;
			m_saved_registers |= static_cast<u16>(1u << host);
			if (high_host != NO_GPR_PIN_HOST)
				m_saved_registers |= static_cast<u16>(1u << high_host);
		}
		m_staged_pin_count = 0;

#if defined(VITASX2_QEMU_VALIDATION)
		if (m_pin_count != 0)
		{
			g_qemuGprPinnedBlocks++;
			g_qemuGprPinnedRegisters += m_pin_count;
			for (unsigned i = 0; i < m_pin_count; i++)
			{
				if (m_pin_high_host[i] != NO_GPR_PIN_HOST)
					g_qemuGprPinnedDwordRegisters++;
				if (m_pin_host[i] == HOST_VTLB_VMAP ||
					m_pin_host[i] == HOST_VTLB_HOST_MEMORY_BASE)
					g_qemuGprPinnedVtlbFreeHostRegisters++;
				if (m_pin_high_host[i] == HOST_VTLB_VMAP ||
					m_pin_high_host[i] == HOST_VTLB_HOST_MEMORY_BASE)
					g_qemuGprPinnedVtlbFreeHostRegisters++;
			}
		}
#endif

		// Keep SP 8-byte aligned for AAPCS helper calls after any resident-register choice.
		if ((RegisterCount(m_saved_registers | REG_LR) & 1u) != 0)
		{
			if (!(m_saved_registers & REG_R9))
				m_saved_registers |= REG_R9;
			else if (!(m_saved_registers & REG_R10))
				m_saved_registers |= REG_R10;
			else if (!(m_saved_registers & REG_R11))
				m_saved_registers |= REG_R11;
			else
				m_saved_registers |= REG_R3;
		}

		if (!m_code.EmitPush(m_saved_registers | REG_LR) ||
			!m_code.EmitMovImm32(HOST_CPU_REGS, static_cast<u32>(reinterpret_cast<uptr>(&cpuRegs))))
		{
			return false;
		}

		if (use_cop1_exponent_mask_register &&
			!m_code.EmitMovImm32(HOST_COP1_EXPONENT_MASK, FPU_FLOAT_EXPONENT_MASK))
		{
			return false;
		}

		if (use_vu0_base_register &&
			!m_code.EmitMovImm32(HOST_VU0_BASE, static_cast<u32>(reinterpret_cast<uptr>(&VU0))))
		{
			return false;
		}

		// PCSX2 owner: vtlb.cpp::vtlb_memRead*()/vtlb_memWrite*() read
		// these stable pointers from vtlbdata for every access. Keep them
		// resident for EE A32 blocks that actually emit VTLB fast paths.
		return !use_vtlb_registers ||
			   (m_code.EmitMovImm32(HOST_VTLB_VMAP,
				   static_cast<u32>(reinterpret_cast<uptr>(&vtlb_private::vtlbdata.vmap))) &&
			   m_code.EmitLdrImm12(HOST_VTLB_VMAP, HOST_VTLB_VMAP, 0) &&
			   m_code.EmitMovImm32(HOST_VTLB_HOST_MEMORY_BASE,
				   static_cast<u32>(reinterpret_cast<uptr>(&vtlb_private::vtlbdata.host_memory_base))) &&
			   m_code.EmitLdrImm12(HOST_VTLB_HOST_MEMORY_BASE, HOST_VTLB_HOST_MEMORY_BASE, 0));
	}

	bool BlockCompiler::CompileStraightLineBlock(u32 start_pc, u32 instruction_count, const void* direct_exit,
		const void* event_exit, u32* scaled_cycles, DirectLinkSlots* direct_links)
	{
		if (instruction_count == 0 || instruction_count > ((UINT32_MAX - start_pc) / 4))
			return false;

		if (direct_links)
			*direct_links = {};

		const bool use_vtlb_registers = BlockMayUseVtlbFastPath(start_pc, instruction_count);
		const bool use_cop1_exponent_mask_register =
			BlockShouldUseCop1ExponentMaskRegister(start_pc, instruction_count);
		const bool use_vu0_base_register = BlockShouldUseVu0BaseRegister(start_pc, instruction_count);
		m_gpr_q_cache_enabled = BlockShouldUseGprQCache(start_pc, instruction_count);
		StageGprPinsForBlock(start_pc, instruction_count, !use_vtlb_registers, !use_vtlb_registers,
			!use_cop1_exponent_mask_register, !use_vu0_base_register);
		if (!BeginBlock(use_vtlb_registers, use_cop1_exponent_mask_register, use_vu0_base_register))
			return false;
		// Pins must be live before any emitted GPR read, including the Goemon
		// hook's GPR4 argument load; the hook's helpers are AAPCS calls that
		// preserve the callee-saved pin hosts and never write the GPR file.
		if (!EmitGprPinLoads())
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
				if (!EmitDeviceTracePreInstruction(pc))
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
					case 0x12:
						branch_target_pc = BranchTarget(pc, op);
						if (branch_is_likely)
							has_static_likely_direct_links = true;
						else
							has_static_conditional_direct_links = true;
						if (!EmitCop2Branch(op))
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
					if (!m_code.EmitCmpImm32(HOST_BRANCH_FLAG, 0))
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
			if (!EmitDeviceTracePreInstruction(pc))
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
				if (IsSupportedBranchOpcode(next_op) ||
					(RequiresBlockEndAfterOpcode(next_op) && !IsSYNC(next_op)) ||
					IsCycleCommittingFastCOP0(next_op))
				{
					return false;
				}

				pending_di_clear = true;
				continue;
			}
			if (!EmitOpcode(op, pc, raw_cycles, event_exit, branch_delay_slot))
				return false;

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

			if ((IsSYSCALL(op) || IsBREAK(op) || IsTrapOpcode(op)) && !branch_delay_slot)
			{
				if (scaled_cycles)
					*scaled_cycles = committed_scaled_cycles + ScaleBlockCycles(raw_cycles);
				return FlushColdTails();
			}

			if ((op >> 26) == 0x10 && CanCompileCOP0(op) && !IsFastMFC0(op) && !IsFastMTC0(op) &&
				!IsInBlockTLBReadProbe(op))
			{
				if (scaled_cycles)
					*scaled_cycles = committed_scaled_cycles + ScaleBlockCycles(raw_cycles);
				return FlushColdTails();
			}

			if ((op >> 26) == 0x11 && CanCompileCOP1(op) && !IsFastCOP1InBlock(op))
			{
				if (scaled_cycles)
					*scaled_cycles = committed_scaled_cycles + ScaleBlockCycles(raw_cycles);
				return FlushColdTails();
			}

			if ((op >> 26) == 0x12 && CanCompileCOP2(op) && !IsCOP2BranchOpcode(op) &&
				!IsFastCOP2InBlock(op))
			{
				if (scaled_cycles)
					*scaled_cycles = committed_scaled_cycles + ScaleBlockCycles(raw_cycles);
				return FlushColdTails();
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

			if (!FlushColdTails())
				return false;

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

		if (!FlushColdTails())
			return false;

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
			case 0x01: // REGIMM, including traps and MTSAB/MTSAH from R5900OpcodeImpl.cpp.
				if (IsRegImmTrap(op))
					return EmitTrapEventExit(op, pc, raw_cycles_through_instruction, event_exit, branch_delay_slot);
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
			case 0x12: // COP2/VU0 macro interface, owned by COP2.cpp, VU0.cpp, and x86/microVU_Macro.inl.
				return EmitCOP2(op, pc, raw_cycles_through_instruction, event_exit);
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
				return EmitLWU(op, pc, raw_cycles_through_instruction, event_exit);
			case 0x28: // SB, owned by R5900OpcodeImpl.cpp::SB().
				return EmitSB(op);
			case 0x29: // SH, owned by R5900OpcodeImpl.cpp::SH().
				return EmitSH(op, pc, raw_cycles_through_instruction, event_exit);
			case 0x2a: // SWL, owned by R5900OpcodeImpl.cpp::SWL().
				return EmitSWL(op);
			case 0x2b: // SW, owned by R5900OpcodeImpl.cpp::SW().
				return EmitSW(op, pc, raw_cycles_through_instruction, event_exit);
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
				return EmitLD(op, pc, raw_cycles_through_instruction, event_exit);
			case 0x39: // SWC1, owned by FPU.cpp::SWC1().
				return EmitSWC1(op);
			case 0x3e: // SQC2, owned by VU0.cpp::SQC2().
				return EmitSQC2(op);
			case 0x3f: // SD, owned by R5900OpcodeImpl.cpp::SD().
				return EmitSD(op, pc, raw_cycles_through_instruction, event_exit);
			default:
				return false;
		}
	}

		bool BlockCompiler::EndBlockReturn(u8 value)
		{
			return m_code.EmitMovImm8(0, value) &&
				   m_code.EmitPop(m_saved_registers | REG_PC);
		}

	bool BlockCompiler::EndBlockWithCycleTest(u32 block_cycles, const void* direct_exit, const void* event_exit,
		size_t* direct_link_target_offset, size_t* taken_link_target_offset)
	{
		if (!direct_exit || !event_exit)
			return false;

		if (!EmitLoadCpuRegsU64(CYCLE_OFFSET, HOST_TMP0, HOST_TMP1, HOST_TMP2))
		{
			return false;
		}

		if (!m_code.EmitAddImm32(HOST_TMP0, HOST_TMP0, block_cycles, true))
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
			!EmitStoreCpuRegsU64(CYCLE_OFFSET, HOST_TMP0, HOST_TMP1, HOST_TMP2) ||
			!EmitLoadCpuRegsU64(NEXT_EVENT_OFFSET, HOST_TMP2, HOST_TMP3, HOST_TMP4) ||
			!m_code.EmitSubReg(HOST_TMP2, HOST_TMP0, HOST_TMP2, true) ||
			!m_code.EmitSbcReg(HOST_TMP3, HOST_TMP1, HOST_TMP3, true))
		{
			return false;
		}

		const size_t direct_branch = m_code.EmitBranchPlaceholder(VitaA32::Condition::MI);
		if (direct_branch == static_cast<size_t>(-1))
			return false;

		if (!m_code.EmitCallAbsolute(event_exit) ||
			!m_code.EmitPop(m_saved_registers | REG_PC))
		{
			return false;
		}

		const size_t direct_target = m_code.Size();
		if (taken_link_target_offset)
		{
			if (!m_code.EmitCmpImm32(HOST_BRANCH_FLAG, 0))
			{
				return false;
			}

			const size_t taken_tail = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (taken_tail == static_cast<size_t>(-1))
				return false;

			if (!m_code.EmitPop(m_saved_registers | REG_LR))
				return false;

			const size_t fallthrough_target_offset = m_code.Size();
			if (!m_code.EmitMovImm32Patchable(HOST_TMP4, static_cast<u32>(reinterpret_cast<uptr>(direct_exit))) ||
				!m_code.EmitBx(HOST_TMP4))
			{
				return false;
			}

			const size_t taken_tail_target = m_code.Size();
			if (!m_code.PatchBranch(taken_tail, taken_tail_target, VitaA32::Condition::NE) ||
				!m_code.EmitPop(m_saved_registers | REG_LR))
			{
				return false;
			}

			const size_t taken_target_offset = m_code.Size();
			if (!m_code.EmitMovImm32Patchable(HOST_TMP4, static_cast<u32>(reinterpret_cast<uptr>(direct_exit))) ||
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

		if (!m_code.EmitPop(m_saved_registers | REG_LR))
			return false;

		const size_t target_offset = m_code.Size();
		if (!m_code.EmitMovImm32Patchable(HOST_TMP4, static_cast<u32>(reinterpret_cast<uptr>(direct_exit))) ||
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

		if (!EmitLoadCpuRegsU64(CYCLE_OFFSET, HOST_TMP0, HOST_TMP1, HOST_TMP2) ||
			!m_code.EmitCmpImm32(HOST_BRANCH_FLAG, 0))
		{
			return false;
		}

		const size_t taken_path = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
		if (taken_path == static_cast<size_t>(-1))
			return false;

		const auto add_cycles = [this](u32 cycles) {
			if (!m_code.EmitAddImm32(HOST_TMP0, HOST_TMP0, cycles, true))
			{
				if (!m_code.EmitMovImm32(HOST_TMP2, cycles) ||
					!m_code.EmitAddReg(HOST_TMP0, HOST_TMP0, HOST_TMP2, true))
				{
					return false;
				}
			}

			return m_code.EmitAdcImm8(HOST_TMP1, HOST_TMP1, 0) &&
				   EmitStoreCpuRegsU64(CYCLE_OFFSET, HOST_TMP0, HOST_TMP1, HOST_TMP2);
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

		if (!EmitLoadCpuRegsU64(NEXT_EVENT_OFFSET, HOST_TMP2, HOST_TMP3, HOST_TMP4) ||
			!m_code.EmitSubReg(HOST_TMP2, HOST_TMP0, HOST_TMP2, true) ||
			!m_code.EmitSbcReg(HOST_TMP3, HOST_TMP1, HOST_TMP3, true))
		{
			return false;
		}

		const size_t direct_branch = m_code.EmitBranchPlaceholder(VitaA32::Condition::MI);
		if (direct_branch == static_cast<size_t>(-1))
			return false;

		if (!m_code.EmitCallAbsolute(event_exit) ||
			!m_code.EmitPop(m_saved_registers | REG_PC))
		{
			return false;
		}

		const size_t direct_target = m_code.Size();
		if (not_taken_link_target_offset || taken_link_target_offset)
		{
			if (!m_code.EmitCmpImm32(HOST_BRANCH_FLAG, 0))
			{
				return false;
			}

			const size_t taken_tail = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (taken_tail == static_cast<size_t>(-1))
				return false;

			if (!m_code.EmitPop(m_saved_registers | REG_LR))
				return false;

			const size_t not_taken_target_offset = m_code.Size();
			if (!m_code.EmitMovImm32Patchable(HOST_TMP4, static_cast<u32>(reinterpret_cast<uptr>(direct_exit))) ||
				!m_code.EmitBx(HOST_TMP4))
			{
				return false;
			}

			const size_t taken_tail_target = m_code.Size();
			if (!m_code.PatchBranch(taken_tail, taken_tail_target, VitaA32::Condition::NE) ||
				!m_code.EmitPop(m_saved_registers | REG_LR))
			{
				return false;
			}

			const size_t taken_target_offset = m_code.Size();
			if (!m_code.EmitMovImm32Patchable(HOST_TMP4, static_cast<u32>(reinterpret_cast<uptr>(direct_exit))) ||
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

		if (!m_code.EmitPop(m_saved_registers | REG_LR))
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
			case 0x0c: // SYSCALL, owned by R5900OpcodeImpl.cpp::SYSCALL().
				return EmitSYSCALL(op, pc, raw_cycles_through_instruction, event_exit, branch_delay_slot);
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
			case 0x28: // MFSA, owned by R5900OpcodeImpl.cpp::MFSA().
				return EmitMFSA(op);
			case 0x29: // MTSA, owned by R5900OpcodeImpl.cpp::MTSA().
				return EmitMTSA(op);
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
			case 0x30: // TGE, owned by R5900OpcodeImpl.cpp::TGE().
			case 0x31: // TGEU, owned by R5900OpcodeImpl.cpp::TGEU().
			case 0x32: // TLT, owned by R5900OpcodeImpl.cpp::TLT().
			case 0x33: // TLTU, owned by R5900OpcodeImpl.cpp::TLTU().
			case 0x34: // TEQ, owned by R5900OpcodeImpl.cpp::TEQ().
			case 0x36: // TNE, owned by R5900OpcodeImpl.cpp::TNE().
				return EmitTrapEventExit(op, pc, raw_cycles_through_instruction, event_exit, branch_delay_slot);
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
						return EmitTLBRInBlock();
					case 0x02: // TLBWI, owned by COP0.cpp::TLBWI().
						return EmitSystemHelperEventExit(op, pc + 4, raw_cycles_through_instruction,
							reinterpret_cast<const void*>(&TLBWI), event_exit, true);
					case 0x06: // TLBWR, owned by COP0.cpp::TLBWR().
						return EmitSystemHelperEventExit(op, pc + 4, raw_cycles_through_instruction,
							reinterpret_cast<const void*>(&TLBWR), event_exit, true);
					case 0x08: // TLBP, owned by COP0.cpp::TLBP().
						return EmitTLBPInBlock();
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
			if ((op & 1u) != 0)
				return EmitMFC0PerfCounterFast(op, ScaleBlockCycles(raw_cycles_through_instruction));

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

		if (!EmitLoadCpuRegsU64(CYCLE_OFFSET, HOST_TMP0, HOST_TMP1, HOST_TMP2))
		{
			return false;
		}

		if (!m_code.EmitAddImm32(HOST_TMP0, HOST_TMP0, scaled_cycles_through_instruction, true))
		{
			if (!m_code.EmitMovImm32(HOST_TMP2, scaled_cycles_through_instruction) ||
				!m_code.EmitAddReg(HOST_TMP0, HOST_TMP0, HOST_TMP2, true))
			{
				return false;
			}
		}

		if (!m_code.EmitAdcImm8(HOST_TMP1, HOST_TMP1, 0) ||
			!EmitStoreCpuRegsU64(CYCLE_OFFSET, HOST_TMP0, HOST_TMP1, HOST_TMP2) ||
			!m_code.EmitLdrImm12(HOST_TMP2, HOST_CPU_REGS, static_cast<u16>(LAST_COP0_CYCLE_OFFSET)) ||
			!m_code.EmitSubReg(HOST_TMP2, HOST_TMP0, HOST_TMP2) ||
			!m_code.EmitLdrImm12(HOST_TMP3, HOST_CPU_REGS, static_cast<u16>(Cp0Offset(9))) ||
			!m_code.EmitAddReg(HOST_TMP3, HOST_TMP3, HOST_TMP2) ||
			!m_code.EmitStrImm12(HOST_TMP3, HOST_CPU_REGS, static_cast<u16>(Cp0Offset(9))) ||
			!EmitStoreCpuRegsU64(LAST_COP0_CYCLE_OFFSET, HOST_TMP0, HOST_TMP1, HOST_TMP4))
		{
			return false;
		}

		if (rt == 0)
			return true;

			return m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP3, VitaA32::ShiftType::ASR, 31) &&
				   EmitStoreGpr64(rt, HOST_TMP3, HOST_TMP2);
		}

		bool BlockCompiler::EmitMFC0PerfCounterFast(u32 op, u32 scaled_cycles_through_instruction)
		{
			// PCSX2 owner: x86/iCOP0.cpp::recMFC0() rd 25 odd selectors. MFPC0
			// and MFPC1 commit cycles, call COP0_UpdatePCCR(), then sign-extend
			// the selected PCR into the target GPR.
			const unsigned rt = RT(op);
			if (rt == 0)
				return true;
			if (scaled_cycles_through_instruction == 0)
				return false;

			const bool pcr1 = (op & 2u) != 0;
			const size_t pcr_offset = pcr1 ? PERF_PCR1_OFFSET : PERF_PCR0_OFFSET;
			return EmitAddScaledCyclesToCpu(scaled_cycles_through_instruction) &&
				   m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&COP0_UpdatePCCR)) &&
				   m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(pcr_offset)) &&
				   m_code.EmitMovRegShiftImm(HOST_TMP1, HOST_TMP0, VitaA32::ShiftType::ASR, 31) &&
				   EmitStoreGpr64(rt, HOST_TMP0, HOST_TMP1);
		}

		bool BlockCompiler::EmitMTC0Fast(u32 op, u32 raw_cycles_through_instruction)
		{
			// PCSX2 owner: x86/iCOP0.cpp::recMTC0() under CP0_RECOMPILE.
			// Status calls PCSX2's WriteCP0Status() in-block after committing
			// cycles, matching the x86 helper-call shape while avoiding an
			// artificial event-tail split. Count and the perf counter writes
			// commit cycles here using the same scaleblockcycles_clear() cadence;
			// MTPS/PCCR calls COP0_UpdatePCCR() before storing the new PCCR and
			// then runs COP0_DiagnosticPCCR(), as PCSX2's x86 path does.
			const unsigned rt = RT(op);
			const unsigned rd = RD(op);
			const auto load_rt_low = [this, rt](unsigned host_reg) {
				return EmitLoadGprLow(rt, host_reg);
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
						   EmitStoreCpuRegsU64(LAST_COP0_CYCLE_OFFSET, HOST_TMP0, HOST_TMP1, HOST_TMP4);
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
						   EmitBicImm32OrReg(HOST_TMP0, HOST_TMP0, 0x00000fc0u, HOST_TMP1) &&
						   EmitOrrImm32OrReg(HOST_TMP0, HOST_TMP0, 0x00000440u, HOST_TMP1) &&
						   m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(Cp0Offset(16)));
				case 0x18: // Breakpoint debug registers
					return true;
				case 0x19: // Perf counters
					if ((op & 1u) == 0)
					{
						if ((op & 0x3eu) != 0)
							return true; // Non-zero even selectors are no-op.

						const u32 cycles = ScaleBlockCycles(raw_cycles_through_instruction);
						if (cycles == 0 || !EmitAddScaledCyclesToCpu(cycles))
							return false;

						return m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&COP0_UpdatePCCR)) &&
							   load_rt_low(HOST_TMP0) &&
							   m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS,
								   static_cast<u16>(PERF_PCCR_OFFSET)) &&
							   m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&COP0_DiagnosticPCCR));
					}

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
							   EmitStoreCpuRegsU64(last_perf_offset, HOST_TMP0, HOST_TMP1, HOST_TMP4);
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
				!EmitCmpImm32OrReg(HOST_TMP4, EVENT_DELTA, HOST_TMP5))
			{
				return false;
			}

			const size_t keep_event = m_code.EmitBranchPlaceholder(VitaA32::Condition::LE);
			if (keep_event == static_cast<size_t>(-1))
				return false;

			if (!EmitStoreCpuRegsU64(NEXT_EVENT_OFFSET, HOST_TMP2, HOST_TMP3, HOST_TMP4))
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
				!EmitAndImm32OrReg(HOST_TMP4, HOST_TMP2, EI_ALLOWED_MASK, HOST_TMP3, true))
			{
				return false;
			}

			const size_t set_eie = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (set_eie == static_cast<size_t>(-1))
				return false;

			if (!m_code.EmitTstImm32(HOST_TMP2, EI_KSU_MASK))
			{
				return false;
			}

			const size_t skip_ei = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (skip_ei == static_cast<size_t>(-1))
				return false;

			const size_t set_target = m_code.Size();
			if (!m_code.PatchBranch(set_eie, set_target, VitaA32::Condition::NE) ||
				!EmitOrrImm32OrReg(HOST_TMP2, HOST_TMP2, STATUS_EIE_SET_MASK, HOST_TMP3) ||
				!m_code.EmitStrImm12(HOST_TMP2, HOST_CPU_REGS, static_cast<u16>(Cp0Offset(12))) ||
				!EmitSetNextEventDelta4FromCurrentCycle())
			{
				return false;
			}

			if (!m_code.PatchBranch(skip_ei, m_code.Size(), VitaA32::Condition::NE))
				return false;

			return m_code.EmitCallAbsolute(event_exit) &&
				   m_code.EmitPop(m_saved_registers | REG_PC);
		}

		bool BlockCompiler::EmitTLBRInBlock()
		{
			// PCSX2 owners: COP0.cpp::TLBR(), x86/iCOP0.cpp::recTLBR().
			// TLBR reads the architectural TLB table only, so it can be emitted
			// directly; the normal block tail owns PC, cycle, and event testing.
			if (!m_code.EmitLdrImm12(HOST_TMP2, HOST_CPU_REGS, static_cast<u16>(Cp0Offset(0))) ||
				!m_code.EmitAndImm8(HOST_TMP2, HOST_TMP2, 0x3f) ||
				!EmitCmpImm32OrReg(HOST_TMP2, TLB_ENTRY_COUNT, HOST_TMP3))
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
				!EmitAndImm32OrReg(HOST_TMP4, HOST_TMP4, TLB_PAGE_MASK_REGISTER_MASK, HOST_TMP5) ||
				!m_code.EmitStrImm12(HOST_TMP4, HOST_CPU_REGS, static_cast<u16>(Cp0Offset(5))) ||
				!m_code.EmitLdrImm12(HOST_TMP0, HOST_TMP3, static_cast<u16>(TLB_ENTRY_HI_OFFSET)) ||
				!EmitOrrImm32OrReg(HOST_TMP5, HOST_TMP4, 0x1f00u, HOST_TMP5) ||
				!m_code.EmitMvnReg(HOST_TMP5, HOST_TMP5) ||
				!m_code.EmitAndReg(HOST_TMP0, HOST_TMP0, HOST_TMP5) ||
				!m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(Cp0Offset(10))) ||
				!m_code.EmitLdrImm12(HOST_TMP0, HOST_TMP3, static_cast<u16>(TLB_ENTRY_LO0_OFFSET)) ||
				!m_code.EmitLdrImm12(HOST_TMP1, HOST_TMP3, static_cast<u16>(TLB_ENTRY_LO1_OFFSET)) ||
				!m_code.EmitAndReg(HOST_TMP2, HOST_TMP0, HOST_TMP1) ||
				!m_code.EmitAndImm8(HOST_TMP2, HOST_TMP2, 1) ||
				!EmitAndImm32OrReg(HOST_TMP0, HOST_TMP0, TLB_TLBR_ENTRY_LO0_MASK, HOST_TMP5) ||
				!m_code.EmitOrrReg(HOST_TMP0, HOST_TMP0, HOST_TMP2) ||
				!m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(Cp0Offset(2))) ||
				!EmitAndImm32OrReg(HOST_TMP1, HOST_TMP1, TLB_TLBR_ENTRY_LO1_MASK, HOST_TMP5) ||
				!m_code.EmitOrrReg(HOST_TMP1, HOST_TMP1, HOST_TMP2) ||
				!m_code.EmitStrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(Cp0Offset(3))))
			{
				return false;
			}

			if (!m_code.PatchBranch(invalid_index, m_code.Size(), VitaA32::Condition::CS))
				return false;

			return true;
		}

		bool BlockCompiler::EmitTLBPInBlock()
		{
			// PCSX2 owners: COP0.cpp::TLBP(), x86/iCOP0.cpp::recTLBP().
			// Keep COP0.cpp's EntryHi32 bitfield view exactly: VPN2 is the
			// low 19 bits of EntryHi, while ASID is bits 24..31.
			// The operation has no cycle-dependent side effects, so the normal
			// block tail owns PC, cycle, and event testing.
			if (!m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(Cp0Offset(10))) ||
				!EmitAndImm32OrReg(HOST_TMP3, HOST_TMP0, TLB_ENTRY_HI32_VPN2_MASK, HOST_TMP1) ||
				!m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP0, VitaA32::ShiftType::LSR, 24) ||
				!m_code.EmitMovImm32(HOST_TMP5, static_cast<u32>(reinterpret_cast<uptr>(&tlb[0]))) ||
				!m_code.EmitMovImm8(HOST_TMP4, 0))
			{
				return false;
			}

			const size_t loop_start = m_code.Size();
			if (!m_code.EmitLdrImm12(HOST_TMP0, HOST_TMP5, static_cast<u16>(TLB_PAGE_MASK_OFFSET)) ||
				!m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_TMP0, VitaA32::ShiftType::LSR, 13) ||
				!EmitAndImm32OrReg(HOST_TMP0, HOST_TMP0, TLB_MASK_FIELD_MASK, HOST_TMP1) ||
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
				!EmitCmpImm32OrReg(HOST_TMP4, TLB_ENTRY_COUNT, HOST_TMP0))
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

			return true;
		}

		bool BlockCompiler::EmitERETEventExit(u32 op, u32 raw_cycles_through_instruction,
			const void* event_exit)
		{
			// PCSX2 owner: x86/iCOP0.cpp::recERET() branches after
			// COP0.cpp::ERET(). Inline the ERL/ErrorEPC versus EXL/EPC state
			// update and keep the required event-test tail.
			constexpr u32 STATUS_EXL_MASK = 0x00000002u;
			constexpr u32 STATUS_ERL_MASK = 0x00000004u;

			if (!event_exit || raw_cycles_through_instruction == 0)
				return false;

			const u32 cycles = ScaleBlockCycles(raw_cycles_through_instruction);
			if (!m_code.EmitMovImm32(HOST_TMP0, op) ||
				!m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(CODE_OFFSET)) ||
				!EmitAddScaledCyclesToCpu(cycles) ||
				!m_code.EmitLdrImm12(HOST_TMP2, HOST_CPU_REGS, static_cast<u16>(Cp0Offset(12))) ||
				!m_code.EmitTstImm32(HOST_TMP2, STATUS_ERL_MASK))
			{
				return false;
			}

			const size_t exl_path = m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
			if (exl_path == static_cast<size_t>(-1))
				return false;

			if (!m_code.EmitLdrImm12(HOST_TMP4, HOST_CPU_REGS, static_cast<u16>(Cp0Offset(30))) ||
				!m_code.EmitStrImm12(HOST_TMP4, HOST_CPU_REGS, static_cast<u16>(PC_OFFSET)) ||
				!EmitBicImm32OrReg(HOST_TMP2, HOST_TMP2, STATUS_ERL_MASK, HOST_TMP3) ||
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
				!EmitBicImm32OrReg(HOST_TMP2, HOST_TMP2, STATUS_EXL_MASK, HOST_TMP3) ||
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
				   m_code.EmitPop(m_saved_registers | REG_PC);
		}

		bool BlockCompiler::EmitDIDelayedStatusClear()
		{
			// PCSX2 owner: x86/iCOP0.cpp::recDI() inlines COP0.cpp::DI() after
			// recompiling the next instruction, so Status.EIE changes only after
			// that following instruction has observed the old Status value.
			constexpr u32 DI_ALLOWED_MASK = 0x00020006u; // Status._EDI | EXL | ERL
			constexpr u32 DI_KSU_MASK = 0x00000018u;
			constexpr u32 STATUS_EIE_MASK = 0x00010000u;

			if (!m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(Cp0Offset(12))) ||
				!EmitAndImm32OrReg(HOST_TMP2, HOST_TMP0, DI_ALLOWED_MASK, HOST_TMP1, true))
			{
				return false;
			}

			const size_t clear_eie = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (clear_eie == static_cast<size_t>(-1))
				return false;

			if (!m_code.EmitTstImm32(HOST_TMP0, DI_KSU_MASK))
			{
				return false;
			}

			const size_t done = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (done == static_cast<size_t>(-1))
				return false;

			const size_t clear_target = m_code.Size();
			if (!m_code.PatchBranch(clear_eie, clear_target, VitaA32::Condition::NE) ||
				!EmitBicImm32OrReg(HOST_TMP0, HOST_TMP0, STATUS_EIE_MASK, HOST_TMP1) ||
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

	bool BlockCompiler::EmitCOP2(u32 op, u32 pc, u32 raw_cycles_through_instruction, const void* event_exit)
	{
		if (IsFastCOP2VectorTransfer(op))
			return EmitCOP2VectorTransferFast(op, pc + 4, raw_cycles_through_instruction, event_exit);
		if (IsFastCOP2ControlRead(op))
			return EmitCOP2ControlReadFast(op, pc + 4, raw_cycles_through_instruction, event_exit);
		if (IsFastCOP2ControlWrite(op))
			return EmitCOP2ControlWriteFast(op, pc + 4, raw_cycles_through_instruction, event_exit);
		if (IsFastCOP2MacroInBlock(op))
			return EmitCOP2MacroFast(op, pc + 4, raw_cycles_through_instruction, event_exit);

#if defined(VITASX2_QEMU_PROVIDER_FIXTURE)
		return false;
#else
		// PCSX2 owners: R5900OpcodeImpl.cpp::COP2(), COP2.cpp, VU0.cpp, and
		// VUops.cpp. Non-branch COP2 forms exit through the same interpreter
		// dispatcher so VU0 sync, VCALLMS, flags, and transfer side effects stay
		// PCSX2-owned while the full machine is brought up.
		return EmitSystemHelperEventExit(op, pc + 4, raw_cycles_through_instruction,
			reinterpret_cast<const void*>(&R5900::Interpreter::OpcodeImpl::COP2), event_exit);
#endif
	}

	bool BlockCompiler::EmitCOP2IdleBranch(size_t* vu0_idle)
	{
		// PCSX2 owner: VU0.cpp::vu0Sync(). The native in-block path is valid
		// only while VU0 is idle; if a microprogram is running, the caller emits
		// the previous sync-and-exit path at this instruction's EE cycle.
		if (!vu0_idle)
			return false;

		if (!EmitVu0ViAddress(HOST_TMP0, VU0_REG_VPU_STAT) ||
			!m_code.EmitLdrImm12(HOST_TMP1, HOST_TMP0, 0) ||
			!m_code.EmitTstImm32(HOST_TMP1, 1))
		{
			return false;
		}

		*vu0_idle = m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
		return *vu0_idle != static_cast<size_t>(-1);
	}

	bool BlockCompiler::EmitCOP2VectorTransferBody(u32 op)
	{
		const unsigned rt = RT(op);
		const unsigned fs = RD(op);
		constexpr unsigned NEON_VALUE = 0;

		switch ((op >> 21) & 0x1f)
		{
			case 0x01: // QMFC2
				if (rt != 0 &&
					(!EmitVu0VfAddress(HOST_TMP0, fs) ||
					 !m_code.EmitVld1Q32Aligned(NEON_VALUE, HOST_TMP0) ||
					 !EmitStoreGprQ128(rt, NEON_VALUE, HOST_TMP1)))
				{
					return false;
				}
				break;
			case 0x05: // QMTC2
				if (fs != 0 &&
					(InvalidateGprQCacheForQreg(NEON_VALUE),
					 !EmitLoadCpuRegsQ128(GprOffset(rt), NEON_VALUE, HOST_TMP0) ||
					 !EmitVu0VfAddress(HOST_TMP1, fs) ||
					 !m_code.EmitVst1Q32Aligned(NEON_VALUE, HOST_TMP1)))
				{
					return false;
				}
				break;
			default:
				return false;
		}

		return true;
	}

	bool BlockCompiler::EmitCOP2ControlReadBody(u32 op)
	{
		// PCSX2 owners: VU0.cpp::CFC2() and x86/microVU_Macro.inl::recCFC2().
		// REG_R only writes the low GPR word, while other VI registers
		// sign-extend into the low 64-bit GPR half.
		const unsigned rt = RT(op);
		const unsigned fs = RD(op);

		if (rt != 0)
		{
			if (!EmitVu0ViAddress(HOST_TMP0, fs) ||
				!m_code.EmitLdrImm12(HOST_TMP1, HOST_TMP0, 0))
			{
				return false;
			}

			if (fs == VU0_REG_R)
			{
				if (!m_code.EmitUbfx(HOST_TMP1, HOST_TMP1, 0, 23) ||
					!EmitStoreGprLowPreserveHigh(rt, HOST_TMP1))
				{
					return false;
				}
			}
			else if (!m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP1, VitaA32::ShiftType::ASR, 31) ||
					 !EmitStoreGpr64(rt, HOST_TMP1, HOST_TMP2))
			{
				return false;
			}
		}

		return true;
	}

	bool BlockCompiler::EmitCOP2ControlWriteBody(u32 op)
	{
		// PCSX2 owners: VU0.cpp::CTC2() and x86/microVU_Macro.inl::recCTC2().
		// FBRST and CMSAR1 remain helper-backed because they reset/start VUs;
		// the idle in-block subset is no-op, masked, or raw VI low-word writes.
		const unsigned rt = RT(op);
		const unsigned fs = RD(op);

		switch (fs)
		{
			case 0:
			case VU0_REG_MAC_FLAG:
			case VU0_REG_TPC:
			case VU0_REG_VPU_STAT:
				break;

			case VU0_REG_R:
				if (!EmitLoadGprLowRawZero(rt, HOST_TMP1) ||
					!m_code.EmitUbfx(HOST_TMP1, HOST_TMP1, 0, 23) ||
					!EmitOrrImm32OrReg(HOST_TMP1, HOST_TMP1, 0x3f800000u, HOST_TMP2) ||
					!EmitVu0ViAddress(HOST_TMP0, fs) ||
					!m_code.EmitStrImm12(HOST_TMP1, HOST_TMP0, 0))
				{
					return false;
				}
				break;

			case VU0_REG_CLIP_FLAG:
				if (!EmitLoadGprLowRawZero(rt, HOST_TMP1) ||
					!m_code.EmitMovImm32(HOST_TMP0, static_cast<u32>(reinterpret_cast<uptr>(&VU0.clipflag))) ||
					!m_code.EmitStrImm12(HOST_TMP1, HOST_TMP0, 0) ||
					!EmitVu0ViAddress(HOST_TMP0, fs) ||
					!m_code.EmitStrImm12(HOST_TMP1, HOST_TMP0, 0))
				{
					return false;
				}
				break;

			default:
				if (!EmitLoadGprLowRawZero(rt, HOST_TMP1) ||
					!EmitVu0ViAddress(HOST_TMP0, fs) ||
					!m_code.EmitStrImm12(HOST_TMP1, HOST_TMP0, 0))
				{
					return false;
				}
				break;
		}

		return true;
	}

	bool BlockCompiler::EmitCOP2MacroCodeWrite(u32 op)
	{
		if (!m_code.EmitMovImm32(HOST_TMP0, op) ||
			!m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(CODE_OFFSET)))
		{
			return false;
		}

#if defined(VITASX2_QEMU_PROVIDER_FIXTURE)
		return true;
#else
		return EmitVu0RegisterAddress(HOST_TMP1, VU0_CODE_OFFSET) &&
			   m_code.EmitStrImm12(HOST_TMP0, HOST_TMP1, 0);
#endif
	}

	bool BlockCompiler::EmitCOP2MacroBody(u32 op)
	{
		// PCSX2 owners: VU0.cpp::COP2_SPECIAL(), VUops.cpp macro helpers, and
		// x86/microVU_Macro.inl rec* macro lowerings. This body is emitted only
		// on the idle-VU0 path; running VU0 exits through the COP2 helper so
		// _vu0FinishMicro() still owns the interlock and cycle side effects.
		if (DecodeCop2MacroMinMax(op).valid)
			return EmitCOP2MacroCodeWrite(op) && EmitCOP2MacroMinMaxBody(op);
		if (DecodeCop2MacroMove(op).valid)
			return EmitCOP2MacroCodeWrite(op) && EmitCOP2MacroMoveBody(op);
		if (DecodeCop2MacroVi(op).valid)
			return EmitCOP2MacroCodeWrite(op) && EmitCOP2MacroViBody(op);
		if (DecodeCop2MacroViTransfer(op).valid)
			return EmitCOP2MacroCodeWrite(op) && EmitCOP2MacroViTransferBody(op);
		if (DecodeCop2MacroRandom(op).valid)
			return EmitCOP2MacroCodeWrite(op) && EmitCOP2MacroRandomBody(op);
		if (DecodeCop2MacroIndexedViMemory(op).valid)
			return EmitCOP2MacroCodeWrite(op) && EmitCOP2MacroIndexedViMemoryBody(op);
		if (DecodeCop2MacroIndexedVectorMemory(op).valid)
			return EmitCOP2MacroCodeWrite(op) && EmitCOP2MacroIndexedVectorMemoryBody(op);
		if (IsCop2MacroClip(op))
			return EmitCOP2MacroCodeWrite(op) && EmitCOP2MacroClipBody(op);
		if (DecodeCop2MacroItof(op).valid)
			return EmitCOP2MacroCodeWrite(op) && EmitCOP2MacroItofBody(op);
		if (DecodeCop2MacroFtoi(op).valid)
			return EmitCOP2MacroCodeWrite(op) && EmitCOP2MacroFtoiBody(op);
		if (DecodeCop2MacroFdiv(op).valid)
			return EmitCOP2MacroCodeWrite(op) && EmitCOP2MacroFdivBody(op);

		const u32 special2_index = (op & 0x3) | ((op >> 4) & 0x7c);
		if (!EmitCOP2MacroCodeWrite(op))
			return false;

		if (special2_index == 0x2f) // VNOP
			return true;

		if (special2_index != 0x1d) // VABS
			return false;

		const unsigned ft = RT(op);
		if (ft == 0)
			return true;

		const unsigned fs = RD(op);
		const unsigned mask = (op >> 21) & 0x0f;
		if (mask == 0)
			return true;

		if (mask == 0x0f)
		{
			constexpr unsigned NEON_VALUE = 0;
			constexpr unsigned NEON_MASK = 1;
			return EmitVu0VfAddress(HOST_TMP0, fs) &&
				   m_code.EmitVld1Q32Aligned(NEON_VALUE, HOST_TMP0) &&
				   m_code.EmitMovImm32(HOST_TMP2, 0x7fffffffu) &&
				   m_code.EmitVdupI32QFromCore(NEON_MASK, HOST_TMP2) &&
				   m_code.EmitVandQ(NEON_VALUE, NEON_VALUE, NEON_MASK) &&
				   EmitVu0VfAddress(HOST_TMP1, ft) &&
				   m_code.EmitVst1Q32Aligned(NEON_VALUE, HOST_TMP1);
		}

		if (!EmitVu0VfAddress(HOST_TMP0, fs) ||
			!EmitVu0VfAddress(HOST_TMP1, ft))
		{
			return false;
		}

		const auto emit_lane = [this](unsigned lane) {
			const u16 offset = static_cast<u16>(lane * sizeof(u32));
			return m_code.EmitLdrImm12(HOST_TMP2, HOST_TMP0, offset) &&
				   EmitBicImm32OrReg(HOST_TMP2, HOST_TMP2, 0x80000000u, HOST_TMP3) &&
				   m_code.EmitStrImm12(HOST_TMP2, HOST_TMP1, offset);
		};

		if ((mask & 0x8) && !emit_lane(0))
			return false;
		if ((mask & 0x4) && !emit_lane(1))
			return false;
		if ((mask & 0x2) && !emit_lane(2))
			return false;
		if ((mask & 0x1) && !emit_lane(3))
			return false;

		return true;
	}

	bool BlockCompiler::EmitVu0ViBackup(unsigned vi_reg)
	{
#if defined(VITASX2_QEMU_PROVIDER_FIXTURE)
		return false;
#else
		// PCSX2 owner: VUops.cpp::_vuBackupVI(). Repeated writes to the same VI
		// register must keep the old value from before the write chain.
		if (!EmitVu0RegisterAddress(HOST_TMP0, VU0_VI_BACKUP_CYCLES_OFFSET) ||
			!EmitVu0RegisterAddress(HOST_TMP2, VU0_VI_REG_NUMBER_OFFSET) ||
			!m_code.EmitLdrbImm12(HOST_TMP1, HOST_TMP0, 0) ||
			!m_code.EmitCmpImm32(HOST_TMP1, 0))
		{
			return false;
		}

		const size_t set_new_from_zero = m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
		if (set_new_from_zero == static_cast<size_t>(-1))
			return false;

		if (!m_code.EmitLdrImm12(HOST_TMP3, HOST_TMP2, 0) ||
			!m_code.EmitCmpImm32(HOST_TMP3, vi_reg))
		{
			return false;
		}

		const size_t set_new_from_different = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
		if (set_new_from_different == static_cast<size_t>(-1))
			return false;

		if (!m_code.EmitMovImm8(HOST_TMP1, 2) ||
			!m_code.EmitStrbImm12(HOST_TMP1, HOST_TMP0, 0))
		{
			return false;
		}

		const size_t done = m_code.EmitBranchPlaceholder();
		if (done == static_cast<size_t>(-1))
			return false;

		const size_t set_new_target = m_code.Size();
		if (!m_code.PatchBranch(set_new_from_zero, set_new_target, VitaA32::Condition::EQ) ||
			!m_code.PatchBranch(set_new_from_different, set_new_target, VitaA32::Condition::NE) ||
			!m_code.EmitMovImm8(HOST_TMP1, 2) ||
			!m_code.EmitStrbImm12(HOST_TMP1, HOST_TMP0, 0) ||
			!m_code.EmitMovImm8(HOST_TMP3, static_cast<u8>(vi_reg)) ||
			!m_code.EmitStrImm12(HOST_TMP3, HOST_TMP2, 0) ||
			!EmitVu0ViAddress(HOST_TMP4, vi_reg) ||
			!m_code.EmitLdrhImm8(HOST_TMP3, HOST_TMP4, 0) ||
			!EmitVu0RegisterAddress(HOST_TMP4, VU0_VI_OLD_VALUE_OFFSET) ||
			!m_code.EmitStrImm12(HOST_TMP3, HOST_TMP4, 0))
		{
			return false;
		}

		return m_code.PatchBranch(done, m_code.Size());
#endif
	}

	bool BlockCompiler::EmitCOP2MacroViBody(u32 op)
	{
		// PCSX2 owners: VUops.cpp::_vuIADD()/IADDI/IAND/IOR/ISUB. These write
		// only VI.US[0]/SS[0] and update _vuBackupVI() before the halfword store.
		const Cop2MacroViOp vi = DecodeCop2MacroVi(op);
		if (!vi.valid)
			return false;

		const unsigned it = RT(op) & 0x0f;
		const unsigned is = RD(op) & 0x0f;
		const unsigned id = SA(op) & 0x0f;
		const bool immediate = vi.kind == Cop2MacroViKind::AddImmediate;
		const bool logical = vi.kind == Cop2MacroViKind::And || vi.kind == Cop2MacroViKind::Or;
		const unsigned dest = immediate ? it : id;
		if (dest == 0)
			return true;

		if (!EmitVu0ViBackup(dest) ||
			!EmitVu0ViAddress(HOST_TMP0, is) ||
			!(logical ? m_code.EmitLdrhImm8(HOST_TMP2, HOST_TMP0, 0) :
						m_code.EmitLdrshImm8(HOST_TMP2, HOST_TMP0, 0)))
		{
			return false;
		}

		if (immediate)
		{
			const unsigned imm5 = SA(op) & 0x1f;
			const int imm = (imm5 & 0x10) ? static_cast<int>(imm5) - 0x20 : static_cast<int>(imm5);
			if (imm > 0)
			{
				if (!m_code.EmitAddImm8(HOST_TMP2, HOST_TMP2, static_cast<u8>(imm)))
					return false;
			}
			else if (imm < 0 && !m_code.EmitSubImm8(HOST_TMP2, HOST_TMP2, static_cast<u8>(-imm)))
			{
				return false;
			}
		}
		else
		{
			if (!EmitVu0ViAddress(HOST_TMP1, it) ||
				!(logical ? m_code.EmitLdrhImm8(HOST_TMP3, HOST_TMP1, 0) :
							m_code.EmitLdrshImm8(HOST_TMP3, HOST_TMP1, 0)))
			{
				return false;
			}

			switch (vi.kind)
			{
				case Cop2MacroViKind::Add:
					if (!m_code.EmitAddReg(HOST_TMP2, HOST_TMP2, HOST_TMP3))
						return false;
					break;
				case Cop2MacroViKind::Sub:
					if (!m_code.EmitSubReg(HOST_TMP2, HOST_TMP2, HOST_TMP3))
						return false;
					break;
				case Cop2MacroViKind::And:
					if (!m_code.EmitAndReg(HOST_TMP2, HOST_TMP2, HOST_TMP3))
						return false;
					break;
				case Cop2MacroViKind::Or:
					if (!m_code.EmitOrrReg(HOST_TMP2, HOST_TMP2, HOST_TMP3))
						return false;
					break;
				case Cop2MacroViKind::AddImmediate:
					return false;
			}
		}

		return EmitVu0ViAddress(HOST_TMP0, dest) &&
			   m_code.EmitStrhImm8(HOST_TMP2, HOST_TMP0, 0);
	}

	bool BlockCompiler::EmitCOP2MacroViTransferBody(u32 op)
	{
		// PCSX2 owners: VUops.cpp::_vuMFIR() / _vuMTIR(). MFIR sign-extends
		// VI.SS[0] into selected VF lanes; MTIR stores the selected VF lane's
		// low 16 bits into VI.US[0] after _vuBackupVI().
		const Cop2MacroViTransferOp transfer = DecodeCop2MacroViTransfer(op);
		if (!transfer.valid)
			return false;

		if (transfer.vi_to_vf)
		{
			const unsigned ft = RT(op);
			const unsigned mask = (op >> 21) & 0x0f;
			if (ft == 0 || mask == 0)
				return true;

			const unsigned is = RD(op) & 0x0f;
			if (!EmitVu0ViAddress(HOST_TMP0, is) ||
				!m_code.EmitLdrshImm8(HOST_TMP2, HOST_TMP0, 0))
			{
				return false;
			}

			if (mask == 0x0f)
			{
				constexpr unsigned NEON_VALUE = 0;
				return m_code.EmitVdupI32QFromCore(NEON_VALUE, HOST_TMP2) &&
					   EmitVu0VfAddress(HOST_TMP1, ft) &&
					   m_code.EmitVst1Q32Aligned(NEON_VALUE, HOST_TMP1);
			}

			if (!EmitVu0VfAddress(HOST_TMP1, ft))
				return false;

			const auto emit_lane = [&](unsigned lane) {
				const u16 offset = static_cast<u16>(lane * sizeof(u32));
				return m_code.EmitStrImm12(HOST_TMP2, HOST_TMP1, offset);
			};

			if ((mask & 0x8) && !emit_lane(0))
				return false;
			if ((mask & 0x4) && !emit_lane(1))
				return false;
			if ((mask & 0x2) && !emit_lane(2))
				return false;
			if ((mask & 0x1) && !emit_lane(3))
				return false;
			return true;
		}

		const unsigned it = RT(op) & 0x0f;
		if (it == 0)
			return true;

		const unsigned fs = RD(op);
		const unsigned lane = (op >> 21) & 0x03;
		const u8 source_offset = static_cast<u8>(lane * sizeof(u32));
		return EmitVu0ViBackup(it) &&
			   EmitVu0VfAddress(HOST_TMP0, fs) &&
			   m_code.EmitLdrhImm8(HOST_TMP2, HOST_TMP0, source_offset) &&
			   EmitVu0ViAddress(HOST_TMP1, it) &&
			   m_code.EmitStrhImm8(HOST_TMP2, HOST_TMP1, 0);
	}

	bool BlockCompiler::EmitVu0RandomAdvance()
	{
		// PCSX2 owner: VUops.cpp::AdvanceLFSR(). Keep the low-bit tap order and
		// final exponent/mantissa policy exact for REG_R.
		return EmitVu0ViAddress(HOST_TMP0, VU0_REG_R) &&
			   m_code.EmitLdrImm12(HOST_TMP1, HOST_TMP0, 0) &&
			   m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP1, VitaA32::ShiftType::LSR, 4) &&
			   m_code.EmitEorRegShiftImm(HOST_TMP2, HOST_TMP2, HOST_TMP1, VitaA32::ShiftType::LSR, 22) &&
			   EmitAndImm32OrReg(HOST_TMP2, HOST_TMP2, 1u, HOST_TMP3) &&
			   m_code.EmitOrrRegShiftImm(HOST_TMP1, HOST_TMP2, HOST_TMP1, VitaA32::ShiftType::LSL, 1) &&
			   EmitAndImm32OrReg(HOST_TMP1, HOST_TMP1, 0x007fffffu, HOST_TMP3) &&
			   EmitOrrImm32OrReg(HOST_TMP1, HOST_TMP1, 0x3f800000u, HOST_TMP3) &&
			   m_code.EmitStrImm12(HOST_TMP1, HOST_TMP0, 0);
	}

	bool BlockCompiler::EmitCOP2MacroRandomBody(u32 op)
	{
		// PCSX2 owners: VUops.cpp::_vuWAITQ() / _vuRINIT() / _vuRGET() /
		// _vuRNEXT() / _vuRXOR(). RNEXT returns before AdvanceLFSR when Ft is
		// zero, but otherwise advances REG_R even when the XYZW mask writes no
		// VF lanes.
		const Cop2MacroRandomOp random = DecodeCop2MacroRandom(op);
		if (!random.valid)
			return false;

		if (random.kind == Cop2MacroRandomKind::WaitQ)
			return true;

		if (random.kind == Cop2MacroRandomKind::RInit ||
			random.kind == Cop2MacroRandomKind::RXor)
		{
			const unsigned fs = RD(op);
			const unsigned lane = (op >> 21) & 0x03;
			const u8 source_offset = static_cast<u8>(lane * sizeof(u32));
			if (!EmitVu0ViAddress(HOST_TMP0, VU0_REG_R) ||
				!EmitVu0VfAddress(HOST_TMP1, fs) ||
				!m_code.EmitLdrImm12(HOST_TMP2, HOST_TMP1, source_offset))
			{
				return false;
			}

			if (random.kind == Cop2MacroRandomKind::RXor &&
				(!m_code.EmitLdrImm12(HOST_TMP3, HOST_TMP0, 0) ||
				 !m_code.EmitEorReg(HOST_TMP2, HOST_TMP2, HOST_TMP3)))
			{
				return false;
			}

			return EmitAndImm32OrReg(HOST_TMP2, HOST_TMP2, 0x007fffffu, HOST_TMP3) &&
				   EmitOrrImm32OrReg(HOST_TMP2, HOST_TMP2, 0x3f800000u, HOST_TMP3) &&
				   m_code.EmitStrImm12(HOST_TMP2, HOST_TMP0, 0);
		}

		const unsigned ft = RT(op);
		if (ft == 0)
			return true;

		if (random.kind == Cop2MacroRandomKind::RNext &&
			!EmitVu0RandomAdvance())
		{
			return false;
		}

		const unsigned mask = (op >> 21) & 0x0f;
		if (mask == 0)
			return true;

		if (!EmitVu0ViAddress(HOST_TMP0, VU0_REG_R) ||
			!m_code.EmitLdrImm12(HOST_TMP2, HOST_TMP0, 0))
		{
			return false;
		}

		if (mask == 0x0f)
		{
			constexpr unsigned NEON_VALUE = 0;
			return m_code.EmitVdupI32QFromCore(NEON_VALUE, HOST_TMP2) &&
				   EmitVu0VfAddress(HOST_TMP1, ft) &&
				   m_code.EmitVst1Q32Aligned(NEON_VALUE, HOST_TMP1);
		}

		if (!EmitVu0VfAddress(HOST_TMP1, ft))
			return false;

		const auto emit_lane = [this](unsigned lane) {
			const u16 offset = static_cast<u16>(lane * sizeof(u32));
			return m_code.EmitStrImm12(HOST_TMP2, HOST_TMP1, offset);
		};

		if ((mask & 0x8) && !emit_lane(0))
			return false;
		if ((mask & 0x4) && !emit_lane(1))
			return false;
		if ((mask & 0x2) && !emit_lane(2))
			return false;
		if ((mask & 0x1) && !emit_lane(3))
			return false;

		return true;
	}

	bool BlockCompiler::EmitVu0IndexedMemoryAddress(unsigned vi_reg)
	{
#if defined(VITASX2_QEMU_PROVIDER_FIXTURE)
		(void)vi_reg;
		return false;
#else
		// PCSX2 owner: VUops.cpp::GET_VU_MEM(). For VU0, addresses below
		// 0x4000 wrap in VU0 data memory; 0x4000 maps into VU1 VF/VI storage.
		if (!EmitVu0ViAddress(HOST_TMP0, vi_reg) ||
			!m_code.EmitLdrhImm8(HOST_TMP2, HOST_TMP0, 0) ||
			!m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP2, VitaA32::ShiftType::LSL, 4) ||
			!m_code.EmitTstImm32(HOST_TMP2, 0x4000u))
		{
			return false;
		}

		const size_t vu0_memory = m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
		if (vu0_memory == static_cast<size_t>(-1))
			return false;

		if (!m_code.EmitMovImm32(HOST_TMP0,
				static_cast<u32>(reinterpret_cast<uptr>(&VU1.VF[0]))) ||
			!EmitAndImm32OrReg(HOST_TMP2, HOST_TMP2, 0x03ffu, HOST_TMP3) ||
			!m_code.EmitAddReg(HOST_TMP0, HOST_TMP0, HOST_TMP2))
		{
			return false;
		}

		const size_t done = m_code.EmitBranchPlaceholder();
		if (done == static_cast<size_t>(-1))
			return false;

		const size_t vu0_memory_target = m_code.Size();
		if (!m_code.PatchBranch(vu0_memory, vu0_memory_target, VitaA32::Condition::EQ) ||
			!EmitVu0RegisterAddress(HOST_TMP0, VU0_MEM_OFFSET) ||
			!m_code.EmitLdrImm12(HOST_TMP0, HOST_TMP0, 0) ||
			!EmitAndImm32OrReg(HOST_TMP2, HOST_TMP2, 0x0fffu, HOST_TMP3) ||
			!m_code.EmitAddReg(HOST_TMP0, HOST_TMP0, HOST_TMP2))
		{
			return false;
		}

		return m_code.PatchBranch(done, m_code.Size());
#endif
	}

	bool BlockCompiler::EmitCOP2MacroIndexedViMemoryBody(u32 op)
	{
		// PCSX2 owners: VUops.cpp::_vuILWR() / _vuISWR(). These use VI.US[0]
		// as an indexed VU memory qword address and do not update _vuBackupVI().
		const Cop2MacroIndexedViMemoryOp memory = DecodeCop2MacroIndexedViMemory(op);
		if (!memory.valid)
			return false;

		const unsigned mask = (op >> 21) & 0x0f;
		if (mask == 0)
			return true;

		const unsigned it = RT(op) & 0x0f;
		const unsigned is = RD(op) & 0x0f;
		if (memory.load)
		{
			if (it == 0)
				return true;

			unsigned lane = 0;
			if (mask & 0x1)
				lane = 3;
			else if (mask & 0x2)
				lane = 2;
			else if (mask & 0x4)
				lane = 1;

			const u8 memory_offset = static_cast<u8>(lane * sizeof(u32));
			return EmitVu0IndexedMemoryAddress(is) &&
				   m_code.EmitLdrhImm8(HOST_TMP2, HOST_TMP0, memory_offset) &&
				   EmitVu0ViAddress(HOST_TMP1, it) &&
				   m_code.EmitStrhImm8(HOST_TMP2, HOST_TMP1, 0);
		}

		if (!EmitVu0IndexedMemoryAddress(is) ||
			!EmitVu0ViAddress(HOST_TMP1, it) ||
			!m_code.EmitLdrhImm8(HOST_TMP2, HOST_TMP1, 0) ||
			!m_code.EmitMovImm8(HOST_TMP3, 0))
		{
			return false;
		}

		const auto emit_lane = [this](unsigned lane) {
			const u8 offset = static_cast<u8>(lane * sizeof(u32));
			return m_code.EmitStrhImm8(HOST_TMP2, HOST_TMP0, offset) &&
				   m_code.EmitStrhImm8(HOST_TMP3, HOST_TMP0, static_cast<u8>(offset + sizeof(u16)));
		};

		if ((mask & 0x8) && !emit_lane(0))
			return false;
		if ((mask & 0x4) && !emit_lane(1))
			return false;
		if ((mask & 0x2) && !emit_lane(2))
			return false;
		if ((mask & 0x1) && !emit_lane(3))
			return false;

		return true;
	}

	bool BlockCompiler::EmitVu0ViLowHalfwordAdjust(unsigned vi_reg, bool decrement)
	{
		return EmitVu0ViAddress(HOST_TMP1, vi_reg) &&
			   m_code.EmitLdrhImm8(HOST_TMP2, HOST_TMP1, 0) &&
			   (decrement ? m_code.EmitSubImm8(HOST_TMP2, HOST_TMP2, 1) :
							m_code.EmitAddImm8(HOST_TMP2, HOST_TMP2, 1)) &&
			   m_code.EmitStrhImm8(HOST_TMP2, HOST_TMP1, 0);
	}

	bool BlockCompiler::EmitCOP2MacroIndexedVectorMemoryBody(u32 op)
	{
		// PCSX2 owners: VUops.cpp::_vuLQI() / _vuLQD() / _vuSQI() / _vuSQD().
		// Preserve the exact VI backup and pre/post update order around the VU
		// memory access; running VU0 still exits through the helper interlock.
		const Cop2MacroIndexedVectorMemoryOp memory = DecodeCop2MacroIndexedVectorMemory(op);
		if (!memory.valid)
			return false;

		const bool load = memory.kind == Cop2MacroIndexedVectorMemoryKind::LoadIncrement ||
						  memory.kind == Cop2MacroIndexedVectorMemoryKind::LoadDecrement;
		const bool decrement = memory.kind == Cop2MacroIndexedVectorMemoryKind::LoadDecrement ||
							   memory.kind == Cop2MacroIndexedVectorMemoryKind::StoreDecrement;
		const unsigned ft = RT(op);
		const unsigned fs = RD(op);
		const unsigned vi = load ? (fs & 0x0f) : (ft & 0x0f);
		const unsigned mask = (op >> 21) & 0x0f;

		if (!EmitVu0ViBackup(vi))
			return false;

		if (decrement && (load ? vi != 0 : ft != 0) &&
			!EmitVu0ViLowHalfwordAdjust(vi, true))
		{
			return false;
		}

		const auto emit_post_increment = [&]() {
			const bool do_increment = load ? fs != 0 : ft != 0;
			return !do_increment || EmitVu0ViLowHalfwordAdjust(vi, false);
		};

		if (load)
		{
			if (ft != 0 && mask != 0)
			{
				if (!EmitVu0IndexedMemoryAddress(vi))
					return false;

				if (mask == 0x0f)
				{
					constexpr unsigned NEON_VALUE = 0;
					if (!m_code.EmitVld1Q32Aligned(NEON_VALUE, HOST_TMP0) ||
						!EmitVu0VfAddress(HOST_TMP1, ft) ||
						!m_code.EmitVst1Q32Aligned(NEON_VALUE, HOST_TMP1))
					{
						return false;
					}
				}
				else
				{
					if (!EmitVu0VfAddress(HOST_TMP1, ft))
						return false;

					const auto emit_lane = [this](unsigned lane) {
						const u16 offset = static_cast<u16>(lane * sizeof(u32));
						return m_code.EmitLdrImm12(HOST_TMP2, HOST_TMP0, offset) &&
							   m_code.EmitStrImm12(HOST_TMP2, HOST_TMP1, offset);
					};

					if ((mask & 0x8) && !emit_lane(0))
						return false;
					if ((mask & 0x4) && !emit_lane(1))
						return false;
					if ((mask & 0x2) && !emit_lane(2))
						return false;
					if ((mask & 0x1) && !emit_lane(3))
						return false;
				}
			}

			return decrement || emit_post_increment();
		}

		if (mask != 0)
		{
			if (!EmitVu0IndexedMemoryAddress(vi) ||
				!EmitVu0VfAddress(HOST_TMP1, fs))
			{
				return false;
			}

			if (mask == 0x0f)
			{
				constexpr unsigned NEON_VALUE = 0;
				if (!m_code.EmitVld1Q32Aligned(NEON_VALUE, HOST_TMP1) ||
					!m_code.EmitVst1Q32Aligned(NEON_VALUE, HOST_TMP0))
				{
					return false;
				}
			}
			else
			{
				const auto emit_lane = [this](unsigned lane) {
					const u16 offset = static_cast<u16>(lane * sizeof(u32));
					return m_code.EmitLdrImm12(HOST_TMP2, HOST_TMP1, offset) &&
						   m_code.EmitStrImm12(HOST_TMP2, HOST_TMP0, offset);
				};

				if ((mask & 0x8) && !emit_lane(0))
					return false;
				if ((mask & 0x4) && !emit_lane(1))
					return false;
				if ((mask & 0x2) && !emit_lane(2))
					return false;
				if ((mask & 0x1) && !emit_lane(3))
					return false;
			}
		}

		return decrement || emit_post_increment();
	}

	bool BlockCompiler::EmitCOP2MacroClipBody(u32 op)
	{
		// PCSX2 owners: VUops.cpp::_vuCLIP() and VCLIPw(). The macro shifts
		// the 24-bit clip history, emits signed raw-bit comparisons for Fs.xyz
		// against abs(Ft.w), mirrors clipflag into VI[REG_CLIP_FLAG], and has
		// no MAC/status/FDIV side effects.
		if (!IsCop2MacroClip(op))
			return false;

		const unsigned ft = RT(op);
		const unsigned fs = RD(op);

		if (!EmitVu0VfAddress(HOST_TMP0, ft) ||
			!m_code.EmitLdrImm12(HOST_TMP1, HOST_TMP0, static_cast<u16>(3 * sizeof(u32))) ||
			!EmitAndImm32OrReg(HOST_TMP2, HOST_TMP1, FPU_FLOAT_EXPONENT_MASK, HOST_TMP3, true))
		{
			return false;
		}

		const size_t denormal_limit = m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
		if (denormal_limit == static_cast<size_t>(-1))
			return false;

		if (!EmitBicImm32OrReg(HOST_TMP2, HOST_TMP1, FPU_FLOAT_SIGN_MASK, HOST_TMP3))
			return false;

		const size_t limit_done = m_code.EmitBranchPlaceholder();
		if (limit_done == static_cast<size_t>(-1))
			return false;

		const size_t denormal_target = m_code.Size();
		if (!m_code.PatchBranch(denormal_limit, denormal_target, VitaA32::Condition::EQ) ||
			!m_code.EmitMovImm32(HOST_TMP2, 0x007fffffu) ||
			!m_code.PatchBranch(limit_done, m_code.Size()))
		{
			return false;
		}

		constexpr unsigned NEON_FS_POS = 0;
		constexpr unsigned NEON_FS_NEG = 1;
		constexpr unsigned NEON_LIMIT = 2;
		constexpr unsigned NEON_SIGN = 3;
		if (!EmitVu0VfAddress(HOST_TMP0, fs) ||
			!m_code.EmitVld1Q32Aligned(NEON_FS_POS, HOST_TMP0) ||
			!m_code.EmitVdupI32QFromCore(NEON_LIMIT, HOST_TMP2) ||
			!m_code.EmitMovImm32(HOST_TMP3, FPU_FLOAT_SIGN_MASK) ||
			!m_code.EmitVdupI32QFromCore(NEON_SIGN, HOST_TMP3) ||
			!m_code.EmitVeorQ(NEON_FS_NEG, NEON_FS_POS, NEON_SIGN) ||
			!m_code.EmitVcgtS32Q(NEON_FS_POS, NEON_FS_POS, NEON_LIMIT) ||
			!m_code.EmitVcgtS32Q(NEON_FS_NEG, NEON_FS_NEG, NEON_LIMIT) ||
			!m_code.EmitMovImm8(HOST_TMP4, 0))
		{
			return false;
		}

		const auto emit_extract_flag = [&](unsigned qreg, unsigned lane, unsigned shift) {
			return m_code.EmitVmovSToCore(HOST_TMP1, qreg * 4 + lane) &&
				   m_code.EmitMovRegShiftImm(HOST_TMP1, HOST_TMP1, VitaA32::ShiftType::LSR, 31) &&
				   (shift == 0 ?
						   m_code.EmitOrrReg(HOST_TMP4, HOST_TMP4, HOST_TMP1) :
						   m_code.EmitOrrRegShiftImm(HOST_TMP4, HOST_TMP4, HOST_TMP1,
							   VitaA32::ShiftType::LSL, static_cast<u8>(shift)));
		};

		if (!emit_extract_flag(NEON_FS_POS, 0, 0) ||
			!emit_extract_flag(NEON_FS_NEG, 0, 1) ||
			!emit_extract_flag(NEON_FS_POS, 1, 2) ||
			!emit_extract_flag(NEON_FS_NEG, 1, 3) ||
			!emit_extract_flag(NEON_FS_POS, 2, 4) ||
			!emit_extract_flag(NEON_FS_NEG, 2, 5))
		{
			return false;
		}

		return m_code.EmitMovImm32(HOST_TMP0, static_cast<u32>(reinterpret_cast<uptr>(&VU0.clipflag))) &&
			   m_code.EmitLdrImm12(HOST_TMP1, HOST_TMP0, 0) &&
			   m_code.EmitMovRegShiftImm(HOST_TMP1, HOST_TMP1, VitaA32::ShiftType::LSL, 6) &&
			   m_code.EmitOrrReg(HOST_TMP1, HOST_TMP1, HOST_TMP4) &&
			   EmitAndImm32OrReg(HOST_TMP1, HOST_TMP1, 0x00ffffffu, HOST_TMP2) &&
			   m_code.EmitStrImm12(HOST_TMP1, HOST_TMP0, 0) &&
			   EmitVu0ViAddress(HOST_TMP0, VU0_REG_CLIP_FLAG) &&
			   m_code.EmitStrImm12(HOST_TMP1, HOST_TMP0, 0);
	}

	bool BlockCompiler::EmitCOP2MacroItofBody(u32 op)
	{
		// PCSX2 owners: VUops.cpp::intToFloat<>() and _vuITOF*(). VITOF
		// treats source lanes as signed 32-bit integers, converts to float,
		// scales by 2^-offset, and writes only selected destination lanes.
		const Cop2MacroItofOp itof = DecodeCop2MacroItof(op);
		if (!itof.valid)
			return false;

		const unsigned ft = RT(op);
		const unsigned mask = (op >> 21) & 0x0f;
		if (ft == 0 || mask == 0)
			return true;

		constexpr unsigned NEON_VALUE = 0;
		constexpr unsigned NEON_SCALE = 1;
		const unsigned fs = RD(op);
		if (!EmitVu0VfAddress(HOST_TMP0, fs) ||
			!m_code.EmitVld1Q32Aligned(NEON_VALUE, HOST_TMP0) ||
			!m_code.EmitVcvtF32S32Q(NEON_VALUE, NEON_VALUE))
		{
			return false;
		}

		if (itof.offset != 0)
		{
			const u32 scale_bits = 0x3f800000u - (itof.offset << 23);
			if (!m_code.EmitMovImm32(HOST_TMP2, scale_bits) ||
				!m_code.EmitVdupI32QFromCore(NEON_SCALE, HOST_TMP2) ||
				!m_code.EmitVmulF32Q(NEON_VALUE, NEON_VALUE, NEON_SCALE))
			{
				return false;
			}
		}

		if (!EmitVu0VfAddress(HOST_TMP1, ft))
			return false;

		if (mask == 0x0f)
			return m_code.EmitVst1Q32Aligned(NEON_VALUE, HOST_TMP1);

		const auto emit_lane = [&](unsigned lane) {
			const u16 offset = static_cast<u16>(lane * sizeof(u32));
			return m_code.EmitVmovSToCore(HOST_TMP2, NEON_VALUE * 4 + lane) &&
				   m_code.EmitStrImm12(HOST_TMP2, HOST_TMP1, offset);
		};

		if ((mask & 0x8) && !emit_lane(0))
			return false;
		if ((mask & 0x4) && !emit_lane(1))
			return false;
		if ((mask & 0x2) && !emit_lane(2))
			return false;
		if ((mask & 0x1) && !emit_lane(3))
			return false;

		return true;
	}

	bool BlockCompiler::EmitCOP2MacroFtoiBody(u32 op)
	{
		// PCSX2 owners: VUops.cpp::floatToInt<>() and _vuFTOI*(). VFTOI
		// scales source floats by 2^offset, truncates toward zero, and
		// saturates any exponent at or above signed 32-bit range.
		const Cop2MacroFtoiOp ftoi = DecodeCop2MacroFtoi(op);
		if (!ftoi.valid)
			return false;

		const unsigned ft = RT(op);
		const unsigned mask = (op >> 21) & 0x0f;
		if (ft == 0 || mask == 0)
			return true;

		constexpr unsigned NEON_VALUE = 0;
		constexpr unsigned NEON_BITS = 1;
		constexpr unsigned NEON_MASK = 2;
		constexpr unsigned NEON_SATURATED = 3;
		const unsigned fs = RD(op);
		if (!EmitVu0VfAddress(HOST_TMP0, fs) ||
			!m_code.EmitVld1Q32Aligned(NEON_VALUE, HOST_TMP0))
		{
			return false;
		}

		if (ftoi.offset != 0)
		{
			const u32 scale_bits = 0x3f800000u + (ftoi.offset << 23);
			if (!m_code.EmitMovImm32(HOST_TMP2, scale_bits) ||
				!m_code.EmitVdupI32QFromCore(NEON_BITS, HOST_TMP2) ||
				!m_code.EmitVmulF32Q(NEON_VALUE, NEON_VALUE, NEON_BITS))
			{
				return false;
			}
		}

		if (!m_code.EmitVorrQ(NEON_BITS, NEON_VALUE, NEON_VALUE) ||
			!m_code.EmitVcvtS32F32Q(NEON_VALUE, NEON_VALUE))
		{
			return false;
		}

		if (!m_code.EmitMovImm32(HOST_TMP2, 0x7f800000u) ||
			!m_code.EmitVdupI32QFromCore(NEON_MASK, HOST_TMP2) ||
			!m_code.EmitVandQ(NEON_MASK, NEON_BITS, NEON_MASK) ||
			!m_code.EmitMovImm32(HOST_TMP2, 0x4effffffu) ||
			!m_code.EmitVdupI32QFromCore(NEON_SATURATED, HOST_TMP2) ||
			!m_code.EmitVcgtS32Q(NEON_MASK, NEON_MASK, NEON_SATURATED) ||
			!m_code.EmitVshrS32Q(NEON_SATURATED, NEON_BITS, 31) ||
			!m_code.EmitMovImm32(HOST_TMP2, 0x7fffffffu) ||
			!m_code.EmitVdupI32QFromCore(NEON_BITS, HOST_TMP2) ||
			!m_code.EmitVeorQ(NEON_SATURATED, NEON_SATURATED, NEON_BITS) ||
			!m_code.EmitVandQ(NEON_SATURATED, NEON_SATURATED, NEON_MASK) ||
			!m_code.EmitVmvnQ(NEON_MASK, NEON_MASK) ||
			!m_code.EmitVandQ(NEON_VALUE, NEON_VALUE, NEON_MASK) ||
			!m_code.EmitVorrQ(NEON_VALUE, NEON_VALUE, NEON_SATURATED))
		{
			return false;
		}

		if (!EmitVu0VfAddress(HOST_TMP1, ft))
			return false;

		if (mask == 0x0f)
			return m_code.EmitVst1Q32Aligned(NEON_VALUE, HOST_TMP1);

		const auto emit_lane = [&](unsigned lane) {
			const u16 offset = static_cast<u16>(lane * sizeof(u32));
			return m_code.EmitVmovSToCore(HOST_TMP2, NEON_VALUE * 4 + lane) &&
				   m_code.EmitStrImm12(HOST_TMP2, HOST_TMP1, offset);
		};

		if ((mask & 0x8) && !emit_lane(0))
			return false;
		if ((mask & 0x4) && !emit_lane(1))
			return false;
		if ((mask & 0x2) && !emit_lane(2))
			return false;
		if ((mask & 0x1) && !emit_lane(3))
			return false;

		return true;
	}

	bool BlockCompiler::EmitCOP2MacroFdivBody(u32 op)
	{
		// PCSX2 owners: VUops.cpp::_vuDIV() / _vuSQRT() / _vuRSQRT() and
		// SYNCFDIV(). Macro COP2 updates VU0.q/statusflag immediately and
		// mirrors Q plus D/I status bits into VI[REG_Q]/VI[REG_STATUS_FLAG].
		const Cop2MacroFdivOp fdiv = DecodeCop2MacroFdiv(op);
		if (!fdiv.valid)
			return false;

#if defined(VITASX2_QEMU_PROVIDER_FIXTURE)
		const bool vu0_overflow_clamp = true;
#else
		const bool vu0_overflow_clamp = CHECK_VU_OVERFLOW(0);
#endif
		const unsigned fs = RD(op);
		const unsigned ft = RT(op);
		const unsigned fsf = (op >> 21) & 0x03;
		const unsigned ftf = (op >> 23) & 0x03;
		constexpr unsigned VFP_FS_S0 = 0;
		constexpr unsigned VFP_FT_S1 = 1;
		constexpr unsigned VFP_Q_S2 = 2;

		const auto emit_load_vf_lane = [&](unsigned host_reg, unsigned vf_reg, unsigned lane) {
			return EmitVu0VfAddress(host_reg, vf_reg) &&
				   m_code.EmitLdrImm12(host_reg, host_reg, static_cast<u16>(lane * sizeof(u32)));
		};

		const auto emit_normalize_vu_float_word = [&](unsigned reg) {
			if (!EmitAndImm32OrReg(HOST_TMP3, reg, FPU_FLOAT_EXPONENT_MASK, HOST_TMP5) ||
				!m_code.EmitCmpImm32(HOST_TMP3, 0))
			{
				return false;
			}

			const size_t exponent_nonzero = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (exponent_nonzero == static_cast<size_t>(-1))
				return false;

			if (!EmitAndImm32OrReg(reg, reg, FPU_FLOAT_SIGN_MASK, HOST_TMP5))
				return false;

			const size_t done_from_zero = m_code.EmitBranchPlaceholder();
			if (done_from_zero == static_cast<size_t>(-1))
				return false;

			const size_t exponent_nonzero_target = m_code.Size();
			if (!m_code.PatchBranch(exponent_nonzero, exponent_nonzero_target, VitaA32::Condition::NE))
				return false;

			size_t done_from_finite = static_cast<size_t>(-1);
			if (vu0_overflow_clamp)
			{
				if (!EmitCmpImm32OrReg(HOST_TMP3, FPU_FLOAT_EXPONENT_MASK, HOST_TMP5))
					return false;

				done_from_finite = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
				if (done_from_finite == static_cast<size_t>(-1))
					return false;

				if (!EmitAndImm32OrReg(reg, reg, FPU_FLOAT_SIGN_MASK, HOST_TMP5) ||
					!EmitOrrImm32OrReg(reg, reg, FPU_FLOAT_MAX_FINITE, HOST_TMP5))
				{
					return false;
				}
			}

			const size_t done_target = m_code.Size();
			return m_code.PatchBranch(done_from_zero, done_target) &&
				   (done_from_finite == static_cast<size_t>(-1) ||
					   m_code.PatchBranch(done_from_finite, done_target, VitaA32::Condition::NE));
		};

		const auto emit_abs_word = [&](unsigned rd, unsigned rn) {
			return EmitBicImm32OrReg(rd, rn, FPU_FLOAT_SIGN_MASK, HOST_TMP5);
		};

		const auto emit_set_invalid_if_negative_nonzero = [&](unsigned reg) {
			if (!emit_abs_word(HOST_TMP3, reg) ||
				!m_code.EmitCmpImm32(HOST_TMP3, 0))
			{
				return false;
			}

			const size_t zero = m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
			if (zero == static_cast<size_t>(-1))
				return false;

			if (!m_code.EmitTstImm32(reg, FPU_FLOAT_SIGN_MASK))
				return false;

			const size_t positive = m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
			if (positive == static_cast<size_t>(-1))
				return false;

			if (!m_code.EmitMovImm8(HOST_TMP4, 0x10))
				return false;

			const size_t done_target = m_code.Size();
			return m_code.PatchBranch(zero, done_target, VitaA32::Condition::EQ) &&
				   m_code.PatchBranch(positive, done_target, VitaA32::Condition::EQ);
		};

		const auto emit_compute_div = [&]() {
			return m_code.EmitVmovCoreToS(VFP_FS_S0, HOST_TMP0) &&
				   m_code.EmitVmovCoreToS(VFP_FT_S1, HOST_TMP1) &&
				   m_code.EmitVdivF32(VFP_Q_S2, VFP_FS_S0, VFP_FT_S1) &&
				   m_code.EmitVmovSToCore(HOST_TMP0, VFP_Q_S2) &&
				   emit_normalize_vu_float_word(HOST_TMP0);
		};

		const auto emit_compute_sqrt_abs_ft = [&]() {
			return emit_abs_word(HOST_TMP1, HOST_TMP1) &&
				   m_code.EmitVmovCoreToS(VFP_FT_S1, HOST_TMP1) &&
				   m_code.EmitVsqrtF32(VFP_Q_S2, VFP_FT_S1) &&
				   m_code.EmitVmovSToCore(HOST_TMP0, VFP_Q_S2) &&
				   emit_normalize_vu_float_word(HOST_TMP0);
		};

		const auto emit_store_q_and_status = [&]() {
			if (!EmitVu0RegisterAddress(HOST_TMP1, VU0_Q_OFFSET) ||
				!m_code.EmitStrImm12(HOST_TMP0, HOST_TMP1, 0) ||
				!EmitVu0ViAddress(HOST_TMP1, VU0_REG_Q) ||
				!m_code.EmitStrImm12(HOST_TMP0, HOST_TMP1, 0) ||
				!EmitVu0RegisterAddress(HOST_TMP1, VU0_STATUSFLAG_OFFSET) ||
				!m_code.EmitLdrImm12(HOST_TMP2, HOST_TMP1, 0) ||
				!EmitBicImm32OrReg(HOST_TMP2, HOST_TMP2, 0x30u, HOST_TMP5) ||
				!m_code.EmitOrrReg(HOST_TMP2, HOST_TMP2, HOST_TMP4) ||
				!m_code.EmitStrImm12(HOST_TMP2, HOST_TMP1, 0) ||
				!EmitVu0ViAddress(HOST_TMP1, VU0_REG_STATUS_FLAG) ||
				!m_code.EmitLdrImm12(HOST_TMP2, HOST_TMP1, 0) ||
				!EmitBicImm32OrReg(HOST_TMP2, HOST_TMP2, 0x0c30u, HOST_TMP5) ||
				!m_code.EmitOrrReg(HOST_TMP2, HOST_TMP2, HOST_TMP4) ||
				!m_code.EmitOrrRegShiftImm(HOST_TMP2, HOST_TMP2, HOST_TMP4, VitaA32::ShiftType::LSL, 6) ||
				!m_code.EmitStrImm12(HOST_TMP2, HOST_TMP1, 0))
			{
				return false;
			}
			return true;
		};

		if (!m_code.EmitMovImm8(HOST_TMP4, 0))
			return false;

		switch (fdiv.kind)
		{
			case Cop2MacroFdivKind::Div:
			{
				if (!emit_load_vf_lane(HOST_TMP0, fs, fsf) ||
					!emit_normalize_vu_float_word(HOST_TMP0) ||
					!emit_load_vf_lane(HOST_TMP1, ft, ftf) ||
					!emit_normalize_vu_float_word(HOST_TMP1) ||
					!emit_abs_word(HOST_TMP3, HOST_TMP1) ||
					!m_code.EmitCmpImm32(HOST_TMP3, 0))
				{
					return false;
				}

				const size_t divisor_nonzero = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
				if (divisor_nonzero == static_cast<size_t>(-1))
					return false;

				if (!emit_abs_word(HOST_TMP3, HOST_TMP0) ||
					!m_code.EmitMovImm8(HOST_TMP4, 0x20) ||
					!m_code.EmitCmpImm32(HOST_TMP3, 0) ||
					!m_code.EmitMovImm8(HOST_TMP4, 0x10, VitaA32::Condition::EQ) ||
					!m_code.EmitEorReg(HOST_TMP0, HOST_TMP0, HOST_TMP1) ||
					!EmitAndImm32OrReg(HOST_TMP0, HOST_TMP0, FPU_FLOAT_SIGN_MASK, HOST_TMP5) ||
					!EmitOrrImm32OrReg(HOST_TMP0, HOST_TMP0, FPU_FLOAT_MAX_FINITE, HOST_TMP5))
				{
					return false;
				}

				const size_t done_from_zero = m_code.EmitBranchPlaceholder();
				if (done_from_zero == static_cast<size_t>(-1))
					return false;

				const size_t divisor_nonzero_target = m_code.Size();
				if (!m_code.PatchBranch(divisor_nonzero, divisor_nonzero_target, VitaA32::Condition::NE) ||
					!emit_compute_div())
				{
					return false;
				}

				return m_code.PatchBranch(done_from_zero, m_code.Size()) &&
					   emit_store_q_and_status();
			}

			case Cop2MacroFdivKind::Sqrt:
				if (!emit_load_vf_lane(HOST_TMP1, ft, ftf) ||
					!emit_normalize_vu_float_word(HOST_TMP1) ||
					!emit_set_invalid_if_negative_nonzero(HOST_TMP1) ||
					!emit_compute_sqrt_abs_ft())
				{
					return false;
				}
				return emit_store_q_and_status();

			case Cop2MacroFdivKind::Rsqrt:
			{
				if (!emit_load_vf_lane(HOST_TMP0, fs, fsf) ||
					!emit_normalize_vu_float_word(HOST_TMP0) ||
					!emit_load_vf_lane(HOST_TMP1, ft, ftf) ||
					!emit_normalize_vu_float_word(HOST_TMP1) ||
					!emit_abs_word(HOST_TMP3, HOST_TMP1) ||
					!m_code.EmitCmpImm32(HOST_TMP3, 0))
				{
					return false;
				}

				const size_t ft_nonzero = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
				if (ft_nonzero == static_cast<size_t>(-1))
					return false;

				if (!emit_abs_word(HOST_TMP3, HOST_TMP0) ||
					!m_code.EmitMovImm8(HOST_TMP4, 0x20) ||
					!m_code.EmitCmpImm32(HOST_TMP3, 0))
				{
					return false;
				}

				const size_t fs_nonzero = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
				if (fs_nonzero == static_cast<size_t>(-1))
					return false;

				if (!m_code.EmitMovImm8(HOST_TMP4, 0x30) ||
					!m_code.EmitEorReg(HOST_TMP0, HOST_TMP0, HOST_TMP1) ||
					!EmitAndImm32OrReg(HOST_TMP0, HOST_TMP0, FPU_FLOAT_SIGN_MASK, HOST_TMP5))
				{
					return false;
				}

				const size_t done_from_zero_zero = m_code.EmitBranchPlaceholder();
				if (done_from_zero_zero == static_cast<size_t>(-1))
					return false;

				const size_t fs_nonzero_target = m_code.Size();
				if (!m_code.PatchBranch(fs_nonzero, fs_nonzero_target, VitaA32::Condition::NE) ||
					!m_code.EmitEorReg(HOST_TMP0, HOST_TMP0, HOST_TMP1) ||
					!EmitAndImm32OrReg(HOST_TMP0, HOST_TMP0, FPU_FLOAT_SIGN_MASK, HOST_TMP5) ||
					!EmitOrrImm32OrReg(HOST_TMP0, HOST_TMP0, FPU_FLOAT_MAX_FINITE, HOST_TMP5))
				{
					return false;
				}

				const size_t done_from_zero = m_code.EmitBranchPlaceholder();
				if (done_from_zero == static_cast<size_t>(-1))
					return false;

				const size_t ft_nonzero_target = m_code.Size();
				if (!m_code.PatchBranch(ft_nonzero, ft_nonzero_target, VitaA32::Condition::NE) ||
					!emit_set_invalid_if_negative_nonzero(HOST_TMP1) ||
					!emit_abs_word(HOST_TMP1, HOST_TMP1) ||
					!m_code.EmitVmovCoreToS(VFP_FS_S0, HOST_TMP0) ||
					!m_code.EmitVmovCoreToS(VFP_FT_S1, HOST_TMP1) ||
					!m_code.EmitVsqrtF32(VFP_FT_S1, VFP_FT_S1) ||
					!m_code.EmitVdivF32(VFP_Q_S2, VFP_FS_S0, VFP_FT_S1) ||
					!m_code.EmitVmovSToCore(HOST_TMP0, VFP_Q_S2) ||
					!emit_normalize_vu_float_word(HOST_TMP0))
				{
					return false;
				}

				const size_t done_target = m_code.Size();
				return m_code.PatchBranch(done_from_zero_zero, done_target) &&
					   m_code.PatchBranch(done_from_zero, done_target) &&
					   emit_store_q_and_status();
			}
		}

		return false;
	}

	bool BlockCompiler::EmitCOP2MacroMoveBody(u32 op)
	{
		// PCSX2 owners: VUops.cpp::_vuMOVE() / _vuMR32(). These are raw
		// bit-copy lane operations; keep self-overlap behavior by reading the
		// original X lane before any stores for VMR32.
		const Cop2MacroMoveOp move = DecodeCop2MacroMove(op);
		if (!move.valid)
			return false;

		const unsigned ft = RT(op);
		const unsigned mask = (op >> 21) & 0x0f;
		if (ft == 0 || mask == 0)
			return true;

		const unsigned fs = RD(op);
		if (mask == 0x0f)
		{
			constexpr unsigned NEON_VALUE = 0;
			return EmitVu0VfAddress(HOST_TMP0, fs) &&
				   m_code.EmitVld1Q32Aligned(NEON_VALUE, HOST_TMP0) &&
				   (!move.rotate32 || m_code.EmitVextI8Q(NEON_VALUE, NEON_VALUE, NEON_VALUE, 4)) &&
				   EmitVu0VfAddress(HOST_TMP1, ft) &&
				   m_code.EmitVst1Q32Aligned(NEON_VALUE, HOST_TMP1);
		}

		if (!EmitVu0VfAddress(HOST_TMP0, fs) ||
			!EmitVu0VfAddress(HOST_TMP1, ft))
		{
			return false;
		}

		if (move.rotate32 &&
			!m_code.EmitLdrImm12(HOST_TMP4, HOST_TMP0, 0))
		{
			return false;
		}

		const auto emit_lane_copy = [&](unsigned dest_lane, unsigned source_lane) {
			const u16 dest_offset = static_cast<u16>(dest_lane * sizeof(u32));
			const u16 source_offset = static_cast<u16>(source_lane * sizeof(u32));
			return m_code.EmitLdrImm12(HOST_TMP2, HOST_TMP0, source_offset) &&
				   m_code.EmitStrImm12(HOST_TMP2, HOST_TMP1, dest_offset);
		};

		if (move.rotate32)
		{
			if ((mask & 0x8) && !emit_lane_copy(0, 1))
				return false;
			if ((mask & 0x4) && !emit_lane_copy(1, 2))
				return false;
			if ((mask & 0x2) && !emit_lane_copy(2, 3))
				return false;
			if ((mask & 0x1) &&
				!m_code.EmitStrImm12(HOST_TMP4, HOST_TMP1, static_cast<u16>(3 * sizeof(u32))))
			{
				return false;
			}
			return true;
		}

		if ((mask & 0x8) && !emit_lane_copy(0, 0))
			return false;
		if ((mask & 0x4) && !emit_lane_copy(1, 1))
			return false;
		if ((mask & 0x2) && !emit_lane_copy(2, 2))
			return false;
		if ((mask & 0x1) && !emit_lane_copy(3, 3))
			return false;

		return true;
	}

	bool BlockCompiler::EmitCOP2MacroMinMaxSelect(bool take_max)
	{
		const auto emit_signed_select = [&](bool select_max) {
			return m_code.EmitMovRegShiftImm(HOST_TMP4, HOST_TMP2, VitaA32::ShiftType::LSL, 0) &&
				   m_code.EmitCmpReg(HOST_TMP2, HOST_TMP3) &&
				   m_code.EmitMovRegShiftImm(HOST_TMP4, HOST_TMP3, VitaA32::ShiftType::LSL, 0,
					   false, select_max ? VitaA32::Condition::LT : VitaA32::Condition::GE);
		};

		if (!m_code.EmitAndReg(HOST_TMP4, HOST_TMP2, HOST_TMP3, true))
			return false;

		const size_t both_negative = m_code.EmitBranchPlaceholder(VitaA32::Condition::MI);
		if (both_negative == static_cast<size_t>(-1))
			return false;

		if (!emit_signed_select(take_max))
			return false;

		const size_t done = m_code.EmitBranchPlaceholder();
		if (done == static_cast<size_t>(-1))
			return false;

		const size_t both_negative_target = m_code.Size();
		if (!m_code.PatchBranch(both_negative, both_negative_target, VitaA32::Condition::MI) ||
			!emit_signed_select(!take_max))
		{
			return false;
		}

		return m_code.PatchBranch(done, m_code.Size());
	}

	bool BlockCompiler::EmitCOP2MacroMinMaxBody(u32 op)
	{
		// PCSX2 owner: VUops.cpp::fp_max()/fp_min(). Those helpers compare
		// raw float bits as signed words, but swap min/max selection when both
		// inputs are negative. Keep that bit policy exactly; no host FP here.
		const Cop2MacroMinMaxOp minmax = DecodeCop2MacroMinMax(op);
		if (!minmax.valid)
			return false;

		const unsigned fd = SA(op);
		const unsigned mask = (op >> 21) & 0x0f;
		if (fd == 0 || mask == 0)
			return true;

		if (!EmitVu0VfAddress(HOST_TMP0, RD(op)) ||
			!EmitVu0VfAddress(HOST_TMP1, fd))
		{
			return false;
		}

		if (minmax.immediate_operand)
		{
			if (!EmitVu0ViAddress(HOST_TMP5, VU0_REG_I))
				return false;
		}
		else if (!EmitVu0VfAddress(HOST_TMP5, RT(op)))
		{
			return false;
		}

		const auto emit_lane = [&](unsigned lane) {
			const u16 dest_offset = static_cast<u16>(lane * sizeof(u32));
			const u16 operand_offset = static_cast<u16>(
				minmax.vector_operand ? dest_offset :
				minmax.immediate_operand ? 0 :
				minmax.broadcast_lane * sizeof(u32));

			return m_code.EmitLdrImm12(HOST_TMP2, HOST_TMP0, dest_offset) &&
				   m_code.EmitLdrImm12(HOST_TMP3, HOST_TMP5, operand_offset) &&
				   EmitCOP2MacroMinMaxSelect(minmax.take_max) &&
				   m_code.EmitStrImm12(HOST_TMP4, HOST_TMP1, dest_offset);
		};

		if ((mask & 0x8) && !emit_lane(0))
			return false;
		if ((mask & 0x4) && !emit_lane(1))
			return false;
		if ((mask & 0x2) && !emit_lane(2))
			return false;
		if ((mask & 0x1) && !emit_lane(3))
			return false;

		return true;
	}

	bool BlockCompiler::EmitCOP2MacroFast(u32 op, u32 next_pc,
		u32 raw_cycles_through_instruction, const void* event_exit)
	{
#if defined(VITASX2_QEMU_PROVIDER_FIXTURE)
		return false;
#else
		if (!event_exit || raw_cycles_through_instruction == 0)
			return false;

		size_t vu0_idle = static_cast<size_t>(-1);
		if (!EmitCOP2IdleBranch(&vu0_idle) ||
			!EmitSystemHelperEventExit(op, next_pc, raw_cycles_through_instruction,
				reinterpret_cast<const void*>(&R5900::Interpreter::OpcodeImpl::COP2), event_exit) ||
			!m_code.PatchBranch(vu0_idle, m_code.Size(), VitaA32::Condition::EQ))
		{
			return false;
		}

		return EmitCOP2MacroBody(op);
#endif
	}

	bool BlockCompiler::EmitCOP2VectorTransferFast(u32 op, u32 next_pc,
		u32 raw_cycles_through_instruction, const void* event_exit)
	{
		// PCSX2 owners: VU0.cpp::QMFC2() / QMTC2() and
		// x86/microVU_Macro.inl::recQMFC2()/recQMTC2(). Idle VU0 transfers stay
		// in the A32 block; running VU0 takes the guarded sync/event exit.
		if (!event_exit || raw_cycles_through_instruction == 0)
			return false;

		size_t vu0_idle = static_cast<size_t>(-1);
		const u32 cycles = ScaleBlockCycles(raw_cycles_through_instruction);
		if (!EmitCOP2IdleBranch(&vu0_idle) ||
			!m_code.EmitMovImm32(HOST_TMP0, op) ||
			!m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(CODE_OFFSET)) ||
			!EmitStorePc(next_pc) ||
			!EmitAddScaledCyclesToCpu(cycles) ||
			!m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&vu0Sync)) ||
			!EmitCOP2VectorTransferBody(op) ||
			!m_code.EmitCallAbsolute(event_exit) ||
			!m_code.EmitPop(m_saved_registers | REG_PC) ||
			!m_code.PatchBranch(vu0_idle, m_code.Size(), VitaA32::Condition::EQ))
		{
			return false;
		}

		return EmitCOP2VectorTransferBody(op);
	}

	bool BlockCompiler::EmitCOP2ControlReadFast(u32 op, u32 next_pc,
		u32 raw_cycles_through_instruction, const void* event_exit)
	{
		if (!event_exit || raw_cycles_through_instruction == 0)
			return false;

		size_t vu0_idle = static_cast<size_t>(-1);
		const u32 cycles = ScaleBlockCycles(raw_cycles_through_instruction);
		if (!EmitCOP2IdleBranch(&vu0_idle) ||
			!m_code.EmitMovImm32(HOST_TMP0, op) ||
			!m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(CODE_OFFSET)) ||
			!EmitStorePc(next_pc) ||
			!EmitAddScaledCyclesToCpu(cycles) ||
			!m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&vu0Sync)) ||
			!EmitCOP2ControlReadBody(op) ||
			!m_code.EmitCallAbsolute(event_exit) ||
			!m_code.EmitPop(m_saved_registers | REG_PC) ||
			!m_code.PatchBranch(vu0_idle, m_code.Size(), VitaA32::Condition::EQ))
		{
			return false;
		}

		return EmitCOP2ControlReadBody(op);
	}

	bool BlockCompiler::EmitCOP2ControlWriteFast(u32 op, u32 next_pc,
		u32 raw_cycles_through_instruction, const void* event_exit)
	{
		if (!event_exit || raw_cycles_through_instruction == 0)
			return false;

		size_t vu0_idle = static_cast<size_t>(-1);
		const u32 cycles = ScaleBlockCycles(raw_cycles_through_instruction);
		if (!EmitCOP2IdleBranch(&vu0_idle) ||
			!m_code.EmitMovImm32(HOST_TMP0, op) ||
			!m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(CODE_OFFSET)) ||
			!EmitStorePc(next_pc) ||
			!EmitAddScaledCyclesToCpu(cycles) ||
			!m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&vu0Sync)) ||
			!EmitCOP2ControlWriteBody(op) ||
			!m_code.EmitCallAbsolute(event_exit) ||
			!m_code.EmitPop(m_saved_registers | REG_PC) ||
			!m_code.PatchBranch(vu0_idle, m_code.Size(), VitaA32::Condition::EQ))
		{
			return false;
		}

		return EmitCOP2ControlWriteBody(op);
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
		const bool same_source = fs == ft;
		const unsigned vfp_ft_source = same_source ? VFP_FS_S0 : VFP_FT_S1;

		const auto normalize_arithmetic_word = [&](unsigned reg) {
			if (!m_code.EmitAndReg(HOST_TMP3, reg, HOST_TMP5) ||
				!m_code.EmitCmpReg(HOST_TMP3, HOST_TMP5))
			{
				return false;
			}

			const size_t not_infinity_or_nan = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (not_infinity_or_nan == static_cast<size_t>(-1))
				return false;

			if (!EmitAndImm32OrReg(reg, reg, FPU_FLOAT_SIGN_MASK, HOST_TMP4) ||
				!m_code.EmitSubImm8(HOST_TMP4, HOST_TMP5, 1) ||
				!m_code.EmitOrrReg(reg, reg, HOST_TMP4))
			{
				return false;
			}

			const size_t done_from_clamp = m_code.EmitBranchPlaceholder();
			if (done_from_clamp == static_cast<size_t>(-1))
				return false;

			const size_t finite_or_zero_target = m_code.Size();
			if (!m_code.PatchBranch(not_infinity_or_nan, finite_or_zero_target, VitaA32::Condition::NE) ||
				!m_code.EmitCmpImm32(HOST_TMP3, 0))
			{
				return false;
			}

			const size_t done_from_finite = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (done_from_finite == static_cast<size_t>(-1))
				return false;

			if (!EmitAndImm32OrReg(reg, reg, FPU_FLOAT_SIGN_MASK, HOST_TMP4))
			{
				return false;
			}

			const size_t done_target = m_code.Size();
			return m_code.PatchBranch(done_from_clamp, done_target) &&
				   m_code.PatchBranch(done_from_finite, done_target, VitaA32::Condition::NE);
		};

		const auto apply_overflow_underflow_flags = [&]() {
			if (!m_code.EmitLdrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(FprcOffset(31))) ||
				!m_code.EmitBicImm32(HOST_TMP2, HOST_TMP0, FPU_FLOAT_SIGN_MASK) ||
				!m_code.EmitCmpReg(HOST_TMP2, HOST_TMP5))
			{
				return false;
			}

			const size_t no_overflow = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (no_overflow == static_cast<size_t>(-1))
				return false;

			if (!EmitAndImm32OrReg(HOST_TMP0, HOST_TMP0, FPU_FLOAT_SIGN_MASK, HOST_TMP2) ||
				!m_code.EmitSubImm8(HOST_TMP2, HOST_TMP5, 1) ||
				!m_code.EmitOrrReg(HOST_TMP0, HOST_TMP0, HOST_TMP2) ||
				!EmitOrrImm32OrReg(HOST_TMP1, HOST_TMP1, FPU_FCR31_ARITHMETIC_OVERFLOW_FLAGS, HOST_TMP2))
			{
				return false;
			}

			const size_t store_result = m_code.EmitBranchPlaceholder();
			if (store_result == static_cast<size_t>(-1))
				return false;

			const size_t no_overflow_target = m_code.Size();
			if (!m_code.PatchBranch(no_overflow, no_overflow_target, VitaA32::Condition::NE) ||
				!EmitBicImm32OrReg(HOST_TMP1, HOST_TMP1, FPU_FCR31_OVERFLOW_FLAG, HOST_TMP2) ||
				!m_code.EmitAndReg(HOST_TMP3, HOST_TMP0, HOST_TMP5) ||
				!m_code.EmitCmpImm32(HOST_TMP3, 0))
			{
				return false;
			}

			const size_t no_underflow_from_exponent = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (no_underflow_from_exponent == static_cast<size_t>(-1))
				return false;

			if (!EmitAndCop1FractionMask(HOST_TMP3, HOST_TMP0) ||
				!m_code.EmitCmpImm32(HOST_TMP3, 0))
			{
				return false;
			}

			const size_t no_underflow_from_fraction = m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
			if (no_underflow_from_fraction == static_cast<size_t>(-1))
				return false;

			if (!EmitAndImm32OrReg(HOST_TMP0, HOST_TMP0, FPU_FLOAT_SIGN_MASK, HOST_TMP2) ||
				!EmitOrrImm32OrReg(HOST_TMP1, HOST_TMP1, FPU_FCR31_ARITHMETIC_UNDERFLOW_FLAGS, HOST_TMP2))
			{
				return false;
			}

			const size_t underflow_store = m_code.EmitBranchPlaceholder();
			if (underflow_store == static_cast<size_t>(-1))
				return false;

			const size_t clear_underflow_target = m_code.Size();
			if (!m_code.PatchBranch(no_underflow_from_exponent, clear_underflow_target, VitaA32::Condition::NE) ||
				!m_code.PatchBranch(no_underflow_from_fraction, clear_underflow_target, VitaA32::Condition::EQ) ||
				!EmitBicImm32OrReg(HOST_TMP1, HOST_TMP1, FPU_FCR31_UNDERFLOW_FLAG, HOST_TMP2))
			{
				return false;
			}

			const size_t store_target = m_code.Size();
			return m_code.PatchBranch(store_result, store_target) &&
				   m_code.PatchBranch(underflow_store, store_target) &&
				   m_code.EmitStrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(FprcOffset(31))) &&
				   m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(FprOffset(fd)));
		};

		const auto store_positive_zero_and_clear_overflow_underflow = [&]() {
			return m_code.EmitMovImm8(HOST_TMP0, 0) &&
				   m_code.EmitLdrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(FprcOffset(31))) &&
				   EmitBicImm32OrReg(HOST_TMP1, HOST_TMP1, FPU_FCR31_OVERFLOW_UNDERFLOW_FLAGS, HOST_TMP2) &&
				   m_code.EmitStrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(FprcOffset(31))) &&
				   m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(FprOffset(fd)));
		};

		if (function == 0x01 && fs == ft) // SUB_S
		{
			// PCSX2 FPU.cpp::fpuDouble() maps one source bit-pattern identically
			// on both sides, so SUB_S of the same FPR is +0 and clears O/U causes.
			return store_positive_zero_and_clear_overflow_underflow();
		}

		if (!m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(FprOffset(fs))) ||
			!EmitCop1ExponentMask(HOST_TMP5) ||
			!normalize_arithmetic_word(HOST_TMP0) ||
			!m_code.EmitVmovCoreToS(VFP_FS_S0, HOST_TMP0))
		{
			return false;
		}

		if (!same_source &&
			(!m_code.EmitLdrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(FprOffset(ft))) ||
				!normalize_arithmetic_word(HOST_TMP1) ||
				!m_code.EmitVmovCoreToS(VFP_FT_S1, HOST_TMP1)))
		{
			return false;
		}

		switch (function)
		{
			case 0x00: // ADD_S
				if (!m_code.EmitVaddF32(VFP_FD_S2, VFP_FS_S0, vfp_ft_source))
					return false;
				break;
			case 0x01: // SUB_S
				if (!m_code.EmitVsubF32(VFP_FD_S2, VFP_FS_S0, vfp_ft_source))
					return false;
				break;
			case 0x02: // MUL_S
				if (!m_code.EmitVmulF32(VFP_FD_S2, VFP_FS_S0, vfp_ft_source))
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
		const bool same_source = fs == ft;

		const auto normalize_arithmetic_word_with_mask = [&](unsigned reg, unsigned exponent_mask_reg) {
			if (!m_code.EmitAndReg(HOST_TMP3, reg, exponent_mask_reg) ||
				!m_code.EmitCmpReg(HOST_TMP3, exponent_mask_reg))
			{
				return false;
			}

			const size_t not_infinity_or_nan = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (not_infinity_or_nan == static_cast<size_t>(-1))
				return false;

			if (!EmitAndImm32OrReg(reg, reg, FPU_FLOAT_SIGN_MASK, HOST_TMP4) ||
				!m_code.EmitSubImm8(HOST_TMP4, exponent_mask_reg, 1) ||
				!m_code.EmitOrrReg(reg, reg, HOST_TMP4))
			{
				return false;
			}

			const size_t done_from_clamp = m_code.EmitBranchPlaceholder();
			if (done_from_clamp == static_cast<size_t>(-1))
				return false;

			const size_t finite_or_zero_target = m_code.Size();
			if (!m_code.PatchBranch(not_infinity_or_nan, finite_or_zero_target, VitaA32::Condition::NE) ||
				!m_code.EmitCmpImm32(HOST_TMP3, 0))
			{
				return false;
			}

			const size_t done_from_finite = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (done_from_finite == static_cast<size_t>(-1))
				return false;

			if (!EmitAndImm32OrReg(reg, reg, FPU_FLOAT_SIGN_MASK, HOST_TMP4))
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

		const auto clamp_result_no_flags_with_mask = [&](bool store_fcr31, unsigned exponent_mask_reg) {
			if (!m_code.EmitBicImm32(HOST_TMP2, HOST_TMP0, FPU_FLOAT_SIGN_MASK) ||
				!m_code.EmitCmpReg(HOST_TMP2, exponent_mask_reg))
			{
				return false;
			}

			const size_t no_overflow = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (no_overflow == static_cast<size_t>(-1))
				return false;

			if (!EmitAndImm32OrReg(HOST_TMP0, HOST_TMP0, FPU_FLOAT_SIGN_MASK, HOST_TMP2) ||
				!m_code.EmitSubImm8(HOST_TMP2, exponent_mask_reg, 1) ||
				!m_code.EmitOrrReg(HOST_TMP0, HOST_TMP0, HOST_TMP2))
			{
				return false;
			}

			const size_t store_after_overflow = m_code.EmitBranchPlaceholder();
			if (store_after_overflow == static_cast<size_t>(-1))
				return false;

			const size_t no_overflow_target = m_code.Size();
			if (!m_code.PatchBranch(no_overflow, no_overflow_target, VitaA32::Condition::NE) ||
				!m_code.EmitAndReg(HOST_TMP3, HOST_TMP0, exponent_mask_reg) ||
				!m_code.EmitCmpImm32(HOST_TMP3, 0))
			{
				return false;
			}

			const size_t no_underflow_from_exponent = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (no_underflow_from_exponent == static_cast<size_t>(-1))
				return false;

			if (!EmitAndCop1FractionMask(HOST_TMP3, HOST_TMP0) ||
				!m_code.EmitCmpImm32(HOST_TMP3, 0))
			{
				return false;
			}

			const size_t no_underflow_from_fraction = m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
			if (no_underflow_from_fraction == static_cast<size_t>(-1))
				return false;

			if (!EmitAndImm32OrReg(HOST_TMP0, HOST_TMP0, FPU_FLOAT_SIGN_MASK, HOST_TMP2))
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
				   EmitBicImm32OrReg(HOST_TMP5, HOST_TMP5, FPU_FCR31_INVALID_DIVIDE_CAUSE_FLAGS, HOST_TMP2);
		};

		const auto emit_divide_by_zero_result = [&](unsigned exponent_mask_reg) {
			if (!m_code.EmitLdrImm12(HOST_TMP5, HOST_CPU_REGS, static_cast<u16>(FprcOffset(31))) ||
				!m_code.EmitAndReg(HOST_TMP3, HOST_TMP0, exponent_mask_reg) ||
				!m_code.EmitCmpImm32(HOST_TMP3, 0))
			{
				return false;
			}

			const size_t dividend_nonzero = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (dividend_nonzero == static_cast<size_t>(-1))
				return false;

			if (!EmitOrrImm32OrReg(HOST_TMP5, HOST_TMP5, FPU_FCR31_INVALID_FLAGS, HOST_TMP4))
				return false;

			const size_t flags_ready = m_code.EmitBranchPlaceholder();
			if (flags_ready == static_cast<size_t>(-1))
				return false;

			const size_t dividend_nonzero_target = m_code.Size();
			if (!m_code.PatchBranch(dividend_nonzero, dividend_nonzero_target, VitaA32::Condition::NE) ||
				!EmitOrrImm32OrReg(HOST_TMP5, HOST_TMP5, FPU_FCR31_DIVIDE_BY_ZERO_FLAGS, HOST_TMP4))
			{
				return false;
			}

			const size_t flags_ready_target = m_code.Size();
			return m_code.PatchBranch(flags_ready, flags_ready_target) &&
				   m_code.EmitSubImm8(HOST_TMP3, exponent_mask_reg, 1) &&
				   m_code.EmitEorReg(HOST_TMP0, HOST_TMP0, HOST_TMP1) &&
				   EmitAndImm32OrReg(HOST_TMP0, HOST_TMP0, FPU_FLOAT_SIGN_MASK, HOST_TMP2) &&
				   m_code.EmitOrrReg(HOST_TMP0, HOST_TMP0, HOST_TMP3) &&
				   store_result(true);
		};

		if (function == 0x03 && same_source) // DIV_S
		{
			if (!m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(FprOffset(fs))) ||
				!EmitCop1ExponentMask(HOST_TMP2) ||
				!m_code.EmitAndReg(HOST_TMP3, HOST_TMP0, HOST_TMP2) ||
				!m_code.EmitCmpImm32(HOST_TMP3, 0))
			{
				return false;
			}

			const size_t source_nonzero = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (source_nonzero == static_cast<size_t>(-1))
				return false;

			if (!m_code.EmitLdrImm12(HOST_TMP5, HOST_CPU_REGS, static_cast<u16>(FprcOffset(31))) ||
				!EmitOrrImm32OrReg(HOST_TMP5, HOST_TMP5, FPU_FCR31_INVALID_FLAGS, HOST_TMP4) ||
				!m_code.EmitSubImm8(HOST_TMP0, HOST_TMP2, 1) ||
				!store_result(true))
			{
				return false;
			}

			const size_t done_from_zero = m_code.EmitBranchPlaceholder();
			if (done_from_zero == static_cast<size_t>(-1))
				return false;

			const size_t source_nonzero_target = m_code.Size();
			// PCSX2 FPU.cpp::DIV_S() first rejects exponent-zero divisors.
			// Otherwise identical fpuDouble() inputs divide to +1 without
			// touching FCR31 because checkOverflow/Underflow receive flags=0.
			return m_code.PatchBranch(source_nonzero, source_nonzero_target, VitaA32::Condition::NE) &&
				   m_code.EmitMovImm32(HOST_TMP0, FPU_FLOAT_ONE) &&
				   store_result(false) &&
				   m_code.PatchBranch(done_from_zero, m_code.Size());
		}

		if (function == 0x03) // DIV_S
		{
			if (!m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(FprOffset(fs))) ||
				!m_code.EmitLdrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(FprOffset(ft))) ||
				!EmitCop1ExponentMask(HOST_TMP2) ||
				!m_code.EmitAndReg(HOST_TMP3, HOST_TMP1, HOST_TMP2) ||
				!m_code.EmitCmpImm32(HOST_TMP3, 0))
			{
				return false;
			}

			const size_t divisor_nonzero = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (divisor_nonzero == static_cast<size_t>(-1))
				return false;

			if (!emit_divide_by_zero_result(HOST_TMP2))
				return false;

			const size_t done_from_zero = m_code.EmitBranchPlaceholder();
			if (done_from_zero == static_cast<size_t>(-1))
				return false;

			const size_t divisor_nonzero_target = m_code.Size();
			if (!m_code.PatchBranch(divisor_nonzero, divisor_nonzero_target, VitaA32::Condition::NE))
			{
				return false;
			}

			if (!m_code.EmitMovRegShiftImm(HOST_TMP5, HOST_TMP2, VitaA32::ShiftType::LSL, 0) ||
				!normalize_arithmetic_word_with_mask(HOST_TMP0, HOST_TMP5) ||
				!normalize_arithmetic_word_with_mask(HOST_TMP1, HOST_TMP5) ||
				!m_code.EmitVmovCoreToS(VFP_FS_S0, HOST_TMP0) ||
				!m_code.EmitVmovCoreToS(VFP_FT_S1, HOST_TMP1) ||
				!m_code.EmitVdivF32(VFP_FD_S2, VFP_FS_S0, VFP_FT_S1) ||
				!m_code.EmitVmovSToCore(HOST_TMP0, VFP_FD_S2) ||
				!clamp_result_no_flags_with_mask(false, HOST_TMP5))
			{
				return false;
			}

			return m_code.PatchBranch(done_from_zero, m_code.Size());
		}

		if (function == 0x04) // SQRT_S
		{
			if (!m_code.EmitLdrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(FprOffset(ft))) ||
				!clear_invalid_divide_causes() ||
				!EmitCop1ExponentMask(HOST_TMP2) ||
				!m_code.EmitAndReg(HOST_TMP3, HOST_TMP1, HOST_TMP2) ||
				!m_code.EmitCmpImm32(HOST_TMP3, 0))
			{
				return false;
			}

			const size_t operand_nonzero = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (operand_nonzero == static_cast<size_t>(-1))
				return false;

			if (!EmitAndImm32OrReg(HOST_TMP0, HOST_TMP1, FPU_FLOAT_SIGN_MASK, HOST_TMP2))
			{
				return false;
			}

			const size_t done_from_zero = m_code.EmitBranchPlaceholder();
			if (done_from_zero == static_cast<size_t>(-1))
				return false;

			const size_t operand_nonzero_target = m_code.Size();
			if (!m_code.PatchBranch(operand_nonzero, operand_nonzero_target, VitaA32::Condition::NE) ||
				!EmitAndImm32OrReg(HOST_TMP3, HOST_TMP1, FPU_FLOAT_SIGN_MASK, HOST_TMP4) ||
				!m_code.EmitCmpImm32(HOST_TMP3, 0))
			{
				return false;
			}

			const size_t operand_positive = m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
			if (operand_positive == static_cast<size_t>(-1))
				return false;

			if (!EmitOrrImm32OrReg(HOST_TMP5, HOST_TMP5, FPU_FCR31_INVALID_FLAGS, HOST_TMP4) ||
				!normalize_arithmetic_word_with_mask(HOST_TMP1, HOST_TMP2) ||
				!EmitBicImm32OrReg(HOST_TMP1, HOST_TMP1, FPU_FLOAT_SIGN_MASK, HOST_TMP4))
			{
				return false;
			}

			const size_t operand_ready = m_code.EmitBranchPlaceholder();
			if (operand_ready == static_cast<size_t>(-1))
				return false;

			const size_t operand_positive_target = m_code.Size();
			if (!m_code.PatchBranch(operand_positive, operand_positive_target, VitaA32::Condition::EQ) ||
				!normalize_arithmetic_word_with_mask(HOST_TMP1, HOST_TMP2))
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
				!EmitCop1ExponentMask(HOST_TMP2) ||
				!m_code.EmitAndReg(HOST_TMP3, HOST_TMP1, HOST_TMP2) ||
				!m_code.EmitCmpImm32(HOST_TMP3, 0))
			{
				return false;
			}

			const size_t operand_nonzero = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (operand_nonzero == static_cast<size_t>(-1))
				return false;

			if (!EmitOrrImm32OrReg(HOST_TMP5, HOST_TMP5, FPU_FCR31_DIVIDE_BY_ZERO_FLAGS, HOST_TMP4) ||
				!m_code.EmitSubImm8(HOST_TMP4, HOST_TMP2, 1) ||
				!EmitAndImm32OrReg(HOST_TMP0, HOST_TMP1, FPU_FLOAT_SIGN_MASK, HOST_TMP2) ||
				!m_code.EmitOrrReg(HOST_TMP0, HOST_TMP0, HOST_TMP4) ||
				!store_result(true))
			{
				return false;
			}

			const size_t done_from_zero = m_code.EmitBranchPlaceholder();
			if (done_from_zero == static_cast<size_t>(-1))
				return false;

			const size_t operand_nonzero_target = m_code.Size();
			if (!m_code.PatchBranch(operand_nonzero, operand_nonzero_target, VitaA32::Condition::NE) ||
				!EmitAndImm32OrReg(HOST_TMP3, HOST_TMP1, FPU_FLOAT_SIGN_MASK, HOST_TMP4) ||
				!m_code.EmitCmpImm32(HOST_TMP3, 0))
			{
				return false;
			}

			const size_t operand_positive = m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
			if (operand_positive == static_cast<size_t>(-1))
				return false;

			if (!EmitOrrImm32OrReg(HOST_TMP5, HOST_TMP5, FPU_FCR31_INVALID_FLAGS, HOST_TMP4) ||
				!normalize_arithmetic_word_with_mask(HOST_TMP1, HOST_TMP2) ||
				!EmitBicImm32OrReg(HOST_TMP1, HOST_TMP1, FPU_FLOAT_SIGN_MASK, HOST_TMP4))
			{
				return false;
			}

			const size_t operand_ready = m_code.EmitBranchPlaceholder();
			if (operand_ready == static_cast<size_t>(-1))
				return false;

			const size_t operand_positive_target = m_code.Size();
			if (!m_code.PatchBranch(operand_positive, operand_positive_target, VitaA32::Condition::EQ) ||
				!normalize_arithmetic_word_with_mask(HOST_TMP1, HOST_TMP2))
			{
				return false;
			}

			const size_t operand_ready_target = m_code.Size();
			if (!m_code.PatchBranch(operand_ready, operand_ready_target) ||
				!normalize_arithmetic_word_with_mask(HOST_TMP0, HOST_TMP2) ||
				!m_code.EmitVmovCoreToS(VFP_FS_S0, HOST_TMP0) ||
				!m_code.EmitVmovCoreToS(VFP_FT_S1, HOST_TMP1) ||
				!m_code.EmitVsqrtF32(VFP_FT_S1, VFP_FT_S1) ||
				!m_code.EmitVdivF32(VFP_FD_S2, VFP_FS_S0, VFP_FT_S1) ||
				!m_code.EmitVmovSToCore(HOST_TMP0, VFP_FD_S2) ||
				!m_code.EmitMovRegShiftImm(HOST_TMP3, HOST_TMP2, VitaA32::ShiftType::LSL, 0) ||
				!clamp_result_no_flags_with_mask(true, HOST_TMP3))
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
		const bool same_source = fs == ft;
		const unsigned vfp_ft_source = same_source ? VFP_FS_S0 : VFP_FT_S1;

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
			if (!m_code.EmitAndReg(HOST_TMP3, reg, HOST_TMP5) ||
				!m_code.EmitCmpReg(HOST_TMP3, HOST_TMP5))
			{
				return false;
			}

			const size_t not_infinity_or_nan = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (not_infinity_or_nan == static_cast<size_t>(-1))
				return false;

			if (!EmitAndImm32OrReg(reg, reg, FPU_FLOAT_SIGN_MASK, HOST_TMP4) ||
				!m_code.EmitSubImm8(HOST_TMP4, HOST_TMP5, 1) ||
				!m_code.EmitOrrReg(reg, reg, HOST_TMP4))
			{
				return false;
			}

			const size_t done_from_clamp = m_code.EmitBranchPlaceholder();
			if (done_from_clamp == static_cast<size_t>(-1))
				return false;

			const size_t finite_or_zero_target = m_code.Size();
			if (!m_code.PatchBranch(not_infinity_or_nan, finite_or_zero_target, VitaA32::Condition::NE) ||
				!m_code.EmitCmpImm32(HOST_TMP3, 0))
			{
				return false;
			}

			const size_t done_from_finite = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (done_from_finite == static_cast<size_t>(-1))
				return false;

			if (!EmitAndImm32OrReg(reg, reg, FPU_FLOAT_SIGN_MASK, HOST_TMP4))
			{
				return false;
			}

			const size_t done_target = m_code.Size();
			return m_code.PatchBranch(done_from_clamp, done_target) &&
				   m_code.PatchBranch(done_from_finite, done_target, VitaA32::Condition::NE);
		};

		const auto apply_overflow_underflow_flags = [&](size_t dest_offset) {
			if (!m_code.EmitLdrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(FprcOffset(31))) ||
				!m_code.EmitBicImm32(HOST_TMP2, HOST_TMP0, FPU_FLOAT_SIGN_MASK) ||
				!m_code.EmitCmpReg(HOST_TMP2, HOST_TMP5))
			{
				return false;
			}

			const size_t no_overflow = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (no_overflow == static_cast<size_t>(-1))
				return false;

			if (!EmitAndImm32OrReg(HOST_TMP0, HOST_TMP0, FPU_FLOAT_SIGN_MASK, HOST_TMP2) ||
				!m_code.EmitSubImm8(HOST_TMP2, HOST_TMP5, 1) ||
				!m_code.EmitOrrReg(HOST_TMP0, HOST_TMP0, HOST_TMP2) ||
				!EmitOrrImm32OrReg(HOST_TMP1, HOST_TMP1, FPU_FCR31_ARITHMETIC_OVERFLOW_FLAGS, HOST_TMP2))
			{
				return false;
			}

			const size_t store_result = m_code.EmitBranchPlaceholder();
			if (store_result == static_cast<size_t>(-1))
				return false;

			const size_t no_overflow_target = m_code.Size();
			if (!m_code.PatchBranch(no_overflow, no_overflow_target, VitaA32::Condition::NE) ||
				!EmitBicImm32OrReg(HOST_TMP1, HOST_TMP1, FPU_FCR31_OVERFLOW_FLAG, HOST_TMP2) ||
				!m_code.EmitAndReg(HOST_TMP3, HOST_TMP0, HOST_TMP5) ||
				!m_code.EmitCmpImm32(HOST_TMP3, 0))
			{
				return false;
			}

			const size_t no_underflow_from_exponent = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (no_underflow_from_exponent == static_cast<size_t>(-1))
				return false;

			if (!EmitAndCop1FractionMask(HOST_TMP3, HOST_TMP0) ||
				!m_code.EmitCmpImm32(HOST_TMP3, 0))
			{
				return false;
			}

			const size_t no_underflow_from_fraction = m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
			if (no_underflow_from_fraction == static_cast<size_t>(-1))
				return false;

			if (!EmitAndImm32OrReg(HOST_TMP0, HOST_TMP0, FPU_FLOAT_SIGN_MASK, HOST_TMP2) ||
				!EmitOrrImm32OrReg(HOST_TMP1, HOST_TMP1, FPU_FCR31_ARITHMETIC_UNDERFLOW_FLAGS, HOST_TMP2))
			{
				return false;
			}

			const size_t underflow_store = m_code.EmitBranchPlaceholder();
			if (underflow_store == static_cast<size_t>(-1))
				return false;

			const size_t clear_underflow_target = m_code.Size();
			if (!m_code.PatchBranch(no_underflow_from_exponent, clear_underflow_target, VitaA32::Condition::NE) ||
				!m_code.PatchBranch(no_underflow_from_fraction, clear_underflow_target, VitaA32::Condition::EQ) ||
				!EmitBicImm32OrReg(HOST_TMP1, HOST_TMP1, FPU_FCR31_UNDERFLOW_FLAG, HOST_TMP2))
			{
				return false;
			}

			const size_t store_target = m_code.Size();
			return m_code.PatchBranch(store_result, store_target) &&
				   m_code.PatchBranch(underflow_store, store_target) &&
				   m_code.EmitStrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(FprcOffset(31))) &&
				   m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(dest_offset));
		};

		const auto store_positive_zero_and_clear_overflow_underflow = [&](size_t dest_offset) {
			return m_code.EmitMovImm8(HOST_TMP0, 0) &&
				   m_code.EmitLdrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(FprcOffset(31))) &&
				   EmitBicImm32OrReg(HOST_TMP1, HOST_TMP1, FPU_FCR31_OVERFLOW_UNDERFLOW_FLAGS, HOST_TMP2) &&
				   m_code.EmitStrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(FprcOffset(31))) &&
				   m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(dest_offset));
		};

		const auto load_normalized_operands = [&]() {
			return m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(FprOffset(fs))) &&
				   EmitCop1ExponentMask(HOST_TMP5) &&
				   normalize_arithmetic_word(HOST_TMP0) &&
				   m_code.EmitVmovCoreToS(VFP_FS_S0, HOST_TMP0) &&
				   (same_source ||
					   (m_code.EmitLdrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(FprOffset(ft))) &&
						   normalize_arithmetic_word(HOST_TMP1) &&
						   m_code.EmitVmovCoreToS(VFP_FT_S1, HOST_TMP1)));
		};

		if (function == 0x19 && fs == ft) // SUBA_S
		{
			// PCSX2 FPU.cpp::fpuDouble() normalizes both operands identically
			// for same-FPR SUBA_S, producing +0 in ACC with O/U causes cleared.
			return store_positive_zero_and_clear_overflow_underflow(destination_offset());
		}

		if (!load_normalized_operands())
			return false;

		switch (function)
		{
			case 0x18: // ADDA_S
				if (!m_code.EmitVaddF32(VFP_FD_S2, VFP_FS_S0, vfp_ft_source))
					return false;
				break;
			case 0x19: // SUBA_S
				if (!m_code.EmitVsubF32(VFP_FD_S2, VFP_FS_S0, vfp_ft_source))
					return false;
				break;
			case 0x1a: // MULA_S
			case 0x1c: // MADD_S
			case 0x1d: // MSUB_S
			case 0x1e: // MADDA_S
			case 0x1f: // MSUBA_S
				if (!m_code.EmitVmulF32(VFP_FD_S2, VFP_FS_S0, vfp_ft_source))
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
				   EmitBicImm32OrReg(HOST_TMP0, HOST_TMP0, FPU_FCR31_OVERFLOW_UNDERFLOW_FLAGS, HOST_TMP1) &&
				   m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(FprcOffset(31)));
		};

		if (function == 0x06 && fd == fs) // MOV_S
			return true;

		if ((function == 0x28 || function == 0x29) && fs == ft) // MAX_S / MIN_S
		{
			// PCSX2 FPU.cpp::fp_max()/fp_min() return the identical source word
			// for same-FPR operands; MAX_S/MIN_S still clear O/U causes.
			if (fd != fs &&
				(!m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(FprOffset(fs))) ||
					!m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(FprOffset(fd)))))
			{
				return false;
			}
			return clear_overflow_underflow();
		}

		if (!m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(FprOffset(fs))))
			return false;

		switch (function)
		{
			case 0x05: // ABS_S
				if (!m_code.EmitBicImm32(HOST_TMP0, HOST_TMP0, FPU_FLOAT_SIGN_MASK))
				{
					return false;
				}
				break;
			case 0x06: // MOV_S
				break;
			case 0x07: // NEG_S
				if (!EmitEorImm32OrReg(HOST_TMP0, HOST_TMP0, FPU_FLOAT_SIGN_MASK, HOST_TMP1))
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
					!m_code.EmitAndReg(HOST_TMP4, HOST_TMP0, HOST_TMP1) ||
					!EmitAndImm32OrReg(HOST_TMP4, HOST_TMP4, FPU_FLOAT_SIGN_MASK, HOST_TMP3) ||
					!m_code.EmitCmpImm32(HOST_TMP4, FPU_FLOAT_SIGN_MASK))
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
				   EmitBicImm32OrReg(HOST_TMP0, HOST_TMP0, FPU_FCR31_CONDITION_FLAG, HOST_TMP1) &&
				   m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(FprcOffset(31)));
		};
		const auto set_condition_flag = [&]() {
			return m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(FprcOffset(31))) &&
				   EmitOrrImm32OrReg(HOST_TMP0, HOST_TMP0, FPU_FCR31_CONDITION_FLAG, HOST_TMP1) &&
				   m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(FprcOffset(31)));
		};

		const auto normalize_compare_word = [&](unsigned reg) {
			if (!m_code.EmitAndReg(HOST_TMP3, reg, HOST_TMP5) ||
				!m_code.EmitCmpReg(HOST_TMP3, HOST_TMP5))
			{
				return false;
			}

			const size_t not_infinity_or_nan = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (not_infinity_or_nan == static_cast<size_t>(-1))
				return false;

			if (!EmitAndImm32OrReg(reg, reg, FPU_FLOAT_SIGN_MASK, HOST_TMP4) ||
				!m_code.EmitSubImm8(HOST_TMP4, HOST_TMP5, 1) ||
				!m_code.EmitOrrReg(reg, reg, HOST_TMP4))
			{
				return false;
			}

			const size_t done_from_clamp = m_code.EmitBranchPlaceholder();
			if (done_from_clamp == static_cast<size_t>(-1))
				return false;

			const size_t finite_or_zero_target = m_code.Size();
			if (!m_code.PatchBranch(not_infinity_or_nan, finite_or_zero_target, VitaA32::Condition::NE) ||
				!m_code.EmitCmpImm32(HOST_TMP3, 0))
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
				   EmitOrrImm32OrReg(HOST_TMP2, HOST_TMP2, FPU_FLOAT_SIGN_MASK, HOST_TMP3) &&
				   m_code.EmitEorReg(reg, reg, HOST_TMP2);
		};

		const auto inverse_condition = [](VitaA32::Condition condition, VitaA32::Condition* inverse) {
			switch (condition)
			{
				case VitaA32::Condition::EQ:
					*inverse = VitaA32::Condition::NE;
					return true;
				case VitaA32::Condition::CC:
					*inverse = VitaA32::Condition::CS;
					return true;
				case VitaA32::Condition::LS:
					*inverse = VitaA32::Condition::HI;
					return true;
				default:
					return false;
			}
		};

		const auto apply_condition_flag = [&](VitaA32::Condition condition) {
			VitaA32::Condition clear_only_condition = VitaA32::Condition::AL;
			if (!inverse_condition(condition, &clear_only_condition) ||
				!m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(FprcOffset(31))) ||
				!EmitBicImm32OrReg(HOST_TMP0, HOST_TMP0, FPU_FCR31_CONDITION_FLAG, HOST_TMP2))
			{
				return false;
			}

			const size_t clear_only = m_code.EmitBranchPlaceholder(clear_only_condition);
			if (clear_only == static_cast<size_t>(-1))
				return false;

			if (!EmitOrrImm32OrReg(HOST_TMP0, HOST_TMP0, FPU_FCR31_CONDITION_FLAG, HOST_TMP1))
				return false;

			const size_t store_target = m_code.Size();
			return m_code.PatchBranch(clear_only, store_target, clear_only_condition) &&
				   m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(FprcOffset(31)));
		};

		if (function == 0x30) // C_F
			return clear_condition_flag();
		if (fs == ft)
		{
			// PCSX2 FPU.cpp::fpuDouble() maps one source bit-pattern identically
			// on both sides, including exponent-0 and exponent-0xff clamps.
			if (function == 0x32 || function == 0x36) // C_EQ / C_LE
				return set_condition_flag();
			if (function == 0x34) // C_LT
				return clear_condition_flag();
		}

		if (!m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(FprOffset(fs))) ||
			!m_code.EmitLdrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(FprOffset(ft))) ||
			!EmitCop1ExponentMask(HOST_TMP5) ||
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
			!EmitAndCop1ExponentMask(HOST_TMP2, HOST_TMP0, HOST_TMP1) ||
			!EmitCmpImm32OrReg(HOST_TMP2, FPU_CVT_W_MAX_EXPONENT_MASK, HOST_TMP3))
		{
			return false;
		}

		const size_t convert_path = m_code.EmitBranchPlaceholder(VitaA32::Condition::LS);
		if (convert_path == static_cast<size_t>(-1))
			return false;

		if (!m_code.EmitTstImm32(HOST_TMP0, FPU_FLOAT_SIGN_MASK) ||
			!m_code.EmitMovImm32(HOST_TMP0, 0x7fffffffu) ||
			!m_code.EmitMovImm32(HOST_TMP0, FPU_FLOAT_SIGN_MASK, VitaA32::Condition::NE))
		{
			return false;
		}

		const size_t store_result = m_code.EmitBranchPlaceholder();
		if (store_result == static_cast<size_t>(-1))
			return false;

		const size_t convert_target = m_code.Size();
		if (!m_code.PatchBranch(convert_path, convert_target, VitaA32::Condition::LS) ||
			!m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP2, VitaA32::ShiftType::LSR, 23) ||
			!EmitCmpImm32OrReg(HOST_TMP2, FPU_FLOAT_EXPONENT_BIAS, HOST_TMP3))
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
			!EmitAndCop1FractionMask(HOST_TMP3, HOST_TMP0) ||
			!EmitOrrImm32OrReg(HOST_TMP3, HOST_TMP3, FPU_FLOAT_IMPLICIT_MANTISSA, HOST_TMP4) ||
			!m_code.EmitMovImm8(HOST_TMP4, FPU_FLOAT_EXPONENT_BIAS) ||
			!m_code.EmitSubReg(HOST_TMP2, HOST_TMP2, HOST_TMP4) ||
			!m_code.EmitCmpImm32(HOST_TMP2, FPU_FLOAT_MANTISSA_BITS))
		{
			return false;
		}

		const size_t left_shift_path = m_code.EmitBranchPlaceholder(VitaA32::Condition::CS);
		if (left_shift_path == static_cast<size_t>(-1))
			return false;

		if (!m_code.EmitRsbImm32(HOST_TMP4, HOST_TMP2, FPU_FLOAT_MANTISSA_BITS) ||
			!m_code.EmitMovRegShiftReg(HOST_TMP3, HOST_TMP3, VitaA32::ShiftType::LSR, HOST_TMP4))
		{
			return false;
		}

		const size_t apply_sign = m_code.EmitBranchPlaceholder();
		if (apply_sign == static_cast<size_t>(-1))
			return false;

		const size_t left_shift_target = m_code.Size();
		if (!m_code.PatchBranch(left_shift_path, left_shift_target, VitaA32::Condition::CS) ||
			!m_code.EmitSubImm32(HOST_TMP4, HOST_TMP2, FPU_FLOAT_MANTISSA_BITS) ||
			!m_code.EmitMovRegShiftReg(HOST_TMP3, HOST_TMP3, VitaA32::ShiftType::LSL, HOST_TMP4))
		{
			return false;
		}

		const size_t apply_sign_target = m_code.Size();
		if (!m_code.PatchBranch(apply_sign, apply_sign_target) ||
			!EmitAndImm32OrReg(HOST_TMP4, HOST_TMP0, FPU_FLOAT_SIGN_MASK, HOST_TMP1, true) ||
			!m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_TMP3, VitaA32::ShiftType::LSL, 0))
		{
			return false;
		}

		const size_t positive_done = m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
		if (positive_done == static_cast<size_t>(-1))
			return false;

		if (!m_code.EmitRsbImm32(HOST_TMP0, HOST_TMP3, 0))
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
		(void)pc;
		(void)raw_cycles_through_instruction;
		(void)event_exit;

		if (IsNoOpCACHE(op))
			return true;

		if (!IsHelperCACHE(op))
			return false;

		using namespace R5900::Interpreter::OpcodeImpl;
		// PCSX2 owner: Cache.cpp::CACHE(). Keep data-cache/TagLo modes inside
		// the generated A32 tail so the normal cycle test can direct-exit instead
		// of forcing an interpreter event split after every CACHE instruction.
		return m_code.EmitMovImm32(HOST_TMP0, op) &&
			   m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(CODE_OFFSET)) &&
			   m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&CACHE));
	}

	bool BlockCompiler::EmitSpecialExceptionEventExit(u32 op, u32 pc, u32 raw_cycles_through_instruction,
		const void* event_exit, bool branch_delay_slot, const void* helper)
	{
		if (!event_exit || !helper || raw_cycles_through_instruction == 0)
			return false;

		const u32 cycles = ScaleBlockCycles(raw_cycles_through_instruction);
		if (!m_code.EmitMovImm32(HOST_TMP0, op) ||
			!m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(CODE_OFFSET)) ||
			!EmitStorePc(pc + 4) ||
			!m_code.EmitMovImm8(HOST_TMP0, branch_delay_slot ? 1 : 0) ||
			!m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(BRANCH_OFFSET)) ||
			!EmitAddScaledCyclesToCpu(cycles) ||
			!m_code.EmitCallAbsolute(helper))
		{
			return false;
		}

		return m_code.EmitCallAbsolute(event_exit) &&
			   m_code.EmitPop(m_saved_registers | REG_PC);
	}

	bool BlockCompiler::EmitSYSCALL(u32 op, u32 pc, u32 raw_cycles_through_instruction,
		const void* event_exit, bool branch_delay_slot)
	{
		using namespace R5900::Interpreter::OpcodeImpl;
		return EmitSpecialExceptionEventExit(op, pc, raw_cycles_through_instruction,
			event_exit, branch_delay_slot, reinterpret_cast<const void*>(&SYSCALL));
	}

	bool BlockCompiler::EmitBREAK(u32 op, u32 pc, u32 raw_cycles_through_instruction,
		const void* event_exit, bool branch_delay_slot)
	{
		using namespace R5900::Interpreter::OpcodeImpl;
		return EmitSpecialExceptionEventExit(op, pc, raw_cycles_through_instruction,
			event_exit, branch_delay_slot, reinterpret_cast<const void*>(&BREAK));
	}

	bool BlockCompiler::EmitTrapEventExit(u32 op, u32 pc, u32 raw_cycles_through_instruction,
		const void* event_exit, bool branch_delay_slot)
	{
		// PCSX2 owners: R5900OpcodeImpl.cpp::TGE/TGEU/TLT/TLTU/TEQ/TNE and
		// TGEI/TGEIU/TLTI/TLTIU/TEQI/TNEI. The x86 recompiler keeps these as
		// recBranchCall() helper tails, so the first A32 port does the same.
		return EmitSpecialExceptionEventExit(op, pc, raw_cycles_through_instruction,
			event_exit, branch_delay_slot, TrapHelperForOpcode(op));
	}

	bool BlockCompiler::EmitADDIU(u32 op)
	{
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		const s32 imm = static_cast<s32>(IMM_S(op));

		if (rt == 0)
			return true;

		// PCSX2 owner: R5900OpcodeImpl.cpp::ADDIU() sign-extends the 32-bit
		// result of RS.low + sign_extend_16(imm). With RS=$zero that is just
		// the sign-extended immediate.
		if (rs == 0)
		{
			return m_code.EmitMovImm32(HOST_TMP0, static_cast<u32>(imm)) &&
				   m_code.EmitMovRegShiftImm(HOST_TMP1, HOST_TMP0, VitaA32::ShiftType::ASR, 31) &&
				   EmitStoreGpr64(rt, HOST_TMP0, HOST_TMP1);
		}

		unsigned rs_host;
		if (!EmitGprLowOperand(rs, HOST_TMP0, &rs_host))
			return false;

		const bool encoded_imm = (imm >= 0 && m_code.EmitAddImm32(HOST_TMP0, rs_host, static_cast<u32>(imm))) ||
								 (imm < 0 && m_code.EmitSubImm32(HOST_TMP0, rs_host, static_cast<u32>(-imm)));
		if (!encoded_imm)
		{
			if (!m_code.EmitMovImm32(HOST_TMP2, static_cast<u32>(imm)) ||
				!m_code.EmitAddReg(HOST_TMP0, rs_host, HOST_TMP2))
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

		// PCSX2 owner: R5900OpcodeImpl.cpp::DADDIU() sign-extends the 16-bit
		// immediate and adds it to the low 64-bit GPR value. With RS=$zero the
		// result is exactly that sign-extended immediate.
		if (rs == 0)
		{
			return m_code.EmitMovImm32(HOST_TMP0, static_cast<u32>(imm)) &&
				   m_code.EmitMovImm32(HOST_TMP1, (imm < 0) ? 0xffffffffu : 0) &&
				   EmitStoreGpr64(rt, HOST_TMP0, HOST_TMP1);
		}
		if (imm == 0 && rt == rs)
			return true;

		unsigned rs_low;
		if (!EmitGpr64OperandLow(rs, HOST_TMP0, HOST_TMP1, &rs_low))
			return false;

		// PCSX2 owner: R5900OpcodeImpl.cpp::DADDIU() sign-extends the 16-bit
		// immediate and performs the 64-bit add without overflow trapping.
		if (imm == 0)
			return EmitStoreGpr64(rt, rs_low, HOST_TMP1);

		if (imm > 0 && m_code.EmitAddImm32(HOST_TMP0, rs_low, static_cast<u32>(imm), true))
			return m_code.EmitAdcImm8(HOST_TMP1, HOST_TMP1, 0) &&
				   EmitStoreGpr64(rt, HOST_TMP0, HOST_TMP1);

		if (imm < 0 && m_code.EmitSubImm32(HOST_TMP0, rs_low, static_cast<u32>(-imm), true))
			return m_code.EmitSbcImm8(HOST_TMP1, HOST_TMP1, 0) &&
				   EmitStoreGpr64(rt, HOST_TMP0, HOST_TMP1);

		return m_code.EmitMovImm32(HOST_TMP2, static_cast<u32>(imm)) &&
			   m_code.EmitAddReg(HOST_TMP0, rs_low, HOST_TMP2, true) &&
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

		// PCSX2 owner: R5900OpcodeImpl.cpp::SLTI(). With RS=$zero, the
		// signed comparison is the constant predicate 0 < sign_extend_16(imm).
		if (rs == 0)
		{
			const u32 result = (imm > 0) ? 1u : 0u;
			return m_code.EmitMovImm8(HOST_TMP0, static_cast<u8>(result)) &&
				   m_code.EmitMovImm8(HOST_TMP1, 0) &&
				   EmitStoreGpr64(rt, HOST_TMP0, HOST_TMP1);
		}

		unsigned rs_low;
		return EmitGpr64OperandLow(rs, HOST_TMP0, HOST_TMP1, &rs_low) &&
			   EmitSetLessThan64Imm(rt, imm, true, rs_low, HOST_TMP1);
	}

	bool BlockCompiler::EmitSLTIU(u32 op)
	{
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		const s32 imm = static_cast<s32>(IMM_S(op));

		if (rt == 0)
			return true;

		// PCSX2 owner: R5900OpcodeImpl.cpp::SLTIU(). The immediate is
		// sign-extended before the unsigned 64-bit compare, so 0 is less than
		// every nonzero immediate.
		if (rs == 0)
		{
			const u32 result = (imm != 0) ? 1u : 0u;
			return m_code.EmitMovImm8(HOST_TMP0, static_cast<u8>(result)) &&
				   m_code.EmitMovImm8(HOST_TMP1, 0) &&
				   EmitStoreGpr64(rt, HOST_TMP0, HOST_TMP1);
		}

		unsigned rs_low;
		return EmitGpr64OperandLow(rs, HOST_TMP0, HOST_TMP1, &rs_low) &&
			   EmitSetLessThan64Imm(rt, imm, false, rs_low, HOST_TMP1);
	}

	bool BlockCompiler::EmitANDI(u32 op)
	{
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		const u16 imm = IMM_U(op);

		if (rt == 0)
			return true;

		// PCSX2 owner: R5900OpcodeImpl.cpp::ANDI(); result is zero-extended into the low 64 bits.
		if (rs == 0 || imm == 0)
		{
			return m_code.EmitMovImm8(HOST_TMP0, 0) &&
				   m_code.EmitMovImm8(HOST_TMP1, 0) &&
				   EmitStoreGpr64(rt, HOST_TMP0, HOST_TMP1);
		}

		unsigned rs_host;
		if (!EmitGprLowOperand(rs, HOST_TMP0, &rs_host))
			return false;

		if (!m_code.EmitAndImm32(HOST_TMP0, rs_host, imm))
		{
			if (!m_code.EmitMovImm32(HOST_TMP2, imm) ||
				!m_code.EmitAndReg(HOST_TMP0, rs_host, HOST_TMP2))
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

		// PCSX2 owner: R5900OpcodeImpl.cpp::ORI(); result is zero-extended into the low 64 bits.
		if (imm == 0 && rt == rs)
			return true;

		if (rs == 0)
		{
			return m_code.EmitMovImm32(HOST_TMP0, imm) &&
				   m_code.EmitMovImm8(HOST_TMP1, 0) &&
				   EmitStoreGpr64(rt, HOST_TMP0, HOST_TMP1);
		}

		unsigned rs_low;
		if (!EmitGpr64OperandLow(rs, HOST_TMP0, HOST_TMP1, &rs_low))
			return false;

		if (!m_code.EmitOrrImm32(HOST_TMP0, rs_low, imm))
		{
			if (!m_code.EmitMovImm32(HOST_TMP2, imm) ||
				!m_code.EmitOrrReg(HOST_TMP0, rs_low, HOST_TMP2))
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

		// PCSX2 owner: R5900OpcodeImpl.cpp::XORI(); result is zero-extended into the low 64 bits.
		if (imm == 0 && rt == rs)
			return true;

		if (rs == 0)
		{
			return m_code.EmitMovImm32(HOST_TMP0, imm) &&
				   m_code.EmitMovImm8(HOST_TMP1, 0) &&
				   EmitStoreGpr64(rt, HOST_TMP0, HOST_TMP1);
		}

		unsigned rs_low;
		if (!EmitGpr64OperandLow(rs, HOST_TMP0, HOST_TMP1, &rs_low))
			return false;

		if (!m_code.EmitEorImm32(HOST_TMP0, rs_low, imm))
		{
			if (!m_code.EmitMovImm32(HOST_TMP2, imm) ||
				!m_code.EmitEorReg(HOST_TMP0, rs_low, HOST_TMP2))
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

		unsigned rs_host;
		unsigned rt_host;
		if (!EmitGprLowOperand(rs, HOST_TMP0, &rs_host) ||
			!EmitGprLowOperand(rt, HOST_TMP1, &rt_host))
		{
			return false;
		}

		if (signed_multiply)
		{
			if (!m_code.EmitSmull(HOST_TMP2, HOST_TMP3, rs_host, rt_host))
				return false;
		}
		else
		{
			if (!m_code.EmitUmull(HOST_TMP2, HOST_TMP3, rs_host, rt_host))
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

		unsigned rs_host;
		unsigned rt_host;
		if (!EmitGprLowOperand(rs, HOST_TMP0, &rs_host) ||
			!EmitGprLowOperand(rt, HOST_TMP1, &rt_host))
		{
			return false;
		}

		if (signed_multiply)
		{
			if (!m_code.EmitSmull(HOST_TMP2, HOST_TMP3, rs_host, rt_host))
				return false;
		}
		else
		{
			if (!m_code.EmitUmull(HOST_TMP2, HOST_TMP3, rs_host, rt_host))
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

		unsigned dividend_low;
		unsigned divisor_low;
		if (!EmitGprLowOperand(RS(op), HOST_TMP0, &dividend_low) ||
			!EmitGprLowOperand(RT(op), HOST_TMP1, &divisor_low))
		{
			return false;
		}

		const auto emit_helper_args = [this, dividend_low, divisor_low]() {
			// The divide helper follows AAPCS (r0=rs low, r1=rt low). Pinned
			// operands can bypass r0/r1 in the fast guards, but the fallback
			// helper still needs the arguments materialized there.
			if (dividend_low != HOST_TMP0 &&
				!m_code.EmitMovRegShiftImm(HOST_TMP0, dividend_low, VitaA32::ShiftType::LSL, 0))
			{
				return false;
			}

			if (divisor_low != HOST_TMP1 &&
				!m_code.EmitMovRegShiftImm(HOST_TMP1, divisor_low, VitaA32::ShiftType::LSL, 0))
			{
				return false;
			}

			return true;
		};

		BranchPatch divzero_branch{};
		BranchPatch zero_branch{};
		BranchPatch divone_branch{};
		BranchPatch negone_or_below_branch{};
		BranchPatch equal_branch{};
		BranchPatch power_of_two_branch{};
		BranchPatch fallback_branch{};
		BranchPatch done_branches[6]{};
		unsigned done_branch_count = 0;

		if (!m_code.EmitCmpImm32(divisor_low, 0) ||
			!emit_branch(divzero_branch, VitaA32::Condition::EQ) ||
			!m_code.EmitCmpImm32(dividend_low, 0) ||
			!emit_branch(zero_branch, VitaA32::Condition::EQ) ||
			!m_code.EmitCmpImm32(divisor_low, 1) ||
			!emit_branch(divone_branch, VitaA32::Condition::EQ))
		{
			return false;
		}

		if (signed_divide)
		{
			if (!m_code.EmitCmpImm32(divisor_low, 0xffffffffu) ||
				!emit_branch(negone_or_below_branch, VitaA32::Condition::EQ) ||
				!m_code.EmitCmpReg(dividend_low, divisor_low) ||
				!emit_branch(equal_branch, VitaA32::Condition::EQ) ||
				!m_code.EmitMovRegShiftImm(HOST_TMP3, divisor_low, VitaA32::ShiftType::ASR, 31) ||
				!m_code.EmitEorReg(HOST_TMP2, divisor_low, HOST_TMP3) ||
				!m_code.EmitSubReg(HOST_TMP2, HOST_TMP2, HOST_TMP3) ||
				!m_code.EmitSubImm8(HOST_TMP3, HOST_TMP2, 1) ||
				!m_code.EmitAndReg(HOST_TMP2, HOST_TMP2, HOST_TMP3, true) ||
				!emit_branch(power_of_two_branch, VitaA32::Condition::EQ))
			{
				return false;
			}
		}
		else if (!m_code.EmitCmpReg(dividend_low, divisor_low) ||
				 !emit_branch(negone_or_below_branch, VitaA32::Condition::CC) ||
				 !emit_branch(equal_branch, VitaA32::Condition::EQ) ||
				 !m_code.EmitSubImm8(HOST_TMP2, divisor_low, 1) ||
				 !m_code.EmitAndReg(HOST_TMP2, divisor_low, HOST_TMP2, true) ||
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
			(!m_code.EmitCmpImm32(dividend_low, 0) ||
				!m_code.EmitMovImm8(HOST_TMP2, 1, VitaA32::Condition::LT)))
		{
			return false;
		}

		if (!store_signed_word_as_doubleword(HOST_TMP2, lo_offset) ||
			!store_signed_word_as_doubleword(dividend_low, hi_offset) ||
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
			!m_code.EmitMovRegShiftImm(HOST_TMP2, dividend_low, VitaA32::ShiftType::LSL, 0) ||
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
				!m_code.EmitRsbImm32(HOST_TMP2, dividend_low, 0) ||
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
				!m_code.EmitMovRegShiftImm(HOST_TMP3, dividend_low, VitaA32::ShiftType::ASR, 31) ||
				!m_code.EmitMovRegShiftImm(HOST_TMP2, divisor_low, VitaA32::ShiftType::ASR, 31) ||
				!m_code.EmitEorReg(HOST_TMP4, divisor_low, HOST_TMP2) ||
				!m_code.EmitSubReg(HOST_TMP4, HOST_TMP4, HOST_TMP2) ||
				!m_code.EmitSubImm8(HOST_TMP2, HOST_TMP4, 1) ||
				!m_code.EmitAndReg(HOST_TMP3, HOST_TMP3, HOST_TMP2) ||
				!m_code.EmitAddReg(HOST_TMP3, dividend_low, HOST_TMP3) ||
				!m_code.EmitClz(HOST_TMP4, HOST_TMP4) ||
				!m_code.EmitRsbImm32(HOST_TMP2, HOST_TMP4, 31) ||
				!m_code.EmitMovRegShiftReg(HOST_TMP4, HOST_TMP3, VitaA32::ShiftType::ASR, HOST_TMP2) ||
				!m_code.EmitMovRegShiftReg(HOST_TMP3, HOST_TMP4, VitaA32::ShiftType::LSL, HOST_TMP2) ||
				!m_code.EmitSubReg(HOST_TMP3, dividend_low, HOST_TMP3) ||
				!m_code.EmitMovRegShiftImm(HOST_TMP2, divisor_low, VitaA32::ShiftType::ASR, 31) ||
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
				!store_signed_word_as_doubleword(dividend_low, hi_offset) ||
				!emit_branch(done_branches[done_branch_count++], VitaA32::Condition::AL) ||
				!patch_branch(equal_branch, m_code.Size()) ||
				!m_code.EmitMovImm8(HOST_TMP2, 1) ||
				!store_signed_word_as_doubleword(HOST_TMP2, lo_offset) ||
				!m_code.EmitMovImm8(HOST_TMP2, 0) ||
				!store_signed_word_as_doubleword(HOST_TMP2, hi_offset) ||
				!emit_branch(done_branches[done_branch_count++], VitaA32::Condition::AL) ||
				!patch_branch(power_of_two_branch, m_code.Size()) ||
				!m_code.EmitSubImm8(HOST_TMP2, divisor_low, 1) ||
				!m_code.EmitAndReg(HOST_TMP3, dividend_low, HOST_TMP2) ||
				!m_code.EmitClz(HOST_TMP2, divisor_low) ||
				!m_code.EmitRsbImm32(HOST_TMP4, HOST_TMP2, 31) ||
				!m_code.EmitMovRegShiftReg(HOST_TMP2, dividend_low, VitaA32::ShiftType::LSR, HOST_TMP4) ||
				!store_signed_word_as_doubleword(HOST_TMP2, lo_offset) ||
				!store_signed_word_as_doubleword(HOST_TMP3, hi_offset) ||
				!emit_branch(done_branches[done_branch_count++], VitaA32::Condition::AL))
			{
				return false;
			}
		}

		if (!patch_branch(fallback_branch, m_code.Size()) ||
			!emit_helper_args() ||
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

		unsigned rs_low;
		if (!EmitGpr64OperandLow(RS(op), HOST_TMP0, HOST_TMP1, &rs_low))
			return false;

		const auto emit_lane = [this, rd](unsigned word, unsigned source_reg, unsigned result_reg) {
			return m_code.EmitMovRegShiftImm(HOST_TMP2, source_reg, VitaA32::ShiftType::ASR, 31) &&
				   m_code.EmitEorReg(result_reg, source_reg, HOST_TMP2) &&
				   m_code.EmitClz(result_reg, result_reg) &&
				   m_code.EmitSubImm8(result_reg, result_reg, 1) &&
				   EmitStoreGprWord(rd, word, result_reg);
		};

		return emit_lane(0, rs_low, HOST_TMP0) &&
			   emit_lane(1, HOST_TMP1, HOST_TMP1);
	}

	bool BlockCompiler::EmitPMFHL(u32 op)
	{
		// PCSX2 owner: MMI.cpp::PMFHL(). Mode 2 saturates the signed 64-bit
		// LO/HI pair into a sign-extended signed 32-bit doubleword.
		const unsigned rd = RD(op);
		if (rd == 0)
			return true;

		const auto emit_word_vector = [this, rd](unsigned source_word) {
			constexpr unsigned NEON_RESULT = 0;
			InvalidateGprQCacheForQreg(NEON_RESULT);
			return m_code.EmitVldrSImm(NEON_RESULT * 4, HOST_CPU_REGS,
					   static_cast<u16>(LO_OFFSET + source_word * sizeof(u32))) &&
				   m_code.EmitVldrSImm(NEON_RESULT * 4 + 1, HOST_CPU_REGS,
					   static_cast<u16>(HI_OFFSET + source_word * sizeof(u32))) &&
				   m_code.EmitVldrSImm(NEON_RESULT * 4 + 2, HOST_CPU_REGS,
					   static_cast<u16>(LO_OFFSET + (source_word + 2) * sizeof(u32))) &&
				   m_code.EmitVldrSImm(NEON_RESULT * 4 + 3, HOST_CPU_REGS,
					   static_cast<u16>(HI_OFFSET + (source_word + 2) * sizeof(u32))) &&
				   EmitStoreGprQ128(rd, NEON_RESULT, HOST_TMP2);
		};
		const auto emit_halfword_vector = [this, rd]() {
			constexpr unsigned NEON_RESULT = 0;
			constexpr unsigned NEON_HI = 1;
			InvalidateGprQCacheForQreg(NEON_RESULT);
			InvalidateGprQCacheForQreg(NEON_HI);
			return EmitLoadCpuRegsQ128(LO_OFFSET, NEON_RESULT, HOST_TMP0) &&
				   EmitLoadCpuRegsQ128(HI_OFFSET, NEON_HI, HOST_TMP1) &&
				   m_code.EmitVuzpI16Q(NEON_RESULT, NEON_HI) &&
				   m_code.EmitVtrnI32D(NEON_RESULT * 2, NEON_RESULT * 2 + 1) &&
				   EmitStoreGprQ128(rd, NEON_RESULT, HOST_TMP2);
		};
		const auto emit_saturating_halfword_vector = [this, rd]() {
			constexpr unsigned NEON_RESULT = 0;
			constexpr unsigned NEON_HI = 1;
			constexpr unsigned NEON_RESULT_LOW_D = NEON_RESULT * 2;
			constexpr unsigned NEON_RESULT_HIGH_D = NEON_RESULT_LOW_D + 1;
			InvalidateGprQCacheForQreg(NEON_RESULT);
			InvalidateGprQCacheForQreg(NEON_HI);
			return EmitLoadCpuRegsQ128(LO_OFFSET, NEON_RESULT, HOST_TMP0) &&
				   EmitLoadCpuRegsQ128(HI_OFFSET, NEON_HI, HOST_TMP1) &&
				   m_code.EmitVqmovnS32D(NEON_RESULT_LOW_D, NEON_RESULT) &&
				   m_code.EmitVqmovnS32D(NEON_RESULT_HIGH_D, NEON_HI) &&
				   m_code.EmitVtrnI32D(NEON_RESULT_LOW_D, NEON_RESULT_HIGH_D) &&
				   EmitStoreGprQ128(rd, NEON_RESULT, HOST_TMP2);
		};
		const auto emit_saturating_word_vector = [this, rd]() {
			constexpr unsigned NEON_RESULT = 0;
			constexpr unsigned NEON_SIGN = 1;
			constexpr unsigned NEON_RESULT_LOW_D = NEON_RESULT * 2;
			constexpr unsigned NEON_RESULT_HIGH_D = NEON_RESULT_LOW_D + 1;
			constexpr unsigned NEON_SIGN_LOW_D = NEON_SIGN * 2;
			InvalidateGprQCacheForQreg(NEON_RESULT);
			InvalidateGprQCacheForQreg(NEON_SIGN);
			return m_code.EmitVldrSImm(NEON_RESULT * 4, HOST_CPU_REGS,
					   static_cast<u16>(LO_OFFSET)) &&
				   m_code.EmitVldrSImm(NEON_RESULT * 4 + 1, HOST_CPU_REGS,
					   static_cast<u16>(HI_OFFSET)) &&
				   m_code.EmitVldrSImm(NEON_RESULT * 4 + 2, HOST_CPU_REGS,
					   static_cast<u16>(LO_OFFSET + 2 * sizeof(u32))) &&
				   m_code.EmitVldrSImm(NEON_RESULT * 4 + 3, HOST_CPU_REGS,
					   static_cast<u16>(HI_OFFSET + 2 * sizeof(u32))) &&
				   m_code.EmitVqmovnS64D(NEON_RESULT_LOW_D, NEON_RESULT) &&
				   m_code.EmitVorrD(NEON_SIGN_LOW_D, NEON_RESULT_LOW_D, NEON_RESULT_LOW_D) &&
				   m_code.EmitVshrS32Q(NEON_SIGN, NEON_SIGN, 31) &&
				   m_code.EmitVtrnI32D(NEON_RESULT_LOW_D, NEON_SIGN_LOW_D) &&
				   m_code.EmitVorrD(NEON_RESULT_HIGH_D, NEON_SIGN_LOW_D, NEON_SIGN_LOW_D) &&
				   EmitStoreGprQ128(rd, NEON_RESULT, HOST_TMP2);
		};

		switch (SA(op))
		{
			case 0x00: // LW
				return emit_word_vector(0);
			case 0x01: // UW
				return emit_word_vector(1);
			case 0x02: // SLW
				return emit_saturating_word_vector();
			case 0x03: // LH
				return emit_halfword_vector();
			case 0x04: // SH
				return emit_saturating_halfword_vector();
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
		const auto emit_store_core_word = [this](unsigned source_reg, size_t dst_offset) {
			return m_code.EmitStrImm12(source_reg, HOST_CPU_REGS, static_cast<u16>(dst_offset));
		};

		if (rs == 0)
		{
			return m_code.EmitMovImm8(HOST_TMP0, 0) &&
				   emit_store_core_word(HOST_TMP0, LO_OFFSET) &&
				   emit_store_core_word(HOST_TMP0, HI_OFFSET) &&
				   emit_store_core_word(HOST_TMP0, LO_OFFSET + 2 * sizeof(u32)) &&
				   emit_store_core_word(HOST_TMP0, HI_OFFSET + 2 * sizeof(u32));
		}

		const int cached_qreg = FindGprQCache(rs);
		if (cached_qreg >= 0)
		{
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuGprQCacheWordHits += 4;
#endif
			const unsigned source_s = static_cast<unsigned>(cached_qreg) * 4;
			return m_code.EmitVstrSImm(source_s, HOST_CPU_REGS, static_cast<u16>(LO_OFFSET)) &&
				   m_code.EmitVstrSImm(source_s + 1, HOST_CPU_REGS, static_cast<u16>(HI_OFFSET)) &&
				   m_code.EmitVstrSImm(source_s + 2, HOST_CPU_REGS,
					   static_cast<u16>(LO_OFFSET + 2 * sizeof(u32))) &&
				   m_code.EmitVstrSImm(source_s + 3, HOST_CPU_REGS,
					   static_cast<u16>(HI_OFFSET + 2 * sizeof(u32)));
		}

		constexpr unsigned NEON_RS = 0;
		const auto emit_store_source_s = [this](unsigned source_word, size_t dst_offset) {
			return m_code.EmitVstrSImm(NEON_RS * 4 + source_word, HOST_CPU_REGS,
				static_cast<u16>(dst_offset));
		};

		return EmitLoadGprQ128(rs, NEON_RS, HOST_TMP0) &&
			   emit_store_source_s(0, LO_OFFSET) &&
			   emit_store_source_s(1, HI_OFFSET) &&
			   emit_store_source_s(2, LO_OFFSET + 2 * sizeof(u32)) &&
			   emit_store_source_s(3, HI_OFFSET + 2 * sizeof(u32));
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

	bool BlockCompiler::EmitMFSA(u32 op)
	{
		// PCSX2 owner: R5900OpcodeImpl.cpp::MFSA().
		const unsigned rd = RD(op);
		if (rd == 0)
			return true;

		return m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(SA_OFFSET)) &&
			   m_code.EmitMovImm8(HOST_TMP1, 0) &&
			   EmitStoreGpr64(rd, HOST_TMP0, HOST_TMP1);
	}

	bool BlockCompiler::EmitMTSA(u32 op)
	{
		// PCSX2 owner: R5900OpcodeImpl.cpp::MTSA().
		unsigned rs_host;
		return EmitGprLowOperand(RS(op), HOST_TMP0, &rs_host) &&
			   m_code.EmitStrImm12(rs_host, HOST_CPU_REGS, static_cast<u16>(SA_OFFSET));
	}

	bool BlockCompiler::EmitMoveFromHiLo(u32 op, size_t hilo_offset)
	{
		const unsigned rd = RD(op);
		if (rd == 0)
			return true;

		return EmitLoadCpuRegsU64(hilo_offset, HOST_TMP0, HOST_TMP1, HOST_TMP2) &&
			   EmitStoreGpr64(rd, HOST_TMP0, HOST_TMP1);
	}

	bool BlockCompiler::EmitMoveToHiLo(u32 op, size_t hilo_offset)
	{
		unsigned rs_low;
		return EmitGpr64OperandLow(RS(op), HOST_TMP0, HOST_TMP1, &rs_low) &&
			   EmitStoreCpuRegsU64(hilo_offset, rs_low, HOST_TMP1, HOST_TMP2);
	}

	bool BlockCompiler::EmitMoveFullFromHiLo(u32 op, size_t hilo_offset)
	{
		const unsigned rd = RD(op);
		if (rd == 0)
			return true;

		constexpr unsigned NEON_VALUE = 0;
		return EmitLoadCpuRegsQ128(hilo_offset, NEON_VALUE, HOST_TMP0) &&
			   EmitStoreGprQ128(rd, NEON_VALUE, HOST_TMP1);
	}

	bool BlockCompiler::EmitMoveFullToHiLo(u32 op, size_t hilo_offset)
	{
		const unsigned rs = RS(op);

		constexpr unsigned NEON_VALUE = 0;
		if (!EmitLoadGprQ128(rs, NEON_VALUE, HOST_TMP0))
			return false;

		return EmitStoreCpuRegsQ128(hilo_offset, NEON_VALUE, HOST_TMP1);
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

		constexpr unsigned NEON_RS = 0;
		constexpr unsigned NEON_RT = 1;
		constexpr unsigned NEON_PRODUCT = 2;
		constexpr unsigned NEON_SIGN = 3;

		if (rs == 0 || rt == 0)
		{
			InvalidateGprQCacheForQreg(NEON_RS);
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuMmiPackedWordMultiplyVectorOps++;
#endif

			// PCSX2 x86/iMMI.cpp::recPMULTW()/recPMULTUW() skip source reads and
			// zero RD/LO/HI when either multiplicand is architectural $zero.
			return m_code.EmitVeorQ(NEON_RS, NEON_RS, NEON_RS) &&
			       EmitStoreCpuRegsQ128(LO_OFFSET, NEON_RS, HOST_TMP0) &&
			       EmitStoreCpuRegsQ128(HI_OFFSET, NEON_RS, HOST_TMP1) &&
			       EmitStoreGprQ128(rd, NEON_RS, HOST_TMP2);
		}

		if (!EmitLoadGprQ128(rs, NEON_RS, HOST_TMP0) ||
			!EmitLoadGprQ128(rt, NEON_RT, HOST_TMP1))
		{
			return false;
		}

		InvalidateGprQCacheForQreg(NEON_RS);
		InvalidateGprQCacheForQreg(NEON_RT);
		InvalidateGprQCacheForQreg(NEON_PRODUCT);
		InvalidateGprQCacheForQreg(NEON_SIGN);
#if defined(VITASX2_QEMU_VALIDATION)
		g_qemuMmiPackedWordMultiplyVectorOps++;
#endif

		// PCSX2 owners: MMI.cpp::_PMULTW()/_PMULTUW() and
		// x86/iMMI.cpp::recPMULTW()/recPMULTUW(). Active words 0/2 are
		// multiplied into raw 64-bit products, then LO/HI receive sign-extended
		// low/high product words. VTRN.32 on each source D pair forms the active
		// [word0, word2] vector without the copy+VUZP sequence.
		if (!m_code.EmitVtrnI32D(NEON_RS * 2, NEON_RS * 2 + 1) ||
			!m_code.EmitVtrnI32D(NEON_RT * 2, NEON_RT * 2 + 1))
		{
			return false;
		}

		const bool multiply_ok = signed_multiply ?
		                             m_code.EmitVmullS32Q(NEON_PRODUCT, NEON_RS * 2, NEON_RT * 2) :
		                             m_code.EmitVmullU32Q(NEON_PRODUCT, NEON_RS * 2, NEON_RT * 2);
		if (!multiply_ok)
			return false;

		if (rd == 0)
		{
			// PCSX2 MMI.cpp::PMULTW()/PMULTUW() still update LO/HI when RD is
			// $zero. Split the product in place instead of preserving it for RD.
			return m_code.EmitVextI8Q(NEON_RT, NEON_PRODUCT, NEON_PRODUCT, 4) &&
			       m_code.EmitVshrS32Q(NEON_SIGN, NEON_PRODUCT, 31) &&
			       m_code.EmitVtrnI32Q(NEON_PRODUCT, NEON_SIGN) &&
			       EmitStoreCpuRegsQ128(LO_OFFSET, NEON_PRODUCT, HOST_TMP0) &&
			       m_code.EmitVshrS32Q(NEON_SIGN, NEON_RT, 31) &&
			       m_code.EmitVtrnI32Q(NEON_RT, NEON_SIGN) &&
			       EmitStoreCpuRegsQ128(HI_OFFSET, NEON_RT, HOST_TMP1);
		}

		if (!m_code.EmitVorrQ(NEON_RS, NEON_PRODUCT, NEON_PRODUCT) ||
			!m_code.EmitVshrS32Q(NEON_SIGN, NEON_RS, 31) ||
			!m_code.EmitVtrnI32Q(NEON_RS, NEON_SIGN) ||
			!EmitStoreCpuRegsQ128(LO_OFFSET, NEON_RS, HOST_TMP0) ||
			!m_code.EmitVextI8Q(NEON_RT, NEON_PRODUCT, NEON_PRODUCT, 4) ||
			!m_code.EmitVshrS32Q(NEON_SIGN, NEON_RT, 31) ||
			!m_code.EmitVtrnI32Q(NEON_RT, NEON_SIGN) ||
			!EmitStoreCpuRegsQ128(HI_OFFSET, NEON_RT, HOST_TMP1))
		{
			return false;
		}

		return EmitStoreGprQ128(rd, NEON_PRODUCT, HOST_TMP2);
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

		const auto load_word = [this](unsigned guest_reg, unsigned word,
								   unsigned host_reg) {
			return EmitLoadGprWord(guest_reg, word, host_reg);
		};

		const auto emit_branch = [this](BranchPatch& patch,
									 VitaA32::Condition condition) {
			patch.offset = m_code.EmitBranchPlaceholder(condition);
			patch.condition = condition;
			return patch.offset != static_cast<size_t>(-1);
		};

		const auto patch_branch = [this](const BranchPatch& patch,
									  size_t target) {
			return m_code.PatchBranch(patch.offset, target, patch.condition);
		};

		const auto patch_branches =
			[patch_branch](const BranchPatch* branches, unsigned count,
				size_t target) {
				for (unsigned i = 0; i < count; i++)
				{
					if (!patch_branch(branches[i], target))
						return false;
				}
				return true;
			};

		const auto store_signed_word_as_doubleword = [this](
														 unsigned value_reg,
														 size_t offset,
														 unsigned sign_reg = HOST_TMP4,
														 unsigned address_scratch = HOST_TMP5) {
			return m_code.EmitMovRegShiftImm(sign_reg, value_reg,
					   VitaA32::ShiftType::ASR, 31) &&
			       EmitStoreCpuRegsU64(offset, value_reg, sign_reg,
					   address_scratch);
		};

		BranchPatch fallback_branches[2]{};
		unsigned fallback_branch_count = 0;
		const auto emit_fallback = [&]() {
			if (fallback_branch_count >= 2)
				return false;
			return emit_branch(fallback_branches[fallback_branch_count++],
				VitaA32::Condition::AL);
		};

		const auto emit_signed_fast_guard = [&](unsigned dividend_reg,
												unsigned divisor_reg) {
			BranchPatch ok_branches[6]{};
			unsigned ok_branch_count = 0;
			if (!m_code.EmitCmpImm32(divisor_reg, 0) ||
				!emit_branch(ok_branches[ok_branch_count++],
					VitaA32::Condition::EQ) ||
				!m_code.EmitCmpImm32(dividend_reg, 0) ||
				!emit_branch(ok_branches[ok_branch_count++],
					VitaA32::Condition::EQ) ||
				!EmitCmpImm32OrReg(divisor_reg, 1, HOST_TMP4) ||
				!emit_branch(ok_branches[ok_branch_count++],
					VitaA32::Condition::EQ) ||
				!EmitCmpImm32OrReg(divisor_reg, 0xffffffffu, HOST_TMP4) ||
				!emit_branch(ok_branches[ok_branch_count++],
					VitaA32::Condition::EQ) ||
				!m_code.EmitCmpReg(dividend_reg, divisor_reg) ||
				!emit_branch(ok_branches[ok_branch_count++],
					VitaA32::Condition::EQ) ||
				!m_code.EmitMovRegShiftImm(HOST_TMP4, divisor_reg,
					VitaA32::ShiftType::ASR, 31) ||
				!m_code.EmitEorReg(HOST_TMP5, divisor_reg, HOST_TMP4) ||
				!m_code.EmitSubReg(HOST_TMP5, HOST_TMP5, HOST_TMP4) ||
				!m_code.EmitSubImm8(HOST_TMP4, HOST_TMP5, 1) ||
				!m_code.EmitAndReg(HOST_TMP4, HOST_TMP5, HOST_TMP4, true) ||
				!emit_branch(ok_branches[ok_branch_count++],
					VitaA32::Condition::EQ) ||
				!emit_fallback())
			{
				return false;
			}

			return patch_branches(ok_branches, ok_branch_count, m_code.Size());
		};

		const auto emit_unsigned_fast_guard = [&](unsigned dividend_reg,
												  unsigned divisor_reg) {
			BranchPatch ok_branches[5]{};
			unsigned ok_branch_count = 0;
			if (!m_code.EmitCmpImm32(divisor_reg, 0) ||
				!emit_branch(ok_branches[ok_branch_count++],
					VitaA32::Condition::EQ) ||
				!m_code.EmitCmpImm32(dividend_reg, 0) ||
				!emit_branch(ok_branches[ok_branch_count++],
					VitaA32::Condition::EQ) ||
				!EmitCmpImm32OrReg(divisor_reg, 1, HOST_TMP4) ||
				!emit_branch(ok_branches[ok_branch_count++],
					VitaA32::Condition::EQ) ||
				!m_code.EmitCmpReg(dividend_reg, divisor_reg) ||
				!emit_branch(ok_branches[ok_branch_count++],
					VitaA32::Condition::LS) ||
				!m_code.EmitSubImm8(HOST_TMP4, divisor_reg, 1) ||
				!m_code.EmitAndReg(HOST_TMP4, divisor_reg, HOST_TMP4, true) ||
				!emit_branch(ok_branches[ok_branch_count++],
					VitaA32::Condition::EQ) ||
				!emit_fallback())
			{
				return false;
			}

			return patch_branches(ok_branches, ok_branch_count, m_code.Size());
		};

		const auto emit_signed_direct_lane = [&](unsigned source_word,
												 unsigned dest_lane,
												 bool operands_preloaded) {
			const size_t lo_offset = LO_OFFSET + dest_lane * sizeof(u64);
			const size_t hi_offset = HI_OFFSET + dest_lane * sizeof(u64);
			BranchPatch divzero_branch{};
			BranchPatch zero_branch{};
			BranchPatch divone_branch{};
			BranchPatch negone_branch{};
			BranchPatch equal_branch{};
			BranchPatch done_branches[6]{};
			unsigned done_branch_count = 0;

			if ((!operands_preloaded &&
					(!load_word(rs, source_word, HOST_TMP0) ||
						!load_word(rt, source_word, HOST_TMP1))) ||
				!m_code.EmitCmpImm32(HOST_TMP1, 0) ||
				!emit_branch(divzero_branch, VitaA32::Condition::EQ) ||
				!m_code.EmitCmpImm32(HOST_TMP0, 0) ||
				!emit_branch(zero_branch, VitaA32::Condition::EQ) ||
				!EmitCmpImm32OrReg(HOST_TMP1, 1, HOST_TMP3) ||
				!emit_branch(divone_branch, VitaA32::Condition::EQ) ||
				!EmitCmpImm32OrReg(HOST_TMP1, 0xffffffffu, HOST_TMP3) ||
				!emit_branch(negone_branch, VitaA32::Condition::EQ) ||
				!m_code.EmitCmpReg(HOST_TMP0, HOST_TMP1) ||
				!emit_branch(equal_branch, VitaA32::Condition::EQ))
			{
				return false;
			}

			if (!m_code.EmitMovRegShiftImm(HOST_TMP3, HOST_TMP0,
					VitaA32::ShiftType::ASR, 31) ||
				!m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP1,
					VitaA32::ShiftType::ASR, 31) ||
				!m_code.EmitEorReg(HOST_TMP4, HOST_TMP1, HOST_TMP2) ||
				!m_code.EmitSubReg(HOST_TMP4, HOST_TMP4, HOST_TMP2) ||
				!m_code.EmitSubImm8(HOST_TMP2, HOST_TMP4, 1) ||
				!m_code.EmitAndReg(HOST_TMP3, HOST_TMP3, HOST_TMP2) ||
				!m_code.EmitAddReg(HOST_TMP3, HOST_TMP0, HOST_TMP3) ||
				!m_code.EmitClz(HOST_TMP4, HOST_TMP4) ||
				!m_code.EmitRsbImm32(HOST_TMP2, HOST_TMP4, 31) ||
				!m_code.EmitMovRegShiftReg(
					HOST_TMP4, HOST_TMP3, VitaA32::ShiftType::ASR, HOST_TMP2) ||
				!m_code.EmitMovRegShiftReg(
					HOST_TMP3, HOST_TMP4, VitaA32::ShiftType::LSL, HOST_TMP2) ||
				!m_code.EmitSubReg(HOST_TMP3, HOST_TMP0, HOST_TMP3) ||
				!m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP1,
					VitaA32::ShiftType::ASR, 31) ||
				!m_code.EmitEorReg(HOST_TMP4, HOST_TMP4, HOST_TMP2) ||
				!m_code.EmitSubReg(HOST_TMP2, HOST_TMP4, HOST_TMP2) ||
				!store_signed_word_as_doubleword(HOST_TMP2, lo_offset) ||
				!store_signed_word_as_doubleword(HOST_TMP3, hi_offset) ||
				!emit_branch(done_branches[done_branch_count++],
					VitaA32::Condition::AL))
			{
				return false;
			}

			if (!patch_branch(negone_branch, m_code.Size()) ||
				!m_code.EmitRsbImm32(HOST_TMP2, HOST_TMP0, 0) ||
				!store_signed_word_as_doubleword(HOST_TMP2, lo_offset, HOST_TMP3) ||
				!m_code.EmitMovImm8(HOST_TMP2, 0) ||
				!store_signed_word_as_doubleword(HOST_TMP2, hi_offset, HOST_TMP3) ||
				!emit_branch(done_branches[done_branch_count++],
					VitaA32::Condition::AL))
			{
				return false;
			}

			if (!patch_branch(equal_branch, m_code.Size()) ||
				!m_code.EmitMovImm8(HOST_TMP2, 1) ||
				!store_signed_word_as_doubleword(HOST_TMP2, lo_offset, HOST_TMP3) ||
				!m_code.EmitMovImm8(HOST_TMP2, 0) ||
				!store_signed_word_as_doubleword(HOST_TMP2, hi_offset, HOST_TMP3) ||
				!emit_branch(done_branches[done_branch_count++],
					VitaA32::Condition::AL))
			{
				return false;
			}

			if (!patch_branch(divone_branch, m_code.Size()) ||
				!store_signed_word_as_doubleword(HOST_TMP0, lo_offset, HOST_TMP1) ||
				!m_code.EmitMovImm8(HOST_TMP2, 0) ||
				!store_signed_word_as_doubleword(HOST_TMP2, hi_offset, HOST_TMP3) ||
				!emit_branch(done_branches[done_branch_count++],
					VitaA32::Condition::AL))
			{
				return false;
			}

			if (!patch_branch(zero_branch, m_code.Size()) ||
				!m_code.EmitMovImm8(HOST_TMP2, 0) ||
				!store_signed_word_as_doubleword(HOST_TMP2, lo_offset, HOST_TMP3) ||
				!store_signed_word_as_doubleword(HOST_TMP2, hi_offset, HOST_TMP3) ||
				!emit_branch(done_branches[done_branch_count++],
					VitaA32::Condition::AL))
			{
				return false;
			}

			if (!patch_branch(divzero_branch, m_code.Size()) ||
				!m_code.EmitMovImm32(HOST_TMP2, 0xffffffffu) ||
				!m_code.EmitCmpImm32(HOST_TMP0, 0) ||
				!m_code.EmitMovImm8(HOST_TMP2, 1, VitaA32::Condition::LT) ||
				!store_signed_word_as_doubleword(HOST_TMP2, lo_offset, HOST_TMP3) ||
				!store_signed_word_as_doubleword(HOST_TMP0, hi_offset, HOST_TMP1))
			{
				return false;
			}

			return patch_branches(done_branches, done_branch_count,
				m_code.Size());
		};

		const auto emit_unsigned_direct_lane = [&](unsigned source_word,
												   unsigned dest_lane,
												   bool operands_preloaded) {
			const size_t lo_offset = LO_OFFSET + dest_lane * sizeof(u64);
			const size_t hi_offset = HI_OFFSET + dest_lane * sizeof(u64);
			BranchPatch divzero_branch{};
			BranchPatch zero_branch{};
			BranchPatch divone_branch{};
			BranchPatch below_branch{};
			BranchPatch equal_branch{};
			BranchPatch done_branches[6]{};
			unsigned done_branch_count = 0;

			if ((!operands_preloaded &&
					(!load_word(rs, source_word, HOST_TMP0) ||
						!load_word(rt, source_word, HOST_TMP1))) ||
				!m_code.EmitCmpImm32(HOST_TMP1, 0) ||
				!emit_branch(divzero_branch, VitaA32::Condition::EQ) ||
				!m_code.EmitCmpImm32(HOST_TMP0, 0) ||
				!emit_branch(zero_branch, VitaA32::Condition::EQ) ||
				!EmitCmpImm32OrReg(HOST_TMP1, 1, HOST_TMP3) ||
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
				!m_code.EmitRsbImm32(HOST_TMP4, HOST_TMP2, 31) ||
				!m_code.EmitMovRegShiftReg(
					HOST_TMP2, HOST_TMP0, VitaA32::ShiftType::LSR, HOST_TMP4) ||
				!store_signed_word_as_doubleword(HOST_TMP2, lo_offset) ||
				!store_signed_word_as_doubleword(HOST_TMP3, hi_offset) ||
				!emit_branch(done_branches[done_branch_count++],
					VitaA32::Condition::AL))
			{
				return false;
			}

			if (!patch_branch(equal_branch, m_code.Size()) ||
				!m_code.EmitMovImm8(HOST_TMP2, 1) ||
				!store_signed_word_as_doubleword(HOST_TMP2, lo_offset, HOST_TMP3) ||
				!m_code.EmitMovImm8(HOST_TMP2, 0) ||
				!store_signed_word_as_doubleword(HOST_TMP2, hi_offset, HOST_TMP3) ||
				!emit_branch(done_branches[done_branch_count++],
					VitaA32::Condition::AL))
			{
				return false;
			}

			if (!patch_branch(below_branch, m_code.Size()) ||
				!m_code.EmitMovImm8(HOST_TMP2, 0) ||
				!store_signed_word_as_doubleword(HOST_TMP2, lo_offset, HOST_TMP3) ||
				!store_signed_word_as_doubleword(HOST_TMP0, hi_offset, HOST_TMP1) ||
				!emit_branch(done_branches[done_branch_count++],
					VitaA32::Condition::AL))
			{
				return false;
			}

			if (!patch_branch(divone_branch, m_code.Size()) ||
				!store_signed_word_as_doubleword(HOST_TMP0, lo_offset, HOST_TMP1) ||
				!m_code.EmitMovImm8(HOST_TMP2, 0) ||
				!store_signed_word_as_doubleword(HOST_TMP2, hi_offset, HOST_TMP3) ||
				!emit_branch(done_branches[done_branch_count++],
					VitaA32::Condition::AL))
			{
				return false;
			}

			if (!patch_branch(zero_branch, m_code.Size()) ||
				!m_code.EmitMovImm8(HOST_TMP2, 0) ||
				!store_signed_word_as_doubleword(HOST_TMP2, lo_offset, HOST_TMP3) ||
				!store_signed_word_as_doubleword(HOST_TMP2, hi_offset, HOST_TMP3) ||
				!emit_branch(done_branches[done_branch_count++],
					VitaA32::Condition::AL))
			{
				return false;
			}

			if (!patch_branch(divzero_branch, m_code.Size()) ||
				!m_code.EmitMovImm32(HOST_TMP2, 0xffffffffu) ||
				!store_signed_word_as_doubleword(HOST_TMP2, lo_offset, HOST_TMP3) ||
				!store_signed_word_as_doubleword(HOST_TMP0, hi_offset, HOST_TMP1))
			{
				return false;
			}

			return patch_branches(done_branches, done_branch_count,
				m_code.Size());
		};

		const auto emit_store_active_word_lanes_as_doublewords =
			[&](size_t offset, unsigned value_qreg, unsigned sign_qreg,
				unsigned address_scratch) {
				return m_code.EmitVshrS32Q(sign_qreg, value_qreg, 31) &&
			           m_code.EmitVtrnI32Q(value_qreg, sign_qreg) &&
			           EmitStoreCpuRegsQ128(offset, value_qreg,
						   address_scratch);
			};

		const auto emit_signed_power_of_two_test =
			[&](unsigned divisor_reg, BranchPatch* scalar_branches,
				unsigned& scalar_branch_count) {
				if (scalar_branch_count + 2 > 4)
					return false;

				if (!m_code.EmitCmpImm32(divisor_reg, 0) ||
					!emit_branch(scalar_branches[scalar_branch_count++],
						VitaA32::Condition::EQ) ||
					!m_code.EmitMovRegShiftImm(HOST_TMP4, divisor_reg,
						VitaA32::ShiftType::ASR, 31) ||
					!m_code.EmitEorReg(HOST_TMP5, divisor_reg, HOST_TMP4) ||
					!m_code.EmitSubReg(HOST_TMP5, HOST_TMP5, HOST_TMP4) ||
					!m_code.EmitSubImm8(HOST_TMP4, HOST_TMP5, 1) ||
					!m_code.EmitAndReg(HOST_TMP4, HOST_TMP5, HOST_TMP4, true) ||
					!emit_branch(scalar_branches[scalar_branch_count++],
						VitaA32::Condition::NE))
				{
					return false;
				}

				return true;
			};

		const auto emit_unsigned_power_of_two_test =
			[&](unsigned divisor_reg, BranchPatch* scalar_branches,
				unsigned& scalar_branch_count) {
				if (scalar_branch_count + 2 > 4)
					return false;

				if (!m_code.EmitCmpImm32(divisor_reg, 0) ||
					!emit_branch(scalar_branches[scalar_branch_count++],
						VitaA32::Condition::EQ) ||
					!m_code.EmitSubImm8(HOST_TMP4, divisor_reg, 1) ||
					!m_code.EmitAndReg(HOST_TMP4, divisor_reg, HOST_TMP4,
						true) ||
					!emit_branch(scalar_branches[scalar_branch_count++],
						VitaA32::Condition::NE))
				{
					return false;
				}

				return true;
			};

		const auto emit_signed_power_of_two_vector_from_loaded = [&]() {
			constexpr unsigned NEON_RS = 0;
			constexpr unsigned NEON_SIGN = 1;
			constexpr unsigned NEON_BIAS_MASK = 2;
			constexpr unsigned NEON_SHIFT_RIGHT = 3;
			constexpr unsigned NEON_QUOTIENT = 4;
			constexpr unsigned NEON_PRODUCT = 5;
			constexpr unsigned NEON_DIVISOR_SIGN = 6;
			constexpr unsigned NEON_SHIFT_LEFT = 7;

			const auto set_lane_config = [&](unsigned divisor_reg,
											 unsigned lane) {
				const unsigned divisor_sign_s = NEON_DIVISOR_SIGN * 4 + lane;
				const unsigned bias_mask_s = NEON_BIAS_MASK * 4 + lane;
				const unsigned shift_right_s = NEON_SHIFT_RIGHT * 4 + lane;
				const unsigned shift_left_s = NEON_SHIFT_LEFT * 4 + lane;

				return m_code.EmitMovRegShiftImm(HOST_TMP4, divisor_reg,
						   VitaA32::ShiftType::ASR, 31) &&
				       m_code.EmitVmovCoreToS(divisor_sign_s, HOST_TMP4) &&
				       m_code.EmitEorReg(HOST_TMP5, divisor_reg, HOST_TMP4) &&
				       m_code.EmitSubReg(HOST_TMP5, HOST_TMP5, HOST_TMP4) &&
				       m_code.EmitSubImm8(HOST_TMP4, HOST_TMP5, 1) &&
				       m_code.EmitVmovCoreToS(bias_mask_s, HOST_TMP4) &&
				       m_code.EmitClz(HOST_TMP4, HOST_TMP5) &&
				       m_code.EmitRsbImm32(HOST_TMP4, HOST_TMP4, 31) &&
				       m_code.EmitVmovCoreToS(shift_left_s, HOST_TMP4) &&
				       m_code.EmitRsbImm32(HOST_TMP4, HOST_TMP4, 0) &&
				       m_code.EmitVmovCoreToS(shift_right_s, HOST_TMP4);
			};

			if (!EmitLoadGprQ128(rs, NEON_RS, HOST_TMP0))
				return false;

			InvalidateGprQCacheForQreg(NEON_RS);
			InvalidateGprQCacheForQreg(NEON_SIGN);
			InvalidateGprQCacheForQreg(NEON_BIAS_MASK);
			InvalidateGprQCacheForQreg(NEON_SHIFT_RIGHT);
			InvalidateGprQCacheForQreg(NEON_QUOTIENT);
			InvalidateGprQCacheForQreg(NEON_PRODUCT);
			InvalidateGprQCacheForQreg(NEON_DIVISOR_SIGN);
			InvalidateGprQCacheForQreg(NEON_SHIFT_LEFT);
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuMmiPackedWordDivideVectorOps++;
#endif

			// PCSX2 owner: MMI.cpp::_PDIVW(). For signed power-of-two
			// divisors, bias negative dividends before the arithmetic shift
			// so the quotient still truncates toward zero.
			return m_code.EmitVeorQ(NEON_BIAS_MASK, NEON_BIAS_MASK,
					   NEON_BIAS_MASK) &&
			       m_code.EmitVeorQ(NEON_SHIFT_RIGHT, NEON_SHIFT_RIGHT,
					   NEON_SHIFT_RIGHT) &&
			       m_code.EmitVeorQ(NEON_DIVISOR_SIGN, NEON_DIVISOR_SIGN,
					   NEON_DIVISOR_SIGN) &&
			       m_code.EmitVeorQ(NEON_SHIFT_LEFT, NEON_SHIFT_LEFT,
					   NEON_SHIFT_LEFT) &&
			       set_lane_config(HOST_TMP1, 0) &&
			       set_lane_config(HOST_TMP3, 2) &&
			       m_code.EmitVshrS32Q(NEON_SIGN, NEON_RS, 31) &&
			       m_code.EmitVandQ(NEON_SIGN, NEON_SIGN, NEON_BIAS_MASK) &&
			       m_code.EmitVaddI32Q(NEON_QUOTIENT, NEON_RS, NEON_SIGN) &&
			       m_code.EmitVshlS32Q(NEON_QUOTIENT, NEON_QUOTIENT,
					   NEON_SHIFT_RIGHT) &&
			       m_code.EmitVeorQ(NEON_QUOTIENT, NEON_QUOTIENT,
					   NEON_DIVISOR_SIGN) &&
			       m_code.EmitVsubI32Q(NEON_QUOTIENT, NEON_QUOTIENT,
					   NEON_DIVISOR_SIGN) &&
			       m_code.EmitVshlS32Q(NEON_PRODUCT, NEON_QUOTIENT,
					   NEON_SHIFT_LEFT) &&
			       m_code.EmitVeorQ(NEON_PRODUCT, NEON_PRODUCT,
					   NEON_DIVISOR_SIGN) &&
			       m_code.EmitVsubI32Q(NEON_PRODUCT, NEON_PRODUCT,
					   NEON_DIVISOR_SIGN) &&
			       m_code.EmitVsubI32Q(NEON_PRODUCT, NEON_RS, NEON_PRODUCT) &&
			       emit_store_active_word_lanes_as_doublewords(
					   LO_OFFSET, NEON_QUOTIENT, NEON_SIGN, HOST_TMP0) &&
			       emit_store_active_word_lanes_as_doublewords(
					   HI_OFFSET, NEON_PRODUCT, NEON_SIGN, HOST_TMP1);
		};

		const auto emit_unsigned_power_of_two_vector_from_loaded = [&]() {
			constexpr unsigned NEON_RS = 0;
			constexpr unsigned NEON_MASK = 1;
			constexpr unsigned NEON_SHIFT_RIGHT = 2;
			constexpr unsigned NEON_QUOTIENT = 3;
			constexpr unsigned NEON_REMAINDER = 4;
			constexpr unsigned NEON_SIGN = 5;

			const auto set_lane_config = [&](unsigned divisor_reg,
											 unsigned lane) {
				const unsigned mask_s = NEON_MASK * 4 + lane;
				const unsigned shift_right_s = NEON_SHIFT_RIGHT * 4 + lane;

				return m_code.EmitSubImm8(HOST_TMP4, divisor_reg, 1) &&
				       m_code.EmitVmovCoreToS(mask_s, HOST_TMP4) &&
				       m_code.EmitClz(HOST_TMP4, divisor_reg) &&
				       m_code.EmitRsbImm32(HOST_TMP4, HOST_TMP4, 31) &&
				       m_code.EmitRsbImm32(HOST_TMP4, HOST_TMP4, 0) &&
				       m_code.EmitVmovCoreToS(shift_right_s, HOST_TMP4);
			};

			if (!EmitLoadGprQ128(rs, NEON_RS, HOST_TMP0))
				return false;

			InvalidateGprQCacheForQreg(NEON_RS);
			InvalidateGprQCacheForQreg(NEON_MASK);
			InvalidateGprQCacheForQreg(NEON_SHIFT_RIGHT);
			InvalidateGprQCacheForQreg(NEON_QUOTIENT);
			InvalidateGprQCacheForQreg(NEON_REMAINDER);
			InvalidateGprQCacheForQreg(NEON_SIGN);
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuMmiPackedWordDivideVectorOps++;
#endif

			// PCSX2 owner: MMI.cpp::_PDIVUW(). Power-of-two unsigned
			// divisors are quotient=dividend>>log2(divisor),
			// remainder=dividend&(divisor-1).
			return m_code.EmitVeorQ(NEON_MASK, NEON_MASK, NEON_MASK) &&
			       m_code.EmitVeorQ(NEON_SHIFT_RIGHT, NEON_SHIFT_RIGHT,
					   NEON_SHIFT_RIGHT) &&
			       set_lane_config(HOST_TMP1, 0) &&
			       set_lane_config(HOST_TMP3, 2) &&
			       m_code.EmitVshlU32Q(NEON_QUOTIENT, NEON_RS,
					   NEON_SHIFT_RIGHT) &&
			       m_code.EmitVandQ(NEON_REMAINDER, NEON_RS, NEON_MASK) &&
			       emit_store_active_word_lanes_as_doublewords(
					   LO_OFFSET, NEON_QUOTIENT, NEON_SIGN, HOST_TMP0) &&
			       emit_store_active_word_lanes_as_doublewords(
					   HI_OFFSET, NEON_REMAINDER, NEON_SIGN, HOST_TMP1);
		};

		const auto emit_power_of_two_vector_path =
			[&](bool signed_path, BranchPatch& done_branch,
				bool& has_done_branch) {
				BranchPatch scalar_branches[4]{};
				unsigned scalar_branch_count = 0;
				const bool tests_ok =
					signed_path ? (emit_signed_power_of_two_test(HOST_TMP1,
									   scalar_branches,
									   scalar_branch_count) &&
									  emit_signed_power_of_two_test(
										  HOST_TMP3, scalar_branches, scalar_branch_count)) :
								  (emit_unsigned_power_of_two_test(
									   HOST_TMP1, scalar_branches,
									   scalar_branch_count) &&
									  emit_unsigned_power_of_two_test(
										  HOST_TMP3, scalar_branches,
										  scalar_branch_count));
				if (!tests_ok)
					return false;

				const bool vector_ok =
					signed_path ? emit_signed_power_of_two_vector_from_loaded() : emit_unsigned_power_of_two_vector_from_loaded();
				if (!vector_ok ||
					!emit_branch(done_branch, VitaA32::Condition::AL))
				{
					return false;
				}

				has_done_branch = true;
				return patch_branches(scalar_branches, scalar_branch_count,
					m_code.Size());
			};

		if (!load_word(rs, 0, HOST_TMP0) || !load_word(rt, 0, HOST_TMP1) ||
			!load_word(rs, 2, HOST_TMP2) || !load_word(rt, 2, HOST_TMP3))
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

		BranchPatch vector_done_branch{};
		bool has_vector_done_branch = false;
		if (signed_divide)
		{
			// The guard path loaded lane 0 into HOST_TMP0/HOST_TMP1 and lane
			// 2 into HOST_TMP2/HOST_TMP3. If both divisors are signed
			// powers of two, emit one NEON path for both active words before
			// falling back to the scalar per-lane direct forms.
			if (!emit_power_of_two_vector_path(true, vector_done_branch,
					has_vector_done_branch) ||
				!emit_signed_direct_lane(0, 0, true) ||
				!emit_signed_direct_lane(2, 1, false))
			{
				return false;
			}
		}
		else if (!emit_power_of_two_vector_path(false, vector_done_branch,
					 has_vector_done_branch) ||
				 !emit_unsigned_direct_lane(0, 0, true) ||
				 !emit_unsigned_direct_lane(2, 1, false))
		{
			return false;
		}

		BranchPatch done_branch{};
		if (!emit_branch(done_branch, VitaA32::Condition::AL))
			return false;

		if (!patch_branches(fallback_branches, fallback_branch_count,
				m_code.Size()) ||
			!m_code.EmitCallAbsolute(
				signed_divide ? reinterpret_cast<const void*>(
									&VitaEePackedDivSignedWords) :
								reinterpret_cast<const void*>(
									&VitaEePackedDivUnsignedWords)))
		{
			return false;
		}

		  const size_t done_target = m_code.Size();
          if (has_vector_done_branch &&
              !patch_branch(vector_done_branch, done_target)) {
            return false;
          }

          return patch_branch(done_branch, done_target);
	}

		bool BlockCompiler::EmitPackedWordByHalfwordDivide(u32 op) {
          const unsigned rs = RS(op);
          const unsigned rt = RT(op);
          struct BranchPatch {
            size_t offset = static_cast<size_t>(-1);
            VitaA32::Condition condition = VitaA32::Condition::AL;
          };

          const auto emit_branch = [this](BranchPatch &patch,
                                          VitaA32::Condition condition) {
            patch.offset = m_code.EmitBranchPlaceholder(condition);
            patch.condition = condition;
            return patch.offset != static_cast<size_t>(-1);
          };

          const auto patch_branch = [this](const BranchPatch &patch,
                                           size_t target) {
            return m_code.PatchBranch(patch.offset, target, patch.condition);
          };

          const auto patch_branches =
              [patch_branch](const BranchPatch *branches, unsigned count,
                             size_t target) {
                for (unsigned i = 0; i < count; i++) {
                  if (!patch_branch(branches[i], target))
                    return false;
                }
                return true;
              };

          const auto load_divisor = [&]() {
            if (rt == 0)
              return m_code.EmitMovImm8(HOST_TMP1, 0);

            unsigned rt_low;
            return EmitGprWordOperand(rt, 0, HOST_TMP1, &rt_low) &&
                   m_code.EmitUxth(HOST_TMP1, rt_low);
          };

          const auto emit_divzero_vector = [&]() {
            constexpr unsigned NEON_RS = 0;
            constexpr unsigned NEON_LO = 1;

            if (!EmitLoadGprQ128(rs, NEON_RS, HOST_TMP0))
              return false;

            InvalidateGprQCacheForQreg(NEON_RS);
            InvalidateGprQCacheForQreg(NEON_LO);
#if defined(VITASX2_QEMU_VALIDATION)
            g_qemuMmiPackedWordByHalfwordDivideVectorOps++;
#endif

            // PCSX2 owner: MMI.cpp::_PDIVBW(). Dividing by zero keeps the
            // dividend in HI and writes LO = (dividend < 0) ? 1 : -1.
            return EmitStoreCpuRegsQ128(HI_OFFSET, NEON_RS, HOST_TMP1) &&
                   m_code.EmitVshrS32Q(NEON_LO, NEON_RS, 31) &&
                   m_code.EmitVshlI32Q(NEON_LO, NEON_LO, 1) &&
                   m_code.EmitVmvnQ(NEON_LO, NEON_LO) &&
                   EmitStoreCpuRegsQ128(LO_OFFSET, NEON_LO, HOST_TMP2);
          };

          const auto emit_unit_divisor_vector = [&](bool negate) {
            constexpr unsigned NEON_RS = 0;
            constexpr unsigned NEON_ZERO = 1;

            if (!EmitLoadGprQ128(rs, NEON_RS, HOST_TMP0))
              return false;

            InvalidateGprQCacheForQreg(NEON_RS);
            InvalidateGprQCacheForQreg(NEON_ZERO);
#if defined(VITASX2_QEMU_VALIDATION)
            g_qemuMmiPackedWordByHalfwordDivideVectorOps++;
#endif

            // PCSX2 owner: MMI.cpp::_PDIVBW(). Divisors +/-1 write zero HI;
            // -1 wraps INT_MIN to itself in the 32-bit quotient lane.
            return m_code.EmitVeorQ(NEON_ZERO, NEON_ZERO, NEON_ZERO) &&
                   EmitStoreCpuRegsQ128(HI_OFFSET, NEON_ZERO, HOST_TMP1) &&
                   (!negate || m_code.EmitVnegS32Q(NEON_RS, NEON_RS)) &&
                   EmitStoreCpuRegsQ128(LO_OFFSET, NEON_RS, HOST_TMP2);
          };

          // PCSX2 owner: MMI.cpp::_PDIVBW() does signed word / signed halfword,
          // with C truncation toward zero and HI = dividend - quotient *
          // divisor.
          const auto emit_power_of_two_vector = [&]() {
            constexpr unsigned NEON_RS = 0;
            constexpr unsigned NEON_SIGN = 1;
            constexpr unsigned NEON_BIAS_MASK = 2;
            constexpr unsigned NEON_SHIFT_RIGHT = 3;
            constexpr unsigned NEON_QUOTIENT = 4;
            constexpr unsigned NEON_PRODUCT = 5;
            constexpr unsigned NEON_DIVISOR_SIGN = 6;
            constexpr unsigned NEON_SHIFT_LEFT = 7;

            const auto broadcast_i32 = [this](unsigned qreg,
                                              unsigned host_reg) {
              return m_code.EmitVdupI32QFromCore(qreg, host_reg);
            };

            if (!m_code.EmitClz(HOST_TMP5, HOST_TMP3) ||
                !m_code.EmitRsbImm32(HOST_TMP5, HOST_TMP5, 31) ||
                !m_code.EmitSubImm8(HOST_TMP3, HOST_TMP3, 1) ||
                !m_code.EmitMovRegShiftImm(HOST_TMP4, HOST_TMP1,
                                           VitaA32::ShiftType::ASR, 31) ||
                !m_code.EmitRsbImm32(HOST_TMP2, HOST_TMP5, 0) ||
                !EmitLoadGprQ128(rs, NEON_RS, HOST_TMP0)) {
              return false;
            }

            InvalidateGprQCacheForQreg(NEON_RS);
            InvalidateGprQCacheForQreg(NEON_SIGN);
            InvalidateGprQCacheForQreg(NEON_BIAS_MASK);
            InvalidateGprQCacheForQreg(NEON_SHIFT_RIGHT);
            InvalidateGprQCacheForQreg(NEON_QUOTIENT);
            InvalidateGprQCacheForQreg(NEON_PRODUCT);
            InvalidateGprQCacheForQreg(NEON_DIVISOR_SIGN);
            InvalidateGprQCacheForQreg(NEON_SHIFT_LEFT);
#if defined(VITASX2_QEMU_VALIDATION)
            g_qemuMmiPackedWordByHalfwordDivideVectorOps++;
#endif

            return broadcast_i32(NEON_BIAS_MASK, HOST_TMP3) &&
                   broadcast_i32(NEON_SHIFT_RIGHT, HOST_TMP2) &&
                   broadcast_i32(NEON_DIVISOR_SIGN, HOST_TMP4) &&
                   broadcast_i32(NEON_SHIFT_LEFT, HOST_TMP5) &&
                   m_code.EmitVshrS32Q(NEON_SIGN, NEON_RS, 31) &&
                   m_code.EmitVandQ(NEON_SIGN, NEON_SIGN, NEON_BIAS_MASK) &&
                   m_code.EmitVaddI32Q(NEON_QUOTIENT, NEON_RS, NEON_SIGN) &&
                   m_code.EmitVshlS32Q(NEON_QUOTIENT, NEON_QUOTIENT,
                                       NEON_SHIFT_RIGHT) &&
                   m_code.EmitVeorQ(NEON_QUOTIENT, NEON_QUOTIENT,
                                    NEON_DIVISOR_SIGN) &&
                   m_code.EmitVsubI32Q(NEON_QUOTIENT, NEON_QUOTIENT,
                                       NEON_DIVISOR_SIGN) &&
                   EmitStoreCpuRegsQ128(LO_OFFSET, NEON_QUOTIENT, HOST_TMP0) &&
                   m_code.EmitVshlS32Q(NEON_PRODUCT, NEON_QUOTIENT,
                                       NEON_SHIFT_LEFT) &&
                   m_code.EmitVeorQ(NEON_PRODUCT, NEON_PRODUCT,
                                    NEON_DIVISOR_SIGN) &&
                   m_code.EmitVsubI32Q(NEON_PRODUCT, NEON_PRODUCT,
                                       NEON_DIVISOR_SIGN) &&
                   m_code.EmitVsubI32Q(NEON_PRODUCT, NEON_RS, NEON_PRODUCT) &&
                   EmitStoreCpuRegsQ128(HI_OFFSET, NEON_PRODUCT, HOST_TMP1);
          };

          BranchPatch divzero_branch{};
          BranchPatch divone_branch{};
          BranchPatch negone_branch{};
          BranchPatch power_of_two_branch{};
          BranchPatch fallback_branch{};
          BranchPatch done_branches[4]{};
          unsigned done_branch_count = 0;

          if (!load_divisor() || !m_code.EmitCmpImm32(HOST_TMP1, 0) ||
              !emit_branch(divzero_branch, VitaA32::Condition::EQ) ||
              !EmitCmpImm32OrReg(HOST_TMP1, 1, HOST_TMP4) ||
              !emit_branch(divone_branch, VitaA32::Condition::EQ) ||
              !EmitCmpImm32OrReg(HOST_TMP1, 0xffffu, HOST_TMP4) ||
              !emit_branch(negone_branch, VitaA32::Condition::EQ) ||
              !m_code.EmitSxth(HOST_TMP1, HOST_TMP1) ||
              !m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP1,
                                         VitaA32::ShiftType::ASR, 31) ||
              !m_code.EmitEorReg(HOST_TMP3, HOST_TMP1, HOST_TMP2) ||
              !m_code.EmitSubReg(HOST_TMP3, HOST_TMP3, HOST_TMP2) ||
              !m_code.EmitSubImm8(HOST_TMP4, HOST_TMP3, 1) ||
              !m_code.EmitAndReg(HOST_TMP4, HOST_TMP3, HOST_TMP4, true) ||
              !emit_branch(power_of_two_branch, VitaA32::Condition::EQ) ||
              !emit_branch(fallback_branch, VitaA32::Condition::AL)) {
            return false;
          }

          if (!patch_branch(divzero_branch, m_code.Size()) ||
              !emit_divzero_vector() ||
              !emit_branch(done_branches[done_branch_count++],
                           VitaA32::Condition::AL)) {
            return false;
          }

          if (!patch_branch(divone_branch, m_code.Size()) ||
              !emit_unit_divisor_vector(false) ||
              !emit_branch(done_branches[done_branch_count++],
                           VitaA32::Condition::AL)) {
            return false;
          }

          if (!patch_branch(negone_branch, m_code.Size()) ||
              !emit_unit_divisor_vector(true) ||
              !emit_branch(done_branches[done_branch_count++],
                           VitaA32::Condition::AL)) {
            return false;
          }

          if (!patch_branch(power_of_two_branch, m_code.Size()) ||
              !emit_power_of_two_vector() ||
              !emit_branch(done_branches[done_branch_count++],
                           VitaA32::Condition::AL)) {
            return false;
          }

          if (!patch_branch(fallback_branch, m_code.Size()) ||
              !m_code.EmitMovImm32(HOST_TMP0, op) ||
              !m_code.EmitCallAbsolute(reinterpret_cast<const void *>(
                  &VitaEePackedDivSignedWordsByHalfword))) {
            return false;
          }

          return patch_branches(done_branches, done_branch_count,
                                m_code.Size());
        }

		bool BlockCompiler::EmitPackedSignedWordMultiplyAccumulate(u32 op, bool subtract)
		{
			const unsigned rd = RD(op);
			const unsigned rs = RS(op);
			const unsigned rt = RT(op);

			const auto load_word = [this](unsigned guest_reg, unsigned word, unsigned host_reg) {
				return EmitLoadGprWord(guest_reg, word, host_reg);
			};

			const auto store_signed_word_as_doubleword = [this](unsigned value_reg, size_t offset) {
				return m_code.EmitMovRegShiftImm(HOST_TMP0, value_reg, VitaA32::ShiftType::ASR, 31) &&
			           m_code.EmitStrImm12(value_reg, HOST_CPU_REGS, static_cast<u16>(offset)) &&
			           m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(offset + sizeof(u32)));
			};

			const auto emit_zero_product_accumulator_vector = [&]() {
				constexpr unsigned NEON_LO = 0;
				constexpr unsigned NEON_HI = 1;
				constexpr unsigned NEON_LO_SIGN = 2;
				constexpr unsigned NEON_HI_SIGN = 3;

				InvalidateGprQCacheForQreg(NEON_LO);
				InvalidateGprQCacheForQreg(NEON_HI);
				InvalidateGprQCacheForQreg(NEON_LO_SIGN);
				InvalidateGprQCacheForQreg(NEON_HI_SIGN);

				if (!EmitLoadCpuRegsQ128(LO_OFFSET, NEON_LO, HOST_TMP0) ||
					!EmitLoadCpuRegsQ128(HI_OFFSET, NEON_HI, HOST_TMP1) ||
					!m_code.EmitVshrS32Q(NEON_LO_SIGN, NEON_LO, 31) ||
					!m_code.EmitVshrS32Q(NEON_HI_SIGN, NEON_HI, 31) ||
					!m_code.EmitVtrnI32Q(NEON_LO, NEON_LO_SIGN) ||
					!m_code.EmitVtrnI32Q(NEON_HI, NEON_HI_SIGN) ||
					!EmitStoreCpuRegsQ128(LO_OFFSET, NEON_LO, HOST_TMP0) ||
					!EmitStoreCpuRegsQ128(HI_OFFSET, NEON_HI, HOST_TMP1))
				{
					return false;
				}

				if (rd == 0)
					return true;

				return m_code.EmitVtrnI32Q(NEON_LO, NEON_HI) &&
				       EmitStoreGprQ128(rd, NEON_LO, HOST_TMP2);
			};

			if ((rs == 0 || rt == 0) && (subtract || (rs == 0 && rt == 0)))
			{
				// PCSX2 owners: MMI.cpp::_PMSUBW() and _PMADDW(). A zero product
				// still sign-extends the active LO/HI words into their 64-bit lanes.
				// PMADDW's lower-lane quirk is only compile-time impossible when both
				// sources are architectural $zero; PMSUBW has no quirk path.
				return emit_zero_product_accumulator_vector();
			}

			const auto emit_divide_signed_64_by_u32_max = [this]() {
				if (!m_code.EmitCmpImm32(HOST_TMP3, 0))
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

				if (!m_code.EmitRsbImm32(HOST_TMP0, HOST_TMP3, 0) ||
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

			const auto emit_pmaddw_lower_quirk = [this](unsigned rs_value_reg) {
				if (!m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP1, VitaA32::ShiftType::LSL, 1) ||
					!m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP2, VitaA32::ShiftType::LSR, 1) ||
					!m_code.EmitCmpImm32(HOST_TMP2, 0))
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
					!m_code.EmitCmpReg(rs_value_reg, HOST_TMP1))
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

			const bool store_rd_from_live_lanes = rd != 0 && rd != rs && rd != rt;
			const auto store_rd_lane_pair = [this, rd, store_rd_from_live_lanes](
												unsigned dest_lane) {
				if (!store_rd_from_live_lanes)
					return true;

				if (dest_lane == 0)
					return EmitStoreGpr64(rd, HOST_TMP2, HOST_TMP3);

				if (!EmitStoreCpuRegsU64(GprOffset(rd) + sizeof(u64),
						HOST_TMP2, HOST_TMP3, HOST_TMP0))
				{
					return false;
				}

				InvalidateGprQCacheForGuest(rd);
				return true;
			};

			const auto emit_lane = [this, rs, rt, subtract, load_word, store_signed_word_as_doubleword,
									   store_rd_lane_pair,
									   emit_divide_signed_64_by_u32_max, emit_pmaddw_lower_quirk](unsigned source_word, unsigned dest_lane) {
				const size_t lo_source_offset = LO_OFFSET + source_word * sizeof(u32);
				const size_t hi_source_offset = HI_OFFSET + source_word * sizeof(u32);
				const size_t lo_dest_offset = LO_OFFSET + dest_lane * sizeof(u64);
				const size_t hi_dest_offset = HI_OFFSET + dest_lane * sizeof(u64);

				if (!load_word(rs, source_word, HOST_TMP0) ||
					(!subtract && source_word == 0 &&
						!m_code.EmitMovRegShiftImm(HOST_TMP5, HOST_TMP0, VitaA32::ShiftType::LSL, 0)) ||
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
					if (!m_code.EmitRsbImm32(HOST_TMP4, HOST_TMP4, 0, true) ||
						!m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(hi_source_offset)) ||
						!m_code.EmitSbcReg(HOST_TMP3, HOST_TMP0, HOST_TMP3))
					{
						return false;
					}
				}
				else if (!m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(hi_source_offset)) ||
						 !m_code.EmitAddReg(HOST_TMP3, HOST_TMP3, HOST_TMP0) ||
						 (source_word == 0 && !emit_pmaddw_lower_quirk(HOST_TMP5)))
				{
					return false;
				}

				return emit_divide_signed_64_by_u32_max() &&
			           store_signed_word_as_doubleword(HOST_TMP3, hi_dest_offset) &&
			           store_rd_lane_pair(dest_lane);
			};

			if (!emit_lane(0, 0) || !emit_lane(2, 1))
				return false;

			if (rd == 0 || store_rd_from_live_lanes)
				return true;

			const auto copy_accumulator_to_rd = [this, rd](unsigned rd_word,
													size_t accumulator_offset) {
				return m_code.EmitLdrImm12(HOST_TMP0, HOST_CPU_REGS,
						   static_cast<u16>(accumulator_offset)) &&
			           EmitStoreGprWord(rd, rd_word, HOST_TMP0);
			};

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

			constexpr unsigned NEON_RS = 0;
			constexpr unsigned NEON_RT = 1;
			constexpr unsigned NEON_PRODUCT_LOW = 2;
			constexpr unsigned NEON_PRODUCT_HIGH = 3;
			constexpr unsigned NEON_LO_PRODUCTS = NEON_PRODUCT_LOW;
			constexpr unsigned NEON_HI_PRODUCTS = NEON_PRODUCT_HIGH;
			constexpr unsigned NEON_LO = 0;
			constexpr unsigned NEON_HI = 1;

			if (rs == 0 || rt == 0)
			{
#if defined(VITASX2_QEMU_VALIDATION)
				g_qemuMmiPackedHalfwordMultiplyAccumulateVectorOps++;
#endif
				if (rd == 0)
					return true;

				InvalidateGprQCacheForQreg(NEON_LO);
				InvalidateGprQCacheForQreg(NEON_HI);

				// PCSX2 MMI.cpp::PMADDH()/PMSUBH() add/subtract zero products when
				// either source is $zero; LO/HI remain authoritative and RD receives
				// the same even LO/HI words as the normal path.
				return EmitLoadCpuRegsQ128(LO_OFFSET, NEON_LO, HOST_TMP0) &&
			           EmitLoadCpuRegsQ128(HI_OFFSET, NEON_HI, HOST_TMP1) &&
			           m_code.EmitVtrnI32Q(NEON_LO, NEON_HI) &&
			           EmitStoreGprQ128(rd, NEON_LO, HOST_TMP2);
			}

			if (!EmitLoadGprQ128(rs, NEON_RS, HOST_TMP0) ||
				!EmitLoadGprQ128(rt, NEON_RT, HOST_TMP1))
			{
				return false;
			}

			InvalidateGprQCacheForQreg(NEON_RS);
			InvalidateGprQCacheForQreg(NEON_RT);
			InvalidateGprQCacheForQreg(NEON_PRODUCT_LOW);
			InvalidateGprQCacheForQreg(NEON_PRODUCT_HIGH);
			InvalidateGprQCacheForQreg(NEON_LO_PRODUCTS);
			InvalidateGprQCacheForQreg(NEON_HI_PRODUCTS);
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuMmiPackedHalfwordMultiplyAccumulateVectorOps++;
#endif

			// PCSX2 owners: MMI.cpp::PMADDH()/PMSUBH() and
			// x86/iMMI.cpp::recPMSUBH(). Products are signed 16x16->32 lanes;
			// swapping the middle D registers makes LO 0/1/4/5 and HI 2/3/6/7.
			if (!m_code.EmitVmullS16Q(NEON_PRODUCT_LOW, NEON_RS * 2, NEON_RT * 2) ||
				!m_code.EmitVmullS16Q(NEON_PRODUCT_HIGH, NEON_RS * 2 + 1, NEON_RT * 2 + 1) ||
				!m_code.EmitVswpD(NEON_PRODUCT_LOW * 2 + 1, NEON_PRODUCT_HIGH * 2) ||
				!EmitLoadCpuRegsQ128(LO_OFFSET, NEON_LO, HOST_TMP0) ||
				!EmitLoadCpuRegsQ128(HI_OFFSET, NEON_HI, HOST_TMP1))
			{
				return false;
			}

			const bool accumulator_ok = subtract ?
		                                    (m_code.EmitVsubI32Q(NEON_LO, NEON_LO, NEON_LO_PRODUCTS) &&
												m_code.EmitVsubI32Q(NEON_HI, NEON_HI, NEON_HI_PRODUCTS)) :
		                                    (m_code.EmitVaddI32Q(NEON_LO, NEON_LO, NEON_LO_PRODUCTS) &&
												m_code.EmitVaddI32Q(NEON_HI, NEON_HI, NEON_HI_PRODUCTS));
			if (!accumulator_ok ||
				!EmitStoreCpuRegsQ128(LO_OFFSET, NEON_LO, HOST_TMP0) ||
				!EmitStoreCpuRegsQ128(HI_OFFSET, NEON_HI, HOST_TMP1))
			{
				return false;
			}

			if (rd == 0)
				return true;

			return m_code.EmitVtrnI32Q(NEON_LO, NEON_HI) &&
		           EmitStoreGprQ128(rd, NEON_LO, HOST_TMP2);
		}

		bool BlockCompiler::EmitPackedHalfwordPairMultiply(u32 op, bool subtract)
		{
			const unsigned rd = RD(op);
			const unsigned rs = RS(op);
			const unsigned rt = RT(op);

			constexpr unsigned NEON_RS = 0;
			constexpr unsigned NEON_RT = 1;
			constexpr unsigned NEON_PAIR = 2;
			constexpr unsigned NEON_ODD = 3;
			constexpr unsigned NEON_LO = 0;
			constexpr unsigned NEON_HI = 1;

			if (rs == 0 || rt == 0)
			{
				InvalidateGprQCacheForQreg(NEON_LO);
				InvalidateGprQCacheForQreg(NEON_HI);
				InvalidateGprQCacheForQreg(NEON_PAIR);
#if defined(VITASX2_QEMU_VALIDATION)
				g_qemuMmiPackedHalfwordPairMultiplyVectorOps++;
#endif

				// PCSX2 MMI.cpp::PHMADH()/PHMSBH() produce zero pair results with a
				// $zero source; PHMSBH still writes the undocumented ~p(odd) side words.
				if (!subtract)
				{
					return m_code.EmitVeorQ(NEON_LO, NEON_LO, NEON_LO) &&
				           EmitStoreCpuRegsQ128(LO_OFFSET, NEON_LO, HOST_TMP0) &&
				           EmitStoreCpuRegsQ128(HI_OFFSET, NEON_LO, HOST_TMP1) &&
				           EmitStoreGprQ128(rd, NEON_LO, HOST_TMP2);
				}

				if (!m_code.EmitVeorQ(NEON_PAIR, NEON_PAIR, NEON_PAIR) ||
					!m_code.EmitVmvnQ(NEON_HI, NEON_PAIR))
				{
					return false;
				}

				if (rd == 0)
				{
					return m_code.EmitVtrnI32Q(NEON_PAIR, NEON_HI) &&
					       EmitStoreCpuRegsQ128(LO_OFFSET, NEON_PAIR, HOST_TMP0) &&
					       EmitStoreCpuRegsQ128(HI_OFFSET, NEON_HI, HOST_TMP1);
				}

				if (!m_code.EmitVorrQ(NEON_LO, NEON_PAIR, NEON_PAIR) ||
					!m_code.EmitVtrnI32Q(NEON_LO, NEON_HI) ||
					!EmitStoreCpuRegsQ128(LO_OFFSET, NEON_LO, HOST_TMP0) ||
					!EmitStoreCpuRegsQ128(HI_OFFSET, NEON_HI, HOST_TMP1))
				{
					return false;
				}

				return EmitStoreGprQ128(rd, NEON_PAIR, HOST_TMP2);
			}

			if (!EmitLoadGprQ128(rs, NEON_RS, HOST_TMP0) ||
				!EmitLoadGprQ128(rt, NEON_RT, HOST_TMP1))
			{
				return false;
			}

			InvalidateGprQCacheForQreg(NEON_RS);
			InvalidateGprQCacheForQreg(NEON_RT);
			InvalidateGprQCacheForQreg(NEON_PAIR);
			InvalidateGprQCacheForQreg(NEON_ODD);
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuMmiPackedHalfwordPairMultiplyVectorOps++;
#endif

			// PCSX2 owners: MMI.cpp::PHMADH()/PHMSBH() and
			// x86/iMMI.cpp::recPHMADH()/recPHMSBH(). The first word in each pair is
			// p(odd) +/- p(even); the second is p(odd), or ~p(odd) for PHMSBH.
			if (!m_code.EmitVmullS16Q(NEON_PAIR, NEON_RS * 2, NEON_RT * 2) ||
				!m_code.EmitVmullS16Q(NEON_ODD, NEON_RS * 2 + 1, NEON_RT * 2 + 1) ||
				!m_code.EmitVuzpI32Q(NEON_PAIR, NEON_ODD))
			{
				return false;
			}

			const bool pair_ok = subtract ?
		                             m_code.EmitVsubI32Q(NEON_PAIR, NEON_ODD, NEON_PAIR) :
		                             m_code.EmitVaddI32Q(NEON_PAIR, NEON_ODD, NEON_PAIR);
			if (!pair_ok)
				return false;

			if (subtract && !m_code.EmitVmvnQ(NEON_ODD, NEON_ODD))
				return false;

			if (rd == 0)
			{
				return m_code.EmitVtrnI32Q(NEON_PAIR, NEON_ODD) &&
				       EmitStoreCpuRegsQ128(LO_OFFSET, NEON_PAIR, HOST_TMP0) &&
				       EmitStoreCpuRegsQ128(HI_OFFSET, NEON_ODD, HOST_TMP1);
			}

			return m_code.EmitVorrQ(NEON_LO, NEON_PAIR, NEON_PAIR) &&
		           m_code.EmitVtrnI32Q(NEON_LO, NEON_ODD) &&
		           EmitStoreCpuRegsQ128(LO_OFFSET, NEON_LO, HOST_TMP0) &&
		           EmitStoreCpuRegsQ128(HI_OFFSET, NEON_ODD, HOST_TMP1) &&
		           EmitStoreGprQ128(rd, NEON_PAIR, HOST_TMP2);
		}

		bool BlockCompiler::EmitPackedHalfwordMultiply(u32 op)
		{
			const unsigned rd = RD(op);
			const unsigned rs = RS(op);
			const unsigned rt = RT(op);

			constexpr unsigned NEON_RS = 0;
			constexpr unsigned NEON_RT = 1;
			constexpr unsigned NEON_PRODUCT_LOW = 2;
			constexpr unsigned NEON_PRODUCT_HIGH = 3;
			constexpr unsigned NEON_RD = 0;
			constexpr unsigned NEON_LO = NEON_PRODUCT_LOW;
			constexpr unsigned NEON_HI = NEON_PRODUCT_HIGH;

			if (rs == 0 || rt == 0)
			{
				InvalidateGprQCacheForQreg(NEON_RD);
#if defined(VITASX2_QEMU_VALIDATION)
				g_qemuMmiPackedHalfwordMultiplyVectorOps++;
#endif

				// PCSX2 MMI.cpp::PMULTH() multiplies every signed halfword lane, so
				// architectural $zero as either source makes RD/LO/HI all zero.
				return m_code.EmitVeorQ(NEON_RD, NEON_RD, NEON_RD) &&
				       EmitStoreCpuRegsQ128(LO_OFFSET, NEON_RD, HOST_TMP0) &&
				       EmitStoreCpuRegsQ128(HI_OFFSET, NEON_RD, HOST_TMP1) &&
				       EmitStoreGprQ128(rd, NEON_RD, HOST_TMP2);
			}

			if (!EmitLoadGprQ128(rs, NEON_RS, HOST_TMP0) ||
				!EmitLoadGprQ128(rt, NEON_RT, HOST_TMP1))
			{
				return false;
			}

			InvalidateGprQCacheForQreg(NEON_RS);
			InvalidateGprQCacheForQreg(NEON_RT);
			InvalidateGprQCacheForQreg(NEON_PRODUCT_LOW);
			InvalidateGprQCacheForQreg(NEON_PRODUCT_HIGH);
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuMmiPackedHalfwordMultiplyVectorOps++;
#endif

			// PCSX2 owners: MMI.cpp::PMULTH() and x86/iMMI.cpp::recPMULTH().
			// VUZP first packs RD's active products 0/2/4/6; VTRN then restores
			// LO as 0/1/4/5 and HI as 2/3/6/7 without per-D-register copies.
			if (!m_code.EmitVmullS16Q(NEON_PRODUCT_LOW, NEON_RS * 2, NEON_RT * 2) ||
				!m_code.EmitVmullS16Q(NEON_PRODUCT_HIGH, NEON_RS * 2 + 1, NEON_RT * 2 + 1))
			{
				return false;
			}

			if (rd == 0)
			{
				// RD is suppressed, so assemble only PCSX2-visible LO/HI with
				// the same middle-D swap used by PMADDH/PMSUBH.
				return m_code.EmitVswpD(NEON_PRODUCT_LOW * 2 + 1, NEON_PRODUCT_HIGH * 2) &&
				       EmitStoreCpuRegsQ128(LO_OFFSET, NEON_LO, HOST_TMP0) &&
				       EmitStoreCpuRegsQ128(HI_OFFSET, NEON_HI, HOST_TMP1);
			}

			if (!m_code.EmitVuzpI32Q(NEON_LO, NEON_HI))
				return false;

			if (rd != 0 && !m_code.EmitVorrQ(NEON_RD, NEON_LO, NEON_LO))
			{
				return false;
			}

			if (!m_code.EmitVtrnI32Q(NEON_LO, NEON_HI) ||
				!EmitStoreCpuRegsQ128(LO_OFFSET, NEON_LO, HOST_TMP0) ||
				!EmitStoreCpuRegsQ128(HI_OFFSET, NEON_HI, HOST_TMP1))
			{
				return false;
			}

			return EmitStoreGprQ128(rd, NEON_RD, HOST_TMP2);
	}

	bool BlockCompiler::EmitPackedUnsignedWordMultiplyAdd(u32 op)
	{
		const unsigned rd = RD(op);
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);

		constexpr unsigned NEON_RS = 0;
		constexpr unsigned NEON_RT = 1;
		constexpr unsigned NEON_PRODUCT = 2;
		constexpr unsigned NEON_SIGN = 3;

		if (rs == 0 || rt == 0)
		{
			InvalidateGprQCacheForQreg(NEON_RS);
			InvalidateGprQCacheForQreg(NEON_RT);
			InvalidateGprQCacheForQreg(NEON_PRODUCT);
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuMmiPackedWordMultiplyAddVectorOps++;
#endif

			// PCSX2 MMI.cpp::_PMADDUW() adds a zero product to the raw
			// LO.UL[0/2]|HI.UL[0/2] accumulators when either source is $zero.
			if (!EmitLoadCpuRegsQ128(LO_OFFSET, NEON_RS, HOST_TMP0) ||
				!EmitLoadCpuRegsQ128(HI_OFFSET, NEON_RT, HOST_TMP1) ||
				!m_code.EmitVtrnI32Q(NEON_RS, NEON_RT) ||
				!m_code.EmitVorrQ(NEON_PRODUCT, NEON_RS, NEON_RS) ||
				!m_code.EmitVshrS32Q(NEON_RT, NEON_PRODUCT, 31) ||
				!m_code.EmitVtrnI32Q(NEON_PRODUCT, NEON_RT) ||
				!EmitStoreCpuRegsQ128(LO_OFFSET, NEON_PRODUCT, HOST_TMP0) ||
				!m_code.EmitVextI8Q(NEON_PRODUCT, NEON_RS, NEON_RS, 4) ||
				!m_code.EmitVshrS32Q(NEON_RT, NEON_PRODUCT, 31) ||
				!m_code.EmitVtrnI32Q(NEON_PRODUCT, NEON_RT) ||
				!EmitStoreCpuRegsQ128(HI_OFFSET, NEON_PRODUCT, HOST_TMP1))
			{
				return false;
			}

			return EmitStoreGprQ128(rd, NEON_RS, HOST_TMP2);
		}

		if (!EmitLoadGprQ128(rs, NEON_RS, HOST_TMP0) ||
			!EmitLoadGprQ128(rt, NEON_RT, HOST_TMP1))
		{
			return false;
		}

		InvalidateGprQCacheForQreg(NEON_RS);
		InvalidateGprQCacheForQreg(NEON_RT);
		InvalidateGprQCacheForQreg(NEON_PRODUCT);
		InvalidateGprQCacheForQreg(NEON_SIGN);
#if defined(VITASX2_QEMU_VALIDATION)
		g_qemuMmiPackedWordMultiplyAddVectorOps++;
#endif

		// PCSX2 owners: MMI.cpp::_PMADDUW() and x86/iMMI.cpp::recPMADDUW().
		// LO.UL[0/2] and HI.UL[0/2] form raw 64-bit accumulators; LO/HI are
		// written back as sign-extended low/high 32-bit result halves. As with
		// PMULTW/PMULTUW, VTRN.32 on each source D pair forms active words 0/2.
		if (!m_code.EmitVtrnI32D(NEON_RS * 2, NEON_RS * 2 + 1) ||
			!m_code.EmitVtrnI32D(NEON_RT * 2, NEON_RT * 2 + 1) ||
			!m_code.EmitVmullU32Q(NEON_PRODUCT, NEON_RS * 2, NEON_RT * 2) ||
			!EmitLoadCpuRegsQ128(LO_OFFSET, NEON_RS, HOST_TMP0) ||
			!EmitLoadCpuRegsQ128(HI_OFFSET, NEON_RT, HOST_TMP1) ||
			!m_code.EmitVtrnI32Q(NEON_RS, NEON_RT) ||
			!m_code.EmitVaddI64Q(NEON_PRODUCT, NEON_PRODUCT, NEON_RS))
		{
			return false;
		}

		if (rd == 0)
		{
			// PCSX2 MMI.cpp::PMADDUW() writes only LO/HI when RD is $zero; split
			// the accumulated product in place instead of keeping a pristine RD copy.
			return m_code.EmitVextI8Q(NEON_RT, NEON_PRODUCT, NEON_PRODUCT, 4) &&
			       m_code.EmitVshrS32Q(NEON_SIGN, NEON_PRODUCT, 31) &&
			       m_code.EmitVtrnI32Q(NEON_PRODUCT, NEON_SIGN) &&
			       EmitStoreCpuRegsQ128(LO_OFFSET, NEON_PRODUCT, HOST_TMP0) &&
			       m_code.EmitVshrS32Q(NEON_SIGN, NEON_RT, 31) &&
			       m_code.EmitVtrnI32Q(NEON_RT, NEON_SIGN) &&
			       EmitStoreCpuRegsQ128(HI_OFFSET, NEON_RT, HOST_TMP1);
		}

		if (!m_code.EmitVorrQ(NEON_RS, NEON_PRODUCT, NEON_PRODUCT) ||
			!m_code.EmitVshrS32Q(NEON_SIGN, NEON_RS, 31) ||
			!m_code.EmitVtrnI32Q(NEON_RS, NEON_SIGN) ||
			!EmitStoreCpuRegsQ128(LO_OFFSET, NEON_RS, HOST_TMP0) ||
			!m_code.EmitVextI8Q(NEON_RT, NEON_PRODUCT, NEON_PRODUCT, 4) ||
			!m_code.EmitVshrS32Q(NEON_SIGN, NEON_RT, 31) ||
			!m_code.EmitVtrnI32Q(NEON_RT, NEON_SIGN) ||
			!EmitStoreCpuRegsQ128(HI_OFFSET, NEON_RT, HOST_TMP1))
		{
			return false;
		}

		return EmitStoreGprQ128(rd, NEON_PRODUCT, HOST_TMP2);
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

		const auto emit_store_result = [this, rd](unsigned qreg) {
			return EmitStoreGprQ128(rd, qreg, HOST_TMP2);
		};

		const auto emit_zero_result = [&]() {
			return m_code.EmitVeorQ(NEON_RD, NEON_RD, NEON_RD) &&
				   emit_store_result(NEON_RD);
		};

		const auto emit_copy_result = [&](unsigned guest_reg) {
			if (guest_reg == 0)
				return emit_zero_result();

			if (guest_reg == rd)
				return true;

			return EmitLoadGprQ128(guest_reg, NEON_RD, HOST_TMP0) &&
				   emit_store_result(NEON_RD);
		};

		const auto emit_inverted_result = [&](unsigned guest_reg) {
			if (guest_reg == 0)
			{
				if (!m_code.EmitVeorQ(NEON_RD, NEON_RD, NEON_RD))
					return false;
			}
			else if (!EmitLoadGprQ128(guest_reg, NEON_RD, HOST_TMP0))
			{
				return false;
			}

			return m_code.EmitVmvnQ(NEON_RD, NEON_RD) &&
				   emit_store_result(NEON_RD);
		};

		// PCSX2 owners: MMI.cpp packed add/sub/compare/min/max/logical helpers.
		// Fold exact zero/idempotent cases before the generic two-source NEON path.
		switch (operation)
		{
			case MmiVectorOp::AddWord:
			case MmiVectorOp::AddHalfword:
			case MmiVectorOp::AddByte:
			case MmiVectorOp::SaturatingAddSignedWord:
			case MmiVectorOp::SaturatingAddSignedHalfword:
			case MmiVectorOp::SaturatingAddSignedByte:
			case MmiVectorOp::SaturatingAddUnsignedWord:
			case MmiVectorOp::SaturatingAddUnsignedHalfword:
			case MmiVectorOp::SaturatingAddUnsignedByte:
				if (rs == 0)
					return emit_copy_result(rt);
				if (rt == 0)
					return emit_copy_result(rs);
				break;
			case MmiVectorOp::SubtractWord:
			case MmiVectorOp::SubtractHalfword:
			case MmiVectorOp::SubtractByte:
			case MmiVectorOp::SaturatingSubtractSignedWord:
			case MmiVectorOp::SaturatingSubtractSignedHalfword:
			case MmiVectorOp::SaturatingSubtractSignedByte:
				if (rs == rt)
					return emit_zero_result();
				if (rt == 0)
					return emit_copy_result(rs);
				break;
			case MmiVectorOp::SaturatingSubtractUnsignedWord:
			case MmiVectorOp::SaturatingSubtractUnsignedHalfword:
			case MmiVectorOp::SaturatingSubtractUnsignedByte:
				if (rs == 0 || rs == rt)
					return emit_zero_result();
				if (rt == 0)
					return emit_copy_result(rs);
				break;
			case MmiVectorOp::CompareGreaterSignedWord:
			case MmiVectorOp::CompareGreaterSignedHalfword:
			case MmiVectorOp::CompareGreaterSignedByte:
				if (rs == rt)
					return emit_zero_result();
				break;
			case MmiVectorOp::CompareEqualWord:
			case MmiVectorOp::CompareEqualHalfword:
			case MmiVectorOp::CompareEqualByte:
				if (rs == rt)
					return emit_inverted_result(0);
				break;
			case MmiVectorOp::MaxSignedWord:
			case MmiVectorOp::MaxSignedHalfword:
			case MmiVectorOp::MinSignedWord:
			case MmiVectorOp::MinSignedHalfword:
				if (rs == rt)
					return emit_copy_result(rs);
				break;
			case MmiVectorOp::BitwiseAnd:
				if (rs == 0 || rt == 0)
					return emit_zero_result();
				if (rs == rt)
					return emit_copy_result(rs);
				break;
			case MmiVectorOp::BitwiseXor:
				if (rs == rt)
					return emit_zero_result();
				if (rs == 0)
					return emit_copy_result(rt);
				if (rt == 0)
					return emit_copy_result(rs);
				break;
			case MmiVectorOp::BitwiseOr:
				if (rs == rt)
					return emit_copy_result(rs);
				if (rs == 0)
					return emit_copy_result(rt);
				if (rt == 0)
					return emit_copy_result(rs);
				break;
			case MmiVectorOp::BitwiseNor:
				if (rs == rt)
					return emit_inverted_result(rs);
				if (rs == 0)
					return emit_inverted_result(rt);
				if (rt == 0)
					return emit_inverted_result(rs);
				break;
			default:
				break;
		}

		const auto emit_gpr_address = [this](unsigned guest_reg, unsigned host_reg) {
			return EmitCpuRegsAddress(host_reg, GprOffset(guest_reg));
		};

		const auto emit_store_rd = [&](unsigned qreg) {
			if (rd == rs && rs != 0)
			{
				return emit_gpr_address(rd, HOST_TMP0) &&
					   EmitStoreGprQ128ToAddress(rd, qreg, HOST_TMP0);
			}

			if (rd == rt && rt != 0)
			{
				return emit_gpr_address(rd, HOST_TMP1) &&
					   EmitStoreGprQ128ToAddress(rd, qreg, HOST_TMP1);
			}

			return EmitStoreGprQ128(rd, qreg, HOST_TMP2);
		};

		if (!EmitLoadGprQ128(rs, NEON_RS, HOST_TMP0))
		{
			return false;
		}

		if (!EmitLoadGprQ128(rt, NEON_RT, HOST_TMP1))
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
			!emit_store_rd(NEON_RD))
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
		if (rt == 0)
		{
			return m_code.EmitVeorQ(NEON_RT, NEON_RT, NEON_RT) &&
				   EmitStoreGprQ128(rd, NEON_RT, HOST_TMP2);
		}

		const auto emit_gpr_address = [this](unsigned guest_reg, unsigned host_reg) {
			return EmitCpuRegsAddress(host_reg, GprOffset(guest_reg));
		};

		const auto emit_store_rd = [&]() {
			if (rd == rt && rt != 0)
			{
				return emit_gpr_address(rd, HOST_TMP0) &&
					   EmitStoreGprQ128ToAddress(rd, NEON_RT, HOST_TMP0);
			}

			return EmitStoreGprQ128(rd, NEON_RT, HOST_TMP2);
		};

		if (!EmitLoadGprQ128(rt, NEON_RT, HOST_TMP0))
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
			!emit_store_rd())
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

		const u8 amount = static_cast<u8>((operation == MmiImmediateShiftOp::ShiftLeftHalfword ||
			operation == MmiImmediateShiftOp::ShiftRightLogicalHalfword ||
			operation == MmiImmediateShiftOp::ShiftRightArithmeticHalfword) ?
			(SA(op) & 0x0f) : SA(op));
		if (amount == 0 && rd == rt && rt != 0)
			return true;
		if (rt == 0)
		{
			return m_code.EmitVeorQ(NEON_RT, NEON_RT, NEON_RT) &&
				   EmitStoreGprQ128(rd, NEON_RT, HOST_TMP2);
		}

		const auto emit_gpr_address = [this](unsigned guest_reg, unsigned host_reg) {
			return EmitCpuRegsAddress(host_reg, GprOffset(guest_reg));
		};

		const auto emit_store_rd = [&]() {
			if (rd == rt && rt != 0)
			{
				return emit_gpr_address(rd, HOST_TMP0) &&
					   EmitStoreGprQ128ToAddress(rd, NEON_RT, HOST_TMP0);
			}

			return EmitStoreGprQ128(rd, NEON_RT, HOST_TMP2);
		};

		if (!EmitLoadGprQ128(rt, NEON_RT, HOST_TMP0))
		{
			return false;
		}

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
			!emit_store_rd())
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

		constexpr unsigned NEON_RT = 0;
		constexpr unsigned NEON_SHIFT = 1;
		constexpr unsigned NEON_SIGN = 2;

		const auto emit_zero_result = [&]() {
			return m_code.EmitVeorQ(NEON_RT, NEON_RT, NEON_RT) &&
				   EmitStoreGprQ128(rd, NEON_RT, HOST_TMP0);
		};

		const auto emit_sign_extend_active_words = [&]() {
			return m_code.EmitVshrS32Q(NEON_SIGN, NEON_RT, 31) &&
				   m_code.EmitVtrnI32Q(NEON_RT, NEON_SIGN);
		};

		if (rt == 0)
			return emit_zero_result();

		if (!EmitLoadGprQ128(rt, NEON_RT, HOST_TMP0))
			return false;

		InvalidateGprQCacheForQreg(NEON_SIGN);
#if defined(VITASX2_QEMU_VALIDATION)
		g_qemuMmiVariableWordShiftVectorOps++;
#endif

		if (rs != 0)
		{
			// PCSX2's x86 recPSLLVW/recPSRLVW/recPSRAVW extracts 5-bit counts
			// in vector regs. Keep RS on NEON instead of round-tripping through core regs.
			const int cached_shift_qreg = FindGprQCache(rs);
			if (cached_shift_qreg != static_cast<int>(NEON_SHIFT))
				InvalidateGprQCacheForQreg(NEON_SHIFT);

			const auto emit_shift_count_word = [&](unsigned source_word, unsigned target_sreg) {
				if (cached_shift_qreg >= 0)
				{
#if defined(VITASX2_QEMU_VALIDATION)
					g_qemuGprQCacheWordHits++;
#endif
					const unsigned source_sreg = static_cast<unsigned>(cached_shift_qreg) * 4 + source_word;
					return source_sreg == target_sreg ||
						   m_code.EmitVmovS(target_sreg, source_sreg);
				}

				if (source_word == 0)
				{
					const int pin_host = FindGprPinHost(rs);
					if (pin_host >= 0)
						return m_code.EmitVmovCoreToS(target_sreg, static_cast<unsigned>(pin_host));
				}

				return m_code.EmitVldrSImm(target_sreg, HOST_CPU_REGS,
					static_cast<u16>(GprOffset(rs) + source_word * sizeof(u32)));
			};

			if (!emit_shift_count_word(0, NEON_SHIFT * 4) ||
				!emit_shift_count_word(2, NEON_SHIFT * 4 + 2))
			{
				return false;
			}
			InvalidateGprQCacheForQreg(NEON_SHIFT);

			if (!m_code.EmitVshlI32Q(NEON_SHIFT, NEON_SHIFT, 27) ||
				!m_code.EmitVshrU32Q(NEON_SHIFT, NEON_SHIFT, 27))
			{
				return false;
			}

			if (operation != MmiVariableWordShiftOp::ShiftLeftLogical &&
				!m_code.EmitVnegS32Q(NEON_SHIFT, NEON_SHIFT))
			{
				return false;
			}

			bool shift_ok = true;
			switch (operation)
			{
				case MmiVariableWordShiftOp::ShiftLeftLogical:
					shift_ok = m_code.EmitVshlU32Q(NEON_RT, NEON_RT, NEON_SHIFT);
					break;
				case MmiVariableWordShiftOp::ShiftRightLogical:
					shift_ok = m_code.EmitVshlU32Q(NEON_RT, NEON_RT, NEON_SHIFT);
					break;
				case MmiVariableWordShiftOp::ShiftRightArithmetic:
					shift_ok = m_code.EmitVshlS32Q(NEON_RT, NEON_RT, NEON_SHIFT);
					break;
			}

			if (!shift_ok)
				return false;
		}

		return emit_sign_extend_active_words() &&
			   EmitStoreGprQ128(rd, NEON_RT, HOST_TMP2);
	}

	bool BlockCompiler::EmitMmiWordShuffleOp(u32 op, MmiWordShuffleOp operation)
	{
		const unsigned rd = RD(op);
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		if (rd == 0)
			return true;

		constexpr unsigned NEON_DWORD_PAIR = 0;
		constexpr unsigned NEON_LOW_D = NEON_DWORD_PAIR * 2;
		constexpr unsigned NEON_HIGH_D = NEON_DWORD_PAIR * 2 + 1;

		const auto emit_zero_qword = [&]() {
			return m_code.EmitVeorQ(NEON_DWORD_PAIR, NEON_DWORD_PAIR, NEON_DWORD_PAIR) &&
				   EmitStoreGprQ128(rd, NEON_DWORD_PAIR, HOST_TMP2);
		};

		const auto emit_copy_dword_pair = [&](unsigned low_guest_reg, unsigned low_dword,
			unsigned high_guest_reg, unsigned high_dword) {
			struct DwordSource
			{
				unsigned guest_reg;
				unsigned dword;
				unsigned target_d;
				int cached_qreg = -1;
				bool emitted_cache = false;
			};

			DwordSource low{low_guest_reg, low_dword, NEON_LOW_D};
			DwordSource high{high_guest_reg, high_dword, NEON_HIGH_D};
			if (low.guest_reg != 0)
				low.cached_qreg = FindGprQCache(low.guest_reg);
			if (high.guest_reg != 0)
				high.cached_qreg = FindGprQCache(high.guest_reg);

			const auto cached_source_d = [](const DwordSource& source) {
				return static_cast<unsigned>(source.cached_qreg) * 2 + source.dword;
			};

			const auto emit_cached_source = [&](DwordSource& source) {
				if (source.cached_qreg < 0 || source.emitted_cache)
					return true;

				source.emitted_cache = true;
#if defined(VITASX2_QEMU_VALIDATION)
				g_qemuGprQCacheDwordHits++;
#endif
				const unsigned source_d = cached_source_d(source);
				return source_d == source.target_d ||
					   m_code.EmitVorrD(source.target_d, source_d, source_d);
			};

			const auto q0_source_would_be_clobbered = [&](const DwordSource& source) {
				return source.cached_qreg == static_cast<int>(NEON_DWORD_PAIR) &&
					   cached_source_d(source) != source.target_d;
			};

			// Copy any q0-backed source half before the other half writes D0/D1.
			if ((q0_source_would_be_clobbered(low) && !emit_cached_source(low)) ||
				(q0_source_would_be_clobbered(high) && !emit_cached_source(high)) ||
				!emit_cached_source(low) ||
				!emit_cached_source(high))
			{
				return false;
			}

			const auto emit_uncached_source = [&](const DwordSource& source) {
				if (source.cached_qreg >= 0)
					return true;
				if (source.guest_reg == 0)
					return m_code.EmitVeorD(source.target_d, source.target_d, source.target_d);

				return m_code.EmitVldrDImm(source.target_d, HOST_CPU_REGS,
					static_cast<u16>(GprOffset(source.guest_reg) + source.dword * sizeof(u64)));
			};

			if (!emit_uncached_source(low) || !emit_uncached_source(high))
				return false;

			return EmitStoreGprQ128(rd, NEON_DWORD_PAIR, HOST_TMP2);
		};

		const auto emit_load_rt_q = [&]() {
			return EmitLoadGprQ128(rt, NEON_DWORD_PAIR, HOST_TMP0);
		};

		const auto emit_store_rd_rt_q = [&]() {
			if (rd == rt && rt != 0)
			{
				return EmitCpuRegsAddress(HOST_TMP0, GprOffset(rd)) &&
					   EmitStoreGprQ128ToAddress(rd, NEON_DWORD_PAIR, HOST_TMP0);
			}

			return EmitStoreGprQ128(rd, NEON_DWORD_PAIR, HOST_TMP2);
		};

		const auto emit_shuffle_rt_words = [&](MmiWordShuffleOp shuffle) {
			if (rt == 0)
				return emit_zero_qword();

			if (!emit_load_rt_q())
				return false;

#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuMmiWordShuffleVectorOps++;
#endif

			bool shuffle_ok = false;
			switch (shuffle)
			{
				case MmiWordShuffleOp::Pexew:
					shuffle_ok = m_code.EmitVtrnI32D(NEON_LOW_D, NEON_HIGH_D) &&
								 m_code.EmitVrev64I32D(NEON_LOW_D, NEON_LOW_D) &&
								 m_code.EmitVtrnI32D(NEON_LOW_D, NEON_HIGH_D);
					break;
				case MmiWordShuffleOp::Prot3w:
					shuffle_ok = m_code.EmitVextI8Q(NEON_DWORD_PAIR, NEON_DWORD_PAIR,
									  NEON_DWORD_PAIR, sizeof(u32)) &&
								 m_code.EmitVrev64I32D(NEON_HIGH_D, NEON_HIGH_D);
					break;
				case MmiWordShuffleOp::Pexcw:
					shuffle_ok = m_code.EmitVtrnI32D(NEON_LOW_D, NEON_HIGH_D);
					break;
				default:
					return false;
			}

			return shuffle_ok && emit_store_rd_rt_q();
		};

		switch (operation)
		{
			case MmiWordShuffleOp::Pcpyld:
				// PCSX2 owners: MMI.cpp::PCPYLD() and x86/iMMI.cpp::recPCPYLD().
				// Load both halves before storing so rd==rs/rt stays alias-safe.
				return emit_copy_dword_pair(rt, 0, rs, 0);
			case MmiWordShuffleOp::Pcpyud:
				// PCSX2 owners: MMI.cpp::PCPYUD() and x86/iMMI.cpp::recPCPYUD().
				return emit_copy_dword_pair(rs, 1, rt, 1);
			case MmiWordShuffleOp::Pexew:
				// PCSX2 owners: MMI.cpp::PEXEW() and x86/iMMI.cpp::recPEXEW().
				return emit_shuffle_rt_words(operation);
			case MmiWordShuffleOp::Prot3w:
				// PCSX2 owners: MMI.cpp::PROT3W() and x86/iMMI.cpp::recPROT3W().
				return emit_shuffle_rt_words(operation);
			case MmiWordShuffleOp::Pexcw:
				// PCSX2 owners: MMI.cpp::PEXCW() and x86/iMMI.cpp::recPEXCW().
				return emit_shuffle_rt_words(operation);
		}

		return false;
	}

	bool BlockCompiler::EmitMmiFiveBitOp(u32 op, MmiFiveBitOp operation)
	{
		const unsigned rd = RD(op);
		const unsigned rt = RT(op);
		if (rd == 0)
			return true;

		constexpr unsigned NEON_RT = 0;
		constexpr unsigned NEON_FIELD0 = 1;
		constexpr unsigned NEON_FIELD1 = 2;
		constexpr unsigned NEON_FIELD2 = 3;

		const auto emit_load_rt_q = [&]() {
			return EmitLoadGprQ128(rt, NEON_RT, HOST_TMP0);
		};

		const auto emit_store_rd_q = [&]() {
			if (rd == rt && rt != 0)
			{
				return EmitCpuRegsAddress(HOST_TMP0, GprOffset(rd)) &&
					   EmitStoreGprQ128ToAddress(rd, NEON_RT, HOST_TMP0);
			}

			return EmitStoreGprQ128(rd, NEON_RT, HOST_TMP2);
		};

		if (rt == 0)
		{
			return m_code.EmitVeorQ(NEON_RT, NEON_RT, NEON_RT) &&
				   emit_store_rd_q();
		}

		if (!emit_load_rt_q())
			return false;

		InvalidateGprQCacheForQreg(NEON_FIELD0);
		InvalidateGprQCacheForQreg(NEON_FIELD1);
		InvalidateGprQCacheForQreg(NEON_FIELD2);

#if defined(VITASX2_QEMU_VALIDATION)
		g_qemuMmiFiveBitVectorOps++;
#endif

		// PCSX2 owners: MMI.cpp::PEXT5()/PPAC5() and x86/iMMI.cpp::recPEXT5()/
		// recPPAC5(). Shifts isolate each 5:5:5:1 field per 32-bit lane without
		// materializing vector masks.
		if (operation == MmiFiveBitOp::Expand)
		{
			return m_code.EmitVshlI32Q(NEON_FIELD0, NEON_RT, 27) &&
				   m_code.EmitVshrU32Q(NEON_FIELD0, NEON_FIELD0, 27) &&
				   m_code.EmitVshlI32Q(NEON_FIELD0, NEON_FIELD0, 3) &&
				   m_code.EmitVshlI32Q(NEON_FIELD1, NEON_RT, 22) &&
				   m_code.EmitVshrU32Q(NEON_FIELD1, NEON_FIELD1, 27) &&
				   m_code.EmitVshlI32Q(NEON_FIELD1, NEON_FIELD1, 11) &&
				   m_code.EmitVshlI32Q(NEON_FIELD2, NEON_RT, 17) &&
				   m_code.EmitVshrU32Q(NEON_FIELD2, NEON_FIELD2, 27) &&
				   m_code.EmitVshlI32Q(NEON_FIELD2, NEON_FIELD2, 19) &&
				   m_code.EmitVshrU32Q(NEON_RT, NEON_RT, 15) &&
				   m_code.EmitVshlI32Q(NEON_RT, NEON_RT, 31) &&
				   m_code.EmitVorrQ(NEON_FIELD0, NEON_FIELD0, NEON_FIELD1) &&
				   m_code.EmitVorrQ(NEON_FIELD0, NEON_FIELD0, NEON_FIELD2) &&
				   m_code.EmitVorrQ(NEON_RT, NEON_RT, NEON_FIELD0) &&
				   emit_store_rd_q();
		}

		return m_code.EmitVshlI32Q(NEON_FIELD0, NEON_RT, 24) &&
			   m_code.EmitVshrU32Q(NEON_FIELD0, NEON_FIELD0, 27) &&
			   m_code.EmitVshlI32Q(NEON_FIELD1, NEON_RT, 16) &&
			   m_code.EmitVshrU32Q(NEON_FIELD1, NEON_FIELD1, 27) &&
			   m_code.EmitVshlI32Q(NEON_FIELD1, NEON_FIELD1, 5) &&
			   m_code.EmitVshlI32Q(NEON_FIELD2, NEON_RT, 8) &&
			   m_code.EmitVshrU32Q(NEON_FIELD2, NEON_FIELD2, 27) &&
			   m_code.EmitVshlI32Q(NEON_FIELD2, NEON_FIELD2, 10) &&
			   m_code.EmitVshrU32Q(NEON_RT, NEON_RT, 31) &&
			   m_code.EmitVshlI32Q(NEON_RT, NEON_RT, 15) &&
			   m_code.EmitVorrQ(NEON_FIELD0, NEON_FIELD0, NEON_FIELD1) &&
			   m_code.EmitVorrQ(NEON_FIELD0, NEON_FIELD0, NEON_FIELD2) &&
			   m_code.EmitVorrQ(NEON_RT, NEON_RT, NEON_FIELD0) &&
			   emit_store_rd_q();
	}

	bool BlockCompiler::EmitMmiHalfwordShuffleOp(u32 op, MmiHalfwordShuffleOp operation)
	{
		const unsigned rd = RD(op);
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		if (rd == 0)
			return true;

		constexpr unsigned NEON_RT = 0;
		constexpr unsigned NEON_RS = 1;

		const auto emit_zero_rt_result = [&]() {
			return m_code.EmitVeorQ(NEON_RT, NEON_RT, NEON_RT) &&
				   EmitStoreGprQ128(rd, NEON_RT, HOST_TMP2);
		};

		const auto emit_gpr_address = [this](unsigned guest_reg, unsigned host_reg) {
			return EmitCpuRegsAddress(host_reg, GprOffset(guest_reg));
		};

		const auto emit_load_q = [&](unsigned guest_reg, unsigned qreg, unsigned address_reg) {
			return EmitLoadGprQ128(guest_reg, qreg, address_reg);
		};

		const auto emit_store_rd_q = [&]() {
			if (rd == rt && rt != 0)
			{
				return emit_gpr_address(rd, HOST_TMP0) &&
					   EmitStoreGprQ128ToAddress(rd, NEON_RT, HOST_TMP0);
			}

			if (rd == rs && rs != 0)
			{
				return emit_gpr_address(rd, HOST_TMP1) &&
					   EmitStoreGprQ128ToAddress(rd, NEON_RT, HOST_TMP1);
			}

			return EmitStoreGprQ128(rd, NEON_RT, HOST_TMP2);
		};

		const auto emit_store_rd_rt_q = [&]() {
			if (rd == rt && rt != 0)
			{
				return emit_gpr_address(rd, HOST_TMP0) &&
					   EmitStoreGprQ128ToAddress(rd, NEON_RT, HOST_TMP0);
			}

			return EmitStoreGprQ128(rd, NEON_RT, HOST_TMP2);
		};

		const auto emit_store_rd_q_after_rs_clobber = [&]() {
			InvalidateGprQCacheForQreg(NEON_RS);
			return emit_store_rd_q();
		};

		const auto emit_store_rd_rt_q_after_rs_clobber = [&]() {
			InvalidateGprQCacheForQreg(NEON_RS);
			return emit_store_rd_rt_q();
		};

		if (operation == MmiHalfwordShuffleOp::Pinth)
		{
			// PCSX2 owners: MMI.cpp::PINTH() and x86/iMMI.cpp::recPINTH().
			// RT low halfwords are interleaved with RS upper halfwords.
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuMmiHalfwordShuffleVectorOps++;
#endif
			return emit_load_q(rt, NEON_RT, HOST_TMP0) &&
				   emit_load_q(rs, NEON_RS, HOST_TMP1) &&
				   m_code.EmitVextI8Q(NEON_RS, NEON_RS, NEON_RS, 8) &&
				   m_code.EmitVzipI16Q(NEON_RT, NEON_RS) &&
				   emit_store_rd_q_after_rs_clobber();
		}

		if (operation == MmiHalfwordShuffleOp::Pinteh)
		{
			// PCSX2 owners: MMI.cpp::PINTEH() and x86/iMMI.cpp::recPINTEH().
			// Keep even halfwords from each source, then interleave RT/RS.
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuMmiHalfwordShuffleVectorOps++;
#endif
			return emit_load_q(rt, NEON_RT, HOST_TMP0) &&
				   emit_load_q(rs, NEON_RS, HOST_TMP1) &&
				   m_code.EmitVuzpI16Q(NEON_RT, NEON_RS) &&
				   m_code.EmitVextI8Q(NEON_RS, NEON_RT, NEON_RT, 8) &&
				   m_code.EmitVzipI16Q(NEON_RT, NEON_RS) &&
				   emit_store_rd_q_after_rs_clobber();
		}

		if (operation == MmiHalfwordShuffleOp::Pexch)
		{
			// PCSX2 owners: MMI.cpp::PEXCH() and x86/iMMI.cpp::recPEXCH().
			// Duplicate RT, split even/odd halfwords, then zip 32-bit pairs.
			if (rt == 0)
				return emit_zero_rt_result();

#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuMmiHalfwordShuffleVectorOps++;
#endif
			return emit_load_q(rt, NEON_RT, HOST_TMP0) &&
				   m_code.EmitVorrQ(NEON_RS, NEON_RT, NEON_RT) &&
				   m_code.EmitVuzpI16Q(NEON_RT, NEON_RS) &&
				   m_code.EmitVzipI32Q(NEON_RT, NEON_RS) &&
				   emit_store_rd_rt_q_after_rs_clobber();
		}

		if (operation == MmiHalfwordShuffleOp::Prevh)
		{
			// PCSX2 owners: MMI.cpp::PREVH() and x86/iMMI.cpp::recPREVH().
			// PREVH reverses halfwords independently inside each 64-bit half.
			if (rt == 0)
				return emit_zero_rt_result();

#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuMmiHalfwordShuffleVectorOps++;
#endif
			return emit_load_q(rt, NEON_RT, HOST_TMP0) &&
				   m_code.EmitVrev64I16Q(NEON_RT, NEON_RT) &&
				   emit_store_rd_rt_q();
		}

		if (operation == MmiHalfwordShuffleOp::Pexeh)
		{
			// PCSX2 owners: MMI.cpp::PEXEH() and x86/iMMI.cpp::recPEXEH().
			// Duplicate even and odd halfword lanes with VTRN.16, swap the
			// duplicated even lane pairs inside each 64-bit half, then
			// interleave back to RT.US[2], RT.US[1], RT.US[0], RT.US[3].
			if (rt == 0)
				return emit_zero_rt_result();

			constexpr unsigned NEON_TMP = 1;
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuMmiHalfwordShuffleVectorOps++;
#endif
			return emit_load_q(rt, NEON_RT, HOST_TMP0) &&
				   m_code.EmitVorrQ(NEON_TMP, NEON_RT, NEON_RT) &&
				   m_code.EmitVtrnI16Q(NEON_RT, NEON_TMP) &&
				   m_code.EmitVrev64I32Q(NEON_RT, NEON_RT) &&
				   m_code.EmitVtrnI16Q(NEON_RT, NEON_TMP) &&
				   emit_store_rd_rt_q_after_rs_clobber();
		}

		if (operation == MmiHalfwordShuffleOp::Pcpyh)
		{
			// PCSX2 owners: MMI.cpp::PCPYH() and x86/iMMI.cpp::recPCPYH().
			// Broadcast the first halfword independently within each 64-bit half.
			if (rt == 0)
				return emit_zero_rt_result();

			constexpr unsigned NEON_RT_LOW_D = NEON_RT * 2;
			constexpr unsigned NEON_RT_HIGH_D = NEON_RT * 2 + 1;
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuMmiHalfwordShuffleVectorOps++;
#endif
			return emit_load_q(rt, NEON_RT, HOST_TMP0) &&
				   m_code.EmitVdupI16D(NEON_RT_LOW_D, NEON_RT_LOW_D, 0) &&
				   m_code.EmitVdupI16D(NEON_RT_HIGH_D, NEON_RT_HIGH_D, 0) &&
				   emit_store_rd_rt_q();
		}

		const auto emit_load_gpr_word = [this](unsigned guest_reg, unsigned word, unsigned host_reg) {
			return EmitLoadGprWord(guest_reg, word, host_reg);
		};

		const auto emit_store_rd_word = [this, rd](unsigned word, unsigned host_reg) {
			return EmitStoreGprWord(rd, word, host_reg);
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

		const auto emit_interleave = [&]() {
			switch (operation)
			{
				case MmiUpperInterleaveOp::Word:
					return m_code.EmitVzipI32Q(NEON_RT, NEON_RS);
				case MmiUpperInterleaveOp::Halfword:
					return m_code.EmitVzipI16Q(NEON_RT, NEON_RS);
				case MmiUpperInterleaveOp::Byte:
					return m_code.EmitVzipI8Q(NEON_RT, NEON_RS);
			}

			return false;
		};

		if (rs == 0 && rt == 0)
		{
			const unsigned result = upper_half ? NEON_RS : NEON_RT;
			return m_code.EmitVeorQ(result, result, result) &&
				   EmitStoreGprQ128(rd, result, HOST_TMP2);
		}

		// PCSX2 MMI.cpp::PEXTL*/PEXTU* interleave only one 64-bit half from
		// each source. Materialize those D halves directly instead of loading
		// full qwords; the other D halves are only written to the discarded
		// vzip result register.
		{
			const unsigned half_index = upper_half ? 1 : 0;
			const unsigned rt_d = NEON_RT * 2 + half_index;
			const unsigned rs_d = NEON_RS * 2 + half_index;
			const unsigned result = upper_half ? NEON_RS : NEON_RT;

			struct HalfSource
			{
				unsigned guest_reg;
				unsigned target_d;
				int cached_qreg = -1;
				bool emitted = false;
			};

			HalfSource rt_source{rt, rt_d};
			HalfSource rs_source{rs, rs_d};
			if (rt != 0)
				rt_source.cached_qreg = FindGprQCache(rt);
			if (rs != 0)
				rs_source.cached_qreg = FindGprQCache(rs);

			const auto cached_source_d = [half_index](const HalfSource& source) {
				return static_cast<unsigned>(source.cached_qreg) * 2 + half_index;
			};

			const auto emit_cached_source = [&](HalfSource& source) {
				if (source.cached_qreg < 0 || source.emitted)
					return true;

				source.emitted = true;
#if defined(VITASX2_QEMU_VALIDATION)
				g_qemuGprQCacheDwordHits++;
#endif
				const unsigned source_d = cached_source_d(source);
				return source_d == source.target_d ||
					   m_code.EmitVorrD(source.target_d, source_d, source_d);
			};

			const auto cached_source_would_be_clobbered = [&](const HalfSource& source,
				const HalfSource& clobber) {
				return source.cached_qreg >= 0 && !source.emitted &&
					   cached_source_d(source) == clobber.target_d &&
					   cached_source_d(source) != source.target_d;
			};

			if ((cached_source_would_be_clobbered(rt_source, rs_source) &&
				 !emit_cached_source(rt_source)) ||
				(cached_source_would_be_clobbered(rs_source, rt_source) &&
				 !emit_cached_source(rs_source)) ||
				!emit_cached_source(rt_source) ||
				!emit_cached_source(rs_source))
			{
				return false;
			}

			const auto emit_uncached_source = [&](HalfSource& source) {
				if (source.emitted)
					return true;
				source.emitted = true;

				if (source.guest_reg == 0)
					return m_code.EmitVeorD(source.target_d, source.target_d, source.target_d);

				return m_code.EmitVldrDImm(source.target_d, HOST_CPU_REGS,
					static_cast<u16>(GprOffset(source.guest_reg) + half_index * sizeof(u64)));
			};

			if (!emit_uncached_source(rt_source) ||
				!emit_uncached_source(rs_source))
			{
				return false;
			}

			InvalidateGprQCacheForQreg(NEON_RS);
			InvalidateGprQCacheForQreg(NEON_RT);
			return emit_interleave() &&
				   EmitStoreGprQ128(rd, result, HOST_TMP2);
		}
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
			return EmitCpuRegsAddress(host_reg, GprOffset(guest_reg));
		};

		const auto emit_store_rd = [&]() {
			if (rd == rs && rs != 0)
			{
				return emit_gpr_address(rd, HOST_TMP0) &&
					   EmitStoreGprQ128ToAddress(rd, NEON_RT, HOST_TMP0);
			}

			if (rd == rt && rt != 0)
			{
				return emit_gpr_address(rd, HOST_TMP1) &&
					   EmitStoreGprQ128ToAddress(rd, NEON_RT, HOST_TMP1);
			}

			return EmitStoreGprQ128(rd, NEON_RT, HOST_TMP2);
		};

		if (!EmitLoadGprQ128(rs, NEON_RS, HOST_TMP0))
		{
			return false;
		}

		if (!EmitLoadGprQ128(rt, NEON_RT, HOST_TMP1))
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
		if (!vector_op_ok)
			return false;

		InvalidateGprQCacheForQreg(NEON_RS);
		if (!emit_store_rd())
			return false;

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
		constexpr unsigned NEON_RS_HIGH_D = NEON_RS * 2 + 1;
		constexpr unsigned NEON_ZERO_LOW_D = NEON_RT * 2;

		const auto emit_zero_qword = [&]() {
			return m_code.EmitVeorQ(NEON_RS, NEON_RS, NEON_RS) &&
				   EmitStoreGprQ128(rd, NEON_RS, HOST_TMP2);
		};

		const auto emit_copy_qword = [&]() {
			if (rs == 0)
				return emit_zero_qword();

			if (rd == rs)
				return true;

			return EmitLoadGprQ128(rs, NEON_RS, HOST_TMP0) &&
				   EmitStoreGprQ128(rd, NEON_RS, HOST_TMP2);
		};

		if (rt == 0)
			return emit_copy_qword();

		if (rs == rt)
		{
			// PCSX2 owners: MMI.cpp::PADSBH() and x86/iMMI.cpp::recPADSBH().
			// Same-source PADSBH is lower-half zero and upper-half PADDH(rs, rs).
			if (rs == 0)
				return emit_zero_qword();

			return EmitLoadGprQ128(rs, NEON_RS, HOST_TMP0) &&
				   m_code.EmitVaddI16Q(NEON_RS, NEON_RS, NEON_RS) &&
				   m_code.EmitVeorQ(NEON_RT, NEON_RT, NEON_RT) &&
				   EmitStoreGprDwordPair(rd, NEON_ZERO_LOW_D, NEON_RS_HIGH_D);
		}

		if (!EmitLoadGprQ128(rs, NEON_RS, HOST_TMP0))
		{
			return false;
		}

		if (!EmitLoadGprQ128(rt, NEON_RT, HOST_TMP1))
		{
			return false;
		}

		// PCSX2 owner: MMI.cpp::PADSBH() subtracts lanes 0..3 with PSUBH
		// semantics and adds lanes 4..7 with PADDH semantics.
		return m_code.EmitVsubI16Q(NEON_SUB, NEON_RS, NEON_RT) &&
			   m_code.EmitVaddI16Q(NEON_ADD, NEON_RS, NEON_RT) &&
			   EmitStoreGprDwordPair(rd, NEON_SUB_LOW_D, NEON_ADD_HIGH_D);
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
			return EmitCpuRegsAddress(host_reg, GprOffset(guest_reg));
		};

		if (rt != 0 && rs == rt + 1)
		{
			// PCSX2 x86 owner: x86/iMMI.cpp::recQFSRV() directly loads from
			// contiguous RT||RS storage when RS physically follows RT.
			if (!emit_gpr_address(rt, HOST_TMP0) ||
				!m_code.EmitLdrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(SA_OFFSET)) ||
				!m_code.EmitAndImm8(HOST_TMP1, HOST_TMP1, 0x0f) ||
				!m_code.EmitAddReg(HOST_TMP0, HOST_TMP0, HOST_TMP1) ||
				!m_code.EmitVld1Q32(NEON_RD, HOST_TMP0) ||
				!EmitStoreGprQ128(rd, NEON_RD, HOST_TMP2))
			{
				return false;
			}

			return true;
		}

		if (!EmitLoadGprQ128(rs, NEON_RS, HOST_TMP0))
		{
			return false;
		}

		if (!EmitLoadGprQ128(rt, NEON_RT, HOST_TMP1))
		{
			return false;
		}

		// PCSX2 x86 owner: x86/iMMI.cpp::recQFSRV() spills RT||RS to tempqw
		// for non-contiguous registers, then performs one unaligned 16-byte load.
		return m_code.EmitSubImm8(HOST_SP, HOST_SP, 32) &&
			   m_code.EmitVst1Q32(NEON_RT, HOST_SP) &&
			   m_code.EmitAddImm8(HOST_TMP0, HOST_SP, 16) &&
			   m_code.EmitVst1Q32(NEON_RS, HOST_TMP0) &&
			   m_code.EmitLdrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(SA_OFFSET)) &&
			   m_code.EmitAndImm8(HOST_TMP1, HOST_TMP1, 0x0f) &&
			   m_code.EmitAddReg(HOST_TMP0, HOST_SP, HOST_TMP1) &&
			   m_code.EmitVld1Q32(NEON_RD, HOST_TMP0) &&
			   m_code.EmitAddImm8(HOST_SP, HOST_SP, 32) &&
			   EmitStoreGprQ128(rd, NEON_RD, HOST_TMP2);
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
		unsigned rs_host;
		if (!EmitGprLowOperand(RS(op), HOST_TMP0, &rs_host) ||
			!m_code.EmitAndImm8(HOST_TMP0, rs_host, 0x0f))
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
		unsigned rs_host;
		if (!EmitGprLowOperand(RS(op), HOST_TMP0, &rs_host) ||
			!m_code.EmitAndImm8(HOST_TMP0, rs_host, 0x07))
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
			ScalarLoadWidth::Byte, 0, true);
	}

	bool BlockCompiler::EmitLH(u32 op, u32 pc, u32 raw_cycles_through_instruction,
		const void* event_exit, bool branch_delay_slot)
	{
		return EmitLoadWithCounterReadEvent(op, pc, raw_cycles_through_instruction, event_exit,
			reinterpret_cast<const void*>(&VitaEeMemRead16Checked), true, 16, branch_delay_slot,
			ScalarLoadWidth::Halfword, 1, true);
	}

	bool BlockCompiler::EmitLW(u32 op, u32 pc, u32 raw_cycles_through_instruction,
		const void* event_exit, bool branch_delay_slot)
	{
		return EmitLoadWithCounterReadEvent(op, pc, raw_cycles_through_instruction, event_exit,
			reinterpret_cast<const void*>(&VitaEeMemRead32Checked), true, 0, branch_delay_slot,
			ScalarLoadWidth::Word, 3, true);
	}

	bool BlockCompiler::EmitLBU(u32 op, u32 pc, u32 raw_cycles_through_instruction,
		const void* event_exit, bool branch_delay_slot)
	{
		return EmitLoadWithCounterReadEvent(op, pc, raw_cycles_through_instruction, event_exit,
			reinterpret_cast<const void*>(&VitaEeMemRead8), false, 0, branch_delay_slot,
			ScalarLoadWidth::Byte, 0, true);
	}

	bool BlockCompiler::EmitLHU(u32 op, u32 pc, u32 raw_cycles_through_instruction,
		const void* event_exit, bool branch_delay_slot)
	{
		return EmitLoadWithCounterReadEvent(op, pc, raw_cycles_through_instruction, event_exit,
			reinterpret_cast<const void*>(&VitaEeMemRead16Checked), false, 0, branch_delay_slot,
			ScalarLoadWidth::Halfword, 1, true);
	}

	bool BlockCompiler::EmitLWU(u32 op, u32 pc, u32 raw_cycles_through_instruction, const void* event_exit)
	{
		return EmitLoadWithCounterReadEvent(op, pc, raw_cycles_through_instruction, event_exit,
			reinterpret_cast<const void*>(&VitaEeMemRead32Checked), false, 0, false,
			ScalarLoadWidth::Word, 3, false);
	}

	bool BlockCompiler::EmitLWL(u32 op)
	{
		return EmitPartialWordLoad(op, true);
	}

	bool BlockCompiler::EmitLWR(u32 op)
	{
		return EmitPartialWordLoad(op, false);
	}

	bool BlockCompiler::EmitLD(u32 op, u32 pc, u32 raw_cycles_through_instruction, const void* event_exit)
	{
		return EmitLoadWithCounterReadEvent(op, pc, raw_cycles_through_instruction, event_exit,
			reinterpret_cast<const void*>(&VitaEeMemRead64Checked), false, 0, false,
			ScalarLoadWidth::Dword, 7, false);
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
			!EmitAlignQwordAddress(HOST_TMP0, HOST_TMP1) ||
			!EmitVtlbNonHandlerHostAddress128(HOST_TMP0, HOST_TMP1, HOST_TMP2, &handler_fallback))
		{
			return false;
		}

		if (rt != 0 &&
			(!m_code.EmitVld1Q32Aligned(NEON_VALUE, HOST_TMP0) ||
			 !EmitStoreGprQ128(rt, NEON_VALUE, HOST_TMP1)))
		{
			return false;
		}

		m_qword_load_cold_tails.push_back({
			handler_fallback,
			m_code.Size(),
			reinterpret_cast<const void*>(&VitaEeMemRead128Aligned),
			rt,
		});
		return true;
	}

	bool BlockCompiler::EmitLWC1(u32 op)
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
			!m_code.EmitLdrImm12(HOST_TMP1, HOST_TMP0, 0) ||
			!m_code.EmitStrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(FprOffset(rt))))
		{
			return false;
		}

		m_cop1_word_memory_cold_tails.push_back({
			unaligned_fallback,
			handler_fallback,
			m_code.Size(),
			rt,
			false,
		});
		return true;
	}

	bool BlockCompiler::EmitLQC2(u32 op)
	{
		const unsigned rt = RT(op);
		constexpr unsigned NEON_VALUE = 0;

		size_t handler_fallback = static_cast<size_t>(-1);
		if (!EmitEffectiveAddress(op, HOST_TMP0) ||
			!EmitVtlbNonHandlerHostAddress128(HOST_TMP0, HOST_TMP1, HOST_TMP2, &handler_fallback) ||
			!EmitVu0SyncIfRunning(HOST_TMP0, HOST_TMP5) ||
			!m_code.EmitVld1Q32(NEON_VALUE, HOST_TMP0))
		{
			return false;
		}

		if (rt != 0 &&
			(!EmitVu0VfAddress(HOST_TMP0, rt) ||
			 !m_code.EmitVst1Q32Aligned(NEON_VALUE, HOST_TMP0)))
		{
			return false;
		}

		m_cop2_qword_memory_cold_tails.push_back({
			handler_fallback,
			m_code.Size(),
			reinterpret_cast<const void*>(&VitaEeMemReadVu0Quad),
			rt,
		});
		return true;
	}

	bool BlockCompiler::EmitSB(u32 op)
	{
		const unsigned rt = RT(op);

		size_t handler_fallback = static_cast<size_t>(-1);
		unsigned rt_host;
		if (!EmitEffectiveAddress(op, HOST_TMP0) ||
			!EmitVtlbNonHandlerHostAddress(HOST_TMP0, HOST_TMP1, HOST_TMP2, &handler_fallback) ||
			!EmitGprLowOperand(rt, HOST_TMP1, &rt_host) ||
			!m_code.EmitStrbImm12(rt_host, HOST_TMP0, 0))
		{
			return false;
		}

		m_scalar_store_cold_tails.push_back({
			static_cast<size_t>(-1),
			handler_fallback,
			m_code.Size(),
			0,
			0,
			nullptr,
			reinterpret_cast<const void*>(&VitaEeMemWrite8),
			rt,
			ScalarStoreWidth::Byte,
		});
		return true;
	}

	bool BlockCompiler::EmitSH(u32 op, u32 pc, u32 raw_cycles_through_instruction, const void* event_exit)
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

		unsigned rt_host;
		if (!EmitVtlbNonHandlerHostAddress(HOST_TMP0, HOST_TMP1, HOST_TMP2, &handler_fallback) ||
			!EmitGprLowOperand(rt, HOST_TMP1, &rt_host) ||
			!m_code.EmitStrhImm8(rt_host, HOST_TMP0, 0))
		{
			return false;
		}

		m_scalar_store_cold_tails.push_back({
			unaligned_fallback,
			handler_fallback,
			m_code.Size(),
			pc,
			raw_cycles_through_instruction,
			event_exit,
			reinterpret_cast<const void*>(&VitaEeMemWrite16Checked),
			rt,
			ScalarStoreWidth::Halfword,
		});
		return true;
	}

	bool BlockCompiler::EmitSW(u32 op, u32 pc, u32 raw_cycles_through_instruction, const void* event_exit)
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

		unsigned rt_host;
		if (!EmitVtlbNonHandlerHostAddress(HOST_TMP0, HOST_TMP1, HOST_TMP2, &handler_fallback) ||
			!EmitGprLowOperand(rt, HOST_TMP1, &rt_host) ||
			!m_code.EmitStrImm12(rt_host, HOST_TMP0, 0))
		{
			return false;
		}

		m_scalar_store_cold_tails.push_back({
			unaligned_fallback,
			handler_fallback,
			m_code.Size(),
			pc,
			raw_cycles_through_instruction,
			event_exit,
			reinterpret_cast<const void*>(&VitaEeMemWrite32Checked),
			rt,
			ScalarStoreWidth::Word,
		});
		return true;
	}

	bool BlockCompiler::EmitSWL(u32 op)
	{
		return EmitPartialWordStore(op, true);
	}

	bool BlockCompiler::EmitSWR(u32 op)
	{
		return EmitPartialWordStore(op, false);
	}

	bool BlockCompiler::EmitSD(u32 op, u32 pc, u32 raw_cycles_through_instruction, const void* event_exit)
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

		unsigned rt_low;
		if (!EmitVtlbNonHandlerHostAddress(HOST_TMP0, HOST_TMP1, HOST_TMP2, &handler_fallback) ||
			!EmitGpr64OperandLow(rt, HOST_TMP2, HOST_TMP3, &rt_low))
		{
			return false;
		}

		// PCSX2 owner: R5900OpcodeImpl.cpp::SD() via vtlb_memWrite64().
		// Unpinned values stay in r2/r3 so Cortex-A9 can issue one STRD;
		// pinned low words skip the load and store as two scalar words.
		if (rt_low == HOST_TMP2)
		{
			if (!m_code.EmitStrdImm8(HOST_TMP2, HOST_TMP3, HOST_TMP0, 0))
				return false;
		}
		else if (!m_code.EmitStrImm12(rt_low, HOST_TMP0, 0) ||
				 !m_code.EmitStrImm12(HOST_TMP3, HOST_TMP0, static_cast<u16>(sizeof(u32))))
		{
			return false;
		}

		m_scalar_store_cold_tails.push_back({
			unaligned_fallback,
			handler_fallback,
			m_code.Size(),
			pc,
			raw_cycles_through_instruction,
			event_exit,
			reinterpret_cast<const void*>(&VitaEeMemWrite64Checked),
			rt,
			ScalarStoreWidth::Dword,
		});
		return true;
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
			!EmitAlignQwordAddress(HOST_TMP0, HOST_TMP1) ||
			!EmitVtlbNonHandlerHostAddress128(HOST_TMP0, HOST_TMP1, HOST_TMP2, &handler_fallback))
		{
			return false;
		}

		if (rt == 0)
		{
			if (!EmitLoadRawGpr0KnownZeroFlag(HOST_TMP1) ||
				!m_code.EmitMovRegShiftImm(HOST_TMP1, HOST_TMP1, VitaA32::ShiftType::LSL, 0, true))
			{
				return false;
			}

			const size_t raw_fallback = m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
			if (raw_fallback == static_cast<size_t>(-1))
				return false;

			if (!m_code.EmitVeorQ(NEON_VALUE, NEON_VALUE, NEON_VALUE) ||
				!m_code.EmitVst1Q32Aligned(NEON_VALUE, HOST_TMP0))
			{
				return false;
			}

			const size_t zero_done = m_code.EmitBranchPlaceholder();
			if (zero_done == static_cast<size_t>(-1))
				return false;

			const size_t raw_fallback_target = m_code.Size();
			if (!m_code.PatchBranch(raw_fallback, raw_fallback_target, VitaA32::Condition::EQ) ||
				!EmitLoadCpuRegsQ128(GprOffset(0), NEON_VALUE, HOST_TMP1) ||
				!m_code.EmitVst1Q32Aligned(NEON_VALUE, HOST_TMP0) ||
				!m_code.PatchBranch(zero_done, m_code.Size()))
			{
				return false;
			}
		}
		else if (!EmitLoadCpuRegsQ128(GprOffset(rt), NEON_VALUE, HOST_TMP1) ||
				 !m_code.EmitVst1Q32Aligned(NEON_VALUE, HOST_TMP0))
		{
			return false;
		}

		m_qword_store_cold_tails.push_back({
			handler_fallback,
			m_code.Size(),
			reinterpret_cast<const void*>(&VitaEeMemWrite128Aligned),
			rt,
		});
		return true;
	}

	bool BlockCompiler::EmitSWC1(u32 op)
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
			!m_code.EmitLdrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(FprOffset(rt))) ||
			!m_code.EmitStrImm12(HOST_TMP1, HOST_TMP0, 0))
		{
			return false;
		}

		m_cop1_word_memory_cold_tails.push_back({
			unaligned_fallback,
			handler_fallback,
			m_code.Size(),
			rt,
			true,
		});
		return true;
	}

	bool BlockCompiler::EmitSQC2(u32 op)
	{
		const unsigned rt = RT(op);
		constexpr unsigned NEON_VALUE = 0;

		size_t handler_fallback = static_cast<size_t>(-1);
		if (!EmitEffectiveAddress(op, HOST_TMP0) ||
			!EmitVtlbNonHandlerHostAddress128(HOST_TMP0, HOST_TMP1, HOST_TMP2, &handler_fallback) ||
			!EmitVu0SyncIfRunning(HOST_TMP0, HOST_TMP5) ||
			!EmitVu0VfAddress(HOST_TMP1, rt) ||
			!m_code.EmitVld1Q32Aligned(NEON_VALUE, HOST_TMP1) ||
			!m_code.EmitVst1Q32(NEON_VALUE, HOST_TMP0))
		{
			return false;
		}

		m_cop2_qword_memory_cold_tails.push_back({
			handler_fallback,
			m_code.Size(),
			reinterpret_cast<const void*>(&VitaEeMemWriteVu0Quad),
			rt,
		});
		return true;
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

		if (rs == 0 || rt == 0)
		{
			const unsigned src = (rs == 0) ? rt : rs;
			if (src == 0)
				return EmitStoreGprZero64(rd);

			unsigned src_host;
			return EmitGprLowOperand(src, HOST_TMP0, &src_host) &&
				   m_code.EmitMovRegShiftImm(HOST_TMP1, src_host, VitaA32::ShiftType::ASR, 31) &&
				   EmitStoreGpr64(rd, src_host, HOST_TMP1);
		}

		unsigned rs_host;
		unsigned rt_host;
		return EmitGprLowOperand(rs, HOST_TMP0, &rs_host) &&
			   EmitGprLowOperand(rt, HOST_TMP1, &rt_host) &&
			   m_code.EmitAddReg(HOST_TMP0, rs_host, rt_host) &&
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

		if (rs == rt)
			return EmitStoreGprZero64(rd);

		if (rt == 0)
		{
			unsigned rs_host;
			return EmitGprLowOperand(rs, HOST_TMP0, &rs_host) &&
				   m_code.EmitMovRegShiftImm(HOST_TMP1, rs_host, VitaA32::ShiftType::ASR, 31) &&
				   EmitStoreGpr64(rd, rs_host, HOST_TMP1);
		}

		if (rs == 0)
		{
			unsigned rt_host;
			return EmitGprLowOperand(rt, HOST_TMP0, &rt_host) &&
				   m_code.EmitRsbImm32(HOST_TMP0, rt_host, 0) &&
				   m_code.EmitMovRegShiftImm(HOST_TMP1, HOST_TMP0, VitaA32::ShiftType::ASR, 31) &&
				   EmitStoreGpr64(rd, HOST_TMP0, HOST_TMP1);
		}

		unsigned rs_host;
		unsigned rt_host;
		return EmitGprLowOperand(rs, HOST_TMP0, &rs_host) &&
			   EmitGprLowOperand(rt, HOST_TMP1, &rt_host) &&
			   m_code.EmitSubReg(HOST_TMP0, rs_host, rt_host) &&
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

		if (rs == 0 || rt == 0)
		{
			const unsigned src = (rs == 0) ? rt : rs;
			if (src == 0)
				return EmitStoreGprZero64(rd);
			if (rd == src)
				return true;

			unsigned src_low;
			return EmitGpr64OperandLow(src, HOST_TMP0, HOST_TMP1, &src_low) &&
				   EmitStoreGpr64(rd, src_low, HOST_TMP1);
		}

		unsigned rs_low;
		unsigned rt_low;
		return EmitGpr64OperandLow(rs, HOST_TMP0, HOST_TMP1, &rs_low) &&
			   EmitGpr64OperandLow(rt, HOST_TMP2, HOST_TMP3, &rt_low) &&
			   m_code.EmitAddReg(HOST_TMP0, rs_low, rt_low, true) &&
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

		if (rs == rt)
			return EmitStoreGprZero64(rd);

		if (rt == 0)
		{
			if (rd == rs)
				return true;

			unsigned rs_low;
			return EmitGpr64OperandLow(rs, HOST_TMP0, HOST_TMP1, &rs_low) &&
				   EmitStoreGpr64(rd, rs_low, HOST_TMP1);
		}

		if (rs == 0)
		{
			unsigned rt_low;
			return EmitGpr64OperandLow(rt, HOST_TMP2, HOST_TMP3, &rt_low) &&
				   m_code.EmitRsbImm32(HOST_TMP0, rt_low, 0, true) &&
				   m_code.EmitMovImm8(HOST_TMP1, 0) &&
				   m_code.EmitSbcReg(HOST_TMP1, HOST_TMP1, HOST_TMP3) &&
				   EmitStoreGpr64(rd, HOST_TMP0, HOST_TMP1);
		}

		unsigned rs_low;
		unsigned rt_low;
		return EmitGpr64OperandLow(rs, HOST_TMP0, HOST_TMP1, &rs_low) &&
			   EmitGpr64OperandLow(rt, HOST_TMP2, HOST_TMP3, &rt_low) &&
			   m_code.EmitSubReg(HOST_TMP0, rs_low, rt_low, true) &&
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

		// PCSX2 owner: R5900OpcodeImpl.cpp::AND() assigns only UD[0].
		// Fold $zero/self identities before loading both low-64 operands.
		if (rs == 0 || rt == 0)
			return EmitStoreGprZero64(rd);
		if (rs == rt)
		{
			if (rd == rs)
				return true;

			unsigned rs_low;
			return EmitGpr64OperandLow(rs, HOST_TMP0, HOST_TMP1, &rs_low) &&
				   EmitStoreGpr64(rd, rs_low, HOST_TMP1);
		}

		unsigned rs_low;
		unsigned rt_low;
		return EmitGpr64OperandLow(rs, HOST_TMP0, HOST_TMP1, &rs_low) &&
			   EmitGpr64OperandLow(rt, HOST_TMP2, HOST_TMP3, &rt_low) &&
			   m_code.EmitAndReg(HOST_TMP0, rs_low, rt_low) &&
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

		// PCSX2 owner: R5900OpcodeImpl.cpp::OR() assigns only UD[0].
		if (rs == 0 || rt == 0 || rs == rt)
		{
			const unsigned src = (rs == 0) ? rt : rs;
			if (src == 0)
				return EmitStoreGprZero64(rd);
			if (rd == src)
				return true;

			unsigned src_low;
			return EmitGpr64OperandLow(src, HOST_TMP0, HOST_TMP1, &src_low) &&
				   EmitStoreGpr64(rd, src_low, HOST_TMP1);
		}

		unsigned rs_low;
		unsigned rt_low;
		return EmitGpr64OperandLow(rs, HOST_TMP0, HOST_TMP1, &rs_low) &&
			   EmitGpr64OperandLow(rt, HOST_TMP2, HOST_TMP3, &rt_low) &&
			   m_code.EmitOrrReg(HOST_TMP0, rs_low, rt_low) &&
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

		// PCSX2 owner: R5900OpcodeImpl.cpp::XOR() assigns only UD[0].
		if (rs == rt)
			return EmitStoreGprZero64(rd);
		if (rs == 0 || rt == 0)
		{
			const unsigned src = (rs == 0) ? rt : rs;
			if (rd == src)
				return true;

			unsigned src_low;
			return EmitGpr64OperandLow(src, HOST_TMP0, HOST_TMP1, &src_low) &&
				   EmitStoreGpr64(rd, src_low, HOST_TMP1);
		}

		unsigned rs_low;
		unsigned rt_low;
		return EmitGpr64OperandLow(rs, HOST_TMP0, HOST_TMP1, &rs_low) &&
			   EmitGpr64OperandLow(rt, HOST_TMP2, HOST_TMP3, &rt_low) &&
			   m_code.EmitEorReg(HOST_TMP0, rs_low, rt_low) &&
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

		// PCSX2 owner: R5900OpcodeImpl.cpp::NOR() assigns only UD[0].
		if (rs == 0 && rt == 0)
		{
			return m_code.EmitMovImm32(HOST_TMP0, 0xffffffffu) &&
				   m_code.EmitMovImm32(HOST_TMP1, 0xffffffffu) &&
				   EmitStoreGpr64(rd, HOST_TMP0, HOST_TMP1);
		}
		if (rs == 0 || rt == 0 || rs == rt)
		{
			const unsigned src = (rs == 0) ? rt : rs;
			unsigned src_low;
			return EmitGpr64OperandLow(src, HOST_TMP0, HOST_TMP1, &src_low) &&
				   m_code.EmitMvnReg(HOST_TMP0, src_low) &&
				   m_code.EmitMvnReg(HOST_TMP1, HOST_TMP1) &&
				   EmitStoreGpr64(rd, HOST_TMP0, HOST_TMP1);
		}

		unsigned rs_low;
		unsigned rt_low;
		return EmitGpr64OperandLow(rs, HOST_TMP0, HOST_TMP1, &rs_low) &&
			   EmitGpr64OperandLow(rt, HOST_TMP2, HOST_TMP3, &rt_low) &&
			   m_code.EmitOrrReg(HOST_TMP0, rs_low, rt_low) &&
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

		// PCSX2 owner: R5900OpcodeImpl.cpp::SLT() compares signed UD[0] and
		// writes the 0/1 result back to UD[0].
		if (rs == rt)
			return EmitStoreGprZero64(rd);
		if (rt == 0)
		{
			return EmitLoadGprHigh(rs, HOST_TMP0) &&
				   m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_TMP0, VitaA32::ShiftType::LSR, 31) &&
				   m_code.EmitMovImm8(HOST_TMP1, 0) &&
				   EmitStoreGpr64(rd, HOST_TMP0, HOST_TMP1);
		}

		unsigned rs_low;
		unsigned rt_low;
		return EmitGpr64OperandLow(rs, HOST_TMP0, HOST_TMP1, &rs_low) &&
			   EmitGpr64OperandLow(rt, HOST_TMP2, HOST_TMP3, &rt_low) &&
			   EmitSetLessThan64(rd, true, rs_low, HOST_TMP1, rt_low, HOST_TMP3);
	}

	bool BlockCompiler::EmitSLTU(u32 op)
	{
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		const unsigned rd = RD(op);

		if (rd == 0)
			return true;

		// PCSX2 owner: R5900OpcodeImpl.cpp::SLTU() compares unsigned UD[0].
		if (rs == rt || rt == 0)
			return EmitStoreGprZero64(rd);

		unsigned rs_low;
		unsigned rt_low;
		return EmitGpr64OperandLow(rs, HOST_TMP0, HOST_TMP1, &rs_low) &&
			   EmitGpr64OperandLow(rt, HOST_TMP2, HOST_TMP3, &rt_low) &&
			   EmitSetLessThan64(rd, false, rs_low, HOST_TMP1, rt_low, HOST_TMP3);
	}

	bool BlockCompiler::EmitShift32Immediate(u32 op, VitaA32::ShiftType shift)
	{
		const unsigned rt = RT(op);
		const unsigned rd = RD(op);
		const unsigned sa = SA(op);

		if (rd == 0)
			return true;

		// PCSX2 owner: R5900OpcodeImpl.cpp::SLL()/SRL()/SRA(). Shifting
		// register zero still writes signed-extended zero.
		if (rt == 0)
			return EmitStoreGprZero64(rd);

		unsigned rt_host;
		if (!EmitGprLowOperand(rt, HOST_TMP0, &rt_host))
			return false;

		// PCSX2 owner: R5900OpcodeImpl.cpp::SLL()/SRL()/SRA(). ARM immediate
		// LSR/ASR with amount 0 encodes a shift of 32; the R5900 sa=0 case
		// leaves the low word unchanged, so only the final sign extension is
		// needed.
		if (sa != 0)
		{
			if (!m_code.EmitMovRegShiftImm(HOST_TMP0, rt_host, shift, static_cast<u8>(sa)))
				return false;

			rt_host = HOST_TMP0;
		}

		return m_code.EmitMovRegShiftImm(HOST_TMP1, rt_host, VitaA32::ShiftType::ASR, 31) &&
			   EmitStoreGpr64(rd, rt_host, HOST_TMP1);
	}

	bool BlockCompiler::EmitShift32Variable(u32 op, VitaA32::ShiftType shift)
	{
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		const unsigned rd = RD(op);

		if (rd == 0)
			return true;

		// PCSX2 owner: R5900OpcodeImpl.cpp::SLLV()/SRLV()/SRAV(). The shift
		// amount is irrelevant when the source register is zero.
		if (rt == 0)
			return EmitStoreGprZero64(rd);

		// Register zero supplies a shift amount of 0; the 32-bit result still
		// needs the owner's sign-extension into the low 64-bit lane.
		unsigned rt_host;
		if (!EmitGprLowOperand(rt, HOST_TMP0, &rt_host))
			return false;

		if (rs != 0)
		{
			unsigned rs_host;
			if (!EmitGprLowOperand(rs, HOST_TMP2, &rs_host) ||
				!m_code.EmitAndImm8(HOST_TMP2, rs_host, 0x1f) ||
				!m_code.EmitMovRegShiftReg(HOST_TMP0, rt_host, shift, HOST_TMP2))
			{
				return false;
			}

			rt_host = HOST_TMP0;
		}

		return m_code.EmitMovRegShiftImm(HOST_TMP1, rt_host, VitaA32::ShiftType::ASR, 31) &&
			   EmitStoreGpr64(rd, rt_host, HOST_TMP1);
	}

	bool BlockCompiler::EmitShift64LeftImmediate(u32 op, unsigned amount)
	{
		const unsigned rt = RT(op);
		const unsigned rd = RD(op);

		if (rd == 0)
			return true;

		// PCSX2 owner: R5900OpcodeImpl.cpp::DSLL()/DSLL32(). Shifting
		// register zero writes zero for every immediate amount.
		if (rt == 0)
			return EmitStoreGprZero64(rd);

		unsigned rt_low;
		if (!EmitGpr64OperandLow(rt, HOST_TMP0, HOST_TMP1, &rt_low))
			return false;

		if (amount == 0)
		{
			return EmitStoreGpr64(rd, rt_low, HOST_TMP1);
		}
		else if (amount < 32)
		{
			return m_code.EmitMovRegShiftImm(HOST_TMP2, rt_low, VitaA32::ShiftType::LSR, static_cast<u8>(32 - amount)) &&
				   m_code.EmitMovRegShiftImm(HOST_TMP1, HOST_TMP1, VitaA32::ShiftType::LSL, static_cast<u8>(amount)) &&
				   m_code.EmitOrrReg(HOST_TMP1, HOST_TMP1, HOST_TMP2) &&
				   m_code.EmitMovRegShiftImm(HOST_TMP0, rt_low, VitaA32::ShiftType::LSL, static_cast<u8>(amount)) &&
				   EmitStoreGpr64(rd, HOST_TMP0, HOST_TMP1);
		}
		else if (amount == 32)
		{
			return m_code.EmitMovRegShiftImm(HOST_TMP1, rt_low, VitaA32::ShiftType::LSL, 0) &&
				   m_code.EmitMovImm8(HOST_TMP0, 0) &&
				   EmitStoreGpr64(rd, HOST_TMP0, HOST_TMP1);
		}

		return m_code.EmitMovRegShiftImm(HOST_TMP1, rt_low, VitaA32::ShiftType::LSL, static_cast<u8>(amount - 32)) &&
			   m_code.EmitMovImm8(HOST_TMP0, 0) &&
			   EmitStoreGpr64(rd, HOST_TMP0, HOST_TMP1);
	}

	bool BlockCompiler::EmitShift64RightImmediate(u32 op, unsigned amount, bool arithmetic)
	{
		const unsigned rt = RT(op);
		const unsigned rd = RD(op);

		if (rd == 0)
			return true;

		// PCSX2 owner: R5900OpcodeImpl.cpp::DSRL()/DSRL32()/DSRA()/DSRA32().
		// Shifting register zero writes zero for every immediate amount.
		if (rt == 0)
			return EmitStoreGprZero64(rd);

		unsigned rt_low;
		if (!EmitGpr64OperandLow(rt, HOST_TMP0, HOST_TMP1, &rt_low))
			return false;

		const VitaA32::ShiftType high_shift = arithmetic ? VitaA32::ShiftType::ASR : VitaA32::ShiftType::LSR;
		if (amount == 0)
		{
			return EmitStoreGpr64(rd, rt_low, HOST_TMP1);
		}
		else if (amount < 32)
		{
			return m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP1, VitaA32::ShiftType::LSL, static_cast<u8>(32 - amount)) &&
				   m_code.EmitMovRegShiftImm(HOST_TMP0, rt_low, VitaA32::ShiftType::LSR, static_cast<u8>(amount)) &&
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

		// PCSX2 owner: R5900OpcodeImpl.cpp::DSLLV(). The shift amount is
		// irrelevant when the source register is zero.
		if (rt == 0)
			return EmitStoreGprZero64(rd);
		// Register zero supplies a shift amount of 0.
		if (rs == 0)
		{
			if (rd == rt)
				return true;

			unsigned rt_low;
			return EmitGpr64OperandLow(rt, HOST_TMP0, HOST_TMP1, &rt_low) &&
				   EmitStoreGpr64(rd, rt_low, HOST_TMP1);
		}

		unsigned rs_host;
		if (!EmitLoadGpr64(rt, HOST_TMP0, HOST_TMP1) ||
			!EmitGprLowOperand(rs, HOST_TMP2, &rs_host) ||
			!m_code.EmitAndImm8(HOST_TMP2, rs_host, 0x3f) ||
			!EmitCmpImm32OrReg(HOST_TMP2, 32, HOST_TMP3))
		{
			return false;
		}

		const size_t ge32_branch = m_code.EmitBranchPlaceholder(VitaA32::Condition::CS);
		if (ge32_branch == static_cast<size_t>(-1))
			return false;

		if (!m_code.EmitCmpImm32(HOST_TMP2, 0))
		{
			return false;
		}

		const size_t zero_branch = m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
		if (zero_branch == static_cast<size_t>(-1))
			return false;

		if (!m_code.EmitRsbImm32(HOST_TMP3, HOST_TMP2, 32) ||
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

		// PCSX2 owner: R5900OpcodeImpl.cpp::DSRLV()/DSRAV(). The shift amount
		// is irrelevant when the source register is zero.
		if (rt == 0)
			return EmitStoreGprZero64(rd);
		// Register zero supplies a shift amount of 0.
		if (rs == 0)
		{
			if (rd == rt)
				return true;

			unsigned rt_low;
			return EmitGpr64OperandLow(rt, HOST_TMP0, HOST_TMP1, &rt_low) &&
				   EmitStoreGpr64(rd, rt_low, HOST_TMP1);
		}

		unsigned rs_host;
		if (!EmitLoadGpr64(rt, HOST_TMP0, HOST_TMP1) ||
			!EmitGprLowOperand(rs, HOST_TMP2, &rs_host) ||
			!m_code.EmitAndImm8(HOST_TMP2, rs_host, 0x3f) ||
			!EmitCmpImm32OrReg(HOST_TMP2, 32, HOST_TMP3))
		{
			return false;
		}

		const size_t ge32_branch = m_code.EmitBranchPlaceholder(VitaA32::Condition::CS);
		if (ge32_branch == static_cast<size_t>(-1))
			return false;

		if (!m_code.EmitCmpImm32(HOST_TMP2, 0))
		{
			return false;
		}

		const size_t zero_branch = m_code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
		if (zero_branch == static_cast<size_t>(-1))
			return false;

		const VitaA32::ShiftType high_shift = arithmetic ? VitaA32::ShiftType::ASR : VitaA32::ShiftType::LSR;
		if (!m_code.EmitRsbImm32(HOST_TMP3, HOST_TMP2, 32) ||
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

		unsigned rt_low;
		if (!EmitGpr64OperandLow(rt, HOST_TMP2, HOST_TMP3, &rt_low) ||
			!m_code.EmitOrrReg(HOST_TMP4, rt_low, HOST_TMP3, true))
		{
			return false;
		}

		const VitaA32::Condition skip_condition = move_on_zero ? VitaA32::Condition::NE : VitaA32::Condition::EQ;
		const size_t skip_store = m_code.EmitBranchPlaceholder(skip_condition);
		if (skip_store == static_cast<size_t>(-1))
			return false;

		unsigned rs_low;
		if (!EmitGpr64OperandLow(rs, HOST_TMP0, HOST_TMP1, &rs_low) ||
			!EmitStoreGpr64(rd, rs_low, HOST_TMP1))
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

	bool BlockCompiler::EmitCompareGpr64ForBranch(unsigned lhs_guest_reg, unsigned rhs_guest_reg)
	{
		// PCSX2 owners: Interpreter.cpp::BEQ()/BNE()/BEQL()/BNEL(). Branch
		// equality observes the low 64-bit SD[0] value; folding $zero only
		// changes how the A32 flags are produced.
		if (lhs_guest_reg == rhs_guest_reg)
			return m_code.EmitCmpReg(HOST_TMP0, HOST_TMP0);

		if (lhs_guest_reg == 0 || rhs_guest_reg == 0)
		{
			const unsigned nonzero_guest_reg = (lhs_guest_reg == 0) ? rhs_guest_reg : lhs_guest_reg;
			unsigned low_host;
			return EmitGpr64OperandLow(nonzero_guest_reg, HOST_TMP0, HOST_TMP1, &low_host) &&
				   m_code.EmitOrrReg(HOST_TMP0, low_host, HOST_TMP1, true);
		}

		unsigned lhs_low;
		unsigned rhs_low;
		return EmitGpr64OperandLow(lhs_guest_reg, HOST_TMP0, HOST_TMP1, &lhs_low) &&
			   EmitGpr64OperandLow(rhs_guest_reg, HOST_TMP2, HOST_TMP3, &rhs_low) &&
			   m_code.EmitCmpReg(lhs_low, rhs_low) &&
			   m_code.EmitCmpReg(HOST_TMP1, HOST_TMP3, VitaA32::Condition::EQ);
	}

	bool BlockCompiler::EmitBranchEqual(u32 op, bool branch_on_equal)
	{
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);

		return EmitCompareGpr64ForBranch(rs, rt) &&
			   m_code.EmitMovImm8(HOST_BRANCH_FLAG, 0) &&
			   m_code.EmitMovImm8(HOST_BRANCH_FLAG, 1, branch_on_equal ? VitaA32::Condition::EQ : VitaA32::Condition::NE);
	}

	bool BlockCompiler::EmitBranchSigned(u32 op, SignedBranchCondition condition)
	{
		const unsigned rs = RS(op);

		// PCSX2 owners: Interpreter.cpp::BLTZ/BGEZ/BLEZ/BGTZ and likely/link
		// variants compare the low 64-bit SD[0] value. $zero makes those
		// predicates compile-time constants.
		if (rs == 0)
		{
			const bool taken = condition == SignedBranchCondition::GreaterEqualZero ||
							   condition == SignedBranchCondition::LessEqualZero;
			return m_code.EmitMovImm8(HOST_BRANCH_FLAG, taken ? 1 : 0);
		}

		if (condition == SignedBranchCondition::LessThanZero ||
			condition == SignedBranchCondition::GreaterEqualZero)
		{
			return EmitLoadGprHigh(rs, HOST_TMP1) &&
				   m_code.EmitCmpImm32(HOST_TMP1, 0) &&
				   m_code.EmitMovImm8(HOST_BRANCH_FLAG, 0) &&
				   m_code.EmitMovImm8(HOST_BRANCH_FLAG, 1,
					   condition == SignedBranchCondition::LessThanZero ? VitaA32::Condition::LT : VitaA32::Condition::GE);
		}

		unsigned rs_low;
		if (!EmitGpr64OperandLow(rs, HOST_TMP0, HOST_TMP1, &rs_low) ||
			!m_code.EmitMovImm8(HOST_BRANCH_FLAG, 0) ||
			!m_code.EmitCmpImm32(HOST_TMP1, 0) ||
			!m_code.EmitMovImm8(HOST_BRANCH_FLAG, 1, VitaA32::Condition::LT) ||
			!m_code.EmitCmpImm32(rs_low, 0, VitaA32::Condition::EQ) ||
			!m_code.EmitMovImm8(HOST_BRANCH_FLAG, 1, VitaA32::Condition::EQ))
		{
			return false;
		}

		if (condition == SignedBranchCondition::LessEqualZero)
			return true;

		return m_code.EmitEorImm8(HOST_BRANCH_FLAG, HOST_BRANCH_FLAG, 1);
	}

	bool BlockCompiler::EmitCop1Branch(u32 op)
	{
		// PCSX2 owner: FPU.cpp::BC1F()/BC1T()/BC1FL()/BC1TL() branch on
		// FCR31.C; x86/iFPU.cpp::REC_FPUBRANCH() flushes FPU state before
		// entering the shared branch path. Vita helper-backed scalar COP1 ops
		// already commit through fpuRegs before a BC1 block observes FCR31.
		const unsigned rt = RT(op);
		const bool branch_on_true = rt == 0x01 || rt == 0x03;

		return m_code.EmitLdrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(FprcOffset(31))) &&
			   m_code.EmitTstImm32(HOST_TMP1, FPU_FCR31_CONDITION_FLAG) &&
			   m_code.EmitMovImm8(HOST_BRANCH_FLAG, 0) &&
			   m_code.EmitMovImm8(HOST_BRANCH_FLAG, 1,
				   branch_on_true ? VitaA32::Condition::NE : VitaA32::Condition::EQ);
	}

	bool BlockCompiler::EmitCop2Branch(u32 op)
	{
		// PCSX2 owners: COP2.cpp::BC2F()/BC2T()/BC2FL()/BC2TL() branch on
		// ((VU0.VI[REG_VPU_STAT].US[0] >> 8) & 1). x86/microVU_Macro.inl
		// tests the same bit through VU0.VI[REG_VPU_STAT].UL & 0x100.
		const unsigned rt = RT(op);
		const bool branch_on_true = rt == 0x01 || rt == 0x03;

		return EmitVu0ViAddress(HOST_TMP0, VU0_REG_VPU_STAT) &&
			   m_code.EmitLdrImm12(HOST_TMP1, HOST_TMP0, 0) &&
			   m_code.EmitTstImm32(HOST_TMP1, 0x100u) &&
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
			   m_code.EmitLdrImm12(HOST_TMP2, HOST_TMP0, DMAC_STAT_DMAC_OFFSET) &&
			   m_code.EmitBicRegShiftImm(HOST_TMP1, HOST_TMP1, HOST_TMP2, VitaA32::ShiftType::LSL, 0) &&
			   m_code.EmitMovImm32(HOST_TMP2, DMAC_CPCOND_MASK) &&
			   m_code.EmitAndReg(HOST_TMP1, HOST_TMP1, HOST_TMP2, true) &&
			   m_code.EmitMovImm8(HOST_BRANCH_FLAG, 0) &&
			   m_code.EmitMovImm8(HOST_BRANCH_FLAG, 1,
				   branch_on_true ? VitaA32::Condition::EQ : VitaA32::Condition::NE);
	}

	bool BlockCompiler::EmitSetLessThan64(unsigned guest_reg, bool signed_compare,
		unsigned lhs_low, unsigned lhs_high, unsigned rhs_low, unsigned rhs_high)
	{
		if (guest_reg == 0)
			return true;

		if (!signed_compare)
		{
			return m_code.EmitMovImm8(HOST_TMP4, 0) &&
				   m_code.EmitCmpReg(lhs_high, rhs_high) &&
				   m_code.EmitCmpReg(lhs_low, rhs_low, VitaA32::Condition::EQ) &&
				   m_code.EmitMovImm8(HOST_TMP4, 1, VitaA32::Condition::CC) &&
				   m_code.EmitMovImm8(HOST_TMP1, 0) &&
				   EmitStoreGpr64(guest_reg, HOST_TMP4, HOST_TMP1);
		}

		if (!m_code.EmitMovImm8(HOST_TMP4, 0) ||
			!m_code.EmitCmpReg(lhs_high, rhs_high))
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
		if (!m_code.EmitCmpReg(lhs_low, rhs_low) ||
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

	bool BlockCompiler::EmitSetLessThan64Imm(unsigned guest_reg, s32 imm, bool signed_compare,
		unsigned lhs_low, unsigned lhs_high)
	{
		if (guest_reg == 0)
			return true;

		// PCSX2 owner: R5900OpcodeImpl.cpp::SLTI()/SLTIU(). The immediate is
		// sign-extended to 64 bits, then compared as signed or unsigned.
		const u32 imm_low = static_cast<u32>(imm);
		const u32 imm_high = (imm < 0) ? 0xffffffffu : 0u;
		if (!signed_compare)
		{
			if (!m_code.EmitMovImm8(HOST_TMP4, 0) ||
				!m_code.EmitCmpImm32(lhs_high, imm_high) ||
				!(m_code.EmitCmpImm32(lhs_low, imm_low, VitaA32::Condition::EQ) ||
					(m_code.EmitMovImm32(HOST_TMP2, imm_low) &&
						m_code.EmitCmpReg(lhs_low, HOST_TMP2, VitaA32::Condition::EQ))) ||
				!m_code.EmitMovImm8(HOST_TMP4, 1, VitaA32::Condition::CC) ||
				!m_code.EmitMovImm8(HOST_TMP1, 0))
			{
				return false;
			}

			return EmitStoreGpr64(guest_reg, HOST_TMP4, HOST_TMP1);
		}

		if (!m_code.EmitMovImm8(HOST_TMP4, 0) ||
			!m_code.EmitCmpImm32(lhs_high, imm_high))
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
		if (!EmitCmpImm32OrReg(lhs_low, imm_low, HOST_TMP2) ||
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

		if (left && !m_code.EmitRsbImm32(HOST_TMP3, HOST_TMP3, 24))
		{
			return false;
		}

		if (!m_code.EmitBicImm32(HOST_TMP0, HOST_TMP0, 3) ||
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
					!m_code.EmitRsbImm32(HOST_TMP4, HOST_TMP3, 32) ||
					!m_code.EmitMovImm32(HOST_TMP2, 0xffffffffu) ||
					!m_code.EmitAndRegShiftReg(HOST_TMP1, HOST_TMP1, HOST_TMP2,
						VitaA32::ShiftType::LSR, HOST_TMP4) ||
					!m_code.EmitOrrRegShiftReg(HOST_TMP0, HOST_TMP1, HOST_TMP0,
						VitaA32::ShiftType::LSL, HOST_TMP3) ||
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
					!m_code.EmitRsbImm32(HOST_TMP4, HOST_TMP3, 32) ||
					!m_code.EmitMovImm32(HOST_TMP5, 0xffffffffu) ||
					!m_code.EmitAndRegShiftReg(HOST_TMP1, HOST_TMP1, HOST_TMP5,
						VitaA32::ShiftType::LSL, HOST_TMP4) ||
					!m_code.EmitOrrRegShiftReg(HOST_TMP0, HOST_TMP1, HOST_TMP0,
						VitaA32::ShiftType::LSR, HOST_TMP3) ||
					!m_code.EmitCmpImm32(HOST_TMP3, 0) ||
					!m_code.EmitMovRegShiftImm(HOST_TMP2, HOST_TMP0, VitaA32::ShiftType::ASR, 31, false,
						VitaA32::Condition::EQ) ||
					!EmitStoreGpr64(rt, HOST_TMP0, HOST_TMP2))
				{
					return false;
				}
			}
		}

		const void* fallback_helper = left ? reinterpret_cast<const void*>(&VitaEeMemReadWordLeft) :
											 reinterpret_cast<const void*>(&VitaEeMemReadWordRight);
		m_partial_memory_cold_tails.push_back({
			handler_fallback,
			m_code.Size(),
			fallback_helper,
			rt,
		});
		return true;
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

		if (left && !m_code.EmitRsbImm32(HOST_TMP3, HOST_TMP3, 24))
		{
			return false;
		}

		if (!m_code.EmitBicImm32(HOST_TMP0, HOST_TMP0, 3) ||
			!EmitVtlbNonHandlerHostAddress(HOST_TMP0, HOST_TMP1, HOST_TMP2, &handler_fallback) ||
			!m_code.EmitMovRegShiftImm(HOST_TMP5, HOST_TMP0, VitaA32::ShiftType::LSL, 0) ||
			!m_code.EmitLdrImm12(HOST_TMP0, HOST_TMP5, 0) ||
			!m_code.EmitLdrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(GprOffset(rt))) ||
			!m_code.EmitRsbImm32(HOST_TMP4, HOST_TMP3, 32) ||
			!m_code.EmitMovImm32(HOST_TMP2, 0xffffffffu) ||
			!(left ? m_code.EmitAndRegShiftReg(HOST_TMP0, HOST_TMP0, HOST_TMP2,
						   VitaA32::ShiftType::LSL, HOST_TMP4) :
						 m_code.EmitAndRegShiftReg(HOST_TMP0, HOST_TMP0, HOST_TMP2,
						   VitaA32::ShiftType::LSR, HOST_TMP4)) ||
			!(left ? m_code.EmitOrrRegShiftReg(HOST_TMP0, HOST_TMP0, HOST_TMP1,
						   VitaA32::ShiftType::LSR, HOST_TMP3) :
						 m_code.EmitOrrRegShiftReg(HOST_TMP0, HOST_TMP0, HOST_TMP1,
						   VitaA32::ShiftType::LSL, HOST_TMP3)) ||
			!m_code.EmitStrImm12(HOST_TMP0, HOST_TMP5, 0))
		{
			return false;
		}

		const void* fallback_helper = left ? reinterpret_cast<const void*>(&VitaEeMemWriteWordLeft) :
											 reinterpret_cast<const void*>(&VitaEeMemWriteWordRight);
		m_partial_memory_cold_tails.push_back({
			handler_fallback,
			m_code.Size(),
			fallback_helper,
			rt,
		});
		return true;
	}

	bool BlockCompiler::EmitPartialDwordLoad(u32 op, bool left)
	{
		// PCSX2 owners: R5900OpcodeImpl.cpp::LDL() / LDR().
		const unsigned rt = RT(op);

		size_t handler_fallback = static_cast<size_t>(-1);
		if (!EmitEffectiveAddress(op, HOST_TMP0) ||
			!m_code.EmitMovRegShiftImm(HOST_TMP5, HOST_TMP0, VitaA32::ShiftType::LSL, 0) ||
			!m_code.EmitAndImm8(HOST_TMP3, HOST_TMP0, 7) ||
			!m_code.EmitBicImm32(HOST_TMP0, HOST_TMP0, 7) ||
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
			if (!EmitCmpImm32OrReg(HOST_TMP3, shift, HOST_TMP4))
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

		const void* fallback_helper = left ? reinterpret_cast<const void*>(&VitaEeMemReadDwordLeft) :
											 reinterpret_cast<const void*>(&VitaEeMemReadDwordRight);
		const size_t done_target = m_code.Size();
		for (size_t branch : done_branches)
		{
			if (!m_code.PatchBranch(branch, done_target))
				return false;
		}

		if (!EmitRefreshGprPinFromBacking(rt))
			return false;

		const size_t join_offset = m_code.Size();
		m_partial_memory_cold_tails.push_back({
			handler_fallback,
			join_offset,
			fallback_helper,
			rt,
		});
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
			!m_code.EmitBicImm32(HOST_TMP0, HOST_TMP0, 7) ||
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
			if (!EmitCmpImm32OrReg(HOST_TMP3, shift, HOST_TMP4))
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

		const void* fallback_helper = left ? reinterpret_cast<const void*>(&VitaEeMemWriteDwordLeft) :
											 reinterpret_cast<const void*>(&VitaEeMemWriteDwordRight);
		const size_t done_target = m_code.Size();
		for (size_t branch : done_branches)
		{
			if (!m_code.PatchBranch(branch, done_target))
				return false;
		}

		m_partial_memory_cold_tails.push_back({
			handler_fallback,
			done_target,
			fallback_helper,
			rt,
		});
		return true;
	}

	bool BlockCompiler::EmitLoadWithCounterReadEvent(u32 op, u32 pc, u32 raw_cycles_through_instruction,
		const void* event_exit, const void* read_helper, bool sign_extend, unsigned sign_shift,
		bool branch_delay_slot, ScalarLoadWidth width, u8 alignment_mask, bool counter_read_event)
	{
		const unsigned rt = RT(op);
		const unsigned address_reg = (width == ScalarLoadWidth::Dword) ? HOST_TMP2 : HOST_TMP0;
		const unsigned vmap_reg = HOST_TMP1;
		const unsigned scratch_reg = (width == ScalarLoadWidth::Dword) ? HOST_TMP0 : HOST_TMP2;

		size_t unaligned_fallback = static_cast<size_t>(-1);
		size_t handler_fallback = static_cast<size_t>(-1);
		if (!EmitEffectiveAddress(op, address_reg))
			return false;

		if (alignment_mask != 0)
		{
			if (!m_code.EmitAndImm8(vmap_reg, address_reg, alignment_mask, true))
				return false;

			unaligned_fallback = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
			if (unaligned_fallback == static_cast<size_t>(-1))
				return false;
		}

		if (!EmitVtlbNonHandlerHostAddress(address_reg, vmap_reg, scratch_reg, &handler_fallback))
			return false;

		switch (width)
		{
			case ScalarLoadWidth::Byte:
				if (!(sign_extend ? m_code.EmitLdrsbImm8(HOST_TMP0, HOST_TMP0, 0) :
									 m_code.EmitLdrbImm12(HOST_TMP0, HOST_TMP0, 0)))
					return false;
				break;

			case ScalarLoadWidth::Halfword:
				if (!(sign_extend ? m_code.EmitLdrshImm8(HOST_TMP0, HOST_TMP0, 0) :
									 m_code.EmitLdrhImm8(HOST_TMP0, HOST_TMP0, 0)))
					return false;
				break;

			case ScalarLoadWidth::Word:
				if (!m_code.EmitLdrImm12(HOST_TMP0, HOST_TMP0, 0))
					return false;
				break;

			case ScalarLoadWidth::Dword:
				// PCSX2 owner: R5900OpcodeImpl.cpp::LD() via vtlb_memRead64().
				// Keep the translated address out of r0/r1 so Cortex-A9 can use
				// one LDRD instead of two dependent scalar loads.
				if (!m_code.EmitLdrdImm8(HOST_TMP0, HOST_TMP1, address_reg, 0))
				{
					return false;
				}
				break;
		}

		const auto emit_store_result = [&]() -> bool {
			if (width == ScalarLoadWidth::Dword)
			{
				if (rt == 0)
				{
					// PCSX2 owner: R5900OpcodeImpl.cpp::LD() writes cpuRegs.GPR.r[_Rt_]
					// directly. This is intentionally different from LQ's gpr_GetWritePtr().
					const size_t offset = GprOffset(0);
					return m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(offset)) &&
						   m_code.EmitStrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(offset + sizeof(u32))) &&
						   EmitRefreshRawGpr0KnownZeroFromLow64(HOST_TMP0, HOST_TMP1);
				}

				return EmitStoreGpr64(rt, HOST_TMP0, HOST_TMP1);
			}

			if (rt == 0)
				return true;

			if (sign_extend)
			{
				if (!m_code.EmitMovRegShiftImm(HOST_TMP1, HOST_TMP0, VitaA32::ShiftType::ASR, 31))
					return false;
			}
			else if (!m_code.EmitMovImm8(HOST_TMP1, 0))
			{
				return false;
			}

			return EmitStoreGpr64(rt, HOST_TMP0, HOST_TMP1);
		};

		if (!emit_store_result())
			return false;

		m_scalar_load_cold_tails.push_back({
			unaligned_fallback,
			handler_fallback,
			m_code.Size(),
			pc,
			raw_cycles_through_instruction,
			event_exit,
			read_helper,
			width,
			rt,
			sign_shift,
			sign_extend,
			branch_delay_slot,
			counter_read_event,
			address_reg,
		});
		return true;
	}

	bool BlockCompiler::EmitCounterReadFlagFromAddress(unsigned host_reg)
	{
		// PCSX2 owners: R5900OpcodeImpl.cpp::LB()/LBU()/LH()/LHU()/LW()
		// check (addr & 0xffffe000) == 0x10000000 after the load to force
		// an EE counter-read event test.
		constexpr u32 EE_COUNTER_PAGE_SHIFT = 13;
		constexpr u32 EE_COUNTER_PAGE_TAG = 0x10000000u >> EE_COUNTER_PAGE_SHIFT;
		return m_code.EmitMovRegShiftImm(HOST_TMP2, host_reg, VitaA32::ShiftType::LSR, EE_COUNTER_PAGE_SHIFT) &&
			   m_code.EmitCmpImm32(HOST_TMP2, EE_COUNTER_PAGE_TAG) &&
			   m_code.EmitMovImm8(HOST_BRANCH_FLAG, 0) &&
			   m_code.EmitMovImm8(HOST_BRANCH_FLAG, 1, VitaA32::Condition::EQ);
	}

	bool BlockCompiler::EmitCounterReadEventExit(u32 next_pc, u32 raw_cycles_through_instruction, const void* event_exit)
	{
		if (!event_exit || raw_cycles_through_instruction == 0)
			return false;

		if (!m_code.EmitCmpImm32(HOST_BRANCH_FLAG, 0))
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
			!m_code.EmitPop(m_saved_registers | REG_PC))
		{
			return false;
		}

		return m_code.PatchBranch(not_counter_read, m_code.Size(), VitaA32::Condition::EQ);
	}

	bool BlockCompiler::EmitAddressErrorEventExit(u32 next_pc, u32 raw_cycles_through_instruction,
		const void* event_exit, bool store)
	{
		if (!event_exit || raw_cycles_through_instruction == 0)
			return false;

		const u32 cycles = ScaleBlockCycles(raw_cycles_through_instruction);
		return m_code.EmitMovRegShiftImm(HOST_TMP4, HOST_TMP0, VitaA32::ShiftType::LSL, 0) &&
			   EmitStorePc(next_pc) &&
			   EmitAddScaledCyclesToCpu(cycles) &&
			   m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_TMP4, VitaA32::ShiftType::LSL, 0) &&
			   m_code.EmitMovImm8(HOST_TMP1, store ? 1 : 0) &&
			   m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&VitaEeRaiseAddressError)) &&
			   m_code.EmitCallAbsolute(event_exit) &&
			   m_code.EmitPop(m_saved_registers | REG_PC);
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
			   m_code.EmitPop(m_saved_registers | REG_PC);
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

		for (const QwordLoadColdTail& tail : m_qword_load_cold_tails)
		{
			if (!EmitQwordLoadColdTail(tail))
				return false;
		}

		for (const QwordStoreColdTail& tail : m_qword_store_cold_tails)
		{
			if (!EmitQwordStoreColdTail(tail))
				return false;
		}

		for (const Cop1WordMemoryColdTail& tail : m_cop1_word_memory_cold_tails)
		{
			if (!EmitCop1WordMemoryColdTail(tail))
				return false;
		}

		for (const Cop2QwordMemoryColdTail& tail : m_cop2_qword_memory_cold_tails)
		{
			if (!EmitCop2QwordMemoryColdTail(tail))
				return false;
		}

		for (const Vu0SyncColdTail& tail : m_vu0_sync_cold_tails)
		{
			if (!EmitVu0SyncColdTail(tail))
				return false;
		}

		for (const PartialMemoryColdTail& tail : m_partial_memory_cold_tails)
		{
			if (!EmitPartialMemoryColdTail(tail))
				return false;
		}

		m_scalar_load_cold_tails.clear();
		m_scalar_store_cold_tails.clear();
		m_qword_load_cold_tails.clear();
		m_qword_store_cold_tails.clear();
		m_cop1_word_memory_cold_tails.clear();
		m_cop2_qword_memory_cold_tails.clear();
		m_vu0_sync_cold_tails.clear();
		m_partial_memory_cold_tails.clear();
		return true;
	}

	bool BlockCompiler::EmitScalarLoadColdTail(const ScalarLoadColdTail& tail)
	{
		// PCSX2 owner: vtlb.cpp::vtlb_memRead*() /
		// R5900OpcodeImpl.cpp::LB/LBU/LH/LHU/LW/LWU/LD().
		// Handler and unaligned pages stay on the helper/event path, but the hot
		// non-handler VTLB path falls through to the next guest instruction.
		if (tail.unaligned_fallback != static_cast<size_t>(-1))
		{
			const size_t address_error_target = m_code.Size();
			if (!m_code.PatchBranch(tail.unaligned_fallback, address_error_target, VitaA32::Condition::NE) ||
				(tail.address_reg != HOST_TMP0 &&
					!m_code.EmitMovRegShiftImm(HOST_TMP0, tail.address_reg, VitaA32::ShiftType::LSL, 0)) ||
				!EmitAddressErrorEventExit(tail.pc + 4, tail.raw_cycles_through_instruction, tail.event_exit, false))
			{
				return false;
			}
		}

		const size_t fallback_target = m_code.Size();
		if (!m_code.PatchBranch(tail.handler_fallback, fallback_target, VitaA32::Condition::MI))
			return false;
		if (tail.address_reg != HOST_TMP0 &&
			!m_code.EmitMovRegShiftImm(HOST_TMP0, tail.address_reg, VitaA32::ShiftType::LSL, 0))
		{
			return false;
		}

		const bool needs_counter_event = tail.counter_read_event && tail.rt != 0;
		const auto emit_sign_extend_low = [&]() -> bool {
			switch (tail.sign_shift)
			{
				case 0:
					return true;
				case 16:
					return m_code.EmitSxth(HOST_TMP0, HOST_TMP0);
				case 24:
					return m_code.EmitSxtb(HOST_TMP0, HOST_TMP0);
				default:
					return m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_TMP0, VitaA32::ShiftType::LSL,
							   static_cast<u8>(tail.sign_shift)) &&
						   m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_TMP0, VitaA32::ShiftType::ASR,
							   static_cast<u8>(tail.sign_shift));
			}
		};
		const auto emit_store_result = [&]() -> bool {
			if (tail.width == ScalarLoadWidth::Dword)
			{
				if (tail.rt == 0)
				{
					// PCSX2 owner: R5900OpcodeImpl.cpp::LD() writes cpuRegs.GPR.r[_Rt_]
					// directly. This is intentionally different from LQ's gpr_GetWritePtr().
					const size_t offset = GprOffset(0);
					return m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(offset)) &&
						   m_code.EmitStrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(offset + sizeof(u32))) &&
						   EmitRefreshRawGpr0KnownZeroFromLow64(HOST_TMP0, HOST_TMP1);
				}

				return EmitStoreGpr64(tail.rt, HOST_TMP0, HOST_TMP1);
			}

			if (tail.rt == 0)
				return true;

			if (tail.sign_extend)
			{
				if (!m_code.EmitMovRegShiftImm(HOST_TMP1, HOST_TMP0, VitaA32::ShiftType::ASR, 31))
					return false;
			}
			else if (!m_code.EmitMovImm8(HOST_TMP1, 0))
			{
				return false;
			}

			return EmitStoreGpr64(tail.rt, HOST_TMP0, HOST_TMP1);
		};

		if ((tail.branch_delay_slot && needs_counter_event &&
			 !m_code.EmitMovRegShiftImm(HOST_TMP5, HOST_BRANCH_FLAG, VitaA32::ShiftType::LSL, 0)) ||
			(needs_counter_event && !EmitCounterReadFlagFromAddress(HOST_TMP0)) ||
			!m_code.EmitCallAbsolute(tail.read_helper) ||
			(tail.width != ScalarLoadWidth::Dword && tail.rt != 0 && tail.sign_extend && !emit_sign_extend_low()) ||
			!emit_store_result())
		{
			return false;
		}

		if (needs_counter_event &&
			(!EmitCounterReadEventExit(tail.pc + 4, tail.raw_cycles_through_instruction, tail.event_exit) ||
			 (tail.branch_delay_slot &&
				 !m_code.EmitMovRegShiftImm(HOST_BRANCH_FLAG, HOST_TMP5, VitaA32::ShiftType::LSL, 0))))
		{
			return false;
		}

		const size_t tail_done = m_code.EmitBranchPlaceholder();
		return tail_done != static_cast<size_t>(-1) &&
			   m_code.PatchBranch(tail_done, tail.join_offset);
	}

	bool BlockCompiler::EmitScalarStoreColdTail(const ScalarStoreColdTail& tail)
	{
		// PCSX2 owner: vtlb.cpp::vtlb_memWrite*() / R5900OpcodeImpl.cpp::SB/SH/SW/SD().
		// Handler and unaligned writes keep the existing helper/event behavior;
		// the non-handler VTLB path falls through after the native store.
		if (tail.unaligned_fallback != static_cast<size_t>(-1))
		{
			const size_t address_error_target = m_code.Size();
			if (!m_code.PatchBranch(tail.unaligned_fallback, address_error_target, VitaA32::Condition::NE) ||
				!EmitAddressErrorEventExit(tail.pc + 4, tail.raw_cycles_through_instruction, tail.event_exit, true))
			{
				return false;
			}
		}

		const size_t fallback_target = m_code.Size();
		if (!m_code.PatchBranch(tail.handler_fallback, fallback_target, VitaA32::Condition::MI))
			return false;

		switch (tail.width)
		{
			case ScalarStoreWidth::Byte:
			case ScalarStoreWidth::Halfword:
			case ScalarStoreWidth::Word:
				if (!EmitLoadGprLow(tail.rt, HOST_TMP1))
					return false;
				break;

			case ScalarStoreWidth::Dword:
				if (!EmitLoadGpr64(tail.rt, HOST_TMP1, HOST_TMP2))
					return false;
				break;
		}

		if (!m_code.EmitCallAbsolute(tail.write_helper))
			return false;

		const size_t tail_done = m_code.EmitBranchPlaceholder();
		return tail_done != static_cast<size_t>(-1) &&
			   m_code.PatchBranch(tail_done, tail.join_offset);
	}

	bool BlockCompiler::EmitQwordLoadColdTail(const QwordLoadColdTail& tail)
	{
		// PCSX2 owner: vtlb.cpp::vtlb_memRead128() / R5900OpcodeImpl.cpp::LQ().
		// Handler-backed pages call the existing helper; non-handler pages fall
		// through after the native NEON load/store.
		const size_t fallback_target = m_code.Size();
		if (!m_code.PatchBranch(tail.handler_fallback, fallback_target, VitaA32::Condition::MI) ||
			!m_code.EmitMovImm8(HOST_TMP1, static_cast<u8>(tail.rt)) ||
			!m_code.EmitCallAbsolute(tail.read_helper) ||
			!EmitRefreshGprPinFromBacking(tail.rt))
		{
			return false;
		}

		const size_t tail_done = m_code.EmitBranchPlaceholder();
		return tail_done != static_cast<size_t>(-1) &&
			   m_code.PatchBranch(tail_done, tail.join_offset);
	}

	bool BlockCompiler::EmitQwordStoreColdTail(const QwordStoreColdTail& tail)
	{
		// PCSX2 owner: vtlb.cpp::vtlb_memWrite128() / R5900OpcodeImpl.cpp::SQ().
		// Handler-backed pages call the existing helper; non-handler pages fall
		// through after the native NEON store.
		const size_t fallback_target = m_code.Size();
		if (!m_code.PatchBranch(tail.handler_fallback, fallback_target, VitaA32::Condition::MI) ||
			!m_code.EmitMovImm8(HOST_TMP1, static_cast<u8>(tail.rt)) ||
			!m_code.EmitCallAbsolute(tail.write_helper))
		{
			return false;
		}

		const size_t tail_done = m_code.EmitBranchPlaceholder();
		return tail_done != static_cast<size_t>(-1) &&
			   m_code.PatchBranch(tail_done, tail.join_offset);
	}

	bool BlockCompiler::EmitCop1WordMemoryColdTail(const Cop1WordMemoryColdTail& tail)
	{
		// PCSX2 owner: vtlb.cpp::vtlb_memRead32()/vtlb_memWrite32() plus
		// FPU.cpp::LWC1()/SWC1(). Unaligned FPU word accesses return before
		// memory dispatch in FPU.cpp, while handler pages still dispatch through vTLB.
		if (!m_code.PatchBranch(tail.unaligned_fallback, tail.join_offset, VitaA32::Condition::NE))
			return false;

		const size_t fallback_target = m_code.Size();
		if (!m_code.PatchBranch(tail.handler_fallback, fallback_target, VitaA32::Condition::MI))
			return false;

		if (tail.store)
		{
			if (!m_code.EmitLdrImm12(HOST_TMP1, HOST_CPU_REGS, static_cast<u16>(FprOffset(tail.rt))) ||
				!m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&memWrite32)))
			{
				return false;
			}
		}
		else if (!m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&memRead32)) ||
				 !m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(FprOffset(tail.rt))))
		{
			return false;
		}

		const size_t tail_done = m_code.EmitBranchPlaceholder();
		return tail_done != static_cast<size_t>(-1) &&
			   m_code.PatchBranch(tail_done, tail.join_offset);
	}

	bool BlockCompiler::EmitCop2QwordMemoryColdTail(const Cop2QwordMemoryColdTail& tail)
	{
		// PCSX2 owner: vtlb.cpp::vtlb_memRead128()/vtlb_memWrite128()
		// plus VU0.cpp::LQC2()/SQC2(). The handler branch is emitted before
		// the translated host pointer replaces the guest address, so HOST_TMP0
		// still carries the PCSX2 helper address argument on the cold edge.
		const size_t fallback_target = m_code.Size();
		if (!m_code.PatchBranch(tail.handler_fallback, fallback_target, VitaA32::Condition::MI) ||
			!m_code.EmitMovImm8(HOST_TMP1, static_cast<u8>(tail.rt)) ||
			!m_code.EmitCallAbsolute(tail.helper))
		{
			return false;
		}

		const size_t tail_done = m_code.EmitBranchPlaceholder();
		return tail_done != static_cast<size_t>(-1) &&
			   m_code.PatchBranch(tail_done, tail.join_offset);
	}

	bool BlockCompiler::EmitVu0SyncColdTail(const Vu0SyncColdTail& tail)
	{
		// PCSX2 owners: VU0.cpp::vu0Sync() / _vu0run(). Blocks branch here
		// only when VU0.VI[REG_VPU_STAT].UL bit 0 says macro VU0 is running;
		// idle VU0 stays on the fallthrough path.
		const size_t fallback_target = m_code.Size();
		if (!m_code.PatchBranch(tail.running_branch, fallback_target, VitaA32::Condition::NE))
			return false;

		const bool preserve = tail.preserve_reg < 16;
		if ((preserve &&
			 (!m_code.EmitMovRegShiftImm(tail.save_reg, tail.preserve_reg, VitaA32::ShiftType::LSL, 0))) ||
			!m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&vu0Sync)) ||
			(preserve &&
			 !m_code.EmitMovRegShiftImm(tail.preserve_reg, tail.save_reg, VitaA32::ShiftType::LSL, 0)))
		{
			return false;
		}

		const size_t tail_done = m_code.EmitBranchPlaceholder();
		return tail_done != static_cast<size_t>(-1) &&
			   m_code.PatchBranch(tail_done, tail.join_offset);
	}

	bool BlockCompiler::EmitPartialMemoryColdTail(const PartialMemoryColdTail& tail)
	{
		// PCSX2 owner: vtlb.cpp::vtlb_memRead*()/vtlb_memWrite*() plus
		// R5900OpcodeImpl.cpp::LWL/LWR/LDL/LDR/SWL/SWR/SDL/SDR().
		// Partial accesses do not raise address errors; handler-backed pages
		// call the existing helper, while non-handler pages fall through.
		const size_t fallback_target = m_code.Size();
		if (!m_code.PatchBranch(tail.handler_fallback, fallback_target, VitaA32::Condition::MI) ||
			!m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_TMP5, VitaA32::ShiftType::LSL, 0) ||
			!m_code.EmitMovImm8(HOST_TMP1, static_cast<u8>(tail.rt)) ||
			!m_code.EmitCallAbsolute(tail.helper) ||
			!EmitRefreshGprPinFromBacking(tail.rt))
		{
			return false;
		}

		const size_t tail_done = m_code.EmitBranchPlaceholder();
		return tail_done != static_cast<size_t>(-1) &&
			   m_code.PatchBranch(tail_done, tail.join_offset);
	}

	bool BlockCompiler::EmitDeviceTracePreInstruction(u32 pc)
	{
#if defined(VITASX2_QEMU_PROVIDER_FIXTURE)
		return true;
#else
		if (!Pcsx2Trace::IsGsTraceEnabled() && !Pcsx2Trace::IsVuTraceEnabled())
			return true;

		if (Pcsx2Trace::IsGsTraceEnabled())
		{
			if (!m_code.EmitMovImm32(HOST_TMP0, pc) ||
				!m_code.EmitCallAbsolute(
					reinterpret_cast<const void*>(
						static_cast<bool (*)(u32)>(&Pcsx2Trace::RecordGsPreEeInstruction))))
			{
				return false;
			}
		}

		if (Pcsx2Trace::IsVuTraceEnabled())
		{
			if (!m_code.EmitMovImm32(HOST_TMP0, pc) ||
				!m_code.EmitCallAbsolute(
					reinterpret_cast<const void*>(
						static_cast<bool (*)(u32)>(&Pcsx2Trace::RecordVuPreEeInstruction))))
			{
				return false;
			}
		}

		return true;
#endif
	}

	bool BlockCompiler::EmitLoadCpuRegsU64(size_t offset, unsigned host_low, unsigned host_high,
		unsigned address_scratch)
	{
		if (CanUseA32DualTransferPair(host_low, host_high))
		{
			if (offset <= 0xff)
				return m_code.EmitLdrdImm8(host_low, host_high, HOST_CPU_REGS, static_cast<u8>(offset));

			if (EmitCpuRegsAddress(address_scratch, offset))
				return m_code.EmitLdrdImm8(host_low, host_high, address_scratch, 0);
		}

		return m_code.EmitLdrImm12(host_low, HOST_CPU_REGS, static_cast<u16>(offset)) &&
			   m_code.EmitLdrImm12(host_high, HOST_CPU_REGS, static_cast<u16>(offset + sizeof(u32)));
	}

	bool BlockCompiler::EmitStoreCpuRegsU64(size_t offset, unsigned host_low, unsigned host_high,
		unsigned address_scratch)
	{
		if (CanUseA32DualTransferPair(host_low, host_high))
		{
			if (offset <= 0xff)
				return m_code.EmitStrdImm8(host_low, host_high, HOST_CPU_REGS, static_cast<u8>(offset));

			if (EmitCpuRegsAddress(address_scratch, offset))
				return m_code.EmitStrdImm8(host_low, host_high, address_scratch, 0);
		}

		return m_code.EmitStrImm12(host_low, HOST_CPU_REGS, static_cast<u16>(offset)) &&
			   m_code.EmitStrImm12(host_high, HOST_CPU_REGS, static_cast<u16>(offset + sizeof(u32)));
	}

	bool BlockCompiler::EmitLoadCpuRegsQ128(size_t offset, unsigned qreg, unsigned address_scratch)
	{
		if (offset == 0)
		{
			// PCSX2 owner: R5900.h::cpuRegistersPack keeps cpuRegs alignas(16)
			// with GPR first, so GPR[0] can use the aligned 128-bit NEON form.
			return m_code.EmitVld1Q32Aligned(qreg, HOST_CPU_REGS);
		}

		const unsigned low_d = qreg * 2;
		const unsigned high_d = low_d + 1;
		if (offset + sizeof(u64) <= 0x3fc && (offset & 0x3u) == 0)
		{
			return m_code.EmitVldrDImm(low_d, HOST_CPU_REGS, static_cast<u16>(offset)) &&
				   m_code.EmitVldrDImm(high_d, HOST_CPU_REGS, static_cast<u16>(offset + sizeof(u64)));
		}

		return EmitCpuRegsAddress(address_scratch, offset) &&
			   m_code.EmitVld1Q32Aligned(qreg, address_scratch);
	}

	bool BlockCompiler::EmitStoreCpuRegsQ128(size_t offset, unsigned qreg, unsigned address_scratch)
	{
		if (offset == 0)
		{
			// PCSX2 owner: R5900.h::cpuRegistersPack keeps cpuRegs alignas(16)
			// with GPR first, so GPR[0] can use the aligned 128-bit NEON form.
			return m_code.EmitVst1Q32Aligned(qreg, HOST_CPU_REGS);
		}

		const unsigned low_d = qreg * 2;
		const unsigned high_d = low_d + 1;
		if (offset + sizeof(u64) <= 0x3fc && (offset & 0x3u) == 0)
		{
			return m_code.EmitVstrDImm(low_d, HOST_CPU_REGS, static_cast<u16>(offset)) &&
				   m_code.EmitVstrDImm(high_d, HOST_CPU_REGS, static_cast<u16>(offset + sizeof(u64)));
		}

		return EmitCpuRegsAddress(address_scratch, offset) &&
			   m_code.EmitVst1Q32Aligned(qreg, address_scratch);
	}

	bool BlockCompiler::EmitCop1ExponentMask(unsigned host_reg)
	{
		if (!m_cop1_exponent_mask_available)
			return m_code.EmitMovImm32(host_reg, FPU_FLOAT_EXPONENT_MASK);

		if (host_reg == HOST_COP1_EXPONENT_MASK)
			return true;

		return m_code.EmitMovRegShiftImm(host_reg, HOST_COP1_EXPONENT_MASK, VitaA32::ShiftType::LSL, 0);
	}

	bool BlockCompiler::EmitAndCop1ExponentMask(unsigned rd, unsigned rn, unsigned scratch)
	{
		if (m_cop1_exponent_mask_available)
			return m_code.EmitAndReg(rd, rn, HOST_COP1_EXPONENT_MASK);

		return EmitAndImm32OrReg(rd, rn, FPU_FLOAT_EXPONENT_MASK, scratch);
	}

	bool BlockCompiler::EmitAndCop1FractionMask(unsigned rd, unsigned rn)
	{
		// PCSX2 owner: FPU.cpp fast paths inspect IEEE-754 fraction bits as
		// word & 0x007fffff; A32 UBFX extracts the same bit range in one insn.
		return m_code.EmitUbfx(rd, rn, 0, 23);
	}

	bool BlockCompiler::EmitAddScaledCyclesToCpu(u32 cycles)
	{
		if (!EmitLoadCpuRegsU64(CYCLE_OFFSET, HOST_TMP0, HOST_TMP1, HOST_TMP2))
			return false;

		if (!m_code.EmitAddImm32(HOST_TMP0, HOST_TMP0, cycles, true))
		{
			if (!m_code.EmitMovImm32(HOST_TMP2, cycles) ||
				!m_code.EmitAddReg(HOST_TMP0, HOST_TMP0, HOST_TMP2, true))
			{
				return false;
			}
		}

		return m_code.EmitAdcImm8(HOST_TMP1, HOST_TMP1, 0) &&
			   EmitStoreCpuRegsU64(CYCLE_OFFSET, HOST_TMP0, HOST_TMP1, HOST_TMP2);
	}

	bool BlockCompiler::EmitEffectiveAddress(u32 op, unsigned host_reg)
	{
		const unsigned rs = RS(op);
		const s32 imm = static_cast<s32>(IMM_S(op));

		// PCSX2 owner: R5900OpcodeImpl.cpp memory ops form
		// cpuRegs.GPR.r[_Rs_].UL[0] + _Imm_ before vtlb_memRead/Write.
		if (rs == 0)
			return m_code.EmitMovImm32(host_reg, static_cast<u32>(imm));

		// Callers mutate the address register, so fold a pinned base into the
		// displacement add instead of returning the pin itself.
		unsigned base_reg;
		if (!EmitGprLowOperand(rs, host_reg, &base_reg))
			return false;

		if (imm == 0)
		{
			return base_reg == host_reg ||
				   m_code.EmitMovRegShiftImm(host_reg, base_reg, VitaA32::ShiftType::LSL, 0);
		}

		if (imm > 0 && m_code.EmitAddImm32(host_reg, base_reg, static_cast<u32>(imm)))
			return true;

		if (imm < 0 && m_code.EmitSubImm32(host_reg, base_reg, static_cast<u32>(-imm)))
			return true;

		const unsigned scratch_reg = (host_reg == HOST_TMP2) ? HOST_TMP0 : HOST_TMP2;
		return m_code.EmitMovImm32(scratch_reg, static_cast<u32>(imm)) &&
			   m_code.EmitAddReg(host_reg, base_reg, scratch_reg);
	}

	bool BlockCompiler::EmitCpuRegsAddress(unsigned host_reg, size_t offset)
	{
		// PCSX2 owner: R5900.h::cpuRegisters keeps GPR/HI/LO as contiguous
		// 16-byte slots, so these guest-state addresses can use A32's
		// modified-immediate ADD whenever the struct offset encodes directly.
		if (offset <= 0xffffffffu && m_code.EmitAddImm32(host_reg, HOST_CPU_REGS, static_cast<u32>(offset)))
			return true;

		return m_code.EmitMovImm32(host_reg, static_cast<u32>(offset)) &&
			   m_code.EmitAddReg(host_reg, HOST_CPU_REGS, host_reg);
	}

	bool BlockCompiler::EmitLoadRawGpr0KnownZeroFlag(unsigned host_reg)
	{
		return m_code.EmitMovImm32(host_reg, static_cast<u32>(reinterpret_cast<uptr>(&s_raw_gpr0_known_zero))) &&
			   m_code.EmitLdrImm12(host_reg, host_reg, 0);
	}

	bool BlockCompiler::EmitRefreshRawGpr0KnownZeroFromLow64(unsigned low_reg, unsigned high_reg)
	{
		const size_t high64_offset = GprOffset(0) + 2 * sizeof(u32);
		return m_code.EmitOrrReg(HOST_TMP2, low_reg, high_reg) &&
			   m_code.EmitLdrImm12(HOST_TMP3, HOST_CPU_REGS, static_cast<u16>(high64_offset)) &&
			   m_code.EmitOrrReg(HOST_TMP2, HOST_TMP2, HOST_TMP3) &&
			   m_code.EmitLdrImm12(HOST_TMP3, HOST_CPU_REGS, static_cast<u16>(high64_offset + sizeof(u32))) &&
			   m_code.EmitOrrReg(HOST_TMP2, HOST_TMP2, HOST_TMP3, true) &&
			   m_code.EmitMovImm8(HOST_TMP2, 0) &&
			   m_code.EmitMovImm8(HOST_TMP2, 1, VitaA32::Condition::EQ) &&
			   m_code.EmitMovImm32(HOST_TMP3, static_cast<u32>(reinterpret_cast<uptr>(&s_raw_gpr0_known_zero))) &&
			   m_code.EmitStrImm12(HOST_TMP2, HOST_TMP3, 0);
	}

	bool BlockCompiler::EmitVu0SyncIfRunning(unsigned preserve_reg, unsigned save_reg)
	{
		// PCSX2 owners: VU0.cpp::vu0Sync() / _vu0run() and
		// x86/microVU_Macro.inl::mVUSyncVU0(). _vu0run() returns before any
		// side effect when VU0.VI[REG_VPU_STAT].UL bit 0 is clear. Keep that
		// idle case as fallthrough and branch only the running case to a cold
		// helper tail.
		if (!EmitVu0ViAddress(HOST_TMP1, VU0_REG_VPU_STAT) ||
			!m_code.EmitLdrImm12(HOST_TMP2, HOST_TMP1, 0) ||
			!m_code.EmitAndImm8(HOST_TMP2, HOST_TMP2, 1, true))
		{
			return false;
		}

		const size_t vu0_running = m_code.EmitBranchPlaceholder(VitaA32::Condition::NE);
		if (vu0_running == static_cast<size_t>(-1))
			return false;

		m_vu0_sync_cold_tails.push_back({
			vu0_running,
			m_code.Size(),
			preserve_reg,
			save_reg,
		});
		return true;
	}

	bool BlockCompiler::EmitVu0RegisterAddress(unsigned host_reg, size_t offset)
	{
		// PCSX2 owners: VU0.cpp and VU.h keep VU0 VF/VI state in one singleton.
		// Repeated COP2/VU0 memory blocks can address it from a resident base.
		if (m_vu0_base_available)
		{
			if (offset == 0)
			{
				if (host_reg == HOST_VU0_BASE)
					return true;

				return m_code.EmitMovRegShiftImm(host_reg, HOST_VU0_BASE, VitaA32::ShiftType::LSL, 0);
			}

			if (m_code.EmitAddImm32(host_reg, HOST_VU0_BASE, static_cast<u32>(offset)))
				return true;
		}

		return m_code.EmitMovImm32(host_reg,
			static_cast<u32>(reinterpret_cast<uptr>(&VU0)) + static_cast<u32>(offset));
	}

	bool BlockCompiler::EmitVu0VfAddress(unsigned host_reg, unsigned vf_reg)
	{
		return EmitVu0RegisterAddress(host_reg, VU0_VF_OFFSET + vf_reg * VU0_VF_STRIDE);
	}

	bool BlockCompiler::EmitVu0ViAddress(unsigned host_reg, unsigned vi_reg)
	{
		return EmitVu0RegisterAddress(host_reg, VU0_VI_OFFSET + vi_reg * VU0_VI_STRIDE);
	}

	bool BlockCompiler::EmitAlignQwordAddress(unsigned host_reg, unsigned)
	{
		return m_code.EmitBicImm32(host_reg, host_reg, 0x0f);
	}

	bool BlockCompiler::EmitVtlbNonHandlerHostAddress(unsigned host_reg, unsigned vmap_reg,
		unsigned scratch_reg, size_t* handler_fallback_branch)
	{
		// PCSX2 owner: vtlb.cpp::vtlb_memRead*() / vtlb_memWrite*().
		// Fast path only mirrors the non-handler VTLB case. Handler-backed
		// cache/MMIO/unmapped pages branch to the existing PCSX2 helper path.
		constexpr u8 VTLB_VIRTUAL_ENTRY_SHIFT = 2;
		static_assert((sizeof(vtlb_private::VTLBVirtual) >> VTLB_VIRTUAL_ENTRY_SHIFT) == 1);

		if (m_vtlb_registers_available)
		{
			if (!m_code.EmitMovRegShiftImm(scratch_reg, host_reg, VitaA32::ShiftType::LSR,
					vtlb_private::VTLB_PAGE_BITS) ||
				!m_code.EmitLdrRegShift(vmap_reg, HOST_VTLB_VMAP, scratch_reg, VitaA32::ShiftType::LSL,
					VTLB_VIRTUAL_ENTRY_SHIFT) ||
				!m_code.EmitAddReg(vmap_reg, vmap_reg, host_reg, true))
			{
				return false;
			}

			*handler_fallback_branch = m_code.EmitBranchPlaceholder(VitaA32::Condition::MI);
			if (*handler_fallback_branch == static_cast<size_t>(-1))
				return false;

			return m_code.EmitAddReg(host_reg, vmap_reg, HOST_VTLB_HOST_MEMORY_BASE);
		}

		if (!m_code.EmitMovImm32(vmap_reg,
				static_cast<u32>(reinterpret_cast<uptr>(&vtlb_private::vtlbdata.vmap))) ||
			!m_code.EmitLdrImm12(vmap_reg, vmap_reg, 0) ||
			!m_code.EmitMovRegShiftImm(scratch_reg, host_reg, VitaA32::ShiftType::LSR,
				vtlb_private::VTLB_PAGE_BITS) ||
			!m_code.EmitLdrRegShift(vmap_reg, vmap_reg, scratch_reg, VitaA32::ShiftType::LSL,
				VTLB_VIRTUAL_ENTRY_SHIFT) ||
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

	bool BlockCompiler::EmitGprLowOperand(unsigned guest_reg, unsigned fallback_host, unsigned* operand_host)
	{
		// Pinned guest registers are handed out directly as read-only A32 source
		// operands; the caller must consume the operand before any code that can
		// write that guest register and must never modify the returned register.
		const int pin_host = FindGprPinHost(guest_reg);
		if (pin_host >= 0)
		{
			*operand_host = static_cast<unsigned>(pin_host);
			return true;
		}

		*operand_host = fallback_host;
		return EmitLoadGprLow(guest_reg, fallback_host);
	}

	bool BlockCompiler::EmitLoadGprWord(unsigned guest_reg, unsigned word, unsigned host_reg)
	{
		if (word == 0)
			return EmitLoadGprLow(guest_reg, host_reg);

		if (guest_reg == 0)
			return m_code.EmitMovImm8(host_reg, 0);

		bool qcache_loaded = false;
		if (!TryEmitLoadGprWordFromQCache(guest_reg, word, host_reg, &qcache_loaded))
			return false;
		if (qcache_loaded)
			return true;

		return m_code.EmitLdrImm12(host_reg, HOST_CPU_REGS,
			static_cast<u16>(GprOffset(guest_reg) + word * sizeof(u32)));
	}

	bool BlockCompiler::EmitGprWordOperand(unsigned guest_reg, unsigned word, unsigned fallback_host,
		unsigned* operand_host)
	{
		if (word == 0)
			return EmitGprLowOperand(guest_reg, fallback_host, operand_host);

		*operand_host = fallback_host;
		if (guest_reg == 0)
			return m_code.EmitMovImm8(fallback_host, 0);

		bool qcache_loaded = false;
		if (!TryEmitLoadGprWordFromQCache(guest_reg, word, fallback_host, &qcache_loaded))
			return false;
		if (qcache_loaded)
			return true;

		return m_code.EmitLdrImm12(fallback_host, HOST_CPU_REGS,
			static_cast<u16>(GprOffset(guest_reg) + word * sizeof(u32)));
	}

	bool BlockCompiler::EmitGpr64OperandLow(unsigned guest_reg, unsigned fallback_low, unsigned host_high,
		unsigned* low_operand_host)
	{
		// 64-bit reads keep the LDRD pairing when the register is unpinned; a
		// pinned register supplies the low word directly and only the high word
		// is loaded. The low operand is read-only for the caller.
		const int pin_host = FindGprPinHost(guest_reg);
		if (pin_host >= 0)
		{
			*low_operand_host = static_cast<unsigned>(pin_host);
			return EmitLoadGprHigh(guest_reg, host_high);
		}

		*low_operand_host = fallback_low;
		return EmitLoadGpr64(guest_reg, fallback_low, host_high);
	}

	bool BlockCompiler::EmitLoadGprLow(unsigned guest_reg, unsigned host_reg)
	{
		if (guest_reg == 0)
			return m_code.EmitMovImm8(host_reg, 0);

		// Pinned registers are write-through, so the host copy always matches
		// memory and a 1-cycle move replaces the Cortex-A9 load-use stall.
		const int pin_host = FindGprPinHost(guest_reg);
		if (pin_host >= 0)
		{
			return m_code.EmitMovRegShiftImm(host_reg, static_cast<unsigned>(pin_host),
				VitaA32::ShiftType::LSL, 0);
		}

		return m_code.EmitLdrImm12(host_reg, HOST_CPU_REGS, static_cast<u16>(GprOffset(guest_reg)));
	}

	bool BlockCompiler::EmitLoadGprLowRawZero(unsigned guest_reg, unsigned host_reg)
	{
		if (guest_reg != 0)
			return EmitLoadGprLow(guest_reg, host_reg);

		// PCSX2 VU0.cpp::CTC2() reads cpuRegs.GPR.r[_Rt_].UL[0] directly, so
		// rt=$zero observes the raw backing slot instead of architectural zero.
		return m_code.EmitLdrImm12(host_reg, HOST_CPU_REGS, static_cast<u16>(GprOffset(0)));
	}

	bool BlockCompiler::EmitRefreshGprPinFromBacking(unsigned guest_reg)
	{
		if (guest_reg == 0)
			return true;

		const int pin_host = FindGprPinHost(guest_reg);
		if (pin_host < 0)
			return true;

		if (!m_code.EmitLdrImm12(static_cast<unsigned>(pin_host), HOST_CPU_REGS,
			static_cast<u16>(GprOffset(guest_reg))))
		{
			return false;
		}

		const int high_pin_host = FindGprPinHighHost(guest_reg);
		return high_pin_host < 0 ||
			   m_code.EmitLdrImm12(static_cast<unsigned>(high_pin_host), HOST_CPU_REGS,
				   static_cast<u16>(GprOffset(guest_reg) + sizeof(u32)));
	}

	bool BlockCompiler::TryEmitLoadGprWordFromQCache(unsigned guest_reg, unsigned word,
		unsigned host_reg, bool* emitted)
	{
		*emitted = false;
		if (word >= 4)
			return true;

		const int cached_qreg = FindGprQCache(guest_reg);
		if (cached_qreg < 0)
			return true;

		*emitted = true;
#if defined(VITASX2_QEMU_VALIDATION)
		g_qemuGprQCacheWordHits++;
#endif
		return m_code.EmitVmovSToCore(host_reg, static_cast<unsigned>(cached_qreg) * 4 + word);
	}

	bool BlockCompiler::EmitLoadGprHigh(unsigned guest_reg, unsigned host_reg)
	{
		if (guest_reg == 0)
			return m_code.EmitMovImm8(host_reg, 0);

		const int high_pin_host = FindGprPinHighHost(guest_reg);
		if (high_pin_host >= 0)
		{
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuGprPinnedHighWordHits++;
#endif
			return m_code.EmitMovRegShiftImm(host_reg, static_cast<unsigned>(high_pin_host),
				VitaA32::ShiftType::LSL, 0);
		}

		return m_code.EmitLdrImm12(host_reg, HOST_CPU_REGS, static_cast<u16>(GprOffset(guest_reg) + sizeof(u32)));
	}

	bool BlockCompiler::EmitLoadGpr64(unsigned guest_reg, unsigned host_low, unsigned host_high)
	{
		if (guest_reg == 0)
			return m_code.EmitMovImm8(host_low, 0) &&
				   m_code.EmitMovImm8(host_high, 0);

		const size_t offset = GprOffset(guest_reg);
		const int pin_host = FindGprPinHost(guest_reg);
		if (pin_host >= 0)
		{
			return m_code.EmitMovRegShiftImm(host_low, static_cast<unsigned>(pin_host),
					   VitaA32::ShiftType::LSL, 0) &&
				   EmitLoadGprHigh(guest_reg, host_high);
		}

		if (offset <= 0xff && CanUseA32DualTransferPair(host_low, host_high))
			return m_code.EmitLdrdImm8(host_low, host_high, HOST_CPU_REGS, static_cast<u8>(offset));

		return m_code.EmitLdrImm12(host_low, HOST_CPU_REGS, static_cast<u16>(offset)) &&
			   m_code.EmitLdrImm12(host_high, HOST_CPU_REGS, static_cast<u16>(offset + sizeof(u32)));
	}

	bool BlockCompiler::EmitLoadGprQ128(unsigned guest_reg, unsigned qreg, unsigned address_scratch)
	{
		const int cached_qreg = FindGprQCache(guest_reg);
		if (cached_qreg >= 0)
		{
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuGprQCacheHits++;
#endif
			const unsigned source_qreg = static_cast<unsigned>(cached_qreg);
			if (source_qreg == qreg)
				return true;

			InvalidateGprQCacheForQreg(qreg);
			return m_code.EmitVorrQ(qreg, source_qreg, source_qreg);
		}

		InvalidateGprQCacheForQreg(qreg);
		if (guest_reg == 0)
			return m_code.EmitVeorQ(qreg, qreg, qreg);

#if defined(VITASX2_QEMU_VALIDATION)
		if (m_gpr_q_cache_enabled)
			g_qemuGprQCacheMisses++;
#endif
		if (!EmitLoadCpuRegsQ128(GprOffset(guest_reg), qreg, address_scratch))
			return false;

		MarkGprQCache(guest_reg, qreg);
		return true;
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
			!m_code.EmitCmpImm32(HOST_BRANCH_FLAG, 0))
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

	bool BlockCompiler::EmitStoreGprZero64(unsigned guest_reg)
	{
		if (guest_reg == 0)
			return true;

		const int pin_host = FindGprPinHost(guest_reg);
		if (pin_host >= 0 && !m_code.EmitMovImm8(static_cast<unsigned>(pin_host), 0))
			return false;
		const int high_pin_host = FindGprPinHighHost(guest_reg);
		if (high_pin_host >= 0 && !m_code.EmitMovImm8(static_cast<unsigned>(high_pin_host), 0))
			return false;

		const size_t offset = GprOffset(guest_reg);
		bool stored = false;
		if (offset <= 0xff && CanUseA32DualTransferPair(HOST_TMP0, HOST_TMP1))
		{
			stored = m_code.EmitMovImm8(HOST_TMP0, 0) &&
					 m_code.EmitMovImm8(HOST_TMP1, 0) &&
					 m_code.EmitStrdImm8(HOST_TMP0, HOST_TMP1, HOST_CPU_REGS, static_cast<u8>(offset));
		}
		else
		{
			stored = m_code.EmitMovImm8(HOST_TMP0, 0) &&
					 m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(offset)) &&
					 m_code.EmitStrImm12(HOST_TMP0, HOST_CPU_REGS, static_cast<u16>(offset + sizeof(u32)));
		}

		if (!stored)
			return false;

		InvalidateGprQCacheForGuest(guest_reg);
		return true;
	}

	bool BlockCompiler::EmitStoreGprQ128(unsigned guest_reg, unsigned qreg, unsigned address_scratch)
	{
		if (guest_reg == 0)
			return true;

		const int pin_host = FindGprPinHost(guest_reg);
		if (pin_host >= 0)
		{
			// VMOV S->core can only name S0-S31; current EE qword GPR writers use
			// low NEON temporaries, and rejecting high qregs avoids a stale pin if
			// a future path tries to reuse this seam with q8+.
			if (qreg >= 8 ||
				!m_code.EmitVmovSToCore(static_cast<unsigned>(pin_host), qreg * 4))
			{
				return false;
			}
		}
		const int high_pin_host = FindGprPinHighHost(guest_reg);
		if (high_pin_host >= 0 &&
			(qreg >= 8 || !m_code.EmitVmovSToCore(static_cast<unsigned>(high_pin_host), qreg * 4 + 1)))
		{
			return false;
		}

		if (!EmitStoreCpuRegsQ128(GprOffset(guest_reg), qreg, address_scratch))
			return false;

		MarkGprQCache(guest_reg, qreg);
		return true;
	}

	bool BlockCompiler::EmitStoreGprQ128ToAddress(unsigned guest_reg, unsigned qreg, unsigned address_reg)
	{
		if (guest_reg == 0)
			return true;

		const int pin_host = FindGprPinHost(guest_reg);
		if (pin_host >= 0)
		{
			if (qreg >= 8 ||
				!m_code.EmitVmovSToCore(static_cast<unsigned>(pin_host), qreg * 4))
			{
				return false;
			}
		}
		const int high_pin_host = FindGprPinHighHost(guest_reg);
		if (high_pin_host >= 0 &&
			(qreg >= 8 || !m_code.EmitVmovSToCore(static_cast<unsigned>(high_pin_host), qreg * 4 + 1)))
		{
			return false;
		}

		if (!m_code.EmitVst1Q32Aligned(qreg, address_reg))
			return false;

		MarkGprQCache(guest_reg, qreg);
		return true;
	}

	bool BlockCompiler::EmitStoreGprDwordPair(unsigned guest_reg, unsigned low_d, unsigned high_d)
	{
		if (guest_reg == 0)
			return true;

		const int pin_host = FindGprPinHost(guest_reg);
		if (pin_host >= 0)
		{
			const unsigned low_s = low_d * 2;
			if (low_s >= 32 ||
				!m_code.EmitVmovSToCore(static_cast<unsigned>(pin_host), low_s))
			{
				return false;
			}
		}
		const int high_pin_host = FindGprPinHighHost(guest_reg);
		if (high_pin_host >= 0)
		{
			const unsigned high_s = low_d * 2 + 1;
			if (high_s >= 32 ||
				!m_code.EmitVmovSToCore(static_cast<unsigned>(high_pin_host), high_s))
			{
				return false;
			}
		}

		if (!m_code.EmitVstrDImm(low_d, HOST_CPU_REGS, static_cast<u16>(GprOffset(guest_reg))) ||
			!m_code.EmitVstrDImm(high_d, HOST_CPU_REGS,
				static_cast<u16>(GprOffset(guest_reg) + sizeof(u64))))
		{
			return false;
		}

		InvalidateGprQCacheForGuest(guest_reg);
		InvalidateGprQCacheForQreg(low_d / 2);
		InvalidateGprQCacheForQreg(high_d / 2);
		return true;
	}

	bool BlockCompiler::EmitStoreGprWord(unsigned guest_reg, unsigned word, unsigned host_reg)
	{
		if (guest_reg == 0)
			return true;

		if (word == 0)
		{
			const int pin_host = FindGprPinHost(guest_reg);
			if (pin_host >= 0 &&
				!m_code.EmitMovRegShiftImm(static_cast<unsigned>(pin_host), host_reg,
					VitaA32::ShiftType::LSL, 0))
			{
				return false;
			}
		}
		else if (word == 1)
		{
			const int high_pin_host = FindGprPinHighHost(guest_reg);
			if (high_pin_host >= 0 &&
				!m_code.EmitMovRegShiftImm(static_cast<unsigned>(high_pin_host), host_reg,
					VitaA32::ShiftType::LSL, 0))
			{
				return false;
			}
		}

		if (!m_code.EmitStrImm12(host_reg, HOST_CPU_REGS,
			static_cast<u16>(GprOffset(guest_reg) + word * sizeof(u32))))
		{
			return false;
		}

		InvalidateGprQCacheForGuest(guest_reg);
		return true;
	}

	bool BlockCompiler::EmitStoreGprLowPreserveHigh(unsigned guest_reg, unsigned host_low)
	{
		if (guest_reg == 0)
			return true;

		// PCSX2 VU0.cpp::CFC2(REG_R) updates only GPR.UL[0]. Keep the
		// write-through pin in sync while preserving the existing high word.
		const int pin_host = FindGprPinHost(guest_reg);
		if (pin_host >= 0 &&
			!m_code.EmitMovRegShiftImm(static_cast<unsigned>(pin_host), host_low,
				VitaA32::ShiftType::LSL, 0))
		{
			return false;
		}

		if (!m_code.EmitStrImm12(host_low, HOST_CPU_REGS, static_cast<u16>(GprOffset(guest_reg))))
			return false;

		InvalidateGprQCacheForGuest(guest_reg);
		return true;
	}

	bool BlockCompiler::EmitStoreGpr64(unsigned guest_reg, unsigned host_low, unsigned host_high)
	{
		if (guest_reg == 0)
			return true;

		// Write-through: memory stays authoritative and the pinned host copy is
		// refreshed with the same value on every path that reaches this store.
		const int pin_host = FindGprPinHost(guest_reg);
		if (pin_host >= 0 &&
			!m_code.EmitMovRegShiftImm(static_cast<unsigned>(pin_host), host_low,
				VitaA32::ShiftType::LSL, 0))
		{
			return false;
		}

		const int high_pin_host = FindGprPinHighHost(guest_reg);
		if (high_pin_host >= 0 &&
			!m_code.EmitMovRegShiftImm(static_cast<unsigned>(high_pin_host), host_high,
				VitaA32::ShiftType::LSL, 0))
		{
			return false;
		}

		const size_t offset = GprOffset(guest_reg);
		bool stored = false;
		if (offset <= 0xff && CanUseA32DualTransferPair(host_low, host_high))
			stored = m_code.EmitStrdImm8(host_low, host_high, HOST_CPU_REGS, static_cast<u8>(offset));
		else
			stored = m_code.EmitStrImm12(host_low, HOST_CPU_REGS, static_cast<u16>(offset)) &&
					 m_code.EmitStrImm12(host_high, HOST_CPU_REGS, static_cast<u16>(offset + sizeof(u32)));

		if (!stored)
			return false;

		InvalidateGprQCacheForGuest(guest_reg);
		return true;
	}
} // namespace VitaEE
