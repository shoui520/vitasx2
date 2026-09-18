// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#include "PrecompiledHeader.h"

#include "pcsx2/vita/VitaEeRegionExecutionPlan.h"

#include <array>
#include <limits>

namespace VitaEE::RegionExecution
{
	namespace
	{
		constexpr u16 INVALID_ORIGIN = std::numeric_limits<u16>::max();
		using WordOrigins = std::array<u16, 4>;

		BuildResult Fail(BuildFailure failure, u32 block, u32 value,
			const char* detail)
		{
			BuildResult result{};
			result.failure = failure;
			result.block = block;
			result.value = value;
			result.detail = detail;
			return result;
		}

		void AppendInternalTransfer(const RegionIR::Transfer& transfer,
			std::vector<std::vector<const RegionIR::Transfer*>>* incoming)
		{
			if (transfer.target_block != RegionIR::INVALID_BLOCK &&
				transfer.target_block < incoming->size())
			{
				(*incoming)[transfer.target_block].push_back(&transfer);
			}
		}

		u8 ValueWordCount(RegionIR::ValueType type)
		{
			switch (type)
			{
				case RegionIR::ValueType::I1:
				case RegionIR::ValueType::I32:
				case RegionIR::ValueType::F32Bits:
				case RegionIR::ValueType::Address:
					return 1;
				case RegionIR::ValueType::I64:
				case RegionIR::ValueType::Cycle:
					return 2;
				case RegionIR::ValueType::I128:
				case RegionIR::ValueType::VuF32x4Bits:
					return 4;
				case RegionIR::ValueType::Void:
				case RegionIR::ValueType::MemoryEffect:
					return 0;
			}
			return 0;
		}

		u16 EncodeOrigin(size_t slot, u8 word)
		{
			return static_cast<u16>(slot * 4 + word);
		}

		WordOrigins InvalidOrigins()
		{
			WordOrigins origins{};
			origins.fill(INVALID_ORIGIN);
			return origins;
		}

