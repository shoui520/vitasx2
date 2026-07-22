// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

// Vita VU micro-mode block providers. VU1 compiles straight-line upper/lower
// instruction pairs from VU1 micro memory into A32 code that reproduces
// PCSX2's VU1microInterp.cpp::_vu1Exec() step semantics exactly: per-step
// cycle/budget accounting, VUops.cpp stall/pipe bookkeeping, hazard
// backup/discard rules, branch-delay countdown, and E-bit termination.
// VU0 currently uses the same pair emitter for scan-proven bodies, including
// lower LSU with VU0's data-memory/register-window map, VU0 XGKICK no-op
// behavior, VU0 branch-delay continuations, and VU0 M/D/T/E-bit stop handling.
// It falls back to PCSX2's VU0 interpreter for COP2 macro-visibility or other
// unsupported pairs.
namespace VitaVU
{
	struct Vu1ProviderStats
	{
		u64 executed_blocks = 0;
		u64 executed_pairs = 0;
		u64 interpreter_steps = 0;
		u32 compiled_blocks = 0;
		u32 compiled_pairs = 0;
		u64 generated_host_instructions = 0;
		u64 generated_host_load_instructions = 0;
		u64 generated_host_store_instructions = 0;
		u64 generated_helper_call_instructions = 0;
		u64 generated_state_load_instructions = 0;
		u64 generated_state_store_instructions = 0;
		u64 program_prepare_checks = 0;
		u64 program_prepare_calls = 0;
		u64 program_quick_cache_hits = 0;
		u64 program_version_cache_hits = 0;
		u64 program_block_maps_reused = 0;
		u32 program_versions_created = 0;
		u32 scan_rejects = 0;
		u32 compile_failures = 0;
		u32 cycle_resident_blocks = 0;
		u32 cycle_resident_pairs = 0;
		u64 cycle_resident_pair_instructions_removed = 0;
		u32 cycle_high_resident_blocks = 0;
		u32 cycle_high_resident_pairs = 0;
		u64 cycle_high_resident_hot_branches_removed = 0;
		u32 local_fmac_pipeline_blocks = 0;
		u32 local_fmac_pipeline_pairs = 0;
		u32 empty_pipeline_entry_blocks = 0;
		u32 empty_pipeline_entry_pairs = 0;
		u32 empty_pipeline_local_fmac_blocks = 0;
		u32 empty_pipeline_local_fmac_pairs = 0;
		u32 empty_pipeline_test_pipes_elisions = 0;
		u64 empty_pipeline_entry_executions = 0;
		u32 canonical_fmac_stall_tests_elided = 0;
		u32 scheduled_upper_stall_tests_elided = 0;
		u32 scheduled_lower_stall_tests_elided = 0;
		u32 scheduled_ialu_producers_elided = 0;
		u32 scheduled_vi_backup_writes_elided = 0;
		u32 scheduled_fmac_hazard_metadata_pairs = 0;
		u32 scheduled_local_fmac_warmup_pairs_elided = 0;
		u32 scheduled_local_fmac_relative_cycle_pairs = 0;
		u32 instant_qp_producers = 0;
		u32 instant_qp_waits_elided = 0;
		u32 local_fmac_cycle_snapshot_elision_pairs = 0;
		u32 resident_pipe_activity_blocks = 0;
		u32 resident_pipe_activity_pairs = 0;
		u64 local_fmac_pipeline_entries = 0;
		u64 local_fmac_pipeline_commits = 0;
		u64 local_fmac_cycle_snapshot_elisions = 0;
		u32 local_fmac_producer_snapshot_pairs = 0;
		u32 local_fmac_clip_snapshot_elisions = 0;
		u32 mac_flag_classification_elisions = 0;
		u32 canonical_mac_flag_classification_elisions = 0;
		u64 mac_flag_classification_minimum_instructions_removed = 0;
		u32 mvu_flag_hack_blocks = 0;
		u32 status_flag_classification_elisions = 0;
		u32 complete_flag_classification_elisions = 0;
		u64 local_fmac_producer_snapshot_entries = 0;
		u32 resident_working_fmac_flag_blocks = 0;
		u32 resident_working_fmac_flag_producers = 0;
		u32 resident_working_fmac_fdiv_barriers = 0;
		u64 resident_working_fmac_state_stores_removed = 0;
		u32 deferred_fmac_flag_blocks = 0;
		u32 deferred_fmac_flag_retirements = 0;
		u64 deferred_fmac_flag_entries = 0;
		u64 deferred_fmac_flag_runtime_retirements = 0;
		u64 canonical_deferred_fmac_runtime_retirements = 0;
		u32 deferred_fmac_compact_retirements = 0;
		u64 deferred_fmac_compact_runtime_retirements = 0;
		u64 deferred_fmac_compact_instructions_removed = 0;
		u64 deferred_fmac_linked_entries = 0;
		u64 deferred_fmac_linked_instructions_removed = 0;
		u64 deferred_fmac_linked_memory_words_removed = 0;
		u32 test_pipes_fast_guard_pairs = 0;
		u32 nop_pipe_test_defer_pairs = 0;
		u32 empty_pipe_nop_batch_runs = 0;
		u32 empty_pipe_nop_batch_pairs = 0;
		u64 empty_pipe_nop_batch_runtime_runs = 0;
		u64 empty_pipe_nop_batch_runtime_pairs = 0;
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
		u32 branch_continuation_blocks = 0;
		u32 branch_continuation_pairs = 0;
		u32 ebit_continuation_blocks = 0;
		u32 ebit_continuation_pairs = 0;
		u32 invalidate_alls = 0;
		u32 content_cache_hits = 0;
		u32 direct_link_exits = 0;
		u32 direct_link_patches = 0;
		u32 direct_link_runtime_patches = 0;
		u32 direct_link_runtime_observed_slots = 0;
		// Compile-time code-generation evidence, not runtime counters. Hits,
		// misses, evictions, and writebacks are allocator events; uncached_*
		// count counterfactual wrapper accesses; canonical_* count emitted
		// backing-state operations.
		u64 vector_cache_hits = 0;
		u64 vector_cache_misses = 0;
		u64 vector_cache_evictions = 0;
		u64 vector_cache_writebacks = 0;
		u64 vector_cache_acc_hits = 0;
		u64 vector_cache_scalar_invalidations = 0;
		u64 vector_cache_preloads = 0;
		u32 vector_cache_candidate_blocks = 0;
		u32 vector_cache_selected_blocks = 0;
		u32 vector_cache_rejected_blocks = 0;
		u64 vector_cache_baseline_instructions = 0;
		u64 vector_cache_selected_instructions = 0;
		u64 vector_cache_instructions_removed = 0;
		u64 vector_cache_canonical_bytes_removed = 0;
		u64 uncached_vector_loads = 0;
		u64 uncached_vector_stores = 0;
		u64 canonical_vf_word_loads = 0;
		u64 canonical_vf_word_stores = 0;
		u64 canonical_vf_quad_loads = 0;
		u64 canonical_vf_quad_stores = 0;
		u64 canonical_acc_word_loads = 0;
		u64 canonical_acc_word_stores = 0;
		u64 canonical_acc_quad_loads = 0;
		u64 canonical_acc_quad_stores = 0;
		// Compile-time representation evidence. PCSX2 microVU keeps the
		// clamped/normalized representation of resident VF/ACC values, while
		// Cortex-A9 scalar VFP under FZ natively consumes denormals as signed zero.
		// Count redundant vuDouble() operand work removed from emitted A32.
		u64 normalized_operand_quads_bypassed = 0;
		u64 normalization_instructions_removed = 0;
		u64 single_d_broadcast_operands = 0;
		u64 nearest_neon_fmac_ops = 0;
		u64 nearest_neon_scalar_ops_removed = 0;
		u64 nearest_neon_conversion_ops = 0;
		u64 nearest_neon_conversion_scalar_ops_removed = 0;
		u64 nearest_neon_half_ops = 0;
		u64 nearest_neon_efu_ops = 0;
		u64 nearest_neon_efu_scalar_ops_removed = 0;
		u64 approximate_q_ops = 0;
		u64 approximate_p_ops = 0;
		u64 linked_frame_entries = 0;
		u64 linked_frame_instructions_removed = 0;
		u64 linked_frame_stack_words_removed = 0;
		u64 resident_pipe_linked_entries = 0;
		u64 resident_pipe_linked_instructions_removed = 0;
		u64 resident_pipe_linked_state_loads_removed = 0;
		u32 code_cache_resets = 0;
		size_t code_cache_used = 0;
		size_t code_cache_capacity = 0;
	};

