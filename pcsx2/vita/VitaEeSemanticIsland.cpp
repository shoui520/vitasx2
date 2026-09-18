// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#include "pcsx2/vita/VitaEeSemanticIsland.h"

#include <algorithm>
#include <bit>
#include <deque>
#include <map>
#include <utility>

namespace VitaEE::SemanticIsland
{
	namespace
	{
		using namespace RegionIR;

		constexpr u32 MAX_ISLAND_BLOCKS = 63;

		struct EdgeRef
		{
			u32 source = INVALID_BLOCK;
			u32 target = INVALID_BLOCK;
			u8 ordinal = 0;
			const Transfer* transfer = nullptr;
		};

		BuildResult Fail(Failure failure, std::string detail,
			u32 block = INVALID_BLOCK, u32 related_block = INVALID_BLOCK)
		{
			BuildResult result{};
			result.failure = failure;
			result.block = block;
			result.related_block = related_block;
			result.detail = std::move(detail);
			return result;
		}

		bool IsBackedge(const std::vector<u64>& dominators,
			const EdgeRef& edge)
		{
			return edge.source < dominators.size() && edge.target < dominators.size() &&
				(dominators[edge.source] & (u64{1} << edge.target)) != 0;
		}

		u32 EdgeCycleCost(const Block& block, const Transfer* transfer)
		{
			if (block.terminator.kind == TerminatorKind::Branch &&
				transfer == &block.terminator.not_taken)
			{
				return block.not_taken_scaled_cycle_cost;
			}
			return block.scaled_cycle_cost;
		}

		const RegionExecution::ExitSite* FindExitSite(
			const RegionExecution::Plan& execution,
			RegionExecution::ExitSiteKind kind, u32 block, u32 ordinal)
		{
			const RegionExecution::ExitSite* result = nullptr;
			for (const RegionExecution::ExitSite& site : execution.exits)
			{
				if (site.kind != kind || site.block != block || site.ordinal != ordinal)
					continue;
				if (result)
					return nullptr;
				result = &site;
			}
			return result;
		}

		bool ResolveStaticPc(const Program& program, const Transfer& transfer,
			u32* pc)
		{
			if (!pc)
				return false;
			if (transfer.target_block < program.blocks.size())
			{
				*pc = program.blocks[transfer.target_block].pc;
				return true;
			}
			for (const Block& block : program.blocks)
			{
				for (const Node& node : block.nodes)
				{
					if (node.id != transfer.pc)
						continue;
					if (node.opcode != Opcode::ConstantAddress)
						return false;
					*pc = static_cast<u32>(node.literal);
					return true;
				}
			}
			return false;
		}

		u32 DirtyBindings(const RegionExecution::ExitSite& site)
		{
			return static_cast<u32>(site.dirty_state.count());
		}

		u32 DirtyWords(const RegionExecution::ExitSite& site)
		{
			u32 words = 0;
			for (size_t slot = 0; slot < RegionExecution::STATE_SLOT_COUNT; slot++)
			{
				const u8 valid = static_cast<u8>(
					(1u << RegionExecution::StateWordCount(slot)) - 1u);
				words += std::popcount(static_cast<unsigned>(
					site.dirty_words[slot] & valid));
			}
			return words;
		}

		ExitClass ClassifyExit(RegionExecution::ExitSiteKind kind,
			const Transfer& transfer)
		{
			if ((kind == RegionExecution::ExitSiteKind::Taken ||
				 kind == RegionExecution::ExitSiteKind::NotTaken) &&
				transfer.external_reason == ExitReason::RegionBoundary)
			{
				return ExitClass::Completion;
			}
			switch (kind)
			{
				case RegionExecution::ExitSiteKind::Guarded:
					return ExitClass::GuardedObserver;
				case RegionExecution::ExitSiteKind::Observer:
					return ExitClass::SynchronizationObserver;
				case RegionExecution::ExitSiteKind::Memory:
					return ExitClass::MemoryFallback;
				default:
					return ExitClass::ControlFallback;
			}
		}