		WordOrigins DeriveOrigins(const RegionIR::Node& node,
			const std::vector<WordOrigins>& origins,
			const std::vector<const RegionIR::Node*>& definitions)
		{
			WordOrigins result = InvalidOrigins();
			auto operand_word = [&](u32 operand, u8 word) -> u16 {
				if (operand >= node.operand_count ||
					node.operands[operand] >= origins.size())
				{
					return INVALID_ORIGIN;
				}
				return origins[node.operands[operand]][word];
			};
			auto copy_words = [&](u32 operand, u8 source_first,
				u8 destination_first, u8 count) {
				for (u8 word = 0; word < count; word++)
					result[destination_first + word] =
						operand_word(operand, source_first + word);
			};

			switch (node.opcode)
			{
				case RegionIR::Opcode::ExtractLow32:
					copy_words(0, 0, 0, 1);
					break;
				case RegionIR::Opcode::ExtractLow64:
					copy_words(0, 0, 0, 2);
					break;
				case RegionIR::Opcode::ExtractHigh64:
					copy_words(0, 2, 0, 2);
					break;
				case RegionIR::Opcode::ReplaceLow64:
					copy_words(1, 0, 0, 2);
					copy_words(0, 2, 2, 2);
					break;
				case RegionIR::Opcode::ReplaceHigh64:
					copy_words(0, 0, 0, 2);
					copy_words(1, 0, 2, 2);
					break;
				case RegionIR::Opcode::BitcastI32ToF32Bits:
				case RegionIR::Opcode::BitcastF32BitsToI32:
				case RegionIR::Opcode::AddressFromI32:
					copy_words(0, 0, 0, 1);
					break;
				case RegionIR::Opcode::SignExtend32To64:
				case RegionIR::Opcode::ZeroExtend32To64:
					// The low word is bit-identical. The upper word is derived and
					// therefore cannot claim identity with any canonical word.
					copy_words(0, 0, 0, 1);
					break;
				case RegionIR::Opcode::Select64:
					for (u8 word = 0; word < 2; word++)
					{
						const u16 selected_true = operand_word(1, word);
						const u16 selected_false = operand_word(2, word);
						if (selected_true == selected_false)
							result[word] = selected_true;
					}
					break;
				case RegionIR::Opcode::PackLow64:
					copy_words(1, 0, 0, 2);
					copy_words(0, 0, 2, 2);
					break;
				case RegionIR::Opcode::PackHigh64:
					copy_words(0, 2, 0, 2);
					copy_words(1, 2, 2, 2);
					break;
				case RegionIR::Opcode::Vu0RequireIdle:
					copy_words(1, 0, 0, 4);
					break;
				case RegionIR::Opcode::MemoryLoadValue:
				{
					// Scalar EE loads replace the low 64 bits of the destination but
					// preserve its upper qword. The ordered MemoryLoad node carries
					// that pre-access architectural value explicitly as operand two.
					// Full LQ/LQC2 and non-GPR loads intentionally retain no identity.
					const RegionIR::ValueId effect_id = node.operands[0];
					const RegionIR::Node* effect = effect_id < definitions.size() ?
						definitions[effect_id] : nullptr;
					if (!effect || effect->opcode != RegionIR::Opcode::MemoryLoad ||
						effect->operand_count != 3 ||
						node.type != RegionIR::ValueType::I128)
					{
						break;
					}
					const auto kind = static_cast<RegionIR::MemoryAccessKind>(
						effect->immediate);
					if (kind != RegionIR::MemoryAccessKind::Load128)
					{
						const RegionIR::ValueId old_value = effect->operands[2];
						if (old_value < origins.size())
						{
							result[2] = origins[old_value][2];
							result[3] = origins[old_value][3];
						}
					}
					break;
				}
				case RegionIR::Opcode::And32:
				case RegionIR::Opcode::And64:
				case RegionIR::Opcode::Or64:
				case RegionIR::Opcode::And128:
				case RegionIR::Opcode::Or128:
					// AND/OR with the same SSA operand are exact identity operations.
					if (node.operand_count == 2 &&
						node.operands[0] == node.operands[1])
					{
						copy_words(0, 0, 0, ValueWordCount(node.type));
					}
					break;
				case RegionIR::Opcode::Parameter:
					// Parameters are seeded and updated by the CFG fixed point.
					return node.id < origins.size() ? origins[node.id] : result;
				default:
					break;
			}
			return result;
		}

		Low32Extension DeriveLow32Extension(const RegionIR::Node& node,
			const std::vector<Low32Extension>& extensions,
			const std::vector<StateMask>& dependencies,
			const std::vector<const RegionIR::Node*>& definitions,
			StateMask* result_dependencies)
		{
			if (!result_dependencies)
				return Low32Extension::None;
			result_dependencies->reset();
			auto operand = [&](u32 index, StateMask* guards = nullptr) {
				if (guards)
				{
					guards->reset();
					if (index < node.operand_count &&
						node.operands[index] < dependencies.size())
					{
						*guards = dependencies[node.operands[index]];
					}
				}
				return index < node.operand_count &&
					node.operands[index] < extensions.size() ?
						extensions[node.operands[index]] : Low32Extension::None;
			};
			switch (node.opcode)
			{
				case RegionIR::Opcode::SignExtend32To64:
					return Low32Extension::Sign;
				case RegionIR::Opcode::ZeroExtend32To64:
					return Low32Extension::Zero;
				case RegionIR::Opcode::ExtractLow64:
					return operand(0, result_dependencies);
				case RegionIR::Opcode::ReplaceLow64:
					return operand(1, result_dependencies);
				case RegionIR::Opcode::ReplaceHigh64:
					return operand(0, result_dependencies);
				case RegionIR::Opcode::Select64:
				{
					StateMask true_guards;
					StateMask false_guards;
					const Low32Extension selected_true = operand(1, &true_guards);
					const Low32Extension selected_false = operand(2, &false_guards);
					if (selected_true == Low32Extension::None ||
						selected_true != selected_false)
					{
						return Low32Extension::None;
					}
					*result_dependencies = true_guards | false_guards;
					return selected_true;
				}
				case RegionIR::Opcode::MemoryLoadValue:
				{
					if (node.operand_count != 1 ||
						node.operands[0] >= definitions.size())
					{
						return Low32Extension::None;
					}
					const RegionIR::Node* const load = definitions[node.operands[0]];
					if (!load || load->opcode != RegionIR::Opcode::MemoryLoad)
						return Low32Extension::None;
					switch (static_cast<RegionIR::MemoryAccessKind>(load->immediate))
					{
						case RegionIR::MemoryAccessKind::LoadS8:
						case RegionIR::MemoryAccessKind::LoadS16:
						case RegionIR::MemoryAccessKind::LoadS32:
							return Low32Extension::Sign;
						case RegionIR::MemoryAccessKind::LoadU8:
						case RegionIR::MemoryAccessKind::LoadU16:
						case RegionIR::MemoryAccessKind::LoadU32:
							return Low32Extension::Zero;
						default:
							return Low32Extension::None;
					}
				}
				case RegionIR::Opcode::Parameter:
				default:
					return Low32Extension::None;
			}
		}
	} // namespace

