// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#include "pcsx2/vita/VitaEeSemanticKernel.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <map>
#include <set>

namespace VitaEE::SemanticKernel
{
	namespace
	{
		using namespace RegionIR;

		struct Edge
		{
			u32 source = INVALID_BLOCK;
			u32 target = INVALID_BLOCK;
			const Transfer* transfer = nullptr;
		};

		BuildResult Fail(Failure failure, std::string detail,
			u32 block = INVALID_BLOCK, ValueId value = INVALID_VALUE)
		{
			BuildResult result{};
			result.failure = failure;
			result.block = block;
			result.value = value;
			result.detail = std::move(detail);
			return result;
		}

		template <typename Callback>
		void VisitTransfers(const Terminator& terminator, Callback&& callback)
		{
			callback(terminator.taken);
			if (terminator.kind == TerminatorKind::Branch)
				callback(terminator.not_taken);
			if (terminator.kind == TerminatorKind::RegisterJump)
			{
				for (const Transfer& transfer : terminator.register_targets)
					callback(transfer);
			}
		}

		bool IsPureInvariantOpcode(Opcode opcode)
		{
			switch (opcode)
			{
				case Opcode::ConstantI1:
				case Opcode::ConstantI32:
				case Opcode::ConstantI64:
				case Opcode::ConstantAddress:
				case Opcode::ExtractLow32:
				case Opcode::ExtractLow64:
				case Opcode::ExtractHigh64:
				case Opcode::ReplaceLow64:
				case Opcode::ReplaceHigh64:
				case Opcode::BitcastI32ToF32Bits:
				case Opcode::BitcastF32BitsToI32:
				case Opcode::BitcastI128ToVuF32x4Bits:
				case Opcode::BitcastVuF32x4BitsToI128:
				case Opcode::SignExtend32To64:
				case Opcode::ZeroExtend32To64:
				case Opcode::Truncate64To32:
				case Opcode::Add32:
				case Opcode::Add64:
				case Opcode::Sub32:
				case Opcode::Sub64:
				case Opcode::And32:
				case Opcode::And64:
				case Opcode::Or64:
				case Opcode::Xor32:
				case Opcode::Xor64:
				case Opcode::Nor64:
				case Opcode::And128:
				case Opcode::Or128:
				case Opcode::Xor128:
				case Opcode::Nor128:
				case Opcode::ShiftLeft32:
				case Opcode::ShiftRightLogical32:
				case Opcode::ShiftRightArithmetic32:
				case Opcode::ShiftLeft64:
				case Opcode::ShiftRightLogical64:
				case Opcode::ShiftRightArithmetic64:
				case Opcode::ShiftLeft32Variable:
				case Opcode::ShiftRightLogical32Variable:
				case Opcode::ShiftRightArithmetic32Variable:
				case Opcode::ShiftLeft64Variable:
				case Opcode::ShiftRightLogical64Variable:
				case Opcode::ShiftRightArithmetic64Variable:
				case Opcode::Select64:
				case Opcode::AddressFromI32:
				case Opcode::EffectiveAddress32:
					return true;
				default:
					return false;
			}
		}

		class Analyzer
		{
		public:
			Analyzer(const Program& program,
				const RegionExecution::Plan& execution,
				const RegionMemoryPlan::Plan& memory)
				: m_program(program)
				, m_execution(execution)
				, m_memory(memory)
				, m_definitions(program.value_count, nullptr)
				, m_defining_blocks(program.value_count, INVALID_BLOCK)
				, m_invariant_state(program.value_count, 0)
			{
				m_predecessors.resize(program.blocks.size());
				for (u32 block_index = 0; block_index < program.blocks.size(); block_index++)
				{
					const Block& block = program.blocks[block_index];
					for (const Node& node : block.nodes)
					{
						if (node.id < m_definitions.size())
						{
							m_definitions[node.id] = &node;
							m_defining_blocks[node.id] = block_index;
						}
					}
					VisitInternalTransfers(block.terminator,
						[&](const Transfer& transfer, u8) {
							m_edges.push_back({block_index, transfer.target_block,
								&transfer});
							m_predecessors[transfer.target_block].push_back(
								block_index);
							return true;
						});
				}
			}

			BuildResult Run()
			{
				if (!m_memory.timing.valid || !m_memory.control.valid ||
					m_memory.timing.header_block != m_memory.control.header_block ||
					m_memory.timing.header_block >= m_program.blocks.size() ||
					m_memory.timing.backedge_source_block >= m_program.blocks.size())
				{
					return Fail(Failure::MissingNaturalLoop,
						"no exact counted natural-loop timing contract");
				}

				m_header = m_memory.timing.header_block;
				m_latch = m_memory.timing.backedge_source_block;
				BuildResult topology = BuildNaturalLoop();
				if (!topology)
					return topology;
				BuildResult path = BuildDeterministicPath();
				bool bounded_search = false;
				BuildResult memory{};
				if (!path && path.failure == Failure::NonDeterministicIteration)
				{
					ResetRepeatedPath();
					memory = BuildBoundedEqualSearch();
					if (!memory)
						return memory;
					bounded_search = true;
				}
				else if (!path)
					return path;
				if (!bounded_search)
				{
					const bool contains_load = LoopContainsLoad();
					memory = BuildPatternFill();
					if (!memory && contains_load)
					{
						ResetMemoryDescriptor();
						memory = BuildForwardCopy();
					}
					if (!memory && LoopContainsVu0VectorMemory())
					{
						ResetMemoryDescriptor();
						memory = BuildVu0AffineTransform();
					}
					if (!memory && LoopContainsCop1Arithmetic())
					{
						ResetMemoryDescriptor();
						memory = BuildCop1Stream();
					}
				}
				if (!memory)
					return memory;
				if (m_has_vu0_observers && m_plan.kind != Kind::Vu0AffineTransform)
				{
					return Fail(Failure::ObserverInsideIteration,
						"only an exact VU0 affine kernel may coalesce VU0 idle observers");
				}
				if (m_signed_overflow_exits != 0 &&
					m_plan.kind != Kind::Vu0AffineTransform)
				{
					return Fail(Failure::ObserverInsideIteration,
						"only an exact affine transform currently preflights signed ADDI exits");
				}
				BuildResult state = m_plan.kind == Kind::Cop1Stream ?
					BuildResult{} : BuildStateContract();
				if (!state)
					return state;

				m_plan.header_block = m_header;
				m_plan.latch_block = m_latch;
				m_plan.completion_block = m_completion->target_block;
				if (m_completion->pc < m_definitions.size() &&
					m_definitions[m_completion->pc] &&
					m_definitions[m_completion->pc]->opcode == Opcode::ConstantAddress)
				{
					m_plan.completion_pc = static_cast<u32>(
						m_definitions[m_completion->pc]->literal);
				}
				else if (m_plan.completion_block < m_program.blocks.size())
				{
					m_plan.completion_pc = m_program.blocks[m_plan.completion_block].pc;
				}
				if (m_plan.completion_pc == 0)
					return Fail(Failure::InexactEventPhase,
						"completion edge has no exact verifier-owned PC", m_latch);
				m_plan.control = m_memory.control;
				m_plan.complete_preflight = true;
				m_plan.exact_event_phase = true;
				BuildResult result{};
				result.plan = std::move(m_plan);
				return result;
			}

		private:
			bool LoopContainsLoad() const
			{
				for (u32 block : m_plan.iteration_blocks)
				{
					for (const Node& node : m_program.blocks[block].nodes)
					{
						if (node.opcode == Opcode::MemoryLoad)
							return true;
					}
				}
				return false;
			}

			bool LoopContainsVu0VectorMemory() const
			{
				for (u32 block : m_plan.iteration_blocks)
				{
					for (const Node& node : m_program.blocks[block].nodes)
					{
						if ((node.opcode == Opcode::MemoryLoad ||
							 node.opcode == Opcode::MemoryStore) &&
							(static_cast<MemoryAccessKind>(node.immediate) ==
								MemoryAccessKind::LoadVu0Vector ||
							 static_cast<MemoryAccessKind>(node.immediate) ==
								MemoryAccessKind::StoreVu0Vector))
						{
							return true;
						}
					}
				}
				return false;
			}

			bool LoopContainsCop1Arithmetic() const
			{
				for (u32 block : m_plan.iteration_blocks)
				{
					for (const Node& node : m_program.blocks[block].nodes)
					{
						if (node.opcode == Opcode::Cop1AddRaw ||
							node.opcode == Opcode::Cop1SubRaw ||
							node.opcode == Opcode::Cop1MulRaw)
						{
							return true;
						}
					}
				}
				return false;
			}

			void ResetMemoryDescriptor()
			{
				m_plan.kind = Kind::None;
				m_plan.pattern_streams.clear();
				m_plan.copy_streams.clear();
				m_plan.vu0_affine_streams.clear();
				m_plan.cop1_streams.clear();
				m_plan.bounded_equal_searches.clear();
				m_plan.final_load_state.clear();
				m_plan.memory_operations_per_iteration = 0;
				m_plan.bytes_per_iteration = 0;
				m_plan.read_bytes_per_iteration = 0;
				m_plan.write_bytes_per_iteration = 0;
				m_plan.complete_preflight = false;
				m_plan.requires_disjoint_write_streams = false;
				m_plan.requires_forward_copy_batch_alias_guard = false;
			}

			void ResetRepeatedPath()
			{
				m_plan.iteration_blocks.clear();
				m_plan.source_instructions_per_iteration = 0;
				m_plan.repeated_scaled_cycles = 0;
				m_plan.completion_scaled_cycles = 0;
				m_backedge = nullptr;
				m_completion = nullptr;
				m_search_match = nullptr;
				m_search_continue = nullptr;
				m_search_block = INVALID_BLOCK;
				m_search_loop_mask = 0;
			}

			bool HasExactMemoryExit(u32 block, ValueId operation) const
			{
				if (block >= m_program.blocks.size())
					return false;
				const Block& source = m_program.blocks[block];
				for (u32 ordinal = 0; ordinal < source.memory_exits.size(); ordinal++)
				{
					if (source.memory_exits[ordinal].operation != operation)
						continue;
					const auto found = std::find_if(m_execution.exits.begin(),
						m_execution.exits.end(), [&](const RegionExecution::ExitSite& site) {
							return site.kind == RegionExecution::ExitSiteKind::Memory &&
								site.block == block && site.ordinal == ordinal;
						});
					if (found == m_execution.exits.end())
						return false;
					RegionExecution::ExitContractView contract{};
					return RegionExecution::ResolveExitContract(m_program, *found, &contract) &&
						contract.transfer == &source.memory_exits[ordinal].transfer &&
						contract.state != nullptr && contract.cycle_commit_deferred;
				}
				return false;
			}

			u32 MemoryOperationOrder(ValueId operation) const
			{
				u32 ordinal = 0;
				for (u32 block : m_plan.iteration_blocks)
				{
					for (const Node& node : m_program.blocks[block].nodes)
					{
						if (node.opcode != Opcode::MemoryLoad &&
							node.opcode != Opcode::MemoryStore)
							continue;
						if (node.id == operation)
							return ordinal;
						ordinal++;
					}
				}
				return UINT32_MAX;
			}

			BuildResult BuildNaturalLoop()
			{
				const size_t count = m_program.blocks.size();
				if (count == 0 || count > 63)
					return Fail(Failure::IrreducibleLoop, "CFG exceeds loop bitset bound");
				const u64 all = (u64{1} << count) - 1;
				std::vector<u64> dominators(count, all);
				dominators[m_program.entry_block] = u64{1} << m_program.entry_block;
				bool changed = true;
				while (changed)
				{
					changed = false;
					for (u32 block = 0; block < count; block++)
					{
						if (block == m_program.entry_block)
							continue;
						u64 incoming = all;
						if (m_predecessors[block].empty())
							incoming = 0;
						else
							for (u32 predecessor : m_predecessors[block])
								incoming &= dominators[predecessor];
						const u64 next = incoming | (u64{1} << block);
						if (next != dominators[block])
						{
							dominators[block] = next;
							changed = true;
						}
					}
				}
				if ((dominators[m_latch] & (u64{1} << m_header)) == 0)
					return Fail(Failure::IrreducibleLoop,
						"loop header does not dominate its latch", m_latch);

				u32 latch_count = 0;
				for (const Edge& edge : m_edges)
				{
					if (edge.target == m_header &&
						(dominators[edge.source] & (u64{1} << m_header)) != 0)
					{
						latch_count++;
					}
				}
				if (latch_count != 1)
					return Fail(Failure::MultipleLatches,
						"semantic kernels currently require one exact latch", m_header);

				m_loop_mask = (u64{1} << m_header) | (u64{1} << m_latch);
				std::vector<u32> pending = {m_latch};
				while (!pending.empty())
				{
					const u32 block = pending.back();
					pending.pop_back();
					for (u32 predecessor : m_predecessors[block])
					{
						if (predecessor == m_header ||
							(m_loop_mask & (u64{1} << predecessor)) != 0)
						{
							continue;
						}
						if ((dominators[predecessor] & (u64{1} << m_header)) == 0)
							return Fail(Failure::IrreducibleLoop,
								"natural loop has a non-dominated predecessor", predecessor);
						m_loop_mask |= u64{1} << predecessor;
						pending.push_back(predecessor);
					}
				}
				return {};
			}

			bool InLoop(u32 block) const
			{
				return block < 64 && (m_loop_mask & (u64{1} << block)) != 0;
			}

			BuildResult BuildDeterministicPath()
			{
				u32 block = m_header;
				std::set<u32> visited;
				for (;;)
				{
					if (!InLoop(block) || !visited.insert(block).second)
						return Fail(Failure::NonDeterministicIteration,
							"repeated path does not visit each loop block once", block);
					const Block& source = m_program.blocks[block];
					for (u32 exit_index = 0;
						exit_index < source.guarded_exits.size(); exit_index++)
					{
						const Node* exit = nullptr;
						for (const Node& node : source.nodes)
						{
							if (node.opcode == Opcode::ExitIfTrue &&
								node.immediate == exit_index)
							{
								if (exit)
									return Fail(Failure::ObserverInsideIteration,
										"guarded exit has multiple executable owners", block,
										node.id);
								exit = &node;
							}
						}
						const Node* overflow = exit && exit->operand_count == 1 ?
							Definition(exit->operands[0]) : nullptr;
						const Node* add = nullptr;
						if (overflow && overflow->operand_count == 2)
						{
							for (const Node& candidate : source.nodes)
							{
								if (candidate.opcode == Opcode::Add32 &&
									candidate.operand_count == 2 &&
									candidate.operands[0] == overflow->operands[0] &&
									candidate.operands[1] == overflow->operands[1])
								{
									if (add)
										return Fail(Failure::ObserverInsideIteration,
											"signed ADDI guard has ambiguous wrapped value",
											block, overflow->id);
									add = &candidate;
								}
							}
						}
						if (!exit || !overflow ||
							overflow->opcode != Opcode::SignedAddOverflow32 ||
							!add || add->opcode != Opcode::Add32)
						{
							return Fail(Failure::ObserverInsideIteration,
								"loop contains a non-affine conditional exception", block,
								exit ? exit->id : INVALID_VALUE);
						}
						m_signed_overflow_exits++;
					}
					for (const ObserverExit& observer : source.observer_exits)
					{
						if (observer.operation >= m_definitions.size() ||
							!m_definitions[observer.operation] ||
							m_definitions[observer.operation]->opcode !=
								Opcode::Vu0RequireIdle)
						{
							return Fail(Failure::ObserverInsideIteration,
								"loop contains a non-VU0 synchronization observer", block,
								observer.operation);
						}
						m_has_vu0_observers = true;
					}
					m_plan.iteration_blocks.push_back(block);
					m_plan.source_instructions_per_iteration +=
						static_cast<u32>(source.source.size());

					std::vector<const Transfer*> transfers;
					VisitTransfers(source.terminator,
						[&](const Transfer& transfer) { transfers.push_back(&transfer); });
					if (block == m_latch)
					{
						if (source.terminator.kind != TerminatorKind::Branch ||
							transfers.size() != 2)
						{
							return Fail(Failure::NonDeterministicIteration,
								"latch is not one conditional backedge plus completion", block);
						}
						for (const Transfer* transfer : transfers)
						{
							if (transfer->target_block == m_header)
								m_backedge = transfer;
							else if (!InLoop(transfer->target_block))
								m_completion = transfer;
						}
						if (!m_backedge || !m_completion)
							return Fail(Failure::NonDeterministicIteration,
								"latch has no unique completion edge", block);
						break;
					}

					if (transfers.size() != 1 || !InLoop(transfers[0]->target_block) ||
						transfers[0]->event_horizon_check)
					{
						return Fail(Failure::NonDeterministicIteration,
							"iteration has a branch or scheduler seam before its latch", block);
					}
					block = transfers[0]->target_block;
				}

				if (visited.size() != static_cast<size_t>(std::popcount(m_loop_mask)))
					return Fail(Failure::NonDeterministicIteration,
						"natural loop contains a cold or alternate internal arm");
				if (!m_backedge->event_horizon_check ||
					!m_completion->event_horizon_check)
				{
					return Fail(Failure::InexactEventPhase,
						"latch does not own the exact PCSX2 scheduler observation", m_latch);
				}
				for (size_t slot = 0; slot < RegionExecution::STATE_SLOT_COUNT; slot++)
				{
					if (RegionExecution::StateValue(m_backedge->state, slot) !=
						RegionExecution::StateValue(m_completion->state, slot))
					{
						return Fail(Failure::InexactEventPhase,
							"backedge and completion publish different architectural state",
							m_latch);
					}
				}
				if (m_backedge->state.memory_effect !=
					m_completion->state.memory_effect)
				{
					return Fail(Failure::InexactEventPhase,
						"backedge and completion publish different memory effects", m_latch);
				}

				for (u32 current : m_plan.iteration_blocks)
				{
					const Block& source = m_program.blocks[current];
					u32 repeated_cost = source.scaled_cycle_cost;
					u32 completion_cost = source.scaled_cycle_cost;
					if (current == m_latch && m_backedge == &source.terminator.not_taken)
						repeated_cost = source.not_taken_scaled_cycle_cost;
					if (current == m_latch && m_completion == &source.terminator.not_taken)
						completion_cost = source.not_taken_scaled_cycle_cost;
					if (m_plan.repeated_scaled_cycles > UINT32_MAX - repeated_cost ||
						m_plan.completion_scaled_cycles > UINT32_MAX - completion_cost)
					{
						return Fail(Failure::InexactEventPhase,
							"iteration cycle cost overflows", current);
					}
					m_plan.repeated_scaled_cycles += repeated_cost;
					m_plan.completion_scaled_cycles += completion_cost;
				}
				if (m_plan.repeated_scaled_cycles == 0 ||
					m_plan.repeated_scaled_cycles !=
						m_memory.timing.maximum_scaled_cycles)
				{
					return Fail(Failure::InexactEventPhase,
						"memory timing is not the exact repeated-path cycle cost", m_latch);
				}
				return {};
			}

			u32 TransferCycleCost(const Block& block,
				const Transfer* transfer) const
			{
				return block.terminator.kind == TerminatorKind::Branch &&
					transfer == &block.terminator.not_taken ?
					block.not_taken_scaled_cycle_cost : block.scaled_cycle_cost;
			}

			bool ValueDependsOn(ValueId value, ValueId dependency,
				std::set<ValueId>* visiting) const
			{
				if (!visiting)
					return false;
				value = ResolveLoopParameter(value);
				dependency = ResolveLoopParameter(dependency);
				if (value == dependency)
					return true;
				if (!visiting->insert(value).second || value >= m_definitions.size() ||
					!m_definitions[value])
				{
					return false;
				}
				const Node& node = *m_definitions[value];
				bool found = false;
				for (u8 operand = 0; operand < node.operand_count && !found; operand++)
					found = ValueDependsOn(node.operands[operand], dependency, visiting);
				visiting->erase(value);
				return found;
			}

			bool ValueDependsOn(ValueId value, ValueId dependency) const
			{
				std::set<ValueId> visiting;
				return ValueDependsOn(value, dependency, &visiting);
			}

			bool KnownBoolean(ValueId value, bool* result) const
			{
				if (!result)
					return false;
				if (value >= m_definitions.size() || !m_definitions[value])
					return false;
				const Node& node = *m_definitions[value];
				if (node.opcode == Opcode::ConstantI1 ||
					node.opcode == Opcode::ConstantI32 ||
					node.opcode == Opcode::ConstantI64)
				{
					*result = node.literal != 0;
					return true;
				}
				if (node.operand_count == 2 &&
					EquivalentExpression(node.operands[0], node.operands[1]))
				{
					switch (node.opcode)
					{
						case Opcode::CompareEqual64:
						case Opcode::CompareSignedLessEqualZero64:
						case Opcode::CompareSignedGreaterEqualZero64:
							*result = true;
							return true;
						case Opcode::CompareNotEqual64:
						case Opcode::CompareSignedLess64:
						case Opcode::CompareUnsignedLess64:
						case Opcode::CompareSignedLessZero64:
						case Opcode::CompareSignedGreaterZero64:
							*result = false;
							return true;
						default:
							break;
					}
				}
				return false;
			}

