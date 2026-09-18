// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"
#include "pcsx2/vita/VitaEeBlockCompiler.h"
#include "pcsx2/vita/VitaEeRegionExecutionPlan.h"
#include "pcsx2/vita/VitaEeRegionIR.h"

#include <array>
#include <cstddef>
#include <vector>

namespace VitaA32
{
	class CodeBuffer;
}

namespace VitaEE::RegionA32
{
	constexpr size_t STATE_CLASS_COUNT =
		static_cast<size_t>(RegionExecution::StateClass::Cycle) + 1;
	// Only the continuously executed slab is constrained by the Cortex-A9's
	// instruction-cache budget. Exact observer exits are out of line and may use
	// the remainder; committed code-cache storage is trimmed to actual size.
	static constexpr size_t DEFAULT_CODE_CAPACITY = 8 * 1024;

	// This is a private generated-code ABI. It is intentionally independent of
	// cpuRegisters so every product-opt-in region proves the fields it observes or
	// publishes before the runtime adapts them to canonical PCSX2 state.
	struct ExecutionResult
	{
		u32 completed = 0;
		u32 reason = static_cast<u32>(RegionIR::ExitReason::RegionBoundary);
		u32 cycle_commit_deferred = 0;
		u32 pending_raw_cycles = 0;
		u32 memory_address = 0;
	};

	struct ExecutionContext
	{
		RegionIR::CanonicalState* state = nullptr;
		// Raw PCSX2 VTLBVirtual table and its additive host-memory base. The
		// generated load path duplicates VTLBVirtual::{isHandler,assumePtr}.
		const u32* vmap = nullptr;
		const u8* host_memory_base = nullptr;
		u8* main_ram = nullptr;
		// Inclusive last host address at which a 32-bit main-RAM read can begin.
		const u8* main_ram_last_word = nullptr;
		// Authoritative tier-zero EE source ownership. A generated store checks
		// these tables before committing the write. The page byte makes ordinary
		// data-only stores cheap; the 64-byte bitmap conservatively exits before
		// any store which could overlap live generated source. Tier zero then owns
		// the write and PCSX2 recClear-equivalent exact invalidation.
		const u8* ram_source_page_live_flags = nullptr;
		const u8* ram_source_chunk_live_bits = nullptr;
		// Current architecturally exposed main-RAM size. Counted entry ranges
		// prove their own VTLB pages against main_ram and use this only as a
		// bounds guard.
		u32 main_ram_limit = 0;
		// Nonzero only while PCSX2's default low-main-RAM identity proof holds.
		u32 identity_main_ram_limit = 0;
		u32 next_event_cycle_low = UINT32_MAX;
		u32 next_event_cycle_high = UINT32_MAX;
		ExecutionResult result{};
	};

	enum class CompileFailure : u8
	{
		None,
		InvalidProgram,
		UnattestedSource,
		UnsupportedInstruction,
		UnsupportedMemory,
		RegisterPressure,
		CodeCapacity,
		Emission,
		Patch,
		UnsupportedStateContract,
		UnprovenProfitability,
	};

	// Compile-only explanation for whether internal CFG scheduler checks were
	// folded into one verified loop horizon. This never changes admission; it
	// distinguishes a missing timing proof from memory or allocation cost.
	enum class AggregateCyclePlanStatus : u8
	{
		NotAttempted,
		Enabled,
		NotPersistent,
		TimingInvalid,
		HeaderNotEntry,
		InvalidCycleTransfer,
		CycleDebtOverflow,
		InvalidTarget,
		InconsistentDebt,
		UnreachableBlock,
	};

