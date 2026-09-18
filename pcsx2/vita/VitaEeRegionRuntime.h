// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "pcsx2/vita/A32Emitter.h"
#include "pcsx2/vita/VitaEeExecutor.h"
#include "pcsx2/vita/VitaEeRegionA32.h"
#include "pcsx2/vita/VitaEeRegionMemory.h"
#include "pcsx2/vita/VitaEeSemanticKernel.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <vector>

namespace VitaEE::RegionRuntime
{
	enum class BuildFailureStage : u8
	{
		None,
		SourceWindow,
		SourceRead,
		DiscoveryLift,
		DiscoveryBackedge,
		ProfitabilityTriage,
		SpecializedWait,
		SourceContractCapacity,
		ContractLift,
		ContractBackedge,
		UncertifiedCop1,
		ExactContinuation,
		CanonicalDuplicate,
		CodeReservation,
		A32Compile,
		PersistentEntry,
		CodeCommit,
		EntryCapacity,
		HeapAllocation,
		// Appended so existing profiler/build-failure numeric records remain
		// stable. VU0 Region IR is validation-only until Phase 4's exact-state
		// and Cortex-A9 performance gates pass.
		UncertifiedVu0,
		// Appended diagnostic distinctions for immutable dependency-span assembly.
		// SourceWindow remains the external/lifter category; these identify the
		// exact cold builder rule without changing guest behavior.
		SourceWindowDisjoint,
		SourceWindowOverlap,
		SourceWindowCapacity,
		// A validation-only complete graph reached exact A32 emission, but had no
		// independently measured Cortex-A9 publication certificate. The emitted
		// owner is discarded and the immutable candidate is remembered terminally.
		TargetCostUnproven,
		// ExactSupport completed successfully.  The independently evaluated emitted
		// image did not meet the Cortex-A9 publication contract, so tier zero remains
		// authoritative.  This is not an instruction/backend support failure.
		PublishProfitability,
	};

	struct BuildFailureRecord
	{
		u32 entry_pc = 0;
		u32 failure_pc = 0;
		u32 opcode = 0;
		BuildFailureStage stage = BuildFailureStage::None;
		RegionA32::CompileFailure backend = RegionA32::CompileFailure::None;
		u8 backend_emission_step = 0;
		u16 backend_ir_opcode = UINT16_MAX;
		RegionIR::ValueId backend_value = RegionIR::INVALID_VALUE;
		u32 failure_detail = 0;
	};

	// Cold admission-history result for a semantically discovered caller unit
	// which must contain an actual repeated path.  This is deliberately separate
	// from generic sampled-forward failures: startup traffic must not evict the
	// evidence explaining why a VU0 caller/leaf unit did not become executable.
	enum class RepeatedCandidateOutcome : u8
	{
		None,
		SourceUnavailable,
		GraphIncomplete,
		GraphInvalid,
		ActiveOwnerOverlap,
		ProbeOwnerOverlap,
		MissingTopologyDeferred,
		NoRepeatedPath,
		AlreadyKnown,
		AdmissionSaturated,
		QueuedProbe,
		QueuedPending,
		ProfitabilityProven,
		ProfitabilityRejected,
		ProfitabilityUnknown,
		ProbeEmitted,
		BuildRejected,
		// A complete, semantically valid source graph was retained because every
		// generated probe owner was transiently armed. This is a retryable resource
		// state, not a negative admission result.
		AdmissionDeferred,
		RegionPublished,
		// A retained admission-capacity owner acquired a probe slot without a new
		// sparse sample or source-graph discovery transaction.
		AdmissionRetried,
	};