	StateSlot DecodeStateSlot(size_t slot)
	{
		if (slot < 32)
			return {StateClass::Gpr, static_cast<u8>(slot)};
		slot -= 32;
		if (slot == 0)
			return {StateClass::Hi, 0};
		if (slot == 1)
			return {StateClass::Lo, 0};
		if (slot == 2)
			return {StateClass::Sa, 0};
		slot -= 3;
		if (slot < 32)
			return {StateClass::Fpr, static_cast<u8>(slot)};
		slot -= 32;
		if (slot == 0)
			return {StateClass::Fcr0, 0};
		if (slot == 1)
			return {StateClass::Fcr31, 0};
		if (slot == 2)
			return {StateClass::Acc, 0};
		if (slot == 3)
			return {StateClass::AccFlag, 0};
		slot -= 4;
		if (slot < 32)
			return {StateClass::Vu0Vf, static_cast<u8>(slot)};
		slot -= 32;
		if (slot == 0)
			return {StateClass::Vu0Acc, 0};
		if (slot == 1)
			return {StateClass::Vu0MacFlag, 0};
		if (slot == 2)
			return {StateClass::Vu0StatusFlag, 0};
		if (slot == 3)
			return {StateClass::Vu0ClipFlag, 0};
		if (slot == 4)
			return {StateClass::Vu0Q, 0};
		slot -= 5;
		if (slot < 32)
			return {StateClass::Vu0Vi, static_cast<u8>(slot)};
		slot -= 32;
		if (slot < 4)
			return {StateClass::Vu0MicroMacFlag, static_cast<u8>(slot)};
		slot -= 4;
		if (slot < 4)
			return {StateClass::Vu0MicroClipFlag, static_cast<u8>(slot)};
		slot -= 4;
		if (slot < 4)
			return {StateClass::Vu0MicroStatusFlag, static_cast<u8>(slot)};
		return {StateClass::Cycle, 0};
	}

	u8 StateWordCount(size_t slot)
	{
		const StateSlot decoded = DecodeStateSlot(slot);
		switch (decoded.state_class)
		{
			case StateClass::Gpr:
			case StateClass::Hi:
			case StateClass::Lo:
			case StateClass::Vu0Vf:
			case StateClass::Vu0Acc:
				return 4;
			case StateClass::Cycle:
				return 2;
			case StateClass::Sa:
			case StateClass::Fpr:
			case StateClass::Fcr0:
			case StateClass::Fcr31:
			case StateClass::Acc:
			case StateClass::AccFlag:
			case StateClass::Vu0MacFlag:
			case StateClass::Vu0StatusFlag:
			case StateClass::Vu0ClipFlag:
			case StateClass::Vu0Q:
			case StateClass::Vu0Vi:
			case StateClass::Vu0MicroMacFlag:
			case StateClass::Vu0MicroClipFlag:
			case StateClass::Vu0MicroStatusFlag:
				return 1;
		}
		return 0;
	}

