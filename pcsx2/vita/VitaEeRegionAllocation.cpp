// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#include "PrecompiledHeader.h"

#include "pcsx2/vita/VitaEeRegionAllocation.h"

#include <algorithm>
#include <array>
#include <bitset>

namespace VitaEE::RegionAllocation
{
	namespace
	{
		constexpr size_t EDGE_STATE_SLOTS = RegionExecution::STATE_SLOT_COUNT + 1;

		struct Range
		{
			u32 block = RegionIR::INVALID_BLOCK;
			u32 begin = 0;
			u32 end = 0;
			u32 uses = 0;
		};

		struct InternalEdge
		{
			u32 source_block = RegionIR::INVALID_BLOCK;
			const RegionIR::Transfer* transfer = nullptr;
			u8 edge_index = 0;
			std::bitset<EDGE_STATE_SLOTS> propagated{};
		};

		BuildResult Fail(BuildFailure failure, u32 block,
			RegionIR::ValueId value)
		{
			BuildResult result{};
			result.failure = failure;
			result.block = block;
			result.value = value;
			return result;
		}

		bool IsConstant(RegionIR::Opcode opcode)
		{
			return opcode == RegionIR::Opcode::ConstantI1 ||
			       opcode == RegionIR::Opcode::ConstantI32 ||
			       opcode == RegionIR::Opcode::ConstantI64 ||
			       opcode == RegionIR::Opcode::ConstantAddress;
		}

		bool IsArchitecturalZeroParameter(const RegionIR::Program& program,
			const RegionIR::Node& node)
		{
			if (node.opcode != RegionIR::Opcode::Parameter)
				return false;
			for (const RegionIR::Block& block : program.blocks)
			{
				if (block.parameters.gpr[0] == node.id)
					return true;
			}
			return false;
		}

		bool IsKnownZeroValue(const RegionIR::Program& program,
			const std::vector<const RegionIR::Node*>& definitions,
			RegionIR::ValueId value)
		{
			for (u32 depth = 0; depth < definitions.size(); depth++)
			{
				if (value >= definitions.size() || !definitions[value])
					return false;
				const RegionIR::Node& node = *definitions[value];
				if (IsConstant(node.opcode))
					return node.literal == 0;
				if (IsArchitecturalZeroParameter(program, node))
					return true;
				switch (node.opcode)
				{
					case RegionIR::Opcode::ExtractLow32:
					case RegionIR::Opcode::ExtractLow64:
					case RegionIR::Opcode::BitcastI32ToF32Bits:
					case RegionIR::Opcode::BitcastF32BitsToI32:
					case RegionIR::Opcode::AddressFromI32:
					case RegionIR::Opcode::SignExtend32To64:
					case RegionIR::Opcode::ZeroExtend32To64:
						if (node.operand_count != 1)
							return false;
						value = node.operands[0];
						break;
					default:
						return false;
				}
			}
			return false;
		}

		struct AffineI32
		{
			RegionIR::ValueId base = RegionIR::INVALID_VALUE;
			u32 delta = 0;
			bool valid = false;
		};

		bool ConstantI32(const std::vector<const RegionIR::Node*>& definitions,
			RegionIR::ValueId value, u32* literal)
		{
			if (!literal || value >= definitions.size() || !definitions[value])
				return false;
			const RegionIR::Node& node = *definitions[value];
			if (node.opcode != RegionIR::Opcode::ConstantI32 ||
				node.type != RegionIR::ValueType::I32)
			{
				return false;
			}
			*literal = static_cast<u32>(node.literal);
			return true;
		}

		AffineI32 ResolveAffineI32(
			const std::vector<const RegionIR::Node*>& definitions,
			RegionIR::ValueId value, u32 depth);

		AffineI32 ResolveLow32View(
			const std::vector<const RegionIR::Node*>& definitions,
			RegionIR::ValueId value, RegionIR::ValueId fallback, u32 depth)
		{
			if (depth >= definitions.size() || value >= definitions.size() ||
				!definitions[value])
			{
				return {fallback, 0, fallback != RegionIR::INVALID_VALUE};
			}
			const RegionIR::Node& node = *definitions[value];
			if (node.type == RegionIR::ValueType::I32)
				return ResolveAffineI32(definitions, value, depth + 1);
			if (node.type == RegionIR::ValueType::I64)
			{
				if ((node.opcode == RegionIR::Opcode::SignExtend32To64 ||
					 node.opcode == RegionIR::Opcode::ZeroExtend32To64) &&
					node.operand_count == 1)
				{
					return ResolveAffineI32(definitions, node.operands[0], depth + 1);
				}
				if (node.opcode == RegionIR::Opcode::ExtractLow64 &&
					node.operand_count == 1)
				{
					return ResolveLow32View(definitions, node.operands[0], fallback,
						depth + 1);
				}
			}
			if (node.type == RegionIR::ValueType::I128 &&
				node.opcode == RegionIR::Opcode::ReplaceLow64 &&
				node.operand_count == 2)
			{
				return ResolveLow32View(definitions, node.operands[1], fallback,
					depth + 1);
			}
			return {fallback, 0, fallback != RegionIR::INVALID_VALUE};
		}

		AffineI32 ResolveAffineI32(
			const std::vector<const RegionIR::Node*>& definitions,
			RegionIR::ValueId value, u32 depth)
		{
			if (depth >= definitions.size() || value >= definitions.size() ||
				!definitions[value])
			{
				return {};
			}
			const RegionIR::Node& node = *definitions[value];
			if (node.type != RegionIR::ValueType::I32)
				return {};

			if (node.opcode == RegionIR::Opcode::ExtractLow32 &&
				node.operand_count == 1)
			{
				return ResolveLow32View(definitions, node.operands[0], value, depth + 1);
			}

			u32 literal = 0;
			RegionIR::ValueId input = RegionIR::INVALID_VALUE;
			u32 delta = 0;
			if (node.opcode == RegionIR::Opcode::Add32 && node.operand_count == 2)
			{
				if (ConstantI32(definitions, node.operands[1], &literal))
				{
					input = node.operands[0];
					delta = literal;
				}
				else if (ConstantI32(definitions, node.operands[0], &literal))
				{
					input = node.operands[1];
					delta = literal;
				}
			}
			else if (node.opcode == RegionIR::Opcode::Sub32 &&
				node.operand_count == 2 &&
				ConstantI32(definitions, node.operands[1], &literal))
			{
				input = node.operands[0];
				delta = 0u - literal;
			}
			if (input == RegionIR::INVALID_VALUE)
				return {value, 0, true};

			AffineI32 source = ResolveAffineI32(definitions, input, depth + 1);
			if (!source.valid)
				return {};
			source.delta += delta;
			return source;
		}

		std::vector<RegionIR::ValueId> BuildExactI32Aliases(
			const RegionIR::Program& program,
			const std::vector<const RegionIR::Node*>& definitions,
			const std::vector<Range>& ranges)
		{
			std::vector<RegionIR::ValueId> aliases(program.value_count,
				RegionIR::INVALID_VALUE);
			for (const RegionIR::Block& block : program.blocks)
			{
				for (const RegionIR::Node& node : block.nodes)
				{
					if (node.type != RegionIR::ValueType::I32 ||
						(node.opcode != RegionIR::Opcode::Add32 &&
						 node.opcode != RegionIR::Opcode::Sub32))
					{
						continue;
					}
					const AffineI32 affine = ResolveAffineI32(definitions, node.id, 0);
					if (!affine.valid || affine.delta != 0 || affine.base >= node.id ||
						affine.base >= ranges.size() ||
						ranges[affine.base].block != ranges[node.id].block ||
						!definitions[affine.base] ||
						definitions[affine.base]->type != RegionIR::ValueType::I32)
					{
						continue;
					}
					aliases[node.id] = affine.base;
				}
			}
			return aliases;
		}

		u32 StorageBytes(RegionIR::ValueType type);

		u8 FullWordMask(RegionIR::ValueType type)
		{
			const u32 bytes = StorageBytes(type);
			const u32 words = bytes / sizeof(u32);
			return words == 0 ? 0 : static_cast<u8>((1u << words) - 1u);
		}

		bool IsBinding(RegionIR::Opcode opcode)
		{
			return opcode == RegionIR::Opcode::BindGpr ||
			       opcode == RegionIR::Opcode::BindHi ||
			       opcode == RegionIR::Opcode::BindLo ||
			       opcode == RegionIR::Opcode::BindSa ||
			       opcode == RegionIR::Opcode::BindFpr ||
			       opcode == RegionIR::Opcode::BindVu0Vf ||
			       opcode == RegionIR::Opcode::BindVu0Acc ||
			       opcode == RegionIR::Opcode::BindVu0MacFlag ||
		       opcode == RegionIR::Opcode::BindVu0StatusFlag ||
		       opcode == RegionIR::Opcode::BindVu0ViMac ||
		       opcode == RegionIR::Opcode::BindVu0ViStatus ||
		       opcode == RegionIR::Opcode::BindVu0Q ||
			       opcode == RegionIR::Opcode::BindVu0ViQ ||
			       opcode == RegionIR::Opcode::BindVu0Vi ||
			       opcode == RegionIR::Opcode::BindVu0ClipFlag ||
			       opcode == RegionIR::Opcode::BindVu0MicroStatusFlag ||
		       opcode == RegionIR::Opcode::BindFcr31 ||
			       opcode == RegionIR::Opcode::BindAcc;
		}

		bool IsDirectBranchComparison(RegionIR::Opcode opcode)
		{
			switch (opcode)
			{
				case RegionIR::Opcode::CompareEqual64:
				case RegionIR::Opcode::CompareNotEqual64:
				case RegionIR::Opcode::CompareSignedLess64:
				case RegionIR::Opcode::CompareUnsignedLess64:
				case RegionIR::Opcode::CompareSignedLessEqualZero64:
				case RegionIR::Opcode::CompareSignedGreaterZero64:
				case RegionIR::Opcode::CompareSignedLessZero64:
				case RegionIR::Opcode::CompareSignedGreaterEqualZero64:
				case RegionIR::Opcode::Cop1BranchCondition:
					return true;
				default:
					return false;
			}
		}

		u8 MemoryDataMask(RegionIR::MemoryAccessKind kind)
		{
			switch (kind)
			{
				case RegionIR::MemoryAccessKind::Store8:
				case RegionIR::MemoryAccessKind::Store16:
				case RegionIR::MemoryAccessKind::Store32:
				case RegionIR::MemoryAccessKind::StoreF32Bits:
					return 0x1;
				case RegionIR::MemoryAccessKind::Store64:
					return 0x3;
				case RegionIR::MemoryAccessKind::Store128:
				case RegionIR::MemoryAccessKind::StoreVu0Vector:
					return 0x0f;
				default:
					return 0;
			}
		}

		bool IsPreflightedMemoryExit(const RegionIR::Program& program,
			const RegionExecution::ExitSite& site,
			const std::vector<RegionIR::ValueId>* preflighted_operations)
		{
			if (site.kind != RegionExecution::ExitSiteKind::Memory ||
				!preflighted_operations || site.block >= program.blocks.size() ||
				site.ordinal >= program.blocks[site.block].memory_exits.size())
			{
				return false;
			}
			const RegionIR::ValueId operation =
				program.blocks[site.block].memory_exits[site.ordinal].operation;
			return std::find(preflighted_operations->begin(),
				preflighted_operations->end(), operation) !=
				preflighted_operations->end();
		}

		bool IsForwardedMemoryExit(const RegionIR::Program& program,
			const RegionExecution::ExitSite& site,
			const std::vector<u8>* forwarded_memory_load_effect)
		{
			if (site.kind != RegionExecution::ExitSiteKind::Memory ||
				!forwarded_memory_load_effect || site.block >= program.blocks.size() ||
				site.ordinal >= program.blocks[site.block].memory_exits.size())
			{
				return false;
			}
			const RegionIR::ValueId operation =
				program.blocks[site.block].memory_exits[site.ordinal].operation;
			return operation < forwarded_memory_load_effect->size() &&
				(*forwarded_memory_load_effect)[operation] != 0;
		}

		bool IsNonExecutableInternalExit(const RegionIR::Program& program,
			const RegionExecution::ExitSite& site, u32 aggregate_cycle_header_block)
		{
			// The semantic plan retains the generic unmatched register-jump exit.
			// A verified direct callee cannot take it: every possible r31 value was
			// produced by one of the attested internal JAL edges and is represented
			// by exactly one register_targets transfer.
			if (site.kind == RegionExecution::ExitSiteKind::Taken &&
				RegionIR::HasExhaustiveDirectReturnTargets(program, site.block))
			{
				const RegionIR::Terminator& terminator =
					program.blocks[site.block].terminator;
				const bool every_return_event_is_suppressed = std::all_of(
					terminator.register_targets.begin(),
					terminator.register_targets.end(),
					[&](const RegionIR::Transfer& target) {
						return !target.event_horizon_check ||
							(aggregate_cycle_header_block != RegionIR::INVALID_BLOCK &&
							 target.target_block != aggregate_cycle_header_block);
					});
				if (every_return_event_is_suppressed)
					return true;
			}
			if (site.kind != RegionExecution::ExitSiteKind::Taken &&
				site.kind != RegionExecution::ExitSiteKind::NotTaken)
			{
				return false;
			}
			const RegionIR::Transfer* const transfer =
				RegionExecution::ResolveTransfer(program, site);
			if (!transfer || transfer->target_block == RegionIR::INVALID_BLOCK)
				return false;
			// An internal edge has no ordinary side exit. Its only possible cold leaf
			// is the PCSX2 scheduler check; an explicitly non-observing transfer or an
			// aggregate proof which suppresses this intermediate check makes the
			// RegionExecution exit contract non-executable in this backend.
			return !transfer->event_horizon_check ||
				(aggregate_cycle_header_block != RegionIR::INVALID_BLOCK &&
				 transfer->target_block != aggregate_cycle_header_block);
		}

		// RegionExecution retains the complete semantic exit contract, including
		// every per-access memory failure.  An allocated backend may prove a subset
		// of those failures impossible with one entry range certificate.  Rebuild
		// the allocator's exit demand from the exits it can actually emit instead of
		// inheriting the semantic union: otherwise values needed only by an
		// impossible mid-region failure occupy registers and spill slots throughout
		// the hot loop.  No exit map is weakened or removed from the verifier plan.
		bool BuildBackendExitWordDemands(const RegionIR::Program& program,
			const RegionExecution::Plan& execution,
			const std::vector<RegionIR::ValueId>* preflighted_operations,
			const std::vector<u8>* forwarded_memory_load_effect,
			u32 aggregate_cycle_header_block,
			std::vector<u8>* output, u32* executable_sites = nullptr,
			u32* executable_state_bindings = nullptr,
			u32* executable_state_words = nullptr,
			u64* executable_sites_by_kind = nullptr,
			u64* executable_state_words_by_kind = nullptr,
			u64* executable_control_sites_by_target = nullptr,
			u64* executable_control_state_words_by_target = nullptr,
			std::vector<u32>* executable_sites_by_block = nullptr,
			std::vector<u32>* executable_state_words_by_block = nullptr)
		{
			if (!output)
				return false;
			if (executable_sites)
				*executable_sites = 0;
			if (executable_state_bindings)
				*executable_state_bindings = 0;
			if (executable_state_words)
				*executable_state_words = 0;
			if (executable_sites_by_kind)
				*executable_sites_by_kind = 0;
			if (executable_state_words_by_kind)
				*executable_state_words_by_kind = 0;
			if (executable_control_sites_by_target)
				*executable_control_sites_by_target = 0;
			if (executable_control_state_words_by_target)
				*executable_control_state_words_by_target = 0;
			if (executable_sites_by_block)
				executable_sites_by_block->assign(program.blocks.size(), 0);
			if (executable_state_words_by_block)
				executable_state_words_by_block->assign(program.blocks.size(), 0);
			output->assign(program.value_count, 0);
			auto demand = [&](RegionIR::ValueId value, u8 mask) {
				if (value >= output->size())
					return false;
				(*output)[value] |= mask;
				return true;
			};
			for (const RegionExecution::ExitSite& site : execution.exits)
			{
				if (site.kind == RegionExecution::ExitSiteKind::BlockEntry ||
					IsPreflightedMemoryExit(program, site, preflighted_operations) ||
					IsForwardedMemoryExit(program, site,
						forwarded_memory_load_effect) ||
					IsNonExecutableInternalExit(program, site,
						aggregate_cycle_header_block))
				{
					continue;
				}
				RegionExecution::ExitContractView contract{};
				if (!RegionExecution::ResolveExitContract(program, site, &contract) ||
					!contract.state)
				{
					return false;
				}
				if (executable_sites)
					(*executable_sites)++;
				if (executable_sites_by_block && site.block < program.blocks.size())
					(*executable_sites_by_block)[site.block]++;
				u32 kind_lane = 0;
				switch (site.kind)
				{
					case RegionExecution::ExitSiteKind::EntryEvent:
					case RegionExecution::ExitSiteKind::EntryFallback:
						kind_lane = 0;
						break;
					case RegionExecution::ExitSiteKind::Guarded:
					case RegionExecution::ExitSiteKind::Observer:
						kind_lane = 1;
						break;
					case RegionExecution::ExitSiteKind::Memory:
						kind_lane = 2;
						break;
					case RegionExecution::ExitSiteKind::Taken:
					case RegionExecution::ExitSiteKind::NotTaken:
						kind_lane = 3;
						break;
					case RegionExecution::ExitSiteKind::BlockEntry:
						return false;
				}
				if (executable_sites_by_kind)
					*executable_sites_by_kind += 1ull << (kind_lane * 16);
				u32 control_target_lane = UINT32_MAX;
				if (kind_lane == 3)
				{
					const RegionIR::Transfer* const transfer =
						RegionExecution::ResolveTransfer(program, site);
					if (!transfer || transfer->target_block == RegionIR::INVALID_BLOCK)
						control_target_lane = 0;
					else if (transfer->target_block == aggregate_cycle_header_block)
						control_target_lane = 1;
					else
						control_target_lane = 2;
					if (executable_control_sites_by_target)
						*executable_control_sites_by_target +=
							1ull << (control_target_lane * 16);
				}
				for (size_t slot = 0; slot < RegionExecution::STATE_SLOT_COUNT; slot++)
				{
					if (!site.dirty_state.test(slot))
						continue;
					const u8 valid_words = static_cast<u8>(
						(1u << RegionExecution::StateWordCount(slot)) - 1u);
					const u8 dirty_words = site.dirty_words[slot] & valid_words;
					if (!demand(RegionExecution::StateValue(*contract.state, slot),
							dirty_words))
					{
						return false;
					}
					if (dirty_words != 0 && executable_state_bindings)
						(*executable_state_bindings)++;
					const u32 word_count = std::popcount(dirty_words);
					if (executable_state_words)
						*executable_state_words += word_count;
					if (executable_state_words_by_block &&
						site.block < program.blocks.size())
					{
						(*executable_state_words_by_block)[site.block] += word_count;
					}
					if (executable_state_words_by_kind)
						*executable_state_words_by_kind +=
							static_cast<u64>(word_count) << (kind_lane * 16);
					if (control_target_lane != UINT32_MAX &&
						executable_control_state_words_by_target)
					{
						*executable_control_state_words_by_target +=
							static_cast<u64>(word_count) <<
							(control_target_lane * 16);
					}
				}
				if (contract.transfer && !demand(contract.transfer->pc, 0x1))
					return false;
				if (contract.event_horizon_check &&
					!demand(contract.state->cycle, 0x3))
				{
					return false;
				}
			}
			return true;
		}

