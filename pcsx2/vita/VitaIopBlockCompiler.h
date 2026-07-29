// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"
#include "pcsx2/HostMemoryMap.h"
#include "pcsx2/MemoryTypes.h"
#include "pcsx2/vita/A32Emitter.h"

#include <array>
#include <bitset>
#include <cstddef>
#include <memory>
#include <vector>
#if defined(VITASX2_QEMU_VALIDATION)
#include <unordered_map>
#endif

namespace VitaIOP
{
// Marker for private bodies whose VFP contract is enforced by the d8-d15
// translation-unit reservations in VitaIopBlockCompiler.cpp.
#define VITASX2_IOP_PRIVATE_VFP_ABI

	enum class BlockExitKind : u32
	{
		Direct = 0x10300a32u,
		IsolateModeWrite = 0x10310a32u,
		LogicalContinuation = 0x10320a32u,
		LogicalContinuationIsolateModeWrite = 0x10330a32u,
	};

	struct BlockScanResult
	{
		u32 start_pc = 0;
		u32 instruction_count = 0;
		u32 stop_pc = 0;
		bool logical_continuation = false;
	};

#if defined(VITASX2_PORTABLE_REPLAY_VALIDATION) || \
	defined(VITASX2_PRODUCT_BOOT_VALIDATION)
	// Minimal product-dispatch sentinels for real-hardware all-native replay.
	// Keep the large QEMU profiling surface out of the PSP2 validation build.
	struct PortableValidationStats
	{
		u32 native_provider_entries = 0;
		u32 interpreter_fallback_entries = 0;
		u32 invalid_provider_results = 0;
	};
#endif

	enum class BlockScanStatus : u8
	{
		Success,
		InvalidStart,
		UnmappedStart,
		AnalysisCeiling,
		SourceBoundary,
	};

	// Successful executor calls publish every payload field and initialize the
	// five control flags explicitly. Keep this aggregate free of member
	// initializers: the provider hot path must not construct/clear the complete
	// telemetry object before RunValidatedBlock overwrites it.
	struct BlockExecutionResult
	{
		BlockExitKind exit;
		u32 instruction_count;
		u32 native_instruction_count;
		u32 helper_instruction_count;
		size_t code_size;
		size_t code_cache_footprint;
		u32 physical_fragment_count;
		u32 block_records;
		u32 link_records;
		u32 cache_slots;
		u32 code_cache_resets;
		size_t code_cache_used;
		size_t code_cache_capacity;
		bool isolate_mode_switched;
#if defined(VITASX2_QEMU_VALIDATION)
		u64 hot_dispatch_cache_hits;
		u64 hot_dispatch_cache_misses;
		u64 hot_dispatch_cache_way_probes;
		u64 hot_dispatch_cache_64_set_hits;
		u64 hot_dispatch_cache_64_set_misses;
		u64 hot_dispatch_cache_64_set_way_probes;
		u64 scheduler_direct_resume_candidates;
		u64 scheduler_direct_resume_installs;
		u64 scheduler_direct_resume_attempts;
		u64 scheduler_direct_resume_hits;
		u64 scheduler_direct_resume_misses;
		u64 scheduler_direct_resume_no_target;
		u64 scheduler_direct_resume_target_mismatch;
		u64 scheduler_direct_event_entries;
		u64 scheduler_direct_event_forwards;
		u64 scheduler_direct_event_fallbacks;
		u64 scheduler_direct_event_remainders;
		u64 scheduler_direct_event_installs;
		u64 scheduler_direct_event_clears;
		u64 scheduler_prediction_attempts;
		u64 scheduler_prediction_hits;
		u64 scheduler_prediction_misses;
		u64 scheduler_prediction_two_way_hits;
		u64 scheduler_prediction_four_way_hits;
		u64 scheduler_prediction_forwards;
		u64 scheduler_prediction_fallbacks;
		u64 scheduler_prediction_remainders;
		u64 scheduler_dispatch_cache_attempts;
		u64 scheduler_dispatch_cache_hits;
		u64 scheduler_dispatch_cache_misses;
		u64 scheduler_dispatch_cache_forwards;
		u64 scheduler_dispatch_cache_fallbacks;
		u64 scheduler_dispatch_cache_remainders;
		u64 scheduler_dispatch_cache_installs;
		u64 hot_dispatch_trusted_raw_hits;
		u64 hot_dispatch_owned_hits;
		u32 hot_dispatch_hit_pc_count;
		u32 hot_dispatch_hit_pcs[16];
		u64 hot_dispatch_hit_pc_hits[16];
		u32 interpreter_fallback_pc_count;
		u32 interpreter_fallback_pcs[16];
		u32 interpreter_fallback_owner_pcs[16];
		u32 interpreter_fallback_owner_opcodes[16];
		u32 interpreter_fallback_instruction_counts[16];
		u64 interpreter_fallback_source_hashes[16];
		u64 interpreter_fallback_hits[16];
		u64 wait_resume_cache_attempts;
		u64 wait_resume_cache_hits;
		u64 wait_resume_cache_misses;
		u64 wait_resume_event_entries;
		u64 wait_resume_event_forwards;
		u64 wait_resume_event_fallbacks;
		u64 wait_resume_event_installs;
		u64 wait_resume_event_clears;
		u64 wait_resume_first_entry_owned;
		u64 wait_resume_post_event_identity_checks;
		u64 wait_resume_kind_specific_entries;
		u64 wait_resume_kind_specific_unconditional_forwards;
		u64 wait_resume_kind_specific_poll_forwards;
		u64 wait_resume_kind_specific_conditional_forwards;
		u64 wait_resume_clock_specific_entries;
		u64 wait_resume_clock_specific_forwards;
		u64 wait_resume_no_link_specific_entries;
		u64 wait_resume_no_link_specific_forwards;
		u64 wait_resume_descriptor_forwards;
		u64 wait_resume_unconditional_forwards;
		u64 wait_resume_poll_forwards;
		u64 wait_resume_conditional_forwards;
		u64 direct_budget_exit_provider_entries;
		u64 constant_cycle_budget_provider_entries;
		u64 validation_calls;
		u64 validation_words;
		u64 raw_validation_calls;
		u64 raw_validation_words;
		u64 translated_validation_words;
		u64 wait_loop_configuration_checks;
		u64 trusted_source_hits;
		u64 trusted_source_audit_words;
		u64 trusted_source_audit_failures;
		u64 ram_invalidation_calls;
		u64 ram_invalidation_record_visits;
		u64 clock_mode_check_instructions_removed;
		u64 saved_register_stack_words_removed;
		u64 saved_register_frame_instructions_added;
		u64 saved_register_frame_instructions_removed;
		u64 batched_cycle_instructions_removed;
		u64 batched_cycle_stack_words_removed;
		u64 expanded_cycle_batching_provider_entries;
		u64 linked_frame_bypass_entries;
		u64 linked_frame_instructions_removed;
		u64 linked_frame_stack_words_removed;
		u64 sequential_qword_copy_fast_paths;
		u64 sequential_qword_copy_instructions_removed;
		u64 private_dispatcher_calls;
		u64 private_dispatcher_provider_entries;
		u64 private_dispatcher_wait_forwards;
		u64 private_dispatcher_generated_entries;
		u64 private_dispatcher_fallbacks;
		u64 private_dispatcher_inlined_hot_entries;
		u64 private_frame_provider_entries;
		u64 private_frame_stack_words_removed;
		u64 private_frame_zero_scratch_entries;
		u64 cached_wait_descriptor_checks;
		u64 cached_wait_descriptor_forwards;
		u64 cached_wait_descriptor_opcode_reads_removed;
		u64 cached_wait_descriptor_unconditional_checks;
		u64 compiled_ps1_bios_gate_blocks;
		u64 compiled_ps1_bios_gate_entries;
		u64 dispatcher_ps1_bios_gate_checks_removed;
		u64 inline_wait_fast_forwards;
		u64 branch_event_candidates;
		u64 branch_event_budget_positive;
		u64 branch_event_tests_entered;
		u64 budget_before_event_fast_exits;
		u64 budget_before_event_instructions_removed;
		u64 event_deadline_fast_skips;
		u64 event_deadline_instructions_removed;
		u64 total_pinned_gpr_memory_ops_saved;
		u64 total_pinned_branch_operand_moves_removed;
		u64 total_condition_code_branch_instructions_removed;
		u64 total_producer_branch_compare_instructions_removed;
		u64 total_fused_ram_guard_instructions_removed;
		u64 total_source_page_guard_instructions_removed;
		u64 total_source_page_literal_instructions_removed;
		u64 total_isolate_cache_guard_instructions_removed;
		u32 pinned_gpr_memory_ops_saved;
		u32 pinned_branch_operand_moves_removed;
		u32 condition_code_branch_instructions_removed;
		u32 producer_branch_compare_instructions_removed;
		u32 fused_ram_guard_instructions_removed;
		u32 source_page_guard_instructions_removed;
		u32 source_page_literal_instructions_removed;
		u32 isolate_cache_guard_instructions_removed;
#endif
		bool cache_hit;
		bool lookup_hit;
		bool fast_dispatch_hit;
		bool wait_loop_fast_forward;
	};

