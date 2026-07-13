// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"
#include "pcsx2/HostMemoryMap.h"
#include "pcsx2/vita/A32Emitter.h"
#include "pcsx2/vita/VitaEeBlockCompiler.h"
#if defined(VITASX2_QEMU_VALIDATION)
#include "pcsx2/vita/VitaCore.h"
#endif

#include <array>
#include <cstddef>
#include <memory>
#include <vector>
#if defined(VITASX2_QEMU_VALIDATION)
#include <unordered_map>
#endif

namespace VitaEE
{
	enum class BlockExecutionPath : u8
	{
		Compiled,
		InterpreterStep,
	};

	enum class BlockExitKind : u32
	{
		Direct = 0xd1,
		Event = 0xe7,
		InterpreterStep = 0x1e,
	};

	enum class BlockScanStop : u8
	{
		UnsupportedOpcode,
		PageBoundary,
		DebugBoundary,
		OpcodeBoundary,
		Branch,
		MaxInstructions,
		AddressWrap,
	};

	struct BlockScanResult
	{
		u32 start_pc = 0;
		u32 instruction_count = 0;
		u32 stop_pc = 0;
		BlockScanStop stop = BlockScanStop::UnsupportedOpcode;
	};

	struct BlockExecutionResult
	{
		BlockExecutionPath path = BlockExecutionPath::Compiled;
		BlockExitKind exit = BlockExitKind::Direct;
		u32 exit_value = 0;
		u32 instruction_count = 0;
		// The scanner may expose a larger straight-line region than fits the
		// bounded A32 code slice.  PCSX2's split-block path makes the emitted
		// prefix a normal block boundary; retain the original region size so
		// validation can prove that the split was adaptive rather than fallback.
		u32 source_instruction_count = 0;
		u32 scaled_cycles = 0;
		size_t code_size = 0;
		u32 block_records = 0;
		u32 link_records = 0;
		u32 cache_slots = 0;
		u32 code_cache_resets = 0;
		size_t code_cache_used = 0;
		size_t code_cache_capacity = 0;
		bool cache_hit = false;
		bool lookup_hit = false;
		bool fast_dispatch_hit = false;
#if defined(VITASX2_QEMU_VALIDATION)
		u32 generated_frame_pushes = 0;
		u32 generated_frame_pops = 0;
		u32 dispatcher_frame_pushes = 0;
		u32 dispatcher_frame_pops = 0;
		u32 resident_self_links = 0;
		u32 resident_self_link_entry_instructions = 0;
		u32 resident_self_link_entry_loads = 0;
		u32 compatible_gpr_links = 0;
		u32 compatible_gpr_link_entry_instructions = 0;
		u32 compatible_gpr_link_entry_loads = 0;
		u32 compatible_gpr_words_carried = 0;
		u32 compatible_gpr_dirty_words_carried = 0;
		u32 compatible_gpr_chain_blocks = 0;
		u32 compatible_scheduler_links = 0;
		u32 compatible_vtlb_pointer_links = 0;
		u32 embedded_compatible_continuations = 0;
#endif
	};

	class BlockExecutor
	{
	public:
		using PersistentBoundaryCallback = bool (*)(void* userdata,
			const BlockExecutionResult& completed_chain);

		static constexpr u32 MAX_STRAIGHT_LINE_BLOCK_INSTRUCTIONS = 64;

		BlockExecutor();
		~BlockExecutor();

