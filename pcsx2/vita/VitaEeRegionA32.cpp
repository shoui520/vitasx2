// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#include "PrecompiledHeader.h"

#include "pcsx2/vita/A32Emitter.h"
#include "pcsx2/vita/VitaEeRegionA32.h"
#include "pcsx2/vita/VitaEeRegionAllocation.h"
#include "pcsx2/vita/VitaEeRegionExecutionPlan.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <utility>
#include <vector>

namespace VitaEE::RegionA32
{
	using namespace RegionIR;
	using VitaA32::Condition;
	using VitaA32::ShiftType;

	namespace
	{
		constexpr unsigned RETURN_VALUE = 0;
		constexpr unsigned CYCLE_LOW = 1;
		constexpr unsigned CYCLE_HIGH = 2;
		constexpr unsigned FIRST_GPR_HOST = 3;
		constexpr unsigned CONTEXT = 11;
		constexpr unsigned SCRATCH = 12;
		constexpr unsigned LINK_SCRATCH = 14;
		constexpr u8 SOURCE_PAGE_SHIFT = 12;
		constexpr u8 SOURCE_CHUNK_SHIFT = 6;
		constexpr u16 SAVED_REGISTERS =
			static_cast<u16>(((1u << 12) - (1u << 3)) | (1u << 14));
		constexpr u16 RESTORED_REGISTERS =
			static_cast<u16>(((1u << 12) - (1u << 3)) | (1u << 15));

		constexpr size_t StateGprLowOffset(u32 gpr)
		{
			return offsetof(CanonicalState, gpr) + gpr * sizeof(u128);
		}

		constexpr size_t ResultOffset(size_t member)
		{
			return offsetof(ExecutionContext, result) + member;
		}

		static_assert(offsetof(CanonicalState, cycle) < 4096);
		static_assert(StateGprLowOffset(31) + sizeof(u32) < 4096);
		static_assert(offsetof(ExecutionContext, result) + sizeof(ExecutionResult) <
					  4096);
#if UINTPTR_MAX == UINT32_MAX
		static_assert(sizeof(ExecutionContext) == 64);
#endif

		u32 Rs(u32 opcode) { return (opcode >> 21) & 0x1f; }
		u32 Rt(u32 opcode) { return (opcode >> 16) & 0x1f; }
		u32 Rd(u32 opcode) { return (opcode >> 11) & 0x1f; }
		u32 Sa(u32 opcode) { return (opcode >> 6) & 0x1f; }
		s16 Immediate(u32 opcode) { return static_cast<s16>(opcode); }

		bool IsLoadPrimary(u32 primary)
		{
			return primary == 0x23 || primary == 0x37;
		}

		bool IsStorePrimary(u32 primary)
		{
			return primary == 0x2b || primary == 0x3f;
		}

		u32 MemoryWidthForPrimary(u32 primary)
		{
			return primary == 0x37 || primary == 0x3f ? sizeof(u64) : sizeof(u32);
		}

		bool IsSupportedBranch(u32 opcode)
		{
			switch (opcode >> 26)
			{
				case 0x04: // BEQ
				case 0x05: // BNE
				case 0x14: // BEQL
				case 0x15: // BNEL
					return true;
				default:
					return false;
			}
		}

		bool IsSupportedBody(u32 opcode)
		{
			if (opcode == 0)
				return true; // Canonical SLL r0,r0,0 NOP.
			const u32 primary = opcode >> 26;
			if (primary == 0x09 || primary == 0x0a || primary == 0x0c ||
				IsLoadPrimary(primary) || IsStorePrimary(primary))
			{
				return true; // ADDIU, SLTI, ANDI, LW/SW, LD/SD.
			}
			if (primary != 0)
				return false;
			const u32 function = opcode & 0x3f;
			return (function == 0x00 && Rs(opcode) == 0) || // SLL.
			       (function == 0x21 && Sa(opcode) == 0) || // ADDU.
			       (function == 0x2b && Sa(opcode) == 0); // SLTU.
		}

		struct GuestPair
		{
			u8 low = 0xff;
			u8 high = 0xff;
			bool valid = false;
			bool high_valid = false;
		};

		struct InternalPatch
		{
			size_t offset = static_cast<size_t>(-1);
			u32 target_block = INVALID_BLOCK;
		};

		struct ColdExit
		{
			struct AliasPatch
			{
				size_t offset = static_cast<size_t>(-1);
				Condition condition = Condition::AL;
			};

			size_t branch_offset = static_cast<size_t>(-1);
			Condition condition = Condition::AL;
			std::vector<AliasPatch> alias_patches;
			ExitReason reason = ExitReason::RegionBoundary;
			u32 pc = 0;
			u32 pending_raw_cycles = 0;
			bool cycle_commit_deferred = false;
			bool publish_memory_address = false;
			// Nonzero only for a cold store-ownership exit reached after the hot
			// guard reused r0. Rebuild the exact guest EA from still-resident GPRs.
			u32 memory_address_opcode = 0;
			// Block-entry range guards may reuse r0 while scanning ownership.
			// Rebuild their published address from this still-resident guest GPR.
			u32 memory_address_guest = 0;
			const RegionExecution::ExitSite* state_site = nullptr;
		};

		struct AffineMemoryAccess
		{
			u32 source_pc = 0;
			u32 opcode = 0;
			u32 base_guest = 0;
			u32 offset = 0;
			u32 width_bytes = sizeof(u32);
			bool store = false;
		};

		struct BlockMemoryPlan
		{
			std::vector<AffineMemoryAccess> word_accesses;
			std::vector<AffineMemoryAccess> accesses;
			bool preflight = false;
			bool entry_counted_range = false;
			u32 induction_guest = 0;
			u32 bound_guest = 0;
			u32 stride_bytes = 0;
		};

		class Compiler
		{
		public:
			Compiler(const Program& program, VitaA32::CodeBuffer& code,
				const CompileOptions& options)
				: m_program(program)
				, m_code(code)
				, m_options(options)
			{
				m_guest_pairs.fill({});
				m_guest_first_pc.fill(0);
				m_guest_write_pc.fill(0);
			}

			CompileResult Run()
			{
				m_code.Reset();
				RegionExecution::BuildResult execution =
					RegionExecution::Build(m_program);
				if (!execution)
					return Fail(CompileFailure::InvalidProgram,
						execution.block < m_program.blocks.size() ?
							m_program.blocks[execution.block].pc : 0);
				m_execution_plan = std::move(execution.plan);
				m_result.exit_sites = static_cast<u32>(m_execution_plan.exits.size());
				m_result.exit_state_bindings =
					m_execution_plan.dirty_state_bindings;
				m_result.exit_state_words = m_execution_plan.dirty_state_words;
				RegionAllocation::BuildResult allocation =
					RegionAllocation::Build(m_program, m_execution_plan);
				if (!allocation)
				{
					return Fail(allocation.failure ==
							RegionAllocation::BuildFailure::SpillCapacity ?
						CompileFailure::RegisterPressure : CompileFailure::InvalidProgram,
						allocation.block < m_program.blocks.size() ?
							m_program.blocks[allocation.block].pc : 0);
				}
				m_allocation_plan = std::move(allocation.plan);
				m_result.allocation_live_values = m_allocation_plan.live_values;
				m_result.allocation_core_peak_words =
					m_allocation_plan.core_peak_words;
				m_result.allocation_neon_peak_q = m_allocation_plan.neon_peak_q;
				m_result.allocation_spilled_values =
					m_allocation_plan.spilled_values;
				m_result.allocation_spill_bytes = m_allocation_plan.spill_bytes;
				m_result.allocation_edge_moves = static_cast<u32>(
					m_allocation_plan.edge_moves.size());
				m_result.allocation_coalesced_edge_values =
					m_allocation_plan.coalesced_edge_values;
				m_result.allocation_demanded_scalar_values =
					m_allocation_plan.demanded_scalar_values;
				m_result.allocation_demanded_full_vector_values =
					m_allocation_plan.demanded_full_vector_values;
				if (m_program.source_blocks.empty())
					return Fail(CompileFailure::UnattestedSource,
						m_program.blocks[m_program.entry_block].pc);
				if (m_options.max_mapped_gprs == 0 ||
					m_options.max_mapped_gprs > m_result.mapped_gprs.size() ||
					m_options.max_resident_gpr_words == 0 ||
					m_options.max_resident_gpr_words > 8)
				{
					return Fail(CompileFailure::RegisterPressure,
						m_program.blocks[m_program.entry_block].pc);
				}
				if (m_options.max_hot_code_bytes == 0 ||
					m_options.max_code_bytes == 0)
				{
					return Fail(CompileFailure::CodeCapacity,
						m_program.blocks[m_program.entry_block].pc);
				}
				if (!AnalyzeSurface() || !AnalyzeExecutionPlanSurface() ||
					!AllocateGuestRegisters())
					return m_result;
				const BlockMemoryPlan& entry_memory =
					m_block_memory_plans[m_program.entry_block];
				if (entry_memory.entry_counted_range)
				{
					m_result.entry_range_induction_gpr =
						static_cast<u8>(entry_memory.induction_guest);
					m_result.entry_range_bound_gpr =
						static_cast<u8>(entry_memory.bound_guest);
					m_result.entry_range_stride_bytes =
						static_cast<u8>(entry_memory.stride_bytes);
				}

				m_block_offsets.assign(m_program.blocks.size(), static_cast<size_t>(-1));
				if (!EmitPrologue())
					return Fail(CompileFailure::Emission,
						m_program.blocks[m_program.entry_block].pc);

				if (!EmitEntryEventCheck())
					return Fail(CompileFailure::Emission,
						m_program.blocks[m_program.entry_block].pc);
				if (!EmitEntryMemoryPreflight())
					return Fail(CompileFailure::Emission,
						m_program.blocks[m_program.entry_block].pc);
				const size_t entry_branch = m_code.EmitBranchPlaceholder();
				if (entry_branch == static_cast<size_t>(-1))
					return Fail(CompileFailure::Emission,
						m_program.blocks[m_program.entry_block].pc);

				for (u32 block_index = 0; block_index < m_program.blocks.size();
					 block_index++)
				{
					m_block_offsets[block_index] = m_code.Size();
					if (!EmitBlock(block_index))
					{
						if (m_result.failure != CompileFailure::None)
							return m_result;
						return Fail(CompileFailure::Emission,
							m_program.blocks[block_index].pc);
					}
				}
				if (!m_code.PatchBranch(entry_branch,
						m_block_offsets[m_program.entry_block]))
				{
					return Fail(CompileFailure::Patch,
						m_program.blocks[m_program.entry_block].pc);
				}
				for (const InternalPatch& patch : m_internal_patches)
				{
					if (patch.target_block >= m_block_offsets.size() ||
						!m_code.PatchBranch(patch.offset,
							m_block_offsets[patch.target_block]))
					{
						return Fail(CompileFailure::Patch, 0);
					}
				}
				m_result.hot_code_bytes = static_cast<u32>(m_code.Size());
				if (m_code.OutOfSpace() ||
					m_result.hot_code_bytes > m_options.max_hot_code_bytes ||
					m_result.hot_code_bytes > m_options.max_code_bytes)
				{
					return Fail(CompileFailure::CodeCapacity, 0);
				}

				for (const ColdExit& exit : m_cold_exits)
				{
					const size_t target = m_code.Size();
					bool patched =
						m_code.PatchBranch(exit.branch_offset, target, exit.condition);
					for (const ColdExit::AliasPatch& alias : exit.alias_patches)
						patched &= m_code.PatchBranch(alias.offset, target, alias.condition);
					if (!patched || !EmitExit(exit))
					{
						return Fail(CompileFailure::Patch, exit.pc);
					}
				}

				if (m_code.OutOfSpace() || m_code.Size() > m_options.max_code_bytes)
					return Fail(CompileFailure::CodeCapacity, 0);
				if (!m_code.Flush())
					return Fail(CompileFailure::Emission, 0);

				m_result.code_bytes = static_cast<u32>(m_code.Size());
				m_result.cold_code_bytes =
					m_result.code_bytes - m_result.hot_code_bytes;
				const VitaA32::CodeBuffer::GeneratedCodeStats all =
					m_code.AnalyzeGeneratedCode(CONTEXT);
				const VitaA32::CodeBuffer::GeneratedCodeStats hot =
					m_code.AnalyzeGeneratedCodeRange(0, m_result.hot_code_bytes,
						CONTEXT);
				m_result.host_instructions = static_cast<u32>(all.host_instructions);
				m_result.hot_host_instructions = static_cast<u32>(hot.host_instructions);
				m_result.hot_host_loads = static_cast<u32>(hot.host_load_instructions);
				m_result.hot_host_stores = static_cast<u32>(hot.host_store_instructions);
				m_result.hot_state_loads = static_cast<u32>(hot.state_load_instructions);
				m_result.hot_state_stores = static_cast<u32>(hot.state_store_instructions);
				return m_result;
			}