	enum ProviderDispatchFlag : u32
	{
		ProviderDispatchSuccess = 1u << 0,
		ProviderDispatchCacheHit = 1u << 1,
		ProviderDispatchLookupHit = 1u << 2,
		ProviderDispatchFastHit = 1u << 3,
		ProviderDispatchWaitForward = 1u << 4,
		ProviderDispatchIsolateSwitch = 1u << 5,
		ProviderDispatchIsolateWrite = 1u << 6,
		ProviderDispatchLogicalContinuation = 1u << 7,
	};

	enum class WaitLoopCondition : u8
	{
		Invalid,
		Always,
		Equal,
		NotEqual,
		LessThanZero,
		GreaterEqualZero,
		LessEqualZero,
		GreaterThanZero,
	};

	struct WaitLoopDescriptor
	{
		WaitLoopCondition condition = WaitLoopCondition::Invalid;
		u32 cycles = 0;
		u8 rs = 0;
		u8 rt = 0;
		bool writes_link = false;
	};

	enum class WaitResumeKind : u32
	{
		Unconditional,
		PollCall,
		Conditional,
		Invalid,
	};

	// PCSX2's iopEnterRecompiledCode dispatcher returns hot control state in
	// registers. Product cache hits leave this cold-compile metadata untouched;
	// QEMU fills it for the hot-PC report only.
	struct ProviderCompileResult
	{
		u32 instruction_count;
		u32 native_instruction_count;
		u32 helper_instruction_count;
		u32 code_cache_resets;
	};

	struct DirectLinkSlot
	{
		u32 target_pc = 0;
		size_t target_offset = static_cast<size_t>(-1);
		size_t fallback_offset = static_cast<size_t>(-1);
		size_t scheduler_resume_offset = static_cast<size_t>(-1);
		u16 fragment_index = 0;
		bool valid = false;
		bool logical_continuation = false;
	};

	struct DirectLinkSlots
	{
		DirectLinkSlot slots[2]{};
	};

	class BlockCompiler
	{
	public:
		explicit BlockCompiler(VitaA32::CodeBuffer& code,
			const u32* ram_source_page_live_counts,
			const u8* ram_source_page_live_flags,
			const u8* ram_source_chunk_live_flags,
			bool source_page_literal_allowed = true);

		static bool CanCompileOpcode(u32 op);

		bool CompileStraightLineBlock(
			u32 start_pc, u32 instruction_count, const void* direct_exit = nullptr,
			DirectLinkSlots* direct_links = nullptr,
			size_t* linked_entry_offset = nullptr,
			size_t* provider_entry_offset = nullptr,
			bool test_fallthrough_budget = true, bool allow_entry_gate = true,
			bool inherited_isolate_write = false, u32 logical_cycle_prefix = 0,
			u32 logical_cycle_total = 0, bool fragmented_logical_block = false,
			bool logical_continuation_tail = false);
		u32 NativeInstructionCount() const { return m_native_instruction_count; }
		u32 HelperInstructionCount() const { return m_helper_instruction_count; }
		bool UsesCompiledPs1BiosGate() const { return m_compiled_ps1_bios_gate; }
		bool UsesDirectBudgetExit() const { return m_has_budget_exit; }
		bool UsesConstantCycleBudget() const { return m_defer_cycle_updates; }
		bool WritesIsolateMode() const { return m_writes_isolate_mode; }
		u32 ClockModeCheckInstructionsRemoved() const
		{
			return m_clock_mode_check_instructions_removed;
		}
		u32 SavedRegisterStackWordsRemoved() const
		{
			return m_saved_register_stack_words_removed;
		}
		u32 SavedRegisterFrameInstructionsAdded() const
		{
			return m_saved_register_frame_instructions_added;
		}
		u32 SavedRegisterFrameInstructionsRemoved() const
		{
			return m_saved_register_frame_instructions_removed;
		}
		u32 BatchedCycleInstructionsRemoved() const
		{
			return m_batched_cycle_instructions_removed;
		}
		u32 BatchedCycleStackWordsRemoved() const
		{
			return m_batched_cycle_stack_words_removed;
		}
		bool UsesExpandedCycleBatching() const { return m_expanded_cycle_batching; }
		u16 SavedRegisters() const { return m_saved_registers; }
		u8 StackFrameSize() const { return m_stack_frame_size; }
		u32 PinnedGprMemoryOpsSaved() const { return m_pinned_gpr_memory_ops_saved; }
		u32 PinnedBranchOperandMovesRemoved() const
		{
			return m_pinned_branch_operand_moves_removed;
		}
		u32 ConditionCodeBranchInstructionsRemoved() const
		{
			return m_condition_code_branch_instructions_removed;
		}
		u32 ProducerBranchCompareInstructionsRemoved() const
		{
			return m_producer_branch_compare_instructions_removed;
		}
		u32 FusedRamGuardInstructionsRemoved() const
		{
			return m_fused_ram_guard_instructions_removed;
		}
		u32 SourcePageGuardInstructionsRemoved() const
		{
			return m_source_page_guard_instructions_removed;
		}
		u32 SourcePageLiteralInstructionsRemoved() const
		{
			return m_source_page_literal_instructions_removed;
		}
		u32 IsolateCacheGuardInstructionsRemoved() const
		{
			return m_isolate_cache_guard_instructions_removed;
		}
		bool SourcePageLiteralOutOfRange() const
		{
			return m_source_page_literal_out_of_range;
		}

