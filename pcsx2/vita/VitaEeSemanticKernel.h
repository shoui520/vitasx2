// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "pcsx2/vita/VitaEeRegionExecutionPlan.h"
#include "pcsx2/vita/VitaEeRegionMemory.h"

#include <array>
#include <string>
#include <vector>

namespace VitaEE::SemanticKernel
{
	// Semantic kernels are complete repeated operations derived from verified
	// Region IR. They are deliberately not source-pattern identifiers: no title,
	// hash, symbol, or source-word sequence is retained. Exact entry/completion
	// control locations may exist only as verifier-owned execution outputs and
	// must never participate in admission.
	enum class Kind : u8
	{
		None,
		PatternFill,
		ForwardCopy,
		// One affine stream of raw VU0 vectors transformed by four invariant
		// matrix columns. Admission is reconstructed from verified Region IR FMAC
		// dataflow; it never keys on a source word sequence, PC, symbol, or title.
		Vu0AffineTransform,
		// One deterministic serial iteration DAG with scalar COP1 arithmetic,
		// explicit loop-carried state, bounded or affine read streams, and affine
		// outputs. This is descriptor-only until a guarded first-class A32 owner
		// meets the inclusive Cortex-A9 gate.
		Cop1Stream,
		// A bounded affine scalar load stream which stops on the first value equal
		// to one loop-invariant key.  The surrounding prefix/found/exhausted suffix
		// remains part of the same first-class Region IR owner; only the repeated
		// search core may be replaced.  This is descriptor-only until its separate
		// reference, native owner, and target-cost gates pass.
		BoundedEqualSearch,
		Count,
	};

	enum class Failure : u8
	{
		None,
		InvalidProgram,
		InvalidExecutionPlan,
		InvalidMemoryPlan,
		MissingNaturalLoop,
		IrreducibleLoop,
		MultipleLatches,
		NonDeterministicIteration,
		ObserverInsideIteration,
		IncompleteMemoryCoverage,
		UnsupportedMemoryEffect,
		NonContiguousStream,
		VariantPattern,
		UnsupportedStateRecurrence,
		InexactEventPhase,
		InvalidMemoryExitContract,
		UnsupportedCopyShape,
		UnsupportedCopyDataflow,
		UnsupportedVu0AffineShape,
		UnsupportedVu0AffineDataflow,
		UnsupportedVu0AffineState,
		UnsupportedCop1StreamShape,
		UnsupportedCop1StreamDataflow,
		UnsupportedCop1StreamState,
		IncompleteCop1StreamMemory,
		UnsupportedBoundedSearchShape,
		UnsupportedBoundedSearchDataflow,
		UnsupportedBoundedSearchState,
		Count,
	};

	struct PatternFragment
	{
		RegionIR::ValueId operation = RegionIR::INVALID_VALUE;
		RegionIR::ValueId value = RegionIR::INVALID_VALUE;
		RegionIR::MemoryAccessKind kind = RegionIR::MemoryAccessKind::Store8;
		u32 source_pc = 0;
		s32 offset = 0;
		u8 width = 0;

		bool operator==(const PatternFragment&) const = default;
	};

	// One direct-RAM destination tile. One guest iteration writes every byte in
	// [minimum_offset, minimum_offset + stride) exactly once. The entry guard of
	// a future native lowering must prove the complete runtime range writable and
	// source-unowned before it performs any store.
	struct PatternStream
	{
		u8 entry_induction_gpr = 0;
		s32 entry_induction_offset = 0;
		u8 induction_gpr = 0;
		u32 stride = 0;
		u32 alignment = 1;
		s32 minimum_offset = 0;
		std::vector<PatternFragment> fragments;

		bool operator==(const PatternStream&) const = default;
	};

	struct CopyFragment
	{
		RegionIR::ValueId load_operation = RegionIR::INVALID_VALUE;
		RegionIR::ValueId store_operation = RegionIR::INVALID_VALUE;
		RegionIR::MemoryAccessKind load_kind = RegionIR::MemoryAccessKind::LoadU8;
		RegionIR::MemoryAccessKind store_kind = RegionIR::MemoryAccessKind::Store8;
		u32 load_pc = 0;
		u32 store_pc = 0;
		s32 source_offset = 0;
		s32 destination_offset = 0;
		u8 width = 0;