	// Cross-thread-safe, deliberately narrow view used by recurring Vita
	// performance windows. Compile/cache fields are EE-producer-owned; runtime
	// fields are release-published by the VU worker once per Execute() job.
	struct Vu1TelemetryStats
	{
		u64 completed_programs = 0;
		u64 executed_blocks = 0;
		u64 executed_pairs = 0;
		u64 interpreter_steps = 0;
		u64 generated_blocks = 0;
		u64 generated_pairs = 0;
		u64 generated_host_instructions = 0;
		u64 generated_host_load_instructions = 0;
		u64 generated_host_store_instructions = 0;
		u64 generated_helper_call_instructions = 0;
		u64 generated_state_load_instructions = 0;
		u64 generated_state_store_instructions = 0;
		u64 resident_working_fmac_flag_blocks = 0;
		u64 resident_working_fmac_flag_producers = 0;
		u64 resident_working_fmac_fdiv_barriers = 0;
		u64 resident_working_fmac_state_stores_removed = 0;
		u64 nearest_neon_fmac_ops = 0;
		u64 nearest_neon_scalar_ops_removed = 0;
		u64 nearest_neon_conversion_ops = 0;
		u64 nearest_neon_conversion_scalar_ops_removed = 0;
		u64 nearest_neon_half_ops = 0;
		u64 nearest_neon_efu_ops = 0;
		u64 nearest_neon_efu_scalar_ops_removed = 0;
		u64 approximate_q_ops = 0;
		u64 approximate_p_ops = 0;
		u64 neon_clip_pairs = 0;
		u64 mac_flag_classification_elisions = 0;
		u64 mac_flag_classification_minimum_instructions_removed = 0;
		u64 mvu_flag_hack_blocks = 0;
		u64 status_flag_classification_elisions = 0;
		u64 complete_flag_classification_elisions = 0;
		u64 scheduled_upper_stall_tests_elided = 0;
		u64 scheduled_lower_stall_tests_elided = 0;
		u64 scheduled_ialu_producers_elided = 0;
		u64 scheduled_vi_backup_writes_elided = 0;
		u64 scheduled_fmac_hazard_metadata_pairs = 0;
		u64 scheduled_local_fmac_warmup_pairs_elided = 0;
		u64 scheduled_local_fmac_relative_cycle_pairs = 0;
		u64 empty_pipeline_entry_blocks = 0;
		u64 empty_pipeline_entry_pairs = 0;
		u64 empty_pipeline_local_fmac_blocks = 0;
		u64 empty_pipeline_local_fmac_pairs = 0;
		u64 empty_pipeline_test_pipes_elisions = 0;
		u64 empty_pipeline_entry_executions = 0;
		u64 program_prepare_checks = 0;
		u64 program_prepare_calls = 0;
		u64 program_quick_cache_hits = 0;
		u64 program_compile_requests = 0;
		u64 program_version_cache_hits = 0;
		u64 program_block_maps_reused = 0;
		u64 program_versions_created = 0;
		u64 content_cache_hits = 0;
		u64 invalidations = 0;
		u64 compile_failures = 0;
	};

