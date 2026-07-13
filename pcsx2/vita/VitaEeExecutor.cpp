// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "pcsx2/vita/VitaEeExecutor.h"

#include "common/Vita/VitaJitMemory.h"
#include "pcsx2/Config.h"
#include "pcsx2/Memory.h"
#include "pcsx2/R5900.h"
#include "pcsx2/vtlb.h"
#include "pcsx2/vita/VitaEeBlockCompiler.h"
#if !defined(VITASX2_QEMU_VALIDATION) || defined(VITASX2_QEMU_FULL_CORE)
#include "pcsx2/DebugTools/GsTrace.h"
#include "pcsx2/DebugTools/VuTrace.h"
#include "pcsx2/vita/VitaCore.h"
#endif

#include <cstring>
#include <new>
#include <algorithm>

#if defined(VITASX2_QEMU_VALIDATION)
extern u32 g_qemuEmbeddedCompatibleContinuationActivations;
extern u32 g_qemuEmbeddedCompatibleContinuationSourceMismatches;
extern u32 g_qemuEmbeddedCompatibleContinuationIncompatibleTargets;
extern u32 g_qemuCompatibleVtlbWriteFastEntryActivations;
extern u32 g_qemuCompatibleVtlbReadFastEntryActivations;
extern u32 g_qemuEeDirectExitSourcePc;
#endif

namespace
{
	using GeneratedBlock = u32 (*)();
	using PersistentDispatcher = u32 (*)(const void* entry_point, void* context,
		const void* dispatch_callback);

	constexpr u16 REG_LR = 1u << 14;
	constexpr u16 REG_PC = 1u << 15;
	constexpr unsigned HOST_CPU_REGS = 4;
	constexpr unsigned HOST_VTLB_VMAP = 7;
	constexpr unsigned HOST_VTLB_HOST_MEMORY_BASE = 8;
	constexpr unsigned HOST_SP = 13;
	constexpr unsigned HOST_CALLBACK = 12;
	constexpr u8 PERSISTENT_METADATA_SIZE =
		VitaEE::BlockCompiler::PERSISTENT_LINK_METADATA_SIZE;
	constexpr u16 PERSISTENT_CONTEXT_OFFSET =
		VitaEE::BlockCompiler::PERSISTENT_LINK_CONTEXT_OFFSET;
	constexpr u16 PERSISTENT_CALLBACK_OFFSET =
		VitaEE::BlockCompiler::PERSISTENT_LINK_CALLBACK_OFFSET;
	constexpr u16 PERSISTENT_EXIT_VALUE_OFFSET =
		VitaEE::BlockCompiler::PERSISTENT_LINK_EXIT_VALUE_OFFSET;
	constexpr u16 PERSISTENT_VTLB_VMAP_OFFSET =
		VitaEE::BlockCompiler::PERSISTENT_LINK_VTLB_VMAP_OFFSET;
	constexpr u16 PERSISTENT_VTLB_HOST_BASE_OFFSET =
		VitaEE::BlockCompiler::PERSISTENT_LINK_VTLB_HOST_BASE_OFFSET;
	constexpr size_t PERSISTENT_DISPATCH_CODE_CAPACITY = 4096;

	extern "C" __attribute__((noinline)) u32 VitaEeA32DirectExit()
	{
		return static_cast<u32>(VitaEE::BlockExitKind::Direct);
	}

	extern "C" __attribute__((noinline)) u32 VitaEeA32EventExit()
	{
		return static_cast<u32>(VitaEE::BlockExitKind::Event);
	}

	bool DecodeExitKind(u32 value, VitaEE::BlockExitKind* exit)
	{
		if (value == static_cast<u32>(VitaEE::BlockExitKind::Direct))
		{
			*exit = VitaEE::BlockExitKind::Direct;
			return true;
		}

		if (value == static_cast<u32>(VitaEE::BlockExitKind::Event))
		{
			*exit = VitaEE::BlockExitKind::Event;
			return true;
		}

		return false;
	}

	size_t AlignUp(size_t value, size_t alignment)
	{
		return (value + alignment - 1) & ~(alignment - 1);
	}

#if defined(VITASX2_QEMU_VALIDATION)
	u32 CountA32Instructions(const VitaA32::CodeBuffer& code, u32 instruction_mask,
		u32 expected_instruction)
	{
		u32 count = 0;
		for (size_t offset = 0; offset + sizeof(u32) <= code.Size(); offset += sizeof(u32))
		{
			u32 instruction = 0;
			std::memcpy(&instruction, code.Data() + offset, sizeof(instruction));
			count += (instruction & instruction_mask) == expected_instruction ? 1u : 0u;
		}
		return count;
	}

	void PopulateFrameEvidence(const VitaA32::CodeBuffer& block,
		const VitaA32::CodeBuffer& dispatcher, VitaEE::BlockExecutionResult* result)
	{
		constexpr u32 FRAME_INSTRUCTION_MASK = 0xffff0000u;
		constexpr u32 PUSH = 0xe92d0000u;
		constexpr u32 POP = 0xe8bd0000u;
		result->generated_frame_pushes =
			CountA32Instructions(block, FRAME_INSTRUCTION_MASK, PUSH);
		result->generated_frame_pops =
			CountA32Instructions(block, FRAME_INSTRUCTION_MASK, POP);
		result->dispatcher_frame_pushes =
			CountA32Instructions(dispatcher, FRAME_INSTRUCTION_MASK, PUSH);
		result->dispatcher_frame_pops =
			CountA32Instructions(dispatcher, FRAME_INSTRUCTION_MASK, POP);
	}
#endif
} // namespace

namespace VitaEE
{
	BlockExecutor::BlockExecutor()
	{
		m_cache.reserve(INITIAL_CACHE_CAPACITY);
		m_free_cache_entries.reserve(INITIAL_CACHE_CAPACITY);
		m_block_records.reserve(INITIAL_CACHE_CAPACITY);
		m_incoming_links.reserve(INITIAL_CACHE_CAPACITY * DIRECT_LINK_SLOT_COUNT);
	}

	BlockExecutor::~BlockExecutor()
	{
		Shutdown();
		ReleaseLookupPages();
		ReleaseGeneratedLookupPages();
	}

	u32 BlockExecutor::LookupPageIndex(u32 start_pc)
	{
		return start_pc >> 16;
	}

	u32 BlockExecutor::LookupEntryIndex(u32 start_pc)
	{
		return (start_pc & 0xffffu) >> 2;
	}

	bool BlockExecutor::EnsureLookupDirectory()
	{
		if (m_lookup_pages)
			return true;

		m_lookup_pages = new (std::nothrow) LookupPage*[LOOKUP_DIRECTORY_ENTRY_COUNT] {};
		return (m_lookup_pages != nullptr);
	}

	BlockExecutor::LookupPage* BlockExecutor::GetLookupPage(u32 start_pc, bool allocate)
	{
		if (!m_lookup_pages && (!allocate || !EnsureLookupDirectory()))
			return nullptr;

		const u32 page = LookupPageIndex(start_pc);
		if (!m_lookup_pages[page] && allocate)
			m_lookup_pages[page] = new (std::nothrow) LookupPage();

		return m_lookup_pages[page];
	}

	bool BlockExecutor::EnsureGeneratedLookupDirectory()
	{
		if (m_generated_lookup_pages)
			return true;

		m_generated_lookup_pages = new (std::nothrow) GeneratedLookupPage*[LOOKUP_DIRECTORY_ENTRY_COUNT] {};
		if (m_generated_lookup_pages && m_direct_linking_enabled)
			m_active_generated_lookup_pages = m_generated_lookup_pages;
		return (m_generated_lookup_pages != nullptr);
	}

	BlockExecutor::GeneratedLookupPage* BlockExecutor::GetGeneratedLookupPage(u32 start_pc, bool allocate)
	{
		if (!m_generated_lookup_pages && (!allocate || !EnsureGeneratedLookupDirectory()))
			return nullptr;

		const u32 page = LookupPageIndex(start_pc);
		if (!m_generated_lookup_pages[page] && allocate)
			m_generated_lookup_pages[page] = new (std::nothrow) GeneratedLookupPage();

		return m_generated_lookup_pages[page];
	}

	void BlockExecutor::RegisterBlockLookup(CachedBlock& block)
	{
		if (!block.valid || (block.start_pc & 0x3u) != 0)
			return;

		// PCSX2 owner: x86/BaseblockEx.h::PC_GETBLOCK_()/recLUT_SetPage().
		// Vita keeps the same 64 KiB guest-page lookup granularity, but allocates
		// pages lazily instead of reserving a BASEBLOCK for every possible EE word.
		const u32 index = LookupEntryIndex(block.start_pc);
		if (LookupPage* page = GetLookupPage(block.start_pc, true))
			page->blocks[index] = &block;
		if (GeneratedLookupPage* page = GetGeneratedLookupPage(block.start_pc, true))
			page->entry_points[index] = LinkedEntryPoint(block);
	}

	void BlockExecutor::UnregisterBlockLookup(CachedBlock& block)
	{
		if ((block.start_pc & 0x3u) != 0)
			return;

		const u32 index = LookupEntryIndex(block.start_pc);
		if (LookupPage* page = GetLookupPage(block.start_pc, false))
		{
			CachedBlock*& entry = page->blocks[index];
			if (entry == &block)
				entry = nullptr;
		}

		if (GeneratedLookupPage* page = GetGeneratedLookupPage(block.start_pc, false))
		{
			const void*& entry = page->entry_points[index];
			if (entry == LinkedEntryPoint(block))
				entry = nullptr;
		}
	}

	void BlockExecutor::ReleaseLookupPages()
	{
		if (!m_lookup_pages)
			return;

		for (u32 i = 0; i < LOOKUP_DIRECTORY_ENTRY_COUNT; i++)
			delete m_lookup_pages[i];

		delete[] m_lookup_pages;
		m_lookup_pages = nullptr;
	}

	void BlockExecutor::ReleaseGeneratedLookupPages()
	{
		if (!m_generated_lookup_pages)
			return;

		for (u32 i = 0; i < LOOKUP_DIRECTORY_ENTRY_COUNT; i++)
			delete m_generated_lookup_pages[i];

		delete[] m_generated_lookup_pages;
		m_generated_lookup_pages = nullptr;
		m_active_generated_lookup_pages = nullptr;
	}

	s32 BlockExecutor::LastBlockRecordIndex(u32 pc) const
	{
		if (m_block_records.empty())
			return -1;

		s32 min = 0;
		s32 max = static_cast<s32>(m_block_records.size() - 1);
		while (min != max)
		{
			const s32 mid = (min + max + 1) >> 1;
			if (m_block_records[mid].start_pc > pc)
				max = mid - 1;
			else
				min = mid;
		}

		return min;
	}

