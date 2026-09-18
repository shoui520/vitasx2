// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#include "PrecompiledHeader.h"

#include "Memory.h"
#include "pcsx2/vita/VitaEeExecutor.h"
#include "pcsx2/vita/VitaEeRegionMemory.h"

#include <algorithm>
#include <cstring>
#include <optional>

namespace VitaEE::RegionIR
{
	namespace
	{
		constexpr u32 SOURCE_PAGE_SHIFT = BlockExecutor::RAM_SOURCE_PAGE_SHIFT;
		constexpr u32 SOURCE_CHUNK_SHIFT = BlockExecutor::RAM_SOURCE_CHUNK_SHIFT;
		constexpr u32 SOURCE_PAGE_COUNT = BlockExecutor::RAM_SOURCE_PAGE_COUNT;
		constexpr u32 SOURCE_CHUNK_COUNT = BlockExecutor::RAM_SOURCE_CHUNK_COUNT;
		static_assert(SOURCE_PAGE_SHIFT == vtlb_private::VTLB_PAGE_BITS);
		static_assert(SOURCE_CHUNK_SHIFT == 6);

		u32 AccessSize(MemoryAccessKind kind)
		{
			return MemoryAccessWidth(kind);
		}

		bool IsStore(MemoryAccessKind kind)
		{
			return kind >= MemoryAccessKind::Store8;
		}

		bool ResolveMainRam(const MemoryRequest& request, uptr* host,
			u32* backing_offset)
		{
			if (!eeMem || !vtlb_private::vtlbdata.vmap)
				return false;
			const u32 size = AccessSize(request.kind);
			if (size == 0 || size > vtlb_private::VTLB_PAGE_SIZE -
										(request.address & vtlb_private::VTLB_PAGE_MASK))
			{
				return false;
			}

			const vtlb_private::VTLBVirtual mapping =
				vtlb_private::vtlbdata.vmap[request.address >> vtlb_private::VTLB_PAGE_BITS];
			if (mapping.isHandler(request.address))
				return false;
			const uptr resolved = mapping.assumePtr(request.address);
			const uptr ram_begin = reinterpret_cast<uptr>(eeMem->Main);
			const uptr ram_end = ram_begin +
			                     std::min(Ps2MemSize::ExposedRam, Ps2MemSize::MainRam);
			if (resolved < ram_begin || resolved >= ram_end ||
				static_cast<uptr>(size) > ram_end - resolved)
			{
				return false;
			}
			*host = resolved;
			*backing_offset = static_cast<u32>(resolved - ram_begin);
			return true;
		}

		bool DirectRead(void* opaque, const MemoryRequest& request, u128* value)
		{
			if (!value || ProbeVtlbMemory(opaque, request) !=
							  MemoryProbeResult::Direct)
			{
				return false;
			}
			uptr host = 0;
			u32 backing_offset = 0;
			if (!ResolveMainRam(request, &host, &backing_offset))
				return false;
			*value = {};
			std::memcpy(value, reinterpret_cast<const void*>(host),
				AccessSize(request.kind));
			return true;
		}

		bool DirectWrite(void* opaque, const MemoryRequest& request,
			const u128& value)
		{
			if (ProbeVtlbMemory(opaque, request) != MemoryProbeResult::Direct)
				return false;
			uptr host = 0;
			u32 backing_offset = 0;
			if (!ResolveMainRam(request, &host, &backing_offset))
				return false;
			std::memcpy(reinterpret_cast<void*>(host), &value,
				AccessSize(request.kind));
			return true;
		}
	} // namespace

	VtlbMemoryContext MakeVtlbMemoryContext(const VitaEE::BlockExecutor& executor)
	{
		return {executor.RamSourcePageLiveFlags(),
			executor.RamSourceChunkLiveBits()};
	}

	RegionMemoryInterface MakeVtlbMemoryInterface(VtlbMemoryContext* context)
	{
		return {context, ProbeVtlbMemory, DirectRead, DirectWrite};
	}

	MemoryProbeResult ProbeVtlbMemory(void* opaque,
		const MemoryRequest& request)
	{
		VtlbMemoryContext* const context =
			static_cast<VtlbMemoryContext*>(opaque);
		if (!context || !eeMem || !vtlb_private::vtlbdata.vmap)
			return MemoryProbeResult::Translation;
		const u32 size = AccessSize(request.kind);
		if (size == 0 || size > vtlb_private::VTLB_PAGE_SIZE -
									(request.address & vtlb_private::VTLB_PAGE_MASK))
		{
			return MemoryProbeResult::Translation;
		}

		const vtlb_private::VTLBVirtual mapping =
			vtlb_private::vtlbdata.vmap[request.address >> vtlb_private::VTLB_PAGE_BITS];
		if (mapping.isHandler(request.address))
			return MemoryProbeResult::Handler;

		uptr host = 0;
		u32 backing_offset = 0;
		if (!ResolveMainRam(request, &host, &backing_offset))
			return MemoryProbeResult::Translation;
		if (!IsStore(request.kind))
			return MemoryProbeResult::Direct;

		// A store without the authoritative maps is not proven safe.
		if (!context->ram_source_page_live_flags ||
			!context->ram_source_chunk_live_bits)
		{
			return MemoryProbeResult::SelfModifyingCode;
		}
		const u32 end = backing_offset + size;
		const u32 first_page = backing_offset >> SOURCE_PAGE_SHIFT;
		const u32 last_page = (end - 1) >> SOURCE_PAGE_SHIFT;
		for (u32 page = first_page; page <= last_page; page++)
		{
			if (page >= SOURCE_PAGE_COUNT ||
				context->ram_source_page_live_flags[page] == 0)
			{
				continue;
			}
			const u32 first_chunk =
				std::max(backing_offset, page << SOURCE_PAGE_SHIFT) >>
				SOURCE_CHUNK_SHIFT;
			const u32 last_chunk =
				(std::min(end, (page + 1) << SOURCE_PAGE_SHIFT) - 1) >>
				SOURCE_CHUNK_SHIFT;
			for (u32 chunk = first_chunk;
				 chunk <= last_chunk && chunk < SOURCE_CHUNK_COUNT; chunk++)
			{
				if ((context->ram_source_chunk_live_bits[chunk >> 3] &
						static_cast<u8>(1u << (chunk & 7))) != 0)
				{
					return MemoryProbeResult::SelfModifyingCode;
				}
			}
		}
		return MemoryProbeResult::Direct;
	}
} // namespace VitaEE::RegionIR

namespace VitaEE::RegionMemoryPlan
{
	using namespace RegionIR;

	bool CalculateTripCount(const LoopControl& control, u32 counter_seed,
		u32 bound, u32* trip_count)
	{
		if (!trip_count || !control.valid || control.counter_gpr == 0 ||
			control.counter_stride == 0)
		{
			return false;
		}
		const u32 seed = control.counter_seed_is_immediate ?
			control.counter_seed_immediate : counter_seed;
		const u32 endpoint = control.bound_is_immediate ?
			control.bound_immediate : bound;
		const bool increasing =
			control.termination == TerminationKind::UnsignedLess ||
			control.termination == TerminationKind::EqualEndpoint;
		const u64 step = increasing ?
			static_cast<u32>(control.counter_stride) :
			static_cast<u64>(-static_cast<s64>(control.counter_stride));
		if (step == 0 || step > 4096 || (step & (step - 1)) != 0)
			return false;

		u64 iterations = 0;
		if (increasing)
		{
			if (control.counter_stride <= 0 ||
				(control.signed_counter_compare &&
				 ((seed | endpoint) & 0x80000000u) != 0) || seed >= endpoint)
			{
				return false;
			}
			const u64 distance = static_cast<u64>(endpoint) - seed;
			if ((distance & (step - 1)) != 0)
				return false;
			iterations = distance / step + control.trip_count_adjustment;
		}
		else
		{
			if (control.counter_stride >= 0)
				return false;
			switch (control.termination)
			{
				case TerminationKind::DecrementToZero:
					if (seed == 0 || (seed & (step - 1)) != 0)
						return false;
					iterations = seed / step;
					break;
				case TerminationKind::DecrementWhileNonNegative:
					if ((seed & 0x80000000u) != 0)
						return false;
					iterations = seed / step + 1;
					break;
				case TerminationKind::DecrementWhilePositive:
					if (seed == 0 || (seed & 0x80000000u) != 0)
						return false;
					iterations = (seed + step - 1) / step;
					break;
				default:
					return false;
			}
		}
		if (iterations == 0 || iterations > UINT32_MAX)
			return false;
		*trip_count = static_cast<u32>(iterations);
		return true;
	}

	bool CalculateImmediateTripCount(const LoopControl& control, u32* trip_count)
	{
		if (!control.counter_seed_is_immediate ||
			((control.termination == TerminationKind::UnsignedLess ||
			  control.termination == TerminationKind::EqualEndpoint) &&
			 !control.bound_is_immediate))
		{
			return false;
		}
		return CalculateTripCount(control, control.counter_seed_immediate,
			control.bound_immediate, trip_count);
	}

	namespace
	{
		struct Definition
		{
			const Node* node = nullptr;
			u32 block = INVALID_BLOCK;
		};

		struct Incoming
		{
			u32 source_block = INVALID_BLOCK;
			const Transfer* transfer = nullptr;
		};

		// Direct-call IR deliberately shares one immutable callee body between all
		// verified callers.  A block-only CFG therefore gives its JR r31 every
		// possible return edge simultaneously and can manufacture cycles which no
		// execution can take.  Memory/timing proofs use bounded call-context nodes;
		// the executable IR and its source ownership remain shared and unchanged.
		struct ControlEdge
		{
			u32 target_node = INVALID_BLOCK;
			u32 state_source_block = INVALID_BLOCK;
			const Transfer* transfer = nullptr;
		};

		struct ControlIncoming
		{
			u32 source_node = INVALID_BLOCK;
			u32 state_source_block = INVALID_BLOCK;
			const Transfer* transfer = nullptr;
		};

		struct ControlNode
		{
			u32 block = INVALID_BLOCK;
			// Zero is the region owner.  Nonzero is one verified DirectCallContract
			// index plus one; nested calls fail closed in this first leaf-call model.
			u32 call_context = 0;
			std::vector<ControlEdge> successors;
			std::vector<ControlIncoming> predecessors;
		};

		struct Affine
		{
			bool valid = false;
			bool has_base = false;
			s64 offset = 0;
		};

		struct Bounded
		{
			bool valid = false;
			bool has_base = false;
			s64 minimum = 0;
			s64 maximum = 0;
			// Every possible (value - base), or value for a base-free fact, is
			// divisible by this power of two. The entry guard separately checks the
			// invariant base, so an aligned dynamic access is legal only when both
			// the base and every proven offset meet the guest instruction's alignment.
			u32 alignment = 1;
		};

		struct CompositeAffine
		{
			bool valid = false;
			bool has_invariant_base = false;
			bool has_induction = false;
			u32 induction_scale = 0;
			s64 offset = 0;
		};

		constexpr u32 MAX_PROVEN_ALIGNMENT = 1u << 30;

		u32 ConstantAlignment(s64 value)
		{
			if (value == 0)
				return MAX_PROVEN_ALIGNMENT;
			const u64 magnitude = value < 0 ? static_cast<u64>(-value) :
				static_cast<u64>(value);
			return static_cast<u32>(std::min<u64>(magnitude & (~magnitude + 1),
				MAX_PROVEN_ALIGNMENT));
		}

		bool SameGprTransfer(const Transfer& left, const Transfer& right)
		{
			return left.target_block == right.target_block &&
			       left.state.gpr == right.state.gpr;
		}

		class Analyzer
		{
		public:
			explicit Analyzer(const Program& program)
				: m_program(program)
			{
			}