		private:
			CompileResult Fail(CompileFailure failure, u32 pc)
			{
				m_result.failure = failure;
				m_result.failure_pc = pc;
				m_code.Reset();
				return m_result;
			}

			bool EnsureGuest(u32 guest, u32 pc)
			{
				if (guest == 0)
					return true;
				if (m_guest_observed[guest])
					return true;
				if (m_result.mapped_gpr_count >= m_options.max_mapped_gprs)
				{
					Fail(CompileFailure::RegisterPressure, pc);
					return false;
				}
				m_guest_observed[guest] = true;
				m_guest_first_pc[guest] = pc;
				const u8 index = m_result.mapped_gpr_count++;
				m_result.mapped_gprs[index] = static_cast<u8>(guest);
				return true;
			}

			bool ObserveGuest(u32 guest, u32 pc, bool high)
			{
				if (guest == 0)
					return true;
				m_guest_low_uses[guest]++;
				m_guest_high_read[guest] |= high;
				return EnsureGuest(guest, pc);
			}

			bool WriteGuest(u32 guest, u32 pc)
			{
				if (guest == 0)
					return true;
				if (!EnsureGuest(guest, pc))
					return false;
				m_guest_written[guest] = true;
				m_guest_write_pc[guest] = pc;
				m_result.written_gpr_mask |= 1u << guest;
				return true;
			}

			bool AllocateGuestPair(u32 guest, u32 pc)
			{
				GuestPair& pair = m_guest_pairs[guest];
				if (pair.high_valid)
					return true;
				const u8 required_words = pair.valid ? 1 : 2;
				if (m_result.resident_gpr_words + required_words >
					m_options.max_resident_gpr_words)
				{
					Fail(CompileFailure::RegisterPressure, pc);
					return false;
				}
				if (!pair.valid)
				{
					pair.low = static_cast<u8>(
						FIRST_GPR_HOST + m_result.resident_gpr_words++);
					pair.valid = true;
				}
				pair.high = static_cast<u8>(
					FIRST_GPR_HOST + m_result.resident_gpr_words++);
				pair.high_valid = true;
				return true;
			}

			bool AllocateGuestLow(u32 guest)
			{
				GuestPair& pair = m_guest_pairs[guest];
				if (pair.valid)
					return true;
				if (m_result.resident_gpr_words >= m_options.max_resident_gpr_words)
					return false;
				pair.low = static_cast<u8>(
					FIRST_GPR_HOST + m_result.resident_gpr_words++);
				pair.valid = true;
				return true;
			}

			bool AllocateGuestRegisters()
			{
				// Values written by the region must survive every internal edge and cold
				// exit, so they are always resident pairs. Full-width read-only inputs are
				// next because a 64-bit compare cannot be reconstructed from one word.
				// Remaining low-only inputs stay canonical and are loaded at their use.
				for (u32 guest = 1; guest < 32; guest++)
				{
					if (m_guest_written[guest] &&
						!AllocateGuestPair(guest, m_guest_write_pc[guest]))
					{
						return false;
					}
				}
				for (u32 guest = 1; guest < 32; guest++)
				{
					if (!m_guest_written[guest] && m_guest_high_read[guest] &&
						!AllocateGuestPair(guest, m_guest_first_pc[guest]))
					{
						return false;
					}
				}

				// Spend any remaining callee-saved words on the low-only inputs with
				// the highest static reuse. They remain read-only, so no exit-map state
				// is added. This keeps values such as a four-store fill word resident
				// without forcing every address-only participant into a wasteful pair.
				std::vector<std::pair<u32, u32>> low_candidates;
				for (u32 guest = 1; guest < 32; guest++)
				{
					if (m_guest_observed[guest] && !m_guest_pairs[guest].valid)
						low_candidates.emplace_back(m_guest_low_uses[guest], guest);
				}
				std::sort(low_candidates.begin(), low_candidates.end(),
					[](const auto& left, const auto& right) {
						return left.first != right.first ? left.first > right.first :
							left.second < right.second;
					});
				for (const auto& candidate : low_candidates)
				{
					if (!AllocateGuestLow(candidate.second))
						break;
				}
				return true;
			}

			bool AnalyzeInstruction(u32 opcode, u32 pc, bool control)
			{
				if (control)
				{
					if (!IsSupportedBranch(opcode))
					{
						Fail(CompileFailure::UnsupportedInstruction, pc);
						return false;
					}
					return ObserveGuest(Rs(opcode), pc, true) &&
					       ObserveGuest(Rt(opcode), pc, true);
				}

				if (!IsSupportedBody(opcode))
				{
					Fail(CompileFailure::UnsupportedInstruction, pc);
					return false;
				}
				if (opcode == 0)
					return true;
				const u32 primary = opcode >> 26;
				if (IsLoadPrimary(primary) || IsStorePrimary(primary))
				{
					if (IsLoadPrimary(primary))
						m_result.memory_loads++;
					else
						m_result.memory_stores++;
					return ObserveGuest(Rs(opcode), pc, false) &&
					       (IsLoadPrimary(primary) ? WriteGuest(Rt(opcode), pc) :
							ObserveGuest(Rt(opcode), pc, primary == 0x3f));
				}
				if (primary == 0)
				{
					const u32 function = opcode & 0x3f;
					if (function == 0x00)
						return ObserveGuest(Rt(opcode), pc, false) &&
						       WriteGuest(Rd(opcode), pc);
					const bool full_width = function == 0x2b;
					return ObserveGuest(Rs(opcode), pc, full_width) &&
					       ObserveGuest(Rt(opcode), pc, full_width) &&
					       WriteGuest(Rd(opcode), pc);
				}
				const bool reads_high = primary == 0x0a;
				return ObserveGuest(Rs(opcode), pc, reads_high) &&
				       WriteGuest(Rt(opcode), pc);
			}

			bool AnalyzeSurface()
			{
				m_block_memory_plans.clear();
				m_block_memory_plans.resize(m_program.blocks.size());
				for (u32 block_index = 0; block_index < m_program.blocks.size();
					 block_index++)
				{
					const Block& block = m_program.blocks[block_index];
					if (!block.guarded_exits.empty() ||
						(block.terminator.kind != TerminatorKind::Transfer &&
							block.terminator.kind != TerminatorKind::Branch))
					{
						return static_cast<bool>(
							Fail(CompileFailure::UnsupportedInstruction, block.pc));
					}
					for (u32 index = 0; index < block.source.size(); index++)
					{
						const bool control = block.terminator.kind == TerminatorKind::Branch &&
						                     index + 2 == block.source.size();
						if (!AnalyzeInstruction(block.source[index].opcode,
								block.source[index].pc, control))
						{
							return false;
						}
					}
					for (const Node& node : block.nodes)
					{
						const bool supported_store = node.opcode == Opcode::MemoryStore &&
							(node.immediate == static_cast<u32>(MemoryAccessKind::Store32) ||
							 node.immediate == static_cast<u32>(MemoryAccessKind::Store64));
						const bool supported_load = node.opcode == Opcode::MemoryLoad &&
							(node.immediate == static_cast<u32>(MemoryAccessKind::LoadS32) ||
							 node.immediate == static_cast<u32>(MemoryAccessKind::Load64));
						if ((node.opcode == Opcode::MemoryStore && !supported_store) ||
							(node.opcode == Opcode::MemoryLoad && !supported_load))
						{
							Fail(CompileFailure::UnsupportedMemory, node.source_pc);
							return false;
						}
					}
					u32 preflight_failure_pc = 0;
					if (!AnalyzeBlockMemoryPlan(block_index, block,
							&m_block_memory_plans[block_index],
							&preflight_failure_pc))
					{
						Fail(CompileFailure::UnsupportedMemory,
							preflight_failure_pc ? preflight_failure_pc : block.pc);
						return false;
					}
				}
				return true;
			}

