// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"
#include "pcsx2/HostMemoryMap.h"
#include "pcsx2/vita/A32Emitter.h"

#include <array>
#include <cstddef>
#include <memory>
#include <vector>

namespace VitaIOP
{
	enum class BlockExitKind : u32
	{
		Direct = 0x10300a32u,
	};

	struct BlockScanResult
	{
		u32 start_pc = 0;
		u32 instruction_count = 0;
		u32 stop_pc = 0;
	};

	struct BlockExecutionResult
	{
		BlockExitKind exit = BlockExitKind::Direct;
		u32 instruction_count = 0;
		u32 native_instruction_count = 0;
		u32 helper_instruction_count = 0;
		size_t code_size = 0;
		u32 block_records = 0;
		u32 link_records = 0;
		u32 cache_slots = 0;
		u32 code_cache_resets = 0;
		size_t code_cache_used = 0;
		size_t code_cache_capacity = 0;
		bool cache_hit = false;
		bool lookup_hit = false;
	};

	struct DirectLinkSlot
	{
		u32 target_pc = 0;
		size_t target_offset = 0;
		bool valid = false;
	};

	struct DirectLinkSlots
	{
		DirectLinkSlot slots[2]{};
	};

	class BlockCompiler
	{
	public:
		explicit BlockCompiler(VitaA32::CodeBuffer& code);

		static bool CanCompileOpcode(u32 op);

		bool CompileStraightLineBlock(u32 start_pc, u32 instruction_count,
			const void* direct_exit = nullptr, DirectLinkSlots* direct_links = nullptr);
		u32 NativeInstructionCount() const { return m_native_instruction_count; }
		u32 HelperInstructionCount() const { return m_helper_instruction_count; }

	private:
		bool BeginBlock();
		bool EndBlockReturn(BlockExitKind exit);
		bool EndBlockDirectTail(const void* direct_exit, size_t* direct_link_target_offset);
		bool EmitInstruction(u32 op, u32 pc, bool store_pc, std::vector<size_t>& direct_exit_branches);
		bool EmitNativeInstruction(u32 op, u32 pc);
		bool EmitNativeSPECIAL(u32 op, u32 pc);
		bool EmitNativeCOP0(u32 op);
		bool EmitNativeCOP2(u32 op);
		bool EmitStoreCode(u32 op);
		bool EmitTraceCheck(u32 pc, u32 op, std::vector<size_t>& direct_exit_branches);
		bool EmitStorePc(u32 pc);
		bool EmitStorePcReg(unsigned host_reg);
		bool EmitAddCycles(u32 cycles);
		bool EmitIncrementCycle();
		bool EmitPcChangedExitCheck(u32 expected_pc, std::vector<size_t>& direct_exit_branches);
		bool EmitPcChangedExitCheckReg(unsigned expected_host_reg, std::vector<size_t>& direct_exit_branches);
		bool EmitLoadGpr(unsigned guest_reg, unsigned host_reg);
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
		bool EmitImmediateOp(u32 op);
		bool EmitEffectiveAddress(u32 op);
		bool EmitEffectiveAddress(u32 op, unsigned host_reg);
		bool EmitLoadOp(u32 op);
		bool EmitStoreOp(u32 op);
		bool EmitUnalignedLoadOp(u32 op);
		bool EmitUnalignedStoreOp(u32 op);
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
		std::vector<ScalarLoadColdTail> m_scalar_load_cold_tails;
		std::vector<ScalarStoreColdTail> m_scalar_store_cold_tails;
		std::vector<UnalignedReadColdTail> m_unaligned_read_cold_tails;
		std::vector<UnalignedWriteColdTail> m_unaligned_write_cold_tails;
		std::vector<Cop2LoadColdTail> m_cop2_load_cold_tails;
		std::vector<Cop2StoreColdTail> m_cop2_store_cold_tails;
		u32 m_native_instruction_count = 0;
		u32 m_helper_instruction_count = 0;
		u16 m_saved_registers = 0;
		bool m_iop_ram_registers_available = false;
		bool m_iop_cycle_base_register_available = false;
		bool m_defer_cycle_updates = false;
		bool m_emit_trace_checks = false;
		bool m_emit_native_static_branch = false;
		bool m_emit_native_static_jump = false;
		bool m_emit_native_register_jump = false;
	};

