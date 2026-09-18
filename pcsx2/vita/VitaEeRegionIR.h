// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <array>
#include <string>
#include <vector>

namespace VitaEE::RegionIR
{
	using ValueId = u32;
	static constexpr ValueId INVALID_VALUE = UINT32_MAX;
	static constexpr u32 INVALID_BLOCK = UINT32_MAX;

	// These types describe PS2 architectural values, not their eventual A32
	// allocation. In particular, an EE GPR remains I128 even when an operation
	// replaces only its low 64 bits.
	enum class ValueType : u8
	{
		Void,
		I1,
		I32,
		// Raw architectural FPR word. Keeping this distinct from I32 prevents
		// integer dataflow from becoming floating-point data without an explicit
		// bit-preserving COP1 transfer.
		F32Bits,
		// Four raw VU floating-point words. This is deliberately distinct from
		// an EE I128 value: a bit-identical qword cannot silently cross between
		// the EE GPR/MMI and VU0 FMAC register files.
		VuF32x4Bits,
		I64,
		I128,
		Address,
		Cycle,
		MemoryEffect,
	};

	// Lane-wise EE MMI operations which map exactly to one AArch32 NEON
	// instruction.  The semantic kind is carried by PackedBinary128 rather than
	// split into opcode-specific IR nodes so decoding, interpretation, exit-map
	// verification, allocation, and native lowering share one complete contract.
	enum class PackedBinaryKind : u8
	{
		AddWrap8,
		AddWrap16,
		AddWrap32,
		SubtractWrap8,
		SubtractWrap16,
		SubtractWrap32,
		CompareGreaterSigned8,
		CompareGreaterSigned16,
		CompareGreaterSigned32,
		MaximumSigned16,
		MaximumSigned32,
		AddSaturateSigned8,
		AddSaturateSigned16,
		AddSaturateSigned32,
		SubtractSaturateSigned8,
		SubtractSaturateSigned16,
		SubtractSaturateSigned32,
		CompareEqual8,
		CompareEqual16,
		CompareEqual32,
		MinimumSigned16,
		MinimumSigned32,
		AddSaturateUnsigned8,
		AddSaturateUnsigned16,
		AddSaturateUnsigned32,
		SubtractSaturateUnsigned8,
		SubtractSaturateUnsigned16,
		SubtractSaturateUnsigned32,
		// MMI.cpp::{PEXTLW,PEXTUW}: interleave the lower or upper pair of
		// 32-bit lanes as RT0,RS0,RT1,RS1 or RT2,RS2,RT3,RS3.  These are
		// binary 128-bit operations even though the natural A32 lowering is
		// the corresponding half of one destructive VZIP.32 pair.
		InterleaveLower32,
		InterleaveUpper32,
		Count,
	};

	// The six immediate packed shifts share one exact unary I128 contract.  The
	// kind selects the lane width and signedness; Node::literal owns the decoded
	// shift amount so a verifier can compare both independently with the source.
	enum class PackedShiftKind : u8
	{
		LeftLogical16,
		RightLogical16,
		RightArithmetic16,
		LeftLogical32,
		RightLogical32,
		RightArithmetic32,
		Count,
	};

