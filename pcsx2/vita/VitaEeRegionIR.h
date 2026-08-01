// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <array>
#include <string>
#include <vector>

namespace VitaEE::RegionIR
{
	using ValueId = u32;
	static constexpr ValueId INVALID_VALUE = UINT32_MAX;
	static constexpr u32 INVALID_BLOCK = UINT32_MAX;

	// These types describe PS2 architectural values, not their eventual A32
	// allocation. In particular, an EE GPR remains I128 even when an operation
	// replaces only its low 64 bits.
	enum class ValueType : u8
	{
		Void,
		I1,
		I32,
		I64,
		I128,
		Address,
		Cycle,
	};

	enum class Opcode : u8
	{
		Parameter,
		ConstantI32,
		ConstantI64,
		ConstantAddress,
		ExtractLow32,
		ExtractLow64,
		ReplaceLow64,
		SignExtend32To64,
		ZeroExtend32To64,
		Add32,
		Add64,
		Sub32,
		Sub64,
		And64,
		Or64,
		Xor64,
		Nor64,
		ShiftLeft32,
		ShiftRightLogical32,
		ShiftRightArithmetic32,
		ShiftLeft64,
		ShiftRightLogical64,
		ShiftRightArithmetic64,
		CompareEqual64,
		CompareNotEqual64,
		CompareSignedLess64,
		CompareUnsignedLess64,
		CompareSignedLessEqualZero64,
		CompareSignedGreaterZero64,
		CompareSignedLessZero64,
		CompareSignedGreaterEqualZero64,
		BindGpr,
		BindHi,
		BindLo,
		AdvanceCycles,
	};

	struct Node
	{
		ValueId id = INVALID_VALUE;
		Opcode opcode = Opcode::Parameter;
		ValueType type = ValueType::Void;
		std::array<ValueId, 3> operands = {INVALID_VALUE, INVALID_VALUE,
			INVALID_VALUE};
		u8 operand_count = 0;
		// Parameter/BindGpr slot, shift amount, or immediate cycle delta.
		u32 immediate = 0;
		// Constants use the complete 64-bit payload. Address constants consume
		// its low word.
		u64 literal = 0;
		// Owning guest instruction. Parameters and edge-only constants use the
		// block PC or transfer source PC.
		u32 source_pc = 0;
	};

	struct StateMap
	{
		std::array<ValueId, 32> gpr{};
		ValueId hi = INVALID_VALUE;
		ValueId lo = INVALID_VALUE;
		ValueId cycle = INVALID_VALUE;
	};

	enum class ExitReason : u8
	{
		RegionBoundary,
		UnsupportedOpcode,
		MemoryObserver,
		HelperObserver,
		UnsupportedControlFlow,
		EventHorizon,
	};

	// A transfer owns one complete canonical-state map. If target_block is
	// valid, the same map supplies its block parameters and the event-horizon
	// exit at this edge. Otherwise it is an ordinary side exit.
	struct Transfer
	{
		u32 target_block = INVALID_BLOCK;
		ValueId pc = INVALID_VALUE;
		StateMap state{};
		ExitReason external_reason = ExitReason::RegionBoundary;
	};

	enum class TerminatorKind : u8
	{
		Transfer,
		Branch,
	};

	struct Terminator
	{
		TerminatorKind kind = TerminatorKind::Transfer;
		ValueId condition = INVALID_VALUE;
		Transfer taken{};
		Transfer not_taken{};
		u32 branch_pc = 0;
		u32 delay_slot_pc = 0;
	};

	struct SourceInstruction
	{
		u32 pc = 0;
		u32 opcode = 0;
		bool delay_slot = false;
	};

	struct Block
	{
		u32 pc = 0;
		StateMap parameters{};
		std::vector<Node> nodes;
		std::vector<SourceInstruction> source;
		u32 raw_cycle_cost = 0;
		u32 scaled_cycle_cost = 0;
		Terminator terminator{};
	};

	struct LiftOptions
	{
		// Interpreter.cpp::execI() multiplies each opcode cost by this value,
		// derived from CP0.Config bit 18. Valid EE values are one and two.
		u32 cycle_factor = 2;
		s8 ee_cycle_rate = 0;
		u32 max_blocks = 8;
		u32 max_source_instructions = 64;
	};

	struct Program
	{
		u32 source_base_pc = 0;
		std::vector<u32> source_words;
		LiftOptions options{};
		u32 entry_block = INVALID_BLOCK;
		u32 value_count = 0;
		std::vector<Block> blocks;
	};

	enum class LiftFailure : u8
	{
		None,
		InvalidSource,
		EntryOutsideSource,
		SourceLimit,
		BlockLimit,
		MissingDelaySlot,
		BranchInDelaySlot,
		OverlappingSource,
		ValueLimit,
		InternalError,
	};

	struct LiftResult
	{
		Program program{};
		LiftFailure failure = LiftFailure::None;
		u32 failure_pc = 0;

		explicit operator bool() const { return failure == LiftFailure::None; }
	};

	enum class VerifyFailure : u8
	{
		None,
		InvalidProgram,
		DuplicateBlockPc,
		InvalidEntry,
		ValueIdMismatch,
		ParameterContract,
		OperandOutOfRange,
		OperandNotLocal,
		OperandType,
		ResultType,
		StateMapMismatch,
		SourceMismatch,
		SourceOverlap,
		CycleMismatch,
		ControlFlowMismatch,
		UnreachableBlock,
	};

	struct VerifyResult
	{
		VerifyFailure failure = VerifyFailure::None;
		u32 block = INVALID_BLOCK;
		u32 node = UINT32_MAX;
		std::string detail;

		explicit operator bool() const { return failure == VerifyFailure::None; }
	};

	struct CanonicalState
	{
		std::array<u128, 32> gpr{};
		u128 hi{};
		u128 lo{};
		u32 pc = 0;
		u64 cycle = 0;
	};

	struct InterpretOptions
	{
		u64 next_event_cycle = UINT64_MAX;
		u32 max_block_executions = 4096;
	};

	struct InterpretResult
	{
		bool completed = false;
		ExitReason reason = ExitReason::RegionBoundary;
		u32 blocks_executed = 0;
		std::string error;
	};

	// Pure PCSX2 cycle scaling contract, parameterized so product-disabled
	// validation does not need to read global configuration.
	u32 ScaleBlockCycles(u32 raw_cycles, s8 ee_cycle_rate);

	LiftResult Lift(u32 source_base_pc, const u32* source_words,
		u32 source_word_count, u32 entry_pc,
		const LiftOptions& options = {});
	VerifyResult Verify(const Program& program);
	InterpretResult Interpret(const Program& program, const CanonicalState& input,
		CanonicalState* output,
		const InterpretOptions& options = {});
} // namespace VitaEE::RegionIR