		bool operator==(const CopyFragment&) const = default;
	};

	// One ordered forward-copy tile. Scalar execution is valid for every alias
	// relationship because fragments retain guest load/store order. A widened or
	// batched lowering must additionally prove ForwardCopyBatchAliasSafe() for the
	// complete runtime ranges before committing its first write.
	struct CopyStream
	{
		u8 source_entry_induction_gpr = 0;
		s32 source_entry_induction_offset = 0;
		u8 source_induction_gpr = 0;
		u8 destination_entry_induction_gpr = 0;
		s32 destination_entry_induction_offset = 0;
		u8 destination_induction_gpr = 0;
		u32 stride = 0;
		u32 source_alignment = 1;
		u32 destination_alignment = 1;
		s32 source_minimum_offset = 0;
		s32 destination_minimum_offset = 0;
		std::vector<CopyFragment> fragments;

		bool operator==(const CopyStream&) const = default;
	};

	// Architectural load destinations are observable after a copy loop. The
	// final iteration's exact MemoryLoad result (including narrow sign/zero
	// extension and preserved upper qword lanes) must be published once.
	struct FinalLoadState
	{
		u8 gpr = 0;
		RegionIR::ValueId load_operation = RegionIR::INVALID_VALUE;
		RegionIR::ValueId load_value = RegionIR::INVALID_VALUE;
		RegionIR::MemoryAccessKind kind = RegionIR::MemoryAccessKind::LoadS8;

		bool operator==(const FinalLoadState&) const = default;
	};

	struct Low32Recurrence
	{
		u8 gpr = 0;
		s32 delta = 0;
		RegionExecution::Low32Extension extension =
			RegionExecution::Low32Extension::None;
		// True when the decoded recurrence is ADDI rather than ADDIU. A native
		// kernel must prove every represented update stays within signed I32 before
		// its first architectural effect, otherwise tier zero owns the loop.
		bool signed_overflow_guard = false;

		bool operator==(const Low32Recurrence&) const = default;
	};

	struct Vu0AffineStream
	{
		u8 source_entry_induction_gpr = 0;
		s32 source_entry_induction_offset = 0;
		u8 source_induction_gpr = 0;
		s32 source_offset = 0;
		u8 destination_entry_induction_gpr = 0;
		s32 destination_entry_induction_offset = 0;
		u8 destination_induction_gpr = 0;
		s32 destination_offset = 0;
		u32 stride = 0;
		u32 source_alignment = 1;
		u32 destination_alignment = 1;
		u32 load_pc = 0;
		u32 store_pc = 0;
		RegionIR::ValueId load_operation = RegionIR::INVALID_VALUE;
		RegionIR::ValueId load_value = RegionIR::INVALID_VALUE;
		RegionIR::ValueId store_operation = RegionIR::INVALID_VALUE;
		std::array<RegionIR::ValueId, 4> matrix_values = {
			RegionIR::INVALID_VALUE, RegionIR::INVALID_VALUE,
			RegionIR::INVALID_VALUE, RegionIR::INVALID_VALUE};
		std::array<RegionIR::ValueId, 4> normalized_matrix_values = {
			RegionIR::INVALID_VALUE, RegionIR::INVALID_VALUE,
			RegionIR::INVALID_VALUE, RegionIR::INVALID_VALUE};
		std::array<u8, 4> matrix_vf{};
		u8 input_vf = 0;
		u8 output_vf = 0;
		u8 idle_vi = 0;
		u8 status_vi = 0;
		u8 mac_vi = 0;
		RegionIR::ValueId final_input = RegionIR::INVALID_VALUE;
		RegionIR::ValueId final_output = RegionIR::INVALID_VALUE;
		RegionIR::ValueId final_acc = RegionIR::INVALID_VALUE;
		RegionIR::ValueId final_mac = RegionIR::INVALID_VALUE;
		RegionIR::ValueId final_status = RegionIR::INVALID_VALUE;
		RegionIR::ValueId final_vi_status = RegionIR::INVALID_VALUE;

		bool operator==(const Vu0AffineStream&) const = default;
	};

