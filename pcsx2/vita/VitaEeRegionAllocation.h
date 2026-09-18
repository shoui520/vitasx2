// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "pcsx2/vita/VitaEeRegionExecutionPlan.h"

#include <vector>

namespace VitaEE::RegionAllocation
{
	enum class LocationKind : u8
	{
		None,
		Immediate,
		// A verifier-proven unchanged architectural Parameter. Index is its
		// RegionExecution state slot; the backend loads demanded words directly
		// from canonical state instead of carrying them around the internal CFG.
		CanonicalState,
		Core,
		FixedCycle,
		VfpS,
		NeonQ,
		Spill,
	};

	struct Location
	{
		LocationKind kind = LocationKind::None;
		// Core, VfpS, and NeonQ use a zero-based logical register. CanonicalState
		// uses a RegionExecution state slot. Spill uses a byte offset in the
		// region-private, naturally aligned frame.
		u16 index = 0;
		u8 words = 0;
		// Architectural word components carried by this packed physical value.
		// Full NeonQ values normally use 0x0f; a two-word low EE fragment uses
		// 0x03. Zero is reserved for None and legacy-independent test inputs.
		u8 word_mask = 0;

		bool operator==(const Location& other) const
		{
			return kind == other.kind && index == other.index && words == other.words &&
			       word_mask == other.word_mask;
		}
		bool operator!=(const Location& other) const { return !(*this == other); }
	};

	enum class RepresentationKind : u8
	{
		None,
		Immediate,
		CoreWords,
		FixedCycle,
		VfpWord,
		NeonQ,
	};

	struct Representation
	{
		RepresentationKind kind = RepresentationKind::None;
		// Architectural 32-bit components which occupy physical storage.  A
		// consumer's complete semantic demand is this mask plus the matching
		// Plan::value_rematerialized_word_masks entry. CoreWords packs the stored
		// components in ascending word order; NeonQ retains the natural four-lane
		// layout.
		u8 word_mask = 0;
		u8 words = 0;
	};

	enum class RematerializationKind : u8
	{
		None,
		SignExtendLow32,
		ZeroExtendLow32,
	};

	struct Interval
	{
		RegionIR::ValueId value = RegionIR::INVALID_VALUE;
		RegionIR::ValueType type = RegionIR::ValueType::Void;
		u32 block = RegionIR::INVALID_BLOCK;
		u32 begin = 0;
		u32 end = 0;
		u32 uses = 0;
		u8 exit_word_mask = 0;
		Location location{};
	};

	struct EdgeMove
	{
		u32 source_block = RegionIR::INVALID_BLOCK;
		u32 target_block = RegionIR::INVALID_BLOCK;
		// Zero is the primary/taken transfer; one is the branch not-taken transfer.
		u8 edge_index = 0;
		RegionIR::ValueId source = RegionIR::INVALID_VALUE;
		RegionIR::ValueId target_parameter = RegionIR::INVALID_VALUE;
		Location source_location{};
		Location target_location{};
		u8 word_mask = 0;
	};

	struct EntryLow32Guard
	{
		RegionIR::ValueId parameter = RegionIR::INVALID_VALUE;
		u16 state_slot = 0;
		RematerializationKind extension = RematerializationKind::None;
	};

	enum class EdgeCopyStepKind : u8
	{
		Copy,
		SaveScratch,
	};

	// Internal CFG edges are SSA parallel copies. This is the destructive order
	// which an A32 backend may execute without overwriting a still-live source.
	// A SaveScratch step has no target_parameter; its target_location names the
	// one region-private scratch slot subsequently used by Copy steps.
	struct EdgeCopyStep
	{
		EdgeCopyStepKind kind = EdgeCopyStepKind::Copy;
		u32 source_block = RegionIR::INVALID_BLOCK;
		u32 target_block = RegionIR::INVALID_BLOCK;
		u8 edge_index = 0;
		RegionIR::ValueId source = RegionIR::INVALID_VALUE;
		RegionIR::ValueId target_parameter = RegionIR::INVALID_VALUE;
		Location source_location{};
		Location target_location{};
		u8 word_mask = 0;
	};

	enum class BuildFailure : u8
	{
		None,
		InvalidExecutionPlan,
		InvalidValue,
		EdgeCopyConflict,
		SpillCapacity,
	};