			bool EquivalentExpression(ValueId left, ValueId right,
				std::set<std::pair<ValueId, ValueId>>* visiting) const
			{
				if (left == right)
					return true;
				if (!visiting || left >= m_definitions.size() ||
					right >= m_definitions.size() || !m_definitions[left] ||
					!m_definitions[right])
				{
					return false;
				}
				const std::pair key{left, right};
				if (!visiting->insert(key).second)
					return false;
				const Node& a = *m_definitions[left];
				const Node& b = *m_definitions[right];
				if (a.opcode == Opcode::Parameter || b.opcode == Opcode::Parameter ||
					a.opcode == Opcode::MemoryLoad || b.opcode == Opcode::MemoryLoad ||
					a.opcode == Opcode::MemoryLoadValue ||
					b.opcode == Opcode::MemoryLoadValue || a.opcode != b.opcode ||
					a.type != b.type || a.operand_count != b.operand_count ||
					a.immediate != b.immediate || a.literal != b.literal)
				{
					visiting->erase(key);
					return false;
				}
				bool equal = true;
				for (u8 operand = 0; operand < a.operand_count && equal; operand++)
					equal = EquivalentExpression(a.operands[operand], b.operands[operand],
						visiting);
				visiting->erase(key);
				return equal;
			}

			bool EquivalentExpression(ValueId left, ValueId right) const
			{
				std::set<std::pair<ValueId, ValueId>> visiting;
				return EquivalentExpression(left, right, &visiting);
			}

			template <typename Callback>
			void VisitFeasibleTransfers(const Block& block,
				Callback&& callback) const
			{
				if (block.terminator.kind == TerminatorKind::Branch)
				{
					bool condition = false;
					if (KnownBoolean(block.terminator.condition, &condition))
					{
						callback(condition ? block.terminator.taken :
							block.terminator.not_taken);
						return;
					}
				}
				VisitTransfers(block.terminator,
					[&](const Transfer& transfer) { callback(transfer); });
			}

			bool IsFeasibleEdge(const Edge& edge) const
			{
				if (edge.source >= m_program.blocks.size() || !edge.transfer)
					return false;
				const Block& source = m_program.blocks[edge.source];
				if (source.terminator.kind != TerminatorKind::Branch)
					return true;
				bool condition = false;
				if (!KnownBoolean(source.terminator.condition, &condition))
					return true;
				return edge.transfer == (condition ? &source.terminator.taken :
					&source.terminator.not_taken);
			}

			bool BuildSearchRepeatedMask()
			{
				m_search_loop_mask = u64{1} << m_latch;
				bool changed = true;
				while (changed)
				{
					changed = false;
					for (u32 block = 0; block < m_program.blocks.size(); block++)
					{
						if (!InLoop(block) ||
							(m_search_loop_mask & (u64{1} << block)) != 0)
						{
							continue;
						}
						bool reaches = false;
						VisitFeasibleTransfers(m_program.blocks[block],
							[&](const Transfer& transfer) {
								reaches |= transfer.target_block < 64 &&
									(m_search_loop_mask &
										(u64{1} << transfer.target_block)) != 0;
							});
						if (reaches)
						{
							m_search_loop_mask |= u64{1} << block;
							changed = true;
						}
					}
				}
				return (m_search_loop_mask & (u64{1} << m_header)) != 0;
			}

			bool InSearchLoop(u32 block) const
			{
				return block < 64 &&
					(m_search_loop_mask & (u64{1} << block)) != 0;
			}

			bool MatchHeaderAffineAddress(ValueId address, u8* gpr,
				s32* offset) const
			{
				if (!gpr || !offset)
					return false;
				const Node* effective = Definition(address);
				if (!effective || effective->opcode != Opcode::EffectiveAddress32 ||
					effective->operand_count != 2)
				{
					return false;
				}
				auto constant = [&](ValueId value, s32* literal) {
					const Node* node = Definition(value);
					if (!node || node->opcode != Opcode::ConstantI32)
						return false;
					*literal = static_cast<s32>(static_cast<u32>(node->literal));
					return true;
				};
				auto header_gpr = [&](ValueId value, u8* index) {
					const Node* extract = Definition(value);
					if (!extract || extract->opcode != Opcode::ExtractLow32 ||
						extract->operand_count != 1)
					{
						return false;
					}
					const ValueId parameter =
						ResolveLoopParameter(extract->operands[0]);
					const StateMap& header = m_program.blocks[m_header].parameters;
					for (u32 candidate = 1; candidate < header.gpr.size(); candidate++)
					{
						if (parameter == header.gpr[candidate])
						{
							*index = static_cast<u8>(candidate);
							return true;
						}
					}
					return false;
				};
				u8 index = 0;
				s32 displacement = 0;
				if ((header_gpr(effective->operands[0], &index) &&
					 constant(effective->operands[1], &displacement)) ||
					(header_gpr(effective->operands[1], &index) &&
					 constant(effective->operands[0], &displacement)))
				{
					*gpr = index;
					*offset = displacement;
					return true;
				}
				return false;
			}

			bool IsBoundedSearchNodeSupported(const Node& node) const
			{
				switch (node.opcode)
				{
					case Opcode::Parameter:
					case Opcode::ConstantI1:
					case Opcode::ConstantI32:
					case Opcode::ConstantI64:
					case Opcode::ConstantAddress:
					case Opcode::NoEffect:
					case Opcode::ExtractLow32:
					case Opcode::ExtractLow64:
					case Opcode::ExtractHigh64:
					case Opcode::ReplaceLow64:
					case Opcode::ReplaceHigh64:
					case Opcode::SignExtend32To64:
					case Opcode::ZeroExtend32To64:
					case Opcode::Truncate64To32:
					case Opcode::Add32:
					case Opcode::Add64:
					case Opcode::Sub32:
					case Opcode::Sub64:
					case Opcode::And32:
					case Opcode::And64:
					case Opcode::Or64:
					case Opcode::Xor32:
					case Opcode::Xor64:
					case Opcode::Nor64:
					case Opcode::ShiftLeft32:
					case Opcode::ShiftRightLogical32:
					case Opcode::ShiftRightArithmetic32:
					case Opcode::ShiftLeft64:
					case Opcode::ShiftRightLogical64:
					case Opcode::ShiftRightArithmetic64:
					case Opcode::ShiftLeft32Variable:
					case Opcode::ShiftRightLogical32Variable:
					case Opcode::ShiftRightArithmetic32Variable:
					case Opcode::ShiftLeft64Variable:
					case Opcode::ShiftRightLogical64Variable:
					case Opcode::ShiftRightArithmetic64Variable:
					case Opcode::Select64:
					case Opcode::CompareEqual64:
					case Opcode::CompareNotEqual64:
					case Opcode::CompareSignedLess64:
					case Opcode::CompareUnsignedLess64:
					case Opcode::CompareSignedLessEqualZero64:
					case Opcode::CompareSignedGreaterZero64:
					case Opcode::CompareSignedLessZero64:
					case Opcode::CompareSignedGreaterEqualZero64:
					case Opcode::AddressFromI32:
					case Opcode::EffectiveAddress32:
					case Opcode::MemoryLoad:
					case Opcode::MemoryLoadValue:
					case Opcode::MemoryStore:
					case Opcode::BindGpr:
					case Opcode::AdvanceCycles:
						return true;
					default:
						return false;
				}
			}

			BuildResult BuildBoundedEqualSearch()
			{
				if (!m_memory.control.valid || !m_memory.timing.valid)
				{
					return Fail(Failure::UnsupportedBoundedSearchShape,
						"bounded search has no exact counted-loop contract", m_header);
				}
				if (!BuildSearchRepeatedMask())
					return Fail(Failure::UnsupportedBoundedSearchShape,
						"bounded search header cannot reach its latch", m_header);
				u32 block = m_header;
				std::set<u32> visited;
				u32 repeated_cycles = 0;
				u32 match_cycles = 0;
				while (true)
				{
					if (!InSearchLoop(block) || !visited.insert(block).second)
					{
						return Fail(Failure::UnsupportedBoundedSearchShape,
							"bounded search repeated path is not a simple acyclic body",
							block);
					}
					const Block& source = m_program.blocks[block];
					if (!source.guarded_exits.empty() || !source.observer_exits.empty())
					{
						return Fail(Failure::ObserverInsideIteration,
							"bounded search contains an instruction observer", block);
					}
					m_plan.iteration_blocks.push_back(block);
					m_plan.source_instructions_per_iteration +=
						static_cast<u32>(source.source.size());

					std::vector<const Transfer*> transfers;
					VisitFeasibleTransfers(source,
						[&](const Transfer& transfer) { transfers.push_back(&transfer); });
					if (block == m_latch)
					{
						if (source.terminator.kind != TerminatorKind::Branch ||
							transfers.size() != 2)
						{
							return Fail(Failure::UnsupportedBoundedSearchShape,
								"bounded search latch is not one backedge and one exit",
								block);
						}
						for (const Transfer* transfer : transfers)
						{
							if (transfer->target_block == m_header)
								m_backedge = transfer;
							else if (!InSearchLoop(transfer->target_block))
								m_completion = transfer;
						}
						if (!m_backedge || !m_completion ||
							!m_backedge->event_horizon_check ||
							!m_completion->event_horizon_check)
						{
							return Fail(Failure::InexactEventPhase,
								"bounded search latch lacks its exact scheduler edges",
								block);
						}
						repeated_cycles += TransferCycleCost(source, m_backedge);
						m_plan.completion_scaled_cycles =
							repeated_cycles - TransferCycleCost(source, m_backedge) +
							TransferCycleCost(source, m_completion);
						break;
					}

					const Transfer* in_loop = nullptr;
					const Transfer* out_of_loop = nullptr;
					for (const Transfer* transfer : transfers)
					{
						if (InSearchLoop(transfer->target_block))
						{
							if (in_loop)
								return Fail(Failure::UnsupportedBoundedSearchShape,
									"bounded search has two repeated successors", block);
							in_loop = transfer;
						}
						else
						{
							if (out_of_loop)
								return Fail(Failure::UnsupportedBoundedSearchShape,
									"bounded search has two early exits", block);
							out_of_loop = transfer;
						}
					}
					if (!in_loop)
						return Fail(Failure::UnsupportedBoundedSearchShape,
							"bounded search repeated path terminates before its latch",
							block);
					if (out_of_loop)
					{
						if (m_search_match ||
							source.terminator.kind != TerminatorKind::Branch)
						{
							return Fail(Failure::UnsupportedBoundedSearchShape,
								"bounded search has more than one data-dependent exit",
								block);
						}
						m_search_match = out_of_loop;
						m_search_continue = in_loop;
						m_search_block = block;
						match_cycles = repeated_cycles +
							TransferCycleCost(source, out_of_loop);
					}
					repeated_cycles += TransferCycleCost(source, in_loop);
					block = in_loop->target_block;
				}

				if (!m_search_match || !m_search_continue ||
					visited.size() !=
						static_cast<size_t>(std::popcount(m_search_loop_mask)) ||
					repeated_cycles == 0 || match_cycles == 0 ||
					repeated_cycles > m_memory.timing.maximum_scaled_cycles)
				{
					return Fail(Failure::UnsupportedBoundedSearchShape,
						"bounded search repeated path mismatch visited=" +
							std::to_string(visited.size()) + "/" +
							std::to_string(std::popcount(m_search_loop_mask)) +
							" cycles=" + std::to_string(repeated_cycles) + "/" +
							std::to_string(m_memory.timing.maximum_scaled_cycles) +
							" match=" + std::to_string(match_cycles),
						m_search_block);
				}

				ValueId load_operation = INVALID_VALUE;
				ValueId load_value = INVALID_VALUE;
				u32 load_block = INVALID_BLOCK;
				for (u32 current : m_plan.iteration_blocks)
				{
					for (const Node& node : m_program.blocks[current].nodes)
					{
						if (!IsBoundedSearchNodeSupported(node))
						{
							return Fail(Failure::UnsupportedBoundedSearchDataflow,
								"bounded search contains an unsupported scalar operation",
								current, node.id);
						}
						if (node.opcode == Opcode::MemoryStore)
							return Fail(Failure::UnsupportedBoundedSearchShape,
								"bounded search may not write memory", current, node.id);
						if (node.opcode == Opcode::MemoryLoad)
						{
							if (load_operation != INVALID_VALUE)
								return Fail(Failure::UnsupportedBoundedSearchShape,
									"bounded search requires one scalar load", current,
									node.id);
							load_operation = node.id;
							load_block = current;
						}
						if (node.opcode == Opcode::MemoryLoadValue &&
							node.operand_count == 1 &&
							node.operands[0] == load_operation)
						{
							load_value = node.id;
						}
					}
				}
				if (load_operation == INVALID_VALUE || load_value == INVALID_VALUE ||
					!HasExactMemoryExit(load_block, load_operation))
				{
					return Fail(Failure::UnsupportedBoundedSearchShape,
						"bounded search load lacks one exact restartable value",
						load_block, load_operation);
				}

				const Block& comparison_block = m_program.blocks[m_search_block];
				const Node* comparison = Definition(comparison_block.terminator.condition);
				if (!comparison || (comparison->opcode != Opcode::CompareEqual64 &&
					comparison->opcode != Opcode::CompareNotEqual64) ||
					comparison->operand_count != 2)
				{
					return Fail(Failure::UnsupportedBoundedSearchDataflow,
						"bounded search exit is not scalar equality", m_search_block,
						comparison_block.terminator.condition);
				}
				const bool left_load = ValueDependsOn(
					comparison->operands[0], load_value);
				const bool right_load = ValueDependsOn(
					comparison->operands[1], load_value);
				if (left_load == right_load)
				{
					return Fail(Failure::UnsupportedBoundedSearchDataflow,
						"bounded search equality does not compare its load once",
						m_search_block, comparison->id);
				}
				const ValueId key = comparison->operands[left_load ? 1 : 0];
				if (!IsLoopInvariant(key))
				{
					return Fail(Failure::UnsupportedBoundedSearchDataflow,
						"bounded search key is not loop invariant", m_search_block,
						key);
				}
				const bool match_is_taken =
					m_search_match == &comparison_block.terminator.taken;
				const bool true_is_equal =
					comparison->opcode == Opcode::CompareEqual64;
				if (match_is_taken != true_is_equal)
				{
					return Fail(Failure::UnsupportedBoundedSearchDataflow,
						"bounded search exits on inequality rather than equality",
						m_search_block, comparison->id);
				}

				const RegionMemoryPlan::CountedRange* counted = nullptr;
				const RegionMemoryPlan::Access* counted_access = nullptr;
				for (const RegionMemoryPlan::CountedRange& range :
					m_memory.counted_ranges)
				{
					for (const RegionMemoryPlan::Access& access : range.accesses)
					{
						if (access.operation != load_operation)
							continue;
						if (counted)
							return Fail(Failure::UnsupportedBoundedSearchShape,
								"bounded search load belongs to multiple ranges",
								access.block, access.operation);
						counted = &range;
						counted_access = &access;
					}
				}
				const Node* load = Definition(load_operation);
				if (!load || load->opcode != Opcode::MemoryLoad ||
					load->operand_count != 3)
					return Fail(Failure::UnsupportedBoundedSearchDataflow,
						"bounded search load definition is absent", load_block,
						load_operation);
				const MemoryAccessKind load_kind =
					static_cast<MemoryAccessKind>(load->immediate);
				if (load_kind != MemoryAccessKind::LoadS8 &&
					load_kind != MemoryAccessKind::LoadU8 &&
					load_kind != MemoryAccessKind::LoadS16 &&
					load_kind != MemoryAccessKind::LoadU16 &&
					load_kind != MemoryAccessKind::LoadS32 &&
					load_kind != MemoryAccessKind::LoadU32 &&
					load_kind != MemoryAccessKind::Load64)
				{
					return Fail(Failure::UnsupportedBoundedSearchShape,
						"bounded search load is not a scalar GPR load", load_block,
						load_operation);
				}
				BoundedEqualSearch search{};
				search.load_operation = load_operation;
				search.load_value = load_value;
				search.key_value = key;
				search.comparison = comparison->id;
				search.load_kind = load_kind;
				search.comparison_block = m_search_block;
				search.match_target_block = m_search_match->target_block;
				search.continue_target_block = m_search_continue->target_block;
				if (counted && counted_access && counted->valid &&
					!counted_access->store && counted->stride != 0 &&
					counted->accesses.size() == 1)
				{
					search.entry_induction_gpr = counted->entry_induction_gpr;
					search.entry_induction_offset = counted->entry_induction_offset;
					search.induction_gpr = counted->induction_gpr;
					search.stride = counted->stride;
					search.alignment = counted->alignment;
					search.load_offset = counted_access->induction_offset;
				}
				else
				{
					u8 induction_gpr = 0;
					s32 load_offset = 0;
					if (!MatchHeaderAffineAddress(load->operands[1],
							&induction_gpr, &load_offset))
					{
						return Fail(Failure::UnsupportedBoundedSearchShape,
							"bounded search has no loop-header affine read range",
							load_block, load_operation);
					}
					const ValueId parameter =
						m_program.blocks[m_header].parameters.gpr[induction_gpr];
					const ValueId incoming = RegionExecution::StateValue(
						m_backedge->state, induction_gpr);
					s32 delta = 0;
					RegionExecution::Low32Extension extension{};
					bool overflow_guard = false;
					if (!MatchLow32AddRecurrence(incoming, parameter, &delta,
							&extension, &overflow_guard) || delta <= 0 ||
						overflow_guard)
					{
						return Fail(Failure::UnsupportedBoundedSearchState,
							"bounded search address is not a positive affine recurrence",
							m_latch, incoming);
					}
					search.entry_induction_gpr = induction_gpr;
					search.induction_gpr = induction_gpr;
					search.stride = static_cast<u32>(delta);
					search.alignment = MemoryAlignmentMask(search.load_kind) + 1;
					search.load_offset = load_offset;
				}
				search.match_scaled_cycles = match_cycles;
				search.condition_true_is_match = match_is_taken;
				search.match_edge_checks_event =
					m_search_match->event_horizon_check;
				m_plan.kind = Kind::BoundedEqualSearch;
				m_plan.bounded_equal_searches.push_back(search);
				m_plan.control = m_memory.control;
				m_plan.repeated_scaled_cycles = repeated_cycles;
				m_plan.memory_operations_per_iteration = 1;
				m_plan.bytes_per_iteration = MemoryAccessWidth(search.load_kind);
				m_plan.read_bytes_per_iteration = m_plan.bytes_per_iteration;
				m_plan.complete_preflight = true;
				m_plan.exact_event_phase = true;
				return {};
			}

			bool IsLoopInvariant(ValueId value)
			{
				if (value >= m_definitions.size() || !m_definitions[value])
					return false;
				u8& state = m_invariant_state[value];
				if (state == 2)
					return true;
				if (state == 1 || state == 3)
					return false;
				state = 1;
				const Node& node = *m_definitions[value];
				const u32 block = m_defining_blocks[value];
				bool invariant = !InLoop(block);
				if (!invariant && node.opcode == Opcode::Parameter)
				{
					size_t slot = RegionExecution::STATE_SLOT_COUNT;
					for (size_t candidate = 0;
						candidate < RegionExecution::STATE_SLOT_COUNT; candidate++)
					{
						if (RegionExecution::StateValue(
								m_program.blocks[block].parameters, candidate) == value)
						{
							slot = candidate;
							break;
						}
					}
					if (slot < RegionExecution::STATE_SLOT_COUNT)
					{
						invariant = true;
						for (const Edge& edge : m_edges)
						{
							if (edge.target != block || !InLoop(edge.source) ||
								!IsFeasibleEdge(edge) ||
								(m_search_loop_mask != 0 &&
									!InSearchLoop(edge.source)))
								continue;
							const ValueId incoming =
								RegionExecution::StateValue(edge.transfer->state, slot);
							if (ResolveLoopParameter(incoming) != value &&
								!IsLoopInvariant(incoming))
							{
								invariant = false;
								break;
							}
						}
					}
				}
				else if (!invariant && IsPureInvariantOpcode(node.opcode))
				{
					invariant = true;
					for (u8 operand = 0; operand < node.operand_count; operand++)
					{
						if (!IsLoopInvariant(node.operands[operand]))
						{
							invariant = false;
							break;
						}
					}
				}
				state = invariant ? 2 : 3;
				return invariant;
			}