			BuildResult Run()
			{
				BuildResult result{};
				if (m_program.blocks.empty() ||
					m_program.entry_block >= m_program.blocks.size() ||
					m_program.value_count == 0)
				{
					result.failure = BuildFailure::InvalidProgram;
					result.detail = "invalid verified-program dimensions";
					return result;
				}
				m_definitions.assign(m_program.value_count, {});
				m_incoming.resize(m_program.blocks.size());
				m_successors.resize(m_program.blocks.size());
				for (u32 block_index = 0; block_index < m_program.blocks.size(); block_index++)
				{
					for (const Node& node : m_program.blocks[block_index].nodes)
					{
						if (node.id >= m_definitions.size() ||
							m_definitions[node.id].node)
						{
							result.failure = BuildFailure::InvalidProgram;
							result.detail = "invalid value definition table";
							return result;
						}
						m_definitions[node.id] = {&node, block_index};
					}
				}

				auto append_edge = [&](u32 source, const Transfer& transfer) {
					if (transfer.target_block >= m_program.blocks.size())
						return;
					// A decoded conditional may have the same internal destination and
					// identical GPR transfer on both arms (for example after a constant
					// condition or convergent branch). It is one CFG edge for affine
					// dataflow, not two distinct predecessors of the target.
					for (const Incoming& incoming : m_incoming[transfer.target_block])
					{
						if (incoming.source_block == source && incoming.transfer &&
							SameGprTransfer(*incoming.transfer, transfer))
						{
							return;
						}
					}
					m_incoming[transfer.target_block].push_back({source, &transfer});
					m_successors[source].push_back(transfer.target_block);
				};
				for (u32 block_index = 0; block_index < m_program.blocks.size(); block_index++)
				{
					const Terminator& terminator =
						m_program.blocks[block_index].terminator;
					const std::optional<bool> constant_direction =
						terminator.kind == TerminatorKind::Branch ?
							ConstantBranchDirection(terminator.condition) :
							std::nullopt;
					if (!constant_direction || *constant_direction)
						append_edge(block_index, terminator.taken);
					if (terminator.kind == TerminatorKind::Branch &&
						(!constant_direction || !*constant_direction))
					{
						append_edge(block_index, terminator.not_taken);
					}
					if (terminator.kind == TerminatorKind::RegisterJump)
					{
						for (const Transfer& target : terminator.register_targets)
							append_edge(block_index, target);
					}
				}
				if (!BuildContextControlGraph())
				{
					result.reducibility_rejection = 3;
					result.detail = "direct-call control context is incomplete";
					return result;
				}
				if (!SelectNaturalLoop())
				{
					result.reducibility_rejection = static_cast<u8>(
						10 + m_control_reducibility_failure);
					result.detail = "region has multiple or irreducible cycles";
					return result;
				}
				// A reducible forward region has no repeated edge, but its complete
				// fixed-width memory footprint is still an entry invariant.  Prove that
				// footprint once so a hot caller+leaf unit does not retain one vTLB/SMC
				// check per LQC2/SQC2 merely because repetition belongs to an enclosing
				// tier-zero loop.  This is a memory proof only: event-frequency admission
				// remains owned by the generated entry probe.
				if (m_backedge_count == 0 && !m_program.direct_calls.empty() &&
					IsAcyclicReachableGraph())
				{
					m_header = m_program.entry_block;
					result.plan.header_seeds_match_entry = true;
					CollectBoundedRanges(&result.plan);
					result.detail = result.plan.bounded_ranges.empty() ?
						"acyclic region has no bounded memory range" : "";
					return result;
				}
				if (m_backedge_count != 1 || !m_backedge ||
					!IsAcyclicIterationBody())
				{
					result.reducibility_rejection = 2;
					result.detail = "region is not one entry-header natural loop";
					return result;
				}
				u32 maximum_iteration_cycles = 0;
				if (!MaximumIterationCycles(&maximum_iteration_cycles))
				{
					result.detail = "natural loop has no exact iteration-cycle bound";
					return result;
				}
				u32 maximum_prefix_cycles = 0;
				if (!MaximumPrefixCycles(&maximum_prefix_cycles) ||
					maximum_iteration_cycles > UINT32_MAX - maximum_prefix_cycles)
				{
					result.detail = "natural loop has no exact preheader-cycle bound";
					return result;
				}
				// Charging the one-time preheader on every predicted iteration is a
				// conservative horizon proof. It can reject a short loop, but can never
				// allow an event to become observable inside the fused region.
				maximum_iteration_cycles += maximum_prefix_cycles;
				result.plan.timing = {true, m_header, m_backedge_source,
					maximum_iteration_cycles};
				result.plan.header_seeds_match_entry = true;
				LoopControl first_control{};

				const Block& continuation = m_program.blocks[m_backedge_source];
				if (continuation.terminator.kind != TerminatorKind::Branch ||
					&continuation.terminator.taken != m_backedge)
				{
					result.detail = "backedge is not the true arm of one branch";
					return result;
				}

				for (u32 induction = 1; induction < 32; induction++)
				{
					const Affine recurrence = ResolveAffine(m_backedge_source,
						m_backedge->state.gpr[induction], induction);
					u32 counter_seed = 0;
					const bool counter_seed_matches_entry =
						HeaderGprMatchesEntry(induction);
					const bool counter_seed_is_immediate =
						!counter_seed_matches_entry &&
						HeaderGprConstantU32(induction, &counter_seed);
					if (!recurrence.valid || !recurrence.has_base || recurrence.offset <= 0 ||
						recurrence.offset > 4096 ||
						(static_cast<u64>(recurrence.offset) &
						 (static_cast<u64>(recurrence.offset) - 1)) != 0 ||
						(!counter_seed_matches_entry && !counter_seed_is_immediate))
					{
						continue;
					}
					u32 bound = 0;
					bool bound_is_immediate = false;
					TerminationKind termination{};
					u8 trip_count_adjustment = 0;
					bool signed_counter_compare = false;
					if (!MatchContinuation(induction,
						static_cast<u32>(recurrence.offset), &bound,
						&bound_is_immediate, &termination,
						&trip_count_adjustment, &signed_counter_compare))
					{
						continue;
					}
					if (!bound_is_immediate && !HeaderGprMatchesEntry(bound))
					{
						u32 constant_bound = 0;
						if (!HeaderGprConstantU32(bound, &constant_bound))
							continue;
						bound = constant_bound;
						bound_is_immediate = true;
					}
					LoopControl control{};
					control.valid = true;
					control.header_block = m_header;
					control.counter_gpr = static_cast<u8>(induction);
					control.counter_seed_is_immediate = counter_seed_is_immediate;
					control.counter_seed_immediate = counter_seed;
					control.bound_gpr = bound_is_immediate ? 0 :
						static_cast<u8>(bound);
					control.bound_is_immediate = bound_is_immediate;
					control.bound_immediate = bound_is_immediate ? bound : 0;
					control.counter_stride = static_cast<s32>(recurrence.offset);
					control.termination = termination;
					control.trip_count_adjustment = trip_count_adjustment;
					control.signed_counter_compare = signed_counter_compare;
					control.maximum_iteration_scaled_cycles =
						maximum_iteration_cycles;
					std::vector<CountedRange> candidates;
					for (u32 pointer = 1; pointer < 32; pointer++)
					{
						const Affine pointer_recurrence = ResolveAffine(m_backedge_source,
							m_backedge->state.gpr[pointer], pointer);
						u32 entry_pointer = 0;
						s32 entry_pointer_offset = 0;
						if (!pointer_recurrence.valid || !pointer_recurrence.has_base ||
							pointer_recurrence.offset <= 0 ||
							pointer_recurrence.offset > 4096 ||
							(static_cast<u64>(pointer_recurrence.offset) &
							 (static_cast<u64>(pointer_recurrence.offset) - 1)) != 0 ||
							!HeaderGprAffineEntry(pointer, &entry_pointer,
								&entry_pointer_offset))
						{
							continue;
						}
						CountedRange candidate{};
						if (!CollectAccesses(pointer,
								static_cast<u32>(pointer_recurrence.offset), &candidate))
						{
							continue;
						}
						candidate.entry_induction_gpr =
							static_cast<u8>(entry_pointer);
						candidate.entry_induction_offset = entry_pointer_offset;
						candidates.push_back(std::move(candidate));
					}
					SelectDisjointRanges(std::move(candidates), control, &result.plan);
					if (!result.plan.counted_ranges.empty())
					{
						CollectBoundedRanges(&result.plan);
						return result;
					}
					if (!first_control.valid)
						first_control = control;
				}

				// A fill/copy loop commonly advances its address GPR while decrementing
				// an independent remaining-count GPR.  The old proof required the address
				// itself to appear in the branch predicate, so these loops paid vTLB and
				// source-ownership guards for every byte.  Prove the trip count from either
				// an exact zero recurrence or a signed post-decrement nonnegative test,
				// then select the affine pointers which cover the most memory operations.
				// Uncovered operations retain their exact per-access checks.
				Plan best_plan{};
				size_t best_accesses = 0;
				for (u32 counter = 1; counter < 32; counter++)
				{
					const Affine counter_recurrence = ResolveAffine(m_backedge_source,
						m_backedge->state.gpr[counter], counter);
					const u64 magnitude = counter_recurrence.offset < 0 ?
						static_cast<u64>(-counter_recurrence.offset) : 0;
					u32 counter_seed = 0;
					const bool counter_seed_matches_entry = HeaderGprMatchesEntry(counter);
					const bool counter_seed_is_immediate = !counter_seed_matches_entry &&
						HeaderGprConstantU32(counter, &counter_seed);
					if (!counter_recurrence.valid || !counter_recurrence.has_base ||
						counter_recurrence.offset >= 0 || magnitude == 0 ||
						magnitude > 4096 || (magnitude & (magnitude - 1)) != 0 ||
						(!counter_seed_matches_entry && !counter_seed_is_immediate))
					{
						continue;
					}
					TerminationKind termination{};
					if (MatchDecrementToZero(counter, counter_recurrence.offset))
						termination = TerminationKind::DecrementToZero;
					else if (MatchDecrementWhileNonNegative(counter,
							counter_recurrence.offset))
					{
						termination = TerminationKind::DecrementWhileNonNegative;
					}
					else if (MatchDecrementWhilePositive(counter,
							counter_recurrence.offset))
					{
						termination = TerminationKind::DecrementWhilePositive;
					}
					else
					{
						continue;
					}
					LoopControl control{};
					control.valid = true;
					control.header_block = m_header;
					control.counter_gpr = static_cast<u8>(counter);
					control.counter_seed_is_immediate = counter_seed_is_immediate;
					control.counter_seed_immediate = counter_seed;
					control.counter_stride = static_cast<s32>(
						counter_recurrence.offset);
					control.termination = termination;
					control.maximum_iteration_scaled_cycles =
						maximum_iteration_cycles;
					if (!first_control.valid)
						first_control = control;
					std::vector<CountedRange> candidates;
					for (u32 induction = 1; induction < 32; induction++)
					{
						const Affine pointer_recurrence = ResolveAffine(m_backedge_source,
							m_backedge->state.gpr[induction], induction);
						u32 entry_pointer = 0;
						s32 entry_pointer_offset = 0;
						if (!pointer_recurrence.valid || !pointer_recurrence.has_base ||
							pointer_recurrence.offset <= 0 ||
							pointer_recurrence.offset > 4096 ||
							(static_cast<u64>(pointer_recurrence.offset) &
							 (static_cast<u64>(pointer_recurrence.offset) - 1)) != 0 ||
							!HeaderGprAffineEntry(induction, &entry_pointer,
								&entry_pointer_offset))
						{
							continue;
						}
						CountedRange candidate{};
						if (!CollectAccesses(induction,
								static_cast<u32>(pointer_recurrence.offset), &candidate))
						{
							continue;
						}
						candidate.entry_induction_gpr =
							static_cast<u8>(entry_pointer);
						candidate.entry_induction_offset = entry_pointer_offset;
						candidates.push_back(std::move(candidate));
					}
					Plan candidate_plan{};
					candidate_plan.timing = result.plan.timing;
					candidate_plan.header_seeds_match_entry = true;
					SelectDisjointRanges(std::move(candidates), control,
						&candidate_plan);
					size_t access_count = 0;
					for (const CountedRange& range : candidate_plan.counted_ranges)
						access_count += range.accesses.size();
					if (access_count > best_accesses)
					{
						best_accesses = access_count;
						best_plan = std::move(candidate_plan);
					}
				}
				if (!best_plan.counted_ranges.empty())
					result.plan = std::move(best_plan);
				if (!result.plan.counted_ranges.empty())
				{
					CollectBoundedRanges(&result.plan);
					return result;
				}
				result.plan.control = first_control;
				CollectBoundedRanges(&result.plan);
				if (!result.plan.counted_ranges.empty() ||
					!result.plan.bounded_ranges.empty())
					return result;
				result.detail = first_control.valid ?
					"counted natural loop has no affine memory range" :
					"no affine induction/bound/access proof";
				return result;
			}

		private:
			bool IsSemanticZero(ValueId value, u32 depth = 0) const
			{
				if (depth > 128)
					return false;
				const Definition* definition = Def(value);
				if (!definition)
					return false;
				const Node& node = *definition->node;
				if ((node.opcode == Opcode::ConstantI1 ||
					 node.opcode == Opcode::ConstantI32 ||
					 node.opcode == Opcode::ConstantI64) && node.literal == 0)
				{
					return true;
				}
				u32 gpr = 0;
				if (node.opcode == Opcode::Parameter &&
					ParameterGpr(*definition, &gpr))
				{
					return gpr == 0;
				}
				if ((node.opcode == Opcode::ExtractLow32 ||
					 node.opcode == Opcode::ExtractLow64 ||
					 node.opcode == Opcode::SignExtend32To64 ||
					 node.opcode == Opcode::ZeroExtend32To64) &&
					node.operand_count == 1)
				{
					return IsSemanticZero(node.operands[0], depth + 1);
				}
				if (node.opcode == Opcode::ReplaceLow64 &&
					node.operand_count == 2)
				{
					return IsSemanticZero(node.operands[1], depth + 1);
				}
				return false;
			}

			std::optional<bool> ConstantBranchDirection(ValueId condition) const
			{
				const Definition* definition = Def(condition);
				if (!definition)
					return std::nullopt;
				const Node& node = *definition->node;
				if (node.opcode == Opcode::ConstantI1)
					return node.literal != 0;
				// The EE branch lowering represents BEQ/BNE through exact integer
				// equality nodes.  Equal(x,x) and NotEqual(x,x) are semantic
				// constants even when x is a block parameter.  Removing the
				// impossible arm is required for a faithful semantic CFG: otherwise
				// an unconditional BEQ r0,r0 fallthrough becomes a fake predecessor
				// and destroys the natural-loop recurrence proof.
				if (node.operand_count == 2 &&
					(node.operands[0] == node.operands[1] ||
					 (IsSemanticZero(node.operands[0]) &&
					  IsSemanticZero(node.operands[1]))))
				{
					if (node.opcode == Opcode::CompareEqual64)
						return true;
					if (node.opcode == Opcode::CompareNotEqual64)
						return false;
				}
				return std::nullopt;
			}

			static void SelectDisjointRanges(std::vector<CountedRange> candidates,
				const LoopControl& control, Plan* plan)
			{
				if (!plan)
					return;
				std::sort(candidates.begin(), candidates.end(),
					[](const CountedRange& left, const CountedRange& right) {
						if (left.accesses.size() != right.accesses.size())
							return left.accesses.size() > right.accesses.size();
						return left.induction_gpr < right.induction_gpr;
					});
				std::vector<ValueId> covered;
				for (CountedRange& candidate : candidates)
				{
					candidate.accesses.erase(std::remove_if(candidate.accesses.begin(),
						candidate.accesses.end(), [&](const Access& access) {
							return std::find(covered.begin(), covered.end(),
								access.operation) != covered.end();
						}), candidate.accesses.end());
					if (candidate.accesses.empty())
						continue;
					for (const Access& access : candidate.accesses)
						covered.push_back(access.operation);
					plan->counted_ranges.push_back(std::move(candidate));
				}
				if (!plan->counted_ranges.empty())
					plan->control = control;
			}

			const Definition* Def(ValueId value) const
			{
				return value < m_definitions.size() && m_definitions[value].node ?
					&m_definitions[value] : nullptr;
			}

			const Transfer* EdgeTransfer(u32 source, u32 target) const
			{
				if (source >= m_program.blocks.size())
					return nullptr;
				const Terminator& terminator = m_program.blocks[source].terminator;
				if (terminator.taken.target_block == target)
					return &terminator.taken;
				if (terminator.kind == TerminatorKind::Branch &&
					terminator.not_taken.target_block == target)
				{
					return &terminator.not_taken;
				}
				if (terminator.kind == TerminatorKind::RegisterJump)
				{
					const auto found = std::find_if(terminator.register_targets.begin(),
						terminator.register_targets.end(), [&](const Transfer& transfer) {
							return transfer.target_block == target;
						});
					if (found != terminator.register_targets.end())
						return &*found;
				}
				return nullptr;
			}

			bool BuildContextControlGraph()
			{
				m_control_nodes.clear();
				m_entry_control_node = INVALID_BLOCK;
				if (m_program.entry_block >= m_program.blocks.size())
					return false;

				auto find_block_pc = [&](u32 pc) {
					const auto found = std::find_if(m_program.blocks.begin(),
						m_program.blocks.end(), [&](const Block& block) {
							return block.pc == pc;
						});
					return found == m_program.blocks.end() ? INVALID_BLOCK :
						static_cast<u32>(found - m_program.blocks.begin());
				};
				auto call_at_block = [&](u32 block) -> u32 {
					if (block >= m_program.blocks.size())
						return INVALID_BLOCK;
					const u32 branch_pc =
						m_program.blocks[block].terminator.branch_pc;
					for (u32 index = 0; index < m_program.direct_calls.size(); index++)
					{
						if (m_program.direct_calls[index].call_pc == branch_pc)
							return index;
					}
					return INVALID_BLOCK;
				};
				auto find_or_add_node = [&](u32 block, u32 context) {
					for (u32 index = 0; index < m_control_nodes.size(); index++)
					{
						if (m_control_nodes[index].block == block &&
							m_control_nodes[index].call_context == context)
						{
							return index;
						}
					}
					m_control_nodes.push_back({block, context, {}, {}});
					return static_cast<u32>(m_control_nodes.size() - 1);
				};

				m_entry_control_node = find_or_add_node(m_program.entry_block, 0);
				for (u32 node_index = 0; node_index < m_control_nodes.size(); node_index++)
				{
					const u32 block_index = m_control_nodes[node_index].block;
					const u32 context = m_control_nodes[node_index].call_context;
					if (block_index >= m_program.blocks.size() ||
						context > m_program.direct_calls.size())
					{
						return false;
					}
					const Block& block = m_program.blocks[block_index];
					const Terminator& terminator = block.terminator;
					const u32 nested_call = call_at_block(block_index);

					auto append = [&](const Transfer& transfer, u32 target_context) {
						u32 target_node = INVALID_BLOCK;
						if (transfer.target_block != INVALID_BLOCK)
						{
							if (transfer.target_block >= m_program.blocks.size())
								return false;
							target_node = find_or_add_node(transfer.target_block,
								target_context);
						}
						for (const ControlEdge& existing :
							m_control_nodes[node_index].successors)
						{
							if (target_node != INVALID_BLOCK &&
								existing.target_node == target_node &&
								existing.state_source_block == block_index &&
								existing.transfer &&
								SameGprTransfer(*existing.transfer, transfer))
							{
								return true;
							}
						}
						m_control_nodes[node_index].successors.push_back(
							{target_node, block_index, &transfer});
						if (target_node != INVALID_BLOCK)
						{
							m_control_nodes[target_node].predecessors.push_back(
								{node_index, block_index, &transfer});
						}
						return true;
					};

					if (context == 0 && nested_call != INVALID_BLOCK)
					{
						const DirectCallContract& call =
							m_program.direct_calls[nested_call];
						const u32 callee = find_block_pc(call.callee_pc);
						if (terminator.kind != TerminatorKind::Jump ||
							callee == INVALID_BLOCK ||
							terminator.taken.target_block != callee ||
							!append(terminator.taken, nested_call + 1))
						{
							return false;
						}
						continue;
					}

					if (context != 0)
					{
						// A context is one verified leaf invocation. Nested ownership
						// needs a bounded call stack rather than guessing which r31 wins.
						if (nested_call != INVALID_BLOCK)
							return false;
						const DirectCallContract& call =
							m_program.direct_calls[context - 1];
						if (terminator.branch_pc == call.return_jump_pc)
						{
							const auto target = std::find_if(
								terminator.register_targets.begin(),
								terminator.register_targets.end(),
								[&](const Transfer& transfer) {
									return transfer.register_target_proven &&
										transfer.proven_register_target_pc == call.return_pc;
								});
							if (terminator.kind != TerminatorKind::RegisterJump ||
								target == terminator.register_targets.end() ||
								!append(*target, 0))
							{
								return false;
							}
							continue;
						}
						if (terminator.kind == TerminatorKind::RegisterJump)
							return false;
					}
					else if (terminator.kind == TerminatorKind::RegisterJump &&
						!terminator.register_targets.empty())
					{
						return false;
					}

					const std::optional<bool> constant_direction =
						terminator.kind == TerminatorKind::Branch ?
							ConstantBranchDirection(terminator.condition) :
							std::nullopt;
					if ((!constant_direction || *constant_direction) &&
						!append(terminator.taken, context))
					{
						return false;
					}
					if (terminator.kind == TerminatorKind::Branch &&
						(!constant_direction || !*constant_direction) &&
						!append(terminator.not_taken, context))
					{
						return false;
					}
				}

				// Every immutable IR owner must be reachable in exactly one ownership
				// class. A shared leaf may have several nonzero contexts; appearing both
				// as an outer block and a leaf would make its return contract ambiguous.
				std::vector<u8> ownership(m_program.blocks.size(), 0);
				for (const ControlNode& node : m_control_nodes)
				{
					const u8 bit = node.call_context == 0 ? 1u : 2u;
					ownership[node.block] |= bit;
					if (ownership[node.block] == 3)
						return false;
				}
				return std::none_of(ownership.begin(), ownership.end(),
					[](u8 value) { return value == 0; });
			}

