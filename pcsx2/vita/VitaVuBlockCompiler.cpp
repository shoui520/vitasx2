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
#include "DebugTools/VuTrace.h"
#include "VUmicro.h"
#include "VUmicroFast.h"
#include "Vif.h"

#include "vita/A32Emitter.h"
#include "vita/VitaVuBlockCompiler.h"

#include "common/Vita/VitaJitMemory.h"

#include <array>
#include <cstring>
#include <memory>
#include <vector>

extern void _vuFlushAll(VURegs* VU);

namespace VitaVU
{
	using VitaA32::CodeBuffer;
	using VitaA32::Condition;
	using VitaA32::ShiftType;

	namespace
	{
		// ------------------------------------------------------------------
		// Per-kind op-body entry points.
		//
		// PCSX2 owners: VUmicroFast.h::ExecuteUpperNoLowerKnownKind() /
		// ExecuteLowerNoUpperKnownKind(). Instantiating the kind as a template
		// constant folds the kind switch away, so generated code calls the op
		// body directly instead of re-dispatching on the decoded kind.
		// ------------------------------------------------------------------
		using KindExecFn = void (*)(VURegs* VU, u32 code);

		template <VUInterpFast::UpperFastKind K>
		void ExecUpperKindThunk(VURegs* VU, u32 code)
		{
			VUInterpFast::ExecuteUpperNoLowerKnownKind(VU, code, K);
		}

		template <VUInterpFast::LowerFastKind K>
		void ExecLowerKindThunk(VURegs* VU, u32 code)
		{
			VUInterpFast::ExecuteLowerNoUpperKnownKind(VU, code, K);
		}

		template <size_t... I>
		constexpr std::array<KindExecFn, sizeof...(I)> MakeUpperKindTable(std::index_sequence<I...>)
		{
			return {{&ExecUpperKindThunk<static_cast<VUInterpFast::UpperFastKind>(I)>...}};
		}

		template <size_t... I>
		constexpr std::array<KindExecFn, sizeof...(I)> MakeLowerKindTable(std::index_sequence<I...>)
		{
			return {{&ExecLowerKindThunk<static_cast<VUInterpFast::LowerFastKind>(I)>...}};
		}

		const std::array<KindExecFn, VUInterpFast::UpperFastKindCount> s_upper_kind_exec =
			MakeUpperKindTable(std::make_index_sequence<VUInterpFast::UpperFastKindCount>{});
		const std::array<KindExecFn, VUInterpFast::LowerFastKindCount> s_lower_kind_exec =
			MakeLowerKindTable(std::make_index_sequence<VUInterpFast::LowerFastKindCount>{});

		// PCSX2 owner: VU1microInterp.cpp::_vu1Exec() E-bit completion body
		// (the `ebit-- == 1` branch). Generated blocks call this after storing
		// the statically-known ebit value of zero.
		void Vu1EbitFinish(VURegs* VU)
		{
			VU->VIBackupCycles = 0;
			_vuFlushAll(VU);
			VU0.VI[REG_VPU_STAT].UL &= ~0x100;
			vif1Regs.stat.VEW = false;

			if (VU1.xgkickenable)
				_vuXGKICKTransfer(0, true);
			if (INSTANT_VU1)
				VU1.xgkicklastcycle = cpuRegs.cycle;
		}

		// ------------------------------------------------------------------
		// Static pair analysis.
		// ------------------------------------------------------------------

		// PCSX2 owners: VU1microInterp.cpp::_vu1IsUpperNop()/_vu1IsLowerNop().
		constexpr bool IsUpperNop(u32 upper) { return (upper & 0x07ffffffu) == 0x000002ffu; }
		constexpr bool IsLowerNop(u32 lower) { return lower == 0x8000033cu; }

		constexpr u32 UPPER_I_BIT = 0x80000000u;
		constexpr u32 UPPER_E_BIT = 0x40000000u;
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
			// Tail work windows.
			bool branch_tail = false;
			bool ebit_tail = false;
			u32 ebit_store = 0; // valid when ebit_tail: statically-known post-decrement value
			bool ends_block = false;
			_VURegsNum uregs = {};
			_VURegsNum lregs = {};
		};

		constexpr u32 MAX_BLOCK_PAIRS = 64;

