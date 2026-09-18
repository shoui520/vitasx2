// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#include "PrecompiledHeader.h"

#include "Config.h"
#include "Memory.h"
#include "R5900.h"
#include "VU.h"
#include "pcsx2/vita/VitaEeBlockCompiler.h"
#include "pcsx2/vita/VitaEeRegionAllocation.h"
#include "pcsx2/vita/VitaEeRegionRuntime.h"
#include "pcsx2/vita/VitaEeRegionMemory.h"
#include "pcsx2/vita/VitaPerformanceTelemetry.h"
#include "vtlb.h"

#include "common/Timer.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <new>
#include <utility>
#include <vector>

namespace VitaEE::RegionRuntime
{
	namespace
	{
		constexpr u32 MAX_PRIMARY_SOURCE_WORDS = 64;
		// A caller/leaf VU0 graph on physical Vita required more than the
		// original 96-word immutable-source envelope before target lowering could
		// classify it. This is only the cold source/IR ceiling: the backend still
		// enforces eight direct calls, a 4 KiB hot slab, 1.5 KiB entry,
		// 3 KiB body, and the measured Cortex-A9 profitability certificate. Let
		// those target limits decide whether the larger attested unit is executable.
		// The physical tballs caller/leaf graph establishes a 134-word immutable
		// ownership requirement. Keep modest headroom for split-fragment unions;
		// executable admission remains independently bounded by the ownership,
		// eight-call and Cortex-A9 code-size/profitability gates below.
		constexpr u32 MAX_REGION_SOURCE_WORDS = 160;

		struct NarrowedPreflightedPrescreen
		{
			u32 required_observations = 0;
			u32 narrowed_comparisons = 0;
			bool proven = false;
		};

		NarrowedPreflightedPrescreen BuildNarrowedPreflightedPrescreen(
			const RegionIR::Program& program,
			const RegionMemoryPlan::BuildResult& memory,
			const RegionMemoryPlan::A9ProfitabilityCertificate& certificate)
		{
			NarrowedPreflightedPrescreen result{};
			if (!memory || certificate.kind != RegionMemoryPlan::
					A9ProfitabilityCertificate::Kind::ObservedPreflightedLoop ||
				certificate.work_per_iteration == 0)
			{
				return result;
			}

			const RegionExecution::BuildResult execution =
				RegionExecution::Build(program);
			if (!execution)
				return result;

			std::vector<RegionIR::ValueId> preflighted_operations;
			for (const RegionMemoryPlan::CountedRange& range :
				memory.plan.counted_ranges)
			{
				for (const RegionMemoryPlan::Access& access : range.accesses)
					preflighted_operations.push_back(access.operation);
			}
			for (const RegionMemoryPlan::BoundedRange& range :
				memory.plan.bounded_ranges)
			{
				for (const RegionMemoryPlan::Access& access : range.accesses)
					preflighted_operations.push_back(access.operation);
			}

			RegionAllocation::Options options{};
			// The fully preflighted product ABI can reclaim r7 from the resident vTLB
			// pair. Keep this prescreen identical to the backend's eight-word identity-
			// RAM bank; final emission remains the authority if another refinement
			// changes pressure or code shape.
			options.core_register_words = 8;
			options.neon_q_registers = 6;
			options.fixed_cycle_word_mask = 0x1;
			options.enable_guarded_entry_low32 = true;
			options.preflighted_memory_operations = &preflighted_operations;
			options.aggregate_cycle_header_block = memory.plan.timing.header_block;
			const RegionAllocation::BuildResult allocation =
				RegionAllocation::Build(program, execution.plan, options);
			if (!allocation || allocation.plan.spilled_values != 0)
				return result;

			auto extension = [&](RegionIR::ValueId value) {
				return value < allocation.plan.value_low32_extensions.size() ?
					allocation.plan.value_low32_extensions[value] :
					RegionAllocation::RematerializationKind::None;
			};
			for (const RegionIR::Block& block : program.blocks)
			{
				for (const RegionIR::Node& node : block.nodes)
				{
					if (node.operand_count != 2)
						continue;
					const RegionAllocation::RematerializationKind left =
						extension(node.operands[0]);
					const RegionAllocation::RematerializationKind right =
						extension(node.operands[1]);
					const bool same = left !=
						RegionAllocation::RematerializationKind::None && left == right;
					switch (node.opcode)
					{
						case RegionIR::Opcode::CompareEqual64:
						case RegionIR::Opcode::CompareNotEqual64:
						case RegionIR::Opcode::CompareSignedLess64:
							result.narrowed_comparisons += same ? 1u : 0u;
							break;
						case RegionIR::Opcode::CompareUnsignedLess64:
							result.narrowed_comparisons += (same ||
								(left == RegionAllocation::RematerializationKind::SignExtendLow32 &&
								 right == RegionAllocation::RematerializationKind::ZeroExtendLow32)) ?
								1u : 0u;
							break;
						default:
							break;
					}
				}
			}
			if (result.narrowed_comparisons < 2)
				return result;

			const u32 work = RegionA32::CompileOptions::
				DEFAULT_MINIMUM_A9_NARROWED_PREFLIGHTED_WORK;
			result.required_observations = std::max<u32>(1,
				static_cast<u32>((static_cast<u64>(work) +
					certificate.work_per_iteration - 1) /
					certificate.work_per_iteration));
			result.proven = result.required_observations <
				certificate.minimum_profitable_iterations;
			return result;
		}

		bool IsSupportedRegionEntrySignature(const GprLinkSignature& signature)
		{
			if (!signature.IsValid() ||
				signature.HasVtlbPointer() || signature.HasVtlbWritePointer() ||
				signature.HasVtlbStaticPage() || signature.HasGprQword() ||
				signature.HasPredicate())
			{
				return false;
			}

			// r7/r8 are the persistent dispatcher's VTLB bases.  r12 and lr are
			// region scratch/branch registers.  Accept the ordinary callee-saved
			// scalar pin bank plus the exact signed scheduler countdown in r6.  The
			// region backend reconstructs the canonical 64-bit cycle from that
			// countdown before any entry proof observes it.  VTLB pointers, qword
			// state, and predicates remain on the canonical entry until their whole
			// private representations have mechanically checked adapters.
			u16 hosts = 0;
			auto accept_host = [&](u8 host) {
				if (host < GprLinkSignature::DEFAULT_FIRST_HOST ||
					host > GprLinkSignature::LAST_CALLEE_HOST ||
					(hosts & (1u << host)) != 0)
				{
					return false;
				}
				hosts |= static_cast<u16>(1u << host);
				return true;
			};
			for (u8 index = 0; index < signature.count; index++)
			{
				const GprLinkMapping& mapping = signature.mappings[index];
				if (mapping.guest == 0 || mapping.guest >= 32 ||
					!accept_host(mapping.low_host))
				{
					return false;
				}
				if (mapping.width == GprLinkWidth::Low64 &&
					!accept_host(mapping.high_host))
				{
					return false;
				}
			}
			return true;
		}

		bool UsesUncertifiedCop1Tier(const RegionIR::Program& program)
		{
			for (const RegionIR::Block& block : program.blocks)
			{
				for (const RegionIR::Node& node : block.nodes)
				{
					switch (node.opcode)
					{
						case RegionIR::Opcode::BitcastI32ToF32Bits:
						case RegionIR::Opcode::BitcastF32BitsToI32:
						case RegionIR::Opcode::Cop1NormalizeInput:
						case RegionIR::Opcode::Cop1AddRaw:
						case RegionIR::Opcode::Cop1SubRaw:
						case RegionIR::Opcode::Cop1MulRaw:
						case RegionIR::Opcode::Cop1ClampOuResult:
						case RegionIR::Opcode::Cop1UpdateOuFlags:
						case RegionIR::Opcode::Cop1CompareEqual:
						case RegionIR::Opcode::Cop1CompareLess:
						case RegionIR::Opcode::Cop1CompareLessEqual:
						case RegionIR::Opcode::Cop1UpdateConditionFlag:
						case RegionIR::Opcode::Cop1BranchCondition:
						case RegionIR::Opcode::Cop1AbsoluteWord:
						case RegionIR::Opcode::Cop1NegateWord:
						case RegionIR::Opcode::Cop1ClearOuFlags:
						case RegionIR::Opcode::Cop1ConvertWord:
						case RegionIR::Opcode::Cop1ConvertSingle:
						case RegionIR::Opcode::BindFpr:
						case RegionIR::Opcode::BindFcr31:
						case RegionIR::Opcode::BindAcc:
							return true;
						default:
							break;
					}
					if (node.opcode != RegionIR::Opcode::Parameter &&
						node.type == RegionIR::ValueType::F32Bits)
					{
						return true;
					}
				}
			}
			return false;
		}

		bool HasCertifiedCop1Residency(
			const RegionMemoryPlan::A9ProfitabilityCertificate& certificate)
		{
			return certificate.kind == RegionMemoryPlan::
					A9ProfitabilityCertificate::Kind::ObservedCop1Residency ||
				certificate.kind == RegionMemoryPlan::
					A9ProfitabilityCertificate::Kind::ObservedCop1Vu0Residency;
		}

		bool HasCertifiedVu0FmacStream(
			const RegionMemoryPlan::A9ProfitabilityCertificate& certificate)
		{
			return certificate.kind == RegionMemoryPlan::
					A9ProfitabilityCertificate::Kind::ObservedVu0FmacStream ||
				certificate.kind == RegionMemoryPlan::
					A9ProfitabilityCertificate::Kind::ObservedVu0AcyclicFmacLeaf ||
				certificate.kind == RegionMemoryPlan::
					A9ProfitabilityCertificate::Kind::ObservedCop1Vu0Residency;
		}

		bool UsesUncertifiedVu0Tier(const RegionIR::Program& program)
		{
			for (const RegionIR::Block& block : program.blocks)
			{
				for (const RegionIR::Node& node : block.nodes)
				{
						switch (node.opcode)
						{
							case RegionIR::Opcode::Vu0RequireIdle:
							case RegionIR::Opcode::Vu0ConvertFixed:
							case RegionIR::Opcode::Vu0ConvertIntegerToFloat:
							case RegionIR::Opcode::Vu0Rotate32:
							case RegionIR::Opcode::Vu0NormalizeVector:
							case RegionIR::Opcode::Vu0BroadcastLane:
							case RegionIR::Opcode::Vu0BroadcastScalar:
							case RegionIR::Opcode::Vu0FdivQ:
							case RegionIR::Opcode::Vu0FdivFlags:
							case RegionIR::Opcode::Vu0UpdateFdivStatus:
							case RegionIR::Opcode::Vu0SyncFdivStatusControl:
							case RegionIR::Opcode::Vu0MulRaw:
							case RegionIR::Opcode::Vu0AddRaw:
							case RegionIR::Opcode::Vu0SubRaw:
							case RegionIR::Opcode::Vu0ClampFmacResult:
							case RegionIR::Opcode::Vu0MacFlagsFromRaw:
							case RegionIR::Opcode::Vu0StatusFlagsFromMac:
							case RegionIR::Opcode::Vu0MergeMasked:
							case RegionIR::Opcode::Vu0SyncStatusControl:
							case RegionIR::Opcode::Vu0ControlWrite:
							case RegionIR::Opcode::Vu0DenormalizeStatus:
							case RegionIR::Opcode::BindVu0Q:
							case RegionIR::Opcode::BindVu0ViQ:
							case RegionIR::Opcode::BindVu0Vf:
						case RegionIR::Opcode::BindVu0Acc:
						case RegionIR::Opcode::BindVu0MacFlag:
						case RegionIR::Opcode::BindVu0StatusFlag:
						case RegionIR::Opcode::BindVu0ViMac:
						case RegionIR::Opcode::BindVu0ViStatus:
						case RegionIR::Opcode::BindVu0Vi:
						case RegionIR::Opcode::BindVu0ClipFlag:
						case RegionIR::Opcode::BindVu0MicroStatusFlag:
							return true;
						case RegionIR::Opcode::MemoryLoad:
						case RegionIR::Opcode::MemoryStore:
						{
							const RegionIR::MemoryAccessKind kind =
								static_cast<RegionIR::MemoryAccessKind>(node.immediate);
							if (kind == RegionIR::MemoryAccessKind::LoadVu0Vector ||
								kind == RegionIR::MemoryAccessKind::StoreVu0Vector)
							{
								return true;
							}
							break;
						}
						default:
							break;
					}
				}
			}
			return false;
		}

		bool IsRuntimeStateClassSupported(RegionExecution::StateClass state_class)
		{
			switch (state_class)
			{
				case RegionExecution::StateClass::Gpr:
				case RegionExecution::StateClass::Hi:
				case RegionExecution::StateClass::Lo:
				case RegionExecution::StateClass::Sa:
				case RegionExecution::StateClass::Fpr:
				case RegionExecution::StateClass::Fcr0:
				case RegionExecution::StateClass::Fcr31:
				case RegionExecution::StateClass::Acc:
				case RegionExecution::StateClass::AccFlag:
				case RegionExecution::StateClass::Cycle:
					return true;
				case RegionExecution::StateClass::Vu0Vf:
				case RegionExecution::StateClass::Vu0Acc:
				case RegionExecution::StateClass::Vu0MacFlag:
				case RegionExecution::StateClass::Vu0StatusFlag:
				case RegionExecution::StateClass::Vu0ClipFlag:
				case RegionExecution::StateClass::Vu0Q:
				case RegionExecution::StateClass::Vu0Vi:
				case RegionExecution::StateClass::Vu0MicroMacFlag:
				case RegionExecution::StateClass::Vu0MicroClipFlag:
				case RegionExecution::StateClass::Vu0MicroStatusFlag:
					return true;
			}
			return false;
		}

		bool RuntimeStateWordOffsets(size_t slot, u8 word,
			u16* canonical_offset, u16* runtime_offset,
			RegionA32::CompileOptions::PersistentStateBase* runtime_base)
		{
			if (!canonical_offset || !runtime_offset || !runtime_base ||
				slot >= RegionExecution::STATE_SLOT_COUNT ||
				word >= RegionExecution::StateWordCount(slot))
			{
				return false;
			}

			const size_t canonical =
				RegionExecution::CanonicalStateWordOffset(slot, word);
			if (canonical == SIZE_MAX || canonical > UINT16_MAX)
				return false;

			const RegionExecution::StateSlot decoded =
				RegionExecution::DecodeStateSlot(slot);
			static_assert(offsetof(cpuRegistersPack, cpuRegs) == 0);
			constexpr size_t FPU_BASE = offsetof(cpuRegistersPack, fpuRegs);
			size_t runtime = 0;
			*runtime_base =
				RegionA32::CompileOptions::PersistentStateBase::CpuRegisters;
			switch (decoded.state_class)
			{
				case RegionExecution::StateClass::Gpr:
					runtime = offsetof(cpuRegisters, GPR) +
						decoded.index * sizeof(GPR_reg) + word * sizeof(u32);
					break;
				case RegionExecution::StateClass::Hi:
					runtime = offsetof(cpuRegisters, HI) + word * sizeof(u32);
					break;
				case RegionExecution::StateClass::Lo:
					runtime = offsetof(cpuRegisters, LO) + word * sizeof(u32);
					break;
				case RegionExecution::StateClass::Sa:
					if (word != 0)
						return false;
					runtime = offsetof(cpuRegisters, sa);
					break;
				case RegionExecution::StateClass::Fpr:
					if (word != 0)
						return false;
					runtime = FPU_BASE + offsetof(fpuRegisters, fpr) +
						decoded.index * sizeof(FPRreg);
					break;
				case RegionExecution::StateClass::Fcr0:
				case RegionExecution::StateClass::Fcr31:
					if (word != 0)
						return false;
					runtime = FPU_BASE + offsetof(fpuRegisters, fprc) +
						(decoded.state_class == RegionExecution::StateClass::Fcr31 ?
							31u : 0u) * sizeof(u32);
					break;
				case RegionExecution::StateClass::Acc:
					if (word != 0)
						return false;
					runtime = FPU_BASE + offsetof(fpuRegisters, ACC);
					break;
				case RegionExecution::StateClass::AccFlag:
					if (word != 0)
						return false;
					runtime = FPU_BASE + offsetof(fpuRegisters, ACCflag);
					break;
				case RegionExecution::StateClass::Cycle:
					if (word >= 2)
						return false;
					runtime = offsetof(cpuRegisters, cycle) + word * sizeof(u32);
					break;
				case RegionExecution::StateClass::Vu0Vf:
					runtime = offsetof(VURegs, VF) +
						decoded.index * sizeof(VECTOR) + word * sizeof(u32);
					*runtime_base =
						RegionA32::CompileOptions::PersistentStateBase::Vu0;
					break;
				case RegionExecution::StateClass::Vu0Acc:
					runtime = offsetof(VURegs, ACC) + word * sizeof(u32);
					*runtime_base =
						RegionA32::CompileOptions::PersistentStateBase::Vu0;
					break;
				case RegionExecution::StateClass::Vu0MacFlag:
					if (word != 0)
						return false;
					runtime = offsetof(VURegs, macflag);
					*runtime_base =
						RegionA32::CompileOptions::PersistentStateBase::Vu0;
					break;
				case RegionExecution::StateClass::Vu0StatusFlag:
					if (word != 0)
						return false;
					runtime = offsetof(VURegs, statusflag);
					*runtime_base =
						RegionA32::CompileOptions::PersistentStateBase::Vu0;
					break;
				case RegionExecution::StateClass::Vu0ClipFlag:
					if (word != 0)
						return false;
					runtime = offsetof(VURegs, clipflag);
					*runtime_base =
						RegionA32::CompileOptions::PersistentStateBase::Vu0;
					break;
				case RegionExecution::StateClass::Vu0Q:
					if (word != 0)
						return false;
					runtime = offsetof(VURegs, q);
					*runtime_base =
						RegionA32::CompileOptions::PersistentStateBase::Vu0;
					break;
				case RegionExecution::StateClass::Vu0Vi:
					if (word != 0)
						return false;
					runtime = offsetof(VURegs, VI) +
						decoded.index * sizeof(REG_VI);
					*runtime_base =
						RegionA32::CompileOptions::PersistentStateBase::Vu0;
					break;
				case RegionExecution::StateClass::Vu0MicroMacFlag:
				case RegionExecution::StateClass::Vu0MicroClipFlag:
				case RegionExecution::StateClass::Vu0MicroStatusFlag:
					if (word != 0 || decoded.index >= 4)
						return false;
					runtime =
						(decoded.state_class == RegionExecution::StateClass::Vu0MicroMacFlag ?
							offsetof(VURegs, micro_macflags) :
						 decoded.state_class == RegionExecution::StateClass::Vu0MicroClipFlag ?
							offsetof(VURegs, micro_clipflags) :
							offsetof(VURegs, micro_statusflags)) +
						decoded.index * sizeof(u32);
					*runtime_base =
						RegionA32::CompileOptions::PersistentStateBase::Vu0;
					break;
				default:
					return false;
			}
			// Persistent generated state accesses currently use one A32 LDR/STR
			// immediate from r4. Fail closed if the canonical pack ever outgrows that
			// encoding rather than silently manufacturing a different ABI.
			if (runtime > 4095)
				return false;
			*canonical_offset = static_cast<u16>(canonical);
			*runtime_offset = static_cast<u16>(runtime);
			return true;
		}

		bool BuildRuntimeStateTransferPlan(
			const RegionA32::CompileResult& compiled, StateTransferPlan* plan)
		{
			if (!plan || !compiled.uses_state_word_contract)
				return false;
			*plan = {};
			try
			{
				plan->entry.reserve(compiled.entry_state_word_count);
				plan->output.reserve(compiled.output_state_word_count);
			}
			catch (const std::bad_alloc&)
			{
				*plan = {};
				return false;
			}
			auto append = [&](std::vector<StateWordTransfer>& transfers,
				u16* count, size_t slot, u8 word) {
				if (!count || transfers.size() >= StateTransferPlan::MAX_WORDS)
					return false;
				try
				{
					transfers.emplace_back();
				}
				catch (const std::bad_alloc&)
				{
					return false;
				}
				StateWordTransfer& transfer = transfers.back();
				if (!RuntimeStateWordOffsets(slot, word,
						&transfer.canonical_offset, &transfer.runtime_offset,
						&transfer.runtime_base))
				{
					transfers.pop_back();
					return false;
				}
				*count = static_cast<u16>(transfers.size());
				return true;
			};

			for (size_t slot = 0; slot < RegionExecution::STATE_SLOT_COUNT; slot++)
			{
				const u8 entry_mask = compiled.entry_state_words[slot];
				const u8 output_mask = compiled.output_state_words[slot];
				if ((entry_mask | output_mask) != 0 &&
					!IsRuntimeStateClassSupported(
						RegionExecution::DecodeStateSlot(slot).state_class))
				{
					return false;
				}
				for (u8 word = 0; word < RegionExecution::StateWordCount(slot); word++)
				{
					if ((entry_mask & (1u << word)) != 0 &&
						!append(plan->entry, &plan->entry_count, slot, word))
					{
						return false;
					}
					if ((output_mask & (1u << word)) != 0 &&
						!append(plan->output, &plan->output_count, slot, word))
					{
						return false;
					}
				}
			}
			return plan->entry_count == compiled.entry_state_word_count &&
			       plan->output_count == compiled.output_state_word_count;
		}

		bool BuildPersistentStateWordsImpl(
			std::array<RegionA32::CompileOptions::PersistentStateWord,
				StateTransferPlan::MAX_WORDS + 1>* words,
			size_t* word_count)
		{
			if (!words || !word_count)
				return false;
			*word_count = 0;
			for (size_t slot = 0; slot < RegionExecution::STATE_SLOT_COUNT; slot++)
			{
				if (!IsRuntimeStateClassSupported(
						RegionExecution::DecodeStateSlot(slot).state_class))
				{
					continue;
				}
				for (u8 word = 0; word < RegionExecution::StateWordCount(slot); word++)
				{
					if (*word_count >= words->size())
						return false;
					u16 canonical = 0;
					u16 runtime = 0;
					RegionA32::CompileOptions::PersistentStateBase runtime_base{};
					if (!RuntimeStateWordOffsets(slot, word, &canonical, &runtime,
							&runtime_base))
						return false;
					(*words)[(*word_count)++] = {
						canonical, runtime, runtime_base};
				}
			}
			if (*word_count >= words->size() ||
				offsetof(RegionIR::CanonicalState, pc) > UINT16_MAX ||
				offsetof(cpuRegisters, pc) > UINT16_MAX)
			{
				return false;
			}
			(*words)[(*word_count)++] = {
				static_cast<u16>(offsetof(RegionIR::CanonicalState, pc)),
				static_cast<u16>(offsetof(cpuRegisters, pc)),
			};
			return true;
		}

		bool LoadRuntimeStateContract(const StateTransferPlan& plan,
			RegionIR::CanonicalState* state)
		{
			if (!state)
				return false;
			u8* const canonical = reinterpret_cast<u8*>(state);
			const u8* const runtime = reinterpret_cast<const u8*>(&cpuRegs);
			const u8* const vu0 = reinterpret_cast<const u8*>(&VU0);
			for (u16 index = 0; index < plan.entry_count; index++)
			{
				const StateWordTransfer& transfer = plan.entry[index];
				const u8* const source = transfer.runtime_base ==
					RegionA32::CompileOptions::PersistentStateBase::Vu0 ? vu0 : runtime;
				std::memcpy(canonical + transfer.canonical_offset,
					source + transfer.runtime_offset, sizeof(u32));
			}
			return true;
		}

		bool StoreRuntimeStateContract(const StateTransferPlan& plan,
			const RegionIR::CanonicalState& state)
		{
			const u8* const canonical = reinterpret_cast<const u8*>(&state);
			u8* const runtime = reinterpret_cast<u8*>(&cpuRegs);
			u8* const vu0 = reinterpret_cast<u8*>(&VU0);
			for (u16 index = 0; index < plan.output_count; index++)
			{
				const StateWordTransfer& transfer = plan.output[index];
				u8* const destination = transfer.runtime_base ==
					RegionA32::CompileOptions::PersistentStateBase::Vu0 ? vu0 : runtime;
				std::memcpy(destination + transfer.runtime_offset,
					canonical + transfer.canonical_offset, sizeof(u32));
			}
			return true;
		}

	} // namespace

	bool BuildPersistentStateWordMap(
		std::array<RegionA32::CompileOptions::PersistentStateWord,
			StateTransferPlan::MAX_WORDS + 1>* words,
		size_t* word_count)
	{
		return BuildPersistentStateWordsImpl(words, word_count);
	}

	Runtime::Runtime(BlockExecutor* executor)
		: m_executor(executor)
	{
		m_entry_lookup.fill(UINT8_MAX);
#if defined(VITASX2_EE_REGION_VU0_VALIDATION)
		m_allow_uncertified_vu0_for_validation = true;
#endif
#if defined(VITASX2_EE_REGION_TARGET_COST_DIAGNOSTIC)
		m_emit_unproven_target_cost_for_validation = true;
#endif
#if defined(VITASX2_EE_SEMANTIC_KERNEL_VALIDATION)
		m_semantic_kernel_lowering_enabled = true;
#endif
	}

	RegionIR::LiftOptions Runtime::CurrentOptions()
	{
		RegionIR::LiftOptions options{};
		options.cycle_factor = 2 - ((cpuRegs.CP0.n.Config >> 18) & 1u);
		options.ee_cycle_rate = EmuConfig.Speedhacks.EECycleRate;
		options.goemon_tlb_hack = EmuConfig.Gamefixes.GoemonTlbHack;
		options.ee_cache_enabled = EmuConfig.Cpu.Recompiler.EnableEECache;
		options.vu0_overflow_clamp = CHECK_VU_OVERFLOW(0);
		options.max_blocks =
			PersistentGeneratedEntry::MAX_INTERNAL_SOURCE_BLOCKS;
		options.max_source_instructions = MAX_REGION_SOURCE_WORDS;
		// RegionIR already verifies each bounded direct callee independently. Product
		// formation used to retain only the first JAL seed, which made an otherwise
		// valid natural loop depend on which call happened to appear first. Bound the
		// general contract by the same small block/source budget instead.
		options.max_direct_calls = 8;
		return options;
	}

	bool Runtime::OptionsEqual(const RegionIR::LiftOptions& left,
		const RegionIR::LiftOptions& right)
	{
		return left.cycle_factor == right.cycle_factor &&
		       left.ee_cycle_rate == right.ee_cycle_rate &&
		       left.goemon_tlb_hack == right.goemon_tlb_hack &&
		       left.ee_cache_enabled == right.ee_cache_enabled &&
		       left.vu0_overflow_clamp == right.vu0_overflow_clamp &&
		       left.cop1_lazy_ou_guards == right.cop1_lazy_ou_guards &&
		       left.max_blocks == right.max_blocks &&
		       left.max_source_instructions == right.max_source_instructions &&
		       left.max_direct_calls == right.max_direct_calls;
	}

	void Runtime::ClearCandidateHistory()
	{
		for (ProbeCandidate& probe : m_probes)
			RetireProbeCode(&probe);
		m_pending = {};
		m_probes = {};
		m_probe_peaks = {};
		m_forward_samples = {};
		m_negative_candidates = {};
		m_deferred_candidates = {};
		m_negative_insert = 0;
		m_build_budget_cycle = cpuRegs.cycle;
		m_build_tokens = m_build_token_capacity;
		m_pending_budget_deferred = false;
		m_target_cost_analyses_generation = 0;
		m_bypass_once_pc = UINT32_MAX;
		m_probe_serial = 0;
		m_probe_peak_serial = 0;
		m_deferred_progress_serial = 0;
		m_armed_probe_count = 0;
		m_armed_probes.fill(nullptr);
		m_probe_sample_event_countdown = PROBE_SAMPLE_EVENT_INTERVAL;
		m_forward_sample_event_countdown = PROBE_SAMPLE_EVENT_INTERVAL;
		m_forward_sample_request_pc = UINT32_MAX;
		m_publication_dirty = false;
		m_generated_maintenance_requested = 0;
		m_probe_epoch = 1;
		m_event_scoped_probe_count = 0;
	}

	bool Runtime::UseBarrierProbesForValidation() const
	{
#if defined(VITASX2_QEMU_VALIDATION)
		return m_callable_execution_for_validation;
#else
		return false;
#endif
	}

	u32 Runtime::ObservedRegionWorkFloor() const
	{
#if defined(VITASX2_QEMU_VALIDATION)
		// Existing admission fixtures express their threshold in latch traversals.
		// Twelve work units is the reference fixture iteration; product builds never
		// use this compatibility conversion.
		return std::max<u32>(1, m_minimum_backedge_observations) * 12u;
#else
		return MINIMUM_EVENT_SCOPED_REGION_WORK;
#endif
	}

	void Runtime::AdvanceEventScopedProbeEpoch()
	{
		if (m_event_scoped_probe_count == 0)
			return;
		RecordEventScopedProbeMaximum();
		if (++m_probe_epoch == 0)
			m_probe_epoch = 1;
	}

	void Runtime::RecordEventScopedProbeMaximum()
	{
		for (u32 index = 0; index < m_armed_probe_count; index++)
		{
			ProbeCandidate* const probe = m_armed_probes[index];
			if (!probe || !probe->valid || !probe->armed ||
				!probe->event_scoped_observations ||
				probe->observation_epoch != m_probe_epoch)
			{
				continue;
			}
			probe->maximum_observations = std::max(
				probe->maximum_observations, probe->observations);
			RecordProbePeak(*probe);
			m_statistics.maximum_event_scoped_probe_observations = std::max<u64>(
				m_statistics.maximum_event_scoped_probe_observations,
				probe->observations);
		}
	}

	void Runtime::RecordProbePeak(const ProbeCandidate& probe)
	{
		if (!probe.valid || probe.maximum_observations == 0)
			return;

		ProbePeak* slot = nullptr;
		for (ProbePeak& peak : m_probe_peaks)
		{
			if (peak.valid && peak.snapshot.entry_pc == probe.candidate.entry_pc &&
				peak.snapshot.source_end_pc == probe.candidate.source_end_pc)
			{
				slot = &peak;
				break;
			}
			if (!peak.valid && !slot)
				slot = &peak;
		}
		if (!slot)
		{
			slot = &*std::min_element(m_probe_peaks.begin(), m_probe_peaks.end(),
				[](const ProbePeak& left, const ProbePeak& right) {
					if (left.snapshot.maximum_observations !=
						right.snapshot.maximum_observations)
					{
						return left.snapshot.maximum_observations <
							right.snapshot.maximum_observations;
					}
					return left.serial < right.serial;
				});
			if (slot->snapshot.maximum_observations > probe.maximum_observations)
				return;
		}

		const u32 retained_maximum = std::max(
			slot->snapshot.maximum_observations, probe.maximum_observations);
		slot->snapshot.entry_pc = probe.candidate.entry_pc;
		slot->snapshot.backedge_pc = probe.candidate.backedge_block_pc;
		slot->snapshot.source_end_pc = probe.candidate.source_end_pc;
		slot->snapshot.observations = probe.observations;
		slot->snapshot.maximum_observations = retained_maximum;
		slot->snapshot.samples = probe.samples;
		slot->snapshot.required_observations = probe.required_observations != 0 ?
			probe.required_observations :
			(probe.has_iteration_guard ? 1u : m_minimum_backedge_observations);
		slot->snapshot.internal_source_blocks = probe.internal_source_block_count;
		slot->snapshot.guarded = probe.has_iteration_guard;
		slot->snapshot.event_scoped = probe.event_scoped_observations;
		slot->snapshot.armed = probe.armed;
		slot->snapshot.dormant = probe.dormant;
		slot->serial = ++m_probe_peak_serial;
		slot->valid = true;
	}

	bool Runtime::ResetCode()
	{
		bool changed = false;
		for (Entry& entry : m_entries)
		{
			if (entry.active)
			{
				AccumulateEntryShape(&m_profile_window_shapes, entry,
					EntryProfileWindowExecutions(entry));
				m_statistics.executions += static_cast<u32>(
					entry.generated_dispatch_executions -
					entry.reported_generated_dispatch_executions);
					m_statistics.continuation_failures += static_cast<u32>(
						entry.generated_continuation_failures -
						entry.reported_generated_continuation_failures);
					m_statistics.profitability_fallbacks += static_cast<u32>(
						entry.generated_profitability_fallbacks -
						entry.reported_generated_profitability_fallbacks);
				m_statistics.entry_state_fallbacks += static_cast<u32>(
					entry.generated_entry_state_fallbacks -
					entry.reported_generated_entry_state_fallbacks);
			}
			changed |= entry.active;
			RetireEntryCode(&entry);
			entry = {};
		}
		RebuildEntryLookup();
		if (m_executor)
			m_executor->RetirePersistentRegionContinuations();
		changed |= std::any_of(m_probes.begin(), m_probes.end(),
			[](const ProbeCandidate& probe) { return probe.armed; });
		ClearCandidateHistory();
		m_source_generation = m_executor ? m_executor->GetSourceGeneration() : 0;
		m_options = CurrentOptions();
		return changed;
	}

	void Runtime::ResetStatistics()
	{
		m_statistics = {};
		m_profile_window_shapes = {};
		m_probe_peaks = {};
		m_use_serial = 0;
		m_probe_peak_serial = 0;
		m_repeated_candidate_serial = 0;
		for (ProbeCandidate& probe : m_probes)
			probe.maximum_observations = 0;
		for (Entry& entry : m_entries)
		{
			entry.reported_executions = entry.executions;
			entry.reported_work_units = entry.work_units;
			entry.profile_reported_executions = entry.executions;
			entry.reported_generated_dispatch_executions =
				entry.generated_dispatch_executions;
			entry.profile_reported_generated_dispatch_executions =
				entry.generated_dispatch_executions;
			entry.profile_reported_generated_counted_iterations =
				entry.generated_counted_iterations;
				entry.reported_generated_continuation_failures =
					entry.generated_continuation_failures;
				entry.reported_generated_profitability_fallbacks =
					entry.generated_profitability_fallbacks;
			entry.reported_generated_entry_state_fallbacks =
				entry.generated_entry_state_fallbacks;
		}
	}

	void Runtime::BeginStatisticsWindow()
	{
		m_profile_window_shapes = {};
		for (Entry& entry : m_entries)
		{
			entry.profile_reported_executions = entry.executions;
			entry.profile_reported_generated_dispatch_executions =
				entry.generated_dispatch_executions;
			entry.profile_reported_generated_counted_iterations =
				entry.generated_counted_iterations;
		}
	}

	bool Runtime::Synchronize()
	{
		if (!m_executor)
			return ResetCode();

		const u32 generation = m_executor->GetSourceGeneration();
		const RegionIR::LiftOptions options = CurrentOptions();
		if (m_source_generation == 0)
		{
			m_source_generation = generation;
			m_options = options;
			return false;
		}
		if (m_source_generation == generation && OptionsEqual(m_options, options))
			return false;

		const bool changed = ResetCode();
		m_source_generation = generation;
		m_options = options;
		m_statistics.generation_resets++;
		return changed;
	}

	bool Runtime::DecodeBackwardBranchTo(
		u32 branch_pc, u32 opcode, u32 target_pc)
	{
		const u32 primary = opcode >> 26;
		bool relative_conditional = false;
		if (primary == 0x01)
		{
			const u32 rs = (opcode >> 21) & 0x1fu;
			const u32 rt = (opcode >> 16) & 0x1fu;
			const bool regimm = rt <= 0x03 || (rt >= 0x10 && rt <= 0x13);
			// PCSX2's EE decoder and RegionIR::IsConditionalBranch() both leave
			// linked REGIMM forms with rs==r31 to tier zero: SCE defines that
			// source/link-register alias as undefined. Discovery must use the same
			// semantic surface as the eventual lift rather than accepting a broader
			// opcode family and failing later for an unrelated reason.
			relative_conditional = regimm && !(rt >= 0x10 && rs == 31);
		}
		else
		{
			// BEQ/BNE/BLEZ/BGTZ and their likely forms all share the signed
			// PC-relative target encoding. These are precisely the remaining
			// branch families owned by RegionIR::IsConditionalBranch().
			relative_conditional =
				(primary >= 0x04 && primary <= 0x07) ||
				(primary >= 0x14 && primary <= 0x17);
		}
		if (!relative_conditional)
		{
			return false;
		}
		const s32 displacement = static_cast<s16>(opcode) * 4;
		const u32 decoded_target = branch_pc + sizeof(u32) +
			static_cast<u32>(displacement);
		return decoded_target == target_pc && target_pc <= branch_pc;
	}