			bool SelectNaturalLoop()
			{
				m_control_reducibility_failure = 0;
				const u32 count = static_cast<u32>(m_control_nodes.size());
				if (m_entry_control_node >= count)
				{
					m_control_reducibility_failure = 1;
					return false;
				}
				std::vector<u8> reached(count, 0);
				std::vector<u32> worklist{m_entry_control_node};
				reached[m_entry_control_node] = 1;
				while (!worklist.empty())
				{
					const u32 source = worklist.back();
					worklist.pop_back();
					for (const ControlEdge& edge :
						m_control_nodes[source].successors)
					{
						const u32 target = edge.target_node;
						if (target >= count || reached[target])
							continue;
						reached[target] = 1;
						worklist.push_back(target);
					}
				}
				if (std::any_of(reached.begin(), reached.end(), [](u8 value) {
						return value == 0;
					}))
				{
					m_control_reducibility_failure = 2;
					return false;
				}

				// A reducible backedge is exactly an edge whose destination dominates
				// its source. Compute dominators over the tiny (at most RegionIR-sized)
				// decoded CFG instead of relying on block order or a DFS accident.
				std::vector<std::vector<u8>> dominates(count,
					std::vector<u8>(count, 1));
				dominates[m_entry_control_node].assign(count, 0);
				dominates[m_entry_control_node][m_entry_control_node] = 1;
				bool changed = true;
				for (u32 pass = 0; changed && pass <= count; pass++)
				{
					changed = false;
					for (u32 block = 0; block < count; block++)
					{
						if (block == m_entry_control_node)
							continue;
						std::vector<u8> next(count, 1);
						bool any = false;
						for (const ControlIncoming& incoming :
							m_control_nodes[block].predecessors)
						{
							const u32 source = incoming.source_node;
							if (source >= count)
								continue;
							if (!any)
								next = dominates[source];
							else
								for (u32 bit = 0; bit < count; bit++)
									next[bit] &= dominates[source][bit];
							any = true;
						}
						if (!any)
						{
							m_control_reducibility_failure = 3;
							return false;
						}
						next[block] = 1;
						if (next != dominates[block])
						{
							dominates[block] = std::move(next);
							changed = true;
						}
					}
				}

				m_backedge_count = 0;
				m_backedge_source = INVALID_BLOCK;
				m_backedge_control_source = INVALID_BLOCK;
				m_header_control_node = INVALID_BLOCK;
				m_backedge = nullptr;
				m_header = INVALID_BLOCK;
				for (u32 source = 0; source < count; source++)
				{
					for (const ControlEdge& edge :
						m_control_nodes[source].successors)
					{
						const u32 target = edge.target_node;
						if (target >= count || !dominates[source][target])
							continue;
						m_backedge_count++;
						m_header_control_node = target;
						m_header = m_control_nodes[target].block;
						m_backedge_control_source = source;
						m_backedge_source = edge.state_source_block;
						m_backedge = edge.transfer;
					}
				}
				if (m_backedge_count == 0)
				{
					m_header = m_program.entry_block;
					m_header_control_node = m_entry_control_node;
					m_loop_members.assign(m_program.blocks.size(), 0);
					m_control_loop_members.assign(count, 0);
					return true;
				}
				if (m_backedge_count != 1 || !m_backedge ||
					m_header_control_node >= count ||
					m_backedge_control_source >= count)
				{
					m_control_reducibility_failure = 4;
					return false;
				}

				m_control_loop_members.assign(count, 0);
				m_control_loop_members[m_header_control_node] = 1;
				m_control_loop_members[m_backedge_control_source] = 1;
				worklist = {m_backedge_control_source};
				while (!worklist.empty())
				{
					const u32 target = worklist.back();
					worklist.pop_back();
					if (target == m_header_control_node)
						continue;
					for (const ControlIncoming& incoming :
						m_control_nodes[target].predecessors)
					{
						const u32 source = incoming.source_node;
						if (source >= count || m_control_loop_members[source])
							continue;
						m_control_loop_members[source] = 1;
						worklist.push_back(source);
					}
				}
				m_loop_members.assign(m_program.blocks.size(), 0);
				for (u32 block = 0; block < count; block++)
				{
					if (!m_control_loop_members[block])
						continue;
					m_loop_members[m_control_nodes[block].block] = 1;
					if (!dominates[block][m_header_control_node])
					{
						m_control_reducibility_failure = 5;
						return false;
					}
				}
				return true;
			}

			bool ResolveEntryGprIdentity(u32 block, ValueId value, u32 expected_gpr,
				u32 depth = 0) const
			{
				(void)block;
				if (depth > m_program.blocks.size() * 2 + 2)
					return false;
				const Definition* definition = Def(value);
				if (!definition || definition->node->opcode != Opcode::Parameter)
					return false;
				u32 gpr = 0;
				if (!ParameterGpr(*definition, &gpr) || gpr != expected_gpr)
					return false;
				if (definition->block == m_program.entry_block)
					return true;
				if (definition->block >= m_incoming.size() ||
					m_incoming[definition->block].empty())
				{
					return false;
				}
				for (const Incoming& incoming : m_incoming[definition->block])
				{
					if (!incoming.transfer ||
						!ResolveEntryGprIdentity(incoming.source_block,
							incoming.transfer->state.gpr[gpr], expected_gpr, depth + 1))
					{
						return false;
					}
				}
				return true;
			}

			bool HeaderGprMatchesEntry(u32 gpr) const
			{
				if (gpr == 0 || gpr >= 32 || m_header >= m_incoming.size())
					return false;
				bool saw_preheader = m_header == m_program.entry_block;
				for (const Incoming& incoming : m_incoming[m_header])
				{
					if (incoming.source_block == m_backedge_source &&
						incoming.transfer == m_backedge)
					{
						continue;
					}
					saw_preheader = true;
					if (!incoming.transfer ||
						!ResolveEntryGprIdentity(incoming.source_block,
							incoming.transfer->state.gpr[gpr], gpr))
					{
						return false;
					}
				}
				return saw_preheader;
			}

			bool HeaderGprConstantU32(u32 gpr, u32* value) const
			{
				if (!value || gpr == 0 || gpr >= 32 ||
					m_header >= m_incoming.size() || m_header == m_program.entry_block)
				{
					return false;
				}
				bool saw_preheader = false;
				u32 common = 0;
				for (const Incoming& incoming : m_incoming[m_header])
				{
					if (incoming.source_block == m_backedge_source &&
						incoming.transfer == m_backedge)
					{
						continue;
					}
					if (!incoming.transfer)
						return false;
					const Affine resolved = ResolveAffine(incoming.source_block,
						incoming.transfer->state.gpr[gpr], gpr);
					if (!resolved.valid || resolved.has_base || resolved.offset < 0 ||
						static_cast<u64>(resolved.offset) > UINT32_MAX)
					{
						return false;
					}
					const u32 candidate = static_cast<u32>(resolved.offset);
					if (saw_preheader && candidate != common)
						return false;
					common = candidate;
					saw_preheader = true;
				}
				if (!saw_preheader)
					return false;
				*value = common;
				return true;
			}

			bool HeaderGprAffineEntry(u32 gpr, u32* entry_gpr,
				s32* entry_offset) const
			{
				if (!entry_gpr || !entry_offset || gpr == 0 || gpr >= 32 ||
					m_header >= m_incoming.size())
				{
					return false;
				}
				if (m_header == m_program.entry_block)
				{
					*entry_gpr = gpr;
					*entry_offset = 0;
					return true;
				}

				bool saw_preheader = false;
				u32 common_gpr = 0;
				s64 common_offset = 0;
				for (const Incoming& incoming : m_incoming[m_header])
				{
					if (incoming.source_block == m_backedge_source &&
						incoming.transfer == m_backedge)
					{
						continue;
					}
					if (!incoming.transfer)
						return false;

					const ValueId seed = incoming.transfer->state.gpr[gpr];
					bool found = false;
					u32 candidate_gpr = 0;
					s64 candidate_offset = 0;
					for (u32 candidate = 1; candidate < 32; candidate++)
					{
						const Affine resolved = ResolveAffine(incoming.source_block,
							seed, candidate);
						if (!resolved.valid || !resolved.has_base ||
							resolved.offset < INT32_MIN || resolved.offset > INT32_MAX)
						{
							continue;
						}
						if (found)
							return false;
						found = true;
						candidate_gpr = candidate;
						candidate_offset = resolved.offset;
					}
					if (!found || (saw_preheader &&
						(candidate_gpr != common_gpr ||
						 candidate_offset != common_offset)))
					{
						return false;
					}
					common_gpr = candidate_gpr;
					common_offset = candidate_offset;
					saw_preheader = true;
				}
				if (!saw_preheader)
					return false;
				*entry_gpr = common_gpr;
				*entry_offset = static_cast<s32>(common_offset);
				return true;
			}

			void InternalSuccessors(u32 node, std::vector<u32>* output) const
			{
				if (!output || node >= m_control_nodes.size())
					return;
				for (const ControlEdge& edge : m_control_nodes[node].successors)
				{
					if (edge.target_node == INVALID_BLOCK ||
						(node == m_backedge_control_source &&
						 edge.target_node == m_header_control_node))
					{
						continue;
					}
					output->push_back(edge.target_node);
				}
			}

			bool IsAcyclicIterationBody() const
			{
				std::vector<u8> color(m_control_nodes.size(), 0);
				std::vector<u8> reached(m_control_nodes.size(), 0);
				auto visit = [&](auto&& self, u32 node) -> bool {
					if (node >= color.size() || color[node] == 1)
						return false;
					if (color[node] == 2)
						return true;
					color[node] = 1;
					reached[node] = 1;
					std::vector<u32> successors;
					InternalSuccessors(node, &successors);
					for (u32 target : successors)
					{
						if (!self(self, target))
							return false;
					}
					color[node] = 2;
					return true;
				};
				if (!visit(visit, m_entry_control_node))
					return false;
				for (u32 node = 0; node < reached.size(); node++)
				{
					if (!reached[node])
					{
						return false;
					}
				}
				return true;
			}

			bool IsLoopBlock(u32 block) const
			{
				return block < m_loop_members.size() && m_loop_members[block] != 0;
			}

			bool IsAcyclicReachableGraph() const
			{
				std::vector<u8> color(m_control_nodes.size(), 0);
				auto visit = [&](auto&& self, u32 node) -> bool {
					if (node >= color.size() || color[node] == 1)
						return false;
					if (color[node] == 2)
						return true;
					color[node] = 1;
					for (const ControlEdge& edge : m_control_nodes[node].successors)
					{
						if (edge.target_node != INVALID_BLOCK &&
							!self(self, edge.target_node))
							return false;
					}
					color[node] = 2;
					return true;
				};
				if (!visit(visit, m_header_control_node))
					return false;
				return std::none_of(color.begin(), color.end(),
					[](u8 state) { return state == 0; });
			}

			bool ParameterGpr(const Definition& definition, u32* gpr) const
			{
				if (definition.node->opcode != Opcode::Parameter ||
					definition.block >= m_program.blocks.size())
				{
					return false;
				}
				for (u32 index = 0; index < 32; index++)
				{
					if (m_program.blocks[definition.block].parameters.gpr[index] ==
						definition.node->id)
					{
						*gpr = index;
						return true;
					}
				}
				return false;
			}

			bool FollowParameter(const Definition& definition,
				u32* block, ValueId* value) const
			{
				u32 gpr = 0;
				if (!ParameterGpr(definition, &gpr) || definition.block == m_header ||
					m_incoming[definition.block].size() != 1)
				{
					return false;
				}
				const Incoming& incoming = m_incoming[definition.block][0];
				if (!incoming.transfer)
					return false;
				*block = incoming.source_block;
				*value = incoming.transfer->state.gpr[gpr];
				return true;
			}

			Affine ResolveAffine(u32 block, ValueId value, u32 base_gpr,
				u32 depth = 0) const
			{
				if (depth > 128)
					return {};
				const Definition* definition = Def(value);
				if (!definition)
					return {};
				const Node& node = *definition->node;
				if (node.opcode == Opcode::Parameter)
				{
					u32 gpr = 0;
					if (!ParameterGpr(*definition, &gpr))
						return {};
					if (gpr == 0)
						return {true, false, 0};
					if (definition->block == m_header ||
						definition->block == m_program.entry_block)
					{
						if (gpr == base_gpr)
							return {true, true, 0};
						return {};
					}
					if (definition->block < m_incoming.size() &&
						m_incoming[definition->block].size() > 1)
					{
						Affine common{};
						bool have_common = false;
						for (const Incoming& incoming : m_incoming[definition->block])
						{
							if (!incoming.transfer)
								return {};
							const Affine candidate = ResolveAffine(
								incoming.source_block,
								incoming.transfer->state.gpr[gpr], base_gpr,
								depth + 1);
							if (!candidate.valid || (have_common &&
								(candidate.has_base != common.has_base ||
								 candidate.offset != common.offset)))
							{
								return {};
							}
							common = candidate;
							have_common = true;
						}
						return have_common ? common : Affine{};
					}
					u32 source = block;
					ValueId incoming = value;
					if (!FollowParameter(*definition, &source, &incoming))
						return {};
					return ResolveAffine(source, incoming, base_gpr, depth + 1);
				}
				if (node.opcode == Opcode::ConstantI1 ||
					node.opcode == Opcode::ConstantI32 ||
					node.opcode == Opcode::ConstantI64 ||
					node.opcode == Opcode::ConstantAddress)
				{
					return {true, false,
						static_cast<s32>(static_cast<u32>(node.literal))};
				}
				auto unary = [&]() {
					return node.operand_count >= 1 ?
						ResolveAffine(definition->block, node.operands[0], base_gpr,
							depth + 1) : Affine{};
				};
				switch (node.opcode)
				{
					case Opcode::ExtractLow32:
					case Opcode::ExtractLow64:
					case Opcode::AddressFromI32:
					case Opcode::SignExtend32To64:
					case Opcode::ZeroExtend32To64:
						return unary();
					case Opcode::ReplaceLow64:
						return node.operand_count == 2 ?
							ResolveAffine(definition->block, node.operands[1], base_gpr,
								depth + 1) : Affine{};
					case Opcode::Add32:
					case Opcode::Add64:
					case Opcode::EffectiveAddress32:
					case Opcode::Sub32:
					case Opcode::Sub64:
					{
						if (node.operand_count != 2)
							return {};
						const Affine left = ResolveAffine(definition->block,
							node.operands[0], base_gpr, depth + 1);
						const Affine right = ResolveAffine(definition->block,
							node.operands[1], base_gpr, depth + 1);
						const bool subtract = node.opcode == Opcode::Sub32 ||
							node.opcode == Opcode::Sub64;
						if (!left.valid || !right.valid ||
							(left.has_base && right.has_base) ||
							(subtract && right.has_base))
						{
							return {};
						}
						const s64 offset = left.offset +
							(subtract ? -right.offset : right.offset);
						if (offset < INT32_MIN || offset > INT32_MAX)
							return {};
						return {true, left.has_base || right.has_base, offset};
					}
					default:
						return {};
				}
			}