	enum class Opcode : u8
	{
		Parameter,
		ConstantI1,
		ConstantI32,
		ConstantI64,
		ConstantAddress,
		// A source-backed instruction which has no operation-body effect under the
		// selected PCSX2 recompiler contract (SYNC, PREF, cache-disabled CACHE, or
		// an idle-guarded VNOP/VWAITQ). Required observers remain separate nodes.
		NoEffect,
		ExtractLow32,
		ExtractLow64,
		ExtractHigh64,
		ReplaceLow64,
		ReplaceHigh64,
		BitcastI32ToF32Bits,
		BitcastF32BitsToI32,
		// Raw, bit-preserving EE GPR <-> VU0 VF transfers. Keeping these
		// explicit prevents an arbitrary I128 from entering the floating-point
		// register file without a decoded QMFC2/QMTC2 ownership witness.
		BitcastI128ToVuF32x4Bits,
		BitcastVuF32x4BitsToI128,
		// PCSX2 FPU.cpp::fpuDouble() operand normalization, followed by one
		// uncontracted single-precision operation and its architectural O/U
		// result/flag normalization. Keeping the raw result shared prevents the
		// value and FCR31 paths from silently evaluating different operations.
		Cop1NormalizeInput,
		Cop1AddRaw,
		Cop1SubRaw,
		Cop1MulRaw,
		// True when the final raw arithmetic result requires PCSX2's ordered
		// overflow/underflow correction. A guarded cold exit re-enters tier zero
		// at the owning instruction, leaving the ordinary region path finite and
		// normalized without weakening the exact decomposed result/flag graph.
		Cop1ExceptionalOuResult,
		Cop1ClampOuResult,
		Cop1UpdateOuFlags,
		// Comparisons consume PCSX2 fpuDouble()-normalized raw FPR words and
		// update only FCR31.C through an explicit old-state dependency.
		Cop1CompareEqual,
		Cop1CompareLess,
		Cop1CompareLessEqual,
		Cop1UpdateConditionFlag,
		// BC1F/T/FL/TL snapshots FCR31.C before its delay slot.  immediate is
		// zero for the false forms and one for the true forms, so this node is
		// the exact branch-taken predicate rather than a generic bit test.
		Cop1BranchCondition,
		// ABS.S and NEG.S are raw word transformations on the EE: they clear or
		// toggle only the sign bit and do not normalize NaNs or denormals.
		Cop1AbsoluteWord,
		Cop1NegateWord,
		// Both instructions clear only the current O/U cause bits.  The sticky
		// SO/SU bits and every unrelated FCR31 field remain unchanged.
		Cop1ClearOuFlags,
		// SCE CVT.W.S writes the truncated/saturated signed word as raw FPR
		// bits and leaves FCR31 untouched.
		Cop1ConvertWord,
		// SCE CVT.S.W interprets the raw source FPR word as signed I32 and
		// writes its single-precision representation using the active EE FPU
		// rounding mode without changing flags.
		Cop1ConvertSingle,
		SignExtend32To64,
		ZeroExtend32To64,
		// EE scalar multiply consumes the low 32 bits of two GPRs and produces
		// one raw 64-bit product. HI/LO lane selection, multiply-add accumulation,
		// and the architecturally sign-extended word publications remain explicit
		// IR around this value so both accumulator banks share one contract.
		MultiplySigned32,
		MultiplyUnsigned32,
		Truncate64To32,
		Add32,
		// Signed ADD/ADDI overflow is an architectural condition, not host UB.
		// The first admitted use is ADDI; keeping the predicate separate from
		// the wrapping value lets a backend branch to an exact canonical exit
		// before the result is bound.
		SignedAddOverflow32,
		Add64,
		Sub32,
		Sub64,
		And32,
		And64,
		Or64,
		Xor32,
		Xor64,
		Nor64,
		And128,
		Or128,
		Xor128,
		Nor128,
		PackedBinary128,
		PackedShift128,
		PackLow64,
		PackHigh64,
		BroadcastLowHalfwordPer64,
		ShiftLeft32,
		ShiftRightLogical32,
		ShiftRightArithmetic32,
		ShiftLeft64,
		ShiftRightLogical64,
		ShiftRightArithmetic64,
		ShiftLeft32Variable,
		ShiftRightLogical32Variable,
		ShiftRightArithmetic32Variable,
		ShiftLeft64Variable,
		ShiftRightLogical64Variable,
		ShiftRightArithmetic64Variable,
		Select64,
		CompareEqual64,
		CompareNotEqual64,
		CompareSignedLess64,
		CompareUnsignedLess64,
		CompareSignedLessEqualZero64,
		CompareSignedGreaterZero64,
		CompareSignedLessZero64,
		CompareSignedGreaterEqualZero64,
		AddressFromI32,
		EffectiveAddress32,
		MemoryLoad,
		MemoryLoadValue,
		MemoryStore,
		// Direct VU0 macro operations are admitted only while VU0 is idle. The
		// node consumes current VPU_STAT and the exact value used after the
		// observer, exits before the source instruction when bit zero says micro
		// mode is running, and otherwise forwards that value. Making the operation
		// depend on this result prevents a backend from moving it ahead of the
		// synchronization observer.
		Vu0RequireIdle,
		// Macro-mode VFTOI0/4/12/15 consumes one raw VU vector, applies the
		// source-selected fixed-point scale, truncates toward zero, and saturates
		// out-of-range lanes. The scale exponent is carried in immediate.
		Vu0ConvertFixed,
		// Macro-mode ITOF0/4/12/15 consumes signed 32-bit fixed-point lanes,
		// converts them with the EE/VU rounding contract, and applies the exact
		// 2^-offset scale. The fractional-bit count is carried in immediate.
		Vu0ConvertIntegerToFloat,
		// VMR32 rotates the source's four raw 32-bit lanes left by one. It is a
		// bit permutation, not floating-point arithmetic, and therefore must not
		// normalize its input or output.
		Vu0Rotate32,
		// VU0 FMAC arithmetic remains decomposed into uncontracted multiply/add/sub,
		// one shared raw result, architectural result normalization, and explicit
		// flag publication. The lane/mask lives in immediate where applicable.
		Vu0NormalizeVector,
		Vu0BroadcastLane,
		Vu0BroadcastScalar,
		Vu0FdivQ,
		Vu0FdivFlags,
		Vu0UpdateFdivStatus,
		Vu0SyncFdivStatusControl,
		Vu0MulRaw,
		Vu0AddRaw,
		Vu0SubRaw,
		Vu0ClampFmacResult,
		Vu0MacFlagsFromRaw,
		Vu0StatusFlagsFromMac,
		Vu0MergeMasked,
		Vu0SyncStatusControl,
		// CTC2 is represented as one decoded control-register operation rather
		// than an arbitrary integer-expression graph.  Operand zero is the old
		// target VI word, operand one is the idle-guarded low GPR word, and the
		// target control-register index is carried in immediate.  This lets the
		// verifier mechanically enforce the target-specific masks and mirrors.
		Vu0ControlWrite,
		// PCSX2 microVU_Macro.inl::mVUallocSFLAGd() conversion used when CTC2
		// writes STATUS.  Keeping it semantic prevents four hidden micro-status
		// mirrors from drifting away from the architectural VI word.
		Vu0DenormalizeStatus,
		BindVu0Q,
		BindVu0ViQ,
		BindGpr,
		BindHi,
		BindLo,
		BindSa,
		BindFpr,
		BindVu0Vf,
		BindVu0Acc,
		BindVu0MacFlag,
		BindVu0StatusFlag,
		BindVu0ViMac,
		BindVu0ViStatus,
		BindVu0Vi,
		BindVu0ClipFlag,
		BindVu0MicroStatusFlag,
		BindFcr31,
		BindAcc,
		// Operand zero is an I1 condition. immediate indexes Block::guarded_exits;
		// a true condition materializes that complete transfer before any later
		// node from the owning source instruction can publish state.
		ExitIfTrue,
		AdvanceCycles,
	};