	struct EdgeCopyScheduleResult
	{
		std::vector<EdgeCopyStep> steps;
		BuildFailure failure = BuildFailure::None;
		u32 block = RegionIR::INVALID_BLOCK;
		RegionIR::ValueId value = RegionIR::INVALID_VALUE;
		u32 spill_bytes = 0;
		u32 scratch_offset = 0;
		u32 scratch_bytes = 0;

		explicit operator bool() const { return failure == BuildFailure::None; }
	};

	struct Options
	{
		// Logical core words are mapped by the backend onto the registers its entry
		// ABI owns. Canonical callable execution reserves r1-r2 for cycle; the
		// persistent countdown ABI can reclaim the dead high word.
		u8 core_register_words = 8;
		// Scalar VFP values normally use a bank which the backend maps to s16-s29.
		// This never aliases logical q0-q5: q0-q3 occupy s0-s15, while logical
		// q4-q5 are remapped to physical q12-q13 (d24-d27). When demand analysis
		// proves that a region owns no NEON value, the allocator may prepend the
		// otherwise idle caller-saved s0-s15 bank.
		u8 vfp_s_registers = 14;
		// Physical S register backing logical VfpS zero whenever NEON values are
		// present. RegionA32 may compare disjoint mixed-bank partitions; the complete
		// [first, first + count) range must avoid every physical NEON allocation.
		u8 first_vfp_s = 16;
		bool use_low_vfp_bank_when_neon_unused = true;
		// Store F32Bits SSA values as ordinary core words. This target partition is
		// useful when exact VU0 arithmetic needs the scalar-addressable q0-q6 bank;
		// COP1 arithmetic still executes in VFP scratch lanes, while its bit values
		// avoid repeated frame spill loads/stores. Semantics are unchanged.
		bool allocate_vfp_words_in_core = false;
		// q0-q3 and logical q4-q7 mapped to physical q12-q15 are caller-saved.
		u8 neon_q_registers = 8;
		// Prefix of the logical NeonQ bank which the target can also name as four
		// scalar VFP lanes.  Exact VU0 macro arithmetic observes FPSCR Chop/Zero and
		// therefore benefits materially when its raw operands and destination land
		// in this prefix.  Zero disables the preference.  This is a target register-
		// file fact; semantic correctness never depends on satisfying it.
		u8 scalar_addressable_neon_q_registers = 0;
		u32 max_spill_bytes = 4096;
		// Memory operations named by the already verified entry-range certificate
		// cannot reach their per-access fallback after the body begins. The IR keeps
		// those exits as semantic documentation; allocation may omit only their
		// liveness uses. The generated entry fallback still owns the complete
		// untouched canonical state when the certificate fails.
		const std::vector<RegionIR::ValueId>* preflighted_memory_operations = nullptr;
		// A persistent-region entry may prove VU0 idle before any guest effect.
		// Within the represented macro surface VPU_STAT cannot change, so each
		// instruction-local guard is then an exact vector identity.
		bool vu0_idle_entry_proven = false;
		// RegionA32 may replace the individual PCSX2 scheduler checks on internal
		// edges with one exact aggregate iteration proof. Internal transfers which
		// do not return to this header then have no executable event exit; the header
		// backedge remains the single observer. INVALID_BLOCK disables this fact.
		u32 aggregate_cycle_header_block = RegionIR::INVALID_BLOCK;
		// A backend may retain an entry-proven host address for these operations in
		// private region-owned registers.  Their semantic MemoryLoad/MemoryStore and
		// exit contracts remain in IR, but the address-expression operands are no
		// longer executable body uses and therefore must not consume allocation.
		const std::vector<RegionIR::ValueId>* hoisted_memory_operations = nullptr;
		// Values in this ordered set own the highest logical core words for the
		// complete region. Ordinary interval allocation cannot use those words;
		// phi coalescing may propagate a resident location only when every incoming
		// edge supplies that same value. This is the allocation-time contract needed
		// by entry-hoisted immutable loads: assigning a register after edge-copy
		// scheduling would leave previously coalesced copies missing.
		const std::vector<RegionIR::ValueId>* region_resident_core_values = nullptr;
		// Canonical execution carries a 64-bit cycle. The persistent dispatcher
		// instead owns the exact signed low-word distance to nextEventCycle; its
		// backend reconstructs and stores the canonical high word only on a cold
		// exit, leaving the otherwise dead high host word available to allocation.
		u8 fixed_cycle_word_mask = 0x3;
		// Conditional entry representations are useful only when the owner can
		// reject before the first guest instruction and tail directly into its
		// exact fallback. The product persistent dispatcher provides that seam;
		// ordinary callable compilation keeps the full architectural word.
		bool enable_guarded_entry_low32 = false;
	};