	struct Statistics
	{
		static constexpr size_t ENTRY_SNAPSHOT_CAPACITY = 4;
		static constexpr size_t FAILURE_SNAPSHOT_CAPACITY = 8;
		static constexpr size_t CANDIDATE_SNAPSHOT_CAPACITY = 8;
		static constexpr size_t REPEATED_CANDIDATE_SNAPSHOT_CAPACITY = 16;
		struct EntrySnapshot
		{
			u32 pc = 0;
			u64 executions = 0;
			u64 counted_iterations = 0;
			u64 profitability_fallbacks = 0;
			u64 entry_state_fallbacks = 0;
			u32 block_count = 0;
			u32 source_words = 0;
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
			u32 vu0_raw_spill_word_loads = 0;
			u32 vu0_raw_spill_word_stores = 0;
			u32 vu0_pipeline_spill_word_loads = 0;
			u32 vu0_pipeline_spill_word_stores = 0;
			u32 vu0_transform_spill_word_loads = 0;
			u32 vu0_transform_spill_word_stores = 0;
			u32 frame_bytes = 0;
			u32 work_scratch_bytes = 0;
			u32 hot_code_bytes = 0;
			u32 cold_code_bytes = 0;
			u8 semantic_kernel_kind = 0;
			u32 semantic_kernel_hot_bytes = 0;
			u32 semantic_kernel_bytes_per_iteration = 0;
			u32 semantic_kernel_minimum_profitable_bytes = 0;
			bool semantic_kernel_target_cost_valid = false;
			u32 prologue_hot_bytes = 0;
			u32 entry_event_hot_bytes = 0;
			u32 entry_iteration_hot_bytes = 0;
			u32 entry_memory_hot_bytes = 0;
			u32 entry_cop1_hot_bytes = 0;
			u32 entry_branch_hot_bytes = 0;
			u32 block_hot_bytes = 0;
			u32 memory_hot_bytes = 0;
			u32 memory_loads = 0;
			u32 memory_stores = 0;
			u32 block_host_loads = 0;
			u32 block_host_stores = 0;
			u32 cop1_normalize_emitted = 0;
			u32 cop1_normalize_elided = 0;
			u32 cop1_normalize_hoisted = 0;
			u32 cop1_ou_pairs_fused = 0;
			u32 cop1_destructive_ou_pairs = 0;
			u32 cop1_lazy_exception_guards = 0;
			u32 cop1_lazy_exception_leaves = 0;
			u32 cop1_hot_bytes = 0;
			u32 cop1_normalize_hot_bytes = 0;
			u32 cop1_ou_hot_bytes = 0;
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
			u8 vu0_folded_broadcast_lane_mask = 0;
			u32 vu0_folded_broadcast_source_s_mask = 0;
			u32 reused_materialized_branch_flags = 0;
			u32 elided_load_high_words = 0;
			u32 entry_state_words = 0;
			u32 output_state_words = 0;
			u32 compact_exit_descriptors = 0;
			u32 compact_exit_words = 0;
			u32 compact_snapshot_bytes = 0;
			u32 core_peak_words = 0;
			u32 vfp_peak_s = 0;
			u32 neon_peak_q = 0;
			u32 spilled_values = 0;
			u32 spill_bytes = 0;
			u32 spilled_core_values = 0;
			u32 spilled_vfp_values = 0;
			u32 spilled_neon_values = 0;
			u32 edge_moves = 0;
			u32 coalesced_edge_values = 0;
			u32 edge_core_moves = 0;
			u32 edge_vfp_moves = 0;
			u32 edge_neon_moves = 0;
			u32 edge_spill_moves = 0;
			u32 edge_spill_to_spill_moves = 0;
			u32 edge_spill_to_register_moves = 0;
			u32 edge_register_to_spill_moves = 0;
			u32 edge_cross_kind_moves = 0;
			u32 edge_gpr_words = 0;
			u32 edge_fpr_words = 0;
			u32 edge_vu0_vector_words = 0;
			u32 edge_other_state_words = 0;
			std::array<u32, RegionA32::STATE_CLASS_COUNT>
				edge_state_class_words{};
			u32 edge_parameter_sources = 0;
			u32 edge_computed_sources = 0;
			u32 edge_call_moves = 0;
			u32 edge_return_moves = 0;
			u32 edge_backedge_moves = 0;
			u32 edge_other_moves = 0;
			u32 coalesced_spill_edge_values = 0;
			u32 residual_spill_shape_mismatches = 0;
			u32 residual_spill_missing_groups = 0;
			u32 residual_spill_same_groups = 0;
			u32 residual_spill_group_interferences = 0;
			u32 residual_spill_unexplained = 0;
			u32 preflight_accesses = 0;
			u32 preflight_store_accesses = 0;
			u8 memory_plan_reducibility_rejection = 0;
			u32 indexed_scalar_memory_accesses = 0;
			u32 hoisted_memory_bases = 0;
			u32 hoisted_memory_accesses = 0;
			u32 hoisted_memory_loads = 0;
			u32 forwarded_memory_loads = 0;
			u32 aggregated_cycle_edges = 0;
			u8 aggregate_cycle_plan_status = 0;
			u32 conditional_layout_fallthroughs = 0;
			u32 preflight_stride = 0;
			u32 minimum_profitable_iterations = 0;
			u32 pre_entry_profitability_leaves = 0;
			u32 pre_entry_state_leaves = 0;
			u32 entry_low32_guards = 0;
		};
		struct ProbeSnapshot
		{
			u32 entry_pc = 0;
			u32 backedge_pc = 0;
			u32 source_end_pc = 0;
			u32 observations = 0;
			u32 maximum_observations = 0;
			u32 samples = 0;
			u32 required_observations = 0;
			u8 internal_source_blocks = 0;
			bool guarded = false;
			bool event_scoped = false;
			bool armed = false;
			bool dormant = false;
			bool live = false;
		};
		struct DeferredSnapshot
		{
			u32 entry_pc = 0;
			u32 backedge_pc = 0;
			u32 source_end_pc = 0;
			u32 missing_contract_pc = 0;
			bool admission_capacity = false;
			bool classification_prepared = false;
		};
		struct RepeatedCandidateSnapshot
		{
			u32 entry_pc = 0;
			u32 source_end_pc = 0;
			u32 detail_pc = 0;
			u32 observations = 0;
			u64 last_serial = 0;
			u8 internal_source_blocks = 0;
			RepeatedCandidateOutcome outcome = RepeatedCandidateOutcome::None;
			BuildFailureStage failure_stage = BuildFailureStage::None;
			u32 failure_detail = 0;
		};
		struct TargetCostBlockSnapshot
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
			u8 direct_call_roles = 0;
		};
		static constexpr u32 TARGET_COST_BLOCK_SNAPSHOT_CAPACITY = 8;
		struct TargetCostSnapshot
		{
			u32 entry_pc = 0;
			u32 source_end_pc = 0;
			u32 semantic_diagnostics = 0;
			u32 blocks = 0;
			u32 source_words = 0;
			u32 direct_calls = 0;
			u32 host_instructions = 0;
			u32 hot_code_bytes = 0;
			u32 entry_state_words = 0;
			u32 output_state_words = 0;
			u32 exit_sites = 0;
			u32 exit_state_words = 0;
			u64 exit_sites_by_kind = 0;
			u64 exit_state_words_by_kind = 0;
			u64 control_exit_sites_by_target = 0;
			u64 control_exit_state_words_by_target = 0;
			u32 compact_exit_descriptors = 0;
			u32 compact_exit_words = 0;
			u32 compact_snapshot_bytes = 0;
			u32 core_peak_words = 0;
			u32 vfp_peak_s = 0;
			u32 neon_peak_q = 0;
			u32 spilled_values = 0;
			u32 spill_bytes = 0;
			u32 spilled_core_values = 0;
			u32 spilled_vfp_values = 0;
			u32 spilled_neon_values = 0;
			u32 edge_moves = 0;
			u32 edge_call_moves = 0;
			u32 edge_return_moves = 0;
			u32 edge_backedge_moves = 0;
			u32 edge_state_words = 0;
			u32 memory_loads = 0;
			u32 memory_stores = 0;
			u32 forwarded_memory_loads = 0;
			u32 memory_forward_candidates = 0;
			u32 memory_forward_reaching_stores = 0;
			u32 memory_forward_address_matches = 0;
			u32 memory_forward_state_matches = 0;
			u32 memory_preflight_ranges = 0;
			u32 memory_preflight_accesses = 0;
			u8 aggregate_cycle_plan_status = 0;
			u32 failure_pc = 0;
			u32 failure_detail = 0;
			RegionIR::ValueId failure_value = RegionIR::INVALID_VALUE;
			u16 failure_ir_opcode = UINT16_MAX;
			BuildFailureStage failure_stage = BuildFailureStage::None;
			RegionA32::CompileFailure backend_failure =
				RegionA32::CompileFailure::None;
			u8 failure_emission_step = 0;
			bool backend_emitted = false;
			bool valid = false;
			u32 block_snapshot_count = 0;
			std::array<TargetCostBlockSnapshot,
				TARGET_COST_BLOCK_SNAPSHOT_CAPACITY> block_snapshot{};
		};
		static constexpr u32 TARGET_COST_SNAPSHOT_CAPACITY = 4;
		u64 candidates = 0;
		u64 build_attempts = 0;
		u64 compiles = 0;
		u64 compile_failures = 0;
		u64 compile_wall_us = 0;
		u64 compile_budget_deferrals = 0;
		u64 compile_budget_refills = 0;
		u64 target_cost_analyses = 0;
		// ExactSupport completed allocation/emission. This is deliberately distinct
		// from `compiles`, which counts only committed executable owners.
		u64 exact_support_emissions = 0;
		u64 publish_profitability_rejections = 0;
		u64 profitability_prescreen_passes = 0;
		u64 profitability_prescreen_rejections = 0;
		u64 profitability_prescreen_unknowns = 0;
		u64 compiler_heap_failures = 0;
		u64 code_cache_failures = 0;
		u64 source_contract_deferrals = 0;
		u64 source_contract_resumes = 0;
		u64 source_contract_replacements = 0;
		u64 source_contract_capacity_rejections = 0;
		u64 source_graph_attempts = 0;
		u64 source_graph_formations = 0;
		u64 forward_event_samples = 0;
		u64 forward_sample_collisions = 0;
		u64 forward_sample_requests = 0;
		u64 forward_sample_candidates = 0;
		u64 forward_sample_rejections = 0;
		u64 probe_observations = 0;
		u64 maximum_event_scoped_probe_observations = 0;
		u64 probe_sample_misses = 0;
		u64 probe_sample_retries = 0;
		u64 probe_code_failures = 0;
		u64 hot_promotions = 0;
		u64 probe_evictions = 0;
		u64 admission_saturations = 0;
		u64 admission_capacity_deferrals = 0;
		u64 admission_capacity_retries = 0;
		u64 admission_capacity_rejections = 0;
		u64 probe_directory_repairs = 0;
		u64 evictions = 0;
		u64 generation_resets = 0;
		u64 executions = 0;
		u64 boundary_exits = 0;
		u64 event_exits = 0;
		u64 event_budget_fallbacks = 0;
		u64 profitability_fallbacks = 0;
		u64 entry_state_fallbacks = 0;
		u64 profitability_retirements = 0;
		u64 memory_exits = 0;
		u64 memory_alignment_exits = 0;
		u64 memory_handler_exits = 0;
		u64 memory_translation_exits = 0;
		u64 self_modifying_code_exits = 0;
		u64 continuation_exits = 0;
		u64 continuation_nonzero_debt_exits = 0;
		u64 continuation_event_exits = 0;
		u64 continuation_scheduler_elided_exits = 0;
		u64 continuation_failures = 0;
		// First failed counted-range translation guard. This bounded snapshot is
		// captured from canonical state after the zero-effect cold exit and makes
		// the exact failed proof visible without printing on the EE thread.
		u32 translation_snapshot_valid = 0;
		u32 translation_start_low = 0;
		u32 translation_start_high = 0;
		u32 translation_bound_low = 0;
		u32 translation_bound_high = 0;
		u32 translation_identity_limit = 0;
		u32 translation_stride_bytes = 0;
		u64 code_bytes = 0;
		// Execution-weighted product-ABI shape attribution, sampled only at the
		// existing cold reporting boundary. The generated hot path still performs
		// only its one bounded per-entry execution increment.
		u64 one_block_executions = 0;
		u64 multi_block_executions = 0;
		u64 preflight_executions = 0;
		u64 preflight_store_executions = 0;
		u64 spill_executions = 0;
		u64 weighted_host_instructions = 0;
		u64 weighted_hot_code_bytes = 0;
		u64 weighted_entry_state_words = 0;
		u64 weighted_output_state_words = 0;
		u64 semantic_kernel_executions = 0;
		u64 semantic_kernel_iterations = 0;
		u64 semantic_kernel_bytes = 0;
		// Index is SemanticKernel::Kind's stable telemetry value. Index zero is
		// intentionally unused because it names ordinary region execution.
		std::array<u64, static_cast<size_t>(SemanticKernel::Kind::Count)>
			semantic_kernel_executions_by_kind{};
		u32 active_regions = 0;
		u32 active_probes = 0;
		u32 armed_probes = 0;
		u32 deferred_candidates = 0;
		// Cold ownership snapshot. These expose whether an event unwind left real
		// work queued; they are never consulted by generated code or admission.
		u32 pending_candidate = 0;
		u32 publication_dirty = 0;
		u32 maintenance_requested = 0;
		u32 compile_budget_tokens = 0;
		u64 compile_budget_wait_cycles = 0;
		// Current cache identity plus lifetime executions survive the 120-VSync
		// window counter reset. This distinguishes a region which ran only during
		// startup from one which is active in the measured steady-state window.
		std::array<EntrySnapshot, ENTRY_SNAPSHOT_CAPACITY> entry_snapshot{};
		u32 probe_snapshot_count = 0;
		std::array<ProbeSnapshot, CANDIDATE_SNAPSHOT_CAPACITY> probe_snapshot{};
		u32 deferred_snapshot_count = 0;
		std::array<DeferredSnapshot, CANDIDATE_SNAPSHOT_CAPACITY>
			deferred_snapshot{};
		u32 repeated_candidate_snapshot_count = 0;
		std::array<RepeatedCandidateSnapshot,
			REPEATED_CANDIDATE_SNAPSHOT_CAPACITY>
			repeated_candidate_snapshot{};
		// The compatibility snapshot retains the largest inspected unit. The
		// bounded array preserves every cold ExactSupport attempt in this reporting
		// window so a smaller executable island cannot be hidden by one oversized
		// wrapper. None of this data participates in admission or publication.
		TargetCostSnapshot target_cost_snapshot{};
		u32 target_cost_snapshot_count = 0;
		std::array<TargetCostSnapshot, TARGET_COST_SNAPSHOT_CAPACITY>
			target_cost_snapshots{};
		u32 failure_snapshot_count = 0;
		std::array<BuildFailureRecord, FAILURE_SNAPSHOT_CAPACITY>
			failure_snapshot{};
	};

	struct Execution
	{
		enum class Failure : u8
		{
			None,
			GeneratedReturn,
			StateMaterialization,
			MissingContinuation,
			ContinuationExecution,
			UnhandledExit,
		};

		bool executed = false;
		// One fallible entry guard may ask tier zero to execute the same source
		// fragment exactly once. The persistent boundary bridge uses this bit to
		// enter that block instead of unwinding and immediately retrying the region.
		bool bypassed = false;
		bool valid = false;
		RegionIR::ExitReason reason = RegionIR::ExitReason::RegionBoundary;
		RegionIR::ExitReason generated_reason =
			RegionIR::ExitReason::RegionBoundary;
		u32 memory_address = 0;
		u32 pending_raw_cycles = 0;
		bool cycle_commit_deferred = false;
		bool scheduler_test_elided = false;
		// A persistent-dispatch build executes an exact cold suffix as the next
		// generated fragment instead of nesting another callable A32 frame inside
		// Runtime::ExecuteAtPc().
		const void* persistent_resume_entry = nullptr;
		BlockExecutionResult persistent_resume_result{};
		// A callable exact suffix may invalidate tier-zero source ownership.  The
		// persistent dispatcher must unwind before Synchronize() retires or
		// republishes any region executable state.
		bool requires_outer_synchronization = false;
		Failure failure = Failure::None;
		u32 entry_pc = 0;
		u32 resume_pc = 0;
		u32 generated_token = 0;
		u32 generated_completed = 0;
		u32 continuation_count = 0;
		u32 continuation_pc_matches = 0;
		u32 continuation_debt_matches = 0;
	};

	using PersistentEventOwner = bool (*)(void* userdata);
	using PersistentUnwindPredicate = bool (*)(void* userdata);

	struct PersistentChainExecution
	{
		enum class Outcome : u8
		{
			NoRegion,
			ResumeTierZero,
			ResumeGenerated,
			UnwindOuter,
			Fatal,
		};

		Outcome outcome = Outcome::NoRegion;
		Execution last{};
		const void* persistent_resume_entry = nullptr;
		BlockExecutionResult persistent_resume_result{};
		u32 region_count = 0;
		bool bypassed = false;
	};

	// A verifier-derived state contract is sparse. Resolve its architectural and
	// private-frame offsets once when the region is built instead of decoding all
	// state slots on every invocation. One transfer is exactly one aligned
	// 32-bit architectural word; memcpy in the adapter preserves aliasing rules.
	struct StateWordTransfer
	{
		u16 canonical_offset = 0;
		u16 runtime_offset = 0;
		RegionA32::CompileOptions::PersistentStateBase runtime_base =
			RegionA32::CompileOptions::PersistentStateBase::CpuRegisters;
	};

	struct StateTransferPlan
	{
		// Integer/MMI Phase 3 can expose 32 GPRs, HI, LO, SA and the 64-bit cycle.
		// Complete word capacity for every mapped architectural class. EE/COP1
		// words use persistent r4; VU0's 32 VF registers, ACC, flags and control
		// complete VI file and provider-visible flag mirrors use its separate
		// singleton base.
		static constexpr size_t MAX_WORDS =
			32 * 4 + 4 + 4 + 1 + 2 + 32 + 4 +
			32 * 4 + 4 + 4 + 32 + 12;
		// Contracts are sparse and are built only at the cold publication boundary.
		// Keeping MAX_WORDS inline in every one of the bounded region-cache entries
		// made adding VU0 inflate the complete runtime (and the native adversarial
		// fixture stack) even when a region touched one VF. Exact-sized cold vectors
		// preserve the hard bound without charging inactive product entries.
		std::vector<StateWordTransfer> entry;
		std::vector<StateWordTransfer> output;
		u16 entry_count = 0;
		u16 output_count = 0;
	};

	// Build the exact canonical-to-live cpuRegistersPack word directory consumed
	// by first-class generated regions. Keeping one owner for this mapping lets
	// A9 corpus diagnostics compile the product ABI instead of approximating it
	// with the callable CanonicalState adapter.
	bool BuildPersistentStateWordMap(
		std::array<RegionA32::CompileOptions::PersistentStateWord,
			StateTransferPlan::MAX_WORDS + 1>* words,
		size_t* word_count);

	// Bounded owner for the first product-connected Phase 3 natural-loop cache.
	// Admission is derived only from an observed backward tier-zero edge and the
	// executor's immutable source contracts. The provider remains responsible for
	// publishing this object's desired entry barriers as part of its one complete
	// barrier-owner transaction.
	class Runtime
	{
	public:
		// Active code ownership is independent of the four-entry telemetry sample.
		// Thirty-two entries keep the complete metadata comfortably small while a
		// 64-slot rebuilt open-addressed index makes every product entry lookup O(1).
		// Region code occupies fixed authoritative EE-arena slots. Replacement keeps
		// one staging slot, then reuses the retired victim only after directory and
		// incoming/generated-edge publication has made the old bytes unreachable.
		static constexpr size_t MAX_REGIONS = 32;
		static constexpr size_t MAX_CONTINUATIONS_PER_REGION = 64;
		static constexpr size_t ENTRY_LOOKUP_CAPACITY = 64;
		static_assert((ENTRY_LOOKUP_CAPACITY & (ENTRY_LOOKUP_CAPACITY - 1)) == 0);
		// The product directory owns probes exactly like regions. Keep one bounded
		// probe owner for every possible region slot so a burst of newly compiled
		// tier-zero loops is measured at its actual latch instead of being serialized
		// through a four-entry FIFO. Each probe reserves at most 128 bytes in the
		// authoritative EE executable arena, so the complete set is 4 KiB.
		static constexpr size_t MAX_ARMED_PROBES = 32;
		static constexpr size_t MAX_GENERATED_OWNERS =
			MAX_REGIONS + MAX_ARMED_PROBES;
		static constexpr size_t MAX_CANDIDATE_HISTORY = 128;
		static constexpr size_t MAX_DEFERRED_CANDIDATES = 32;
		// Compilation is opportunistic CPU0 work. A small initial burst lets a
		// replay or newly entered workload promote its genuinely hot loops, then one
		// token returns per deterministic EE-cycle interval. Unlike the former
		// lifetime 128-attempt ceiling, this cannot let startup permanently disable
		// gameplay discovery. At full 294.912 MHz guest progress the refill is at
		// most about 17.6 attempts/s; on the measured Vita PES replay it is about
		// 1.1/s. Failed shapes are remembered separately, so tokens are not spent on
		// the same source contract twice within one source generation.
		static constexpr u32 BUILD_TOKEN_CAPACITY = 8;
		static constexpr u64 BUILD_TOKEN_REFILL_EE_CYCLES = 16ull * 1024 * 1024;
		// A dynamic trip-count miss is not a compile-budget event. Rechecking it at
		// the token-refill cadence repeatedly republished executable ownership for a
		// loop which remained cheaper in tier zero. Begin near one guest second at
		// full EE clock and exponentially back off to a bounded multi-second sample.
		// This retains discovery when later invocations become substantially larger
		// without making small library-style calls a permanent CPU0 admission tax.
		static constexpr u32 PROBE_RETRY_INITIAL_EE_CYCLES = 256u * 1024 * 1024;
		static constexpr u32 PROBE_RETRY_MAXIMUM_EE_CYCLES = 1u << 30;
		// A source block being prepared proves only that a loop-shaped branch
		// exists. Require repeated execution before spending a product compile slot.
		// Product probes are first-class generated entries: they increment a bounded
		// counter and tail-enter the exact tier-zero owner without a hot C++ callback.
		static constexpr u32 MINIMUM_BACKEDGE_OBSERVATIONS = 8;
		// A counted loop's first call is often cold setup work and is not a sound
		// predictor of later batch sizes. Keep its generated guard live for a bounded
		// number of external invocations before sleeping the probe. This preserves
		// discovery of changing trip counts without a C++ callback, a permanent entry
		// tax, or any relaxation of the measured 512-work profitability floor.
		static constexpr u32 COUNTED_PROBE_INVOCATION_BUDGET = 32;
		// Event-bounded natural loops are compared by decoded repeated work rather
		// than the exact latch count of the first PES loop.  Generated probes still
		// reset at each scheduler epoch, so unrelated short invocations cannot pool
		// enough cold heat to displace tier zero.
		static constexpr u32 MINIMUM_EVENT_SCOPED_REGION_WORK =
			RegionA32::CompileOptions::DEFAULT_MINIMUM_A9_OBSERVED_REGION_WORK;
		// Probe liveness is audited sparsely at the scheduler owner. Promotion itself
		// sets a generated flag and is noticed at the next event rather than waiting
		// for this cadence.
		static constexpr u32 PROBE_SAMPLE_EVENT_INTERVAL = 32;
		// Phase 4 forward/reducible discovery samples one architectural PC only at
		// this already-required scheduler cadence. A direct-mapped cold table avoids
		// source lookup or allocation in the live dispatcher frame; repeated samples
		// merely request one outer-boundary CFG transaction.
		static constexpr size_t FORWARD_SAMPLE_CAPACITY = 64;
		static constexpr u32 MINIMUM_FORWARD_EVENT_SAMPLES = 4;
		static_assert((FORWARD_SAMPLE_CAPACITY &
			(FORWARD_SAMPLE_CAPACITY - 1)) == 0);
		// The older entry-barrier probe remains validation-only so adversarial tests
		// can compare its exact taken-edge observations with the sampled product
		// policy. It is never selected in a Vita product build.
		static constexpr u32 PROBE_IDLE_OUTER_BOUNDARIES = 256;

		explicit Runtime(BlockExecutor* executor);
		~Runtime() = default;

		Runtime(const Runtime&) = delete;
		Runtime& operator=(const Runtime&) = delete;

		// Drops generated regions and candidate history. Returns true when the
		// provider's desired barrier set changed.
		bool ResetCode();
		void ResetStatistics();
		// Starts one cold correlated-profile window without disturbing lifetime
		// execution/failure counters used by the existing start/end deltas.
		void BeginStatisticsWindow();
		// Retires regions after any tier-zero source-owner or relevant configuration
		// generation change. Call only at an outer provider boundary.
		bool Synchronize();

		// Observe one cold/private-dispatch boundary. A true result asks the caller
		// to unwind to the outer provider: either a compiled entry is ready, or one
		// source-backed candidate is pending compilation there.
		bool ObserveTierZeroBoundary(
			const BlockExecutionResult& result, u32 target_pc);
		// Observe a newly created source owner before tier zero enters it. This is
		// the only reliable cold seam for a backedge which is immediately linked to
		// an already-live header and would otherwise stay inside generated code.
		bool ObservePreparedTierZeroBlock(const BlockExecutionResult& result);
		// Compiles a pending source-backed candidate at an outer provider boundary.
		// Returns true if the desired barrier set changed.
		bool PreparePendingAtOuterBoundary();
		// The persistent A32 dispatcher can service scheduler events without
		// returning to recExecute(). Audit generated probe promotion and liveness at
		// that already-owned boundary; request one outer unwind only when executable
		// ownership must change.
		bool ObservePersistentEventBoundary(u32 sampled_pc)
		{
#if defined(VITASX2_QEMU_VALIDATION)
			// Validation retains the exact entry-barrier oracle and exercises both
			// policies through one explicit slow seam.
			return ObservePersistentEventBoundarySlow(sampled_pc);
#else
			// Product code pays one shared generated-maintenance flag test on an
			// ordinary scheduler event. A promoted candidate which has exhausted the
			// deterministic compile token bucket remains pending without repeatedly
			// unwinding the dispatcher; its first eligible scheduler event owns the
			// cold compile transaction.
			if (m_publication_dirty ||
				(m_pending.valid && BuildBudgetAvailable()))
				return true;
			if (m_generated_maintenance_requested != 0)
				return ProcessGeneratedMaintenance();
			AdvanceEventScopedProbeEpoch();
			if (m_forward_sample_request_pc != UINT32_MAX)
				return true;
			if (m_forward_sample_event_countdown > 1)
				m_forward_sample_event_countdown--;
			else
			{
				m_forward_sample_event_countdown = PROBE_SAMPLE_EVENT_INTERVAL;
				if (SampleForwardRegionPc(sampled_pc))
					return true;
			}
			return false;
#endif
		}
		// Read-only execution-policy query.  Compilation and barrier publication
		// are outer-provider transactions and must never run while the generated
		// persistent-dispatch frame is live.
		bool RequiresOuterBoundary() const;
		Execution ExecuteAtPc(u32 pc, bool persistent_dispatch_frame = false);
		// Execute every consecutively live region before the persistent dispatcher
		// re-enters tier zero.  Event and lifecycle/checkpoint owners are supplied
		// by the provider, keeping this state machine directly testable on A9.
		PersistentChainExecution ExecutePersistentChain(
			PersistentEventOwner event_owner, void* event_userdata,
			PersistentUnwindPredicate should_unwind, void* unwind_userdata);

		size_t CopyBarrierPcs(u32* output, size_t capacity) const;
			size_t CopyPersistentGeneratedEntries(
				PersistentGeneratedEntry* output, size_t capacity) const;
			// Rebind every live region-boundary branch to the complete generated-owner
			// set currently published by BlockExecutor. Compilation initially binds an
			// exit to whatever owner exists at that outer boundary; later region
			// publication, eviction, or retirement can change that owner. The provider
			// must call this only after SetPersistentGeneratedEntries() and while no
			// persistent dispatcher frame is live.
			bool RepatchPersistentGeneratedExits();
			// The provider calls this only after both generated-directory publication
			// and generated-exit repatching succeed. Until then retired fixed slots keep
			// their bytes intact because an old incoming branch may still reach them.
			void AcknowledgePublishedCodeRetirements();
			Statistics GetStatistics() const;

	#if defined(VITASX2_QEMU_VALIDATION)
		// Existing runtime ownership fixtures exercise compilation rather than hot
		// selection. The dedicated admission fixture below retains the product
		// threshold and proves that cold loops do not compile.
		void SetMinimumBackedgeObservationsForValidation(u32 observations)
		{
			m_minimum_backedge_observations = observations;
		}
		void SetMaximumActiveRegionsForValidation(u32 regions)
		{
			m_max_active_regions = std::max<u32>(1,
				std::min<u32>(regions, MAX_REGIONS));
		}
		void SetCallableExecutionForValidation(bool enabled)
		{
			m_callable_execution_for_validation = enabled;
		}
		void SetRequireProvenProfitabilityForValidation(bool required)
		{
			m_require_proven_profitability = required;
		}
		// Phase 5 native lowering is deliberately disconnected from product
		// admission until its exact persistent-dispatch and inclusive Cortex-A9
		// gates pass.  The switch reaches only CompileOptions in validation builds;
		// it cannot alter an ordinary Vita product binary.
		void SetSemanticKernelLoweringForValidation(bool enabled)
		{
			m_semantic_kernel_lowering_enabled = enabled;
			m_allow_unretained_semantic_kernel_lowering_for_validation = enabled;
		}
		// Mirrors the controlled Vita switch: enable only subclasses which already
		// passed the physical-A9 retention gate. This lets lifecycle fixtures prove
		// that an exact-but-slow descriptor cannot leak into product publication.
		void SetRetainedSemanticKernelLoweringForValidation(bool enabled)
		{
			m_semantic_kernel_lowering_enabled = enabled;
			m_allow_unretained_semantic_kernel_lowering_for_validation = false;
		}
		void SetMinimumCountedMemoryWorkForValidation(u32 work)
		{
			m_minimum_counted_memory_work_for_validation = std::max<u32>(1, work);
		}
		void SetMinimumVu0CountedMemoryWorkForValidation(u32 work)
		{
			m_minimum_vu0_counted_memory_work = std::max<u32>(1, work);
		}
		void SetMinimumObservedVu0FmacIterationsForValidation(u32 iterations)
		{
			m_minimum_observed_vu0_fmac_iterations =
				std::max<u32>(1, iterations);
		}
		void SetMinimumObservedVu0FmacLeafInvocationsForValidation(u32 invocations)
		{
			m_minimum_observed_vu0_fmac_leaf_invocations =
				std::max<u32>(1, invocations);
		}
		void SetFinalProfitabilityHeapFailureForValidation(bool enabled)
		{
			m_final_profitability_heap_failure_for_validation = enabled;
		}
		void SetAllowUncertifiedCop1ForValidation(bool allowed)
		{
			m_allow_uncertified_cop1_for_validation = allowed;
		}
		void SetAllowUncertifiedVu0ForValidation(bool allowed)
		{
			m_allow_uncertified_vu0_for_validation = allowed;
		}
		void SetEmitUnprovenTargetCostForValidation(bool enabled)
		{
			m_emit_unproven_target_cost_for_validation = enabled;
		}
		// Diagnostic-only: distinguish the architectural 4 KiB hot-region budget
		// from an unsupported lowering. Product compilation always retains the
		// Cortex-A9 working-set caps in RegionA32::CompileOptions.
		void SetMaximumHotCodeBytesForValidation(u32 bytes)
		{
			m_maximum_hot_code_bytes_for_validation = std::max<u32>(1, bytes);
		}
		bool ObserveForwardSampleForValidation(u32 pc)
		{
			return SampleForwardRegionPc(pc);
		}
		void DiscardForwardSampleRequestForValidation()
		{
			m_forward_sample_request_pc = UINT32_MAX;
		}
		void SetBuildBudgetForValidation(u32 tokens, u64 refill_cycles);
		void SetProbeRetryCyclesForValidation(u32 cycles)
		{
			m_probe_retry_cycles = std::max<u32>(cycles, 1);
		}
		void SetAdmissionCapacityBlockedForValidation(bool blocked)
		{
			m_admission_capacity_blocked_for_validation = blocked;
		}
		bool OmitArmedProbeDirectoryOwnerForValidation()
		{
			if (m_armed_probe_count == 0)
				return false;
			m_armed_probes[--m_armed_probe_count] = nullptr;
			return true;
		}
		// Build one bounded source-backed forward/reducible CFG through the same
		// first-class persistent-dispatch compiler used by natural loops. Product
		// hotness and profitability policy remain disabled until native Cortex-A9
		// validation establishes the break-even.
		bool PrepareForwardRegionForValidation(u32 entry_pc);
		bool PrepareRepeatedForwardRegionForValidation(u32 entry_pc);
		bool PrepareObservedBackedgeForValidation(u32 source_start_pc,
			u32 branch_pc, u32 source_end_pc, u32 target_pc);
		// Read allocation/emission evidence for a validation-owned compiled entry
		// without executing it or relying on the activity-filtered profile window.
		bool GetEntrySnapshotForValidation(u32 entry_pc,
			Statistics::EntrySnapshot* snapshot) const;
		u32 ClassifyRequiredObservationsForValidation(
			const RegionIR::Program& program);
#endif

	private:
		struct Continuation
		{
			u32 resume_pc = 0;
			u32 pending_raw_cycles = 0;
			u32 source_end_pc = 0;
			u32 scaled_cycles = 0;
			size_t callable_entry_offset = 0;
			size_t persistent_entry_offset = 0;
			const void* persistent_entry_point = nullptr;
			bool scheduler_test_at_end = true;
		};

		struct Entry
		{
			VitaA32::CodeBuffer code;
			size_t code_slot_offset = static_cast<size_t>(-1);
			RegionA32::CompileResult compiled{};
			StateTransferPlan state_transfers{};
			RegionIR::LiftOptions options{};
			u32 entry_pc = 0;
			u32 canonical_entry_pc = 0;
			u32 source_generation = 0;
			u32 source_word_count = 0;
			u32 source_end_pc = 0;
			u32 block_count = 0;
			std::array<u32,
				PersistentGeneratedEntry::MAX_INTERNAL_SOURCE_BLOCKS>
				internal_source_blocks{};
			u8 internal_source_block_count = 0;
			u64 executions = 0;
			u64 reported_executions = 0;
			u64 profile_reported_executions = 0;
			u64 work_units = 0;
			u64 reported_work_units = 0;
			u64 last_use_serial = 0;
			std::array<Continuation, MAX_CONTINUATIONS_PER_REGION>
				continuations{};
			u8 continuation_count = 0;
			size_t persistent_entry_offset = static_cast<size_t>(-1);
			u32 generated_dispatch_executions = 0;
			u32 reported_generated_dispatch_executions = 0;
			u32 profile_reported_generated_dispatch_executions = 0;
			u32 generated_counted_iterations = 0;
			u32 profile_reported_generated_counted_iterations = 0;
			u32 generated_continuation_failures = 0;
			u32 reported_generated_continuation_failures = 0;
			u32 generated_profitability_fallbacks = 0;
			u32 reported_generated_profitability_fallbacks = 0;
			u32 generated_entry_state_fallbacks = 0;
			u32 reported_generated_entry_state_fallbacks = 0;
			bool active = false;
		};

		// Legacy callable-adapter support retained only until its now-unused
		// emitter is removed below. Product publication never enters it.
		struct TierZeroFallback
		{
			u32 pc = 0;
			const void* entry_point = nullptr;
		};

		enum class CandidateKind : u8
		{
			NaturalLoop,
			ForwardReducible,
		};

		struct PendingCandidate
		{
			u32 entry_pc = 0;
			u32 backedge_block_pc = 0;
			u32 branch_pc = 0;
			u32 source_end_pc = 0;
			u32 attempted_block_records = UINT32_MAX;
			bool valid = false;
			bool profitability_prescreened = false;
			// A deferred prepared-owner candidate must repeat the PCSX2-owned
			// topology proof once its missing return/latch owner appears.  Without
			// retaining this requirement, a stale partial graph could be published.
			bool require_repeated_path = false;
			// VU0 caller discovery must retain at least one verified JAL inside the
			// selected cycle. Otherwise compact-island selection collapses back to an
			// isolated macro leaf and recreates the canonical-state seam this ownership
			// tier exists to remove. Generic integer/MMI regions leave this false.
			bool require_direct_call_island = false;
			CandidateKind kind = CandidateKind::NaturalLoop;
		};

		struct NegativeCandidate
		{
			u32 entry_pc = 0;
			u32 source_end_pc = 0;
			u32 failure_pc = UINT32_MAX;
			CandidateKind kind = CandidateKind::NaturalLoop;
			BuildFailureStage failure_stage = BuildFailureStage::None;
			RepeatedCandidateOutcome outcome =
				RepeatedCandidateOutcome::AlreadyKnown;
			u32 failure_detail = 0;
			bool valid = false;
		};

		struct ProbeCandidate
		{
			PendingCandidate candidate{};
			VitaA32::CodeBuffer code;
			size_t code_slot_offset = static_cast<size_t>(-1);
			u32 observations = 0;
			u32 maximum_observations = 0;
			u32 observation_epoch = 0;
			u32 samples = 0;
			u32 reported_observations = 0;
			u32 idle_outer_boundaries = 0;
			u64 last_observation_serial = 0;
			u32 retry_cycle = 0;
			u8 retry_backoff_shift = 0;
			size_t persistent_entry_offset = static_cast<size_t>(-1);
			size_t compatible_entry_offset = static_cast<size_t>(-1);
			GprLinkSignature compatible_signature{};
			PersistentProbeIterationGuard iteration_guard{};
			u32 required_observations = 0;
			std::array<u32,
				PersistentGeneratedEntry::MAX_INTERNAL_SOURCE_BLOCKS>
				internal_source_blocks{};
			u8 internal_source_block_count = 0;
			bool has_iteration_guard = false;
			bool event_scoped_observations = false;
			bool dormant = false;
			bool armed = false;
			bool valid = false;
		};

		struct ProbePeak
		{
			// Product probe counters are reset in generated code at each scheduler
			// epoch and their owners may be retired before the next GS snapshot. Keep
			// only the cold, bounded top observations so attribution never adds work
			// to the persistent dispatcher.
			Statistics::ProbeSnapshot snapshot{};
			u64 serial = 0;
			bool valid = false;
		};

		struct ForwardSample
		{
			u32 pc = 0;
			u8 observations = 0;
			// One source generation needs at most one cold CFG transaction for an
			// entry. The resulting generated probe owns hotness and retries; repeated
			// scheduler samples must not keep unwinding for the same candidate.
			bool consumed = false;
			bool valid = false;
			u8 reserved = 0;
		};
		static_assert(sizeof(ForwardSample) == 8);

		struct DeferredCandidate
		{
			enum class Reason : u8
			{
				SourceDependency,
				AdmissionCapacity,
			};

			PendingCandidate candidate{};
			// An admission-capacity deferral owns the complete cold classification.
			// These source PCs are the immutable final graph identity and the guard is
			// its target-cost proof. Promotion copies them directly into the generated
			// probe owner; it never repeats the sparse sample or CFG walk.
			PersistentProbeIterationGuard iteration_guard{};
			std::array<u32,
				PersistentGeneratedEntry::MAX_INTERNAL_SOURCE_BLOCKS>
				internal_source_blocks{};
			u32 required_observations = 0;
			u32 missing_contract_pc = UINT32_MAX;
			// Retain the owner which satisfied this dependency until the candidate
			// is promoted. This lets its publication take causal priority without
			// allowing unrelated ready metadata to hide discovery for that owner.
			u32 ready_contract_pc = UINT32_MAX;
			// Monotonic cold-cache age. A source owner which has not appeared while
			// every bounded slot filled must not permanently exclude all later CFGs.
			u64 progress_serial = 0;
			u8 internal_source_block_count = 0;
			Reason reason = Reason::SourceDependency;
			bool has_iteration_guard = false;
			bool event_scoped_observations = false;
			bool classification_prepared = false;
			bool valid = false;
		};

		static RegionIR::LiftOptions CurrentOptions();
		static bool OptionsEqual(const RegionIR::LiftOptions& left,
			const RegionIR::LiftOptions& right);
		static bool DecodeBackwardBranchTo(
			u32 branch_pc, u32 opcode, u32 target_pc);
		bool QueueBackwardCandidate(u32 backedge_block_pc,
			u32 branch_pc, u32 source_end_pc, u32 target_pc,
			bool observed_backedge);
		bool ReadRamSourceWord(u32 pc, u32* word) const;
		bool HasEntryAtPc(u32 pc) const;
		bool HasCanonicalEntry(u32 canonical_pc) const;
		size_t FindEntryIndex(u32 pc) const;
		void RebuildEntryLookup();
		ProbeCandidate* FindProbe(u32 entry_pc, u32 source_end_pc,
			CandidateKind kind = CandidateKind::NaturalLoop);
		const ProbeCandidate* FindProbe(u32 entry_pc, u32 source_end_pc,
			CandidateKind kind = CandidateKind::NaturalLoop) const;
		ProbeCandidate* SelectProbeSlot();
		bool RepairArmedProbeDirectory();
		void ArmQueuedProbes();
		bool PrepareProductProbeEntries();
		bool EmitProductProbeEntry(ProbeCandidate* probe);
		void AdvanceProbeEpoch();
		void RetireEntryCode(Entry* entry);
		void RetireProbeCode(ProbeCandidate* probe);
		void RemoveProbe(ProbeCandidate* probe);
		bool ObserveArmedProbeEntry(u32 target_pc);
		bool PromoteRequestedProductProbe();
		bool SuspendSampledProductProbeMisses();
		bool SuspendProbeUntilRetry(ProbeCandidate* probe);
		bool ProcessGeneratedMaintenance();
		void SampleProductProbeLiveness();
		bool UseBarrierProbesForValidation() const;
		u32 ObservedRegionWorkFloor() const;
		void AdvanceEventScopedProbeEpoch();
		void RecordEventScopedProbeMaximum();
		void RecordProbePeak(const ProbeCandidate& probe);
		bool SampleForwardRegionPc(u32 sampled_pc);
		bool QueueForwardCandidateAtEntry(u32 entry_pc,
			bool require_repeated_path = false,
			bool* dependency_deferred = nullptr,
			u32* selected_entry_pc = nullptr,
			bool require_direct_call_island = false);
		bool SourceBlockContainsVu0Work(
			const RegionSourceBlockContract& source) const;
		bool QueuePreparedVu0CallerCandidate(
			const RegionSourceBlockContract& source);
		bool PrepareSampledForwardCandidate();
		bool ObservePersistentEventSample(u32 sampled_pc);
		bool ObservePersistentEventBoundarySlow(u32 sampled_pc);
		bool IsNegativeCandidate(u32 entry_pc, u32 source_end_pc,
			CandidateKind kind = CandidateKind::NaturalLoop) const;
		const NegativeCandidate* FindNegativeCandidate(u32 entry_pc,
			u32 source_end_pc, CandidateKind kind) const;
		bool IsDeferredCandidate(u32 entry_pc, u32 source_end_pc,
			CandidateKind kind = CandidateKind::NaturalLoop) const;
		void RememberNegativeCandidate(u32 entry_pc, u32 source_end_pc,
			CandidateKind kind = CandidateKind::NaturalLoop,
			BuildFailureStage failure_stage = BuildFailureStage::None,
			u32 failure_pc = UINT32_MAX,
			RepeatedCandidateOutcome outcome =
				RepeatedCandidateOutcome::AlreadyKnown,
			u32 failure_detail = 0);
		bool DeferCandidate(const PendingCandidate& candidate,
			u32 missing_contract_pc,
			DeferredCandidate::Reason reason =
				DeferredCandidate::Reason::SourceDependency);
		bool DeferAdmissionCandidate(const PendingCandidate& candidate);
		void MarkDeferredSourceReady(u32 prepared_pc);
		bool PromoteReadyDeferredCandidate(
			u32 causal_prepared_pc = UINT32_MAX);
		bool BuildBudgetAvailable() const;
		void RefillBuildBudget();
		u32 AvailableBuildTokens() const;
		u64 BuildBudgetWaitCycles() const;
		bool ProgramHasObservedBackedge(const RegionIR::Program& program,
			const PendingCandidate& candidate) const;
		bool DiscoverSourceBackedNaturalLoop(
			const PendingCandidate& candidate,
			std::vector<u32>* block_pcs, u32* missing_contract_pc) const;
		bool DiscoverSourceBackedForwardRegion(
			const PendingCandidate& candidate, std::vector<u32>* block_pcs,
			u32* source_end_pc, u32* missing_contract_pc,
			u32* selected_entry_pc = nullptr) const;
		u32 FindObservedBackedgeBlocker(const RegionIR::Program& program,
			const PendingCandidate& candidate) const;
		bool BuildPending();
		enum class ProfitabilityPrescreen : u8
		{
			Unknown,
			Proven,
			Rejected,
		};
		enum class PublishProfitabilityDecision : u8
		{
			Disabled,
			Proven,
			MissingCertificate,
			UncertifiedCop1Cost,
			UncertifiedVu0Cost,
			UncoveredMemoryCost,
			EmittedCostClassMismatch,
			UnprovenSemanticKernelCost,
		};
		ProfitabilityPrescreen PrescreenCandidateProfitability(
			const PendingCandidate& candidate);
		ProfitabilityPrescreen BuildProbeProfitabilityGuard(
			const PendingCandidate& candidate,
			PersistentProbeIterationGuard* guard,
			std::array<u32,
				PersistentGeneratedEntry::MAX_INTERNAL_SOURCE_BLOCKS>*
				internal_source_blocks = nullptr,
			u8* internal_source_block_count = nullptr,
			bool* use_entry_guard = nullptr,
			u32* required_observations = nullptr,
			bool* event_scoped_observations = nullptr);
		ProfitabilityPrescreen BuildProgramProfitabilityGuard(
			const RegionIR::Program& program,
			PersistentProbeIterationGuard* guard,
			std::array<u32,
				PersistentGeneratedEntry::MAX_INTERNAL_SOURCE_BLOCKS>*
				internal_source_blocks = nullptr,
			u8* internal_source_block_count = nullptr,
			bool* use_entry_guard = nullptr,
			u32* required_observations = nullptr,
			bool* event_scoped_observations = nullptr);
		void RememberProfitabilityPrescreenRejection(
			const PendingCandidate& candidate);
		void RememberUnclassifiedProfitabilityFallback(
			const PendingCandidate& candidate);
		PublishProfitabilityDecision EvaluatePublishProfitability(
			const RegionMemoryPlan::A9ProfitabilityCertificate& certificate,
			const RegionA32::CompileResult& compiled,
			bool persistent, bool uses_uncertified_cop1,
			bool uses_uncertified_vu0,
			bool classify_narrowed_preflighted_cost) const;
		void RecordRepeatedCandidateOutcome(const PendingCandidate& candidate,
			RepeatedCandidateOutcome outcome, u32 detail_pc = UINT32_MAX,
			u8 internal_source_blocks = 0,
			BuildFailureStage failure_stage = BuildFailureStage::None,
			u32 failure_detail = 0);
		bool BuildCandidate(const PendingCandidate& candidate,
			ProfitabilityPrescreen* classification_result = nullptr,
			PersistentProbeIterationGuard* classification_guard = nullptr,
			std::array<u32,
				PersistentGeneratedEntry::MAX_INTERNAL_SOURCE_BLOCKS>*
				classification_source_blocks = nullptr,
			u8* classification_source_block_count = nullptr,
			bool* classification_use_entry_guard = nullptr,
			u32* classification_required_observations = nullptr,
			bool* classification_event_scoped_observations = nullptr);
		static u64 EntryWindowExecutions(const Entry& entry);
		static u64 EntryProfileWindowExecutions(const Entry& entry);
		static Statistics::EntrySnapshot MakeEntrySnapshot(
			const Entry& entry, u64 executions);
		static void InsertEntrySnapshot(Statistics* statistics,
			const Statistics::EntrySnapshot& snapshot);
		static void AccumulateEntryShape(Statistics* statistics,
			const Entry& entry, u64 executions);
		bool AppendContinuations(const RegionIR::Program& program,
			const RegionA32::CompileResult& compiled,
			VitaA32::CodeBuffer* code,
			std::array<Continuation, MAX_CONTINUATIONS_PER_REGION>* continuations,
			u8* continuation_count, u32* failure_pc,
			bool append_callable);
		bool PatchPersistentExits(VitaA32::CodeBuffer* code,
			RegionA32::CompileResult& compiled,
			const std::array<Continuation, MAX_CONTINUATIONS_PER_REGION>& continuations,
			u8 continuation_count);
		bool AppendPersistentEntry(const RegionIR::Program& program,
			VitaA32::CodeBuffer* code, Entry* owner,
			const StateTransferPlan& state_transfers,
			const std::array<Continuation, MAX_CONTINUATIONS_PER_REGION>& continuations,
			u8 continuation_count, size_t* entry_offset);
		const Continuation* FindContinuation(const Entry& entry,
			u32 resume_pc, u32 pending_raw_cycles) const;
		bool RejectCandidate(const PendingCandidate& candidate,
			BuildFailureStage stage, u32 failure_pc,
			RegionA32::CompileFailure backend = RegionA32::CompileFailure::None,
			bool count_compile_failure = true, u8 backend_emission_step = 0,
			u16 backend_ir_opcode = UINT16_MAX,
			RegionIR::ValueId backend_value = RegionIR::INVALID_VALUE,
			u32 failure_detail = 0);
		Entry* FindEntry(u32 pc);
		const Entry* FindEntry(u32 pc) const;
		Entry* SelectEntryForReplacement(bool* evicted);
		void ResetForwardSampleForPc(u32 pc);
		void ClearCandidateHistory();

		BlockExecutor* m_executor = nullptr;
		std::array<Entry, MAX_REGIONS> m_entries{};
		std::array<u8, ENTRY_LOOKUP_CAPACITY> m_entry_lookup{};
		std::array<ProbeCandidate, MAX_CANDIDATE_HISTORY> m_probes{};
		std::array<ProbeCandidate*, MAX_ARMED_PROBES> m_armed_probes{};
		std::array<ProbePeak, Statistics::CANDIDATE_SNAPSHOT_CAPACITY>
			m_probe_peaks{};
		std::array<ForwardSample, FORWARD_SAMPLE_CAPACITY> m_forward_samples{};
		std::array<NegativeCandidate, MAX_CANDIDATE_HISTORY> m_negative_candidates{};
		std::array<DeferredCandidate, MAX_DEFERRED_CANDIDATES> m_deferred_candidates{};
		PendingCandidate m_pending{};
		RegionIR::LiftOptions m_options{};
		Statistics m_statistics{};
		Statistics m_profile_window_shapes{};
		u32 m_source_generation = 0;
		u32 m_bypass_once_pc = UINT32_MAX;
		u32 m_negative_insert = 0;
		u32 m_minimum_backedge_observations = MINIMUM_BACKEDGE_OBSERVATIONS;
		u32 m_max_active_regions = MAX_REGIONS;
		u64 m_use_serial = 0;
		u64 m_probe_serial = 0;
		u64 m_probe_peak_serial = 0;
		u64 m_deferred_progress_serial = 0;
		u64 m_repeated_candidate_serial = 0;
		u32 m_armed_probe_count = 0;
		u32 m_probe_sample_event_countdown = PROBE_SAMPLE_EVENT_INTERVAL;
		u32 m_forward_sample_event_countdown = PROBE_SAMPLE_EVENT_INTERVAL;
		u32 m_forward_sample_request_pc = UINT32_MAX;
		u32 m_probe_retry_cycles = PROBE_RETRY_INITIAL_EE_CYCLES;
		// Generated-directory and validation-barrier publication share one outer
		// transaction. This flag never authorizes mutation in a live dispatcher.
		bool m_publication_dirty = false;
		volatile u32 m_generated_maintenance_requested = 0;
		u32 m_probe_epoch = 1;
		u32 m_event_scoped_probe_count = 0;
		u64 m_build_budget_cycle = 0;
		u64 m_build_token_refill_cycles = BUILD_TOKEN_REFILL_EE_CYCLES;
		u32 m_build_token_capacity = BUILD_TOKEN_CAPACITY;
		u32 m_build_tokens = BUILD_TOKEN_CAPACITY;
		bool m_pending_budget_deferred = false;
		// This diagnostic cold-compile budget belongs to an immutable source
		// generation. A telemetry-window reset must not replenish compiler work on
		// the 496 MHz product core.
		u32 m_target_cost_analyses_generation = 0;
		bool m_require_proven_profitability = true;
		// Scalar COP1 lowering is oracle-validated but has not passed Phase 3's
		// complete persistent-dispatch A9 speed gate. Product discovery therefore
		// keeps it validation-only until the same unit compiler gains the Phase 4
		// lazy-flag/numeric-fact backend rather than admitting a known parity path.
		bool m_allow_uncertified_cop1_for_validation = false;
		// The IR/verifier/backend may be exercised by native validation, but the
		// product cannot publish VU0 regions until Phase 4 proves exact interlocks,
		// state materialization, and a real Cortex-A9 gain.
		bool m_allow_uncertified_vu0_for_validation = false;
		// Compile-and-discard target-cost inspection. This never authorizes
		// executable publication and is false in every ordinary product build.
		bool m_emit_unproven_target_cost_for_validation = false;
		// The controlled Vita activation build enables only classes which already
		// passed the physical-A9 retention gate. Ordinary products leave this false.
		bool m_semantic_kernel_lowering_enabled = false;
		u32 m_maximum_hot_code_bytes_for_validation = 0;
		// This independent floor is consulted only when the explicit VU0 validation
		// tier is active. It must never weaken ordinary integer/MMI admission.
		u32 m_minimum_vu0_counted_memory_work =
			RegionA32::CompileOptions::DEFAULT_MINIMUM_A9_VU0_COUNTED_MEMORY_WORK;
		u32 m_minimum_observed_vu0_fmac_iterations =
			RegionMemoryPlan::A9ProfitabilityCertificate::
				DEFAULT_MINIMUM_OBSERVED_VU0_FMAC_ITERATIONS;
		// Validation may explicitly enable this class. Product keeps it disabled
		// until an acyclic return can beat tier zero on Vita; discovery must instead
		// form the enclosing repeated unit which can retain VU0 state.
		u32 m_minimum_observed_vu0_fmac_leaf_invocations = 0;
#if defined(VITASX2_QEMU_VALIDATION)
		bool m_callable_execution_for_validation = false;
		bool m_final_profitability_heap_failure_for_validation = false;
		bool m_admission_capacity_blocked_for_validation = false;
		bool m_allow_unretained_semantic_kernel_lowering_for_validation = false;
		u32 m_minimum_counted_memory_work_for_validation =
			RegionA32::CompileOptions::DEFAULT_MINIMUM_A9_COUNTED_MEMORY_WORK;
#endif
	};
} // namespace VitaEE::RegionRuntime