			bool AnalyzeExecutionPlanSurface()
			{
				for (const RegionExecution::ExitSite& site : m_execution_plan.exits)
				{
					RegionExecution::ExitContractView contract{};
					if (!RegionExecution::ResolveExitContract(m_program, site, &contract) ||
						!contract.state)
					{
						Fail(CompileFailure::InvalidProgram,
							site.block < m_program.blocks.size() ?
								m_program.blocks[site.block].pc : 0);
						return false;
					}
					for (size_t slot = 0; slot < RegionExecution::STATE_SLOT_COUNT; slot++)
					{
						const u8 word_mask = site.dirty_words[slot];
						const u8 valid_mask = static_cast<u8>(
							(1u << RegionExecution::StateWordCount(slot)) - 1u);
						if (site.dirty_state.test(slot) != (word_mask != 0) ||
							(word_mask & ~valid_mask) != 0)
						{
							m_result.failure_state_slot = static_cast<u16>(slot);
							m_result.failure_state_word_mask = word_mask;
							Fail(CompileFailure::InvalidProgram,
								m_program.blocks[site.block].pc);
							return false;
						}
						if (word_mask == 0)
							continue;
						const RegionExecution::StateSlot decoded =
							RegionExecution::DecodeStateSlot(slot);
						if (decoded.state_class == RegionExecution::StateClass::Cycle &&
							word_mask == 0x3)
							continue;
						if (decoded.state_class != RegionExecution::StateClass::Gpr ||
							decoded.index == 0 || !m_guest_written[decoded.index] ||
							(word_mask & ~0x3u) != 0)
						{
							m_result.failure_state_slot = static_cast<u16>(slot);
							m_result.failure_state_word_mask = word_mask;
							Fail(CompileFailure::UnsupportedInstruction,
								m_program.blocks[site.block].pc);
							return false;
						}
					}
				}
				return true;
			}

			const SourceBlockContract* FindSourceContract(u32 pc) const
			{
				for (const SourceBlockContract& contract : m_program.source_blocks)
				{
					if (contract.start_pc == pc)
						return &contract;
				}
				return nullptr;
			}

			bool AnalyzeBlockMemoryPlan(u32 block_index, const Block& block,
				BlockMemoryPlan* plan, u32* failure_pc)
			{
				if (!plan || !failure_pc)
					return false;
				*plan = {};
				*failure_pc = 0;

				const SourceBlockContract* contract = FindSourceContract(block.pc);
				bool needs_preflight = false;
				u32 memory_count = 0;
				for (u32 index = 0; index < block.source.size(); index++)
				{
					const SourceInstruction& source = block.source[index];
					const u32 primary = source.opcode >> 26;
					if (!IsLoadPrimary(primary) && !IsStorePrimary(primary))
						continue;
					if (block.terminator.kind == TerminatorKind::Branch &&
						block.terminator.likely &&
						source.pc == block.terminator.delay_slot_pc)
					{
						// The likely delay is emitted only after the taken branch. Its
						// ordinary per-access guard must not become an unconditional entry
						// observation or reject an annulled edge.
						continue;
					}
					memory_count++;
					const bool restartable = index == 0 &&
						block.source[index].pc == block.pc && contract &&
						contract->dependency_start_pc == block.pc &&
						contract->charged_scaled_cycles_before == 0;
					needs_preflight |= !restartable;
				}
				if (memory_count == 0 || !needs_preflight)
					return true;

				struct AffineValue
				{
					u32 base_guest = 0;
					u32 offset = 0;
					bool valid = false;
				};
				std::array<AffineValue, 32> values{};
				values[0] = {0, 0, true};
				for (u32 gpr = 1; gpr < values.size(); gpr++)
					values[gpr] = {gpr, 0, true};

				const bool likely = block.terminator.kind == TerminatorKind::Branch &&
					block.terminator.likely;
				for (u32 index = 0; index < block.source.size(); index++)
				{
					const SourceInstruction& source = block.source[index];
					const u32 opcode = source.opcode;
					const u32 primary = opcode >> 26;
					const bool delay = block.terminator.kind == TerminatorKind::Branch &&
						index + 1 == block.source.size();
					if (IsLoadPrimary(primary) || IsStorePrimary(primary))
					{
						if (delay && likely)
							continue;
						if (!values[Rs(opcode)].valid)
						{
							*failure_pc = source.pc;
							return false;
						}
						const AffineValue base = values[Rs(opcode)];
						plan->accesses.push_back({source.pc, opcode,
							base.base_guest,
							base.offset + static_cast<u32>(static_cast<s32>(Immediate(opcode))),
							MemoryWidthForPrimary(primary),
							IsStorePrimary(primary)});
						if (IsLoadPrimary(primary) && Rt(opcode) != 0)
							values[Rt(opcode)] = {};
						continue;
					}

					if (primary == 0x09 && Rt(opcode) != 0)
					{
						const AffineValue input = values[Rs(opcode)];
						values[Rt(opcode)] = input.valid ?
							AffineValue{input.base_guest,
								input.offset + static_cast<u32>(
									static_cast<s32>(Immediate(opcode))), true} :
							AffineValue{};
					}
					else if ((primary == 0x0a || primary == 0x0c) && Rt(opcode) != 0)
					{
						values[Rt(opcode)] = {};
					}
					else if (primary == 0 && opcode != 0 && Rd(opcode) != 0)
					{
						values[Rd(opcode)] = {};
					}
				}

				plan->word_accesses = plan->accesses;
				// Adjacent accesses derived from the same block-entry base are one
				// fallible span. This is the common four-word SDK memset shape and
				// avoids redoing identical range/source-map checks four times.
				std::vector<AffineMemoryAccess> spans;
				spans.reserve(plan->accesses.size());
				for (const AffineMemoryAccess& access : plan->accesses)
				{
					if (!spans.empty())
					{
						AffineMemoryAccess& previous = spans.back();
						if (previous.store == access.store &&
							previous.base_guest == access.base_guest &&
							previous.offset + previous.width_bytes == access.offset &&
							access.width_bytes <= 64 - previous.width_bytes)
						{
							previous.width_bytes += access.width_bytes;
							continue;
						}
					}
					spans.push_back(access);
				}
				plan->accesses = std::move(spans);
				plan->preflight = true;
				m_result.memory_preflight_blocks++;
				m_result.memory_preflight_accesses += memory_count;

				// A single-block do-while store loop can prove its complete remaining
				// write range once at region entry. This is structural: one positive,
				// power-of-two induction step; one adjacent store span covering exactly
				// that step; and a non-likely BNE backedge comparing induction to a
				// stable bound. Runtime guards prove the terminating count, RAM range,
				// page containment, and absence of live generated source before the
				// first guest effect. Any failed proof restarts the untouched tier-zero
				// block, so the ordinary per-block preflight remains the fallback.
				if (m_program.blocks.size() == 1 &&
					block_index == m_program.entry_block &&
					block.terminator.kind == TerminatorKind::Branch &&
					!block.terminator.likely && block.source.size() >= 2 &&
					block.terminator.taken.target_block == block_index &&
					plan->accesses.size() == 1 && plan->accesses[0].store &&
					(block.source[0].opcode >> 26) == 0x2b &&
					plan->accesses[0].offset == 0)
				{
					const u32 body_count = static_cast<u32>(block.source.size() - 2);
					const u32 branch_opcode = block.source[body_count].opcode;
					const u32 induction = plan->accesses[0].base_guest;
					const bool branch_uses_induction = (branch_opcode >> 26) == 0x05 &&
						(Rs(branch_opcode) == induction || Rt(branch_opcode) == induction);
					const u32 bound = Rs(branch_opcode) == induction ?
						Rt(branch_opcode) : Rs(branch_opcode);
					u32 induction_writes = 0;
					u32 stride = 0;
					for (u32 index = 0; index < body_count; index++)
					{
						const u32 opcode = block.source[index].opcode;
						const u32 primary = opcode >> 26;
						const u32 written = primary == 0 ? Rd(opcode) : Rt(opcode);
						if (written != induction)
							continue;
						induction_writes++;
						if (primary == 0x09 && Rs(opcode) == induction && Immediate(opcode) > 0)
							stride = static_cast<u32>(Immediate(opcode));
						else
							stride = 0;
					}
					const bool power_of_two = stride != 0 && (stride & (stride - 1)) == 0;
					if (branch_uses_induction && bound != 0 && induction_writes == 1 &&
						power_of_two && stride <= 64 &&
						plan->accesses[0].width_bytes == stride &&
						block.source.back().opcode == 0)
					{
						plan->entry_counted_range = true;
						plan->induction_guest = induction;
						plan->bound_guest = bound;
						plan->stride_bytes = stride;
						m_result.memory_preflight_ranges++;
					}
				}
				return true;
			}

			const GuestPair& Pair(u32 guest) const { return m_guest_pairs[guest]; }

			bool EmitMove(unsigned destination, unsigned source)
			{
				return m_code.EmitMovRegShiftImm(destination, source, ShiftType::LSL, 0);
			}

			bool EmitGuestWord(u32 guest, bool high, unsigned destination,
				Condition condition = Condition::AL)
			{
				if (guest == 0)
					return m_code.EmitMovImm8(destination, 0, condition);
				const GuestPair& pair = Pair(guest);
				if (pair.valid && (!high || pair.high_valid))
				{
					return m_code.EmitMovRegShiftImm(destination,
						high ? pair.high : pair.low, ShiftType::LSL, 0, false,
						condition);
				}

				// Low-only read-only operands deliberately do not consume one of the
				// Cortex-A9's eight callee-saved region words. Load them through the
				// private canonical frame at the exact use instead.
				const unsigned pointer = destination == RETURN_VALUE ? SCRATCH : RETURN_VALUE;
				const size_t offset = StateGprLowOffset(guest) +
					(high ? sizeof(u32) : 0);
				return m_code.EmitLdrImm12(pointer, CONTEXT,
						static_cast<u16>(offsetof(ExecutionContext, state)), condition) &&
				       m_code.EmitLdrImm12(destination, pointer,
						static_cast<u16>(offset), condition);
			}

			bool EmitPrologue()
			{
				if (!m_code.EmitPush(SAVED_REGISTERS) || !EmitMove(CONTEXT, 0) ||
					!m_code.EmitLdrImm12(
						RETURN_VALUE, CONTEXT,
						static_cast<u16>(offsetof(ExecutionContext, state))) ||
					!m_code.EmitLdrImm12(
						CYCLE_LOW, RETURN_VALUE,
						static_cast<u16>(offsetof(CanonicalState, cycle))) ||
					!m_code.EmitLdrImm12(
						CYCLE_HIGH, RETURN_VALUE,
						static_cast<u16>(offsetof(CanonicalState, cycle) + sizeof(u32))))
				{
					return false;
				}
				for (u32 guest = 1; guest < 32; guest++)
				{
					const GuestPair& pair = Pair(guest);
					if (!pair.valid)
						continue;
					const size_t offset = StateGprLowOffset(guest);
					if (!m_code.EmitLdrImm12(pair.low, RETURN_VALUE,
							static_cast<u16>(offset)) ||
						(pair.high_valid &&
							!m_code.EmitLdrImm12(pair.high, RETURN_VALUE,
								static_cast<u16>(offset + sizeof(u32)))))
					{
						return false;
					}
				}
				return true;
			}

