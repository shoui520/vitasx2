// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"
#include "pcsx2/vita/A32Emitter.h"

#include <array>
#include <cstddef>

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
		bool cache_hit = false;
	};

	class BlockExecutor
	{
	public:
		static constexpr u32 MAX_STRAIGHT_LINE_BLOCK_INSTRUCTIONS = 64;

		u32 Reset();
		u32 InvalidateRange(u32 start_pc, u32 instruction_count);
		static bool ScanStraightLineBlock(u32 start_pc, u32 max_instruction_count, BlockScanResult* result);
		bool ExecuteCompiledBlock(u32 start_pc, u32 instruction_count,
			bool run_event_test_on_event_exit, BlockExecutionResult* result);
		bool ExecuteStraightLineBlockOrInterpreterStep(u32 start_pc, u32 instruction_count,
			bool run_event_test_on_event_exit, BlockExecutionResult* result);

	private:
		static constexpr size_t CACHE_CAPACITY = 32;

		struct CachedBlock
		{
			VitaA32::CodeBuffer code;
			std::array<u32, MAX_STRAIGHT_LINE_BLOCK_INSTRUCTIONS> opcodes{};
			u32 start_pc = 0;
			u32 instruction_count = 0;
			u32 scaled_cycles = 0;
			s8 ee_cycle_rate = 0;
			u8 cp0_config_cycle_shift = 0;
			bool valid = false;
		};

		bool FindCachedBlock(u32 start_pc, u32 instruction_count, CachedBlock** block);
		CachedBlock* AllocateCacheEntry();
		bool CompileIntoCacheEntry(CachedBlock& block, u32 start_pc, u32 instruction_count, u32* scaled_cycles);
		bool RunCachedBlock(CachedBlock& block, bool run_event_test_on_event_exit, BlockExecutionResult* result);

		std::array<CachedBlock, CACHE_CAPACITY> m_cache{};
		size_t m_next_victim = 0;
	};
} // namespace VitaEE