	bool Runtime::QueueBackwardCandidate(u32 backedge_block_pc,
		u32 branch_pc, u32 source_end_pc, u32 target_pc,
		bool observed_backedge)
	{
		const Entry* const active_entry = FindEntry(target_pc);
		if (source_end_pc <= target_pc ||
			(active_entry && source_end_pc <= active_entry->source_end_pc) ||
			IsNegativeCandidate(target_pc, source_end_pc,
				CandidateKind::NaturalLoop) ||
			IsDeferredCandidate(target_pc, source_end_pc,
				CandidateKind::NaturalLoop))
		{
			return false;
		}
		for (const ProbeCandidate& existing : m_probes)
		{
			if (!existing.valid ||
				existing.candidate.kind != CandidateKind::ForwardReducible)
			{
				continue;
			}
			// A caller region and a separately entered direct callee are valid
			// first-class owners; final direct-call expansion already permits that
			// overlap and authoritative source invalidation tracks both. Do not let
			// an unproven caller probe permanently consume a callee's one-time cold
			// discovery event. Only an identical generated entry is mutually
			// exclusive in the persistent lookup.
			if (target_pc == existing.candidate.entry_pc &&
				!(active_entry && observed_backedge))
				return false;
		}
		if (m_pending.valid)
		{
			if (m_pending.entry_pc != target_pc)
				return false;
			if (source_end_pc <= m_pending.source_end_pc)
				return source_end_pc == m_pending.source_end_pc;
			// A second source-backed latch can expose a wider natural loop before
			// the first candidate reaches its cold build boundary. Keep the wider
			// ownership contract and discard no executable code here.
			m_pending = {target_pc, backedge_block_pc, branch_pc,
				source_end_pc, UINT32_MAX, true};
			m_pending_budget_deferred = false;
			m_statistics.candidates++;
			return true;
		}

		if (active_entry && observed_backedge)
		{
			// The currently published entry is itself stronger hotness evidence than a
			// second generated probe at the same lookup PC, and this exact wider
			// backedge was just executed. A same-PC probe cannot receive control while
			// the existing region owns the lookup entry; arming one only consumes a
			// scarce generated-owner slot until idle ageing removes it. Build the wider
			// source contract at the required outer boundary instead. Its independent
			// target-cost certificate and entry guard remain authoritative, and the old
			// region remains published if the replacement is rejected or fails.
			m_pending = {target_pc, backedge_block_pc, branch_pc,
				source_end_pc, UINT32_MAX, true};
			m_pending_budget_deferred = false;
			m_statistics.candidates++;
			return true;
		}

		ProbeCandidate* probe = FindProbe(target_pc, source_end_pc);
		bool created_probe = false;
		if (!probe)
		{
			probe = SelectProbeSlot();
			if (!probe)
				return false;
			if (probe->valid)
			{
				m_statistics.probe_evictions++;
				RemoveProbe(probe);
			}
			*probe = {};
			probe->candidate = {target_pc, backedge_block_pc, branch_pc,
				source_end_pc, UINT32_MAX, true};
			probe->valid = true;
			created_probe = true;
			probe->last_observation_serial = ++m_probe_serial;
			m_statistics.candidates++;
			if (UseBarrierProbesForValidation())
				ArmQueuedProbes();
		}

		if (observed_backedge)
		{
			probe->observations++;
			probe->idle_outer_boundaries = 0;
			probe->last_observation_serial = ++m_probe_serial;
			m_statistics.probe_observations++;
		}
		if (probe->observations < m_minimum_backedge_observations)
		{
			// A newly discovered cold product candidate needs one outer transaction
			// to allocate and publish its bounded generated probe. If validation sets
			// the threshold to zero, promote directly without claiming executable
			// ownership changed merely because transient candidate metadata existed.
			// Queued metadata is not executable ownership. Once the bounded generated
			// directory is full, a newly discovered candidate must not unwind the live
			// dispatcher merely to discover that there is no probe slot to publish.
			// Removing or promoting an armed owner marks publication dirty and the same
			// outer transaction then rotates the oldest queued candidate into its slot.
			if (created_probe && !UseBarrierProbesForValidation() &&
				m_armed_probe_count < MAX_ARMED_PROBES)
				m_publication_dirty = true;
			return UseBarrierProbesForValidation() ?
				(m_publication_dirty || observed_backedge) :
				m_publication_dirty;
		}

		m_pending = probe->candidate;
		m_pending_budget_deferred = false;
		RemoveProbe(probe);
		m_statistics.hot_promotions++;
		return true;
	}

	bool Runtime::HasEntryAtPc(u32 pc) const
	{
		return FindEntry(pc) != nullptr;
	}

	size_t Runtime::FindEntryIndex(u32 pc) const
	{
		const size_t mask = ENTRY_LOOKUP_CAPACITY - 1;
		size_t slot = ((pc >> 2) * 2654435761u) & mask;
		for (size_t probe = 0; probe < ENTRY_LOOKUP_CAPACITY; probe++)
		{
			const u8 index = m_entry_lookup[slot];
			if (index == UINT8_MAX)
				return MAX_REGIONS;
			if (index < m_entries.size())
			{
				const Entry& entry = m_entries[index];
				if (entry.active && entry.entry_pc == pc)
					return index;
			}
			slot = (slot + 1) & mask;
		}
		return MAX_REGIONS;
	}

	void Runtime::RebuildEntryLookup()
	{
		m_entry_lookup.fill(UINT8_MAX);
		const size_t mask = ENTRY_LOOKUP_CAPACITY - 1;
		for (size_t index = 0; index < m_entries.size(); index++)
		{
			if (!m_entries[index].active)
				continue;
			size_t slot = ((m_entries[index].entry_pc >> 2) * 2654435761u) & mask;
			while (m_entry_lookup[slot] != UINT8_MAX)
				slot = (slot + 1) & mask;
			m_entry_lookup[slot] = static_cast<u8>(index);
		}
	}

	bool Runtime::HasCanonicalEntry(u32 canonical_pc) const
	{
		for (const Entry& entry : m_entries)
		{
			if (entry.active && entry.canonical_entry_pc == canonical_pc)
				return true;
		}
		return false;
	}

	Runtime::ProbeCandidate* Runtime::FindProbe(
		u32 entry_pc, u32 source_end_pc, CandidateKind kind)
	{
		for (ProbeCandidate& probe : m_probes)
		{
			if (probe.valid && probe.candidate.entry_pc == entry_pc &&
				probe.candidate.source_end_pc == source_end_pc &&
				probe.candidate.kind == kind)
			{
				return &probe;
			}
		}
		return nullptr;
	}

	const Runtime::ProbeCandidate* Runtime::FindProbe(
		u32 entry_pc, u32 source_end_pc, CandidateKind kind) const
	{
		for (const ProbeCandidate& probe : m_probes)
		{
			if (probe.valid && probe.candidate.entry_pc == entry_pc &&
				probe.candidate.source_end_pc == source_end_pc &&
				probe.candidate.kind == kind)
			{
				return &probe;
			}
		}
		return nullptr;
	}

	Runtime::ProbeCandidate* Runtime::SelectProbeSlot()
	{
#if defined(VITASX2_QEMU_VALIDATION)
		if (m_admission_capacity_blocked_for_validation)
			return nullptr;
#endif
		(void)RepairArmedProbeDirectory();
		ProbeCandidate* victim = nullptr;
		for (ProbeCandidate& probe : m_probes)
		{
			if (!probe.valid)
				return &probe;
			if (!probe.armed && (!victim ||
				probe.last_observation_serial < victim->last_observation_serial))
			{
				victim = &probe;
			}
		}
		return victim;
	}

	bool Runtime::RepairArmedProbeDirectory()
	{
		// m_probes[].armed is the executable-ownership truth consumed by
		// CopyPersistentGeneratedEntries(); m_armed_probes is its bounded cold
		// directory. A missing pointer must never turn an otherwise removable owner
		// into a permanent ghost. Prefer the already ordered live directory, then add
		// any valid armed owner it omitted. If corruption left more than the product
		// limit marked armed, queue the extras and quarantine their code until the
		// corrected generated directory is republished.
		std::array<ProbeCandidate*, MAX_ARMED_PROBES> rebuilt{};
		u32 rebuilt_count = 0;
		auto append = [&](ProbeCandidate* probe) {
			if (!probe || !probe->valid || !probe->armed ||
				std::find(rebuilt.begin(), rebuilt.begin() + rebuilt_count, probe) !=
					rebuilt.begin() + rebuilt_count)
			{
				return;
			}
			if (rebuilt_count < rebuilt.size())
				rebuilt[rebuilt_count++] = probe;
		};
		for (u32 index = 0; index <
				std::min<u32>(m_armed_probe_count, m_armed_probes.size()); index++)
		{
			append(m_armed_probes[index]);
		}
		for (ProbeCandidate& probe : m_probes)
			append(&probe);

		bool changed = rebuilt_count != m_armed_probe_count;
		for (u32 index = 0; !changed && index < rebuilt.size(); index++)
			changed = rebuilt[index] != m_armed_probes[index];

		for (ProbeCandidate& probe : m_probes)
		{
			if (!probe.valid || !probe.armed ||
				std::find(rebuilt.begin(), rebuilt.begin() + rebuilt_count, &probe) !=
					rebuilt.begin() + rebuilt_count)
			{
				continue;
			}
			probe.armed = false;
			probe.dormant = false;
			RetireProbeCode(&probe);
			changed = true;
		}
		if (!changed)
			return false;

		m_armed_probes = rebuilt;
		m_armed_probe_count = rebuilt_count;
		m_statistics.probe_directory_repairs++;
		m_publication_dirty = true;
		return true;
	}

	void Runtime::ArmQueuedProbes()
	{
		(void)RepairArmedProbeDirectory();
		while (m_armed_probe_count < MAX_ARMED_PROBES)
		{
			ProbeCandidate* next = nullptr;
			for (ProbeCandidate& probe : m_probes)
			{
				if (!probe.valid || probe.armed)
					continue;
				if (probe.dormant &&
					static_cast<s32>(cpuRegs.cycle - probe.retry_cycle) < 0)
				{
					continue;
				}
				if (!next || probe.last_observation_serial <
					next->last_observation_serial)
				{
					next = &probe;
				}
			}
			if (!next)
				break;
			if (next->dormant)
			{
				next->dormant = false;
				next->samples = 0;
				next->observations = 0;
				next->reported_observations = 0;
				m_statistics.probe_sample_retries++;
			}
			next->armed = true;
			next->idle_outer_boundaries = 0;
			m_armed_probes[m_armed_probe_count++] = next;
			m_publication_dirty = true;
		}
	}

	bool Runtime::EmitProductProbeEntry(ProbeCandidate* probe)
	{
		if (!probe || !probe->valid || !probe->armed || !m_executor ||
			probe->code.EntryPoint())
		{
			return false;
		}
		// Admission policy owns the counters, while BlockExecutor owns and emits the
		// private persistent-dispatch ABI. A guarded product entry samples one
		// external invocation entirely in A32; the next existing scheduler event then
		// asks the outer owner to replace this entry transactionally.
		constexpr size_t PROBE_CODE_CAPACITY =
			BlockExecutor::REGION_PROBE_CODE_BUFFER_CAPACITY;
		VitaA32::CodeBuffer code;
		size_t slice_offset = 0;
		if (!m_executor->PrepareRegionCodeBuffer(
				&code, PROBE_CODE_CAPACITY, &slice_offset))
		{
			return false;
		}
		size_t entry_offset = static_cast<size_t>(-1);
		size_t compatible_entry_offset = static_cast<size_t>(-1);
		GprLinkSignature compatible_signature{};
		// Observe the decoded natural-loop latch rather than its header. This counts
		// one real backedge decision per iteration and avoids treating unrelated
		// external entries at the same header as loop heat.
		const u32 probe_pc = probe->has_iteration_guard ?
			probe->candidate.entry_pc : probe->candidate.backedge_block_pc;
		const u32 required_observations = probe->required_observations != 0 ?
			probe->required_observations :
			(probe->has_iteration_guard ? 1u : m_minimum_backedge_observations);
		bool emitted = m_executor->AppendPersistentTierZeroProbe(&code,
			probe_pc, &probe->observations,
			probe->event_scoped_observations ? &probe->observation_epoch : nullptr,
			probe->event_scoped_observations ? &m_probe_epoch : nullptr,
			probe->has_iteration_guard ? &probe->samples : nullptr,
			&m_generated_maintenance_requested,
			required_observations,
			probe->has_iteration_guard ? COUNTED_PROBE_INVOCATION_BUDGET : 0,
			probe->has_iteration_guard ? &probe->iteration_guard : nullptr,
			&entry_offset,
			&compatible_entry_offset, &compatible_signature);
		emitted = emitted && !code.OutOfSpace() && code.Flush();
		if (!emitted || !m_executor->CommitRegionCodeBuffer(
				slice_offset, code.Size()))
		{
			m_executor->DiscardRegionCodeBuffer(&code, slice_offset);
			return false;
		}
		probe->persistent_entry_offset = entry_offset;
		probe->compatible_entry_offset = compatible_entry_offset;
		probe->compatible_signature = compatible_signature;
		probe->code_slot_offset = slice_offset;
		probe->code = std::move(code);
		return true;
	}

	bool Runtime::PrepareProductProbeEntries()
	{
		if (UseBarrierProbesForValidation())
			return false;
		bool changed = false;
		for (;;)
		{
			ArmQueuedProbes();
			ProbeCandidate* missing = nullptr;
			for (u32 index = 0; index < m_armed_probe_count; index++)
			{
				ProbeCandidate* const probe = m_armed_probes[index];
				if (probe && probe->valid && probe->armed &&
					!probe->code.EntryPoint())
				{
					missing = probe;
					break;
				}
			}
			if (!missing)
				break;
			try
			{
				if (m_require_proven_profitability &&
					!missing->candidate.profitability_prescreened)
				{
					PersistentProbeIterationGuard guard{};
					std::array<u32,
						PersistentGeneratedEntry::MAX_INTERNAL_SOURCE_BLOCKS>
						internal_source_blocks{};
					u8 internal_source_block_count = 0;
					bool use_entry_guard = false;
					u32 required_observations = 0;
					bool event_scoped_observations = false;
					const ProfitabilityPrescreen prescreen =
						BuildProbeProfitabilityGuard(missing->candidate, &guard,
							&internal_source_blocks, &internal_source_block_count,
							&use_entry_guard, &required_observations,
							&event_scoped_observations);
					if (prescreen == ProfitabilityPrescreen::Rejected)
					{
						RememberProfitabilityPrescreenRejection(missing->candidate);
						RemoveProbe(missing);
						changed = true;
						continue;
					}
					if (prescreen == ProfitabilityPrescreen::Unknown)
					{
						// Direct-call and dependency-fragment candidates cannot be classified
						// from the discovery window alone. Assemble the same immutable final CFG
						// that product emission would own, but stop before code reservation or
						// token consumption. This keeps one semantic compiler contract instead
						// of teaching admission a reduced, workload-shaped approximation.
						ProfitabilityPrescreen final_prescreen =
							ProfitabilityPrescreen::Unknown;
#if defined(VITASX2_QEMU_VALIDATION)
						if (m_final_profitability_heap_failure_for_validation)
						{
							m_final_profitability_heap_failure_for_validation = false;
							throw std::bad_alloc();
						}
#endif
						if (BuildCandidate(missing->candidate, &final_prescreen, &guard,
								&internal_source_blocks,
								&internal_source_block_count, &use_entry_guard,
								&required_observations, &event_scoped_observations))
						{
							missing->iteration_guard = guard;
							missing->internal_source_blocks = internal_source_blocks;
							missing->internal_source_block_count =
								internal_source_block_count;
							missing->has_iteration_guard = use_entry_guard;
							missing->required_observations = required_observations;
							missing->event_scoped_observations =
								event_scoped_observations;
							m_event_scoped_probe_count +=
								missing->event_scoped_observations ? 1u : 0u;
							missing->candidate.profitability_prescreened = true;
							m_statistics.profitability_prescreen_passes++;
							RecordRepeatedCandidateOutcome(missing->candidate,
								RepeatedCandidateOutcome::ProfitabilityProven,
								UINT32_MAX, internal_source_block_count);
						}
						else
						{
							const bool deferred = IsDeferredCandidate(
								missing->candidate.entry_pc,
								missing->candidate.source_end_pc,
								missing->candidate.kind);
							if (!deferred &&
								final_prescreen == ProfitabilityPrescreen::Unknown)
							{
								m_statistics.profitability_prescreen_unknowns++;
								RecordRepeatedCandidateOutcome(missing->candidate,
									RepeatedCandidateOutcome::ProfitabilityUnknown);
							}
							RemoveProbe(missing);
							changed = true;
							continue;
						}
					}
					if (prescreen == ProfitabilityPrescreen::Proven)
					{
						missing->iteration_guard = guard;
						missing->internal_source_blocks = internal_source_blocks;
						missing->internal_source_block_count =
							internal_source_block_count;
						missing->has_iteration_guard = use_entry_guard;
						missing->required_observations = required_observations;
						missing->event_scoped_observations =
							event_scoped_observations;
						m_event_scoped_probe_count +=
							missing->event_scoped_observations ? 1u : 0u;
						missing->candidate.profitability_prescreened = true;
						m_statistics.profitability_prescreen_passes++;
						RecordRepeatedCandidateOutcome(missing->candidate,
							RepeatedCandidateOutcome::ProfitabilityProven,
							UINT32_MAX, internal_source_block_count);
					}
				}
			}
			catch (const std::bad_alloc&)
			{
				const PendingCandidate failed = missing->candidate;
				m_statistics.compiler_heap_failures++;
				(void)RejectCandidate(failed, BuildFailureStage::HeapAllocation,
					failed.entry_pc);
				RemoveProbe(missing);
				changed = true;
				continue;
			}
			if (EmitProductProbeEntry(missing))
			{
				RecordRepeatedCandidateOutcome(missing->candidate,
					RepeatedCandidateOutcome::ProbeEmitted, UINT32_MAX,
					missing->internal_source_block_count);
				changed = true;
				continue;
			}
			m_statistics.probe_code_failures++;
			RememberNegativeCandidate(missing->candidate.entry_pc,
				missing->candidate.source_end_pc, missing->candidate.kind);
			RemoveProbe(missing);
			changed = true;
		}
		return changed;
	}

	void Runtime::AdvanceProbeEpoch()
	{
		if (m_armed_probe_count == 0)
			return;
		u32 index = 0;
		while (index < m_armed_probe_count)
		{
			ProbeCandidate* const probe = m_armed_probes[index];
			if (!probe || !probe->armed)
				return;
			if (++probe->idle_outer_boundaries < PROBE_IDLE_OUTER_BOUNDARIES)
			{
				index++;
				continue;
			}
			m_statistics.probe_evictions++;
			RemoveProbe(probe);
		}
		ArmQueuedProbes();
	}

	void Runtime::RetireEntryCode(Entry* entry)
	{
		if (!entry)
			return;
		if (entry->code_slot_offset != static_cast<size_t>(-1) && m_executor)
		{
			(void)m_executor->RetireRegionCodeBuffer(&entry->code,
				entry->code_slot_offset);
		}
		else
		{
			entry->code.Release();
		}
		entry->code_slot_offset = static_cast<size_t>(-1);
	}

	void Runtime::RetireProbeCode(ProbeCandidate* probe)
	{
		if (!probe)
			return;
		if (probe->code_slot_offset != static_cast<size_t>(-1) && m_executor)
		{
			(void)m_executor->RetireRegionCodeBuffer(&probe->code,
				probe->code_slot_offset);
		}
		else
		{
			probe->code.Release();
		}
		probe->code_slot_offset = static_cast<size_t>(-1);
	}

	void Runtime::RemoveProbe(ProbeCandidate* probe)
	{
		if (!probe || !probe->valid)
			return;
		probe->maximum_observations = std::max(
			probe->maximum_observations, probe->observations);
		RecordProbePeak(*probe);
		const bool armed = probe->armed;
		if (armed)
		{
			u32 index = 0;
			while (index < m_armed_probe_count &&
				m_armed_probes[index] != probe)
			{
				index++;
			}
			if (index >= m_armed_probe_count)
			{
				(void)RepairArmedProbeDirectory();
				index = 0;
				while (index < m_armed_probe_count &&
					m_armed_probes[index] != probe)
				{
					index++;
				}
			}
			// Repair may have safely queued an excess ghost owner. Either way the
			// record itself must still be retired; only remove a directory pointer
			// when it remains present after rebuilding.
			if (index < m_armed_probe_count)
			{
			m_armed_probe_count--;
			m_armed_probes[index] = m_armed_probes[m_armed_probe_count];
			m_armed_probes[m_armed_probe_count] = nullptr;
			}
		}
		if (armed && !UseBarrierProbesForValidation() &&
			probe->observations > probe->reported_observations)
		{
			m_statistics.probe_observations +=
				probe->observations - probe->reported_observations;
		}
		if (probe->event_scoped_observations && m_event_scoped_probe_count != 0)
			m_event_scoped_probe_count--;
		RetireProbeCode(probe);
		*probe = {};
		m_publication_dirty |= armed;
	}

	bool Runtime::SuspendProbeUntilRetry(ProbeCandidate* probe)
	{
		if (!probe || !probe->valid || !probe->armed ||
			!probe->has_iteration_guard || probe->samples == 0 ||
			probe->observations != 0)
		{
			return false;
		}
		u32 index = 0;
		while (index < m_armed_probe_count && m_armed_probes[index] != probe)
			index++;
		if (index >= m_armed_probe_count)
		{
			(void)RepairArmedProbeDirectory();
			index = 0;
			while (index < m_armed_probe_count && m_armed_probes[index] != probe)
				index++;
			if (index >= m_armed_probe_count)
				return false;
		}

		m_armed_probe_count--;
		m_armed_probes[index] = m_armed_probes[m_armed_probe_count];
		m_armed_probes[m_armed_probe_count] = nullptr;
		probe->armed = false;
		probe->dormant = true;
		const u64 scaled_retry = static_cast<u64>(m_probe_retry_cycles) <<
			probe->retry_backoff_shift;
		const u32 retry_delay = static_cast<u32>(std::min<u64>(scaled_retry,
			PROBE_RETRY_MAXIMUM_EE_CYCLES));
		probe->retry_cycle = cpuRegs.cycle + retry_delay;
		if (retry_delay < PROBE_RETRY_MAXIMUM_EE_CYCLES)
			probe->retry_backoff_shift++;
		probe->last_observation_serial = ++m_probe_serial;
		m_statistics.probe_sample_misses++;
		m_publication_dirty = true;
		return true;
	}

	bool Runtime::ObserveArmedProbeEntry(u32 target_pc)
	{
		for (u32 index = 0; index < m_armed_probe_count; index++)
		{
			ProbeCandidate* const probe = m_armed_probes[index];
			if (!probe || !probe->valid || !probe->armed ||
				probe->candidate.entry_pc != target_pc)
			{
				continue;
			}

			// The generated lookup barrier itself is the observation point. A
			// persistent chain can cross several directly linked tier-zero blocks,
			// so its cold callback does not carry trustworthy immediate-predecessor
			// metadata. The candidate already owns a decoded, source-attested
			// backedge; reaching its armed entry is sufficient only for the bounded
			// profitability counter and cannot change guest semantics.
			probe->observations++;
			probe->idle_outer_boundaries = 0;
			probe->last_observation_serial = ++m_probe_serial;
			m_statistics.probe_observations++;
			if (probe->observations >= m_minimum_backedge_observations)
			{
				m_pending = probe->candidate;
				RemoveProbe(probe);
				m_statistics.hot_promotions++;
			}
			return true;
		}
		return false;
	}

	bool Runtime::PromoteRequestedProductProbe()
	{
		if (UseBarrierProbesForValidation() || m_pending.valid)
			return false;
		ProbeCandidate* promoted = nullptr;
		for (u32 index = 0; index < m_armed_probe_count; index++)
		{
			ProbeCandidate* const probe = m_armed_probes[index];
			const u32 required = probe && probe->required_observations != 0 ?
				probe->required_observations :
				(probe && probe->has_iteration_guard ?
					1u : m_minimum_backedge_observations);
			if (!probe || !probe->valid || !probe->armed ||
				probe->observations < required)
				continue;
			if (!promoted || probe->observations > promoted->observations ||
				(probe->observations == promoted->observations &&
				 probe->last_observation_serial < promoted->last_observation_serial))
			{
				promoted = probe;
			}
		}
		if (!promoted)
			return false;

		m_pending = promoted->candidate;
		m_pending_budget_deferred = false;
		RemoveProbe(promoted);
		m_statistics.hot_promotions++;
		return true;
	}

	bool Runtime::SuspendSampledProductProbeMisses()
	{
		bool changed = false;
		u32 index = 0;
		while (index < m_armed_probe_count)
		{
			ProbeCandidate* const probe = m_armed_probes[index];
			if (SuspendProbeUntilRetry(probe))
			{
				changed = true;
				continue;
			}
			index++;
		}
		return changed;
	}

	bool Runtime::ProcessGeneratedMaintenance()
	{
		// Generated code can only request work; it cannot mutate executable
		// ownership while the persistent dispatcher frame is live. Clear the
		// coalesced request, classify its bounded owners, and make the caller unwind
		// only when the subsequent outer transaction has real work to publish.
		RecordEventScopedProbeMaximum();
		m_generated_maintenance_requested = 0;
		(void)PromoteRequestedProductProbe();
		(void)SuspendSampledProductProbeMisses();
		return m_publication_dirty ||
		       (m_pending.valid && BuildBudgetAvailable());
	}

	void Runtime::SampleProductProbeLiveness()
	{
		if (UseBarrierProbesForValidation())
			return;
		u32 index = 0;
		while (index < m_armed_probe_count)
		{
			ProbeCandidate* const probe = m_armed_probes[index];
			if (!probe || !probe->valid || !probe->armed)
				return;
			const u32 observations = probe->observations;
			if (observations != probe->reported_observations)
			{
				// Event-scoped generated probes reset their bounded counter when the
				// epoch changes. A lower value is new-epoch activity, not a wrapping
				// cumulative counter; only charge the monotonic part of an epoch.
				if (observations > probe->reported_observations)
				{
					m_statistics.probe_observations +=
						observations - probe->reported_observations;
				}
				probe->reported_observations = observations;
				probe->idle_outer_boundaries = 0;
				index++;
				continue;
			}
			if (++probe->idle_outer_boundaries < PROBE_IDLE_OUTER_BOUNDARIES)
			{
				index++;
				continue;
			}
			m_statistics.probe_evictions++;
			RemoveProbe(probe);
		}
	}

	bool Runtime::IsNegativeCandidate(u32 entry_pc, u32 source_end_pc,
		CandidateKind kind) const
	{
		return FindNegativeCandidate(entry_pc, source_end_pc, kind) != nullptr;
	}

	const Runtime::NegativeCandidate* Runtime::FindNegativeCandidate(
		u32 entry_pc, u32 source_end_pc, CandidateKind kind) const
	{
		for (const NegativeCandidate& candidate : m_negative_candidates)
		{
			if (candidate.valid && candidate.entry_pc == entry_pc &&
				candidate.source_end_pc == source_end_pc &&
				candidate.kind == kind)
			{
				return &candidate;
			}
		}
		return nullptr;
	}

	bool Runtime::IsDeferredCandidate(u32 entry_pc, u32 source_end_pc,
		CandidateKind kind) const
	{
		for (const DeferredCandidate& deferred : m_deferred_candidates)
		{
			if (deferred.valid && deferred.candidate.entry_pc == entry_pc &&
				deferred.candidate.source_end_pc == source_end_pc &&
				deferred.candidate.kind == kind)
			{
				return true;
			}
		}
		return false;
	}

	void Runtime::RememberNegativeCandidate(u32 entry_pc, u32 source_end_pc,
		CandidateKind kind, BuildFailureStage failure_stage, u32 failure_pc,
		RepeatedCandidateOutcome outcome, u32 failure_detail)
	{
		if (const NegativeCandidate* existing =
				FindNegativeCandidate(entry_pc, source_end_pc, kind))
		{
			// The array address is stable for the runtime lifetime. Preserve the
			// first specific terminal cause; a later sampled "already known" result
			// must not erase the compiler layer which rejected the graph.
			if (existing->failure_stage == BuildFailureStage::None &&
				failure_stage != BuildFailureStage::None)
			{
				NegativeCandidate* mutable_existing =
					const_cast<NegativeCandidate*>(existing);
				mutable_existing->failure_stage = failure_stage;
				mutable_existing->failure_pc = failure_pc;
				mutable_existing->outcome = outcome;
				mutable_existing->failure_detail = failure_detail;
			}
			return;
		}
		m_negative_candidates[m_negative_insert] =
			{entry_pc, source_end_pc, failure_pc, kind, failure_stage, outcome,
				failure_detail, true};
		m_negative_insert = (m_negative_insert + 1) %
			m_negative_candidates.size();
	}

	bool Runtime::DeferCandidate(const PendingCandidate& candidate,
		u32 missing_contract_pc, DeferredCandidate::Reason reason)
	{
		RecordRepeatedCandidateOutcome(candidate,
			reason == DeferredCandidate::Reason::SourceDependency ?
				RepeatedCandidateOutcome::MissingTopologyDeferred :
				RepeatedCandidateOutcome::AdmissionDeferred,
			missing_contract_pc);
		for (DeferredCandidate& deferred : m_deferred_candidates)
		{
			if (deferred.valid &&
				deferred.candidate.entry_pc == candidate.entry_pc &&
				deferred.candidate.source_end_pc == candidate.source_end_pc &&
				deferred.candidate.kind == candidate.kind)
			{
				if (deferred.reason ==
						DeferredCandidate::Reason::AdmissionCapacity &&
					reason == DeferredCandidate::Reason::AdmissionCapacity)
				{
					return true;
				}
				deferred.candidate = candidate;
				deferred.missing_contract_pc = missing_contract_pc;
				deferred.ready_contract_pc = UINT32_MAX;
				deferred.progress_serial = ++m_deferred_progress_serial;
				deferred.reason = reason;
				return true;
			}
		}
		DeferredCandidate* free_slot = nullptr;
		DeferredCandidate* oldest_unresolved = nullptr;
		for (DeferredCandidate& deferred : m_deferred_candidates)
		{
			if (!deferred.valid)
			{
				if (!free_slot)
					free_slot = &deferred;
				continue;
			}

			// A dependency can become available only when BlockExecutor prepares its
			// immutable tier-zero owner. ObservePreparedTierZeroBlock() reports that
			// transition to MarkDeferredSourceReady() before discovering any new
			// candidate from the owner. If the owner predated this candidate, the
			// GetRegionSourceBlockContract() which requested deferral would already
			// have succeeded. Consequently missing_contract_pc is the authoritative
			// readiness bit: rescanning up to 32 source blocks on every deferral made
			// bounded metadata maintenance needlessly expensive on Cortex-A9.
			if (deferred.reason ==
					DeferredCandidate::Reason::SourceDependency &&
				deferred.missing_contract_pc != UINT32_MAX &&
				(!oldest_unresolved ||
				 deferred.progress_serial < oldest_unresolved->progress_serial))
			{
				oldest_unresolved = &deferred;
			}
		}

		DeferredCandidate* slot = free_slot;
		if (!slot)
		{
			// Ready candidates have a complete dependency and are next in line for
			// promotion. Only unresolved metadata may be displaced. Oldest-first
			// replacement gives a bounded cache eventual admission without deriving
			// policy from a title, PC, opcode sequence, or content hash.
			slot = oldest_unresolved;
			if (slot)
				m_statistics.source_contract_replacements++;
		}
		if (slot)
		{
			*slot = {};
			slot->candidate = candidate;
			slot->missing_contract_pc = missing_contract_pc;
			slot->ready_contract_pc = UINT32_MAX;
			slot->progress_serial = ++m_deferred_progress_serial;
			slot->reason = reason;
			slot->valid = true;
			if (reason == DeferredCandidate::Reason::SourceDependency)
				m_statistics.source_contract_deferrals++;
			else
				m_statistics.admission_capacity_deferrals++;
			return true;
		}
		if (reason == DeferredCandidate::Reason::SourceDependency)
			m_statistics.source_contract_capacity_rejections++;
		else
			m_statistics.admission_capacity_rejections++;
		return false;
	}

	bool Runtime::DeferAdmissionCandidate(const PendingCandidate& candidate)
	{
		PersistentProbeIterationGuard guard{};
		std::array<u32,
			PersistentGeneratedEntry::MAX_INTERNAL_SOURCE_BLOCKS>
			internal_source_blocks{};
		u8 internal_source_block_count = 0;
		bool use_entry_guard = false;
		u32 required_observations = 0;
		bool event_scoped_observations = false;
		ProfitabilityPrescreen final_prescreen = ProfitabilityPrescreen::Unknown;
		try
		{
			if (!BuildCandidate(candidate, &final_prescreen, &guard,
					&internal_source_blocks, &internal_source_block_count,
					&use_entry_guard, &required_observations,
					&event_scoped_observations))
			{
				return false;
			}
		}
		catch (const std::bad_alloc&)
		{
			m_statistics.compiler_heap_failures++;
			(void)RejectCandidate(candidate, BuildFailureStage::HeapAllocation,
				candidate.entry_pc, RegionA32::CompileFailure::None, false);
			return false;
		}

		PendingCandidate prepared = candidate;
		prepared.profitability_prescreened = true;
		if (!DeferCandidate(prepared, UINT32_MAX,
				DeferredCandidate::Reason::AdmissionCapacity))
		{
			return false;
		}
		for (DeferredCandidate& deferred : m_deferred_candidates)
		{
			if (!deferred.valid || deferred.reason !=
					DeferredCandidate::Reason::AdmissionCapacity ||
				deferred.candidate.entry_pc != prepared.entry_pc ||
				deferred.candidate.source_end_pc != prepared.source_end_pc ||
				deferred.candidate.kind != prepared.kind)
			{
				continue;
			}
			deferred.iteration_guard = guard;
			deferred.internal_source_blocks = internal_source_blocks;
			deferred.internal_source_block_count = internal_source_block_count;
			deferred.has_iteration_guard = use_entry_guard;
			deferred.required_observations = required_observations;
			deferred.event_scoped_observations = event_scoped_observations;
			deferred.classification_prepared = true;
			m_statistics.profitability_prescreen_passes++;
			m_statistics.candidates++;
			RecordRepeatedCandidateOutcome(prepared,
				RepeatedCandidateOutcome::ProfitabilityProven, UINT32_MAX,
				internal_source_block_count);
			return true;
		}
		return false;
	}

	void Runtime::MarkDeferredSourceReady(u32 prepared_pc)
	{
		for (DeferredCandidate& deferred : m_deferred_candidates)
		{
			if (deferred.valid &&
				deferred.reason == DeferredCandidate::Reason::SourceDependency &&
				deferred.missing_contract_pc == prepared_pc)
			{
				deferred.missing_contract_pc = UINT32_MAX;
				deferred.ready_contract_pc = prepared_pc;
				deferred.progress_serial = ++m_deferred_progress_serial;
			}
		}
	}