		u32 Shutdown();
		u32 Reset();
		u32 InvalidateRange(u32 start_pc, u32 instruction_count);
		void SetDirectLinkingEnabled(bool enabled);
		void SetPersistentDispatchEnabled(bool enabled);
#if defined(VITASX2_QEMU_VALIDATION)
		void SetCompatibleGprDirtyCarryEnabled(bool enabled);
		void SetCompatibleSchedulerCarryEnabled(bool enabled);
		void SetCompatibleVtlbPointerCarryEnabled(bool enabled);
		void SetCompatibleVtlbHostReclaimEnabled(bool enabled);
		void SetCompatiblePredicateCarryEnabled(bool enabled);
		void SetCompatibleLikelyTakenSuffixEnabled(bool enabled);
		void SetCompatiblePredicateEntryVariantEnabled(bool enabled);
		void SetEmbeddedCompatibleContinuationEnabled(bool enabled);
		void SetFusedDirectEventLinkEnabled(bool enabled);
		void SetCombinedCompatibleTakenEventEnabled(bool enabled);
		void SetCompatibleVtlbWriteGuardHoistEnabled(bool enabled);
		void SetCompatibleVtlbReadGuardHoistEnabled(bool enabled);
		void SetSingleBlockGprLinkEnabled(bool enabled);
		void SetReciprocalJumpGprLinkEnabled(bool enabled);
		void SetThreeBlockGprLinkEnabled(bool enabled);
		void SetVtlbLinkedEntryPcPublicationEnabled(bool enabled);
		void SetDirectLinkRejectionProfileEnabled(bool enabled);
		void ResetDirectLinkRejectionProfile();
		VitaA32EeLinkRejectionProfile GetDirectLinkRejectionProfile() const;
#endif
		static bool ScanStraightLineBlock(u32 start_pc, u32 max_instruction_count, BlockScanResult* result);
		bool ExecuteCompiledBlock(u32 start_pc, u32 instruction_count,
			bool run_event_test_on_event_exit, BlockExecutionResult* result,
			bool allow_code_budget_split = false);
		bool ExecuteCompiledBlockAtPc(u32 start_pc, bool run_event_test_on_event_exit,
			BlockExecutionResult* result);
		bool ExecutePersistentAtPc(u32 start_pc, bool run_event_test_on_event_exit,
			PersistentBoundaryCallback boundary_callback, void* callback_userdata,
			BlockExecutionResult* result);
		bool ExecuteStraightLineBlockOrInterpreterStep(u32 start_pc, u32 instruction_count,
			bool run_event_test_on_event_exit, BlockExecutionResult* result);

	private:
		// PCSX2 owner: x86/BaseblockEx.h::BaseBlocks() starts at 0x4000
		// BASEBLOCKEX records and grows from there. Vita keeps the same
		// game-scale order while bounding metadata for the smaller memory budget.
		static constexpr size_t INITIAL_CACHE_CAPACITY = 512;
		static constexpr size_t MAX_CACHE_CAPACITY = 0x4000;
		static constexpr size_t STRAIGHT_LINE_BLOCK_CODE_CAPACITY = 4096;
		// PCSX2 x86/ix86-32/iR5900.cpp::recRecompile() uses its split-block
		// continuation when a block must remain manageable.  A32 first grows the
		// temporary slice for ordinary variance, then turns an over-budget region
		// into two normal directly-linkable blocks instead of admitting a single
		// pathological body into the Cortex-A9 instruction cache.
		static constexpr size_t MAX_STRAIGHT_LINE_BLOCK_CODE_CAPACITY = 32 * 1024;
		static constexpr size_t EE_CODE_CACHE_CAPACITY = HostMemoryMap::EErecSize;
		static constexpr size_t CODE_CACHE_ALIGNMENT = 32;
		static constexpr size_t DIRECT_LINK_SLOT_COUNT = 2;
		static constexpr size_t MAX_INCOMING_LINKS = MAX_CACHE_CAPACITY * DIRECT_LINK_SLOT_COUNT;
		static constexpr u32 LOOKUP_DIRECTORY_ENTRY_COUNT = 0x10000;
		static constexpr u32 LOOKUP_PAGE_ENTRY_COUNT = 0x4000;

		struct CachedBlock
		{
			VitaA32::CodeBuffer code;
			std::array<u32, MAX_STRAIGHT_LINE_BLOCK_INSTRUCTIONS> opcodes{};
			u32 start_pc = 0;
			u32 instruction_count = 0;
			u32 source_instruction_count = 0;
			u32 dependency_start_pc = 0;
			u32 dependency_instruction_count = 0;
			u32 dependency_charged_cycles_before = 0;
			u32 scaled_cycles = 0;
			s8 ee_cycle_rate = 0;
			u8 cp0_config_cycle_shift = 0;
			size_t linked_entry_offset = 0;
			size_t resident_self_link_entry_offset = static_cast<size_t>(-1);
			u8 resident_self_link_entry_loads = 0;
			GprLinkSignature gpr_link_signature{};
			size_t compatible_link_entry_offset = static_cast<size_t>(-1);
			CompatibleVtlbFastEntryOffsets compatible_vtlb_fast_entries{};
			u8 compatible_link_entry_loads = 0;
			DirectLinkSlots direct_links{};
			bool valid = false;
			bool queued_free = false;
		};