	struct CompileOptions
	{
		static constexpr u32 DEFAULT_MINIMUM_A9_COUNTED_MEMORY_WORK = 512;
		// Inclusive first-class Cortex-A9 measurements put both retained Phase 5
		// byte-stream classes below 8x at 2 KiB and above 10x at 4 KiB. This is a
		// target publication floor, not semantic support: short invocations retain
		// the exact tier-zero owner through the generated entry selector.
		static constexpr u32 DEFAULT_MINIMUM_A9_SEMANTIC_KERNEL_BYTES = 4096;
		// The validation-only VU0 tier removes substantially more tier-zero work per
		// vector iteration than a scalar memory loop. The native first-class tdiff
		// fixture crosses 2x at four iterations, with eight transferred words per
		// iteration. Keep this separate from the product scalar floor until two real
		// VU0 workloads and the SDK sweep retire the diagnostic admission gate.
		static constexpr u32 DEFAULT_MINIMUM_A9_VU0_COUNTED_MEMORY_WORK = 32;
		// The first native Cortex-A9 boundary-elision loop proved >=2x after
		// 4,608 decoded guest instructions in one uninterrupted scheduler epoch.
		// Express that evidence as work so larger generic regions do not inherit a
		// sample-shaped 384-latch rule. Final emission still rejects un-preflighted
		// memory, while the normal 4 KiB hot-code cap protects L1I and the allocator's
		// exact spill accounting remains visible to the hardware gate.
		static constexpr u32 DEFAULT_MINIMUM_A9_OBSERVED_REGION_WORK = 4608;
		// The generic three-block preflighted/narrowed fixture crosses the complete
		// persistent-dispatch 2x gate at 109 iterations on the physical Cortex-A9.
		// Express that measured crossover as decoded repeated work (18 * 109), then
		// require the cold allocation prescreen and final emitted-code certificate
		// below. This is not a replacement for the conservative 8192-work class in
		// RegionMemoryPlan: loops which do not prove the narrower generated-code cost
		// class retain that older floor.
		static constexpr u32 DEFAULT_MINIMUM_A9_NARROWED_PREFLIGHTED_WORK = 1962;

		enum class PersistentStateBase : u8
		{
			CpuRegisters,
			Vu0,
		};

		struct PersistentStateWord
		{
			u16 canonical_offset = 0;
			u16 runtime_offset = 0;
			PersistentStateBase runtime_base = PersistentStateBase::CpuRegisters;
		};

		struct PersistentDispatch
		{
			struct CompatibleTarget
			{
				u32 pc = 0;
				GprLinkSignature signature{};
			};

			const PersistentStateWord* state_words = nullptr;
			size_t state_word_count = 0;
			// VU0 is not embedded in cpuRegistersPack. Phase 4 regions use this
			// distinct architectural base so VF/VI/ACC/flags can remain in one
			// verified unit without a CanonicalState callback adapter.
			const void* vu0_state = nullptr;
			const u32* vmap = nullptr;
			// Persistent tier-zero code keeps the additive vTLB host base in r8.
			// Memory-free regions may borrow that otherwise dead register for their
			// complete hot lifetime, then restore this exact ABI value before any
			// generated tail leaves the region.
			const void* host_memory_base = nullptr;
			u8* main_ram = nullptr;
			const u8* main_ram_last_word = nullptr;
			const u8* ram_source_page_live_flags = nullptr;
			const u8* ram_source_chunk_live_bits = nullptr;
			u32 main_ram_limit = 0;
			u32 identity_main_ram_limit = 0;
			u32* execution_counter = nullptr;
			// Profiler-build-only aggregate of exact counted-loop iterations. The
			// generated entry already derives this value for its range/event proof;
			// accumulating it here distinguishes real repeated work from one long
			// probe which is followed by many short invocations. Product builds leave
			// this null and emit no instructions for it.
			u32* counted_iteration_counter = nullptr;
			// Diagnostic-only count of exact pre-entry selector rejections. Product
			// execution leaves this null: the selector must not turn a dynamic short
			// invocation into either a hot write or a permanent admission decision.
			u32* profitability_fallback_counter = nullptr;
			// Diagnostic-only count of exact low-32 representation guard
			// rejections. A rejected entry has executed no guest instruction and
			// must enter the tier-zero block once at the same PC.
			u32* entry_state_fallback_counter = nullptr;
			// Optional exact private state accepted from a signature-equal tier-zero
			// direct edge.  The backend emits a distinct entry and never uses it for
			// indirect lookup or a mismatched source.
			const GprLinkSignature* compatible_entry_signature = nullptr;
			const CompatibleTarget* compatible_targets = nullptr;
			size_t compatible_target_count = 0;
		};