	bool Runtime::PromoteReadyDeferredCandidate(u32 causal_prepared_pc)
	{
		if (m_pending.valid)
			return false;
		DeferredCandidate* selected = nullptr;
		for (DeferredCandidate& deferred : m_deferred_candidates)
		{
			if (!deferred.valid || deferred.missing_contract_pc != UINT32_MAX ||
				(causal_prepared_pc != UINT32_MAX &&
				 deferred.ready_contract_pc != causal_prepared_pc))
				continue;
			if (!selected || deferred.progress_serial < selected->progress_serial)
				selected = &deferred;
		}
		if (!selected)
			return false;
		DeferredCandidate& deferred = *selected;

		if (deferred.reason == DeferredCandidate::Reason::AdmissionCapacity)
		{
			if (!deferred.classification_prepared ||
				!deferred.candidate.profitability_prescreened)
			{
				m_statistics.admission_capacity_rejections++;
				deferred = {};
				return false;
			}
			ProbeCandidate* probe = SelectProbeSlot();
			if (!probe)
				return false;
			if (probe->valid)
			{
				m_statistics.probe_evictions++;
				RemoveProbe(probe);
			}
			*probe = {};
			probe->candidate = deferred.candidate;
			probe->iteration_guard = deferred.iteration_guard;
			probe->internal_source_blocks = deferred.internal_source_blocks;
			probe->internal_source_block_count =
				deferred.internal_source_block_count;
			probe->has_iteration_guard = deferred.has_iteration_guard;
			probe->required_observations = deferred.required_observations;
			probe->event_scoped_observations =
				deferred.event_scoped_observations;
			probe->valid = true;
			probe->last_observation_serial = ++m_probe_serial;
			m_event_scoped_probe_count +=
				probe->event_scoped_observations ? 1u : 0u;
			const PendingCandidate retried = deferred.candidate;
			const u8 block_count = deferred.internal_source_block_count;
			deferred = {};
			m_statistics.admission_capacity_retries++;
			RecordRepeatedCandidateOutcome(retried,
				RepeatedCandidateOutcome::AdmissionRetried, UINT32_MAX,
				block_count);
			ArmQueuedProbes();
			return true;
		}

		{
			// A caller/leaf unit may have been discovered before its return or latch
			// tier-zero owner existed. Re-run the exact topology proof now instead of
			// publishing the deliberately partial graph stored by the deferral.
			if (deferred.reason ==
					DeferredCandidate::Reason::SourceDependency &&
				deferred.candidate.kind == CandidateKind::ForwardReducible &&
				deferred.candidate.require_repeated_path)
			{
				const u32 entry_pc = deferred.candidate.entry_pc;
				const bool require_direct_call_island =
					deferred.candidate.require_direct_call_island;
				deferred = {};
				m_statistics.source_contract_resumes++;
				bool dependency_deferred = false;
				return QueueForwardCandidateAtEntry(entry_pc, true,
					&dependency_deferred, nullptr,
					require_direct_call_island);
			}
			if (m_require_proven_profitability &&
				!deferred.candidate.profitability_prescreened &&
				!UseBarrierProbesForValidation())
			{
				ProbeCandidate* probe = FindProbe(deferred.candidate.entry_pc,
					deferred.candidate.source_end_pc,
					deferred.candidate.kind);
				if (!probe)
				{
					probe = SelectProbeSlot();
					if (!probe)
						return false;
					if (probe->valid)
					{
						m_statistics.probe_evictions++;
						RemoveProbe(probe);
					}
					*probe = {};
					probe->candidate = deferred.candidate;
					probe->valid = true;
					probe->last_observation_serial = ++m_probe_serial;
				}
				const bool source_dependency = deferred.reason ==
					DeferredCandidate::Reason::SourceDependency;
				deferred = {};
				if (source_dependency)
					m_statistics.source_contract_resumes++;
				ArmQueuedProbes();
				return true;
			}
			m_pending = deferred.candidate;
			m_pending_budget_deferred = false;
			m_pending.attempted_block_records = UINT32_MAX;
			const bool source_dependency = deferred.reason ==
				DeferredCandidate::Reason::SourceDependency;
			deferred = {};
			if (source_dependency)
				m_statistics.source_contract_resumes++;
			return true;
		}
		return false;
	}

	bool Runtime::BuildBudgetAvailable() const
	{
		if (m_build_tokens != 0)
			return true;
		const u64 cycle = cpuRegs.cycle;
		return cycle < m_build_budget_cycle ||
		       cycle - m_build_budget_cycle >= m_build_token_refill_cycles;
	}

	u32 Runtime::AvailableBuildTokens() const
	{
		if (m_build_tokens >= m_build_token_capacity)
			return m_build_token_capacity;
		const u64 cycle = cpuRegs.cycle;
		if (cycle < m_build_budget_cycle)
			return m_build_token_capacity;
		const u64 refills =
			(cycle - m_build_budget_cycle) / m_build_token_refill_cycles;
		return static_cast<u32>(std::min<u64>(m_build_token_capacity,
			static_cast<u64>(m_build_tokens) + refills));
	}

	u64 Runtime::BuildBudgetWaitCycles() const
	{
		if (AvailableBuildTokens() != 0 || cpuRegs.cycle < m_build_budget_cycle)
			return 0;
		const u64 elapsed = cpuRegs.cycle - m_build_budget_cycle;
		return m_build_token_refill_cycles - elapsed;
	}

	void Runtime::RefillBuildBudget()
	{
		const u64 cycle = cpuRegs.cycle;
		if (cycle < m_build_budget_cycle)
		{
			const u32 added = m_build_token_capacity - m_build_tokens;
			m_build_tokens = m_build_token_capacity;
			m_build_budget_cycle = cycle;
			m_statistics.compile_budget_refills += added;
			return;
		}
		if (m_build_tokens >= m_build_token_capacity)
		{
			// Do not bank more than one full burst while no candidate exists.
			m_build_budget_cycle = cycle;
			return;
		}
		const u64 elapsed = cycle - m_build_budget_cycle;
		const u64 available = elapsed / m_build_token_refill_cycles;
		if (available == 0)
			return;
		const u32 added = static_cast<u32>(std::min<u64>(available,
			m_build_token_capacity - m_build_tokens));
		m_build_tokens += added;
		m_statistics.compile_budget_refills += added;
		if (m_build_tokens == m_build_token_capacity)
			m_build_budget_cycle = cycle;
		else
			m_build_budget_cycle +=
				static_cast<u64>(added) * m_build_token_refill_cycles;
	}

	bool Runtime::ObserveTierZeroBoundary(
		const BlockExecutionResult& result, u32 target_pc)
	{
		if (!m_executor || (target_pc & 3u) != 0)
			return false;
		auto finish_boundary = [this](bool unwind) {
			// Entry barriers are retained only by the validation policy. Product
			// admission samples the architectural PC at an existing scheduler
			// boundary and therefore has no barrier reservoir to age here.
			if (UseBarrierProbesForValidation())
				AdvanceProbeEpoch();
			return unwind || m_publication_dirty;
		};
		if (HasEntryAtPc(target_pc))
			return finish_boundary(true);
		if (m_pending.valid)
			return finish_boundary(BuildBudgetAvailable() &&
				m_pending.entry_pc == target_pc);
		if (UseBarrierProbesForValidation() &&
			ObserveArmedProbeEntry(target_pc))
			return finish_boundary(true);

		// Region discovery belongs to ObservePreparedTierZeroBlock(), the cold seam
		// which runs exactly once when a source owner is compiled. Re-decoding the
		// completed source block here charged every scheduler boundary for contract
		// lookup, RAM translation and opcode analysis—even after all hot regions were
		// published. Product hotness comes from sparse scheduler-boundary PC samples;
		// no generated edge is unlinked merely to update a counter. Source-generation
		// resets recompile tier-zero owners and therefore revisit the prepared seam.
		(void)result;
		return finish_boundary(false);
	}

	bool Runtime::ObservePreparedTierZeroBlock(
		const BlockExecutionResult& result)
	{
		if (!m_executor || (result.start_pc & 3u) != 0)
			return false;
		MarkDeferredSourceReady(result.start_pc);
		const bool resumed_candidate = PromoteReadyDeferredCandidate(
			result.start_pc);
		// A pending compiler owner is exclusive. A resumed generated probe is only
		// bounded admission metadata/code and may coexist with discovery for the
		// source owner which made it ready. Treating both as exclusive discarded
		// subsequent profitable leaf loops during product startup.
		if (resumed_candidate && m_pending.valid)
			return true;
		auto promote_ready = [this]() {
			const bool resumed = PromoteReadyDeferredCandidate();
			return resumed || (m_pending.valid && BuildBudgetAvailable());
		};
		if (m_pending.valid)
		{
			return BuildBudgetAvailable() &&
			       m_pending.attempted_block_records !=
			       m_executor->GetCodeCacheBlockRecordCount();
		}

		RegionSourceBlockContract source{};
		if (!m_executor->GetRegionSourceBlockContract(result.start_pc, &source) ||
			source.instruction_count < 2 || source.specialized_wait ||
			source.instruction_count >
				(UINT32_MAX - source.start_pc) / sizeof(u32))
		{
			return resumed_candidate || promote_ready();
		}
		// Direct VU0 leaves are too short to amortize their own dispatcher entry.
		// Search only already-published PCSX2 predecessors at this cold source-owner
		// seam and queue a candidate only when those edges prove an enclosing repeated
		// path. This discovers caller/leaf units without an address, title, or opcode-
		// sequence key and adds no work to a generated hot edge.
		const bool caller_candidate_queued =
			QueuePreparedVu0CallerCandidate(source);
		// Deferral records metadata only. It does not own executable publication,
		// and a generated caller probe does not conflict with an independently safe
		// leaf-loop owner. Preserve both: the leaf is today's proven fallback while
		// the broader unit gathers enough hotness to remove its dispatcher seam.

		const u32 source_end =
			source.start_pc + source.instruction_count * sizeof(u32);
		const u32 branch_pc = source_end - 2 * sizeof(u32);
		u32 branch_opcode = 0;
		if (!ReadRamSourceWord(branch_pc, &branch_opcode))
			return resumed_candidate || caller_candidate_queued || promote_ready();
		const s32 displacement = static_cast<s16>(branch_opcode) * 4;
		const u32 target_pc = branch_pc + sizeof(u32) +
			static_cast<u32>(displacement);
		if (!DecodeBackwardBranchTo(branch_pc, branch_opcode, target_pc))
			return resumed_candidate || caller_candidate_queued || promote_ready();
		if (QueueBackwardCandidate(
				source.start_pc, branch_pc, source_end, target_pc, false))
		{
			return true;
		}
		return resumed_candidate || caller_candidate_queued || promote_ready();
	}

	bool Runtime::ReadRamSourceWord(u32 pc, u32* word) const
	{
		if (!word || !eeMem || !vtlb_private::vtlbdata.vmap || (pc & 3u) != 0)
			return false;
		const vtlb_private::VTLBVirtual mapping =
			vtlb_private::vtlbdata.vmap[pc >> vtlb_private::VTLB_PAGE_BITS];
		if (mapping.isHandler(pc))
			return false;
		const uptr host = mapping.assumePtr(pc);
		const uptr ram = reinterpret_cast<uptr>(eeMem->Main);
		const u32 ram_size = std::min(Ps2MemSize::ExposedRam, Ps2MemSize::MainRam);
		if (host < ram || host > ram + ram_size - sizeof(u32))
			return false;
		std::memcpy(word, reinterpret_cast<const void*>(host), sizeof(*word));
		return true;
	}

	bool Runtime::ProgramHasObservedBackedge(const RegionIR::Program& program,
		const PendingCandidate& candidate) const
	{
		if (program.entry_block >= program.blocks.size() ||
			program.blocks[program.entry_block].pc != candidate.entry_pc)
		{
			return false;
		}
		for (const RegionIR::Block& block : program.blocks)
		{
			if (block.pc != candidate.backedge_block_pc ||
				block.terminator.kind != RegionIR::TerminatorKind::Branch ||
				block.terminator.branch_pc != candidate.branch_pc)
			{
				continue;
			}
			return block.terminator.taken.target_block == program.entry_block ||
			       block.terminator.not_taken.target_block == program.entry_block;
		}
		return false;
	}

	bool Runtime::DiscoverSourceBackedNaturalLoop(
		const PendingCandidate& candidate, std::vector<u32>* block_pcs,
		u32* missing_contract_pc) const
	{
		if (!block_pcs || !missing_contract_pc || !m_executor)
			return false;
		block_pcs->clear();
		*missing_contract_pc = UINT32_MAX;

		// Formation follows PCSX2's already-published direct-link topology. Search
		// beyond the final eight-block region so a cold exit arm cannot consume the
		// entire discovery budget before the observed latch is reached, but keep the
		// cold transaction strictly bounded for the Cortex-A9.
		static constexpr size_t MAX_DISCOVERY_BLOCKS =
			PersistentGeneratedEntry::MAX_INTERNAL_SOURCE_BLOCKS * 2;
		struct DiscoveredBlock
		{
			u32 pc = 0;
			std::array<u32, 2> successors{};
			u8 successor_count = 0;
			bool reaches_latch = false;
		};
		std::vector<DiscoveredBlock> graph;
		graph.reserve(MAX_DISCOVERY_BLOCKS);
		std::vector<u32> pending;
		pending.reserve(MAX_DISCOVERY_BLOCKS);
		pending.push_back(candidate.entry_pc);
		if (candidate.backedge_block_pc != candidate.entry_pc)
			pending.push_back(candidate.backedge_block_pc);

		auto find_block = [&](u32 pc) -> DiscoveredBlock* {
			const auto found = std::find_if(graph.begin(), graph.end(),
				[&](const DiscoveredBlock& block) { return block.pc == pc; });
			return found != graph.end() ? &*found : nullptr;
		};
		auto already_pending = [&](u32 pc) {
			return std::find(pending.begin(), pending.end(), pc) != pending.end();
		};

		for (size_t read = 0; read < pending.size(); read++)
		{
			const u32 pc = pending[read];
			if (find_block(pc))
				continue;
			if (graph.size() >= MAX_DISCOVERY_BLOCKS)
				continue;

			RegionSourceBlockTopology topology{};
			if (!m_executor->GetRegionSourceBlockTopology(pc, &topology))
			{
				if (*missing_contract_pc == UINT32_MAX)
					*missing_contract_pc = pc;
				continue;
			}
			if (topology.contract.specialized_wait)
				continue;

			DiscoveredBlock block{};
			block.pc = pc;
			block.successors = topology.successors;
			block.successor_count = topology.successor_count;
			graph.push_back(block);

			// JAL is admitted only through Region IR's separately verified direct-call
			// contract. For natural-loop topology, its semantic successor is the return
			// PC, not the callee's tier-zero entry. This lets discovery continue across
			// any bounded number of calls without treating a callee edge as a loop
			// edge; final lifting still proves each callee and return independently.
			bool direct_call = false;
			u32 direct_return_pc = 0;
			if (topology.contract.instruction_count >= 2)
			{
				u32 control = 0;
				const u32 control_pc = pc +
					(topology.contract.instruction_count - 2) * sizeof(u32);
				direct_call = ReadRamSourceWord(control_pc, &control) &&
					(control >> 26) == 0x03;
				direct_return_pc = control_pc + 2 * sizeof(u32);
			}
			if (direct_call)
			{
				block.successors = {};
				block.successors[0] = direct_return_pc;
				block.successor_count = 1;
				graph.back() = block;
				if (direct_return_pc != candidate.entry_pc &&
					!find_block(direct_return_pc) &&
					!already_pending(direct_return_pc))
				{
					if (pending.size() >= MAX_DISCOVERY_BLOCKS)
						return false;
					pending.push_back(direct_return_pc);
				}
				continue;
			}

			std::array<u32, 2> successors = topology.successors;
			std::sort(successors.begin(),
				successors.begin() + topology.successor_count,
				[&](u32 left, u32 right) {
					auto priority = [&](u32 successor) {
						if (successor == candidate.backedge_block_pc)
							return 0u;
						if (successor >= candidate.entry_pc &&
							successor < candidate.source_end_pc)
						{
							return 1u;
						}
						return 2u;
					};
					const u32 left_priority = priority(left);
					const u32 right_priority = priority(right);
					return left_priority != right_priority ?
						left_priority < right_priority : left < right;
				});
			for (u8 index = 0; index < topology.successor_count; index++)
			{
				const u32 successor = successors[index];
				if (successor == candidate.entry_pc || find_block(successor) ||
					already_pending(successor))
				{
					continue;
				}
				if (pending.size() >= MAX_DISCOVERY_BLOCKS)
					continue;
				pending.push_back(successor);
			}
		}

		DiscoveredBlock* const latch = find_block(candidate.backedge_block_pc);
		if (!latch)
			return false;
		const bool closes_loop = std::any_of(latch->successors.begin(),
			latch->successors.begin() + latch->successor_count,
			[&](u32 target) { return target == candidate.entry_pc; });
		if (!closes_loop)
			return false;
		latch->reaches_latch = true;

		bool changed = true;
		while (changed)
		{
			changed = false;
			for (DiscoveredBlock& block : graph)
			{
				if (block.reaches_latch)
					continue;
				for (u8 index = 0; index < block.successor_count; index++)
				{
					DiscoveredBlock* const successor =
						find_block(block.successors[index]);
					if (!successor || !successor->reaches_latch)
						continue;
					block.reaches_latch = true;
					changed = true;
					break;
				}
			}
		}

		const DiscoveredBlock* const entry = find_block(candidate.entry_pc);
		if (!entry || !entry->reaches_latch)
			return false;
		for (const DiscoveredBlock& block : graph)
		{
			if (!block.reaches_latch)
				continue;
			if (block_pcs->size() >= m_options.max_blocks)
				return false;
			block_pcs->push_back(block.pc);
		}
		return !block_pcs->empty();
	}

	bool Runtime::DiscoverSourceBackedForwardRegion(
		const PendingCandidate& candidate, std::vector<u32>* block_pcs,
		u32* source_end_pc, u32* missing_contract_pc,
		u32* selected_entry_pc) const
	{
		if (!block_pcs || !source_end_pc || !missing_contract_pc || !m_executor ||
			candidate.kind != CandidateKind::ForwardReducible ||
			(candidate.entry_pc & 3u) != 0)
		{
			return false;
		}
		block_pcs->clear();
		*source_end_pc = candidate.entry_pc;
		*missing_contract_pc = UINT32_MAX;
		if (selected_entry_pc)
			*selected_entry_pc = candidate.entry_pc;

		// PCSX2's already-published direct-link records are the sole CFG authority.
		// Discovery may inspect more owners than the executable block budget so cold
		// exit arms do not consume every slot before a repeated core is reached. Only blocks
		// which can reach that core survive below; all omitted edges remain exact
		// RegionBoundary exits. This is cold topology work and never speculatively
		// compiles a missing tier-zero owner on the weak product CPU.
		// The repeated-core reachability proof uses one u64 bit per discovered
		// source owner.  Discovery is cold and may inspect more owners than the
		// executable unit retains, but it must never exceed that exact mask width.
		static constexpr size_t MAX_FORWARD_DISCOVERY_BLOCKS = 64;
		static_assert(PersistentGeneratedEntry::MAX_INTERNAL_SOURCE_BLOCKS < 64);
		struct ForwardBlock
		{
			u32 pc = 0;
			std::array<u32, 2> successors{};
			u8 successor_count = 0;
			u32 instruction_count = 0;
			bool direct_call = false;
		};
		std::vector<ForwardBlock> graph;
		graph.reserve(MAX_FORWARD_DISCOVERY_BLOCKS);
		std::vector<u32> pending;
		pending.reserve(MAX_FORWARD_DISCOVERY_BLOCKS);
		pending.push_back(candidate.entry_pc);
		auto contains_pc = [](const auto& blocks, u32 pc) {
			return std::any_of(blocks.begin(), blocks.end(),
				[&](const auto& block) { return block.pc == pc; });
		};

		for (size_t read = 0; read < pending.size() &&
			graph.size() < MAX_FORWARD_DISCOVERY_BLOCKS; read++)
		{
			const u32 pc = pending[read];
			if (contains_pc(graph, pc))
				continue;
			RegionSourceBlockTopology topology{};
			if (!m_executor->GetRegionSourceBlockTopology(pc, &topology))
			{
				// A reachable source owner may simply not have been compiled yet.
				// Report the first dependency so prepared caller/leaf discovery can
				// retry at that owner's cold publication seam. Ordinary forward
				// formation may still treat this induced edge as RegionBoundary.
				if (*missing_contract_pc == UINT32_MAX)
					*missing_contract_pc = pc;
				continue;
			}
			if (topology.contract.specialized_wait ||
				topology.contract.start_pc != pc ||
				topology.contract.instruction_count == 0 ||
				topology.contract.instruction_count >
					(UINT32_MAX - pc) / sizeof(u32))
			{
				if (pc == candidate.entry_pc)
					return false;
				continue;
			}

			ForwardBlock block{};
			block.pc = pc;
			block.successors = topology.successors;
			block.successor_count = topology.successor_count;
			block.instruction_count = topology.contract.instruction_count;
			const u32 end_pc = pc +
				topology.contract.instruction_count * sizeof(u32);
			*source_end_pc = std::max(*source_end_pc, end_pc);

			// A direct call is represented by Region IR's separately attested
			// call contract. Intraprocedural forward formation follows its return PC;
			// it never mistakes the callee edge for ordinary reducible control flow.
			if (topology.contract.instruction_count >= 2)
			{
				u32 control = 0;
				const u32 control_pc = end_pc - 2 * sizeof(u32);
				if (ReadRamSourceWord(control_pc, &control) &&
					(control >> 26) == 0x03)
				{
					block.direct_call = true;
					block.successors = {};
					block.successors[0] = control_pc + 2 * sizeof(u32);
					block.successor_count = 1;
				}
			}
			graph.push_back(block);

			std::sort(block.successors.begin(),
				block.successors.begin() + block.successor_count);
			for (u8 successor = 0; successor < block.successor_count; successor++)
			{
				const u32 target = block.successors[successor];
				if (contains_pc(graph, target) ||
					std::find(pending.begin(), pending.end(), target) != pending.end() ||
					pending.size() >= MAX_FORWARD_DISCOVERY_BLOCKS)
				{
					continue;
				}
				pending.push_back(target);
			}
		}
		// A single source-backed block is a reducible CFG too. Keeping it here is
		// important for hot architectural leaf routines whose dynamic JR/JALR exit
		// is already represented by RegionIR's exact register-transfer contract.
		// Profitability remains a separate target-measured admission decision; this
		// discovery step must describe support, not silently impose a two-block
		// cost policy.
		if (graph.empty() || graph.front().pc != candidate.entry_pc)
			return false;

		if (candidate.require_repeated_path)
		{
			const size_t discovered_count = graph.size();
			std::array<u64, MAX_FORWARD_DISCOVERY_BLOCKS> reachable{};
			auto discovered_index_of = [&](u32 pc) {
				for (size_t index = 0; index < discovered_count; index++)
				{
					if (graph[index].pc == pc)
						return index;
				}
				return discovered_count;
			};
			for (size_t source = 0; source < discovered_count; source++)
			{
				for (u8 edge = 0; edge < graph[source].successor_count; edge++)
				{
					const size_t target = discovered_index_of(
						graph[source].successors[edge]);
					if (target != discovered_count)
					reachable[source] |= 1ull << target;
				}
			}
			for (size_t intermediate = 0; intermediate < discovered_count;
				intermediate++)
			{
				for (size_t source = 0; source < discovered_count; source++)
				{
					if ((reachable[source] & (1ull << intermediate)) != 0)
						reachable[source] |= reachable[intermediate];
				}
			}
			u64 repeated = 0;
			for (size_t index = 0; index < discovered_count; index++)
			{
				if ((reachable[index] & (1ull << index)) != 0)
					repeated |= 1ull << index;
			}

			if (repeated != 0)
			{
				// Select a compact source-attested cyclic island, not the shortest cycle
				// which happens to pass through the sampled owner.  One outer SCC can
				// contain a much smaller nested loop; forcing the outer entry into the
				// compilation unit imported cold setup/arms and every direct callee before
				// the backend could measure the useful repeated path.  The selected island
				// receives its own generated lookup probe, so structural reachability alone
				// can never publish or execute a cold nested loop.  Omitted paths remain
				// exact tier-zero RegionBoundary exits.
				auto shortest_path = [&](size_t start, size_t target,
					u64* path_mask, u32* path_length) {
					if (!path_mask || !path_length || start >= discovered_count ||
						target >= discovered_count)
					{
						return false;
					}
					std::array<s8, MAX_FORWARD_DISCOVERY_BLOCKS> predecessor{};
					predecessor.fill(-1);
					std::array<u8, MAX_FORWARD_DISCOVERY_BLOCKS> distance{};
					distance.fill(UINT8_MAX);
					std::array<size_t, MAX_FORWARD_DISCOVERY_BLOCKS> queue{};
					size_t read = 0;
					size_t write = 1;
					queue[0] = start;
					distance[start] = 0;
					while (read < write && distance[target] == UINT8_MAX)
					{
						const size_t source = queue[read++];
						for (u8 edge = 0; edge < graph[source].successor_count;
							edge++)
						{
							const size_t next = discovered_index_of(
								graph[source].successors[edge]);
							if (next == discovered_count ||
								distance[next] != UINT8_MAX)
							{
								continue;
							}
							distance[next] = distance[source] + 1;
							predecessor[next] = static_cast<s8>(source);
							queue[write++] = next;
						}
					}
					if (distance[target] == UINT8_MAX)
						return false;
					u64 mask = 0;
					for (size_t node = target;;)
					{
						mask |= 1ull << node;
						if (node == start)
							break;
						if (predecessor[node] < 0)
							return false;
						node = static_cast<size_t>(predecessor[node]);
					}
					*path_mask = mask;
					*path_length = distance[target];
					return true;
				};

				struct Island
				{
					size_t entry = 0;
					u32 blocks = 0;
					u32 source_words = 0;
					u32 edge_length = 0;
					u32 distance_from_sample = 0;
					u64 mask = 0;
					bool valid = false;
				};
				Island best{};
				for (size_t node = 0; node < discovered_count; node++)
				{
					if ((repeated & (1ull << node)) == 0)
						continue;
					u64 entry_path = 0;
					u32 entry_distance = 0;
					if (!shortest_path(0, node, &entry_path, &entry_distance))
						continue;
					for (u8 edge = 0; edge < graph[node].successor_count; edge++)
					{
						const size_t successor = discovered_index_of(
							graph[node].successors[edge]);
						if (successor == discovered_count)
							continue;
						u64 return_path = 0;
						u32 return_length = 0;
						if (!shortest_path(successor, node, &return_path,
								&return_length))
						{
							continue;
						}
						const u64 mask = return_path | (1ull << node);
						const u32 blocks = std::popcount(mask);
						if (blocks == 0 || blocks > m_options.max_blocks)
							continue;
						u32 source_words = 0;
						u32 direct_calls = 0;
						for (size_t member = 0; member < discovered_count; member++)
						{
							if ((mask & (1ull << member)) != 0)
							{
								source_words += graph[member].instruction_count;
								direct_calls += graph[member].direct_call ? 1u : 0u;
							}
						}
						if (candidate.require_direct_call_island && direct_calls == 0)
							continue;
						const u32 edge_length = return_length + 1;
						const bool better = !best.valid ||
							source_words < best.source_words ||
							(source_words == best.source_words && blocks < best.blocks) ||
							(source_words == best.source_words && blocks == best.blocks &&
							 edge_length < best.edge_length) ||
							(source_words == best.source_words && blocks == best.blocks &&
							 edge_length == best.edge_length &&
							 entry_distance < best.distance_from_sample) ||
							(source_words == best.source_words && blocks == best.blocks &&
							 edge_length == best.edge_length &&
							 entry_distance == best.distance_from_sample &&
							 graph[node].pc < graph[best.entry].pc);
						if (better)
						{
							best = {node, blocks, source_words, edge_length,
								entry_distance, mask, true};
						}
					}
				}
				if (!best.valid)
					return false;
				if (selected_entry_pc)
					*selected_entry_pc = graph[best.entry].pc;
				std::vector<ForwardBlock> repeated_core;
				repeated_core.reserve(best.blocks);
				repeated_core.push_back(graph[best.entry]);
				for (size_t index = 0; index < discovered_count; index++)
				{
					if (index != best.entry && (best.mask & (1ull << index)) != 0)
						repeated_core.push_back(graph[index]);
				}
				if (repeated_core.empty())
					return false;
				graph = std::move(repeated_core);
			}
			else if (graph.size() > m_options.max_blocks)
			{
				// Preserve the prior bounded partial graph so the caller can defer on
				// missing topology rather than turning a cold absent arm into a hard
				// negative candidate.
				graph.resize(m_options.max_blocks);
			}
		}
		else if (graph.size() > m_options.max_blocks)
		{
			graph.resize(m_options.max_blocks);
		}

		*source_end_pc = candidate.entry_pc;
		for (const ForwardBlock& block : graph)
		{
			RegionSourceBlockContract source{};
			if (!m_executor->GetRegionSourceBlockContract(block.pc, &source) ||
				source.instruction_count >
					(UINT32_MAX - source.start_pc) / sizeof(u32))
			{
				return false;
			}
			*source_end_pc = std::max(*source_end_pc, source.start_pc +
				source.instruction_count * sizeof(u32));
		}

		// A reducible flow graph is exactly one whose retreating edges target a
		// dominator. Compute dominators over the bounded induced graph, remove those
		// backedges, and require the remainder to be acyclic. This rejects irreducible
		// multi-entry cycles before lifting or executable allocation.
		const size_t count = graph.size();
		auto index_of = [&](u32 pc) {
			for (size_t index = 0; index < count; index++)
			{
				if (graph[index].pc == pc)
					return index;
			}
			return count;
		};
		const u32 all = (1u << count) - 1u;
		std::array<u32, PersistentGeneratedEntry::MAX_INTERNAL_SOURCE_BLOCKS>
			dominators{};
		dominators.fill(all);
		dominators[0] = 1u;
		bool changed = true;
		while (changed)
		{
			changed = false;
			for (size_t node = 1; node < count; node++)
			{
				u32 predecessor_intersection = all;
				bool have_predecessor = false;
				for (size_t source = 0; source < count; source++)
				{
					for (u8 edge = 0;
						edge < graph[source].successor_count; edge++)
					{
						if (index_of(graph[source].successors[edge]) != node)
							continue;
						predecessor_intersection &= dominators[source];
						have_predecessor = true;
					}
				}
				if (!have_predecessor)
					return false;
				const u32 next = predecessor_intersection | (1u << node);
				if (next != dominators[node])
				{
					dominators[node] = next;
					changed = true;
				}
			}
		}

		std::array<u8, PersistentGeneratedEntry::MAX_INTERNAL_SOURCE_BLOCKS>
			indegree{};
		for (size_t source = 0; source < count; source++)
		{
			for (u8 edge = 0; edge < graph[source].successor_count; edge++)
			{
				const size_t target = index_of(graph[source].successors[edge]);
				if (target == count ||
					(dominators[source] & (1u << target)) != 0)
				{
					continue;
				}
				indegree[target]++;
			}
		}
		std::array<size_t, PersistentGeneratedEntry::MAX_INTERNAL_SOURCE_BLOCKS>
			ready{};
		size_t ready_read = 0;
		size_t ready_write = 0;
		for (size_t node = 0; node < count; node++)
		{
			if (indegree[node] == 0)
				ready[ready_write++] = node;
		}
		size_t visited = 0;
		while (ready_read < ready_write)
		{
			const size_t source = ready[ready_read++];
			visited++;
			for (u8 edge = 0; edge < graph[source].successor_count; edge++)
			{
				const size_t target = index_of(graph[source].successors[edge]);
				if (target == count ||
					(dominators[source] & (1u << target)) != 0)
				{
					continue;
				}
				if (--indegree[target] == 0)
					ready[ready_write++] = target;
			}
		}
		if (visited != count)
			return false;

		block_pcs->reserve(count);
		for (const ForwardBlock& block : graph)
			block_pcs->push_back(block.pc);
		return true;
	}

	u32 Runtime::FindObservedBackedgeBlocker(
		const RegionIR::Program& program,
		const PendingCandidate& candidate) const
	{
		auto constant_address = [&](RegionIR::ValueId value, u32* pc) {
			if (!pc || value == RegionIR::INVALID_VALUE)
				return false;
			for (const RegionIR::Block& block : program.blocks)
			{
				for (const RegionIR::Node& node : block.nodes)
				{
					if (node.id != value)
						continue;
					if (node.opcode != RegionIR::Opcode::ConstantAddress)
						return false;
					*pc = static_cast<u32>(node.literal);
					return true;
				}
			}
			return false;
		};

		u32 blocker = candidate.branch_pc;
		for (const RegionIR::Block& block : program.blocks)
		{
			const std::array<const RegionIR::Transfer*, 2> transfers = {{
				&block.terminator.taken, &block.terminator.not_taken}};
			const size_t transfer_count =
				block.terminator.kind == RegionIR::TerminatorKind::Branch ? 2 : 1;
			for (size_t index = 0; index < transfer_count; index++)
			{
				const RegionIR::Transfer& transfer = *transfers[index];
				if (transfer.target_block != RegionIR::INVALID_BLOCK)
					continue;
				u32 pc = 0;
				if (constant_address(transfer.pc, &pc) &&
					pc >= candidate.entry_pc && pc < blocker)
				{
					blocker = pc;
				}
			}
		}
		return blocker;
	}

	Runtime::Entry* Runtime::FindEntry(u32 pc)
	{
		const size_t index = FindEntryIndex(pc);
		return index < m_entries.size() ? &m_entries[index] : nullptr;
	}

	const Runtime::Entry* Runtime::FindEntry(u32 pc) const
	{
		const size_t index = FindEntryIndex(pc);
		return index < m_entries.size() ? &m_entries[index] : nullptr;
	}

	Runtime::Entry* Runtime::SelectEntryForReplacement(bool* evicted)
	{
		if (evicted)
			*evicted = false;
		Entry* oldest = nullptr;
		for (u32 index = 0; index < m_max_active_regions; index++)
		{
			Entry& entry = m_entries[index];
			if (!entry.active)
				return &entry;
			if (!oldest || entry.last_use_serial < oldest->last_use_serial)
				oldest = &entry;
		}
		if (oldest && evicted)
			*evicted = true;
		return oldest;
	}

	void Runtime::ResetForwardSampleForPc(u32 pc)
	{
		const size_t slot_index =
			((pc >> 2) * 2654435761u) & (FORWARD_SAMPLE_CAPACITY - 1);
		ForwardSample& sample = m_forward_samples[slot_index];
		if (sample.valid && sample.pc == pc)
			sample = {};
	}

	bool Runtime::RejectCandidate(const PendingCandidate& candidate,
		BuildFailureStage stage, u32 failure_pc,
		RegionA32::CompileFailure backend, bool count_compile_failure,
		u8 backend_emission_step, u16 backend_ir_opcode,
		RegionIR::ValueId backend_value, u32 failure_detail)
	{
		RecordRepeatedCandidateOutcome(candidate,
			RepeatedCandidateOutcome::BuildRejected, failure_pc, 0, stage,
			failure_detail);
		RememberNegativeCandidate(candidate.entry_pc, candidate.source_end_pc,
			candidate.kind, stage, failure_pc,
			RepeatedCandidateOutcome::BuildRejected, failure_detail);
		if (count_compile_failure)
			m_statistics.compile_failures++;
		if (m_statistics.failure_snapshot_count <
			m_statistics.failure_snapshot.size())
		{
			BuildFailureRecord& record = m_statistics.failure_snapshot[
				m_statistics.failure_snapshot_count++];
			record.entry_pc = candidate.entry_pc;
			record.failure_pc = failure_pc;
			record.stage = stage;
			record.backend = backend;
			record.backend_emission_step = backend_emission_step;
			record.backend_ir_opcode = backend_ir_opcode;
			record.backend_value = backend_value;
			record.failure_detail = failure_detail;
			(void)ReadRamSourceWord(failure_pc, &record.opcode);
		}
		return false;
	}

	Runtime::ProfitabilityPrescreen Runtime::PrescreenCandidateProfitability(
		const PendingCandidate& candidate)
	{
		return BuildProbeProfitabilityGuard(candidate, nullptr);
	}