	size_t CanonicalStateWordOffset(size_t slot, u8 word)
	{
		if (slot >= STATE_SLOT_COUNT || word >= StateWordCount(slot))
			return SIZE_MAX;
		const StateSlot decoded = DecodeStateSlot(slot);
		size_t base = 0;
		switch (decoded.state_class)
		{
			case StateClass::Gpr:
				base = offsetof(RegionIR::CanonicalState, gpr) +
					decoded.index * sizeof(u128);
				break;
			case StateClass::Hi:
				base = offsetof(RegionIR::CanonicalState, hi);
				break;
			case StateClass::Lo:
				base = offsetof(RegionIR::CanonicalState, lo);
				break;
			case StateClass::Sa:
				base = offsetof(RegionIR::CanonicalState, sa);
				break;
			case StateClass::Fpr:
				base = offsetof(RegionIR::CanonicalState, fpr) +
					decoded.index * sizeof(u32);
				break;
			case StateClass::Fcr0:
				base = offsetof(RegionIR::CanonicalState, fcr0);
				break;
			case StateClass::Fcr31:
				base = offsetof(RegionIR::CanonicalState, fcr31);
				break;
			case StateClass::Acc:
				base = offsetof(RegionIR::CanonicalState, acc);
				break;
			case StateClass::AccFlag:
				base = offsetof(RegionIR::CanonicalState, acc_flag);
				break;
			case StateClass::Vu0Vf:
				base = offsetof(RegionIR::CanonicalState, vu0_vf) +
					decoded.index * sizeof(u128);
				break;
			case StateClass::Vu0Acc:
				base = offsetof(RegionIR::CanonicalState, vu0_acc);
				break;
			case StateClass::Vu0MacFlag:
				base = offsetof(RegionIR::CanonicalState, vu0_macflag);
				break;
			case StateClass::Vu0StatusFlag:
				base = offsetof(RegionIR::CanonicalState, vu0_statusflag);
				break;
			case StateClass::Vu0ClipFlag:
				base = offsetof(RegionIR::CanonicalState, vu0_clipflag);
				break;
			case StateClass::Vu0Q:
				base = offsetof(RegionIR::CanonicalState, vu0_q);
				break;
			case StateClass::Vu0Vi:
				base = offsetof(RegionIR::CanonicalState, vu0_vi) +
					decoded.index * sizeof(u32);
				break;
			case StateClass::Vu0MicroMacFlag:
				base = offsetof(RegionIR::CanonicalState, vu0_micro_macflags) +
					decoded.index * sizeof(u32);
				break;
			case StateClass::Vu0MicroClipFlag:
				base = offsetof(RegionIR::CanonicalState, vu0_micro_clipflags) +
					decoded.index * sizeof(u32);
				break;
			case StateClass::Vu0MicroStatusFlag:
				base = offsetof(RegionIR::CanonicalState, vu0_micro_statusflags) +
					decoded.index * sizeof(u32);
				break;
			case StateClass::Cycle:
				base = offsetof(RegionIR::CanonicalState, cycle);
				break;
		}
		return base + word * sizeof(u32);
	}

	RegionIR::ValueId StateValue(const RegionIR::StateMap& state, size_t slot)
	{
		const StateSlot decoded = DecodeStateSlot(slot);
		switch (decoded.state_class)
		{
			case StateClass::Gpr:
				return state.gpr[decoded.index];
			case StateClass::Hi:
				return state.hi;
			case StateClass::Lo:
				return state.lo;
			case StateClass::Sa:
				return state.sa;
			case StateClass::Fpr:
				return state.fpr[decoded.index];
			case StateClass::Fcr0:
				return state.fcr0;
			case StateClass::Fcr31:
				return state.fcr31;
			case StateClass::Acc:
				return state.acc;
			case StateClass::AccFlag:
				return state.acc_flag;
			case StateClass::Vu0Vf:
				return state.vu0_vf[decoded.index];
			case StateClass::Vu0Acc:
				return state.vu0_acc;
			case StateClass::Vu0MacFlag:
				return state.vu0_macflag;
			case StateClass::Vu0StatusFlag:
				return state.vu0_statusflag;
			case StateClass::Vu0ClipFlag:
				return state.vu0_clipflag;
			case StateClass::Vu0Q:
				return state.vu0_q;
			case StateClass::Vu0Vi:
				return state.vu0_vi[decoded.index];
			case StateClass::Vu0MicroMacFlag:
				return state.vu0_micro_macflags[decoded.index];
			case StateClass::Vu0MicroClipFlag:
				return state.vu0_micro_clipflags[decoded.index];
			case StateClass::Vu0MicroStatusFlag:
				return state.vu0_micro_statusflags[decoded.index];
			case StateClass::Cycle:
				return state.cycle;
		}
		return RegionIR::INVALID_VALUE;
	}