		bool BuildWordDemands(const RegionIR::Program& program,
			const RegionExecution::Plan& execution,
			const std::vector<const RegionIR::Node*>& definitions,
			const std::vector<RegionIR::ValueId>& cop1_normalize_alias,
			const std::vector<RegionIR::ValueId>& vu0_idle_alias,
			const std::vector<RegionIR::ValueId>& vu0_normalize_alias,
			const std::vector<u8>& vu0_hoisted_normalize,
			const std::vector<RegionIR::ValueId>& vu0_merge_alias,
			const std::vector<RegionIR::ValueId>& vu0_status_direct_raw,
			const std::vector<RegionIR::ValueId>& vu0_sync_direct_raw,
			const std::vector<RegionIR::ValueId>& vu0_add_fused_mul,
			const std::vector<RegionIR::ValueId>& vu0_folded_broadcast_source,
			const std::vector<RegionIR::ValueId>& exact_i32_aliases,
			const std::vector<RegionIR::ValueId>& forwarded_memory_load_alias,
			const std::vector<RematerializationKind>& low32_extensions,
			const std::vector<u8>& backend_exit_word_demands,
			const std::vector<RegionIR::ValueId>* hoisted_memory_operations,
			std::vector<u8>* output)
		{
			if (!output || definitions.size() != program.value_count ||
				cop1_normalize_alias.size() != program.value_count ||
				vu0_idle_alias.size() != program.value_count ||
				vu0_normalize_alias.size() != program.value_count ||
				vu0_hoisted_normalize.size() != program.value_count ||
				vu0_merge_alias.size() != program.value_count ||
				vu0_status_direct_raw.size() != program.value_count ||
				vu0_sync_direct_raw.size() != program.value_count ||
				vu0_add_fused_mul.size() != program.value_count ||
				vu0_folded_broadcast_source.size() != program.value_count ||
				exact_i32_aliases.size() != program.value_count ||
				forwarded_memory_load_alias.size() != program.value_count ||
				low32_extensions.size() != program.value_count ||
				execution.value_exit_word_uses.size() != program.value_count ||
				backend_exit_word_demands.size() != program.value_count)
			{
				return false;
			}
			*output = backend_exit_word_demands;
			auto address_is_hoisted = [&](RegionIR::ValueId operation) {
				return hoisted_memory_operations &&
					std::find(hoisted_memory_operations->begin(),
						hoisted_memory_operations->end(), operation) !=
						hoisted_memory_operations->end();
			};
			std::vector<RegionIR::ValueId> memory_value_for_effect(
				program.value_count, RegionIR::INVALID_VALUE);
			for (const RegionIR::Block& block : program.blocks)
			{
				for (const RegionIR::Node& node : block.nodes)
				{
					if (node.opcode == RegionIR::Opcode::MemoryLoadValue &&
						node.operand_count == 1 &&
						node.operands[0] < memory_value_for_effect.size())
					{
						memory_value_for_effect[node.operands[0]] = node.id;
					}
				}
			}
			auto load_is_forwarded = [&](RegionIR::ValueId operation) {
				if (operation >= memory_value_for_effect.size())
					return false;
				const RegionIR::ValueId value = memory_value_for_effect[operation];
				return value < forwarded_memory_load_alias.size() &&
					forwarded_memory_load_alias[value] != RegionIR::INVALID_VALUE;
			};
			auto demand = [&](RegionIR::ValueId value, u8 mask,
				bool* changed = nullptr) -> bool {
				if (value >= definitions.size() || !definitions[value])
					return false;
				mask &= FullWordMask(definitions[value]->type);
				const u8 before = (*output)[value];
				(*output)[value] |= mask;
				if (changed)
					*changed |= before != (*output)[value];
				return true;
			};
			// Seed executable side effects and control. Bind nodes merely name the
			// state maps already represented by execution exits and internal edges;
			// treating them as value consumers would force every scalar write into a
			// full architectural qword.
			for (const RegionIR::Block& block : program.blocks)
			{
				if (block.terminator.kind == RegionIR::TerminatorKind::Branch &&
					!demand(block.terminator.condition, 0x1))
				{
					return false;
				}
				for (const RegionIR::Node& node : block.nodes)
				{
					if (node.opcode == RegionIR::Opcode::MemoryLoad ||
						node.opcode == RegionIR::Opcode::MemoryStore)
					{
						if (node.operand_count < 2 ||
							(!load_is_forwarded(node.id) &&
							 !address_is_hoisted(node.id) &&
							 !demand(node.operands[1], 0x1)))
							return false;
						if (node.opcode == RegionIR::Opcode::MemoryStore &&
							(node.operand_count < 3 ||
							 !demand(node.operands[2], MemoryDataMask(
								 static_cast<RegionIR::MemoryAccessKind>(node.immediate)))))
						{
							return false;
						}
					}
					else if (node.opcode == RegionIR::Opcode::ExitIfTrue)
					{
						if (node.operand_count != 1 || !demand(node.operands[0], 0x1))
							return false;
					}
					else if (node.opcode ==
							RegionIR::Opcode::Cop1ExceptionalOuResult)
					{
						// This classifier controls an internal exceptional veneer.  Its
						// boolean remains in NZCV and has no allocated SSA result, but the
						// exact raw COP1 value must remain live at the classifier.
						if (node.operand_count != 1 || !demand(node.operands[0], 0x1))
							return false;
					}
					else if (node.opcode == RegionIR::Opcode::Vu0RequireIdle)
					{
						if (node.operand_count != 2 ||
							(vu0_idle_alias[node.id] == RegionIR::INVALID_VALUE &&
							 !demand(node.operands[0], 0x1)))
							return false;
					}
				}
			}

			auto propagate = [&](const RegionIR::Node& node, bool* changed) -> bool {
				const u8 result_mask = (*output)[node.id];
				auto operand = [&](u32 index, u8 mask) -> bool {
					return index < node.operand_count &&
						demand(node.operands[index], mask, changed);
				};
				auto operand_full = [&](u32 index) -> bool {
					if (index >= node.operand_count ||
						node.operands[index] >= definitions.size() ||
						!definitions[node.operands[index]])
					{
						return false;
					}
					return operand(index,
						FullWordMask(definitions[node.operands[index]]->type));
				};

				if (result_mask != 0 &&
					exact_i32_aliases[node.id] != RegionIR::INVALID_VALUE)
				{
					return demand(exact_i32_aliases[node.id], result_mask, changed);
				}
				if (result_mask != 0 &&
					forwarded_memory_load_alias[node.id] != RegionIR::INVALID_VALUE)
				{
					return demand(forwarded_memory_load_alias[node.id], result_mask,
						changed);
				}
				if (result_mask != 0 &&
					vu0_idle_alias[node.id] != RegionIR::INVALID_VALUE)
				{
					return demand(vu0_idle_alias[node.id], result_mask & 0x0f,
						changed);
				}
				if (result_mask != 0 &&
					vu0_normalize_alias[node.id] != RegionIR::INVALID_VALUE)
				{
					return demand(vu0_normalize_alias[node.id], result_mask & 0x0f,
						changed);
				}
				if (result_mask != 0 &&
					node.opcode == RegionIR::Opcode::Vu0NormalizeVector &&
					vu0_hoisted_normalize[node.id] != 0 &&
					(result_mask & 0x0f) == 0x0f)
				{
					// The generated entry prefix reads the invariant architectural
					// parameter directly into the normalized value's allocation. The
					// unchanged raw parameter remains canonical and owns no register
					// merely to be overwritten once before the loop begins.
					return true;
				}
				if (result_mask != 0 &&
					vu0_merge_alias[node.id] != RegionIR::INVALID_VALUE)
				{
					return demand(vu0_merge_alias[node.id], result_mask & 0x0f,
						changed);
				}
				if (result_mask != 0 &&
					node.opcode == RegionIR::Opcode::Vu0SyncStatusControl &&
					vu0_sync_direct_raw[node.id] != RegionIR::INVALID_VALUE)
				{
					return operand(0, 0x1) &&
						demand(vu0_sync_direct_raw[node.id], 0x0f, changed);
				}
				if (result_mask != 0 &&
					vu0_status_direct_raw[node.id] != RegionIR::INVALID_VALUE)
				{
					return demand(vu0_status_direct_raw[node.id], 0x0f, changed);
				}
				if (result_mask != 0 && node.opcode == RegionIR::Opcode::Vu0AddRaw &&
					vu0_add_fused_mul[node.id] != RegionIR::INVALID_VALUE)
				{
					const RegionIR::ValueId multiply_id = vu0_add_fused_mul[node.id];
					const RegionIR::Node* const multiply =
						multiply_id < definitions.size() ? definitions[multiply_id] : nullptr;
					if (!multiply || multiply->opcode != RegionIR::Opcode::Vu0MulRaw ||
						multiply->operand_count != 2 || node.operand_count != 2)
					{
						return false;
					}
					const RegionIR::ValueId addend = node.operands[0] == multiply_id ?
						node.operands[1] : node.operands[0];
					return demand(addend, result_mask, changed) &&
					       demand(multiply->operands[0], result_mask, changed) &&
					       demand(multiply->operands[1], result_mask, changed);
				}
				if (result_mask != 0 &&
					node.opcode == RegionIR::Opcode::Vu0BroadcastLane &&
					vu0_folded_broadcast_source[node.id] != RegionIR::INVALID_VALUE &&
					node.immediate < 4)
				{
					return demand(vu0_folded_broadcast_source[node.id],
						static_cast<u8>(1u << node.immediate), changed);
				}
				if (node.opcode == RegionIR::Opcode::Parameter || IsConstant(node.opcode) ||
					node.opcode == RegionIR::Opcode::NoEffect || IsBinding(node.opcode) ||
					node.opcode == RegionIR::Opcode::MemoryLoad ||
					node.opcode == RegionIR::Opcode::MemoryStore ||
					node.opcode == RegionIR::Opcode::ExitIfTrue || result_mask == 0)
				{
					return true;
				}

				switch (node.opcode)
				{
					case RegionIR::Opcode::CompareEqual64:
					case RegionIR::Opcode::CompareNotEqual64:
					{
						if (node.operand_count != 2 ||
							node.operands[0] >= low32_extensions.size() ||
							node.operands[1] >= low32_extensions.size())
							return false;
						const RematerializationKind left =
							low32_extensions[node.operands[0]];
						const RematerializationKind right =
							low32_extensions[node.operands[1]];
						const bool narrow =
							(left != RematerializationKind::None &&
							 IsKnownZeroValue(program, definitions,
								node.operands[1])) ||
							(right != RematerializationKind::None &&
							 IsKnownZeroValue(program, definitions,
								node.operands[0])) ||
							(left != RematerializationKind::None && left == right);
						const u8 mask = narrow ? 0x1 : 0x3;
						return operand(0, mask) && operand(1, mask);
					}
					case RegionIR::Opcode::CompareSignedLess64:
					case RegionIR::Opcode::CompareUnsignedLess64:
					{
						if (node.operand_count != 2 ||
							node.operands[0] >= low32_extensions.size() ||
							node.operands[1] >= low32_extensions.size())
						{
							return false;
						}
						const RematerializationKind left =
							low32_extensions[node.operands[0]];
						const RematerializationKind right =
							low32_extensions[node.operands[1]];
						const bool same_extension =
							left != RematerializationKind::None && left == right;
						// Signed ordering narrows when both operands share an exact
						// extension.  Unsigned ordering additionally has a compact
						// low-word lowering for sign-extended left versus zero-extended
						// right: a negative left is never below the zero extension;
						// otherwise an ordinary unsigned low comparison is exact.
						const bool mixed_unsigned =
							node.opcode == RegionIR::Opcode::CompareUnsignedLess64 &&
							left == RematerializationKind::SignExtendLow32 &&
							right == RematerializationKind::ZeroExtendLow32;
						const u8 mask = (same_extension || mixed_unsigned) ? 0x1 : 0x3;
						return operand(0, mask) && operand(1, mask);
					}
					case RegionIR::Opcode::Cop1NormalizeInput:
						if (cop1_normalize_alias[node.id] != RegionIR::INVALID_VALUE)
							return demand(cop1_normalize_alias[node.id],
								result_mask & 0x1, changed);
						return operand(0, result_mask & 0x1);
					case RegionIR::Opcode::Vu0NormalizeVector:
					case RegionIR::Opcode::Vu0ConvertFixed:
					case RegionIR::Opcode::Vu0ConvertIntegerToFloat:
						return operand(0, result_mask & 0x0f);
					case RegionIR::Opcode::Vu0Rotate32:
					{
						const u8 rotated = static_cast<u8>(
							((result_mask << 1) & 0x0fu) | ((result_mask >> 3) & 1u));
						return operand(0, rotated);
					}
					case RegionIR::Opcode::Vu0BroadcastScalar:
						return operand(0, result_mask ? 0x1 : 0);
					case RegionIR::Opcode::Vu0FdivQ:
					case RegionIR::Opcode::Vu0FdivFlags:
					{
						const u32 kind = node.immediate & 0x3u;
						const u32 fs_lane = (node.immediate >> 2) & 0x3u;
						const u32 ft_lane = (node.immediate >> 4) & 0x3u;
						if (kind > 2 || node.operand_count != 2)
							return false;
						const bool fs_required = kind != 1;
						return (!fs_required || operand(0,
							static_cast<u8>(1u << fs_lane))) &&
							operand(1, static_cast<u8>(1u << ft_lane));
					}
					case RegionIR::Opcode::ExtractLow32:
						return operand(0, result_mask & 0x1);
					case RegionIR::Opcode::ExtractLow64:
						return operand(0, result_mask & 0x3);
					case RegionIR::Opcode::ExtractHigh64:
						return operand(0, static_cast<u8>((result_mask & 0x3) << 2));
					case RegionIR::Opcode::ReplaceLow64:
						return operand(0, result_mask & 0x0c) &&
						       operand(1, result_mask & 0x03);
					case RegionIR::Opcode::ReplaceHigh64:
						return operand(0, result_mask & 0x03) &&
						       operand(1, static_cast<u8>((result_mask >> 2) & 0x03));
					case RegionIR::Opcode::BitcastI32ToF32Bits:
					case RegionIR::Opcode::BitcastF32BitsToI32:
					case RegionIR::Opcode::AddressFromI32:
						return operand(0, result_mask & 0x1);
					case RegionIR::Opcode::BitcastI128ToVuF32x4Bits:
					case RegionIR::Opcode::BitcastVuF32x4BitsToI128:
						return operand(0, result_mask & 0x0f);
					case RegionIR::Opcode::SignExtend32To64:
					case RegionIR::Opcode::ZeroExtend32To64:
						return operand(0, result_mask ? 0x1 : 0);
					case RegionIR::Opcode::Truncate64To32:
						return operand(0, result_mask & 0x1);
					case RegionIR::Opcode::Select64:
						return operand(0, 0x1) && operand(1, result_mask & 0x3) &&
						       operand(2, result_mask & 0x3);
					case RegionIR::Opcode::PackLow64:
						return operand(0, static_cast<u8>((result_mask >> 2) & 0x3)) &&
						       operand(1, result_mask & 0x3);
					case RegionIR::Opcode::PackHigh64:
						return operand(0, static_cast<u8>((result_mask & 0x3) << 2)) &&
						       operand(1, result_mask & 0x0c);
					case RegionIR::Opcode::And128:
					case RegionIR::Opcode::Or128:
					case RegionIR::Opcode::Xor128:
					case RegionIR::Opcode::Nor128:
						return operand(0, result_mask & 0x0f) &&
						       operand(1, result_mask & 0x0f);
					case RegionIR::Opcode::PackedBinary128:
						// A packed operation is one indivisible NEON qword operation.  Demand
						// complete inputs even when later dataflow observes only some lanes;
						// this both preserves lane semantics and keeps its value in the vector
						// register file instead of scalarizing it at an observer boundary.
						return operand(0, result_mask ? 0x0f : 0) &&
						       operand(1, result_mask ? 0x0f : 0);
					case RegionIR::Opcode::PackedShift128:
						return operand(0, result_mask ? 0x0f : 0);
					case RegionIR::Opcode::MemoryLoadValue:
					{
						const RegionIR::ValueId effect_id = node.operands[0];
						const RegionIR::Node* effect = effect_id < definitions.size() ?
							definitions[effect_id] : nullptr;
						if (effect && effect->opcode == RegionIR::Opcode::MemoryLoad &&
							effect->operand_count == 3 && node.type == RegionIR::ValueType::I128 &&
							(result_mask & 0x0c) != 0)
						{
							return demand(effect->operands[2], result_mask & 0x0c,
								changed);
						}
						return true;
					}
					case RegionIR::Opcode::Vu0RequireIdle:
						return operand(0, 0x1) && operand(1, result_mask);
					case RegionIR::Opcode::Vu0ControlWrite:
						if (node.immediate == 0 || node.immediate == 17 ||
							node.immediate == 26 || node.immediate == 29)
						{
							return operand(0, result_mask & 0x1);
						}
						if (node.immediate == 16)
						{
							return operand(0, result_mask & 0x1) &&
							       operand(1, result_mask & 0x1);
						}
						if (node.immediate < 16)
						{
							return operand(0, result_mask & 0x1) &&
							       operand(1, result_mask & 0x1);
						}
						return operand(1, result_mask & 0x1);
					case RegionIR::Opcode::Vu0DenormalizeStatus:
						return operand(0, result_mask & 0x1);
					default:
						for (u32 index = 0; index < node.operand_count; index++)
						{
							if (!operand_full(index))
								return false;
						}
						return true;
				}
			};

			bool changed = true;
			while (changed)
			{
				changed = false;
				for (auto block = program.blocks.rbegin(); block != program.blocks.rend();
					block++)
				{
					for (auto node = block->nodes.rbegin(); node != block->nodes.rend(); node++)
					{
						if (!propagate(*node, &changed))
							return false;
					}
				}
				for (const RegionIR::Block& block : program.blocks)
				{
					auto propagate_edge = [&](const RegionIR::Transfer& transfer) -> bool {
						if (transfer.target_block == RegionIR::INVALID_BLOCK)
							return true;
						if (transfer.target_block >= program.blocks.size())
							return false;
						const RegionIR::StateMap& parameters =
							program.blocks[transfer.target_block].parameters;
						for (size_t slot = 0; slot < RegionExecution::STATE_SLOT_COUNT; slot++)
						{
							const RegionIR::ValueId target =
								RegionExecution::StateValue(parameters, slot);
							const RegionIR::ValueId source =
								RegionExecution::StateValue(transfer.state, slot);
							if (target >= output->size() ||
								!demand(source, (*output)[target], &changed))
							{
								return false;
							}
						}
						return true;
					};
					if (!RegionIR::VisitInternalTransfers(block.terminator,
							[&](const RegionIR::Transfer& transfer, u8) {
								return propagate_edge(transfer);
							}))
					{
						return false;
					}
				}
			}
			return true;
		}

		u32 StorageBytes(RegionIR::ValueType type)
		{
			switch (type)
			{
				case RegionIR::ValueType::I1:
				case RegionIR::ValueType::I32:
				case RegionIR::ValueType::F32Bits:
				case RegionIR::ValueType::Address:
					return 4;
				case RegionIR::ValueType::I64:
				case RegionIR::ValueType::Cycle:
					return 8;
				case RegionIR::ValueType::I128:
				case RegionIR::ValueType::VuF32x4Bits:
					return 16;
				case RegionIR::ValueType::Void:
				case RegionIR::ValueType::MemoryEffect:
					return 0;
			}
			return 0;
		}

		RegionIR::ValueId EdgeStateValue(const RegionIR::StateMap& state,
			size_t slot)
		{
			return slot < RegionExecution::STATE_SLOT_COUNT ?
				RegionExecution::StateValue(state, slot) : state.memory_effect;
		}

		RegionIR::ValueId BlockStateParameter(const RegionIR::Block& block,
			size_t slot)
		{
			return slot < RegionExecution::STATE_SLOT_COUNT ?
				RegionExecution::StateValue(block.parameters, slot) :
				block.parameters.memory_effect;
		}

		u32 AlignUp(u32 value, u32 alignment)
		{
			return (value + alignment - 1) & ~(alignment - 1);
		}

		bool LocationsOverlap(const Location& left, const Location& right)
		{
			if (left.kind != right.kind)
				return false;
			if (left.kind == LocationKind::Core ||
				left.kind == LocationKind::FixedCycle)
			{
				return left.index < right.index + right.words &&
				       right.index < left.index + left.words;
			}
			if (left.kind == LocationKind::NeonQ)
			{
				const u8 left_mask = left.word_mask != 0 ? left.word_mask : 0x0f;
				const u8 right_mask = right.word_mask != 0 ? right.word_mask : 0x0f;
				return left.index == right.index && (left_mask & right_mask) != 0;
			}
			if (left.kind == LocationKind::VfpS)
				return left.index == right.index;
			if (left.kind == LocationKind::Spill)
			{
				const u32 left_bytes = left.words * sizeof(u32);
				const u32 right_bytes = right.words * sizeof(u32);
				return left.index < right.index + right_bytes &&
				       right.index < left.index + left_bytes;
			}
			return false;
		}

		bool SamePhysicalStorage(const Location& left, const Location& right)
		{
			if (left.kind != right.kind || left.index != right.index ||
				left.words != right.words)
			{
				return false;
			}
			if (left.kind == LocationKind::NeonQ)
			{
				const u8 left_mask = left.word_mask != 0 ? left.word_mask : 0x0f;
				const u8 right_mask = right.word_mask != 0 ? right.word_mask : 0x0f;
				return left_mask == right_mask;
			}
			return left.kind == LocationKind::Core ||
			       left.kind == LocationKind::FixedCycle ||
			       left.kind == LocationKind::VfpS ||
			       left.kind == LocationKind::Spill;
		}

		u8 NormalizedWordMask(const Location& location)
		{
			return location.word_mask != 0 ? location.word_mask :
				(location.words == 0 ? 0 :
				 static_cast<u8>((1u << std::min<u8>(location.words, 4)) - 1u));
		}

		u8 CountWords(u8 mask)
		{
			u8 words = 0;
			for (u8 word = 0; word < 4; word++)
				words += (mask >> word) & 1u;
			return words;
		}

		bool IsLowPrefixMask(u8 mask)
		{
			return mask != 0 && (mask & static_cast<u8>(mask + 1u)) == 0;
		}

		u8 RankWord(u8 mask, u8 word)
		{
			return CountWords(mask & static_cast<u8>((1u << word) - 1u));
		}

		Location ComponentLocation(const Location& location, u8 word)
		{
			const u8 mask = NormalizedWordMask(location);
			const u8 component = static_cast<u8>(1u << word);
			if ((mask & component) == 0)
				return {};

			Location fragment = location;
			fragment.words = 1;
			fragment.word_mask = component;
			switch (location.kind)
			{
				case LocationKind::Core:
				case LocationKind::FixedCycle:
					fragment.index = static_cast<u16>(
						location.index + RankWord(mask, word));
					break;
				case LocationKind::Spill:
				{
					// Four-word spill locations retain the natural qword lane
					// layout. Scalar fragments are packed by ascending demanded
					// component, exactly like consecutive core words.
					const u8 physical_word = location.words == 4 ?
						word : RankWord(mask, word);
					fragment.index = static_cast<u16>(location.index +
						physical_word * sizeof(u32));
					break;
				}
			case LocationKind::Immediate:
			case LocationKind::CanonicalState:
			case LocationKind::VfpS:
				case LocationKind::NeonQ:
					break;
				case LocationKind::None:
					return {};
			}
			return fragment;
		}

		bool IsMoveSource(const Location& location)
		{
			return location.kind == LocationKind::Immediate ||
			       location.kind == LocationKind::CanonicalState ||
			       location.kind == LocationKind::Core ||
			       location.kind == LocationKind::FixedCycle ||
			       location.kind == LocationKind::VfpS ||
			       location.kind == LocationKind::NeonQ ||
			       location.kind == LocationKind::Spill;
		}

		bool IsMoveTarget(const Location& location)
		{
			return location.kind == LocationKind::Core ||
			       location.kind == LocationKind::FixedCycle ||
			       location.kind == LocationKind::VfpS ||
			       location.kind == LocationKind::NeonQ ||
			       location.kind == LocationKind::Spill;
		}
	} // namespace

	EdgeCopyScheduleResult BuildEdgeCopySchedule(
		const std::vector<EdgeMove>& moves, u32 spill_bytes,
		u32 max_spill_bytes)
	{
		EdgeCopyScheduleResult result{};
		result.spill_bytes = spill_bytes;
		if (spill_bytes > max_spill_bytes || spill_bytes > UINT16_MAX)
		{
			result.failure = BuildFailure::SpillCapacity;
			return result;
		}

		struct PendingMove
		{
			EdgeMove move{};
			Location source{};
		};
		constexpr u16 SCRATCH_PLACEHOLDER = UINT16_MAX;
		u32 scratch_bytes = 0;
		std::vector<bool> grouped(moves.size(), false);
		for (size_t first = 0; first < moves.size(); first++)
		{
			if (grouped[first])
				continue;
			const EdgeMove& identity = moves[first];
			std::vector<EdgeMove> logical_group;
			std::vector<PendingMove> pending;
			for (size_t index = first; index < moves.size(); index++)
			{
				const EdgeMove& move = moves[index];
				if (move.source_block != identity.source_block ||
					move.target_block != identity.target_block ||
					move.edge_index != identity.edge_index)
				{
					continue;
				}
				grouped[index] = true;
				const u8 target_mask = NormalizedWordMask(move.target_location);
				const u8 source_mask = NormalizedWordMask(move.source_location);
				const u8 copy_mask = move.word_mask != 0 ? move.word_mask : target_mask;
				if (!IsMoveSource(move.source_location) ||
					!IsMoveTarget(move.target_location) ||
					move.target_location.words == 0 ||
					(target_mask & copy_mask) != copy_mask ||
					(source_mask & copy_mask) != copy_mask ||
					CountWords(copy_mask) > move.target_location.words)
				{
					result.failure = BuildFailure::EdgeCopyConflict;
					result.block = move.source_block;
					result.value = move.source;
					return result;
				}
				logical_group.push_back(move);
			}

			// Q-register copies are kept atomic only when every use of the physical
			// q register on this edge is a complete qword-to-qword transfer. A
			// representation conversion (for example qword to two core words) marks
			// that q register for component lowering, including any fan-out copies.
			// This guarantees the destructive-copy scheduler never mixes an atomic q
			// source with a partially overlapping lane source.
			std::vector<u16> fragmented_q;
			auto mark_fragmented_q = [&](const Location& location) {
				if (location.kind == LocationKind::NeonQ &&
					std::find(fragmented_q.begin(), fragmented_q.end(), location.index) ==
						fragmented_q.end())
				{
					fragmented_q.push_back(location.index);
				}
			};
			for (const EdgeMove& move : logical_group)
			{
				const u8 source_mask = NormalizedWordMask(move.source_location);
				const u8 target_mask = NormalizedWordMask(move.target_location);
				const u8 copy_mask = move.word_mask != 0 ? move.word_mask : target_mask;
				const bool complete_q_copy =
					move.source_location.kind == LocationKind::NeonQ &&
					move.target_location.kind == LocationKind::NeonQ &&
					move.source_location.words == 4 && move.target_location.words == 4 &&
					source_mask == 0x0f && target_mask == 0x0f && copy_mask == 0x0f;
				if (!complete_q_copy)
				{
					mark_fragmented_q(move.source_location);
					mark_fragmented_q(move.target_location);
				}
			}

			std::vector<EdgeMove> group;
			for (const EdgeMove& move : logical_group)
			{
				const u8 source_mask = NormalizedWordMask(move.source_location);
				const u8 target_mask = NormalizedWordMask(move.target_location);
				const u8 copy_mask = move.word_mask != 0 ? move.word_mask : target_mask;
				const bool atomic_q =
					move.source_location.kind == LocationKind::NeonQ &&
					move.target_location.kind == LocationKind::NeonQ &&
					move.source_location.words == 4 && move.target_location.words == 4 &&
					source_mask == 0x0f && target_mask == 0x0f && copy_mask == 0x0f &&
					std::find(fragmented_q.begin(), fragmented_q.end(),
						move.source_location.index) == fragmented_q.end() &&
					std::find(fragmented_q.begin(), fragmented_q.end(),
						move.target_location.index) == fragmented_q.end();
				if (atomic_q)
				{
					EdgeMove normalized = move;
					normalized.source_location.word_mask = source_mask;
					normalized.target_location.word_mask = target_mask;
					normalized.word_mask = copy_mask;
					group.push_back(normalized);
					continue;
				}

				for (u8 word = 0; word < 4; word++)
				{
					if ((copy_mask & (1u << word)) == 0)
						continue;
					EdgeMove fragment = move;
					fragment.source_location =
						ComponentLocation(move.source_location, word);
					fragment.target_location = ComponentLocation(move.target_location, word);
					fragment.word_mask = static_cast<u8>(1u << word);
					if (fragment.source_location.kind == LocationKind::None ||
						fragment.target_location.kind == LocationKind::None)
					{
						result.failure = BuildFailure::EdgeCopyConflict;
						result.block = move.source_block;
						result.value = move.source;
						return result;
					}
					group.push_back(fragment);
				}
			}

			// Sources may overlap exactly or partially (fan-out); the scheduler
			// preserves them until their last use. Block parameters are independent
			// destinations and therefore may not collide physically.
			for (size_t left = 0; left < group.size(); left++)
			{
				for (size_t right = left + 1; right < group.size(); right++)
				{
					const Location& left_target = group[left].target_location;
					const Location& right_target = group[right].target_location;
					const bool target_collision = LocationsOverlap(left_target,
						right_target);
					if (target_collision)
					{
						result.failure = BuildFailure::EdgeCopyConflict;
						result.block = identity.source_block;
						result.value = group[right].target_parameter;
						return result;
					}
				}
			}
			for (const EdgeMove& move : group)
			{
				if (!SamePhysicalStorage(move.source_location, move.target_location))
					pending.push_back({move, move.source_location});
			}

			while (!pending.empty())
			{
				size_t ready = pending.size();
				for (size_t candidate = 0; candidate < pending.size(); candidate++)
				{
					const Location& target =
						pending[candidate].move.target_location;
					bool target_is_source = false;
					for (const PendingMove& other : pending)
					{
						if (LocationsOverlap(target, other.source))
						{
							target_is_source = true;
							break;
						}
					}
					if (!target_is_source)
					{
						ready = candidate;
						break;
					}
				}

				if (ready != pending.size())
				{
					const PendingMove item = pending[ready];
					result.steps.push_back({EdgeCopyStepKind::Copy,
						identity.source_block, identity.target_block,
						identity.edge_index, item.move.source,
						item.move.target_parameter, item.source,
						item.move.target_location,
						item.move.word_mask != 0 ? item.move.word_mask :
							NormalizedWordMask(item.move.target_location)});
					pending.erase(pending.begin() + ready);
					continue;
				}

				// A remaining dependency cycle has no destructive first move. Save
				// one complete source, rewrite every fan-out use to the shared
				// scratch slot, and resume the same ready-move algorithm.
				const PendingMove breaker = pending.front();
				if (breaker.source.kind == LocationKind::Immediate ||
					breaker.source.kind == LocationKind::None)
				{
					result.failure = BuildFailure::EdgeCopyConflict;
					result.block = identity.source_block;
					result.value = breaker.move.source;
					return result;
				}
				const u32 bytes = breaker.source.words * sizeof(u32);
				if (bytes == 0 || bytes > 16)
				{
					result.failure = BuildFailure::EdgeCopyConflict;
					result.block = identity.source_block;
					result.value = breaker.move.source;
					return result;
				}
				scratch_bytes = std::max(scratch_bytes, bytes);
				const Location placeholder{LocationKind::Spill,
					SCRATCH_PLACEHOLDER, breaker.source.words,
					NormalizedWordMask(breaker.source)};
				result.steps.push_back({EdgeCopyStepKind::SaveScratch,
					identity.source_block, identity.target_block,
					identity.edge_index, breaker.move.source,
					RegionIR::INVALID_VALUE, breaker.source, placeholder,
					NormalizedWordMask(breaker.source)});
				for (PendingMove& item : pending)
				{
					if (SamePhysicalStorage(item.source, breaker.source))
					{
						item.source = placeholder;
						item.source.word_mask =
							NormalizedWordMask(item.move.source_location);
					}
				}
			}
		}

		if (scratch_bytes != 0)
		{
			const u32 scratch_offset = AlignUp(spill_bytes,
				std::min(scratch_bytes, 16u));
			if (scratch_offset > max_spill_bytes ||
				scratch_bytes > max_spill_bytes - scratch_offset ||
				scratch_offset > UINT16_MAX)
			{
				result.failure = BuildFailure::SpillCapacity;
				return result;
			}
			result.scratch_offset = scratch_offset;
			result.scratch_bytes = scratch_bytes;
			result.spill_bytes = scratch_offset + scratch_bytes;
			for (EdgeCopyStep& step : result.steps)
			{
				if (step.source_location.kind == LocationKind::Spill &&
					step.source_location.index == SCRATCH_PLACEHOLDER)
				{
					step.source_location.index = static_cast<u16>(scratch_offset);
				}
				if (step.target_location.kind == LocationKind::Spill &&
					step.target_location.index == SCRATCH_PLACEHOLDER)
				{
					step.target_location.index = static_cast<u16>(scratch_offset);
				}
			}
		}
		return result;
	}

