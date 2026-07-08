// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "pcsx2/vita/VitaEeExecutor.h"

#include "common/Vita/VitaJitMemory.h"
#include "pcsx2/Config.h"
#include "pcsx2/Memory.h"
#include "pcsx2/R5900.h"
#include "pcsx2/vita/VitaEeBlockCompiler.h"

#include <cstring>
#include <new>

namespace
{
	using GeneratedBlock = u32 (*)();

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
		Reset();
		ReleaseLookupPages();
		ReleaseGeneratedLookupPages();
		ReleaseCodeCache();
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
			block.direct_links = {};
			RememberFreeCacheEntry(block);
		}

		ClearBlockRecords();
		ClearIncomingLinks();
		ReleaseLookupPages();
		ReleaseGeneratedLookupPages();
		m_code_cache_resets = 0;
		ReleaseCodeCache();
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

	bool BlockExecutor::ScanStraightLineBlock(u32 start_pc, u32 max_instruction_count, BlockScanResult* result)
	{
		if (!result || max_instruction_count == 0)
			return false;

		*result = {};
		result->start_pc = start_pc;
		result->stop_pc = start_pc;
		result->stop = BlockScanStop::MaxInstructions;

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
				if ((delay_pc & 0xffcu) == 0)
				{
					result->stop = BlockScanStop::PageBoundary;
					return true;
				}

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
			if (BlockCompiler::RequiresBlockEndAfterOpcode(op))
			{
				result->stop = BlockScanStop::OpcodeBoundary;
				return true;
			}
		}

		return true;
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
			// and protected-page invalidation. Vita blocks are page-bounded by
			// ScanStraightLineBlock(), so a raw vmap pointer compare replaces
			// the old per-opcode vtlb read loop on normal RAM/ROM/scratchpad
			// dispatcher hits; handler-backed pages keep the exact memRead32()
			// fallback.
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
			u32 attempt_scaled_cycles = 0;
			size_t attempt_linked_entry_offset = 0;
			DirectLinkSlots attempt_direct_links;
			const bool compiled = compiler.CompileStraightLineBlock(start_pc, instruction_count,
				reinterpret_cast<const void*>(&VitaEeA32DirectExit),
				reinterpret_cast<const void*>(&VitaEeA32EventExit), &attempt_scaled_cycles, &attempt_direct_links,
				&m_active_generated_lookup_pages, &m_direct_linking_enabled, &attempt_linked_entry_offset);
			const bool out_of_block_space = !compiled && block.code.Size() >= block.code.Capacity();
			const size_t failure_code_size = block.code.Size();
			const size_t failure_code_capacity = block.code.Capacity();
			if (compiled && block.code.Flush())
			{
				block_code_slice_offset = code_slice_offset;
				CommitCodeSlice(code_slice_offset, block.code.Size());
				compiled_scaled_cycles = attempt_scaled_cycles;
				compiled_linked_entry_offset = attempt_linked_entry_offset;
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
			PatchIncomingLinks(block.start_pc, LinkedEntryPoint(block));
			for (DirectLinkSlot& link : block.direct_links.slots)
			{
				if (link.valid)
				{
					if (CachedBlock* target = FindCachedBlockByStartPc(link.target_pc, false))
						PatchDirectLink(block, link, LinkedEntryPoint(*target));
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

	bool BlockExecutor::PatchDirectLink(CachedBlock& block, DirectLinkSlot& link, const void* target)
	{
		if (!target || !block.valid || !link.valid ||
			link.target_offset == static_cast<size_t>(-1) ||
			link.fallback_offset == static_cast<size_t>(-1))
		{
			return false;
		}

		const bool target_is_direct_exit =
			target == reinterpret_cast<const void*>(&VitaEeA32DirectExit);
		const bool patched = target_is_direct_exit ?
			block.code.PatchBranch(link.target_offset, link.fallback_offset) :
			block.code.PatchBranchToAddress(link.target_offset, target);
		return patched && block.code.Flush();
	}

	void BlockExecutor::PatchIncomingLinks(u32 target_pc, const void* target)
	{
		if (!m_direct_linking_enabled || !target)
			return;

		s32 index = LastIncomingLinkIndex(target_pc);
		while (index >= 0 && m_incoming_links[index].target_pc == target_pc)
		{
			IncomingLinkRecord& record = m_incoming_links[index--];
			if (DirectLinkSlot* link = GetRecordedDirectLink(record))
				PatchDirectLink(*record.source, *link, target);
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
					PatchDirectLink(*record.source, *link, reinterpret_cast<const void*>(&VitaEeA32DirectExit));
			}
			return;
		}

		s32 index = LastIncomingLinkIndex(target_pc);
		while (index >= 0 && m_incoming_links[index].target_pc == target_pc)
		{
			IncomingLinkRecord& record = m_incoming_links[index--];
			if (DirectLinkSlot* link = GetRecordedDirectLink(record))
				PatchDirectLink(*record.source, *link, reinterpret_cast<const void*>(&VitaEeA32DirectExit));
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

			const CachedBlock* target = FindCachedBlockByStartPc(record.target_pc, false);
			PatchDirectLink(*record.source, *link, target ? LinkedEntryPoint(*target) :
															reinterpret_cast<const void*>(&VitaEeA32DirectExit));
		}
	}

	bool BlockExecutor::RunCachedBlock(CachedBlock& block, bool run_event_test_on_event_exit, BlockExecutionResult* result)
	{
		if (!result || !block.valid)
			return false;

		RefreshRawGpr0KnownZero();
		cpuRegs.pc = block.start_pc;
		const u32 exit_value = reinterpret_cast<GeneratedBlock>(block.code.EntryPoint())();
		RefreshRawGpr0KnownZero();

		BlockExitKind exit = BlockExitKind::Direct;
		if (!DecodeExitKind(exit_value, &exit))
			return false;

		// Mirrors the x86 provider's DispatcherEvent -> recEventTest() path in
		// x86/ix86-32/iR5900.cpp. Bare generated-code smokes can leave this off
		// until the full VM scheduler/device state is initialized.
		if (exit == BlockExitKind::Event && run_event_test_on_event_exit)
			_cpuEventTest_Shared();
		RefreshRawGpr0KnownZero();

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
		return true;
	}

	bool BlockExecutor::ExecuteCompiledBlock(u32 start_pc, u32 instruction_count,
		bool run_event_test_on_event_exit, BlockExecutionResult* result)
	{
		if (!result || instruction_count == 0 ||
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

	bool BlockExecutor::ExecuteCompiledBlockAtPc(u32 start_pc, bool run_event_test_on_event_exit,
		BlockExecutionResult* result)
	{
		if (!result || (start_pc & 0x3u) != 0)
			return false;

		*result = {};

		// PCSX2 owner: x86/BaseblockEx.h::PC_GETBLOCK_() looks up the
		// translated BaseBlock by guest PC before doing any decode work. Keep
		// the Vita EE hot dispatcher on the same shape in non-trace execution:
		// run it immediately, and only scan on misses or SMC invalidation. Once
		// direct/generated-indirect linking is enabled, generated-to-generated
		// transitions already trust recClear()/InvalidateRange(); keep the C++
		// re-entry path on the same policy and avoid an opcode-window memcmp on
		// every JR/JALR return. Pre-ELF/non-linked runs keep source validation.
		const bool validate_source_words = !m_direct_linking_enabled;
		if (CachedBlock* entry = FindLookupBlockByStartPc(start_pc))
		{
			if (entry->valid && ValidateCachedBlock(*entry, validate_source_words))
			{
				result->cache_hit = true;
				result->lookup_hit = true;
				result->fast_dispatch_hit = true;
				return RunCachedBlock(*entry, run_event_test_on_event_exit, result);
			}
		}

		if (CachedBlock* entry = FindRecordedBlockByStartPc(start_pc, 0, false, validate_source_words))
		{
			result->cache_hit = true;
			result->fast_dispatch_hit = true;
			return RunCachedBlock(*entry, run_event_test_on_event_exit, result);
		}

		BlockScanResult scan;
		if (!ScanStraightLineBlock(start_pc, MAX_STRAIGHT_LINE_BLOCK_INSTRUCTIONS, &scan) ||
			scan.instruction_count == 0)
		{
			return false;
		}

		return ExecuteCompiledBlock(start_pc, scan.instruction_count, run_event_test_on_event_exit, result);
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