		struct PersistentRunContext
		{
			BlockExecutor* executor = nullptr;
			CachedBlock* current_block = nullptr;
			BlockExecutionResult current_result{};
			BlockExecutionResult* final_result = nullptr;
			PersistentBoundaryCallback boundary_callback = nullptr;
			void* callback_userdata = nullptr;
			bool run_event_test_on_event_exit = true;
			bool failed = false;
		};

		struct LookupPage
		{
			std::array<CachedBlock*, LOOKUP_PAGE_ENTRY_COUNT> blocks{};
		};

		struct GeneratedLookupPage
		{
			std::array<const void*, LOOKUP_PAGE_ENTRY_COUNT> entry_points{};
		};

		struct IncomingLinkRecord
		{
			CachedBlock* source = nullptr;
			u32 target_pc = 0;
			u8 slot_index = 0;
		};

		struct BlockRecord
		{
			CachedBlock* block = nullptr;
			const void* entry_point = nullptr;
			u32 start_pc = 0;
			u32 instruction_count = 0;
			size_t code_size = 0;
		};

		static u32 LookupPageIndex(u32 start_pc);
		static u32 LookupEntryIndex(u32 start_pc);
		bool EnsureLookupDirectory();
		LookupPage* GetLookupPage(u32 start_pc, bool allocate);
		bool EnsureGeneratedLookupDirectory();
		GeneratedLookupPage* GetGeneratedLookupPage(u32 start_pc, bool allocate);
		void RegisterBlockLookup(CachedBlock& block);
		void UnregisterBlockLookup(CachedBlock& block);
		void ReleaseLookupPages();
		void ReleaseGeneratedLookupPages();
		s32 LastBlockRecordIndex(u32 pc) const;
		bool RegisterBlockRecord(CachedBlock& block);
		void UnregisterBlockRecord(CachedBlock& block);
		void ClearBlockRecords();
		CachedBlock* FindRecordedBlockByStartPc(
			u32 start_pc, u32 instruction_count, bool match_instruction_count, bool validate_source_words = true);
		void RememberFreeCacheEntry(CachedBlock& block);
		CachedBlock* TakeFreeCacheEntry();
		void InvalidateCachedBlock(CachedBlock& block);
		DirectLinkSlot* GetRecordedDirectLink(IncomingLinkRecord& record);
		s32 LastIncomingLinkIndex(u32 target_pc) const;
		void ClearIncomingLinks();
		void RegisterIncomingLink(CachedBlock& block, u8 slot_index, const DirectLinkSlot& link);
		void RegisterIncomingLinks(CachedBlock& block);
		void UnregisterIncomingLinks(CachedBlock& block);
		bool ValidateCachedBlock(CachedBlock& block, bool validate_source_words = true);
		CachedBlock* FindLookupBlockByStartPc(u32 start_pc);
		bool FindCachedBlock(u32 start_pc, u32 instruction_count, CachedBlock** block,
			bool* lookup_hit, bool match_code_budget_source = false);
		CachedBlock* FindCachedBlockByStartPc(u32 start_pc, bool validate_source_words = true);
		void ResolveAdjacentSplitDependency(u32 start_pc, u32 instruction_count,
			u32* dependency_start_pc, u32* dependency_instruction_count,
			u32* dependency_charged_cycles_before) const;
		CachedBlock* AllocateCacheEntry();
		bool EnsureCodeCache();
		void ReleaseCodeCache();
		u8* AllocateCodeSlice(size_t capacity, size_t* slice_offset);
		void CommitCodeSlice(size_t slice_offset, size_t code_size);
		void RewindCodeCache(size_t slice_offset);
		u32 ResetForCachePressure();
		bool CompileIntoCacheEntry(CachedBlock& block, u32 start_pc, u32 instruction_count,
			u32* scaled_cycles, bool allow_code_budget_split = false,
			u32 dependency_start_pc = 0, u32 dependency_instruction_count = 0,
			u32 dependency_charged_cycles_before = 0);
		bool AnalyzeGprLinkSignature(u32 start_pc, u32 instruction_count,
			GprLinkSignature* signature) const;
		bool PrepareCompiledBlockAtPc(u32 start_pc, CachedBlock** block, BlockExecutionResult* result);
		bool RunCachedBlock(CachedBlock& block, bool run_event_test_on_event_exit, BlockExecutionResult* result);
		bool EnsurePersistentDispatcher();
		static const void* PersistentDispatchThunk(u32 exit_value, void* userdata);
		const void* LinkedEntryPoint(const CachedBlock& block) const;
		const void* ResidentSelfLinkEntryPoint(const CachedBlock& block) const;
		const void* CompatibleLinkEntryPoint(const CachedBlock& block) const;
		const void* CompatibleVtlbFastEntryPoint(const CachedBlock& block,
			CompatibleVtlbGuardKind kind) const;
		bool PatchDirectLink(CachedBlock& block, DirectLinkSlot& link, CachedBlock* target);
		void PatchIncomingLinks(CachedBlock& target);
		void UnlinkIncomingLinks(u32 target_pc);
		void RelinkDirectLinks();
#if defined(VITASX2_QEMU_VALIDATION)
		void RecordPersistentExit(BlockExitKind exit);
#endif