			BuildResult BuildPatternFill()
			{
				std::vector<u8> covered(m_program.value_count, 0);
				for (const RegionMemoryPlan::CountedRange& range :
					m_memory.counted_ranges)
				{
					if (!range.valid || range.header_block != m_header ||
						range.stride == 0)
						continue;
					PatternStream stream{};
					stream.entry_induction_gpr = range.entry_induction_gpr;
					stream.entry_induction_offset = range.entry_induction_offset;
					stream.induction_gpr = range.induction_gpr;
					stream.stride = range.stride;
					stream.alignment = range.alignment;
					stream.minimum_offset = range.minimum_offset;
					for (const RegionMemoryPlan::Access& access : range.accesses)
					{
						if (!InLoop(access.block))
							continue;
						if (access.operation >= m_definitions.size() ||
							!m_definitions[access.operation] ||
							covered[access.operation] != 0)
						{
							return Fail(Failure::IncompleteMemoryCoverage,
								"memory operation is missing or covered twice", access.block,
								access.operation);
						}
						const Node& node = *m_definitions[access.operation];
						if (!access.store || node.opcode != Opcode::MemoryStore ||
							node.operand_count != 3 || access.width == 0 ||
							access.width > 16)
						{
							return Fail(Failure::UnsupportedMemoryEffect,
								"first kernel class accepts only direct pattern stores",
								access.block, access.operation);
						}
						if (!HasExactMemoryExit(access.block, access.operation))
						{
							return Fail(Failure::InvalidMemoryExitContract,
								"pattern store lacks its mechanical restartable exit",
								access.block, access.operation);
						}
						if (!IsLoopInvariant(node.operands[2]))
						{
							return Fail(Failure::VariantPattern,
								"store payload changes within the repeated operation",
								access.block, node.operands[2]);
						}
						covered[access.operation] = 1;
						stream.fragments.push_back({access.operation, node.operands[2],
							static_cast<MemoryAccessKind>(node.immediate), node.source_pc,
							access.induction_offset, static_cast<u8>(access.width)});
					}
					if (stream.fragments.empty())
						continue;
					std::sort(stream.fragments.begin(), stream.fragments.end(),
						[](const PatternFragment& left, const PatternFragment& right) {
							return left.offset < right.offset;
						});
					s64 cursor = stream.fragments.front().offset;
					if (cursor != stream.minimum_offset)
						return Fail(Failure::NonContiguousStream,
							"stream minimum does not match its first store");
					for (const PatternFragment& fragment : stream.fragments)
					{
						if (fragment.offset != cursor)
							return Fail(Failure::NonContiguousStream,
								"pattern stream contains a gap or overlap", INVALID_BLOCK,
								fragment.operation);
						cursor += fragment.width;
					}
					if (cursor - stream.minimum_offset != stream.stride)
						return Fail(Failure::NonContiguousStream,
							"one pattern tile does not cover exactly one stream stride");
					if (m_plan.bytes_per_iteration > UINT32_MAX - stream.stride)
						return Fail(Failure::UnsupportedMemoryEffect,
							"pattern byte count overflows");
					m_plan.bytes_per_iteration += stream.stride;
					m_plan.write_bytes_per_iteration += stream.stride;
					m_plan.memory_operations_per_iteration +=
						static_cast<u32>(stream.fragments.size());
					m_plan.pattern_streams.push_back(std::move(stream));
				}

				if (m_plan.pattern_streams.empty())
					return Fail(Failure::IncompleteMemoryCoverage,
						"loop has no complete affine pattern stream");
				for (u32 block : m_plan.iteration_blocks)
				{
					for (const Node& node : m_program.blocks[block].nodes)
					{
						if (node.opcode == Opcode::MemoryLoad ||
							node.opcode == Opcode::MemoryLoadValue)
						{
							return Fail(Failure::UnsupportedMemoryEffect,
								"load/copy kernels use a separate alias contract", block,
								node.id);
						}
						if (node.opcode == Opcode::MemoryStore &&
							(node.id >= covered.size() || covered[node.id] == 0))
						{
							return Fail(Failure::IncompleteMemoryCoverage,
								"a repeated store lacks complete entry preflight", block,
								node.id);
						}
					}
				}
				m_plan.kind = Kind::PatternFill;
				m_plan.complete_preflight = true;
				m_plan.requires_disjoint_write_streams =
					m_plan.pattern_streams.size() > 1;
				return {};
			}

			BuildResult BuildForwardCopy()
			{
				struct RangeView
				{
					const RegionMemoryPlan::CountedRange* range = nullptr;
					bool loads = false;
					bool stores = false;
				};
				std::vector<RangeView> ranges;
				std::map<ValueId, const RegionMemoryPlan::Access*> accesses;
				for (const RegionMemoryPlan::CountedRange& range : m_memory.counted_ranges)
				{
					if (!range.valid || range.header_block != m_header || range.stride == 0)
						continue;
					RangeView view{&range};
					for (const RegionMemoryPlan::Access& access : range.accesses)
					{
						if (!InLoop(access.block))
							continue;
						if (!accesses.emplace(access.operation, &access).second)
							return Fail(Failure::IncompleteMemoryCoverage,
								"copy memory operation belongs to multiple ranges",
								access.block, access.operation);
						view.loads |= !access.store;
						view.stores |= access.store;
					}
					if (view.loads || view.stores)
						ranges.push_back(view);
				}

				const RangeView* source = nullptr;
				const RangeView* destination = nullptr;
				for (const RangeView& range : ranges)
				{
					if (range.loads && !range.stores && !source)
						source = &range;
					else if (range.stores && !range.loads && !destination)
						destination = &range;
					else
						return Fail(Failure::UnsupportedCopyShape,
							"copy requires one load-only and one store-only affine stream");
				}
				if (!source || !destination || source->range->stride == 0 ||
					source->range->stride != destination->range->stride)
				{
					return Fail(Failure::UnsupportedCopyShape,
						"copy source and destination tiles have different extents");
				}

				CopyStream stream{};
				stream.source_entry_induction_gpr = source->range->entry_induction_gpr;
				stream.source_entry_induction_offset = source->range->entry_induction_offset;
				stream.source_induction_gpr = source->range->induction_gpr;
				stream.destination_entry_induction_gpr =
					destination->range->entry_induction_gpr;
				stream.destination_entry_induction_offset =
					destination->range->entry_induction_offset;
				stream.destination_induction_gpr = destination->range->induction_gpr;
				stream.stride = source->range->stride;
				stream.source_alignment = source->range->alignment;
				stream.destination_alignment = destination->range->alignment;
				stream.source_minimum_offset = source->range->minimum_offset;
				stream.destination_minimum_offset = destination->range->minimum_offset;

				std::map<ValueId, const RegionMemoryPlan::Access*> load_values;
				for (const auto& [operation, access] : accesses)
				{
					if (access->store)
						continue;
					if (operation >= m_definitions.size() || !m_definitions[operation] ||
						m_definitions[operation]->opcode != Opcode::MemoryLoad ||
						!HasExactMemoryExit(access->block, operation))
					{
						return Fail(Failure::InvalidMemoryExitContract,
							"copy load lacks its mechanical restartable exit",
							access->block, operation);
					}
					for (const Node& node : m_program.blocks[access->block].nodes)
					{
						if (node.opcode == Opcode::MemoryLoadValue && node.operand_count == 1 &&
							node.operands[0] == operation)
						{
							load_values.emplace(node.id, access);
							u8 target_gpr = 0;
							for (const Node& bind : m_program.blocks[access->block].nodes)
							{
								if (bind.opcode == Opcode::BindGpr && bind.operand_count == 1 &&
									bind.operands[0] == node.id && bind.immediate > 0 &&
									bind.immediate < 32)
								{
									if (target_gpr != 0)
										return Fail(Failure::UnsupportedCopyDataflow,
											"one copy load value binds multiple GPRs",
											access->block, node.id);
									target_gpr = static_cast<u8>(bind.immediate);
								}
							}
							if (target_gpr == 0)
								return Fail(Failure::UnsupportedCopyDataflow,
									"copy load value has no exact architectural destination",
									access->block, node.id);
							m_plan.final_load_state.push_back({target_gpr, operation,
								node.id, static_cast<MemoryAccessKind>(
									m_definitions[operation]->immediate)});
						}
					}
				}

				for (const auto& [operation, access] : accesses)
				{
					if (!access->store)
						continue;
					if (operation >= m_definitions.size() || !m_definitions[operation])
						return Fail(Failure::IncompleteMemoryCoverage,
							"copy store definition is absent", access->block, operation);
					const Node& store = *m_definitions[operation];
					if (store.opcode != Opcode::MemoryStore || store.operand_count != 3 ||
						!HasExactMemoryExit(access->block, operation))
					{
						return Fail(Failure::InvalidMemoryExitContract,
							"copy store lacks its mechanical restartable exit",
							access->block, operation);
					}
					const auto load = load_values.find(ResolveLoopParameter(store.operands[2]));
					if (load == load_values.end() || load->second->width != access->width ||
						access->width == 0 || access->width > 16)
					{
						return Fail(Failure::UnsupportedCopyDataflow,
							"store payload is not one same-width load from this iteration",
							access->block, store.operands[2]);
					}
					const Node& load_node = *m_definitions[load->second->operation];
					stream.fragments.push_back({load->second->operation, operation,
						static_cast<MemoryAccessKind>(load_node.immediate),
						static_cast<MemoryAccessKind>(store.immediate),
						load_node.source_pc, store.source_pc,
						load->second->induction_offset, access->induction_offset,
						static_cast<u8>(access->width)});
				}
				std::sort(stream.fragments.begin(), stream.fragments.end(),
					[](const CopyFragment& left, const CopyFragment& right) {
						return left.destination_offset < right.destination_offset;
					});
				s64 source_cursor = stream.source_minimum_offset;
				s64 destination_cursor = stream.destination_minimum_offset;
				u32 preceding_store_order = 0;
				bool first_fragment = true;
				for (const CopyFragment& fragment : stream.fragments)
				{
					if (fragment.source_offset != source_cursor ||
						fragment.destination_offset != destination_cursor)
					{
						return Fail(Failure::UnsupportedCopyShape,
							"copy tile contains a gap, overlap, or reordered fragment");
					}
					const u32 load_order = MemoryOperationOrder(fragment.load_operation);
					const u32 store_order = MemoryOperationOrder(fragment.store_operation);
					if (load_order == UINT32_MAX || store_order == UINT32_MAX ||
						load_order >= store_order ||
						(!first_fragment && preceding_store_order >= load_order))
					{
						return Fail(Failure::UnsupportedCopyDataflow,
							"copy tile cannot preserve exact load/store operation order");
					}
					first_fragment = false;
					preceding_store_order = store_order;
					source_cursor += fragment.width;
					destination_cursor += fragment.width;
				}
				if (stream.fragments.empty() ||
					source_cursor - stream.source_minimum_offset != stream.stride ||
					destination_cursor - stream.destination_minimum_offset != stream.stride)
				{
					return Fail(Failure::UnsupportedCopyShape,
						"copy tile does not cover both stream strides exactly");
				}

				for (u32 block : m_plan.iteration_blocks)
				{
					for (const Node& node : m_program.blocks[block].nodes)
					{
						if ((node.opcode == Opcode::MemoryLoad ||
							 node.opcode == Opcode::MemoryStore) &&
							accesses.find(node.id) == accesses.end())
						{
							return Fail(Failure::IncompleteMemoryCoverage,
								"copy operation lacks complete entry preflight", block, node.id);
						}
					}
				}
				m_plan.kind = Kind::ForwardCopy;
				m_plan.copy_streams.push_back(std::move(stream));
				m_plan.bytes_per_iteration = source->range->stride;
				m_plan.read_bytes_per_iteration = source->range->stride;
				m_plan.write_bytes_per_iteration = destination->range->stride;
				m_plan.memory_operations_per_iteration = static_cast<u32>(
					source->range->accesses.size() + destination->range->accesses.size());
				m_plan.complete_preflight = true;
				m_plan.requires_forward_copy_batch_alias_guard = true;
				return {};
			}

			struct Vu0FmacStage
			{
				ValueId result = INVALID_VALUE;
				ValueId raw = INVALID_VALUE;
				ValueId matrix = INVALID_VALUE;
				ValueId normalized_matrix = INVALID_VALUE;
				ValueId mac = INVALID_VALUE;
				ValueId status = INVALID_VALUE;
				ValueId vi_status = INVALID_VALUE;
			};

			const Node* Definition(ValueId value) const
			{
				value = ResolveLoopParameter(value);
				return value < m_definitions.size() ? m_definitions[value] : nullptr;
			}

			bool HeaderVu0VfIndex(ValueId value, u8* index) const
			{
				value = ResolveLoopParameter(value);
				const StateMap& state = m_program.blocks[m_header].parameters;
				for (u32 candidate = 0; candidate < state.vu0_vf.size(); candidate++)
				{
					if (state.vu0_vf[candidate] == value)
					{
						if (index)
							*index = static_cast<u8>(candidate);
						return true;
					}
				}
				return false;
			}

			bool HeaderVu0ViIndex(ValueId value, u8* index) const
			{
				value = ResolveLoopParameter(value);
				const StateMap& state = m_program.blocks[m_header].parameters;
				for (u32 candidate = 0; candidate < state.vu0_vi.size(); candidate++)
				{
					if (state.vu0_vi[candidate] == value)
					{
						if (index)
							*index = static_cast<u8>(candidate);
						return true;
					}
				}
				return false;
			}

			const Node* FindUniqueConsumer(Opcode opcode, ValueId operand,
				u8 operand_index, u32 immediate = UINT32_MAX) const
			{
				const Node* found = nullptr;
				for (u32 block : m_plan.iteration_blocks)
				{
					for (const Node& node : m_program.blocks[block].nodes)
					{
						if (node.opcode != opcode || node.operand_count <= operand_index ||
							ResolveLoopParameter(node.operands[operand_index]) !=
								ResolveLoopParameter(operand) ||
							(immediate != UINT32_MAX && node.immediate != immediate))
						{
							continue;
						}
						if (found)
							return nullptr;
						found = &node;
					}
				}
				return found;
			}

			bool FindUniqueVu0VfBind(ValueId value, u8* target) const
			{
				const Node* found = nullptr;
				for (u32 block : m_plan.iteration_blocks)
				{
					for (const Node& node : m_program.blocks[block].nodes)
					{
						if (node.opcode != Opcode::BindVu0Vf || node.operand_count != 1 ||
							ResolveLoopParameter(node.operands[0]) !=
								ResolveLoopParameter(value) || node.immediate >= 32)
						{
							continue;
						}
						if (found)
							return false;
						found = &node;
					}
				}
				if (!found)
					return false;
				*target = static_cast<u8>(found->immediate);
				return true;
			}

			bool MatchIdleValue(ValueId value, ValueId idle_control,
				ValueId* forwarded) const
			{
				const Node* node = Definition(value);
				if (!node || node->opcode != Opcode::Vu0RequireIdle ||
					node->operand_count != 2 ||
					ResolveLoopParameter(node->operands[0]) !=
						ResolveLoopParameter(idle_control))
				{
					return false;
				}
				*forwarded = ResolveLoopParameter(node->operands[1]);
				return true;
			}

			bool MatchVu0FmacStage(ValueId result, ValueId previous_acc,
				ValueId input, u32 lane, ValueId previous_vi_status,
				ValueId idle_control, Vu0FmacStage* stage)
			{
				if (!stage || lane >= 4)
					return false;
				result = ResolveLoopParameter(result);
				const Node* merge = Definition(result);
				if (!merge || merge->opcode != Opcode::Vu0MergeMasked ||
					merge->operand_count != 2 || merge->immediate != 0x0fu)
				{
					return false;
				}
				const Node* clamp = Definition(merge->operands[1]);
				if (!clamp || clamp->opcode != Opcode::Vu0ClampFmacResult ||
					clamp->operand_count != 1 || clamp->immediate != 0x0fu)
				{
					return false;
				}
				const ValueId raw_value = ResolveLoopParameter(clamp->operands[0]);
				const Node* raw = Definition(raw_value);
				if (!raw || raw->operand_count != 2)
					return false;

				ValueId multiply_value = raw_value;
				if (lane == 0)
				{
					if (raw->opcode != Opcode::Vu0MulRaw || previous_acc != INVALID_VALUE)
						return false;
				}
				else
				{
					if (raw->opcode != Opcode::Vu0AddRaw || previous_acc == INVALID_VALUE)
						return false;
					const Node* normalized_acc = Definition(raw->operands[0]);
					if (!normalized_acc ||
						normalized_acc->opcode != Opcode::Vu0NormalizeVector ||
						normalized_acc->operand_count != 1 ||
						ResolveLoopParameter(normalized_acc->operands[0]) !=
							ResolveLoopParameter(previous_acc))
					{
						return false;
					}
					multiply_value = ResolveLoopParameter(raw->operands[1]);
					raw = Definition(multiply_value);
					if (!raw || raw->opcode != Opcode::Vu0MulRaw ||
						raw->operand_count != 2)
					{
						return false;
					}
				}

				const Node* normalized_matrix = Definition(raw->operands[0]);
				const Node* broadcast = Definition(raw->operands[1]);
				if (!normalized_matrix ||
					normalized_matrix->opcode != Opcode::Vu0NormalizeVector ||
					normalized_matrix->operand_count != 1 || !broadcast ||
					broadcast->opcode != Opcode::Vu0BroadcastLane ||
					broadcast->operand_count != 1 || broadcast->immediate != lane)
				{
					return false;
				}
				const Node* normalized_input = Definition(broadcast->operands[0]);
				if (!normalized_input ||
					normalized_input->opcode != Opcode::Vu0NormalizeVector ||
					normalized_input->operand_count != 1 ||
					ResolveLoopParameter(normalized_input->operands[0]) !=
						ResolveLoopParameter(input))
				{
					return false;
				}
				ValueId matrix = INVALID_VALUE;
				if (!MatchIdleValue(normalized_matrix->operands[0], idle_control,
						&matrix) || !IsLoopInvariant(matrix))
				{
					return false;
				}

				const Node* mac = FindUniqueConsumer(Opcode::Vu0MacFlagsFromRaw,
					raw_value, 0, 0x0fu);
				const Node* status = mac ? FindUniqueConsumer(
					Opcode::Vu0StatusFlagsFromMac, mac->id, 0) : nullptr;
				const Node* vi_status = status ? FindUniqueConsumer(
					Opcode::Vu0SyncStatusControl, status->id, 1) : nullptr;
				if (!mac || !status || !vi_status || vi_status->operand_count != 2 ||
					ResolveLoopParameter(vi_status->operands[0]) !=
						ResolveLoopParameter(previous_vi_status))
				{
					return false;
				}

				stage->result = result;
				stage->raw = raw_value;
				stage->matrix = matrix;
				stage->normalized_matrix = normalized_matrix->id;
				stage->mac = mac->id;
				stage->status = status->id;
				stage->vi_status = vi_status->id;
				return true;
			}

