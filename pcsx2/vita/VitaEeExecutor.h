// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"
#include "pcsx2/HostMemoryMap.h"
#include "pcsx2/MemoryTypes.h"
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
		BranchTargetBoundary,
		ExistingBlockBoundary,
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
		// bounded A32 code slice. Retain the original PCSX2 logical region size so
		// validation can prove that an A32 physical fragment did not become an
		// extra scheduler boundary or an interpreter fallback.
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
		// Set from the token emitted by the generated block which actually
		// returned to the dispatcher. Unlike the prepared-entry telemetry below,
		// this remains authoritative after one or more patched direct links.
		bool scheduler_test_elided = false;
#if defined(VITASX2_QEMU_VALIDATION)
		u32 concatenated_short_blocks = 0;
		u32 concatenated_short_scheduler_tests_elided = 0;
		u32 concatenated_short_hot_instructions_elided = 0;
		u32 code_budget_continuation_blocks = 0;
		u32 code_budget_continuation_scheduler_tests_elided = 0;
		u32 code_budget_continuation_hot_instructions_elided = 0;
		u32 generated_frame_pushes = 0;
		u32 generated_frame_pops = 0;
		u32 dispatcher_frame_pushes = 0;
		u32 dispatcher_frame_pops = 0;
		u32 resident_self_links = 0;
		u32 resident_self_link_entry_instructions = 0;
		u32 resident_self_link_entry_loads = 0;
		u32 compatible_gpr_links = 0;
		u32 post_writeback_canonical_links = 0;
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

		// PCSX2 recRecompile() discovers through the next branch, existing
		// BaseBlock, debugger seam, or 4 KiB source-page boundary. A branch in the
		// final page word still owns its delay slot on the following page, hence
		// 1024 page words plus one atomic follower. Host-code budget splitting is a
		// separate concern below; imposing a smaller discovery ceiling changes
		// fixed-point cycle rounding and therefore Count/event timing.
		static constexpr u32 MAX_STRAIGHT_LINE_BLOCK_INSTRUCTIONS = 1025;
		static_assert(MAX_STRAIGHT_LINE_BLOCK_INSTRUCTIONS ==
			BlockCompiler::MAX_COMPILE_INSTRUCTIONS);
		// EE source ownership is tracked at the same 4 KiB granularity as the
		// vTLB and PCSX2's mmap code-page protection. Records and reference
		// counts cover retail EE RAM only. Generated direct-store guards can see
		// any non-handler pointer in the compact ARM32 HostMemoryMap arena, so
		// their byte lookup table spans that complete arena; entries outside RAM
		// remain zero throughout the executor's lifetime.
		static constexpr u32 RAM_SOURCE_PAGE_SHIFT = 12;
		static constexpr u32 RAM_SOURCE_PAGE_COUNT =
			Ps2MemSize::MainRam >> RAM_SOURCE_PAGE_SHIFT;
		static constexpr u32 RAM_WRITE_GUARD_PAGE_COUNT =
			HostMemoryMap::MainSize >> RAM_SOURCE_PAGE_SHIFT;
		static_assert((HostMemoryMap::MainSize &
			((1u << RAM_SOURCE_PAGE_SHIFT) - 1)) == 0);
		static_assert(RAM_SOURCE_PAGE_COUNT <= RAM_WRITE_GUARD_PAGE_COUNT);

		BlockExecutor();
		~BlockExecutor();

		u32 Shutdown();
		u32 Reset();
		u32 InvalidateRange(u32 start_pc, u32 instruction_count);
		// backing_start is a byte offset into eeMem->Main, not a guest virtual
		// address.  This is deliberately separate from InvalidateRange(): several
		// physical/KSEG/TLB aliases can own the same compiled source bytes.
		u32 InvalidateRamSourceRange(u32 backing_start, u32 size);
		const u8* RamSourcePageLiveFlags() const
		{
			return m_ram_source_page_live_flags.data();
		}
		void SetDirectLinkingEnabled(bool enabled);
		// A helper can request a whole-cache reset while generated code is still
		// executing. Stop dynamic lookup immediately without patching the current
		// code page; Reset() and the next directory allocation republish it.
		void SuspendGeneratedLookupUntilReset();
		void SetPersistentDispatchEnabled(bool enabled);
		// A persistent block normally enters its next block without returning to
		// the provider. PCSX2's x86 recRecompile() embeds a few lifecycle hooks at
		// specific block entries; A32 keeps those entries on the private
		// dispatcher boundary so the provider can run the same hooks before the
		// target block executes, without disabling linking for the rest of boot.
		static u32 CanonicalizeRamBackedPc(u32 pc);
		bool SetPersistentDispatchBarrier(u32 pc, bool enabled);
		// Reconcile the complete set of lifecycle owners as one unique union.
		// Required barriers are installed before stale barriers are removed, so a
		// failed code patch leaves dispatch conservatively on provider boundaries.
		bool SetPersistentDispatchBarriers(const u32* pcs, size_t count);
		void ClearPersistentDispatchBarriers();
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
		static constexpr u32 INVALID_RAM_SOURCE = UINT32_MAX;
		// A maximum-size aligned EE block covers at most the tail of one vTLB
		// page and the complete following page. Keep one spare fragment so this
		// invariant remains robust if the scanner's atomic follower grows.
		static constexpr u32 MAX_RAM_SOURCE_FRAGMENTS = 3;

		struct RamSourceFragment
		{
			u32 start = INVALID_RAM_SOURCE;
			u32 size = 0;
		};

		struct CachedBlock
		{
			CachedBlock* next_free = nullptr;
			VitaA32::CodeBuffer code;
			// Match recRAMCopy without charging every cached block for a worst-case
			// page-sized inline array. The nothrow allocation is released with the
			// BaseBlock, so cold long blocks cannot leave page-sized capacity behind
			// in every recycled Vita cache slot.
			std::unique_ptr<u32[]> opcodes;
			u32 start_pc = 0;
			u32 instruction_count = 0;
			u32 source_instruction_count = 0;
			u32 dependency_start_pc = 0;
			u32 dependency_instruction_count = 0;
			u32 dependency_charged_cycles_before = 0;
			std::array<RamSourceFragment, MAX_RAM_SOURCE_FRAGMENTS>
				ram_source_fragments{};
			u32 source_serial = 0;
			u8 ram_source_fragment_count = 0;
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
			DirectContinuationKind direct_continuation_kind =
				DirectContinuationKind::SchedulerTestedTail;
			bool discovered_topology = false;
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

		struct RamSourceRecord
		{
			CachedBlock* block = nullptr;
			u32 serial = 0;
		};

		static u32 LookupPageIndex(u32 start_pc);
		static u32 LookupEntryIndex(u32 start_pc);
		bool EnsureLookupDirectory(bool discovered_topology);
		LookupPage* GetLookupPage(u32 start_pc, bool allocate,
			bool discovered_topology);
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
		bool CaptureRamSourceFragments(CachedBlock& block);
		void RegisterRamSource(CachedBlock& block);
		void UnregisterRamSource(const CachedBlock& block);
		void ClearRamSourcePages();
		static void InvalidateRamSourceRangeThunk(
			void* context, u32 backing_start, u32 size);
		CachedBlock* FindRecordedBlockByStartPc(u32 start_pc,
			u32 instruction_count, bool match_instruction_count,
			bool discovered_topology, bool validate_source_words = true);
		void RememberFreeCacheEntry(CachedBlock& block);
		CachedBlock* TakeFreeCacheEntry();
		void InvalidateCachedBlock(CachedBlock& block);
		DirectLinkSlot* GetRecordedDirectLink(IncomingLinkRecord& record);
		s32 LastIncomingLinkIndex(u32 target_pc) const;
		void ClearIncomingLinks();
		void RegisterIncomingLink(CachedBlock& block, u8 slot_index, const DirectLinkSlot& link);
		void RegisterIncomingLinks(CachedBlock& block);
		void UnregisterIncomingLinks(CachedBlock& block);
		bool CachedBlockHasDirectSourceSpan(const CachedBlock& block) const;
		bool CachedBlockSourceMatches(const CachedBlock& block) const;
		u32 RetireStaleOverlappingBlocks(u32 start_pc, u32 instruction_count,
			bool discovered_topology);
		bool ValidateCachedBlock(CachedBlock& block, bool validate_source_words = true);
		CachedBlock* FindLookupBlockByStartPc(u32 start_pc,
			bool discovered_topology);
		bool FindCachedBlock(u32 start_pc, u32 instruction_count, CachedBlock** block,
			bool* lookup_hit, bool match_code_budget_source = false);
		CachedBlock* FindLinkTargetByStartPc(u32 start_pc,
			bool discovered_topology, bool validate_source_words = true);
		void ResolveAdjacentSplitDependency(u32 start_pc, u32 instruction_count,
			bool discovered_topology,
			u32* dependency_start_pc, u32* dependency_instruction_count,
			u32* dependency_charged_cycles_before) const;
		CachedBlock* AllocateCacheEntry();
		bool EnsureCodeCache();
		void ReleaseCodeCache();
		u8* AllocateCodeSlice(size_t capacity, size_t* slice_offset);
		void CommitCodeSlice(size_t slice_offset, size_t code_size);
		void RewindCodeCache(size_t slice_offset);
		u32 ResetForCachePressure();
		u32 InvalidateRangeInternal(u32 start_pc, u32 instruction_count,
			const bool* discovered_topology);
		bool CompileIntoCacheEntry(CachedBlock& block, u32 start_pc, u32 instruction_count,
			u32* scaled_cycles, bool allow_code_budget_split = false,
			u32 dependency_start_pc = 0, u32 dependency_instruction_count = 0,
			u32 dependency_charged_cycles_before = 0,
			bool pcsx2_short_split = false,
			bool discovered_topology = false);
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
		bool UnlinkIncomingLinks(u32 target_pc,
			const bool* discovered_topology = nullptr);
		void RelinkDirectLinks();
		bool IsPersistentDispatchBarrier(u32 pc) const;
		bool SetCanonicalPersistentDispatchBarrier(u32 canonical_pc, bool enabled);
#if defined(VITASX2_QEMU_VALIDATION)
		void RecordPersistentExit(BlockExitKind exit);
#endif

		std::vector<std::unique_ptr<CachedBlock>> m_cache;
		CachedBlock* m_free_cache_head = nullptr;
		std::vector<BlockRecord> m_block_records;
		std::vector<IncomingLinkRecord> m_incoming_links;
		std::array<std::vector<RamSourceRecord>, RAM_SOURCE_PAGE_COUNT>
			m_ram_source_pages;
		std::array<u32, RAM_SOURCE_PAGE_COUNT> m_ram_source_page_live_counts{};
		std::array<u8, RAM_WRITE_GUARD_PAGE_COUNT>
			m_ram_source_page_live_flags{};
		u32 m_next_source_serial = 1;
		// Explicit trace windows and PCSX2-discovered BaseBlocks can have the same
		// guest PC but different spans and scheduler tails. Keep their metadata
		// lookups independent; only discovered blocks enter generated dispatch.
		std::array<LookupPage**, 2> m_lookup_pages{};
		GeneratedLookupPage** m_generated_lookup_pages = nullptr;
		GeneratedLookupPage** m_active_generated_lookup_pages = nullptr;
		VitaA32::CodeBuffer m_persistent_dispatch_code;
		const void* m_persistent_dispatch_entry = nullptr;
		const void* m_persistent_direct_exit = nullptr;
		const void* m_persistent_scheduler_elided_direct_exit = nullptr;
		const void* m_persistent_event_exit = nullptr;
		static constexpr size_t MAX_PERSISTENT_DISPATCH_BARRIERS = 8;
		std::array<u32, MAX_PERSISTENT_DISPATCH_BARRIERS>
			m_persistent_dispatch_barriers{};
		u8 m_persistent_dispatch_barrier_count = 0;
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