		// The Vita Cortex-A9 has a 32 KiB L1 instruction cache. Bound one direct
		// region to one eighth of it, and independently bound the one-shot entry
		// certificate and repeated CFG body. This prevents a large affine/event/SMC
		// proof from silently consuming the loop-body budget while retaining a hard
		// working-set limit. Rare exact-state exits live after this slab.
		u32 max_hot_code_bytes = 4096;
		u32 max_entry_hot_code_bytes = 1536;
		u32 max_block_hot_code_bytes = 3072;
		u32 max_code_bytes = DEFAULT_CODE_CAPACITY;
		// A discard-only validation compile may need the complete emitted image to
		// identify why a hot execution island exceeded the product's 4 KiB working-
		// set limit. This suppresses only the three hot-slab policy checks above; the
		// CodeBuffer and max_code_bytes bounds remain authoritative. Runtime may set
		// this only on its target-cost shadow path, which cannot commit, publish,
		// construct continuations, or execute the resulting image.
		bool validation_only_measure_oversize_hot_image = false;
		// Canonical participants are not synonymous with resident host values.
		// Read-only low-word inputs can be loaded from the private state frame while
		// written/full-width values consume the eight callee-saved A32 words.
		u8 max_mapped_gprs = 16;
		u8 max_resident_gpr_words = 8;
		// Validation can isolate the exact per-access VTLB/SMC lowering from the
		// counted-loop entry proof. Product compilation leaves this enabled.
		bool enable_counted_memory_preflight = true;
		// Exact backend emission is independent of publication policy.  When this is
		// true the backend may emit a selector derived from an optional Cortex-A9 cost
		// classifier, but an absent/failed classifier never makes an otherwise exact
		// Region IR program unsupported. Runtime owns the later publish decision.
		bool emit_profitability_guard = false;
		// Ask exact emission to report whether the allocated image satisfies the
		// independently measured narrowed/preflighted class.  This is classification
		// only: the backend records the result and still returns a valid image.
		bool classify_narrowed_preflighted_cost = false;
		// Phase 5 lowering. Exact semantic recognition may replace the represented
		// counted loop with one observation-bounded native operation, but it still
		// enters through the persistent generated lookup and uses the verifier-derived
		// exit map. Ordinary product runtime leaves this false until inclusive
		// Cortex-A9 and two-workload Vita gates pass.
		bool enable_semantic_kernel_lowering = false;
		// Physical-A9 fixtures may exercise an exact emitted subclass which missed
		// the >=8x retention gate. Product-controlled validation must never set this:
		// it enables measurement, not publication evidence.
		bool allow_unretained_semantic_kernel_lowering = false;
		// Target-local cost-model floor used to derive the entry trip-count guard.
		// It is a minimum iteration count for scalar streams and an aggregate
		// transferred-word floor for 128-bit streams, matching the independently
		// measured Cortex-A9 break-even curves. Native validation may lower it to
		// measure the complete first-class ABI without weakening product admission.
		u32 minimum_a9_counted_memory_work =
			DEFAULT_MINIMUM_A9_COUNTED_MEMORY_WORK;
		// A verified event-bounded direct-call loop has no architectural trip-count
		// GPR to guard at entry. Product admission therefore observes this many real
		// latch traversals in generated A32 before spending a compile token.
		u32 minimum_a9_observed_region_work =
			DEFAULT_MINIMUM_A9_OBSERVED_REGION_WORK;
		// Target-cost selector for the complete affine VU0 FMAC stream. Product
		// leaves this at the independently validated four-iteration 2x floor;
		// physical-A9 validation may lower it solely to measure the generated ABI
		// below that guard before changing admission policy.
		u32 minimum_a9_observed_vu0_fmac_iterations = 4;
		// Acyclic VU0 leaves use generated entry-frequency evidence rather than an
		// architectural loop-count guard. Product retains the independently measured
		// 512-call Cortex-A9 floor; validation may lower it without changing semantics.
		u32 minimum_a9_observed_vu0_fmac_leaf_invocations = 512;
		// Runtime publication may set this only after an identical first emission
		// proved that the region has neither allocated spills nor any work-scratch
		// access. This removes the otherwise unconditional stack adjustment from
		// the real dispatcher ABI without guessing from guest opcodes.
		bool omit_proven_unused_work_scratch_frame = false;
		// Non-null emits the region itself in the live persistent-dispatch ABI.
		// r4 is &cpuRegs, r7/r8 are the resident VTLB bases, and every verified
		// exit leaves one direct branch relocation for the runtime to bind to an
		// exact suffix, generated redispatch, or the existing event stub. No
		// CanonicalState adapter or AAPCS call exists in this mode.
		const PersistentDispatch* persistent_dispatch = nullptr;
	};