	private:
		bool BeginBlock(size_t* linked_entry_offset, size_t* provider_entry_offset);
		bool EndBlockReturn(BlockExitKind exit, bool charge_budget = true,
			bool flush_pins = true, u32 known_cycle_count = 0);
		bool EndBlockIsolateModeWriteReturn(bool charge_budget = true,
			bool flush_pins = true,
			u32 known_cycle_count = 0);
		bool EndBlockLogicalContinuationReturn(bool charge_budget = true,
			bool flush_pins = true, u32 known_cycle_count = 0);
		bool EndBlockDirectTail(const void* direct_exit,
			DirectLinkSlot* direct_link_slot,
			u8 direct_link_slot_index, bool charge_budget = true,
			bool test_budget = true,
			BlockExitKind fallback_exit = BlockExitKind::Direct);
		bool EmitInstruction(u32 op, u32 pc, bool store_pc,
			std::vector<size_t>& trace_exit_branches);
		bool EmitNativeInstruction(u32 op, u32 pc);
		bool EmitNativeSPECIAL(u32 op, u32 pc);
		bool EmitNativeCOP0(u32 op);
		bool EmitNativeCOP2(u32 op);
		bool EmitStoreCode(u32 op);
		bool EmitSourcePageLiteralPool();
		bool EmitIsolateCacheGuard(size_t* isolated_branch);
		struct SequentialQwordCopy
		{
			u32 load_ops[4]{};
			u32 store_ops[4]{};
			u8 first_result = 0;
		};
		bool MatchSequentialQwordCopyShape(u32 start_pc, u32 instruction_index,
			u32 instruction_count,
			SequentialQwordCopy* copy) const;
		bool MatchSequentialQwordCopy(u32 start_pc, u32 instruction_index,
			u32 instruction_count,
			SequentialQwordCopy* copy) const;
		bool EmitSequentialQwordCopy(const SequentialQwordCopy& copy, u32 start_pc,
			u32 instruction_index);
		bool EmitTraceCheck(u32 pc, u32 op,
			std::vector<size_t>& direct_exit_branches);
		void AnalyzePinnedGprs(u32 start_pc, u32 instruction_count);
		void AnalyzeSavedRegisters(u32 start_pc, u32 instruction_count);
		int PinnedHostForGuest(unsigned guest_reg) const;
		bool EmitFlushPinnedGprs();
		void RecordPinnedGprExitPathSavings();
		bool EmitInterpreterTraceBranchHelperExit(const void* helper);
		void ResetGprConstState();
		bool TryGetKnownGpr(unsigned guest_reg, u32* value) const;
		void SetKnownGpr(unsigned guest_reg, u32 value);
		void ClearKnownGpr(unsigned guest_reg);
		bool TryGetKnownHiLo(bool lo, u32* value) const;
		void SetKnownHiLo(bool lo, u32 value);
		void ClearKnownHiLo(bool lo);
		void ClearKnownHiLo();
		void UpdateGprConstStateAfterOpcode(u32 op, u32 pc);
		bool TryKnownDirectIopRamAddress(u32 op, u8 alignment_mask,
			u32* address) const;
		bool EmitStorePc(u32 pc);
		bool EmitStorePcReg(unsigned host_reg);
		bool EmitAddCycles(u32 cycles);
		bool EmitPublishCyclePrefix(u32 cycle_prefix);
		void RecordBatchedCycleExitSavings(u32 cycle_prefix, bool preserves_argument);
		bool EmitChargeEeBudget(u32 known_cycle_count = 0, bool pins_flushed = true,
			u8 scheduler_resume_slot = UINT8_MAX,
			bool test_budget = true);
		bool BranchTestSchedulingEnabled() const;
		bool EmitBranchEventTest(u8 scheduler_resume_slot = UINT8_MAX);
		bool EmitQemuCounterIncrement(u32* counter);
		bool EmitChargeEeBudgetPs1(u32 known_block_cycles);
		bool EmitPcChangedExitCheck(u32 expected_pc,
			std::vector<size_t>& direct_exit_branches);
		bool EmitPcChangedExitCheckReg(unsigned expected_host_reg,
			std::vector<size_t>& direct_exit_branches);
		bool EmitLoadGpr(unsigned guest_reg, unsigned host_reg);
		bool EmitLoadGprValue(unsigned guest_reg, unsigned host_reg,
			bool* used_known_value);
		bool EmitStoreGpr(unsigned guest_reg, unsigned host_reg);
		bool EmitStoreGprZero(unsigned guest_reg);
		bool EmitMoveGpr(unsigned dst_guest_reg, unsigned src_guest_reg);
		bool EmitCompareGprs(unsigned lhs_guest_reg, unsigned rhs_guest_reg);
		bool EmitBinaryRegOp(u32 op);
		bool EmitShiftImmOp(u32 op);
		bool EmitShiftRegOp(u32 op);
		bool EmitSetLessThanRegOp(u32 op, bool is_signed);
		bool EmitMultiplyOp(u32 op, bool is_signed);
		bool EmitDivideOp(u32 op, bool is_signed);
		bool EmitExceptionOp(u32 pc, u32 code);
		bool EmitPrivateCycleHelperCall(const void* helper);
		bool EmitImmediateOp(u32 op, u32 pc);
		void ResolveIrxImport(u32 marker_pc, u32 marker_op);
		bool EmitIrxImportMarker(u32 marker_pc, u32 marker_op);
		bool EmitEffectiveAddress(u32 op);
		bool EmitEffectiveAddress(u32 op, unsigned host_reg);
		bool EmitLoadOp(u32 op);
		bool EmitKnownDirectRamLoadOp(u32 op, u32 address);
		bool EmitStoreOp(u32 op);
		bool EmitKnownDirectRamStoreOp(u32 op, u32 address);
		bool EmitUnalignedLoadOp(u32 op);
		bool EmitKnownDirectRamUnalignedLoadOp(u32 op, u32 address);
		bool EmitUnalignedStoreOp(u32 op);
		bool EmitKnownDirectRamUnalignedStoreOp(u32 op, u32 address);
		bool EmitConditionalBranchOp(u32 op, u32 pc);
		bool EmitSignedBranchOp(u32 op, u32 pc);
		bool EmitConditionalBranchFlag(u32 op);
		bool EmitSignedBranchFlag(u32 op);
		bool EmitJumpOp(u32 op, u32 pc);
		bool EmitStaticJumpOp(u32 op, u32 pc);
		bool EmitRegisterJumpOp(u32 op, u32 pc);
		bool EmitRegisterJumpCaptureOp(u32 op, u32 pc);
		bool EmitIopEventTestFastPath();
		bool EmitCop0TransferOp(u32 op, bool to_cop0);
		bool EmitCop0RfeOp();
		bool EmitCop2CommandOp(u32 op);
		bool EmitReadCop2DataReg(unsigned cop2_reg, unsigned host_reg);
		bool EmitWriteCop2DataReg(unsigned cop2_reg, unsigned host_reg);
		bool EmitCop2LoadStoreOp(u32 op);
		bool EmitWaitLoopFastForwardBlock(u32 start_pc, u32 block_cycles);
		bool EmitCompiledPs1BiosGate();
		bool EmitKnownDirectRamCop2LoadOp(u32 op, u32 address);
		bool EmitKnownDirectRamCop2StoreOp(u32 op, u32 address);
		bool FlushColdTails();

		struct ScalarLoadColdTail
		{
			size_t fallback_branch = static_cast<size_t>(-1);
			size_t alignment_fallback_branch = static_cast<size_t>(-1);
			size_t join_offset = 0;
			const void* helper = nullptr;
			unsigned rt = 0;
			unsigned opcode = 0;
		};
		bool EmitScalarLoadColdTail(const ScalarLoadColdTail& tail);

		struct ScalarStoreColdTail
		{
			size_t fallback_branch = static_cast<size_t>(-1);
			size_t alignment_fallback_branch = static_cast<size_t>(-1);
			size_t isolated_fallback_branch = static_cast<size_t>(-1);
			size_t join_offset = 0;
			const void* helper = nullptr;
			unsigned rt = 0;
		};
		bool EmitScalarStoreColdTail(const ScalarStoreColdTail& tail);

		struct UnalignedReadColdTail
		{
			size_t fallback_branch = static_cast<size_t>(-1);
			size_t join_offset = 0;
		};
		bool EmitUnalignedReadColdTail(const UnalignedReadColdTail& tail);

		struct UnalignedWriteColdTail
		{
			size_t write_fallback_branch = static_cast<size_t>(-1);
			size_t isolated_fallback_branch = static_cast<size_t>(-1);
			size_t join_offset = 0;
		};
		bool EmitUnalignedWriteColdTail(const UnalignedWriteColdTail& tail);

		struct Cop2LoadColdTail
		{
			size_t fallback_branch = static_cast<size_t>(-1);
			size_t alignment_fallback_branch = static_cast<size_t>(-1);
			size_t join_offset = 0;
			unsigned cop2_reg = 0;
		};
		bool EmitCop2LoadColdTail(const Cop2LoadColdTail& tail);

		struct Cop2StoreColdTail
		{
			size_t fallback_branch = static_cast<size_t>(-1);
			size_t alignment_fallback_branch = static_cast<size_t>(-1);
			size_t isolated_fallback_branch = static_cast<size_t>(-1);
			size_t join_offset = 0;
		};
		bool EmitCop2StoreColdTail(const Cop2StoreColdTail& tail);

		VitaA32::CodeBuffer& m_code;
		const u32* m_ram_source_page_live_counts = nullptr;
		const u8* m_ram_source_page_live_flags = nullptr;
		const u8* m_ram_source_chunk_live_flags = nullptr;
		bool m_source_page_literal_allowed = true;
		std::vector<ScalarLoadColdTail> m_scalar_load_cold_tails;
		std::vector<ScalarStoreColdTail> m_scalar_store_cold_tails;
		std::vector<UnalignedReadColdTail> m_unaligned_read_cold_tails;
		std::vector<UnalignedWriteColdTail> m_unaligned_write_cold_tails;
		std::vector<Cop2LoadColdTail> m_cop2_load_cold_tails;
		std::vector<Cop2StoreColdTail> m_cop2_store_cold_tails;
		std::vector<size_t> m_source_page_literal_loads;
		std::vector<size_t> m_source_chunk_literal_loads;
		u32 m_native_instruction_count = 0;
		u32 m_helper_instruction_count = 0;
		u16 m_saved_registers = 0;
		u16 m_required_saved_registers = 0;
		u8 m_stack_frame_size = 0;
		struct PinnedGpr
		{
			u8 guest = 0;
			u8 host = 0;
			bool needs_initial_load = false;
			bool written = false;
			bool ever_written = false;
		};
		std::array<PinnedGpr, 2> m_pinned_gprs{};
		u8 m_pinned_gpr_count = 0;
		u32 m_pinned_gpr_load_hits = 0;
		u32 m_pinned_gpr_store_hits = 0;
		u32 m_pinned_gpr_initial_loads = 0;
		u32 m_pinned_gpr_memory_ops_saved = 0;
		u32 m_pinned_branch_operand_moves_removed = 0;
		u32 m_condition_code_branch_instructions_removed = 0;
		u32 m_producer_branch_compare_instructions_removed = 0;
		u32 m_fused_ram_guard_instructions_removed = 0;
		u32 m_source_page_guard_instructions_removed = 0;
		u32 m_source_page_literal_instructions_removed = 0;
		u32 m_isolate_cache_guard_instructions_removed = 0;
		u32 m_pinned_gpr_min_exit_savings = UINT32_MAX;
		std::vector<size_t>* m_direct_exit_branches = nullptr;
		std::vector<size_t>* m_budget_exit_branches = nullptr;
		std::vector<size_t>* m_unflushed_budget_exit_branches = nullptr;
		std::array<std::vector<size_t>*, 2> m_scheduler_budget_exit_branches{};
		std::array<std::vector<size_t>*, 2>
			m_unflushed_scheduler_budget_exit_branches{};
		u32 m_block_cycle_count = 0;
		u32 m_clock_mode_check_instructions_removed = 0;
		u32 m_saved_register_stack_words_removed = 0;
		u32 m_saved_register_frame_instructions_added = 0;
		u32 m_saved_register_frame_instructions_removed = 0;
		u32 m_batched_cycle_instructions_removed = 0;
		u32 m_batched_cycle_stack_words_removed = 0;
		u32 m_current_instruction_count = 0;
		u32 m_current_cycle_count = 0;
		u32 m_logical_cycle_prefix = 0;
		u32 m_budget_cycle_count = 0;
		bool m_iop_ram_registers_available = false;
		bool m_iop_ram_mask_register_available = false;
		bool m_iop_cycle_base_register_available = false;
		bool m_defer_cycle_updates = false;
		bool m_expanded_cycle_batching = false;
		bool m_fragmented_logical_block = false;
		bool m_has_budget_exit = false;
		bool m_source_page_literal_out_of_range = false;
		bool m_isolate_cache_specialization = false;
		bool m_isolate_cache_guard_stable = false;
		bool m_isolate_cache_active = false;
		bool m_writes_isolate_mode = false;
		bool m_compiled_ps1_bios_gate = false;
		bool m_emit_trace_checks = false;
		bool m_emit_native_static_branch = false;
		bool m_emit_native_static_branch_flags = false;
		bool m_emit_branch_predicate_producer = false;
		bool m_emit_native_static_jump = false;
		bool m_emit_native_register_jump = false;
		bool m_static_branch_outcome_known = false;
		bool m_static_branch_taken = false;
		bool m_static_branch_flags_live = false;
		VitaA32::Condition m_static_branch_taken_condition = VitaA32::Condition::AL;
		bool m_branch_predicate_producer_flags_live = false;
		unsigned m_branch_predicate_producer_guest = 0;
		VitaA32::Condition m_branch_predicate_producer_true_condition =
			VitaA32::Condition::AL;
		bool m_register_jump_target_known = false;
		u32 m_register_jump_target = 0;
		bool m_emit_irx_import = false;
		bool m_irx_import_log = false;
		u32 m_irx_import_table = 0;
		u16 m_irx_import_index = 0;
		const void* m_irx_import_hle = nullptr;
		const void* m_irx_import_debug = nullptr;
		const char* m_irx_import_funcname = nullptr;
		std::array<u32, 32> m_gpr_const_values{};
		u32 m_gpr_const_known_mask = 1;
		std::array<u32, 2> m_hilo_const_values{};
		u8 m_hilo_const_known_mask = 0;
	};