	BuildResult Build(const RegionIR::Program& program,
		const RegionExecution::Plan& execution, const Options& options)
	{
		const size_t resident_core_values = options.region_resident_core_values ?
			options.region_resident_core_values->size() : 0;
		if (execution.value_exit_uses.size() != program.value_count ||
			execution.value_exit_word_uses.size() != program.value_count ||
			execution.value_canonical_parameter_words.size() != program.value_count ||
			execution.value_low32_extensions.size() != program.value_count ||
			execution.value_low32_entry_guards.size() != program.value_count ||
			options.core_register_words > 16 || options.vfp_s_registers > 14 ||
			options.neon_q_registers > 8 ||
			options.scalar_addressable_neon_q_registers >
				options.neon_q_registers ||
			resident_core_values > options.core_register_words ||
			(resident_core_values != 0 &&
			 resident_core_values == options.core_register_words))
		{
			return Fail(BuildFailure::InvalidExecutionPlan,
				program.entry_block, RegionIR::INVALID_VALUE);
		}

		BuildResult result{};
		const u32 allocatable_core_register_words =
			options.core_register_words - static_cast<u32>(resident_core_values);
		result.plan.cop1_normalize_alias.assign(program.value_count,
			RegionIR::INVALID_VALUE);
		result.plan.cop1_hoisted_normalize.assign(program.value_count, 0);
		result.plan.cop1_region_resident_normalize.assign(program.value_count, 0);
		result.plan.cop1_exception_for_guarded_raw.assign(program.value_count,
			RegionIR::INVALID_VALUE);
		result.plan.cop1_guarded_raw_for_exception.assign(program.value_count,
			RegionIR::INVALID_VALUE);
		result.plan.cop1_guarded_clamp_alias.assign(program.value_count,
			RegionIR::INVALID_VALUE);
		result.plan.cop1_fused_flag_for_clamp.assign(program.value_count,
			RegionIR::INVALID_VALUE);
		result.plan.cop1_fused_clamp_for_flag.assign(program.value_count,
			RegionIR::INVALID_VALUE);
		result.plan.vu0_idle_alias.assign(program.value_count,
			RegionIR::INVALID_VALUE);
		result.plan.vu0_normalize_alias.assign(program.value_count,
			RegionIR::INVALID_VALUE);
		result.plan.vu0_hoisted_normalize.assign(program.value_count, 0);
		result.plan.vu0_merge_alias.assign(program.value_count,
			RegionIR::INVALID_VALUE);
		result.plan.vu0_status_direct_raw.assign(program.value_count,
			RegionIR::INVALID_VALUE);
		result.plan.vu0_add_fused_mul.assign(program.value_count,
			RegionIR::INVALID_VALUE);
		result.plan.vu0_folded_broadcast_source.assign(program.value_count,
			RegionIR::INVALID_VALUE);
		result.plan.vu0_sync_folded_predecessor.assign(program.value_count,
			RegionIR::INVALID_VALUE);
		result.plan.vu0_sync_folded_into.assign(program.value_count,
			RegionIR::INVALID_VALUE);
		result.plan.vu0_sync_direct_raw.assign(program.value_count,
			RegionIR::INVALID_VALUE);
		result.plan.vu0_status_fused_sync.assign(program.value_count,
			RegionIR::INVALID_VALUE);
		result.plan.vu0_clamp_deferred_to_status.assign(program.value_count,
			RegionIR::INVALID_VALUE);
		result.plan.vu0_status_deferred_clamp.assign(program.value_count,
			RegionIR::INVALID_VALUE);
		result.plan.vu0_clamp_deferred_to_mac.assign(program.value_count,
			RegionIR::INVALID_VALUE);
		result.plan.vu0_mac_deferred_clamp.assign(program.value_count,
			RegionIR::INVALID_VALUE);
		result.plan.forwarded_memory_load_alias.assign(program.value_count,
			RegionIR::INVALID_VALUE);
		result.plan.forwarded_memory_load_effect.assign(program.value_count, 0);
		result.plan.signed_overflow_for_flagged_add.assign(program.value_count,
			RegionIR::INVALID_VALUE);
		result.plan.flagged_add_for_signed_overflow.assign(program.value_count,
			RegionIR::INVALID_VALUE);
		std::vector<const RegionIR::Node*> definitions(program.value_count, nullptr);
		std::vector<Range> ranges(program.value_count);
		std::vector<u32> block_entry(program.blocks.size());
		std::vector<u32> block_taken_edge(program.blocks.size());
		std::vector<u32> block_not_taken_edge(program.blocks.size());
		u32 position = 1;
		for (u32 block_index = 0; block_index < program.blocks.size(); block_index++)
		{
			const RegionIR::Block& block = program.blocks[block_index];
			const bool likely = block.terminator.kind ==
				RegionIR::TerminatorKind::Branch && block.terminator.likely;
			std::vector<std::pair<RegionIR::ValueId, RegionIR::ValueId>>
				normalized_values;
			std::vector<std::pair<RegionIR::ValueId, RegionIR::ValueId>>
				normalized_vu0_values;
			std::vector<const RegionIR::Node*> taken_delay_nodes;
			const u32 entry_position = position++;
			block_entry[block_index] = entry_position;
			for (const RegionIR::Node& node : block.nodes)
			{
				if (node.id >= program.value_count || definitions[node.id])
					return Fail(BuildFailure::InvalidValue, block_index, node.id);
				definitions[node.id] = &node;
				if (node.opcode == RegionIR::Opcode::Vu0MergeMasked &&
					node.operand_count == 2 && node.immediate == 0x0f)
				{
					result.plan.vu0_merge_alias[node.id] = node.operands[1];
				}
				if (node.opcode == RegionIR::Opcode::Cop1NormalizeInput &&
					node.operand_count == 1)
				{
					const auto prior = std::find_if(normalized_values.begin(),
						normalized_values.end(), [&](const auto& candidate) {
							return candidate.first == node.operands[0];
						});
					if (prior == normalized_values.end())
						normalized_values.emplace_back(node.operands[0], node.id);
					else
						result.plan.cop1_normalize_alias[node.id] = prior->second;
				}
				if (options.vu0_idle_entry_proven &&
					node.opcode == RegionIR::Opcode::Vu0RequireIdle &&
					node.operand_count == 2)
				{
					result.plan.vu0_idle_alias[node.id] = node.operands[1];
				}
				if (node.opcode == RegionIR::Opcode::Vu0NormalizeVector &&
					node.operand_count == 1)
				{
					RegionIR::ValueId source = node.operands[0];
					if (source < result.plan.vu0_idle_alias.size() &&
						result.plan.vu0_idle_alias[source] != RegionIR::INVALID_VALUE)
					{
						source = result.plan.vu0_idle_alias[source];
					}
					if (source < result.plan.vu0_merge_alias.size() &&
						result.plan.vu0_merge_alias[source] != RegionIR::INVALID_VALUE)
					{
						source = result.plan.vu0_merge_alias[source];
					}
					const RegionIR::Node* const source_definition =
						source < definitions.size() ? definitions[source] : nullptr;
					if (source_definition &&
						source_definition->opcode ==
							RegionIR::Opcode::Vu0ClampFmacResult &&
						source_definition->immediate == 0x0f)
					{
						result.plan.vu0_normalize_alias[node.id] = source;
					}
					const auto prior = std::find_if(normalized_vu0_values.begin(),
						normalized_vu0_values.end(), [&](const auto& candidate) {
							return candidate.first == source;
						});
					if (result.plan.vu0_normalize_alias[node.id] !=
						RegionIR::INVALID_VALUE)
					{
						// The source clamp is already the exact normalized value.
					}
					else if (prior == normalized_vu0_values.end())
						normalized_vu0_values.emplace_back(source, node.id);
					else
						result.plan.vu0_normalize_alias[node.id] = prior->second;
				}
				const bool taken_delay = likely &&
					node.opcode != RegionIR::Opcode::Parameter &&
					node.source_pc == block.terminator.delay_slot_pc;
				const u32 definition_position =
					node.opcode == RegionIR::Opcode::Parameter ? entry_position :
						taken_delay ? 0 : position++;
				ranges[node.id] = {block_index, definition_position,
					definition_position, 0};
				if (taken_delay)
					taken_delay_nodes.push_back(&node);
			}
			// A likely branch annuls its delay slot on the not-taken edge. Model the
			// two edge positions separately so a taken-only result can destructively
			// reuse an input which remains architectural on the not-taken exit. This
			// is the allocation counterpart of edge-local delay-slot emission below;
			// it does not infer mutual exclusion from opcode sequences.
			if (likely)
			{
				block_not_taken_edge[block_index] = position++;
				for (const RegionIR::Node* node : taken_delay_nodes)
				{
					ranges[node->id].begin = position;
					ranges[node->id].end = position++;
				}
				block_taken_edge[block_index] = position++;
			}
			else
			{
				block_taken_edge[block_index] = position++;
				block_not_taken_edge[block_index] = block_taken_edge[block_index];
			}
		}

		// Propagate the exact PS2-normalized-bit fact through internal state
		// transfers. Cop1ClampOuResult and Cop1ConvertSingle are architectural
		// anchors; Cop1NormalizeInput is the explicit fpuDouble() anchor. A
		// non-entry block parameter acquires the fact only when every internal
		// predecessor supplies an anchored normalized value. Starting from false and
		// iterating monotonically means an unanchored cycle cannot prove itself. The
		// published region entry is deliberately excluded because canonical FPR/ACC
		// words there may still contain denormals or exponent-255 values.
		std::vector<u8> cop1_normalized(program.value_count, 0);
		for (const RegionIR::Block& block : program.blocks)
		{
			for (const RegionIR::Node& node : block.nodes)
			{
				if (node.opcode == RegionIR::Opcode::Cop1NormalizeInput ||
					node.opcode == RegionIR::Opcode::Cop1ClampOuResult ||
					node.opcode == RegionIR::Opcode::Cop1ConvertSingle)
				{
					cop1_normalized[node.id] = 1;
				}
			}
		}
		std::vector<std::vector<const RegionIR::Transfer*>> cop1_incoming(
			program.blocks.size());
		for (const RegionIR::Block& block : program.blocks)
		{
			auto append = [&](const RegionIR::Transfer& transfer) {
				if (transfer.target_block < cop1_incoming.size())
					cop1_incoming[transfer.target_block].push_back(&transfer);
			};
			(void)RegionIR::VisitInternalTransfers(block.terminator,
				[&](const RegionIR::Transfer& transfer, u8) {
					append(transfer);
					return true;
				});
		}
		bool normalized_changed = true;
		while (normalized_changed)
		{
			normalized_changed = false;
			for (u32 block_index = 0; block_index < program.blocks.size(); block_index++)
			{
				if (block_index == program.entry_block ||
					cop1_incoming[block_index].empty())
				{
					continue;
				}
				const RegionIR::Block& block = program.blocks[block_index];
				for (size_t slot = 0; slot < RegionExecution::STATE_SLOT_COUNT; slot++)
				{
					const RegionIR::ValueId parameter =
						RegionExecution::StateValue(block.parameters, slot);
					if (parameter >= definitions.size() || !definitions[parameter] ||
						definitions[parameter]->type != RegionIR::ValueType::F32Bits ||
						cop1_normalized[parameter])
					{
						continue;
					}
					bool complete = true;
					for (const RegionIR::Transfer* predecessor :
						cop1_incoming[block_index])
					{
						const RegionIR::ValueId source =
							RegionExecution::StateValue(predecessor->state, slot);
						if (source >= cop1_normalized.size() ||
							!cop1_normalized[source])
						{
							complete = false;
							break;
						}
					}
					if (complete)
					{
						cop1_normalized[parameter] = 1;
						normalized_changed = true;
					}
				}
			}
		}
		for (const RegionIR::Block& block : program.blocks)
		{
			for (const RegionIR::Node& node : block.nodes)
			{
				if (node.opcode == RegionIR::Opcode::Cop1NormalizeInput &&
					node.operand_count == 1 &&
					result.plan.cop1_normalize_alias[node.id] ==
						RegionIR::INVALID_VALUE &&
					node.operands[0] < cop1_normalized.size() &&
					cop1_normalized[node.operands[0]])
				{
					result.plan.cop1_normalize_alias[node.id] = node.operands[0];
				}
			}
		}

		// Carry the same exact fact through the VU0 vector state graph.  The former
		// block-local CSE forgot the fact at every direct-call, return and CFG seam,
		// so a compiler-owned multi-block unit re-normalized an already clamped VF or
		// ACC on each side of the edge.  Only explicit full-vector normalization and
		// full-mask architectural clamps anchor the fact.  A non-entry parameter is
		// proven only when every internal predecessor supplies such a value; as with
		// COP1 above, an unanchored cycle cannot prove itself.
		std::vector<u8> vu0_normalized(program.value_count, 0);
		for (const RegionIR::Block& block : program.blocks)
		{
			for (const RegionIR::Node& node : block.nodes)
			{
				if (node.opcode == RegionIR::Opcode::Vu0NormalizeVector ||
					(node.opcode == RegionIR::Opcode::Vu0ClampFmacResult &&
					 node.immediate == 0x0f))
				{
					vu0_normalized[node.id] = 1;
				}
			}
		}
		bool vu0_normalized_changed = true;
		while (vu0_normalized_changed)
		{
			vu0_normalized_changed = false;
			for (const RegionIR::Block& block : program.blocks)
			{
				for (const RegionIR::Node& node : block.nodes)
				{
					if (vu0_normalized[node.id])
						continue;
					RegionIR::ValueId alias = RegionIR::INVALID_VALUE;
					if (node.id < result.plan.vu0_idle_alias.size() &&
						result.plan.vu0_idle_alias[node.id] !=
							RegionIR::INVALID_VALUE)
					{
						alias = result.plan.vu0_idle_alias[node.id];
					}
					else if (node.id < result.plan.vu0_merge_alias.size() &&
						result.plan.vu0_merge_alias[node.id] !=
							RegionIR::INVALID_VALUE)
					{
						alias = result.plan.vu0_merge_alias[node.id];
					}
					if (alias < vu0_normalized.size() && vu0_normalized[alias])
					{
						vu0_normalized[node.id] = 1;
						vu0_normalized_changed = true;
					}
				}

				const u32 block_index = static_cast<u32>(&block - program.blocks.data());
				if (block_index == program.entry_block ||
					cop1_incoming[block_index].empty())
				{
					continue;
				}
				for (size_t slot = 0; slot < RegionExecution::STATE_SLOT_COUNT; slot++)
				{
					const RegionIR::ValueId parameter =
						RegionExecution::StateValue(block.parameters, slot);
					if (parameter >= definitions.size() || !definitions[parameter] ||
						definitions[parameter]->type !=
							RegionIR::ValueType::VuF32x4Bits ||
						vu0_normalized[parameter])
					{
						continue;
					}
					bool complete = true;
					for (const RegionIR::Transfer* predecessor :
						cop1_incoming[block_index])
					{
						const RegionIR::ValueId source =
							RegionExecution::StateValue(predecessor->state, slot);
						if (source >= vu0_normalized.size() ||
							!vu0_normalized[source])
						{
							complete = false;
							break;
						}
					}
					if (complete)
					{
						vu0_normalized[parameter] = 1;
						vu0_normalized_changed = true;
					}
				}
			}
		}
		for (const RegionIR::Block& block : program.blocks)
		{
			for (const RegionIR::Node& node : block.nodes)
			{
				if (node.opcode != RegionIR::Opcode::Vu0NormalizeVector ||
					node.operand_count != 1 ||
					result.plan.vu0_normalize_alias[node.id] !=
						RegionIR::INVALID_VALUE)
				{
					continue;
				}
				RegionIR::ValueId source = node.operands[0];
				if (source < result.plan.vu0_idle_alias.size() &&
					result.plan.vu0_idle_alias[source] != RegionIR::INVALID_VALUE)
				{
					source = result.plan.vu0_idle_alias[source];
				}
				if (source < result.plan.vu0_merge_alias.size() &&
					result.plan.vu0_merge_alias[source] != RegionIR::INVALID_VALUE)
				{
					source = result.plan.vu0_merge_alias[source];
				}
				if (source < vu0_normalized.size() && vu0_normalized[source])
					result.plan.vu0_normalize_alias[node.id] = source;
			}
		}

		// Keep one normalized derivative of an unchanged canonical FPR resident
		// across the region. The execution plan has already proven individual
		// parameter words to retain their architectural entry origin through every
		// incoming edge. Restrict the source to a Parameter and require the reusable
		// normalizer itself to exist in the sole externally reachable entry block;
		// this makes prefix placement and dominance mechanical. Changed FPRs are not
		// canonical and continue to use the normalized-result propagation above.
		if (program.entry_block < program.blocks.size())
		{
			std::array<RegionIR::ValueId, RegionExecution::STATE_SLOT_COUNT>
				entry_normalizer{};
			entry_normalizer.fill(RegionIR::INVALID_VALUE);
			const RegionIR::Block& entry = program.blocks[program.entry_block];
			for (const RegionIR::Node& node : entry.nodes)
			{
				if (node.opcode != RegionIR::Opcode::Cop1NormalizeInput ||
					node.operand_count != 1 ||
					node.operands[0] >= definitions.size())
				{
					continue;
				}
				const RegionIR::Node* const parameter =
					definitions[node.operands[0]];
				if (!parameter || parameter->opcode != RegionIR::Opcode::Parameter ||
					parameter->type != RegionIR::ValueType::F32Bits ||
					parameter->immediate >= entry_normalizer.size() ||
					(execution.value_canonical_parameter_words[parameter->id] & 0x1) == 0)
				{
					continue;
				}
				if (entry_normalizer[parameter->immediate] == RegionIR::INVALID_VALUE)
					entry_normalizer[parameter->immediate] = node.id;
			}
			const u32 resident_limit = options.vfp_s_registers > 2 ?
				options.vfp_s_registers - 2 : 0;
			u32 resident_count = 0;
			for (u32 block_index = 0; block_index < program.blocks.size(); block_index++)
			{
				if (block_index == program.entry_block)
					continue;
				for (const RegionIR::Node& node : program.blocks[block_index].nodes)
				{
					if (node.opcode != RegionIR::Opcode::Cop1NormalizeInput ||
						node.operand_count != 1 ||
						node.operands[0] >= definitions.size() ||
						result.plan.cop1_normalize_alias[node.id] !=
							RegionIR::INVALID_VALUE)
					{
						continue;
					}
					const RegionIR::Node* const parameter =
						definitions[node.operands[0]];
					if (!parameter || parameter->opcode != RegionIR::Opcode::Parameter ||
						parameter->type != RegionIR::ValueType::F32Bits ||
						parameter->immediate >= entry_normalizer.size() ||
						(execution.value_canonical_parameter_words[parameter->id] & 0x1) == 0)
					{
						continue;
					}
					const RegionIR::ValueId normalized =
						entry_normalizer[parameter->immediate];
					if (normalized == RegionIR::INVALID_VALUE)
						continue;
					if (result.plan.cop1_region_resident_normalize[normalized] == 0)
					{
						if (resident_count >= resident_limit)
							continue;
						result.plan.cop1_region_resident_normalize[normalized] = 1;
						resident_count++;
					}
					result.plan.cop1_normalize_alias[node.id] = normalized;
					result.plan.cop1_hoisted_normalize[normalized] = 1;
				}
			}
		}
		// ADDI's wrapping sum, overflow predicate, and guarded exit are adjacent by
		// verifier contract.  Preserve that structural proof here so A32 can use the
		// native V flag instead of materializing the textbook XOR/AND boolean.  Zero
		// is excluded because an elided same-register ADD would not define NZCV.
		for (const RegionIR::Block& block : program.blocks)
		{
			for (size_t index = 0; index + 2 < block.nodes.size(); index++)
			{
				const RegionIR::Node& add = block.nodes[index];
				const RegionIR::Node& overflow = block.nodes[index + 1];
				size_t guard_index = index + 2;
				while (guard_index < block.nodes.size() &&
					block.nodes[guard_index].opcode ==
						RegionIR::Opcode::ConstantAddress &&
					block.nodes[guard_index].source_pc == add.source_pc)
				{
					guard_index++;
				}
				if (guard_index >= block.nodes.size())
					continue;
				const RegionIR::Node& guard = block.nodes[guard_index];
				if (add.opcode != RegionIR::Opcode::Add32 || add.operand_count != 2 ||
					overflow.opcode != RegionIR::Opcode::SignedAddOverflow32 ||
					overflow.operand_count != 2 || guard.opcode !=
						RegionIR::Opcode::ExitIfTrue || guard.operand_count != 1 ||
					guard.operands[0] != overflow.id ||
					overflow.operands[0] != add.operands[0] ||
					overflow.operands[1] != add.operands[1] ||
					add.source_pc != overflow.source_pc ||
					add.source_pc != guard.source_pc ||
					guard.immediate >= block.guarded_exits.size())
				{
					continue;
				}
				const RegionIR::Node* const right =
					add.operands[1] < definitions.size() ?
						definitions[add.operands[1]] : nullptr;
				if (right && (right->opcode == RegionIR::Opcode::ConstantI32 ||
						right->opcode == RegionIR::Opcode::ConstantI1) &&
					right->literal == 0)
				{
					continue;
				}
				result.plan.signed_overflow_for_flagged_add[add.id] = overflow.id;
				result.plan.flagged_add_for_signed_overflow[overflow.id] = add.id;
			}
		}
		// The verifier has already proved these predicates are unique and own the
		// exact decoded COP1 raw result.  A lazy O/U correction is an internal target
		// lowering: both the ordinary and exceptional paths rejoin the exact IR clamp
		// and flag values.  Pair the classifier with that group before word-demand and
		// liveness construction; it is deliberately not an architectural exit.
		for (const RegionIR::Block& block : program.blocks)
		{
			for (const RegionIR::Node& exceptional : block.nodes)
			{
				if (exceptional.opcode !=
						RegionIR::Opcode::Cop1ExceptionalOuResult ||
					exceptional.operand_count != 1)
				{
					continue;
				}
				const RegionIR::ValueId raw = exceptional.operands[0];
				const auto clamp = std::find_if(block.nodes.begin(), block.nodes.end(),
					[&](const RegionIR::Node& candidate) {
						return candidate.opcode ==
								RegionIR::Opcode::Cop1ClampOuResult &&
						       candidate.operand_count == 1 &&
						       candidate.operands[0] == raw &&
						       candidate.source_pc == exceptional.source_pc;
					});
				const auto flags = std::find_if(block.nodes.begin(), block.nodes.end(),
					[&](const RegionIR::Node& candidate) {
						return candidate.opcode ==
								RegionIR::Opcode::Cop1UpdateOuFlags &&
						       candidate.operand_count == 2 &&
						       candidate.operands[1] == raw &&
						       candidate.source_pc == exceptional.source_pc;
					});
				if (clamp == block.nodes.end() || flags == block.nodes.end())
					continue;
				result.plan.cop1_exception_for_guarded_raw[
					raw] = exceptional.id;
				result.plan.cop1_guarded_raw_for_exception[exceptional.id] =
					raw;
			}
		}
		std::vector<u8> backend_exit_word_demands;
		result.plan.value_low32_extensions.assign(program.value_count,
			RematerializationKind::None);
		for (RegionIR::ValueId value = 0; value < program.value_count; value++)
		{
			// Conditional facts become usable only after allocation selects and the
			// backend emits every required entry guard. Preserve the old unconditional
			// behavior here while the guarded phi-web selection below owns that choice.
			if (execution.value_low32_entry_guards[value].any())
				continue;
			switch (execution.value_low32_extensions[value])
			{
				case RegionExecution::Low32Extension::Sign:
					result.plan.value_low32_extensions[value] =
						RematerializationKind::SignExtendLow32;
					break;
				case RegionExecution::Low32Extension::Zero:
					result.plan.value_low32_extensions[value] =
						RematerializationKind::ZeroExtendLow32;
					break;
				case RegionExecution::Low32Extension::None:
					break;
			}
		}
		// Several semantic folds below must conservatively know every executable
		// exit before memory-SSA forwarding has been discovered. Build the ordinary
		// demand first, then rebuild it after the forwarding proof removes any exact
		// same-store load exit.
		if (!BuildBackendExitWordDemands(program, execution,
				options.preflighted_memory_operations, nullptr,
				options.aggregate_cycle_header_block,
				&backend_exit_word_demands))
		{
			return Fail(BuildFailure::InvalidExecutionPlan,
				program.entry_block, RegionIR::INVALID_VALUE);
		}
		auto has_only_executable_consumer = [&](RegionIR::ValueId value,
			RegionIR::ValueId consumer_id) {
			for (const RegionIR::Block& consumer_block : program.blocks)
			{
				for (const RegionIR::Node& consumer : consumer_block.nodes)
				{
					if (consumer.id == consumer_id || IsBinding(consumer.opcode))
						continue;
					for (u32 operand = 0; operand < consumer.operand_count; operand++)
					{
						if (consumer.operands[operand] == value)
							return false;
					}
				}
				if (consumer_block.terminator.condition == value)
					return false;
			}
			return true;
		};
		// Preserve the VU0 STATUS recurrence across compiler-owned direct-call
		// blocks.  The typed IR deliberately gives every block a complete state
		// parameter map, so an otherwise direct SYNCMSFLAGS def-use chain is split
		// by one or more phi-like parameters at a JAL/JR edge.  Resolve only a
		// strictly linear state web: the target block has one internal predecessor,
		// the supplied value has one state-edge use, and every intermediate value
		// has no executable node consumer.  Entry parameters are excluded because
		// they also have an external canonical-state predecessor.  This is a CFG and
		// effect proof; source PCs and opcode sequences do not participate.
		std::vector<std::vector<const RegionIR::Transfer*>> internal_incoming(
			program.blocks.size());
		std::vector<u32> internal_state_edge_uses(program.value_count, 0);
		for (const RegionIR::Block& source_block : program.blocks)
		{
			(void)RegionIR::VisitInternalTransfers(source_block.terminator,
				[&](const RegionIR::Transfer& transfer, u8) {
					if (transfer.target_block >= program.blocks.size())
						return false;
					internal_incoming[transfer.target_block].push_back(&transfer);
					for (size_t slot = 0;
						slot < RegionExecution::STATE_SLOT_COUNT; slot++)
					{
						const RegionIR::ValueId source =
							RegionExecution::StateValue(transfer.state, slot);
						if (source < internal_state_edge_uses.size())
							internal_state_edge_uses[source]++;
					}
					return true;
				});
		}
		std::vector<RegionIR::ValueId> linear_state_predecessor(
			program.value_count, RegionIR::INVALID_VALUE);
		std::vector<RegionIR::ValueId> linear_memory_predecessor(
			program.value_count, RegionIR::INVALID_VALUE);
		for (u32 block_index = 0; block_index < program.blocks.size(); block_index++)
		{
			if (block_index == program.entry_block ||
				internal_incoming[block_index].size() != 1)
			{
				continue;
			}
			const RegionIR::Transfer& incoming =
				*internal_incoming[block_index].front();
			const RegionIR::StateMap& parameters =
				program.blocks[block_index].parameters;
			for (size_t slot = 0; slot < RegionExecution::STATE_SLOT_COUNT; slot++)
			{
				const RegionIR::ValueId target =
					RegionExecution::StateValue(parameters, slot);
				const RegionIR::ValueId source =
					RegionExecution::StateValue(incoming.state, slot);
				const RegionIR::Node* const parameter =
					target < definitions.size() ? definitions[target] : nullptr;
				if (parameter && parameter->opcode == RegionIR::Opcode::Parameter &&
					parameter->immediate == slot && source < definitions.size() &&
					definitions[source] && parameter->type == definitions[source]->type)
				{
					linear_state_predecessor[target] = source;
				}
			}
			const RegionIR::ValueId memory_target = parameters.memory_effect;
			const RegionIR::ValueId memory_source = incoming.state.memory_effect;
			const RegionIR::Node* const memory_parameter =
				memory_target < definitions.size() ? definitions[memory_target] : nullptr;
			if (memory_parameter &&
				memory_parameter->opcode == RegionIR::Opcode::Parameter &&
				memory_parameter->type == RegionIR::ValueType::MemoryEffect &&
				memory_source < definitions.size() && definitions[memory_source] &&
				definitions[memory_source]->type == RegionIR::ValueType::MemoryEffect)
			{
				linear_memory_predecessor[memory_target] = memory_source;
			}
		}
		auto resolve_linear_state_predecessor = [&](RegionIR::ValueId value,
			RegionIR::ValueId direct_consumer) {
			RegionIR::ValueId current = value;
			RegionIR::ValueId permitted_consumer = direct_consumer;
			for (u32 depth = 0; depth <= program.value_count; depth++)
			{
				if (current >= definitions.size() || !definitions[current] ||
					!has_only_executable_consumer(current, permitted_consumer))
				{
					return RegionIR::INVALID_VALUE;
				}
				const RegionIR::ValueId predecessor =
					linear_state_predecessor[current];
				if (predecessor == RegionIR::INVALID_VALUE)
					return current;
				if (predecessor == current ||
					internal_state_edge_uses[predecessor] != 1)
				{
					return RegionIR::INVALID_VALUE;
				}
				current = predecessor;
				permitted_consumer = RegionIR::INVALID_VALUE;
			}
			return RegionIR::INVALID_VALUE;
		};
		for (const RegionIR::Block& block : program.blocks)
		{
			for (const RegionIR::Node& multiply : block.nodes)
			{
				if (multiply.opcode != RegionIR::Opcode::Vu0MulRaw ||
					multiply.operand_count != 2)
				{
					continue;
				}
				for (u32 operand = 0; operand < multiply.operand_count; operand++)
				{
					const RegionIR::ValueId broadcast_id = multiply.operands[operand];
					const RegionIR::Node* const broadcast =
						broadcast_id < definitions.size() ? definitions[broadcast_id] : nullptr;
					if (!broadcast || broadcast->opcode !=
							RegionIR::Opcode::Vu0BroadcastLane ||
						broadcast->operand_count != 1 || broadcast->immediate >= 4 ||
						backend_exit_word_demands[broadcast_id] != 0 ||
						!has_only_executable_consumer(broadcast_id, multiply.id))
					{
						continue;
					}
					result.plan.vu0_folded_broadcast_source[broadcast_id] =
						broadcast->operands[0];
				}
			}
		}
		// Keep the verifier-visible MUL and ADD rounding points, but let their sole
		// executable consumer own one allocation/lowering unit. This is pure SSA
		// def-use fusion: source PC, title, and instruction sequence never participate.
		for (const RegionIR::Block& block : program.blocks)
		{
			for (const RegionIR::Node& add : block.nodes)
			{
				if (add.opcode != RegionIR::Opcode::Vu0AddRaw ||
					add.operand_count != 2)
				{
					continue;
				}
				for (u32 operand = 0; operand < add.operand_count; operand++)
				{
					const RegionIR::ValueId multiply_id = add.operands[operand];
					const RegionIR::Node* const multiply = multiply_id < definitions.size() ?
						definitions[multiply_id] : nullptr;
					if (!multiply || multiply->opcode != RegionIR::Opcode::Vu0MulRaw ||
						multiply->operand_count != 2 ||
						backend_exit_word_demands[multiply_id] != 0 ||
						!has_only_executable_consumer(multiply_id, add.id))
					{
						continue;
					}
					result.plan.vu0_add_fused_mul[add.id] = multiply_id;
					break;
				}
			}
		}
		// Preserve the verifier's explicit MAC->STATUS graph, but avoid allocating
		// and packing an intermediate architectural MAC word when STATUS is its only
		// executable consumer. Any edge/exit demand discovered below still revives
		// the MAC node independently, so this cannot erase an observable value.
		for (const RegionIR::Block& block : program.blocks)
		{
			for (const RegionIR::Node& status : block.nodes)
			{
				if (status.opcode != RegionIR::Opcode::Vu0StatusFlagsFromMac ||
					status.operand_count != 1 ||
					status.operands[0] >= definitions.size())
				{
					continue;
				}
				const RegionIR::Node* const mac = definitions[status.operands[0]];
				if (!mac || mac->opcode != RegionIR::Opcode::Vu0MacFlagsFromRaw ||
					mac->operand_count != 1 || mac->immediate != 0x0f ||
					backend_exit_word_demands[mac->id] != 0)
				{
					continue;
				}
				bool only_status_consumer = true;
				for (const RegionIR::Block& consumer_block : program.blocks)
				{
					for (const RegionIR::Node& consumer : consumer_block.nodes)
					{
						if (consumer.id == status.id || IsBinding(consumer.opcode))
							continue;
						for (u32 operand = 0; operand < consumer.operand_count; operand++)
							only_status_consumer &= consumer.operands[operand] != mac->id;
					}
					only_status_consumer &=
						consumer_block.terminator.condition != mac->id;
				}
				if (only_status_consumer)
					result.plan.vu0_status_direct_raw[status.id] = mac->operands[0];
			}
		}
		// STATUS's finite fast path has already proven that a full-mask clamp is
		// the identity. If raw has no other executable or exit consumer, let that
		// classifier own the exceptional normalization instead of repeating the
		// same exponent work unconditionally before it.
		for (const RegionIR::Block& block : program.blocks)
		{
			for (const RegionIR::Node& status : block.nodes)
			{
				const RegionIR::ValueId raw = status.id <
						result.plan.vu0_status_direct_raw.size() ?
					result.plan.vu0_status_direct_raw[status.id] :
					RegionIR::INVALID_VALUE;
				if (raw == RegionIR::INVALID_VALUE || status.operand_count != 1 ||
					raw >= definitions.size() || backend_exit_word_demands[raw] != 0)
				{
					continue;
				}
				const RegionIR::Node* const mac = definitions[status.operands[0]];
				if (!mac || mac->opcode != RegionIR::Opcode::Vu0MacFlagsFromRaw ||
					mac->operand_count != 1 || mac->operands[0] != raw)
				{
					continue;
				}
				const RegionIR::Node* clamp = nullptr;
				bool safe = true;
				for (const RegionIR::Block& consumer_block : program.blocks)
				{
					for (const RegionIR::Node& consumer : consumer_block.nodes)
					{
						if (IsBinding(consumer.opcode))
							continue;
						for (u32 operand = 0; operand < consumer.operand_count; operand++)
						{
							if (consumer.operands[operand] != raw)
								continue;
							if (consumer.id == mac->id)
								continue;
							if (consumer.opcode == RegionIR::Opcode::Vu0ClampFmacResult &&
								consumer.operand_count == 1 && consumer.immediate == 0x0f &&
								!clamp)
							{
								clamp = &consumer;
								continue;
							}
							safe = false;
						}
					}
					safe &= consumer_block.terminator.condition != raw;
				}
				if (safe && clamp)
				{
					result.plan.vu0_clamp_deferred_to_status[clamp->id] = status.id;
					result.plan.vu0_status_deferred_clamp[status.id] = clamp->id;
				}
			}
		}
		// A demanded full MAC classifier has the same complete finite/edge split.
		// Record it as the alternate owner for clamps whose STATUS cannot bypass an
		// architecturally observed MAC word.
		for (const RegionIR::Block& block : program.blocks)
		{
			for (const RegionIR::Node& mac : block.nodes)
			{
				if (mac.opcode != RegionIR::Opcode::Vu0MacFlagsFromRaw ||
					mac.operand_count != 1 || mac.immediate != 0x0f ||
					mac.operands[0] >= definitions.size() ||
					backend_exit_word_demands[mac.operands[0]] != 0)
				{
					continue;
				}
				const RegionIR::ValueId raw = mac.operands[0];
				const RegionIR::Node* clamp = nullptr;
				bool safe = true;
				for (const RegionIR::Block& consumer_block : program.blocks)
				{
					for (const RegionIR::Node& consumer : consumer_block.nodes)
					{
						if (IsBinding(consumer.opcode))
							continue;
						for (u32 operand = 0; operand < consumer.operand_count; operand++)
						{
							if (consumer.operands[operand] != raw || consumer.id == mac.id)
								continue;
							if (consumer.opcode == RegionIR::Opcode::Vu0ClampFmacResult &&
								consumer.operand_count == 1 && consumer.immediate == 0x0f &&
								!clamp)
							{
								clamp = &consumer;
								continue;
							}
							safe = false;
						}
					}
					safe &= consumer_block.terminator.condition != raw;
				}
				if (safe && clamp &&
					result.plan.vu0_clamp_deferred_to_status[clamp->id] ==
						RegionIR::INVALID_VALUE)
				{
					result.plan.vu0_clamp_deferred_to_mac[clamp->id] = mac.id;
					result.plan.vu0_mac_deferred_clamp[mac.id] = clamp->id;
				}
			}
		}
		// Fold only a direct SSA chain whose intermediate VI STATUS result is
		// unobservable. The verifier and exit-demand pass remain authoritative: a
		// helper, side exit, branch, or second executable consumer prevents this
		// algebraic lowering automatically.
		for (const RegionIR::Block& block : program.blocks)
		{
			for (const RegionIR::Node& sync : block.nodes)
			{
				if (sync.opcode != RegionIR::Opcode::Vu0SyncStatusControl ||
					sync.operand_count != 2 || sync.operands[0] >= definitions.size())
				{
					continue;
				}
				const RegionIR::ValueId predecessor_id =
					resolve_linear_state_predecessor(sync.operands[0], sync.id);
				const RegionIR::Node* const predecessor =
					predecessor_id < definitions.size() ?
						definitions[predecessor_id] : nullptr;
				if (!predecessor || predecessor->opcode !=
						RegionIR::Opcode::Vu0SyncStatusControl ||
					predecessor->operand_count != 2 ||
					backend_exit_word_demands[predecessor->id] != 0 ||
					result.plan.vu0_sync_folded_into[predecessor->id] !=
						RegionIR::INVALID_VALUE)
				{
					continue;
				}
				result.plan.vu0_sync_folded_predecessor[sync.id] = predecessor->id;
				result.plan.vu0_sync_folded_into[predecessor->id] = sync.id;
			}
		}
		// An intermediate macro STATUS value is not architecturally observable when
		// its only executable consumer is this SYNCMSFLAGS recurrence and that sync
		// itself feeds another sync. The finite path then needs only to update the
		// sticky sign category. Zero, denormal, and overflow still branch to the
		// complete PCSX2 classifier.
		for (const RegionIR::Block& block : program.blocks)
		{
			for (const RegionIR::Node& sync : block.nodes)
			{
				if (sync.opcode != RegionIR::Opcode::Vu0SyncStatusControl ||
					sync.operand_count != 2 || sync.operands[1] >= definitions.size() ||
					result.plan.vu0_sync_folded_into[sync.id] == RegionIR::INVALID_VALUE)
				{
					continue;
				}
				const RegionIR::Node* const status = definitions[sync.operands[1]];
				const RegionIR::ValueId raw = status && status->id <
						result.plan.vu0_status_direct_raw.size() ?
					result.plan.vu0_status_direct_raw[status->id] :
					RegionIR::INVALID_VALUE;
				if (!status || status->opcode !=
						RegionIR::Opcode::Vu0StatusFlagsFromMac ||
					raw == RegionIR::INVALID_VALUE ||
					backend_exit_word_demands[status->id] != 0 ||
					!has_only_executable_consumer(status->id, sync.id))
				{
					continue;
				}
				result.plan.vu0_sync_direct_raw[sync.id] = raw;
				result.plan.vu0_status_fused_sync[status->id] = sync.id;
			}
		}
		// Establish VU0 loop-invariant normalization before backward demand. This
		// lets the entry prefix load the canonical parameter directly into the
		// normalized value's allocation instead of keeping both raw and normalized
		// quads live. The later common COP1/VU0 hoist pass retains the complete
		// lifetime proof; this early pass changes only physical input demand.
		if (program.entry_block < program.blocks.size())
		{
			const RegionIR::Block& header = program.blocks[program.entry_block];
			for (size_t slot = 0; slot < RegionExecution::STATE_SLOT_COUNT; slot++)
			{
				const RegionIR::ValueId parameter =
					RegionExecution::StateValue(header.parameters, slot);
				if (parameter >= definitions.size() || !definitions[parameter] ||
					definitions[parameter]->type !=
						RegionIR::ValueType::VuF32x4Bits)
				{
					continue;
				}
				bool have_backedge = false;
				bool invariant = true;
				for (u32 source_block = 0; source_block < program.blocks.size();
					source_block++)
				{
					const RegionIR::Block& source = program.blocks[source_block];
					auto inspect = [&](const RegionIR::Transfer& transfer) {
						if (transfer.target_block != program.entry_block)
							return;
						have_backedge = true;
						invariant &= source_block == program.entry_block &&
							RegionExecution::StateValue(transfer.state, slot) == parameter;
					};
					(void)RegionIR::VisitInternalTransfers(source.terminator,
						[&](const RegionIR::Transfer& transfer, u8) {
							inspect(transfer);
							return true;
						});
				}
				if (!have_backedge || !invariant)
					continue;
				for (const RegionIR::Node& node : header.nodes)
				{
					RegionIR::ValueId source = node.operand_count == 1 ?
						node.operands[0] : RegionIR::INVALID_VALUE;
					if (source < result.plan.vu0_idle_alias.size() &&
						result.plan.vu0_idle_alias[source] != RegionIR::INVALID_VALUE)
					{
						source = result.plan.vu0_idle_alias[source];
					}
					if (node.opcode == RegionIR::Opcode::Vu0NormalizeVector &&
						source == parameter &&
						result.plan.vu0_normalize_alias[node.id] ==
							RegionIR::INVALID_VALUE)
					{
						result.plan.vu0_hoisted_normalize[node.id] = 1;
						break;
					}
				}
			}
		}
		result.plan.exact_i32_aliases = BuildExactI32Aliases(
			program, definitions, ranges);
		result.plan.folded_exact_i32_values = static_cast<u32>(std::count_if(
			result.plan.exact_i32_aliases.begin(),
			result.plan.exact_i32_aliases.end(), [](RegionIR::ValueId value) {
				return value != RegionIR::INVALID_VALUE;
			}));

		// Preserve a direct-call result in the compiler-owned state graph instead of
		// writing it to guest RAM and subsequently reading it back into another VF.
		// The guest store is still emitted.  We remove only a load whose incoming
		// memory token resolves through unique internal edges and any number of
		// intervening read-only memory effects to that exact store, whose effective
		// address is symbolically identical, and whose stored value is present in the
		// load block's verified architectural state parameters.  An intervening load
		// cannot change the stored bytes; an intervening store always stops this proof
		// because distinct EE virtual addresses are not by themselves a physical
		// no-alias certificate under the vTLB.
		// Therefore a failed translation, alignment check, SMC check, event horizon,
		// or other observer exits before the forwarded use exactly as before.
		std::vector<RegionIR::ValueId> memory_value_for_effect(
			program.value_count, RegionIR::INVALID_VALUE);
		for (const RegionIR::Block& block : program.blocks)
		{
			for (const RegionIR::Node& node : block.nodes)
			{
				if (node.opcode == RegionIR::Opcode::MemoryLoadValue &&
					node.operand_count == 1 &&
					node.operands[0] < memory_value_for_effect.size())
				{
					memory_value_for_effect[node.operands[0]] = node.id;
				}
			}
		}
		auto semantic_alias = [&](RegionIR::ValueId value) {
			for (u32 depth = 0; depth < program.value_count; depth++)
			{
				if (value >= program.value_count)
					return RegionIR::INVALID_VALUE;
				RegionIR::ValueId next = RegionIR::INVALID_VALUE;
				if (result.plan.forwarded_memory_load_alias[value] !=
					RegionIR::INVALID_VALUE)
				{
					next = result.plan.forwarded_memory_load_alias[value];
				}
				else if (result.plan.exact_i32_aliases[value] !=
					RegionIR::INVALID_VALUE)
				{
					next = result.plan.exact_i32_aliases[value];
				}
				else if (result.plan.vu0_idle_alias[value] !=
					RegionIR::INVALID_VALUE)
				{
					next = result.plan.vu0_idle_alias[value];
				}
				else if (result.plan.vu0_merge_alias[value] !=
					RegionIR::INVALID_VALUE)
				{
					next = result.plan.vu0_merge_alias[value];
				}
				if (next == RegionIR::INVALID_VALUE)
					return value;
				if (next == value)
					return RegionIR::INVALID_VALUE;
				value = next;
			}
			return RegionIR::INVALID_VALUE;
		};
		auto state_origin = [&](RegionIR::ValueId value) {
			for (u32 depth = 0; depth < program.value_count; depth++)
			{
				value = semantic_alias(value);
				if (value >= definitions.size() || !definitions[value])
					return RegionIR::INVALID_VALUE;
				const RegionIR::ValueId predecessor =
					linear_state_predecessor[value];
				if (predecessor == RegionIR::INVALID_VALUE)
					return value;
				if (predecessor == value)
					return RegionIR::INVALID_VALUE;
				value = predecessor;
			}
			return RegionIR::INVALID_VALUE;
		};
		struct AffineAddress
		{
			RegionIR::ValueId root = RegionIR::INVALID_VALUE;
			u32 offset = 0;
			bool valid = false;
		};
		// Resolve the architecturally observed low 32 address bits rather than the
		// container which happens to carry them.  Builder::WriteLow64 represents an
		// EE argument move/update as ReplaceLow64(I128, I64); treating that distinct
		// container node as the address root loses exact identity when one stack
		// object is passed through different argument GPRs at adjacent direct calls.
		// Every case below is a bit-preserving projection from the verified typed IR.
		// Unknown arithmetic remains an opaque SSA root, so this can miss an alias but
		// can never manufacture one from equal runtime values.
		auto resolve_low32_address = [&](auto&& self, RegionIR::ValueId value,
			u32 depth) -> AffineAddress {
			if (depth > program.value_count)
				return {};
			value = semantic_alias(value);
			if (value >= definitions.size() || !definitions[value])
				return {};
			const RegionIR::Node& node = *definitions[value];
			if (node.opcode == RegionIR::Opcode::ConstantI32 ||
				node.opcode == RegionIR::Opcode::ConstantAddress)
			{
				return {RegionIR::INVALID_VALUE, static_cast<u32>(node.literal), true};
			}
			if (node.opcode == RegionIR::Opcode::Parameter)
			{
				if (IsArchitecturalZeroParameter(program, node))
					return {RegionIR::INVALID_VALUE, 0, true};
				const RegionIR::ValueId predecessor =
					linear_state_predecessor[value];
				return predecessor != RegionIR::INVALID_VALUE ?
					self(self, predecessor, depth + 1) :
					AffineAddress{value, 0, true};
			}
			if ((node.opcode == RegionIR::Opcode::ExtractLow32 ||
				 node.opcode == RegionIR::Opcode::ExtractLow64 ||
				 node.opcode == RegionIR::Opcode::AddressFromI32 ||
				 node.opcode == RegionIR::Opcode::BitcastI32ToF32Bits ||
				 node.opcode == RegionIR::Opcode::BitcastF32BitsToI32 ||
				 node.opcode == RegionIR::Opcode::SignExtend32To64 ||
				 node.opcode == RegionIR::Opcode::ZeroExtend32To64 ||
				 node.opcode == RegionIR::Opcode::Truncate64To32) &&
				node.operand_count == 1)
			{
				return self(self, node.operands[0], depth + 1);
			}
			if (node.opcode == RegionIR::Opcode::ReplaceLow64 &&
				node.operand_count == 2)
			{
				return self(self, node.operands[1], depth + 1);
			}
			if (node.opcode == RegionIR::Opcode::ReplaceHigh64 &&
				node.operand_count == 2)
			{
				return self(self, node.operands[0], depth + 1);
			}
			if ((node.opcode == RegionIR::Opcode::EffectiveAddress32 ||
				 node.opcode == RegionIR::Opcode::Add32) &&
				node.operand_count == 2)
			{
				const AffineAddress left =
					self(self, node.operands[0], depth + 1);
				const AffineAddress right =
					self(self, node.operands[1], depth + 1);
				if (!left.valid || !right.valid ||
					(left.root != RegionIR::INVALID_VALUE &&
					 right.root != RegionIR::INVALID_VALUE))
				{
					return {};
				}
				return {left.root != RegionIR::INVALID_VALUE ? left.root : right.root,
					left.offset + right.offset, true};
			}
			return {value, 0, true};
		};
		auto reaching_memory_store = [&](RegionIR::ValueId effect) {
			for (u32 depth = 0; depth < program.value_count; depth++)
			{
				if (effect >= definitions.size() || !definitions[effect])
					return RegionIR::INVALID_VALUE;
				const RegionIR::Node& definition = *definitions[effect];
				if (definition.opcode == RegionIR::Opcode::MemoryStore)
					return effect;
				if (definition.opcode == RegionIR::Opcode::MemoryLoad)
				{
					if (definition.operand_count != 3 ||
						definition.operands[0] == effect)
					{
						return RegionIR::INVALID_VALUE;
					}
					effect = definition.operands[0];
					continue;
				}
				const RegionIR::ValueId predecessor =
					linear_memory_predecessor[effect];
				if (predecessor == RegionIR::INVALID_VALUE)
					return effect;
				if (predecessor == effect)
					return RegionIR::INVALID_VALUE;
				effect = predecessor;
			}
			return RegionIR::INVALID_VALUE;
		};
		for (u32 block_index = 0; block_index < program.blocks.size(); block_index++)
		{
			const RegionIR::Block& block = program.blocks[block_index];
			if (block_index == program.entry_block ||
				internal_incoming[block_index].size() != 1)
			{
				continue;
			}
			for (const RegionIR::Node& load : block.nodes)
			{
				if (load.opcode != RegionIR::Opcode::MemoryLoad ||
					load.operand_count != 3 || load.immediate != static_cast<u32>(
						RegionIR::MemoryAccessKind::LoadVu0Vector))
				{
					continue;
				}
				result.plan.memory_forward_candidates++;
				const RegionIR::ValueId store_id =
					reaching_memory_store(load.operands[0]);
				const RegionIR::Node* const store = store_id < definitions.size() ?
					definitions[store_id] : nullptr;
				if (!store || store->opcode != RegionIR::Opcode::MemoryStore ||
					store->operand_count != 3 || store->immediate != static_cast<u32>(
						RegionIR::MemoryAccessKind::StoreVu0Vector))
				{
					continue;
				}
				result.plan.memory_forward_reaching_stores++;
				const AffineAddress stored_address = resolve_low32_address(
					resolve_low32_address, store->operands[1], 0);
				const AffineAddress loaded_address = resolve_low32_address(
					resolve_low32_address, load.operands[1], 0);
				if (!stored_address.valid || !loaded_address.valid ||
					stored_address.root != loaded_address.root ||
					stored_address.offset != loaded_address.offset)
				{
					continue;
				}
				result.plan.memory_forward_address_matches++;
				const RegionIR::ValueId stored_origin =
					state_origin(store->operands[2]);
				if (stored_origin == RegionIR::INVALID_VALUE)
				{
					continue;
				}
				RegionIR::ValueId carried = RegionIR::INVALID_VALUE;
				for (size_t slot = 0; slot < RegionExecution::STATE_SLOT_COUNT; slot++)
				{
					const RegionIR::ValueId parameter =
						RegionExecution::StateValue(block.parameters, slot);
					if (parameter >= definitions.size() || !definitions[parameter] ||
						definitions[parameter]->opcode != RegionIR::Opcode::Parameter ||
						definitions[parameter]->type !=
							RegionIR::ValueType::VuF32x4Bits ||
						state_origin(parameter) != stored_origin)
					{
						continue;
					}
					carried = parameter;
					break;
				}
				const RegionIR::ValueId loaded_value = load.id <
					memory_value_for_effect.size() ?
					memory_value_for_effect[load.id] : RegionIR::INVALID_VALUE;
				if (carried == RegionIR::INVALID_VALUE ||
					loaded_value >= definitions.size() || !definitions[loaded_value] ||
					definitions[loaded_value]->type !=
						RegionIR::ValueType::VuF32x4Bits)
				{
					continue;
				}
				result.plan.memory_forward_state_matches++;
				result.plan.forwarded_memory_load_alias[loaded_value] = carried;
				result.plan.forwarded_memory_load_effect[load.id] = 1;
				result.plan.forwarded_memory_loads++;
				if (carried < vu0_normalized.size() && vu0_normalized[carried])
					vu0_normalized[loaded_value] = 1;
			}
		}
		// Reuse the normalized-state proof through the newly forwarded value.  The
		// normalizer remains explicit in IR for interpretation and exit-map checking;
		// only its generated storage aliases the carried state parameter.
		for (const RegionIR::Block& block : program.blocks)
		{
			for (const RegionIR::Node& normalize : block.nodes)
			{
				if (normalize.opcode != RegionIR::Opcode::Vu0NormalizeVector ||
					normalize.operand_count != 1 ||
					result.plan.vu0_normalize_alias[normalize.id] !=
						RegionIR::INVALID_VALUE)
				{
					continue;
				}
				const RegionIR::ValueId source = semantic_alias(normalize.operands[0]);
				if (source < vu0_normalized.size() && vu0_normalized[source])
					result.plan.vu0_normalize_alias[normalize.id] = source;
			}
		}

		if (!BuildBackendExitWordDemands(program, execution,
				options.preflighted_memory_operations,
				&result.plan.forwarded_memory_load_effect,
				options.aggregate_cycle_header_block,
				&backend_exit_word_demands,
				&result.plan.executable_exit_sites,
				&result.plan.executable_exit_state_bindings,
				&result.plan.executable_exit_state_words,
				&result.plan.executable_exit_sites_by_kind,
				&result.plan.executable_exit_state_words_by_kind,
				&result.plan.executable_control_exit_sites_by_target,
				&result.plan.executable_control_exit_state_words_by_target,
				&result.plan.executable_exit_sites_by_block,
				&result.plan.executable_exit_state_words_by_block))
		{
			return Fail(BuildFailure::InvalidExecutionPlan,
				program.entry_block, RegionIR::INVALID_VALUE);
		}
		if (!BuildWordDemands(program, execution, definitions,
				result.plan.cop1_normalize_alias,
				result.plan.vu0_idle_alias,
				result.plan.vu0_normalize_alias,
				result.plan.vu0_hoisted_normalize,
				result.plan.vu0_merge_alias,
				result.plan.vu0_status_direct_raw,
				result.plan.vu0_sync_direct_raw,
				result.plan.vu0_add_fused_mul,
				result.plan.vu0_folded_broadcast_source,
				result.plan.exact_i32_aliases,
				result.plan.forwarded_memory_load_alias,
				result.plan.value_low32_extensions,
				backend_exit_word_demands,
				options.hoisted_memory_operations,
				&result.plan.value_word_demands))
		{
			return Fail(BuildFailure::InvalidExecutionPlan,
				program.entry_block, RegionIR::INVALID_VALUE);
		}
		for (RegionIR::ValueId overflow = 0; overflow < program.value_count;
			overflow++)
		{
			if (result.plan.flagged_add_for_signed_overflow[overflow] !=
				RegionIR::INVALID_VALUE)
			{
				result.plan.value_word_demands[overflow] = 0;
			}
		}
		for (RegionIR::ValueId exceptional = 0;
			exceptional < program.value_count; exceptional++)
		{
			if (result.plan.cop1_guarded_raw_for_exception[exceptional] !=
				RegionIR::INVALID_VALUE)
			{
				result.plan.value_word_demands[exceptional] = 0;
			}
		}
		if ((options.fixed_cycle_word_mask & ~0x3u) != 0 ||
			options.fixed_cycle_word_mask == 0)
		{
			return Fail(BuildFailure::InvalidExecutionPlan,
				program.entry_block, RegionIR::INVALID_VALUE);
		}
		for (RegionIR::ValueId value = 0; value < program.value_count; value++)
		{
			if (definitions[value] &&
				definitions[value]->type == RegionIR::ValueType::Cycle)
			{
				result.plan.value_word_demands[value] &=
					options.fixed_cycle_word_mask;
			}
		}
		for (RegionIR::ValueId value = 0; value < program.value_count; value++)
		{
			if (result.plan.vu0_folded_broadcast_source[value] !=
				RegionIR::INVALID_VALUE)
			{
				result.plan.value_word_demands[value] = 0;
			}
		}
		for (RegionIR::ValueId status = 0; status < program.value_count; status++)
		{
			const RegionIR::ValueId clamp =
				result.plan.vu0_status_deferred_clamp[status];
			if (clamp == RegionIR::INVALID_VALUE)
				continue;
			const RegionIR::ValueId fused_sync =
				result.plan.vu0_status_fused_sync[status];
			const bool classifier_demanded =
				result.plan.value_word_demands[status] != 0 ||
				(fused_sync != RegionIR::INVALID_VALUE &&
				 result.plan.value_word_demands[fused_sync] != 0);
			if (!classifier_demanded ||
				result.plan.value_word_demands[clamp] == 0)
			{
				result.plan.vu0_status_deferred_clamp[status] =
					RegionIR::INVALID_VALUE;
				result.plan.vu0_clamp_deferred_to_status[clamp] =
					RegionIR::INVALID_VALUE;
			}
		}
		for (RegionIR::ValueId mac = 0; mac < program.value_count; mac++)
		{
			const RegionIR::ValueId clamp = result.plan.vu0_mac_deferred_clamp[mac];
			if (clamp == RegionIR::INVALID_VALUE)
				continue;
			if (result.plan.value_word_demands[mac] == 0 ||
				result.plan.value_word_demands[clamp] == 0)
			{
				result.plan.vu0_mac_deferred_clamp[mac] = RegionIR::INVALID_VALUE;
				result.plan.vu0_clamp_deferred_to_mac[clamp] =
					RegionIR::INVALID_VALUE;
			}
		}

		// Select the same exact multi-result clamp/O-U pair consumed by the A32
		// backend before constructing physical live intervals. Both values must be
		// demanded, share one raw node/source instruction, and reside in one block.
		// The IR and execution plan retain their independent nodes and exit maps.
		for (const RegionIR::Block& block : program.blocks)
		{
			for (const RegionIR::Node& flag : block.nodes)
			{
				if (flag.opcode != RegionIR::Opcode::Cop1UpdateOuFlags ||
					flag.operand_count != 2 ||
					result.plan.value_word_demands[flag.id] == 0)
				{
					continue;
				}
				for (const RegionIR::Node& clamp : block.nodes)
				{
					if (clamp.opcode != RegionIR::Opcode::Cop1ClampOuResult ||
						clamp.operand_count != 1 ||
						clamp.operands[0] != flag.operands[1] ||
						clamp.source_pc != flag.source_pc ||
						result.plan.value_word_demands[clamp.id] == 0)
					{
						continue;
					}
					result.plan.cop1_fused_flag_for_clamp[clamp.id] = flag.id;
					result.plan.cop1_fused_clamp_for_flag[flag.id] = clamp.id;
					break;
				}
			}
		}
		// On the fallthrough of its exact exceptional guard, the clamp is an
		// identity. FCR31 cannot use the same path-insensitive alias: an exceptional
		// veneer rejoins this graph with current O/U causes which the following
		// ordinary arithmetic instruction must clear.
		for (const RegionIR::Block& block : program.blocks)
		{
			for (const RegionIR::Node& clamp : block.nodes)
			{
				if (clamp.opcode != RegionIR::Opcode::Cop1ClampOuResult ||
					clamp.operand_count != 1 ||
					result.plan.cop1_exception_for_guarded_raw[
						clamp.operands[0]] == RegionIR::INVALID_VALUE)
				{
					continue;
				}
				result.plan.cop1_guarded_clamp_alias[clamp.id] =
					clamp.operands[0];
				const RegionIR::ValueId flag_id =
					result.plan.cop1_fused_flag_for_clamp[clamp.id];
				const RegionIR::Node* const flag = flag_id < definitions.size() ?
					definitions[flag_id] : nullptr;
				if (!flag || flag->operand_count != 2)
					continue;
				// The exceptional guard now owns classification and the clamp aliases
				// raw. The flag node therefore emits only its normal-path clear; do not
				// leave the former clamp-owned multi-result emission selected.
				result.plan.cop1_fused_flag_for_clamp[clamp.id] =
					RegionIR::INVALID_VALUE;
				result.plan.cop1_fused_clamp_for_flag[flag->id] =
					RegionIR::INVALID_VALUE;
			}
		}

		// The first hoist is deliberately a complete one-block natural-loop
		// proof, not a heuristic.  An entry FPR/ACC/VU vector parameter is invariant only if
		// every internal edge to the entry comes from that same block and carries
		// the exact same SSA parameter for the state slot.  External entry remains
		// arbitrary: the generated prologue normalizes it once after all guards.
		if (program.entry_block < program.blocks.size())
		{
			const RegionIR::Block& header = program.blocks[program.entry_block];
			for (size_t slot = 0; slot < RegionExecution::STATE_SLOT_COUNT; slot++)
			{
				const RegionIR::ValueId parameter =
					RegionExecution::StateValue(header.parameters, slot);
				if (parameter >= definitions.size() || !definitions[parameter] ||
					(definitions[parameter]->type != RegionIR::ValueType::F32Bits &&
					 definitions[parameter]->type != RegionIR::ValueType::VuF32x4Bits))
				{
					continue;
				}
				bool have_backedge = false;
				bool invariant = true;
				for (u32 source_block = 0; source_block < program.blocks.size();
					source_block++)
				{
					const RegionIR::Block& source = program.blocks[source_block];
					auto inspect = [&](const RegionIR::Transfer& transfer) {
						if (transfer.target_block != program.entry_block)
							return;
						have_backedge = true;
						invariant &= source_block == program.entry_block &&
							RegionExecution::StateValue(transfer.state, slot) == parameter;
					};
					(void)RegionIR::VisitInternalTransfers(source.terminator,
						[&](const RegionIR::Transfer& transfer, u8) {
							inspect(transfer);
							return true;
						});
				}
				if (!have_backedge || !invariant)
					continue;
				for (const RegionIR::Node& node : header.nodes)
				{
					if (node.opcode == RegionIR::Opcode::Cop1NormalizeInput &&
						node.operand_count == 1 && node.operands[0] == parameter &&
						result.plan.cop1_normalize_alias[node.id] ==
							RegionIR::INVALID_VALUE)
					{
						result.plan.cop1_hoisted_normalize[node.id] = 1;
						break;
					}
					RegionIR::ValueId vu0_source = node.operand_count == 1 ?
						node.operands[0] : RegionIR::INVALID_VALUE;
					if (vu0_source < result.plan.vu0_idle_alias.size() &&
						result.plan.vu0_idle_alias[vu0_source] != RegionIR::INVALID_VALUE)
					{
						vu0_source = result.plan.vu0_idle_alias[vu0_source];
					}
					if (node.opcode == RegionIR::Opcode::Vu0NormalizeVector &&
						node.operand_count == 1 && vu0_source == parameter &&
						result.plan.vu0_normalize_alias[node.id] ==
							RegionIR::INVALID_VALUE)
					{
						result.plan.vu0_hoisted_normalize[node.id] = 1;
						break;
					}
				}
			}
		}
		result.plan.value_representations.resize(program.value_count);
		result.plan.value_rematerialized_word_masks.assign(program.value_count, 0);
		result.plan.value_rematerializations.assign(program.value_count,
			RematerializationKind::None);
		std::vector<RematerializationKind> rematerialization_candidates =
			result.plan.value_low32_extensions;
		result.plan.folded_branch_conditions.assign(program.value_count, 0);
		result.plan.folded_effective_addresses.assign(program.value_count, 0);
		// A semantic low-word-extension fact becomes a physical rematerialization
		// candidate only when some remaining consumer still demands the high word.
		// Equality narrowing above may already have eliminated that demand.
		for (RegionIR::ValueId value = 0; value < program.value_count; value++)
		{
			if ((result.plan.value_word_demands[value] & 0x2) == 0)
				rematerialization_candidates[value] = RematerializationKind::None;
		}
		for (RegionIR::ValueId value = 0; value < program.value_count; value++)
		{
			const u8 demand = result.plan.value_word_demands[value];
			if (demand == 0)
				continue;
			const RegionIR::Node& node = *definitions[value];
			Representation representation{};
			representation.word_mask = demand;
			for (u8 word = 0; word < 4; word++)
				representation.words += (representation.word_mask >> word) & 1u;
			if (IsConstant(node.opcode) ||
				IsKnownZeroValue(program, definitions, value))
				representation.kind = RepresentationKind::Immediate;
			else if (node.type == RegionIR::ValueType::Cycle)
				representation.kind = RepresentationKind::FixedCycle;
			else if (node.type == RegionIR::ValueType::F32Bits)
				representation.kind = options.allocate_vfp_words_in_core ?
					RepresentationKind::CoreWords : RepresentationKind::VfpWord;
			else if (node.type == RegionIR::ValueType::VuF32x4Bits)
				representation.kind = RepresentationKind::NeonQ;
			else if (node.type == RegionIR::ValueType::I128)
			{
				const bool contiguous_one_or_two = representation.words <= 2 &&
					(demand == 0x1 || demand == 0x2 || demand == 0x4 ||
					 demand == 0x8 || demand == 0x3 || demand == 0x6 ||
					 demand == 0x0c);
				representation.kind = contiguous_one_or_two ?
					RepresentationKind::CoreWords : RepresentationKind::NeonQ;
			}
			else
				representation.kind = RepresentationKind::CoreWords;
			result.plan.value_representations[value] = representation;
			if (representation.kind == RepresentationKind::CoreWords ||
				representation.kind == RepresentationKind::VfpWord ||
				representation.kind == RepresentationKind::FixedCycle)
			{
				result.plan.demanded_scalar_values++;
			}
			else if (representation.kind == RepresentationKind::NeonQ)
				result.plan.demanded_full_vector_values++;
		}

		// The generated dispatcher owns one complete AAPCS frame. Scalar-only
		// regions can therefore use caller-saved s0-s15 as well as the preserved
		// s16-s29 bank without adding entry/exit work. Derive this from the final
		// representation demands, not from source opcodes: a single live NEON value
		// keeps s0-s15 reserved for q0-q3 and retains the disjoint s16-s29 bank.
		const bool scalar_only_vfp_bank =
			options.use_low_vfp_bank_when_neon_unused &&
			result.plan.demanded_full_vector_values == 0;
		if (!scalar_only_vfp_bank &&
			(static_cast<u32>(options.first_vfp_s) + options.vfp_s_registers > 30 ||
			 options.first_vfp_s < 16))
		{
			return Fail(BuildFailure::InvalidExecutionPlan,
				program.entry_block, RegionIR::INVALID_VALUE);
		}
		const u32 available_vfp_s_registers = options.vfp_s_registers +
			(scalar_only_vfp_bank ? 16u : 0u);
		const u32 resident_vfp_values = static_cast<u32>(std::count(
			result.plan.cop1_region_resident_normalize.begin(),
			result.plan.cop1_region_resident_normalize.end(), 1));
		if (resident_vfp_values > available_vfp_s_registers)
		{
			return Fail(BuildFailure::InvalidExecutionPlan,
				program.entry_block, RegionIR::INVALID_VALUE);
		}
		const u32 allocatable_vfp_s_registers =
			available_vfp_s_registers - resident_vfp_values;
		result.plan.first_vfp_s = scalar_only_vfp_bank ? 0 : options.first_vfp_s;
		result.plan.vfp_s_registers =
			static_cast<u8>(available_vfp_s_registers);
		result.plan.vfp_peak_s = resident_vfp_values;

		auto add_use = [&](RegionIR::ValueId value, u32 use_position) -> bool {
			if (value >= program.value_count || !definitions[value])
				return false;
			Range& range = ranges[value];
			range.end = std::max(range.end, use_position);
			range.uses++;
			return true;
		};
		auto add_vu0_arithmetic_use = [&](RegionIR::ValueId value,
			u32 use_position) -> bool {
			bool folded_broadcast = false;
			if (value < result.plan.vu0_folded_broadcast_source.size() &&
				result.plan.vu0_folded_broadcast_source[value] !=
					RegionIR::INVALID_VALUE)
			{
				value = result.plan.vu0_folded_broadcast_source[value];
				folded_broadcast = true;
			}
			// A folded broadcast is one scalar source consumed by every emitted
			// destination lane. The lane-wise fallback cannot let the result begin
			// in the source vector at the same position: its first store would
			// destroy the scalar before the remaining lanes read it. Keep that
			// vector live through the complete IR operation. Ordinary component-wise
			// operations retain the usual destructive-source affinity.
			return add_use(value, use_position + (folded_broadcast ? 1u : 0u));
		};
		auto operand_is_used = [&](const RegionIR::Node& node,
			u32 operand) -> bool {
			if (node.opcode == RegionIR::Opcode::MemoryLoad &&
				node.id < result.plan.forwarded_memory_load_effect.size() &&
				result.plan.forwarded_memory_load_effect[node.id] != 0)
			{
				return false;
			}
			if ((node.opcode == RegionIR::Opcode::MemoryLoad ||
				 node.opcode == RegionIR::Opcode::MemoryStore) && operand == 1 &&
				options.hoisted_memory_operations &&
				std::find(options.hoisted_memory_operations->begin(),
					options.hoisted_memory_operations->end(), node.id) !=
					options.hoisted_memory_operations->end())
			{
				return false;
			}
			const u8 demand = result.plan.value_word_demands[node.id];
			switch (node.opcode)
			{
				case RegionIR::Opcode::ReplaceLow64:
					return operand == 0 ? (demand & 0x0c) != 0 :
						operand == 1 && (demand & 0x03) != 0;
				case RegionIR::Opcode::ReplaceHigh64:
					return operand == 0 ? (demand & 0x03) != 0 :
						operand == 1 && (demand & 0x0c) != 0;
				case RegionIR::Opcode::PackLow64:
					return operand == 0 ? ((demand >> 2) & 0x03) != 0 :
						operand == 1 && (demand & 0x03) != 0;
				case RegionIR::Opcode::PackHigh64:
					return operand == 0 ? (demand & 0x03) != 0 :
						operand == 1 && (demand & 0x0c) != 0;
				default:
					return true;
			}
		};

		for (u32 block_index = 0; block_index < program.blocks.size(); block_index++)
		{
			const RegionIR::Block& block = program.blocks[block_index];
			for (const RegionIR::Node& node : block.nodes)
			{
				const bool intrinsic = node.opcode == RegionIR::Opcode::MemoryLoad ||
					node.opcode == RegionIR::Opcode::MemoryStore ||
					node.opcode == RegionIR::Opcode::ExitIfTrue ||
					node.opcode == RegionIR::Opcode::Cop1ExceptionalOuResult ||
					node.opcode == RegionIR::Opcode::Vu0RequireIdle;
				if (IsBinding(node.opcode) || node.opcode == RegionIR::Opcode::NoEffect ||
					(!intrinsic && result.plan.value_word_demands[node.id] == 0))
				{
					continue;
				}
					const bool deferred_branch_compare =
					block.terminator.kind == RegionIR::TerminatorKind::Branch &&
					block.terminator.condition == node.id &&
					IsDirectBranchComparison(node.opcode);
					const u32 use_position = deferred_branch_compare ?
						block_not_taken_edge[block_index] : ranges[node.id].begin;
					if (result.plan.exact_i32_aliases[node.id] !=
						RegionIR::INVALID_VALUE)
					{
						continue;
					}
					if (result.plan.vu0_idle_alias[node.id] !=
						RegionIR::INVALID_VALUE)
					{
						continue;
					}
					if (node.opcode == RegionIR::Opcode::Vu0AddRaw &&
						result.plan.vu0_add_fused_mul[node.id] !=
							RegionIR::INVALID_VALUE)
					{
						const RegionIR::ValueId multiply_id =
							result.plan.vu0_add_fused_mul[node.id];
						const RegionIR::Node* const multiply =
							multiply_id < definitions.size() ?
								definitions[multiply_id] : nullptr;
						if (!multiply || multiply->operand_count != 2)
							return Fail(BuildFailure::InvalidValue, block_index,
								multiply_id);
						const RegionIR::ValueId addend =
							node.operands[0] == multiply_id ?
								node.operands[1] : node.operands[0];
						if (!add_vu0_arithmetic_use(addend, use_position) ||
							!add_vu0_arithmetic_use(multiply->operands[0], use_position) ||
							!add_vu0_arithmetic_use(multiply->operands[1], use_position))
						{
							return Fail(BuildFailure::InvalidValue, block_index,
								node.id);
							}
							continue;
						}
					if (node.opcode ==
							RegionIR::Opcode::Vu0SyncStatusControl &&
							node.id < result.plan.vu0_sync_direct_raw.size() &&
							result.plan.vu0_sync_direct_raw[node.id] !=
								RegionIR::INVALID_VALUE)
						{
							if (!add_use(node.operands[0], use_position) ||
								!add_use(result.plan.vu0_sync_direct_raw[node.id],
									use_position))
							{
								return Fail(BuildFailure::InvalidValue, block_index,
									node.id);
							}
						continue;
					}
					if (node.opcode == RegionIR::Opcode::Vu0NormalizeVector &&
						node.id < result.plan.vu0_hoisted_normalize.size() &&
						result.plan.vu0_hoisted_normalize[node.id] != 0 &&
						(result.plan.value_word_demands[node.id] & 0x0f) == 0x0f)
					{
						continue;
					}
					if (node.opcode == RegionIR::Opcode::Cop1UpdateOuFlags &&
						node.id < result.plan.cop1_fused_clamp_for_flag.size() &&
						result.plan.cop1_fused_clamp_for_flag[node.id] !=
							RegionIR::INVALID_VALUE)
					{
						const RegionIR::ValueId clamp =
							result.plan.cop1_fused_clamp_for_flag[node.id];
						if (clamp >= ranges.size() || node.operand_count != 2 ||
							!add_use(node.operands[0], ranges[clamp].begin))
						{
							return Fail(BuildFailure::InvalidValue, block_index,
								node.id);
						}
						// Clamp's ordinary operand use already covers the shared raw
						// value at this exact emission position.
						continue;
					}
					for (u32 operand = 0; operand < node.operand_count; operand++)
				{
					if (!operand_is_used(node, operand))
						continue;
					RegionIR::ValueId value =
						node.opcode == RegionIR::Opcode::Cop1NormalizeInput &&
						operand == 0 &&
						result.plan.cop1_normalize_alias[node.id] !=
							RegionIR::INVALID_VALUE ?
							result.plan.cop1_normalize_alias[node.id] :
						node.opcode == RegionIR::Opcode::Vu0NormalizeVector &&
							operand == 0 &&
							result.plan.vu0_normalize_alias[node.id] !=
								RegionIR::INVALID_VALUE ?
							result.plan.vu0_normalize_alias[node.id] :
						node.opcode == RegionIR::Opcode::Vu0StatusFlagsFromMac &&
							operand == 0 &&
							result.plan.vu0_status_direct_raw[node.id] !=
								RegionIR::INVALID_VALUE ?
							result.plan.vu0_status_direct_raw[node.id] :
							node.operands[operand];
					const bool folded_vu0_broadcast =
						value < result.plan.vu0_folded_broadcast_source.size() &&
						result.plan.vu0_folded_broadcast_source[value] !=
							RegionIR::INVALID_VALUE;
					if (folded_vu0_broadcast)
						value = result.plan.vu0_folded_broadcast_source[value];
					if (definitions[value]->type == RegionIR::ValueType::MemoryEffect ||
						result.plan.value_word_demands[value] == 0)
					{
						continue;
					}
					if (!(folded_vu0_broadcast ?
						add_vu0_arithmetic_use(node.operands[operand], use_position) :
						add_use(value, use_position)))
						return Fail(BuildFailure::InvalidValue, block_index,
							value);
				}
			}
			if (block.terminator.kind == RegionIR::TerminatorKind::Branch &&
				!add_use(block.terminator.condition,
					block_not_taken_edge[block_index]))
			{
				return Fail(BuildFailure::InvalidValue, block_index,
					block.terminator.condition);
			}
		}

		for (const RegionExecution::ExitSite& site : execution.exits)
		{
			// RegionA32::CompileAllocated never emits the legacy source
			// backend's BlockEntry fallback. Treating that phantom site as a
			// block-edge observer pins every changed loop parameter through the
			// whole block and prevents its result from reusing the same register.
			if (site.kind == RegionExecution::ExitSiteKind::BlockEntry)
				continue;
			if (IsPreflightedMemoryExit(program, site,
					options.preflighted_memory_operations) ||
				IsForwardedMemoryExit(program, site,
					&result.plan.forwarded_memory_load_effect) ||
				IsNonExecutableInternalExit(program, site,
					options.aggregate_cycle_header_block))
				continue;
			RegionExecution::ExitContractView contract{};
			if (!RegionExecution::ResolveExitContract(program, site, &contract) ||
				!contract.state || site.block >= program.blocks.size())
			{
				return Fail(BuildFailure::InvalidExecutionPlan, site.block,
					RegionIR::INVALID_VALUE);
			}
			u32 use_position = site.kind == RegionExecution::ExitSiteKind::NotTaken ?
				block_not_taken_edge[site.block] : block_taken_edge[site.block];
			const RegionIR::Block& block = program.blocks[site.block];
			if (site.kind == RegionExecution::ExitSiteKind::EntryEvent ||
				site.kind == RegionExecution::ExitSiteKind::EntryFallback)
			{
				use_position = block_entry[site.block];
			}
			else if (site.kind == RegionExecution::ExitSiteKind::Memory)
			{
				if (site.ordinal >= block.memory_exits.size() ||
					block.memory_exits[site.ordinal].operation >= ranges.size())
				{
					return Fail(BuildFailure::InvalidExecutionPlan, site.block,
						RegionIR::INVALID_VALUE);
				}
				use_position = ranges[
					block.memory_exits[site.ordinal].operation].begin;
			}
			else if (site.kind == RegionExecution::ExitSiteKind::Observer)
			{
				if (site.ordinal >= block.observer_exits.size() ||
					block.observer_exits[site.ordinal].operation >= ranges.size())
				{
					return Fail(BuildFailure::InvalidExecutionPlan, site.block,
						RegionIR::INVALID_VALUE);
				}
				use_position = ranges[
					block.observer_exits[site.ordinal].operation].begin;
			}
			else if (site.kind == RegionExecution::ExitSiteKind::Guarded)
			{
				const auto guard = std::find_if(block.nodes.begin(), block.nodes.end(),
					[&](const RegionIR::Node& node) {
						return node.opcode == RegionIR::Opcode::ExitIfTrue &&
						       node.immediate == site.ordinal;
					});
				if (guard == block.nodes.end())
					return Fail(BuildFailure::InvalidExecutionPlan, site.block,
						RegionIR::INVALID_VALUE);
				use_position = ranges[guard->id].begin;
			}
			for (size_t slot = 0; slot < RegionExecution::STATE_SLOT_COUNT; slot++)
			{
				if (site.dirty_state.test(slot) &&
					!add_use(RegionExecution::StateValue(*contract.state, slot),
						use_position))
				{
					return Fail(BuildFailure::InvalidValue, site.block,
						RegionExecution::StateValue(*contract.state, slot));
				}
			}
			if (contract.transfer && !add_use(contract.transfer->pc, use_position))
				return Fail(BuildFailure::InvalidValue, site.block, contract.transfer->pc);
			if (contract.event_horizon_check &&
				!add_use(contract.state->cycle, use_position))
			{
				return Fail(BuildFailure::InvalidValue, site.block,
					contract.state->cycle);
			}
		}
		for (RegionIR::ValueId value = 0; value < program.value_count; value++)
		{
			if (result.plan.cop1_hoisted_normalize[value] == 0 &&
				result.plan.vu0_hoisted_normalize[value] == 0)
				continue;
			if (ranges[value].block != program.entry_block)
				return Fail(BuildFailure::InvalidValue, ranges[value].block, value);
			// Lowering moves this definition from its original instruction position
			// into the entry prefix. Its physical lifetime must move with it; merely
			// extending the end lets earlier body temporaries reuse and overwrite the
			// VFP register after the hoisted value has already been produced.
			ranges[value].begin = block_entry[program.entry_block];
			ranges[value].end = std::max(ranges[value].end,
				block_taken_edge[program.entry_block]);
			ranges[value].uses++;
		}

		std::vector<InternalEdge> edges;
		auto append_edge = [&](u32 block_index, const RegionIR::Transfer& transfer,
			u8 edge_index) {
			if (transfer.target_block != RegionIR::INVALID_BLOCK)
				edges.push_back({block_index, &transfer, edge_index, {}});
		};
		for (u32 block_index = 0; block_index < program.blocks.size(); block_index++)
		{
			const RegionIR::Block& block = program.blocks[block_index];
			(void)RegionIR::VisitInternalTransfers(block.terminator,
				[&](const RegionIR::Transfer& transfer, u8 edge_index) {
					append_edge(block_index, transfer, edge_index);
					return true;
				});
		}

		bool changed = true;
		while (changed)
		{
			changed = false;
			for (InternalEdge& edge : edges)
			{
				if (!edge.transfer || edge.transfer->target_block >= program.blocks.size())
					return Fail(BuildFailure::InvalidExecutionPlan, edge.source_block,
						RegionIR::INVALID_VALUE);
				const RegionIR::Block& target =
					program.blocks[edge.transfer->target_block];
				for (size_t slot = 0; slot < EDGE_STATE_SLOTS; slot++)
				{
					const RegionIR::ValueId parameter =
						BlockStateParameter(target, slot);
					// Identity nodes such as an entry-proven Vu0RequireIdle are
					// coalesced only after edge discovery.  Their architectural source
					// already has an exact component demand, but can still have zero
					// ordinary range uses here.  Treat that demand as a live phi input;
					// otherwise an invariant VU lane may be allocated over a destination
					// written later in the body and be lost on the first backedge.
					const bool state_parameter_demanded =
						slot < RegionExecution::STATE_SLOT_COUNT &&
						parameter < result.plan.value_word_demands.size() &&
						result.plan.value_word_demands[parameter] != 0;
					if (parameter >= ranges.size() ||
						(ranges[parameter].uses == 0 && !state_parameter_demanded) ||
						edge.propagated.test(slot))
					{
						continue;
					}
					const RegionIR::ValueId source =
						EdgeStateValue(edge.transfer->state, slot);
					const bool was_unused = source < ranges.size() &&
						ranges[source].uses == 0;
					const u32 edge_position = edge.edge_index == 1 ?
						block_not_taken_edge[edge.source_block] :
						block_taken_edge[edge.source_block];
					if (!add_use(source, edge_position))
						return Fail(BuildFailure::InvalidValue, edge.source_block, source);
					edge.propagated.set(slot);
					changed |= was_unused;
				}
			}
		}

		// A comparison used only by its branch is flags, not an SSA value which
		// deserves a register or spill slot. Its operands were deliberately kept
		// live through the block edge above. Record the fold in the immutable
		// allocation contract so every backend decision agrees with allocation;
		// any architectural/exit/ordinary use makes uses exceed one and retains
		// the materialized boolean path.
		for (u32 block_index = 0; block_index < program.blocks.size(); block_index++)
		{
			const RegionIR::Block& block = program.blocks[block_index];
			if (block.terminator.kind != RegionIR::TerminatorKind::Branch ||
				block.terminator.condition >= ranges.size())
			{
				continue;
			}
			const RegionIR::ValueId condition = block.terminator.condition;
			const RegionIR::Node* const definition = definitions[condition];
			if (!definition || !IsDirectBranchComparison(definition->opcode) ||
				ranges[condition].uses != 1)
			{
				continue;
			}
			Representation& representation =
				result.plan.value_representations[condition];
			if (representation.kind == RepresentationKind::CoreWords ||
				representation.kind == RepresentationKind::VfpWord ||
				representation.kind == RepresentationKind::FixedCycle)
			{
				result.plan.demanded_scalar_values--;
			}
			else if (representation.kind == RepresentationKind::NeonQ)
			{
				result.plan.demanded_full_vector_values--;
			}
			result.plan.folded_branch_conditions[condition] = 1;
			result.plan.value_word_demands[condition] = 0;
			representation = {};
			ranges[condition].uses = 0;
		}

		// Effective addresses are an instruction-selection value, not useful
		// architectural state. A single-use base+offset address should be formed in
		// one of the backend's reserved scratch registers at the memory operation.
		// Allocating it as ordinary SSA needlessly evicts a loop-carried GPR on the
		// six-register persistent ABI and turns pointer walks into stack traffic.
		for (const RegionIR::Block& block : program.blocks)
		{
			for (const RegionIR::Node& node : block.nodes)
			{
				if (node.opcode != RegionIR::Opcode::EffectiveAddress32 ||
					node.operand_count != 2 || ranges[node.id].uses != 1 ||
					result.plan.value_representations[node.id].kind !=
						RepresentationKind::CoreWords ||
					result.plan.value_representations[node.id].word_mask != 0x1)
				{
					continue;
				}
				bool memory_address_use = false;
				bool other_use = false;
				for (const RegionIR::Block& user_block : program.blocks)
				{
					for (const RegionIR::Node& user : user_block.nodes)
					{
						for (u32 operand = 0; operand < user.operand_count; operand++)
						{
							if (user.operands[operand] != node.id)
								continue;
							const bool address =
								(user.opcode == RegionIR::Opcode::MemoryLoad ||
								 user.opcode == RegionIR::Opcode::MemoryStore) &&
								operand == 1;
							memory_address_use |= address;
							other_use |= !address;
						}
					}
				}
				if (!memory_address_use || other_use)
					continue;
				result.plan.folded_effective_addresses[node.id] = 1;
				result.plan.value_word_demands[node.id] = 0;
				result.plan.value_representations[node.id] = {};
				ranges[node.id].uses = 0;
				if (result.plan.demanded_scalar_values == 0)
					return Fail(BuildFailure::InvalidValue, ranges[node.id].block, node.id);
				result.plan.demanded_scalar_values--;
			}
		}

		// Coalesce typed identity views before assigning physical storage. These
		// nodes remain in IR for verification, but extending the source interval
		// through every identity consumer lets immutable SSA values share one
		// location instead of generating stack-backed copy chains.
		std::vector<RegionIR::ValueId> alias_source(program.value_count,
			RegionIR::INVALID_VALUE);
		for (RegionIR::ValueId value = 0; value < program.value_count; value++)
		{
			if (result.plan.forwarded_memory_load_alias[value] !=
				RegionIR::INVALID_VALUE)
			{
				alias_source[value] =
					result.plan.forwarded_memory_load_alias[value];
			}
			else if (result.plan.cop1_guarded_clamp_alias[value] !=
				RegionIR::INVALID_VALUE)
			{
				alias_source[value] = result.plan.cop1_guarded_clamp_alias[value];
			}
			else if (result.plan.exact_i32_aliases[value] != RegionIR::INVALID_VALUE)
				alias_source[value] = result.plan.exact_i32_aliases[value];
			else if (result.plan.vu0_idle_alias[value] != RegionIR::INVALID_VALUE)
				alias_source[value] = result.plan.vu0_idle_alias[value];
			else if (result.plan.vu0_merge_alias[value] != RegionIR::INVALID_VALUE)
				alias_source[value] = result.plan.vu0_merge_alias[value];
			else if (result.plan.vu0_clamp_deferred_to_status[value] !=
				RegionIR::INVALID_VALUE)
			{
				const RegionIR::Node* const clamp = definitions[value];
				if (clamp && clamp->operand_count == 1)
					alias_source[value] = clamp->operands[0];
			}
			else if (result.plan.vu0_clamp_deferred_to_mac[value] !=
				RegionIR::INVALID_VALUE)
			{
				const RegionIR::Node* const clamp = definitions[value];
				if (clamp && clamp->operand_count == 1)
					alias_source[value] = clamp->operands[0];
			}
		}
		auto alias_root = [&](RegionIR::ValueId value) {
			for (u32 depth = 0; depth < program.value_count &&
				alias_source[value] != RegionIR::INVALID_VALUE; depth++)
			{
				value = alias_source[value];
			}
			return value;
		};
		auto is_zero_constant = [&](RegionIR::ValueId value) {
			if (value >= definitions.size() || !definitions[value])
				return false;
			const RegionIR::Node& node = *definitions[value];
			return IsConstant(node.opcode) && node.literal == 0;
		};
		for (const RegionIR::Block& block : program.blocks)
		{
			for (const RegionIR::Node& node : block.nodes)
			{
				const u8 demand = result.plan.value_word_demands[node.id];
				RegionIR::ValueId source = RegionIR::INVALID_VALUE;
				switch (node.opcode)
				{
					case RegionIR::Opcode::Cop1NormalizeInput:
						if (result.plan.cop1_normalize_alias[node.id] !=
							RegionIR::INVALID_VALUE)
						{
							source = result.plan.cop1_normalize_alias[node.id];
						}
						break;
					case RegionIR::Opcode::Vu0RequireIdle:
						if (result.plan.vu0_idle_alias[node.id] !=
							RegionIR::INVALID_VALUE)
						{
							source = result.plan.vu0_idle_alias[node.id];
						}
						break;
					case RegionIR::Opcode::Vu0NormalizeVector:
						if (result.plan.vu0_normalize_alias[node.id] !=
							RegionIR::INVALID_VALUE)
						{
							source = result.plan.vu0_normalize_alias[node.id];
						}
						break;
					case RegionIR::Opcode::Vu0MergeMasked:
						if (result.plan.vu0_merge_alias[node.id] !=
							RegionIR::INVALID_VALUE)
						{
							source = result.plan.vu0_merge_alias[node.id];
						}
						break;
					case RegionIR::Opcode::ExtractLow32:
					case RegionIR::Opcode::ExtractLow64:
					case RegionIR::Opcode::BitcastI32ToF32Bits:
					case RegionIR::Opcode::BitcastF32BitsToI32:
					case RegionIR::Opcode::AddressFromI32:
						if (node.operand_count == 1)
							source = node.operands[0];
						break;
					case RegionIR::Opcode::BitcastI128ToVuF32x4Bits:
					case RegionIR::Opcode::BitcastVuF32x4BitsToI128:
						if (node.operand_count == 1)
							source = node.operands[0];
						break;
					case RegionIR::Opcode::SignExtend32To64:
					case RegionIR::Opcode::ZeroExtend32To64:
						if (node.operand_count == 1 && demand == 0x1)
							source = node.operands[0];
						break;
					case RegionIR::Opcode::ReplaceLow64:
						if (node.operand_count == 2 && (demand & ~0x3u) == 0)
							source = node.operands[1];
						break;
					case RegionIR::Opcode::EffectiveAddress32:
						if (node.operand_count == 2)
						{
							if (is_zero_constant(node.operands[1]))
								source = node.operands[0];
							else if (is_zero_constant(node.operands[0]))
								source = node.operands[1];
						}
						break;
					default:
						break;
				}
				const RegionIR::ValueId root = source < program.value_count ?
					alias_root(source) : RegionIR::INVALID_VALUE;
				const bool cross_block_resident_cop1 =
					root < result.plan.cop1_region_resident_normalize.size() &&
					result.plan.cop1_region_resident_normalize[root] != 0;
				if (root < program.value_count && definitions[source] &&
					(result.plan.value_representations[root].word_mask &
					 result.plan.value_representations[node.id].word_mask) ==
						result.plan.value_representations[node.id].word_mask &&
					(ranges[source].block == ranges[node.id].block ||
					 cross_block_resident_cop1))
				{
					alias_source[node.id] = source;
				}
				}
			}

			// Propagate an immutable value through non-entry block parameters when every
			// internal predecessor supplies the same exact alias root. This is sparse
			// constant propagation over the verifier-owned SSA edges, not a control-flow
			// guess: external entry parameters remain arbitrary, and only Immediate roots
			// are admitted here. It lets JAL return addresses and other immutable state be
			// materialized only by a genuine cold observer instead of every loop lap.
			bool immutable_changed = true;
			while (immutable_changed)
			{
				immutable_changed = false;
				for (u32 target_block = 0; target_block < program.blocks.size();
					target_block++)
				{
					if (target_block == program.entry_block)
						continue;
					const RegionIR::Block& target = program.blocks[target_block];
					for (size_t slot = 1; slot < EDGE_STATE_SLOTS; slot++)
					{
						const RegionIR::ValueId parameter =
							BlockStateParameter(target, slot);
						if (parameter >= ranges.size() || ranges[parameter].uses == 0 ||
							alias_source[parameter] != RegionIR::INVALID_VALUE)
						{
							continue;
						}
						RegionIR::ValueId unanimous = RegionIR::INVALID_VALUE;
						bool have_incoming = false;
						bool agree = true;
						for (const InternalEdge& edge : edges)
						{
							if (!edge.transfer || edge.transfer->target_block != target_block ||
								!edge.propagated.test(slot))
							{
								continue;
							}
							const RegionIR::ValueId source = alias_root(
								EdgeStateValue(edge.transfer->state, slot));
							if (!have_incoming)
							{
								unanimous = source;
								have_incoming = true;
							}
							else if (source != unanimous)
							{
								agree = false;
								break;
							}
						}
						if (!have_incoming || !agree || unanimous >= program.value_count ||
							result.plan.value_representations[unanimous].kind !=
								RepresentationKind::Immediate ||
							(result.plan.value_representations[unanimous].word_mask &
							 result.plan.value_representations[parameter].word_mask) !=
								result.plan.value_representations[parameter].word_mask)
						{
							continue;
						}
						alias_source[parameter] = unanimous;
						immutable_changed = true;
					}
				}
			}

			result.plan.value_aliases.resize(program.value_count);
			for (RegionIR::ValueId value = 0; value < program.value_count; value++)
				result.plan.value_aliases[value] = alias_root(value);
			for (RegionIR::ValueId value = 0; value < program.value_count; value++)
		{
			if (alias_source[value] == RegionIR::INVALID_VALUE ||
				ranges[value].uses == 0)
			{
				continue;
			}
			const RegionIR::ValueId root = alias_root(value);
			if (root >= program.value_count || root == value)
				return Fail(BuildFailure::InvalidValue, ranges[value].block, value);
			ranges[root].end = std::max(ranges[root].end, ranges[value].end);
			ranges[root].uses += ranges[value].uses;
			ranges[value].uses = 0;
		}

		// Exact VU0 macro arithmetic observes scalar VFP rounding. Some target
		// partitions expose only a prefix of the logical Q bank as scalar S lanes,
		// while every Q remains valid for bitwise and normalization work. Derive a
		// cold coloring preference from verified IR def-use roles so scarce scalar-
		// addressable colors go to raw arithmetic values. This is never a pin:
		// interference and phi ownership remain authoritative, and the backend keeps
		// its exact lane-transfer fallback when the preferred coloring is impossible.
		std::vector<u16> neon_scalar_priority(program.value_count, 0);
		auto add_neon_scalar_priority = [&](RegionIR::ValueId value,
			u16 weight) {
			if (value < result.plan.vu0_folded_broadcast_source.size() &&
				result.plan.vu0_folded_broadcast_source[value] !=
					RegionIR::INVALID_VALUE)
			{
				value = result.plan.vu0_folded_broadcast_source[value];
			}
			if (value >= program.value_count)
				return;
			value = alias_root(value);
			if (value >= program.value_count || ranges[value].uses == 0 ||
				result.plan.value_representations[value].kind !=
					RepresentationKind::NeonQ)
			{
				return;
			}
			neon_scalar_priority[value] = static_cast<u16>(std::min<u32>(
				UINT16_MAX, neon_scalar_priority[value] + weight));
		};
		for (const RegionIR::Block& block : program.blocks)
		{
			for (const RegionIR::Node& node : block.nodes)
			{
				if (node.opcode == RegionIR::Opcode::Vu0AddRaw &&
					node.id < result.plan.vu0_add_fused_mul.size() &&
					result.plan.vu0_add_fused_mul[node.id] !=
						RegionIR::INVALID_VALUE)
				{
					const RegionIR::ValueId multiply_id =
						result.plan.vu0_add_fused_mul[node.id];
					const RegionIR::Node* const multiply =
						multiply_id < definitions.size() ? definitions[multiply_id] : nullptr;
					if (!multiply || multiply->operand_count != 2 ||
						node.operand_count != 2)
					{
						return Fail(BuildFailure::InvalidValue,
							ranges[node.id].block, node.id);
					}
					const RegionIR::ValueId addend =
						node.operands[0] == multiply_id ?
							node.operands[1] : node.operands[0];
					add_neon_scalar_priority(node.id, 16);
					add_neon_scalar_priority(addend, 16);
					add_neon_scalar_priority(multiply->operands[0], 16);
					add_neon_scalar_priority(multiply->operands[1], 16);
					continue;
				}
				if (node.opcode == RegionIR::Opcode::Vu0MulRaw ||
					node.opcode == RegionIR::Opcode::Vu0AddRaw ||
					node.opcode == RegionIR::Opcode::Vu0SubRaw)
				{
					add_neon_scalar_priority(node.id, 8);
					for (u32 operand = 0; operand < node.operand_count; operand++)
						add_neon_scalar_priority(node.operands[operand], 8);
				}
				else if (node.opcode == RegionIR::Opcode::Vu0ConvertFixed ||
					node.opcode == RegionIR::Opcode::Vu0ConvertIntegerToFloat)
				{
					add_neon_scalar_priority(node.id, 4);
					if (node.operand_count == 1)
						add_neon_scalar_priority(node.operands[0], 4);
				}
			}
		}

		// Rematerialization is profitable under actual scalar pressure, not as a
		// blanket opcode rewrite. First account for the identity aliases above, then
		// enable derivable high words only in blocks whose simultaneous physical
		// CoreWords demand fills the available bank.  At exact saturation the
		// derived word prevents any loop-invariant scalar from owning a register for
		// the complete region, even though rebuilding that word at its real observer
		// is cheaper than reloading the invariant on every iteration. Low-pressure
		// loops retain a resident high word and pay no repeated ASR/MOV cost.
		std::vector<u32> core_pressure(program.blocks.size(), 0);
		for (u32 block_index = 0; block_index < program.blocks.size(); block_index++)
		{
			const u32 first = block_entry[block_index];
			const u32 last = std::max(block_taken_edge[block_index],
				block_not_taken_edge[block_index]);
			for (u32 live_position = first; live_position < last; live_position++)
			{
				u32 words = 0;
				for (RegionIR::ValueId value = 0; value < program.value_count; value++)
				{
					const Range& range = ranges[value];
					const Representation& representation =
						result.plan.value_representations[value];
					if (range.uses != 0 && range.block == block_index &&
						representation.kind == RepresentationKind::CoreWords &&
						range.begin <= live_position && live_position < range.end)
					{
						words += representation.words;
					}
				}
				core_pressure[block_index] = std::max(
					core_pressure[block_index], words);
			}
		}
		// Record only the components physically required by an internal edge. A
		// derivable high word may remain absent when the target parameter carries
		// only the low word; banning rematerialization merely because the state slot
		// crosses a backedge turns every sign-extending pointer update into two host
		// registers. If the target really demands the high word, the bit remains a
		// barrier and the ordinary edge-copy contract is preserved.
		std::vector<u8> rematerialization_edge_barrier(program.value_count, 0);
		std::vector<RegionIR::ValueId> extension_parent(program.value_count);
		for (RegionIR::ValueId value = 0; value < program.value_count; value++)
			extension_parent[value] = value;
		auto extension_root = [&](RegionIR::ValueId value) {
			RegionIR::ValueId root = value;
			while (extension_parent[root] != root)
				root = extension_parent[root];
			while (extension_parent[value] != value)
			{
				const RegionIR::ValueId next = extension_parent[value];
				extension_parent[value] = root;
				value = next;
			}
			return root;
		};
		auto join_extension = [&](RegionIR::ValueId left,
			RegionIR::ValueId right) {
			left = extension_root(left);
			right = extension_root(right);
			if (left != right)
				extension_parent[right] = left;
		};
		for (const InternalEdge& edge : edges)
		{
			if (!edge.transfer)
				continue;
			for (size_t slot = 0; slot < EDGE_STATE_SLOTS; slot++)
			{
				if (!edge.propagated.test(slot))
					continue;
				const RegionIR::ValueId source =
					EdgeStateValue(edge.transfer->state, slot);
				const RegionIR::Block& target =
					program.blocks[edge.transfer->target_block];
				const RegionIR::ValueId parameter =
					BlockStateParameter(target, slot);
				if (source >= program.value_count || parameter >= program.value_count)
					continue;
				rematerialization_edge_barrier[alias_root(source)] |=
					result.plan.value_representations[parameter].word_mask;
				const RegionIR::ValueId source_root = alias_root(source);
				const RegionIR::ValueId parameter_root = alias_root(parameter);
				if (source_root < program.value_count &&
					parameter_root < program.value_count &&
					(result.plan.value_representations[parameter].word_mask & 0x2) != 0)
				{
					join_extension(source_root, parameter_root);
				}
			}
		}
		for (RegionIR::ValueId value = 0; value < program.value_count; value++)
		{
			if (rematerialization_candidates[value] ==
					RematerializationKind::None ||
				ranges[value].block >= core_pressure.size())
			{
				continue;
			}
			const RegionIR::ValueId root = alias_root(value);
			if (root >= program.value_count ||
				(rematerialization_edge_barrier[root] & 0x2) != 0 ||
				core_pressure[ranges[root].block] < allocatable_core_register_words)
			{
				continue;
			}
			result.plan.value_rematerializations[value] =
				rematerialization_candidates[value];
			result.plan.value_rematerialized_word_masks[value] = 0x2;
			Representation& representation =
			result.plan.value_representations[value];
			representation.word_mask &= static_cast<u8>(~0x2u);
			representation.word_mask |= 0x1;
			representation.words = CountWords(representation.word_mask);
		}

		auto semantic_extension = [&](RegionIR::ValueId value) {
			if (value >= execution.value_low32_extensions.size())
				return RematerializationKind::None;
			switch (execution.value_low32_extensions[value])
			{
				case RegionExecution::Low32Extension::Sign:
					return RematerializationKind::SignExtendLow32;
				case RegionExecution::Low32Extension::Zero:
					return RematerializationKind::ZeroExtendLow32;
				case RegionExecution::Low32Extension::None:
					return RematerializationKind::None;
			}
			return RematerializationKind::None;
		};

		// An unchanged architectural parameter cannot acquire an unconditional
		// extension fact from its own backedge. It can, however, use an exact entry
		// guard when a 64-bit comparison already has an extended peer. This is a
		// consumer-derived representation choice, not a semantic assumption: every
		// storage value in the web must remain the same architectural parameter, and
		// incompatible entry state takes the untouched tier-zero path.
		std::vector<s32> guarded_invariant_slots(program.value_count, -2);
		for (RegionIR::ValueId value = 0; value < program.value_count; value++)
		{
			if (alias_root(value) != value || ranges[value].uses == 0 ||
				(result.plan.value_representations[value].word_mask & 0x2) == 0)
			{
				continue;
			}
			const RegionIR::ValueId root = extension_root(value);
			if (root >= guarded_invariant_slots.size())
				continue;
			const RegionIR::Node* const node = definitions[value];
			const s32 slot = node && node->opcode == RegionIR::Opcode::Parameter &&
				node->type != RegionIR::ValueType::Cycle &&
				node->immediate < RegionExecution::STATE_SLOT_COUNT ?
				static_cast<s32>(node->immediate) : -1;
			s32& group_slot = guarded_invariant_slots[root];
			if (group_slot == -2)
				group_slot = slot;
			else if (group_slot != slot)
				group_slot = -1;
		}
		std::vector<RematerializationKind> inferred_extensions(program.value_count,
			RematerializationKind::None);
		std::vector<u8> conflicting_inferred_extensions(program.value_count, 0);
		auto request_inferred_extension = [&](RegionIR::ValueId value,
			RematerializationKind extension) {
			if (value >= program.value_count || extension == RematerializationKind::None)
				return;
			const RegionIR::ValueId root = extension_root(alias_root(value));
			if (root >= guarded_invariant_slots.size() ||
				guarded_invariant_slots[root] < 0 ||
				semantic_extension(value) != RematerializationKind::None)
			{
				return;
			}
			if (inferred_extensions[root] == RematerializationKind::None)
				inferred_extensions[root] = extension;
			else if (inferred_extensions[root] != extension)
				conflicting_inferred_extensions[root] = 1;
		};
		for (const RegionIR::Block& block : program.blocks)
		{
			for (const RegionIR::Node& node : block.nodes)
			{
				if ((node.opcode != RegionIR::Opcode::CompareEqual64 &&
					 node.opcode != RegionIR::Opcode::CompareNotEqual64 &&
					 node.opcode != RegionIR::Opcode::CompareSignedLess64 &&
					 node.opcode != RegionIR::Opcode::CompareUnsignedLess64) ||
					node.operand_count != 2)
				{
					continue;
				}
				request_inferred_extension(node.operands[0],
					semantic_extension(node.operands[1]));
				request_inferred_extension(node.operands[1],
					semantic_extension(node.operands[0]));
			}
		}

		// A backedge-carried scalar may become one physical low word only as a
		// complete phi web. RegionExecution supplies unconditional facts; the
		// guarded invariant proof above may supply a consumer-compatible fact for an
		// otherwise arbitrary entry parameter. Select a web only at full core
		// pressure and reject every conflicting or non-GPR predicate.
		struct ExtensionGroup
		{
			RematerializationKind extension = RematerializationKind::None;
			RegionExecution::StateMask guards{};
			bool valid = true;
			bool pressured = false;
			bool has_edge = false;
		};
		std::vector<ExtensionGroup> extension_groups(program.value_count);
		for (const InternalEdge& edge : edges)
		{
			if (!edge.transfer || edge.transfer->target_block >= program.blocks.size())
				continue;
			const RegionIR::Block& target =
				program.blocks[edge.transfer->target_block];
			for (size_t slot = 0; slot < EDGE_STATE_SLOTS; slot++)
			{
				if (!edge.propagated.test(slot))
					continue;
				const RegionIR::ValueId source = alias_root(
					EdgeStateValue(edge.transfer->state, slot));
				const RegionIR::ValueId parameter = alias_root(
					BlockStateParameter(target, slot));
				if (source < program.value_count && parameter < program.value_count &&
					extension_root(source) == extension_root(parameter) &&
					(result.plan.value_representations[parameter].word_mask & 0x2) != 0)
				{
					extension_groups[extension_root(source)].has_edge = true;
				}
			}
		}
		for (RegionIR::ValueId value = 0; value < program.value_count; value++)
		{
			const RegionIR::ValueId root = extension_root(alias_root(value));
			if (root >= extension_groups.size() ||
				(result.plan.value_representations[value].word_mask & 0x2) == 0)
			{
				continue;
			}
			ExtensionGroup& group = extension_groups[root];
			RematerializationKind extension = semantic_extension(value);
			if (extension == RematerializationKind::None &&
				guarded_invariant_slots[root] >= 0 &&
				conflicting_inferred_extensions[root] == 0)
			{
				extension = inferred_extensions[root];
				if (extension != RematerializationKind::None)
					group.guards.set(static_cast<size_t>(guarded_invariant_slots[root]));
			}
			if (extension == RematerializationKind::None ||
				(group.extension != RematerializationKind::None &&
				 group.extension != extension))
			{
				group.valid = false;
			}
			else
			{
				group.extension = extension;
				group.guards |= execution.value_low32_entry_guards[value];
			}
			const RegionIR::ValueId storage = alias_root(value);
			if (storage < ranges.size() && ranges[storage].block < core_pressure.size())
			{
				group.pressured |= core_pressure[ranges[storage].block] >=
					allocatable_core_register_words;
			}
		}
		for (RegionIR::ValueId root = 0;
			options.enable_guarded_entry_low32 && root < extension_groups.size(); root++)
		{
			ExtensionGroup& group = extension_groups[root];
			if (!group.valid || !group.pressured || !group.has_edge ||
				group.extension == RematerializationKind::None)
			{
				continue;
			}
			std::vector<EntryLow32Guard> selected_guards;
			bool guards_valid = true;
			for (size_t slot = 0; slot < RegionExecution::STATE_SLOT_COUNT; slot++)
			{
				if (!group.guards.test(slot))
					continue;
				const RegionExecution::StateSlot decoded =
					RegionExecution::DecodeStateSlot(slot);
				const auto candidate = std::find_if(
					execution.entry_low32_guard_candidates.begin(),
					execution.entry_low32_guard_candidates.end(),
					[&](const RegionExecution::EntryLow32GuardCandidate& item) {
						return item.state_slot == slot;
					});
				RegionIR::ValueId guard_parameter = RegionIR::INVALID_VALUE;
				RematerializationKind guard_extension = RematerializationKind::None;
				if (candidate != execution.entry_low32_guard_candidates.end())
				{
					guard_parameter = candidate->parameter;
					guard_extension = semantic_extension(candidate->parameter);
				}
				else if (decoded.state_class == RegionExecution::StateClass::Gpr &&
					guarded_invariant_slots[root] == static_cast<s32>(slot) &&
					program.entry_block < program.blocks.size())
				{
					guard_parameter = BlockStateParameter(
						program.blocks[program.entry_block], slot);
					if (guard_parameter < program.value_count &&
						extension_root(alias_root(guard_parameter)) == root)
					{
						guard_extension = inferred_extensions[root];
					}
				}
				if (decoded.state_class != RegionExecution::StateClass::Gpr ||
					guard_parameter >= program.value_count ||
					guard_extension != group.extension)
				{
					guards_valid = false;
					break;
				}
				selected_guards.push_back({guard_parameter,
					static_cast<u16>(slot), group.extension});
			}
			if (!guards_valid)
				continue;
			for (RegionIR::ValueId value = 0; value < program.value_count; value++)
			{
				if (extension_root(alias_root(value)) != root ||
					(result.plan.value_representations[value].word_mask & 0x2) == 0)
				{
					continue;
				}
				result.plan.value_low32_extensions[value] = group.extension;
				result.plan.value_rematerializations[value] = group.extension;
				result.plan.value_rematerialized_word_masks[value] |= 0x2;
				Representation& representation =
					result.plan.value_representations[value];
				representation.word_mask &= static_cast<u8>(~0x2u);
				representation.word_mask |= 0x1;
				representation.words = CountWords(representation.word_mask);
			}
			for (const EntryLow32Guard& guard : selected_guards)
			{
				const auto duplicate = std::find_if(
					result.plan.entry_low32_guards.begin(),
					result.plan.entry_low32_guards.end(),
					[&](const EntryLow32Guard& existing) {
						return existing.state_slot == guard.state_slot;
					});
				if (duplicate == result.plan.entry_low32_guards.end())
					result.plan.entry_low32_guards.push_back(guard);
				else if (duplicate->extension != guard.extension)
					return Fail(BuildFailure::InvalidExecutionPlan,
						program.entry_block, guard.parameter);
			}
		}

		// Alias the physical low word of an extension after pressure-selected
		// rematerialization has removed its high word.  The first identity pass runs
		// before that decision and therefore correctly refuses a two-word extension;
		// once word one is a verifier-proven cold rematerialization, emitting a MOV for
		// word zero on every loop iteration serves no semantic purpose.  Transfer the
		// complete live range to the immutable low-word source so cold exits can still
		// reconstruct the architectural high word from this value's retained extension
		// contract.  This is typed SSA identity, independent of source opcode address or
		// workload shape.
		for (RegionIR::ValueId value = 0; value < program.value_count; value++)
		{
			const RegionIR::Node* const node = definitions[value];
			if (!node ||
				(node->opcode != RegionIR::Opcode::SignExtend32To64 &&
				 node->opcode != RegionIR::Opcode::ZeroExtend32To64) ||
				node->operand_count != 1 || alias_source[value] !=
					RegionIR::INVALID_VALUE || ranges[value].uses == 0 ||
				(result.plan.value_rematerialized_word_masks[value] & 0x2) == 0)
			{
				continue;
			}
			const RegionIR::ValueId source = alias_root(node->operands[0]);
			if (source >= program.value_count || source == value ||
				ranges[source].block != ranges[value].block ||
				result.plan.value_representations[value].kind !=
					RepresentationKind::CoreWords ||
				result.plan.value_representations[value].word_mask != 0x1 ||
				(result.plan.value_representations[source].word_mask & 0x1) == 0)
			{
				continue;
			}
			alias_source[value] = source;
			ranges[source].end = std::max(ranges[source].end, ranges[value].end);
			ranges[source].uses += ranges[value].uses;
			ranges[value].uses = 0;
		}
		for (RegionIR::ValueId value = 0; value < program.value_count; value++)
			result.plan.value_aliases[value] = alias_root(value);

		// A parameter which is unchanged on every incoming edge can be rematerialized
		// from canonical state. Do that only for a genuinely single-use architectural
		// input. Reloading the same invariant in several blocks defeats the region's
		// state-ownership contract on Cortex-A9; repeated users instead form one
		// ordinary phi web and the allocator keeps the value resident when pressure
		// permits. Canonical state remains untouched until a cold exit, and the
		// compatible entry publishes every dirty tier-zero mapping before entering
		// the common body. Keep this conservative across joins: if either side of a
		// propagated state edge cannot use the same canonical slot, both sides remain
		// ordinary allocated values.
		std::array<u32, RegionExecution::STATE_SLOT_COUNT> canonical_slot_uses{};
		auto count_canonical_parameter_use = [&](RegionIR::ValueId value) {
			if (value >= definitions.size())
				return;
			const RegionIR::Node* const parameter = definitions[value];
			if (parameter && parameter->opcode == RegionIR::Opcode::Parameter &&
				parameter->type != RegionIR::ValueType::Cycle &&
				parameter->immediate < RegionExecution::STATE_SLOT_COUNT)
			{
				canonical_slot_uses[parameter->immediate]++;
			}
		};
		for (const RegionIR::Block& block : program.blocks)
		{
			for (const RegionIR::Node& node : block.nodes)
			{
				const bool intrinsic = node.opcode == RegionIR::Opcode::MemoryLoad ||
					node.opcode == RegionIR::Opcode::MemoryStore ||
					node.opcode == RegionIR::Opcode::ExitIfTrue ||
					node.opcode == RegionIR::Opcode::Cop1ExceptionalOuResult ||
					node.opcode == RegionIR::Opcode::Vu0RequireIdle;
				if (IsBinding(node.opcode) || node.opcode == RegionIR::Opcode::NoEffect ||
					(!intrinsic && result.plan.value_word_demands[node.id] == 0))
				{
					continue;
				}
				for (u32 operand = 0; operand < node.operand_count; operand++)
				{
					if (operand_is_used(node, operand))
						count_canonical_parameter_use(node.operands[operand]);
				}
			}
			count_canonical_parameter_use(block.terminator.condition);
		}
		std::vector<u8> canonical_parameters(program.value_count, 0);
		for (RegionIR::ValueId value = 0; value < program.value_count; value++)
		{
			const RegionIR::Node* const node = definitions[value];
			const u8 demand = result.plan.value_word_demands[value];
			if (node && node->opcode == RegionIR::Opcode::Parameter &&
				node->type != RegionIR::ValueType::Cycle &&
				result.plan.value_representations[value].kind ==
					RepresentationKind::CoreWords &&
				node->immediate < RegionExecution::STATE_SLOT_COUNT && demand != 0 &&
				(execution.value_canonical_parameter_words[value] & demand) == demand &&
				canonical_slot_uses[node->immediate] <= 1)
			{
				canonical_parameters[value] = 1;
			}
		}
		bool canonical_changed = true;
		while (canonical_changed)
		{
			canonical_changed = false;
			for (const InternalEdge& edge : edges)
			{
				if (!edge.transfer ||
					edge.transfer->target_block >= program.blocks.size())
					continue;
				const RegionIR::Block& target =
					program.blocks[edge.transfer->target_block];
				for (size_t slot = 0; slot < EDGE_STATE_SLOTS; slot++)
				{
					if (!edge.propagated.test(slot))
						continue;
					const RegionIR::ValueId source =
						EdgeStateValue(edge.transfer->state, slot);
					const RegionIR::ValueId parameter =
						BlockStateParameter(target, slot);
					if (source >= canonical_parameters.size() ||
						parameter >= canonical_parameters.size())
					{
						return Fail(BuildFailure::InvalidValue, edge.source_block,
							source);
					}
					const bool same_canonical = canonical_parameters[source] != 0 &&
						canonical_parameters[parameter] != 0 &&
						definitions[source]->immediate == definitions[parameter]->immediate;
					if (same_canonical ||
						(canonical_parameters[source] == 0 &&
						 canonical_parameters[parameter] == 0))
					{
						continue;
					}
					if (canonical_parameters[source] != 0)
					{
						canonical_parameters[source] = 0;
						canonical_changed = true;
					}
					if (canonical_parameters[parameter] != 0)
					{
						canonical_parameters[parameter] = 0;
						canonical_changed = true;
					}
				}
			}
		}

		result.plan.value_locations.resize(program.value_count);
		std::vector<u8> resident_core_index(program.value_count, UINT8_MAX);
		std::vector<u8> resident_vfp_index(program.value_count, UINT8_MAX);
		if (options.region_resident_core_values)
		{
			for (size_t index = 0; index < resident_core_values; index++)
			{
				const RegionIR::ValueId value =
					(*options.region_resident_core_values)[index];
				if (value >= program.value_count || alias_root(value) != value ||
					resident_core_index[value] != UINT8_MAX)
				{
					return Fail(BuildFailure::InvalidValue, program.entry_block, value);
				}
				resident_core_index[value] = static_cast<u8>(
					allocatable_core_register_words + index);
			}
		}
		u32 next_resident_vfp = allocatable_vfp_s_registers;
		for (RegionIR::ValueId value = 0; value < program.value_count; value++)
		{
			if (result.plan.cop1_region_resident_normalize[value] == 0)
				continue;
			if (next_resident_vfp >= available_vfp_s_registers)
				return Fail(BuildFailure::InvalidValue, program.entry_block, value);
			resident_vfp_index[value] = static_cast<u8>(next_resident_vfp++);
		}
		std::vector<size_t> interval_for_value(program.value_count, SIZE_MAX);
		for (RegionIR::ValueId value = 0; value < program.value_count; value++)
		{
			if (!definitions[value])
				return Fail(BuildFailure::InvalidValue, RegionIR::INVALID_BLOCK, value);
			const RegionIR::Node& node = *definitions[value];
			if (ranges[value].uses == 0 || node.type == RegionIR::ValueType::Void ||
				node.type == RegionIR::ValueType::MemoryEffect)
			{
				continue;
			}
			Interval interval{value, node.type, ranges[value].block,
				ranges[value].begin, ranges[value].end, ranges[value].uses,
				backend_exit_word_demands[value], {}};
			const Representation& representation =
				result.plan.value_representations[value];
			if (representation.kind == RepresentationKind::None ||
				representation.words == 0)
			{
				return Fail(BuildFailure::InvalidValue, ranges[value].block, value);
			}
			if (resident_core_index[value] != UINT8_MAX)
			{
				if (representation.kind != RepresentationKind::CoreWords ||
					representation.words != 1 || representation.word_mask != 0x1)
				{
					return Fail(BuildFailure::InvalidValue, ranges[value].block, value);
				}
				interval.location = {LocationKind::Core,
					resident_core_index[value], 1, 0x1};
			}
			else if (resident_vfp_index[value] != UINT8_MAX)
			{
				if (representation.kind != RepresentationKind::VfpWord ||
					representation.words != 1 || representation.word_mask != 0x1)
				{
					return Fail(BuildFailure::InvalidValue, program.entry_block, value);
				}
				interval.location = {LocationKind::VfpS,
					resident_vfp_index[value], 1, 0x1};
			}
			else if (IsConstant(node.opcode) ||
				representation.kind == RepresentationKind::Immediate)
				interval.location = {LocationKind::Immediate, 0,
					representation.words, representation.word_mask};
			else if (canonical_parameters[value] != 0)
				interval.location = {LocationKind::CanonicalState,
					static_cast<u16>(node.immediate), representation.words,
					representation.word_mask};
			else if (node.type == RegionIR::ValueType::Cycle)
				interval.location = {LocationKind::FixedCycle, 0, 2, 0x3};
			interval_for_value[value] = result.plan.intervals.size();
			result.plan.intervals.push_back(interval);
			result.plan.live_values++;
		}

		auto interval_for = [&](RegionIR::ValueId value) -> Interval* {
			return value < interval_for_value.size() &&
				interval_for_value[value] != SIZE_MAX ?
					&result.plan.intervals[interval_for_value[value]] : nullptr;
		};

		// Derive register affinities for verifier-owned phi webs before allocating
		// individual blocks. An affinity lets the source and target of a hot internal
		// edge converge on one location, but it is not a pin: a conflicting local value
		// may take the register and leave one explicit edge copy. Hard-precoloring the
		// web made an extra available register non-monotonic by forcing dozens of body
		// values to spill merely to remove one backedge move.
		std::vector<size_t> phi_parent(result.plan.intervals.size());
		struct PhiAffinity
		{
			size_t source = SIZE_MAX;
			size_t target = SIZE_MAX;
			u32 source_block = RegionIR::INVALID_BLOCK;
			u32 target_block = RegionIR::INVALID_BLOCK;
			u8 seam_priority = 0;
			u64 weight = 0;
		};
		std::vector<PhiAffinity> phi_affinities;
		for (size_t index = 0; index < phi_parent.size(); index++)
			phi_parent[index] = index;
		auto phi_root = [&](size_t index) {
			size_t root = index;
			while (phi_parent[root] != root)
				root = phi_parent[root];
			while (phi_parent[index] != index)
			{
				const size_t next = phi_parent[index];
				phi_parent[index] = root;
				index = next;
			}
			return root;
		};
		auto join_phi = [&](size_t left, size_t right) {
			left = phi_root(left);
			right = phi_root(right);
			if (left != right)
				phi_parent[right] = left;
		};
		for (const InternalEdge& edge : edges)
		{
			if (!edge.transfer || edge.transfer->target_block >= program.blocks.size())
				continue;
			const RegionIR::Block& target =
				program.blocks[edge.transfer->target_block];
			for (size_t slot = 1; slot < EDGE_STATE_SLOTS; slot++)
			{
				if (!edge.propagated.test(slot))
					continue;
				const RegionIR::ValueId source_value =
					EdgeStateValue(edge.transfer->state, slot);
				const RegionIR::ValueId source_root = source_value < program.value_count ?
					alias_root(source_value) : RegionIR::INVALID_VALUE;
				const RegionIR::ValueId parameter =
					BlockStateParameter(target, slot);
				if (source_root >= interval_for_value.size() ||
					parameter >= interval_for_value.size())
				{
					continue;
				}
				const size_t source_index = interval_for_value[source_root];
				const size_t target_index = interval_for_value[parameter];
				if (source_index == SIZE_MAX || target_index == SIZE_MAX)
					continue;
				const Representation& source_representation =
					result.plan.value_representations[source_root];
				const Representation& target_representation =
					result.plan.value_representations[parameter];
				if (result.plan.intervals[source_index].location.kind ==
					LocationKind::CanonicalState ||
				result.plan.intervals[target_index].location.kind ==
					LocationKind::CanonicalState)
				{
					continue;
				}
				const bool register_resident =
					source_representation.kind == RepresentationKind::CoreWords ||
					source_representation.kind == RepresentationKind::VfpWord ||
					source_representation.kind == RepresentationKind::NeonQ;
					const bool compatible_core_prefix =
						source_representation.kind == RepresentationKind::CoreWords &&
						target_representation.kind == RepresentationKind::CoreWords &&
						IsLowPrefixMask(source_representation.word_mask) &&
						IsLowPrefixMask(target_representation.word_mask) &&
						(source_representation.word_mask &
							target_representation.word_mask) ==
							target_representation.word_mask;
					const bool compatible_exact =
						target_representation.kind == source_representation.kind &&
						source_representation.words == target_representation.words &&
						source_representation.word_mask == target_representation.word_mask;
					if (!register_resident || (!compatible_exact && !compatible_core_prefix))
					{
						continue;
					}
				u8 seam_priority =
					edge.transfer->target_block <= edge.source_block ? 1u : 0u;
				const RegionIR::Block& source_block =
					program.blocks[edge.source_block];
				for (const RegionIR::DirectCallContract& call : program.direct_calls)
				{
					if (source_block.terminator.branch_pc == call.call_pc &&
						target.pc == call.callee_pc)
					{
						seam_priority = std::max<u8>(seam_priority, 2u);
					}
					if (source_block.terminator.branch_pc == call.return_jump_pc &&
						target.pc == call.return_pc)
					{
						seam_priority = std::max<u8>(seam_priority, 3u);
					}
				}
				phi_affinities.push_back({source_index, target_index,
					edge.source_block, edge.transfer->target_block, seam_priority,
					static_cast<u64>(result.plan.intervals[source_index].uses) +
						result.plan.intervals[target_index].uses});
				join_phi(source_index, target_index);
			}
		}

		std::vector<std::vector<size_t>> phi_webs(result.plan.intervals.size());
		for (size_t index = 0; index < result.plan.intervals.size(); index++)
			phi_webs[phi_root(index)].push_back(index);
		std::sort(phi_webs.begin(), phi_webs.end(),
			[&](const std::vector<size_t>& left,
				const std::vector<size_t>& right) {
				auto weight = [&](const std::vector<size_t>& web) {
					u64 uses = 0;
					for (const size_t index : web)
						uses += result.plan.intervals[index].uses;
					return uses;
				};
				return left.size() != right.size() ?
					left.size() > right.size() : weight(left) > weight(right);
			});
		std::vector<Location> phi_preferences(result.plan.intervals.size());
		std::vector<size_t> phi_spill_affinity(result.plan.intervals.size(),
			SIZE_MAX);
		for (const std::vector<size_t>& web : phi_webs)
		{
			if (web.size() < 2)
				continue;
			const Interval& first_interval = result.plan.intervals[web.front()];
			const Representation& representation =
				result.plan.value_representations[first_interval.value];
			LocationKind location_kind = LocationKind::None;
			u32 register_count = 0;
			u8 location_words = representation.words;
			switch (representation.kind)
			{
				case RepresentationKind::CoreWords:
					location_kind = LocationKind::Core;
					register_count = allocatable_core_register_words;
					break;
				case RepresentationKind::VfpWord:
					location_kind = LocationKind::VfpS;
					register_count = allocatable_vfp_s_registers;
					location_words = 1;
					break;
				case RepresentationKind::NeonQ:
					location_kind = LocationKind::NeonQ;
					register_count = options.neon_q_registers;
					location_words = 4;
					break;
				case RepresentationKind::None:
				case RepresentationKind::Immediate:
				case RepresentationKind::FixedCycle:
					break;
			}
			u32 register_width =
				representation.kind == RepresentationKind::CoreWords ?
					representation.words : 1u;
			if (location_kind == LocationKind::None || register_width == 0 ||
				register_width > register_count)
			{
				continue;
			}
			bool compatible = true;
			for (const size_t index : web)
			{
				const Representation& member = result.plan.value_representations[
					result.plan.intervals[index].value];
				if (representation.kind == RepresentationKind::CoreWords)
				{
					compatible &= member.kind == RepresentationKind::CoreWords &&
						IsLowPrefixMask(member.word_mask);
					register_width = std::max<u32>(register_width, member.words);
				}
				else
				{
					compatible &= member.kind == representation.kind &&
						member.words == representation.words &&
						member.word_mask == representation.word_mask;
				}
			}
			if (!compatible)
				continue;
			if (register_width > register_count)
				continue;

			// A web can contain an old state value and its destructive successor in
			// one block. That pair interferes, but it does not invalidate the affinity
			// between every other predecessor/parameter version in the web. Build
			// classes by coalescing the actual SSA transfers rather than by arbitrary
			// interval order. Backedges are considered first, then frequently used
			// values, because those copies execute inside the repeated ownership unit.
			// Every merge still checks all SSA lifetime pairs; rejected affinities remain
			// ordinary verified edge copies.
			std::vector<std::vector<size_t>> classes;
			std::vector<size_t> class_for_interval(result.plan.intervals.size(),
				SIZE_MAX);
			for (const size_t index : web)
			{
				class_for_interval[index] = classes.size();
				classes.push_back({index});
			}
			std::vector<PhiAffinity> ordered_affinities;
			for (const PhiAffinity& affinity : phi_affinities)
			{
				if (phi_root(affinity.source) == phi_root(web.front()))
					ordered_affinities.push_back(affinity);
			}
			std::sort(ordered_affinities.begin(), ordered_affinities.end(),
				[](const PhiAffinity& left, const PhiAffinity& right) {
					return left.seam_priority != right.seam_priority ?
						left.seam_priority > right.seam_priority :
						left.weight != right.weight ? left.weight > right.weight :
						left.source != right.source ? left.source < right.source :
						left.target < right.target;
				});
			for (const PhiAffinity& affinity : ordered_affinities)
			{
				const size_t source_class = class_for_interval[affinity.source];
				const size_t target_class = class_for_interval[affinity.target];
				if (source_class == SIZE_MAX || target_class == SIZE_MAX ||
					source_class == target_class)
				{
					continue;
				}
				bool interferes = false;
				for (const size_t source_member : classes[source_class])
				{
					const Interval& source_interval =
						result.plan.intervals[source_member];
					for (const size_t target_member : classes[target_class])
					{
						const Interval& target_interval =
							result.plan.intervals[target_member];
						if (source_interval.block == target_interval.block &&
							source_interval.begin < target_interval.end &&
							target_interval.begin < source_interval.end)
						{
							interferes = true;
							break;
						}
					}
					if (interferes)
						break;
				}
				if (interferes)
					continue;
				for (const size_t member : classes[target_class])
				{
					classes[source_class].push_back(member);
					class_for_interval[member] = source_class;
				}
				classes[target_class].clear();
			}
			classes.erase(std::remove_if(classes.begin(), classes.end(),
				[](const std::vector<size_t>& group) { return group.empty(); }),
				classes.end());
			std::sort(classes.begin(), classes.end(),
				[&](const std::vector<size_t>& left,
					const std::vector<size_t>& right) {
					auto weight = [&](const std::vector<size_t>& group) {
						u64 uses = 0;
						for (const size_t index : group)
							uses += result.plan.intervals[index].uses;
						return uses;
					};
					return left.size() != right.size() ?
						left.size() > right.size() : weight(left) > weight(right);
				});

			for (const std::vector<size_t>& group : classes)
			{
				if (group.size() < 2)
					continue;
				for (const size_t index : group)
					phi_spill_affinity[index] = group.front();

				u32 selected_first = UINT32_MAX;
				for (u32 candidate_first = 0;
					candidate_first + register_width <= register_count;
					candidate_first++)
				{
					bool free = true;
					for (const size_t member_index : group)
					{
						const Interval& member = result.plan.intervals[member_index];
						const Representation& member_representation =
							result.plan.value_representations[member.value];
						const Location candidate{location_kind,
							static_cast<u16>(candidate_first),
							static_cast<u8>(member_representation.kind ==
								RepresentationKind::CoreWords ?
								member_representation.words : location_words),
							member_representation.word_mask};
						for (size_t other_index = 0;
							other_index < result.plan.intervals.size(); other_index++)
						{
							if (phi_preferences[other_index].kind == LocationKind::None)
								continue;
							const Interval& other = result.plan.intervals[other_index];
							if (other.block != member.block || other.end <= member.begin ||
								member.end <= other.begin)
							{
								continue;
							}
							if (LocationsOverlap(candidate,
									phi_preferences[other_index]))
							{
								free = false;
								break;
							}
						}
						if (!free)
							break;
					}
					if (free)
					{
						selected_first = candidate_first;
						break;
					}
				}
				if (selected_first == UINT32_MAX)
					continue;
				for (const size_t index : group)
				{
					const Representation& member = result.plan.value_representations[
						result.plan.intervals[index].value];
					phi_preferences[index] = {location_kind,
						static_cast<u16>(selected_first),
						static_cast<u8>(member.kind == RepresentationKind::CoreWords ?
							member.words : location_words), member.word_mask};
				}
			}
		}

		// Each non-interfering affinity class owns one physical location throughout
		// the region. If the register interference graph cannot color the class, keep
		// its members in one spill group rather than letting each block choose an
		// unrelated frame slot and paying a parallel copy on every internal edge.
		// Interfering classes remain distinct and communicate through verified edge
		// copies, so destructive old/new values can never alias prematurely.
		std::vector<size_t> spill_group(result.plan.intervals.size(), SIZE_MAX);
		for (size_t index = 0; index < result.plan.intervals.size(); index++)
		{
			if (phi_spill_affinity[index] == SIZE_MAX ||
				phi_preferences[index].kind != LocationKind::None)
			{
				continue;
			}
			const Representation& representation = result.plan.value_representations[
				result.plan.intervals[index].value];
			result.plan.intervals[index].location = {LocationKind::Spill, 0,
				static_cast<u8>(representation.kind == RepresentationKind::NeonQ ?
					4 : representation.words), representation.word_mask};
			spill_group[index] = phi_spill_affinity[index];
		}

		auto allocate_spill = [&](Interval* interval) -> bool {
			const Representation& representation =
				result.plan.value_representations[interval->value];
			const u32 bytes = representation.kind == RepresentationKind::NeonQ ?
				16u : representation.words * sizeof(u32);
			if (bytes == 0)
				return false;
			interval->location = {LocationKind::Spill, 0,
				static_cast<u8>(bytes / sizeof(u32)),
				representation.word_mask};
			const size_t index = static_cast<size_t>(
				interval - result.plan.intervals.data());
			if (index >= spill_group.size())
				return false;
			spill_group[index] = index;
			result.plan.spilled_values++;
			return true;
		};

		for (u32 block_index = 0; block_index < program.blocks.size(); block_index++)
		{
			std::vector<Interval*> ordered;
			for (Interval& interval : result.plan.intervals)
			{
				if (interval.block == block_index &&
					interval.location.kind == LocationKind::None)
				{
					ordered.push_back(&interval);
				}
			}
			std::sort(ordered.begin(), ordered.end(), [](const Interval* left,
				const Interval* right) {
				return left->begin != right->begin ? left->begin < right->begin :
					left->end > right->end;
			});

			std::vector<Interval*> active_core;
			std::vector<Interval*> active_vfp;
			std::vector<Interval*> active_neon;
			for (Interval* interval : ordered)
			{
				auto expire = [&](std::vector<Interval*>* active) {
					active->erase(std::remove_if(active->begin(), active->end(),
						[&](const Interval* candidate) {
							return candidate->end <= interval->begin;
						}), active->end());
				};

				const Representation& representation =
					result.plan.value_representations[interval->value];
				const bool vector =
					representation.kind == RepresentationKind::NeonQ;
				const bool vfp =
					representation.kind == RepresentationKind::VfpWord;
				const bool scalar =
					representation.kind == RepresentationKind::CoreWords;
				if (vfp)
				{
					expire(&active_vfp);
					std::bitset<32> used;
					for (const Interval* candidate : active_vfp)
						used.set(candidate->location.index);
					for (size_t reserved_index = 0;
						reserved_index < result.plan.intervals.size(); reserved_index++)
					{
						const Interval& reserved = result.plan.intervals[reserved_index];
						const Location& location = phi_preferences[reserved_index];
						if (&reserved != interval && reserved.block == interval->block &&
							location.kind == LocationKind::VfpS &&
							reserved.begin < interval->end && interval->begin < reserved.end)
						{
							used.set(location.index);
						}
					}
					const Location& preference = phi_preferences[
						interval_for_value[interval->value]];
					u32 s = preference.kind == LocationKind::VfpS &&
						preference.index < allocatable_vfp_s_registers &&
						!used.test(preference.index) ? preference.index : 0;
					while (s < allocatable_vfp_s_registers && used.test(s))
						s++;
					if (s < allocatable_vfp_s_registers)
					{
						interval->location = {LocationKind::VfpS,
							static_cast<u16>(s), 1, representation.word_mask};
						active_vfp.push_back(interval);
						result.plan.vfp_peak_s = std::max(result.plan.vfp_peak_s,
							static_cast<u32>(active_vfp.size()) + resident_vfp_values);
					}
					else if (!allocate_spill(interval))
						return Fail(BuildFailure::SpillCapacity, block_index, interval->value);
				}
				else if (vector)
				{
					expire(&active_neon);
					std::bitset<8> used;
					for (const Interval* candidate : active_neon)
						used.set(candidate->location.index);
					for (size_t reserved_index = 0;
						reserved_index < result.plan.intervals.size(); reserved_index++)
					{
						const Interval& reserved = result.plan.intervals[reserved_index];
						const Location& location = phi_preferences[reserved_index];
						if (&reserved != interval && reserved.block == interval->block &&
							location.kind == LocationKind::NeonQ &&
							reserved.begin < interval->end && interval->begin < reserved.end)
						{
							used.set(location.index);
						}
					}
					const Location& preference = phi_preferences[
						interval_for_value[interval->value]];
					u32 q = preference.kind == LocationKind::NeonQ &&
						preference.index < options.neon_q_registers &&
						!used.test(preference.index) ? preference.index : 0;
					while (q < options.neon_q_registers && used.test(q))
						q++;
					if (q < options.neon_q_registers)
					{
						interval->location = {LocationKind::NeonQ,
							static_cast<u16>(q), 4, representation.word_mask};
						active_neon.push_back(interval);
						result.plan.neon_peak_q = std::max(result.plan.neon_peak_q,
							static_cast<u32>(active_neon.size()));
					}
					else
					{
						// Standard linear-scan eviction: a short hot temporary may
						// displace a farther-ending non-phi value. Allocation is complete
						// before emission, so changing the victim to one spill location is
						// valid for its entire interval and cannot lose an already emitted
						// register value. Phi-preferred intervals retain their fixed web.
						Interval* victim = nullptr;
						for (Interval* candidate : active_neon)
						{
							const size_t candidate_index = static_cast<size_t>(
								candidate - result.plan.intervals.data());
							if (candidate_index >= phi_preferences.size() ||
								phi_preferences[candidate_index].kind != LocationKind::None ||
								candidate->end <= interval->end ||
								(preference.kind == LocationKind::NeonQ &&
								 candidate->location.index != preference.index))
							{
								continue;
							}
							if (!victim || candidate->end > victim->end)
								victim = candidate;
						}
						if (victim)
						{
							const u16 victim_q = victim->location.index;
							if (!allocate_spill(victim))
							{
								return Fail(BuildFailure::SpillCapacity, block_index,
									victim->value);
							}
							active_neon.erase(std::remove(active_neon.begin(),
								active_neon.end(), victim), active_neon.end());
							interval->location = {LocationKind::NeonQ, victim_q, 4,
								representation.word_mask};
							active_neon.push_back(interval);
						}
						else if (!allocate_spill(interval))
						{
							return Fail(BuildFailure::SpillCapacity, block_index,
								interval->value);
						}
					}
				}
				else if (scalar)
				{
					expire(&active_core);
					std::bitset<16> used;
					for (const Interval* candidate : active_core)
					{
						for (u32 word = 0; word < candidate->location.words; word++)
							used.set(candidate->location.index + word);
					}
					for (size_t reserved_index = 0;
						reserved_index < result.plan.intervals.size(); reserved_index++)
					{
						const Interval& reserved = result.plan.intervals[reserved_index];
						const Location& location = phi_preferences[reserved_index];
						if (&reserved == interval || reserved.block != interval->block ||
							location.kind != LocationKind::Core ||
							reserved.end <= interval->begin || interval->end <= reserved.begin)
						{
							continue;
						}
						for (u32 word = 0; word < location.words; word++)
							used.set(location.index + word);
					}
					const u32 words = representation.words;
					const Location& preference = phi_preferences[
						interval_for_value[interval->value]];
					auto core_window_free = [&](u32 first) {
						if (first + words > allocatable_core_register_words)
							return false;
						for (u32 word = 0; word < words; word++)
							if (used.test(first + word))
								return false;
						return true;
					};
					u32 first = preference.kind == LocationKind::Core &&
						preference.words == words &&
						core_window_free(preference.index) ? preference.index : 0;
					for (; first + words <= allocatable_core_register_words; first++)
					{
						if (core_window_free(first))
							break;
					}
					if (first + words <= allocatable_core_register_words)
					{
						interval->location = {LocationKind::Core,
							static_cast<u16>(first), static_cast<u8>(words),
							representation.word_mask};
						active_core.push_back(interval);
						result.plan.core_peak_words = std::max(
							result.plan.core_peak_words,
							static_cast<u32>(used.count()) + words);
					}
					else
					{
						// A no-eviction scan pins block parameters for their entire block
						// and spills every short SSA result. On Cortex-A9 that turns a small
						// loop into repeated stack traffic. Select a contiguous window whose
						// active occupants live farther than this result, spill those complete
						// intervals, and reuse the registers for the short live range. Static
						// use count breaks ties so frequently consumed loop state stays hot.
						struct Eviction
						{
							u32 first = 0;
							u32 uses = UINT32_MAX;
							u32 latest_end = 0;
							std::vector<Interval*> victims;
							bool valid = false;
						};
						Eviction best{};
						for (u32 candidate_first = 0;
							candidate_first + words <= allocatable_core_register_words;
							candidate_first++)
						{
							Eviction candidate{};
							candidate.first = candidate_first;
							candidate.uses = 0;
							for (Interval* active : active_core)
							{
								const bool overlaps =
									candidate_first < active->location.index +
										active->location.words &&
									active->location.index < candidate_first + words;
								if (!overlaps)
									continue;
								candidate.victims.push_back(active);
								candidate.uses += active->uses;
								candidate.latest_end = std::max(candidate.latest_end,
									active->end);
							}
							candidate.valid = !candidate.victims.empty() &&
								candidate.latest_end > interval->end;
							if (candidate.valid &&
								(!best.valid || candidate.uses < best.uses ||
									(candidate.uses == best.uses &&
									 candidate.latest_end > best.latest_end)))
							{
								best = std::move(candidate);
							}
						}
						if (best.valid)
						{
							for (Interval* victim : best.victims)
							{
								if (!allocate_spill(victim))
									return Fail(BuildFailure::SpillCapacity,
										block_index, victim->value);
							}
							active_core.erase(std::remove_if(active_core.begin(),
								active_core.end(), [&](const Interval* active) {
									return std::find(best.victims.begin(),
										best.victims.end(), active) != best.victims.end();
								}), active_core.end());
							interval->location = {LocationKind::Core,
								static_cast<u16>(best.first), static_cast<u8>(words),
								representation.word_mask};
							active_core.push_back(interval);
						}
						else if (!allocate_spill(interval))
						{
							return Fail(BuildFailure::SpillCapacity, block_index,
								interval->value);
						}
					}
				}
				else if (!allocate_spill(interval))
				{
					return Fail(BuildFailure::SpillCapacity, block_index, interval->value);
				}
			}
		}

		// Coalesce a block parameter with the location supplied by every incoming
		// edge when that location is identical and does not interfere with another
		// value in the target block. This is the first region-wide part of the
		// allocation: loops retain values across backedges without a canonical-state
		// round trip, while joins with disagreeing predecessors keep explicit moves.
		for (u32 target_block = 0; target_block < program.blocks.size(); target_block++)
		{
			const RegionIR::Block& target = program.blocks[target_block];
			for (size_t slot = 0; slot < EDGE_STATE_SLOTS; slot++)
			{
				const RegionIR::ValueId parameter =
					BlockStateParameter(target, slot);
				Interval* target_interval = interval_for(parameter);
				if (!target_interval)
					continue;
				Location candidate{};
				bool have_candidate = false;
				bool unanimous = true;
				for (const InternalEdge& edge : edges)
				{
					if (edge.transfer->target_block != target_block ||
						!edge.propagated.test(slot))
					{
						continue;
					}
					const RegionIR::ValueId edge_source =
						EdgeStateValue(edge.transfer->state, slot);
					Interval* source = interval_for(edge_source < program.value_count ?
						alias_root(edge_source) : RegionIR::INVALID_VALUE);
					if (!source || source->location.kind == LocationKind::Immediate ||
						source->location.kind == LocationKind::None)
					{
						unanimous = false;
						break;
					}
					if (!have_candidate)
					{
						candidate = source->location;
						have_candidate = true;
					}
					else if (candidate != source->location)
					{
						unanimous = false;
						break;
					}
				}
				const bool profitable_kind =
					(candidate.kind == LocationKind::Core ||
					 candidate.kind == LocationKind::NeonQ) &&
					(candidate.kind == target_interval->location.kind ||
					 target_interval->location.kind == LocationKind::Spill);
				if (!have_candidate || !unanimous || !profitable_kind ||
					candidate.words != target_interval->location.words ||
					candidate.word_mask != target_interval->location.word_mask)
				{
					continue;
				}
				bool conflict = false;
				for (const Interval& other : result.plan.intervals)
				{
					if (&other == target_interval || other.block != target_block ||
						other.end <= target_interval->begin ||
						target_interval->end <= other.begin)
					{
						continue;
					}
					if (LocationsOverlap(candidate, other.location))
					{
						conflict = true;
						break;
					}
				}
				if (!conflict)
					target_interval->location = candidate;
			}
		}

		// Color spill classes globally. A class can contain one ordinary SSA value
		// or every member of an uncolored phi web. Classes share bytes exactly when
		// no member lifetimes overlap in any block; this keeps the frame compact while
		// preserving one stable offset across calls, returns, joins and backedges.
		//
		// An incompatible whole phi web may still contain individually coalescible
		// predecessor/successor pairs. If both exact representations already live in
		// the private frame, merge their spill classes only when no member of either
		// class overlaps. This is ordinary interference-checked copy coalescing: the
		// source remains live in the shared slot at the edge and the target begins in
		// that same slot, eliminating a spill-to-spill copy without weakening either
		// SSA lifetime or architectural exit map.
		bool spill_groups_changed = true;
		while (spill_groups_changed)
		{
			spill_groups_changed = false;
			for (const InternalEdge& edge : edges)
			{
				if (!edge.transfer ||
					edge.transfer->target_block >= program.blocks.size())
				{
					continue;
				}
				const RegionIR::Block& target =
					program.blocks[edge.transfer->target_block];
				for (size_t slot = 1; slot < EDGE_STATE_SLOTS; slot++)
				{
					if (!edge.propagated.test(slot))
						continue;
					const RegionIR::ValueId source_value =
						EdgeStateValue(edge.transfer->state, slot);
					const RegionIR::ValueId source_root =
						source_value < program.value_count ?
							alias_root(source_value) : RegionIR::INVALID_VALUE;
					const RegionIR::ValueId target_value =
						BlockStateParameter(target, slot);
					Interval* const source = interval_for(source_root);
					Interval* const destination = interval_for(target_value);
					if (!source || !destination || source == destination ||
						source->location.kind != LocationKind::Spill ||
						destination->location.kind != LocationKind::Spill ||
						source->location.words != destination->location.words ||
						NormalizedWordMask(source->location) !=
							NormalizedWordMask(destination->location))
					{
						continue;
					}
					const size_t source_index = static_cast<size_t>(
						source - result.plan.intervals.data());
					const size_t destination_index = static_cast<size_t>(
						destination - result.plan.intervals.data());
					if (source_index >= spill_group.size() ||
						destination_index >= spill_group.size() ||
						spill_group[source_index] == SIZE_MAX ||
						spill_group[destination_index] == SIZE_MAX ||
						spill_group[source_index] == spill_group[destination_index])
					{
						continue;
					}
					const size_t source_group = spill_group[source_index];
					const size_t destination_group = spill_group[destination_index];
					bool interferes = false;
					for (size_t left = 0; !interferes &&
						left < result.plan.intervals.size(); left++)
					{
						if (spill_group[left] != source_group ||
							result.plan.intervals[left].location.kind !=
								LocationKind::Spill)
						{
							continue;
						}
						const Interval& a = result.plan.intervals[left];
						for (size_t right = 0;
							right < result.plan.intervals.size(); right++)
						{
							if (spill_group[right] != destination_group ||
								result.plan.intervals[right].location.kind !=
									LocationKind::Spill)
							{
								continue;
							}
							const Interval& b = result.plan.intervals[right];
							if (a.block == b.block && a.begin < b.end &&
								b.begin < a.end)
							{
								interferes = true;
								break;
							}
						}
					}
					if (interferes)
						continue;
					for (size_t& group : spill_group)
					{
						if (group == destination_group)
							group = source_group;
					}
					result.plan.coalesced_spill_edge_values++;
					spill_groups_changed = true;
				}
			}
		}
		struct SpillClass
		{
			size_t id = SIZE_MAX;
			std::vector<size_t> members;
			u32 bytes = 0;
			u32 offset = 0;
		};
		std::vector<SpillClass> spill_classes;
		for (size_t index = 0; index < result.plan.intervals.size(); index++)
		{
			if (result.plan.intervals[index].location.kind != LocationKind::Spill)
				continue;
			const size_t group = spill_group[index] == SIZE_MAX ? index : spill_group[index];
			auto found = std::find_if(spill_classes.begin(), spill_classes.end(),
				[&](const SpillClass& candidate) { return candidate.id == group; });
			if (found == spill_classes.end())
			{
				spill_classes.push_back({group, {}, 0, 0});
				found = std::prev(spill_classes.end());
			}
			found->members.push_back(index);
			found->bytes = std::max(found->bytes,
				static_cast<u32>(result.plan.intervals[index].location.words) *
					sizeof(u32));
		}
		std::sort(spill_classes.begin(), spill_classes.end(),
			[](const SpillClass& left, const SpillClass& right) {
				return left.bytes != right.bytes ? left.bytes > right.bytes :
					left.id < right.id;
			});
		auto classes_interfere = [&](const SpillClass& left,
			const SpillClass& right) {
			for (const size_t left_index : left.members)
			{
				const Interval& a = result.plan.intervals[left_index];
				for (const size_t right_index : right.members)
				{
					const Interval& b = result.plan.intervals[right_index];
					if (a.block == b.block && a.begin < b.end && b.begin < a.end)
						return true;
				}
			}
			return false;
		};
		result.plan.spill_bytes = 0;
		for (size_t class_index = 0; class_index < spill_classes.size(); class_index++)
		{
			SpillClass& current = spill_classes[class_index];
			const u32 alignment = std::min(current.bytes, 16u);
			u32 offset = 0;
			for (;;)
			{
				offset = AlignUp(offset, alignment);
				u32 next = offset;
				for (size_t prior_index = 0; prior_index < class_index; prior_index++)
				{
					const SpillClass& prior = spill_classes[prior_index];
					if (classes_interfere(current, prior) &&
						offset < prior.offset + prior.bytes &&
						prior.offset < offset + current.bytes)
					{
						next = std::max(next, prior.offset + prior.bytes);
					}
				}
				if (next == offset)
					break;
				offset = next;
			}
			if (offset > options.max_spill_bytes ||
				current.bytes > options.max_spill_bytes - offset ||
				offset > UINT16_MAX)
			{
				const Interval& failed =
					result.plan.intervals[current.members.front()];
				return Fail(BuildFailure::SpillCapacity, failed.block, failed.value);
			}
			current.offset = offset;
			for (const size_t member : current.members)
				result.plan.intervals[member].location.index = static_cast<u16>(offset);
			result.plan.spill_bytes = std::max(result.plan.spill_bytes,
				offset + current.bytes);
		}

		// Coalescing may promote an initially spilled target parameter into a free
		// incoming register. Recompute pressure and spill counts from the final
		// locations rather than reporting the provisional block-local scan.
		result.plan.core_peak_words = 0;
		result.plan.neon_peak_q = 0;
		result.plan.spilled_values = 0;
		for (const Interval& interval : result.plan.intervals)
		{
			result.plan.spilled_values +=
				interval.location.kind == LocationKind::Spill ? 1u : 0u;
			if (interval.location.kind != LocationKind::Core &&
				interval.location.kind != LocationKind::NeonQ)
			{
				continue;
			}
			std::bitset<16> occupied;
			for (const Interval& other : result.plan.intervals)
			{
				if (other.block != interval.block ||
					other.location.kind != interval.location.kind ||
					other.end <= interval.begin || interval.end <= other.begin)
				{
					continue;
				}
				if (interval.location.kind == LocationKind::NeonQ)
					occupied.set(other.location.index);
				else
				{
					for (u32 word = 0; word < other.location.words; word++)
						occupied.set(other.location.index + word);
				}
			}
			if (interval.location.kind == LocationKind::Core)
				result.plan.core_peak_words = std::max(result.plan.core_peak_words,
					static_cast<u32>(occupied.count()));
			else
				result.plan.neon_peak_q = std::max(result.plan.neon_peak_q,
					static_cast<u32>(occupied.count()));
		}

		// Allocation is an executable ownership contract, not a hint to the
		// backend. Verify the final plan after phi precolouring, eviction and spill
		// compaction: every location must fit its physical bank and no two live SSA
		// intervals may own overlapping storage. This also prevents a malformed
		// narrow word mask from mapping a dead component onto a reserved ABI register.
		for (size_t index = 0; index < result.plan.intervals.size(); index++)
		{
			const Interval& interval = result.plan.intervals[index];
			const Location& location = interval.location;
			const bool in_bounds =
				(location.kind != LocationKind::Core ||
				 location.index + location.words <= options.core_register_words) &&
				(location.kind != LocationKind::VfpS ||
				 location.index < available_vfp_s_registers) &&
				(location.kind != LocationKind::NeonQ ||
				 location.index < options.neon_q_registers) &&
				(location.kind != LocationKind::Spill ||
				 location.index + location.words * sizeof(u32) <=
					 result.plan.spill_bytes) &&
				CountWords(NormalizedWordMask(location)) <= location.words;
			if (!in_bounds)
				return Fail(BuildFailure::InvalidValue, interval.block, interval.value);

			for (size_t other_index = index + 1;
				other_index < result.plan.intervals.size(); other_index++)
			{
				const Interval& other = result.plan.intervals[other_index];
				if (other.block != interval.block ||
					other.end <= interval.begin || interval.end <= other.begin)
				{
					continue;
				}
				// Cycle SSA values deliberately share the fixed r1:r2 countdown pair;
				// EmitAddCycles serializes their updates at edges rather than treating
				// them as ordinary simultaneously resident values.
				if (location.kind != LocationKind::FixedCycle &&
					other.location.kind != LocationKind::FixedCycle &&
					LocationsOverlap(location, other.location))
					return Fail(BuildFailure::EdgeCopyConflict, interval.block,
						interval.value);
			}
		}

		for (const Interval& interval : result.plan.intervals)
			result.plan.value_locations[interval.value] = interval.location;
		for (RegionIR::ValueId value = 0; value < program.value_count; value++)
		{
			if (alias_source[value] == RegionIR::INVALID_VALUE ||
				result.plan.value_word_demands[value] == 0)
			{
				continue;
			}
			const RegionIR::ValueId root = alias_root(value);
			if (root >= result.plan.value_locations.size())
				return Fail(BuildFailure::InvalidValue, ranges[value].block, value);
			const Location& root_location = result.plan.value_locations[root];
			const Representation& representation =
				result.plan.value_representations[value];
			if (root_location.kind == LocationKind::None ||
				representation.words == 0)
			{
				return Fail(BuildFailure::InvalidValue, ranges[value].block, value);
			}
			Location alias = root_location;
			alias.word_mask = representation.word_mask;
			if (alias.kind != LocationKind::NeonQ)
				alias.words = representation.words;
			result.plan.value_locations[value] = alias;
		}
		if (options.scalar_addressable_neon_q_registers != 0)
		{
			for (RegionIR::ValueId value = 0; value < program.value_count; value++)
			{
				if (neon_scalar_priority[value] == 0 || alias_root(value) != value)
					continue;
				result.plan.scalar_preferred_neon_values++;
				const Location& location = result.plan.value_locations[value];
				if (location.kind != LocationKind::NeonQ ||
					location.index >= options.scalar_addressable_neon_q_registers)
				{
					result.plan.scalar_preferred_neon_misses++;
				}
			}
		}

		auto attribute_residual_spill_edge =
			[&](RegionIR::ValueId source_value,
				RegionIR::ValueId target_value) {
				const RegionIR::ValueId source_root =
					source_value < program.value_count ? alias_root(source_value) :
						RegionIR::INVALID_VALUE;
				const RegionIR::ValueId target_root =
					target_value < program.value_count ? alias_root(target_value) :
						RegionIR::INVALID_VALUE;
				Interval* const source = interval_for(source_root);
				Interval* const destination = interval_for(target_root);
				result.plan.residual_spill_to_spill_edges++;
				if (!source || !destination ||
					source->location.kind != LocationKind::Spill ||
					destination->location.kind != LocationKind::Spill)
				{
					result.plan.residual_spill_missing_groups++;
					return;
				}
				if (source->location.words != destination->location.words ||
					NormalizedWordMask(source->location) !=
						NormalizedWordMask(destination->location))
				{
					result.plan.residual_spill_shape_mismatches++;
					return;
				}
				const size_t source_index = static_cast<size_t>(
					source - result.plan.intervals.data());
				const size_t destination_index = static_cast<size_t>(
					destination - result.plan.intervals.data());
				if (source_index >= spill_group.size() ||
					destination_index >= spill_group.size() ||
					spill_group[source_index] == SIZE_MAX ||
					spill_group[destination_index] == SIZE_MAX)
				{
					result.plan.residual_spill_missing_groups++;
					return;
				}
				const size_t source_group = spill_group[source_index];
				const size_t destination_group = spill_group[destination_index];
				if (source_group == destination_group)
				{
					result.plan.residual_spill_same_groups++;
					return;
				}
				bool interferes = false;
				for (size_t left = 0; !interferes &&
					left < result.plan.intervals.size(); left++)
				{
					if (spill_group[left] != source_group ||
						result.plan.intervals[left].location.kind != LocationKind::Spill)
					{
						continue;
					}
					const Interval& a = result.plan.intervals[left];
					for (size_t right = 0; right < result.plan.intervals.size(); right++)
					{
						if (spill_group[right] != destination_group ||
							result.plan.intervals[right].location.kind !=
								LocationKind::Spill)
						{
							continue;
						}
						const Interval& b = result.plan.intervals[right];
						if (a.block == b.block && a.begin < b.end && b.begin < a.end)
						{
							interferes = true;
							break;
						}
					}
				}
				if (interferes)
					result.plan.residual_spill_group_interferences++;
				else
					result.plan.residual_spill_unexplained++;
			};

		for (const InternalEdge& edge : edges)
		{
			const RegionIR::Block& target =
				program.blocks[edge.transfer->target_block];
			for (size_t slot = 0; slot < EDGE_STATE_SLOTS; slot++)
			{
				if (!edge.propagated.test(slot))
					continue;
				// Architectural GPR0 is represented as an immediate zero in every
				// block. It has no physical edge state to copy even when consumers on
				// the two sides demand different subsets of its qword lanes.
				if (slot == 0)
				{
					result.plan.coalesced_edge_values++;
					continue;
				}
				const RegionIR::ValueId source =
					EdgeStateValue(edge.transfer->state, slot);
				const RegionIR::ValueId parameter =
					BlockStateParameter(target, slot);
				if (definitions[source]->type == RegionIR::ValueType::MemoryEffect)
					continue;
				if (definitions[source]->type != definitions[parameter]->type)
					return Fail(BuildFailure::EdgeCopyConflict, edge.source_block, source);
				const Location source_location = result.plan.value_locations[source];
				const Location target_location = result.plan.value_locations[parameter];
				const u8 copy_mask =
					result.plan.value_representations[parameter].word_mask;
				if (source_location.kind == LocationKind::None ||
					target_location.kind == LocationKind::None || copy_mask == 0 ||
					(result.plan.value_rematerialized_word_masks[source] & copy_mask) != 0 ||
					(result.plan.value_representations[source].word_mask & copy_mask) !=
						copy_mask)
				{
					return Fail(BuildFailure::InvalidValue, edge.source_block, source);
				}
				const bool canonical_identity =
					source_location.kind == LocationKind::CanonicalState &&
					target_location.kind == LocationKind::CanonicalState &&
					source_location.index == target_location.index &&
					(NormalizedWordMask(source_location) & copy_mask) == copy_mask;
				u8 move_mask = 0;
				for (u8 word = 0; word < 4; word++)
				{
					const u8 component = static_cast<u8>(1u << word);
					if ((copy_mask & component) == 0)
						continue;
					if (!SamePhysicalStorage(ComponentLocation(source_location, word),
							ComponentLocation(target_location, word)))
					{
						move_mask |= component;
					}
				}
				const bool immutable_identity =
					source_location.kind == LocationKind::Immediate &&
					target_location.kind == LocationKind::Immediate &&
					source < result.plan.value_aliases.size() &&
					parameter < result.plan.value_aliases.size() &&
					result.plan.value_aliases[source] ==
						result.plan.value_aliases[parameter];
				if (move_mask != 0 && !canonical_identity && !immutable_identity)
				{
					result.plan.edge_moves.push_back({edge.source_block,
						edge.transfer->target_block, edge.edge_index, source, parameter,
						source_location, target_location, move_mask});
					if (source_location.kind == LocationKind::Spill &&
						target_location.kind == LocationKind::Spill)
					{
						attribute_residual_spill_edge(source, parameter);
					}
				}
				else
				{
					result.plan.coalesced_edge_values++;
				}
			}
		}

		const EdgeCopyScheduleResult schedule = BuildEdgeCopySchedule(
			result.plan.edge_moves, result.plan.spill_bytes,
			options.max_spill_bytes);
		if (!schedule)
			return Fail(schedule.failure, schedule.block, schedule.value);
		result.plan.edge_copy_steps = schedule.steps;
		result.plan.spill_bytes = schedule.spill_bytes;
		result.plan.edge_scratch_offset = schedule.scratch_offset;
		result.plan.edge_scratch_bytes = schedule.scratch_bytes;
		return result;
	}
} // namespace VitaEE::RegionAllocation