	struct PersistentExitPatch
	{
		size_t branch_offset = static_cast<size_t>(-1);
		// A signature-compatible region boundary has two local leaves. The
		// selector reaches the compatible leaf without materializing write-back
		// GPR words; republishing a mismatched/retired target redirects it to the
		// canonical leaf, which stores those words before leaving this code owner.
		// This makes target repatching safe without charging the compatible path
		// for canonical stores.
		size_t selector_branch_offset = static_cast<size_t>(-1);
		size_t canonical_leaf_offset = static_cast<size_t>(-1);
		size_t canonical_branch_offset = static_cast<size_t>(-1);
		size_t compatible_leaf_offset = static_cast<size_t>(-1);
		RegionIR::ExitReason reason = RegionIR::ExitReason::RegionBoundary;
		u32 resume_pc = 0;
		u32 pending_raw_cycles = 0;
		bool cycle_commit_deferred = false;
		// The verifier may supply a runtime transfer PC for JR/JALR. The cold leaf
		// has already materialized that exact SSA value into cpuRegs.pc; it must
		// tail-enter the generated directory redispatch instead of being patched to
		// a compile-time target.
		bool dynamic_resume_pc = false;
		// A profitability guard before the region prologue has executed no guest
		// instruction, changed no canonical state, and entered no private frame. Its
		// patch tail-enters the immutable tier-zero owner directly; an optional
		// compatible signature preserves the incoming tier-zero register contract.
		bool entry_passthrough = false;
		// Large exact regions can have many distinct cold state maps.  Emitting the
		// complete scatter-store sequence beside every exit makes cold code grow as
		// exits multiplied by architectural state.  A compact exit instead names one
		// immutable descriptor, snapshots the region-owned physical bank in a shared
		// cold leaf, and lets the descriptor materializer return this patch's local
		// branch tail.  Both offsets are compile-time metadata; neither participates
		// in hot dispatch or executable ownership.
		size_t compact_descriptor_pointer_patch = static_cast<size_t>(-1);
		u16 compact_descriptor_index = UINT16_MAX;
		GprLinkSignature compatible_signature{};
	};

	enum class PersistentColdWordSource : u8
	{
		Frame,
		CpuRegisters,
		Vu0,
		Immediate,
		SignExtendFrame,
		SignExtendCpuRegisters,
		SignExtendVu0,
	};

	struct PersistentColdWord
	{
		u16 destination_offset = 0;
		u16 source_offset = 0;
		u32 immediate = 0;
		CompileOptions::PersistentStateBase destination_base =
			CompileOptions::PersistentStateBase::CpuRegisters;
		PersistentColdWordSource source = PersistentColdWordSource::Frame;
	};

	struct PersistentColdExitDescriptor
	{
		// first_word is retained across CompileResult moves.  Runtime resolves the
		// pointer only after the final immutable vector has been built and before the
		// generated owner is published.
		u32 first_word = 0;
		u16 word_count = 0;
		const PersistentColdWord* words = nullptr;
		const void* branch_tail = nullptr;
	};