	struct Node
	{
		ValueId id = INVALID_VALUE;
		Opcode opcode = Opcode::Parameter;
		ValueType type = ValueType::Void;
		std::array<ValueId, 3> operands = {INVALID_VALUE, INVALID_VALUE,
			INVALID_VALUE};
		u8 operand_count = 0;
		// Parameter/BindGpr slot, no-effect kind, shift amount, or immediate
		// cycle delta.
		u32 immediate = 0;
		// Constants use the complete 64-bit payload. Address constants consume
		// its low word.
		u64 literal = 0;
		// Owning guest instruction. Parameters and edge-only constants use the
		// block PC or transfer source PC.
		u32 source_pc = 0;
	};

	struct StateMap
	{
		std::array<ValueId, 32> gpr{};
		ValueId hi = INVALID_VALUE;
		ValueId lo = INVALID_VALUE;
		// Internal byte-offset representation of the EE funnel-shift amount.
		ValueId sa = INVALID_VALUE;
		std::array<ValueId, 32> fpr{};
		// The EE implements FCR0 and FCR31 plus one scalar accumulator. ACCflag
		// is PCSX2's internal accumulator-overflow state and must cross every
		// exact fallback even before arithmetic is admitted.
		ValueId fcr0 = INVALID_VALUE;
		ValueId fcr31 = INVALID_VALUE;
		ValueId acc = INVALID_VALUE;
		ValueId acc_flag = INVALID_VALUE;
		// VU0 state which an admitted macro region can read or write is one
		// mechanically mapped register file.  VI entries are not duplicated as
		// named STATUS/MAC/Q/VPU_STAT fields: CFC2/CTC2, macro arithmetic and the
		// micro-mode boundary must all observe the same SSA value.  PCSX2's scalar
		// interpreter flags and four microVU flag instances remain explicit because
		// they are provider-visible mirrors, not aliases of VI[16..18].
		std::array<ValueId, 32> vu0_vf{};
		ValueId vu0_acc = INVALID_VALUE;
		ValueId vu0_macflag = INVALID_VALUE;
		ValueId vu0_statusflag = INVALID_VALUE;
		ValueId vu0_clipflag = INVALID_VALUE;
		ValueId vu0_q = INVALID_VALUE;
		std::array<ValueId, 32> vu0_vi{};
		std::array<ValueId, 4> vu0_micro_macflags{};
		std::array<ValueId, 4> vu0_micro_clipflags{};
		std::array<ValueId, 4> vu0_micro_statusflags{};
		ValueId cycle = INVALID_VALUE;
		// Ordered, non-architectural memory state. It prevents loads and stores
		// from being reordered across each other while remaining absent from the
		// canonical EE register image.
		ValueId memory_effect = INVALID_VALUE;
	};

