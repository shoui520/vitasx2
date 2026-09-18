// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "pcsx2/vita/VitaEeRegionExecutionPlan.h"

#include <string>
#include <vector>

namespace VitaEE::SemanticIsland
{
	// A semantic operation island is a source-attested reducible execution unit,
	// not a recognized implementation.  This descriptor proves only ownership,
	// control, timing-observer, and exit-map boundaries.  It deliberately has no
	// code-buffer, runtime-lookup, publication, or execution capability.
	enum class Failure : u8
	{
		None,
		InvalidProgram,
		UnattestedSource,
		InvalidExecutionPlan,
		BlockLimit,
		IrreducibleControlFlow,
		OverlappingLoops,
		MissingCompletion,
		InvalidExitContract,
		Count,
	};

	enum class ExitClass : u8
	{
		Completion,
		GuardedObserver,
		SynchronizationObserver,
		MemoryFallback,
		ControlFallback,
	};

	struct InternalEdge
	{
		u32 source_block = RegionIR::INVALID_BLOCK;
		u32 target_block = RegionIR::INVALID_BLOCK;
		u8 ordinal = 0;
		u32 scaled_cycle_cost = 0;
		bool backedge = false;
		bool event_horizon_check = false;
		bool cycle_commit_deferred = false;

		bool operator==(const InternalEdge&) const = default;
	};

	struct Loop
	{
		u32 header_block = RegionIR::INVALID_BLOCK;
		std::vector<u32> latch_blocks;
		// Sorted block indices in the complete natural loop.  A loop with several
		// latches has one union body; nested loops appear in both their own body and
		// every containing parent body.
		std::vector<u32> blocks;
		u32 parent_loop = RegionIR::INVALID_BLOCK;
		u32 depth = 0;
		u32 source_instructions = 0;
		u32 static_scaled_cycles = 0;
		u32 event_checked_latches = 0;

		bool operator==(const Loop&) const = default;
	};

	struct Exit
	{
		RegionExecution::ExitSiteKind site_kind =
			RegionExecution::ExitSiteKind::Taken;
		u32 block = RegionIR::INVALID_BLOCK;
		u32 ordinal = 0;
		ExitClass exit_class = ExitClass::ControlFallback;
		RegionIR::ExitReason reason = RegionIR::ExitReason::RegionBoundary;
		u32 dirty_state_bindings = 0;
		u32 dirty_state_words = 0;
		u32 static_resume_pc = 0;
		bool static_resume_pc_known = false;
		bool cycle_commit_deferred = false;
		u32 pending_raw_cycles = 0;
		bool event_horizon_check = false;

		bool operator==(const Exit&) const = default;
	};

	struct Plan
	{
		u32 entry_block = RegionIR::INVALID_BLOCK;
		// Stable topological order after every dominance backedge is removed.  A
		// semantic recognizer may use this order for dataflow, but may not mistake it
		// for dynamic execution order inside a nested loop.
		std::vector<u32> acyclic_order;
		std::vector<InternalEdge> internal_edges;
		std::vector<Loop> loops;
		std::vector<Exit> exits;
		u32 source_instructions = 0;
		u32 ir_nodes = 0;
		u32 memory_operations = 0;
		u32 direct_calls = 0;
		u32 maximum_loop_depth = 0;
		u32 prefix_blocks = 0;
		u32 repeated_blocks = 0;
		u32 suffix_blocks = 0;
		u32 other_acyclic_blocks = 0;
		u32 internal_event_checkpoints = 0;
		u32 completion_exits = 0;
		u32 cold_exits = 0;
		u32 completion_state_words = 0;
		u32 cold_exit_state_words = 0;
		bool has_repeated_core = false;
		bool completion_reachable_from_repeated_core = false;
		bool exact_source_attestation = false;
		bool exact_exit_maps = false;

		bool operator==(const Plan&) const = default;
	};

	struct BuildResult
	{
		Plan plan{};
		Failure failure = Failure::None;
		u32 block = RegionIR::INVALID_BLOCK;
		u32 related_block = RegionIR::INVALID_BLOCK;
		std::string detail;

		explicit operator bool() const { return failure == Failure::None; }
	};

	// Cold validation-only topology analysis. RegionIR::Verify() and the
	// mechanical RegionExecution exit plan remain the semantic authorities.
	// Successful analysis never implies recognition or profitability.
	BuildResult Build(const RegionIR::Program& program);
} // namespace VitaEE::SemanticIsland