			BuildResult BuildVu0AffineTransform()
			{
				const RegionMemoryPlan::CountedRange* source_range = nullptr;
				const RegionMemoryPlan::CountedRange* destination_range = nullptr;
				const RegionMemoryPlan::Access* load_access = nullptr;
				const RegionMemoryPlan::Access* store_access = nullptr;
				for (const RegionMemoryPlan::CountedRange& range : m_memory.counted_ranges)
				{
					if (!range.valid || range.header_block != m_header)
						continue;
					for (const RegionMemoryPlan::Access& access : range.accesses)
					{
						if (!InLoop(access.block))
							continue;
						const Node* operation = Definition(access.operation);
						if (!operation || operation->opcode !=
								(access.store ? Opcode::MemoryStore : Opcode::MemoryLoad) ||
							access.width != 16)
						{
							return Fail(Failure::UnsupportedVu0AffineShape,
								"affine VU0 transform has a non-vector memory effect",
								access.block, access.operation);
						}
						const MemoryAccessKind kind =
							static_cast<MemoryAccessKind>(operation->immediate);
						if (!access.store && kind == MemoryAccessKind::LoadVu0Vector &&
							!load_access)
						{
							load_access = &access;
							source_range = &range;
						}
						else if (access.store &&
							kind == MemoryAccessKind::StoreVu0Vector && !store_access)
						{
							store_access = &access;
							destination_range = &range;
						}
						else
						{
							return Fail(Failure::UnsupportedVu0AffineShape,
								"affine VU0 transform requires one vector load and store",
								access.block, access.operation);
						}
					}
				}
				if (!source_range || !destination_range || !load_access ||
					!store_access || source_range->stride != 16 ||
					destination_range->stride != 16 ||
					source_range->invariant_base_gpr != 0 ||
					destination_range->invariant_base_gpr != 0 ||
					source_range->induction_scale_shift != 0 ||
					destination_range->induction_scale_shift != 0)
				{
					return Fail(Failure::UnsupportedVu0AffineShape,
						"VU0 transform streams are not direct affine 16-byte vectors");
				}
				if (!HasExactMemoryExit(load_access->block, load_access->operation) ||
					!HasExactMemoryExit(store_access->block, store_access->operation))
				{
					return Fail(Failure::InvalidMemoryExitContract,
						"VU0 transform memory lacks a mechanical restartable exit");
				}

				const Node* load = Definition(load_access->operation);
				const Node* store = Definition(store_access->operation);
				if (!load || load->operand_count != 3 || !store ||
					store->operand_count != 3)
				{
					return Fail(Failure::UnsupportedVu0AffineDataflow,
						"VU0 vector memory has no ordered value/effect graph");
				}
				const Node* load_value = FindUniqueConsumer(Opcode::MemoryLoadValue,
					load->id, 0);
				if (!load_value)
					return Fail(Failure::UnsupportedVu0AffineDataflow,
						"VU0 vector load has no unique result", load_access->block,
						load->id);

				Vu0AffineStream stream{};
				stream.source_entry_induction_gpr =
					source_range->entry_induction_gpr;
				stream.source_entry_induction_offset =
					source_range->entry_induction_offset;
				stream.source_induction_gpr = source_range->induction_gpr;
				stream.source_offset = load_access->induction_offset;
				stream.destination_entry_induction_gpr =
					destination_range->entry_induction_gpr;
				stream.destination_entry_induction_offset =
					destination_range->entry_induction_offset;
				stream.destination_induction_gpr = destination_range->induction_gpr;
				stream.destination_offset = store_access->induction_offset;
				stream.stride = 16;
				stream.source_alignment = source_range->alignment;
				stream.destination_alignment = destination_range->alignment;
				stream.load_pc = load->source_pc;
				stream.store_pc = store->source_pc;
				stream.load_operation = load->id;
				stream.load_value = load_value->id;
				stream.store_operation = store->id;
				stream.final_input = load_value->id;
				if (!FindUniqueVu0VfBind(load_value->id, &stream.input_vf))
				{
					return Fail(Failure::UnsupportedVu0AffineState,
						"VU0 input stream has no unique architectural VF destination",
						load_access->block, load_value->id);
				}

				const Node* load_idle = Definition(load->operands[2]);
				ValueId ignored_old_input = INVALID_VALUE;
				if (!load_idle || load_idle->opcode != Opcode::Vu0RequireIdle ||
					load_idle->operand_count != 2 ||
					!MatchIdleValue(load_idle->id, load_idle->operands[0],
						&ignored_old_input))
				{
					return Fail(Failure::UnsupportedVu0AffineDataflow,
						"VU0 vector load lacks its exact idle observer",
						load_access->block, load->id);
				}
				const ValueId idle_control = ResolveLoopParameter(load_idle->operands[0]);
				if (!HeaderVu0ViIndex(idle_control, &stream.idle_vi))
				{
					return Fail(Failure::UnsupportedVu0AffineState,
						"VU0 idle observer is not canonical VI state",
						load_access->block, idle_control);
				}
				u8 old_input_vf = 0;
				if (!HeaderVu0VfIndex(ignored_old_input, &old_input_vf) ||
					old_input_vf != stream.input_vf)
				{
					return Fail(Failure::UnsupportedVu0AffineState,
						"VU0 load idle observer names a different VF",
						load_access->block, load_idle->id);
				}

				ValueId final_output = INVALID_VALUE;
				if (!MatchIdleValue(store->operands[2], idle_control, &final_output) ||
					!FindUniqueVu0VfBind(final_output, &stream.output_vf))
				{
					return Fail(Failure::UnsupportedVu0AffineState,
						"VU0 output stream lacks an exact idle-guarded VF result",
						store_access->block, store->operands[2]);
				}

				std::array<Vu0FmacStage, 4> stages{};
				u8 status_vi = 0;
				ValueId previous_vi_status = INVALID_VALUE;
				// The first sync consumes the canonical STATUS VI parameter. Discover it
				// from the unique status consumer after matching the arithmetic below.
				const Node* final_merge = Definition(final_output);
				if (!final_merge || final_merge->opcode != Opcode::Vu0MergeMasked)
					return Fail(Failure::UnsupportedVu0AffineDataflow,
						"VU0 output is not a complete FMAC result", store_access->block,
						final_output);

				// Trace accumulator stages forward from their unique full-mask FMAC
				// results. The output stage is known from the store; the first three are
				// identified by the normalized-ACC dependency chain.
				std::vector<ValueId> merge_results;
				for (u32 block : m_plan.iteration_blocks)
				{
					for (const Node& node : m_program.blocks[block].nodes)
					{
						if (node.opcode == Opcode::BindVu0Acc && node.operand_count == 1)
							merge_results.push_back(ResolveLoopParameter(node.operands[0]));
					}
				}
				if (merge_results.size() != 3)
					return Fail(Failure::UnsupportedVu0AffineDataflow,
						"VU0 affine transform requires three accumulator stages");

				// Find the first stage's STATUS sync from its raw-result consumers, then
				// use that canonical VI parameter as the chain seed.
				const Node* first_merge = Definition(merge_results[0]);
				const Node* first_clamp = first_merge && first_merge->operand_count == 2 ?
					Definition(first_merge->operands[1]) : nullptr;
				const Node* first_mac = first_clamp && first_clamp->operand_count == 1 ?
					FindUniqueConsumer(Opcode::Vu0MacFlagsFromRaw,
						first_clamp->operands[0], 0, 0x0fu) : nullptr;
				const Node* first_status = first_mac ? FindUniqueConsumer(
					Opcode::Vu0StatusFlagsFromMac, first_mac->id, 0) : nullptr;
				const Node* first_sync = first_status ? FindUniqueConsumer(
					Opcode::Vu0SyncStatusControl, first_status->id, 1) : nullptr;
				if (!first_sync || first_sync->operand_count != 2 ||
					!HeaderVu0ViIndex(first_sync->operands[0], &status_vi))
				{
					return Fail(Failure::UnsupportedVu0AffineState,
						"VU0 affine STATUS chain has no canonical seed");
				}
				previous_vi_status = first_sync->operands[0];
				ValueId previous_acc = INVALID_VALUE;
				for (u32 lane = 0; lane < 4; lane++)
				{
					const ValueId result = lane < 3 ? merge_results[lane] : final_output;
					if (!MatchVu0FmacStage(result, previous_acc, load_value->id,
							lane, previous_vi_status, idle_control, &stages[lane]))
					{
						return Fail(Failure::UnsupportedVu0AffineDataflow,
							"VU0 affine FMAC chain is not four ordered full-mask lanes",
							m_defining_blocks[result], result);
					}
					if (!HeaderVu0VfIndex(stages[lane].matrix,
							&stream.matrix_vf[lane]))
					{
						return Fail(Failure::UnsupportedVu0AffineState,
							"VU0 affine matrix column is not a canonical invariant VF",
							m_defining_blocks[stages[lane].matrix], stages[lane].matrix);
					}
					stream.matrix_values[lane] = stages[lane].matrix;
					stream.normalized_matrix_values[lane] =
						stages[lane].normalized_matrix;
					previous_acc = stages[lane].result;
					previous_vi_status = stages[lane].vi_status;
				}
				if (std::any_of(stream.matrix_vf.begin(), stream.matrix_vf.end(),
						[&](u8 vf) {
							return vf == stream.input_vf || vf == stream.output_vf;
						}) || stream.idle_vi == status_vi)
				{
					return Fail(Failure::UnsupportedVu0AffineState,
						"VU0 affine invariant/state registers alias a loop-written owner");
				}

				stream.status_vi = status_vi;
				stream.final_output = stages[3].result;
				stream.final_acc = stages[2].result;
				stream.final_mac = stages[3].mac;
				stream.final_status = stages[3].status;
				stream.final_vi_status = stages[3].vi_status;
				m_plan.kind = Kind::Vu0AffineTransform;
				m_plan.vu0_affine_streams.push_back(stream);
				m_plan.bytes_per_iteration = 16;
				m_plan.read_bytes_per_iteration = 16;
				m_plan.write_bytes_per_iteration = 16;
				m_plan.memory_operations_per_iteration = 2;
				m_plan.complete_preflight = true;
				return {};
			}

			BuildResult BuildCop1Stream()
			{
				Cop1Stream stream{};
				if (m_header != m_program.entry_block ||
					!m_memory.header_seeds_match_entry)
				{
					return Fail(Failure::UnsupportedCop1StreamShape,
						"COP1 stream header state is not its canonical entry state",
						m_header);
				}
				std::set<ValueId> memory_operations;
				std::set<ValueId> covered_memory_operations;
				std::set<ValueId> supported_values;
				std::set<ValueId> active;
				std::set<u16> entry_state_slots;
				ValueId unsupported_value = INVALID_VALUE;
				u32 unsupported_block = INVALID_BLOCK;
				u32 matched_signed_overflow_guards = 0;

				auto memory_exit_exact = [&](ValueId operation) {
					if (operation >= m_defining_blocks.size())
						return false;
					return HasExactMemoryExit(m_defining_blocks[operation], operation);
				};
				for (const RegionMemoryPlan::CountedRange& range :
					m_memory.counted_ranges)
				{
					if (!range.valid || range.header_block != m_header)
						continue;
					stream.affine_ranges++;
					for (const RegionMemoryPlan::Access& access : range.accesses)
					{
						if (InLoop(access.block))
							covered_memory_operations.insert(access.operation);
					}
				}
				for (const RegionMemoryPlan::BoundedRange& range :
					m_memory.bounded_ranges)
				{
					if (!range.valid || range.header_block != m_header)
						continue;
					bool has_store = false;
					for (const RegionMemoryPlan::Access& access : range.accesses)
					{
						if (!InLoop(access.block))
							continue;
						has_store |= access.store;
						covered_memory_operations.insert(access.operation);
					}
					// A bounded, data-indexed write stream cannot establish per-iteration
					// disjointness from range bounds alone. Keep it on tier zero.
					if (has_store)
					{
						return Fail(Failure::UnsupportedCop1StreamShape,
							"COP1 stream has a data-indexed output range");
					}
					stream.bounded_read_ranges++;
				}

				for (u32 block : m_plan.iteration_blocks)
				{
					for (const Node& node : m_program.blocks[block].nodes)
					{
						if (node.opcode != Opcode::MemoryLoad &&
							node.opcode != Opcode::MemoryStore)
						{
							continue;
						}
						memory_operations.insert(node.id);
						if (covered_memory_operations.find(node.id) ==
								covered_memory_operations.end() ||
							!memory_exit_exact(node.id))
						{
							return Fail(Failure::IncompleteCop1StreamMemory,
								"COP1 stream memory lacks one complete preflight range",
								block, node.id);
						}
						if (node.opcode == Opcode::MemoryLoad)
							stream.load_operations++;
						else
							stream.store_operations++;
					}
				}
				if (stream.store_operations == 0 || stream.load_operations == 0 ||
					stream.affine_ranges == 0)
				{
					return Fail(Failure::UnsupportedCop1StreamShape,
						"COP1 stream requires affine output and represented inputs");
				}

				// Prove that every loop-produced architectural value is a serial DAG
				// rooted in parameters/constants or represented direct loads. This is
				// intentionally stricter than general Region IR support: comparisons,
				// dynamic branches and helper observers are separate future classes.
				auto value_supported = [&](auto&& self, ValueId value) -> bool {
					value = ResolveLoopParameter(value);
					if (value < m_definitions.size() && m_definitions[value] &&
						m_definitions[value]->opcode == Opcode::Parameter &&
						m_defining_blocks[value] == m_header)
					{
						const Block& header = m_program.blocks[m_header];
						for (size_t slot = 0;
							slot < RegionExecution::STATE_SLOT_COUNT; slot++)
						{
							if (RegionExecution::StateValue(header.parameters, slot) == value)
							{
								entry_state_slots.insert(static_cast<u16>(slot));
								return true;
							}
						}
					}
					if (IsLoopInvariant(value) || supported_values.contains(value))
						return true;
					if (value >= m_definitions.size() || !m_definitions[value] ||
						!active.insert(value).second)
					{
						if (unsupported_value == INVALID_VALUE)
						{
							unsupported_value = value;
							if (value < m_defining_blocks.size())
								unsupported_block = m_defining_blocks[value];
						}
						return false;
					}
					const Node& node = *m_definitions[value];
					bool supported = false;
					switch (node.opcode)
					{
						case Opcode::MemoryLoadValue:
							supported = node.operand_count == 1 &&
								memory_operations.contains(node.operands[0]) &&
								m_definitions[node.operands[0]] &&
								m_definitions[node.operands[0]]->opcode == Opcode::MemoryLoad;
							break;
						case Opcode::Cop1NormalizeInput:
						case Opcode::Cop1ClampOuResult:
						case Opcode::Cop1ExceptionalOuResult:
						case Opcode::Cop1ConvertWord:
						case Opcode::BitcastF32BitsToI32:
						case Opcode::BitcastI32ToF32Bits:
						case Opcode::ExtractLow32:
						case Opcode::ExtractLow64:
						case Opcode::SignExtend32To64:
						case Opcode::ZeroExtend32To64:
						case Opcode::Truncate64To32:
						case Opcode::ShiftRightArithmetic32:
						case Opcode::ShiftRightLogical32:
						case Opcode::ShiftLeft32:
							supported = node.operand_count == 1 &&
								self(self, node.operands[0]);
							break;
						case Opcode::Cop1AddRaw:
						case Opcode::Cop1SubRaw:
						case Opcode::Cop1MulRaw:
						case Opcode::Cop1UpdateOuFlags:
						case Opcode::ReplaceLow64:
						case Opcode::Add32:
						case Opcode::Add64:
						case Opcode::Sub32:
						case Opcode::Sub64:
						case Opcode::And32:
						case Opcode::And64:
						case Opcode::Or64:
						case Opcode::Xor32:
						case Opcode::Xor64:
							supported = node.operand_count == 2 &&
								self(self, node.operands[0]) &&
								self(self, node.operands[1]);
							break;
						default:
							break;
					}
					active.erase(value);
					if (supported)
						supported_values.insert(value);
					else if (unsupported_value == INVALID_VALUE)
					{
						unsupported_value = value;
						unsupported_block = m_defining_blocks[value];
					}
					return supported;
				};

				for (u32 block : m_plan.iteration_blocks)
				{
					for (const Node& node : m_program.blocks[block].nodes)
					{
						switch (node.opcode)
						{
							case Opcode::Cop1AddRaw:
							case Opcode::Cop1SubRaw:
							case Opcode::Cop1MulRaw:
								stream.cop1_arithmetic_nodes++;
								stream.normal_ou_guard_required = true;
								if (!value_supported(value_supported, node.id))
									return Fail(Failure::UnsupportedCop1StreamDataflow,
										"COP1 arithmetic is not in the supported serial DAG",
										unsupported_block, unsupported_value);
								break;
							case Opcode::Cop1ConvertWord:
								stream.cop1_conversion_nodes++;
								if (!value_supported(value_supported, node.id))
									return Fail(Failure::UnsupportedCop1StreamDataflow,
										"COP1 conversion is not in the supported serial DAG",
										unsupported_block, unsupported_value);
								break;
							case Opcode::MemoryStore:
								if (node.operand_count != 3 ||
									!value_supported(value_supported, node.operands[2]))
								{
									return Fail(Failure::UnsupportedCop1StreamDataflow,
										"COP1 stream output is not in the supported serial DAG",
										unsupported_block, unsupported_value);
								}
								break;
							default:
								break;
						}
					}
				}
				if (stream.cop1_arithmetic_nodes == 0)
					return Fail(Failure::UnsupportedCop1StreamShape,
						"COP1 stream has no arithmetic work");

				// The complete backedge state may change only through affine low-word
				// recurrences or values belonging to the proven DAG. Store every latter
				// binding explicitly so the reference verifier can publish the exact
				// final-iteration architectural state.
				const Block& header = m_program.blocks[m_header];
				if (!m_backedge || !m_completion ||
					m_backedge->state.memory_effect !=
						m_completion->state.memory_effect)
				{
					return Fail(Failure::UnsupportedCop1StreamState,
						"COP1 stream completion has a distinct memory effect", m_latch);
				}
				for (size_t slot = 1; slot < RegionExecution::STATE_SLOT_COUNT - 1; slot++)
				{
					const ValueId parameter =
						RegionExecution::StateValue(header.parameters, slot);
					const ValueId backedge_incoming = ResolveLoopParameter(
						RegionExecution::StateValue(m_backedge->state, slot));
					const ValueId completion_incoming = ResolveLoopParameter(
						RegionExecution::StateValue(m_completion->state, slot));
					if (backedge_incoming != completion_incoming)
					{
						return Fail(Failure::UnsupportedCop1StreamState,
							"COP1 stream backedge and completion publish distinct state",
							m_latch, completion_incoming);
					}
					const ValueId incoming = completion_incoming;
					if (incoming == parameter)
						continue;
					const RegionExecution::StateSlot decoded =
						RegionExecution::DecodeStateSlot(slot);
					if (decoded.state_class == RegionExecution::StateClass::Gpr)
					{
						s32 delta = 0;
						RegionExecution::Low32Extension extension{};
						bool signed_guard = false;
						if (MatchLow32AddRecurrence(incoming, parameter, &delta,
								&extension, &signed_guard))
						{
							m_plan.recurrences.push_back({decoded.index, delta,
								extension, signed_guard});
							matched_signed_overflow_guards += signed_guard ? 1u : 0u;
							continue;
						}
					}
					if (!value_supported(value_supported, incoming))
					{
						return Fail(Failure::UnsupportedCop1StreamState,
							"COP1 stream changes state outside its supported serial DAG",
							m_latch, incoming);
					}
					stream.final_state.push_back(
						{static_cast<u16>(slot), incoming});
				}
				if (matched_signed_overflow_guards != m_signed_overflow_exits)
				{
					return Fail(Failure::UnsupportedCop1StreamState,
						"COP1 stream does not own every signed recurrence exit",
						m_latch);
				}

				stream.memory_operations.assign(memory_operations.begin(),
					memory_operations.end());
				stream.entry_state_slots.assign(entry_state_slots.begin(),
					entry_state_slots.end());
				stream.requires_disjoint_read_write_ranges = true;
				m_plan.kind = Kind::Cop1Stream;
				m_plan.cop1_streams.push_back(std::move(stream));
				m_plan.memory_operations_per_iteration =
					static_cast<u32>(memory_operations.size());
				for (ValueId operation : memory_operations)
				{
					const Node& node = *m_definitions[operation];
					const u32 width = MemoryAccessWidth(
						static_cast<MemoryAccessKind>(node.immediate));
					if (node.opcode == Opcode::MemoryLoad)
						m_plan.read_bytes_per_iteration += width;
					else
						m_plan.write_bytes_per_iteration += width;
				}
				m_plan.bytes_per_iteration = m_plan.read_bytes_per_iteration +
					m_plan.write_bytes_per_iteration;
				m_plan.complete_preflight = true;
				return {};
			}

			ValueId ResolveLoopParameter(ValueId value) const
			{
				std::set<ValueId> visited;
				while (value < m_definitions.size() && m_definitions[value] &&
					m_definitions[value]->opcode == Opcode::Parameter &&
					m_defining_blocks[value] != m_header && visited.insert(value).second)
				{
					const u32 block = m_defining_blocks[value];
					size_t slot = RegionExecution::STATE_SLOT_COUNT;
					for (size_t candidate = 0;
						candidate < RegionExecution::STATE_SLOT_COUNT; candidate++)
					{
						if (RegionExecution::StateValue(
								m_program.blocks[block].parameters, candidate) == value)
						{
							slot = candidate;
							break;
						}
					}
					if (slot == RegionExecution::STATE_SLOT_COUNT)
						break;
					const Edge* incoming = nullptr;
					for (const Edge& edge : m_edges)
					{
						if (edge.target != block || !InLoop(edge.source) ||
							!IsFeasibleEdge(edge) ||
							(m_search_loop_mask != 0 &&
								!InSearchLoop(edge.source)))
							continue;
						if (incoming)
							return value;
						incoming = &edge;
					}
					if (!incoming)
						break;
					value = RegionExecution::StateValue(incoming->transfer->state, slot);
				}
				return value;
			}

			bool MatchLow32AddRecurrence(ValueId value, ValueId parameter,
				s32* delta, RegionExecution::Low32Extension* extension,
				bool* signed_overflow_guard) const
			{
				value = ResolveLoopParameter(value);
				if (value >= m_definitions.size() || !m_definitions[value])
					return false;
				const Node& replace = *m_definitions[value];
				if (replace.opcode != Opcode::ReplaceLow64 ||
					replace.operand_count != 2 ||
					ResolveLoopParameter(replace.operands[0]) != parameter)
				{
					return false;
				}
				ValueId low = ResolveLoopParameter(replace.operands[1]);
				if (low >= m_definitions.size() || !m_definitions[low])
					return false;
				const Node& extend = *m_definitions[low];
				if ((extend.opcode != Opcode::SignExtend32To64 &&
					 extend.opcode != Opcode::ZeroExtend32To64) ||
					extend.operand_count != 1)
				{
					return false;
				}
				ValueId sum_value = ResolveLoopParameter(extend.operands[0]);
				if (sum_value >= m_definitions.size() || !m_definitions[sum_value])
					return false;
				const Node& sum = *m_definitions[sum_value];
				if (sum.opcode != Opcode::Add32 || sum.operand_count != 2)
					return false;

				auto is_parameter_low = [&](ValueId candidate) {
					candidate = ResolveLoopParameter(candidate);
					if (candidate >= m_definitions.size() || !m_definitions[candidate])
						return false;
					const Node& extract = *m_definitions[candidate];
					return extract.opcode == Opcode::ExtractLow32 &&
						extract.operand_count == 1 &&
						ResolveLoopParameter(extract.operands[0]) == parameter;
				};
				auto constant = [&](ValueId candidate, u32* literal) {
					candidate = ResolveLoopParameter(candidate);
					if (candidate >= m_definitions.size() || !m_definitions[candidate])
						return false;
					const Node& node = *m_definitions[candidate];
					if (node.opcode != Opcode::ConstantI32)
						return false;
					*literal = static_cast<u32>(node.literal);
					return true;
				};
				u32 literal = 0;
				const bool matched =
					(is_parameter_low(sum.operands[0]) &&
					 constant(sum.operands[1], &literal)) ||
					(is_parameter_low(sum.operands[1]) &&
					 constant(sum.operands[0], &literal));
				if (!matched)
					return false;
				const Node* overflow = nullptr;
				for (u32 block : m_plan.iteration_blocks)
				{
					for (const Node& candidate : m_program.blocks[block].nodes)
					{
						if (candidate.opcode != Opcode::SignedAddOverflow32 ||
							candidate.operand_count != 2 ||
							candidate.operands[0] != sum.operands[0] ||
							candidate.operands[1] != sum.operands[1])
						{
							continue;
						}
						if (overflow)
							return false;
						overflow = &candidate;
					}
				}
				if (overflow && !FindUniqueConsumer(Opcode::ExitIfTrue,
						overflow->id, 0))
				{
					return false;
				}
				*delta = static_cast<s32>(literal);
				*extension = extend.opcode == Opcode::SignExtend32To64 ?
					RegionExecution::Low32Extension::Sign :
					RegionExecution::Low32Extension::Zero;
				*signed_overflow_guard = overflow != nullptr;
				return true;
			}