	enum class MemoryAccessKind : u8
	{
		LoadS8,
		LoadU8,
		LoadS16,
		LoadU16,
		LoadS32,
		LoadU32,
		// LWC1/SWC1 transfer one raw architectural FPR word. They remain
		// distinct from integer word accesses so the verifier cannot bind a
		// memory result to the wrong register file.
		LoadF32Bits,
		Load64,
		Load128,
		// LQC2 has the same 128-bit memory width as LQ, but names the VU0 VF
		// register file and therefore carries a distinct verifier domain.
		LoadVu0Vector,
		Store8,
		Store16,
		Store32,
		StoreF32Bits,
		Store64,
		Store128,
		StoreVu0Vector,
	};

	enum class MemoryProbeResult : u8
	{
		Direct,
		Handler,
		Translation,
		SelfModifyingCode,
	};

	// Shared decoded-memory facts for the interpreter and every generated
	// backend. Backends consume these semantics instead of decoding source
	// opcodes a second time.
	bool IsMemoryLoad(MemoryAccessKind kind);
	u32 MemoryAccessWidth(MemoryAccessKind kind);
	u32 MemoryAlignmentMask(MemoryAccessKind kind);
	bool IsQuadMemoryAccess(MemoryAccessKind kind);

	// Shared exact scalar authority for VU0 macro FMAC primitives. Semantic
	// kernels compose these same operations as the Region IR interpreter, so a
	// batch lowering cannot silently acquire a second normalization, rounding,
	// flag, or mask contract. Raw multiply/add remain separate calls: contraction
	// is not PS2-equivalent under the selected VU numeric contract.
	u128 NormalizeVu0Vector(const u128& value, bool overflow_clamp);
	u128 BroadcastVu0Lane(const u128& value, u32 lane);
	u128 EvaluateVu0RawBinary(Opcode opcode, const u128& left,
		const u128& right);
	u128 ClampVu0FmacResult(const u128& raw, u32 mask,
		bool overflow_clamp);
	u32 EvaluateVu0MacFlags(const u128& raw, u32 mask);
	u32 EvaluateVu0StatusFlags(u32 mac);
	u128 MergeVu0Masked(const u128& old_value, const u128& new_value,
		u32 mask);
	u32 SyncVu0StatusControl(u32 old_status, u32 current_status);

	struct MemoryRequest
	{
		u32 source_pc = 0;
		u32 address = 0;
		MemoryAccessKind kind = MemoryAccessKind::LoadS8;
	};

	// The Region IR interpreter is product-disabled, but its memory contract is
	// also the contract the A32 backend must implement. Probe must be free of
	// guest-visible effects. Read/write are called only after Direct and must
	// perform exactly one access of the requested width.
	struct RegionMemoryInterface
	{
		void* context = nullptr;
		MemoryProbeResult (*probe)(void* context,
			const MemoryRequest& request) = nullptr;
		bool (*read)(void* context, const MemoryRequest& request,
			u128* value) = nullptr;
		bool (*write)(void* context, const MemoryRequest& request,
			const u128& value) = nullptr;
	};

	enum class ExitReason : u8
	{
		RegionBoundary,
		UnsupportedOpcode,
		MemoryObserver,
		MemoryAlignment,
		MemoryHandler,
		MemoryTranslation,
		SelfModifyingCode,
		HelperObserver,
		UnsupportedControlFlow,
		// The represented state is immediately before an instruction which can
		// raise an EE architectural exception. The fallback owns both the
		// exceptional and non-exceptional outcomes; it may not use a generic
		// returning-helper continuation.
		ExceptionObserver,
		EventHorizon,
		// The generated region has not observed an event. Its entry proof could
		// not establish that every internal PCSX2 timing boundary precedes the
		// current horizon, so tier zero must execute once from canonical entry.
		EventBudgetFallback,
		// The entry state is semantically valid, but the exact natural-loop trip
		// count is too small to amortize this Cortex-A9 region's mechanically
		// required range, event, and source-ownership proof. No guest instruction
		// has executed; tier zero owns the complete source fragment once.
		ProfitabilityFallback,
		// A verifier-derived compact representation was not true of the external
		// canonical entry value. No guest instruction or architectural effect has
		// occurred; tier zero owns the complete source fragment once.
		EntryStateFallback,
	};