	struct Plan
	{
		std::vector<Interval> intervals;
		std::vector<Location> value_locations;
		// Storage/rematerialization root for verifier-retained typed identity nodes.
		// Every entry names itself unless allocation proves that its demanded word
		// subset is byte-identical to an earlier immutable SSA value. Backends use
		// this root when an alias inherits Immediate storage, so a cold exit reads the
		// root literal rather than the identity node's non-literal payload.
		std::vector<RegionIR::ValueId> value_aliases;
		// Backward, component-sensitive demand for each SSA value. One bit names
		// one 32-bit word. This is the representation contract which keeps scalar
		// low-half EE work out of NEON until a real full-width consumer exists.
		std::vector<u8> value_word_demands;
		std::vector<Representation> value_representations;
		// Exact semantic fact independent of physical allocation: the I64 high
		// word is the sign- or zero-extension of its low word. Consumers may use
		// this to narrow an operation even when another observer still requires
		// the high word to have ordinary storage.
		std::vector<RematerializationKind> value_low32_extensions;
		// Architecturally demanded components which have no independent physical
		// storage. The first exact facts are I64 word 1 of SignExtend32To64 and
		// ZeroExtend32To64; both are reconstructed from the physically retained low
		// word. This is an allocation fact, never a weakening of an exit state map.
		std::vector<u8> value_rematerialized_word_masks;
		std::vector<RematerializationKind> value_rematerializations;
		// Conditional extension facts selected for physical omission. Every entry
		// below must be checked before the region executes any guest instruction;
		// otherwise all values dependent on it retain their ordinary high word.
		std::vector<EntryLow32Guard> entry_low32_guards;
		// One means the value is a comparison consumed only by its owning branch
		// terminator. The backend must form flags at the edge and must not allocate
		// or materialize the otherwise dead I1 result.
		std::vector<u8> folded_branch_conditions;
		// One means this EffectiveAddress32 has exactly one executable use as a
		// memory address. The backend forms it in a lowering scratch register at
		// that use instead of consuming a region register or spill slot.
		std::vector<u8> folded_effective_addresses;
		// Exact typed I32 identities discovered after semantic verification.  The
		// value aliases the named earlier SSA value modulo 2^32, so allocation may
		// route all consumers to that value and the backend emits no arithmetic for
		// the aliasing node.  This is deliberately value-based rather than an EE
		// opcode/PC matcher; guarded overflow observers and intermediate state exits
		// retain their own independent demands.
		std::vector<RegionIR::ValueId> exact_i32_aliases;
		u32 folded_exact_i32_values = 0;
		// An ADD32 immediately followed by its verifier-owned signed-overflow
		// predicate and ExitIfTrue can publish AArch32 V directly.  The semantic
		// predicate and guarded exit remain in IR; allocation gives the boolean no
		// storage and the backend emits ADDS/SUBS plus one VS cold branch.
		std::vector<RegionIR::ValueId> signed_overflow_for_flagged_add;
		std::vector<RegionIR::ValueId> flagged_add_for_signed_overflow;
		// A normalization aliases an earlier normalized value only when immutable
		// SSA dataflow proves the input already satisfies PCSX2's fpuDouble()
		// contract. This includes dominance-local CSE and normalized results carried
		// through source-backed internal CFG edges; the externally reachable entry
		// parameters remain unknown.
		std::vector<RegionIR::ValueId> cop1_normalize_alias;
		// A normalization of an immutable canonical entry FPR may execute once
		// after entry guards and remain resident across every internal CFG edge
		// whose execution-plan origin is that same entry word. The immutable bit
		// keeps allocation lifetime and backend placement identical.
		std::vector<u8> cop1_hoisted_normalize;
		// A multi-block hoist is also pinned to one allocation-owned VFP lane for
		// the whole region. Ordinary block-local intervals cannot reuse that lane.
		std::vector<u8> cop1_region_resident_normalize;
		// A verifier-owned exceptional-result predicate immediately guards an
		// exact pre-instruction cold exit. Past that guard the clamp is identity,
		// and repeated O/U clears may alias an already-cleared predecessor.
		std::vector<RegionIR::ValueId> cop1_exception_for_guarded_raw;
		std::vector<RegionIR::ValueId> cop1_guarded_raw_for_exception;
		std::vector<RegionIR::ValueId> cop1_guarded_clamp_alias;
		// Clamp and FCR31 publication for one arithmetic instruction share the
		// same raw result and are emitted as one multi-result operation. These maps
		// move both physical input uses to that common point without weakening the
		// verifier's decomposed semantic graph.
		std::vector<RegionIR::ValueId> cop1_fused_flag_for_clamp;
		std::vector<RegionIR::ValueId> cop1_fused_clamp_for_flag;
		// Verifier-retained VU0 idle guards alias their vector operand only after a
		// generated entry certificate has made every observer exit unreachable.
		std::vector<RegionIR::ValueId> vu0_idle_alias;
		// Dominance-local common subexpressions for the pure VU bit normalizer.
		std::vector<RegionIR::ValueId> vu0_normalize_alias;
		// Loop-invariant VU0 input normalization emitted once in the entry prefix.
		std::vector<u8> vu0_hoisted_normalize;
		// A full architectural xyzw merge is exactly its new-value operand.
		std::vector<RegionIR::ValueId> vu0_merge_alias;
		// A STATUS reduction whose intermediate full MAC word has no executable
		// observer may classify the verified raw FMAC bits directly.
		std::vector<RegionIR::ValueId> vu0_status_direct_raw;
		// A raw multiply whose sole executable consumer is the matching raw add may
		// remain a verified IR node without owning a materialized vector. The add
		// lowers both operations together and preserves the scalar-VFP rounding point.
		std::vector<RegionIR::ValueId> vu0_add_fused_mul;
		// A lane broadcast with one raw-multiply consumer is an addressing mode for
		// scalar VFP lowering, not a materialized four-word value.
		std::vector<RegionIR::ValueId> vu0_folded_broadcast_source;
		// Consecutive VU macro operations update VI STATUS as
		//   (old & 0xfc0) | current | (current << 6).
		// When an intermediate value has no observer or executable consumer other
		// than the next synchronization, retain the explicit verifier graph but let
		// the final synchronization apply the equivalent accumulated sticky update.
		std::vector<RegionIR::ValueId> vu0_sync_folded_predecessor;
		std::vector<RegionIR::ValueId> vu0_sync_folded_into;
		// An unobserved intermediate STATUS node whose sole executable consumer is
		// this SYNCMSFLAGS recurrence. The sync consumes the raw vector directly and
		// updates only the sticky control bits on its finite hot path; its cold path
		// still materializes the complete PCSX2 status classification.
		std::vector<RegionIR::ValueId> vu0_sync_direct_raw;
		std::vector<RegionIR::ValueId> vu0_status_fused_sync;
		// A full-mask clamp may be deferred to the exceptional leaf of a direct
		// raw STATUS classifier. Normal finite raw bits are already the exact
		// architectural result, so the clamp aliases raw on the hot path.
		std::vector<RegionIR::ValueId> vu0_clamp_deferred_to_status;
		std::vector<RegionIR::ValueId> vu0_status_deferred_clamp;
		std::vector<RegionIR::ValueId> vu0_clamp_deferred_to_mac;
		std::vector<RegionIR::ValueId> vu0_mac_deferred_clamp;
		// A memory load value may alias a compiler-owned state parameter only when
		// the ordered memory-effect chain proves that its reaching write is an exact
		// same-address, same-representation store, with only read effects in between,
		// and the stored value is carried across the unique internal CFG edge. The
		// store remains
		// executable (including its translation and pre-write SMC checks); the load
		// and its now-unreachable observer exit emit no code.  This is a generic
		// memory-SSA fact, never a source-PC or instruction-sequence match.
		std::vector<RegionIR::ValueId> forwarded_memory_load_alias;
		std::vector<u8> forwarded_memory_load_effect;
		u32 forwarded_memory_loads = 0;
		// Cold proof-stage attribution for VU0 vector load forwarding. These counts
		// identify whether ownership is lost at the memory chain, symbolic address,
		// or carried-state contract without logging generated execution.
		u32 memory_forward_candidates = 0;
		u32 memory_forward_reaching_stores = 0;
		u32 memory_forward_address_matches = 0;
		u32 memory_forward_state_matches = 0;
		std::vector<EdgeMove> edge_moves;
		std::vector<EdgeCopyStep> edge_copy_steps;
		u32 live_values = 0;
		// Physical S register which backs logical VfpS location zero. This is zero
		// only when this same plan proves that no allocated NEON value can alias it.
		u8 first_vfp_s = 16;
		u8 vfp_s_registers = 14;
		u8 neon_q_registers = 8;
		u32 core_peak_words = 0;
		u32 vfp_peak_s = 0;
		u32 neon_peak_q = 0;
		u32 spilled_values = 0;
		u32 spill_bytes = 0;
		u32 edge_scratch_offset = 0;
		u32 edge_scratch_bytes = 0;
		u32 coalesced_edge_values = 0;
		u32 coalesced_spill_edge_values = 0;
		// Cold attribution for residual internal copies whose two endpoints live in
		// the region-private spill frame.  Every exact-shape, non-interfering pair
		// should have been merged into one spill group before frame colouring.  The
		// unexplained category therefore identifies an allocator defect rather than
		// merely reporting generated stack traffic.
		u32 residual_spill_to_spill_edges = 0;
		u32 residual_spill_shape_mismatches = 0;
		u32 residual_spill_missing_groups = 0;
		u32 residual_spill_same_groups = 0;
		u32 residual_spill_group_interferences = 0;
		u32 residual_spill_unexplained = 0;
		u32 demanded_scalar_values = 0;
		u32 demanded_full_vector_values = 0;
		// Cold target-cost evidence after backend-only impossibility proofs have
		// removed preflighted/forwarded memory exits, internal non-observing edges,
		// and the entry-proven VU0 observer exits. RegionExecution retains the full
		// semantic exit set; these counters describe only exits emitted by this plan.
		u32 executable_exit_sites = 0;
		u32 executable_exit_state_bindings = 0;
		u32 executable_exit_state_words = 0;
		// Four packed u16 lanes: entry, guarded/observer, memory, and control.
		// Compile-only attribution; no generated instruction consumes these values.
		u64 executable_exit_sites_by_kind = 0;
		u64 executable_exit_state_words_by_kind = 0;
		// Four packed u16 lanes for executable control exits: external side arm,
		// aggregate-header/event edge, other internal edge, reserved.  This is
		// validation-only attribution used to decide whether formation or the
		// loop-event state seam owns the remaining cost.
		u64 executable_control_exit_sites_by_target = 0;
		u64 executable_control_exit_state_words_by_target = 0;
		// Validation-only block attribution for the same executable exit subset.
		// These vectors are indexed by RegionIR block and are never consumed by
		// allocation, emission, profitability, publication, or generated execution.
		std::vector<u32> executable_exit_sites_by_block;
		std::vector<u32> executable_exit_state_words_by_block;
		// Allocation-time attribution for exact VU0 values whose IR def-use roles
		// prefer scalar-addressable Q storage, and those which could not receive it.
		// These counters are cold compiler evidence and add no generated work.
		u32 scalar_preferred_neon_values = 0;
		u32 scalar_preferred_neon_misses = 0;
	};

	struct BuildResult
	{
		Plan plan{};
		BuildFailure failure = BuildFailure::None;
		u32 block = RegionIR::INVALID_BLOCK;
		RegionIR::ValueId value = RegionIR::INVALID_VALUE;

		explicit operator bool() const { return failure == BuildFailure::None; }
	};

	// The execution plan must be the successful result of
	// RegionExecution::Build(program). That call owns the one expensive semantic
	// verification pass; allocation checks the immutable plan/program dimensions
	// and never re-runs the verifier on the 496 MHz product compiler thread.
	BuildResult Build(const RegionIR::Program& program,
		const RegionExecution::Plan& execution,
		const Options& options = {});

	// Deterministically lower a set of per-edge SSA parallel copies. The caller
	// supplies the already allocated spill-frame size; a cycle reserves at most
	// one reusable, naturally aligned scratch slot in that same private frame.
	// This is public so the copy contract can be exercised independently of
	// instruction selection and later consumed unchanged by the A32 backend.
	EdgeCopyScheduleResult BuildEdgeCopySchedule(
		const std::vector<EdgeMove>& moves, u32 spill_bytes,
		u32 max_spill_bytes);
} // namespace VitaEE::RegionAllocation