			void CollectChangedSearchState(const Transfer& transfer,
				std::vector<StateOutput>* outputs) const
			{
				if (!outputs || m_header >= m_program.blocks.size())
					return;
				const StateMap& header = m_program.blocks[m_header].parameters;
				for (size_t slot = 0;
					slot + 1 < RegionExecution::STATE_SLOT_COUNT; slot++)
				{
					const ValueId parameter =
						RegionExecution::StateValue(header, slot);
					const ValueId incoming =
						RegionExecution::StateValue(transfer.state, slot);
					if (ResolveLoopParameter(incoming) == parameter)
						continue;
					outputs->push_back({static_cast<u16>(slot), incoming});
				}
			}

			bool PreservesHigh64From(ValueId value, ValueId parameter,
				std::set<ValueId>* visiting) const
			{
				if (!visiting)
					return false;
				value = ResolveLoopParameter(value);
				parameter = ResolveLoopParameter(parameter);
				if (value == parameter)
					return true;
				if (!visiting->insert(value).second || value >= m_definitions.size() ||
					!m_definitions[value])
				{
					return false;
				}
				const Node& node = *m_definitions[value];
				ValueId predecessor = INVALID_VALUE;
				if (node.opcode == Opcode::ReplaceLow64 && node.operand_count == 2)
				{
					predecessor = node.operands[0];
				}
				else if (node.opcode == Opcode::MemoryLoadValue &&
					node.operand_count == 1)
				{
					const Node* load = Definition(node.operands[0]);
					if (load && load->opcode == Opcode::MemoryLoad &&
						load->operand_count == 3 &&
						node.type == ValueType::I128 &&
						static_cast<MemoryAccessKind>(load->immediate) !=
							MemoryAccessKind::Load128)
					{
						predecessor = load->operands[2];
					}
				}
				const bool preserved = predecessor != INVALID_VALUE &&
					PreservesHigh64From(predecessor, parameter, visiting);
				visiting->erase(value);
				return preserved;
			}

			bool PreservesHigh64From(ValueId value, ValueId parameter) const
			{
				std::set<ValueId> visiting;
				return PreservesHigh64From(value, parameter, &visiting);
			}

			bool MatchLow32Payload(ValueId value, ValueId parameter,
				ValueId* payload,
				RegionExecution::Low32Extension* extension) const
			{
				if (!payload || !extension)
					return false;
				value = ResolveLoopParameter(value);
				if (value >= m_definitions.size() || !m_definitions[value])
					return false;
				const Node& replace = *m_definitions[value];
				if (replace.opcode != Opcode::ReplaceLow64 ||
					replace.operand_count != 2 ||
					!PreservesHigh64From(replace.operands[0], parameter))
				{
					return false;
				}
				ValueId low = ResolveLoopParameter(replace.operands[1]);
				if (low >= m_definitions.size() || !m_definitions[low])
					return false;
				const Node& extend = *m_definitions[low];
				if ((extend.opcode != Opcode::SignExtend32To64 &&
					 extend.opcode != Opcode::ZeroExtend32To64) ||
					extend.operand_count != 1)
				{
					return false;
				}
				*payload = ResolveLoopParameter(extend.operands[0]);
				*extension = extend.opcode == Opcode::SignExtend32To64 ?
					RegionExecution::Low32Extension::Sign :
					RegionExecution::Low32Extension::Zero;
				return true;
			}

			bool BuildSearchOutputRecipes(const BoundedEqualSearch& search,
				const std::vector<StateOutput>& states, bool match,
				std::vector<BoundedSearchOutput>* recipes) const
			{
				if (!recipes || m_header >= m_program.blocks.size() ||
					m_latch >= m_program.blocks.size())
				{
					return false;
				}
				recipes->clear();
				const StateMap& header = m_program.blocks[m_header].parameters;
				const ValueId latch_condition = ResolveLoopParameter(
					m_program.blocks[m_latch].terminator.condition);
				const ValueId load_value = ResolveLoopParameter(search.load_value);
				auto constant_zero = [&](auto&& self, ValueId value) -> bool {
					value = ResolveLoopParameter(value);
					if (value == header.gpr[0])
						return true;
					if (value >= m_definitions.size() || !m_definitions[value])
						return false;
					const Node& node = *m_definitions[value];
					if (node.opcode == Opcode::ConstantI1 ||
						node.opcode == Opcode::ConstantI32 ||
						node.opcode == Opcode::ConstantI64)
					{
						return node.literal == 0;
					}
					return node.operand_count == 1 &&
						(node.opcode == Opcode::ExtractLow32 ||
						 node.opcode == Opcode::ExtractLow64 ||
						 node.opcode == Opcode::SignExtend32To64 ||
						 node.opcode == Opcode::ZeroExtend32To64 ||
						 node.opcode == Opcode::Truncate64To32) &&
						self(self, node.operands[0]);
				};
				auto exhaustion_proves_false_payload = [&](ValueId payload) {
					if (latch_condition >= m_definitions.size() ||
						!m_definitions[latch_condition])
					{
						return false;
					}
					const Node& condition = *m_definitions[latch_condition];
					if ((condition.opcode != Opcode::CompareEqual64 &&
						 condition.opcode != Opcode::CompareNotEqual64) ||
						condition.operand_count != 2)
					{
						return false;
					}
					const ValueId resolved_payload = ResolveLoopParameter(payload);
					auto equals_boolean_payload = [&](auto&& self,
						ValueId value) -> bool {
						value = ResolveLoopParameter(value);
						if (value == resolved_payload)
							return true;
						if (value >= m_definitions.size() || !m_definitions[value])
							return false;
						const Node& node = *m_definitions[value];
						if (node.operand_count != 1)
						{
							if (node.opcode != Opcode::ReplaceLow64 ||
								node.operand_count != 2)
							{
								return false;
							}
							// ReplaceLow64 completely owns the value observed by a
							// subsequent ExtractLow64.  Its preserved high-half input is
							// deliberately not part of the boolean proof.
							return self(self, node.operands[1]);
						}
						return (node.opcode == Opcode::ExtractLow32 ||
								node.opcode == Opcode::ExtractLow64 ||
								node.opcode == Opcode::SignExtend32To64 ||
								node.opcode == Opcode::ZeroExtend32To64 ||
								node.opcode == Opcode::Truncate64To32) &&
							self(self, node.operands[0]);
					};
					const bool left_payload = equals_boolean_payload(
						equals_boolean_payload, condition.operands[0]);
					const bool right_payload = equals_boolean_payload(
						equals_boolean_payload, condition.operands[1]);
					if (left_payload == right_payload ||
						!constant_zero(constant_zero,
							condition.operands[left_payload ? 1 : 0]))
					{
						return false;
					}
					const Terminator& terminator =
						m_program.blocks[m_latch].terminator;
					if (m_completion != &terminator.taken &&
						m_completion != &terminator.not_taken)
					{
						return false;
					}
					const bool completion_when_true =
						m_completion == &terminator.taken;
					return condition.opcode == Opcode::CompareEqual64 ?
						completion_when_true : !completion_when_true;
				};
				for (const StateOutput& state : states)
				{
					const RegionExecution::StateSlot decoded =
						RegionExecution::DecodeStateSlot(state.state_slot);
					if (decoded.state_class != RegionExecution::StateClass::Gpr ||
						decoded.index == 0 || decoded.index >= header.gpr.size())
					{
						return false;
					}
					BoundedSearchOutput recipe{};
					recipe.publication = state;
					recipe.source_gpr = decoded.index;
					const ValueId parameter = header.gpr[decoded.index];
					const ValueId incoming = ResolveLoopParameter(state.value);
					if (incoming == load_value)
					{
						recipe.kind = BoundedSearchOutputKind::LoadedValue;
						recipe.extension = search.load_kind ==
							MemoryAccessKind::LoadS8 || search.load_kind ==
							MemoryAccessKind::LoadS16 || search.load_kind ==
							MemoryAccessKind::LoadS32 ?
							RegionExecution::Low32Extension::Sign :
							RegionExecution::Low32Extension::Zero;
						recipes->push_back(recipe);
						continue;
					}

					s32 delta = 0;
					RegionExecution::Low32Extension extension{};
					bool signed_overflow = false;
					if (MatchLow32AddRecurrence(incoming, parameter, &delta,
							&extension, &signed_overflow) && !signed_overflow)
					{
						const auto recurrence = std::find_if(m_plan.recurrences.begin(),
							m_plan.recurrences.end(), [&](const Low32Recurrence& item) {
								return item.gpr == decoded.index && item.delta == delta &&
									item.extension == extension &&
									!item.signed_overflow_guard;
							});
						recipe.kind = !match && recurrence != m_plan.recurrences.end() ?
							BoundedSearchOutputKind::AffineRecurrence :
							BoundedSearchOutputKind::HeaderLow32Add;
						recipe.delta = delta;
						recipe.extension = extension;
						recipes->push_back(recipe);
						continue;
					}

					ValueId payload = INVALID_VALUE;
					if (MatchLow32Payload(incoming, parameter, &payload, &extension))
					{
						if (payload == load_value)
						{
							recipe.kind = BoundedSearchOutputKind::LoadedValue;
							recipe.extension = extension;
							recipes->push_back(recipe);
							continue;
						}
						if (!match && exhaustion_proves_false_payload(payload))
						{
							recipe.kind = BoundedSearchOutputKind::FalsePredicate;
							recipe.extension = extension;
							recipes->push_back(recipe);
							continue;
						}
					}
					return false;
				}
				return true;
			}

			BuildResult BuildStateContract()
			{
				std::map<u8, s32> expected;
				auto require_recurrence = [&](u8 gpr, s32 delta) {
					const auto found = expected.find(gpr);
					if (found != expected.end() && found->second != delta)
						return false;
					expected[gpr] = delta;
					return true;
				};
				if (!require_recurrence(m_memory.control.counter_gpr,
						m_memory.control.counter_stride))
				{
					return Fail(Failure::UnsupportedStateRecurrence,
						"loop control has conflicting recurrence deltas");
				}
				for (const PatternStream& stream : m_plan.pattern_streams)
				{
					if (stream.stride > static_cast<u32>(INT32_MAX) ||
						!require_recurrence(stream.induction_gpr,
							static_cast<s32>(stream.stride)))
					{
						return Fail(Failure::UnsupportedStateRecurrence,
							"memory stream conflicts with its loop-carried GPR");
					}
				}
				for (const CopyStream& stream : m_plan.copy_streams)
				{
					if (stream.stride > static_cast<u32>(INT32_MAX) ||
						!require_recurrence(stream.source_induction_gpr,
							static_cast<s32>(stream.stride)) ||
						!require_recurrence(stream.destination_induction_gpr,
							static_cast<s32>(stream.stride)))
					{
						return Fail(Failure::UnsupportedStateRecurrence,
							"copy stream conflicts with a loop-carried GPR");
					}
				}
				for (const Vu0AffineStream& stream : m_plan.vu0_affine_streams)
				{
					if (stream.stride > static_cast<u32>(INT32_MAX) ||
						!require_recurrence(stream.source_induction_gpr,
							static_cast<s32>(stream.stride)) ||
						!require_recurrence(stream.destination_induction_gpr,
							static_cast<s32>(stream.stride)))
					{
						return Fail(Failure::UnsupportedStateRecurrence,
							"VU0 transform stream conflicts with a loop-carried GPR");
					}
				}
				for (const BoundedEqualSearch& search :
					m_plan.bounded_equal_searches)
				{
					if (search.stride > static_cast<u32>(INT32_MAX) ||
						!require_recurrence(search.induction_gpr,
							static_cast<s32>(search.stride)))
					{
						return Fail(Failure::UnsupportedBoundedSearchState,
							"bounded search pointer conflicts with loop control");
					}
				}

				std::set<u8> observed;
				std::set<ValueId> demanded_final_loads;
				Vu0AffineStream* affine = m_plan.vu0_affine_streams.empty() ?
					nullptr : &m_plan.vu0_affine_streams.front();
				bool affine_input = false;
				bool affine_output = false;
				bool affine_acc = false;
				bool affine_mac = false;
				bool affine_status = false;
				bool affine_vi_mac = false;
				bool affine_vi_status = false;
				u32 matched_signed_overflow_guards = 0;
				auto match_affine_state = [&](const RegionExecution::StateSlot& decoded,
					ValueId incoming) {
					if (!affine)
						return false;
					incoming = ResolveLoopParameter(incoming);
					switch (decoded.state_class)
					{
						case RegionExecution::StateClass::Vu0Vf:
							if (decoded.index == affine->output_vf &&
								incoming == ResolveLoopParameter(affine->final_output))
							{
								affine_output = true;
								return true;
							}
							if (decoded.index == affine->input_vf &&
								affine->input_vf != affine->output_vf &&
								incoming == ResolveLoopParameter(affine->final_input))
							{
								affine_input = true;
								return true;
							}
							return false;
						case RegionExecution::StateClass::Vu0Acc:
							affine_acc = incoming == ResolveLoopParameter(affine->final_acc);
							return affine_acc;
						case RegionExecution::StateClass::Vu0MacFlag:
							affine_mac = incoming == ResolveLoopParameter(affine->final_mac);
							return affine_mac;
						case RegionExecution::StateClass::Vu0StatusFlag:
							affine_status = incoming ==
								ResolveLoopParameter(affine->final_status);
							return affine_status;
						case RegionExecution::StateClass::Vu0Vi:
							if (decoded.index == affine->status_vi &&
								incoming == ResolveLoopParameter(affine->final_vi_status))
							{
								affine_vi_status = true;
								return true;
							}
							if (incoming == ResolveLoopParameter(affine->final_mac) &&
								(!affine_vi_mac || affine->mac_vi == decoded.index))
							{
								affine->mac_vi = decoded.index;
								affine_vi_mac = true;
								return true;
							}
							return false;
						default:
							return false;
					}
				};
				auto final_load_for_gpr = [&](u8 gpr, ValueId incoming) {
					incoming = ResolveLoopParameter(incoming);
					for (const FinalLoadState& load : m_plan.final_load_state)
					{
						if (load.gpr == gpr &&
							ResolveLoopParameter(load.load_value) == incoming)
						{
							demanded_final_loads.insert(load.load_value);
							return true;
						}
					}
					return false;
				};
				const Block& header = m_program.blocks[m_header];
				for (size_t slot = 0; slot < RegionExecution::STATE_SLOT_COUNT; slot++)
				{
					const ValueId parameter =
						RegionExecution::StateValue(header.parameters, slot);
					const ValueId incoming =
						RegionExecution::StateValue(m_backedge->state, slot);
					if (slot == RegionExecution::STATE_SLOT_COUNT - 1)
						continue; // cycle is owned by the exact event-phase contract.
					if (ResolveLoopParameter(incoming) == parameter)
						continue;
					const RegionExecution::StateSlot decoded =
						RegionExecution::DecodeStateSlot(slot);
					if (decoded.state_class != RegionExecution::StateClass::Gpr)
					{
						if (match_affine_state(decoded, incoming))
							continue;
						return Fail(Failure::UnsupportedStateRecurrence,
							"kernel changes non-GPR architectural state", m_latch,
							incoming);
					}
					if (final_load_for_gpr(decoded.index, incoming))
						continue;
					const auto expected_delta = expected.find(decoded.index);
					if (expected_delta == expected.end())
					{
						if (m_plan.kind == Kind::BoundedEqualSearch &&
							(ValueDependsOn(incoming,
								 m_program.blocks[m_latch].terminator.condition) ||
							 ValueDependsOn(
								 m_program.blocks[m_latch].terminator.condition,
								 incoming)))
						{
							continue;
						}
						return Fail(Failure::UnsupportedStateRecurrence,
							"kernel changes GPR " +
								std::to_string(decoded.index) +
								" outside its proven streams/control (value " +
								std::to_string(incoming) + ")",
							m_latch, incoming);
					}
					s32 delta = 0;
					RegionExecution::Low32Extension extension{};
					bool signed_overflow_guard = false;
					if (!MatchLow32AddRecurrence(incoming, parameter, &delta, &extension,
							&signed_overflow_guard) ||
						delta != expected_delta->second)
					{
						return Fail(Failure::UnsupportedStateRecurrence,
							"loop-carried GPR is not the exact low-word affine recurrence",
							m_latch, incoming);
					}
					m_plan.recurrences.push_back({decoded.index, delta, extension,
						signed_overflow_guard});
					matched_signed_overflow_guards += signed_overflow_guard ? 1u : 0u;
					observed.insert(decoded.index);
				}
				for (const auto& [gpr, delta] : expected)
				{
					(void)delta;
					if (gpr == 0 || observed.find(gpr) == observed.end())
						return Fail(Failure::UnsupportedStateRecurrence,
							"memory/control recurrence is absent from architectural state",
							m_latch);
				}
				if (affine &&
					((affine->input_vf != affine->output_vf && !affine_input) ||
					 !affine_output || !affine_acc || !affine_mac || !affine_status ||
					 !affine_vi_mac || !affine_vi_status))
				{
					return Fail(Failure::UnsupportedVu0AffineState,
						"VU0 affine completion does not publish its exact VF/ACC/flag state",
						m_latch);
				}
				if (affine && (affine->idle_vi == affine->mac_vi ||
					affine->idle_vi == affine->status_vi ||
					affine->mac_vi == affine->status_vi))
				{
					return Fail(Failure::UnsupportedVu0AffineState,
						"VU0 affine control registers alias distinct architectural owners",
						m_latch);
				}
				if (matched_signed_overflow_guards != m_signed_overflow_exits)
				{
					return Fail(Failure::UnsupportedStateRecurrence,
						"not every signed ADDI exit belongs to a proven affine recurrence",
						m_latch);
				}
				std::erase_if(m_plan.final_load_state,
					[&](const FinalLoadState& load) {
						return demanded_final_loads.find(load.load_value) ==
							demanded_final_loads.end();
					});
				if (m_plan.kind == Kind::BoundedEqualSearch)
				{
					if (m_plan.bounded_equal_searches.size() != 1 || !m_search_match ||
						!m_backedge || !m_completion)
					{
						return Fail(Failure::UnsupportedBoundedSearchState,
							"bounded search has no unique verifier-owned state seams",
							m_latch);
					}
					BoundedEqualSearch& search =
						m_plan.bounded_equal_searches.front();
					CollectChangedSearchState(*m_backedge, &search.backedge_state);
					CollectChangedSearchState(*m_search_match, &search.match_state);
					CollectChangedSearchState(*m_completion, &search.exhausted_state);
					std::vector<BoundedSearchOutput> match_outputs;
					std::vector<BoundedSearchOutput> exhausted_outputs;
					(void)BuildSearchOutputRecipes(search, search.match_state, true,
						&match_outputs);
					(void)BuildSearchOutputRecipes(search, search.exhausted_state, false,
						&exhausted_outputs);
					// A partial vector is intentionally non-publishable (the backend requires
					// one recipe per changed state), but retaining it makes cold diagnostics
					// identify the first unclassified seam without another source-shaped log.
					search.native_match_outputs = std::move(match_outputs);
					search.native_exhausted_outputs = std::move(exhausted_outputs);
				}
				return {};
			}

			const Program& m_program;
			const RegionExecution::Plan& m_execution;
			const RegionMemoryPlan::Plan& m_memory;
			std::vector<const Node*> m_definitions;
			std::vector<u32> m_defining_blocks;
			std::vector<u8> m_invariant_state;
			std::vector<Edge> m_edges;
			std::vector<std::vector<u32>> m_predecessors;
			u32 m_header = INVALID_BLOCK;
			u32 m_latch = INVALID_BLOCK;
			u64 m_loop_mask = 0;
			const Transfer* m_backedge = nullptr;
			const Transfer* m_completion = nullptr;
			const Transfer* m_search_match = nullptr;
			const Transfer* m_search_continue = nullptr;
			u32 m_search_block = INVALID_BLOCK;
			u64 m_search_loop_mask = 0;
			bool m_has_vu0_observers = false;
			u32 m_signed_overflow_exits = 0;
			Plan m_plan{};
		};
	} // namespace