	bool BlockExecutor::RegisterBlockRecord(CachedBlock& block)
	{
		if (!block.valid)
			return false;

		UnregisterBlockRecord(block);
		if (m_block_records.size() >= MAX_CACHE_CAPACITY)
			return false;

		// PCSX2 owner: x86/BaseblockEx.h::BaseBlockArray::insert().
		// Keep translated blocks sorted by guest start PC so invalidation and
		// target lookup do not depend on a linear walk of the cache storage.
		u32 insert_index = 0;
		u32 insert_limit = static_cast<u32>(m_block_records.size());
		while (insert_index < insert_limit)
		{
			const u32 mid = (insert_index + insert_limit) >> 1;
			if (m_block_records[mid].start_pc <= block.start_pc)
				insert_index = mid + 1;
			else
				insert_limit = mid;
		}

		m_block_records.insert(m_block_records.begin() + insert_index, {
			&block,
			block.code.EntryPoint(),
			block.start_pc,
			block.instruction_count,
			block.code.Size(),
		});
		return true;
	}

	void BlockExecutor::UnregisterBlockRecord(CachedBlock& block)
	{
		// PCSX2 owner: x86/BaseblockEx.cpp::BaseBlocks::LastIndex() plus
		// BaseBlocks::Remove(). Records are sorted by start PC, so only the
		// same-PC run can contain this block.
		s32 index = LastBlockRecordIndex(block.start_pc);
		while (index >= 0 && m_block_records[index].start_pc == block.start_pc)
			index--;
		index++;

		for (; index >= 0 && static_cast<u32>(index) < m_block_records.size() &&
			   m_block_records[index].start_pc == block.start_pc;
			 index++)
		{
			if (m_block_records[index].block == &block)
			{
				m_block_records.erase(m_block_records.begin() + index);
				return;
			}
		}
	}

	void BlockExecutor::ClearBlockRecords()
	{
		m_block_records.clear();
	}

	BlockExecutor::CachedBlock* BlockExecutor::FindRecordedBlockByStartPc(
		u32 start_pc, u32 instruction_count, bool match_instruction_count, bool validate_source_words)
	{
		s32 index = LastBlockRecordIndex(start_pc);
		while (index >= 0 && m_block_records[index].start_pc == start_pc)
		{
			CachedBlock* block = m_block_records[index].block;
			if (block && block->valid &&
				(!match_instruction_count || block->instruction_count == instruction_count))
			{
				if (ValidateCachedBlock(*block, validate_source_words))
					return block;

				break;
			}

			index--;
		}

		return nullptr;
	}

	void BlockExecutor::RememberFreeCacheEntry(CachedBlock& block)
	{
		// PCSX2 owner: x86/BaseblockEx.cpp::BaseBlocks::Remove()/New().
		// Removed BaseBlock records become reusable metadata; keep Vita's
		// CachedBlock object reuse O(1) instead of scanning m_cache.
		if (block.queued_free)
			return;

		block.queued_free = true;
		m_free_cache_entries.push_back(&block);
	}

	BlockExecutor::CachedBlock* BlockExecutor::TakeFreeCacheEntry()
	{
		while (!m_free_cache_entries.empty())
		{
			CachedBlock* block = m_free_cache_entries.back();
			m_free_cache_entries.pop_back();
			if (block)
				block->queued_free = false;
			if (block && !block->valid)
				return block;
		}

		return nullptr;
	}

	void BlockExecutor::InvalidateCachedBlock(CachedBlock& block)
	{
		if (!block.valid)
			return;

		UnlinkIncomingLinks(block.start_pc);
		UnregisterIncomingLinks(block);
		UnregisterBlockLookup(block);
		UnregisterBlockRecord(block);
		block.valid = false;
		block.linked_entry_offset = 0;
		block.resident_self_link_entry_offset = static_cast<size_t>(-1);
		block.resident_self_link_entry_loads = 0;
		block.gpr_link_signature = GprLinkSignature{};
		block.compatible_link_entry_offset = static_cast<size_t>(-1);
		block.compatible_vtlb_fast_entries = {};
		block.compatible_link_entry_loads = 0;
		block.direct_links = {};
		block.code.Release();
		RememberFreeCacheEntry(block);
	}

	DirectLinkSlot* BlockExecutor::GetRecordedDirectLink(IncomingLinkRecord& record)
	{
		if (!record.source || !record.source->valid || record.slot_index >= DIRECT_LINK_SLOT_COUNT)
			return nullptr;

		DirectLinkSlot& link = record.source->direct_links.slots[record.slot_index];
		if (!link.valid || link.target_pc != record.target_pc)
			return nullptr;

		return &link;
	}

	s32 BlockExecutor::LastIncomingLinkIndex(u32 target_pc) const
	{
		if (m_incoming_links.empty())
			return -1;

		s32 min = 0;
		s32 max = static_cast<s32>(m_incoming_links.size() - 1);
		while (min != max)
		{
			const s32 mid = (min + max + 1) >> 1;
			if (m_incoming_links[mid].target_pc > target_pc)
				max = mid - 1;
			else
				min = mid;
		}

		return min;
	}

	void BlockExecutor::ClearIncomingLinks()
	{
		m_incoming_links.clear();
	}

	void BlockExecutor::RegisterIncomingLink(CachedBlock& block, u8 slot_index, const DirectLinkSlot& link)
	{
		if (!link.valid || m_incoming_links.size() >= MAX_INCOMING_LINKS)
			return;

		u32 insert_index = 0;
		u32 insert_limit = static_cast<u32>(m_incoming_links.size());
		while (insert_index < insert_limit)
		{
			const u32 mid = (insert_index + insert_limit) >> 1;
			if (m_incoming_links[mid].target_pc <= link.target_pc)
				insert_index = mid + 1;
			else
				insert_limit = mid;
		}
		m_incoming_links.insert(m_incoming_links.begin() + insert_index, {&block, link.target_pc, slot_index});
	}

	void BlockExecutor::RegisterIncomingLinks(CachedBlock& block)
	{
		UnregisterIncomingLinks(block);

		// PCSX2 owner: x86/BaseblockEx.cpp::BaseBlocks::Link(). The x86
		// provider stores target-PC -> patch-site records so New()/Remove()
		// only touch incoming edges for the affected block. Keep Vita's vector
		// sorted by target PC so the common patch/unlink path does the same.
		for (u8 i = 0; i < DIRECT_LINK_SLOT_COUNT; i++)
		{
			const DirectLinkSlot& link = block.direct_links.slots[i];
			RegisterIncomingLink(block, i, link);
		}
	}

	void BlockExecutor::UnregisterIncomingLinks(CachedBlock& block)
	{
		u32 write_index = 0;
		for (u32 read_index = 0; read_index < m_incoming_links.size(); read_index++)
		{
			if (m_incoming_links[read_index].source == &block)
				continue;

			if (write_index != read_index)
				m_incoming_links[write_index] = m_incoming_links[read_index];
			write_index++;
		}

		m_incoming_links.resize(write_index);
	}

	u32 BlockExecutor::Shutdown()
	{
		const u32 invalidated = Reset();
		m_persistent_dispatch_code.Release();
		m_persistent_dispatch_entry = nullptr;
		m_persistent_direct_exit = nullptr;
		m_persistent_event_exit = nullptr;
		ReleaseCodeCache();
		return invalidated;
	}

	u32 BlockExecutor::Reset()
	{
		u32 invalidated = 0;
		m_free_cache_entries.clear();
		for (const std::unique_ptr<CachedBlock>& entry : m_cache)
		{
			CachedBlock& block = *entry;
			if (block.valid)
				invalidated++;

			block.code.Release();
			block.valid = false;
			block.queued_free = false;
			block.linked_entry_offset = 0;
			block.resident_self_link_entry_offset = static_cast<size_t>(-1);
			block.resident_self_link_entry_loads = 0;
			block.gpr_link_signature = GprLinkSignature{};
			block.compatible_link_entry_offset = static_cast<size_t>(-1);
			block.compatible_vtlb_fast_entries = {};
			block.compatible_link_entry_loads = 0;
			block.direct_links = {};
			RememberFreeCacheEntry(block);
		}

		ClearBlockRecords();
		ClearIncomingLinks();
		ReleaseLookupPages();
		ReleaseGeneratedLookupPages();
		m_code_cache_resets = 0;
		// PCSX2's recResetRaw() rewinds the EE cache pointer rather than freeing
		// its executable mapping. Keep Vita's VM-domain block for the executor's
		// lifetime too; the persistent dispatcher occupies a protected slab at
		// the base while resettable block code starts after it.
		m_code_cache_used = m_persistent_dispatch_entry ?
			PERSISTENT_DISPATCH_CODE_CAPACITY : 0;
		return invalidated;
	}

	u32 BlockExecutor::InvalidateRange(u32 start_pc, u32 instruction_count)
	{
		if (instruction_count == 0 || instruction_count > ((UINT32_MAX - start_pc) / 4))
			return 0;

		const u32 end_pc = start_pc + instruction_count * 4;
		u32 invalidated = 0;
		constexpr u32 max_block_bytes = MAX_STRAIGHT_LINE_BLOCK_INSTRUCTIONS * 4;
		const u32 first_candidate_pc = (start_pc > max_block_bytes) ? (start_pc - max_block_bytes) : 0;
		// PCSX2 keeps BaseBlocks sorted by guest start PC. Since Vita blocks are
		// bounded, entries before this lower bound cannot overlap the cleared
		// word range.
		u32 i = 0;
		u32 limit = static_cast<u32>(m_block_records.size());
		while (i < limit)
		{
			const u32 mid = (i + limit) >> 1;
			if (m_block_records[mid].start_pc < first_candidate_pc)
				i = mid + 1;
			else
				limit = mid;
		}

		for (; i < m_block_records.size();)
		{
			CachedBlock* block = m_block_records[i].block;
			if (!block || !block->valid)
			{
				if (block)
					UnregisterBlockRecord(*block);
				else
					i++;
				continue;
			}

			if (block->start_pc >= end_pc)
				break;

			const u32 block_end = block->start_pc + block->instruction_count * 4;
			if (start_pc < block_end)
			{
				InvalidateCachedBlock(*block);
				invalidated++;
				continue;
			}

			i++;
		}

		return invalidated;
	}

	void BlockExecutor::SetDirectLinkingEnabled(bool enabled)
	{
		if (m_direct_linking_enabled == enabled)
			return;

		m_direct_linking_enabled = enabled;
		if (enabled)
		{
			m_active_generated_lookup_pages = m_generated_lookup_pages;
			RelinkDirectLinks();
		}
		else
		{
			m_active_generated_lookup_pages = nullptr;
			UnlinkIncomingLinks(UINT32_MAX);
		}
	}

	void BlockExecutor::SetPersistentDispatchEnabled(bool enabled)
	{
		if (m_persistent_dispatch_enabled == enabled)
			return;

		// Callable blocks return by popping their own frame; persistent blocks
		// tail-jump to the dispatcher stubs. They cannot coexist in one lookup
		// cache, so change modes only through the same whole-cache reset PCSX2 uses
		// when its recompiler ABI changes.
		Reset();
		m_persistent_dispatch_enabled = enabled;
	}

#if defined(VITASX2_QEMU_VALIDATION)
	void BlockExecutor::SetCompatibleGprDirtyCarryEnabled(bool enabled)
	{
		if (m_compatible_gpr_dirty_carry_enabled == enabled)
			return;

		// This is a validation-only A/B control. Dirty state is part of the
		// compiled link ABI, so never mix its two forms in one cache.
		Reset();
		m_compatible_gpr_dirty_carry_enabled = enabled;
	}