	Runtime::PublishProfitabilityDecision
	Runtime::EvaluatePublishProfitability(
		const RegionMemoryPlan::A9ProfitabilityCertificate& certificate,
		const RegionA32::CompileResult& compiled, bool persistent,
		bool uses_uncertified_cop1, bool uses_uncertified_vu0,
		bool classify_narrowed_preflighted_cost) const
	{
		if (!persistent || !m_require_proven_profitability)
			return PublishProfitabilityDecision::Disabled;
		if (!certificate || !compiled.profitability_certificate_valid)
			return PublishProfitabilityDecision::MissingCertificate;
		if (compiled.semantic_kernel_kind != 0 &&
			(!compiled.semantic_kernel_target_cost_valid ||
			 compiled.semantic_kernel_bytes_per_iteration == 0 ||
			 compiled.semantic_kernel_minimum_profitable_bytes <
				RegionA32::CompileOptions::
					DEFAULT_MINIMUM_A9_SEMANTIC_KERNEL_BYTES ||
			 static_cast<u64>(compiled.minimum_profitable_iterations) *
				 compiled.semantic_kernel_bytes_per_iteration <
				 compiled.semantic_kernel_minimum_profitable_bytes))
		{
			return PublishProfitabilityDecision::UnprovenSemanticKernelCost;
		}
		if (uses_uncertified_cop1 &&
			!m_allow_uncertified_cop1_for_validation &&
			!HasCertifiedCop1Residency(certificate))
		{
			return PublishProfitabilityDecision::UncertifiedCop1Cost;
		}
		if (uses_uncertified_vu0 &&
			!m_allow_uncertified_vu0_for_validation &&
			!HasCertifiedVu0FmacStream(certificate))
		{
			return PublishProfitabilityDecision::UncertifiedVu0Cost;
		}
		if (!compiled.profitability_memory_coverage_valid)
			return PublishProfitabilityDecision::UncoveredMemoryCost;
		if (classify_narrowed_preflighted_cost &&
			!compiled.profitability_narrowed_cost_valid)
		{
			return PublishProfitabilityDecision::EmittedCostClassMismatch;
		}
		return PublishProfitabilityDecision::Proven;
	}

	Runtime::ProfitabilityPrescreen Runtime::BuildProbeProfitabilityGuard(
		const PendingCandidate& candidate,
		PersistentProbeIterationGuard* guard,
		std::array<u32,
			PersistentGeneratedEntry::MAX_INTERNAL_SOURCE_BLOCKS>*
			internal_source_blocks,
		u8* internal_source_block_count,
		bool* use_entry_guard,
		u32* required_observations,
		bool* event_scoped_observations)
	{
		if (guard)
			*guard = {};
		if (internal_source_blocks)
			*internal_source_blocks = {};
		if (internal_source_block_count)
			*internal_source_block_count = 0;
		if (use_entry_guard)
			*use_entry_guard = false;
		if (required_observations)
			*required_observations = 0;
		if (event_scoped_observations)
			*event_scoped_observations = false;
		if (!m_require_proven_profitability || !m_executor ||
			!m_executor->PersistentDispatchEnabled())
		{
			return ProfitabilityPrescreen::Unknown;
		}
		// Forward/reducible regions need a measured entry-frequency and aggregate
		// boundary-elision model. A counted-loop certificate cannot soundly stand in
		// for that Phase 4 policy.
		if (candidate.kind == CandidateKind::ForwardReducible)
			return ProfitabilityPrescreen::Unknown;
#if defined(VITASX2_QEMU_VALIDATION)
		if (m_callable_execution_for_validation)
			return ProfitabilityPrescreen::Unknown;
#endif
		const u32 word_count =
			(candidate.source_end_pc - candidate.entry_pc) / sizeof(u32);
		if (word_count < 2 || word_count > MAX_PRIMARY_SOURCE_WORDS)
			return ProfitabilityPrescreen::Unknown;

		std::array<u32, MAX_PRIMARY_SOURCE_WORDS> words{};
		for (u32 index = 0; index < word_count; index++)
		{
			if (!ReadRamSourceWord(candidate.entry_pc + index * sizeof(u32),
					&words[index]))
			{
				return ProfitabilityPrescreen::Unknown;
			}
		}

		RegionIR::LiftOptions options = m_options;
		options.max_direct_calls = 0;
		const RegionIR::LiftResult lifted = RegionIR::Lift(candidate.entry_pc,
			words.data(), word_count, candidate.entry_pc, options);
		if (!lifted || !ProgramHasObservedBackedge(lifted.program, candidate))
		{
			// A direct-call seed can grow into a different final CFG after immutable
			// source contracts are assembled.  Inability to prove the discovery-only
			// form is not evidence that the final program is unprofitable.
			return ProfitabilityPrescreen::Unknown;
		}
		return BuildProgramProfitabilityGuard(lifted.program, guard,
			internal_source_blocks, internal_source_block_count,
			use_entry_guard, required_observations,
			event_scoped_observations);
	}

	Runtime::ProfitabilityPrescreen Runtime::BuildProgramProfitabilityGuard(
		const RegionIR::Program& program,
		PersistentProbeIterationGuard* guard,
		std::array<u32,
			PersistentGeneratedEntry::MAX_INTERNAL_SOURCE_BLOCKS>*
			internal_source_blocks,
		u8* internal_source_block_count,
		bool* use_entry_guard,
		u32* required_observations,
		bool* event_scoped_observations)
	{
		if (guard)
			*guard = {};
		if (internal_source_blocks)
			*internal_source_blocks = {};
		if (internal_source_block_count)
			*internal_source_block_count = 0;
		if (use_entry_guard)
			*use_entry_guard = false;
		if (required_observations)
			*required_observations = 0;
		if (event_scoped_observations)
			*event_scoped_observations = false;
		const bool uses_uncertified_cop1 = UsesUncertifiedCop1Tier(program);
		const bool uses_uncertified_vu0 = UsesUncertifiedVu0Tier(program);
		const RegionMemoryPlan::BuildResult memory =
			RegionMemoryPlan::Build(program);
		if (memory.failure == RegionMemoryPlan::BuildFailure::InvalidProgram)
			return ProfitabilityPrescreen::Unknown;
		u32 minimum_work =
			RegionA32::CompileOptions::DEFAULT_MINIMUM_A9_COUNTED_MEMORY_WORK;
#if defined(VITASX2_QEMU_VALIDATION)
		minimum_work = m_minimum_counted_memory_work_for_validation;
#endif
		if (uses_uncertified_vu0 &&
			m_allow_uncertified_vu0_for_validation)
		{
			minimum_work = m_minimum_vu0_counted_memory_work;
		}
		const RegionMemoryPlan::A9ProfitabilityCertificate certificate =
			RegionMemoryPlan::BuildA9ProfitabilityCertificate(program, memory.plan,
				minimum_work, ObservedRegionWorkFloor(), true,
				RegionMemoryPlan::A9ProfitabilityCertificate::
					DEFAULT_MINIMUM_READ_ONLY_ITERATIONS,
				m_minimum_observed_vu0_fmac_iterations,
				m_minimum_observed_vu0_fmac_leaf_invocations);
		if (uses_uncertified_cop1 &&
			!m_allow_uncertified_cop1_for_validation &&
			!HasCertifiedCop1Residency(certificate))
		{
			return ProfitabilityPrescreen::Unknown;
		}
		if (uses_uncertified_vu0 &&
			!m_allow_uncertified_vu0_for_validation &&
			!HasCertifiedVu0FmacStream(certificate))
		{
			// A Phase 3 memory certificate must never hide a richer VU0 class.
			// Only the complete target-measured FMAC-stream certificate can pass.
			return ProfitabilityPrescreen::Unknown;
		}
		if (!certificate)
			return ProfitabilityPrescreen::Rejected;
		u32 immediate_trip_count = 0;
		if (RegionMemoryPlan::CalculateImmediateTripCount(memory.plan.control,
				&immediate_trip_count))
		{
			if (immediate_trip_count < certificate.minimum_profitable_iterations)
				return ProfitabilityPrescreen::Rejected;
			if (required_observations)
				*required_observations = 1;
			// The immutable source prefix proves the header count. There is no
			// canonical-entry GPR predicate for the probe or generated entry to test.
			return ProfitabilityPrescreen::Proven;
		}
		if (certificate.kind == RegionMemoryPlan::A9ProfitabilityCertificate::
				Kind::ObservedBoundaryElision ||
			certificate.kind == RegionMemoryPlan::A9ProfitabilityCertificate::
				Kind::ObservedPreflightedLoop ||
			certificate.kind == RegionMemoryPlan::A9ProfitabilityCertificate::
				Kind::ObservedCop1Residency ||
			certificate.kind == RegionMemoryPlan::A9ProfitabilityCertificate::
				Kind::ObservedCop1Vu0Residency ||
			certificate.kind == RegionMemoryPlan::A9ProfitabilityCertificate::
				Kind::ObservedVu0AcyclicFmacLeaf ||
			(certificate.kind == RegionMemoryPlan::A9ProfitabilityCertificate::
				Kind::ReadOnlyLoop && !memory.plan.control.valid))
		{
			if (required_observations)
			{
				u32 required = certificate.minimum_profitable_iterations;
				if (certificate.kind == RegionMemoryPlan::
						A9ProfitabilityCertificate::Kind::ObservedPreflightedLoop)
				{
					const NarrowedPreflightedPrescreen narrowed =
						BuildNarrowedPreflightedPrescreen(program, memory,
							certificate);
					if (narrowed.proven)
						required = narrowed.required_observations;
				}
				*required_observations = required;
			}
			if (event_scoped_observations)
			{
				// A loop certificate proves that one uninterrupted invocation amortizes
				// entry work, so its latch counter resets at each scheduler epoch. An
				// acyclic leaf instead has a measured per-call saving; its generated entry
				// counter is cumulative and merely delays cold compilation until the call
				// frequency has paid for it.
				*event_scoped_observations = certificate.kind !=
					RegionMemoryPlan::A9ProfitabilityCertificate::Kind::
						ObservedVu0AcyclicFmacLeaf &&
					certificate.minimum_profitable_iterations > 1;
			}
			return ProfitabilityPrescreen::Proven;
		}
		if (!guard)
			return ProfitabilityPrescreen::Proven;
		if (!internal_source_blocks || !internal_source_block_count ||
			program.blocks.empty() ||
			program.blocks.size() > internal_source_blocks->size())
		{
			return ProfitabilityPrescreen::Unknown;
		}
		for (const RegionIR::Block& block : program.blocks)
		{
			(*internal_source_blocks)[(*internal_source_block_count)++] = block.pc;
		}
		if (use_entry_guard)
			*use_entry_guard = true;
		if (required_observations)
			*required_observations = 1;

		const RegionMemoryPlan::LoopControl& control = memory.plan.control;
		if (!control.valid || control.counter_gpr == 0 ||
			control.counter_stride == 0)
		{
			return ProfitabilityPrescreen::Unknown;
		}
		const bool increasing =
			control.termination == RegionMemoryPlan::TerminationKind::UnsignedLess ||
			control.termination == RegionMemoryPlan::TerminationKind::EqualEndpoint;
		const bool to_zero = control.termination ==
			RegionMemoryPlan::TerminationKind::DecrementToZero;
		const bool while_nonnegative = control.termination ==
			RegionMemoryPlan::TerminationKind::DecrementWhileNonNegative;
		const bool while_positive = control.termination ==
			RegionMemoryPlan::TerminationKind::DecrementWhilePositive;
		if ((!increasing && !to_zero && !while_nonnegative && !while_positive) ||
			(increasing && control.counter_stride <= 0) ||
			(!increasing && control.counter_stride >= 0))
		{
			return ProfitabilityPrescreen::Unknown;
		}
		const u32 step = increasing ?
			static_cast<u32>(control.counter_stride) :
			static_cast<u32>(-static_cast<s64>(control.counter_stride));
		if (step == 0 || step > 4096 || (step & (step - 1)) != 0)
			return ProfitabilityPrescreen::Unknown;

		guard->counter_gpr = control.counter_gpr;
		if (!increasing)
		{
			guard->kind =
				PersistentProbeIterationGuard::Kind::DecrementCounter;
			guard->require_counter_nonnegative =
				while_nonnegative || while_positive;
			guard->alignment_mask = to_zero ? step - 1 : 0;
			u64 threshold = 0;
			if (to_zero)
				threshold = static_cast<u64>(
					certificate.minimum_profitable_iterations) * step;
			else if (while_nonnegative)
				threshold = static_cast<u64>(
					certificate.minimum_profitable_iterations - 1) * step;
			else
				threshold = static_cast<u64>(
					certificate.minimum_profitable_iterations - 1) * step + 1;
			if (threshold > UINT32_MAX)
				return ProfitabilityPrescreen::Rejected;
			guard->threshold = static_cast<u32>(threshold);
		}
		else
		{
			if (!control.bound_is_immediate && control.bound_gpr == 0)
				return ProfitabilityPrescreen::Unknown;
			guard->kind =
				PersistentProbeIterationGuard::Kind::IncreasingEndpoint;
			guard->bound_gpr = control.bound_gpr;
			guard->bound_is_immediate = control.bound_is_immediate;
			guard->bound_immediate = control.bound_immediate;
			guard->require_counter_nonnegative = control.signed_counter_compare;
			guard->require_bound_nonnegative = control.signed_counter_compare;
			guard->alignment_mask = step - 1;
			const u64 threshold =
				certificate.minimum_profitable_iterations <=
					control.trip_count_adjustment ?
				0 :
				static_cast<u64>(certificate.minimum_profitable_iterations -
					control.trip_count_adjustment) * step;
			if (threshold > UINT32_MAX)
				return ProfitabilityPrescreen::Rejected;
			guard->threshold = static_cast<u32>(threshold);
		}
		return ProfitabilityPrescreen::Proven;
	}

#if defined(VITASX2_QEMU_VALIDATION)
	u32 Runtime::ClassifyRequiredObservationsForValidation(
		const RegionIR::Program& program)
	{
		PersistentProbeIterationGuard guard{};
		std::array<u32,
			PersistentGeneratedEntry::MAX_INTERNAL_SOURCE_BLOCKS> source_blocks{};
		u8 source_block_count = 0;
		bool use_entry_guard = false;
		u32 required_observations = 0;
		const ProfitabilityPrescreen result = BuildProgramProfitabilityGuard(
			program, &guard, &source_blocks, &source_block_count,
			&use_entry_guard, &required_observations);
		return result == ProfitabilityPrescreen::Proven ?
			required_observations : 0;
	}
#endif

	void Runtime::RememberProfitabilityPrescreenRejection(
		const PendingCandidate& candidate)
	{
		RecordRepeatedCandidateOutcome(candidate,
			RepeatedCandidateOutcome::ProfitabilityRejected);
		RememberNegativeCandidate(candidate.entry_pc, candidate.source_end_pc,
			candidate.kind, BuildFailureStage::ProfitabilityTriage,
			candidate.entry_pc,
			RepeatedCandidateOutcome::ProfitabilityRejected);
		m_statistics.profitability_prescreen_rejections++;
		if (m_statistics.failure_snapshot_count >=
			m_statistics.failure_snapshot.size())
		{
			return;
		}
		BuildFailureRecord& record = m_statistics.failure_snapshot[
			m_statistics.failure_snapshot_count++];
		record.entry_pc = candidate.entry_pc;
		record.failure_pc = candidate.entry_pc;
		record.stage = BuildFailureStage::ProfitabilityTriage;
		record.backend = RegionA32::CompileFailure::UnprovenProfitability;
		(void)ReadRamSourceWord(record.failure_pc, &record.opcode);
	}

	void Runtime::RememberUnclassifiedProfitabilityFallback(
		const PendingCandidate& candidate)
	{
		RecordRepeatedCandidateOutcome(candidate,
			RepeatedCandidateOutcome::ProfitabilityUnknown);
		RememberNegativeCandidate(candidate.entry_pc, candidate.source_end_pc,
			candidate.kind, BuildFailureStage::ProfitabilityTriage,
			candidate.entry_pc,
			RepeatedCandidateOutcome::ProfitabilityUnknown);
		m_statistics.profitability_prescreen_unknowns++;
		if (m_statistics.failure_snapshot_count >=
			m_statistics.failure_snapshot.size())
		{
			return;
		}
		BuildFailureRecord& record = m_statistics.failure_snapshot[
			m_statistics.failure_snapshot_count++];
		record.entry_pc = candidate.entry_pc;
		record.failure_pc = candidate.entry_pc;
		record.stage = BuildFailureStage::ProfitabilityTriage;
		record.backend = RegionA32::CompileFailure::UnprovenProfitability;
		(void)ReadRamSourceWord(record.failure_pc, &record.opcode);
	}

	void Runtime::RecordRepeatedCandidateOutcome(
		const PendingCandidate& candidate, RepeatedCandidateOutcome outcome,
		u32 detail_pc, u8 internal_source_blocks,
		BuildFailureStage failure_stage, u32 failure_detail)
	{
		if (!candidate.require_repeated_path ||
			outcome == RepeatedCandidateOutcome::None)
		{
			return;
		}
		auto& snapshots = m_statistics.repeated_candidate_snapshot;
		Statistics::RepeatedCandidateSnapshot* destination = nullptr;
		for (Statistics::RepeatedCandidateSnapshot& snapshot : snapshots)
		{
			if (snapshot.outcome != RepeatedCandidateOutcome::None &&
				snapshot.entry_pc == candidate.entry_pc &&
				snapshot.source_end_pc == candidate.source_end_pc)
			{
				destination = &snapshot;
				break;
			}
			if (!destination &&
				snapshot.outcome == RepeatedCandidateOutcome::None)
			{
				destination = &snapshot;
			}
		}
		if (!destination)
		{
			destination = &*std::min_element(snapshots.begin(), snapshots.end(),
				[](const auto& left, const auto& right) {
					return left.last_serial < right.last_serial;
				});
		}
		if (destination->entry_pc == candidate.entry_pc &&
			destination->source_end_pc == candidate.source_end_pc)
		{
			// Preserve the first source-backed terminal reason. Sampling the same
			// hot graph again only proves that it is already remembered; replacing
			// the rejection with AlreadyKnown hid the exact compiler layer which
			// prevented product publication on real Vita workloads.
			const bool retained_terminal =
				destination->outcome == RepeatedCandidateOutcome::BuildRejected ||
				destination->outcome ==
					RepeatedCandidateOutcome::ProfitabilityRejected ||
				destination->outcome ==
					RepeatedCandidateOutcome::ProfitabilityUnknown;
			const bool incoming_less_specific =
				outcome == RepeatedCandidateOutcome::AlreadyKnown ||
				outcome == RepeatedCandidateOutcome::ProfitabilityUnknown;
			if (retained_terminal && incoming_less_specific)
			{
				destination->observations +=
					destination->observations != UINT32_MAX;
				destination->last_serial = ++m_repeated_candidate_serial;
				return;
			}
		}
		const u32 observations = destination->entry_pc == candidate.entry_pc &&
			destination->source_end_pc == candidate.source_end_pc ?
			destination->observations + (destination->observations != UINT32_MAX) : 1;
		*destination = {};
		destination->entry_pc = candidate.entry_pc;
		destination->source_end_pc = candidate.source_end_pc;
		destination->detail_pc = detail_pc;
		destination->observations = observations;
		destination->last_serial = ++m_repeated_candidate_serial;
		destination->internal_source_blocks = internal_source_blocks;
		destination->outcome = outcome;
		destination->failure_stage = failure_stage;
		destination->failure_detail = failure_detail;
		m_statistics.repeated_candidate_snapshot_count = static_cast<u32>(
			std::count_if(snapshots.begin(), snapshots.end(), [](const auto& item) {
				return item.outcome != RepeatedCandidateOutcome::None;
			}));
	}

	bool Runtime::BuildPending()
	{
		if (!m_pending.valid || !m_executor ||
			m_pending.source_end_pc <= m_pending.entry_pc)
		{
			return false;
		}
		RefillBuildBudget();
		if (m_build_tokens == 0)
		{
			if (!m_pending_budget_deferred)
			{
				m_statistics.compile_budget_deferrals++;
				m_pending_budget_deferred = true;
			}
			return false;
		}

		const PendingCandidate candidate = m_pending;
		try
		{
			const ProfitabilityPrescreen prescreen =
				candidate.profitability_prescreened ?
					ProfitabilityPrescreen::Proven :
					PrescreenCandidateProfitability(candidate);
			if (prescreen == ProfitabilityPrescreen::Rejected)
			{
				m_pending = {};
				m_pending_budget_deferred = false;
				RememberProfitabilityPrescreenRejection(candidate);
				return false;
			}
			if (prescreen == ProfitabilityPrescreen::Proven &&
				!candidate.profitability_prescreened)
				m_statistics.profitability_prescreen_passes++;

			m_pending = {};
			m_pending_budget_deferred = false;
			m_statistics.build_attempts++;
			struct CompileWallMeasurement
			{
				u64& total;
				Common::Timer::Value start = Common::Timer::GetCurrentValue();

				~CompileWallMeasurement()
				{
					const Common::Timer::Value end = Common::Timer::GetCurrentValue();
					if (end >= start)
					{
						total += static_cast<u64>(
							Common::Timer::ConvertValueToNanoseconds(
								end - start) / 1000.0);
					}
				}
			} compile_wall_measurement{m_statistics.compile_wall_us};
			const VitaPerformanceTelemetry::ScopedCpuStage compile_profile(
				VitaPerformanceTelemetry::CpuStage::EeCompile);
			return BuildCandidate(candidate);
		}
		catch (const std::bad_alloc&)
		{
			// Region discovery and lowering are opportunistic. The canonical
			// tier-zero owner has not executed any guest instruction at this
			// boundary, so allocation pressure must reject the candidate rather
			// than terminate the emulator or publish a partial region.
			m_pending = {};
			m_pending_budget_deferred = false;
			m_statistics.compiler_heap_failures++;
			return RejectCandidate(candidate,
				BuildFailureStage::HeapAllocation, candidate.entry_pc);
		}
	}

	bool Runtime::AppendContinuations(const RegionIR::Program& program,
		const RegionA32::CompileResult& compiled,
		VitaA32::CodeBuffer* code,
		std::array<Continuation, MAX_CONTINUATIONS_PER_REGION>* continuations,
		u8* continuation_count, u32* failure_pc, bool append_callable)
	{
		if (!m_executor || !code || !continuations || !continuation_count ||
			!failure_pc || program.source_blocks.empty())
		{
			return false;
		}
		*continuation_count = 0;
		*failure_pc = program.blocks[program.entry_block].pc;

		auto definition = [&](RegionIR::ValueId value) -> const RegionIR::Node* {
			for (const RegionIR::Block& block : program.blocks)
			{
				const auto found = std::find_if(block.nodes.begin(), block.nodes.end(),
					[&](const RegionIR::Node& node) { return node.id == value; });
				if (found != block.nodes.end())
					return &*found;
			}
			return nullptr;
		};
		auto source_word = [&](u32 pc, u32* word) -> bool {
			return RegionIR::ReadProgramSourceWord(program, pc, word);
		};
		auto append_resume = [&](u32 resume_pc,
			u32 pending_raw_cycles) -> bool {
			*failure_pc = resume_pc;
			for (u8 index = 0; index < *continuation_count; index++)
			{
				const Continuation& existing = (*continuations)[index];
				if (existing.resume_pc == resume_pc &&
					existing.pending_raw_cycles == pending_raw_cycles)
				{
					return true;
				}
			}

			const RegionIR::SourceBlockContract* source_contract = nullptr;
			for (const RegionIR::SourceBlockContract& candidate :
				program.source_blocks)
			{
				const u64 end = static_cast<u64>(candidate.start_pc) +
					static_cast<u64>(candidate.instruction_count) * sizeof(u32);
				if (resume_pc >= candidate.start_pc && resume_pc < end)
				{
					source_contract = &candidate;
					break;
				}
			}
			if (!source_contract || *continuation_count >= continuations->size())
				return false;

			u32 prefix_raw_cycles = 0;
			for (u32 pc = source_contract->start_pc; pc < resume_pc;
				pc += sizeof(u32))
			{
				u32 opcode = 0;
				if (!source_word(pc, &opcode))
					return false;
				prefix_raw_cycles += RegionIR::RawRecompilerCycles(opcode,
					program.options.cycle_factor);
			}
			if (prefix_raw_cycles != pending_raw_cycles)
				return false;

			Continuation& result =
				(*continuations)[(*continuation_count)++];
			result.resume_pc = resume_pc;
			result.pending_raw_cycles = pending_raw_cycles;
			result.source_end_pc = source_contract->start_pc +
				source_contract->instruction_count * sizeof(u32);
			result.scheduler_test_at_end = source_contract->scheduler_test_at_end;
			return result.source_end_pc > result.resume_pc;
		};
		auto append = [&](const RegionIR::Transfer& transfer) -> bool {
			if (!transfer.cycle_commit_deferred)
				return true;
			const RegionIR::Node* pc_node = definition(transfer.pc);
			if (!pc_node || pc_node->opcode != RegionIR::Opcode::ConstantAddress)
				return false;
			return append_resume(static_cast<u32>(pc_node->literal),
				transfer.pending_raw_cycles);
		};

		// Aggregate event/memory proofs are backend-owned and may reject before
		// executing the first IR node.  They therefore have no Transfer record to
		// enumerate.  Give every region one canonical-entry continuation so any
		// such cold refusal can execute the original PCSX2 block exactly once,
		// without a provider retry or workload-specific recovery path.
		if (program.entry_block >= program.blocks.size() ||
			!append_resume(program.blocks[program.entry_block].pc, 0))
		{
			return false;
		}

		if (!append_callable)
		{
			// Persistent generated code can leave only through the mechanically
			// emitted patch set.  The memory plan deliberately removes per-access
			// exits after an aggregate entry proof; enumerating the original IR exit
			// superset here copied unreachable tier-zero suffixes into every region
			// and made code size scale quadratically with memory-op position.
			for (const RegionA32::PersistentExitPatch& patch :
				compiled.persistent_exit_patches)
			{
				if (patch.dynamic_resume_pc && patch.cycle_commit_deferred)
					return false;
				if (!patch.dynamic_resume_pc && patch.cycle_commit_deferred &&
					!append_resume(patch.resume_pc, patch.pending_raw_cycles))
				{
					return false;
				}
			}
		}
		else
		{
			// Callable validation executes the generic context-return ABI and has no
			// persistent patch set to dispatch through. Keep its deliberately
			// conservative enumeration as a differential oracle for every IR exit.
			for (const RegionIR::Block& block : program.blocks)
			{
				for (const RegionIR::Transfer& transfer : block.guarded_exits)
				{
					if (!append(transfer))
						return false;
				}
				for (const RegionIR::MemoryExit& memory : block.memory_exits)
				{
					if (!append(memory.transfer))
						return false;
				}
				if (!append(block.terminator.taken) ||
					(block.terminator.kind == RegionIR::TerminatorKind::Branch &&
					 !append(block.terminator.not_taken)))
				{
					return false;
				}
			}
		}

		PersistentRegionAbi persistent_abi{};
		if (m_executor->PersistentDispatchEnabled() &&
			!m_executor->GetPersistentRegionAbi(&persistent_abi))
		{
			return false;
		}

		for (u8 index = 0; index < *continuation_count; index++)
		{
			Continuation& continuation = (*continuations)[index];
			const u32 instruction_count =
				(continuation.source_end_pc - continuation.resume_pc) /
					sizeof(u32);
			if (m_executor->PersistentDispatchEnabled())
			{
				const RegionIR::SourceBlockContract* exact_source_entry = nullptr;
				for (const RegionIR::SourceBlockContract& source :
					program.source_blocks)
				{
					if (source.start_pc == continuation.resume_pc)
					{
						exact_source_entry = &source;
						break;
					}
				}
				if (!append_callable && continuation.pending_raw_cycles == 0 &&
					exact_source_entry)
				{
					// A zero-debt continuation at a PCSX2 source-owner entry is
					// already represented by an immutable tier-zero generated block.
					// Tail-enter that exact owner instead of copying the entire block
					// into every region.  This remains a direct generated transition:
					// no lookup can redispatch to the region, and the resume flag above
					// still makes a tier-zero SMC write unwind before publication can
					// change. Mid-block/nonzero-debt exits retain their private exact
					// suffix because no independently compiled owner has that cycle ABI.
					continuation.persistent_entry_offset = code->Size();
					if (!code->EmitMovImm32(0, static_cast<u32>(
							reinterpret_cast<uptr>(
								persistent_abi.resumed_fragment_active))) ||
						!code->EmitMovImm8(1, 1) ||
						!code->EmitStrImm12(1, 0, 0))
					{
						*failure_pc = continuation.resume_pc;
						return false;
					}
					const void* const tier_zero =
						m_executor->GetPersistentTierZeroEntryPoint(
							continuation.resume_pc);
					const size_t tail = code->EmitBranchPlaceholder();
					if (!tier_zero || tail == static_cast<size_t>(-1) ||
						!code->PatchBranchToAddress(tail, tier_zero))
					{
						*failure_pc = continuation.resume_pc;
						return false;
					}
				}
				else
				{
					// Vita exposes one process-wide VM write domain. CompileAllocated()
					// intentionally leaves this private region open for appended leaves;
					// close/sync it before switching to the independently owned shared
					// continuation arena. The region remains unpublished and can be reopened
					// immediately for the small far tail below.
					if (!code->Flush() ||
						!m_executor->GetOrCreatePersistentRegionContinuation(
							continuation.resume_pc, instruction_count,
							continuation.pending_raw_cycles,
							continuation.scheduler_test_at_end,
							&continuation.persistent_entry_point,
							&continuation.scaled_cycles))
					{
						*failure_pc = continuation.resume_pc;
						return false;
					}
					// The shared continuation mapping is independent of the EE arena and
					// therefore is not required to lie inside A32's +/-32 MiB B range.
					// Keep one tiny private far-tail per distinct exit; the expensive exact
					// suffix remains shared and begins by publishing resumed-fragment state.
					continuation.persistent_entry_offset = code->Size();
					if (!code->EmitMovImm32(0, static_cast<u32>(
							reinterpret_cast<uptr>(
								continuation.persistent_entry_point))) ||
						!code->EmitBx(0))
					{
						*failure_pc = continuation.resume_pc;
						return false;
					}
				}
			}
			if (!append_callable)
				continue;
			u32 callable_scaled_cycles = 0;
			if (!m_executor->AppendRegionContinuation(code,
					continuation.resume_pc, instruction_count,
					continuation.pending_raw_cycles,
					continuation.scheduler_test_at_end, false,
					&continuation.callable_entry_offset,
					&callable_scaled_cycles) ||
				(m_executor->PersistentDispatchEnabled() &&
				 callable_scaled_cycles != continuation.scaled_cycles))
			{
				*failure_pc = continuation.resume_pc;
				return false;
			}
			if (!m_executor->PersistentDispatchEnabled())
				continuation.scaled_cycles = callable_scaled_cycles;
		}
		return *continuation_count == 0 || code->Flush();
	}