			bool EmitUnsigned64AtLeastNextEvent()
			{
				return m_code.EmitLdrImm12(RETURN_VALUE, CONTEXT,
						   static_cast<u16>(offsetof(
							   ExecutionContext, next_event_cycle_low))) &&
				       m_code.EmitLdrImm12(SCRATCH, CONTEXT,
						   static_cast<u16>(offsetof(
							   ExecutionContext, next_event_cycle_high))) &&
				       m_code.EmitSubReg(RETURN_VALUE, CYCLE_LOW, RETURN_VALUE, true) &&
				       m_code.EmitSbcReg(SCRATCH, CYCLE_HIGH, SCRATCH, true);
			}

			const RegionExecution::ExitSite* FindExitSite(
				RegionExecution::ExitSiteKind kind, u32 block, u32 ordinal = 0) const
			{
				const auto found = std::find_if(m_execution_plan.exits.begin(),
					m_execution_plan.exits.end(), [&](const RegionExecution::ExitSite& site) {
						return site.kind == kind && site.block == block &&
						       site.ordinal == ordinal;
					});
				return found != m_execution_plan.exits.end() ? &*found : nullptr;
			}

			const RegionExecution::ExitSite* FindMemoryExitSite(
				u32 block_index, u32 source_pc) const
			{
				const Block& block = m_program.blocks[block_index];
				for (u32 index = 0; index < block.memory_exits.size(); index++)
				{
					const ValueId operation = block.memory_exits[index].operation;
					const auto node = std::find_if(block.nodes.begin(), block.nodes.end(),
						[operation](const Node& candidate) {
							return candidate.id == operation;
						});
					if (node != block.nodes.end() && node->source_pc == source_pc)
					{
						return FindExitSite(
							RegionExecution::ExitSiteKind::Memory, block_index, index);
					}
				}
				return nullptr;
			}

			const RegionExecution::ExitSite* FindTransferExitSite(
				const Block& block, const Transfer& transfer) const
			{
				const u32 block_index =
					static_cast<u32>(&block - m_program.blocks.data());
				if (&transfer == &block.terminator.taken)
				{
					return FindExitSite(
						RegionExecution::ExitSiteKind::Taken, block_index);
				}
				if (block.terminator.kind == TerminatorKind::Branch &&
					&transfer == &block.terminator.not_taken)
				{
					return FindExitSite(
						RegionExecution::ExitSiteKind::NotTaken, block_index);
				}
				return nullptr;
			}

			bool AppendColdBranch(Condition condition, ExitReason reason, u32 pc,
				const RegionExecution::ExitSite* state_site,
				u32 pending_raw_cycles = 0, bool deferred = false,
				bool memory_address = false, u32 memory_address_opcode = 0,
				u32 memory_address_guest = 0)
			{
				if (!state_site)
					return false;
				const size_t branch = m_code.EmitBranchPlaceholder(condition);
				if (branch == static_cast<size_t>(-1))
					return false;
				for (ColdExit& exit : m_cold_exits)
				{
					if (exit.reason == reason && exit.pc == pc &&
						exit.pending_raw_cycles == pending_raw_cycles &&
						exit.cycle_commit_deferred == deferred &&
						exit.publish_memory_address == memory_address &&
						exit.memory_address_opcode == memory_address_opcode &&
						exit.memory_address_guest == memory_address_guest &&
						exit.state_site == state_site)
					{
						exit.alias_patches.push_back({branch, condition});
						return true;
					}
				}
				ColdExit exit{};
				exit.branch_offset = branch;
				exit.condition = condition;
				exit.reason = reason;
				exit.pc = pc;
				exit.pending_raw_cycles = pending_raw_cycles;
				exit.cycle_commit_deferred = deferred;
				exit.publish_memory_address = memory_address;
				exit.memory_address_opcode = memory_address_opcode;
				exit.memory_address_guest = memory_address_guest;
				exit.state_site = state_site;
				m_cold_exits.push_back(std::move(exit));
				return true;
			}

			bool EmitEntryEventCheck()
			{
				const RegionExecution::ExitSite* site = FindExitSite(
					RegionExecution::ExitSiteKind::EntryEvent,
					m_program.entry_block);
				return EmitUnsigned64AtLeastNextEvent() &&
				       AppendColdBranch(Condition::CS, ExitReason::EventHorizon,
						   m_program.blocks[m_program.entry_block].pc, site);
			}

			bool EmitEntryMemoryPreflight()
			{
				const u32 block_index = m_program.entry_block;
				const BlockMemoryPlan& plan = m_block_memory_plans[block_index];
				if (!plan.entry_counted_range)
					return true;

				const Block& block = m_program.blocks[block_index];
				const RegionExecution::ExitSite* site = FindExitSite(
					RegionExecution::ExitSiteKind::EntryFallback, block_index);
				const GuestPair& induction = Pair(plan.induction_guest);
				const GuestPair& bound = Pair(plan.bound_guest);
				auto fallback = [&](Condition condition, ExitReason reason) {
					return AppendColdBranch(condition, reason, block.pc, site, 0, true, true,
						0, plan.induction_guest);
				};

				// ADDIU sign-extends the induction value before the terminating BNE.
				// A nonzero upper half on the stable bound therefore cannot terminate
				// this positive low-main-RAM loop and remains tier-zero work.
				if (!m_code.EmitCmpImm32(bound.high, 0) ||
					!fallback(Condition::NE, ExitReason::MemoryTranslation) ||
					!EmitMove(RETURN_VALUE, induction.low) ||
					!m_code.EmitTstImm32(RETURN_VALUE, sizeof(u32) - 1) ||
					!fallback(Condition::NE, ExitReason::MemoryAlignment) ||
					!m_code.EmitCmpReg(RETURN_VALUE, bound.low) ||
					!fallback(Condition::CS, ExitReason::MemoryTranslation) ||
					!m_code.EmitSubReg(SCRATCH, bound.low, RETURN_VALUE) ||
					!m_code.EmitTstImm32(SCRATCH, plan.stride_bytes - 1) ||
					!fallback(Condition::NE, ExitReason::MemoryTranslation) ||
					!m_code.EmitLdrImm12(SCRATCH, CONTEXT,
						static_cast<u16>(offsetof(
							ExecutionContext, main_ram_limit))) ||
					!m_code.EmitCmpReg(bound.low, SCRATCH) ||
					!fallback(Condition::HI, ExitReason::MemoryTranslation) ||
					!m_code.EmitSubImm8(LINK_SCRATCH, bound.low, 1))
				{
					return false;
				}

				// The global identity flag is deliberately conservative: one unrelated
				// low-page remap disables it for the complete 32 MiB window. Prove only
				// this range instead. On ARM32 an identity VTLBVirtual entry is the
				// constant (main_ram - host_memory_base), because assumePtr() adds the
				// raw entry and the guest address. Scan one entry per covered 4 KiB page
				// before the first guest store.
				if (!m_code.EmitLdrImm12(SCRATCH, CONTEXT,
						static_cast<u16>(offsetof(ExecutionContext, vmap))) ||
					!m_code.EmitAddRegShiftImm(SCRATCH, SCRATCH, induction.low,
						ShiftType::LSR, SOURCE_PAGE_SHIFT - 2) ||
					!m_code.EmitLdrImm12(LINK_SCRATCH, CONTEXT,
						static_cast<u16>(offsetof(ExecutionContext, main_ram))) ||
					!m_code.EmitLdrImm12(RETURN_VALUE, CONTEXT,
						static_cast<u16>(offsetof(ExecutionContext, host_memory_base))) ||
					!m_code.EmitSubReg(
						LINK_SCRATCH, LINK_SCRATCH, RETURN_VALUE) ||
					!EmitMove(RETURN_VALUE, bound.low) ||
					!m_code.EmitSubImm8(RETURN_VALUE, RETURN_VALUE, 1) ||
					!m_code.EmitMovRegShiftImm(RETURN_VALUE, RETURN_VALUE,
						ShiftType::LSR, SOURCE_PAGE_SHIFT) ||
					!m_code.EmitSubRegShiftImm(RETURN_VALUE, RETURN_VALUE,
						induction.low, ShiftType::LSR, SOURCE_PAGE_SHIFT) ||
					!m_code.EmitAddImm8(RETURN_VALUE, RETURN_VALUE, 1))
				{
					return false;
				}
				const size_t mapping_loop = m_code.Size();
				if (!m_code.EmitLdrImm12PostIndex(CYCLE_LOW, SCRATCH, sizeof(u32)) ||
					!m_code.EmitCmpReg(CYCLE_LOW, LINK_SCRATCH) ||
					// Restore the cycle register conditionally before the shared exact-state
					// cold exit. These loads preserve the comparison flags and do not run on
					// the successful path.
					!m_code.EmitLdrImm12(SCRATCH, CONTEXT,
						static_cast<u16>(offsetof(ExecutionContext, state)),
						Condition::NE) ||
					!m_code.EmitLdrImm12(CYCLE_LOW, SCRATCH,
						static_cast<u16>(offsetof(CanonicalState, cycle)),
						Condition::NE) ||
					!fallback(Condition::NE, ExitReason::MemoryTranslation) ||
					!m_code.EmitSubImm8(
						RETURN_VALUE, RETURN_VALUE, 1, true))
				{
					return false;
				}
				const size_t mapping_backedge =
					m_code.EmitBranchPlaceholder(Condition::NE);
				if (mapping_backedge == static_cast<size_t>(-1) ||
					!m_code.PatchBranch(
						mapping_backedge, mapping_loop, Condition::NE) ||
					!m_code.EmitLdrImm12(RETURN_VALUE, CONTEXT,
						static_cast<u16>(offsetof(ExecutionContext, state))) ||
					!m_code.EmitLdrImm12(CYCLE_LOW, RETURN_VALUE,
						static_cast<u16>(offsetof(CanonicalState, cycle))) ||
					!EmitMove(RETURN_VALUE, induction.low) ||
					!m_code.EmitSubImm8(LINK_SCRATCH, bound.low, 1))
				{
					return false;
				}

				// Keep the overwhelmingly common one-page case short. Cross-page
				// ranges scan the authoritative page summary once; each byte covers
				// 4 KiB of stores, so this replaces thousands of per-word checks.
				if (!m_code.EmitEorReg(SCRATCH, RETURN_VALUE, LINK_SCRATCH) ||
					!m_code.EmitMovRegShiftImm(SCRATCH, SCRATCH,
						ShiftType::LSR, SOURCE_PAGE_SHIFT, true))
				{
					return false;
				}
				const size_t multiple_pages =
					m_code.EmitBranchPlaceholder(Condition::NE);
				if (multiple_pages == static_cast<size_t>(-1) ||
					!m_code.EmitLdrImm12(SCRATCH, CONTEXT,
						static_cast<u16>(offsetof(
							ExecutionContext, ram_source_page_live_flags))) ||
					!m_code.EmitLdrbRegShift(SCRATCH, SCRATCH, RETURN_VALUE,
						ShiftType::LSR, SOURCE_PAGE_SHIFT) ||
					!m_code.EmitCmpImm32(SCRATCH, 0) ||
					!fallback(Condition::NE, ExitReason::SelfModifyingCode))
				{
					return false;
				}
				const size_t ownership_done = m_code.EmitBranchPlaceholder();
				const size_t multiple_page_target = m_code.Size();
				if (ownership_done == static_cast<size_t>(-1) ||
					!m_code.PatchBranch(multiple_pages, multiple_page_target,
						Condition::NE) ||
					!m_code.EmitMovRegShiftImm(LINK_SCRATCH, LINK_SCRATCH,
						ShiftType::LSR, SOURCE_PAGE_SHIFT) ||
					!m_code.EmitMovRegShiftImm(RETURN_VALUE, RETURN_VALUE,
						ShiftType::LSR, SOURCE_PAGE_SHIFT) ||
					!m_code.EmitSubReg(LINK_SCRATCH, LINK_SCRATCH, RETURN_VALUE) ||
					!m_code.EmitAddImm8(LINK_SCRATCH, LINK_SCRATCH, 1) ||
					!m_code.EmitLdrImm12(SCRATCH, CONTEXT,
						static_cast<u16>(offsetof(
							ExecutionContext, ram_source_page_live_flags))) ||
					!m_code.EmitAddReg(SCRATCH, SCRATCH, RETURN_VALUE))
				{
					return false;
				}
				const size_t ownership_loop = m_code.Size();
				if (!m_code.EmitLdrbImm12PostIndex(RETURN_VALUE, SCRATCH, 1) ||
					!m_code.EmitCmpImm32(RETURN_VALUE, 0) ||
					!fallback(Condition::NE, ExitReason::SelfModifyingCode) ||
					!m_code.EmitSubImm8(LINK_SCRATCH, LINK_SCRATCH, 1, true))
				{
					return false;
				}
				const size_t ownership_backedge =
					m_code.EmitBranchPlaceholder(Condition::NE);
				if (ownership_backedge == static_cast<size_t>(-1) ||
					!m_code.PatchBranch(ownership_backedge, ownership_loop,
						Condition::NE) ||
					!m_code.PatchBranch(ownership_done, m_code.Size()))
				{
					return false;
				}
				return true;
			}