	// A transfer owns one complete canonical-state map. If target_block is
	// valid, the same map supplies its block parameters and the event-horizon
	// exit at this edge. Otherwise it is an ordinary side exit.
	struct Transfer
	{
		u32 target_block = INVALID_BLOCK;
		ValueId pc = INVALID_VALUE;
		StateMap state{};
		ExitReason external_reason = ExitReason::RegionBoundary;
		// A side exit before an observer remains inside the original PCSX2
		// recompiler block. Its architectural cycle is therefore still the block-
		// entry value and the fallback must append this fixed-point raw prefix,
		// execute the observer/remainder, then scale once at the real block edge.
		// Such an exit must not perform an event-horizon test first, including
		// when the prefix is empty and pending_raw_cycles is zero.
		bool cycle_commit_deferred = false;
		u32 pending_raw_cycles = 0;
		// True only at a real PCSX2 scheduler boundary. A cycle-publishing A32
		// physical continuation may deliberately leave this false.
		bool event_horizon_check = true;
		// A register jump is internal only when the verifier proves its dynamic
		// source still equals this exact architectural target. The first supported
		// case is an attested bounded direct callee returning through unchanged r31.
		bool register_target_proven = false;
		u32 proven_register_target_pc = 0;
	};

	enum class TerminatorKind : u8
	{
		Transfer,
		Branch,
		Jump,
		RegisterJump,
	};

	struct Terminator
	{
		TerminatorKind kind = TerminatorKind::Transfer;
		// Likely branches execute the represented delay slot only on the taken
		// edge. Their two transfers therefore own distinct state/cycle maps.
		bool likely = false;
		ValueId condition = INVALID_VALUE;
		Transfer taken{};
		Transfer not_taken{};
		// A shared direct callee may be reached from several statically proven JAL
		// sites. Its JR r31 has one dynamic source but a bounded set of internal
		// return PCs. Each entry is a verifier-proven alternative using the same
		// post-callee state; taken remains the exact unmatched-target side exit.
		std::vector<Transfer> register_targets;
		// For Branch and Jump these identify the indivisible control/delay
		// source pair. Transfer has neither.
		u32 branch_pc = 0;
		u32 delay_slot_pc = 0;
	};

	// Visit every internal CFG edge and its stable parallel-copy index. The primary
	// edge is zero, a conditional not-taken edge is one, and shared-callee return
	// targets start at one. External side exits are deliberately excluded.
	template <typename Callback>
	bool VisitInternalTransfers(const Terminator& terminator, Callback&& callback)
	{
		if (terminator.taken.target_block != INVALID_BLOCK &&
			!callback(terminator.taken, static_cast<u8>(0)))
		{
			return false;
		}
		if (terminator.kind == TerminatorKind::Branch &&
			terminator.not_taken.target_block != INVALID_BLOCK &&
			!callback(terminator.not_taken, static_cast<u8>(1)))
		{
			return false;
		}
		if (terminator.kind == TerminatorKind::RegisterJump)
		{
			for (size_t index = 0; index < terminator.register_targets.size(); index++)
			{
				if (terminator.register_targets[index].target_block != INVALID_BLOCK &&
					!callback(terminator.register_targets[index],
						static_cast<u8>(index + 1)))
				{
					return false;
				}
			}
		}
		return true;
	}

	inline bool TerminatorTargetsBlock(const Terminator& terminator, u32 target)
	{
		bool found = false;
		(void)VisitInternalTransfers(terminator,
			[&](const Transfer& transfer, u8) {
				found |= transfer.target_block == target;
				return !found;
			});
		return found;
	}

	// Every fallible memory operation owns the same complete architectural
	// transfer contract as a control-flow or conditional-exception exit. The
	// backend may classify the runtime failure as alignment, handler,
	// translation, or SMC, but it must materialize this exact pre-access state,
	// resume PC, and fixed-point cycle debt for all of them.
	struct MemoryExit
	{
		ValueId operation = INVALID_VALUE;
		Transfer transfer{};
	};

