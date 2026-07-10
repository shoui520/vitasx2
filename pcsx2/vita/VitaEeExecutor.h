// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"
#include "pcsx2/HostMemoryMap.h"
#include "pcsx2/vita/A32Emitter.h"
#include "pcsx2/vita/VitaEeBlockCompiler.h"

#include <array>
#include <cstddef>
#include <memory>
#include <vector>

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
		static bool ScanStraightLineBlock(u32 start_pc, u32 max_instruction_count, BlockScanResult* result);
		bool ExecuteCompiledBlock(u32 start_pc, u32 instruction_count,
			bool run_event_test_on_event_exit, BlockExecutionResult* result);
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
		static constexpr size_t MAX_STRAIGHT_LINE_BLOCK_CODE_CAPACITY = 16 * 1024;
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
			u32 scaled_cycles = 0;
			s8 ee_cycle_rate = 0;
			u8 cp0_config_cycle_shift = 0;
			size_t linked_entry_offset = 0;
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
		bool FindCachedBlock(u32 start_pc, u32 instruction_count, CachedBlock** block, bool* lookup_hit);
		CachedBlock* FindCachedBlockByStartPc(u32 start_pc, bool validate_source_words = true);
		CachedBlock* AllocateCacheEntry();
		bool EnsureCodeCache();
		void ReleaseCodeCache();
		u8* AllocateCodeSlice(size_t capacity, size_t* slice_offset);
		void CommitCodeSlice(size_t slice_offset, size_t code_size);
		void RewindCodeCache(size_t slice_offset);
		u32 ResetForCachePressure();
		bool CompileIntoCacheEntry(CachedBlock& block, u32 start_pc, u32 instruction_count, u32* scaled_cycles);
		bool PrepareCompiledBlockAtPc(u32 start_pc, CachedBlock** block, BlockExecutionResult* result);
		bool RunCachedBlock(CachedBlock& block, bool run_event_test_on_event_exit, BlockExecutionResult* result);
		bool EnsurePersistentDispatcher();
		static const void* PersistentDispatchThunk(u32 exit_value, void* userdata);
		const void* LinkedEntryPoint(const CachedBlock& block) const;
		bool PatchDirectLink(CachedBlock& block, DirectLinkSlot& link, const void* target);
		void PatchIncomingLinks(u32 target_pc, const void* target);
		void UnlinkIncomingLinks(u32 target_pc);
		void RelinkDirectLinks();

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
	};
} // namespace VitaEE
