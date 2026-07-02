// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "pcsx2/vita/VitaEeExecutor.h"

#include "pcsx2/Config.h"
#include "pcsx2/Memory.h"
#include "pcsx2/R5900.h"
#include "pcsx2/vita/VitaEeBlockCompiler.h"

#include <new>

namespace
{
	using GeneratedBlock = u32 (*)();

	constexpr size_t STRAIGHT_LINE_BLOCK_CODE_CAPACITY = 4096;

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
} // namespace

namespace VitaEE
{
	BlockExecutor::~BlockExecutor()
	{
		Reset();
		ReleaseLookupPages();
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

	u32 BlockExecutor::Reset()
	{
		u32 invalidated = 0;
		for (CachedBlock& block : m_cache)
		{
			if (block.valid)
				invalidated++;

			block.code.Release();
			block.valid = false;
			block.direct_links = {};
		}

		ReleaseLookupPages();
		m_next_victim = 0;
		return invalidated;
	}

	u32 BlockExecutor::InvalidateRange(u32 start_pc, u32 instruction_count)
	{
		if (instruction_count == 0 || instruction_count > ((UINT32_MAX - start_pc) / 4))
			return 0;

		const u32 end_pc = start_pc + instruction_count * 4;
		u32 invalidated = 0;

		for (CachedBlock& block : m_cache)
		{
			if (!block.valid)
				continue;

			const u32 block_end = block.start_pc + block.instruction_count * 4;
			if (block.start_pc < end_pc && start_pc < block_end)
			{
				UnlinkIncomingLinks(block.start_pc);
				UnregisterBlockLookup(block);
				block.valid = false;
				block.direct_links = {};
				invalidated++;
			}
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
		UnlinkIncomingLinks(block.start_pc);
		UnregisterBlockLookup(block);
		block.valid = false;
		block.direct_links = {};
		return false;
	}

	void BlockExecutor::ValidateCachedBlocks()
	{
		for (CachedBlock& block : m_cache)
			ValidateCachedBlock(block);
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

		for (CachedBlock& entry : m_cache)
		{
			if (!entry.valid || entry.start_pc != start_pc || entry.instruction_count != instruction_count)
				continue;

			if (ValidateCachedBlock(entry))
			{
				*block = &entry;
				return true;
			}
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

		for (CachedBlock& entry : m_cache)
		{
			if (entry.valid && entry.start_pc == start_pc && ValidateCachedBlock(entry))
				return &entry;
		}

		return nullptr;
	}

	BlockExecutor::CachedBlock* BlockExecutor::AllocateCacheEntry()
	{
		for (CachedBlock& entry : m_cache)
		{
			if (!entry.valid)
				return &entry;
		}

		CachedBlock& victim = m_cache[m_next_victim];
		m_next_victim = (m_next_victim + 1) % m_cache.size();
		if (victim.valid)
		{
			UnlinkIncomingLinks(victim.start_pc);
			UnregisterBlockLookup(victim);
		}
		victim.valid = false;
		victim.direct_links = {};
		return &victim;
	}

	bool BlockExecutor::CompileIntoCacheEntry(CachedBlock& block, u32 start_pc, u32 instruction_count, u32* scaled_cycles)
	{
		if (instruction_count == 0 ||
			instruction_count > MAX_STRAIGHT_LINE_BLOCK_INSTRUCTIONS ||
			instruction_count > ((UINT32_MAX - start_pc) / 4))
		{
			return false;
		}

		block.valid = false;

		for (u32 i = 0; i < instruction_count; i++)
		{
			const u32 op = memRead32(start_pc + i * 4);
			if (!BlockCompiler::CanCompileOpcode(op))
				return false;
			block.opcodes[i] = op;
		}

		if (!block.code.Data())
		{
			if (!block.code.Allocate(STRAIGHT_LINE_BLOCK_CODE_CAPACITY))
				return false;
		}
		else
		{
			block.code.Reset();
		}

		BlockCompiler compiler(block.code);
		u32 compiled_scaled_cycles = 0;
		DirectLinkSlots direct_links;
		if (!compiler.CompileStraightLineBlock(start_pc, instruction_count,
				reinterpret_cast<const void*>(&VitaEeA32DirectExit),
				reinterpret_cast<const void*>(&VitaEeA32EventExit), &compiled_scaled_cycles, &direct_links) ||
			!block.code.Flush())
		{
			return false;
		}

		block.start_pc = start_pc;
		block.instruction_count = instruction_count;
		block.scaled_cycles = compiled_scaled_cycles;
		block.ee_cycle_rate = EmuConfig.Speedhacks.EECycleRate;
		block.cp0_config_cycle_shift = static_cast<u8>((cpuRegs.CP0.n.Config >> 18) & 0x1);
		block.direct_links = direct_links;
		block.valid = true;
		RegisterBlockLookup(block);

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

		for (CachedBlock& block : m_cache)
		{
			if (!block.valid)
				continue;

			for (DirectLinkSlot& link : block.direct_links.slots)
			{
				if (link.valid && link.target_pc == target_pc)
					PatchDirectLink(block, link, target);
			}
		}
	}

	void BlockExecutor::UnlinkIncomingLinks(u32 target_pc)
	{
		for (CachedBlock& block : m_cache)
		{
			if (!block.valid)
				continue;

			for (DirectLinkSlot& link : block.direct_links.slots)
			{
				if (link.valid && (target_pc == UINT32_MAX || link.target_pc == target_pc))
					PatchDirectLink(block, link, reinterpret_cast<const void*>(&VitaEeA32DirectExit));
			}
		}
	}

	void BlockExecutor::RelinkDirectLinks()
	{
		for (CachedBlock& block : m_cache)
		{
			if (!block.valid)
				continue;

			for (DirectLinkSlot& link : block.direct_links.slots)
			{
				if (!link.valid)
					continue;

				const CachedBlock* target = FindCachedBlockByStartPc(link.target_pc);
				PatchDirectLink(block, link, target ? target->code.EntryPoint() :
													  reinterpret_cast<const void*>(&VitaEeA32DirectExit));
			}
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