	// A synchronization observer which may reject an instruction before it has
	// produced any architectural effect.  The operation identifies the exact IR
	// node which performs the runtime test; the transfer owns the mechanically
	// derived pre-instruction state, restart PC, and fixed-point cycle debt.
	struct ObserverExit
	{
		ValueId operation = INVALID_VALUE;
		Transfer transfer{};
	};

	struct SourceInstruction
	{
		u32 pc = 0;
		u32 opcode = 0;
		bool delay_slot = false;
	};

	struct Block
	{
		u32 pc = 0;
		StateMap parameters{};
		std::vector<Node> nodes;
		std::vector<SourceInstruction> source;
		u32 raw_cycle_cost = 0;
		u32 scaled_cycle_cost = 0;
		// Equal to the primary cost except for a likely branch, where this
		// excludes the annulled delay slot.
		u32 not_taken_raw_cycle_cost = 0;
		u32 not_taken_scaled_cycle_cost = 0;
		// Conditional exceptional/observer exits are explicit executable IR
		// nodes. Each ExitIfTrue owns exactly one entry here, and the verifier
		// derives its complete state, resume PC, and cycle debt from source.
		std::vector<Transfer> guarded_exits;
		// One entry for every MemoryLoad/MemoryStore node. Unlike a guarded exit,
		// the generated backend supplies the runtime condition and concrete reason.
		std::vector<MemoryExit> memory_exits;
		// One entry for every Vu0RequireIdle node.  This is separate from memory
		// fallback because VU0-busy is a COP2 synchronization observation even when
		// the owning instruction is LQC2/SQC2.
		std::vector<ObserverExit> observer_exits;
		Terminator terminator{};
	};

	// One immutable timing fragment from the existing Vita EE provider. A
	// PCSX2 source block can be emitted as several A32 fragments when the host
	// code budget is exhausted; all such fragments share dependency_start_pc /
	// dependency_instruction_count. charged_scaled_cycles_before is the exact
	// architectural cycle charge already published by preceding fragments.
	// scheduler_test_at_end is separate because PCSX2 short splits and Vita's
	// cycle-proven A32 continuations publish cycles without polling at that seam.
	struct SourceBlockContract
	{
		u32 start_pc = 0;
		u32 instruction_count = 0;
		u32 dependency_start_pc = 0;
		u32 dependency_instruction_count = 0;
		u32 charged_scaled_cycles_before = 0;
		bool scheduler_test_at_end = true;
	};

	// Tier zero can legitimately publish both a wide fallthrough owner and an
	// independently entered suffix owner ending at the same guest PC. A region
	// needs one disjoint ownership partition, but may synthesize the prefix only
	// after the owning EE compiler proves that introducing the split preserves
	// PCSX2's complete scaled-cycle timeline.
	enum class SuffixPartitionFailure : u8
	{
		None,
		InvalidRange,
		NotExactSuffix,
		NonStandaloneDependency,
		SchedulerMismatch,
		CycleTimeline,
	};

	SuffixPartitionFailure BuildCycleProvenSuffixPartition(
		const SourceBlockContract& wide, const SourceBlockContract& suffix,
		bool cycle_timeline_proven, SourceBlockContract* prefix);

	// One disjoint immutable guest-code range. Region source is represented as
	// ordered spans rather than one artificial address extent so a bounded region
	// may own a caller and a direct callee without attesting the untouched gap.
	// Spans are byte-exact snapshots and must be sorted, non-overlapping, aligned,
	// and collectively fit max_source_instructions.
	struct SourceSpan
	{
		u32 base_pc = 0;
		std::vector<u32> words;
	};

	// Canonicalize one additional immutable source owner into an ordered,
	// disjoint source image. PCSX2 tier-zero fragments can expose partially
	// overlapping dependency ranges when independently discovered entries were
	// split from different original blocks. Those ranges describe the same guest
	// bytes and therefore form one ownership union, not two overlapping IR spans.
	// The operation is transactional and rejects disagreeing snapshots so an SMC
	// change observed while a region is being assembled cannot be hidden.
	enum class SourceSpanMergeFailure : u8
	{
		None,
		InvalidSource,
		ConflictingSource,
		SourceLimit,
	};

	SourceSpanMergeFailure MergeImmutableSourceSpan(
		std::vector<SourceSpan>* spans, SourceSpan incoming,
		u32 max_source_instructions,
		u32* required_source_instructions = nullptr);