	BuildResult Build(const RegionIR::Program& program)
	{
		const RegionIR::VerifyResult verified = RegionIR::Verify(program);
		if (!verified)
			return Fail(Failure::InvalidProgram, verified.detail, verified.block);
		const RegionExecution::BuildResult execution = RegionExecution::Build(program);
		if (!execution)
			return Fail(Failure::InvalidExecutionPlan, execution.detail,
				execution.block, execution.value);
		const RegionMemoryPlan::BuildResult memory = RegionMemoryPlan::Build(program);
		if (!memory)
			return Fail(Failure::InvalidMemoryPlan, memory.detail);
		return Analyzer(program, execution.plan, memory.plan).Run();
	}

	namespace
	{
		constexpr u32 COP1_SIGN = 0x80000000u;
		constexpr u32 COP1_EXPONENT = 0x7f800000u;
		constexpr u32 COP1_FRACTION = 0x007fffffu;
		constexpr u32 COP1_MAX_FINITE = 0x7f7fffffu;
		constexpr u32 COP1_CVT_W_MAX_EXPONENT = 0x4e800000u;
		constexpr u32 COP1_EXPONENT_BIAS = 127u;
		constexpr u32 COP1_MANTISSA_BITS = 23u;
		constexpr u32 COP1_IMPLICIT_MANTISSA = 1u << COP1_MANTISSA_BITS;
		constexpr u32 FCR31_O = 0x00008000u;
		constexpr u32 FCR31_U = 0x00004000u;
		constexpr u32 FCR31_SO = 0x00000010u;
		constexpr u32 FCR31_SU = 0x00000008u;

		u128 ReferenceBits(u64 low, u64 high = 0)
		{
			return {low, high};
		}

		bool ReadCanonicalStateSlot(const CanonicalState& state, size_t slot,
			u128* value)
		{
			if (!value)
				return false;
			*value = {};
			const u8 words = RegionExecution::StateWordCount(slot);
			if (words == 0 || words > 4)
				return false;
			u32 raw[4]{};
			for (u8 word = 0; word < words; word++)
			{
				const size_t offset =
					RegionExecution::CanonicalStateWordOffset(slot, word);
				if (offset == SIZE_MAX || offset + sizeof(u32) > sizeof(state))
					return false;
				std::memcpy(&raw[word], reinterpret_cast<const u8*>(&state) + offset,
					sizeof(raw[word]));
			}
			std::memcpy(value, raw, words * sizeof(u32));
			return true;
		}

		bool WriteCanonicalStateSlot(CanonicalState* state, size_t slot,
			const u128& value)
		{
			if (!state)
				return false;
			const u8 words = RegionExecution::StateWordCount(slot);
			if (words == 0 || words > 4)
				return false;
			u32 raw[4]{};
			std::memcpy(raw, &value, words * sizeof(u32));
			for (u8 word = 0; word < words; word++)
			{
				const size_t offset =
					RegionExecution::CanonicalStateWordOffset(slot, word);
				if (offset == SIZE_MAX || offset + sizeof(u32) > sizeof(*state))
					return false;
				std::memcpy(reinterpret_cast<u8*>(state) + offset, &raw[word],
					sizeof(raw[word]));
			}
			return true;
		}

		u32 ReferenceNormalizeCop1Input(u32 value)
		{
			const u32 exponent = value & COP1_EXPONENT;
			if (exponent == 0)
				return value & COP1_SIGN;
			if (exponent == COP1_EXPONENT)
				return (value & COP1_SIGN) | COP1_MAX_FINITE;
			return value;
		}

		u32 ReferenceEvaluateCop1Raw(Opcode opcode, u32 left_bits,
			u32 right_bits)
		{
			const float left = std::bit_cast<float>(left_bits);
			const float right = std::bit_cast<float>(right_bits);
			float result = 0.0f;
			switch (opcode)
			{
				case Opcode::Cop1AddRaw:
					result = left + right;
					break;
				case Opcode::Cop1SubRaw:
					result = left - right;
					break;
				case Opcode::Cop1MulRaw:
					result = left * right;
					break;
				default:
					break;
			}
			return std::bit_cast<u32>(result);
		}

		bool ReferenceExceptionalCop1Result(u32 raw)
		{
			return (raw & ~COP1_SIGN) == COP1_EXPONENT ||
				((raw & COP1_EXPONENT) == 0 && (raw & COP1_FRACTION) != 0);
		}

		u32 ReferenceClampCop1Result(u32 raw)
		{
			if ((raw & ~COP1_SIGN) == COP1_EXPONENT)
				return (raw & COP1_SIGN) | COP1_MAX_FINITE;
			if ((raw & COP1_EXPONENT) == 0 && (raw & COP1_FRACTION) != 0)
				return raw & COP1_SIGN;
			return raw;
		}

		u32 ReferenceUpdateCop1OuFlags(u32 fcr31, u32 raw)
		{
			if ((raw & ~COP1_SIGN) == COP1_EXPONENT)
				return fcr31 | FCR31_O | FCR31_SO;
			fcr31 &= ~FCR31_O;
			if ((raw & COP1_EXPONENT) == 0 && (raw & COP1_FRACTION) != 0)
				return fcr31 | FCR31_U | FCR31_SU;
			return fcr31 & ~FCR31_U;
		}

		u32 ReferenceConvertCop1Word(u32 raw)
		{
			const u32 exponent_bits = raw & COP1_EXPONENT;
			if (exponent_bits > COP1_CVT_W_MAX_EXPONENT)
				return (raw & COP1_SIGN) != 0 ? 0x80000000u : 0x7fffffffu;
			const u32 exponent = exponent_bits >> COP1_MANTISSA_BITS;
			if (exponent < COP1_EXPONENT_BIAS)
				return 0;
			const u32 unbiased = exponent - COP1_EXPONENT_BIAS;
			const u32 mantissa =
				(raw & COP1_FRACTION) | COP1_IMPLICIT_MANTISSA;
			const u32 magnitude = unbiased >= COP1_MANTISSA_BITS ?
				mantissa << (unbiased - COP1_MANTISSA_BITS) :
				mantissa >> (COP1_MANTISSA_BITS - unbiased);
			return (raw & COP1_SIGN) != 0 ? 0u - magnitude : magnitude;
		}

		ReferenceResult InterpretCop1Stream(const Program& program,
			const Plan& plan, const CanonicalState& input, CanonicalState* output,
			const ReferenceOptions& options)
		{
			auto fail = [](ReferenceFailure failure, u32 address = 0,
				ValueId value = INVALID_VALUE) {
				ReferenceResult result{};
				result.failure = failure;
				result.memory_address = address;
				result.value = value;
				return result;
			};
			if (!output || !options.memory || !options.memory->probe ||
				plan.kind != Kind::Cop1Stream || plan.cop1_streams.size() != 1 ||
				plan.header_block != program.entry_block ||
				plan.header_block >= program.blocks.size() ||
				plan.latch_block >= program.blocks.size())
			{
				return fail(ReferenceFailure::InvalidDescriptor);
			}
			const RegionMemoryPlan::BuildResult memory_plan =
				RegionMemoryPlan::Build(program);
			if (!memory_plan || !memory_plan.plan.header_seeds_match_entry)
				return fail(ReferenceFailure::InvalidDescriptor);
			*output = input;

			u32 iterations = 0;
			const u32 counter = plan.control.counter_seed_is_immediate ?
				plan.control.counter_seed_immediate :
				static_cast<u32>(input.gpr[plan.control.counter_gpr].lo);
			const u32 bound = plan.control.bound_is_immediate ?
				plan.control.bound_immediate :
				(plan.control.bound_gpr < input.gpr.size() ?
					static_cast<u32>(input.gpr[plan.control.bound_gpr].lo) : 0);
			if (!RegionMemoryPlan::CalculateTripCount(plan.control, counter, bound,
					&iterations) || iterations == 0 ||
				iterations > options.maximum_iterations)
			{
				return fail(ReferenceFailure::InvalidTripCount);
			}
			for (const Low32Recurrence& recurrence : plan.recurrences)
			{
				if (!recurrence.signed_overflow_guard)
					continue;
				if (recurrence.gpr == 0 || recurrence.gpr >= input.gpr.size())
					return fail(ReferenceFailure::InvalidDescriptor);
				s64 current = static_cast<s32>(
					static_cast<u32>(input.gpr[recurrence.gpr].lo));
				for (u32 iteration = 0; iteration < iterations; iteration++)
				{
					const s64 next = current + recurrence.delta;
					if (next < INT32_MIN || next > INT32_MAX)
						return fail(ReferenceFailure::ArithmeticOverflow);
					current = next;
				}
			}
			const u64 repeated = static_cast<u64>(iterations - 1) *
				plan.repeated_scaled_cycles;
			if (repeated > UINT64_MAX - plan.completion_scaled_cycles ||
				input.cycle > UINT64_MAX - repeated - plan.completion_scaled_cycles)
			{
				return fail(ReferenceFailure::CycleOverflow);
			}

			std::vector<const Node*> definitions(program.value_count, nullptr);
			for (const Block& block : program.blocks)
			{
				for (const Node& node : block.nodes)
				{
					if (node.id >= definitions.size() || definitions[node.id])
						return fail(ReferenceFailure::InvalidDescriptor, 0, node.id);
					definitions[node.id] = &node;
				}
			}
			std::vector<u128> values(program.value_count);
			std::vector<bool> defined(program.value_count, false);
			auto read_value = [&](ValueId value) {
				if (value >= values.size())
					return false;
				if (defined[value])
					return true;
				const Node* node = definitions[value];
				if (node && (node->opcode == Opcode::ConstantI1 ||
					node->opcode == Opcode::ConstantI32 ||
					node->opcode == Opcode::ConstantI64 ||
					node->opcode == Opcode::ConstantAddress))
				{
					values[value] = ReferenceBits(node->literal);
					defined[value] = true;
					return true;
				}
				u128 bits{};
				if (!options.values.read ||
					!options.values.read(options.values.context, value, &bits))
				{
					return false;
				}
				values[value] = bits;
				defined[value] = true;
				return true;
			};
			auto assign_header = [&](const CanonicalState& state) {
				const Block& header = program.blocks[plan.header_block];
				for (size_t slot = 0; slot < RegionExecution::STATE_SLOT_COUNT;
					slot++)
				{
					const ValueId parameter =
						RegionExecution::StateValue(header.parameters, slot);
					u128 bits{};
					if (parameter >= values.size() ||
						!ReadCanonicalStateSlot(state, slot, &bits))
					{
						return false;
					}
					values[parameter] = bits;
					defined[parameter] = true;
				}
				if (header.parameters.memory_effect >= values.size())
					return false;
				values[header.parameters.memory_effect] = {};
				defined[header.parameters.memory_effect] = true;
				return true;
			};
			auto assign_transfer = [&](const Transfer& transfer,
				u32 target_block) {
				if (target_block >= program.blocks.size())
					return false;
				const Block& target = program.blocks[target_block];
				for (size_t slot = 0; slot < RegionExecution::STATE_SLOT_COUNT;
					slot++)
				{
					const ValueId source =
						RegionExecution::StateValue(transfer.state, slot);
					const ValueId parameter =
						RegionExecution::StateValue(target.parameters, slot);
					if (!read_value(source) || parameter >= values.size())
						return false;
					values[parameter] = values[source];
					defined[parameter] = true;
				}
				if (!read_value(transfer.state.memory_effect) ||
					target.parameters.memory_effect >= values.size())
				{
					return false;
				}
				values[target.parameters.memory_effect] =
					values[transfer.state.memory_effect];
				defined[target.parameters.memory_effect] = true;
				return true;
			};
			auto transfer_to = [&](const Block& block, u32 target) -> const Transfer* {
				const Transfer* found = nullptr;
				bool ambiguous = false;
				VisitTransfers(block.terminator, [&](const Transfer& transfer) {
					if (transfer.target_block == target)
					{
						ambiguous |= found != nullptr;
						found = &transfer;
					}
				});
				return ambiguous ? nullptr : found;
			};

			struct Span
			{
				u32 begin = 0;
				u32 end = 0;
			};
			struct PendingWrite
			{
				MemoryRequest request{};
				u128 value{};
			};
			std::vector<Span> read_spans;
			std::vector<Span> write_spans;
			std::vector<PendingWrite> writes;
			if (!assign_header(input))
				return fail(ReferenceFailure::InvalidDescriptor);
			for (u32 iteration = 0; iteration < iterations; iteration++)
			{
				for (size_t path_index = 0;
					path_index < plan.iteration_blocks.size(); path_index++)
				{
					const u32 block_index = plan.iteration_blocks[path_index];
					if (block_index >= program.blocks.size())
						return fail(ReferenceFailure::InvalidDescriptor);
					const Block& block = program.blocks[block_index];
					if (path_index != 0)
					{
						const Block& previous = program.blocks[
							plan.iteration_blocks[path_index - 1]];
						const Transfer* incoming = transfer_to(previous, block_index);
						if (!incoming || !assign_transfer(*incoming, block_index))
							return fail(ReferenceFailure::InvalidDescriptor);
					}

					for (const Node& node : block.nodes)
					{
						if (node.opcode == Opcode::Parameter)
							continue;
						const bool annulled_likely_delay =
							block.terminator.kind == TerminatorKind::Branch &&
							block.terminator.likely &&
							node.source_pc == block.terminator.delay_slot_pc &&
							read_value(block.terminator.condition) &&
							values[block.terminator.condition].lo == 0;
						if (annulled_likely_delay)
							continue;
						for (u32 operand = 0; operand < node.operand_count; operand++)
						{
							if (!read_value(node.operands[operand]))
								return fail(ReferenceFailure::MissingInvariantValue, 0,
									node.operands[operand]);
						}
						const u128 left = node.operand_count > 0 ?
							values[node.operands[0]] : u128{};
						const u128 right = node.operand_count > 1 ?
							values[node.operands[1]] : u128{};
						const u128 third = node.operand_count > 2 ?
							values[node.operands[2]] : u128{};
						u128 bits{};
						switch (node.opcode)
						{
							case Opcode::ConstantI1:
							case Opcode::ConstantI32:
							case Opcode::ConstantI64:
							case Opcode::ConstantAddress:
								bits = ReferenceBits(node.literal);
								break;
							case Opcode::NoEffect:
								break;
							case Opcode::ExtractLow32:
								bits = ReferenceBits(static_cast<u32>(left.lo));
								break;
							case Opcode::ExtractLow64:
								bits = ReferenceBits(left.lo);
								break;
							case Opcode::ReplaceLow64:
								bits = left;
								bits.lo = right.lo;
								break;
							case Opcode::BitcastI32ToF32Bits:
							case Opcode::BitcastF32BitsToI32:
								bits = ReferenceBits(static_cast<u32>(left.lo));
								break;
							case Opcode::Cop1NormalizeInput:
								bits = ReferenceBits(ReferenceNormalizeCop1Input(
									static_cast<u32>(left.lo)));
								break;
							case Opcode::Cop1AddRaw:
							case Opcode::Cop1SubRaw:
							case Opcode::Cop1MulRaw:
								bits = ReferenceBits(ReferenceEvaluateCop1Raw(node.opcode,
									static_cast<u32>(left.lo),
									static_cast<u32>(right.lo)));
								break;
							case Opcode::Cop1ExceptionalOuResult:
								bits = ReferenceBits(ReferenceExceptionalCop1Result(
									static_cast<u32>(left.lo)) ? 1u : 0u);
								break;
							case Opcode::Cop1ClampOuResult:
								bits = ReferenceBits(ReferenceClampCop1Result(
									static_cast<u32>(left.lo)));
								break;
							case Opcode::Cop1UpdateOuFlags:
								bits = ReferenceBits(ReferenceUpdateCop1OuFlags(
									static_cast<u32>(left.lo),
									static_cast<u32>(right.lo)));
								break;
							case Opcode::Cop1ConvertWord:
								bits = ReferenceBits(ReferenceConvertCop1Word(
									static_cast<u32>(left.lo)));
								break;
							case Opcode::SignExtend32To64:
								bits = ReferenceBits(static_cast<u64>(static_cast<s64>(
									static_cast<s32>(static_cast<u32>(left.lo)))));
								break;
							case Opcode::ZeroExtend32To64:
								bits = ReferenceBits(static_cast<u32>(left.lo));
								break;
							case Opcode::Truncate64To32:
								bits = ReferenceBits(static_cast<u32>(left.lo));
								break;
							case Opcode::Add32:
								bits = ReferenceBits(static_cast<u32>(left.lo) +
									static_cast<u32>(right.lo));
								break;
							case Opcode::Add64:
								bits = ReferenceBits(left.lo + right.lo);
								break;
							case Opcode::Sub32:
								bits = ReferenceBits(static_cast<u32>(left.lo) -
									static_cast<u32>(right.lo));
								break;
							case Opcode::Sub64:
								bits = ReferenceBits(left.lo - right.lo);
								break;
							case Opcode::And32:
								bits = ReferenceBits(static_cast<u32>(left.lo) &
									static_cast<u32>(right.lo));
								break;
							case Opcode::And64:
								bits = ReferenceBits(left.lo & right.lo);
								break;
							case Opcode::Or64:
								bits = ReferenceBits(left.lo | right.lo);
								break;
							case Opcode::Xor32:
								bits = ReferenceBits(static_cast<u32>(left.lo) ^
									static_cast<u32>(right.lo));
								break;
							case Opcode::Xor64:
								bits = ReferenceBits(left.lo ^ right.lo);
								break;
							case Opcode::ShiftLeft32:
								bits = ReferenceBits(static_cast<u32>(left.lo) <<
									node.immediate);
								break;
							case Opcode::ShiftRightLogical32:
								bits = ReferenceBits(static_cast<u32>(left.lo) >>
									node.immediate);
								break;
							case Opcode::ShiftRightArithmetic32:
								bits = ReferenceBits(static_cast<u32>(
									static_cast<s32>(static_cast<u32>(left.lo)) >>
									node.immediate));
								break;
							case Opcode::AddressFromI32:
								bits = ReferenceBits(static_cast<u32>(left.lo));
								break;
							case Opcode::EffectiveAddress32:
								bits = ReferenceBits(static_cast<u32>(left.lo) +
									static_cast<u32>(right.lo));
								break;
							case Opcode::CompareEqual64:
								bits = ReferenceBits(left.lo == right.lo);
								break;
							case Opcode::CompareNotEqual64:
								bits = ReferenceBits(left.lo != right.lo);
								break;
							case Opcode::CompareSignedLess64:
								bits = ReferenceBits(std::bit_cast<s64>(left.lo) <
									std::bit_cast<s64>(right.lo));
								break;
							case Opcode::CompareUnsignedLess64:
								bits = ReferenceBits(left.lo < right.lo);
								break;
							case Opcode::CompareSignedLessEqualZero64:
								bits = ReferenceBits(std::bit_cast<s64>(left.lo) <= 0);
								break;
							case Opcode::CompareSignedGreaterZero64:
								bits = ReferenceBits(std::bit_cast<s64>(left.lo) > 0);
								break;
							case Opcode::CompareSignedLessZero64:
								bits = ReferenceBits(std::bit_cast<s64>(left.lo) < 0);
								break;
							case Opcode::CompareSignedGreaterEqualZero64:
								bits = ReferenceBits(std::bit_cast<s64>(left.lo) >= 0);
								break;
							case Opcode::MemoryLoad:
							case Opcode::MemoryStore:
							{
								const MemoryAccessKind kind =
									static_cast<MemoryAccessKind>(node.immediate);
								const u32 unaligned = static_cast<u32>(right.lo);
								if ((unaligned & MemoryAlignmentMask(kind)) != 0)
									return fail(ReferenceFailure::MemoryProbe, unaligned,
										node.id);
								const u32 address = IsQuadMemoryAccess(kind) ?
									(unaligned & ~0xfu) : unaligned;
								const u32 width = MemoryAccessWidth(kind);
								if (width == 0 || address > UINT32_MAX - width)
									return fail(ReferenceFailure::RangeOverflow, address,
										node.id);
								MemoryRequest request{node.source_pc, address, kind};
								if (options.memory->probe(options.memory->context, request) !=
									MemoryProbeResult::Direct)
								{
									return fail(ReferenceFailure::MemoryProbe, address,
										node.id);
								}
								if (node.opcode == Opcode::MemoryLoad)
								{
									u128 raw{};
									if (!options.memory->read ||
										!options.memory->read(options.memory->context,
											request, &raw))
									{
										return fail(ReferenceFailure::MemoryRead, address,
											node.id);
									}
									bits = third;
									switch (kind)
									{
										case MemoryAccessKind::LoadS8:
											bits.lo = static_cast<u64>(static_cast<s64>(
												static_cast<s8>(raw.lo)));
											break;
										case MemoryAccessKind::LoadU8:
											bits.lo = static_cast<u8>(raw.lo);
											break;
										case MemoryAccessKind::LoadS16:
											bits.lo = static_cast<u64>(static_cast<s64>(
												static_cast<s16>(raw.lo)));
											break;
										case MemoryAccessKind::LoadU16:
											bits.lo = static_cast<u16>(raw.lo);
											break;
										case MemoryAccessKind::LoadS32:
											bits.lo = static_cast<u64>(static_cast<s64>(
												static_cast<s32>(raw.lo)));
											break;
										case MemoryAccessKind::LoadU32:
										case MemoryAccessKind::LoadF32Bits:
											bits.lo = static_cast<u32>(raw.lo);
											break;
										case MemoryAccessKind::Load64:
											bits.lo = raw.lo;
											break;
										case MemoryAccessKind::Load128:
										case MemoryAccessKind::LoadVu0Vector:
											bits = raw;
											break;
										default:
											return fail(ReferenceFailure::InvalidDescriptor, address,
												node.id);
									}
									read_spans.push_back({address, address + width});
								}
								else
								{
									writes.push_back({request, third});
									write_spans.push_back({address, address + width});
								}
								break;
							}
							case Opcode::MemoryLoadValue:
							case Opcode::BindGpr:
							case Opcode::BindHi:
							case Opcode::BindLo:
							case Opcode::BindSa:
							case Opcode::BindFpr:
							case Opcode::BindFcr31:
							case Opcode::BindAcc:
								bits = left;
								break;
							case Opcode::ExitIfTrue:
								if (left.lo != 0)
									return fail(ReferenceFailure::ArithmeticOverflow, 0,
										node.id);
								break;
							case Opcode::AdvanceCycles:
								bits = ReferenceBits(left.lo + node.immediate);
								break;
							default:
								return fail(ReferenceFailure::InvalidDescriptor, 0,
									node.id);
						}
						if (node.id >= values.size())
							return fail(ReferenceFailure::InvalidDescriptor, 0,
								node.id);
						values[node.id] = bits;
						defined[node.id] = true;
					}
				}

				const Block& latch = program.blocks[plan.latch_block];
				if (!read_value(latch.terminator.condition))
					return fail(ReferenceFailure::InvalidDescriptor, 0,
						latch.terminator.condition);
				const Transfer* selected = &latch.terminator.taken;
				if (latch.terminator.kind == TerminatorKind::Branch &&
					values[latch.terminator.condition].lo == 0)
				{
					selected = &latch.terminator.not_taken;
				}
				const u32 expected = iteration + 1 < iterations ?
					plan.header_block : plan.completion_block;
				if (selected->target_block != expected)
					return fail(ReferenceFailure::InvalidTripCount, 0,
						latch.terminator.condition);
				if (iteration + 1 < iterations &&
					!assign_transfer(*selected, plan.header_block))
				{
					return fail(ReferenceFailure::InvalidDescriptor);
				}
			}

			if (plan.cop1_streams.front().requires_disjoint_read_write_ranges)
			{
				auto by_begin = [](const Span& left, const Span& right) {
					return left.begin != right.begin ? left.begin < right.begin :
						left.end < right.end;
				};
				std::sort(read_spans.begin(), read_spans.end(), by_begin);
				std::sort(write_spans.begin(), write_spans.end(), by_begin);
				size_t read = 0;
				size_t write = 0;
				while (read < read_spans.size() && write < write_spans.size())
				{
					if (read_spans[read].begin < write_spans[write].end &&
						write_spans[write].begin < read_spans[read].end)
					{
						return fail(ReferenceFailure::AliasGuard,
							write_spans[write].begin);
					}
					if (read_spans[read].end <= write_spans[write].begin)
						read++;
					else
						write++;
				}
			}
			for (const PendingWrite& write : writes)
			{
				if (!options.memory->write ||
					!options.memory->write(options.memory->context, write.request,
						write.value))
				{
					return fail(ReferenceFailure::MemoryWrite,
						write.request.address);
				}
			}

			*output = input;
			for (const Low32Recurrence& recurrence : plan.recurrences)
			{
				if (recurrence.gpr == 0 || recurrence.gpr >= output->gpr.size() ||
					recurrence.extension == RegionExecution::Low32Extension::None)
				{
					return fail(ReferenceFailure::InvalidDescriptor);
				}
				const u32 value = static_cast<u32>(output->gpr[recurrence.gpr].lo) +
					static_cast<u32>(static_cast<u64>(
						static_cast<u32>(recurrence.delta)) * iterations);
				output->gpr[recurrence.gpr].lo =
					recurrence.extension == RegionExecution::Low32Extension::Sign ?
						static_cast<u64>(static_cast<s64>(static_cast<s32>(value))) :
						value;
			}
			for (const StateOutput& state :
				plan.cop1_streams.front().final_state)
			{
				if (!read_value(state.value) ||
					!WriteCanonicalStateSlot(output, state.state_slot,
						values[state.value]))
				{
					return fail(ReferenceFailure::InvalidDescriptor, 0, state.value);
				}
			}
			output->cycle = input.cycle + repeated + plan.completion_scaled_cycles;
			output->pc = plan.completion_pc;
			output->gpr[0] = {};
			ReferenceResult result{};
			result.iterations = iterations;
			result.memory_operations = static_cast<u32>(read_spans.size() +
				writes.size());
			return result;
		}