			Bounded ResolveBoundedAtAnchor(u32 block, ValueId value, u32 base_gpr,
				u32 anchor_block, u32 depth = 0,
				u32 control_node = INVALID_BLOCK,
				u32 anchor_control_node = INVALID_BLOCK) const
			{
				if (depth > 128)
					return {};
				const Definition* definition = Def(value);
				if (!definition)
					return {};
				const Node& node = *definition->node;
				if (node.opcode == Opcode::Parameter)
				{
					u32 gpr = 0;
					if (!ParameterGpr(*definition, &gpr))
						return {};
					if (control_node != INVALID_BLOCK)
					{
						if (control_node >= m_control_nodes.size() ||
							m_control_nodes[control_node].block != definition->block ||
							anchor_control_node >= m_control_nodes.size())
						{
							return {};
						}
						if (control_node == anchor_control_node)
						{
							if (gpr == base_gpr)
								return {true, true, 0, 0, MAX_PROVEN_ALIGNMENT};
							if (gpr == 0)
								return {true, false, 0, 0, MAX_PROVEN_ALIGNMENT};
							return {};
						}
						Bounded merged{};
						bool have_merged = false;
						for (const ControlIncoming& incoming :
							m_control_nodes[control_node].predecessors)
						{
							if (!incoming.transfer)
								return {};
							const Bounded candidate = ResolveBoundedAtAnchor(
								incoming.state_source_block,
								incoming.transfer->state.gpr[gpr], base_gpr,
								anchor_block, depth + 1, incoming.source_node,
								anchor_control_node);
							if (!candidate.valid || (have_merged &&
								candidate.has_base != merged.has_base))
							{
								return {};
							}
							if (!have_merged)
							{
								merged = candidate;
								have_merged = true;
							}
							else
							{
								merged.minimum = std::min(merged.minimum,
									candidate.minimum);
								merged.maximum = std::max(merged.maximum,
									candidate.maximum);
								merged.alignment = std::min(merged.alignment,
									candidate.alignment);
							}
						}
						return have_merged ? merged : Bounded{};
					}
					if (definition->block == anchor_block)
					{
						if (gpr == base_gpr)
							return {true, true, 0, 0, MAX_PROVEN_ALIGNMENT};
						if (gpr == 0)
							return {true, false, 0, 0, MAX_PROVEN_ALIGNMENT};
						return {};
					}
					if (definition->block < m_incoming.size() &&
						m_incoming[definition->block].size() > 1)
					{
						Bounded merged{};
						bool have_merged = false;
						for (const Incoming& incoming : m_incoming[definition->block])
						{
							if (!incoming.transfer)
								return {};
							const Bounded candidate = ResolveBoundedAtAnchor(
								incoming.source_block,
								incoming.transfer->state.gpr[gpr], base_gpr,
								anchor_block, depth + 1);
							if (!candidate.valid || (have_merged &&
								candidate.has_base != merged.has_base))
							{
								return {};
							}
							if (!have_merged)
							{
								merged = candidate;
								have_merged = true;
							}
							else
							{
								merged.minimum = std::min(merged.minimum,
									candidate.minimum);
								merged.maximum = std::max(merged.maximum,
									candidate.maximum);
								merged.alignment = std::min(merged.alignment,
									candidate.alignment);
							}
						}
						return have_merged ? merged : Bounded{};
					}
					u32 source = block;
					ValueId incoming = value;
					return FollowParameter(*definition, &source, &incoming) ?
						ResolveBoundedAtAnchor(source, incoming, base_gpr,
							anchor_block, depth + 1) :
						Bounded{};
				}
				if (node.opcode == Opcode::ConstantI1 ||
					node.opcode == Opcode::ConstantI32 ||
					node.opcode == Opcode::ConstantI64 ||
					node.opcode == Opcode::ConstantAddress)
				{
					const s64 literal = static_cast<s32>(
						static_cast<u32>(node.literal));
					return {true, false, literal, literal,
						ConstantAlignment(literal)};
				}
				auto unary = [&]() {
					return node.operand_count >= 1 ?
						ResolveBoundedAtAnchor(definition->block, node.operands[0],
							base_gpr, anchor_block, depth + 1, control_node,
							anchor_control_node) : Bounded{};
				};
				switch (node.opcode)
				{
					case Opcode::ExtractLow32:
					case Opcode::ExtractLow64:
					case Opcode::AddressFromI32:
					case Opcode::ZeroExtend32To64:
					case Opcode::SignExtend32To64:
						return unary();
					case Opcode::ReplaceLow64:
						return node.operand_count == 2 ?
							ResolveBoundedAtAnchor(definition->block, node.operands[1],
								base_gpr, anchor_block, depth + 1, control_node,
								anchor_control_node) : Bounded{};
					case Opcode::MemoryLoadValue:
					{
						if (node.operand_count != 1)
							return {};
						const Definition* effect = Def(node.operands[0]);
						if (!effect || effect->node->opcode != Opcode::MemoryLoad ||
							effect->node->immediate >
								static_cast<u32>(MemoryAccessKind::StoreVu0Vector))
						{
							return {};
						}
							switch (static_cast<MemoryAccessKind>(effect->node->immediate))
							{
								case MemoryAccessKind::LoadU8:
									return {true, false, 0, UINT8_MAX, 1};
								case MemoryAccessKind::LoadU16:
									return {true, false, 0, UINT16_MAX, 1};
							default:
								return {};
						}
					}
					case Opcode::ShiftLeft32:
					{
						Bounded input = unary();
						if (!input.valid || input.has_base || node.immediate >= 32 ||
							input.minimum < 0 ||
							static_cast<u64>(input.maximum) >
								(UINT32_MAX >> node.immediate))
						{
							return {};
						}
						input.minimum <<= node.immediate;
						input.maximum <<= node.immediate;
						input.alignment = static_cast<u32>(std::min<u64>(
							static_cast<u64>(input.alignment) << node.immediate,
							MAX_PROVEN_ALIGNMENT));
						return input;
					}
					case Opcode::And32:
					{
						if (node.operand_count != 2)
							return {};
						for (u32 constant_index = 0; constant_index < 2;
							constant_index++)
						{
							const Definition* mask = Def(node.operands[constant_index]);
							if (!mask || (mask->node->opcode != Opcode::ConstantI32 &&
								mask->node->opcode != Opcode::ConstantI64) ||
								mask->node->literal > UINT32_MAX)
							{
								continue;
							}
								const u32 value = static_cast<u32>(mask->node->literal);
								return {true, false, 0, value,
									ConstantAlignment(value)};
						}
						return {};
					}
					case Opcode::Add32:
					case Opcode::Add64:
					case Opcode::EffectiveAddress32:
					case Opcode::Sub32:
					case Opcode::Sub64:
					{
						if (node.operand_count != 2)
							return {};
						const Bounded left = ResolveBoundedAtAnchor(definition->block,
							node.operands[0], base_gpr, anchor_block, depth + 1,
							control_node, anchor_control_node);
						const Bounded right = ResolveBoundedAtAnchor(definition->block,
							node.operands[1], base_gpr, anchor_block, depth + 1,
							control_node, anchor_control_node);
						const bool subtract = node.opcode == Opcode::Sub32 ||
							node.opcode == Opcode::Sub64;
						if (!left.valid || !right.valid ||
							(left.has_base && right.has_base) ||
							(subtract && right.has_base))
						{
							return {};
						}
						const s64 minimum = left.minimum +
							(subtract ? -right.maximum : right.minimum);
						const s64 maximum = left.maximum +
							(subtract ? -right.minimum : right.maximum);
						if (minimum < INT32_MIN || maximum > INT32_MAX ||
							minimum > maximum)
						{
							return {};
						}
						return {true, left.has_base || right.has_base,
							minimum, maximum,
							std::min(left.alignment, right.alignment)};
					}
					default:
						return {};
				}
			}

			Bounded ResolveBounded(u32 block, ValueId value, u32 base_gpr,
				u32 depth = 0) const
			{
				return ResolveBoundedAtAnchor(block, value, base_gpr, m_header,
					depth);
			}

			Bounded ResolveBoundedAcrossControlContexts(u32 block, ValueId value,
				u32 base_gpr) const
			{
				Bounded merged{};
				bool have_merged = false;
				for (u32 context = 0; context < m_control_nodes.size(); context++)
				{
					if (m_control_nodes[context].block != block)
						continue;
					const bool repeated = !m_backedge ||
						(context < m_control_loop_members.size() &&
						 m_control_loop_members[context]);
					const u32 anchor = repeated ? m_header_control_node :
						m_entry_control_node;
					if (anchor >= m_control_nodes.size())
						return {};
					const Bounded candidate = ResolveBoundedAtAnchor(block, value,
						base_gpr, m_control_nodes[anchor].block, 0, context, anchor);
					if (!candidate.valid || (have_merged &&
						candidate.has_base != merged.has_base))
					{
						return {};
					}
					if (!have_merged)
					{
						merged = candidate;
						have_merged = true;
					}
					else
					{
						merged.minimum = std::min(merged.minimum,
							candidate.minimum);
						merged.maximum = std::max(merged.maximum,
							candidate.maximum);
						merged.alignment = std::min(merged.alignment,
							candidate.alignment);
					}
				}
				return have_merged ? merged : Bounded{};
			}

			CompositeAffine ResolveCompositeAffine(u32 block, ValueId value,
				u32 invariant_base_gpr, u32 induction_gpr, u32 depth = 0) const
			{
				if (depth > 128 || invariant_base_gpr == 0 ||
					induction_gpr == 0 || invariant_base_gpr == induction_gpr)
				{
					return {};
				}
				const Definition* definition = Def(value);
				if (!definition)
					return {};
				const Node& node = *definition->node;
				if (node.opcode == Opcode::Parameter)
				{
					u32 gpr = 0;
					if (!ParameterGpr(*definition, &gpr))
						return {};
					if (definition->block == m_header)
					{
						if (gpr == invariant_base_gpr)
							return {true, true, false, 0, 0};
						if (gpr == induction_gpr)
							return {true, false, true, 1, 0};
						if (gpr == 0)
							return {true, false, false, 0, 0};
						return {};
					}
					u32 source = block;
					ValueId incoming = value;
					return FollowParameter(*definition, &source, &incoming) ?
						ResolveCompositeAffine(source, incoming,
							invariant_base_gpr, induction_gpr, depth + 1) :
						CompositeAffine{};
				}
				if (node.opcode == Opcode::ConstantI1 ||
					node.opcode == Opcode::ConstantI32 ||
					node.opcode == Opcode::ConstantI64 ||
					node.opcode == Opcode::ConstantAddress)
				{
					return {true, false, false, 0,
						static_cast<s32>(static_cast<u32>(node.literal))};
				}
				auto unary = [&]() {
					return node.operand_count >= 1 ?
						ResolveCompositeAffine(definition->block, node.operands[0],
							invariant_base_gpr, induction_gpr, depth + 1) :
						CompositeAffine{};
				};
				switch (node.opcode)
				{
					case Opcode::ExtractLow32:
					case Opcode::ExtractLow64:
					case Opcode::AddressFromI32:
					case Opcode::SignExtend32To64:
					case Opcode::ZeroExtend32To64:
						return unary();
					case Opcode::ReplaceLow64:
						return node.operand_count == 2 ?
							ResolveCompositeAffine(definition->block,
								node.operands[1], invariant_base_gpr,
								induction_gpr, depth + 1) : CompositeAffine{};
					case Opcode::ShiftLeft32:
					{
						CompositeAffine input = unary();
						if (!input.valid || input.has_invariant_base ||
							!input.has_induction || input.offset != 0 ||
							node.immediate >= 32 || input.induction_scale == 0 ||
							input.induction_scale > (UINT32_MAX >> node.immediate))
						{
							return {};
						}
						input.induction_scale <<= node.immediate;
						return input;
					}
					case Opcode::Add32:
					case Opcode::Add64:
					case Opcode::EffectiveAddress32:
					case Opcode::Sub32:
					case Opcode::Sub64:
					{
						if (node.operand_count != 2)
							return {};
						const CompositeAffine left = ResolveCompositeAffine(
							definition->block, node.operands[0], invariant_base_gpr,
							induction_gpr, depth + 1);
						const CompositeAffine right = ResolveCompositeAffine(
							definition->block, node.operands[1], invariant_base_gpr,
							induction_gpr, depth + 1);
						const bool subtract = node.opcode == Opcode::Sub32 ||
							node.opcode == Opcode::Sub64;
						if (!left.valid || !right.valid ||
							(left.has_invariant_base && right.has_invariant_base) ||
							(left.has_induction && right.has_induction) ||
							(subtract && (right.has_invariant_base ||
								right.has_induction)))
						{
							return {};
						}
						const s64 offset = left.offset +
							(subtract ? -right.offset : right.offset);
						if (offset < INT32_MIN || offset > INT32_MAX)
							return {};
						return {true,
							left.has_invariant_base || right.has_invariant_base,
							left.has_induction || right.has_induction,
							left.has_induction ? left.induction_scale :
								right.induction_scale,
							offset};
					}
					default:
						return {};
				}
			}

			void CollectCompositeCountedRanges(Plan* plan) const
			{
				if (!plan || !plan->control.valid || plan->control.counter_gpr == 0 ||
					plan->control.counter_stride <= 0)
				{
					return;
				}
				u32 entry_induction = 0;
				s32 entry_induction_offset = 0;
				if (!HeaderGprAffineEntry(plan->control.counter_gpr,
						&entry_induction, &entry_induction_offset))
				{
					return;
				}
				std::vector<ValueId> covered;
				for (const CountedRange& range : plan->counted_ranges)
					for (const Access& access : range.accesses)
						covered.push_back(access.operation);

				std::vector<CountedRange> ranges;
				for (u32 block_index = 0; block_index < m_program.blocks.size(); block_index++)
				{
					if (m_backedge && !IsLoopBlock(block_index))
						continue;
					for (const Node& node : m_program.blocks[block_index].nodes)
					{
						if ((node.opcode != Opcode::MemoryLoad &&
							 node.opcode != Opcode::MemoryStore) ||
							node.operand_count != 3 ||
							std::find(covered.begin(), covered.end(), node.id) !=
								covered.end() ||
							node.immediate >
								static_cast<u32>(MemoryAccessKind::StoreVu0Vector))
						{
							continue;
						}
						const MemoryAccessKind kind =
							static_cast<MemoryAccessKind>(node.immediate);
						const u32 width = MemoryAccessWidth(kind);
						const u32 access_alignment = IsQuadMemoryAccess(kind) ? 16 :
							MemoryAlignmentMask(kind) + 1;
						for (u32 base = 1; base < 32; base++)
						{
							if (base == plan->control.counter_gpr ||
								!HeaderGprInvariant(base) ||
								!HeaderGprMatchesEntry(base))
							{
								continue;
							}
							const CompositeAffine address = ResolveCompositeAffine(
								block_index, node.operands[1], base,
								plan->control.counter_gpr);
							if (!address.valid || !address.has_invariant_base ||
								!address.has_induction || address.induction_scale == 0 ||
								(address.induction_scale &
									(address.induction_scale - 1)) != 0 ||
								address.offset < 0 || address.offset > INT32_MAX ||
								width == 0 ||
								(static_cast<u64>(address.offset) &
									(access_alignment - 1)) != 0)
							{
								continue;
							}
							const u64 stride = static_cast<u64>(
								plan->control.counter_stride) * address.induction_scale;
							if (stride == 0 || stride > UINT32_MAX ||
								(stride & (access_alignment - 1)) != 0)
							{
								continue;
							}
							u8 shift = 0;
							for (u32 scale = address.induction_scale; scale > 1;
								scale >>= 1)
							{
								shift++;
							}
							auto found = std::find_if(ranges.begin(), ranges.end(),
								[&](const CountedRange& range) {
									return range.invariant_base_gpr == base &&
										range.induction_gpr == plan->control.counter_gpr &&
										range.induction_scale_shift == shift;
								});
							if (found == ranges.end())
							{
								CountedRange range{};
								range.valid = true;
								range.header_block = m_header;
								range.entry_induction_gpr =
									static_cast<u8>(entry_induction);
								range.entry_induction_offset =
									entry_induction_offset;
								range.invariant_base_gpr = static_cast<u8>(base);
								range.induction_gpr = plan->control.counter_gpr;
								range.induction_scale_shift = shift;
								range.stride = static_cast<u32>(stride);
								range.alignment = access_alignment;
								range.minimum_offset = static_cast<s32>(address.offset);
								range.maximum_offset_end = static_cast<s32>(
									address.offset + width);
								range.accesses.push_back({node.id, block_index,
									static_cast<s32>(address.offset), width,
									node.opcode == Opcode::MemoryStore});
								ranges.push_back(std::move(range));
							}
							else
							{
								found->alignment = std::max(found->alignment,
									access_alignment);
								found->minimum_offset = std::min(found->minimum_offset,
									static_cast<s32>(address.offset));
								found->maximum_offset_end = std::max(
									found->maximum_offset_end,
									static_cast<s32>(address.offset + width));
								found->accesses.push_back({node.id, block_index,
									static_cast<s32>(address.offset), width,
									node.opcode == Opcode::MemoryStore});
							}
							break;
						}
					}
				}
				for (CountedRange& range : ranges)
					plan->counted_ranges.push_back(std::move(range));
			}