			bool EmitAddCycles(u32 cycles)
			{
				return m_code.EmitMovImm32(RETURN_VALUE, cycles) &&
				       m_code.EmitAddReg(CYCLE_LOW, CYCLE_LOW, RETURN_VALUE, true) &&
				       m_code.EmitAdcImm8(CYCLE_HIGH, CYCLE_HIGH, 0);
			}

			bool EmitCompareEqual64(u32 left_guest, u32 right_guest)
			{
				if (left_guest == 0 && right_guest == 0)
					return m_code.EmitCmpReg(CYCLE_LOW, CYCLE_LOW);
				if (left_guest == 0 || right_guest == 0)
				{
					const GuestPair& value = Pair(left_guest == 0 ? right_guest : left_guest);
					return m_code.EmitCmpImm32(value.low, 0) &&
					       m_code.EmitCmpImm32(value.high, 0, Condition::EQ);
				}
				const GuestPair& left = Pair(left_guest);
				const GuestPair& right = Pair(right_guest);
				return m_code.EmitCmpReg(left.low, right.low) &&
				       m_code.EmitCmpReg(left.high, right.high, Condition::EQ);
			}

			bool EmitSub64ForFlags(u32 left_guest, u32 right_guest)
			{
				if (left_guest == 0)
				{
					if (!m_code.EmitMovImm8(RETURN_VALUE, 0) ||
						!m_code.EmitMovImm8(SCRATCH, 0))
					{
						return false;
					}
				}
				else
				{
					const GuestPair& left = Pair(left_guest);
					if (!EmitMove(RETURN_VALUE, left.low) || !EmitMove(SCRATCH, left.high))
					{
						return false;
					}
				}
				if (right_guest == 0)
				{
					return m_code.EmitSubImm8(RETURN_VALUE, RETURN_VALUE, 0, true) &&
					       m_code.EmitSbcImm8(SCRATCH, SCRATCH, 0, true);
				}
				const GuestPair& right = Pair(right_guest);
				return m_code.EmitSubReg(RETURN_VALUE, RETURN_VALUE, right.low, true) &&
				       m_code.EmitSbcReg(SCRATCH, SCRATCH, right.high, true);
			}

			bool EmitAddiu(u32 opcode)
			{
				const u32 destination = Rt(opcode);
				if (destination == 0)
					return true;
				const s32 immediate = Immediate(opcode);
				const GuestPair& output = Pair(destination);
				if (Rs(opcode) == 0)
				{
					return m_code.EmitMovImm32(output.low, static_cast<u32>(immediate)) &&
					       m_code.EmitMovImm32(output.high, immediate < 0 ? UINT32_MAX : 0);
				}
				// PCSX2 owner: R5900OpcodeImpl.cpp::ADDIU() wraps RS.low32 plus
				// the signed immediate, then sign-extends that 32-bit result. ADDIU
				// is not DADDIU; carrying into RS bits 32..63 is architecturally
				// wrong even though ordinary pointer induction rarely exposes it.
				return EmitGuestWord(Rs(opcode), false, RETURN_VALUE) &&
				       m_code.EmitAddImm32(output.low, RETURN_VALUE,
						static_cast<u32>(immediate)) &&
				       m_code.EmitMovRegShiftImm(
						   output.high, output.low, ShiftType::ASR, 31);
			}

			bool EmitAndi(u32 opcode)
			{
				const u32 destination = Rt(opcode);
				if (destination == 0)
					return true;
				const GuestPair& output = Pair(destination);
				if (Rs(opcode) == 0)
				{
					return m_code.EmitMovImm8(output.low, 0) &&
					       m_code.EmitMovImm8(output.high, 0);
				}
				return EmitGuestWord(Rs(opcode), false, RETURN_VALUE) &&
				       m_code.EmitAndImm32(output.low, RETURN_VALUE, opcode & 0xffffu) &&
				       m_code.EmitMovImm8(output.high, 0);
			}

			bool EmitSlti(u32 opcode)
			{
				const u32 destination = Rt(opcode);
				if (destination == 0)
					return true;
				const s32 immediate = Immediate(opcode);
				if (Rs(opcode) == 0)
				{
					const GuestPair& output = Pair(destination);
					return m_code.EmitMovImm8(output.low, immediate > 0 ? 1 : 0) &&
					       m_code.EmitMovImm8(output.high, 0);
				}
				const GuestPair& input = Pair(Rs(opcode));
				const GuestPair& output = Pair(destination);
				return m_code.EmitMovImm32(RETURN_VALUE, static_cast<u32>(immediate)) &&
				       m_code.EmitSubReg(RETURN_VALUE, input.low, RETURN_VALUE, true) &&
				       m_code.EmitMovImm32(SCRATCH, immediate < 0 ? UINT32_MAX : 0) &&
				       m_code.EmitSbcReg(SCRATCH, input.high, SCRATCH, true) &&
				       m_code.EmitMovImm8(output.low, 0) &&
				       m_code.EmitMovImm8(output.low, 1, Condition::LT) &&
				       m_code.EmitMovImm8(output.high, 0);
			}

			bool EmitSltu(u32 opcode)
			{
				const u32 destination = Rd(opcode);
				if (destination == 0)
					return true;
				if (!EmitSub64ForFlags(Rs(opcode), Rt(opcode)))
					return false;
				const GuestPair& output = Pair(destination);
				return m_code.EmitMovImm8(output.low, 0) &&
				       m_code.EmitMovImm8(output.low, 1, Condition::CC) &&
				       m_code.EmitMovImm8(output.high, 0);
			}

			bool EmitSll(u32 opcode)
			{
				const u32 destination = Rd(opcode);
				if (destination == 0)
					return true;
				const GuestPair& output = Pair(destination);
				return EmitGuestWord(Rt(opcode), false, RETURN_VALUE) &&
				       m_code.EmitMovRegShiftImm(output.low, RETURN_VALUE,
					   ShiftType::LSL, static_cast<u8>(Sa(opcode))) &&
				       m_code.EmitMovRegShiftImm(
					   output.high, output.low, ShiftType::ASR, 31);
			}

			bool EmitAddu(u32 opcode)
			{
				const u32 destination = Rd(opcode);
				if (destination == 0)
					return true;
				const GuestPair& output = Pair(destination);
				return EmitGuestWord(Rs(opcode), false, RETURN_VALUE) &&
				       EmitGuestWord(Rt(opcode), false, SCRATCH) &&
				       m_code.EmitAddReg(output.low, RETURN_VALUE, SCRATCH) &&
				       m_code.EmitMovRegShiftImm(
					   output.high, output.low, ShiftType::ASR, 31);
			}

			u32 PendingRawBefore(const Block& block, u32 pc) const
			{
				u32 pending = 0;
				for (const SourceInstruction& source : block.source)
				{
					if (source.pc == pc)
						break;
					pending +=
						RawRecompilerCycles(source.opcode, m_program.options.cycle_factor);
				}
				return pending;
			}

			bool EmitEffectiveAddress(u32 opcode, unsigned destination)
			{
				const u32 base_guest = Rs(opcode);
				const s32 immediate = Immediate(opcode);
				if (base_guest == 0)
					return m_code.EmitMovImm32(destination, static_cast<u32>(immediate));

				return EmitGuestWord(base_guest, false, destination) &&
				       (immediate == 0 || m_code.EmitAddImm32(destination, destination,
						static_cast<u32>(immediate)));
			}

			bool EmitAffineEffectiveAddress(const AffineMemoryAccess& access,
				unsigned destination)
			{
				if (access.base_guest == 0)
					return m_code.EmitMovImm32(destination, access.offset);
				return EmitGuestWord(access.base_guest, false, destination) &&
				       (access.offset == 0 ||
						m_code.EmitAddImm32(destination, destination, access.offset));
			}