	class BlockExecutor
	{
	public:
		// A PCSX2 logical IOP block is allowed to span host-code and guest-page
		// allocation boundaries. A32 generation is deliberately bounded per
		// fragment; only the complete logical block owns scheduling and events.
		static constexpr u32 MAX_STRAIGHT_LINE_BLOCK_INSTRUCTIONS = 64;
		static constexpr u32 MAX_LOGICAL_BLOCK_INSTRUCTIONS = 0xffff;

		explicit BlockExecutor(bool owns_ee_event_entry = false);
		~BlockExecutor();

		u32 Reset();
		u32 Shutdown();
		void ResetInstrumentationCounters();
#if defined(VITASX2_PORTABLE_REPLAY_VALIDATION) || \
	defined(VITASX2_PRODUCT_BOOT_VALIDATION)
		PortableValidationStats GetPortableValidationStats() const;
#endif
		void NotifyPcDiscontinuity();
		u32 InvalidateRange(u32 start_pc, u32 instruction_count);
		bool MayInvalidateRange(u32 start_pc, u32 instruction_count) const;
		void SetDirectLinkingEnabled(bool enabled);
		u32 GetCodeCacheResetCount() const { return m_code_cache_resets; }
		u32 GetSemanticBlockDescriptorCount() const
		{
			return static_cast<u32>(m_semantic_block_descriptors.size());
		}
		u32 GetCodeCacheBlockRecordCount() const
		{
			return static_cast<u32>(m_block_records.size());
		}
		u32 GetCodeCacheSlotCount() const
		{
			return static_cast<u32>(m_cache.size());
		}
		size_t GetCodeCacheUsed() const { return m_code_cache_used; }
		size_t GetCodeCacheCapacity() const { return m_code_cache_capacity; }
#if defined(VITASX2_QEMU_VALIDATION)
		void SnapshotInstrumentation(BlockExecutionResult* result) const;
#endif
		static void SetTrustedSourceAuditEnabled(bool enabled);
		static void SetPinnedGprResidencyEnabled(bool enabled);
		static void SetPinnedBranchDirectCompareEnabled(bool enabled);
		static void SetConditionCodeBranchEnabled(bool enabled);
		static void SetProducerBranchFlagsEnabled(bool enabled);
		static void SetRamProvenanceSpecializationEnabled(bool enabled);
		static void SetSourcePageLiteralEnabled(bool enabled);
		static void SetIsolateCacheSpecializationEnabled(bool enabled);
		static void SetClockModeSpecializationEnabled(bool enabled);
		static void SetSavedRegisterNarrowingEnabled(bool enabled);
		static void SetBlockCycleBatchingEnabled(bool enabled);
		static void SetLinkedFrameBypassEnabled(bool enabled);
		static void SetSequentialQwordCopyEnabled(bool enabled);
		static void SetBranchTestSchedulingEnabled(bool enabled);
		static void SetPrivateDispatcherHotPathEnabled(bool enabled);
		static void SetHotDispatchOwnershipEnabled(bool enabled);
		static void SetCachedWaitDescriptorEnabled(bool enabled);
		static void SetInlineWaitFastForwardEnabled(bool enabled);
		static void SetWaitResumeCacheEnabled(bool enabled);
		static void SetWaitResumeFirstEntryOwnershipEnabled(bool enabled);
		static void SetWaitResumeKindEntryEnabled(bool enabled);
		static void SetWaitResumeClockEntryEnabled(bool enabled);
		static void SetWaitResumeNoLinkEntryEnabled(bool enabled);
		static void SetWaitResumeDescriptorSpecializationEnabled(bool enabled);
		static void SetCompiledPs1BiosGateEnabled(bool enabled);
		static bool CompiledPs1BiosGateEnabled();
		static void SetSchedulerDirectResumeEnabled(bool enabled);
		static void SetNullOpcodeCompilationEnabled(bool enabled);
		static bool SchedulerDirectResumeEnabled();
		static bool ReadRecompilerOwnedOpcode(u32 pc, u32* opcode);
		static void SetCodeCacheCapacityLimit(size_t capacity);
		static bool TryFastForwardWaitLoopAtPc(u32 start_pc);
		static bool ScanStraightLineBlock(u32 start_pc, u32 max_instruction_count,
			BlockScanResult* result);
		bool ExecuteCompiledBlock(u32 start_pc, u32 instruction_count,
			BlockExecutionResult* result,
			bool publish_details = true);
		bool ExecuteCompiledBlockAtPc(u32 start_pc, BlockExecutionResult* result,
			bool publish_details = true,
			bool forced_continuation = false);
		u32 ExecuteProviderBlockAtPc(u32 start_pc,
			ProviderCompileResult* compile_result,
			bool forced_continuation = false);
		s32 ExecuteInterpreterFallbackTimeslice(s32 ee_cycles);
		s32 ExecuteInterpreterRecompilerBlock(s32 ee_cycles,
			bool* logical_continuation = nullptr,
			bool* execution_terminated = nullptr);
#if defined(__arm__)
		__attribute__((naked, noinline)) s32 ExecuteProviderTimeslice(s32 ee_cycles);
#else
		s32 ExecuteProviderTimeslice(s32 ee_cycles);
#endif