	// One mechanically proven direct-call/leaf-return pair. This metadata is part
	// of the verified program contract; it is never inferred from a title, PC, or
	// content identity. The caller JAL, callee JR r31, both delay slots, unchanged
	// link value, and internal return target are all re-derived by Verify().
	struct DirectCallContract
	{
		u32 call_pc = 0;
		u32 callee_pc = 0;
		u32 return_jump_pc = 0;
		u32 return_pc = 0;
	};

	struct LiftOptions
	{
		// Interpreter.cpp::execI() multiplies each opcode cost by this value,
		// derived from CP0.Config bit 18. Valid EE values are one and two.
		u32 cycle_factor = 2;
		s8 ee_cycle_rate = 0;
		// PCSX2's Goemon TLB gamefix adds observable jump/JR behavior. Static
		// jumps fail closed while it is active until that helper contract is IR.
		bool goemon_tlb_hack = false;
		// Vita's playable product fixes this false. CACHE is a no-op only under
		// PCSX2's recompiler contract; cache-emulation validation must fail closed.
		bool ee_cache_enabled = false;
		// PCSX2 CHECK_VU_OVERFLOW(0). This is part of the selected VU numeric
		// contract and therefore cannot be read implicitly by disconnected IR.
		bool vu0_overflow_clamp = true;
		// Product-disabled Phase 4 proof: guard final COP1 O/U correction with an
		// exact pre-instruction cold exit so the ordinary reducible path may carry
		// normalized values and demanded flags lazily. This remains false until
		// cold exits are compact enough for general region publication.
		bool cop1_lazy_ou_guards = false;
		u32 max_blocks = 8;
		u32 max_source_instructions = 64;
		// Zero preserves the original intraprocedural CFG. A nonzero bound permits
		// only unique attested direct-call leaves with statically proven returns.
		u32 max_direct_calls = 0;
	};

	struct Program
	{
		std::vector<SourceSpan> source_spans;
		// Empty only for the explicitly unattested validation overload of Lift().
		// Product compilation must use LiftWithSourceBlocks() or the multi-span
		// LiftWithSourceSpans() equivalent.
		std::vector<SourceBlockContract> source_blocks;
		std::vector<DirectCallContract> direct_calls;
		LiftOptions options{};
		u32 entry_block = INVALID_BLOCK;
		u32 value_count = 0;
		std::vector<Block> blocks;
	};

	// Valid only for a Program which has passed Verify().  Verify mechanically
	// proves the complete direct-callee CFG, unchanged r31 on every path, exact
	// incoming JAL ownership, and a one-to-one set of internal return targets.
	// Allocated backends may therefore omit the otherwise general unmatched-JR
	// side exit for this block; the semantic IR/interpreter deliberately retains
	// that fallback as part of the architectural register-jump contract.
	bool HasExhaustiveDirectReturnTargets(const Program& program, u32 block_index);

	enum class LiftFailure : u8
	{
		None,
		InvalidSource,
		EntryOutsideSource,
		SourceLimit,
		BlockLimit,
		MissingDelaySlot,
		BranchInDelaySlot,
		OverlappingSource,
		SourceBlockContract,
		DirectCallContract,
		ValueLimit,
		InternalError,
	};

	// Stable, allocation-free detail for LiftFailure::SourceBlockContract.
	// Product discovery records this value in cold failure telemetry, while the
	// host and Cortex-A9 fixtures use it to distinguish malformed ownership from
	// a valid but incompatible tier-zero partition.  Keep these semantic rather
	// than tied to a workload address.
	enum class LiftFailureDetail : u8
	{
		None,
		SourceBlockInvalidRange,
		SourceBlockOverlap,
		SourceBlockMissingEntry,
		SourceBlockChargedEntry,
		SourceBlockUntestedEntryPredecessor,
		SourceBlockMissingContinuation,
		SourceBlockDiscontinuousDependency,
		SourceBlockIncompleteDependency,
		SourceBlockMissingLeader,
		SourceBlockCrossesContract,
		SourceBlockControlNotAtEnd,
	};

	// Stable, allocation-free stage for LiftFailure::InternalError.  The lifter
	// deliberately keeps unsupported guest semantics in the ordinary LiftFailure
	// categories; InternalError therefore means that a supposedly supported unit
	// failed while constructing or mechanically verifying the IR.  Product cold
	// telemetry records this stage so a broad reducible candidate cannot collapse
	// into an address-shaped "failed to lift" diagnosis.
	enum class LiftInternalStage : u8
	{
		None,
		RawBlockSet,
		Instruction,
		BranchCondition,
		DelaySlot,
		Verification,
	};