			bool EmitPreflightWordAccess(const Block& block,
				const AffineMemoryAccess& access)
			{
				const u32 block_index = static_cast<u32>(&block - m_program.blocks.data());
				const RegionExecution::ExitSite* site = FindExitSite(
					RegionExecution::ExitSiteKind::BlockEntry, block_index);
				// No guest instruction has executed in this block yet. Every failed
				// proof therefore returns to the real tier-zero block entry with zero
				// cycle debt; the fallback re-executes the complete PCSX2 unit.
				if (!EmitAffineEffectiveAddress(access, RETURN_VALUE) ||
					!m_code.EmitTstImm32(RETURN_VALUE, access.width_bytes - 1) ||
					!AppendColdBranch(Condition::NE, ExitReason::MemoryAlignment,
						block.pc, site, 0, true, true))
				{
					return false;
				}
				if (access.width_bytes > sizeof(u32))
				{
					if (!m_code.EmitAddImm32(LINK_SCRATCH, RETURN_VALUE,
							access.width_bytes - sizeof(u32), true) ||
						!AppendColdBranch(Condition::CS,
							ExitReason::MemoryTranslation, block.pc, site, 0, true, true))
					{
						return false;
					}
				}
				else if (!EmitMove(LINK_SCRATCH, RETURN_VALUE))
				{
					return false;
				}
				if (!m_code.EmitLdrImm12(SCRATCH, CONTEXT,
						static_cast<u16>(offsetof(
							ExecutionContext, identity_main_ram_limit))) ||
					!m_code.EmitCmpReg(LINK_SCRATCH, SCRATCH) ||
					!AppendColdBranch(Condition::CS, ExitReason::MemoryTranslation,
						block.pc, site, 0, true, true))
				{
					return false;
				}
				if (!access.store)
					return true;
				if (!m_code.EmitEorReg(SCRATCH, RETURN_VALUE, LINK_SCRATCH) ||
					!m_code.EmitMovRegShiftImm(SCRATCH, SCRATCH,
						ShiftType::LSR, SOURCE_CHUNK_SHIFT, true) ||
					!AppendColdBranch(Condition::NE, ExitReason::MemoryTranslation,
						block.pc, site, 0, true, true))
				{
					return false;
				}

				// identity_main_ram_limit proves that the guest EA is also the
				// physical RAM offset. The page byte keeps the ordinary data-page
				// path short; a live page receives the authoritative 64-byte test.
				if (!m_code.EmitLdrImm12(SCRATCH, CONTEXT,
						static_cast<u16>(offsetof(
							ExecutionContext, ram_source_page_live_flags))) ||
					!m_code.EmitLdrbRegShift(SCRATCH, SCRATCH, RETURN_VALUE,
						ShiftType::LSR, SOURCE_PAGE_SHIFT) ||
					!m_code.EmitCmpImm32(SCRATCH, 0))
				{
					return false;
				}
				const size_t data_page =
					m_code.EmitBranchPlaceholder(Condition::EQ);
				if (data_page == static_cast<size_t>(-1) ||
					!m_code.EmitLdrImm12(LINK_SCRATCH, CONTEXT,
						static_cast<u16>(offsetof(
							ExecutionContext, ram_source_chunk_live_bits))) ||
					!m_code.EmitLdrbRegShift(LINK_SCRATCH, LINK_SCRATCH,
						RETURN_VALUE, ShiftType::LSR, SOURCE_CHUNK_SHIFT + 3) ||
					!m_code.EmitMovRegShiftImm(SCRATCH, RETURN_VALUE,
						ShiftType::LSR, SOURCE_CHUNK_SHIFT) ||
					!m_code.EmitAndImm8(SCRATCH, SCRATCH, 7) ||
					!m_code.EmitMovRegShiftReg(LINK_SCRATCH, LINK_SCRATCH,
						ShiftType::LSR, SCRATCH) ||
					!m_code.EmitTstImm32(LINK_SCRATCH, 1) ||
					!AppendColdBranch(Condition::NE, ExitReason::SelfModifyingCode,
						block.pc, site, 0, true, true) ||
					!m_code.PatchBranch(data_page, m_code.Size(), Condition::EQ))
				{
					return false;
				}
				return true;
			}

			bool EmitBlockMemoryPreflight(u32 block_index)
			{
				const BlockMemoryPlan& plan = m_block_memory_plans[block_index];
				if (!plan.preflight || plan.entry_counted_range)
					return true;
				const Block& block = m_program.blocks[block_index];
				for (const AffineMemoryAccess& access : plan.accesses)
				{
					if (!EmitPreflightWordAccess(block, access))
						return false;
				}
				return true;
			}

			bool EmitDirectMemoryHostAddress(u32 opcode)
			{
				return EmitEffectiveAddress(opcode, RETURN_VALUE) &&
					m_code.EmitLdrImm12(SCRATCH, CONTEXT,
						static_cast<u16>(offsetof(ExecutionContext, main_ram))) &&
					m_code.EmitAddReg(SCRATCH, SCRATCH, RETURN_VALUE);
			}

			// On success r12 is the translated host pointer to one aligned scalar in
			// retail EE RAM. Every rejection is
			// before the memory effect and carries the original PCSX2 block-cycle
			// debt to tier zero.
			bool EmitResolveMemoryHostAddress(const Block& block,
				const SourceInstruction& source, u32 width_bytes)
			{
				const u32 opcode = source.opcode;
				const u32 block_index = static_cast<u32>(&block - m_program.blocks.data());
				const RegionExecution::ExitSite* site =
					FindMemoryExitSite(block_index, source.pc);
				if (!EmitEffectiveAddress(opcode, RETURN_VALUE))
					return false;

				const u32 pending = PendingRawBefore(block, source.pc);
				if (!m_code.EmitTstImm32(RETURN_VALUE, width_bytes - 1) ||
					!AppendColdBranch(Condition::NE, ExitReason::MemoryAlignment,
						source.pc, site, pending, true, true) ||
					(width_bytes > sizeof(u32) &&
						(!m_code.EmitAddImm32(LINK_SCRATCH, RETURN_VALUE,
							width_bytes - sizeof(u32), true) ||
						 !AppendColdBranch(Condition::CS,
							 ExitReason::MemoryTranslation, source.pc, site, pending, true, true))) ||
					(width_bytes == sizeof(u32) &&
						!EmitMove(LINK_SCRATCH, RETURN_VALUE)) ||
					!m_code.EmitLdrImm12(SCRATCH, CONTEXT,
						static_cast<u16>(offsetof(
							ExecutionContext, identity_main_ram_limit))) ||
					!m_code.EmitCmpReg(LINK_SCRATCH, SCRATCH))
				{
					return false;
				}
				const size_t identity = m_code.EmitBranchPlaceholder(Condition::CC);
				if (identity == static_cast<size_t>(-1) ||
					!m_code.EmitLdrImm12(
						SCRATCH, CONTEXT,
						static_cast<u16>(offsetof(ExecutionContext, vmap))) ||
					!m_code.EmitMovRegShiftImm(LINK_SCRATCH, RETURN_VALUE, ShiftType::LSR,
						SOURCE_PAGE_SHIFT) ||
					!m_code.EmitLdrRegShift(SCRATCH, SCRATCH, LINK_SCRATCH, ShiftType::LSL,
						2) ||
					!m_code.EmitAddReg(SCRATCH, SCRATCH, RETURN_VALUE, true) ||
					!AppendColdBranch(Condition::MI, ExitReason::MemoryHandler,
						source.pc, site, pending, true, true) ||
					!m_code.EmitLdrImm12(
						LINK_SCRATCH, CONTEXT,
						static_cast<u16>(offsetof(ExecutionContext, host_memory_base))) ||
					!m_code.EmitAddReg(SCRATCH, SCRATCH, LINK_SCRATCH) ||
					!m_code.EmitLdrImm12(
						LINK_SCRATCH, CONTEXT,
						static_cast<u16>(offsetof(ExecutionContext, main_ram))) ||
					!m_code.EmitCmpReg(SCRATCH, LINK_SCRATCH) ||
					!AppendColdBranch(Condition::CC, ExitReason::MemoryTranslation,
						source.pc, site, pending, true, true) ||
					!m_code.EmitLdrImm12(
						LINK_SCRATCH, CONTEXT,
						static_cast<u16>(offsetof(ExecutionContext, main_ram_last_word))) ||
					(width_bytes > sizeof(u32) &&
						(!m_code.EmitAddImm32(RETURN_VALUE, SCRATCH,
							width_bytes - sizeof(u32), true) ||
						 !AppendColdBranch(Condition::CS,
							 ExitReason::MemoryTranslation, source.pc, site, pending, true, true,
							 opcode))) ||
					(width_bytes == sizeof(u32) &&
						!EmitMove(RETURN_VALUE, SCRATCH)) ||
					!m_code.EmitCmpReg(RETURN_VALUE, LINK_SCRATCH) ||
					!AppendColdBranch(Condition::HI, ExitReason::MemoryTranslation,
						source.pc, site, pending, true, true, opcode))
				{
					return false;
				}

				const size_t translated = m_code.EmitBranchPlaceholder();
				const size_t identity_target = m_code.Size();
				return translated != static_cast<size_t>(-1) &&
					m_code.PatchBranch(identity, identity_target, Condition::CC) &&
					m_code.EmitLdrImm12(
						SCRATCH, CONTEXT,
						static_cast<u16>(offsetof(ExecutionContext, main_ram))) &&
					m_code.EmitAddReg(SCRATCH, SCRATCH, RETURN_VALUE) &&
					m_code.PatchBranch(translated, m_code.Size());
			}

			bool EmitLoadWord(const Block& block, const SourceInstruction& source,
				bool preflighted)
			{
				const u32 opcode = source.opcode;
				if (!(preflighted ? EmitDirectMemoryHostAddress(opcode) :
					EmitResolveMemoryHostAddress(block, source, sizeof(u32))))
					return false;

				const u32 destination = Rt(opcode);
				if (destination == 0)
					return m_code.EmitLdrImm12(LINK_SCRATCH, SCRATCH, 0);
				const GuestPair& output = Pair(destination);
				return m_code.EmitLdrImm12(output.low, SCRATCH, 0) &&
				       m_code.EmitMovRegShiftImm(output.high, output.low, ShiftType::ASR,
						   31);
			}

			bool EmitLoadDouble(const Block& block, const SourceInstruction& source,
				bool preflighted)
			{
				const u32 opcode = source.opcode;
				if (!(preflighted ? EmitDirectMemoryHostAddress(opcode) :
					EmitResolveMemoryHostAddress(block, source, sizeof(u64))))
				{
					return false;
				}
				const u32 destination = Rt(opcode);
				if (destination == 0)
					return true;
				const GuestPair& output = Pair(destination);
				if ((output.low & 1u) == 0 && output.high == output.low + 1)
					return m_code.EmitLdrdImm8(output.low, output.high, SCRATCH, 0);
				return m_code.EmitLdrImm12(output.low, SCRATCH, 0) &&
				       m_code.EmitLdrImm12(output.high, SCRATCH, sizeof(u32));
			}