	bool Runtime::AppendPersistentEntry(const RegionIR::Program& program,
		VitaA32::CodeBuffer* code, Entry* owner,
		const StateTransferPlan& state_transfers,
		const std::array<Continuation, MAX_CONTINUATIONS_PER_REGION>& continuations,
		u8 continuation_count, size_t* entry_offset)
	{
		if (!m_executor || !code || !owner || !entry_offset ||
			continuation_count > continuations.size() || !eeMem ||
			!vtlb_private::vtlbdata.vmap)
		{
			return false;
		}

		PersistentRegionAbi abi{};
		if (!m_executor->GetPersistentRegionAbi(&abi))
			return false;

		std::array<TierZeroFallback,
			PersistentGeneratedEntry::MAX_INTERNAL_SOURCE_BLOCKS> tier_zero{};
		u8 tier_zero_count = 0;
		for (const RegionIR::SourceBlockContract& source : program.source_blocks)
		{
			bool duplicate = false;
			for (u8 index = 0; index < tier_zero_count; index++)
				duplicate |= tier_zero[index].pc == source.start_pc;
			if (duplicate)
				continue;
			if (tier_zero_count >= tier_zero.size())
				return false;
			const void* const entry =
				m_executor->GetPersistentTierZeroEntryPoint(source.start_pc);
			if (!entry)
				return false;
			tier_zero[tier_zero_count++] = {source.start_pc, entry};
		}

		using VitaA32::Condition;
		constexpr unsigned TEMP0 = 0;
		constexpr unsigned TEMP1 = 1;
		constexpr unsigned TEMP2 = 2;
		constexpr unsigned TEMP3 = 3;
		constexpr unsigned CPU_REGS = 4;
		constexpr unsigned VTLB_VMAP = 7;
		constexpr unsigned VTLB_HOST_BASE = 8;
		constexpr unsigned CALL_TARGET = 12;
		constexpr unsigned STACK = 13;
		constexpr u32 CONTEXT_OFFSET = 0;
		constexpr u32 STATE_OFFSET =
			(sizeof(RegionA32::ExecutionContext) + 15u) & ~15u;
		constexpr u32 FRAME_SIZE =
			(STATE_OFFSET + sizeof(RegionIR::CanonicalState) + 7u) & ~7u;
		constexpr u16 CPU_PC_OFFSET =
			static_cast<u16>(offsetof(cpuRegisters, pc));
		constexpr u16 CPU_NEXT_EVENT_OFFSET =
			static_cast<u16>(offsetof(cpuRegisters, nextEventCycle));
		const u32 persistent_entry_pc =
			program.blocks[program.entry_block].pc;
		static_assert(FRAME_SIZE < 4096);
		static_assert(STATE_OFFSET + sizeof(RegionIR::CanonicalState) <= 4096);
		static_assert(offsetof(cpuRegisters, nextEventCycle) + sizeof(u64) <= 4096);

		auto emit_tail = [&](const void* target) {
			return target &&
			       code->EmitMovImm32(CALL_TARGET,
				   static_cast<u32>(reinterpret_cast<uptr>(target))) &&
			       code->EmitBx(CALL_TARGET);
		};
		auto emit_stack_adjust = [&](bool allocate) {
			return code->EmitMovImm32(TEMP3, FRAME_SIZE) &&
				(allocate ? code->EmitSubReg(STACK, STACK, TEMP3) :
					code->EmitAddReg(STACK, STACK, TEMP3));
		};
		auto emit_compare_u32 = [&](unsigned value_register, u32 value) {
			return code->EmitMovImm32(TEMP3, value) &&
				code->EmitCmpReg(value_register, TEMP3);
		};
		auto emit_restore_and_tail = [&](const void* target) {
			return emit_stack_adjust(false) &&
			       emit_tail(target);
		};

		*entry_offset = code->Size();
		if (!emit_stack_adjust(true) ||
			!code->EmitMovImm32(TEMP1, STATE_OFFSET) ||
			!code->EmitAddReg(TEMP1, STACK, TEMP1) ||
			!code->EmitStrImm12(TEMP1, STACK,
				static_cast<u16>(offsetof(RegionA32::ExecutionContext, state))) ||
			!code->EmitStrImm12(VTLB_VMAP, STACK,
				static_cast<u16>(offsetof(RegionA32::ExecutionContext, vmap))) ||
			!code->EmitStrImm12(VTLB_HOST_BASE, STACK,
				static_cast<u16>(offsetof(RegionA32::ExecutionContext,
					host_memory_base))))
		{
			return false;
		}

		const u32 ram_size = std::min(Ps2MemSize::ExposedRam,
			Ps2MemSize::MainRam);
		auto store_pointer = [&](size_t offset, const void* pointer) {
			return code->EmitMovImm32(TEMP1,
					static_cast<u32>(reinterpret_cast<uptr>(pointer))) &&
			       code->EmitStrImm12(TEMP1, STACK,
					static_cast<u16>(offset));
		};
		auto store_u32 = [&](size_t offset, u32 value) {
			return code->EmitMovImm32(TEMP1, value) &&
			       code->EmitStrImm12(TEMP1, STACK,
					static_cast<u16>(offset));
		};
		if (!store_pointer(offsetof(RegionA32::ExecutionContext, main_ram),
				eeMem->Main) ||
			!store_pointer(offsetof(RegionA32::ExecutionContext,
				main_ram_last_word), eeMem->Main + ram_size - sizeof(u32)) ||
			!store_pointer(offsetof(RegionA32::ExecutionContext,
				ram_source_page_live_flags),
				m_executor->RamSourcePageLiveFlags()) ||
			!store_pointer(offsetof(RegionA32::ExecutionContext,
				ram_source_chunk_live_bits),
				m_executor->RamSourceChunkLiveBits()) ||
			!store_u32(offsetof(RegionA32::ExecutionContext, main_ram_limit),
				ram_size) ||
			!store_u32(offsetof(RegionA32::ExecutionContext,
				identity_main_ram_limit),
				vtlb_private::HasDefaultMainRamIdentityWindow() ? ram_size : 0))
		{
			return false;
		}

		for (u16 index = 0; index < state_transfers.entry_count; index++)
		{
			const StateWordTransfer& transfer = state_transfers.entry[index];
			if (!code->EmitLdrImm12(TEMP1, CPU_REGS,
					transfer.runtime_offset) ||
				!code->EmitStrImm12(TEMP1, STACK,
					static_cast<u16>(STATE_OFFSET +
						transfer.canonical_offset)))
			{
				return false;
			}
		}
		if (!code->EmitMovImm32(TEMP1, persistent_entry_pc) ||
			!code->EmitStrImm12(TEMP1, STACK,
				static_cast<u16>(STATE_OFFSET +
					offsetof(RegionIR::CanonicalState, pc))) ||
			!code->EmitLdrImm12(TEMP1, CPU_REGS, CPU_NEXT_EVENT_OFFSET) ||
			!code->EmitStrImm12(TEMP1, STACK,
				static_cast<u16>(offsetof(RegionA32::ExecutionContext,
					next_event_cycle_low))) ||
			!code->EmitLdrImm12(TEMP1, CPU_REGS,
				CPU_NEXT_EVENT_OFFSET + sizeof(u32)) ||
			!code->EmitStrImm12(TEMP1, STACK,
				static_cast<u16>(offsetof(RegionA32::ExecutionContext,
					next_event_cycle_high))) ||
			!code->EmitMovImm8(TEMP1, 0) ||
			!code->EmitStrImm12(TEMP1, STACK,
				static_cast<u16>(offsetof(RegionA32::ExecutionContext, result) +
					offsetof(RegionA32::ExecutionResult, completed))))
		{
			return false;
		}

		// One bounded hot counter is intentionally generated. It replaces the old
		// C++ invocation counter without adding a callback or any formatted output.
		if (!code->EmitMovImm32(TEMP1, static_cast<u32>(
				reinterpret_cast<uptr>(&owner->generated_dispatch_executions))) ||
			!code->EmitLdrImm12(TEMP2, TEMP1, 0) ||
			!code->EmitAddImm8(TEMP2, TEMP2, 1) ||
			!code->EmitStrImm12(TEMP2, TEMP1, 0) ||
			!code->EmitMovRegShiftImm(TEMP0, STACK,
				VitaA32::ShiftType::LSL, 0))
		{
			return false;
		}
		const size_t call_region = code->EmitBranchLinkPlaceholder();
		if (call_region == static_cast<size_t>(-1) ||
			!code->PatchBranchLink(call_region, 0) ||
			!code->EmitCmpImm32(TEMP0, 1))
		{
			return false;
		}
		std::vector<std::pair<size_t, Condition>> failure_branches;
		failure_branches.push_back({
			code->EmitBranchPlaceholder(Condition::NE), Condition::NE});
		if (failure_branches.back().first == static_cast<size_t>(-1) ||
			!code->EmitLdrImm12(TEMP1, STACK,
				static_cast<u16>(offsetof(RegionA32::ExecutionContext, result) +
					offsetof(RegionA32::ExecutionResult, completed))) ||
			!code->EmitCmpImm32(TEMP1, 1))
		{
			return false;
		}
		failure_branches.push_back({
			code->EmitBranchPlaceholder(Condition::NE), Condition::NE});
		if (failure_branches.back().first == static_cast<size_t>(-1))
			return false;

		for (u16 index = 0; index < state_transfers.output_count; index++)
		{
			const StateWordTransfer& transfer = state_transfers.output[index];
			if (!code->EmitLdrImm12(TEMP1, STACK,
					static_cast<u16>(STATE_OFFSET +
						transfer.canonical_offset)) ||
				!code->EmitStrImm12(TEMP1, CPU_REGS,
					transfer.runtime_offset))
			{
				return false;
			}
		}
		if (!code->EmitLdrImm12(TEMP1, STACK,
				static_cast<u16>(STATE_OFFSET +
					offsetof(RegionIR::CanonicalState, pc))) ||
			!code->EmitStrImm12(TEMP1, CPU_REGS, CPU_PC_OFFSET) ||
			!code->EmitLdrImm12(TEMP2, STACK,
				static_cast<u16>(offsetof(RegionA32::ExecutionContext, result) +
					offsetof(RegionA32::ExecutionResult,
					cycle_commit_deferred))) ||
			!code->EmitCmpImm32(TEMP2, 0))
		{
			return false;
		}
		const size_t normal_exit = code->EmitBranchPlaceholder(Condition::EQ);
		if (normal_exit == static_cast<size_t>(-1))
			return false;

		for (u8 index = 0; index < continuation_count; index++)
		{
			const Continuation& continuation = continuations[index];
			if (!code->EmitLdrImm12(TEMP0, CPU_REGS, CPU_PC_OFFSET) ||
				!emit_compare_u32(TEMP0, continuation.resume_pc))
			{
				return false;
			}
			const size_t wrong_pc =
				code->EmitBranchPlaceholder(Condition::NE);
			if (wrong_pc == static_cast<size_t>(-1) ||
				!code->EmitLdrImm12(TEMP0, STACK,
					static_cast<u16>(offsetof(RegionA32::ExecutionContext,
						result) + offsetof(RegionA32::ExecutionResult,
						pending_raw_cycles))) ||
				!emit_compare_u32(TEMP0, continuation.pending_raw_cycles))
			{
				return false;
			}
			const size_t wrong_debt =
				code->EmitBranchPlaceholder(Condition::NE);
			if (wrong_debt == static_cast<size_t>(-1) ||
				!code->EmitMovImm32(TEMP0, static_cast<u32>(
					reinterpret_cast<uptr>(abi.resumed_fragment_active))) ||
				!code->EmitMovImm8(TEMP1, 1) ||
				!code->EmitStrImm12(TEMP1, TEMP0, 0) ||
				!emit_stack_adjust(false))
			{
				return false;
			}
			const size_t continuation_branch = code->EmitBranchPlaceholder();
			const size_t next = code->Size();
			if (continuation_branch == static_cast<size_t>(-1) ||
				!code->PatchBranch(continuation_branch,
					continuation.persistent_entry_offset) ||
				!code->PatchBranch(wrong_pc, next, Condition::NE) ||
				!code->PatchBranch(wrong_debt, next, Condition::NE))
			{
				return false;
			}
		}
		const size_t missing_continuation = code->EmitBranchPlaceholder();
		if (missing_continuation == static_cast<size_t>(-1))
			return false;

		const size_t normal_offset = code->Size();
		if (!code->PatchBranch(normal_exit, normal_offset, Condition::EQ) ||
			!code->EmitLdrImm12(TEMP1, STACK,
				static_cast<u16>(offsetof(RegionA32::ExecutionContext, result) +
					offsetof(RegionA32::ExecutionResult, reason))))
		{
			return false;
		}
		if (!code->EmitCmpImm32(TEMP1,
				static_cast<u32>(RegionIR::ExitReason::RegionBoundary)))
			return false;
		const size_t direct_exit = code->EmitBranchPlaceholder(Condition::EQ);
		if (direct_exit == static_cast<size_t>(-1) ||
			!code->EmitCmpImm32(TEMP1,
				static_cast<u32>(RegionIR::ExitReason::EventHorizon)))
			return false;
		const size_t event_exit = code->EmitBranchPlaceholder(Condition::EQ);
		if (event_exit == static_cast<size_t>(-1))
			return false;

		std::vector<size_t> fallback_reasons;
		constexpr std::array<RegionIR::ExitReason, 8> fallback_reason_values = {{
			RegionIR::ExitReason::EventBudgetFallback,
			RegionIR::ExitReason::ProfitabilityFallback,
			RegionIR::ExitReason::EntryStateFallback,
			RegionIR::ExitReason::MemoryAlignment,
			RegionIR::ExitReason::MemoryHandler,
			RegionIR::ExitReason::MemoryTranslation,
			RegionIR::ExitReason::SelfModifyingCode,
			RegionIR::ExitReason::ExceptionObserver,
		}};
		for (const RegionIR::ExitReason reason : fallback_reason_values)
		{
			if (!code->EmitCmpImm32(TEMP1, static_cast<u32>(reason)))
				return false;
			fallback_reasons.push_back(
				code->EmitBranchPlaceholder(Condition::EQ));
			if (fallback_reasons.back() == static_cast<size_t>(-1))
				return false;
		}
		const size_t unknown_reason = code->EmitBranchPlaceholder();
		if (unknown_reason == static_cast<size_t>(-1))
			return false;

		const size_t fallback_offset = code->Size();
		for (const size_t branch : fallback_reasons)
		{
			if (!code->PatchBranch(branch, fallback_offset, Condition::EQ))
				return false;
		}
		for (u8 index = 0; index < tier_zero_count; index++)
		{
			if (!code->EmitLdrImm12(TEMP0, CPU_REGS, CPU_PC_OFFSET) ||
				!emit_compare_u32(TEMP0, tier_zero[index].pc))
			{
				return false;
			}
			const size_t wrong_pc =
				code->EmitBranchPlaceholder(Condition::NE);
			if (wrong_pc == static_cast<size_t>(-1) ||
				!emit_restore_and_tail(tier_zero[index].entry_point) ||
				!code->PatchBranch(wrong_pc, code->Size(), Condition::NE))
			{
				return false;
			}
		}
		const size_t missing_tier_zero = code->EmitBranchPlaceholder();
		if (missing_tier_zero == static_cast<size_t>(-1))
			return false;

		const size_t direct_offset = code->Size();
		if (!code->PatchBranch(direct_exit, direct_offset, Condition::EQ) ||
			!emit_restore_and_tail(abi.scheduler_elided_redispatch))
		{
			return false;
		}
		const size_t event_offset = code->Size();
		if (!code->PatchBranch(event_exit, event_offset, Condition::EQ) ||
			!emit_restore_and_tail(abi.event_exit))
		{
			return false;
		}

		const size_t failure_offset = code->Size();
		failure_branches.push_back({missing_continuation, Condition::AL});
		failure_branches.push_back({unknown_reason, Condition::AL});
		failure_branches.push_back({missing_tier_zero, Condition::AL});
		for (const auto& [branch, condition] : failure_branches)
		{
			if (branch == static_cast<size_t>(-1) ||
				!code->PatchBranch(branch, failure_offset, condition))
			{
				return false;
			}
		}
		if (!code->EmitMovImm32(TEMP1, static_cast<u32>(reinterpret_cast<uptr>(
				&owner->generated_continuation_failures))) ||
			!code->EmitLdrImm12(TEMP2, TEMP1, 0) ||
			!code->EmitAddImm8(TEMP2, TEMP2, 1) ||
			!code->EmitStrImm12(TEMP2, TEMP1, 0) ||
			!emit_restore_and_tail(abi.generated_failure_exit))
		{
			return false;
		}
		if (!code->Flush())
			return false;
		return true;
	}

	bool Runtime::PatchPersistentExits(VitaA32::CodeBuffer* code,
		RegionA32::CompileResult& compiled,
		const std::array<Continuation, MAX_CONTINUATIONS_PER_REGION>& continuations,
		u8 continuation_count)
	{
		if (!code || !m_executor ||
			continuation_count > continuations.size())
		{
			return false;
		}

		PersistentRegionAbi abi{};
		if (!m_executor->GetPersistentRegionAbi(&abi))
			return false;

		for (RegionA32::PersistentColdExitDescriptor& descriptor :
			compiled.persistent_cold_exit_descriptors)
		{
			if (descriptor.first_word > compiled.persistent_cold_exit_words.size() ||
				descriptor.word_count >
					compiled.persistent_cold_exit_words.size() - descriptor.first_word)
			{
				return false;
			}
			descriptor.words = descriptor.word_count == 0 ? nullptr :
				compiled.persistent_cold_exit_words.data() + descriptor.first_word;
		}

		for (const RegionA32::PersistentExitPatch& patch :
			compiled.persistent_exit_patches)
		{
			if (patch.compact_descriptor_index != UINT16_MAX)
			{
				if (patch.compact_descriptor_index >=
						compiled.persistent_cold_exit_descriptors.size() ||
					patch.compact_descriptor_pointer_patch ==
						static_cast<size_t>(-1) ||
					patch.branch_offset >= code->Size())
				{
					return false;
				}
				RegionA32::PersistentColdExitDescriptor& descriptor =
					compiled.persistent_cold_exit_descriptors[
						patch.compact_descriptor_index];
				descriptor.branch_tail = code->Data() + patch.branch_offset;
				if (!code->PatchMovImm32(patch.compact_descriptor_pointer_patch, 0,
						static_cast<u32>(reinterpret_cast<uptr>(&descriptor))))
				{
					return false;
				}
			}
			const void* target = nullptr;
			if (patch.entry_passthrough)
			{
				// This leaf precedes the region frame and has changed only r0/lr,
				// which are private-dispatch scratch. It must bypass the generated
				// owner being published and enter the immutable tier-zero body with
				// the exact canonical or compatible incoming contract.
				target = patch.compatible_signature.IsValid() ?
					m_executor->GetPersistentCompatibleTierZeroEntryPoint(
						patch.resume_pc, patch.compatible_signature) :
					m_executor->GetPersistentTierZeroEntryPoint(patch.resume_pc);
			}
			else if (patch.reason == RegionIR::ExitReason::RegionBoundary)
			{
				if (patch.dynamic_resume_pc)
				{
					target = abi.scheduler_elided_redispatch;
				}
				else if (patch.compatible_signature.IsValid())
				{
					if (patch.selector_branch_offset == static_cast<size_t>(-1) ||
						patch.canonical_leaf_offset == static_cast<size_t>(-1) ||
						patch.canonical_branch_offset == static_cast<size_t>(-1) ||
						patch.compatible_leaf_offset == static_cast<size_t>(-1))
					{
						return false;
					}
					const void* const compatible =
						m_executor->GetPersistentCompatibleDispatchEntryPoint(
							patch.resume_pc, patch.compatible_signature);
					const void* canonical =
						m_executor->GetPersistentDispatchEntryPoint(patch.resume_pc);
					if (!canonical)
						canonical = abi.scheduler_elided_redispatch;
					const bool use_compatible = compatible != nullptr;
					if (!canonical || !code->PatchBranch(
							patch.selector_branch_offset,
							use_compatible ? patch.compatible_leaf_offset :
								patch.canonical_leaf_offset) ||
						!code->PatchBranchToAddress(
							use_compatible ? patch.branch_offset :
								patch.canonical_branch_offset,
							use_compatible ? compatible : canonical))
					{
						return false;
					}
					continue;
				}
				// A verifier-proven static region boundary tail-enters
				// its current generated owner directly when it already exists. The
				// directory redispatch remains only the cold target-discovery fallback;
				// using it unconditionally made PES re-enter the C++ provider on every
				// region invocation even though region entry itself was first-class.
				if (!target)
				{
					target = m_executor->GetPersistentDispatchEntryPoint(patch.resume_pc);
					if (!target)
						target = abi.scheduler_elided_redispatch;
				}
			}
			else if (patch.reason == RegionIR::ExitReason::EventHorizon)
			{
				target = abi.event_exit;
			}
			else if (patch.cycle_commit_deferred)
			{
				for (u8 index = 0; index < continuation_count; index++)
				{
					const Continuation& continuation = continuations[index];
					if (continuation.resume_pc == patch.resume_pc &&
						continuation.pending_raw_cycles ==
							patch.pending_raw_cycles)
					{
						target = code->Data() +
							continuation.persistent_entry_offset;
						break;
					}
				}
			}
			else
			{
				target = m_executor->GetPersistentTierZeroEntryPoint(
					patch.resume_pc);
			}

			if (!target || !code->PatchBranchToAddress(
					patch.branch_offset, target))
			{
				return false;
			}
		}
		return code->Flush();
	}

	const Runtime::Continuation* Runtime::FindContinuation(const Entry& entry,
		u32 resume_pc, u32 pending_raw_cycles) const
	{
		for (u8 index = 0; index < entry.continuation_count; index++)
		{
			const Continuation& continuation = entry.continuations[index];
			if (continuation.resume_pc == resume_pc &&
				continuation.pending_raw_cycles == pending_raw_cycles)
			{
				return &continuation;
			}
		}
		return nullptr;
	}