	enum class VerifyFailure : u8
	{
		None,
		InvalidProgram,
		DuplicateBlockPc,
		InvalidEntry,
		ValueIdMismatch,
		ParameterContract,
		OperandOutOfRange,
		OperandNotLocal,
		OperandType,
		ResultType,
		StateMapMismatch,
		SourceMismatch,
		SourceOverlap,
		SourceBlockContract,
		DirectCallContract,
		CycleMismatch,
		ExitContractMismatch,
		ControlFlowMismatch,
		UnreachableBlock,
	};

	struct LiftResult
	{
		Program program{};
		LiftFailure failure = LiftFailure::None;
		LiftFailureDetail failure_detail = LiftFailureDetail::None;
		LiftInternalStage internal_stage = LiftInternalStage::None;
		VerifyFailure verify_failure = VerifyFailure::None;
		u32 failure_pc = 0;

		explicit operator bool() const { return failure == LiftFailure::None; }
	};

	struct VerifyResult
	{
		VerifyFailure failure = VerifyFailure::None;
		u32 block = INVALID_BLOCK;
		u32 node = UINT32_MAX;
		std::string detail;

		explicit operator bool() const { return failure == VerifyFailure::None; }
	};

	struct CanonicalState
	{
		std::array<u128, 32> gpr{};
		u128 hi{};
		u128 lo{};
		u32 sa = 0;
		std::array<u32, 32> fpr{};
		u32 fcr0 = 0;
		u32 fcr31 = 0;
		u32 acc = 0;
		u32 acc_flag = 0;
		std::array<u128, 32> vu0_vf{};
		u128 vu0_acc{};
		u32 vu0_macflag = 0;
		u32 vu0_statusflag = 0;
		u32 vu0_clipflag = 0;
		u32 vu0_q = 0;
		std::array<u32, 32> vu0_vi{};
		std::array<u32, 4> vu0_micro_macflags{};
		std::array<u32, 4> vu0_micro_clipflags{};
		std::array<u32, 4> vu0_micro_statusflags{};
		u32 pc = 0;
		u64 cycle = 0;
	};

	struct InterpretOptions
	{
		u64 next_event_cycle = UINT64_MAX;
		u32 max_block_executions = 4096;
		const RegionMemoryInterface* memory = nullptr;
	};

	struct InterpretResult
	{
		bool completed = false;
		ExitReason reason = ExitReason::RegionBoundary;
		u32 blocks_executed = 0;
		// Dynamic guest instructions whose complete architectural effects were
		// committed. An observer instruction at a side exit is not included.
		u32 source_instructions_executed = 0;
		// When execution stops before an observer, canonical cycle remains at the
		// original PCSX2 block entry. This is the exact fixed-point recompiler cost
		// already executed in that block. A product continuation must append the
		// remaining source cost and scale once at the original block edge.
		bool cycle_commit_deferred = false;
		u32 pending_raw_cycles = 0;
		u32 memory_address = 0;
		std::string error;
	};

	// Pure PCSX2 cycle scaling contract, parameterized so product-disabled
	// validation does not need to read global configuration.
	u32 RawRecompilerCycles(u32 opcode, u32 cycle_factor);
	u32 ScaleBlockCycles(u32 raw_cycles, s8 ee_cycle_rate);

	LiftResult Lift(u32 source_base_pc, const u32* source_words,
		u32 source_word_count, u32 entry_pc,
		const LiftOptions& options = {});
	LiftResult LiftWithSourceBlocks(u32 source_base_pc, const u32* source_words,
		u32 source_word_count, const SourceBlockContract* source_blocks,
		u32 source_block_count, u32 entry_pc,
		const LiftOptions& options = {});
	LiftResult LiftWithSourceSpans(const SourceSpan* source_spans,
		u32 source_span_count, const SourceBlockContract* source_blocks,
		u32 source_block_count, u32 entry_pc,
		const LiftOptions& options = {},
		const u32* execution_owner_pcs = nullptr,
		u32 execution_owner_count = 0);
	bool ProgramContainsPc(const Program& program, u32 pc);
	bool ReadProgramSourceWord(const Program& program, u32 pc, u32* word);
	u32 ProgramSourceInstructionCount(const Program& program);
	VerifyResult Verify(const Program& program);
	InterpretResult Interpret(const Program& program, const CanonicalState& input,
		CanonicalState* output,
		const InterpretOptions& options = {});
} // namespace VitaEE::RegionIR