			bool HeaderGprInvariant(u32 gpr) const
			{
				if (gpr == 0 || gpr >= 32)
					return false;
				// With no backedge, ResolveBounded() expresses every access directly
				// relative to the canonical entry parameter.  There is no later
				// invocation inside this unit across which the base could change.
				if (!m_backedge)
					return true;
				if (!m_program.direct_calls.empty())
				{
					if (m_backedge_control_source >= m_control_nodes.size() ||
						m_header_control_node >= m_control_nodes.size())
					{
						return false;
					}
					const Bounded recurrence = ResolveBoundedAtAnchor(
						m_backedge_source, m_backedge->state.gpr[gpr], gpr,
						m_header, 0, m_backedge_control_source,
						m_header_control_node);
					return recurrence.valid && recurrence.has_base &&
						recurrence.minimum == 0 && recurrence.maximum == 0 &&
						HeaderGprMatchesEntry(gpr);
				}
				const Affine recurrence = ResolveAffine(m_backedge_source,
					m_backedge->state.gpr[gpr], gpr);
				return recurrence.valid && recurrence.has_base && recurrence.offset == 0 &&
					HeaderGprMatchesEntry(gpr);
			}

			void CollectBoundedRanges(Plan* plan) const
			{
				if (!plan)
					return;
				if (plan->timing.valid)
					CollectCompositeCountedRanges(plan);
				std::vector<ValueId> covered;
				for (const CountedRange& range : plan->counted_ranges)
				{
					for (const Access& access : range.accesses)
						covered.push_back(access.operation);
				}
				std::array<BoundedRange, 32> ranges{};
				for (u32 block_index = 0; block_index < m_program.blocks.size();
					block_index++)
				{
					const bool repeated_block = !m_backedge || IsLoopBlock(block_index);
					// Loop accesses are expressed relative to the natural-loop header.
					// One-time prefix/suffix accesses instead have to be reducible all the
					// way to a canonical region-entry GPR, because the range guard runs
					// before that code. This admits invariant setup loads without assuming
					// that a callee's argument register is already live at region entry.
					const u32 range_anchor = repeated_block ? m_header :
						m_program.entry_block;
					for (const Node& node : m_program.blocks[block_index].nodes)
					{
						if ((node.opcode != Opcode::MemoryLoad &&
							 node.opcode != Opcode::MemoryStore) ||
							node.operand_count != 3 ||
							std::find(covered.begin(), covered.end(), node.id) !=
								covered.end() ||
							node.immediate >
								static_cast<u32>(MemoryAccessKind::StoreVu0Vector))
						{
							continue;
						}
						const MemoryAccessKind kind =
							static_cast<MemoryAccessKind>(node.immediate);
						const u32 width = MemoryAccessWidth(kind);
						if (width == 0)
							continue;
						// LQ/SQ mask the guest effective address instead of raising an
						// alignment exception. Bypassing the per-access lowering is still
						// legal only when that mask is an identity for every bounded value.
						const u32 access_alignment = IsQuadMemoryAccess(kind) ? 16 :
							MemoryAlignmentMask(kind) + 1;
						// A LUI/add-immediate address is a semantic constant, not a
						// dynamic GPR range. Record base-free reads independently so the
						// persistent identity backend can bypass a VTLB lookup without
						// pretending an architectural register owns the address. Stores
						// retain the ordinary source-ownership path until an absolute-range
						// SMC guard is represented explicitly.
						const Bounded absolute = m_program.direct_calls.empty() ?
							ResolveBoundedAtAnchor(block_index, node.operands[1], 0,
								range_anchor) :
							ResolveBoundedAcrossControlContexts(block_index,
								node.operands[1], 0);
						if (repeated_block && node.opcode == Opcode::MemoryLoad &&
							absolute.valid &&
							!absolute.has_base && absolute.minimum >= 0 &&
							absolute.maximum == absolute.minimum &&
							absolute.maximum <= INT32_MAX - width &&
							absolute.alignment >= access_alignment)
						{
							BoundedRange range{};
							range.valid = true;
							range.header_block = m_header;
							range.absolute_address = true;
							range.alignment = access_alignment;
							range.minimum_offset = static_cast<s32>(absolute.minimum);
							range.maximum_offset_end =
								static_cast<s32>(absolute.maximum + width);
							range.accesses.push_back({node.id, block_index,
								static_cast<s32>(absolute.minimum), width, false});
							plan->bounded_ranges.push_back(std::move(range));
							continue;
						}
						u32 selected_base = 0;
						Bounded selected{};
						for (u32 base = 1; base < 32; base++)
						{
							if (repeated_block && !HeaderGprInvariant(base))
								continue;
							const Bounded candidate = m_program.direct_calls.empty() ?
								ResolveBoundedAtAnchor(block_index, node.operands[1], base,
									range_anchor) :
								ResolveBoundedAcrossControlContexts(block_index,
									node.operands[1], base);
							if (!candidate.valid || !candidate.has_base ||
								candidate.maximum > INT32_MAX - width ||
								candidate.alignment < access_alignment)
							{
								continue;
							}
							if (selected_base != 0)
							{
								selected_base = 0;
								break;
							}
							selected_base = base;
							selected = candidate;
						}
						if (selected_base == 0)
							continue;
						BoundedRange& range = ranges[selected_base];
						if (!range.valid)
						{
							range.valid = true;
							range.header_block = m_header;
							range.absolute_address = false;
							range.base_gpr = static_cast<u8>(selected_base);
							range.alignment = access_alignment;
							range.minimum_offset =
								static_cast<s32>(selected.minimum);
							range.maximum_offset_end =
								static_cast<s32>(selected.maximum + width);
						}
						else
						{
							range.alignment = std::max(range.alignment,
								access_alignment);
							range.minimum_offset = std::min(range.minimum_offset,
								static_cast<s32>(selected.minimum));
							range.maximum_offset_end = std::max(
								range.maximum_offset_end,
								static_cast<s32>(selected.maximum + width));
						}
						range.accesses.push_back({node.id, block_index,
							static_cast<s32>(selected.minimum), width,
							node.opcode == Opcode::MemoryStore});
					}
				}
				for (BoundedRange& range : ranges)
				{
					if (range.valid && !range.accesses.empty())
						plan->bounded_ranges.push_back(std::move(range));
				}
			}

			bool ResolveHeaderGpr(u32 block, ValueId value, u32* gpr,
				s64* offset) const
			{
				bool found = false;
				for (u32 candidate = 1; candidate < 32; candidate++)
				{
					const Affine affine = ResolveAffine(block, value, candidate);
					if (!affine.valid || !affine.has_base)
						continue;
					if (found)
						return false;
					found = true;
					*gpr = candidate;
					*offset = affine.offset;
				}
				return found;
			}

			const Definition* StripLowValue(const Definition* definition,
				u32 depth = 0) const
			{
				if (!definition || depth > 128)
					return nullptr;
				const Node& node = *definition->node;
				if (node.opcode == Opcode::Parameter && definition->block != m_header)
				{
					u32 source = definition->block;
					ValueId incoming = node.id;
					return FollowParameter(*definition, &source, &incoming) ?
						StripLowValue(Def(incoming), depth + 1) : nullptr;
				}
				if ((node.opcode == Opcode::ReplaceLow64 && node.operand_count == 2))
					return StripLowValue(Def(node.operands[1]), depth + 1);
				if ((node.opcode == Opcode::SignExtend32To64 ||
						node.opcode == Opcode::ZeroExtend32To64 ||
						node.opcode == Opcode::ExtractLow32 ||
						node.opcode == Opcode::ExtractLow64) &&
					node.operand_count == 1)
				{
					return StripLowValue(Def(node.operands[0]), depth + 1);
				}
				return definition;
			}

			bool IsZeroValue(const Definition* definition) const
			{
				definition = StripLowValue(definition);
				if (!definition)
					return false;
				const Node& node = *definition->node;
				if ((node.opcode == Opcode::ConstantI1 ||
						node.opcode == Opcode::ConstantI32 ||
						node.opcode == Opcode::ConstantI64) &&
					node.literal == 0)
				{
					return true;
				}
				u32 gpr = 0;
				return node.opcode == Opcode::Parameter &&
					definition->block == m_header &&
					ParameterGpr(*definition, &gpr) && gpr == 0;
			}

			bool MatchInductionAndBound(u32 block, ValueId induction_value,
				ValueId bound_value, u32 expected_induction, s64 expected_offset,
				u32* bound) const
			{
				u32 induction_gpr = 0;
				u32 bound_gpr = 0;
				s64 induction_offset = 0;
				s64 bound_offset = 0;
				if (!ResolveHeaderGpr(block, induction_value, &induction_gpr,
						&induction_offset) ||
					!ResolveHeaderGpr(block, bound_value, &bound_gpr, &bound_offset) ||
					induction_gpr != expected_induction ||
					induction_offset != expected_offset || bound_offset != 0 ||
					bound_gpr == expected_induction)
				{
					return false;
				}
				*bound = bound_gpr;
				return true;
			}

			bool MatchInductionAndImmediateBound(u32 block,
				ValueId induction_value, ValueId bound_value,
				u32 expected_induction, s64 expected_offset,
				bool signed_compare, u32* bound) const
			{
				if (!bound)
					return false;
				u32 induction_gpr = 0;
				s64 induction_offset = 0;
				if (!ResolveHeaderGpr(block, induction_value, &induction_gpr,
						&induction_offset) || induction_gpr != expected_induction ||
					induction_offset != expected_offset)
				{
					return false;
				}
				const Definition* immediate = StripLowValue(Def(bound_value));
				if (!immediate ||
					(immediate->node->opcode != Opcode::ConstantI32 &&
					 immediate->node->opcode != Opcode::ConstantI64))
				{
					return false;
				}
				const u64 literal = immediate->node->literal;
				if (literal > UINT32_MAX ||
					(signed_compare && literal > static_cast<u32>(INT32_MAX)))
				{
					return false;
				}
				*bound = static_cast<u32>(literal);
				return true;
			}

			bool MatchContinuation(u32 induction, u32 stride, u32* bound,
				bool* bound_is_immediate, TerminationKind* termination,
				u8* trip_count_adjustment,
				bool* signed_counter_compare) const
			{
				if (!bound || !bound_is_immediate || !termination ||
					!trip_count_adjustment ||
					!signed_counter_compare)
					return false;
				*bound_is_immediate = false;
				*trip_count_adjustment = 0;
				*signed_counter_compare = false;
				const Block& block = m_program.blocks[m_backedge_source];
				const Definition* condition = Def(block.terminator.condition);
				if (!condition || condition->node->opcode != Opcode::CompareNotEqual64 ||
					condition->node->operand_count != 2)
				{
					return false;
				}
				const ValueId left = condition->node->operands[0];
				const ValueId right = condition->node->operands[1];
				const Definition* predicate = nullptr;
				if (IsZeroValue(Def(left)))
					predicate = StripLowValue(Def(right));
				else if (IsZeroValue(Def(right)))
					predicate = StripLowValue(Def(left));
				if (predicate &&
					(predicate->node->opcode == Opcode::CompareUnsignedLess64 ||
					 predicate->node->opcode == Opcode::CompareSignedLess64) &&
					predicate->node->operand_count == 2)
				{
					const bool dynamic_bound = MatchInductionAndBound(predicate->block,
							predicate->node->operands[0], predicate->node->operands[1],
							induction, 0, bound) ||
						MatchInductionAndBound(predicate->block,
							predicate->node->operands[0], predicate->node->operands[1],
							induction, stride, bound);
					const bool immediate_bound = !dynamic_bound &&
						(MatchInductionAndImmediateBound(predicate->block,
							predicate->node->operands[0], predicate->node->operands[1],
							induction, 0,
							predicate->node->opcode == Opcode::CompareSignedLess64,
							bound) ||
						MatchInductionAndImmediateBound(predicate->block,
							predicate->node->operands[0], predicate->node->operands[1],
							induction, stride,
							predicate->node->opcode == Opcode::CompareSignedLess64,
							bound));
					if (dynamic_bound || immediate_bound)
					{
						*bound_is_immediate = immediate_bound;
						*termination = TerminationKind::UnsignedLess;
						*signed_counter_compare =
							predicate->node->opcode == Opcode::CompareSignedLess64;
						s64 observed_offset = 0;
						u32 observed_gpr = 0;
						if (!ResolveHeaderGpr(predicate->block,
								predicate->node->operands[0], &observed_gpr,
								&observed_offset) || observed_gpr != induction)
						{
							return false;
						}
						*trip_count_adjustment = observed_offset == 0 ? 1 : 0;
						return true;
					}
					return false;
				}

				if (MatchInductionAndBound(condition->block, left, right,
						induction, stride, bound) ||
					MatchInductionAndBound(condition->block, right, left,
						induction, stride, bound))
				{
					*termination = TerminationKind::EqualEndpoint;
					return true;
				}
				return false;
			}

			bool MatchDecrementToZero(u32 counter, s64 stride) const
			{
				const Block& block = m_program.blocks[m_backedge_source];
				const Definition* condition = Def(block.terminator.condition);
				if (!condition || condition->node->opcode != Opcode::CompareNotEqual64 ||
					condition->node->operand_count != 2)
				{
					return false;
				}
				const ValueId left = condition->node->operands[0];
				const ValueId right = condition->node->operands[1];
				const Definition* recurrence = nullptr;
				if (IsZeroValue(Def(left)))
					recurrence = Def(right);
				else if (IsZeroValue(Def(right)))
					recurrence = Def(left);
				if (!recurrence)
					return false;
				const Affine affine = ResolveAffine(recurrence->block,
					recurrence->node->id, counter);
				return affine.valid && affine.has_base && affine.offset == stride;
			}

			bool MatchDecrementWhileNonNegative(u32 counter, s64 stride) const
			{
				const Block& block = m_program.blocks[m_backedge_source];
				const Definition* condition = Def(block.terminator.condition);
				if (!condition ||
					condition->node->opcode != Opcode::CompareSignedGreaterEqualZero64 ||
					condition->node->operand_count != 1)
				{
					return false;
				}
				const Affine affine = ResolveAffine(condition->block,
					condition->node->operands[0], counter);
				return affine.valid && affine.has_base && affine.offset == stride;
			}

			bool MatchDecrementWhilePositive(u32 counter, s64 stride) const
			{
				const Block& block = m_program.blocks[m_backedge_source];
				const Definition* condition = Def(block.terminator.condition);
				if (!condition ||
					condition->node->opcode != Opcode::CompareSignedGreaterZero64 ||
					condition->node->operand_count != 1)
				{
					return false;
				}
				const Affine affine = ResolveAffine(condition->block,
					condition->node->operands[0], counter);
				return affine.valid && affine.has_base && affine.offset == stride;
			}