	struct StateOutput
	{
		u16 state_slot = 0;
		RegionIR::ValueId value = RegionIR::INVALID_VALUE;

		bool operator==(const StateOutput&) const = default;
	};

	enum class BoundedSearchOutputKind : u8
	{
		// Header low word plus delta for every completed continue iteration.
		AffineRecurrence,
		// The exact sign/zero-extended value read by the matching iteration.
		LoadedValue,
		// The false latch predicate published by the exhausted edge.
		FalsePredicate,
		// One conditional/delay-slot low-word add relative to header state.
		HeaderLow32Add,
	};

	// A post-search state publication mechanically reconstructed from the same
	// verified SSA value named by StateOutput.  This is a backend contract, not a
	// source-pattern hint: a native owner may be selected only when every changed
	// match/exhaustion state has exactly one recipe.
	struct BoundedSearchOutput
	{
		StateOutput publication{};
		BoundedSearchOutputKind kind =
			BoundedSearchOutputKind::AffineRecurrence;
		u8 source_gpr = 0;
		s32 delta = 0;
		RegionExecution::Low32Extension extension =
			RegionExecution::Low32Extension::None;

		bool operator==(const BoundedSearchOutput&) const = default;
	};

	// This descriptor deliberately retains verifier-owned IR value identities,
	// not guest PCs or source words. A backend must compile the complete serial
	// DAG and keep loop-carried state resident. It may software-pipeline only
	// dependency-independent subexpressions after proving every represented range
	// before its first write. normal_ou_guard_required means scalar VFP execution
	// is exact only for normal finite inputs/results; an exceptional batch must
	// side-exit before committing that batch.
	struct Cop1Stream
	{
		std::vector<RegionIR::ValueId> memory_operations;
		std::vector<u16> entry_state_slots;
		std::vector<StateOutput> final_state;
		u32 affine_ranges = 0;
		u32 bounded_read_ranges = 0;
		u32 cop1_arithmetic_nodes = 0;
		u32 cop1_conversion_nodes = 0;
		u32 load_operations = 0;
		u32 store_operations = 0;
		bool normal_ou_guard_required = false;
		bool requires_disjoint_read_write_ranges = false;

		bool operator==(const Cop1Stream&) const = default;
	};

	struct BoundedEqualSearch
	{
		RegionIR::ValueId load_operation = RegionIR::INVALID_VALUE;
		RegionIR::ValueId load_value = RegionIR::INVALID_VALUE;
		RegionIR::ValueId key_value = RegionIR::INVALID_VALUE;
		RegionIR::ValueId comparison = RegionIR::INVALID_VALUE;
		RegionIR::MemoryAccessKind load_kind =
			RegionIR::MemoryAccessKind::LoadS32;
		u32 comparison_block = RegionIR::INVALID_BLOCK;
		u32 match_target_block = RegionIR::INVALID_BLOCK;
		u32 continue_target_block = RegionIR::INVALID_BLOCK;
		u8 entry_induction_gpr = 0;
		s32 entry_induction_offset = 0;
		u8 induction_gpr = 0;
		u32 stride = 0;
		u32 alignment = 1;
		s32 load_offset = 0;
		u32 match_scaled_cycles = 0;
		bool condition_true_is_match = false;
		bool match_edge_checks_event = false;
		// Exact SSA publications required at the three semantic seams. These are
		// verifier-owned values, not an authorization for a backend to skip their
		// computation. A native lowering must implement every listed value or fail.
		std::vector<StateOutput> backedge_state;
		std::vector<StateOutput> match_state;
		std::vector<StateOutput> exhausted_state;
		std::vector<BoundedSearchOutput> native_match_outputs;
		std::vector<BoundedSearchOutput> native_exhausted_outputs;

		bool operator==(const BoundedEqualSearch&) const = default;
	};