	bool Runtime::BuildCandidate(const PendingCandidate& candidate,
		ProfitabilityPrescreen* classification_result,
		PersistentProbeIterationGuard* classification_guard,
		std::array<u32,
			PersistentGeneratedEntry::MAX_INTERNAL_SOURCE_BLOCKS>*
			classification_source_blocks,
		u8* classification_source_block_count,
		bool* classification_use_entry_guard,
		u32* classification_required_observations,
		bool* classification_event_scoped_observations)
	{
		const bool classification_only = classification_result != nullptr;
		if (classification_result)
			*classification_result = ProfitabilityPrescreen::Unknown;
		if (classification_event_scoped_observations)
			*classification_event_scoped_observations = false;
		auto reject = [&](BuildFailureStage stage, u32 failure_pc,
			RegionA32::CompileFailure backend =
				RegionA32::CompileFailure::None,
			u32 failure_detail = 0) {
			return RejectCandidate(candidate, stage, failure_pc, backend,
				!classification_only, 0, UINT16_MAX, RegionIR::INVALID_VALUE,
				failure_detail);
		};
		const u32 primary_word_count = candidate.source_end_pc > candidate.entry_pc ?
			(candidate.source_end_pc - candidate.entry_pc) / sizeof(u32) : 0;
		if (primary_word_count < 2)
			return reject(BuildFailureStage::SourceWindow, candidate.entry_pc);

		// Keep only source PCs and bounded direct-call seeds from the discovery
		// lift. Holding two complete programs at once made the first real product
		// compile needlessly peak the Vita process heap.
		struct DirectCallSeed
		{
			u32 call_pc = 0;
			u32 callee_pc = 0;
			u32 return_pc = 0;
			bool valid = false;
		};
		std::array<DirectCallSeed, 8> direct_calls{};
		u32 direct_call_count = 0;
		std::vector<u32> discovered_block_pcs;
		bool use_source_graph =
			candidate.kind == CandidateKind::ForwardReducible ||
			primary_word_count > MAX_PRIMARY_SOURCE_WORDS;
		if (candidate.kind == CandidateKind::NaturalLoop &&
			primary_word_count <= MAX_PRIMARY_SOURCE_WORDS)
		{
			std::array<u32, MAX_PRIMARY_SOURCE_WORDS> words{};
			for (u32 index = 0; index < primary_word_count; index++)
			{
				if (!ReadRamSourceWord(candidate.entry_pc + index * sizeof(u32),
						&words[index]))
				{
					return reject(BuildFailureStage::SourceRead,
						candidate.entry_pc + index * sizeof(u32));
				}
			}
			RegionIR::LiftOptions discovery_options = m_options;
			discovery_options.max_direct_calls = 0;
			const RegionIR::LiftResult discovered = RegionIR::Lift(candidate.entry_pc,
				words.data(), primary_word_count, candidate.entry_pc,
				discovery_options);
			// A one-block natural loop is still a complete region and is especially
			// common in SDK/runtime memory kernels. The observed source-backed
			// backedge below is the semantic admission proof; requiring an unrelated
			// second leader discarded teapot's hottest memset/copy loops before the
			// backend could report their real coverage requirement.
			if (!discovered || discovered.program.blocks.empty())
				return reject(BuildFailureStage::DiscoveryLift,
					discovered.failure_pc ? discovered.failure_pc : candidate.entry_pc);
			const bool has_intraprocedural_backedge =
				ProgramHasObservedBackedge(discovered.program, candidate);
			use_source_graph = !has_intraprocedural_backedge;

			if (!use_source_graph)
			{
				discovered_block_pcs.reserve(discovered.program.blocks.size());
				for (const RegionIR::Block& block : discovered.program.blocks)
					discovered_block_pcs.push_back(block.pc);
			}
		}
		if (use_source_graph)
		{
			m_statistics.source_graph_attempts++;
			u32 missing_contract_pc = UINT32_MAX;
			u32 discovered_source_end = candidate.source_end_pc;
			u32 discovered_entry_pc = candidate.entry_pc;
			const bool discovered = candidate.kind ==
					CandidateKind::ForwardReducible ?
				DiscoverSourceBackedForwardRegion(candidate,
					&discovered_block_pcs, &discovered_source_end,
					&missing_contract_pc, &discovered_entry_pc) :
				DiscoverSourceBackedNaturalLoop(candidate,
					&discovered_block_pcs, &missing_contract_pc);
			if (!discovered)
			{
				if (missing_contract_pc != UINT32_MAX &&
					DeferCandidate(candidate, missing_contract_pc))
				{
					return false;
				}
				return reject(BuildFailureStage::DiscoveryBackedge,
					missing_contract_pc != UINT32_MAX ? missing_contract_pc :
						(candidate.kind == CandidateKind::NaturalLoop ?
							candidate.backedge_block_pc : candidate.entry_pc));
			}
			// QueueForwardCandidateAtEntry() rebases repeated forward candidates
			// before they acquire probe or compiler ownership.  A different answer at
			// the immutable build boundary means source topology changed without the
			// owning generation transition and must fail closed.
			if (candidate.kind == CandidateKind::ForwardReducible &&
				discovered_entry_pc != candidate.entry_pc)
			{
				return reject(BuildFailureStage::DiscoveryBackedge,
					discovered_entry_pc);
			}
			m_statistics.source_graph_formations++;
		}

		if (discovered_block_pcs.empty())
			return reject(BuildFailureStage::DiscoveryLift, candidate.entry_pc);
		// The complete source-contract set below attests PCSX2's timing dependency,
		// including physical code-budget continuations.  Execution ownership is a
		// separate structural set: only the selected repeated island, its exact call
		// returns, and verified direct callees may become internal Region IR edges.
		// Conflating these sets re-imported every cold arm in the dependency after
		// discovery had already partitioned it.
		std::vector<u32> execution_owner_pcs;
		execution_owner_pcs.reserve(discovered_block_pcs.size() + 3);
		auto own_execution_pc = [&](u32 pc) {
			if (std::find(execution_owner_pcs.begin(), execution_owner_pcs.end(), pc) ==
				execution_owner_pcs.end())
			{
				execution_owner_pcs.push_back(pc);
			}
		};

		// Collect calls only from exact immutable tier-zero owners.  The same helper
		// is reused below as a callee topology is expanded, so nested calls are
		// discovered structurally rather than from a title, address, or byte pattern.
		auto collect_direct_call = [&](u32 block_pc) {
			RegionSourceBlockContract contract{};
			if (!m_executor->GetRegionSourceBlockContract(block_pc, &contract) ||
				contract.instruction_count < 2)
			{
				return true;
			}
			const u32 call_pc = block_pc +
				(contract.instruction_count - 2) * sizeof(u32);
			u32 opcode = 0;
			if (!ReadRamSourceWord(call_pc, &opcode) || (opcode >> 26) != 0x03)
				return true;
			for (u32 index = 0; index < direct_call_count; index++)
			{
				if (direct_calls[index].call_pc == call_pc)
					return true;
			}
			if (direct_call_count >= direct_calls.size())
				return false;
			DirectCallSeed& direct_call = direct_calls[direct_call_count++];
			direct_call.call_pc = call_pc;
			direct_call.callee_pc =
				((call_pc + sizeof(u32)) & 0xf0000000u) |
				((opcode & 0x03ffffffu) << 2);
			direct_call.return_pc = call_pc + 2 * sizeof(u32);
			direct_call.valid = true;
			return true;
		};
		for (const u32 block_pc : discovered_block_pcs)
		{
			if (!collect_direct_call(block_pc))
				return reject(BuildFailureStage::DiscoveryLift, block_pc);
		}

		std::vector<RegionIR::SourceSpan> source_spans;
		std::vector<RegionIR::SourceBlockContract> contracts;
		contracts.reserve(discovered_block_pcs.size() + 3);
		std::vector<u32> unavailable_block_pcs;
		unavailable_block_pcs.reserve(discovered_block_pcs.size());
		enum class ContractAppendResult : u8
		{
			Success,
			Deferred,
			Failure,
		};
		BuildFailureStage append_failure_stage = BuildFailureStage::ContractLift;
		u32 append_failure_pc = candidate.entry_pc;
		u32 append_failure_detail = 0;
		auto append_contract_chain = [&](u32 block_pc,
			bool allow_disjoint_source, bool require_dependency_entry) {
			std::array<u32,
				PersistentGeneratedEntry::MAX_INTERNAL_SOURCE_BLOCKS> chain{};
			u32 chain_count = 0;
			for (;;)
			{
				if (chain_count >= chain.size() ||
					std::find(chain.begin(), chain.begin() + chain_count,
						block_pc) != chain.begin() + chain_count)
				{
					append_failure_stage =
						BuildFailureStage::SourceContractCapacity;
					append_failure_pc = block_pc;
					append_failure_detail = 8;
					return ContractAppendResult::Failure;
				}
				chain[chain_count++] = block_pc;
			RegionSourceBlockContract owner{};
			if (!m_executor->GetRegionSourceBlockContract(block_pc, &owner))
			{
				if (!DeferCandidate(candidate, block_pc))
				{
					append_failure_stage =
						BuildFailureStage::SourceContractCapacity;
					append_failure_pc = block_pc;
					return ContractAppendResult::Failure;
				}
				return ContractAppendResult::Deferred;
			}
			if (owner.specialized_wait)
			{
				append_failure_stage = BuildFailureStage::SpecializedWait;
				append_failure_pc = block_pc;
				return ContractAppendResult::Failure;
			}
			if (owner.start_pc != block_pc || owner.instruction_count == 0 ||
				owner.dependency_instruction_count == 0)
			{
				append_failure_pc = owner.start_pc;
				append_failure_detail = owner.start_pc != block_pc ? 1u :
					(owner.instruction_count == 0 ? 2u : 3u);
				return ContractAppendResult::Failure;
			}
			if (require_dependency_entry &&
				(owner.dependency_start_pc != block_pc ||
				 owner.charged_scaled_cycles_before != 0))
			{
				append_failure_pc = owner.dependency_start_pc;
				append_failure_detail = 0x04000000u |
					std::min(owner.charged_scaled_cycles_before, 0x00ffffffu);
				return ContractAppendResult::Failure;
			}
			const u64 dependency_end =
				static_cast<u64>(owner.dependency_start_pc) +
				static_cast<u64>(owner.dependency_instruction_count) * sizeof(u32);
			if (dependency_end > static_cast<u64>(UINT32_MAX) + 1)
			{
				append_failure_pc = owner.dependency_start_pc;
				append_failure_detail = 5;
				return ContractAppendResult::Failure;
			}

			auto span_covers = [&](const RegionIR::SourceSpan& span) {
				const u64 span_end = static_cast<u64>(span.base_pc) +
					static_cast<u64>(span.words.size()) * sizeof(u32);
				return owner.dependency_start_pc >= span.base_pc &&
					dependency_end <= span_end;
			};
			const bool covered = std::any_of(source_spans.begin(),
				source_spans.end(), span_covers);
			if (!covered)
			{
				if (!allow_disjoint_source)
				{
					append_failure_stage =
						BuildFailureStage::SourceWindowDisjoint;
					append_failure_pc = owner.dependency_start_pc;
					return ContractAppendResult::Failure;
				}
				RegionIR::SourceSpan span{};
				span.base_pc = owner.dependency_start_pc;
				span.words.resize(owner.dependency_instruction_count);
				for (u32 index = 0; index < span.words.size(); index++)
				{
					const u32 pc = span.base_pc + index * sizeof(u32);
					if (!ReadRamSourceWord(pc, &span.words[index]))
					{
						append_failure_stage = BuildFailureStage::SourceRead;
						append_failure_pc = pc;
						return ContractAppendResult::Failure;
					}
				}
				u32 required_source_words = 0;
				const RegionIR::SourceSpanMergeFailure merged =
					RegionIR::MergeImmutableSourceSpan(&source_spans,
						std::move(span), m_options.max_source_instructions,
						&required_source_words);
				if (merged != RegionIR::SourceSpanMergeFailure::None)
				{
					append_failure_stage = merged ==
							RegionIR::SourceSpanMergeFailure::SourceLimit ?
						BuildFailureStage::SourceWindowCapacity :
						BuildFailureStage::SourceWindowOverlap;
					append_failure_pc = owner.dependency_start_pc;
					append_failure_detail = required_source_words;
					return ContractAppendResult::Failure;
				}
			}

			u32 fragment_pc = owner.dependency_start_pc;
			bool scheduler_test_at_dependency_end = true;
			while (static_cast<u64>(fragment_pc) < dependency_end)
			{
				RegionSourceBlockContract fragment{};
				if (!m_executor->GetRegionSourceBlockContract(fragment_pc, &fragment))
				{
					if (!DeferCandidate(candidate, fragment_pc))
					{
						append_failure_stage =
							BuildFailureStage::SourceContractCapacity;
						append_failure_pc = fragment_pc;
						return ContractAppendResult::Failure;
					}
					return ContractAppendResult::Deferred;
				}
				if (fragment.specialized_wait)
				{
					append_failure_stage = BuildFailureStage::SpecializedWait;
					append_failure_pc = fragment_pc;
					return ContractAppendResult::Failure;
				}
				if (fragment.start_pc != fragment_pc ||
					fragment.instruction_count == 0 ||
					fragment.dependency_start_pc != owner.dependency_start_pc ||
					fragment.dependency_instruction_count !=
						owner.dependency_instruction_count)
				{
					append_failure_pc = fragment_pc;
					append_failure_detail = 6;
					return ContractAppendResult::Failure;
				}
				const auto duplicate = std::find_if(contracts.begin(), contracts.end(),
					[&](const RegionIR::SourceBlockContract& contract) {
						return contract.start_pc == fragment.start_pc;
					});
				if (duplicate == contracts.end())
				{
					contracts.push_back({fragment.start_pc,
						fragment.instruction_count, fragment.dependency_start_pc,
						fragment.dependency_instruction_count,
						fragment.charged_scaled_cycles_before,
						fragment.scheduler_test_at_end});
				}
				// Every fragment in one immutable PCSX2 timing dependency is an
				// executable part of the selected owner. Code-budget splits do not
				// create guest observers, so stopping between them would expose a
				// charged-cycle suffix which is not a canonical entry. Conversely, a
				// statically discovered arm for which no owner exists never reaches
				// this point and remains an exact external RegionBoundary.
				own_execution_pc(fragment.start_pc);
				scheduler_test_at_dependency_end =
					fragment.scheduler_test_at_end;
				const u64 next_pc = static_cast<u64>(fragment_pc) +
					static_cast<u64>(fragment.instruction_count) * sizeof(u32);
				if (next_pc > dependency_end || next_pc > UINT32_MAX)
				{
					append_failure_pc = fragment_pc;
					append_failure_detail = 7;
					return ContractAppendResult::Failure;
				}
				fragment_pc = static_cast<u32>(next_pc);
			}
			if (scheduler_test_at_dependency_end)
				return ContractAppendResult::Success;

			// An A32 physical fragment which omits the scheduler test is not an
			// observable exit.  Its exact adjacent owner is part of the same PCSX2
			// execution contract even when that owner begins a fresh dependency (for
			// example, a function prologue falling directly into a natural loop).
			// Follow that immutable seam until a scheduler-tested owner terminates it;
			// never weaken the lifter's missing-continuation verifier.
			if (dependency_end > UINT32_MAX)
			{
				append_failure_pc = owner.dependency_start_pc;
				append_failure_detail = 9;
				return ContractAppendResult::Failure;
			}
			block_pc = static_cast<u32>(dependency_end);
			require_dependency_entry = true;
			}
		};

		for (const u32 block_pc : discovered_block_pcs)
		{
			// Static discovery deliberately sees every arm inside the immutable
			// candidate window.  A cold side arm need not already have a PCSX2
			// tier-zero owner, and requiring one made that unrelated arm monopolize
			// a deferred-candidate slot indefinitely.  Build the final program from
			// the induced subgraph of source-backed blocks instead.  A transfer into
			// an omitted span is an ordinary exact RegionBoundary exit, so the region
			// neither claims nor executes an unattested guest instruction.
			RegionSourceBlockContract available{};
			if (!m_executor->GetRegionSourceBlockContract(block_pc, &available))
			{
				unavailable_block_pcs.push_back(block_pc);
				continue;
			}
			const ContractAppendResult appended =
				append_contract_chain(block_pc, true, false);
			if (appended == ContractAppendResult::Deferred)
				return false;
			if (appended == ContractAppendResult::Failure)
			{
				return reject(append_failure_stage, append_failure_pc,
					RegionA32::CompileFailure::None, append_failure_detail);
			}
		}
		// A call target is a bounded reducible subgraph, not necessarily a one-block
		// leaf.  Follow only PCSX2's immutable generated-link topology, including
		// scheduler-elided physical continuations, and let Region IR independently
		// prove that every path reaches one exact JR r31 return.  The loop is allowed
		// to discover nested calls; unsupported link-register ownership then fails
		// closed in the lifter rather than silently truncating guest execution.
		for (u32 call_index = 0; call_index < direct_call_count; call_index++)
		{
			const DirectCallSeed direct_call = direct_calls[call_index];
			ContractAppendResult appended = append_contract_chain(
				direct_call.return_pc, true, false);
			if (appended == ContractAppendResult::Deferred)
				return false;
			if (appended == ContractAppendResult::Failure)
			{
				return reject(append_failure_stage, append_failure_pc,
					RegionA32::CompileFailure::None, append_failure_detail);
			}

			std::array<u32,
				PersistentGeneratedEntry::MAX_INTERNAL_SOURCE_BLOCKS> pending{};
			std::array<u32,
				PersistentGeneratedEntry::MAX_INTERNAL_SOURCE_BLOCKS> visited{};
			u32 pending_count = 1;
			u32 visited_count = 0;
			pending[0] = direct_call.callee_pc;
			for (u32 read = 0; read < pending_count; read++)
			{
				const u32 pc = pending[read];
				if (std::find(visited.begin(), visited.begin() + visited_count, pc) !=
					visited.begin() + visited_count)
				{
					continue;
				}
				if (visited_count >= visited.size())
				{
					return reject(BuildFailureStage::SourceContractCapacity, pc,
						RegionA32::CompileFailure::None, visited_count + 1);
				}
				visited[visited_count++] = pc;
				appended = append_contract_chain(pc, true,
					pc == direct_call.callee_pc);
				if (appended == ContractAppendResult::Deferred)
					return false;
				if (appended == ContractAppendResult::Failure)
				{
					return reject(append_failure_stage, append_failure_pc,
						RegionA32::CompileFailure::None, append_failure_detail);
				}

				RegionSourceBlockTopology topology{};
				if (!m_executor->GetRegionSourceBlockTopology(pc, &topology))
				{
					if (DeferCandidate(candidate, pc))
						return false;
					return reject(BuildFailureStage::SourceContractCapacity, pc);
				}
				for (u8 successor = 0; successor < topology.successor_count;
					successor++)
				{
					const u32 target = topology.successors[successor];
					if (std::find(visited.begin(), visited.begin() + visited_count,
							target) != visited.begin() + visited_count ||
						std::find(pending.begin(), pending.begin() + pending_count,
							target) != pending.begin() + pending_count)
					{
						continue;
					}
					if (pending_count >= pending.size())
					{
						return reject(BuildFailureStage::SourceContractCapacity,
							target, RegionA32::CompileFailure::None,
							pending_count + 1);
					}
					pending[pending_count++] = target;
				}
				if (!collect_direct_call(pc))
					return reject(BuildFailureStage::DiscoveryLift, pc);
			}
		}
		std::sort(source_spans.begin(), source_spans.end(),
			[](const RegionIR::SourceSpan& left,
				const RegionIR::SourceSpan& right) {
				return left.base_pc < right.base_pc;
			});
		std::sort(contracts.begin(), contracts.end(),
			[](const RegionIR::SourceBlockContract& left,
				const RegionIR::SourceBlockContract& right) {
				return left.start_pc < right.start_pc;
			});
		for (size_t index = 1; index < contracts.size(); index++)
		{
			RegionIR::SourceBlockContract& previous = contracts[index - 1];
			const RegionIR::SourceBlockContract& current = contracts[index];
			const u64 previous_end = static_cast<u64>(previous.start_pc) +
				static_cast<u64>(previous.instruction_count) * sizeof(u32);
			if (static_cast<u64>(current.start_pc) >= previous_end)
				continue;

			const u32 distance = current.start_pc > previous.start_pc ?
				(current.start_pc - previous.start_pc) / sizeof(u32) : 0;
			const bool cycle_timeline_proven = distance != 0 &&
				VitaEE::BlockCompiler::DoesSplitPreserveScaledCycleTimeline(
					previous.start_pc, previous.instruction_count, distance);
			RegionIR::SourceBlockContract prefix{};
			const RegionIR::SuffixPartitionFailure partitioned =
				RegionIR::BuildCycleProvenSuffixPartition(previous, current,
					cycle_timeline_proven, &prefix);
			if (partitioned == RegionIR::SuffixPartitionFailure::None)
			{
				// The synthetic seam is scheduler-elided. The independently entered
				// suffix retains the original natural-tail observation, so both entry
				// paths preserve the tier-zero ordering and exact cycle publication.
				previous = prefix;
				continue;
			}

			// Keep every other overlap rejected. Encode both its geometry and the
			// stable structural/proof reason in the cold record; admission remains
			// based solely on source ownership and PCSX2 cycle semantics.
			const u32 overlap_detail = 0xa0000000u |
				(static_cast<u32>(partitioned) << 24) |
				(std::min(distance, 0xffu) << 16) |
				(std::min(previous.instruction_count, 0xffu) << 8) |
				std::min(current.instruction_count, 0xffu);
			return reject(BuildFailureStage::ContractLift, current.start_pc,
				RegionA32::CompileFailure::None, overlap_detail);
		}

		RegionIR::LiftOptions final_options = m_options;
		final_options.max_direct_calls = direct_call_count;
		// Reducible forward units use an exact internal exceptional veneer. It
		// rejoins the same verified clamp/FCR31 graph and therefore is not a region
		// observer or state-materialization boundary. Natural loops retain the eager
		// shared edge leaf because the A9 dense-loop gate shows the per-operation
		// classifier regresses that cost class.
		final_options.cop1_lazy_ou_guards =
			candidate.kind == CandidateKind::ForwardReducible;
		if (source_spans.empty())
		{
			const u32 missing = unavailable_block_pcs.empty() ?
				candidate.entry_pc : unavailable_block_pcs.front();
			if (DeferCandidate(candidate, missing))
				return false;
			return reject(BuildFailureStage::SourceContractCapacity, missing);
		}

		const RegionIR::LiftResult lifted = RegionIR::LiftWithSourceSpans(
			source_spans.data(), source_spans.size(), contracts.data(),
			contracts.size(), candidate.entry_pc, final_options,
			execution_owner_pcs.data(),
			static_cast<u32>(execution_owner_pcs.size()));
		if (!lifted)
		{
			// The omitted arm was actually required to form a source-attested path
			// from the entry to the observed latch.  Preserve the existing exact
			// deferral contract and retry only when that owner is published.
			if (!unavailable_block_pcs.empty() &&
				DeferCandidate(candidate, unavailable_block_pcs.front()))
			{
				return false;
			}
			// Packed cold-only diagnostic: [31]=lift, [23:16]=LiftFailure,
			// [15:8]=internal stage, [7:0]=source-contract or verifier detail.
			// No hot admission decision depends on this value.
			const u32 low_detail = lifted.internal_stage ==
					RegionIR::LiftInternalStage::Verification ?
				static_cast<u32>(lifted.verify_failure) :
				static_cast<u32>(lifted.failure_detail);
			const u32 lift_detail = 0x80000000u |
				(static_cast<u32>(lifted.failure) << 16) |
				(static_cast<u32>(lifted.internal_stage) << 8) |
				low_detail;
			return reject(BuildFailureStage::ContractLift,
				lifted.failure_pc ? lifted.failure_pc : candidate.entry_pc,
				RegionA32::CompileFailure::None, lift_detail);
		}
		if (lifted.program.direct_calls.size() != direct_call_count)
		{
			u32 failure_pc = candidate.entry_pc;
			for (u32 call_index = 0; call_index < direct_call_count; call_index++)
			{
				const u32 call_pc = direct_calls[call_index].call_pc;
				const bool present = std::any_of(lifted.program.direct_calls.begin(),
					lifted.program.direct_calls.end(), [&](const auto& call) {
						return call.call_pc == call_pc;
					});
				if (!present)
				{
					failure_pc = call_pc;
					break;
				}
			}
			return reject(BuildFailureStage::ContractBackedge,
				failure_pc);
		}
		if (candidate.kind == CandidateKind::NaturalLoop &&
			!ProgramHasObservedBackedge(lifted.program, candidate))
		{
			if (!unavailable_block_pcs.empty() &&
				DeferCandidate(candidate, unavailable_block_pcs.front()))
			{
				return false;
			}
			return reject(BuildFailureStage::ContractBackedge,
				FindObservedBackedgeBlocker(lifted.program, candidate));
		}
		const bool uses_uncertified_cop1 =
			UsesUncertifiedCop1Tier(lifted.program);
		const bool uses_uncertified_vu0 =
			UsesUncertifiedVu0Tier(lifted.program);
		u32 certificate_minimum_work = RegionA32::CompileOptions::
			DEFAULT_MINIMUM_A9_COUNTED_MEMORY_WORK;
#if defined(VITASX2_QEMU_VALIDATION)
		certificate_minimum_work = m_minimum_counted_memory_work_for_validation;
#endif
		// ExactSupport owns Region IR verification and the exact memory/SMC plan.
		// The certificate derived beside it is only cold cost evidence; it is never
		// allowed to redefine whether the verified program can reach A32 emission.
		const RegionMemoryPlan::BuildResult exact_memory =
			RegionMemoryPlan::Build(lifted.program);
		RegionMemoryPlan::A9ProfitabilityCertificate semantic_certificate{};
		if (exact_memory.failure != RegionMemoryPlan::BuildFailure::InvalidProgram)
		{
			semantic_certificate =
				RegionMemoryPlan::BuildA9ProfitabilityCertificate(lifted.program,
					exact_memory.plan, certificate_minimum_work,
					ObservedRegionWorkFloor(), true,
					RegionMemoryPlan::A9ProfitabilityCertificate::
						DEFAULT_MINIMUM_READ_ONLY_ITERATIONS,
					m_minimum_observed_vu0_fmac_iterations,
					m_minimum_observed_vu0_fmac_leaf_invocations);
		}
		constexpr u64 MAX_TARGET_COST_ANALYSES = 4;
		const bool unproven_target_cost_candidate =
			m_emit_unproven_target_cost_for_validation &&
			candidate.require_repeated_path && lifted.program.blocks.size() > 1 &&
			!lifted.program.direct_calls.empty() &&
			(uses_uncertified_cop1 || uses_uncertified_vu0) &&
			m_target_cost_analyses_generation < MAX_TARGET_COST_ANALYSES;
		const bool inspect_unproven_target_cost =
			unproven_target_cost_candidate && !classification_only;
		if (classification_only)
		{
			if (unproven_target_cost_candidate)
			{
				if (!classification_source_blocks ||
					!classification_source_block_count ||
					lifted.program.blocks.size() >
						classification_source_blocks->size())
				{
					return reject(BuildFailureStage::ProfitabilityTriage,
						candidate.entry_pc,
						RegionA32::CompileFailure::UnprovenProfitability);
				}
				*classification_source_blocks = {};
				*classification_source_block_count = 0;
				for (const RegionIR::Block& block : lifted.program.blocks)
				{
					(*classification_source_blocks)[
						(*classification_source_block_count)++] = block.pc;
				}
				if (classification_guard)
					*classification_guard = {};
				if (classification_use_entry_guard)
					*classification_use_entry_guard = false;
				if (classification_required_observations)
					*classification_required_observations = 1;
				if (classification_event_scoped_observations)
					*classification_event_scoped_observations = false;
				*classification_result = ProfitabilityPrescreen::Proven;
				return true;
			}
			if (!classification_guard || !classification_source_blocks ||
				!classification_source_block_count)
			{
				return reject(BuildFailureStage::ProfitabilityTriage,
					candidate.entry_pc,
					RegionA32::CompileFailure::UnprovenProfitability);
			}
			*classification_result = BuildProgramProfitabilityGuard(
				lifted.program, classification_guard,
				classification_source_blocks,
				classification_source_block_count,
				classification_use_entry_guard,
				classification_required_observations,
				classification_event_scoped_observations);
			if (*classification_result == ProfitabilityPrescreen::Proven)
				return true;

			// A complete repeatedly observed graph which lacks a pre-emission cost
			// class is still eligible for one bounded exact compile. ColdCompileBudget
			// already owns source stability, sparse hotness, probe capacity, tokens and
			// remembered source-generation results. Do not turn a cost-model miss into
			// an instruction-support rejection before real allocation/emission exists.
			if (lifted.program.blocks.size() > 1 && classification_source_blocks &&
				classification_source_block_count &&
				lifted.program.blocks.size() <= classification_source_blocks->size())
			{
				*classification_source_blocks = {};
				*classification_source_block_count = 0;
				for (const RegionIR::Block& block : lifted.program.blocks)
				{
					(*classification_source_blocks)[
						(*classification_source_block_count)++] = block.pc;
				}
				if (classification_guard)
					*classification_guard = {};
				if (classification_use_entry_guard)
					*classification_use_entry_guard = false;
				if (classification_required_observations)
					*classification_required_observations = 1;
				if (classification_event_scoped_observations)
					*classification_event_scoped_observations = false;
				*classification_result = ProfitabilityPrescreen::Proven;
				return true;
			}
			return reject(BuildFailureStage::ProfitabilityTriage,
				candidate.entry_pc,
				RegionA32::CompileFailure::UnprovenProfitability);
		}
		bool persistent = m_executor->PersistentDispatchEnabled();
#if defined(VITASX2_QEMU_VALIDATION)
		persistent = persistent && !m_callable_execution_for_validation;
#endif
		bool require_narrowed_preflighted_cost_class = false;
		if (persistent && m_require_proven_profitability && semantic_certificate &&
			semantic_certificate.kind == RegionMemoryPlan::
				A9ProfitabilityCertificate::Kind::ObservedPreflightedLoop)
		{
			const NarrowedPreflightedPrescreen narrowed =
				BuildNarrowedPreflightedPrescreen(lifted.program, exact_memory,
					semantic_certificate);
			require_narrowed_preflighted_cost_class = narrowed.proven;
		}
		// Mid-block RAM accesses are admitted only if the A32 backend can move
		// every fallible check to the untouched block entry. Its affine preflight
		// then either authorizes direct RAM accesses or resumes the complete real
		// tier-zero block with zero cycle debt. Unsupported/data-dependent cases
		// fail closed from RegionA32::Compile as UnsupportedMemory.

		const u32 canonical_entry =
			BlockExecutor::CanonicalizeRamBackedPc(candidate.entry_pc);
		Entry* const existing_entry = FindEntry(candidate.entry_pc);
		if (existing_entry &&
			candidate.source_end_pc <= existing_entry->source_end_pc)
		{
			return RejectCandidate(candidate,
				BuildFailureStage::CanonicalDuplicate, candidate.entry_pc);
		}
		if (!existing_entry && HasCanonicalEntry(canonical_entry))
			return RejectCandidate(candidate, BuildFailureStage::CanonicalDuplicate,
				candidate.entry_pc);
		// Executable ownership is unique by canonical entry, not by immutable
		// source dependency. Different callers may compile the same direct-call
		// leaf into independently allocated regions while the leaf retains its own
		// externally callable entry. The tier-zero source owners remain authoritative
		// for SMC and advance one global generation; Synchronize() retires every
		// dependent region before publication can resume after that generation
		// changes. Treating a source superset as an executable replacement discarded
		// the hot shared leaf and sent every other caller back through tier zero.
		bool evicted = existing_entry != nullptr;
		Entry* const entry = existing_entry ? existing_entry :
			SelectEntryForReplacement(&evicted);
		if (!entry)
		{
			(void)RejectCandidate(candidate, BuildFailureStage::EntryCapacity,
				candidate.entry_pc);
			return false;
		}
		const bool replacing_different_entry = evicted && entry->active &&
			entry->entry_pc != candidate.entry_pc;
		const u32 replaced_entry_pc = replacing_different_entry ?
			entry->entry_pc : UINT32_MAX;

		// The continuously executed region remains independently capped at 2 KiB
		// by RegionA32. Deferred PCSX2 suffixes are cold callable functions in the
		// same source-generation allocation; reserve room for them once and trim the
		// committed slice to the actual total below.
		constexpr size_t REGION_CODE_CAPACITY =
			BlockExecutor::REGION_CODE_BUFFER_CAPACITY;
		VitaA32::CodeBuffer code;
		size_t code_slice_offset = 0;
		// The token accounts for target allocation/emission, not source-contract
		// collection or semantic classification. Failed/unsupported cold analyses
		// must not starve a later structurally profitable loop for 16 Mi guest cycles.
		m_build_tokens--;
		if (!m_executor->PrepareRegionCodeBuffer(
				&code, REGION_CODE_CAPACITY, &code_slice_offset))
		{
			m_statistics.code_cache_failures++;
			return RejectCandidate(candidate, BuildFailureStage::CodeReservation,
				candidate.entry_pc);
		}
		std::array<RegionA32::CompileOptions::PersistentStateWord,
			StateTransferPlan::MAX_WORDS + 1> persistent_state_words{};
		size_t persistent_state_word_count = 0;
		RegionA32::CompileOptions compile_options{};
		compile_options.emit_profitability_guard =
			persistent && m_require_proven_profitability;
		compile_options.classify_narrowed_preflighted_cost =
			require_narrowed_preflighted_cost_class;
		compile_options.validation_only_measure_oversize_hot_image =
			inspect_unproven_target_cost;
		compile_options.minimum_a9_observed_region_work =
			ObservedRegionWorkFloor();
		compile_options.minimum_a9_observed_vu0_fmac_iterations =
			m_minimum_observed_vu0_fmac_iterations;
		compile_options.minimum_a9_observed_vu0_fmac_leaf_invocations =
			m_minimum_observed_vu0_fmac_leaf_invocations;
		compile_options.enable_semantic_kernel_lowering =
			m_semantic_kernel_lowering_enabled;
#if defined(VITASX2_QEMU_VALIDATION)
		compile_options.allow_unretained_semantic_kernel_lowering =
			m_allow_unretained_semantic_kernel_lowering_for_validation;
		compile_options.minimum_a9_counted_memory_work =
			m_minimum_counted_memory_work_for_validation;
		if (m_maximum_hot_code_bytes_for_validation != 0)
		{
			const u32 capacity = std::min<u32>(
				m_maximum_hot_code_bytes_for_validation,
				static_cast<u32>(REGION_CODE_CAPACITY));
			compile_options.max_hot_code_bytes = capacity;
			compile_options.max_entry_hot_code_bytes = capacity;
			compile_options.max_block_hot_code_bytes = capacity;
		}
#endif
		if (uses_uncertified_vu0 &&
			m_allow_uncertified_vu0_for_validation)
		{
			compile_options.minimum_a9_counted_memory_work =
				m_minimum_vu0_counted_memory_work;
		}
		RegionA32::CompileOptions::PersistentDispatch persistent_dispatch{};
		GprLinkSignature compatible_entry_signature{};
		std::array<RegionA32::CompileOptions::PersistentDispatch::CompatibleTarget,
			MAX_CONTINUATIONS_PER_REGION> compatible_targets{};
		size_t compatible_target_count = 0;
		if (persistent)
		{
			compile_options.max_code_bytes = REGION_CODE_CAPACITY;
			const u32 ram_size = std::min(Ps2MemSize::ExposedRam,
				Ps2MemSize::MainRam);
			if (!BuildPersistentStateWordMap(&persistent_state_words,
					&persistent_state_word_count) || !eeMem ||
				!vtlb_private::vtlbdata.vmap || ram_size < sizeof(u32))
			{
				m_executor->DiscardRegionCodeBuffer(&code, code_slice_offset);
				return RejectCandidate(candidate,
					BuildFailureStage::PersistentEntry, candidate.entry_pc,
					RegionA32::CompileFailure::UnsupportedStateContract);
			}
			persistent_dispatch.state_words = persistent_state_words.data();
			persistent_dispatch.state_word_count =
				persistent_state_word_count;
			persistent_dispatch.vu0_state = &VU0;
			persistent_dispatch.vmap = reinterpret_cast<const u32*>(
				vtlb_private::vtlbdata.vmap);
			persistent_dispatch.host_memory_base = reinterpret_cast<const void*>(
				vtlb_private::vtlbdata.host_memory_base);
			persistent_dispatch.main_ram = eeMem->Main;
			persistent_dispatch.main_ram_last_word =
				eeMem->Main + ram_size - sizeof(u32);
			persistent_dispatch.ram_source_page_live_flags =
				m_executor->RamSourcePageLiveFlags();
			persistent_dispatch.ram_source_chunk_live_bits =
				m_executor->RamSourceChunkLiveBits();
			persistent_dispatch.main_ram_limit = ram_size;
			persistent_dispatch.identity_main_ram_limit =
				vtlb_private::HasDefaultMainRamIdentityWindow() ? ram_size : 0;
			// The execution counter is diagnostic-only. The product cache does not
			// use it for admission, validity, or replacement, so a normal build must
			// not pay an absolute-address load/update/store on every region entry.
			persistent_dispatch.execution_counter =
					VitaPerformanceTelemetry::IsEnabled() ?
						&entry->generated_dispatch_executions : nullptr;
#if defined(VITASX2_CPU_PROFILER)
			persistent_dispatch.counted_iteration_counter =
				VitaPerformanceTelemetry::IsEnabled() ?
					&entry->generated_counted_iterations : nullptr;
#elif defined(VITASX2_QEMU_VALIDATION)
			// The bounded-search adversaries use this already-derived trip count to
			// prove that aggregate event/range guards admitted (or rejected) before
			// the first semantic operation. Do not perturb unrelated native cost gates:
			// validation regions which did not request semantic lowering retain the
			// product-profiler-off instruction stream.
			persistent_dispatch.counted_iteration_counter =
				m_semantic_kernel_lowering_enabled ?
					&entry->generated_counted_iterations : nullptr;
#endif
			persistent_dispatch.profitability_fallback_counter =
					VitaPerformanceTelemetry::IsEnabled() ?
						&entry->generated_profitability_fallbacks : nullptr;
			persistent_dispatch.entry_state_fallback_counter =
				VitaPerformanceTelemetry::IsEnabled() ?
					&entry->generated_entry_state_fallbacks : nullptr;
			if (m_executor->GetPersistentTierZeroLinkSignature(
					candidate.entry_pc, &compatible_entry_signature) &&
				IsSupportedRegionEntrySignature(compatible_entry_signature))
			{
				persistent_dispatch.compatible_entry_signature =
					&compatible_entry_signature;
			}
			auto constant_address = [&](RegionIR::ValueId value, u32* pc) {
				if (!pc || value == RegionIR::INVALID_VALUE)
					return false;
				for (const RegionIR::Block& block : lifted.program.blocks)
				{
					for (const RegionIR::Node& node : block.nodes)
					{
						if (node.id != value)
							continue;
						if (node.opcode != RegionIR::Opcode::ConstantAddress)
							return false;
						*pc = static_cast<u32>(node.literal);
						return true;
					}
				}
				return false;
			};
			auto append_compatible_target = [&](const RegionIR::Transfer& transfer) {
				if (transfer.target_block != RegionIR::INVALID_BLOCK ||
					transfer.external_reason != RegionIR::ExitReason::RegionBoundary)
				{
					return;
				}
				u32 pc = 0;
				if (!constant_address(transfer.pc, &pc))
					return;
				for (size_t index = 0; index < compatible_target_count; index++)
				{
					if (compatible_targets[index].pc == pc)
						return;
				}
				if (compatible_target_count >= compatible_targets.size())
					return;
				GprLinkSignature signature{};
				if (!m_executor->GetPersistentTierZeroLinkSignature(pc, &signature) ||
					!IsSupportedRegionEntrySignature(signature))
				{
					return;
				}
				compatible_targets[compatible_target_count++] = {pc, signature};
			};
			for (const RegionIR::Block& block : lifted.program.blocks)
			{
				append_compatible_target(block.terminator.taken);
				if (block.terminator.kind == RegionIR::TerminatorKind::Branch)
					append_compatible_target(block.terminator.not_taken);
			}
			persistent_dispatch.compatible_targets = compatible_targets.data();
			persistent_dispatch.compatible_target_count = compatible_target_count;
				compile_options.persistent_dispatch = &persistent_dispatch;
		}

		Statistics::TargetCostSnapshot target_cost_attempt{};
		auto retain_target_cost_attempt = [&]() {
			if (!inspect_unproven_target_cost)
				return;
			if (m_statistics.target_cost_snapshot_count <
					m_statistics.target_cost_snapshots.size())
			{
				m_statistics.target_cost_snapshots[
					m_statistics.target_cost_snapshot_count++] = target_cost_attempt;
			}
			const Statistics::TargetCostSnapshot& current =
				m_statistics.target_cost_snapshot;
			const u64 score = static_cast<u64>(target_cost_attempt.blocks) * 256u +
				target_cost_attempt.source_words;
			const u64 current_score = static_cast<u64>(current.blocks) * 256u +
				current.source_words;
			if (!current.valid || score > current_score)
				m_statistics.target_cost_snapshot = target_cost_attempt;
		};
		if (inspect_unproven_target_cost)
		{
			target_cost_attempt.entry_pc = candidate.entry_pc;
			target_cost_attempt.source_end_pc = candidate.source_end_pc;
			target_cost_attempt.semantic_diagnostics =
				semantic_certificate.diagnostic_flags;
			target_cost_attempt.blocks =
				static_cast<u32>(lifted.program.blocks.size());
			target_cost_attempt.source_words =
				RegionIR::ProgramSourceInstructionCount(lifted.program);
			target_cost_attempt.direct_calls =
				static_cast<u32>(lifted.program.direct_calls.size());
			target_cost_attempt.valid = true;
		}
			auto populate_target_cost = [&](const RegionA32::CompileResult& result,
			bool complete) {
			if (!inspect_unproven_target_cost)
				return;
			target_cost_attempt.host_instructions = result.host_instructions;
			target_cost_attempt.hot_code_bytes = result.hot_code_bytes;
			target_cost_attempt.entry_state_words = result.entry_state_word_count;
			target_cost_attempt.output_state_words = result.output_state_word_count;
			target_cost_attempt.exit_sites = result.exit_sites;
			target_cost_attempt.exit_state_words = result.exit_state_words;
			target_cost_attempt.exit_sites_by_kind = result.exit_sites_by_kind;
			target_cost_attempt.exit_state_words_by_kind =
				result.exit_state_words_by_kind;
			target_cost_attempt.control_exit_sites_by_target =
				result.control_exit_sites_by_target;
			target_cost_attempt.control_exit_state_words_by_target =
				result.control_exit_state_words_by_target;
			target_cost_attempt.compact_exit_descriptors = static_cast<u32>(
				result.persistent_cold_exit_descriptors.size());
			target_cost_attempt.compact_exit_words = static_cast<u32>(
				result.persistent_cold_exit_words.size());
			target_cost_attempt.compact_snapshot_bytes =
				result.persistent_cold_snapshot_bytes;
			target_cost_attempt.core_peak_words = result.allocation_core_peak_words;
			target_cost_attempt.vfp_peak_s = result.allocation_vfp_peak_s;
			target_cost_attempt.neon_peak_q = result.allocation_neon_peak_q;
			target_cost_attempt.spilled_values = result.allocation_spilled_values;
			target_cost_attempt.spill_bytes = result.allocation_spill_bytes;
			target_cost_attempt.spilled_core_values =
				result.allocation_spilled_core_values;
			target_cost_attempt.spilled_vfp_values =
				result.allocation_spilled_vfp_values;
			target_cost_attempt.spilled_neon_values =
				result.allocation_spilled_neon_values;
			target_cost_attempt.edge_moves = result.allocation_edge_moves;
			target_cost_attempt.edge_call_moves = result.allocation_edge_call_moves;
			target_cost_attempt.edge_return_moves = result.allocation_edge_return_moves;
			target_cost_attempt.edge_backedge_moves =
				result.allocation_edge_backedge_moves;
			target_cost_attempt.edge_state_words = result.allocation_edge_gpr_words +
				result.allocation_edge_fpr_words +
				result.allocation_edge_vu0_vector_words +
				result.allocation_edge_other_state_words;
			target_cost_attempt.memory_loads = result.memory_loads;
			target_cost_attempt.memory_stores = result.memory_stores;
			target_cost_attempt.forwarded_memory_loads =
				result.forwarded_memory_loads;
			target_cost_attempt.memory_forward_candidates =
				result.memory_forward_candidates;
			target_cost_attempt.memory_forward_reaching_stores =
				result.memory_forward_reaching_stores;
			target_cost_attempt.memory_forward_address_matches =
				result.memory_forward_address_matches;
			target_cost_attempt.memory_forward_state_matches =
				result.memory_forward_state_matches;
			target_cost_attempt.memory_preflight_ranges =
				result.memory_preflight_ranges;
			target_cost_attempt.memory_preflight_accesses =
				result.memory_preflight_accesses;
			target_cost_attempt.aggregate_cycle_plan_status =
				static_cast<u8>(result.aggregate_cycle_plan_status);
			target_cost_attempt.block_snapshot_count = 0;
			std::vector<size_t> ranked_blocks(result.target_cost_blocks.size());
			for (size_t index = 0; index < ranked_blocks.size(); index++)
				ranked_blocks[index] = index;
			std::stable_sort(ranked_blocks.begin(), ranked_blocks.end(),
				[&](size_t left, size_t right) {
					const auto& lhs = result.target_cost_blocks[left];
					const auto& rhs = result.target_cost_blocks[right];
					if (lhs.hot_bytes != rhs.hot_bytes)
						return lhs.hot_bytes > rhs.hot_bytes;
					const u32 lhs_spill_io = lhs.spill_loads + lhs.spill_stores;
					const u32 rhs_spill_io = rhs.spill_loads + rhs.spill_stores;
					if (lhs_spill_io != rhs_spill_io)
						return lhs_spill_io > rhs_spill_io;
					if (lhs.exit_state_words != rhs.exit_state_words)
						return lhs.exit_state_words > rhs.exit_state_words;
					return lhs.pc < rhs.pc;
				});
			for (const size_t index : ranked_blocks)
			{
				if (target_cost_attempt.block_snapshot_count >=
						target_cost_attempt.block_snapshot.size())
				{
					break;
				}
				const auto& source = result.target_cost_blocks[index];
				auto& destination = target_cost_attempt.block_snapshot[
					target_cost_attempt.block_snapshot_count++];
				destination.pc = source.pc;
				destination.source_instructions = source.source_instructions;
				destination.hot_bytes = source.hot_bytes;
				destination.host_loads = source.host_loads;
				destination.host_stores = source.host_stores;
				destination.spilled_core_values = source.spilled_core_values;
				destination.spilled_vfp_values = source.spilled_vfp_values;
				destination.spilled_neon_values = source.spilled_neon_values;
				destination.spill_loads = source.spill_loads;
				destination.spill_stores = source.spill_stores;
				destination.edge_moves = source.edge_moves;
				destination.edge_state_words = source.edge_state_words;
				destination.exit_sites = source.exit_sites;
				destination.exit_state_words = source.exit_state_words;
				destination.direct_call_roles = source.direct_call_roles;
			}
			target_cost_attempt.backend_emitted = complete;
		};

		RegionA32::CompileResult compiled{};
		if (inspect_unproven_target_cost)
		{
			m_statistics.target_cost_analyses++;
			m_target_cost_analyses_generation++;
		}
		try
		{
			compiled = RegionA32::CompileAllocated(lifted.program, code,
				compile_options);
			if (compiled && compiled.work_scratch_bytes == 0)
			{
				// The first deterministic emission is the proof pass: it tracks every
				// use of the 16-byte lowering scratch area. Re-emit the exact same
				// verified program without that area whenever it remained unused. This is
				// independent of real allocation spills; a region which needs eight bytes
				// of spill storage must not reserve a 32-byte frame merely because a cold
				// lowering might have needed scratch.
				code.Reset();
				compile_options.omit_proven_unused_work_scratch_frame = true;
				compiled = RegionA32::CompileAllocated(lifted.program, code,
					compile_options);
			}
		}
		catch (...)
		{
			m_executor->DiscardRegionCodeBuffer(&code, code_slice_offset);
			throw;
		}
		if (!compiled)
		{
			if (inspect_unproven_target_cost)
			{
				populate_target_cost(compiled, false);
				target_cost_attempt.failure_pc = compiled.failure_pc;
				target_cost_attempt.failure_detail = compiled.code_bytes;
				target_cost_attempt.failure_value = compiled.failure_value;
				target_cost_attempt.failure_ir_opcode = compiled.failure_ir_opcode;
				target_cost_attempt.failure_stage = BuildFailureStage::A32Compile;
				target_cost_attempt.backend_failure = compiled.failure;
				target_cost_attempt.failure_emission_step =
					compiled.failure_emission_step;
				retain_target_cost_attempt();
			}
			m_executor->DiscardRegionCodeBuffer(&code, code_slice_offset);
			return RejectCandidate(candidate, BuildFailureStage::A32Compile,
				compiled.failure_pc, compiled.failure, true,
				compiled.failure_emission_step, compiled.failure_ir_opcode,
				compiled.failure_value, compiled.code_bytes);
		}
		m_statistics.exact_support_emissions++;
		if (inspect_unproven_target_cost)
		{
			populate_target_cost(compiled, true);
			retain_target_cost_attempt();

			// This path deliberately cannot reach continuation construction, code
			// commit, generated-directory publication, or execution. The existing
			// tier-zero owner remains authoritative for this generation.
			m_executor->DiscardRegionCodeBuffer(&code, code_slice_offset);
			return RejectCandidate(candidate,
				BuildFailureStage::TargetCostUnproven, candidate.entry_pc,
				RegionA32::CompileFailure::UnprovenProfitability, false, 0,
				UINT16_MAX, RegionIR::INVALID_VALUE,
				semantic_certificate.diagnostic_flags);
		}
		const PublishProfitabilityDecision publish_decision =
			EvaluatePublishProfitability(semantic_certificate, compiled, persistent,
				uses_uncertified_cop1, uses_uncertified_vu0,
				require_narrowed_preflighted_cost_class);
		if (publish_decision != PublishProfitabilityDecision::Disabled &&
			publish_decision != PublishProfitabilityDecision::Proven)
		{
			const u32 detail =
				(static_cast<u32>(publish_decision) << 24) |
				(semantic_certificate.diagnostic_flags & 0x00ffffffu);
			m_statistics.publish_profitability_rejections++;
			m_executor->DiscardRegionCodeBuffer(&code, code_slice_offset);
			return RejectCandidate(candidate,
				BuildFailureStage::PublishProfitability, candidate.entry_pc,
				RegionA32::CompileFailure::UnprovenProfitability, false, 0,
				UINT16_MAX, RegionIR::INVALID_VALUE, detail);
		}
		StateTransferPlan state_transfers{};
		if (!BuildRuntimeStateTransferPlan(compiled, &state_transfers))
		{
			m_executor->DiscardRegionCodeBuffer(&code, code_slice_offset);
			return RejectCandidate(candidate, BuildFailureStage::A32Compile,
				candidate.entry_pc,
				RegionA32::CompileFailure::UnsupportedStateContract);
		}
		std::array<Continuation, MAX_CONTINUATIONS_PER_REGION> continuations{};
		u8 continuation_count = 0;
		u32 continuation_failure_pc = candidate.entry_pc;
		if (!AppendContinuations(lifted.program, compiled, &code, &continuations,
				&continuation_count, &continuation_failure_pc, !persistent))
		{
			const u32 failed_code_bytes = static_cast<u32>(code.Size());
			m_executor->DiscardRegionCodeBuffer(&code, code_slice_offset);
			return RejectCandidate(candidate,
				BuildFailureStage::ExactContinuation,
				continuation_failure_pc,
				RegionA32::CompileFailure::Emission, true, 0, UINT16_MAX,
				RegionIR::INVALID_VALUE, failed_code_bytes);
		}
		if (lifted.program.blocks.size() >
				PersistentGeneratedEntry::MAX_INTERNAL_SOURCE_BLOCKS)
		{
			m_executor->DiscardRegionCodeBuffer(&code, code_slice_offset);
			return RejectCandidate(candidate,
				BuildFailureStage::PersistentEntry, candidate.entry_pc,
				RegionA32::CompileFailure::UnsupportedStateContract);
		}
		size_t persistent_entry_offset = static_cast<size_t>(-1);
		if (persistent)
		{
			persistent_entry_offset = 0;
			if (!PatchPersistentExits(&code, compiled, continuations,
					continuation_count))
			{
				m_executor->DiscardRegionCodeBuffer(&code, code_slice_offset);
				return RejectCandidate(candidate,
					BuildFailureStage::PersistentEntry, candidate.entry_pc,
					RegionA32::CompileFailure::Patch);
			}
		}
		if (!m_executor->CommitRegionCodeBuffer(
				code_slice_offset, code.Size()))
		{
			m_executor->DiscardRegionCodeBuffer(&code, code_slice_offset);
			m_statistics.code_cache_failures++;
			return RejectCandidate(candidate, BuildFailureStage::CodeCommit,
				candidate.entry_pc);
		}
		const size_t total_code_bytes = code.Size();
		if (evicted)
		{
			m_statistics.evictions++;
			AccumulateEntryShape(&m_profile_window_shapes, *entry,
				EntryProfileWindowExecutions(*entry));
			m_statistics.executions += static_cast<u32>(
				entry->generated_dispatch_executions -
				entry->reported_generated_dispatch_executions);
				m_statistics.continuation_failures += static_cast<u32>(
					entry->generated_continuation_failures -
					entry->reported_generated_continuation_failures);
				m_statistics.profitability_fallbacks += static_cast<u32>(
					entry->generated_profitability_fallbacks -
					entry->reported_generated_profitability_fallbacks);
			m_statistics.entry_state_fallbacks += static_cast<u32>(
				entry->generated_entry_state_fallbacks -
				entry->reported_generated_entry_state_fallbacks);
		}
		if (replacing_different_entry)
		{
			// A consumed scheduler sample belongs to the published owner generation,
			// not to this PC forever. Once that owner is truly evicted, permit the
			// same entry to be sampled and rebuilt if it becomes hot again. This is a
			// cold direct-mapped reset; same-PC replacement retains its probe history.
			ResetForwardSampleForPc(replaced_entry_pc);
		}
		RetireEntryCode(entry);
		*entry = {};
		entry->code = std::move(code);
		entry->code_slot_offset = code_slice_offset;
		// CompileResult owns the immutable compact cold-exit descriptors referenced
		// by generated MOVW/MOVT leaves. std::vector move preserves their allocated
		// storage; copying here would leave executable pointers aimed at the temporary
		// compiler result which dies at this boundary.
		entry->compiled = std::move(compiled);
		entry->state_transfers = std::move(state_transfers);
		entry->options = m_options;
		entry->entry_pc = candidate.entry_pc;
		entry->canonical_entry_pc = canonical_entry;
		entry->source_generation = m_source_generation;
		entry->source_word_count =
			RegionIR::ProgramSourceInstructionCount(lifted.program);
		entry->source_end_pc = candidate.source_end_pc;
		entry->block_count = static_cast<u32>(lifted.program.blocks.size());
		for (const RegionIR::Block& block : lifted.program.blocks)
		{
			entry->internal_source_blocks[
				entry->internal_source_block_count++] = block.pc;
		}
		entry->continuations = continuations;
		entry->continuation_count = continuation_count;
		entry->persistent_entry_offset = persistent_entry_offset;
		entry->last_use_serial = ++m_use_serial;
		entry->active = true;
		for (ProbeCandidate& probe : m_probes)
		{
			if (!probe.valid ||
				BlockExecutor::CanonicalizeRamBackedPc(
					probe.candidate.entry_pc) != entry->canonical_entry_pc ||
				probe.candidate.source_end_pc > entry->source_end_pc)
			{
				continue;
			}
			// A committed region and an equal-or-narrower probe cannot both own one
			// persistent lookup PC. The region wins transactionally; retire the now
			// unreachable probe at this already-cold publication boundary so it cannot
			// starve unrelated candidates. A genuinely wider probe is retained for a
			// later replacement contract.
			m_statistics.probe_evictions++;
			RemoveProbe(&probe);
		}
		RebuildEntryLookup();
		m_statistics.compiles++;
		m_statistics.code_bytes += total_code_bytes;
		RecordRepeatedCandidateOutcome(candidate,
			RepeatedCandidateOutcome::RegionPublished, UINT32_MAX,
			static_cast<u8>(std::min<size_t>(lifted.program.blocks.size(),
				UINT8_MAX)));
		return true;
	}