			bool CollectAccesses(u32 induction, u32 stride,
				CountedRange* range) const
			{
				*range = {};
				range->header_block = m_header;
				range->induction_gpr = static_cast<u8>(induction);
				range->stride = stride;
				s64 minimum = INT64_MAX;
				s64 maximum_end = 0;
				u32 alignment = 1;
				for (u32 block_index = 0; block_index < m_program.blocks.size(); block_index++)
				{
					if (m_backedge && !IsLoopBlock(block_index))
						continue;
					for (const Node& node : m_program.blocks[block_index].nodes)
					{
						if (node.opcode != Opcode::MemoryLoad &&
							node.opcode != Opcode::MemoryStore)
						{
							continue;
						}
						if (node.operand_count != 3 ||
							node.immediate >
								static_cast<u32>(MemoryAccessKind::StoreVu0Vector))
						{
							return false;
						}
						const MemoryAccessKind kind =
							static_cast<MemoryAccessKind>(node.immediate);
						const Affine address = ResolveAffine(block_index,
							node.operands[1], induction);
						const u32 width = MemoryAccessWidth(kind);
						// LQ/SQ normally mask the effective address. A range proof may bypass
						// that per-access operation only when entry, offset, and recurrence
						// establish that masking is an identity for every iteration.
						const u32 access_alignment = IsQuadMemoryAccess(kind) ? 16 :
							MemoryAlignmentMask(kind) + 1;
						if (!address.valid || !address.has_base || width == 0 ||
							address.offset < 0 || address.offset > INT32_MAX ||
							(static_cast<u64>(address.offset) &
							 (access_alignment - 1)) != 0 ||
							(stride & (access_alignment - 1)) != 0)
						{
							continue;
						}
						range->accesses.push_back({node.id, block_index,
							static_cast<s32>(address.offset), width,
							node.opcode == Opcode::MemoryStore});
						minimum = std::min(minimum, address.offset);
						maximum_end = std::max(maximum_end,
							address.offset + static_cast<s64>(width));
						alignment = std::max(alignment, access_alignment);
					}
				}
				if (range->accesses.empty() || minimum < 0 ||
					maximum_end <= 0 || maximum_end > INT32_MAX)
				{
					return false;
				}
				range->minimum_offset = static_cast<s32>(minimum);
				range->maximum_offset_end = static_cast<s32>(maximum_end);
				range->alignment = alignment;
				range->valid = true;
				return true;
			}

			bool TransferCycleCost(u32 block_index, const Transfer& transfer,
				u32* cycles) const
			{
				if (!cycles || block_index >= m_program.blocks.size())
					return false;
				const Block& block = m_program.blocks[block_index];
				if (transfer.state.cycle == block.parameters.cycle)
				{
					*cycles = 0;
					return true;
				}
				const Definition* advanced = Def(transfer.state.cycle);
				if (!advanced || advanced->block != block_index ||
					advanced->node->opcode != Opcode::AdvanceCycles ||
					advanced->node->operand_count != 1 ||
					advanced->node->operands[0] != block.parameters.cycle)
				{
					return false;
				}
				*cycles = advanced->node->immediate;
				return true;
			}

			bool MaximumPrefixCycles(u32* maximum) const
			{
				if (!maximum || m_entry_control_node >= m_control_nodes.size() ||
					m_header_control_node >= m_control_nodes.size())
				{
					return false;
				}
				if (m_entry_control_node == m_header_control_node)
				{
					*maximum = 0;
					return true;
				}
				std::vector<u8> visiting(m_control_nodes.size(), 0);
				std::vector<u8> known(m_control_nodes.size(), 0);
				std::vector<u64> memo(m_control_nodes.size(), 0);
				auto path = [&](auto&& self, u32 node_index, u64* result) -> bool {
					if (node_index == m_header_control_node)
					{
						*result = 0;
						return true;
					}
					if (node_index >= m_control_nodes.size() || visiting[node_index])
						return false;
					if (known[node_index])
					{
						*result = memo[node_index];
						return true;
					}
					visiting[node_index] = 1;
					u64 best = 0;
					bool any = false;
					auto edge = [&](const ControlEdge& transfer) {
						u32 edge_cycles = 0;
						// A profitability certificate for an internal loop is valid only
						// when every region-entry path reaches that loop. A preheader side
						// exit remains semantically supported, but it cannot borrow the
						// repeated work of a loop it may skip.
						if (transfer.target_node == INVALID_BLOCK || !transfer.transfer ||
							!TransferCycleCost(transfer.state_source_block,
								*transfer.transfer, &edge_cycles))
							return false;
						u64 suffix = 0;
						if (!self(self, transfer.target_node, &suffix))
						{
							return false;
						}
						const u64 total = static_cast<u64>(edge_cycles) + suffix;
						best = any ? std::max(best, total) : total;
						any = true;
						return true;
					};
					bool edges_ok = true;
					for (const ControlEdge& target :
						m_control_nodes[node_index].successors)
						edges_ok &= edge(target);
					if (!edges_ok)
					{
						visiting[node_index] = 0;
						return false;
					}
					visiting[node_index] = 0;
					known[node_index] = 1;
					memo[node_index] = best;
					*result = best;
					return any;
				};
				u64 result = 0;
				if (!path(path, m_entry_control_node, &result) || result > UINT32_MAX)
					return false;
				*maximum = static_cast<u32>(result);
				return true;
			}

			bool MaximumIterationCycles(u32* maximum) const
			{
				if (!maximum || m_header_control_node >= m_control_nodes.size())
					return false;
				std::vector<u8> visiting(m_control_nodes.size(), 0);
				std::vector<u8> known(m_control_nodes.size(), 0);
				std::vector<u64> memo(m_control_nodes.size(), 0);
				auto path = [&](auto&& self, u32 node_index, u64* result) -> bool {
					if (node_index >= m_control_nodes.size() || visiting[node_index])
						return false;
					if (known[node_index])
					{
						*result = memo[node_index];
						return true;
					}
					visiting[node_index] = 1;
					u64 best = 0;
					bool any = false;
					auto edge = [&](const ControlEdge& transfer) {
						u32 edge_cycles = 0;
						if (!transfer.transfer ||
							!TransferCycleCost(transfer.state_source_block,
								*transfer.transfer, &edge_cycles))
							return false;
						u64 suffix = 0;
						if (transfer.target_node != INVALID_BLOCK &&
							!(node_index == m_backedge_control_source &&
								transfer.target_node == m_header_control_node) &&
							!self(self, transfer.target_node, &suffix))
						{
							return false;
						}
						const u64 total = static_cast<u64>(edge_cycles) + suffix;
						best = any ? std::max(best, total) : total;
						any = true;
						return true;
					};
					bool edges_ok = true;
					for (const ControlEdge& target :
						m_control_nodes[node_index].successors)
						edges_ok &= edge(target);
					if (!edges_ok)
					{
						visiting[node_index] = 0;
						return false;
					}
					visiting[node_index] = 0;
					known[node_index] = 1;
					memo[node_index] = best;
					*result = best;
					return any;
				};
				u64 result = 0;
				if (!path(path, m_header_control_node, &result) || result == 0 ||
					result > UINT32_MAX)
				{
					return false;
				}
				*maximum = static_cast<u32>(result);
				return true;
			}

			const Program& m_program;
			u32 m_header = INVALID_BLOCK;
			u32 m_entry_control_node = INVALID_BLOCK;
			u32 m_header_control_node = INVALID_BLOCK;
			u32 m_backedge_count = 0;
			u32 m_backedge_source = INVALID_BLOCK;
			u32 m_backedge_control_source = INVALID_BLOCK;
			u8 m_control_reducibility_failure = 0;
			const Transfer* m_backedge = nullptr;
			std::vector<u8> m_loop_members;
			std::vector<u8> m_control_loop_members;
			std::vector<ControlNode> m_control_nodes;
			std::vector<Definition> m_definitions;
			std::vector<std::vector<Incoming>> m_incoming;
			std::vector<std::vector<u32>> m_successors;
		};
	} // namespace