	struct CompileResult
	{
		struct TargetCostBlock
		{
			u32 pc = 0;
			u32 source_instructions = 0;
			u32 hot_bytes = 0;
			u32 host_loads = 0;
			u32 host_stores = 0;
			u32 spilled_core_values = 0;
			u32 spilled_vfp_values = 0;
			u32 spilled_neon_values = 0;
			u32 spill_loads = 0;
			u32 spill_stores = 0;
			u32 edge_moves = 0;
			u32 edge_state_words = 0;
			u32 exit_sites = 0;
			u32 exit_state_words = 0;
			// bit0 contains a JAL, bit1 is a direct callee entry, bit2 is a
			// verified return-PC block, bit3 contains the callee's JR r31.
			u8 direct_call_roles = 0;
		};

		CompileFailure failure = CompileFailure::None;
		u32 failure_pc = 0;
		u16 failure_state_slot = UINT16_MAX;
		u8 failure_state_word_mask = 0;
		u16 failure_ir_opcode = UINT16_MAX;
		RegionIR::ValueId failure_value = RegionIR::INVALID_VALUE;
		// Nonzero identifies the failed primitive within a supported lowering.
		// It makes corpus failures actionable without logging from generated code.
		u8 failure_emission_step = 0;
		RegionIR::ValueId failure_operand_value = RegionIR::INVALID_VALUE;
		u8 failure_operand_location_kind = 0;
		u8 failure_operand_word_mask = 0;
		u8 failure_operand_folded = 0;
		u32 code_bytes = 0;
		u32 hot_code_bytes = 0;
		u32 cold_code_bytes = 0;
		u32 cold_exit_leaves = 0;
		u32 cold_exit_epilogues = 0;
		u32 pre_entry_profitability_leaves = 0;
		u32 pre_entry_state_leaves = 0;
		u32 entry_low32_guards = 0;
		u32 host_instructions = 0;
		u32 hot_host_instructions = 0;
		u32 hot_host_loads = 0;
		u32 hot_host_stores = 0;
		u32 hot_state_loads = 0;
		u32 hot_state_stores = 0;
		u32 hot_stack_loads = 0;
		u32 hot_stack_stores = 0;
		u32 block_stack_loads = 0;
		u32 block_stack_stores = 0;
		u32 spill_word_loads = 0;
		u32 spill_word_stores = 0;
		u32 spill_vfp_loads = 0;
		u32 spill_vfp_stores = 0;
		u32 spill_vector_loads = 0;
		u32 spill_vector_stores = 0;
		u32 block_spill_word_loads = 0;
		u32 block_spill_word_stores = 0;
		u32 block_spill_vfp_loads = 0;
		u32 block_spill_vfp_stores = 0;
		u32 block_spill_vector_loads = 0;
		u32 block_spill_vector_stores = 0;
		// Populated only by validation_only_measure_oversize_hot_image. This is
		// compile-and-discard attribution and cannot affect executable admission.
		std::vector<TargetCostBlock> target_cost_blocks;
		u32 vu0_raw_spill_word_loads = 0;
		u32 vu0_raw_spill_word_stores = 0;
		u32 vu0_pipeline_spill_word_loads = 0;
		u32 vu0_pipeline_spill_word_stores = 0;
		u32 vu0_transform_spill_word_loads = 0;
		u32 vu0_transform_spill_word_stores = 0;
		u32 frame_bytes = 0;
		u32 work_scratch_bytes = 0;
		u32 memory_loads = 0;
		u32 memory_stores = 0;
		u32 memory_preflight_blocks = 0;
		u32 memory_preflight_accesses = 0;
		u32 memory_preflight_store_accesses = 0;
		u32 memory_preflight_ranges = 0;
		// Cold RegionMemoryPlan diagnostic; never read by generated code.
		u8 memory_plan_reducibility_rejection = 0;
		// Preflighted scalar RAM accesses which consume the verified guest address
		// directly as an A32 register offset from the persistent VTLB host base.
		// This is compile-time attribution, not a generated hot-path counter.
		u32 indexed_scalar_memory_accesses = 0;
		// Fixed-address memory families whose complete range is proven at entry and
		// whose host base remains in a private region register for the entire loop.
		u32 hoisted_memory_bases = 0;
		u32 hoisted_memory_accesses = 0;
		// Scalar low-RAM loads whose fixed addresses are entry-proven and whose
		// allocated core word is exclusive for the complete CFG.  A store-free
		// region reads these once after its event/range guards and retains the value
		// until the next genuine observer instead of reloading it on every latch.
		u32 hoisted_memory_loads = 0;
		// Exact memory-SSA store-to-load forwarding performed across internal
		// compiler-owned edges. This is cold compile attribution only.
		u32 forwarded_memory_loads = 0;
		u32 memory_forward_candidates = 0;
		u32 memory_forward_reaching_stores = 0;
		u32 memory_forward_address_matches = 0;
		u32 memory_forward_state_matches = 0;
		// Internal edges whose exact cycle advances are accumulated at the next
		// backedge/side exit under one verified acyclic-iteration horizon.
		u32 aggregated_cycle_edges = 0;
		AggregateCyclePlanStatus aggregate_cycle_plan_status =
			AggregateCyclePlanStatus::NotAttempted;
		// Conditional side exits placed out of line while their ordinary internal
		// successor falls through. This removes one unconditional A32 branch from
		// each repeated traversal without changing the verified edge state.
		u32 conditional_layout_fallthroughs = 0;
		// Compile-time count of exact I64 equality predicates narrowed to their
		// low word by a proven sign/zero-extension fact. This adds no generated
		// telemetry and keeps the A9 validation gate from merely inferring that
		// the intended lowering happened from timing noise.
		u32 narrowed_equal64_comparisons = 0;
		// Compile-time count of ordered I64 predicates decided entirely by exact
		// low-32 extension facts.  This is distinct from an approximation: the
		// selected A32 condition implements the complete signed/unsigned 64-bit
		// ordering for the proven operand representations.
		u32 narrowed_ordered64_comparisons = 0;
		// A comparison whose architectural 0/1 result is immediately tested by a
		// branch can materialize that result with non-flag-setting MOV/STR operations
		// and let the branch consume the original compare flags.  This is derived from
		// the IR def-use chain, never from a guest PC or opcode sequence identity.
		u32 reused_materialized_branch_flags = 0;
		// Narrow integer loads whose high word is absent from physical allocation do
		// not emit a throwaway sign/zero extension merely because it is available by
		// rematerialization at a later observer.
		u32 elided_load_high_words = 0;
		// Compile-time attribution for the region-owned scalar COP1 lowering.
		// These never add generated/runtime telemetry; they make code-size
		// regressions and numeric-fact wins visible to the A9 corpus gate.
		u32 cop1_normalize_emitted = 0;
		u32 cop1_normalize_elided = 0;
		u32 cop1_normalize_hoisted = 0;
		u32 cop1_ou_pairs_fused = 0;
		// Fused clamp/O-U pairs whose raw and architectural result share one VFP
		// lane. The ordinary finite path therefore needs no core-to-VFP commit.
		u32 cop1_destructive_ou_pairs = 0;
		// Verifier-owned exceptional predicates which branch to compact generated
		// numeric veneers and then rejoin the region. These are cold semantic paths,
		// not pre-instruction exits or embedded tier-zero suffixes.
		u32 cop1_lazy_exception_guards = 0;
		u32 cop1_lazy_exception_leaves = 0;
		u32 cop1_hot_bytes = 0;
		u32 cop1_normalize_hot_bytes = 0;
		u32 cop1_ou_hot_bytes = 0;
		// Compile-time attribution for exact VU0 macro lowering. This is code-size
		// evidence only and adds no instruction to generated execution.
		u32 vu0_normalize_hot_bytes = 0;
		u32 vu0_broadcast_hot_bytes = 0;
		u32 vu0_arithmetic_hot_bytes = 0;
		u32 vu0_clamp_hot_bytes = 0;
		u32 vu0_mac_flag_hot_bytes = 0;
		u32 vu0_status_flag_hot_bytes = 0;
		u32 vu0_merge_hot_bytes = 0;
		u32 vu0_direct_vfp_lanes = 0;
		u32 vu0_spilled_quad_arithmetic = 0;
		u32 vu0_fused_madd_lanes = 0;
		// Folded VU broadcast attribution. The lane mask records decoded xyzw
		// components and the scalar-register mask records their allocated physical
		// sources; both are compile-time evidence and add no generated work.
		u8 vu0_folded_broadcast_lane_mask = 0;
		u32 vu0_folded_broadcast_source_s_mask = 0;
		u32 memory_hot_bytes = 0;
		// Cold compile attribution for a Phase 5 semantic body. A nonzero kind means
		// the ordinary repeated IR blocks were replaced in this image; it does not
		// imply that Runtime published or executed the image.
		u8 semantic_kernel_kind = 0;
		u32 semantic_kernel_hot_bytes = 0;
		u32 semantic_kernel_bytes_per_iteration = 0;
		u32 semantic_kernel_minimum_profitable_bytes = 0;
		// Set only after the complete native body emitted. Runtime consumes this
		// cold result in PublishProfitability; recognition alone can never publish.
		bool semantic_kernel_target_cost_valid = false;
		// Mutually exclusive emission phases. These locate code-size pressure
		// without instrumenting generated execution or conflating one-time entry
		// certificates with the repeated loop body.
		u32 prologue_hot_bytes = 0;
		u32 entry_event_hot_bytes = 0;
		u32 entry_iteration_hot_bytes = 0;
		u32 entry_memory_hot_bytes = 0;
		u32 entry_cop1_hot_bytes = 0;
		u32 entry_branch_hot_bytes = 0;
		u32 block_hot_bytes = 0;
		u32 block_host_loads = 0;
		u32 block_host_stores = 0;
		u32 aggregate_event_iteration_cycles = 0;
		// Structural Cortex-A9 admission certificate for counted memory loops.
		// The generated entry compares its exact proven trip count against this
		// bound before any guest instruction executes and enters tier zero below
		// the break-even floor.
		u32 minimum_profitable_iterations = 0;
		// Cold post-emission evidence consumed only by Runtime's independent
		// PublishProfitability decision. None of these fields participates in exact
		// instruction support, allocation, verification, or memory/SMC semantics.
		u32 profitability_diagnostic_flags = 0;
		u8 profitability_certificate_kind = 0;
		bool profitability_certificate_valid = false;
		bool profitability_memory_coverage_valid = true;
		bool profitability_narrowed_cost_valid = true;
		// Mechanically derived from the verified Region IR. The current source
		// emitter consumes this plan as its correctness gate; the scalable backend
		// will allocate these exact values and use them for every cold exit.
		u32 exit_sites = 0;
		u32 exit_state_bindings = 0;
		u32 exit_state_words = 0;
		u64 exit_sites_by_kind = 0;
		u64 exit_state_words_by_kind = 0;
		u64 control_exit_sites_by_target = 0;
		u64 control_exit_state_words_by_target = 0;
		u32 allocation_live_values = 0;
		u32 allocation_core_peak_words = 0;
		u32 allocation_vfp_peak_s = 0;
		u32 allocation_neon_peak_q = 0;
		u32 allocation_spilled_values = 0;
		u32 allocation_spill_bytes = 0;
		u32 allocation_spilled_core_values = 0;
		u32 allocation_spilled_vfp_values = 0;
		u32 allocation_spilled_neon_values = 0;
		u32 allocation_edge_moves = 0;
		u32 allocation_coalesced_edge_values = 0;
		u32 allocation_edge_core_moves = 0;
		u32 allocation_edge_vfp_moves = 0;
		u32 allocation_edge_neon_moves = 0;
		u32 allocation_edge_spill_moves = 0;
		u32 allocation_edge_spill_to_spill_moves = 0;
		u32 allocation_edge_spill_to_register_moves = 0;
		u32 allocation_edge_register_to_spill_moves = 0;
		u32 allocation_edge_cross_kind_moves = 0;
		u32 allocation_edge_gpr_words = 0;
		u32 allocation_edge_fpr_words = 0;
		u32 allocation_edge_vu0_vector_words = 0;
		u32 allocation_edge_other_state_words = 0;
		std::array<u32, STATE_CLASS_COUNT> allocation_edge_state_class_words{};
		u32 allocation_edge_parameter_sources = 0;
		u32 allocation_edge_computed_sources = 0;
		// Cold compiler attribution for internal ownership seams. Direct-call and
		// return edges are identified only through verified DirectCallContract
		// metadata; backedges and ordinary CFG edges remain structural categories.
		u32 allocation_edge_call_moves = 0;
		u32 allocation_edge_return_moves = 0;
		u32 allocation_edge_backedge_moves = 0;
		u32 allocation_edge_other_moves = 0;
		u32 allocation_coalesced_spill_edge_values = 0;
		u32 allocation_residual_spill_shape_mismatches = 0;
		u32 allocation_residual_spill_missing_groups = 0;
		u32 allocation_residual_spill_same_groups = 0;
		u32 allocation_residual_spill_group_interferences = 0;
		u32 allocation_residual_spill_unexplained = 0;
		u32 allocation_demanded_scalar_values = 0;
		u32 allocation_demanded_full_vector_values = 0;
		// Allocator-backed regions expose their complete private-call-frame ABI as
		// architectural 32-bit word masks. Entry includes every word read by the
		// generated prologue or an entry guard, plus every possible output word so
		// an exit which leaves that word untouched can be copied back safely. Output
		// is the union of the verifier-derived dirty words for all cold exits. The
		// product runtime rejects state classes it cannot yet adapt; it never guesses
		// from decoded opcodes or keeps a hand-written register flush list.
		RegionExecution::StateWordMasks entry_state_words{};
		RegionExecution::StateWordMasks output_state_words{};
		u32 entry_state_word_count = 0;
		u32 output_state_word_count = 0;
		bool uses_state_word_contract = false;
		// Nonzero only when the entry owns a structurally proven counted memory
		// range. The runtime uses this immutable compile contract to classify a
		// failed entry guard without adding logging or callbacks to generated code.
		u8 entry_range_induction_gpr = 0;
		u8 entry_range_bound_gpr = 0;
		u8 entry_range_stride_bytes = 0;
		u8 mapped_gpr_count = 0;
		std::array<u8, 16> mapped_gprs{};
		// Only these canonical participants can change. The runtime initializes all
		// mapped inputs but copies back this mask, avoiding needless architectural
		// traffic for read-only address and bound values.
		u32 written_gpr_mask = 0;
		u8 resident_gpr_words = 0;
		size_t compatible_entry_offset = static_cast<size_t>(-1);
		GprLinkSignature compatible_entry_signature{};
		std::vector<PersistentExitPatch> persistent_exit_patches;
		std::vector<PersistentColdExitDescriptor> persistent_cold_exit_descriptors;
		std::vector<PersistentColdWord> persistent_cold_exit_words;
		u32 persistent_cold_snapshot_bytes = 0;

		explicit operator bool() const { return failure == CompileFailure::None; }
	};

	using GeneratedRegion = u32 (*)(ExecutionContext* context);

	CompileResult Compile(const RegionIR::Program& program,
		VitaA32::CodeBuffer& code,
		const CompileOptions& options = {});
	// Allocator-consuming Region IR backend. The default-off product runtime uses
	// only this entry point; Compile() remains a validation oracle.
	CompileResult CompileAllocated(const RegionIR::Program& program,
		VitaA32::CodeBuffer& code,
		const CompileOptions& options = {});
	const char* CompileFailureName(CompileFailure failure);
} // namespace VitaEE::RegionA32