	bool Runtime::PreparePendingAtOuterBoundary()
	{
		// Probe ageing is admission-cache maintenance, not guest work. Do it only
		// at an already-required outer ownership boundary; the former 32-event
		// sampler made every scheduler event carry discovery overhead indefinitely.
		SampleProductProbeLiveness();
		const bool sampled_candidate_prepared =
			PrepareSampledForwardCandidate();
		if (!m_pending.valid)
			(void)PromoteReadyDeferredCandidate();
		bool compiled = false;
		if (!m_pending.valid || !m_executor ||
			m_pending.attempted_block_records ==
				m_executor->GetCodeCacheBlockRecordCount())
		{
			if (UseBarrierProbesForValidation())
				ArmQueuedProbes();
			const bool probes_prepared = PrepareProductProbeEntries();
			const bool publication_changed =
				std::exchange(m_publication_dirty, false);
			return sampled_candidate_prepared || probes_prepared ||
				publication_changed;
		}
		compiled = BuildPending();
		// Several generated probes can cross their thresholds before the same
		// scheduler event.  Their edge-triggered stores coalesce into one shared
		// flag, so after consuming one candidate explicitly queue the next ready
		// owner.  It will be built at the next already-required outer boundary;
		// no probe needs to keep reasserting the request in its hot loop.
		if (!m_pending.valid)
			(void)PromoteRequestedProductProbe();
		if (UseBarrierProbesForValidation())
			ArmQueuedProbes();
		const bool probes_prepared = PrepareProductProbeEntries();
		const bool publication_changed =
			std::exchange(m_publication_dirty, false);
		return sampled_candidate_prepared || compiled || probes_prepared ||
			publication_changed;
	}

	bool Runtime::SampleForwardRegionPc(u32 sampled_pc)
	{
#if defined(VITASX2_QEMU_VALIDATION)
		m_statistics.forward_event_samples++;
#else
		// Product telemetry is diagnostic-only. A profiler-off build must not turn
		// the sparse discovery sample into a 64-bit counter update on Cortex-A9.
		if (VitaPerformanceTelemetry::IsEnabled())
			m_statistics.forward_event_samples++;
#endif
		if ((sampled_pc & 3u) != 0)
			return false;

		const size_t slot_index =
			((sampled_pc >> 2) * 2654435761u) &
			(FORWARD_SAMPLE_CAPACITY - 1);
		ForwardSample& sample = m_forward_samples[slot_index];
		if (!sample.valid || sample.pc != sampled_pc)
		{
			if (sample.valid)
			{
#if defined(VITASX2_QEMU_VALIDATION)
				m_statistics.forward_sample_collisions++;
#else
				if (VitaPerformanceTelemetry::IsEnabled())
					m_statistics.forward_sample_collisions++;
#endif
			}
			sample = {};
			sample.pc = sampled_pc;
			sample.valid = true;
		}
		if (sample.consumed)
			return false;
		if (sample.observations != UINT8_MAX)
			sample.observations++;
		if (sample.observations < MINIMUM_FORWARD_EVENT_SAMPLES)
			return false;

		// The event callback owns only this scalar handoff. Source lookup, graph
		// formation, allocation, and executable publication all happen after the
		// generated dispatcher has unwound to PreparePendingAtOuterBoundary().
		sample.consumed = true;
		m_forward_sample_request_pc = sampled_pc;
		m_statistics.forward_sample_requests++;
		return true;
	}

	bool Runtime::QueueForwardCandidateAtEntry(u32 entry_pc,
		bool require_repeated_path, bool* dependency_deferred,
		u32* selected_entry_pc, bool require_direct_call_island)
	{
		if (dependency_deferred)
			*dependency_deferred = false;
		if (selected_entry_pc)
			*selected_entry_pc = entry_pc;
		if (!m_executor || m_pending.valid || HasEntryAtPc(entry_pc) ||
			(entry_pc & 3u) != 0)
		{
			return false;
		}
		PendingCandidate candidate{};
		candidate.entry_pc = entry_pc;
		candidate.backedge_block_pc = entry_pc;
		candidate.source_end_pc = entry_pc;
		candidate.valid = true;
		candidate.require_repeated_path = require_repeated_path;
		candidate.require_direct_call_island = require_direct_call_island;
		candidate.kind = CandidateKind::ForwardReducible;
		RegionSourceBlockContract source{};
		if (!m_executor->GetRegionSourceBlockContract(entry_pc, &source) ||
			source.start_pc != entry_pc || source.instruction_count < 2 ||
			source.specialized_wait || source.instruction_count >
				(UINT32_MAX - entry_pc) / sizeof(u32))
		{
			RecordRepeatedCandidateOutcome(candidate,
				RepeatedCandidateOutcome::SourceUnavailable, entry_pc);
			return false;
		}

		candidate.source_end_pc = entry_pc +
			source.instruction_count * sizeof(u32);
		std::vector<u32> blocks;
		u32 missing_contract_pc = UINT32_MAX;
		u32 selected_entry = candidate.entry_pc;
		try
		{
			if (!DiscoverSourceBackedForwardRegion(candidate, &blocks,
					&candidate.source_end_pc, &missing_contract_pc,
					&selected_entry) || blocks.empty())
			{
				if (missing_contract_pc != UINT32_MAX &&
					require_direct_call_island &&
					DeferCandidate(candidate, missing_contract_pc))
				{
					RecordRepeatedCandidateOutcome(candidate,
						RepeatedCandidateOutcome::MissingTopologyDeferred,
						missing_contract_pc,
						static_cast<u8>(std::min<size_t>(blocks.size(), UINT8_MAX)));
					if (dependency_deferred)
						*dependency_deferred = true;
					return false;
				}
				RecordRepeatedCandidateOutcome(candidate,
					missing_contract_pc != UINT32_MAX ?
						RepeatedCandidateOutcome::GraphIncomplete :
						RepeatedCandidateOutcome::GraphInvalid,
					missing_contract_pc,
					static_cast<u8>(std::min<size_t>(blocks.size(), UINT8_MAX)));
				return false;
			}
		}
		catch (const std::bad_alloc&)
		{
			m_statistics.compiler_heap_failures++;
			(void)RejectCandidate(candidate, BuildFailureStage::HeapAllocation,
				entry_pc);
			return false;
		}
		if (selected_entry != candidate.entry_pc)
		{
			candidate.entry_pc = selected_entry;
			candidate.backedge_block_pc = selected_entry;
			if (HasEntryAtPc(selected_entry))
				return false;
		}
		if (selected_entry_pc)
			*selected_entry_pc = candidate.entry_pc;
		// Active regions own only their canonical generated lookup entry. Their
		// source-block lists are immutable dependencies and may overlap: direct-call
		// leaves are deliberately cloned into caller-owned regions while retaining a
		// separate leaf entry for all other callers. Canonical-entry duplicates are
		// rejected by BuildCandidate(); source-generation changes retire the complete
		// directory before any stale dependency can execute.
		for (const ProbeCandidate& probe : m_probes)
		{
			if (!probe.valid)
				continue;
			// Probes also own a generated entry, not every immutable block they
			// inspect. Only an equal canonical entry can conflict with publication;
			// overlapping caller/leaf evidence must remain independently promotable.
			if (BlockExecutor::CanonicalizeRamBackedPc(
					probe.candidate.entry_pc) ==
				BlockExecutor::CanonicalizeRamBackedPc(candidate.entry_pc))
			{
				RecordRepeatedCandidateOutcome(candidate,
					RepeatedCandidateOutcome::ProbeOwnerOverlap,
					probe.candidate.entry_pc, static_cast<u8>(blocks.size()));
				return false;
			}
		}

		if (require_repeated_path)
		{
			// Compute reachability over the same PCSX2-owned induced CFG used by
			// forward formation. JAL's intraprocedural edge is its return PC; the
			// separately verified callee is not itself evidence of repetition.
			const size_t count = blocks.size();
			if (count > PersistentGeneratedEntry::MAX_INTERNAL_SOURCE_BLOCKS)
			{
				RecordRepeatedCandidateOutcome(candidate,
					RepeatedCandidateOutcome::GraphInvalid, UINT32_MAX,
					static_cast<u8>(std::min<size_t>(count, UINT8_MAX)));
				return false;
			}
			std::array<u32,
				PersistentGeneratedEntry::MAX_INTERNAL_SOURCE_BLOCKS> reachable{};
			auto index_of = [&](u32 pc) {
				const auto found = std::find(blocks.begin(), blocks.end(), pc);
				return found == blocks.end() ? count :
					static_cast<size_t>(found - blocks.begin());
			};
			for (size_t index = 0; index < count; index++)
			{
				RegionSourceBlockTopology topology{};
				if (!m_executor->GetRegionSourceBlockTopology(
						blocks[index], &topology))
				{
					RecordRepeatedCandidateOutcome(candidate,
						RepeatedCandidateOutcome::GraphIncomplete, blocks[index],
						static_cast<u8>(count));
					return false;
				}
				std::array<u32, 2> successors = topology.successors;
				u8 successor_count = topology.successor_count;
				if (topology.contract.instruction_count >= 2)
				{
					const u32 control_pc = blocks[index] +
						(topology.contract.instruction_count - 2) * sizeof(u32);
					u32 control = 0;
					if (ReadRamSourceWord(control_pc, &control) &&
						(control >> 26) == 0x03)
					{
						successors = {};
						successors[0] = control_pc + 2 * sizeof(u32);
						successor_count = 1;
					}
				}
				for (u8 edge = 0; edge < successor_count; edge++)
				{
					const size_t target = index_of(successors[edge]);
					if (target != count)
						reachable[index] |= 1u << target;
				}
			}
			for (size_t intermediate = 0; intermediate < count; intermediate++)
			{
				for (size_t source_index = 0; source_index < count; source_index++)
				{
					if ((reachable[source_index] & (1u << intermediate)) != 0)
						reachable[source_index] |= reachable[intermediate];
				}
			}
			bool repeated = false;
			for (size_t index = 0; index < count; index++)
				repeated |= (reachable[index] & (1u << index)) != 0;
			if (!repeated)
			{
				if (missing_contract_pc != UINT32_MAX &&
					DeferCandidate(candidate, missing_contract_pc))
				{
					RecordRepeatedCandidateOutcome(candidate,
						RepeatedCandidateOutcome::MissingTopologyDeferred,
						missing_contract_pc, static_cast<u8>(count));
					if (dependency_deferred)
						*dependency_deferred = true;
				}
				else
				{
					RecordRepeatedCandidateOutcome(candidate,
						RepeatedCandidateOutcome::NoRepeatedPath,
						missing_contract_pc, static_cast<u8>(count));
				}
				return false;
			}
		}

		if (const NegativeCandidate* negative = FindNegativeCandidate(
				candidate.entry_pc,
				candidate.source_end_pc, CandidateKind::ForwardReducible))
		{
			RecordRepeatedCandidateOutcome(candidate, negative->outcome,
				negative->failure_pc, static_cast<u8>(blocks.size()),
				negative->failure_stage, negative->failure_detail);
			return false;
		}
		if (FindProbe(candidate.entry_pc, candidate.source_end_pc,
				CandidateKind::ForwardReducible) ||
			IsDeferredCandidate(candidate.entry_pc, candidate.source_end_pc,
				CandidateKind::ForwardReducible))
		{
			RecordRepeatedCandidateOutcome(candidate,
				RepeatedCandidateOutcome::AlreadyKnown, UINT32_MAX,
				static_cast<u8>(blocks.size()));
			return false;
		}

		if (m_require_proven_profitability && !UseBarrierProbesForValidation())
		{
			ProbeCandidate* probe = SelectProbeSlot();
			if (!probe)
			{
				m_statistics.admission_saturations++;
				// Probe ownership is bounded, but a transiently full generated
				// directory is not a semantic rejection. Retain this already-formed
				// source graph in the bounded cold queue. Probe ageing runs before
				// ready-candidate promotion, so the first subsequently free slot owns
				// this candidate without another hot sample or CFG walk.
				if (!DeferAdmissionCandidate(candidate) &&
					!IsDeferredCandidate(candidate.entry_pc,
						candidate.source_end_pc, candidate.kind) &&
					!IsNegativeCandidate(candidate.entry_pc,
						candidate.source_end_pc, candidate.kind))
				{
					RecordRepeatedCandidateOutcome(candidate,
						RepeatedCandidateOutcome::AdmissionSaturated,
						UINT32_MAX, static_cast<u8>(blocks.size()));
				}
				return false;
			}
			if (probe->valid)
			{
				m_statistics.probe_evictions++;
				RemoveProbe(probe);
			}
			*probe = {};
			probe->candidate = candidate;
			probe->valid = true;
			probe->last_observation_serial = ++m_probe_serial;
			RecordRepeatedCandidateOutcome(candidate,
				RepeatedCandidateOutcome::QueuedProbe, UINT32_MAX,
				static_cast<u8>(blocks.size()));
		}
		else
		{
			m_pending = candidate;
			m_pending_budget_deferred = false;
			RecordRepeatedCandidateOutcome(candidate,
				RepeatedCandidateOutcome::QueuedPending, UINT32_MAX,
				static_cast<u8>(blocks.size()));
		}
		m_statistics.candidates++;
		return true;
	}

	bool Runtime::SourceBlockContainsVu0Work(
		const RegionSourceBlockContract& source) const
	{
		if (source.instruction_count == 0 || source.start_pc & 3u)
			return false;
		for (u32 index = 0; index < source.instruction_count; index++)
		{
			u32 opcode = 0;
			if (!ReadRamSourceWord(source.start_pc + index * sizeof(u32),
					&opcode))
			{
				return false;
			}
			const u32 primary = opcode >> 26;
			if (primary == 0x12 || primary == 0x36 || primary == 0x3e)
				return true;
		}
		return false;
	}

	bool Runtime::QueuePreparedVu0CallerCandidate(
		const RegionSourceBlockContract& source)
	{
		if (!m_executor || m_pending.valid)
			return false;
		constexpr u8 MAX_REVERSE_BLOCKS =
			PersistentGeneratedEntry::MAX_INTERNAL_SOURCE_BLOCKS;
		std::array<u32, MAX_REVERSE_BLOCKS> reverse{};
		u8 reverse_count = 1;
		reverse[0] = source.start_pc;
		for (u8 read = 0; read < reverse_count; read++)
		{
			const u32 pc = reverse[read];
			RegionSourceBlockContract current{};
			if (m_executor->GetRegionSourceBlockContract(pc, &current))
			{
				if (SourceBlockContainsVu0Work(current))
				{
					RegionDirectCallPredecessors calls{};
					if (m_executor->GetRegionDirectCallPredecessors(pc, &calls))
					{
						for (u8 index = 0; index < calls.count; index++)
						{
							bool deferred = false;
							if (QueueForwardCandidateAtEntry(
									calls.entry_pcs[index], true, &deferred,
									nullptr, true))
							{
								return true;
							}
							if (deferred)
								return false;
						}
					}
				}
				if (current.instruction_count >= 2)
				{
					const u32 call_pc = current.start_pc +
						(current.instruction_count - 2) * sizeof(u32);
					u32 opcode = 0;
					if (ReadRamSourceWord(call_pc, &opcode) &&
						(opcode >> 26) == 0x03)
					{
						const u32 callee = ((call_pc + sizeof(u32)) & 0xf0000000u) |
							((opcode & 0x03ffffffu) << 2);
						RegionSourceBlockContract callee_source{};
						bool deferred = false;
						if (m_executor->GetRegionSourceBlockContract(
								callee, &callee_source) &&
							SourceBlockContainsVu0Work(callee_source) &&
							QueueForwardCandidateAtEntry(current.start_pc, true,
								&deferred, nullptr, true))
						{
							return true;
						}
						if (deferred)
							return false;
					}
				}
			}

			RegionSourceBlockPredecessors predecessors{};
			if (!m_executor->GetRegionSourceBlockPredecessors(pc, &predecessors))
				continue;
			for (u8 index = 0; index < predecessors.count; index++)
			{
				const u32 predecessor = predecessors.entry_pcs[index];
				if (reverse_count >= reverse.size() ||
					std::find(reverse.begin(), reverse.begin() + reverse_count,
						predecessor) != reverse.begin() + reverse_count)
				{
					continue;
				}
				reverse[reverse_count++] = predecessor;
			}
		}
		return false;
	}

	bool Runtime::PrepareSampledForwardCandidate()
	{
		const u32 sampled_pc = std::exchange(
			m_forward_sample_request_pc, UINT32_MAX);
		if (sampled_pc == UINT32_MAX)
			return false;
		auto reject = [&]() {
			m_statistics.forward_sample_rejections++;
			return false;
		};
		if (!m_executor)
			return reject();

		// A scheduler boundary can land at any source-backed block inside a short
		// direct leaf. Walk the immutable incoming-link graph backwards through its
		// callers and their enclosing CFG. Leaf-only compilation remains a validation
		// tier: on Vita its mandatory dynamic-return materialization erased the local
		// gain. Product discovery therefore seeks a repeated enclosing unit which can
		// own VU0 state across the call/return seam. This is bounded cold discovery
		// only; the final semantic and target-cost certificate remains authoritative.
		constexpr u8 MAX_REVERSE_BLOCKS =
			PersistentGeneratedEntry::MAX_INTERNAL_SOURCE_BLOCKS;
		std::array<u32, MAX_REVERSE_BLOCKS> reverse{};
		std::array<u32, MAX_REVERSE_BLOCKS> leaf_seeds{};
		std::array<u32, MAX_REVERSE_BLOCKS +
			RegionDirectCallPredecessors::CAPACITY> caller_seeds{};
		u8 reverse_count = 1;
		reverse[0] = sampled_pc;
		u8 leaf_seed_count = 0;
		u8 caller_seed_count = 0;
		auto append_seed = [](auto* seeds, u8* count, u32 pc) {
			if (*count < seeds->size() &&
				std::find(seeds->begin(), seeds->begin() + *count, pc) ==
					seeds->begin() + *count)
			{
				(*seeds)[(*count)++] = pc;
			}
		};
		for (u8 read = 0; read < reverse_count; read++)
		{
			const u32 pc = reverse[read];
			RegionSourceBlockContract source{};
			RegionDirectCallPredecessors calls{};
			const bool has_calls =
				m_executor->GetRegionDirectCallPredecessors(pc, &calls) &&
				calls.count != 0;
			if (has_calls && m_executor->GetRegionSourceBlockContract(pc, &source) &&
				SourceBlockContainsVu0Work(source))
			{
				// First try the measured unit itself. Acyclic VU0 leaves have their own
				// generated entry-frequency certificate; consuming this sample on a much
				// larger caller first left the profitable leaf permanently undiscovered.
				// A broader caller may still supersede the leaf later through the ordinary
				// source graph and target-cost proof.
				append_seed(&leaf_seeds, &leaf_seed_count, pc);
			}
			if (has_calls)
			{
				for (u8 index = 0; index < calls.count; index++)
					append_seed(&caller_seeds, &caller_seed_count,
						calls.entry_pcs[index]);
			}
			RegionSourceBlockPredecessors predecessors{};
			if (!m_executor->GetRegionSourceBlockPredecessors(pc, &predecessors))
				continue;
			for (u8 index = 0; index < predecessors.count; index++)
			{
				const u32 predecessor = predecessors.entry_pcs[index];
				if (reverse_count >= reverse.size() ||
					std::find(reverse.begin(), reverse.begin() + reverse_count,
						predecessor) != reverse.begin() + reverse_count)
				{
					continue;
				}
				reverse[reverse_count++] = predecessor;
			}
		}
		auto try_seeds = [&](const auto& seeds, u8 count,
			bool require_repeated_path, bool require_direct_call_island) {
			for (u8 index = 0; index < count; index++)
			{
				bool dependency_deferred = false;
				if (QueueForwardCandidateAtEntry(seeds[index],
						require_repeated_path, &dependency_deferred, nullptr,
						require_direct_call_island))
				{
					m_statistics.forward_sample_candidates++;
					return true;
				}
			}
			return false;
		};
		// Explicit validation can still measure the leaf ABI. Product's zero leaf
		// floor skips it and goes directly to a repeated caller/source-graph unit.
		if ((m_minimum_observed_vu0_fmac_leaf_invocations != 0 &&
			 try_seeds(leaf_seeds, leaf_seed_count, false, false)) ||
			try_seeds(caller_seeds, caller_seed_count, true,
				leaf_seed_count != 0))
		{
			return true;
		}
		if (leaf_seed_count != 0)
		{
			// A JAL block can sit inside a larger natural loop and therefore cannot
			// dominate the loop header by itself. Try the already-published incoming
			// source chain from the outermost bounded predecessor inward. Requiring a
			// repeated path prevents an acyclic function prefix or unrelated caller
			// from becoming the execution unit.
			for (u8 reverse_index = reverse_count; reverse_index != 0;
				reverse_index--)
			{
				const u32 seed = reverse[reverse_index - 1];
				if (std::find(leaf_seeds.begin(),
						leaf_seeds.begin() + leaf_seed_count, seed) !=
						leaf_seeds.begin() + leaf_seed_count ||
					std::find(caller_seeds.begin(),
						caller_seeds.begin() + caller_seed_count, seed) !=
						caller_seeds.begin() + caller_seed_count)
				{
					continue;
				}
				bool dependency_deferred = false;
				if (QueueForwardCandidateAtEntry(seed, true,
						&dependency_deferred, nullptr, true))
				{
					m_statistics.forward_sample_candidates++;
					return true;
				}
			}
		}
		// A sample which is not a direct-call target can still seed an ordinary
		// source-backed forward region. Do this last so it cannot duplicate a leaf
		// already rejected for a sound semantic or target-cost reason.
		if (std::find(leaf_seeds.begin(), leaf_seeds.begin() + leaf_seed_count,
				sampled_pc) == leaf_seeds.begin() + leaf_seed_count &&
			std::find(caller_seeds.begin(), caller_seeds.begin() + caller_seed_count,
				sampled_pc) == caller_seeds.begin() + caller_seed_count &&
			QueueForwardCandidateAtEntry(sampled_pc))
		{
			m_statistics.forward_sample_candidates++;
			return true;
		}
		return reject();
	}

	bool Runtime::ObservePersistentEventSample(u32 sampled_pc)
	{
		(void)sampled_pc;
		SampleProductProbeLiveness();
		return m_publication_dirty ||
		       (m_pending.valid && BuildBudgetAvailable());
	}

	bool Runtime::ObservePersistentEventBoundarySlow(u32 sampled_pc)
	{
		if (UseBarrierProbesForValidation())
		{
			if (m_armed_probe_count != 0)
				AdvanceProbeEpoch();
			return m_publication_dirty;
		}

		// This slow form is retained for the callable/barrier validation policy.
		// Product probes and regions use the one shared generated-maintenance flag in
		// the inline event path and age their cache only at existing outer boundaries.
		if (m_generated_maintenance_requested != 0)
			return ProcessGeneratedMaintenance();
		AdvanceEventScopedProbeEpoch();
		if (m_forward_sample_request_pc != UINT32_MAX)
			return true;
		if (m_forward_sample_event_countdown > 1)
			m_forward_sample_event_countdown--;
		else
		{
			m_forward_sample_event_countdown = PROBE_SAMPLE_EVENT_INTERVAL;
			if (SampleForwardRegionPc(sampled_pc))
				return true;
		}
		if (!m_pending.valid)
		{
			if (m_probe_sample_event_countdown > 1)
				m_probe_sample_event_countdown--;
			else
			{
				m_probe_sample_event_countdown = PROBE_SAMPLE_EVENT_INTERVAL;
				return ObservePersistentEventSample(sampled_pc);
			}
		}
		return m_publication_dirty ||
		       (m_pending.valid && BuildBudgetAvailable());
	}

	bool Runtime::RequiresOuterBoundary() const
	{
		return m_publication_dirty ||
			(m_pending.valid && BuildBudgetAvailable()) ||
			m_forward_sample_request_pc != UINT32_MAX ||
			(m_executor && m_source_generation != 0 &&
			 m_executor->GetSourceGeneration() != m_source_generation);
	}

	Execution Runtime::ExecuteAtPc(u32 pc, bool persistent_dispatch_frame)
	{
		Execution execution{};
		if (m_bypass_once_pc == pc)
		{
			m_bypass_once_pc = UINT32_MAX;
			execution.bypassed = true;
			return execution;
		}

		Entry* const entry = FindEntry(pc);
		// Synchronize() is the outer-boundary owner for source/config generations.
		// Re-reading and comparing the complete option set here made every admitted
		// hot invocation pay for a lifecycle proof already performed immediately
		// before this call.
		if (!entry || !m_executor || !eeMem ||
			!vtlb_private::vtlbdata.vmap)
		{
			return execution;
		}
		execution.entry_pc = entry->entry_pc;
		execution.continuation_count = entry->continuation_count;

		// This private call frame is deliberately not a complete architectural
		// snapshot. The allocated backend publishes its exact slot/word ABI; seed
		// only those inputs and copy back only verifier-derived output words. This
		// avoids a ~1.2 KiB full-state copy on every hot invocation without relying
		// on an opcode-specific flush list.
		RegionIR::CanonicalState state;
		if (!LoadRuntimeStateContract(entry->state_transfers, &state))
			return execution;
		state.pc = cpuRegs.pc;
		const u32 entry_range_start =
			entry->compiled.entry_range_stride_bytes != 0 ?
				state.gpr[entry->compiled.entry_range_induction_gpr]._u32[0] : 0;

		RegionA32::ExecutionContext context{};
		context.state = &state;
		context.vmap = reinterpret_cast<const u32*>(vtlb_private::vtlbdata.vmap);
		context.host_memory_base = reinterpret_cast<const u8*>(
			vtlb_private::vtlbdata.host_memory_base);
		context.main_ram = eeMem->Main;
		const u32 ram_size = std::min(Ps2MemSize::ExposedRam, Ps2MemSize::MainRam);
		context.main_ram_last_word = eeMem->Main + ram_size - sizeof(u32);
		context.ram_source_page_live_flags =
			m_executor->RamSourcePageLiveFlags();
		context.ram_source_chunk_live_bits =
			m_executor->RamSourceChunkLiveBits();
		context.main_ram_limit = ram_size;
		context.identity_main_ram_limit =
			vtlb_private::HasDefaultMainRamIdentityWindow() ? ram_size : 0;
		context.next_event_cycle_low = static_cast<u32>(cpuRegs.nextEventCycle);
		context.next_event_cycle_high = static_cast<u32>(cpuRegs.nextEventCycle >> 32);

		execution.executed = true;
		const u32 token = reinterpret_cast<RegionA32::GeneratedRegion>(
			entry->code.EntryPoint())(&context);
		execution.generated_token = token;
		execution.generated_completed = context.result.completed;
		execution.valid = token == 1 && context.result.completed == 1;
		if (!execution.valid)
		{
			execution.failure = Execution::Failure::GeneratedReturn;
			return execution;
		}

		if (!StoreRuntimeStateContract(entry->state_transfers, state))
		{
			execution.failure = Execution::Failure::StateMaterialization;
			execution.valid = false;
			return execution;
		}
		cpuRegs.pc = state.pc;
		execution.resume_pc = cpuRegs.pc;
		execution.reason =
			static_cast<RegionIR::ExitReason>(context.result.reason);
		execution.memory_address = context.result.memory_address;
		execution.pending_raw_cycles = context.result.pending_raw_cycles;
		execution.cycle_commit_deferred =
			context.result.cycle_commit_deferred != 0;
		const RegionIR::ExitReason generated_reason = execution.reason;
		execution.generated_reason = generated_reason;
		bool continuation_completed = false;
		BlockExitKind continuation_exit = BlockExitKind::Direct;
		if (execution.cycle_commit_deferred)
		{
			for (u8 index = 0; index < entry->continuation_count; index++)
			{
				const Continuation& candidate = entry->continuations[index];
				execution.continuation_pc_matches +=
					candidate.resume_pc == cpuRegs.pc ? 1u : 0u;
				execution.continuation_debt_matches +=
					candidate.pending_raw_cycles == execution.pending_raw_cycles ? 1u : 0u;
			}
			const Continuation* const continuation = FindContinuation(*entry,
				cpuRegs.pc, execution.pending_raw_cycles);
			if (!continuation)
			{
				m_statistics.continuation_failures++;
				execution.failure = Execution::Failure::MissingContinuation;
				execution.valid = false;
				return execution;
			}
			else
			{
				if (persistent_dispatch_frame)
				{
					execution.persistent_resume_entry =
						entry->code.Data() +
						continuation->persistent_entry_offset;
					BlockExecutionResult& resumed =
						execution.persistent_resume_result;
					resumed.path = BlockExecutionPath::Compiled;
					resumed.exit = BlockExitKind::Direct;
					resumed.start_pc = continuation->resume_pc;
					resumed.instruction_count =
						(continuation->source_end_pc -
						 continuation->resume_pc) / sizeof(u32);
					resumed.source_instruction_count =
						resumed.instruction_count;
					resumed.scaled_cycles = continuation->scaled_cycles;
					resumed.code_size = entry->code.Size() -
						continuation->persistent_entry_offset;
					resumed.cache_hit = true;
					resumed.lookup_hit = true;
					resumed.fast_dispatch_hit = true;
					resumed.scheduler_test_elided =
						!continuation->scheduler_test_at_end;
					resumed.persistent_resume_requires_boundary =
						generated_reason ==
						RegionIR::ExitReason::SelfModifyingCode;
				}
				else
				{
					bool scheduler_test_elided = false;
					if (!m_executor->ExecuteRegionContinuation(entry->code,
							continuation->callable_entry_offset, false,
							&continuation_exit,
							&scheduler_test_elided))
					{
						m_statistics.continuation_failures++;
						execution.failure = Execution::Failure::ContinuationExecution;
						execution.valid = false;
						return execution;
					}
					m_statistics.continuation_event_exits +=
						continuation_exit == BlockExitKind::Event ? 1u : 0u;
					m_statistics.continuation_scheduler_elided_exits +=
						scheduler_test_elided ? 1u : 0u;
					execution.scheduler_test_elided = scheduler_test_elided;
				}
				m_statistics.continuation_exits++;
				m_statistics.continuation_nonzero_debt_exits +=
					execution.pending_raw_cycles != 0 ? 1u : 0u;
				continuation_completed = true;
			}
		}

		entry->executions++;
		if (continuation_completed)
		{
			// The private suffix has now reached the original source fragment's
			// direct or event boundary.  Keep the scheduler in its existing provider
			// owner: EventHorizon asks recExecute() to run _cpuEventTest_Shared()
			// exactly once after this callable frame has unwound.  A direct suffix
			// needs no additional device work.
			execution.reason = execution.persistent_resume_entry ?
				RegionIR::ExitReason::RegionBoundary :
				(continuation_exit == BlockExitKind::Event ?
				RegionIR::ExitReason::EventHorizon :
				RegionIR::ExitReason::RegionBoundary);
			execution.pending_raw_cycles = 0;
			execution.cycle_commit_deferred = false;
		}
		if (entry->compiled.entry_range_stride_bytes != 0 &&
			(execution.reason == RegionIR::ExitReason::RegionBoundary ||
				execution.reason == RegionIR::ExitReason::EventHorizon))
		{
			const u32 entry_range_end =
				state.gpr[entry->compiled.entry_range_induction_gpr]._u32[0];
			const u32 delta = entry_range_end - entry_range_start;
			if ((delta % entry->compiled.entry_range_stride_bytes) == 0)
				entry->work_units += delta / entry->compiled.entry_range_stride_bytes;
		}
		entry->last_use_serial = ++m_use_serial;
		execution.requires_outer_synchronization =
			m_executor->GetSourceGeneration() != m_source_generation;
		m_statistics.executions++;
		switch (generated_reason)
		{
			case RegionIR::ExitReason::RegionBoundary:
				m_statistics.boundary_exits++;
				break;
			case RegionIR::ExitReason::EventHorizon:
				m_statistics.event_exits++;
				break;
			case RegionIR::ExitReason::EventBudgetFallback:
				m_statistics.event_budget_fallbacks++;
				m_bypass_once_pc = cpuRegs.pc;
				break;
			case RegionIR::ExitReason::ProfitabilityFallback:
				m_statistics.profitability_fallbacks++;
				m_bypass_once_pc = cpuRegs.pc;
				break;
			case RegionIR::ExitReason::EntryStateFallback:
				// The representation guard precedes the frame and every guest
				// instruction. Execute the exact tier-zero block once at the same PC;
				// do not retire an otherwise useful region for dynamic entry state.
				m_statistics.entry_state_fallbacks++;
				m_bypass_once_pc = cpuRegs.pc;
				break;
			case RegionIR::ExitReason::MemoryAlignment:
				m_statistics.memory_alignment_exits++;
				m_statistics.memory_exits++;
				if (!continuation_completed)
					m_bypass_once_pc = cpuRegs.pc;
				break;
			case RegionIR::ExitReason::MemoryHandler:
				m_statistics.memory_handler_exits++;
				m_statistics.memory_exits++;
				if (!continuation_completed)
					m_bypass_once_pc = cpuRegs.pc;
				break;
			case RegionIR::ExitReason::MemoryTranslation:
				m_statistics.memory_translation_exits++;
				m_statistics.memory_exits++;
				if (!m_statistics.translation_snapshot_valid &&
					entry->compiled.entry_range_stride_bytes != 0)
				{
					const u32 induction =
						entry->compiled.entry_range_induction_gpr;
					const u32 bound = entry->compiled.entry_range_bound_gpr;
					m_statistics.translation_snapshot_valid = 1;
					m_statistics.translation_start_low = state.gpr[induction]._u32[0];
					m_statistics.translation_start_high = state.gpr[induction]._u32[1];
					m_statistics.translation_bound_low = state.gpr[bound]._u32[0];
					m_statistics.translation_bound_high = state.gpr[bound]._u32[1];
					m_statistics.translation_identity_limit =
						context.identity_main_ram_limit;
					m_statistics.translation_stride_bytes =
						entry->compiled.entry_range_stride_bytes;
				}
				if (!continuation_completed)
					m_bypass_once_pc = cpuRegs.pc;
				break;
			case RegionIR::ExitReason::SelfModifyingCode:
				m_statistics.self_modifying_code_exits++;
				m_statistics.memory_exits++;
				if (!continuation_completed)
					m_bypass_once_pc = cpuRegs.pc;
				break;
			case RegionIR::ExitReason::UnsupportedOpcode:
			case RegionIR::ExitReason::MemoryObserver:
			case RegionIR::ExitReason::HelperObserver:
			case RegionIR::ExitReason::UnsupportedControlFlow:
			case RegionIR::ExitReason::ExceptionObserver:
				if (!continuation_completed)
				{
					m_statistics.continuation_failures++;
					execution.failure = Execution::Failure::UnhandledExit;
					execution.valid = false;
				}
				break;
			default:
				if (!continuation_completed)
				{
					execution.failure = Execution::Failure::UnhandledExit;
					execution.valid = false;
				}
				break;
		}
		return execution;
	}