	void BlockExecutor::SetCompatibleSchedulerCarryEnabled(bool enabled)
	{
		if (m_compatible_scheduler_carry_enabled == enabled)
			return;

		// The scheduler host/representation is part of the generated link ABI.
		Reset();
		m_compatible_scheduler_carry_enabled = enabled;
	}

	void BlockExecutor::SetCompatibleVtlbPointerCarryEnabled(bool enabled)
	{
		if (m_compatible_vtlb_pointer_carry_enabled == enabled)
			return;

		// The translated pointer's host/provenance is part of the link ABI.
		Reset();
		m_compatible_vtlb_pointer_carry_enabled = enabled;
	}

	void BlockExecutor::SetCompatibleVtlbHostReclaimEnabled(bool enabled)
	{
		if (m_compatible_vtlb_host_reclaim_enabled == enabled)
			return;

		Reset();
		m_compatible_vtlb_host_reclaim_enabled = enabled;
	}

	void BlockExecutor::SetCompatiblePredicateCarryEnabled(bool enabled)
	{
		if (m_compatible_predicate_carry_enabled == enabled)
			return;

		// The predicate host and its linked-entry skip are part of the chain ABI.
		Reset();
		m_compatible_predicate_carry_enabled = enabled;
	}

	void BlockExecutor::SetCompatibleLikelyTakenSuffixEnabled(bool enabled)
	{
		if (m_compatible_likely_taken_suffix_enabled == enabled)
			return;

		Reset();
		m_compatible_likely_taken_suffix_enabled = enabled;
	}

	void BlockExecutor::SetCompatiblePredicateEntryVariantEnabled(bool enabled)
	{
		if (m_compatible_predicate_entry_variant_enabled == enabled)
			return;

		Reset();
		m_compatible_predicate_entry_variant_enabled = enabled;
	}

	void BlockExecutor::SetEmbeddedCompatibleContinuationEnabled(bool enabled)
	{
		if (m_embedded_compatible_continuation_enabled == enabled)
			return;

		Reset();
		m_embedded_compatible_continuation_enabled = enabled;
	}

	void BlockExecutor::SetFusedDirectEventLinkEnabled(bool enabled)
	{
		if (m_fused_direct_event_link_enabled == enabled)
			return;

		Reset();
		m_fused_direct_event_link_enabled = enabled;
	}

	void BlockExecutor::SetCombinedCompatibleTakenEventEnabled(bool enabled)
	{
		if (m_combined_compatible_taken_event_enabled == enabled)
			return;

		Reset();
		m_combined_compatible_taken_event_enabled = enabled;
	}

	void BlockExecutor::SetCompatibleVtlbWriteGuardHoistEnabled(bool enabled)
	{
		if (m_compatible_vtlb_write_guard_hoist_enabled == enabled)
			return;

		Reset();
		m_compatible_vtlb_write_guard_hoist_enabled = enabled;
	}

	void BlockExecutor::SetCompatibleVtlbReadGuardHoistEnabled(bool enabled)
	{
		if (m_compatible_vtlb_read_guard_hoist_enabled == enabled)
			return;

		Reset();
		m_compatible_vtlb_read_guard_hoist_enabled = enabled;
	}

	void BlockExecutor::SetSingleBlockGprLinkEnabled(bool enabled)
	{
		if (m_single_block_gpr_link_enabled == enabled)
			return;

		Reset();
		m_single_block_gpr_link_enabled = enabled;
	}

	void BlockExecutor::SetReciprocalJumpGprLinkEnabled(bool enabled)
	{
		if (m_reciprocal_jump_gpr_link_enabled == enabled)
			return;

		Reset();
		m_reciprocal_jump_gpr_link_enabled = enabled;
	}

	void BlockExecutor::SetThreeBlockGprLinkEnabled(bool enabled)
	{
		if (m_three_block_gpr_link_enabled == enabled)
			return;

		Reset();
		m_three_block_gpr_link_enabled = enabled;
	}

	void BlockExecutor::SetVtlbLinkedEntryPcPublicationEnabled(bool enabled)
	{
		if (m_vtlb_linked_entry_pc_publication_enabled == enabled)
			return;

		// Validation-only A/B control for PCSX2's FLUSH_FULLVTLB == 0 contract.
		// Entry layout changes, so discard every cached block before switching.
		Reset();
		m_vtlb_linked_entry_pc_publication_enabled = enabled;
	}

	void BlockExecutor::SetDirectLinkRejectionProfileEnabled(bool enabled)
	{
		if (m_direct_link_rejection_profile_enabled == enabled)
			return;

		// Source publication is emitted only into direct-exit cold tails.
		Reset();
		m_direct_link_rejection_profile_enabled = enabled;
		ResetDirectLinkRejectionProfile();
	}

	void BlockExecutor::ResetDirectLinkRejectionProfile()
	{
		m_direct_link_rejection_profile = VitaA32EeLinkRejectionProfile{};
		m_direct_link_rejection_edges.clear();
		g_qemuEeDirectExitSourcePc = 0;
	}

	VitaA32EeLinkRejectionProfile BlockExecutor::GetDirectLinkRejectionProfile() const
	{
		VitaA32EeLinkRejectionProfile result = m_direct_link_rejection_profile;
		std::vector<VitaA32EeLinkRejectionEdge> edges;
		for (const auto& [edge_key, counts] : m_direct_link_rejection_edges)
		{
			for (u32 kind = 0; kind < counts.size(); kind++)
			{
				if (counts[kind] == 0)
					continue;
				VitaA32EeLinkRejectionEdge edge;
				edge.source_pc = static_cast<u32>(edge_key >> 32);
				edge.target_pc = static_cast<u32>(edge_key);
				edge.kind = static_cast<VitaA32EeLinkRejectionKind>(kind);
				edge.exits = counts[kind];
				edges.push_back(edge);
			}
		}
		std::sort(edges.begin(), edges.end(), [](const auto& lhs, const auto& rhs) {
			return lhs.exits != rhs.exits ? lhs.exits > rhs.exits :
				(lhs.source_pc != rhs.source_pc ? lhs.source_pc < rhs.source_pc :
				 lhs.target_pc < rhs.target_pc);
		});
		result.edge_count = std::min<u32>(static_cast<u32>(edges.size()),
			VITA_A32_EE_LINK_REJECTION_EDGE_COUNT);
		for (u32 i = 0; i < result.edge_count; i++)
			result.edges[i] = edges[i];
		return result;
	}

	void BlockExecutor::RecordPersistentExit(BlockExitKind exit)
	{
		if (!m_direct_link_rejection_profile_enabled)
			return;

		m_direct_link_rejection_profile.persistent_boundaries++;
		if (exit == BlockExitKind::Event)
		{
			m_direct_link_rejection_profile.event_exits++;
			return;
		}
		if (exit != BlockExitKind::Direct)
			return;

		m_direct_link_rejection_profile.direct_exits++;
		const u32 source_pc = g_qemuEeDirectExitSourcePc;
		const u32 target_pc = cpuRegs.pc;
		g_qemuEeDirectExitSourcePc = 0;
		VitaA32EeLinkRejectionKind kind = VitaA32EeLinkRejectionKind::UnrecordedEdge;
		CachedBlock* const source = source_pc ? FindLookupBlockByStartPc(source_pc) : nullptr;
		DirectLinkSlot* link = nullptr;
		if (source && source->valid)
		{
			for (DirectLinkSlot& candidate : source->direct_links.slots)
			{
				if (candidate.valid && candidate.target_pc == target_pc)
				{
					link = &candidate;
					break;
				}
			}
		}

		if (link)
		{
			CachedBlock* const target = FindLookupBlockByStartPc(target_pc);
			if (!target || !target->valid)
			{
				kind = VitaA32EeLinkRejectionKind::TargetNotCompiled;
			}
			else if (link->embedded_compatible_continuation &&
				(memRead32(target_pc) != link->embedded_source_opcodes[0] ||
				 memRead32(target_pc + sizeof(u32)) != link->embedded_source_opcodes[1]))
			{
				kind = VitaA32EeLinkRejectionKind::EmbeddedSourceMismatch;
			}
			else if (link->requires_compatible_entry)
			{
				if (!source->gpr_link_signature.IsValid())
					kind = VitaA32EeLinkRejectionKind::SourceSignatureMissing;
				else if (!target->gpr_link_signature.IsValid())
					kind = VitaA32EeLinkRejectionKind::TargetSignatureMissing;
				else if (!(source->gpr_link_signature == target->gpr_link_signature))
					kind = VitaA32EeLinkRejectionKind::SignatureMismatch;
				else if (target->compatible_link_entry_offset == static_cast<size_t>(-1) ||
					target->compatible_link_entry_offset >= target->code.Size())
					kind = VitaA32EeLinkRejectionKind::CompatibleEntryMissing;
				else
					kind = VitaA32EeLinkRejectionKind::UnexpectedFallback;
			}
			else
			{
				kind = VitaA32EeLinkRejectionKind::UnexpectedFallback;
			}
		}

		const u32 kind_index = static_cast<u32>(kind);
		m_direct_link_rejection_profile.kinds[kind_index]++;
		const u64 edge_key = (static_cast<u64>(source_pc) << 32) | target_pc;
		m_direct_link_rejection_edges[edge_key][kind_index]++;
	}
#endif