	struct Plan
	{
		Kind kind = Kind::None;
		u32 header_block = RegionIR::INVALID_BLOCK;
		u32 latch_block = RegionIR::INVALID_BLOCK;
		u32 completion_block = RegionIR::INVALID_BLOCK;
		u32 completion_pc = 0;
		// Ordered exactly as the deterministic repeated path executes them.
		std::vector<u32> iteration_blocks;
		std::vector<PatternStream> pattern_streams;
		std::vector<CopyStream> copy_streams;
		std::vector<Vu0AffineStream> vu0_affine_streams;
		std::vector<Cop1Stream> cop1_streams;
		std::vector<BoundedEqualSearch> bounded_equal_searches;
		std::vector<FinalLoadState> final_load_state;
		std::vector<Low32Recurrence> recurrences;
		RegionMemoryPlan::LoopControl control{};
		u32 repeated_scaled_cycles = 0;
		u32 completion_scaled_cycles = 0;
		u32 source_instructions_per_iteration = 0;
		u32 memory_operations_per_iteration = 0;
		u32 bytes_per_iteration = 0;
		u32 read_bytes_per_iteration = 0;
		u32 write_bytes_per_iteration = 0;
		// A native implementation may batch only when the existing region entry
		// preflight proves every stream and this flag remains true.
		bool complete_preflight = false;
		// Multiple write streams require pairwise disjointness unless the selected
		// class supplies a stronger ordered alias contract.
		bool requires_disjoint_write_streams = false;
		// The descriptor is always exact in scalar guest order. This flag prevents a
		// future wide copy lowering from being selected without its range alias guard.
		bool requires_forward_copy_batch_alias_guard = false;
		// There is exactly one PCSX2 scheduler observation after each complete
		// repeated iteration and no hidden intermediate observation.
		bool exact_event_phase = false;

		bool operator==(const Plan&) const = default;
	};

	struct BuildResult
	{
		Plan plan{};
		Failure failure = Failure::None;
		u32 block = RegionIR::INVALID_BLOCK;
		RegionIR::ValueId value = RegionIR::INVALID_VALUE;
		std::string detail;

		explicit operator bool() const { return failure == Failure::None; }
	};

	enum class ReferenceFailure : u8
	{
		None,
		InvalidDescriptor,
		InvalidTripCount,
		MissingInvariantValue,
		RangeOverflow,
		AliasGuard,
		MemoryProbe,
		MemoryRead,
		MemoryWrite,
		Vu0Busy,
		ArithmeticOverflow,
		CycleOverflow,
	};

	struct ReferenceValueInterface
	{
		void* context = nullptr;
		bool (*read)(void* context, RegionIR::ValueId value, u128* bits) = nullptr;
	};

	struct ReferenceOptions
	{
		const RegionIR::RegionMemoryInterface* memory = nullptr;
		ReferenceValueInterface values{};
		u32 maximum_iterations = 1u << 20;
	};

	struct ReferenceResult
	{
		ReferenceFailure failure = ReferenceFailure::None;
		u32 iterations = 0;
		u32 memory_operations = 0;
		u32 memory_address = 0;
		RegionIR::ValueId value = RegionIR::INVALID_VALUE;
		// Multi-exit semantic cores report the exact verifier-owned seam selected
		// by their data.  Other kernel kinds leave exit_block invalid and matched
		// false because they have one ordinary completion edge.
		u32 exit_block = RegionIR::INVALID_BLOCK;
		bool matched = false;

		explicit operator bool() const { return failure == ReferenceFailure::None; }
	};

	// Cold, side-effect-free recognition. This function verifies the source-
	// attested IR and rebuilds its mechanical exit and memory plans. It has no
	// code buffer, runtime, lookup, continuation, or publication capability.
	BuildResult Build(const RegionIR::Program& program);
	// Product-disabled descriptor interpreter. It starts at the descriptor's loop
	// header with canonical architectural state plus explicit invariant SSA values,
	// preflights every represented memory effect, executes exact guest order, and
	// publishes the completion-edge state. It neither calls nor publishes code.
	ReferenceResult Interpret(const RegionIR::Program& program, const Plan& plan,
		const RegionIR::CanonicalState& input, RegionIR::CanonicalState* output,
		const ReferenceOptions& options);

	// Runtime-independent half of the widened forward-copy alias proof. The caller
	// must already have proven both byte ranges non-wrapping and directly mapped.
	// Copying toward a lower/equal address is forward-safe; copying toward a higher
	// overlapping address must retain exact scalar guest ordering.
	bool ForwardCopyBatchAliasSafe(u32 source_begin, u32 destination_begin,
		u32 byte_count);
} // namespace VitaEE::SemanticKernel
