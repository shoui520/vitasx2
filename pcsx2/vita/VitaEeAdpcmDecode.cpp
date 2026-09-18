// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#include "pcsx2/vita/VitaEeAdpcmDecode.h"

#include <algorithm>
#include <cstdlib>
#include <string_view>
#include <utility>

namespace VitaEE::AdpcmDecode
{
	namespace
	{
		using namespace RegionIR;

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

		bool Contains(const std::vector<u32>& blocks, u32 block)
		{
			return std::find(blocks.begin(), blocks.end(), block) != blocks.end();
		}

		bool IsCop1Arithmetic(Opcode opcode)
		{
			switch (opcode)
			{
				case Opcode::Cop1NormalizeInput:
				case Opcode::Cop1AddRaw:
				case Opcode::Cop1SubRaw:
				case Opcode::Cop1MulRaw:
				case Opcode::Cop1ExceptionalOuResult:
				case Opcode::Cop1ClampOuResult:
				case Opcode::Cop1UpdateOuFlags:
				case Opcode::Cop1ConvertWord:
				case Opcode::Cop1ConvertSingle:
					return true;
				default:
					return false;
			}
		}

		struct Counts
		{
			std::array<u32, static_cast<size_t>(MemoryAccessKind::StoreVu0Vector) + 1>
				memory{};
			u32 stores = 0;
			u32 cop1_arithmetic = 0;
			u32 cop1_multiply = 0;
			u32 cop1_add = 0;
			u32 cop1_clamp = 0;
			u32 cop1_convert_word = 0;
			u32 integer_multiply = 0;
			u32 integer_shift_right = 0;
		};

		u32 MemoryCount(const Counts& counts, MemoryAccessKind kind)
		{
			return counts.memory[static_cast<size_t>(kind)];
		}

		Counts CountNodes(const Program& program, const std::vector<u32>& blocks)
		{
			Counts counts{};
			for (u32 block_index : blocks)
			{
				if (block_index >= program.blocks.size())
					continue;
				for (const Node& node : program.blocks[block_index].nodes)
				{
					if (node.opcode == Opcode::MemoryLoad ||
						node.opcode == Opcode::MemoryStore)
					{
						const auto kind = static_cast<MemoryAccessKind>(node.immediate);
						counts.memory[static_cast<size_t>(kind)]++;
						counts.stores += node.opcode == Opcode::MemoryStore;
					}
					counts.cop1_arithmetic += IsCop1Arithmetic(node.opcode);
					counts.cop1_multiply += node.opcode == Opcode::Cop1MulRaw;
					counts.cop1_add += node.opcode == Opcode::Cop1AddRaw;
					counts.cop1_clamp += node.opcode == Opcode::Cop1ClampOuResult;
					counts.cop1_convert_word +=
						node.opcode == Opcode::Cop1ConvertWord;
					counts.integer_multiply +=
						node.opcode == Opcode::MultiplySigned32;
					counts.integer_shift_right +=
						node.opcode == Opcode::ShiftRightArithmetic32;
				}
			}
			return counts;
		}

		bool HasDirectAddConstant(const Program& program,
			const std::vector<u32>& blocks, u32 literal)
		{
			for (u32 block_index : blocks)
			{
				if (block_index >= program.blocks.size())
					continue;
				const Block& block = program.blocks[block_index];
				for (const Node& add : block.nodes)
				{
					if (add.opcode != Opcode::Add32 || add.operand_count != 2)
						continue;
					for (ValueId operand : {add.operands[0], add.operands[1]})
					{
						const auto found = std::find_if(block.nodes.begin(),
							block.nodes.end(), [&](const Node& node) {
								return node.id == operand;
							});
						if (found != block.nodes.end() &&
							found->opcode == Opcode::ConstantI32 &&
							static_cast<u32>(found->literal) == literal)
						{
							return true;
						}
					}
				}
			}
			return false;
		}

		struct Definition
		{
			const Node* node = nullptr;
			u32 block = INVALID_BLOCK;
		};

		struct Affine
		{
			bool valid = false;
			bool has_base = false;
			s64 offset = 0;
		};
		using AffineAnchors = std::vector<std::pair<ValueId, Affine>>;

		Definition FindDefinition(const Program& program, ValueId value)
		{
			for (u32 block_index = 0; block_index < program.blocks.size(); block_index++)
			{
				for (const Node& node : program.blocks[block_index].nodes)
					if (node.id == value)
						return {&node, block_index};
			}
			return {};
		}

		bool ParameterGpr(const Program& program, const Definition& definition,
			u32* gpr)
		{
			if (!gpr || !definition.node ||
				definition.node->opcode != Opcode::Parameter ||
				definition.block >= program.blocks.size())
			{
				return false;
			}
			for (u32 index = 0; index < 32; index++)
			{
				if (program.blocks[definition.block].parameters.gpr[index] ==
					definition.node->id)
				{
					*gpr = index;
					return true;
				}
			}
			return false;
		}