		ReferenceResult InterpretBoundedEqualSearch(const Program& program,
			const Plan& plan, const CanonicalState& input, CanonicalState* output,
			const ReferenceOptions& options)
		{
			auto fail = [](ReferenceFailure failure, u32 address = 0,
				ValueId value = INVALID_VALUE) {
				ReferenceResult result{};
				result.failure = failure;
				result.memory_address = address;
				result.value = value;
				return result;
			};
			if (!output || !options.memory || !options.memory->probe ||
				!options.memory->read || plan.kind != Kind::BoundedEqualSearch ||
				plan.bounded_equal_searches.size() != 1 ||
				plan.header_block >= program.blocks.size() ||
				plan.latch_block >= program.blocks.size() ||
				plan.iteration_blocks.empty() ||
				plan.iteration_blocks.front() != plan.header_block)
			{
				return fail(ReferenceFailure::InvalidDescriptor);
			}
			*output = input;
			const BoundedEqualSearch& search = plan.bounded_equal_searches.front();
			if (search.comparison_block >= program.blocks.size() ||
				search.match_target_block >= program.blocks.size() ||
				search.continue_target_block >= program.blocks.size() ||
				search.induction_gpr == 0 ||
				search.induction_gpr >= input.gpr.size() || search.stride == 0 ||
				search.alignment == 0 ||
				(search.alignment & (search.alignment - 1)) != 0 ||
				MemoryAccessWidth(search.load_kind) == 0 ||
				MemoryAlignmentMask(search.load_kind) + 1 > search.alignment)
			{
				return fail(ReferenceFailure::InvalidDescriptor);
			}

			u32 maximum_iterations = 0;
			const u32 counter = plan.control.counter_seed_is_immediate ?
				plan.control.counter_seed_immediate :
				static_cast<u32>(input.gpr[plan.control.counter_gpr].lo);
			const u32 bound = plan.control.bound_is_immediate ?
				plan.control.bound_immediate :
				(plan.control.bound_gpr < input.gpr.size() ?
					static_cast<u32>(input.gpr[plan.control.bound_gpr].lo) : 0);
			if (!RegionMemoryPlan::CalculateTripCount(plan.control, counter, bound,
					&maximum_iterations) || maximum_iterations == 0 ||
				maximum_iterations > options.maximum_iterations)
			{
				return fail(ReferenceFailure::InvalidTripCount);
			}

			const Node* load_node = nullptr;
			std::vector<const Node*> definitions(program.value_count, nullptr);
			for (const Block& block : program.blocks)
			{
				for (const Node& node : block.nodes)
				{
					if (node.id >= definitions.size() || definitions[node.id])
						return fail(ReferenceFailure::InvalidDescriptor, 0, node.id);
					definitions[node.id] = &node;
					if (node.id == search.load_operation)
						load_node = &node;
				}
			}
			if (!load_node || load_node->opcode != Opcode::MemoryLoad ||
				load_node->immediate != static_cast<u32>(search.load_kind) ||
				search.load_value >= definitions.size() ||
				!definitions[search.load_value] ||
				definitions[search.load_value]->opcode != Opcode::MemoryLoadValue ||
				definitions[search.load_value]->operand_count != 1 ||
				definitions[search.load_value]->operands[0] != search.load_operation)
			{
				return fail(ReferenceFailure::InvalidDescriptor, 0,
					search.load_operation);
			}

			auto address_for = [&](u32 iteration, u32* address) {
				if (!address)
					return false;
				const s64 candidate = static_cast<u32>(
					input.gpr[search.induction_gpr].lo) +
					static_cast<s64>(search.load_offset) +
					static_cast<s64>(search.stride) * iteration;
				if (candidate < 0 || candidate > UINT32_MAX)
					return false;
				*address = static_cast<u32>(candidate);
				return true;
			};
			// A future native owner must be transactional even though this class is
			// read-only: prove the complete maximum stream direct and aligned before
			// consuming its first value, so a later handler/TLB page restarts tier zero
			// with no partially observed search.
			for (u32 iteration = 0; iteration < maximum_iterations; iteration++)
			{
				u32 address = 0;
				if (!address_for(iteration, &address) ||
					(address & MemoryAlignmentMask(search.load_kind)) != 0)
				{
					return fail(ReferenceFailure::RangeOverflow, address,
						search.load_operation);
				}
				if (options.memory->probe(options.memory->context,
						{load_node->source_pc, address, search.load_kind}) !=
					MemoryProbeResult::Direct)
				{
					return fail(ReferenceFailure::MemoryProbe, address,
						search.load_operation);
				}
			}

			auto transfer_to = [&](const Block& block,
				u32 target) -> const Transfer* {
				const Transfer* found = nullptr;
				bool ambiguous = false;
				VisitTransfers(block.terminator, [&](const Transfer& transfer) {
					if (transfer.target_block == target)
					{
						ambiguous |= found != nullptr;
						found = &transfer;
					}
				});
				return ambiguous ? nullptr : found;
			};
			auto state_outputs_valid = [&](const std::vector<StateOutput>& states) {
				std::set<u16> slots;
				for (const StateOutput& state : states)
				{
					if (state.state_slot + 1 >= RegionExecution::STATE_SLOT_COUNT ||
						state.value >= program.value_count ||
						!slots.insert(state.state_slot).second)
					{
						return false;
					}
				}
				return true;
			};
			if (!state_outputs_valid(search.backedge_state) ||
				!state_outputs_valid(search.match_state) ||
				!state_outputs_valid(search.exhausted_state))
			{
				return fail(ReferenceFailure::InvalidDescriptor);
			}

			CanonicalState current = input;
			u32 memory_operations = 0;
			for (u32 iteration = 0; iteration < maximum_iterations; iteration++)
			{
				std::vector<u128> values(program.value_count);
				std::vector<bool> defined(program.value_count, false);
				auto read_value = [&](ValueId value, u128* bits) {
					if (!bits || value >= values.size() || !defined[value])
						return false;
					*bits = values[value];
					return true;
				};
				auto assign_header = [&]() {
					const StateMap& parameters =
						program.blocks[plan.header_block].parameters;
					for (size_t slot = 0;
						slot < RegionExecution::STATE_SLOT_COUNT; slot++)
					{
						const ValueId parameter =
							RegionExecution::StateValue(parameters, slot);
						u128 bits{};
						if (parameter >= values.size() ||
							!ReadCanonicalStateSlot(current, slot, &bits))
						{
							return false;
						}
						values[parameter] = bits;
						defined[parameter] = true;
					}
					if (parameters.memory_effect >= values.size())
						return false;
					values[parameters.memory_effect] = {};
					defined[parameters.memory_effect] = true;
					return true;
				};
				auto assign_transfer = [&](const Transfer& transfer,
					u32 target_block) {
					if (target_block >= program.blocks.size())
						return false;
					const StateMap& parameters =
						program.blocks[target_block].parameters;
					for (size_t slot = 0;
						slot < RegionExecution::STATE_SLOT_COUNT; slot++)
					{
						const ValueId source =
							RegionExecution::StateValue(transfer.state, slot);
						const ValueId parameter =
							RegionExecution::StateValue(parameters, slot);
						u128 bits{};
						if (parameter >= values.size() || !read_value(source, &bits))
							return false;
						values[parameter] = bits;
						defined[parameter] = true;
					}
					u128 memory{};
					if (parameters.memory_effect >= values.size() ||
						!read_value(transfer.state.memory_effect, &memory))
					{
						return false;
					}
					values[parameters.memory_effect] = memory;
					defined[parameters.memory_effect] = true;
					return true;
				};
				auto apply_state = [&](const std::vector<StateOutput>& states,
					CanonicalState* state) {
					if (!state)
						return false;
					CanonicalState next = *state;
					for (const StateOutput& publication : states)
					{
						u128 bits{};
						if (!read_value(publication.value, &bits) ||
							!WriteCanonicalStateSlot(&next, publication.state_slot, bits))
						{
							return false;
						}
					}
					next.gpr[0] = {};
					*state = next;
					return true;
				};
				if (!assign_header())
					return fail(ReferenceFailure::InvalidDescriptor);

				for (size_t path_index = 0;
					path_index < plan.iteration_blocks.size(); path_index++)
				{
					const u32 block_index = plan.iteration_blocks[path_index];
					if (block_index >= program.blocks.size())
						return fail(ReferenceFailure::InvalidDescriptor);
					const Block& block = program.blocks[block_index];
					for (const Node& node : block.nodes)
					{
						if (node.opcode == Opcode::Parameter)
							continue;
						if (block.terminator.kind == TerminatorKind::Branch &&
							block.terminator.likely &&
							node.source_pc == block.terminator.delay_slot_pc)
						{
							u128 condition{};
							if (!read_value(block.terminator.condition, &condition))
								return fail(ReferenceFailure::InvalidDescriptor, 0,
									block.terminator.condition);
							if (condition.lo == 0)
								continue;
						}
						u128 operand[3]{};
						for (u8 index = 0; index < node.operand_count; index++)
						{
							if (!read_value(node.operands[index], &operand[index]))
								return fail(ReferenceFailure::InvalidDescriptor, 0,
									node.operands[index]);
						}
						u128 bits{};
						const u64 left = operand[0].lo;
						const u64 right = operand[1].lo;
						const u64 third = operand[2].lo;
						switch (node.opcode)
						{
							case Opcode::ConstantI1:
							case Opcode::ConstantI32:
							case Opcode::ConstantI64:
							case Opcode::ConstantAddress:
								bits = ReferenceBits(node.literal);
								break;
							case Opcode::NoEffect:
								break;
							case Opcode::ExtractLow32:
								bits = ReferenceBits(static_cast<u32>(left));
								break;
							case Opcode::ExtractLow64:
								bits = ReferenceBits(left);
								break;
							case Opcode::ExtractHigh64:
								bits = ReferenceBits(operand[0].hi);
								break;
							case Opcode::ReplaceLow64:
								bits = operand[0];
								bits.lo = right;
								break;
							case Opcode::ReplaceHigh64:
								bits = operand[0];
								bits.hi = right;
								break;
							case Opcode::SignExtend32To64:
								bits = ReferenceBits(static_cast<u64>(static_cast<s64>(
									static_cast<s32>(static_cast<u32>(left)))));
								break;
							case Opcode::ZeroExtend32To64:
							case Opcode::Truncate64To32:
								bits = ReferenceBits(static_cast<u32>(left));
								break;
							case Opcode::Add32:
								bits = ReferenceBits(static_cast<u32>(left) +
									static_cast<u32>(right));
								break;
							case Opcode::Add64:
								bits = ReferenceBits(left + right);
								break;
							case Opcode::Sub32:
								bits = ReferenceBits(static_cast<u32>(left) -
									static_cast<u32>(right));
								break;
							case Opcode::Sub64:
								bits = ReferenceBits(left - right);
								break;
							case Opcode::And32:
								bits = ReferenceBits(static_cast<u32>(left) &
									static_cast<u32>(right));
								break;
							case Opcode::And64:
								bits = ReferenceBits(left & right);
								break;
							case Opcode::Or64:
								bits = ReferenceBits(left | right);
								break;
							case Opcode::Xor32:
								bits = ReferenceBits(static_cast<u32>(left) ^
									static_cast<u32>(right));
								break;
							case Opcode::Xor64:
								bits = ReferenceBits(left ^ right);
								break;
							case Opcode::Nor64:
								bits = ReferenceBits(~(left | right));
								break;
							case Opcode::ShiftLeft32:
								bits = ReferenceBits(static_cast<u32>(left) << node.immediate);
								break;
							case Opcode::ShiftRightLogical32:
								bits = ReferenceBits(static_cast<u32>(left) >> node.immediate);
								break;
							case Opcode::ShiftRightArithmetic32:
								bits = ReferenceBits(static_cast<u32>(static_cast<s32>(
									static_cast<u32>(left)) >> node.immediate));
								break;
							case Opcode::ShiftLeft64:
								bits = ReferenceBits(left << node.immediate);
								break;
							case Opcode::ShiftRightLogical64:
								bits = ReferenceBits(left >> node.immediate);
								break;
							case Opcode::ShiftRightArithmetic64:
								bits = ReferenceBits(std::bit_cast<u64>(
									std::bit_cast<s64>(left) >> node.immediate));
								break;
							case Opcode::ShiftLeft32Variable:
								bits = ReferenceBits(static_cast<u32>(left) <<
									(static_cast<u32>(right) & 31u));
								break;
							case Opcode::ShiftRightLogical32Variable:
								bits = ReferenceBits(static_cast<u32>(left) >>
									(static_cast<u32>(right) & 31u));
								break;
							case Opcode::ShiftRightArithmetic32Variable:
								bits = ReferenceBits(static_cast<u32>(static_cast<s32>(
									static_cast<u32>(left)) >>
									(static_cast<u32>(right) & 31u)));
								break;
							case Opcode::ShiftLeft64Variable:
								bits = ReferenceBits(left << (static_cast<u32>(right) & 63u));
								break;
							case Opcode::ShiftRightLogical64Variable:
								bits = ReferenceBits(left >> (static_cast<u32>(right) & 63u));
								break;
							case Opcode::ShiftRightArithmetic64Variable:
								bits = ReferenceBits(std::bit_cast<u64>(
									std::bit_cast<s64>(left) >>
									(static_cast<u32>(right) & 63u)));
								break;
							case Opcode::Select64:
								bits = ReferenceBits(left != 0 ? right : third);
								break;
							case Opcode::CompareEqual64:
								bits = ReferenceBits(left == right);
								break;
							case Opcode::CompareNotEqual64:
								bits = ReferenceBits(left != right);
								break;
							case Opcode::CompareSignedLess64:
								bits = ReferenceBits(std::bit_cast<s64>(left) <
									std::bit_cast<s64>(right));
								break;
							case Opcode::CompareUnsignedLess64:
								bits = ReferenceBits(left < right);
								break;
							case Opcode::CompareSignedLessEqualZero64:
								bits = ReferenceBits(std::bit_cast<s64>(left) <= 0);
								break;
							case Opcode::CompareSignedGreaterZero64:
								bits = ReferenceBits(std::bit_cast<s64>(left) > 0);
								break;
							case Opcode::CompareSignedLessZero64:
								bits = ReferenceBits(std::bit_cast<s64>(left) < 0);
								break;
							case Opcode::CompareSignedGreaterEqualZero64:
								bits = ReferenceBits(std::bit_cast<s64>(left) >= 0);
								break;
							case Opcode::AddressFromI32:
								bits = ReferenceBits(static_cast<u32>(left));
								break;
							case Opcode::EffectiveAddress32:
								bits = ReferenceBits(static_cast<u32>(left) +
									static_cast<u32>(right));
								break;
							case Opcode::MemoryLoad:
							{
								const u32 address = static_cast<u32>(right);
								if (node.id != search.load_operation ||
									(address & MemoryAlignmentMask(search.load_kind)) != 0)
								{
									return fail(ReferenceFailure::InvalidDescriptor, address,
										node.id);
								}
								u128 raw{};
								if (!options.memory->read(options.memory->context,
										{node.source_pc, address, search.load_kind}, &raw))
								{
									return fail(ReferenceFailure::MemoryRead, address, node.id);
								}
								bits = operand[2];
								switch (search.load_kind)
								{
									case MemoryAccessKind::LoadS8:
										bits.lo = static_cast<u64>(static_cast<s64>(
											static_cast<s8>(raw.lo)));
										break;
									case MemoryAccessKind::LoadU8:
										bits.lo = static_cast<u8>(raw.lo);
										break;
									case MemoryAccessKind::LoadS16:
										bits.lo = static_cast<u64>(static_cast<s64>(
											static_cast<s16>(raw.lo)));
										break;
									case MemoryAccessKind::LoadU16:
										bits.lo = static_cast<u16>(raw.lo);
										break;
									case MemoryAccessKind::LoadS32:
										bits.lo = static_cast<u64>(static_cast<s64>(
											static_cast<s32>(raw.lo)));
										break;
									case MemoryAccessKind::LoadU32:
										bits.lo = static_cast<u32>(raw.lo);
										break;
									case MemoryAccessKind::Load64:
										bits.lo = raw.lo;
										break;
									default:
										return fail(ReferenceFailure::InvalidDescriptor,
											address, node.id);
								}
								memory_operations++;
								break;
							}
							case Opcode::MemoryLoadValue:
							case Opcode::BindGpr:
								bits = operand[0];
								break;
							case Opcode::AdvanceCycles:
								bits = ReferenceBits(left + node.immediate);
								break;
							default:
								return fail(ReferenceFailure::InvalidDescriptor, 0,
									node.id);
						}
						if (node.id >= values.size())
							return fail(ReferenceFailure::InvalidDescriptor, 0, node.id);
						values[node.id] = bits;
						defined[node.id] = true;
					}

					if (block_index == search.comparison_block)
					{
						u128 condition{};
						if (!read_value(block.terminator.condition, &condition))
							return fail(ReferenceFailure::InvalidDescriptor, 0,
								block.terminator.condition);
						const bool matched = (condition.lo != 0) ==
							search.condition_true_is_match;
						if (matched)
						{
							if (!apply_state(search.match_state, &current))
								return fail(ReferenceFailure::InvalidDescriptor);
							const u64 prior = static_cast<u64>(iteration) *
								plan.repeated_scaled_cycles;
							if (prior > UINT64_MAX - search.match_scaled_cycles ||
								input.cycle > UINT64_MAX - prior -
									search.match_scaled_cycles)
							{
								return fail(ReferenceFailure::CycleOverflow);
							}
							current.cycle = input.cycle + prior +
								search.match_scaled_cycles;
							current.pc = program.blocks[search.match_target_block].pc;
							*output = current;
							ReferenceResult result{};
							result.iterations = iteration + 1;
							result.memory_operations = memory_operations;
							result.exit_block = search.match_target_block;
							result.matched = true;
							return result;
						}
					}

					if (block_index == plan.latch_block)
					{
						u128 condition{};
						if (!read_value(block.terminator.condition, &condition))
							return fail(ReferenceFailure::InvalidDescriptor, 0,
								block.terminator.condition);
						const Transfer* selected = condition.lo != 0 ?
							&block.terminator.taken : &block.terminator.not_taken;
						const u32 expected = iteration + 1 < maximum_iterations ?
							plan.header_block : plan.completion_block;
						if (selected->target_block != expected)
							return fail(ReferenceFailure::InvalidTripCount, 0,
								block.terminator.condition);
						if (iteration + 1 < maximum_iterations)
						{
							if (!apply_state(search.backedge_state, &current))
								return fail(ReferenceFailure::InvalidDescriptor);
							const u64 cycles = static_cast<u64>(iteration + 1) *
								plan.repeated_scaled_cycles;
							if (input.cycle > UINT64_MAX - cycles)
								return fail(ReferenceFailure::CycleOverflow);
							current.cycle = input.cycle + cycles;
							current.pc = program.blocks[plan.header_block].pc;
							break;
						}
						if (!apply_state(search.exhausted_state, &current))
							return fail(ReferenceFailure::InvalidDescriptor);
						const u64 prior = static_cast<u64>(iteration) *
							plan.repeated_scaled_cycles;
						if (prior > UINT64_MAX - plan.completion_scaled_cycles ||
							input.cycle > UINT64_MAX - prior -
								plan.completion_scaled_cycles)
						{
							return fail(ReferenceFailure::CycleOverflow);
						}
						current.cycle = input.cycle + prior +
							plan.completion_scaled_cycles;
						current.pc = plan.completion_pc;
						*output = current;
						ReferenceResult result{};
						result.iterations = maximum_iterations;
						result.memory_operations = memory_operations;
						result.exit_block = plan.completion_block;
						return result;
					}

					if (path_index + 1 >= plan.iteration_blocks.size())
						return fail(ReferenceFailure::InvalidDescriptor);
					const u32 next = plan.iteration_blocks[path_index + 1];
					const Transfer* selected = nullptr;
					if (block.terminator.kind == TerminatorKind::Branch)
					{
						u128 condition{};
						if (!read_value(block.terminator.condition, &condition))
							return fail(ReferenceFailure::InvalidDescriptor, 0,
								block.terminator.condition);
						selected = condition.lo != 0 ? &block.terminator.taken :
							&block.terminator.not_taken;
					}
					else
					{
						selected = transfer_to(block, next);
					}
					if (!selected || selected->target_block != next ||
						!assign_transfer(*selected, next))
					{
						return fail(ReferenceFailure::InvalidDescriptor);
					}
				}
			}
			return fail(ReferenceFailure::InvalidTripCount);
		}
	} // namespace