		std::vector<std::unique_ptr<CachedBlock>> m_cache;
		std::vector<CachedBlock*> m_free_cache_entries;
		std::vector<BlockRecord> m_block_records;
		std::vector<IncomingLinkRecord> m_incoming_links;
		LookupPage** m_lookup_pages = nullptr;
		GeneratedLookupPage** m_generated_lookup_pages = nullptr;
		GeneratedLookupPage** m_active_generated_lookup_pages = nullptr;
		VitaA32::CodeBuffer m_persistent_dispatch_code;
		const void* m_persistent_dispatch_entry = nullptr;
		const void* m_persistent_direct_exit = nullptr;
		const void* m_persistent_event_exit = nullptr;
		u8* m_code_cache = nullptr;
		size_t m_code_cache_capacity = 0;
		size_t m_code_cache_used = 0;
		u32 m_code_cache_resets = 0;
		bool m_direct_linking_enabled = true;
		bool m_persistent_dispatch_enabled = false;
#if defined(VITASX2_QEMU_VALIDATION)
		bool m_compatible_gpr_dirty_carry_enabled = true;
		bool m_compatible_scheduler_carry_enabled = true;
		bool m_compatible_vtlb_pointer_carry_enabled = true;
		bool m_compatible_vtlb_host_reclaim_enabled = true;
		bool m_compatible_predicate_carry_enabled = true;
		bool m_compatible_likely_taken_suffix_enabled = true;
		bool m_compatible_predicate_entry_variant_enabled = true;
		bool m_embedded_compatible_continuation_enabled = true;
		bool m_fused_direct_event_link_enabled = true;
		bool m_combined_compatible_taken_event_enabled = true;
		bool m_compatible_vtlb_write_guard_hoist_enabled = true;
		bool m_compatible_vtlb_read_guard_hoist_enabled = true;
		bool m_single_block_gpr_link_enabled = true;
		bool m_reciprocal_jump_gpr_link_enabled = true;
		bool m_three_block_gpr_link_enabled = true;
		bool m_vtlb_linked_entry_pc_publication_enabled = false;
		bool m_direct_link_rejection_profile_enabled = false;
		VitaA32EeLinkRejectionProfile m_direct_link_rejection_profile{};
		std::unordered_map<u64,
			std::array<u64, static_cast<u32>(VitaA32EeLinkRejectionKind::Count)>>
			m_direct_link_rejection_edges;
#endif
	};
} // namespace VitaEE