		BuildResult AppendExit(const Program& program,
			const RegionExecution::Plan& execution,
			RegionExecution::ExitSiteKind kind, u32 block, u32 ordinal,
			Plan* plan)
		{
			if (!plan)
				return Fail(Failure::InvalidExitContract,
					"operation-island output is null", block);
			const RegionExecution::ExitSite* site =
				FindExitSite(execution, kind, block, ordinal);
			if (!site)
			{
				return Fail(Failure::InvalidExitContract,
					"external edge has no unique mechanical exit site", block);
			}
			RegionExecution::ExitContractView contract{};
			if (!RegionExecution::ResolveExitContract(program, *site, &contract) ||
				!contract.transfer || !contract.state ||
				contract.transfer->target_block != INVALID_BLOCK)
			{
				return Fail(Failure::InvalidExitContract,
					"external edge does not resolve to its verifier-owned state map", block);
			}

			Exit exit{};
			exit.site_kind = kind;
			exit.block = block;
			exit.ordinal = ordinal;
			exit.exit_class = ClassifyExit(kind, *contract.transfer);
			exit.reason = contract.transfer->external_reason;
			exit.dirty_state_bindings = DirtyBindings(*site);
			exit.dirty_state_words = DirtyWords(*site);
			exit.static_resume_pc_known =
				ResolveStaticPc(program, *contract.transfer, &exit.static_resume_pc);
			exit.cycle_commit_deferred = contract.cycle_commit_deferred;
			exit.pending_raw_cycles = contract.pending_raw_cycles;
			exit.event_horizon_check = contract.event_horizon_check;
			if (exit.exit_class == ExitClass::Completion)
			{
				plan->completion_exits++;
				plan->completion_state_words += exit.dirty_state_words;
			}
			else
			{
				plan->cold_exits++;
				plan->cold_exit_state_words += exit.dirty_state_words;
			}
			plan->exits.push_back(std::move(exit));
			return {};
		}

		bool MaskContains(u64 outer, u64 inner)
		{
			return (outer & inner) == inner;
		}
	} // namespace

