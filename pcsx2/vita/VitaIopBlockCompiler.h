// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"
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
		static constexpr u32 HELPER_OPCODE_CLASS_SLOTS = 16;

		BlockExitKind exit = BlockExitKind::Direct;
		u32 instruction_count = 0;
		u32 native_instruction_count = 0;
		u32 helper_instruction_count = 0;
		u32 helper_opcode_class_count = 0;
		u32 helper_opcode_class_overflow = 0;
		std::array<u32, HELPER_OPCODE_CLASS_SLOTS> helper_opcode_classes{};
		std::array<u32, HELPER_OPCODE_CLASS_SLOTS> helper_opcode_class_hits{};
		std::array<u32, HELPER_OPCODE_CLASS_SLOTS> helper_opcode_class_first_pc{};
		std::array<u32, HELPER_OPCODE_CLASS_SLOTS> helper_opcode_class_first_opcode{};
		size_t code_size = 0;
		u32 cache_slots = 0;
		u32 code_cache_resets = 0;
		size_t code_cache_used = 0;
		size_t code_cache_capacity = 0;
		bool cache_hit = false;
	};

	class BlockCompiler
	{
	public:
		explicit BlockCompiler(VitaA32::CodeBuffer& code);

		static bool CanCompileOpcode(u32 op);

		bool CompileStraightLineBlock(u32 start_pc, u32 instruction_count);
		u32 NativeInstructionCount() const { return m_native_instruction_count; }
		u32 HelperInstructionCount() const { return m_helper_instruction_count; }
		u32 HelperOpcodeClassCount() const { return m_helper_opcode_class_count; }
		u32 HelperOpcodeClassOverflow() const { return m_helper_opcode_class_overflow; }
		const std::array<u32, BlockExecutionResult::HELPER_OPCODE_CLASS_SLOTS>& HelperOpcodeClasses() const { return m_helper_opcode_classes; }
		const std::array<u32, BlockExecutionResult::HELPER_OPCODE_CLASS_SLOTS>& HelperOpcodeClassHits() const { return m_helper_opcode_class_hits; }
		const std::array<u32, BlockExecutionResult::HELPER_OPCODE_CLASS_SLOTS>& HelperOpcodeClassFirstPc() const { return m_helper_opcode_class_first_pc; }
		const std::array<u32, BlockExecutionResult::HELPER_OPCODE_CLASS_SLOTS>& HelperOpcodeClassFirstOpcode() const { return m_helper_opcode_class_first_opcode; }

	private:
		bool BeginBlock();
		bool EndBlockReturn(BlockExitKind exit);
		bool EmitInstruction(u32 op, u32 pc, std::vector<size_t>& direct_exit_branches);
		void RecordHelperOpcode(u32 op, u32 pc);
		bool EmitNativeInstruction(u32 op, u32 pc);
		bool EmitNativeSPECIAL(u32 op, u32 pc);
		bool EmitNativeCOP0(u32 op);
		bool EmitNativeCOP2(u32 op);
		bool EmitHelperInstruction(u32 op, u32 pc, std::vector<size_t>& direct_exit_branches);
		bool EmitStoreCode(u32 op);
		bool EmitTraceCheck(u32 pc, u32 op, std::vector<size_t>& direct_exit_branches);
		bool EmitStorePc(u32 pc);
		bool EmitIncrementCycle();
		bool EmitPcChangedExitCheck(u32 expected_pc, std::vector<size_t>& direct_exit_branches);
		bool EmitLoadGpr(unsigned guest_reg, unsigned host_reg);
		bool EmitStoreGpr(unsigned guest_reg, unsigned host_reg);
		bool EmitMoveGpr(unsigned dst_guest_reg, unsigned src_guest_reg);
		bool EmitBinaryRegOp(u32 op);
		bool EmitShiftImmOp(u32 op);
		bool EmitShiftRegOp(u32 op);
		bool EmitSetLessThanRegOp(u32 op, bool is_signed);
		bool EmitMultiplyOp(u32 op, bool is_signed);
		bool EmitDivideOp(u32 op, bool is_signed);
		bool EmitExceptionOp(u32 pc, u32 code);
		bool EmitImmediateOp(u32 op);
		bool EmitEffectiveAddress(u32 op);
		bool EmitLoadOp(u32 op);
		bool EmitStoreOp(u32 op);
		bool EmitUnalignedLoadOp(u32 op);
		bool EmitUnalignedStoreOp(u32 op);
		bool EmitConditionalBranchOp(u32 op, u32 pc);
		bool EmitSignedBranchOp(u32 op, u32 pc);
		bool EmitJumpOp(u32 op, u32 pc);
		bool EmitRegisterJumpOp(u32 op, u32 pc);
		bool EmitCop0TransferOp(u32 op, bool to_cop0);
		bool EmitCop0RfeOp();
		bool EmitReadCop2DataReg(unsigned cop2_reg, unsigned host_reg);
		bool EmitWriteCop2DataReg(unsigned cop2_reg, unsigned host_reg);
		bool EmitCop2LoadStoreOp(u32 op);

		VitaA32::CodeBuffer& m_code;
		u32 m_native_instruction_count = 0;
		u32 m_helper_instruction_count = 0;
		u32 m_helper_opcode_class_count = 0;
		u32 m_helper_opcode_class_overflow = 0;
		std::array<u32, BlockExecutionResult::HELPER_OPCODE_CLASS_SLOTS> m_helper_opcode_classes{};
		std::array<u32, BlockExecutionResult::HELPER_OPCODE_CLASS_SLOTS> m_helper_opcode_class_hits{};
		std::array<u32, BlockExecutionResult::HELPER_OPCODE_CLASS_SLOTS> m_helper_opcode_class_first_pc{};
		std::array<u32, BlockExecutionResult::HELPER_OPCODE_CLASS_SLOTS> m_helper_opcode_class_first_opcode{};
	};

	class BlockExecutor
	{
	public:
		static constexpr u32 MAX_STRAIGHT_LINE_BLOCK_INSTRUCTIONS = 64;

		BlockExecutor();
		~BlockExecutor();

		u32 Reset();
		u32 InvalidateRange(u32 start_pc, u32 instruction_count);
		static bool ScanStraightLineBlock(u32 start_pc, u32 max_instruction_count, BlockScanResult* result);
		bool ExecuteCompiledBlock(u32 start_pc, u32 instruction_count, BlockExecutionResult* result);

	private:
		static constexpr size_t INITIAL_CACHE_CAPACITY = 32;
		static constexpr size_t MAX_CACHE_CAPACITY = 256;
		static constexpr size_t STRAIGHT_LINE_BLOCK_CODE_CAPACITY = 4096;
		static constexpr size_t MAX_STRAIGHT_LINE_BLOCK_CODE_CAPACITY = 16 * 1024;
		static constexpr size_t IOP_CODE_CACHE_CAPACITY = 512 * 1024;
		static constexpr size_t CODE_CACHE_ALIGNMENT = 32;

		struct CachedBlock
		{
			VitaA32::CodeBuffer code;
			std::array<u32, MAX_STRAIGHT_LINE_BLOCK_INSTRUCTIONS> opcodes{};
			u32 start_pc = 0;
			u32 instruction_count = 0;
			u32 native_instruction_count = 0;
			u32 helper_instruction_count = 0;
			u32 helper_opcode_class_count = 0;
			u32 helper_opcode_class_overflow = 0;
			std::array<u32, BlockExecutionResult::HELPER_OPCODE_CLASS_SLOTS> helper_opcode_classes{};
			std::array<u32, BlockExecutionResult::HELPER_OPCODE_CLASS_SLOTS> helper_opcode_class_hits{};
			std::array<u32, BlockExecutionResult::HELPER_OPCODE_CLASS_SLOTS> helper_opcode_class_first_pc{};
			std::array<u32, BlockExecutionResult::HELPER_OPCODE_CLASS_SLOTS> helper_opcode_class_first_opcode{};
			bool valid = false;
		};

		CachedBlock* FindCachedBlock(u32 start_pc, u32 instruction_count);
		CachedBlock* AllocateCacheEntry();
		void InvalidateCachedBlock(CachedBlock& block);
		bool ValidateCachedBlock(CachedBlock& block);
		bool EnsureCodeCache();
		void ReleaseCodeCache();
		u8* AllocateCodeSlice(size_t capacity, size_t* slice_offset);
		void RewindCodeCache(size_t slice_offset);
		u32 ResetForCachePressure();
		bool CompileIntoCacheEntry(CachedBlock& block, u32 start_pc, u32 instruction_count);
		bool RunCachedBlock(CachedBlock& block, BlockExecutionResult* result);

		std::vector<std::unique_ptr<CachedBlock>> m_cache;
		u8* m_code_cache = nullptr;
		size_t m_code_cache_capacity = 0;
		size_t m_code_cache_used = 0;
		u32 m_code_cache_resets = 0;
	};
} // namespace VitaIOP