	const RegionIR::Transfer* ResolveTransfer(
		const RegionIR::Program& program, const ExitSite& site)
	{
		if (site.kind == ExitSiteKind::EntryEvent ||
			site.kind == ExitSiteKind::EntryFallback ||
			site.kind == ExitSiteKind::BlockEntry ||
			site.block >= program.blocks.size())
		{
			return nullptr;
		}
		const RegionIR::Block& block = program.blocks[site.block];
		switch (site.kind)
		{
			case ExitSiteKind::Guarded:
				return site.ordinal < block.guarded_exits.size() ?
					&block.guarded_exits[site.ordinal] : nullptr;
			case ExitSiteKind::Observer:
				return site.ordinal < block.observer_exits.size() ?
					&block.observer_exits[site.ordinal].transfer : nullptr;
			case ExitSiteKind::Memory:
				return site.ordinal < block.memory_exits.size() ?
					&block.memory_exits[site.ordinal].transfer : nullptr;
			case ExitSiteKind::Taken:
				return &block.terminator.taken;
			case ExitSiteKind::NotTaken:
				return block.terminator.kind == RegionIR::TerminatorKind::Branch ?
					&block.terminator.not_taken : nullptr;
			case ExitSiteKind::EntryEvent:
			case ExitSiteKind::EntryFallback:
			case ExitSiteKind::BlockEntry:
				return nullptr;
		}
		return nullptr;
	}

	bool ResolveExitContract(const RegionIR::Program& program,
		const ExitSite& site, ExitContractView* contract)
	{
		if (!contract || site.block >= program.blocks.size())
			return false;
		*contract = {};
		const RegionIR::Block& block = program.blocks[site.block];
		if (site.kind == ExitSiteKind::EntryEvent ||
			site.kind == ExitSiteKind::EntryFallback ||
			site.kind == ExitSiteKind::BlockEntry)
		{
			contract->state = &block.parameters;
			contract->static_pc = block.pc;
			contract->cycle_commit_deferred =
				site.kind != ExitSiteKind::EntryEvent;
			contract->event_horizon_check =
				site.kind == ExitSiteKind::EntryEvent;
			return true;
		}

		const RegionIR::Transfer* transfer = ResolveTransfer(program, site);
		if (!transfer)
			return false;
		contract->state = &transfer->state;
		contract->transfer = transfer;
		contract->cycle_commit_deferred = transfer->cycle_commit_deferred;
		contract->pending_raw_cycles = transfer->pending_raw_cycles;
		contract->event_horizon_check = transfer->event_horizon_check;
		return true;
	}