	bool BlockExecutor::EnsurePersistentDispatcher()
	{
		if (m_persistent_dispatch_entry)
			return true;
		if (!EnsureCodeCache())
			return false;

		size_t dispatcher_slice_offset = 0;
		u8* dispatcher_slice = AllocateCodeSlice(
			PERSISTENT_DISPATCH_CODE_CAPACITY, &dispatcher_slice_offset);
		if (!dispatcher_slice || dispatcher_slice_offset != 0 ||
			!m_persistent_dispatch_code.Attach(
				dispatcher_slice, PERSISTENT_DISPATCH_CODE_CAPACITY))
		{
			RewindCodeCache(dispatcher_slice_offset);
			return false;
		}

		const auto fail = [this, dispatcher_slice_offset]() {
			m_persistent_dispatch_code.Release();
			RewindCodeCache(dispatcher_slice_offset);
			m_persistent_dispatch_entry = nullptr;
			m_persistent_direct_exit = nullptr;
			m_persistent_event_exit = nullptr;
			return false;
		};

		VitaA32::CodeBuffer& code = m_persistent_dispatch_code;
		const u16 frame = BlockCompiler::LINK_FRAME_REGISTER_MASK;
		if (!code.EmitPush(frame | REG_LR) ||
			!code.EmitSubImm8(HOST_SP, HOST_SP, PERSISTENT_METADATA_SIZE) ||
			!code.EmitStrImm12(1, HOST_SP, PERSISTENT_CONTEXT_OFFSET) ||
			!code.EmitStrImm12(2, HOST_SP, PERSISTENT_CALLBACK_OFFSET) ||
			!code.EmitMovImm32(HOST_CPU_REGS, static_cast<u32>(reinterpret_cast<uptr>(&cpuRegs))) ||
			// PCSX2's private recompiler dispatcher keeps stable VM bases in host
			// registers. Do the same for ARM32's compact vTLB representation: r7 is
			// the virtual-page table and r8 is the base added to its 32-bit pointer
			// offsets. AAPCS callbacks preserve both registers for the whole chain.
			!code.EmitMovImm32(HOST_VTLB_VMAP,
				static_cast<u32>(reinterpret_cast<uptr>(&vtlb_private::vtlbdata.vmap))) ||
			!code.EmitLdrImm12(HOST_VTLB_VMAP, HOST_VTLB_VMAP, 0) ||
			!code.EmitMovImm32(HOST_VTLB_HOST_MEMORY_BASE,
				static_cast<u32>(reinterpret_cast<uptr>(&vtlb_private::vtlbdata.host_memory_base))) ||
			!code.EmitLdrImm12(HOST_VTLB_HOST_MEMORY_BASE, HOST_VTLB_HOST_MEMORY_BASE, 0) ||
			!code.EmitStrImm12(HOST_VTLB_VMAP, HOST_SP, PERSISTENT_VTLB_VMAP_OFFSET) ||
			!code.EmitStrImm12(HOST_VTLB_HOST_MEMORY_BASE, HOST_SP,
				PERSISTENT_VTLB_HOST_BASE_OFFSET) ||
			!code.EmitBx(0))
		{
			return fail();
		}

		const size_t direct_exit_offset = code.Size();
		if (!code.EmitMovImm8(0, static_cast<u8>(BlockExitKind::Direct)))
			return fail();
		const size_t direct_to_common = code.EmitBranchPlaceholder();
		if (direct_to_common == static_cast<size_t>(-1))
			return fail();

		const size_t event_exit_offset = code.Size();
		if (!code.EmitMovImm8(0, static_cast<u8>(BlockExitKind::Event)))
			return fail();

		const size_t common_offset = code.Size();
		if (!code.PatchBranch(direct_to_common, common_offset) ||
			// Compatible chains may lend r7/r8 to GPR mappings. Restore the
			// canonical dispatcher vTLB ABI once in this shared cold exit instead of
			// duplicating reloads in every generated direct/event tail.
			!code.EmitLdrImm12(HOST_VTLB_VMAP, HOST_SP, PERSISTENT_VTLB_VMAP_OFFSET) ||
			!code.EmitLdrImm12(HOST_VTLB_HOST_MEMORY_BASE, HOST_SP,
				PERSISTENT_VTLB_HOST_BASE_OFFSET) ||
			!code.EmitStrImm12(0, HOST_SP, PERSISTENT_EXIT_VALUE_OFFSET) ||
			!code.EmitLdrImm12(1, HOST_SP, PERSISTENT_CONTEXT_OFFSET) ||
			!code.EmitLdrImm12(HOST_CALLBACK, HOST_SP, PERSISTENT_CALLBACK_OFFSET) ||
			!code.EmitBlx(HOST_CALLBACK) ||
			!code.EmitCmpImm32(0, 0))
		{
			return fail();
		}

		const size_t return_branch = code.EmitBranchPlaceholder(VitaA32::Condition::EQ);
		if (return_branch == static_cast<size_t>(-1) || !code.EmitBx(0))
			return fail();

		const size_t return_offset = code.Size();
		if (!code.PatchBranch(return_branch, return_offset, VitaA32::Condition::EQ) ||
			!code.EmitLdrImm12(0, HOST_SP, PERSISTENT_EXIT_VALUE_OFFSET) ||
			!code.EmitAddImm8(HOST_SP, HOST_SP, PERSISTENT_METADATA_SIZE) ||
			!code.EmitPop(frame | REG_PC) || !code.Flush())
		{
			return fail();
		}

		m_persistent_dispatch_entry = code.EntryPoint();
		m_persistent_direct_exit = code.Data() + direct_exit_offset;
		m_persistent_event_exit = code.Data() + event_exit_offset;
		return true;
	}

	bool BlockExecutor::ScanStraightLineBlock(u32 start_pc, u32 max_instruction_count, BlockScanResult* result)
	{
		if (!result || max_instruction_count == 0)
			return false;

		*result = {};
		result->start_pc = start_pc;
		result->stop_pc = start_pc;
		result->stop = BlockScanStop::MaxInstructions;

		u32 exact_region_instruction_count = 0;
		bool exact_region_scan_enabled = true;
#if !defined(VITASX2_QEMU_VALIDATION) || defined(VITASX2_QEMU_FULL_CORE)
		exact_region_scan_enabled = !VitaIsEePreInstructionTraceEnabled() &&
			!Pcsx2Trace::IsGsTraceEnabled() && !Pcsx2Trace::IsVuTraceEnabled() &&
			!EmuConfig.Gamefixes.GoemonTlbHack;
#endif
		if (exact_region_scan_enabled && max_instruction_count >= 35 &&
			BlockCompiler::IsExactCacheDxltgTagSweep(start_pc, 35))
		{
			exact_region_instruction_count = 35;
		}
		else if (exact_region_scan_enabled && max_instruction_count >= 17 &&
			BlockCompiler::IsExactSelfAddressPairScan(start_pc, 17))
			exact_region_instruction_count = 17;
		else if (exact_region_scan_enabled && max_instruction_count >= 9 &&
			BlockCompiler::IsExactCacheDxwbinLoop(start_pc, 9))
			exact_region_instruction_count = 9;
		else if (exact_region_scan_enabled && max_instruction_count >= 8 &&
			BlockCompiler::IsExactCacheIxinLoop(start_pc, 8))
			exact_region_instruction_count = 8;
		if (exact_region_instruction_count != 0)
		{
			for (u32 i = 0; i < exact_region_instruction_count; i++)
			{
				const u32 pc = start_pc + i * sizeof(u32);
				if (isBreakpointNeeded(pc) != 0 || isMemcheckNeeded(pc) != 0 ||
					(i != 0 && (pc & 0xffcu) == 0))
				{
					exact_region_instruction_count = 0;
					break;
				}
			}
		}

		for (u32 i = 0; i < max_instruction_count; i++)
		{
			if (i > ((UINT32_MAX - start_pc) / 4))
			{
				result->stop = BlockScanStop::AddressWrap;
				return true;
			}

			const u32 pc = start_pc + i * 4;

			// Ported from PCSX2 x86/ix86-32/iR5900.cpp::recRecompile():
			// do not fold a required debugger boundary into a regular block.
			if (isBreakpointNeeded(pc) != 0 || isMemcheckNeeded(pc) != 0)
			{
				result->stop = BlockScanStop::DebugBoundary;
				return true;
			}

			// Ported from PCSX2 x86/ix86-32/iR5900.cpp::recRecompile():
			// split before crossing a 4 KiB guest page.
			if (i != 0 && (pc & 0xffcu) == 0)
			{
				result->stop = BlockScanStop::PageBoundary;
				return true;
			}

			const u32 op = memRead32(pc);
			if (BlockCompiler::IsSupportedBranchOpcode(op))
			{
				if (exact_region_instruction_count != 0 &&
					i + 2 < exact_region_instruction_count)
				{
					// Exact multi-block descriptors own or side-exit before every
					// internal branch, including its path-specific cycle seam. Keep
					// scanning until the descriptor's outer backedge and delay slot.
					result->instruction_count++;
					result->stop_pc = pc + 4;
					continue;
				}
				// Ported from PCSX2 x86/ix86-32/iR5900.cpp::recRecompile():
				// branches and jumps end the block after the delay slot. If the
				// delay slot is itself a supported branch, the compiler applies
				// recompileNextInstruction()'s branch-in-delay-slot skip rule.
				if (i + 1 >= max_instruction_count)
				{
					// The A32 compiler's block contract matches PCSX2's branch
					// path: a branch is only compiled together with its delay
					// slot. Stop before this branch and let the next scan own
					// the pair instead of returning an uncompileable tail.
					result->stop = BlockScanStop::MaxInstructions;
					return true;
				}

				if (pc > UINT32_MAX - 4)
				{
					result->stop = BlockScanStop::AddressWrap;
					return true;
				}

				const u32 delay_pc = pc + 4;
				const u32 delay_op = memRead32(delay_pc);
				if (!BlockCompiler::CanCompileDelaySlotOpcode(delay_op))
				{
					result->stop = BlockScanStop::UnsupportedOpcode;
					return true;
				}

				result->instruction_count += 2;
				result->stop_pc = delay_pc + 4;
				result->stop = BlockScanStop::Branch;
				return true;
			}

			if (!BlockCompiler::CanCompileOpcode(op))
			{
				result->stop = BlockScanStop::UnsupportedOpcode;
				return true;
			}

			result->instruction_count++;
			result->stop_pc = pc + 4;
			if (BlockCompiler::RequiresBlockEndAfterOpcode(op) && exact_region_instruction_count == 0)
			{
				result->stop = BlockScanStop::OpcodeBoundary;
				return true;
			}
		}

		return true;
	}