			bool EmitStoreWord(const Block& block, const SourceInstruction& source,
				bool preflighted)
			{
				const u32 opcode = source.opcode;
				const u32 block_index = static_cast<u32>(&block - m_program.blocks.data());
				const RegionExecution::ExitSite* site =
					FindMemoryExitSite(block_index, source.pc);
				if (preflighted)
				{
					if (!EmitDirectMemoryHostAddress(opcode))
						return false;
					if (Rt(opcode) == 0)
						return m_code.EmitMovImm8(RETURN_VALUE, 0) &&
						       m_code.EmitStrImm12(RETURN_VALUE, SCRATCH, 0);
					const GuestPair& source_pair = Pair(Rt(opcode));
					return source_pair.valid ?
						m_code.EmitStrImm12(source_pair.low, SCRATCH, 0) :
						(EmitGuestWord(Rt(opcode), false, LINK_SCRATCH) &&
						 m_code.EmitStrImm12(LINK_SCRATCH, SCRATCH, 0));
				}
				if (!EmitResolveMemoryHostAddress(block, source, sizeof(u32)))
					return false;

				// An aligned word cannot cross a 64-byte source chunk. Preserve the
				// translated host pointer in lr, derive its physical RAM offset, and
				// consult the exact tier-zero page/chunk ownership tables before STR.
				// The overwhelmingly common data-page path is one byte load, compare,
				// and correctly predicted branch. A positive chunk exits before the
				// write; tier zero performs both the store and exact invalidation.
				if (!EmitMove(LINK_SCRATCH, SCRATCH) ||
					!m_code.EmitLdrImm12(SCRATCH, CONTEXT,
						static_cast<u16>(offsetof(ExecutionContext, main_ram))) ||
					!m_code.EmitSubReg(SCRATCH, LINK_SCRATCH, SCRATCH) ||
					!m_code.EmitLdrImm12(RETURN_VALUE, CONTEXT,
						static_cast<u16>(offsetof(
							ExecutionContext, ram_source_page_live_flags))) ||
					!m_code.EmitLdrbRegShift(RETURN_VALUE, RETURN_VALUE, SCRATCH,
						ShiftType::LSR, SOURCE_PAGE_SHIFT) ||
					!m_code.EmitCmpImm32(RETURN_VALUE, 0))
				{
					return false;
				}
				const size_t data_page =
					m_code.EmitBranchPlaceholder(Condition::EQ);
				if (data_page == static_cast<size_t>(-1) ||
					!m_code.EmitLdrImm12(RETURN_VALUE, CONTEXT,
						static_cast<u16>(offsetof(
							ExecutionContext, ram_source_chunk_live_bits))) ||
					!m_code.EmitLdrbRegShift(RETURN_VALUE, RETURN_VALUE, SCRATCH,
						ShiftType::LSR, SOURCE_CHUNK_SHIFT + 3) ||
					!m_code.EmitMovRegShiftImm(SCRATCH, SCRATCH, ShiftType::LSR,
						SOURCE_CHUNK_SHIFT) ||
					!m_code.EmitAndImm8(SCRATCH, SCRATCH, 7) ||
					!m_code.EmitMovRegShiftReg(RETURN_VALUE, RETURN_VALUE,
						ShiftType::LSR, SCRATCH) ||
					!m_code.EmitTstImm32(RETURN_VALUE, 1))
				{
					return false;
				}
				const u32 pending = PendingRawBefore(block, source.pc);
				if (!AppendColdBranch(Condition::NE, ExitReason::SelfModifyingCode,
						source.pc, site, pending, true, true, opcode) ||
					!m_code.PatchBranch(data_page, m_code.Size(), Condition::EQ))
				{
					return false;
				}

				if (Rt(opcode) == 0)
					return m_code.EmitMovImm8(SCRATCH, 0) &&
					       m_code.EmitStrImm12(SCRATCH, LINK_SCRATCH, 0);
				const GuestPair& source_pair = Pair(Rt(opcode));
				return source_pair.valid ?
					m_code.EmitStrImm12(source_pair.low, LINK_SCRATCH, 0) :
					(EmitGuestWord(Rt(opcode), false, SCRATCH) &&
					 m_code.EmitStrImm12(SCRATCH, LINK_SCRATCH, 0));
			}

			bool EmitStoreDouble(const Block& block,
				const SourceInstruction& source, bool preflighted)
			{
				const u32 opcode = source.opcode;
				const u32 block_index = static_cast<u32>(&block - m_program.blocks.data());
				const RegionExecution::ExitSite* site =
					FindMemoryExitSite(block_index, source.pc);
				if (!(preflighted ? EmitDirectMemoryHostAddress(opcode) :
					EmitResolveMemoryHostAddress(block, source, sizeof(u64))))
				{
					return false;
				}

				// The preflight owns source overlap before the block begins. A
				// restartable first-instruction SD still needs the same exact live-source
				// check as SW before committing either word.
				if (!preflighted)
				{
					if (!EmitMove(LINK_SCRATCH, SCRATCH) ||
						!m_code.EmitLdrImm12(SCRATCH, CONTEXT,
							static_cast<u16>(offsetof(ExecutionContext, main_ram))) ||
						!m_code.EmitSubReg(SCRATCH, LINK_SCRATCH, SCRATCH) ||
						!m_code.EmitLdrImm12(RETURN_VALUE, CONTEXT,
							static_cast<u16>(offsetof(
								ExecutionContext, ram_source_page_live_flags))) ||
						!m_code.EmitLdrbRegShift(RETURN_VALUE, RETURN_VALUE, SCRATCH,
							ShiftType::LSR, SOURCE_PAGE_SHIFT) ||
						!m_code.EmitCmpImm32(RETURN_VALUE, 0))
					{
						return false;
					}
					const size_t data_page =
						m_code.EmitBranchPlaceholder(Condition::EQ);
					if (data_page == static_cast<size_t>(-1) ||
						!m_code.EmitLdrImm12(RETURN_VALUE, CONTEXT,
							static_cast<u16>(offsetof(
								ExecutionContext, ram_source_chunk_live_bits))) ||
						!m_code.EmitLdrbRegShift(RETURN_VALUE, RETURN_VALUE, SCRATCH,
							ShiftType::LSR, SOURCE_CHUNK_SHIFT + 3) ||
						!m_code.EmitMovRegShiftImm(SCRATCH, SCRATCH,
							ShiftType::LSR, SOURCE_CHUNK_SHIFT) ||
						!m_code.EmitAndImm8(SCRATCH, SCRATCH, 7) ||
						!m_code.EmitMovRegShiftReg(RETURN_VALUE, RETURN_VALUE,
							ShiftType::LSR, SCRATCH) ||
						!m_code.EmitTstImm32(RETURN_VALUE, 1) ||
						!AppendColdBranch(Condition::NE,
							ExitReason::SelfModifyingCode, source.pc,
							site, PendingRawBefore(block, source.pc), true, true, opcode) ||
						!m_code.PatchBranch(data_page, m_code.Size(), Condition::EQ))
					{
						return false;
					}
				}

				const unsigned address = preflighted ? SCRATCH : LINK_SCRATCH;
				if (Rt(opcode) == 0)
				{
					return m_code.EmitMovImm8(RETURN_VALUE, 0) &&
					       m_code.EmitStrImm12(RETURN_VALUE, address, 0) &&
					       m_code.EmitStrImm12(RETURN_VALUE, address, sizeof(u32));
				}
				const GuestPair& source_pair = Pair(Rt(opcode));
				if ((source_pair.low & 1u) == 0 &&
					source_pair.high == source_pair.low + 1)
				{
					return m_code.EmitStrdImm8(
						source_pair.low, source_pair.high, address, 0);
				}
				return m_code.EmitStrImm12(source_pair.low, address, 0) &&
				       m_code.EmitStrImm12(
					   source_pair.high, address, sizeof(u32));
			}

			bool EmitCountedRangeHostBase(const BlockMemoryPlan& plan)
			{
				return m_code.EmitLdrImm12(LINK_SCRATCH, CONTEXT,
						static_cast<u16>(offsetof(ExecutionContext, main_ram))) &&
				       m_code.EmitAddReg(LINK_SCRATCH, LINK_SCRATCH,
						   Pair(plan.induction_guest).low);
			}

			bool EmitCountedRangeStore(const BlockMemoryPlan& plan,
				const SourceInstruction& source)
			{
				const auto access = std::find_if(plan.word_accesses.begin(),
					plan.word_accesses.end(), [&](const AffineMemoryAccess& candidate) {
						return candidate.source_pc == source.pc;
					});
				if (access == plan.word_accesses.end() || !access->store ||
					access->base_guest != plan.induction_guest || access->offset > 0x0fffu)
				{
					return false;
				}
				const u32 source_guest = Rt(source.opcode);
				if (source_guest == 0)
				{
					return m_code.EmitMovImm8(RETURN_VALUE, 0) &&
					       m_code.EmitStrImm12(RETURN_VALUE, LINK_SCRATCH,
							   static_cast<u16>(access->offset));
				}
				const GuestPair& source_pair = Pair(source_guest);
				return source_pair.valid ?
					m_code.EmitStrImm12(source_pair.low, LINK_SCRATCH,
						static_cast<u16>(access->offset)) :
					(EmitGuestWord(source_guest, false, RETURN_VALUE) &&
					 m_code.EmitStrImm12(RETURN_VALUE, LINK_SCRATCH,
						static_cast<u16>(access->offset)));
			}

			bool EmitBodyInstruction(const Block& block,
				const SourceInstruction& source, const BlockMemoryPlan& plan)
			{
				const u32 opcode = source.opcode;
				if (opcode == 0)
					return true;
					switch (opcode >> 26)
				{
					case 0x09:
						return EmitAddiu(opcode);
					case 0x0a:
						return EmitSlti(opcode);
					case 0x0c:
						return EmitAndi(opcode);
					case 0x23:
						return EmitLoadWord(block, source, plan.preflight);
					case 0x37:
						return EmitLoadDouble(block, source, plan.preflight);
					case 0x2b:
						return plan.entry_counted_range ?
							EmitCountedRangeStore(plan, source) :
							EmitStoreWord(block, source, plan.preflight);
					case 0x3f:
						return EmitStoreDouble(block, source, plan.preflight);
					case 0:
						switch (opcode & 0x3f)
						{
							case 0x00:
								return EmitSll(opcode);
							case 0x21:
								return EmitAddu(opcode);
							case 0x2b:
								return EmitSltu(opcode);
							default:
								return false;
						}
					default:
						return false;
				}
			}

			bool ResolveTransferPc(const Block& block, const Transfer& transfer,
				u32* pc) const
			{
				for (const Node& node : block.nodes)
				{
					if (node.id == transfer.pc && node.opcode == Opcode::ConstantAddress)
					{
						*pc = static_cast<u32>(node.literal);
						return true;
					}
				}
				return false;
			}