		struct BlockPlan
		{
			u32 start_pc = 0;
			u32 pair_count = 0;
			std::array<PairPlan, MAX_BLOCK_PAIRS> pairs{};
		};

		// Mirrors the analysis+path-selection half of _vu1Exec() for one pair.
		// Returns false when the interpreter must own this pair (non-fast op,
		// or D/T flags whose FBRST behavior is runtime-dependent).
		bool AnalyzePair(u32 pc, u32 upper, u32 lower, PairPlan* plan)
		{
			if (upper & (UPPER_D_BIT | UPPER_T_BIT))
				return false;

			plan->pc = pc;
			plan->upper = upper;
			plan->lower = lower;
			plan->ebit = (upper & UPPER_E_BIT) != 0;
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
					if (!VuMicroAnalyzeLowerNoUpperCached(1, pc, lower, &plan->lregs, &plan->lower_kind))
						return false;
					plan->exec_lower = true;
				}
			}
			else
			{
				if (!VuMicroAnalyzeUpperNoLowerCached(1, pc, upper, &plan->uregs, &plan->upper_kind))
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
					if (!VuMicroAnalyzeLowerNoUpperCached(1, pc, lower, &plan->lregs, &plan->lower_kind))
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

			plan->fmac_pipe = (plan->uregs.pipe == VUPIPE_FMAC) || (plan->lregs.pipe == VUPIPE_FMAC);
			return true;
		}

		// Scans a straight-line pair run from start_pc, statically simulating
		// _vu1Exec()'s branch-delay and E-bit windows. Entry contract (checked
		// by the dispatcher): VU1.branch == 0 && VU1.ebit == 0.
		bool ScanBlock(const u8* micro, u32 start_pc, BlockPlan* block)
		{
			block->start_pc = start_pc;
			block->pair_count = 0;

			u32 pc = start_pc;
			u32 branch_window = 0;
			u32 ebit_static = 0;

			while (block->pair_count < MAX_BLOCK_PAIRS && pc < VU1_PROGSIZE)
			{
				u32 upper;
				u32 lower;
				std::memcpy(&lower, &micro[pc + 0], sizeof(lower));
				std::memcpy(&upper, &micro[pc + 4], sizeof(upper));

				PairPlan& plan = block->pairs[block->pair_count];
				plan = {};
				if (!AnalyzePair(pc, upper, lower, &plan))
					break;

				if (plan.ebit)
					ebit_static = 2;

				if (branch_window > 0)
				{
					plan.branch_tail = true;
					branch_window--;
					if (branch_window == 0)
						plan.ends_block = true; // TPC may have been redirected
				}
				if (plan.lregs.pipe == VUPIPE_BRANCH && !plan.ends_block)
				{
					plan.branch_tail = true;
					branch_window = 1; // next pair is the delay slot resolution
				}

				if (ebit_static > 0)
				{
					ebit_static--;
					plan.ebit_tail = true;
					plan.ebit_store = ebit_static;
					if (ebit_static == 0)
						plan.ends_block = true; // VPU_STAT run bit cleared
				}

				block->pair_count++;
				pc += 8;
				if (plan.ends_block)
					break;
			}

			return block->pair_count != 0;
		}

		// ------------------------------------------------------------------
		// Code generation.
		// ------------------------------------------------------------------

		// Generated block ABI: u32 executed_pairs = block(VURegs*, unused,
		// limit_lo, limit_hi) with limit = startcycles + cycles, matching
		// InterpVU1::Execute()'s budget condition.
		using BlockFn = u32 (*)(VURegs*, u32, u32, u32);

		constexpr unsigned HOST_VU = 4;       // VURegs*
		constexpr unsigned HOST_CYCLE_LO = 5; // cycle low word after this pair's increment
		constexpr unsigned HOST_LIMIT_LO = 6;
		constexpr unsigned HOST_LIMIT_HI = 7;
		constexpr unsigned HOST_CLIP_OLD = 8; // paired clip-flag hazard backups
		constexpr unsigned HOST_CLIP_NEW = 9;
		constexpr unsigned SP = 13;

		constexpr u16 SAVED_REGISTER_MASK = 0x43f0; // r4-r9, sl, lr
		constexpr u32 STACK_FRAME_SIZE = 32;        // VF hazard backup slots

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
			explicit BlockCompiler(CodeBuffer& code, const BlockPlan& plan, PairPlan* stable_pairs)
				: m_code(code)
				, m_plan(plan)
				, m_pairs(stable_pairs)
			{
			}

			bool Compile()
			{
				if (!EmitPrologue())
					return false;

				for (u32 i = 0; i < m_plan.pair_count; i++)
				{
					if (!EmitPair(i))
						return false;
				}

				// Fall-through return: every pair executed.
				if (!m_code.EmitMovImm32(0, m_plan.pair_count))
					return false;
				const size_t epilogue_offset = m_code.Size();
				if (!EmitEpilogue())
					return false;

				// Budget-exit stubs: return the number of fully executed pairs.
				for (const BudgetExit& exit : m_budget_exits)
				{
					const size_t stub_offset = m_code.Size();
					if (!m_code.PatchBranch(exit.branch_site, stub_offset, Condition::CS))
						return false;
					if (!m_code.EmitMovImm32(0, exit.executed_pairs))
						return false;
					const size_t jump = m_code.EmitBranchPlaceholder();
					if (jump == static_cast<size_t>(-1) ||
						!m_code.PatchBranch(jump, epilogue_offset))
					{
						return false;
					}
				}

				return true;
			}

		private:
			struct BudgetExit
			{
				size_t branch_site = 0;
				u32 executed_pairs = 0;
			};

			bool EmitPrologue()
			{
				return m_code.EmitPush(SAVED_REGISTER_MASK) &&
					m_code.EmitSubImm8(SP, SP, STACK_FRAME_SIZE) &&
					EmitMovReg(HOST_VU, 0) &&
					EmitMovReg(HOST_LIMIT_LO, 2) &&
					EmitMovReg(HOST_LIMIT_HI, 3);
			}

			bool EmitEpilogue()
			{
				return m_code.EmitAddImm8(SP, SP, STACK_FRAME_SIZE) &&
					m_code.EmitPop(0x83f0); // r4-r9, sl, pc
			}

			bool EmitMovReg(unsigned rd, unsigned rm, Condition condition = Condition::AL)
			{
				return m_code.EmitMovRegShiftImm(rd, rm, ShiftType::LSL, 0, false, condition);
			}

			bool EmitCallHelper(const void* fn)
			{
				return EmitMovReg(0, HOST_VU) && m_code.EmitCallAbsolute(fn);
			}

			bool EmitCallHelperRegs(const void* fn, const _VURegsNum* regs)
			{
				return EmitMovReg(0, HOST_VU) &&
					m_code.EmitMovImm32(1, static_cast<u32>(reinterpret_cast<uptr>(regs))) &&
					m_code.EmitCallAbsolute(fn);
			}

			bool EmitCallKindExec(KindExecFn fn, u32 code)
			{
				return EmitMovReg(0, HOST_VU) &&
					m_code.EmitMovImm32(1, code) &&
					m_code.EmitCallAbsolute(reinterpret_cast<const void*>(fn));
			}

			// PCSX2 owner: InterpVU1::Execute()'s `(VU1.cycle - startcycles) <
			// cycles` guard, evaluated before every step, plus vu1Exec()'s
			// unconditional cycle increment. Leaves the post-increment low
			// word in HOST_CYCLE_LO for the VIBackupCycles window math.
			bool EmitBudgetCheckAndCycleIncrement(u32 pair_index)
			{
				const u16 lo = VuOffset(offsetof(VURegs, cycle));
				const u16 hi = VuOffset(offsetof(VURegs, cycle) + 4);
				if (!m_code.EmitLdrImm12(0, HOST_VU, lo) ||
					!m_code.EmitLdrImm12(1, HOST_VU, hi))
				{
					return false;
				}

				if (pair_index != 0)
				{
					// The dispatcher already proved the budget for pair 0.
					if (!m_code.EmitCmpReg(1, HOST_LIMIT_HI) ||
						!m_code.EmitCmpReg(0, HOST_LIMIT_LO, Condition::EQ))
					{
						return false;
					}
					const size_t exit_site = m_code.EmitBranchPlaceholder(Condition::CS);
					if (exit_site == static_cast<size_t>(-1))
						return false;
					m_budget_exits.push_back({exit_site, pair_index});
				}

				return m_code.EmitAddImm8(0, 0, 1, true) &&
					m_code.EmitAdcImm8(1, 1, 0) &&
					m_code.EmitStrImm12(0, HOST_VU, lo) &&
					m_code.EmitStrImm12(1, HOST_VU, hi) &&
					EmitMovReg(HOST_CYCLE_LO, 0);
			}

			// PCSX2 owner: the per-step `VU->VIBackupCycles -=
			// std::min((u8)(VU1.cycle - cyclesBeforeOp), VU->VIBackupCycles)`
			// update, where cyclesBeforeOp is the pre-stall cycle minus one.
			bool EmitViBackupUpdate()
			{
				const u16 backup = VuOffset(offsetof(VURegs, VIBackupCycles));
				if (!m_code.EmitLdrbImm12(0, HOST_VU, backup) ||
					!m_code.EmitCmpImm32(0, 0))
				{
					return false;
				}
				const size_t skip = m_code.EmitBranchPlaceholder(Condition::EQ);
				if (skip == static_cast<size_t>(-1))
					return false;

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

			// PCSX2 owner: _vu1Exec()'s per-step branch-delay countdown,
			// including the branch-in-delay-slot takedelaybranch handoff.
			bool EmitBranchTail()
			{
				const u16 branch = VuOffset(offsetof(VURegs, branch));
				if (!m_code.EmitLdrImm12(0, HOST_VU, branch) ||
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
				return m_code.EmitAddImm32(0, HOST_VU, VfOffset(vf_reg)) &&
					m_code.EmitVld1Q32Aligned(0, 0) &&
					m_code.EmitAddImm32(1, SP, stack_offset) &&
					m_code.EmitVst1Q32(0, 1);
			}

			bool EmitVfCopyFromStack(unsigned vf_reg, u32 stack_offset)
			{
				return m_code.EmitAddImm32(1, SP, stack_offset) &&
					m_code.EmitVld1Q32(0, 1) &&
					m_code.EmitAddImm32(0, HOST_VU, VfOffset(vf_reg)) &&
					m_code.EmitVst1Q32Aligned(0, 0);
			}

			bool EmitPair(u32 pair_index)
			{
				const PairPlan& plan = m_pairs[pair_index];

				if (!EmitBudgetCheckAndCycleIncrement(pair_index))
					return false;

				// VU->VI[REG_TPC].UL += 8, as a compile-time constant.
				if (!m_code.EmitMovImm32(0, plan.pc + 8) ||
					!m_code.EmitStrImm12(0, HOST_VU, ViOffset(REG_TPC)))
				{
					return false;
				}

				// E flag decode, compile-time: VU->ebit = 2.
				if (plan.ebit)
				{
					if (!m_code.EmitMovImm8(0, 2) ||
						!m_code.EmitStrImm12(0, HOST_VU, VuOffset(offsetof(VURegs, ebit))))
					{
						return false;
					}
				}

				// VU->code ends every interpreter step holding the lower word,
				// except I-bit steps which keep the upper word. Nothing in the
				// fast op bodies or stall helpers reads it mid-step, so one
				// store of the final value is exact.
				const u32 final_code = (plan.shape == PairShape::IBit) ? plan.upper : plan.lower;
				if (!m_code.EmitMovImm32(0, final_code) ||
					!m_code.EmitStrImm12(0, HOST_VU, VuOffset(offsetof(VURegs, code))))
				{
					return false;
				}

				// Stall tests in _vu1Exec() order: upper, lower, pipes.
				if (plan.test_upper_stalls &&
					!EmitCallHelperRegs(reinterpret_cast<const void*>(&_vuTestUpperStalls), &m_pairs[pair_index].uregs))
				{
					return false;
				}
				if (plan.test_lower_stalls &&
					!EmitCallHelperRegs(reinterpret_cast<const void*>(&_vuTestLowerStalls), &m_pairs[pair_index].lregs))
				{
					return false;
				}
				if (!EmitCallHelper(reinterpret_cast<const void*>(&_vuTestPipes)))
					return false;

				if (!EmitViBackupUpdate())
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

				if (plan.exec_upper &&
					!EmitCallKindExec(s_upper_kind_exec[plan.upper_kind], plan.upper))
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
					if (!m_code.EmitAddImm32(0, HOST_VU, VfOffset(plan.vf_backup_reg)) ||
						!m_code.EmitVld1Q32Aligned(0, 0) ||
						!m_code.EmitAddImm32(1, SP, 16) ||
						!m_code.EmitVst1Q32(0, 1) ||
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

				if (plan.exec_lower &&
					!EmitCallKindExec(s_lower_kind_exec[plan.lower_kind], plan.lower))
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
				if (plan.fmac_pipe &&
					!EmitCallHelper(reinterpret_cast<const void*>(&_vuClearFMAC)))
				{
					return false;
				}
				if (plan.add_upper_stalls &&
					!EmitCallHelperRegs(reinterpret_cast<const void*>(&_vuAddUpperStalls), &m_pairs[pair_index].uregs))
				{
					return false;
				}
				if (plan.add_lower_stalls &&
					!EmitCallHelperRegs(reinterpret_cast<const void*>(&_vuAddLowerStalls), &m_pairs[pair_index].lregs))
				{
					return false;
				}

				if (plan.branch_tail && !EmitBranchTail())
					return false;

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
						!EmitCallHelper(reinterpret_cast<const void*>(&Vu1EbitFinish)))
					{
						return false;
					}
				}

				if (plan.fmac_pipe)
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

				return true;
			}

			CodeBuffer& m_code;
			const BlockPlan& m_plan;
			PairPlan* m_pairs;
			std::vector<BudgetExit> m_budget_exits;
		};

		// ------------------------------------------------------------------
		// Block cache.
		// ------------------------------------------------------------------

		struct CachedBlock
		{
			const void* entry = nullptr;
			size_t code_size = 0;
			u32 start_pc = 0;
			u32 pair_count = 0;
			// Generated code embeds pointers into this array (stall-helper
			// _VURegsNum arguments), so it must stay stable for the lifetime
			// of the block.
			std::unique_ptr<PairPlan[]> pairs;
		};

		constexpr u32 VU1_PAIR_SLOTS = VU1_PROGSIZE / 8;
		// PCSX2 owner: HostMemoryMap sizes the x86 mVU1rec cache; the Vita
		// cache holds one 16 KiB microprogram's worth of expanded pair code.
		constexpr size_t VU1_CODE_CACHE_CAPACITY = 4 * 1024 * 1024;
		constexpr size_t CODE_ALIGNMENT = 32;

		CachedBlock* const BLOCK_UNCOMPILABLE = reinterpret_cast<CachedBlock*>(1);

		struct Vu1State
		{
			std::array<CachedBlock*, VU1_PAIR_SLOTS> map{};
			std::vector<std::unique_ptr<CachedBlock>> blocks;
			u8* code_cache = nullptr;
			size_t code_cache_used = 0;
			size_t code_cache_capacity = 0;
			bool map_populated = false; // any block or uncompilable marker present
			Vu1ProviderStats stats;
		};

		Vu1State s_vu1;

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

		void DropVu1Blocks()
		{
			if (!s_vu1.map_populated)
				return;
			s_vu1.map.fill(nullptr);
			s_vu1.blocks.clear();
			s_vu1.code_cache_used = 0;
			s_vu1.map_populated = false;
		}

		CachedBlock* CompileVu1Block(u32 start_pc)
		{
			BlockPlan plan;
			if (!ScanBlock(VU1.Micro, start_pc, &plan))
			{
				s_vu1.stats.scan_rejects++;
				return nullptr;
			}

			if (!EnsureVu1CodeCache())
			{
				s_vu1.stats.compile_failures++;
				return nullptr;
			}

			auto block = std::make_unique<CachedBlock>();
			block->start_pc = start_pc;
			block->pair_count = plan.pair_count;
			block->pairs = std::make_unique<PairPlan[]>(plan.pair_count);
			std::copy_n(plan.pairs.begin(), plan.pair_count, block->pairs.get());

			for (int attempt = 0; attempt < 2; attempt++)
			{
				const size_t offset = (s_vu1.code_cache_used + (CODE_ALIGNMENT - 1)) & ~(CODE_ALIGNMENT - 1);
				CodeBuffer code;
				if (offset < s_vu1.code_cache_capacity &&
					code.Attach(s_vu1.code_cache + offset, s_vu1.code_cache_capacity - offset))
				{
					BlockCompiler compiler(code, plan, block->pairs.get());
					if (compiler.Compile())
					{
						code.Flush();
						block->entry = code.EntryPoint();
						block->code_size = code.Size();
						s_vu1.code_cache_used = offset + code.Size();
						s_vu1.stats.code_cache_used = s_vu1.code_cache_used;
						s_vu1.stats.compiled_blocks++;
						s_vu1.stats.compiled_pairs += plan.pair_count;

						CachedBlock* result = block.get();
						s_vu1.blocks.push_back(std::move(block));
						s_vu1.map[start_pc / 8] = result;
						s_vu1.map_populated = true;
						return result;
					}
				}

				// Whole-cache pressure reset, PCSX2 owner:
				// x86/microVU.cpp::mVUreset() on cache exhaustion.
				DropVu1Blocks();
				s_vu1.stats.code_cache_resets++;
			}

			s_vu1.stats.compile_failures++;
			return nullptr;
		}

		CachedBlock* LookupOrCompileVu1Block(u32 start_pc)
		{
			CachedBlock* block = s_vu1.map[start_pc / 8];
			if (block == BLOCK_UNCOMPILABLE)
				return nullptr;
			if (block)
				return block;

			block = CompileVu1Block(start_pc);
			if (!block)
			{
				s_vu1.map[start_pc / 8] = BLOCK_UNCOMPILABLE;
				s_vu1.map_populated = true;
			}
			return block;
		}
	} // anonymous namespace

	void ExecuteVu1Blocks(u32 cycles)
	{
		// PCSX2 owner: InterpVU1::Execute(). The loop shape, TPC byte/index
		// conversion, VPU_STAT stop fixup, budget condition, and
		// nextBlockCycles update are byte-for-byte the interpreter's; only
		// eligible windows run through compiled blocks.
		const FPControlRegisterBackup fpcr_backup(EmuConfig.Cpu.VU1FPCR);

		VU1.VI[REG_TPC].UL <<= 3;
		const u64 startcycles = VU1.cycle;
		const u64 limit = startcycles + cycles;
		// Micro-step tracing must go through vu1Exec() so every step records.
		const bool blocks_eligible = !Pcsx2Trace::IsVuTraceEnabled();

		while ((VU1.cycle - startcycles) < cycles)
		{
			if (!(VU0.VI[REG_VPU_STAT].UL & 0x100))
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
			if (blocks_eligible && VU1.branch == 0 && VU1.ebit == 0 &&
				(VU1.VI[REG_TPC].UL & 7) == 0)
			{
				if (CachedBlock* block = LookupOrCompileVu1Block(VU1.VI[REG_TPC].UL))
				{
					const BlockFn fn = reinterpret_cast<BlockFn>(const_cast<void*>(block->entry));
					const u32 executed = fn(&VU1, 0,
						static_cast<u32>(limit), static_cast<u32>(limit >> 32));
					s_vu1.stats.executed_blocks++;
					s_vu1.stats.executed_pairs += executed;
					continue;
				}
			}

			CpuIntVU1.Step();
			s_vu1.stats.interpreter_steps++;
		}

		VU1.VI[REG_TPC].UL >>= 3;
		VU1.nextBlockCycles = (VU1.cycle - cpuRegs.cycle) + 1;
	}

	void InvalidateVu1Blocks(u32 addr, u32 size)
	{
		// Whole-program invalidation on any micro-memory write, standing in
		// for x86 microVU's content-hashed program cache.
		(void)addr;
		(void)size;
		if (s_vu1.map_populated)
		{
			DropVu1Blocks();
			s_vu1.stats.invalidate_alls++;
		}
	}

	void ResetVu1Blocks()
	{
		DropVu1Blocks();
	}

	void ShutdownVu1Blocks()
	{
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
		return s_vu1.stats;
	}

	void ResetVu1ProviderStats()
	{
		const size_t used = s_vu1.stats.code_cache_used;
		const size_t capacity = s_vu1.stats.code_cache_capacity;
		s_vu1.stats = {};
		s_vu1.stats.code_cache_used = used;
		s_vu1.stats.code_cache_capacity = capacity;
	}
} // namespace VitaVU