	bool BlockExecutor::AnalyzeGprLinkSignature(u32 start_pc, u32 instruction_count,
		GprLinkSignature* signature) const
	{
		if (!signature)
			return false;
		*signature = GprLinkSignature{};
		if (!m_persistent_dispatch_enabled || !m_direct_linking_enabled ||
			instruction_count < 2)
		{
			return false;
		}

		const auto successors = [this](u32 block_pc, u32 block_instructions,
			u32* fallthrough, u32* taken) {
			if (!fallthrough || !taken || block_instructions < 2 ||
				block_instructions > ((UINT32_MAX - block_pc) / sizeof(u32)))
			{
				return false;
			}
			const u32 branch_pc = block_pc + (block_instructions - 2) * sizeof(u32);
			const u32 op = memRead32(branch_pc);
			const unsigned opcode = op >> 26;
			if (opcode == 0x02)
			{
#if defined(VITASX2_QEMU_VALIDATION)
				if (!m_reciprocal_jump_gpr_link_enabled)
					return false;
#endif
				// PCSX2 BaseBlocks::Link() treats a static J as one reversible edge.
				// Report it in both slots so reciprocal-cycle discovery can share its
				// existing conditional-partner walk without inventing a fallthrough.
				const u32 target = ((branch_pc + sizeof(u32)) & 0xf0000000u) |
					((op & 0x03ffffffu) << 2);
				*fallthrough = target;
				*taken = target;
				return true;
			}
			if (!((opcode >= 0x04 && opcode <= 0x07) ||
				(opcode >= 0x14 && opcode <= 0x17)))
			{
				return false;
			}
			*fallthrough = block_pc + block_instructions * sizeof(u32);
			const s32 displacement = static_cast<s32>(static_cast<s16>(op & 0xffffu)) * 4;
			*taken = branch_pc + sizeof(u32) + static_cast<u32>(displacement);
			return true;
		};

		u32 fallthrough = 0;
		u32 taken = 0;
		if (!successors(start_pc, instruction_count, &fallthrough, &taken))
			return false;

		const u32 candidates[2] = {fallthrough, taken};
		const auto apply_validation_options = [this](GprLinkSignature* candidate) {
#if defined(VITASX2_QEMU_VALIDATION)
			if (!m_compatible_gpr_dirty_carry_enabled)
			{
				for (u8 i = 0; i < candidate->count; i++)
					candidate->mappings[i].dirty = GprLinkDirtyState::Clean;
			}
			if (!m_compatible_scheduler_carry_enabled)
				candidate->scheduler = SchedulerLinkMapping{};
			if (!m_compatible_vtlb_pointer_carry_enabled)
			{
				candidate->vtlb_pointer = VtlbPointerLinkMapping{};
				candidate->vtlb_write_pointer = VtlbPointerLinkMapping{};
				candidate->gpr_qword = GprQwordLinkMapping{};
			}
			if (!m_compatible_predicate_carry_enabled)
				candidate->predicate = PredicateLinkMapping{};
#else
			(void)candidate;
#endif
		};

		// PCSX2 x86/iCore.cpp keeps one allocator mapping across an exact linked
		// self-edge just as it does across a multi-block chain. Give a one-block
		// cycle the same signature path so memory translation and scheduler state
		// can participate instead of falling back to scalar-only self residency.
		if (taken == start_pc)
		{
#if defined(VITASX2_QEMU_VALIDATION)
			if (!m_single_block_gpr_link_enabled)
				return false;
#endif
			const u32 chain_pcs[] = {start_pc};
			const u32 chain_counts[] = {instruction_count};
			GprLinkSignature candidate_signature;
			if (BlockCompiler::BuildGprLinkSignature(chain_pcs, chain_counts, 1,
					&candidate_signature
#if defined(VITASX2_QEMU_VALIDATION)
					, m_compatible_vtlb_host_reclaim_enabled &&
						m_compatible_vtlb_pointer_carry_enabled
#endif
					))
			{
				apply_validation_options(&candidate_signature);
				*signature = candidate_signature;
				return true;
			}
		}
		for (const u32 candidate_pc : candidates)
		{
			if (candidate_pc == start_pc || (candidate_pc & 3u) != 0)
				continue;

			BlockScanResult partner;
			if (!ScanStraightLineBlock(candidate_pc,
					MAX_STRAIGHT_LINE_BLOCK_INSTRUCTIONS, &partner) ||
				partner.instruction_count < 2 || partner.stop != BlockScanStop::Branch)
			{
				continue;
			}

			u32 partner_fallthrough = 0;
			u32 partner_taken = 0;
			if (!successors(candidate_pc, partner.instruction_count,
					&partner_fallthrough, &partner_taken) ||
				(partner_fallthrough != start_pc && partner_taken != start_pc))
			{
				continue;
			}

			if (!BlockCompiler::BuildGprLinkSignature(start_pc, instruction_count,
					candidate_pc, partner.instruction_count, signature
#if defined(VITASX2_QEMU_VALIDATION)
					, m_compatible_vtlb_host_reclaim_enabled &&
						m_compatible_vtlb_pointer_carry_enabled
#endif
					))
			{
				continue;
			}
			apply_validation_options(signature);
			return true;
		}

#if defined(VITASX2_QEMU_VALIDATION)
		if (!m_three_block_gpr_link_enabled)
			return false;
#endif

		// PCSX2's x86 allocator keeps MODE_READ/MODE_WRITE mappings live across
		// linked control flow, not only reciprocal block pairs. Adapt the first
		// measured extension as a deterministic three-block cycle: every member
		// discovers the same sorted PC set and therefore builds the same mapping
		// signature independently. Side exits retain their canonical flush tails.
		GprLinkSignature best_signature;
		bool found_three_block_cycle = false;
		for (const u32 second_pc : candidates)
		{
			if (second_pc == start_pc || (second_pc & 3u) != 0)
				continue;
			BlockScanResult second_scan;
			if (!ScanStraightLineBlock(second_pc,
					MAX_STRAIGHT_LINE_BLOCK_INSTRUCTIONS, &second_scan) ||
				second_scan.instruction_count < 2 ||
				second_scan.stop != BlockScanStop::Branch)
			{
				continue;
			}
			u32 second_fallthrough = 0;
			u32 second_taken = 0;
			if (!successors(second_pc, second_scan.instruction_count,
					&second_fallthrough, &second_taken))
			{
				continue;
			}
			const u32 third_candidates[2] = {second_fallthrough, second_taken};
			for (const u32 third_pc : third_candidates)
			{
				if (third_pc == start_pc || third_pc == second_pc ||
					(third_pc & 3u) != 0)
				{
					continue;
				}
				BlockScanResult third_scan;
				if (!ScanStraightLineBlock(third_pc,
						MAX_STRAIGHT_LINE_BLOCK_INSTRUCTIONS, &third_scan) ||
					third_scan.instruction_count < 2 ||
					third_scan.stop != BlockScanStop::Branch)
				{
					continue;
				}
				u32 third_fallthrough = 0;
				u32 third_taken = 0;
				if (!successors(third_pc, third_scan.instruction_count,
						&third_fallthrough, &third_taken) ||
					(third_fallthrough != start_pc && third_taken != start_pc))
				{
					continue;
				}

				const u32 chain_pcs[] = {start_pc, second_pc, third_pc};
				const u32 chain_counts[] = {instruction_count,
					second_scan.instruction_count, third_scan.instruction_count};
				GprLinkSignature candidate_signature;
				if (!BlockCompiler::BuildGprLinkSignature(chain_pcs, chain_counts, 3,
						&candidate_signature
#if defined(VITASX2_QEMU_VALIDATION)
						, m_compatible_vtlb_host_reclaim_enabled &&
							m_compatible_vtlb_pointer_carry_enabled
#endif
						))
				{
					continue;
				}
				if (!found_three_block_cycle ||
					candidate_signature.block_pcs[0] < best_signature.block_pcs[0] ||
					(candidate_signature.block_pcs[0] == best_signature.block_pcs[0] &&
					 candidate_signature.block_pcs[1] < best_signature.block_pcs[1]) ||
					(candidate_signature.block_pcs[0] == best_signature.block_pcs[0] &&
					 candidate_signature.block_pcs[1] == best_signature.block_pcs[1] &&
					 candidate_signature.block_pcs[2] < best_signature.block_pcs[2]))
				{
					best_signature = candidate_signature;
					found_three_block_cycle = true;
				}
			}
		}
		if (found_three_block_cycle)
		{
			apply_validation_options(&best_signature);
			*signature = best_signature;
			return true;
		}

		return false;
	}

	bool BlockExecutor::ValidateCachedBlock(CachedBlock& block, bool validate_source_words)
	{
		if (!block.valid)
			return false;

		const s8 ee_cycle_rate = EmuConfig.Speedhacks.EECycleRate;
		const u8 cp0_config_cycle_shift = static_cast<u8>((cpuRegs.CP0.n.Config >> 18) & 0x1);
		bool matches = (block.ee_cycle_rate == ee_cycle_rate &&
						block.cp0_config_cycle_shift == cp0_config_cycle_shift);

		if (matches && validate_source_words)
		{
			// PCSX2's x86 recompiler validates source words through recRAMCopy
			// and protected-page invalidation. Ordinary Vita blocks are page-
			// bounded, so a raw vmap pointer compare replaces the old per-opcode
			// vtlb read loop on normal RAM/ROM/scratchpad dispatcher hits. PCSX2
			// keeps a branch at the final page word together with its delay slot;
			// that two-page block and handler-backed pages use the exact memRead32()
			// fallback. InvalidateRange() checks the complete block span, so a write
			// to either source page still discards the cached translation.
			const u32 opcode_bytes = block.instruction_count * static_cast<u32>(sizeof(u32));
			const u32 page_remaining =
				vtlb_private::VTLB_PAGE_SIZE - (block.start_pc & vtlb_private::VTLB_PAGE_MASK);
			bool compared_raw_window = false;
			if (vtlb_private::vtlbdata.vmap && opcode_bytes <= page_remaining)
			{
				const vtlb_private::VTLBVirtual vmv =
					vtlb_private::vtlbdata.vmap[block.start_pc >> vtlb_private::VTLB_PAGE_BITS];
				if (!vmv.isHandler(block.start_pc))
				{
					matches = (std::memcmp(block.opcodes.data(),
								   reinterpret_cast<const void*>(vmv.assumePtr(block.start_pc)), opcode_bytes) == 0);
					compared_raw_window = true;
				}
			}

			if (!compared_raw_window)
			{
				for (u32 i = 0; matches && i < block.instruction_count; i++)
					matches = (block.opcodes[i] == memRead32(block.start_pc + i * 4));
			}
		}

		if (matches)
			return true;

		// PCSX2's x86 path combines recRAMCopy with protected-page faults.
		// Vita has no user-mode fault repair, so validate the dispatcher entry
		// block and rely on Cpu->Clear() invalidation to repair other blocks
		// and their direct links.
		InvalidateCachedBlock(block);
		return false;
	}

	BlockExecutor::CachedBlock* BlockExecutor::FindLookupBlockByStartPc(u32 start_pc)
	{
		if ((start_pc & 0x3u) != 0)
			return nullptr;

		LookupPage* page = GetLookupPage(start_pc, false);
		return page ? page->blocks[LookupEntryIndex(start_pc)] : nullptr;
	}

	bool BlockExecutor::FindCachedBlock(u32 start_pc, u32 instruction_count, CachedBlock** block, bool* lookup_hit)
	{
		if (!block || instruction_count == 0 ||
			instruction_count > MAX_STRAIGHT_LINE_BLOCK_INSTRUCTIONS ||
			instruction_count > ((UINT32_MAX - start_pc) / 4))
		{
			return false;
		}

		*block = nullptr;
		if (lookup_hit)
			*lookup_hit = false;

		if (CachedBlock* entry = FindLookupBlockByStartPc(start_pc))
		{
			if (entry->valid && entry->instruction_count == instruction_count && ValidateCachedBlock(*entry))
			{
				*block = entry;
				if (lookup_hit)
					*lookup_hit = true;
				return true;
			}
		}

		if (CachedBlock* entry = FindRecordedBlockByStartPc(start_pc, instruction_count, true))
		{
			*block = entry;
			return true;
		}

		return false;
	}

	BlockExecutor::CachedBlock* BlockExecutor::FindCachedBlockByStartPc(u32 start_pc, bool validate_source_words)
	{
		if (CachedBlock* entry = FindLookupBlockByStartPc(start_pc))
		{
			if (entry->valid && ValidateCachedBlock(*entry, validate_source_words))
				return entry;
		}

		return FindRecordedBlockByStartPc(start_pc, 0, false, validate_source_words);
	}

	BlockExecutor::CachedBlock* BlockExecutor::AllocateCacheEntry()
	{
		const auto append_entry = [this]() -> CachedBlock* {
			if (m_cache.size() >= MAX_CACHE_CAPACITY)
				return nullptr;

			std::unique_ptr<CachedBlock> entry(new (std::nothrow) CachedBlock());
			if (!entry)
				return nullptr;

			CachedBlock* block = entry.get();
			m_cache.push_back(std::move(entry));
			return block;
		};

		if (CachedBlock* block = TakeFreeCacheEntry())
			return block;

		if (CachedBlock* block = append_entry())
			return block;

		// PCSX2 owner: x86/ix86-32/iR5900.cpp::recRecompile() requests
		// recResetRaw() when recPtr reaches recPtrEnd; keep the same whole-cache
		// pressure behavior instead of replacing one arbitrary translated block.
		ResetForCachePressure();
		return TakeFreeCacheEntry();
	}