	private:
		// PCSX2 owner: x86/BaseblockEx.h::BaseBlocks() starts at 0x4000
		// BASEBLOCKEX records and grows from there. Vita keeps the same
		// game-scale order while bounding metadata for the smaller memory budget.
		static constexpr size_t INITIAL_CACHE_CAPACITY = 512;
		static constexpr size_t MAX_CACHE_CAPACITY = 0x4000;
		static constexpr size_t STRAIGHT_LINE_BLOCK_CODE_CAPACITY = 4096;
		static constexpr size_t MAX_STRAIGHT_LINE_BLOCK_CODE_CAPACITY = 16 * 1024;
		static constexpr size_t IOP_CODE_CACHE_CAPACITY = HostMemoryMap::IOPrecSize;
		static constexpr size_t CODE_CACHE_ALIGNMENT = 32;
		static constexpr size_t DIRECT_LINK_SLOT_COUNT = 2;
		static constexpr u32 SCHEDULER_DIRECT_RESUME_TAG = 1u;
		static constexpr size_t MAX_INCOMING_LINKS =
			MAX_CACHE_CAPACITY * DIRECT_LINK_SLOT_COUNT;
		static constexpr u32 LOOKUP_DIRECTORY_ENTRY_COUNT = 0x10000;
		static constexpr u32 LOOKUP_PAGE_ENTRY_COUNT = 0x4000;
		// PCSX2's R3000A dispatcher indexes psxRecLUT directly by guest PC.
		// A full flat map is wasteful on Vita, so keep the hottest exact mappings
		// in a Cortex-A9 D-cache-sized first level ahead of the lazy page table.
		// 512 sets use an 8 KiB active isolate bank and still compile to one UBFX;
		// smaller geometries discarded high-value PC bits and forced millions of
		// otherwise exact hits through their second way.
		static constexpr u32 HOT_DISPATCH_CACHE_SET_COUNT = 512;
		static constexpr u32 HOT_DISPATCH_CACHE_WAY_COUNT = 2;
#if defined(VITASX2_QEMU_VALIDATION)
		static constexpr u32 HOT_DISPATCH_CACHE_CONTROL_SET_COUNT = 64;
#endif
		// PCSX2's recClearIOP() invalidates only blocks whose protected source
		// pages were written. Use host-page-sized buckets here too: a 64 KiB LUT
		// bucket makes every ordinary IOP RAM store walk unrelated blocks.
		static constexpr u32 RAM_SOURCE_PAGE_SHIFT = 12;
		static constexpr u32 RAM_SOURCE_PAGE_SIZE = 1u << RAM_SOURCE_PAGE_SHIFT;
		static constexpr u32 RAM_SOURCE_PAGE_COUNT =
			Ps2MemSize::TotalIopRam / RAM_SOURCE_PAGE_SIZE;
		// PCSX2 x86's PSXREC_CLEARM tests the exact recLUT word before entering
		// psxRecClearMem(). A byte per 64 guest bytes gives A32 a compact second
		// level after the page guard, eliminating code/data page-sharing false
		// positives without putting an 18 MiB exact-word count table on Vita.
		static constexpr u32 RAM_SOURCE_CHUNK_SHIFT = 6;
		static constexpr u32 RAM_SOURCE_CHUNK_SIZE =
			1u << RAM_SOURCE_CHUNK_SHIFT;
		static constexpr u32 RAM_SOURCE_CHUNK_COUNT =
			Ps2MemSize::TotalIopRam / RAM_SOURCE_CHUNK_SIZE;
		static constexpr u32 INVALID_RAM_SOURCE = UINT32_MAX;
		struct RamSourceChunkOwnership
		{
			std::array<u32, RAM_SOURCE_CHUNK_COUNT> live_counts{};
			std::array<u8, RAM_SOURCE_CHUNK_COUNT> live_flags{};
		};

		struct CachedBlock
		{
			CachedBlock* next_free = nullptr;
			struct CodeFragment
		{
			VitaA32::CodeBuffer code;
				u32 start_pc = 0;
				u32 instruction_count = 0;
				size_t linked_entry_offset = 0;
				size_t provider_entry_offset = 0;
			};

			// Keep the overwhelmingly common <=64-opcode/single-fragment block
			// allocation-free. Only a PCSX2 logical block which actually crosses
			// an A32 code-generation boundary allocates overflow metadata.
			CodeFragment first_fragment;
			std::vector<CodeFragment> overflow_fragments;
			std::array<u32, MAX_STRAIGHT_LINE_BLOCK_INSTRUCTIONS> opcodes{};
			std::vector<u32> overflow_opcodes;
			u16 fragment_count = 0;

			bool HasFragments() const { return fragment_count != 0; }
			CodeFragment& Fragment(u32 index)
			{
				return index == 0 ? first_fragment : overflow_fragments[index - 1];
			}
			const CodeFragment& Fragment(u32 index) const
			{
				return index == 0 ? first_fragment : overflow_fragments[index - 1];
			}
			void ReserveFragments(u32 count)
			{
				if (count > 1)
					overflow_fragments.reserve(count - 1);
			}
			CodeFragment& AddFragment()
			{
				if (fragment_count++ == 0)
					return first_fragment;
				overflow_fragments.emplace_back();
				return overflow_fragments.back();
			}
			void ClearFragments()
			{
				first_fragment = {};
				overflow_fragments.clear();
				fragment_count = 0;
			}
			void PrepareOpcodes(u32 count)
			{
				overflow_opcodes.resize(count > opcodes.size() ? count - opcodes.size() : 0);
			}
			u32& Opcode(u32 index)
			{
				return index < opcodes.size() ? opcodes[index] : overflow_opcodes[index - opcodes.size()];
			}
			u32 Opcode(u32 index) const
			{
				return index < opcodes.size() ? opcodes[index] : overflow_opcodes[index - opcodes.size()];
			}
			void ClearOpcodes() { overflow_opcodes.clear(); }
			void ReleaseOversizedMetadata()
			{
				if (overflow_fragments.capacity() > 8)
					std::vector<CodeFragment>().swap(overflow_fragments);
				if (overflow_opcodes.capacity() > 512)
					std::vector<u32>().swap(overflow_opcodes);
			}
			const u32* raw_opcodes = nullptr;
			u32 ram_source_start = INVALID_RAM_SOURCE;
			const u32* poll_branch_opcodes = nullptr;
			const u32* poll_leaf_opcodes = nullptr;
			std::array<u32, 2> poll_branch_expected{};
			std::array<u32, 5> poll_leaf_expected{};
			u32 poll_branch_source_start = INVALID_RAM_SOURCE;
			u32 poll_leaf_source_start = INVALID_RAM_SOURCE;
			u32 poll_word_address = 0;
			u32 source_serial = 0;
			u32 start_pc = 0;
			u32 rec_lookup_identity = UINT32_MAX;
			u32 rec_link_identity = UINT32_MAX;
			u32 instruction_count = 0;
			u32 native_instruction_count = 0;
			u32 helper_instruction_count = 0;
			bool compiled_ps1_bios_gate = false;
			u32 pinned_gpr_memory_ops_saved = 0;
			u32 pinned_branch_operand_moves_removed = 0;
			u32 condition_code_branch_instructions_removed = 0;
			u32 producer_branch_compare_instructions_removed = 0;
			u32 fused_ram_guard_instructions_removed = 0;
			u32 source_page_guard_instructions_removed = 0;
			u32 source_page_literal_instructions_removed = 0;
			u32 isolate_cache_guard_instructions_removed = 0;
			u32 clock_mode_check_instructions_removed = 0;
			u32 saved_register_stack_words_removed = 0;
			u32 saved_register_frame_instructions_added = 0;
			u32 saved_register_frame_instructions_removed = 0;
			u32 batched_cycle_instructions_removed = 0;
			u32 batched_cycle_stack_words_removed = 0;
			bool expanded_cycle_batching = false;
			u16 saved_registers = 0;
			u8 stack_frame_size = 0;
			DirectLinkSlots direct_links{};
			bool wait_loop_shape = false;
			bool wait_loop_enabled_at_compile = false;
			WaitLoopDescriptor wait_loop_descriptor{};
			bool poll_call_wait_loop = false;
			bool inline_ram_poll_wait_loop = false;
			bool direct_budget_exit = false;
			bool constant_cycle_budget = false;
			bool isolate_cache_active = false;
			bool logical_continuation = false;
			bool discovered_topology = false;
			bool trusted_source = false;
			u8 poll_result_register = 0;
			u8 poll_load_opcode = 0;
			bool valid = false;
			bool queued_free = false;

			bool HasRamPollWaitLoop() const
			{
				return poll_call_wait_loop || inline_ram_poll_wait_loop;
			}
		};

	public:
		static constexpr u32 SchedulerPredictionSecondOffset();
		static constexpr u32 SchedulerDispatchCacheEntryCount() { return 64; }
		static constexpr u32 SchedulerDispatchCacheOffset();

	private:
		struct LookupPage
		{
			std::array<std::array<CachedBlock*, LOOKUP_PAGE_ENTRY_COUNT>, 2> blocks{};
		};

		struct HotDispatchCacheEntry
		{
			CachedBlock* block = nullptr;
			u32 rec_lookup_identity = UINT32_MAX;
		};

		struct IncomingLinkRecord
		{
			CachedBlock* source = nullptr;
			u32 target_lookup_identity = UINT32_MAX;
			u32 target_link_identity = UINT32_MAX;
			u8 slot_index = 0;
		};

		struct InterpreterFallbackBlock
		{
			std::vector<u32> opcodes;
			u32 start_pc = 0;
			u32 rec_lookup_identity = UINT32_MAX;
			u32 rec_link_identity = UINT32_MAX;
			u32 instruction_count = 0;
			u32 stop_pc = 0;
			u32 ram_source_start = INVALID_RAM_SOURCE;
			u32 source_serial = 0;
			bool logical_continuation = false;
			bool valid = false;
		};

		struct RamSourceRecord
		{
			CachedBlock* block = nullptr;
			InterpreterFallbackBlock* fallback = nullptr;
			u32 serial = 0;
		};

		struct BlockRecord
		{
			CachedBlock* block = nullptr;
			u32 rec_lookup_identity = UINT32_MAX;
		};
#if defined(__arm__)
		static_assert(sizeof(BlockRecord) == 8);
#endif