	ReferenceResult Interpret(const RegionIR::Program& program, const Plan& plan,
		const RegionIR::CanonicalState& input, RegionIR::CanonicalState* output,
		const ReferenceOptions& options)
	{
		auto fail = [](ReferenceFailure failure, u32 address = 0,
			ValueId value = INVALID_VALUE) {
			ReferenceResult result{};
			result.failure = failure;
			result.memory_address = address;
			result.value = value;
			return result;
		};
		if (!output || !options.memory || !options.memory->probe ||
			plan.kind == Kind::None || !plan.complete_preflight ||
			!plan.exact_event_phase || plan.header_block >= program.blocks.size() ||
			plan.latch_block >= program.blocks.size() || plan.completion_pc == 0 ||
			(plan.completion_block != INVALID_BLOCK &&
			 plan.completion_block >= program.blocks.size()))
		{
			return fail(ReferenceFailure::InvalidDescriptor);
		}
		// Rebuild from source-attested IR so a mutated descriptor cannot become a
		// second, unchecked semantic authority in validation or future emission.
		const BuildResult rebuilt = Build(program);
		if (!rebuilt || !(rebuilt.plan == plan))
		{
			return fail(ReferenceFailure::InvalidDescriptor);
		}
		if (plan.kind == Kind::Cop1Stream)
			return InterpretCop1Stream(program, plan, input, output, options);
		if (plan.kind == Kind::BoundedEqualSearch)
			return InterpretBoundedEqualSearch(program, plan, input, output, options);
		*output = input;

		u32 iterations = 0;
		const u32 counter = static_cast<u32>(input.gpr[plan.control.counter_gpr].lo);
		const u32 bound = plan.control.bound_gpr < input.gpr.size() ?
			static_cast<u32>(input.gpr[plan.control.bound_gpr].lo) : 0;
		if (!RegionMemoryPlan::CalculateTripCount(plan.control, counter, bound,
				&iterations) || iterations == 0 || iterations > options.maximum_iterations)
		{
			return fail(ReferenceFailure::InvalidTripCount);
		}
		for (const Low32Recurrence& recurrence : plan.recurrences)
		{
			if (!recurrence.signed_overflow_guard)
				continue;
			if (recurrence.gpr == 0 || recurrence.gpr >= input.gpr.size())
				return fail(ReferenceFailure::InvalidDescriptor);
			s64 current = static_cast<s32>(static_cast<u32>(input.gpr[recurrence.gpr].lo));
			for (u32 iteration = 0; iteration < iterations; iteration++)
			{
				const s64 next = current + recurrence.delta;
				if (next < INT32_MIN || next > INT32_MAX)
					return fail(ReferenceFailure::ArithmeticOverflow);
				current = next;
			}
		}

		auto address_for = [&](u8 gpr, s32 entry_offset, s32 operation_offset,
			u32 stride, u32 iteration, u32* address) {
			if (gpr == 0 || gpr >= input.gpr.size() || !address)
				return false;
			const s64 value = static_cast<s64>(static_cast<u32>(input.gpr[gpr].lo)) +
				static_cast<s64>(entry_offset) + static_cast<s64>(operation_offset) +
				static_cast<s64>(stride) * iteration;
			if (value < 0 || value > UINT32_MAX)
				return false;
			*address = static_cast<u32>(value);
			return true;
		};
		auto probe = [&](u32 pc, u32 address, MemoryAccessKind kind) {
			return options.memory->probe(options.memory->context,
				{pc, address, kind}) == MemoryProbeResult::Direct;
		};

		std::map<ValueId, u128> invariant_values;
		for (const PatternStream& stream : plan.pattern_streams)
		{
			for (const PatternFragment& fragment : stream.fragments)
			{
				if (invariant_values.find(fragment.value) != invariant_values.end())
					continue;
				u128 bits{};
				if (!options.values.read ||
					!options.values.read(options.values.context, fragment.value, &bits))
				{
					return fail(ReferenceFailure::MissingInvariantValue, 0,
						fragment.value);
				}
				invariant_values.emplace(fragment.value, bits);
			}
		}
		for (const Vu0AffineStream& stream : plan.vu0_affine_streams)
		{
			if (stream.idle_vi >= input.vu0_vi.size() ||
				stream.status_vi >= input.vu0_vi.size() ||
				stream.idle_vi == stream.status_vi)
				return fail(ReferenceFailure::InvalidDescriptor);
			if ((input.vu0_vi[stream.idle_vi] & 1u) != 0)
				return fail(ReferenceFailure::Vu0Busy);
		}

		struct Span
		{
			u32 begin = 0;
			u32 end = 0;
		};
		std::vector<Span> write_spans;
		for (const PatternStream& stream : plan.pattern_streams)
		{
			u32 begin = 0;
			u32 last = 0;
			if (stream.fragments.empty() ||
				!address_for(stream.entry_induction_gpr,
					stream.entry_induction_offset, stream.fragments.front().offset,
					stream.stride, 0, &begin) ||
				!address_for(stream.entry_induction_gpr,
					stream.entry_induction_offset, stream.fragments.back().offset,
					stream.stride, iterations - 1, &last) ||
				last > UINT32_MAX - stream.fragments.back().width)
			{
				return fail(ReferenceFailure::RangeOverflow);
			}
			write_spans.push_back({begin,
				last + stream.fragments.back().width});
		}
		if (plan.requires_disjoint_write_streams)
		{
			for (size_t left = 0; left < write_spans.size(); left++)
			{
				for (size_t right = left + 1; right < write_spans.size(); right++)
				{
					if (write_spans[left].begin < write_spans[right].end &&
						write_spans[right].begin < write_spans[left].end)
					{
						return fail(ReferenceFailure::AliasGuard);
					}
				}
			}
		}

		// All fallible address/mapping/source-ownership checks precede the first
		// write, matching the future native kernel's transactional entry contract.
		for (u32 iteration = 0; iteration < iterations; iteration++)
		{
			for (const PatternStream& stream : plan.pattern_streams)
			{
				for (const PatternFragment& fragment : stream.fragments)
				{
					u32 address = 0;
					if (!address_for(stream.entry_induction_gpr,
							stream.entry_induction_offset, fragment.offset,
							stream.stride, iteration, &address))
						return fail(ReferenceFailure::RangeOverflow);
					if (!probe(fragment.source_pc, address, fragment.kind))
						return fail(ReferenceFailure::MemoryProbe, address);
				}
			}
			for (const CopyStream& stream : plan.copy_streams)
			{
				for (const CopyFragment& fragment : stream.fragments)
				{
					u32 source = 0;
					u32 destination = 0;
					if (!address_for(stream.source_entry_induction_gpr,
							stream.source_entry_induction_offset, fragment.source_offset,
							stream.stride, iteration, &source) ||
						!address_for(stream.destination_entry_induction_gpr,
							stream.destination_entry_induction_offset,
							fragment.destination_offset, stream.stride, iteration,
							&destination))
						return fail(ReferenceFailure::RangeOverflow);
					if (!probe(fragment.load_pc, source, fragment.load_kind))
						return fail(ReferenceFailure::MemoryProbe, source);
					if (!probe(fragment.store_pc, destination, fragment.store_kind))
						return fail(ReferenceFailure::MemoryProbe, destination);
				}
			}
			for (const Vu0AffineStream& stream : plan.vu0_affine_streams)
			{
				u32 source = 0;
				u32 destination = 0;
				if (!address_for(stream.source_entry_induction_gpr,
						stream.source_entry_induction_offset, stream.source_offset,
						stream.stride, iteration, &source) ||
					!address_for(stream.destination_entry_induction_gpr,
						stream.destination_entry_induction_offset,
						stream.destination_offset, stream.stride, iteration,
						&destination))
				{
					return fail(ReferenceFailure::RangeOverflow);
				}
				if (!probe(stream.load_pc, source,
						MemoryAccessKind::LoadVu0Vector))
					return fail(ReferenceFailure::MemoryProbe, source);
				if (!probe(stream.store_pc, destination,
						MemoryAccessKind::StoreVu0Vector))
					return fail(ReferenceFailure::MemoryProbe, destination);
			}
		}

		ReferenceResult result{};
		result.iterations = iterations;
		std::map<ValueId, u128> final_load_bits;
		struct Vu0FinalState
		{
			bool valid = false;
			u128 input{};
			u128 output{};
			u128 acc{};
			u32 mac = 0;
			u32 status = 0;
			u32 vi_status = 0;
		};
		std::vector<Vu0FinalState> vu0_final(plan.vu0_affine_streams.size());
		for (size_t stream_index = 0;
			stream_index < plan.vu0_affine_streams.size(); stream_index++)
		{
			const Vu0AffineStream& stream = plan.vu0_affine_streams[stream_index];
			vu0_final[stream_index].vi_status = input.vu0_vi[stream.status_vi];
		}
		for (u32 iteration = 0; iteration < iterations; iteration++)
		{
			for (const PatternStream& stream : plan.pattern_streams)
			{
				for (const PatternFragment& fragment : stream.fragments)
				{
					u32 address = 0;
					if (!address_for(stream.entry_induction_gpr,
							stream.entry_induction_offset, fragment.offset,
							stream.stride, iteration, &address))
						return fail(ReferenceFailure::RangeOverflow);
					if (!options.memory->write ||
						!options.memory->write(options.memory->context,
							{fragment.source_pc, address, fragment.kind},
							invariant_values[fragment.value]))
						return fail(ReferenceFailure::MemoryWrite, address);
					result.memory_operations++;
				}
			}
			for (const CopyStream& stream : plan.copy_streams)
			{
				for (const CopyFragment& fragment : stream.fragments)
				{
					u32 source = 0;
					u32 destination = 0;
					if (!address_for(stream.source_entry_induction_gpr,
							stream.source_entry_induction_offset, fragment.source_offset,
							stream.stride, iteration, &source) ||
						!address_for(stream.destination_entry_induction_gpr,
							stream.destination_entry_induction_offset,
							fragment.destination_offset, stream.stride, iteration,
							&destination))
						return fail(ReferenceFailure::RangeOverflow);
					u128 bits{};
					if (!options.memory->read ||
						!options.memory->read(options.memory->context,
							{fragment.load_pc, source, fragment.load_kind}, &bits))
						return fail(ReferenceFailure::MemoryRead, source);
					if (!options.memory->write ||
						!options.memory->write(options.memory->context,
							{fragment.store_pc, destination, fragment.store_kind}, bits))
						return fail(ReferenceFailure::MemoryWrite, destination);
					final_load_bits[fragment.load_operation] = bits;
					result.memory_operations += 2;
				}
			}
			for (size_t stream_index = 0;
				stream_index < plan.vu0_affine_streams.size(); stream_index++)
			{
				const Vu0AffineStream& stream = plan.vu0_affine_streams[stream_index];
				Vu0FinalState& final = vu0_final[stream_index];
				u32 source = 0;
				u32 destination = 0;
				if (!address_for(stream.source_entry_induction_gpr,
						stream.source_entry_induction_offset, stream.source_offset,
						stream.stride, iteration, &source) ||
					!address_for(stream.destination_entry_induction_gpr,
						stream.destination_entry_induction_offset,
						stream.destination_offset, stream.stride, iteration,
						&destination))
				{
					return fail(ReferenceFailure::RangeOverflow);
				}
				u128 input_vector{};
				if (!options.memory->read ||
					!options.memory->read(options.memory->context,
						{stream.load_pc, source, MemoryAccessKind::LoadVu0Vector},
						&input_vector))
				{
					return fail(ReferenceFailure::MemoryRead, source);
				}

				const u128 normalized_input = NormalizeVu0Vector(input_vector,
					program.options.vu0_overflow_clamp);
				u128 accumulator{};
				u128 output_vector{};
				for (u32 lane = 0; lane < 4; lane++)
				{
					if (stream.matrix_vf[lane] >= input.vu0_vf.size())
						return fail(ReferenceFailure::InvalidDescriptor);
					const u128 matrix = NormalizeVu0Vector(
						input.vu0_vf[stream.matrix_vf[lane]],
						program.options.vu0_overflow_clamp);
					const u128 component = BroadcastVu0Lane(normalized_input, lane);
					const u128 product = EvaluateVu0RawBinary(Opcode::Vu0MulRaw,
						matrix, component);
					const u128 raw = lane == 0 ? product : EvaluateVu0RawBinary(
						Opcode::Vu0AddRaw,
						NormalizeVu0Vector(accumulator,
							program.options.vu0_overflow_clamp), product);
					const u128 clamped = ClampVu0FmacResult(raw, 0x0fu,
						program.options.vu0_overflow_clamp);
					final.mac = EvaluateVu0MacFlags(raw, 0x0fu);
					final.status = EvaluateVu0StatusFlags(final.mac);
					final.vi_status = SyncVu0StatusControl(final.vi_status,
						final.status);
					if (lane < 3)
						accumulator = clamped;
					else
						output_vector = clamped;
				}
				if (!options.memory->write ||
					!options.memory->write(options.memory->context,
						{stream.store_pc, destination,
							MemoryAccessKind::StoreVu0Vector}, output_vector))
				{
					return fail(ReferenceFailure::MemoryWrite, destination);
				}
				final.valid = true;
				final.input = input_vector;
				final.output = output_vector;
				final.acc = accumulator;
				result.memory_operations += 2;
			}
		}

		*output = input;
		for (const Low32Recurrence& recurrence : plan.recurrences)
		{
			if (recurrence.gpr == 0 || recurrence.gpr >= output->gpr.size() ||
				recurrence.extension == RegionExecution::Low32Extension::None)
				return fail(ReferenceFailure::InvalidDescriptor);
			const u32 value = static_cast<u32>(output->gpr[recurrence.gpr].lo) +
				static_cast<u32>(static_cast<u64>(static_cast<u32>(recurrence.delta)) *
					iterations);
			output->gpr[recurrence.gpr].lo =
				recurrence.extension == RegionExecution::Low32Extension::Sign ?
					static_cast<u64>(static_cast<s64>(static_cast<s32>(value))) : value;
		}
		for (const FinalLoadState& load : plan.final_load_state)
		{
			const auto found = final_load_bits.find(load.load_operation);
			if (load.gpr == 0 || load.gpr >= output->gpr.size() ||
				found == final_load_bits.end())
				return fail(ReferenceFailure::InvalidDescriptor);
			const u128 raw = found->second;
			u128& target = output->gpr[load.gpr];
			switch (load.kind)
			{
				case MemoryAccessKind::LoadS8:
					target.lo = static_cast<u64>(static_cast<s64>(static_cast<s8>(raw.lo)));
					break;
				case MemoryAccessKind::LoadU8:
					target.lo = static_cast<u8>(raw.lo);
					break;
				case MemoryAccessKind::LoadS16:
					target.lo = static_cast<u64>(static_cast<s64>(static_cast<s16>(raw.lo)));
					break;
				case MemoryAccessKind::LoadU16:
					target.lo = static_cast<u16>(raw.lo);
					break;
				case MemoryAccessKind::LoadS32:
					target.lo = static_cast<u64>(static_cast<s64>(static_cast<s32>(raw.lo)));
					break;
				case MemoryAccessKind::LoadU32:
					target.lo = static_cast<u32>(raw.lo);
					break;
				case MemoryAccessKind::Load64:
					target.lo = raw.lo;
					break;
				case MemoryAccessKind::Load128:
					target = raw;
					break;
				default:
					return fail(ReferenceFailure::InvalidDescriptor);
			}
		}
		for (size_t stream_index = 0;
			stream_index < plan.vu0_affine_streams.size(); stream_index++)
		{
			const Vu0AffineStream& stream = plan.vu0_affine_streams[stream_index];
			const Vu0FinalState& final = vu0_final[stream_index];
			if (!final.valid || stream.input_vf >= output->vu0_vf.size() ||
				stream.output_vf >= output->vu0_vf.size() ||
				stream.mac_vi >= output->vu0_vi.size() ||
				stream.status_vi >= output->vu0_vi.size())
			{
				return fail(ReferenceFailure::InvalidDescriptor);
			}
			if (stream.input_vf != stream.output_vf)
				output->vu0_vf[stream.input_vf] = final.input;
			output->vu0_vf[stream.output_vf] = final.output;
			output->vu0_acc = final.acc;
			output->vu0_macflag = final.mac;
			output->vu0_statusflag = final.status;
			output->vu0_vi[stream.mac_vi] = final.mac;
			output->vu0_vi[stream.status_vi] = final.vi_status;
		}
		const u64 repeated = static_cast<u64>(iterations - 1) *
			plan.repeated_scaled_cycles;
		if (repeated > UINT64_MAX - plan.completion_scaled_cycles ||
			input.cycle > UINT64_MAX - repeated - plan.completion_scaled_cycles)
			return fail(ReferenceFailure::CycleOverflow);
		output->cycle = input.cycle + repeated + plan.completion_scaled_cycles;
		output->pc = plan.completion_pc;
		output->gpr[0] = {};
		return result;
	}

	bool ForwardCopyBatchAliasSafe(u32 source_begin, u32 destination_begin,
		u32 byte_count)
	{
		if (byte_count == 0 || source_begin > UINT32_MAX - byte_count ||
			destination_begin > UINT32_MAX - byte_count)
		{
			return false;
		}
		return destination_begin <= source_begin ||
			destination_begin >= source_begin + byte_count;
	}
} // namespace VitaEE::SemanticKernel