	bool BlockExecutor::EnsureCodeCache()
	{
		if (m_code_cache)
			return true;

		// Vita VM-domain allocations are rounded to 1 MiB by VitaVM::AllocJitMemory().
		// PCSX2 owner: x86/ix86-32/iR5900.cpp owns one EE code cache and
		// BaseblockEx tracks block entries inside that cache; do the same here
		// instead of allocating a VM block per translated guest block.
		m_code_cache = static_cast<u8*>(VitaVM::AllocJitMemory(EE_CODE_CACHE_CAPACITY));
		m_code_cache_capacity = m_code_cache ? EE_CODE_CACHE_CAPACITY : 0;
		m_code_cache_used = 0;
		return (m_code_cache != nullptr);
	}

	void BlockExecutor::ReleaseCodeCache()
	{
		if (!m_code_cache)
			return;

		VitaVM::FreeJitMemory(m_code_cache);
		m_code_cache = nullptr;
		m_code_cache_capacity = 0;
		m_code_cache_used = 0;
	}

	u8* BlockExecutor::AllocateCodeSlice(size_t capacity, size_t* slice_offset)
	{
		if (!EnsureCodeCache())
			return nullptr;

		const size_t aligned_offset = AlignUp(m_code_cache_used, CODE_CACHE_ALIGNMENT);
		if (capacity > m_code_cache_capacity || aligned_offset > (m_code_cache_capacity - capacity))
			return nullptr;

		if (slice_offset)
			*slice_offset = aligned_offset;

		m_code_cache_used = aligned_offset + capacity;
		return m_code_cache + aligned_offset;
	}

	void BlockExecutor::RewindCodeCache(size_t slice_offset)
	{
		if (slice_offset <= m_code_cache_used)
			m_code_cache_used = slice_offset;
	}

	void BlockExecutor::CommitCodeSlice(size_t slice_offset, size_t code_size)
	{
		if (slice_offset > m_code_cache_used || code_size > m_code_cache_used - slice_offset)
			return;

		const size_t committed_size = AlignUp(code_size, CODE_CACHE_ALIGNMENT);
		if (committed_size > m_code_cache_used - slice_offset)
			return;

		m_code_cache_used = slice_offset + committed_size;
	}

	u32 BlockExecutor::ResetForCachePressure()
	{
		const u32 previous_resets = m_code_cache_resets;
		const u32 invalidated = Reset();
		m_code_cache_resets = previous_resets + 1;
		return invalidated;
	}

	bool BlockExecutor::CompileIntoCacheEntry(CachedBlock& block, u32 start_pc, u32 instruction_count, u32* scaled_cycles)
	{
		if (instruction_count == 0 ||
			instruction_count > MAX_STRAIGHT_LINE_BLOCK_INSTRUCTIONS ||
			instruction_count > ((UINT32_MAX - start_pc) / 4))
		{
			return false;
		}
		if (m_persistent_dispatch_enabled && !EnsurePersistentDispatcher())
			return false;

		const void* direct_exit = m_persistent_dispatch_enabled ?
			m_persistent_direct_exit : reinterpret_cast<const void*>(&VitaEeA32DirectExit);
		const void* event_exit = m_persistent_dispatch_enabled ?
			m_persistent_event_exit : reinterpret_cast<const void*>(&VitaEeA32EventExit);

		InvalidateCachedBlock(block);

		for (u32 i = 0; i < instruction_count; i++)
		{
			const u32 op = memRead32(start_pc + i * 4);
			if (!BlockCompiler::CanCompileOpcode(op))
			{
				return false;
			}
			block.opcodes[i] = op;
		}

		size_t block_code_capacity = STRAIGHT_LINE_BLOCK_CODE_CAPACITY;
		size_t block_code_slice_offset = 0;
		u32 compiled_scaled_cycles = 0;
		size_t compiled_linked_entry_offset = 0;
		size_t compiled_resident_self_link_entry_offset = static_cast<size_t>(-1);
		u8 compiled_resident_self_link_entry_loads = 0;
		GprLinkSignature compiled_gpr_link_signature{};
		AnalyzeGprLinkSignature(start_pc, instruction_count, &compiled_gpr_link_signature);
		size_t compiled_compatible_link_entry_offset = static_cast<size_t>(-1);
		u8 compiled_compatible_link_entry_loads = 0;
		CompatibleVtlbFastEntryOffsets compiled_compatible_vtlb_fast_entries{};
		DirectLinkSlots direct_links;
#if defined(VITASX2_QEMU_VALIDATION)
		const auto report_compile_failure = [start_pc, instruction_count](size_t code_size, size_t code_capacity) {
			std::printf("a32-block-compile-failed pc=%08x instructions=%u code=%zu capacity=%zu\n",
				start_pc, instruction_count, code_size, code_capacity);
		};
#endif
		for (;;)
		{
			size_t code_slice_offset = 0;
			u8* code_slice = AllocateCodeSlice(block_code_capacity, &code_slice_offset);
			if (!code_slice)
			{
				ResetForCachePressure();
				code_slice = AllocateCodeSlice(block_code_capacity, &code_slice_offset);
				if (!code_slice)
					return false;
			}

			if (!block.code.Attach(code_slice, block_code_capacity))
			{
				RewindCodeCache(code_slice_offset);
				return false;
			}

			BlockCompiler compiler(block.code);
#if defined(VITASX2_QEMU_VALIDATION)
			compiler.SetVtlbLinkedEntryPcPublicationEnabled(
				m_vtlb_linked_entry_pc_publication_enabled);
			compiler.SetCompatibleLikelyTakenSuffixEnabled(
				m_compatible_likely_taken_suffix_enabled);
			compiler.SetCompatiblePredicateEntryVariantEnabled(
				m_compatible_predicate_entry_variant_enabled);
			compiler.SetEmbeddedCompatibleContinuationEnabled(
				m_embedded_compatible_continuation_enabled);
			compiler.SetFusedDirectEventLinkEnabled(
				m_fused_direct_event_link_enabled);
			compiler.SetCombinedCompatibleTakenEventEnabled(
				m_combined_compatible_taken_event_enabled);
			compiler.SetCompatibleVtlbWriteGuardHoistEnabled(
				m_compatible_vtlb_write_guard_hoist_enabled);
			compiler.SetCompatibleVtlbReadGuardHoistEnabled(
				m_compatible_vtlb_read_guard_hoist_enabled);
			compiler.SetDirectLinkRejectionProfilingEnabled(
				m_direct_link_rejection_profile_enabled);
#endif
			u32 attempt_scaled_cycles = 0;
			size_t attempt_linked_entry_offset = 0;
			size_t attempt_resident_self_link_entry_offset = static_cast<size_t>(-1);
			u8 attempt_resident_self_link_entry_loads = 0;
			size_t attempt_compatible_link_entry_offset = static_cast<size_t>(-1);
			u8 attempt_compatible_link_entry_loads = 0;
			CompatibleVtlbFastEntryOffsets attempt_compatible_vtlb_fast_entries{};
			DirectLinkSlots attempt_direct_links;
			const bool compiled = compiler.CompileStraightLineBlock(start_pc, instruction_count,
				direct_exit, event_exit, &attempt_scaled_cycles, &attempt_direct_links,
				&m_active_generated_lookup_pages, &m_direct_linking_enabled, &attempt_linked_entry_offset,
				m_persistent_dispatch_enabled, &attempt_resident_self_link_entry_offset,
				&attempt_resident_self_link_entry_loads, &compiled_gpr_link_signature,
				&attempt_compatible_link_entry_offset, &attempt_compatible_link_entry_loads,
				&attempt_compatible_vtlb_fast_entries);
			const bool out_of_block_space = !compiled && block.code.Size() >= block.code.Capacity();
			const size_t failure_code_size = block.code.Size();
			const size_t failure_code_capacity = block.code.Capacity();
			if (compiled && block.code.Flush())
			{
				block_code_slice_offset = code_slice_offset;
				CommitCodeSlice(code_slice_offset, block.code.Size());
				compiled_scaled_cycles = attempt_scaled_cycles;
				compiled_linked_entry_offset = attempt_linked_entry_offset;
				compiled_resident_self_link_entry_offset = attempt_resident_self_link_entry_offset;
				compiled_resident_self_link_entry_loads = attempt_resident_self_link_entry_loads;
				compiled_compatible_link_entry_offset = attempt_compatible_link_entry_offset;
				compiled_compatible_link_entry_loads = attempt_compatible_link_entry_loads;
				compiled_compatible_vtlb_fast_entries =
					attempt_compatible_vtlb_fast_entries;
				direct_links = attempt_direct_links;
				break;
			}

			block.code.Release();
			RewindCodeCache(code_slice_offset);
			if (!out_of_block_space || block_code_capacity >= MAX_STRAIGHT_LINE_BLOCK_CODE_CAPACITY)
			{
#if defined(VITASX2_QEMU_VALIDATION)
				report_compile_failure(failure_code_size, failure_code_capacity);
#endif
				return false;
			}

			block_code_capacity *= 2;
		}

		block.start_pc = start_pc;
		block.instruction_count = instruction_count;
		block.scaled_cycles = compiled_scaled_cycles;
		block.ee_cycle_rate = EmuConfig.Speedhacks.EECycleRate;
		block.cp0_config_cycle_shift = static_cast<u8>((cpuRegs.CP0.n.Config >> 18) & 0x1);
		block.linked_entry_offset = compiled_linked_entry_offset;
		block.resident_self_link_entry_offset = compiled_resident_self_link_entry_offset;
		block.resident_self_link_entry_loads = compiled_resident_self_link_entry_loads;
		block.gpr_link_signature = compiled_gpr_link_signature;
		block.compatible_link_entry_offset = compiled_compatible_link_entry_offset;
		block.compatible_link_entry_loads = compiled_compatible_link_entry_loads;
		block.compatible_vtlb_fast_entries = compiled_compatible_vtlb_fast_entries;
		block.direct_links = direct_links;
		block.valid = true;
		if (!RegisterBlockRecord(block))
		{
			block.valid = false;
			block.direct_links = {};
			block.code.Release();
			RewindCodeCache(block_code_slice_offset);
			return false;
		}
		RegisterBlockLookup(block);
		RegisterIncomingLinks(block);

		if (m_direct_linking_enabled)
		{
			PatchIncomingLinks(block);
			for (DirectLinkSlot& link : block.direct_links.slots)
			{
				if (link.valid)
				{
					if (CachedBlock* target = FindCachedBlockByStartPc(link.target_pc, false))
						PatchDirectLink(block, link, target);
				}
			}
		}

		if (scaled_cycles)
			*scaled_cycles = compiled_scaled_cycles;

		return true;
	}

	const void* BlockExecutor::LinkedEntryPoint(const CachedBlock& block) const
	{
		if (!block.code.EntryPoint() || block.linked_entry_offset >= block.code.Size())
			return block.code.EntryPoint();

		return static_cast<const u8*>(block.code.EntryPoint()) + block.linked_entry_offset;
	}