		// PCSX2's recLUT entry is both a code pointer and the semantic fact that a
		// BaseBlock starts at this guest word. Vita's much smaller physical code
		// arena can recycle code without discarding that second fact. Source extent
		// remains attached so psxRecClearMem()-owned SMC can retire it selectively.
		struct SemanticBlockDescriptor
		{
			u32 rec_lookup_identity = UINT32_MAX;
			u32 ram_source_start = INVALID_RAM_SOURCE;
			u32 instruction_count = 0;
			bool logical_continuation = false;
		};

		static inline __attribute__((always_inline)) u32 RecLookupIdentity(u32 pc);
		static inline __attribute__((always_inline)) u32 RecLinkIdentity(u32 pc);
		static u32 LookupPageIndex(u32 rec_lookup_identity);
		static u32 LookupEntryIndex(u32 rec_lookup_identity);
		static u32 HotDispatchCacheIndex(u32 rec_lookup_identity);
		static const u32* ResolveRawOpcodeSpan(u32 start_pc, u32 instruction_count,
			u32* ram_source_start);
		bool EnsureLookupDirectory();
		LookupPage* GetLookupPage(u32 start_pc, bool allocate);
		void RegisterBlockLookup(CachedBlock& block);
		void UnregisterBlockLookup(CachedBlock& block);
		void RegisterHotDispatchCache(CachedBlock& block);
		void UnregisterHotDispatchCache(CachedBlock& block);
		CachedBlock* FindHotDispatchCacheBlock(u32 start_pc);
		inline __attribute__((always_inline)) CachedBlock*
		FindHotDispatchCacheBlockInline(u32 start_pc);
		void ClearHotDispatchCache();
		void ReleaseLookupPages();
		void RegisterRamSource(CachedBlock& block);
		void UnregisterRamSource(const CachedBlock& block);
		void RegisterRamSource(InterpreterFallbackBlock& block);
		void UnregisterRamSource(const InterpreterFallbackBlock& block);
		void RegisterRamSourceChunks(u32 source_start, u32 source_size);
		void UnregisterRamSourceChunks(u32 source_start, u32 source_size);
		bool AnalyzePollCallWaitLoop(CachedBlock& block, u32 start_pc,
			u32 instruction_count);
		bool AnalyzeInlineRamPollWaitLoop(CachedBlock& block, u32 start_pc,
			u32 instruction_count);
		u32 InvalidateRamSourceRange(u32 start, u32 size);
		void ClearRamSourcePages();
		InterpreterFallbackBlock* FindInterpreterFallbackBlock(u32 start_pc) const;
		bool RegisterInterpreterFallbackBlock(u32 start_pc);
		void InvalidateInterpreterFallbackBlock(InterpreterFallbackBlock& block);
		s32 LastBlockRecordIndex(u32 rec_lookup_identity) const;
		bool RegisterBlockRecord(CachedBlock& block);
		void UnregisterBlockRecord(CachedBlock& block);
		void ClearBlockRecords();
		const SemanticBlockDescriptor* FindSemanticBlockDescriptor(
			u32 rec_lookup_identity) const;
		void RememberSemanticBlockDescriptor(u32 rec_lookup_identity,
			u32 ram_source_start, u32 instruction_count, bool logical_continuation);
		void ForgetSemanticBlockDescriptorsForLookupRange(u32 start, u32 size);
		void RegisterSemanticRamSource(const SemanticBlockDescriptor& descriptor);
		void UnregisterSemanticRamSource(const SemanticBlockDescriptor& descriptor);
		CachedBlock* FindRecordedBlockByStartPc(
			u32 start_pc, u32 instruction_count, bool match_instruction_count,
			bool isolate_cache_active, bool discovered_topology_only = false);
		void RememberFreeCacheEntry(CachedBlock& block);
		CachedBlock* TakeFreeCacheEntry();
		void RemoveFreeCacheEntry(CachedBlock& block);
		DirectLinkSlot* GetRecordedDirectLink(IncomingLinkRecord& record);
		s32 LastIncomingLinkIndex(u32 target_lookup_identity) const;
		void ClearIncomingLinks();
		void RegisterIncomingLinks(CachedBlock& block);
		void UnregisterIncomingLinks(CachedBlock& block);
		CachedBlock* FindLookupBlockByStartPc(u32 start_pc,
			bool isolate_cache_active);
		bool FindCachedBlock(u32 start_pc, u32 instruction_count, CachedBlock** block,
			bool* lookup_hit);
		CachedBlock* FindCachedBlockByStartPc(u32 start_pc,
			bool isolate_cache_active);
		CachedBlock* AllocateCacheEntry();
		void InvalidateCachedBlock(CachedBlock& block);
		bool ValidateCachedBlock(CachedBlock& block);
		static bool TryFastForwardTrustedWaitLoopAtPc(u32 start_pc);
		static inline __attribute__((always_inline)) u32
		ReadInlineRamPollValue(const CachedBlock& block);
		inline __attribute__((always_inline)) bool
		TryFastForwardPollCallWaitLoop(CachedBlock& block);
		inline __attribute__((always_inline)) bool
		TryFastForwardInlineRamPollWaitLoop(CachedBlock& block);
		template <bool Ps1Clock>
		inline __attribute__((always_inline)) bool
		TryFastForwardPollCallWaitLoopForClock(CachedBlock& block);
		template <bool Ps1Clock>
		inline __attribute__((always_inline)) bool
		TryFastForwardInlineRamPollWaitLoopForClock(CachedBlock& block);
		inline __attribute__((always_inline)) bool
		TryFastForwardCachedUnconditionalWaitLoop(CachedBlock& block);
		inline __attribute__((always_inline)) void
		FastForwardRetainedUnconditionalWaitLoop(CachedBlock& block);
		template <bool Ps1Clock>
		inline __attribute__((always_inline)) void
			FastForwardRetainedUnconditionalWaitLoopForClock(CachedBlock& block);
		template <bool Ps1Clock>
		inline __attribute__((always_inline)) void
			FastForwardRetainedUnconditionalNoLinkWaitLoopForClock(CachedBlock& block);
		inline __attribute__((always_inline)) bool
		TryFastForwardRetainedWaitLoop(CachedBlock& block, WaitResumeKind kind);
		__attribute__((noinline, cold)) bool
		TryFastForwardCachedWaitLoop(CachedBlock& block);
		template <int ClockMode>
		__attribute__((noinline, cold)) bool
		TryFastForwardCachedWaitLoopForClock(CachedBlock& block);
		bool EnsureCodeCache();
		void ReleaseCodeCache();
		u8* AllocateCodeSlice(size_t capacity, size_t* slice_offset);
		void CommitCodeSlice(size_t slice_offset, size_t code_size);
		void RewindCodeCache(size_t slice_offset);
		u32 ResetForCachePressure();
		bool CompileIntoCacheEntry(CachedBlock& block, u32 start_pc,
			u32 instruction_count,
			bool entry_effects_already_applied = false,
			bool allow_cache_pressure_retry = true,
			bool logical_continuation = false,
			bool discovered_topology = false);
		BlockScanStatus ScanProviderLogicalBlock(u32 start_pc,
			BlockScanResult* result);
		static size_t TotalCodeSize(const CachedBlock& block);
		static size_t TotalCodeCacheFootprint(const CachedBlock& block);
		static VitaA32::CodeBuffer* DirectLinkCode(CachedBlock& block,
			const DirectLinkSlot& link);
		bool ExecuteCompiledBlockInternal(u32 start_pc, u32 instruction_count,
			BlockExecutionResult* result,
			bool publish_details,
			bool entry_effects_already_applied,
			bool logical_continuation = false,
			bool discovered_topology = false,
			bool forced_continuation = false);
		void PublishExecutionDetails(const CachedBlock& block,
			BlockExecutionResult* result) const;
		bool RunValidatedBlock(CachedBlock& block, BlockExecutionResult* result,
			bool publish_details,
			bool forced_continuation = false);
		u32 RunProviderBlock(CachedBlock& block, u32 dispatch_flags,
			bool forced_continuation = false);
		inline __attribute__((always_inline)) u32
		RunProviderBlockInline(CachedBlock& block, u32 dispatch_flags,
			bool forced_continuation = false);
		__attribute__((noinline, cold)) CachedBlock*
		FindProviderBlockAtPcSlow(u32 start_pc, ProviderCompileResult* compile_result,
			u32* dispatch_flags);
		inline __attribute__((always_inline)) CachedBlock*
		FindProviderBlockAtPcInline(u32 start_pc,
			ProviderCompileResult* compile_result,
			u32* dispatch_flags);
		inline __attribute__((always_inline)) u32
		ExecuteProviderBlockAtPcInline(u32 start_pc,
			ProviderCompileResult* compile_result,
			bool forced_continuation = false);
		inline __attribute__((always_inline)) s32 ExecuteProviderTimesliceLoop();
		__attribute__((noinline, cold)) s32 ExecuteProviderTimesliceRemainder();
#if defined(__arm__)
		VITASX2_IOP_PRIVATE_VFP_ABI
		__attribute__((no_stack_protector)) __attribute__((noinline)) s32
		ExecuteProviderTimeslicePrivateBody(s32 ee_cycles) __asm__(
			"VitaIopA32ProviderTimesliceBody");
		VITASX2_IOP_PRIVATE_VFP_ABI
		__attribute__((no_stack_protector)) __attribute__((noinline)) s32
		ExecuteProviderSchedulerDirectResumePrivateBody(
			s32 ee_cycles,
			CachedBlock* block) __asm__("VitaIopA32ProviderSchedulerDirectResumeBody");
		VITASX2_IOP_PRIVATE_VFP_ABI
		__attribute__((no_stack_protector)) __attribute__((noinline)) s32
		ExecuteProviderSchedulerPredictedResumePrivateBody(
			s32 ee_cycles,
			CachedBlock* block) __asm__("VitaIopA32ProviderSchedulerPredictedResumeBody");
		VITASX2_IOP_PRIVATE_VFP_ABI __attribute__((noinline)) s32
		ExecuteProviderSchedulerDispatchCachedResumePrivateBody(
			s32 ee_cycles,
			CachedBlock*
				block) __asm__("VitaIopA32ProviderSchedulerDispatchCachedResumeBody");
		VITASX2_IOP_PRIVATE_VFP_ABI inline __attribute__((always_inline)) s32
		ExecuteProviderWaitResumePrivateBodyCore(s32 ee_cycles, CachedBlock* block,
			WaitResumeKind kind,
			bool kind_specific,
			bool clock_specific, bool ps1_clock,
				bool no_link_specific);
#if defined(VITASX2_QEMU_VALIDATION) || \
	defined(VITASX2_IOP_WAIT_RESUME_KIND_ENTRY_CONTROL)
		VITASX2_IOP_PRIVATE_VFP_ABI
		__attribute__((no_stack_protector, noinline)) s32
			ExecuteProviderWaitResumePrivateBody(
			s32 ee_cycles, CachedBlock* block,
			WaitResumeKind kind) __asm__("VitaIopA32ProviderWaitResumeBody");
#endif
#if defined(VITASX2_QEMU_VALIDATION) || \
	defined(VITASX2_IOP_WAIT_RESUME_CLOCK_ENTRY_CONTROL)
		VITASX2_IOP_PRIVATE_VFP_ABI
		__attribute__((no_stack_protector, noinline)) s32
			ExecuteProviderWaitResumeUnconditionalPrivateBody(
			s32 ee_cycles,
			CachedBlock* block) __asm__("VitaIopA32ProviderWaitResumeUnconditionalBody");
		VITASX2_IOP_PRIVATE_VFP_ABI
		__attribute__((no_stack_protector, noinline)) s32
			ExecuteProviderWaitResumePollPrivateBody(
			s32 ee_cycles,
			CachedBlock* block) __asm__("VitaIopA32ProviderWaitResumePollBody");
		VITASX2_IOP_PRIVATE_VFP_ABI
		__attribute__((no_stack_protector, noinline)) s32
			ExecuteProviderWaitResumeConditionalPrivateBody(
			s32 ee_cycles,
			CachedBlock* block) __asm__("VitaIopA32ProviderWaitResumeConditionalBody");
#endif
		VITASX2_IOP_PRIVATE_VFP_ABI
		__attribute__((no_stack_protector, noinline)) s32
			ExecuteProviderWaitResumeUnconditionalNormalPrivateBody(
			s32 ee_cycles,
			CachedBlock*
				block) __asm__("VitaIopA32ProviderWaitResumeUnconditionalNormalBody");
		VITASX2_IOP_PRIVATE_VFP_ABI
		__attribute__((no_stack_protector, noinline)) s32
			ExecuteProviderWaitResumeUnconditionalPs1PrivateBody(
			s32 ee_cycles,
			CachedBlock* block) __asm__("VitaIopA32ProviderWaitResumeUnconditionalPs1Body");
		VITASX2_IOP_PRIVATE_VFP_ABI
		__attribute__((no_stack_protector, noinline)) s32
			ExecuteProviderWaitResumeUnconditionalNoLinkNormalPrivateBody(
			s32 ee_cycles,
			CachedBlock* block) __asm__("VitaIopA32ProviderWaitResumeUnconditionalNoL"
										"inkNormalBody");
		VITASX2_IOP_PRIVATE_VFP_ABI
		__attribute__((no_stack_protector, noinline)) s32
			ExecuteProviderWaitResumeUnconditionalNoLinkPs1PrivateBody(
			s32 ee_cycles, CachedBlock* block) __asm__("VitaIopA32ProviderWaitResumeU"
													   "nconditionalNoLinkPs1Body");
		VITASX2_IOP_PRIVATE_VFP_ABI
		__attribute__((no_stack_protector, noinline)) s32
			ExecuteProviderWaitResumePollNormalPrivateBody(
			s32 ee_cycles,
			CachedBlock* block) __asm__("VitaIopA32ProviderWaitResumePollNormalBody");
		VITASX2_IOP_PRIVATE_VFP_ABI
		__attribute__((no_stack_protector, noinline)) s32
			ExecuteProviderWaitResumePollPs1PrivateBody(
			s32 ee_cycles,
			CachedBlock* block) __asm__("VitaIopA32ProviderWaitResumePollPs1Body");
		VITASX2_IOP_PRIVATE_VFP_ABI
		__attribute__((no_stack_protector, noinline)) s32
			ExecuteProviderWaitResumeConditionalNormalPrivateBody(
			s32 ee_cycles,
			CachedBlock* block) __asm__("VitaIopA32ProviderWaitResumeConditionalNormalBody");
		VITASX2_IOP_PRIVATE_VFP_ABI
		__attribute__((no_stack_protector, noinline)) s32
			ExecuteProviderWaitResumeConditionalPs1PrivateBody(
			s32 ee_cycles,
			CachedBlock* block) __asm__("VitaIopA32ProviderWaitResumeConditionalPs1Body");
#endif
		void SetWaitResumeBlock(CachedBlock* block);
		void ClearWaitResumeBlock();
		inline __attribute__((always_inline)) void
		SetSchedulerDirectResumeBlock(CachedBlock* block);
		void ClearSchedulerDirectResume();
		inline __attribute__((always_inline)) void
		SetSchedulerPredictedResumeBlock(CachedBlock* block);
		void ClearSchedulerPredictedResume();
		inline __attribute__((always_inline)) CachedBlock*
		FindSchedulerDirectResumeBlock(u32* dispatch_flags);
		const void* LinkedEntryPoint(const CachedBlock& block) const;
		const void* ProviderEntryPoint(const CachedBlock& block) const;
		bool PatchDirectLink(CachedBlock& block, DirectLinkSlot& link,
			CachedBlock* target);
		void PatchIncomingLinks(CachedBlock& target);
		void UnlinkIncomingLinks(u32 target_link_identity,
			int isolate_cache_mode = -1);
		void RelinkDirectLinks();

