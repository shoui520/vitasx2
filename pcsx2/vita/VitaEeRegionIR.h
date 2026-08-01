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
		MemoryEffect,
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
		AddressFromI32,
		EffectiveAddress32,
		MemoryLoad,
		MemoryLoadValue,
		MemoryStore,
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
		// Ordered, non-architectural memory state. It prevents loads and stores
		// from being reordered across each other while remaining absent from the
		// canonical EE register image.
		ValueId memory_effect = INVALID_VALUE;
	};

	enum class MemoryAccessKind : u8
	{
		LoadS8,
		LoadU8,
		LoadS16,
		LoadU16,
		LoadS32,
		LoadU32,
		Load64,
		Load128,
		Store8,
		Store16,
		Store32,
		Store64,
		Store128,
	};

	enum class MemoryProbeResult : u8
	{
		Direct,
		Handler,
		Translation,
		SelfModifyingCode,
	};

	struct MemoryRequest
	{
		u32 source_pc = 0;
		u32 address = 0;
		MemoryAccessKind kind = MemoryAccessKind::LoadS8;
	};

	// The Region IR interpreter is product-disabled, but its memory contract is
	// also the contract the A32 backend must implement. Probe must be free of
	// guest-visible effects. Read/write are called only after Direct and must
	// perform exactly one access of the requested width.
	struct RegionMemoryInterface
	{
		void* context = nullptr;
		MemoryProbeResult (*probe)(void* context,
			const MemoryRequest& request) = nullptr;
		bool (*read)(void* context, const MemoryRequest& request,
			u128* value) = nullptr;
		bool (*write)(void* context, const MemoryRequest& request,
			const u128& value) = nullptr;
	};

	enum class ExitReason : u8
	{
		RegionBoundary,
		UnsupportedOpcode,
		MemoryObserver,
		MemoryAlignment,
		MemoryHandler,
		MemoryTranslation,
		SelfModifyingCode,
		HelperObserver,
		UnsupportedControlFlow,
		// The represented state is immediately before an instruction which can
		// raise an EE architectural exception. The fallback owns both the
		// exceptional and non-exceptional outcomes; it may not use a generic
		// returning-helper continuation.
		ExceptionObserver,
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
		// A side exit before an observer remains inside the original PCSX2
		// recompiler block. Its architectural cycle is therefore still the block-
		// entry value and the fallback must append this fixed-point raw prefix,
		// execute the observer/remainder, then scale once at the real block edge.
		// Such an exit must not perform an event-horizon test first, including
		// when the prefix is empty and pending_raw_cycles is zero.
		bool cycle_commit_deferred = false;
		u32 pending_raw_cycles = 0;
		// True only at a real PCSX2 scheduler boundary. A cycle-publishing A32
		// physical continuation may deliberately leave this false.
		bool event_horizon_check = true;
	};

	enum class TerminatorKind : u8
	{
		Transfer,
		Branch,
		Jump,
		RegisterJump,
	};

	struct Terminator
	{
		TerminatorKind kind = TerminatorKind::Transfer;
		// Likely branches execute the represented delay slot only on the taken
		// edge. Their two transfers therefore own distinct state/cycle maps.
		bool likely = false;
		ValueId condition = INVALID_VALUE;
		Transfer taken{};
		Transfer not_taken{};
		// For Branch and Jump these identify the indivisible control/delay
		// source pair. Transfer has neither.
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
		// Equal to the primary cost except for a likely branch, where this
		// excludes the annulled delay slot.
		u32 not_taken_raw_cycle_cost = 0;
		u32 not_taken_scaled_cycle_cost = 0;
		Terminator terminator{};
	};

	// One immutable timing fragment from the existing Vita EE provider. A
	// PCSX2 source block can be emitted as several A32 fragments when the host
	// code budget is exhausted; all such fragments share dependency_start_pc /
	// dependency_instruction_count. charged_scaled_cycles_before is the exact
	// architectural cycle charge already published by preceding fragments.
	// scheduler_test_at_end is separate because PCSX2 short splits and Vita's
	// cycle-proven A32 continuations publish cycles without polling at that seam.
	struct SourceBlockContract
	{
		u32 start_pc = 0;
		u32 instruction_count = 0;
		u32 dependency_start_pc = 0;
		u32 dependency_instruction_count = 0;
		u32 charged_scaled_cycles_before = 0;
		bool scheduler_test_at_end = true;
	};

	struct LiftOptions
	{
		// Interpreter.cpp::execI() multiplies each opcode cost by this value,
		// derived from CP0.Config bit 18. Valid EE values are one and two.
		u32 cycle_factor = 2;
		s8 ee_cycle_rate = 0;
		// PCSX2's Goemon TLB gamefix adds observable jump/JR behavior. Static
		// jumps fail closed while it is active until that helper contract is IR.
		bool goemon_tlb_hack = false;
		u32 max_blocks = 8;
		u32 max_source_instructions = 64;
	};

	struct Program
	{
		u32 source_base_pc = 0;
		std::vector<u32> source_words;
		// Empty only for the explicitly unattested validation overload of Lift().
		// Product compilation must use LiftWithSourceBlocks().
		std::vector<SourceBlockContract> source_blocks;
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
		SourceBlockContract,
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
		SourceBlockContract,
		CycleMismatch,
		ExitContractMismatch,
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
		const RegionMemoryInterface* memory = nullptr;
	};

	struct InterpretResult
	{
		bool completed = false;
		ExitReason reason = ExitReason::RegionBoundary;
		u32 blocks_executed = 0;
		// Dynamic guest instructions whose complete architectural effects were
		// committed. An observer instruction at a side exit is not included.
		u32 source_instructions_executed = 0;
		// When execution stops before an observer, canonical cycle remains at the
		// original PCSX2 block entry. This is the exact fixed-point recompiler cost
		// already executed in that block. A product continuation must append the
		// remaining source cost and scale once at the original block edge.
		bool cycle_commit_deferred = false;
		u32 pending_raw_cycles = 0;
		u32 memory_address = 0;
		std::string error;
	};

	// Pure PCSX2 cycle scaling contract, parameterized so product-disabled
	// validation does not need to read global configuration.
	u32 RawRecompilerCycles(u32 opcode, u32 cycle_factor);
	u32 ScaleBlockCycles(u32 raw_cycles, s8 ee_cycle_rate);

	LiftResult Lift(u32 source_base_pc, const u32* source_words,
		u32 source_word_count, u32 entry_pc,
		const LiftOptions& options = {});
	LiftResult LiftWithSourceBlocks(u32 source_base_pc, const u32* source_words,
		u32 source_word_count, const SourceBlockContract* source_blocks,
		u32 source_block_count, u32 entry_pc,
		const LiftOptions& options = {});
	VerifyResult Verify(const Program& program);
	InterpretResult Interpret(const Program& program, const CanonicalState& input,
		CanonicalState* output,
		const InterpretOptions& options = {});
} // namespace VitaEE::RegionIR