	BuildResult Build(const RegionIR::Program& program)
	{
		const VerifyResult verified = Verify(program);
		if (!verified)
		{
			return Fail(Failure::InvalidProgram, verified.detail,
				verified.block, verified.node);
		}
		if (program.source_spans.empty() || program.source_blocks.empty())
		{
			return Fail(Failure::UnattestedSource,
				"operation island requires immutable source spans and source-block contracts");
		}
		if (program.blocks.empty() || program.blocks.size() > MAX_ISLAND_BLOCKS ||
			program.entry_block >= program.blocks.size())
		{
			return Fail(Failure::BlockLimit,
				"operation island exceeds the bounded CFG bitset");
		}

		const RegionExecution::BuildResult execution =
			RegionExecution::Build(program);
		if (!execution)
		{
			return Fail(Failure::InvalidExecutionPlan, execution.detail,
				execution.block, execution.value);
		}

		const u32 block_count = static_cast<u32>(program.blocks.size());
		std::vector<EdgeRef> edges;
		std::vector<std::vector<u32>> predecessors(block_count);
			for (u32 block_index = 0; block_index < block_count; block_index++)
		{
			const Block& block = program.blocks[block_index];
			if (!VisitInternalTransfers(block.terminator,
					[&](const Transfer& transfer, u8 ordinal) {
						if (transfer.target_block >= block_count)
							return false;
							edges.push_back({block_index, transfer.target_block,
								ordinal, &transfer});
							predecessors[transfer.target_block].push_back(block_index);
							return true;
					}))
			{
				return Fail(Failure::InvalidProgram,
					"internal transfer target is outside the verified CFG", block_index);
			}
		}

		const u64 all = (u64{1} << block_count) - 1u;
		std::vector<u64> dominators(block_count, all);
		dominators[program.entry_block] = u64{1} << program.entry_block;
		bool changed = true;
		while (changed)
		{
			changed = false;
			for (u32 block = 0; block < block_count; block++)
			{
				if (block == program.entry_block)
					continue;
				u64 incoming = all;
				if (predecessors[block].empty())
					incoming = 0;
				else
					for (u32 predecessor : predecessors[block])
						incoming &= dominators[predecessor];
				const u64 next = incoming | (u64{1} << block);
				if (next != dominators[block])
				{
					dominators[block] = next;
					changed = true;
				}
			}
		}

		std::vector<u8> indegree(block_count, 0);
		std::vector<EdgeRef> backedges;
		for (const EdgeRef& edge : edges)
		{
			if (IsBackedge(dominators, edge))
				backedges.push_back(edge);
			else if (indegree[edge.target] == UINT8_MAX)
				return Fail(Failure::BlockLimit, "operation-island indegree overflows");
			else
				indegree[edge.target]++;
		}
		std::deque<u32> ready;
		for (u32 block = 0; block < block_count; block++)
			if (indegree[block] == 0)
				ready.push_back(block);
		std::vector<u32> acyclic_order;
		while (!ready.empty())
		{
			const u32 source = ready.front();
			ready.pop_front();
			acyclic_order.push_back(source);
				for (const EdgeRef& edge : edges)
				{
					if (edge.source != source || IsBackedge(dominators, edge))
						continue;
					if (--indegree[edge.target] == 0)
						ready.push_back(edge.target);
				}
		}
		if (acyclic_order.size() != block_count)
		{
			return Fail(Failure::IrreducibleControlFlow,
				"removing dominance backedges does not make the CFG acyclic");
		}

		Plan plan{};
		plan.entry_block = program.entry_block;
		plan.acyclic_order = std::move(acyclic_order);
		plan.source_instructions = ProgramSourceInstructionCount(program);
		plan.direct_calls = static_cast<u32>(program.direct_calls.size());
		plan.exact_source_attestation = true;
		plan.exact_exit_maps = true;
		for (const Block& block : program.blocks)
		{
			plan.ir_nodes += static_cast<u32>(block.nodes.size());
			for (const Node& node : block.nodes)
				plan.memory_operations += node.opcode == Opcode::MemoryLoad ||
					node.opcode == Opcode::MemoryStore;
		}

		for (const EdgeRef& edge : edges)
		{
			const bool backedge = IsBackedge(dominators, edge);
			const Block& source = program.blocks[edge.source];
			plan.internal_edges.push_back({edge.source, edge.target, edge.ordinal,
				EdgeCycleCost(source, edge.transfer), backedge,
				edge.transfer->event_horizon_check,
				edge.transfer->cycle_commit_deferred});
			plan.internal_event_checkpoints += edge.transfer->event_horizon_check;
		}

		std::map<u32, std::vector<EdgeRef>> latches_by_header;
		for (const EdgeRef& edge : backedges)
			latches_by_header[edge.target].push_back(edge);
		std::vector<u64> loop_masks;
		for (const auto& [header, latches] : latches_by_header)
		{
			u64 mask = u64{1} << header;
			std::vector<u32> pending;
			for (const EdgeRef& latch : latches)
			{
				mask |= u64{1} << latch.source;
				if (latch.source != header)
					pending.push_back(latch.source);
			}
			while (!pending.empty())
			{
				const u32 block = pending.back();
				pending.pop_back();
				for (u32 predecessor : predecessors[block])
				{
					if (predecessor == header ||
						(mask & (u64{1} << predecessor)) != 0)
					{
						mask |= u64{1} << predecessor;
						continue;
					}
					if ((dominators[predecessor] & (u64{1} << header)) == 0)
					{
						return Fail(Failure::IrreducibleControlFlow,
							"natural loop has an entry which bypasses its header",
							predecessor, header);
					}
					mask |= u64{1} << predecessor;
					pending.push_back(predecessor);
				}
			}

			Loop loop{};
			loop.header_block = header;
			for (const EdgeRef& latch : latches)
			{
				loop.latch_blocks.push_back(latch.source);
				loop.event_checked_latches += latch.transfer->event_horizon_check;
			}
			std::sort(loop.latch_blocks.begin(), loop.latch_blocks.end());
			loop.latch_blocks.erase(std::unique(loop.latch_blocks.begin(),
				loop.latch_blocks.end()), loop.latch_blocks.end());
			for (u32 block = 0; block < block_count; block++)
			{
				if ((mask & (u64{1} << block)) == 0)
					continue;
				loop.blocks.push_back(block);
				loop.source_instructions +=
					static_cast<u32>(program.blocks[block].source.size());
				loop.static_scaled_cycles += program.blocks[block].scaled_cycle_cost;
			}
			loop_masks.push_back(mask);
			plan.loops.push_back(std::move(loop));
		}

		for (u32 left = 0; left < loop_masks.size(); left++)
		{
			for (u32 right = left + 1; right < loop_masks.size(); right++)
			{
				const u64 overlap = loop_masks[left] & loop_masks[right];
				if (overlap != 0 && !MaskContains(loop_masks[left], loop_masks[right]) &&
					!MaskContains(loop_masks[right], loop_masks[left]))
				{
					return Fail(Failure::OverlappingLoops,
						"reducible loops overlap without a nesting relation",
						plan.loops[left].header_block,
						plan.loops[right].header_block);
				}
			}
		}
		for (u32 child = 0; child < loop_masks.size(); child++)
		{
			u32 parent = INVALID_BLOCK;
			u32 parent_size = UINT32_MAX;
			for (u32 candidate = 0; candidate < loop_masks.size(); candidate++)
			{
				if (candidate == child ||
					!MaskContains(loop_masks[candidate], loop_masks[child]) ||
					loop_masks[candidate] == loop_masks[child])
				{
					continue;
				}
				const u32 size = std::popcount(loop_masks[candidate]);
				if (size < parent_size)
				{
					parent = candidate;
					parent_size = size;
				}
			}
			plan.loops[child].parent_loop = parent;
		}
		for (u32 loop_index = 0; loop_index < plan.loops.size(); loop_index++)
		{
			u32 depth = 1;
			u32 parent = plan.loops[loop_index].parent_loop;
			for (u32 guard = 0; parent != INVALID_BLOCK && guard < plan.loops.size(); guard++)
			{
				depth++;
				parent = plan.loops[parent].parent_loop;
			}
			plan.loops[loop_index].depth = depth;
			plan.maximum_loop_depth = std::max(plan.maximum_loop_depth, depth);
		}
		plan.has_repeated_core = !plan.loops.empty();

		u64 repeated_mask = 0;
		for (u64 mask : loop_masks)
			repeated_mask |= mask;
		plan.repeated_blocks = std::popcount(repeated_mask);
		std::vector<u64> reachable(block_count, 0);
		for (const EdgeRef& edge : edges)
			reachable[edge.source] |= u64{1} << edge.target;
		for (u32 intermediate = 0; intermediate < block_count; intermediate++)
			for (u32 source = 0; source < block_count; source++)
				if ((reachable[source] & (u64{1} << intermediate)) != 0)
					reachable[source] |= reachable[intermediate];
		for (u32 block = 0; block < block_count; block++)
		{
			if ((repeated_mask & (u64{1} << block)) != 0)
				continue;
			bool reaches_repeated = false;
			bool reached_from_repeated = false;
			for (u32 repeated = 0; repeated < block_count; repeated++)
			{
				if ((repeated_mask & (u64{1} << repeated)) == 0)
					continue;
				reaches_repeated |=
					(reachable[block] & (u64{1} << repeated)) != 0;
				reached_from_repeated |=
					(reachable[repeated] & (u64{1} << block)) != 0;
			}
			if (reached_from_repeated)
				plan.suffix_blocks++;
			else if (reaches_repeated)
				plan.prefix_blocks++;
			else
				plan.other_acyclic_blocks++;
		}

		auto append_external = [&](RegionExecution::ExitSiteKind kind, u32 block,
			u32 ordinal, const Transfer& transfer) -> BuildResult {
			if (transfer.target_block != INVALID_BLOCK)
				return {};
			return AppendExit(program, execution.plan, kind, block, ordinal, &plan);
		};
		for (u32 block_index = 0; block_index < block_count; block_index++)
		{
			const Block& block = program.blocks[block_index];
			for (u32 ordinal = 0; ordinal < block.guarded_exits.size(); ordinal++)
			{
				BuildResult result = append_external(
					RegionExecution::ExitSiteKind::Guarded, block_index, ordinal,
					block.guarded_exits[ordinal]);
				if (!result)
					return result;
			}
			for (u32 ordinal = 0; ordinal < block.observer_exits.size(); ordinal++)
			{
				BuildResult result = append_external(
					RegionExecution::ExitSiteKind::Observer, block_index, ordinal,
					block.observer_exits[ordinal].transfer);
				if (!result)
					return result;
			}
			for (u32 ordinal = 0; ordinal < block.memory_exits.size(); ordinal++)
			{
				BuildResult result = append_external(
					RegionExecution::ExitSiteKind::Memory, block_index, ordinal,
					block.memory_exits[ordinal].transfer);
				if (!result)
					return result;
			}
			BuildResult taken = append_external(
				RegionExecution::ExitSiteKind::Taken, block_index, 0,
				block.terminator.taken);
			if (!taken)
				return taken;
			if (block.terminator.kind == TerminatorKind::Branch)
			{
				BuildResult not_taken = append_external(
					RegionExecution::ExitSiteKind::NotTaken, block_index, 0,
					block.terminator.not_taken);
				if (!not_taken)
					return not_taken;
			}
		}
		if (plan.completion_exits == 0)
			return Fail(Failure::MissingCompletion,
				"operation island has no exact external RegionBoundary completion");

		if (repeated_mask != 0)
		{
			for (const Exit& exit : plan.exits)
			{
				if (exit.exit_class != ExitClass::Completion ||
					exit.block >= block_count)
				{
					continue;
				}
				if ((repeated_mask & (u64{1} << exit.block)) != 0)
				{
					plan.completion_reachable_from_repeated_core = true;
					break;
				}
				for (u32 repeated = 0; repeated < block_count; repeated++)
				{
					if ((repeated_mask & (u64{1} << repeated)) != 0 &&
						(reachable[repeated] & (u64{1} << exit.block)) != 0)
					{
						plan.completion_reachable_from_repeated_core = true;
						break;
					}
				}
			}
		}

		BuildResult result{};
		result.plan = std::move(plan);
		return result;
	}
} // namespace VitaEE::SemanticIsland