		std::vector<std::unique_ptr<CachedBlock>> m_cache;
		std::vector<std::unique_ptr<InterpreterFallbackBlock>>
			m_interpreter_fallback_blocks;
		CachedBlock* m_free_cache_head = nullptr;
		std::vector<BlockRecord> m_block_records;
		std::vector<SemanticBlockDescriptor> m_semantic_block_descriptors;
		std::vector<IncomingLinkRecord> m_incoming_links;
		// Keep the per-dispatch selector before the large inline source/cache
		// banks so Cortex-A9 can load it with one immediate-offset LDR.
		bool m_active_isolate_cache_mode = false;
		bool m_force_logical_continuation = false;
		std::array<std::vector<RamSourceRecord>, RAM_SOURCE_PAGE_COUNT>
			m_ram_source_pages;
		std::array<u32, RAM_SOURCE_PAGE_COUNT> m_ram_source_page_live_counts{};
		std::array<u8, RAM_SOURCE_PAGE_COUNT> m_ram_source_page_live_flags{};
		std::unique_ptr<RamSourceChunkOwnership> m_ram_source_chunks;
		LookupPage** m_lookup_pages = nullptr;
		std::array<std::array<std::array<HotDispatchCacheEntry,
								  HOT_DISPATCH_CACHE_WAY_COUNT>,
					   HOT_DISPATCH_CACHE_SET_COUNT>,
			2>
			m_hot_dispatch_cache{};
#if defined(VITASX2_QEMU_VALIDATION)
		std::array<std::array<std::array<HotDispatchCacheEntry,
								  HOT_DISPATCH_CACHE_WAY_COUNT>,
					   HOT_DISPATCH_CACHE_CONTROL_SET_COUNT>,
			2>
			m_hot_dispatch_cache_64_set_control{};
#endif
		CachedBlock* m_wait_resume_block = nullptr;
		struct SchedulerDirectResumeEventContext
		{
			struct DispatchCacheEntry
			{
				CachedBlock* block = nullptr;
				u32 guest_pc = UINT32_MAX;
			};
			BlockExecutor* executor = nullptr;
			CachedBlock* block = nullptr;
			CachedBlock* predicted_block = nullptr;
			u32 predicted_pc = UINT32_MAX;
			CachedBlock* predicted_block_second = nullptr;
			u32 predicted_pc_second = UINT32_MAX;
			std::array<DispatchCacheEntry, 64> dispatch_cache{};
		} m_scheduler_direct_resume_event_context;
#if defined(VITASX2_QEMU_VALIDATION)
		std::array<CachedBlock*, 4> m_scheduler_prediction_shadow{};
#endif
		struct WaitResumeEventContext
		{
			BlockExecutor* executor = nullptr;
			CachedBlock* block = nullptr;
			WaitResumeKind kind = WaitResumeKind::Invalid;
		} m_wait_resume_event_context;
		bool m_owns_ee_event_entry = false;
		u32 m_next_source_serial = 1;
		u8* m_code_cache = nullptr;
		size_t m_code_cache_capacity = 0;
		size_t m_code_cache_used = 0;
		u32 m_code_cache_resets = 0;
#if defined(VITASX2_QEMU_VALIDATION)
		u64 m_hot_dispatch_cache_hits = 0;
		u64 m_hot_dispatch_cache_misses = 0;
		u64 m_hot_dispatch_cache_way_probes = 0;
		u64 m_hot_dispatch_cache_64_set_hits = 0;
		u64 m_hot_dispatch_cache_64_set_misses = 0;
		u64 m_hot_dispatch_cache_64_set_way_probes = 0;
		u64 m_scheduler_direct_resume_candidates = 0;
		u64 m_scheduler_direct_resume_installs = 0;
		u64 m_scheduler_direct_resume_attempts = 0;
		u64 m_scheduler_direct_resume_hits = 0;
		u64 m_scheduler_direct_resume_misses = 0;
		u64 m_scheduler_direct_resume_no_target = 0;
		u64 m_scheduler_direct_resume_target_mismatch = 0;
		u64 m_scheduler_direct_event_entries = 0;
		u64 m_scheduler_direct_event_forwards = 0;
		u64 m_scheduler_direct_event_fallbacks = 0;
		u64 m_scheduler_direct_event_remainders = 0;
		u64 m_scheduler_direct_event_installs = 0;
		u64 m_scheduler_direct_event_clears = 0;
		u64 m_scheduler_prediction_attempts = 0;
		u64 m_scheduler_prediction_hits = 0;
		u64 m_scheduler_prediction_misses = 0;
		u64 m_scheduler_prediction_two_way_hits = 0;
		u64 m_scheduler_prediction_four_way_hits = 0;
		u64 m_scheduler_prediction_forwards = 0;
		u64 m_scheduler_prediction_fallbacks = 0;
		u64 m_scheduler_prediction_remainders = 0;
		u64 m_scheduler_dispatch_cache_attempts = 0;
		u64 m_scheduler_dispatch_cache_hits = 0;
		u64 m_scheduler_dispatch_cache_misses = 0;
		u64 m_scheduler_dispatch_cache_forwards = 0;
		u64 m_scheduler_dispatch_cache_fallbacks = 0;
		u64 m_scheduler_dispatch_cache_remainders = 0;
		u64 m_scheduler_dispatch_cache_installs = 0;
		u64 m_hot_dispatch_trusted_raw_hits = 0;
		u64 m_hot_dispatch_owned_hits = 0;
		std::unordered_map<u32, u64> m_hot_dispatch_hit_pc_profile;
		struct InterpreterFallbackProfileEntry
		{
			u32 start_pc = 0;
			u32 owner_pc = 0;
			u32 owner_opcode = 0;
			u32 instruction_count = 0;
			u64 source_hash = 0;
			u64 hits = 0;
		};
		std::unordered_map<u64, InterpreterFallbackProfileEntry>
			m_interpreter_fallback_pc_profile;
		u64 m_wait_resume_cache_attempts = 0;
		u64 m_wait_resume_cache_hits = 0;
		u64 m_wait_resume_cache_misses = 0;
		u64 m_wait_resume_event_entries = 0;
		u64 m_wait_resume_event_forwards = 0;
		u64 m_wait_resume_event_fallbacks = 0;
		u64 m_wait_resume_event_installs = 0;
		u64 m_wait_resume_event_clears = 0;
		u64 m_wait_resume_first_entry_owned = 0;
		u64 m_wait_resume_post_event_identity_checks = 0;
		u64 m_wait_resume_kind_specific_entries = 0;
		u64 m_wait_resume_kind_specific_unconditional_forwards = 0;
		u64 m_wait_resume_kind_specific_poll_forwards = 0;
		u64 m_wait_resume_kind_specific_conditional_forwards = 0;
		u64 m_wait_resume_clock_specific_entries = 0;
		u64 m_wait_resume_clock_specific_forwards = 0;
		u64 m_wait_resume_no_link_specific_entries = 0;
		u64 m_wait_resume_no_link_specific_forwards = 0;
		u64 m_wait_resume_descriptor_forwards = 0;
		u64 m_wait_resume_unconditional_forwards = 0;
		u64 m_wait_resume_poll_forwards = 0;
		u64 m_wait_resume_conditional_forwards = 0;
		u64 m_direct_budget_exit_provider_entries = 0;
		u64 m_constant_cycle_budget_provider_entries = 0;
		u64 m_validation_calls = 0;
		u64 m_validation_words = 0;
		u64 m_raw_validation_calls = 0;
		u64 m_raw_validation_words = 0;
		u64 m_translated_validation_words = 0;
		u64 m_wait_loop_configuration_checks = 0;
		u64 m_trusted_source_hits = 0;
		u64 m_trusted_source_audit_words = 0;
		u64 m_trusted_source_audit_failures = 0;
		u64 m_ram_invalidation_calls = 0;
		u64 m_ram_invalidation_record_visits = 0;
		u64 m_clock_mode_check_instructions_removed = 0;
		u64 m_saved_register_stack_words_removed = 0;
		u64 m_saved_register_frame_instructions_added = 0;
		u64 m_saved_register_frame_instructions_removed = 0;
		u64 m_batched_cycle_instructions_removed = 0;
		u64 m_batched_cycle_stack_words_removed = 0;
		u64 m_expanded_cycle_batching_provider_entries = 0;
		u64 m_pinned_gpr_memory_ops_saved = 0;
		u64 m_pinned_branch_operand_moves_removed = 0;
		u64 m_condition_code_branch_instructions_removed = 0;
		u64 m_producer_branch_compare_instructions_removed = 0;
		u64 m_fused_ram_guard_instructions_removed = 0;
		u64 m_source_page_guard_instructions_removed = 0;
		u64 m_source_page_literal_instructions_removed = 0;
		u64 m_isolate_cache_guard_instructions_removed = 0;
		u64 m_private_dispatcher_calls = 0;
		u64 m_private_dispatcher_provider_entries = 0;
		u64 m_private_dispatcher_wait_forwards = 0;
		u64 m_private_dispatcher_generated_entries = 0;
		u64 m_private_dispatcher_fallbacks = 0;
		u64 m_private_dispatcher_inlined_hot_entries = 0;
		u64 m_private_frame_provider_entries = 0;
		u64 m_private_frame_stack_words_removed = 0;
		u64 m_private_frame_zero_scratch_entries = 0;
		u64 m_cached_wait_descriptor_checks = 0;
		u64 m_cached_wait_descriptor_forwards = 0;
		u64 m_cached_wait_descriptor_opcode_reads_removed = 0;
		u64 m_cached_wait_descriptor_unconditional_checks = 0;
		u64 m_compiled_ps1_bios_gate_blocks = 0;
		u64 m_compiled_ps1_bios_gate_entries = 0;
		u64 m_dispatcher_ps1_bios_gate_checks_removed = 0;
#endif

#if defined(VITASX2_PORTABLE_REPLAY_VALIDATION) || \
	defined(VITASX2_PRODUCT_BOOT_VALIDATION)
		PortableValidationStats m_portable_validation_stats{};
#endif
		bool m_direct_linking_enabled = true;
	};

	inline constexpr u32 BlockExecutor::SchedulerPredictionSecondOffset()
	{
		return static_cast<u32>(
			offsetof(BlockExecutor, m_scheduler_direct_resume_event_context) +
			offsetof(SchedulerDirectResumeEventContext, predicted_block_second));
	}

	inline constexpr u32 BlockExecutor::SchedulerDispatchCacheOffset()
	{
		return static_cast<u32>(
			offsetof(BlockExecutor, m_scheduler_direct_resume_event_context) +
			offsetof(SchedulerDirectResumeEventContext, dispatch_cache));
	}
} // namespace VitaIOP