	PersistentChainExecution Runtime::ExecutePersistentChain(
		PersistentEventOwner event_owner, void* event_userdata,
		PersistentUnwindPredicate should_unwind, void* unwind_userdata)
	{
		PersistentChainExecution chain{};
		for (;;)
		{
			Execution execution = ExecuteAtPc(cpuRegs.pc, true);
			if (!execution.executed)
			{
				chain.bypassed |= execution.bypassed;
				chain.outcome = (chain.region_count != 0 || execution.bypassed) ?
					PersistentChainExecution::Outcome::ResumeTierZero :
					PersistentChainExecution::Outcome::NoRegion;
				return chain;
			}

			chain.last = execution;
			chain.region_count++;
			const bool invalid_memory_continuation =
				(execution.reason == RegionIR::ExitReason::MemoryAlignment ||
				 execution.reason == RegionIR::ExitReason::MemoryHandler ||
				 execution.reason == RegionIR::ExitReason::MemoryTranslation ||
				 execution.reason == RegionIR::ExitReason::SelfModifyingCode) &&
				(!execution.cycle_commit_deferred ||
				 execution.pending_raw_cycles != 0);
			if (!execution.valid || invalid_memory_continuation)
			{
				chain.outcome = PersistentChainExecution::Outcome::Fatal;
				return chain;
			}
			if (execution.persistent_resume_entry)
			{
				chain.persistent_resume_entry =
					execution.persistent_resume_entry;
				chain.persistent_resume_result =
					execution.persistent_resume_result;
				chain.outcome =
					PersistentChainExecution::Outcome::ResumeGenerated;
				return chain;
			}

			if (execution.reason == RegionIR::ExitReason::EventHorizon &&
				(!event_owner || !event_owner(event_userdata)))
			{
				chain.outcome = PersistentChainExecution::Outcome::UnwindOuter;
				return chain;
			}
			if (execution.generated_reason ==
					RegionIR::ExitReason::SelfModifyingCode ||
				execution.requires_outer_synchronization ||
				RequiresOuterBoundary() ||
				(should_unwind && should_unwind(unwind_userdata)))
			{
				chain.outcome = PersistentChainExecution::Outcome::UnwindOuter;
				return chain;
			}
		}
	}

	size_t Runtime::CopyBarrierPcs(u32* output, size_t capacity) const
	{
		if (!output && capacity != 0)
			return 0;
		size_t count = 0;
		for (const Entry& entry : m_entries)
		{
			if (!entry.active ||
				entry.persistent_entry_offset != static_cast<size_t>(-1))
			{
				continue;
			}
			if (count >= capacity)
				return capacity + 1;
			output[count++] = entry.entry_pc;
		}
		if (UseBarrierProbesForValidation())
		{
			for (const ProbeCandidate& probe : m_probes)
			{
				if (!probe.armed)
					continue;
				if (count >= capacity)
					return capacity + 1;
				output[count++] = probe.candidate.entry_pc;
			}
		}
		return count;
	}

	size_t Runtime::CopyPersistentGeneratedEntries(
		PersistentGeneratedEntry* output, size_t capacity) const
	{
		if (!output && capacity != 0)
			return 0;
		size_t count = 0;
		for (const Entry& entry : m_entries)
		{
			if (!entry.active ||
				entry.persistent_entry_offset == static_cast<size_t>(-1))
				continue;
			if (entry.persistent_entry_offset >= entry.code.Size())
				return capacity + 1;
			if (count >= capacity)
				return capacity + 1;
			PersistentGeneratedEntry& generated = output[count++];
			generated.pc = entry.entry_pc;
			generated.entry_point =
				static_cast<const u8*>(entry.code.EntryPoint()) +
				entry.persistent_entry_offset;
			if (entry.compiled.compatible_entry_offset !=
					static_cast<size_t>(-1))
			{
				if (entry.compiled.compatible_entry_offset >= entry.code.Size() ||
					!entry.compiled.compatible_entry_signature.IsValid())
				{
					return capacity + 1;
				}
				generated.compatible_entry_point =
					static_cast<const u8*>(entry.code.EntryPoint()) +
					entry.compiled.compatible_entry_offset;
				generated.compatible_signature =
					entry.compiled.compatible_entry_signature;
			}
			if (entry.compiled.pre_entry_profitability_leaves != 0)
			{
				// A profitability-selected region owns the external invocation entry.
				// If its exact selector rejects, its passthrough leaf enters tier zero at
				// the same PC. Keep every tier-zero edge originating inside the region CFG
				// linked to tier zero so that fallback executes one complete invocation
				// rather than re-entering the selector at every natural-loop backedge.
				generated.external_incoming_only = true;
				generated.internal_source_blocks = entry.internal_source_blocks;
				generated.internal_source_block_count =
					entry.internal_source_block_count;
			}
		}
		if (!UseBarrierProbesForValidation())
		{
			for (const ProbeCandidate& probe : m_probes)
			{
				if (!probe.valid || !probe.armed ||
					probe.persistent_entry_offset == static_cast<size_t>(-1))
				{
					continue;
				}
				if (!probe.code.EntryPoint() ||
					probe.persistent_entry_offset >= probe.code.Size() ||
					count >= capacity)
				{
					return capacity + 1;
				}
				PersistentGeneratedEntry& generated = output[count++];
				generated.pc = probe.has_iteration_guard ?
					probe.candidate.entry_pc : probe.candidate.backedge_block_pc;
				generated.entry_point =
					static_cast<const u8*>(probe.code.EntryPoint()) +
					probe.persistent_entry_offset;
				if (probe.compatible_entry_offset != static_cast<size_t>(-1))
				{
					if (probe.compatible_entry_offset >= probe.code.Size() ||
						!probe.compatible_signature.IsValid())
					{
						return capacity + 1;
					}
					generated.compatible_entry_point =
						static_cast<const u8*>(probe.code.EntryPoint()) +
						probe.compatible_entry_offset;
					generated.compatible_signature =
						probe.compatible_signature;
				}
				if (probe.has_iteration_guard)
				{
					generated.external_incoming_only = true;
					generated.internal_source_blocks =
						probe.internal_source_blocks;
					generated.internal_source_block_count =
						probe.internal_source_block_count;
				}
			}
		}
		return count;
	}

	bool Runtime::RepatchPersistentGeneratedExits()
	{
		if (!m_executor)
			return false;

		PersistentRegionAbi abi{};
		if (!m_executor->GetPersistentRegionAbi(&abi))
			return false;

		for (Entry& entry : m_entries)
		{
			if (!entry.active ||
				entry.persistent_entry_offset == static_cast<size_t>(-1))
			{
				continue;
			}

			bool changed = false;
			for (const RegionA32::PersistentExitPatch& patch :
				entry.compiled.persistent_exit_patches)
			{
				if (patch.reason != RegionIR::ExitReason::RegionBoundary)
					continue;
				if (patch.dynamic_resume_pc)
				{
					if (!abi.scheduler_elided_redispatch ||
						!entry.code.PatchBranchToAddress(patch.branch_offset,
							abi.scheduler_elided_redispatch))
					{
						return false;
					}
				}
				else if (patch.compatible_signature.IsValid())
				{
					if (patch.selector_branch_offset == static_cast<size_t>(-1) ||
						patch.canonical_leaf_offset == static_cast<size_t>(-1) ||
						patch.canonical_branch_offset == static_cast<size_t>(-1) ||
						patch.compatible_leaf_offset == static_cast<size_t>(-1))
					{
						return false;
					}
					const void* const compatible =
						m_executor->GetPersistentCompatibleDispatchEntryPoint(
							patch.resume_pc, patch.compatible_signature);
					const void* canonical =
						m_executor->GetPersistentDispatchEntryPoint(patch.resume_pc);
					if (!canonical)
						canonical = abi.scheduler_elided_redispatch;
					const bool use_compatible = compatible != nullptr;
					if (!canonical || !entry.code.PatchBranch(
							patch.selector_branch_offset,
							use_compatible ? patch.compatible_leaf_offset :
								patch.canonical_leaf_offset) ||
						!entry.code.PatchBranchToAddress(
							use_compatible ? patch.branch_offset :
								patch.canonical_branch_offset,
							use_compatible ? compatible : canonical))
					{
						return false;
					}
				}
				else
				{
					const void* target =
						m_executor->GetPersistentDispatchEntryPoint(patch.resume_pc);
					if (!target)
						target = abi.scheduler_elided_redispatch;
					if (!target || !entry.code.PatchBranchToAddress(
							patch.branch_offset, target))
					{
						return false;
					}
				}
				changed = true;
			}
			if (changed && !entry.code.Flush())
				return false;
		}
		return true;
	}

	void Runtime::AcknowledgePublishedCodeRetirements()
	{
		if (m_executor)
			m_executor->AcknowledgeRegionCodeRetirements();
	}

#if defined(VITASX2_QEMU_VALIDATION)
	void Runtime::SetBuildBudgetForValidation(u32 tokens, u64 refill_cycles)
	{
		m_build_token_capacity = std::max<u32>(1, tokens);
		m_build_tokens = std::min(tokens, m_build_token_capacity);
		m_build_token_refill_cycles = std::max<u64>(1, refill_cycles);
		m_build_budget_cycle = cpuRegs.cycle;
		m_pending_budget_deferred = false;
	}

	bool Runtime::PrepareForwardRegionForValidation(u32 entry_pc)
	{
		if (!m_executor || m_pending.valid || HasEntryAtPc(entry_pc) ||
			(entry_pc & 3u) != 0)
		{
			return false;
		}
		RegionSourceBlockContract source{};
		if (!m_executor->GetRegionSourceBlockContract(entry_pc, &source) ||
			source.instruction_count < 2 || source.specialized_wait ||
			source.instruction_count >
				(UINT32_MAX - entry_pc) / sizeof(u32))
		{
			return false;
		}

		PendingCandidate candidate{};
		candidate.entry_pc = entry_pc;
		candidate.source_end_pc = entry_pc +
			source.instruction_count * sizeof(u32);
		candidate.valid = true;
		candidate.kind = CandidateKind::ForwardReducible;
		std::vector<u32> blocks;
		u32 source_end_pc = candidate.source_end_pc;
		u32 missing_contract_pc = UINT32_MAX;
		if (!DiscoverSourceBackedForwardRegion(candidate, &blocks,
				&source_end_pc, &missing_contract_pc) || blocks.empty())
		{
			return false;
		}
		candidate.source_end_pc = source_end_pc;
		if (IsNegativeCandidate(candidate.entry_pc, candidate.source_end_pc,
				candidate.kind) ||
			IsDeferredCandidate(candidate.entry_pc, candidate.source_end_pc,
				candidate.kind))
		{
			return false;
		}
		m_pending = candidate;
		m_pending_budget_deferred = false;
		m_statistics.candidates++;
		(void)PreparePendingAtOuterBoundary();
		return HasEntryAtPc(entry_pc);
	}

	bool Runtime::PrepareRepeatedForwardRegionForValidation(u32 entry_pc)
	{
		u32 selected_entry_pc = entry_pc;
		if (!QueueForwardCandidateAtEntry(entry_pc, true, nullptr,
				&selected_entry_pc, true))
			return false;
		(void)PreparePendingAtOuterBoundary();
		return HasEntryAtPc(selected_entry_pc);
	}

	bool Runtime::PrepareObservedBackedgeForValidation(u32 source_start_pc,
		u32 branch_pc, u32 source_end_pc, u32 target_pc)
	{
		if (!QueueBackwardCandidate(source_start_pc, branch_pc, source_end_pc,
				target_pc, true))
		{
			return false;
		}
		(void)PreparePendingAtOuterBoundary();
		return HasEntryAtPc(target_pc);
	}

	bool Runtime::GetEntrySnapshotForValidation(u32 entry_pc,
		Statistics::EntrySnapshot* snapshot) const
	{
		if (!snapshot)
			return false;
		for (const Entry& entry : m_entries)
		{
			if (!entry.active || entry.entry_pc != entry_pc)
				continue;
			*snapshot = MakeEntrySnapshot(entry, 0);
			return true;
		}
		return false;
	}

#endif

	u64 Runtime::EntryWindowExecutions(const Entry& entry)
	{
		return (entry.executions - entry.reported_executions) +
			static_cast<u32>(entry.generated_dispatch_executions -
				entry.reported_generated_dispatch_executions);
	}

	u64 Runtime::EntryProfileWindowExecutions(const Entry& entry)
	{
		return (entry.executions - entry.profile_reported_executions) +
			static_cast<u32>(entry.generated_dispatch_executions -
				entry.profile_reported_generated_dispatch_executions);
	}

	Statistics::EntrySnapshot Runtime::MakeEntrySnapshot(
		const Entry& entry, u64 executions)
	{
		Statistics::EntrySnapshot result{};
		result.pc = entry.entry_pc;
		result.executions = executions;
		result.counted_iterations = static_cast<u32>(
			entry.generated_counted_iterations -
				entry.profile_reported_generated_counted_iterations);
		result.profitability_fallbacks = static_cast<u32>(
			entry.generated_profitability_fallbacks -
			entry.reported_generated_profitability_fallbacks);
		result.entry_state_fallbacks = static_cast<u32>(
			entry.generated_entry_state_fallbacks -
			entry.reported_generated_entry_state_fallbacks);
		result.block_count = entry.block_count;
		result.source_words = entry.source_word_count;
		result.host_instructions = entry.compiled.host_instructions;
		result.hot_host_instructions = entry.compiled.hot_host_instructions;
		result.hot_host_loads = entry.compiled.hot_host_loads;
		result.hot_host_stores = entry.compiled.hot_host_stores;
		result.hot_state_loads = entry.compiled.hot_state_loads;
		result.hot_state_stores = entry.compiled.hot_state_stores;
		result.hot_stack_loads = entry.compiled.hot_stack_loads;
		result.hot_stack_stores = entry.compiled.hot_stack_stores;
		result.block_stack_loads = entry.compiled.block_stack_loads;
		result.block_stack_stores = entry.compiled.block_stack_stores;
		result.spill_word_loads = entry.compiled.spill_word_loads;
		result.spill_word_stores = entry.compiled.spill_word_stores;
		result.spill_vfp_loads = entry.compiled.spill_vfp_loads;
		result.spill_vfp_stores = entry.compiled.spill_vfp_stores;
		result.spill_vector_loads = entry.compiled.spill_vector_loads;
		result.spill_vector_stores = entry.compiled.spill_vector_stores;
		result.block_spill_word_loads = entry.compiled.block_spill_word_loads;
		result.block_spill_word_stores = entry.compiled.block_spill_word_stores;
		result.block_spill_vfp_loads = entry.compiled.block_spill_vfp_loads;
		result.block_spill_vfp_stores = entry.compiled.block_spill_vfp_stores;
		result.block_spill_vector_loads = entry.compiled.block_spill_vector_loads;
		result.block_spill_vector_stores = entry.compiled.block_spill_vector_stores;
		result.vu0_raw_spill_word_loads = entry.compiled.vu0_raw_spill_word_loads;
		result.vu0_raw_spill_word_stores = entry.compiled.vu0_raw_spill_word_stores;
		result.vu0_pipeline_spill_word_loads =
			entry.compiled.vu0_pipeline_spill_word_loads;
		result.vu0_pipeline_spill_word_stores =
			entry.compiled.vu0_pipeline_spill_word_stores;
		result.vu0_transform_spill_word_loads =
			entry.compiled.vu0_transform_spill_word_loads;
		result.vu0_transform_spill_word_stores =
			entry.compiled.vu0_transform_spill_word_stores;
		result.frame_bytes = entry.compiled.frame_bytes;
		result.work_scratch_bytes = entry.compiled.work_scratch_bytes;
		result.hot_code_bytes = entry.compiled.hot_code_bytes;
		result.cold_code_bytes = entry.compiled.cold_code_bytes;
		result.semantic_kernel_kind = entry.compiled.semantic_kernel_kind;
		result.semantic_kernel_hot_bytes =
			entry.compiled.semantic_kernel_hot_bytes;
		result.semantic_kernel_bytes_per_iteration =
			entry.compiled.semantic_kernel_bytes_per_iteration;
		result.semantic_kernel_minimum_profitable_bytes =
			entry.compiled.semantic_kernel_minimum_profitable_bytes;
		result.semantic_kernel_target_cost_valid =
			entry.compiled.semantic_kernel_target_cost_valid;
		result.prologue_hot_bytes = entry.compiled.prologue_hot_bytes;
		result.entry_event_hot_bytes = entry.compiled.entry_event_hot_bytes;
		result.entry_iteration_hot_bytes = entry.compiled.entry_iteration_hot_bytes;
		result.entry_memory_hot_bytes = entry.compiled.entry_memory_hot_bytes;
		result.entry_cop1_hot_bytes = entry.compiled.entry_cop1_hot_bytes;
		result.entry_branch_hot_bytes = entry.compiled.entry_branch_hot_bytes;
		result.block_hot_bytes = entry.compiled.block_hot_bytes;
		result.memory_hot_bytes = entry.compiled.memory_hot_bytes;
		result.memory_loads = entry.compiled.memory_loads;
		result.memory_stores = entry.compiled.memory_stores;
		result.block_host_loads = entry.compiled.block_host_loads;
		result.block_host_stores = entry.compiled.block_host_stores;
		result.cop1_normalize_emitted = entry.compiled.cop1_normalize_emitted;
		result.cop1_normalize_elided = entry.compiled.cop1_normalize_elided;
		result.cop1_normalize_hoisted = entry.compiled.cop1_normalize_hoisted;
		result.cop1_ou_pairs_fused = entry.compiled.cop1_ou_pairs_fused;
		result.cop1_destructive_ou_pairs =
			entry.compiled.cop1_destructive_ou_pairs;
		result.cop1_lazy_exception_guards =
			entry.compiled.cop1_lazy_exception_guards;
		result.cop1_lazy_exception_leaves =
			entry.compiled.cop1_lazy_exception_leaves;
		result.cop1_hot_bytes = entry.compiled.cop1_hot_bytes;
		result.cop1_normalize_hot_bytes =
			entry.compiled.cop1_normalize_hot_bytes;
		result.cop1_ou_hot_bytes = entry.compiled.cop1_ou_hot_bytes;
		result.vu0_normalize_hot_bytes = entry.compiled.vu0_normalize_hot_bytes;
		result.vu0_broadcast_hot_bytes = entry.compiled.vu0_broadcast_hot_bytes;
		result.vu0_arithmetic_hot_bytes = entry.compiled.vu0_arithmetic_hot_bytes;
		result.vu0_clamp_hot_bytes = entry.compiled.vu0_clamp_hot_bytes;
		result.vu0_mac_flag_hot_bytes = entry.compiled.vu0_mac_flag_hot_bytes;
		result.vu0_status_flag_hot_bytes = entry.compiled.vu0_status_flag_hot_bytes;
		result.vu0_merge_hot_bytes = entry.compiled.vu0_merge_hot_bytes;
		result.vu0_direct_vfp_lanes = entry.compiled.vu0_direct_vfp_lanes;
		result.vu0_spilled_quad_arithmetic =
			entry.compiled.vu0_spilled_quad_arithmetic;
		result.vu0_fused_madd_lanes = entry.compiled.vu0_fused_madd_lanes;
		result.vu0_folded_broadcast_lane_mask =
			entry.compiled.vu0_folded_broadcast_lane_mask;
		result.vu0_folded_broadcast_source_s_mask =
			entry.compiled.vu0_folded_broadcast_source_s_mask;
		result.reused_materialized_branch_flags =
			entry.compiled.reused_materialized_branch_flags;
		result.elided_load_high_words =
			entry.compiled.elided_load_high_words;
		result.entry_state_words = entry.compiled.entry_state_word_count;
		result.output_state_words = entry.compiled.output_state_word_count;
		result.compact_exit_descriptors = static_cast<u32>(
			entry.compiled.persistent_cold_exit_descriptors.size());
		result.compact_exit_words = static_cast<u32>(
			entry.compiled.persistent_cold_exit_words.size());
		result.compact_snapshot_bytes =
			entry.compiled.persistent_cold_snapshot_bytes;
		result.core_peak_words = entry.compiled.allocation_core_peak_words;
		result.vfp_peak_s = entry.compiled.allocation_vfp_peak_s;
		result.neon_peak_q = entry.compiled.allocation_neon_peak_q;
		result.spilled_values = entry.compiled.allocation_spilled_values;
		result.spill_bytes = entry.compiled.allocation_spill_bytes;
		result.spilled_core_values =
			entry.compiled.allocation_spilled_core_values;
		result.spilled_vfp_values =
			entry.compiled.allocation_spilled_vfp_values;
		result.spilled_neon_values =
			entry.compiled.allocation_spilled_neon_values;
		result.edge_moves = entry.compiled.allocation_edge_moves;
		result.coalesced_edge_values =
			entry.compiled.allocation_coalesced_edge_values;
		result.edge_core_moves = entry.compiled.allocation_edge_core_moves;
		result.edge_vfp_moves = entry.compiled.allocation_edge_vfp_moves;
		result.edge_neon_moves = entry.compiled.allocation_edge_neon_moves;
		result.edge_spill_moves = entry.compiled.allocation_edge_spill_moves;
		result.edge_spill_to_spill_moves =
			entry.compiled.allocation_edge_spill_to_spill_moves;
		result.edge_spill_to_register_moves =
			entry.compiled.allocation_edge_spill_to_register_moves;
		result.edge_register_to_spill_moves =
			entry.compiled.allocation_edge_register_to_spill_moves;
		result.edge_cross_kind_moves =
			entry.compiled.allocation_edge_cross_kind_moves;
		result.edge_gpr_words = entry.compiled.allocation_edge_gpr_words;
		result.edge_fpr_words = entry.compiled.allocation_edge_fpr_words;
		result.edge_vu0_vector_words =
			entry.compiled.allocation_edge_vu0_vector_words;
		result.edge_other_state_words =
			entry.compiled.allocation_edge_other_state_words;
		result.edge_state_class_words =
			entry.compiled.allocation_edge_state_class_words;
		result.edge_parameter_sources =
			entry.compiled.allocation_edge_parameter_sources;
		result.edge_computed_sources =
			entry.compiled.allocation_edge_computed_sources;
		result.edge_call_moves = entry.compiled.allocation_edge_call_moves;
		result.edge_return_moves = entry.compiled.allocation_edge_return_moves;
		result.edge_backedge_moves = entry.compiled.allocation_edge_backedge_moves;
		result.edge_other_moves = entry.compiled.allocation_edge_other_moves;
		result.coalesced_spill_edge_values =
			entry.compiled.allocation_coalesced_spill_edge_values;
		result.residual_spill_shape_mismatches =
			entry.compiled.allocation_residual_spill_shape_mismatches;
		result.residual_spill_missing_groups =
			entry.compiled.allocation_residual_spill_missing_groups;
		result.residual_spill_same_groups =
			entry.compiled.allocation_residual_spill_same_groups;
		result.residual_spill_group_interferences =
			entry.compiled.allocation_residual_spill_group_interferences;
		result.residual_spill_unexplained =
			entry.compiled.allocation_residual_spill_unexplained;
		result.preflight_accesses = entry.compiled.memory_preflight_accesses;
		result.preflight_store_accesses =
			entry.compiled.memory_preflight_store_accesses;
		result.memory_plan_reducibility_rejection =
			entry.compiled.memory_plan_reducibility_rejection;
		result.indexed_scalar_memory_accesses =
			entry.compiled.indexed_scalar_memory_accesses;
		result.hoisted_memory_bases = entry.compiled.hoisted_memory_bases;
		result.hoisted_memory_accesses = entry.compiled.hoisted_memory_accesses;
		result.hoisted_memory_loads = entry.compiled.hoisted_memory_loads;
		result.forwarded_memory_loads = entry.compiled.forwarded_memory_loads;
		result.aggregated_cycle_edges = entry.compiled.aggregated_cycle_edges;
		result.aggregate_cycle_plan_status = static_cast<u8>(
			entry.compiled.aggregate_cycle_plan_status);
		result.conditional_layout_fallthroughs =
			entry.compiled.conditional_layout_fallthroughs;
		result.preflight_stride = entry.compiled.entry_range_stride_bytes;
		result.minimum_profitable_iterations =
			entry.compiled.minimum_profitable_iterations;
		result.pre_entry_profitability_leaves =
			entry.compiled.pre_entry_profitability_leaves;
		result.pre_entry_state_leaves = entry.compiled.pre_entry_state_leaves;
		result.entry_low32_guards = entry.compiled.entry_low32_guards;
		return result;
	}

	void Runtime::InsertEntrySnapshot(Statistics* statistics,
		const Statistics::EntrySnapshot& snapshot)
	{
		if (!statistics ||
			(snapshot.executions == 0 && snapshot.profitability_fallbacks == 0 &&
				snapshot.entry_state_fallbacks == 0))
			return;
		auto activity = [](const Statistics::EntrySnapshot& entry) {
			return entry.executions + entry.profitability_fallbacks +
				entry.entry_state_fallbacks;
		};
		auto& entries = statistics->entry_snapshot;
		for (Statistics::EntrySnapshot& existing : entries)
		{
			if (existing.executions != 0 && existing.pc == snapshot.pc)
			{
				const u64 combined_executions =
					existing.executions + snapshot.executions;
				const u64 combined_counted_iterations =
					existing.counted_iterations + snapshot.counted_iterations;
				const u64 combined_fallbacks =
					existing.profitability_fallbacks +
					snapshot.profitability_fallbacks;
				const u64 combined_state_fallbacks =
					existing.entry_state_fallbacks +
					snapshot.entry_state_fallbacks;
				existing = snapshot;
				existing.executions = combined_executions;
				existing.counted_iterations = combined_counted_iterations;
				existing.profitability_fallbacks = combined_fallbacks;
				existing.entry_state_fallbacks = combined_state_fallbacks;
				std::sort(entries.begin(), entries.end(),
					[&](const Statistics::EntrySnapshot& left,
						const Statistics::EntrySnapshot& right) {
						return activity(left) != activity(right) ?
							activity(left) > activity(right) : left.pc < right.pc;
					});
				return;
			}
		}
		if (activity(entries.back()) == 0 ||
			activity(snapshot) > activity(entries.back()))
		{
			entries.back() = snapshot;
			std::sort(entries.begin(), entries.end(),
				[&](const Statistics::EntrySnapshot& left,
					const Statistics::EntrySnapshot& right) {
					return activity(left) != activity(right) ?
						activity(left) > activity(right) : left.pc < right.pc;
				});
		}
	}

	void Runtime::AccumulateEntryShape(Statistics* statistics,
		const Entry& entry, u64 executions)
	{
		if (!statistics || !entry.active)
			return;
		const Statistics::EntrySnapshot snapshot =
			MakeEntrySnapshot(entry, executions);
		if (executions == 0 && snapshot.profitability_fallbacks == 0 &&
			snapshot.entry_state_fallbacks == 0)
			return;
		if (entry.block_count <= 1)
			statistics->one_block_executions += executions;
		else
			statistics->multi_block_executions += executions;
		if (entry.compiled.memory_preflight_ranges != 0)
			statistics->preflight_executions += executions;
		if (entry.compiled.memory_preflight_store_accesses != 0)
			statistics->preflight_store_executions += executions;
		if (entry.compiled.allocation_spilled_values != 0)
			statistics->spill_executions += executions;
		statistics->weighted_host_instructions += executions *
			entry.compiled.host_instructions;
		statistics->weighted_hot_code_bytes += executions *
			entry.compiled.hot_code_bytes;
		statistics->weighted_entry_state_words += executions *
			entry.compiled.entry_state_word_count;
		statistics->weighted_output_state_words += executions *
			entry.compiled.output_state_word_count;
		if (entry.compiled.semantic_kernel_kind != 0 &&
			entry.compiled.semantic_kernel_kind <
				statistics->semantic_kernel_executions_by_kind.size())
		{
			statistics->semantic_kernel_executions += executions;
			statistics->semantic_kernel_iterations += snapshot.counted_iterations;
			statistics->semantic_kernel_bytes +=
				static_cast<u64>(snapshot.counted_iterations) *
				entry.compiled.semantic_kernel_bytes_per_iteration;
			statistics->semantic_kernel_executions_by_kind[
				entry.compiled.semantic_kernel_kind] += executions;
		}
		InsertEntrySnapshot(statistics, snapshot);
	}

	Statistics Runtime::GetStatistics() const
	{
		Statistics result = m_statistics;
		std::sort(result.repeated_candidate_snapshot.begin(),
			result.repeated_candidate_snapshot.end(), [](const auto& left,
				const auto& right) {
				return left.last_serial > right.last_serial;
			});
		result.one_block_executions =
			m_profile_window_shapes.one_block_executions;
		result.multi_block_executions =
			m_profile_window_shapes.multi_block_executions;
		result.preflight_executions =
			m_profile_window_shapes.preflight_executions;
		result.preflight_store_executions =
			m_profile_window_shapes.preflight_store_executions;
		result.spill_executions = m_profile_window_shapes.spill_executions;
		result.weighted_host_instructions =
			m_profile_window_shapes.weighted_host_instructions;
		result.weighted_hot_code_bytes =
			m_profile_window_shapes.weighted_hot_code_bytes;
		result.weighted_entry_state_words =
			m_profile_window_shapes.weighted_entry_state_words;
		result.weighted_output_state_words =
			m_profile_window_shapes.weighted_output_state_words;
		result.semantic_kernel_executions =
			m_profile_window_shapes.semantic_kernel_executions;
		result.semantic_kernel_iterations =
			m_profile_window_shapes.semantic_kernel_iterations;
		result.semantic_kernel_bytes =
			m_profile_window_shapes.semantic_kernel_bytes;
		result.semantic_kernel_executions_by_kind =
			m_profile_window_shapes.semantic_kernel_executions_by_kind;
		result.entry_snapshot = m_profile_window_shapes.entry_snapshot;
		for (size_t index = 0; index < m_entries.size(); index++)
		{
			const Entry& entry = m_entries[index];
			const u64 executions = EntryProfileWindowExecutions(entry);
			result.executions += static_cast<u32>(
				entry.generated_dispatch_executions -
				entry.reported_generated_dispatch_executions);
			result.continuation_failures += static_cast<u32>(
				entry.generated_continuation_failures -
				entry.reported_generated_continuation_failures);
			result.profitability_fallbacks += static_cast<u32>(
				entry.generated_profitability_fallbacks -
				entry.reported_generated_profitability_fallbacks);
			result.entry_state_fallbacks += static_cast<u32>(
				entry.generated_entry_state_fallbacks -
				entry.reported_generated_entry_state_fallbacks);
			result.active_regions += entry.active ? 1u : 0u;
			if (entry.active)
				AccumulateEntryShape(&result, entry, executions);
		}
		struct RankedProbe
		{
			Statistics::ProbeSnapshot snapshot{};
			u64 serial = 0;
			bool live = false;
		};
		std::array<RankedProbe,
			Statistics::CANDIDATE_SNAPSHOT_CAPACITY * 2> ranked_probes{};
		size_t ranked_probe_count = 0;
		const auto ranks_before = [](const RankedProbe& left,
			const RankedProbe& right) {
			return left.snapshot.maximum_observations !=
					right.snapshot.maximum_observations ?
				left.snapshot.maximum_observations >
					right.snapshot.maximum_observations :
				left.snapshot.observations != right.snapshot.observations ?
					left.snapshot.observations > right.snapshot.observations :
				left.snapshot.samples != right.snapshot.samples ?
					left.snapshot.samples > right.snapshot.samples :
				left.serial > right.serial;
		};
		const auto insert_probe = [&](RankedProbe incoming) {
			for (size_t index = 0; index < ranked_probe_count; index++)
			{
				RankedProbe& existing = ranked_probes[index];
				if (existing.snapshot.entry_pc != incoming.snapshot.entry_pc ||
					existing.snapshot.source_end_pc !=
						incoming.snapshot.source_end_pc)
				{
					continue;
				}
				const u32 retained_maximum = std::max(
					existing.snapshot.maximum_observations,
					incoming.snapshot.maximum_observations);
				const u64 retained_serial = std::max(
					existing.serial, incoming.serial);
				if (incoming.live || !existing.live)
					existing = incoming;
				existing.snapshot.maximum_observations = retained_maximum;
				existing.serial = retained_serial;
				existing.live |= incoming.live;
				return;
			}
			if (ranked_probe_count < ranked_probes.size())
			{
				ranked_probes[ranked_probe_count++] = incoming;
				return;
			}
			size_t worst = 0;
			for (size_t index = 1; index < ranked_probes.size(); index++)
				if (ranks_before(ranked_probes[worst], ranked_probes[index]))
					worst = index;
			if (ranks_before(incoming, ranked_probes[worst]))
				ranked_probes[worst] = incoming;
		};
		for (const ProbePeak& peak : m_probe_peaks)
		{
			if (peak.valid)
				insert_probe({peak.snapshot, peak.serial, false});
		}
		for (const ProbeCandidate& probe : m_probes)
		{
			if (!probe.valid)
				continue;
			Statistics::ProbeSnapshot snapshot{};
			snapshot.entry_pc = probe.candidate.entry_pc;
			snapshot.backedge_pc = probe.candidate.backedge_block_pc;
			snapshot.source_end_pc = probe.candidate.source_end_pc;
			snapshot.observations = probe.observations;
			snapshot.maximum_observations = probe.maximum_observations;
			snapshot.samples = probe.samples;
			snapshot.required_observations = probe.required_observations != 0 ?
				probe.required_observations :
				(probe.has_iteration_guard ? 1u : m_minimum_backedge_observations);
			snapshot.internal_source_blocks = probe.internal_source_block_count;
			snapshot.guarded = probe.has_iteration_guard;
			snapshot.event_scoped = probe.event_scoped_observations;
			snapshot.armed = probe.armed;
			snapshot.dormant = probe.dormant;
			insert_probe({snapshot, probe.last_observation_serial, true});
		}
		std::sort(ranked_probes.begin(),
			ranked_probes.begin() + ranked_probe_count, ranks_before);
		result.probe_snapshot_count = static_cast<u32>(std::min<size_t>(
			ranked_probe_count, result.probe_snapshot.size()));
		for (u32 index = 0; index < result.probe_snapshot_count; index++)
		{
			result.probe_snapshot[index] = ranked_probes[index].snapshot;
			result.probe_snapshot[index].live = ranked_probes[index].live;
		}

		std::array<const DeferredCandidate*, Statistics::CANDIDATE_SNAPSHOT_CAPACITY>
			deferred_snapshot{};
		for (const DeferredCandidate& deferred : m_deferred_candidates)
		{
			if (!deferred.valid)
				continue;
			result.deferred_candidates++;
			for (size_t rank = 0; rank < deferred_snapshot.size(); rank++)
			{
				const DeferredCandidate* const incumbent = deferred_snapshot[rank];
				if (incumbent &&
					incumbent->progress_serial >= deferred.progress_serial)
				{
					continue;
				}
				for (size_t shift = deferred_snapshot.size() - 1;
					shift > rank; shift--)
				{
					deferred_snapshot[shift] = deferred_snapshot[shift - 1];
				}
				deferred_snapshot[rank] = &deferred;
				break;
			}
		}
		for (const DeferredCandidate* const deferred : deferred_snapshot)
		{
			if (!deferred)
				break;
			Statistics::DeferredSnapshot& snapshot =
				result.deferred_snapshot[result.deferred_snapshot_count++];
			snapshot.entry_pc = deferred->candidate.entry_pc;
			snapshot.backedge_pc = deferred->candidate.backedge_block_pc;
			snapshot.source_end_pc = deferred->candidate.source_end_pc;
			snapshot.missing_contract_pc = deferred->missing_contract_pc;
			snapshot.admission_capacity = deferred->reason ==
				DeferredCandidate::Reason::AdmissionCapacity;
			snapshot.classification_prepared =
				deferred->classification_prepared;
		}
		result.active_probes = UseBarrierProbesForValidation() ?
			m_armed_probe_count :
			static_cast<u32>(std::count_if(m_probes.begin(), m_probes.end(),
				[](const ProbeCandidate& probe) { return probe.valid; }));
		result.armed_probes = m_armed_probe_count;
		result.pending_candidate = m_pending.valid ? 1u : 0u;
		result.publication_dirty = m_publication_dirty ? 1u : 0u;
		result.maintenance_requested =
			m_generated_maintenance_requested != 0 ? 1u : 0u;
		result.compile_budget_tokens = AvailableBuildTokens();
		result.compile_budget_wait_cycles = BuildBudgetWaitCycles();
		return result;
	}
} // namespace VitaEE::RegionRuntime