	// VU0 micro execution runs synchronously on the EE thread, so this snapshot
	// requires no cross-core publication atomics.  COP2 macro instructions are
	// accounted separately by the EE generated guest-family telemetry.
	struct Vu0TelemetryStats
	{
		u64 execute_calls = 0;
		u64 executed_blocks = 0;
		u64 executed_pairs = 0;
		u64 interpreter_steps = 0;
		u64 generated_blocks = 0;
		u64 generated_pairs = 0;
		u64 generated_host_instructions = 0;
		u64 generated_host_load_instructions = 0;
		u64 generated_host_store_instructions = 0;
		u64 generated_helper_call_instructions = 0;
		u64 generated_state_load_instructions = 0;
		u64 generated_state_store_instructions = 0;
		u64 content_cache_hits = 0;
		u64 invalidations = 0;
		u64 scan_rejects = 0;
		u64 compile_failures = 0;
	};

	// recMicroVU1::Execute() body: mirrors InterpVU1::Execute()'s loop while
	// routing eligible windows through compiled blocks.
	void ExecuteVu1Blocks(u32 cycles);

	// recMicroVU1::SetStartPC() hook. Pairs PCSX2's external-program-start seam
	// with the preceding natural program completion so the first A32 block can
	// consume the exact empty-pipeline state without reloading every pipe field.
	void LatchVu1ExternalProgramStart();

	// MTVU producer-side compilation seam. PSP2 VM-domain write mode is
	// process-wide, so the VU worker must never generate or patch code while the
	// EE thread can be executing its own VM-domain blocks. The EE producer calls
	// this only after draining the VU queue; it compiles the requested entry and
	// its statically-owned PCSX2 microVU link closure before execution is queued.
	bool Vu1ProgramNeedsPreparation(s32 vu_addr);
	void PrepareVu1Program(s32 vu_addr);

	// recMicroVU1::Clear() hook. VU1 micro writes clear the current program
	// selection while retaining immutable program-owned block maps for one-shot
	// content-matched reuse, mirroring x86 microVU's Clear()+program-search split.
	void InvalidateVu1Blocks(u32 addr, u32 size);

	// recMicroVU1::Reset()/Shutdown() hooks.
	void ResetVu1Blocks();
	void ShutdownVu1Blocks();

	Vu1ProviderStats GetVu1ProviderStats();
	Vu1TelemetryStats GetVu1TelemetryStats();
	void ResetVu1ProviderStats();

	// recMicroVU0::Execute() body: mirrors InterpVU0::Execute()'s loop while
	// routing scan-proven VU0 micro bodies through compiled A32 blocks.
	void ExecuteVu0Blocks(u32 cycles);

	// recMicroVU0::Clear()/Reset()/Shutdown() hooks.
	void InvalidateVu0Blocks(u32 addr, u32 size);
	void ResetVu0Blocks();
	void ShutdownVu0Blocks();

	Vu1ProviderStats GetVu0ProviderStats();
	Vu0TelemetryStats GetVu0TelemetryStats();
	void ResetVu0ProviderStats();
}