	class BlockExecutor
	{
	public:
		static constexpr u32 MAX_STRAIGHT_LINE_BLOCK_INSTRUCTIONS = 64;

		BlockExecutor();
		~BlockExecutor();

		u32 Reset();
		u32 InvalidateRange(u32 start_pc, u32 instruction_count);
		void SetDirectLinkingEnabled(bool enabled);
		static bool ScanStraightLineBlock(u32 start_pc, u32 max_instruction_count, BlockScanResult* result);
		bool ExecuteCompiledBlock(u32 start_pc, u32 instruction_count, BlockExecutionResult* result);

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
		static constexpr size_t MAX_INCOMING_LINKS = MAX_CACHE_CAPACITY * DIRECT_LINK_SLOT_COUNT;
		static constexpr u32 LOOKUP_DIRECTORY_ENTRY_COUNT = 0x10000;
		static constexpr u32 LOOKUP_PAGE_ENTRY_COUNT = 0x4000;

		struct CachedBlock
		{
			VitaA32::CodeBuffer code;
			std::array<u32, MAX_STRAIGHT_LINE_BLOCK_INSTRUCTIONS> opcodes{};
			u32 start_pc = 0;
			u32 instruction_count = 0;
			u32 native_instruction_count = 0;
			u32 helper_instruction_count = 0;
			DirectLinkSlots direct_links{};
			bool valid = false;
			bool queued_free = false;
		};

		struct LookupPage
		{
			std::array<CachedBlock*, LOOKUP_PAGE_ENTRY_COUNT> blocks{};
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
		void RegisterBlockLookup(CachedBlock& block);
		void UnregisterBlockLookup(CachedBlock& block);
		void ReleaseLookupPages();
		s32 LastBlockRecordIndex(u32 pc) const;
		bool RegisterBlockRecord(CachedBlock& block);
		void UnregisterBlockRecord(CachedBlock& block);
		void ClearBlockRecords();
		CachedBlock* FindRecordedBlockByStartPc(u32 start_pc, u32 instruction_count, bool match_instruction_count);
		void RememberFreeCacheEntry(CachedBlock& block);
		CachedBlock* TakeFreeCacheEntry();
		DirectLinkSlot* GetRecordedDirectLink(IncomingLinkRecord& record);
		s32 LastIncomingLinkIndex(u32 target_pc) const;
		void ClearIncomingLinks();
		void RegisterIncomingLink(CachedBlock& block, u8 slot_index, const DirectLinkSlot& link);
		void RegisterIncomingLinks(CachedBlock& block);
		void UnregisterIncomingLinks(CachedBlock& block);
		CachedBlock* FindLookupBlockByStartPc(u32 start_pc);
		bool FindCachedBlock(u32 start_pc, u32 instruction_count, CachedBlock** block, bool* lookup_hit);
		CachedBlock* FindCachedBlockByStartPc(u32 start_pc);
		CachedBlock* AllocateCacheEntry();
		void InvalidateCachedBlock(CachedBlock& block);
		bool ValidateCachedBlock(CachedBlock& block);
		bool EnsureCodeCache();
		void ReleaseCodeCache();
		u8* AllocateCodeSlice(size_t capacity, size_t* slice_offset);
		void CommitCodeSlice(size_t slice_offset, size_t code_size);
		void RewindCodeCache(size_t slice_offset);
		u32 ResetForCachePressure();
		bool CompileIntoCacheEntry(CachedBlock& block, u32 start_pc, u32 instruction_count);
		bool RunCachedBlock(CachedBlock& block, BlockExecutionResult* result);
		bool PatchDirectLink(CachedBlock& block, DirectLinkSlot& link, const void* target);
		void PatchIncomingLinks(u32 target_pc, const void* target);
		void UnlinkIncomingLinks(u32 target_pc);
		void RelinkDirectLinks();

		std::vector<std::unique_ptr<CachedBlock>> m_cache;
		std::vector<CachedBlock*> m_free_cache_entries;
		std::vector<BlockRecord> m_block_records;
		std::vector<IncomingLinkRecord> m_incoming_links;
		LookupPage** m_lookup_pages = nullptr;
		u8* m_code_cache = nullptr;
		size_t m_code_cache_capacity = 0;
		size_t m_code_cache_used = 0;
		u32 m_code_cache_resets = 0;
		bool m_direct_linking_enabled = true;
	};
} // namespace VitaIOP
