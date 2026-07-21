// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

// Vita VU1 micro-mode block provider.
//
// PCSX2 owners: VU1microInterp.cpp::_vu1Exec()/InterpVU1::Execute() define
// the step semantics reproduced here, VUops.cpp owns the stall/pipe/flush
// helpers the generated code calls, and VUmicroFast.h owns the per-kind op
// bodies reached through the kind-exec tables below. The x86 shape this
// stands in for is x86/microVU_Compile.inl's block compiler; the Vita port
// keeps interpreter-exact per-step behavior (the trace oracle runs the
// interpreter) while deleting the per-step fetch/decode/dispatch work:
// each upper/lower pair is a compile-time constant, so path selection,
// E/D/T decoding, hazard backup rules, stall-helper selection, branch
// countdown, and E-bit termination all specialize at compile time.

#include "Common.h"

#include "Config.h"
#include "Dmac.h"
#include "DebugTools/VuTrace.h"
#include "MTVU.h"
#include "VUmicro.h"
#include "VUmicroFast.h"
#include "Vif.h"

#include "vita/A32Emitter.h"
#include "vita/VitaFpRounding.h"
#include "vita/VitaPerformanceTelemetry.h"
#include "vita/VitaVuBlockCompiler.h"

#include "common/Vita/VitaJitMemory.h"

#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#if defined(VITASX2_QEMU_VALIDATION)
u32 g_qemuVuJitEbitFinishInlineOps = 0;
u32 g_qemuVuJitTestPipesFastSkips = 0;
u32 g_qemuVuJitTestPipesIaluFlushInlineOps = 0;
u32 g_qemuVuJitTestPipesFmacFlushInlineOps = 0;
u32 g_qemuVuJitResidentFmacQueueRetirements = 0;
u32 g_qemuVuJitResidentPipeAggregateRefreshes = 0;
u32 g_qemuVuJitResidentPipeAggregateXgkickCalls = 0;
u32 g_qemuVuJitTestPipesFdivFlushInlineOps = 0;
u32 g_qemuVuJitTestPipesEfuFlushInlineOps = 0;
u32 g_qemuVuJitTestPipesXgkickTransferInlineOps = 0;
u32 g_qemuVuJitNopPipeTestDeferrals = 0;
u32 g_qemuVuJitEmptyPipeNopBatchRuns = 0;
u32 g_qemuVuJitEmptyPipeNopBatchPairs = 0;
u32 g_qemuVuJitAllPipesEmptyFastSkips = 0;
u32 g_qemuVuJitNormConstantMaterializations = 0;
u32 g_qemuVuJitFmacClearInlineOps = 0;
u32 g_qemuVuJitFmacWriteposLoadElisions = 0;
u32 g_qemuVuJitResidentFmacCountAppendElisions = 0;
u32 g_qemuVuJitCanonicalFmacStallTestRuntimeElisions = 0;
u32 g_qemuVuJitUpperFmacStallTestInlineOps = 0;
u32 g_qemuVuJitLowerFmacStallTestInlineOps = 0;
u32 g_qemuVuJitUpperFmacStallInlineOps = 0;
u32 g_qemuVuJitLowerFmacStallInlineOps = 0;
u32 g_qemuVuJitUpperAddSubInlineOps = 0;
u32 g_qemuVuJitUpperMulInlineOps = 0;
u32 g_qemuVuJitUpperMaddMsubInlineOps = 0;
u32 g_qemuVuJitUpperOuterInlineOps = 0;
u32 g_qemuVuJitUpperNopInlineOps = 0;
u32 g_qemuVuJitUpperUnaryInlineOps = 0;
u32 g_qemuVuJitUpperClipInlineOps = 0;
u32 g_qemuVuJitUpperMinMaxInlineOps = 0;
u32 g_qemuVuJitLowerIaluInlineOps = 0;
u32 g_qemuVuJitLowerFlagInlineOps = 0;
u32 g_qemuVuJitLowerMoveInlineOps = 0;
u32 g_qemuVuJitLowerLsuInlineOps = 0;
u32 g_qemuVuJitLowerControlInlineOps = 0;
u32 g_qemuVuJitLowerBranchInlineOps = 0;
u32 g_qemuVuJitLowerFdivInlineOps = 0;
u32 g_qemuVuJitLowerEfuInlineOps = 0;
u32 g_qemuVuJitLowerXgkickInlineOps = 0;
u32 g_qemuVuJitLowerFdivStallTestInlineOps = 0;
u32 g_qemuVuJitLowerEfuStallTestInlineOps = 0;
u32 g_qemuVuJitLowerBranchStallTestInlineOps = 0;
u32 g_qemuVuJitLowerStallInlineOps = 0;
u32 g_qemuVuJitDtFlagInlineOps = 0;
u32 g_qemuVuJitNormalizedOperandQuadBypasses = 0;
u32 g_qemuVu0JitNormalizedOperandQuadBypasses = 0;
u32 g_qemuVuJitLinkedFrameEntries = 0;
u32 g_qemuVuJitLinkedVectorFrameEntries = 0;
u32 g_qemuVuJitResidentPipeLinkedEntries = 0;
u32 g_qemuVuJitLocalFmacPipelineEntries = 0;
u32 g_qemuVuJitLocalFmacPipelineCommits = 0;
u32 g_qemuVuJitLocalFmacCycleSnapshotElisions = 0;
u32 g_qemuVuJitLocalFmacProducerSnapshotEntries = 0;
u32 g_qemuVuJitDeferredFmacFlagEntries = 0;
u32 g_qemuVuJitDeferredFmacFlagRetirements = 0;
u32 g_qemuVuJitCanonicalDeferredFmacRetirements = 0;
u32 g_qemuVuJitDeferredFmacCompactRetirements = 0;
u32 g_qemuVuJitDeferredFmacCompactInstructionsRemoved = 0;
u32 g_qemuVuJitDeferredFmacLinkedEntries = 0;
bool g_qemuVuJitForceInterpreterFallback = false;
#endif

namespace VitaVU
{
	using VitaA32::CodeBuffer;
	using VitaA32::Condition;
	using VitaA32::ShiftType;

	namespace
	{
		// ------------------------------------------------------------------
		// Static layout facts used by generated pipe/stall code.
		// ------------------------------------------------------------------
		constexpr size_t FDIV_ENABLE_OFFSET = offsetof(VURegs, fdiv) + offsetof(fdivPipe, enable);
		constexpr size_t EFU_ENABLE_OFFSET = offsetof(VURegs, efu) + offsetof(efuPipe, enable);
		constexpr size_t VI_BACKUP_CYCLES_OFFSET = offsetof(VURegs, VIBackupCycles);
		constexpr size_t VI_OLD_VALUE_OFFSET = offsetof(VURegs, VIOldValue);
		constexpr size_t VI_REG_NUMBER_OFFSET = offsetof(VURegs, VIRegNumber);
		constexpr u32 FPU_FLOAT_SIGN_MASK = 0x80000000u;
		constexpr u32 FPU_FLOAT_EXPONENT_MASK = 0x7f800000u;
		constexpr u32 FPU_FLOAT_MANTISSA_MASK = 0x007fffffu;
		constexpr u32 FPU_FLOAT_MAX_FINITE = 0x7f7fffffu;

		bool Vu1ProgramActive()
		{
			return THREAD_VU1 ? vu1Thread.IsProgramActive() :
				(VU0.VI[REG_VPU_STAT].UL & 0x100) != 0;
		}

		void Vu1MtvuMarkDBitEnd()
		{
			// PCSX2 owner: x86/microVU_Branch.inl::mVUDTendProgram()
			// publishes both enabled D and T exits through mVUTBit().
			vu1Thread.MarkDtProgramEnd(VU_Thread::InterruptFlagVUTBit);
		}

		void Vu1MtvuMarkTBitEnd()
		{
			vu1Thread.MarkDtProgramEnd(VU_Thread::InterruptFlagVUTBit);
		}

		void Vu1MtvuFinishDtProgram()
		{
			vu1Thread.EndProgram(0);
		}

		void Vu1MtvuFinishEbitProgram()
		{
			vu1Thread.EndProgram(VU_Thread::InterruptFlagVUEBit);
		}
		static_assert(sizeof(ialuPipe) == 24);
		static_assert(offsetof(ialuPipe, reg) == 0);
		static_assert(offsetof(ialuPipe, sCycle) == 8);
		static_assert(offsetof(ialuPipe, Cycle) == 16);
		static_assert(sizeof(fmacPipe) == 48);
		static_assert(offsetof(fmacPipe, regupper) == 0);
		static_assert(offsetof(fmacPipe, reglower) == 4);
		static_assert(offsetof(fmacPipe, flagreg) == 8);
		static_assert(offsetof(fmacPipe, xyzwupper) == 12);
		static_assert(offsetof(fmacPipe, xyzwlower) == 16);
		static_assert(offsetof(fmacPipe, sCycle) == 24);
		static_assert(offsetof(fmacPipe, Cycle) == 32);
		static_assert(offsetof(fmacPipe, macflag) == 36);
		static_assert(offsetof(fmacPipe, statusflag) == 40);
		static_assert(offsetof(fmacPipe, clipflag) == 44);
		static_assert(offsetof(VURegs, statusflag) == offsetof(VURegs, macflag) + 4);
		static_assert(offsetof(VURegs, clipflag) == offsetof(VURegs, macflag) + 8);
		static_assert(offsetof(fdivPipe, enable) == 0);
		static_assert(offsetof(fdivPipe, reg) == 4);
		static_assert(offsetof(fdivPipe, sCycle) == 24);
		static_assert(offsetof(fdivPipe, Cycle) == 32);
		static_assert(offsetof(fdivPipe, statusflag) == 36);
		static_assert(offsetof(efuPipe, enable) == 0);
		static_assert(offsetof(efuPipe, reg) == 4);
		static_assert(offsetof(efuPipe, sCycle) == 24);
		static_assert(offsetof(efuPipe, Cycle) == 32);

		// ------------------------------------------------------------------
		// Static pair analysis.
		// ------------------------------------------------------------------

		// PCSX2 owners: VU1microInterp.cpp::_vu1IsUpperNop()/_vu1IsLowerNop().
		constexpr bool IsUpperNop(u32 upper) { return (upper & 0x07ffffffu) == 0x000002ffu; }
		constexpr bool IsLowerNop(u32 lower) { return lower == 0x8000033cu; }

		constexpr bool IsInlineUpperNopKind(VUInterpFast::UpperFastKind kind)
		{
			return kind == VUInterpFast::UpperFastKind::NOP;
		}

		constexpr bool IsInlineUpperUnaryKind(VUInterpFast::UpperFastKind kind)
		{
			switch (kind)
			{
				case VUInterpFast::UpperFastKind::ABS:
				case VUInterpFast::UpperFastKind::FTOI0:
				case VUInterpFast::UpperFastKind::FTOI4:
				case VUInterpFast::UpperFastKind::FTOI12:
				case VUInterpFast::UpperFastKind::FTOI15:
				case VUInterpFast::UpperFastKind::ITOF0:
				case VUInterpFast::UpperFastKind::ITOF4:
				case VUInterpFast::UpperFastKind::ITOF12:
				case VUInterpFast::UpperFastKind::ITOF15:
					return true;
				default:
					return false;
			}
		}

		constexpr bool IsInlineUpperClipKind(VUInterpFast::UpperFastKind kind)
		{
			return kind == VUInterpFast::UpperFastKind::CLIP;
		}

		constexpr bool IsInlineUpperMinMaxKind(VUInterpFast::UpperFastKind kind)
		{
			switch (kind)
			{
				case VUInterpFast::UpperFastKind::MAX:
				case VUInterpFast::UpperFastKind::MAXi:
				case VUInterpFast::UpperFastKind::MAXx:
				case VUInterpFast::UpperFastKind::MAXy:
				case VUInterpFast::UpperFastKind::MAXz:
				case VUInterpFast::UpperFastKind::MAXw:
				case VUInterpFast::UpperFastKind::MINI:
				case VUInterpFast::UpperFastKind::MINIi:
				case VUInterpFast::UpperFastKind::MINIx:
				case VUInterpFast::UpperFastKind::MINIy:
				case VUInterpFast::UpperFastKind::MINIz:
				case VUInterpFast::UpperFastKind::MINIw:
					return true;
				default:
					return false;
			}
		}

		constexpr bool IsInlineUpperMulKind(VUInterpFast::UpperFastKind kind)
		{
			switch (kind)
			{
				case VUInterpFast::UpperFastKind::MUL:
				case VUInterpFast::UpperFastKind::MULi:
				case VUInterpFast::UpperFastKind::MULq:
				case VUInterpFast::UpperFastKind::MULx:
				case VUInterpFast::UpperFastKind::MULy:
				case VUInterpFast::UpperFastKind::MULz:
				case VUInterpFast::UpperFastKind::MULw:
				case VUInterpFast::UpperFastKind::MULA:
				case VUInterpFast::UpperFastKind::MULAi:
				case VUInterpFast::UpperFastKind::MULAq:
				case VUInterpFast::UpperFastKind::MULAx:
				case VUInterpFast::UpperFastKind::MULAy:
				case VUInterpFast::UpperFastKind::MULAz:
				case VUInterpFast::UpperFastKind::MULAw:
					return true;
				default:
					return false;
			}
		}

		constexpr bool IsInlineUpperMaddMsubKind(VUInterpFast::UpperFastKind kind)
		{
			switch (kind)
			{
				case VUInterpFast::UpperFastKind::MADD:
				case VUInterpFast::UpperFastKind::MADDi:
				case VUInterpFast::UpperFastKind::MADDq:
				case VUInterpFast::UpperFastKind::MADDx:
				case VUInterpFast::UpperFastKind::MADDy:
				case VUInterpFast::UpperFastKind::MADDz:
				case VUInterpFast::UpperFastKind::MADDw:
				case VUInterpFast::UpperFastKind::MADDA:
				case VUInterpFast::UpperFastKind::MADDAi:
				case VUInterpFast::UpperFastKind::MADDAq:
				case VUInterpFast::UpperFastKind::MADDAx:
				case VUInterpFast::UpperFastKind::MADDAy:
				case VUInterpFast::UpperFastKind::MADDAz:
				case VUInterpFast::UpperFastKind::MADDAw:
				case VUInterpFast::UpperFastKind::MSUB:
				case VUInterpFast::UpperFastKind::MSUBi:
				case VUInterpFast::UpperFastKind::MSUBq:
				case VUInterpFast::UpperFastKind::MSUBx:
				case VUInterpFast::UpperFastKind::MSUBy:
				case VUInterpFast::UpperFastKind::MSUBz:
				case VUInterpFast::UpperFastKind::MSUBw:
				case VUInterpFast::UpperFastKind::MSUBA:
				case VUInterpFast::UpperFastKind::MSUBAi:
				case VUInterpFast::UpperFastKind::MSUBAq:
				case VUInterpFast::UpperFastKind::MSUBAx:
				case VUInterpFast::UpperFastKind::MSUBAy:
				case VUInterpFast::UpperFastKind::MSUBAz:
				case VUInterpFast::UpperFastKind::MSUBAw:
					return true;
				default:
					return false;
			}
		}

		constexpr bool IsInlineUpperOuterKind(VUInterpFast::UpperFastKind kind)
		{
			return kind == VUInterpFast::UpperFastKind::OPMULA ||
				kind == VUInterpFast::UpperFastKind::OPMSUB;
		}

		constexpr bool IsUpperMaddMsubVfBroadcastKind(VUInterpFast::UpperFastKind kind)
		{
			switch (kind)
			{
				case VUInterpFast::UpperFastKind::MADDx:
				case VUInterpFast::UpperFastKind::MADDAx:
				case VUInterpFast::UpperFastKind::MSUBx:
				case VUInterpFast::UpperFastKind::MSUBAx:
				case VUInterpFast::UpperFastKind::MADDy:
				case VUInterpFast::UpperFastKind::MADDAy:
				case VUInterpFast::UpperFastKind::MSUBy:
				case VUInterpFast::UpperFastKind::MSUBAy:
				case VUInterpFast::UpperFastKind::MADDz:
				case VUInterpFast::UpperFastKind::MADDAz:
				case VUInterpFast::UpperFastKind::MSUBz:
				case VUInterpFast::UpperFastKind::MSUBAz:
				case VUInterpFast::UpperFastKind::MADDw:
				case VUInterpFast::UpperFastKind::MADDAw:
				case VUInterpFast::UpperFastKind::MSUBw:
				case VUInterpFast::UpperFastKind::MSUBAw:
					return true;
				default:
					return false;
			}
		}

		constexpr bool IsInlineUpperAddSubKind(VUInterpFast::UpperFastKind kind)
		{
			switch (kind)
			{
				case VUInterpFast::UpperFastKind::ADD:
				case VUInterpFast::UpperFastKind::ADDi:
				case VUInterpFast::UpperFastKind::ADDq:
				case VUInterpFast::UpperFastKind::ADDx:
				case VUInterpFast::UpperFastKind::ADDy:
				case VUInterpFast::UpperFastKind::ADDz:
				case VUInterpFast::UpperFastKind::ADDw:
				case VUInterpFast::UpperFastKind::ADDA:
				case VUInterpFast::UpperFastKind::ADDAi:
				case VUInterpFast::UpperFastKind::ADDAq:
				case VUInterpFast::UpperFastKind::ADDAx:
				case VUInterpFast::UpperFastKind::ADDAy:
				case VUInterpFast::UpperFastKind::ADDAz:
				case VUInterpFast::UpperFastKind::ADDAw:
				case VUInterpFast::UpperFastKind::SUB:
				case VUInterpFast::UpperFastKind::SUBi:
				case VUInterpFast::UpperFastKind::SUBq:
				case VUInterpFast::UpperFastKind::SUBx:
				case VUInterpFast::UpperFastKind::SUBy:
				case VUInterpFast::UpperFastKind::SUBz:
				case VUInterpFast::UpperFastKind::SUBw:
				case VUInterpFast::UpperFastKind::SUBA:
				case VUInterpFast::UpperFastKind::SUBAi:
				case VUInterpFast::UpperFastKind::SUBAq:
				case VUInterpFast::UpperFastKind::SUBAx:
				case VUInterpFast::UpperFastKind::SUBAy:
				case VUInterpFast::UpperFastKind::SUBAz:
				case VUInterpFast::UpperFastKind::SUBAw:
					return true;
				default:
					return false;
			}
		}

		constexpr bool IsInlineLowerIaluKind(VUInterpFast::LowerFastKind kind)
		{
			switch (kind)
			{
				case VUInterpFast::LowerFastKind::IADDIU:
				case VUInterpFast::LowerFastKind::ISUBIU:
				case VUInterpFast::LowerFastKind::IADD:
				case VUInterpFast::LowerFastKind::ISUB:
				case VUInterpFast::LowerFastKind::IADDI:
				case VUInterpFast::LowerFastKind::IAND:
				case VUInterpFast::LowerFastKind::IOR:
					return true;
				default:
					return false;
			}
		}

		constexpr bool IsInlineLowerFlagKind(VUInterpFast::LowerFastKind kind)
		{
			switch (kind)
			{
				case VUInterpFast::LowerFastKind::FCAND:
				case VUInterpFast::LowerFastKind::FCSET:
				case VUInterpFast::LowerFastKind::FCEQ:
				case VUInterpFast::LowerFastKind::FCOR:
				case VUInterpFast::LowerFastKind::FSEQ:
				case VUInterpFast::LowerFastKind::FSSET:
				case VUInterpFast::LowerFastKind::FSAND:
				case VUInterpFast::LowerFastKind::FSOR:
				case VUInterpFast::LowerFastKind::FMEQ:
				case VUInterpFast::LowerFastKind::FMAND:
				case VUInterpFast::LowerFastKind::FMOR:
				case VUInterpFast::LowerFastKind::FCGET:
					return true;
				default:
					return false;
			}
		}

		constexpr bool IsInlineLowerMoveKind(VUInterpFast::LowerFastKind kind)
		{
			switch (kind)
			{
				case VUInterpFast::LowerFastKind::MOVE:
				case VUInterpFast::LowerFastKind::MR32:
				case VUInterpFast::LowerFastKind::MFIR:
				case VUInterpFast::LowerFastKind::MTIR:
					return true;
				default:
					return false;
			}
		}

		constexpr bool IsInlineLowerLsuKind(VUInterpFast::LowerFastKind kind)
		{
			switch (kind)
			{
				case VUInterpFast::LowerFastKind::LQ:
				case VUInterpFast::LowerFastKind::SQ:
				case VUInterpFast::LowerFastKind::ILW:
				case VUInterpFast::LowerFastKind::ISW:
				case VUInterpFast::LowerFastKind::LQI:
				case VUInterpFast::LowerFastKind::LQD:
				case VUInterpFast::LowerFastKind::SQI:
				case VUInterpFast::LowerFastKind::SQD:
				case VUInterpFast::LowerFastKind::ILWR:
				case VUInterpFast::LowerFastKind::ISWR:
					return true;
				default:
					return false;
			}
		}

		constexpr bool IsInlineLowerControlKind(VUInterpFast::LowerFastKind kind)
		{
			switch (kind)
			{
				case VUInterpFast::LowerFastKind::RINIT:
				case VUInterpFast::LowerFastKind::RGET:
				case VUInterpFast::LowerFastKind::RNEXT:
				case VUInterpFast::LowerFastKind::RXOR:
				case VUInterpFast::LowerFastKind::MFP:
				case VUInterpFast::LowerFastKind::WAITQ:
				case VUInterpFast::LowerFastKind::WAITP:
				case VUInterpFast::LowerFastKind::XITOP:
				case VUInterpFast::LowerFastKind::XTOP:
					return true;
				default:
					return false;
			}
		}

		constexpr bool IsInlineLowerBranchKind(VUInterpFast::LowerFastKind kind)
		{
			switch (kind)
			{
				case VUInterpFast::LowerFastKind::IBEQ:
				case VUInterpFast::LowerFastKind::IBNE:
				case VUInterpFast::LowerFastKind::IBLTZ:
				case VUInterpFast::LowerFastKind::IBGTZ:
				case VUInterpFast::LowerFastKind::IBLEZ:
				case VUInterpFast::LowerFastKind::IBGEZ:
				case VUInterpFast::LowerFastKind::B:
				case VUInterpFast::LowerFastKind::BAL:
				case VUInterpFast::LowerFastKind::JR:
				case VUInterpFast::LowerFastKind::JALR:
					return true;
				default:
					return false;
			}
		}

		constexpr bool IsInlineLowerFdivKind(VUInterpFast::LowerFastKind kind)
		{
			switch (kind)
			{
				case VUInterpFast::LowerFastKind::DIV:
				case VUInterpFast::LowerFastKind::SQRT:
				case VUInterpFast::LowerFastKind::RSQRT:
					return true;
				default:
					return false;
			}
		}

		constexpr bool IsInlineLowerEfuKind(VUInterpFast::LowerFastKind kind)
		{
			switch (kind)
			{
				case VUInterpFast::LowerFastKind::ESADD:
				case VUInterpFast::LowerFastKind::ERSADD:
				case VUInterpFast::LowerFastKind::ELENG:
				case VUInterpFast::LowerFastKind::ERLENG:
				case VUInterpFast::LowerFastKind::EATANxy:
				case VUInterpFast::LowerFastKind::EATANxz:
				case VUInterpFast::LowerFastKind::ESUM:
				case VUInterpFast::LowerFastKind::ERCPR:
				case VUInterpFast::LowerFastKind::ESQRT:
				case VUInterpFast::LowerFastKind::ERSQRT:
				case VUInterpFast::LowerFastKind::ESIN:
				case VUInterpFast::LowerFastKind::EATAN:
				case VUInterpFast::LowerFastKind::EEXP:
					return true;
				default:
					return false;
			}
		}

		constexpr bool IsInlineLowerXgkickKind(VUInterpFast::LowerFastKind kind)
		{
			return kind == VUInterpFast::LowerFastKind::XGKICK;
		}

		constexpr bool IsInlineUpperKind(VUInterpFast::UpperFastKind kind)
		{
			return IsInlineUpperNopKind(kind) ||
				IsInlineUpperUnaryKind(kind) ||
				IsInlineUpperClipKind(kind) ||
				IsInlineUpperMinMaxKind(kind) ||
				IsInlineUpperAddSubKind(kind) ||
				IsInlineUpperMulKind(kind) ||
				IsInlineUpperMaddMsubKind(kind) ||
				IsInlineUpperOuterKind(kind);
		}

		constexpr bool IsInlineLowerKind(VUInterpFast::LowerFastKind kind)
		{
			return IsInlineLowerIaluKind(kind) ||
				IsInlineLowerFlagKind(kind) ||
				IsInlineLowerMoveKind(kind) ||
				IsInlineLowerLsuKind(kind) ||
				IsInlineLowerControlKind(kind) ||
				IsInlineLowerBranchKind(kind) ||
				IsInlineLowerFdivKind(kind) ||
				IsInlineLowerEfuKind(kind) ||
				IsInlineLowerXgkickKind(kind);
		}

		template <size_t... I>
		constexpr bool AllUpperFastKindsInline(std::index_sequence<I...>)
		{
			return (... && (I == 0 || IsInlineUpperKind(static_cast<VUInterpFast::UpperFastKind>(I))));
		}

		template <size_t... I>
		constexpr bool AllLowerFastKindsInline(std::index_sequence<I...>)
		{
			return (... && (I == 0 || IsInlineLowerKind(static_cast<VUInterpFast::LowerFastKind>(I))));
		}

		static_assert(AllUpperFastKindsInline(std::make_index_sequence<VUInterpFast::UpperFastKindCount>{}));
		static_assert(AllLowerFastKindsInline(std::make_index_sequence<VUInterpFast::LowerFastKindCount>{}));

		constexpr u8 LastIlwHalfwordIndex(unsigned mask)
		{
			constexpr u8 table[16] = {
				0, 6, 4, 6,
				2, 6, 4, 6,
				0, 6, 4, 6,
				2, 6, 4, 6,
			};
			return table[mask & 0x0f];
		}

		constexpr u32 UPPER_I_BIT = 0x80000000u;
		constexpr u32 UPPER_E_BIT = 0x40000000u;
		constexpr u32 UPPER_M_BIT = 0x20000000u;
		constexpr u32 UPPER_D_BIT = 0x10000000u;
		constexpr u32 UPPER_T_BIT = 0x08000000u;

		// The four control shapes of VU1microInterp.cpp::_vu1Exec(), selected
		// at compile time from the constant opcode pair.
		enum class PairShape : u8
		{
			UpperNop, // !(I) && upper NOP: lower-only step, no upper stall work
			IBit,     // I flag: upper exec, then REG_I = lower word
			LowerNop, // lower 0x8000033c: upper exec only
			Paired,   // full upper+lower step with hazard rules
		};

		struct PairPlan
		{
			u32 pc = 0;
			u32 upper = 0;
			u32 lower = 0;
			PairShape shape = PairShape::Paired;
			u8 upper_kind = 0;
			u8 lower_kind = 0;
			bool exec_upper = false;
			bool exec_lower = false;
			bool ebit = false;
			bool mflag = false;
			bool dflag = false;
			bool tflag = false;
			// Hazard rules from _vu1Exec()'s paired path: back up VF/clip when
			// the upper result must stay invisible to the lower op, or discard
			// the lower op entirely on same-target writes.
			u8 vf_backup_reg = 0;
			bool vi_clip_backup = false;
			bool discard_lower = false;
			// Statically selected stall bookkeeping (VUops.cpp switch arms
			// that are provably no-ops for this pair are not emitted).
			bool test_upper_stalls = false;
			bool test_lower_stalls = false;
			bool add_upper_stalls = false;
			bool add_lower_stalls = false;
			bool fmac_pipe = false;
			bool local_fmac_cycle_snapshot = true;
			bool test_pipes_fast_guard = false;
			bool defer_nop_pipe_test = false;
			bool upper_fmac_stall_test_inline = false;
			bool lower_fmac_stall_test_inline = false;
			bool upper_unary_inline = false;
			bool upper_clip_inline = false;
			bool upper_minmax_inline = false;
			bool upper_addsub_inline = false;
			bool upper_mul_inline = false;
			bool upper_maddmsub_inline = false;
			bool upper_outer_inline = false;
			bool upper_nop_inline = false;
			bool lower_ialu_inline = false;
			bool lower_flag_inline = false;
			bool lower_move_inline = false;
			bool lower_lsu_inline = false;
			bool lower_control_inline = false;
			bool lower_branch_inline = false;
			bool lower_fdiv_inline = false;
			bool lower_efu_inline = false;
			bool lower_xgkick_inline = false;
			bool lower_fdiv_stall_test_inline = false;
			bool lower_efu_stall_test_inline = false;
			bool lower_branch_stall_test_inline = false;
			bool lower_stall_inline = false;
			// PCSX2 owner: microVU_Analyze.inl::flagSet() and
			// microVU_Upper.inl::mVUupdateFlags(). STATUS remains live for every
			// FMAC producer because its result contributes to delayed sticky state.
			// MAC classification is independently live only when a later MAC reader,
			// preserve-inactive producer, or block-seam pipeline state can observe it.
			bool mac_flag_result_required = true;
			// PCSX2 microVU owner: microVU_IR.h::microRegInfo::backupVI.
			// True only when this exact lower opcode calls VUops.cpp::_vuBackupVI().
			bool vi_backup_write = false;
			// Tail work windows.
			bool branch_tail = false;
			bool resolves_branch = false;
			bool ebit_tail = false;
			u32 ebit_store = 0; // valid when ebit_tail: statically-known post-decrement value
			bool ends_block = false;
			_VURegsNum uregs = {};
			_VURegsNum lregs = {};
		};

		constexpr u32 MAX_BLOCK_PAIRS = 64;
		constexpr u32 MAX_DIRECT_LINK_SLOTS = 2;
		// Sony VU User Manual 3.4.4: the FMAC pipeline has a fixed four-cycle
		// latency. PCSX2 owners: microVU_Analyze.inl's compile-time pipeline
		// state and VUops.cpp::_vuTestFMACStalls(). The first four pairs retain
		// the canonical entry queue; later writers use compiler-owned slots.
		constexpr u32 FMAC_PIPELINE_LATENCY_CYCLES = 4;
		constexpr u32 LOCAL_FMAC_WARMUP_PAIRS = FMAC_PIPELINE_LATENCY_CYCLES;
		constexpr u32 LOCAL_FMAC_CANONICAL_STALL_MATURE_PAIR =
			LOCAL_FMAC_WARMUP_PAIRS + FMAC_PIPELINE_LATENCY_CYCLES - 1;

			struct DirectLinkPlan
			{
				bool valid = false;
				bool runtime_observed = false;
				bool guard_tpc = false;
				u32 guard_tpc_value = 0;
				u32 target_pc = 0;
				bool target_branch_tail = false;
				bool target_ebit_tail = false;
		};

		struct BlockPlan
		{
			u32 start_pc = 0;
			u32 pair_count = 0;
			bool entry_branch_tail = false;
			bool entry_ebit_tail = false;
			// True when this A32 fragment stopped before PCSX2 microVU's natural
			// block boundary (currently the 64-pair emitter cap, a decode fallback,
			// or a D/T pair whose runtime FBRST condition did not stop the VU).
			bool continues_logical_block_if_busy = false;
			u32 test_pipes_fast_guard_pairs = 0;
			u32 nop_pipe_test_defer_pairs = 0;
			u32 empty_pipe_nop_batch_runs = 0;
			u32 empty_pipe_nop_batch_pairs = 0;
			u32 fmac_clear_inline_pairs = 0;
			u32 upper_fmac_stall_test_inline_pairs = 0;
			u32 lower_fmac_stall_test_inline_pairs = 0;
			u32 upper_fmac_stall_inline_pairs = 0;
			u32 lower_fmac_stall_inline_pairs = 0;
			u32 upper_addsub_inline_pairs = 0;
			u32 upper_mul_inline_pairs = 0;
			u32 upper_maddmsub_inline_pairs = 0;
			u32 upper_outer_inline_pairs = 0;
			u32 upper_nop_inline_pairs = 0;
			u32 upper_unary_inline_pairs = 0;
			u32 upper_clip_inline_pairs = 0;
			u32 upper_minmax_inline_pairs = 0;
			u32 lower_ialu_inline_pairs = 0;
			u32 lower_flag_inline_pairs = 0;
			u32 lower_move_inline_pairs = 0;
			u32 lower_lsu_inline_pairs = 0;
			u32 lower_control_inline_pairs = 0;
			u32 lower_branch_inline_pairs = 0;
			u32 lower_fdiv_inline_pairs = 0;
			u32 lower_efu_inline_pairs = 0;
			u32 lower_xgkick_inline_pairs = 0;
			u32 lower_fdiv_stall_test_inline_pairs = 0;
			u32 lower_efu_stall_test_inline_pairs = 0;
			u32 lower_branch_stall_test_inline_pairs = 0;
			u32 lower_stall_inline_pairs = 0;
			u32 vi_backup_update_elided_pairs = 0;
			u32 vi_backup_zero_store_pairs = 0;
			u32 dt_flag_inline_pairs = 0;
			bool resident_cycle = false;
			// Local-FMAC blocks can retain the complete 64-bit VU cycle in an
			// A32 register pair. r9 is unavailable only when paired clip backup
			// needs it across the upper/lower execution window.
			bool resident_cycle_high = false;
			// PCSX2 microVU owner: microVU_IR.h::microRegInfo plus
			// microVU_Compile.inl::mVUincCycles()/mVUsetCycles(). Long VU1
			// blocks with no architectural flag observer can keep the fixed
			// four-cycle FMAC window in compile-time-owned local slots after a
			// four-pair canonical warm-up. The emitter republishes the exact
			// interpreter fmacPipe state at the block seam.
			bool local_fmac_pipeline = false;
			u32 local_fmac_pipeline_pairs = 0;
			u32 canonical_fmac_stall_tests_elided = 0;
			u32 local_fmac_cycle_snapshot_elision_pairs = 0;
			u32 local_fmac_producer_snapshot_pairs = 0;
			u32 local_fmac_clip_snapshot_elisions = 0;
			u32 mac_flag_classification_elisions = 0;
			// PCSX2 microVU owner: microVU_Analyze.inl's mVUregs pipeline
			// state. Profitable multi-pair VU1 blocks retain the coarse
			// canonical-pipe activity predicate in r10 instead of rebuilding it
			// from five VURegs words before every pair.
			bool resident_pipe_activity = false;
			// PCSX2 owner: microVU_Flags.inl::mVUsetFlags() retains flag
			// instances in compiler state until an observer or block seam. Keep
			// STATUS and MAC in callee-saved A32 registers from block entry and
			// publish them once at the seam; canonical queue flushes update the
			// same private instances.
			bool deferred_fmac_flags = false;
			u32 deferred_fmac_flag_retirements = 0;
			u32 deferred_fmac_compact_retirements = 0;
			u32 deferred_fmac_compact_instructions_removed = 0;
			// PCSX2 microVU owner: microVU_Upper.inl::mVUupdateFlags() keeps
			// the current working MAC/STATUS instances in allocator registers.
			// When every local producer is isolated from a flag/FDIV/helper seam,
			// keep Vita's identical working snapshots in the private frame and
			// publish VURegs::macflag/statusflag once at the block seam.
			bool resident_working_fmac_flags = false;
			bool direct_link_tail = false;
			std::array<DirectLinkPlan, MAX_DIRECT_LINK_SLOTS> direct_links{};
			std::array<PairPlan, MAX_BLOCK_PAIRS> pairs{};
		};

		bool CanUseLocalFmacPipeline(const BlockPlan& block)
		{
			constexpr u32 MIN_LOCAL_COMMITS = 6;
			if (block.pair_count < LOCAL_FMAC_WARMUP_PAIRS + MIN_LOCAL_COMMITS)
				return false;

			const u32 flag_mask = (1u << REG_STATUS_FLAG) |
				(1u << REG_MAC_FLAG) | (1u << REG_CLIP_FLAG);
			u32 fmac_pairs = 0;
			for (u32 i = LOCAL_FMAC_WARMUP_PAIRS; i < block.pair_count; i++)
			{
				const PairPlan& pair = block.pairs[i];
				// E/D/T completion and PATH1 calls are externally observable seams
				// and retain the canonical interpreter queue. A flag instruction or
				// flag-register read is compatible only when every older compiler-owned
				// snapshot has reached PCSX2's fixed four-cycle FMAC visibility point.
				// EmitRetireLocalFmacEntries() publishes those snapshots before this
				// pair executes; an FMAC produced by the observer pair itself is queued
				// later in the pair tail and therefore remains eligible.
				if (pair.ebit || pair.dflag || pair.tflag || pair.lower_xgkick_inline)
				{
					return false;
				}
				const bool observes_flags = pair.lower_flag_inline ||
					((pair.uregs.VIread | pair.lregs.VIread) & flag_mask) != 0;
				if (observes_flags)
				{
					for (u32 writer = LOCAL_FMAC_WARMUP_PAIRS; writer < i; writer++)
					{
						if (block.pairs[writer].fmac_pipe &&
							writer + FMAC_PIPELINE_LATENCY_CYCLES > i)
							return false;
					}
				}
				fmac_pairs += pair.fmac_pipe ? 1u : 0u;
			}

			return fmac_pairs >= MIN_LOCAL_COMMITS;
		}

		bool CanDeferFmacFlags(const BlockPlan& block)
		{
			const u32 deferred_flag_mask = (1u << REG_STATUS_FLAG) |
				(1u << REG_MAC_FLAG);
			for (u32 i = 0; i < block.pair_count; i++)
			{
				const PairPlan& pair = block.pairs[i];
				if (pair.ebit || pair.dflag || pair.tflag || pair.lower_xgkick_inline)
				{
					return false;
				}

				const u32 deferred_flag_reads =
					(pair.uregs.VIread | pair.lregs.VIread) & deferred_flag_mask;
				if (deferred_flag_reads != 0 && !pair.lower_flag_inline)
					return false;

				if (pair.lower_flag_inline &&
					static_cast<VUInterpFast::LowerFastKind>(pair.lower_kind) ==
						VUInterpFast::LowerFastKind::FSSET)
				{
					// FSSET changes the working STATUS instance sampled by a paired
					// FMAC producer. Keep that block on canonical working/deferred
					// state until this mutation is represented in the resident path.
					return false;
				}
			}
			return true;
		}

		bool CanSnapshotLocalFmacFlagsAtProducer(const PairPlan& pair)
		{
			// PCSX2 owner: x86/microVU_Upper.inl::mVUupdateFlags() writes a
			// rotating MAC/STATUS instance directly from the FMAC result. These
			// Vita upper paths end with the equivalent MAC/STATUS values in core
			// registers. A lower FDIV or FSSET can still change VURegs::statusflag
			// before VUops.cpp::_vuAddFMACStalls() snapshots the pair, so retain the
			// canonical post-lower loads for those mixed pairs.
			const bool produces_mac_status = pair.add_upper_stalls &&
				(pair.upper_addsub_inline || pair.upper_mul_inline ||
				 pair.upper_maddmsub_inline || pair.upper_outer_inline);
			return produces_mac_status && !pair.lower_fdiv_inline && !pair.lower_flag_inline;
		}

		bool FmacWriterConflicts(const PairPlan& writer, const _VURegsNum& reader)
		{
			const u32 upper_reg = writer.add_upper_stalls ? writer.uregs.VFwrite : 0;
			const u32 upper_mask = writer.add_upper_stalls ? writer.uregs.VFwxyzw : 0;
			const bool lower_fmac = writer.add_lower_stalls &&
				writer.lregs.pipe == VUPIPE_FMAC;
			const u32 lower_reg = lower_fmac ? writer.lregs.VFwrite : 0;
			const u32 lower_mask = lower_fmac ? writer.lregs.VFwxyzw : 0;
			const auto overlaps = [](u32 write_reg, u32 write_mask,
				u32 read_reg, u32 read_mask) {
				return write_reg != 0 && write_reg == read_reg &&
					(write_mask & read_mask) != 0;
			};
			return overlaps(upper_reg, upper_mask, reader.VFread0,
					reader.VFr0xyzw) ||
				overlaps(upper_reg, upper_mask, reader.VFread1,
					reader.VFr1xyzw) ||
				overlaps(lower_reg, lower_mask, reader.VFread0,
					reader.VFr0xyzw) ||
				overlaps(lower_reg, lower_mask, reader.VFread1,
					reader.VFr1xyzw);
		}

		bool MergeFmacStallReadSets(const _VURegsNum& first,
			const _VURegsNum& second, _VURegsNum* merged)
		{
			// PCSX2 owner: VUops.cpp::_vuTestFMACStalls() applies each VF source
			// independently, but every matching queue entry contributes only
			// cycle = max(cycle, sCycle + 4). Union equal-register lane masks and
			// admit only unions representable by the existing two-source scan.
			*merged = {};
			const auto add_read = [&](u8 reg, u8 lanes) {
				if (reg == 0 || lanes == 0)
					return true;
				if (merged->VFread0 == reg)
				{
					merged->VFr0xyzw |= lanes;
					return true;
				}
				if (merged->VFread1 == reg)
				{
					merged->VFr1xyzw |= lanes;
					return true;
				}
				if (merged->VFread0 == 0)
				{
					merged->VFread0 = reg;
					merged->VFr0xyzw = lanes;
					return true;
				}
				if (merged->VFread1 == 0)
				{
					merged->VFread1 = reg;
					merged->VFr1xyzw = lanes;
					return true;
				}
				return false;
			};

			return add_read(first.VFread0, first.VFr0xyzw) &&
				add_read(first.VFread1, first.VFr1xyzw) &&
				add_read(second.VFread0, second.VFr0xyzw) &&
				add_read(second.VFread1, second.VFr1xyzw);
		}

		bool CanElideAnalyzedCanonicalFmacStallTest(const BlockPlan& block,
			u32 pair_index, const _VURegsNum& regs)
		{
			if (regs.VFread0 == 0 && regs.VFread1 == 0)
				return false;

			// Local-FMAC blocks retain their established split: pairs 0..3 are
			// canonical, while later writers are tested by the private pipeline.
			if (block.local_fmac_pipeline)
				return pair_index >= LOCAL_FMAC_CANONICAL_STALL_MATURE_PAIR;

			// Sony's FMAC latency is exactly four cycles. EmitPair() increments
			// the cycle before this test, so after three earlier pairs every
			// entry inherited at the block seam is at least four cycles old.
			// Stalls can only make it older. Of this block's own canonical
			// writers, only the preceding three issue slots can still be busy.
			if (pair_index < FMAC_PIPELINE_LATENCY_CYCLES - 1)
				return false;
			for (u32 writer_index = pair_index - (FMAC_PIPELINE_LATENCY_CYCLES - 1);
				writer_index < pair_index; writer_index++)
			{
				const PairPlan& writer = block.pairs[writer_index];
				if (writer.fmac_pipe && FmacWriterConflicts(writer, regs))
					return false;
			}
			return true;
		}

		bool ProducesInlineMacStatus(const PairPlan& pair)
		{
			return pair.upper_addsub_inline || pair.upper_mul_inline ||
				pair.upper_maddmsub_inline || pair.upper_outer_inline;
		}

		bool ReadsArchitecturalMacFlag(const PairPlan& pair)
		{
			const u32 reads = pair.uregs.VIread | pair.lregs.VIread;
			if ((reads & (1u << REG_MAC_FLAG)) != 0)
				return true;
			if (!pair.exec_lower)
				return false;

			switch (static_cast<VUInterpFast::LowerFastKind>(pair.lower_kind))
			{
				case VUInterpFast::LowerFastKind::FMEQ:
				case VUInterpFast::LowerFastKind::FMAND:
				case VUInterpFast::LowerFastKind::FMOR:
					return true;
				default:
					return false;
			}
		}

		bool IsMaddMsubAliasFlagPath(const PairPlan& pair)
		{
			if (!pair.upper_maddmsub_inline)
				return false;
			const auto kind = static_cast<VUInterpFast::UpperFastKind>(pair.upper_kind);
			return IsUpperMaddMsubVfBroadcastKind(kind) &&
				VUInterpFast::Fd(pair.upper) == VUInterpFast::Ft(pair.upper);
		}

		void AnalyzeMacFlagLiveness(BlockPlan* block)
		{
			if (!block->local_fmac_pipeline)
				return;

			std::array<bool, MAX_BLOCK_PAIRS> required{};
			const auto mark_visible_window = [&](u32 observer_pair, bool block_seam) {
				// Sony VU User Manual 3.4.4: a producer becomes visible four
				// cycles after issue. Every pair advances at least one cycle, so the
				// newest producer at least four pair positions old is guaranteed
				// visible. Runtime dependency stalls can additionally expose any
				// younger producer; retain that complete bounded suffix.
				s32 newest_guaranteed = -1;
				for (u32 i = 0; i < observer_pair; i++)
				{
					if (!ProducesInlineMacStatus(block->pairs[i]))
						continue;
					const bool guaranteed = block_seam ?
						(i + FMAC_PIPELINE_LATENCY_CYCLES < observer_pair) :
						(i + FMAC_PIPELINE_LATENCY_CYCLES <= observer_pair);
					if (guaranteed)
						newest_guaranteed = static_cast<s32>(i);
				}

				const u32 first = newest_guaranteed >= 0 ?
					static_cast<u32>(newest_guaranteed) : observer_pair;
				for (u32 i = first; i < observer_pair; i++)
				{
					if (ProducesInlineMacStatus(block->pairs[i]))
						required[i] = true;
				}
			};

			for (u32 observer = 0; observer < block->pair_count; observer++)
			{
				if (ReadsArchitecturalMacFlag(block->pairs[observer]))
					mark_visible_window(observer, false);

				// OPMULA/OPMSUB preserve the inactive W MAC lane and STATUS is
				// derived from the resulting complete MAC value. Therefore the
				// immediately preceding working instance is live even when no FMxx
				// instruction reads MAC architecturally.
				if (block->pairs[observer].upper_outer_inline)
				{
					for (u32 i = observer; i > 0; i--)
					{
						if (ProducesInlineMacStatus(block->pairs[i - 1]))
						{
							required[i - 1] = true;
							break;
						}
					}
				}
			}

			// Preserve the exact architectural MAC and every possibly-live
			// four-cycle queue instance reconstructed at the generated-code seam.
			mark_visible_window(block->pair_count, true);

			for (u32 i = LOCAL_FMAC_WARMUP_PAIRS; i < block->pair_count; i++)
			{
				PairPlan& pair = block->pairs[i];
				if (!required[i] && CanSnapshotLocalFmacFlagsAtProducer(pair) &&
					!pair.upper_outer_inline && !IsMaddMsubAliasFlagPath(pair))
				{
					pair.mac_flag_result_required = false;
					block->mac_flag_classification_elisions++;
				}
			}
		}

		bool IsImmediateBranchKind(VUInterpFast::LowerFastKind kind)
		{
			switch (kind)
			{
				case VUInterpFast::LowerFastKind::IBEQ:
				case VUInterpFast::LowerFastKind::IBNE:
				case VUInterpFast::LowerFastKind::IBLTZ:
				case VUInterpFast::LowerFastKind::IBGTZ:
				case VUInterpFast::LowerFastKind::IBLEZ:
				case VUInterpFast::LowerFastKind::IBGEZ:
				case VUInterpFast::LowerFastKind::B:
				case VUInterpFast::LowerFastKind::BAL:
					return true;
				default:
					return false;
			}
		}

		bool LowerWritesViBackup(u32 code, VUInterpFast::LowerFastKind kind)
		{
			// PCSX2 owner: VUops.cpp lower integer, MTIR, and post-indexed LSU
			// bodies. ILW/ILWR deliberately do not create the two-cycle branch
			// visibility window; LQI/LQD/SQI/SQD call _vuBackupVI() even for VI0.
			switch (kind)
			{
				case VUInterpFast::LowerFastKind::IADDIU:
				case VUInterpFast::LowerFastKind::ISUBIU:
				case VUInterpFast::LowerFastKind::IADDI:
				case VUInterpFast::LowerFastKind::MTIR:
					return VUInterpFast::It(code) != 0;

				case VUInterpFast::LowerFastKind::IADD:
				case VUInterpFast::LowerFastKind::ISUB:
				case VUInterpFast::LowerFastKind::IAND:
				case VUInterpFast::LowerFastKind::IOR:
					return VUInterpFast::Id(code) != 0;

				case VUInterpFast::LowerFastKind::LQI:
				case VUInterpFast::LowerFastKind::LQD:
				case VUInterpFast::LowerFastKind::SQI:
				case VUInterpFast::LowerFastKind::SQD:
					return true;

				default:
					return false;
			}
		}

		bool IsUnconditionalImmediateBranchKind(VUInterpFast::LowerFastKind kind)
		{
			return kind == VUInterpFast::LowerFastKind::B ||
				kind == VUInterpFast::LowerFastKind::BAL;
		}

		u32 StaticBranchTargetPc(u32 pc, u32 lower, u32 prog_mask)
		{
			return (pc + 8 + static_cast<s32>(VUInterpFast::Imm11(lower)) * 8) & prog_mask;
		}

		// Mirrors the analysis+path-selection half of _vu0Exec()/_vu1Exec() for one pair.
		// Returns false when the interpreter must own this pair (non-fast op).
		bool AnalyzePair(u32 vu_index, u32 pc, u32 upper, u32 lower, PairPlan* plan)
		{
			plan->pc = pc;
			plan->upper = upper;
			plan->lower = lower;
			plan->ebit = (upper & UPPER_E_BIT) != 0;
			plan->mflag = (upper & UPPER_M_BIT) != 0;
			plan->dflag = (upper & UPPER_D_BIT) != 0;
			plan->tflag = (upper & UPPER_T_BIT) != 0;
			plan->uregs = {};
			plan->lregs = {};

			const bool lower_nop = IsLowerNop(lower);

			if (!(upper & UPPER_I_BIT) && IsUpperNop(upper))
			{
				plan->shape = PairShape::UpperNop;
				plan->exec_upper = false;
				if (lower_nop)
				{
					plan->exec_lower = false;
				}
				else
				{
					if (!VuMicroAnalyzeLowerNoUpperCached(vu_index, pc, lower, &plan->lregs, &plan->lower_kind))
						return false;
					plan->exec_lower = true;
				}
			}
			else
			{
				if (!VuMicroAnalyzeUpperNoLowerCached(vu_index, pc, upper, &plan->uregs, &plan->upper_kind))
					return false;
				plan->exec_upper = true;

				if (upper & UPPER_I_BIT)
				{
					plan->shape = PairShape::IBit;
					plan->exec_lower = false;
				}
				else if (lower_nop)
				{
					plan->shape = PairShape::LowerNop;
					plan->exec_lower = false;
				}
				else
				{
					plan->shape = PairShape::Paired;
					if (!VuMicroAnalyzeLowerNoUpperCached(vu_index, pc, lower, &plan->lregs, &plan->lower_kind))
						return false;
					plan->exec_lower = true;
				}
			}

			// Hazard rules, PCSX2 owner: _vu1Exec() lines guarding the paired
			// upper/lower same-cycle VF and clip-flag interactions.
			plan->vf_backup_reg = 0;
			plan->vi_clip_backup = false;
			plan->discard_lower = false;
			if (plan->shape == PairShape::Paired)
			{
				if (plan->uregs.VFwrite)
				{
					if (plan->lregs.VFwrite == plan->uregs.VFwrite)
						plan->discard_lower = true;
					if (plan->lregs.VFread0 == plan->uregs.VFwrite ||
						plan->lregs.VFread1 == plan->uregs.VFwrite)
					{
						plan->vf_backup_reg = plan->uregs.VFwrite;
					}
				}
				if (plan->uregs.VIwrite & (1u << REG_CLIP_FLAG))
				{
					if (plan->lregs.VIwrite & (1u << REG_CLIP_FLAG))
						plan->discard_lower = true;
					if (plan->lregs.VIread & (1u << REG_CLIP_FLAG))
						plan->vi_clip_backup = true;
				}
				if (plan->discard_lower)
				{
					plan->vf_backup_reg = 0;
					plan->vi_clip_backup = false;
					plan->exec_lower = false;
				}
			}

			if (plan->exec_upper &&
				!IsInlineUpperKind(static_cast<VUInterpFast::UpperFastKind>(plan->upper_kind)))
			{
				return false;
			}
			if (plan->exec_lower &&
				!IsInlineLowerKind(static_cast<VUInterpFast::LowerFastKind>(plan->lower_kind)))
			{
				return false;
			}

			plan->upper_nop_inline = plan->exec_upper &&
				IsInlineUpperNopKind(static_cast<VUInterpFast::UpperFastKind>(plan->upper_kind));
			plan->upper_unary_inline = plan->exec_upper &&
				IsInlineUpperUnaryKind(static_cast<VUInterpFast::UpperFastKind>(plan->upper_kind));
			plan->upper_clip_inline = plan->exec_upper &&
				IsInlineUpperClipKind(static_cast<VUInterpFast::UpperFastKind>(plan->upper_kind));
			plan->upper_minmax_inline = plan->exec_upper &&
				IsInlineUpperMinMaxKind(static_cast<VUInterpFast::UpperFastKind>(plan->upper_kind));
			plan->upper_addsub_inline = plan->exec_upper &&
				IsInlineUpperAddSubKind(static_cast<VUInterpFast::UpperFastKind>(plan->upper_kind));
			plan->upper_mul_inline = plan->exec_upper &&
				IsInlineUpperMulKind(static_cast<VUInterpFast::UpperFastKind>(plan->upper_kind));
			plan->upper_maddmsub_inline = plan->exec_upper &&
				IsInlineUpperMaddMsubKind(static_cast<VUInterpFast::UpperFastKind>(plan->upper_kind));
			plan->upper_outer_inline = plan->exec_upper &&
				IsInlineUpperOuterKind(static_cast<VUInterpFast::UpperFastKind>(plan->upper_kind));
			plan->lower_ialu_inline = plan->exec_lower &&
				IsInlineLowerIaluKind(static_cast<VUInterpFast::LowerFastKind>(plan->lower_kind));
			plan->lower_flag_inline = plan->exec_lower &&
				IsInlineLowerFlagKind(static_cast<VUInterpFast::LowerFastKind>(plan->lower_kind));
			plan->lower_move_inline = plan->exec_lower &&
				IsInlineLowerMoveKind(static_cast<VUInterpFast::LowerFastKind>(plan->lower_kind));
			plan->lower_lsu_inline = plan->exec_lower &&
				IsInlineLowerLsuKind(static_cast<VUInterpFast::LowerFastKind>(plan->lower_kind));
			plan->lower_control_inline = plan->exec_lower &&
				IsInlineLowerControlKind(static_cast<VUInterpFast::LowerFastKind>(plan->lower_kind));
			plan->lower_branch_inline = plan->exec_lower &&
				IsInlineLowerBranchKind(static_cast<VUInterpFast::LowerFastKind>(plan->lower_kind));
			plan->lower_fdiv_inline = plan->exec_lower &&
				IsInlineLowerFdivKind(static_cast<VUInterpFast::LowerFastKind>(plan->lower_kind));
			plan->lower_efu_inline = plan->exec_lower &&
				IsInlineLowerEfuKind(static_cast<VUInterpFast::LowerFastKind>(plan->lower_kind));
			plan->lower_xgkick_inline = plan->exec_lower &&
				IsInlineLowerXgkickKind(static_cast<VUInterpFast::LowerFastKind>(plan->lower_kind));
			plan->vi_backup_write = plan->exec_lower && LowerWritesViBackup(plan->lower,
				static_cast<VUInterpFast::LowerFastKind>(plan->lower_kind));

			// Stall-helper selection, PCSX2 owner: VUops.cpp. The switch arms
			// are pure functions of the compile-time _VURegsNum:
			//  - _vuTestUpperStalls() only acts on FMAC pipes with VF reads.
			//  - _vuTestLowerStalls() acts on FMAC (VF reads), FDIV/EFU
			//    (pending pipe wait), and BRANCH (IALU result wait).
			//  - _vuAddUpperStalls()/_vuAddLowerStalls() record FMAC entries
			//    unconditionally (flag snapshots matter even with no writes),
			//    FDIV on REG_Q writes, EFU on REG_P writes, IALU when the op
			//    carries a nonzero latency.
			const bool upper_stall_shape = (plan->shape != PairShape::UpperNop);
			plan->test_upper_stalls = upper_stall_shape &&
				plan->uregs.pipe == VUPIPE_FMAC &&
				(plan->uregs.VFread0 != 0 || plan->uregs.VFread1 != 0);
			plan->add_upper_stalls = upper_stall_shape && plan->uregs.pipe == VUPIPE_FMAC;

			switch (plan->lregs.pipe)
			{
				case VUPIPE_FMAC:
					plan->test_lower_stalls = (plan->lregs.VFread0 != 0 || plan->lregs.VFread1 != 0);
					plan->add_lower_stalls = true;
					break;
				case VUPIPE_FDIV:
					plan->test_lower_stalls = true;
					plan->add_lower_stalls = (plan->lregs.VIwrite & (1u << REG_Q)) != 0;
					break;
				case VUPIPE_EFU:
					plan->test_lower_stalls = true;
					plan->add_lower_stalls = (plan->lregs.VIwrite & (1u << REG_P)) != 0;
					break;
				case VUPIPE_BRANCH:
					plan->test_lower_stalls = plan->lregs.VIread != 0;
					plan->add_lower_stalls = false;
					break;
				case VUPIPE_IALU:
					plan->test_lower_stalls = false;
					plan->add_lower_stalls = plan->lregs.cycles != 0;
					break;
				default:
					plan->test_lower_stalls = false;
					plan->add_lower_stalls = false;
					break;
			}
			plan->lower_stall_inline = plan->add_lower_stalls &&
				(plan->lregs.pipe == VUPIPE_FMAC ||
					plan->lregs.pipe == VUPIPE_IALU ||
					plan->lregs.pipe == VUPIPE_FDIV ||
					plan->lregs.pipe == VUPIPE_EFU);
			plan->lower_branch_stall_test_inline = plan->test_lower_stalls &&
				plan->lregs.pipe == VUPIPE_BRANCH;
			plan->lower_fdiv_stall_test_inline = plan->test_lower_stalls &&
				plan->lregs.pipe == VUPIPE_FDIV;
			plan->lower_efu_stall_test_inline = plan->test_lower_stalls &&
				plan->lregs.pipe == VUPIPE_EFU;
			plan->upper_fmac_stall_test_inline = plan->test_upper_stalls &&
				plan->uregs.pipe == VUPIPE_FMAC;
			plan->lower_fmac_stall_test_inline = plan->test_lower_stalls &&
				plan->lregs.pipe == VUPIPE_FMAC;

			plan->fmac_pipe = (plan->uregs.pipe == VUPIPE_FMAC) || (plan->lregs.pipe == VUPIPE_FMAC);
			// _vuTestPipes() can be skipped whenever the runtime pipe-ready
			// guard proves every PCSX2 flush arm would be side-effect-free.
			plan->test_pipes_fast_guard = true;
			return true;
		}

		bool IsVu0ConservativeUnsupportedPair(const PairPlan& plan)
		{
			// VU0 shares VUops.cpp branch helpers and VU0microInterp.cpp's
			// branch-delay countdown with VU1; remaining unsupported pairs are
			// rejected by AnalyzePair()/inline kind selection.
			(void)plan;
			return false;
		}

		bool IsUnobservableNopPair(const PairPlan& plan)
		{
			// PCSX2 owners: VU1microInterp.cpp::_vu1IsUpperNop()/
			// _vu1IsLowerNop() and x86/microVU_Compile.inl::mVUincCycles().
			// A pair is an empty scheduling slot only when neither half executes and
			// no pair flag, branch-delay, or E-bit seam has architectural work.
			return plan.shape == PairShape::UpperNop && !plan.exec_upper && !plan.exec_lower &&
				!plan.ebit && !plan.mflag && !plan.dflag && !plan.tflag &&
				!plan.branch_tail && !plan.ebit_tail && !plan.ends_block;
		}

		// Scans a straight-line pair run from start_pc, statically simulating
		// _vu0Exec()/_vu1Exec() branch-delay and E-bit windows. Entry contract
		// (checked by the dispatcher): ebit is 0 or 1 and branch is either 0 for
		// a normal block or 1 for a one-pair pending-branch continuation.
		bool ScanBlock(const u8* micro, u32 vu_index, u32 prog_size, u32 prog_mask,
			bool conservative_vu0, u32 start_pc, bool entry_branch_tail,
			bool entry_ebit_tail, BlockPlan* block)
		{
			block->start_pc = start_pc;
			block->pair_count = 0;
			block->entry_branch_tail = entry_branch_tail;
			block->entry_ebit_tail = entry_ebit_tail;
			block->continues_logical_block_if_busy = false;
			block->test_pipes_fast_guard_pairs = 0;
			block->nop_pipe_test_defer_pairs = 0;
			block->empty_pipe_nop_batch_runs = 0;
			block->empty_pipe_nop_batch_pairs = 0;
			block->fmac_clear_inline_pairs = 0;
			block->upper_fmac_stall_test_inline_pairs = 0;
			block->lower_fmac_stall_test_inline_pairs = 0;
			block->upper_fmac_stall_inline_pairs = 0;
			block->lower_fmac_stall_inline_pairs = 0;
			block->upper_addsub_inline_pairs = 0;
			block->upper_mul_inline_pairs = 0;
			block->upper_maddmsub_inline_pairs = 0;
			block->upper_outer_inline_pairs = 0;
			block->upper_nop_inline_pairs = 0;
			block->upper_unary_inline_pairs = 0;
			block->upper_clip_inline_pairs = 0;
			block->upper_minmax_inline_pairs = 0;
			block->lower_ialu_inline_pairs = 0;
			block->lower_flag_inline_pairs = 0;
			block->lower_move_inline_pairs = 0;
			block->lower_lsu_inline_pairs = 0;
			block->lower_control_inline_pairs = 0;
			block->lower_branch_inline_pairs = 0;
			block->lower_fdiv_inline_pairs = 0;
			block->lower_efu_inline_pairs = 0;
			block->lower_xgkick_inline_pairs = 0;
			block->lower_fdiv_stall_test_inline_pairs = 0;
			block->lower_efu_stall_test_inline_pairs = 0;
			block->lower_branch_stall_test_inline_pairs = 0;
			block->lower_stall_inline_pairs = 0;
			block->vi_backup_update_elided_pairs = 0;
			block->vi_backup_zero_store_pairs = 0;
			block->dt_flag_inline_pairs = 0;
			block->mac_flag_classification_elisions = 0;
			block->resident_cycle = false;
			block->resident_cycle_high = false;
			block->resident_pipe_activity = false;
			block->direct_link_tail = false;
			block->direct_links = {};

			u32 pc = start_pc;
			u32 branch_window = entry_branch_tail ? 1 : 0;
			u32 ebit_static = entry_ebit_tail ? 1 : 0;
			bool pending_branch_static = false;
			bool pending_branch_unconditional = false;
			u32 pending_branch_target = 0;
			u32 pending_branch_fallthrough = 0;
			bool resident_cycle = true;

			while (block->pair_count < MAX_BLOCK_PAIRS && pc < prog_size)
			{
				u32 upper;
				u32 lower;
				std::memcpy(&lower, &micro[pc + 0], sizeof(lower));
				std::memcpy(&upper, &micro[pc + 4], sizeof(upper));

				PairPlan& plan = block->pairs[block->pair_count];
				plan = {};
				if (!AnalyzePair(vu_index, pc, upper, lower, &plan))
					break;
				if (conservative_vu0 && IsVu0ConservativeUnsupportedPair(plan))
					break;
				if (plan.test_upper_stalls || plan.test_lower_stalls ||
					(plan.lregs.pipe == VUPIPE_XGKICK && !conservative_vu0))
				{
					resident_cycle = false;
				}
				if (plan.test_pipes_fast_guard)
					block->test_pipes_fast_guard_pairs++;
				if (plan.fmac_pipe)
					block->fmac_clear_inline_pairs++;
				if (plan.upper_fmac_stall_test_inline)
					block->upper_fmac_stall_test_inline_pairs++;
				if (plan.lower_fmac_stall_test_inline)
					block->lower_fmac_stall_test_inline_pairs++;
				if (plan.add_upper_stalls)
					block->upper_fmac_stall_inline_pairs++;
				if (plan.add_lower_stalls && plan.lregs.pipe == VUPIPE_FMAC)
					block->lower_fmac_stall_inline_pairs++;
				if (plan.upper_addsub_inline)
					block->upper_addsub_inline_pairs++;
				if (plan.upper_mul_inline)
					block->upper_mul_inline_pairs++;
				if (plan.upper_maddmsub_inline)
					block->upper_maddmsub_inline_pairs++;
				if (plan.upper_outer_inline)
					block->upper_outer_inline_pairs++;
				if (plan.upper_nop_inline)
					block->upper_nop_inline_pairs++;
				if (plan.upper_unary_inline)
					block->upper_unary_inline_pairs++;
				if (plan.upper_clip_inline)
					block->upper_clip_inline_pairs++;
				if (plan.upper_minmax_inline)
					block->upper_minmax_inline_pairs++;
				if (plan.lower_ialu_inline)
					block->lower_ialu_inline_pairs++;
				if (plan.lower_flag_inline)
					block->lower_flag_inline_pairs++;
				if (plan.lower_move_inline)
					block->lower_move_inline_pairs++;
				if (plan.lower_lsu_inline)
					block->lower_lsu_inline_pairs++;
				if (plan.lower_control_inline)
					block->lower_control_inline_pairs++;
				if (plan.lower_branch_inline)
					block->lower_branch_inline_pairs++;
				if (plan.lower_fdiv_inline)
					block->lower_fdiv_inline_pairs++;
				if (plan.lower_efu_inline)
					block->lower_efu_inline_pairs++;
				if (plan.lower_xgkick_inline)
					block->lower_xgkick_inline_pairs++;
				if (plan.lower_fdiv_stall_test_inline)
					block->lower_fdiv_stall_test_inline_pairs++;
				if (plan.lower_efu_stall_test_inline)
					block->lower_efu_stall_test_inline_pairs++;
				if (plan.lower_branch_stall_test_inline)
					block->lower_branch_stall_test_inline_pairs++;
				if (plan.lower_stall_inline)
					block->lower_stall_inline_pairs++;
				if (plan.dflag || plan.tflag)
					block->dt_flag_inline_pairs++;

				if (plan.ebit)
					ebit_static = 2;

				if (branch_window > 0)
				{
					plan.branch_tail = true;
					plan.resolves_branch = true;
					branch_window--;
					if (branch_window == 0)
						plan.ends_block = true; // TPC may have been redirected
				}
				if (plan.lregs.pipe == VUPIPE_BRANCH && !plan.ends_block)
				{
					plan.branch_tail = true;
					branch_window = 1; // next pair is the delay slot resolution
					const auto branch_kind = static_cast<VUInterpFast::LowerFastKind>(plan.lower_kind);
					pending_branch_static = IsImmediateBranchKind(branch_kind);
					pending_branch_unconditional = IsUnconditionalImmediateBranchKind(branch_kind);
					pending_branch_target = pending_branch_static ? StaticBranchTargetPc(plan.pc, plan.lower, prog_mask) : 0;
					pending_branch_fallthrough = (plan.pc + 16) & prog_mask;
				}

				if (ebit_static > 0)
				{
					ebit_static--;
					plan.ebit_tail = true;
					plan.ebit_store = ebit_static;
					if (ebit_static == 0)
						plan.ends_block = true; // VPU_STAT run bit cleared
				}
				if (conservative_vu0 && plan.mflag)
					plan.ends_block = true; // InterpVU0::Execute() exits on M-bit after the pair
				if (plan.dflag || plan.tflag)
					plan.ends_block = true; // runtime FBRST can clear the run bit

				block->pair_count++;
				pc += 8;
				if (plan.ends_block)
					break;
			}

			if (block->pair_count == 0)
				return false;

			// _vuBackupVI() installs exactly a two-cycle visibility window and
			// every admitted pair advances at least one cycle before this update.
			// Consequently a writer two pairs back is unconditionally expired here,
			// while a pair with no writer in either preceding slot is already zero.
			// This is the first block-state reduction owned by PCSX2 microVU's
			// compile-time `viBackUp` model; it is independent of runtime stall size.
			for (u32 i = 2; i < block->pair_count; i++)
			{
				if (block->pairs[i - 1].vi_backup_write)
					continue;
				if (block->pairs[i - 2].vi_backup_write)
					block->vi_backup_zero_store_pairs++;
				else
					block->vi_backup_update_elided_pairs++;
			}

			// PCSX2 microVU advances compile-time pipe state through unobservable
			// scheduling slots instead of publishing every intermediate cycle. Keep
			// the final NOP in each run canonical so every real consumer and every
			// generated-code seam sees the exact VUops.cpp pipeline state. VU0 stays
			// conservative until its COP2 macro visibility contract is represented by
			// the same compile-time pipeline model.
			if (!conservative_vu0)
			{
				for (u32 i = 0; i + 1 < block->pair_count; i++)
				{
					PairPlan& current = block->pairs[i];
					const PairPlan& next = block->pairs[i + 1];
					if (IsUnobservableNopPair(current) && IsUnobservableNopPair(next))
					{
						current.defer_nop_pipe_test = true;
						block->nop_pipe_test_defer_pairs++;
					}
				}
			}

			block->resident_cycle = resident_cycle;
			if (block->pair_count != 0)
			{
				const PairPlan& last = block->pairs[block->pair_count - 1];
				block->continues_logical_block_if_busy = !last.ends_block ||
					(!last.resolves_branch && (last.dflag || last.tflag));
				block->direct_link_tail = !block->continues_logical_block_if_busy &&
					!conservative_vu0 && !last.dflag && !last.tflag &&
					!(last.ebit_tail && last.ebit_store == 0);
				if (block->direct_link_tail)
				{
					const bool target_ebit_tail = last.ebit_tail && last.ebit_store != 0;
					if (!last.branch_tail)
					{
						block->direct_links[0] = {
							true,
							false,
							false,
							0,
							(block->start_pc + block->pair_count * 8) & prog_mask,
							false,
							target_ebit_tail,
						};
					}
					else if (pending_branch_static)
					{
						const bool delay_branch_tail = last.lregs.pipe == VUPIPE_BRANCH;
						block->direct_links[0] = {
							true,
							false,
							!pending_branch_unconditional,
							pending_branch_target,
							pending_branch_target,
							delay_branch_tail,
							target_ebit_tail,
						};
						if (!pending_branch_unconditional)
						{
							block->direct_links[1] = {
								true,
								false,
								true,
								pending_branch_fallthrough,
								pending_branch_fallthrough,
								delay_branch_tail,
								target_ebit_tail,
							};
						}
					}
					else
					{
						const bool delay_branch_tail = last.lregs.pipe == VUPIPE_BRANCH;
						for (DirectLinkPlan& link : block->direct_links)
						{
							link = {
								true,
								true,
								true,
								0,
								0,
								delay_branch_tail,
								target_ebit_tail,
							};
						}
					}
				}

				block->local_fmac_pipeline = !conservative_vu0 && CanUseLocalFmacPipeline(*block);
				// The canonical empty-pipe guard is five loads plus four ORRs and a
				// branch at every pair. A resident block pays one exact five-field
				// refresh at entry, then CMP+BLNE per guard. Three guards recover the
				// entry cost even when the block arrived through a linked-entry refresh.
				// VU0 remains conservative around macro-mode/COP2 visibility.
				block->resident_pipe_activity = !conservative_vu0 &&
					block->test_pipes_fast_guard_pairs >= 3;
				// Compiler-owned FMAC slots make the long block's steady-state hazard
				// updates local as well. Keep cycle.low in r5 for those blocks; the few
				// canonical FDIV/EFU/IALU/FMAC stall scans are explicitly synchronized
				// around their exact VURegs cycle updates by EmitPair().
				block->resident_cycle = block->resident_cycle || block->local_fmac_pipeline;
				// r9 has only two generated-code owners: paired CLIP backup or the
				// cycle high word used by exact pipe/stall arithmetic. Every block
				// admitted to low-word residency has scan-proven cycle updates, so
				// retain the complete r5:r9 pair whenever no pair needs CLIP backup.
				// Besides replacing each normally-taken carry branch with ADC, this
				// also removes canonical cycle.high reloads inside inherited-pipe
				// publisher calls.
				block->resident_cycle_high = block->resident_cycle;
				if (block->resident_cycle_high)
				{
					for (u32 i = 0; i < block->pair_count; i++)
					{
						if (block->pairs[i].vi_clip_backup)
						{
							block->resident_cycle_high = false;
							break;
						}
					}
				}
				if (block->local_fmac_pipeline)
				{
					for (u32 i = LOCAL_FMAC_WARMUP_PAIRS; i < block->pair_count; i++)
					{
						PairPlan& pair = block->pairs[i];
						if (!pair.fmac_pipe)
							continue;
						// PCSX2 microVU owner: microVU_Analyze.inl::analyzeReg1()
						// and microVU_Compile.inl::mVUsetCycles(). A compiler-owned
						// FMAC issue time is observable only by a VF read during the
						// remaining three-cycle hazard window, or when the entry must be
						// reconstructed at a block seam. A read four pairs later is
						// already at the fixed S-stage visibility point even if an older
						// pipe introduced additional stalls.
						bool needs_cycle_snapshot =
							i + FMAC_PIPELINE_LATENCY_CYCLES >= block->pair_count;
						const u32 hazard_end = std::min(block->pair_count,
							i + FMAC_PIPELINE_LATENCY_CYCLES);
						for (u32 reader_index = i + 1;
							!needs_cycle_snapshot && reader_index < hazard_end;
							reader_index++)
						{
							const PairPlan& reader = block->pairs[reader_index];
							needs_cycle_snapshot =
								(reader.test_upper_stalls &&
									FmacWriterConflicts(pair, reader.uregs)) ||
								(reader.test_lower_stalls &&
									FmacWriterConflicts(pair, reader.lregs));
						}
						pair.local_fmac_cycle_snapshot = needs_cycle_snapshot;
						block->local_fmac_cycle_snapshot_elision_pairs +=
							needs_cycle_snapshot ? 0u : 1u;
						block->local_fmac_pipeline_pairs++;
						block->local_fmac_producer_snapshot_pairs +=
							CanSnapshotLocalFmacFlagsAtProducer(pair) ? 1u : 0u;
						const u32 flagreg = (pair.add_upper_stalls ? pair.uregs.VIwrite : 0) |
							((pair.add_lower_stalls && pair.lregs.pipe == VUPIPE_FMAC) ?
								pair.lregs.VIwrite : 0);
						block->local_fmac_clip_snapshot_elisions +=
							(flagreg & (1u << REG_CLIP_FLAG)) == 0 ? 1u : 0u;
						block->deferred_fmac_flag_retirements +=
							(i + FMAC_PIPELINE_LATENCY_CYCLES < block->pair_count) ? 1u : 0u;
					}
					block->deferred_fmac_flags =
						block->deferred_fmac_flag_retirements >= 4 &&
						CanDeferFmacFlags(*block);
					block->resident_working_fmac_flags = block->deferred_fmac_flags &&
						block->local_fmac_producer_snapshot_pairs >= 2;
					if (block->resident_working_fmac_flags)
					{
						// DIV/SQRT/RSQRT update the working status bits immediately and
						// therefore still use the canonical VURegs representation. The
						// architectural delayed STATUS/MAC residency above remains valid.
						for (u32 i = 0; i < block->pair_count; i++)
						{
							if (block->pairs[i].lower_fdiv_inline)
							{
								block->resident_working_fmac_flags = false;
								break;
							}
						}
					}
					if (block->deferred_fmac_flags)
					{
						for (u32 i = LOCAL_FMAC_WARMUP_PAIRS;
							i + FMAC_PIPELINE_LATENCY_CYCLES < block->pair_count; i++)
						{
							const PairPlan& pair = block->pairs[i];
							const u32 flagreg = (pair.add_upper_stalls ? pair.uregs.VIwrite : 0) |
								((pair.add_lower_stalls && pair.lregs.pipe == VUPIPE_FMAC) ?
									pair.lregs.VIwrite : 0);
							if (pair.fmac_pipe && CanSnapshotLocalFmacFlagsAtProducer(pair))
							{
								block->deferred_fmac_compact_retirements++;
								block->deferred_fmac_compact_instructions_removed +=
									(flagreg & (1u << REG_STATUS_FLAG)) != 0 ? 3u : 2u;
							}
						}
					}
					AnalyzeMacFlagLiveness(block);
				}
				else
				{
					// PCSX2 microVU_Flags.inl keeps delayed flag instances private
					// independently of whether the FMAC queue itself is compiler-owned.
					// A canonical producer is guaranteed to retire before pair i+4:
					// Sony fixes FMAC latency at four cycles and every pair advances at
					// least one cycle. Retaining r6/r7 across such a block therefore
					// removes the same architectural STATUS/MAC traffic from the
					// canonical publisher. Runtime stalls can only retire an entry
					// earlier and increase the saving.
					for (u32 i = 0;
						i + FMAC_PIPELINE_LATENCY_CYCLES < block->pair_count; i++)
					{
						block->deferred_fmac_flag_retirements +=
							block->pairs[i].fmac_pipe ? 1u : 0u;
					}
					block->deferred_fmac_flags =
						block->deferred_fmac_flag_retirements >= 4 &&
						CanDeferFmacFlags(*block);
				}

				for (u32 i = 0; i < block->pair_count; i++)
				{
					const PairPlan& pair = block->pairs[i];
					block->canonical_fmac_stall_tests_elided +=
						(pair.test_upper_stalls &&
							CanElideAnalyzedCanonicalFmacStallTest(*block, i,
								pair.uregs)) ? 1u : 0u;
					const bool lower_has_fmac_test = pair.lregs.pipe == VUPIPE_FMAC ||
						pair.lregs.pipe == VUPIPE_FDIV || pair.lregs.pipe == VUPIPE_EFU;
					block->canonical_fmac_stall_tests_elided +=
						(pair.test_lower_stalls && lower_has_fmac_test &&
							CanElideAnalyzedCanonicalFmacStallTest(*block, i,
								pair.lregs)) ? 1u : 0u;
				}

				// PCSX2's interpreter-side _vu1FastForwardPlainNopPairs() owns
				// the empty-pipeline batching contract. The native compiler may
				// additionally have private FMAC instances, so these runs are only
				// candidates here; emission retires those private instances at the
				// same final scheduling point and keeps an exact cold per-pair path
				// whenever the canonical pipe aggregate is nonzero at runtime.
				if (block->local_fmac_pipeline)
				{
					for (u32 i = 0; i < block->pair_count;)
					{
						if (!IsUnobservableNopPair(block->pairs[i]))
						{
							i++;
							continue;
						}
						u32 end = i + 1;
						while (end < block->pair_count &&
							IsUnobservableNopPair(block->pairs[end]))
						{
							end++;
						}
						const u32 pairs = end - i;
						if (pairs >= 2)
						{
							block->empty_pipe_nop_batch_runs++;
							block->empty_pipe_nop_batch_pairs += pairs;
						}
						i = end;
					}
				}
			}
			return block->pair_count != 0;
		}

		// ------------------------------------------------------------------
		// Code generation.
		// ------------------------------------------------------------------

		// Generated block ABI: u32 executed_pairs = block(VURegs*, executed_base,
		// limit_lo, limit_hi) with limit = startcycles + cycles. PCSX2's
		// x86/microVU_Compile.inl::mVUtestCycles() admits an entire compiled block
		// whenever at least one requested cycle remains, then tests the budget at
		// the next block entry. Generated tail links carry executed_base across
		// those block-entry tests without growing the stack.
		using BlockFn = u32 (*)(VURegs*, u32, u32, u32);
		constexpr u32 EXECUTED_PAIRS_LOGICAL_CONTINUATION = 0x80000000u;

		constexpr unsigned HOST_VU = 4;       // VURegs*
		constexpr unsigned HOST_CYCLE_LO = 5; // cycle low word after this pair's increment
		constexpr unsigned HOST_LIMIT_LO = 6;
		constexpr unsigned HOST_LIMIT_HI = 7;
		constexpr unsigned HOST_CLIP_OLD = 8; // paired clip-flag hazard backups
		constexpr unsigned HOST_CLIP_NEW = 9;
		// In local-FMAC blocks without paired clip backup, r9 instead owns the
		// resident high word paired with HOST_CYCLE_LO.
		constexpr unsigned HOST_CYCLE_HI = HOST_CLIP_NEW;
		// Stall scans use r10 before the per-pair pipe test.  A shared pipe-test
		// thunk reuses it as its private return register; all C++ callees preserve
		// r10 under AAPCS32.
		constexpr unsigned HOST_STALL_SCRATCH = 10;
		constexpr unsigned HOST_EXEC_BASE = 11;
		constexpr unsigned HOST_CALL_SCRATCH = 12;
		constexpr unsigned SP = 13;
		// Resident pipe activity packs the exact four-entry canonical FMAC
		// count in bits [2:0]. Every other pipe predicate is shifted above its
		// low byte, allowing a little-endian STRB to publish only the exact count;
		// consumers which only need the aggregate still use the same zero test.
		constexpr u32 RESIDENT_FMAC_COUNT_MASK = 0x7u;
		constexpr u8 RESIDENT_OTHER_PIPE_SHIFT = 8;
		constexpr u32 RESIDENT_OTHER_PIPE_MARKER = 1u << RESIDENT_OTHER_PIPE_SHIFT;

		// Scratch NEON quad registers for the vuDouble() quad-normalize path.
		// Q8-Q15 alias D16-D31, which have no single-precision (S) aliases, so
		// they never clash with the S0-S7 (Q0/Q1) operands the FMAC arithmetic
		// reads and writes.
		constexpr unsigned VU_NORM_EXP_Q = 8;   // 0x7f800000 broadcast (exponent mask)
		constexpr unsigned VU_NORM_SIGN_Q = 9;  // 0x80000000 broadcast (sign mask)
		constexpr unsigned VU_NORM_MAXF_Q = 10; // 0x7f7fffff broadcast (max finite)
		// Observer-free dead MAC producers contribute only sticky STATUS category
		// bits. Q11 accumulates their packed per-lane categories until the seam.
		constexpr unsigned VU_DEAD_FMAC_STICKY_Q = 11;
		// When no dead-FMAC categories are pending, the same otherwise-free Q11
		// caches the fixed full-XYZW MAC weights {8,4,2,1} across producers.
		constexpr unsigned VU_FULL_MAC_WEIGHTS_Q = VU_DEAD_FMAC_STICKY_Q;
		constexpr unsigned VU_NORM_EXPV_Q = 12; // exp = v & exponent mask
		constexpr unsigned VU_NORM_SIGNV_Q = 13; // sign = v & sign mask
		constexpr unsigned VU_NORM_TMP_Q = 14;  // select scratch
		constexpr unsigned VU_NORM_MASK_Q = 15; // per-lane select mask
		constexpr unsigned VU_VECTOR_CACHE_FIRST_Q = 4;
		constexpr unsigned VU_VECTOR_CACHE_SLOTS = 4;
		constexpr u8 VU_VECTOR_CACHE_ACC = 32;

		// XYZW lane weights used to pack four per-lane NEON comparison masks into
		// the VU MAC flag. The row is selected at compile time by the instruction's
		// destination mask. PCSX2's MAC layout repeats the XYZW nibble for
		// zero/sign/underflow/overflow, so one weighted horizontal OR produces the
		// base nibble for all four flag groups.
		alignas(16) static constexpr u32 VU_MAC_LANE_WEIGHTS[16][4] = {
			{0, 0, 0, 0}, {0, 0, 0, 1}, {0, 0, 2, 0}, {0, 0, 2, 1},
			{0, 4, 0, 0}, {0, 4, 0, 1}, {0, 4, 2, 0}, {0, 4, 2, 1},
			{8, 0, 0, 0}, {8, 0, 0, 1}, {8, 0, 2, 0}, {8, 0, 2, 1},
			{8, 4, 0, 0}, {8, 4, 0, 1}, {8, 4, 2, 0}, {8, 4, 2, 1},
		};

		// A full-XYZW result can first assemble one all-zero/all-one nibble per
		// category, then select the one lane bit from each nibble in a single
		// VAND. Repeating each lane's 8/4/2/1 weight across all four nibbles
		// makes that final selection produce the exact Sony Z/S/U/O MAC layout.
		alignas(16) static constexpr u32 VU_FULL_MAC_CATEGORY_WEIGHTS[4] = {
			0x8888u, 0x4444u, 0x2222u, 0x1111u,
		};

		// Same active-lane selection as VU_MAC_LANE_WEIGHTS, but each lane
		// contributes to one STATUS category bit instead of its XYZW MAC bit.
		// This is PCSX2 microVU_Upper.inl::mVUupdateFlags()'s sFLAG-only form.
		alignas(16) static constexpr u32 VU_STATUS_LANE_WEIGHTS[16][4] = {
			{0, 0, 0, 0}, {0, 0, 0, 1}, {0, 0, 1, 0}, {0, 0, 1, 1},
			{0, 1, 0, 0}, {0, 1, 0, 1}, {0, 1, 1, 0}, {0, 1, 1, 1},
			{1, 0, 0, 0}, {1, 0, 0, 1}, {1, 0, 1, 0}, {1, 0, 1, 1},
			{1, 1, 0, 0}, {1, 1, 0, 1}, {1, 1, 1, 0}, {1, 1, 1, 1},
		};

		constexpr u16 SAVED_REGISTER_MASK = 0x4ff0; // r4-r11, lr; each VU path realigns its private frame
		constexpr u16 RETURN_REGISTER_MASK = 0x8ff0; // r4-r11, pc
		constexpr u32 LOCAL_FMAC_BASE = 32;
		constexpr u32 LOCAL_FMAC_SLOT_SIZE = 24;
		constexpr u32 LOCAL_FMAC_SLOT_COUNT = 4;
		constexpr u32 LOCAL_FMAC_CYCLE_OFFSET = 0;
		constexpr u32 LOCAL_FMAC_MAC_OFFSET = 8;
		constexpr u32 LOCAL_FMAC_STATUS_OFFSET = 12;
		constexpr u32 LOCAL_FMAC_CLIP_OFFSET = 16;
		constexpr u32 DEFERRED_LIMIT_SAVE_OFFSET = 128;
		// The resident pipe aggregate lives in r10. This stack word is used only by
		// the shared publisher's rare XGKICK arm to preserve its LR across the C++ call.
		constexpr u32 XGKICK_THUNK_LR_SAVE_OFFSET = 136;
		constexpr u32 STACK_FRAME_SIZE = 140;
		constexpr u32 VECTOR_STACK_FRAME_SIZE = 144;

			struct CachedBlock;
			struct Vu1Program;

			struct Vu1DirectLinkSlot
			{
				CachedBlock* owner = nullptr;
				bool valid = false;
				bool runtime_observed = false;
				bool observed_target = false;
				bool guard_tpc = false;
				u32 guard_tpc_value = 0;
				u32 target_pc = 0;
				bool target_branch_tail = false;
				bool target_ebit_tail = false;
				size_t unlinked_fallback_offset = static_cast<size_t>(-1);
				size_t target_offset = static_cast<size_t>(-1);
				size_t fallback_offset = static_cast<size_t>(-1);
				size_t guard_tpc_offset = static_cast<size_t>(-1);
				size_t helper_slot_offset = static_cast<size_t>(-1);
				const void* patched_target = nullptr;
			};

			struct LinkedEntryOffsets
			{
				size_t normal = static_cast<size_t>(-1);
				size_t deferred_fmac = static_cast<size_t>(-1);
				size_t resident_pipe = static_cast<size_t>(-1);
				size_t resident_pipe_deferred_fmac = static_cast<size_t>(-1);
			};

			const void* LookupVu1DirectLinkBlockScalar(VURegs* vu, Vu1DirectLinkSlot* runtime_link);
			const void* LookupVu1DirectLinkBlockVector(VURegs* vu, Vu1DirectLinkSlot* runtime_link);
			const void* LookupVu1DirectLinkBlockScalarResidentPipe(
				VURegs* vu, Vu1DirectLinkSlot* runtime_link);
			const void* LookupVu1DirectLinkBlockVectorResidentPipe(
				VURegs* vu, Vu1DirectLinkSlot* runtime_link);
			const void* LookupVu1DirectLinkBlockScalarDeferredFmac(
				VURegs* vu, Vu1DirectLinkSlot* runtime_link);
			const void* LookupVu1DirectLinkBlockVectorDeferredFmac(
				VURegs* vu, Vu1DirectLinkSlot* runtime_link);
			const void* LookupVu1DirectLinkBlockScalarDeferredFmacResidentPipe(
				VURegs* vu, Vu1DirectLinkSlot* runtime_link);
			const void* LookupVu1DirectLinkBlockVectorDeferredFmacResidentPipe(
				VURegs* vu, Vu1DirectLinkSlot* runtime_link);

		u16 VuOffset(size_t offset)
		{
			pxAssert(offset < 4096);
			return static_cast<u16>(offset);
		}

		u16 ViOffset(unsigned reg)
		{
			return VuOffset(offsetof(VURegs, VI) + reg * sizeof(REG_VI));
		}

		u16 VfOffset(unsigned reg)
		{
			return VuOffset(offsetof(VURegs, VF) + reg * sizeof(VECTOR));
		}

		class BlockCompiler
		{
		public:
			enum class VectorCacheMode : u8
			{
				Disabled,
				Trace,
				Enabled,
			};

			enum class VectorAccessKind : u8
			{
				WordLoad,
				WordStore,
				LaneBroadcast,
				QuadLoad,
				QuadStore,
				Barrier,
			};

			struct VectorAccessEvent
			{
				VectorAccessKind kind = VectorAccessKind::Barrier;
				u8 guest = 0;
				bool needs_old_value = false;
				bool admit = false;
				u32 next_same_guest = 0xffffffffu;
			};

			struct VectorCacheOpportunity
			{
				u32 wrapper_instructions_removed = 0;
				u32 canonical_bytes_removed = 0;
				bool profitable = false;
			};

			struct VectorCacheStats
			{
				u64 hits = 0;
				u64 misses = 0;
				u64 evictions = 0;
				u64 writebacks = 0;
				u64 acc_hits = 0;
				u64 scalar_invalidations = 0;
				u64 preloads = 0;
				u64 uncached_loads = 0;
				u64 uncached_stores = 0;
				u64 vf_word_loads = 0;
				u64 vf_word_stores = 0;
				u64 vf_quad_loads = 0;
				u64 vf_quad_stores = 0;
				u64 acc_word_loads = 0;
				u64 acc_word_stores = 0;
				u64 acc_quad_loads = 0;
				u64 acc_quad_stores = 0;
			};

			explicit BlockCompiler(CodeBuffer& code, const BlockPlan& plan, PairPlan* stable_pairs,
				u32 mem_mask, VectorCacheMode vector_cache_mode = VectorCacheMode::Disabled,
				std::vector<VectorAccessEvent>* vector_accesses = nullptr)
				: m_code(code)
				, m_plan(plan)
				, m_pairs(stable_pairs)
				, m_mem_mask(mem_mask)
				, m_vu0_memory_map(mem_mask == VU0_MEMMASK)
				, m_resident_cycle(plan.resident_cycle)
				, m_resident_cycle_high(plan.resident_cycle_high)
				, m_vector_cache_mode(vector_cache_mode)
				, m_vector_accesses(vector_accesses)
			{
				pxAssert(m_vu0_memory_map || vector_cache_mode != VectorCacheMode::Trace || vector_accesses);
				pxAssert(m_vu0_memory_map || vector_cache_mode != VectorCacheMode::Enabled || vector_accesses);
				// VF0 is the architectural (0, 0, 0, 1) constant. Other live-ins
				// may have arrived through VU memory, COP2, or an earlier program and
				// therefore start with unknown vuDouble() representation.
				m_normalized_vector_lanes[0] = 0x0f;
				for (u32 i = 0; i < m_local_fmac_entries.size(); i++)
					m_local_fmac_entries[i].slot = static_cast<u8>(i);
			}

			bool Compile()
			{
				if (!EmitPrologue())
					return false;
				const size_t body_offset = m_code.Size();
				if (!EmitPreloadVectorCache())
					return false;

				for (u32 i = 0; i < m_plan.pair_count;)
				{
					u32 nop_run_end = i;
					if (m_plan.local_fmac_pipeline && IsUnobservableNopPair(m_pairs[i]))
					{
						nop_run_end++;
						while (nop_run_end < m_plan.pair_count &&
							IsUnobservableNopPair(m_pairs[nop_run_end]))
						{
							nop_run_end++;
						}
					}
					if (nop_run_end - i >= 2)
					{
						if (!EmitEmptyPipeNopRun(i, nop_run_end - i))
							return false;
						i = nop_run_end;
						continue;
					}
					if (!EmitPair(i))
						return false;
					i++;
				}
				if (!EmitMergeAccumulatedDeadFmacSticky())
					return false;
				if (!EmitCanonicalizeLocalFmacPipeline())
					return false;
				if (!EmitPublishResidentWorkingFmacFlags())
					return false;

				// PCSX2 owner: x86/microVU_Compile.inl keeps the micro PC in
				// compiler state and publishes it at block-management seams. Branch
				// lowering below consumes the compile-time post-increment PC, while a
				// branch-tail pair publishes its runtime-selected target itself.
				if (!EmitPublishPairTpc(m_plan.pair_count))
					return false;

				// PCSX2 owner: x86/microVU_Compile.inl keeps the decoded opcode in
				// compiler state and publishes architectural state at block-management
				// seams. No native op or stall helper observes VURegs::code between
				// pairs, so materialize its final value once before a dispatcher return
				// or direct link instead of emitting MOV+STR for every pair.
				// TPC follows the separate block-private publication contract above.
				if (!EmitPublishPairCode(m_plan.pair_count))
					return false;

				// Block-local vector mappings never cross an externally observable
				// seam. PCSX2 owner: x86/microVU_IR.h::microRegAlloc::flushAll().
				if (!EmitFlushVectorCache())
					return false;
				if (!EmitPublishResidentCycle())
					return false;

				// Fall-through: every pair executed. If the next block is
				// already cached, tail-call it with the accumulated count;
				// otherwise return to the dispatcher.
				if (m_plan.direct_link_tail && !EmitDirectLinkTail(m_plan.pair_count))
					return false;
				// PCSX2's microRegAlloc/pStateEnd keeps compatible block state in
				// host registers. A deferred-FMAC source reaches this point with the
				// current STATUS/MAC values in r6/r7; a compatible linked target owns
				// those values directly. Publish them only after the link fast path has
				// declined the edge, at the actual dispatcher/architectural seam.
				if (!EmitPublishDeferredFmacFlags())
					return false;
				if (!EmitReturnExecutedPairs(m_plan.pair_count,
						m_plan.continues_logical_block_if_busy))
					return false;
				const size_t epilogue_offset = m_code.Size();
				if (!EmitEpilogue())
					return false;
				if (!EmitEmptyPipeNopSlowPaths())
					return false;

				// Block-admission exit stubs: return the accumulated number of fully
				// executed pairs before entering an over-budget linked target.
				for (const BudgetExit& exit : m_budget_exits)
				{
					const size_t stub_offset = m_code.Size();
					if (!m_code.PatchBranch(exit.branch_site, stub_offset, exit.condition))
						return false;
					if (!EmitPublishPairTpc(exit.executed_pairs) ||
						!EmitPublishPairCode(exit.executed_pairs) ||
						!EmitPublishResidentCycle() ||
						!EmitPublishDeferredFmacFlags() ||
						!EmitReturnExecutedPairs(exit.executed_pairs))
						return false;
					const size_t jump = m_code.EmitBranchPlaceholder();
					if (jump == static_cast<size_t>(-1) ||
						!m_code.PatchBranch(jump, epilogue_offset))
					{
						return false;
					}
				}

				if (!EmitSharedTestPipesFastGuardThunk(false) ||
					!EmitSharedTestPipesFastGuardThunk(true) ||
					!EmitXgkickNormalizePreserveThunk())
					return false;

				if (m_vu0_memory_map)
					return true;

				// PCSX2 owner: x86/microVU_Branch.inl links compatible allocator
				// states without returning through the dispatcher. The outer VU1
				// execution wrapper gives every target the same private frame, so linked
				// chains retain r4/r6/r7/r10/r11 without a dispatcher round trip.
				// Both source representations receive an entry. A deferred source
				// entering a canonical target publishes r6/r7 and restores the saved
				// limit pair in EmitLinkedEntry(); the reverse transition installs
				// r6/r7 from canonical VURegs exactly as before.
				m_linked_entries.deferred_fmac = m_code.Size();
				if (!EmitLinkedEntry(body_offset, true, false))
					return false;
				if (UsesResidentPipeActivity())
				{
					m_linked_entries.resident_pipe_deferred_fmac = m_code.Size();
					if (!EmitLinkedEntry(body_offset, true, true))
						return false;
				}

				m_linked_entries.normal = m_code.Size();
				if (!EmitLinkedEntry(body_offset, false, false))
					return false;
				if (UsesResidentPipeActivity())
				{
					m_linked_entries.resident_pipe = m_code.Size();
					if (!EmitLinkedEntry(body_offset, false, true))
						return false;
				}

				return m_vector_cache_mode != VectorCacheMode::Enabled ||
					(m_vector_accesses && m_vector_access_cursor == m_vector_accesses->size());
			}

			const std::array<Vu1DirectLinkSlot, MAX_DIRECT_LINK_SLOTS>& DirectLinks() const { return m_direct_links; }
			const LinkedEntryOffsets& LinkedEntries() const { return m_linked_entries; }
			const VectorCacheStats& GetVectorCacheStats() const { return m_vector_cache_stats; }
			u32 GetNormalizedOperandQuadBypasses() const { return m_normalized_operand_quad_bypasses; }
			u32 GetNormalizationInstructionsRemoved() const { return m_normalization_instructions_removed; }
			u32 GetSingleDBroadcastOperands() const { return m_single_d_broadcast_operands; }

			static VectorCacheOpportunity AnalyzeVectorCacheOpportunity(
				std::vector<VectorAccessEvent>* events, bool outer_frame_already_required)
			{
				VectorCacheOpportunity opportunity;
				if (!events)
					return opportunity;

				struct Lifetime
				{
					u32 first = 0xffffffffu;
					u32 last = 0xffffffffu;
					u32 accesses = 0;
					bool needs_old_value = false;
					bool dirty = false;
					u32 canonical_bytes = 0;
				};
				std::array<Lifetime, VU_VECTOR_CACHE_ACC + 1> lifetimes{};
				const auto finish_lifetime = [&](u8 guest) {
					Lifetime& lifetime = lifetimes[guest];
					if (lifetime.accesses == 0)
						return;

					const u32 fixed_cost = (lifetime.needs_old_value ? 2u : 0u) +
						(lifetime.dirty ? 2u : 0u);
					const u32 fixed_bytes = (lifetime.needs_old_value ? 16u : 0u) +
						(lifetime.dirty ? 16u : 0u);
					if (lifetime.accesses > fixed_cost &&
						lifetime.canonical_bytes >= fixed_bytes)
					{
						opportunity.wrapper_instructions_removed +=
							lifetime.accesses - fixed_cost;
						opportunity.canonical_bytes_removed +=
							lifetime.canonical_bytes - fixed_bytes;
						for (u32 index = lifetime.first; index != 0xffffffffu;
							index = (*events)[index].next_same_guest)
						{
							(*events)[index].admit = true;
						}
					}
					lifetime = {};
				};
				const auto finish_all = [&]() {
					for (u8 guest = 1; guest <= VU_VECTOR_CACHE_ACC; guest++)
						finish_lifetime(guest);
				};

				for (size_t index = 0; index < events->size(); index++)
				{
					VectorAccessEvent& event = (*events)[index];
					event.admit = false;
					event.next_same_guest = 0xffffffffu;
					if (event.kind == VectorAccessKind::Barrier)
					{
						finish_all();
						continue;
					}
					if (event.guest == 0 || event.guest > VU_VECTOR_CACHE_ACC)
						continue;
					if (event.kind == VectorAccessKind::WordLoad ||
						event.kind == VectorAccessKind::WordStore)
					{
						finish_lifetime(event.guest);
						continue;
					}
					Lifetime& lifetime = lifetimes[event.guest];
					const u32 event_index = static_cast<u32>(index);
					if (lifetime.accesses == 0)
					{
						lifetime.first = event_index;
						lifetime.needs_old_value = event.needs_old_value;
					}
					else
					{
						(*events)[lifetime.last].next_same_guest = event_index;
					}
					lifetime.last = event_index;
					lifetime.accesses++;
					lifetime.dirty |= event.kind == VectorAccessKind::QuadStore;
					// A lane broadcast replaces one canonical 32-bit read, while
					// ordinary cache events replace a complete 128-bit transfer.
					// Do not admit a lane-heavy lifetime if its initial quad load
					// would increase total memory traffic.
					lifetime.canonical_bytes +=
						event.kind == VectorAccessKind::LaneBroadcast ? 4u : 16u;
				}
				finish_all();

				// ExecuteVu1Blocks() preserves D8-D15 once per complete native window,
				// not once per generated block. The first cached block in a program must
				// still repay that 128-byte outer frame. Once another block has selected
				// residency, admitting a later block has no additional frame cost: let
				// the exact baseline/candidate compile below accept every strict code and
				// canonical-memory reduction instead of charging the same frame again.
				opportunity.profitable = outer_frame_already_required ?
					(opportunity.wrapper_instructions_removed != 0 &&
						opportunity.canonical_bytes_removed != 0) :
					(opportunity.wrapper_instructions_removed >= 20 &&
						opportunity.canonical_bytes_removed > 128);
				if (!opportunity.profitable)
				{
					for (VectorAccessEvent& event : *events)
						event.admit = false;
				}
				return opportunity;
			}

		private:
				struct BudgetExit
				{
					size_t branch_site = 0;
					u32 executed_pairs = 0;
					Condition condition = Condition::CS;
				};

			bool EmitLinkedEntry(size_t body_offset, bool deferred_values_resident,
				bool pipe_aggregate_resident)
			{
				if (pipe_aggregate_resident && !UsesResidentPipeActivity())
					return false;

#if defined(VITASX2_QEMU_VALIDATION)
				u32* const entry_counter = deferred_values_resident ?
					&g_qemuVuJitDeferredFmacLinkedEntries : &g_qemuVuJitLinkedFrameEntries;
				if (!m_code.EmitMovImm32(0, static_cast<u32>(reinterpret_cast<uptr>(entry_counter))) ||
					!m_code.EmitLdrImm12(1, 0, 0) ||
					!m_code.EmitAddImm8(1, 1, 1) ||
					!m_code.EmitStrImm12(1, 0, 0))
				{
					return false;
				}
				if (pipe_aggregate_resident &&
					(!m_code.EmitMovImm32(0, static_cast<u32>(reinterpret_cast<uptr>(
							&g_qemuVuJitResidentPipeLinkedEntries))) ||
					 !m_code.EmitLdrImm12(1, 0, 0) ||
					 !m_code.EmitAddImm8(1, 1, 1) ||
					 !m_code.EmitStrImm12(1, 0, 0)))
				{
					return false;
				}
				if (!deferred_values_resident && UsesVectorCacheFrame() &&
					(!m_code.EmitMovImm32(0, static_cast<u32>(reinterpret_cast<uptr>(
							&g_qemuVuJitLinkedVectorFrameEntries))) ||
					 !m_code.EmitLdrImm12(1, 0, 0) ||
					 !m_code.EmitAddImm8(1, 1, 1) ||
					 !m_code.EmitStrImm12(1, 0, 0)))
				{
					return false;
				}
#endif

				if (!deferred_values_resident && m_plan.deferred_fmac_flags &&
					(!m_code.EmitStrdImm8(HOST_LIMIT_LO, HOST_LIMIT_HI, SP,
						static_cast<u8>(DEFERRED_LIMIT_SAVE_OFFSET)) ||
					 !m_code.EmitLdrImm12(HOST_LIMIT_LO, HOST_VU,
							ViOffset(REG_STATUS_FLAG)) ||
					 !m_code.EmitLdrImm12(HOST_LIMIT_HI, HOST_VU, ViOffset(REG_MAC_FLAG))))
				{
					return false;
				}
				if (deferred_values_resident && !m_plan.deferred_fmac_flags &&
					(!m_code.EmitStrImm12(HOST_LIMIT_LO, HOST_VU,
						ViOffset(REG_STATUS_FLAG)) ||
					 !m_code.EmitStrImm12(HOST_LIMIT_HI, HOST_VU,
						ViOffset(REG_MAC_FLAG)) ||
					 !m_code.EmitLdrdImm8(HOST_LIMIT_LO, HOST_LIMIT_HI, SP,
						static_cast<u8>(DEFERRED_LIMIT_SAVE_OFFSET))))
				{
					return false;
				}
				if (m_resident_cycle &&
					(!m_code.EmitLdrImm12(HOST_CYCLE_LO, HOST_VU,
							VuOffset(offsetof(VURegs, cycle))) ||
					 (m_resident_cycle_high &&
						 !m_code.EmitLdrImm12(HOST_CYCLE_HI, HOST_VU,
							 VuOffset(offsetof(VURegs, cycle) + 4)))))
				{
					return false;
				}
				// The source and target both opt into the same generated r10 ABI. The
				// source's canonicalization leaves its exact fmaccount in bits [2:0]
				// and the zero/nonzero aggregate of every other pipe above bit 7.
				// AAPCS preserves r10 across a runtime lookup, while patched links branch
				// directly, so compatible links need no five-load/four-ORR rebuild.
				if (!pipe_aggregate_resident && !EmitRefreshResidentPipeActivity())
					return false;

				const size_t linked_to_body = m_code.EmitBranchPlaceholder();
				return linked_to_body != static_cast<size_t>(-1) &&
					m_code.PatchBranch(linked_to_body, body_offset);
			}

			bool UsesResidentPipeActivity() const
			{
				return m_plan.resident_pipe_activity;
			}

			bool EmitRefreshResidentPipeActivity()
			{
				if (!UsesResidentPipeActivity())
					return true;

				// PCSX2 owner: VUops.cpp::_vuTestPipes() and microVU's mVUregs
				// pipeline state. Preserve exact fmaccount in low bits; shift every
				// other nonzero predicate above the four-entry count. The canonical
				// queues, timestamps, values, and publication order remain in VURegs.
				return m_code.EmitLdrImm12(0, HOST_VU,
						VuOffset(offsetof(VURegs, fmaccount))) &&
					m_code.EmitLdrImm12(1, HOST_VU,
						VuOffset(offsetof(VURegs, fdiv) + offsetof(fdivPipe, enable))) &&
					m_code.EmitLdrImm12(2, HOST_VU,
						VuOffset(offsetof(VURegs, efu) + offsetof(efuPipe, enable))) &&
					m_code.EmitLdrImm12(3, HOST_VU,
						VuOffset(offsetof(VURegs, ialucount))) &&
					m_code.EmitOrrRegShiftImm(0, 0, 1, ShiftType::LSL,
						RESIDENT_OTHER_PIPE_SHIFT) &&
					m_code.EmitOrrRegShiftImm(0, 0, 2, ShiftType::LSL,
						RESIDENT_OTHER_PIPE_SHIFT) &&
					m_code.EmitLdrImm12(1, HOST_VU,
						VuOffset(offsetof(VURegs, xgkickenable))) &&
					m_code.EmitOrrRegShiftImm(0, 0, 3, ShiftType::LSL,
						RESIDENT_OTHER_PIPE_SHIFT) &&
					m_code.EmitOrrRegShiftImm(HOST_STALL_SCRATCH, 0, 1,
						ShiftType::LSL, RESIDENT_OTHER_PIPE_SHIFT);
			}

			bool EmitMarkResidentPipeActivity(bool fmac_append = false)
			{
				if (!UsesResidentPipeActivity())
					return true;
				// Canonical FMAC append updates the packed low count itself. Other
				// creators need only preserve that count and mark a high activity bit;
				// the next publisher reconstructs the exact high aggregate.
				return fmac_append || m_code.EmitOrrImm32(HOST_STALL_SCRATCH,
					HOST_STALL_SCRATCH, RESIDENT_OTHER_PIPE_MARKER);
			}

			bool EmitPrologue()
			{
				if (!m_code.EmitPush(SAVED_REGISTER_MASK))
					return false;
				if (!UsesVectorCacheFrame())
				{
					if (!m_code.EmitSubImm8(SP, SP, STACK_FRAME_SIZE))
						return false;
				}
				else if (!m_code.EmitSubImm8(SP, SP, 4) ||
					!m_code.EmitVpushDRange(8, 8) ||
					!m_code.EmitSubImm8(SP, SP, VECTOR_STACK_FRAME_SIZE))
				{
					return false;
				}

				if (!EmitMovReg(HOST_VU, 0) ||
					!EmitMovReg(HOST_LIMIT_LO, 2) ||
					!EmitMovReg(HOST_LIMIT_HI, 3) ||
					!EmitMovReg(HOST_EXEC_BASE, 1))
				{
					return false;
				}
				if (m_plan.deferred_fmac_flags &&
					!m_code.EmitStrdImm8(HOST_LIMIT_LO, HOST_LIMIT_HI, SP,
						static_cast<u8>(DEFERRED_LIMIT_SAVE_OFFSET)))
				{
					return false;
				}
				if (m_plan.deferred_fmac_flags &&
					(!m_code.EmitLdrImm12(HOST_LIMIT_LO, HOST_VU,
							ViOffset(REG_STATUS_FLAG)) ||
					 !m_code.EmitLdrImm12(HOST_LIMIT_HI, HOST_VU, ViOffset(REG_MAC_FLAG))))
				{
					return false;
				}
				if (!EmitRefreshResidentPipeActivity())
					return false;

				if (!m_resident_cycle)
					return true;

				// If static analysis proves no stall/XGKICK path can advance
				// VU1.cycle beyond the modeled scheduling operations, keep its low
				// word in r5. Eligible local-FMAC blocks also keep the high word in
				// r9, forming an exact 64-bit r5:r9 cycle pair. Admission still uses
				// the exact 64-bit limit, so no redundant per-pair countdown is
				// required.
				return m_code.EmitLdrImm12(HOST_CYCLE_LO, HOST_VU,
						VuOffset(offsetof(VURegs, cycle))) &&
					(!m_resident_cycle_high ||
						m_code.EmitLdrImm12(HOST_CYCLE_HI, HOST_VU,
							VuOffset(offsetof(VURegs, cycle) + 4)));
			}

			bool EmitEpilogue()
			{
				if (!UsesVectorCacheFrame())
					return m_code.EmitAddImm8(SP, SP, STACK_FRAME_SIZE) &&
						m_code.EmitPop(RETURN_REGISTER_MASK);

				return m_code.EmitAddImm8(SP, SP, VECTOR_STACK_FRAME_SIZE) &&
					m_code.EmitVpopDRange(8, 8) &&
					m_code.EmitAddImm8(SP, SP, 4) &&
					m_code.EmitPop(RETURN_REGISTER_MASK);
			}

			bool UsesVectorCacheFrame() const
			{
				// ExecuteVu1Blocks() preserves AAPCS D8-D15 once around the complete
				// native execution window, and this translation unit reserves those
				// registers from C++ allocation. Generated blocks therefore share the
				// compact scalar private frame even when Q4-Q7 carry VF/ACC values.
				return false;
			}

			bool EmitMovReg(unsigned rd, unsigned rm, Condition condition = Condition::AL)
			{
				return m_code.EmitMovRegShiftImm(rd, rm, ShiftType::LSL, 0, false, condition);
			}

			bool EmitLoadCurrentCycleLow(unsigned rd)
			{
				// PCSX2 owner: x86/microVU_Compile.inl accumulates mVUcycles for a
				// whole analyzed block and publishes it only at scheduling seams. The
				// Vita emitter retains only blocks whose scan proves one cycle per
				// pair, so their current low word can remain in r5 as well.
				if (m_resident_cycle)
					return rd == HOST_CYCLE_LO || EmitMovReg(rd, HOST_CYCLE_LO);

				return m_code.EmitLdrImm12(rd, HOST_VU, VuOffset(offsetof(VURegs, cycle)));
			}

			bool EmitLoadCurrentCycleHigh(unsigned rd)
			{
				if (m_resident_cycle_high)
					return rd == HOST_CYCLE_HI || EmitMovReg(rd, HOST_CYCLE_HI);

				return m_code.EmitLdrImm12(rd, HOST_VU,
					VuOffset(offsetof(VURegs, cycle) + 4));
			}

			bool EmitPublishResidentCycle()
			{
				// Helper calls, dispatcher returns, and linked-entry admission are
				// the observable joins. Low-only residency keeps the high word
				// canonical on its rare carry path; full residency publishes both
				// halves of the exact r5:r9 pair at the join.
				return !m_resident_cycle ||
					(m_code.EmitStrImm12(HOST_CYCLE_LO, HOST_VU,
						VuOffset(offsetof(VURegs, cycle))) &&
					 (!m_resident_cycle_high ||
						 m_code.EmitStrImm12(HOST_CYCLE_HI, HOST_VU,
							 VuOffset(offsetof(VURegs, cycle) + 4))));
			}

			bool EmitResyncResidentCycle()
			{
				return !m_resident_cycle ||
					(m_code.EmitLdrImm12(HOST_CYCLE_LO, HOST_VU,
							VuOffset(offsetof(VURegs, cycle))) &&
					 (!m_resident_cycle_high ||
						 m_code.EmitLdrImm12(HOST_CYCLE_HI, HOST_VU,
							 VuOffset(offsetof(VURegs, cycle) + 4))));
			}

			bool EmitPublishPairCode(u32 executed_pairs)
			{
				if (executed_pairs == 0)
					return true;

				const PairPlan& last = m_pairs[executed_pairs - 1];
				const u32 final_code =
					(last.shape == PairShape::IBit) ? last.upper : last.lower;
				return m_code.EmitMovImm32(0, final_code) &&
					m_code.EmitStrImm12(0, HOST_VU, VuOffset(offsetof(VURegs, code)));
			}

			bool EmitPublishPairTpc(u32 executed_pairs)
			{
				if (executed_pairs == 0)
					return true;

				const PairPlan& last = m_pairs[executed_pairs - 1];
				// EmitBranchTail() publishes either the sequential PC or the exact
				// runtime-selected branch target, including branch-in-delay handoff.
				if (last.branch_tail)
					return true;

				return m_code.EmitMovImm32(0, last.pc + 8) &&
					m_code.EmitStrImm12(0, HOST_VU, ViOffset(REG_TPC));
			}

			bool EmitReturnExecutedPairs(u32 executed_pairs,
				bool logical_continuation = false)
			{
				const bool emitted_count = executed_pairs == 0 ?
					EmitMovReg(0, HOST_EXEC_BASE) :
					m_code.EmitAddImm8(0, HOST_EXEC_BASE,
						static_cast<u8>(executed_pairs));
				return emitted_count && (!logical_continuation ||
					m_code.EmitOrrImm32(0, 0, EXECUTED_PAIRS_LOGICAL_CONTINUATION));
			}

			bool EmitCallHelper(const void* fn)
			{
				return EmitMovReg(0, HOST_VU) && EmitCallAbsoluteClobberVectorState(fn);
			}

			bool EmitCallHelperRegs(const void* fn, const _VURegsNum* regs)
			{
				return EmitMovReg(0, HOST_VU) &&
					m_code.EmitMovImm32(1, static_cast<u32>(reinterpret_cast<uptr>(regs))) &&
					EmitCallAbsoluteClobberVectorState(fn);
			}

			bool EmitCallAbsoluteClobberVectorState(const void* fn)
			{
				// AAPCS32 makes D16-D31/Q8-Q15 caller-clobbered. Q8-Q10 cache
				// vuDouble() normalization constants across pairs, so every helper
				// seam must force a later normalize to rematerialize them. This is
				// required even for a conditional call: compile-time state joins the
				// called and skipped arms after the call site. Q4-Q7 are AAPCS
				// callee-saved. The helpers reachable here own pipe/GIF/INTC/link
				// state, not VF/ACC storage; D/T and E-bit observable joins publish
				// the block-local cache explicitly at their unconditional barriers.
				// Q11 can carry either replaceable MAC lane weights or sticky
				// categories from older, liveness-dead FMAC instances. Only the latter
				// are runtime state and must survive either arm of a conditional helper
				// call; an invalidated weight vector is simply rematerialized later.
				const bool preserve_dead_sticky = m_dead_fmac_sticky_pending;
				const bool emitted = EmitPublishResidentCycle() &&
					(!preserve_dead_sticky || m_code.EmitVpushDRange(22, 2)) &&
					m_code.EmitCallAbsolute(fn) &&
					(!preserve_dead_sticky || m_code.EmitVpopDRange(22, 2)) &&
					EmitResyncResidentCycle();
				m_norm_consts_ready = false;
				m_norm_maxf_ready = false;
				m_full_mac_weights_ready = false;
				return emitted;
			}

			bool EmitCallXgkickPreserveNormalizeState()
			{
				if (!m_norm_consts_ready && !m_dead_fmac_sticky_pending)
				{
					const bool emitted = m_code.EmitCallAbsolute(
						 reinterpret_cast<const void*>(&_vuXGKICKTransferMicroVU));
					m_norm_consts_ready = false;
					m_norm_maxf_ready = false;
					m_full_mac_weights_ready = false;
					return emitted;
				}

				return EmitCallXgkickAlwaysPreserveNormalizeState();
			}

			bool EmitCallXgkickAlwaysPreserveNormalizeState()
			{
				// PCSX2 owner: x86/microVU_Lower.inl::mVU_XGKICK_SYNC(). The
				// PATH1 helper owns GIF/XGKICK state, not the host-only vuDouble()
				// constants/weights/accumulators. Q8-Q11 are AAPCS caller-clobbered, so route the rare
				// taken edge through one block-local save/call/restore thunk. A local
				// BL also contracts every eligible hot call site from MOVW/MOVT/BLX
				// to one instruction. The skipped arm retains the constants without
				// executing any additional instruction.
				const size_t call_site = m_code.EmitBranchLinkPlaceholder();
				if (call_site == static_cast<size_t>(-1))
					return false;
				m_xgkick_norm_preserve_calls.push_back(call_site);
				return true;
			}

			bool EmitXgkickNormalizePreserveThunk()
			{
				if (m_xgkick_norm_preserve_calls.empty())
					return true;

				const size_t thunk_offset = m_code.Size();
				for (const size_t call_site : m_xgkick_norm_preserve_calls)
				{
					if (!m_code.PatchBranchLink(call_site, thunk_offset))
						return false;
				}

				// Preserve LR from the local BL and keep the AAPCS stack 8-byte
				// aligned across the C++ helper. r3 is caller-clobbered padding. Only
				// D16-D23 contain live block state; Q12-Q15 remain operation scratch.
				constexpr u16 THUNK_CORE_SAVE = (1u << 3) | (1u << 14);
				return m_code.EmitPush(THUNK_CORE_SAVE) &&
					m_code.EmitVpushDRange(16, 8) &&
					m_code.EmitCallAbsolute(reinterpret_cast<const void*>(&_vuXGKICKTransferMicroVU)) &&
					m_code.EmitVpopDRange(16, 8) &&
					m_code.EmitPop(THUNK_CORE_SAVE) &&
					m_code.EmitBx(14);
			}

			bool EmitDirectLinkTail(u32 executed_pairs)
			{
				// PCSX2 owner: x86/microVU_Branch.inl block manager links.
				// This conservative Vita link only jumps to blocks already in
				// the quick map, so generated code never compiles or resets the
				// code cache while another generated block is executing.
				Vu1DirectLinkSlot* runtime_link = nullptr;
				for (u32 slot_index = 0; slot_index < MAX_DIRECT_LINK_SLOTS; slot_index++)
				{
					const DirectLinkPlan& plan = m_plan.direct_links[slot_index];
					if (!plan.valid)
						continue;

					Vu1DirectLinkSlot& link = m_direct_links[slot_index];
					link.valid = true;
					link.runtime_observed = plan.runtime_observed;
					link.observed_target = !plan.runtime_observed;
					link.guard_tpc = plan.guard_tpc;
					link.guard_tpc_value = plan.guard_tpc_value;
					link.target_pc = plan.target_pc;
					link.target_branch_tail = plan.target_branch_tail;
					link.target_ebit_tail = plan.target_ebit_tail;
					link.unlinked_fallback_offset = m_code.Size();
					link.target_offset = static_cast<size_t>(-1);
					link.fallback_offset = static_cast<size_t>(-1);
					link.guard_tpc_offset = static_cast<size_t>(-1);
					if (plan.runtime_observed && !runtime_link)
						runtime_link = &link;

					const size_t skip_fast = m_code.EmitBranchPlaceholder();
					if (skip_fast == static_cast<size_t>(-1))
						return false;

					size_t skip_guard = static_cast<size_t>(-1);
					if (plan.guard_tpc)
					{
						if (!m_code.EmitLdrImm12(0, HOST_VU, ViOffset(REG_TPC)))
							return false;

						if (plan.runtime_observed)
						{
							link.guard_tpc_offset = m_code.Size();
							if (!m_code.EmitMovImm32Patchable(1, plan.guard_tpc_value) ||
								!m_code.EmitCmpReg(0, 1))
							{
								return false;
							}
						}
						else if (!m_code.EmitCmpImm32(0, plan.guard_tpc_value))
						{
							link.guard_tpc_offset = m_code.Size();
							if (!m_code.EmitMovImm32Patchable(1, plan.guard_tpc_value) ||
								!m_code.EmitCmpReg(0, 1))
							{
								return false;
							}
						}

						skip_guard = m_code.EmitBranchPlaceholder(Condition::NE);
						if (skip_guard == static_cast<size_t>(-1))
							return false;
					}

					if (!m_code.EmitAddImm8(HOST_EXEC_BASE, HOST_EXEC_BASE,
							static_cast<u8>(executed_pairs)))
						return false;

					link.target_offset = m_code.Size();
					const size_t target_branch = m_code.EmitBranchPlaceholder();
					if (target_branch == static_cast<size_t>(-1))
						return false;

					link.fallback_offset = m_code.Size();
					if (!m_code.PatchBranch(skip_fast, link.fallback_offset) ||
						!m_code.PatchBranch(target_branch, link.fallback_offset))
					{
						return false;
					}
					if (skip_guard != static_cast<size_t>(-1) &&
						!m_code.PatchBranch(skip_guard, link.fallback_offset, Condition::NE))
					{
						return false;
					}
				}

				if (!EmitMovReg(0, HOST_VU))
					return false;
				if (runtime_link)
				{
					runtime_link->helper_slot_offset = m_code.Size();
					if (!m_code.EmitMovImm32Patchable(1, 0))
						return false;
				}
				else if (!m_code.EmitMovImm8(1, 0))
				{
					return false;
				}

				const void* lookup = nullptr;
				if (m_plan.deferred_fmac_flags)
				{
					if (UsesResidentPipeActivity())
					{
						lookup = UsesVectorCacheFrame() ?
							reinterpret_cast<const void*>(&LookupVu1DirectLinkBlockVectorDeferredFmacResidentPipe) :
							reinterpret_cast<const void*>(&LookupVu1DirectLinkBlockScalarDeferredFmacResidentPipe);
					}
					else
					{
						lookup = UsesVectorCacheFrame() ?
							reinterpret_cast<const void*>(&LookupVu1DirectLinkBlockVectorDeferredFmac) :
							reinterpret_cast<const void*>(&LookupVu1DirectLinkBlockScalarDeferredFmac);
					}
				}
				else
				{
					if (UsesResidentPipeActivity())
					{
						lookup = UsesVectorCacheFrame() ?
							reinterpret_cast<const void*>(&LookupVu1DirectLinkBlockVectorResidentPipe) :
							reinterpret_cast<const void*>(&LookupVu1DirectLinkBlockScalarResidentPipe);
					}
					else
					{
						lookup = UsesVectorCacheFrame() ?
							reinterpret_cast<const void*>(&LookupVu1DirectLinkBlockVector) :
							reinterpret_cast<const void*>(&LookupVu1DirectLinkBlockScalar);
					}
				}
				if (!EmitCallAbsoluteClobberVectorState(lookup) ||
					!m_code.EmitCmpImm32(0, 0))
				{
					return false;
				}

				const size_t skip = m_code.EmitBranchPlaceholder(Condition::EQ);
				if (skip == static_cast<size_t>(-1))
					return false;

				if (!EmitMovReg(HOST_CALL_SCRATCH, 0) ||
					!m_code.EmitAddImm8(HOST_EXEC_BASE, HOST_EXEC_BASE,
						static_cast<u8>(executed_pairs)) ||
					!m_code.EmitBx(HOST_CALL_SCRATCH))
				{
					return false;
				}

				return m_code.PatchBranch(skip, m_code.Size(), Condition::EQ);
			}

			bool EmitCallXgkickTransferFlush()
			{
				return m_code.EmitMovImm8(0, 0) &&
					m_code.EmitMovImm8(1, 1) &&
					EmitPublishResidentCycle() &&
					EmitCallXgkickPreserveNormalizeState() &&
					EmitResyncResidentCycle();
			}

			bool EmitOrVuWordField(unsigned accum, size_t offset)
			{
				return m_code.EmitLdrImm12(1, HOST_VU, VuOffset(offset)) &&
					m_code.EmitOrrReg(accum, accum, 1);
			}

#if defined(VITASX2_QEMU_VALIDATION)
			bool EmitQemuCanonicalDeferredFmacRetirementCounter()
			{
				return m_code.EmitMovImm32(0, static_cast<u32>(reinterpret_cast<uptr>(
						&g_qemuVuJitCanonicalDeferredFmacRetirements))) &&
					m_code.EmitLdrImm12(1, 0, 0) &&
					m_code.EmitAddImm8(1, 1, 1) &&
					m_code.EmitStrImm12(1, 0, 0);
			}

			bool EmitQemuLocalFmacPipelineCounters()
			{
				if (!m_plan.local_fmac_pipeline)
					return true;
				const bool local_counts =
					m_code.EmitMovImm32(0, static_cast<u32>(reinterpret_cast<uptr>(
						&g_qemuVuJitLocalFmacPipelineEntries))) &&
					m_code.EmitLdrImm12(1, 0, 0) &&
					m_code.EmitAddImm8(1, 1, 1) &&
					m_code.EmitStrImm12(1, 0, 0) &&
					m_code.EmitMovImm32(0, static_cast<u32>(reinterpret_cast<uptr>(
						&g_qemuVuJitLocalFmacPipelineCommits))) &&
					m_code.EmitLdrImm12(1, 0, 0) &&
					m_code.EmitAddImm8(1, 1,
						static_cast<u8>(m_plan.local_fmac_pipeline_pairs)) &&
					m_code.EmitStrImm12(1, 0, 0);
				if (!local_counts)
					return false;
				if (m_plan.local_fmac_producer_snapshot_pairs != 0 &&
					(!m_code.EmitMovImm32(0, static_cast<u32>(reinterpret_cast<uptr>(
						&g_qemuVuJitLocalFmacProducerSnapshotEntries))) ||
					 !m_code.EmitLdrImm12(1, 0, 0) ||
					 !m_code.EmitAddImm8(1, 1,
						 static_cast<u8>(m_plan.local_fmac_producer_snapshot_pairs)) ||
					 !m_code.EmitStrImm12(1, 0, 0)))
				{
					return false;
				}
				if (m_plan.local_fmac_cycle_snapshot_elision_pairs != 0 &&
					(!m_code.EmitMovImm32(0, static_cast<u32>(reinterpret_cast<uptr>(
						&g_qemuVuJitLocalFmacCycleSnapshotElisions))) ||
					 !m_code.EmitLdrImm12(1, 0, 0) ||
					 !m_code.EmitAddImm8(1, 1,
						 static_cast<u8>(m_plan.local_fmac_cycle_snapshot_elision_pairs)) ||
					 !m_code.EmitStrImm12(1, 0, 0)))
				{
					return false;
				}
				if (!m_plan.deferred_fmac_flags)
					return local_counts;

				const bool deferred_counts = m_code.EmitMovImm32(0, static_cast<u32>(reinterpret_cast<uptr>(
						&g_qemuVuJitDeferredFmacFlagEntries))) &&
					m_code.EmitLdrImm12(1, 0, 0) &&
					m_code.EmitAddImm8(1, 1, 1) &&
					m_code.EmitStrImm12(1, 0, 0) &&
					m_code.EmitMovImm32(0, static_cast<u32>(reinterpret_cast<uptr>(
						&g_qemuVuJitDeferredFmacFlagRetirements))) &&
					m_code.EmitLdrImm12(1, 0, 0) &&
					m_code.EmitAddImm8(1, 1,
						static_cast<u8>(m_plan.deferred_fmac_flag_retirements)) &&
					m_code.EmitStrImm12(1, 0, 0);
				if (!deferred_counts || m_plan.deferred_fmac_compact_retirements == 0)
					return deferred_counts;
				return m_code.EmitMovImm32(0, static_cast<u32>(reinterpret_cast<uptr>(
						&g_qemuVuJitDeferredFmacCompactRetirements))) &&
					m_code.EmitLdrImm12(1, 0, 0) &&
					m_code.EmitAddImm8(1, 1,
						static_cast<u8>(m_plan.deferred_fmac_compact_retirements)) &&
					m_code.EmitStrImm12(1, 0, 0) &&
					m_code.EmitMovImm32(0, static_cast<u32>(reinterpret_cast<uptr>(
						&g_qemuVuJitDeferredFmacCompactInstructionsRemoved))) &&
					m_code.EmitLdrImm12(1, 0, 0) &&
					m_code.EmitAddImm8(1, 1,
						static_cast<u8>(m_plan.deferred_fmac_compact_instructions_removed)) &&
					m_code.EmitStrImm12(1, 0, 0);
			}

				bool EmitQemuEbitFinishInlineCounter()
				{
					return m_code.EmitMovImm32(0, static_cast<u32>(reinterpret_cast<uptr>(&g_qemuVuJitEbitFinishInlineOps))) &&
						m_code.EmitLdrImm12(1, 0, 0) &&
						m_code.EmitAddImm8(1, 1, 1) &&
						m_code.EmitStrImm12(1, 0, 0);
				}

				bool EmitQemuTestPipesFastSkipCounter()
				{
					return m_code.EmitMovImm32(0, static_cast<u32>(reinterpret_cast<uptr>(&g_qemuVuJitTestPipesFastSkips))) &&
						m_code.EmitLdrImm12(1, 0, 0) &&
						m_code.EmitAddImm8(1, 1, 1) &&
						m_code.EmitStrImm12(1, 0, 0);
				}

				bool EmitQemuResidentPipeAggregateRefreshCounter()
				{
					return m_code.EmitMovImm32(0, static_cast<u32>(reinterpret_cast<uptr>(
							&g_qemuVuJitResidentPipeAggregateRefreshes))) &&
						m_code.EmitLdrImm12(1, 0, 0) &&
						m_code.EmitAddImm8(1, 1, 1) &&
						m_code.EmitStrImm12(1, 0, 0);
				}

				bool EmitQemuResidentPipeAggregateXgkickCounter()
				{
					return m_code.EmitMovImm32(0, static_cast<u32>(reinterpret_cast<uptr>(
							&g_qemuVuJitResidentPipeAggregateXgkickCalls))) &&
						m_code.EmitLdrImm12(1, 0, 0) &&
						m_code.EmitAddImm8(1, 1, 1) &&
						m_code.EmitStrImm12(1, 0, 0);
				}

				bool EmitQemuTestPipesIaluFlushInlineCounter()
				{
					return m_code.EmitMovImm32(0, static_cast<u32>(reinterpret_cast<uptr>(&g_qemuVuJitTestPipesIaluFlushInlineOps))) &&
						m_code.EmitLdrImm12(1, 0, 0) &&
						m_code.EmitAddImm8(1, 1, 1) &&
						m_code.EmitStrImm12(1, 0, 0);
				}

				bool EmitQemuTestPipesFmacFlushInlineCounter(bool resident_queue)
				{
					if (!m_code.EmitMovImm32(0, static_cast<u32>(reinterpret_cast<uptr>(&g_qemuVuJitTestPipesFmacFlushInlineOps))) ||
						!m_code.EmitLdrImm12(1, 0, 0) ||
						!m_code.EmitAddImm8(1, 1, 1) ||
						!m_code.EmitStrImm12(1, 0, 0))
					{
						return false;
					}
					if (!resident_queue)
						return true;
					return m_code.EmitMovImm32(0, static_cast<u32>(reinterpret_cast<uptr>(
							&g_qemuVuJitResidentFmacQueueRetirements))) &&
						m_code.EmitLdrImm12(1, 0, 0) &&
						m_code.EmitAddImm8(1, 1, 1) &&
						m_code.EmitStrImm12(1, 0, 0);
				}

				bool EmitQemuTestPipesFdivFlushInlineCounter()
				{
					return m_code.EmitMovImm32(0, static_cast<u32>(reinterpret_cast<uptr>(&g_qemuVuJitTestPipesFdivFlushInlineOps))) &&
						m_code.EmitLdrImm12(1, 0, 0) &&
						m_code.EmitAddImm8(1, 1, 1) &&
						m_code.EmitStrImm12(1, 0, 0);
				}

				bool EmitQemuTestPipesEfuFlushInlineCounter()
				{
					return m_code.EmitMovImm32(0, static_cast<u32>(reinterpret_cast<uptr>(&g_qemuVuJitTestPipesEfuFlushInlineOps))) &&
						m_code.EmitLdrImm12(1, 0, 0) &&
						m_code.EmitAddImm8(1, 1, 1) &&
						m_code.EmitStrImm12(1, 0, 0);
				}

				bool EmitQemuNormConstantMaterializationCounter()
				{
					return m_code.EmitMovImm32(0, static_cast<u32>(reinterpret_cast<uptr>(&g_qemuVuJitNormConstantMaterializations))) &&
						m_code.EmitLdrImm12(1, 0, 0) &&
						m_code.EmitAddImm8(1, 1, 1) &&
						m_code.EmitStrImm12(1, 0, 0);
				}

				bool EmitQemuTestPipesXgkickTransferInlineCounter()
				{
					return m_code.EmitMovImm32(0, static_cast<u32>(reinterpret_cast<uptr>(&g_qemuVuJitTestPipesXgkickTransferInlineOps))) &&
						m_code.EmitLdrImm12(1, 0, 0) &&
						m_code.EmitAddImm8(1, 1, 1) &&
						m_code.EmitStrImm12(1, 0, 0);
				}

				bool EmitQemuNopPipeTestDeferralCounter()
				{
					return m_code.EmitMovImm32(0, static_cast<u32>(reinterpret_cast<uptr>(
							&g_qemuVuJitNopPipeTestDeferrals))) &&
						m_code.EmitLdrImm12(1, 0, 0) &&
						m_code.EmitAddImm8(1, 1, 1) &&
						m_code.EmitStrImm12(1, 0, 0);
				}

				bool EmitQemuEmptyPipeNopBatchCounters(u8 pairs)
				{
					return m_code.EmitMovImm32(0, static_cast<u32>(reinterpret_cast<uptr>(
							&g_qemuVuJitEmptyPipeNopBatchRuns))) &&
						m_code.EmitLdrImm12(1, 0, 0) &&
						m_code.EmitAddImm8(1, 1, 1) &&
						m_code.EmitStrImm12(1, 0, 0) &&
						m_code.EmitMovImm32(0, static_cast<u32>(reinterpret_cast<uptr>(
							&g_qemuVuJitEmptyPipeNopBatchPairs))) &&
						m_code.EmitLdrImm12(1, 0, 0) &&
						m_code.EmitAddImm8(1, 1, pairs) &&
						m_code.EmitStrImm12(1, 0, 0);
				}

				bool EmitQemuAllPipesEmptyFastSkipCounter(
					Condition condition = Condition::AL)
				{
					return m_code.EmitMovImm32(0, static_cast<u32>(reinterpret_cast<uptr>(
							&g_qemuVuJitAllPipesEmptyFastSkips)), condition) &&
						m_code.EmitLdrImm12(1, 0, 0, condition) &&
						m_code.EmitAddImm8(1, 1, 1, false, condition) &&
						m_code.EmitStrImm12(1, 0, 0, condition);
				}

			bool EmitQemuFmacClearInlineCounter()
			{
				return m_code.EmitMovImm32(0, static_cast<u32>(reinterpret_cast<uptr>(&g_qemuVuJitFmacClearInlineOps))) &&
					m_code.EmitLdrImm12(1, 0, 0) &&
					m_code.EmitAddImm8(1, 1, 1) &&
					m_code.EmitStrImm12(1, 0, 0);
			}

			bool EmitQemuFmacWriteposLoadElisionCounter()
			{
				return m_code.EmitMovImm32(0, static_cast<u32>(reinterpret_cast<uptr>(
						&g_qemuVuJitFmacWriteposLoadElisions))) &&
					m_code.EmitLdrImm12(1, 0, 0) &&
					m_code.EmitAddImm8(1, 1, 1) &&
					m_code.EmitStrImm12(1, 0, 0);
			}

			bool EmitQemuResidentFmacCountAppendElisionCounter()
			{
				return m_code.EmitMovImm32(0, static_cast<u32>(reinterpret_cast<uptr>(
						&g_qemuVuJitResidentFmacCountAppendElisions))) &&
					m_code.EmitLdrImm12(1, 0, 0) &&
					m_code.EmitAddImm8(1, 1, 1) &&
					m_code.EmitStrImm12(1, 0, 0);
			}

			bool EmitQemuUpperFmacStallTestInlineCounter()
			{
				return m_code.EmitMovImm32(0, static_cast<u32>(reinterpret_cast<uptr>(&g_qemuVuJitUpperFmacStallTestInlineOps))) &&
					m_code.EmitLdrImm12(1, 0, 0) &&
					m_code.EmitAddImm8(1, 1, 1) &&
					m_code.EmitStrImm12(1, 0, 0);
			}

			bool EmitQemuCanonicalFmacStallTestElisionCounter()
			{
				return m_code.EmitMovImm32(0, static_cast<u32>(reinterpret_cast<uptr>(
						&g_qemuVuJitCanonicalFmacStallTestRuntimeElisions))) &&
					m_code.EmitLdrImm12(1, 0, 0) &&
					m_code.EmitAddImm8(1, 1, 1) &&
					m_code.EmitStrImm12(1, 0, 0);
			}

			bool EmitQemuLowerFmacStallTestInlineCounter()
			{
				return m_code.EmitMovImm32(0, static_cast<u32>(reinterpret_cast<uptr>(&g_qemuVuJitLowerFmacStallTestInlineOps))) &&
					m_code.EmitLdrImm12(1, 0, 0) &&
					m_code.EmitAddImm8(1, 1, 1) &&
					m_code.EmitStrImm12(1, 0, 0);
			}

			bool EmitQemuUpperFmacStallInlineCounter()
			{
				return m_code.EmitMovImm32(0, static_cast<u32>(reinterpret_cast<uptr>(&g_qemuVuJitUpperFmacStallInlineOps))) &&
					m_code.EmitLdrImm12(1, 0, 0) &&
					m_code.EmitAddImm8(1, 1, 1) &&
					m_code.EmitStrImm12(1, 0, 0);
			}

			bool EmitQemuLowerFmacStallInlineCounter()
			{
				return m_code.EmitMovImm32(0, static_cast<u32>(reinterpret_cast<uptr>(&g_qemuVuJitLowerFmacStallInlineOps))) &&
					m_code.EmitLdrImm12(1, 0, 0) &&
					m_code.EmitAddImm8(1, 1, 1) &&
					m_code.EmitStrImm12(1, 0, 0);
			}

			bool EmitQemuUpperMulInlineCounter()
			{
				return m_code.EmitMovImm32(0, static_cast<u32>(reinterpret_cast<uptr>(&g_qemuVuJitUpperMulInlineOps))) &&
					m_code.EmitLdrImm12(1, 0, 0) &&
					m_code.EmitAddImm8(1, 1, 1) &&
					m_code.EmitStrImm12(1, 0, 0);
			}

			bool EmitQemuUpperAddSubInlineCounter()
			{
				return m_code.EmitMovImm32(0, static_cast<u32>(reinterpret_cast<uptr>(&g_qemuVuJitUpperAddSubInlineOps))) &&
					m_code.EmitLdrImm12(1, 0, 0) &&
					m_code.EmitAddImm8(1, 1, 1) &&
					m_code.EmitStrImm12(1, 0, 0);
			}

			bool EmitQemuUpperMaddMsubInlineCounter()
			{
				return m_code.EmitMovImm32(0, static_cast<u32>(reinterpret_cast<uptr>(&g_qemuVuJitUpperMaddMsubInlineOps))) &&
					m_code.EmitLdrImm12(1, 0, 0) &&
					m_code.EmitAddImm8(1, 1, 1) &&
					m_code.EmitStrImm12(1, 0, 0);
			}

			bool EmitQemuUpperOuterInlineCounter()
			{
				return m_code.EmitMovImm32(0, static_cast<u32>(reinterpret_cast<uptr>(&g_qemuVuJitUpperOuterInlineOps))) &&
					m_code.EmitLdrImm12(1, 0, 0) &&
					m_code.EmitAddImm8(1, 1, 1) &&
					m_code.EmitStrImm12(1, 0, 0);
			}

			bool EmitQemuUpperNopInlineCounter()
			{
				return m_code.EmitMovImm32(0, static_cast<u32>(reinterpret_cast<uptr>(&g_qemuVuJitUpperNopInlineOps))) &&
					m_code.EmitLdrImm12(1, 0, 0) &&
					m_code.EmitAddImm8(1, 1, 1) &&
					m_code.EmitStrImm12(1, 0, 0);
			}

			bool EmitQemuUpperUnaryInlineCounter()
			{
				return m_code.EmitMovImm32(0, static_cast<u32>(reinterpret_cast<uptr>(&g_qemuVuJitUpperUnaryInlineOps))) &&
					m_code.EmitLdrImm12(1, 0, 0) &&
					m_code.EmitAddImm8(1, 1, 1) &&
					m_code.EmitStrImm12(1, 0, 0);
			}

			bool EmitQemuUpperClipInlineCounter()
			{
				return m_code.EmitMovImm32(0, static_cast<u32>(reinterpret_cast<uptr>(&g_qemuVuJitUpperClipInlineOps))) &&
					m_code.EmitLdrImm12(1, 0, 0) &&
					m_code.EmitAddImm8(1, 1, 1) &&
					m_code.EmitStrImm12(1, 0, 0);
			}

			bool EmitQemuUpperMinMaxInlineCounter()
			{
				return m_code.EmitMovImm32(0, static_cast<u32>(reinterpret_cast<uptr>(&g_qemuVuJitUpperMinMaxInlineOps))) &&
					m_code.EmitLdrImm12(1, 0, 0) &&
					m_code.EmitAddImm8(1, 1, 1) &&
					m_code.EmitStrImm12(1, 0, 0);
			}

			bool EmitQemuLowerIaluInlineCounter()
			{
				return m_code.EmitMovImm32(0, static_cast<u32>(reinterpret_cast<uptr>(&g_qemuVuJitLowerIaluInlineOps))) &&
					m_code.EmitLdrImm12(1, 0, 0) &&
					m_code.EmitAddImm8(1, 1, 1) &&
					m_code.EmitStrImm12(1, 0, 0);
			}

			bool EmitQemuLowerFlagInlineCounter()
			{
				return m_code.EmitMovImm32(0, static_cast<u32>(reinterpret_cast<uptr>(&g_qemuVuJitLowerFlagInlineOps))) &&
					m_code.EmitLdrImm12(1, 0, 0) &&
					m_code.EmitAddImm8(1, 1, 1) &&
					m_code.EmitStrImm12(1, 0, 0);
			}

			bool EmitQemuLowerMoveInlineCounter()
			{
				return m_code.EmitMovImm32(0, static_cast<u32>(reinterpret_cast<uptr>(&g_qemuVuJitLowerMoveInlineOps))) &&
					m_code.EmitLdrImm12(1, 0, 0) &&
					m_code.EmitAddImm8(1, 1, 1) &&
					m_code.EmitStrImm12(1, 0, 0);
			}

			bool EmitQemuLowerLsuInlineCounter()
			{
				return m_code.EmitMovImm32(0, static_cast<u32>(reinterpret_cast<uptr>(&g_qemuVuJitLowerLsuInlineOps))) &&
					m_code.EmitLdrImm12(1, 0, 0) &&
					m_code.EmitAddImm8(1, 1, 1) &&
					m_code.EmitStrImm12(1, 0, 0);
			}

			bool EmitQemuLowerControlInlineCounter()
			{
				return m_code.EmitMovImm32(0, static_cast<u32>(reinterpret_cast<uptr>(&g_qemuVuJitLowerControlInlineOps))) &&
					m_code.EmitLdrImm12(1, 0, 0) &&
					m_code.EmitAddImm8(1, 1, 1) &&
					m_code.EmitStrImm12(1, 0, 0);
			}

			bool EmitQemuLowerBranchInlineCounter()
			{
				return m_code.EmitMovImm32(0, static_cast<u32>(reinterpret_cast<uptr>(&g_qemuVuJitLowerBranchInlineOps))) &&
					m_code.EmitLdrImm12(1, 0, 0) &&
					m_code.EmitAddImm8(1, 1, 1) &&
					m_code.EmitStrImm12(1, 0, 0);
			}

			bool EmitQemuLowerFdivInlineCounter()
			{
				return m_code.EmitMovImm32(0, static_cast<u32>(reinterpret_cast<uptr>(&g_qemuVuJitLowerFdivInlineOps))) &&
					m_code.EmitLdrImm12(1, 0, 0) &&
					m_code.EmitAddImm8(1, 1, 1) &&
					m_code.EmitStrImm12(1, 0, 0);
			}

			bool EmitQemuLowerEfuInlineCounter()
			{
				return m_code.EmitMovImm32(0, static_cast<u32>(reinterpret_cast<uptr>(&g_qemuVuJitLowerEfuInlineOps))) &&
					m_code.EmitLdrImm12(1, 0, 0) &&
					m_code.EmitAddImm8(1, 1, 1) &&
					m_code.EmitStrImm12(1, 0, 0);
			}

			bool EmitQemuLowerXgkickInlineCounter()
			{
				return m_code.EmitMovImm32(0, static_cast<u32>(reinterpret_cast<uptr>(&g_qemuVuJitLowerXgkickInlineOps))) &&
					m_code.EmitLdrImm12(1, 0, 0) &&
					m_code.EmitAddImm8(1, 1, 1) &&
					m_code.EmitStrImm12(1, 0, 0);
			}

			bool EmitQemuLowerFdivStallTestInlineCounter()
			{
				return m_code.EmitMovImm32(0, static_cast<u32>(reinterpret_cast<uptr>(&g_qemuVuJitLowerFdivStallTestInlineOps))) &&
					m_code.EmitLdrImm12(1, 0, 0) &&
					m_code.EmitAddImm8(1, 1, 1) &&
					m_code.EmitStrImm12(1, 0, 0);
			}

			bool EmitQemuLowerEfuStallTestInlineCounter()
			{
				return m_code.EmitMovImm32(0, static_cast<u32>(reinterpret_cast<uptr>(&g_qemuVuJitLowerEfuStallTestInlineOps))) &&
					m_code.EmitLdrImm12(1, 0, 0) &&
					m_code.EmitAddImm8(1, 1, 1) &&
					m_code.EmitStrImm12(1, 0, 0);
			}

			bool EmitQemuLowerBranchStallTestInlineCounter()
			{
				return m_code.EmitMovImm32(0, static_cast<u32>(reinterpret_cast<uptr>(&g_qemuVuJitLowerBranchStallTestInlineOps))) &&
					m_code.EmitLdrImm12(1, 0, 0) &&
					m_code.EmitAddImm8(1, 1, 1) &&
					m_code.EmitStrImm12(1, 0, 0);
			}

			bool EmitQemuLowerStallInlineCounter()
			{
				return m_code.EmitMovImm32(0, static_cast<u32>(reinterpret_cast<uptr>(&g_qemuVuJitLowerStallInlineOps))) &&
					m_code.EmitLdrImm12(1, 0, 0) &&
					m_code.EmitAddImm8(1, 1, 1) &&
					m_code.EmitStrImm12(1, 0, 0);
			}

			bool EmitQemuDtFlagInlineCounter()
			{
				return m_code.EmitMovImm32(0, static_cast<u32>(reinterpret_cast<uptr>(&g_qemuVuJitDtFlagInlineOps))) &&
					m_code.EmitLdrImm12(1, 0, 0) &&
					m_code.EmitAddImm8(1, 1, 1) &&
					m_code.EmitStrImm12(1, 0, 0);
			}

			bool EmitQemuNormalizedOperandBypassCounter()
			{
				u32* const counter = m_vu0_memory_map ?
					&g_qemuVu0JitNormalizedOperandQuadBypasses :
					&g_qemuVuJitNormalizedOperandQuadBypasses;
				return m_code.EmitMovImm32(0, static_cast<u32>(reinterpret_cast<uptr>(counter))) &&
					m_code.EmitLdrImm12(1, 0, 0) &&
					m_code.EmitAddImm8(1, 1, 1) &&
					m_code.EmitStrImm12(1, 0, 0);
			}
#endif

			bool EmitInlineTestPipesFmacFlush(bool shared_thunk,
				bool deferred_fmac_flags, unsigned current_cycle_low)
			{
				// PCSX2 owner: VUops.cpp::_vuFMACflush(). Publish all ready
				// FMAC flag snapshots in queue order, stopping at the first
				// not-ready head entry exactly like the helper. PCSX2 microVU's
				// microVU_Flags.inl::mVUsetFlags() models the same four flag
				// instances at a fixed four-cycle latency. A full interpreter
				// queue therefore has an unconditionally ready head: at most one
				// merged FMAC entry is committed per pair, so four distinct prior
				// pairs have elapsed. Keep the timestamp path for partial queues
				// and for any additional entries made ready by a stall.
				constexpr unsigned HOST_INDEX = 0;
				constexpr unsigned HOST_PTR = 1;
				constexpr unsigned HOST_TEMP = 2;
				constexpr unsigned HOST_VALUE = 3;
				constexpr unsigned HOST_MASK_SCRATCH = HOST_CALL_SCRATCH;
				constexpr size_t FMAC_ARRAY_OFFSET = offsetof(VURegs, fmac);
				static_assert(FMAC_ARRAY_OFFSET + offsetof(fmacPipe, clipflag) < 4096);
				// This publisher only loads fields from the indexed entry. Keep the
				// fixed array offset in each LDR and form VU + index*48 in two A32
				// shifted ADDs: index*3 followed by (index*3)<<4.
				const auto emit_slot_offset_base = [&]() {
					return m_code.EmitAddRegShiftImm(HOST_PTR, HOST_INDEX, HOST_INDEX,
							ShiftType::LSL, 1) &&
						m_code.EmitAddRegShiftImm(HOST_PTR, HOST_VU, HOST_PTR,
							ShiftType::LSL, 4);
				};
				// PCSX2's _vuFMACflush() keeps fmaccount and its queue iterator in
				// local variables. A normal shared thunk preserves LR in r10 and uses
				// r14 for the count. A resident-activity thunk leaves LR in r14 and
				// uses callee-saved r10 for both the count and the aggregate returned
				// by the other pipe publishers.
				const unsigned host_count = shared_thunk ?
					(UsesResidentPipeActivity() ? HOST_STALL_SCRATCH : 14) :
					HOST_CALL_SCRATCH;

				const bool resident_count = shared_thunk && UsesResidentPipeActivity();
				const size_t loop_start = m_code.Size();
				// The resident count extraction's Z result is the empty predicate.
				// ANDS supplies it directly; the canonical load path still needs CMP.
				if (!(resident_count ?
						m_code.EmitAndImm32(host_count, host_count,
							RESIDENT_FMAC_COUNT_MASK, true) :
						(m_code.EmitLdrImm12(host_count, HOST_VU,
							 VuOffset(offsetof(VURegs, fmaccount))) &&
						 m_code.EmitCmpImm32(host_count, 0))))
				{
					return false;
				}
				const size_t done_empty = m_code.EmitBranchPlaceholder(Condition::EQ);
				if (done_empty == static_cast<size_t>(-1))
					return false;

				if (!m_code.EmitLdrImm12(HOST_INDEX, HOST_VU,
						VuOffset(offsetof(VURegs, fmacreadpos))) ||
					!emit_slot_offset_base())
				{
					return false;
				}
				const size_t resident_loop_start = m_code.Size();

				size_t ready_full = static_cast<size_t>(-1);
				if (shared_thunk)
				{
					if (!m_code.EmitCmpImm32(host_count, 4))
						return false;
					ready_full = m_code.EmitBranchPlaceholder(Condition::EQ);
					if (ready_full == static_cast<size_t>(-1))
						return false;
				}

				// Only the initial queue check can observe the full four entries.
				// Every loop back follows SUBS count,#1 and the zero exit, so its
				// surviving count is exactly 1..3 and must use the timestamp test.
				// Re-enter there directly instead of executing a dead CMP #4/BEQ.
				const size_t timestamp_check_start = m_code.Size();
				if (!m_code.EmitLdrImm12(HOST_TEMP, HOST_PTR,
						VuOffset(FMAC_ARRAY_OFFSET + offsetof(fmacPipe, sCycle))) ||
					!m_code.EmitLdrImm12(HOST_VALUE, HOST_PTR,
						VuOffset(FMAC_ARRAY_OFFSET + offsetof(fmacPipe, sCycle) + 4)) ||
					!m_code.EmitSubReg(HOST_TEMP, current_cycle_low, HOST_TEMP, true) ||
					!m_code.EmitSbcReg(HOST_VALUE, HOST_CLIP_NEW, HOST_VALUE, true) ||
					!m_code.EmitCmpImm32(HOST_VALUE, 0))
				{
					return false;
				}

				const size_t ready_high = m_code.EmitBranchPlaceholder(Condition::NE);
				if (ready_high == static_cast<size_t>(-1) ||
					// Sony VU User Manual 3.4.4 fixes every FMAC operation at four
					// cycles, and PCSX2's sole producer, _vuAddFMACStalls(), writes 4
					// unconditionally. Keep that word in canonical fmacPipe state for
					// helpers/oracle inspection, but do not reload the invariant on A9.
					!m_code.EmitCmpImm32(HOST_TEMP, FMAC_PIPELINE_LATENCY_CYCLES))
				{
					return false;
				}
				const size_t done_not_ready = m_code.EmitBranchPlaceholder(Condition::CC);
				if (done_not_ready == static_cast<size_t>(-1))
					return false;

				const size_t ready_target = m_code.Size();
				if ((ready_full != static_cast<size_t>(-1) &&
						!m_code.PatchBranch(ready_full, ready_target, Condition::EQ)) ||
					!m_code.PatchBranch(ready_high, ready_target, Condition::NE) ||
					!m_code.EmitLdrImm12(HOST_TEMP, HOST_PTR,
						VuOffset(FMAC_ARRAY_OFFSET + offsetof(fmacPipe, flagreg))) ||
					!m_code.EmitTstImm32(HOST_TEMP, 1u << REG_CLIP_FLAG))
				{
					return false;
				}
				const size_t skip_clip = m_code.EmitBranchPlaceholder(Condition::EQ);
				if (skip_clip == static_cast<size_t>(-1))
					return false;
				if (!m_code.EmitLdrImm12(HOST_VALUE, HOST_PTR,
						VuOffset(FMAC_ARRAY_OFFSET + offsetof(fmacPipe, clipflag))) ||
					!m_code.EmitStrImm12(HOST_VALUE, HOST_VU, ViOffset(REG_CLIP_FLAG)) ||
					!m_code.PatchBranch(skip_clip, m_code.Size(), Condition::EQ))
				{
					return false;
				}

				if (!m_code.EmitTstImm32(HOST_TEMP, 1u << REG_STATUS_FLAG))
					return false;
				const size_t no_sticky_status = m_code.EmitBranchPlaceholder(Condition::EQ);
				if (no_sticky_status == static_cast<size_t>(-1))
					return false;

				const unsigned status_reg = deferred_fmac_flags ? HOST_LIMIT_LO : HOST_VALUE;
				if ((!deferred_fmac_flags &&
						!m_code.EmitLdrImm12(status_reg, HOST_VU, ViOffset(REG_STATUS_FLAG))) ||
					!EmitAndRegImm32(status_reg, status_reg, 0x30u, HOST_MASK_SCRATCH) ||
					!m_code.EmitLdrImm12(HOST_TEMP, HOST_PTR,
						VuOffset(FMAC_ARRAY_OFFSET + offsetof(fmacPipe, statusflag))) ||
					// Preserve HOST_INDEX from the queue-head address calculation. The
					// exact sticky result is (old & 0x30) | (snapshot & 0x0fcf), so the
					// dead snapshot temporary can hold both snapshot fields at once.
					!EmitAndRegImm32(HOST_TEMP, HOST_TEMP, 0x0fcfu, HOST_MASK_SCRATCH) ||
					!m_code.EmitOrrReg(status_reg, status_reg, HOST_TEMP) ||
					(!deferred_fmac_flags &&
						!m_code.EmitStrImm12(status_reg, HOST_VU, ViOffset(REG_STATUS_FLAG))))
				{
					return false;
				}
				const size_t status_done = m_code.EmitBranchPlaceholder();
				if (status_done == static_cast<size_t>(-1))
					return false;

				const size_t no_sticky_target = m_code.Size();
				if (!m_code.PatchBranch(no_sticky_status, no_sticky_target, Condition::EQ) ||
					(!deferred_fmac_flags &&
						!m_code.EmitLdrImm12(status_reg, HOST_VU, ViOffset(REG_STATUS_FLAG))) ||
					!EmitAndRegImm32(status_reg, status_reg, 0x0ff0u, HOST_MASK_SCRATCH) ||
					!m_code.EmitLdrImm12(HOST_TEMP, HOST_PTR,
						VuOffset(FMAC_ARRAY_OFFSET + offsetof(fmacPipe, statusflag))) ||
					!EmitAndRegImm32(HOST_TEMP, HOST_TEMP, 0x0fu, HOST_MASK_SCRATCH) ||
					!m_code.EmitOrrReg(status_reg, status_reg, HOST_TEMP) ||
					!m_code.EmitOrrRegShiftImm(status_reg, status_reg, HOST_TEMP, ShiftType::LSL, 6) ||
					(!deferred_fmac_flags &&
						!m_code.EmitStrImm12(status_reg, HOST_VU, ViOffset(REG_STATUS_FLAG))))
				{
					return false;
				}

				const size_t after_status = m_code.Size();
				const unsigned mac_reg = deferred_fmac_flags ? HOST_LIMIT_HI : HOST_VALUE;
				if (!m_code.PatchBranch(status_done, after_status) ||
					!m_code.EmitLdrImm12(mac_reg, HOST_PTR,
						VuOffset(FMAC_ARRAY_OFFSET + offsetof(fmacPipe, macflag))) ||
					(!deferred_fmac_flags &&
						!m_code.EmitStrImm12(mac_reg, HOST_VU, ViOffset(REG_MAC_FLAG))) ||
					// STATUS packing deliberately preserves the queue-head index loaded
					// before the readiness test, avoiding a second VU-state load here.
					!m_code.EmitAddImm8(HOST_INDEX, HOST_INDEX, 1) ||
					!m_code.EmitAndImm32(HOST_INDEX, HOST_INDEX, 3) ||
					!m_code.EmitStrImm12(HOST_INDEX, HOST_VU, VuOffset(offsetof(VURegs, fmacreadpos))))
				{
					return false;
				}
#if defined(VITASX2_QEMU_VALIDATION)
				if (!EmitQemuTestPipesFmacFlushInlineCounter(shared_thunk) ||
					(deferred_fmac_flags &&
						!EmitQemuCanonicalDeferredFmacRetirementCounter()))
					return false;
				// The validation-only counter uses r0/r1. Restore the resident
				// iterator needed by the shared-thunk loop; product code emits
				// neither the counter nor this diagnostic reload.
				if (shared_thunk &&
					!m_code.EmitLdrImm12(HOST_INDEX, HOST_VU,
						VuOffset(offsetof(VURegs, fmacreadpos))))
				{
					return false;
				}
#endif

				size_t done_after_retire = static_cast<size_t>(-1);
				if (shared_thunk)
				{
					// SUBS supplies the exact post-retirement empty predicate. STR does
					// not change NZCV, so the following BEQ consumes it directly.
					if (!m_code.EmitSubImm8(host_count, host_count, 1, true) ||
						!m_code.EmitStrImm12(host_count, HOST_VU,
							VuOffset(offsetof(VURegs, fmaccount))))
						return false;
					done_after_retire = m_code.EmitBranchPlaceholder(Condition::EQ);
					if (done_after_retire == static_cast<size_t>(-1) ||
						!emit_slot_offset_base())
					{
						return false;
					}
				}
				else if (!m_code.EmitLdrImm12(host_count, HOST_VU,
						VuOffset(offsetof(VURegs, fmaccount))) ||
					!m_code.EmitSubImm8(host_count, host_count, 1) ||
					!m_code.EmitStrImm12(host_count, HOST_VU,
						VuOffset(offsetof(VURegs, fmaccount))))
				{
					return false;
				}

				const size_t loop_jump = m_code.EmitBranchPlaceholder();
				if (loop_jump == static_cast<size_t>(-1) ||
					!m_code.PatchBranch(loop_jump,
						shared_thunk ? timestamp_check_start : loop_start))
				{
					return false;
				}

				const size_t done_target = m_code.Size();
				return m_code.PatchBranch(done_empty, done_target, Condition::EQ) &&
					m_code.PatchBranch(done_not_ready, done_target, Condition::CC) &&
					(done_after_retire == static_cast<size_t>(-1) ||
						m_code.PatchBranch(done_after_retire, done_target, Condition::EQ));
			}

				bool EmitInlineTestPipesFdivFlush(bool deferred_fmac_flags,
					unsigned current_cycle_low)
				{
				// PCSX2 owner: VUops.cpp::_vuFDIVflush(). FDIV publication
				// exposes Q and FDIV-owned D/I status bits when the pipe latency
				// has elapsed; it does not change VU->cycle.
				constexpr size_t base = offsetof(VURegs, fdiv);
				if (!m_code.EmitLdrImm12(0, HOST_VU, VuOffset(base + offsetof(fdivPipe, enable))) ||
					!m_code.EmitCmpImm32(0, 0))
				{
					return false;
				}
				const size_t done_disabled = m_code.EmitBranchPlaceholder(Condition::EQ);
				if (done_disabled == static_cast<size_t>(-1))
					return false;

				if (!m_code.EmitLdrImm12(2, HOST_VU, VuOffset(base + offsetof(fdivPipe, sCycle))) ||
					!m_code.EmitLdrImm12(3, HOST_VU, VuOffset(base + offsetof(fdivPipe, sCycle) + 4)) ||
					!m_code.EmitSubReg(2, current_cycle_low, 2, true) ||
					!m_code.EmitSbcReg(3, HOST_CLIP_NEW, 3, true) ||
					!m_code.EmitCmpImm32(3, 0))
				{
					return false;
				}
				const size_t ready_high = m_code.EmitBranchPlaceholder(Condition::NE);
				if (ready_high == static_cast<size_t>(-1) ||
					!m_code.EmitLdrImm12(3, HOST_VU, VuOffset(base + offsetof(fdivPipe, Cycle))) ||
					!m_code.EmitCmpReg(2, 3))
				{
					return false;
				}
				const size_t done_not_ready = m_code.EmitBranchPlaceholder(Condition::CC);
				if (done_not_ready == static_cast<size_t>(-1))
					return false;

				const size_t ready_target = m_code.Size();
				if (!m_code.PatchBranch(ready_high, ready_target, Condition::NE) ||
					!m_code.EmitMovImm8(0, 0) ||
					!m_code.EmitStrImm12(0, HOST_VU, VuOffset(base + offsetof(fdivPipe, enable))) ||
					!m_code.EmitLdrImm12(1, HOST_VU, VuOffset(base + offsetof(fdivPipe, reg))) ||
					!m_code.EmitStrImm12(1, HOST_VU, ViOffset(REG_Q)))
				{
					return false;
				}

				// Keep r0 equal to the exact post-publisher enable value for the
				// resident aggregate. r2 is dead after the latency comparison.
				const unsigned status_reg = deferred_fmac_flags ? HOST_LIMIT_LO : 2;
				if ((!deferred_fmac_flags &&
						!m_code.EmitLdrImm12(status_reg, HOST_VU, ViOffset(REG_STATUS_FLAG))) ||
					!EmitAndRegImm32(status_reg, status_reg, 0x0fcfu, HOST_CALL_SCRATCH) ||
					!m_code.EmitLdrImm12(1, HOST_VU, VuOffset(base + offsetof(fdivPipe, statusflag))) ||
					!EmitAndRegImm32(1, 1, 0x0c30u, HOST_CALL_SCRATCH) ||
					!m_code.EmitOrrReg(status_reg, status_reg, 1) ||
					(!deferred_fmac_flags &&
						!m_code.EmitStrImm12(status_reg, HOST_VU, ViOffset(REG_STATUS_FLAG))))
				{
					return false;
				}

#if defined(VITASX2_QEMU_VALIDATION)
				if (!EmitQemuTestPipesFdivFlushInlineCounter())
					return false;
#endif

				const size_t done_target = m_code.Size();
				return m_code.PatchBranch(done_disabled, done_target, Condition::EQ) &&
					m_code.PatchBranch(done_not_ready, done_target, Condition::CC);
				}

				bool EmitInlineTestPipesEfuFlush(unsigned current_cycle_low)
				{
				// PCSX2 owner: VUops.cpp::_vuEFUflush(). EFU publication exposes
				// P when the latency has elapsed; it does not change VU->cycle.
				constexpr size_t base = offsetof(VURegs, efu);
				if (!m_code.EmitLdrImm12(0, HOST_VU, VuOffset(base + offsetof(efuPipe, enable))) ||
					!m_code.EmitCmpImm32(0, 0))
				{
					return false;
				}
				const size_t done_disabled = m_code.EmitBranchPlaceholder(Condition::EQ);
				if (done_disabled == static_cast<size_t>(-1))
					return false;

				if (!m_code.EmitLdrImm12(2, HOST_VU, VuOffset(base + offsetof(efuPipe, sCycle))) ||
					!m_code.EmitLdrImm12(3, HOST_VU, VuOffset(base + offsetof(efuPipe, sCycle) + 4)) ||
					!m_code.EmitSubReg(2, current_cycle_low, 2, true) ||
					!m_code.EmitSbcReg(3, HOST_CLIP_NEW, 3, true) ||
					!m_code.EmitCmpImm32(3, 0))
				{
					return false;
				}
				const size_t ready_high = m_code.EmitBranchPlaceholder(Condition::NE);
				if (ready_high == static_cast<size_t>(-1) ||
					!m_code.EmitLdrImm12(3, HOST_VU, VuOffset(base + offsetof(efuPipe, Cycle))) ||
					!m_code.EmitCmpReg(2, 3))
				{
					return false;
				}
				const size_t done_not_ready = m_code.EmitBranchPlaceholder(Condition::CC);
				if (done_not_ready == static_cast<size_t>(-1))
					return false;

				const size_t ready_target = m_code.Size();
				if (!m_code.PatchBranch(ready_high, ready_target, Condition::NE) ||
					!m_code.EmitMovImm8(0, 0) ||
					!m_code.EmitStrImm12(0, HOST_VU, VuOffset(base + offsetof(efuPipe, enable))) ||
					!m_code.EmitLdrImm12(1, HOST_VU, VuOffset(base + offsetof(efuPipe, reg))) ||
					!m_code.EmitStrImm12(1, HOST_VU, ViOffset(REG_P)))
				{
					return false;
				}

#if defined(VITASX2_QEMU_VALIDATION)
				if (!EmitQemuTestPipesEfuFlushInlineCounter())
					return false;
#endif

				const size_t done_target = m_code.Size();
				return m_code.PatchBranch(done_disabled, done_target, Condition::EQ) &&
					m_code.PatchBranch(done_not_ready, done_target, Condition::CC);
				}

				bool EmitInlineTestPipesIaluFlush(unsigned current_cycle_low)
				{
				// PCSX2 owner: VUops.cpp::_vuIALUflush(). This path is reached
				// only after FMAC/FDIV/EFU have been proven not publishable, so
				// ready IALU entries can be consumed without calling _vuTestPipes().
				constexpr unsigned HOST_INDEX = 0;
				constexpr unsigned HOST_PTR = 1;
				constexpr unsigned HOST_DIFF_LO = 2;
				constexpr unsigned HOST_DIFF_HI = 3;
				constexpr unsigned HOST_COUNT = HOST_CALL_SCRATCH;

				const size_t loop_start = m_code.Size();
				if (!m_code.EmitLdrImm12(HOST_COUNT, HOST_VU, VuOffset(offsetof(VURegs, ialucount))) ||
					!m_code.EmitCmpImm32(HOST_COUNT, 0))
				{
					return false;
				}
				const size_t done_empty = m_code.EmitBranchPlaceholder(Condition::EQ);
				if (done_empty == static_cast<size_t>(-1))
					return false;

				if (!m_code.EmitLdrImm12(HOST_INDEX, HOST_VU, VuOffset(offsetof(VURegs, ialureadpos))) ||
					!m_code.EmitAddImm32(HOST_PTR, HOST_VU, offsetof(VURegs, ialu)) ||
					!m_code.EmitAddRegShiftImm(HOST_PTR, HOST_PTR, HOST_INDEX, ShiftType::LSL, 4) ||
					!m_code.EmitAddRegShiftImm(HOST_PTR, HOST_PTR, HOST_INDEX, ShiftType::LSL, 3) ||
					!m_code.EmitLdrImm12(HOST_DIFF_LO, HOST_PTR, offsetof(ialuPipe, sCycle)) ||
					!m_code.EmitLdrImm12(HOST_DIFF_HI, HOST_PTR, offsetof(ialuPipe, sCycle) + 4) ||
					!m_code.EmitSubReg(HOST_DIFF_LO, current_cycle_low, HOST_DIFF_LO, true) ||
					!m_code.EmitSbcReg(HOST_DIFF_HI, HOST_CLIP_NEW, HOST_DIFF_HI, true) ||
					!m_code.EmitCmpImm32(HOST_DIFF_HI, 0))
				{
					return false;
				}

				const size_t ready_high = m_code.EmitBranchPlaceholder(Condition::NE);
				if (ready_high == static_cast<size_t>(-1) ||
					!m_code.EmitLdrImm12(HOST_DIFF_HI, HOST_PTR, offsetof(ialuPipe, Cycle)) ||
					!m_code.EmitCmpReg(HOST_DIFF_LO, HOST_DIFF_HI))
				{
					return false;
				}
				const size_t done_not_ready = m_code.EmitBranchPlaceholder(Condition::CC);
				if (done_not_ready == static_cast<size_t>(-1))
					return false;

				const size_t ready_target = m_code.Size();
				if (!m_code.PatchBranch(ready_high, ready_target, Condition::NE) ||
					!m_code.EmitAddImm8(HOST_INDEX, HOST_INDEX, 1) ||
					!m_code.EmitAndImm32(HOST_INDEX, HOST_INDEX, 3) ||
					!m_code.EmitStrImm12(HOST_INDEX, HOST_VU, VuOffset(offsetof(VURegs, ialureadpos))) ||
					!m_code.EmitSubImm8(HOST_COUNT, HOST_COUNT, 1) ||
					!m_code.EmitStrImm12(HOST_COUNT, HOST_VU, VuOffset(offsetof(VURegs, ialucount))))
				{
					return false;
				}

#if defined(VITASX2_QEMU_VALIDATION)
				if (!EmitQemuTestPipesIaluFlushInlineCounter())
					return false;
#endif

				const size_t loop_jump = m_code.EmitBranchPlaceholder();
				if (loop_jump == static_cast<size_t>(-1) ||
					!m_code.PatchBranch(loop_jump, loop_start))
				{
					return false;
				}

				const size_t done_target = m_code.Size();
					return m_code.PatchBranch(done_empty, done_target, Condition::EQ) &&
						m_code.PatchBranch(done_not_ready, done_target, Condition::CC);
				}

				bool EmitInlineTestPipesXgkickTransfer(bool always_preserve_normalize_state,
					bool preserve_resident_aggregate, unsigned current_cycle_low)
				{
					// PCSX2 owners: VUops.cpp::_vuTestPipes() XGKICK arm and
					// x86/microVU_Lower.inl::mVU_XGKICK_DELAY(). Generated A32
					// computes the scheduling delta; the microVU helper performs one
					// complete EOP-bounded PATH1 transfer. VU0 has no XGKICK arm.
					if (m_vu0_memory_map)
						return true;

					if (!m_code.EmitLdrImm12(0, HOST_VU, VuOffset(offsetof(VURegs, xgkickenable))) ||
						!m_code.EmitCmpImm32(0, 0))
					{
						return false;
					}
					const size_t done_disabled = m_code.EmitBranchPlaceholder(Condition::EQ);
					if (done_disabled == static_cast<size_t>(-1))
						return false;

					if ((preserve_resident_aggregate &&
							!m_code.EmitStrImm12(14, SP, XGKICK_THUNK_LR_SAVE_OFFSET)) ||
						!m_code.EmitLdrImm12(1, HOST_VU, VuOffset(offsetof(VURegs, xgkicklastcycle))) ||
						!m_code.EmitSubReg(0, current_cycle_low, 1) ||
						!m_code.EmitSubImm8(0, 0, 1) ||
						!m_code.EmitMovImm8(1, 0) ||
						!(always_preserve_normalize_state ?
							EmitCallXgkickAlwaysPreserveNormalizeState() :
							EmitCallXgkickPreserveNormalizeState()))
					{
						return false;
					}

#if defined(VITASX2_QEMU_VALIDATION)
					if (!EmitQemuTestPipesXgkickTransferInlineCounter())
						return false;
					if (preserve_resident_aggregate &&
						!EmitQemuResidentPipeAggregateXgkickCounter())
					{
						return false;
					}
#endif

					if (preserve_resident_aggregate &&
						(!m_code.EmitLdrImm12(14, SP, XGKICK_THUNK_LR_SAVE_OFFSET) ||
						 !m_code.EmitLdrImm12(0, HOST_VU,
							 VuOffset(offsetof(VURegs, xgkickenable)))))
					{
						return false;
					}

					return m_code.PatchBranch(done_disabled, m_code.Size(), Condition::EQ);
				}

				bool EmitTestPipesFastGuardBody(bool shared_thunk, bool deferred_fmac_flags)
				{
					// PCSX2 owner: VUops.cpp::_vuTestPipes(). The generated path
					// handles FMAC, FDIV, EFU, IALU, then XGKICK in helper order.
					// The publishers only read the exact current cycle. Scan-admitted
					// resident blocks can therefore consume r5 directly instead of
					// copying it to the otherwise conventional CLIP-backup register.
					const unsigned current_cycle_low =
						m_resident_cycle ? HOST_CYCLE_LO : HOST_CLIP_OLD;
					if (!EmitLoadCurrentCycleLow(current_cycle_low) ||
						!EmitLoadCurrentCycleHigh(HOST_CLIP_NEW))
					{
						return false;
					}

					const bool resident_aggregate = shared_thunk && UsesResidentPipeActivity();
					if (!EmitInlineTestPipesFmacFlush(shared_thunk, deferred_fmac_flags,
						current_cycle_low))
						return false;
					if (!EmitInlineTestPipesFdivFlush(deferred_fmac_flags, current_cycle_low))
						return false;
					if (resident_aggregate)
					{
#if defined(VITASX2_QEMU_VALIDATION)
						if (!m_code.EmitLdrImm12(0, HOST_VU, VuOffset(FDIV_ENABLE_OFFSET)))
							return false;
#endif
						if (!m_code.EmitOrrRegShiftImm(HOST_STALL_SCRATCH,
							HOST_STALL_SCRATCH, 0, ShiftType::LSL,
							RESIDENT_OTHER_PIPE_SHIFT))
							return false;
					}
					if (!EmitInlineTestPipesEfuFlush(current_cycle_low))
						return false;
					if (resident_aggregate)
					{
#if defined(VITASX2_QEMU_VALIDATION)
						if (!m_code.EmitLdrImm12(0, HOST_VU, VuOffset(EFU_ENABLE_OFFSET)))
							return false;
#endif
						if (!m_code.EmitOrrRegShiftImm(HOST_STALL_SCRATCH,
							HOST_STALL_SCRATCH, 0, ShiftType::LSL,
							RESIDENT_OTHER_PIPE_SHIFT))
							return false;
					}
					if (!EmitInlineTestPipesIaluFlush(current_cycle_low))
						return false;
					if (resident_aggregate &&
						!m_code.EmitOrrRegShiftImm(HOST_STALL_SCRATCH,
							HOST_STALL_SCRATCH, HOST_CALL_SCRATCH, ShiftType::LSL,
							RESIDENT_OTHER_PIPE_SHIFT))
					{
						return false;
					}
					if (!EmitInlineTestPipesXgkickTransfer(shared_thunk,
						resident_aggregate, current_cycle_low))
						return false;
					if (resident_aggregate && !m_vu0_memory_map &&
						!m_code.EmitOrrRegShiftImm(HOST_STALL_SCRATCH,
							HOST_STALL_SCRATCH, 0, ShiftType::LSL,
							RESIDENT_OTHER_PIPE_SHIFT))
					{
						return false;
					}
#if defined(VITASX2_QEMU_VALIDATION)
					if (!EmitQemuTestPipesFastSkipCounter())
						return false;
					if (resident_aggregate &&
						!EmitQemuResidentPipeAggregateRefreshCounter())
					{
						return false;
					}
#endif

					return resident_aggregate ? true : EmitRefreshResidentPipeActivity();
				}

				bool EmitTestPipesFastGuard(bool deferred_fmac_flags)
				{
					// PCSX2 microVU keeps pipeline scheduling outside individual
					// opcode bodies. On Cortex-A9, duplicating this large exact
					// interpreter-state publisher in every pair streams far more code
					// than the 32 KiB L1 I-cache can retain. Multi-pair blocks call one
					// block-local copy; single-pair blocks remain inline.
					// PCSX2's interpreter-side _vu1CanFastForwardPlainNopPairs() owns
					// the exact aggregate-empty predicate. Test its five independent
					// words in parallel before entering the publisher; ORRS supplies the
					// branch condition without a separate CMP. This makes the common
					// no-pipeline path ten straight-line A32 instructions and avoids the
					// shared thunk, five queue arms, and return entirely.
					if (UsesResidentPipeActivity())
					{
						// The exact publisher refreshes this private aggregate whenever it
						// runs, and every canonical in-block pipe creation marks it. The
						// overwhelmingly common local-FMAC steady state therefore tests the
						// callee-saved aggregate directly instead of loading even hot stack.
						if (!m_code.EmitCmpImm32(HOST_STALL_SCRATCH, 0))
						{
							return false;
						}

#if defined(VITASX2_QEMU_VALIDATION)
						// These predicated instructions do not set NZCV, so BLNE below
						// remains controlled by the resident-aggregate CMP.
						if (!EmitQemuAllPipesEmptyFastSkipCounter(Condition::EQ))
							return false;
#endif

						const size_t call_site =
							m_code.EmitBranchLinkPlaceholder(Condition::NE);
						if (call_site == static_cast<size_t>(-1))
							return false;
						(deferred_fmac_flags ? m_deferred_test_pipes_fast_guard_calls :
							m_test_pipes_fast_guard_calls).push_back(call_site);
						return true;
					}

					if (!m_code.EmitLdrImm12(0, HOST_VU, VuOffset(offsetof(VURegs, fmaccount))) ||
						!m_code.EmitLdrImm12(1, HOST_VU,
							VuOffset(offsetof(VURegs, fdiv) + offsetof(fdivPipe, enable))) ||
						!m_code.EmitLdrImm12(2, HOST_VU,
							VuOffset(offsetof(VURegs, efu) + offsetof(efuPipe, enable))) ||
						!m_code.EmitLdrImm12(3, HOST_VU, VuOffset(offsetof(VURegs, ialucount))) ||
						!m_code.EmitLdrImm12(HOST_CALL_SCRATCH, HOST_VU,
							VuOffset(offsetof(VURegs, xgkickenable))) ||
						!m_code.EmitOrrReg(0, 0, 1) ||
						!m_code.EmitOrrReg(2, 2, 3) ||
						!m_code.EmitOrrReg(0, 0, 2) ||
						!m_code.EmitOrrReg(0, 0, HOST_CALL_SCRATCH, true))
					{
						return false;
					}
					const size_t empty = m_code.EmitBranchPlaceholder(Condition::EQ);
					if (empty == static_cast<size_t>(-1))
						return false;

					bool emitted_body = false;
					if (m_plan.pair_count < 2)
					{
						emitted_body = EmitTestPipesFastGuardBody(false, deferred_fmac_flags);
					}
					else
					{
						const size_t call_site = m_code.EmitBranchLinkPlaceholder();
						if (call_site != static_cast<size_t>(-1))
						{
							(deferred_fmac_flags ? m_deferred_test_pipes_fast_guard_calls :
								m_test_pipes_fast_guard_calls).push_back(call_site);
							emitted_body = true;
						}
					}
					if (!emitted_body)
						return false;

#if defined(VITASX2_QEMU_VALIDATION)
					const size_t done = m_code.EmitBranchPlaceholder();
					if (done == static_cast<size_t>(-1) ||
						!m_code.PatchBranch(empty, m_code.Size(), Condition::EQ) ||
						!EmitQemuAllPipesEmptyFastSkipCounter())
					{
						return false;
					}
					return m_code.PatchBranch(done, m_code.Size());
#else
					return m_code.PatchBranch(empty, m_code.Size(), Condition::EQ);
#endif
				}

				bool EmitDeferredNopPipeTest(bool deferred_fmac_flags)
				{
					// Publishing FMAC/FDIV/EFU/IALU state may move to the next empty
					// pair because no VU instruction can observe it there. XGKICK is
					// different: PATH1 transfer progress is device-visible each cycle,
					// so retain the exact VUops.cpp path whenever it is active.
					if (!m_code.EmitLdrImm12(0, HOST_VU,
							VuOffset(offsetof(VURegs, xgkickenable))) ||
						!m_code.EmitCmpImm32(0, 0))
					{
						return false;
					}

					const size_t defer = m_code.EmitBranchPlaceholder(Condition::EQ);
					if (defer == static_cast<size_t>(-1) ||
						!EmitTestPipesFastGuard(deferred_fmac_flags))
					{
						return false;
					}

#if defined(VITASX2_QEMU_VALIDATION)
					const size_t done = m_code.EmitBranchPlaceholder();
					if (done == static_cast<size_t>(-1) ||
						!m_code.PatchBranch(defer, m_code.Size(), Condition::EQ) ||
						!EmitQemuNopPipeTestDeferralCounter())
					{
						return false;
					}
					return m_code.PatchBranch(done, m_code.Size());
#else
					return m_code.PatchBranch(defer, m_code.Size(), Condition::EQ);
#endif
				}

				bool EmitSharedTestPipesFastGuardThunk(bool deferred_fmac_flags)
				{
					std::vector<size_t>& calls = deferred_fmac_flags ?
						m_deferred_test_pipes_fast_guard_calls : m_test_pipes_fast_guard_calls;
					if (calls.empty())
						return true;

					const size_t thunk_offset = m_code.Size();
					const Condition call_condition = UsesResidentPipeActivity() ?
						Condition::NE : Condition::AL;
					for (const size_t call_site : calls)
					{
						if (!m_code.PatchBranchLink(call_site, thunk_offset,
							call_condition))
							return false;
					}

					// r10 is dead after each pair's stall tests and is callee-saved
					// across the only possible C++ call
					// (_vuXGKICKTransferMicroVU). The resident ABI keeps LR in r14
					// and returns the exact post-publisher pipe aggregate in r10. The
					// legacy ABI instead moves LR to r10 so r14 can hold fmaccount.
					if (UsesResidentPipeActivity())
					{
						return EmitTestPipesFastGuardBody(true, deferred_fmac_flags) &&
							m_code.EmitBx(14);
					}
					return EmitMovReg(HOST_STALL_SCRATCH, 14) &&
						EmitTestPipesFastGuardBody(true, deferred_fmac_flags) &&
						m_code.EmitBx(HOST_STALL_SCRATCH);
				}

			bool EmitLoadViHalfword(unsigned rd, unsigned reg)
			{
				if (reg == 0)
					return m_code.EmitMovImm8(rd, 0);

				return EmitLoadViHalfwordRaw(rd, reg);
			}

			bool EmitLoadViHalfwordRaw(unsigned rd, unsigned reg)
			{
				// PCSX2 microVU_Flags.inl keeps delayed STATUS/MAC instances in
				// allocator registers until an actual architectural seam. Deferred-flag
				// admission has already proved that a flag observer sees every older
				// four-cycle instance retired before its pair executes, so r6/r7 are
				// the exact VI flag values here. UXTH preserves the interpreter's
				// REG_VI::US[0] read even if a seam supplied nonzero upper bits.
				if (m_plan.deferred_fmac_flags &&
					(reg == REG_STATUS_FLAG || reg == REG_MAC_FLAG))
				{
					return m_code.EmitUxth(rd,
						reg == REG_STATUS_FLAG ? HOST_LIMIT_LO : HOST_LIMIT_HI);
				}

				return m_code.EmitAddImm32(3, HOST_VU, ViOffset(reg)) &&
					m_code.EmitLdrhImm8(rd, 3, 0);
			}

			bool EmitStoreViHalfword(unsigned rs, unsigned reg)
			{
				return m_code.EmitAddImm32(3, HOST_VU, ViOffset(reg)) &&
					m_code.EmitStrhImm8(rs, 3, 0);
			}

			bool EmitLoadViWordRaw(unsigned rd, unsigned reg)
			{
				return m_code.EmitLdrImm12(rd, HOST_VU, ViOffset(reg));
			}

			bool EmitLoadViLowResultOperand(unsigned rd, unsigned reg)
			{
				// Sony VI00-VI15 arithmetic publishes only the low 16-bit result.
				// Loading the aligned host word is therefore congruent for ADD,
				// SUB, AND, and OR, while using A32 LDR's 12-bit immediate instead
				// of a separate address ADD plus LDRH.
				if (reg == 0)
					return m_code.EmitMovImm8(rd, 0);

				return EmitLoadViWordRaw(rd, reg);
			}

			bool EmitStoreViWordRaw(unsigned rs, unsigned reg)
			{
				return m_code.EmitStrImm12(rs, HOST_VU, ViOffset(reg));
			}

			bool EmitLoadViSignedHalfword(unsigned rd, unsigned reg)
			{
				if (reg == 0)
					return m_code.EmitMovImm8(rd, 0);

				return EmitLoadViSignedHalfwordRaw(rd, reg);
			}

			bool EmitLoadViSignedHalfwordRaw(unsigned rd, unsigned reg)
			{
				return m_code.EmitAddImm32(3, HOST_VU, ViOffset(reg)) &&
					m_code.EmitLdrshImm8(rd, 3, 0);
			}

			// PCSX2 owners: x86/microVU_IR.h::microRegAlloc,
			// microVU_Analyze.inl's per-block use information, and
			// microVU_Alloc.inl's dirty writeback. Cortex-A9 has only four
			// practical callee-saved quads, so use a compact next-use cache and
			// admit only values proven reusable before the next state barrier.
			struct VectorCacheSlot
			{
				u8 guest = 0;
				bool dirty = false;
			};

			bool AreVectorLanesNormalized(u8 guest, u8 lanes) const
			{
				return lanes == 0 ||
					(m_normalized_vector_lanes[guest] & lanes) == lanes;
			}

			void MarkVectorLanesUnknown(u8 guest, u8 lanes)
			{
				if (guest != 0)
					m_normalized_vector_lanes[guest] &= static_cast<u8>(~lanes);
			}

			void MarkVectorLanesNormalized(u8 guest, u8 lanes)
			{
				if (guest != 0)
					m_normalized_vector_lanes[guest] |= lanes;
			}

			bool RecordOrConsumeVectorAccess(u8 guest, VectorAccessKind kind,
				bool needs_old_value, bool* admit)
			{
				if (admit)
					*admit = false;
				if (m_vu0_memory_map || m_vector_cache_mode == VectorCacheMode::Disabled)
					return true;
				if (!m_vector_accesses)
					return false;

				if (m_vector_cache_mode == VectorCacheMode::Trace)
				{
					m_vector_accesses->push_back({kind, guest, needs_old_value, false});
					return true;
				}

				if (m_vector_access_cursor >= m_vector_accesses->size())
					return false;
				const VectorAccessEvent& event = (*m_vector_accesses)[m_vector_access_cursor++];
				if (event.kind != kind || event.guest != guest ||
					event.needs_old_value != needs_old_value)
				{
					return false;
				}
				if (admit)
					*admit = event.admit;
				return true;
			}

			bool RecordOrConsumeVectorBarrier()
			{
				return RecordOrConsumeVectorAccess(0, VectorAccessKind::Barrier, false, nullptr);
			}

			bool EmitPreloadVectorCache()
			{
				if (!VectorCacheEnabled())
					return true;

				// PCSX2 owner: x86/microVU_Compile.inl::mvuPreloadRegisters().
				// It walks the analyzed block before emission and fills otherwise
				// idle vector registers with upcoming live-ins. Cortex-A9 VLD1.32
				// of a quad occupies two MPE issue cycles, so issue independent
				// live-in loads at the linked/body entry rather than on the first
				// consumer's dependency chain. A barrier ends the only lifetime
				// region eligible for entry preloading.
				for (const VectorAccessEvent& event : *m_vector_accesses)
				{
					if (event.kind == VectorAccessKind::Barrier)
						break;
					if (!event.admit || !event.needs_old_value || event.guest == 0 ||
						FindVectorCacheSlot(event.guest) >= 0)
					{
						continue;
					}

					u32 slot = VU_VECTOR_CACHE_SLOTS;
					for (u32 candidate = 0; candidate < VU_VECTOR_CACHE_SLOTS; candidate++)
					{
						if (m_vector_cache[candidate].guest == 0)
						{
							slot = candidate;
							break;
						}
					}
					if (slot == VU_VECTOR_CACHE_SLOTS)
						break;

					m_vector_cache[slot] = {event.guest, false};
					if (!EmitCanonicalVectorAddress(HOST_CALL_SCRATCH, event.guest) ||
						!m_code.EmitVld1Q32Aligned(VU_VECTOR_CACHE_FIRST_Q + slot,
							HOST_CALL_SCRATCH))
					{
						return false;
					}
					m_vector_cache_stats.misses++;
					m_vector_cache_stats.preloads++;
					if (event.guest == VU_VECTOR_CACHE_ACC)
						m_vector_cache_stats.acc_quad_loads++;
					else
						m_vector_cache_stats.vf_quad_loads++;
				}
				return true;
			}

			u32 NextAdmittedVectorUse(u8 guest) const
			{
				if (m_vector_cache_mode != VectorCacheMode::Enabled || !m_vector_accesses)
					return 0xffffffffu;
				for (size_t index = m_vector_access_cursor; index < m_vector_accesses->size(); index++)
				{
					const VectorAccessEvent& event = (*m_vector_accesses)[index];
					if (event.kind == VectorAccessKind::Barrier ||
						(event.guest == guest &&
						 (event.kind == VectorAccessKind::WordLoad ||
						  event.kind == VectorAccessKind::WordStore)))
					{
						break;
					}
					if (event.guest == guest && event.admit)
						return static_cast<u32>(index);
				}
				return 0xffffffffu;
			}

			int FindVectorCacheSlot(u8 guest) const
			{
				for (u32 slot = 0; slot < VU_VECTOR_CACHE_SLOTS; slot++)
				{
					if (m_vector_cache[slot].guest == guest)
						return static_cast<int>(slot);
				}
				return -1;
			}

			unsigned SelectCachedCompleteOverwriteQ(u8 guest, unsigned fallback_q) const
			{
				// The caller has already copied every source operand out of the cache.
				// Selecting an existing mapping here is therefore safe even when the
				// architectural destination aliases a source. New admissions remain on
				// the ordinary store path because no mapping exists until that event.
				if (!VectorCacheEnabled() || guest == 0)
					return fallback_q;
				const int slot = FindVectorCacheSlot(guest);
				return slot < 0 ? fallback_q :
					VU_VECTOR_CACHE_FIRST_Q + static_cast<unsigned>(slot);
			}

			bool EmitCanonicalVectorAddress(unsigned rd, u8 guest)
			{
				const size_t offset = guest == VU_VECTOR_CACHE_ACC ?
					offsetof(VURegs, ACC) : VfOffset(guest);
				return m_code.EmitAddImm32(rd, HOST_VU, VuOffset(offset));
			}

			bool EmitWritebackVectorCacheSlot(u32 slot)
			{
				VectorCacheSlot& mapping = m_vector_cache[slot];
				if (mapping.guest == 0 || !mapping.dirty)
					return true;

				if (!EmitCanonicalVectorAddress(HOST_CALL_SCRATCH, mapping.guest) ||
					!m_code.EmitVst1Q32Aligned(VU_VECTOR_CACHE_FIRST_Q + slot, HOST_CALL_SCRATCH))
				{
					return false;
				}
				mapping.dirty = false;
				m_vector_cache_stats.writebacks++;
				if (mapping.guest == VU_VECTOR_CACHE_ACC)
					m_vector_cache_stats.acc_quad_stores++;
				else
					m_vector_cache_stats.vf_quad_stores++;
				return true;
			}

			bool EmitFlushVectorCache()
			{
				if (m_vu0_memory_map)
					return true;
				if (!RecordOrConsumeVectorBarrier())
					return false;
				for (u32 slot = 0; slot < VU_VECTOR_CACHE_SLOTS; slot++)
				{
					if (!EmitWritebackVectorCacheSlot(slot))
						return false;
					m_vector_cache[slot] = {};
				}
				return true;
			}

			bool VectorCacheEnabled() const
			{
				return !m_vu0_memory_map &&
					m_vector_cache_mode == VectorCacheMode::Enabled &&
					!m_vector_cache_suspended;
			}

			bool EmitInvalidateCachedVector(u8 guest)
			{
				const int slot = FindVectorCacheSlot(guest);
				if (slot < 0)
					return true;
				if (!EmitWritebackVectorCacheSlot(static_cast<u32>(slot)))
					return false;
				m_vector_cache_stats.scalar_invalidations++;
				m_vector_cache[slot] = {};
				return true;
			}

			bool AcquireCachedVector(u8 guest, bool needs_old_value, unsigned* qreg)
			{
				pxAssert(!m_vu0_memory_map && guest != 0 && qreg != nullptr);
				int slot = FindVectorCacheSlot(guest);
				if (slot >= 0)
				{
					m_vector_cache_stats.hits++;
					if (guest == VU_VECTOR_CACHE_ACC)
						m_vector_cache_stats.acc_hits++;
					*qreg = VU_VECTOR_CACHE_FIRST_Q + static_cast<unsigned>(slot);
					return true;
				}

				m_vector_cache_stats.misses++;
				for (u32 candidate = 0; candidate < VU_VECTOR_CACHE_SLOTS; candidate++)
				{
					if (m_vector_cache[candidate].guest == 0)
					{
						slot = static_cast<int>(candidate);
						break;
					}
				}
				if (slot < 0)
				{
					u32 farthest_use = 0;
					slot = 0;
					for (u32 candidate = 0; candidate < VU_VECTOR_CACHE_SLOTS; candidate++)
					{
						const u32 next_use = NextAdmittedVectorUse(m_vector_cache[candidate].guest);
						if (next_use >= farthest_use)
						{
							farthest_use = next_use;
							slot = static_cast<int>(candidate);
						}
					}
					m_vector_cache_stats.evictions++;
				}

				if (!EmitWritebackVectorCacheSlot(static_cast<u32>(slot)))
					return false;
				m_vector_cache[slot] = {guest, false};
				*qreg = VU_VECTOR_CACHE_FIRST_Q + static_cast<unsigned>(slot);
				if (!needs_old_value)
					return true;

				if (!EmitCanonicalVectorAddress(HOST_CALL_SCRATCH, guest) ||
					!m_code.EmitVld1Q32Aligned(*qreg, HOST_CALL_SCRATCH))
				{
					return false;
				}
				if (guest == VU_VECTOR_CACHE_ACC)
					m_vector_cache_stats.acc_quad_loads++;
				else
					m_vector_cache_stats.vf_quad_loads++;
				return true;
			}

			bool EmitLoadCachedVectorQuad(unsigned qd, u8 guest)
			{
				unsigned cached_q = 0;
				return AcquireCachedVector(guest, true, &cached_q) &&
					(qd == cached_q || m_code.EmitVorrQ(qd, cached_q, cached_q));
			}

			bool EmitStoreCachedVectorQuad(unsigned qs, u8 guest, bool needs_old_value)
			{
				unsigned cached_q = 0;
				if (!AcquireCachedVector(guest, needs_old_value, &cached_q) ||
					(qs != cached_q && !m_code.EmitVorrQ(cached_q, qs, qs)))
				{
					return false;
				}
				m_vector_cache[cached_q - VU_VECTOR_CACHE_FIRST_Q].dirty = true;
				return true;
			}

			u16 VfLaneOffset(unsigned reg, unsigned lane)
			{
				return VuOffset(static_cast<size_t>(VfOffset(reg)) + lane * sizeof(u32));
			}

			bool EmitLoadVfWord(unsigned rd, unsigned reg, unsigned lane)
			{
				if (!RecordOrConsumeVectorAccess(static_cast<u8>(reg),
						VectorAccessKind::WordLoad, true, nullptr) ||
					(VectorCacheEnabled() && reg != 0 &&
					 !EmitInvalidateCachedVector(static_cast<u8>(reg))))
				{
					return false;
				}
				if (!m_vu0_memory_map)
					m_vector_cache_stats.uncached_loads++;
				if (!m_vu0_memory_map)
					m_vector_cache_stats.vf_word_loads++;
				return m_code.EmitLdrImm12(rd, HOST_VU, VfLaneOffset(reg, lane));
			}

			bool EmitLoadVfLaneBroadcastSelected(unsigned qd, unsigned reg, unsigned lane,
				bool use_single_d, int* selected_s)
			{
				if (!selected_s)
					return false;
				*selected_s = -1;
				// A resident VF can feed the broadcast entirely inside NEON with
				// VDUP.32. FMAC needs only one D-register copy because its exact
				// bit normalization can use D-form operations and scalar VFP can
				// reuse the low S alias for all four results.
				bool admit = false;
				if (!RecordOrConsumeVectorAccess(static_cast<u8>(reg),
					VectorAccessKind::LaneBroadcast, true, &admit))
				{
					return false;
				}
				if (!m_vu0_memory_map)
					m_vector_cache_stats.uncached_loads++;

				if (VectorCacheEnabled() && reg != 0 &&
					(FindVectorCacheSlot(static_cast<u8>(reg)) >= 0 || admit))
				{
					unsigned cached_q = 0;
					if (!AcquireCachedVector(static_cast<u8>(reg), true, &cached_q))
						return false;
					if (use_single_d)
					{
						m_single_d_broadcast_operands++;
						*selected_s = static_cast<int>(qd * 4);
						return m_code.EmitVdupI32DFromQlane(qd * 2, cached_q,
							static_cast<u8>(lane));
					}
					return m_code.EmitVdupI32QFromQlane(qd, cached_q,
						static_cast<u8>(lane));
				}

				if (!m_vu0_memory_map)
					m_vector_cache_stats.vf_word_loads++;
				if (!m_code.EmitLdrImm12(3, HOST_VU, VfLaneOffset(reg, lane)))
					return false;
				if (use_single_d)
				{
					m_single_d_broadcast_operands++;
					*selected_s = static_cast<int>(qd * 4);
					return m_code.EmitVdupI32DFromCore(qd * 2, 3);
				}
				return m_code.EmitVdupI32QFromCore(qd, 3);
			}

			bool EmitLoadVfLaneBroadcast(unsigned qd, unsigned reg, unsigned lane)
			{
				int selected_s = -1;
				return EmitLoadVfLaneBroadcastSelected(qd, reg, lane, false,
					&selected_s);
			}

			bool EmitStoreVfWord(unsigned rs, unsigned reg, unsigned lane)
			{
				if (!RecordOrConsumeVectorAccess(static_cast<u8>(reg),
						VectorAccessKind::WordStore, false, nullptr) ||
					(VectorCacheEnabled() && reg != 0 &&
					 !EmitInvalidateCachedVector(static_cast<u8>(reg))))
				{
					return false;
				}
				if (!m_vu0_memory_map && reg != 0)
					m_vector_cache_stats.uncached_stores++;
				if (!m_vu0_memory_map && reg != 0)
					m_vector_cache_stats.vf_word_stores++;
				if (!m_code.EmitStrImm12(rs, HOST_VU, VfLaneOffset(reg, lane)))
					return false;
				MarkVectorLanesUnknown(static_cast<u8>(reg),
					static_cast<u8>(1u << (3 - lane)));
				return true;
			}

			bool EmitStoreVfWordFromS(unsigned ss, unsigned reg, unsigned lane)
			{
				if (!RecordOrConsumeVectorAccess(static_cast<u8>(reg),
						VectorAccessKind::WordStore, false, nullptr) ||
					(VectorCacheEnabled() && reg != 0 &&
						!EmitInvalidateCachedVector(static_cast<u8>(reg))))
				{
					return false;
				}
				if (!m_vu0_memory_map && reg != 0)
					m_vector_cache_stats.uncached_stores++;
				if (!m_vu0_memory_map && reg != 0)
					m_vector_cache_stats.vf_word_stores++;
				if (!m_code.EmitVstrSImm(ss, HOST_VU, VfLaneOffset(reg, lane)))
					return false;
				MarkVectorLanesUnknown(static_cast<u8>(reg),
					static_cast<u8>(1u << (3 - lane)));
				return true;
			}

			bool EmitLoadVfQuad(unsigned qd, unsigned reg)
			{
				bool admit = false;
				if (!RecordOrConsumeVectorAccess(static_cast<u8>(reg),
					VectorAccessKind::QuadLoad, true, &admit))
				{
					return false;
				}
				if (!m_vu0_memory_map)
					m_vector_cache_stats.uncached_loads++;
				if (VectorCacheEnabled() && reg != 0 &&
					(FindVectorCacheSlot(static_cast<u8>(reg)) >= 0 || admit))
					return EmitLoadCachedVectorQuad(qd, static_cast<u8>(reg));
				if (!m_vu0_memory_map)
					m_vector_cache_stats.vf_quad_loads++;
				return EmitCanonicalVectorAddress(3, static_cast<u8>(reg)) &&
					m_code.EmitVld1Q32Aligned(qd, 3);
			}

			bool EmitStoreVfQuad(unsigned qs, unsigned reg, bool needs_old_value = false)
			{
				if (reg == 0)
					return true;
				bool admit = false;
				if (!RecordOrConsumeVectorAccess(static_cast<u8>(reg),
					VectorAccessKind::QuadStore, needs_old_value, &admit))
				{
					return false;
				}
				if (!m_vu0_memory_map)
					m_vector_cache_stats.uncached_stores++;
				bool emitted = false;
				if (VectorCacheEnabled() &&
					(FindVectorCacheSlot(static_cast<u8>(reg)) >= 0 || admit))
				{
					emitted = EmitStoreCachedVectorQuad(qs, static_cast<u8>(reg), needs_old_value);
				}
				else
				{
					if (!m_vu0_memory_map)
						m_vector_cache_stats.vf_quad_stores++;
					emitted = EmitCanonicalVectorAddress(3, static_cast<u8>(reg)) &&
						m_code.EmitVst1Q32Aligned(qs, 3);
				}
				if (!emitted)
					return false;
				MarkVectorLanesUnknown(static_cast<u8>(reg), 0x0f);
				return true;
			}

			bool EmitLoadAccWord(unsigned rd, unsigned lane)
			{
				if (!RecordOrConsumeVectorAccess(VU_VECTOR_CACHE_ACC,
						VectorAccessKind::WordLoad, true, nullptr) ||
					(VectorCacheEnabled() && !EmitInvalidateCachedVector(VU_VECTOR_CACHE_ACC)))
				{
					return false;
				}
				if (!m_vu0_memory_map)
					m_vector_cache_stats.uncached_loads++;
				if (!m_vu0_memory_map)
					m_vector_cache_stats.acc_word_loads++;
				return m_code.EmitLdrImm12(rd, HOST_VU,
					VuOffset(offsetof(VURegs, ACC) + lane * sizeof(u32)));
			}

			bool EmitStoreAccWord(unsigned rs, unsigned lane)
			{
				if (!RecordOrConsumeVectorAccess(VU_VECTOR_CACHE_ACC,
						VectorAccessKind::WordStore, false, nullptr) ||
					(VectorCacheEnabled() && !EmitInvalidateCachedVector(VU_VECTOR_CACHE_ACC)))
				{
					return false;
				}
				if (!m_vu0_memory_map)
					m_vector_cache_stats.uncached_stores++;
				if (!m_vu0_memory_map)
					m_vector_cache_stats.acc_word_stores++;
				if (!m_code.EmitStrImm12(rs, HOST_VU,
						VuOffset(offsetof(VURegs, ACC) + lane * sizeof(u32))))
				{
					return false;
				}
				MarkVectorLanesUnknown(VU_VECTOR_CACHE_ACC,
					static_cast<u8>(1u << (3 - lane)));
				return true;
			}

			bool EmitStoreAccWordFromS(unsigned ss, unsigned lane)
			{
				if (!RecordOrConsumeVectorAccess(VU_VECTOR_CACHE_ACC,
						VectorAccessKind::WordStore, false, nullptr) ||
					(VectorCacheEnabled() && !EmitInvalidateCachedVector(VU_VECTOR_CACHE_ACC)))
				{
					return false;
				}
				if (!m_vu0_memory_map)
					m_vector_cache_stats.uncached_stores++;
				if (!m_vu0_memory_map)
					m_vector_cache_stats.acc_word_stores++;
				// ACC begins at byte 1024, just beyond VSTR's 10-bit scaled
				// immediate range. Materialize its base once for this lane rather
				// than forcing the value through an ARM core register.
				if (!EmitCanonicalVectorAddress(3, VU_VECTOR_CACHE_ACC) ||
					!m_code.EmitVstrSImm(ss, 3, static_cast<u16>(lane * sizeof(u32))))
				{
					return false;
				}
				MarkVectorLanesUnknown(VU_VECTOR_CACHE_ACC,
					static_cast<u8>(1u << (3 - lane)));
				return true;
			}

			bool EmitLoadAccQuadSelected(unsigned qd, bool use_cached_q_directly,
				unsigned* selected_q)
			{
				if (!selected_q)
					return false;
				*selected_q = qd;
				bool admit = false;
				if (!RecordOrConsumeVectorAccess(VU_VECTOR_CACHE_ACC,
					VectorAccessKind::QuadLoad, true, &admit))
				{
					return false;
				}
				if (!m_vu0_memory_map)
					m_vector_cache_stats.uncached_loads++;
				if (VectorCacheEnabled() &&
					(FindVectorCacheSlot(VU_VECTOR_CACHE_ACC) >= 0 || admit))
				{
					unsigned cached_q = 0;
					if (!AcquireCachedVector(VU_VECTOR_CACHE_ACC, true, &cached_q))
						return false;
					if (use_cached_q_directly)
					{
						// Q4-Q7 are preserved once around the complete VU execution
						// window. A normalized full-mask ACC can therefore be the scalar
						// VFP accumulator itself (S16-S31), avoiding both Q-form VMOVs
						// around a MADDA/MSUBA. Cortex-A9 issues each Q logical move as
						// two D operations; keeping the allocator-owned value in place is
						// both fewer instructions and less MPE pressure.
						*selected_q = cached_q;
						return true;
					}
					return qd == cached_q || m_code.EmitVorrQ(qd, cached_q, cached_q);
				}
				if (!m_vu0_memory_map)
					m_vector_cache_stats.acc_quad_loads++;
				return EmitCanonicalVectorAddress(3, VU_VECTOR_CACHE_ACC) &&
					m_code.EmitVld1Q32Aligned(qd, 3);
			}

			bool EmitLoadAccQuad(unsigned qd)
			{
				unsigned selected_q = qd;
				return EmitLoadAccQuadSelected(qd, false, &selected_q);
			}

			bool EmitStoreAccQuad(unsigned qs, bool needs_old_value = false)
			{
				bool admit = false;
				if (!RecordOrConsumeVectorAccess(VU_VECTOR_CACHE_ACC,
					VectorAccessKind::QuadStore, needs_old_value, &admit))
				{
					return false;
				}
				if (!m_vu0_memory_map)
					m_vector_cache_stats.uncached_stores++;

				bool emitted = false;
				if (VectorCacheEnabled() &&
					(FindVectorCacheSlot(VU_VECTOR_CACHE_ACC) >= 0 || admit))
				{
					emitted = EmitStoreCachedVectorQuad(qs, VU_VECTOR_CACHE_ACC,
						needs_old_value);
				}
				else
				{
					if (!m_vu0_memory_map)
						m_vector_cache_stats.acc_quad_stores++;
					emitted = EmitCanonicalVectorAddress(3, VU_VECTOR_CACHE_ACC) &&
						m_code.EmitVst1Q32Aligned(qs, 3);
				}
				if (!emitted)
					return false;
				MarkVectorLanesUnknown(VU_VECTOR_CACHE_ACC, 0x0f);
				return true;
			}

			bool EmitStoreWordToVfMasked(unsigned value_reg, unsigned ft, unsigned mask)
			{
				if (ft == 0 || mask == 0)
					return true;

				if (mask == 0x0f)
				{
					return m_code.EmitVdupI32QFromCore(0, value_reg) &&
						EmitStoreVfQuad(0, ft);
				}

				bool emitted = true;
				if ((mask & 0x8) != 0)
					emitted = emitted && EmitStoreVfWord(value_reg, ft, 0);
				if ((mask & 0x4) != 0)
					emitted = emitted && EmitStoreVfWord(value_reg, ft, 1);
				if ((mask & 0x2) != 0)
					emitted = emitted && EmitStoreVfWord(value_reg, ft, 2);
				if ((mask & 0x1) != 0)
					emitted = emitted && EmitStoreVfWord(value_reg, ft, 3);
				return emitted;
			}

			bool EmitStoreQ0ToVfMasked(unsigned ft, unsigned mask)
			{
				if (ft == 0 || mask == 0)
					return true;

				if (mask == 0x0f)
					return EmitStoreVfQuad(0, ft);

				bool emitted = true;
				if ((mask & 0x8) != 0)
					emitted = emitted && EmitStoreVfWordFromS(0, ft, 0);
				if ((mask & 0x4) != 0)
					emitted = emitted && EmitStoreVfWordFromS(1, ft, 1);
				if ((mask & 0x2) != 0)
					emitted = emitted && EmitStoreVfWordFromS(2, ft, 2);
				if ((mask & 0x1) != 0)
					emitted = emitted && EmitStoreVfWordFromS(3, ft, 3);
				return emitted;
			}

			bool EmitInlineUpperNop()
			{
				// PCSX2 owners: VUops.cpp::_vuNOP() and
				// VUmicroFast.h::ExecuteUpperNoLowerKnownKind(NOP).
				// The upper body is intentionally empty; I-bit REG_I ordering
				// is handled by EmitPair() after the upper slot.
#if defined(VITASX2_QEMU_VALIDATION)
				return EmitQemuUpperNopInlineCounter();
#else
				return true;
#endif
			}

			bool EmitInlineUpperUnary(u32 code, VUInterpFast::UpperFastKind kind)
			{
				const unsigned ft = VUInterpFast::Ft(code);
				const unsigned fs = VUInterpFast::Fs(code);
				const unsigned mask = VUInterpFast::XYZW(code);
				if (ft == 0 || mask == 0)
				{
#if defined(VITASX2_QEMU_VALIDATION)
					return EmitQemuUpperUnaryInlineCounter();
#else
					return true;
#endif
				}

				// PCSX2 owner: VUops.cpp::{floatToInt,intToFloat}() and
				// VUmicroFast.h::ExecuteUpperNoLowerKnownKind(). Cortex-A9 Advanced
				// SIMD arithmetic ignores FPSCR.RMode, while scalar VFP observes it.
				// Keep the qword load/store and exact integer ABS work and use scalar
				// VFP for rounding-sensitive arithmetic and ITOF.
				if (!EmitLoadVfQuad(0, fs))
				{
					return false;
				}

				bool emitted_body = true;
					switch (kind)
					{
						case VUInterpFast::UpperFastKind::ABS:
							// Sony ABS clears the raw sign bit. ARM ARM A8.6.277
							// performs exactly q0 &= ~0x80000000 in one NEON
							// instruction, including for zero, denormals and NaNs.
							emitted_body = m_code.EmitVbicI32Q(0, 0x80u, 24);
						break;
					case VUInterpFast::UpperFastKind::FTOI0:
					case VUInterpFast::UpperFastKind::FTOI4:
					case VUInterpFast::UpperFastKind::FTOI12:
					case VUInterpFast::UpperFastKind::FTOI15:
					{
						unsigned offset = 0;
						if (kind == VUInterpFast::UpperFastKind::FTOI4)
							offset = 4;
						else if (kind == VUInterpFast::UpperFastKind::FTOI12)
							offset = 12;
						else if (kind == VUInterpFast::UpperFastKind::FTOI15)
							offset = 15;

						emitted_body = true;
						if (offset != 0)
						{
							emitted_body = emitted_body &&
								m_code.EmitMovImm32(0, 0x3f800000u + (offset << 23)) &&
								m_code.EmitVmovCoreToS(4, 0);
						}

						for (unsigned lane = 0; lane < 4 && emitted_body; lane++)
						{
							const unsigned lane_bit = 1u << (3 - lane);
							if ((mask & lane_bit) == 0)
								continue;

							emitted_body =
								(offset == 0 || m_code.EmitVmulF32(lane, lane, 4)) &&
								m_code.EmitVmovSToCore(0, lane) &&
								EmitAndRegImm32(1, 0, FPU_FLOAT_EXPONENT_MASK, HOST_CALL_SCRATCH) &&
								m_code.EmitMovRegShiftImm(2, 0, ShiftType::ASR, 31) &&
								// signmask ^ 0x80000000, then invert, maps positive to
								// INT_MAX and negative to INT_MIN using two encodable ops.
								m_code.EmitEorImm32(2, 2, FPU_FLOAT_SIGN_MASK) &&
								m_code.EmitMvnReg(2, 2) &&
								m_code.EmitVcvtS32F32(lane, lane) &&
								m_code.EmitVmovSToCore(0, lane) &&
								EmitCmpRegImm32(1, 0x4f000000u, HOST_CALL_SCRATCH) &&
								EmitMovReg(0, 2, Condition::CS) &&
								m_code.EmitVmovCoreToS(lane, 0);
						}
						break;
					}
					case VUInterpFast::UpperFastKind::ITOF0:
					case VUInterpFast::UpperFastKind::ITOF4:
					case VUInterpFast::UpperFastKind::ITOF12:
					case VUInterpFast::UpperFastKind::ITOF15:
					{
						unsigned offset = 0;
						if (kind == VUInterpFast::UpperFastKind::ITOF4)
							offset = 4;
						if (kind == VUInterpFast::UpperFastKind::ITOF12)
							offset = 12;
						else if (kind == VUInterpFast::UpperFastKind::ITOF15)
							offset = 15;

						// Ordinary scalar VFP conversion observes the installed VU FPCR.
						// Scaling by 2^-offset is exact for the complete signed-int domain;
						// materialize it once and use scalar VMUL only on active lanes.
						emitted_body = offset == 0 ||
							(m_code.EmitMovImm32(0, 0x3f800000u - (offset << 23)) &&
							 m_code.EmitVmovCoreToS(4, 0));
						for (unsigned lane = 0; lane < 4 && emitted_body; lane++)
						{
							const unsigned lane_bit = 1u << (3 - lane);
							if ((mask & lane_bit) == 0)
								continue;
							emitted_body = m_code.EmitVcvtF32S32(lane, lane) &&
								(offset == 0 || m_code.EmitVmulF32(lane, lane, 4));
						}
						break;
					}
					default:
						return false;
				}

				if (!emitted_body ||
					!EmitStoreQ0ToVfMasked(ft, mask))
				{
					return false;
				}

#if defined(VITASX2_QEMU_VALIDATION)
				return EmitQemuUpperUnaryInlineCounter();
#else
				return true;
#endif
			}

			bool EmitOrClipBitIfSignedGreater(unsigned flags_reg, unsigned value_reg,
				unsigned lane_reg, u8 bit)
			{
				return m_code.EmitCmpReg(lane_reg, value_reg) &&
					m_code.EmitMovImm8(3, 0) &&
					m_code.EmitMovImm8(3, bit, Condition::GT) &&
					m_code.EmitOrrReg(flags_reg, flags_reg, 3);
			}

			bool EmitInlineUpperClip(u32 code)
			{
				const unsigned fs = VUInterpFast::Fs(code);
				const unsigned ft = VUInterpFast::Ft(code);

				// PCSX2 owner: VUops.cpp::_vuCLIP() /
				// VUmicroFast.h::ExecuteClip(). It updates VU->clipflag; the
				// existing upper FMAC stall tail snapshots and later exposes it
				// through VI[REG_CLIP_FLAG].
				if (!EmitLoadVfWord(0, ft, 3) ||
					!EmitAndRegImm32(1, 0, FPU_FLOAT_EXPONENT_MASK, HOST_CALL_SCRATCH) ||
					!EmitAndRegImm32(0, 0, ~FPU_FLOAT_SIGN_MASK, HOST_CALL_SCRATCH) ||
					!m_code.EmitCmpImm32(1, 0) ||
					!m_code.EmitMovImm32(0, FPU_FLOAT_MANTISSA_MASK, Condition::EQ) ||
					!m_code.EmitMovImm8(2, 0))
				{
					return false;
				}

				for (unsigned lane = 0; lane < 3; lane++)
				{
					const u8 pos_bit = static_cast<u8>(1u << (lane * 2));
					const u8 neg_bit = static_cast<u8>(1u << (lane * 2 + 1));
					if (!EmitLoadVfWord(1, fs, lane) ||
						!EmitOrClipBitIfSignedGreater(2, 0, 1, pos_bit) ||
						!m_code.EmitEorImm32(1, 1, FPU_FLOAT_SIGN_MASK) ||
						!EmitOrClipBitIfSignedGreater(2, 0, 1, neg_bit))
					{
						return false;
					}
				}

				if (!m_code.EmitLdrImm12(1, HOST_VU, VuOffset(offsetof(VURegs, clipflag))) ||
					!m_code.EmitMovRegShiftImm(1, 1, ShiftType::LSL, 6) ||
					!m_code.EmitOrrReg(1, 1, 2) ||
					!EmitAndRegImm32(1, 1, 0x00ffffffu, HOST_CALL_SCRATCH) ||
					!m_code.EmitStrImm12(1, HOST_VU, VuOffset(offsetof(VURegs, clipflag))))
				{
					return false;
				}

#if defined(VITASX2_QEMU_VALIDATION)
				return EmitQemuUpperClipInlineCounter();
#else
				return true;
#endif
			}

			bool EmitLoadUpperMinMaxOperandQ1(u32 code, VUInterpFast::UpperFastKind kind)
			{
				const unsigned ft = VUInterpFast::Ft(code);
				const int broadcast_lane = UpperVfBroadcastLane(kind);
				if (broadcast_lane >= 0)
					return EmitLoadVfLaneBroadcast(1, ft, static_cast<unsigned>(broadcast_lane));
				switch (kind)
				{
					case VUInterpFast::UpperFastKind::MAX:
					case VUInterpFast::UpperFastKind::MINI:
						return EmitLoadVfQuad(1, ft);
					case VUInterpFast::UpperFastKind::MAXi:
					case VUInterpFast::UpperFastKind::MINIi:
						return EmitLoadViWordRaw(0, REG_I) &&
							m_code.EmitVdupI32QFromCore(1, 0);
					default:
						return false;
				}
			}

			bool EmitInlineUpperMinMax(u32 code, VUInterpFast::UpperFastKind kind)
			{
				const unsigned fd = VUInterpFast::Fd(code);
				const unsigned fs = VUInterpFast::Fs(code);
				const unsigned mask = VUInterpFast::XYZW(code);
				if (fd == 0 || mask == 0)
				{
#if defined(VITASX2_QEMU_VALIDATION)
					return EmitQemuUpperMinMaxInlineCounter();
#else
					return true;
#endif
				}

				const bool take_max =
					kind == VUInterpFast::UpperFastKind::MAX ||
					kind == VUInterpFast::UpperFastKind::MAXi ||
					kind == VUInterpFast::UpperFastKind::MAXx ||
					kind == VUInterpFast::UpperFastKind::MAXy ||
					kind == VUInterpFast::UpperFastKind::MAXz ||
					kind == VUInterpFast::UpperFastKind::MAXw;

				// PCSX2 owner: VUmicroFast.h::MinMaxBitsNeon(), which
				// implements VUops.cpp::fp_max()/fp_min() signed raw-bit
				// ordering with the both-negative lane inversion. After a&b,
				// its signed word is below zero exactly when both sign bits are
				// set, so VCLT.S32 #0 directly constructs the inversion mask.
				bool emitted_body =
					EmitLoadVfQuad(0, fs) &&
					EmitLoadUpperMinMaxOperandQ1(code, kind) &&
					m_code.EmitVminS32Q(2, 0, 1) &&
					m_code.EmitVmaxS32Q(3, 0, 1) &&
					m_code.EmitVandQ(0, 0, 1) &&
					m_code.EmitVcltS32ZeroQ(0, 0) &&
					m_code.EmitVeorQ(1, 2, 3) &&
					m_code.EmitVandQ(1, 1, 0);
				emitted_body = emitted_body &&
					(take_max ? m_code.EmitVeorQ(0, 3, 1) : m_code.EmitVeorQ(0, 2, 1)) &&
					EmitStoreQ0ToVfMasked(fd, mask);

				if (!emitted_body)
					return false;

#if defined(VITASX2_QEMU_VALIDATION)
				return EmitQemuUpperMinMaxInlineCounter();
#else
				return true;
#endif
			}

			bool IsUpperMulAccKind(VUInterpFast::UpperFastKind kind)
			{
				switch (kind)
				{
					case VUInterpFast::UpperFastKind::MULA:
					case VUInterpFast::UpperFastKind::MULAi:
					case VUInterpFast::UpperFastKind::MULAq:
					case VUInterpFast::UpperFastKind::MULAx:
					case VUInterpFast::UpperFastKind::MULAy:
					case VUInterpFast::UpperFastKind::MULAz:
					case VUInterpFast::UpperFastKind::MULAw:
						return true;
					default:
						return false;
				}
			}

			bool IsUpperAddSubAccKind(VUInterpFast::UpperFastKind kind)
			{
				switch (kind)
				{
					case VUInterpFast::UpperFastKind::ADDA:
					case VUInterpFast::UpperFastKind::ADDAi:
					case VUInterpFast::UpperFastKind::ADDAq:
					case VUInterpFast::UpperFastKind::ADDAx:
					case VUInterpFast::UpperFastKind::ADDAy:
					case VUInterpFast::UpperFastKind::ADDAz:
					case VUInterpFast::UpperFastKind::ADDAw:
					case VUInterpFast::UpperFastKind::SUBA:
					case VUInterpFast::UpperFastKind::SUBAi:
					case VUInterpFast::UpperFastKind::SUBAq:
					case VUInterpFast::UpperFastKind::SUBAx:
					case VUInterpFast::UpperFastKind::SUBAy:
					case VUInterpFast::UpperFastKind::SUBAz:
					case VUInterpFast::UpperFastKind::SUBAw:
						return true;
					default:
						return false;
				}
			}

			bool IsUpperSubKind(VUInterpFast::UpperFastKind kind)
			{
				switch (kind)
				{
					case VUInterpFast::UpperFastKind::SUB:
					case VUInterpFast::UpperFastKind::SUBi:
					case VUInterpFast::UpperFastKind::SUBq:
					case VUInterpFast::UpperFastKind::SUBx:
					case VUInterpFast::UpperFastKind::SUBy:
					case VUInterpFast::UpperFastKind::SUBz:
					case VUInterpFast::UpperFastKind::SUBw:
					case VUInterpFast::UpperFastKind::SUBA:
					case VUInterpFast::UpperFastKind::SUBAi:
					case VUInterpFast::UpperFastKind::SUBAq:
					case VUInterpFast::UpperFastKind::SUBAx:
					case VUInterpFast::UpperFastKind::SUBAy:
					case VUInterpFast::UpperFastKind::SUBAz:
					case VUInterpFast::UpperFastKind::SUBAw:
						return true;
					default:
						return false;
				}
			}

			bool IsUpperAddiTriAceKind(VUInterpFast::UpperFastKind kind)
			{
				return kind == VUInterpFast::UpperFastKind::ADDi;
			}

			// ADD/SUB/MUL forms whose second operand is the full VF[ft] vector
			// (one word per lane) rather than a broadcast of REG_I/REG_Q or a
			// single VF lane. Vector forms load ft directly as a NEON quad;
			// broadcast forms need only one scalar-VFP source value.
			bool IsUpperVectorOperandForm(VUInterpFast::UpperFastKind kind)
			{
				switch (kind)
				{
					case VUInterpFast::UpperFastKind::ADD:
					case VUInterpFast::UpperFastKind::ADDA:
					case VUInterpFast::UpperFastKind::SUB:
					case VUInterpFast::UpperFastKind::SUBA:
					case VUInterpFast::UpperFastKind::MUL:
					case VUInterpFast::UpperFastKind::MULA:
						return true;
					default:
						return false;
				}
			}

			// MADD/MSUB whose second operand is the full VF[ft] vector.
			bool IsUpperMaddMsubVectorForm(VUInterpFast::UpperFastKind kind)
			{
				switch (kind)
				{
					case VUInterpFast::UpperFastKind::MADD:
					case VUInterpFast::UpperFastKind::MADDA:
					case VUInterpFast::UpperFastKind::MSUB:
					case VUInterpFast::UpperFastKind::MSUBA:
						return true;
					default:
						return false;
				}
			}

			int UpperVfBroadcastLane(VUInterpFast::UpperFastKind kind) const
			{
				switch (kind)
				{
					case VUInterpFast::UpperFastKind::ADDx:
					case VUInterpFast::UpperFastKind::ADDAx:
					case VUInterpFast::UpperFastKind::SUBx:
					case VUInterpFast::UpperFastKind::SUBAx:
					case VUInterpFast::UpperFastKind::MULx:
					case VUInterpFast::UpperFastKind::MULAx:
					case VUInterpFast::UpperFastKind::MADDx:
					case VUInterpFast::UpperFastKind::MADDAx:
					case VUInterpFast::UpperFastKind::MSUBx:
					case VUInterpFast::UpperFastKind::MSUBAx:
					case VUInterpFast::UpperFastKind::MAXx:
					case VUInterpFast::UpperFastKind::MINIx:
						return 0;
					case VUInterpFast::UpperFastKind::ADDy:
					case VUInterpFast::UpperFastKind::ADDAy:
					case VUInterpFast::UpperFastKind::SUBy:
					case VUInterpFast::UpperFastKind::SUBAy:
					case VUInterpFast::UpperFastKind::MULy:
					case VUInterpFast::UpperFastKind::MULAy:
					case VUInterpFast::UpperFastKind::MADDy:
					case VUInterpFast::UpperFastKind::MADDAy:
					case VUInterpFast::UpperFastKind::MSUBy:
					case VUInterpFast::UpperFastKind::MSUBAy:
					case VUInterpFast::UpperFastKind::MAXy:
					case VUInterpFast::UpperFastKind::MINIy:
						return 1;
					case VUInterpFast::UpperFastKind::ADDz:
					case VUInterpFast::UpperFastKind::ADDAz:
					case VUInterpFast::UpperFastKind::SUBz:
					case VUInterpFast::UpperFastKind::SUBAz:
					case VUInterpFast::UpperFastKind::MULz:
					case VUInterpFast::UpperFastKind::MULAz:
					case VUInterpFast::UpperFastKind::MADDz:
					case VUInterpFast::UpperFastKind::MADDAz:
					case VUInterpFast::UpperFastKind::MSUBz:
					case VUInterpFast::UpperFastKind::MSUBAz:
					case VUInterpFast::UpperFastKind::MAXz:
					case VUInterpFast::UpperFastKind::MINIz:
						return 2;
					case VUInterpFast::UpperFastKind::ADDw:
					case VUInterpFast::UpperFastKind::ADDAw:
					case VUInterpFast::UpperFastKind::SUBw:
					case VUInterpFast::UpperFastKind::SUBAw:
					case VUInterpFast::UpperFastKind::MULw:
					case VUInterpFast::UpperFastKind::MULAw:
					case VUInterpFast::UpperFastKind::MADDw:
					case VUInterpFast::UpperFastKind::MADDAw:
					case VUInterpFast::UpperFastKind::MSUBw:
					case VUInterpFast::UpperFastKind::MSUBAw:
					case VUInterpFast::UpperFastKind::MAXw:
					case VUInterpFast::UpperFastKind::MINIw:
						return 3;
					default:
						return -1;
				}
			}

			bool IsUpperOperandNormalized(u32 code, VUInterpFast::UpperFastKind kind,
				u8 active_lanes, bool vector_form) const
			{
				const u8 ft = static_cast<u8>(VUInterpFast::Ft(code));
				if (vector_form)
					return AreVectorLanesNormalized(ft, active_lanes);

				const int lane = UpperVfBroadcastLane(kind);
				return lane >= 0 && AreVectorLanesNormalized(ft,
					static_cast<u8>(1u << (3 - lane)));
			}

			// MADD/MSUB whose second operand broadcasts one VF[ft] lane. When the
			// destination aliases ft, the interpreter's per-lane store order lets a
			// later lane observe the just-written value, so these keep the scalar
			// per-lane path when fd == ft.
			bool IsUpperMaddMsubVfBroadcastForm(VUInterpFast::UpperFastKind kind)
			{
				switch (kind)
				{
					case VUInterpFast::UpperFastKind::MADDx:
					case VUInterpFast::UpperFastKind::MADDAx:
					case VUInterpFast::UpperFastKind::MSUBx:
					case VUInterpFast::UpperFastKind::MSUBAx:
					case VUInterpFast::UpperFastKind::MADDy:
					case VUInterpFast::UpperFastKind::MADDAy:
					case VUInterpFast::UpperFastKind::MSUBy:
					case VUInterpFast::UpperFastKind::MSUBAy:
					case VUInterpFast::UpperFastKind::MADDz:
					case VUInterpFast::UpperFastKind::MADDAz:
					case VUInterpFast::UpperFastKind::MSUBz:
					case VUInterpFast::UpperFastKind::MSUBAz:
					case VUInterpFast::UpperFastKind::MADDw:
					case VUInterpFast::UpperFastKind::MADDAw:
					case VUInterpFast::UpperFastKind::MSUBw:
					case VUInterpFast::UpperFastKind::MSUBAw:
						return true;
					default:
						return false;
				}
			}

			bool IsUpperMaddMsubAccKind(VUInterpFast::UpperFastKind kind)
			{
				switch (kind)
				{
					case VUInterpFast::UpperFastKind::MADDA:
					case VUInterpFast::UpperFastKind::MADDAi:
					case VUInterpFast::UpperFastKind::MADDAq:
					case VUInterpFast::UpperFastKind::MADDAx:
					case VUInterpFast::UpperFastKind::MADDAy:
					case VUInterpFast::UpperFastKind::MADDAz:
					case VUInterpFast::UpperFastKind::MADDAw:
					case VUInterpFast::UpperFastKind::MSUBA:
					case VUInterpFast::UpperFastKind::MSUBAi:
					case VUInterpFast::UpperFastKind::MSUBAq:
					case VUInterpFast::UpperFastKind::MSUBAx:
					case VUInterpFast::UpperFastKind::MSUBAy:
					case VUInterpFast::UpperFastKind::MSUBAz:
					case VUInterpFast::UpperFastKind::MSUBAw:
						return true;
					default:
						return false;
				}
			}

			bool IsUpperMsubKind(VUInterpFast::UpperFastKind kind)
			{
				switch (kind)
				{
					case VUInterpFast::UpperFastKind::MSUB:
					case VUInterpFast::UpperFastKind::MSUBi:
					case VUInterpFast::UpperFastKind::MSUBq:
					case VUInterpFast::UpperFastKind::MSUBx:
					case VUInterpFast::UpperFastKind::MSUBy:
					case VUInterpFast::UpperFastKind::MSUBz:
					case VUInterpFast::UpperFastKind::MSUBw:
					case VUInterpFast::UpperFastKind::MSUBA:
					case VUInterpFast::UpperFastKind::MSUBAi:
					case VUInterpFast::UpperFastKind::MSUBAq:
					case VUInterpFast::UpperFastKind::MSUBAx:
					case VUInterpFast::UpperFastKind::MSUBAy:
					case VUInterpFast::UpperFastKind::MSUBAz:
					case VUInterpFast::UpperFastKind::MSUBAw:
						return true;
					default:
						return false;
				}
			}

			bool EmitLoadUpperAddSubOperandWord(unsigned rd, u32 code, VUInterpFast::UpperFastKind kind,
				unsigned lane)
			{
				const unsigned ft = VUInterpFast::Ft(code);
				switch (kind)
				{
					case VUInterpFast::UpperFastKind::ADD:
					case VUInterpFast::UpperFastKind::ADDA:
					case VUInterpFast::UpperFastKind::SUB:
					case VUInterpFast::UpperFastKind::SUBA:
						return EmitLoadVfWord(rd, ft, lane);
					case VUInterpFast::UpperFastKind::ADDi:
					case VUInterpFast::UpperFastKind::ADDAi:
					case VUInterpFast::UpperFastKind::SUBi:
					case VUInterpFast::UpperFastKind::SUBAi:
						return EmitLoadViWordRaw(rd, REG_I);
					case VUInterpFast::UpperFastKind::ADDq:
					case VUInterpFast::UpperFastKind::ADDAq:
					case VUInterpFast::UpperFastKind::SUBq:
					case VUInterpFast::UpperFastKind::SUBAq:
						return EmitLoadViWordRaw(rd, REG_Q);
					case VUInterpFast::UpperFastKind::ADDx:
					case VUInterpFast::UpperFastKind::ADDAx:
					case VUInterpFast::UpperFastKind::SUBx:
					case VUInterpFast::UpperFastKind::SUBAx:
						return EmitLoadVfWord(rd, ft, 0);
					case VUInterpFast::UpperFastKind::ADDy:
					case VUInterpFast::UpperFastKind::ADDAy:
					case VUInterpFast::UpperFastKind::SUBy:
					case VUInterpFast::UpperFastKind::SUBAy:
						return EmitLoadVfWord(rd, ft, 1);
					case VUInterpFast::UpperFastKind::ADDz:
					case VUInterpFast::UpperFastKind::ADDAz:
					case VUInterpFast::UpperFastKind::SUBz:
					case VUInterpFast::UpperFastKind::SUBAz:
						return EmitLoadVfWord(rd, ft, 2);
					case VUInterpFast::UpperFastKind::ADDw:
					case VUInterpFast::UpperFastKind::ADDAw:
					case VUInterpFast::UpperFastKind::SUBw:
					case VUInterpFast::UpperFastKind::SUBAw:
						return EmitLoadVfWord(rd, ft, 3);
					default:
						return false;
				}
			}

			// Loads the ADD/SUB second operand. Vector forms use a complete Q
			// register; broadcast forms duplicate the one semantic source word
			// into a D register and report its low scalar-VFP alias in selected_s.
			bool EmitLoadUpperAddSubOperand(unsigned qd, u32 code,
				VUInterpFast::UpperFastKind kind, int* selected_s)
			{
				if (!selected_s)
					return false;
				*selected_s = -1;
				if (IsUpperVectorOperandForm(kind))
					return EmitLoadVfQuad(qd, VUInterpFast::Ft(code));
				const int broadcast_lane = UpperVfBroadcastLane(kind);
				if (broadcast_lane >= 0)
					return EmitLoadVfLaneBroadcastSelected(qd, VUInterpFast::Ft(code),
						static_cast<unsigned>(broadcast_lane), true,
						selected_s);

				if (!EmitLoadUpperAddSubOperandWord(3, code, kind, 0))
					return false;
				*selected_s = static_cast<int>(qd * 4);
				m_single_d_broadcast_operands++;
				return m_code.EmitVdupI32DFromCore(qd * 2, 3);
			}

			bool EmitLoadUpperMulOperandWord(unsigned rd, u32 code, VUInterpFast::UpperFastKind kind,
				unsigned lane)
			{
				const unsigned ft = VUInterpFast::Ft(code);
				switch (kind)
				{
					case VUInterpFast::UpperFastKind::MUL:
					case VUInterpFast::UpperFastKind::MULA:
						return EmitLoadVfWord(rd, ft, lane);
					case VUInterpFast::UpperFastKind::MULi:
					case VUInterpFast::UpperFastKind::MULAi:
						return EmitLoadViWordRaw(rd, REG_I);
					case VUInterpFast::UpperFastKind::MULq:
					case VUInterpFast::UpperFastKind::MULAq:
						return EmitLoadViWordRaw(rd, REG_Q);
					case VUInterpFast::UpperFastKind::MULx:
					case VUInterpFast::UpperFastKind::MULAx:
						return EmitLoadVfWord(rd, ft, 0);
					case VUInterpFast::UpperFastKind::MULy:
					case VUInterpFast::UpperFastKind::MULAy:
						return EmitLoadVfWord(rd, ft, 1);
					case VUInterpFast::UpperFastKind::MULz:
					case VUInterpFast::UpperFastKind::MULAz:
						return EmitLoadVfWord(rd, ft, 2);
					case VUInterpFast::UpperFastKind::MULw:
					case VUInterpFast::UpperFastKind::MULAw:
						return EmitLoadVfWord(rd, ft, 3);
					default:
						return false;
				}
			}

			// Loads the MUL second operand (see the ADD/SUB operand loader above).
			bool EmitLoadUpperMulOperand(unsigned qd, u32 code,
				VUInterpFast::UpperFastKind kind, int* selected_s)
			{
				if (!selected_s)
					return false;
				*selected_s = -1;
				if (IsUpperVectorOperandForm(kind))
					return EmitLoadVfQuad(qd, VUInterpFast::Ft(code));
				const int broadcast_lane = UpperVfBroadcastLane(kind);
				if (broadcast_lane >= 0)
					return EmitLoadVfLaneBroadcastSelected(qd, VUInterpFast::Ft(code),
						static_cast<unsigned>(broadcast_lane), true,
						selected_s);

				if (!EmitLoadUpperMulOperandWord(3, code, kind, 0))
					return false;
				*selected_s = static_cast<int>(qd * 4);
				m_single_d_broadcast_operands++;
				return m_code.EmitVdupI32DFromCore(qd * 2, 3);
			}

			bool EmitLoadUpperMaddMsubOperandWord(unsigned rd, u32 code,
				VUInterpFast::UpperFastKind kind, unsigned lane)
			{
				const unsigned ft = VUInterpFast::Ft(code);
				switch (kind)
				{
					case VUInterpFast::UpperFastKind::MADD:
					case VUInterpFast::UpperFastKind::MADDA:
					case VUInterpFast::UpperFastKind::MSUB:
					case VUInterpFast::UpperFastKind::MSUBA:
						return EmitLoadVfWord(rd, ft, lane);
					case VUInterpFast::UpperFastKind::MADDi:
					case VUInterpFast::UpperFastKind::MADDAi:
					case VUInterpFast::UpperFastKind::MSUBi:
					case VUInterpFast::UpperFastKind::MSUBAi:
						return EmitLoadViWordRaw(rd, REG_I);
					case VUInterpFast::UpperFastKind::MADDq:
					case VUInterpFast::UpperFastKind::MADDAq:
					case VUInterpFast::UpperFastKind::MSUBq:
					case VUInterpFast::UpperFastKind::MSUBAq:
						return EmitLoadViWordRaw(rd, REG_Q);
					case VUInterpFast::UpperFastKind::MADDx:
					case VUInterpFast::UpperFastKind::MADDAx:
					case VUInterpFast::UpperFastKind::MSUBx:
					case VUInterpFast::UpperFastKind::MSUBAx:
						return EmitLoadVfWord(rd, ft, 0);
					case VUInterpFast::UpperFastKind::MADDy:
					case VUInterpFast::UpperFastKind::MADDAy:
					case VUInterpFast::UpperFastKind::MSUBy:
					case VUInterpFast::UpperFastKind::MSUBAy:
						return EmitLoadVfWord(rd, ft, 1);
					case VUInterpFast::UpperFastKind::MADDz:
					case VUInterpFast::UpperFastKind::MADDAz:
					case VUInterpFast::UpperFastKind::MSUBz:
					case VUInterpFast::UpperFastKind::MSUBAz:
						return EmitLoadVfWord(rd, ft, 2);
					case VUInterpFast::UpperFastKind::MADDw:
					case VUInterpFast::UpperFastKind::MADDAw:
					case VUInterpFast::UpperFastKind::MSUBw:
					case VUInterpFast::UpperFastKind::MSUBAw:
						return EmitLoadVfWord(rd, ft, 3);
					default:
						return false;
				}
			}

			// Loads the MADD/MSUB second operand. Vector forms use a complete Q
			// register; broadcast forms use the one-word D representation.
			bool EmitLoadUpperMaddMsubOperand(unsigned qd, u32 code,
				VUInterpFast::UpperFastKind kind, int* selected_s)
			{
				if (!selected_s)
					return false;
				*selected_s = -1;
				if (IsUpperMaddMsubVectorForm(kind))
					return EmitLoadVfQuad(qd, VUInterpFast::Ft(code));
				const int broadcast_lane = UpperVfBroadcastLane(kind);
				if (broadcast_lane >= 0)
					return EmitLoadVfLaneBroadcastSelected(qd, VUInterpFast::Ft(code),
						static_cast<unsigned>(broadcast_lane), true,
						selected_s);

				if (!EmitLoadUpperMaddMsubOperandWord(3, code, kind, 0))
					return false;
				*selected_s = static_cast<int>(qd * 4);
				m_single_d_broadcast_operands++;
				return m_code.EmitVdupI32DFromCore(qd * 2, 3);
			}

			bool EmitClearMacLaneInReg(unsigned mac_reg, unsigned lane, unsigned scratch_reg)
			{
				const unsigned shift = 3 - lane;
				return EmitAndRegImm32(mac_reg, mac_reg, ~(0x1111u << shift), scratch_reg);
			}

			bool EmitUpdateMacLaneFromResult(unsigned mac_reg, unsigned value_reg, unsigned lane,
				unsigned temp_reg, unsigned scratch_reg)
			{
				const unsigned shift = 3 - lane;
				const u32 sign_bit = 0x0010u << shift;
				const u32 zero_bit = 0x0001u << shift;
				const u32 under_bit = 0x0100u << shift;
				const u32 over_bit = 0x1000u << shift;
				const bool overflow_clamp = CHECK_VU_OVERFLOW(1);

				// PCSX2 owner: VUflags.cpp::VU_MAC_UPDATE(). Recreate the
				// MAC lane flags and returned result bits without calling the
				// C++ helper from the generated VU1 block.
				if (!EmitAndRegImm32(mac_reg, mac_reg, ~sign_bit, scratch_reg) ||
					!m_code.EmitMovRegShiftImm(temp_reg, value_reg, ShiftType::LSR, 31) ||
					!m_code.EmitMovRegShiftImm(temp_reg, temp_reg, ShiftType::LSL, 4 + shift) ||
					!m_code.EmitOrrReg(mac_reg, mac_reg, temp_reg) ||
					!EmitAndRegImm32(temp_reg, value_reg, ~FPU_FLOAT_SIGN_MASK, scratch_reg) ||
					!m_code.EmitCmpImm32(temp_reg, 0))
				{
					return false;
				}

				const size_t nonzero = m_code.EmitBranchPlaceholder(Condition::NE);
				if (nonzero == static_cast<size_t>(-1))
					return false;

				if (!EmitAndRegImm32(mac_reg, mac_reg, ~((under_bit | over_bit)), scratch_reg) ||
					!EmitOrrRegImm32(mac_reg, mac_reg, zero_bit, scratch_reg))
				{
					return false;
				}
				const size_t done_zero = m_code.EmitBranchPlaceholder();
				if (done_zero == static_cast<size_t>(-1))
					return false;

				const size_t nonzero_target = m_code.Size();
				if (!m_code.PatchBranch(nonzero, nonzero_target, Condition::NE) ||
					!EmitAndRegImm32(temp_reg, value_reg, FPU_FLOAT_EXPONENT_MASK, scratch_reg) ||
					!m_code.EmitCmpImm32(temp_reg, 0))
				{
					return false;
				}

				const size_t exponent_nonzero = m_code.EmitBranchPlaceholder(Condition::NE);
				if (exponent_nonzero == static_cast<size_t>(-1))
					return false;

				if (!EmitAndRegImm32(mac_reg, mac_reg, ~over_bit, scratch_reg) ||
					!EmitOrrRegImm32(mac_reg, mac_reg, under_bit | zero_bit, scratch_reg) ||
					!EmitAndRegImm32(value_reg, value_reg, FPU_FLOAT_SIGN_MASK, scratch_reg))
				{
					return false;
				}
				const size_t done_denormal = m_code.EmitBranchPlaceholder();
				if (done_denormal == static_cast<size_t>(-1))
					return false;

				const size_t exponent_nonzero_target = m_code.Size();
				if (!m_code.PatchBranch(exponent_nonzero, exponent_nonzero_target, Condition::NE) ||
					!EmitCmpRegImm32(temp_reg, FPU_FLOAT_EXPONENT_MASK, scratch_reg))
				{
					return false;
				}

				const size_t finite = m_code.EmitBranchPlaceholder(Condition::NE);
				if (finite == static_cast<size_t>(-1))
					return false;

				if (!EmitAndRegImm32(mac_reg, mac_reg, ~(under_bit | zero_bit), scratch_reg) ||
					!EmitOrrRegImm32(mac_reg, mac_reg, over_bit, scratch_reg))
				{
					return false;
				}
				if (overflow_clamp &&
					(!EmitAndRegImm32(value_reg, value_reg, FPU_FLOAT_SIGN_MASK, scratch_reg) ||
						!EmitOrrRegImm32(value_reg, value_reg, FPU_FLOAT_MAX_FINITE, scratch_reg)))
				{
					return false;
				}
				const size_t done_special = m_code.EmitBranchPlaceholder();
				if (done_special == static_cast<size_t>(-1))
					return false;

				const size_t finite_target = m_code.Size();
				if (!m_code.PatchBranch(finite, finite_target, Condition::NE) ||
					!EmitAndRegImm32(mac_reg, mac_reg, ~(over_bit | under_bit | zero_bit), scratch_reg))
				{
					return false;
				}

				const size_t done = m_code.Size();
				return m_code.PatchBranch(done_zero, done) &&
					m_code.PatchBranch(done_denormal, done) &&
					m_code.PatchBranch(done_special, done);
			}

			bool EmitUpdateStatusFromMacReg(unsigned mac_reg, unsigned status_reg,
				unsigned /*temp_reg*/, bool publish_working = true)
			{
				// PCSX2 owner: VUflags.cpp::VU_STAT_UPDATE(). Each MAC nibble
				// contributes one Status bit when any of its four XYZW lanes is
				// set. A32 conditional execution maps the four PCSX2 tests directly:
				// no branch and no temporary zero/materialize/merge sequence. Keeping
				// the tests independent also avoids the long dependent shift chain
				// which measured slower on the real Cortex-A9 despite smaller code.
				if (!m_code.EmitMovImm8(status_reg, 0))
					return false;

				constexpr std::array<std::pair<u32, u8>, 4> groups = {{
					{0x000fu, 0x1},
					{0x00f0u, 0x2},
					{0x0f00u, 0x4},
					{0xf000u, 0x8},
				}};

				for (const auto& [mask, bit] : groups)
				{
					if (!m_code.EmitTstImm32(mac_reg, mask) ||
						!m_code.EmitOrrImm32(status_reg, status_reg, bit, false, Condition::NE))
					{
						return false;
					}
				}

				return !publish_working ||
					m_code.EmitStrImm12(status_reg, HOST_VU, VuOffset(offsetof(VURegs, statusflag)));
			}

			bool EmitLoadWorkingFmacMac(unsigned target_reg)
			{
				if (m_plan.resident_working_fmac_flags && m_latest_working_fmac_entry)
				{
					return m_code.EmitLdrImm12(target_reg, SP,
						LocalFmacOffset(*m_latest_working_fmac_entry, LOCAL_FMAC_MAC_OFFSET));
				}

				return m_code.EmitLdrImm12(target_reg, HOST_VU,
					VuOffset(offsetof(VURegs, macflag)));
			}

			bool EmitLoadWorkingFmacMacStatus(unsigned mac_reg, unsigned status_reg)
			{
				if (mac_reg + 1 != status_reg || (mac_reg & 1u) != 0)
					return false;
				if (m_plan.resident_working_fmac_flags && m_latest_working_fmac_entry)
				{
					return m_code.EmitLdrdImm8(mac_reg, status_reg, SP,
						static_cast<u8>(LocalFmacOffset(*m_latest_working_fmac_entry,
							LOCAL_FMAC_MAC_OFFSET)));
				}

				return m_code.EmitLdrImm12(mac_reg, HOST_VU,
						VuOffset(offsetof(VURegs, macflag))) &&
					m_code.EmitLdrImm12(status_reg, HOST_VU,
						VuOffset(offsetof(VURegs, statusflag)));
			}

			bool EmitCapturePendingLocalFmacFlags(unsigned mac_reg, unsigned status_reg)
			{
				if (!m_capture_pending_local_fmac_flags || !m_pending_local_fmac_entry)
					return true;
				if (mac_reg + 1 != status_reg || (mac_reg & 1u) != 0)
					return false;
				if (!m_code.EmitStrdImm8(mac_reg, status_reg, SP,
						static_cast<u8>(LocalFmacOffset(*m_pending_local_fmac_entry,
							LOCAL_FMAC_MAC_OFFSET))))
				{
					return false;
				}
				m_pending_local_fmac_entry->producer_flags_captured = true;
				if (m_plan.resident_working_fmac_flags)
					m_latest_working_fmac_entry = m_pending_local_fmac_entry;
				return true;
			}

			bool EmitStoreMacResultWord(unsigned value_reg, bool acc, unsigned fd, unsigned lane)
			{
				if (acc)
					return EmitStoreAccWord(value_reg, lane);

				if (fd == 0)
					return true;

				return EmitStoreVfWord(value_reg, fd, lane);
			}

			bool EmitStoreMacResultS(unsigned value_sreg, bool acc, unsigned fd, unsigned lane)
			{
				if (acc)
					return EmitStoreAccWordFromS(value_sreg, lane);

				if (fd == 0)
					return true;

				return EmitStoreVfWordFromS(value_sreg, fd, lane);
			}

			bool EmitEnsureFullMacLaneWeights()
			{
				if (m_dead_fmac_sticky_pending)
					return false;
				if (m_full_mac_weights_ready)
					return true;

				// Sony VU User Manual 3.3.1 fixes the full MAC layout as XYZW
				// in every Z/S/U/O nibble. Keep its repeated category-weight row resident
				// instead of paying MOVW/MOVT plus a two-cycle Cortex-A9 VLD1 for
				// every full-mask producer. Q11 is otherwise free until the dead-
				// FMAC sticky accumulator takes ownership of it.
				if (!m_code.EmitMovImm32(3, static_cast<u32>(reinterpret_cast<uptr>(
						VU_FULL_MAC_CATEGORY_WEIGHTS))) ||
					!m_code.EmitVld1Q32Aligned(VU_FULL_MAC_WEIGHTS_Q, 3))
				{
					return false;
				}
				m_full_mac_weights_ready = true;
				return true;
			}

			bool EmitClassifyAndNormalizeMacOrStatusQ(unsigned result_q, unsigned mask, bool mac_result,
				bool accumulate_sticky_only)
			{
				mask &= 0x0f;
				if (mask == 0)
					return m_code.EmitMovImm8(2, 0);
				const unsigned vu_index = m_vu0_memory_map ? 0u : 1u;
				const bool overflow_clamp = CHECK_VU_OVERFLOW(vu_index);
				const bool result_flush_to_zero = (m_vu0_memory_map ?
					EmuConfig.Cpu.VU0FPCR : EmuConfig.Cpu.VU1FPCR).GetFlushToZero();
				// The sign vector is needed while software rewrites denormals or
				// overflow values. Under Vita's FZ/no-overflow-clamp configuration,
				// scalar VFP has already produced the exact signed result: weighted MAC
				// packing derives its all-ones mask with VCLT, and full STATUS can shift
				// the raw result sign directly.
				const bool normalize_requires_sign = !result_flush_to_zero || overflow_clamp;
				const auto reduce_packed_lanes = [&](bool disjoint_lane_bits) {
					// Q2 is D4:D5. Cortex-A9 NEON MPE TRM tables 3-4 and 3-7
					// give D logical/pairwise/permute operations one issue cycle,
					// while each old Q VEXT/VORR consumes two. MAC's lane weights
					// are disjoint, so pairwise addition is exactly bitwise OR.
					if (!m_code.EmitVorrD(4, 4, 5))
						return false;
					if (disjoint_lane_bits)
					{
						if (!m_code.EmitVpaddI32D(4, 4, 4))
							return false;
					}
					else if (!m_code.EmitVrev64I32D(6, 4) ||
						!m_code.EmitVorrD(4, 4, 6))
					{
						return false;
					}
					return m_code.EmitVmovSToCore(2, 8);
				};

				// PCSX2 owners: x86/microVU_Upper.inl::mVUupdateFlags() and
				// x86/microVU_Clamp.inl::mVUclampReg(). Both inspect the same raw
				// FMAC exponent/sign state. Keep those masks live and normalize the
				// selected result quad before packing flags instead of reconstructing
				// them afterward.
				// Sony VU User Manual 2.3 requires a nonzero exponent-zero result to
				// clamp to signed zero and set both U and Z; exponent-all-ones results
				// clamp to signed max finite when PCSX2's overflow option is active.
				if (!EmitEnsureVuFloatNormalizeConstants(overflow_clamp))
				{
					return false;
				}
				if (!m_code.EmitVandQ(VU_NORM_EXPV_Q, result_q, VU_NORM_EXP_Q) ||
					(normalize_requires_sign &&
						!m_code.EmitVandQ(VU_NORM_SIGNV_Q, result_q, VU_NORM_SIGN_Q)) ||
					!m_code.EmitVceqI32ZeroQ(VU_NORM_MASK_Q, VU_NORM_EXPV_Q) ||
					!m_code.EmitVceqI32Q(VU_NORM_EXPV_Q, VU_NORM_EXPV_Q, VU_NORM_EXP_Q))
				{
					return false;
				}
				// PCSX2 installs the selected VU FPCR around native execution and
				// VMManager::CheckForCPUConfigChanges() resets every VU code cache
				// when it changes. With FZ set, scalar VFP cannot leave a nonzero
				// exponent-zero result in the result quad, so U is provably clear. Preserve the
				// complete Sony U+Z classifier for the non-default FZ-disabled mode.
				if (!result_flush_to_zero &&
					(!m_code.EmitVshlI32Q(VU_NORM_TMP_Q, result_q, 1) ||
					 !m_code.EmitVceqI32ZeroQ(VU_NORM_TMP_Q, VU_NORM_TMP_Q) ||
					 !m_code.EmitVmvnQ(3, VU_NORM_TMP_Q) ||
					 !m_code.EmitVandQ(3, 3, VU_NORM_MASK_Q)))
				{
					return false;
				}
				// ARM ARM A2.7.5 requires VFPv3 flush-to-zero to preserve the
				// sign of a flushed result. Under the installed FZ FPCR, the selected
				// result quad therefore already contains the exact signed-zero VU result and the Q-form
				// VBIT (two issue cycles on Cortex-A9) would only rewrite identical
				// bits. FZ-disabled blocks can still contain a nonzero exponent-zero
				// result, so retain the complete PCSX2 vuFloat() normalization there.
				if (!result_flush_to_zero &&
					!m_code.EmitVbitQ(result_q, VU_NORM_SIGNV_Q, VU_NORM_MASK_Q))
				{
					return false;
				}
				if (overflow_clamp)
				{
					if (!m_code.EmitVorrQ(1, VU_NORM_SIGNV_Q, VU_NORM_MAXF_Q) ||
						!m_code.EmitVbitQ(result_q, 1, VU_NORM_EXPV_Q))
					{
						return false;
					}
				}
				// When mFLAG.doFlag is false, the same per-lane sign/zero/underflow/
				// overflow classification is reduced directly into STATUS's four
				// category bits; it does not construct the 16-bit XYZW MAC layout.
				// Vita keeps one NEON body and selects the lane weights and category
				// shifts at compile time. Both forms return their packed word in r2.
				if (!mac_result && mask == 0x0f)
				{
					if (accumulate_sticky_only)
					{
						// FZ makes U provably clear. Shift the all-ones/all-zero overflow
						// comparison left three positions: the low nibble now has only O
						// in bit 3. VSRI then inserts sign and zero into bits 1 and 0 while
						// preserving bit 2 as zero. Higher all-one bits are harmless and are
						// masked once after the block-wide reduction. The first producer
						// builds Q11 directly, avoiding both a zero and a self-OR.
						if (!m_dead_fmac_sticky_pending)
							m_full_mac_weights_ready = false;
						const unsigned packed_q = m_dead_fmac_sticky_pending ? 2u :
							VU_DEAD_FMAC_STICKY_Q;
						if (!m_code.EmitVshlI32Q(packed_q, VU_NORM_EXPV_Q, 3) ||
							!m_code.EmitVsriI32Q(packed_q, result_q, 30) ||
							!m_code.EmitVsriI32Q(packed_q, VU_NORM_MASK_Q, 31) ||
							(m_dead_fmac_sticky_pending &&
								!m_code.EmitVorrQ(VU_DEAD_FMAC_STICKY_Q,
									VU_DEAD_FMAC_STICKY_Q, packed_q)))
						{
							return false;
						}
						m_dead_fmac_sticky_pending = true;
						return true;
					}

					// Sony VU User Manual 3.3.2 fixes the low STATUS nibble as
					// Z/S/U/O. Build those four all-lane category bits directly,
					// then OR-reduce the D halves. VSRI first inserts U into bits
					// [2:0], then the normalized result sign into [1:0], and finally
					// Z into bit 0; each later insert deliberately overwrites the
					// irrelevant lower source bits. O starts in bit 3 and is
					// preserved throughout. Under FZ, U is provably zero and its
					// insert disappears.
					if (!m_code.EmitVshlI32Q(2, VU_NORM_EXPV_Q, 3) ||
						(!result_flush_to_zero && !m_code.EmitVsriI32Q(2, 3, 29)) ||
						!m_code.EmitVsriI32Q(2, result_q, 30) ||
						!m_code.EmitVsriI32Q(2, VU_NORM_MASK_Q, 31))
					{
						return false;
					}

					return reduce_packed_lanes(false);
				}

				const bool full_mac_result = mac_result && mask == 0x0f;
				unsigned weights_q = 1;
				if (full_mac_result && !m_dead_fmac_sticky_pending)
				{
					if (!EmitEnsureFullMacLaneWeights())
						return false;
					weights_q = VU_FULL_MAC_WEIGHTS_Q;
				}
				else
				{
					const u32* weights = full_mac_result ? VU_FULL_MAC_CATEGORY_WEIGHTS :
						(mac_result ? VU_MAC_LANE_WEIGHTS[mask] :
							VU_STATUS_LANE_WEIGHTS[mask]);
					if (!m_code.EmitMovImm32(3,
							static_cast<u32>(reinterpret_cast<uptr>(weights))) ||
						!m_code.EmitVld1Q32Aligned(weights_q, 3))
					{
						return false;
					}
				}
				const u8 sign_shift = mac_result ? 4 : 1;
				const u8 underflow_shift = mac_result ? 8 : 2;
				const u8 overflow_shift = mac_result ? 12 : 3;
				if (!m_code.EmitVcltS32ZeroQ(VU_NORM_SIGNV_Q, result_q))
					return false;
				if (full_mac_result)
				{
					// Sony VU User Manual 3.3.1 fixes MAC as four XYZW
					// nibbles ordered Z/S/U/O. VSRI writes the lower source
					// bits and preserves the higher destination bits, so these
					// operations assemble those four all-zero/all-one category
					// nibbles directly. One final VAND with repeated 8/4/2/1
					// lane weights replaces three weighted masks, three shifts,
					// and two merges on the dominant full-result path.
					if (!m_code.EmitVshlI32Q(2, VU_NORM_EXPV_Q, 12) ||
						(!result_flush_to_zero && !m_code.EmitVsriI32Q(2, 3, 20)) ||
						!m_code.EmitVsriI32Q(2, VU_NORM_SIGNV_Q, 24) ||
						!m_code.EmitVsriI32Q(2, VU_NORM_MASK_Q, 28) ||
						!m_code.EmitVandQ(2, 2, weights_q))
					{
						return false;
					}
					return reduce_packed_lanes(true);
				}

				if (!m_code.EmitVandQ(2, VU_NORM_MASK_Q, weights_q) ||
					!m_code.EmitVandQ(VU_NORM_SIGNV_Q, VU_NORM_SIGNV_Q, weights_q) ||
					!m_code.EmitVshlI32Q(VU_NORM_SIGNV_Q, VU_NORM_SIGNV_Q, sign_shift) ||
					!m_code.EmitVorrQ(2, 2, VU_NORM_SIGNV_Q))
				{
					return false;
				}
				if (!result_flush_to_zero &&
					(!m_code.EmitVandQ(3, 3, weights_q) ||
					 !m_code.EmitVshlI32Q(3, 3, underflow_shift) ||
					 !m_code.EmitVorrQ(2, 2, 3)))
				{
					return false;
				}
				return m_code.EmitVandQ(VU_NORM_EXPV_Q, VU_NORM_EXPV_Q, weights_q) &&
					m_code.EmitVshlI32Q(VU_NORM_EXPV_Q, VU_NORM_EXPV_Q, overflow_shift) &&
					m_code.EmitVorrQ(2, 2, VU_NORM_EXPV_Q) &&
					reduce_packed_lanes(mac_result);
			}

			bool EmitFinishMacQ(unsigned result_q, bool acc, unsigned fd, unsigned mask, bool preserve_inactive,
				bool mac_result_required)
			{
				mask &= 0x0f;
				if (!mac_result_required && preserve_inactive)
					return false;
				const bool accumulate_sticky_only = !mac_result_required && mask == 0x0f &&
					m_plan.deferred_fmac_flags && m_plan.resident_working_fmac_flags &&
					m_capture_pending_local_fmac_flags && m_pending_local_fmac_entry &&
					VuFpcrFlushesInputsToZero() &&
					!CHECK_VU_OVERFLOW(m_vu0_memory_map ? 0u : 1u) &&
					(FmacFlagReg(m_pairs[m_pending_local_fmac_entry->pair_index]) &
						(1u << REG_CLIP_FLAG)) == 0;
				if (!EmitClassifyAndNormalizeMacOrStatusQ(result_q, mask, mac_result_required,
						accumulate_sticky_only))
					return false;
				if (accumulate_sticky_only)
					m_pending_local_fmac_entry->sticky_only_accumulated = true;

				if (mac_result_required && preserve_inactive)
				{
					const u32 active_mac_bits = mask * 0x1111u;
					if (!EmitLoadWorkingFmacMac(0) ||
						!EmitAndRegImm32(0, 0, ~active_mac_bits, HOST_CALL_SCRATCH) ||
						!m_code.EmitOrrReg(2, 2, 0))
					{
						return false;
					}
				}

				// PCSX2 owner: microVU_IR.h::microRegAlloc::writeBackReg(). A
				// complete XYZW result remains one cached SIMD value and is written
				// back with mVUsaveReg() as a whole. The result quad already contains
				// the exact normalized result, so do not split it into four scalar stores. On
				// Cortex-A9 that also avoids rematerializing ACC's out-of-range base
				// separately for every lane and exposes the complete result to the
				// block-local Q-register allocator.
				bool result_stored = true;
				if (mask == 0x0f)
				{
					result_stored = acc ? EmitStoreAccQuad(result_q) : EmitStoreVfQuad(result_q, fd);
				}
				else
				{
					for (unsigned lane = 0; lane < 4; lane++)
					{
						if ((mask & (1u << (3 - lane))) != 0 &&
							!EmitStoreMacResultS(result_q * 4 + lane, acc, fd, lane))
						{
							result_stored = false;
							break;
						}
					}
				}
				if (!result_stored)
					return false;

				const unsigned status_reg = m_capture_pending_local_fmac_flags ? 3u : 0u;
				const bool defer_working_store = m_plan.resident_working_fmac_flags &&
					m_capture_pending_local_fmac_flags;
				if (accumulate_sticky_only)
				{
					// The result quad still writes back normally. This producer's MAC and
					// non-sticky STATUS instance are liveness-proven unobservable; its
					// sticky categories are already resident in Q11.
				}
				else if (mac_result_required)
				{
					if ((!defer_working_store &&
							!m_code.EmitStrImm12(2, HOST_VU, VuOffset(offsetof(VURegs, macflag)))) ||
						!EmitUpdateStatusFromMacReg(2, status_reg, 1, !defer_working_store) ||
						!EmitCapturePendingLocalFmacFlags(2, status_reg))
					{
						return false;
					}
				}
				else
				{
					// r2 already is the exact non-sticky STATUS category nibble.
					// Preserve the previous working MAC in this otherwise unobservable
					// local pipeline instance so STATUS/sticky publication remains exact.
					if (!EmitMovReg(status_reg, 2) ||
						(!defer_working_store && !m_code.EmitStrImm12(status_reg, HOST_VU,
							VuOffset(offsetof(VURegs, statusflag)))) ||
						(m_capture_pending_local_fmac_flags &&
							(!EmitLoadWorkingFmacMac(2) ||
							 !EmitCapturePendingLocalFmacFlags(2, status_reg))))
					{
						return false;
					}
				}

				// PCSX2 microVU's register allocator retains the clamped FMAC
				// representation. EmitFinishMacQ() just applied the same vuFloat()
				// normalization to every active result lane, so later FMAC operands
				// in this straight-line block do not need another vuDouble() pass.
				if (acc)
					MarkVectorLanesNormalized(VU_VECTOR_CACHE_ACC, static_cast<u8>(mask));
				else if (fd != 0)
					MarkVectorLanesNormalized(static_cast<u8>(fd), static_cast<u8>(mask));
				return true;
			}

			bool EmitApplyTriAceAddHack(unsigned fs_reg, unsigned operand_reg, unsigned diff_reg,
				unsigned scratch_reg)
			{
				// PCSX2 owner: VUops.cpp::vuADD_TriAceHack() /
				// VUmicroFast.h::ApplyTriAceAddHackNeon(). This runs before
				// vuDouble() normalization and only applies to ADDi when the
				// gamefix is enabled.
				if (!m_code.EmitMovRegShiftImm(diff_reg, fs_reg, ShiftType::LSR, 23) ||
					!m_code.EmitAndImm32(diff_reg, diff_reg, 0xff) ||
					!m_code.EmitMovRegShiftImm(scratch_reg, operand_reg, ShiftType::LSR, 23) ||
					!m_code.EmitAndImm32(scratch_reg, scratch_reg, 0xff) ||
					!m_code.EmitSubReg(diff_reg, diff_reg, scratch_reg) ||
					!m_code.EmitCmpImm32(diff_reg, 25))
				{
					return false;
				}

				const size_t keep_operand = m_code.EmitBranchPlaceholder(Condition::LT);
				if (keep_operand == static_cast<size_t>(-1))
					return false;
				if (!EmitAndRegImm32(operand_reg, operand_reg, FPU_FLOAT_SIGN_MASK, scratch_reg))
					return false;
				const size_t have_operand = m_code.Size();
				if (!m_code.PatchBranch(keep_operand, have_operand, Condition::LT))
					return false;

				if (!EmitCmpRegImm32(diff_reg, 0xffffffe7u, scratch_reg))
					return false;
				const size_t keep_fs = m_code.EmitBranchPlaceholder(Condition::GT);
				if (keep_fs == static_cast<size_t>(-1))
					return false;
				if (!EmitAndRegImm32(fs_reg, fs_reg, FPU_FLOAT_SIGN_MASK, scratch_reg))
					return false;
				return m_code.PatchBranch(keep_fs, m_code.Size(), Condition::GT);
			}

			bool EmitInlineUpperAddSub(u32 code, VUInterpFast::UpperFastKind kind,
				bool mac_result_required)
			{
				const unsigned fd = VUInterpFast::Fd(code);
				const unsigned fs = VUInterpFast::Fs(code);
				const unsigned mask = VUInterpFast::XYZW(code);
				const bool acc = IsUpperAddSubAccKind(kind);
				const bool subtract = IsUpperSubKind(kind);
				const bool triace_add = IsUpperAddiTriAceKind(kind) && CHECK_VUADDSUBHACK;
				int operand_s = -1;

				// PCSX2 owners: VUops.cpp::_vuADD* / _vuSUB* /
				// _vuADDA* / _vuSUBA* and
				// VUmicroFast.h::ExecuteAddSubMasked(). NEON still owns qword
				// loads and exact integer vuDouble() normalization, but Cortex-A9
				// Advanced SIMD FP ignores FPSCR rounding. Execute only the active
				// arithmetic lanes with scalar VFP under the installed VU FPCR.
				if (triace_add)
				{
					// The VUADDSUBHACK gamefix rewrites operands based on their
					// exponents before vuDouble(); keep it on the per-lane scalar
					// path where EmitApplyTriAceAddHack() operates on GPR words.
					for (unsigned lane = 0; lane < 4; lane++)
					{
						const unsigned lane_bit = 1u << (3 - lane);
						if ((mask & lane_bit) == 0)
							continue;

						if (!EmitLoadVfWord(0, fs, lane) ||
							!EmitLoadUpperAddSubOperandWord(1, code, kind, lane) ||
							!EmitApplyTriAceAddHack(0, 1, 3, HOST_CALL_SCRATCH) ||
							!EmitNormalizeVuFloatWord(0, 3, HOST_CALL_SCRATCH) ||
							!m_code.EmitVmovCoreToS(lane, 0) ||
							!EmitNormalizeVuFloatWord(1, 3, HOST_CALL_SCRATCH) ||
							!m_code.EmitVmovCoreToS(4 + lane, 1))
						{
							return false;
						}
					}
				}
				else if (mask != 0)
				{
					// Load fs as Q0 and the second operand as either vector Q1 or a
					// one-word D2 broadcast. Normalize in NEON, avoiding per-lane
					// vuDouble() work and ARM-to-VFP single-register transfers.
					const bool fs_normalized = AreVectorLanesNormalized(
						static_cast<u8>(fs), static_cast<u8>(mask));
					const bool operand_normalized = IsUpperOperandNormalized(code, kind,
						static_cast<u8>(mask), IsUpperVectorOperandForm(kind));
					if (!EmitLoadVfQuad(0, fs) ||
						!EmitLoadUpperAddSubOperand(1, code, kind, &operand_s))
					{
						return false;
					}
					if (!EmitNormalizeVuFloatQuadAndOperandKnown(0, fs_normalized,
						1, operand_s, operand_normalized))
						return false;
				}

				// A complete ADD/SUB-family write can use an existing destination cache
				// quad after both operands have been copied out. This includes ADDA/SUBA
				// and VF destinations, even when Fd aliases a source. Inactive lanes
				// prohibit this for masked writes because classification is allowed to
				// normalize every lane in the result quad.
				const unsigned result_q = mask == 0x0f ?
					SelectCachedCompleteOverwriteQ(acc ? VU_VECTOR_CACHE_ACC :
						static_cast<u8>(fd), 0) : 0;

				for (unsigned lane = 0; lane < 4; lane++)
				{
					const unsigned lane_bit = 1u << (3 - lane);
					if ((mask & lane_bit) == 0)
						continue;
					if (subtract ?
						!m_code.EmitVsubF32(result_q * 4 + lane,
							lane, operand_s >= 0 ?
								static_cast<unsigned>(operand_s) : 4 + lane) :
						!m_code.EmitVaddF32(result_q * 4 + lane,
							lane, operand_s >= 0 ?
								static_cast<unsigned>(operand_s) : 4 + lane))
						return false;
				}

				if (!EmitFinishMacQ(result_q, acc, fd, mask, false, mac_result_required))
					return false;

#if defined(VITASX2_QEMU_VALIDATION)
				return EmitQemuUpperAddSubInlineCounter();
#else
				return true;
#endif
			}

			bool EmitInlineUpperMul(u32 code, VUInterpFast::UpperFastKind kind,
				bool mac_result_required)
			{
				const unsigned fd = VUInterpFast::Fd(code);
				const unsigned fs = VUInterpFast::Fs(code);
				const unsigned mask = VUInterpFast::XYZW(code);
				const bool acc = IsUpperMulAccKind(kind);
				int operand_s = -1;

				// PCSX2 owners: VUops.cpp::_vuMUL* / _vuMULA* and
				// VUmicroFast.h::ExecuteMulMasked(). Normalize operands in NEON,
				// then use scalar VFP per active lane because Advanced SIMD VMUL
				// is fixed round-to-nearest and cannot implement PCSX2's VU
				// chop-zero MXCSR/FPSCR contract.
				if (mask != 0)
				{
					// Load fs as Q0 and the second operand as either vector Q1 or a
					// one-word D2 broadcast. Normalize in NEON, avoiding per-lane
					// vuDouble() work and ARM-to-VFP single-register transfers.
					const bool fs_normalized = AreVectorLanesNormalized(
						static_cast<u8>(fs), static_cast<u8>(mask));
					const bool operand_normalized = IsUpperOperandNormalized(code, kind,
						static_cast<u8>(mask), IsUpperVectorOperandForm(kind));
					if (!EmitLoadVfQuad(0, fs) ||
						!EmitLoadUpperMulOperand(1, code, kind, &operand_s))
					{
						return false;
					}
					if (!EmitNormalizeVuFloatQuadAndOperandKnown(0, fs_normalized,
						1, operand_s, operand_normalized))
						return false;
				}

				// A complete MUL-family write has the same overwrite property. Reuse an
				// existing ACC or VF cache mapping as the scalar destination so the
				// following whole-quad store only marks it dirty instead of emitting a
				// Q-form VMOV.
				const unsigned result_q = mask == 0x0f ?
					SelectCachedCompleteOverwriteQ(acc ? VU_VECTOR_CACHE_ACC :
						static_cast<u8>(fd), 0) : 0;

				for (unsigned lane = 0; lane < 4; lane++)
				{
					const unsigned lane_bit = 1u << (3 - lane);
					if ((mask & lane_bit) != 0 &&
						!m_code.EmitVmulF32(result_q * 4 + lane,
							lane, operand_s >= 0 ?
								static_cast<unsigned>(operand_s) : 4 + lane))
					{
						return false;
					}
				}

				if (!EmitFinishMacQ(result_q, acc, fd, mask, false, mac_result_required))
					return false;

#if defined(VITASX2_QEMU_VALIDATION)
				return EmitQemuUpperMulInlineCounter();
#else
				return true;
#endif
			}

			bool EmitInlineUpperMaddMsub(u32 code, VUInterpFast::UpperFastKind kind,
				bool mac_result_required)
			{
				const unsigned fd = VUInterpFast::Fd(code);
				const unsigned fs = VUInterpFast::Fs(code);
				const unsigned ft = VUInterpFast::Ft(code);
				const unsigned mask = VUInterpFast::XYZW(code);
				const bool acc = IsUpperMaddMsubAccKind(kind);
				const bool subtract = IsUpperMsubKind(kind);

				// PCSX2 owners: VUops.cpp::_vuMADD* / _vuMSUB* /
				// _vuMADDA* / _vuMSUBA* and VUmicroFast.h::VuMaddMsubScalar() /
				// ExecuteMaddMsubMaskedScalar(). The multiply/add stays scalar VFP
				// because qword NEON drifts by 1 ULP in dependent ACC chains, but
				// vuDouble() input normalization is exact integer bit work: load
				// ACC/fs as NEON quads and the operand as either a Q vector or a D
				// broadcast, normalize them in place (matching VuDoubleBitsNeon()),
				// then run each active lane's scalar vmul/vadd. A VF-lane
				// broadcast whose result aliases Ft keeps the per-lane scalar path
				// so a later lane still observes the just-written Ft, matching
				// ExecuteMaddMsubMaskedScalar()'s store order.
				const bool alias_hazard = IsUpperMaddMsubVfBroadcastForm(kind) && fd == ft;
				unsigned result_q = 0;

				if (alias_hazard && !EmitLoadWorkingFmacMac(2))
					return false;

				if (alias_hazard)
				{
					for (unsigned lane = 0; lane < 4; lane++)
					{
						const unsigned lane_bit = 1u << (3 - lane);
						if ((mask & lane_bit) == 0)
						{
							if (!EmitClearMacLaneInReg(2, lane, HOST_CALL_SCRATCH))
								return false;
							continue;
						}

						if (!EmitLoadAccWord(0, lane) ||
							!EmitNormalizeVuFloatWord(0, 3, HOST_CALL_SCRATCH) ||
							!m_code.EmitVmovCoreToS(0, 0) ||
							!EmitLoadVfWord(0, fs, lane) ||
							!EmitNormalizeVuFloatWord(0, 3, HOST_CALL_SCRATCH) ||
							!m_code.EmitVmovCoreToS(1, 0) ||
							!EmitLoadUpperMaddMsubOperandWord(0, code, kind, lane) ||
							!EmitNormalizeVuFloatWord(0, 3, HOST_CALL_SCRATCH) ||
							!m_code.EmitVmovCoreToS(2, 0) ||
							!m_code.EmitVmulF32(1, 1, 2) ||
							(subtract ? !m_code.EmitVsubF32(0, 0, 1) : !m_code.EmitVaddF32(0, 0, 1)) ||
							!m_code.EmitVmovSToCore(0, 0) ||
							!EmitUpdateMacLaneFromResult(2, 0, lane, 1, HOST_CALL_SCRATCH) ||
							!EmitStoreMacResultWord(0, acc, fd, lane))
						{
							return false;
						}
					}
				}
				else
				{
					int operand_s = -1;
					if (mask != 0)
					{
						const bool acc_normalized = AreVectorLanesNormalized(
							VU_VECTOR_CACHE_ACC, static_cast<u8>(mask));
						const bool fs_normalized = AreVectorLanesNormalized(
							static_cast<u8>(fs), static_cast<u8>(mask));
						const bool operand_normalized = IsUpperOperandNormalized(code, kind,
							static_cast<u8>(mask), IsUpperMaddMsubVectorForm(kind));
						// Read both VF operands before selecting a destination mapping. This
						// makes a complete Fd overwrite safe even when it aliases Fs/Ft, and
						// ensures the selection observes any cache eviction caused by a load.
						if (!EmitLoadVfQuad(1, fs) ||
							!EmitLoadUpperMaddMsubOperand(2, code, kind, &operand_s))
						{
							return false;
						}

						// A full normalized MADDA/MSUBA updates every ACC lane and leaves
						// the same representation behind, so cached ACC can be both input and
						// result. A full MADD/MSUB to an existing cached VF can read normalized
						// ACC directly and copy it once into that final destination, removing
						// the old ACC->Q0 plus Q0->Fd pair of Q moves. Partial masks retain Q0
						// because classifying inactive lanes must not modify architectural state.
						const bool cached_vf_destination = !acc && fd != 0 && mask == 0x0f &&
							VectorCacheEnabled() && FindVectorCacheSlot(static_cast<u8>(fd)) >= 0;
						const bool direct_resident_acc = mask == 0x0f && acc_normalized &&
							(acc || cached_vf_destination);
						unsigned acc_q = 0;
						if (!EmitLoadAccQuadSelected(0, direct_resident_acc, &acc_q))
							return false;
						result_q = acc_q;
						if (!acc && acc_q != 0)
						{
							result_q = SelectCachedCompleteOverwriteQ(static_cast<u8>(fd), 0);
							if (!m_code.EmitVorrQ(result_q, acc_q, acc_q))
								return false;
						}
						if (!EmitNormalizeVuFloatQuadsAndOperandKnown(result_q,
							acc_normalized, 1, fs_normalized, 2, operand_s,
							operand_normalized))
						{
							return false;
						}
					}

					for (unsigned lane = 0; lane < 4; lane++)
					{
						const unsigned lane_bit = 1u << (3 - lane);
						if ((mask & lane_bit) == 0)
						{
							if (!EmitClearMacLaneInReg(2, lane, HOST_CALL_SCRATCH))
								return false;
							continue;
						}

						// ARM ARM A8.6.324 defines scalar VFP VMLA/VMLS as an
						// ordered FPMul followed by FPAdd, which is PCSX2's exact
						// non-fused MADD/MSUB contract. Accumulating directly into
						// the selected result quad also uses Cortex-A9's
						// multiplier-accumulator forwarding
						// instead of VMUL -> VADD/VSUB -> VMOV for every lane.
						if (subtract ?
							!m_code.EmitVmlsF32(result_q * 4 + lane,
								4 + lane, operand_s >= 0 ?
									static_cast<unsigned>(operand_s) : 8 + lane) :
							!m_code.EmitVmlaF32(result_q * 4 + lane,
								4 + lane, operand_s >= 0 ?
									static_cast<unsigned>(operand_s) : 8 + lane))
						{
							return false;
						}
					}
				}

				const unsigned status_reg = m_capture_pending_local_fmac_flags ? 3u : 0u;
				const bool defer_working_store = m_plan.resident_working_fmac_flags &&
					m_capture_pending_local_fmac_flags;
				if (alias_hazard ?
					((!defer_working_store &&
							!m_code.EmitStrImm12(2, HOST_VU, VuOffset(offsetof(VURegs, macflag)))) ||
						!EmitUpdateStatusFromMacReg(2, status_reg, 1, !defer_working_store) ||
						!EmitCapturePendingLocalFmacFlags(2, status_reg)) :
					!EmitFinishMacQ(result_q, acc, fd, mask, false,
						mac_result_required))
				{
					return false;
				}
				if (alias_hazard)
				{
					if (acc)
						MarkVectorLanesNormalized(VU_VECTOR_CACHE_ACC, static_cast<u8>(mask));
					else if (fd != 0)
						MarkVectorLanesNormalized(static_cast<u8>(fd), static_cast<u8>(mask));
				}

#if defined(VITASX2_QEMU_VALIDATION)
				return EmitQemuUpperMaddMsubInlineCounter();
#else
				return true;
#endif
			}

			bool EmitInlineUpperOuter(u32 code, VUInterpFast::UpperFastKind kind)
			{
				const unsigned fd = VUInterpFast::Fd(code);
				const unsigned fs = VUInterpFast::Fs(code);
				const unsigned ft = VUInterpFast::Ft(code);

				// PCSX2 owners: VUops.cpp::_vuOPMULA()/_vuOPMSUB() and
				// VUmicroFast.h::ExecuteOpmula()/ExecuteOpmsub()/OuterProductNeon().
				// W is ignored and its MAC bits are left untouched. Load fs/ft (and
				// ACC for OPMSUB) as NEON quads, normalize them with the shared
				// vuDouble() quad path, then arrange the cross-product lanes with
				// cheap S-register moves instead of per-lane scalar normalize plus
				// ARM->NEON transfers. The three products and subtractions use
				// scalar VFP because Advanced SIMD FP is fixed nearest on Cortex-A9.
				// fs/ft/ACC are all read before any store, so Fd aliases keep the
				// same source visibility as the direct path.
				const bool opmsub = kind != VUInterpFast::UpperFastKind::OPMULA;

				// Q2 = fs, Q3 = ft. OPMSUB initially loads ACC in Q0, then
				// moves it to Q2 only after the rearranged fs lanes have consumed
				// that source. Keeping operand/result temporaries in Q0-Q3 (with
				// normalization constants/scratch in caller-clobbered Q8-Q15) avoids
				// Q4-Q7, which are reserved for block-local VF/ACC residency and
				// preserved once by the generated private frame.
				if (!EmitLoadVfQuad(2, fs) ||
					!EmitLoadVfQuad(3, ft))
				{
					return false;
				}

				if (opmsub)
				{
					if (!EmitLoadAccQuad(0) ||
						!EmitNormalizeVuFloatQuads3(2, 3, 0))
					{
						return false;
					}
				}
				else if (!EmitNormalizeVuFloatQuads(2, 3))
				{
					return false;
				}

				if (!opmsub)
				{
					// fs_yzx = {fs.y, fs.z, fs.x, fs.x} in Q0;
					// ft_zxy = {ft.z, ft.x, ft.y, ft.y} in Q1.
					if (!m_code.EmitVmovS(0, 9) || !m_code.EmitVmovS(1, 10) ||
						!m_code.EmitVmovS(2, 8) || !m_code.EmitVmovS(3, 8) ||
						!m_code.EmitVmovS(4, 14) || !m_code.EmitVmovS(5, 12) ||
						!m_code.EmitVmovS(6, 13) || !m_code.EmitVmovS(7, 13) ||
						!m_code.EmitVmulF32(0, 0, 4) ||
						!m_code.EmitVmulF32(1, 1, 5) ||
						!m_code.EmitVmulF32(2, 2, 6))
					{
						return false;
					}
				}
				else
				{
					// Build fs_yzx in Q1 while Q0 retains ACC, then reuse dead
					// Q2 for ACC and Q0 for ft_zxy. VMUL keeps PCSX2's fs*ft
					// operand order for NaN behavior before ACC-product subtraction.
					if (!m_code.EmitVmovS(4, 9) || !m_code.EmitVmovS(5, 10) ||
						!m_code.EmitVmovS(6, 8) || !m_code.EmitVmovS(7, 8) ||
						!m_code.EmitVorrQ(2, 0, 0) ||
						!m_code.EmitVmovS(0, 14) || !m_code.EmitVmovS(1, 12) ||
						!m_code.EmitVmovS(2, 13) || !m_code.EmitVmovS(3, 13) ||
						!m_code.EmitVmulF32(4, 4, 0) ||
						!m_code.EmitVmulF32(5, 5, 1) ||
						!m_code.EmitVmulF32(6, 6, 2) ||
						!m_code.EmitVsubF32(0, 8, 4) ||
						!m_code.EmitVsubF32(1, 9, 5) ||
						!m_code.EmitVsubF32(2, 10, 6))
					{
						return false;
					}
				}

				if (!EmitFinishMacQ(0, !opmsub, fd, 0x0e, true, true))
					return false;

#if defined(VITASX2_QEMU_VALIDATION)
				return EmitQemuUpperOuterInlineCounter();
#else
				return true;
#endif
			}

			bool EmitLoadVifWord(unsigned rd, size_t offset)
			{
				const uptr base = reinterpret_cast<uptr>(m_vu0_memory_map ? &vif0Regs :
					(THREAD_VU1 ? &vu1Thread.vifRegs : &vif1Regs));
				return m_code.EmitMovImm32(3, static_cast<u32>(base + offset)) &&
					m_code.EmitLdrImm12(rd, 3, 0);
			}

			bool EmitAndRegImm32(unsigned rd, unsigned rn, u32 value, unsigned scratch)
			{
				if (m_code.EmitAndImm32(rd, rn, value))
					return true;

				return m_code.EmitMovImm32(scratch, value) &&
					m_code.EmitAndReg(rd, rn, scratch);
			}

			bool EmitOrrRegImm32(unsigned rd, unsigned rn, u32 value, unsigned scratch)
			{
				if (m_code.EmitOrrImm32(rd, rn, value))
					return true;

				return m_code.EmitMovImm32(scratch, value) &&
					m_code.EmitOrrReg(rd, rn, scratch);
			}

			bool EmitCmpRegImm32(unsigned rn, u32 value, unsigned scratch)
			{
				if (m_code.EmitCmpImm32(rn, value))
					return true;

				return m_code.EmitMovImm32(scratch, value) &&
					m_code.EmitCmpReg(rn, scratch);
			}

			bool EmitStoreViBoolFromFlags(unsigned dest, Condition true_condition)
			{
				pxAssert(dest != 0);
				return m_code.EmitMovImm8(0, 0) &&
					m_code.EmitMovImm8(0, 1, true_condition) &&
					EmitStoreViHalfword(0, dest);
			}

			bool EmitAddSignedImmToR0(s32 imm)
			{
				if (imm == 0)
					return true;
				if (imm > 0)
					return imm <= 255 ? m_code.EmitAddImm8(0, 0, static_cast<u8>(imm)) :
						(m_code.EmitMovImm32(1, static_cast<u32>(imm)) && m_code.EmitAddReg(0, 0, 1));

				const u32 magnitude = static_cast<u32>(-imm);
				return magnitude <= 255 ? m_code.EmitSubImm8(0, 0, static_cast<u8>(magnitude)) :
					(m_code.EmitMovImm32(1, magnitude) && m_code.EmitSubReg(0, 0, 1));
			}

			bool EmitVuDataMemoryPointerFromRawByteAddress(unsigned mask_scratch = 2)
			{
				// PCSX2 owner: VUops.cpp::GET_VU_MEM(). VU1 wraps to VU1 data
				// RAM; VU0 either wraps to VU0 data RAM or maps 0x4000-tagged
				// addresses into the first 0x400 bytes of VU1's VF/VI register
				// window.
				if (m_vu0_memory_map)
				{
					if (!m_code.EmitTstImm32(1, 0x4000u))
						return false;
					const size_t data_ram = m_code.EmitBranchPlaceholder(Condition::EQ);
					if (data_ram == static_cast<size_t>(-1))
						return false;

					if (!EmitAndRegImm32(1, 1, 0x3ffu, mask_scratch) ||
						!m_code.EmitMovImm32(0, static_cast<u32>(reinterpret_cast<uptr>(&VU1.VF[0]))) ||
						!m_code.EmitAddReg(0, 0, 1))
					{
						return false;
					}
					const size_t done = m_code.EmitBranchPlaceholder();
					if (done == static_cast<size_t>(-1))
						return false;

					const size_t data_ram_target = m_code.Size();
					if (!m_code.PatchBranch(data_ram, data_ram_target, Condition::EQ))
						return false;
					if (!EmitAndRegImm32(1, 1, m_mem_mask, mask_scratch) ||
						!m_code.EmitLdrImm12(0, HOST_VU, VuOffset(offsetof(VURegs, Mem))) ||
						!m_code.EmitAddReg(0, 0, 1))
					{
						return false;
					}
					return m_code.PatchBranch(done, m_code.Size());
				}

				return EmitAndRegImm32(1, 1, m_mem_mask, mask_scratch) &&
					m_code.EmitLdrImm12(0, HOST_VU, VuOffset(offsetof(VURegs, Mem))) &&
					m_code.EmitAddReg(0, 0, 1);
			}

			bool EmitVuDataMemoryAddressFromQwordIndex(unsigned index_reg,
				unsigned mask_scratch = 2)
			{
				return m_code.EmitMovRegShiftImm(1, index_reg, ShiftType::LSL, 4) &&
					EmitVuDataMemoryPointerFromRawByteAddress(mask_scratch);
			}

			bool EmitVuDataMemoryAddressFromBaseImm(unsigned vi_reg, s32 imm)
			{
				// VU VI00-VI15 are 16-bit. GET_VU_MEM discards every host word
				// bit above that architectural value after the add and << 4, so
				// an aligned LDR is equivalent here and avoids the Cortex-A9's
				// separate address-forming ADD required by LDRSH.
				return EmitLoadViWordRaw(0, vi_reg) &&
					EmitAddSignedImmToR0(imm) &&
					EmitVuDataMemoryAddressFromQwordIndex(0);
			}

			bool EmitVuDataMemoryAddressFromVi(unsigned vi_reg)
			{
				return EmitLoadViWordRaw(0, vi_reg) &&
					EmitVuDataMemoryAddressFromQwordIndex(0);
			}

			bool EmitInlineBackupVI(unsigned reg)
			{
				// PCSX2 owner: VUops.cpp::_vuBackupVI(). Keep the exact
				// repeated-write rule in generated A32 so lower IALU ops can
				// avoid a C++ call without changing branch-operand visibility.
				if (!m_code.EmitLdrbImm12(0, HOST_VU, VuOffset(VI_BACKUP_CYCLES_OFFSET)) ||
					!m_code.EmitCmpImm32(0, 0))
				{
					return false;
				}
				const size_t full_backup_from_empty = m_code.EmitBranchPlaceholder(Condition::EQ);
				if (full_backup_from_empty == static_cast<size_t>(-1))
					return false;

				if (!m_code.EmitLdrImm12(1, HOST_VU, VuOffset(VI_REG_NUMBER_OFFSET)) ||
					!m_code.EmitCmpImm32(1, reg))
				{
					return false;
				}
				const size_t full_backup_from_other_reg = m_code.EmitBranchPlaceholder(Condition::NE);
				if (full_backup_from_other_reg == static_cast<size_t>(-1))
					return false;

				if (!m_code.EmitMovImm8(0, 2) ||
					!m_code.EmitStrbImm12(0, HOST_VU, VuOffset(VI_BACKUP_CYCLES_OFFSET)))
				{
					return false;
				}
				const size_t done = m_code.EmitBranchPlaceholder();
				if (done == static_cast<size_t>(-1))
					return false;

				const size_t full_backup = m_code.Size();
				if (!m_code.PatchBranch(full_backup_from_empty, full_backup, Condition::EQ) ||
					!m_code.PatchBranch(full_backup_from_other_reg, full_backup, Condition::NE))
				{
					return false;
				}

				if (!m_code.EmitMovImm8(0, 2) ||
					!m_code.EmitStrbImm12(0, HOST_VU, VuOffset(VI_BACKUP_CYCLES_OFFSET)) ||
					!m_code.EmitMovImm32(1, reg) ||
					!m_code.EmitStrImm12(1, HOST_VU, VuOffset(VI_REG_NUMBER_OFFSET)) ||
					!EmitLoadViHalfwordRaw(2, reg) ||
					!m_code.EmitStrImm12(2, HOST_VU, VuOffset(VI_OLD_VALUE_OFFSET)))
				{
					return false;
				}

				return m_code.PatchBranch(done, m_code.Size());
			}

			bool EmitInlineLowerIalu(u32 code, VUInterpFast::LowerFastKind kind)
			{
				unsigned dest = 0;
				bool emitted_body = true;

				// PCSX2 owner: VUmicroFast.h::ExecuteLowerNoUpperKnownKind()
				// for the fixed-latency VI integer ops. These are pure
				// halfword operations plus _vuBackupVI(), so emitting them
				// directly removes the per-op kind thunk from VU1 hot blocks.
				switch (kind)
				{
					case VUInterpFast::LowerFastKind::IADDIU:
						dest = VUInterpFast::It(code);
						if (dest == 0)
							return true;
						emitted_body = EmitInlineBackupVI(dest);
						if (VUInterpFast::Imm15(code) != 0 || dest != VUInterpFast::Is(code))
						{
							emitted_body = emitted_body &&
								EmitLoadViLowResultOperand(0, VUInterpFast::Is(code)) &&
								EmitAddSignedImmToR0(VUInterpFast::Imm15(code)) &&
								EmitStoreViHalfword(0, dest);
						}
						break;

					case VUInterpFast::LowerFastKind::ISUBIU:
						dest = VUInterpFast::It(code);
						if (dest == 0)
							return true;
						emitted_body = EmitInlineBackupVI(dest);
						if (VUInterpFast::Imm15(code) != 0 || dest != VUInterpFast::Is(code))
						{
							emitted_body = emitted_body &&
								EmitLoadViLowResultOperand(0, VUInterpFast::Is(code)) &&
								EmitAddSignedImmToR0(-VUInterpFast::Imm15(code)) &&
								EmitStoreViHalfword(0, dest);
						}
						break;

					case VUInterpFast::LowerFastKind::IADD:
					{
						dest = VUInterpFast::Id(code);
						if (dest == 0)
							return true;
						const unsigned is = VUInterpFast::Is(code);
						const unsigned it = VUInterpFast::It(code);
						emitted_body = EmitInlineBackupVI(dest);
						if (is == 0 || it == 0)
						{
							const unsigned source = is == 0 ? it : is;
							if (dest != source)
							{
								emitted_body = emitted_body &&
									EmitLoadViLowResultOperand(0, source) &&
									EmitStoreViHalfword(0, dest);
							}
						}
						else
						{
							emitted_body = emitted_body &&
								EmitLoadViLowResultOperand(0, is) &&
								EmitLoadViLowResultOperand(1, it) &&
								m_code.EmitAddReg(0, 0, 1) &&
								EmitStoreViHalfword(0, dest);
						}
						break;
					}

					case VUInterpFast::LowerFastKind::ISUB:
					{
						dest = VUInterpFast::Id(code);
						if (dest == 0)
							return true;
						const unsigned is = VUInterpFast::Is(code);
						const unsigned it = VUInterpFast::It(code);
						emitted_body = EmitInlineBackupVI(dest);
						if (is == it)
						{
							emitted_body = emitted_body &&
								m_code.EmitMovImm8(0, 0) &&
								EmitStoreViHalfword(0, dest);
						}
						else if (it == 0)
						{
							if (dest != is)
							{
								emitted_body = emitted_body &&
									EmitLoadViLowResultOperand(0, is) &&
									EmitStoreViHalfword(0, dest);
							}
						}
						else
						{
							emitted_body = emitted_body &&
								EmitLoadViLowResultOperand(0, is) &&
								EmitLoadViLowResultOperand(1, it) &&
								m_code.EmitSubReg(0, 0, 1) &&
								EmitStoreViHalfword(0, dest);
						}
						break;
					}

					case VUInterpFast::LowerFastKind::IADDI:
						dest = VUInterpFast::It(code);
						if (dest == 0)
							return true;
						emitted_body = EmitInlineBackupVI(dest);
						if (VUInterpFast::Imm5(code) != 0 || dest != VUInterpFast::Is(code))
						{
							emitted_body = emitted_body &&
								EmitLoadViLowResultOperand(0, VUInterpFast::Is(code)) &&
								EmitAddSignedImmToR0(VUInterpFast::Imm5(code)) &&
								EmitStoreViHalfword(0, dest);
						}
						break;

					case VUInterpFast::LowerFastKind::IAND:
					{
						dest = VUInterpFast::Id(code);
						if (dest == 0)
							return true;
						const unsigned is = VUInterpFast::Is(code);
						const unsigned it = VUInterpFast::It(code);
						emitted_body = EmitInlineBackupVI(dest);
						if (is == it)
						{
							if (dest != is)
							{
								emitted_body = emitted_body &&
									EmitLoadViLowResultOperand(0, is) &&
									EmitStoreViHalfword(0, dest);
							}
						}
						else if (is == 0 || it == 0)
						{
							emitted_body = emitted_body &&
								m_code.EmitMovImm8(0, 0) &&
								EmitStoreViHalfword(0, dest);
						}
						else
						{
							emitted_body = emitted_body &&
								EmitLoadViLowResultOperand(0, is) &&
								EmitLoadViLowResultOperand(1, it) &&
								m_code.EmitAndReg(0, 0, 1) &&
								EmitStoreViHalfword(0, dest);
						}
						break;
					}

					case VUInterpFast::LowerFastKind::IOR:
					{
						dest = VUInterpFast::Id(code);
						if (dest == 0)
							return true;
						const unsigned is = VUInterpFast::Is(code);
						const unsigned it = VUInterpFast::It(code);
						emitted_body = EmitInlineBackupVI(dest);
						if (is == it || is == 0 || it == 0)
						{
							const unsigned source = is == 0 ? it : is;
							if (dest != source)
							{
								emitted_body = emitted_body &&
									EmitLoadViLowResultOperand(0, source) &&
									EmitStoreViHalfword(0, dest);
							}
						}
						else
						{
							emitted_body = emitted_body &&
								EmitLoadViLowResultOperand(0, is) &&
								EmitLoadViLowResultOperand(1, it) &&
								m_code.EmitOrrReg(0, 0, 1) &&
								EmitStoreViHalfword(0, dest);
						}
						break;
					}

					default:
						return false;
				}

				if (!emitted_body)
					return false;

#if defined(VITASX2_QEMU_VALIDATION)
				return EmitQemuLowerIaluInlineCounter();
#else
				return true;
#endif
			}

			bool EmitInlineLowerFlag(u32 code, VUInterpFast::LowerFastKind kind)
			{
				const unsigned it = VUInterpFast::It(code);
				bool emitted_body = true;

				// PCSX2 owner: VUmicroFast.h::ExecuteLowerNoUpperKnownKind()
				// flag/control lower ops. These are scalar flag/VI
				// operations; FMAC pipe clear/add-stall bookkeeping remains
				// in EmitPair() exactly as in VUops.cpp.
				switch (kind)
				{
					case VUInterpFast::LowerFastKind::FCAND:
						emitted_body =
							m_code.EmitLdrImm12(0, HOST_VU, ViOffset(REG_CLIP_FLAG)) &&
							m_code.EmitMovImm32(1, code & 0x00ffffffu) &&
							m_code.EmitAndReg(0, 0, 1, true) &&
							EmitStoreViBoolFromFlags(1, Condition::NE);
						break;

					case VUInterpFast::LowerFastKind::FCSET:
						emitted_body =
							m_code.EmitMovImm32(0, code & 0x00ffffffu) &&
							m_code.EmitStrImm12(0, HOST_VU, VuOffset(offsetof(VURegs, clipflag)));
						break;

					case VUInterpFast::LowerFastKind::FCEQ:
						emitted_body =
							m_code.EmitLdrImm12(0, HOST_VU, ViOffset(REG_CLIP_FLAG)) &&
							EmitAndRegImm32(0, 0, 0x00ffffffu, 1) &&
							EmitCmpRegImm32(0, code & 0x00ffffffu, 1) &&
							EmitStoreViBoolFromFlags(1, Condition::EQ);
						break;

					case VUInterpFast::LowerFastKind::FCOR:
						emitted_body =
							m_code.EmitLdrImm12(0, HOST_VU, ViOffset(REG_CLIP_FLAG)) &&
							EmitAndRegImm32(0, 0, 0x00ffffffu, 1) &&
							EmitOrrRegImm32(0, 0, code & 0x00ffffffu, 1) &&
							EmitCmpRegImm32(0, 0x00ffffffu, 1) &&
							EmitStoreViBoolFromFlags(1, Condition::EQ);
						break;

					case VUInterpFast::LowerFastKind::FSEQ:
						if (it == 0)
							return true;
						emitted_body =
							EmitLoadViHalfword(0, REG_STATUS_FLAG) &&
							EmitAndRegImm32(0, 0, 0x0fffu, 1) &&
							EmitCmpRegImm32(0, VUInterpFast::FlagImm12(code), 1) &&
							EmitStoreViBoolFromFlags(it, Condition::EQ);
						break;

					case VUInterpFast::LowerFastKind::FSSET:
						emitted_body =
							m_code.EmitLdrImm12(0, HOST_VU, VuOffset(offsetof(VURegs, statusflag))) &&
							EmitAndRegImm32(0, 0, 0x003fu, 1) &&
							EmitOrrRegImm32(0, 0, VUInterpFast::FlagImm12(code) & 0x0fc0u, 1) &&
							m_code.EmitStrImm12(0, HOST_VU, VuOffset(offsetof(VURegs, statusflag)));
						break;

					case VUInterpFast::LowerFastKind::FSAND:
						if (it == 0)
							return true;
						emitted_body =
							EmitLoadViHalfword(0, REG_STATUS_FLAG) &&
							EmitAndRegImm32(0, 0, 0x0fffu, 1) &&
							EmitAndRegImm32(0, 0, VUInterpFast::FlagImm12(code), 1) &&
							EmitStoreViHalfword(0, it);
						break;

					case VUInterpFast::LowerFastKind::FSOR:
						if (it == 0)
							return true;
						emitted_body =
							EmitLoadViHalfword(0, REG_STATUS_FLAG) &&
							EmitAndRegImm32(0, 0, 0x0fffu, 1) &&
							EmitOrrRegImm32(0, 0, VUInterpFast::FlagImm12(code), 1) &&
							EmitStoreViHalfword(0, it);
						break;

					case VUInterpFast::LowerFastKind::FMEQ:
						if (it == 0)
							return true;
						emitted_body =
							EmitLoadViHalfword(0, REG_MAC_FLAG) &&
							EmitLoadViHalfword(1, VUInterpFast::Is(code)) &&
							m_code.EmitCmpReg(0, 1) &&
							EmitStoreViBoolFromFlags(it, Condition::EQ);
						break;

					case VUInterpFast::LowerFastKind::FMAND:
						if (it == 0)
							return true;
						emitted_body =
							EmitLoadViHalfword(0, VUInterpFast::Is(code)) &&
							EmitLoadViHalfword(1, REG_MAC_FLAG) &&
							m_code.EmitAndReg(0, 0, 1) &&
							EmitStoreViHalfword(0, it);
						break;

					case VUInterpFast::LowerFastKind::FMOR:
						if (it == 0)
							return true;
						emitted_body =
							EmitLoadViHalfword(0, REG_MAC_FLAG) &&
							EmitLoadViHalfword(1, VUInterpFast::Is(code)) &&
							m_code.EmitOrrReg(0, 0, 1) &&
							EmitStoreViHalfword(0, it);
						break;

					case VUInterpFast::LowerFastKind::FCGET:
						if (it == 0)
							return true;
						emitted_body =
							EmitLoadViHalfword(0, REG_CLIP_FLAG) &&
							EmitAndRegImm32(0, 0, 0x0fffu, 1) &&
							EmitStoreViHalfword(0, it);
						break;

					default:
						return false;
				}

				if (!emitted_body)
					return false;

#if defined(VITASX2_QEMU_VALIDATION)
				return EmitQemuLowerFlagInlineCounter();
#else
				return true;
#endif
			}

			bool EmitInlineLowerMove(u32 code, VUInterpFast::LowerFastKind kind)
			{
				const unsigned mask = VUInterpFast::XYZW(code);
				bool emitted_body = true;

				// PCSX2 owner: VUmicroFast.h::ExecuteLowerNoUpperKnownKind()
				// lower vector moves. Full masks use NEON qword moves/rotates;
				// partial masks use ordered scalar lane stores, which preserves
				// MR32's same-register source behavior from VUops.cpp.
				switch (kind)
				{
					case VUInterpFast::LowerFastKind::MOVE:
					{
						const unsigned ft = VUInterpFast::Ft(code);
						if (ft == 0 || mask == 0)
							return true;

						const unsigned fs = VUInterpFast::Fs(code);
						if (mask == 0x0f)
						{
							emitted_body =
								EmitLoadVfQuad(0, fs) &&
								EmitStoreVfQuad(0, ft);
							break;
						}

						emitted_body = true;
						if ((mask & 0x8) != 0)
							emitted_body = emitted_body && EmitLoadVfWord(0, fs, 0) && EmitStoreVfWord(0, ft, 0);
						if ((mask & 0x4) != 0)
							emitted_body = emitted_body && EmitLoadVfWord(0, fs, 1) && EmitStoreVfWord(0, ft, 1);
						if ((mask & 0x2) != 0)
							emitted_body = emitted_body && EmitLoadVfWord(0, fs, 2) && EmitStoreVfWord(0, ft, 2);
						if ((mask & 0x1) != 0)
							emitted_body = emitted_body && EmitLoadVfWord(0, fs, 3) && EmitStoreVfWord(0, ft, 3);
						break;
					}

					case VUInterpFast::LowerFastKind::MR32:
					{
						const unsigned ft = VUInterpFast::Ft(code);
						if (ft == 0 || mask == 0)
							return true;

						const unsigned fs = VUInterpFast::Fs(code);
						if (mask == 0x0f)
						{
							emitted_body =
								EmitLoadVfQuad(0, fs) &&
								m_code.EmitVextI8Q(0, 0, 0, 4) &&
								EmitStoreVfQuad(0, ft);
							break;
						}

						emitted_body =
							EmitLoadVfWord(0, fs, 0) &&
							EmitLoadVfWord(1, fs, 1) &&
							EmitLoadVfWord(2, fs, 2) &&
							EmitLoadVfWord(3, fs, 3);
						if ((mask & 0x8) != 0)
							emitted_body = emitted_body && EmitStoreVfWord(1, ft, 0);
						if ((mask & 0x4) != 0)
							emitted_body = emitted_body && EmitStoreVfWord(2, ft, 1);
						if ((mask & 0x2) != 0)
							emitted_body = emitted_body && EmitStoreVfWord(3, ft, 2);
						if ((mask & 0x1) != 0)
							emitted_body = emitted_body && EmitStoreVfWord(0, ft, 3);
						break;
					}

					case VUInterpFast::LowerFastKind::MFIR:
					{
						const unsigned ft = VUInterpFast::Ft(code);
						if (ft == 0 || mask == 0)
							return true;

						emitted_body = EmitLoadViSignedHalfword(0, VUInterpFast::Is(code));
						if (mask == 0x0f)
						{
							emitted_body = emitted_body &&
								m_code.EmitVdupI32QFromCore(0, 0) &&
								EmitStoreVfQuad(0, ft);
							break;
						}

						if ((mask & 0x8) != 0)
							emitted_body = emitted_body && EmitStoreVfWord(0, ft, 0);
						if ((mask & 0x4) != 0)
							emitted_body = emitted_body && EmitStoreVfWord(0, ft, 1);
						if ((mask & 0x2) != 0)
							emitted_body = emitted_body && EmitStoreVfWord(0, ft, 2);
						if ((mask & 0x1) != 0)
							emitted_body = emitted_body && EmitStoreVfWord(0, ft, 3);
						break;
					}

					case VUInterpFast::LowerFastKind::MTIR:
					{
						const unsigned it = VUInterpFast::It(code);
						if (it == 0)
							return true;

						emitted_body =
							EmitInlineBackupVI(it) &&
							EmitLoadVfWord(0, VUInterpFast::Fs(code), VUInterpFast::Fsf(code)) &&
							EmitStoreViHalfword(0, it);
						break;
					}

					default:
						return false;
				}

				if (!emitted_body)
					return false;

#if defined(VITASX2_QEMU_VALIDATION)
				return EmitQemuLowerMoveInlineCounter();
#else
				return true;
#endif
			}

			bool EmitLoadVfMaskedFromAddress(unsigned ft, unsigned mask)
			{
				if (ft == 0 || mask == 0)
					return true;

				if (mask == 0x0f)
				{
					return m_code.EmitVld1Q32Aligned(0, 0) &&
						EmitStoreVfQuad(0, ft);
				}

				bool emitted = true;
				if ((mask & 0x8) != 0)
					emitted = emitted && m_code.EmitLdrImm12(1, 0, 0) && EmitStoreVfWord(1, ft, 0);
				if ((mask & 0x4) != 0)
					emitted = emitted && m_code.EmitLdrImm12(1, 0, 4) && EmitStoreVfWord(1, ft, 1);
				if ((mask & 0x2) != 0)
					emitted = emitted && m_code.EmitLdrImm12(1, 0, 8) && EmitStoreVfWord(1, ft, 2);
				if ((mask & 0x1) != 0)
					emitted = emitted && m_code.EmitLdrImm12(1, 0, 12) && EmitStoreVfWord(1, ft, 3);
				return emitted;
			}

			bool EmitStoreVfMaskedToAddress(unsigned fs, unsigned mask)
			{
				if (mask == 0)
					return true;

				if (mask == 0x0f)
				{
					return EmitLoadVfQuad(0, fs) &&
						m_code.EmitVst1Q32Aligned(0, 0);
				}

				bool emitted = true;
				if ((mask & 0x8) != 0)
					emitted = emitted && EmitLoadVfWord(1, fs, 0) && m_code.EmitStrImm12(1, 0, 0);
				if ((mask & 0x4) != 0)
					emitted = emitted && EmitLoadVfWord(1, fs, 1) && m_code.EmitStrImm12(1, 0, 4);
				if ((mask & 0x2) != 0)
					emitted = emitted && EmitLoadVfWord(1, fs, 2) && m_code.EmitStrImm12(1, 0, 8);
				if ((mask & 0x1) != 0)
					emitted = emitted && EmitLoadVfWord(1, fs, 3) && m_code.EmitStrImm12(1, 0, 12);
				return emitted;
			}

			bool EmitLoadViFromMemoryMaskedAtAddress(unsigned it, unsigned mask)
			{
				if (it == 0 || mask == 0)
					return true;

				return m_code.EmitLdrhImm8(1, 0, LastIlwHalfwordIndex(mask) * sizeof(u16)) &&
					EmitStoreViHalfword(1, it);
			}

			bool EmitStoreViToMemoryMaskedAtAddress(unsigned it, unsigned mask)
			{
				if (mask == 0)
					return true;

				if (!EmitLoadViHalfword(1, it))
					return false;

				if (mask == 0x0f)
				{
					return m_code.EmitVdupI32QFromCore(0, 1) &&
						m_code.EmitVst1Q32Aligned(0, 0);
				}

				bool emitted = true;
				if ((mask & 0x8) != 0)
					emitted = emitted && m_code.EmitStrImm12(1, 0, 0);
				if ((mask & 0x4) != 0)
					emitted = emitted && m_code.EmitStrImm12(1, 0, 4);
				if ((mask & 0x2) != 0)
					emitted = emitted && m_code.EmitStrImm12(1, 0, 8);
				if ((mask & 0x1) != 0)
					emitted = emitted && m_code.EmitStrImm12(1, 0, 12);
				return emitted;
			}

			bool EmitInlineLowerLsu(u32 code, VUInterpFast::LowerFastKind kind)
			{
				const unsigned mask = VUInterpFast::XYZW(code);
				const s32 imm = VUInterpFast::Imm11(code);
				bool emitted_body = true;

				// PCSX2 owner: VUmicroFast.h::ExecuteLowerNoUpperKnownKind()
				// VU memory ops. The generated code keeps _vuBackupVI(),
				// predecrement/postincrement ordering, mask-zero no-op writes,
				// and VUops.cpp::GET_VU_MEM()'s VU0/VU1 wrap behavior exact.
				switch (kind)
				{
					case VUInterpFast::LowerFastKind::LQ:
					{
						const unsigned ft = VUInterpFast::Ft(code);
						if (ft == 0 || mask == 0)
							return true;

						emitted_body =
							EmitVuDataMemoryAddressFromBaseImm(VUInterpFast::Is(code), imm) &&
							EmitLoadVfMaskedFromAddress(ft, mask);
						break;
					}

					case VUInterpFast::LowerFastKind::SQ:
					{
						if (mask == 0)
							return true;

						emitted_body =
							EmitVuDataMemoryAddressFromBaseImm(VUInterpFast::It(code), imm) &&
							EmitStoreVfMaskedToAddress(VUInterpFast::Fs(code), mask);
						break;
					}

					case VUInterpFast::LowerFastKind::ILW:
					{
						if (VUInterpFast::It(code) == 0 || mask == 0)
							return true;

						emitted_body =
							EmitVuDataMemoryAddressFromBaseImm(VUInterpFast::Is(code), imm) &&
							EmitLoadViFromMemoryMaskedAtAddress(VUInterpFast::It(code), mask);
						break;
					}

					case VUInterpFast::LowerFastKind::ISW:
					{
						if (mask == 0)
							return true;

						emitted_body =
							EmitVuDataMemoryAddressFromBaseImm(VUInterpFast::Is(code), imm) &&
							EmitStoreViToMemoryMaskedAtAddress(VUInterpFast::It(code), mask);
						break;
					}

					case VUInterpFast::LowerFastKind::LQI:
					{
						const unsigned is = VUInterpFast::Is(code);
						const bool load_memory = VUInterpFast::Ft(code) != 0 && mask != 0;
						const bool postincrement = VUInterpFast::Fs(code) != 0;
						emitted_body = EmitInlineBackupVI(is);
						if (load_memory && postincrement)
						{
							// PCSX2's microVU_Lower.inl::mVU_LQI keeps the VI value
							// in a host register for both the load address and VI++.
							// r2 survives the r0/r1 memory path; r3 supplies the
							// address-mask literal and the eventual STRH address.
							emitted_body = emitted_body &&
								EmitLoadViWordRaw(2, is) &&
								EmitVuDataMemoryAddressFromQwordIndex(2, 3) &&
								EmitLoadVfMaskedFromAddress(VUInterpFast::Ft(code), mask) &&
								m_code.EmitAddImm8(2, 2, 1) &&
								EmitStoreViHalfword(2, is);
						}
						else
						{
							if (load_memory)
							{
								emitted_body = emitted_body &&
									EmitVuDataMemoryAddressFromVi(is) &&
									EmitLoadVfMaskedFromAddress(VUInterpFast::Ft(code), mask);
							}
							if (postincrement)
							{
								emitted_body = emitted_body &&
									EmitLoadViWordRaw(0, is) &&
									m_code.EmitAddImm8(0, 0, 1) &&
									EmitStoreViHalfword(0, is);
							}
						}
						break;
					}

					case VUInterpFast::LowerFastKind::LQD:
					{
						const unsigned is = VUInterpFast::Is(code);
						const bool load_memory = VUInterpFast::Ft(code) != 0 && mask != 0;
						emitted_body = EmitInlineBackupVI(is);
						if (is != 0 && load_memory)
						{
							emitted_body = emitted_body &&
								EmitLoadViWordRaw(2, is) &&
								m_code.EmitSubImm8(2, 2, 1) &&
								EmitStoreViHalfword(2, is) &&
								EmitVuDataMemoryAddressFromQwordIndex(2, 3) &&
								EmitLoadVfMaskedFromAddress(VUInterpFast::Ft(code), mask);
						}
						else
						{
							if (is != 0)
							{
								emitted_body = emitted_body &&
									EmitLoadViWordRaw(0, is) &&
									m_code.EmitSubImm8(0, 0, 1) &&
									EmitStoreViHalfword(0, is);
							}
							if (load_memory)
							{
								emitted_body = emitted_body &&
									EmitVuDataMemoryAddressFromVi(is) &&
									EmitLoadVfMaskedFromAddress(VUInterpFast::Ft(code), mask);
							}
						}
						break;
					}

					case VUInterpFast::LowerFastKind::SQI:
					{
						const unsigned it = VUInterpFast::It(code);
						const bool store_memory = mask != 0;
						const bool postincrement = VUInterpFast::Ft(code) != 0;
						emitted_body = EmitInlineBackupVI(it);
						if (store_memory && postincrement)
						{
							emitted_body = emitted_body &&
								EmitLoadViWordRaw(2, it) &&
								EmitVuDataMemoryAddressFromQwordIndex(2, 3) &&
								EmitStoreVfMaskedToAddress(VUInterpFast::Fs(code), mask) &&
								m_code.EmitAddImm8(2, 2, 1) &&
								EmitStoreViHalfword(2, it);
						}
						else
						{
							if (store_memory)
							{
								emitted_body = emitted_body &&
									EmitVuDataMemoryAddressFromVi(it) &&
									EmitStoreVfMaskedToAddress(VUInterpFast::Fs(code), mask);
							}
							if (postincrement)
							{
								emitted_body = emitted_body &&
									EmitLoadViWordRaw(0, it) &&
									m_code.EmitAddImm8(0, 0, 1) &&
									EmitStoreViHalfword(0, it);
							}
						}
						break;
					}

					case VUInterpFast::LowerFastKind::SQD:
					{
						const unsigned it = VUInterpFast::It(code);
						const bool store_memory = mask != 0;
						emitted_body = EmitInlineBackupVI(it);
						if (VUInterpFast::Ft(code) != 0 && store_memory)
						{
							emitted_body = emitted_body &&
								EmitLoadViWordRaw(2, it) &&
								m_code.EmitSubImm8(2, 2, 1) &&
								EmitStoreViHalfword(2, it) &&
								EmitVuDataMemoryAddressFromQwordIndex(2, 3) &&
								EmitStoreVfMaskedToAddress(VUInterpFast::Fs(code), mask);
						}
						else
						{
							if (VUInterpFast::Ft(code) != 0)
							{
								emitted_body = emitted_body &&
									EmitLoadViWordRaw(0, it) &&
									m_code.EmitSubImm8(0, 0, 1) &&
									EmitStoreViHalfword(0, it);
							}
							if (store_memory)
							{
								emitted_body = emitted_body &&
									EmitVuDataMemoryAddressFromVi(it) &&
									EmitStoreVfMaskedToAddress(VUInterpFast::Fs(code), mask);
							}
						}
						break;
					}

					case VUInterpFast::LowerFastKind::ILWR:
					{
						if (VUInterpFast::It(code) == 0 || mask == 0)
							return true;

						emitted_body =
							EmitVuDataMemoryAddressFromVi(VUInterpFast::Is(code)) &&
							EmitLoadViFromMemoryMaskedAtAddress(VUInterpFast::It(code), mask);
						break;
					}

					case VUInterpFast::LowerFastKind::ISWR:
					{
						if (mask == 0)
							return true;

						emitted_body =
							EmitVuDataMemoryAddressFromVi(VUInterpFast::Is(code)) &&
							EmitStoreViToMemoryMaskedAtAddress(VUInterpFast::It(code), mask);
						break;
					}

					default:
						return false;
				}

				if (!emitted_body)
					return false;

#if defined(VITASX2_QEMU_VALIDATION)
				return EmitQemuLowerLsuInlineCounter();
#else
				return true;
#endif
			}

			bool EmitAdvanceR()
			{
				// PCSX2 owner: VUmicroFast.h::AdvanceR() / VUops.cpp::AdvanceLFSR().
				return EmitLoadViWordRaw(0, REG_R) &&
					m_code.EmitMovRegShiftImm(1, 0, ShiftType::LSR, 4) &&
					m_code.EmitAndImm32(1, 1, 1) &&
					m_code.EmitMovRegShiftImm(2, 0, ShiftType::LSR, 22) &&
					m_code.EmitAndImm32(2, 2, 1) &&
					m_code.EmitEorReg(1, 1, 2) &&
					m_code.EmitMovRegShiftImm(0, 0, ShiftType::LSL, 1) &&
					m_code.EmitEorReg(0, 0, 1) &&
					EmitAndRegImm32(0, 0, 0x007fffffu, 1) &&
					EmitOrrRegImm32(0, 0, 0x3f800000u, 1) &&
					EmitStoreViWordRaw(0, REG_R);
			}

			bool EmitInlineLowerControl(u32 code, VUInterpFast::LowerFastKind kind)
			{
				const unsigned mask = VUInterpFast::XYZW(code);
				bool emitted_body = true;

				// PCSX2 owner: VUmicroFast.h::ExecuteLowerNoUpperKnownKind().
				// Stall waits for WAITQ/WAITP are already emitted before the
				// lower body, matching VUops.cpp's no-op instruction bodies.
				switch (kind)
				{
					case VUInterpFast::LowerFastKind::RINIT:
						emitted_body =
							EmitLoadVfWord(0, VUInterpFast::Fs(code), VUInterpFast::Fsf(code)) &&
							EmitAndRegImm32(0, 0, 0x007fffffu, 1) &&
							EmitOrrRegImm32(0, 0, 0x3f800000u, 1) &&
							EmitStoreViWordRaw(0, REG_R);
						break;

					case VUInterpFast::LowerFastKind::RGET:
						if (VUInterpFast::Ft(code) == 0 || mask == 0)
							return true;
						emitted_body =
							EmitLoadViWordRaw(0, REG_R) &&
							EmitStoreWordToVfMasked(0, VUInterpFast::Ft(code), mask);
						break;

					case VUInterpFast::LowerFastKind::RNEXT:
						if (VUInterpFast::Ft(code) == 0)
							return true;
						emitted_body =
							EmitAdvanceR() &&
							((mask == 0) ? true :
								(EmitLoadViWordRaw(0, REG_R) &&
									EmitStoreWordToVfMasked(0, VUInterpFast::Ft(code), mask)));
						break;

					case VUInterpFast::LowerFastKind::RXOR:
						emitted_body =
							EmitLoadViWordRaw(0, REG_R) &&
							EmitLoadVfWord(1, VUInterpFast::Fs(code), VUInterpFast::Fsf(code)) &&
							m_code.EmitEorReg(0, 0, 1) &&
							EmitAndRegImm32(0, 0, 0x007fffffu, 1) &&
							EmitOrrRegImm32(0, 0, 0x3f800000u, 1) &&
							EmitStoreViWordRaw(0, REG_R);
						break;

					case VUInterpFast::LowerFastKind::MFP:
						if (VUInterpFast::Ft(code) == 0 || mask == 0)
							return true;
						emitted_body =
							EmitLoadViWordRaw(0, REG_P) &&
							EmitStoreWordToVfMasked(0, VUInterpFast::Ft(code), mask);
						break;

					case VUInterpFast::LowerFastKind::WAITQ:
					case VUInterpFast::LowerFastKind::WAITP:
						emitted_body = true;
						break;

					case VUInterpFast::LowerFastKind::XITOP:
						if (VUInterpFast::It(code) == 0)
							return true;
						// PCSX2 owner: VUops.cpp::_vuXITOP(). On ARM32
						// THREAD_VU1 is compiled false, so this reads vif0Regs
						// for VU0 and vif1Regs for VU1 through GetVifRegs().
						emitted_body =
							EmitLoadVifWord(0, offsetof(VIFregisters, itop)) &&
							EmitStoreViHalfword(0, VUInterpFast::It(code));
						break;

					case VUInterpFast::LowerFastKind::XTOP:
						if (VUInterpFast::It(code) == 0)
							return true;
						if (m_vu0_memory_map)
							return true;
						// PCSX2 owners: VUops.cpp::VU0MI_XTOP() is a no-op,
						// while VU1MI_XTOP() calls _vuXTOP(). On ARM32
						// THREAD_VU1=false, so VU1 reads vif1Regs.top.
						emitted_body =
							EmitLoadVifWord(0, offsetof(VIFregisters, top)) &&
							EmitStoreViHalfword(0, VUInterpFast::It(code));
						break;

					default:
						return false;
				}

				if (!emitted_body)
					return false;

#if defined(VITASX2_QEMU_VALIDATION)
				return EmitQemuLowerControlInlineCounter();
#else
				return true;
#endif
			}

			bool EmitLoadViBranchOperand(unsigned rd, unsigned reg)
			{
				pxAssert(rd == 0 || rd == 1);

				// PCSX2 owner: VUmicroFast.h::ReadViBranchOperand() and
				// VUops.cpp::_vuIBxx(). Conditional branches see the backed-up
				// VI value while the IALU writeback visibility window is open.
				if (!EmitLoadViHalfwordRaw(rd, reg) ||
					!m_code.EmitLdrbImm12(2, HOST_VU, VuOffset(VI_BACKUP_CYCLES_OFFSET)) ||
					!m_code.EmitCmpImm32(2, 0))
				{
					return false;
				}
				const size_t no_backup = m_code.EmitBranchPlaceholder(Condition::EQ);
				if (no_backup == static_cast<size_t>(-1))
					return false;

				if (!m_code.EmitLdrImm12(2, HOST_VU, VuOffset(VI_REG_NUMBER_OFFSET)) ||
					!m_code.EmitCmpImm32(2, reg))
				{
					return false;
				}
				const size_t other_reg = m_code.EmitBranchPlaceholder(Condition::NE);
				if (other_reg == static_cast<size_t>(-1))
					return false;

				if (!m_code.EmitLdrImm12(rd, HOST_VU, VuOffset(VI_OLD_VALUE_OFFSET)))
					return false;

				const size_t sign_extend = m_code.Size();
				return m_code.PatchBranch(no_backup, sign_extend, Condition::EQ) &&
					m_code.PatchBranch(other_reg, sign_extend, Condition::NE) &&
					m_code.EmitSxth(rd, rd);
			}

			bool EmitBranchAddressToR0(u32 code, u32 postincrement_tpc)
			{
				// PCSX2 owner: VUops.cpp::_branchAddr() /
				// VUmicroFast.h::BranchAddress(). Sony's VU contract defines the
				// target relative to the branch delay-slot address, which is this
				// compile-time post-increment byte TPC.
				const s32 byte_offset = static_cast<s32>(VUInterpFast::Imm11(code)) * 8;
				const u32 prog_mask = m_vu0_memory_map ? VU0_PROGMASK : VU1_PROGMASK;
				return m_code.EmitMovImm32(0,
					(static_cast<u32>(static_cast<s32>(postincrement_tpc) + byte_offset) & prog_mask));
			}

			bool EmitSetBranchFromReg(unsigned bpc_reg)
			{
				// PCSX2 owner: VUops.cpp::_setBranch() /
				// VUmicroFast.h::SetBranch(), including branch-in-delay-slot
				// handoff through delaybranchpc/takedelaybranch.
				const u16 branch = VuOffset(offsetof(VURegs, branch));
				if (!m_code.EmitLdrImm12(1, HOST_VU, branch) ||
					!m_code.EmitCmpImm32(1, 1))
				{
					return false;
				}
				const size_t normal_branch = m_code.EmitBranchPlaceholder(Condition::NE);
				if (normal_branch == static_cast<size_t>(-1))
					return false;

				if (!m_code.EmitStrImm12(bpc_reg, HOST_VU, VuOffset(offsetof(VURegs, delaybranchpc))) ||
					!m_code.EmitMovImm8(1, 1) ||
					!m_code.EmitStrbImm12(1, HOST_VU, VuOffset(offsetof(VURegs, takedelaybranch))))
				{
					return false;
				}
				const size_t done = m_code.EmitBranchPlaceholder();
				if (done == static_cast<size_t>(-1))
					return false;

				const size_t normal_target = m_code.Size();
				if (!m_code.PatchBranch(normal_branch, normal_target, Condition::NE) ||
					!m_code.EmitMovImm8(1, 2) ||
					!m_code.EmitStrImm12(1, HOST_VU, branch) ||
					!m_code.EmitStrImm12(bpc_reg, HOST_VU, VuOffset(offsetof(VURegs, branchpc))))
				{
					return false;
				}

				return m_code.PatchBranch(done, m_code.Size());
			}

			bool EmitWriteBranchLink(unsigned reg, u32 postincrement_tpc)
			{
				if (reg == 0)
					return true;

				// PCSX2 owner: VUops.cpp::_vuBAL()/_vuJALR() link write.
				// In a delay slot, the link comes from the first branch target;
				// otherwise it comes from the already post-incremented TPC.
				if (!m_code.EmitLdrImm12(1, HOST_VU, VuOffset(offsetof(VURegs, branch))) ||
					!m_code.EmitCmpImm32(1, 1))
				{
					return false;
				}
				const size_t use_static_tpc = m_code.EmitBranchPlaceholder(Condition::NE);
				if (use_static_tpc == static_cast<size_t>(-1))
					return false;

				if (!m_code.EmitLdrImm12(0, HOST_VU, VuOffset(offsetof(VURegs, branchpc))))
					return false;
				const size_t have_base = m_code.EmitBranchPlaceholder();
				if (have_base == static_cast<size_t>(-1))
					return false;

				const size_t use_static_tpc_target = m_code.Size();
				if (!m_code.PatchBranch(use_static_tpc, use_static_tpc_target, Condition::NE) ||
					!m_code.EmitMovImm32(0, postincrement_tpc))
				{
					return false;
				}

				const size_t have_base_target = m_code.Size();
				return m_code.PatchBranch(have_base, have_base_target) &&
					m_code.EmitAddImm8(0, 0, 8) &&
					m_code.EmitMovRegShiftImm(0, 0, ShiftType::LSR, 3) &&
					EmitStoreViHalfword(0, reg);
			}

			bool EmitSetBranchWhenCondition(u32 code, u32 postincrement_tpc,
				Condition skip_condition)
			{
				const size_t skip = m_code.EmitBranchPlaceholder(skip_condition);
				if (skip == static_cast<size_t>(-1))
					return false;

				if (!EmitBranchAddressToR0(code, postincrement_tpc) ||
					!EmitSetBranchFromReg(0))
				{
					return false;
				}

				return m_code.PatchBranch(skip, m_code.Size(), skip_condition);
			}

			bool EmitInlineLowerBranch(u32 code, u32 postincrement_tpc,
				VUInterpFast::LowerFastKind kind)
			{
				bool emitted_body = true;

				// PCSX2 owner: VUops.cpp::_vuIBEQ/_vuIBNE/_vuIBLTZ/
				// _vuIBGTZ/_vuIBLEZ/_vuIBGEZ/_vuB/_vuBAL/_vuJR/_vuJALR,
				// with the fast helpers in VUmicroFast.h supplying the same
				// branch-address and VI-backup operand rules.
				switch (kind)
				{
					case VUInterpFast::LowerFastKind::IBEQ:
						emitted_body =
							EmitLoadViBranchOperand(0, VUInterpFast::It(code)) &&
							EmitLoadViBranchOperand(1, VUInterpFast::Is(code)) &&
							m_code.EmitCmpReg(0, 1) &&
							EmitSetBranchWhenCondition(code, postincrement_tpc, Condition::NE);
						break;

					case VUInterpFast::LowerFastKind::IBNE:
						emitted_body =
							EmitLoadViBranchOperand(0, VUInterpFast::It(code)) &&
							EmitLoadViBranchOperand(1, VUInterpFast::Is(code)) &&
							m_code.EmitCmpReg(0, 1) &&
							EmitSetBranchWhenCondition(code, postincrement_tpc, Condition::EQ);
						break;

					case VUInterpFast::LowerFastKind::IBLTZ:
						emitted_body =
							EmitLoadViBranchOperand(0, VUInterpFast::Is(code)) &&
							m_code.EmitCmpImm32(0, 0) &&
							EmitSetBranchWhenCondition(code, postincrement_tpc, Condition::GE);
						break;

					case VUInterpFast::LowerFastKind::IBGTZ:
						emitted_body =
							EmitLoadViBranchOperand(0, VUInterpFast::Is(code)) &&
							m_code.EmitCmpImm32(0, 0) &&
							EmitSetBranchWhenCondition(code, postincrement_tpc, Condition::LE);
						break;

					case VUInterpFast::LowerFastKind::IBLEZ:
						emitted_body =
							EmitLoadViBranchOperand(0, VUInterpFast::Is(code)) &&
							m_code.EmitCmpImm32(0, 0) &&
							EmitSetBranchWhenCondition(code, postincrement_tpc, Condition::GT);
						break;

					case VUInterpFast::LowerFastKind::IBGEZ:
						emitted_body =
							EmitLoadViBranchOperand(0, VUInterpFast::Is(code)) &&
							m_code.EmitCmpImm32(0, 0) &&
							EmitSetBranchWhenCondition(code, postincrement_tpc, Condition::LT);
						break;

					case VUInterpFast::LowerFastKind::B:
						emitted_body =
							EmitBranchAddressToR0(code, postincrement_tpc) &&
							EmitSetBranchFromReg(0);
						break;

					case VUInterpFast::LowerFastKind::BAL:
						emitted_body =
							EmitWriteBranchLink(VUInterpFast::It(code), postincrement_tpc) &&
							EmitBranchAddressToR0(code, postincrement_tpc) &&
							EmitSetBranchFromReg(0);
						break;

					case VUInterpFast::LowerFastKind::JR:
						emitted_body =
							EmitLoadViHalfwordRaw(0, VUInterpFast::Is(code)) &&
							m_code.EmitMovRegShiftImm(0, 0, ShiftType::LSL, 3) &&
							EmitSetBranchFromReg(0);
						break;

					case VUInterpFast::LowerFastKind::JALR:
						emitted_body =
							EmitLoadViHalfwordRaw(2, VUInterpFast::Is(code)) &&
							m_code.EmitMovRegShiftImm(2, 2, ShiftType::LSL, 3) &&
							EmitWriteBranchLink(VUInterpFast::It(code), postincrement_tpc) &&
							EmitSetBranchFromReg(2);
						break;

					default:
						return false;
				}

				if (!emitted_body)
					return false;

#if defined(VITASX2_QEMU_VALIDATION)
				return EmitQemuLowerBranchInlineCounter();
#else
				return true;
#endif
			}

			// Synthesizes the vuDouble() bit-mask constants entirely in NEON. ARM
			// VMOV.I32 can encode the sign bit and exponent bit 23 directly:
			//   0x80000000 - 0x00800000 = 0x7f800000 (exponent mask)
			// ~(0x80000000 | 0x00800000) = 0x7f7fffff (max finite)
			// This replaces the old ARM MOVW/MOVT + core-to-NEON VDUP crossings.
			// ARM ARM A8.6.281/A8.6.290 supply zero directly to comparisons, so
			// this prologue does not spend an instruction or register on zero.
			// Q14 is normalization scratch and does not need to retain bit 23.
			bool EmitMaterializeVuFloatNormalizeConstants(bool overflow_clamp)
			{
				if (!m_code.EmitVmovI32Q(VU_NORM_SIGN_Q, 0x80u, 24) ||
					!m_code.EmitVmovI32Q(VU_NORM_TMP_Q, 0x80u, 16) ||
					!m_code.EmitVsubI32Q(VU_NORM_EXP_Q, VU_NORM_SIGN_Q, VU_NORM_TMP_Q))
				{
					return false;
				}

				if (overflow_clamp &&
					(!m_code.EmitVorrQ(VU_NORM_MAXF_Q, VU_NORM_SIGN_Q, VU_NORM_TMP_Q) ||
					 !m_code.EmitVmvnQ(VU_NORM_MAXF_Q, VU_NORM_MAXF_Q)))
				{
					return false;
				}

#if defined(VITASX2_QEMU_VALIDATION)
				return EmitQemuNormConstantMaterializationCounter();
#else
				return true;
#endif
			}

			// Materializes the constants once per block. Q8-Q10 are then reused by
			// every later FMAC/EFU normalize in the same straight-line block (see
			// m_norm_consts_ready). Blocks are entered only at their start_pc and
			// each is compiled by a fresh BlockCompiler, so the first normalize in
			// every block always materializes.
			bool EmitEnsureVuFloatNormalizeConstants(bool overflow_clamp)
			{
				if (m_norm_consts_ready && (!overflow_clamp || m_norm_maxf_ready))
					return true;
				if (m_norm_consts_ready)
				{
					if (!m_code.EmitVmovI32Q(VU_NORM_TMP_Q, 0x80u, 16) ||
						!m_code.EmitVorrQ(VU_NORM_MAXF_Q, VU_NORM_SIGN_Q, VU_NORM_TMP_Q) ||
						!m_code.EmitVmvnQ(VU_NORM_MAXF_Q, VU_NORM_MAXF_Q))
					{
						return false;
					}
					m_norm_maxf_ready = true;
					return true;
				}
				if (!EmitMaterializeVuFloatNormalizeConstants(overflow_clamp))
					return false;
				m_norm_consts_ready = true;
				m_norm_maxf_ready = overflow_clamp;
				return true;
			}

			bool VuFpcrFlushesInputsToZero() const
			{
				return (m_vu0_memory_map ? EmuConfig.Cpu.VU0FPCR :
					EmuConfig.Cpu.VU1FPCR).GetFlushToZero();
			}

			bool EmitEnsureVuFloatInputNormalizeConstants(bool overflow_clamp)
			{
				// With FZ installed and PCSX2's optional overflow clamp disabled,
				// scalar VFP itself implements the complete vuDouble() input rule.
				return (VuFpcrFlushesInputsToZero() && !overflow_clamp) ||
					EmitEnsureVuFloatNormalizeConstants(overflow_clamp);
			}

			// NEON quad form of EmitNormalizeVuFloatWord() over all four lanes at
			// once. The software portions require the normalization constants;
			// FZ with overflow clamping disabled returns before consuming them.
			// PCSX2 owner: VUops.cpp::vuDouble() / VUmicroFast.h::VuDouble().
			// Denormals (exponent 0) flush to signed zero; with the VU overflow
			// clamp enabled, infinities/NaNs (exponent 0xff) become signed max
			// finite. ARM ARM A2.7.5 requires scalar VFP under FZ to treat every
			// denormal input as zero with its sign preserved. Every caller feeds
			// these quads to scalar VFP arithmetic, so an FZ-specialized block can
			// leave denormal bits in place and omit the VCEQ plus Q-form VBIT. The
			// explicit overflow clamp remains because VFP does not implement that
			// PCSX2 rule. FZ-disabled blocks retain both branchless selects.
			bool EmitNormalizeVuFloatQuadInPlace(unsigned vq, bool overflow_clamp)
			{
				const bool input_flush_to_zero = VuFpcrFlushesInputsToZero();
				if (input_flush_to_zero)
				{
					// The complete software path is four instructions without overflow
					// clamping and seven with it. FZ removes respectively all four or
					// only the denormal compare/select pair.
					m_normalization_instructions_removed += overflow_clamp ? 2 : 4;
					if (!overflow_clamp)
						return true;
				}

				if (!m_code.EmitVandQ(VU_NORM_EXPV_Q, vq, VU_NORM_EXP_Q) ||
					!m_code.EmitVandQ(VU_NORM_SIGNV_Q, vq, VU_NORM_SIGN_Q))
				{
					return false;
				}
				if (!input_flush_to_zero &&
					(!m_code.EmitVceqI32ZeroQ(VU_NORM_MASK_Q, VU_NORM_EXPV_Q) ||
					 !m_code.EmitVbitQ(vq, VU_NORM_SIGNV_Q, VU_NORM_MASK_Q)))
				{
					return false;
				}

				if (!overflow_clamp)
					return true;

				return m_code.EmitVceqI32Q(VU_NORM_MASK_Q, VU_NORM_EXPV_Q, VU_NORM_EXP_Q) &&
					m_code.EmitVorrQ(VU_NORM_TMP_Q, VU_NORM_SIGNV_Q, VU_NORM_MAXF_Q) &&
					m_code.EmitVbitQ(vq, VU_NORM_TMP_Q, VU_NORM_MASK_Q);
			}

			// A broadcast operand has one semantic source word. Keep only two
			// identical copies in a D register and apply the same vuDouble() bit
			// selects with D-form NEON operations. Cortex-A9 MPE tables 3-4 and 3-6
			// charge one issue cycle for each D operation but two for its Q form.
			// The arithmetic reads the low aliased S register for every VU lane.
			bool EmitNormalizeVuFloatDInPlace(unsigned vd, bool overflow_clamp)
			{
				const bool input_flush_to_zero = VuFpcrFlushesInputsToZero();
				if (input_flush_to_zero)
				{
					m_normalization_instructions_removed += overflow_clamp ? 2 : 4;
					if (!overflow_clamp)
						return true;
				}

				constexpr unsigned exp_d = VU_NORM_EXP_Q * 2;
				constexpr unsigned sign_d = VU_NORM_SIGN_Q * 2;
				constexpr unsigned maxf_d = VU_NORM_MAXF_Q * 2;
				constexpr unsigned expv_d = VU_NORM_EXPV_Q * 2;
				constexpr unsigned signv_d = VU_NORM_SIGNV_Q * 2;
				constexpr unsigned tmp_d = VU_NORM_TMP_Q * 2;
				constexpr unsigned mask_d = VU_NORM_MASK_Q * 2;
				if (!m_code.EmitVandD(expv_d, vd, exp_d) ||
					!m_code.EmitVandD(signv_d, vd, sign_d))
				{
					return false;
				}
				if (!input_flush_to_zero &&
					(!m_code.EmitVceqI32ZeroD(mask_d, expv_d) ||
					 !m_code.EmitVbitD(vd, signv_d, mask_d)))
				{
					return false;
				}
				if (!overflow_clamp)
					return true;
				return m_code.EmitVceqI32D(mask_d, expv_d, exp_d) &&
					m_code.EmitVorrD(tmp_d, signv_d, maxf_d) &&
					m_code.EmitVbitD(vd, tmp_d, mask_d);
			}

			// Normalizes the two FMAC operand quads with vuDouble() semantics.
			bool EmitNormalizeVuFloatQuads(unsigned vq_a, unsigned vq_b)
			{
#if defined(INT_VUDOUBLEHACK)
				(void)vq_a;
				(void)vq_b;
				return true;
#else
				const bool overflow_clamp = CHECK_VU_OVERFLOW(0);
				return EmitEnsureVuFloatInputNormalizeConstants(overflow_clamp) &&
					EmitNormalizeVuFloatQuadInPlace(vq_a, overflow_clamp) &&
					EmitNormalizeVuFloatQuadInPlace(vq_b, overflow_clamp);
#endif
			}

			// Normalizes a single operand quad with vuDouble() semantics.
			bool EmitNormalizeVuFloatQuad1(unsigned vq)
			{
#if defined(INT_VUDOUBLEHACK)
				(void)vq;
				return true;
#else
				const bool overflow_clamp = CHECK_VU_OVERFLOW(0);
				return EmitEnsureVuFloatInputNormalizeConstants(overflow_clamp) &&
					EmitNormalizeVuFloatQuadInPlace(vq, overflow_clamp);
#endif
			}

			// Normalizes the three MADD/MSUB operand quads (ACC, fs, operand).
			bool EmitNormalizeVuFloatQuads3(unsigned vq_a, unsigned vq_b, unsigned vq_c)
			{
#if defined(INT_VUDOUBLEHACK)
				(void)vq_a;
				(void)vq_b;
				(void)vq_c;
				return true;
#else
				const bool overflow_clamp = CHECK_VU_OVERFLOW(0);
				return EmitEnsureVuFloatInputNormalizeConstants(overflow_clamp) &&
					EmitNormalizeVuFloatQuadInPlace(vq_a, overflow_clamp) &&
					EmitNormalizeVuFloatQuadInPlace(vq_b, overflow_clamp) &&
					EmitNormalizeVuFloatQuadInPlace(vq_c, overflow_clamp);
#endif
			}

			void RecordNormalizedOperandBypass(bool overflow_clamp)
			{
				m_normalized_operand_quad_bypasses++;
				// Count against the complete PCSX2 software normalization body:
				// four NEON instructions for signed-denormal flushing plus three for
				// overflow clamping. A known normalized value bypasses the entire body;
				// an unknown value under FZ separately records its two-instruction
				// denormal compare/select elision in EmitNormalizeVuFloatQuadInPlace().
				m_normalization_instructions_removed += overflow_clamp ? 7 : 4;
			}

			bool EmitNormalizeKnownQuad(unsigned vq, bool already_normalized,
				bool overflow_clamp)
			{
				if (already_normalized)
				{
					RecordNormalizedOperandBypass(overflow_clamp);
#if defined(VITASX2_QEMU_VALIDATION)
					return EmitQemuNormalizedOperandBypassCounter();
#else
					return true;
#endif
				}
				return EmitNormalizeVuFloatQuadInPlace(vq, overflow_clamp);
			}

			bool EmitNormalizeKnownD(unsigned vd, bool already_normalized,
				bool overflow_clamp)
			{
				if (already_normalized)
				{
					RecordNormalizedOperandBypass(overflow_clamp);
#if defined(VITASX2_QEMU_VALIDATION)
					return EmitQemuNormalizedOperandBypassCounter();
#else
					return true;
#endif
				}
				return EmitNormalizeVuFloatDInPlace(vd, overflow_clamp);
			}

			bool EmitNormalizeVuFloatQuadsKnown(unsigned vq_a, bool a_normalized,
				unsigned vq_b, bool b_normalized)
			{
#if defined(INT_VUDOUBLEHACK)
				(void)vq_a;
				(void)a_normalized;
				(void)vq_b;
				(void)b_normalized;
				return true;
#else
				const bool overflow_clamp = CHECK_VU_OVERFLOW(0);
				return ((a_normalized && b_normalized) ||
					EmitEnsureVuFloatInputNormalizeConstants(overflow_clamp)) &&
					EmitNormalizeKnownQuad(vq_a, a_normalized, overflow_clamp) &&
					EmitNormalizeKnownQuad(vq_b, b_normalized, overflow_clamp);
#endif
			}

			bool EmitNormalizeVuFloatQuadAndOperandKnown(unsigned vq_a,
				bool a_normalized, unsigned operand_q, int operand_s,
				bool operand_normalized)
			{
#if defined(INT_VUDOUBLEHACK)
				(void)vq_a;
				(void)a_normalized;
				(void)operand_q;
				(void)operand_s;
				(void)operand_normalized;
				return true;
#else
				const bool overflow_clamp = CHECK_VU_OVERFLOW(0);
				return ((a_normalized && operand_normalized) ||
					EmitEnsureVuFloatInputNormalizeConstants(overflow_clamp)) &&
					EmitNormalizeKnownQuad(vq_a, a_normalized, overflow_clamp) &&
					(operand_s >= 0 ?
						EmitNormalizeKnownD(static_cast<unsigned>(operand_s) / 2,
							operand_normalized, overflow_clamp) :
						EmitNormalizeKnownQuad(operand_q, operand_normalized,
							overflow_clamp));
#endif
			}

			bool EmitNormalizeVuFloatQuads3Known(unsigned vq_a, bool a_normalized,
				unsigned vq_b, bool b_normalized, unsigned vq_c, bool c_normalized)
			{
#if defined(INT_VUDOUBLEHACK)
				(void)vq_a;
				(void)a_normalized;
				(void)vq_b;
				(void)b_normalized;
				(void)vq_c;
				(void)c_normalized;
				return true;
#else
				const bool overflow_clamp = CHECK_VU_OVERFLOW(0);
				return ((a_normalized && b_normalized && c_normalized) ||
					EmitEnsureVuFloatInputNormalizeConstants(overflow_clamp)) &&
					EmitNormalizeKnownQuad(vq_a, a_normalized, overflow_clamp) &&
					EmitNormalizeKnownQuad(vq_b, b_normalized, overflow_clamp) &&
					EmitNormalizeKnownQuad(vq_c, c_normalized, overflow_clamp);
#endif
			}

			bool EmitNormalizeVuFloatQuadsAndOperandKnown(unsigned vq_a,
				bool a_normalized, unsigned vq_b, bool b_normalized,
				unsigned operand_q, int operand_s, bool operand_normalized)
			{
#if defined(INT_VUDOUBLEHACK)
				(void)vq_a;
				(void)a_normalized;
				(void)vq_b;
				(void)b_normalized;
				(void)operand_q;
				(void)operand_s;
				(void)operand_normalized;
				return true;
#else
				const bool overflow_clamp = CHECK_VU_OVERFLOW(0);
				return ((a_normalized && b_normalized && operand_normalized) ||
					EmitEnsureVuFloatInputNormalizeConstants(overflow_clamp)) &&
					EmitNormalizeKnownQuad(vq_a, a_normalized, overflow_clamp) &&
					EmitNormalizeKnownQuad(vq_b, b_normalized, overflow_clamp) &&
					(operand_s >= 0 ?
						EmitNormalizeKnownD(static_cast<unsigned>(operand_s) / 2,
							operand_normalized, overflow_clamp) :
						EmitNormalizeKnownQuad(operand_q, operand_normalized,
							overflow_clamp));
#endif
			}

			bool EmitNormalizeVuFloatWord(unsigned reg, unsigned exponent_reg, unsigned scratch_reg)
			{
#if defined(INT_VUDOUBLEHACK)
				(void)reg;
				(void)exponent_reg;
				(void)scratch_reg;
				return true;
#else
				// PCSX2 owner: VUops.cpp::vuDouble() / VUmicroFast.h::VuDouble().
				// Normalize denormals to signed zero and, when the VU overflow
				// clamp is enabled, infinities/NaNs to signed max finite.
				const bool overflow_clamp = CHECK_VU_OVERFLOW(0);
				if (!EmitAndRegImm32(exponent_reg, reg, FPU_FLOAT_EXPONENT_MASK, scratch_reg) ||
					!m_code.EmitCmpImm32(exponent_reg, 0))
				{
					return false;
				}

				const size_t exponent_nonzero = m_code.EmitBranchPlaceholder(Condition::NE);
				if (exponent_nonzero == static_cast<size_t>(-1))
					return false;

				if (!EmitAndRegImm32(reg, reg, FPU_FLOAT_SIGN_MASK, scratch_reg))
					return false;

				const size_t done_from_zero = m_code.EmitBranchPlaceholder();
				if (done_from_zero == static_cast<size_t>(-1))
					return false;

				const size_t exponent_nonzero_target = m_code.Size();
				if (!m_code.PatchBranch(exponent_nonzero, exponent_nonzero_target, Condition::NE))
					return false;

				size_t done_from_finite = static_cast<size_t>(-1);
				if (overflow_clamp)
				{
					if (!EmitCmpRegImm32(exponent_reg, FPU_FLOAT_EXPONENT_MASK, scratch_reg))
						return false;

					done_from_finite = m_code.EmitBranchPlaceholder(Condition::NE);
					if (done_from_finite == static_cast<size_t>(-1))
						return false;

					if (!EmitAndRegImm32(reg, reg, FPU_FLOAT_SIGN_MASK, scratch_reg) ||
						!EmitOrrRegImm32(reg, reg, FPU_FLOAT_MAX_FINITE, scratch_reg))
					{
						return false;
					}
				}

				const size_t done_target = m_code.Size();
				return m_code.PatchBranch(done_from_zero, done_target) &&
					(done_from_finite == static_cast<size_t>(-1) ||
						m_code.PatchBranch(done_from_finite, done_target, Condition::NE));
#endif
			}

			bool EmitAbsWord(unsigned rd, unsigned rn, unsigned scratch_reg)
			{
				return EmitAndRegImm32(rd, rn, ~FPU_FLOAT_SIGN_MASK, scratch_reg);
			}

			bool EmitSetInvalidIfNegativeNonzero(unsigned reg, unsigned status_reg,
				unsigned temp_reg, unsigned scratch_reg)
			{
				// PCSX2 owner: VUops.cpp::_vuSQRT()/_vuRSQRT() `ft < 0.0`.
				// After VuDouble normalization, negative non-zero finite values
				// and negative infinity compare less than zero. Raw NaNs compare
				// unordered when overflow clamp is disabled, so keep them out.
				if (!EmitAbsWord(temp_reg, reg, scratch_reg) ||
					!m_code.EmitCmpImm32(temp_reg, 0))
				{
					return false;
				}

				const size_t zero = m_code.EmitBranchPlaceholder(Condition::EQ);
				if (zero == static_cast<size_t>(-1))
					return false;

				if (!EmitAndRegImm32(temp_reg, reg, FPU_FLOAT_SIGN_MASK, scratch_reg) ||
					!m_code.EmitCmpImm32(temp_reg, 0))
				{
					return false;
				}

				const size_t non_negative = m_code.EmitBranchPlaceholder(Condition::EQ);
				if (non_negative == static_cast<size_t>(-1))
					return false;

				size_t exponent_not_special = static_cast<size_t>(-1);
				size_t raw_nan = static_cast<size_t>(-1);
				if (!CHECK_VU_OVERFLOW(0))
				{
					if (!EmitAndRegImm32(scratch_reg, reg, FPU_FLOAT_EXPONENT_MASK, temp_reg) ||
						!EmitCmpRegImm32(scratch_reg, FPU_FLOAT_EXPONENT_MASK, temp_reg))
					{
						return false;
					}

					exponent_not_special = m_code.EmitBranchPlaceholder(Condition::NE);
					if (exponent_not_special == static_cast<size_t>(-1))
						return false;

					if (!EmitAndRegImm32(scratch_reg, reg, FPU_FLOAT_MANTISSA_MASK, temp_reg) ||
						!m_code.EmitCmpImm32(scratch_reg, 0))
					{
						return false;
					}

					raw_nan = m_code.EmitBranchPlaceholder(Condition::NE);
					if (raw_nan == static_cast<size_t>(-1))
						return false;
				}

				const size_t set_invalid = m_code.Size();
				if (exponent_not_special != static_cast<size_t>(-1) &&
					!m_code.PatchBranch(exponent_not_special, set_invalid, Condition::NE))
				{
					return false;
				}

				if (!EmitOrrRegImm32(status_reg, status_reg, 0x10u, scratch_reg))
					return false;

				const size_t done = m_code.Size();
				return m_code.PatchBranch(zero, done, Condition::EQ) &&
					m_code.PatchBranch(non_negative, done, Condition::EQ) &&
					(raw_nan == static_cast<size_t>(-1) ||
						m_code.PatchBranch(raw_nan, done, Condition::NE));
			}

			bool EmitComputeDiv(unsigned q_reg, unsigned fs_reg, unsigned ft_reg,
				unsigned temp_reg, unsigned scratch_reg)
			{
				constexpr unsigned VFP_FS_S0 = 0;
				constexpr unsigned VFP_FT_S1 = 1;
				constexpr unsigned VFP_Q_S2 = 2;
				return m_code.EmitVmovCoreToS(VFP_FS_S0, fs_reg) &&
					m_code.EmitVmovCoreToS(VFP_FT_S1, ft_reg) &&
					m_code.EmitVdivF32(VFP_Q_S2, VFP_FS_S0, VFP_FT_S1) &&
					m_code.EmitVmovSToCore(q_reg, VFP_Q_S2) &&
					EmitNormalizeVuFloatWord(q_reg, temp_reg, scratch_reg);
			}

			bool EmitComputeSqrtAbsFt(unsigned q_reg, unsigned ft_reg,
				unsigned temp_reg, unsigned scratch_reg)
			{
				constexpr unsigned VFP_FT_S1 = 1;
				constexpr unsigned VFP_Q_S2 = 2;
				return EmitAbsWord(ft_reg, ft_reg, scratch_reg) &&
					m_code.EmitVmovCoreToS(VFP_FT_S1, ft_reg) &&
					m_code.EmitVsqrtF32(VFP_Q_S2, VFP_FT_S1) &&
					m_code.EmitVmovSToCore(q_reg, VFP_Q_S2) &&
					EmitNormalizeVuFloatWord(q_reg, temp_reg, scratch_reg);
			}

			bool EmitStoreVu1QAndStatus(unsigned q_reg, unsigned status_reg,
				unsigned temp_reg, unsigned scratch_reg)
			{
				return m_code.EmitStrImm12(q_reg, HOST_VU, VuOffset(offsetof(VURegs, q))) &&
					m_code.EmitLdrImm12(temp_reg, HOST_VU, VuOffset(offsetof(VURegs, statusflag))) &&
					EmitAndRegImm32(temp_reg, temp_reg, ~0x30u, scratch_reg) &&
					m_code.EmitOrrReg(temp_reg, temp_reg, status_reg) &&
					m_code.EmitStrImm12(temp_reg, HOST_VU, VuOffset(offsetof(VURegs, statusflag)));
			}

			bool EmitInlineLowerFdiv(u32 code, VUInterpFast::LowerFastKind kind)
			{
				constexpr unsigned HOST_FS_Q = 0;
				constexpr unsigned HOST_FT = 1;
				constexpr unsigned HOST_TEMP = 2;
				constexpr unsigned HOST_STATUS_BITS = 3;
				const unsigned fs = VUInterpFast::Fs(code);
				const unsigned ft = VUInterpFast::Ft(code);
				const unsigned fsf = VUInterpFast::Fsf(code);
				const unsigned ftf = (code >> 23) & 0x03;
				bool emitted_body = true;

				// PCSX2 owners: VUops.cpp::_vuDIV()/_vuSQRT()/_vuRSQRT() and
				// VUmicroFast.h::ExecuteLowerNoUpperKnownKind(). Micro-mode
				// writes VU->q/statusflag here; VI[Q]/VI[STATUS_FLAG] become
				// visible later through the FDIV pipe flush.
				if (!m_code.EmitMovImm8(HOST_STATUS_BITS, 0))
					return false;

				switch (kind)
				{
					case VUInterpFast::LowerFastKind::DIV:
					{
						emitted_body =
							EmitLoadVfWord(HOST_FS_Q, fs, fsf) &&
							EmitNormalizeVuFloatWord(HOST_FS_Q, HOST_TEMP, HOST_CALL_SCRATCH) &&
							EmitLoadVfWord(HOST_FT, ft, ftf) &&
							EmitNormalizeVuFloatWord(HOST_FT, HOST_TEMP, HOST_CALL_SCRATCH) &&
							EmitAbsWord(HOST_TEMP, HOST_FT, HOST_CALL_SCRATCH) &&
							m_code.EmitCmpImm32(HOST_TEMP, 0);
						if (!emitted_body)
							return false;

						const size_t divisor_nonzero = m_code.EmitBranchPlaceholder(Condition::NE);
						if (divisor_nonzero == static_cast<size_t>(-1))
							return false;

						emitted_body =
							EmitAbsWord(HOST_TEMP, HOST_FS_Q, HOST_CALL_SCRATCH) &&
							m_code.EmitMovImm8(HOST_STATUS_BITS, 0x20) &&
							m_code.EmitCmpImm32(HOST_TEMP, 0) &&
							m_code.EmitMovImm8(HOST_STATUS_BITS, 0x10, Condition::EQ) &&
							m_code.EmitEorReg(HOST_FS_Q, HOST_FS_Q, HOST_FT) &&
							EmitAndRegImm32(HOST_FS_Q, HOST_FS_Q, FPU_FLOAT_SIGN_MASK, HOST_CALL_SCRATCH) &&
							EmitOrrRegImm32(HOST_FS_Q, HOST_FS_Q, FPU_FLOAT_MAX_FINITE, HOST_CALL_SCRATCH);
						if (!emitted_body)
							return false;

						const size_t done_from_zero = m_code.EmitBranchPlaceholder();
						if (done_from_zero == static_cast<size_t>(-1))
							return false;

						const size_t divisor_nonzero_target = m_code.Size();
						if (!m_code.PatchBranch(divisor_nonzero, divisor_nonzero_target, Condition::NE) ||
							!EmitComputeDiv(HOST_FS_Q, HOST_FS_Q, HOST_FT, HOST_TEMP, HOST_CALL_SCRATCH))
						{
							return false;
						}

						emitted_body = m_code.PatchBranch(done_from_zero, m_code.Size()) &&
							EmitStoreVu1QAndStatus(HOST_FS_Q, HOST_STATUS_BITS, HOST_TEMP, HOST_CALL_SCRATCH);
						break;
					}

					case VUInterpFast::LowerFastKind::SQRT:
						emitted_body =
							EmitLoadVfWord(HOST_FT, ft, ftf) &&
							EmitNormalizeVuFloatWord(HOST_FT, HOST_TEMP, HOST_CALL_SCRATCH) &&
							EmitSetInvalidIfNegativeNonzero(HOST_FT, HOST_STATUS_BITS, HOST_TEMP, HOST_CALL_SCRATCH) &&
							EmitComputeSqrtAbsFt(HOST_FS_Q, HOST_FT, HOST_TEMP, HOST_CALL_SCRATCH) &&
							EmitStoreVu1QAndStatus(HOST_FS_Q, HOST_STATUS_BITS, HOST_TEMP, HOST_CALL_SCRATCH);
						break;

					case VUInterpFast::LowerFastKind::RSQRT:
					{
						emitted_body =
							EmitLoadVfWord(HOST_FS_Q, fs, fsf) &&
							EmitNormalizeVuFloatWord(HOST_FS_Q, HOST_TEMP, HOST_CALL_SCRATCH) &&
							EmitLoadVfWord(HOST_FT, ft, ftf) &&
							EmitNormalizeVuFloatWord(HOST_FT, HOST_TEMP, HOST_CALL_SCRATCH) &&
							EmitAbsWord(HOST_TEMP, HOST_FT, HOST_CALL_SCRATCH) &&
							m_code.EmitCmpImm32(HOST_TEMP, 0);
						if (!emitted_body)
							return false;

						const size_t ft_nonzero = m_code.EmitBranchPlaceholder(Condition::NE);
						if (ft_nonzero == static_cast<size_t>(-1))
							return false;

						emitted_body =
							EmitAbsWord(HOST_TEMP, HOST_FS_Q, HOST_CALL_SCRATCH) &&
							m_code.EmitMovImm8(HOST_STATUS_BITS, 0x20) &&
							m_code.EmitCmpImm32(HOST_TEMP, 0);
						if (!emitted_body)
							return false;

						const size_t fs_nonzero = m_code.EmitBranchPlaceholder(Condition::NE);
						if (fs_nonzero == static_cast<size_t>(-1))
							return false;

						emitted_body =
							m_code.EmitMovImm8(HOST_STATUS_BITS, 0x30) &&
							m_code.EmitEorReg(HOST_FS_Q, HOST_FS_Q, HOST_FT) &&
							EmitAndRegImm32(HOST_FS_Q, HOST_FS_Q, FPU_FLOAT_SIGN_MASK, HOST_CALL_SCRATCH);
						if (!emitted_body)
							return false;

						const size_t done_from_zero_zero = m_code.EmitBranchPlaceholder();
						if (done_from_zero_zero == static_cast<size_t>(-1))
							return false;

						const size_t fs_nonzero_target = m_code.Size();
						if (!m_code.PatchBranch(fs_nonzero, fs_nonzero_target, Condition::NE))
							return false;

						emitted_body =
							m_code.EmitEorReg(HOST_FS_Q, HOST_FS_Q, HOST_FT) &&
							EmitAndRegImm32(HOST_FS_Q, HOST_FS_Q, FPU_FLOAT_SIGN_MASK, HOST_CALL_SCRATCH) &&
							EmitOrrRegImm32(HOST_FS_Q, HOST_FS_Q, FPU_FLOAT_MAX_FINITE, HOST_CALL_SCRATCH);
						if (!emitted_body)
							return false;

						const size_t done_from_zero = m_code.EmitBranchPlaceholder();
						if (done_from_zero == static_cast<size_t>(-1))
							return false;

						const size_t ft_nonzero_target = m_code.Size();
						if (!m_code.PatchBranch(ft_nonzero, ft_nonzero_target, Condition::NE) ||
							!EmitSetInvalidIfNegativeNonzero(HOST_FT, HOST_STATUS_BITS, HOST_TEMP, HOST_CALL_SCRATCH) ||
							!EmitComputeSqrtAbsFt(HOST_FT, HOST_FT, HOST_TEMP, HOST_CALL_SCRATCH) ||
							!EmitComputeDiv(HOST_FS_Q, HOST_FS_Q, HOST_FT, HOST_TEMP, HOST_CALL_SCRATCH))
						{
							return false;
						}

						const size_t done_target = m_code.Size();
						emitted_body =
							m_code.PatchBranch(done_from_zero_zero, done_target) &&
							m_code.PatchBranch(done_from_zero, done_target) &&
							EmitStoreVu1QAndStatus(HOST_FS_Q, HOST_STATUS_BITS, HOST_TEMP, HOST_CALL_SCRATCH);
						break;
					}

					default:
						return false;
				}

				if (!emitted_body)
					return false;

#if defined(VITASX2_QEMU_VALIDATION)
				return EmitQemuLowerFdivInlineCounter();
#else
				return true;
#endif
			}

			bool EmitLoadVuLaneToS(unsigned sreg, unsigned vf, unsigned lane,
				unsigned word_reg, unsigned temp_reg, unsigned scratch_reg)
			{
				return EmitLoadVfWord(word_reg, vf, lane) &&
					EmitNormalizeVuFloatWord(word_reg, temp_reg, scratch_reg) &&
					m_code.EmitVmovCoreToS(sreg, word_reg);
			}

			bool EmitStoreSToVuP(unsigned sreg, unsigned word_reg)
			{
				return m_code.EmitVmovSToCore(word_reg, sreg) &&
					m_code.EmitStrImm12(word_reg, HOST_VU, VuOffset(offsetof(VURegs, p)));
			}

			bool EmitLoadOneToS(unsigned sreg, unsigned word_reg)
			{
				return m_code.EmitMovImm32(word_reg, 0x3f800000u) &&
					m_code.EmitVmovCoreToS(sreg, word_reg);
			}

			bool EmitLoadFloatConstToS(unsigned sreg, u32 bits, unsigned word_reg)
			{
				return m_code.EmitMovImm32(word_reg, bits) &&
					m_code.EmitVmovCoreToS(sreg, word_reg);
			}

			bool EmitLoadFloatConstToD(unsigned dreg, unsigned sreg, u32 bits, unsigned word_reg)
			{
				return EmitLoadFloatConstToS(sreg, bits, word_reg) &&
					m_code.EmitVcvtF64F32(dreg, sreg);
			}

			bool EmitFloatConstMulInputToD(unsigned accum_dreg, u32 const_bits,
				unsigned input_sreg, unsigned const_sreg, unsigned word_reg)
			{
				return EmitLoadFloatConstToS(const_sreg, const_bits, word_reg) &&
					m_code.EmitVmulF32(const_sreg, const_sreg, input_sreg) &&
					m_code.EmitVcvtF64F32(accum_dreg, const_sreg);
			}

			bool EmitAddPowerTermF64(unsigned accum_dreg, unsigned power_dreg,
				u32 const_bits, unsigned const_dreg, unsigned const_sreg, unsigned word_reg)
			{
				return EmitLoadFloatConstToD(const_dreg, const_sreg, const_bits, word_reg) &&
					m_code.EmitVmulF64(const_dreg, const_dreg, power_dreg) &&
					m_code.EmitVaddF64(accum_dreg, accum_dreg, const_dreg);
			}

			bool EmitNormalizeSToS(unsigned sreg, unsigned word_reg,
				unsigned temp_reg, unsigned scratch_reg)
			{
				return m_code.EmitVmovSToCore(word_reg, sreg) &&
					EmitNormalizeVuFloatWord(word_reg, temp_reg, scratch_reg) &&
					m_code.EmitVmovCoreToS(sreg, word_reg);
			}

			bool EmitStoreNormalizedSToVuP(unsigned sreg, unsigned word_reg,
				unsigned temp_reg, unsigned scratch_reg)
			{
				return m_code.EmitVmovSToCore(word_reg, sreg) &&
					EmitNormalizeVuFloatWord(word_reg, temp_reg, scratch_reg) &&
					m_code.EmitStrImm12(word_reg, HOST_VU, VuOffset(offsetof(VURegs, p)));
			}

			bool EmitReciprocalIfNonzero(unsigned value_sreg, unsigned one_sreg,
				unsigned word_reg, unsigned temp_reg, unsigned scratch_reg)
			{
				if (!m_code.EmitVmovSToCore(word_reg, value_sreg) ||
					!EmitAbsWord(temp_reg, word_reg, scratch_reg) ||
					!m_code.EmitCmpImm32(temp_reg, 0))
				{
					return false;
				}

				const size_t zero = m_code.EmitBranchPlaceholder(Condition::EQ);
				if (zero == static_cast<size_t>(-1))
					return false;

				if (!EmitLoadOneToS(one_sreg, word_reg) ||
					!m_code.EmitVdivF32(value_sreg, one_sreg, value_sreg))
				{
					return false;
				}

				return m_code.PatchBranch(zero, m_code.Size(), Condition::EQ);
			}

			bool EmitDoubleReciprocalIfNonzero(unsigned value_sreg,
				unsigned word_reg, unsigned temp_reg, unsigned scratch_reg)
			{
				if (!m_code.EmitVmovSToCore(word_reg, value_sreg) ||
					!EmitAbsWord(temp_reg, word_reg, scratch_reg) ||
					!m_code.EmitCmpImm32(temp_reg, 0))
				{
					return false;
				}

				const size_t zero = m_code.EmitBranchPlaceholder(Condition::EQ);
				if (zero == static_cast<size_t>(-1))
					return false;

				return m_code.EmitVcvtF64F32(0, value_sreg) &&
					m_code.EmitMovImm8(word_reg, 0) &&
					m_code.EmitMovImm32(temp_reg, 0x3ff00000u) &&
					m_code.EmitVmovCorePairToD(1, word_reg, temp_reg) &&
					m_code.EmitVdivF64(0, 1, 0) &&
					m_code.EmitVcvtF32F64(value_sreg, 0) &&
					m_code.PatchBranch(zero, m_code.Size(), Condition::EQ);
			}

			size_t EmitBranchIfFloatNotNonNegative(unsigned word_reg, unsigned temp_reg,
				unsigned scratch_reg)
			{
				// Implements the C++ `p >= 0.0f` branch predicate used by the
				// PCSX2 EFU ops: true for +/-0, positive finite, and +inf; false
				// for negative values and NaNs.
				if (!EmitAbsWord(temp_reg, word_reg, scratch_reg) ||
					!m_code.EmitCmpImm32(temp_reg, 0))
				{
					return static_cast<size_t>(-1);
				}

				const size_t do_op_from_zero = m_code.EmitBranchPlaceholder(Condition::EQ);
				if (do_op_from_zero == static_cast<size_t>(-1))
					return static_cast<size_t>(-1);

				if (!EmitAndRegImm32(temp_reg, word_reg, FPU_FLOAT_SIGN_MASK, scratch_reg) ||
					!m_code.EmitCmpImm32(temp_reg, 0))
				{
					return static_cast<size_t>(-1);
				}

				const size_t skip_from_negative = m_code.EmitBranchPlaceholder(Condition::NE);
				if (skip_from_negative == static_cast<size_t>(-1))
					return static_cast<size_t>(-1);

				if (!EmitAndRegImm32(temp_reg, word_reg, FPU_FLOAT_EXPONENT_MASK, scratch_reg) ||
					!EmitCmpRegImm32(temp_reg, FPU_FLOAT_EXPONENT_MASK, scratch_reg))
				{
					return static_cast<size_t>(-1);
				}

				const size_t do_op_from_finite = m_code.EmitBranchPlaceholder(Condition::NE);
				if (do_op_from_finite == static_cast<size_t>(-1))
					return static_cast<size_t>(-1);

				if (!EmitAndRegImm32(temp_reg, word_reg, FPU_FLOAT_MANTISSA_MASK, scratch_reg) ||
					!m_code.EmitCmpImm32(temp_reg, 0))
				{
					return static_cast<size_t>(-1);
				}

				const size_t skip_from_nan = m_code.EmitBranchPlaceholder(Condition::NE);
				if (skip_from_nan == static_cast<size_t>(-1))
					return static_cast<size_t>(-1);

				const size_t do_op = m_code.Size();
				if (!m_code.PatchBranch(do_op_from_zero, do_op, Condition::EQ) ||
					!m_code.PatchBranch(do_op_from_finite, do_op, Condition::NE))
				{
					return static_cast<size_t>(-1);
				}

				const size_t continue_from_positive_inf = m_code.EmitBranchPlaceholder();
				if (continue_from_positive_inf == static_cast<size_t>(-1))
					return static_cast<size_t>(-1);

				const size_t skip_target = m_code.Size();
				if (!m_code.PatchBranch(skip_from_negative, skip_target, Condition::NE) ||
					!m_code.PatchBranch(skip_from_nan, skip_target, Condition::NE))
				{
					return static_cast<size_t>(-1);
				}

				return continue_from_positive_inf;
			}

			bool EmitSqrtIfNonNegative(unsigned value_sreg, unsigned word_reg,
				unsigned temp_reg, unsigned scratch_reg)
			{
				if (!m_code.EmitVmovSToCore(word_reg, value_sreg))
					return false;

				const size_t continue_from_positive_inf =
					EmitBranchIfFloatNotNonNegative(word_reg, temp_reg, scratch_reg);
				if (continue_from_positive_inf == static_cast<size_t>(-1))
					return false;

				const size_t done = m_code.EmitBranchPlaceholder();
				if (done == static_cast<size_t>(-1))
					return false;

				const size_t do_sqrt = m_code.Size();
				if (!m_code.PatchBranch(continue_from_positive_inf, do_sqrt) ||
					!m_code.EmitVsqrtF32(value_sreg, value_sreg))
				{
					return false;
				}

				return m_code.PatchBranch(done, m_code.Size());
			}

			bool EmitSqrtAndReciprocalIfNonNegative(unsigned value_sreg, unsigned one_sreg,
				unsigned word_reg, unsigned temp_reg, unsigned scratch_reg)
			{
				if (!m_code.EmitVmovSToCore(word_reg, value_sreg))
					return false;

				const size_t continue_from_positive_inf =
					EmitBranchIfFloatNotNonNegative(word_reg, temp_reg, scratch_reg);
				if (continue_from_positive_inf == static_cast<size_t>(-1))
					return false;

				const size_t done = m_code.EmitBranchPlaceholder();
				if (done == static_cast<size_t>(-1))
					return false;

				const size_t do_sqrt = m_code.Size();
				if (!m_code.PatchBranch(continue_from_positive_inf, do_sqrt) ||
					!m_code.EmitVsqrtF32(value_sreg, value_sreg) ||
					!EmitReciprocalIfNonzero(value_sreg, one_sreg, word_reg, temp_reg, scratch_reg))
				{
					return false;
				}

				return m_code.PatchBranch(done, m_code.Size());
			}

			bool EmitEfuSumXyzSquaresToS0(unsigned vf)
			{
				// PCSX2 owner: VUmicroFast.h::VuSumXYZSquaresNeon(). Quad load and
				// normalize, square XYZ with FPSCR-aware scalar VFP, then reduce
				// (x*x + y*y) + z*z with the same scalar order as the reference.
				return EmitLoadVfQuad(0, vf) &&
					EmitNormalizeVuFloatQuad1(0) &&
					m_code.EmitVmulF32(0, 0, 0) &&
					m_code.EmitVmulF32(1, 1, 1) &&
					m_code.EmitVmulF32(2, 2, 2) &&
					m_code.EmitVaddF32(0, 0, 1) &&
					m_code.EmitVaddF32(0, 0, 2);
			}

			bool EmitEfuSumXyzwToS0(unsigned vf)
			{
				// PCSX2 owner: VUmicroFast.h::VuSumXYZWNeon(). Quad load and
				// normalize, then reduce ((x + y) + z) + w.
				return EmitLoadVfQuad(0, vf) &&
					EmitNormalizeVuFloatQuad1(0) &&
					m_code.EmitVaddF32(0, 0, 1) &&
					m_code.EmitVaddF32(0, 0, 2) &&
					m_code.EmitVaddF32(0, 0, 3);
			}

			bool EmitEatanPolynomialFromS0ToP(unsigned word_reg,
				unsigned temp_reg, unsigned scratch_reg)
			{
				// PCSX2 owner: VUmicroFast.h::VuCalculateEatan(), which
				// preserves the original VUops.cpp coefficients while replacing
				// fixed-integer pow() calls with explicit double-power chains.
				static constexpr u32 EATAN_CONSTS[9] = {
					0x3f7ffff5u, 0xbeaaa61cu, 0x3e4c40a6u, 0xbe05fe6du,
					0x3dc577dfu, 0xbd6501c4u, 0x3cb31652u, 0xbb84d7e7u,
					0x3f490fdbu,
				};
				constexpr unsigned INPUT_D = 0;
				constexpr unsigned X2_D = 1;
				constexpr unsigned POWER_D = 2;
				constexpr unsigned ACCUM_D = 3;
				constexpr unsigned CONST_D = 4;
				constexpr unsigned CONST_S = 8;

				if (!EmitFloatConstMulInputToD(ACCUM_D, EATAN_CONSTS[0], 0, CONST_S, word_reg) ||
					!m_code.EmitVcvtF64F32(INPUT_D, 0) ||
					!m_code.EmitVmulF64(X2_D, INPUT_D, INPUT_D) ||
					!m_code.EmitVmulF64(POWER_D, INPUT_D, X2_D))
				{
					return false;
				}

				for (unsigned i = 1; i < 8; i++)
				{
					if (!EmitAddPowerTermF64(ACCUM_D, POWER_D, EATAN_CONSTS[i],
							CONST_D, CONST_S, word_reg))
					{
						return false;
					}
					if (i != 7 && !m_code.EmitVmulF64(POWER_D, POWER_D, X2_D))
						return false;
				}

				return m_code.EmitVcvtF32F64(0, ACCUM_D) &&
					EmitLoadFloatConstToS(1, EATAN_CONSTS[8], word_reg) &&
					m_code.EmitVaddF32(0, 0, 1) &&
					EmitStoreNormalizedSToVuP(0, word_reg, temp_reg, scratch_reg);
			}

			bool EmitEsinPolynomialFromS0ToP(unsigned word_reg,
				unsigned temp_reg, unsigned scratch_reg)
			{
				// PCSX2 owner: VUmicroFast.h::ExecuteLowerNoUpperKnownKind()
				// LowerFastKind::ESIN; the first coefficient multiply is
				// rounded as float before the remaining double polynomial.
				static constexpr u32 SIN_CONSTS[5] = {
					0x3f800000u, 0xbe2aaaa4u, 0x3c08873eu, 0xb94fb21fu,
					0x362e9c14u,
				};
				constexpr unsigned INPUT_D = 0;
				constexpr unsigned X2_D = 1;
				constexpr unsigned POWER_D = 2;
				constexpr unsigned ACCUM_D = 3;
				constexpr unsigned CONST_D = 4;
				constexpr unsigned CONST_S = 8;

				if (!EmitFloatConstMulInputToD(ACCUM_D, SIN_CONSTS[0], 0, CONST_S, word_reg) ||
					!m_code.EmitVcvtF64F32(INPUT_D, 0) ||
					!m_code.EmitVmulF64(X2_D, INPUT_D, INPUT_D) ||
					!m_code.EmitVmulF64(POWER_D, INPUT_D, X2_D))
				{
					return false;
				}

				for (unsigned i = 1; i < 5; i++)
				{
					if (!EmitAddPowerTermF64(ACCUM_D, POWER_D, SIN_CONSTS[i],
							CONST_D, CONST_S, word_reg))
					{
						return false;
					}
					if (i != 4 && !m_code.EmitVmulF64(POWER_D, POWER_D, X2_D))
						return false;
				}

				return m_code.EmitVcvtF32F64(0, ACCUM_D) &&
					EmitStoreNormalizedSToVuP(0, word_reg, temp_reg, scratch_reg);
			}

			bool EmitEexpPolynomialFromS0ToP(unsigned word_reg,
				unsigned temp_reg, unsigned scratch_reg)
			{
				// PCSX2 owner: VUmicroFast.h::ExecuteLowerNoUpperKnownKind()
				// LowerFastKind::EEXP. Keep the exact mixed float/double
				// evaluation order, then normalize before the final float divide.
				static constexpr u32 EXP_CONSTS[6] = {
					0x3e7fffa8u, 0x3d0007f4u, 0x3b29d3ffu,
					0x3933e553u, 0x36b63510u, 0x353961acu,
				};
				constexpr unsigned INPUT_D = 0;
				constexpr unsigned X2_D = 1;
				constexpr unsigned X3_D = 2;
				constexpr unsigned ACCUM_D = 3;
				constexpr unsigned CONST_D = 4;
				constexpr unsigned X4_D = 5;
				constexpr unsigned X5_D = 6;
				constexpr unsigned X6_D = 7;
				constexpr unsigned CONST_S = 8;

				if (!EmitLoadFloatConstToS(1, EXP_CONSTS[0], word_reg) ||
					!m_code.EmitVmulF32(1, 1, 0) ||
					!EmitLoadOneToS(2, word_reg) ||
					!m_code.EmitVaddF32(1, 2, 1) ||
					!m_code.EmitVcvtF64F32(ACCUM_D, 1) ||
					!m_code.EmitVcvtF64F32(INPUT_D, 0) ||
					!m_code.EmitVmulF64(X2_D, INPUT_D, INPUT_D) ||
					!m_code.EmitVmulF64(X3_D, X2_D, INPUT_D) ||
					!m_code.EmitVmulF64(X4_D, X2_D, X2_D) ||
					!m_code.EmitVmulF64(X5_D, X4_D, INPUT_D) ||
					!m_code.EmitVmulF64(X6_D, X3_D, X3_D) ||
					!EmitAddPowerTermF64(ACCUM_D, X2_D, EXP_CONSTS[1], CONST_D, CONST_S, word_reg) ||
					!EmitAddPowerTermF64(ACCUM_D, X3_D, EXP_CONSTS[2], CONST_D, CONST_S, word_reg) ||
					!EmitAddPowerTermF64(ACCUM_D, X4_D, EXP_CONSTS[3], CONST_D, CONST_S, word_reg) ||
					!EmitAddPowerTermF64(ACCUM_D, X5_D, EXP_CONSTS[4], CONST_D, CONST_S, word_reg) ||
					!EmitAddPowerTermF64(ACCUM_D, X6_D, EXP_CONSTS[5], CONST_D, CONST_S, word_reg) ||
					!m_code.EmitVcvtF32F64(0, ACCUM_D) ||
					!m_code.EmitVcvtF64F32(INPUT_D, 0) ||
					!m_code.EmitVmulF64(X2_D, INPUT_D, INPUT_D) ||
					!m_code.EmitVmulF64(X2_D, X2_D, X2_D) ||
					!m_code.EmitVcvtF32F64(0, X2_D) ||
					!EmitNormalizeSToS(0, word_reg, temp_reg, scratch_reg) ||
					!EmitLoadOneToS(1, word_reg) ||
					!m_code.EmitVdivF32(0, 1, 0))
				{
					return false;
				}

				return EmitStoreSToVuP(0, word_reg);
			}

			bool EmitEatanXyOrXzToP(unsigned fs, unsigned numerator_lane,
				unsigned word_reg, unsigned temp_reg, unsigned scratch_reg)
			{
				// PCSX2 owner: VUmicroFast.h lower EATANxy/EATANxz. The
				// zero-X branch stores +0 directly; nonzero uses VFP's exact
				// float divide before the shared EATAN polynomial.
				if (!EmitLoadVuLaneToS(0, fs, 0, word_reg, temp_reg, scratch_reg) ||
					!EmitAbsWord(temp_reg, word_reg, scratch_reg) ||
					!m_code.EmitCmpImm32(temp_reg, 0))
				{
					return false;
				}

				const size_t x_nonzero = m_code.EmitBranchPlaceholder(Condition::NE);
				if (x_nonzero == static_cast<size_t>(-1))
					return false;

				if (!m_code.EmitMovImm8(word_reg, 0) ||
					!m_code.EmitStrImm12(word_reg, HOST_VU, VuOffset(offsetof(VURegs, p))))
				{
					return false;
				}

				const size_t done_from_zero = m_code.EmitBranchPlaceholder();
				if (done_from_zero == static_cast<size_t>(-1))
					return false;

				const size_t nonzero_target = m_code.Size();
				if (!m_code.PatchBranch(x_nonzero, nonzero_target, Condition::NE) ||
					!EmitLoadVuLaneToS(1, fs, numerator_lane, word_reg, temp_reg, scratch_reg) ||
					!m_code.EmitVdivF32(0, 1, 0) ||
					!EmitEatanPolynomialFromS0ToP(word_reg, temp_reg, scratch_reg))
				{
					return false;
				}

				return m_code.PatchBranch(done_from_zero, m_code.Size());
			}

			bool EmitInlineLowerEfu(u32 code, VUInterpFast::LowerFastKind kind)
			{
				constexpr unsigned HOST_WORD = 0;
				constexpr unsigned HOST_TEMP = 2;
				const unsigned fs = VUInterpFast::Fs(code);
				const unsigned fsf = VUInterpFast::Fsf(code);
				bool emitted_body = true;

				// PCSX2 owners: VUops.cpp lower EFU ops and
				// VUmicroFast.h::ExecuteLowerNoUpperKnownKind(). These write
				// VU->p immediately; the generated EFU stall tail snapshots it
				// into the pending P pipe for later VI[REG_P] visibility.
				switch (kind)
				{
					case VUInterpFast::LowerFastKind::ESADD:
						emitted_body =
							EmitEfuSumXyzSquaresToS0(fs) &&
							EmitStoreSToVuP(0, HOST_WORD);
						break;

					case VUInterpFast::LowerFastKind::ERSADD:
						emitted_body =
							EmitEfuSumXyzSquaresToS0(fs) &&
							EmitReciprocalIfNonzero(0, 1, HOST_WORD, HOST_TEMP, HOST_CALL_SCRATCH) &&
							EmitStoreSToVuP(0, HOST_WORD);
						break;

					case VUInterpFast::LowerFastKind::ELENG:
						emitted_body =
							EmitEfuSumXyzSquaresToS0(fs) &&
							EmitSqrtIfNonNegative(0, HOST_WORD, HOST_TEMP, HOST_CALL_SCRATCH) &&
							EmitStoreSToVuP(0, HOST_WORD);
						break;

					case VUInterpFast::LowerFastKind::ERLENG:
						emitted_body =
							EmitEfuSumXyzSquaresToS0(fs) &&
							EmitSqrtAndReciprocalIfNonNegative(0, 1, HOST_WORD, HOST_TEMP, HOST_CALL_SCRATCH) &&
							EmitStoreSToVuP(0, HOST_WORD);
						break;

					case VUInterpFast::LowerFastKind::ESUM:
						emitted_body =
							EmitEfuSumXyzwToS0(fs) &&
							EmitStoreSToVuP(0, HOST_WORD);
						break;

					case VUInterpFast::LowerFastKind::ERCPR:
						emitted_body =
							EmitLoadVuLaneToS(0, fs, fsf, HOST_WORD, HOST_TEMP, HOST_CALL_SCRATCH) &&
							EmitDoubleReciprocalIfNonzero(0, HOST_WORD, HOST_TEMP, HOST_CALL_SCRATCH) &&
							EmitStoreSToVuP(0, HOST_WORD);
						break;

					case VUInterpFast::LowerFastKind::ESQRT:
						emitted_body =
							EmitLoadVuLaneToS(0, fs, fsf, HOST_WORD, HOST_TEMP, HOST_CALL_SCRATCH) &&
							EmitSqrtIfNonNegative(0, HOST_WORD, HOST_TEMP, HOST_CALL_SCRATCH) &&
							EmitStoreSToVuP(0, HOST_WORD);
						break;

					case VUInterpFast::LowerFastKind::ERSQRT:
						emitted_body =
							EmitLoadVuLaneToS(0, fs, fsf, HOST_WORD, HOST_TEMP, HOST_CALL_SCRATCH) &&
							EmitSqrtAndReciprocalIfNonNegative(0, 1, HOST_WORD, HOST_TEMP, HOST_CALL_SCRATCH) &&
							EmitStoreSToVuP(0, HOST_WORD);
						break;

					case VUInterpFast::LowerFastKind::EATANxy:
						emitted_body =
							EmitEatanXyOrXzToP(fs, 1, HOST_WORD, HOST_TEMP, HOST_CALL_SCRATCH);
						break;

					case VUInterpFast::LowerFastKind::EATANxz:
						emitted_body =
							EmitEatanXyOrXzToP(fs, 2, HOST_WORD, HOST_TEMP, HOST_CALL_SCRATCH);
						break;

					case VUInterpFast::LowerFastKind::ESIN:
						emitted_body =
							EmitLoadVuLaneToS(0, fs, fsf, HOST_WORD, HOST_TEMP, HOST_CALL_SCRATCH) &&
							EmitEsinPolynomialFromS0ToP(HOST_WORD, HOST_TEMP, HOST_CALL_SCRATCH);
						break;

					case VUInterpFast::LowerFastKind::EATAN:
						emitted_body =
							EmitLoadVuLaneToS(0, fs, fsf, HOST_WORD, HOST_TEMP, HOST_CALL_SCRATCH) &&
							EmitEatanPolynomialFromS0ToP(HOST_WORD, HOST_TEMP, HOST_CALL_SCRATCH);
						break;

					case VUInterpFast::LowerFastKind::EEXP:
						emitted_body =
							EmitLoadVuLaneToS(0, fs, fsf, HOST_WORD, HOST_TEMP, HOST_CALL_SCRATCH) &&
							EmitEexpPolynomialFromS0ToP(HOST_WORD, HOST_TEMP, HOST_CALL_SCRATCH);
						break;

					default:
						return false;
				}

				if (!emitted_body)
					return false;

#if defined(VITASX2_QEMU_VALIDATION)
				return EmitQemuLowerEfuInlineCounter();
#else
				return true;
#endif
			}

			bool EmitInlineLowerXgkick(u32 code)
			{
				// PCSX2 owners: VUmicroFast.h::ExecuteLowerNoUpperKnownKind(XGKICK)
				// and x86/microVU_Lower.inl::mVU_XGKICK(). VU0's opcode body is an
				// explicit no-op; VU1 records the delayed address while the microVU
				// transfer helper owns GIF parsing and PATH1 side effects.
				if (m_vu0_memory_map)
				{
#if defined(VITASX2_QEMU_VALIDATION)
					return EmitQemuLowerXgkickInlineCounter();
#else
					return true;
#endif
				}

				if (!m_code.EmitLdrImm12(0, HOST_VU, VuOffset(offsetof(VURegs, xgkickenable))) ||
					!m_code.EmitCmpImm32(0, 0))
				{
					return false;
				}
				const size_t skip_pending_flush = m_code.EmitBranchPlaceholder(Condition::EQ);
				if (skip_pending_flush == static_cast<size_t>(-1))
					return false;
				if (!EmitCallXgkickTransferFlush() ||
					!m_code.PatchBranch(skip_pending_flush, m_code.Size(), Condition::EQ))
				{
					return false;
				}

				const unsigned is = VUInterpFast::Is(code);
				bool emitted_body =
					EmitLoadViHalfwordRaw(0, is) &&
					EmitAndRegImm32(0, 0, 0x3ffu, HOST_CALL_SCRATCH) &&
					m_code.EmitMovRegShiftImm(0, 0, ShiftType::LSL, 4) &&
					m_code.EmitMovImm8(1, 1) &&
					m_code.EmitStrImm12(1, HOST_VU, VuOffset(offsetof(VURegs, xgkickenable))) &&
					m_code.EmitStrImm12(0, HOST_VU, VuOffset(offsetof(VURegs, xgkickaddr))) &&
					m_code.EmitMovImm32(1, 0x4000u) &&
					m_code.EmitSubReg(1, 1, 0) &&
					m_code.EmitStrImm12(1, HOST_VU, VuOffset(offsetof(VURegs, xgkickdiff))) &&
					m_code.EmitMovImm8(1, 0) &&
					m_code.EmitStrImm12(1, HOST_VU, VuOffset(offsetof(VURegs, xgkicksizeremaining))) &&
					m_code.EmitStrImm12(1, HOST_VU, VuOffset(offsetof(VURegs, xgkickendpacket))) &&
					m_code.EmitLdrImm12(1, HOST_VU, VuOffset(offsetof(VURegs, cycle))) &&
					m_code.EmitLdrImm12(2, HOST_VU, VuOffset(offsetof(VURegs, cycle) + 4)) &&
					m_code.EmitStrImm12(1, HOST_VU, VuOffset(offsetof(VURegs, xgkicklastcycle))) &&
					m_code.EmitStrImm12(2, HOST_VU, VuOffset(offsetof(VURegs, xgkicklastcycle) + 4)) &&
					m_code.EmitMovImm8(1, 1) &&
					m_code.EmitStrImm12(1, HOST_VU, VuOffset(offsetof(VURegs, xgkickcyclecount)));
				if (!emitted_body)
					return false;

				if (!THREAD_VU1)
				{
					constexpr size_t vpu_stat_offset = offsetof(VURegs, VI) +
						REG_VPU_STAT * sizeof(REG_VI);
					emitted_body =
						m_code.EmitMovImm32(3, static_cast<u32>(
							reinterpret_cast<uptr>(&VU0) + vpu_stat_offset)) &&
						m_code.EmitLdrImm12(1, 3, 0) &&
						EmitOrrRegImm32(1, 1, 1u << 12, HOST_CALL_SCRATCH) &&
						m_code.EmitStrImm12(1, 3, 0);
					if (!emitted_body)
						return false;
				}

#if defined(VITASX2_QEMU_VALIDATION)
				return EmitQemuLowerXgkickInlineCounter();
#else
				return true;
#endif
			}

			bool EmitInlineLowerBranchStallTest(const _VURegsNum& regs)
			{
				if (regs.VIread == 0)
					return true;

				// PCSX2 owner: VUops.cpp::_vuTestALUStalls(), reached through
				// _vuTestLowerStalls() for lower branch pipes. The generated
				// block scans the same four-entry IALU queue, advances
				// VU->cycle to the pending writer's ready cycle on register
				// overlap, and leaves pipe flushing to the following
				// _vuTestPipes() path exactly like _vu1Exec().
				constexpr unsigned HOST_INDEX = 0;
				constexpr unsigned HOST_PTR = 1;
				constexpr unsigned HOST_VALUE = 2;
				constexpr unsigned HOST_TEMP = 3;
				constexpr unsigned HOST_STALL_CYCLE_LO = HOST_CLIP_OLD;
				constexpr unsigned HOST_STALL_CYCLE_HI = HOST_CLIP_NEW;
				// Local-FMAC blocks retain the pipe aggregate in r10. Their inline
				// canonical warm-up scans can borrow LR because the block prologue has
				// already saved the caller's LR and these scans issue no host call.
				const unsigned HOST_COUNT = UsesResidentPipeActivity() ? 14u :
					HOST_STALL_SCRATCH;
				constexpr size_t base = offsetof(VURegs, ialu);

				std::array<size_t, 4> done_jumps{};
				if (!m_code.EmitLdrImm12(HOST_COUNT, HOST_VU, VuOffset(offsetof(VURegs, ialucount))) ||
					!m_code.EmitLdrImm12(HOST_STALL_CYCLE_LO, HOST_VU, VuOffset(offsetof(VURegs, cycle))) ||
					!m_code.EmitLdrImm12(HOST_STALL_CYCLE_HI, HOST_VU, VuOffset(offsetof(VURegs, cycle) + 4)) ||
					!m_code.EmitLdrImm12(HOST_INDEX, HOST_VU, VuOffset(offsetof(VURegs, ialureadpos))))
				{
					return false;
				}

				for (u32 slot = 0; slot < 4; slot++)
				{
					if (!m_code.EmitCmpImm32(HOST_COUNT, slot))
						return false;
					done_jumps[slot] = m_code.EmitBranchPlaceholder(Condition::LS);
					if (done_jumps[slot] == static_cast<size_t>(-1))
						return false;

					if (!m_code.EmitAddImm32(HOST_PTR, HOST_VU, base) ||
						!m_code.EmitAddRegShiftImm(HOST_PTR, HOST_PTR, HOST_INDEX, ShiftType::LSL, 4) ||
						!m_code.EmitAddRegShiftImm(HOST_PTR, HOST_PTR, HOST_INDEX, ShiftType::LSL, 3) ||
						!m_code.EmitLdrImm12(HOST_VALUE, HOST_PTR, offsetof(ialuPipe, sCycle)) ||
						!m_code.EmitLdrImm12(HOST_TEMP, HOST_PTR, offsetof(ialuPipe, sCycle) + 4) ||
						!m_code.EmitSubReg(HOST_CALL_SCRATCH, HOST_STALL_CYCLE_LO, HOST_VALUE, true) ||
						!m_code.EmitSbcReg(HOST_TEMP, HOST_STALL_CYCLE_HI, HOST_TEMP, true) ||
						!m_code.EmitCmpImm32(HOST_TEMP, 0))
					{
						return false;
					}
					const size_t skip_elapsed_high = m_code.EmitBranchPlaceholder(Condition::NE);
					if (skip_elapsed_high == static_cast<size_t>(-1))
						return false;

					if (!m_code.EmitLdrImm12(HOST_TEMP, HOST_PTR, offsetof(ialuPipe, Cycle)) ||
						!m_code.EmitCmpReg(HOST_CALL_SCRATCH, HOST_TEMP))
					{
						return false;
					}
					const size_t skip_elapsed_low = m_code.EmitBranchPlaceholder(Condition::CS);
					if (skip_elapsed_low == static_cast<size_t>(-1))
						return false;

					if (!m_code.EmitLdrImm12(HOST_CALL_SCRATCH, HOST_PTR, offsetof(ialuPipe, reg)) ||
						!EmitAndRegImm32(HOST_CALL_SCRATCH, HOST_CALL_SCRATCH, regs.VIread, HOST_TEMP) ||
						!m_code.EmitCmpImm32(HOST_CALL_SCRATCH, 0))
					{
						return false;
					}
					const size_t skip_no_match = m_code.EmitBranchPlaceholder(Condition::EQ);
					if (skip_no_match == static_cast<size_t>(-1))
						return false;

					if (!m_code.EmitLdrImm12(HOST_TEMP, HOST_PTR, offsetof(ialuPipe, Cycle)) ||
						!m_code.EmitAddReg(HOST_VALUE, HOST_VALUE, HOST_TEMP, true) ||
						!m_code.EmitLdrImm12(HOST_TEMP, HOST_PTR, offsetof(ialuPipe, sCycle) + 4) ||
						!m_code.EmitAdcImm8(HOST_TEMP, HOST_TEMP, 0) ||
						!m_code.EmitStrImm12(HOST_VALUE, HOST_VU, VuOffset(offsetof(VURegs, cycle))) ||
						!m_code.EmitStrImm12(HOST_TEMP, HOST_VU, VuOffset(offsetof(VURegs, cycle) + 4)) ||
						!EmitMovReg(HOST_STALL_CYCLE_LO, HOST_VALUE) ||
						!EmitMovReg(HOST_STALL_CYCLE_HI, HOST_TEMP))
					{
						return false;
					}

					const size_t advance_index = m_code.Size();
					if (!m_code.PatchBranch(skip_elapsed_high, advance_index, Condition::NE) ||
						!m_code.PatchBranch(skip_elapsed_low, advance_index, Condition::CS) ||
						!m_code.PatchBranch(skip_no_match, advance_index, Condition::EQ) ||
						!m_code.EmitAddImm8(HOST_INDEX, HOST_INDEX, 1) ||
						!m_code.EmitAndImm32(HOST_INDEX, HOST_INDEX, 3))
					{
						return false;
					}
				}

				const size_t done = m_code.Size();
				for (size_t jump : done_jumps)
				{
					if (!m_code.PatchBranch(jump, done, Condition::LS))
						return false;
				}

#if defined(VITASX2_QEMU_VALIDATION)
				return EmitQemuLowerBranchStallTestInlineCounter();
#else
				return true;
#endif
			}

			bool EmitInlineFmacStallTestReads(unsigned vf_reg0, unsigned xyzw0,
				unsigned vf_reg1, unsigned xyzw1, bool resident_cycle_pair)
			{
				if (vf_reg0 == 0 || xyzw0 == 0)
				{
					vf_reg0 = vf_reg1;
					xyzw0 = xyzw1;
					vf_reg1 = 0;
					xyzw1 = 0;
				}
				if (vf_reg1 == vf_reg0)
				{
					xyzw0 |= xyzw1;
					vf_reg1 = 0;
					xyzw1 = 0;
				}
				if (vf_reg1 == 0 || xyzw1 == 0)
				{
					vf_reg1 = 0;
					xyzw1 = 0;
				}
				if (vf_reg0 == 0 || xyzw0 == 0)
					return true;

				// PCSX2 owners: microVU_Analyze.inl::analyzeReg1(), where both
				// source hazards contribute to one maximum mVUstall, and
				// VUops.cpp::_vuTestFMACStalls(). The interpreter expresses two
				// sources as two queue walks, but their only result is the maximum
				// pending ready cycle. Scan once and match either dependency. A
				// ready entry can be ignored and `_vuTestPipes()` still owns
				// flag/VF visibility after the stall cycle is applied.
				constexpr unsigned HOST_PTR = 1;
				constexpr unsigned HOST_VALUE = 2;
				constexpr unsigned HOST_TEMP = 3;
				// Full resident-cycle blocks already own the exact current cycle in
				// r5:r9. The scan has no externally observable seam, so consume and
				// advance that pair directly. Other blocks retain the canonical
				// VURegs::cycle contract and use the CLIP-backup scratch pair.
				const unsigned HOST_STALL_CYCLE_LO =
					resident_cycle_pair ? HOST_CYCLE_LO : HOST_CLIP_OLD;
				const unsigned HOST_STALL_CYCLE_HI =
					resident_cycle_pair ? HOST_CYCLE_HI : HOST_CLIP_NEW;
				// Preserve r10 when it owns the resident pipe aggregate. LR is saved by
				// the generated-block prologue and this inline scan contains no call.
				const unsigned HOST_COUNT = UsesResidentPipeActivity() ? 14u :
					HOST_STALL_SCRATCH;
				constexpr size_t FMAC_ARRAY_OFFSET = offsetof(VURegs, fmac);
				constexpr size_t ring_bytes = 4 * sizeof(fmacPipe);
				static_assert(sizeof(fmacPipe) == 48);
				static_assert(ring_bytes <= 255);
				static_assert(FMAC_ARRAY_OFFSET + offsetof(fmacPipe, sCycle) + 4 < 4096);

				// Every producer appends the current nondecreasing VU cycle and the
				// Sony VU manual fixes every FMAC latency at four cycles. Walk from
				// newest to oldest: the first matching writer is therefore the
				// maximum required stall, while the first elapsed entry proves every
				// older entry elapsed. This preserves _vuFMACTestStall()'s exact
				// result while terminating either case immediately. Form VU +
				// index*48 in two shifted ADDs and fold the fixed array offset into
				// each LDR.
				// Resident r10 already contains the exact canonical count in bits
				// [2:0]. Extract it into scratch LR with ANDS so the empty branch
				// consumes Z directly; preserve r10's complete pipe aggregate.
				if (!(UsesResidentPipeActivity() ?
						m_code.EmitAndImm32(HOST_COUNT, HOST_STALL_SCRATCH,
							RESIDENT_FMAC_COUNT_MASK, true) :
						(m_code.EmitLdrImm12(HOST_COUNT, HOST_VU,
							 VuOffset(offsetof(VURegs, fmaccount))) &&
						 m_code.EmitCmpImm32(HOST_COUNT, 0))))
				{
					return false;
				}
				const size_t empty_jump = m_code.EmitBranchPlaceholder(Condition::EQ);
				if (empty_jump == static_cast<size_t>(-1) ||
					(!resident_cycle_pair &&
						(!m_code.EmitLdrImm12(HOST_STALL_CYCLE_LO, HOST_VU,
							VuOffset(offsetof(VURegs, cycle))) ||
						 !m_code.EmitLdrImm12(HOST_STALL_CYCLE_HI, HOST_VU,
							 VuOffset(offsetof(VURegs, cycle) + 4)))) ||
					!m_code.EmitLdrImm12(HOST_CALL_SCRATCH, HOST_VU,
						VuOffset(offsetof(VURegs, fmacwritepos))) ||
					!m_code.EmitSubImm8(HOST_CALL_SCRATCH, HOST_CALL_SCRATCH, 1) ||
					!m_code.EmitAndImm32(HOST_CALL_SCRATCH, HOST_CALL_SCRATCH, 3) ||
					!m_code.EmitAddRegShiftImm(HOST_PTR, HOST_CALL_SCRATCH,
						HOST_CALL_SCRATCH, ShiftType::LSL, 1) ||
					!m_code.EmitAddRegShiftImm(HOST_PTR, HOST_VU, HOST_PTR,
						ShiftType::LSL, 4))
				{
					return false;
				}

				const size_t loop_start = m_code.Size();
				// r8/r12 are not yet occupied by this pair's optional CLIP
				// backup, so a cycle-resident scan retains the exact issue
				// timestamp there. Keep the common elapsed test instruction-for-
				// instruction identical to PCSX2's unsigned
				// (current - sCycle) >= 4 operation. On a real dependency those
				// same retained words then form sCycle + 4, removing the old
				// second pair of Cortex-A9 data-cache loads.
				const unsigned HOST_ISSUE_LO = resident_cycle_pair ?
					HOST_CLIP_OLD : HOST_VALUE;
				const unsigned HOST_ISSUE_HI = resident_cycle_pair ?
					HOST_CALL_SCRATCH : HOST_TEMP;
				if (!m_code.EmitLdrImm12(HOST_ISSUE_LO, HOST_PTR,
						VuOffset(FMAC_ARRAY_OFFSET + offsetof(fmacPipe, sCycle))) ||
					!m_code.EmitLdrImm12(HOST_ISSUE_HI, HOST_PTR,
						VuOffset(FMAC_ARRAY_OFFSET + offsetof(fmacPipe, sCycle) + 4)) ||
					!m_code.EmitSubReg(HOST_VALUE, HOST_STALL_CYCLE_LO,
						HOST_ISSUE_LO, true) ||
					!m_code.EmitSbcReg(HOST_TEMP, HOST_STALL_CYCLE_HI,
						HOST_ISSUE_HI, true) ||
					!m_code.EmitCmpImm32(HOST_TEMP, 0))
				{
					return false;
				}
				const size_t done_elapsed_high =
					m_code.EmitBranchPlaceholder(Condition::NE);
				if (done_elapsed_high == static_cast<size_t>(-1) ||
					!m_code.EmitCmpImm32(HOST_VALUE, FMAC_PIPELINE_LATENCY_CYCLES))
				{
					return false;
				}
				const size_t done_elapsed_low =
					m_code.EmitBranchPlaceholder(Condition::CS);
				if (done_elapsed_low == static_cast<size_t>(-1))
					return false;

				std::array<size_t, 4> matched_jumps{};
				u32 matched_count = 0;
				// Coalescing above guarantees distinct nonzero source registers.
				// Equal lane masks make the exact predicate
				//   (writer == source0 || writer == source1) && writer_lanes & lanes.
				// A32 conditional execution preserves Z when the first compare
				// succeeds and otherwise performs CMPNE against source1. Test the
				// shared lane mask once instead of duplicating the complete mask arm.
				const bool shared_source_lanes = vf_reg1 != 0 && xyzw1 == xyzw0;
				if (shared_source_lanes)
				{
					if (!m_code.EmitLdrImm12(HOST_VALUE, HOST_PTR,
							VuOffset(FMAC_ARRAY_OFFSET + offsetof(fmacPipe, regupper))) ||
						!m_code.EmitCmpImm32(HOST_VALUE, vf_reg0) ||
						!m_code.EmitCmpImm32(HOST_VALUE, vf_reg1, Condition::NE))
					{
						return false;
					}
					const size_t skip_upper_no_reg =
						m_code.EmitBranchPlaceholder(Condition::NE);
					if (skip_upper_no_reg == static_cast<size_t>(-1) ||
						!m_code.EmitLdrImm12(HOST_TEMP, HOST_PTR,
							VuOffset(FMAC_ARRAY_OFFSET + offsetof(fmacPipe, xyzwupper))) ||
						!m_code.EmitTstImm32(HOST_TEMP, xyzw0))
					{
						return false;
					}
					matched_jumps[matched_count++] = m_code.EmitBranchPlaceholder(Condition::NE);
					if (matched_jumps[matched_count - 1] == static_cast<size_t>(-1) ||
						!m_code.PatchBranch(skip_upper_no_reg, m_code.Size(), Condition::NE))
					{
						return false;
					}
				}
				else
				{
					if (!m_code.EmitLdrImm12(HOST_VALUE, HOST_PTR,
							VuOffset(FMAC_ARRAY_OFFSET + offsetof(fmacPipe, regupper))) ||
						!m_code.EmitCmpImm32(HOST_VALUE, vf_reg0))
					{
						return false;
					}
					const size_t check_upper1 =
						m_code.EmitBranchPlaceholder(Condition::NE);
					if (check_upper1 == static_cast<size_t>(-1) ||
						!m_code.EmitLdrImm12(HOST_TEMP, HOST_PTR,
							VuOffset(FMAC_ARRAY_OFFSET + offsetof(fmacPipe, xyzwupper))) ||
						!m_code.EmitTstImm32(HOST_TEMP, xyzw0))
					{
						return false;
					}
					matched_jumps[matched_count++] =
						m_code.EmitBranchPlaceholder(Condition::NE);
					if (matched_jumps[matched_count - 1] == static_cast<size_t>(-1) ||
						!m_code.PatchBranch(check_upper1, m_code.Size(), Condition::NE))
					{
						return false;
					}
					if (vf_reg1 != 0)
					{
						if (!m_code.EmitCmpImm32(HOST_VALUE, vf_reg1))
							return false;
						const size_t check_lower =
							m_code.EmitBranchPlaceholder(Condition::NE);
						if (check_lower == static_cast<size_t>(-1) ||
							!m_code.EmitLdrImm12(HOST_TEMP, HOST_PTR,
								VuOffset(FMAC_ARRAY_OFFSET + offsetof(fmacPipe, xyzwupper))) ||
							!m_code.EmitTstImm32(HOST_TEMP, xyzw1))
						{
							return false;
						}
						matched_jumps[matched_count++] =
							m_code.EmitBranchPlaceholder(Condition::NE);
						if (matched_jumps[matched_count - 1] == static_cast<size_t>(-1) ||
							!m_code.PatchBranch(check_lower, m_code.Size(), Condition::NE))
						{
							return false;
						}
					}
				}

				size_t skip_no_lower_reg = static_cast<size_t>(-1);
				if (shared_source_lanes)
				{
					if (!m_code.EmitLdrImm12(HOST_VALUE, HOST_PTR,
							VuOffset(FMAC_ARRAY_OFFSET + offsetof(fmacPipe, reglower))) ||
						!m_code.EmitCmpImm32(HOST_VALUE, vf_reg0) ||
						!m_code.EmitCmpImm32(HOST_VALUE, vf_reg1, Condition::NE))
					{
						return false;
					}
					skip_no_lower_reg = m_code.EmitBranchPlaceholder(Condition::NE);
					if (skip_no_lower_reg == static_cast<size_t>(-1) ||
						!m_code.EmitLdrImm12(HOST_TEMP, HOST_PTR,
							VuOffset(FMAC_ARRAY_OFFSET + offsetof(fmacPipe, xyzwlower))) ||
						!m_code.EmitTstImm32(HOST_TEMP, xyzw0))
					{
						return false;
					}
					matched_jumps[matched_count++] =
						m_code.EmitBranchPlaceholder(Condition::NE);
					if (matched_jumps[matched_count - 1] == static_cast<size_t>(-1))
						return false;
				}
				else
				{
					if (!m_code.EmitLdrImm12(HOST_VALUE, HOST_PTR,
							VuOffset(FMAC_ARRAY_OFFSET + offsetof(fmacPipe, reglower))) ||
						!m_code.EmitCmpImm32(HOST_VALUE, vf_reg0))
					{
						return false;
					}
					const size_t check_lower1 =
						m_code.EmitBranchPlaceholder(Condition::NE);
					if (check_lower1 == static_cast<size_t>(-1) ||
						!m_code.EmitLdrImm12(HOST_TEMP, HOST_PTR,
							VuOffset(FMAC_ARRAY_OFFSET + offsetof(fmacPipe, xyzwlower))) ||
						!m_code.EmitTstImm32(HOST_TEMP, xyzw0))
					{
						return false;
					}
					matched_jumps[matched_count++] =
						m_code.EmitBranchPlaceholder(Condition::NE);
					if (matched_jumps[matched_count - 1] == static_cast<size_t>(-1) ||
						!m_code.PatchBranch(check_lower1, m_code.Size(), Condition::NE))
					{
						return false;
					}

					if (vf_reg1 != 0)
					{
						if (!m_code.EmitCmpImm32(HOST_VALUE, vf_reg1))
							return false;
						skip_no_lower_reg = m_code.EmitBranchPlaceholder(Condition::NE);
						if (skip_no_lower_reg == static_cast<size_t>(-1) ||
							!m_code.EmitLdrImm12(HOST_TEMP, HOST_PTR,
								VuOffset(FMAC_ARRAY_OFFSET + offsetof(fmacPipe, xyzwlower))) ||
							!m_code.EmitTstImm32(HOST_TEMP, xyzw1))
						{
							return false;
						}
						matched_jumps[matched_count++] =
							m_code.EmitBranchPlaceholder(Condition::NE);
						if (matched_jumps[matched_count - 1] == static_cast<size_t>(-1))
							return false;
					}
				}

				const size_t advance_entry = m_code.Size();
				if ((vf_reg1 != 0 &&
						!m_code.PatchBranch(skip_no_lower_reg, advance_entry, Condition::NE)) ||
					!m_code.EmitSubImm8(HOST_COUNT, HOST_COUNT, 1, true))
				{
					return false;
				}
				const size_t final_jump = m_code.EmitBranchPlaceholder(Condition::EQ);
				if (final_jump == static_cast<size_t>(-1) ||
					!m_code.EmitCmpReg(HOST_PTR, HOST_VU) ||
					!m_code.EmitSubImm8(HOST_PTR, HOST_PTR, sizeof(fmacPipe), false,
						Condition::NE) ||
					!m_code.EmitAddImm8(HOST_PTR, HOST_VU,
						3 * sizeof(fmacPipe), false, Condition::EQ))
				{
					return false;
				}
				const size_t loop_jump = m_code.EmitBranchPlaceholder();
				if (loop_jump == static_cast<size_t>(-1) ||
					!m_code.PatchBranch(loop_jump, loop_start))
				{
					return false;
				}

				const size_t match_target = m_code.Size();
				for (u32 i = 0; i < matched_count; i++)
				{
					if (!m_code.PatchBranch(matched_jumps[i], match_target, Condition::NE))
						return false;
				}
				if (resident_cycle_pair ?
					(!m_code.EmitAddImm8(HOST_CLIP_OLD, HOST_CLIP_OLD,
						FMAC_PIPELINE_LATENCY_CYCLES, true) ||
					 !m_code.EmitAdcImm8(HOST_CALL_SCRATCH, HOST_CALL_SCRATCH, 0) ||
					 !EmitMovReg(HOST_STALL_CYCLE_LO, HOST_CLIP_OLD) ||
					 !EmitMovReg(HOST_STALL_CYCLE_HI, HOST_CALL_SCRATCH)) :
					(!m_code.EmitLdrImm12(HOST_VALUE, HOST_PTR,
						VuOffset(FMAC_ARRAY_OFFSET + offsetof(fmacPipe, sCycle))) ||
					 !m_code.EmitAddImm8(HOST_VALUE, HOST_VALUE,
						FMAC_PIPELINE_LATENCY_CYCLES, true) ||
					 !m_code.EmitLdrImm12(HOST_TEMP, HOST_PTR,
						VuOffset(FMAC_ARRAY_OFFSET + offsetof(fmacPipe, sCycle) + 4)) ||
					 !m_code.EmitAdcImm8(HOST_TEMP, HOST_TEMP, 0) ||
						(!m_code.EmitStrImm12(HOST_VALUE, HOST_VU,
							VuOffset(offsetof(VURegs, cycle))) ||
						 !m_code.EmitStrImm12(HOST_TEMP, HOST_VU,
							 VuOffset(offsetof(VURegs, cycle) + 4))) ||
					 !EmitMovReg(HOST_STALL_CYCLE_LO, HOST_VALUE) ||
					 !EmitMovReg(HOST_STALL_CYCLE_HI, HOST_TEMP)))
				{
					return false;
				}

				const size_t done_target = m_code.Size();
				return m_code.PatchBranch(empty_jump, done_target, Condition::EQ) &&
					m_code.PatchBranch(done_elapsed_high, done_target, Condition::NE) &&
					m_code.PatchBranch(done_elapsed_low, done_target, Condition::CS) &&
					m_code.PatchBranch(final_jump, done_target, Condition::EQ);
			}

			bool EmitInlineFmacStallTestBody(const _VURegsNum& regs,
				bool resident_cycle_pair = false)
			{
				return EmitInlineFmacStallTestReads(regs.VFread0, regs.VFr0xyzw,
					regs.VFread1, regs.VFr1xyzw, resident_cycle_pair);
			}

			bool EmitInlineFmacStallTest(const _VURegsNum& regs, bool upper,
				bool resident_cycle_pair)
			{
				if (!EmitInlineFmacStallTestBody(regs, resident_cycle_pair))
					return false;

#if defined(VITASX2_QEMU_VALIDATION)
				return upper ? EmitQemuUpperFmacStallTestInlineCounter() :
					EmitQemuLowerFmacStallTestInlineCounter();
#else
				return true;
#endif
			}

			bool EmitInlineFmacStallTestPreservingCycleResidency(
				const _VURegsNum& regs, bool upper)
			{
				// Canonical queue state remains in VURegs, but a full resident block
				// has an exact r5:r9 cycle pair. No helper or dispatcher can observe
				// cycle during this generated scan; publisher seams already materialize
				// the pair. Avoid six cycle loads/stores at each direct scan site.
				if (m_resident_cycle_high)
					return EmitInlineFmacStallTest(regs, upper, true);

				return EmitPublishResidentCycle() &&
					EmitInlineFmacStallTest(regs, upper, false) &&
					EmitResyncResidentCycle();
			}

			bool EmitStoreCycleIfNewer(unsigned new_lo, unsigned new_hi,
				unsigned current_lo, unsigned current_hi)
			{
				if (!m_code.EmitLdrImm12(current_lo, HOST_VU, VuOffset(offsetof(VURegs, cycle))) ||
					!m_code.EmitLdrImm12(current_hi, HOST_VU, VuOffset(offsetof(VURegs, cycle) + 4)) ||
					!m_code.EmitCmpReg(current_hi, new_hi))
				{
					return false;
				}

				const size_t update_from_high = m_code.EmitBranchPlaceholder(Condition::CC);
				const size_t done_from_high = m_code.EmitBranchPlaceholder(Condition::NE);
				if (update_from_high == static_cast<size_t>(-1) ||
					done_from_high == static_cast<size_t>(-1) ||
					!m_code.EmitCmpReg(current_lo, new_lo))
				{
					return false;
				}
				const size_t done_from_low = m_code.EmitBranchPlaceholder(Condition::CS);
				if (done_from_low == static_cast<size_t>(-1))
					return false;

				const size_t update_target = m_code.Size();
				if (!m_code.PatchBranch(update_from_high, update_target, Condition::CC) ||
					!m_code.EmitStrImm12(new_lo, HOST_VU, VuOffset(offsetof(VURegs, cycle))) ||
					!m_code.EmitStrImm12(new_hi, HOST_VU, VuOffset(offsetof(VURegs, cycle) + 4)))
				{
					return false;
				}
				const size_t done_after_update = m_code.EmitBranchPlaceholder();
				if (done_after_update == static_cast<size_t>(-1))
					return false;

				const size_t done_target = m_code.Size();
				return m_code.PatchBranch(done_from_high, done_target, Condition::NE) &&
					m_code.PatchBranch(done_from_low, done_target, Condition::CS) &&
					m_code.PatchBranch(done_after_update, done_target);
			}

			bool EmitStoreLocalFmacCycleIfNewer(unsigned new_lo, unsigned current_lo,
				unsigned delta, unsigned high_scratch)
			{
				// Sony VU User Manual 3.4.4 fixes FMAC latency at four cycles.
				// PCSX2's microVU pipeline likewise compares the issue cycle plus four.
				// For that bounded distance, signed subtraction of the low words is an
				// exact modulo-2^32 ordering test, including a low-word wrap. Only the
				// taken wrap edge increments the high word, resident or canonical.
				if (!(m_resident_cycle ?
						(current_lo == HOST_CYCLE_LO || EmitMovReg(current_lo, HOST_CYCLE_LO)) :
						m_code.EmitLdrImm12(current_lo, HOST_VU,
							VuOffset(offsetof(VURegs, cycle)))) ||
					!m_code.EmitSubReg(delta, new_lo, current_lo, true))
				{
					return false;
				}
				const size_t done = m_code.EmitBranchPlaceholder(Condition::LE);
				if (done == static_cast<size_t>(-1) ||
					!m_code.EmitCmpReg(new_lo, current_lo))
				{
					return false;
				}
				const size_t no_wrap = m_code.EmitBranchPlaceholder(Condition::CS);
				if (no_wrap == static_cast<size_t>(-1))
					return false;
				const bool high_updated = m_resident_cycle_high ?
					m_code.EmitAddImm8(HOST_CYCLE_HI, HOST_CYCLE_HI, 1) :
					(m_code.EmitLdrImm12(high_scratch, HOST_VU,
							VuOffset(offsetof(VURegs, cycle) + 4)) &&
					 m_code.EmitAddImm8(high_scratch, high_scratch, 1) &&
					 m_code.EmitStrImm12(high_scratch, HOST_VU,
							VuOffset(offsetof(VURegs, cycle) + 4)));
				if (!high_updated ||
					!m_code.PatchBranch(no_wrap, m_code.Size(), Condition::CS) ||
					!(m_resident_cycle ? EmitMovReg(HOST_CYCLE_LO, new_lo) :
						m_code.EmitStrImm12(new_lo, HOST_VU,
							VuOffset(offsetof(VURegs, cycle)))))
				{
					return false;
				}
				return m_code.PatchBranch(done, m_code.Size(), Condition::LE);
			}

			bool EmitInlineFlushAllFdiv()
			{
				// PCSX2 owner: VUops.cpp::_vuFlushAll() FDIV section. E-bit
				// completion publishes Q/status regardless of readiness, then
				// advances VU->cycle to fdiv.sCycle + fdiv.Cycle if needed.
				constexpr size_t base = offsetof(VURegs, fdiv);
				if (!m_code.EmitLdrImm12(0, HOST_VU, VuOffset(base + offsetof(fdivPipe, enable))) ||
					!m_code.EmitCmpImm32(0, 0))
				{
					return false;
				}
				const size_t done_disabled = m_code.EmitBranchPlaceholder(Condition::EQ);
				if (done_disabled == static_cast<size_t>(-1))
					return false;

				if (!m_code.EmitMovImm8(0, 0) ||
					!m_code.EmitStrImm12(0, HOST_VU, VuOffset(base + offsetof(fdivPipe, enable))) ||
					!m_code.EmitLdrImm12(0, HOST_VU, VuOffset(base + offsetof(fdivPipe, reg))) ||
					!m_code.EmitStrImm12(0, HOST_VU, ViOffset(REG_Q)) ||
					!m_code.EmitLdrImm12(0, HOST_VU, ViOffset(REG_STATUS_FLAG)) ||
					!EmitAndRegImm32(0, 0, 0x0fcfu, HOST_CALL_SCRATCH) ||
					!m_code.EmitLdrImm12(1, HOST_VU, VuOffset(base + offsetof(fdivPipe, statusflag))) ||
					!EmitAndRegImm32(1, 1, 0x0c30u, HOST_CALL_SCRATCH) ||
					!m_code.EmitOrrReg(0, 0, 1) ||
					!m_code.EmitStrImm12(0, HOST_VU, ViOffset(REG_STATUS_FLAG)) ||
					!m_code.EmitLdrImm12(0, HOST_VU, VuOffset(base + offsetof(fdivPipe, sCycle))) ||
					!m_code.EmitLdrImm12(1, HOST_VU, VuOffset(base + offsetof(fdivPipe, sCycle) + 4)) ||
					!m_code.EmitLdrImm12(2, HOST_VU, VuOffset(base + offsetof(fdivPipe, Cycle))) ||
					!m_code.EmitAddReg(0, 0, 2, true) ||
					!m_code.EmitAdcImm8(1, 1, 0) ||
					!EmitStoreCycleIfNewer(0, 1, 2, 3))
				{
					return false;
				}

				return m_code.PatchBranch(done_disabled, m_code.Size(), Condition::EQ);
			}

			bool EmitInlineFlushAllEfu()
			{
				// PCSX2 owner: VUops.cpp::_vuFlushAll() EFU section. E-bit
				// completion publishes P regardless of readiness, then advances
				// VU->cycle to efu.sCycle + efu.Cycle if needed.
				constexpr size_t base = offsetof(VURegs, efu);
				if (!m_code.EmitLdrImm12(0, HOST_VU, VuOffset(base + offsetof(efuPipe, enable))) ||
					!m_code.EmitCmpImm32(0, 0))
				{
					return false;
				}
				const size_t done_disabled = m_code.EmitBranchPlaceholder(Condition::EQ);
				if (done_disabled == static_cast<size_t>(-1))
					return false;

				if (!m_code.EmitMovImm8(0, 0) ||
					!m_code.EmitStrImm12(0, HOST_VU, VuOffset(base + offsetof(efuPipe, enable))) ||
					!m_code.EmitLdrImm12(0, HOST_VU, VuOffset(base + offsetof(efuPipe, reg))) ||
					!m_code.EmitStrImm12(0, HOST_VU, ViOffset(REG_P)) ||
					!m_code.EmitLdrImm12(0, HOST_VU, VuOffset(base + offsetof(efuPipe, sCycle))) ||
					!m_code.EmitLdrImm12(1, HOST_VU, VuOffset(base + offsetof(efuPipe, sCycle) + 4)) ||
					!m_code.EmitLdrImm12(2, HOST_VU, VuOffset(base + offsetof(efuPipe, Cycle))) ||
					!m_code.EmitAddReg(0, 0, 2, true) ||
					!m_code.EmitAdcImm8(1, 1, 0) ||
					!EmitStoreCycleIfNewer(0, 1, 2, 3))
				{
					return false;
				}

				return m_code.PatchBranch(done_disabled, m_code.Size(), Condition::EQ);
			}

			bool EmitInlineFlushAllFmac()
			{
				// PCSX2 owner: VUops.cpp::_vuFlushAll() FMAC loop. Unlike the
				// per-pair _vuFMACflush() path this consumes every queued entry,
				// publishing flags in queue order and waiting out unfinished pipes.
				constexpr unsigned HOST_INDEX = 0;
				constexpr unsigned HOST_PTR = 1;
				constexpr unsigned HOST_TEMP = 2;
				constexpr unsigned HOST_VALUE = 3;
				constexpr unsigned HOST_COUNT = HOST_CALL_SCRATCH;

				const size_t loop_start = m_code.Size();
				if (!m_code.EmitLdrImm12(HOST_COUNT, HOST_VU, VuOffset(offsetof(VURegs, fmaccount))) ||
					!m_code.EmitCmpImm32(HOST_COUNT, 0))
				{
					return false;
				}
				const size_t done_empty = m_code.EmitBranchPlaceholder(Condition::EQ);
				if (done_empty == static_cast<size_t>(-1))
					return false;

				if (!m_code.EmitLdrImm12(HOST_INDEX, HOST_VU, VuOffset(offsetof(VURegs, fmacreadpos))) ||
					!m_code.EmitAddImm32(HOST_PTR, HOST_VU, offsetof(VURegs, fmac)) ||
					!m_code.EmitAddRegShiftImm(HOST_PTR, HOST_PTR, HOST_INDEX, ShiftType::LSL, 5) ||
					!m_code.EmitAddRegShiftImm(HOST_PTR, HOST_PTR, HOST_INDEX, ShiftType::LSL, 4) ||
					!m_code.EmitLdrImm12(HOST_TEMP, HOST_PTR, offsetof(fmacPipe, flagreg)) ||
					!m_code.EmitTstImm32(HOST_TEMP, 1u << REG_CLIP_FLAG))
				{
					return false;
				}
				const size_t skip_clip = m_code.EmitBranchPlaceholder(Condition::EQ);
				if (skip_clip == static_cast<size_t>(-1))
					return false;
				if (!m_code.EmitLdrImm12(HOST_VALUE, HOST_PTR, offsetof(fmacPipe, clipflag)) ||
					!m_code.EmitStrImm12(HOST_VALUE, HOST_VU, ViOffset(REG_CLIP_FLAG)) ||
					!m_code.PatchBranch(skip_clip, m_code.Size(), Condition::EQ))
				{
					return false;
				}

				if (!m_code.EmitTstImm32(HOST_TEMP, 1u << REG_STATUS_FLAG))
					return false;
				const size_t no_sticky_status = m_code.EmitBranchPlaceholder(Condition::EQ);
				if (no_sticky_status == static_cast<size_t>(-1))
					return false;

				if (!m_code.EmitLdrImm12(HOST_VALUE, HOST_VU, ViOffset(REG_STATUS_FLAG)) ||
					!EmitAndRegImm32(HOST_VALUE, HOST_VALUE, 0x30u, HOST_COUNT) ||
					!m_code.EmitLdrImm12(HOST_TEMP, HOST_PTR, offsetof(fmacPipe, statusflag)) ||
					!EmitAndRegImm32(HOST_INDEX, HOST_TEMP, 0x0fc0u, HOST_COUNT) ||
					!m_code.EmitOrrReg(HOST_VALUE, HOST_VALUE, HOST_INDEX) ||
					!EmitAndRegImm32(HOST_INDEX, HOST_TEMP, 0x0fu, HOST_COUNT) ||
					!m_code.EmitOrrReg(HOST_VALUE, HOST_VALUE, HOST_INDEX) ||
					!m_code.EmitStrImm12(HOST_VALUE, HOST_VU, ViOffset(REG_STATUS_FLAG)))
				{
					return false;
				}
				const size_t status_done = m_code.EmitBranchPlaceholder();
				if (status_done == static_cast<size_t>(-1))
					return false;

				const size_t no_sticky_target = m_code.Size();
				if (!m_code.PatchBranch(no_sticky_status, no_sticky_target, Condition::EQ) ||
					!m_code.EmitLdrImm12(HOST_VALUE, HOST_VU, ViOffset(REG_STATUS_FLAG)) ||
					!EmitAndRegImm32(HOST_VALUE, HOST_VALUE, 0x0ff0u, HOST_COUNT) ||
					!m_code.EmitLdrImm12(HOST_TEMP, HOST_PTR, offsetof(fmacPipe, statusflag)) ||
					!EmitAndRegImm32(HOST_INDEX, HOST_TEMP, 0x0fu, HOST_COUNT) ||
					!m_code.EmitOrrReg(HOST_VALUE, HOST_VALUE, HOST_INDEX) ||
					!m_code.EmitOrrRegShiftImm(HOST_VALUE, HOST_VALUE, HOST_INDEX, ShiftType::LSL, 6) ||
					!m_code.EmitStrImm12(HOST_VALUE, HOST_VU, ViOffset(REG_STATUS_FLAG)))
				{
					return false;
				}

				const size_t after_status = m_code.Size();
				if (!m_code.PatchBranch(status_done, after_status) ||
					!m_code.EmitLdrImm12(HOST_VALUE, HOST_PTR, offsetof(fmacPipe, macflag)) ||
					!m_code.EmitStrImm12(HOST_VALUE, HOST_VU, ViOffset(REG_MAC_FLAG)) ||
					!m_code.EmitLdrImm12(HOST_INDEX, HOST_VU, VuOffset(offsetof(VURegs, fmacreadpos))) ||
					!m_code.EmitAddImm8(HOST_INDEX, HOST_INDEX, 1) ||
					!m_code.EmitAndImm32(HOST_INDEX, HOST_INDEX, 3) ||
					!m_code.EmitStrImm12(HOST_INDEX, HOST_VU, VuOffset(offsetof(VURegs, fmacreadpos))) ||
					!m_code.EmitLdrImm12(HOST_VALUE, HOST_PTR, offsetof(fmacPipe, sCycle)) ||
					!m_code.EmitLdrImm12(HOST_PTR, HOST_PTR, offsetof(fmacPipe, sCycle) + 4) ||
					!m_code.EmitAddImm8(HOST_VALUE, HOST_VALUE,
						FMAC_PIPELINE_LATENCY_CYCLES, true) ||
					!m_code.EmitAdcImm8(HOST_PTR, HOST_PTR, 0) ||
					!EmitStoreCycleIfNewer(HOST_VALUE, HOST_PTR, HOST_TEMP, HOST_COUNT) ||
					!m_code.EmitLdrImm12(HOST_COUNT, HOST_VU, VuOffset(offsetof(VURegs, fmaccount))) ||
					!m_code.EmitSubImm8(HOST_COUNT, HOST_COUNT, 1) ||
					!m_code.EmitStrImm12(HOST_COUNT, HOST_VU, VuOffset(offsetof(VURegs, fmaccount))))
				{
					return false;
				}

				const size_t loop_jump = m_code.EmitBranchPlaceholder();
				if (loop_jump == static_cast<size_t>(-1) ||
					!m_code.PatchBranch(loop_jump, loop_start))
				{
					return false;
				}

				return m_code.PatchBranch(done_empty, m_code.Size(), Condition::EQ);
			}

			bool EmitInlineFlushAllIalu()
			{
				// PCSX2 owner: VUops.cpp::_vuFlushAll() IALU loop. E-bit
				// completion consumes all queued VI writes and waits out their
				// cycles, but does not publish a value here.
				constexpr unsigned HOST_INDEX = 0;
				constexpr unsigned HOST_PTR = 1;
				constexpr unsigned HOST_VALUE = 2;
				constexpr unsigned HOST_TEMP = 3;
				constexpr unsigned HOST_COUNT = HOST_CALL_SCRATCH;

				const size_t loop_start = m_code.Size();
				if (!m_code.EmitLdrImm12(HOST_COUNT, HOST_VU, VuOffset(offsetof(VURegs, ialucount))) ||
					!m_code.EmitCmpImm32(HOST_COUNT, 0))
				{
					return false;
				}
				const size_t done_empty = m_code.EmitBranchPlaceholder(Condition::EQ);
				if (done_empty == static_cast<size_t>(-1))
					return false;

				if (!m_code.EmitLdrImm12(HOST_INDEX, HOST_VU, VuOffset(offsetof(VURegs, ialureadpos))) ||
					!m_code.EmitAddImm32(HOST_PTR, HOST_VU, offsetof(VURegs, ialu)) ||
					!m_code.EmitAddRegShiftImm(HOST_PTR, HOST_PTR, HOST_INDEX, ShiftType::LSL, 4) ||
					!m_code.EmitAddRegShiftImm(HOST_PTR, HOST_PTR, HOST_INDEX, ShiftType::LSL, 3) ||
					!m_code.EmitAddImm8(HOST_INDEX, HOST_INDEX, 1) ||
					!m_code.EmitAndImm32(HOST_INDEX, HOST_INDEX, 3) ||
					!m_code.EmitStrImm12(HOST_INDEX, HOST_VU, VuOffset(offsetof(VURegs, ialureadpos))) ||
					!m_code.EmitLdrImm12(HOST_VALUE, HOST_PTR, offsetof(ialuPipe, sCycle)) ||
					!m_code.EmitLdrImm12(HOST_TEMP, HOST_PTR, offsetof(ialuPipe, Cycle)) ||
					!m_code.EmitLdrImm12(HOST_PTR, HOST_PTR, offsetof(ialuPipe, sCycle) + 4) ||
					!m_code.EmitAddReg(HOST_VALUE, HOST_VALUE, HOST_TEMP, true) ||
					!m_code.EmitAdcImm8(HOST_PTR, HOST_PTR, 0) ||
					!EmitStoreCycleIfNewer(HOST_VALUE, HOST_PTR, HOST_TEMP, HOST_COUNT) ||
					!m_code.EmitLdrImm12(HOST_COUNT, HOST_VU, VuOffset(offsetof(VURegs, ialucount))) ||
					!m_code.EmitSubImm8(HOST_COUNT, HOST_COUNT, 1) ||
					!m_code.EmitStrImm12(HOST_COUNT, HOST_VU, VuOffset(offsetof(VURegs, ialucount))))
				{
					return false;
				}

				const size_t loop_jump = m_code.EmitBranchPlaceholder();
				if (loop_jump == static_cast<size_t>(-1) ||
					!m_code.PatchBranch(loop_jump, loop_start))
				{
					return false;
				}

				return m_code.PatchBranch(done_empty, m_code.Size(), Condition::EQ);
			}

			bool EmitInlineEbitFinish(bool publish_ebit = true)
			{
				// PCSX2 owners: x86/microVU_Branch.inl::mVUendProgram() and
				// mVUDTendProgram(), plus VUops.cpp::_vuFlushAll(). microVU clears
				// the VPU_STAT busy bit but, unlike the interpreter, deliberately
				// leaves VIF_STAT_VEW unchanged. VU1 also owns the XGKICK completion
				// side effects.
#if defined(VITASX2_QEMU_VALIDATION)
				if (!EmitQemuEbitFinishInlineCounter())
					return false;
#endif

				if (!m_code.EmitMovImm8(0, 0) ||
					!m_code.EmitStrbImm12(0, HOST_VU, VuOffset(VI_BACKUP_CYCLES_OFFSET)) ||
					!EmitInlineFlushAllFdiv() ||
					!EmitInlineFlushAllEfu() ||
					!EmitInlineFlushAllFmac() ||
					!EmitInlineFlushAllIalu())
				{
					return false;
				}

				if (m_vu0_memory_map || !THREAD_VU1)
				{
					const u32 vpu_stat_run_bit = m_vu0_memory_map ? 0x1u : 0x100u;
					if (!m_code.EmitMovImm32(3, static_cast<u32>(reinterpret_cast<uptr>(&VU0) + ViOffset(REG_VPU_STAT))) ||
						!m_code.EmitLdrImm12(0, 3, 0) ||
						!m_code.EmitBicImm32(0, 0, vpu_stat_run_bit) ||
						!m_code.EmitStrImm12(0, 3, 0))
					{
						return false;
					}
				}

				if (m_vu0_memory_map)
					return true;

				if (!m_code.EmitLdrImm12(0, HOST_VU, VuOffset(offsetof(VURegs, xgkickenable))) ||
					!m_code.EmitCmpImm32(0, 0))
				{
					return false;
				}
				const size_t skip_xgkick = m_code.EmitBranchPlaceholder(Condition::EQ);
				if (skip_xgkick == static_cast<size_t>(-1))
					return false;
				if (!EmitCallXgkickTransferFlush() ||
					!m_code.PatchBranch(skip_xgkick, m_code.Size(), Condition::EQ))
				{
					return false;
				}

				constexpr u32 SPEEDHACK_VU1_INSTANT_BIT = 1u << 5;
				constexpr size_t speedhack_bitset_offset =
					offsetof(Pcsx2Config, Speedhacks) + offsetof(Pcsx2Config::SpeedhackOptions, bitset);
				if (!m_code.EmitMovImm32(3, static_cast<u32>(reinterpret_cast<uptr>(&EmuConfig) + speedhack_bitset_offset)) ||
					!m_code.EmitLdrImm12(0, 3, 0) ||
					!m_code.EmitTstImm32(0, SPEEDHACK_VU1_INSTANT_BIT))
				{
					return false;
				}
				const size_t skip_instant = m_code.EmitBranchPlaceholder(Condition::EQ);
				if (skip_instant == static_cast<size_t>(-1))
					return false;

				if (!m_code.EmitMovImm32(3, static_cast<u32>(reinterpret_cast<uptr>(&cpuRegs) + offsetof(cpuRegisters, cycle))) ||
					!m_code.EmitLdrImm12(0, 3, 0) ||
					!m_code.EmitLdrImm12(1, 3, 4) ||
					!m_code.EmitStrImm12(0, HOST_VU, VuOffset(offsetof(VURegs, xgkicklastcycle))) ||
					!m_code.EmitStrImm12(1, HOST_VU, VuOffset(offsetof(VURegs, xgkicklastcycle) + 4)))
				{
					return false;
				}

				if (!m_code.PatchBranch(skip_instant, m_code.Size(), Condition::EQ))
					return false;

				if (THREAD_VU1)
				{
					return EmitCallAbsoluteClobberVectorState(publish_ebit ?
						reinterpret_cast<const void*>(&Vu1MtvuFinishEbitProgram) :
						reinterpret_cast<const void*>(&Vu1MtvuFinishDtProgram));
				}
				return true;
			}

			bool EmitInlineDtFlag(u32 fbrst_mask, u32 vpu_stat_bit, u8 intc_irq)
			{
				// PCSX2 owners: VU0microInterp.cpp::_vu0Exec() and
				// VU1microInterp.cpp::_vu1Exec() D/T flag handling. FBRST is a
				// runtime VU0 register; the D/T opcode bit is compile-time-known.
				const uptr fbrst_address = (!m_vu0_memory_map && THREAD_VU1) ?
					reinterpret_cast<uptr>(&vu1Thread.vuFBRST) :
					reinterpret_cast<uptr>(&VU0) + ViOffset(REG_FBRST);
				if (!m_code.EmitMovImm32(3, static_cast<u32>(fbrst_address)) ||
					!m_code.EmitLdrImm12(0, 3, 0) ||
					!m_code.EmitTstImm32(0, fbrst_mask))
				{
					return false;
				}
				const size_t skip = m_code.EmitBranchPlaceholder(Condition::EQ);
				if (skip == static_cast<size_t>(-1))
					return false;

				if (!m_vu0_memory_map && THREAD_VU1)
				{
					const void* mark_end = (vpu_stat_bit == 0x400u) ?
						reinterpret_cast<const void*>(&Vu1MtvuMarkTBitEnd) :
						reinterpret_cast<const void*>(&Vu1MtvuMarkDBitEnd);
					if (!EmitCallAbsoluteClobberVectorState(mark_end) ||
						!m_code.EmitMovImm8(0, 1) ||
						!m_code.EmitStrImm12(0, HOST_VU, VuOffset(offsetof(VURegs, ebit))))
					{
						return false;
					}
				}
				else if (!m_code.EmitMovImm32(3, static_cast<u32>(reinterpret_cast<uptr>(&VU0) + ViOffset(REG_VPU_STAT))) ||
					!m_code.EmitLdrImm12(0, 3, 0) ||
					!m_code.EmitOrrImm32(0, 0, vpu_stat_bit) ||
					!m_code.EmitStrImm12(0, 3, 0) ||
					!m_code.EmitMovImm8(0, intc_irq) ||
					!EmitCallAbsoluteClobberVectorState(reinterpret_cast<const void*>(&hwIntcIrq)) ||
					!m_code.EmitMovImm8(0, 1) ||
					!m_code.EmitStrImm12(0, HOST_VU, VuOffset(offsetof(VURegs, ebit))))
				{
					return false;
				}
#if defined(VITASX2_QEMU_VALIDATION)
				if (!EmitQemuDtFlagInlineCounter())
					return false;
#endif

				return m_code.PatchBranch(skip, m_code.Size(), Condition::EQ);
			}

			bool EmitRuntimeDtEbitFinish(size_t* skip_static_ebit)
			{
				// When D/T fires, _vu0Exec()/_vu1Exec() force ebit=1. Finish
				// only that runtime case; if an E-bit also exists and D/T did
				// not fire, the static E-bit countdown below remains authoritative.
				if (!m_code.EmitLdrImm12(0, HOST_VU, VuOffset(offsetof(VURegs, ebit))) ||
					!m_code.EmitCmpImm32(0, 1))
				{
					return false;
				}
				const size_t skip = m_code.EmitBranchPlaceholder(Condition::NE);
				if (skip == static_cast<size_t>(-1))
					return false;

				if (!m_code.EmitMovImm8(0, 0) ||
					!m_code.EmitStrImm12(0, HOST_VU, VuOffset(offsetof(VURegs, ebit))) ||
					!EmitInlineEbitFinish(false))
				{
					return false;
				}

				*skip_static_ebit = m_code.EmitBranchPlaceholder();
				if (*skip_static_ebit == static_cast<size_t>(-1))
					return false;

				return m_code.PatchBranch(skip, m_code.Size(), Condition::NE);
			}

			bool EmitInlineVu0MFlag()
			{
				// PCSX2 owner: VU0microInterp.cpp::_vu0Exec() sets
				// VUFLAG_MFLAGSET before executing the flagged pair; the outer
				// Execute() loop exits after that pair.
				return m_code.EmitLdrImm12(0, HOST_VU, VuOffset(offsetof(VURegs, flags))) &&
					m_code.EmitOrrImm32(0, 0, VUFLAG_MFLAGSET) &&
					m_code.EmitStrImm12(0, HOST_VU, VuOffset(offsetof(VURegs, flags)));
			}

			bool EmitInlineLowerFdivStallTest(const _VURegsNum& regs,
				bool elide_canonical_fmac_test)
			{
				// PCSX2 owner: VUops.cpp::_vuTestFDIVStalls(). FMAC read
				// stalls run first, then a pending FDIV pipe can advance
				// VU->cycle to fdiv.sCycle + fdiv.Cycle.
				constexpr size_t base = offsetof(VURegs, fdiv);
				if ((!elide_canonical_fmac_test && !EmitInlineFmacStallTestBody(regs)) ||
					!m_code.EmitLdrImm12(0, HOST_VU, VuOffset(base + offsetof(fdivPipe, enable))) ||
					!m_code.EmitCmpImm32(0, 0))
				{
					return false;
				}
				const size_t skip = m_code.EmitBranchPlaceholder(Condition::EQ);
				if (skip == static_cast<size_t>(-1))
					return false;

				if (!m_code.EmitLdrImm12(0, HOST_VU, VuOffset(base + offsetof(fdivPipe, sCycle))) ||
					!m_code.EmitLdrImm12(1, HOST_VU, VuOffset(base + offsetof(fdivPipe, sCycle) + 4)) ||
					!m_code.EmitLdrImm12(2, HOST_VU, VuOffset(base + offsetof(fdivPipe, Cycle))) ||
					!m_code.EmitAddReg(0, 0, 2, true) ||
					!m_code.EmitAdcImm8(1, 1, 0) ||
					!EmitStoreCycleIfNewer(0, 1, 2, 3))
				{
					return false;
				}

				if (!m_code.PatchBranch(skip, m_code.Size(), Condition::EQ))
					return false;

#if defined(VITASX2_QEMU_VALIDATION)
				return EmitQemuLowerFdivStallTestInlineCounter();
#else
				return true;
#endif
			}

			bool EmitInlineLowerEfuStallTest(const _VURegsNum& regs,
				bool elide_canonical_fmac_test)
			{
				// PCSX2 owner: VUops.cpp::_vuTestEFUStalls(). Preserve the
				// helper's `efu.Cycle -= 1` side effect before waiting; the
				// following _vuTestPipes() observes the adjusted EFU latency.
				constexpr size_t base = offsetof(VURegs, efu);
				if ((!elide_canonical_fmac_test && !EmitInlineFmacStallTestBody(regs)) ||
					!m_code.EmitLdrImm12(0, HOST_VU, VuOffset(base + offsetof(efuPipe, enable))) ||
					!m_code.EmitCmpImm32(0, 0))
				{
					return false;
				}
				const size_t skip = m_code.EmitBranchPlaceholder(Condition::EQ);
				if (skip == static_cast<size_t>(-1))
					return false;

				if (!m_code.EmitLdrImm12(2, HOST_VU, VuOffset(base + offsetof(efuPipe, Cycle))) ||
					!m_code.EmitSubImm8(2, 2, 1) ||
					!m_code.EmitStrImm12(2, HOST_VU, VuOffset(base + offsetof(efuPipe, Cycle))) ||
					!m_code.EmitLdrImm12(0, HOST_VU, VuOffset(base + offsetof(efuPipe, sCycle))) ||
					!m_code.EmitLdrImm12(1, HOST_VU, VuOffset(base + offsetof(efuPipe, sCycle) + 4)) ||
					!m_code.EmitAddReg(0, 0, 2, true) ||
					!m_code.EmitAdcImm8(1, 1, 0) ||
					!EmitStoreCycleIfNewer(0, 1, 2, 3))
				{
					return false;
				}

				if (!m_code.PatchBranch(skip, m_code.Size(), Condition::EQ))
					return false;

#if defined(VITASX2_QEMU_VALIDATION)
				return EmitQemuLowerEfuStallTestInlineCounter();
#else
				return true;
#endif
			}

			bool EmitInlineAddFdivStalls(u32 cycles)
			{
				// PCSX2 owner: VUops.cpp::_vuFDIVAdd() via
				// _vuAddLowerStalls(). The lower body has already produced Q
				// and statusflag; this records the pending FDIV pipe without a
				// C++ tail call.
				constexpr size_t base = offsetof(VURegs, fdiv);
				return m_code.EmitMovImm8(0, 1) &&
					m_code.EmitStrImm12(0, HOST_VU, VuOffset(base + offsetof(fdivPipe, enable))) &&
					EmitLoadCurrentCycleLow(0) &&
					EmitLoadCurrentCycleHigh(1) &&
					m_code.EmitStrImm12(0, HOST_VU, VuOffset(base + offsetof(fdivPipe, sCycle))) &&
					m_code.EmitStrImm12(1, HOST_VU, VuOffset(base + offsetof(fdivPipe, sCycle) + 4)) &&
					m_code.EmitMovImm32(0, cycles) &&
					m_code.EmitStrImm12(0, HOST_VU, VuOffset(base + offsetof(fdivPipe, Cycle))) &&
					m_code.EmitLdrImm12(0, HOST_VU, VuOffset(offsetof(VURegs, q))) &&
					m_code.EmitStrImm12(0, HOST_VU, VuOffset(base + offsetof(fdivPipe, reg))) &&
					m_code.EmitLdrImm12(0, HOST_VU, VuOffset(offsetof(VURegs, statusflag))) &&
					m_code.EmitStrImm12(0, HOST_VU, VuOffset(base + offsetof(fdivPipe, statusflag)));
			}

			bool EmitInlineAddEfuStalls(u32 cycles)
			{
				// PCSX2 owner: VUops.cpp::_vuEFUAdd() via
				// _vuAddLowerStalls(). P is snapshotted into the pending EFU
				// pipe after the lower EFU body computes it.
				constexpr size_t base = offsetof(VURegs, efu);
				return m_code.EmitMovImm8(0, 1) &&
					m_code.EmitStrImm12(0, HOST_VU, VuOffset(base + offsetof(efuPipe, enable))) &&
					EmitLoadCurrentCycleLow(0) &&
					EmitLoadCurrentCycleHigh(1) &&
					m_code.EmitStrImm12(0, HOST_VU, VuOffset(base + offsetof(efuPipe, sCycle))) &&
					m_code.EmitStrImm12(1, HOST_VU, VuOffset(base + offsetof(efuPipe, sCycle) + 4)) &&
					m_code.EmitMovImm32(0, cycles) &&
					m_code.EmitStrImm12(0, HOST_VU, VuOffset(base + offsetof(efuPipe, Cycle))) &&
					m_code.EmitLdrImm12(0, HOST_VU, VuOffset(offsetof(VURegs, p))) &&
					m_code.EmitStrImm12(0, HOST_VU, VuOffset(base + offsetof(efuPipe, reg)));
			}

			bool EmitInlineAddIaluStalls(const _VURegsNum& regs)
			{
				if (regs.cycles == 0)
					return true;

				// PCSX2 owner: VUops.cpp::_vuAddIALUStalls(). Keep the same
				// four-entry circular queue so branch ops still observe VI
				// write visibility through _vuTestALUStalls().
				constexpr size_t base = offsetof(VURegs, ialu);
				return m_code.EmitLdrImm12(0, HOST_VU, VuOffset(offsetof(VURegs, ialuwritepos))) &&
					m_code.EmitAddImm32(3, HOST_VU, base) &&
					m_code.EmitAddRegShiftImm(3, 3, 0, ShiftType::LSL, 4) &&
					m_code.EmitAddRegShiftImm(3, 3, 0, ShiftType::LSL, 3) &&
					m_code.EmitMovImm32(1, regs.VIwrite) &&
					m_code.EmitStrImm12(1, 3, offsetof(ialuPipe, reg)) &&
					EmitLoadCurrentCycleLow(1) &&
					EmitLoadCurrentCycleHigh(2) &&
					m_code.EmitStrImm12(1, 3, offsetof(ialuPipe, sCycle)) &&
					m_code.EmitStrImm12(2, 3, offsetof(ialuPipe, sCycle) + 4) &&
					m_code.EmitMovImm32(1, regs.cycles) &&
					m_code.EmitStrImm12(1, 3, offsetof(ialuPipe, Cycle)) &&
					m_code.EmitAddImm8(0, 0, 1) &&
					m_code.EmitAndImm32(0, 0, 3) &&
					m_code.EmitStrImm12(0, HOST_VU, VuOffset(offsetof(VURegs, ialuwritepos))) &&
					m_code.EmitLdrImm12(0, HOST_VU, VuOffset(offsetof(VURegs, ialucount))) &&
					m_code.EmitAddImm8(0, 0, 1) &&
					m_code.EmitStrImm12(0, HOST_VU, VuOffset(offsetof(VURegs, ialucount)));
			}

			bool EmitComputeFmacWritePtr(unsigned ptr_reg, unsigned index_reg)
			{
				return m_code.EmitLdrImm12(index_reg, HOST_VU, VuOffset(offsetof(VURegs, fmacwritepos))) &&
					m_code.EmitAddImm32(ptr_reg, HOST_VU, offsetof(VURegs, fmac)) &&
					m_code.EmitAddRegShiftImm(ptr_reg, ptr_reg, index_reg, ShiftType::LSL, 5) &&
					m_code.EmitAddRegShiftImm(ptr_reg, ptr_reg, index_reg, ShiftType::LSL, 4);
			}

			struct LocalFmacEntry
			{
				u32 pair_index = 0;
				u8 slot = 0;
				bool active = false;
				bool producer_flags_captured = false;
				bool sticky_only_accumulated = false;
			};

			struct EmptyPipeNopSlowPath
			{
				size_t branch_site = static_cast<size_t>(-1);
				size_t continuation = static_cast<size_t>(-1);
				u32 first_pair = 0;
				u32 pair_count = 0;
				std::array<LocalFmacEntry, LOCAL_FMAC_SLOT_COUNT> initial_fmac{};
				std::array<LocalFmacEntry, LOCAL_FMAC_SLOT_COUNT> final_fmac{};
				s8 initial_latest_working = -1;
				s8 final_latest_working = -1;
				bool initial_norm_consts_ready = false;
				bool initial_norm_maxf_ready = false;
				bool initial_dead_fmac_sticky_pending = false;
				bool initial_full_mac_weights_ready = false;
				bool final_norm_consts_ready = false;
				bool final_norm_maxf_ready = false;
				bool final_dead_fmac_sticky_pending = false;
				bool final_full_mac_weights_ready = false;
			};

			s8 LocalFmacEntryIndex(const LocalFmacEntry* entry) const
			{
				if (!entry)
					return -1;
				const ptrdiff_t index = entry - m_local_fmac_entries.data();
				return index >= 0 && index < static_cast<ptrdiff_t>(LOCAL_FMAC_SLOT_COUNT) ?
					static_cast<s8>(index) : -1;
			}

			LocalFmacEntry* LocalFmacEntryAt(s8 index)
			{
				return index >= 0 ? &m_local_fmac_entries[static_cast<u32>(index)] : nullptr;
			}

			static bool LocalFmacStatesMatch(
				const std::array<LocalFmacEntry, LOCAL_FMAC_SLOT_COUNT>& lhs,
				const std::array<LocalFmacEntry, LOCAL_FMAC_SLOT_COUNT>& rhs)
			{
				for (u32 i = 0; i < LOCAL_FMAC_SLOT_COUNT; i++)
				{
					if (lhs[i].pair_index != rhs[i].pair_index ||
						lhs[i].slot != rhs[i].slot || lhs[i].active != rhs[i].active ||
						lhs[i].producer_flags_captured != rhs[i].producer_flags_captured ||
						lhs[i].sticky_only_accumulated != rhs[i].sticky_only_accumulated)
					{
						return false;
					}
				}
				return true;
			}

			bool EmitAdvanceAdditionalNopCycles(u8 cycles)
			{
				if (cycles == 0)
					return true;
				if (m_resident_cycle_high)
				{
					return m_code.EmitAddImm8(HOST_CYCLE_LO, HOST_CYCLE_LO,
						cycles, true) &&
						m_code.EmitAdcImm8(HOST_CYCLE_HI, HOST_CYCLE_HI, 0);
				}

				if (!m_resident_cycle)
				{
					return m_code.EmitLdrImm12(0, HOST_VU,
							VuOffset(offsetof(VURegs, cycle) + 4)) &&
						m_code.EmitAddImm8(HOST_CYCLE_LO, HOST_CYCLE_LO, cycles, true) &&
						m_code.EmitAdcImm8(0, 0, 0) &&
						m_code.EmitStrImm12(HOST_CYCLE_LO, HOST_VU,
							VuOffset(offsetof(VURegs, cycle))) &&
						m_code.EmitStrImm12(0, HOST_VU,
							VuOffset(offsetof(VURegs, cycle) + 4));
				}

				if (!m_code.EmitAddImm8(HOST_CYCLE_LO, HOST_CYCLE_LO, cycles, true))
					return false;
				const size_t skip_high = m_code.EmitBranchPlaceholder(Condition::CC);
				if (skip_high == static_cast<size_t>(-1) ||
					!m_code.EmitLdrImm12(0, HOST_VU,
						VuOffset(offsetof(VURegs, cycle) + 4)) ||
					!m_code.EmitAddImm8(0, 0, 1) ||
					!m_code.EmitStrImm12(0, HOST_VU,
						VuOffset(offsetof(VURegs, cycle) + 4)))
				{
					return false;
				}
				return m_code.PatchBranch(skip_high, m_code.Size(), Condition::CC);
			}

			bool EmitEmptyPipeNopRun(u32 first_pair, u32 pair_count)
			{
				if (!UsesResidentPipeActivity() || pair_count < 2 || pair_count > 64)
					return false;

				EmptyPipeNopSlowPath slow;
				slow.first_pair = first_pair;
				slow.pair_count = pair_count;
				slow.initial_fmac = m_local_fmac_entries;
				slow.initial_latest_working = LocalFmacEntryIndex(m_latest_working_fmac_entry);
				slow.initial_norm_consts_ready = m_norm_consts_ready;
				slow.initial_norm_maxf_ready = m_norm_maxf_ready;
				slow.initial_dead_fmac_sticky_pending = m_dead_fmac_sticky_pending;
				slow.initial_full_mac_weights_ready = m_full_mac_weights_ready;

				// PCSX2 owner: VU1microInterp.cpp::_vu1CanFastForwardPlainNopPairs().
					// r10 packs exact fmaccount below shifted non-FMAC predicates.
				// Compiler-owned FMAC slots are handled below.
				if (!m_code.EmitCmpImm32(HOST_STALL_SCRATCH, 0))
				{
					return false;
				}
				slow.branch_site = m_code.EmitBranchPlaceholder(Condition::NE);
				if (slow.branch_site == static_cast<size_t>(-1) ||
					!EmitBudgetCheckAndCycleIncrement(first_pair))
				{
					return false;
				}
#if defined(VITASX2_QEMU_VALIDATION)
				if (first_pair == 0 && !EmitQemuLocalFmacPipelineCounters())
					return false;
#endif
				if (!EmitAdvanceAdditionalNopCycles(static_cast<u8>(pair_count - 1)) ||
					!EmitRetireLocalFmacEntries(first_pair + pair_count - 1) ||
					// _vu1FastForwardPlainNopPairs() subtracts min(run, backup).
					// The architectural backup window is two cycles and this run is at
					// least two pairs, so its exact final value is unconditionally zero.
					!m_code.EmitMovImm8(0, 0) ||
					!m_code.EmitStrbImm12(0, HOST_VU,
						VuOffset(offsetof(VURegs, VIBackupCycles))))
				{
					return false;
				}
#if defined(VITASX2_QEMU_VALIDATION)
				if (!EmitQemuEmptyPipeNopBatchCounters(static_cast<u8>(pair_count)))
					return false;
#endif

				slow.continuation = m_code.Size();
				slow.final_fmac = m_local_fmac_entries;
				slow.final_latest_working = LocalFmacEntryIndex(m_latest_working_fmac_entry);
				slow.final_norm_consts_ready = m_norm_consts_ready;
				slow.final_norm_maxf_ready = m_norm_maxf_ready;
				slow.final_dead_fmac_sticky_pending = m_dead_fmac_sticky_pending;
				slow.final_full_mac_weights_ready = m_full_mac_weights_ready;
				m_empty_pipe_nop_slow_paths.push_back(slow);
				return true;
			}

			bool EmitEmptyPipeNopSlowPaths()
			{
				if (m_empty_pipe_nop_slow_paths.empty())
					return true;

				const auto block_final_fmac = m_local_fmac_entries;
				const s8 block_final_latest = LocalFmacEntryIndex(m_latest_working_fmac_entry);
				const bool block_final_norm_consts = m_norm_consts_ready;
				const bool block_final_norm_maxf = m_norm_maxf_ready;
				const bool block_final_dead_fmac_sticky_pending = m_dead_fmac_sticky_pending;
				const bool block_final_full_mac_weights_ready = m_full_mac_weights_ready;

				for (const EmptyPipeNopSlowPath& slow : m_empty_pipe_nop_slow_paths)
				{
					const size_t slow_target = m_code.Size();
					if (!m_code.PatchBranch(slow.branch_site, slow_target, Condition::NE))
						return false;

					m_local_fmac_entries = slow.initial_fmac;
					m_latest_working_fmac_entry =
						LocalFmacEntryAt(slow.initial_latest_working);
					m_pending_local_fmac_entry = nullptr;
					m_capture_pending_local_fmac_flags = false;
					m_norm_consts_ready = slow.initial_norm_consts_ready;
					m_norm_maxf_ready = slow.initial_norm_maxf_ready;
					m_dead_fmac_sticky_pending = slow.initial_dead_fmac_sticky_pending;
					m_full_mac_weights_ready = slow.initial_full_mac_weights_ready;
					for (u32 i = 0; i < slow.pair_count; i++)
					{
						if (!EmitPair(slow.first_pair + i))
							return false;
					}
					if (!LocalFmacStatesMatch(m_local_fmac_entries, slow.final_fmac) ||
						LocalFmacEntryIndex(m_latest_working_fmac_entry) !=
							slow.final_latest_working ||
						m_norm_consts_ready != slow.final_norm_consts_ready ||
						m_norm_maxf_ready != slow.final_norm_maxf_ready ||
						m_dead_fmac_sticky_pending != slow.final_dead_fmac_sticky_pending ||
						m_full_mac_weights_ready != slow.final_full_mac_weights_ready)
					{
						return false;
					}
					const size_t rejoin = m_code.EmitBranchPlaceholder();
					if (rejoin == static_cast<size_t>(-1) ||
						!m_code.PatchBranch(rejoin, slow.continuation))
					{
						return false;
					}
				}

				m_local_fmac_entries = block_final_fmac;
				m_latest_working_fmac_entry = LocalFmacEntryAt(block_final_latest);
				m_pending_local_fmac_entry = nullptr;
				m_capture_pending_local_fmac_flags = false;
				m_norm_consts_ready = block_final_norm_consts;
				m_norm_maxf_ready = block_final_norm_maxf;
				m_dead_fmac_sticky_pending = block_final_dead_fmac_sticky_pending;
				m_full_mac_weights_ready = block_final_full_mac_weights_ready;
				return true;
			}

			static u32 FmacRegUpper(const PairPlan& plan)
			{
				return plan.add_upper_stalls ? plan.uregs.VFwrite : 0;
			}

			static u32 FmacRegLower(const PairPlan& plan)
			{
				return (plan.add_lower_stalls && plan.lregs.pipe == VUPIPE_FMAC) ?
					plan.lregs.VFwrite : 0;
			}

			static u32 FmacFlagReg(const PairPlan& plan)
			{
				return (plan.add_upper_stalls ? plan.uregs.VIwrite : 0) |
					((plan.add_lower_stalls && plan.lregs.pipe == VUPIPE_FMAC) ?
						plan.lregs.VIwrite : 0);
			}

			static u32 FmacXyzwUpper(const PairPlan& plan)
			{
				return plan.add_upper_stalls ? plan.uregs.VFwxyzw : 0;
			}

			static u32 FmacXyzwLower(const PairPlan& plan)
			{
				return (plan.add_lower_stalls && plan.lregs.pipe == VUPIPE_FMAC) ?
					plan.lregs.VFwxyzw : 0;
			}

			static u32 LocalFmacOffset(const LocalFmacEntry& entry, u32 field)
			{
				return LOCAL_FMAC_BASE + static_cast<u32>(entry.slot) * LOCAL_FMAC_SLOT_SIZE + field;
			}

			bool LocalFmacConflicts(const LocalFmacEntry& entry, const _VURegsNum& regs) const
			{
				const PairPlan& writer = m_pairs[entry.pair_index];
				const auto conflicts = [](u32 write_reg, u32 write_mask, u32 read_reg, u32 read_mask) {
					return write_reg != 0 && write_reg == read_reg && (write_mask & read_mask) != 0;
				};
				return conflicts(FmacRegUpper(writer), FmacXyzwUpper(writer), regs.VFread0, regs.VFr0xyzw) ||
					conflicts(FmacRegUpper(writer), FmacXyzwUpper(writer), regs.VFread1, regs.VFr1xyzw) ||
					conflicts(FmacRegLower(writer), FmacXyzwLower(writer), regs.VFread0, regs.VFr0xyzw) ||
					conflicts(FmacRegLower(writer), FmacXyzwLower(writer), regs.VFread1, regs.VFr1xyzw);
			}

			bool CanElideCanonicalFmacStallTest(u32 pair_index, const _VURegsNum& regs) const
			{
				// _vuTestPipes() remains in place and owns ready flag/result
				// publication. This only skips a dependency scan which source analysis
				// proves cannot advance VURegs::cycle.
				return CanElideAnalyzedCanonicalFmacStallTest(m_plan, pair_index, regs);
			}

			bool EmitLocalFmacStallTest(u32 pair_index, const _VURegsNum& regs)
			{
				if (!m_plan.local_fmac_pipeline || (regs.VFread0 == 0 && regs.VFread1 == 0))
					return true;

				for (const LocalFmacEntry& entry : m_local_fmac_entries)
				{
					if (!entry.active ||
						entry.pair_index + FMAC_PIPELINE_LATENCY_CYCLES <= pair_index ||
						!LocalFmacConflicts(entry, regs))
						continue;
					if (!m_pairs[entry.pair_index].local_fmac_cycle_snapshot)
						return false;

					const u8 offset = static_cast<u8>(LocalFmacOffset(entry, LOCAL_FMAC_CYCLE_OFFSET));
					if (!m_code.EmitLdrImm12(0, SP, offset) ||
						!m_code.EmitAddImm8(0, 0, FMAC_PIPELINE_LATENCY_CYCLES) ||
						!EmitStoreLocalFmacCycleIfNewer(0, 2, 3, 1))
					{
						return false;
					}
				}
				return true;
			}

			bool EmitMergeAccumulatedDeadFmacSticky()
			{
				if (!m_dead_fmac_sticky_pending)
					return true;

				// Q11's low nibble in each lane holds the category; overflow packing
				// deliberately leaves irrelevant high bits set. Sticky STATUS is the OR
				// of the low categories regardless of retirement order, so reduce all
				// lanes and mask once here. The newest non-sticky nibble remains owned by
				// the ordinary live FMAC entry. Cortex-A9 executes these D-register
				// logical/permute operations in one MPE issue cycle each, and the merge
				// is paid once per block.
				constexpr unsigned accumulator_lo = VU_DEAD_FMAC_STICKY_Q * 2;
				constexpr unsigned accumulator_hi = accumulator_lo + 1;
				constexpr unsigned scratch_d = VU_NORM_TMP_Q * 2;
				if (!m_code.EmitVorrD(accumulator_lo, accumulator_lo, accumulator_hi) ||
					!m_code.EmitVrev64I32D(scratch_d, accumulator_lo) ||
					!m_code.EmitVorrD(accumulator_lo, accumulator_lo, scratch_d) ||
					!m_code.EmitVmovD32LaneToCore(0, accumulator_lo, 0) ||
					!m_code.EmitAndImm32(0, 0, 0x0f) ||
					!m_code.EmitOrrRegShiftImm(HOST_LIMIT_LO, HOST_LIMIT_LO, 0,
						ShiftType::LSL, 6))
				{
					return false;
				}

				m_dead_fmac_sticky_pending = false;
				return true;
			}

			bool EmitPublishLocalFmacFlags(const LocalFmacEntry& entry)
			{
				if (entry.sticky_only_accumulated)
					return true;

				const PairPlan& plan = m_pairs[entry.pair_index];
				const u32 flagreg = FmacFlagReg(plan);
				if ((flagreg & (1u << REG_CLIP_FLAG)) != 0 &&
					(!m_code.EmitLdrImm12(0, SP, LocalFmacOffset(entry, LOCAL_FMAC_CLIP_OFFSET)) ||
					 !m_code.EmitStrImm12(0, HOST_VU, ViOffset(REG_CLIP_FLAG))))
				{
					return false;
				}

				if (m_plan.deferred_fmac_flags)
				{
					// PCSX2 owner: VUops.cpp::_vuFMACflush(). r6/r7 are the
					// block-private STATUS/MAC instances loaded at block entry.
					// Preserve the sticky/non-sticky STATUS formulas exactly, but do not
					// round-trip either flag through VURegs for every retired pair.
					if ((flagreg & (1u << REG_STATUS_FLAG)) != 0 &&
						entry.producer_flags_captured)
					{
						// VUflags.cpp::VU_STAT_UPDATE() produces only STATUS[3:0].
						// CanSnapshotLocalFmacFlagsAtProducer() excludes a paired FSSET
						// or FDIV, so this captured snapshot has no [11:6] sticky bits.
						// PCSX2's STATUS-write _vuFMACflush() formula therefore reduces
						// exactly to (architectural & 0x30) | captured_category.
						if (!EmitAndRegImm32(HOST_LIMIT_LO, HOST_LIMIT_LO, 0x30u,
								HOST_CALL_SCRATCH) ||
							!m_code.EmitLdrImm12(0, SP,
								LocalFmacOffset(entry, LOCAL_FMAC_STATUS_OFFSET)) ||
							!m_code.EmitOrrReg(HOST_LIMIT_LO, HOST_LIMIT_LO, 0))
						{
							return false;
						}
					}
					else if ((flagreg & (1u << REG_STATUS_FLAG)) != 0)
					{
						if (!EmitAndRegImm32(HOST_LIMIT_LO, HOST_LIMIT_LO, 0x30u,
								HOST_CALL_SCRATCH) ||
							!m_code.EmitLdrImm12(0, SP,
								LocalFmacOffset(entry, LOCAL_FMAC_STATUS_OFFSET)) ||
							!EmitAndRegImm32(1, 0, 0xfc0u, HOST_CALL_SCRATCH) ||
							!m_code.EmitOrrReg(HOST_LIMIT_LO, HOST_LIMIT_LO, 1) ||
							!EmitAndRegImm32(0, 0, 0x0fu, HOST_CALL_SCRATCH) ||
							!m_code.EmitOrrReg(HOST_LIMIT_LO, HOST_LIMIT_LO, 0))
						{
							return false;
						}
					}
					else if (entry.producer_flags_captured)
					{
						// EmitCapturePendingLocalFmacFlags() captured the producer's
						// exact non-sticky category nibble. ARMv7 BFI replaces only
						// STATUS[3:0], preserving the accumulated sticky field; the ORR
						// then applies PCSX2's _vuFMACflush() sticky update. This is the
						// same formula as the conservative path below in four rather than
						// six A32 instructions, including the MAC load.
						if (!m_code.EmitLdrImm12(0, SP,
								LocalFmacOffset(entry, LOCAL_FMAC_STATUS_OFFSET)) ||
							!m_code.EmitBfi(HOST_LIMIT_LO, 0, 0, 4) ||
							!m_code.EmitOrrRegShiftImm(HOST_LIMIT_LO, HOST_LIMIT_LO, 0,
								ShiftType::LSL, 6))
						{
							return false;
						}
					}
					else if (!EmitAndRegImm32(HOST_LIMIT_LO, HOST_LIMIT_LO, 0xff0u,
							HOST_CALL_SCRATCH) ||
						!m_code.EmitLdrImm12(0, SP,
							LocalFmacOffset(entry, LOCAL_FMAC_STATUS_OFFSET)) ||
						!EmitAndRegImm32(0, 0, 0x0fu, HOST_CALL_SCRATCH) ||
						!m_code.EmitOrrReg(HOST_LIMIT_LO, HOST_LIMIT_LO, 0) ||
						!m_code.EmitOrrRegShiftImm(HOST_LIMIT_LO, HOST_LIMIT_LO, 0,
							ShiftType::LSL, 6))
					{
						return false;
					}

					return m_code.EmitLdrImm12(HOST_LIMIT_HI, SP,
						LocalFmacOffset(entry, LOCAL_FMAC_MAC_OFFSET));
				}

				if ((flagreg & (1u << REG_STATUS_FLAG)) != 0)
				{
					if (!m_code.EmitLdrImm12(0, HOST_VU, ViOffset(REG_STATUS_FLAG)) ||
						!EmitAndRegImm32(0, 0, 0x30u, HOST_CALL_SCRATCH) ||
						!m_code.EmitLdrImm12(1, SP, LocalFmacOffset(entry, LOCAL_FMAC_STATUS_OFFSET)) ||
						!EmitAndRegImm32(2, 1, 0xfc0u, HOST_CALL_SCRATCH) ||
						!m_code.EmitOrrReg(0, 0, 2) ||
						!EmitAndRegImm32(1, 1, 0x0fu, HOST_CALL_SCRATCH) ||
						!m_code.EmitOrrReg(0, 0, 1) ||
						!m_code.EmitStrImm12(0, HOST_VU, ViOffset(REG_STATUS_FLAG)))
					{
						return false;
					}
				}
				else
				{
					if (!m_code.EmitLdrImm12(0, HOST_VU, ViOffset(REG_STATUS_FLAG)) ||
						!EmitAndRegImm32(0, 0, 0xff0u, HOST_CALL_SCRATCH) ||
						!m_code.EmitLdrImm12(1, SP, LocalFmacOffset(entry, LOCAL_FMAC_STATUS_OFFSET)) ||
						!EmitAndRegImm32(1, 1, 0x0fu, HOST_CALL_SCRATCH) ||
						!m_code.EmitOrrReg(0, 0, 1) ||
						!m_code.EmitOrrRegShiftImm(0, 0, 1, ShiftType::LSL, 6) ||
						!m_code.EmitStrImm12(0, HOST_VU, ViOffset(REG_STATUS_FLAG)))
					{
						return false;
					}
				}

				return m_code.EmitLdrImm12(0, SP, LocalFmacOffset(entry, LOCAL_FMAC_MAC_OFFSET)) &&
					m_code.EmitStrImm12(0, HOST_VU, ViOffset(REG_MAC_FLAG));
			}

			bool EmitPublishDeferredFmacFlags()
			{
				if (!m_plan.deferred_fmac_flags)
					return true;

				return m_code.EmitStrImm12(HOST_LIMIT_LO, HOST_VU,
						ViOffset(REG_STATUS_FLAG)) &&
					m_code.EmitStrImm12(HOST_LIMIT_HI, HOST_VU, ViOffset(REG_MAC_FLAG));
			}

			bool EmitRetireLocalFmacEntries(u32 pair_index)
			{
				if (!m_plan.local_fmac_pipeline)
					return true;
				for (u32 ordered_index = 0; ordered_index < LOCAL_FMAC_SLOT_COUNT; ordered_index++)
				{
					LocalFmacEntry* oldest_ready = nullptr;
					for (LocalFmacEntry& candidate : m_local_fmac_entries)
					{
						if (!candidate.active ||
							candidate.pair_index + FMAC_PIPELINE_LATENCY_CYCLES > pair_index)
							continue;
						if (!oldest_ready || candidate.pair_index < oldest_ready->pair_index)
							oldest_ready = &candidate;
					}
					if (!oldest_ready)
						break;
					if (!EmitPublishLocalFmacFlags(*oldest_ready))
						return false;
					oldest_ready->active = false;
				}
				return true;
			}

			bool EmitCommitLocalFmac(u32 pair_index, const PairPlan& plan)
			{
				LocalFmacEntry* entry = m_pending_local_fmac_entry;
				if (!entry)
					return false;

				if (entry->active || entry->pair_index != pair_index)
					return false;
				entry->active = true;
				bool emitted = true;
				if (plan.local_fmac_cycle_snapshot)
				{
					const u8 cycle_offset = static_cast<u8>(
						LocalFmacOffset(*entry, LOCAL_FMAC_CYCLE_OFFSET));
					emitted = EmitLoadCurrentCycleLow(0) &&
						m_code.EmitStrImm12(0, SP, cycle_offset);
				}
				if (emitted && !entry->producer_flags_captured &&
					!entry->sticky_only_accumulated)
				{
					emitted = EmitLoadWorkingFmacMacStatus(0, 1) &&
						m_code.EmitStrdImm8(0, 1, SP,
							static_cast<u8>(LocalFmacOffset(*entry, LOCAL_FMAC_MAC_OFFSET)));
					if (emitted && m_plan.resident_working_fmac_flags)
						m_latest_working_fmac_entry = entry;
				}
				if (emitted && (FmacFlagReg(plan) & (1u << REG_CLIP_FLAG)) != 0)
				{
					emitted = m_code.EmitLdrImm12(0, HOST_VU,
							VuOffset(offsetof(VURegs, clipflag))) &&
						m_code.EmitStrImm12(0, SP,
							LocalFmacOffset(*entry, LOCAL_FMAC_CLIP_OFFSET));
				}
				if (!emitted)
					return false;
				m_pending_local_fmac_entry = nullptr;
				m_capture_pending_local_fmac_flags = false;

#if defined(VITASX2_QEMU_VALIDATION)
				if (!EmitQemuFmacClearInlineCounter())
					return false;
				if (plan.add_upper_stalls && !EmitQemuUpperFmacStallInlineCounter())
					return false;
				if (plan.add_lower_stalls && plan.lregs.pipe == VUPIPE_FMAC &&
					(!EmitQemuLowerFmacStallInlineCounter() || !EmitQemuLowerStallInlineCounter()))
				{
					return false;
				}
#endif
				return true;
			}

			bool EmitPublishResidentWorkingFmacFlags()
			{
				if (!m_plan.resident_working_fmac_flags)
					return true;
				if (!m_latest_working_fmac_entry)
					return false;

				// PCSX2 microVU keeps the current non-architectural MAC/STATUS
				// instances resident in its allocator. Vita keeps the same pair in
				// the newest private FMAC slot and publishes it once before any
				// direct-link lookup, dispatcher return, or helper-visible seam.
				return m_code.EmitLdrdImm8(0, 1, SP,
						static_cast<u8>(LocalFmacOffset(*m_latest_working_fmac_entry,
							LOCAL_FMAC_MAC_OFFSET))) &&
					m_code.EmitStrImm12(0, HOST_VU, VuOffset(offsetof(VURegs, macflag))) &&
					m_code.EmitStrImm12(1, HOST_VU, VuOffset(offsetof(VURegs, statusflag)));
			}

			bool EmitAppendLocalFmacEntry(const LocalFmacEntry& entry)
			{
				const PairPlan& plan = m_pairs[entry.pair_index];
				// Resident-pipe blocks preserve the non-FMAC aggregate above r10's low
				// count bits. Form the circular slot address from only that exact index;
				// r0 is overwritten by the static FMAC header immediately afterwards.
				if (!m_code.EmitAndImm32(0, HOST_STALL_SCRATCH, 3) ||
					!m_code.EmitAddImm32(HOST_CALL_SCRATCH, HOST_VU, offsetof(VURegs, fmac)) ||
					!m_code.EmitAddRegShiftImm(HOST_CALL_SCRATCH, HOST_CALL_SCRATCH,
						0, ShiftType::LSL, 5) ||
					!m_code.EmitAddRegShiftImm(HOST_CALL_SCRATCH, HOST_CALL_SCRATCH,
						0, ShiftType::LSL, 4) ||
					!m_code.EmitMovImm32(0, FmacRegUpper(plan)) ||
					!m_code.EmitMovImm32(1, FmacRegLower(plan)) ||
					!m_code.EmitStrdImm8(0, 1, HOST_CALL_SCRATCH, offsetof(fmacPipe, regupper)) ||
					!m_code.EmitMovImm32(0, FmacFlagReg(plan)) ||
					!m_code.EmitMovImm32(1, FmacXyzwUpper(plan)) ||
					!m_code.EmitStrdImm8(0, 1, HOST_CALL_SCRATCH, offsetof(fmacPipe, flagreg)) ||
					!m_code.EmitMovImm32(0, FmacXyzwLower(plan)) ||
					!m_code.EmitMovImm8(1, 0) ||
					!m_code.EmitStrdImm8(0, 1, HOST_CALL_SCRATCH, offsetof(fmacPipe, xyzwlower)) ||
					!m_code.EmitLdrImm12(0, SP,
						LocalFmacOffset(entry, LOCAL_FMAC_CYCLE_OFFSET)) ||
					!EmitLoadCurrentCycleHigh(1) ||
					!EmitLoadCurrentCycleLow(2) ||
					!m_code.EmitCmpReg(0, 2) ||
					!m_code.EmitSubImm8(1, 1, 1, false, Condition::HI) ||
					!m_code.EmitStrdImm8(0, 1, HOST_CALL_SCRATCH, offsetof(fmacPipe, sCycle)) ||
					!m_code.EmitMovImm8(0, FMAC_PIPELINE_LATENCY_CYCLES) ||
					!m_code.EmitLdrImm12(1, SP, LocalFmacOffset(entry, LOCAL_FMAC_MAC_OFFSET)) ||
					!m_code.EmitStrdImm8(0, 1, HOST_CALL_SCRATCH, offsetof(fmacPipe, Cycle)) ||
					!m_code.EmitLdrImm12(0, SP, LocalFmacOffset(entry, LOCAL_FMAC_STATUS_OFFSET)) ||
					!m_code.EmitLdrImm12(1, SP, LocalFmacOffset(entry, LOCAL_FMAC_CLIP_OFFSET)) ||
					!m_code.EmitStrdImm8(0, 1, HOST_CALL_SCRATCH, offsetof(fmacPipe, statusflag)) ||
					!m_code.EmitAddImm8(HOST_STALL_SCRATCH, HOST_STALL_SCRATCH, 1))
				{
					return false;
				}
				return true;
			}

			bool EmitCanonicalizeLocalFmacPipeline()
			{
				if (!m_plan.local_fmac_pipeline)
					return true;

				// The canonical FMAC queue is rebuilt from the block-private entries.
				// Retain the exact FDIV/EFU/IALU/XGKICK aggregate carried above bit 7
				// so a compatible linked target can consume it without state reloads.
				if (!(UsesResidentPipeActivity() ?
						m_code.EmitBicImm32(HOST_STALL_SCRATCH, HOST_STALL_SCRATCH,
							RESIDENT_FMAC_COUNT_MASK) :
						m_code.EmitMovImm8(HOST_STALL_SCRATCH, 0)))
					return false;
				for (u32 retire_index = 0; retire_index < LOCAL_FMAC_SLOT_COUNT; retire_index++)
				{
					LocalFmacEntry* oldest = nullptr;
					for (LocalFmacEntry& candidate : m_local_fmac_entries)
					{
						if (candidate.active && (!oldest || candidate.pair_index < oldest->pair_index))
							oldest = &candidate;
					}
					if (!oldest)
						break;
					LocalFmacEntry& entry = *oldest;
					if (!m_pairs[entry.pair_index].local_fmac_cycle_snapshot)
						return false;

					// Entries made ready early by an FDIV/EFU/IALU or dependency
					// stall publish here; younger entries retain their exact sCycle
					// and are compacted into PCSX2's canonical circular queue.
					if (!m_code.EmitLdrImm12(0, SP,
							LocalFmacOffset(entry, LOCAL_FMAC_CYCLE_OFFSET)) ||
						!m_code.EmitAddImm8(0, 0, FMAC_PIPELINE_LATENCY_CYCLES) ||
						!m_code.EmitLdrImm12(2, HOST_VU,
							VuOffset(offsetof(VURegs, cycle))) ||
						!m_code.EmitSubReg(3, 2, 0, true))
					{
						return false;
					}
					const size_t ready = m_code.EmitBranchPlaceholder(Condition::GE);
					if (ready == static_cast<size_t>(-1) || !EmitAppendLocalFmacEntry(entry))
						return false;
					const size_t done = m_code.EmitBranchPlaceholder();
					if (done == static_cast<size_t>(-1))
						return false;
					const size_t ready_target = m_code.Size();
					if (!m_code.PatchBranch(ready, ready_target, Condition::GE) ||
						!EmitPublishLocalFmacFlags(entry) ||
						!m_code.PatchBranch(done, m_code.Size()))
					{
						return false;
					}
					entry.active = false;
				}

				return m_code.EmitMovImm8(0, 0) &&
					m_code.EmitStrImm12(0, HOST_VU, VuOffset(offsetof(VURegs, fmacreadpos))) &&
					EmitAndRegImm32(0, HOST_STALL_SCRATCH, 3u, 1) &&
					m_code.EmitStrImm12(0, HOST_VU, VuOffset(offsetof(VURegs, fmacwritepos))) &&
					(UsesResidentPipeActivity() ?
						// Canonical fmaccount is always 0..4, hence its upper bytes are
						// already zero. Store r10's exact low count without its aggregate.
						m_code.EmitStrbImm12(HOST_STALL_SCRATCH, HOST_VU,
							VuOffset(offsetof(VURegs, fmaccount))) :
						m_code.EmitStrImm12(HOST_STALL_SCRATCH, HOST_VU,
							VuOffset(offsetof(VURegs, fmaccount))));
			}

			bool EmitInlineCommitFmacPipe(const PairPlan& plan,
				bool advance_writepos_early)
			{
				// PCSX2 owner: VUops.cpp::_vuClearFMAC() followed immediately by
				// _vuAddUpperStalls() and _vuAddLowerStalls(). No observer exists
				// between those operations. Build their exact final 48-byte pipe
				// state once from the compile-time register metadata.
				const bool upper_fmac = plan.add_upper_stalls;
				const bool lower_fmac = plan.add_lower_stalls && plan.lregs.pipe == VUPIPE_FMAC;
				if (!upper_fmac && !lower_fmac)
					return false;

				const u32 regupper = upper_fmac ? plan.uregs.VFwrite : 0;
				const u32 reglower = lower_fmac ? plan.lregs.VFwrite : 0;
				const u32 flagreg = (upper_fmac ? plan.uregs.VIwrite : 0) |
					(lower_fmac ? plan.lregs.VIwrite : 0);
				const u32 xyzwupper = upper_fmac ? plan.uregs.VFwxyzw : 0;
				const u32 xyzwlower = lower_fmac ? plan.lregs.VFwxyzw : 0;

				// ARM ARM A8.6.189 defines STMIA's register-list order as ascending
				// register number to ascending address. At this pair tail r0-r3, r8,
				// r12, and LR are dead scratch (the generated prologue owns the real
				// return address), so lay out the complete 24-byte static header in
				// those registers and issue one writeback STMIA through LR. Writeback
				// advances LR to sCycle; a second ascending STMIA then publishes the
				// remaining six words. This preserves the implicit padding word cleared
				// by _vuClearFMAC() and all field order while replacing six STRD stores
				// with two Cortex-A9 store-multiple instructions.
				constexpr u16 FMAC_HEADER_REGS =
					(1u << 0) | (1u << 1) | (1u << 2) |
					(1u << 3) | (1u << 8) | (1u << 12);
				// Interpreter working MAC/STATUS/CLIP are three contiguous words.
				// Their required second-half destinations r3/r8/r12 are also in
				// ascending order, so ARM ARM A8.6.53 maps them exactly with LDMIA.
				// r0 is overwritten immediately by the exact current cycle low word.
				constexpr u16 FMAC_WORKING_FLAG_REGS =
					(1u << 3) | (1u << 8) | (1u << 12);
				bool emitted_body =
					EmitComputeFmacWritePtr(14, 0) &&
					// Normal pairs have no observer between canonical queue append and
					// the pair-tail write-position update. Preserve the already-loaded
					// index through address formation, advance it before r0 becomes the
					// static header word, and avoid reloading fmacwritepos at the tail.
					(!advance_writepos_early ||
						(m_code.EmitAddImm8(0, 0, 1) &&
						 m_code.EmitAndImm32(0, 0, 3) &&
						 m_code.EmitStrImm12(0, HOST_VU,
							 VuOffset(offsetof(VURegs, fmacwritepos))))) &&
					m_code.EmitMovImm32(0, regupper) &&
					m_code.EmitMovImm32(1, reglower) &&
					m_code.EmitMovImm32(2, flagreg) &&
					m_code.EmitMovImm32(3, xyzwupper) &&
					m_code.EmitMovImm32(HOST_CLIP_OLD, xyzwlower) &&
					m_code.EmitMovImm8(HOST_CALL_SCRATCH, 0) &&
					m_code.EmitStmIa(14, FMAC_HEADER_REGS, true) &&
					m_code.EmitAddImm32(0, HOST_VU,
						VuOffset(offsetof(VURegs, macflag))) &&
					m_code.EmitLdmIa(0, FMAC_WORKING_FLAG_REGS) &&
					EmitLoadCurrentCycleLow(0) &&
					EmitLoadCurrentCycleHigh(1) &&
					m_code.EmitMovImm8(2, FMAC_PIPELINE_LATENCY_CYCLES) &&
					m_code.EmitStmIa(14, FMAC_HEADER_REGS) &&
					(UsesResidentPipeActivity() ?
						// All canonical writers keep fmaccount in 0..4, so its upper
						// three bytes are zero. Non-FMAC activity starts at bit 8;
						// little-endian Vita can publish the low count directly and
						// leave those already-zero bytes unchanged.
						(m_code.EmitAddImm8(HOST_STALL_SCRATCH, HOST_STALL_SCRATCH, 1) &&
						 m_code.EmitStrbImm12(HOST_STALL_SCRATCH, HOST_VU,
							 VuOffset(offsetof(VURegs, fmaccount)))) :
						(m_code.EmitLdrImm12(0, HOST_VU,
							 VuOffset(offsetof(VURegs, fmaccount))) &&
						 m_code.EmitAddImm8(0, 0, 1) &&
						 m_code.EmitStrImm12(0, HOST_VU,
							 VuOffset(offsetof(VURegs, fmaccount)))));
				if (!emitted_body)
					return false;

#if defined(VITASX2_QEMU_VALIDATION)
				if (!EmitQemuFmacClearInlineCounter())
					return false;
				if (advance_writepos_early && !EmitQemuFmacWriteposLoadElisionCounter())
					return false;
				if (UsesResidentPipeActivity() &&
					!EmitQemuResidentFmacCountAppendElisionCounter())
				{
					return false;
				}
				if (upper_fmac && !EmitQemuUpperFmacStallInlineCounter())
					return false;
				if (lower_fmac &&
					(!EmitQemuLowerFmacStallInlineCounter() || !EmitQemuLowerStallInlineCounter()))
				{
					return false;
				}
#endif
				return true;
			}

			bool EmitInlineAddLowerStalls(const PairPlan& plan)
			{
				bool emitted_body = true;
				switch (plan.lregs.pipe)
				{
					case VUPIPE_IALU:
						emitted_body = EmitInlineAddIaluStalls(plan.lregs);
						break;
					case VUPIPE_FDIV:
						emitted_body = EmitInlineAddFdivStalls(plan.lregs.cycles);
						break;
					case VUPIPE_EFU:
						emitted_body = EmitInlineAddEfuStalls(plan.lregs.cycles);
						break;
					default:
						return false;
				}

				if (!emitted_body)
					return false;

#if defined(VITASX2_QEMU_VALIDATION)
				return EmitQemuLowerStallInlineCounter();
#else
				return true;
#endif
			}

			// PCSX2 owner: x86/microVU_Compile.inl::mVUtestCycles(). The caller
			// admits the initial block only with a positive budget; a generated link
			// tests again before entering its target. Once admitted, the whole block
			// runs even when its final pairs overshoot the requested window. This is
			// microVU's observable scheduling contract, not the interpreter's
			// per-step Execute() guard. Each pair still performs vu1Exec()'s cycle
			// increment and leaves its low word in HOST_CYCLE_LO for VIBackupCycles.
			// Scan-proven one-cycle blocks keep that word private until a helper,
			// link-admission, or dispatcher-return seam observes VURegs.
			bool EmitBudgetCheckAndCycleIncrement(u32 pair_index)
			{
				if (m_resident_cycle)
					return EmitResidentCycleBudgetCheckAndIncrement(pair_index);

				const u16 lo = VuOffset(offsetof(VURegs, cycle));
				const u16 hi = VuOffset(offsetof(VURegs, cycle) + 4);
				if (!m_code.EmitLdrImm12(0, HOST_VU, lo) ||
					!m_code.EmitLdrImm12(1, HOST_VU, hi))
				{
					return false;
				}

				if (pair_index == 0)
				{
					if (!m_code.EmitCmpImm32(HOST_EXEC_BASE, 0))
						return false;
					const size_t skip_entry_check = m_code.EmitBranchPlaceholder(Condition::EQ);
					if (skip_entry_check == static_cast<size_t>(-1))
						return false;
					unsigned limit_lo = HOST_LIMIT_LO;
					unsigned limit_hi = HOST_LIMIT_HI;
					if (m_plan.deferred_fmac_flags)
					{
						limit_lo = 2;
						limit_hi = 3;
						if (!m_code.EmitLdrdImm8(limit_lo, limit_hi, SP,
								static_cast<u8>(DEFERRED_LIMIT_SAVE_OFFSET)))
						{
							return false;
						}
					}
					if (!m_code.EmitCmpReg(1, limit_hi) ||
						!m_code.EmitCmpReg(0, limit_lo, Condition::EQ))
					{
						return false;
					}
					const size_t exit_site = m_code.EmitBranchPlaceholder(Condition::CS);
					if (exit_site == static_cast<size_t>(-1))
						return false;
					m_budget_exits.push_back({exit_site, pair_index, Condition::CS});
					if (!m_code.PatchBranch(skip_entry_check, m_code.Size(), Condition::EQ))
						return false;
				}

				return m_code.EmitAddImm8(0, 0, 1, true) &&
					m_code.EmitAdcImm8(1, 1, 0) &&
					m_code.EmitStrImm12(0, HOST_VU, lo) &&
					m_code.EmitStrImm12(1, HOST_VU, hi) &&
					EmitMovReg(HOST_CYCLE_LO, 0);
			}

			bool EmitResidentCycleBudgetCheckAndIncrement(u32 pair_index)
			{
				if (pair_index == 0)
				{
					if (!m_code.EmitCmpImm32(HOST_EXEC_BASE, 0))
						return false;
					const size_t skip_entry_check = m_code.EmitBranchPlaceholder(Condition::EQ);
					if (skip_entry_check == static_cast<size_t>(-1))
						return false;
					// The linked-entry stub has refreshed the resident cycle word(s). Compare
					// the full 64-bit cycle so a preceding block's permitted
					// overshoot cannot enter another block.
					unsigned limit_lo = HOST_LIMIT_LO;
					unsigned limit_hi = HOST_LIMIT_HI;
					if (m_plan.deferred_fmac_flags)
					{
						limit_lo = 2;
						limit_hi = 3;
						if (!m_code.EmitLdrdImm8(limit_lo, limit_hi, SP,
								static_cast<u8>(DEFERRED_LIMIT_SAVE_OFFSET)))
						{
							return false;
						}
					}
					if (!EmitLoadCurrentCycleHigh(0) ||
						!m_code.EmitCmpReg(0, limit_hi) ||
						!m_code.EmitCmpReg(HOST_CYCLE_LO, limit_lo, Condition::EQ))
						return false;
					const size_t exit_site = m_code.EmitBranchPlaceholder(Condition::CS);
					if (exit_site == static_cast<size_t>(-1))
						return false;
					m_budget_exits.push_back({exit_site, pair_index, Condition::CS});
					if (!m_code.PatchBranch(skip_entry_check, m_code.Size(), Condition::EQ))
						return false;
				}

				if (!m_code.EmitAddImm8(HOST_CYCLE_LO, HOST_CYCLE_LO, 1, true))
				{
					return false;
				}
				if (m_resident_cycle_high)
					return m_code.EmitAdcImm8(HOST_CYCLE_HI, HOST_CYCLE_HI, 0);

				// The high cycle word changes only on the rare low-word wrap.
				// Avoid the normal pair's high-word load/store traffic on A9.
				const size_t skip_high = m_code.EmitBranchPlaceholder(Condition::CC);
				if (skip_high == static_cast<size_t>(-1))
					return false;
				if (!m_code.EmitLdrImm12(0, HOST_VU, VuOffset(offsetof(VURegs, cycle) + 4)) ||
					!m_code.EmitAddImm8(0, 0, 1) ||
					!m_code.EmitStrImm12(0, HOST_VU, VuOffset(offsetof(VURegs, cycle) + 4)))
				{
					return false;
				}

				return m_code.PatchBranch(skip_high, m_code.Size(), Condition::CC);
			}

			// PCSX2 owner: the per-step `VU->VIBackupCycles -=
			// std::min((u8)(VU1.cycle - cyclesBeforeOp), VU->VIBackupCycles)`
			// update, where cyclesBeforeOp is the pre-stall cycle minus one.
			bool EmitViBackupUpdate(u32 pair_index)
			{
				const u16 backup = VuOffset(offsetof(VURegs, VIBackupCycles));
				if (pair_index >= 2 && !m_pairs[pair_index - 1].vi_backup_write)
				{
					if (!m_pairs[pair_index - 2].vi_backup_write)
					{
						// No writer exists inside the only two preceding visibility
						// slots. Any entry-state backup has expired, so canonical state
						// is already zero and there is no work to emit.
						return true;
					}

					// The writer two pairs back installed 2. The intervening pair
					// advanced at least one cycle and this pair has already advanced
					// another, so the exact result is zero even if either pair stalled.
					return m_code.EmitMovImm8(0, 0) &&
						m_code.EmitStrbImm12(0, HOST_VU, backup);
				}

				if (!m_code.EmitLdrbImm12(0, HOST_VU, backup) ||
					!m_code.EmitCmpImm32(0, 0))
				{
					return false;
				}
				const size_t skip = m_code.EmitBranchPlaceholder(Condition::EQ);
				if (skip == static_cast<size_t>(-1))
					return false;

				// Resident-cycle blocks cannot take a stall path, so the exact
				// _vu0Exec()/_vu1Exec() delta is one. Keep the cycle private and
				// decrement the nonzero backup directly instead of round-tripping the
				// resident low word through VURegs just to reconstruct that constant.
				if (m_resident_cycle)
				{
					if (!m_code.EmitSubImm8(0, 0, 1) ||
						!m_code.EmitStrbImm12(0, HOST_VU, backup))
					{
						return false;
					}
					return m_code.PatchBranch(skip, m_code.Size(), Condition::EQ);
				}

				// delta = (u8)(cycle_now - (cycle_at_step_start - 1))
				if (!m_code.EmitLdrImm12(1, HOST_VU, VuOffset(offsetof(VURegs, cycle))) ||
					!m_code.EmitSubReg(1, 1, HOST_CYCLE_LO) ||
					!m_code.EmitAddImm8(1, 1, 1) ||
					!m_code.EmitAndImm32(1, 1, 0xff) ||
					// backup -= min(delta, backup)
					!m_code.EmitSubReg(2, 0, 1, true) ||
					!m_code.EmitMovImm8(2, 0, Condition::CC) ||
					!m_code.EmitStrbImm12(2, HOST_VU, backup))
				{
					return false;
				}

				return m_code.PatchBranch(skip, m_code.Size(), Condition::EQ);
			}

			// PCSX2 owner: _vu0Exec()/_vu1Exec() per-step branch-delay countdown,
			// including the branch-in-delay-slot takedelaybranch handoff.
			bool EmitBranchTail(u32 sequential_tpc)
			{
				const u16 branch = VuOffset(offsetof(VURegs, branch));
				// The interpreter writes the sequential post-increment TPC before
				// branch countdown. Publish that value here, then overwrite it only
				// when this pair resolves a taken branch.
				if (!m_code.EmitMovImm32(1, sequential_tpc) ||
					!m_code.EmitStrImm12(1, HOST_VU, ViOffset(REG_TPC)) ||
					!m_code.EmitLdrImm12(0, HOST_VU, branch) ||
					!m_code.EmitCmpImm32(0, 0))
				{
					return false;
				}
				const size_t skip_all = m_code.EmitBranchPlaceholder(Condition::EQ);
				if (skip_all == static_cast<size_t>(-1))
					return false;

				if (!m_code.EmitSubImm8(0, 0, 1) ||
					!m_code.EmitStrImm12(0, HOST_VU, branch) ||
					!m_code.EmitCmpImm32(0, 0))
				{
					return false;
				}
				const size_t skip_resolve = m_code.EmitBranchPlaceholder(Condition::NE);
				if (skip_resolve == static_cast<size_t>(-1))
					return false;

				// Resolve: TPC = branchpc, then consume a delay-slot branch.
				if (!m_code.EmitLdrImm12(1, HOST_VU, VuOffset(offsetof(VURegs, branchpc))) ||
					!m_code.EmitStrImm12(1, HOST_VU, ViOffset(REG_TPC)) ||
					!m_code.EmitLdrbImm12(2, HOST_VU, VuOffset(offsetof(VURegs, takedelaybranch))) ||
					!m_code.EmitCmpImm32(2, 0))
				{
					return false;
				}
				const size_t skip_tdb = m_code.EmitBranchPlaceholder(Condition::EQ);
				if (skip_tdb == static_cast<size_t>(-1))
					return false;

				if (!m_code.EmitMovImm8(0, 1) ||
					!m_code.EmitStrImm12(0, HOST_VU, branch) ||
					!m_code.EmitLdrImm12(1, HOST_VU, VuOffset(offsetof(VURegs, delaybranchpc))) ||
					!m_code.EmitStrImm12(1, HOST_VU, VuOffset(offsetof(VURegs, branchpc))) ||
					!m_code.EmitMovImm8(2, 0) ||
					!m_code.EmitStrbImm12(2, HOST_VU, VuOffset(offsetof(VURegs, takedelaybranch))))
				{
					return false;
				}

				const size_t end = m_code.Size();
				return m_code.PatchBranch(skip_all, end, Condition::EQ) &&
					m_code.PatchBranch(skip_resolve, end, Condition::NE) &&
					m_code.PatchBranch(skip_tdb, end, Condition::EQ);
			}

			// Loads &VF[reg] into r0 and copies the quadword between VURegs
			// and the stack frame hazard slots.
			bool EmitVfCopyToStack(unsigned vf_reg, u32 stack_offset)
			{
				return EmitLoadVfQuad(0, vf_reg) &&
					m_code.EmitAddImm32(1, SP, stack_offset) &&
					m_code.EmitVst1Q32(0, 1);
			}

			bool EmitVfCopyFromStack(unsigned vf_reg, u32 stack_offset)
			{
				return m_code.EmitAddImm32(1, SP, stack_offset) &&
					m_code.EmitVld1Q32(0, 1) &&
					EmitStoreVfQuad(0, vf_reg);
			}

			bool EmitPair(u32 pair_index)
			{
				const PairPlan& plan = m_pairs[pair_index];
				// microVU's paired old-value swap and D/T exits are state joins with
				// canonical observers. Keep those uncommon pairs entirely outside the
				// block-local mapping instead of inventing path-specific cache states.
				const bool suspend_vector_cache = !m_vu0_memory_map &&
					(plan.vf_backup_reg != 0 || plan.dflag || plan.tflag);
				if (suspend_vector_cache)
				{
					if (!EmitFlushVectorCache())
						return false;
					m_vector_cache_suspended = true;
				}

				if (!EmitBudgetCheckAndCycleIncrement(pair_index))
					return false;
#if defined(VITASX2_QEMU_VALIDATION)
				// Count only admitted blocks. A linked target can reach its body and
				// reject on the pair-zero budget check without executing any VU pair.
				if (pair_index == 0 && !EmitQemuLocalFmacPipelineCounters())
					return false;
#endif

				// E flag decode, compile-time: VU->ebit = 2.
				if (plan.ebit)
				{
					if (!m_code.EmitMovImm8(0, 2) ||
						!m_code.EmitStrImm12(0, HOST_VU, VuOffset(offsetof(VURegs, ebit))))
					{
						return false;
					}
				}
				if (m_vu0_memory_map && plan.mflag && !EmitInlineVu0MFlag())
					return false;
				if (plan.dflag && !(m_vu0_memory_map ?
					EmitInlineDtFlag(0x4u, 0x2u, INTC_VU0) :
					EmitInlineDtFlag(0x400u, 0x200u, INTC_VU1)))
				{
					return false;
				}
				if (plan.tflag && !(m_vu0_memory_map ?
					EmitInlineDtFlag(0x8u, 0x4u, INTC_VU0) :
					EmitInlineDtFlag(0x800u, 0x400u, INTC_VU1)))
				{
					return false;
				}

				// Stall tests in _vu1Exec() order: upper, lower, pipes.
				const bool elide_upper_canonical_fmac =
					CanElideCanonicalFmacStallTest(pair_index, plan.uregs);
				const bool lower_has_canonical_fmac_test =
					plan.lregs.pipe == VUPIPE_FMAC || plan.lregs.pipe == VUPIPE_FDIV ||
					plan.lregs.pipe == VUPIPE_EFU;
				const bool elide_lower_canonical_fmac = lower_has_canonical_fmac_test &&
					CanElideCanonicalFmacStallTest(pair_index, plan.lregs);
				_VURegsNum merged_pipe_wait_fmac_reads{};
				// _vuTestFDIVStalls()/_vuTestEFUStalls() begin with the same canonical
				// FMAC dependency test as the adjacent upper arm. No pipe publication
				// occurs between them. Their source union can therefore share the lower
				// walk; the private tests in between are also monotone max operations,
				// so moving the upper contribution after them preserves the exact cycle.
				const bool merge_upper_fmac_into_lower_pipe_wait =
					plan.test_upper_stalls && !elide_upper_canonical_fmac &&
					plan.upper_fmac_stall_test_inline &&
					plan.test_lower_stalls && !elide_lower_canonical_fmac &&
					(plan.lower_fdiv_stall_test_inline ||
						plan.lower_efu_stall_test_inline) &&
					MergeFmacStallReadSets(plan.uregs, plan.lregs,
						&merged_pipe_wait_fmac_reads);
#if defined(VITASX2_QEMU_VALIDATION)
				if (((plan.test_upper_stalls && elide_upper_canonical_fmac) ||
						(plan.test_lower_stalls && elide_lower_canonical_fmac)) &&
					!EmitQemuCanonicalFmacStallTestElisionCounter())
				{
					return false;
				}
				if (plan.test_upper_stalls && elide_upper_canonical_fmac &&
					plan.test_lower_stalls && elide_lower_canonical_fmac &&
					!EmitQemuCanonicalFmacStallTestElisionCounter())
				{
					return false;
				}
#endif
				if (!merge_upper_fmac_into_lower_pipe_wait &&
					plan.test_upper_stalls && !elide_upper_canonical_fmac &&
					plan.upper_fmac_stall_test_inline)
				{
					if (!EmitInlineFmacStallTestPreservingCycleResidency(
						m_pairs[pair_index].uregs, true))
						return false;
				}
				else if (!merge_upper_fmac_into_lower_pipe_wait &&
					plan.test_upper_stalls && !elide_upper_canonical_fmac &&
					!EmitCallHelperRegs(reinterpret_cast<const void*>(&_vuTestUpperStalls), &m_pairs[pair_index].uregs))
				{
					return false;
				}
				if (plan.test_upper_stalls && !EmitLocalFmacStallTest(pair_index, plan.uregs))
					return false;
				if (plan.test_lower_stalls && !EmitLocalFmacStallTest(pair_index, plan.lregs))
					return false;
				if (plan.test_lower_stalls && !elide_lower_canonical_fmac &&
					plan.lower_fmac_stall_test_inline)
				{
					if (!EmitInlineFmacStallTestPreservingCycleResidency(
						m_pairs[pair_index].lregs, false))
						return false;
				}
				else if (plan.test_lower_stalls && plan.lower_fdiv_stall_test_inline)
				{
					if (!EmitPublishResidentCycle() ||
						!EmitInlineLowerFdivStallTest(
							merge_upper_fmac_into_lower_pipe_wait ?
								merged_pipe_wait_fmac_reads : m_pairs[pair_index].lregs,
							elide_lower_canonical_fmac) ||
						!EmitResyncResidentCycle())
						return false;
				}
				else if (plan.test_lower_stalls && plan.lower_efu_stall_test_inline)
				{
					if (!EmitPublishResidentCycle() ||
						!EmitInlineLowerEfuStallTest(
							merge_upper_fmac_into_lower_pipe_wait ?
								merged_pipe_wait_fmac_reads : m_pairs[pair_index].lregs,
							elide_lower_canonical_fmac) ||
						!EmitResyncResidentCycle())
						return false;
				}
				else if (plan.test_lower_stalls && plan.lower_branch_stall_test_inline)
				{
					if (!EmitPublishResidentCycle() ||
						!EmitInlineLowerBranchStallTest(m_pairs[pair_index].lregs) ||
						!EmitResyncResidentCycle())
						return false;
				}
				else if (plan.test_lower_stalls && !elide_lower_canonical_fmac &&
					!EmitCallHelperRegs(reinterpret_cast<const void*>(&_vuTestLowerStalls), &m_pairs[pair_index].lregs))
				{
					return false;
				}
				if (plan.test_pipes_fast_guard)
				{
					if (!(plan.defer_nop_pipe_test ?
						EmitDeferredNopPipeTest(m_plan.deferred_fmac_flags) :
						EmitTestPipesFastGuard(m_plan.deferred_fmac_flags)))
						return false;
				}
				else if (!EmitCallHelper(reinterpret_cast<const void*>(&_vuTestPipes)))
				{
					return false;
				}
				if (!EmitRetireLocalFmacEntries(pair_index))
					return false;

				const bool local_fmac_commit = m_plan.local_fmac_pipeline &&
					pair_index >= LOCAL_FMAC_WARMUP_PAIRS && plan.fmac_pipe;
				// D/T/E completion can synchronously flush or expose canonical queue
				// metadata before the interpreter's pair-tail write-position update.
				// Keep those rare seams in their original order; ordinary pairs can
				// consume the index already loaded by EmitInlineCommitFmacPipe().
				const bool advance_fmac_writepos_early = plan.fmac_pipe &&
					!local_fmac_commit && !plan.ebit && !plan.ebit_tail &&
					!plan.dflag && !plan.tflag;
				m_pending_local_fmac_entry = nullptr;
				m_capture_pending_local_fmac_flags = false;
				if (local_fmac_commit)
				{
					for (LocalFmacEntry& candidate : m_local_fmac_entries)
					{
						if (!candidate.active)
						{
							m_pending_local_fmac_entry = &candidate;
							break;
						}
					}
					if (!m_pending_local_fmac_entry)
						return false;
					m_pending_local_fmac_entry->pair_index = pair_index;
					m_pending_local_fmac_entry->producer_flags_captured = false;
					m_pending_local_fmac_entry->sticky_only_accumulated = false;
					m_capture_pending_local_fmac_flags =
						CanSnapshotLocalFmacFlagsAtProducer(plan);
				}

				if (!EmitViBackupUpdate(pair_index))
					return false;

				// Hazard backup of the upper target the lower op reads.
				if (plan.vf_backup_reg != 0 &&
					!EmitVfCopyToStack(plan.vf_backup_reg, 0))
				{
					return false;
				}
				if (plan.vi_clip_backup &&
					!m_code.EmitLdrImm12(HOST_CLIP_OLD, HOST_VU, ViOffset(REG_CLIP_FLAG)))
				{
					return false;
				}

				if (plan.exec_upper && plan.upper_nop_inline &&
					!EmitInlineUpperNop())
				{
					return false;
				}
				else if (plan.exec_upper && plan.upper_unary_inline &&
					!EmitInlineUpperUnary(plan.upper, static_cast<VUInterpFast::UpperFastKind>(plan.upper_kind)))
				{
					return false;
				}
				else if (plan.exec_upper && plan.upper_addsub_inline &&
					!EmitInlineUpperAddSub(plan.upper,
						static_cast<VUInterpFast::UpperFastKind>(plan.upper_kind),
						plan.mac_flag_result_required))
				{
					return false;
				}
				else if (plan.exec_upper && plan.upper_mul_inline &&
					!EmitInlineUpperMul(plan.upper,
						static_cast<VUInterpFast::UpperFastKind>(plan.upper_kind),
						plan.mac_flag_result_required))
				{
					return false;
				}
				else if (plan.exec_upper && plan.upper_maddmsub_inline &&
					!EmitInlineUpperMaddMsub(plan.upper,
						static_cast<VUInterpFast::UpperFastKind>(plan.upper_kind),
						plan.mac_flag_result_required))
				{
					return false;
				}
				else if (plan.exec_upper && plan.upper_outer_inline &&
					!EmitInlineUpperOuter(plan.upper, static_cast<VUInterpFast::UpperFastKind>(plan.upper_kind)))
				{
					return false;
				}
				else if (plan.exec_upper && plan.upper_clip_inline &&
					!EmitInlineUpperClip(plan.upper))
				{
					return false;
				}
				else if (plan.exec_upper && plan.upper_minmax_inline &&
					!EmitInlineUpperMinMax(plan.upper, static_cast<VUInterpFast::UpperFastKind>(plan.upper_kind)))
				{
					return false;
				}
				else if (plan.exec_upper && !plan.upper_unary_inline && !plan.upper_clip_inline &&
					!plan.upper_minmax_inline && !plan.upper_addsub_inline && !plan.upper_mul_inline &&
					!plan.upper_maddmsub_inline && !plan.upper_outer_inline && !plan.upper_nop_inline)
				{
					return false;
				}

				if (plan.shape == PairShape::IBit)
				{
					// PCSX2 owner: _vu1Exec()'s `VU->VI[REG_I].UL = ptr[0]`,
					// after the upper op so it still reads the previous I.
					if (!m_code.EmitMovImm32(0, plan.lower) ||
						!m_code.EmitStrImm12(0, HOST_VU, ViOffset(REG_I)))
					{
						return false;
					}
				}

				if (plan.vf_backup_reg != 0)
				{
					// _VFc = VF[reg]; VF[reg] = _VF (pre-upper value).
					if (!EmitVfCopyToStack(plan.vf_backup_reg, 16) ||
						!EmitVfCopyFromStack(plan.vf_backup_reg, 0))
					{
						return false;
					}
				}
				if (plan.vi_clip_backup)
				{
					if (!m_code.EmitLdrImm12(HOST_CLIP_NEW, HOST_VU, ViOffset(REG_CLIP_FLAG)) ||
						!m_code.EmitStrImm12(HOST_CLIP_OLD, HOST_VU, ViOffset(REG_CLIP_FLAG)))
					{
						return false;
					}
				}

				if (plan.exec_lower && plan.lower_ialu_inline &&
					!EmitInlineLowerIalu(plan.lower, static_cast<VUInterpFast::LowerFastKind>(plan.lower_kind)))
				{
					return false;
				}
				else if (plan.exec_lower && plan.lower_flag_inline &&
					!EmitInlineLowerFlag(plan.lower, static_cast<VUInterpFast::LowerFastKind>(plan.lower_kind)))
				{
					return false;
				}
				else if (plan.exec_lower && plan.lower_move_inline &&
					!EmitInlineLowerMove(plan.lower, static_cast<VUInterpFast::LowerFastKind>(plan.lower_kind)))
				{
					return false;
				}
				else if (plan.exec_lower && plan.lower_lsu_inline &&
					!EmitInlineLowerLsu(plan.lower, static_cast<VUInterpFast::LowerFastKind>(plan.lower_kind)))
				{
					return false;
				}
				else if (plan.exec_lower && plan.lower_control_inline &&
					!EmitInlineLowerControl(plan.lower, static_cast<VUInterpFast::LowerFastKind>(plan.lower_kind)))
				{
					return false;
				}
				else if (plan.exec_lower && plan.lower_branch_inline &&
					!EmitInlineLowerBranch(plan.lower, plan.pc + 8,
						static_cast<VUInterpFast::LowerFastKind>(plan.lower_kind)))
				{
					return false;
				}
				else if (plan.exec_lower && plan.lower_fdiv_inline &&
					!EmitInlineLowerFdiv(plan.lower, static_cast<VUInterpFast::LowerFastKind>(plan.lower_kind)))
				{
					return false;
				}
				else if (plan.exec_lower && plan.lower_efu_inline &&
					!EmitInlineLowerEfu(plan.lower, static_cast<VUInterpFast::LowerFastKind>(plan.lower_kind)))
				{
					return false;
				}
				else if (plan.exec_lower && plan.lower_xgkick_inline &&
					(!EmitInlineLowerXgkick(plan.lower) || !EmitMarkResidentPipeActivity()))
				{
					return false;
				}
				else if (plan.exec_lower && !plan.lower_ialu_inline && !plan.lower_flag_inline &&
					!plan.lower_move_inline && !plan.lower_lsu_inline && !plan.lower_control_inline &&
					!plan.lower_branch_inline && !plan.lower_fdiv_inline && !plan.lower_efu_inline &&
					!plan.lower_xgkick_inline)
				{
					return false;
				}

				if (plan.vf_backup_reg != 0 &&
					!EmitVfCopyFromStack(plan.vf_backup_reg, 16))
				{
					return false;
				}
				if (plan.vi_clip_backup &&
					!m_code.EmitStrImm12(HOST_CLIP_NEW, HOST_VU, ViOffset(REG_CLIP_FLAG)))
				{
					return false;
				}

				// Step tail, in _vu1Exec() order.
				if (local_fmac_commit && !EmitCommitLocalFmac(pair_index, plan))
				{
					return false;
				}
				if (plan.fmac_pipe && !local_fmac_commit &&
					(!EmitInlineCommitFmacPipe(plan, advance_fmac_writepos_early) ||
					 !EmitMarkResidentPipeActivity(true)))
				{
					return false;
				}
				if (plan.add_lower_stalls && plan.lregs.pipe != VUPIPE_FMAC && plan.lower_stall_inline)
				{
					if (!EmitInlineAddLowerStalls(plan) || !EmitMarkResidentPipeActivity())
						return false;
				}
				else if (plan.add_lower_stalls && plan.lregs.pipe != VUPIPE_FMAC &&
					(!EmitCallHelperRegs(reinterpret_cast<const void*>(&_vuAddLowerStalls),
						&m_pairs[pair_index].lregs) || !EmitMarkResidentPipeActivity()))
				{
					return false;
				}

				if (plan.branch_tail && !EmitBranchTail(plan.pc + 8))
					return false;

				size_t skip_static_ebit = static_cast<size_t>(-1);
				if ((plan.dflag || plan.tflag) &&
					!EmitRuntimeDtEbitFinish(&skip_static_ebit))
				{
					return false;
				}

				if (plan.ebit_tail)
				{
					// The ebit countdown value is statically known on every
					// step of the window (entry contract: ebit == 0).
					if (!m_code.EmitMovImm8(0, static_cast<u8>(plan.ebit_store)) ||
						!m_code.EmitStrImm12(0, HOST_VU, VuOffset(offsetof(VURegs, ebit))))
					{
						return false;
					}
					if (plan.ebit_store == 0 &&
						(!EmitFlushVectorCache() || !EmitInlineEbitFinish()))
					{
						return false;
					}
				}
				if (skip_static_ebit != static_cast<size_t>(-1) &&
					!m_code.PatchBranch(skip_static_ebit, m_code.Size()))
				{
					return false;
				}

				if (plan.fmac_pipe && !local_fmac_commit &&
					!advance_fmac_writepos_early)
				{
					const u16 writepos = VuOffset(offsetof(VURegs, fmacwritepos));
					if (!m_code.EmitLdrImm12(0, HOST_VU, writepos) ||
						!m_code.EmitAddImm8(0, 0, 1) ||
						!m_code.EmitAndImm32(0, 0, 3) ||
						!m_code.EmitStrImm12(0, HOST_VU, writepos))
					{
						return false;
					}
				}

				m_vector_cache_suspended = false;
				return true;
			}

			CodeBuffer& m_code;
			const BlockPlan& m_plan;
			PairPlan* m_pairs;
			u32 m_mem_mask = VU1_MEMMASK;
			bool m_vu0_memory_map = false;
			bool m_resident_cycle = false;
			bool m_resident_cycle_high = false;
			bool m_vector_cache_suspended = false;
			VectorCacheMode m_vector_cache_mode = VectorCacheMode::Disabled;
			std::vector<VectorAccessEvent>* m_vector_accesses = nullptr;
			size_t m_vector_access_cursor = 0;
			std::array<VectorCacheSlot, VU_VECTOR_CACHE_SLOTS> m_vector_cache{};
			VectorCacheStats m_vector_cache_stats{};
			std::array<u8, VU_VECTOR_CACHE_ACC + 1> m_normalized_vector_lanes{};
			u32 m_normalized_operand_quad_bypasses = 0;
			u32 m_normalization_instructions_removed = 0;
			u32 m_single_d_broadcast_operands = 0;
			std::array<LocalFmacEntry, LOCAL_FMAC_SLOT_COUNT> m_local_fmac_entries{};
			LocalFmacEntry* m_pending_local_fmac_entry = nullptr;
			LocalFmacEntry* m_latest_working_fmac_entry = nullptr;
			bool m_capture_pending_local_fmac_flags = false;
			bool m_dead_fmac_sticky_pending = false;
			// True once the vuDouble() bit-select constant quads (Q8-Q10) have been
			// materialized in this block. Q8-Q15 are exclusive to normalize scratch
			// plus Q11's mutually exclusive MAC weights/dead-FMAC accumulator (operation temporaries use
			// Q0-Q3 and the vector cache owns Q4-Q7), so once loaded the
			// constants survive across pairs and later FMAC/EFU ops in the same
			// straight-line block skip re-materializing them. Fresh per block via
			// the per-block BlockCompiler construction.
			bool m_norm_consts_ready = false;
			bool m_norm_maxf_ready = false;
			bool m_full_mac_weights_ready = false;
			std::vector<size_t> m_xgkick_norm_preserve_calls;
			std::vector<size_t> m_test_pipes_fast_guard_calls;
			std::vector<size_t> m_deferred_test_pipes_fast_guard_calls;
			std::vector<EmptyPipeNopSlowPath> m_empty_pipe_nop_slow_paths;
			std::vector<BudgetExit> m_budget_exits;
			std::array<Vu1DirectLinkSlot, MAX_DIRECT_LINK_SLOTS> m_direct_links{};
			LinkedEntryOffsets m_linked_entries{};
		};

		// ------------------------------------------------------------------
		// Block cache.
		// ------------------------------------------------------------------

		struct CachedBlock
		{
			CodeBuffer code;
			const void* entry = nullptr;
			const void* linked_entry = nullptr;
			const void* deferred_fmac_linked_entry = nullptr;
			const void* resident_pipe_linked_entry = nullptr;
			const void* resident_pipe_deferred_fmac_linked_entry = nullptr;
			size_t code_size = 0;
			u32 start_pc = 0;
			u32 pair_count = 0;
			u32 micro_size = 0;
			u32 micro_hash = 0;
			bool entry_branch_tail = false;
			bool entry_ebit_tail = false;
			bool continues_logical_block_if_busy = false;
			bool vector_cache_frame = false;
			bool deferred_fmac_flags = false;
			bool resident_pipe_activity = false;
			// PCSX2 owner: x86/microVU.h::microProgram. VU1 direct links are
			// valid only within the immutable MicroMem version which owns both
			// source and target blocks. VU0 does not use program versions.
			Vu1Program* program = nullptr;
			std::array<Vu1DirectLinkSlot, MAX_DIRECT_LINK_SLOTS> direct_links{};
			// Generated code embeds pointers into this array (stall-helper
			// _VURegsNum arguments), so it must stay stable for the lifetime
			// of the block.
			std::unique_ptr<PairPlan[]> pairs;
			std::unique_ptr<u8[]> micro_bytes;
		};

		constexpr u32 VU1_PAIR_SLOTS = VU1_PROGSIZE / 8;
		// PCSX2 owner: HostMemoryMap sizes the x86 mVU1rec cache; the Vita
		// cache holds one 16 KiB microprogram's worth of expanded pair code.
		constexpr size_t VU1_CODE_CACHE_CAPACITY = 4 * 1024 * 1024;
		constexpr size_t CODE_ALIGNMENT = 32;
		using Vu1BlockMap = std::array<CachedBlock*, VU1_PAIR_SLOTS>;

		CachedBlock* const BLOCK_UNCOMPILABLE = reinterpret_cast<CachedBlock*>(1);

		// PCSX2 owner: x86/microVU.h::{microProgram,microProgManager} and
		// x86/microVU.cpp::{mVUclear,mVUsearchProg}. A microprogram version owns
		// its complete compiled-block maps. MicroMem writes discard only the quick
		// selection; selecting identical contents recovers every block and direct
		// link at once instead of reconstructing the flat maps from CPU1 misses.
		struct Vu1Program
		{
			struct MicroRange
			{
				u32 start;
				u32 end;
			};

			Vu1BlockMap map{};
			Vu1BlockMap branch_map{};
			Vu1BlockMap ebit_map{};
			Vu1BlockMap branch_ebit_map{};
			std::array<u8, VU1_PROGSIZE> micro{};
			std::vector<MicroRange> ranges;
			u32 primary_start_pc = 0;
			u32 mapped_blocks = 0;
			bool uses_vector_cache = false;
		};

		struct Vu1State
		{
			std::array<std::vector<Vu1Program*>, VU1_PAIR_SLOTS> programs_by_start{};
			std::array<Vu1Program*, VU1_PAIR_SLOTS> quick_programs{};
			std::vector<std::unique_ptr<Vu1Program>> programs;
			Vu1Program* active_program = nullptr;
			std::vector<std::unique_ptr<CachedBlock>> blocks;
			u8* code_cache = nullptr;
			size_t code_cache_used = 0;
			size_t code_cache_capacity = 0;
			bool map_populated = false; // any block or uncompilable marker present
			Vu1ProviderStats stats;
		};

		Vu1State s_vu1;

		struct Vu1CompileKey
		{
			u32 pc;
			bool entry_branch_tail;
			bool entry_ebit_tail;
		};

		constexpr u32 VU1_COMPILE_REQUEST_WORDS = VU1_PAIR_SLOTS / 64;
		constexpr u32 VU1_COMPILE_REQUEST_VARIANTS = 4;
		std::array<std::atomic<u64>,
			VU1_COMPILE_REQUEST_WORDS * VU1_COMPILE_REQUEST_VARIANTS>
			s_vu1_compile_requests{};
		std::atomic<u64> s_vu1_compile_requests_total{0};
		std::atomic<u64> s_vu1_completed_programs{0};
		std::atomic<u64> s_vu1_published_executed_blocks{0};
		std::atomic<u64> s_vu1_published_executed_pairs{0};
		std::atomic<u64> s_vu1_published_interpreter_steps{0};

		u32 Vu1CompileVariant(bool entry_branch_tail, bool entry_ebit_tail)
		{
			return static_cast<u32>(entry_branch_tail) |
				(static_cast<u32>(entry_ebit_tail) << 1);
		}

		void RequestVu1Compile(u32 pc, bool entry_branch_tail,
			bool entry_ebit_tail)
		{
			if ((pc & 7) != 0 || pc > VU1_PROGMASK)
				return;
			const u32 slot = pc / 8;
			const u32 word = Vu1CompileVariant(entry_branch_tail,
				entry_ebit_tail) * VU1_COMPILE_REQUEST_WORDS + slot / 64;
			s_vu1_compile_requests[word].fetch_or(1ull << (slot & 63),
				std::memory_order_release);
			if (VitaPerformanceTelemetry::IsEnabled())
			{
				s_vu1_compile_requests_total.fetch_add(
					1, std::memory_order_relaxed);
			}
		}

		bool HasVu1CompileRequests()
		{
			for (const std::atomic<u64>& requests : s_vu1_compile_requests)
			{
				if (requests.load(std::memory_order_acquire) != 0)
					return true;
			}
			return false;
		}

		void DrainVu1CompileRequests(std::vector<Vu1CompileKey>* keys)
		{
			for (u32 word = 0; word < s_vu1_compile_requests.size(); word++)
			{
				u64 bits = s_vu1_compile_requests[word].exchange(0,
					std::memory_order_acq_rel);
				const u32 variant = word / VU1_COMPILE_REQUEST_WORDS;
				const u32 slot_base = (word % VU1_COMPILE_REQUEST_WORDS) * 64;
				while (bits != 0)
				{
					const u32 bit = static_cast<u32>(__builtin_ctzll(bits));
					keys->push_back({(slot_base + bit) * 8,
						(variant & 1) != 0, (variant & 2) != 0});
					bits &= bits - 1;
				}
			}
		}

		void ClearVu1CompileRequests()
		{
			for (std::atomic<u64>& requests : s_vu1_compile_requests)
				requests.store(0, std::memory_order_relaxed);
		}

		constexpr u32 VU0_PAIR_SLOTS = VU0_PROGSIZE / 8;
		// VU0 has a 4 KiB microprogram space; keep its first native cache
		// separate from VU1 so VU0 macro/micro invalidation cannot disturb PATH1
		// VU1 blocks.
		constexpr size_t VU0_CODE_CACHE_CAPACITY = 1024 * 1024;

		struct Vu0State
		{
			std::array<CachedBlock*, VU0_PAIR_SLOTS> map{};
			std::array<CachedBlock*, VU0_PAIR_SLOTS> branch_map{};
			std::array<CachedBlock*, VU0_PAIR_SLOTS> ebit_map{};
			std::array<CachedBlock*, VU0_PAIR_SLOTS> branch_ebit_map{};
			std::vector<std::unique_ptr<CachedBlock>> blocks;
			u8* code_cache = nullptr;
			size_t code_cache_used = 0;
			size_t code_cache_capacity = 0;
			bool map_populated = false;
			Vu1ProviderStats stats;
		};

		Vu0State s_vu0;
		u64 s_vu0_execute_calls = 0;

		u32 HashVu1MicroBytes(const u8* bytes, u32 size)
		{
			u32 hash = 2166136261u;
			for (u32 i = 0; i < size; i++)
			{
				hash ^= bytes[i];
				hash *= 16777619u;
			}
			return hash;
		}

		bool CachedBlockMatchesMicro(const CachedBlock& block, const u8* bytes,
			u32 size, u32 hash)
		{
			return block.micro_size == size &&
				block.micro_hash == hash &&
				std::memcmp(block.micro_bytes.get(), bytes, size) == 0;
		}

		CachedBlock* FindCachedVu0Block(u32 start_pc, bool entry_branch_tail,
			bool entry_ebit_tail, const u8* bytes, u32 size, u32 hash)
		{
			for (const std::unique_ptr<CachedBlock>& block : s_vu0.blocks)
			{
				if (block->start_pc == start_pc &&
					block->entry_branch_tail == entry_branch_tail &&
					block->entry_ebit_tail == entry_ebit_tail &&
					CachedBlockMatchesMicro(*block, bytes, size, hash))
				{
					return block.get();
				}
			}
			return nullptr;
		}

		bool EnsureVu1CodeCache()
		{
			if (s_vu1.code_cache)
				return true;

			s_vu1.code_cache = static_cast<u8*>(VitaVM::AllocJitMemory(VU1_CODE_CACHE_CAPACITY));
			s_vu1.code_cache_capacity = s_vu1.code_cache ? VU1_CODE_CACHE_CAPACITY : 0;
			s_vu1.code_cache_used = 0;
			s_vu1.stats.code_cache_capacity = s_vu1.code_cache_capacity;
			return s_vu1.code_cache != nullptr;
		}

		bool EnsureVu0CodeCache()
		{
			if (s_vu0.code_cache)
				return true;

			s_vu0.code_cache = static_cast<u8*>(VitaVM::AllocJitMemory(VU0_CODE_CACHE_CAPACITY));
			s_vu0.code_cache_capacity = s_vu0.code_cache ? VU0_CODE_CACHE_CAPACITY : 0;
			s_vu0.code_cache_used = 0;
			s_vu0.stats.code_cache_capacity = s_vu0.code_cache_capacity;
			return s_vu0.code_cache != nullptr;
		}

		void DropVu1Blocks()
		{
			if (!s_vu1.map_populated && s_vu1.programs.empty())
				return;
			s_vu1.active_program = nullptr;
			s_vu1.quick_programs.fill(nullptr);
			s_vu1.blocks.clear();
			for (std::vector<Vu1Program*>& programs : s_vu1.programs_by_start)
				programs.clear();
			s_vu1.programs.clear();
			s_vu1.code_cache_used = 0;
			s_vu1.map_populated = false;
		}

		void DropVu0Blocks()
		{
			if (!s_vu0.map_populated)
				return;
			s_vu0.map.fill(nullptr);
			s_vu0.branch_map.fill(nullptr);
			s_vu0.ebit_map.fill(nullptr);
			s_vu0.branch_ebit_map.fill(nullptr);
			s_vu0.blocks.clear();
			s_vu0.code_cache_used = 0;
			s_vu0.map_populated = false;
		}

		void ClearVu0BlockMapSlots(u32 first_slot, u32 last_slot)
		{
			for (u32 slot = first_slot; slot < last_slot; slot++)
			{
				s_vu0.map[slot] = nullptr;
				s_vu0.branch_map[slot] = nullptr;
				s_vu0.ebit_map[slot] = nullptr;
				s_vu0.branch_ebit_map[slot] = nullptr;
			}
		}

		std::array<CachedBlock*, VU0_PAIR_SLOTS>& SelectVu0BlockMap(bool entry_branch_tail, bool entry_ebit_tail)
		{
			if (entry_branch_tail)
				return entry_ebit_tail ? s_vu0.branch_ebit_map : s_vu0.branch_map;
			return entry_ebit_tail ? s_vu0.ebit_map : s_vu0.map;
		}

		Vu1BlockMap& SelectBlockMap(Vu1Program& program, bool entry_branch_tail,
			bool entry_ebit_tail)
		{
			if (entry_branch_tail)
				return entry_ebit_tail ? program.branch_ebit_map : program.branch_map;
			return entry_ebit_tail ? program.ebit_map : program.map;
		}

		void RegisterVu1ProgramStart(Vu1Program& program, u32 start_pc)
		{
			std::vector<Vu1Program*>& programs = s_vu1.programs_by_start[start_pc / 8];
			for (Vu1Program* candidate : programs)
			{
				if (candidate == &program)
					return;
			}
			programs.insert(programs.begin(), &program);
		}

		Vu1Program* CreateVu1Program(u32 start_pc)
		{
			auto program = std::make_unique<Vu1Program>();
			program->primary_start_pc = start_pc;
			Vu1Program* result = program.get();
			s_vu1.programs.push_back(std::move(program));
			RegisterVu1ProgramStart(*result, start_pc);
			s_vu1.quick_programs[start_pc / 8] = result;
			s_vu1.active_program = result;
			s_vu1.stats.program_versions_created++;
			return result;
		}

		bool Vu1ProgramMatchesMicro(const Vu1Program& program)
		{
			// PCSX2's release owner deliberately sets doWholeProgCompare=false.
			// mVUcmpProg() compares only ranges which have actually contributed to
			// generated blocks, allowing games to rewrite unrelated MicroMem without
			// multiplying program versions.
			if (program.ranges.empty())
				return false;
			for (const Vu1Program::MicroRange& range : program.ranges)
			{
				if (std::memcmp(program.micro.data() + range.start,
						VU1.Micro + range.start, range.end - range.start) != 0)
				{
					return false;
				}
			}
			return true;
		}

		void CacheVu1ProgramRange(Vu1Program& program, u32 start, u32 size)
		{
			const u32 end = std::min(start + size, VU1_PROGSIZE);
			pxAssert(start < end);
			std::memcpy(program.micro.data() + start, VU1.Micro + start, end - start);

			Vu1Program::MicroRange merged{start, end};
			auto it = program.ranges.begin();
			while (it != program.ranges.end() && it->end < merged.start)
				++it;
			while (it != program.ranges.end() && it->start <= merged.end)
			{
				merged.start = std::min(merged.start, it->start);
				merged.end = std::max(merged.end, it->end);
				it = program.ranges.erase(it);
			}
			program.ranges.insert(it, merged);
		}

		Vu1Program* ActivateVu1Program(u32 start_pc)
		{
			pxAssert((start_pc & 7) == 0 && start_pc <= VU1_PROGMASK);
			if (s_vu1.active_program)
			{
				// The Vita provider's generated block ABI consumes architectural
				// pipeline state at runtime rather than keying x86 microBlock variants
				// by mVU's microRegInfo. One immutable content version can therefore
				// own all external MSCAL entry maps without duplicating its blocks.
				RegisterVu1ProgramStart(*s_vu1.active_program, start_pc);
				s_vu1.quick_programs[start_pc / 8] = s_vu1.active_program;
				return s_vu1.active_program;
			}

			if (Vu1Program* quick = s_vu1.quick_programs[start_pc / 8])
			{
				s_vu1.active_program = quick;
				return quick;
			}

			std::vector<Vu1Program*>& programs = s_vu1.programs_by_start[start_pc / 8];
			for (size_t i = 0; i < programs.size(); i++)
			{
				Vu1Program* candidate = programs[i];
				if (!Vu1ProgramMatchesMicro(*candidate))
					continue;

				if (i != 0)
				{
					programs.erase(programs.begin() + i);
					programs.insert(programs.begin(), candidate);
				}
				s_vu1.quick_programs[start_pc / 8] = candidate;
				s_vu1.active_program = candidate;
				s_vu1.stats.content_cache_hits++;
				s_vu1.stats.program_version_cache_hits++;
				s_vu1.stats.program_block_maps_reused += candidate->mapped_blocks;
				return candidate;
			}

			return CreateVu1Program(start_pc);
		}

			bool DirectLinkTargetsBlock(const Vu1DirectLinkSlot& link, const CachedBlock& target)
			{
				return link.valid &&
					(!link.runtime_observed || link.observed_target) &&
					link.target_pc == target.start_pc &&
					link.target_branch_tail == target.entry_branch_tail &&
					link.target_ebit_tail == target.entry_ebit_tail;
			}

			bool DirectLinkFramesCompatible(const CachedBlock& source, const CachedBlock& target)
			{
				return source.vector_cache_frame == target.vector_cache_frame;
			}

			const void* DirectLinkTargetEntry(const CachedBlock& source,
				const CachedBlock& target)
			{
				if (source.resident_pipe_activity && target.resident_pipe_activity)
				{
					return source.deferred_fmac_flags ?
						target.resident_pipe_deferred_fmac_linked_entry :
						target.resident_pipe_linked_entry;
				}
				return source.deferred_fmac_flags ?
					target.deferred_fmac_linked_entry : target.linked_entry;
			}

			const void* DirectLinkTargetEntry(bool source_deferred_fmac_flags,
				bool source_resident_pipe_activity, const CachedBlock& target)
			{
				if (source_resident_pipe_activity && target.resident_pipe_activity)
				{
					return source_deferred_fmac_flags ?
						target.resident_pipe_deferred_fmac_linked_entry :
						target.resident_pipe_linked_entry;
				}
				return source_deferred_fmac_flags ?
					target.deferred_fmac_linked_entry : target.linked_entry;
			}

			bool PatchVu1DirectLink(CachedBlock& source, Vu1DirectLinkSlot& link,
				const void* target)
			{
				if (!link.valid ||
					link.unlinked_fallback_offset == static_cast<size_t>(-1) ||
					link.target_offset == static_cast<size_t>(-1) ||
					link.fallback_offset == static_cast<size_t>(-1))
				{
					return false;
				}
				if (link.patched_target == target)
					return true;

				if (target)
				{
					if (!source.code.PatchBranchToAddress(link.target_offset, target) ||
						!source.code.PatchNop(link.unlinked_fallback_offset))
					{
						return false;
					}
				}
				else
				{
					if (!source.code.PatchBranch(link.unlinked_fallback_offset, link.fallback_offset) ||
						!source.code.PatchBranch(link.target_offset, link.fallback_offset))
					{
						return false;
					}
				}
				if (link.guard_tpc && link.guard_tpc_offset != static_cast<size_t>(-1) &&
					!source.code.PatchMovImm32(link.guard_tpc_offset, 1,
						target ? link.guard_tpc_value : 0))
				{
					return false;
				}
				if (!source.code.Flush())
					return false;

				link.patched_target = target;
				if (target)
				{
					s_vu1.stats.direct_link_patches++;
					if (link.runtime_observed)
						s_vu1.stats.direct_link_runtime_patches++;
				}
				return true;
			}

			bool PatchVu1RuntimeLinkSlotPointers(CachedBlock& block)
			{
				bool patched = false;
				for (Vu1DirectLinkSlot& link : block.direct_links)
				{
					link.owner = &block;
					if (!link.valid || !link.runtime_observed ||
						link.helper_slot_offset == static_cast<size_t>(-1))
					{
						continue;
					}

					if (!block.code.PatchMovImm32(link.helper_slot_offset, 1,
							static_cast<u32>(reinterpret_cast<uptr>(&link))))
					{
						return false;
					}
					patched = true;
				}
				return !patched || block.code.Flush();
			}

			void PatchVu1LinksForCurrentMap(CachedBlock& source)
			{
				if (!source.program)
					return;
				for (Vu1DirectLinkSlot& link : source.direct_links)
				{
					if (!link.valid || (link.runtime_observed && !link.observed_target))
						continue;

					Vu1BlockMap& target_map = SelectBlockMap(*source.program,
						link.target_branch_tail, link.target_ebit_tail);
					CachedBlock* target = target_map[link.target_pc / 8];
					if (target && target != BLOCK_UNCOMPILABLE &&
						DirectLinkFramesCompatible(source, *target))
						PatchVu1DirectLink(source, link,
							DirectLinkTargetEntry(source, *target));
					else if (link.patched_target)
						PatchVu1DirectLink(source, link, nullptr);
				}
			}

			void PatchVu1IncomingLinks(CachedBlock& target)
			{
				for (const std::unique_ptr<CachedBlock>& source : s_vu1.blocks)
				{
					if (!source || source->program != target.program)
						continue;

				for (Vu1DirectLinkSlot& link : source->direct_links)
				{
					if (DirectLinkTargetsBlock(link, target) &&
						DirectLinkFramesCompatible(*source, target))
						PatchVu1DirectLink(*source, link,
							DirectLinkTargetEntry(*source, target));
					else if (DirectLinkTargetsBlock(link, target) && link.patched_target)
						PatchVu1DirectLink(*source, link, nullptr);
				}
			}
		}

		void PatchVu1LinksForBlock(CachedBlock& block)
		{
			PatchVu1IncomingLinks(block);
			PatchVu1LinksForCurrentMap(block);
		}

			Vu1DirectLinkSlot* SelectVu1RuntimeObservedSlot(Vu1DirectLinkSlot* seed,
				u32 target_pc, bool entry_branch_tail, bool entry_ebit_tail)
			{
				if (!seed || !seed->runtime_observed || !seed->owner)
					return nullptr;

				Vu1DirectLinkSlot* first_unobserved = nullptr;
				Vu1DirectLinkSlot* replacement = nullptr;
				for (Vu1DirectLinkSlot& link : seed->owner->direct_links)
				{
					if (!link.valid || !link.runtime_observed ||
						link.target_branch_tail != entry_branch_tail ||
						link.target_ebit_tail != entry_ebit_tail)
					{
						continue;
					}

					if (link.observed_target && link.target_pc == target_pc)
						return &link;
					if (!link.observed_target && !first_unobserved)
						first_unobserved = &link;
					if (!replacement)
						replacement = &link;
				}

				return first_unobserved ? first_unobserved : replacement;
			}

			CachedBlock* CompileVu1Block(u32 start_pc, bool entry_branch_tail, bool entry_ebit_tail)
			{
				Vu1Program* program = s_vu1.active_program;
				if (!program)
					return nullptr;
			BlockPlan plan;
				if (!ScanBlock(VU1.Micro, 1, VU1_PROGSIZE, VU1_PROGMASK, false,
						start_pc, entry_branch_tail, entry_ebit_tail, &plan))
				{
					// A retained fallback marker is part of this immutable program
					// version too.  Record the rejected pair so a later MicroMem
					// rewrite cannot inherit BLOCK_UNCOMPILABLE from different code.
					CacheVu1ProgramRange(*program, start_pc, 8);
					s_vu1.stats.scan_rejects++;
					return nullptr;
				}
			const u32 micro_size = plan.pair_count * 8;
			const u8* const micro_bytes = &VU1.Micro[start_pc];
			const u32 micro_hash = HashVu1MicroBytes(micro_bytes, micro_size);

			if (!EnsureVu1CodeCache())
			{
				CacheVu1ProgramRange(*program, start_pc, micro_size);
				s_vu1.stats.compile_failures++;
				return nullptr;
			}

			auto block = std::make_unique<CachedBlock>();
			block->start_pc = start_pc;
			block->pair_count = plan.pair_count;
			block->micro_size = micro_size;
			block->micro_hash = micro_hash;
			block->entry_branch_tail = entry_branch_tail;
			block->entry_ebit_tail = entry_ebit_tail;
			block->continues_logical_block_if_busy = plan.continues_logical_block_if_busy;
			block->program = program;
			block->pairs = std::make_unique<PairPlan[]>(plan.pair_count);
			std::copy_n(plan.pairs.begin(), plan.pair_count, block->pairs.get());
			block->micro_bytes = std::make_unique<u8[]>(micro_size);
			std::memcpy(block->micro_bytes.get(), micro_bytes, micro_size);

			for (int attempt = 0; attempt < 2; attempt++)
			{
				const size_t offset = (s_vu1.code_cache_used + (CODE_ALIGNMENT - 1)) & ~(CODE_ALIGNMENT - 1);
				CodeBuffer code;
				if (offset < s_vu1.code_cache_capacity &&
					code.Attach(s_vu1.code_cache + offset, s_vu1.code_cache_capacity - offset))
				{
					std::array<Vu1DirectLinkSlot, MAX_DIRECT_LINK_SLOTS> chosen_direct_links{};
					LinkedEntryOffsets chosen_linked_entries{};
					BlockCompiler::VectorCacheStats chosen_vector_stats{};
					u32 chosen_normalized_operand_quad_bypasses = 0;
					u32 chosen_normalization_instructions_removed = 0;
					u32 chosen_single_d_broadcast_operands = 0;
					bool compiled = false;
					bool vector_cache_candidate = false;
					bool vector_cache_selected = false;
					u64 vector_cache_baseline_instructions = 0;
					u64 vector_cache_selected_instructions = 0;
					u64 vector_cache_canonical_bytes_removed = 0;
					std::vector<BlockCompiler::VectorAccessEvent> vector_accesses;
					vector_accesses.reserve(static_cast<size_t>(plan.pair_count) * 4 + 16);

					const auto canonical_vector_bytes = [](const BlockCompiler::VectorCacheStats& stats) {
						return static_cast<u64>(stats.vf_word_loads + stats.vf_word_stores +
							stats.acc_word_loads + stats.acc_word_stores) * 4u +
							static_cast<u64>(stats.vf_quad_loads + stats.vf_quad_stores +
								stats.acc_quad_loads + stats.acc_quad_stores) * 16u;
					};

					{
						BlockCompiler baseline(code, plan, block->pairs.get(), VU1_MEMMASK,
							BlockCompiler::VectorCacheMode::Trace, &vector_accesses);
						if (baseline.Compile())
						{
							const size_t baseline_size = code.Size();
							const auto baseline_links = baseline.DirectLinks();
							const LinkedEntryOffsets baseline_linked_entries =
								baseline.LinkedEntries();
							const BlockCompiler::VectorCacheStats baseline_stats =
								baseline.GetVectorCacheStats();
							const bool outer_vector_frame_already_required =
								program->uses_vector_cache;
							const BlockCompiler::VectorCacheOpportunity opportunity =
								BlockCompiler::AnalyzeVectorCacheOpportunity(&vector_accesses,
									outer_vector_frame_already_required);

							if (!opportunity.profitable)
							{
								chosen_direct_links = baseline_links;
								chosen_linked_entries = baseline_linked_entries;
								chosen_vector_stats = baseline_stats;
								chosen_normalized_operand_quad_bypasses =
									baseline.GetNormalizedOperandQuadBypasses();
								chosen_normalization_instructions_removed =
									baseline.GetNormalizationInstructionsRemoved();
								chosen_single_d_broadcast_operands =
									baseline.GetSingleDBroadcastOperands();
								compiled = true;
							}
							else
							{
								vector_cache_candidate = true;
								code.Reset();
								BlockCompiler cached(code, plan, block->pairs.get(), VU1_MEMMASK,
									BlockCompiler::VectorCacheMode::Enabled, &vector_accesses);
								if (cached.Compile())
								{
									const size_t cached_size = code.Size();
									const BlockCompiler::VectorCacheStats cached_stats =
										cached.GetVectorCacheStats();
									const u64 baseline_bytes = canonical_vector_bytes(baseline_stats);
									const u64 cached_bytes = canonical_vector_bytes(cached_stats);
									const u64 vector_frame_traffic_bytes =
										outer_vector_frame_already_required ? 0u : 128u;
									// The two complete compiles are the final profitability oracle: a
									// cached block may not grow and must reduce canonical vector traffic.
									// Charge D8-D15's outer frame only to the first selected block in the
									// program; every later block executes inside that already-paid frame.
									vector_cache_selected =
										baseline_size >= cached_size &&
										baseline_bytes > cached_bytes + vector_frame_traffic_bytes;
									if (vector_cache_selected)
									{
										chosen_direct_links = cached.DirectLinks();
										chosen_linked_entries = cached.LinkedEntries();
										chosen_vector_stats = cached_stats;
										chosen_normalized_operand_quad_bypasses =
											cached.GetNormalizedOperandQuadBypasses();
										chosen_normalization_instructions_removed =
											cached.GetNormalizationInstructionsRemoved();
										chosen_single_d_broadcast_operands =
											cached.GetSingleDBroadcastOperands();
										vector_cache_baseline_instructions = baseline_size / sizeof(u32);
										vector_cache_selected_instructions = cached_size / sizeof(u32);
										vector_cache_canonical_bytes_removed = baseline_bytes - cached_bytes;
										compiled = true;
									}
								}

								if (!vector_cache_selected)
								{
									code.Reset();
									BlockCompiler fallback(code, plan, block->pairs.get(), VU1_MEMMASK);
									if (fallback.Compile())
									{
										chosen_direct_links = fallback.DirectLinks();
										chosen_linked_entries = fallback.LinkedEntries();
										chosen_vector_stats = fallback.GetVectorCacheStats();
										chosen_normalized_operand_quad_bypasses =
											fallback.GetNormalizedOperandQuadBypasses();
										chosen_normalization_instructions_removed =
											fallback.GetNormalizationInstructionsRemoved();
										chosen_single_d_broadcast_operands =
											fallback.GetSingleDBroadcastOperands();
										compiled = true;
									}
								}
							}
						}
					}

					if (compiled && code.Flush())
					{
						block->direct_links = chosen_direct_links;
						block->vector_cache_frame = false;
						block->deferred_fmac_flags = plan.deferred_fmac_flags;
						block->resident_pipe_activity = plan.resident_pipe_activity;
						block->code = std::move(code);
						block->entry = block->code.EntryPoint();
						block->linked_entry = static_cast<const u8*>(block->entry) +
							chosen_linked_entries.normal;
						if (chosen_linked_entries.deferred_fmac != static_cast<size_t>(-1))
						{
							block->deferred_fmac_linked_entry =
								static_cast<const u8*>(block->entry) +
								chosen_linked_entries.deferred_fmac;
						}
						if (chosen_linked_entries.resident_pipe != static_cast<size_t>(-1))
						{
							block->resident_pipe_linked_entry =
								static_cast<const u8*>(block->entry) +
								chosen_linked_entries.resident_pipe;
						}
						if (chosen_linked_entries.resident_pipe_deferred_fmac !=
							static_cast<size_t>(-1))
						{
							block->resident_pipe_deferred_fmac_linked_entry =
								static_cast<const u8*>(block->entry) +
								chosen_linked_entries.resident_pipe_deferred_fmac;
						}
						block->code_size = block->code.Size();
						CodeBuffer::GeneratedCodeStats generated;
						if (VitaPerformanceTelemetry::IsEnabled())
							generated = block->code.AnalyzeGeneratedCode();
						if (!PatchVu1RuntimeLinkSlotPointers(*block))
							break;
						s_vu1.code_cache_used = offset + block->code_size;
						s_vu1.stats.code_cache_used = s_vu1.code_cache_used;
						s_vu1.stats.compiled_blocks++;
						s_vu1.stats.compiled_pairs += plan.pair_count;
						s_vu1.stats.generated_host_instructions +=
							generated.host_instructions;
						s_vu1.stats.generated_host_load_instructions +=
							generated.host_load_instructions;
						s_vu1.stats.generated_host_store_instructions +=
							generated.host_store_instructions;
						s_vu1.stats.generated_helper_call_instructions +=
							generated.helper_call_instructions;
						s_vu1.stats.generated_state_load_instructions +=
							generated.state_load_instructions;
						s_vu1.stats.generated_state_store_instructions +=
							generated.state_store_instructions;
						s_vu1.stats.normalized_operand_quads_bypassed +=
							chosen_normalized_operand_quad_bypasses;
						s_vu1.stats.normalization_instructions_removed +=
							chosen_normalization_instructions_removed;
						s_vu1.stats.single_d_broadcast_operands +=
							chosen_single_d_broadcast_operands;
						s_vu1.stats.canonical_fmac_stall_tests_elided +=
							plan.canonical_fmac_stall_tests_elided;
						if (plan.local_fmac_pipeline)
						{
							s_vu1.stats.local_fmac_pipeline_blocks++;
							s_vu1.stats.local_fmac_pipeline_pairs +=
								plan.local_fmac_pipeline_pairs;
							s_vu1.stats.local_fmac_cycle_snapshot_elision_pairs +=
								plan.local_fmac_cycle_snapshot_elision_pairs;
							s_vu1.stats.local_fmac_producer_snapshot_pairs +=
								plan.local_fmac_producer_snapshot_pairs;
							s_vu1.stats.local_fmac_clip_snapshot_elisions +=
								plan.local_fmac_clip_snapshot_elisions;
							s_vu1.stats.mac_flag_classification_elisions +=
								plan.mac_flag_classification_elisions;
							// Every liveness-selected producer captures its local flag
							// instance. Reusing a resident working MAC costs one extra
							// load, so STATUS-only classification removes seven A32
							// instructions there and eight on the canonical local path.
							s_vu1.stats.mac_flag_classification_minimum_instructions_removed +=
								static_cast<u64>(plan.mac_flag_classification_elisions) *
								(plan.resident_working_fmac_flags ? 7u : 8u);
						}
						if (plan.resident_working_fmac_flags)
						{
							s_vu1.stats.resident_working_fmac_flag_blocks++;
							s_vu1.stats.resident_working_fmac_flag_producers +=
								plan.local_fmac_producer_snapshot_pairs;
							// The canonical path publishes MAC and STATUS after every
							// producer. Residency replaces those 2*N stores with the two
							// stores at the sole block-seam publication.
							s_vu1.stats.resident_working_fmac_state_stores_removed +=
								static_cast<u64>(plan.local_fmac_producer_snapshot_pairs) * 2u - 2u;
						}
						if (plan.resident_pipe_activity)
						{
							s_vu1.stats.resident_pipe_activity_blocks++;
							s_vu1.stats.resident_pipe_activity_pairs += plan.pair_count;
						}
						if (plan.deferred_fmac_flags)
						{
							s_vu1.stats.deferred_fmac_flag_blocks++;
							s_vu1.stats.deferred_fmac_flag_retirements +=
								plan.deferred_fmac_flag_retirements;
							s_vu1.stats.deferred_fmac_compact_retirements +=
								plan.deferred_fmac_compact_retirements;
						}
						const BlockCompiler::VectorCacheStats& vector_stats = chosen_vector_stats;
						if (vector_cache_candidate)
							s_vu1.stats.vector_cache_candidate_blocks++;
						if (vector_cache_selected)
						{
							s_vu1.stats.vector_cache_selected_blocks++;
							s_vu1.stats.vector_cache_baseline_instructions +=
								vector_cache_baseline_instructions;
							s_vu1.stats.vector_cache_selected_instructions +=
								vector_cache_selected_instructions;
							s_vu1.stats.vector_cache_instructions_removed +=
								vector_cache_baseline_instructions - vector_cache_selected_instructions;
							s_vu1.stats.vector_cache_canonical_bytes_removed +=
								vector_cache_canonical_bytes_removed;
							program->uses_vector_cache = true;
						}
						else if (vector_cache_candidate)
						{
							s_vu1.stats.vector_cache_rejected_blocks++;
						}
						s_vu1.stats.vector_cache_hits += vector_stats.hits;
						s_vu1.stats.vector_cache_misses += vector_stats.misses;
						s_vu1.stats.vector_cache_evictions += vector_stats.evictions;
						s_vu1.stats.vector_cache_writebacks += vector_stats.writebacks;
						s_vu1.stats.vector_cache_acc_hits += vector_stats.acc_hits;
						s_vu1.stats.vector_cache_scalar_invalidations +=
							vector_stats.scalar_invalidations;
						s_vu1.stats.vector_cache_preloads += vector_stats.preloads;
						s_vu1.stats.uncached_vector_loads += vector_stats.uncached_loads;
						s_vu1.stats.uncached_vector_stores += vector_stats.uncached_stores;
						s_vu1.stats.canonical_vf_word_loads += vector_stats.vf_word_loads;
						s_vu1.stats.canonical_vf_word_stores += vector_stats.vf_word_stores;
						s_vu1.stats.canonical_vf_quad_loads += vector_stats.vf_quad_loads;
						s_vu1.stats.canonical_vf_quad_stores += vector_stats.vf_quad_stores;
						s_vu1.stats.canonical_acc_word_loads += vector_stats.acc_word_loads;
						s_vu1.stats.canonical_acc_word_stores += vector_stats.acc_word_stores;
						s_vu1.stats.canonical_acc_quad_loads += vector_stats.acc_quad_loads;
						s_vu1.stats.canonical_acc_quad_stores += vector_stats.acc_quad_stores;
						if (plan.entry_branch_tail)
						{
							s_vu1.stats.branch_continuation_blocks++;
							s_vu1.stats.branch_continuation_pairs += plan.pair_count;
						}
						if (plan.entry_ebit_tail)
						{
							s_vu1.stats.ebit_continuation_blocks++;
							s_vu1.stats.ebit_continuation_pairs += plan.pair_count;
						}
						s_vu1.stats.test_pipes_fast_guard_pairs += plan.test_pipes_fast_guard_pairs;
						s_vu1.stats.nop_pipe_test_defer_pairs += plan.nop_pipe_test_defer_pairs;
						s_vu1.stats.empty_pipe_nop_batch_runs +=
							plan.empty_pipe_nop_batch_runs;
						s_vu1.stats.empty_pipe_nop_batch_pairs +=
							plan.empty_pipe_nop_batch_pairs;
						s_vu1.stats.fmac_clear_inline_pairs += plan.fmac_clear_inline_pairs;
						s_vu1.stats.upper_fmac_stall_test_inline_pairs += plan.upper_fmac_stall_test_inline_pairs;
						s_vu1.stats.lower_fmac_stall_test_inline_pairs += plan.lower_fmac_stall_test_inline_pairs;
						s_vu1.stats.upper_fmac_stall_inline_pairs += plan.upper_fmac_stall_inline_pairs;
						s_vu1.stats.lower_fmac_stall_inline_pairs += plan.lower_fmac_stall_inline_pairs;
						s_vu1.stats.upper_addsub_inline_pairs += plan.upper_addsub_inline_pairs;
						s_vu1.stats.upper_mul_inline_pairs += plan.upper_mul_inline_pairs;
						s_vu1.stats.upper_maddmsub_inline_pairs += plan.upper_maddmsub_inline_pairs;
						s_vu1.stats.upper_outer_inline_pairs += plan.upper_outer_inline_pairs;
						s_vu1.stats.upper_nop_inline_pairs += plan.upper_nop_inline_pairs;
						s_vu1.stats.upper_unary_inline_pairs += plan.upper_unary_inline_pairs;
						s_vu1.stats.upper_clip_inline_pairs += plan.upper_clip_inline_pairs;
						s_vu1.stats.upper_minmax_inline_pairs += plan.upper_minmax_inline_pairs;
						s_vu1.stats.lower_ialu_inline_pairs += plan.lower_ialu_inline_pairs;
						s_vu1.stats.lower_flag_inline_pairs += plan.lower_flag_inline_pairs;
						s_vu1.stats.lower_move_inline_pairs += plan.lower_move_inline_pairs;
						s_vu1.stats.lower_lsu_inline_pairs += plan.lower_lsu_inline_pairs;
						s_vu1.stats.lower_control_inline_pairs += plan.lower_control_inline_pairs;
						s_vu1.stats.lower_branch_inline_pairs += plan.lower_branch_inline_pairs;
						s_vu1.stats.lower_fdiv_inline_pairs += plan.lower_fdiv_inline_pairs;
						s_vu1.stats.lower_efu_inline_pairs += plan.lower_efu_inline_pairs;
						s_vu1.stats.lower_xgkick_inline_pairs += plan.lower_xgkick_inline_pairs;
						s_vu1.stats.lower_fdiv_stall_test_inline_pairs += plan.lower_fdiv_stall_test_inline_pairs;
						s_vu1.stats.lower_efu_stall_test_inline_pairs += plan.lower_efu_stall_test_inline_pairs;
						s_vu1.stats.lower_branch_stall_test_inline_pairs += plan.lower_branch_stall_test_inline_pairs;
						s_vu1.stats.lower_stall_inline_pairs += plan.lower_stall_inline_pairs;
						s_vu1.stats.vi_backup_update_elided_pairs +=
							plan.vi_backup_update_elided_pairs;
						s_vu1.stats.vi_backup_zero_store_pairs +=
							plan.vi_backup_zero_store_pairs;
						s_vu1.stats.dt_flag_inline_pairs += plan.dt_flag_inline_pairs;
						if (plan.resident_cycle)
						{
							s_vu1.stats.cycle_resident_blocks++;
							s_vu1.stats.cycle_resident_pairs += plan.pair_count;
							s_vu1.stats.cycle_resident_pair_instructions_removed +=
								plan.pair_count;
						}
						if (plan.resident_cycle_high)
						{
							s_vu1.stats.cycle_high_resident_blocks++;
							s_vu1.stats.cycle_high_resident_pairs += plan.pair_count;
							s_vu1.stats.cycle_high_resident_hot_branches_removed +=
								plan.pair_count;
						}

						CachedBlock* result = block.get();
						s_vu1.blocks.push_back(std::move(block));
						Vu1BlockMap& map = SelectBlockMap(*program,
							entry_branch_tail, entry_ebit_tail);
						map[start_pc / 8] = result;
						CacheVu1ProgramRange(*program, start_pc, micro_size);
						program->mapped_blocks++;
						s_vu1.map_populated = true;
						PatchVu1LinksForBlock(*result);
						return result;
					}
				}

				// Whole-cache pressure reset, PCSX2 owner:
				// x86/microVU.cpp::mVUreset() on cache exhaustion.
				const u32 program_start_pc = program->primary_start_pc;
				DropVu1Blocks();
				s_vu1.stats.code_cache_resets++;
				program = CreateVu1Program(program_start_pc);
				block->program = program;
			}

			// Keep a failed-emission sentinel versioned by the complete analyzed
			// range for the same reason as a scan rejection above.
			CacheVu1ProgramRange(*program, start_pc, micro_size);
			s_vu1.stats.compile_failures++;
			return nullptr;
		}

		CachedBlock* LookupOrCompileVu1Block(u32 start_pc, bool entry_branch_tail, bool entry_ebit_tail)
		{
			if (!s_vu1.active_program)
			{
				if (THREAD_VU1)
				{
					RequestVu1Compile(start_pc, entry_branch_tail, entry_ebit_tail);
					return nullptr;
				}
				ActivateVu1Program(start_pc);
			}
			Vu1BlockMap& map = SelectBlockMap(*s_vu1.active_program,
				entry_branch_tail, entry_ebit_tail);
			CachedBlock* block = map[start_pc / 8];
			if (block == BLOCK_UNCOMPILABLE)
				return nullptr;
			if (block)
				return block;
			if (THREAD_VU1)
			{
				RequestVu1Compile(start_pc, entry_branch_tail, entry_ebit_tail);
				return nullptr;
			}

			block = CompileVu1Block(start_pc, entry_branch_tail, entry_ebit_tail);
			if (!block)
			{
				SelectBlockMap(*s_vu1.active_program, entry_branch_tail,
					entry_ebit_tail)[start_pc / 8] = BLOCK_UNCOMPILABLE;
				s_vu1.map_populated = true;
			}
			return block;
		}

		CachedBlock* CompileVu0Block(u32 start_pc, bool entry_branch_tail, bool entry_ebit_tail)
		{
			BlockPlan plan;
			if (!ScanBlock(VU0.Micro, 0, VU0_PROGSIZE, VU0_PROGMASK, true,
					start_pc, entry_branch_tail, entry_ebit_tail, &plan))
			{
				s_vu0.stats.scan_rejects++;
				return nullptr;
			}
			plan.direct_link_tail = false;

			const u32 micro_size = plan.pair_count * 8;
			const u8* const micro_bytes = &VU0.Micro[start_pc];
			const u32 micro_hash = HashVu1MicroBytes(micro_bytes, micro_size);
			if (CachedBlock* cached = FindCachedVu0Block(start_pc, entry_branch_tail,
					entry_ebit_tail, micro_bytes, micro_size, micro_hash))
			{
				SelectVu0BlockMap(entry_branch_tail, entry_ebit_tail)[start_pc / 8] = cached;
				s_vu0.map_populated = true;
				s_vu0.stats.content_cache_hits++;
				return cached;
			}

			if (!EnsureVu0CodeCache())
			{
				s_vu0.stats.compile_failures++;
				return nullptr;
			}

			auto block = std::make_unique<CachedBlock>();
			block->start_pc = start_pc;
			block->pair_count = plan.pair_count;
			block->micro_size = micro_size;
			block->micro_hash = micro_hash;
			block->entry_branch_tail = entry_branch_tail;
			block->entry_ebit_tail = entry_ebit_tail;
			block->continues_logical_block_if_busy = plan.continues_logical_block_if_busy;
			block->pairs = std::make_unique<PairPlan[]>(plan.pair_count);
			std::copy_n(plan.pairs.begin(), plan.pair_count, block->pairs.get());
			block->micro_bytes = std::make_unique<u8[]>(micro_size);
			std::memcpy(block->micro_bytes.get(), micro_bytes, micro_size);

			for (int attempt = 0; attempt < 2; attempt++)
			{
				const size_t offset = (s_vu0.code_cache_used + (CODE_ALIGNMENT - 1)) & ~(CODE_ALIGNMENT - 1);
				CodeBuffer code;
				if (offset < s_vu0.code_cache_capacity &&
					code.Attach(s_vu0.code_cache + offset, s_vu0.code_cache_capacity - offset))
				{
					BlockCompiler compiler(code, plan, block->pairs.get(), VU0_MEMMASK);
					if (compiler.Compile() && code.Flush())
					{
						block->code = std::move(code);
						block->entry = block->code.EntryPoint();
						block->code_size = block->code.Size();
						CodeBuffer::GeneratedCodeStats generated;
						if (VitaPerformanceTelemetry::IsEnabled())
							generated = block->code.AnalyzeGeneratedCode();
						s_vu0.code_cache_used = offset + block->code_size;
						s_vu0.stats.code_cache_used = s_vu0.code_cache_used;
						s_vu0.stats.compiled_blocks++;
						s_vu0.stats.compiled_pairs += plan.pair_count;
						s_vu0.stats.generated_host_instructions +=
							generated.host_instructions;
						s_vu0.stats.generated_host_load_instructions +=
							generated.host_load_instructions;
						s_vu0.stats.generated_host_store_instructions +=
							generated.host_store_instructions;
						s_vu0.stats.generated_helper_call_instructions +=
							generated.helper_call_instructions;
						s_vu0.stats.generated_state_load_instructions +=
							generated.state_load_instructions;
						s_vu0.stats.generated_state_store_instructions +=
							generated.state_store_instructions;
						s_vu0.stats.normalized_operand_quads_bypassed +=
							compiler.GetNormalizedOperandQuadBypasses();
						s_vu0.stats.normalization_instructions_removed +=
							compiler.GetNormalizationInstructionsRemoved();
						s_vu0.stats.single_d_broadcast_operands +=
							compiler.GetSingleDBroadcastOperands();
						if (plan.entry_branch_tail)
						{
							s_vu0.stats.branch_continuation_blocks++;
							s_vu0.stats.branch_continuation_pairs += plan.pair_count;
						}
						if (plan.entry_ebit_tail)
						{
							s_vu0.stats.ebit_continuation_blocks++;
							s_vu0.stats.ebit_continuation_pairs += plan.pair_count;
						}
						s_vu0.stats.test_pipes_fast_guard_pairs += plan.test_pipes_fast_guard_pairs;
						s_vu0.stats.nop_pipe_test_defer_pairs += plan.nop_pipe_test_defer_pairs;
						s_vu0.stats.empty_pipe_nop_batch_runs +=
							plan.empty_pipe_nop_batch_runs;
						s_vu0.stats.empty_pipe_nop_batch_pairs +=
							plan.empty_pipe_nop_batch_pairs;
						s_vu0.stats.fmac_clear_inline_pairs += plan.fmac_clear_inline_pairs;
						s_vu0.stats.upper_fmac_stall_test_inline_pairs += plan.upper_fmac_stall_test_inline_pairs;
						s_vu0.stats.lower_fmac_stall_test_inline_pairs += plan.lower_fmac_stall_test_inline_pairs;
						s_vu0.stats.upper_fmac_stall_inline_pairs += plan.upper_fmac_stall_inline_pairs;
						s_vu0.stats.lower_fmac_stall_inline_pairs += plan.lower_fmac_stall_inline_pairs;
						s_vu0.stats.upper_addsub_inline_pairs += plan.upper_addsub_inline_pairs;
						s_vu0.stats.upper_mul_inline_pairs += plan.upper_mul_inline_pairs;
						s_vu0.stats.upper_maddmsub_inline_pairs += plan.upper_maddmsub_inline_pairs;
						s_vu0.stats.upper_outer_inline_pairs += plan.upper_outer_inline_pairs;
						s_vu0.stats.upper_nop_inline_pairs += plan.upper_nop_inline_pairs;
						s_vu0.stats.upper_unary_inline_pairs += plan.upper_unary_inline_pairs;
						s_vu0.stats.upper_clip_inline_pairs += plan.upper_clip_inline_pairs;
						s_vu0.stats.upper_minmax_inline_pairs += plan.upper_minmax_inline_pairs;
						s_vu0.stats.lower_ialu_inline_pairs += plan.lower_ialu_inline_pairs;
						s_vu0.stats.lower_flag_inline_pairs += plan.lower_flag_inline_pairs;
						s_vu0.stats.lower_move_inline_pairs += plan.lower_move_inline_pairs;
						s_vu0.stats.lower_lsu_inline_pairs += plan.lower_lsu_inline_pairs;
						s_vu0.stats.lower_control_inline_pairs += plan.lower_control_inline_pairs;
						s_vu0.stats.lower_branch_inline_pairs += plan.lower_branch_inline_pairs;
						s_vu0.stats.lower_fdiv_inline_pairs += plan.lower_fdiv_inline_pairs;
						s_vu0.stats.lower_efu_inline_pairs += plan.lower_efu_inline_pairs;
						s_vu0.stats.lower_xgkick_inline_pairs += plan.lower_xgkick_inline_pairs;
						s_vu0.stats.lower_fdiv_stall_test_inline_pairs += plan.lower_fdiv_stall_test_inline_pairs;
						s_vu0.stats.lower_efu_stall_test_inline_pairs += plan.lower_efu_stall_test_inline_pairs;
						s_vu0.stats.lower_branch_stall_test_inline_pairs += plan.lower_branch_stall_test_inline_pairs;
						s_vu0.stats.lower_stall_inline_pairs += plan.lower_stall_inline_pairs;
						s_vu0.stats.vi_backup_update_elided_pairs +=
							plan.vi_backup_update_elided_pairs;
						s_vu0.stats.vi_backup_zero_store_pairs +=
							plan.vi_backup_zero_store_pairs;
						s_vu0.stats.dt_flag_inline_pairs += plan.dt_flag_inline_pairs;
						if (plan.resident_cycle)
						{
							s_vu0.stats.cycle_resident_blocks++;
							s_vu0.stats.cycle_resident_pairs += plan.pair_count;
							s_vu0.stats.cycle_resident_pair_instructions_removed +=
								plan.pair_count;
						}
						if (plan.resident_cycle_high)
						{
							s_vu0.stats.cycle_high_resident_blocks++;
							s_vu0.stats.cycle_high_resident_pairs += plan.pair_count;
							s_vu0.stats.cycle_high_resident_hot_branches_removed +=
								plan.pair_count;
						}

						CachedBlock* result = block.get();
						s_vu0.blocks.push_back(std::move(block));
						SelectVu0BlockMap(entry_branch_tail, entry_ebit_tail)[start_pc / 8] = result;
						s_vu0.map_populated = true;
						return result;
					}
				}

				// PCSX2 owner: x86/microVU.cpp::mVUreset() on cache exhaustion.
				DropVu0Blocks();
				s_vu0.stats.code_cache_resets++;
			}

			s_vu0.stats.compile_failures++;
			return nullptr;
		}

		CachedBlock* LookupOrCompileVu0Block(u32 start_pc, bool entry_branch_tail, bool entry_ebit_tail)
		{
			std::array<CachedBlock*, VU0_PAIR_SLOTS>& map = SelectVu0BlockMap(entry_branch_tail, entry_ebit_tail);
			CachedBlock* block = map[start_pc / 8];
			if (block == BLOCK_UNCOMPILABLE)
				return nullptr;
			if (block)
				return block;

			block = CompileVu0Block(start_pc, entry_branch_tail, entry_ebit_tail);
			if (!block)
			{
				map[start_pc / 8] = BLOCK_UNCOMPILABLE;
				s_vu0.map_populated = true;
			}
			return block;
		}

			const void* LookupVu1DirectLinkBlockCommon(VURegs* vu,
				Vu1DirectLinkSlot* runtime_link, bool source_vector_frame,
				bool source_deferred_fmac_flags, bool source_resident_pipe_activity)
			{
				if (!Vu1ProgramActive())
				{
				if (vu->branch == 1)
				{
					vu->VI[REG_TPC].UL = vu->branchpc;
					vu->branch = 0;
				}
				return nullptr;
				}

				vu->VI[REG_TPC].UL &= VU1_PROGMASK;
				const u32 target_pc = vu->VI[REG_TPC].UL;
				if ((vu->branch != 0 && vu->branch != 1) ||
					(vu->ebit != 0 && vu->ebit != 1) ||
					(target_pc & 7) != 0)
				{
					return nullptr;
				}

				const bool entry_branch_tail = vu->branch == 1;
				const bool entry_ebit_tail = vu->ebit == 1;
				Vu1Program* const program = runtime_link && runtime_link->owner ?
					runtime_link->owner->program : s_vu1.active_program;
				if (!program || program != s_vu1.active_program)
					return nullptr;
				Vu1BlockMap& map = SelectBlockMap(*program,
					entry_branch_tail, entry_ebit_tail);
				CachedBlock* block = map[target_pc / 8];
				if (!block || block == BLOCK_UNCOMPILABLE)
				{
					if (!block && THREAD_VU1)
						RequestVu1Compile(target_pc, entry_branch_tail, entry_ebit_tail);
					return nullptr;
				}
				if (block->vector_cache_frame != source_vector_frame ||
					(runtime_link && (!runtime_link->owner ||
						runtime_link->owner->vector_cache_frame != source_vector_frame ||
						runtime_link->owner->deferred_fmac_flags != source_deferred_fmac_flags ||
						runtime_link->owner->resident_pipe_activity !=
							source_resident_pipe_activity)))
				{
					return nullptr;
				}

				s_vu1.stats.direct_link_exits++;
				if (!THREAD_VU1)
				{
					if (Vu1DirectLinkSlot* observed_link = SelectVu1RuntimeObservedSlot(
							runtime_link, target_pc, entry_branch_tail, entry_ebit_tail))
					{
						const bool first_observation = !observed_link->observed_target;
						observed_link->target_pc = target_pc;
						observed_link->guard_tpc_value = target_pc;
						observed_link->observed_target = true;
						if (PatchVu1DirectLink(*observed_link->owner, *observed_link,
								DirectLinkTargetEntry(*observed_link->owner, *block)))
						{
							if (first_observation)
								s_vu1.stats.direct_link_runtime_observed_slots++;
						}
						else
						{
							observed_link->observed_target = false;
						}
					}
				}
				return DirectLinkTargetEntry(source_deferred_fmac_flags,
					source_resident_pipe_activity, *block);
			}

			const void* LookupVu1DirectLinkBlockScalar(VURegs* vu,
				Vu1DirectLinkSlot* runtime_link)
			{
				return LookupVu1DirectLinkBlockCommon(vu, runtime_link, false, false, false);
			}

			const void* LookupVu1DirectLinkBlockVector(VURegs* vu,
				Vu1DirectLinkSlot* runtime_link)
			{
				return LookupVu1DirectLinkBlockCommon(vu, runtime_link, true, false, false);
			}

			const void* LookupVu1DirectLinkBlockScalarResidentPipe(VURegs* vu,
				Vu1DirectLinkSlot* runtime_link)
			{
				return LookupVu1DirectLinkBlockCommon(vu, runtime_link, false, false, true);
			}

			const void* LookupVu1DirectLinkBlockVectorResidentPipe(VURegs* vu,
				Vu1DirectLinkSlot* runtime_link)
			{
				return LookupVu1DirectLinkBlockCommon(vu, runtime_link, true, false, true);
			}

			const void* LookupVu1DirectLinkBlockScalarDeferredFmac(VURegs* vu,
				Vu1DirectLinkSlot* runtime_link)
			{
				return LookupVu1DirectLinkBlockCommon(vu, runtime_link, false, true, false);
			}

			const void* LookupVu1DirectLinkBlockVectorDeferredFmac(VURegs* vu,
				Vu1DirectLinkSlot* runtime_link)
			{
				return LookupVu1DirectLinkBlockCommon(vu, runtime_link, true, true, false);
			}

			const void* LookupVu1DirectLinkBlockScalarDeferredFmacResidentPipe(VURegs* vu,
				Vu1DirectLinkSlot* runtime_link)
			{
				return LookupVu1DirectLinkBlockCommon(vu, runtime_link, false, true, true);
			}

			const void* LookupVu1DirectLinkBlockVectorDeferredFmacResidentPipe(VURegs* vu,
				Vu1DirectLinkSlot* runtime_link)
			{
				return LookupVu1DirectLinkBlockCommon(vu, runtime_link, true, true, true);
			}
	} // anonymous namespace

	bool Vu1ProgramNeedsPreparation(s32 vu_addr)
	{
		if (VitaPerformanceTelemetry::IsEnabled())
			s_vu1.stats.program_prepare_checks++;
		if (HasVu1CompileRequests())
			return true;
		if (!s_vu1.active_program)
			return true;

		// MSCNT resumes from the TPC owned by the MTVU worker.  CPU0's VU1
		// register shadow is not made current until WaitVU()/Get_MTVUChanges(),
		// so consulting it here can manufacture a compile barrier for a stale PC.
		// PCSX2 owner: MTVU.cpp::ExecuteVU()/Get_MTVUChanges() and
		// x86/microVU.cpp::mVUsearchProg().  The worker looks up the live TPC in
		// the active immutable program; a genuinely absent entry publishes a
		// compile request and takes the interpreter path for that execution.
		if (vu_addr == -1)
		{
			if (VitaPerformanceTelemetry::IsEnabled())
				s_vu1.stats.program_quick_cache_hits++;
			return false;
		}

		const u32 start_pc =
			(static_cast<u32>(vu_addr) & 0x7ffu) << 3;
		const bool needs_preparation =
			s_vu1.active_program->map[start_pc / 8] == nullptr;
		if (!needs_preparation && VitaPerformanceTelemetry::IsEnabled())
			s_vu1.stats.program_quick_cache_hits++;
		return needs_preparation;
	}

	void PrepareVu1Program(s32 vu_addr)
	{
		if (VitaPerformanceTelemetry::IsEnabled())
			s_vu1.stats.program_prepare_calls++;
		const u32 start_pc = (vu_addr == -1) ?
			((VU1.VI[REG_TPC].UL & 0x7ffu) << 3) :
			((static_cast<u32>(vu_addr) & 0x7ffu) << 3);
		ActivateVu1Program(start_pc);
		std::vector<Vu1CompileKey> queue;
		queue.reserve(32);
		queue.push_back({start_pc, false, false});
		DrainVu1CompileRequests(&queue);

		std::array<u64,
			VU1_COMPILE_REQUEST_WORDS * VU1_COMPILE_REQUEST_VARIANTS> visited{};
		for (size_t cursor = 0; cursor < queue.size(); cursor++)
		{
			const Vu1CompileKey key = queue[cursor];
			const u32 slot = key.pc / 8;
			const u32 visited_word = Vu1CompileVariant(key.entry_branch_tail,
				key.entry_ebit_tail) * VU1_COMPILE_REQUEST_WORDS + slot / 64;
			const u64 visited_bit = 1ull << (slot & 63);
			if (visited[visited_word] & visited_bit)
				continue;
			visited[visited_word] |= visited_bit;

			Vu1BlockMap& map = SelectBlockMap(*s_vu1.active_program,
				key.entry_branch_tail, key.entry_ebit_tail);
			CachedBlock* block = map[slot];
			if (block == BLOCK_UNCOMPILABLE)
				continue;
			if (!block)
			{
				block = CompileVu1Block(key.pc, key.entry_branch_tail,
					key.entry_ebit_tail);
				if (!block)
				{
					SelectBlockMap(*s_vu1.active_program,
						key.entry_branch_tail, key.entry_ebit_tail)[slot] =
						BLOCK_UNCOMPILABLE;
					s_vu1.map_populated = true;
					continue;
				}
			}

			for (const Vu1DirectLinkSlot& link : block->direct_links)
			{
				if (link.valid && !link.runtime_observed)
				{
					queue.push_back({link.target_pc, link.target_branch_tail,
						link.target_ebit_tail});
				}
			}
		}
	}

	static void UpdateNextBlockCyclesAtExecuteExit(VURegs& vu, u32 busy_mask,
		bool forced_program_exit)
	{
		// PCSX2 owner: x86/microVU_Compile.inl::mVUtestCycles() publishes
		// nextBlockCycles only for the VU sync hacks when the upcoming compiled
		// block cannot enter its cycle budget.  microVU_Branch.inl clears it at
		// architectural program exits.  With both hacks disabled the field stays
		// at the zero established by mVUreset().
		if (!EmuConfig.Gamefixes.VUSyncHack && !EmuConfig.Gamefixes.FullVU0SyncHack)
			return;

		const bool running = (busy_mask == 0x100u && THREAD_VU1) ?
			vu1Thread.IsProgramActive() :
			(VU0.VI[REG_VPU_STAT].UL & busy_mask) != 0;
		if (forced_program_exit || !running)
		{
			vu.nextBlockCycles = 0;
			return;
		}

		// A32 now owns microVU's default whole-logical-block admission rule, but
		// it does not yet retain the pipeline-specialized static span of the next
		// block. Preserve the interpreter-compatible estimate only for this
		// tracked sync-hack fallback; it must not leak into the ordinary contract.
		vu.nextBlockCycles = (vu.cycle - cpuRegs.cycle) + 1;
	}

	static void ClampVuCycleAfterAdmittedBlock(VURegs& vu, u64 start_cycle,
		u32 requested_cycles)
	{
		// PCSX2 owner: x86/microVU_Execute.inl::mVUcleanUp(). Generated microVU
		// may execute beyond the requested window to finish an admitted logical
		// block, but its public VU clock advances by
		// totalCycles - max(0, remainingCycles), never by the overshoot. A32 uses
		// VURegs::cycle as its private work clock too, so translate every live
		// absolute pipeline anchor with the public clock when clamping it.
		const u64 work_cycles = vu.cycle - start_cycle;
		if (work_cycles <= requested_cycles)
			return;

		const u64 overshoot = work_cycles - requested_cycles;
		vu.cycle -= overshoot;

		for (u32 offset = 0; offset < vu.fmaccount && offset < 4; offset++)
			vu.fmac[(vu.fmacreadpos + offset) & 3].sCycle -= overshoot;
		if (vu.fdiv.enable)
			vu.fdiv.sCycle -= overshoot;
		if (vu.efu.enable)
			vu.efu.sCycle -= overshoot;
		for (u32 offset = 0; offset < vu.ialucount && offset < 4; offset++)
			vu.ialu[(vu.ialureadpos + offset) & 3].sCycle -= overshoot;
		if (vu.xgkickenable)
			vu.xgkicklastcycle -= overshoot;
	}

	void ExecuteVu0Blocks(u32 cycles)
	{
		// PCSX2 owner: InterpVU0::Execute(). Vita keeps the same TPC
		// byte/index conversion, run-bit/M-bit exits, cycle budget, and VU0
		// cycle-rate speedhack tail; scan-proven body windows run as A32.
		const FPControlRegisterBackup fpcr_backup(EmuConfig.Cpu.VU0FPCR);

		VU0.VI[REG_TPC].UL <<= 3;
		VU0.flags &= ~VUFLAG_MFLAGSET;
		const u64 startcycles = VU0.cycle;
		const u64 limit = startcycles + cycles;
		const bool blocks_eligible = !Pcsx2Trace::IsVuTraceEnabled();
		const bool performance_telemetry_enabled =
			VitaPerformanceTelemetry::IsEnabled();

		bool admitted_logical_continuation = false;
		while (admitted_logical_continuation || (VU0.cycle - startcycles) < cycles)
		{
			if (!(VU0.VI[REG_VPU_STAT].UL & 0x1))
			{
				if (VU0.branch)
				{
					VU0.VI[REG_TPC].UL = VU0.branchpc;
					VU0.branch = 0;
				}
				break;
			}
			if (VU0.flags & VUFLAG_MFLAGSET)
				break;

			VU0.VI[REG_TPC].UL &= VU0_PROGMASK;
			if (blocks_eligible && (VU0.branch == 0 || VU0.branch == 1) &&
				(VU0.ebit == 0 || VU0.ebit == 1) &&
				(VU0.VI[REG_TPC].UL & 7) == 0)
			{
				const bool entry_branch_tail = VU0.branch == 1;
				const bool entry_ebit_tail = VU0.ebit == 1;
				if (CachedBlock* block = LookupOrCompileVu0Block(VU0.VI[REG_TPC].UL,
						entry_branch_tail, entry_ebit_tail))
				{
					const BlockFn fn = reinterpret_cast<BlockFn>(const_cast<void*>(block->entry));
					const u32 result = fn(&VU0, 0,
						static_cast<u32>(limit), static_cast<u32>(limit >> 32));
					const u32 executed = result & ~EXECUTED_PAIRS_LOGICAL_CONTINUATION;
					if (executed != 0)
					{
						if (performance_telemetry_enabled)
						{
							s_vu0.stats.executed_blocks++;
							s_vu0.stats.executed_pairs += executed;
						}
						admitted_logical_continuation =
							(result & EXECUTED_PAIRS_LOGICAL_CONTINUATION) != 0 &&
							(VU0.VI[REG_VPU_STAT].UL & 0x1) != 0 &&
							!(VU0.flags & VUFLAG_MFLAGSET);
						continue;
					}
				}
			}

			const bool resolving_admitted_branch = VU0.branch != 0;
			CpuIntVU0.Step();
			if (performance_telemetry_enabled)
				s_vu0.stats.interpreter_steps++;
			if (blocks_eligible)
			{
				// A decode fallback is still part of the already admitted natural
				// microVU block. Keep stepping until its branch/program boundary.
				admitted_logical_continuation =
					(VU0.VI[REG_VPU_STAT].UL & 0x1) != 0 &&
					!(VU0.flags & VUFLAG_MFLAGSET) && !resolving_admitted_branch;
			}
		}

		ClampVuCycleAfterAdmittedBlock(VU0, startcycles, cycles);
		VU0.VI[REG_TPC].UL >>= 3;

		if (EmuConfig.Speedhacks.EECycleRate != 0 &&
			(!EmuConfig.Gamefixes.VUSyncHack || EmuConfig.Speedhacks.EECycleRate < 0))
		{
			u64 cycle_change = VU0.cycle - startcycles;
			VU0.cycle -= cycle_change;
			const int cycle_rate = EmuConfig.Speedhacks.EECycleRate;
			const int cycle_case = (cycle_rate < static_cast<int>(cycle_change)) ?
				cycle_rate :
				static_cast<int>(cycle_change);
			switch (cycle_case)
			{
				case -3:
					cycle_change *= 2.0f;
					break;
				case -2:
					cycle_change *= 1.6666667f;
					break;
				case -1:
					cycle_change *= 1.3333333f;
					break;
				case 1:
					cycle_change /= 1.3f;
					break;
				case 2:
					cycle_change /= 1.8f;
					break;
				case 3:
					cycle_change /= 3.0f;
					break;
				default:
					break;
			}
			VU0.cycle += cycle_change;
		}

		UpdateNextBlockCyclesAtExecuteExit(VU0, 0x1,
			(VU0.flags & VUFLAG_MFLAGSET) != 0);
		if (VitaPerformanceTelemetry::IsEnabled())
			s_vu0_execute_calls++;
	}

	void InvalidateVu0Blocks(u32 addr, u32 size)
	{
		if (!s_vu0.map_populated || size == 0)
			return;

		// PCSX2 owner: x86/microVU.cpp::mVUclear() clears quick program
		// references but keeps compiled microprograms for content reuse.
		const u32 masked_addr = addr & VU0_PROGMASK;
		const u64 end_addr = static_cast<u64>(masked_addr) + size;
		if (size >= VU0_PROGSIZE || end_addr > VU0_PROGSIZE)
		{
			ClearVu0BlockMapSlots(0, VU0_PAIR_SLOTS);
			s_vu0.stats.invalidate_alls++;
			return;
		}

		const u32 first_touched_slot = masked_addr / 8;
		u32 last_touched_slot = static_cast<u32>((end_addr + 7) / 8);
		if (last_touched_slot > VU0_PAIR_SLOTS)
			last_touched_slot = VU0_PAIR_SLOTS;

		const u32 first_slot = (first_touched_slot > MAX_BLOCK_PAIRS)
			? (first_touched_slot - MAX_BLOCK_PAIRS)
			: 0;
		ClearVu0BlockMapSlots(first_slot, last_touched_slot);
		s_vu0.stats.invalidate_alls++;
	}

	void ResetVu0Blocks()
	{
		// PCSX2 owner: x86/microVU.cpp::mVUreset().  InterpVU0::Reset() does
		// not own this native-provider scheduling hint.
		VU0.nextBlockCycles = 0;
		DropVu0Blocks();
	}

	void ShutdownVu0Blocks()
	{
		DropVu0Blocks();
		if (s_vu0.code_cache)
		{
			VitaVM::FreeJitMemory(s_vu0.code_cache);
			s_vu0.code_cache = nullptr;
			s_vu0.code_cache_capacity = 0;
		}
	}

	Vu1ProviderStats GetVu0ProviderStats()
	{
		return s_vu0.stats;
	}

	Vu0TelemetryStats GetVu0TelemetryStats()
	{
		Vu0TelemetryStats stats;
		stats.execute_calls = s_vu0_execute_calls;
		stats.executed_blocks = s_vu0.stats.executed_blocks;
		stats.executed_pairs = s_vu0.stats.executed_pairs;
		stats.interpreter_steps = s_vu0.stats.interpreter_steps;
		stats.generated_blocks = s_vu0.stats.compiled_blocks;
		stats.generated_pairs = s_vu0.stats.compiled_pairs;
		stats.generated_host_instructions = s_vu0.stats.generated_host_instructions;
		stats.generated_host_load_instructions =
			s_vu0.stats.generated_host_load_instructions;
		stats.generated_host_store_instructions =
			s_vu0.stats.generated_host_store_instructions;
		stats.generated_helper_call_instructions =
			s_vu0.stats.generated_helper_call_instructions;
		stats.generated_state_load_instructions =
			s_vu0.stats.generated_state_load_instructions;
		stats.generated_state_store_instructions =
			s_vu0.stats.generated_state_store_instructions;
		stats.content_cache_hits = s_vu0.stats.content_cache_hits;
		stats.invalidations = s_vu0.stats.invalidate_alls;
		stats.scan_rejects = s_vu0.stats.scan_rejects;
		stats.compile_failures = s_vu0.stats.compile_failures;
		return stats;
	}

	void ResetVu0ProviderStats()
	{
		const size_t used = s_vu0.stats.code_cache_used;
		const size_t capacity = s_vu0.stats.code_cache_capacity;
		s_vu0.stats = {};
		s_vu0.stats.code_cache_used = used;
		s_vu0.stats.code_cache_capacity = capacity;
		s_vu0_execute_calls = 0;
	}

	extern "C" void VitaVu1ExecuteReservedVectorBody(u32 cycles);

#if defined(_M_ARM32)
	extern "C" void VitaVu1ExecuteReservedVectorFrame(u32 cycles);
	asm(
		".text\n"
		".align 2\n"
		".syntax unified\n"
		".arm\n"
		".fpu neon\n"
		".global VitaVu1ExecuteReservedVectorFrame\n"
		".type VitaVu1ExecuteReservedVectorFrame, %function\n"
		"VitaVu1ExecuteReservedVectorFrame:\n"
		"  push {r4, lr}\n"
		"  vpush {d8-d15}\n"
		"  bl VitaVu1ExecuteReservedVectorBody\n"
		"  vpop {d8-d15}\n"
		"  pop {r4, pc}\n"
		".size VitaVu1ExecuteReservedVectorFrame, .-VitaVu1ExecuteReservedVectorFrame\n");
#endif

	void ExecuteVu1Blocks(u32 cycles)
	{
		// PCSX2 owner: x86/microVU_Execute.inl enters microVU's private host
		// register domain once per Execute() window. Q4-Q7 are the Vita allocator's
		// VF/ACC cache and D8-D15 are AAPCS callee-saved, so preserve the host values
		// once outside the complete dispatcher/link chain instead of around every
		// generated block or every scalar/vector transition.
#if defined(_M_ARM32)
		// MTVU compiles the reachable block graph on CPU0 before the worker job.
		// A program which selected no Q4-Q7 mapping does not need the outer save.
		// The single-threaded provider may compile inside this call, so retain the
		// wrapper there until the same preparation separation is available.
		if (!THREAD_VU1 ||
			(s_vu1.active_program && s_vu1.active_program->uses_vector_cache))
		{
			VitaVu1ExecuteReservedVectorFrame(cycles);
		}
		else
		{
			VitaVu1ExecuteReservedVectorBody(cycles);
		}
#else
		VitaVu1ExecuteReservedVectorBody(cycles);
#endif
	}

	extern "C" __attribute__((noinline)) void VitaVu1ExecuteReservedVectorBody(u32 cycles)
	{
		// PCSX2 owners: InterpVU1::Execute() supplies the loop shape, TPC
		// byte/index conversion, VPU_STAT stop fixup, and budget condition;
		// x86 microVU owns the native-provider nextBlockCycles contract.
		// Eligible windows run through compiled blocks.
		const FPControlRegisterBackup fpcr_backup(EmuConfig.Cpu.VU1FPCR);

		VU1.VI[REG_TPC].UL <<= 3;
		const u64 startcycles = VU1.cycle;
		const u64 limit = startcycles + cycles;
		const bool performance_telemetry_enabled =
			VitaPerformanceTelemetry::IsEnabled();
		// Micro-step tracing must go through vu1Exec() so every step records.
		const bool blocks_eligible = !Pcsx2Trace::IsVuTraceEnabled()
#if defined(VITASX2_QEMU_VALIDATION)
			&& !g_qemuVuJitForceInterpreterFallback
#endif
			;

		bool admitted_logical_continuation = false;
		while (admitted_logical_continuation || (VU1.cycle - startcycles) < cycles)
		{
			if (!Vu1ProgramActive())
			{
				if (VU1.branch == 1)
				{
					VU1.VI[REG_TPC].UL = VU1.branchpc;
					VU1.branch = 0;
				}
				break;
			}

			VU1.VI[REG_TPC].UL &= VU1_PROGMASK;

			// Pair-misaligned TPC values (possible through direct TPC writes)
			// would alias block-map slots; the interpreter owns those steps.
			if (blocks_eligible && (VU1.branch == 0 || VU1.branch == 1) &&
				(VU1.ebit == 0 || VU1.ebit == 1) &&
				(VU1.VI[REG_TPC].UL & 7) == 0)
			{
				const bool entry_branch_tail = VU1.branch == 1;
				const bool entry_ebit_tail = VU1.ebit == 1;
				if (CachedBlock* block = LookupOrCompileVu1Block(VU1.VI[REG_TPC].UL,
						entry_branch_tail, entry_ebit_tail))
				{
					const BlockFn fn = reinterpret_cast<BlockFn>(const_cast<void*>(block->entry));
					const u32 result = fn(&VU1, 0,
						static_cast<u32>(limit), static_cast<u32>(limit >> 32));
					const u32 executed = result & ~EXECUTED_PAIRS_LOGICAL_CONTINUATION;
					if (executed != 0)
					{
						if (performance_telemetry_enabled)
						{
							s_vu1.stats.executed_blocks++;
							s_vu1.stats.executed_pairs += executed;
						}
						admitted_logical_continuation =
							(result & EXECUTED_PAIRS_LOGICAL_CONTINUATION) != 0 &&
							Vu1ProgramActive();
						continue;
					}
				}
			}

			const bool resolving_admitted_branch = VU1.branch != 0;
			CpuIntVU1.Step();
			if (performance_telemetry_enabled)
				s_vu1.stats.interpreter_steps++;
			if (blocks_eligible)
			{
				admitted_logical_continuation =
					Vu1ProgramActive() &&
					!resolving_admitted_branch;
			}
		}

		ClampVuCycleAfterAdmittedBlock(VU1, startcycles, cycles);
		VU1.VI[REG_TPC].UL >>= 3;
		UpdateNextBlockCyclesAtExecuteExit(VU1, 0x100, false);
		// One release publication per MTVU Execute job keeps the hot generated
		// block path free of atomic traffic while allowing the EE producer to
		// sample monotonic VU work without racing the worker-owned counters.
		if (performance_telemetry_enabled)
		{
			s_vu1_published_executed_blocks.store(s_vu1.stats.executed_blocks,
				std::memory_order_relaxed);
			s_vu1_published_executed_pairs.store(s_vu1.stats.executed_pairs,
				std::memory_order_relaxed);
			s_vu1_published_interpreter_steps.store(s_vu1.stats.interpreter_steps,
				std::memory_order_relaxed);
			s_vu1_completed_programs.fetch_add(1, std::memory_order_release);
		}
	}

	void InvalidateVu1Blocks(u32 addr, u32 size)
	{
		if (size == 0)
			return;

		// PCSX2 owner: x86/microVU.cpp::mVUclear() clears the quick program
		// references but keeps each microProgram's complete block-manager map.
		// Direct links are program-owned and remain valid inside that immutable
		// version. Pending CPU1 misses belong to the old version and must not be
		// replayed after the next MicroMem contents are selected.
		(void)addr;
		s_vu1.active_program = nullptr;
		s_vu1.quick_programs.fill(nullptr);
		ClearVu1CompileRequests();
		s_vu1.stats.invalidate_alls++;
	}

	void ResetVu1Blocks()
	{
		// PCSX2 owner: x86/microVU.cpp::mVUreset().  InterpVU1::Reset() does
		// not own this native-provider scheduling hint.
		VU1.nextBlockCycles = 0;
		ClearVu1CompileRequests();
		DropVu1Blocks();
	}

	void ShutdownVu1Blocks()
	{
		ClearVu1CompileRequests();
		DropVu1Blocks();
		if (s_vu1.code_cache)
		{
			VitaVM::FreeJitMemory(s_vu1.code_cache);
			s_vu1.code_cache = nullptr;
			s_vu1.code_cache_capacity = 0;
		}
	}

	Vu1ProviderStats GetVu1ProviderStats()
	{
		Vu1ProviderStats stats = s_vu1.stats;
#if defined(VITASX2_QEMU_VALIDATION)
		stats.linked_frame_entries = g_qemuVuJitLinkedFrameEntries;
		stats.linked_frame_instructions_removed =
			static_cast<u64>(g_qemuVuJitLinkedFrameEntries) * 10 +
			static_cast<u64>(g_qemuVuJitLinkedVectorFrameEntries) * 4;
		stats.linked_frame_stack_words_removed =
			static_cast<u64>(g_qemuVuJitLinkedFrameEntries) * 18 +
			static_cast<u64>(g_qemuVuJitLinkedVectorFrameEntries) * 32;
		stats.resident_pipe_linked_entries = g_qemuVuJitResidentPipeLinkedEntries;
		stats.resident_pipe_linked_instructions_removed =
			static_cast<u64>(g_qemuVuJitResidentPipeLinkedEntries) * 9;
		stats.resident_pipe_linked_state_loads_removed =
			static_cast<u64>(g_qemuVuJitResidentPipeLinkedEntries) * 5;
		stats.local_fmac_pipeline_entries = g_qemuVuJitLocalFmacPipelineEntries;
		stats.local_fmac_pipeline_commits = g_qemuVuJitLocalFmacPipelineCommits;
		stats.local_fmac_cycle_snapshot_elisions =
			g_qemuVuJitLocalFmacCycleSnapshotElisions;
		stats.local_fmac_producer_snapshot_entries =
			g_qemuVuJitLocalFmacProducerSnapshotEntries;
		stats.deferred_fmac_flag_entries = g_qemuVuJitDeferredFmacFlagEntries;
		stats.deferred_fmac_flag_runtime_retirements =
			g_qemuVuJitDeferredFmacFlagRetirements;
		stats.canonical_deferred_fmac_runtime_retirements =
			g_qemuVuJitCanonicalDeferredFmacRetirements;
		stats.deferred_fmac_compact_runtime_retirements =
			g_qemuVuJitDeferredFmacCompactRetirements;
		stats.deferred_fmac_compact_instructions_removed =
			g_qemuVuJitDeferredFmacCompactInstructionsRemoved;
		stats.deferred_fmac_linked_entries = g_qemuVuJitDeferredFmacLinkedEntries;
		stats.deferred_fmac_linked_instructions_removed =
			static_cast<u64>(g_qemuVuJitDeferredFmacLinkedEntries) * 6;
		stats.deferred_fmac_linked_memory_words_removed =
			static_cast<u64>(g_qemuVuJitDeferredFmacLinkedEntries) * 8;
		stats.empty_pipe_nop_batch_runtime_runs = g_qemuVuJitEmptyPipeNopBatchRuns;
		stats.empty_pipe_nop_batch_runtime_pairs = g_qemuVuJitEmptyPipeNopBatchPairs;
#endif
		return stats;
	}

	Vu1TelemetryStats GetVu1TelemetryStats()
	{
		Vu1TelemetryStats stats;
		stats.completed_programs =
			s_vu1_completed_programs.load(std::memory_order_acquire);
		stats.executed_blocks =
			s_vu1_published_executed_blocks.load(std::memory_order_relaxed);
		stats.executed_pairs =
			s_vu1_published_executed_pairs.load(std::memory_order_relaxed);
		stats.interpreter_steps =
			s_vu1_published_interpreter_steps.load(std::memory_order_relaxed);
		stats.generated_blocks = s_vu1.stats.compiled_blocks;
		stats.generated_pairs = s_vu1.stats.compiled_pairs;
		stats.generated_host_instructions = s_vu1.stats.generated_host_instructions;
		stats.generated_host_load_instructions =
			s_vu1.stats.generated_host_load_instructions;
		stats.generated_host_store_instructions =
			s_vu1.stats.generated_host_store_instructions;
		stats.generated_helper_call_instructions =
			s_vu1.stats.generated_helper_call_instructions;
		stats.generated_state_load_instructions =
			s_vu1.stats.generated_state_load_instructions;
		stats.generated_state_store_instructions =
			s_vu1.stats.generated_state_store_instructions;
		stats.resident_working_fmac_flag_blocks =
			s_vu1.stats.resident_working_fmac_flag_blocks;
		stats.resident_working_fmac_flag_producers =
			s_vu1.stats.resident_working_fmac_flag_producers;
		stats.resident_working_fmac_state_stores_removed =
			s_vu1.stats.resident_working_fmac_state_stores_removed;
		stats.mac_flag_classification_elisions =
			s_vu1.stats.mac_flag_classification_elisions;
		stats.mac_flag_classification_minimum_instructions_removed =
			s_vu1.stats.mac_flag_classification_minimum_instructions_removed;
		stats.program_prepare_checks = s_vu1.stats.program_prepare_checks;
		stats.program_prepare_calls = s_vu1.stats.program_prepare_calls;
		stats.program_quick_cache_hits = s_vu1.stats.program_quick_cache_hits;
		stats.program_compile_requests =
			s_vu1_compile_requests_total.load(std::memory_order_relaxed);
		stats.program_version_cache_hits = s_vu1.stats.program_version_cache_hits;
		stats.program_block_maps_reused = s_vu1.stats.program_block_maps_reused;
		stats.program_versions_created = s_vu1.stats.program_versions_created;
		stats.content_cache_hits = s_vu1.stats.content_cache_hits;
		stats.invalidations = s_vu1.stats.invalidate_alls;
		stats.compile_failures = s_vu1.stats.compile_failures;
		return stats;
	}

	void ResetVu1ProviderStats()
	{
#if defined(VITASX2_QEMU_VALIDATION)
		g_qemuVuJitLinkedFrameEntries = 0;
		g_qemuVuJitLinkedVectorFrameEntries = 0;
		g_qemuVuJitResidentPipeLinkedEntries = 0;
		g_qemuVuJitLocalFmacPipelineEntries = 0;
		g_qemuVuJitLocalFmacPipelineCommits = 0;
		g_qemuVuJitLocalFmacCycleSnapshotElisions = 0;
		g_qemuVuJitLocalFmacProducerSnapshotEntries = 0;
		g_qemuVuJitDeferredFmacFlagEntries = 0;
		g_qemuVuJitDeferredFmacFlagRetirements = 0;
		g_qemuVuJitCanonicalDeferredFmacRetirements = 0;
		g_qemuVuJitDeferredFmacCompactRetirements = 0;
		g_qemuVuJitDeferredFmacCompactInstructionsRemoved = 0;
		g_qemuVuJitDeferredFmacLinkedEntries = 0;
		g_qemuVuJitEmptyPipeNopBatchRuns = 0;
		g_qemuVuJitEmptyPipeNopBatchPairs = 0;
#endif
		const size_t used = s_vu1.stats.code_cache_used;
		const size_t capacity = s_vu1.stats.code_cache_capacity;
		s_vu1.stats = {};
		s_vu1.stats.code_cache_used = used;
		s_vu1.stats.code_cache_capacity = capacity;
		s_vu1_compile_requests_total.store(0, std::memory_order_relaxed);
		s_vu1_completed_programs.store(0, std::memory_order_relaxed);
		s_vu1_published_executed_blocks.store(0, std::memory_order_relaxed);
		s_vu1_published_executed_pairs.store(0, std::memory_order_relaxed);
		s_vu1_published_interpreter_steps.store(0, std::memory_order_relaxed);
	}
} // namespace VitaVU