	A9ProfitabilityCertificate BuildA9ProfitabilityCertificate(
		const Program& program, const Plan& plan,
		u32 minimum_counted_memory_work,
		u32 minimum_observed_region_work,
		bool use_measured_scalar_floor,
		u32 minimum_read_only_iterations,
		u32 minimum_observed_vu0_fmac_iterations,
		u32 minimum_observed_vu0_fmac_leaf_invocations)
	{
		A9ProfitabilityCertificate result{};
		const u32 header = plan.control.valid ? plan.control.header_block :
			plan.timing.header_block;
		auto add_access = [&](const Access& access) {
			if (access.block != header)
				return;
			result.work_per_iteration += std::max<u32>(
				1, (access.width + sizeof(u32) - 1) / sizeof(u32));
			result.has_unconditional_store |= access.store;
			result.has_header_vector_access |= access.width == 16;
		};
		for (const CountedRange& range : plan.counted_ranges)
		{
			for (const Access& access : range.accesses)
				add_access(access);
		}
		for (const BoundedRange& range : plan.bounded_ranges)
		{
			for (const Access& access : range.accesses)
				add_access(access);
		}

		auto build_observed_direct_call = [&]() {
			A9ProfitabilityCertificate observed{};
			// Direct calls inside one verified natural-loop iteration are exactly the
			// kind of boundaries a unit compiler can remove but a basic-block compiler
			// cannot. Admit only a read-only repeated path with an exact event-cycle
			// bound, require every attested call to lie on that path, and publish only
			// after the generated latch probe has observed real traversals. This is a
			// target cost certificate, not a wait-loop semantic shortcut: the region
			// still executes every instruction and its ordinary event guard remains
			// authoritative.
			if (!plan.timing.valid || program.direct_calls.empty() ||
				plan.timing.header_block >= program.blocks.size() ||
				plan.timing.backedge_source_block >= program.blocks.size() ||
				minimum_observed_region_work == 0)
			{
				return observed;
			}

			std::vector<u8> repeated_path(program.blocks.size(), 0);
			u32 current = plan.timing.backedge_source_block;
			for (u32 depth = 0; depth < program.blocks.size(); depth++)
			{
				if (current >= program.blocks.size() || repeated_path[current])
					return observed;
				repeated_path[current] = 1;
				if (current == plan.timing.header_block)
					break;

				u32 predecessor = INVALID_BLOCK;
				for (u32 source = 0; source < program.blocks.size(); source++)
				{
					const Terminator& terminator = program.blocks[source].terminator;
					(void)VisitInternalTransfers(terminator,
						[&](const Transfer& edge, u8) {
						if (edge.target_block != current ||
							(source == plan.timing.backedge_source_block &&
							 current == plan.timing.header_block))
						{
							return true;
						}
						if (predecessor != INVALID_BLOCK && predecessor != source)
							predecessor = UINT32_MAX - 1;
						else
							predecessor = source;
						return predecessor != UINT32_MAX - 1;
						});
					if (predecessor == UINT32_MAX - 1)
						return observed;
				}
				if (predecessor == INVALID_BLOCK)
					return observed;
				current = predecessor;
			}
			if (!repeated_path[plan.timing.header_block])
				return observed;

			u32 source_instructions = 0;
			u32 repeated_blocks = 0;
			u32 memory_loads = 0;
			u32 covered_scalar_loads = 0;
			bool memory_store = false;
			bool uncovered_or_wide_load = false;
			u32 direct_calls_on_path = 0;
			for (u32 block_index = 0; block_index < program.blocks.size(); block_index++)
			{
				if (!repeated_path[block_index])
					continue;
				repeated_blocks++;
				const Block& block = program.blocks[block_index];
				source_instructions += static_cast<u32>(block.source.size());
				for (const SourceInstruction& source : block.source)
				{
					direct_calls_on_path += static_cast<u32>(std::count_if(
						program.direct_calls.begin(), program.direct_calls.end(),
						[&](const DirectCallContract& call) {
							return source.pc == call.call_pc;
						}));
				}
				for (const Node& node : block.nodes)
				{
					if (node.opcode == Opcode::MemoryLoad)
					{
						memory_loads++;
						const auto covered = [&](const auto& ranges) {
							return std::any_of(ranges.begin(), ranges.end(),
								[&](const auto& range) {
									return std::any_of(range.accesses.begin(),
										range.accesses.end(), [&](const Access& access) {
											return access.operation == node.id &&
												!access.store && access.width <= sizeof(u32);
										});
								});
						};
						if (covered(plan.counted_ranges) || covered(plan.bounded_ranges))
							covered_scalar_loads++;
						else
							uncovered_or_wide_load = true;
					}
					memory_store |= node.opcode == Opcode::MemoryStore;
				}
			}
			// The call boundaries are the structural source of savings. Do not turn
			// measured block/instruction/load counts into an admission whitelist. The
			// full RegionIR verifier and final A32 emission own support and resource
			// limits; this proof merely requires a side-effect-free repeated path whose
			// fallible reads have all moved to the entry preflight.
			if (direct_calls_on_path != program.direct_calls.size() ||
				source_instructions == 0 || memory_store ||
				uncovered_or_wide_load || covered_scalar_loads != memory_loads)
			{
				return observed;
			}
			observed.kind =
				A9ProfitabilityCertificate::Kind::ObservedBoundaryElision;
			observed.work_per_iteration = source_instructions;
			observed.repeated_blocks = repeated_blocks;
			observed.covered_scalar_loads = covered_scalar_loads;
			observed.minimum_profitable_iterations = std::max<u32>(1,
				static_cast<u32>((static_cast<u64>(minimum_observed_region_work) +
					source_instructions - 1) / source_instructions));
			return observed;
		};

		auto build_read_only_loop = [&]() {
			A9ProfitabilityCertificate read_only{};
			// This is a target-cost certificate, not a semantic shortcut. Every
			// guest instruction and every load still executes in order. Admission is
			// limited to the exact structural class measured through the first-class
			// dispatcher: a natural loop with no stores/calls/guarded observers and
			// only scalar reads whose translation checks are all moved to entry.
			if (!plan.timing.valid ||
				plan.timing.header_block != program.entry_block ||
				!program.direct_calls.empty() || minimum_read_only_iterations == 0)
			{
				return read_only;
			}

			u32 source_instructions = 0;
			u32 memory_loads = 0;
			u32 covered_scalar_loads = 0;
			u32 external_exits = 0;
			bool disallowed_effect = false;
			auto covered = [&](ValueId operation, u32 width) {
				if (width > sizeof(u32))
					return false;
				const auto range_covers = [&](const auto& ranges) {
					return std::any_of(ranges.begin(), ranges.end(),
						[&](const auto& range) {
							return std::any_of(range.accesses.begin(),
								range.accesses.end(), [&](const Access& access) {
									return access.operation == operation &&
										!access.store && access.width == width;
								});
						});
				};
				return range_covers(plan.counted_ranges) ||
					range_covers(plan.bounded_ranges);
			};
			for (const Block& block : program.blocks)
			{
				source_instructions += static_cast<u32>(block.source.size());
				disallowed_effect |= !block.guarded_exits.empty();
				const u32 block_index = static_cast<u32>(&block - program.blocks.data());
				auto count_external = [&](const Transfer& transfer) {
					if (transfer.target_block != INVALID_BLOCK)
						return;
					external_exits++;
					// A trip count is a lower-bound certificate only when the loop has
					// no earlier search/match exit. The sole external transfer must be
					// the counted latch's termination edge; otherwise the dynamic entry
					// bound can substantially overstate work actually executed.
					disallowed_effect |=
						block_index != plan.timing.backedge_source_block;
				};
				if (block.terminator.kind != TerminatorKind::RegisterJump ||
					block.terminator.register_targets.empty())
				{
					count_external(block.terminator.taken);
				}
				if (block.terminator.kind == TerminatorKind::Branch)
					count_external(block.terminator.not_taken);
				for (const Node& node : block.nodes)
				{
					if (node.opcode == Opcode::MemoryStore ||
						node.opcode == Opcode::ExitIfTrue ||
						node.opcode == Opcode::Vu0RequireIdle)
					{
						disallowed_effect = true;
					}
					if (node.opcode != Opcode::MemoryLoad)
						continue;
					memory_loads++;
					const u32 width = node.immediate <=
						static_cast<u32>(MemoryAccessKind::StoreVu0Vector) ?
						MemoryAccessWidth(static_cast<MemoryAccessKind>(node.immediate)) : 0;
					covered_scalar_loads += covered(node.id, width) ? 1u : 0u;
				}
			}
			if (disallowed_effect || external_exits != 1 ||
				source_instructions == 0 || memory_loads == 0 ||
				covered_scalar_loads != memory_loads)
			{
				return read_only;
			}

			read_only.kind = A9ProfitabilityCertificate::Kind::ReadOnlyLoop;
			read_only.work_per_iteration = source_instructions;
			read_only.repeated_blocks = static_cast<u32>(program.blocks.size());
			read_only.covered_scalar_loads = covered_scalar_loads;
			read_only.minimum_profitable_iterations = minimum_read_only_iterations;
			if (plan.control.valid && plan.control.bound_is_immediate &&
				plan.control.counter_stride > 0)
			{
				read_only.maximum_iterations_known = true;
				read_only.maximum_iterations =
					static_cast<u64>(plan.control.bound_immediate) /
						static_cast<u32>(plan.control.counter_stride) +
					plan.control.trip_count_adjustment;
			}
			return read_only;
		};

		auto build_observed_preflighted_loop = [&]() {
			A9ProfitabilityCertificate observed{};
			if (!plan.timing.valid ||
				plan.timing.header_block != program.entry_block ||
				plan.timing.header_block >= program.blocks.size() ||
				plan.timing.backedge_source_block >= program.blocks.size() ||
				!program.direct_calls.empty())
			{
				return observed;
			}

			// Recover the complete natural-loop body from the verified latch. Count
			// decoded work only in blocks which can feed that latch; acyclic side arms
			// may still execute, but cannot inflate the repeated-work certificate.
			std::vector<u8> repeated(program.blocks.size(), 0);
			std::vector<u32> worklist;
			repeated[plan.timing.header_block] = 1;
			repeated[plan.timing.backedge_source_block] = 1;
			worklist.push_back(plan.timing.backedge_source_block);
			while (!worklist.empty())
			{
				const u32 target = worklist.back();
				worklist.pop_back();
				if (target == plan.timing.header_block)
					continue;
				for (u32 source = 0; source < program.blocks.size(); source++)
				{
					const Terminator& terminator = program.blocks[source].terminator;
					const bool predecessor =
						TerminatorTargetsBlock(terminator, target);
					if (!predecessor || repeated[source])
						continue;
					repeated[source] = 1;
					worklist.push_back(source);
				}
			}

			u32 source_instructions = 0;
			u32 repeated_blocks = 0;
			u32 memory_accesses = 0;
			u32 covered_memory_accesses = 0;
			bool disallowed_effect = false;
			auto covered = [&](ValueId operation, bool store, u32 width) {
				if (width == 0 || width > sizeof(u32))
					return false;
				const auto range_covers = [&](const auto& ranges) {
					return std::any_of(ranges.begin(), ranges.end(),
						[&](const auto& range) {
							return std::any_of(range.accesses.begin(),
								range.accesses.end(), [&](const Access& access) {
									return access.operation == operation &&
										access.store == store && access.width == width;
								});
						});
				};
				return range_covers(plan.counted_ranges) ||
					range_covers(plan.bounded_ranges);
			};
			for (u32 block_index = 0; block_index < program.blocks.size(); block_index++)
			{
				const Block& block = program.blocks[block_index];
				if (repeated[block_index])
				{
					repeated_blocks++;
					source_instructions += static_cast<u32>(block.source.size());
				}
				disallowed_effect |= !block.guarded_exits.empty();
				for (const Node& node : block.nodes)
				{
					if (node.opcode == Opcode::ExitIfTrue ||
						node.opcode == Opcode::Vu0RequireIdle)
					{
						disallowed_effect = true;
					}
					const bool load = node.opcode == Opcode::MemoryLoad;
					const bool store = node.opcode == Opcode::MemoryStore;
					if (!load && !store)
						continue;
					memory_accesses++;
					const u32 width = node.immediate <=
						static_cast<u32>(MemoryAccessKind::StoreVu0Vector) ?
						MemoryAccessWidth(static_cast<MemoryAccessKind>(node.immediate)) : 0;
					covered_memory_accesses += covered(node.id, store, width) ? 1u : 0u;
				}
			}
			// This cost class is intentionally distinct from tiny wait/search loops:
			// it proves that at least three tier-zero owners and twelve decoded
			// instructions are fused, and that no mid-region memory fallback remains.
			if (disallowed_effect || repeated_blocks < 3 ||
				source_instructions < 12 || memory_accesses == 0 ||
				covered_memory_accesses != memory_accesses)
			{
				return observed;
			}

			observed.kind =
				A9ProfitabilityCertificate::Kind::ObservedPreflightedLoop;
			observed.work_per_iteration = source_instructions;
			observed.repeated_blocks = repeated_blocks;
			observed.covered_scalar_loads = covered_memory_accesses;
			observed.minimum_profitable_iterations = std::max<u32>(1,
				static_cast<u32>((static_cast<u64>(A9ProfitabilityCertificate::
					DEFAULT_MINIMUM_OBSERVED_PREFLIGHTED_WORK) +
					source_instructions - 1) / source_instructions));
			return observed;
		};

		auto build_observed_cop1_residency = [&]() {
			A9ProfitabilityCertificate observed{};
			// This is deliberately narrower than general COP1 support.  It certifies
			// the target-measured class whose ordinary path retains basic O/U
			// arithmetic in VFP registers and whose exceptional results enter exact
			// internal cold veneers which rejoin the same verified result/flag graph.
			// Comparisons, conversions, GPR/FPR transfers,
			// compound arithmetic, memory, VU0 and eager flag paths remain separate
			// cost classes even though the verifier/backend may already execute them.
			if (!program.options.cop1_lazy_ou_guards || !plan.timing.valid ||
				plan.timing.header_block != program.entry_block ||
				plan.timing.header_block >= program.blocks.size() ||
				plan.timing.backedge_source_block >= program.blocks.size() ||
				!program.direct_calls.empty())
			{
				return observed;
			}

			// Recover only blocks which can feed the verified latch.  Acyclic setup or
			// exit arms must not inflate the observed repeated-work certificate.
			std::vector<u8> repeated(program.blocks.size(), 0);
			std::vector<u32> worklist;
			repeated[plan.timing.header_block] = 1;
			repeated[plan.timing.backedge_source_block] = 1;
			worklist.push_back(plan.timing.backedge_source_block);
			while (!worklist.empty())
			{
				const u32 target = worklist.back();
				worklist.pop_back();
				if (target == plan.timing.header_block)
					continue;
				for (u32 source = 0; source < program.blocks.size(); source++)
				{
					const Terminator& terminator = program.blocks[source].terminator;
					const bool predecessor =
						TerminatorTargetsBlock(terminator, target);
					if (!predecessor || repeated[source])
						continue;
					repeated[source] = 1;
					worklist.push_back(source);
				}
			}

			std::vector<const Node*> definitions(program.value_count, nullptr);
			for (const Block& block : program.blocks)
			{
				for (const Node& node : block.nodes)
				{
					if (node.id >= definitions.size() || definitions[node.id])
						return observed;
					definitions[node.id] = &node;
				}
			}

			u32 source_instructions = 0;
			u32 repeated_blocks = 0;
			u32 cop1_blocks = 0;
			u32 raw_arithmetic = 0;
			u32 exceptional_predicates = 0;
			u32 clamp_results = 0;
			u32 flag_updates = 0;
			u32 guarded_nodes = 0;
			bool disallowed_effect = false;
			for (u32 block_index = 0; block_index < program.blocks.size(); block_index++)
			{
				const Block& block = program.blocks[block_index];
				bool block_has_cop1 = false;
				if (!block.observer_exits.empty() || !block.memory_exits.empty())
					disallowed_effect = true;
				disallowed_effect |= !block.guarded_exits.empty();
				if (repeated[block_index])
				{
					repeated_blocks++;
					source_instructions += static_cast<u32>(block.source.size());
				}
				for (const Node& node : block.nodes)
				{
					if (node.opcode == Opcode::MemoryLoad ||
						node.opcode == Opcode::MemoryStore ||
						(node.opcode >= Opcode::Vu0RequireIdle &&
						 node.opcode <= Opcode::Vu0DenormalizeStatus))
					{
						disallowed_effect = true;
					}

					switch (node.opcode)
					{
						case Opcode::BitcastI32ToF32Bits:
						case Opcode::BitcastF32BitsToI32:
						case Opcode::Cop1CompareEqual:
						case Opcode::Cop1CompareLess:
						case Opcode::Cop1CompareLessEqual:
						case Opcode::Cop1UpdateConditionFlag:
						case Opcode::Cop1BranchCondition:
						case Opcode::Cop1ConvertWord:
						case Opcode::Cop1ConvertSingle:
						case Opcode::BitcastI128ToVuF32x4Bits:
						case Opcode::BitcastVuF32x4BitsToI128:
						case Opcode::BindVu0Q:
						case Opcode::BindVu0ViQ:
						case Opcode::BindVu0Vf:
						case Opcode::BindVu0Acc:
						case Opcode::BindVu0MacFlag:
						case Opcode::BindVu0StatusFlag:
						case Opcode::BindVu0ViMac:
						case Opcode::BindVu0ViStatus:
						case Opcode::BindVu0Vi:
						case Opcode::BindVu0ClipFlag:
						case Opcode::BindVu0MicroStatusFlag:
							disallowed_effect = true;
							break;
						case Opcode::Cop1AddRaw:
						case Opcode::Cop1SubRaw:
						case Opcode::Cop1MulRaw:
							raw_arithmetic += repeated[block_index] ? 1u : 0u;
							block_has_cop1 |= repeated[block_index] != 0;
							break;
						case Opcode::Cop1ExceptionalOuResult:
							exceptional_predicates += repeated[block_index] ? 1u : 0u;
							block_has_cop1 |= repeated[block_index] != 0;
							break;
						case Opcode::Cop1ClampOuResult:
							clamp_results += repeated[block_index] ? 1u : 0u;
							break;
						case Opcode::Cop1UpdateOuFlags:
							flag_updates += repeated[block_index] ? 1u : 0u;
							break;
					case Opcode::ExitIfTrue:
						disallowed_effect = true;
						guarded_nodes++;
						break;
						default:
							break;
					}
				}
				if (block_has_cop1)
					cop1_blocks++;
			}

			// Equal raw/predicate/clamp/flag counts exclude compound two-stage
			// arithmetic until it has its own Cortex-A9 cost evidence.  Requiring at
			// least half of the decoded repeated instructions to be retained COP1
			// arithmetic keeps this certificate tied to the measured residency win.
			if (disallowed_effect || repeated_blocks < 3 || cop1_blocks < 2 ||
				source_instructions < 12 || raw_arithmetic < 4 ||
				raw_arithmetic != exceptional_predicates ||
				raw_arithmetic != clamp_results || raw_arithmetic != flag_updates ||
				guarded_nodes != 0 ||
				raw_arithmetic * 2u < source_instructions)
			{
				return observed;
			}

			observed.kind = A9ProfitabilityCertificate::Kind::ObservedCop1Residency;
			observed.work_per_iteration = source_instructions;
			observed.repeated_blocks = repeated_blocks;
			observed.minimum_profitable_iterations = std::max<u32>(1,
				static_cast<u32>((static_cast<u64>(A9ProfitabilityCertificate::
					DEFAULT_MINIMUM_OBSERVED_COP1_WORK) + source_instructions - 1) /
					source_instructions));
			return observed;
		};

		auto build_observed_vu0_fmac_stream = [&]() {
			A9ProfitabilityCertificate observed{};
			// This certificate names the measured complete macro-mode stream, not
			// general VU0 support: one entry-idle proof, affine LQC2/SQC2, and a
			// resident uncontracted VMULA/VMADDA/VMADD graph. A mechanically verified
			// direct leaf may be part of the repeated path: compiling its caller and
			// leaf as one unit is the measured Cortex-A9 class which removes the hot
			// JAL/JR dispatcher seam. Conversions, Q/control state, lane rotations,
			// scalar/COP1 memory, and uncovered accesses remain different target-cost
			// classes.
			const bool repeated_stream = plan.timing.valid && plan.control.valid &&
				plan.header_seeds_match_entry &&
				plan.timing.header_block < program.blocks.size() &&
				plan.timing.backedge_source_block < program.blocks.size();
			const bool acyclic_leaf = !plan.timing.valid && !plan.control.valid &&
				program.blocks.size() == 1 && program.entry_block == 0 &&
				program.direct_calls.empty() &&
				program.blocks[0].terminator.kind == TerminatorKind::RegisterJump &&
				program.blocks[0].terminator.taken.target_block == INVALID_BLOCK &&
				program.blocks[0].terminator.register_targets.empty() &&
				!program.blocks[0].terminator.taken.register_target_proven &&
				program.blocks[0].terminator.taken.external_reason ==
					ExitReason::RegionBoundary;
			// Semantic support is retained for verifier/backend fixtures, but a zero
			// target floor deliberately disables leaf-only product admission.  The
			// physical Vita comparison showed that materializing VF/ACC/flags at every
			// dynamic return erased the A9 fixture gain.  Enclosing caller/loop regions
			// remain eligible through the repeated-stream certificate below.
			if (acyclic_leaf && minimum_observed_vu0_fmac_leaf_invocations == 0)
			{
				observed.diagnostic_flags |=
					A9ProfitabilityCertificate::DiagnosticBelowMeasuredFloor;
				return observed;
			}
			if (!repeated_stream && !acyclic_leaf)
			{
				observed.diagnostic_flags |=
					A9ProfitabilityCertificate::DiagnosticUnsupportedShape;
				return observed;
			}

			std::vector<u8> repeated(program.blocks.size(), 0);
			if (acyclic_leaf)
			{
				repeated[0] = 1;
			}
			else
			{
				std::vector<u32> worklist;
				repeated[plan.timing.header_block] = 1;
				repeated[plan.timing.backedge_source_block] = 1;
				worklist.push_back(plan.timing.backedge_source_block);
				while (!worklist.empty())
				{
					const u32 target = worklist.back();
					worklist.pop_back();
					if (target == plan.timing.header_block)
						continue;
					for (u32 source = 0; source < program.blocks.size(); source++)
					{
						const Terminator& terminator =
							program.blocks[source].terminator;
						const bool predecessor =
							TerminatorTargetsBlock(terminator, target);
						if (!predecessor || repeated[source])
							continue;
						repeated[source] = 1;
						worklist.push_back(source);
					}
				}
			}

			auto append_unique = [](std::vector<u32>* values, u32 value) {
				if (std::find(values->begin(), values->end(), value) == values->end())
					values->push_back(value);
			};
			auto memory_covered = [&](ValueId operation, bool store, u32 width) {
				const auto range_covers = [&](const auto& ranges) {
					return std::any_of(ranges.begin(), ranges.end(),
						[&](const auto& range) {
							return std::any_of(range.accesses.begin(),
								range.accesses.end(), [&](const Access& access) {
									return access.operation == operation &&
										access.store == store && access.width == width;
								});
						});
				};
				return range_covers(plan.counted_ranges) ||
					range_covers(plan.bounded_ranges);
			};

			u32 source_instructions = 0;
			u32 repeated_blocks = 0;
			u32 idle_nodes = 0;
			u32 memory_accesses = 0;
			u32 covered_memory_accesses = 0;
			u32 vector_loads = 0;
			u32 vector_stores = 0;
			u32 direct_calls_on_path = 0;
			std::vector<u32> multiply_sources;
			std::vector<u32> add_sources;
			std::vector<u32> clamp_sources;
			u32 mac_nodes = 0;
			u32 status_nodes = 0;
			u32 result_bindings = 0;
			u32 cop1_blocks = 0;
			u32 cop1_raw_arithmetic = 0;
			u32 cop1_exceptional_predicates = 0;
			u32 cop1_clamp_results = 0;
			u32 cop1_flag_updates = 0;
			bool disallowed_effect = false;
			bool nonrepeated_vu0_or_memory = false;
			bool nonrepeated_cop1 = false;
			for (u32 block_index = 0; block_index < program.blocks.size(); block_index++)
			{
				const Block& block = program.blocks[block_index];
				const bool on_repeated_path = repeated[block_index] != 0;
				bool block_has_cop1 = false;
				if (repeated[block_index])
				{
					repeated_blocks++;
					source_instructions += static_cast<u32>(block.source.size());
					for (const SourceInstruction& source : block.source)
					{
						direct_calls_on_path += static_cast<u32>(std::count_if(
							program.direct_calls.begin(), program.direct_calls.end(),
							[&](const DirectCallContract& call) {
								return source.pc == call.call_pc;
							}));
					}
				}
				u32 block_idle_nodes = 0;
				for (const Node& node : block.nodes)
				{
					const bool vu0_semantic =
						node.opcode >= Opcode::Vu0RequireIdle &&
						node.opcode <= Opcode::Vu0DenormalizeStatus;
					const bool vu0_binding =
						node.opcode == Opcode::BindVu0Q ||
						node.opcode == Opcode::BindVu0ViQ ||
						(node.opcode >= Opcode::BindVu0Vf &&
						 node.opcode <= Opcode::BindVu0MicroStatusFlag);
					const bool nonrepeated_state = !on_repeated_path &&
						((vu0_semantic &&
						  node.opcode != Opcode::Vu0RequireIdle) || vu0_binding ||
						 node.opcode == Opcode::MemoryLoad ||
						 node.opcode == Opcode::MemoryStore);
					nonrepeated_vu0_or_memory |= nonrepeated_state;
					if (nonrepeated_state)
					{
						observed.diagnostic_flags |=
							A9ProfitabilityCertificate::DiagnosticNonRepeatedState;
					}
					if ((vu0_semantic || vu0_binding) && !on_repeated_path &&
						node.opcode != Opcode::Vu0RequireIdle &&
						node.opcode != Opcode::Vu0NormalizeVector &&
						node.opcode != Opcode::BindVu0Vf)
					{
						disallowed_effect = true;
					}

					switch (node.opcode)
					{
						case Opcode::Vu0RequireIdle:
							idle_nodes += repeated[block_index] ? 1u : 0u;
							block_idle_nodes++;
							break;
						case Opcode::Vu0NormalizeVector:
						case Opcode::Vu0BroadcastLane:
						case Opcode::Vu0MergeMasked:
						case Opcode::Vu0SyncStatusControl:
						case Opcode::BindVu0MacFlag:
						case Opcode::BindVu0StatusFlag:
						case Opcode::BindVu0ViMac:
						case Opcode::BindVu0ViStatus:
						case Opcode::BindVu0MicroStatusFlag:
							break;
						case Opcode::Vu0MulRaw:
							if (on_repeated_path)
								append_unique(&multiply_sources, node.source_pc);
							break;
						case Opcode::Vu0AddRaw:
							if (on_repeated_path)
								append_unique(&add_sources, node.source_pc);
							break;
						case Opcode::Vu0ClampFmacResult:
							if (on_repeated_path)
								append_unique(&clamp_sources, node.source_pc);
							break;
						case Opcode::Vu0MacFlagsFromRaw:
							mac_nodes += on_repeated_path ? 1u : 0u;
							break;
						case Opcode::Vu0StatusFlagsFromMac:
							status_nodes += on_repeated_path ? 1u : 0u;
							break;
						case Opcode::BindVu0Vf:
						case Opcode::BindVu0Acc:
							result_bindings += on_repeated_path ? 1u : 0u;
							break;
						case Opcode::Vu0ConvertFixed:
						case Opcode::Vu0ConvertIntegerToFloat:
						case Opcode::Vu0Rotate32:
						case Opcode::Vu0BroadcastScalar:
						case Opcode::Vu0FdivQ:
						case Opcode::Vu0FdivFlags:
						case Opcode::Vu0UpdateFdivStatus:
						case Opcode::Vu0SyncFdivStatusControl:
						case Opcode::Vu0SubRaw:
						case Opcode::Vu0ControlWrite:
						case Opcode::Vu0DenormalizeStatus:
							observed.diagnostic_flags |=
								A9ProfitabilityCertificate::
									DiagnosticUnsupportedVu0Semantic;
							disallowed_effect = true;
							break;
						case Opcode::BindVu0Q:
						case Opcode::BindVu0ViQ:
						case Opcode::BindVu0Vi:
						case Opcode::BindVu0ClipFlag:
							observed.diagnostic_flags |=
								A9ProfitabilityCertificate::
									DiagnosticUnsupportedVu0Binding;
							disallowed_effect = true;
							break;
						case Opcode::Cop1NormalizeInput:
						case Opcode::Cop1AddRaw:
						case Opcode::Cop1SubRaw:
						case Opcode::Cop1MulRaw:
						case Opcode::Cop1ClampOuResult:
						case Opcode::Cop1UpdateOuFlags:
						case Opcode::BindFpr:
						case Opcode::BindFcr31:
						case Opcode::BindAcc:
							// The mixed certificate retains only the same basic lazy-O/U
							// graph as ObservedCop1Residency. It may coexist with VU0
							// because both banks are allocated once for the complete unit.
							nonrepeated_cop1 |= !on_repeated_path;
							block_has_cop1 |= on_repeated_path;
							if (node.opcode == Opcode::Cop1AddRaw ||
								node.opcode == Opcode::Cop1SubRaw ||
								node.opcode == Opcode::Cop1MulRaw)
							{
								cop1_raw_arithmetic += on_repeated_path ? 1u : 0u;
							}
							else if (node.opcode == Opcode::Cop1ClampOuResult)
							{
								cop1_clamp_results += on_repeated_path ? 1u : 0u;
							}
							else if (node.opcode == Opcode::Cop1UpdateOuFlags)
							{
								cop1_flag_updates += on_repeated_path ? 1u : 0u;
							}
							break;
						case Opcode::Cop1ExceptionalOuResult:
							nonrepeated_cop1 |= !on_repeated_path;
							block_has_cop1 |= on_repeated_path;
							cop1_exceptional_predicates +=
								on_repeated_path ? 1u : 0u;
							break;
						case Opcode::BitcastI32ToF32Bits:
						case Opcode::BitcastF32BitsToI32:
						case Opcode::Cop1CompareEqual:
						case Opcode::Cop1CompareLess:
						case Opcode::Cop1CompareLessEqual:
						case Opcode::Cop1UpdateConditionFlag:
						case Opcode::Cop1BranchCondition:
						case Opcode::Cop1AbsoluteWord:
						case Opcode::Cop1NegateWord:
						case Opcode::Cop1ClearOuFlags:
						case Opcode::Cop1ConvertWord:
						case Opcode::Cop1ConvertSingle:
							observed.diagnostic_flags |=
								A9ProfitabilityCertificate::
									DiagnosticUnsupportedCop1Semantic;
							disallowed_effect = true;
							break;
						default:
							// Fail closed as the VU0 IR grows.  A newly added macro
							// semantic or architectural binding is a different target-cost
							// class until this certificate names it explicitly.
							if (vu0_semantic || vu0_binding)
							{
								observed.diagnostic_flags |= vu0_semantic ?
									A9ProfitabilityCertificate::
										DiagnosticUnsupportedVu0Semantic :
									A9ProfitabilityCertificate::
										DiagnosticUnsupportedVu0Binding;
								disallowed_effect = true;
							}
							break;
					}

					if (node.opcode != Opcode::MemoryLoad &&
						node.opcode != Opcode::MemoryStore)
					{
						continue;
					}
					if (!on_repeated_path)
					{
						const bool invariant_vu0_load =
							node.opcode == Opcode::MemoryLoad &&
							node.immediate == static_cast<u32>(
								MemoryAccessKind::LoadVu0Vector);
						disallowed_effect |= !invariant_vu0_load;
						continue;
					}
					memory_accesses++;
					if (node.immediate >
						static_cast<u32>(MemoryAccessKind::StoreVu0Vector))
					{
						disallowed_effect = true;
						continue;
					}
					const MemoryAccessKind kind =
						static_cast<MemoryAccessKind>(node.immediate);
					const bool load = kind == MemoryAccessKind::LoadVu0Vector;
					const bool store = kind == MemoryAccessKind::StoreVu0Vector;
					if (!load && !store)
					{
						disallowed_effect = true;
						continue;
					}
					vector_loads += load ? 1u : 0u;
					vector_stores += store ? 1u : 0u;
					covered_memory_accesses += memory_covered(node.id, store,
						MemoryAccessWidth(kind)) ? 1u : 0u;
					}
				if (block_has_cop1)
					cop1_blocks++;
				const bool observer_mismatch =
					block.observer_exits.size() != block_idle_nodes;
				if (observer_mismatch)
				{
					observed.diagnostic_flags |=
						A9ProfitabilityCertificate::DiagnosticObserverMismatch;
				}
				disallowed_effect |= observer_mismatch;
			}

			auto same_sources = [](std::vector<u32> left,
				std::vector<u32> right) {
				std::sort(left.begin(), left.end());
				std::sort(right.begin(), right.end());
				return left == right;
			};
			// Each independent VMULA/VMADDA stream has one initial multiply without
			// an add source. Direct callees contribute separate streams; treating the
			// entire reducible unit as one stream rejected the exact three-leaf mixed
			// ownership class after it had already passed all semantic checks.
			const u32 fmac_streams = std::max<u32>(1, direct_calls_on_path);
			const bool complete_fmac_graph =
				multiply_sources.size() >= 4 &&
				add_sources.size() + fmac_streams >= multiply_sources.size() &&
				same_sources(multiply_sources, clamp_sources) &&
				mac_nodes >= multiply_sources.size() &&
				status_nodes >= multiply_sources.size() &&
				result_bindings >= multiply_sources.size();
			const bool mixed_cop1 = cop1_raw_arithmetic != 0 ||
				cop1_exceptional_predicates != 0 || cop1_clamp_results != 0 ||
				cop1_flag_updates != 0;
			const bool complete_mixed_cop1_graph = !mixed_cop1 ||
				(program.options.cop1_lazy_ou_guards && !nonrepeated_cop1 &&
				 cop1_blocks != 0 &&
				 cop1_raw_arithmetic == cop1_exceptional_predicates &&
				 cop1_raw_arithmetic == cop1_clamp_results &&
				 cop1_raw_arithmetic == cop1_flag_updates);
			if (!complete_fmac_graph)
			{
				observed.diagnostic_flags |=
					A9ProfitabilityCertificate::DiagnosticIncompleteFmacGraph;
			}
			if (!complete_mixed_cop1_graph)
			{
				observed.diagnostic_flags |=
					A9ProfitabilityCertificate::DiagnosticIncompleteCop1Graph;
			}
			if (memory_accesses != covered_memory_accesses)
			{
				observed.diagnostic_flags |=
					A9ProfitabilityCertificate::DiagnosticUncoveredMemory;
			}
			if (direct_calls_on_path != program.direct_calls.size())
			{
				observed.diagnostic_flags |=
					A9ProfitabilityCertificate::DiagnosticDirectCallCoverage;
			}
			if (vector_loads == 0 || vector_stores == 0)
			{
				observed.diagnostic_flags |=
					A9ProfitabilityCertificate::DiagnosticMissingVectorIo;
			}
			if (idle_nodes < multiply_sources.size() + memory_accesses)
			{
				observed.diagnostic_flags |=
					A9ProfitabilityCertificate::DiagnosticIdleCoverage;
			}
			if (repeated_blocks == 0 || source_instructions < 10)
			{
				observed.diagnostic_flags |=
					A9ProfitabilityCertificate::DiagnosticBelowMeasuredFloor;
			}
			// The measured >=2x class begins directly at the repeated stream. A
			// reducible caller with invariant VU0 setup is semantically valid, but its
			// one-time entry/range/state cost is not interchangeable with four more
			// FMAC iterations: the physical Cortex-A9 matrix fixture measures only
			// about 1.1x. Keep that broader shape available to validation while product
			// policy waits for either a larger state-resident unit or a separately
			// measured target-cost certificate.
			if (disallowed_effect || nonrepeated_vu0_or_memory ||
				!complete_mixed_cop1_graph ||
				repeated_blocks == 0 ||
				direct_calls_on_path != program.direct_calls.size() ||
				source_instructions < 10 || vector_loads == 0 ||
				vector_stores == 0 ||
				(!acyclic_leaf && memory_accesses != covered_memory_accesses) ||
				(acyclic_leaf && multiply_sources.size() + memory_accesses + 1 <
					source_instructions) ||
				idle_nodes < multiply_sources.size() + memory_accesses ||
				!complete_fmac_graph)
			{
				return observed;
			}
			// The complete first-class A9 benchmark for the mixed class contains
			// three direct affine transform leaves and basic retained COP1 arithmetic.
			// One broader caller/leaf wrapper measured only ~1.14x, so do not infer
			// profitability merely from containing either subsystem. These are generic
			// decoded-cost bounds, not source identity or instruction-sequence keys.
			if (mixed_cop1 && (acyclic_leaf || direct_calls_on_path < 3 ||
				repeated_blocks < 4 || source_instructions < 32))
			{
				observed.diagnostic_flags |=
					A9ProfitabilityCertificate::DiagnosticBelowMeasuredFloor;
				return observed;
			}

			observed.kind = mixed_cop1 ?
				A9ProfitabilityCertificate::Kind::ObservedCop1Vu0Residency :
				(acyclic_leaf ?
					A9ProfitabilityCertificate::Kind::ObservedVu0AcyclicFmacLeaf :
					A9ProfitabilityCertificate::Kind::ObservedVu0FmacStream);
			observed.work_per_iteration = source_instructions;
			observed.repeated_blocks = repeated_blocks;
			observed.covered_scalar_loads = covered_memory_accesses;
			observed.minimum_profitable_iterations = std::max<u32>(1,
				acyclic_leaf ? minimum_observed_vu0_fmac_leaf_invocations :
					minimum_observed_vu0_fmac_iterations);
			observed.diagnostic_flags =
				A9ProfitabilityCertificate::DiagnosticNone;
			return observed;
		};

		A9ProfitabilityCertificate vu0 = build_observed_vu0_fmac_stream();
		if (vu0)
			return vu0;
		result.diagnostic_flags |= vu0.diagnostic_flags;

		if (result.work_per_iteration == 0 || !plan.control.valid ||
			!result.has_unconditional_store)
		{
			A9ProfitabilityCertificate observed = build_observed_direct_call();
			if (observed)
				return observed;
			A9ProfitabilityCertificate read_only = build_read_only_loop();
			if (read_only)
				return read_only;
			A9ProfitabilityCertificate preflighted =
				build_observed_preflighted_loop();
			if (preflighted)
				return preflighted;
			A9ProfitabilityCertificate cop1 = build_observed_cop1_residency();
			if (cop1)
				return cop1;
			// Preserve the old diagnostic cost estimate for a structurally counted
			// read-only loop. It remains an invalid product certificate because there is
			// no repeated store to amortize, but validation uses the estimate when
			// reporting why the region remains below break-even.
			if (result.work_per_iteration == 0 || !plan.control.valid)
				return result;
		}

		result.kind = A9ProfitabilityCertificate::Kind::CountedMemory;

		// Native Cortex-A9 first-class fixtures establish two different cost
		// models.  A 128-bit stream amortizes the fixed entry proof according to
		// the number of transferred words, while scalar fill/copy loops both cross
		// 2x at 512 iterations even though they perform different numbers of memory
		// accesses.  Treating scalar accesses as interchangeable "work units"
		// previously made that floor circular and over-rejected a one-byte fill by
		// 4x. Keep the target model here so cold triage and final emission agree.
		if (use_measured_scalar_floor && !result.has_header_vector_access)
		{
			result.minimum_profitable_iterations =
				std::max<u32>(1, minimum_counted_memory_work);
		}
		else
		{
			const u32 required_work =
				std::max<u32>(1, minimum_counted_memory_work);
			result.minimum_profitable_iterations = std::max<u32>(1,
				static_cast<u32>((static_cast<u64>(required_work) +
					result.work_per_iteration - 1) /
					result.work_per_iteration));
		}

		if (plan.control.valid && plan.control.bound_is_immediate &&
			plan.control.counter_stride > 0)
		{
			result.maximum_iterations_known = true;
			result.maximum_iterations =
				static_cast<u64>(plan.control.bound_immediate) /
					static_cast<u32>(plan.control.counter_stride) +
				plan.control.trip_count_adjustment;
		}
		return result;
	}

	BuildResult Build(const Program& program)
	{
		return Analyzer(program).Run();
	}
} // namespace VitaEE::RegionMemoryPlan