			bool EmitEdge(const Block& block, const Transfer& transfer,
				u32 scaled_cycles)
			{
				const RegionExecution::ExitSite* site =
					FindTransferExitSite(block, transfer);
				if (!site)
					return false;
				if (!transfer.cycle_commit_deferred && !EmitAddCycles(scaled_cycles))
					return false;
				u32 pc = 0;
				if (!ResolveTransferPc(block, transfer, &pc))
					return false;
				if (transfer.event_horizon_check)
				{
					if (!EmitUnsigned64AtLeastNextEvent() ||
						!AppendColdBranch(Condition::CS, ExitReason::EventHorizon, pc,
							site, transfer.pending_raw_cycles,
							transfer.cycle_commit_deferred))
					{
						return false;
					}
				}
				if (transfer.target_block != INVALID_BLOCK)
				{
					const size_t branch = m_code.EmitBranchPlaceholder();
					if (branch == static_cast<size_t>(-1))
						return false;
					m_internal_patches.push_back({branch, transfer.target_block});
					return true;
				}
				ColdExit exit{};
				exit.reason = transfer.external_reason;
				exit.pc = pc;
				exit.pending_raw_cycles = transfer.pending_raw_cycles;
				exit.cycle_commit_deferred = transfer.cycle_commit_deferred;
				exit.state_site = site;
				return EmitExit(exit);
			}

			Condition TakenCondition(u32 opcode) const
			{
				const u32 primary = opcode >> 26;
				return primary == 0x04 || primary == 0x14 ? Condition::EQ : Condition::NE;
			}

			bool EmitBlock(u32 block_index)
			{
				const Block& block = m_program.blocks[block_index];
				const BlockMemoryPlan& memory_plan =
					m_block_memory_plans[block_index];
				if (!EmitBlockMemoryPreflight(block_index))
					return false;
				if (memory_plan.entry_counted_range &&
					!EmitCountedRangeHostBase(memory_plan))
				{
					return false;
				}
				if (block.terminator.kind == TerminatorKind::Transfer)
				{
					for (const SourceInstruction& source : block.source)
					{
						if (!EmitBodyInstruction(block, source, memory_plan))
							return static_cast<bool>(Fail(CompileFailure::Emission, source.pc));
					}
					return EmitEdge(block, block.terminator.taken, block.scaled_cycle_cost);
				}
				// Static and register calls are owned by the region-wide allocated
				// backend, which can carry r31 and edge state across the complete unit.
				// The legacy block-local emitter must fail closed rather than decode a
				// J/JR pair as a conditional branch.
				if (block.terminator.kind != TerminatorKind::Branch)
					return static_cast<bool>(Fail(CompileFailure::UnsupportedInstruction,
						block.terminator.branch_pc));

				if (block.source.size() < 2)
					return false;
				const size_t body_count = block.source.size() - 2;
				for (size_t index = 0; index < body_count; index++)
				{
					if (!EmitBodyInstruction(
							block, block.source[index], memory_plan))
						return static_cast<bool>(
							Fail(CompileFailure::Emission, block.source[index].pc));
				}

				const SourceInstruction& branch = block.source[body_count];
				const SourceInstruction& delay = block.source[body_count + 1];
				if (!EmitCompareEqual64(Rs(branch.opcode), Rt(branch.opcode)))
					return false;
				const Condition condition = TakenCondition(branch.opcode);
				const size_t taken = m_code.EmitBranchPlaceholder(condition);
				if (taken == static_cast<size_t>(-1))
					return false;

				if (!block.terminator.likely &&
					!EmitBodyInstruction(block, delay, memory_plan))
				{
					return static_cast<bool>(Fail(CompileFailure::Emission, delay.pc));
				}
				if (!EmitEdge(block, block.terminator.not_taken,
						block.not_taken_scaled_cycle_cost))
				{
					return false;
				}

				const size_t taken_target = m_code.Size();
				if (!m_code.PatchBranch(taken, taken_target, condition) ||
					!EmitBodyInstruction(block, delay, memory_plan) ||
					!EmitEdge(block, block.terminator.taken, block.scaled_cycle_cost))
				{
					return false;
				}
				return true;
			}

			bool ResolveColdExitContract(const ColdExit& exit,
				RegionExecution::ExitContractView* contract) const
			{
				if (!exit.state_site ||
					!RegionExecution::ResolveExitContract(
						m_program, *exit.state_site, contract) || !contract->state)
				{
					return false;
				}
				u32 contract_pc = contract->static_pc;
				if (contract->transfer &&
					!ResolveTransferPc(m_program.blocks[exit.state_site->block],
						*contract->transfer, &contract_pc))
				{
					return false;
				}
				return contract_pc == exit.pc &&
				       contract->cycle_commit_deferred == exit.cycle_commit_deferred &&
				       contract->pending_raw_cycles == exit.pending_raw_cycles &&
				       (exit.reason != ExitReason::EventHorizon ||
						contract->event_horizon_check);
			}

			bool EmitMaterializeState(const ColdExit& exit)
			{
				RegionExecution::ExitContractView contract{};
				if (!ResolveColdExitContract(exit, &contract))
					return false;
				if (!m_code.EmitLdrImm12(
						RETURN_VALUE, CONTEXT,
						static_cast<u16>(offsetof(ExecutionContext, state))))
				{
					return false;
				}
				for (u32 guest = 1; guest < 32; guest++)
				{
					if (!exit.state_site->dirty_state.test(guest))
						continue;
					const GuestPair& pair = Pair(guest);
					if (!pair.valid || !pair.high_valid)
						return false;
					const size_t offset = StateGprLowOffset(guest);
					if (!m_code.EmitStrImm12(pair.low, RETURN_VALUE,
							static_cast<u16>(offset)) ||
						!m_code.EmitStrImm12(pair.high, RETURN_VALUE,
							static_cast<u16>(offset + sizeof(u32))))
					{
						return false;
					}
				}
				return m_code.EmitStrImm12(
						   CYCLE_LOW, RETURN_VALUE,
						   static_cast<u16>(offsetof(CanonicalState, cycle))) &&
				       m_code.EmitStrImm12(
						   CYCLE_HIGH, RETURN_VALUE,
						   static_cast<u16>(offsetof(CanonicalState, cycle) +
											sizeof(u32))) &&
				       m_code.EmitMovImm32(SCRATCH, exit.pc) &&
				       m_code.EmitStrImm12(SCRATCH, RETURN_VALUE,
						   static_cast<u16>(offsetof(CanonicalState, pc)));
			}

			bool EmitExit(const ColdExit& exit)
			{
				// Memory exits arrive with the exact guest effective address in r0.
				// The store source-ownership guard deliberately reuses r0 on its hot
				// path; its rare exit rebuilds that EA from the still-resident base.
				// Materialization needs r0 for the canonical-state pointer, so retain
				// the address in the otherwise-dead saved link register first.
				if (exit.publish_memory_address &&
					((exit.memory_address_opcode != 0 &&
						!EmitEffectiveAddress(exit.memory_address_opcode, RETURN_VALUE)) ||
					 (exit.memory_address_opcode == 0 && exit.memory_address_guest != 0 &&
						!EmitGuestWord(exit.memory_address_guest, false, RETURN_VALUE)) ||
					 !EmitMove(LINK_SCRATCH, RETURN_VALUE)))
				{
					return false;
				}

				if (!EmitMaterializeState(exit) || !m_code.EmitMovImm8(SCRATCH, 1) ||
					!m_code.EmitStrImm12(SCRATCH, CONTEXT,
						static_cast<u16>(ResultOffset(
							offsetof(ExecutionResult, completed)))) ||
					!m_code.EmitMovImm32(SCRATCH, static_cast<u32>(exit.reason)) ||
					!m_code.EmitStrImm12(SCRATCH, CONTEXT,
						static_cast<u16>(ResultOffset(
							offsetof(ExecutionResult, reason)))) ||
					!m_code.EmitMovImm8(SCRATCH, exit.cycle_commit_deferred ? 1 : 0) ||
					!m_code.EmitStrImm12(SCRATCH, CONTEXT,
						static_cast<u16>(ResultOffset(offsetof(
							ExecutionResult, cycle_commit_deferred)))) ||
					!m_code.EmitMovImm32(SCRATCH, exit.pending_raw_cycles) ||
					!m_code.EmitStrImm12(SCRATCH, CONTEXT,
						static_cast<u16>(ResultOffset(offsetof(
							ExecutionResult, pending_raw_cycles)))))
				{
					return false;
				}

				const unsigned memory_address =
					exit.publish_memory_address ? LINK_SCRATCH : RETURN_VALUE;
				if (!exit.publish_memory_address && !m_code.EmitMovImm8(RETURN_VALUE, 0))
				{
					return false;
				}
				if (!m_code.EmitStrImm12(memory_address, CONTEXT,
						static_cast<u16>(ResultOffset(
							offsetof(ExecutionResult, memory_address)))) ||
					!m_code.EmitMovImm8(RETURN_VALUE, 1) ||
					!m_code.EmitPop(RESTORED_REGISTERS))
				{
					return false;
				}
				return true;
			}

			const Program& m_program;
			VitaA32::CodeBuffer& m_code;
			CompileOptions m_options{};
			CompileResult m_result{};
			RegionExecution::Plan m_execution_plan{};
			RegionAllocation::Plan m_allocation_plan{};
			std::array<GuestPair, 32> m_guest_pairs{};
			std::array<u32, 32> m_guest_first_pc{};
			std::array<u32, 32> m_guest_write_pc{};
			std::array<u32, 32> m_guest_low_uses{};
			std::array<bool, 32> m_guest_high_read{};
			std::array<bool, 32> m_guest_observed{};
			std::array<bool, 32> m_guest_written{};
			std::vector<size_t> m_block_offsets;
			std::vector<BlockMemoryPlan> m_block_memory_plans;
			std::vector<InternalPatch> m_internal_patches;
			std::vector<ColdExit> m_cold_exits;
		};
	} // namespace

	CompileResult Compile(const Program& program, VitaA32::CodeBuffer& code,
		const CompileOptions& options)
	{
		return Compiler(program, code, options).Run();
	}

	const char* CompileFailureName(CompileFailure failure)
	{
		switch (failure)
		{
			case CompileFailure::None:
				return "none";
			case CompileFailure::InvalidProgram:
				return "invalid-program";
			case CompileFailure::UnattestedSource:
				return "unattested-source";
			case CompileFailure::UnsupportedInstruction:
				return "unsupported-instruction";
			case CompileFailure::UnsupportedMemory:
				return "unsupported-memory";
			case CompileFailure::RegisterPressure:
				return "register-pressure";
			case CompileFailure::CodeCapacity:
				return "code-capacity";
			case CompileFailure::Emission:
				return "emission";
			case CompileFailure::Patch:
				return "patch";
			case CompileFailure::UnsupportedStateContract:
				return "unsupported-state-contract";
			case CompileFailure::UnprovenProfitability:
				return "unproven-profitability";
		}
		return "unknown";
	}
} // namespace VitaEE::RegionA32
