// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "pcsx2/vita/VitaEeExecutor.h"

#include "common/Vita/VitaJitMemory.h"
#include "pcsx2/Config.h"
#include "pcsx2/Memory.h"
#include "pcsx2/R5900.h"
#include "pcsx2/vita/VitaEeBlockCompiler.h"

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
		m_block_records.reserve(INITIAL_CACHE_CAPACITY);
		m_incoming_links.reserve(INITIAL_CACHE_CAPACITY * DIRECT_LINK_SLOT_COUNT);
	}

	BlockExecutor::~BlockExecutor()
	{
		Reset();
		ReleaseLookupPages();
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

	void BlockExecutor::RegisterBlockLookup(CachedBlock& block)
	{
		if (!block.valid || (block.start_pc & 0x3u) != 0)
			return;

		// PCSX2 owner: x86/BaseblockEx.h::PC_GETBLOCK_()/recLUT_SetPage().
		// Vita keeps the same 64 KiB guest-page lookup granularity, but allocates
		// pages lazily instead of reserving a BASEBLOCK for every possible EE word.
		if (LookupPage* page = GetLookupPage(block.start_pc, true))
			page->blocks[LookupEntryIndex(block.start_pc)] = &block;
	}

	void BlockExecutor::UnregisterBlockLookup(CachedBlock& block)
	{
		if ((block.start_pc & 0x3u) != 0)
			return;

		if (LookupPage* page = GetLookupPage(block.start_pc, false))
		{
			CachedBlock*& entry = page->blocks[LookupEntryIndex(block.start_pc)];
			if (entry == &block)
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
		while (insert_index < m_block_records.size() &&
			   m_block_records[insert_index].start_pc <= block.start_pc)
		{
			insert_index++;
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
		u32 write_index = 0;
		for (u32 read_index = 0; read_index < m_block_records.size(); read_index++)
		{
			if (m_block_records[read_index].block == &block)
				continue;

			if (write_index != read_index)
				m_block_records[write_index] = m_block_records[read_index];
			write_index++;
		}

		m_block_records.resize(write_index);
	}

	void BlockExecutor::ClearBlockRecords()
	{
		m_block_records.clear();
	}

	BlockExecutor::CachedBlock* BlockExecutor::FindRecordedBlockByStartPc(
		u32 start_pc, u32 instruction_count, bool match_instruction_count)
	{
		s32 index = LastBlockRecordIndex(start_pc);
		while (index >= 0 && m_block_records[index].start_pc == start_pc)
		{
			CachedBlock* block = m_block_records[index].block;
			if (block && block->valid &&
				(!match_instruction_count || block->instruction_count == instruction_count))
			{
				if (ValidateCachedBlock(*block))
					return block;

				break;
			}

			index--;
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
		block.direct_links = {};
		block.code.Release();
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

	void BlockExecutor::ClearIncomingLinks()
	{
		m_incoming_links.clear();
	}

	void BlockExecutor::RegisterIncomingLinks(CachedBlock& block)
	{
		UnregisterIncomingLinks(block);

		// PCSX2 owner: x86/BaseblockEx.cpp::BaseBlocks::Link(). The x86
		// provider stores target-PC -> patch-site records so New()/Remove() only
		// touch incoming edges for the affected block.
		for (u8 i = 0; i < DIRECT_LINK_SLOT_COUNT; i++)
		{
			const DirectLinkSlot& link = block.direct_links.slots[i];
			if (!link.valid || m_incoming_links.size() >= MAX_INCOMING_LINKS)
				continue;

			m_incoming_links.push_back({&block, link.target_pc, i});
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
		for (const std::unique_ptr<CachedBlock>& entry : m_cache)
		{
			CachedBlock& block = *entry;
			if (block.valid)
				invalidated++;

			block.code.Release();
			block.valid = false;
			block.direct_links = {};
		}

		ClearBlockRecords();
		ClearIncomingLinks();
		ReleaseLookupPages();
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

		for (u32 i = 0; i < m_block_records.size();)
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
			RelinkDirectLinks();
		else
			UnlinkIncomingLinks(UINT32_MAX);
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

	bool BlockExecutor::ValidateCachedBlock(CachedBlock& block)
	{
		if (!block.valid)
			return false;

		const s8 ee_cycle_rate = EmuConfig.Speedhacks.EECycleRate;
		const u8 cp0_config_cycle_shift = static_cast<u8>((cpuRegs.CP0.n.Config >> 18) & 0x1);
		bool matches = (block.ee_cycle_rate == ee_cycle_rate &&
						block.cp0_config_cycle_shift == cp0_config_cycle_shift);

		for (u32 i = 0; matches && i < block.instruction_count; i++)
			matches = (block.opcodes[i] == memRead32(block.start_pc + i * 4));

		if (matches)
			return true;

		// PCSX2's x86 path combines recRAMCopy with protected-page faults.
		// Vita has no user-mode fault repair, so validate cached opcodes before
		// any direct-linked dispatch can reach the block.
		InvalidateCachedBlock(block);
		return false;
	}

	void BlockExecutor::ValidateCachedBlocks()
	{
		for (const std::unique_ptr<CachedBlock>& block : m_cache)
			ValidateCachedBlock(*block);
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

	BlockExecutor::CachedBlock* BlockExecutor::FindCachedBlockByStartPc(u32 start_pc)
	{
		if (CachedBlock* entry = FindLookupBlockByStartPc(start_pc))
		{
			if (entry->valid && ValidateCachedBlock(*entry))
				return entry;
		}

		return FindRecordedBlockByStartPc(start_pc, 0, false);
	}

	BlockExecutor::CachedBlock* BlockExecutor::AllocateCacheEntry()
	{
		for (const std::unique_ptr<CachedBlock>& entry : m_cache)
		{
			if (!entry->valid)
				return entry.get();
		}

		if (m_cache.size() < MAX_CACHE_CAPACITY)
		{
			std::unique_ptr<CachedBlock> entry(new (std::nothrow) CachedBlock());
			if (!entry)
				return nullptr;

			CachedBlock* block = entry.get();
			m_cache.push_back(std::move(entry));
			return block;
		}

		// PCSX2 owner: x86/ix86-32/iR5900.cpp::recRecompile() requests
		// recResetRaw() when recPtr reaches recPtrEnd; keep the same whole-cache
		// pressure behavior instead of replacing one arbitrary translated block.
		ResetForCachePressure();
		for (const std::unique_ptr<CachedBlock>& entry : m_cache)
		{
			if (!entry->valid)
				return entry.get();
		}

		return nullptr;
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

		size_t code_slice_offset = 0;
		u8* code_slice = AllocateCodeSlice(STRAIGHT_LINE_BLOCK_CODE_CAPACITY, &code_slice_offset);
		if (!code_slice)
		{
			ResetForCachePressure();
			code_slice = AllocateCodeSlice(STRAIGHT_LINE_BLOCK_CODE_CAPACITY, &code_slice_offset);
			if (!code_slice)
				return false;
		}

		if (!block.code.Attach(code_slice, STRAIGHT_LINE_BLOCK_CODE_CAPACITY))
		{
			RewindCodeCache(code_slice_offset);
			return false;
		}

		for (u32 i = 0; i < instruction_count; i++)
		{
			const u32 op = memRead32(start_pc + i * 4);
			if (!BlockCompiler::CanCompileOpcode(op))
			{
				block.code.Release();
				RewindCodeCache(code_slice_offset);
				return false;
			}
			block.opcodes[i] = op;
		}

		BlockCompiler compiler(block.code);
		u32 compiled_scaled_cycles = 0;
		DirectLinkSlots direct_links;
		if (!compiler.CompileStraightLineBlock(start_pc, instruction_count,
				reinterpret_cast<const void*>(&VitaEeA32DirectExit),
				reinterpret_cast<const void*>(&VitaEeA32EventExit), &compiled_scaled_cycles, &direct_links) ||
			!block.code.Flush())
		{
			block.code.Release();
			RewindCodeCache(code_slice_offset);
			return false;
		}

		block.start_pc = start_pc;
		block.instruction_count = instruction_count;
		block.scaled_cycles = compiled_scaled_cycles;
		block.ee_cycle_rate = EmuConfig.Speedhacks.EECycleRate;
		block.cp0_config_cycle_shift = static_cast<u8>((cpuRegs.CP0.n.Config >> 18) & 0x1);
		block.direct_links = direct_links;
		block.valid = true;
		if (!RegisterBlockRecord(block))
		{
			block.valid = false;
			block.direct_links = {};
			block.code.Release();
			RewindCodeCache(code_slice_offset);
			return false;
		}
		RegisterBlockLookup(block);
		RegisterIncomingLinks(block);

		if (m_direct_linking_enabled)
		{
			PatchIncomingLinks(block.start_pc, block.code.EntryPoint());
			for (DirectLinkSlot& link : block.direct_links.slots)
			{
				if (link.valid)
				{
					if (CachedBlock* target = FindCachedBlockByStartPc(link.target_pc))
						PatchDirectLink(block, link, target->code.EntryPoint());
				}
			}
		}

		if (scaled_cycles)
			*scaled_cycles = compiled_scaled_cycles;

		return true;
	}

	bool BlockExecutor::PatchDirectLink(CachedBlock& block, DirectLinkSlot& link, const void* target)
	{
		if (!target || !block.valid || !link.valid)
			return false;

		return block.code.PatchMovImm32(link.target_offset, 12,
				   static_cast<u32>(reinterpret_cast<uptr>(target))) &&
			   block.code.Flush();
	}

	void BlockExecutor::PatchIncomingLinks(u32 target_pc, const void* target)
	{
		if (!m_direct_linking_enabled || !target)
			return;

		for (u32 i = 0; i < m_incoming_links.size(); i++)
		{
			IncomingLinkRecord& record = m_incoming_links[i];
			if (record.target_pc != target_pc)
				continue;

			if (DirectLinkSlot* link = GetRecordedDirectLink(record))
				PatchDirectLink(*record.source, *link, target);
		}
	}

	void BlockExecutor::UnlinkIncomingLinks(u32 target_pc)
	{
		for (u32 i = 0; i < m_incoming_links.size(); i++)
		{
			IncomingLinkRecord& record = m_incoming_links[i];
			if (target_pc != UINT32_MAX && record.target_pc != target_pc)
				continue;

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

			const CachedBlock* target = FindCachedBlockByStartPc(record.target_pc);
			PatchDirectLink(*record.source, *link, target ? target->code.EntryPoint() :
															reinterpret_cast<const void*>(&VitaEeA32DirectExit));
		}
	}

	bool BlockExecutor::RunCachedBlock(CachedBlock& block, bool run_event_test_on_event_exit, BlockExecutionResult* result)
	{
		if (!result || !block.valid)
			return false;

		ValidateCachedBlocks();
		if (!block.valid)
			return false;

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