	const void* BlockExecutor::ResidentSelfLinkEntryPoint(const CachedBlock& block) const
	{
		if (!block.code.EntryPoint() || block.resident_self_link_entry_loads == 0 ||
			block.resident_self_link_entry_offset >= block.code.Size())
		{
			return LinkedEntryPoint(block);
		}

		return static_cast<const u8*>(block.code.EntryPoint()) +
			block.resident_self_link_entry_offset;
	}

	const void* BlockExecutor::CompatibleLinkEntryPoint(const CachedBlock& block) const
	{
		if (!block.code.EntryPoint() || !block.gpr_link_signature.IsValid() ||
			block.compatible_link_entry_offset == static_cast<size_t>(-1) ||
			block.compatible_link_entry_offset >= block.code.Size())
		{
			return LinkedEntryPoint(block);
		}

		return static_cast<const u8*>(block.code.EntryPoint()) +
			block.compatible_link_entry_offset;
	}

	const void* BlockExecutor::CompatibleVtlbFastEntryPoint(
		const CachedBlock& block, CompatibleVtlbGuardKind kind) const
	{
		const size_t offset = block.compatible_vtlb_fast_entries.For(kind);
		if (!block.code.EntryPoint() || offset == static_cast<size_t>(-1) ||
			offset >= block.code.Size())
		{
			return CompatibleLinkEntryPoint(block);
		}

		return static_cast<const u8*>(block.code.EntryPoint()) + offset;
	}

	bool BlockExecutor::PatchDirectLink(CachedBlock& block, DirectLinkSlot& link, CachedBlock* target)
	{
		if (!block.valid || !link.valid ||
			link.target_offset == static_cast<size_t>(-1) ||
			link.fallback_offset == static_cast<size_t>(-1))
		{
			return false;
		}

		const void* direct_exit = m_persistent_dispatch_enabled ?
			m_persistent_direct_exit : reinterpret_cast<const void*>(&VitaEeA32DirectExit);
		// PCSX2 x86/BaseblockEx.cpp::BaseBlocks::Link() owns reversible target-PC
		// patch sites. Exact self-edges retain their richer private state; other
		// links may skip canonical GPR loads and dirty publication only when both
		// blocks publish the exact same width/host/state/representation/provenance
		// contract. A dirty incompatible edge stays on its generated writeback
		// fallback instead of jumping directly to a canonical target.
		const bool use_resident_entry = target && m_persistent_dispatch_enabled &&
			link.target_pc == block.start_pc && block.resident_self_link_entry_loads != 0;
		const bool use_compatible_entry = target && !use_resident_entry &&
			m_persistent_dispatch_enabled && block.gpr_link_signature.IsValid() &&
			block.gpr_link_signature == target->gpr_link_signature &&
			target->compatible_link_entry_offset != static_cast<size_t>(-1) &&
			target->compatible_link_entry_offset < target->code.Size();
		const bool embedded_source_matches =
			link.embedded_compatible_continuation &&
			memRead32(link.target_pc) == link.embedded_source_opcodes[0] &&
			memRead32(link.target_pc + sizeof(u32)) == link.embedded_source_opcodes[1];
		const bool use_embedded_continuation =
			use_compatible_entry && embedded_source_matches;
		const CompatibleVtlbGuardKind prevalidated_vtlb_guard =
			(link.prevalidated_vtlb_read_pointer !=
				link.prevalidated_vtlb_write_pointer) ?
				(link.prevalidated_vtlb_read_pointer ? CompatibleVtlbGuardKind::Read :
					CompatibleVtlbGuardKind::Write) :
				CompatibleVtlbGuardKind::None;
		const size_t prevalidated_vtlb_entry_offset = target ?
			target->compatible_vtlb_fast_entries.For(prevalidated_vtlb_guard) :
			static_cast<size_t>(-1);
		const bool use_prevalidated_vtlb_entry = use_compatible_entry &&
			prevalidated_vtlb_guard != CompatibleVtlbGuardKind::None &&
			prevalidated_vtlb_entry_offset != static_cast<size_t>(-1) &&
			prevalidated_vtlb_entry_offset < target->code.Size();
#if defined(VITASX2_QEMU_VALIDATION)
		g_qemuCompatibleVtlbWriteFastEntryActivations +=
			(use_prevalidated_vtlb_entry &&
				prevalidated_vtlb_guard == CompatibleVtlbGuardKind::Write) ? 1u : 0u;
		g_qemuCompatibleVtlbReadFastEntryActivations +=
			(use_prevalidated_vtlb_entry &&
				prevalidated_vtlb_guard == CompatibleVtlbGuardKind::Read) ? 1u : 0u;
#endif
#if defined(VITASX2_QEMU_VALIDATION)
		if (link.embedded_compatible_continuation && target)
		{
			g_qemuEmbeddedCompatibleContinuationActivations += use_embedded_continuation;
			g_qemuEmbeddedCompatibleContinuationSourceMismatches +=
				use_compatible_entry && !embedded_source_matches;
			g_qemuEmbeddedCompatibleContinuationIncompatibleTargets +=
				!use_compatible_entry;
		}
#endif
		const bool use_generated_fallback = !target ||
			(link.requires_compatible_entry &&
				 !use_resident_entry && !use_compatible_entry) ||
			(link.embedded_compatible_continuation && !use_embedded_continuation);
		const void* patched_target = use_resident_entry ? ResidentSelfLinkEntryPoint(block) :
			(use_prevalidated_vtlb_entry ?
				CompatibleVtlbFastEntryPoint(*target, prevalidated_vtlb_guard) :
			 (use_compatible_entry ? CompatibleLinkEntryPoint(*target) :
				(!use_generated_fallback ? LinkedEntryPoint(*target) : direct_exit)));
		const VitaA32::Condition condition = link.branch_if_no_event ?
			VitaA32::Condition::MI : link.branch_on_taken ?
			(link.branch_on_unsigned_less ? VitaA32::Condition::CC : VitaA32::Condition::NE) :
			VitaA32::Condition::AL;
		const bool patched = use_embedded_continuation ?
			block.code.PatchInstruction(link.target_offset,
				link.embedded_active_instruction) :
			(use_generated_fallback ?
				block.code.PatchBranch(link.target_offset, link.fallback_offset, condition) :
				block.code.PatchBranchToAddress(link.target_offset, patched_target, condition));
		const bool secondary_patched =
			link.secondary_target_offset == static_cast<size_t>(-1) ||
			// A failed edge-side pointer check must retain the target's normal
			// translation/handler entry. Invalidation or an incompatible replacement
			// restores the same site to the source block's generated fallback.
			block.code.PatchBranchToAddress(link.secondary_target_offset,
				use_compatible_entry ? CompatibleLinkEntryPoint(*target) :
					static_cast<const u8*>(block.code.EntryPoint()) + link.fallback_offset,
				link.secondary_branch_unconditional ? VitaA32::Condition::AL :
					VitaA32::Condition::NE);
		if (!patched || !secondary_patched || !block.code.Flush())
			return false;

		link.patched_to_resident_entry = !use_generated_fallback && use_resident_entry;
		link.patched_to_compatible_entry = !use_generated_fallback && use_compatible_entry;
		link.embedded_continuation_active = use_embedded_continuation;
		const size_t selected_compatible_entry_offset = use_prevalidated_vtlb_entry ?
			prevalidated_vtlb_entry_offset :
			target ? target->compatible_link_entry_offset : 0;
		link.compatible_entry_instructions = use_compatible_entry ? static_cast<u8>(
			(selected_compatible_entry_offset - target->linked_entry_offset) /
			sizeof(u32)) : 0;
		link.compatible_entry_loads =
			use_compatible_entry ? target->compatible_link_entry_loads : 0;
		return true;
	}

	void BlockExecutor::PatchIncomingLinks(CachedBlock& target)
	{
		if (!m_direct_linking_enabled || !target.valid)
			return;

		s32 index = LastIncomingLinkIndex(target.start_pc);
		while (index >= 0 && m_incoming_links[index].target_pc == target.start_pc)
		{
			IncomingLinkRecord& record = m_incoming_links[index--];
			if (DirectLinkSlot* link = GetRecordedDirectLink(record))
				PatchDirectLink(*record.source, *link, &target);
		}
	}

	void BlockExecutor::UnlinkIncomingLinks(u32 target_pc)
	{
		if (target_pc == UINT32_MAX)
		{
			for (u32 i = 0; i < m_incoming_links.size(); i++)
			{
				IncomingLinkRecord& record = m_incoming_links[i];
				if (DirectLinkSlot* link = GetRecordedDirectLink(record))
					PatchDirectLink(*record.source, *link, nullptr);
			}
			return;
		}

		s32 index = LastIncomingLinkIndex(target_pc);
		while (index >= 0 && m_incoming_links[index].target_pc == target_pc)
		{
			IncomingLinkRecord& record = m_incoming_links[index--];
			if (DirectLinkSlot* link = GetRecordedDirectLink(record))
				PatchDirectLink(*record.source, *link, nullptr);
		}
	}

	void BlockExecutor::RelinkDirectLinks()
	{
		for (u32 i = 0; i < m_incoming_links.size(); i++)
		{
			IncomingLinkRecord& record = m_incoming_links[i];
			DirectLinkSlot* link = GetRecordedDirectLink(record);
			if (!link)
				continue;

			CachedBlock* target = FindCachedBlockByStartPc(record.target_pc, false);
			PatchDirectLink(*record.source, *link, target);
		}
	}

	bool BlockExecutor::RunCachedBlock(CachedBlock& block, bool run_event_test_on_event_exit, BlockExecutionResult* result)
	{
		if (!result || !block.valid || m_persistent_dispatch_enabled)
			return false;

		RefreshRawGpr0KnownZero();
		cpuRegs.pc = block.start_pc;
		const u32 exit_value = reinterpret_cast<GeneratedBlock>(block.code.EntryPoint())();

		BlockExitKind exit = BlockExitKind::Direct;
		if (!DecodeExitKind(exit_value, &exit))
			return false;

		// Mirrors the x86 provider's DispatcherEvent -> recEventTest() path in
		// x86/ix86-32/iR5900.cpp. Bare generated-code smokes can leave this off
		// until the full VM scheduler/device state is initialized.
		if (exit == BlockExitKind::Event && run_event_test_on_event_exit)
			_cpuEventTest_Shared();
		// s_raw_gpr0_known_zero is consumed only by generated EE code. Native LD
		// $zero write seams refresh it in-chain, and the next callable entry always
		// derives it from cpuRegs before executing, so rescanning after the block
		// and again after an event is redundant host work.

		result->path = BlockExecutionPath::Compiled;
		result->exit = exit;
		result->exit_value = exit_value;
		result->instruction_count = block.instruction_count;
		result->scaled_cycles = block.scaled_cycles;
		result->code_size = block.code.Size();
		result->block_records = static_cast<u32>(m_block_records.size());
		result->link_records = static_cast<u32>(m_incoming_links.size());
		result->cache_slots = static_cast<u32>(m_cache.size());
		result->code_cache_resets = m_code_cache_resets;
		result->code_cache_used = m_code_cache_used;
		result->code_cache_capacity = m_code_cache_capacity;
#if defined(VITASX2_QEMU_VALIDATION)
		PopulateFrameEvidence(block.code, m_persistent_dispatch_code, result);
		for (const DirectLinkSlot& link : block.direct_links.slots)
		{
			if (link.valid && link.patched_to_resident_entry)
			{
				result->resident_self_links++;
				result->resident_self_link_entry_instructions += static_cast<u32>(
					(block.resident_self_link_entry_offset - block.linked_entry_offset) /
					sizeof(u32));
				result->resident_self_link_entry_loads += block.resident_self_link_entry_loads;
			}
			if (link.valid && link.patched_to_compatible_entry)
			{
				result->compatible_gpr_links++;
				result->compatible_gpr_link_entry_instructions +=
					link.compatible_entry_instructions;
				result->compatible_gpr_link_entry_loads += link.compatible_entry_loads;
				result->compatible_gpr_words_carried += link.compatible_words;
				result->compatible_gpr_dirty_words_carried += link.compatible_dirty_words;
				if (block.gpr_link_signature.block_count >
					result->compatible_gpr_chain_blocks)
				{
					result->compatible_gpr_chain_blocks =
						block.gpr_link_signature.block_count;
				}
				result->compatible_scheduler_links +=
					link.compatible_scheduler_countdown ? 1u : 0u;
				result->compatible_vtlb_pointer_links +=
					link.compatible_vtlb_pointer ? 1u : 0u;
				result->embedded_compatible_continuations +=
					link.embedded_continuation_active ? 1u : 0u;
			}
		}
#endif
		return true;
	}