		Affine ResolveAffineFromHeaderImpl(const Program& program, u32 header,
			u32 block, ValueId value, u32 base_gpr,
			const AffineAnchors* anchors, u32 depth)
		{
			if (depth > 128)
				return {};
			if (anchors)
			{
				const auto anchor = std::find_if(anchors->begin(), anchors->end(),
					[&](const auto& item) { return item.first == value; });
				if (anchor != anchors->end())
					return anchor->second;
			}
			const Definition definition = FindDefinition(program, value);
			if (!definition.node)
				return {};
			const Node& node = *definition.node;
			if (node.opcode == Opcode::Parameter)
			{
				u32 gpr = 0;
				if (!ParameterGpr(program, definition, &gpr))
					return {};
				// The verifier materializes r0 as an ordinary block parameter, but its
				// architectural value is invariantly zero on every incoming edge.
				if (gpr == 0)
					return {true, false, 0};
				if (definition.block == header)
				{
					if (gpr == base_gpr)
						return {true, true, 0};
					return {};
				}
				Affine common{};
				bool have_common = false;
				for (u32 source = 0; source < program.blocks.size(); source++)
				{
					VisitInternalTransfers(program.blocks[source].terminator,
						[&](const Transfer& transfer, u8) {
							if (transfer.target_block != definition.block)
								return true;
							const Affine incoming = ResolveAffineFromHeaderImpl(program,
								header, source, transfer.state.gpr[gpr], base_gpr,
								anchors, depth + 1);
							if (!incoming.valid || (have_common &&
								(incoming.has_base != common.has_base ||
								 incoming.offset != common.offset)))
							{
								common = {};
								have_common = true;
								return false;
							}
							common = incoming;
							have_common = true;
							return true;
						});
					if (have_common && !common.valid)
						return {};
				}
				return have_common ? common : Affine{};
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
					ResolveAffineFromHeaderImpl(program, header, definition.block,
						node.operands[0], base_gpr, anchors, depth + 1) : Affine{};
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
						ResolveAffineFromHeaderImpl(program, header, definition.block,
							node.operands[1], base_gpr, anchors, depth + 1) : Affine{};
				case Opcode::Add32:
				case Opcode::Add64:
				case Opcode::EffectiveAddress32:
				case Opcode::Sub32:
				case Opcode::Sub64:
				{
					if (node.operand_count != 2)
						return {};
					const Affine left = ResolveAffineFromHeaderImpl(program, header,
						definition.block, node.operands[0], base_gpr, anchors,
						depth + 1);
					const Affine right = ResolveAffineFromHeaderImpl(program, header,
						definition.block, node.operands[1], base_gpr, anchors,
						depth + 1);
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

		Affine ResolveAffineFromHeader(const Program& program, u32 header,
			u32 block, ValueId value, u32 base_gpr, u32 depth = 0)
		{
			return ResolveAffineFromHeaderImpl(program, header, block, value,
				base_gpr, nullptr, depth);
		}

		Affine ResolveAffineFromHeaderAnchored(const Program& program, u32 header,
			u32 block, ValueId value, u32 base_gpr,
			const AffineAnchors& anchors)
		{
			return ResolveAffineFromHeaderImpl(program, header, block, value,
				base_gpr, &anchors, 0);
		}

		bool ResolveConstant(const Program& program, u32 block, ValueId value,
			s64* constant)
		{
			if (!constant)
				return false;
			// INVALID_BLOCK cannot equal a real header, so encountering any parameter
			// fails while a directly constructed constant/extension/addition succeeds.
			const Affine affine = ResolveAffineFromHeader(program, INVALID_BLOCK,
				block, value, UINT32_MAX);
			if (!affine.valid || affine.has_base)
				return false;
			*constant = affine.offset;
			return true;
		}

		bool MatchExactInnerControl(const Program& program,
			const SemanticIsland::Loop& loop, u8* counter_gpr, s32* stride,
			u32* iterations)
		{
			if (!counter_gpr || !stride || !iterations ||
				loop.header_block >= program.blocks.size() ||
				loop.latch_blocks.size() != 1)
			{
				return false;
			}
			const u32 latch = loop.latch_blocks.front();
			if (latch >= program.blocks.size())
				return false;
			const Block& latch_block = program.blocks[latch];
			if (latch_block.terminator.kind != TerminatorKind::Branch ||
				latch_block.terminator.taken.target_block != loop.header_block ||
				Contains(loop.blocks, latch_block.terminator.not_taken.target_block))
			{
				return false;
			}
			const Definition condition =
				FindDefinition(program, latch_block.terminator.condition);
			if (!condition.node ||
				condition.node->opcode !=
					Opcode::CompareSignedGreaterEqualZero64 ||
				condition.node->operand_count != 1)
			{
				return false;
			}

			u32 matches = 0;
			for (u32 gpr = 1; gpr < 32; gpr++)
			{
				s64 entry_seed = 0;
				bool have_entry = false;
				bool entry_valid = true;
				for (u32 source = 0; source < program.blocks.size(); source++)
				{
					VisitInternalTransfers(program.blocks[source].terminator,
						[&](const Transfer& transfer, u8) {
							if (transfer.target_block != loop.header_block ||
								Contains(loop.blocks, source))
							{
								return true;
							}
							s64 candidate = 0;
							if (!ResolveConstant(program, source,
									transfer.state.gpr[gpr], &candidate) ||
								(have_entry && candidate != entry_seed))
							{
								entry_valid = false;
								return false;
							}
							entry_seed = candidate;
							have_entry = true;
							return true;
						});
					if (!entry_valid)
						break;
				}
				if (!entry_valid || !have_entry || entry_seed != 30)
					continue;

				const Affine recurrence = ResolveAffineFromHeader(program,
					loop.header_block, latch,
					latch_block.terminator.taken.state.gpr[gpr], gpr);
				const Affine predicate = ResolveAffineFromHeader(program,
					loop.header_block, condition.block,
					condition.node->operands[0], gpr);
				if (!recurrence.valid || !recurrence.has_base ||
					recurrence.offset != -2 || !predicate.valid ||
					!predicate.has_base || predicate.offset != recurrence.offset)
				{
					continue;
				}
				*counter_gpr = static_cast<u8>(gpr);
				*stride = static_cast<s32>(recurrence.offset);
				*iterations = static_cast<u32>(entry_seed / -recurrence.offset) + 1;
				matches++;
			}
			return matches == 1 && *iterations == 16;
		}

		Definition StripScalarWrappers(const Program& program, ValueId value)
		{
			for (u32 depth = 0; depth < 64; depth++)
			{
				const Definition definition = FindDefinition(program, value);
				if (!definition.node)
					return {};
				const Node& node = *definition.node;
				switch (node.opcode)
				{
					case Opcode::ExtractLow32:
					case Opcode::ExtractLow64:
					case Opcode::AddressFromI32:
					case Opcode::SignExtend32To64:
					case Opcode::ZeroExtend32To64:
						if (node.operand_count < 1)
							return {};
						value = node.operands[0];
						break;
					case Opcode::ReplaceLow64:
						if (node.operand_count != 2)
							return {};
						value = node.operands[1];
						break;
					default:
						return definition;
				}
			}
			return {};
		}

		bool IsZero(const Program& program, ValueId value)
		{
			const Definition definition = StripScalarWrappers(program, value);
			if (!definition.node)
				return false;
			if ((definition.node->opcode == Opcode::ConstantI1 ||
				 definition.node->opcode == Opcode::ConstantI32 ||
				 definition.node->opcode == Opcode::ConstantI64 ||
				 definition.node->opcode == Opcode::ConstantAddress) &&
				definition.node->literal == 0)
			{
				return true;
			}
			u32 gpr = UINT32_MAX;
			return ParameterGpr(program, definition, &gpr) && gpr == 0;
		}

		Affine ResolveAffineFromAnchor(const Program& program, ValueId value,
			ValueId anchor, u32 depth = 0)
		{
			if (depth > 128)
				return {};
			if (value == anchor)
				return {true, true, 0};
			const Definition definition = FindDefinition(program, value);
			if (!definition.node)
				return {};
			const Node& node = *definition.node;
			if (node.opcode == Opcode::ConstantI1 ||
				node.opcode == Opcode::ConstantI32 ||
				node.opcode == Opcode::ConstantI64 ||
				node.opcode == Opcode::ConstantAddress)
			{
				return {true, false,
					static_cast<s32>(static_cast<u32>(node.literal))};
			}
			u32 parameter_gpr = UINT32_MAX;
			if (ParameterGpr(program, definition, &parameter_gpr) &&
				parameter_gpr == 0)
			{
				return {true, false, 0};
			}
			auto unary = [&]() {
				return node.operand_count >= 1 ?
					ResolveAffineFromAnchor(program, node.operands[0], anchor,
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
						ResolveAffineFromAnchor(program, node.operands[1], anchor,
							depth + 1) : Affine{};
				case Opcode::Add32:
				case Opcode::Add64:
				case Opcode::EffectiveAddress32:
				case Opcode::Sub32:
				case Opcode::Sub64:
				{
					if (node.operand_count != 2)
						return {};
					const Affine left = ResolveAffineFromAnchor(program,
						node.operands[0], anchor, depth + 1);
					const Affine right = ResolveAffineFromAnchor(program,
						node.operands[1], anchor, depth + 1);
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
					return offset >= INT32_MIN && offset <= INT32_MAX ?
						Affine{true, left.has_base || right.has_base, offset} :
						Affine{};
				}
				default:
					return {};
			}
		}

		bool MatchAddOne(const Program& program, ValueId value,
			Definition* addition, ValueId* input)
		{
			if (!addition || !input)
				return false;
			const Definition found = StripScalarWrappers(program, value);
			if (!found.node ||
				(found.node->opcode != Opcode::Add32 &&
				 found.node->opcode != Opcode::Add64) ||
				found.node->operand_count != 2)
			{
				return false;
			}
			for (u32 constant_operand = 0; constant_operand < 2;
				constant_operand++)
			{
				s64 constant = 0;
				if (ResolveConstant(program, found.block,
						found.node->operands[constant_operand], &constant) &&
					constant == 1)
				{
					*addition = found;
					*input = found.node->operands[1 - constant_operand];
					return true;
				}
			}
			return false;
		}

		MemoryOperation DescribeMemoryOperation(const Program& program,
			u32 block, const Node& node)
		{
			MemoryOperation operation{};
			operation.operation = node.id;
			operation.block = block;
			operation.kind = static_cast<MemoryAccessKind>(node.immediate);
			if (block < program.blocks.size())
			{
				const auto& nodes = program.blocks[block].nodes;
				const auto found = std::find_if(nodes.begin(), nodes.end(),
					[&](const Node& candidate) { return candidate.id == node.id; });
				if (found != nodes.end())
					operation.node_ordinal = static_cast<u32>(found - nodes.begin());
			}
			return operation;
		}

		bool IsHeaderGprInvariant(const Program& program, u32 header,
			ValueId value, u32 base_gpr,
			std::vector<std::pair<u32, u32>>* active, u32 depth = 0)
		{
			if (!active || depth > 128)
				return false;
			const Definition definition = FindDefinition(program, value);
			if (!definition.node)
				return false;
			const Node& node = *definition.node;
			switch (node.opcode)
			{
				case Opcode::ExtractLow32:
				case Opcode::ExtractLow64:
				case Opcode::AddressFromI32:
				case Opcode::SignExtend32To64:
				case Opcode::ZeroExtend32To64:
					return node.operand_count >= 1 && IsHeaderGprInvariant(program,
						header, node.operands[0], base_gpr, active, depth + 1);
				case Opcode::ReplaceLow64:
					return node.operand_count == 2 && IsHeaderGprInvariant(program,
						header, node.operands[1], base_gpr, active, depth + 1);
				case Opcode::Add32:
				case Opcode::Add64:
				{
					if (node.operand_count != 2)
						return false;
					for (u32 constant_operand = 0; constant_operand < 2;
						constant_operand++)
					{
						s64 constant = 0;
						if (ResolveConstant(program, definition.block,
								node.operands[constant_operand], &constant) &&
							constant == 0)
						{
							return IsHeaderGprInvariant(program, header,
								node.operands[1 - constant_operand], base_gpr,
								active, depth + 1);
						}
					}
					return false;
				}
				case Opcode::Parameter:
					break;
				default:
					return false;
			}

			u32 gpr = UINT32_MAX;
			if (!ParameterGpr(program, definition, &gpr) || gpr != base_gpr)
				return false;
			if (definition.block == header)
				return true;
			const std::pair<u32, u32> key{definition.block, gpr};
			if (std::find(active->begin(), active->end(), key) != active->end())
				return true;
			active->push_back(key);
			bool valid = true;
			bool have_incoming = false;
			for (u32 source = 0; source < program.blocks.size(); source++)
			{
				VisitInternalTransfers(program.blocks[source].terminator,
					[&](const Transfer& transfer, u8) {
						if (transfer.target_block != definition.block)
							return true;
						have_incoming = true;
						if (!IsHeaderGprInvariant(program, header,
								transfer.state.gpr[gpr], base_gpr, active, depth + 1))
						{
							valid = false;
							return false;
						}
						return true;
					});
				if (!valid)
					break;
			}
			active->pop_back();
			return valid && have_incoming;
		}

		bool IsHeaderGprInvariant(const Program& program, u32 header,
			ValueId value, u32 base_gpr)
		{
			std::vector<std::pair<u32, u32>> active;
			return IsHeaderGprInvariant(program, header, value, base_gpr,
				&active);
		}

		Affine ResolveInvariantAffineFromHeader(const Program& program, u32 header,
			ValueId value, u32 base_gpr, u32 depth = 0)
		{
			if (depth > 128)
				return {};
			if (IsHeaderGprInvariant(program, header, value, base_gpr))
				return {true, true, 0};
			const Definition definition = FindDefinition(program, value);
			if (!definition.node)
				return {};
			const Node& node = *definition.node;
			if (node.opcode == Opcode::ConstantI1 ||
				node.opcode == Opcode::ConstantI32 ||
				node.opcode == Opcode::ConstantI64 ||
				node.opcode == Opcode::ConstantAddress)
			{
				return {true, false,
					static_cast<s32>(static_cast<u32>(node.literal))};
			}
			auto unary = [&]() {
				return node.operand_count >= 1 ? ResolveInvariantAffineFromHeader(
					program, header, node.operands[0], base_gpr, depth + 1) :
					Affine{};
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
						ResolveInvariantAffineFromHeader(program, header,
							node.operands[1], base_gpr, depth + 1) : Affine{};
				case Opcode::Add32:
				case Opcode::Add64:
				case Opcode::EffectiveAddress32:
				case Opcode::Sub32:
				case Opcode::Sub64:
				{
					if (node.operand_count != 2)
						return {};
					const Affine left = ResolveInvariantAffineFromHeader(program,
						header, node.operands[0], base_gpr, depth + 1);
					const Affine right = ResolveInvariantAffineFromHeader(program,
						header, node.operands[1], base_gpr, depth + 1);
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
					return offset >= INT32_MIN && offset <= INT32_MAX ?
						Affine{true, left.has_base || right.has_base, offset} :
						Affine{};
				}
				default:
					return {};
			}
		}

		bool MatchExactOuterControl(const Program& program,
			const SemanticIsland::Loop& loop, u8* counter_gpr, u8* bound_gpr,
			OuterScalarStorage* counter_storage,
			MemoryOperation* counter_initialization,
			MemoryOperation* counter_load, MemoryOperation* counter_store,
			OuterScalarStorage* bound_storage,
			MemoryOperation* bound_initialization,
			MemoryOperation* bound_load, ValueId* bound_initial_value,
			std::string* failure_detail)
		{
			auto Reject = [&](std::string detail) {
				if (failure_detail)
					*failure_detail = std::move(detail);
				return false;
			};
			if (!counter_gpr || !bound_gpr || !counter_storage ||
				!counter_initialization || !counter_load || !counter_store ||
				!bound_storage || !bound_initialization || !bound_load ||
				!bound_initial_value ||
				loop.header_block >= program.blocks.size() ||
				loop.latch_blocks.size() != 1)
			{
				return Reject("invalid outer-loop descriptor inputs");
			}
			const u32 latch = loop.latch_blocks.front();
			if (latch >= program.blocks.size())
				return Reject("outer latch is not the sole taken backedge");
			const Block& block = program.blocks[latch];
			if (block.terminator.kind != TerminatorKind::Branch ||
				block.terminator.taken.target_block != loop.header_block ||
				Contains(loop.blocks, block.terminator.not_taken.target_block))
			{
				return Reject("outer branch is not compare-not-equal against zero");
			}
			const Definition condition =
				StripScalarWrappers(program, block.terminator.condition);
			if (!condition.node ||
				condition.node->opcode != Opcode::CompareNotEqual64 ||
				condition.node->operand_count != 2)
			{
				return Reject("outer branch predicate is not signed-less-than");
			}
			ValueId predicate_value = INVALID_VALUE;
			if (IsZero(program, condition.node->operands[0]))
				predicate_value = condition.node->operands[1];
			else if (IsZero(program, condition.node->operands[1]))
				predicate_value = condition.node->operands[0];
			const Definition predicate =
				StripScalarWrappers(program, predicate_value);
			if (!predicate.node ||
				predicate.node->opcode != Opcode::CompareSignedLess64 ||
				predicate.node->operand_count != 2)
			{
				return false;
			}

			Definition counter_add{};
			ValueId counter_input = INVALID_VALUE;
			u32 predicate_counter_operand = UINT32_MAX;
			for (u32 operand = 0; operand < 2; operand++)
			{
				Definition candidate_add{};
				ValueId candidate_input = INVALID_VALUE;
				if (MatchAddOne(program, predicate.node->operands[operand],
						&candidate_add, &candidate_input))
				{
					if (predicate_counter_operand != UINT32_MAX)
						return Reject("outer predicate has multiple add-one operands");
					predicate_counter_operand = operand;
					counter_add = candidate_add;
					counter_input = candidate_input;
				}
			}
			if (predicate_counter_operand == UINT32_MAX)
				return Reject("outer predicate has no exact add-one operand");

			u32 bound_matches = 0;
			u32 bound_value_mask = 0;
			u32 bound_backedge_mask = 0;
			for (u32 bound = 1; bound < 32; bound++)
			{
				const bool value_matches = IsHeaderGprInvariant(program,
					loop.header_block,
					predicate.node->operands[1 - predicate_counter_operand], bound);
				const bool backedge_matches = IsHeaderGprInvariant(program,
					loop.header_block,
					block.terminator.taken.state.gpr[bound], bound);
				bound_value_mask |= value_matches ? (1u << bound) : 0;
				bound_backedge_mask |= backedge_matches ? (1u << bound) : 0;
				if (value_matches && backedge_matches)
				{
					*bound_gpr = static_cast<u8>(bound);
					bound_matches++;
				}
			}
			if (bound_matches > 1)
			{
				const Definition bound_definition = StripScalarWrappers(program,
					predicate.node->operands[1 - predicate_counter_operand]);
				return Reject("outer bound is not one invariant header GPR: value-mask=" +
					std::to_string(bound_value_mask) + " backedge-mask=" +
					std::to_string(bound_backedge_mask) + " definition=" +
					std::to_string(bound_definition.node ?
						static_cast<u32>(bound_definition.node->opcode) : UINT32_MAX) +
					":" + std::to_string(bound_definition.block) + ":" +
					std::to_string(bound_definition.node ?
						bound_definition.node->id : INVALID_VALUE));
			}
			if (bound_matches == 1)
			{
				*bound_storage = OuterScalarStorage::Gpr;
			}
			else
			{
				const ValueId bound_value =
					predicate.node->operands[1 - predicate_counter_operand];
				const Definition loaded_value = StripScalarWrappers(program, bound_value);
				if (!loaded_value.node ||
					loaded_value.node->opcode != Opcode::MemoryLoadValue ||
					loaded_value.node->operand_count != 1)
				{
					return Reject("outer bound is neither invariant GPR nor load value");
				}
				const Definition load = FindDefinition(program,
					loaded_value.node->operands[0]);
				if (!load.node || load.node->opcode != Opcode::MemoryLoad ||
					load.node->operand_count != 3 ||
					(static_cast<MemoryAccessKind>(load.node->immediate) !=
						MemoryAccessKind::LoadS32 &&
					 static_cast<MemoryAccessKind>(load.node->immediate) !=
						MemoryAccessKind::LoadU32))
				{
					return Reject("outer spill bound is not one signed/unsigned word load");
				}
				u8 spill_base_gpr = 0;
				s32 spill_offset = 0;
				u32 spill_address_matches = 0;
				for (u32 gpr = 1; gpr < 32; gpr++)
				{
					const Affine address = ResolveInvariantAffineFromHeader(program,
						loop.header_block, load.node->operands[1], gpr);
					if (address.valid && address.has_base &&
						address.offset >= INT32_MIN && address.offset <= INT32_MAX)
					{
						spill_base_gpr = static_cast<u8>(gpr);
						spill_offset = static_cast<s32>(address.offset);
						spill_address_matches++;
					}
				}
				if (spill_address_matches != 1)
					return Reject("outer spill bound address is not uniquely header-affine");

				for (u32 loop_block : loop.blocks)
				{
					for (const Node& node : program.blocks[loop_block].nodes)
					{
						if (node.opcode != Opcode::MemoryStore ||
							node.operand_count != 3 ||
							static_cast<MemoryAccessKind>(node.immediate) !=
								MemoryAccessKind::Store32)
						{
							continue;
						}
						const Affine address = ResolveInvariantAffineFromHeader(program,
							loop.header_block, node.operands[1],
							spill_base_gpr);
						if (address.valid && address.has_base &&
							address.offset == spill_offset)
						{
							return Reject("outer spill bound is written inside the loop");
						}
					}
				}

				const Node* initialization = nullptr;
				u32 initialization_block = INVALID_BLOCK;
				bool have_entry = false;
				bool entry_valid = true;
				for (u32 source = 0; source < program.blocks.size(); source++)
				{
					VisitInternalTransfers(program.blocks[source].terminator,
						[&](const Transfer& transfer, u8) {
							if (transfer.target_block != loop.header_block ||
								Contains(loop.blocks, source))
								return true;
							have_entry = true;
							const Node* found = nullptr;
							for (const Node& node : program.blocks[source].nodes)
							{
								if (node.opcode != Opcode::MemoryStore ||
									node.operand_count != 3 ||
									static_cast<MemoryAccessKind>(node.immediate) !=
										MemoryAccessKind::Store32)
								{
									continue;
								}
								const Affine address = ResolveAffineFromAnchor(program,
									node.operands[1],
									transfer.state.gpr[spill_base_gpr]);
								if (address.valid && address.has_base &&
									address.offset == spill_offset)
								{
									found = &node;
								}
							}
							if (!found || (initialization &&
								initialization->id != found->id))
							{
								entry_valid = false;
								return false;
							}
							initialization = found;
							initialization_block = source;
							return true;
						});
					if (!entry_valid)
						break;
				}
				if (!have_entry || !entry_valid || !initialization)
					return Reject("outer spill bound lacks one initialization on entry");

				u32 bound_result_matches = 0;
				for (u32 gpr = 1; gpr < 32; gpr++)
				{
					const Definition outgoing = StripScalarWrappers(program,
						block.terminator.taken.state.gpr[gpr]);
					if (outgoing.node && outgoing.node->id == loaded_value.node->id)
					{
						*bound_gpr = static_cast<u8>(gpr);
						bound_result_matches++;
					}
				}
				if (bound_result_matches != 1)
					return Reject("outer spill bound is not one backedge GPR");

				*bound_storage = OuterScalarStorage::Spill32;
				*bound_initialization = DescribeMemoryOperation(program,
					initialization_block, *initialization);
				*bound_load = DescribeMemoryOperation(program, load.block, *load.node);
				*bound_initial_value = initialization->operands[2];
				for (MemoryOperation* operation : {bound_initialization, bound_load})
				{
					operation->address_base_gpr = spill_base_gpr;
					operation->address_offset = spill_offset;
					operation->exact_header_affine_address = true;
				}
			}

			u32 counter_matches = 0;
			for (u32 counter = 1; counter < 32; counter++)
			{
				const Definition outgoing = StripScalarWrappers(program,
					block.terminator.taken.state.gpr[counter]);
				if (outgoing.node && outgoing.node->id == counter_add.node->id)
				{
					*counter_gpr = static_cast<u8>(counter);
					counter_matches++;
				}
			}
			if (counter_matches != 1)
				return Reject("outer add-one result is not one backedge GPR");

			bool direct_seed_zero = true;
			bool have_direct_seed = false;
			for (u32 source = 0; source < program.blocks.size(); source++)
			{
				VisitInternalTransfers(program.blocks[source].terminator,
					[&](const Transfer& transfer, u8) {
						if (transfer.target_block != loop.header_block ||
							Contains(loop.blocks, source))
							return true;
						s64 seed = 0;
						have_direct_seed = true;
						if (!ResolveConstant(program, source,
								transfer.state.gpr[*counter_gpr], &seed) || seed != 0)
						{
							direct_seed_zero = false;
							return false;
						}
						return true;
					});
			}
			if (have_direct_seed && direct_seed_zero &&
				IsHeaderGprInvariant(program, loop.header_block, counter_input,
					*counter_gpr))
			{
				*counter_storage = OuterScalarStorage::Gpr;
				return true;
			}

			const Definition loaded_value = StripScalarWrappers(program, counter_input);
			if (!loaded_value.node ||
				loaded_value.node->opcode != Opcode::MemoryLoadValue ||
				loaded_value.node->operand_count != 1)
			{
				return Reject("outer counter is neither zero-seeded GPR nor load value");
			}
			const Definition load = FindDefinition(program,
				loaded_value.node->operands[0]);
			if (!load.node || load.node->opcode != Opcode::MemoryLoad ||
				load.node->operand_count != 3 ||
				(static_cast<MemoryAccessKind>(load.node->immediate) !=
					MemoryAccessKind::LoadS32 &&
				 static_cast<MemoryAccessKind>(load.node->immediate) !=
					MemoryAccessKind::LoadU32))
			{
				return Reject("outer spill counter is not one signed/unsigned word load");
			}
			u8 spill_base_gpr = 0;
			s32 spill_offset = 0;
			u32 spill_address_matches = 0;
			for (u32 gpr = 1; gpr < 32; gpr++)
			{
				const Affine address = ResolveInvariantAffineFromHeader(program,
					loop.header_block, load.node->operands[1], gpr);
				if (address.valid && address.has_base &&
					address.offset >= INT32_MIN && address.offset <= INT32_MAX)
				{
					spill_base_gpr = static_cast<u8>(gpr);
					spill_offset = static_cast<s32>(address.offset);
					spill_address_matches++;
				}
			}
			if (spill_address_matches != 1)
				return Reject("outer spill address is not uniquely header-affine");

			const Node* update_store = nullptr;
			for (const Node& node : block.nodes)
			{
				if (node.opcode != Opcode::MemoryStore || node.operand_count != 3 ||
					static_cast<MemoryAccessKind>(node.immediate) !=
						MemoryAccessKind::Store32)
				{
					continue;
				}
				const Definition stored = StripScalarWrappers(program, node.operands[2]);
				const Affine address = ResolveInvariantAffineFromHeader(program,
					loop.header_block, node.operands[1], spill_base_gpr);
				if (stored.node && stored.node->id == counter_add.node->id &&
					address.valid && address.has_base && address.offset == spill_offset)
				{
					if (update_store)
						return Reject("outer counter has multiple matching spill stores");
					update_store = &node;
				}
			}
			if (!update_store)
				return Reject("outer spill update is not stored to its load address");

			const Node* zero_initialization = nullptr;
			u32 zero_initialization_block = INVALID_BLOCK;
			bool have_entry = false;
			bool entry_valid = true;
			for (u32 source = 0; source < program.blocks.size(); source++)
			{
				VisitInternalTransfers(program.blocks[source].terminator,
					[&](const Transfer& transfer, u8) {
						if (transfer.target_block != loop.header_block ||
							Contains(loop.blocks, source))
							return true;
						have_entry = true;
						const Node* found = nullptr;
						for (const Node& node : program.blocks[source].nodes)
						{
							if (node.opcode != Opcode::MemoryStore ||
								node.operand_count != 3 ||
								static_cast<MemoryAccessKind>(node.immediate) !=
									MemoryAccessKind::Store32 ||
								!IsZero(program, node.operands[2]))
							{
								continue;
							}
							const Affine address = ResolveAffineFromAnchor(program,
								node.operands[1],
								transfer.state.gpr[spill_base_gpr]);
							if (address.valid && address.has_base &&
								address.offset == spill_offset)
							{
								found = &node;
							}
						}
						if (!found || (zero_initialization &&
							zero_initialization->id != found->id))
						{
							entry_valid = false;
							return false;
						}
						zero_initialization = found;
						zero_initialization_block = source;
						return true;
					});
				if (!entry_valid)
					break;
			}
			if (!have_entry || !entry_valid || !zero_initialization)
				return Reject("outer spill counter lacks one zero initialization on entry");

			*counter_storage = OuterScalarStorage::Spill32;
			*counter_initialization = DescribeMemoryOperation(program,
				zero_initialization_block, *zero_initialization);
			*counter_load = DescribeMemoryOperation(program, load.block, *load.node);
			*counter_store = DescribeMemoryOperation(program, latch, *update_store);
			for (MemoryOperation* operation : {counter_initialization,
				counter_load, counter_store})
			{
				operation->address_base_gpr = spill_base_gpr;
				operation->address_offset = spill_offset;
				operation->exact_header_affine_address = true;
			}
			return true;
		}

		bool ResolveUniqueHeaderAffineAddress(const Program& program, u32 header,
			u32 block, ValueId address, u8* base_gpr, s32* offset)
		{
			if (!base_gpr || !offset)
				return false;
			u32 matches = 0;
			for (u32 gpr = 1; gpr < 32; gpr++)
			{
				const Affine affine = ResolveAffineFromHeader(program, header, block,
					address, gpr);
				if (!affine.valid || !affine.has_base ||
					affine.offset < INT32_MIN || affine.offset > INT32_MAX)
				{
					continue;
				}
				*base_gpr = static_cast<u8>(gpr);
				*offset = static_cast<s32>(affine.offset);
				matches++;
			}
			return matches == 1;
		}

		bool ExactStreamAddressShape(Candidate* candidate)
		{
			if (!candidate)
				return false;
			std::array<std::vector<s32>, 32> input_offsets;
			std::array<std::vector<s32>, 32> output_offsets;
			for (const MemoryOperation& operation : candidate->inner_input_bytes)
			{
				if (!operation.exact_header_affine_address ||
					operation.address_base_gpr == 0)
					return false;
				input_offsets[operation.address_base_gpr].push_back(
					operation.address_offset);
			}
			for (const MemoryOperation& operation : candidate->inner_outputs)
			{
				if (!operation.exact_header_affine_address ||
					operation.address_base_gpr == 0)
					return false;
				output_offsets[operation.address_base_gpr].push_back(
					operation.address_offset);
			}
			std::vector<std::vector<s32>> inputs;
			std::vector<std::vector<s32>> outputs;
			for (u32 gpr = 1; gpr < 32; gpr++)
			{
				if (!input_offsets[gpr].empty())
				{
					std::sort(input_offsets[gpr].begin(), input_offsets[gpr].end());
					inputs.push_back(input_offsets[gpr]);
					candidate->inner_input_base_gprs.push_back(
						static_cast<u8>(gpr));
				}
				if (!output_offsets[gpr].empty())
				{
					std::sort(output_offsets[gpr].begin(), output_offsets[gpr].end());
					outputs.push_back(output_offsets[gpr]);
					candidate->inner_output_base_gprs.push_back(
						static_cast<u8>(gpr));
				}
			}
			std::sort(inputs.begin(), inputs.end());
			std::sort(outputs.begin(), outputs.end());
			const bool input_shape =
				(inputs == std::vector<std::vector<s32>>{{0, 18}}) ||
				(inputs == std::vector<std::vector<s32>>{{0}, {0}});
			return input_shape &&
				outputs == std::vector<std::vector<s32>>{{0, 2}, {0, 2}};
		}

		bool MatchHeaderPointerRecurrence(const Program& program,
			const SemanticIsland::Loop& loop, u32 gpr, s64 expected_stride)
		{
			if (loop.latch_blocks.size() != 1 ||
				loop.latch_blocks.front() >= program.blocks.size())
			{
				return false;
			}
			const u32 latch = loop.latch_blocks.front();
			const Block& block = program.blocks[latch];
			const Transfer* backedge = nullptr;
			VisitInternalTransfers(block.terminator,
				[&](const Transfer& transfer, u8) {
					if (transfer.target_block == loop.header_block)
						backedge = &transfer;
					return true;
				});
			if (!backedge)
				return false;
			const Affine recurrence = ResolveAffineFromHeader(program,
				loop.header_block, latch, backedge->state.gpr[gpr], gpr);
			return recurrence.valid && recurrence.has_base &&
				recurrence.offset == expected_stride;
		}

		bool HeaderGprsHaveEqualLowAddress(const Program& program,
			const SemanticIsland::Loop& loop, u32 left_gpr, u32 right_gpr)
		{
			if (left_gpr == right_gpr)
				return true;
			if (loop.header_block >= program.blocks.size())
				return false;

			bool have_incoming = false;
			bool equal = true;
			for (u32 source = 0; source < program.blocks.size() && equal; source++)
			{
				VisitInternalTransfers(program.blocks[source].terminator,
					[&](const Transfer& transfer, u8) {
						if (transfer.target_block != loop.header_block)
							return true;
						have_incoming = true;
						const Definition left = StripScalarWrappers(program,
							transfer.state.gpr[left_gpr]);
						const Definition right = StripScalarWrappers(program,
							transfer.state.gpr[right_gpr]);
						if (!left.node || !right.node)
						{
							equal = false;
							return false;
						}
						if (left.node->id == right.node->id)
							return true;
						const Affine left_from_right = ResolveAffineFromAnchor(program,
							left.node->id, right.node->id);
						const Affine right_from_left = ResolveAffineFromAnchor(program,
							right.node->id, left.node->id);
						equal = (left_from_right.valid && left_from_right.has_base &&
							left_from_right.offset == 0) ||
							(right_from_left.valid && right_from_left.has_base &&
							 right_from_left.offset == 0);
						return equal;
					});
			}
			return equal && have_incoming;
		}

		bool BuildCountedLoopExitAnchors(const Program& program,
			const SemanticIsland::Loop& containing_loop,
			const SemanticIsland::Loop& counted_loop, u32 state_gpr, u32 base_gpr,
			s64 iteration_stride, u32 iteration_count,
			const AffineAnchors& header_anchors, AffineAnchors* exit_anchors)
		{
			if (!exit_anchors ||
				containing_loop.header_block >= program.blocks.size() ||
				counted_loop.header_block >= program.blocks.size() ||
				counted_loop.latch_blocks.size() != 1)
			{
				return false;
			}

			auto same_affine = [](const Affine& left, const Affine& right) {
				return left.valid == right.valid &&
					left.has_base == right.has_base && left.offset == right.offset;
			};
			auto merge_affine = [&](const Affine& incoming, Affine* common,
				bool* have_common) {
				if (!incoming.valid || (*have_common &&
					!same_affine(incoming, *common)))
				{
					return false;
				}
				*common = incoming;
				*have_common = true;
				return true;
			};

			// Prove the value entering the counted inner loop relative to this
			// outer iteration.  Inner backedges are excluded here; their one-step
			// recurrence was independently proven by MatchHeaderPointerRecurrence.
			Affine entry{};
			bool have_entry = false;
			bool valid = true;
			for (u32 source = 0; source < program.blocks.size() && valid; source++)
			{
				VisitInternalTransfers(program.blocks[source].terminator,
					[&](const Transfer& transfer, u8) {
						if (transfer.target_block != counted_loop.header_block ||
							Contains(counted_loop.blocks, source))
						{
							return true;
						}
						if (!Contains(containing_loop.blocks, source))
						{
							valid = false;
							return false;
						}
						const Affine incoming = ResolveAffineFromHeaderAnchored(program,
							containing_loop.header_block, source,
							transfer.state.gpr[state_gpr], base_gpr, header_anchors);
						valid = merge_affine(incoming, &entry, &have_entry);
						return valid;
					});
			}
			if (!valid || !have_entry || !entry.has_base)
				return false;

			const s64 dynamic_offset = entry.offset +
				iteration_stride * static_cast<s64>(iteration_count);
			if (dynamic_offset < INT32_MIN || dynamic_offset > INT32_MAX)
				return false;
			const Affine dynamic_exit{true, true, dynamic_offset};

			// A verifier transfer describes one syntactic trip through a loop.  For
			// a proven counted loop, replace only its normal exit parameter with the
			// closed-form dynamic value.  Every incoming edge to that parameter must
			// independently agree, so this cannot conceal a bypass or early exit.
			bool have_exit = false;
			const u32 latch = counted_loop.latch_blocks.front();
			for (u32 source : counted_loop.blocks)
			{
				if (!valid || source >= program.blocks.size())
					return false;
				VisitInternalTransfers(program.blocks[source].terminator,
					[&](const Transfer& transfer, u8) {
						if (Contains(counted_loop.blocks, transfer.target_block))
							return true;
						if (source != latch || transfer.target_block >= program.blocks.size() ||
							!Contains(containing_loop.blocks, transfer.target_block))
						{
							valid = false;
							return false;
						}
						const Affine syntactic_exit = ResolveAffineFromHeader(program,
							counted_loop.header_block, source,
							transfer.state.gpr[state_gpr], state_gpr);
						if (!syntactic_exit.valid || !syntactic_exit.has_base ||
							syntactic_exit.offset != iteration_stride)
						{
							valid = false;
							return false;
						}

						const ValueId parameter = program.blocks[transfer.target_block]
							.parameters.gpr[state_gpr];
						if (parameter == INVALID_VALUE)
						{
							valid = false;
							return false;
						}
						Affine common{};
						bool have_common = false;
						for (u32 predecessor = 0;
							predecessor < program.blocks.size() && valid; predecessor++)
						{
							VisitInternalTransfers(program.blocks[predecessor].terminator,
								[&](const Transfer& incoming, u8) {
									if (incoming.target_block != transfer.target_block)
										return true;
									Affine value{};
									if (Contains(counted_loop.blocks, predecessor))
									{
										const Affine one_trip = ResolveAffineFromHeader(program,
											counted_loop.header_block, predecessor,
											incoming.state.gpr[state_gpr], state_gpr);
										if (!one_trip.valid || !one_trip.has_base ||
											one_trip.offset != iteration_stride)
										{
											valid = false;
											return false;
										}
										value = dynamic_exit;
									}
									else
									{
										value = ResolveAffineFromHeaderAnchored(program,
											containing_loop.header_block, predecessor,
											incoming.state.gpr[state_gpr], base_gpr,
											header_anchors);
									}
									valid = merge_affine(value, &common, &have_common);
									return valid;
								});
						}
						if (!valid || !have_common || !same_affine(common, dynamic_exit))
						{
							valid = false;
							return false;
						}
						const auto existing = std::find_if(exit_anchors->begin(),
							exit_anchors->end(),
							[&](const auto& item) { return item.first == parameter; });
						if (existing != exit_anchors->end())
						{
							if (!same_affine(existing->second, dynamic_exit))
							{
								valid = false;
								return false;
							}
						}
						else
						{
							exit_anchors->emplace_back(parameter, dynamic_exit);
						}
						have_exit = true;
						return true;
					});
			}
			return valid && have_exit;
		}

		struct SymbolicExpression
		{
			bool valid = false;
			bool constant = false;
			u32 constant_value = 0;
			bool header_gpr = false;
			u8 gpr = 0;
			bool addition = false;
			std::string left;
			std::string right;
			std::string key;
		};

		SymbolicExpression ConstantExpression(u32 value)
		{
			return {true, true, value, false, 0, false, {}, {},
				"c:" + std::to_string(value)};
		}

		SymbolicExpression ResolveSymbolicExpression(const Program& program,
			u32 header, ValueId value, u32 depth = 0)
		{
			if (depth > 128)
				return {.key = "depth:" + std::to_string(value)};
			const Definition definition = FindDefinition(program, value);
			if (!definition.node)
				return {.key = "definition:" + std::to_string(value)};
			const Node& node = *definition.node;
			if (node.opcode == Opcode::Parameter)
			{
				u32 gpr = 0;
				if (!ParameterGpr(program, definition, &gpr))
					return {.key = "parameter-domain:" + std::to_string(value)};
				if (gpr == 0)
					return ConstantExpression(0);
				if (definition.block == header)
				{
					SymbolicExpression expression{};
					expression.valid = true;
					expression.header_gpr = true;
					expression.gpr = static_cast<u8>(gpr);
					expression.key = "g:" + std::to_string(gpr);
					return expression;
				}
				SymbolicExpression common{};
				bool have_common = false;
				for (u32 source = 0; source < program.blocks.size(); source++)
				{
					VisitInternalTransfers(program.blocks[source].terminator,
						[&](const Transfer& transfer, u8) {
							if (transfer.target_block != definition.block)
								return true;
							const SymbolicExpression incoming =
								ResolveSymbolicExpression(program, header,
									transfer.state.gpr[gpr], depth + 1);
							if (!incoming.valid ||
								(have_common && incoming.key != common.key))
							{
								common = incoming.valid ? SymbolicExpression{} : incoming;
								if (common.key.empty())
									common.key = "parameter-merge:" +
										std::to_string(value);
								have_common = true;
								return false;
							}
							common = incoming;
							have_common = true;
							return true;
						});
					if (have_common && !common.valid)
						return common;
				}
				return have_common ? common :
					SymbolicExpression{.key = "parameter-incoming:" +
						std::to_string(value)};
			}
			if (node.opcode == Opcode::ConstantI1 ||
				node.opcode == Opcode::ConstantI32 ||
				node.opcode == Opcode::ConstantI64 ||
				node.opcode == Opcode::ConstantAddress)
			{
				return ConstantExpression(static_cast<u32>(node.literal));
			}
			auto unary = [&]() {
				return node.operand_count >= 1 ?
					ResolveSymbolicExpression(program, header, node.operands[0],
						depth + 1) : SymbolicExpression{};
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
						ResolveSymbolicExpression(program, header, node.operands[1],
							depth + 1) : SymbolicExpression{};
				case Opcode::MemoryLoadValue:
				{
					if (node.operand_count != 1)
						return {.key = "load-value-arity:" + std::to_string(value)};
					SymbolicExpression expression{};
					expression.valid = true;
					expression.key = "m:" + std::to_string(node.operands[0]);
					return expression;
				}
				case Opcode::Add32:
				case Opcode::Add64:
				case Opcode::EffectiveAddress32:
				{
					if (node.operand_count != 2)
						return {.key = "add-arity:" + std::to_string(value)};
					SymbolicExpression left = ResolveSymbolicExpression(program,
						header, node.operands[0], depth + 1);
					SymbolicExpression right = ResolveSymbolicExpression(program,
						header, node.operands[1], depth + 1);
					if (!left.valid || !right.valid)
						return !left.valid ? left : right;
					if (left.constant && right.constant)
						return ConstantExpression(left.constant_value + right.constant_value);
					if (left.constant && left.constant_value == 0)
						return right;
					if (right.constant && right.constant_value == 0)
						return left;
					if (right.key < left.key)
						std::swap(left, right);
					SymbolicExpression expression{};
					expression.valid = true;
					expression.addition = true;
					expression.left = left.key;
					expression.right = right.key;
					expression.key = "a(" + left.key + "," + right.key + ")";
					return expression;
				}
				case Opcode::And32:
				case Opcode::And64:
				{
					if (node.operand_count != 2)
						return {.key = "and-arity:" + std::to_string(value)};
					SymbolicExpression left = ResolveSymbolicExpression(program,
						header, node.operands[0], depth + 1);
					SymbolicExpression right = ResolveSymbolicExpression(program,
						header, node.operands[1], depth + 1);
					if (!left.valid || !right.valid)
						return !left.valid ? left : right;
					if (left.constant && right.constant)
						return ConstantExpression(left.constant_value & right.constant_value);
					// The integer implementation sign-extends each LBU through
					// SLL 24/SRA 24 before selecting its low nibble. Under mask 15
					// those upper replicated bits are unobservable, so retain the
					// originating verifier-owned byte-load expression.
					auto strip_signed_byte_under_low_nibble =
						[](SymbolicExpression* expression) {
							constexpr std::string_view PREFIX = "r:24(l:24(";
							constexpr std::string_view SUFFIX = "))";
							if (!expression || expression->key.size() <=
								PREFIX.size() + SUFFIX.size() ||
								expression->key.compare(0, PREFIX.size(), PREFIX) != 0 ||
								expression->key.compare(expression->key.size() -
									SUFFIX.size(), SUFFIX.size(), SUFFIX) != 0)
							{
								return;
							}
							expression->key = expression->key.substr(PREFIX.size(),
								expression->key.size() - PREFIX.size() - SUFFIX.size());
						};
					if (left.constant && left.constant_value == 15)
						strip_signed_byte_under_low_nibble(&right);
					if (right.constant && right.constant_value == 15)
						strip_signed_byte_under_low_nibble(&left);
					if (right.key < left.key)
						std::swap(left, right);
					SymbolicExpression expression{};
					expression.valid = true;
					expression.key = "n(" + left.key + "," + right.key + ")";
					return expression;
				}
				case Opcode::ShiftLeft32:
				{
					SymbolicExpression input = unary();
					if (!input.valid || node.immediate >= 32)
						return !input.valid ? input :
							SymbolicExpression{.key = "shift-left:" +
								std::to_string(value)};
					if (input.constant)
						return ConstantExpression(input.constant_value << node.immediate);
					SymbolicExpression expression{};
					expression.valid = true;
					expression.key = "l:" + std::to_string(node.immediate) +
						"(" + input.key + ")";
					return expression;
				}
				case Opcode::ShiftRightArithmetic32:
				{
					SymbolicExpression input = unary();
					if (!input.valid || node.immediate >= 32)
						return !input.valid ? input :
							SymbolicExpression{.key = "shift-right:" +
								std::to_string(value)};
					if (input.constant)
					{
						return ConstantExpression(static_cast<u32>(
							static_cast<s32>(input.constant_value) >> node.immediate));
					}
					SymbolicExpression expression{};
					expression.valid = true;
					expression.key = "r:" + std::to_string(node.immediate) +
						"(" + input.key + ")";
					return expression;
				}
				default:
					return {.key = "opcode:" +
						std::to_string(static_cast<u32>(node.opcode)) + ":" +
						std::to_string(value)};
			}
		}

		std::string ExpectedScaledInputKey(ValueId operation, bool masked)
		{
			const std::string input = "m:" + std::to_string(operation);
			if (!masked)
				return "l:2(" + input + ")";
			const std::string constant = "c:15";
			const std::string conjunction = constant < input ?
				"n(" + constant + "," + input + ")" :
				"n(" + input + "," + constant + ")";
			return "l:2(" + conjunction + ")";
		}

		bool DecodeSymbolicBase(const std::string& key, PredictorLookup* lookup)
		{
			if (!lookup)
				return false;
			if (key.rfind("g:", 0) == 0)
			{
				char* end = nullptr;
				const unsigned long value = std::strtoul(key.c_str() + 2, &end, 10);
				if (!end || *end != '\0' || value == 0 || value >= 32)
					return false;
				lookup->base_gpr = static_cast<u8>(value);
				return true;
			}
			if (key.rfind("c:", 0) == 0)
			{
				char* end = nullptr;
				const unsigned long value = std::strtoul(key.c_str() + 2, &end, 10);
				if (!end || *end != '\0' || value > UINT32_MAX)
					return false;
				lookup->base_is_immediate = true;
				lookup->base_immediate = static_cast<u32>(value);
				return true;
			}
			return false;
		}

		bool BuildPredictorLookup(const Program& program,
			const SemanticIsland::Loop& inner, const MemoryOperation& load,
			const std::vector<MemoryOperation>& inputs, bool masked,
			PredictorLookup* lookup)
		{
			if (!lookup || load.block >= program.blocks.size() ||
				load.node_ordinal >= program.blocks[load.block].nodes.size())
			{
				return false;
			}
			const Node& operation =
				program.blocks[load.block].nodes[load.node_ordinal];
			if (operation.id != load.operation || operation.opcode != Opcode::MemoryLoad ||
				operation.operand_count != 3)
			{
				return false;
			}
			const SymbolicExpression address = ResolveSymbolicExpression(program,
				inner.header_block, operation.operands[1]);
			if (!address.valid || !address.addition)
				return false;
			for (u32 input_index = 0; input_index < inputs.size(); input_index++)
			{
				const std::string index =
					ExpectedScaledInputKey(inputs[input_index].operation, masked);
				const std::string* base = nullptr;
				if (address.left == index)
					base = &address.right;
				else if (address.right == index)
					base = &address.left;
				if (!base)
					continue;
				PredictorLookup candidate{};
				candidate.load_operation = load.operation;
				candidate.input_operation = inputs[input_index].operation;
				candidate.low_nibble_mask = masked;
				candidate.index_shift = 2;
				if (!DecodeSymbolicBase(*base, &candidate))
					continue;
				*lookup = candidate;
				return true;
			}
			return false;
		}

		bool ExactPredictorLookupShape(const Candidate& candidate)
		{
			if (candidate.inner_input_bytes.size() != 2 ||
				candidate.predictor_lookups.size() !=
					(candidate.arithmetic_mode == ArithmeticMode::IntegerFixedPoint ? 2 : 4))
			{
				return false;
			}
			for (const MemoryOperation& input : candidate.inner_input_bytes)
			{
				u32 uses = 0;
				for (const PredictorLookup& lookup : candidate.predictor_lookups)
					uses += lookup.input_operation == input.operation;
				if (uses != (candidate.arithmetic_mode ==
						ArithmeticMode::IntegerFixedPoint ? 1u : 2u))
				{
					return false;
				}
			}
			auto same_base = [](const PredictorLookup& left,
				const PredictorLookup& right) {
				return left.base_is_immediate == right.base_is_immediate &&
					(left.base_is_immediate ?
						left.base_immediate == right.base_immediate :
						(left.base_gpr == right.base_gpr &&
						 left.base_offset == right.base_offset));
			};
			std::vector<const PredictorLookup*> bases;
			for (const PredictorLookup& lookup : candidate.predictor_lookups)
			{
				if (lookup.index_shift != 2 ||
					lookup.low_nibble_mask != (candidate.arithmetic_mode ==
						ArithmeticMode::IntegerFixedPoint))
				{
					return false;
				}
				if (std::none_of(bases.begin(), bases.end(), [&](const auto* base) {
					return same_base(*base, lookup);
				}))
				{
					bases.push_back(&lookup);
				}
			}
			if (bases.size() != (candidate.arithmetic_mode ==
					ArithmeticMode::IntegerFixedPoint ? 1u : 2u))
			{
				return false;
			}
			if (candidate.arithmetic_mode == ArithmeticMode::Cop1Single)
			{
				for (const MemoryOperation& input : candidate.inner_input_bytes)
					for (const PredictorLookup* base : bases)
						if (std::none_of(candidate.predictor_lookups.begin(),
							candidate.predictor_lookups.end(), [&](const auto& lookup) {
								return lookup.input_operation == input.operation &&
									same_base(*base, lookup);
							}))
							return false;
			}
			return true;
		}

		bool BuildExactInnerMemorySchedule(const Program& program,
			const SemanticIsland::Loop& loop,
			std::vector<MemoryAccessKind>* schedule,
			std::vector<ValueId>* operation_order, u32* path_count)
		{
			if (!schedule || !operation_order || !path_count ||
				loop.header_block >= program.blocks.size() ||
				loop.latch_blocks.empty())
			{
				return false;
			}
			schedule->clear();
			operation_order->clear();
			*path_count = 0;
			std::vector<u8> visited_union(program.blocks.size(), 0);
			std::vector<u8> active(program.blocks.size(), 0);
			std::vector<MemoryAccessKind> path;
			std::vector<ValueId> operation_path;
			bool valid = true;
			auto is_latch = [&](u32 block) {
				return std::find(loop.latch_blocks.begin(), loop.latch_blocks.end(),
					block) != loop.latch_blocks.end();
			};
			auto visit = [&](auto&& self, u32 block_index) -> void {
				if (!valid || block_index >= program.blocks.size() ||
					!Contains(loop.blocks, block_index) || active[block_index] ||
					*path_count >= 256)
				{
					valid = false;
					return;
				}
				active[block_index] = 1;
				visited_union[block_index] = 1;
				const size_t path_start = path.size();
				const size_t operation_path_start = operation_path.size();
				const Block& block = program.blocks[block_index];
				for (const Node& node : block.nodes)
				{
					if (node.opcode == Opcode::MemoryLoad ||
						node.opcode == Opcode::MemoryStore)
					{
						path.push_back(static_cast<MemoryAccessKind>(node.immediate));
						operation_path.push_back(node.id);
					}
				}
				if (is_latch(block_index))
				{
					if (*path_count == 0)
					{
						*schedule = path;
						*operation_order = operation_path;
					}
					else if (*schedule != path || *operation_order != operation_path)
						valid = false;
					(*path_count)++;
				}
				else
				{
					u32 successors = 0;
					VisitInternalTransfers(block.terminator,
						[&](const Transfer& transfer, u8) {
							if (transfer.target_block == loop.header_block)
								return true;
							if (!Contains(loop.blocks, transfer.target_block))
							{
								valid = false;
								return false;
							}
							successors++;
							self(self, transfer.target_block);
							return valid;
						});
					if (successors == 0)
						valid = false;
				}
				path.resize(path_start);
				operation_path.resize(operation_path_start);
				active[block_index] = 0;
			};
			visit(visit, loop.header_block);
			if (!valid || *path_count == 0)
				return false;
			for (u32 block : loop.blocks)
				if (block >= visited_union.size() || !visited_union[block])
					return false;
			return true;
		}
	} // namespace

	BuildResult BuildCandidate(const RegionIR::Program& program)
	{
		const VerifyResult verified = Verify(program);
		if (!verified)
		{
			return Fail(Failure::InvalidProgram, verified.detail,
				verified.block, verified.node);
		}
		const SemanticIsland::BuildResult island = SemanticIsland::Build(program);
		if (!island)
		{
			return Fail(Failure::InvalidIsland, island.detail,
				island.block, island.related_block);
		}
		if (island.plan.loops.size() != 2 ||
			island.plan.maximum_loop_depth != 2 ||
			island.plan.direct_calls != 0)
		{
			return Fail(Failure::UnsupportedTopology,
				"framed predictor requires exactly one nested loop pair");
		}

		u32 inner_loop = INVALID_BLOCK;
		u32 outer_loop = INVALID_BLOCK;
		for (u32 index = 0; index < island.plan.loops.size(); index++)
		{
			const SemanticIsland::Loop& loop = island.plan.loops[index];
			if (loop.depth == 2)
				inner_loop = index;
			else if (loop.depth == 1)
				outer_loop = index;
		}
		if (inner_loop == INVALID_BLOCK || outer_loop == INVALID_BLOCK ||
			island.plan.loops[inner_loop].parent_loop != outer_loop ||
			island.plan.loops[inner_loop].latch_blocks.size() != 1 ||
			island.plan.loops[outer_loop].latch_blocks.size() != 1)
		{
			return Fail(Failure::UnsupportedTopology,
				"nested loops do not have one exact parent and one latch each");
		}

		for (const SemanticIsland::Exit& exit : island.plan.exits)
		{
			if (exit.exit_class != SemanticIsland::ExitClass::Completion &&
				exit.exit_class != SemanticIsland::ExitClass::MemoryFallback)
			{
				return Fail(Failure::UnsupportedExitContract,
					"candidate has a non-memory cold observer", exit.block);
			}
		}
		if (island.plan.completion_exits != 1 ||
			!island.plan.completion_reachable_from_repeated_core)
		{
			return Fail(Failure::UnsupportedExitContract,
				"candidate lacks one exact completion after its repeated core");
		}
		for (const SemanticIsland::InternalEdge& edge : island.plan.internal_edges)
		{
			if (edge.cycle_commit_deferred)
			{
				return Fail(Failure::UnsupportedExitContract,
					"candidate has a deferred-cycle internal timing seam",
					edge.source_block);
			}
		}

		const SemanticIsland::Loop& inner = island.plan.loops[inner_loop];
		const SemanticIsland::Loop& outer = island.plan.loops[outer_loop];
		std::vector<u32> outer_only;
		for (u32 block : outer.blocks)
			if (!Contains(inner.blocks, block))
				outer_only.push_back(block);
		if (!HasDirectAddConstant(program, outer_only, 18))
		{
			return Fail(Failure::UnsupportedFrameControl,
				"candidate lacks a verified 18-byte frame advance");
		}
		u8 inner_counter_gpr = 0;
		s32 inner_counter_stride = 0;
		u32 inner_iterations = 0;
		if (!MatchExactInnerControl(program, inner, &inner_counter_gpr,
				&inner_counter_stride, &inner_iterations))
		{
			return Fail(Failure::UnsupportedFrameControl,
				"inner loop is not the exact 30,-2,signed-nonnegative contract");
		}
		u8 outer_counter_gpr = 0;
		u8 outer_bound_gpr = 0;
		OuterScalarStorage outer_counter_storage = OuterScalarStorage::Gpr;
		MemoryOperation outer_counter_initialization{};
		MemoryOperation outer_counter_load{};
		MemoryOperation outer_counter_store{};
		OuterScalarStorage outer_bound_storage = OuterScalarStorage::Gpr;
		MemoryOperation outer_bound_initialization{};
		MemoryOperation outer_bound_load{};
		ValueId outer_bound_initial_value = INVALID_VALUE;
		std::string outer_control_failure;
		if (!MatchExactOuterControl(program, outer, &outer_counter_gpr,
				&outer_bound_gpr, &outer_counter_storage,
				&outer_counter_initialization, &outer_counter_load,
				&outer_counter_store, &outer_bound_storage,
				&outer_bound_initialization, &outer_bound_load,
				&outer_bound_initial_value, &outer_control_failure))
		{
			return Fail(Failure::UnsupportedFrameControl,
				"outer loop is not the exact stereo-pair control contract: " +
					outer_control_failure);
		}

		const Counts inner_counts = CountNodes(program, inner.blocks);
		if (MemoryCount(inner_counts, MemoryAccessKind::LoadU8) != 2 ||
			MemoryCount(inner_counts, MemoryAccessKind::Store16) != 4 ||
			inner_counts.stores != 4)
		{
			return Fail(Failure::UnsupportedInnerMemory,
				"inner loop is not two byte inputs and four halfword outputs");
		}
		const Counts outer_counts = CountNodes(program, outer_only);
		if (MemoryCount(outer_counts, MemoryAccessKind::LoadU8) < 2 ||
			MemoryCount(outer_counts, MemoryAccessKind::LoadU16) < 2)
		{
			return Fail(Failure::UnsupportedOuterMemory,
				"outer loop lacks two channel headers and marker bytes");
		}
		std::vector<u32> all_blocks(program.blocks.size());
		for (u32 index = 0; index < all_blocks.size(); index++)
			all_blocks[index] = index;
		const Counts all_counts = CountNodes(program, all_blocks);
		if (MemoryCount(all_counts, MemoryAccessKind::LoadS16) < 4 ||
			MemoryCount(all_counts, MemoryAccessKind::Store16) < 8)
		{
			return Fail(Failure::UnsupportedOuterMemory,
				"candidate lacks four signed histories and their final publication");
		}

		ArithmeticMode mode{};
		if (inner_counts.cop1_arithmetic == 0 &&
			MemoryCount(inner_counts, MemoryAccessKind::LoadS32) >= 2 &&
			inner_counts.integer_multiply >= 4 &&
			inner_counts.integer_shift_right >= 4)
		{
			mode = ArithmeticMode::IntegerFixedPoint;
		}
		else if (MemoryCount(inner_counts, MemoryAccessKind::LoadF32Bits) >= 4 &&
			inner_counts.cop1_multiply >= 4 && inner_counts.cop1_add >= 4 &&
			inner_counts.cop1_clamp >= 4 && inner_counts.cop1_convert_word >= 4)
		{
			mode = ArithmeticMode::Cop1Single;
		}
		else
		{
			return Fail(Failure::UnsupportedArithmetic,
				"inner recurrence is neither exact integer nor explicit COP1 candidate");
		}
		std::vector<MemoryAccessKind> inner_memory_schedule;
		std::vector<ValueId> inner_memory_order;
		u32 equivalent_inner_schedule_paths = 0;
		if (!BuildExactInnerMemorySchedule(program, inner, &inner_memory_schedule,
				&inner_memory_order, &equivalent_inner_schedule_paths))
		{
			return Fail(Failure::UnsupportedInnerMemory,
				"inner loop memory order differs across control-flow paths");
		}
		const std::vector<MemoryAccessKind> expected_memory_schedule =
			mode == ArithmeticMode::IntegerFixedPoint ?
				std::vector<MemoryAccessKind>{MemoryAccessKind::LoadU8,
					MemoryAccessKind::LoadU8, MemoryAccessKind::LoadS32,
					MemoryAccessKind::Store16, MemoryAccessKind::Store16,
					MemoryAccessKind::LoadS32, MemoryAccessKind::Store16,
					MemoryAccessKind::Store16} :
				std::vector<MemoryAccessKind>{MemoryAccessKind::LoadU8,
					MemoryAccessKind::LoadU8, MemoryAccessKind::LoadF32Bits,
					MemoryAccessKind::LoadF32Bits, MemoryAccessKind::LoadF32Bits,
					MemoryAccessKind::LoadF32Bits, MemoryAccessKind::Store16,
					MemoryAccessKind::Store16, MemoryAccessKind::Store16,
					MemoryAccessKind::Store16};
		if (inner_memory_schedule != expected_memory_schedule)
		{
			return Fail(Failure::UnsupportedInnerMemory,
				"inner loop memory schedule is not the framed predictor order");
		}

		Candidate candidate{};
		candidate.arithmetic_mode = mode;
		candidate.outer_loop = outer_loop;
		candidate.inner_loop = inner_loop;
		candidate.outer_blocks = outer.blocks;
		candidate.inner_blocks = inner.blocks;
		candidate.frame_advance_bytes = 18;
		candidate.inner_counter_seed = 30;
		candidate.inner_counter_gpr = inner_counter_gpr;
		candidate.inner_counter_stride = inner_counter_stride;
		candidate.inner_iterations = inner_iterations;
		candidate.exact_inner_control = true;
		candidate.outer_counter_gpr = outer_counter_gpr;
		candidate.outer_bound_gpr = outer_bound_gpr;
		candidate.outer_counter_storage = outer_counter_storage;
		candidate.outer_counter_initialization = outer_counter_initialization;
		candidate.outer_counter_load = outer_counter_load;
		candidate.outer_counter_store = outer_counter_store;
		candidate.outer_bound_storage = outer_bound_storage;
		candidate.outer_bound_initialization = outer_bound_initialization;
		candidate.outer_bound_load = outer_bound_load;
		candidate.outer_bound_initial_value = outer_bound_initial_value;
		candidate.outer_counter_stride = 1;
		candidate.outer_frame_pointer_stride = 36;
		candidate.exact_outer_control = true;
		candidate.cop1_arithmetic_nodes = inner_counts.cop1_arithmetic;
		candidate.integer_multiply_nodes = inner_counts.integer_multiply;
		candidate.exact_source_attestation = island.plan.exact_source_attestation;
		candidate.inner_memory_schedule = std::move(inner_memory_schedule);
		candidate.inner_memory_order = std::move(inner_memory_order);
		candidate.equivalent_inner_schedule_paths =
			equivalent_inner_schedule_paths;
		for (const SemanticIsland::InternalEdge& edge : island.plan.internal_edges)
		{
			if (Contains(outer.blocks, edge.source_block) &&
				!Contains(outer.blocks, edge.target_block))
			{
				candidate.outer_exit_edges.push_back({edge.source_block,
					edge.target_block, edge.ordinal, edge.scaled_cycle_cost,
					edge.event_horizon_check});
			}
		}
		for (u32 block_index : inner.blocks)
		{
			const Block& block = program.blocks[block_index];
			for (u32 node_ordinal = 0; node_ordinal < block.nodes.size();
				node_ordinal++)
			{
				const Node& node = block.nodes[node_ordinal];
				if (node.opcode != Opcode::MemoryLoad &&
					node.opcode != Opcode::MemoryStore)
				{
					continue;
				}
				const auto kind = static_cast<MemoryAccessKind>(node.immediate);
				MemoryOperation operation{node.id, block_index, node_ordinal, kind};
				operation.exact_header_affine_address =
					ResolveUniqueHeaderAffineAddress(program, inner.header_block,
						block_index, node.operands[1], &operation.address_base_gpr,
						&operation.address_offset);
				if (kind == MemoryAccessKind::LoadU8)
					candidate.inner_input_bytes.push_back(operation);
				else if (kind == MemoryAccessKind::LoadS32 ||
					kind == MemoryAccessKind::LoadF32Bits)
					candidate.inner_predictor_loads.push_back(operation);
				else if (kind == MemoryAccessKind::Store16)
					candidate.inner_outputs.push_back(operation);
			}
		}
		if (!ExactStreamAddressShape(&candidate))
		{
			return Fail(Failure::UnsupportedInnerMemory,
				"inner input/output addresses are not two exact framed streams");
		}
		candidate.outer_frame_pointer_gprs = candidate.inner_input_base_gprs;
		std::sort(candidate.outer_frame_pointer_gprs.begin(),
			candidate.outer_frame_pointer_gprs.end());
		candidate.outer_frame_pointer_gprs.erase(
			std::unique(candidate.outer_frame_pointer_gprs.begin(),
				candidate.outer_frame_pointer_gprs.end()),
			candidate.outer_frame_pointer_gprs.end());
		for (u8 gpr : candidate.inner_input_base_gprs)
		{
			if (!MatchHeaderPointerRecurrence(program, inner, gpr, 1))
			{
				return Fail(Failure::UnsupportedInnerMemory,
					"inner input stream does not advance by one byte", inner.header_block);
			}
		}
		for (u8 gpr : candidate.inner_output_base_gprs)
		{
			if (!MatchHeaderPointerRecurrence(program, inner, gpr, 4))
			{
				return Fail(Failure::UnsupportedInnerMemory,
					"inner output stream does not advance by four bytes",
					inner.header_block);
			}
		}
		candidate.inner_input_pointer_stride = 1;
		candidate.inner_output_pointer_stride = 4;
		candidate.exact_inner_stream_recurrences = true;
		for (u8 base_gpr : candidate.outer_frame_pointer_gprs)
		{
			AffineAnchors header_anchors;
			std::vector<u8> equivalent_gprs;
			for (u8 state_gpr : candidate.outer_frame_pointer_gprs)
			{
				if (!HeaderGprsHaveEqualLowAddress(program, outer, base_gpr,
						state_gpr))
				{
					continue;
				}
				equivalent_gprs.push_back(state_gpr);
				if (state_gpr != base_gpr)
				{
					const ValueId parameter = program.blocks[outer.header_block]
						.parameters.gpr[state_gpr];
					if (parameter == INVALID_VALUE)
					{
						return Fail(Failure::UnsupportedFrameControl,
							"outer input stream equality has no verifier parameter",
							outer.header_block);
					}
					header_anchors.emplace_back(parameter, Affine{true, true, 0});
				}
			}

			AffineAnchors anchors = header_anchors;
			bool summarized = !equivalent_gprs.empty();
			for (u8 state_gpr : equivalent_gprs)
			{
				summarized &= BuildCountedLoopExitAnchors(program, outer, inner,
					state_gpr, base_gpr, candidate.inner_input_pointer_stride,
					candidate.inner_iterations, header_anchors, &anchors);
			}
			if (!summarized)
			{
				return Fail(Failure::UnsupportedFrameControl,
					"outer input stream has no exact counted-inner-loop summary",
					inner.header_block);
			}
			const u32 outer_latch = outer.latch_blocks.front();
			const Block& outer_latch_block = program.blocks[outer_latch];
			const Affine recurrence = ResolveAffineFromHeaderAnchored(program,
				outer.header_block, outer_latch,
				outer_latch_block.terminator.taken.state.gpr[base_gpr], base_gpr,
				anchors);
			if (!recurrence.valid || !recurrence.has_base ||
				recurrence.offset != candidate.outer_frame_pointer_stride)
			{
				return Fail(Failure::UnsupportedFrameControl,
					"outer input stream does not advance by one stereo frame",
					outer_latch);
			}
		}
		for (const MemoryOperation& load : candidate.inner_predictor_loads)
		{
			PredictorLookup lookup{};
			if (!BuildPredictorLookup(program, inner, load,
					candidate.inner_input_bytes,
					mode == ArithmeticMode::IntegerFixedPoint, &lookup))
			{
				std::string expression = "invalid";
				if (load.block < program.blocks.size() &&
					load.node_ordinal < program.blocks[load.block].nodes.size())
				{
					const Node& operation =
						program.blocks[load.block].nodes[load.node_ordinal];
					if (operation.operand_count >= 2)
					{
						const SymbolicExpression symbolic =
							ResolveSymbolicExpression(program, inner.header_block,
								operation.operands[1]);
						if (!symbolic.key.empty())
							expression = symbolic.key;
					}
				}
				return Fail(Failure::UnsupportedInnerMemory,
					"predictor load is not indexed by one exact input byte: " +
						expression,
					load.block, load.operation);
			}
			candidate.predictor_lookups.push_back(lookup);
		}
		if (!ExactPredictorLookupShape(candidate))
		{
			return Fail(Failure::UnsupportedInnerMemory,
				"predictor tables do not form the exact per-channel lookup set");
		}
		if (candidate.outer_exit_edges.size() != 3)
		{
			return Fail(Failure::UnsupportedFrameControl,
				"outer loop does not expose two marker arms and one normal completion");
		}

		BuildResult result{};
		result.candidate = std::move(candidate);
		return result;
	}
} // namespace VitaEE::AdpcmDecode
