// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "pcsx2/vita/VitaEeSemanticIsland.h"

#include <string>
#include <vector>

namespace VitaEE::AdpcmDecode
{
	// This is a cold structural candidate descriptor, not ExactSupport. It records
	// only facts mechanically recovered from verified Region IR. No PC, source
	// word, title, symbol, hash, or caller-selected family identity is retained.
	enum class ArithmeticMode : u8
	{
		IntegerFixedPoint,
		Cop1Single,
		Count,
	};

	enum class OuterScalarStorage : u8
	{
		Gpr,
		Spill32,
		Count,
	};

	enum class Failure : u8
	{
		None,
		InvalidProgram,
		InvalidIsland,
		UnsupportedTopology,
		UnsupportedExitContract,
		UnsupportedFrameControl,
		UnsupportedInnerMemory,
		UnsupportedOuterMemory,
		UnsupportedArithmetic,
		Count,
	};

	struct MemoryOperation
	{
		RegionIR::ValueId operation = RegionIR::INVALID_VALUE;
		u32 block = RegionIR::INVALID_BLOCK;
		u32 node_ordinal = 0;
		RegionIR::MemoryAccessKind kind = RegionIR::MemoryAccessKind::LoadS8;
		u8 address_base_gpr = 0;
		s32 address_offset = 0;
		bool exact_header_affine_address = false;

		bool operator==(const MemoryOperation&) const = default;
	};

	struct ControlEdge
	{
		u32 source_block = RegionIR::INVALID_BLOCK;
		u32 target_block = RegionIR::INVALID_BLOCK;
		u8 ordinal = 0;
		u32 scaled_cycle_cost = 0;
		bool event_horizon_check = false;

		bool operator==(const ControlEdge&) const = default;
	};

	struct PredictorLookup
	{
		RegionIR::ValueId load_operation = RegionIR::INVALID_VALUE;
		RegionIR::ValueId input_operation = RegionIR::INVALID_VALUE;
		bool low_nibble_mask = false;
		u8 index_shift = 0;
		bool base_is_immediate = false;
		u32 base_immediate = 0;
		u8 base_gpr = 0;
		s32 base_offset = 0;

		bool operator==(const PredictorLookup&) const = default;
	};

	struct Candidate
	{
		ArithmeticMode arithmetic_mode = ArithmeticMode::IntegerFixedPoint;
		u32 outer_loop = RegionIR::INVALID_BLOCK;
		u32 inner_loop = RegionIR::INVALID_BLOCK;
		std::vector<u32> outer_blocks;
		std::vector<u32> inner_blocks;
		std::vector<MemoryOperation> inner_input_bytes;
		std::vector<MemoryOperation> inner_predictor_loads;
		std::vector<PredictorLookup> predictor_lookups;
		std::vector<MemoryOperation> inner_outputs;
		// One complete dynamic iteration schedule. Every acyclic header-to-latch
		// path has been proven to contain these memory kinds in this exact order.
		std::vector<RegionIR::MemoryAccessKind> inner_memory_schedule;
		// The corresponding verifier value identities. Equal identities on every
		// path prove operation order, not merely an equal sequence of access kinds.
		std::vector<RegionIR::ValueId> inner_memory_order;
		u32 equivalent_inner_schedule_paths = 0;
		std::vector<u8> inner_input_base_gprs;
		std::vector<u8> inner_output_base_gprs;
		u32 inner_input_pointer_stride = 0;
		u32 inner_output_pointer_stride = 0;
		bool exact_inner_stream_recurrences = false;
		// The two marker arms and the ordinary outer-loop completion are retained
		// as separate verifier-owned edges.  They are not yet semantic exit recipes.
		std::vector<ControlEdge> outer_exit_edges;
		u32 frame_advance_bytes = 0;
		u32 inner_counter_seed = 0;
		u8 inner_counter_gpr = 0;
		s32 inner_counter_stride = 0;
		u32 inner_iterations = 0;
		bool exact_inner_control = false;
		u8 outer_counter_gpr = 0;
		u8 outer_bound_gpr = 0;
		OuterScalarStorage outer_counter_storage = OuterScalarStorage::Gpr;
		MemoryOperation outer_counter_initialization;
		MemoryOperation outer_counter_load;
		MemoryOperation outer_counter_store;
		OuterScalarStorage outer_bound_storage = OuterScalarStorage::Gpr;
		MemoryOperation outer_bound_initialization;
		MemoryOperation outer_bound_load;
		RegionIR::ValueId outer_bound_initial_value = RegionIR::INVALID_VALUE;
		std::vector<u8> outer_frame_pointer_gprs;
		s32 outer_counter_stride = 0;
		u32 outer_frame_pointer_stride = 0;
		bool exact_outer_control = false;
		u32 cop1_arithmetic_nodes = 0;
		u32 integer_multiply_nodes = 0;
		bool exact_source_attestation = false;

		bool operator==(const Candidate&) const = default;
	};

	struct BuildResult
	{
		Candidate candidate{};
		Failure failure = Failure::None;
		u32 block = RegionIR::INVALID_BLOCK;
		RegionIR::ValueId value = RegionIR::INVALID_VALUE;
		std::string detail;

		explicit operator bool() const { return failure == Failure::None; }
	};

	// Recognize the common nested framed-predictor skeleton shared by the two
	// byte-attested CRI ADX implementations. Success is candidate coverage only:
	// exact recurrence expressions, alias ranges, live-outs, and all exit recipes
	// still have to be reconstructed before a semantic kind may be admitted.
	BuildResult BuildCandidate(const RegionIR::Program& program);
} // namespace VitaEE::AdpcmDecode