	bool BlockExecutor::ExecuteCompiledBlock(u32 start_pc, u32 instruction_count,
		bool run_event_test_on_event_exit, BlockExecutionResult* result)
	{
		if (!result || m_persistent_dispatch_enabled || instruction_count == 0 ||
			instruction_count > MAX_STRAIGHT_LINE_BLOCK_INSTRUCTIONS ||
			instruction_count > ((UINT32_MAX - start_pc) / 4))
		{
			return false;
		}

		*result = {};

		CachedBlock* block = nullptr;
		bool lookup_hit = false;
		if (FindCachedBlock(start_pc, instruction_count, &block, &lookup_hit))
		{
			result->cache_hit = true;
			result->lookup_hit = lookup_hit;
			return RunCachedBlock(*block, run_event_test_on_event_exit, result);
		}

		block = AllocateCacheEntry();
		if (!block || !CompileIntoCacheEntry(*block, start_pc, instruction_count, &result->scaled_cycles))
			return false;

		result->cache_hit = false;
		result->lookup_hit = false;
		return RunCachedBlock(*block, run_event_test_on_event_exit, result);
	}

	bool BlockExecutor::PrepareCompiledBlockAtPc(u32 start_pc, CachedBlock** block,
		BlockExecutionResult* result)
	{
		if (!block || !result || (start_pc & 0x3u) != 0)
			return false;

		*block = nullptr;
		*result = {};
		const auto finish = [&](CachedBlock* entry, bool lookup_hit, bool fast_dispatch_hit,
			bool cache_hit) {
			*block = entry;
			result->path = BlockExecutionPath::Compiled;
			result->instruction_count = entry->instruction_count;
			result->scaled_cycles = entry->scaled_cycles;
			result->code_size = entry->code.Size();
			result->block_records = static_cast<u32>(m_block_records.size());
			result->link_records = static_cast<u32>(m_incoming_links.size());
			result->cache_slots = static_cast<u32>(m_cache.size());
			result->code_cache_resets = m_code_cache_resets;
			result->code_cache_used = m_code_cache_used;
			result->code_cache_capacity = m_code_cache_capacity;
			result->cache_hit = cache_hit;
			result->lookup_hit = lookup_hit;
			result->fast_dispatch_hit = fast_dispatch_hit;
#if defined(VITASX2_QEMU_VALIDATION)
			PopulateFrameEvidence(entry->code, m_persistent_dispatch_code, result);
			for (const DirectLinkSlot& link : entry->direct_links.slots)
			{
				if (link.valid && link.patched_to_resident_entry)
				{
					result->resident_self_links++;
					result->resident_self_link_entry_instructions += static_cast<u32>(
						(entry->resident_self_link_entry_offset - entry->linked_entry_offset) /
						sizeof(u32));
					result->resident_self_link_entry_loads +=
						entry->resident_self_link_entry_loads;
				}
				if (link.valid && link.patched_to_compatible_entry)
				{
					result->compatible_gpr_links++;
					result->compatible_gpr_link_entry_instructions +=
						link.compatible_entry_instructions;
					result->compatible_gpr_link_entry_loads += link.compatible_entry_loads;
					result->compatible_gpr_words_carried += link.compatible_words;
					result->compatible_gpr_dirty_words_carried += link.compatible_dirty_words;
					if (entry->gpr_link_signature.block_count >
						result->compatible_gpr_chain_blocks)
					{
						result->compatible_gpr_chain_blocks =
							entry->gpr_link_signature.block_count;
					}
					result->compatible_scheduler_links +=
						link.compatible_scheduler_countdown ? 1u : 0u;
					result->compatible_vtlb_pointer_links +=
						link.compatible_vtlb_pointer ? 1u : 0u;
					result->embedded_compatible_continuations +=
						link.embedded_continuation_active ? 1u : 0u;
				}
			}
#endif
			return true;
		};

		// PCSX2 owner: x86/BaseblockEx.h::PC_GETBLOCK_() looks up the
		// translated BaseBlock by guest PC before doing any decode work. Once
		// linking is enabled, generated transitions trust recClear() invalidation;
		// pre-ELF/non-linked runs retain source-word validation.
		const bool validate_source_words = !m_direct_linking_enabled;
		if (CachedBlock* entry = FindLookupBlockByStartPc(start_pc))
		{
			if (entry->valid && ValidateCachedBlock(*entry, validate_source_words))
				return finish(entry, true, true, true);
		}

		if (CachedBlock* entry = FindRecordedBlockByStartPc(start_pc, 0, false, validate_source_words))
			return finish(entry, false, true, true);

		BlockScanResult scan;
		if (!ScanStraightLineBlock(start_pc, MAX_STRAIGHT_LINE_BLOCK_INSTRUCTIONS, &scan) ||
			scan.instruction_count == 0)
		{
			return false;
		}

		CachedBlock* entry = AllocateCacheEntry();
		if (!entry || !CompileIntoCacheEntry(*entry, start_pc, scan.instruction_count,
				&result->scaled_cycles))
		{
			return false;
		}

		return finish(entry, false, false, false);
	}

	bool BlockExecutor::ExecuteCompiledBlockAtPc(u32 start_pc, bool run_event_test_on_event_exit,
		BlockExecutionResult* result)
	{
		if (m_persistent_dispatch_enabled)
			return false;

		CachedBlock* block = nullptr;
		if (!PrepareCompiledBlockAtPc(start_pc, &block, result))
			return false;
		return RunCachedBlock(*block, run_event_test_on_event_exit, result);
	}

	const void* BlockExecutor::PersistentDispatchThunk(u32 exit_value, void* userdata)
	{
		PersistentRunContext* context = static_cast<PersistentRunContext*>(userdata);
		if (!context || !context->executor || !context->current_block ||
			!context->final_result)
		{
			return nullptr;
		}

		BlockExitKind exit = BlockExitKind::Direct;
		if (!DecodeExitKind(exit_value, &exit))
		{
			context->failed = true;
			return nullptr;
		}

#if defined(VITASX2_QEMU_VALIDATION)
		context->executor->RecordPersistentExit(exit);
#endif

		context->current_result.exit = exit;
		context->current_result.exit_value = exit_value;
		*context->final_result = context->current_result;

		// PCSX2 owner: _DynGen_DispatcherEvent() calls recEventTest() without
		// unwinding the private JIT frame, then resumes the main dispatcher.
		if (exit == BlockExitKind::Event && context->run_event_test_on_event_exit)
			_cpuEventTest_Shared();

		if (!context->boundary_callback ||
			!context->boundary_callback(context->callback_userdata, context->current_result))
		{
			return nullptr;
		}

		BlockExecutionResult next_result;
		CachedBlock* next_block = nullptr;
		if (!context->executor->PrepareCompiledBlockAtPc(cpuRegs.pc, &next_block, &next_result))
		{
			context->failed = true;
			return nullptr;
		}

		context->current_block = next_block;
		context->current_result = next_result;
		RefreshRawGpr0KnownZero();
		return context->executor->LinkedEntryPoint(*next_block);
	}

	bool BlockExecutor::ExecutePersistentAtPc(u32 start_pc,
		bool run_event_test_on_event_exit, PersistentBoundaryCallback boundary_callback,
		void* callback_userdata, BlockExecutionResult* result)
	{
		if (!result || !m_persistent_dispatch_enabled || !EnsurePersistentDispatcher())
			return false;

		CachedBlock* block = nullptr;
		BlockExecutionResult initial_result;
		if (!PrepareCompiledBlockAtPc(start_pc, &block, &initial_result))
			return false;

		PersistentRunContext context;
		context.executor = this;
		context.current_block = block;
		context.current_result = initial_result;
		context.final_result = result;
		context.boundary_callback = boundary_callback;
		context.callback_userdata = callback_userdata;
		context.run_event_test_on_event_exit = run_event_test_on_event_exit;

		RefreshRawGpr0KnownZero();
		cpuRegs.pc = start_pc;
		const u32 exit_value = reinterpret_cast<PersistentDispatcher>(m_persistent_dispatch_entry)(
			LinkedEntryPoint(*block), &context,
			reinterpret_cast<const void*>(&BlockExecutor::PersistentDispatchThunk));
		if (context.failed)
			return false;

		BlockExitKind exit = BlockExitKind::Direct;
		return DecodeExitKind(exit_value, &exit);
	}

	bool BlockExecutor::ExecuteStraightLineBlockOrInterpreterStep(u32 start_pc, u32 instruction_count,
		bool run_event_test_on_event_exit, BlockExecutionResult* result)
	{
		if (!result || instruction_count == 0 ||
			instruction_count > MAX_STRAIGHT_LINE_BLOCK_INSTRUCTIONS ||
			instruction_count > ((UINT32_MAX - start_pc) / 4))
		{
			return false;
		}

		for (u32 i = 0; i < instruction_count; i++)
		{
			const u32 op = memRead32(start_pc + i * 4);
			if (!BlockCompiler::CanCompileOpcode(op))
			{
				cpuRegs.pc = start_pc;
				intCpu.Step(); // PCSX2 interpreter owner: Interpreter.cpp::execI().
				RefreshRawGpr0KnownZero();

				*result = {};
				result->path = BlockExecutionPath::InterpreterStep;
				result->exit = BlockExitKind::InterpreterStep;
				result->exit_value = static_cast<u32>(BlockExitKind::InterpreterStep);
				result->instruction_count = 1;
				return true;
			}
		}

		return ExecuteCompiledBlock(start_pc, instruction_count, run_event_test_on_event_exit, result);
	}
} // namespace VitaEE