	BuildResult Build(const RegionIR::Program& program)
	{
		const RegionIR::VerifyResult verified = RegionIR::Verify(program);
		if (!verified)
		{
			return Fail(BuildFailure::InvalidProgram, verified.block,
				verified.node, verified.detail.c_str());
		}

		BuildResult result{};
		result.plan.value_exit_uses.assign(program.value_count, 0);
		result.plan.value_exit_word_uses.assign(program.value_count, 0);
		result.plan.value_canonical_parameter_words.assign(program.value_count, 0);
		result.plan.value_low32_extensions.assign(program.value_count,
			Low32Extension::None);
		result.plan.value_low32_entry_guards.resize(program.value_count);
		std::vector<WordOrigins> origins(program.value_count, InvalidOrigins());
		std::vector<const RegionIR::Node*> definitions(program.value_count, nullptr);
		for (const RegionIR::Block& block : program.blocks)
		{
			for (const RegionIR::Node& node : block.nodes)
				definitions[node.id] = &node;
		}
		for (const RegionIR::Block& block : program.blocks)
		{
			for (size_t slot = 0; slot < STATE_SLOT_COUNT; slot++)
			{
				const RegionIR::ValueId parameter = StateValue(block.parameters, slot);
				if (parameter >= program.value_count)
				{
					return Fail(BuildFailure::InvalidStateValue,
						static_cast<u32>(&block - program.blocks.data()), parameter,
						"block parameter is outside the verified value table");
				}
				for (u8 word = 0; word < StateWordCount(slot); word++)
					origins[parameter][word] = EncodeOrigin(slot, word);
			}
		}

		std::vector<std::vector<const RegionIR::Transfer*>> incoming(
			program.blocks.size());
		for (const RegionIR::Block& block : program.blocks)
		{
			(void)RegionIR::VisitInternalTransfers(block.terminator,
				[&](const RegionIR::Transfer& transfer, u8) {
					AppendInternalTransfer(transfer, &incoming);
					return true;
				});
		}

		// Compute only anchored extension facts. Explicit extensions and scalar
		// loads are anchors; identity/select nodes preserve them, and a block
		// parameter receives a fact only after every internal predecessor already
		// proves the same relationship. Consequently an unchanged cyclic parameter
		// cannot invent either signedness by referring to itself. Entry parameters
		// remain conditional facts because the external predecessor is canonical
		// machine state rather than an IR edge; the candidate guard records exactly
		// what a backend must check before using one.
		bool extension_changed = true;
		while (extension_changed)
		{
			extension_changed = false;
			for (const RegionIR::Block& block : program.blocks)
			{
				for (const RegionIR::Node& node : block.nodes)
				{
					if (node.opcode == RegionIR::Opcode::Parameter ||
						result.plan.value_low32_extensions[node.id] !=
							Low32Extension::None)
					{
						continue;
					}
					StateMask dependencies;
					const Low32Extension derived = DeriveLow32Extension(node,
						result.plan.value_low32_extensions,
						result.plan.value_low32_entry_guards, definitions,
						&dependencies);
					if (derived != Low32Extension::None)
					{
						result.plan.value_low32_extensions[node.id] = derived;
						result.plan.value_low32_entry_guards[node.id] = dependencies;
						extension_changed = true;
					}
				}
			}
			for (u32 block_index = 0; block_index < program.blocks.size(); block_index++)
			{
				const RegionIR::Block& block = program.blocks[block_index];
				if (incoming[block_index].empty())
					continue;
				for (size_t slot = 0; slot < STATE_SLOT_COUNT; slot++)
				{
					const RegionIR::ValueId parameter =
						StateValue(block.parameters, slot);
					if (parameter >= program.value_count ||
						result.plan.value_low32_extensions[parameter] !=
							Low32Extension::None || StateWordCount(slot) < 2)
					{
						continue;
					}
					Low32Extension common = Low32Extension::None;
					StateMask dependencies;
					bool complete = true;
					for (const RegionIR::Transfer* predecessor : incoming[block_index])
					{
						const RegionIR::ValueId source =
							StateValue(predecessor->state, slot);
						const Low32Extension source_extension =
							source < result.plan.value_low32_extensions.size() ?
								result.plan.value_low32_extensions[source] :
								Low32Extension::None;
						if (source_extension == Low32Extension::None ||
							(common != Low32Extension::None &&
							 common != source_extension))
						{
							complete = false;
							break;
						}
						common = source_extension;
						dependencies |=
							result.plan.value_low32_entry_guards[source];
					}
					if (complete && common != Low32Extension::None)
					{
						if (block_index == program.entry_block)
							dependencies.set(slot);
						result.plan.value_low32_extensions[parameter] = common;
						result.plan.value_low32_entry_guards[parameter] = dependencies;
						extension_changed = true;
					}
				}
			}
		}
		if (program.entry_block < program.blocks.size())
		{
			const RegionIR::Block& entry = program.blocks[program.entry_block];
			for (size_t slot = 0; slot < STATE_SLOT_COUNT; slot++)
			{
				const RegionIR::ValueId parameter = StateValue(entry.parameters, slot);
				const Low32Extension extension = parameter <
					result.plan.value_low32_extensions.size() ?
						result.plan.value_low32_extensions[parameter] :
						Low32Extension::None;
				if (extension != Low32Extension::None && StateWordCount(slot) >= 2)
				{
					result.plan.entry_low32_guard_candidates.push_back({parameter,
						static_cast<u16>(slot), extension});
				}
			}
		}

		// Optimistically treat every 32-bit lane of every block parameter as its
		// canonical source, then monotonically invalidate individual lanes when an
		// internal predecessor cannot prove that identity. Re-deriving local alias
		// nodes on every iteration lets ReplaceLow64 preserve the untouched upper
		// half around a loop without mistaking the changed scalar half for a full
		// qword write.
		bool changed = true;
		while (changed)
		{
			changed = false;
			for (const RegionIR::Block& block : program.blocks)
			{
				for (const RegionIR::Node& node : block.nodes)
				{
					if (node.opcode != RegionIR::Opcode::Parameter)
						origins[node.id] = DeriveOrigins(node, origins, definitions);
				}
			}
			for (u32 block_index = 0; block_index < program.blocks.size(); block_index++)
			{
				const RegionIR::Block& block = program.blocks[block_index];
				for (size_t slot = 0; slot < STATE_SLOT_COUNT; slot++)
				{
					const RegionIR::ValueId parameter =
						StateValue(block.parameters, slot);
					for (u8 word = 0; word < StateWordCount(slot); word++)
					{
						if (origins[parameter][word] != EncodeOrigin(slot, word))
							continue;
						for (const RegionIR::Transfer* predecessor : incoming[block_index])
						{
							const RegionIR::ValueId source =
								StateValue(predecessor->state, slot);
							if (source >= origins.size() ||
								origins[source][word] != EncodeOrigin(slot, word))
							{
								origins[parameter][word] = INVALID_ORIGIN;
								changed = true;
								break;
							}
						}
					}
				}
			}
		}
		for (const RegionIR::Block& block : program.blocks)
		{
			for (const RegionIR::Node& node : block.nodes)
			{
				if (node.opcode != RegionIR::Opcode::Parameter)
					break;
				if (node.id >= origins.size())
				{
					return Fail(BuildFailure::InvalidStateValue,
						static_cast<u32>(&block - program.blocks.data()), node.id,
						"parameter canonical-origin slot is invalid");
				}
				if (node.immediate >= STATE_SLOT_COUNT)
				{
					if (node.type == RegionIR::ValueType::MemoryEffect &&
						node.id == block.parameters.memory_effect)
					{
						continue;
					}
					return Fail(BuildFailure::InvalidStateValue,
						static_cast<u32>(&block - program.blocks.data()), node.id,
						"parameter canonical-origin slot is invalid");
				}
				u8 mask = 0;
				for (u8 word = 0; word < StateWordCount(node.immediate); word++)
				{
					if (origins[node.id][word] == EncodeOrigin(node.immediate, word))
						mask |= static_cast<u8>(1u << word);
				}
				result.plan.value_canonical_parameter_words[node.id] = mask;
			}
		}

		result.plan.exits.push_back(
			{ExitSiteKind::EntryEvent, program.entry_block, 0, {}});
		result.plan.exits.push_back(
			{ExitSiteKind::EntryFallback, program.entry_block, 0, {}});
		const RegionIR::ValueId entry_cycle =
			program.blocks[program.entry_block].parameters.cycle;
		result.plan.value_exit_uses[entry_cycle]++;
		result.plan.value_exit_word_uses[entry_cycle] |= 0x3;

		auto dirty_word_mask = [&](RegionIR::ValueId value, size_t slot) -> u8 {
			if (value >= origins.size())
				return 0xff;
			u8 mask = 0;
			for (u8 word = 0; word < StateWordCount(slot); word++)
			{
				if (origins[value][word] != EncodeOrigin(slot, word))
					mask |= static_cast<u8>(1u << word);
			}
			return mask;
		};
		auto record_dirty = [&](ExitSite* site, size_t slot,
			RegionIR::ValueId value) {
			const u8 mask = dirty_word_mask(value, slot);
			if (slot == 0 || mask == 0)
				return;
			site->dirty_state.set(slot);
			site->dirty_words[slot] = mask;
			result.plan.value_exit_uses[value]++;
			result.plan.value_exit_word_uses[value] |= mask;
			result.plan.dirty_state_bindings++;
			for (u8 word = 0; word < 4; word++)
				result.plan.dirty_state_words += (mask >> word) & 1u;
		};

		auto append_exit = [&](ExitSiteKind kind, u32 block, u32 ordinal) -> bool {
			ExitSite site{kind, block, ordinal, {}};
			const RegionIR::Transfer* transfer = ResolveTransfer(program, site);
			if (!transfer || transfer->pc >= program.value_count)
				return false;
			result.plan.value_exit_uses[transfer->pc]++;
			result.plan.value_exit_word_uses[transfer->pc] |= 0x1;
			for (size_t slot = 0; slot < STATE_SLOT_COUNT; slot++)
			{
				const RegionIR::ValueId value = StateValue(transfer->state, slot);
				if (value >= program.value_count)
					return false;
				record_dirty(&site, slot, value);
			}
			result.plan.exits.push_back(std::move(site));
			return true;
		};
		auto append_block_entry = [&](u32 block_index) -> bool {
			ExitSite site{ExitSiteKind::BlockEntry, block_index, 0, {}};
			const RegionIR::StateMap& state =
				program.blocks[block_index].parameters;
			for (size_t slot = 1; slot < STATE_SLOT_COUNT; slot++)
			{
				const RegionIR::ValueId value = StateValue(state, slot);
				if (value >= program.value_count)
					return false;
				record_dirty(&site, slot, value);
			}
			result.plan.exits.push_back(std::move(site));
			return true;
		};

		for (u32 block_index = 0; block_index < program.blocks.size(); block_index++)
		{
			const RegionIR::Block& block = program.blocks[block_index];
			if (!append_block_entry(block_index))
				return Fail(BuildFailure::InvalidExitSite, block_index, 0,
					"block-entry fallback cannot be resolved");
			for (u32 index = 0; index < block.guarded_exits.size(); index++)
			{
				if (!append_exit(ExitSiteKind::Guarded, block_index, index))
					return Fail(BuildFailure::InvalidExitSite, block_index, index,
						"guarded exit cannot be resolved");
			}
			for (u32 index = 0; index < block.observer_exits.size(); index++)
			{
				if (!append_exit(ExitSiteKind::Observer, block_index, index))
					return Fail(BuildFailure::InvalidExitSite, block_index, index,
						"observer exit cannot be resolved");
			}
			for (u32 index = 0; index < block.memory_exits.size(); index++)
			{
				if (!append_exit(ExitSiteKind::Memory, block_index, index))
					return Fail(BuildFailure::InvalidExitSite, block_index, index,
						"memory exit cannot be resolved");
			}
			if (!append_exit(ExitSiteKind::Taken, block_index, 0))
				return Fail(BuildFailure::InvalidExitSite, block_index, 0,
					"primary transfer cannot be resolved");
			if (block.terminator.kind == RegionIR::TerminatorKind::Branch &&
				!append_exit(ExitSiteKind::NotTaken, block_index, 0))
			{
				return Fail(BuildFailure::InvalidExitSite, block_index, 0,
					"secondary transfer cannot be resolved");
			}
		}
		return result;
	}
} // namespace VitaEE::RegionExecution
