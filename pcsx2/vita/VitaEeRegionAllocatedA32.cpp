// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#include "PrecompiledHeader.h"

#include "pcsx2/vita/A32Emitter.h"
#include "pcsx2/R5900.h"
#include "pcsx2/vita/VitaEeRegionA32.h"
#include "pcsx2/vita/VitaEeRegionAllocation.h"
#include "pcsx2/vita/VitaEeRegionExecutionPlan.h"
#include "pcsx2/vita/VitaEeRegionMemory.h"
#include "pcsx2/vita/VitaEeSemanticKernel.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstring>
#include <vector>

namespace VitaEE::RegionA32
{
	using namespace RegionIR;
	using VitaA32::Condition;
	using VitaA32::ShiftType;

	namespace
	{
		const void* MaterializePersistentColdExit(
			const PersistentColdExitDescriptor* descriptor, const u8* frame,
			u8* cpu_registers, u8* vu0) noexcept
		{
			if (!descriptor ||
				(!descriptor->words && descriptor->word_count != 0) ||
				!descriptor->branch_tail ||
				!frame || !cpu_registers || !vu0)
			{
				return nullptr;
			}

			for (u16 index = 0; index < descriptor->word_count; index++)
			{
				const PersistentColdWord& word = descriptor->words[index];
				u32 value = 0;
				switch (word.source)
				{
					case PersistentColdWordSource::Frame:
					case PersistentColdWordSource::SignExtendFrame:
						std::memcpy(&value, frame + word.source_offset,
							sizeof(value));
						break;
					case PersistentColdWordSource::CpuRegisters:
					case PersistentColdWordSource::SignExtendCpuRegisters:
						std::memcpy(&value, cpu_registers + word.source_offset,
							sizeof(value));
						break;
					case PersistentColdWordSource::Vu0:
					case PersistentColdWordSource::SignExtendVu0:
						std::memcpy(&value, vu0 + word.source_offset, sizeof(value));
						break;
					case PersistentColdWordSource::Immediate:
						value = word.immediate;
						break;
				}
				if (word.source == PersistentColdWordSource::SignExtendFrame ||
					word.source == PersistentColdWordSource::SignExtendCpuRegisters ||
					word.source == PersistentColdWordSource::SignExtendVu0)
				{
					value = static_cast<u32>(static_cast<s32>(value) >> 31);
				}
				u8* const destination =
					word.destination_base == CompileOptions::PersistentStateBase::Vu0 ?
						vu0 : cpu_registers;
				std::memcpy(destination + word.destination_offset, &value,
					sizeof(value));
			}
			return descriptor->branch_tail;
		}

		constexpr unsigned TEMP0 = 0;
		constexpr unsigned CYCLE_LOW = 1;
		constexpr unsigned CYCLE_HIGH = 2;
		constexpr unsigned FIRST_ALLOCATED_CORE = 3;
		constexpr unsigned CPU_REGS = 4;
		constexpr unsigned VTLB_VMAP = 7;
		constexpr unsigned VTLB_HOST_BASE = 8;
		constexpr unsigned CONTEXT = 11;
		constexpr unsigned TEMP1 = 12;
		constexpr unsigned STACK = 13;
		constexpr unsigned TEMP2 = 14;
		// s16-s29 do not alias the region's logical NEON bank. s30-s31 are
		// reserved for scalar lowering when an operand or result is spilled.
		constexpr unsigned VFP_SCRATCH0 = 30;
		constexpr unsigned VFP_SCRATCH1 = 31;
		constexpr u32 COP1_SIGN = 0x80000000u;
		constexpr u32 COP1_EXPONENT = 0x7f800000u;
		constexpr u32 COP1_CVT_W_MAX_EXPONENT = 0x4e800000u;
		constexpr u32 FCR31_O = 0x00008000u;
		constexpr u32 FCR31_U = 0x00004000u;
		constexpr u32 FCR31_SO = 0x00000010u;
		constexpr u32 FCR31_SU = 0x00000008u;
		constexpr u32 FCR31_C = 0x00800000u;
		// q0-q5 are always region allocated. A region which proves that it owns no
		// scalar VFP values may also allocate q6. Physical q7 remains reserved
		// because its s28-s31 lanes are the scalar scratch bank. Physical q14-q15 remain
		// caller-saved AAPCS scratch for spill/immediate vector operands. Naming
		// them directly keeps scratch independent of the conditional q4-q5 remap.
		constexpr unsigned VECTOR_SCRATCH0 = 14;
		constexpr unsigned VECTOR_SCRATCH1 = 15;
		// Physical q8-q11 are outside both the region allocator and scalar VFP bank.
		// VU0 regions keep their normalization constants in q8-q10 for the whole
		// generated invocation and use q11 as transient predicate storage.
		constexpr unsigned VU0_EXPONENT_Q = 8;
		constexpr unsigned VU0_SIGN_Q = 9;
		constexpr unsigned VU0_MAX_FINITE_Q = 10;
		constexpr unsigned VU0_MASK_Q = 11;
		// q12 is free only in a vector-only allocation.  In that mode it holds the
		// lane shifts which pack xyzw sign predicates into MAC flag bits 7..4.
		constexpr unsigned VU0_MAC_SIGN_SHIFTS_Q = 12;
		constexpr unsigned VU0_AFFINE_STICKY_STATUS_Q = 12;
		constexpr unsigned VU0_AFFINE_CURRENT_STATUS_Q = 13;
		constexpr u32 VU_FLOAT_SIGN = 0x80000000u;
		constexpr u32 VU_FLOAT_EXPONENT = 0x7f800000u;
		constexpr u32 VU_FLOAT_MAX_FINITE = 0x7f7fffffu;
		constexpr u8 BASE_ALLOCATED_NEON_Q_REGISTERS = 6;
		constexpr u8 ALLOCATED_NEON_Q_REGISTERS = 7;
		// The affine VU0 semantic body borrows four otherwise unallocated physical
		// Q registers and four persistent-dispatch core registers for the complete
		// batch.  Preserve their allocator-owned entry values once, then stage the
		// three architectural vector results before restoring the private ABI.
		// Nothing in this area is touched per guest scalar operation.
		constexpr u32 VU0_AFFINE_FLAGS_OFFSET = 0;
		constexpr u32 VU0_AFFINE_STATUS_OFFSET = 4;
		constexpr u32 SEMANTIC_TRIP_COUNT_OFFSET = 8;
		constexpr u32 VU0_AFFINE_VI_STATUS_OFFSET = 12;
		constexpr u32 VU0_AFFINE_CORE_SAVE_OFFSET = 16;
		constexpr u32 VU0_AFFINE_Q_SAVE_OFFSET = 32;
		constexpr u32 VU0_AFFINE_FINAL_INPUT_OFFSET = 96;
		constexpr u32 VU0_AFFINE_FINAL_OUTPUT_OFFSET = 112;
		constexpr u32 VU0_AFFINE_FINAL_ACC_OFFSET = 128;
		constexpr u32 VU0_AFFINE_WORK_SCRATCH_BYTES = 144;
		// A bounded-search island borrows five persistent allocator words only for
		// the tight native loop.  The architectural header values are restored before
		// either ordinary suffix is entered.  Offsets 0..15 remain the shared range
		// proof/trip-count area used by EmitEntryMemoryPlanGuard().
		constexpr u32 SEARCH_LOADED_LOW_OFFSET = 0;
		constexpr u32 SEARCH_LOADED_HIGH_OFFSET = 4;
		constexpr u32 SEARCH_TRIP_COUNT_OFFSET = SEMANTIC_TRIP_COUNT_OFFSET;
		constexpr u32 SEARCH_PROGRESS_OFFSET = 12;
		constexpr u32 SEARCH_CORE_SAVE_OFFSET = 16;
		constexpr u32 SEARCH_POINTER_LOW_OFFSET = 36;
		constexpr u32 SEARCH_POINTER_HIGH_OFFSET = 40;
		constexpr u32 SEARCH_KEY_LOW_OFFSET = 44;
		constexpr u32 SEARCH_KEY_HIGH_OFFSET = 48;
		constexpr u32 SEARCH_ARITHMETIC_TEMP_OFFSET = 52;
		constexpr u32 SEARCH_OUTPUT_OFFSET = 64;
		// AAPCS32 makes q8-q15 caller-clobbered. q13 (d26-d27) is also outside
		// the project's directly allocated q0-q6 bank; the search selector below
		// additionally requires zero allocated NEON and scalar-VFP pressure before
		// borrowing it. r14 is the persistent frame's saved LR and is otherwise a
		// backend scratch register; generated regions never allocate architectural
		// state there. Neither value crosses a call while live in the search body.
		constexpr unsigned SEARCH_VECTOR_ACCUM_Q = 13;
		constexpr unsigned SEARCH_VECTOR_POINTER = TEMP2;
		static_assert(SEARCH_VECTOR_ACCUM_Q >= 8 &&
			SEARCH_VECTOR_ACCUM_Q != VECTOR_SCRATCH0 &&
			SEARCH_VECTOR_ACCUM_Q != VECTOR_SCRATCH1);
		constexpr std::array<unsigned, 5> SEARCH_BORROWED_CORE = {{
			3, 5, 6, 9, 10,
		}};
		constexpr std::array<unsigned, 4> VU0_AFFINE_BORROWED_CORE = {{
			3, 5, 6, 9,
		}};
		constexpr u8 SOURCE_PAGE_SHIFT = 12;
		constexpr u8 SOURCE_CHUNK_SHIFT = 6;
		constexpr u16 SAVED_REGISTERS =
			static_cast<u16>(((1u << 12) - (1u << 3)) | (1u << 14));
		constexpr u16 RESTORED_REGISTERS =
			static_cast<u16>(((1u << 12) - (1u << 3)) | (1u << 15));
		constexpr std::array<unsigned, 7> PERSISTENT_ALLOCATED_CORE = {{
			2, 3, 5, 6, 9, 10, 11,
		}};
		constexpr std::array<unsigned, 8> PERSISTENT_IDENTITY_ALLOCATED_CORE = {{
			2, 3, 5, 6, 7, 9, 10, 11,
		}};
		// A region with no memory effects cannot observe either half of the
		// persistent vTLB ABI. Borrow r7/r8 for the complete generated body and
		// restore both only when leaving it. This is two more resident scalar words
		// without changing the first-class dispatcher entry contract.
		constexpr std::array<unsigned, 9> PERSISTENT_MEMORY_FREE_ALLOCATED_CORE = {{
			2, 3, 5, 6, 7, 8, 9, 10, 11,
		}};
		static_assert(std::find(PERSISTENT_ALLOCATED_CORE.begin(),
			PERSISTENT_ALLOCATED_CORE.end(), SEARCH_VECTOR_POINTER) ==
				PERSISTENT_ALLOCATED_CORE.end());
		static_assert(std::find(PERSISTENT_IDENTITY_ALLOCATED_CORE.begin(),
			PERSISTENT_IDENTITY_ALLOCATED_CORE.end(), SEARCH_VECTOR_POINTER) ==
				PERSISTENT_IDENTITY_ALLOCATED_CORE.end());
		static_assert(std::find(PERSISTENT_MEMORY_FREE_ALLOCATED_CORE.begin(),
			PERSISTENT_MEMORY_FREE_ALLOCATED_CORE.end(), SEARCH_VECTOR_POINTER) ==
				PERSISTENT_MEMORY_FREE_ALLOCATED_CORE.end());
		constexpr std::array<unsigned, 10> PERSISTENT_COLD_CORE_REGISTERS = {{
			1, 2, 3, 5, 6, 7, 8, 9, 10, 11,
		}};
		constexpr u32 PERSISTENT_COLD_VFP_LOW_S_COUNT = 32;
		constexpr u32 PERSISTENT_COLD_VFP_HIGH_S_FIRST = 48;
		constexpr u32 PERSISTENT_COLD_VFP_HIGH_S_COUNT = 8;
		// Below this aggregate, direct exact stores are both smaller and faster. The
		// threshold is expressed in emitted state words, not guest shape or opcode,
		// and therefore cannot become an admission allow-list.
		constexpr u32 MIN_COMPACT_PERSISTENT_EXIT_WORDS = 128;
		constexpr size_t ResultOffset(size_t member)
		{
			return offsetof(ExecutionContext, result) + member;
		}

		u32 AlignUp(u32 value, u32 alignment)
		{
			return (value + alignment - 1) & ~(alignment - 1);
		}

		u8 WordCount(u8 mask)
		{
			u8 count = 0;
			for (u8 word = 0; word < 4; word++)
				count += (mask >> word) & 1u;
			return count;
		}

		u8 LocationMask(const RegionAllocation::Location& location)
		{
			return location.word_mask != 0 ? location.word_mask :
				(location.words == 0 ? 0 :
				 static_cast<u8>((1u << std::min<u8>(location.words, 4)) - 1u));
		}

		u8 WordRank(u8 mask, u8 word)
		{
			return WordCount(mask & static_cast<u8>((1u << word) - 1u));
		}

		bool IsStateBinding(Opcode opcode)
		{
			switch (opcode)
			{
				case Opcode::BindGpr:
				case Opcode::BindHi:
				case Opcode::BindLo:
				case Opcode::BindSa:
				case Opcode::BindFpr:
				case Opcode::BindVu0Vf:
				case Opcode::BindVu0Acc:
				case Opcode::BindVu0MacFlag:
				case Opcode::BindVu0StatusFlag:
				case Opcode::BindVu0ViMac:
				case Opcode::BindVu0ViStatus:
				case Opcode::BindVu0Q:
				case Opcode::BindVu0ViQ:
				case Opcode::BindVu0Vi:
				case Opcode::BindVu0ClipFlag:
				case Opcode::BindVu0MicroStatusFlag:
				case Opcode::BindFcr31:
				case Opcode::BindAcc:
					return true;
				default:
					return false;
			}
		}

		struct PhysicalWord
		{
			enum class Kind : u8
			{
				None,
				Core,
				Spill,
				VfpS,
				NeonLane,
			} kind = Kind::None;
			u32 index = 0;

			bool operator==(const PhysicalWord& other) const
			{
				return kind == other.kind && index == other.index &&
				       kind != Kind::None;
			}
		};

		struct InternalPatch
		{
			size_t offset = static_cast<size_t>(-1);
			u32 target_block = INVALID_BLOCK;
			Condition condition = Condition::AL;
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
			std::vector<AliasPatch> aliases;
			ExitReason reason = ExitReason::RegionBoundary;
			const RegionExecution::ExitSite* site = nullptr;
			ValueId memory_address = INVALID_VALUE;
			// Exact scaled cycle charge moved out of a conditional hot side exit.
			// Only the selected cold leaf applies it; the internal layout successor
			// continues with the verifier-derived aggregate debt unchanged.
			u32 deferred_cycle_advance = 0;
			bool mask_quad_address = false;
		};

		struct ColdExitEpilogue
		{
			const RegionExecution::ExitSite* site = nullptr;
			std::vector<size_t> branch_offsets;
		};

		struct CompactPersistentExit
		{
			size_t hub_branch = static_cast<size_t>(-1);
			PersistentExitPatch patch{};
		};

		struct Vu0StatusColdPath
		{
			size_t exceptional_branch = static_cast<size_t>(-1);
			size_t hot_continuation = static_cast<size_t>(-1);
			ValueId raw = INVALID_VALUE;
			ValueId output = INVALID_VALUE;
			ValueId deferred_clamp = INVALID_VALUE;
			ValueId fused_sync = INVALID_VALUE;
		};

		struct Vu0MacColdPath
		{
			size_t exceptional_branch = static_cast<size_t>(-1);
			size_t hot_continuation = static_cast<size_t>(-1);
			ValueId raw = INVALID_VALUE;
			ValueId output = INVALID_VALUE;
			u8 mask = 0;
			ValueId deferred_clamp = INVALID_VALUE;
			ValueId fused_status = INVALID_VALUE;
		};

		struct Cop1GuardedColdPath
		{
			size_t exceptional_branch = static_cast<size_t>(-1);
			size_t hot_continuation = static_cast<size_t>(-1);
			ValueId raw = INVALID_VALUE;
			ValueId clamp = INVALID_VALUE;
			ValueId old_flags = INVALID_VALUE;
			ValueId flags = INVALID_VALUE;
		};

		struct PreEntryFallback
		{
			bool compatible = false;
			ExitReason reason = ExitReason::ProfitabilityFallback;
			std::vector<ColdExit::AliasPatch> branches;
		};

		struct LiteralPatch
		{
			size_t instruction_offset = static_cast<size_t>(-1);
			u32 value = 0;
		};

		struct WordMapping
		{
			u8 target_word = 0;
			ValueId source = INVALID_VALUE;
			u8 source_word = 0;
		};

		struct HoistedMemoryAccess
		{
			ValueId operation = INVALID_VALUE;
			u16 offset = 0;
		};

		struct HoistedMemoryBase
		{
			bool absolute = false;
			u8 base_gpr = 0;
			s32 guest_offset = 0;
			u32 score = 0;
			unsigned host = 0;
			std::vector<HoistedMemoryAccess> accesses;
		};

		struct HoistedMemoryLoad
		{
			ValueId operation = INVALID_VALUE;
			ValueId value = INVALID_VALUE;
		};

		enum class BinaryKind : u8
		{
			Add,
			Subtract,
			And,
			Or,
			Xor,
			Nor,
		};

		class AllocatedCompiler
		{
		public:
			AllocatedCompiler(const Program& program, VitaA32::CodeBuffer& code,
				const CompileOptions& options)
				: m_program(program)
				, m_code(code)
				, m_options(options)
			{
			}

			CompileResult Run()
			{
				m_code.Reset();
				RegionExecution::BuildResult execution =
					RegionExecution::Build(m_program);
				if (!execution)
					return Fail(CompileFailure::InvalidProgram, BlockPc(execution.block));
				m_execution = std::move(execution.plan);
				m_pre_entry_vu0_idle_proof = Persistent() &&
					std::any_of(m_program.blocks.begin(), m_program.blocks.end(),
						[](const Block& block) {
							return std::any_of(block.nodes.begin(), block.nodes.end(),
								[](const Node& node) {
									return node.opcode == Opcode::Vu0RequireIdle;
								});
						});
				if (m_pre_entry_vu0_idle_proof)
				{
					// The one entry proof dominates every represented VU0 observer and
					// VPU_STAT cannot change inside this semantic surface. Remove those
					// unreachable exits before liveness/allocation; otherwise intermediate
					// MAC/STATUS values are kept live solely to materialize cold paths which
					// generated code can never select.
					m_execution.exits.erase(std::remove_if(m_execution.exits.begin(),
						m_execution.exits.end(), [](const RegionExecution::ExitSite& site) {
							return site.kind ==
								RegionExecution::ExitSiteKind::Observer;
						}), m_execution.exits.end());
				}
				const RegionMemoryPlan::BuildResult memory_plan =
					m_options.enable_counted_memory_preflight ?
						RegionMemoryPlan::Build(m_program) :
						RegionMemoryPlan::BuildResult{};
				if (memory_plan.failure ==
					RegionMemoryPlan::BuildFailure::InvalidProgram)
				{
					return Fail(CompileFailure::InvalidProgram,
						BlockPc(m_program.entry_block));
				}
				m_memory_plan = memory_plan.plan;
				m_result.memory_plan_reducibility_rejection =
					memory_plan.reducibility_rejection;
				// Base-free ranges are useful only under the persistent dispatcher's
				// complete low-RAM identity contract. They are compile-time address
				// proofs, so reject the optimization (not the region) if this target
				// cannot map guest offset directly through r8.
				const bool complete_persistent_identity = Persistent() &&
					m_options.persistent_dispatch->identity_main_ram_limit != 0 &&
					m_options.persistent_dispatch->identity_main_ram_limit ==
						m_options.persistent_dispatch->main_ram_limit;
				m_memory_plan.bounded_ranges.erase(std::remove_if(
					m_memory_plan.bounded_ranges.begin(),
					m_memory_plan.bounded_ranges.end(),
					[&](const RegionMemoryPlan::BoundedRange& range) {
						if (!range.absolute_address)
							return false;
						return !complete_persistent_identity || range.alignment == 0 ||
							range.minimum_offset < 0 ||
							range.maximum_offset_end <= range.minimum_offset ||
							static_cast<u32>(range.maximum_offset_end) >
								m_options.persistent_dispatch->main_ram_limit ||
							(static_cast<u32>(range.minimum_offset) &
								(range.alignment - 1)) != 0;
					}), m_memory_plan.bounded_ranges.end());
				// A complete range certificate in the default identity window never
				// consults the persistent dispatcher's r7 VTLB map. Borrow that register
				// only when every memory effect is covered; preserve the dispatcher ABI
				// once in the entry/exit frame. r8 remains the resident additive host base.
				m_reclaim_vtlb_vmap = Persistent() &&
					m_options.persistent_dispatch->vmap != nullptr &&
					m_options.persistent_dispatch->identity_main_ram_limit != 0 &&
					m_options.persistent_dispatch->identity_main_ram_limit ==
						m_options.persistent_dispatch->main_ram_limit &&
					(!m_memory_plan.counted_ranges.empty() ||
					 !m_memory_plan.bounded_ranges.empty());
				bool saw_memory = false;
				for (const Block& block : m_program.blocks)
				{
					for (const Node& node : block.nodes)
					{
						if (node.opcode != Opcode::MemoryLoad &&
							node.opcode != Opcode::MemoryStore)
						{
							continue;
						}
						saw_memory = true;
						bool covered = false;
						for (const RegionMemoryPlan::CountedRange& range :
							m_memory_plan.counted_ranges)
						{
							covered |= std::any_of(range.accesses.begin(),
								range.accesses.end(),
								[&](const RegionMemoryPlan::Access& access) {
									return access.operation == node.id;
								});
						}
						for (const RegionMemoryPlan::BoundedRange& range :
							m_memory_plan.bounded_ranges)
						{
							covered |= std::any_of(range.accesses.begin(),
								range.accesses.end(),
								[&](const RegionMemoryPlan::Access& access) {
									return access.operation == node.id;
								});
						}
						m_reclaim_vtlb_vmap &= covered;
					}
				}
				if (!saw_memory)
				{
					// Neither r7 nor r8 can be observed by a memory-free region. Do not
					// borrow them yet: first compare the ordinary seven-word allocation
					// against a nine-word plan, so spill-free regions pay no restore work.
					m_memory_free_vtlb_registers_available = Persistent() &&
						m_options.persistent_dispatch->vmap != nullptr &&
						m_options.persistent_dispatch->host_memory_base != nullptr;
					m_reclaim_vtlb_vmap = false;
					m_reclaim_vtlb_host_base = false;
				}
				m_definitions.assign(m_program.value_count, nullptr);
				for (const Block& block : m_program.blocks)
				{
					for (const Node& node : block.nodes)
					{
						if (node.id >= m_definitions.size() || m_definitions[node.id])
							return Fail(CompileFailure::InvalidProgram, block.pc);
						m_definitions[node.id] = &node;
					}
				}
				m_memory_value_for_effect.assign(m_program.value_count, INVALID_VALUE);
				for (const Block& block : m_program.blocks)
				{
					for (const Node& node : block.nodes)
					{
						if (node.opcode != Opcode::MemoryLoadValue)
							continue;
						if (node.operand_count != 1 ||
							node.operands[0] >= m_memory_value_for_effect.size() ||
							m_memory_value_for_effect[node.operands[0]] != INVALID_VALUE)
						{
							return Fail(CompileFailure::InvalidProgram, block.pc);
						}
						m_memory_value_for_effect[node.operands[0]] = node.id;
					}
				}
				if (m_options.enable_semantic_kernel_lowering &&
					Persistent())
				{
					const SemanticKernel::BuildResult semantic =
						SemanticKernel::Build(m_program);
					// Product publication is deliberately opt-in per measured semantic
					// class.  A newly added exact descriptor must not silently become a
					// native product lowering before its inclusive Cortex-A9 and Vita
					// gates have passed.
					const bool retained_class = semantic &&
						(semantic.plan.kind == SemanticKernel::Kind::PatternFill ||
							semantic.plan.kind == SemanticKernel::Kind::ForwardCopy);
					if (semantic &&
						m_options.allow_unretained_semantic_kernel_lowering &&
						SelectSemanticSearchIsland(semantic.plan))
					{
						m_semantic_plan = semantic.plan;
						m_emit_semantic_search_island = true;
						m_result.semantic_kernel_kind = static_cast<u8>(
							semantic.plan.kind);
						m_result.semantic_kernel_bytes_per_iteration =
							semantic.plan.bytes_per_iteration;
						// This exact subclass is deliberately validation-only until the
						// complete first-class owner crosses the physical-A9 8x gate.
						m_result.semantic_kernel_minimum_profitable_bytes = 0;
					}
					else if (semantic &&
						(retained_class ||
							m_options.allow_unretained_semantic_kernel_lowering) &&
						SelectSemanticKernel(semantic.plan))
					{
						m_semantic_plan = semantic.plan;
						m_emit_semantic_kernel = true;
						m_result.semantic_kernel_kind = static_cast<u8>(
							semantic.plan.kind);
						m_result.semantic_kernel_bytes_per_iteration =
							semantic.plan.bytes_per_iteration;
						m_result.semantic_kernel_minimum_profitable_bytes =
							CompileOptions::DEFAULT_MINIMUM_A9_SEMANTIC_KERNEL_BYTES;
					}
				}
				if (!m_emit_semantic_kernel && !m_emit_semantic_search_island)
					BuildHoistedMemoryBaseCandidates();
				BuildAggregateCyclePlan();
				RegionAllocation::Options allocation_options{};
				allocation_options.neon_q_registers =
					BASE_ALLOCATED_NEON_Q_REGISTERS;
				allocation_options.vu0_idle_entry_proven =
					m_pre_entry_vu0_idle_proof;
				std::vector<ValueId> preflighted_memory_operations;
				for (const RegionMemoryPlan::CountedRange& range :
					m_memory_plan.counted_ranges)
				{
					for (const RegionMemoryPlan::Access& access : range.accesses)
						preflighted_memory_operations.push_back(access.operation);
				}
				for (const RegionMemoryPlan::BoundedRange& range :
					m_memory_plan.bounded_ranges)
				{
					for (const RegionMemoryPlan::Access& access : range.accesses)
						preflighted_memory_operations.push_back(access.operation);
				}
				allocation_options.preflighted_memory_operations =
					&preflighted_memory_operations;
				if (Persistent())
				{
					allocation_options.fixed_cycle_word_mask = 0x1;
					allocation_options.enable_guarded_entry_low32 = true;
				}
				u8 complete_core_words = Persistent() ? static_cast<u8>(
					m_reclaim_vtlb_host_base ?
						PERSISTENT_MEMORY_FREE_ALLOCATED_CORE.size() :
					m_reclaim_vtlb_vmap ? PERSISTENT_IDENTITY_ALLOCATED_CORE.size() :
						PERSISTENT_ALLOCATED_CORE.size()) :
					allocation_options.core_register_words;
				allocation_options.core_register_words = complete_core_words;
				RegionAllocation::BuildResult baseline_allocation =
					RegionAllocation::Build(m_program, m_execution, allocation_options);
				if (!baseline_allocation)
				{
					m_result.failure_value = baseline_allocation.value;
					m_result.failure_emission_step = 100u +
						static_cast<u32>(baseline_allocation.failure);
					return Fail(baseline_allocation.failure ==
							RegionAllocation::BuildFailure::SpillCapacity ?
						CompileFailure::RegisterPressure : CompileFailure::InvalidProgram,
						BlockPc(baseline_allocation.block));
				}
				if (m_memory_free_vtlb_registers_available &&
					baseline_allocation.plan.spilled_values != 0)
				{
					RegionAllocation::Options expanded_options = allocation_options;
					expanded_options.core_register_words = static_cast<u8>(
						PERSISTENT_MEMORY_FREE_ALLOCATED_CORE.size());
					RegionAllocation::BuildResult expanded_allocation =
						RegionAllocation::Build(m_program, m_execution, expanded_options);
					if (expanded_allocation &&
						expanded_allocation.plan.spilled_values <
							baseline_allocation.plan.spilled_values)
					{
						// The two restored ABI words are cold per invocation; the removed
						// spill traffic was hot per loop iteration. Select this only from the
						// verified allocation plans, never from a title or source shape.
						m_reclaim_vtlb_vmap = true;
						m_reclaim_vtlb_host_base = true;
						complete_core_words = expanded_options.core_register_words;
						allocation_options = expanded_options;
						baseline_allocation = std::move(expanded_allocation);
					}
				}
				// Cortex-A9 exposes q0-q7 through scalar s0-s31 aliases. q7 must remain
				// the backend's s28-s31 scratch bank, but q6 is otherwise idle in a
				// vector-only region. Prove the absence of allocated scalar VFP state
				// with the conservative six-Q plan before widening the vector bank.
				// Mixed COP1/VU0 regions retain six logical Q registers and the existing
				// q4-q5 -> q12-q13 remap, so the two banks can never alias.
				if (baseline_allocation.plan.vfp_peak_s == 0)
				{
					RegionAllocation::Options widened_options = allocation_options;
					widened_options.neon_q_registers =
						ALLOCATED_NEON_Q_REGISTERS;
					widened_options.scalar_addressable_neon_q_registers = 7;
					RegionAllocation::BuildResult widened_allocation =
						RegionAllocation::Build(m_program, m_execution, widened_options);
					if (widened_allocation &&
						widened_allocation.plan.vfp_peak_s == 0 &&
						widened_allocation.plan.spilled_values <=
							baseline_allocation.plan.spilled_values)
					{
						allocation_options.neon_q_registers =
							ALLOCATED_NEON_Q_REGISTERS;
						baseline_allocation = std::move(widened_allocation);
					}
				}
				else
				{
					// Mixed scalar-VFP/vector regions remap logical q4-q5 to
					// physical q12-q13. Physical q6 is therefore still available
					// whenever no allocated scalar lane aliases s24-s27. Decide
					// this from the completed allocation, not from a source opcode
					// or from peak-count alone: resident scalar values can occupy a
					// high lane even when vfp_peak_s is one.
					auto aliases_physical_q6 = [](const RegionAllocation::Plan& plan) {
						for (const RegionAllocation::Location& location :
							plan.value_locations)
						{
							if (location.kind !=
									RegionAllocation::LocationKind::VfpS)
								continue;
							const u32 physical_s =
								static_cast<u32>(plan.first_vfp_s) + location.index;
							if (physical_s >= 24 && physical_s < 28)
								return true;
						}
						return false;
					};
					RegionAllocation::Options widened_options = allocation_options;
					widened_options.neon_q_registers =
						ALLOCATED_NEON_Q_REGISTERS;
					RegionAllocation::BuildResult widened_allocation =
						RegionAllocation::Build(m_program, m_execution, widened_options);
					if (widened_allocation &&
						widened_allocation.plan.vfp_peak_s != 0 &&
						!aliases_physical_q6(widened_allocation.plan) &&
						widened_allocation.plan.spilled_values <=
							baseline_allocation.plan.spilled_values)
					{
						allocation_options.neon_q_registers =
							ALLOCATED_NEON_Q_REGISTERS;
						baseline_allocation = std::move(widened_allocation);
					}
				}
				// A small scalar COP1 working set can live in s24-s27 while logical
				// q0-q5 remain directly scalar-addressable. Logical q6 then moves to
				// caller-saved physical q12. Compare this complete allocation against
				// the ordinary s16-s29/q4-q5-remapped partition and select it only when
				// it represents both banks without increasing spills. This is a target
				// register-file partition, independent of guest PCs or opcode sequences.
				if (baseline_allocation.plan.vfp_peak_s != 0 &&
					baseline_allocation.plan.demanded_full_vector_values != 0 &&
					baseline_allocation.plan.vfp_peak_s <= 4)
				{
					RegionAllocation::Options partitioned_options = allocation_options;
					partitioned_options.first_vfp_s = 24;
					partitioned_options.vfp_s_registers = 4;
					partitioned_options.neon_q_registers =
						ALLOCATED_NEON_Q_REGISTERS;
					partitioned_options.scalar_addressable_neon_q_registers = 6;
					RegionAllocation::BuildResult partitioned_allocation =
						RegionAllocation::Build(m_program, m_execution,
							partitioned_options);
					if (partitioned_allocation &&
						partitioned_allocation.plan.vfp_peak_s != 0 &&
						partitioned_allocation.plan.vfp_peak_s <= 4 &&
						partitioned_allocation.plan.spilled_values <=
							baseline_allocation.plan.spilled_values)
					{
						allocation_options = partitioned_options;
						baseline_allocation = std::move(partitioned_allocation);
					}
				}
				// The A9 scalar VFP namespace aliases q0-q7.  In a mixed region whose
				// exact VU0 arithmetic cannot fit in the scalar-addressable portion of a
				// split bank, it can be cheaper to give q0-q6 to VU0 and keep the smaller
				// COP1 web in the region spill frame. Compare the complete allocation and
				// select this partition only when it eliminates a scalar-VU coloring miss
				// without excessive extra spill intervals. No semantic property depends on
				// this target-cost choice.
				if (baseline_allocation.plan.vfp_peak_s != 0 &&
					baseline_allocation.plan.demanded_full_vector_values != 0)
				{
					RegionAllocation::Options vector_priority_options = allocation_options;
					vector_priority_options.first_vfp_s = 16;
					vector_priority_options.vfp_s_registers = 0;
					vector_priority_options.allocate_vfp_words_in_core = true;
					vector_priority_options.neon_q_registers =
						ALLOCATED_NEON_Q_REGISTERS;
					vector_priority_options.scalar_addressable_neon_q_registers =
						7;
					RegionAllocation::BuildResult vector_priority_allocation =
						RegionAllocation::Build(m_program, m_execution,
							vector_priority_options);
					if (vector_priority_allocation &&
						vector_priority_allocation.plan.scalar_preferred_neon_values != 0)
					{
						allocation_options = vector_priority_options;
						baseline_allocation = std::move(vector_priority_allocation);
					}
				}
				if (m_aggregate_cycle_edges)
					allocation_options.aggregate_cycle_header_block =
						m_memory_plan.timing.header_block;

				RegionAllocation::BuildResult allocation{};
				bool have_allocation = false;
				u8 allocated_core_words = complete_core_words;
				const size_t maximum_hoisted = Persistent() && m_reclaim_vtlb_vmap &&
					baseline_allocation.plan.core_peak_words < complete_core_words ?
					std::min<size_t>(m_hoisted_memory_bases.size(),
						complete_core_words - baseline_allocation.plan.core_peak_words) : 0;
				m_hoisted_memory_bases.resize(maximum_hoisted);

				auto collect_hoisted_load_values = [&](const RegionAllocation::Plan& plan) {
					std::vector<ValueId> values;
					bool has_store = false;
					for (const Block& block : m_program.blocks)
						for (const Node& node : block.nodes)
							has_store |= node.opcode == Opcode::MemoryStore;
					if (has_store)
						return values;
					for (const ValueId operation_id : m_hoisted_memory_operations)
					{
						const Node* const operation = Definition(operation_id);
						const ValueId value = operation_id < m_memory_value_for_effect.size() ?
							m_memory_value_for_effect[operation_id] : INVALID_VALUE;
						if (!operation || operation->opcode != Opcode::MemoryLoad ||
							value == INVALID_VALUE || value >= plan.value_aliases.size() ||
							plan.value_aliases[value] != value ||
							MemoryAccessWidth(static_cast<MemoryAccessKind>(
								operation->immediate)) > sizeof(u32) ||
							value >= plan.value_locations.size())
						{
							continue;
						}
						const RegionAllocation::Location& location =
							plan.value_locations[value];
						if (location.kind != RegionAllocation::LocationKind::Core ||
							location.words != 1 || LocationMask(location) != 0x1 ||
							std::find(values.begin(), values.end(), value) != values.end())
						{
							continue;
						}
						values.push_back(value);
					}
					return values;
				};

				for (;;)
				{
					m_hoisted_memory_operations.clear();
					for (const HoistedMemoryBase& base : m_hoisted_memory_bases)
						for (const HoistedMemoryAccess& access : base.accesses)
							m_hoisted_memory_operations.push_back(access.operation);
					allocation_options.hoisted_memory_operations =
						m_hoisted_memory_operations.empty() ? nullptr :
							&m_hoisted_memory_operations;
					const u8 words_after_bases = static_cast<u8>(
						complete_core_words - m_hoisted_memory_bases.size());
					allocation_options.core_register_words = words_after_bases;
					allocation_options.region_resident_core_values = nullptr;
					RegionAllocation::BuildResult base_candidate = RegionAllocation::Build(
						m_program, m_execution, allocation_options);
					if (base_candidate && base_candidate.plan.spilled_values <=
							baseline_allocation.plan.spilled_values)
					{
						const std::vector<ValueId> possible_loads =
							collect_hoisted_load_values(base_candidate.plan);
						const size_t maximum_loads = words_after_bases > 1 ?
							std::min<size_t>(possible_loads.size(), words_after_bases - 1) : 0;
						for (size_t load_count = maximum_loads;; load_count--)
						{
							std::vector<ValueId> selected_loads(
								possible_loads.begin(), possible_loads.begin() + load_count);
							allocation_options.core_register_words = words_after_bases;
							allocation_options.region_resident_core_values =
								selected_loads.empty() ? nullptr : &selected_loads;
							RegionAllocation::BuildResult candidate = load_count == 0 ?
								std::move(base_candidate) : RegionAllocation::Build(
									m_program, m_execution, allocation_options);
							if (candidate && candidate.plan.spilled_values <=
									baseline_allocation.plan.spilled_values)
							{
								allocation = std::move(candidate);
								have_allocation = true;
								m_region_resident_memory_load_values =
									std::move(selected_loads);
								allocated_core_words = words_after_bases;
								break;
							}
							if (load_count == 0)
								break;
						}
						if (have_allocation)
							break;
					}
					if (m_hoisted_memory_bases.empty())
						break;
						m_hoisted_memory_bases.pop_back();
				}
				allocation_options.region_resident_core_values = nullptr;
				if (!have_allocation)
				{
					// Address/load hoisting and aggregate-exit liveness are optional
					// allocation refinements. If their reduced bank would introduce more
					// spills, retain the already verified conservative plan. A failed
					// optimization must never turn the default-constructed BuildResult
					// (whose failure code is None) into an empty executable plan.
					m_hoisted_memory_bases.clear();
					m_hoisted_memory_operations.clear();
					m_region_resident_memory_load_values.clear();
					allocated_core_words = complete_core_words;
					allocation = std::move(baseline_allocation);
						have_allocation = true;
					}
				m_allocation = std::move(allocation.plan);
				if (m_emit_semantic_kernel && !ValidateSemanticKernelAllocation())
				{
					// Exact semantic support is independent of one physical A9 register
					// partition. Retain the ordinary verified lowering when this image
					// cannot keep all invariants plus four private work quads resident.
					m_emit_semantic_kernel = false;
					m_result.semantic_kernel_kind = 0;
					m_result.semantic_kernel_bytes_per_iteration = 0;
					m_result.semantic_kernel_minimum_profitable_bytes = 0;
				}
				if (m_emit_semantic_kernel &&
					m_semantic_plan.kind == SemanticKernel::Kind::Vu0AffineTransform)
				{
					m_work_scratch_bytes = VU0_AFFINE_WORK_SCRATCH_BYTES;
				}
				else if (m_emit_semantic_kernel &&
					m_semantic_plan.kind == SemanticKernel::Kind::Cop1Stream)
				{
					// Offsets 0..15 remain the shared entry-range scratch and exact trip
					// count.  The serial COP1 owner uses 16..23 only while comparing a
					// represented read range against one represented write range before
					// the first guest effect.
					m_work_scratch_bytes = 24;
				}
				else if (m_emit_semantic_search_island)
				{
					const u32 match_words = SemanticSearchTargetWordCount(true);
					const u32 exhausted_words = SemanticSearchTargetWordCount(false);
					const u32 target_words = std::max(match_words, exhausted_words);
					if (target_words == 0 || target_words > 256)
						return Fail(CompileFailure::RegisterPressure,
							BlockPc(m_semantic_plan.header_block));
					m_work_scratch_bytes = AlignUp(SEARCH_OUTPUT_OFFSET +
						target_words * sizeof(u32), 16);
				}
				if (!m_hoisted_memory_bases.empty())
				{
					const auto& bank = PERSISTENT_IDENTITY_ALLOCATED_CORE;
					for (size_t index = 0; index < m_hoisted_memory_bases.size(); index++)
					{
						m_hoisted_memory_bases[index].host = bank[
							allocated_core_words + index];
					}
				}
				BuildHoistedMemoryLoadCandidates();
				m_persistent_scheduler_countdown = Persistent();
				// A first-class region entry pays the complete exact range/event/SMC
				// certificate once, while tier zero pays per-access work in the loop.
				// RegionMemoryPlan supplies optional Cortex-A9 cost evidence and an exact
				// entry selector. It does not define backend support: Runtime evaluates the
				// emitted image independently before executable publication.
				if (Persistent())
				{
					m_profitability_certificate =
						RegionMemoryPlan::BuildA9ProfitabilityCertificate(m_program,
							m_memory_plan,
							m_options.minimum_a9_counted_memory_work,
							m_options.minimum_a9_observed_region_work,
							m_options.emit_profitability_guard,
							m_options.emit_profitability_guard ?
							RegionMemoryPlan::A9ProfitabilityCertificate::
								DEFAULT_MINIMUM_READ_ONLY_ITERATIONS : 1u,
							m_options.minimum_a9_observed_vu0_fmac_iterations,
							m_options.minimum_a9_observed_vu0_fmac_leaf_invocations);
					m_result.minimum_profitable_iterations =
						m_profitability_certificate.minimum_profitable_iterations;
					m_result.profitability_diagnostic_flags =
						m_profitability_certificate.diagnostic_flags;
					m_result.profitability_certificate_kind = static_cast<u8>(
						m_profitability_certificate.kind);
					m_result.profitability_certificate_valid =
						static_cast<bool>(m_profitability_certificate);
					if (!m_options.emit_profitability_guard &&
						(m_profitability_certificate.kind == RegionMemoryPlan::
							A9ProfitabilityCertificate::Kind::ObservedPreflightedLoop ||
						 m_profitability_certificate.kind == RegionMemoryPlan::
							A9ProfitabilityCertificate::Kind::ObservedBoundaryElision))
					{
						// Explicit oracle/backend fixtures must execute the generated unit
						// even below product break-even. Keeping an observed-boundary selector
						// active here silently tested tier zero instead of the new return/CFG
						// lowering. Product compilation always requires the certificate.
						m_result.minimum_profitable_iterations = 0;
					}
					if (m_profitability_certificate.kind == RegionMemoryPlan::
						A9ProfitabilityCertificate::Kind::ObservedPreflightedLoop)
					{
						// A profitable probe proves that this CFG can have a long invocation;
						// it does not prove that every later invocation is long. Preserve an
						// exact entry selector for the independently measured generated-code
						// class. Runtime publication keeps tier-zero edges originating inside
						// this CFG away from the selector, so one rejected external invocation
						// executes the complete tier-zero loop instead of retrying the region
						// at every backedge.
						if (m_options.classify_narrowed_preflighted_cost &&
							m_profitability_certificate.work_per_iteration != 0)
						{
							const u32 work = CompileOptions::
								DEFAULT_MINIMUM_A9_NARROWED_PREFLIGHTED_WORK;
							m_result.minimum_profitable_iterations =
								std::max<u32>(1, static_cast<u32>((static_cast<u64>(work) +
									m_profitability_certificate.work_per_iteration - 1) /
									m_profitability_certificate.work_per_iteration));
						}
					}
					if (m_emit_semantic_kernel && m_options.emit_profitability_guard &&
						m_result.semantic_kernel_bytes_per_iteration != 0)
					{
						const u32 minimum_iterations = static_cast<u32>((static_cast<u64>(
							m_result.semantic_kernel_minimum_profitable_bytes) +
							m_result.semantic_kernel_bytes_per_iteration - 1) /
							m_result.semantic_kernel_bytes_per_iteration);
						m_result.minimum_profitable_iterations = std::max(
							m_result.minimum_profitable_iterations, minimum_iterations);
					}
				}
				// BlockCompiler::EndBlockWithCycleTest() owns the Vita persistent
				// dispatcher's signed low-word event countdown. Reuse that exact hot
				// contract for every first-class region: one ADDS per edge, with the
				// canonical 64-bit cycle rebuilt only at a cold exit. A counted-memory
				// certificate checks its aggregate budget in this same domain at entry;
				// it does not need a 64-bit cycle in every loop iteration.
				PublishPlanStats();
				if (m_program.source_blocks.empty() || m_program.entry_block >=
						m_program.blocks.size())
				{
					return Fail(CompileFailure::UnattestedSource, BlockPc(m_program.entry_block));
				}
				if (m_options.max_hot_code_bytes == 0 ||
					m_options.max_entry_hot_code_bytes == 0 ||
					m_options.max_block_hot_code_bytes == 0 ||
					m_options.max_code_bytes == 0)
					return Fail(CompileFailure::CodeCapacity, BlockPc(m_program.entry_block));

				BuildCop1LoweringPlan();

				m_saved_vtlb_vmap_offset = AlignUp(m_allocation.spill_bytes, 4);
				m_work_scratch_offset = AlignUp(m_saved_vtlb_vmap_offset +
					(m_reclaim_vtlb_vmap ? sizeof(u32) : 0u), 16);
				m_cycle_snapshot_offset = m_work_scratch_offset +
					(m_options.omit_proven_unused_work_scratch_frame ? 0u :
						m_work_scratch_bytes);
				m_use_compact_persistent_exits = Persistent() &&
					m_allocation.executable_exit_state_words >=
						MIN_COMPACT_PERSISTENT_EXIT_WORDS;
				m_cold_core_snapshot_offset = AlignUp(m_cycle_snapshot_offset, 4);
				m_cold_vfp_snapshot_offset = m_cold_core_snapshot_offset +
					PERSISTENT_COLD_CORE_REGISTERS.size() * sizeof(u32);
				const u32 cold_snapshot_end = m_use_compact_persistent_exits ?
					m_cold_vfp_snapshot_offset +
						(PERSISTENT_COLD_VFP_LOW_S_COUNT +
						 PERSISTENT_COLD_VFP_HIGH_S_COUNT) * sizeof(u32) :
					m_cycle_snapshot_offset;
				m_frame_bytes = AlignUp(cold_snapshot_end, 8);
				m_result.persistent_cold_snapshot_bytes =
					m_use_compact_persistent_exits ?
						static_cast<u32>(PERSISTENT_COLD_CORE_REGISTERS.size() *
							sizeof(u32) +
							(PERSISTENT_COLD_VFP_LOW_S_COUNT +
							 PERSISTENT_COLD_VFP_HIGH_S_COUNT) * sizeof(u32)) : 0;
				if (m_frame_bytes > 8192)
					return Fail(CompileFailure::RegisterPressure, BlockPc(m_program.entry_block));
				// Scalar VFP allocation owns s16-s29, so mixed COP1/vector regions map
				// allocated q4-q5 to physical q12-q13. A VU0/MMI-only region has no
				// such owner: q0-q6 remain directly scalar-addressable. Physical q7
				// remains private scratch for independently rounded VU0 lanes.
				// CodeBuffer storage is reused across cold compilations. Register-bank
				// mapping belongs to this one immutable image, so clear any predecessor's
				// mapping before selecting the current allocation's physical partition.
				m_code.SetNeonQRegisterBankMapping(0, 0, 0);
				m_remap_allocated_neon_high_bank = m_allocation.vfp_peak_s != 0;
				if (m_remap_allocated_neon_high_bank)
				{
					m_neon_remap_first_q =
						m_allocation.first_vfp_s == 24 ? 6 : 4;
					m_neon_remap_physical_first_q = 12;
					m_neon_remap_q_count =
						m_allocation.first_vfp_s == 24 ? 1 : 2;
					m_code.SetNeonQRegisterBankMapping(m_neon_remap_first_q,
						m_neon_remap_physical_first_q,
						m_neon_remap_q_count);
				}
				// The four separately rounded multiply results use s28-s31 only for
				// the duration of one fused VMADD. Mixed COP1/VU0 allocation does not
				// inherently make that bank unsafe: it is available whenever no
				// allocation-owned scalar interval can name s28 or s29. Derive this from
				// physical locations rather than disabling the fast path for every mixed
				// region.
				m_vu0_product_scratch_available = true;
				for (const RegionAllocation::Location& location :
					m_allocation.value_locations)
				{
					if (location.kind != RegionAllocation::LocationKind::VfpS)
						continue;
					const u32 physical_s = static_cast<u32>(
						m_allocation.first_vfp_s) + location.index;
					m_vu0_product_scratch_available &= physical_s < 28;
				}
				m_block_offsets.assign(m_program.blocks.size(), static_cast<size_t>(-1));
				if (!BuildBlockLayout())
					return Fail(CompileFailure::InvalidProgram,
						BlockPc(m_program.entry_block));
				if (m_emit_semantic_search_island &&
					!BuildSemanticSearchBlockLayout())
				{
					return Fail(CompileFailure::InvalidProgram,
						BlockPc(m_semantic_plan.header_block));
				}
				auto emit_phase = [&](auto&& emitter, u32* emitted_bytes) {
					const size_t before = m_code.Size();
					if (!emitter())
						return false;
					*emitted_bytes = static_cast<u32>(m_code.Size() - before);
					return true;
				};
				auto fail_entry_phase = [&](u8 step) -> CompileResult {
					m_result.failure_emission_step = step;
					return Fail(CompileFailure::Emission,
						BlockPc(m_program.entry_block));
				};
				if (!emit_phase([&]() { return EmitPrologue(); },
						&m_result.prologue_hot_bytes))
					return fail_entry_phase(80);
				if (!EmitVu0NormalizationConstants())
					return fail_entry_phase(81);
				if (!emit_phase([&]() { return EmitEntryEventCheck(); },
						&m_result.entry_event_hot_bytes))
					return fail_entry_phase(82);
				if (!emit_phase([&]() { return EmitEntryIterationBudgetGuard(); },
						&m_result.entry_iteration_hot_bytes))
					return fail_entry_phase(83);
				if (!emit_phase([&]() {
						return EmitEntryMemoryPlanGuard() && EmitHoistedMemoryBases() &&
							EmitHoistedMemoryLoads();
					},
						&m_result.entry_memory_hot_bytes))
					return fail_entry_phase(84);
				if (!emit_phase([&]() {
						return EmitHoistedCop1Normalizers() &&
						       EmitHoistedVu0Normalizers();
					},
						&m_result.entry_cop1_hot_bytes))
					return fail_entry_phase(85);
				const size_t before_entry_branch = m_code.Size();
				const size_t entry_branch = m_code.EmitBranchPlaceholder();
				if (entry_branch == static_cast<size_t>(-1))
					return fail_entry_phase(86);
				m_result.entry_branch_hot_bytes = static_cast<u32>(
					m_code.Size() - before_entry_branch);

				const size_t before_blocks = m_code.Size();
				const u32 before_block_spill_word_loads = m_result.spill_word_loads;
				const u32 before_block_spill_word_stores = m_result.spill_word_stores;
				const u32 before_block_spill_vfp_loads = m_result.spill_vfp_loads;
				const u32 before_block_spill_vfp_stores = m_result.spill_vfp_stores;
				const u32 before_block_spill_vector_loads = m_result.spill_vector_loads;
				const u32 before_block_spill_vector_stores = m_result.spill_vector_stores;
				if (m_options.validation_only_measure_oversize_hot_image)
				{
					m_result.target_cost_blocks.resize(m_program.blocks.size());
					for (u32 block = 0; block < m_program.blocks.size(); block++)
					{
						RegionA32::CompileResult::TargetCostBlock& cost =
							m_result.target_cost_blocks[block];
						cost.pc = m_program.blocks[block].pc;
						cost.source_instructions = static_cast<u32>(
							m_program.blocks[block].source.size());
						if (block < m_allocation.executable_exit_sites_by_block.size())
							cost.exit_sites =
								m_allocation.executable_exit_sites_by_block[block];
						if (block <
								m_allocation.executable_exit_state_words_by_block.size())
						{
							cost.exit_state_words =
								m_allocation.executable_exit_state_words_by_block[block];
						}
						for (const RegionAllocation::Interval& interval :
							m_allocation.intervals)
						{
							if (interval.block != block || interval.location.kind !=
									RegionAllocation::LocationKind::Spill ||
								interval.value >=
									m_allocation.value_representations.size())
							{
								continue;
							}
							// Match CompileResult's aggregate semantic-bank attribution.
							// A spilled value has no resident physical bank; F32Bits can
							// deliberately use a core representation, but still belongs to
							// the VFP/COP1 pressure class being diagnosed here.
							if (interval.type == ValueType::VuF32x4Bits ||
								interval.type == ValueType::I128)
							{
								cost.spilled_neon_values++;
							}
							else if (interval.type == ValueType::F32Bits)
							{
								cost.spilled_vfp_values++;
							}
							else
							{
								cost.spilled_core_values++;
							}
						}
						for (const RegionAllocation::EdgeMove& move :
							m_allocation.edge_moves)
						{
							if (move.source_block != block)
								continue;
							cost.edge_moves++;
							const u8 word_mask = move.word_mask != 0 ? move.word_mask :
								(move.target_location.word_mask != 0 ?
									move.target_location.word_mask :
									static_cast<u8>((1u << move.target_location.words) - 1u));
							cost.edge_state_words += std::popcount(word_mask);
						}
						for (const DirectCallContract& call : m_program.direct_calls)
						{
							for (const SourceInstruction& source :
								m_program.blocks[block].source)
							{
								cost.direct_call_roles |= source.pc == call.call_pc ? 1u : 0u;
								cost.direct_call_roles |=
									source.pc == call.return_jump_pc ? 8u : 0u;
							}
							cost.direct_call_roles |=
								m_program.blocks[block].pc == call.callee_pc ? 2u : 0u;
							cost.direct_call_roles |=
								m_program.blocks[block].pc == call.return_pc ? 4u : 0u;
						}
					}
				}
				if (m_emit_semantic_kernel)
				{
					m_block_offsets[m_program.entry_block] = m_code.Size();
					const size_t semantic_begin = m_code.Size();
					if (!EmitSemanticKernel())
					{
						if (m_result.failure != CompileFailure::None)
							return m_result;
						if (m_result.failure_emission_step == 0)
							m_result.failure_emission_step = 120;
						return Fail(CompileFailure::Emission,
							BlockPc(m_program.entry_block));
					}
					m_result.semantic_kernel_hot_bytes = static_cast<u32>(
						m_code.Size() - semantic_begin);
					m_result.semantic_kernel_target_cost_valid =
						m_result.semantic_kernel_hot_bytes != 0 &&
						m_result.semantic_kernel_bytes_per_iteration != 0 &&
						m_result.semantic_kernel_minimum_profitable_bytes >=
							CompileOptions::DEFAULT_MINIMUM_A9_SEMANTIC_KERNEL_BYTES;
				}
				else for (const u32 block : m_block_layout)
				{
					const u32 before_word_loads = m_result.spill_word_loads;
					const u32 before_word_stores = m_result.spill_word_stores;
					const u32 before_vfp_loads = m_result.spill_vfp_loads;
					const u32 before_vfp_stores = m_result.spill_vfp_stores;
					const u32 before_vector_loads = m_result.spill_vector_loads;
					const u32 before_vector_stores = m_result.spill_vector_stores;
					m_block_offsets[block] = m_code.Size();
					const size_t semantic_begin = m_code.Size();
					const bool emitted = m_emit_semantic_search_island &&
						block == m_semantic_plan.header_block ?
						EmitSemanticSearchIsland() : EmitBlock(block);
					if (!emitted)
					{
						if (m_result.failure != CompileFailure::None)
							return m_result;
						return Fail(CompileFailure::Emission, m_program.blocks[block].pc);
					}
					if (m_emit_semantic_search_island &&
						block == m_semantic_plan.header_block)
					{
						m_result.semantic_kernel_hot_bytes = static_cast<u32>(
							m_code.Size() - semantic_begin);
						m_result.memory_loads++;
					}
					if (block < m_result.target_cost_blocks.size())
					{
						RegionA32::CompileResult::TargetCostBlock& cost =
							m_result.target_cost_blocks[block];
						cost.hot_bytes = static_cast<u32>(
							m_code.Size() - m_block_offsets[block]);
						cost.spill_loads =
							(m_result.spill_word_loads - before_word_loads) +
							(m_result.spill_vfp_loads - before_vfp_loads) +
							(m_result.spill_vector_loads - before_vector_loads);
						cost.spill_stores =
							(m_result.spill_word_stores - before_word_stores) +
							(m_result.spill_vfp_stores - before_vfp_stores) +
							(m_result.spill_vector_stores - before_vector_stores);
					}
				}
				m_result.block_hot_bytes = static_cast<u32>(
					m_code.Size() - before_blocks);
				if (m_emit_semantic_search_island)
				{
					// Exact emission is proven here, but target retention remains false
					// until the inclusive physical-A9 benchmark establishes the >=8x
					// floor and its minimum profitable trip count.
					m_result.semantic_kernel_target_cost_valid = false;
				}
				m_result.block_spill_word_loads =
					m_result.spill_word_loads - before_block_spill_word_loads;
				m_result.block_spill_word_stores =
					m_result.spill_word_stores - before_block_spill_word_stores;
				m_result.block_spill_vfp_loads =
					m_result.spill_vfp_loads - before_block_spill_vfp_loads;
				m_result.block_spill_vfp_stores =
					m_result.spill_vfp_stores - before_block_spill_vfp_stores;
				m_result.block_spill_vector_loads =
					m_result.spill_vector_loads - before_block_spill_vector_loads;
				m_result.block_spill_vector_stores =
					m_result.spill_vector_stores - before_block_spill_vector_stores;
				const u32 entry_hot_bytes = static_cast<u32>(before_blocks);
				if (!m_options.validation_only_measure_oversize_hot_image &&
					(entry_hot_bytes > m_options.max_entry_hot_code_bytes ||
					 m_result.block_hot_bytes > m_options.max_block_hot_code_bytes))
				{
					return Fail(CompileFailure::CodeCapacity, 0);
				}
				if (!m_code.PatchBranch(entry_branch,
						m_block_offsets[m_program.entry_block]))
				{
					return Fail(CompileFailure::Patch, BlockPc(m_program.entry_block));
				}
				for (const InternalPatch& patch : m_internal_patches)
				{
					if (patch.target_block >= m_block_offsets.size() ||
						!m_code.PatchBranch(patch.offset,
							m_block_offsets[patch.target_block], patch.condition))
					{
						return Fail(CompileFailure::Patch, 0);
					}
				}

				m_result.hot_code_bytes = static_cast<u32>(m_code.Size());
				if (m_code.OutOfSpace() ||
					(!m_options.validation_only_measure_oversize_hot_image &&
					 m_result.hot_code_bytes > m_options.max_hot_code_bytes) ||
					m_result.hot_code_bytes > m_options.max_code_bytes)
				{
					return Fail(CompileFailure::CodeCapacity, 0);
				}
				if (!EmitVu0StatusColdPaths() || !EmitVu0MacColdPaths() ||
					!EmitCop1GuardedColdPaths())
				{
					return Fail(m_code.OutOfSpace() ? CompileFailure::CodeCapacity :
						CompileFailure::Emission, 0);
				}
				if (!EmitLiteralPool())
					return Fail(CompileFailure::Patch, 0);
				if (!EmitCop1ColdNumericLeaves())
				{
					return Fail(m_code.OutOfSpace() ? CompileFailure::CodeCapacity :
						CompileFailure::Emission, 0);
				}
				if (!EmitPreEntryFallbacks())
				{
					m_result.failure_emission_step = 87;
					return Fail(m_code.OutOfSpace() ? CompileFailure::CodeCapacity :
						CompileFailure::Emission, BlockPc(m_program.entry_block));
				}
				std::vector<ColdExitEpilogue> epilogues;
				for (const ColdExit& exit : m_cold_exits)
				{
					const size_t target = m_code.Size();
					if (!m_code.PatchBranch(exit.branch_offset, target, exit.condition))
						return Fail(CompileFailure::Patch, 0);
					for (const ColdExit::AliasPatch& alias : exit.aliases)
					{
						if (!m_code.PatchBranch(alias.offset, target, alias.condition))
							return Fail(CompileFailure::Patch, 0);
					}
					if ((exit.deferred_cycle_advance != 0 &&
						(!m_persistent_scheduler_countdown ||
							 !EmitCycleAdvance(exit.deferred_cycle_advance, false))) ||
							!EmitExitMetadata(exit))
						{
							m_result.failure_emission_step = 88;
						return Fail(m_code.OutOfSpace() ?
							CompileFailure::CodeCapacity : CompileFailure::Emission,
							exit.site && exit.site->block < m_program.blocks.size() ?
								m_program.blocks[exit.site->block].pc : 0);
					}
					if (Persistent())
					{
						RegionExecution::ExitContractView contract{};
						u32 resume_pc = 0;
						bool dynamic_resume_pc = false;
						bool compact = false;
						if (!exit.site || !RegionExecution::ResolveExitContract(
								m_program, *exit.site, &contract) || !contract.state ||
							!ResolvePersistentResumePc(*exit.site, contract, &resume_pc,
								&dynamic_resume_pc) ||
							!EmitCompactPersistentExit(*exit.site, exit, contract,
								resume_pc, dynamic_resume_pc, &compact))
						{
							m_result.failure_emission_step = 89;
							return Fail(m_code.OutOfSpace() ?
								CompileFailure::CodeCapacity : CompileFailure::Emission,
								exit.site && exit.site->block < m_program.blocks.size() ?
									m_program.blocks[exit.site->block].pc : 0);
						}
						if (compact)
							continue;
						if (!EmitExitState(*exit.site, &exit))
						{
							m_result.failure_emission_step = 89;
							return Fail(m_code.OutOfSpace() ?
								CompileFailure::CodeCapacity : CompileFailure::Emission,
								exit.site && exit.site->block < m_program.blocks.size() ?
									m_program.blocks[exit.site->block].pc : 0);
						}
						m_result.cold_exit_epilogues++;
						m_result.cold_exit_leaves++;
						continue;
					}

					const size_t epilogue_branch = m_code.EmitBranchPlaceholder();
					if (epilogue_branch == static_cast<size_t>(-1))
					{
						return Fail(m_code.OutOfSpace() ?
							CompileFailure::CodeCapacity : CompileFailure::Emission,
							exit.site && exit.site->block < m_program.blocks.size() ?
								m_program.blocks[exit.site->block].pc : 0);
					}
					auto epilogue = std::find_if(epilogues.begin(), epilogues.end(),
						[&](const ColdExitEpilogue& candidate) {
							return candidate.site && exit.site &&
							       ExitStatesEquivalent(*candidate.site, *exit.site);
						});
					if (epilogue == epilogues.end())
					{
						epilogues.push_back({exit.site, {epilogue_branch}});
					}
					else
					{
						epilogue->branch_offsets.push_back(epilogue_branch);
					}
					m_result.cold_exit_leaves++;
				}
				if (!EmitCompactPersistentExitHub())
				{
					m_result.failure_emission_step = 90;
					return Fail(m_code.OutOfSpace() ? CompileFailure::CodeCapacity :
						CompileFailure::Emission, BlockPc(m_program.entry_block));
				}
				for (const ColdExitEpilogue& epilogue : epilogues)
				{
					const size_t target = m_code.Size();
					for (const size_t branch : epilogue.branch_offsets)
					{
						if (!m_code.PatchBranch(branch, target))
							return Fail(CompileFailure::Patch, 0);
					}
					if (!epilogue.site || !EmitExitState(*epilogue.site, nullptr))
					{
						m_result.failure_emission_step = 90;
						return Fail(m_code.OutOfSpace() ?
							CompileFailure::CodeCapacity : CompileFailure::Emission,
							epilogue.site &&
								epilogue.site->block < m_program.blocks.size() ?
								m_program.blocks[epilogue.site->block].pc : 0);
					}
					m_result.cold_exit_epilogues++;
				}
				if (!FinalizeFrame())
					return Fail(CompileFailure::Patch, 0);
				if (m_code.OutOfSpace() || m_code.Size() > m_options.max_code_bytes)
					return Fail(CompileFailure::CodeCapacity, 0);
				if (!m_code.Flush())
					return Fail(CompileFailure::Emission, 0);
				m_result.code_bytes = static_cast<u32>(m_code.Size());
				m_result.cold_code_bytes =
					m_result.code_bytes - m_result.hot_code_bytes;
				// Persistent regions address architectural state through the
				// dispatcher's cpuRegs base in r4.  Callable regions use the
				// ExecutionContext in r11.  Attribute the actual product ABI rather
				// than reporting persistent state traffic as ordinary loads/stores.
				const unsigned architectural_state_base =
					Persistent() ? CPU_REGS : CONTEXT;
				const VitaA32::CodeBuffer::GeneratedCodeStats all =
					m_code.AnalyzeGeneratedCode(architectural_state_base);
				const VitaA32::CodeBuffer::GeneratedCodeStats hot =
					m_code.AnalyzeGeneratedCodeRange(0, m_result.hot_code_bytes,
						architectural_state_base);
				const VitaA32::CodeBuffer::GeneratedCodeStats block =
					m_code.AnalyzeGeneratedCodeRange(
						m_result.hot_code_bytes - m_result.block_hot_bytes,
						m_result.block_hot_bytes, architectural_state_base);
				m_result.host_instructions = static_cast<u32>(all.host_instructions);
				m_result.hot_host_instructions = static_cast<u32>(hot.host_instructions);
				m_result.hot_host_loads = static_cast<u32>(hot.host_load_instructions);
				m_result.hot_host_stores = static_cast<u32>(hot.host_store_instructions);
				m_result.hot_state_loads = static_cast<u32>(hot.state_load_instructions);
				m_result.hot_state_stores = static_cast<u32>(hot.state_store_instructions);
				m_result.hot_stack_loads = static_cast<u32>(hot.stack_load_instructions);
				m_result.hot_stack_stores = static_cast<u32>(hot.stack_store_instructions);
				m_result.block_host_loads = static_cast<u32>(
					block.host_load_instructions);
				m_result.block_host_stores = static_cast<u32>(
					block.host_store_instructions);
				m_result.block_stack_loads = static_cast<u32>(
					block.stack_load_instructions);
				m_result.block_stack_stores = static_cast<u32>(
					block.stack_store_instructions);
				for (u32 block_index = 0;
					block_index < m_result.target_cost_blocks.size(); block_index++)
				{
					if (block_index >= m_block_offsets.size() ||
						m_block_offsets[block_index] == static_cast<size_t>(-1))
					{
						continue;
					}
					const u32 successor = block_index < m_layout_successor.size() ?
						m_layout_successor[block_index] : INVALID_BLOCK;
					const size_t end = successor < m_block_offsets.size() ?
						m_block_offsets[successor] : m_result.hot_code_bytes;
					if (end < m_block_offsets[block_index])
						continue;
					const auto block_cost = m_code.AnalyzeGeneratedCodeRange(
						m_block_offsets[block_index],
						end - m_block_offsets[block_index], architectural_state_base);
					m_result.target_cost_blocks[block_index].host_loads =
						static_cast<u32>(block_cost.host_load_instructions);
					m_result.target_cost_blocks[block_index].host_stores =
						static_cast<u32>(block_cost.host_store_instructions);
				}
				m_result.frame_bytes = m_frame_bytes;
				m_result.work_scratch_bytes = m_work_scratch_used ?
					m_work_scratch_bytes : 0u;
				const bool requires_all_scalar_reads_preflighted =
					m_profitability_certificate.kind == RegionMemoryPlan::
						A9ProfitabilityCertificate::Kind::ObservedBoundaryElision ||
					m_profitability_certificate.kind == RegionMemoryPlan::
						A9ProfitabilityCertificate::Kind::ReadOnlyLoop ||
					m_profitability_certificate.kind == RegionMemoryPlan::
						A9ProfitabilityCertificate::Kind::ObservedPreflightedLoop ||
					m_profitability_certificate.kind == RegionMemoryPlan::
						A9ProfitabilityCertificate::Kind::ObservedCop1Residency;
				const bool observed_preflighted_vu0_fmac_stream =
					m_profitability_certificate.kind == RegionMemoryPlan::
							A9ProfitabilityCertificate::Kind::ObservedVu0FmacStream ||
					m_profitability_certificate.kind == RegionMemoryPlan::
							A9ProfitabilityCertificate::Kind::ObservedCop1Vu0Residency;
				m_result.profitability_memory_coverage_valid =
					!(m_options.emit_profitability_guard &&
					(requires_all_scalar_reads_preflighted ||
					 observed_preflighted_vu0_fmac_stream) &&
					((m_profitability_certificate.kind != RegionMemoryPlan::
							A9ProfitabilityCertificate::Kind::ObservedPreflightedLoop &&
					  m_profitability_certificate.kind != RegionMemoryPlan::
							A9ProfitabilityCertificate::Kind::ObservedVu0FmacStream &&
					  m_profitability_certificate.kind != RegionMemoryPlan::
							A9ProfitabilityCertificate::Kind::ObservedCop1Vu0Residency &&
					  m_profitability_certificate.kind != RegionMemoryPlan::
							A9ProfitabilityCertificate::Kind::ObservedVu0AcyclicFmacLeaf &&
					  m_result.memory_stores != 0) ||
					 m_result.memory_loads + m_result.memory_stores !=
						 m_result.memory_preflight_accesses));
				if (m_options.classify_narrowed_preflighted_cost)
				{
					// This target-local class was measured through the complete persistent
					// dispatcher, not as an isolated callable function. The allocation
					// prescreen is deliberately only permission to spend a compile token;
					// emitted A32 is the final authority. Keep the repeated body below two
					// host instructions per decoded instruction, no more than one host load
					// per two decoded instructions, no store traffic beyond the semantic
					// stores, and no spill traffic. Two exact low-word comparisons are the
					// independently measured narrowing benefit; all fallible memory must
					// still be covered by the ordinary entry certificate.
					const u32 repeated_work =
						m_profitability_certificate.work_per_iteration;
					const u32 body_instructions = m_result.block_hot_bytes / sizeof(u32);
					const bool measured_class =
						m_profitability_certificate.kind == RegionMemoryPlan::
							A9ProfitabilityCertificate::Kind::ObservedPreflightedLoop &&
						repeated_work != 0 &&
						(m_result.block_hot_bytes % sizeof(u32)) == 0 &&
						m_result.allocation_spilled_values == 0 &&
						m_result.allocation_spill_bytes == 0 &&
						m_result.narrowed_equal64_comparisons +
							m_result.narrowed_ordered64_comparisons >= 2 &&
						body_instructions <= repeated_work * 2u &&
						m_result.block_host_loads * 2u <= repeated_work &&
						m_result.block_host_stores <= m_result.memory_stores &&
						m_result.memory_loads + m_result.memory_stores ==
							m_result.memory_preflight_accesses;
					m_result.profitability_narrowed_cost_valid = measured_class;
				}
				return m_result;
			}

		private:
			bool BuildBlockLayout()
			{
				m_block_layout.clear();
				m_layout_successor.assign(m_program.blocks.size(), INVALID_BLOCK);
				if (m_program.entry_block >= m_program.blocks.size())
					return false;

				std::vector<u8> emitted(m_program.blocks.size(), 0);
				auto append_trace = [&](u32 first) {
					u32 block = first;
					while (block < m_program.blocks.size() && !emitted[block])
					{
						emitted[block] = 1;
						m_block_layout.push_back(block);
						const Block& source = m_program.blocks[block];
						u32 next = INVALID_BLOCK;
						if (source.terminator.kind == TerminatorKind::Transfer ||
							source.terminator.kind == TerminatorKind::Jump)
						{
							next = source.terminator.taken.target_block;
						}
						else
						{
							// Continue one internal trace through an unvisited successor.
							// Taken first matches the ordinary loop/body direction; a
							// visited latch target naturally terminates the trace.
							const u32 taken = source.terminator.taken.target_block;
							const u32 not_taken = source.terminator.not_taken.target_block;
							if (taken < emitted.size() && !emitted[taken])
								next = taken;
							else if (not_taken < emitted.size() && !emitted[not_taken])
								next = not_taken;
						}
						if (next >= emitted.size() || emitted[next])
							break;
						block = next;
					}
				};

				append_trace(m_program.entry_block);
				for (u32 block = 0; block < m_program.blocks.size(); block++)
				{
					if (!emitted[block])
						append_trace(block);
				}
				if (m_block_layout.size() != m_program.blocks.size())
					return false;
				for (size_t index = 1; index < m_block_layout.size(); index++)
					m_layout_successor[m_block_layout[index - 1]] =
						m_block_layout[index];
				return true;
			}

			bool BuildSemanticSearchBlockLayout()
			{
				if (!m_emit_semantic_search_island ||
					m_semantic_plan.header_block >= m_program.blocks.size() ||
					m_semantic_plan.completion_block >= m_program.blocks.size() ||
					m_semantic_plan.bounded_equal_searches.size() != 1)
				{
					return false;
				}
				const auto& search = m_semantic_plan.bounded_equal_searches.front();
				std::vector<u8> repeated(m_program.blocks.size(), 0);
				for (const u32 block : m_semantic_plan.iteration_blocks)
				{
					if (block >= repeated.size() || repeated[block])
						return false;
					repeated[block] = 1;
				}

				// No ordinary block may enter the middle of the replaced SCC.  The
				// verifier-owned header is its sole legal entry; match and exhausted
				// suffixes remain ordinary generated blocks.
				for (u32 source = 0; source < m_program.blocks.size(); source++)
				{
					if (repeated[source])
						continue;
					bool valid = true;
					VisitInternalTransfers(m_program.blocks[source].terminator,
						[&](const Transfer& transfer, u8) {
							if (transfer.target_block < repeated.size() &&
								repeated[transfer.target_block] &&
								transfer.target_block != m_semantic_plan.header_block)
							{
								valid = false;
							}
							return valid;
						});
					if (!valid)
						return false;
				}

				std::vector<u32> filtered;
				filtered.reserve(m_block_layout.size());
				for (const u32 block : m_block_layout)
				{
					if (!repeated[block] || block == m_semantic_plan.header_block)
						filtered.push_back(block);
				}
				if (std::find(filtered.begin(), filtered.end(),
						m_semantic_plan.header_block) == filtered.end() ||
					std::find(filtered.begin(), filtered.end(),
						search.match_target_block) == filtered.end() ||
					std::find(filtered.begin(), filtered.end(),
						m_semantic_plan.completion_block) == filtered.end())
				{
					return false;
				}
				m_block_layout = std::move(filtered);
				m_layout_successor.assign(m_program.blocks.size(), INVALID_BLOCK);
				for (size_t index = 1; index < m_block_layout.size(); index++)
					m_layout_successor[m_block_layout[index - 1]] =
						m_block_layout[index];
				return true;
			}

			bool Persistent() const
			{
				return m_options.persistent_dispatch != nullptr;
			}

			struct PersistentStateLocation
			{
				u16 offset = 0;
				CompileOptions::PersistentStateBase base =
					CompileOptions::PersistentStateBase::CpuRegisters;
			};

			bool PersistentRuntimeLocation(u32 canonical_offset,
				PersistentStateLocation* location) const
			{
				if (!location || !m_options.persistent_dispatch)
					return false;
				const CompileOptions::PersistentDispatch& persistent =
					*m_options.persistent_dispatch;
				for (size_t index = 0; index < persistent.state_word_count; index++)
				{
					if (persistent.state_words[index].canonical_offset ==
						canonical_offset)
					{
						location->offset =
							persistent.state_words[index].runtime_offset;
						location->base =
							persistent.state_words[index].runtime_base;
						return true;
					}
				}
				return false;
			}

			bool EmitContextLoad(unsigned destination, size_t offset)
			{
				if (!Persistent())
				{
					return offset <= 4095 && m_code.EmitLdrImm12(destination,
						CONTEXT, static_cast<u16>(offset));
				}

				const CompileOptions::PersistentDispatch& persistent =
					*m_options.persistent_dispatch;
				if (offset == offsetof(ExecutionContext, vmap))
				{
					return m_reclaim_vtlb_vmap ?
						m_code.EmitMovImm32(destination, static_cast<u32>(
							reinterpret_cast<uptr>(persistent.vmap))) :
						EmitMove(destination, VTLB_VMAP);
				}
				if (offset == offsetof(ExecutionContext, host_memory_base))
					return EmitMove(destination, VTLB_HOST_BASE);
				if (offset == offsetof(ExecutionContext, main_ram))
				{
					return m_code.EmitMovImm32(destination, static_cast<u32>(
						reinterpret_cast<uptr>(persistent.main_ram)));
				}
				if (offset == offsetof(ExecutionContext, main_ram_last_word))
				{
					return m_code.EmitMovImm32(destination, static_cast<u32>(
						reinterpret_cast<uptr>(persistent.main_ram_last_word)));
				}
				if (offset == offsetof(ExecutionContext,
					ram_source_page_live_flags))
				{
					return m_code.EmitMovImm32(destination, static_cast<u32>(
						reinterpret_cast<uptr>(
							persistent.ram_source_page_live_flags)));
				}
				if (offset == offsetof(ExecutionContext,
					ram_source_chunk_live_bits))
				{
					return m_code.EmitMovImm32(destination, static_cast<u32>(
						reinterpret_cast<uptr>(
							persistent.ram_source_chunk_live_bits)));
				}
				if (offset == offsetof(ExecutionContext, main_ram_limit))
					return m_code.EmitMovImm32(destination,
						persistent.main_ram_limit);
				if (offset == offsetof(ExecutionContext, identity_main_ram_limit))
					return m_code.EmitMovImm32(destination,
						persistent.identity_main_ram_limit);
				if (offset == offsetof(ExecutionContext, next_event_cycle_low))
				{
					return m_code.EmitLdrImm12(destination, CPU_REGS,
						static_cast<u16>(offsetof(cpuRegisters, nextEventCycle)));
				}
				if (offset == offsetof(ExecutionContext, next_event_cycle_high))
				{
					return m_code.EmitLdrImm12(destination, CPU_REGS,
						static_cast<u16>(offsetof(cpuRegisters, nextEventCycle) +
							sizeof(u32)));
				}
				return false;
			}

			u32 BlockPc(u32 block) const
			{
				return block < m_program.blocks.size() ? m_program.blocks[block].pc : 0;
			}

			CompileResult Fail(CompileFailure failure, u32 pc)
			{
				m_result.failure = failure;
				m_result.failure_pc = pc;
				// Preserve cold compile attribution before Reset() discards the
				// partial image.  This is never generated execution telemetry; it
				// distinguishes a hot-body overflow from a semantic failure without
				// rerunning a real workload under logging.
				if (failure == CompileFailure::CodeCapacity)
				{
					m_result.code_bytes = static_cast<u32>(m_code.Size());
					if (m_result.hot_code_bytes == 0)
						m_result.hot_code_bytes = m_result.code_bytes;
				}
				m_code.Reset();
				return m_result;
			}

		void PublishPlanStats()
		{
				m_result.forwarded_memory_loads =
					m_allocation.forwarded_memory_loads;
				m_result.memory_forward_candidates =
					m_allocation.memory_forward_candidates;
				m_result.memory_forward_reaching_stores =
					m_allocation.memory_forward_reaching_stores;
				m_result.memory_forward_address_matches =
					m_allocation.memory_forward_address_matches;
				m_result.memory_forward_state_matches =
					m_allocation.memory_forward_state_matches;
				m_result.hoisted_memory_bases = static_cast<u32>(
					m_hoisted_memory_bases.size());
				for (const HoistedMemoryBase& base : m_hoisted_memory_bases)
					m_result.hoisted_memory_accesses += static_cast<u32>(
						base.accesses.size());
				m_result.exit_sites = m_allocation.executable_exit_sites;
				m_result.exit_state_bindings =
					m_allocation.executable_exit_state_bindings;
				m_result.exit_state_words =
					m_allocation.executable_exit_state_words;
				m_result.exit_sites_by_kind =
					m_allocation.executable_exit_sites_by_kind;
				m_result.exit_state_words_by_kind =
					m_allocation.executable_exit_state_words_by_kind;
				m_result.control_exit_sites_by_target =
					m_allocation.executable_control_exit_sites_by_target;
				m_result.control_exit_state_words_by_target =
					m_allocation.executable_control_exit_state_words_by_target;
				m_result.allocation_live_values = m_allocation.live_values;
				m_result.allocation_core_peak_words = m_allocation.core_peak_words;
				m_result.allocation_vfp_peak_s = m_allocation.vfp_peak_s;
				m_result.allocation_neon_peak_q = m_allocation.neon_peak_q;
				m_result.allocation_spilled_values = m_allocation.spilled_values;
				m_result.allocation_spill_bytes = m_allocation.spill_bytes;
				for (const RegionAllocation::Interval& interval :
					m_allocation.intervals)
				{
					if (interval.location.kind != RegionAllocation::LocationKind::Spill)
						continue;
					if (interval.type == ValueType::VuF32x4Bits ||
						interval.type == ValueType::I128)
					{
						m_result.allocation_spilled_neon_values++;
					}
					else if (interval.type == ValueType::F32Bits)
					{
						m_result.allocation_spilled_vfp_values++;
					}
					else
					{
						m_result.allocation_spilled_core_values++;
					}
				}
				m_result.allocation_edge_moves =
					static_cast<u32>(m_allocation.edge_moves.size());
				m_result.allocation_coalesced_edge_values =
					m_allocation.coalesced_edge_values;
				m_result.allocation_coalesced_spill_edge_values =
					m_allocation.coalesced_spill_edge_values;
				m_result.allocation_residual_spill_shape_mismatches =
					m_allocation.residual_spill_shape_mismatches;
				m_result.allocation_residual_spill_missing_groups =
					m_allocation.residual_spill_missing_groups;
				m_result.allocation_residual_spill_same_groups =
					m_allocation.residual_spill_same_groups;
				m_result.allocation_residual_spill_group_interferences =
					m_allocation.residual_spill_group_interferences;
				m_result.allocation_residual_spill_unexplained =
					m_allocation.residual_spill_unexplained;
				for (const RegionAllocation::EdgeMove& move : m_allocation.edge_moves)
				{
					using Kind = RegionAllocation::LocationKind;
					bool call_edge = false;
					bool return_edge = false;
					if (move.source_block < m_program.blocks.size() &&
						move.target_block < m_program.blocks.size())
					{
						const Block& source_block = m_program.blocks[move.source_block];
						const Block& target_block = m_program.blocks[move.target_block];
						for (const DirectCallContract& call : m_program.direct_calls)
						{
							call_edge |= source_block.terminator.branch_pc == call.call_pc &&
								target_block.pc == call.callee_pc;
							return_edge |=
								source_block.terminator.branch_pc == call.return_jump_pc &&
								target_block.pc == call.return_pc;
						}
						if (call_edge)
							m_result.allocation_edge_call_moves++;
						else if (return_edge)
							m_result.allocation_edge_return_moves++;
						else if (target_block.pc <= source_block.pc)
							m_result.allocation_edge_backedge_moves++;
						else
							m_result.allocation_edge_other_moves++;
					}
					else
						m_result.allocation_edge_other_moves++;
					if (move.source_location.kind == Kind::Spill &&
						move.target_location.kind == Kind::Spill)
					{
						m_result.allocation_edge_spill_moves++;
						m_result.allocation_edge_spill_to_spill_moves++;
					}
					else if (move.source_location.kind == Kind::Spill)
					{
						m_result.allocation_edge_spill_moves++;
						m_result.allocation_edge_spill_to_register_moves++;
					}
					else if (move.target_location.kind == Kind::Spill)
					{
						m_result.allocation_edge_spill_moves++;
						m_result.allocation_edge_register_to_spill_moves++;
					}
					else if (move.source_location.kind != move.target_location.kind)
					{
						m_result.allocation_edge_cross_kind_moves++;
					}
					else if (move.source_location.kind == Kind::Core ||
						move.source_location.kind == Kind::FixedCycle)
					{
						m_result.allocation_edge_core_moves++;
					}
					else if (move.source_location.kind == Kind::VfpS)
					{
						m_result.allocation_edge_vfp_moves++;
					}
					else if (move.source_location.kind == Kind::NeonQ)
					{
						m_result.allocation_edge_neon_moves++;
					}
					const Node* const target = Definition(move.target_parameter);
					const Node* const source = Definition(move.source);
					if (source && source->opcode == Opcode::Parameter)
						m_result.allocation_edge_parameter_sources++;
					else
						m_result.allocation_edge_computed_sources++;
					const u32 moved_words = WordCount(move.word_mask);
					if (!target || target->opcode != Opcode::Parameter ||
						target->immediate >= RegionExecution::STATE_SLOT_COUNT)
					{
						m_result.allocation_edge_other_state_words += moved_words;
						continue;
					}
					const RegionExecution::StateClass state_class =
						RegionExecution::DecodeStateSlot(target->immediate).state_class;
					m_result.allocation_edge_state_class_words[
						static_cast<size_t>(state_class)] += moved_words;
					switch (state_class)
					{
						case RegionExecution::StateClass::Gpr:
							m_result.allocation_edge_gpr_words += moved_words;
							break;
						case RegionExecution::StateClass::Fpr:
							m_result.allocation_edge_fpr_words += moved_words;
							break;
						case RegionExecution::StateClass::Vu0Vf:
						case RegionExecution::StateClass::Vu0Acc:
							m_result.allocation_edge_vu0_vector_words += moved_words;
							break;
						default:
							m_result.allocation_edge_other_state_words += moved_words;
							break;
					}
				}
				m_result.allocation_demanded_scalar_values =
					m_allocation.demanded_scalar_values;
				m_result.allocation_demanded_full_vector_values =
					m_allocation.demanded_full_vector_values;
			m_result.uses_state_word_contract = true;
			m_result.entry_low32_guards = static_cast<u32>(
				m_allocation.entry_low32_guards.size());

			// Seed exactly the allocated entry parameters. Parameter::immediate is
			// mechanically verified as its RegionExecution state slot, while the
			// allocation word mask is the exact subset EmitSeedEntryParameter reads.
			const Block& entry = m_program.blocks[m_program.entry_block];
			for (const Node& node : entry.nodes)
			{
				if (node.opcode != Opcode::Parameter)
					break;
				if (node.immediate >= RegionExecution::STATE_SLOT_COUNT)
					continue;
				m_result.entry_state_words[node.immediate] |=
					LocationMask(Location(node.id));
			}
			for (ValueId value = 0;
				value < m_allocation.vu0_hoisted_normalize.size(); value++)
			{
				if (m_allocation.vu0_hoisted_normalize[value] == 0)
					continue;
				const Node* const normalize = Definition(value);
				if (!normalize || normalize->operand_count != 1)
					continue;
				ValueId source = normalize->operands[0];
				if (source < m_allocation.vu0_idle_alias.size() &&
					m_allocation.vu0_idle_alias[source] != INVALID_VALUE)
				{
					source = m_allocation.vu0_idle_alias[source];
				}
				const Node* const parameter = Definition(source);
				if (parameter && parameter->opcode == Opcode::Parameter &&
					parameter->immediate < RegionExecution::STATE_SLOT_COUNT)
				{
					m_result.entry_state_words[parameter->immediate] |=
						LocationMask(Location(value));
				}
			}

			// Callable execution copies one union of possibly-dirty words back from
			// CanonicalState after the generated function returns.  Seed that union so
			// an exit which leaves one of those words untouched preserves its incoming
			// value.  Persistent execution has no such union copyback: every generated
			// exit materializes its mechanically verified dirty-word map directly into
			// cpuRegs/VU0.  Parameters actually read by a partial update were already
			// added above, so seeding the complete output union in that ABI is redundant
			// state traffic on every hot entry.
			for (const RegionExecution::ExitSite& exit : m_execution.exits)
			{
				for (size_t slot = 0; slot < RegionExecution::STATE_SLOT_COUNT; slot++)
					m_result.output_state_words[slot] |= exit.dirty_words[slot];
			}
			if (!Persistent())
			{
				for (size_t slot = 0; slot < RegionExecution::STATE_SLOT_COUNT; slot++)
					m_result.entry_state_words[slot] |= m_result.output_state_words[slot];
			}

			if (m_memory_plan.control.valid)
			{
				const RegionMemoryPlan::LoopControl& control =
					m_memory_plan.control;
				if (!control.counter_seed_is_immediate)
					m_result.entry_state_words[control.counter_gpr] |= 0x3;
				if (!control.bound_is_immediate && control.bound_gpr != 0)
					m_result.entry_state_words[control.bound_gpr] |= 0x3;
				m_result.aggregate_event_iteration_cycles =
					control.maximum_iteration_scaled_cycles;
			}
			for (const RegionAllocation::EntryLow32Guard& guard :
				m_allocation.entry_low32_guards)
			{
				if (guard.state_slot < RegionExecution::STATE_SLOT_COUNT)
					m_result.entry_state_words[guard.state_slot] |= 0x3;
			}
			if (!m_memory_plan.counted_ranges.empty() ||
				!m_memory_plan.bounded_ranges.empty())
			{
				m_result.memory_preflight_blocks = 1;
				for (const RegionMemoryPlan::CountedRange& planned_range :
					m_memory_plan.counted_ranges)
				{
					m_result.memory_preflight_accesses += static_cast<u32>(
						planned_range.accesses.size());
					m_result.memory_preflight_store_accesses += static_cast<u32>(
						std::count_if(planned_range.accesses.begin(),
							planned_range.accesses.end(),
							[](const RegionMemoryPlan::Access& access) {
								return access.store;
							}));
					m_result.entry_state_words[
						planned_range.entry_induction_gpr] |= 0x3;
					if (planned_range.invariant_base_gpr != 0)
						m_result.entry_state_words[planned_range.invariant_base_gpr] |= 0x3;
				}
				for (const RegionMemoryPlan::BoundedRange& planned_range :
					m_memory_plan.bounded_ranges)
				{
					m_result.memory_preflight_accesses += static_cast<u32>(
						planned_range.accesses.size());
					m_result.memory_preflight_store_accesses += static_cast<u32>(
						std::count_if(planned_range.accesses.begin(),
							planned_range.accesses.end(),
							[](const RegionMemoryPlan::Access& access) {
								return access.store;
							}));
					if (!planned_range.absolute_address)
						m_result.entry_state_words[planned_range.base_gpr] |= 0x3;
				}
				m_result.memory_preflight_ranges = static_cast<u32>(
					m_memory_plan.counted_ranges.size() +
					m_memory_plan.bounded_ranges.size());
				if (!m_memory_plan.counted_ranges.empty())
				{
					const RegionMemoryPlan::CountedRange& range =
						m_memory_plan.counted_ranges.front();
					m_result.entry_range_induction_gpr = range.induction_gpr;
					m_result.entry_range_bound_gpr =
						m_memory_plan.control.bound_gpr;
					m_result.entry_range_stride_bytes = range.stride;
				}
				// The range guard reads the complete low 64-bit values directly
				// from canonical state, independently of allocator demand.
			}

			for (size_t slot = 0; slot < RegionExecution::STATE_SLOT_COUNT; slot++)
			{
				const u8 valid_words = RegionExecution::StateWordCount(slot);
				const u8 valid_mask = static_cast<u8>((1u << valid_words) - 1u);
				m_result.entry_state_words[slot] &= valid_mask;
				m_result.output_state_words[slot] &= valid_mask;
				m_result.entry_state_word_count +=
					WordCount(m_result.entry_state_words[slot]);
				m_result.output_state_word_count +=
					WordCount(m_result.output_state_words[slot]);
			}
		}

			const RegionAllocation::Location& Location(ValueId value) const
			{
				static const RegionAllocation::Location none{};
				return value < m_allocation.value_locations.size() ?
					m_allocation.value_locations[value] : none;
			}

			bool ScalarVfpLane(const RegionAllocation::Location& location,
				u8 lane, unsigned* scalar_s) const
			{
				if (!scalar_s || lane >= 4 ||
					location.kind != RegionAllocation::LocationKind::NeonQ)
				{
					return false;
				}
				const unsigned physical_q = m_remap_allocated_neon_high_bank &&
						location.index >= m_neon_remap_first_q &&
						location.index < m_neon_remap_first_q + m_neon_remap_q_count ?
						m_neon_remap_physical_first_q + location.index -
							m_neon_remap_first_q : location.index;
				if (physical_q >= 8)
					return false;
				*scalar_s = physical_q * 4 + lane;
				return true;
			}

			bool DirectVu0ArithmeticLane(ValueId value, u8 lane,
				unsigned* source_s)
			{
				if (!source_s || lane >= 4)
					return false;
				bool folded_broadcast = false;
				if (value < m_allocation.vu0_folded_broadcast_source.size() &&
					m_allocation.vu0_folded_broadcast_source[value] != INVALID_VALUE)
				{
					const Node* const broadcast = Definition(value);
					if (!broadcast || broadcast->opcode != Opcode::Vu0BroadcastLane ||
						broadcast->immediate >= 4)
					{
						return false;
					}
					value = m_allocation.vu0_folded_broadcast_source[value];
					lane = static_cast<u8>(broadcast->immediate);
					folded_broadcast = true;
					m_result.vu0_folded_broadcast_lane_mask |=
						static_cast<u8>(1u << lane);
				}
				const bool direct = ScalarVfpLane(Location(value), lane, source_s);
				if (direct && folded_broadcast)
					m_result.vu0_folded_broadcast_source_s_mask |= 1u << *source_s;
				return direct;
			}

			struct DirectVu0FusedAddLanes
			{
				u8 demand = 0;
				bool direct_target = false;
				unsigned target_q = 0;
				ValueId product_spill_source = INVALID_VALUE;
				std::array<unsigned, 4> left{};
				std::array<unsigned, 4> right{};
				std::array<unsigned, 4> addend{};
				std::array<unsigned, 4> destination{};
			};

			struct CachedVu0FusedBroadcastLanes
			{
				u8 demand = 0;
				ValueId broadcast_source = INVALID_VALUE;
				u8 broadcast_lane = 0;
				unsigned broadcast_s = VFP_SCRATCH1;
				bool load_broadcast = true;
				bool direct_target = false;
				unsigned target_q = 0;
				std::array<unsigned, 4> vector{};
				std::array<unsigned, 4> addend{};
				std::array<unsigned, 4> destination{};
			};

			bool ResolveDirectVu0FusedAdd(const Node& node,
				DirectVu0FusedAddLanes* lanes)
			{
				// q7 aliases s28-s31. It is private scratch only when the complete
				// allocation is vector-only; a mixed COP1/VU0 region may own s28-s29.
				if (!lanes || !m_vu0_product_scratch_available ||
					node.opcode != Opcode::Vu0AddRaw || node.operand_count != 2 ||
					node.type != ValueType::VuF32x4Bits ||
					node.id >= m_allocation.vu0_add_fused_mul.size())
				{
					return false;
				}
				const ValueId multiply_id = m_allocation.vu0_add_fused_mul[node.id];
				const Node* const multiply = Definition(multiply_id);
				if (multiply_id == INVALID_VALUE || !multiply ||
					multiply->opcode != Opcode::Vu0MulRaw ||
					multiply->operand_count != 2)
				{
					return false;
				}
				const ValueId addend = node.operands[0] == multiply_id ?
					node.operands[1] : node.operands[0];
				const RegionAllocation::Location& target = Location(node.id);
				lanes->demand = LocationMask(target);
				unsigned target_lane0 = 0;
				lanes->direct_target = ScalarVfpLane(target, 0, &target_lane0);
				if (lanes->demand == 0 || (!lanes->direct_target &&
					(target.kind != RegionAllocation::LocationKind::Spill ||
					 target.words != 4 || lanes->demand != 0x0f)))
				{
					return false;
				}
				lanes->target_q = lanes->direct_target ? target.index : 7;
				bool left_direct = true;
				bool right_direct = true;
				bool addend_direct = true;
				for (u8 lane = 0; lane < 4; lane++)
				{
					if ((lanes->demand & (1u << lane)) == 0)
						continue;
					left_direct &= DirectVu0ArithmeticLane(multiply->operands[0], lane,
						&lanes->left[lane]);
					right_direct &= DirectVu0ArithmeticLane(multiply->operands[1], lane,
						&lanes->right[lane]);
					addend_direct &= DirectVu0ArithmeticLane(addend, lane,
						&lanes->addend[lane]);
					if (lanes->direct_target)
					{
						if (!ScalarVfpLane(target, lane, &lanes->destination[lane]))
							return false;
					}
					else
					{
						lanes->destination[lane] = 28 + lane;
					}
				}
				auto full_spill = [&](ValueId value) {
					const RegionAllocation::Location& location = StorageLocation(value);
					return location.kind == RegionAllocation::LocationKind::Spill &&
						location.words == 4 && LocationMask(Location(value)) == 0x0f;
				};
				const bool load_left = !left_direct && right_direct && addend_direct &&
					full_spill(multiply->operands[0]);
				const bool load_right = left_direct && !right_direct && addend_direct &&
					full_spill(multiply->operands[1]);
				if (!(left_direct && right_direct && addend_direct) &&
					!load_left && !load_right)
				{
					return false;
				}
				if (load_left || load_right)
				{
					lanes->product_spill_source = load_left ?
						multiply->operands[0] : multiply->operands[1];
					for (u8 lane = 0; lane < 4; lane++)
					{
						if (load_left)
							lanes->left[lane] = 28 + lane;
						else
							lanes->right[lane] = 28 + lane;
					}
				}
				return true;
			}

			bool EmitDirectVu0FusedProducts(const DirectVu0FusedAddLanes& lanes)
			{
				if (lanes.product_spill_source != INVALID_VALUE)
				{
					const RegionAllocation::Location& spill = StorageLocation(
						lanes.product_spill_source);
					if (!EmitQAddress(TEMP2, spill.index) ||
						!m_code.EmitVld1Q32(7, TEMP2))
					{
						return false;
					}
					m_result.spill_vector_loads++;
				}
				for (u8 lane = 0; lane < 4; lane++)
				{
					if ((lanes.demand & (1u << lane)) != 0 &&
						!m_code.EmitVmulF32(28 + lane, lanes.left[lane],
							lanes.right[lane]))
					{
						return false;
					}
				}
				return true;
			}

			bool EmitDirectVu0FusedSums(const Node& node,
				const DirectVu0FusedAddLanes& lanes)
			{
				for (u8 lane = 0; lane < 4; lane++)
				{
					if ((lanes.demand & (1u << lane)) == 0)
						continue;
					if (!m_code.EmitVaddF32(lanes.destination[lane],
							lanes.addend[lane], 28 + lane))
					{
						return false;
					}
					m_result.vu0_direct_vfp_lanes++;
					m_result.vu0_fused_madd_lanes++;
				}
				if (!lanes.direct_target ||
					lanes.product_spill_source != INVALID_VALUE)
					m_result.vu0_spilled_quad_arithmetic++;
				return CommitVector(node.id, lanes.target_q);
			}

			bool ResolveCachedVu0FusedBroadcast(const Node& node,
				const Node& multiply, ValueId addend,
				CachedVu0FusedBroadcastLanes* lanes)
			{
				if (!lanes || multiply.operand_count != 2)
					return false;
				auto folded_source = [&](ValueId value, ValueId* source,
					u8* lane) {
					if (!source || !lane ||
						value >= m_allocation.vu0_folded_broadcast_source.size() ||
						m_allocation.vu0_folded_broadcast_source[value] == INVALID_VALUE)
					{
						return false;
					}
					const Node* const broadcast = Definition(value);
					if (!broadcast || broadcast->opcode != Opcode::Vu0BroadcastLane ||
						broadcast->immediate >= 4)
					{
						return false;
					}
					*source = m_allocation.vu0_folded_broadcast_source[value];
					*lane = static_cast<u8>(broadcast->immediate);
					return true;
				};

				ValueId left_source = INVALID_VALUE;
				ValueId right_source = INVALID_VALUE;
				u8 left_lane = 0;
				u8 right_lane = 0;
				const bool left_folded = folded_source(multiply.operands[0],
					&left_source, &left_lane);
				const bool right_folded = folded_source(multiply.operands[1],
					&right_source, &right_lane);
				if (left_folded == right_folded)
					return false;
				lanes->broadcast_source = left_folded ? left_source : right_source;
				lanes->broadcast_lane = left_folded ? left_lane : right_lane;
				const ValueId vector = left_folded ?
					multiply.operands[1] : multiply.operands[0];

				const RegionAllocation::Location& target = Location(node.id);
				lanes->demand = LocationMask(target);
				lanes->direct_target = target.kind ==
					RegionAllocation::LocationKind::NeonQ;
				lanes->target_q = target.index;
				if (lanes->demand == 0 || (!lanes->direct_target &&
					(target.kind != RegionAllocation::LocationKind::Spill ||
					 target.words != 4)))
				{
					return false;
				}

				unsigned direct_broadcast = 0;
				lanes->load_broadcast = !ScalarVfpLane(
					StorageLocation(lanes->broadcast_source),
					lanes->broadcast_lane, &direct_broadcast);
				lanes->broadcast_s = lanes->load_broadcast ?
					VFP_SCRATCH1 : direct_broadcast;
				for (u8 lane = 0; lane < 4; lane++)
				{
					if ((lanes->demand & (1u << lane)) == 0)
						continue;
					if (!DirectVu0ArithmeticLane(vector, lane,
							&lanes->vector[lane]) ||
						!DirectVu0ArithmeticLane(addend, lane,
							&lanes->addend[lane]))
					{
						return false;
					}
					if (lanes->direct_target)
					{
						if (!ScalarVfpLane(target, lane, &lanes->destination[lane]))
							return false;
						lanes->load_broadcast |=
							lanes->destination[lane] == direct_broadcast;
					}
				}
				if (lanes->load_broadcast)
					lanes->broadcast_s = VFP_SCRATCH1;
				m_result.vu0_folded_broadcast_lane_mask |=
					static_cast<u8>(1u << lanes->broadcast_lane);
				return true;
			}

			bool EmitCachedVu0FusedBroadcast(const Node& node,
				const CachedVu0FusedBroadcastLanes& lanes)
			{
				if (!m_remap_allocated_neon_high_bank)
				{
					// Preserve the folded scalar in a core register, duplicate it into
					// private q7, and issue all four independently rounded products
					// before their dependent adds. This keeps the exact non-contracted
					// VU rounding points while covering Cortex-A9 VFP latency even when
					// the source vector itself was selected for spilling.
					if (!EmitLoadValueWord(lanes.broadcast_source,
							lanes.broadcast_lane, TEMP0) ||
						!m_code.EmitVdupI32QFromCore(7, TEMP0))
					{
						return false;
					}
					for (u8 lane = 0; lane < 4; lane++)
					{
						if ((lanes.demand & (1u << lane)) != 0 &&
							!m_code.EmitVmulF32(28 + lane,
								lanes.vector[lane], 28 + lane))
						{
							return false;
						}
					}
					for (u8 lane = 0; lane < 4; lane++)
					{
						if ((lanes.demand & (1u << lane)) == 0)
							continue;
						const unsigned destination = lanes.direct_target ?
							lanes.destination[lane] : 28 + lane;
						if (!m_code.EmitVaddF32(destination, lanes.addend[lane],
								28 + lane) ||
							(!lanes.direct_target &&
							 (!m_code.EmitVmovSToCore(TEMP0, 28 + lane) ||
							  !CommitValueWord(node.id, lane, TEMP0))))
						{
							return false;
						}
						m_result.vu0_fused_madd_lanes++;
					}
					return !lanes.direct_target || CommitVector(node.id, lanes.target_q);
				}
				if (lanes.load_broadcast &&
					(!EmitLoadValueWord(lanes.broadcast_source,
						lanes.broadcast_lane, TEMP0) ||
					 !m_code.EmitVmovCoreToS(VFP_SCRATCH1, TEMP0)))
				{
					return false;
				}
				for (u8 lane = 0; lane < 4; lane++)
				{
					if ((lanes.demand & (1u << lane)) == 0)
						continue;
					const unsigned destination = lanes.direct_target ?
						lanes.destination[lane] : VFP_SCRATCH0;
					if (!m_code.EmitVmulF32(VFP_SCRATCH0,
							lanes.vector[lane], lanes.broadcast_s) ||
						!m_code.EmitVaddF32(destination, lanes.addend[lane],
							VFP_SCRATCH0) ||
						(!lanes.direct_target &&
						 (!m_code.EmitVmovSToCore(TEMP0, VFP_SCRATCH0) ||
						  !CommitValueWord(node.id, lane, TEMP0))))
					{
						return false;
					}
					m_result.vu0_fused_madd_lanes++;
				}
				return !lanes.direct_target || CommitVector(node.id, lanes.target_q);
			}

			bool AcquireVu0ArithmeticLane(ValueId value, u8 lane,
				unsigned scratch_s, unsigned* source_s)
			{
				if (!source_s || lane >= 4)
					return false;
				if (DirectVu0ArithmeticLane(value, lane, source_s))
					return true;
				if (value < m_allocation.vu0_folded_broadcast_source.size() &&
					m_allocation.vu0_folded_broadcast_source[value] != INVALID_VALUE)
				{
					const Node* const broadcast = Definition(value);
					if (!broadcast || broadcast->opcode != Opcode::Vu0BroadcastLane ||
						broadcast->immediate >= 4)
					{
						return false;
					}
					value = m_allocation.vu0_folded_broadcast_source[value];
					lane = static_cast<u8>(broadcast->immediate);
				}
				*source_s = scratch_s;
				return EmitLoadValueWord(value, lane, TEMP0) &&
				       m_code.EmitVmovCoreToS(scratch_s, TEMP0);
			}

			const Node* Definition(ValueId value) const
			{
				return value < m_definitions.size() ? m_definitions[value] : nullptr;
			}

			ValueId StorageValue(ValueId value) const
			{
				return value < m_allocation.value_aliases.size() &&
					m_allocation.value_aliases[value] != INVALID_VALUE ?
						m_allocation.value_aliases[value] : value;
			}

			const RegionAllocation::Location& StorageLocation(ValueId value) const
			{
				return Location(StorageValue(value));
			}

			u8 RematerializedWordMask(ValueId value) const
			{
				return value < m_allocation.value_rematerialized_word_masks.size() ?
					m_allocation.value_rematerialized_word_masks[value] : 0;
			}

			RegionAllocation::RematerializationKind Rematerialization(ValueId value) const
			{
				return value < m_allocation.value_rematerializations.size() ?
					m_allocation.value_rematerializations[value] :
					RegionAllocation::RematerializationKind::None;
			}

			RegionAllocation::RematerializationKind Low32Extension(ValueId value) const
			{
				return value < m_allocation.value_low32_extensions.size() ?
					m_allocation.value_low32_extensions[value] :
					RegionAllocation::RematerializationKind::None;
			}

			u8 ValueCapabilityMask(ValueId value) const
			{
				return static_cast<u8>(LocationMask(Location(value)) |
					RematerializedWordMask(value));
			}

			bool ImmediateWord(ValueId value, u8 word, u32* immediate) const
			{
				if (!immediate || Location(value).kind !=
						RegionAllocation::LocationKind::Immediate)
				{
					return false;
				}
				const Node* const definition = Definition(StorageValue(value));
				if (!definition || word >= 2)
					return false;
				*immediate = static_cast<u32>(definition->literal >> (word * 32));
				return true;
			}

			u32 CoreRegister(const RegionAllocation::Location& location,
				u8 word) const
			{
				const u8 mask = LocationMask(location);
				const u32 packed = WordRank(mask, word);
				if (location.kind == RegionAllocation::LocationKind::FixedCycle)
					return CYCLE_LOW + location.index + packed;
				const u32 index = location.index + packed;
				if (!Persistent())
					return FIRST_ALLOCATED_CORE + index;
				return m_reclaim_vtlb_host_base ?
					PERSISTENT_MEMORY_FREE_ALLOCATED_CORE[index] :
					m_reclaim_vtlb_vmap ?
					PERSISTENT_IDENTITY_ALLOCATED_CORE[index] :
					PERSISTENT_ALLOCATED_CORE[index];
			}

			u32 SpillOffset(const RegionAllocation::Location& location,
				u8 word) const
			{
				const u8 mask = LocationMask(location);
				const u32 physical_word = location.words == 4 ?
					word : WordRank(mask, word);
				return location.index + physical_word * sizeof(u32);
			}

			PhysicalWord Physical(const RegionAllocation::Location& location,
				u8 word) const
			{
				if ((LocationMask(location) & (1u << word)) == 0)
					return {};
				switch (location.kind)
				{
					case RegionAllocation::LocationKind::Core:
					case RegionAllocation::LocationKind::FixedCycle:
						return {PhysicalWord::Kind::Core,
							CoreRegister(location, word)};
					case RegionAllocation::LocationKind::Spill:
						return {PhysicalWord::Kind::Spill,
							SpillOffset(location, word)};
					case RegionAllocation::LocationKind::VfpS:
						return {PhysicalWord::Kind::VfpS,
							static_cast<u32>(m_allocation.first_vfp_s) +
								location.index};
					case RegionAllocation::LocationKind::NeonQ:
						return {PhysicalWord::Kind::NeonLane,
							location.index * 4u + word};
					case RegionAllocation::LocationKind::None:
					case RegionAllocation::LocationKind::Immediate:
					case RegionAllocation::LocationKind::CanonicalState:
						return {};
				}
				return {};
			}

			PhysicalWord PhysicalValueWord(ValueId value, u8 word) const
			{
				// A rematerialized extension high word reads the stored low word. Hazard
				// analysis must therefore name that low physical word even though the
				// semantic consumer asks for word 1.
				return Physical(Location(value),
					(RematerializedWordMask(value) & (1u << word)) != 0 ? 0 : word);
			}

			bool EmitMove(unsigned destination, unsigned source,
				Condition condition = Condition::AL)
			{
				return destination == source || m_code.EmitMovRegShiftImm(
					destination, source, ShiftType::LSL, 0, false, condition);
			}

			bool EmitSpillAddress(unsigned destination, u32 offset)
			{
				return offset == 0 ? EmitMove(destination, STACK) :
					m_code.EmitAddImm32(destination, STACK, offset);
			}

			bool EmitSpillLoad(unsigned destination, u32 offset)
			{
				if (offset < m_allocation.spill_bytes)
					m_result.spill_word_loads++;
				NoteWorkScratchAccess(offset);
				if (m_options.omit_proven_unused_work_scratch_frame &&
					offset >= m_work_scratch_offset)
				{
					return false;
				}
				if (offset <= 4095)
					return m_code.EmitLdrImm12(destination, STACK, static_cast<u16>(offset));
				const unsigned address = destination == TEMP2 ? TEMP1 : TEMP2;
				return EmitSpillAddress(address, offset) &&
				       m_code.EmitLdrImm12(destination, address, 0);
			}

			bool EmitSpillStore(unsigned source, u32 offset)
			{
				if (offset < m_allocation.spill_bytes)
					m_result.spill_word_stores++;
				NoteWorkScratchAccess(offset);
				if (m_options.omit_proven_unused_work_scratch_frame &&
					offset >= m_work_scratch_offset)
				{
					return false;
				}
				if (offset <= 4095)
					return m_code.EmitStrImm12(source, STACK, static_cast<u16>(offset));
				const unsigned address = source == TEMP2 ? TEMP1 : TEMP2;
				return EmitSpillAddress(address, offset) &&
				       m_code.EmitStrImm12(source, address, 0);
			}

			void NoteWorkScratchAccess(u32 offset)
			{
				if (offset >= m_work_scratch_offset &&
					offset < m_work_scratch_offset + m_work_scratch_bytes)
				{
					m_work_scratch_used = true;
				}
			}

			bool EmitFrameAdjustment(bool enter)
			{
				if (m_frame_bytes == 0)
					return true;
				const size_t offset = m_code.Size();
				bool emitted = true;
				if (enter)
				{
					emitted = m_code.EmitSubImm32(STACK, STACK, m_frame_bytes) &&
						(!m_reclaim_vtlb_vmap || EmitSpillStore(VTLB_VMAP,
							m_saved_vtlb_vmap_offset));
				}
				else
				{
					emitted = (!m_reclaim_vtlb_vmap || EmitSpillLoad(VTLB_VMAP,
							m_saved_vtlb_vmap_offset)) &&
						(!m_reclaim_vtlb_host_base ||
						 m_code.EmitMovImm32(VTLB_HOST_BASE, static_cast<u32>(
							reinterpret_cast<uptr>(m_options.persistent_dispatch->
								host_memory_base)))) &&
						m_code.EmitAddImm32(STACK, STACK, m_frame_bytes);
				}
				if (!emitted)
					return false;
				// A spill-free region initially reserves only the 16-byte work area.
				// Remember its one-instruction adjustments so they can become NOPs if
				// emission proves that no lowering actually used the area.
				if (m_work_scratch_bytes == 16 && m_allocation.spill_bytes == 0 &&
					!m_reclaim_vtlb_vmap &&
					!m_reclaim_vtlb_host_base && !m_use_compact_persistent_exits)
				{
					if (m_frame_bytes != 16 || m_code.Size() != offset + sizeof(u32))
						return false;
					m_optional_frame_adjustments.push_back(offset);
				}
				return true;
			}

			bool FinalizeFrame()
			{
				if (m_allocation.spill_bytes != 0 || m_work_scratch_used ||
					m_reclaim_vtlb_vmap || m_reclaim_vtlb_host_base ||
					m_use_compact_persistent_exits)
					return true;
				constexpr u32 A32_NOP = 0xe1a00000u; // mov r0, r0
				for (const size_t offset : m_optional_frame_adjustments)
				{
					if (!m_code.PatchInstruction(offset, A32_NOP))
						return false;
				}
				m_frame_bytes = 0;
				return true;
			}

			bool EmitQAddress(unsigned destination, u32 offset)
			{
				return EmitSpillAddress(destination, offset);
			}

			bool EmitLoadLocationWord(const RegionAllocation::Location& location,
				ValueId value, u8 word, unsigned destination)
			{
				if ((LocationMask(location) & (1u << word)) == 0)
					return false;
				switch (location.kind)
				{
					case RegionAllocation::LocationKind::Immediate:
					{
						const Node* node = Definition(StorageValue(value));
						if (!node)
							return false;
						const u32 literal = word < 2 ?
							static_cast<u32>(node->literal >> (word * 32)) : 0;
						return m_code.EmitMovImm32(destination, literal);
					}
					case RegionAllocation::LocationKind::CanonicalState:
					{
						const size_t offset =
							RegionExecution::CanonicalStateWordOffset(
								location.index, word);
						return offset != SIZE_MAX &&
							EmitStateLoad(static_cast<u32>(offset), destination);
					}
					case RegionAllocation::LocationKind::Core:
					case RegionAllocation::LocationKind::FixedCycle:
						return EmitMove(destination, CoreRegister(location, word));
					case RegionAllocation::LocationKind::Spill:
						return EmitSpillLoad(destination, SpillOffset(location, word));
					case RegionAllocation::LocationKind::VfpS:
						return m_code.EmitVmovSToCore(destination,
							m_allocation.first_vfp_s + location.index);
					case RegionAllocation::LocationKind::NeonQ:
						return m_code.EmitVmovD32LaneToCore(destination,
							location.index * 2 + word / 2, word & 1u);
					case RegionAllocation::LocationKind::None:
						return false;
				}
				return false;
			}

			bool EmitStoreLocationWord(const RegionAllocation::Location& location,
				u8 word, unsigned source)
			{
				if ((LocationMask(location) & (1u << word)) == 0)
					return false;
				switch (location.kind)
				{
					case RegionAllocation::LocationKind::Core:
					case RegionAllocation::LocationKind::FixedCycle:
						return EmitMove(CoreRegister(location, word), source);
					case RegionAllocation::LocationKind::Spill:
						return EmitSpillStore(source, SpillOffset(location, word));
					case RegionAllocation::LocationKind::VfpS:
						return m_code.EmitVmovCoreToS(
							m_allocation.first_vfp_s + location.index, source);
					case RegionAllocation::LocationKind::NeonQ:
						return m_code.EmitVmovCoreToD32Lane(
							location.index * 2 + word / 2, word & 1u, source);
					case RegionAllocation::LocationKind::None:
					case RegionAllocation::LocationKind::Immediate:
					case RegionAllocation::LocationKind::CanonicalState:
						return false;
				}
				return false;
			}

			bool RecordOperandFailure(ValueId value)
			{
				m_result.failure_operand_value = value;
				const RegionAllocation::Location& location = Location(value);
				m_result.failure_operand_location_kind =
					static_cast<u8>(location.kind);
				m_result.failure_operand_word_mask = LocationMask(location);
				m_result.failure_operand_folded =
					value < m_allocation.folded_effective_addresses.size() ?
						m_allocation.folded_effective_addresses[value] : 0;
				return false;
			}

			bool EmitLoadValueWord(ValueId value, u8 word, unsigned destination)
			{
				if (value >= m_program.value_count)
					return false;
				if ((RematerializedWordMask(value) & (1u << word)) != 0)
				{
					const RegionAllocation::RematerializationKind kind =
						Rematerialization(value);
					if (word != 1 || kind ==
						RegionAllocation::RematerializationKind::None)
					{
						return false;
					}
					if (kind == RegionAllocation::RematerializationKind::ZeroExtendLow32)
						return m_code.EmitMovImm8(destination, 0);
					return EmitLoadLocationWord(Location(value), value, 0, destination) &&
					       m_code.EmitMovRegShiftImm(destination, destination,
						   ShiftType::ASR, 31);
				}
				if (word == 0 &&
					value < m_allocation.folded_effective_addresses.size() &&
					m_allocation.folded_effective_addresses[value] != 0)
				{
					const Node* const address = Definition(value);
					if (!address || address->opcode != Opcode::EffectiveAddress32 ||
						address->operand_count != 2)
					{
						return false;
					}
					u32 immediate = 0;
					if (ImmediateWord(address->operands[1], 0, &immediate))
					{
						unsigned base = 0;
						if (!AcquireValueWord(address->operands[0], 0,
								destination, &base))
						{
							return RecordOperandFailure(address->operands[0]);
						}
						if (m_code.EmitAddImm32(destination, base, immediate))
							return true;
						// A32 data-processing immediates use the rotated-eight-bit
						// encoding.  Effective-address offsets are arbitrary signed 16-bit
						// values, so materialize the uncommon non-encodable constant in the
						// remaining reserved scratch register rather than rejecting the
						// region. EmitAddImm32() is side-effect free when encoding fails.
						unsigned constant = TEMP2;
						if (constant == destination || constant == base)
							constant = TEMP1;
						if (constant == destination || constant == base)
							constant = TEMP0;
						return constant != destination && constant != base &&
						       m_code.EmitMovImm32(constant, immediate) &&
						       m_code.EmitAddReg(destination, base, constant);
					}
					if (ImmediateWord(address->operands[0], 0, &immediate))
					{
						unsigned base = 0;
						if (!AcquireValueWord(address->operands[1], 0,
								destination, &base))
						{
							return RecordOperandFailure(address->operands[1]);
						}
						if (m_code.EmitAddImm32(destination, base, immediate))
							return true;
						unsigned constant = TEMP2;
						if (constant == destination || constant == base)
							constant = TEMP1;
						if (constant == destination || constant == base)
							constant = TEMP0;
						return constant != destination && constant != base &&
						       m_code.EmitMovImm32(constant, immediate) &&
						       m_code.EmitAddReg(destination, base, constant);
					}
					const unsigned other_scratch =
						destination == TEMP1 ? TEMP2 : TEMP1;
					unsigned left = 0;
					unsigned right = 0;
					if (!AcquireValueWord(address->operands[0], 0, destination,
							&left))
					{
						return RecordOperandFailure(address->operands[0]);
					}
					if (!AcquireValueWord(address->operands[1], 0, other_scratch,
							&right))
					{
						return RecordOperandFailure(address->operands[1]);
					}
					return m_code.EmitAddReg(destination, left, right);
				}
				const ValueId storage = StorageValue(value);
				return EmitLoadLocationWord(Location(storage), storage, word, destination);
			}

			bool EmitStoreValueWord(ValueId value, u8 word, unsigned source)
			{
				return value < m_program.value_count &&
				       EmitStoreLocationWord(Location(value), word, source);
			}

			unsigned VfpRegister(const RegionAllocation::Location& location) const
			{
				return m_allocation.first_vfp_s + location.index;
			}

			bool EmitVfpSpillLoad(unsigned destination, u32 offset)
			{
				if (offset < m_allocation.spill_bytes)
					m_result.spill_vfp_loads++;
				if (offset <= 0x3fcu)
					return m_code.EmitVldrSImm(destination, STACK,
						static_cast<u16>(offset));
				return EmitSpillAddress(TEMP2, offset) &&
				       m_code.EmitVldrSImm(destination, TEMP2, 0);
			}

			bool EmitVfpSpillStore(unsigned source, u32 offset)
			{
				if (offset < m_allocation.spill_bytes)
					m_result.spill_vfp_stores++;
				if (offset <= 0x3fcu)
					return m_code.EmitVstrSImm(source, STACK,
						static_cast<u16>(offset));
				return EmitSpillAddress(TEMP2, offset) &&
				       m_code.EmitVstrSImm(source, TEMP2, 0);
			}

			bool AcquireVfp(ValueId value, unsigned scratch, unsigned* host)
			{
				if (!host || (scratch != VFP_SCRATCH0 && scratch != VFP_SCRATCH1))
					return false;
				const RegionAllocation::Location& location = Location(value);
				if (location.kind == RegionAllocation::LocationKind::VfpS)
				{
					*host = VfpRegister(location);
					return true;
				}
				*host = scratch;
				if (location.kind == RegionAllocation::LocationKind::Spill)
					return EmitVfpSpillLoad(scratch, SpillOffset(location, 0));
				return EmitLoadValueWord(value, 0, TEMP0) &&
				       m_code.EmitVmovCoreToS(scratch, TEMP0);
			}

			bool CommitVfp(ValueId value, unsigned source)
			{
				const RegionAllocation::Location& location = Location(value);
				if (location.kind == RegionAllocation::LocationKind::VfpS)
					return source == VfpRegister(location) ||
					       m_code.EmitVmovS(VfpRegister(location), source);
				if (location.kind == RegionAllocation::LocationKind::Spill)
					return EmitVfpSpillStore(source, SpillOffset(location, 0));
				return m_code.EmitVmovSToCore(TEMP0, source) &&
				       EmitStoreValueWord(value, 0, TEMP0);
			}

			bool CommitCop1Bits(ValueId value, unsigned source)
			{
				const RegionAllocation::Location& location = Location(value);
				if (location.kind == RegionAllocation::LocationKind::VfpS)
					return m_code.EmitVmovCoreToS(VfpRegister(location), source);
				if (location.kind == RegionAllocation::LocationKind::Spill)
				{
					return m_code.EmitVmovCoreToS(VFP_SCRATCH0, source) &&
					       EmitVfpSpillStore(VFP_SCRATCH0, SpillOffset(location, 0));
				}
				return EmitStoreValueWord(value, 0, source);
			}

			void BuildCop1LoweringPlan()
			{
				m_cop1_normalized.assign(m_program.value_count, false);
				m_cop1_fused_flag_for_clamp =
					m_allocation.cop1_fused_flag_for_clamp;
				m_cop1_fused_clamp_for_flag =
					m_allocation.cop1_fused_clamp_for_flag;

				// These facts describe raw PS2 FPR words, not host VFP modes.  Each
				// producer below is normalized for every input under the verified IR
				// semantics.  A fixed point leaves entry parameters and raw memory/MTC1
				// words unknown, so an unsafe external seed can never skip fpuDouble().
				bool changed = true;
				while (changed)
				{
					changed = false;
					for (const Block& block : m_program.blocks)
					{
						for (const Node& node : block.nodes)
						{
							bool normalized = false;
							switch (node.opcode)
							{
								case Opcode::Cop1NormalizeInput:
								case Opcode::Cop1ClampOuResult:
								case Opcode::Cop1ConvertSingle:
									normalized = true;
									break;
								default:
									break;
							}
							if (normalized && !m_cop1_normalized[node.id])
							{
								m_cop1_normalized[node.id] = true;
								changed = true;
							}
						}
					}
				}

			}

			bool EmitCop1NormalizeInput(const Node& node)
			{
				const ValueId alias = node.id < m_allocation.cop1_normalize_alias.size() ?
					m_allocation.cop1_normalize_alias[node.id] : INVALID_VALUE;
				if (alias != INVALID_VALUE)
				{
					m_result.cop1_normalize_elided++;
					unsigned source = 0;
					return AcquireVfp(alias, VFP_SCRATCH0, &source) &&
					       CommitVfp(node.id, source);
				}
				if (node.operand_count == 1 &&
					node.operands[0] < m_cop1_normalized.size() &&
					m_cop1_normalized[node.operands[0]])
				{
					m_result.cop1_normalize_elided++;
					unsigned source = 0;
					return AcquireVfp(node.operands[0], VFP_SCRATCH0, &source) &&
					       CommitVfp(node.id, source);
				}
				m_result.cop1_normalize_emitted++;
				if (node.operand_count != 1 || node.type != ValueType::F32Bits ||
					!EmitLoadValueWord(node.operands[0], 0, TEMP0) ||
					!m_code.EmitMovRegShiftImm(TEMP1, TEMP0, ShiftType::LSL, 1) ||
					!m_code.EmitSubImm32(TEMP1, TEMP1, 0x01000000u) ||
					!m_code.EmitCmpImm32(TEMP1, 0xfe000000u))
				{
					return false;
				}
				// After subtracting the smallest normal magnitude, every ordinary
				// finite word lies below 0xfe000000. Exponent-zero words wrap above
				// 0xff000000 and exponent-255 words occupy 0xfe000000..0xfefffffe.
				// One unsigned range test therefore sends both PS2 edge classes to
				// one shared cold leaf without weakening fpuDouble() semantics.
				const size_t edge_call =
					m_code.EmitBranchLinkPlaceholder(Condition::CS);
				if (edge_call == static_cast<size_t>(-1))
				{
					return false;
				}
				if (!CommitCop1Bits(node.id, TEMP0))
				{
					return false;
				}
				m_cop1_normalize_edge_calls.push_back(edge_call);
				return true;
			}

			bool EmitHoistedCop1Normalizers()
			{
				for (ValueId value = 0;
					value < m_allocation.cop1_hoisted_normalize.size(); value++)
				{
					if (m_allocation.cop1_hoisted_normalize[value] == 0)
						continue;
					const Node* const node = Definition(value);
					if (!node || node->opcode != Opcode::Cop1NormalizeInput)
						return false;
					const size_t before = m_code.Size();
					if (!EmitCop1NormalizeInput(*node))
						return false;
					m_result.cop1_normalize_hoisted++;
					const u32 bytes = static_cast<u32>(m_code.Size() - before);
					m_result.cop1_hot_bytes += bytes;
					m_result.cop1_normalize_hot_bytes += bytes;
				}
				return true;
			}

			bool EmitHoistedVu0Normalizers()
			{
				for (ValueId value = 0;
					value < m_allocation.vu0_hoisted_normalize.size(); value++)
				{
					if (m_allocation.vu0_hoisted_normalize[value] == 0)
						continue;
					const Node* const node = Definition(value);
					if (!node || node->opcode != Opcode::Vu0NormalizeVector)
					{
						return false;
					}
					const size_t before = m_code.Size();
					ValueId source = node->operands[0];
					if (source < m_allocation.vu0_idle_alias.size() &&
						m_allocation.vu0_idle_alias[source] != INVALID_VALUE)
					{
						source = m_allocation.vu0_idle_alias[source];
					}
					const Node* const parameter = Definition(source);
					const RegionAllocation::Location& target = Location(value);
					const u8 mask = LocationMask(target);
					if (mask != 0x0f)
					{
						if (!EmitVu0NormalizeVector(*node))
							return false;
						m_result.vu0_normalize_hot_bytes +=
							static_cast<u32>(m_code.Size() - before);
						continue;
					}
					if (!Persistent() || !parameter ||
						parameter->opcode != Opcode::Parameter ||
						parameter->immediate >= RegionExecution::STATE_SLOT_COUNT ||
						(target.kind != RegionAllocation::LocationKind::NeonQ &&
						 target.kind != RegionAllocation::LocationKind::Spill))
					{
						return false;
					}
					const size_t canonical = RegionExecution::CanonicalStateWordOffset(
						parameter->immediate, 0);
					PersistentStateLocation state_location{};
					if (canonical == SIZE_MAX ||
						!PersistentRuntimeLocation(static_cast<u32>(canonical),
							&state_location) || (state_location.offset & 0x0f) != 0)
					{
						return false;
					}
					unsigned base = CPU_REGS;
					if (state_location.base ==
						CompileOptions::PersistentStateBase::Vu0)
					{
						const auto& persistent = *m_options.persistent_dispatch;
						if (!persistent.vu0_state ||
							!m_code.EmitMovImm32(TEMP1, static_cast<u32>(
								reinterpret_cast<uptr>(persistent.vu0_state))))
						{
							return false;
						}
						base = TEMP1;
					}
					const bool address = state_location.offset == 0 ?
						(base == TEMP0 || EmitMove(TEMP0, base)) :
						(m_code.EmitAddImm32(TEMP0, base, state_location.offset) ||
						 (m_code.EmitMovImm32(TEMP0, state_location.offset) &&
						  m_code.EmitAddReg(TEMP0, base, TEMP0)));
					const unsigned output =
						target.kind == RegionAllocation::LocationKind::NeonQ ?
							target.index : VECTOR_SCRATCH0;
					if (!address || !m_code.EmitVld1Q32Aligned(output, TEMP0) ||
						!EmitNormalizeVuQuad(output) || !CommitVector(value, output))
					{
						return false;
					}
					m_result.vu0_normalize_hot_bytes +=
						static_cast<u32>(m_code.Size() - before);
				}
				return true;
			}

			bool EmitCop1ClampAndUpdateOu(const Node& clamp, ValueId flag_value)
			{
				const Node* const flag = flag_value < m_definitions.size() ?
					m_definitions[flag_value] : nullptr;
				if (!flag || clamp.operand_count != 1 ||
					flag->operand_count != 2 || flag->operands[1] != clamp.operands[0] ||
					!EmitLoadValueWord(clamp.operands[0], 0, TEMP1) ||
					!EmitLoadValueWord(flag->operands[0], 0, TEMP0) ||
					!m_code.EmitMovRegShiftImm(TEMP2, TEMP1, ShiftType::LSL, 1) ||
					!m_code.EmitSubImm32(TEMP2, TEMP2, 0x01000000u) ||
					!m_code.EmitCmpImm32(TEMP2, 0xfe000000u))
				{
					return false;
				}
				// A normal result leaves C clear. The cold edge leaf returns with C
				// set after applying the exact exceptional result and flags, allowing
				// one predicated BIC to replace both former hot-path skip branches.
				const size_t edge_call =
					m_code.EmitBranchLinkPlaceholder(Condition::CS);
				if (edge_call == static_cast<size_t>(-1) ||
					!m_code.EmitBicImm32(TEMP0, TEMP0, FCR31_O | FCR31_U,
						false, Condition::CC))
				{
					return false;
				}
				const RegionAllocation::Location& raw_location =
					Location(clamp.operands[0]);
				const RegionAllocation::Location& result_location = Location(clamp.id);
				const bool destructive_vfp_result =
					raw_location.kind == RegionAllocation::LocationKind::VfpS &&
					result_location.kind == RegionAllocation::LocationKind::VfpS &&
					raw_location.index == result_location.index;
				// The shared exceptional leaf returns with C set and TEMP1 holding
				// the corrected signed-zero/max-finite bits. On the ordinary finite
				// path C is clear and a destructive raw/result allocation already
				// contains the exact architectural value, so avoid a serializing
				// ARM-to-VFP transfer there.
				if (!(destructive_vfp_result ?
					m_code.EmitVmovCoreToS(VfpRegister(result_location), TEMP1,
						Condition::CS) :
					CommitCop1Bits(clamp.id, TEMP1)) ||
					!CommitValueWord(flag_value, 0, TEMP0))
				{
					return false;
				}
				m_cop1_ou_edge_calls.push_back(edge_call);
				m_result.cop1_ou_pairs_fused++;
				m_result.cop1_destructive_ou_pairs += destructive_vfp_result ? 1u : 0u;
				return true;
			}

			bool EmitCop1GuardedColdPaths()
			{
				for (const Cop1GuardedColdPath& path : m_cop1_guarded_cold_paths)
				{
					if (path.exceptional_branch == static_cast<size_t>(-1) ||
						path.hot_continuation == static_cast<size_t>(-1) ||
						path.raw == INVALID_VALUE || path.clamp == INVALID_VALUE ||
						path.old_flags == INVALID_VALUE || path.flags == INVALID_VALUE)
					{
						return false;
					}

					const size_t target = m_code.Size();
					if (!m_code.PatchBranch(path.exceptional_branch, target,
							Condition::CS) ||
						!EmitLoadValueWord(path.raw, 0, TEMP1) ||
						!EmitLoadValueWord(path.old_flags, 0, TEMP0))
					{
						return false;
					}

					// Reuse the same PCSX2-exact overflow/underflow leaf as the eager
					// lowering.  Only exceptional values reach this veneer, so the hot
					// path pays neither state materialization nor a private tier-zero
					// suffix.  The leaf returns corrected result bits in r1 and FCR31
					// in r0, with C set.
					const size_t edge_call = m_code.EmitBranchLinkPlaceholder();
					if (edge_call == static_cast<size_t>(-1))
						return false;

					const u8 clamp_demand = path.clamp <
						m_allocation.value_word_demands.size() ?
						m_allocation.value_word_demands[path.clamp] : 0;
					const u8 flag_demand = path.flags <
						m_allocation.value_word_demands.size() ?
						m_allocation.value_word_demands[path.flags] : 0;
					if (((clamp_demand & 0x1) != 0 &&
						 !CommitCop1Bits(path.clamp, TEMP1)) ||
						((flag_demand & 0x1) != 0 &&
						 !CommitValueWord(path.flags, 0, TEMP0)))
					{
						return false;
					}

					const size_t rejoin = m_code.EmitBranchPlaceholder();
					if (rejoin == static_cast<size_t>(-1) ||
						!m_code.PatchBranch(rejoin, path.hot_continuation))
					{
						return false;
					}
					m_cop1_ou_edge_calls.push_back(edge_call);
					m_result.cop1_lazy_exception_leaves++;
				}
				return true;
			}

			bool CompleteCop1GuardedColdPath(ValueId value)
			{
				for (Cop1GuardedColdPath& path : m_cop1_guarded_cold_paths)
				{
					if (path.flags != value)
						continue;
					if (path.hot_continuation != static_cast<size_t>(-1))
						return false;
					path.hot_continuation = m_code.Size();
				}
				return true;
			}

			bool EmitCop1ColdNumericLeaves()
			{
				if (!m_cop1_normalize_edge_calls.empty())
				{
					const size_t target = m_code.Size();
					if (!m_code.EmitVmovCoreToS(VFP_SCRATCH1, TEMP2) ||
						!m_code.EmitMovRegShiftImm(TEMP2, TEMP0, ShiftType::LSL, 1) ||
						!m_code.EmitAndImm32(TEMP0, TEMP0, COP1_SIGN) ||
						!m_code.EmitCmpImm32(TEMP2, 0xff000000u))
					{
						return false;
					}
					const size_t exponent_zero =
						m_code.EmitBranchPlaceholder(Condition::CC);
					if (exponent_zero == static_cast<size_t>(-1) ||
						!m_code.EmitMovImm32(TEMP2, COP1_EXPONENT - 1) ||
						!m_code.EmitOrrReg(TEMP0, TEMP0, TEMP2))
					{
						return false;
					}
					const size_t return_target = m_code.Size();
					if (!m_code.PatchBranch(exponent_zero, return_target, Condition::CC) ||
						!m_code.EmitCmpReg(TEMP0, TEMP0) ||
						!m_code.EmitVmovSToCore(TEMP2, VFP_SCRATCH1) ||
						!m_code.EmitBx(TEMP2))
					{
						return false;
					}
					for (const size_t call : m_cop1_normalize_edge_calls)
					{
						if (!m_code.PatchBranchLink(call, target, Condition::CS))
							return false;
					}
				}

				if (!m_cop1_ou_edge_calls.empty())
				{
					const size_t target = m_code.Size();
					if (!m_code.EmitVmovCoreToS(VFP_SCRATCH1, TEMP2) ||
						!m_code.EmitMovRegShiftImm(TEMP2, TEMP1, ShiftType::LSL, 1) ||
						!m_code.EmitCmpImm32(TEMP2, 0xff000000u))
					{
						return false;
					}
					const size_t exponent_zero =
						m_code.EmitBranchPlaceholder(Condition::CC);
					if (exponent_zero == static_cast<size_t>(-1) ||
						!m_code.EmitAndImm32(TEMP1, TEMP1, COP1_SIGN) ||
						!m_code.EmitMovImm32(TEMP2, COP1_EXPONENT - 1) ||
						!m_code.EmitOrrReg(TEMP1, TEMP1, TEMP2) ||
						!m_code.EmitOrrImm32(TEMP0, TEMP0, FCR31_O) ||
						!m_code.EmitOrrImm32(TEMP0, TEMP0, FCR31_SO))
					{
						return false;
					}
					const size_t done_from_overflow = m_code.EmitBranchPlaceholder();
					const size_t exponent_zero_target = m_code.Size();
					if (done_from_overflow == static_cast<size_t>(-1) ||
						!m_code.PatchBranch(exponent_zero, exponent_zero_target,
							Condition::CC) ||
						!m_code.EmitBicImm32(TEMP2, TEMP1, COP1_SIGN) ||
						!m_code.EmitBicImm32(TEMP0, TEMP0, FCR31_O) ||
						!m_code.EmitCmpImm32(TEMP2, 0))
					{
						return false;
					}
					const size_t underflow =
						m_code.EmitBranchPlaceholder(Condition::NE);
					if (underflow == static_cast<size_t>(-1) ||
						!m_code.EmitBicImm32(TEMP0, TEMP0, FCR31_U))
					{
						return false;
					}
					const size_t done_from_zero = m_code.EmitBranchPlaceholder();
					if (done_from_zero == static_cast<size_t>(-1) ||
						!m_code.PatchBranch(underflow, m_code.Size(), Condition::NE) ||
						!m_code.EmitAndImm32(TEMP1, TEMP1, COP1_SIGN) ||
						!m_code.EmitOrrImm32(TEMP0, TEMP0, FCR31_U) ||
						!m_code.EmitOrrImm32(TEMP0, TEMP0, FCR31_SU))
					{
						return false;
					}
					const size_t return_condition = m_code.Size();
					if (!m_code.PatchBranch(done_from_overflow, return_condition) ||
						!m_code.PatchBranch(done_from_zero, return_condition) ||
						!m_code.EmitCmpReg(TEMP0, TEMP0) ||
						!m_code.EmitVmovSToCore(TEMP2, VFP_SCRATCH1) ||
						!m_code.EmitBx(TEMP2))
					{
						return false;
					}
					for (const size_t call : m_cop1_ou_edge_calls)
					{
						if (!m_code.PatchBranchLink(call, target, Condition::CS))
							return false;
					}
				}
				return true;
			}

			bool EmitCop1RawArithmetic(const Node& node)
			{
				if (node.operand_count != 2 || node.type != ValueType::F32Bits)
					return false;
				unsigned left = 0;
				unsigned right = 0;
				if (!AcquireVfp(node.operands[0], VFP_SCRATCH0, &left) ||
					!AcquireVfp(node.operands[1], VFP_SCRATCH1, &right))
				{
					return false;
				}
				const RegionAllocation::Location& target = Location(node.id);
				const unsigned output =
					target.kind == RegionAllocation::LocationKind::VfpS ?
						VfpRegister(target) : VFP_SCRATCH0;
				bool emitted = false;
				switch (node.opcode)
				{
					case Opcode::Cop1AddRaw:
						emitted = m_code.EmitVaddF32(output, left, right);
						break;
					case Opcode::Cop1SubRaw:
						emitted = m_code.EmitVsubF32(output, left, right);
						break;
					case Opcode::Cop1MulRaw:
						emitted = m_code.EmitVmulF32(output, left, right);
						break;
					default:
						return false;
				}
				return emitted && CommitVfp(node.id, output);
			}

			bool EmitCop1ClampOuResult(const Node& node)
			{
				if (node.id < m_cop1_fused_flag_for_clamp.size() &&
					m_cop1_fused_flag_for_clamp[node.id] != INVALID_VALUE)
				{
					return EmitCop1ClampAndUpdateOu(node,
						m_cop1_fused_flag_for_clamp[node.id]);
				}
				if (node.operand_count != 1 || node.type != ValueType::F32Bits ||
					!EmitLoadValueWord(node.operands[0], 0, TEMP0) ||
					!m_code.EmitBicImm32(TEMP1, TEMP0, COP1_SIGN) ||
					!m_code.EmitMovImm32(TEMP2, COP1_EXPONENT) ||
					!m_code.EmitCmpReg(TEMP1, TEMP2))
				{
					return false;
				}
				const size_t no_overflow =
					m_code.EmitBranchPlaceholder(Condition::NE);
				if (no_overflow == static_cast<size_t>(-1) ||
					!m_code.EmitAndImm32(TEMP0, TEMP0, COP1_SIGN) ||
					!m_code.EmitSubImm8(TEMP1, TEMP2, 1) ||
					!m_code.EmitOrrReg(TEMP0, TEMP0, TEMP1))
				{
					return false;
				}
				const size_t done_from_overflow = m_code.EmitBranchPlaceholder();
				if (done_from_overflow == static_cast<size_t>(-1) ||
					!m_code.PatchBranch(no_overflow, m_code.Size(), Condition::NE) ||
					!m_code.EmitAndReg(TEMP1, TEMP0, TEMP2) ||
					!m_code.EmitCmpImm32(TEMP1, 0))
				{
					return false;
				}
				const size_t done_from_exponent =
					m_code.EmitBranchPlaceholder(Condition::NE);
				if (done_from_exponent == static_cast<size_t>(-1) ||
					!m_code.EmitBicImm32(TEMP1, TEMP0, COP1_SIGN) ||
					!m_code.EmitCmpImm32(TEMP1, 0))
				{
					return false;
				}
				const size_t done_from_zero =
					m_code.EmitBranchPlaceholder(Condition::EQ);
				if (done_from_zero == static_cast<size_t>(-1) ||
					!m_code.EmitAndImm32(TEMP0, TEMP0, COP1_SIGN))
				{
					return false;
				}
				const size_t done = m_code.Size();
				return m_code.PatchBranch(done_from_overflow, done) &&
				       m_code.PatchBranch(done_from_exponent, done, Condition::NE) &&
				       m_code.PatchBranch(done_from_zero, done, Condition::EQ) &&
				       CommitCop1Bits(node.id, TEMP0);
			}

			bool EmitCop1UpdateOuFlags(const Node& node)
			{
				if (node.id < m_cop1_fused_clamp_for_flag.size() &&
					m_cop1_fused_clamp_for_flag[node.id] != INVALID_VALUE)
				{
					return true;
				}
				if (node.operand_count == 2 &&
					node.operands[1] <
						m_allocation.cop1_exception_for_guarded_raw.size() &&
					m_allocation.cop1_exception_for_guarded_raw[node.operands[1]] !=
						INVALID_VALUE)
				{
					unsigned old_flags = 0;
					if (!AcquireValueWord(node.operands[0], 0, TEMP0, &old_flags) ||
						!m_code.EmitBicImm32(TEMP0, old_flags, FCR31_O | FCR31_U))
					{
						return false;
					}
					return CommitValueWord(node.id, 0, TEMP0);
				}
				if (node.operand_count != 2 || node.type != ValueType::I32 ||
					!EmitLoadValueWord(node.operands[0], 0, TEMP0) ||
					!EmitLoadValueWord(node.operands[1], 0, TEMP1) ||
					!m_code.EmitBicImm32(TEMP1, TEMP1, COP1_SIGN) ||
					!m_code.EmitMovImm32(TEMP2, COP1_EXPONENT) ||
					!m_code.EmitCmpReg(TEMP1, TEMP2))
				{
					return false;
				}
				const size_t no_overflow =
					m_code.EmitBranchPlaceholder(Condition::NE);
				if (no_overflow == static_cast<size_t>(-1) ||
					!m_code.EmitOrrImm32(TEMP0, TEMP0, FCR31_O) ||
					!m_code.EmitOrrImm32(TEMP0, TEMP0, FCR31_SO))
				{
					return false;
				}
				const size_t done_from_overflow = m_code.EmitBranchPlaceholder();
				if (done_from_overflow == static_cast<size_t>(-1) ||
					!m_code.PatchBranch(no_overflow, m_code.Size(), Condition::NE) ||
					!m_code.EmitBicImm32(TEMP0, TEMP0, FCR31_O) ||
					!m_code.EmitAndReg(TEMP2, TEMP1, TEMP2) ||
					!m_code.EmitCmpImm32(TEMP2, 0))
				{
					return false;
				}
				const size_t normal_exponent =
					m_code.EmitBranchPlaceholder(Condition::NE);
				if (normal_exponent == static_cast<size_t>(-1) ||
					!m_code.EmitCmpImm32(TEMP1, 0))
				{
					return false;
				}
				const size_t exact_zero = m_code.EmitBranchPlaceholder(Condition::EQ);
				if (exact_zero == static_cast<size_t>(-1) ||
					!m_code.EmitOrrImm32(TEMP0, TEMP0, FCR31_U) ||
					!m_code.EmitOrrImm32(TEMP0, TEMP0, FCR31_SU))
				{
					return false;
				}
				const size_t done_from_underflow = m_code.EmitBranchPlaceholder();
				if (done_from_underflow == static_cast<size_t>(-1))
					return false;
				const size_t clear_underflow = m_code.Size();
				if (!m_code.PatchBranch(normal_exponent, clear_underflow, Condition::NE) ||
					!m_code.PatchBranch(exact_zero, clear_underflow, Condition::EQ) ||
					!m_code.EmitBicImm32(TEMP0, TEMP0, FCR31_U))
				{
					return false;
				}
				const size_t done = m_code.Size();
				return m_code.PatchBranch(done_from_overflow, done) &&
				       m_code.PatchBranch(done_from_underflow, done) &&
				       CommitValueWord(node.id, 0, TEMP0);
			}

			bool EmitCop1Compare(const Node& node)
			{
				if (node.operand_count != 2 || node.type != ValueType::I1 ||
					!EmitLoadValueWord(node.operands[0], 0, TEMP0) ||
					!EmitLoadValueWord(node.operands[1], 0, TEMP1))
				{
					return false;
				}

				// NormalizeCop1Input preserves the sign of exponent-zero values.
				// Canonicalize both signed zeros before forming PCSX2's monotonic
				// unsigned compare key so -0 and +0 compare equal.
				auto canonicalize_zero = [&](unsigned value) {
					return m_code.EmitBicImm32(TEMP2, value, COP1_SIGN) &&
					       m_code.EmitCmpImm32(TEMP2, 0) &&
					       m_code.EmitMovImm8(value, 0, Condition::EQ);
				};
				auto make_key = [&](unsigned value) {
					return m_code.EmitMovRegShiftImm(TEMP2, value,
							ShiftType::ASR, 31) &&
					       m_code.EmitOrrImm32(TEMP2, TEMP2, COP1_SIGN) &&
					       m_code.EmitEorReg(value, value, TEMP2);
				};
				if (!canonicalize_zero(TEMP0) || !canonicalize_zero(TEMP1))
					return false;
				Condition condition = Condition::AL;
				switch (node.opcode)
				{
					case Opcode::Cop1CompareEqual:
						condition = Condition::EQ;
						break;
					case Opcode::Cop1CompareLess:
						if (!make_key(TEMP0) || !make_key(TEMP1))
							return false;
						condition = Condition::CC;
						break;
					case Opcode::Cop1CompareLessEqual:
						if (!make_key(TEMP0) || !make_key(TEMP1))
							return false;
						condition = Condition::LS;
						break;
					default:
						return false;
				}
				return m_code.EmitCmpReg(TEMP0, TEMP1) &&
				       m_code.EmitMovImm8(TEMP2, 0) &&
				       m_code.EmitMovImm8(TEMP2, 1, condition) &&
				       CommitValueWord(node.id, 0, TEMP2);
			}

			bool EmitCop1UpdateConditionFlag(const Node& node)
			{
				if (node.operand_count != 2 || node.type != ValueType::I32 ||
					!EmitLoadValueWord(node.operands[0], 0, TEMP0) ||
					!EmitLoadValueWord(node.operands[1], 0, TEMP1) ||
					!m_code.EmitBicImm32(TEMP2, TEMP0, FCR31_C) ||
					!m_code.EmitCmpImm32(TEMP1, 0) ||
					!m_code.EmitOrrImm32(TEMP2, TEMP2, FCR31_C, false,
						Condition::NE))
				{
					return false;
				}
				return CommitValueWord(node.id, 0, TEMP2);
			}

			bool EmitCop1BranchCondition(const Node& node)
			{
				if (node.operand_count != 1 || node.type != ValueType::I1 ||
					node.immediate > 1 ||
					!EmitLoadValueWord(node.operands[0], 0, TEMP0) ||
					!m_code.EmitTstImm32(TEMP0, FCR31_C) ||
					!m_code.EmitMovImm8(TEMP1, 0) ||
					!m_code.EmitMovImm8(TEMP1, 1,
						node.immediate != 0 ? Condition::NE : Condition::EQ))
				{
					return false;
				}
				return CommitValueWord(node.id, 0, TEMP1);
			}

			bool EmitCop1UnaryWord(const Node& node)
			{
				if (node.operand_count != 1 || node.type != ValueType::F32Bits)
					return false;
				unsigned source = 0;
				if (!AcquireVfp(node.operands[0], VFP_SCRATCH0, &source))
					return false;
				const RegionAllocation::Location& target = Location(node.id);
				const unsigned output =
					target.kind == RegionAllocation::LocationKind::VfpS ?
						VfpRegister(target) : VFP_SCRATCH1;
				const bool emitted = node.opcode == Opcode::Cop1AbsoluteWord ?
					m_code.EmitVabsF32(output, source) :
					node.opcode == Opcode::Cop1NegateWord ?
						m_code.EmitVnegF32(output, source) : false;
				return emitted && CommitVfp(node.id, output);
			}

			bool EmitCop1ClearOuFlags(const Node& node)
			{
				unsigned source = 0;
				return node.operand_count == 1 && node.type == ValueType::I32 &&
				       AcquireValueWord(node.operands[0], 0, TEMP0, &source) &&
				       m_code.EmitBicImm32(TEMP0, source, FCR31_O | FCR31_U) &&
				       CommitValueWord(node.id, 0, TEMP0);
			}

			bool EmitCop1ConvertWord(const Node& node)
			{
				if (node.operand_count != 1 || node.type != ValueType::F32Bits ||
					!EmitLoadValueWord(node.operands[0], 0, TEMP0) ||
					!m_code.EmitMovImm32(TEMP2, COP1_EXPONENT) ||
					!m_code.EmitAndReg(TEMP1, TEMP0, TEMP2) ||
					!m_code.EmitMovImm32(TEMP2, COP1_CVT_W_MAX_EXPONENT) ||
					!m_code.EmitCmpReg(TEMP1, TEMP2))
				{
					return false;
				}
				const size_t convert = m_code.EmitBranchPlaceholder(Condition::LS);
				if (convert == static_cast<size_t>(-1) ||
					!m_code.EmitTstImm32(TEMP0, COP1_SIGN) ||
					!m_code.EmitMovImm32(TEMP0, 0x7fffffffu) ||
					!m_code.EmitMovImm32(TEMP0, 0x80000000u, Condition::NE))
				{
					return false;
				}
				const size_t done_from_clamp = m_code.EmitBranchPlaceholder();
				if (done_from_clamp == static_cast<size_t>(-1) ||
					!m_code.PatchBranch(convert, m_code.Size(), Condition::LS) ||
					!m_code.EmitVmovCoreToS(VFP_SCRATCH0, TEMP0) ||
					!m_code.EmitVcvtS32F32(VFP_SCRATCH0, VFP_SCRATCH0) ||
					!m_code.EmitVmovSToCore(TEMP0, VFP_SCRATCH0))
				{
					return false;
				}
				const size_t done = m_code.Size();
				return m_code.PatchBranch(done_from_clamp, done) &&
				       CommitCop1Bits(node.id, TEMP0);
			}

			bool EmitCop1ConvertSingle(const Node& node)
			{
				if (node.operand_count != 1 || node.type != ValueType::F32Bits)
					return false;
				unsigned source = 0;
				if (!AcquireVfp(node.operands[0], VFP_SCRATCH0, &source))
					return false;
				const RegionAllocation::Location& target = Location(node.id);
				const unsigned output =
					target.kind == RegionAllocation::LocationKind::VfpS ?
						VfpRegister(target) : VFP_SCRATCH1;
				return m_code.EmitVcvtF32S32(output, source) &&
				       CommitVfp(node.id, output);
			}

			bool EmitCopyLocationWord(const RegionAllocation::Location& source_location,
				ValueId source_value, u8 source_word,
				const RegionAllocation::Location& target_location, u8 target_word)
			{
				// CanonicalState is a read-only rematerialization location rather than
				// physical region storage. Identity-view aliases (ExtractLow32/64,
				// bitcasts and zero-offset addresses) deliberately inherit it from an
				// unchanged block parameter. Such an alias requires no generated copy,
				// but it must not fall through to the ordinary load/store path and try
				// to write canonical state. A different slot or lane is not an identity
				// and remains unrepresentable as a canonical target. Canonical sources
				// may still materialize into ordinary edge storage below.
				if (target_location.kind ==
						RegionAllocation::LocationKind::CanonicalState)
				{
					return source_location.kind ==
							RegionAllocation::LocationKind::CanonicalState &&
						target_location.kind ==
							RegionAllocation::LocationKind::CanonicalState &&
						source_location.index == target_location.index &&
						source_word == target_word;
				}
				if ((RematerializedWordMask(source_value) &
						(1u << source_word)) != 0)
				{
					const RegionAllocation::RematerializationKind kind =
						Rematerialization(source_value);
					if (source_word != 1 || kind ==
						RegionAllocation::RematerializationKind::None)
					{
						return false;
					}
					const bool target_core =
						target_location.kind == RegionAllocation::LocationKind::Core ||
						target_location.kind == RegionAllocation::LocationKind::FixedCycle;
					const unsigned output = target_core ?
						CoreRegister(target_location, target_word) : TEMP0;
					if (kind == RegionAllocation::RematerializationKind::ZeroExtendLow32)
					{
						if (!m_code.EmitMovImm8(output, 0))
							return false;
					}
					else
					{
						RegionAllocation::Location basis = source_location;
						basis.words = 1;
						basis.word_mask = 0x1;
						const bool source_core =
							basis.kind == RegionAllocation::LocationKind::Core ||
							basis.kind == RegionAllocation::LocationKind::FixedCycle;
						const unsigned input = source_core ?
							CoreRegister(basis, 0) : TEMP1;
						if ((!source_core && !EmitLoadLocationWord(basis,
								source_value, 0, input)) ||
							!m_code.EmitMovRegShiftImm(output, input, ShiftType::ASR, 31))
						{
							return false;
						}
					}
					return target_core || EmitStoreLocationWord(
						target_location, target_word, output);
				}
				if (Physical(source_location, source_word) ==
					Physical(target_location, target_word))
				{
					return true;
				}
				const bool source_core =
					source_location.kind == RegionAllocation::LocationKind::Core ||
					source_location.kind == RegionAllocation::LocationKind::FixedCycle;
				const bool target_core =
					target_location.kind == RegionAllocation::LocationKind::Core ||
					target_location.kind == RegionAllocation::LocationKind::FixedCycle;
				if (target_core)
				{
					const unsigned target = CoreRegister(target_location, target_word);
					if (source_core)
						return EmitMove(target,
							CoreRegister(source_location, source_word));
					if (source_location.kind == RegionAllocation::LocationKind::Spill)
						return EmitSpillLoad(target,
							SpillOffset(source_location, source_word));
					if (source_location.kind == RegionAllocation::LocationKind::NeonQ)
						return m_code.EmitVmovD32LaneToCore(target,
							source_location.index * 2 + source_word / 2,
							source_word & 1u);
					if (source_location.kind == RegionAllocation::LocationKind::Immediate)
					{
						const Node* definition = Definition(StorageValue(source_value));
						if (!definition)
							return false;
						const u32 literal = source_word < 2 ?
							static_cast<u32>(definition->literal >> (source_word * 32)) : 0;
						return m_code.EmitMovImm32(target, literal);
					}
				}
				if (target_location.kind == RegionAllocation::LocationKind::Spill &&
					source_core)
				{
					return EmitSpillStore(CoreRegister(source_location, source_word),
						SpillOffset(target_location, target_word));
				}
				if (target_location.kind == RegionAllocation::LocationKind::NeonQ &&
					source_core)
				{
					return m_code.EmitVmovCoreToD32Lane(
						target_location.index * 2 + target_word / 2,
						target_word & 1u,
						CoreRegister(source_location, source_word));
				}
				return EmitLoadLocationWord(source_location, source_value, source_word,
						TEMP0) &&
				       EmitStoreLocationWord(target_location, target_word, TEMP0);
			}

			bool AcquireValueWord(ValueId value, u8 word, unsigned scratch,
				unsigned* host)
			{
				if (!host)
					return false;
				const RegionAllocation::Location& demand_location = Location(value);
				const RegionAllocation::Location& location = StorageLocation(value);
				if ((LocationMask(demand_location) & (1u << word)) != 0 &&
					(location.kind == RegionAllocation::LocationKind::Core ||
					 location.kind == RegionAllocation::LocationKind::FixedCycle))
				{
					*host = CoreRegister(location, word);
					return true;
				}
				*host = scratch;
				return EmitLoadValueWord(value, word, scratch);
			}

			unsigned DestinationHost(ValueId value, u8 word,
				unsigned fallback) const
			{
				const RegionAllocation::Location& location = Location(value);
				// Some lowerings construct both halves of a scalar load even when
				// backward demand proves that only one architectural word survives.
				// An undemanded word has no allocated register: WordRank() would place
				// it immediately after the location and can therefore alias a reserved
				// ABI register (r11 in the callable backend). Keep such dead work in the
				// explicitly supplied scratch register.
				return (LocationMask(location) & (1u << word)) != 0 &&
					(location.kind == RegionAllocation::LocationKind::Core ||
					 location.kind == RegionAllocation::LocationKind::FixedCycle) ?
					CoreRegister(location, word) : fallback;
			}

			bool CommitValueWord(ValueId value, u8 word, unsigned host)
			{
				return EmitStoreValueWord(value, word, host);
			}

			bool AcquireVector(ValueId value, unsigned scratch,
				unsigned* host)
			{
				if (!host || scratch < VECTOR_SCRATCH0 ||
					scratch > VECTOR_SCRATCH1)
				{
					return false;
				}
				const RegionAllocation::Location& demand_location = Location(value);
				const RegionAllocation::Location& location = StorageLocation(value);
				if (location.kind == RegionAllocation::LocationKind::NeonQ)
				{
					*host = location.index;
					return true;
				}
				if (location.kind == RegionAllocation::LocationKind::Spill &&
					location.words == 4)
				{
					m_result.spill_vector_loads++;
					*host = scratch;
					return EmitQAddress(TEMP2, location.index) &&
					       m_code.EmitVld1Q32(scratch, TEMP2);
				}
				if (location.kind != RegionAllocation::LocationKind::Immediate)
					return false;

				// Constants carry at most a 64-bit literal. Initialize the complete
				// vector so non-demanded lanes cannot leak stack/register contents if a
				// later allocation widens their demand.
				if (!m_code.EmitVmovI32Q(scratch, 0, 0))
					return false;
				const u8 mask = LocationMask(demand_location);
				for (u8 word = 0; word < 4; word++)
				{
					if ((mask & (1u << word)) != 0 &&
						(!EmitLoadLocationWord(location, StorageValue(value), word, TEMP0) ||
						 !m_code.EmitVmovCoreToD32Lane(
							scratch * 2 + word / 2, word & 1u, TEMP0)))
					{
						return false;
					}
				}
				*host = scratch;
				return true;
			}

			bool CommitVector(ValueId value, unsigned source)
			{
				const RegionAllocation::Location& location = Location(value);
				if (location.kind == RegionAllocation::LocationKind::NeonQ)
				{
					return location.index == source ||
					       m_code.EmitVorrQ(location.index, source, source);
				}
				if (location.kind != RegionAllocation::LocationKind::Spill ||
					location.words != 4)
					return false;
				m_result.spill_vector_stores++;
				return EmitQAddress(TEMP2, location.index) &&
				       m_code.EmitVst1Q32(source, TEMP2);
			}

			bool EmitNormalizeVuWord(unsigned value)
			{
				if (!m_code.EmitMovImm32(TEMP2, VU_FLOAT_EXPONENT) ||
					!m_code.EmitAndReg(TEMP1, value, TEMP2) ||
					!m_code.EmitCmpImm32(TEMP1, 0))
				{
					return false;
				}
				const size_t exponent_nonzero =
					m_code.EmitBranchPlaceholder(Condition::NE);
				if (exponent_nonzero == static_cast<size_t>(-1) ||
					!m_code.EmitMovImm32(TEMP2, VU_FLOAT_SIGN) ||
					!m_code.EmitAndReg(value, value, TEMP2))
				{
					return false;
				}
				const size_t done_from_zero = m_code.EmitBranchPlaceholder();
				if (done_from_zero == static_cast<size_t>(-1) ||
					!m_code.PatchBranch(exponent_nonzero, m_code.Size(), Condition::NE))
				{
					return false;
				}

				size_t done_from_finite = static_cast<size_t>(-1);
				if (m_program.options.vu0_overflow_clamp)
				{
					if (!m_code.EmitMovImm32(TEMP2, VU_FLOAT_EXPONENT) ||
						!m_code.EmitCmpReg(TEMP1, TEMP2))
					{
						return false;
					}
					done_from_finite =
						m_code.EmitBranchPlaceholder(Condition::NE);
					if (done_from_finite == static_cast<size_t>(-1) ||
						!m_code.EmitMovImm32(TEMP2, VU_FLOAT_SIGN) ||
						!m_code.EmitAndReg(value, value, TEMP2) ||
						!m_code.EmitMovImm32(TEMP2, VU_FLOAT_MAX_FINITE) ||
						!m_code.EmitOrrReg(value, value, TEMP2))
					{
						return false;
					}
				}
				const size_t done = m_code.Size();
				return m_code.PatchBranch(done_from_zero, done) &&
				       (done_from_finite == static_cast<size_t>(-1) ||
						m_code.PatchBranch(done_from_finite, done, Condition::NE));
			}

			bool EmitNormalizeVuQuad(unsigned value)
			{
				// q7 and physical q11 are never allocator locations.  Keep the
				// exponent and sign paths disjoint while VBIT updates the value in
				// place.  Recomputing the exponent for the overflow test is unnecessary:
				// denormal replacement does not change its zero exponent and all other
				// lanes are still bit-identical.
				if (!m_code.EmitVandQ(VECTOR_SCRATCH1, value, VU0_EXPONENT_Q) ||
					!m_code.EmitVandQ(VU0_MASK_Q, value, VU0_SIGN_Q) ||
					!m_code.EmitVceqI32ZeroQ(VECTOR_SCRATCH1, VECTOR_SCRATCH1) ||
					!m_code.EmitVbitQ(value, VU0_MASK_Q, VECTOR_SCRATCH1))
				{
					return false;
				}
				if (!m_program.options.vu0_overflow_clamp)
					return true;
				return m_code.EmitVandQ(VECTOR_SCRATCH1, value, VU0_EXPONENT_Q) &&
				       m_code.EmitVceqI32Q(VECTOR_SCRATCH1, VECTOR_SCRATCH1,
						   VU0_EXPONENT_Q) &&
				       m_code.EmitVorrQ(VU0_MASK_Q, VU0_MASK_Q,
						   VU0_MAX_FINITE_Q) &&
				       m_code.EmitVbitQ(value, VU0_MASK_Q, VECTOR_SCRATCH1);
			}

			bool EmitVu0NormalizeVector(const Node& node)
			{
				if (node.operand_count != 1 || node.type != ValueType::VuF32x4Bits)
					return false;
				const ValueId alias =
					node.id < m_allocation.vu0_normalize_alias.size() ?
						m_allocation.vu0_normalize_alias[node.id] : INVALID_VALUE;
				if (alias != INVALID_VALUE)
				{
					unsigned source = 0;
					return AcquireVector(alias, VECTOR_SCRATCH0, &source) &&
					       CommitVector(node.id, source);
				}
				const RegionAllocation::Location& target = Location(node.id);
				const u8 mask = LocationMask(target);
				if (target.kind == RegionAllocation::LocationKind::NeonQ ||
					(target.kind == RegionAllocation::LocationKind::Spill &&
					 mask == 0x0f))
				{
					unsigned source = 0;
					const unsigned output =
						target.kind == RegionAllocation::LocationKind::NeonQ ?
							target.index : VECTOR_SCRATCH0;
					if (!AcquireVector(node.operands[0], VECTOR_SCRATCH0, &source) ||
						(output != source &&
						 !m_code.EmitVorrQ(output, source, source)) ||
						!EmitNormalizeVuQuad(output))
					{
						return false;
					}
					return CommitVector(node.id, output);
				}

				for (u8 word = 0; word < 4; word++)
				{
					if ((mask & (1u << word)) != 0 &&
						(!EmitLoadValueWord(node.operands[0], word, TEMP0) ||
						 !EmitNormalizeVuWord(TEMP0) ||
						 !CommitValueWord(node.id, word, TEMP0)))
					{
						return false;
					}
				}
				return true;
			}

			bool EmitVu0ConvertFixed(const Node& node)
			{
				if (node.operand_count != 1 ||
					node.type != ValueType::VuF32x4Bits ||
					(node.immediate != 0 && node.immediate != 4 &&
					 node.immediate != 12 && node.immediate != 15))
				{
					return false;
				}

				const RegionAllocation::Location& target = Location(node.id);
				const u8 demand = LocationMask(target);
				unsigned target_lane0 = 0;
				if (demand != 0 && ScalarVfpLane(target, 0, &target_lane0))
				{
					// PCSX2 owner: VUops.cpp::floatToInt<>(). Keep the selected
					// conversions in scalar VFP lanes so FPSCR rounding/flush behavior
					// remains identical to the established block compiler. Saturation,
					// however, is a lane-independent bit contract: form its predicate
					// once in NEON rather than branching twice for every live lane.
					unsigned source = 0;
					const unsigned output = target.index;
					if (!AcquireVector(node.operands[0], VECTOR_SCRATCH0, &source) ||
						(output != source &&
						 !m_code.EmitVorrQ(output, source, source)))
					{
						return false;
					}

					if (node.immediate != 0)
					{
						if (!m_code.EmitMovImm32(TEMP0,
								0x3f800000u + (node.immediate << 23)) ||
							!m_code.EmitVmovCoreToS(VFP_SCRATCH1, TEMP0))
						{
							return false;
						}
						for (u8 lane = 0; lane < 4; lane++)
						{
							unsigned destination_s = 0;
							if ((demand & (1u << lane)) != 0 &&
								(!ScalarVfpLane(target, lane, &destination_s) ||
								 !m_code.EmitVmulF32(destination_s, destination_s,
									 VFP_SCRATCH1)))
							{
								return false;
							}
						}
					}

					// Preserve the scaled floating-point bits before VCVT overwrites
					// the destination. The comparison |exponent| > 0x4effffff is
					// exactly the scalar >= 0x4f000000 signed-range test.
					if (!m_code.EmitVorrQ(VECTOR_SCRATCH0, output, output))
						return false;
					for (u8 lane = 0; lane < 4; lane++)
					{
						unsigned destination_s = 0;
						if ((demand & (1u << lane)) != 0 &&
							(!ScalarVfpLane(target, lane, &destination_s) ||
							 !m_code.EmitVcvtS32F32(destination_s, destination_s)))
						{
							return false;
						}
					}

					return m_code.EmitVandQ(VECTOR_SCRATCH1, VECTOR_SCRATCH0,
							   VU0_EXPONENT_Q) &&
					       m_code.EmitMovImm32(TEMP0, 0x4effffffu) &&
					       m_code.EmitVdupI32QFromCore(VU0_MASK_Q, TEMP0) &&
					       m_code.EmitVcgtS32Q(VECTOR_SCRATCH1, VECTOR_SCRATCH1,
							   VU0_MASK_Q) &&
					       m_code.EmitVshrS32Q(VU0_MASK_Q, VECTOR_SCRATCH0, 31) &&
					       m_code.EmitMovImm32(TEMP0, 0x7fffffffu) &&
					       m_code.EmitVdupI32QFromCore(VECTOR_SCRATCH0, TEMP0) &&
					       m_code.EmitVeorQ(VU0_MASK_Q, VU0_MASK_Q,
							   VECTOR_SCRATCH0) &&
					       m_code.EmitVbitQ(output, VU0_MASK_Q, VECTOR_SCRATCH1) &&
					       CommitVector(node.id, output);
				}

				// Spill-only and mixed-bank destinations cannot be addressed as four
				// scalar VFP lanes. Retain the exact cold scalar lowering for them;
				// it is also the fail-closed register-pressure path.
				for (u8 lane = 0; lane < 4; lane++)
				{
					if ((demand & (1u << lane)) == 0)
						continue;
					if (!EmitLoadValueWord(node.operands[0], lane, TEMP0))
						return false;
					if (node.immediate != 0 &&
						(!m_code.EmitVmovCoreToS(VFP_SCRATCH0, TEMP0) ||
						 !m_code.EmitMovImm32(TEMP1,
							0x3f800000u + (node.immediate << 23)) ||
						 !m_code.EmitVmovCoreToS(VFP_SCRATCH1, TEMP1) ||
						 !m_code.EmitVmulF32(VFP_SCRATCH0, VFP_SCRATCH0,
							VFP_SCRATCH1) ||
						 !m_code.EmitVmovSToCore(TEMP0, VFP_SCRATCH0)))
					{
						return false;
					}

					if (!EmitMove(TEMP1, TEMP0) ||
						!m_code.EmitMovImm32(TEMP2, VU_FLOAT_EXPONENT) ||
						!m_code.EmitAndReg(TEMP2, TEMP1, TEMP2) ||
						!m_code.EmitCmpImm32(TEMP2, 0x4f000000u))
					{
						return false;
					}
					const size_t convert =
						m_code.EmitBranchPlaceholder(Condition::CC);
					if (convert == static_cast<size_t>(-1) ||
						!m_code.EmitTstImm32(TEMP1, VU_FLOAT_SIGN) ||
						!m_code.EmitMovImm32(TEMP0, 0x7fffffffu) ||
						!m_code.EmitMovImm32(TEMP0, 0x80000000u, Condition::NE))
					{
						return false;
					}
					const size_t done_from_clamp = m_code.EmitBranchPlaceholder();
					if (done_from_clamp == static_cast<size_t>(-1) ||
						!m_code.PatchBranch(convert, m_code.Size(), Condition::CC) ||
						!m_code.EmitVmovCoreToS(VFP_SCRATCH0, TEMP1) ||
						!m_code.EmitVcvtS32F32(VFP_SCRATCH0, VFP_SCRATCH0) ||
						!m_code.EmitVmovSToCore(TEMP0, VFP_SCRATCH0))
					{
						return false;
					}
					const size_t done = m_code.Size();
					if (!m_code.PatchBranch(done_from_clamp, done) ||
						!CommitValueWord(node.id, lane, TEMP0))
					{
						return false;
					}
				}
				return true;
			}

			bool EmitVu0ConvertIntegerToFloat(const Node& node)
			{
				if (node.operand_count != 1 ||
					node.type != ValueType::VuF32x4Bits ||
					(node.immediate != 0 && node.immediate != 4 &&
					 node.immediate != 12 && node.immediate != 15))
				{
					return false;
				}

				const RegionAllocation::Location& target = Location(node.id);
				const u8 demand = LocationMask(target);
				unsigned target_lane0 = 0;
				if (demand != 0 && ScalarVfpLane(target, 0, &target_lane0))
				{
					// PCSX2 owner: VUops.cpp::intToFloat<>(). As in the established
					// tier-zero lowering, scalar VFP VCVT is intentional: it observes
					// the EE FPCR while Advanced SIMD integer conversion does not.
					// ITOF's following power-of-two scale is exact for every nonzero
					// result and cannot produce a denormal.
					unsigned source = 0;
					const unsigned output = target.index;
					if (!AcquireVector(node.operands[0], VECTOR_SCRATCH0, &source) ||
						(output != source && !m_code.EmitVorrQ(output, source, source)))
					{
						return false;
					}
					if (node.immediate != 0 &&
						(!m_code.EmitMovImm32(TEMP0,
							0x3f800000u - (node.immediate << 23)) ||
						 !m_code.EmitVmovCoreToS(VFP_SCRATCH1, TEMP0)))
					{
						return false;
					}
					for (u8 lane = 0; lane < 4; lane++)
					{
						if ((demand & (1u << lane)) == 0)
							continue;
						unsigned destination_s = 0;
						if (!ScalarVfpLane(target, lane, &destination_s) ||
							!m_code.EmitVcvtF32S32(destination_s, destination_s) ||
							(node.immediate != 0 &&
							 !m_code.EmitVmulF32(destination_s, destination_s,
								 VFP_SCRATCH1)))
						{
							return false;
						}
					}
					return CommitVector(node.id, output);
				}

				// Spill-only and mixed-bank destinations have no scalar S-register
				// names. Keep the exact conversion cold and lane-local rather than
				// weakening the architectural rounding contract.
				for (u8 lane = 0; lane < 4; lane++)
				{
					if ((demand & (1u << lane)) == 0)
						continue;
					if (!EmitLoadValueWord(node.operands[0], lane, TEMP0) ||
						!m_code.EmitVmovCoreToS(VFP_SCRATCH0, TEMP0) ||
						!m_code.EmitVcvtF32S32(VFP_SCRATCH0, VFP_SCRATCH0))
					{
						return false;
					}
					if (node.immediate != 0 &&
						(!m_code.EmitMovImm32(TEMP1,
							0x3f800000u - (node.immediate << 23)) ||
						 !m_code.EmitVmovCoreToS(VFP_SCRATCH1, TEMP1) ||
						 !m_code.EmitVmulF32(VFP_SCRATCH0, VFP_SCRATCH0,
							 VFP_SCRATCH1)))
					{
						return false;
					}
					if (!m_code.EmitVmovSToCore(TEMP0, VFP_SCRATCH0) ||
						!CommitValueWord(node.id, lane, TEMP0))
					{
						return false;
					}
				}
				return true;
			}

			bool EmitVu0BroadcastLane(const Node& node)
			{
				if (node.operand_count != 1 || node.type != ValueType::VuF32x4Bits ||
					node.immediate >= 4)
				{
					return false;
				}
				unsigned source = 0;
				if (!AcquireVector(node.operands[0], VECTOR_SCRATCH0, &source))
					return false;
				const RegionAllocation::Location& target = Location(node.id);
				const unsigned output =
					target.kind == RegionAllocation::LocationKind::NeonQ ?
						target.index : VECTOR_SCRATCH0;
				return m_code.EmitVdupI32QFromQlane(output, source,
						node.immediate) &&
				       CommitVector(node.id, output);
			}

			bool EmitVu0Rotate32(const Node& node)
			{
				if (node.operand_count != 1 || node.type != ValueType::VuF32x4Bits ||
					node.immediate != 0 || node.literal != 0)
				{
					return false;
				}

				const RegionAllocation::Location& target = Location(node.id);
				const u8 demand = LocationMask(target);
				if (target.kind == RegionAllocation::LocationKind::NeonQ ||
					(target.kind == RegionAllocation::LocationKind::Spill &&
					 demand == 0x0f))
				{
					unsigned source = 0;
					const unsigned output =
						target.kind == RegionAllocation::LocationKind::NeonQ ?
							target.index : VECTOR_SCRATCH0;
					return AcquireVector(node.operands[0], VECTOR_SCRATCH0, &source) &&
					       m_code.EmitVextI8Q(output, source, source, 4) &&
					       CommitVector(node.id, output);
				}

				for (u8 lane = 0; lane < 4; lane++)
				{
					if ((demand & (1u << lane)) != 0 &&
						(!EmitLoadValueWord(node.operands[0], (lane + 1u) & 3u, TEMP0) ||
						 !CommitValueWord(node.id, lane, TEMP0)))
					{
						return false;
					}
				}
				return true;
			}

			bool EmitVu0BroadcastScalar(const Node& node)
			{
				if (node.operand_count != 1 || node.type != ValueType::VuF32x4Bits ||
					node.immediate != 0 || node.literal != 0 ||
					!EmitLoadValueWord(node.operands[0], 0, TEMP0) ||
					!EmitNormalizeVuWord(TEMP0))
				{
					return false;
				}

				const RegionAllocation::Location& target = Location(node.id);
				const u8 demand = LocationMask(target);
				if (target.kind == RegionAllocation::LocationKind::NeonQ ||
					(target.kind == RegionAllocation::LocationKind::Spill &&
					 demand == 0x0f))
				{
					const unsigned output =
						target.kind == RegionAllocation::LocationKind::NeonQ ?
							target.index : VECTOR_SCRATCH0;
					return m_code.EmitVdupI32QFromCore(output, TEMP0) &&
					       CommitVector(node.id, output);
				}

				for (u8 lane = 0; lane < 4; lane++)
				{
					if ((demand & (1u << lane)) != 0 &&
						!CommitValueWord(node.id, lane, TEMP0))
					{
						return false;
					}
				}
				return true;
			}

			bool DecodeVu0FdivNode(const Node& node, u32* kind, u32* fs_lane,
				u32* ft_lane) const
			{
				if (node.operand_count != 2 || node.type != ValueType::I32 ||
					node.literal != 0 || (node.immediate & ~0x3fu) != 0 ||
					(node.immediate & 0x3u) > 2)
				{
					return false;
				}
				*kind = node.immediate & 0x3u;
				*fs_lane = (node.immediate >> 2) & 0x3u;
				*ft_lane = (node.immediate >> 4) & 0x3u;
				return true;
			}

			bool EmitVu0FdivQ(const Node& node)
			{
				u32 kind = 0;
				u32 fs_lane = 0;
				u32 ft_lane = 0;
				if (!DecodeVu0FdivNode(node, &kind, &fs_lane, &ft_lane))
					return false;

				// PCSX2 owners: VUops.cpp::{_vuDIV,_vuSQRT,_vuRSQRT}.
				// s30/s31 are allocator-reserved, so both normalized operands and the
				// result remain in VFP while the exact zero/sign cases use core scratch.
				if (kind != 1 &&
					(!EmitLoadValueWord(node.operands[0], fs_lane, TEMP0) ||
					 !EmitNormalizeVuWord(TEMP0) ||
					 !m_code.EmitVmovCoreToS(VFP_SCRATCH0, TEMP0)))
				{
					return false;
				}
				if (!EmitLoadValueWord(node.operands[1], ft_lane, TEMP0) ||
					!EmitNormalizeVuWord(TEMP0))
				{
					return false;
				}

				if (kind == 1)
				{
					return m_code.EmitBicImm32(TEMP0, TEMP0, VU_FLOAT_SIGN) &&
					       m_code.EmitVmovCoreToS(VFP_SCRATCH0, TEMP0) &&
					       m_code.EmitVsqrtF32(VFP_SCRATCH0, VFP_SCRATCH0) &&
					       m_code.EmitVmovSToCore(TEMP0, VFP_SCRATCH0) &&
					       EmitNormalizeVuWord(TEMP0) &&
					       CommitValueWord(node.id, 0, TEMP0);
				}

				if (!m_code.EmitVmovCoreToS(VFP_SCRATCH1, TEMP0) ||
					!m_code.EmitVmovSToCore(TEMP1, VFP_SCRATCH0) ||
					!m_code.EmitBicImm32(TEMP2, TEMP0, VU_FLOAT_SIGN) ||
					!m_code.EmitCmpImm32(TEMP2, 0))
				{
					return false;
				}
				const size_t divisor_nonzero =
					m_code.EmitBranchPlaceholder(Condition::NE);
				if (divisor_nonzero == static_cast<size_t>(-1) ||
					!m_code.EmitBicImm32(TEMP2, TEMP1, VU_FLOAT_SIGN) ||
					!m_code.EmitCmpImm32(TEMP2, 0))
				{
					return false;
				}

				size_t done_from_zero_zero = static_cast<size_t>(-1);
				if (kind == 2)
				{
					const size_t numerator_nonzero =
						m_code.EmitBranchPlaceholder(Condition::NE);
					if (numerator_nonzero == static_cast<size_t>(-1) ||
						!m_code.EmitEorReg(TEMP0, TEMP1, TEMP0) ||
						!m_code.EmitMovImm32(TEMP2, VU_FLOAT_SIGN) ||
						!m_code.EmitAndReg(TEMP0, TEMP0, TEMP2))
					{
						return false;
					}
					done_from_zero_zero = m_code.EmitBranchPlaceholder();
					if (done_from_zero_zero == static_cast<size_t>(-1) ||
						!m_code.PatchBranch(numerator_nonzero, m_code.Size(),
							Condition::NE))
					{
						return false;
					}
				}

				if (!m_code.EmitEorReg(TEMP0, TEMP1, TEMP0) ||
					!m_code.EmitMovImm32(TEMP2, VU_FLOAT_SIGN) ||
					!m_code.EmitAndReg(TEMP0, TEMP0, TEMP2) ||
					!m_code.EmitMovImm32(TEMP2, VU_FLOAT_MAX_FINITE) ||
					!m_code.EmitOrrReg(TEMP0, TEMP0, TEMP2))
				{
					return false;
				}
				const size_t done_from_zero = m_code.EmitBranchPlaceholder();
				if (done_from_zero == static_cast<size_t>(-1) ||
					!m_code.PatchBranch(divisor_nonzero, m_code.Size(), Condition::NE))
				{
					return false;
				}

				if (kind == 2 &&
					(!m_code.EmitBicImm32(TEMP0, TEMP0, VU_FLOAT_SIGN) ||
					 !m_code.EmitVmovCoreToS(VFP_SCRATCH1, TEMP0) ||
					 !m_code.EmitVsqrtF32(VFP_SCRATCH1, VFP_SCRATCH1)))
				{
					return false;
				}
				if (!m_code.EmitVdivF32(VFP_SCRATCH0, VFP_SCRATCH0,
						VFP_SCRATCH1) ||
					!m_code.EmitVmovSToCore(TEMP0, VFP_SCRATCH0) ||
					!EmitNormalizeVuWord(TEMP0))
				{
					return false;
				}

				const size_t done = m_code.Size();
				return (done_from_zero_zero == static_cast<size_t>(-1) ||
						m_code.PatchBranch(done_from_zero_zero, done)) &&
				       m_code.PatchBranch(done_from_zero, done) &&
				       CommitValueWord(node.id, 0, TEMP0);
			}

			bool EmitVu0FdivFlags(const Node& node)
			{
				u32 kind = 0;
				u32 fs_lane = 0;
				u32 ft_lane = 0;
				if (!DecodeVu0FdivNode(node, &kind, &fs_lane, &ft_lane) ||
					(kind != 1 &&
					 (!EmitLoadValueWord(node.operands[0], fs_lane, TEMP0) ||
					  !EmitNormalizeVuWord(TEMP0) ||
					  !m_code.EmitVmovCoreToS(VFP_SCRATCH0, TEMP0))) ||
					!EmitLoadValueWord(node.operands[1], ft_lane, TEMP0) ||
					!EmitNormalizeVuWord(TEMP0))
				{
					return false;
				}

				if (kind == 1)
				{
					if (!m_code.EmitBicImm32(TEMP1, TEMP0, VU_FLOAT_SIGN) ||
						!m_code.EmitCmpImm32(TEMP1, 0) ||
						!m_code.EmitMovImm8(TEMP2, 0))
					{
						return false;
					}
					const size_t zero = m_code.EmitBranchPlaceholder(Condition::EQ);
					if (zero == static_cast<size_t>(-1) ||
						!m_code.EmitTstImm32(TEMP0, VU_FLOAT_SIGN) ||
						!m_code.EmitMovImm8(TEMP2, 0x10, Condition::NE) ||
						!m_code.PatchBranch(zero, m_code.Size(), Condition::EQ))
					{
						return false;
					}
					return CommitValueWord(node.id, 0, TEMP2);
				}

				if (!m_code.EmitBicImm32(TEMP1, TEMP0, VU_FLOAT_SIGN) ||
					!m_code.EmitCmpImm32(TEMP1, 0))
				{
					return false;
				}
				const size_t divisor_nonzero =
					m_code.EmitBranchPlaceholder(Condition::NE);
				if (divisor_nonzero == static_cast<size_t>(-1) ||
					!m_code.EmitVmovSToCore(TEMP1, VFP_SCRATCH0) ||
					!m_code.EmitBicImm32(TEMP1, TEMP1, VU_FLOAT_SIGN) ||
					!m_code.EmitCmpImm32(TEMP1, 0) ||
					!m_code.EmitMovImm8(TEMP2, kind == 2 ? 0x30 : 0x10,
						Condition::EQ) ||
					!m_code.EmitMovImm8(TEMP2, 0x20, Condition::NE))
				{
					return false;
				}
				const size_t done_from_zero = m_code.EmitBranchPlaceholder();
				if (done_from_zero == static_cast<size_t>(-1) ||
					!m_code.PatchBranch(divisor_nonzero, m_code.Size(), Condition::NE))
				{
					return false;
				}
				if (!m_code.EmitMovImm8(TEMP2, 0) ||
					(kind == 2 &&
					 (!m_code.EmitTstImm32(TEMP0, VU_FLOAT_SIGN) ||
					  !m_code.EmitMovImm8(TEMP2, 0x10, Condition::NE))))
				{
					return false;
				}
				const size_t done = m_code.Size();
				return m_code.PatchBranch(done_from_zero, done) &&
				       CommitValueWord(node.id, 0, TEMP2);
			}

			bool EmitVu0UpdateFdivStatus(const Node& node)
			{
				return node.operand_count == 2 && node.type == ValueType::I32 &&
				       EmitLoadValueWord(node.operands[0], 0, TEMP0) &&
				       m_code.EmitBicImm32(TEMP0, TEMP0, 0x30u) &&
				       EmitLoadValueWord(node.operands[1], 0, TEMP1) &&
				       m_code.EmitOrrReg(TEMP0, TEMP0, TEMP1) &&
				       CommitValueWord(node.id, 0, TEMP0);
			}

			bool EmitVu0SyncFdivStatusControl(const Node& node)
			{
				return node.operand_count == 2 && node.type == ValueType::I32 &&
				       EmitLoadValueWord(node.operands[0], 0, TEMP0) &&
				       m_code.EmitMovImm32(TEMP2, 0x3cfu) &&
				       m_code.EmitAndReg(TEMP0, TEMP0, TEMP2) &&
				       EmitLoadValueWord(node.operands[1], 0, TEMP1) &&
				       m_code.EmitAndImm32(TEMP1, TEMP1, 0x30u) &&
				       m_code.EmitOrrReg(TEMP0, TEMP0, TEMP1) &&
				       m_code.EmitOrrRegShiftImm(TEMP0, TEMP0, TEMP1,
						ShiftType::LSL, 6) &&
				       CommitValueWord(node.id, 0, TEMP0);
			}

			bool EmitVu0RawArithmetic(const Node& node)
			{
				if (node.operand_count != 2 || node.type != ValueType::VuF32x4Bits)
					return false;
				const ValueId fused_multiply = node.id <
						m_allocation.vu0_add_fused_mul.size() ?
					m_allocation.vu0_add_fused_mul[node.id] : INVALID_VALUE;
				if (node.opcode == Opcode::Vu0AddRaw &&
					fused_multiply != INVALID_VALUE)
				{
					const Node* const multiply = Definition(fused_multiply);
					if (!multiply || multiply->opcode != Opcode::Vu0MulRaw ||
						multiply->operand_count != 2)
					{
						return false;
					}
					const ValueId addend = node.operands[0] == fused_multiply ?
						node.operands[1] : node.operands[0];
					const RegionAllocation::Location& target = Location(node.id);
					const u8 demand = LocationMask(target);
					unsigned target_lane0 = 0;
					const bool direct_target = ScalarVfpLane(target, 0, &target_lane0);
					// q7 is deliberately outside the region allocation.  When every
					// operand and destination is scalar-addressable, retain the four
					// separately rounded VFP multiplies in s28-s31 and issue them before
					// their dependent adds.  This preserves PS2 Chop/Zero semantics (so
					// it cannot be contracted to VMLA) while exposing the independent
					// lane work needed to cover Cortex-A9 VFP latency.  The ordering also
					// makes target/input aliasing safe: no destination is overwritten
					// until every product has consumed its inputs.
					DirectVu0FusedAddLanes direct_lanes{};
					if (ResolveDirectVu0FusedAdd(node, &direct_lanes))
					{
						return EmitDirectVu0FusedProducts(direct_lanes) &&
						       EmitDirectVu0FusedSums(node, direct_lanes);
					}
					CachedVu0FusedBroadcastLanes cached_broadcast{};
					if (ResolveCachedVu0FusedBroadcast(node, *multiply, addend,
							&cached_broadcast))
					{
						return EmitCachedVu0FusedBroadcast(node, cached_broadcast);
					}
					for (u8 lane = 0; lane < 4; lane++)
					{
						if ((demand & (1u << lane)) == 0)
							continue;
						unsigned left_s = 0;
						unsigned right_s = 0;
						unsigned accumulator_s = 0;
						if (!AcquireVu0ArithmeticLane(multiply->operands[0], lane,
								VFP_SCRATCH0, &left_s) ||
							!AcquireVu0ArithmeticLane(multiply->operands[1], lane,
								VFP_SCRATCH1, &right_s) ||
							!m_code.EmitVmulF32(VFP_SCRATCH0, left_s, right_s) ||
							!AcquireVu0ArithmeticLane(addend, lane,
								VFP_SCRATCH1, &accumulator_s))
						{
							return false;
						}
						unsigned destination_s = VFP_SCRATCH0;
						if (direct_target && !ScalarVfpLane(target, lane, &destination_s))
							return false;
						if (!m_code.EmitVaddF32(destination_s, accumulator_s,
								VFP_SCRATCH0) ||
							(!direct_target &&
							 (!m_code.EmitVmovSToCore(TEMP0, VFP_SCRATCH0) ||
							  !CommitValueWord(node.id, lane, TEMP0))))
						{
							return false;
						}
						m_result.vu0_fused_madd_lanes++;
					}
					return !direct_target || CommitVector(node.id, target.index);
				}
				const RegionAllocation::Location& left = Location(node.operands[0]);
				const RegionAllocation::Location& right = Location(node.operands[1]);
				const RegionAllocation::Location& target = Location(node.id);
				// Advanced SIMD floating point is fixed to round-to-nearest. VU0 macro
				// arithmetic instead observes the FPSCR mode installed by PCSX2 (Chop/Zero
				// by default). VitaEeBlockCompiler::EmitCOP2MacroArithmeticBody therefore
				// deliberately keeps normalized operands in quads but executes each active
				// lane with scalar VFP. Preserve that exact owner contract here: a NEON
				// VMUL differs by one ULP for ordinary finite values under Chop/Zero.
				const u8 demand = LocationMask(target);
				// q7 aliases scalar s28-s31 and is outside the seven-Q allocation.
				// When the result is a complete spilled vector and at most one input is
				// spilled, make q7 the temporary architectural result. This replaces four
				// core word loads/stores and ARM<->VFP transfers with one VLD1/VST1 while
				// preserving the independently rounded scalar VFP operation in every lane.
				if (m_vu0_product_scratch_available && demand == 0x0f &&
					target.kind == RegionAllocation::LocationKind::Spill &&
					target.words == 4)
				{
					std::array<unsigned, 4> left_lanes{};
					std::array<unsigned, 4> right_lanes{};
					bool left_direct = true;
					bool right_direct = true;
					for (u8 lane = 0; lane < 4; lane++)
					{
						left_direct &= DirectVu0ArithmeticLane(node.operands[0], lane,
							&left_lanes[lane]);
						right_direct &= DirectVu0ArithmeticLane(node.operands[1], lane,
							&right_lanes[lane]);
					}
					auto full_spill = [&](ValueId value) {
						const RegionAllocation::Location& location = StorageLocation(value);
						return location.kind == RegionAllocation::LocationKind::Spill &&
							location.words == 4 && LocationMask(Location(value)) == 0x0f;
					};
					const bool load_left = !left_direct && right_direct &&
						full_spill(node.operands[0]);
					const bool load_right = left_direct && !right_direct &&
						full_spill(node.operands[1]);
					if ((left_direct && right_direct) || load_left || load_right)
					{
						if (load_left || load_right)
						{
							const RegionAllocation::Location& spill = StorageLocation(
								load_left ? node.operands[0] : node.operands[1]);
							if (!EmitQAddress(TEMP2, spill.index) ||
								!m_code.EmitVld1Q32(7, TEMP2))
							{
								return false;
							}
							m_result.spill_vector_loads++;
						}
						for (u8 lane = 0; lane < 4; lane++)
						{
							const unsigned left_s = load_left ? 28 + lane :
								left_lanes[lane];
							const unsigned right_s = load_right ? 28 + lane :
								right_lanes[lane];
							bool emitted = false;
							switch (node.opcode)
							{
								case Opcode::Vu0MulRaw:
									emitted = m_code.EmitVmulF32(
										28 + lane, left_s, right_s);
									break;
								case Opcode::Vu0AddRaw:
									emitted = m_code.EmitVaddF32(
										28 + lane, left_s, right_s);
									break;
								case Opcode::Vu0SubRaw:
									emitted = m_code.EmitVsubF32(
										28 + lane, left_s, right_s);
									break;
								default:
									return false;
							}
							if (!emitted)
								return false;
							m_result.vu0_direct_vfp_lanes++;
						}
						m_result.vu0_spilled_quad_arithmetic++;
						return CommitVector(node.id, 7);
					}
				}
				unsigned target_lane0 = 0;
				const bool direct_target = ScalarVfpLane(target, 0, &target_lane0);
				for (u8 lane = 0; lane < 4; lane++)
				{
					if ((demand & (1u << lane)) == 0)
						continue;
					unsigned left_s = 0;
					unsigned right_s = 0;
					if (!AcquireVu0ArithmeticLane(node.operands[0], lane,
							VFP_SCRATCH0, &left_s) ||
						!AcquireVu0ArithmeticLane(node.operands[1], lane,
							VFP_SCRATCH1, &right_s))
					{
						return false;
					}
					unsigned destination_s = VFP_SCRATCH0;
					if (direct_target && !ScalarVfpLane(target, lane, &destination_s))
						return false;
					bool emitted = false;
					switch (node.opcode)
					{
						case Opcode::Vu0MulRaw:
							emitted = m_code.EmitVmulF32(destination_s, left_s, right_s);
							break;
						case Opcode::Vu0AddRaw:
							emitted = m_code.EmitVaddF32(destination_s, left_s, right_s);
							break;
						case Opcode::Vu0SubRaw:
							emitted = m_code.EmitVsubF32(destination_s, left_s, right_s);
							break;
						default:
							return false;
					}
					if (!emitted ||
						(!direct_target &&
						 (!m_code.EmitVmovSToCore(TEMP0, VFP_SCRATCH0) ||
						  !CommitValueWord(node.id, lane, TEMP0))))
					{
						return false;
					}
					unsigned left_direct_s = 0;
					unsigned right_direct_s = 0;
					if (ScalarVfpLane(left, lane, &left_direct_s) &&
						ScalarVfpLane(right, lane, &right_direct_s) && direct_target)
					{
						m_result.vu0_direct_vfp_lanes++;
					}
				}
				return !direct_target || CommitVector(node.id, target.index);
			}

			bool EmitVu0ClampFmacResult(const Node& node)
			{
				if (node.operand_count != 1 || node.type != ValueType::VuF32x4Bits ||
					node.immediate > 0x0f)
				{
					return false;
				}
				const RegionAllocation::Location& target = Location(node.id);
				const RegionAllocation::Location& source = Location(node.operands[0]);
				const bool full_vector_target = target.kind ==
						RegionAllocation::LocationKind::NeonQ ||
					(target.kind == RegionAllocation::LocationKind::Spill &&
					 LocationMask(target) == 0x0f);
				const bool vector_path = full_vector_target &&
					(node.immediate == 0x0f || source.kind !=
						RegionAllocation::LocationKind::NeonQ ||
					 source.index != target.index);
				if (vector_path)
				{
					unsigned raw = 0;
					const unsigned output =
						target.kind == RegionAllocation::LocationKind::NeonQ ?
							target.index : VECTOR_SCRATCH0;
					if (!AcquireVector(node.operands[0], VECTOR_SCRATCH0, &raw) ||
						(output != raw &&
						 !m_code.EmitVorrQ(output, raw, raw)) ||
						!EmitNormalizeVuQuad(output))
					{
						return false;
					}
					if (node.immediate != 0x0f)
					{
						// target currently holds normalized lanes.  Insert the original raw
						// lanes selected by the complement of the architectural xyzw mask.
						if (!m_code.EmitVeorQ(VU0_MASK_Q, VU0_MASK_Q, VU0_MASK_Q) ||
							!m_code.EmitVceqI32ZeroQ(VU0_MASK_Q, VU0_MASK_Q) ||
							!m_code.EmitMovImm8(TEMP0, 0))
						{
							return false;
						}
						for (u8 lane = 0; lane < 4; lane++)
						{
							if ((node.immediate & (1u << (3u - lane))) != 0 &&
								!m_code.EmitVmovCoreToD32Lane(
									VU0_MASK_Q * 2 + lane / 2, lane & 1u, TEMP0))
							{
								return false;
							}
						}
						if (!m_code.EmitVbitQ(output, raw, VU0_MASK_Q))
							return false;
					}
					return CommitVector(node.id, output);
				}

				const u8 demand = LocationMask(target);
				for (u8 lane = 0; lane < 4; lane++)
				{
					if ((demand & (1u << lane)) == 0)
						continue;
					if (!EmitLoadValueWord(node.operands[0], lane, TEMP0) ||
						((node.immediate & (1u << (3u - lane))) != 0 &&
						 !EmitNormalizeVuWord(TEMP0)) ||
						!CommitValueWord(node.id, lane, TEMP0))
					{
						return false;
					}
				}
				return true;
			}

			bool EmitVu0FullMacFlagsFromRaw(ValueId raw, ValueId output, u8 mask)
			{
				if (raw == INVALID_VALUE || output == INVALID_VALUE || mask > 0x0f)
					return false;
				const RegionAllocation::Location& target = Location(output);
				const bool resident = target.kind ==
					RegionAllocation::LocationKind::Core &&
					(LocationMask(target) & 1u) != 0;
				const unsigned flags = resident ? CoreRegister(target, 0) : TEMP1;
				if (!m_code.EmitMovImm8(flags, 0) ||
					(resident && !m_code.EmitMovImm32(TEMP1, VU_FLOAT_EXPONENT)))
				{
					return false;
				}
				for (u8 lane = 0; lane < 4; lane++)
				{
					const u32 shift = 3u - lane;
					if ((mask & (1u << shift)) == 0)
						continue;
					if (!EmitLoadValueWord(raw, lane, TEMP0) ||
						!m_code.EmitTstImm32(TEMP0, VU_FLOAT_SIGN) ||
						!m_code.EmitOrrImm32(flags, flags, 0x0010u << shift,
							false, Condition::NE) ||
						!m_code.EmitBicImm32(TEMP2, TEMP0, VU_FLOAT_SIGN, true))
					{
						return false;
					}
					const size_t nonzero =
						m_code.EmitBranchPlaceholder(Condition::NE);
					if (nonzero == static_cast<size_t>(-1) ||
						!m_code.EmitOrrImm32(flags, flags, 0x0001u << shift))
					{
						return false;
					}
					const size_t done_from_zero = m_code.EmitBranchPlaceholder();
					if (done_from_zero == static_cast<size_t>(-1) ||
						!m_code.PatchBranch(nonzero, m_code.Size(), Condition::NE) ||
						(!resident && !m_code.EmitMovImm32(TEMP2,
							VU_FLOAT_EXPONENT)) ||
						!m_code.EmitAndReg(TEMP2, TEMP0,
							resident ? TEMP1 : TEMP2) ||
						!m_code.EmitCmpImm32(TEMP2, 0))
					{
						return false;
					}
					// A nonzero exponent-zero value is a denormal: U and Z are set
					// together. Predication avoids a second per-lane control-flow diamond.
					if (!m_code.EmitOrrImm32(flags, flags, 0x0100u << shift,
							false, Condition::EQ) ||
						!m_code.EmitOrrImm32(flags, flags, 0x0001u << shift,
							false, Condition::EQ))
					{
						return false;
					}
					if ((!resident &&
						 !m_code.EmitMovImm32(TEMP0, VU_FLOAT_EXPONENT)) ||
						!m_code.EmitCmpReg(TEMP2, resident ? TEMP1 : TEMP0) ||
						!m_code.EmitOrrImm32(flags, flags, 0x1000u << shift,
							false, Condition::EQ))
					{
						return false;
					}
					const size_t done = m_code.Size();
					if (!m_code.PatchBranch(done_from_zero, done))
					{
						return false;
					}
				}
				return CommitValueWord(output, 0, flags);
			}

			bool EmitVu0MacFlagsFromRaw(const Node& node)
			{
				if (node.operand_count != 1 || node.type != ValueType::I32 ||
					node.immediate > 0x0f)
				{
					return false;
				}
				if (node.immediate != 0x0f)
					return EmitVu0FullMacFlagsFromRaw(node.operands[0], node.id,
						static_cast<u8>(node.immediate));

				unsigned raw_q = 0;
				if (!AcquireVector(node.operands[0], VECTOR_SCRATCH0, &raw_q))
					return false;
				const RegionAllocation::Location& target = Location(node.id);
				const bool resident = target.kind ==
					RegionAllocation::LocationKind::Core &&
					(LocationMask(target) & 1u) != 0;
				const unsigned flags = resident ? CoreRegister(target, 0) : TEMP1;
				const bool packed_signs = !m_remap_allocated_neon_high_bank;
				if (!m_code.EmitVandQ(VECTOR_SCRATCH1, raw_q, VU0_EXPONENT_Q) ||
					!m_code.EmitVceqI32ZeroQ(VU0_MASK_Q, VECTOR_SCRATCH1) ||
					!m_code.EmitVceqI32Q(VECTOR_SCRATCH1, VECTOR_SCRATCH1,
						VU0_EXPONENT_Q) ||
					!m_code.EmitVorrQ(VU0_MASK_Q, VU0_MASK_Q, VECTOR_SCRATCH1) ||
					!m_code.EmitVpaddI32D(VU0_MASK_Q * 2, VU0_MASK_Q * 2,
						VU0_MASK_Q * 2 + 1) ||
					!m_code.EmitVpaddI32D(VU0_MASK_Q * 2, VU0_MASK_Q * 2,
						VU0_MASK_Q * 2) ||
					!m_code.EmitVmovD32LaneToCore(TEMP0, VU0_MASK_Q * 2, 0) ||
					!m_code.EmitCmpImm32(TEMP0, 0))
				{
					return false;
				}
				const size_t exceptional =
					m_code.EmitBranchPlaceholder(Condition::NE);
				if (exceptional == static_cast<size_t>(-1) ||
					(!packed_signs && !m_code.EmitMovImm8(flags, 0)))
				{
					return false;
				}
				if (packed_signs)
				{
					// Finite nonzero values can contribute only the S flag. Convert
					// each sign to 0/1, place lanes xyzw at bits 7..4, then reduce.
					if (!m_code.EmitVshrU32Q(VU0_MASK_Q, raw_q, 31) ||
						!m_code.EmitVshlU32Q(VU0_MASK_Q, VU0_MASK_Q,
							VU0_MAC_SIGN_SHIFTS_Q) ||
						!m_code.EmitVpaddI32D(VU0_MASK_Q * 2,
							VU0_MASK_Q * 2, VU0_MASK_Q * 2 + 1) ||
						!m_code.EmitVpaddI32D(VU0_MASK_Q * 2,
							VU0_MASK_Q * 2, VU0_MASK_Q * 2) ||
						!m_code.EmitVmovD32LaneToCore(flags,
							VU0_MASK_Q * 2, 0))
					{
						return false;
					}
				}
				else
				{
					for (u8 lane = 0; lane < 4; lane++)
					{
						const u32 shift = 3u - lane;
						if (!m_code.EmitVmovD32LaneToCore(TEMP0,
								raw_q * 2 + lane / 2, lane & 1u) ||
							!m_code.EmitTstImm32(TEMP0, VU_FLOAT_SIGN) ||
							!m_code.EmitOrrImm32(flags, flags, 0x0010u << shift,
								false, Condition::NE))
						{
							return false;
						}
					}
				}
				if (!CommitValueWord(node.id, 0, flags))
					return false;
				const ValueId deferred_clamp = node.id <
						m_allocation.vu0_mac_deferred_clamp.size() ?
					m_allocation.vu0_mac_deferred_clamp[node.id] : INVALID_VALUE;
				m_vu0_mac_cold_paths.push_back({exceptional, m_code.Size(),
					node.operands[0], node.id, 0x0f, deferred_clamp});
				return true;
			}

			bool EmitVu0FullStatusFromRaw(ValueId raw, ValueId output,
				unsigned* status_register = nullptr)
			{
				unsigned raw_q = 0;
				if (!AcquireVector(raw, VECTOR_SCRATCH0, &raw_q))
					return false;
				const RegionAllocation::Location& target = Location(output);
				const bool resident = target.kind ==
					RegionAllocation::LocationKind::Core &&
					(LocationMask(target) & 1u) != 0;
				const unsigned status = resident ? CoreRegister(target, 0) : TEMP1;
				auto reduce_mask = [&](unsigned mask_q, u32 status_bit) {
					return m_code.EmitVpaddI32D(mask_q * 2,
							mask_q * 2, mask_q * 2 + 1) &&
					       m_code.EmitVpaddI32D(mask_q * 2,
							mask_q * 2, mask_q * 2) &&
					       m_code.EmitVmovD32LaneToCore(TEMP0,
							mask_q * 2, 0) &&
					       m_code.EmitCmpImm32(TEMP0, 0) &&
					       m_code.EmitOrrImm32(status, status, status_bit,
							false, Condition::NE);
				};
				if (!m_code.EmitMovImm8(status, 0) ||
					!m_code.EmitVcltS32ZeroQ(VU0_MASK_Q, raw_q) ||
					!reduce_mask(VU0_MASK_Q, 0x2u) ||
					!m_code.EmitVandQ(VECTOR_SCRATCH1, raw_q,
						VU0_EXPONENT_Q) ||
					!m_code.EmitVceqI32ZeroQ(VU0_MASK_Q, VECTOR_SCRATCH1) ||
					!reduce_mask(VU0_MASK_Q, 0x1u) ||
					!m_code.EmitVceqI32Q(VU0_MASK_Q, VECTOR_SCRATCH1,
						VU0_EXPONENT_Q) ||
					!reduce_mask(VU0_MASK_Q, 0x8u) ||
					!m_code.EmitVceqI32ZeroQ(VU0_MASK_Q, VECTOR_SCRATCH1) ||
					!m_code.EmitVandQ(VECTOR_SCRATCH1, raw_q, VU0_SIGN_Q) ||
					!m_code.EmitVmvnQ(VECTOR_SCRATCH1, VECTOR_SCRATCH1) ||
					!m_code.EmitVandQ(VECTOR_SCRATCH1, raw_q, VECTOR_SCRATCH1) ||
					!m_code.EmitVceqI32ZeroQ(VECTOR_SCRATCH1, VECTOR_SCRATCH1) ||
					!m_code.EmitVmvnQ(VECTOR_SCRATCH1, VECTOR_SCRATCH1) ||
					!m_code.EmitVandQ(VU0_MASK_Q, VU0_MASK_Q, VECTOR_SCRATCH1) ||
					!reduce_mask(VU0_MASK_Q, 0x4u))
				{
					return false;
				}
				if (status_register)
					*status_register = status;
				return (LocationMask(target) & 0x1u) == 0 ||
				       CommitValueWord(output, 0, status);
			}

			bool EmitVu0SyncStatusValue(const Node& sync, unsigned status)
			{
				if (sync.opcode != Opcode::Vu0SyncStatusControl ||
					sync.operand_count != 2 || sync.type != ValueType::I32)
				{
					return false;
				}
				const bool folded_predecessor = sync.id <
						m_allocation.vu0_sync_folded_predecessor.size() &&
					m_allocation.vu0_sync_folded_predecessor[sync.id] != INVALID_VALUE;
				const bool folded_into = sync.id <
						m_allocation.vu0_sync_folded_into.size() &&
					m_allocation.vu0_sync_folded_into[sync.id] != INVALID_VALUE;
				const RegionAllocation::Location& target = Location(sync.id);
				const bool resident = target.kind ==
					RegionAllocation::LocationKind::Core &&
					(LocationMask(target) & 1u) != 0;
				const unsigned control = resident ? CoreRegister(target, 0) : TEMP0;
				return EmitLoadValueWord(sync.operands[0], 0, control) &&
				       (folded_predecessor ||
					m_code.EmitAndImm32(control, control, 0x0fc0u)) &&
				       m_code.EmitOrrRegShiftImm(control, control, status,
					ShiftType::LSL, 6) &&
				       (folded_into || m_code.EmitOrrReg(control, control, status)) &&
				       CommitValueWord(sync.id, 0, control);
			}

			bool EmitVu0FusedStatusSync(const Node& sync, ValueId raw)
			{
				if (sync.operand_count != 2 || sync.type != ValueType::I32 ||
					raw == INVALID_VALUE || sync.operands[1] >= m_program.value_count ||
					sync.id >= m_allocation.vu0_sync_folded_into.size() ||
					m_allocation.vu0_sync_folded_into[sync.id] == INVALID_VALUE)
				{
					return false;
				}
				unsigned raw_q = 0;
				if (!AcquireVector(raw, VECTOR_SCRATCH0, &raw_q) ||
					!m_code.EmitVandQ(VECTOR_SCRATCH1, raw_q, VU0_EXPONENT_Q) ||
					!m_code.EmitVceqI32ZeroQ(VU0_MASK_Q, VECTOR_SCRATCH1) ||
					!m_code.EmitVceqI32Q(VECTOR_SCRATCH1, VECTOR_SCRATCH1,
						VU0_EXPONENT_Q) ||
					!m_code.EmitVorrQ(VU0_MASK_Q, VU0_MASK_Q, VECTOR_SCRATCH1) ||
					!m_code.EmitVandQ(VECTOR_SCRATCH1, raw_q, VU0_SIGN_Q) ||
					!m_code.EmitVorrQ(VU0_MASK_Q, VU0_MASK_Q, VECTOR_SCRATCH1) ||
					!m_code.EmitVpmaxU32D(VU0_MASK_Q * 2,
						VU0_MASK_Q * 2, VU0_MASK_Q * 2 + 1) ||
					!m_code.EmitVpmaxU32D(VU0_MASK_Q * 2,
						VU0_MASK_Q * 2, VU0_MASK_Q * 2) ||
					!m_code.EmitVmovD32LaneToCore(TEMP0, VU0_MASK_Q * 2, 0) ||
					!m_code.EmitBicImm32(TEMP1, TEMP0, VU_FLOAT_SIGN, true))
				{
					return false;
				}
				const size_t exceptional =
					m_code.EmitBranchPlaceholder(Condition::NE);
				const bool folded_predecessor = sync.id <
						m_allocation.vu0_sync_folded_predecessor.size() &&
					m_allocation.vu0_sync_folded_predecessor[sync.id] != INVALID_VALUE;
				const RegionAllocation::Location& target = Location(sync.id);
				const bool resident = target.kind ==
					RegionAllocation::LocationKind::Core &&
					(LocationMask(target) & 1u) != 0;
				const unsigned control = resident ? CoreRegister(target, 0) : TEMP1;
				if (exceptional == static_cast<size_t>(-1) ||
					!EmitLoadValueWord(sync.operands[0], 0, control) ||
					(!folded_predecessor &&
					 !m_code.EmitAndImm32(control, control, 0x0fc0u)) ||
					!m_code.EmitOrrRegShiftImm(control, control, TEMP0,
						ShiftType::LSR, 24) ||
					!CommitValueWord(sync.id, 0, control))
				{
					return false;
				}
				const ValueId status = sync.operands[1];
				const ValueId deferred_clamp = status <
						m_allocation.vu0_status_deferred_clamp.size() ?
					m_allocation.vu0_status_deferred_clamp[status] : INVALID_VALUE;
				m_vu0_status_cold_paths.push_back({exceptional, m_code.Size(), raw,
					status, deferred_clamp, sync.id});
				return true;
			}

			bool EmitVu0StatusFlagsFromMac(const Node& node)
			{
				const ValueId raw = node.id <
						m_allocation.vu0_status_direct_raw.size() ?
					m_allocation.vu0_status_direct_raw[node.id] : INVALID_VALUE;
				if (raw != INVALID_VALUE)
				{
					if (node.operand_count != 1 || node.type != ValueType::I32)
						return false;
					unsigned raw_q = 0;
					if (!AcquireVector(raw, VECTOR_SCRATCH0, &raw_q))
						return false;
					const RegionAllocation::Location& target = Location(node.id);
					const bool resident = target.kind ==
						RegionAllocation::LocationKind::Core &&
						(LocationMask(target) & 1u) != 0;
					const unsigned status = resident ? CoreRegister(target, 0) : TEMP1;
					if (!m_code.EmitVandQ(VECTOR_SCRATCH1, raw_q,
							VU0_EXPONENT_Q) ||
						!m_code.EmitVceqI32ZeroQ(VU0_MASK_Q,
							VECTOR_SCRATCH1) ||
						!m_code.EmitVceqI32Q(VECTOR_SCRATCH1,
							VECTOR_SCRATCH1, VU0_EXPONENT_Q) ||
						!m_code.EmitVorrQ(VU0_MASK_Q, VU0_MASK_Q,
							VECTOR_SCRATCH1) ||
						!m_code.EmitVandQ(VECTOR_SCRATCH1, raw_q, VU0_SIGN_Q) ||
						!m_code.EmitVorrQ(VU0_MASK_Q, VU0_MASK_Q,
							VECTOR_SCRATCH1) ||
						!m_code.EmitVpmaxU32D(VU0_MASK_Q * 2,
							VU0_MASK_Q * 2, VU0_MASK_Q * 2 + 1) ||
						!m_code.EmitVpmaxU32D(VU0_MASK_Q * 2,
							VU0_MASK_Q * 2, VU0_MASK_Q * 2) ||
						!m_code.EmitVmovD32LaneToCore(TEMP0, VU0_MASK_Q * 2, 0) ||
						!m_code.EmitBicImm32(TEMP1, TEMP0, VU_FLOAT_SIGN, true))
					{
						return false;
					}
					const size_t exceptional =
						m_code.EmitBranchPlaceholder(Condition::NE);
					if (exceptional == static_cast<size_t>(-1) ||
						!m_code.EmitMovImm8(status, 0) ||
						!m_code.EmitTstImm32(TEMP0, VU_FLOAT_SIGN) ||
						!m_code.EmitOrrImm32(status, status, 0x2u,
							false, Condition::NE) ||
						!CommitValueWord(node.id, 0, status))
					{
						return false;
					}
					const ValueId deferred_clamp = node.id <
							m_allocation.vu0_status_deferred_clamp.size() ?
						m_allocation.vu0_status_deferred_clamp[node.id] : INVALID_VALUE;
					m_vu0_status_cold_paths.push_back(
						{exceptional, m_code.Size(), raw, node.id, deferred_clamp});
					return true;
				}

				if (node.operand_count != 1 || node.type != ValueType::I32)
				{
					return false;
				}
				unsigned mac = 0;
				if (!AcquireValueWord(node.operands[0], 0, TEMP0, &mac))
					return false;
				const RegionAllocation::Location& target = Location(node.id);
				const bool resident = target.kind ==
					RegionAllocation::LocationKind::Core &&
					(LocationMask(target) & 1u) != 0;
				const unsigned status = resident ? CoreRegister(target, 0) : TEMP1;
				auto finite_mac = std::find_if(m_vu0_mac_cold_paths.begin(),
					m_vu0_mac_cold_paths.end(), [&](const Vu0MacColdPath& path) {
						return path.output == node.operands[0] && path.mask == 0x0f &&
						       path.fused_status == INVALID_VALUE;
					});
				if (finite_mac != m_vu0_mac_cold_paths.end())
				{
					// The preceding hot MAC classifier proved that every lane is finite
					// and nonzero. Its only possible bits are therefore S[xyzw], so the
					// architectural STATUS is exactly S=(MAC != 0). Exceptional lanes
					// already branch to the full MAC leaf; that leaf computes the complete
					// STATUS before rejoining after this hot reduction.
					if (!m_code.EmitMovImm8(status, 0) ||
						!m_code.EmitCmpImm32(mac, 0) ||
						!m_code.EmitMovImm8(status, 0x2u, Condition::NE) ||
						!CommitValueWord(node.id, 0, status))
					{
						return false;
					}
					finite_mac->fused_status = node.id;
					finite_mac->hot_continuation = m_code.Size();
					return true;
				}
				return EmitVu0StatusFromMacValue(node.operands[0], node.id);
			}

			bool EmitVu0StatusFromMacValue(ValueId mac_value, ValueId output)
			{
				unsigned mac = 0;
				if (!AcquireValueWord(mac_value, 0, TEMP0, &mac))
					return false;
				const RegionAllocation::Location& target = Location(output);
				const bool resident = target.kind ==
					RegionAllocation::LocationKind::Core &&
					(LocationMask(target) & 1u) != 0;
				const unsigned status = resident ? CoreRegister(target, 0) : TEMP1;
				if (!m_code.EmitMovImm8(status, 0))
					return false;
				constexpr std::array<std::pair<u32, u32>, 4> GROUPS = {{
					{0x000fu, 0x1u}, {0x00f0u, 0x2u},
					{0x0f00u, 0x4u}, {0xf000u, 0x8u},
				}};
				for (const auto& [mask, bit] : GROUPS)
				{
					if (!m_code.EmitTstImm32(mac, mask) ||
						!m_code.EmitOrrImm32(status, status, bit,
							false, Condition::NE))
					{
						return false;
					}
				}
				return CommitValueWord(output, 0, status);
			}

			bool EmitVu0StatusColdPaths()
			{
				for (const Vu0StatusColdPath& path : m_vu0_status_cold_paths)
				{
					unsigned status = TEMP1;
					if (path.exceptional_branch == static_cast<size_t>(-1) ||
						path.hot_continuation == static_cast<size_t>(-1) ||
						path.raw == INVALID_VALUE || path.output == INVALID_VALUE ||
						!m_code.PatchBranch(path.exceptional_branch, m_code.Size(),
							Condition::NE) ||
						!EmitVu0FullStatusFromRaw(path.raw, path.output, &status))
					{
						return false;
					}
					if (path.fused_sync != INVALID_VALUE)
					{
						const Node* const sync = Definition(path.fused_sync);
						if (!sync || !EmitVu0SyncStatusValue(*sync, status))
							return false;
					}
					if (path.deferred_clamp != INVALID_VALUE)
					{
						unsigned raw_q = 0;
						if (!AcquireVector(path.raw, VECTOR_SCRATCH0, &raw_q) ||
							!EmitNormalizeVuQuad(raw_q) ||
							!CommitVector(path.deferred_clamp, raw_q))
						{
							return false;
						}
					}
					const size_t resume = m_code.EmitBranchPlaceholder();
					if (resume == static_cast<size_t>(-1) ||
						!m_code.PatchBranch(resume, path.hot_continuation))
					{
						return false;
					}
				}
				return true;
			}

			bool EmitVu0MacColdPaths()
			{
				for (const Vu0MacColdPath& path : m_vu0_mac_cold_paths)
				{
					if (path.exceptional_branch == static_cast<size_t>(-1) ||
						path.hot_continuation == static_cast<size_t>(-1) ||
						path.raw == INVALID_VALUE || path.output == INVALID_VALUE ||
						!m_code.PatchBranch(path.exceptional_branch, m_code.Size(),
							Condition::NE) ||
						!EmitVu0FullMacFlagsFromRaw(path.raw, path.output, path.mask))
					{
						return false;
					}
					if (path.deferred_clamp != INVALID_VALUE)
					{
						unsigned raw_q = 0;
						if (!AcquireVector(path.raw, VECTOR_SCRATCH0, &raw_q) ||
							!EmitNormalizeVuQuad(raw_q) ||
							!CommitVector(path.deferred_clamp, raw_q))
						{
							return false;
						}
					}
					if (path.fused_status != INVALID_VALUE &&
						!EmitVu0StatusFromMacValue(path.output, path.fused_status))
					{
						return false;
					}
					const size_t resume = m_code.EmitBranchPlaceholder();
					if (resume == static_cast<size_t>(-1) ||
						!m_code.PatchBranch(resume, path.hot_continuation))
					{
						return false;
					}
				}
				return true;
			}

			bool EmitVu0MergeMasked(const Node& node)
			{
				if (node.operand_count != 2 || node.type != ValueType::VuF32x4Bits ||
					node.immediate == 0 || node.immediate > 0x0f)
				{
					return false;
				}
				const RegionAllocation::Location& target = Location(node.id);
				if (target.kind == RegionAllocation::LocationKind::NeonQ)
				{
					unsigned old_value = 0;
					unsigned new_value = 0;
					if (!AcquireVector(node.operands[0], VECTOR_SCRATCH0, &old_value) ||
						!AcquireVector(node.operands[1], VECTOR_SCRATCH1, &new_value) ||
						!m_code.EmitVeorQ(VU0_MASK_Q, VU0_MASK_Q, VU0_MASK_Q) ||
						!m_code.EmitVceqI32ZeroQ(VU0_MASK_Q, VU0_MASK_Q) ||
						!m_code.EmitMovImm8(TEMP0, 0))
					{
						return false;
					}
					for (u8 lane = 0; lane < 4; lane++)
					{
						if ((node.immediate & (1u << (3u - lane))) == 0 &&
							!m_code.EmitVmovCoreToD32Lane(
								VU0_MASK_Q * 2 + lane / 2, lane & 1u, TEMP0))
						{
							return false;
						}
					}
					if (target.index == old_value)
					{
						if (!m_code.EmitVbitQ(target.index, new_value, VU0_MASK_Q))
							return false;
					}
					else if (target.index == new_value)
					{
						if (!m_code.EmitVmvnQ(VU0_MASK_Q, VU0_MASK_Q) ||
							!m_code.EmitVbitQ(target.index, old_value, VU0_MASK_Q))
						{
							return false;
						}
					}
					else if (!m_code.EmitVorrQ(target.index, old_value, old_value) ||
						!m_code.EmitVbitQ(target.index, new_value, VU0_MASK_Q))
					{
						return false;
					}
					return CommitVector(node.id, target.index);
				}

				const u8 demand = LocationMask(target);
				for (u8 lane = 0; lane < 4; lane++)
				{
					const ValueId source =
						(node.immediate & (1u << (3u - lane))) != 0 ?
							node.operands[1] : node.operands[0];
					if ((demand & (1u << lane)) != 0 &&
						(!EmitLoadValueWord(source, lane, TEMP0) ||
						 !CommitValueWord(node.id, lane, TEMP0)))
					{
						return false;
					}
				}
				return true;
			}

			bool EmitVu0SyncStatusControl(const Node& node)
			{
				if (node.operand_count != 2 || node.type != ValueType::I32)
					return false;
				const ValueId direct_raw = node.id <
						m_allocation.vu0_sync_direct_raw.size() ?
					m_allocation.vu0_sync_direct_raw[node.id] : INVALID_VALUE;
				if (direct_raw != INVALID_VALUE)
					return EmitVu0FusedStatusSync(node, direct_raw);
				const bool folded_predecessor = node.id <
						m_allocation.vu0_sync_folded_predecessor.size() &&
					m_allocation.vu0_sync_folded_predecessor[node.id] != INVALID_VALUE;
				const bool folded_into = node.id <
						m_allocation.vu0_sync_folded_into.size() &&
					m_allocation.vu0_sync_folded_into[node.id] != INVALID_VALUE;
				if (!EmitLoadValueWord(node.operands[0], 0, TEMP0) ||
					(!folded_predecessor &&
					 !m_code.EmitAndImm32(TEMP0, TEMP0, 0x0fc0u)) ||
					!EmitLoadValueWord(node.operands[1], 0, TEMP1) ||
					!m_code.EmitOrrRegShiftImm(TEMP0, TEMP0, TEMP1,
						ShiftType::LSL, 6))
				{
					return false;
				}
				// An intermediate is consumed only by the next synchronization and
				// therefore carries sticky bits without the architecturally current low
				// nibble. The final node restores that nibble. This keeps status values
				// short-lived while preserving the exact repeated SYNCMSFLAGS algebra.
				if (!folded_into && !m_code.EmitOrrReg(TEMP0, TEMP0, TEMP1))
				{
					return false;
				}
				return CommitValueWord(node.id, 0, TEMP0);
			}

			bool EmitVu0ControlWrite(const Node& node)
			{
				if (node.operand_count != 2 || node.type != ValueType::I32 ||
					node.immediate >= 32)
				{
					return false;
				}
				constexpr u32 STATUS = 16;
				constexpr u32 R = 20;
				const u32 target = node.immediate;
				const unsigned output = DestinationHost(node.id, 0, TEMP2);
				if (target == STATUS)
				{
					unsigned old_value = 0;
					unsigned source = 0;
					if (!AcquireValueWord(node.operands[0], 0, TEMP0, &old_value))
					{
						m_result.failure_emission_step = 40;
						return false;
					}
					if (!AcquireValueWord(node.operands[1], 0, TEMP1, &source))
					{
						m_result.failure_emission_step = 41;
						return false;
					}
					if (!m_code.EmitAndImm32(TEMP0, old_value, 0x3fu))
					{
						m_result.failure_emission_step = 42;
						return false;
					}
					if (!m_code.EmitAndImm32(TEMP1, source, 0x0fc0u))
					{
						m_result.failure_emission_step = 43;
						return false;
					}
					if (!m_code.EmitOrrReg(output, TEMP0, TEMP1))
					{
						m_result.failure_emission_step = 44;
						return false;
					}
					if (!CommitValueWord(node.id, 0, output))
					{
						m_result.failure_emission_step = 45;
						return false;
					}
					return true;
				}

				unsigned source = 0;
				if (!AcquireValueWord(node.operands[1], 0, TEMP1, &source) ||
					!m_code.EmitMovRegShiftImm(TEMP1, source, ShiftType::LSL, 0))
					return false;
				source = TEMP1;
				bool emitted = false;
				if (target < STATUS)
				{
					unsigned old_value = 0;
					emitted = AcquireValueWord(node.operands[0], 0, TEMP0, &old_value) &&
						m_code.EmitMovRegShiftImm(output, old_value,
							ShiftType::LSL, 0) &&
						m_code.EmitBfi(output, source, 0, 16);
				}
				else if (target == R)
				{
					emitted = m_code.EmitUbfx(output, source, 0, 23) &&
						m_code.EmitOrrImm32(output, output, 0x3f800000u);
				}
				else
					emitted = m_code.EmitMovRegShiftImm(output, source,
						ShiftType::LSL, 0);
				return emitted && CommitValueWord(node.id, 0, output);
			}

			bool EmitVu0DenormalizeStatus(const Node& node)
			{
				if (node.operand_count != 1 || node.type != ValueType::I32)
					return false;
				// mVUallocSFLAGd() scatters four normalized STATUS fields into the
				// microVU status representation.  Express those fields explicitly:
				// the combined 0x03cf0000 mask is not an A32 modified immediate, and
				// materializing it would consume a third scratch register.  UBFX plus
				// shifted ORRs is both allocation-independent and one instruction per
				// source field on Cortex-A9.
				if (!EmitLoadValueWord(node.operands[0], 0, TEMP0))
					return false;
				const unsigned output = DestinationHost(node.id, 0, TEMP2);
				return m_code.EmitUbfx(output, TEMP0, 6, 2) &&
				       m_code.EmitMovRegShiftImm(output, output, ShiftType::LSL, 3) &&
				       m_code.EmitUbfx(TEMP1, TEMP0, 0, 2) &&
				       m_code.EmitOrrRegShiftImm(output, output, TEMP1,
					   ShiftType::LSL, 11) &&
				       m_code.EmitUbfx(TEMP1, TEMP0, 2, 4) &&
				       m_code.EmitOrrRegShiftImm(output, output, TEMP1,
					   ShiftType::LSL, 16) &&
				       m_code.EmitUbfx(TEMP1, TEMP0, 8, 4) &&
				       m_code.EmitOrrRegShiftImm(output, output, TEMP1,
					   ShiftType::LSL, 22) &&
				       CommitValueWord(node.id, 0, output);
			}

			bool EmitBitwiseWord(BinaryKind kind, unsigned output,
				unsigned left, unsigned right)
			{
				switch (kind)
				{
					case BinaryKind::And:
						return m_code.EmitAndReg(output, left, right);
					case BinaryKind::Or:
						return m_code.EmitOrrReg(output, left, right);
					case BinaryKind::Xor:
						return m_code.EmitEorReg(output, left, right);
					case BinaryKind::Nor:
						return m_code.EmitOrrReg(output, left, right) &&
						       m_code.EmitMvnReg(output, output);
					case BinaryKind::Add:
					case BinaryKind::Subtract:
						return false;
				}
				return false;
			}

			bool EmitBinary128Scalar(const Node& node, BinaryKind kind)
			{
				const u8 mask = LocationMask(Location(node.id));
				u8 staged = 0;
				for (u8 word = 0; word < 4; word++)
				{
					if ((mask & (1u << word)) == 0)
						continue;
					if (staged >= 2 ||
						!EmitLoadValueWord(node.operands[0], word, TEMP0) ||
						!EmitSpillStore(TEMP0,
							m_work_scratch_offset + staged * 8) ||
						!EmitLoadValueWord(node.operands[1], word, TEMP0) ||
						!EmitSpillStore(TEMP0,
							m_work_scratch_offset + staged * 8 + 4))
					{
						return false;
					}
					staged++;
				}

				staged = 0;
				for (u8 word = 0; word < 4; word++)
				{
					if ((mask & (1u << word)) == 0)
						continue;
					const unsigned output = DestinationHost(node.id, word, TEMP2);
					if (!EmitSpillLoad(TEMP0,
							m_work_scratch_offset + staged * 8) ||
						!EmitSpillLoad(TEMP1,
							m_work_scratch_offset + staged * 8 + 4) ||
						!EmitBitwiseWord(kind, output, TEMP0, TEMP1) ||
						!CommitValueWord(node.id, word, output))
					{
						return false;
					}
					staged++;
				}
				return true;
			}

			bool EmitBinary128(const Node& node, BinaryKind kind)
			{
				if (node.operand_count != 2 || node.type != ValueType::I128)
					return false;
				const RegionAllocation::Location& target = Location(node.id);
				if (target.kind != RegionAllocation::LocationKind::NeonQ &&
					!(target.kind == RegionAllocation::LocationKind::Spill &&
						target.words == 4))
				{
					return EmitBinary128Scalar(node, kind);
				}

				unsigned left = 0;
				unsigned right = 0;
				if (!AcquireVector(node.operands[0], VECTOR_SCRATCH0, &left) ||
					!AcquireVector(node.operands[1], VECTOR_SCRATCH1, &right))
				{
					return false;
				}
				const unsigned output =
					target.kind == RegionAllocation::LocationKind::NeonQ ?
						target.index : VECTOR_SCRATCH0;
				bool emitted = false;
				switch (kind)
				{
					case BinaryKind::And:
						emitted = m_code.EmitVandQ(output, left, right);
						break;
					case BinaryKind::Or:
						emitted = m_code.EmitVorrQ(output, left, right);
						break;
					case BinaryKind::Xor:
						emitted = m_code.EmitVeorQ(output, left, right);
						break;
					case BinaryKind::Nor:
						emitted = m_code.EmitVorrQ(output, left, right) &&
							m_code.EmitVmvnQ(output, output);
						break;
					case BinaryKind::Add:
					case BinaryKind::Subtract:
						return false;
				}
				return emitted && CommitVector(node.id, output);
			}

			bool EmitPackedBinary128(const Node& node)
			{
				if (node.operand_count != 2 || node.type != ValueType::I128 ||
					node.immediate >= static_cast<u32>(PackedBinaryKind::Count))
				{
					return false;
				}

				unsigned left = 0;
				unsigned right = 0;
				if (!AcquireVector(node.operands[0], VECTOR_SCRATCH0, &left) ||
					!AcquireVector(node.operands[1], VECTOR_SCRATCH1, &right))
				{
					return false;
				}
				const RegionAllocation::Location& target = Location(node.id);
				const unsigned output =
					target.kind == RegionAllocation::LocationKind::NeonQ ?
						target.index : VECTOR_SCRATCH0;
				const PackedBinaryKind kind =
					static_cast<PackedBinaryKind>(node.immediate);

				// VZIP is destructive to both operands.  Never apply it directly to an
				// allocated input: an aliasing EE destination and a later live use must
				// continue to observe the pre-instruction snapshots.  q14/q15 are
				// backend-reserved scratch registers, so stage RS and RT there, zip in
				// architectural RT,RS order, and commit only the demanded half.
				if (kind == PackedBinaryKind::InterleaveLower32 ||
					kind == PackedBinaryKind::InterleaveUpper32)
				{
					if ((left != VECTOR_SCRATCH0 &&
							!m_code.EmitVorrQ(VECTOR_SCRATCH0, left, left)) ||
						(right != VECTOR_SCRATCH1 &&
							!m_code.EmitVorrQ(VECTOR_SCRATCH1, right, right)) ||
						!m_code.EmitVzipI32Q(VECTOR_SCRATCH1, VECTOR_SCRATCH0))
					{
						return false;
					}
					const unsigned selected =
						kind == PackedBinaryKind::InterleaveLower32 ?
							VECTOR_SCRATCH1 : VECTOR_SCRATCH0;
					if (target.kind == RegionAllocation::LocationKind::NeonQ ||
						(target.kind == RegionAllocation::LocationKind::Spill &&
							target.words == 4))
					{
						return CommitVector(node.id, selected);
					}

					const u8 mask = LocationMask(target);
					for (u8 word = 0; word < 4; word++)
					{
						if ((mask & (1u << word)) != 0 &&
							(!m_code.EmitVmovD32LaneToCore(TEMP0,
								selected * 2 + word / 2, word & 1u) ||
							 !CommitValueWord(node.id, word, TEMP0)))
						{
							return false;
						}
					}
					return true;
				}

				bool emitted = false;
				switch (kind)
				{
					case PackedBinaryKind::AddWrap8: emitted = m_code.EmitVaddI8Q(output, left, right); break;
					case PackedBinaryKind::AddWrap16: emitted = m_code.EmitVaddI16Q(output, left, right); break;
					case PackedBinaryKind::AddWrap32: emitted = m_code.EmitVaddI32Q(output, left, right); break;
					case PackedBinaryKind::SubtractWrap8: emitted = m_code.EmitVsubI8Q(output, left, right); break;
					case PackedBinaryKind::SubtractWrap16: emitted = m_code.EmitVsubI16Q(output, left, right); break;
					case PackedBinaryKind::SubtractWrap32: emitted = m_code.EmitVsubI32Q(output, left, right); break;
					case PackedBinaryKind::CompareGreaterSigned8: emitted = m_code.EmitVcgtS8Q(output, left, right); break;
					case PackedBinaryKind::CompareGreaterSigned16: emitted = m_code.EmitVcgtS16Q(output, left, right); break;
					case PackedBinaryKind::CompareGreaterSigned32: emitted = m_code.EmitVcgtS32Q(output, left, right); break;
					case PackedBinaryKind::MaximumSigned16: emitted = m_code.EmitVmaxS16Q(output, left, right); break;
					case PackedBinaryKind::MaximumSigned32: emitted = m_code.EmitVmaxS32Q(output, left, right); break;
					case PackedBinaryKind::AddSaturateSigned8: emitted = m_code.EmitVqaddS8Q(output, left, right); break;
					case PackedBinaryKind::AddSaturateSigned16: emitted = m_code.EmitVqaddS16Q(output, left, right); break;
					case PackedBinaryKind::AddSaturateSigned32: emitted = m_code.EmitVqaddS32Q(output, left, right); break;
					case PackedBinaryKind::SubtractSaturateSigned8: emitted = m_code.EmitVqsubS8Q(output, left, right); break;
					case PackedBinaryKind::SubtractSaturateSigned16: emitted = m_code.EmitVqsubS16Q(output, left, right); break;
					case PackedBinaryKind::SubtractSaturateSigned32: emitted = m_code.EmitVqsubS32Q(output, left, right); break;
					case PackedBinaryKind::CompareEqual8: emitted = m_code.EmitVceqI8Q(output, left, right); break;
					case PackedBinaryKind::CompareEqual16: emitted = m_code.EmitVceqI16Q(output, left, right); break;
					case PackedBinaryKind::CompareEqual32: emitted = m_code.EmitVceqI32Q(output, left, right); break;
					case PackedBinaryKind::MinimumSigned16: emitted = m_code.EmitVminS16Q(output, left, right); break;
					case PackedBinaryKind::MinimumSigned32: emitted = m_code.EmitVminS32Q(output, left, right); break;
					case PackedBinaryKind::AddSaturateUnsigned8: emitted = m_code.EmitVqaddU8Q(output, left, right); break;
					case PackedBinaryKind::AddSaturateUnsigned16: emitted = m_code.EmitVqaddU16Q(output, left, right); break;
					case PackedBinaryKind::AddSaturateUnsigned32: emitted = m_code.EmitVqaddU32Q(output, left, right); break;
					case PackedBinaryKind::SubtractSaturateUnsigned8: emitted = m_code.EmitVqsubU8Q(output, left, right); break;
					case PackedBinaryKind::SubtractSaturateUnsigned16: emitted = m_code.EmitVqsubU16Q(output, left, right); break;
					case PackedBinaryKind::SubtractSaturateUnsigned32: emitted = m_code.EmitVqsubU32Q(output, left, right); break;
					case PackedBinaryKind::InterleaveLower32:
					case PackedBinaryKind::InterleaveUpper32:
						return false;
					case PackedBinaryKind::Count: return false;
				}
				if (!emitted)
					return false;

				if (target.kind == RegionAllocation::LocationKind::NeonQ ||
					(target.kind == RegionAllocation::LocationKind::Spill &&
						target.words == 4))
				{
					return CommitVector(node.id, output);
				}

				const u8 mask = LocationMask(target);
				for (u8 word = 0; word < 4; word++)
				{
					if ((mask & (1u << word)) != 0 &&
						(!m_code.EmitVmovD32LaneToCore(TEMP0,
							output * 2 + word / 2, word & 1u) ||
						 !CommitValueWord(node.id, word, TEMP0)))
					{
						return false;
					}
				}
				return true;
			}

			bool EmitPackedShift128(const Node& node)
			{
				if (node.operand_count != 1 || node.type != ValueType::I128 ||
					node.immediate >= static_cast<u32>(PackedShiftKind::Count))
				{
					return false;
				}
				const PackedShiftKind kind = static_cast<PackedShiftKind>(node.immediate);
				const u32 amount = static_cast<u32>(node.literal);
				if (amount >= (kind <= PackedShiftKind::RightArithmetic16 ? 16u : 32u))
					return false;

				unsigned source = 0;
				if (!AcquireVector(node.operands[0], VECTOR_SCRATCH0, &source))
					return false;
				const RegionAllocation::Location& target = Location(node.id);
				const unsigned output = target.kind == RegionAllocation::LocationKind::NeonQ ?
					target.index : VECTOR_SCRATCH0;
				bool emitted = false;
				if (amount == 0)
				{
					emitted = output == source || m_code.EmitVorrQ(output, source, source);
				}
				else switch (kind)
				{
					case PackedShiftKind::LeftLogical16: emitted = m_code.EmitVshlI16Q(output, source, amount); break;
					case PackedShiftKind::RightLogical16: emitted = m_code.EmitVshrU16Q(output, source, amount); break;
					case PackedShiftKind::RightArithmetic16: emitted = m_code.EmitVshrS16Q(output, source, amount); break;
					case PackedShiftKind::LeftLogical32: emitted = m_code.EmitVshlI32Q(output, source, amount); break;
					case PackedShiftKind::RightLogical32: emitted = m_code.EmitVshrU32Q(output, source, amount); break;
					case PackedShiftKind::RightArithmetic32: emitted = m_code.EmitVshrS32Q(output, source, amount); break;
					case PackedShiftKind::Count: return false;
				}
				if (!emitted)
					return false;
				if (target.kind == RegionAllocation::LocationKind::NeonQ ||
					(target.kind == RegionAllocation::LocationKind::Spill && target.words == 4))
				{
					return CommitVector(node.id, output);
				}
				const u8 mask = LocationMask(target);
				for (u8 word = 0; word < 4; word++)
				{
					if ((mask & (1u << word)) != 0 &&
						(!m_code.EmitVmovD32LaneToCore(TEMP0,
							output * 2 + word / 2, word & 1u) ||
						 !CommitValueWord(node.id, word, TEMP0)))
					{
						return false;
					}
				}
				return true;
			}

			bool EmitBroadcastLowHalfwordPer64(const Node& node)
			{
				if (node.operand_count != 1 || node.type != ValueType::I128)
					return false;
				const RegionAllocation::Location& target = Location(node.id);
				if (target.kind == RegionAllocation::LocationKind::NeonQ ||
					(target.kind == RegionAllocation::LocationKind::Spill &&
						target.words == 4))
				{
					unsigned source = 0;
					if (!AcquireVector(node.operands[0], VECTOR_SCRATCH0, &source))
						return false;
					const unsigned output =
						target.kind == RegionAllocation::LocationKind::NeonQ ?
							target.index : VECTOR_SCRATCH0;
					return m_code.EmitVdupI16D(output * 2, source * 2, 0) &&
					       m_code.EmitVdupI16D(output * 2 + 1,
						   source * 2 + 1, 0) &&
					       CommitVector(node.id, output);
				}

				const u8 mask = LocationMask(target);
				bool need_low = (mask & 0x3) != 0;
				bool need_high = (mask & 0x0c) != 0;
				if ((need_low &&
						(!EmitLoadValueWord(node.operands[0], 0, TEMP0) ||
						 !EmitSpillStore(TEMP0, m_work_scratch_offset))) ||
					(need_high &&
						(!EmitLoadValueWord(node.operands[0], 2, TEMP0) ||
						 !EmitSpillStore(TEMP0, m_work_scratch_offset + 4))))
				{
					return false;
				}
				for (u8 word = 0; word < 4; word++)
				{
					if ((mask & (1u << word)) == 0 ||
						!EmitSpillLoad(TEMP0, m_work_scratch_offset +
							(word < 2 ? 0 : 4)))
					{
						if ((mask & (1u << word)) != 0)
							return false;
						continue;
					}
					const unsigned output = DestinationHost(node.id, word, TEMP2);
					if (!m_code.EmitUxth(output, TEMP0) ||
						!m_code.EmitOrrRegShiftImm(output, output, output,
							ShiftType::LSL, 16) ||
						!CommitValueWord(node.id, word, output))
					{
						return false;
					}
				}
				return true;
			}

			bool EmitCopyMappings(ValueId target,
				const std::vector<WordMapping>& mappings)
			{
				std::vector<WordMapping> demanded;
				const u8 target_mask = LocationMask(Location(target));
				for (const WordMapping& mapping : mappings)
				{
					if ((target_mask & (1u << mapping.target_word)) != 0)
						demanded.push_back(mapping);
				}
				bool destructive = false;
				for (size_t target_index = 0; target_index < demanded.size(); target_index++)
				{
					const PhysicalWord target_word =
						Physical(Location(target), demanded[target_index].target_word);
					for (size_t source_index = target_index + 1;
						source_index < demanded.size(); source_index++)
					{
						if (target_word == PhysicalValueWord(
								demanded[source_index].source,
								demanded[source_index].source_word))
						{
							destructive = true;
						}
					}
				}
				if (destructive)
				{
					if (demanded.size() > 4)
						return false;
					for (size_t index = 0; index < demanded.size(); index++)
					{
						if (!EmitLoadValueWord(demanded[index].source,
								demanded[index].source_word, TEMP0) ||
							!EmitSpillStore(TEMP0,
								m_work_scratch_offset + static_cast<u32>(index) * 4))
						{
							return false;
						}
					}
					for (size_t index = 0; index < demanded.size(); index++)
					{
						if (!EmitSpillLoad(TEMP0,
								m_work_scratch_offset + static_cast<u32>(index) * 4) ||
							!EmitStoreValueWord(target,
								demanded[index].target_word, TEMP0))
						{
							return false;
						}
					}
					return true;
				}

				for (const WordMapping& mapping : demanded)
				{
					if (Location(target).kind ==
							RegionAllocation::LocationKind::Immediate &&
						Location(mapping.source).kind ==
							RegionAllocation::LocationKind::Immediate)
					{
						continue;
					}
					if (!EmitCopyLocationWord(Location(mapping.source), mapping.source,
							mapping.source_word, Location(target), mapping.target_word))
					{
						return false;
					}
				}
				return true;
			}

			bool EmitSignExtend(const Node& node, bool sign)
			{
				if (Location(node.id).kind ==
					RegionAllocation::LocationKind::Immediate)
				{
					return true;
				}
				const u8 mask = LocationMask(Location(node.id));
				unsigned source = 0;
				const unsigned low = DestinationHost(node.id, 0, TEMP1);
				const RegionAllocation::Location& source_location =
					Location(node.operands[0]);
				const bool source_is_core =
					source_location.kind == RegionAllocation::LocationKind::Core ||
					source_location.kind == RegionAllocation::LocationKind::FixedCycle;
				// Load an immediate/spilled/canonical low word straight into its
				// allocated destination.  Loading through TEMP0 and then MOVing into
				// that destination is pure transport overhead and is particularly
				// common for LUI-created loop invariants.
				if ((mask & 0x1) != 0 && low != TEMP1 && !source_is_core)
				{
					source = low;
					if (!EmitLoadValueWord(node.operands[0], 0, source))
					{
						m_result.failure_emission_step = 1;
						return false;
					}
				}
				else if (!AcquireValueWord(node.operands[0], 0, TEMP0, &source))
				{
					m_result.failure_emission_step = 1;
					return false;
				}
				if ((mask & 0x1) != 0)
				{
					if (!EmitMove(low, source))
					{
						m_result.failure_emission_step = 2;
						return false;
					}
					if (!CommitValueWord(node.id, 0, low))
					{
						m_result.failure_emission_step = 3;
						return false;
					}
				}
				if ((mask & 0x2) == 0)
					return true;
				const unsigned high = DestinationHost(node.id, 1, TEMP1);
				if (sign)
				{
					if (!m_code.EmitMovRegShiftImm(
						high, source, ShiftType::ASR, 31))
					{
						m_result.failure_emission_step = 4;
						return false;
					}
				}
				else if (!m_code.EmitMovImm8(high, 0))
				{
					m_result.failure_emission_step = 5;
					return false;
				}
				if (!CommitValueWord(node.id, 1, high))
				{
					m_result.failure_emission_step = 6;
					return false;
				}
				return true;
			}

			bool EmitMultiply32(const Node& node, bool sign)
			{
				unsigned left = 0;
				unsigned right = 0;
				if (!AcquireValueWord(node.operands[0], 0, TEMP0, &left) ||
					!AcquireValueWord(node.operands[1], 0, TEMP1, &right))
				{
					return false;
				}
				const u8 mask = LocationMask(Location(node.id));
				if (!((sign ? m_code.EmitSmull(TEMP2, TEMP0, left, right) :
					           m_code.EmitUmull(TEMP2, TEMP0, left, right))))
				{
					return false;
				}
				return ((mask & 0x1) == 0 || CommitValueWord(node.id, 0, TEMP2)) &&
				       ((mask & 0x2) == 0 || CommitValueWord(node.id, 1, TEMP0));
			}

			bool EmitBinary32(const Node& node, BinaryKind kind)
			{
				const bool publish_overflow_flags = kind == BinaryKind::Add &&
					node.id < m_allocation.signed_overflow_for_flagged_add.size() &&
					m_allocation.signed_overflow_for_flagged_add[node.id] != INVALID_VALUE;
				auto is_zero = [&](ValueId value) {
					const Node* definition = Definition(value);
					return definition &&
						(definition->opcode == Opcode::ConstantI1 ||
						 definition->opcode == Opcode::ConstantI32 ||
						 definition->opcode == Opcode::ConstantI64 ||
						 definition->opcode == Opcode::ConstantAddress) &&
						definition->literal == 0;
				};
				if ((kind == BinaryKind::Add || kind == BinaryKind::Or ||
						kind == BinaryKind::Xor || kind == BinaryKind::Subtract) &&
					is_zero(node.operands[1]))
				{
					return EmitCopyMappings(node.id, {{0, node.operands[0], 0}});
				}
				if ((kind == BinaryKind::Add || kind == BinaryKind::Or ||
						kind == BinaryKind::Xor) && is_zero(node.operands[0]))
				{
					return EmitCopyMappings(node.id, {{0, node.operands[1], 0}});
				}

				// Region IR deliberately keeps constants typed rather than folding
				// them into guest opcodes. Select A32 data-processing immediates here
				// when the other operand is already resident. This is the region-wide
				// equivalent of BlockCompiler::EmitADDIU()'s immediate ADD/SUB and
				// avoids rematerializing the same loop constant every iteration.
				auto emit_immediate = [&](ValueId source_value, ValueId immediate_value,
					bool immediate_on_left) -> int {
					const RegionAllocation::Location& source_location =
						Location(source_value);
					if (source_location.kind != RegionAllocation::LocationKind::Core)
						return 0;
					u32 immediate = 0;
					if (!ImmediateWord(immediate_value, 0, &immediate))
						return 0;
					const unsigned source = CoreRegister(source_location, 0);
					const unsigned output = DestinationHost(node.id, 0, TEMP2);
					bool emitted = false;
					switch (kind)
					{
					case BinaryKind::Add:
						emitted = m_code.EmitAddImm32(output, source, immediate,
							publish_overflow_flags);
							break;
						case BinaryKind::Subtract:
							if (!immediate_on_left)
								emitted = m_code.EmitSubImm32(output, source, immediate);
							break;
						case BinaryKind::And:
							emitted = m_code.EmitAndImm32(output, source, immediate);
							break;
						case BinaryKind::Or:
							emitted = m_code.EmitOrrImm32(output, source, immediate);
							break;
						case BinaryKind::Xor:
							emitted = m_code.EmitEorImm32(output, source, immediate);
							break;
						case BinaryKind::Nor:
							break;
					}
					if (!emitted)
						return 0;
					return CommitValueWord(node.id, 0, output) ? 1 : -1;
				};
				const int right_immediate = emit_immediate(
					node.operands[0], node.operands[1], false);
				if (right_immediate != 0)
					return right_immediate > 0;
				if (kind == BinaryKind::Add || kind == BinaryKind::And ||
					kind == BinaryKind::Or || kind == BinaryKind::Xor)
				{
					const int left_immediate = emit_immediate(
						node.operands[1], node.operands[0], true);
					if (left_immediate != 0)
						return left_immediate > 0;
				}
				unsigned left = 0;
				unsigned right = 0;
				if (!AcquireValueWord(node.operands[0], 0, TEMP0, &left) ||
					!AcquireValueWord(node.operands[1], 0, TEMP1, &right))
				{
					return false;
				}
				const unsigned output = DestinationHost(node.id, 0, TEMP2);
				bool emitted = false;
				switch (kind)
				{
					case BinaryKind::Add:
						emitted = m_code.EmitAddReg(output, left, right,
							publish_overflow_flags);
						break;
					case BinaryKind::Subtract:
						emitted = m_code.EmitSubReg(output, left, right);
						break;
					case BinaryKind::And:
						emitted = m_code.EmitAndReg(output, left, right);
						break;
					case BinaryKind::Or:
						emitted = m_code.EmitOrrReg(output, left, right);
						break;
					case BinaryKind::Xor:
						emitted = m_code.EmitEorReg(output, left, right);
						break;
					case BinaryKind::Nor:
						emitted = m_code.EmitOrrReg(output, left, right) &&
							m_code.EmitMvnReg(output, output);
						break;
				}
				return emitted && CommitValueWord(node.id, 0, output);
			}

			bool StageHighOperandsIfNeeded(const Node& node, bool* staged)
			{
				*staged = false;
				if ((LocationMask(Location(node.id)) & 0x1) == 0)
					return true;
				const PhysicalWord low_target = Physical(Location(node.id), 0);
				const bool hazard =
					low_target == PhysicalValueWord(node.operands[0], 1) ||
					low_target == PhysicalValueWord(node.operands[1], 1);
				if (!hazard)
					return true;
				if (!EmitLoadValueWord(node.operands[0], 1, TEMP0) ||
					!EmitSpillStore(TEMP0, m_work_scratch_offset) ||
					!EmitLoadValueWord(node.operands[1], 1, TEMP0) ||
					!EmitSpillStore(TEMP0, m_work_scratch_offset + 4))
				{
					return false;
				}
				*staged = true;
				return true;
			}

			bool EmitBinary64(const Node& node, BinaryKind kind)
			{
				const u8 mask = LocationMask(Location(node.id));
				bool high_staged = false;
				if ((mask & 0x2) != 0 && !StageHighOperandsIfNeeded(node, &high_staged))
					return false;

				if ((mask & 0x1) != 0 ||
					((mask & 0x2) != 0 &&
					 (kind == BinaryKind::Add || kind == BinaryKind::Subtract)))
				{
					unsigned left = 0;
					unsigned right = 0;
					if (!AcquireValueWord(node.operands[0], 0, TEMP0, &left) ||
						!AcquireValueWord(node.operands[1], 0, TEMP1, &right))
					{
						return false;
					}
					const unsigned output = (mask & 0x1) != 0 ?
						DestinationHost(node.id, 0, TEMP2) : TEMP2;
					bool emitted = false;
					switch (kind)
					{
						case BinaryKind::Add:
							emitted = m_code.EmitAddReg(output, left, right,
								(mask & 0x2) != 0);
							break;
						case BinaryKind::Subtract:
							emitted = m_code.EmitSubReg(output, left, right,
								(mask & 0x2) != 0);
							break;
						case BinaryKind::And:
							emitted = m_code.EmitAndReg(output, left, right);
							break;
						case BinaryKind::Or:
							emitted = m_code.EmitOrrReg(output, left, right);
							break;
						case BinaryKind::Xor:
							emitted = m_code.EmitEorReg(output, left, right);
							break;
						case BinaryKind::Nor:
							emitted = m_code.EmitOrrReg(output, left, right) &&
								m_code.EmitMvnReg(output, output);
							break;
					}
					if (!emitted || ((mask & 0x1) != 0 &&
						!CommitValueWord(node.id, 0, output)))
					{
						return false;
					}
				}

				if ((mask & 0x2) != 0)
				{
					unsigned left = TEMP0;
					unsigned right = TEMP1;
					if (high_staged)
					{
						if (!EmitSpillLoad(TEMP0, m_work_scratch_offset) ||
							!EmitSpillLoad(TEMP1, m_work_scratch_offset + 4))
							return false;
					}
					else if (!AcquireValueWord(node.operands[0], 1, TEMP0, &left) ||
						!AcquireValueWord(node.operands[1], 1, TEMP1, &right))
					{
						return false;
					}
					const unsigned output = DestinationHost(node.id, 1, TEMP2);
					bool emitted = false;
					switch (kind)
					{
						case BinaryKind::Add:
							emitted = m_code.EmitAdcReg(output, left, right);
							break;
						case BinaryKind::Subtract:
							emitted = m_code.EmitSbcReg(output, left, right);
							break;
						case BinaryKind::And:
							emitted = m_code.EmitAndReg(output, left, right);
							break;
						case BinaryKind::Or:
							emitted = m_code.EmitOrrReg(output, left, right);
							break;
						case BinaryKind::Xor:
							emitted = m_code.EmitEorReg(output, left, right);
							break;
						case BinaryKind::Nor:
							emitted = m_code.EmitOrrReg(output, left, right) &&
								m_code.EmitMvnReg(output, output);
							break;
					}
					if (!emitted || !CommitValueWord(node.id, 1, output))
						return false;
				}
				return true;
			}

			bool EmitShift64Immediate(const Node& node, ShiftType shift)
			{
				if (node.operand_count != 1 || node.type != ValueType::I64 ||
					node.immediate >= 64)
				{
					return false;
				}
				if (node.immediate == 0)
				{
					return EmitCopyMappings(node.id,
						{{0, node.operands[0], 0}, {1, node.operands[0], 1}});
				}
				if (!EmitLoadValueWord(node.operands[0], 0, TEMP0) ||
					!EmitSpillStore(TEMP0, m_work_scratch_offset) ||
					!EmitLoadValueWord(node.operands[0], 1, TEMP0) ||
					!EmitSpillStore(TEMP0, m_work_scratch_offset + 4))
				{
					return false;
				}
				const u8 mask = LocationMask(Location(node.id));
				const u32 amount = node.immediate;

				auto commit_zero = [&](u8 word) {
					const unsigned output = DestinationHost(node.id, word, TEMP2);
					return m_code.EmitMovImm8(output, 0) &&
					       CommitValueWord(node.id, word, output);
				};
				if (amount < 32)
				{
					if ((mask & 0x1) != 0)
					{
						const unsigned output = DestinationHost(node.id, 0, TEMP2);
						if (!EmitSpillLoad(TEMP0, m_work_scratch_offset) ||
							!EmitSpillLoad(TEMP1, m_work_scratch_offset + 4))
						{
							return false;
						}
						const bool emitted = shift == ShiftType::LSL ?
							m_code.EmitMovRegShiftImm(output, TEMP0,
								ShiftType::LSL, static_cast<u8>(amount)) :
							(m_code.EmitMovRegShiftImm(output, TEMP0,
								ShiftType::LSR, static_cast<u8>(amount)) &&
							 m_code.EmitOrrRegShiftImm(output, output, TEMP1,
								ShiftType::LSL, static_cast<u8>(32 - amount)));
						if (!emitted || !CommitValueWord(node.id, 0, output))
							return false;
					}
					if ((mask & 0x2) != 0)
					{
						const unsigned output = DestinationHost(node.id, 1, TEMP2);
						if (!EmitSpillLoad(TEMP0, m_work_scratch_offset) ||
							!EmitSpillLoad(TEMP1, m_work_scratch_offset + 4))
						{
							return false;
						}
						bool emitted = false;
						if (shift == ShiftType::LSL)
						{
							emitted = m_code.EmitMovRegShiftImm(output, TEMP1,
								ShiftType::LSL, static_cast<u8>(amount)) &&
								m_code.EmitOrrRegShiftImm(output, output, TEMP0,
									ShiftType::LSR, static_cast<u8>(32 - amount));
						}
						else
						{
							emitted = m_code.EmitMovRegShiftImm(output, TEMP1,
								shift, static_cast<u8>(amount));
						}
						if (!emitted || !CommitValueWord(node.id, 1, output))
							return false;
					}
					return true;
				}

				const u8 high_amount = static_cast<u8>(amount - 32);
				if ((mask & 0x1) != 0)
				{
					if (shift == ShiftType::LSL)
					{
						if (!commit_zero(0))
							return false;
					}
					else
					{
						const unsigned output = DestinationHost(node.id, 0, TEMP2);
						if (!EmitSpillLoad(TEMP0, m_work_scratch_offset + 4) ||
							!(high_amount == 0 ? EmitMove(output, TEMP0) :
								m_code.EmitMovRegShiftImm(output, TEMP0, shift,
									high_amount)) ||
							!CommitValueWord(node.id, 0, output))
						{
							return false;
						}
					}
				}
				if ((mask & 0x2) != 0)
				{
					if (shift == ShiftType::LSL)
					{
						const unsigned output = DestinationHost(node.id, 1, TEMP2);
						if (!EmitSpillLoad(TEMP0, m_work_scratch_offset) ||
							!m_code.EmitMovRegShiftImm(output, TEMP0,
								ShiftType::LSL, high_amount) ||
							!CommitValueWord(node.id, 1, output))
						{
							return false;
						}
					}
					else if (shift == ShiftType::ASR)
					{
						const unsigned output = DestinationHost(node.id, 1, TEMP2);
						if (!EmitSpillLoad(TEMP0, m_work_scratch_offset + 4) ||
							!m_code.EmitMovRegShiftImm(output, TEMP0,
								ShiftType::ASR, 31) ||
							!CommitValueWord(node.id, 1, output))
						{
							return false;
						}
					}
					else if (!commit_zero(1))
						return false;
				}
				return true;
			}

			bool EmitShift64Variable(const Node& node, ShiftType shift)
			{
				if (node.operand_count != 2 || node.type != ValueType::I64 ||
					!EmitLoadValueWord(node.operands[0], 0, TEMP0) ||
					!EmitSpillStore(TEMP0, m_work_scratch_offset) ||
					!EmitLoadValueWord(node.operands[0], 1, TEMP0) ||
					!EmitSpillStore(TEMP0, m_work_scratch_offset + 4) ||
					!EmitLoadValueWord(node.operands[1], 0, TEMP0) ||
					!m_code.EmitAndImm32(TEMP0, TEMP0, 63) ||
					!EmitSpillStore(TEMP0, m_work_scratch_offset + 8) ||
					!m_code.EmitTstImm32(TEMP0, 32))
				{
					return false;
				}
				const u8 mask = LocationMask(Location(node.id));
				const size_t high_half =
					m_code.EmitBranchPlaceholder(Condition::NE);
				if (high_half == static_cast<size_t>(-1))
					return false;

				// Amount 0..31. Register-shift amount 32 deliberately produces a
				// zero cross-word contribution when the guest amount is zero.
				if ((mask & 0x1) != 0)
				{
					bool emitted = false;
					if (shift == ShiftType::LSL)
					{
						const unsigned output =
							DestinationHost(node.id, 0, TEMP0);
						emitted = EmitSpillLoad(TEMP0,
							m_work_scratch_offset) &&
							EmitSpillLoad(TEMP2,
								m_work_scratch_offset + 8) &&
							m_code.EmitMovRegShiftReg(output, TEMP0,
								ShiftType::LSL, TEMP2) &&
							CommitValueWord(node.id, 0, output);
					}
					else
					{
						const unsigned output =
							DestinationHost(node.id, 0, TEMP0);
						emitted = EmitSpillLoad(TEMP2,
							m_work_scratch_offset + 8) &&
							EmitSpillLoad(TEMP0, m_work_scratch_offset) &&
							m_code.EmitMovRegShiftReg(TEMP0, TEMP0,
								ShiftType::LSR, TEMP2) &&
							EmitSpillStore(TEMP0,
								m_work_scratch_offset + 12) &&
							EmitSpillLoad(TEMP2,
								m_work_scratch_offset + 8) &&
							m_code.EmitRsbImm32(TEMP2, TEMP2, 32) &&
							EmitSpillLoad(TEMP0,
								m_work_scratch_offset + 12) &&
							EmitSpillLoad(TEMP1,
								m_work_scratch_offset + 4) &&
							m_code.EmitOrrRegShiftReg(output, TEMP0, TEMP1,
								ShiftType::LSL, TEMP2) &&
							CommitValueWord(node.id, 0, output);
					}
					if (!emitted)
						return false;
				}
				if ((mask & 0x2) != 0)
				{
					bool emitted = false;
					if (shift == ShiftType::LSL)
					{
						const unsigned output =
							DestinationHost(node.id, 1, TEMP1);
						emitted = EmitSpillLoad(TEMP2,
							m_work_scratch_offset + 8) &&
							EmitSpillLoad(TEMP1,
								m_work_scratch_offset + 4) &&
							m_code.EmitMovRegShiftReg(TEMP1, TEMP1,
								ShiftType::LSL, TEMP2) &&
							EmitSpillStore(TEMP1,
								m_work_scratch_offset + 12) &&
							EmitSpillLoad(TEMP2,
								m_work_scratch_offset + 8) &&
							m_code.EmitRsbImm32(TEMP2, TEMP2, 32) &&
							EmitSpillLoad(TEMP0,
								m_work_scratch_offset) &&
							EmitSpillLoad(TEMP1,
								m_work_scratch_offset + 12) &&
							m_code.EmitOrrRegShiftReg(output, TEMP1, TEMP0,
								ShiftType::LSR, TEMP2) &&
							CommitValueWord(node.id, 1, output);
					}
					else
					{
						const unsigned output =
							DestinationHost(node.id, 1, TEMP1);
						emitted = EmitSpillLoad(TEMP1,
							m_work_scratch_offset + 4) &&
							EmitSpillLoad(TEMP2,
								m_work_scratch_offset + 8) &&
							m_code.EmitMovRegShiftReg(output, TEMP1,
								shift, TEMP2) &&
							CommitValueWord(node.id, 1, output);
					}
					if (!emitted)
						return false;
				}
				const size_t done = m_code.EmitBranchPlaceholder();
				const size_t high_target = m_code.Size();
				if (done == static_cast<size_t>(-1) ||
					!m_code.PatchBranch(high_half, high_target, Condition::NE))
				{
					return false;
				}

				// Amount 32..63. The low five bits are the word-local shift.
				if (!EmitSpillLoad(TEMP2, m_work_scratch_offset + 8) ||
					!m_code.EmitAndImm32(TEMP2, TEMP2, 31))
				{
					return false;
				}
				if ((mask & 0x1) != 0)
				{
					const unsigned output = DestinationHost(node.id, 0, TEMP1);
					if (shift == ShiftType::LSL)
					{
						if (!m_code.EmitMovImm8(output, 0))
							return false;
					}
					else if (!EmitSpillLoad(TEMP0,
							m_work_scratch_offset + 4) ||
						!m_code.EmitMovRegShiftReg(output, TEMP0, shift, TEMP2))
					{
						return false;
					}
					if (!CommitValueWord(node.id, 0, output))
						return false;
				}
				if ((mask & 0x2) != 0)
				{
					const unsigned output = DestinationHost(node.id, 1, TEMP1);
					if (shift == ShiftType::LSL)
					{
						if (!EmitSpillLoad(TEMP0, m_work_scratch_offset) ||
							!m_code.EmitMovRegShiftReg(output, TEMP0,
								ShiftType::LSL, TEMP2))
						{
							return false;
						}
					}
					else if (shift == ShiftType::ASR)
					{
						if (!EmitSpillLoad(TEMP0,
								m_work_scratch_offset + 4) ||
							!m_code.EmitMovRegShiftImm(output, TEMP0,
								ShiftType::ASR, 31))
						{
							return false;
						}
					}
					else if (!m_code.EmitMovImm8(output, 0))
						return false;
					if (!CommitValueWord(node.id, 1, output))
						return false;
				}
				return m_code.PatchBranch(done, m_code.Size());
			}

			bool EmitSelect64(const Node& node)
			{
				if (node.operand_count != 3 || node.type != ValueType::I64 ||
					!EmitLoadValueWord(node.operands[1], 0, TEMP0) ||
					!EmitSpillStore(TEMP0, m_work_scratch_offset) ||
					!EmitLoadValueWord(node.operands[2], 0, TEMP0) ||
					!EmitSpillStore(TEMP0, m_work_scratch_offset + 4) ||
					!EmitLoadValueWord(node.operands[1], 1, TEMP0) ||
					!EmitSpillStore(TEMP0, m_work_scratch_offset + 8) ||
					!EmitLoadValueWord(node.operands[2], 1, TEMP0) ||
					!EmitSpillStore(TEMP0, m_work_scratch_offset + 12))
				{
					return false;
				}
				unsigned condition = 0;
				if (!AcquireValueWord(node.operands[0], 0, TEMP0, &condition) ||
					!m_code.EmitCmpImm32(condition, 0))
				{
					return false;
				}
				const u8 mask = LocationMask(Location(node.id));
				for (u8 word = 0; word < 2; word++)
				{
					if ((mask & (1u << word)) == 0)
						continue;
					const unsigned output = DestinationHost(node.id, word, TEMP2);
					if (!EmitSpillLoad(output,
							m_work_scratch_offset + word * 8 + 4) ||
						!EmitSpillLoad(TEMP0,
							m_work_scratch_offset + word * 8) ||
						!EmitMove(output, TEMP0, Condition::NE) ||
						!CommitValueWord(node.id, word, output))
					{
						return false;
					}
				}
				return true;
			}

			bool EmitBoolean(Condition condition, ValueId target)
			{
				const unsigned output = DestinationHost(target, 0, TEMP2);
				return m_code.EmitMovImm8(output, 0) &&
				       m_code.EmitMovImm8(output, 1, condition) &&
				       CommitValueWord(target, 0, output);
			}

			bool EmitCompareEqual64Flags(const Node& node)
			{
				const bool left_zero = IsArchitecturalZeroValue(node.operands[0]);
				const bool right_zero = IsArchitecturalZeroValue(node.operands[1]);
				const RegionAllocation::RematerializationKind left_extension =
					Low32Extension(node.operands[0]);
				const RegionAllocation::RematerializationKind right_extension =
					Low32Extension(node.operands[1]);
				const bool narrow =
					(left_zero && right_extension !=
						RegionAllocation::RematerializationKind::None) ||
					(right_zero && left_extension !=
						RegionAllocation::RematerializationKind::None) ||
					(left_extension != RegionAllocation::RematerializationKind::None &&
					 left_extension == right_extension);
				if (narrow)
				{
					unsigned left = 0;
					unsigned right = 0;
					if (!AcquireValueWord(node.operands[0], 0, TEMP0, &left) ||
						!AcquireValueWord(node.operands[1], 0, TEMP1, &right) ||
						!m_code.EmitCmpReg(left, right))
					{
						return false;
					}
					m_result.narrowed_equal64_comparisons++;
					return true;
				}
				if (left_zero || right_zero)
				{
					const ValueId value = left_zero ? node.operands[1] : node.operands[0];
					unsigned low = 0;
					unsigned high = 0;
					return AcquireValueWord(value, 0, TEMP0, &low) &&
					       AcquireValueWord(value, 1, TEMP1, &high) &&
					       m_code.EmitOrrReg(TEMP2, low, high, true);
				}
				unsigned left = 0;
				unsigned right = 0;
				if (!AcquireValueWord(node.operands[0], 0, TEMP0, &left) ||
					!AcquireValueWord(node.operands[1], 0, TEMP1, &right) ||
					!m_code.EmitCmpReg(left, right) ||
					!AcquireValueWord(node.operands[0], 1, TEMP0, &left) ||
					!AcquireValueWord(node.operands[1], 1, TEMP1, &right) ||
					!m_code.EmitCmpReg(left, right, Condition::EQ))
				{
					return false;
				}
				return true;
			}

			bool EmitCompareEqual64(const Node& node, bool equal)
			{
				return EmitCompareEqual64Flags(node) &&
				       EmitBoolean(equal ? Condition::EQ : Condition::NE, node.id);
			}

			bool EmitCompareOrdered64Flags(const Node& node, bool signed_compare,
				Condition* result_condition)
			{
				if (!result_condition)
					return false;
				const RegionAllocation::RematerializationKind left_extension =
					Low32Extension(node.operands[0]);
				const RegionAllocation::RematerializationKind right_extension =
					Low32Extension(node.operands[1]);
				if (left_extension != RegionAllocation::RematerializationKind::None &&
					left_extension == right_extension)
				{
					unsigned left = 0;
					unsigned right = 0;
					if (!AcquireValueWord(node.operands[0], 0, TEMP0, &left) ||
						!AcquireValueWord(node.operands[1], 0, TEMP1, &right) ||
						!m_code.EmitCmpReg(left, right))
					{
						return false;
					}
					// Signed comparison of two sign extensions is signed-i32;
					// signed comparison of two zero extensions and every unsigned
					// same-extension comparison are unsigned-i32.
					*result_condition = signed_compare &&
						left_extension == RegionAllocation::RematerializationKind::SignExtendLow32 ?
						Condition::LT : Condition::CC;
					m_result.narrowed_ordered64_comparisons++;
					return true;
				}
				if (!signed_compare &&
					left_extension == RegionAllocation::RematerializationKind::SignExtendLow32 &&
					right_extension == RegionAllocation::RematerializationKind::ZeroExtendLow32)
				{
					unsigned left = 0;
					unsigned right = 0;
					if (!AcquireValueWord(node.operands[0], 0, TEMP0, &left) ||
						!AcquireValueWord(node.operands[1], 0, TEMP1, &right) ||
						!m_code.EmitCmpImm32(left, 0) ||
						!m_code.EmitCmpReg(left, right, Condition::PL))
					{
						return false;
					}
					// CMP left,#0 leaves C set when left is negative, making CC
					// false.  Otherwise the conditional low-word CMP supplies the
					// exact unsigned result.
					*result_condition = Condition::CC;
					m_result.narrowed_ordered64_comparisons++;
					return true;
				}
				unsigned left = 0;
				unsigned right = 0;
				if (!AcquireValueWord(node.operands[0], 0, TEMP0, &left) ||
					!AcquireValueWord(node.operands[1], 0, TEMP1, &right) ||
					!m_code.EmitSubReg(TEMP2, left, right, true) ||
					!AcquireValueWord(node.operands[0], 1, TEMP0, &left) ||
					!AcquireValueWord(node.operands[1], 1, TEMP1, &right) ||
					!m_code.EmitSbcReg(TEMP2, left, right, true))
				{
					return false;
				}
				*result_condition = signed_compare ? Condition::LT : Condition::CC;
				return true;
			}

			bool EmitCompareOrdered64(const Node& node, bool signed_compare)
			{
				Condition condition = Condition::AL;
				return EmitCompareOrdered64Flags(node, signed_compare, &condition) &&
				       EmitBoolean(condition, node.id);
			}

			bool EmitCompareZero64(const Node& node, Condition condition)
			{
				unsigned low = 0;
				unsigned high = 0;
				if (!AcquireValueWord(node.operands[0], 0, TEMP0, &low) ||
					!AcquireValueWord(node.operands[0], 1, TEMP1, &high))
					return false;
				if (condition == Condition::MI || condition == Condition::PL)
				{
					if (!m_code.EmitCmpImm32(high, 0))
						return false;
					return EmitBoolean(condition, node.id);
				}
				// GT and LE require sign plus a complete zero test. Form low|high;
				// preserve the sign in TEMP2 before the logical operation.
				if (!EmitMove(TEMP2, high) ||
					!m_code.EmitOrrReg(TEMP0, low, high, true))
					return false;
				if (condition == Condition::GT)
				{
					// Positive means nonzero and a clear high sign bit.
					if (!m_code.EmitMovImm8(TEMP1, 0) ||
						!m_code.EmitMovImm8(TEMP1, 1, Condition::NE) ||
						!m_code.EmitTstImm32(TEMP2, 0x80000000u) ||
						!m_code.EmitMovImm8(TEMP1, 0, Condition::NE))
						return false;
				}
				else
				{
					// Re-evaluate directly with two comparisons to keep the condition
					// independent of the temporary logical flags.
					if (!m_code.EmitMovImm8(TEMP1, 0) ||
						!m_code.EmitCmpImm32(TEMP2, 0) ||
						!m_code.EmitMovImm8(TEMP1, 1, Condition::MI) ||
						!m_code.EmitOrrReg(TEMP0, low, high, true) ||
						!m_code.EmitMovImm8(TEMP1, 1, Condition::EQ))
						return false;
				}
				return CommitValueWord(node.id, 0, TEMP1);
			}

			bool IsArchitecturalZeroValue(ValueId value) const
			{
				u32 low = 0;
				u32 high = 0;
				if (Location(value).kind == RegionAllocation::LocationKind::Immediate &&
					ImmediateWord(value, 0, &low) && ImmediateWord(value, 1, &high))
				{
					return low == 0 && high == 0;
				}
				const Node* const definition = Definition(value);
				if (!definition)
					return false;
				if ((definition->opcode == Opcode::ConstantI1 ||
					 definition->opcode == Opcode::ConstantI32 ||
					 definition->opcode == Opcode::ConstantI64 ||
					 definition->opcode == Opcode::ConstantAddress) &&
					definition->literal == 0)
				{
					return true;
				}
				if ((definition->opcode == Opcode::ExtractLow32 ||
					 definition->opcode == Opcode::ExtractLow64) &&
					definition->operand_count == 1)
				{
					return IsArchitecturalZeroValue(definition->operands[0]);
				}
				if (definition->opcode != Opcode::Parameter)
					return false;
				return std::any_of(m_program.blocks.begin(), m_program.blocks.end(),
					[&](const Block& block) {
						return block.parameters.gpr[0] == value;
					});
			}

			bool IsDirectBranchCondition(u32 block_index, const Node& node) const
			{
				if (block_index >= m_program.blocks.size() ||
					m_program.blocks[block_index].terminator.kind !=
						TerminatorKind::Branch ||
					m_program.blocks[block_index].terminator.condition != node.id ||
					node.id >= m_allocation.folded_branch_conditions.size() ||
					m_allocation.folded_branch_conditions[node.id] == 0)
				{
					return false;
				}
				switch (node.opcode)
				{
					case Opcode::CompareEqual64:
					case Opcode::CompareNotEqual64:
					case Opcode::CompareSignedLess64:
					case Opcode::CompareUnsignedLess64:
					case Opcode::CompareSignedLessEqualZero64:
					case Opcode::CompareSignedGreaterZero64:
					case Opcode::CompareSignedLessZero64:
					case Opcode::CompareSignedGreaterEqualZero64:
					case Opcode::Cop1BranchCondition:
						return true;
					default:
						return false;
				}
			}

			bool ReuseMaterializedBooleanFlags(u32 block_index,
				const Node& branch_condition, Condition* condition) const
			{
				if (!condition || block_index >= m_program.blocks.size() ||
					(branch_condition.opcode != Opcode::CompareEqual64 &&
					 branch_condition.opcode != Opcode::CompareNotEqual64) ||
					branch_condition.operand_count != 2)
				{
					return false;
				}

				ValueId wrapped = INVALID_VALUE;
				if (IsArchitecturalZeroValue(branch_condition.operands[0]))
					wrapped = branch_condition.operands[1];
				else if (IsArchitecturalZeroValue(branch_condition.operands[1]))
					wrapped = branch_condition.operands[0];
				if (wrapped == INVALID_VALUE)
					return false;
				// GPR writes retain the architectural upper 64 bits with ReplaceLow64;
				// a following branch reads that same low half through ExtractLow64. Peel
				// this exact typed identity before looking for the 0/1 extension. The
				// verifier already proves both nodes' word selection and operand order.
				ValueId state_extract = INVALID_VALUE;
				ValueId state_replace = INVALID_VALUE;
				if (const Node* const extract = Definition(wrapped);
					extract && extract->opcode == Opcode::ExtractLow64 &&
					extract->operand_count == 1)
				{
					const Node* const replacement = Definition(extract->operands[0]);
					if (replacement && replacement->opcode == Opcode::ReplaceLow64 &&
						replacement->operand_count == 2)
					{
						state_extract = extract->id;
						state_replace = replacement->id;
						wrapped = replacement->operands[1];
					}
				}

				const Node* const extension = Definition(wrapped);
				if (!extension || extension->operand_count != 1 ||
					(extension->opcode != Opcode::ZeroExtend32To64 &&
					 extension->opcode != Opcode::SignExtend32To64))
				{
					return false;
				}
				const Node* const predicate = Definition(extension->operands[0]);
				if (!predicate || predicate->type != ValueType::I1)
					return false;

				Condition predicate_true = Condition::AL;
				switch (predicate->opcode)
				{
					case Opcode::CompareEqual64:
						predicate_true = Condition::EQ;
						break;
					case Opcode::CompareNotEqual64:
						predicate_true = Condition::NE;
						break;
					case Opcode::CompareSignedLess64:
						predicate_true =
							Low32Extension(predicate->operands[0]) ==
								RegionAllocation::RematerializationKind::ZeroExtendLow32 &&
							Low32Extension(predicate->operands[1]) ==
								RegionAllocation::RematerializationKind::ZeroExtendLow32 ?
							Condition::CC : Condition::LT;
						break;
					case Opcode::CompareUnsignedLess64:
						predicate_true = Condition::CC;
						break;
					case Opcode::CompareSignedLessZero64:
						predicate_true = Condition::MI;
						break;
					case Opcode::CompareSignedGreaterEqualZero64:
						predicate_true = Condition::PL;
						break;
					default:
						return false;
				}

				auto invert = [](Condition input) {
					switch (input)
					{
						case Condition::EQ: return Condition::NE;
						case Condition::NE: return Condition::EQ;
						case Condition::CS: return Condition::CC;
						case Condition::CC: return Condition::CS;
						case Condition::MI: return Condition::PL;
						case Condition::PL: return Condition::MI;
						case Condition::GE: return Condition::LT;
						case Condition::LT: return Condition::GE;
						default: return Condition::AL;
					}
				};
				const bool branch_on_true =
					branch_condition.opcode == Opcode::CompareNotEqual64;
				*condition = branch_on_true ? predicate_true : invert(predicate_true);
				if (*condition == Condition::AL)
					return false;

				// The predicate lowering ends with non-S MOV/STR/VMOV operations. Reuse
				// its flags only when every later node before the terminator is likewise
				// flag-transparent or belongs to a likely delay slot which is emitted on
				// the taken edge after the branch.
				const Block& block = m_program.blocks[block_index];
				bool after_predicate = false;
				for (const Node& node : block.nodes)
				{
					if (node.id == predicate->id)
					{
						after_predicate = true;
						continue;
					}
					if (!after_predicate)
						continue;
					const bool intrinsically_emitted =
						node.opcode == Opcode::MemoryLoad ||
						node.opcode == Opcode::MemoryStore ||
						node.opcode == Opcode::ExitIfTrue ||
						node.opcode == Opcode::Cop1ExceptionalOuResult ||
						node.opcode == Opcode::Vu0RequireIdle;
					const bool demand_elides_node =
						node.id < m_allocation.value_word_demands.size() &&
						m_allocation.value_word_demands[node.id] == 0 &&
						!intrinsically_emitted;
					if (node.id == extension->id || node.id == state_replace ||
						node.id == state_extract || node.id == branch_condition.id ||
						node.opcode == Opcode::Parameter ||
						node.opcode == Opcode::ConstantI1 ||
						node.opcode == Opcode::ConstantI32 ||
						node.opcode == Opcode::ConstantI64 ||
						node.opcode == Opcode::ConstantAddress ||
						node.opcode == Opcode::NoEffect ||
						node.opcode == Opcode::AdvanceCycles ||
						node.opcode == Opcode::Add32 ||
						node.opcode == Opcode::Sub32 || demand_elides_node ||
						IsStateBinding(node.opcode) ||
						IsArchitecturalZeroValue(node.id) ||
						(block.terminator.likely &&
						 node.source_pc == block.terminator.delay_slot_pc))
					{
						continue;
					}
					return false;
				}
				return after_predicate;
			}

			bool EmitDirectBranchCondition(const Node& node, Condition* condition)
			{
				if (!condition)
					return false;
				switch (node.opcode)
				{
					case Opcode::CompareEqual64:
						*condition = Condition::EQ;
						return EmitCompareEqual64Flags(node);
					case Opcode::CompareNotEqual64:
						*condition = Condition::NE;
						return EmitCompareEqual64Flags(node);
					case Opcode::CompareSignedLess64:
						return EmitCompareOrdered64Flags(node, true, condition);
					case Opcode::CompareUnsignedLess64:
						return EmitCompareOrdered64Flags(node, false, condition);
					case Opcode::CompareSignedLessEqualZero64:
					case Opcode::CompareSignedGreaterZero64:
					{
						const RegionAllocation::RematerializationKind rematerialization =
							Low32Extension(node.operands[0]);
						if (rematerialization !=
								RegionAllocation::RematerializationKind::None)
						{
							unsigned low = 0;
							if (!AcquireValueWord(node.operands[0], 0, TEMP0, &low) ||
								!m_code.EmitCmpImm32(low, 0))
							{
								return false;
							}
							const bool greater =
								node.opcode == Opcode::CompareSignedGreaterZero64;
							if (rematerialization ==
									RegionAllocation::RematerializationKind::SignExtendLow32)
							{
								*condition = greater ? Condition::GT : Condition::LE;
							}
							else
							{
								*condition = greater ? Condition::NE : Condition::EQ;
							}
							return true;
						}
						// Form the exact signed-64 predicate without materializing an SSA
						// boolean in the region frame. A value is greater than zero iff it
						// is nonzero and its high word is nonnegative; LEZ is its inverse.
						// TEMP2 receives only the high sign bit, while TEMP1 becomes the
						// final 0/1 truth value consumed by the branch flags.
						if (!EmitLoadValueWord(node.operands[0], 0, TEMP0) ||
							!EmitLoadValueWord(node.operands[0], 1, TEMP1) ||
							!m_code.EmitMovRegShiftImm(TEMP2, TEMP1,
								ShiftType::LSR, 31) ||
							!m_code.EmitOrrReg(TEMP0, TEMP1, TEMP0, true) ||
							!m_code.EmitMovImm8(TEMP1, 0) ||
							!m_code.EmitMovImm8(TEMP1, 1, Condition::NE) ||
							!m_code.EmitBicRegShiftImm(TEMP1, TEMP1, TEMP2,
								ShiftType::LSL, 0) ||
							!m_code.EmitCmpImm32(TEMP1, 0))
						{
							return false;
						}
						*condition = node.opcode == Opcode::CompareSignedGreaterZero64 ?
							Condition::NE : Condition::EQ;
						return true;
					}
					case Opcode::Cop1BranchCondition:
					{
						unsigned flags = 0;
						if (node.operand_count != 1 || node.immediate > 1 ||
							!AcquireValueWord(node.operands[0], 0, TEMP0, &flags) ||
							!m_code.EmitTstImm32(flags, FCR31_C))
						{
							return false;
						}
						*condition = node.immediate != 0 ? Condition::NE : Condition::EQ;
						return true;
					}
					case Opcode::CompareSignedLessZero64:
					case Opcode::CompareSignedGreaterEqualZero64:
					{
						unsigned high = 0;
						if (!AcquireValueWord(node.operands[0], 1, TEMP0, &high) ||
							!m_code.EmitCmpImm32(high, 0))
						{
							return false;
						}
						*condition = node.opcode == Opcode::CompareSignedLessZero64 ?
							Condition::MI : Condition::PL;
						return true;
					}
					default:
						return false;
				}
			}

			bool EmitShift32(const Node& node, ShiftType shift, bool variable)
			{
				unsigned value = 0;
				if (!AcquireValueWord(node.operands[0], 0, TEMP0, &value))
					return false;
				const unsigned output = DestinationHost(node.id, 0, TEMP2);
				if (variable)
				{
					unsigned amount = 0;
					if (!AcquireValueWord(node.operands[1], 0, TEMP1, &amount) ||
						!m_code.EmitMovRegShiftReg(output, value, shift, amount))
						return false;
				}
				else if (!m_code.EmitMovRegShiftImm(output, value, shift,
						static_cast<u8>(node.immediate)))
				{
					return false;
				}
				return CommitValueWord(node.id, 0, output);
			}

			bool EmitSignedOverflow32(const Node& node)
			{
				if (node.id < m_allocation.flagged_add_for_signed_overflow.size() &&
					m_allocation.flagged_add_for_signed_overflow[node.id] != INVALID_VALUE)
				{
					return true;
				}
				unsigned left = 0;
				unsigned right = 0;
				if (!AcquireValueWord(node.operands[0], 0, TEMP0, &left) ||
					!AcquireValueWord(node.operands[1], 0, TEMP1, &right) ||
					!m_code.EmitEorReg(TEMP2, left, right) ||
					!m_code.EmitMvnReg(TEMP2, TEMP2) ||
					!m_code.EmitAddReg(TEMP1, left, right) ||
					!m_code.EmitEorReg(TEMP0, left, TEMP1) ||
					!m_code.EmitAndReg(TEMP2, TEMP2, TEMP0) ||
					!m_code.EmitMovRegShiftImm(TEMP2, TEMP2, ShiftType::LSR, 31))
				{
					return false;
				}
				return CommitValueWord(node.id, 0, TEMP2);
			}

			const RegionExecution::ExitSite* FindMemoryExitSite(
				u32 block_index, ValueId operation) const
			{
				if (block_index >= m_program.blocks.size())
					return nullptr;
				const Block& block = m_program.blocks[block_index];
				for (u32 ordinal = 0; ordinal < block.memory_exits.size(); ordinal++)
				{
					if (block.memory_exits[ordinal].operation == operation)
					{
						return FindExitSite(RegionExecution::ExitSiteKind::Memory,
							block_index, ordinal);
					}
				}
				return nullptr;
			}

			bool EmitMemoryColdBranch(Condition condition, ExitReason reason,
				const RegionExecution::ExitSite* site, ValueId address,
				bool mask_quad_address)
			{
				return AppendColdBranch(condition, reason, site, address,
					mask_quad_address);
			}

			// On success TEMP1 is one host pointer wholly contained in EE main RAM.
			// This is the generated equivalent of PCSX2
			// vtlb_private::VTLBVirtual::{isHandler,assumePtr}; non-RAM direct
			// mappings deliberately exit to tier zero because they may be observable.
			bool EmitResolveMemoryHostAddress(const Node& node,
				MemoryAccessKind kind, const RegionExecution::ExitSite* site)
			{
				const u32 width = MemoryAccessWidth(kind);
				const u32 alignment_mask = MemoryAlignmentMask(kind);
				const bool quad = IsQuadMemoryAccess(kind);
				unsigned address = 0;
				if (width == 0 || node.operand_count != 3)
				{
					m_result.failure_emission_step = 20;
					return false;
				}
				if (!AcquireValueWord(node.operands[1], 0, TEMP0, &address))
				{
					m_result.failure_emission_step = 21;
					return m_result.failure_operand_value == INVALID_VALUE ?
						RecordOperandFailure(node.operands[1]) : false;
				}
				if (alignment_mask != 0 &&
					(!m_code.EmitTstImm32(address, alignment_mask) ||
					 !EmitMemoryColdBranch(Condition::NE,
						 ExitReason::MemoryAlignment, site, node.operands[1], false)))
				{
					return false;
				}
				if (quad)
				{
					if (!EmitMove(TEMP0, address) ||
						!m_code.EmitBicImm32(TEMP0, TEMP0, 0x0f))
					{
						return false;
					}
					address = TEMP0;
				}

				// The persistent dispatcher already proves and owns the default EE
				// low-main-RAM identity window. Keep its ordinary load/store path compact:
				// a non-identity address exits before observation and tier zero executes
				// the exact VTLB/handler case. The full reference translation below remains
				// available to the callable oracle backend.
				if (Persistent())
				{
					const u32 identity_limit =
						m_options.persistent_dispatch->identity_main_ram_limit;
					if (!(m_code.EmitCmpImm32(address, identity_limit) ||
						(m_code.EmitMovImm32(TEMP2, identity_limit) &&
						 m_code.EmitCmpReg(address, TEMP2))) ||
						!EmitMemoryColdBranch(Condition::CS,
							ExitReason::MemoryTranslation, site,
							node.operands[1], quad))
					{
						return false;
					}
					return m_code.EmitAddReg(TEMP1, VTLB_HOST_BASE, address);
				}

				// PCSX2 owner: BlockCompiler::EmitVtlbNonHandlerHostAddress(). Every
				// admitted width is naturally aligned, while EE LQ/SQ first mask their
				// address to 16 bytes. Therefore address + width - 1 cannot wrap, and an
				// address below the page-aligned main-RAM limit contains the complete
				// access. Match tier zero's low identity-window proof instead of adding
				// a redundant end-address calculation to every pointer-chasing load.
				const bool identity_compare =
					EmitContextLoad(TEMP2,
						offsetof(ExecutionContext, identity_main_ram_limit)) &&
					m_code.EmitCmpReg(address, TEMP2);
				if (!identity_compare)
				{
					return false;
				}
				const size_t identity =
					m_code.EmitBranchPlaceholder(Condition::CC);
				if (identity == static_cast<size_t>(-1) ||
					!EmitContextLoad(TEMP1, offsetof(ExecutionContext, vmap)) ||
					!m_code.EmitMovRegShiftImm(TEMP2, address, ShiftType::LSR,
						SOURCE_PAGE_SHIFT) ||
					!m_code.EmitLdrRegShift(TEMP1, TEMP1, TEMP2, ShiftType::LSL, 2) ||
					!m_code.EmitAddReg(TEMP1, TEMP1, address, true) ||
					!EmitMemoryColdBranch(Condition::MI, ExitReason::MemoryHandler,
						site, node.operands[1], quad) ||
					!EmitContextLoad(TEMP2,
						offsetof(ExecutionContext, host_memory_base)) ||
					!m_code.EmitAddReg(TEMP1, TEMP1, TEMP2) ||
					!EmitContextLoad(TEMP2, offsetof(ExecutionContext, main_ram)) ||
					!m_code.EmitCmpReg(TEMP1, TEMP2) ||
					!EmitMemoryColdBranch(Condition::CC,
						ExitReason::MemoryTranslation, site, node.operands[1], quad) ||
					!EmitContextLoad(TEMP0,
						offsetof(ExecutionContext, main_ram_limit)) ||
					!m_code.EmitAddReg(TEMP2, TEMP2, TEMP0) ||
					(width > 1 &&
						(!m_code.EmitAddImm32(TEMP0, TEMP1, width - 1, true) ||
						 !EmitMemoryColdBranch(Condition::CS,
							 ExitReason::MemoryTranslation, site, node.operands[1], quad))) ||
					(width == 1 && !EmitMove(TEMP0, TEMP1)) ||
					!m_code.EmitCmpReg(TEMP0, TEMP2) ||
					!EmitMemoryColdBranch(Condition::CS,
						ExitReason::MemoryTranslation, site, node.operands[1], quad))
				{
					return false;
				}

				const size_t translated = m_code.EmitBranchPlaceholder();
				const size_t identity_target = m_code.Size();
				if (translated == static_cast<size_t>(-1) ||
					!m_code.PatchBranch(identity, identity_target, Condition::CC))
				{
					return false;
				}
				// The persistent dispatcher already owns vtlbdata.host_memory_base in
				// r8, and vtlb_Core_Alloc() maps EE main RAM at arena offset zero.
				// Reusing it is the exact tier-zero identity ABI and avoids rebuilding
				// the same absolute pointer at every region memory operation. Callable
				// validation retains its explicit context-owned main-RAM pointer.
				if (!(EmitContextLoad(TEMP1,
						offsetof(ExecutionContext, main_ram)) &&
					 m_code.EmitAddReg(TEMP1, TEMP1, address)))
				{
					return false;
				}
				return m_code.PatchBranch(translated, m_code.Size());
			}

			// Every direct store checks the authoritative tier-zero live-source
			// tables before its first byte is committed. Natural alignment and the
			// 16-byte LQ/SQ mask prove all admitted widths remain in one 64-byte
			// source chunk, so one exact chunk bit covers the complete access.
			bool EmitStoreSourceOwnership(const Node& node,
				MemoryAccessKind kind, const RegionExecution::ExitSite* site)
			{
				if (Persistent())
				{
					// Page-live is a conservative pre-write certificate. A data-only page
					// commits directly; a page containing any generated source exits before
					// the store so tier zero can perform the exact 64-byte invalidation test.
					if (!m_code.EmitSubReg(TEMP0, TEMP1, VTLB_HOST_BASE) ||
						!EmitContextLoad(TEMP2,
							offsetof(ExecutionContext,
								ram_source_page_live_flags)) ||
						!m_code.EmitLdrbRegShift(TEMP2, TEMP2, TEMP0,
							ShiftType::LSR, SOURCE_PAGE_SHIFT) ||
						!m_code.EmitCmpImm32(TEMP2, 0))
					{
						return false;
					}
					return EmitMemoryColdBranch(Condition::NE,
						ExitReason::SelfModifyingCode, site, node.operands[1],
						IsQuadMemoryAccess(kind));
				}
				if (!EmitContextLoad(TEMP2,
						offsetof(ExecutionContext, main_ram)) ||
					!m_code.EmitSubReg(TEMP0, TEMP1, TEMP2) ||
					!EmitContextLoad(TEMP2,
						offsetof(ExecutionContext, ram_source_page_live_flags)) ||
					!m_code.EmitLdrbRegShift(TEMP2, TEMP2, TEMP0,
						ShiftType::LSR, SOURCE_PAGE_SHIFT) ||
					!m_code.EmitCmpImm32(TEMP2, 0))
				{
					return false;
				}
				const size_t data_page =
					m_code.EmitBranchPlaceholder(Condition::EQ);
				if (data_page == static_cast<size_t>(-1) ||
					!EmitContextLoad(TEMP2,
						offsetof(ExecutionContext, ram_source_chunk_live_bits)) ||
					!m_code.EmitLdrbRegShift(TEMP2, TEMP2, TEMP0,
						ShiftType::LSR, SOURCE_CHUNK_SHIFT + 3) ||
					!m_code.EmitMovRegShiftImm(TEMP0, TEMP0, ShiftType::LSR,
						SOURCE_CHUNK_SHIFT) ||
					!m_code.EmitAndImm8(TEMP0, TEMP0, 7) ||
					!m_code.EmitMovRegShiftReg(TEMP2, TEMP2, ShiftType::LSR, TEMP0) ||
					!m_code.EmitTstImm32(TEMP2, 1) ||
					!EmitMemoryColdBranch(Condition::NE,
						ExitReason::SelfModifyingCode, site, node.operands[1],
						IsQuadMemoryAccess(kind)) ||
					!m_code.PatchBranch(data_page, m_code.Size(), Condition::EQ))
				{
					return false;
				}
				return true;
			}

			struct DirectMemoryAddress
			{
				unsigned base = TEMP1;
				unsigned index = UINT_MAX;
				u16 offset = 0;

				bool Indexed() const { return index != UINT_MAX; }
			};

			bool EmitLoadedWord(ValueId target, u8 word,
				const DirectMemoryAddress& address, u16 offset)
			{
				return !address.Indexed() &&
				       m_code.EmitLdrImm12(TEMP0, address.base, offset) &&
				       EmitStoreValueWord(target, word, TEMP0);
			}

			bool EmitMemoryLoad(const Node& node, MemoryAccessKind kind,
				const DirectMemoryAddress& address)
			{
				m_result.memory_loads++;
				const ValueId target = node.id < m_memory_value_for_effect.size() ?
					m_memory_value_for_effect[node.id] : INVALID_VALUE;
				if (target == INVALID_VALUE)
					return true;
				const RegionAllocation::Location& target_location = Location(target);
				const u8 mask = LocationMask(target_location);
				if ((kind == MemoryAccessKind::Load128 ||
					 kind == MemoryAccessKind::LoadVu0Vector) && mask == 0x0f &&
					target_location.kind == RegionAllocation::LocationKind::NeonQ)
				{
					return !address.Indexed() && address.offset == 0 &&
						m_code.EmitVld1Q32Aligned(target_location.index, address.base);
				}
				const bool preserve_upper = kind != MemoryAccessKind::Load128 &&
					kind != MemoryAccessKind::LoadVu0Vector &&
					Definition(target) && Definition(target)->type == ValueType::I128;
				if (preserve_upper)
				{
					for (u8 word = 2; word < 4; word++)
					{
						if ((mask & (1u << word)) != 0 &&
							(!EmitLoadValueWord(node.operands[2], word, TEMP0) ||
							 !EmitSpillStore(TEMP0, m_work_scratch_offset + word * 4)))
						{
							return false;
						}
					}
				}

				const unsigned low_output =
					DestinationHost(target, 0, TEMP0);
				const unsigned high_output =
					DestinationHost(target, 1, TEMP0);
				auto commit_low = [&](unsigned value) -> bool {
					return (mask & 0x1) == 0 || EmitStoreValueWord(target, 0, value);
				};
				auto commit_high = [&](unsigned value) -> bool {
					return (mask & 0x2) == 0 || EmitStoreValueWord(target, 1, value);
				};
				auto emit_signed_high = [&](unsigned low) -> bool {
					return (mask & 0x2) == 0 ||
						(m_code.EmitMovRegShiftImm(high_output, low,
							ShiftType::ASR, 31) && commit_high(high_output));
				};
				auto emit_zero_high = [&]() -> bool {
					return (mask & 0x2) == 0 ||
						(m_code.EmitMovImm8(high_output, 0) &&
						 commit_high(high_output));
				};
				auto load_s8 = [&](unsigned output) {
					return address.Indexed() ?
						(address.offset == 0 && m_code.EmitLdrsbReg(
							output, address.base, address.index)) :
						m_code.EmitLdrsbImm8(output, address.base,
							static_cast<u8>(address.offset));
				};
				auto load_u8 = [&](unsigned output) {
					return address.Indexed() ?
						(address.offset == 0 && m_code.EmitLdrbRegShift(
							output, address.base, address.index, ShiftType::LSL, 0)) :
						m_code.EmitLdrbImm12(output, address.base, address.offset);
				};
				auto load_s16 = [&](unsigned output) {
					return address.Indexed() ?
						(address.offset == 0 && m_code.EmitLdrshReg(
							output, address.base, address.index)) :
						m_code.EmitLdrshImm8(output, address.base,
							static_cast<u8>(address.offset));
				};
				auto load_u16 = [&](unsigned output) {
					return address.Indexed() ?
						(address.offset == 0 && m_code.EmitLdrhReg(
							output, address.base, address.index)) :
						m_code.EmitLdrhImm8(output, address.base,
							static_cast<u8>(address.offset));
				};
				auto load_u32 = [&](unsigned output) {
					return address.Indexed() ?
						(address.offset == 0 && m_code.EmitLdrRegShift(
							output, address.base, address.index, ShiftType::LSL, 0)) :
						m_code.EmitLdrImm12(output, address.base, address.offset);
				};
				if ((mask & 0x2) == 0 &&
					(kind == MemoryAccessKind::LoadS8 ||
					 kind == MemoryAccessKind::LoadU8 ||
					 kind == MemoryAccessKind::LoadS16 ||
					 kind == MemoryAccessKind::LoadU16 ||
					 kind == MemoryAccessKind::LoadS32 ||
					 kind == MemoryAccessKind::LoadU32))
				{
					m_result.elided_load_high_words++;
				}
				bool loaded = false;
				switch (kind)
				{
					case MemoryAccessKind::LoadS8:
						loaded = load_s8(low_output) &&
							commit_low(low_output) && emit_signed_high(low_output);
						break;
					case MemoryAccessKind::LoadU8:
						loaded = load_u8(low_output) &&
							commit_low(low_output) && emit_zero_high();
						break;
					case MemoryAccessKind::LoadS16:
						loaded = load_s16(low_output) &&
							commit_low(low_output) && emit_signed_high(low_output);
						break;
					case MemoryAccessKind::LoadU16:
						loaded = load_u16(low_output) &&
							commit_low(low_output) && emit_zero_high();
						break;
					case MemoryAccessKind::LoadS32:
						loaded = load_u32(low_output) &&
							commit_low(low_output) && emit_signed_high(low_output);
						break;
					case MemoryAccessKind::LoadU32:
						loaded = load_u32(low_output) &&
							commit_low(low_output) && emit_zero_high();
						break;
					case MemoryAccessKind::LoadF32Bits:
						loaded = (mask & 0x1) == 0 ||
							(load_u32(TEMP0) && EmitStoreValueWord(target, 0, TEMP0));
						break;
					case MemoryAccessKind::Load64:
						loaded = ((mask & 0x1) == 0 ||
							EmitLoadedWord(target, 0, address, address.offset)) &&
							((mask & 0x2) == 0 ||
							 EmitLoadedWord(target, 1, address,
								 static_cast<u16>(address.offset + 4)));
						break;
					case MemoryAccessKind::Load128:
					case MemoryAccessKind::LoadVu0Vector:
						loaded = true;
						for (u8 word = 0; word < 4 && loaded; word++)
						{
							if ((mask & (1u << word)) != 0)
								loaded = EmitLoadedWord(target, word, address,
									static_cast<u16>(address.offset + word * 4));
						}
						break;
					default:
						return false;
				}
				if (!loaded)
					return false;
				if (preserve_upper)
				{
					for (u8 word = 2; word < 4; word++)
					{
						if ((mask & (1u << word)) != 0 &&
							(!EmitSpillLoad(TEMP0, m_work_scratch_offset + word * 4) ||
							 !EmitStoreValueWord(target, word, TEMP0)))
						{
							return false;
						}
					}
				}
				return true;
			}

			bool EmitMemoryStore(const Node& node, MemoryAccessKind kind,
				const DirectMemoryAddress& address)
			{
				m_result.memory_stores++;
				const u32 width = MemoryAccessWidth(kind);
				if (width == 0)
					return false;
				const RegionAllocation::Location& source = Location(node.operands[2]);
				if ((kind == MemoryAccessKind::Store128 ||
					 kind == MemoryAccessKind::StoreVu0Vector) &&
					LocationMask(source) == 0x0f &&
					source.kind == RegionAllocation::LocationKind::NeonQ)
				{
					return !address.Indexed() && address.offset == 0 &&
						m_code.EmitVst1Q32Aligned(source.index, address.base);
				}
				for (u8 word = 0; word < (width + 3) / 4; word++)
				{
					// Range-proven persistent accesses can keep the guest address as an
					// indexed operand in TEMP0. Loading a spilled store value into that
					// same register before STR[B/H] changes the address to the value bits
					// (for example `strh r0, [r8, r0]`). Select a scratch which is not
					// part of the address and let an already-resident source bypass the
					// scratch entirely. Address and data are simultaneous SSA uses, so an
					// allocation-owned source cannot legally alias either address host.
					unsigned scratch = TEMP0;
					if (scratch == address.base ||
						(address.Indexed() && scratch == address.index))
					{
						scratch = TEMP2;
					}
					if (scratch == address.base ||
						(address.Indexed() && scratch == address.index))
					{
						scratch = TEMP1;
					}
					unsigned source_host = 0;
					if (scratch == address.base ||
						(address.Indexed() && scratch == address.index) ||
						!AcquireValueWord(node.operands[2], word, scratch,
							&source_host))
					{
						return false;
					}
					const u16 offset = static_cast<u16>(address.offset + word * 4);
					const bool stored = address.Indexed() ?
						(offset == 0 &&
						 ((width == 1 && m_code.EmitStrbRegShift(source_host, address.base,
							 address.index, ShiftType::LSL, 0)) ||
						  (width == 2 && m_code.EmitStrhReg(source_host, address.base,
							 address.index)) ||
						  (width >= 4 && m_code.EmitStrRegShift(source_host, address.base,
							 address.index, ShiftType::LSL, 0)))) :
						((width == 1 && m_code.EmitStrbImm12(source_host, address.base, offset)) ||
						 (width == 2 && m_code.EmitStrhImm8(source_host, address.base,
							 static_cast<u8>(offset))) ||
						 (width >= 4 && m_code.EmitStrImm12(source_host, address.base,
							 offset)));
					if (!stored)
					{
						return false;
					}
				}
				return true;
			}

			const RegionMemoryPlan::Access* FindRangeAccess(
				ValueId operation) const
			{
				for (const RegionMemoryPlan::CountedRange& range :
					m_memory_plan.counted_ranges)
				{
					const auto found = std::find_if(range.accesses.begin(),
						range.accesses.end(),
						[&](const RegionMemoryPlan::Access& access) {
							return access.operation == operation;
						});
					if (found != range.accesses.end())
						return &*found;
				}
				for (const RegionMemoryPlan::BoundedRange& range :
					m_memory_plan.bounded_ranges)
				{
					const auto found = std::find_if(range.accesses.begin(),
						range.accesses.end(),
						[&](const RegionMemoryPlan::Access& access) {
							return access.operation == operation;
						});
					if (found != range.accesses.end())
						return &*found;
				}
				return nullptr;
			}

			const RegionMemoryPlan::BoundedRange* FindAbsoluteRange(
				ValueId operation) const
			{
				for (const RegionMemoryPlan::BoundedRange& range :
					m_memory_plan.bounded_ranges)
				{
					if (!range.absolute_address)
						continue;
					if (std::any_of(range.accesses.begin(), range.accesses.end(),
						[&](const RegionMemoryPlan::Access& access) {
							return access.operation == operation;
						}))
					{
						return &range;
					}
				}
				return nullptr;
			}

			bool EmitLiteralPool()
			{
				std::vector<std::pair<u32, size_t>> literals;
				literals.reserve(m_literal_patches.size());
				for (const LiteralPatch& patch : m_literal_patches)
				{
					auto found = std::find_if(literals.begin(), literals.end(),
						[&](const auto& literal) {
							return literal.first == patch.value;
						});
					if (found == literals.end())
					{
						const size_t offset = m_code.Size();
						if (!m_code.EmitU32(patch.value))
							return false;
						literals.emplace_back(patch.value, offset);
						found = std::prev(literals.end());
					}
					if (!m_code.PatchLdrLiteral(patch.instruction_offset,
							found->second))
					{
						return false;
					}
				}
				return true;
			}

			u16 DirectMemoryOffsetLimit(MemoryAccessKind kind) const
			{
				switch (kind)
				{
					case MemoryAccessKind::LoadS8:
					case MemoryAccessKind::LoadS16:
					case MemoryAccessKind::LoadU16:
					case MemoryAccessKind::Store16:
						return 255;
					case MemoryAccessKind::LoadU8:
					case MemoryAccessKind::LoadS32:
					case MemoryAccessKind::LoadU32:
					case MemoryAccessKind::LoadF32Bits:
					case MemoryAccessKind::Store8:
					case MemoryAccessKind::Store32:
					case MemoryAccessKind::StoreF32Bits:
						return 4095;
					case MemoryAccessKind::Load64:
					case MemoryAccessKind::Store64:
						return 4091;
					default:
						return 0;
				}
			}

			void BuildHoistedMemoryBaseCandidates()
			{
				m_hoisted_memory_bases.clear();
				if (!Persistent() || !m_reclaim_vtlb_vmap)
					return;
				for (const RegionMemoryPlan::BoundedRange& range :
					m_memory_plan.bounded_ranges)
				{
					if (!range.valid || range.accesses.empty() ||
						(!range.absolute_address && range.base_gpr == 0))
					{
						continue;
					}
					s64 exact_minimum = INT64_MAX;
					s64 exact_maximum_end = INT64_MIN;
					HoistedMemoryBase candidate{};
					candidate.absolute = range.absolute_address;
					candidate.base_gpr = range.base_gpr;
					candidate.guest_offset = range.minimum_offset;
					bool representable = true;
					for (const RegionMemoryPlan::Access& access : range.accesses)
					{
						const Node* const operation = Definition(access.operation);
						if (!operation || operation->immediate >
								static_cast<u32>(MemoryAccessKind::StoreVu0Vector) ||
							access.induction_offset < range.minimum_offset)
						{
							representable = false;
							break;
						}
						const MemoryAccessKind kind =
							static_cast<MemoryAccessKind>(operation->immediate);
						const u32 relative = static_cast<u32>(
							access.induction_offset - range.minimum_offset);
						const u16 limit = DirectMemoryOffsetLimit(kind);
						if ((limit == 0 && relative != 0) || relative > limit)
						{
							representable = false;
							break;
						}
						candidate.accesses.push_back({access.operation,
							static_cast<u16>(relative)});
						exact_minimum = std::min<s64>(exact_minimum,
							access.induction_offset);
						exact_maximum_end = std::max<s64>(exact_maximum_end,
							static_cast<s64>(access.induction_offset) + access.width);
					}
					// Access::induction_offset records the lower endpoint of a
					// bounded expression.  It is a fixed address only when rebuilding
					// the span from those accesses reproduces the complete proven
					// range; otherwise the data-dependent offset must stay in IR.
					if (!representable || exact_minimum != range.minimum_offset ||
						exact_maximum_end != range.maximum_offset_end)
					{
						continue;
					}
					candidate.score = static_cast<u32>(candidate.accesses.size()) *
						(candidate.absolute ? 1u : 2u);
					m_hoisted_memory_bases.push_back(std::move(candidate));
				}
				// Constant-address reads which are close enough for A32's addressing
				// mode share one retained base.  This is a semantic address-range
				// coalescing, not literal-pool CSE: every access keeps its original
				// width, alignment, ordering, and independently verified memory node.
				std::sort(m_hoisted_memory_bases.begin(),
					m_hoisted_memory_bases.end(),
					[](const HoistedMemoryBase& left,
						const HoistedMemoryBase& right) {
						if (left.absolute != right.absolute)
							return !left.absolute;
						return left.guest_offset < right.guest_offset;
					});
				std::vector<HoistedMemoryBase> coalesced;
				for (HoistedMemoryBase& candidate : m_hoisted_memory_bases)
				{
					bool merged = false;
					if (candidate.absolute)
					{
						for (HoistedMemoryBase& prior : coalesced)
						{
							if (!prior.absolute ||
								candidate.guest_offset < prior.guest_offset)
							{
								continue;
							}
							const u32 delta = static_cast<u32>(
								candidate.guest_offset - prior.guest_offset);
							bool fits = true;
							for (const HoistedMemoryAccess& access : candidate.accesses)
							{
								const Node* const operation = Definition(access.operation);
								const MemoryAccessKind kind = static_cast<MemoryAccessKind>(
									operation->immediate);
								const u32 offset = delta + access.offset;
								const u16 limit = DirectMemoryOffsetLimit(kind);
								fits &= (limit != 0 || offset == 0) && offset <= limit;
							}
							if (!fits)
								continue;
							for (const HoistedMemoryAccess& access : candidate.accesses)
								prior.accesses.push_back({access.operation,
									static_cast<u16>(delta + access.offset)});
							prior.score += candidate.score;
							merged = true;
							break;
						}
					}
					if (!merged)
						coalesced.push_back(std::move(candidate));
				}
				m_hoisted_memory_bases = std::move(coalesced);
				std::sort(m_hoisted_memory_bases.begin(),
					m_hoisted_memory_bases.end(),
					[](const HoistedMemoryBase& left,
						const HoistedMemoryBase& right) {
						if (left.score != right.score)
							return left.score > right.score;
						if (left.absolute != right.absolute)
							return !left.absolute;
						return left.guest_offset < right.guest_offset;
					});
			}

			const HoistedMemoryBase* FindHoistedMemoryBase(ValueId operation,
				u16* offset) const
			{
				for (const HoistedMemoryBase& base : m_hoisted_memory_bases)
				{
					const auto found = std::find_if(base.accesses.begin(),
						base.accesses.end(), [&](const HoistedMemoryAccess& access) {
							return access.operation == operation;
						});
					if (found == base.accesses.end())
						continue;
					if (offset)
						*offset = found->offset;
					return &base;
				}
				return nullptr;
			}

			bool IsHoistedMemoryLoad(ValueId operation) const
			{
				return std::any_of(m_hoisted_memory_loads.begin(),
					m_hoisted_memory_loads.end(), [&](const HoistedMemoryLoad& load) {
						return load.operation == operation;
					});
			}

			void BuildHoistedMemoryLoadCandidates()
			{
				m_hoisted_memory_loads.clear();
				if (!Persistent() || m_hoisted_memory_bases.empty())
					return;

				// A scheduler/device event is the only external writer which can become
				// visible while the EE owns this region, and the aggregate event guard
				// exits before the next iteration in that case.  An in-region store is a
				// different matter: even a presently disjoint address may alias through a
				// later TLB or low-word recurrence, so fail closed for the whole region.
				for (const Block& block : m_program.blocks)
					for (const Node& node : block.nodes)
						if (node.opcode == Opcode::MemoryStore)
							return;

				auto core_locations_overlap = [](const RegionAllocation::Location& left,
					const RegionAllocation::Location& right) {
					if (left.kind != RegionAllocation::LocationKind::Core ||
						right.kind != RegionAllocation::LocationKind::Core)
					{
						return false;
					}
					return left.index < right.index + right.words &&
						right.index < left.index + left.words;
				};
				for (const HoistedMemoryBase& base : m_hoisted_memory_bases)
				{
					for (const HoistedMemoryAccess& access : base.accesses)
					{
						const Node* const operation = Definition(access.operation);
						const ValueId value = access.operation <
							m_memory_value_for_effect.size() ?
							m_memory_value_for_effect[access.operation] : INVALID_VALUE;
						if (!operation || operation->opcode != Opcode::MemoryLoad ||
							value == INVALID_VALUE || StorageValue(value) != value ||
							MemoryAccessWidth(static_cast<MemoryAccessKind>(
								operation->immediate)) > sizeof(u32))
						{
							continue;
						}
						const RegionAllocation::Location& location = Location(value);
						if (location.kind != RegionAllocation::LocationKind::Core ||
							location.words != 1 || LocationMask(location) != 0x1)
						{
							continue;
						}
						const bool region_resident = std::find(
							m_region_resident_memory_load_values.begin(),
							m_region_resident_memory_load_values.end(), value) !=
							m_region_resident_memory_load_values.end();
						if (region_resident)
						{
							m_hoisted_memory_loads.push_back({access.operation, value});
							continue;
						}

						// The value is retained across every backedge only when no other SSA
						// interval or edge-copy owner can reuse its core word in any block.
						// This is deliberately stricter than ordinary block-local liveness;
						// it makes the existing allocation itself the mechanical lifetime
						// certificate and needs no hidden cross-block register ABI.
						bool exclusive = true;
						for (const RegionAllocation::Interval& interval :
							m_allocation.intervals)
						{
							if (interval.value != value &&
								core_locations_overlap(location, interval.location))
							{
								exclusive = false;
								break;
							}
						}
						for (const RegionAllocation::EdgeMove& move : m_allocation.edge_moves)
						{
							exclusive &= !core_locations_overlap(location,
								move.source_location) &&
								!core_locations_overlap(location, move.target_location);
						}
						if (exclusive)
							m_hoisted_memory_loads.push_back({access.operation, value});
					}
				}
				m_result.hoisted_memory_loads = static_cast<u32>(
					m_hoisted_memory_loads.size());
			}

			bool EmitHoistedMemoryBases()
			{
				for (const HoistedMemoryBase& base : m_hoisted_memory_bases)
				{
					if (base.absolute)
					{
						const uptr host = reinterpret_cast<uptr>(
							m_options.persistent_dispatch->main_ram) +
							static_cast<u32>(base.guest_offset);
						if (!m_code.EmitMovImm32(base.host, static_cast<u32>(host)))
							return false;
						continue;
					}
					if (!EmitEntryGprWord(base.base_gpr, 0, TEMP0))
						return false;
					if (base.guest_offset > 0 &&
						!m_code.EmitAddImm32(TEMP0, TEMP0,
							static_cast<u32>(base.guest_offset)))
					{
						return false;
					}
					if (base.guest_offset < 0 &&
						!m_code.EmitSubImm32(TEMP0, TEMP0, static_cast<u32>(
							-static_cast<s64>(base.guest_offset))))
					{
						return false;
					}
					if (!m_code.EmitAddReg(base.host, VTLB_HOST_BASE, TEMP0))
						return false;
				}
				return true;
			}

			bool EmitHoistedMemoryLoads()
			{
				for (const HoistedMemoryLoad& load : m_hoisted_memory_loads)
				{
					const Node* const operation = Definition(load.operation);
					if (!operation || operation->opcode != Opcode::MemoryLoad ||
						load.value >= m_definitions.size())
					{
						return false;
					}
					const MemoryAccessKind kind =
						static_cast<MemoryAccessKind>(operation->immediate);
					DirectMemoryAddress address{};
					if (!EmitRangeProvenMemoryHostAddress(*operation, kind, &address) ||
						!EmitMemoryLoad(*operation, kind, address))
					{
						return false;
					}
				}
				return true;
			}

			bool EmitRangeProvenMemoryHostAddress(const Node& node,
				MemoryAccessKind kind, DirectMemoryAddress* result)
			{
				if (!result)
					return false;
				*result = {};
				u16 hoisted_offset = 0;
				if (const HoistedMemoryBase* const hoisted =
						FindHoistedMemoryBase(node.id, &hoisted_offset))
				{
					result->base = hoisted->host;
					result->offset = hoisted_offset;
					return true;
				}
				if (const RegionMemoryPlan::BoundedRange* absolute =
						FindAbsoluteRange(node.id))
				{
					if (!Persistent() || absolute->minimum_offset < 0 ||
						absolute->maximum_offset_end <= absolute->minimum_offset)
					{
						return false;
					}
					const uptr host = reinterpret_cast<uptr>(
						m_options.persistent_dispatch->main_ram) +
						static_cast<u32>(absolute->minimum_offset);
					const size_t load = m_code.EmitLdrLiteralPlaceholder(TEMP1);
					if (load == static_cast<size_t>(-1))
						return false;
					m_literal_patches.push_back({load, static_cast<u32>(host)});
					return true;
				}
				const Node* const address = Definition(node.operands[1]);
				const u16 offset_limit = DirectMemoryOffsetLimit(kind);
				if (address &&
					address->opcode == Opcode::EffectiveAddress32 &&
					address->operand_count == 2)
				{
					u32 immediate = 0;
					ValueId base = INVALID_VALUE;
					if (ImmediateWord(address->operands[1], 0, &immediate))
						base = address->operands[0];
					else if (ImmediateWord(address->operands[0], 0, &immediate))
						base = address->operands[1];
					if (base != INVALID_VALUE && immediate <= offset_limit)
					{
						unsigned base_host = 0;
						if (!AcquireValueWord(base, 0, TEMP0, &base_host))
							return false;
						if (Persistent() && immediate == 0 &&
							MemoryAccessWidth(kind) <= sizeof(u32))
						{
							result->base = VTLB_HOST_BASE;
							result->index = base_host;
							return true;
						}
						const bool emitted = Persistent() ?
							m_code.EmitAddReg(TEMP1, VTLB_HOST_BASE, base_host) :
							(EmitContextLoad(TEMP1,
								offsetof(ExecutionContext, main_ram)) &&
							 m_code.EmitAddReg(TEMP1, TEMP1, base_host));
						if (!emitted)
							return false;
						result->offset = static_cast<u16>(immediate);
						return true;
					}
				}
				unsigned address_host = 0;
				if (!AcquireValueWord(node.operands[1], 0, TEMP0, &address_host))
				{
					m_result.failure_emission_step = 27;
					return false;
				}
				if (Persistent() && MemoryAccessWidth(kind) <= sizeof(u32))
				{
					result->base = VTLB_HOST_BASE;
					result->index = address_host;
					return true;
				}
				// vtlb_Core_Alloc() places EE main RAM at arena offset zero.  The
				// persistent dispatcher already keeps that arena base in r8, so a range
				// proven access is one ADD instead of rematerializing eeMem->Main for
				// every iteration.  Callable validation has no persistent ABI and keeps
				// using its explicit context pointer.
				const bool emitted = Persistent() ?
					m_code.EmitAddReg(TEMP1, VTLB_HOST_BASE, address_host) :
					(EmitContextLoad(TEMP1, offsetof(ExecutionContext, main_ram)) &&
					 m_code.EmitAddReg(TEMP1, TEMP1, address_host));
				if (!emitted)
					m_result.failure_emission_step = 28;
				return emitted;
			}

			bool EmitMemoryOperation(u32 block_index, const Node& node)
			{
				if (node.opcode == Opcode::MemoryLoad &&
					node.id < m_allocation.forwarded_memory_load_effect.size() &&
					m_allocation.forwarded_memory_load_effect[node.id] != 0)
				{
					return true;
				}
				if (node.opcode == Opcode::MemoryLoad &&
					IsHoistedMemoryLoad(node.id))
				{
					return true;
				}
				if (node.immediate > static_cast<u32>(MemoryAccessKind::StoreVu0Vector))
				{
					m_result.failure_emission_step = 10;
					return UnsupportedMemory(node);
				}
				const MemoryAccessKind kind = static_cast<MemoryAccessKind>(node.immediate);
				if ((node.opcode == Opcode::MemoryLoad) != IsMemoryLoad(kind))
				{
					m_result.failure_emission_step = 11;
					return UnsupportedMemory(node);
				}
				const RegionExecution::ExitSite* site =
					FindMemoryExitSite(block_index, node.id);
				const RegionMemoryPlan::Access* range_access =
					FindRangeAccess(node.id);
				DirectMemoryAddress address{};
				if (!site)
				{
					m_result.failure_emission_step = 12;
					return false;
				}
				if (!(range_access ?
					EmitRangeProvenMemoryHostAddress(node, kind, &address) :
						EmitResolveMemoryHostAddress(node, kind, site)))
				{
					if (m_result.failure_emission_step == 0)
						m_result.failure_emission_step = 13;
					return false;
				}
				m_result.indexed_scalar_memory_accesses += address.Indexed() ? 1u : 0u;
				if (!range_access && node.opcode == Opcode::MemoryStore &&
					!EmitStoreSourceOwnership(node, kind, site))
				{
					m_result.failure_emission_step = 14;
					return false;
				}
				const bool emitted = node.opcode == Opcode::MemoryLoad ?
					EmitMemoryLoad(node, kind, address) :
					EmitMemoryStore(node, kind, address);
				if (!emitted)
					m_result.failure_emission_step = 15;
				return emitted;
			}

			bool UnsupportedMemory(const Node& node)
			{
				Fail(CompileFailure::UnsupportedMemory, node.source_pc);
				return false;
			}

			bool EmitNode(u32 block_index, const Node& node)
			{
				if (StorageValue(node.id) != node.id &&
					node.opcode != Opcode::Vu0RequireIdle)
				{
					return true;
				}
				if (node.opcode == Opcode::Cop1NormalizeInput &&
					node.id < m_allocation.cop1_hoisted_normalize.size() &&
					m_allocation.cop1_hoisted_normalize[node.id] != 0)
				{
					return true;
				}
				if (node.opcode == Opcode::Vu0NormalizeVector &&
					node.id < m_allocation.vu0_hoisted_normalize.size() &&
					m_allocation.vu0_hoisted_normalize[node.id] != 0)
				{
					return true;
				}
				const u8 demand = node.id < m_allocation.value_word_demands.size() ?
					m_allocation.value_word_demands[node.id] : 0;
				switch (node.opcode)
				{
					case Opcode::Parameter:
					case Opcode::ConstantI1:
					case Opcode::ConstantI32:
					case Opcode::ConstantI64:
					case Opcode::ConstantAddress:
					case Opcode::NoEffect:
					case Opcode::BindGpr:
					case Opcode::BindHi:
					case Opcode::BindLo:
					case Opcode::BindSa:
					case Opcode::BindFpr:
					case Opcode::BindVu0Vf:
					case Opcode::BindVu0Acc:
					case Opcode::BindVu0MacFlag:
					case Opcode::BindVu0StatusFlag:
					case Opcode::BindVu0ViMac:
					case Opcode::BindVu0ViStatus:
					case Opcode::BindVu0Q:
					case Opcode::BindVu0ViQ:
					case Opcode::BindVu0Vi:
					case Opcode::BindVu0ClipFlag:
					case Opcode::BindVu0MicroStatusFlag:
					case Opcode::BindFcr31:
					case Opcode::BindAcc:
					case Opcode::AdvanceCycles:
						return true;
					case Opcode::ExitIfTrue:
						return EmitGuardedExit(block_index, node);
					case Opcode::Cop1ExceptionalOuResult:
						return EmitCop1InternalOuClassifier(block_index, node);
					case Opcode::MemoryLoad:
					case Opcode::MemoryStore:
						return EmitMemoryOperation(block_index, node);
					case Opcode::MemoryLoadValue:
						// The ordered effect emitted the load directly into this
						// value's allocated location.
						return true;
					case Opcode::Vu0RequireIdle:
						return EmitVu0RequireIdle(block_index, node);
					default:
						break;
				}
				if (demand == 0)
					return true;
				auto attribute = [&](u32* bytes, auto&& emit) {
					const size_t before = m_code.Size();
					if (!emit())
						return false;
					*bytes += static_cast<u32>(m_code.Size() - before);
					return true;
				};

					switch (node.opcode)
					{
					case Opcode::Vu0ConvertFixed:
						return attribute(&m_result.vu0_arithmetic_hot_bytes,
							[&]() { return EmitVu0ConvertFixed(node); });
					case Opcode::Vu0ConvertIntegerToFloat:
						return attribute(&m_result.vu0_arithmetic_hot_bytes,
							[&]() { return EmitVu0ConvertIntegerToFloat(node); });
					case Opcode::Vu0Rotate32:
						return attribute(&m_result.vu0_arithmetic_hot_bytes,
							[&]() { return EmitVu0Rotate32(node); });
					case Opcode::Vu0NormalizeVector:
						return attribute(&m_result.vu0_normalize_hot_bytes,
							[&]() { return EmitVu0NormalizeVector(node); });
					case Opcode::Vu0BroadcastLane:
						return attribute(&m_result.vu0_broadcast_hot_bytes,
							[&]() { return EmitVu0BroadcastLane(node); });
					case Opcode::Vu0BroadcastScalar:
						return attribute(&m_result.vu0_broadcast_hot_bytes,
							[&]() { return EmitVu0BroadcastScalar(node); });
					case Opcode::Vu0FdivQ:
						return attribute(&m_result.vu0_arithmetic_hot_bytes,
							[&]() { return EmitVu0FdivQ(node); });
					case Opcode::Vu0FdivFlags:
						return attribute(&m_result.vu0_status_flag_hot_bytes,
							[&]() { return EmitVu0FdivFlags(node); });
					case Opcode::Vu0UpdateFdivStatus:
						return attribute(&m_result.vu0_status_flag_hot_bytes,
							[&]() { return EmitVu0UpdateFdivStatus(node); });
					case Opcode::Vu0SyncFdivStatusControl:
						return attribute(&m_result.vu0_status_flag_hot_bytes,
							[&]() { return EmitVu0SyncFdivStatusControl(node); });
					case Opcode::Vu0MulRaw:
					case Opcode::Vu0AddRaw:
					case Opcode::Vu0SubRaw:
						return attribute(&m_result.vu0_arithmetic_hot_bytes,
							[&]() { return EmitVu0RawArithmetic(node); });
					case Opcode::Vu0ClampFmacResult:
						return attribute(&m_result.vu0_clamp_hot_bytes,
							[&]() { return EmitVu0ClampFmacResult(node); });
					case Opcode::Vu0MacFlagsFromRaw:
						return attribute(&m_result.vu0_mac_flag_hot_bytes,
							[&]() { return EmitVu0MacFlagsFromRaw(node); });
					case Opcode::Vu0StatusFlagsFromMac:
						return attribute(&m_result.vu0_status_flag_hot_bytes,
							[&]() { return EmitVu0StatusFlagsFromMac(node); });
					case Opcode::Vu0MergeMasked:
						return attribute(&m_result.vu0_merge_hot_bytes,
							[&]() { return EmitVu0MergeMasked(node); });
					case Opcode::Vu0SyncStatusControl:
						return attribute(&m_result.vu0_status_flag_hot_bytes,
							[&]() { return EmitVu0SyncStatusControl(node); });
					case Opcode::Vu0ControlWrite:
						return EmitVu0ControlWrite(node);
					case Opcode::Vu0DenormalizeStatus:
						return EmitVu0DenormalizeStatus(node);
					case Opcode::Cop1NormalizeInput:
						return EmitCop1NormalizeInput(node);
					case Opcode::Cop1AddRaw:
					case Opcode::Cop1SubRaw:
					case Opcode::Cop1MulRaw:
						return EmitCop1RawArithmetic(node);
					case Opcode::Cop1ClampOuResult:
						return EmitCop1ClampOuResult(node);
					case Opcode::Cop1UpdateOuFlags:
						return EmitCop1UpdateOuFlags(node);
					case Opcode::Cop1CompareEqual:
					case Opcode::Cop1CompareLess:
					case Opcode::Cop1CompareLessEqual:
						return EmitCop1Compare(node);
					case Opcode::Cop1UpdateConditionFlag:
						return EmitCop1UpdateConditionFlag(node);
					case Opcode::Cop1BranchCondition:
						return EmitCop1BranchCondition(node);
					case Opcode::Cop1AbsoluteWord:
					case Opcode::Cop1NegateWord:
						return EmitCop1UnaryWord(node);
					case Opcode::Cop1ClearOuFlags:
						return EmitCop1ClearOuFlags(node);
					case Opcode::Cop1ConvertWord:
						return EmitCop1ConvertWord(node);
					case Opcode::Cop1ConvertSingle:
						return EmitCop1ConvertSingle(node);
					case Opcode::ExtractLow32:
						return EmitCopyMappings(node.id, {{0, node.operands[0], 0}});
					case Opcode::ExtractLow64:
						return EmitCopyMappings(node.id,
							{{0, node.operands[0], 0}, {1, node.operands[0], 1}});
					case Opcode::ExtractHigh64:
						return EmitCopyMappings(node.id,
							{{0, node.operands[0], 2}, {1, node.operands[0], 3}});
					case Opcode::ReplaceLow64:
						return EmitCopyMappings(node.id,
							{{0, node.operands[1], 0}, {1, node.operands[1], 1},
							 {2, node.operands[0], 2}, {3, node.operands[0], 3}});
					case Opcode::ReplaceHigh64:
						return EmitCopyMappings(node.id,
							{{0, node.operands[0], 0}, {1, node.operands[0], 1},
							 {2, node.operands[1], 0}, {3, node.operands[1], 1}});
					case Opcode::BitcastI32ToF32Bits:
					case Opcode::BitcastF32BitsToI32:
					case Opcode::AddressFromI32:
						return EmitCopyMappings(node.id, {{0, node.operands[0], 0}});
					case Opcode::BitcastI128ToVuF32x4Bits:
					case Opcode::BitcastVuF32x4BitsToI128:
						return EmitCopyMappings(node.id,
							{{0, node.operands[0], 0}, {1, node.operands[0], 1},
							 {2, node.operands[0], 2}, {3, node.operands[0], 3}});
					case Opcode::SignExtend32To64:
						return EmitSignExtend(node, true);
					case Opcode::ZeroExtend32To64:
						return EmitSignExtend(node, false);
					case Opcode::MultiplySigned32:
						return EmitMultiply32(node, true);
					case Opcode::MultiplyUnsigned32:
						return EmitMultiply32(node, false);
					case Opcode::Truncate64To32:
						return EmitCopyMappings(node.id, {{0, node.operands[0], 0}});
					case Opcode::Add32:
						return EmitBinary32(node, BinaryKind::Add);
					case Opcode::Sub32:
						return EmitBinary32(node, BinaryKind::Subtract);
					case Opcode::And32:
						return EmitBinary32(node, BinaryKind::And);
					case Opcode::Xor32:
						return EmitBinary32(node, BinaryKind::Xor);
					case Opcode::Add64:
						return EmitBinary64(node, BinaryKind::Add);
					case Opcode::Sub64:
						return EmitBinary64(node, BinaryKind::Subtract);
					case Opcode::And64:
						return EmitBinary64(node, BinaryKind::And);
					case Opcode::Or64:
						return EmitBinary64(node, BinaryKind::Or);
					case Opcode::Xor64:
						return EmitBinary64(node, BinaryKind::Xor);
					case Opcode::Nor64:
						return EmitBinary64(node, BinaryKind::Nor);
					case Opcode::And128:
						return EmitBinary128(node, BinaryKind::And);
					case Opcode::Or128:
						return EmitBinary128(node, BinaryKind::Or);
					case Opcode::Xor128:
						return EmitBinary128(node, BinaryKind::Xor);
					case Opcode::Nor128:
						return EmitBinary128(node, BinaryKind::Nor);
					case Opcode::PackedBinary128:
						return EmitPackedBinary128(node);
					case Opcode::PackedShift128:
						return EmitPackedShift128(node);
					case Opcode::BroadcastLowHalfwordPer64:
						return EmitBroadcastLowHalfwordPer64(node);
					case Opcode::SignedAddOverflow32:
						return EmitSignedOverflow32(node);
					case Opcode::ShiftLeft32:
						return EmitShift32(node, ShiftType::LSL, false);
					case Opcode::ShiftRightLogical32:
						return EmitShift32(node, ShiftType::LSR, false);
					case Opcode::ShiftRightArithmetic32:
						return EmitShift32(node, ShiftType::ASR, false);
					case Opcode::ShiftLeft32Variable:
						return EmitShift32(node, ShiftType::LSL, true);
					case Opcode::ShiftRightLogical32Variable:
						return EmitShift32(node, ShiftType::LSR, true);
					case Opcode::ShiftRightArithmetic32Variable:
						return EmitShift32(node, ShiftType::ASR, true);
					case Opcode::ShiftLeft64:
						return EmitShift64Immediate(node, ShiftType::LSL);
					case Opcode::ShiftRightLogical64:
						return EmitShift64Immediate(node, ShiftType::LSR);
					case Opcode::ShiftRightArithmetic64:
						return EmitShift64Immediate(node, ShiftType::ASR);
					case Opcode::ShiftLeft64Variable:
						return EmitShift64Variable(node, ShiftType::LSL);
					case Opcode::ShiftRightLogical64Variable:
						return EmitShift64Variable(node, ShiftType::LSR);
					case Opcode::ShiftRightArithmetic64Variable:
						return EmitShift64Variable(node, ShiftType::ASR);
					case Opcode::Select64:
						return EmitSelect64(node);
					case Opcode::CompareEqual64:
						return EmitCompareEqual64(node, true);
					case Opcode::CompareNotEqual64:
						return EmitCompareEqual64(node, false);
					case Opcode::CompareSignedLess64:
						return EmitCompareOrdered64(node, true);
					case Opcode::CompareUnsignedLess64:
						return EmitCompareOrdered64(node, false);
					case Opcode::CompareSignedLessEqualZero64:
						return EmitCompareZero64(node, Condition::LE);
					case Opcode::CompareSignedGreaterZero64:
						return EmitCompareZero64(node, Condition::GT);
					case Opcode::CompareSignedLessZero64:
						return EmitCompareZero64(node, Condition::MI);
					case Opcode::CompareSignedGreaterEqualZero64:
						return EmitCompareZero64(node, Condition::PL);
					case Opcode::EffectiveAddress32:
						return node.id <
							m_allocation.folded_effective_addresses.size() &&
							m_allocation.folded_effective_addresses[node.id] != 0 ?
							true : EmitBinary32(node, BinaryKind::Add);
					case Opcode::PackLow64:
						return EmitCopyMappings(node.id,
							{{0, node.operands[1], 0}, {1, node.operands[1], 1},
							 {2, node.operands[0], 0}, {3, node.operands[0], 1}});
					case Opcode::PackHigh64:
						return EmitCopyMappings(node.id,
							{{0, node.operands[0], 2}, {1, node.operands[0], 3},
							 {2, node.operands[1], 2}, {3, node.operands[1], 3}});
					default:
						return Unsupported(node);
				}
			}

			bool Unsupported(const Node& node)
			{
				Fail(CompileFailure::UnsupportedInstruction, node.source_pc);
				return false;
			}

			struct SemanticCompletion
			{
				const Transfer* transfer = nullptr;
				RegionExecution::ExitSiteKind site_kind =
					RegionExecution::ExitSiteKind::Taken;
			};

			bool TransferConstantPc(const Transfer& transfer, u32* pc) const
			{
				if (!pc)
					return false;
				const Node* const value = Definition(transfer.pc);
				if (!value || value->opcode != Opcode::ConstantAddress)
					return false;
				*pc = static_cast<u32>(value->literal);
				return true;
			}

			bool FindSemanticCompletion(const SemanticKernel::Plan& plan,
				SemanticCompletion* completion) const
			{
				if (!completion || plan.latch_block >= m_program.blocks.size())
					return false;
				const Block& latch = m_program.blocks[plan.latch_block];
				if (latch.terminator.kind != TerminatorKind::Branch)
					return false;
				SemanticCompletion found{};
				u32 matches = 0;
				auto inspect = [&](const Transfer& transfer,
					RegionExecution::ExitSiteKind kind) {
					u32 pc = 0;
					const bool same_target = plan.completion_block != INVALID_BLOCK ?
						transfer.target_block == plan.completion_block :
						transfer.target_block == INVALID_BLOCK &&
						TransferConstantPc(transfer, &pc) && pc == plan.completion_pc;
					if (same_target)
					{
						found = {&transfer, kind};
						matches++;
					}
				};
				inspect(latch.terminator.taken,
					RegionExecution::ExitSiteKind::Taken);
				inspect(latch.terminator.not_taken,
					RegionExecution::ExitSiteKind::NotTaken);
				if (matches != 1)
					return false;
				*completion = found;
				return true;
			}

			bool IsEntrySemanticValue(ValueId value) const
			{
				const Node* const node = Definition(value);
				if (!node)
					return false;
				if (node->opcode == Opcode::ConstantI32 ||
					node->opcode == Opcode::ConstantI64)
				{
					return true;
				}
				if (node->opcode != Opcode::Parameter ||
					m_program.entry_block >= m_program.blocks.size())
				{
					return false;
				}
				const Block& entry = m_program.blocks[m_program.entry_block];
				return std::any_of(entry.nodes.begin(), entry.nodes.end(),
					[&](const Node& candidate) {
						return candidate.id == value &&
							candidate.opcode == Opcode::Parameter;
					});
			}

			bool IsSemanticSearchIterationBlock(u32 block) const
			{
				return m_emit_semantic_search_island &&
					std::find(m_semantic_plan.iteration_blocks.begin(),
						m_semantic_plan.iteration_blocks.end(), block) !=
						m_semantic_plan.iteration_blocks.end();
			}

			bool SelectSemanticSearchIsland(
				const SemanticKernel::Plan& plan) const
			{
				if (plan.kind != SemanticKernel::Kind::BoundedEqualSearch ||
					plan.bounded_equal_searches.size() != 1 ||
					plan.header_block >= m_program.blocks.size() ||
					plan.latch_block >= m_program.blocks.size() ||
					plan.completion_block >= m_program.blocks.size() ||
					plan.iteration_blocks.empty() || !plan.complete_preflight ||
					!plan.exact_event_phase || plan.requires_disjoint_write_streams ||
					plan.repeated_scaled_cycles == 0 ||
					plan.completion_scaled_cycles == 0 ||
					!m_memory_plan.control.valid ||
					m_memory_plan.control != plan.control ||
					!m_memory_plan.header_seeds_match_entry ||
					!m_reclaim_vtlb_vmap ||
					m_memory_plan.timing.header_block != plan.header_block)
				{
					return false;
				}

				const SemanticKernel::BoundedEqualSearch& search =
					plan.bounded_equal_searches.front();
				if (search.load_operation == INVALID_VALUE ||
					search.load_value == INVALID_VALUE ||
					search.key_value == INVALID_VALUE ||
					search.comparison_block >= m_program.blocks.size() ||
					search.match_target_block >= m_program.blocks.size() ||
					search.continue_target_block >= m_program.blocks.size() ||
					search.entry_induction_gpr == 0 ||
					search.induction_gpr == 0 || search.stride == 0 ||
					(search.stride & (search.stride - 1)) != 0 ||
					search.alignment == 0 ||
					(search.alignment & (search.alignment - 1)) != 0 ||
					search.match_scaled_cycles == 0 ||
					search.native_match_outputs.size() != search.match_state.size() ||
					search.native_exhausted_outputs.size() !=
						search.exhausted_state.size())
				{
					return false;
				}
				if (std::find(plan.iteration_blocks.begin(),
						plan.iteration_blocks.end(), plan.header_block) ==
						plan.iteration_blocks.end() ||
					std::find(plan.iteration_blocks.begin(),
						plan.iteration_blocks.end(), plan.latch_block) ==
						plan.iteration_blocks.end() ||
					std::find(plan.iteration_blocks.begin(),
						plan.iteration_blocks.end(), search.comparison_block) ==
						plan.iteration_blocks.end() ||
					std::find(plan.iteration_blocks.begin(),
						plan.iteration_blocks.end(), search.match_target_block) !=
						plan.iteration_blocks.end() ||
					std::find(plan.iteration_blocks.begin(),
						plan.iteration_blocks.end(), plan.completion_block) !=
						plan.iteration_blocks.end())
				{
					return false;
				}

				for (const SemanticKernel::Low32Recurrence& recurrence :
					plan.recurrences)
				{
					if (recurrence.gpr == 0 || recurrence.delta == 0 ||
						recurrence.signed_overflow_guard)
					{
						return false;
					}
				}

				bool load_range = false;
				for (const RegionMemoryPlan::CountedRange& range :
					m_memory_plan.counted_ranges)
				{
					for (const RegionMemoryPlan::Access& access : range.accesses)
					{
						if (access.operation != search.load_operation)
							continue;
						if (load_range || !range.valid || access.store ||
							range.header_block != plan.header_block ||
							range.entry_induction_gpr != search.entry_induction_gpr ||
							range.entry_induction_offset !=
								search.entry_induction_offset ||
							range.induction_gpr != search.induction_gpr ||
							range.stride != search.stride ||
							access.induction_offset != search.load_offset)
						{
							return false;
						}
						load_range = true;
					}
				}
				if (!load_range)
					return false;

				auto validate_recipes = [&](const std::vector<
					SemanticKernel::StateOutput>& state,
					const std::vector<SemanticKernel::BoundedSearchOutput>& recipes) {
					for (size_t index = 0; index < state.size(); index++)
					{
						if (recipes[index].publication != state[index] ||
							recipes[index].source_gpr == 0 ||
							recipes[index].source_gpr >= 32)
						{
							return false;
						}
					}
					return true;
				};
				return validate_recipes(search.match_state,
						search.native_match_outputs) &&
					validate_recipes(search.exhausted_state,
						search.native_exhausted_outputs);
			}

			bool SelectSemanticKernel(const SemanticKernel::Plan& plan) const
			{
				// Native subclasses are selected only from the source-attested descriptor.
				// Recognition remains broader; failing this target predicate retains the
				// ordinary verified backend and never changes semantic support.
				if (plan.header_block != m_program.entry_block ||
					!plan.complete_preflight ||
					!plan.exact_event_phase || plan.requires_disjoint_write_streams ||
					plan.repeated_scaled_cycles == 0 ||
					plan.completion_scaled_cycles == 0 ||
					!m_memory_plan.control.valid ||
					m_memory_plan.control != plan.control ||
					m_memory_plan.control.maximum_iteration_scaled_cycles <
						std::max(plan.repeated_scaled_cycles,
							plan.completion_scaled_cycles) ||
					!m_reclaim_vtlb_vmap)
				{
					return false;
				}
				bool target_shape = false;
				if (plan.kind == SemanticKernel::Kind::PatternFill &&
					plan.pattern_streams.size() == 1 && plan.copy_streams.empty() &&
					plan.final_load_state.empty())
				{
					const SemanticKernel::PatternStream& stream =
						plan.pattern_streams.front();
					const s64 first_offset = static_cast<s64>(
						stream.entry_induction_offset) + stream.minimum_offset;
					target_shape = stream.entry_induction_gpr != 0 &&
						first_offset >= INT32_MIN && first_offset <= INT32_MAX;
					if (target_shape && stream.stride == 1 &&
						stream.fragments.size() == 1)
					{
						const auto& fragment = stream.fragments.front();
						target_shape = fragment.kind == MemoryAccessKind::Store8 &&
							fragment.width == 1 &&
							fragment.offset == stream.minimum_offset &&
							IsEntrySemanticValue(fragment.value);
					}
					else
					{
						// Four scalar word stores already approach the same memory
						// bandwidth as one NEON store on Cortex-A9.  The physical A9
						// inclusive gate measured this shape at only 1.44x, so it
						// remains a recognized PatternFill but is not a retained native
						// subclass.  Native selection is a profitability decision, not
						// semantic support.
						target_shape = false;
					}
				}
				else if (plan.kind == SemanticKernel::Kind::ForwardCopy &&
					plan.pattern_streams.empty() && plan.copy_streams.size() == 1 &&
					plan.final_load_state.size() <= 1 &&
					plan.requires_forward_copy_batch_alias_guard)
				{
					const SemanticKernel::CopyStream& stream = plan.copy_streams.front();
					const s64 source_offset = static_cast<s64>(
						stream.source_entry_induction_offset) +
						stream.source_minimum_offset;
					const s64 destination_offset = static_cast<s64>(
						stream.destination_entry_induction_offset) +
						stream.destination_minimum_offset;
					target_shape = stream.source_entry_induction_gpr != 0 &&
						stream.destination_entry_induction_gpr != 0 &&
						stream.stride == 1 && stream.fragments.size() == 1 &&
						source_offset >= INT32_MIN && source_offset <= INT32_MAX &&
						destination_offset >= INT32_MIN &&
						destination_offset <= INT32_MAX;
					if (target_shape)
					{
						const auto& fragment = stream.fragments.front();
						target_shape = fragment.load_kind ==
								MemoryAccessKind::LoadU8 &&
							fragment.store_kind == MemoryAccessKind::Store8 &&
							fragment.width == 1 && fragment.source_offset ==
								stream.source_minimum_offset &&
							fragment.destination_offset ==
								stream.destination_minimum_offset;
					}
					if (target_shape && !plan.final_load_state.empty())
						target_shape = plan.final_load_state.front().kind ==
							MemoryAccessKind::LoadU8;
				}
				else if (plan.kind == SemanticKernel::Kind::Vu0AffineTransform &&
					plan.pattern_streams.empty() && plan.copy_streams.empty() &&
					plan.final_load_state.empty() &&
					plan.vu0_affine_streams.size() == 1)
				{
					const SemanticKernel::Vu0AffineStream& stream =
						plan.vu0_affine_streams.front();
					const s64 source_offset = static_cast<s64>(
						stream.source_entry_induction_offset) + stream.source_offset;
					const s64 destination_offset = static_cast<s64>(
						stream.destination_entry_induction_offset) +
						stream.destination_offset;
						target_shape = stream.source_entry_induction_gpr != 0 &&
						stream.destination_entry_induction_gpr != 0 &&
						stream.source_induction_gpr != 0 &&
						stream.destination_induction_gpr != 0 &&
						stream.stride == 16 && stream.source_alignment >= 16 &&
						stream.destination_alignment >= 16 &&
						source_offset >= INT32_MIN && source_offset <= INT32_MAX &&
						destination_offset >= INT32_MIN &&
						destination_offset <= INT32_MAX &&
						stream.input_vf != 0 && stream.output_vf != 0 &&
						stream.idle_vi == 29 && stream.status_vi < 32 &&
							stream.mac_vi < 32;
				}
				else if (plan.kind == SemanticKernel::Kind::Cop1Stream &&
					m_options.allow_unretained_semantic_kernel_lowering &&
					plan.pattern_streams.empty() && plan.copy_streams.empty() &&
					plan.vu0_affine_streams.empty() &&
					plan.cop1_streams.size() == 1 &&
					!plan.iteration_blocks.empty())
				{
					// This is validation-only target support, not product admission.  The
					// first lowering retains exact guest memory order and exact scalar VFP
					// exceptional veneers.  Require each preflight range to have one
					// direction so a complete read/write alias proof can dominate every
					// write without inventing a mixed-range ordering contract.
					target_shape = plan.cop1_streams.front().
						requires_disjoint_read_write_ranges;
					auto one_direction = [](const auto& range) {
						bool reads = false;
						bool writes = false;
						for (const RegionMemoryPlan::Access& access : range.accesses)
						{
							writes |= access.store;
							reads |= !access.store;
						}
						return reads != writes;
					};
					for (const RegionMemoryPlan::CountedRange& range :
						m_memory_plan.counted_ranges)
					{
						target_shape &= range.valid && one_direction(range);
					}
					for (const RegionMemoryPlan::BoundedRange& range :
						m_memory_plan.bounded_ranges)
					{
						target_shape &= range.valid && one_direction(range) &&
							std::none_of(range.accesses.begin(), range.accesses.end(),
								[](const RegionMemoryPlan::Access& access) {
									return access.store;
								});
					}
				}
				if (!target_shape)
					return false;
				SemanticCompletion completion{};
				return FindSemanticCompletion(plan, &completion) &&
					FindExitSite(completion.site_kind, plan.latch_block) != nullptr;
			}

			bool ValidateSemanticKernelAllocation()
			{
				if (!m_emit_semantic_kernel ||
					m_semantic_plan.kind != SemanticKernel::Kind::Vu0AffineTransform)
				{
					return true;
				}
				if (m_semantic_plan.vu0_affine_streams.size() != 1 ||
					m_allocation.vfp_peak_s != 0)
				{
					return false;
				}

				std::array<bool, 8> occupied{};
				const SemanticKernel::Vu0AffineStream& stream =
					m_semantic_plan.vu0_affine_streams.front();
				for (ValueId value : stream.normalized_matrix_values)
				{
					const RegionAllocation::Location& location = Location(value);
					if (value >= m_allocation.vu0_hoisted_normalize.size() ||
						m_allocation.vu0_hoisted_normalize[value] == 0 ||
						location.kind != RegionAllocation::LocationKind::NeonQ ||
						LocationMask(location) != 0x0f ||
						location.index >= occupied.size() ||
						occupied[location.index])
					{
						return false;
					}
					occupied[location.index] = true;
				}

				u32 work = 0;
				// The semantic body needs scalar VMUL/VADD lanes, which AArch32 can
				// address only in s0-s31 (q0-q7). q7 is not an allocator location and
				// ordinarily aliases the backend's scalar scratch lanes, but this batch
				// owns and saves it for its full lifetime and calls no nested lowering
				// which uses s30-s31. The four invariant columns plus four work quads
				// therefore occupy the complete scalar-addressable vector bank exactly.
				for (u32 q = 0; q < occupied.size() &&
					work < m_semantic_affine_work_q.size(); q++)
				{
					if (!occupied[q])
						m_semantic_affine_work_q[work++] = q;
				}
				return work == m_semantic_affine_work_q.size();
			}

			bool EmitAddSignedImmediate(unsigned destination, unsigned source,
				s64 value)
			{
				if (value >= 0 && value <= UINT32_MAX &&
					m_code.EmitAddImm32(destination, source,
						static_cast<u32>(value)))
				{
					return true;
				}
				if (value < 0 && value >= -static_cast<s64>(UINT32_MAX) &&
					m_code.EmitSubImm32(destination, source,
						static_cast<u32>(-value)))
				{
					return true;
				}
				const unsigned constant = destination == TEMP2 || source == TEMP2 ?
					TEMP1 : TEMP2;
				if (constant == destination || constant == source ||
					value < INT32_MIN || value > UINT32_MAX ||
					!m_code.EmitMovImm32(constant, static_cast<u32>(value)))
				{
					return false;
				}
				return m_code.EmitAddReg(destination, source, constant);
			}

			bool EmitSemanticFinalGprState(const Transfer& completion)
			{
				const Block& header = m_program.blocks[m_semantic_plan.header_block];
				for (const SemanticKernel::Low32Recurrence& recurrence :
					m_semantic_plan.recurrences)
				{
					if (recurrence.gpr == 0 || recurrence.gpr >= header.parameters.gpr.size())
						return false;
					const ValueId target = completion.state.gpr[recurrence.gpr];
					const u8 stored_mask = LocationMask(Location(target));
					// A rematerialized sign/zero-extension may demand word 1 without
					// allocating word 0 at the exit.  Compute the low recurrence for
					// either low-half demand, then publish only the allocated words.
					if ((stored_mask & 3u) != 0)
					{
						if (!EmitSpillLoad(TEMP1, m_work_scratch_offset + 8) ||
							!m_code.EmitMovImm32(TEMP0,
								static_cast<u32>(recurrence.delta)) ||
							!m_code.EmitUmull(TEMP1, TEMP2, TEMP1, TEMP0))
						{
							return false;
						}
						const size_t seed_offset =
							RegionExecution::CanonicalStateWordOffset(
								recurrence.gpr, 0);
						// Semantic owners currently advertise only a canonical entry.
						// Read every induction seed from that simultaneous architectural
						// snapshot: the ordinary CFG allocator may legally coalesce one
						// recurrence's final value with another recurrence's entry value,
						// so sequential commits must not become sequential input reads.
						if (seed_offset == SIZE_MAX ||
							!EmitStateLoad(static_cast<u32>(seed_offset), TEMP0) ||
							!m_code.EmitAddReg(TEMP0, TEMP0, TEMP1) ||
							((stored_mask & 1u) != 0 &&
							 !CommitValueWord(target, 0, TEMP0)))
						{
							return false;
						}
					}
					if ((stored_mask & 2u) != 0)
					{
						const bool high = recurrence.extension ==
							RegionExecution::Low32Extension::Sign ?
							m_code.EmitMovRegShiftImm(TEMP1, TEMP0,
								ShiftType::ASR, 31) :
							m_code.EmitMovImm8(TEMP1, 0);
						if (!high || !CommitValueWord(target, 1, TEMP1))
							return false;
					}
					for (u8 word = 2; word < 4; word++)
					{
						if ((stored_mask & (1u << word)) == 0)
							continue;
						const size_t offset = RegionExecution::CanonicalStateWordOffset(
							recurrence.gpr, word);
						if (offset == SIZE_MAX ||
							!EmitStateLoad(static_cast<u32>(offset), TEMP0) ||
							!CommitValueWord(target, word, TEMP0))
						{
							return false;
						}
					}
				}
				return true;
			}

			bool EmitSemanticCycles()
			{
				if (!m_persistent_scheduler_countdown ||
					m_semantic_plan.repeated_scaled_cycles == 0)
				{
					return false;
				}
				if (!EmitSpillLoad(TEMP1, m_work_scratch_offset + 8) ||
					!m_code.EmitMovImm32(TEMP0,
						m_semantic_plan.repeated_scaled_cycles) ||
					!m_code.EmitUmull(TEMP1, TEMP2, TEMP1, TEMP0))
				{
					return false;
				}
				const s64 completion_adjustment = static_cast<s64>(
					m_semantic_plan.completion_scaled_cycles) -
					m_semantic_plan.repeated_scaled_cycles;
				return EmitAddSignedImmediate(TEMP1, TEMP1,
						completion_adjustment) &&
					m_code.EmitAddReg(CYCLE_LOW, CYCLE_LOW, TEMP1);
			}

			bool EmitSemanticFinalLoadState(const Transfer& completion,
				unsigned raw_value)
			{
				if (m_semantic_plan.final_load_state.empty())
					return true;
				if (m_semantic_plan.final_load_state.size() != 1)
					return false;
				const SemanticKernel::FinalLoadState& load =
					m_semantic_plan.final_load_state.front();
				if (load.gpr == 0 || load.gpr >= completion.state.gpr.size() ||
					load.kind != MemoryAccessKind::LoadU8)
				{
					return false;
				}
				const ValueId target = completion.state.gpr[load.gpr];
				const u8 stored_mask = LocationMask(Location(target));
				if ((stored_mask & 1u) != 0 &&
					!CommitValueWord(target, 0, raw_value))
				{
					return false;
				}
				if ((stored_mask & 2u) != 0 &&
					(!m_code.EmitMovImm8(TEMP1, 0) ||
					 !CommitValueWord(target, 1, TEMP1)))
				{
					return false;
				}
				for (u8 word = 2; word < 4; word++)
				{
					if ((stored_mask & (1u << word)) == 0)
						continue;
					const size_t offset = RegionExecution::CanonicalStateWordOffset(
						load.gpr, word);
					if (offset == SIZE_MAX ||
						!EmitStateLoad(static_cast<u32>(offset), TEMP1) ||
						!CommitValueWord(target, word, TEMP1))
					{
						return false;
					}
				}
				return true;
			}

			bool EmitVu0StatusMaskQuad(unsigned raw_q, unsigned status_q)
			{
				// Encode each lane's STATUS contribution directly as bits Z/S/U/O
				// (0/1/2/4/8). Keeping these masks in q12-q13 lets the hot batch OR
				// every lane, stage and iteration without four horizontal reductions
				// and ARM condition-code updates after each guest FMAC.
				return m_code.EmitVshrU32Q(status_q, raw_q, 31) &&
				       m_code.EmitVshlI32Q(status_q, status_q, 1) &&
				       m_code.EmitVandQ(VECTOR_SCRATCH1, raw_q,
						VU0_EXPONENT_Q) &&
				       m_code.EmitVceqI32ZeroQ(VU0_MASK_Q, VECTOR_SCRATCH1) &&
				       m_code.EmitVshrU32Q(VECTOR_SCRATCH0, VU0_MASK_Q, 31) &&
				       m_code.EmitVorrQ(status_q, status_q, VECTOR_SCRATCH0) &&
				       m_code.EmitVceqI32Q(VECTOR_SCRATCH0, VECTOR_SCRATCH1,
						VU0_EXPONENT_Q) &&
				       m_code.EmitVshrU32Q(VECTOR_SCRATCH0, VECTOR_SCRATCH0, 31) &&
				       m_code.EmitVshlI32Q(VECTOR_SCRATCH0, VECTOR_SCRATCH0, 3) &&
				       m_code.EmitVorrQ(status_q, status_q, VECTOR_SCRATCH0) &&
				       m_code.EmitVshlI32Q(VECTOR_SCRATCH1, raw_q, 1) &&
				       m_code.EmitVceqI32ZeroQ(VECTOR_SCRATCH1, VECTOR_SCRATCH1) &&
				       m_code.EmitVmvnQ(VECTOR_SCRATCH1, VECTOR_SCRATCH1) &&
				       m_code.EmitVandQ(VECTOR_SCRATCH1, VECTOR_SCRATCH1,
						VU0_MASK_Q) &&
				       m_code.EmitVshrU32Q(VECTOR_SCRATCH1, VECTOR_SCRATCH1, 31) &&
				       m_code.EmitVshlI32Q(VECTOR_SCRATCH1, VECTOR_SCRATCH1, 2) &&
				       m_code.EmitVorrQ(status_q, status_q, VECTOR_SCRATCH1);
			}

			bool EmitReduceVu0StatusMaskQuad(unsigned status_q, unsigned output)
			{
				if (!m_code.EmitMovImm8(output, 0))
					return false;
				for (u8 lane = 0; lane < 4; lane++)
				{
					if (!m_code.EmitVmovD32LaneToCore(TEMP0,
							status_q * 2 + lane / 2, lane & 1u) ||
						!m_code.EmitOrrReg(output, output, TEMP0))
					{
						return false;
					}
				}
				return true;
			}

			bool EmitVu0MacFromPhysicalQuad(unsigned raw_q, unsigned flags)
			{
				if (!m_code.EmitMovImm8(flags, 0) ||
					!m_code.EmitMovImm32(TEMP1, VU_FLOAT_EXPONENT))
				{
					return false;
				}
				for (u8 lane = 0; lane < 4; lane++)
				{
					const u32 shift = 3u - lane;
					if (!m_code.EmitVmovD32LaneToCore(TEMP0,
							raw_q * 2 + lane / 2, lane & 1u) ||
						!m_code.EmitTstImm32(TEMP0, VU_FLOAT_SIGN) ||
						!m_code.EmitOrrImm32(flags, flags, 0x0010u << shift,
							false, Condition::NE) ||
						!m_code.EmitBicImm32(TEMP2, TEMP0, VU_FLOAT_SIGN, true))
					{
						return false;
					}
					const size_t nonzero =
						m_code.EmitBranchPlaceholder(Condition::NE);
					if (nonzero == static_cast<size_t>(-1) ||
						!m_code.EmitOrrImm32(flags, flags, 0x0001u << shift))
					{
						return false;
					}
					const size_t done_from_zero = m_code.EmitBranchPlaceholder();
					if (done_from_zero == static_cast<size_t>(-1) ||
						!m_code.PatchBranch(nonzero, m_code.Size(), Condition::NE) ||
						!m_code.EmitAndReg(TEMP2, TEMP0, TEMP1) ||
						!m_code.EmitCmpImm32(TEMP2, 0) ||
						!m_code.EmitOrrImm32(flags, flags, 0x0100u << shift,
							false, Condition::EQ) ||
						!m_code.EmitOrrImm32(flags, flags, 0x0001u << shift,
							false, Condition::EQ) ||
						!m_code.EmitCmpReg(TEMP2, TEMP1) ||
						!m_code.EmitOrrImm32(flags, flags, 0x1000u << shift,
							false, Condition::EQ) ||
						!m_code.PatchBranch(done_from_zero, m_code.Size()))
					{
						return false;
					}
				}
				return true;
			}

			bool EmitSemanticSaveAffinePrivateState()
			{
				for (u32 index = 0; index < VU0_AFFINE_BORROWED_CORE.size(); index++)
				{
					if (!EmitSpillStore(VU0_AFFINE_BORROWED_CORE[index],
							m_work_scratch_offset + VU0_AFFINE_CORE_SAVE_OFFSET +
							index * sizeof(u32)))
					{
						return false;
					}
				}
				for (u32 index = 0; index < m_semantic_affine_work_q.size(); index++)
				{
					if (!EmitQAddress(TEMP2,
							m_work_scratch_offset + VU0_AFFINE_Q_SAVE_OFFSET +
							index * sizeof(u128)) ||
						!m_code.EmitVst1Q32(m_semantic_affine_work_q[index], TEMP2))
					{
						return false;
					}
				}
				return true;
			}

			bool EmitSemanticRestoreAffinePrivateState()
			{
				for (u32 index = 0; index < m_semantic_affine_work_q.size(); index++)
				{
					if (!EmitQAddress(TEMP2,
							m_work_scratch_offset + VU0_AFFINE_Q_SAVE_OFFSET +
							index * sizeof(u128)) ||
						!m_code.EmitVld1Q32(m_semantic_affine_work_q[index], TEMP2))
					{
						return false;
					}
				}
				for (u32 index = 0; index < VU0_AFFINE_BORROWED_CORE.size(); index++)
				{
					if (!EmitSpillLoad(VU0_AFFINE_BORROWED_CORE[index],
							m_work_scratch_offset + VU0_AFFINE_CORE_SAVE_OFFSET +
							index * sizeof(u32)))
					{
						return false;
					}
				}
				return true;
			}

			bool EmitSemanticAffineEntryGprWord(u32 gpr, unsigned destination)
			{
				if (gpr >= 32 || m_program.entry_block >= m_program.blocks.size())
					return false;
				const ValueId value =
					m_program.blocks[m_program.entry_block].parameters.gpr[gpr];
				if (value < m_program.value_count &&
					(ValueCapabilityMask(value) & 0x1u) != 0)
				{
					const RegionAllocation::Location& location = Location(value);
					if (location.kind == RegionAllocation::LocationKind::Core ||
						location.kind == RegionAllocation::LocationKind::FixedCycle)
					{
						const unsigned source = CoreRegister(location, 0);
						for (u32 index = 0;
							index < VU0_AFFINE_BORROWED_CORE.size(); index++)
						{
							if (source == VU0_AFFINE_BORROWED_CORE[index])
							{
								// The semantic body saved every borrowed register before
								// installing its private source/destination/count state.
								// Reload entry parameters from that simultaneous snapshot;
								// consulting the allocator register after installing an
								// earlier parameter would create an order-dependent parallel
								// move and can turn a guest pointer into a host pointer twice.
								return EmitSpillLoad(destination,
									m_work_scratch_offset +
									VU0_AFFINE_CORE_SAVE_OFFSET +
									index * sizeof(u32));
							}
						}
						return EmitMove(destination, source);
					}
					return EmitLoadValueWord(value, 0, destination);
				}
				const size_t offset =
					RegionExecution::CanonicalStateWordOffset(gpr, 0);
				return offset != SIZE_MAX &&
					EmitStateLoad(static_cast<u32>(offset), destination);
			}

			bool EmitSemanticCommitStagedVector(ValueId value, u32 offset)
			{
				return EmitQAddress(TEMP2, m_work_scratch_offset + offset) &&
				       m_code.EmitVld1Q32(VECTOR_SCRATCH0, TEMP2) &&
				       CommitVector(value, VECTOR_SCRATCH0);
			}

			bool EmitSemanticVu0AffineTransform(const Transfer& completion,
				const RegionExecution::ExitSite* site)
			{
				if (m_semantic_plan.vu0_affine_streams.size() != 1)
					return false;
				const SemanticKernel::Vu0AffineStream& stream =
					m_semantic_plan.vu0_affine_streams.front();
				std::array<unsigned, 4> matrix_q{};
				for (u32 lane = 0; lane < matrix_q.size(); lane++)
				{
					const RegionAllocation::Location& location =
						Location(stream.normalized_matrix_values[lane]);
					if (location.kind != RegionAllocation::LocationKind::NeonQ ||
						LocationMask(location) != 0x0f || location.index >= 8)
					{
						return false;
					}
					matrix_q[lane] = location.index;
				}

				constexpr unsigned SOURCE = VU0_AFFINE_BORROWED_CORE[0];
				constexpr unsigned DESTINATION = VU0_AFFINE_BORROWED_CORE[1];
				constexpr unsigned COUNT = VU0_AFFINE_BORROWED_CORE[2];
				constexpr unsigned STICKY_STATUS = VU0_AFFINE_BORROWED_CORE[3];
				const unsigned input_q = m_semantic_affine_work_q[0];
				const unsigned normalized_input_q = m_semantic_affine_work_q[1];
				const unsigned accumulator_q = m_semantic_affine_work_q[2];
				const unsigned raw_output_q = m_semantic_affine_work_q[3];
				const s64 source_offset = static_cast<s64>(
					stream.source_entry_induction_offset) + stream.source_offset;
				const s64 destination_offset = static_cast<s64>(
					stream.destination_entry_induction_offset) +
					stream.destination_offset;

				// EmitEntryMemoryPlanGuard has already proved the complete direct-RAM
				// ranges, trip count, signed ADDI domains, event horizon and every
				// destination source-ownership bit. The one VPU_STAT entry proof likewise
				// dominates all represented vu0Sync observers. Save the allocator ABI only
				// after every fallible entry proof has completed.
				if (!EmitSemanticSaveAffinePrivateState() ||
					!EmitSemanticAffineEntryGprWord(
						stream.source_entry_induction_gpr, SOURCE) ||
					!EmitAddSignedImmediate(SOURCE, SOURCE, source_offset) ||
					!m_code.EmitAddReg(SOURCE, VTLB_HOST_BASE, SOURCE) ||
					!EmitSemanticAffineEntryGprWord(
						stream.destination_entry_induction_gpr, DESTINATION) ||
					!EmitAddSignedImmediate(DESTINATION, DESTINATION,
						destination_offset) ||
					!m_code.EmitAddReg(DESTINATION, VTLB_HOST_BASE, DESTINATION) ||
					!EmitSpillLoad(COUNT,
						m_work_scratch_offset + SEMANTIC_TRIP_COUNT_OFFSET) ||
					!EmitStateLoad(offsetof(CanonicalState, vu0_vi) +
						stream.status_vi * sizeof(u32), STICKY_STATUS) ||
					!m_code.EmitAndImm32(STICKY_STATUS, STICKY_STATUS, 0x0fc0u) ||
					!m_code.EmitVeorQ(VU0_AFFINE_STICKY_STATUS_Q,
						VU0_AFFINE_STICKY_STATUS_Q,
						VU0_AFFINE_STICKY_STATUS_Q))
				{
					m_result.failure_emission_step = 121;
					return false;
				}

				const size_t loop = m_code.Size();
				if (!m_code.EmitVld1Q32Aligned(input_q, SOURCE) ||
					!m_code.EmitVorrQ(normalized_input_q, input_q, input_q) ||
					!EmitNormalizeVuQuad(normalized_input_q))
				{
					m_result.failure_emission_step = 122;
					return false;
				}
				for (u32 stage = 0; stage < 4; stage++)
				{
					const unsigned component_s = normalized_input_q * 4 + stage;
					for (u32 lane = 0; lane < 4; lane++)
					{
						const unsigned destination_s = raw_output_q * 4 + lane;
						if (!m_code.EmitVmulF32(destination_s,
								matrix_q[stage] * 4 + lane, component_s) ||
							(stage != 0 && !m_code.EmitVaddF32(destination_s,
								accumulator_q * 4 + lane, destination_s)))
						{
							m_result.failure_emission_step =
								static_cast<u8>(123 + stage);
							return false;
						}
					}
					if (!EmitVu0StatusMaskQuad(raw_output_q,
							VU0_AFFINE_CURRENT_STATUS_Q) ||
						!m_code.EmitVorrQ(VU0_AFFINE_STICKY_STATUS_Q,
							VU0_AFFINE_STICKY_STATUS_Q,
							VU0_AFFINE_CURRENT_STATUS_Q))
					{
						m_result.failure_emission_step =
							static_cast<u8>(127 + stage);
						return false;
					}
					if (stage < 3)
					{
						if (!m_code.EmitVorrQ(accumulator_q, raw_output_q,
								raw_output_q) ||
							!EmitNormalizeVuQuad(accumulator_q))
						{
							m_result.failure_emission_step = 131;
							return false;
						}
					}
					else if (!EmitQAddress(TEMP2, m_work_scratch_offset +
							VU0_AFFINE_FINAL_OUTPUT_OFFSET) ||
						!m_code.EmitVst1Q32(raw_output_q, TEMP2) ||
						!EmitNormalizeVuQuad(raw_output_q))
					{
						m_result.failure_emission_step = 132;
						return false;
					}
				}

				if (!m_code.EmitVst1Q32Aligned(raw_output_q, DESTINATION) ||
					!m_code.EmitAddImm8(SOURCE, SOURCE, 16) ||
					!m_code.EmitAddImm8(DESTINATION, DESTINATION, 16) ||
					!m_code.EmitSubImm8(COUNT, COUNT, 1, true))
				{
					m_result.failure_emission_step = 133;
					return false;
				}
				const size_t repeat = m_code.EmitBranchPlaceholder(Condition::NE);
				if (repeat == static_cast<size_t>(-1) ||
					!m_code.PatchBranch(repeat, loop, Condition::NE) ||
					!EmitReduceVu0StatusMaskQuad(
						VU0_AFFINE_STICKY_STATUS_Q, TEMP1) ||
					!m_code.EmitOrrRegShiftImm(STICKY_STATUS, STICKY_STATUS,
						TEMP1, ShiftType::LSL, 6) ||
					!EmitReduceVu0StatusMaskQuad(
						VU0_AFFINE_CURRENT_STATUS_Q, TEMP1) ||
					!EmitSpillStore(TEMP1,
						m_work_scratch_offset + VU0_AFFINE_STATUS_OFFSET) ||
					!m_code.EmitOrrReg(STICKY_STATUS, STICKY_STATUS, TEMP1) ||
					!EmitSpillStore(STICKY_STATUS,
						m_work_scratch_offset + VU0_AFFINE_VI_STATUS_OFFSET) ||
					!EmitQAddress(TEMP2, m_work_scratch_offset +
						VU0_AFFINE_FINAL_OUTPUT_OFFSET) ||
					!m_code.EmitVld1Q32(VECTOR_SCRATCH0, TEMP2) ||
					!EmitVu0MacFromPhysicalQuad(VECTOR_SCRATCH0, SOURCE) ||
					!EmitSpillStore(SOURCE,
						m_work_scratch_offset + VU0_AFFINE_FLAGS_OFFSET) ||
					!EmitQAddress(TEMP2, m_work_scratch_offset +
						VU0_AFFINE_FINAL_INPUT_OFFSET) ||
					!m_code.EmitVst1Q32(input_q, TEMP2) ||
					!EmitQAddress(TEMP2, m_work_scratch_offset +
						VU0_AFFINE_FINAL_OUTPUT_OFFSET) ||
					!m_code.EmitVst1Q32(raw_output_q, TEMP2) ||
					!EmitQAddress(TEMP2, m_work_scratch_offset +
						VU0_AFFINE_FINAL_ACC_OFFSET) ||
					!m_code.EmitVst1Q32(accumulator_q, TEMP2) ||
					!EmitSemanticRestoreAffinePrivateState() ||
					!EmitSemanticFinalGprState(completion) ||
					(stream.input_vf != stream.output_vf &&
						!EmitSemanticCommitStagedVector(stream.final_input,
							VU0_AFFINE_FINAL_INPUT_OFFSET)) ||
					!EmitSemanticCommitStagedVector(stream.final_output,
						VU0_AFFINE_FINAL_OUTPUT_OFFSET) ||
					!EmitSemanticCommitStagedVector(stream.final_acc,
						VU0_AFFINE_FINAL_ACC_OFFSET) ||
					!EmitSpillLoad(TEMP0,
						m_work_scratch_offset + VU0_AFFINE_FLAGS_OFFSET) ||
					!CommitValueWord(stream.final_mac, 0, TEMP0) ||
					!EmitSpillLoad(TEMP0,
						m_work_scratch_offset + VU0_AFFINE_STATUS_OFFSET) ||
					!CommitValueWord(stream.final_status, 0, TEMP0) ||
					!EmitSpillLoad(TEMP0,
						m_work_scratch_offset + VU0_AFFINE_VI_STATUS_OFFSET) ||
					!CommitValueWord(stream.final_vi_status, 0, TEMP0) ||
					!EmitSemanticCycles() ||
					!AppendColdBranch(Condition::AL, completion.external_reason, site))
				{
					m_result.failure_emission_step = 134;
					return false;
				}
				return true;
			}

			bool EmitSemanticPatternFill(const Transfer& completion,
				const RegionExecution::ExitSite* site)
			{
				const SemanticKernel::PatternStream& stream =
					m_semantic_plan.pattern_streams.front();
				const s64 first_offset = static_cast<s64>(
					stream.entry_induction_offset) + stream.minimum_offset;
				if (!EmitEntryGprWord(stream.entry_induction_gpr, 0, TEMP0) ||
					!EmitAddSignedImmediate(TEMP0, TEMP0, first_offset) ||
					!m_code.EmitAddReg(TEMP0, VTLB_HOST_BASE, TEMP0))
				{
					return false;
				}
				if (stream.stride == 1)
				{
					const auto& fragment = stream.fragments.front();
					if (!EmitLoadValueWord(fragment.value, 0, TEMP2) ||
						!m_code.EmitAndImm32(TEMP2, TEMP2, 0xffu) ||
						!m_code.EmitOrrRegShiftImm(TEMP2, TEMP2, TEMP2,
							ShiftType::LSL, 8) ||
						!m_code.EmitOrrRegShiftImm(TEMP2, TEMP2, TEMP2,
							ShiftType::LSL, 16) ||
						!m_code.EmitVdupI32QFromCore(VECTOR_SCRATCH0, TEMP2) ||
						!EmitSpillLoad(TEMP1, m_work_scratch_offset + 8))
					{
						return false;
					}
					if (!m_code.EmitCmpImm32(TEMP1, 16))
						return false;
					const size_t tail = m_code.EmitBranchPlaceholder(Condition::CC);
					if (tail == static_cast<size_t>(-1))
						return false;
					const size_t vector_loop = m_code.Size();
					if (!m_code.EmitVst1Q32(VECTOR_SCRATCH0, TEMP0) ||
						!m_code.EmitAddImm8(TEMP0, TEMP0, 16) ||
						!m_code.EmitSubImm8(TEMP1, TEMP1, 16) ||
						!m_code.EmitCmpImm32(TEMP1, 16))
					{
						return false;
					}
					const size_t repeat = m_code.EmitBranchPlaceholder(Condition::CS);
					if (repeat == static_cast<size_t>(-1) ||
						!m_code.PatchBranch(repeat, vector_loop, Condition::CS) ||
						!m_code.PatchBranch(tail, m_code.Size(), Condition::CC) ||
						!m_code.EmitCmpImm32(TEMP1, 0))
					{
						return false;
					}
					const size_t filled = m_code.EmitBranchPlaceholder(Condition::EQ);
					if (filled == static_cast<size_t>(-1) ||
						!m_code.EmitVmovD32LaneToCore(TEMP2,
							VECTOR_SCRATCH0 * 2, 0))
					{
						return false;
					}
					const size_t scalar_loop = m_code.Size();
					if (!m_code.EmitStrbImm12PostIndex(TEMP2, TEMP0, 1) ||
						!m_code.EmitSubImm8(TEMP1, TEMP1, 1, true))
					{
						return false;
					}
					const size_t scalar_repeat =
						m_code.EmitBranchPlaceholder(Condition::NE);
					if (scalar_repeat == static_cast<size_t>(-1) ||
						!m_code.PatchBranch(scalar_repeat, scalar_loop, Condition::NE) ||
						!m_code.PatchBranch(filled, m_code.Size(), Condition::EQ))
					{
						return false;
					}
				}
				else
				{
					for (u32 index = 0; index < stream.fragments.size(); index++)
					{
						if (!EmitLoadValueWord(stream.fragments[index].value, 0, TEMP2) ||
							!m_code.EmitVmovCoreToD32Lane(
								VECTOR_SCRATCH0 * 2 + index / 2,
								static_cast<u8>(index & 1u), TEMP2))
						{
							return false;
						}
					}
					if (!EmitSpillLoad(TEMP1, m_work_scratch_offset + 8))
						return false;
					const size_t loop = m_code.Size();
					if (!m_code.EmitVst1Q32(VECTOR_SCRATCH0, TEMP0) ||
						!m_code.EmitAddImm8(TEMP0, TEMP0, 16) ||
						!m_code.EmitSubImm8(TEMP1, TEMP1, 1, true))
					{
						return false;
					}
					const size_t repeat = m_code.EmitBranchPlaceholder(Condition::NE);
					if (repeat == static_cast<size_t>(-1) ||
						!m_code.PatchBranch(repeat, loop, Condition::NE))
					{
						return false;
					}
				}
				return EmitSemanticFinalGprState(completion) &&
					EmitSemanticCycles() &&
					AppendColdBranch(Condition::AL, completion.external_reason, site);
			}

			bool EmitSemanticForwardCopy(const Transfer& completion,
				const RegionExecution::ExitSite* site)
			{
				const auto& stream = m_semantic_plan.copy_streams.front();
				const s64 source_offset = static_cast<s64>(
					stream.source_entry_induction_offset) +
					stream.source_minimum_offset;
				const s64 destination_offset = static_cast<s64>(
					stream.destination_entry_induction_offset) +
					stream.destination_minimum_offset;
				if (!EmitEntryGprWord(stream.source_entry_induction_gpr, 0, TEMP0) ||
					!EmitAddSignedImmediate(TEMP0, TEMP0, source_offset) ||
					!m_code.EmitAddReg(TEMP0, VTLB_HOST_BASE, TEMP0) ||
					!EmitEntryGprWord(stream.destination_entry_induction_gpr, 0, TEMP2) ||
					!EmitAddSignedImmediate(TEMP2, TEMP2, destination_offset) ||
					!m_code.EmitAddReg(TEMP2, VTLB_HOST_BASE, TEMP2) ||
					!EmitSpillLoad(TEMP1, m_work_scratch_offset + 8) ||
					!m_code.EmitCmpReg(TEMP2, TEMP0))
				{
					return false;
				}
				const size_t safe_lower =
					m_code.EmitBranchPlaceholder(Condition::LS);
				if (safe_lower == static_cast<size_t>(-1) ||
					!m_code.EmitAddReg(TEMP1, TEMP0, TEMP1) ||
					!m_code.EmitCmpReg(TEMP2, TEMP1))
				{
					return false;
				}
				// A higher destination inside the unread source range cannot be
				// widened without changing the architecturally ordered LBU/SB stream.
				// Keep that uncommon case inside this first-class owner as one exact
				// scalar arm. Tail-entering tier zero here would rediscover this owner
				// at every backedge and pay one dispatcher fallback per byte.
				const size_t ordered_scalar =
					m_code.EmitBranchPlaceholder(Condition::CC);
				if (ordered_scalar == static_cast<size_t>(-1) ||
					!m_code.PatchBranch(safe_lower, m_code.Size(), Condition::LS) ||
					!EmitSpillLoad(TEMP1, m_work_scratch_offset + 8) ||
					!m_code.EmitCmpImm32(TEMP1, 16))
				{
					return false;
				}
				const size_t tail = m_code.EmitBranchPlaceholder(Condition::CC);
				if (tail == static_cast<size_t>(-1))
					return false;
				const size_t vector_loop = m_code.Size();
				if (!m_code.EmitVld1Q32(VECTOR_SCRATCH0, TEMP0) ||
					!m_code.EmitAddImm8(TEMP0, TEMP0, 16) ||
					!m_code.EmitVst1Q32(VECTOR_SCRATCH0, TEMP2) ||
					!m_code.EmitAddImm8(TEMP2, TEMP2, 16) ||
					!m_code.EmitSubImm8(TEMP1, TEMP1, 16) ||
					!m_code.EmitCmpImm32(TEMP1, 16))
				{
					return false;
				}
				const size_t repeat = m_code.EmitBranchPlaceholder(Condition::CS);
				if (repeat == static_cast<size_t>(-1) ||
					!m_code.PatchBranch(repeat, vector_loop, Condition::CS) ||
					!m_code.PatchBranch(tail, m_code.Size(), Condition::CC) ||
					!m_code.EmitCmpImm32(TEMP1, 0))
				{
					return false;
				}
				const size_t copied = m_code.EmitBranchPlaceholder(Condition::EQ);
				if (copied == static_cast<size_t>(-1) ||
					!EmitSpillStore(TEMP1, m_work_scratch_offset + 12))
				{
					return false;
				}
				const size_t scalar_loop = m_code.Size();
				if (!m_code.EmitLdrbImm12PostIndex(TEMP1, TEMP0, 1) ||
					!m_code.EmitStrbImm12PostIndex(TEMP1, TEMP2, 1) ||
					!EmitSpillLoad(TEMP1, m_work_scratch_offset + 12) ||
					!m_code.EmitSubImm8(TEMP1, TEMP1, 1, true) ||
					!EmitSpillStore(TEMP1, m_work_scratch_offset + 12))
				{
					return false;
				}
				const size_t scalar_repeat =
					m_code.EmitBranchPlaceholder(Condition::NE);
				if (scalar_repeat == static_cast<size_t>(-1) ||
					!m_code.PatchBranch(scalar_repeat, scalar_loop, Condition::NE) ||
					!m_code.PatchBranch(copied, m_code.Size(), Condition::EQ))
				{
					return false;
				}
				const size_t safe_complete = m_code.EmitBranchPlaceholder();
				const size_t ordered_offset = m_code.Size();
				if (safe_complete == static_cast<size_t>(-1) ||
					!m_code.PatchBranch(ordered_scalar, ordered_offset,
						Condition::CC) ||
					!EmitSpillLoad(TEMP1, m_work_scratch_offset + 8) ||
					!EmitSpillStore(TEMP1, m_work_scratch_offset + 12))
				{
					return false;
				}
				const size_t ordered_loop = m_code.Size();
				if (!m_code.EmitLdrbImm12PostIndex(TEMP1, TEMP0, 1) ||
					!m_code.EmitStrbImm12PostIndex(TEMP1, TEMP2, 1) ||
					!EmitSpillLoad(TEMP1, m_work_scratch_offset + 12) ||
					!m_code.EmitSubImm8(TEMP1, TEMP1, 1, true) ||
					!EmitSpillStore(TEMP1, m_work_scratch_offset + 12))
				{
					return false;
				}
				const size_t ordered_repeat =
					m_code.EmitBranchPlaceholder(Condition::NE);
				if (ordered_repeat == static_cast<size_t>(-1) ||
					!m_code.PatchBranch(ordered_repeat, ordered_loop, Condition::NE) ||
					!m_code.PatchBranch(safe_complete, m_code.Size()) ||
					!m_code.EmitSubImm8(TEMP0, TEMP0, 1) ||
					!m_code.EmitLdrbImm12(TEMP2, TEMP0, 0) ||
					!EmitSemanticFinalGprState(completion) ||
					!EmitSemanticFinalLoadState(completion, TEMP2) ||
					!EmitSemanticCycles() ||
					!AppendColdBranch(Condition::AL, completion.external_reason, site))
				{
					return false;
				}
				return true;
			}

			const Transfer* SemanticSearchTransfer(bool matched,
				u32* source_block, u32* target_block, u8* edge_index) const
			{
				if (!source_block || !target_block || !edge_index ||
					m_semantic_plan.bounded_equal_searches.size() != 1)
				{
					return nullptr;
				}
				const auto& search =
					m_semantic_plan.bounded_equal_searches.front();
				*source_block = matched ? search.comparison_block :
					m_semantic_plan.latch_block;
				*target_block = matched ? search.match_target_block :
					m_semantic_plan.completion_block;
				return FindSemanticPathTransfer(*source_block, *target_block,
					edge_index);
			}

			u32 SemanticSearchTargetWordCount(bool matched) const
			{
				u32 source = INVALID_BLOCK;
				u32 target = INVALID_BLOCK;
				u8 edge = 0;
				if (!SemanticSearchTransfer(matched, &source, &target, &edge) ||
					target >= m_program.blocks.size())
				{
					return 0;
				}
				u32 words = 0;
				const StateMap& state = m_program.blocks[target].parameters;
				for (size_t slot = 0;
					slot + 1 < RegionExecution::STATE_SLOT_COUNT; slot++)
				{
					const ValueId value = RegionExecution::StateValue(state, slot);
					if (value >= m_allocation.value_locations.size())
						return 0;
					words += WordCount(LocationMask(Location(value)));
				}
				return words;
			}

			const SemanticKernel::BoundedSearchOutput*
			FindSemanticSearchRecipe(bool matched, u16 state_slot) const
			{
				if (m_semantic_plan.bounded_equal_searches.size() != 1)
					return nullptr;
				const auto& search =
					m_semantic_plan.bounded_equal_searches.front();
				const auto& recipes = matched ? search.native_match_outputs :
					search.native_exhausted_outputs;
				const auto found = std::find_if(recipes.begin(), recipes.end(),
					[&](const SemanticKernel::BoundedSearchOutput& recipe) {
						return recipe.publication.state_slot == state_slot;
					});
				return found == recipes.end() ? nullptr : &*found;
			}

			const SemanticKernel::Low32Recurrence*
			FindSemanticSearchRecurrence(u8 gpr) const
			{
				const auto found = std::find_if(m_semantic_plan.recurrences.begin(),
					m_semantic_plan.recurrences.end(),
					[&](const SemanticKernel::Low32Recurrence& recurrence) {
						return recurrence.gpr == gpr;
					});
				return found == m_semantic_plan.recurrences.end() ? nullptr : &*found;
			}

			bool EmitSemanticSearchAffineLow(ValueId header_value,
				s32 recurrence_delta, s32 final_delta, unsigned destination)
			{
				if (!EmitLoadValueWord(header_value, 0, TEMP0))
					return false;
				if (recurrence_delta != 0)
				{
					if (!EmitSpillStore(TEMP0, m_work_scratch_offset +
							SEARCH_ARITHMETIC_TEMP_OFFSET) ||
						!EmitSpillLoad(TEMP2, m_work_scratch_offset +
							SEARCH_PROGRESS_OFFSET) ||
						!m_code.EmitMovImm32(TEMP1,
							static_cast<u32>(recurrence_delta)) ||
						!m_code.EmitUmull(TEMP0, TEMP2, TEMP2, TEMP1) ||
						!EmitSpillLoad(TEMP1, m_work_scratch_offset +
							SEARCH_ARITHMETIC_TEMP_OFFSET) ||
						!m_code.EmitAddReg(TEMP0, TEMP1, TEMP0))
					{
						return false;
					}
				}
				if (final_delta != 0 &&
					!EmitAddSignedImmediate(TEMP0, TEMP0, final_delta))
				{
					return false;
				}
				return EmitMove(destination, TEMP0);
			}

			bool EmitSemanticSearchTarget(bool matched)
			{
				auto fail = [&](u8 step, ValueId value = INVALID_VALUE) {
					m_result.failure_emission_step = step;
					m_result.failure_value = value;
					return false;
				};
				u32 source = INVALID_BLOCK;
				u32 target = INVALID_BLOCK;
				u8 edge = 0;
				const Transfer* const transfer = SemanticSearchTransfer(matched,
					&source, &target, &edge);
				if (!transfer || target >= m_program.blocks.size() ||
					m_semantic_plan.header_block >= m_program.blocks.size())
				{
					return fail(151);
				}
				const auto& search =
					m_semantic_plan.bounded_equal_searches.front();
				const auto& recipes = matched ? search.native_match_outputs :
					search.native_exhausted_outputs;
				const auto& states = matched ? search.match_state :
					search.exhausted_state;
				if (recipes.size() != states.size())
					return fail(152);
				for (size_t index = 0; index < recipes.size(); index++)
				{
					if (recipes[index].publication != states[index] ||
						RegionExecution::StateValue(transfer->state,
							recipes[index].publication.state_slot) !=
							recipes[index].publication.value)
					{
						return fail(153, recipes[index].publication.value);
					}
				}

				for (size_t index = 0; index < SEARCH_BORROWED_CORE.size(); index++)
				{
					if (!EmitSpillLoad(SEARCH_BORROWED_CORE[index],
						m_work_scratch_offset + SEARCH_CORE_SAVE_OFFSET +
							static_cast<u32>(index) * sizeof(u32)))
					{
						return fail(154);
					}
				}

				struct StagedWord
				{
					ValueId target = INVALID_VALUE;
					u16 state_slot = 0;
					u8 word = 0;
					u32 offset = 0;
				};
				std::vector<StagedWord> staged;
				staged.reserve(SemanticSearchTargetWordCount(matched));
				std::array<u32, RegionExecution::STATE_SLOT_COUNT> staged_low{};
				staged_low.fill(UINT32_MAX);
				const StateMap& header =
					m_program.blocks[m_semantic_plan.header_block].parameters;
				const StateMap& destination = m_program.blocks[target].parameters;
				for (size_t slot = 0;
					slot + 1 < RegionExecution::STATE_SLOT_COUNT; slot++)
				{
					const ValueId target_value =
						RegionExecution::StateValue(destination, slot);
					const ValueId header_value = RegionExecution::StateValue(header, slot);
					if (target_value >= m_allocation.value_locations.size() ||
						header_value >= m_allocation.value_locations.size())
					{
						return fail(155, target_value);
					}
					const RegionAllocation::Location& target_location =
						Location(target_value);
					const u8 target_mask = LocationMask(target_location);
					if (target_mask == 0)
						continue;
					const RegionExecution::StateSlot decoded =
						RegionExecution::DecodeStateSlot(slot);
					const SemanticKernel::BoundedSearchOutput* const recipe =
						FindSemanticSearchRecipe(matched, static_cast<u16>(slot));
					const SemanticKernel::Low32Recurrence* const recurrence =
						decoded.state_class == RegionExecution::StateClass::Gpr ?
							FindSemanticSearchRecurrence(decoded.index) : nullptr;
					if ((recipe || recurrence) &&
						decoded.state_class != RegionExecution::StateClass::Gpr)
					{
						return fail(156, target_value);
					}
					if (recipe && (recipe->source_gpr != decoded.index ||
						recipe->publication.state_slot != slot))
					{
						return fail(157, target_value);
					}
					if (target_location.kind ==
							RegionAllocation::LocationKind::CanonicalState)
					{
						// An unchanged canonical parameter already names the correct
						// entry word. A verifier-proven changed value must be staged and
						// written to that exact architectural slot before entering the
						// ordinary suffix; no broad exit materialization is required.
						if (!recipe && !recurrence)
							continue;
					}
					if (target_location.kind == RegionAllocation::LocationKind::Immediate)
					{
						// The destination SSA parameter already denotes this exact
						// compile-time value. It has no physical edge location to fill;
						// the ordinary suffix consumes the immediate and its real observer
						// exit performs any required canonical materialization.
						continue;
					}

					for (u8 word = 0; word < RegionExecution::StateWordCount(slot);
						word++)
					{
						if ((target_mask & (1u << word)) == 0)
							continue;
						const u32 output_offset = m_work_scratch_offset +
							SEARCH_OUTPUT_OFFSET +
							static_cast<u32>(staged.size()) * sizeof(u32);
						bool emitted = false;
						if (word >= 2 || (!recipe && !recurrence))
						{
							emitted = EmitLoadValueWord(header_value, word, TEMP0);
						}
						else if (recipe && recipe->kind ==
								SemanticKernel::BoundedSearchOutputKind::LoadedValue)
						{
							emitted = EmitSpillLoad(TEMP0, m_work_scratch_offset +
								(word == 0 ? SEARCH_LOADED_LOW_OFFSET :
									SEARCH_LOADED_HIGH_OFFSET));
						}
						else if (recipe && recipe->kind ==
								SemanticKernel::BoundedSearchOutputKind::FalsePredicate)
						{
							emitted = m_code.EmitMovImm8(TEMP0, 0);
						}
						else if (word == 0)
						{
							s32 recurrence_delta = recurrence ? recurrence->delta : 0;
							s32 final_delta = 0;
							if (recipe)
							{
								if (recipe->kind == SemanticKernel::
									BoundedSearchOutputKind::AffineRecurrence)
								{
									if (!recurrence || recipe->delta != recurrence->delta ||
										recipe->extension != recurrence->extension)
									{
										return fail(164, target_value);
									}
								}
								else if (recipe->kind == SemanticKernel::
									BoundedSearchOutputKind::HeaderLow32Add)
								{
									final_delta = recipe->delta;
								}
								else
								{
									return fail(165, target_value);
								}
							}
							emitted = EmitSemanticSearchAffineLow(header_value,
								recurrence_delta, final_delta, TEMP0);
							staged_low[slot] = output_offset;
						}
						else
						{
							const RegionExecution::Low32Extension extension = recipe ?
								recipe->extension : recurrence->extension;
							if (staged_low[slot] == UINT32_MAX)
								return fail(166, target_value);
							emitted = extension == RegionExecution::Low32Extension::Zero ?
								m_code.EmitMovImm8(TEMP0, 0) :
								extension == RegionExecution::Low32Extension::Sign &&
								EmitSpillLoad(TEMP0, staged_low[slot]) &&
								m_code.EmitMovRegShiftImm(TEMP0, TEMP0,
									ShiftType::ASR, 31);
						}
						if (!emitted || !EmitSpillStore(TEMP0, output_offset))
							return fail(159, target_value);
						staged.push_back({target_value, static_cast<u16>(slot), word,
							output_offset});
					}
				}

				for (const StagedWord& word : staged)
				{
					if (!EmitSpillLoad(TEMP0, word.offset))
					{
						return fail(160, word.target);
					}
					const RegionAllocation::Location& location = Location(word.target);
					if (location.kind == RegionAllocation::LocationKind::CanonicalState)
					{
						const size_t offset = RegionExecution::CanonicalStateWordOffset(
							word.state_slot, word.word);
						if (offset == SIZE_MAX ||
							!EmitStateStore(static_cast<u32>(offset), TEMP0))
						{
							return fail(161, word.target);
						}
					}
					else if (!EmitStoreLocationWord(location, word.word, TEMP0))
					{
						return fail(162, word.target);
					}
				}
				const size_t branch = m_code.EmitBranchPlaceholder();
				if (branch == static_cast<size_t>(-1))
					return fail(163);
				m_internal_patches.push_back({branch, target});
				return true;
			}

			bool EmitSemanticSearchLoad(
				const SemanticKernel::BoundedEqualSearch& search,
				unsigned pointer, unsigned loaded_high)
			{
				switch (search.load_kind)
				{
					case MemoryAccessKind::LoadS8:
						return m_code.EmitLdrsbImm8(TEMP0, pointer, 0) &&
							m_code.EmitMovRegShiftImm(loaded_high, TEMP0,
								ShiftType::ASR, 31);
					case MemoryAccessKind::LoadU8:
						return m_code.EmitLdrbImm12(TEMP0, pointer, 0) &&
							m_code.EmitMovImm8(loaded_high, 0);
					case MemoryAccessKind::LoadS16:
						return m_code.EmitLdrshImm8(TEMP0, pointer, 0) &&
							m_code.EmitMovRegShiftImm(loaded_high, TEMP0,
								ShiftType::ASR, 31);
					case MemoryAccessKind::LoadU16:
						return m_code.EmitLdrhImm8(TEMP0, pointer, 0) &&
							m_code.EmitMovImm8(loaded_high, 0);
					case MemoryAccessKind::LoadS32:
						return m_code.EmitLdrImm12(TEMP0, pointer, 0) &&
							m_code.EmitMovRegShiftImm(loaded_high, TEMP0,
								ShiftType::ASR, 31);
					case MemoryAccessKind::LoadU32:
						return m_code.EmitLdrImm12(TEMP0, pointer, 0) &&
							m_code.EmitMovImm8(loaded_high, 0);
					case MemoryAccessKind::Load64:
						return m_code.EmitLdrImm12(TEMP0, pointer, 0) &&
							m_code.EmitLdrImm12(loaded_high, pointer, 4);
					default:
						return false;
				}
			}

			bool EmitSemanticSearchCycles(bool matched)
			{
				if (!m_persistent_scheduler_countdown ||
					m_semantic_plan.repeated_scaled_cycles == 0 ||
					m_semantic_plan.completion_scaled_cycles == 0 ||
					m_semantic_plan.bounded_equal_searches.size() != 1)
				{
					return false;
				}
				const u32 final_cycles = matched ?
					m_semantic_plan.bounded_equal_searches.front().match_scaled_cycles :
					m_semantic_plan.completion_scaled_cycles;
				// EntryLoopControlGuard has already proved trip_count * the
				// maximum per-iteration charge fits the signed persistent event
				// horizon. Aggregate the exact repeated-edge debt once at the
				// selected suffix instead of updating the countdown on every
				// non-matching element.
				if (!EmitSpillLoad(TEMP0, m_work_scratch_offset +
						SEARCH_PROGRESS_OFFSET) ||
					(!matched && !m_code.EmitSubImm8(TEMP0, TEMP0, 1)) ||
					!m_code.EmitMovImm32(TEMP1,
						m_semantic_plan.repeated_scaled_cycles) ||
					!m_code.EmitUmull(TEMP0, TEMP2, TEMP0, TEMP1) ||
					!EmitAddSignedImmediate(TEMP0, TEMP0, final_cycles) ||
					!m_code.EmitAddReg(CYCLE_LOW, CYCLE_LOW, TEMP0))
				{
					return false;
				}
				return true;
			}

			bool EmitSemanticSearchIsland()
			{
				if (!m_emit_semantic_search_island ||
					m_semantic_plan.bounded_equal_searches.size() != 1 ||
					m_semantic_plan.header_block >= m_program.blocks.size())
				{
					return false;
				}
				const auto& search =
					m_semantic_plan.bounded_equal_searches.front();
				const StateMap& header =
					m_program.blocks[m_semantic_plan.header_block].parameters;
				constexpr unsigned POINTER = SEARCH_BORROWED_CORE[0];
				constexpr unsigned REMAINING = SEARCH_BORROWED_CORE[1];
				constexpr unsigned KEY_LOW = SEARCH_BORROWED_CORE[2];
				constexpr unsigned KEY_HIGH = SEARCH_BORROWED_CORE[3];
				constexpr unsigned LOADED_HIGH = SEARCH_BORROWED_CORE[4];

				for (size_t index = 0; index < SEARCH_BORROWED_CORE.size(); index++)
				{
					if (!EmitSpillStore(SEARCH_BORROWED_CORE[index],
						m_work_scratch_offset + SEARCH_CORE_SAVE_OFFSET +
							static_cast<u32>(index) * sizeof(u32)))
					{
						return false;
					}
				}
				const ValueId pointer_value = header.gpr[search.induction_gpr];
				if (!EmitLoadValueWord(pointer_value, 0, TEMP0) ||
					!EmitSpillStore(TEMP0, m_work_scratch_offset +
						SEARCH_POINTER_LOW_OFFSET) ||
					!EmitLoadValueWord(pointer_value, 1, TEMP0) ||
					!EmitSpillStore(TEMP0, m_work_scratch_offset +
						SEARCH_POINTER_HIGH_OFFSET) ||
					!EmitLoadValueWord(search.key_value, 0, TEMP0) ||
					!EmitSpillStore(TEMP0, m_work_scratch_offset +
						SEARCH_KEY_LOW_OFFSET) ||
					!EmitLoadValueWord(search.key_value, 1, TEMP0) ||
					!EmitSpillStore(TEMP0, m_work_scratch_offset +
						SEARCH_KEY_HIGH_OFFSET) ||
					!EmitSpillLoad(POINTER, m_work_scratch_offset +
						SEARCH_POINTER_LOW_OFFSET) ||
					!EmitAddSignedImmediate(POINTER, POINTER, search.load_offset) ||
					!m_code.EmitAddReg(POINTER, VTLB_HOST_BASE, POINTER) ||
					!EmitSpillLoad(REMAINING, m_work_scratch_offset +
						SEARCH_TRIP_COUNT_OFFSET) ||
					!EmitSpillLoad(KEY_LOW, m_work_scratch_offset +
						SEARCH_KEY_LOW_OFFSET) ||
					!EmitSpillLoad(KEY_HIGH, m_work_scratch_offset +
						SEARCH_KEY_HIGH_OFFSET))
				{
					return false;
				}

				const bool vector_search = search.stride == sizeof(u32) &&
					search.load_offset == 0 && search.alignment >= alignof(u32) &&
					(search.load_kind == MemoryAccessKind::LoadS32 ||
					 search.load_kind == MemoryAccessKind::LoadU32);
				const bool wide_vector_search = vector_search &&
					m_allocation.neon_peak_q == 0 && m_allocation.vfp_peak_s == 0;
				std::array<size_t, 2> scalar_entries{{
					static_cast<size_t>(-1), static_cast<size_t>(-1)}};
				size_t vector_match = static_cast<size_t>(-1);
				size_t vector_exhausted = static_cast<size_t>(-1);
				size_t wide_vector_match = static_cast<size_t>(-1);
				size_t wide_vector_exhausted = static_cast<size_t>(-1);
				if (vector_search)
				{
					if (search.load_kind == MemoryAccessKind::LoadS32)
					{
						if (!m_code.EmitMovRegShiftImm(TEMP0, KEY_LOW,
								ShiftType::ASR, 31) ||
							!m_code.EmitCmpReg(KEY_HIGH, TEMP0))
						{
							return false;
						}
					}
					else if (!m_code.EmitCmpImm32(KEY_HIGH, 0))
					{
						return false;
					}
					scalar_entries[0] =
						m_code.EmitBranchPlaceholder(Condition::NE);
					if (scalar_entries[0] == static_cast<size_t>(-1) ||
						!m_code.EmitVdupI32QFromCore(VECTOR_SCRATCH1, KEY_LOW))
					{
						return false;
					}
					if (wide_vector_search)
					{
						if (!m_code.EmitCmpImm32(REMAINING, 32))
							return false;
						const size_t wide_tail =
							m_code.EmitBranchPlaceholder(Condition::CC);
						const size_t wide_loop = m_code.Size();
						if (wide_tail == static_cast<size_t>(-1) ||
						!EmitMove(SEARCH_VECTOR_POINTER, POINTER) ||
						!m_code.EmitVeorQ(SEARCH_VECTOR_ACCUM_Q,
							SEARCH_VECTOR_ACCUM_Q, SEARCH_VECTOR_ACCUM_Q))
						{
							return false;
						}
						for (u32 vector = 0; vector < 8; vector++)
						{
							if (!m_code.EmitVld1Q32(VECTOR_SCRATCH0,
									SEARCH_VECTOR_POINTER) ||
								!m_code.EmitAddImm8(SEARCH_VECTOR_POINTER,
									SEARCH_VECTOR_POINTER, 4 * sizeof(u32)) ||
								!m_code.EmitVceqI32Q(VECTOR_SCRATCH0,
									VECTOR_SCRATCH0, VECTOR_SCRATCH1) ||
								!m_code.EmitVorrQ(SEARCH_VECTOR_ACCUM_Q,
									SEARCH_VECTOR_ACCUM_Q, VECTOR_SCRATCH0))
							{
								return false;
							}
						}
						if (!m_code.EmitVpmaxU32D(SEARCH_VECTOR_ACCUM_Q * 2,
								SEARCH_VECTOR_ACCUM_Q * 2,
								SEARCH_VECTOR_ACCUM_Q * 2 + 1) ||
							!m_code.EmitVpmaxU32D(SEARCH_VECTOR_ACCUM_Q * 2,
								SEARCH_VECTOR_ACCUM_Q * 2,
								SEARCH_VECTOR_ACCUM_Q * 2) ||
							!m_code.EmitVmovD32LaneToCore(TEMP0,
								SEARCH_VECTOR_ACCUM_Q * 2, 0) ||
							!m_code.EmitCmpImm32(TEMP0, 0))
						{
							return false;
						}
						wide_vector_match =
							m_code.EmitBranchPlaceholder(Condition::NE);
						if (wide_vector_match == static_cast<size_t>(-1) ||
							!EmitMove(POINTER, SEARCH_VECTOR_POINTER) ||
							!m_code.EmitSubImm8(REMAINING, REMAINING, 32) ||
							!m_code.EmitCmpImm32(REMAINING, 32))
						{
							return false;
						}
						const size_t wide_repeat =
							m_code.EmitBranchPlaceholder(Condition::CS);
						if (wide_repeat == static_cast<size_t>(-1) ||
							!m_code.PatchBranch(wide_repeat, wide_loop,
								Condition::CS) ||
							!m_code.PatchBranch(wide_tail, m_code.Size(),
								Condition::CC) ||
							!m_code.EmitCmpImm32(REMAINING, 0))
						{
							return false;
						}
						wide_vector_exhausted =
							m_code.EmitBranchPlaceholder(Condition::EQ);
						if (wide_vector_exhausted == static_cast<size_t>(-1))
							return false;
					}
					if (!m_code.EmitCmpImm32(REMAINING, 4))
						return false;
					scalar_entries[1] =
						m_code.EmitBranchPlaceholder(Condition::CC);
					const size_t vector_loop = m_code.Size();
					if (scalar_entries[1] == static_cast<size_t>(-1) ||
						!m_code.EmitVld1Q32(VECTOR_SCRATCH0, POINTER) ||
						!m_code.EmitVceqI32Q(VECTOR_SCRATCH0,
							VECTOR_SCRATCH0, VECTOR_SCRATCH1) ||
						!m_code.EmitVpmaxU32D(VECTOR_SCRATCH0 * 2,
							VECTOR_SCRATCH0 * 2, VECTOR_SCRATCH0 * 2 + 1) ||
						!m_code.EmitVpmaxU32D(VECTOR_SCRATCH0 * 2,
							VECTOR_SCRATCH0 * 2, VECTOR_SCRATCH0 * 2) ||
						!m_code.EmitVmovD32LaneToCore(TEMP0,
							VECTOR_SCRATCH0 * 2, 0) ||
						!m_code.EmitCmpImm32(TEMP0, 0))
					{
						return false;
					}
					vector_match = m_code.EmitBranchPlaceholder(Condition::NE);
					if (vector_match == static_cast<size_t>(-1) ||
						!m_code.EmitAddImm8(POINTER, POINTER, 4 * sizeof(u32)) ||
						!m_code.EmitSubImm8(REMAINING, REMAINING, 4) ||
						!m_code.EmitCmpImm32(REMAINING, 4))
					{
						return false;
					}
					const size_t vector_repeat =
						m_code.EmitBranchPlaceholder(Condition::CS);
					if (vector_repeat == static_cast<size_t>(-1) ||
						!m_code.PatchBranch(vector_repeat, vector_loop,
							Condition::CS) ||
						!m_code.EmitCmpImm32(REMAINING, 0))
					{
						return false;
					}
					vector_exhausted =
						m_code.EmitBranchPlaceholder(Condition::EQ);
					if (vector_exhausted == static_cast<size_t>(-1))
						return false;
				}

				const size_t loop = m_code.Size();
				for (const size_t branch : scalar_entries)
				{
					if (branch != static_cast<size_t>(-1) &&
						!m_code.PatchBranch(branch, loop,
							branch == scalar_entries[0] ? Condition::NE : Condition::CC))
					{
						return false;
					}
				}
				if (!EmitSemanticSearchLoad(search, POINTER, LOADED_HIGH) ||
					!m_code.EmitCmpReg(TEMP0, KEY_LOW))
				{
					return false;
				}
				const size_t low_mismatch =
					m_code.EmitBranchPlaceholder(Condition::NE);
				if (low_mismatch == static_cast<size_t>(-1) ||
					!m_code.EmitCmpReg(LOADED_HIGH, KEY_HIGH))
				{
					return false;
				}
				const size_t match = m_code.EmitBranchPlaceholder(Condition::EQ);
				if (match == static_cast<size_t>(-1) ||
					!m_code.PatchBranch(low_mismatch, m_code.Size(), Condition::NE))
				{
					return false;
				}
				const bool advanced = m_code.EmitAddImm32(POINTER, POINTER,
					search.stride) ||
					(m_code.EmitMovImm32(TEMP1, search.stride) &&
					 m_code.EmitAddReg(POINTER, POINTER, TEMP1));
				if (!advanced || !m_code.EmitSubImm8(REMAINING, REMAINING, 1, true))
					return false;
				const size_t exhausted =
					m_code.EmitBranchPlaceholder(Condition::EQ);
				if (exhausted == static_cast<size_t>(-1))
				{
					return false;
				}
				const size_t repeat = m_code.EmitBranchPlaceholder();
				auto fail = [&](u8 step) {
					if (m_result.failure_emission_step == 0)
						m_result.failure_emission_step = step;
					return false;
				};
				if (repeat == static_cast<size_t>(-1) ||
					!m_code.PatchBranch(repeat, loop))
					return fail(141);

				std::array<size_t, 4> vector_lane_matches{};
				vector_lane_matches.fill(static_cast<size_t>(-1));
				size_t wide_lane_match = static_cast<size_t>(-1);
				if (wide_vector_search)
				{
					if (!m_code.PatchBranch(wide_vector_match, m_code.Size(),
							Condition::NE) ||
						!EmitMove(LOADED_HIGH, KEY_HIGH))
					{
						return fail(176);
					}
					const size_t scan = m_code.Size();
					if (!m_code.EmitLdrImm12(TEMP0, POINTER, 0) ||
						!m_code.EmitCmpReg(TEMP0, KEY_LOW))
					{
						return fail(177);
					}
					wide_lane_match =
						m_code.EmitBranchPlaceholder(Condition::EQ);
					if (wide_lane_match == static_cast<size_t>(-1) ||
						!m_code.EmitAddImm8(POINTER, POINTER, sizeof(u32)) ||
						!m_code.EmitSubImm8(REMAINING, REMAINING, 1))
					{
						return fail(178);
					}
					const size_t scan_repeat = m_code.EmitBranchPlaceholder();
					if (scan_repeat == static_cast<size_t>(-1) ||
						!m_code.PatchBranch(scan_repeat, scan))
						return fail(178);
				}
				if (vector_search)
				{
					if (!m_code.PatchBranch(vector_match, m_code.Size(), Condition::NE) ||
						!EmitMove(LOADED_HIGH, KEY_HIGH))
					{
						return fail(167);
					}
					for (u32 lane = 0; lane < 4; lane++)
					{
						if (!m_code.EmitLdrImm12(TEMP0, POINTER,
								static_cast<u16>(lane * sizeof(u32))))
						{
							return fail(168);
						}
						size_t next_lane = static_cast<size_t>(-1);
						if (lane != 3)
						{
							if (!m_code.EmitCmpReg(TEMP0, KEY_LOW))
								return fail(169);
							next_lane = m_code.EmitBranchPlaceholder(Condition::NE);
							if (next_lane == static_cast<size_t>(-1))
								return fail(170);
						}
						if (lane != 0 &&
							(!m_code.EmitAddImm8(POINTER, POINTER,
								lane * sizeof(u32)) ||
							 !m_code.EmitSubImm8(REMAINING, REMAINING, lane)))
						{
							return fail(171);
						}
						vector_lane_matches[lane] =
							m_code.EmitBranchPlaceholder();
						if (vector_lane_matches[lane] == static_cast<size_t>(-1) ||
							(next_lane != static_cast<size_t>(-1) &&
							 !m_code.PatchBranch(next_lane, m_code.Size(),
								 Condition::NE)))
						{
							return fail(172);
						}
					}
					if (!m_code.PatchBranch(vector_exhausted, m_code.Size(),
							Condition::EQ) ||
						(wide_vector_exhausted != static_cast<size_t>(-1) &&
						 !m_code.PatchBranch(wide_vector_exhausted, m_code.Size(),
							 Condition::EQ)) ||
						!EmitAddSignedImmediate(TEMP1, POINTER,
							-static_cast<s32>(sizeof(u32))) ||
						!EmitSemanticSearchLoad(search, TEMP1, LOADED_HIGH))
					{
						return fail(173);
					}
				}
				if (!m_code.PatchBranch(exhausted, m_code.Size(), Condition::EQ))
					return fail(174);
				if (!EmitSpillStore(TEMP0, m_work_scratch_offset +
						SEARCH_LOADED_LOW_OFFSET) ||
					!EmitSpillStore(LOADED_HIGH, m_work_scratch_offset +
						SEARCH_LOADED_HIGH_OFFSET))
					return fail(142);
				if (!EmitSpillLoad(TEMP0, m_work_scratch_offset +
						SEARCH_TRIP_COUNT_OFFSET) ||
					!EmitSpillStore(TEMP0, m_work_scratch_offset +
						SEARCH_PROGRESS_OFFSET))
					return fail(143);
				if (!EmitSemanticSearchCycles(false))
					return fail(144);
				if (!EmitSemanticSearchTarget(false))
					return fail(145);
				if (!m_code.PatchBranch(match, m_code.Size(), Condition::EQ))
					return fail(146);
				for (const size_t branch : vector_lane_matches)
				{
					if (branch != static_cast<size_t>(-1) &&
						!m_code.PatchBranch(branch, m_code.Size()))
					{
						return fail(175);
					}
				}
				if (wide_lane_match != static_cast<size_t>(-1) &&
					!m_code.PatchBranch(wide_lane_match, m_code.Size(), Condition::EQ))
				{
					return fail(179);
				}
				if (!EmitSpillStore(TEMP0, m_work_scratch_offset +
						SEARCH_LOADED_LOW_OFFSET) ||
					!EmitSpillStore(LOADED_HIGH, m_work_scratch_offset +
						SEARCH_LOADED_HIGH_OFFSET))
					return fail(147);
				if (!EmitSpillLoad(TEMP0, m_work_scratch_offset +
						SEARCH_TRIP_COUNT_OFFSET) ||
					!m_code.EmitSubReg(TEMP0, TEMP0, REMAINING) ||
					!EmitSpillStore(TEMP0, m_work_scratch_offset +
						SEARCH_PROGRESS_OFFSET))
					return fail(148);
				if (!EmitSemanticSearchCycles(true))
					return fail(149);
				if (!EmitSemanticSearchTarget(true))
					return fail(150);
				return true;
			}

			const Transfer* FindSemanticPathTransfer(u32 source_block,
				u32 target_block, u8* edge_index) const
			{
				if (!edge_index || source_block >= m_program.blocks.size() ||
					target_block >= m_program.blocks.size())
				{
					return nullptr;
				}
				const Transfer* found = nullptr;
				u8 found_edge = 0;
				bool ambiguous = false;
				VisitInternalTransfers(m_program.blocks[source_block].terminator,
					[&](const Transfer& transfer, u8 edge) {
						if (transfer.target_block == target_block)
						{
							ambiguous |= found != nullptr;
							found = &transfer;
							found_edge = edge;
						}
						return true;
					});
				if (!found || ambiguous)
					return nullptr;
				*edge_index = found_edge;
				return found;
			}

			bool EmitSemanticCountedRangeBounds(
				const RegionMemoryPlan::CountedRange& range,
				u32 begin_offset, u32 end_offset)
			{
				if (!range.valid || range.entry_induction_gpr == 0 ||
					range.stride == 0 || (range.stride & (range.stride - 1)) != 0 ||
					range.maximum_offset_end <= range.minimum_offset)
				{
					return false;
				}
				const u8 base_gpr = range.invariant_base_gpr != 0 ?
					range.invariant_base_gpr : range.entry_induction_gpr;
				if (!EmitEntryGprWord(base_gpr, 0, TEMP0))
					return false;
				if (range.invariant_base_gpr != 0)
				{
					if (range.induction_scale_shift >= 32 ||
						!EmitEntryGprWord(range.entry_induction_gpr, 0, TEMP1) ||
						(range.induction_scale_shift != 0 &&
						 !m_code.EmitMovRegShiftImm(TEMP1, TEMP1, ShiftType::LSL,
							range.induction_scale_shift)) ||
						!m_code.EmitAddReg(TEMP0, TEMP0, TEMP1))
					{
						return false;
					}
				}
				const s64 first_offset = static_cast<s64>(
					range.entry_induction_offset) + range.minimum_offset;
				const s64 extent = static_cast<s64>(range.maximum_offset_end) -
					range.minimum_offset;
				if (first_offset < INT32_MIN || first_offset > INT32_MAX ||
					extent <= 0 || extent > UINT32_MAX ||
					!EmitAddSignedImmediate(TEMP0, TEMP0, first_offset) ||
					!EmitSpillStore(TEMP0, begin_offset) ||
					!EmitSpillLoad(TEMP1,
						m_work_scratch_offset + SEMANTIC_TRIP_COUNT_OFFSET) ||
					!m_code.EmitSubImm8(TEMP1, TEMP1, 1))
				{
					return false;
				}
				u8 shift = 0;
				for (u32 stride = range.stride; stride > 1; stride >>= 1)
					shift++;
				return (shift == 0 || m_code.EmitMovRegShiftImm(
						TEMP1, TEMP1, ShiftType::LSL, shift)) &&
					m_code.EmitAddReg(TEMP0, TEMP0, TEMP1) &&
					EmitAddSignedImmediate(TEMP0, TEMP0, extent) &&
					EmitSpillStore(TEMP0, end_offset);
			}

			bool EmitSemanticBoundedRangeBounds(
				const RegionMemoryPlan::BoundedRange& range,
				u32 begin_offset, u32 end_offset)
			{
				const s64 extent = static_cast<s64>(range.maximum_offset_end) -
					range.minimum_offset;
				if (!range.valid || extent <= 0 || extent > UINT32_MAX)
					return false;
				if (range.absolute_address)
				{
					if (range.minimum_offset < 0 ||
						range.maximum_offset_end < 0 ||
						!m_code.EmitMovImm32(TEMP0,
							static_cast<u32>(range.minimum_offset)))
					{
						return false;
					}
				}
				else if (range.base_gpr == 0 ||
					!EmitEntryGprWord(range.base_gpr, 0, TEMP0) ||
					!EmitAddSignedImmediate(TEMP0, TEMP0, range.minimum_offset))
				{
					return false;
				}
				return EmitSpillStore(TEMP0, begin_offset) &&
					EmitAddSignedImmediate(TEMP0, TEMP0, extent) &&
					EmitSpillStore(TEMP0, end_offset);
			}

			bool EmitSemanticCop1StreamAliasGuard()
			{
				if (m_semantic_plan.cop1_streams.size() != 1 ||
					!m_semantic_plan.cop1_streams.front().
						requires_disjoint_read_write_ranges)
				{
					return false;
				}
				const RegionExecution::ExitSite* const site = FindExitSite(
					RegionExecution::ExitSiteKind::EntryFallback,
					m_program.entry_block);
				if (!site)
					return false;
				bool saw_read = false;
				bool saw_write = false;
				auto stores = [](const auto& range) {
					return std::any_of(range.accesses.begin(), range.accesses.end(),
						[](const RegionMemoryPlan::Access& access) {
							return access.store;
						});
				};
				auto compare_write = [&](const RegionMemoryPlan::CountedRange& write) {
					saw_write = true;
					if (!EmitSemanticCountedRangeBounds(write,
							m_work_scratch_offset,
							m_work_scratch_offset + sizeof(u32)) ||
						!EmitSpillLoad(TEMP0, m_work_scratch_offset + 16) ||
						!EmitSpillLoad(TEMP1, m_work_scratch_offset + sizeof(u32)) ||
						!m_code.EmitCmpReg(TEMP0, TEMP1))
					{
						return false;
					}
					const size_t disjoint_below =
						m_code.EmitBranchPlaceholder(Condition::CS);
					if (disjoint_below == static_cast<size_t>(-1) ||
						!EmitSpillLoad(TEMP0, m_work_scratch_offset) ||
						!EmitSpillLoad(TEMP1, m_work_scratch_offset + 20) ||
						!m_code.EmitCmpReg(TEMP0, TEMP1) ||
						!AppendColdBranch(Condition::CC,
							ExitReason::MemoryTranslation, site) ||
						!m_code.PatchBranch(disjoint_below, m_code.Size(),
							Condition::CS))
					{
						return false;
					}
					return true;
				};
				auto compare_read = [&](const auto& read, auto&& emit_bounds) {
					saw_read = true;
					if (!emit_bounds(read, m_work_scratch_offset + 16,
							m_work_scratch_offset + 20))
					{
						return false;
					}
					for (const RegionMemoryPlan::CountedRange& write :
						m_memory_plan.counted_ranges)
					{
						if (stores(write) && !compare_write(write))
							return false;
					}
					return true;
				};
				for (const RegionMemoryPlan::CountedRange& read :
					m_memory_plan.counted_ranges)
				{
					if (!stores(read) && !compare_read(read,
							[&](const auto& range, u32 begin, u32 end) {
								return EmitSemanticCountedRangeBounds(range, begin, end);
							}))
					{
						return false;
					}
				}
				for (const RegionMemoryPlan::BoundedRange& read :
					m_memory_plan.bounded_ranges)
				{
					if (!stores(read) && !compare_read(read,
							[&](const auto& range, u32 begin, u32 end) {
								return EmitSemanticBoundedRangeBounds(range, begin, end);
							}))
					{
						return false;
					}
				}
				return saw_read && saw_write;
			}

			bool EmitSemanticCop1Stream(const Transfer& completion,
				const RegionExecution::ExitSite* site)
			{
				if (m_semantic_plan.cop1_streams.size() != 1 ||
					m_semantic_plan.iteration_blocks.empty() ||
					!EmitSemanticCop1StreamAliasGuard() ||
					!EmitSpillLoad(TEMP0,
						m_work_scratch_offset + SEMANTIC_TRIP_COUNT_OFFSET) ||
					!EmitSpillStore(TEMP0, m_work_scratch_offset + 12))
				{
					m_result.failure_emission_step = 135;
					return false;
				}

				const size_t loop = m_code.Size();
				for (size_t path = 0;
					path < m_semantic_plan.iteration_blocks.size(); path++)
				{
					const u32 block_index = m_semantic_plan.iteration_blocks[path];
					if (block_index >= m_program.blocks.size())
						return false;
					const Block& block = m_program.blocks[block_index];
					for (const Node& node : block.nodes)
					{
						if (node.opcode == Opcode::Parameter)
							continue;
						if (!EmitNode(block_index, node) ||
							!CompleteCop1GuardedColdPath(node.id))
						{
							m_result.failure_ir_opcode =
								static_cast<u16>(node.opcode);
							m_result.failure_value = node.id;
							m_result.failure_emission_step = 136;
							return false;
						}
					}
					if (path + 1 < m_semantic_plan.iteration_blocks.size())
					{
						u8 edge = 0;
						const u32 target =
							m_semantic_plan.iteration_blocks[path + 1];
						if (!FindSemanticPathTransfer(block_index, target, &edge) ||
							!EmitEdgeCopies(block_index, target, edge))
						{
							m_result.failure_emission_step = 137;
							return false;
						}
					}
				}

				if (!EmitSpillLoad(TEMP0, m_work_scratch_offset + 12) ||
					!m_code.EmitSubImm8(TEMP0, TEMP0, 1, true) ||
					!EmitSpillStore(TEMP0, m_work_scratch_offset + 12))
				{
					m_result.failure_emission_step = 138;
					return false;
				}
				const size_t complete =
					m_code.EmitBranchPlaceholder(Condition::EQ);
				u8 backedge = 0;
				if (complete == static_cast<size_t>(-1) ||
					!FindSemanticPathTransfer(m_semantic_plan.latch_block,
						m_semantic_plan.header_block, &backedge) ||
					!EmitEdgeCopies(m_semantic_plan.latch_block,
						m_semantic_plan.header_block, backedge))
				{
					m_result.failure_emission_step = 139;
					return false;
				}
				const size_t repeat = m_code.EmitBranchPlaceholder();
				if (repeat == static_cast<size_t>(-1) ||
					!m_code.PatchBranch(repeat, loop) ||
					!m_code.PatchBranch(complete, m_code.Size(), Condition::EQ) ||
					!EmitSemanticFinalGprState(completion) ||
					!EmitSemanticCycles() ||
					!AppendColdBranch(Condition::AL,
						completion.external_reason, site))
				{
					m_result.failure_emission_step = 140;
					return false;
				}
				return true;
			}

			bool EmitSemanticKernel()
			{
				if (!m_emit_semantic_kernel)
					return false;
				SemanticCompletion completion{};
				if (!FindSemanticCompletion(m_semantic_plan, &completion) ||
					!completion.transfer)
				{
					return false;
				}
				const RegionExecution::ExitSite* const site = FindExitSite(
					completion.site_kind, m_semantic_plan.latch_block);
				// EmitEntryMemoryPlanGuard has already proved every represented range,
				// direct identity mapping, and pre-write SMC ownership. The copy subclass
				// adds its independent runtime alias proof before its first write.
				const bool emitted = m_semantic_plan.kind ==
					SemanticKernel::Kind::PatternFill ?
					EmitSemanticPatternFill(*completion.transfer, site) :
					m_semantic_plan.kind == SemanticKernel::Kind::ForwardCopy ?
					EmitSemanticForwardCopy(*completion.transfer, site) :
					m_semantic_plan.kind == SemanticKernel::Kind::Vu0AffineTransform ?
						EmitSemanticVu0AffineTransform(*completion.transfer, site) :
					m_semantic_plan.kind == SemanticKernel::Kind::Cop1Stream ?
						EmitSemanticCop1Stream(*completion.transfer, site) : false;
				if (!emitted)
					return false;
				if (m_semantic_plan.kind == SemanticKernel::Kind::Vu0AffineTransform)
				{
					m_result.memory_loads = 1;
					m_result.memory_stores = 1;
				}
				else if (m_semantic_plan.kind == SemanticKernel::Kind::Cop1Stream)
				{
					m_result.memory_loads =
						m_semantic_plan.cop1_streams.front().load_operations;
					m_result.memory_stores =
						m_semantic_plan.cop1_streams.front().store_operations;
				}
				else
				{
					m_result.memory_loads = m_semantic_plan.kind ==
						SemanticKernel::Kind::ForwardCopy ? static_cast<u32>(
							m_semantic_plan.copy_streams.front().fragments.size()) : 0;
					m_result.memory_stores = m_semantic_plan.kind ==
						SemanticKernel::Kind::PatternFill ? static_cast<u32>(
							m_semantic_plan.pattern_streams.front().fragments.size()) :
							static_cast<u32>(m_semantic_plan.copy_streams.front().fragments.size());
				}
				return true;
			}

			const RegionExecution::ExitSite* FindExitSite(
				RegionExecution::ExitSiteKind kind, u32 block, u32 ordinal = 0) const
			{
				const auto found = std::find_if(m_execution.exits.begin(),
					m_execution.exits.end(), [&](const RegionExecution::ExitSite& site) {
						return site.kind == kind && site.block == block &&
						       site.ordinal == ordinal;
					});
				return found != m_execution.exits.end() ? &*found : nullptr;
			}

			bool AppendColdBranch(Condition condition, ExitReason reason,
				const RegionExecution::ExitSite* site,
				ValueId memory_address = INVALID_VALUE,
				bool mask_quad_address = false,
				u32 deferred_cycle_advance = 0)
			{
				if (!site)
					return false;
				// Aggregate timing leaves predecessor-block cycle charges resident in
				// m_block_cycle_debt until a latch or ordinary control exit.  A guarded
				// instruction, VU0 observer, or fallible memory operation can instead
				// leave from the middle of a later block.  Charge the exact block-entry
				// debt in that selected cold leaf before publishing the verifier-owned
				// pre-instruction state.  Taken/not-taken leaves are excluded because
				// EmitEdge()/the branch lowering already applies EffectiveCycleAdvance().
				const bool instruction_cold_exit =
					site->kind == RegionExecution::ExitSiteKind::Guarded ||
					site->kind == RegionExecution::ExitSiteKind::Observer ||
					site->kind == RegionExecution::ExitSiteKind::Memory;
				if (m_aggregate_cycle_edges && instruction_cold_exit)
				{
					if (site->block >= m_block_cycle_debt.size() ||
						m_block_cycle_debt[site->block] >
							UINT32_MAX - deferred_cycle_advance)
					{
						return false;
					}
					deferred_cycle_advance += static_cast<u32>(
						m_block_cycle_debt[site->block]);
				}
				const size_t branch = m_code.EmitBranchPlaceholder(condition);
				if (branch == static_cast<size_t>(-1))
					return false;
				for (ColdExit& exit : m_cold_exits)
				{
					if (exit.reason == reason && exit.site == site &&
						exit.memory_address == memory_address &&
						exit.deferred_cycle_advance == deferred_cycle_advance &&
						exit.mask_quad_address == mask_quad_address)
					{
						exit.aliases.push_back({branch, condition});
						return true;
					}
				}
				m_cold_exits.push_back({branch, condition, {}, reason, site,
					memory_address, deferred_cycle_advance, mask_quad_address});
				return true;
			}

			bool AppendPreEntryBranch(Condition condition, bool compatible,
				ExitReason reason)
			{
				const size_t branch = m_code.EmitBranchPlaceholder(condition);
				if (branch == static_cast<size_t>(-1))
					return false;
				auto fallback = std::find_if(
					m_pre_entry_fallbacks.begin(),
					m_pre_entry_fallbacks.end(),
					[&](const PreEntryFallback& candidate) {
						return candidate.compatible == compatible &&
							candidate.reason == reason;
					});
				if (fallback == m_pre_entry_fallbacks.end())
				{
					m_pre_entry_fallbacks.push_back({compatible, reason, {}});
					fallback = std::prev(m_pre_entry_fallbacks.end());
				}
				fallback->branches.push_back({branch, condition});
				return true;
			}

			bool EmitPreEntryGprWord(bool compatible, u32 gpr, u8 word,
				unsigned destination)
			{
				if (gpr >= 32 || word >= 2 ||
					m_program.entry_block >= m_program.blocks.size())
				{
					return false;
				}
				const ValueId value = m_program.blocks[
					m_program.entry_block].parameters.gpr[gpr];
				const Node* const parameter = Definition(value);
				unsigned source = 0;
				if (compatible && parameter &&
					CompatibleEntrySourceHost(*parameter, word, &source))
				{
					return EmitMove(destination, source);
				}
				if (compatible && CompatibleEntrySignature())
				{
					const GprLinkSignature& signature =
						*CompatibleEntrySignature();
					const GprLinkMapping* const mapping = std::find_if(
						std::begin(signature.mappings),
						std::begin(signature.mappings) + signature.count,
						[&](const GprLinkMapping& item) { return item.guest == gpr; });
					if (mapping != std::begin(signature.mappings) + signature.count &&
						mapping->dirty == GprLinkDirtyState::WriteBack)
					{
						// A Low32 write-back deliberately leaves canonical word one
						// stale. A guard must observe the live architectural pair or
						// reject compatible entry; mixing live low with stale high is
						// not a valid representation proof.
						return false;
					}
				}
				const size_t offset = RegionExecution::CanonicalStateWordOffset(gpr, word);
				return offset != SIZE_MAX &&
					EmitStateLoad(static_cast<u32>(offset), destination);
			}

			bool EmitPreEntryLow32Guards(bool compatible)
			{
				if (m_allocation.entry_low32_guards.empty())
					return true;
				for (const RegionAllocation::EntryLow32Guard& guard :
					m_allocation.entry_low32_guards)
				{
					if (guard.state_slot >= RegionExecution::STATE_SLOT_COUNT)
						return false;
					const RegionExecution::StateSlot slot =
						RegionExecution::DecodeStateSlot(guard.state_slot);
					if (slot.state_class != RegionExecution::StateClass::Gpr ||
						!EmitPreEntryGprWord(compatible, slot.index, 0, TEMP0) ||
						!EmitPreEntryGprWord(compatible, slot.index, 1, TEMP1))
					{
						return false;
					}
					bool expected = false;
					switch (guard.extension)
					{
						case RegionAllocation::RematerializationKind::SignExtendLow32:
							expected = m_code.EmitMovRegShiftImm(TEMP2, TEMP0,
								ShiftType::ASR, 31);
							break;
						case RegionAllocation::RematerializationKind::ZeroExtendLow32:
							expected = m_code.EmitMovImm8(TEMP2, 0);
							break;
						case RegionAllocation::RematerializationKind::None:
							return false;
					}
					if (!expected || !m_code.EmitCmpReg(TEMP1, TEMP2) ||
						!AppendPreEntryBranch(Condition::NE, compatible,
							ExitReason::EntryStateFallback))
					{
						return false;
					}
				}
				return true;
			}

			bool EmitPreEntryProfitabilityGuard(bool compatible)
			{
				if (!Persistent() || m_result.minimum_profitable_iterations <= 1 ||
					!m_memory_plan.control.valid)
				{
					return true;
				}
				const RegionMemoryPlan::LoopControl& control = m_memory_plan.control;
				u32 immediate_trip_count = 0;
				if (RegionMemoryPlan::CalculateImmediateTripCount(control,
						&immediate_trip_count))
				{
					return immediate_trip_count >=
						m_result.minimum_profitable_iterations;
				}
				const bool to_zero = control.termination ==
					RegionMemoryPlan::TerminationKind::DecrementToZero;
				const bool while_nonnegative = control.termination ==
					RegionMemoryPlan::TerminationKind::DecrementWhileNonNegative;
				const bool while_positive = control.termination ==
					RegionMemoryPlan::TerminationKind::DecrementWhilePositive;
				const bool increasing_endpoint = control.termination ==
						RegionMemoryPlan::TerminationKind::UnsignedLess ||
					control.termination ==
						RegionMemoryPlan::TerminationKind::EqualEndpoint;
				if ((!to_zero && !while_nonnegative && !while_positive &&
						!increasing_endpoint) || control.counter_gpr == 0)
				{
					return true;
				}
				if ((increasing_endpoint && control.counter_stride <= 0) ||
					(!increasing_endpoint && control.counter_stride >= 0))
				{
					return false;
				}
				const u32 step = increasing_endpoint ?
					static_cast<u32>(control.counter_stride) :
					static_cast<u32>(-static_cast<s64>(control.counter_stride));
				if (step == 0 || step > 4096 || (step & (step - 1)) != 0)
					return false;

				std::vector<std::pair<size_t, Condition>> invalid;
				auto skip_invalid = [&](Condition condition) {
					const size_t branch = m_code.EmitBranchPlaceholder(condition);
					if (branch == static_cast<size_t>(-1))
						return false;
					invalid.emplace_back(branch, condition);
					return true;
				};
				auto emit_threshold = [&](u64 threshold) {
					if (threshold > UINT32_MAX)
						return AppendPreEntryBranch(Condition::AL, compatible,
							ExitReason::ProfitabilityFallback);
					const u32 immediate = static_cast<u32>(threshold);
					const bool compared = m_code.EmitCmpImm32(TEMP0, immediate) ||
						(m_code.EmitMovImm32(TEMP2, immediate) &&
						 m_code.EmitCmpReg(TEMP0, TEMP2));
					return compared && AppendPreEntryBranch(Condition::CC,
						compatible, ExitReason::ProfitabilityFallback);
				};
				// r12 can carry the predecessor's VTLB pointer in a compatible
				// tier-zero signature. Use only dispatcher scratch r0/lr before a
				// rejected entry tail-enters that immutable owner.
				if (!EmitPreEntryGprWord(compatible, control.counter_gpr, 1, TEMP0) ||
					!m_code.EmitCmpImm32(TEMP0, 0) ||
					!skip_invalid(Condition::NE) ||
					!EmitPreEntryGprWord(compatible, control.counter_gpr, 0,
						increasing_endpoint ? TEMP2 : TEMP0))
				{
					return false;
				}

				bool emitted = false;
				if (!increasing_endpoint)
				{
					if ((while_nonnegative || while_positive) &&
						(!m_code.EmitTstImm32(TEMP0, 0x80000000u) ||
						 !skip_invalid(Condition::NE)))
					{
						return false;
					}
					if ((while_positive || to_zero) &&
						(!m_code.EmitCmpImm32(TEMP0, 0) ||
						 !skip_invalid(Condition::EQ)))
					{
						return false;
					}
					if (to_zero && step > 1 &&
						(!m_code.EmitTstImm32(TEMP0, step - 1) ||
						 !skip_invalid(Condition::NE)))
					{
						return false;
					}

					u64 minimum_entry = 0;
					if (to_zero)
					{
						minimum_entry = static_cast<u64>(
							m_result.minimum_profitable_iterations) * step;
					}
					else if (while_nonnegative)
					{
						minimum_entry = static_cast<u64>(
							m_result.minimum_profitable_iterations - 1) * step;
					}
					else
					{
						minimum_entry = static_cast<u64>(
							m_result.minimum_profitable_iterations - 1) * step + 1;
					}
					emitted = emit_threshold(minimum_entry);
				}
				else
				{
					if (control.signed_counter_compare &&
						(!m_code.EmitTstImm32(TEMP2, 0x80000000u) ||
						 !skip_invalid(Condition::NE)))
					{
						return false;
					}
					if (control.bound_is_immediate)
					{
						if (!m_code.EmitMovImm32(TEMP0, control.bound_immediate))
							return false;
					}
					else if (control.bound_gpr == 0 ||
						!EmitPreEntryGprWord(compatible, control.bound_gpr, 1, TEMP0) ||
						!m_code.EmitCmpImm32(TEMP0, 0) ||
						!skip_invalid(Condition::NE) ||
						!EmitPreEntryGprWord(compatible, control.bound_gpr, 0, TEMP0) ||
						(control.signed_counter_compare &&
							(!m_code.EmitTstImm32(TEMP0, 0x80000000u) ||
							 !skip_invalid(Condition::NE))))
					{
						return false;
					}
					if (!m_code.EmitCmpReg(TEMP2, TEMP0) ||
						!skip_invalid(Condition::CS) ||
						!m_code.EmitSubReg(TEMP0, TEMP0, TEMP2) ||
						(step > 1 &&
							(!m_code.EmitTstImm32(TEMP0, step - 1) ||
							 !skip_invalid(Condition::NE))))
					{
						return false;
					}
					if (m_result.minimum_profitable_iterations <=
						control.trip_count_adjustment)
					{
						emitted = true;
					}
					else
					{
						const u64 minimum_distance = static_cast<u64>(
							m_result.minimum_profitable_iterations -
							control.trip_count_adjustment) * step;
						emitted = emit_threshold(minimum_distance);
					}
				}
				if (!emitted)
					return false;
				const size_t valid_or_unknown = m_code.Size();
				for (const auto& [branch, condition] : invalid)
				{
					if (!m_code.PatchBranch(branch, valid_or_unknown, condition))
						return false;
				}
				return true;
			}

			bool EmitPreEntryVu0IdleGuard(bool compatible)
			{
				if (!m_pre_entry_vu0_idle_proof)
					return true;
				// VU0.cpp::vu0Sync() observes VPU_STAT before each interlocked macro
				// operation. Region IR cannot currently start VU0 or write VPU_STAT, so
				// idle is invariant for one complete generated invocation. Prove it once
				// before executing a guest instruction; failure tails to the exact tier-zero
				// entry. The verified per-instruction observer transfers remain the oracle
				// contract for non-persistent/cold execution.
				return EmitStateLoad(offsetof(CanonicalState, vu0_vi) +
					29 * sizeof(u32), TEMP0) &&
				       m_code.EmitTstImm32(TEMP0, 1) &&
				       AppendPreEntryBranch(Condition::NE, compatible,
					   ExitReason::EntryStateFallback);
			}

			bool EmitEventComparison()
			{
				// EmitAddCycles() has just set N from the resident signed countdown.
				// Do not insert even a CMP on the hot loop edge.
				if (m_persistent_scheduler_countdown)
					return true;
				return EmitContextLoad(TEMP0,
						offsetof(ExecutionContext, next_event_cycle_low)) &&
				       EmitContextLoad(TEMP1,
						   offsetof(ExecutionContext, next_event_cycle_high)) &&
				       m_code.EmitSubReg(TEMP0, CYCLE_LOW, TEMP0, true) &&
				       m_code.EmitSbcReg(TEMP1, CYCLE_HIGH, TEMP1, true);
			}

			bool EmitEntryEventCheck()
			{
				if (m_persistent_scheduler_countdown)
				{
					const RegionExecution::ExitSite* const site =
						FindExitSite(RegionExecution::ExitSiteKind::EntryEvent,
							m_program.entry_block);
					if (m_canonical_entry_event_patch != static_cast<size_t>(-1))
					{
						// The compatible prefix already moved the exact signed
						// countdown from tier zero's r6 into CYCLE_LOW. Test that
						// representation directly, then skip the canonical
						// cycle-to-countdown conversion. The canonical path retains its
						// own event check below. Both failures share one cold exit.
						if (!m_code.EmitCmpImm32(CYCLE_LOW, 0) ||
							!AppendColdBranch(Condition::PL,
								ExitReason::EventHorizon, site))
						{
							return false;
						}
						const size_t compatible_to_common =
							m_code.EmitBranchPlaceholder();
						if (compatible_to_common == static_cast<size_t>(-1) ||
							!m_code.PatchBranch(
								m_canonical_entry_event_patch, m_code.Size()))
						{
							return false;
						}
						m_canonical_entry_event_patch = static_cast<size_t>(-1);
						if (!EmitContextLoad(TEMP0,
								offsetof(ExecutionContext, next_event_cycle_low)) ||
							!m_code.EmitSubReg(CYCLE_LOW, CYCLE_LOW, TEMP0, true) ||
							!AppendColdBranch(Condition::PL,
								ExitReason::EventHorizon, site))
						{
							return false;
						}
						return m_code.PatchBranch(
							compatible_to_common, m_code.Size());
					}
					// The resident value is the exact signed distance from the immutable
					// scheduler horizon. No entry-cycle snapshot is needed: a cold exit
					// reconstructs the full 64-bit cycle as nextEventCycle + countdown.
					// This is the same low-word horizon contract as the existing
					// persistent A32 block dispatcher, without three stack stores per
					// first-class region entry.
					if (!EmitContextLoad(TEMP0,
							offsetof(ExecutionContext, next_event_cycle_low)) ||
						!m_code.EmitSubReg(CYCLE_LOW, CYCLE_LOW, TEMP0, true))
					{
						return false;
					}
					return AppendColdBranch(Condition::PL,
						ExitReason::EventHorizon, site);
				}
				return EmitEventComparison() && AppendColdBranch(Condition::CS,
					ExitReason::EventHorizon,
					FindExitSite(RegionExecution::ExitSiteKind::EntryEvent,
						m_program.entry_block));
			}

			bool EmitEntryIterationBudgetGuard()
			{
				const RegionMemoryPlan::IterationTiming& timing =
					m_memory_plan.timing;
				// Counted streams prove their complete trip count in the memory-plan
				// guard. Callable validation retains the existing full-cycle edge checks;
				// this certificate targets the product persistent-dispatch ABI.
				if (!Persistent() || m_memory_plan.control.valid ||
					!timing.valid)
				{
					return true;
				}
				if (!m_memory_plan.header_seeds_match_entry ||
					timing.maximum_scaled_cycles == 0 ||
					timing.backedge_source_block >= m_program.blocks.size() ||
					!m_persistent_scheduler_countdown)
				{
					return false;
				}
				// Keep the maximum next-iteration charge folded into the resident
				// countdown.  The old representation left CYCLE_LOW unbiased and
				// rebuilt CYCLE_LOW + maximum_scaled_cycles in TEMP0 on every latch.
				// This representation has the same exact predicate, but the latch's
				// ordinary cycle ADD now produces the flags for the next-iteration
				// decision directly.  Cold exits remove the bias before publishing or
				// tail-chaining the scheduler countdown.
				const bool compared = m_code.EmitAddImm32(CYCLE_LOW, CYCLE_LOW,
					timing.maximum_scaled_cycles, true) ||
					(m_code.EmitMovImm32(TEMP0, timing.maximum_scaled_cycles) &&
					 m_code.EmitAddReg(CYCLE_LOW, CYCLE_LOW, TEMP0, true));
				if (!compared)
					return false;
				return AppendColdBranch(Condition::PL,
					ExitReason::EventBudgetFallback,
					FindExitSite(RegionExecution::ExitSiteKind::EntryFallback,
						m_program.entry_block));
			}

			bool EmitNextIterationBudgetGuard(u32 block_index,
				const RegionExecution::ExitSite* site, bool cycle_flags_valid)
			{
				const RegionMemoryPlan::IterationTiming& timing =
					m_memory_plan.timing;
				if (!Persistent() || m_memory_plan.control.valid ||
					!timing.valid || block_index != timing.backedge_source_block ||
					timing.maximum_scaled_cycles == 0 ||
					!m_persistent_scheduler_countdown)
				{
					return false;
				}
				// EmitEntryIterationBudgetGuard() already biased CYCLE_LOW by this
				// maximum. EmitAddCycles() normally leaves the exact signed predicate
				// in APSR; a zero-cost edge is unusual but remains exact with one CMP.
				if (!cycle_flags_valid && !m_code.EmitCmpImm32(CYCLE_LOW, 0))
					return false;
				return AppendColdBranch(Condition::PL,
					ExitReason::EventBudgetFallback, site);
			}

			bool EmitEntryMemoryPlanGuard()
			{
				if (m_memory_plan.control.valid &&
					!EmitEntryLoopControlGuard(m_memory_plan.control))
					return false;
				for (const RegionMemoryPlan::CountedRange& range :
					m_memory_plan.counted_ranges)
				{
					if (!EmitEntryMemoryPointerRangeGuard(range))
						return false;
				}
				for (const RegionMemoryPlan::BoundedRange& range :
					m_memory_plan.bounded_ranges)
				{
					if (!EmitEntryMemoryBoundedRangeGuard(range))
						return false;
				}
				if (Persistent() && m_memory_plan.control.valid &&
					m_options.persistent_dispatch->counted_iteration_counter)
				{
					// Every fallible control, event, translation, alignment and SMC
					// branch above skips this diagnostic update. Thus it counts only a
					// fully admitted exact trip count, and never attributes a cold
					// fallback as region work. This pointer is null outside profiler
					// builds, so normal generated code is byte-identical.
					if (!EmitSpillLoad(TEMP2, m_work_scratch_offset + 8) ||
						!m_code.EmitMovImm32(TEMP0, static_cast<u32>(
							reinterpret_cast<uptr>(m_options.persistent_dispatch->
								counted_iteration_counter))) ||
						!m_code.EmitLdrImm12(TEMP1, TEMP0, 0) ||
						!m_code.EmitAddReg(TEMP1, TEMP1, TEMP2) ||
						!m_code.EmitStrImm12(TEMP1, TEMP0, 0))
					{
						return false;
					}
				}
				return true;
			}

			bool EmitEntryLoopControlGuard(
				const RegionMemoryPlan::LoopControl& control)
			{
				const bool decrement_termination =
					control.termination ==
						RegionMemoryPlan::TerminationKind::DecrementToZero ||
					control.termination ==
						RegionMemoryPlan::TerminationKind::DecrementWhileNonNegative ||
					control.termination ==
						RegionMemoryPlan::TerminationKind::DecrementWhilePositive;
				if (!control.valid || !m_memory_plan.header_seeds_match_entry ||
					control.header_block != m_memory_plan.timing.header_block ||
					control.counter_gpr == 0 ||
					(!decrement_termination &&
						(!control.bound_is_immediate && control.bound_gpr == 0)) ||
					control.counter_stride == 0 ||
					control.maximum_iteration_scaled_cycles == 0)
				{
					return false;
				}
				const RegionExecution::ExitSite* site = FindExitSite(
					RegionExecution::ExitSiteKind::EntryFallback,
					m_program.entry_block);
				// Entry fallback diagnostics retain the first covered memory address,
				// matching the exact per-access path. The loop counter is only proof
				// machinery and is not an observed memory address.
				const ValueId published_control =
					m_memory_plan.counted_ranges.empty() ? INVALID_VALUE :
					m_program.blocks[m_program.entry_block].parameters.gpr[
						m_memory_plan.counted_ranges.front().induction_gpr];
				const ExitReason invalid_proof_reason =
					m_memory_plan.counted_ranges.empty() ?
						ExitReason::EventBudgetFallback :
						ExitReason::MemoryTranslation;
				auto fallback = [&](Condition condition, ExitReason reason) {
					return AppendColdBranch(condition, reason, site,
						published_control, false);
				};
				auto event_fallback = [&](Condition condition) {
					return AppendColdBranch(condition,
						ExitReason::EventBudgetFallback, site);
				};
				auto load_gpr_word = [&](u32 gpr, u8 word, unsigned destination) {
					return EmitEntryGprWord(gpr, word, destination);
				};
				u32 immediate_trip_count = 0;
				const bool immediate_trip =
					RegionMemoryPlan::CalculateImmediateTripCount(control,
						&immediate_trip_count);

				if (immediate_trip)
				{
					if (!m_code.EmitMovImm32(TEMP2, immediate_trip_count))
						return false;
				}
				else if (decrement_termination)
				{
					const bool while_nonnegative =
						control.termination == RegionMemoryPlan::TerminationKind::
							DecrementWhileNonNegative;
					const bool while_positive =
						control.termination == RegionMemoryPlan::TerminationKind::
							DecrementWhilePositive;
					if (control.counter_stride >= 0)
						return false;
					const u32 step = static_cast<u32>(
						-static_cast<s64>(control.counter_stride));
					if (step == 0 || step > 4096 || (step & (step - 1)) != 0)
						return false;
					u8 shift = 0;
					for (u32 value = step; value > 1; value >>= 1)
						shift++;
					if (!load_gpr_word(control.counter_gpr, 0, TEMP0) ||
						!load_gpr_word(control.counter_gpr, 1, TEMP1) ||
						!m_code.EmitCmpImm32(TEMP1, 0) ||
						!fallback(Condition::NE, invalid_proof_reason) ||
						((while_nonnegative || while_positive) &&
							(!m_code.EmitTstImm32(TEMP0, 0x80000000u) ||
							 !fallback(Condition::NE, invalid_proof_reason))) ||
						(while_positive &&
							(!m_code.EmitCmpImm32(TEMP0, 0) ||
							 !fallback(Condition::EQ, invalid_proof_reason))) ||
						(control.termination ==
							RegionMemoryPlan::TerminationKind::DecrementToZero && step > 1 &&
							(!m_code.EmitTstImm32(TEMP0, step - 1) ||
							 !fallback(Condition::NE,
								 invalid_proof_reason))) ||
						(while_positive && step > 1 &&
							(!m_code.EmitAddImm32(TEMP0, TEMP0, step - 1, true) ||
							 !fallback(Condition::CS, invalid_proof_reason))) ||
						(shift != 0 &&
							!m_code.EmitMovRegShiftImm(TEMP0, TEMP0,
								ShiftType::LSR, shift)) ||
						!EmitMove(TEMP2, TEMP0) ||
						(while_nonnegative &&
							(!m_code.EmitAddImm8(TEMP2, TEMP2, 1, true) ||
							 !fallback(Condition::CS, invalid_proof_reason))))
					{
						return false;
					}
				}
				else
				{
					const u32 step = static_cast<u32>(control.counter_stride);
					if (control.counter_stride <= 0 || step > 4096 ||
						(step & (step - 1)) != 0)
					{
						return false;
					}
					u8 shift = 0;
					for (u32 value = step; value > 1; value >>= 1)
						shift++;
					if (!load_gpr_word(control.counter_gpr, 0, TEMP2) ||
						!load_gpr_word(control.counter_gpr, 1, TEMP1) ||
						!m_code.EmitCmpImm32(TEMP1, 0) ||
						!fallback(Condition::NE, invalid_proof_reason) ||
						(control.signed_counter_compare &&
							(!m_code.EmitTstImm32(TEMP2, 0x80000000u) ||
							 !fallback(Condition::NE,
								 invalid_proof_reason))) ||
						(control.bound_is_immediate ?
							!m_code.EmitMovImm32(TEMP0, control.bound_immediate) :
							(!load_gpr_word(control.bound_gpr, 0, TEMP0) ||
							 !load_gpr_word(control.bound_gpr, 1, TEMP1) ||
							 !m_code.EmitCmpImm32(TEMP1, 0) ||
							 !fallback(Condition::NE, invalid_proof_reason) ||
							 (control.signed_counter_compare &&
								(!m_code.EmitTstImm32(TEMP0, 0x80000000u) ||
								 !fallback(Condition::NE,
									 invalid_proof_reason))))) ||
						!m_code.EmitCmpReg(TEMP2, TEMP0) ||
						!fallback(Condition::CS, invalid_proof_reason) ||
						!m_code.EmitSubReg(TEMP2, TEMP0, TEMP2) ||
						(step > 1 &&
							(!m_code.EmitTstImm32(TEMP2, step - 1) ||
							 !fallback(Condition::NE,
								 invalid_proof_reason))) ||
						(shift != 0 &&
							!m_code.EmitMovRegShiftImm(TEMP2, TEMP2,
								ShiftType::LSR, shift)) ||
						(control.trip_count_adjustment != 0 &&
							(!m_code.EmitAddImm8(TEMP2, TEMP2,
								control.trip_count_adjustment, true) ||
							 !fallback(Condition::CS,
								 invalid_proof_reason))))
					{
						return false;
					}
				}

				// The loop count is a shared entry invariant. Preserve it in the work
				// frame while each pointer family reuses the three scratch registers.
				if (!m_code.EmitCmpImm32(TEMP2, 0) ||
					!fallback(Condition::EQ, invalid_proof_reason) ||
					(m_result.minimum_profitable_iterations > 1 &&
						(!m_code.EmitCmpImm32(TEMP2,
							m_result.minimum_profitable_iterations) ||
						 !fallback(Condition::CC,
							 ExitReason::ProfitabilityFallback))) ||
					!EmitSpillStore(TEMP2, m_work_scratch_offset + 8) ||
					!m_code.EmitMovImm32(TEMP1,
						control.maximum_iteration_scaled_cycles) ||
					!m_code.EmitUmull(TEMP0, TEMP2, TEMP2, TEMP1))
				{
					return false;
				}
				if (m_persistent_scheduler_countdown)
				{
					// EntryEventCheck proved countdown < 0. The aggregate nonnegative
					// charge is representable in this signed horizon only when UMULL has
					// no high word or sign bit and the final countdown remains negative.
					// Leave CYCLE_LOW unchanged; the body adds each exact block charge.
					return m_code.EmitCmpImm32(TEMP2, 0) &&
						event_fallback(Condition::NE) &&
						m_code.EmitTstImm32(TEMP0, 0x80000000u) &&
						event_fallback(Condition::NE) &&
						m_code.EmitAddReg(TEMP0, CYCLE_LOW, TEMP0, true) &&
						event_fallback(Condition::PL);
				}
				if (!m_code.EmitAddReg(TEMP0, TEMP0, CYCLE_LOW, true) ||
					!m_code.EmitAdcReg(TEMP2, TEMP2, CYCLE_HIGH, true) ||
					!event_fallback(Condition::CS) ||
					!EmitContextLoad(TEMP1,
						offsetof(ExecutionContext, next_event_cycle_high)) ||
					!m_code.EmitCmpReg(TEMP2, TEMP1) ||
					!event_fallback(Condition::HI))
				{
					return false;
				}
				const size_t before_low =
					m_code.EmitBranchPlaceholder(Condition::CC);
				return before_low != static_cast<size_t>(-1) &&
				       EmitContextLoad(TEMP1,
					   offsetof(ExecutionContext, next_event_cycle_low)) &&
				       m_code.EmitCmpReg(TEMP0, TEMP1) &&
				       event_fallback(Condition::CS) &&
				       m_code.PatchBranch(before_low, m_code.Size(), Condition::CC);
			}

			bool EmitEntryMemoryPointerRangeGuard(
				const RegionMemoryPlan::CountedRange& range)
			{
				const u32 guard_header = m_memory_plan.timing.valid ?
					m_memory_plan.timing.header_block : m_program.entry_block;
				if (!range.valid || !m_memory_plan.header_seeds_match_entry ||
					range.header_block != guard_header ||
					range.entry_induction_gpr == 0 ||
					range.induction_gpr == 0 || range.stride == 0 ||
					(range.stride & (range.stride - 1)) != 0 ||
					range.alignment == 0 || range.minimum_offset < 0 ||
					range.maximum_offset_end <= range.minimum_offset)
				{
					return false;
				}
				if (range.invariant_base_gpr != 0 &&
					(range.invariant_base_gpr == range.induction_gpr ||
					 range.induction_scale_shift >= 32))
				{
					return false;
				}
				const u8 address_base = range.invariant_base_gpr != 0 ?
					range.invariant_base_gpr : range.entry_induction_gpr;
				const u8 additive = range.invariant_base_gpr != 0 ?
					range.entry_induction_gpr : 0;
				const s64 total_minimum = static_cast<s64>(range.minimum_offset) +
					static_cast<s64>(range.entry_induction_offset);
				const s64 total_maximum =
					static_cast<s64>(range.maximum_offset_end) +
					static_cast<s64>(range.entry_induction_offset);
				if (total_minimum < INT32_MIN || total_minimum > INT32_MAX ||
					total_maximum < INT32_MIN || total_maximum > INT32_MAX ||
					(static_cast<u32>(total_minimum) & (range.alignment - 1)) != 0)
				{
					return false;
				}
				return EmitEntryMemoryAddressRangeGuard(address_base, additive,
					range.induction_scale_shift,
					range.alignment, static_cast<s32>(total_minimum),
					static_cast<s32>(total_maximum), range.accesses, range.stride);
			}

			bool EmitEntryMemoryBoundedRangeGuard(
				const RegionMemoryPlan::BoundedRange& range)
			{
				const u32 guard_header = m_memory_plan.timing.valid ?
					m_memory_plan.timing.header_block : m_program.entry_block;
				if (!range.valid || !m_memory_plan.header_seeds_match_entry ||
					range.header_block != guard_header ||
					range.alignment == 0 ||
					range.maximum_offset_end <= range.minimum_offset)
				{
					return false;
				}
				if (range.absolute_address)
				{
					// Run() retained this proof only for a complete persistent
					// identity window and already checked its bounds/alignment. No
					// architectural input exists to guard at runtime.
					return Persistent() && range.base_gpr == 0 &&
						std::none_of(range.accesses.begin(), range.accesses.end(),
							[](const RegionMemoryPlan::Access& access) {
								return access.store;
							});
				}
				if (range.base_gpr == 0)
					return false;
				return EmitEntryMemoryAddressRangeGuard(range.base_gpr, 0, 0,
					range.alignment, range.minimum_offset,
					range.maximum_offset_end, range.accesses, 0);
			}

			bool EmitEntryMemoryAddressRangeGuard(u8 base_gpr, u8 additive_gpr,
				u8 additive_shift, u32 alignment,
				s32 minimum_offset, s32 maximum_offset_end,
				const std::vector<RegionMemoryPlan::Access>& accesses,
				u32 iteration_stride)
			{
				if (base_gpr == 0 || additive_shift >= 32 ||
					(additive_gpr != 0 && additive_gpr == base_gpr) || alignment == 0 ||
					maximum_offset_end <= minimum_offset || accesses.empty() ||
					(iteration_stride != 0 &&
					 ((iteration_stride & (iteration_stride - 1)) != 0)))
				{
					return false;
				}
				const RegionExecution::ExitSite* site = FindExitSite(
					RegionExecution::ExitSiteKind::EntryFallback,
					m_program.entry_block);
				const ValueId published_address = m_program.blocks[
					m_program.entry_block].parameters.gpr[base_gpr];
				auto fallback = [&](Condition condition, ExitReason reason) {
					return AppendColdBranch(condition, reason, site,
						published_address, false);
				};
				auto emit_minimum_offset = [&](unsigned value) {
					if (minimum_offset == 0)
						return true;
					if (minimum_offset > 0)
					{
						return m_code.EmitAddImm32(value, value,
							static_cast<u32>(minimum_offset), true) &&
							fallback(Condition::CS,
								ExitReason::MemoryTranslation);
					}
					const u32 magnitude = static_cast<u32>(
						-static_cast<s64>(minimum_offset));
					return m_code.EmitSubImm32(value, value, magnitude, true) &&
						fallback(Condition::CC, ExitReason::MemoryTranslation);
				};
				auto load_gpr_word = [&](u32 gpr, u8 word, unsigned destination) {
					return EmitEntryGprWord(gpr, word, destination);
				};
				auto emit_address_base = [&](unsigned destination,
					unsigned additive_value, unsigned high_word) {
					if (!load_gpr_word(base_gpr, 0, destination) ||
						!load_gpr_word(base_gpr, 1, high_word) ||
						!m_code.EmitCmpImm32(high_word, 0) ||
						!fallback(Condition::NE, ExitReason::MemoryTranslation))
					{
						return false;
					}
					if (additive_gpr != 0)
					{
						if (!load_gpr_word(additive_gpr, 0, additive_value) ||
							!load_gpr_word(additive_gpr, 1, high_word) ||
							!m_code.EmitCmpImm32(high_word, 0) ||
							!fallback(Condition::NE, ExitReason::MemoryTranslation) ||
							(additive_shift != 0 &&
								(!m_code.EmitMovRegShiftImm(high_word, additive_value,
									ShiftType::LSR, 32 - additive_shift) ||
								 !m_code.EmitCmpImm32(high_word, 0) ||
								 !fallback(Condition::NE,
									 ExitReason::MemoryTranslation) ||
								 !m_code.EmitMovRegShiftImm(additive_value,
									 additive_value, ShiftType::LSL, additive_shift))) ||
							!m_code.EmitAddReg(destination, destination,
								additive_value, true) ||
							!fallback(Condition::CS, ExitReason::MemoryTranslation))
						{
							return false;
						}
					}
					return alignment <= 1 ||
						(m_code.EmitTstImm32(destination, alignment - 1) &&
						 fallback(Condition::NE, ExitReason::MemoryAlignment));
				};
				u8 pointer_shift = 0;
				for (u32 stride = iteration_stride; stride > 1; stride >>= 1)
					pointer_shift++;
				const bool stores = std::any_of(accesses.begin(),
					accesses.end(), [](const RegionMemoryPlan::Access& access) {
						return access.store;
					});
				const bool complete_persistent_identity = Persistent() &&
					m_options.persistent_dispatch->identity_main_ram_limit != 0 &&
					m_options.persistent_dispatch->identity_main_ram_limit ==
						m_options.persistent_dispatch->main_ram_limit;

				// A read-only range inside the dispatcher's complete identity window
				// needs no later ownership walk and no range value after this guard.
				// Keep its start and end in the three lowering scratch registers rather
				// than round-tripping both through the region frame. The same overflow,
				// alignment and end-exclusive RAM proof remains in force.
				if (!stores && complete_persistent_identity)
				{
					if (!emit_address_base(TEMP1, TEMP2, TEMP0) ||
						!emit_minimum_offset(TEMP1) ||
						(iteration_stride != 0 &&
							(!EmitSpillLoad(TEMP2, m_work_scratch_offset + 8) ||
							 !m_code.EmitSubImm8(TEMP2, TEMP2, 1) ||
							 (pointer_shift != 0 &&
								(!m_code.EmitMovRegShiftImm(TEMP0, TEMP2,
									ShiftType::LSR, 32 - pointer_shift) ||
								 !m_code.EmitCmpImm32(TEMP0, 0) ||
								 !fallback(Condition::NE,
									 ExitReason::MemoryTranslation))) ||
							 (pointer_shift != 0 ?
								!m_code.EmitMovRegShiftImm(TEMP0, TEMP2,
									ShiftType::LSL, pointer_shift) :
								!EmitMove(TEMP0, TEMP2)))) ||
						(iteration_stride == 0 &&
							!m_code.EmitMovImm32(TEMP0, 0)) ||
						!m_code.EmitAddReg(TEMP0, TEMP1, TEMP0, true) ||
						!fallback(Condition::CS, ExitReason::MemoryTranslation) ||
						!m_code.EmitAddImm32(TEMP0, TEMP0,
							static_cast<u32>(maximum_offset_end -
								minimum_offset), true) ||
						!fallback(Condition::CS, ExitReason::MemoryTranslation) ||
						!EmitContextLoad(TEMP2,
							offsetof(ExecutionContext, main_ram_limit)) ||
						!m_code.EmitCmpReg(TEMP0, TEMP2) ||
						!fallback(Condition::HI, ExitReason::MemoryTranslation))
					{
						return false;
					}
					return true;
				}

				// Derive [start,end) from this pointer and the single shared trip
				// count. All arithmetic is checked before any guest store occurs.
				if (!emit_address_base(TEMP0, TEMP2, TEMP1) ||
					!emit_minimum_offset(TEMP0) ||
					!EmitSpillStore(TEMP0, m_work_scratch_offset) ||
					(iteration_stride != 0 &&
						(!EmitSpillLoad(TEMP2, m_work_scratch_offset + 8) ||
						 !m_code.EmitSubImm8(TEMP2, TEMP2, 1) ||
						 (pointer_shift != 0 &&
							(!m_code.EmitMovRegShiftImm(TEMP0, TEMP2,
								ShiftType::LSR, 32 - pointer_shift) ||
							 !m_code.EmitCmpImm32(TEMP0, 0) ||
							 !fallback(Condition::NE,
								 ExitReason::MemoryTranslation))) ||
						 (pointer_shift != 0 ?
							!m_code.EmitMovRegShiftImm(TEMP0, TEMP2,
								ShiftType::LSL, pointer_shift) :
							!EmitMove(TEMP0, TEMP2)))) ||
					(iteration_stride == 0 &&
						!m_code.EmitMovImm32(TEMP0, 0)) ||
					!EmitSpillLoad(TEMP1, m_work_scratch_offset) ||
					!m_code.EmitAddReg(TEMP0, TEMP1, TEMP0, true) ||
					!fallback(Condition::CS, ExitReason::MemoryTranslation) ||
					!m_code.EmitAddImm32(TEMP0, TEMP0,
						static_cast<u32>(maximum_offset_end -
							minimum_offset), true) ||
					!fallback(Condition::CS, ExitReason::MemoryTranslation) ||
					!EmitSpillStore(TEMP0, m_work_scratch_offset + 4) ||
					!EmitContextLoad(TEMP2,
						offsetof(ExecutionContext, main_ram_limit)) ||
					!m_code.EmitCmpReg(TEMP0, TEMP2) ||
					!fallback(Condition::HI, ExitReason::MemoryTranslation))
				{
					return false;
				}

				// Persistent compilation snapshots the same default low-main-RAM
				// identity proof already used by the per-access lowering. When it covers
				// the complete bounded RAM window, the page-walk arm is unreachable for
				// this executable generation: do not place it in the Cortex-A9 hot slab.
				// The ordinary callable path and non-identity persistent generations retain
				// the exact page-by-page validation below.
				if (!complete_persistent_identity)
				{
					if (!EmitSpillLoad(TEMP0, m_work_scratch_offset + 4) ||
						!EmitContextLoad(TEMP2,
							offsetof(ExecutionContext, identity_main_ram_limit)) ||
						!m_code.EmitCmpReg(TEMP0, TEMP2))
					{
						return false;
					}
					const size_t identity_range =
						m_code.EmitBranchPlaceholder(Condition::LS);
					if (identity_range == static_cast<size_t>(-1) ||
						!EmitSpillLoad(TEMP0, m_work_scratch_offset) ||
						!m_code.EmitMovRegShiftImm(TEMP0, TEMP0, ShiftType::LSR,
							SOURCE_PAGE_SHIFT) ||
						!EmitSpillLoad(TEMP1, m_work_scratch_offset + 4) ||
						!m_code.EmitSubImm8(TEMP1, TEMP1, 1) ||
						!m_code.EmitMovRegShiftImm(TEMP1, TEMP1, ShiftType::LSR,
							SOURCE_PAGE_SHIFT) ||
						!m_code.EmitSubReg(TEMP2, TEMP1, TEMP0) ||
						!m_code.EmitAddImm8(TEMP2, TEMP2, 1) ||
						!EmitSpillStore(TEMP2, m_work_scratch_offset + 12) ||
						!EmitContextLoad(TEMP1, offsetof(ExecutionContext, vmap)) ||
						!m_code.EmitAddRegShiftImm(TEMP1, TEMP1, TEMP0,
							ShiftType::LSL, 2) ||
						!EmitContextLoad(TEMP0, offsetof(ExecutionContext, main_ram)) ||
						!EmitContextLoad(TEMP2,
							offsetof(ExecutionContext, host_memory_base)) ||
						!m_code.EmitSubReg(TEMP2, TEMP0, TEMP2))
					{
						return false;
					}
					const size_t mapping_loop = m_code.Size();
					if (!m_code.EmitLdrImm12PostIndex(TEMP0, TEMP1, sizeof(u32)) ||
						!m_code.EmitCmpReg(TEMP0, TEMP2) ||
						!fallback(Condition::NE, ExitReason::MemoryTranslation) ||
						!EmitSpillLoad(TEMP0, m_work_scratch_offset + 12) ||
						!m_code.EmitSubImm8(TEMP0, TEMP0, 1, true) ||
						!EmitSpillStore(TEMP0, m_work_scratch_offset + 12))
					{
						return false;
					}
					const size_t mapping_backedge =
						m_code.EmitBranchPlaceholder(Condition::NE);
					if (mapping_backedge == static_cast<size_t>(-1) ||
						!m_code.PatchBranch(mapping_backedge, mapping_loop,
							Condition::NE) ||
						!m_code.PatchBranch(identity_range, m_code.Size(), Condition::LS))
					{
						return false;
					}
				}

				if (!stores)
					return true;

				// The authoritative page summary avoids scanning 64-byte ownership
				// bits for ordinary data. A live page conservatively triggers an exact
				// chunk scan for the complete store range.
				if (!EmitSpillLoad(TEMP0, m_work_scratch_offset) ||
					!m_code.EmitMovRegShiftImm(TEMP0, TEMP0, ShiftType::LSR,
						SOURCE_PAGE_SHIFT) ||
					!EmitSpillLoad(TEMP1, m_work_scratch_offset + 4) ||
					!m_code.EmitSubImm8(TEMP1, TEMP1, 1) ||
					!m_code.EmitMovRegShiftImm(TEMP1, TEMP1, ShiftType::LSR,
						SOURCE_PAGE_SHIFT) ||
					!m_code.EmitSubReg(TEMP2, TEMP1, TEMP0) ||
					!m_code.EmitAddImm8(TEMP2, TEMP2, 1) ||
					!EmitContextLoad(TEMP1,
						offsetof(ExecutionContext, ram_source_page_live_flags)) ||
					!m_code.EmitAddReg(TEMP1, TEMP1, TEMP0))
				{
					return false;
				}
				const size_t page_loop = m_code.Size();
				if (!m_code.EmitLdrbImm12PostIndex(TEMP0, TEMP1, 1) ||
					!m_code.EmitCmpImm32(TEMP0, 0))
				{
					return false;
				}
				const size_t live_page =
					m_code.EmitBranchPlaceholder(Condition::NE);
				if (live_page == static_cast<size_t>(-1) ||
					!m_code.EmitSubImm8(TEMP2, TEMP2, 1, true))
				{
					return false;
				}
				const size_t page_backedge =
					m_code.EmitBranchPlaceholder(Condition::NE);
				const size_t ownership_done = m_code.EmitBranchPlaceholder();
				const size_t chunk_scan = m_code.Size();
				if (page_backedge == static_cast<size_t>(-1) ||
					ownership_done == static_cast<size_t>(-1) ||
					!m_code.PatchBranch(page_backedge, page_loop, Condition::NE) ||
					!m_code.PatchBranch(live_page, chunk_scan, Condition::NE) ||
					!EmitSpillLoad(TEMP0, m_work_scratch_offset) ||
					!m_code.EmitMovRegShiftImm(TEMP0, TEMP0, ShiftType::LSR,
						SOURCE_CHUNK_SHIFT) ||
					!EmitSpillLoad(TEMP1, m_work_scratch_offset + 4) ||
					!m_code.EmitSubImm8(TEMP1, TEMP1, 1) ||
					!m_code.EmitMovRegShiftImm(TEMP1, TEMP1, ShiftType::LSR,
						SOURCE_CHUNK_SHIFT) ||
					!EmitSpillStore(TEMP1, m_work_scratch_offset + 12))
				{
					return false;
				}
				const size_t chunk_loop = m_code.Size();
				if (!EmitContextLoad(TEMP1,
						offsetof(ExecutionContext, ram_source_chunk_live_bits)) ||
					!m_code.EmitLdrbRegShift(TEMP1, TEMP1, TEMP0,
						ShiftType::LSR, 3) ||
					!m_code.EmitAndImm8(TEMP2, TEMP0, 7) ||
					!m_code.EmitMovRegShiftReg(TEMP1, TEMP1,
						ShiftType::LSR, TEMP2) ||
					!m_code.EmitTstImm32(TEMP1, 1) ||
					!fallback(Condition::NE, ExitReason::SelfModifyingCode) ||
					!m_code.EmitAddImm8(TEMP0, TEMP0, 1) ||
					!EmitSpillLoad(TEMP1, m_work_scratch_offset + 12) ||
					!m_code.EmitCmpReg(TEMP0, TEMP1))
				{
					return false;
				}
				const size_t chunk_backedge =
					m_code.EmitBranchPlaceholder(Condition::LS);
				return chunk_backedge != static_cast<size_t>(-1) &&
				       m_code.PatchBranch(chunk_backedge, chunk_loop, Condition::LS) &&
				       m_code.PatchBranch(ownership_done, m_code.Size());
			}

			bool EmitCop1InternalOuClassifier(u32 block_index, const Node& node)
			{
				if (node.opcode != Opcode::Cop1ExceptionalOuResult ||
					node.operand_count != 1 || block_index >= m_program.blocks.size() ||
					node.id >= m_allocation.cop1_guarded_raw_for_exception.size())
				{
					return false;
				}
				const ValueId raw =
					m_allocation.cop1_guarded_raw_for_exception[node.id];
				if (raw == INVALID_VALUE || raw != node.operands[0])
					return false;

				const Block& block = m_program.blocks[block_index];
				const Node* clamp = nullptr;
				const Node* flags = nullptr;
				for (const Node& candidate : block.nodes)
				{
					if (candidate.source_pc != node.source_pc)
						continue;
					if (candidate.opcode == Opcode::Cop1ClampOuResult &&
						candidate.operand_count == 1 && candidate.operands[0] == raw)
					{
						if (clamp)
							return false;
						clamp = &candidate;
					}
					else if (candidate.opcode == Opcode::Cop1UpdateOuFlags &&
						candidate.operand_count == 2 && candidate.operands[1] == raw)
					{
						if (flags)
							return false;
						flags = &candidate;
					}
				}
				if (!clamp || !flags ||
					clamp->id >= m_allocation.cop1_guarded_clamp_alias.size() ||
					m_allocation.cop1_guarded_clamp_alias[clamp->id] != raw)
				{
					return false;
				}
				// Raw is an internal implementation value. A compact exceptional veneer
				// may replace its physical lane only when every executable consumer
				// belongs to this verifier-owned classifier/clamp/flag group.
				for (const Block& consumer_block : m_program.blocks)
				{
					for (const Node& consumer : consumer_block.nodes)
					{
						for (u32 operand = 0; operand < consumer.operand_count; operand++)
						{
							if (consumer.operands[operand] != raw)
								continue;
							if (consumer.id != node.id && consumer.id != clamp->id &&
								consumer.id != flags->id)
							{
								return false;
							}
						}
					}
				}

				unsigned raw_bits = 0;
				if (!AcquireValueWord(raw, 0, TEMP1, &raw_bits) ||
					!m_code.EmitMovRegShiftImm(TEMP2, raw_bits, ShiftType::LSL, 1,
						true))
				{
					return false;
				}
				const size_t zero = m_code.EmitBranchPlaceholder(Condition::EQ);
				if (zero == static_cast<size_t>(-1) ||
					!m_code.EmitSubImm32(TEMP2, TEMP2, 0x01000000u) ||
					!m_code.EmitCmpImm32(TEMP2, 0xfe000000u))
				{
					return false;
				}
				const size_t exceptional =
					m_code.EmitBranchPlaceholder(Condition::CS);
				if (exceptional == static_cast<size_t>(-1) ||
					!m_code.PatchBranch(zero, m_code.Size(), Condition::EQ))
				{
					return false;
				}
				m_cop1_guarded_cold_paths.push_back({exceptional,
					static_cast<size_t>(-1), raw, clamp->id, flags->operands[0],
					flags->id});
				m_result.cop1_lazy_exception_guards++;
				return true;
			}

			bool EmitGuardedExit(u32 block_index, const Node& node)
			{
				if (node.operand_count != 1 ||
					node.immediate >= m_program.blocks[block_index].guarded_exits.size())
				{
					return false;
				}
				const Transfer& transfer =
					m_program.blocks[block_index].guarded_exits[node.immediate];
				const bool native_overflow = node.operands[0] <
						m_allocation.flagged_add_for_signed_overflow.size() &&
					m_allocation.flagged_add_for_signed_overflow[node.operands[0]] !=
						INVALID_VALUE;
				const RegionExecution::ExitSite* const site =
					FindExitSite(RegionExecution::ExitSiteKind::Guarded,
						block_index, node.immediate);
				if (!native_overflow &&
					(!EmitLoadValueWord(node.operands[0], 0, TEMP0) ||
					 !m_code.EmitCmpImm32(TEMP0, 0)))
				{
					return false;
				}
				return AppendColdBranch(native_overflow ? Condition::VS : Condition::NE,
					transfer.external_reason, site);
			}

			bool EmitVu0RequireIdle(u32 block_index, const Node& node)
			{
				if (node.operand_count != 2 ||
					(node.type != ValueType::I32 &&
					 node.type != ValueType::VuF32x4Bits &&
					 node.type != ValueType::I128) ||
					block_index >= m_program.blocks.size())
				{
					return false;
				}
				auto copy_guarded_value = [&]() {
					std::vector<WordMapping> mappings;
					const u8 mask = LocationMask(Location(node.id));
					for (u8 word = 0; word < 4; word++)
					{
						if ((mask & (1u << word)) != 0)
							mappings.push_back({word, node.operands[1], word});
					}
					return EmitCopyMappings(node.id, mappings);
				};
				if (m_pre_entry_vu0_idle_proof)
				{
					return StorageValue(node.id) != node.id ||
						copy_guarded_value();
				}
				if (!EmitLoadValueWord(node.operands[0], 0, TEMP0) ||
					!m_code.EmitTstImm32(TEMP0, 1))
				{
					return false;
				}
				const Block& block = m_program.blocks[block_index];
				const auto observer = std::find_if(block.observer_exits.begin(),
					block.observer_exits.end(), [&](const ObserverExit& exit) {
						return exit.operation == node.id;
					});
				if (observer == block.observer_exits.end())
					return false;
				const u32 ordinal = static_cast<u32>(
					std::distance(block.observer_exits.begin(), observer));
				if (!AppendColdBranch(Condition::NE, ExitReason::HelperObserver,
						FindExitSite(RegionExecution::ExitSiteKind::Observer,
							block_index, ordinal)))
				{
					return false;
				}
				return StorageValue(node.id) != node.id ||
					copy_guarded_value();
			}

			bool EmitStateLoad(u32 offset, unsigned destination)
			{
				if (Persistent())
				{
					PersistentStateLocation location{};
					if (!PersistentRuntimeLocation(offset, &location))
						return false;
					if (location.base ==
						CompileOptions::PersistentStateBase::CpuRegisters)
					{
						return m_code.EmitLdrImm12(destination, CPU_REGS,
							location.offset);
					}
					const CompileOptions::PersistentDispatch& persistent =
						*m_options.persistent_dispatch;
					return persistent.vu0_state &&
					       m_code.EmitMovImm32(destination, static_cast<u32>(
						   reinterpret_cast<uptr>(persistent.vu0_state))) &&
					       m_code.EmitLdrImm12(destination, destination,
						   location.offset);
				}
				// An immediate-offset A32 LDR permits Rd == Rn when writeback is
				// disabled. Use the requested destination itself for the CanonicalState
				// pointer load, then replace it with the requested word. This matters for
				// rematerialized parameters: entry proofs frequently keep two values in
				// TEMP0/TEMP2 while loading a third, so borrowing another temporary here
				// silently destroys a live proof operand.
				return offset <= 4095 &&
				       m_code.EmitLdrImm12(destination, CONTEXT,
					   static_cast<u16>(offsetof(ExecutionContext, state))) &&
				       m_code.EmitLdrImm12(destination, destination,
					   static_cast<u16>(offset));
			}

			bool EmitStateStore(u32 offset, unsigned source)
			{
				if (Persistent())
				{
					PersistentStateLocation location{};
					if (!PersistentRuntimeLocation(offset, &location))
						return false;
					if (location.base ==
						CompileOptions::PersistentStateBase::CpuRegisters)
					{
						return m_code.EmitStrImm12(source, CPU_REGS,
							location.offset);
					}
					const CompileOptions::PersistentDispatch& persistent =
						*m_options.persistent_dispatch;
					const unsigned pointer = source == TEMP1 ? TEMP2 : TEMP1;
					return persistent.vu0_state &&
					       m_code.EmitMovImm32(pointer, static_cast<u32>(
						   reinterpret_cast<uptr>(persistent.vu0_state))) &&
					       m_code.EmitStrImm12(source, pointer, location.offset);
				}
				const unsigned pointer = source == TEMP1 ? TEMP2 : TEMP1;
				return offset <= 4095 &&
				       m_code.EmitLdrImm12(pointer, CONTEXT,
					   static_cast<u16>(offsetof(ExecutionContext, state))) &&
				       m_code.EmitStrImm12(source, pointer, static_cast<u16>(offset));
			}

			bool EmitEntryGprWord(u32 gpr, u8 word, unsigned destination)
			{
				if (gpr >= 32 || word >= 2 ||
					m_program.entry_block >= m_program.blocks.size())
				{
					return false;
				}
				const ValueId value = m_program.blocks[
					m_program.entry_block].parameters.gpr[gpr];
				if (value < m_program.value_count &&
					(ValueCapabilityMask(value) & (1u << word)) != 0)
				{
					// Entry guards execute after EmitEntryPrefix() has seeded the
					// region-wide allocation. Reuse that exact SSA parameter instead
					// of immediately loading the same canonical word again. High words
					// which semantic demand proved unnecessary remain unallocated and
					// deliberately use the canonical-state fallback below.
					return EmitLoadValueWord(value, word, destination);
				}
				const size_t offset =
					RegionExecution::CanonicalStateWordOffset(gpr, word);
				return offset != SIZE_MAX &&
					EmitStateLoad(static_cast<u32>(offset), destination);
			}

			bool EmitSeedEntryParameter(const Node& node, u8 skip_mask = 0)
			{
				const RegionAllocation::Location& location = Location(node.id);
				if (location.kind == RegionAllocation::LocationKind::None ||
					location.kind ==
						RegionAllocation::LocationKind::Immediate ||
					location.kind ==
						RegionAllocation::LocationKind::CanonicalState)
					return true;
				if (node.immediate >= RegionExecution::STATE_SLOT_COUNT)
					return false;
				const u8 mask = LocationMask(location);
				// A full architectural quadword and an allocated Q register have the
				// same 16-byte layout. Seed it as one value; four core loads followed by
				// four lane inserts would recreate the block-local state traffic which
				// region-wide MMI allocation is intended to eliminate.
				if (Persistent() &&
					location.kind == RegionAllocation::LocationKind::NeonQ &&
					mask == 0x0f && skip_mask == 0 &&
					(node.type == ValueType::I128 ||
					 node.type == ValueType::VuF32x4Bits))
				{
					const size_t canonical =
						RegionExecution::CanonicalStateWordOffset(node.immediate, 0);
					PersistentStateLocation state_location{};
					if (canonical == SIZE_MAX ||
						!PersistentRuntimeLocation(static_cast<u32>(canonical),
							&state_location) ||
						(state_location.offset & 0x0f) != 0)
					{
						return false;
					}
					unsigned base = CPU_REGS;
					if (state_location.base ==
						CompileOptions::PersistentStateBase::Vu0)
					{
						const CompileOptions::PersistentDispatch& persistent =
							*m_options.persistent_dispatch;
						if (!persistent.vu0_state ||
							!m_code.EmitMovImm32(TEMP1, static_cast<u32>(
								reinterpret_cast<uptr>(persistent.vu0_state))))
						{
							return false;
						}
						base = TEMP1;
					}
					const bool address = state_location.offset == 0 ?
						(base == TEMP0 || EmitMove(TEMP0, base)) :
						(m_code.EmitAddImm32(TEMP0, base, state_location.offset) ||
						 (m_code.EmitMovImm32(TEMP0, state_location.offset) &&
						  m_code.EmitAddReg(TEMP0, base, TEMP0)));
					return address &&
					       m_code.EmitVld1Q32Aligned(location.index, TEMP0);
				}
				for (u8 word = 0; word < 4; word++)
				{
					if ((mask & (1u << word)) == 0 ||
						(skip_mask & (1u << word)) != 0)
						continue;
					if (m_persistent_scheduler_countdown &&
						node.type == ValueType::Cycle && word == 1)
					{
						continue;
					}
					const size_t offset = RegionExecution::CanonicalStateWordOffset(
						node.immediate, word);
					if (offset == SIZE_MAX || !EmitStateLoad(static_cast<u32>(offset), TEMP0) ||
						!EmitStoreValueWord(node.id, word, TEMP0))
					{
						return false;
					}
				}
				return true;
			}

			const GprLinkSignature* CompatibleEntrySignature() const
			{
				if (!Persistent() ||
					!m_options.persistent_dispatch->compatible_entry_signature)
				{
					return nullptr;
				}
				// Semantic kernels replace a complete repeated state transition, not
				// merely the block body described by the tier-zero link signature. Until
				// a kernel publishes its own mechanically checked live-in mapping, expose
				// only the canonical first-class entry. Incoming generated code then
				// materializes once before the whole batch instead of carrying a stale
				// induction value through an unrelated tier-zero compatible contract.
				if (m_emit_semantic_kernel)
					return nullptr;
				const GprLinkSignature* const signature =
					m_options.persistent_dispatch->compatible_entry_signature;
				if (!signature->IsValid())
					return nullptr;
				for (const RegionAllocation::EntryLow32Guard& guard :
					m_allocation.entry_low32_guards)
				{
					const RegionExecution::StateSlot slot =
						RegionExecution::DecodeStateSlot(guard.state_slot);
					if (slot.state_class != RegionExecution::StateClass::Gpr)
						return nullptr;
					for (u8 index = 0; index < signature->count; index++)
					{
						const GprLinkMapping& mapping = signature->mappings[index];
						if (mapping.guest == slot.index &&
							mapping.width == GprLinkWidth::Low32 &&
							mapping.dirty == GprLinkDirtyState::WriteBack)
						{
							// Low32 write-back has no independently observable live high
							// word and the tier-zero signature does not yet attest its
							// extension. Keep the region's canonical entry, but do not
							// publish an unsound compatible edge.
							return nullptr;
						}
					}
				}
				return signature;
			}

			bool CompatibleEntrySourceHost(const Node& node, u8 word,
				unsigned* host) const
			{
				const GprLinkSignature* const signature = CompatibleEntrySignature();
				if (!host || !signature ||
					node.immediate >= RegionExecution::STATE_SLOT_COUNT)
				{
					return false;
				}
				const RegionExecution::StateSlot slot =
					RegionExecution::DecodeStateSlot(node.immediate);
				if (slot.state_class != RegionExecution::StateClass::Gpr)
					return false;
				for (u8 index = 0; index < signature->count; index++)
				{
					const GprLinkMapping& mapping = signature->mappings[index];
					if (mapping.guest != slot.index)
						continue;
					if (word == 0)
					{
						*host = mapping.low_host;
						return true;
					}
					if (word == 1 && mapping.width == GprLinkWidth::Low64)
					{
						*host = mapping.high_host;
						return true;
					}
					return false;
				}
				return false;
			}

			bool EmitCompatibleEntrySeeds(const Block& entry)
			{
				const GprLinkSignature* const signature =
					CompatibleEntrySignature();
				if (!signature || !signature->IsValid())
					return false;

				// A tier-zero WriteBack mapping makes cpuRegs stale.  Publish it before
				// the region reuses any host register.  Region inputs still come from the
				// live host below, avoiding the redundant canonical reload while every
				// cold/observer exit remains independently correct.
				for (u8 index = 0; index < signature->count; index++)
				{
					const GprLinkMapping& mapping = signature->mappings[index];
					if (mapping.dirty != GprLinkDirtyState::WriteBack)
						continue;
					const size_t low = RegionExecution::CanonicalStateWordOffset(
						mapping.guest, 0);
					if (low == SIZE_MAX ||
						!EmitStateStore(static_cast<u32>(low), mapping.low_host))
					{
						return false;
					}
					if (mapping.width == GprLinkWidth::Low64)
					{
						const size_t high = RegionExecution::CanonicalStateWordOffset(
							mapping.guest, 1);
						if (high == SIZE_MAX || !EmitStateStore(
								static_cast<u32>(high), mapping.high_host))
						{
							return false;
						}
					}
				}

				struct CoreMove
				{
					unsigned destination = 0;
					unsigned source = 0;
				};
				std::vector<CoreMove> moves;
				std::array<u8, RegionExecution::STATE_SLOT_COUNT> carried{};
				if (signature->HasSchedulerCountdown())
				{
					// BlockCompiler::EndBlockWithCycleTest() carries
					//   signed(cpuRegs.cycle - cpuRegs.nextEventCycle)
					// in r6. Every first-class region consumes that representation
					// directly; counted-memory proofs perform their aggregate horizon check
					// in the same domain. Capture it before allocation can reuse r6.
					const ValueId cycle_value = entry.parameters.cycle;
					if (cycle_value == INVALID_VALUE || cycle_value >= m_program.value_count)
						return false;
					const RegionAllocation::Location& cycle_location = Location(cycle_value);
					if ((cycle_location.kind != RegionAllocation::LocationKind::Core &&
						 cycle_location.kind != RegionAllocation::LocationKind::FixedCycle) ||
						(LocationMask(cycle_location) &
							(m_persistent_scheduler_countdown ? 0x1 : 0x3)) !=
							(m_persistent_scheduler_countdown ? 0x1 : 0x3) ||
						CoreRegister(cycle_location, 0) != CYCLE_LOW ||
						(!m_persistent_scheduler_countdown &&
						 CoreRegister(cycle_location, 1) != CYCLE_HIGH) ||
						!(m_persistent_scheduler_countdown ?
							EmitMove(CYCLE_LOW, signature->scheduler.host) :
							(EmitContextLoad(CYCLE_LOW,
								offsetof(ExecutionContext, next_event_cycle_low)) &&
							 m_code.EmitAddReg(CYCLE_LOW, CYCLE_LOW,
								signature->scheduler.host, true))))
					{
						return false;
					}
					if (!m_persistent_scheduler_countdown &&
						(!EmitContextLoad(CYCLE_HIGH,
							offsetof(ExecutionContext, next_event_cycle_high)) ||
						 !m_code.EmitMovRegShiftImm(TEMP0,
							signature->scheduler.host, ShiftType::ASR, 31) ||
						 !m_code.EmitAdcReg(CYCLE_HIGH, CYCLE_HIGH, TEMP0)))
					{
						return false;
					}

					bool found_cycle_parameter = false;
					for (const Node& node : entry.nodes)
					{
						if (node.opcode != Opcode::Parameter)
							break;
						if (node.id != cycle_value ||
							node.immediate >= carried.size() ||
							RegionExecution::DecodeStateSlot(node.immediate).state_class !=
								RegionExecution::StateClass::Cycle)
						{
							continue;
						}
						carried[node.immediate] = m_persistent_scheduler_countdown ? 0x1 : 0x3;
						found_cycle_parameter = true;
						break;
					}
					if (!found_cycle_parameter)
						return false;
				}
				for (const Node& node : entry.nodes)
				{
					if (node.opcode != Opcode::Parameter)
						break;
					const RegionAllocation::Location& location = Location(node.id);
					const u8 mask = LocationMask(location);
					for (u8 word = 0; word < 4; word++)
					{
						unsigned source = 0;
						if ((mask & (1u << word)) == 0 ||
							!CompatibleEntrySourceHost(node, word, &source))
						{
							continue;
						}
						if (node.immediate >= carried.size())
							return false;
						carried[node.immediate] |= static_cast<u8>(1u << word);
						if (location.kind ==
							RegionAllocation::LocationKind::CanonicalState)
						{
							continue;
						}
						if (location.kind == RegionAllocation::LocationKind::Core ||
							location.kind == RegionAllocation::LocationKind::FixedCycle)
						{
							moves.push_back({CoreRegister(location, word), source});
						}
						else if (!EmitStoreLocationWord(location, word, source))
						{
							return false;
						}
					}
				}

				moves.erase(std::remove_if(moves.begin(), moves.end(),
					[](const CoreMove& move) {
						return move.destination == move.source;
					}), moves.end());
				while (!moves.empty())
				{
					size_t ready = moves.size();
					for (size_t index = 0; index < moves.size(); index++)
					{
						bool destination_is_source = false;
						for (const CoreMove& other : moves)
							destination_is_source |=
								other.source == moves[index].destination;
						if (!destination_is_source)
						{
							ready = index;
							break;
						}
					}
					if (ready == moves.size())
					{
						const unsigned preserved = moves.front().destination;
						if (!EmitMove(TEMP0, preserved))
							return false;
						for (CoreMove& move : moves)
						{
							if (move.source == preserved)
								move.source = TEMP0;
						}
						continue;
					}
					const CoreMove move = moves[ready];
					if (!EmitMove(move.destination, move.source))
						return false;
					moves.erase(moves.begin() + ready);
				}

				for (const Node& node : entry.nodes)
				{
					if (node.opcode != Opcode::Parameter)
						break;
					const u8 skip = node.immediate < carried.size() ?
						carried[node.immediate] : 0;
					if (!EmitSeedEntryParameter(node, skip))
						return false;
				}
				return true;
			}

			bool EmitEntryPrefix(bool compatible)
			{
				if (Persistent())
				{
					// A persistent entry already owns CPU_REGS and all private ABI
					// registers. Reject a conditional representation before allocating
					// a frame or executing a guest instruction, so the leaf can tail to
					// the exact tier-zero entry without any state repair.
					if (!EmitPreEntryLow32Guards(compatible) ||
						!EmitPreEntryProfitabilityGuard(compatible) ||
						!EmitPreEntryVu0IdleGuard(compatible) ||
						!EmitFrameAdjustment(true))
						return false;
					if (m_options.persistent_dispatch->execution_counter &&
						(!m_code.EmitMovImm32(TEMP0, static_cast<u32>(
							reinterpret_cast<uptr>(m_options.persistent_dispatch->
								execution_counter))) ||
						 !m_code.EmitLdrImm12(TEMP1, TEMP0, 0) ||
						 !m_code.EmitAddImm8(TEMP1, TEMP1, 1) ||
						 !m_code.EmitStrImm12(TEMP1, TEMP0, 0)))
					{
						return false;
					}
				}
				else if (!m_code.EmitPush(SAVED_REGISTERS) ||
					!m_code.EmitVpushDRange(8, 8) ||
					!EmitMove(CONTEXT, TEMP0) ||
					!EmitPreEntryLow32Guards(compatible) ||
					!EmitPreEntryProfitabilityGuard(compatible) ||
					!EmitPreEntryVu0IdleGuard(compatible) ||
					!EmitFrameAdjustment(true))
				{
					return false;
				}

				const Block& entry = m_program.blocks[m_program.entry_block];
				if (compatible)
					return EmitCompatibleEntrySeeds(entry);
				for (const Node& node : entry.nodes)
				{
					if (node.opcode != Opcode::Parameter)
						break;
					if (!EmitSeedEntryParameter(node))
						return false;
				}
				return true;
			}

			bool EmitVu0NormalizationConstants()
			{
				bool normalization_required = false;
				bool exponent_required = false;
				bool mac_sign_shifts_required = false;
				for (const Block& block : m_program.blocks)
				{
					for (const Node& node : block.nodes)
					{
						if ((node.opcode == Opcode::Vu0NormalizeVector ||
							 node.opcode == Opcode::Vu0ClampFmacResult) &&
							node.id < m_allocation.value_word_demands.size() &&
							m_allocation.value_word_demands[node.id] != 0)
						{
							normalization_required = true;
						}
						exponent_required |= node.opcode == Opcode::Vu0ConvertFixed &&
							node.id < m_allocation.value_word_demands.size() &&
							m_allocation.value_word_demands[node.id] != 0;
						mac_sign_shifts_required |=
								!m_remap_allocated_neon_high_bank &&
							node.opcode == Opcode::Vu0MacFlagsFromRaw &&
							node.immediate == 0x0f &&
							node.id < m_allocation.value_word_demands.size() &&
							m_allocation.value_word_demands[node.id] != 0;
					}
				}
				exponent_required |= normalization_required;
				if (!exponent_required && !mac_sign_shifts_required)
					return true;

				// These constants are invariant for the complete region invocation.
				// Materialize them once before the entry-block branch, never on a loop
				// backedge.  This mirrors PCSX2's VU vuDouble() bit contract while
				// leaving arithmetic and all architecturally named values in allocated
				// NEON registers.
				if (exponent_required &&
					(!m_code.EmitMovImm32(TEMP0, VU_FLOAT_EXPONENT) ||
					 !m_code.EmitVdupI32QFromCore(VU0_EXPONENT_Q, TEMP0)))
				{
					return false;
				}
				if (normalization_required &&
					(!m_code.EmitMovImm32(TEMP0, VU_FLOAT_SIGN) ||
					 !m_code.EmitVdupI32QFromCore(VU0_SIGN_Q, TEMP0) ||
					 (m_program.options.vu0_overflow_clamp &&
						(!m_code.EmitMovImm32(TEMP0, VU_FLOAT_MAX_FINITE) ||
						 !m_code.EmitVdupI32QFromCore(VU0_MAX_FINITE_Q, TEMP0)))))
				{
					return false;
				}
				return !mac_sign_shifts_required ||
					(m_code.EmitMovImm8(TEMP0, 7) &&
					 m_code.EmitMovImm8(TEMP1, 6) &&
					 m_code.EmitVmovCorePairToD(VU0_MAC_SIGN_SHIFTS_Q * 2,
						TEMP0, TEMP1) &&
					 m_code.EmitMovImm8(TEMP0, 5) &&
					 m_code.EmitMovImm8(TEMP1, 4) &&
					 m_code.EmitVmovCorePairToD(VU0_MAC_SIGN_SHIFTS_Q * 2 + 1,
						TEMP0, TEMP1));
			}

			bool EmitPrologue()
			{
				if (!EmitEntryPrefix(false))
					return false;
				const GprLinkSignature* const compatible_signature =
					CompatibleEntrySignature();
				if (!Persistent() || !compatible_signature)
				{
					return true;
				}

				const size_t canonical_to_common = m_code.EmitBranchPlaceholder();
				if (canonical_to_common == static_cast<size_t>(-1))
					return false;
				m_result.compatible_entry_offset = m_code.Size();
				m_result.compatible_entry_signature =
					*compatible_signature;
				if (!EmitEntryPrefix(true))
					return false;
				if (m_persistent_scheduler_countdown &&
					compatible_signature->HasSchedulerCountdown())
				{
					m_canonical_entry_event_patch = canonical_to_common;
					return true;
				}
				return m_code.PatchBranch(canonical_to_common, m_code.Size());
			}

			bool TransferCycleAdvance(const Block& block, const Transfer& transfer,
				u32* advance) const
			{
				if (!advance)
					return false;
				if (transfer.state.cycle == block.parameters.cycle)
				{
					*advance = 0;
					return true;
				}
				const Node* advanced = Definition(transfer.state.cycle);
				if (!advanced || advanced->opcode != Opcode::AdvanceCycles ||
					advanced->operand_count != 1 ||
					advanced->operands[0] != block.parameters.cycle)
				{
					return false;
				}
				*advance = advanced->immediate;
				return true;
			}

			void BuildAggregateCyclePlan()
			{
				m_aggregate_cycle_edges = false;
				m_block_cycle_debt.clear();
				if (!Persistent())
				{
					m_result.aggregate_cycle_plan_status =
						AggregateCyclePlanStatus::NotPersistent;
					return;
				}
				if (!m_memory_plan.timing.valid)
				{
					m_result.aggregate_cycle_plan_status =
						AggregateCyclePlanStatus::TimingInvalid;
					return;
				}
				if (m_memory_plan.timing.header_block != m_program.entry_block)
				{
					m_result.aggregate_cycle_plan_status =
						AggregateCyclePlanStatus::HeaderNotEntry;
					return;
				}
				// Aggregate timing removes only the successful-path scheduler polls on
				// internal CFG edges. Guarded exception/observer exits keep their exact
				// verifier-owned state and cycle maps and can leave the region early.
				// Non-preflighted memory has the same property in the persistent backend:
				// alignment, non-identity translation, handler and pre-write SMC cases all
				// branch before the guest access.  Their cold leaves retain the exact
				// memory exit and charge m_block_cycle_debt; only the successful direct-RAM
				// path participates in this aggregate event horizon.

				constexpr u64 UNKNOWN = UINT64_MAX;
				m_block_cycle_debt.assign(m_program.blocks.size(), UNKNOWN);
				m_block_cycle_debt[m_program.entry_block] = 0;
				bool changed = true;
				for (size_t pass = 0; changed && pass < m_program.blocks.size(); pass++)
				{
					changed = false;
					for (u32 block_index = 0; block_index < m_program.blocks.size();
						block_index++)
					{
						if (m_block_cycle_debt[block_index] == UNKNOWN)
							continue;
						const Block& block = m_program.blocks[block_index];
						auto propagate = [&](const Transfer& transfer) {
							u32 advance = 0;
							if (!TransferCycleAdvance(block, transfer, &advance))
							{
								m_result.aggregate_cycle_plan_status =
									AggregateCyclePlanStatus::InvalidCycleTransfer;
								return false;
							}
							const u64 debt = m_block_cycle_debt[block_index] + advance;
							if (debt > UINT32_MAX)
							{
								m_result.aggregate_cycle_plan_status =
									AggregateCyclePlanStatus::CycleDebtOverflow;
								return false;
							}
							if (transfer.target_block == INVALID_BLOCK ||
								transfer.target_block == m_memory_plan.timing.header_block)
							{
								return true;
							}
							if (transfer.target_block >= m_block_cycle_debt.size())
							{
								m_result.aggregate_cycle_plan_status =
									AggregateCyclePlanStatus::InvalidTarget;
								return false;
							}
							u64& target = m_block_cycle_debt[transfer.target_block];
							if (target == UNKNOWN)
							{
								target = debt;
								changed = true;
								return true;
							}
							if (target != debt)
							{
								m_result.aggregate_cycle_plan_status =
									AggregateCyclePlanStatus::InconsistentDebt;
								return false;
							}
							return true;
						};
						if (!VisitInternalTransfers(block.terminator,
								[&](const Transfer& transfer, u8) {
									return propagate(transfer);
								}))
						{
							m_block_cycle_debt.clear();
							return;
						}
					}
				}
				if (std::any_of(m_block_cycle_debt.begin(), m_block_cycle_debt.end(),
					[&](u64 debt) { return debt == UNKNOWN; }))
				{
					m_result.aggregate_cycle_plan_status =
						AggregateCyclePlanStatus::UnreachableBlock;
					m_block_cycle_debt.clear();
					return;
				}
				m_aggregate_cycle_edges = true;
				m_result.aggregate_cycle_plan_status =
					AggregateCyclePlanStatus::Enabled;
			}

			bool EffectiveCycleAdvance(const Block& block, const Transfer& transfer,
				u32* result) const
			{
				if (!result)
					return false;
				u32 advance = 0;
				if (!TransferCycleAdvance(block, transfer, &advance))
					return false;
				if (m_aggregate_cycle_edges)
				{
					if (transfer.target_block != INVALID_BLOCK &&
						transfer.target_block != m_memory_plan.timing.header_block)
					{
						*result = 0;
						return true;
					}
					const u32 block_index = static_cast<u32>(&block - m_program.blocks.data());
					if (block_index >= m_block_cycle_debt.size() ||
						m_block_cycle_debt[block_index] > UINT32_MAX - advance)
					{
						return false;
					}
					advance += static_cast<u32>(m_block_cycle_debt[block_index]);
				}
				*result = advance;
				return true;
			}

			bool EmitCycleAdvance(u32 advance, bool set_flags,
				bool* flags_written = nullptr)
			{
				if (flags_written)
					*flags_written = false;
				if (advance == 0)
					return true;
				if (m_persistent_scheduler_countdown)
				{
					if (m_code.EmitAddImm32(CYCLE_LOW, CYCLE_LOW,
							advance, set_flags))
					{
						if (flags_written)
							*flags_written = set_flags;
						return true;
					}
					const bool emitted = m_code.EmitMovImm32(TEMP0, advance) &&
						m_code.EmitAddReg(CYCLE_LOW, CYCLE_LOW, TEMP0, set_flags);
					if (emitted && flags_written)
						*flags_written = set_flags;
					return emitted;
				}
				if (!set_flags)
					return false;
				if (m_code.EmitAddImm32(CYCLE_LOW, CYCLE_LOW,
						advance, true))
				{
					return m_code.EmitAdcImm8(CYCLE_HIGH, CYCLE_HIGH, 0);
				}
				return m_code.EmitMovImm32(TEMP0, advance) &&
				       m_code.EmitAddReg(CYCLE_LOW, CYCLE_LOW, TEMP0, true) &&
				       m_code.EmitAdcImm8(CYCLE_HIGH, CYCLE_HIGH, 0);
			}

			bool EmitAddCycles(const Block& block, const Transfer& transfer,
				bool set_flags = true, bool* flags_written = nullptr)
			{
				u32 advance = 0;
				if (!EffectiveCycleAdvance(block, transfer, &advance))
					return false;
				if (m_aggregate_cycle_edges && advance == 0 &&
					transfer.target_block != INVALID_BLOCK &&
					transfer.target_block != m_memory_plan.timing.header_block)
				{
					m_result.aggregated_cycle_edges++;
				}
				return EmitCycleAdvance(advance, set_flags, flags_written);
			}

			bool EquivalentCycleAdvance(const Block& block,
				const Transfer& left, const Transfer& right) const
			{
				if (left.state.cycle == right.state.cycle)
					return true;
				const Node* const left_advance = Definition(left.state.cycle);
				const Node* const right_advance = Definition(right.state.cycle);
				return left_advance && right_advance &&
					left_advance->opcode == Opcode::AdvanceCycles &&
					right_advance->opcode == Opcode::AdvanceCycles &&
					left_advance->operand_count == 1 &&
					right_advance->operand_count == 1 &&
					left_advance->operands[0] == block.parameters.cycle &&
					right_advance->operands[0] == block.parameters.cycle &&
					left_advance->immediate == right_advance->immediate;
			}

			bool HasEdgeCopies(u32 source_block, u32 target_block,
				u8 edge_index) const
			{
				return std::any_of(m_allocation.edge_copy_steps.begin(),
					m_allocation.edge_copy_steps.end(),
					[&](const RegionAllocation::EdgeCopyStep& step) {
						return step.source_block == source_block &&
						       step.target_block == target_block &&
						       step.edge_index == edge_index;
					});
			}

			bool EmitWholeLocationCopy(const RegionAllocation::EdgeCopyStep& step)
			{
				const auto source_kind = step.source_location.kind;
				const auto target_kind = step.target_location.kind;
				if (step.source_location.words == 4 && step.target_location.words == 4 &&
					LocationMask(step.source_location) == 0x0f &&
					LocationMask(step.target_location) == 0x0f)
				{
					if (source_kind == RegionAllocation::LocationKind::NeonQ &&
						target_kind == RegionAllocation::LocationKind::NeonQ)
					{
						return m_code.EmitVorrQ(step.target_location.index,
							step.source_location.index, step.source_location.index);
					}
					if (source_kind == RegionAllocation::LocationKind::NeonQ &&
						target_kind == RegionAllocation::LocationKind::Spill)
					{
						return EmitQAddress(TEMP2, step.target_location.index) &&
						       m_code.EmitVst1Q32(step.source_location.index, TEMP2);
					}
					if (source_kind == RegionAllocation::LocationKind::Spill &&
						target_kind == RegionAllocation::LocationKind::NeonQ)
					{
						return EmitQAddress(TEMP2, step.source_location.index) &&
						       m_code.EmitVld1Q32(step.target_location.index, TEMP2);
					}
				}
				const u8 target_mask = LocationMask(step.target_location);
				for (u8 word = 0; word < 4; word++)
				{
					if ((target_mask & (1u << word)) == 0)
						continue;
					if (!EmitCopyLocationWord(step.source_location, step.source, word,
							step.target_location, word))
					{
						return false;
					}
				}
				return true;
			}

			bool EmitEdgeCopies(u32 source_block, u32 target_block, u8 edge_index)
			{
				for (const RegionAllocation::EdgeCopyStep& step :
					m_allocation.edge_copy_steps)
				{
					if (step.source_block == source_block &&
						step.target_block == target_block && step.edge_index == edge_index &&
						!EmitWholeLocationCopy(step))
					{
						return false;
					}
				}
				return true;
			}

			bool EmitEdge(u32 block_index, const Transfer& transfer,
				u8 edge_index, RegionExecution::ExitSiteKind site_kind,
				bool allow_layout_fallthrough = false)
			{
				const Block& block = m_program.blocks[block_index];
				const RegionExecution::ExitSite* site =
					FindExitSite(site_kind, block_index);
				bool cycle_flags_valid = false;
				if (!site || !EmitAddCycles(block, transfer, true,
						&cycle_flags_valid))
					return false;
				const bool iteration_budgeted = Persistent() &&
					!m_memory_plan.control.valid &&
					m_memory_plan.timing.valid;
				if (transfer.event_horizon_check &&
					!m_memory_plan.control.valid && !iteration_budgeted &&
					(!EmitEventComparison() || !AppendColdBranch(
						m_persistent_scheduler_countdown ? Condition::PL : Condition::CS,
						ExitReason::EventHorizon, site)))
				{
					return false;
				}
				if (iteration_budgeted && transfer.target_block ==
						m_memory_plan.timing.header_block &&
					!EmitNextIterationBudgetGuard(block_index, site,
						cycle_flags_valid))
				{
					return false;
				}
				if (transfer.target_block == INVALID_BLOCK)
					return AppendColdBranch(Condition::AL, transfer.external_reason, site);
				if (!EmitEdgeCopies(block_index, transfer.target_block, edge_index))
					return false;
				if (allow_layout_fallthrough && block_index < m_layout_successor.size() &&
					m_layout_successor[block_index] == transfer.target_block)
				{
					return true;
				}
				const size_t branch = m_code.EmitBranchPlaceholder();
				if (branch == static_cast<size_t>(-1))
					return false;
				m_internal_patches.push_back({branch, transfer.target_block});
				return true;
			}

			bool EmitRegisterReturnDispatch(u32 block_index)
			{
				const Block& block = m_program.blocks[block_index];
				const bool exhaustive =
					HasExhaustiveDirectReturnTargets(m_program, block_index);
				if (block.terminator.kind != TerminatorKind::RegisterJump ||
					block.terminator.register_targets.empty() ||
					(!(exhaustive && block.terminator.register_targets.size() == 1) &&
					 !EmitLoadValueWord(block.terminator.taken.pc, 0, TEMP0)))
				{
					return false;
				}
				std::vector<size_t> matches;
				matches.reserve(block.terminator.register_targets.size());
				const size_t compared_targets = exhaustive ?
					block.terminator.register_targets.size() - 1 :
					block.terminator.register_targets.size();
				for (size_t index = 0; index < compared_targets; index++)
				{
					const Transfer& target =
						block.terminator.register_targets[index];
					const bool compared = m_code.EmitCmpImm32(TEMP0,
						target.proven_register_target_pc) ||
						(m_code.EmitMovImm32(TEMP1,
							target.proven_register_target_pc) &&
						 m_code.EmitCmpReg(TEMP0, TEMP1));
					if (!target.register_target_proven ||
						!compared)
					{
						return false;
					}
					const size_t match =
						m_code.EmitBranchPlaceholder(Condition::EQ);
					if (match == static_cast<size_t>(-1))
						return false;
					matches.push_back(match);
				}
				if (exhaustive)
				{
					// Verify() proves one of the represented r31 values must match. The
					// last target is therefore the exact fall-through alternative and
					// needs neither another compare nor an impossible full-state exit.
					const size_t last =
						block.terminator.register_targets.size() - 1;
					if (!EmitEdge(block_index,
							block.terminator.register_targets[last],
							static_cast<u8>(last + 1),
							RegionExecution::ExitSiteKind::Taken))
					{
						return false;
					}
				}
				else if (!EmitEdge(block_index, block.terminator.taken, 0,
						RegionExecution::ExitSiteKind::Taken))
				{
					return false;
				}
				for (size_t index = 0; index < compared_targets; index++)
				{
					if (!m_code.PatchBranch(matches[index], m_code.Size(), Condition::EQ) ||
						!EmitEdge(block_index,
							block.terminator.register_targets[index],
							static_cast<u8>(index + 1),
							RegionExecution::ExitSiteKind::Taken))
					{
						return false;
					}
				}
				return true;
			}

			bool EmitBlock(u32 block_index)
			{
				const Block& block = m_program.blocks[block_index];
				const bool likely = block.terminator.kind == TerminatorKind::Branch &&
					block.terminator.likely;
				auto taken_delay_node = [&](const Node& node) {
					return likely && node.opcode != Opcode::Parameter &&
						node.source_pc == block.terminator.delay_slot_pc;
				};
				auto emit_measured_node = [&](const Node& node) {
					const size_t before = m_code.Size();
					const u32 spill_loads_before = m_result.spill_word_loads;
					const u32 spill_stores_before = m_result.spill_word_stores;
					if (!EmitNode(block_index, node) ||
						!CompleteCop1GuardedColdPath(node.id))
					{
						if (m_result.failure == CompileFailure::None)
							Fail(CompileFailure::Emission, node.source_pc);
						if (m_result.failure_ir_opcode == UINT16_MAX)
						{
							m_result.failure_ir_opcode =
								static_cast<u16>(node.opcode);
							m_result.failure_value = node.id;
						}
						return false;
					}
					const u32 bytes = static_cast<u32>(m_code.Size() - before);
					const u32 spill_loads =
						m_result.spill_word_loads - spill_loads_before;
					const u32 spill_stores =
						m_result.spill_word_stores - spill_stores_before;
					switch (node.opcode)
					{
						case Opcode::Vu0MulRaw:
						case Opcode::Vu0AddRaw:
						case Opcode::Vu0SubRaw:
							m_result.vu0_raw_spill_word_loads += spill_loads;
							m_result.vu0_raw_spill_word_stores += spill_stores;
							break;
						case Opcode::Vu0NormalizeVector:
						case Opcode::Vu0ClampFmacResult:
						case Opcode::Vu0MacFlagsFromRaw:
						case Opcode::Vu0StatusFlagsFromMac:
						case Opcode::Vu0MergeMasked:
						case Opcode::Vu0SyncStatusControl:
							m_result.vu0_pipeline_spill_word_loads += spill_loads;
							m_result.vu0_pipeline_spill_word_stores += spill_stores;
							break;
						case Opcode::Vu0ConvertFixed:
						case Opcode::Vu0ConvertIntegerToFloat:
						case Opcode::Vu0Rotate32:
						case Opcode::Vu0BroadcastLane:
						case Opcode::Vu0BroadcastScalar:
							m_result.vu0_transform_spill_word_loads += spill_loads;
							m_result.vu0_transform_spill_word_stores += spill_stores;
							break;
						default:
							break;
					}
						switch (node.opcode)
						{
							case Opcode::Cop1NormalizeInput:
								m_result.cop1_hot_bytes += bytes;
								m_result.cop1_normalize_hot_bytes += bytes;
								break;
							case Opcode::Cop1AddRaw:
							case Opcode::Cop1SubRaw:
							case Opcode::Cop1MulRaw:
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
								m_result.cop1_hot_bytes += bytes;
								break;
							case Opcode::Cop1ClampOuResult:
							case Opcode::Cop1UpdateOuFlags:
								m_result.cop1_hot_bytes += bytes;
								m_result.cop1_ou_hot_bytes += bytes;
								break;
						case Opcode::MemoryLoad:
						case Opcode::MemoryStore:
							m_result.memory_hot_bytes += bytes;
							break;
						default:
							break;
					}
					return true;
				};
				for (const Node& node : block.nodes)
				{
					if (node.opcode == Opcode::Parameter ||
						IsDirectBranchCondition(block_index, node) ||
						taken_delay_node(node))
					{
						continue;
					}
					if (!emit_measured_node(node))
						return false;
				}
				if (block.terminator.kind == TerminatorKind::Transfer ||
					block.terminator.kind == TerminatorKind::Jump)
				{
					return EmitEdge(block_index, block.terminator.taken, 0,
						RegionExecution::ExitSiteKind::Taken, true);
				}
				if (block.terminator.kind == TerminatorKind::RegisterJump)
				{
					return block.terminator.register_targets.empty() ?
						EmitEdge(block_index, block.terminator.taken, 0,
							RegionExecution::ExitSiteKind::Taken, true) :
						EmitRegisterReturnDispatch(block_index);
				}
				if (block.terminator.kind != TerminatorKind::Branch)
				{
					return false;
				}
				const Transfer& taken_transfer = block.terminator.taken;
				const Transfer& not_taken_transfer = block.terminator.not_taken;
				Condition taken_condition = Condition::NE;
				const Node* condition = Definition(block.terminator.condition);
				Condition materialized_condition = Condition::AL;
				const bool can_reuse_materialized_flags = condition &&
					IsDirectBranchCondition(block_index, *condition) &&
					ReuseMaterializedBooleanFlags(block_index, *condition,
						&materialized_condition);
				auto emit_condition = [&](bool allow_materialized_flags) {
					if (allow_materialized_flags && can_reuse_materialized_flags)
					{
						taken_condition = materialized_condition;
						m_result.reused_materialized_branch_flags++;
						return true;
					}
					if (condition && IsDirectBranchCondition(block_index, *condition))
						return EmitDirectBranchCondition(*condition, &taken_condition);
					return EmitLoadValueWord(block.terminator.condition, 0, TEMP0) &&
					       m_code.EmitCmpImm32(TEMP0, 0);
				};
				const bool iteration_budgeted = Persistent() &&
					!m_memory_plan.control.valid &&
					m_memory_plan.timing.valid;
				const bool aggregate_budgeted = Persistent() &&
					(m_memory_plan.control.valid || iteration_budgeted);
				const auto requires_next_iteration_guard = [&](const Transfer& transfer) {
					return iteration_budgeted && transfer.target_block ==
						m_memory_plan.timing.header_block;
				};
				const auto aggregate_publishes_cycles = [&](const Transfer& transfer) {
					return !m_aggregate_cycle_edges ||
						transfer.target_block == INVALID_BLOCK ||
						transfer.target_block == m_memory_plan.timing.header_block;
				};
				// Keep a side exit out of the repeated trace when the verified block
				// layout already places the ordinary internal successor immediately
				// after this block. Aggregate timing deliberately owes the internal
				// edge's cycles at a later latch; charge the external edge only in its
				// cold leaf. The former generic sequence emitted a conditional branch
				// around an unconditional branch to this same successor on every loop
				// iteration.
				const bool taken_external = taken_transfer.target_block == INVALID_BLOCK;
				const bool not_taken_external =
					not_taken_transfer.target_block == INVALID_BLOCK;
				const bool likely_internal_taken = likely && !taken_external &&
					not_taken_external;
				if ((!likely || likely_internal_taken) && Persistent() &&
					m_aggregate_cycle_edges &&
					taken_external != not_taken_external)
				{
					const Transfer& internal = taken_external ?
						not_taken_transfer : taken_transfer;
					const Transfer& external = taken_external ?
						taken_transfer : not_taken_transfer;
					const u8 internal_edge = taken_external ? 1 : 0;
					const RegionExecution::ExitSiteKind external_kind = taken_external ?
						RegionExecution::ExitSiteKind::Taken :
						RegionExecution::ExitSiteKind::NotTaken;
					const bool backedge_fallthrough =
						internal.target_block == m_memory_plan.timing.header_block;
					const bool layout_fallthrough =
						internal.target_block < m_layout_successor.size() &&
						m_layout_successor[block_index] == internal.target_block;
					if ((backedge_fallthrough || layout_fallthrough) &&
						!HasEdgeCopies(block_index, internal.target_block, internal_edge))
					{
						if (!emit_condition(true) || taken_condition == Condition::AL)
							return false;
						const Condition external_condition = taken_external ?
							taken_condition : static_cast<Condition>(
								static_cast<u8>(taken_condition) ^ 1u);
						u32 deferred_cycle_advance = 0;
						if (!EffectiveCycleAdvance(block, external,
								&deferred_cycle_advance) ||
							!AppendColdBranch(external_condition,
								external.external_reason,
								FindExitSite(external_kind, block_index),
								INVALID_VALUE, false, deferred_cycle_advance))
						{
							return false;
						}
						m_result.conditional_layout_fallthroughs++;
						if (likely_internal_taken)
						{
							for (const Node& node : block.nodes)
							{
								if (taken_delay_node(node) && !emit_measured_node(node))
									return false;
							}
						}
						if (backedge_fallthrough)
						{
							return EmitEdge(block_index, internal, internal_edge,
								taken_external ?
									RegionExecution::ExitSiteKind::NotTaken :
									RegionExecution::ExitSiteKind::Taken);
						}
						m_result.aggregated_cycle_edges++;
						return true;
					}
				}
				// Aggregate-cycle lowering deliberately defers an internal edge's
				// charge until the iteration reaches its latch or a side exit.  The
				// ordinary coalescing below can share one charge only when both edges
				// make the same publish/defer decision.  A loop latch and external side
				// exit both publish, while two acyclic internal edges both defer.
				const bool coalesced_budgeted_edges = !likely && aggregate_budgeted &&
					aggregate_publishes_cycles(taken_transfer) ==
						aggregate_publishes_cycles(not_taken_transfer) &&
					EquivalentCycleAdvance(block, taken_transfer,
						not_taken_transfer) &&
					!requires_next_iteration_guard(taken_transfer) &&
					!requires_next_iteration_guard(not_taken_transfer) &&
					(taken_transfer.target_block == INVALID_BLOCK ||
					 !HasEdgeCopies(block_index, taken_transfer.target_block, 0)) &&
					(not_taken_transfer.target_block == INVALID_BLOCK ||
					 !HasEdgeCopies(block_index, not_taken_transfer.target_block, 1));
				if (coalesced_budgeted_edges)
				{
					const RegionExecution::ExitSite* const taken_site = FindExitSite(
						RegionExecution::ExitSiteKind::Taken, block_index);
					const RegionExecution::ExitSite* const not_taken_site = FindExitSite(
						RegionExecution::ExitSiteKind::NotTaken, block_index);
					if (!taken_site || !not_taken_site ||
						!EmitAddCycles(block, taken_transfer,
							!can_reuse_materialized_flags) ||
						!emit_condition(true))
					{
						return false;
					}
					auto emit_direct_edge = [&](const Transfer& transfer,
						Condition condition,
						const RegionExecution::ExitSite* site) {
						if (transfer.target_block == INVALID_BLOCK)
							return AppendColdBranch(condition,
								transfer.external_reason, site);
						const size_t branch =
							m_code.EmitBranchPlaceholder(condition);
						if (branch == static_cast<size_t>(-1))
							return false;
						m_internal_patches.push_back({branch,
							transfer.target_block, condition});
						return true;
					};
					return emit_direct_edge(taken_transfer, taken_condition,
							taken_site) &&
					       emit_direct_edge(not_taken_transfer, Condition::AL,
							not_taken_site);
				}
				const bool direct_coalesced_taken = !likely && !iteration_budgeted &&
					m_persistent_scheduler_countdown &&
					taken_transfer.target_block != INVALID_BLOCK &&
					not_taken_transfer.target_block == INVALID_BLOCK &&
					aggregate_publishes_cycles(taken_transfer) &&
					taken_transfer.event_horizon_check &&
					not_taken_transfer.event_horizon_check &&
					EquivalentCycleAdvance(block, taken_transfer,
						not_taken_transfer) &&
					!HasEdgeCopies(block_index, taken_transfer.target_block, 0);
				if (direct_coalesced_taken)
				{
					const RegionExecution::ExitSite* const taken_site = FindExitSite(
						RegionExecution::ExitSiteKind::Taken, block_index);
					const RegionExecution::ExitSite* const not_taken_site = FindExitSite(
						RegionExecution::ExitSiteKind::NotTaken, block_index);
					if (!taken_site || !not_taken_site ||
						!EmitAddCycles(block, taken_transfer) ||
						!EmitEventComparison())
					{
						return false;
					}
					const size_t event_selector =
						m_code.EmitBranchPlaceholder(Condition::PL);
					if (event_selector == static_cast<size_t>(-1) ||
						!emit_condition(false))
					{
						return false;
					}
					const size_t direct_taken =
						m_code.EmitBranchPlaceholder(taken_condition);
					if (direct_taken == static_cast<size_t>(-1))
						return false;
					m_internal_patches.push_back({direct_taken,
						taken_transfer.target_block, taken_condition});
					if (!AppendColdBranch(Condition::AL,
							not_taken_transfer.external_reason, not_taken_site))
					{
						return false;
					}

					const size_t event_target = m_code.Size();
					if (!m_code.PatchBranch(event_selector, event_target,
							Condition::PL) || !emit_condition(false))
					{
						return false;
					}
					const size_t event_taken =
						m_code.EmitBranchPlaceholder(taken_condition);
					if (event_taken == static_cast<size_t>(-1) ||
						!AppendColdBranch(Condition::AL,
							ExitReason::EventHorizon, not_taken_site))
					{
						return false;
					}
					const size_t event_taken_target = m_code.Size();
					return m_code.PatchBranch(event_taken, event_taken_target,
							taken_condition) &&
					       AppendColdBranch(Condition::AL,
							   ExitReason::EventHorizon, taken_site);
				}
				if (can_reuse_materialized_flags)
				{
					taken_condition = materialized_condition;
					m_result.reused_materialized_branch_flags++;
				}
				else if (condition && IsDirectBranchCondition(block_index, *condition))
				{
					if (!EmitDirectBranchCondition(*condition, &taken_condition))
						return false;
				}
				else if (!EmitLoadValueWord(block.terminator.condition, 0, TEMP0) ||
					!m_code.EmitCmpImm32(TEMP0, 0))
				{
					return false;
				}
				const size_t taken =
					m_code.EmitBranchPlaceholder(taken_condition);
				if (taken == static_cast<size_t>(-1) ||
					!EmitEdge(block_index, block.terminator.not_taken, 1,
						RegionExecution::ExitSiteKind::NotTaken))
				{
					return false;
				}
				const size_t taken_target = m_code.Size();
				if (!m_code.PatchBranch(taken, taken_target, taken_condition))
					return false;
				if (likely)
				{
					for (const Node& node : block.nodes)
					{
						if (taken_delay_node(node) && !emit_measured_node(node))
							return false;
					}
				}
				return EmitEdge(block_index, block.terminator.taken, 0,
					RegionExecution::ExitSiteKind::Taken);
			}

			bool EmitExitMetadata(const ColdExit& exit)
			{
				RegionExecution::ExitContractView contract{};
				if (!exit.site || !RegionExecution::ResolveExitContract(
						m_program, *exit.site, &contract) || !contract.state)
				{
					return false;
				}
				if (Persistent())
				{
					if (contract.transfer)
					{
						if (!EmitLoadValueWord(contract.transfer->pc, 0, TEMP0))
							return false;
					}
					else if (!m_code.EmitMovImm32(TEMP0,
							m_program.blocks[exit.site->block].pc))
					{
						return false;
					}
					return EmitStateStore(offsetof(CanonicalState, pc), TEMP0);
				}

				// Capture a failed access before canonical-state materialization can
				// reuse its allocation. EE LQ/SQ publish their masked address; an
				// LQC2/SQC2 alignment failure publishes the original unaligned EA.
				if (exit.memory_address != INVALID_VALUE)
				{
					if (!EmitLoadValueWord(exit.memory_address, 0, TEMP0) ||
						(exit.mask_quad_address &&
						 !m_code.EmitBicImm32(TEMP0, TEMP0, 0x0f)) ||
						!m_code.EmitStrImm12(TEMP0, CONTEXT,
							static_cast<u16>(ResultOffset(offsetof(
								ExecutionResult, memory_address)))))
					{
						return false;
					}
				}
				else if (!m_code.EmitMovImm8(TEMP0, 0) ||
					!m_code.EmitStrImm12(TEMP0, CONTEXT,
						static_cast<u16>(ResultOffset(offsetof(
							ExecutionResult, memory_address)))))
				{
					return false;
				}
				if (contract.transfer)
				{
					if (!EmitLoadValueWord(contract.transfer->pc, 0, TEMP0))
						return false;
				}
				else if (!m_code.EmitMovImm32(TEMP0,
						m_program.blocks[exit.site->block].pc))
				{
					return false;
				}
				return EmitStateStore(offsetof(CanonicalState, pc), TEMP0) &&
				       m_code.EmitMovImm32(TEMP0, static_cast<u32>(exit.reason)) &&
				       m_code.EmitStrImm12(TEMP0, CONTEXT,
					   static_cast<u16>(ResultOffset(offsetof(
						   ExecutionResult, reason))));
			}

			bool ExitStatesEquivalent(const RegionExecution::ExitSite& left,
				const RegionExecution::ExitSite& right) const
			{
				RegionExecution::ExitContractView left_contract{};
				RegionExecution::ExitContractView right_contract{};
				if (!RegionExecution::ResolveExitContract(m_program, left,
						&left_contract) ||
					!RegionExecution::ResolveExitContract(m_program, right,
						&right_contract) ||
					!left_contract.state || !right_contract.state ||
					left_contract.cycle_commit_deferred !=
						right_contract.cycle_commit_deferred ||
					left_contract.pending_raw_cycles !=
						right_contract.pending_raw_cycles)
				{
					return false;
				}
				for (size_t slot = 0; slot < RegionExecution::STATE_SLOT_COUNT; slot++)
				{
					if (left.dirty_words[slot] != right.dirty_words[slot] ||
						left.dirty_state.test(slot) != right.dirty_state.test(slot))
					{
						return false;
					}
					if (left.dirty_words[slot] != 0 &&
						RegionExecution::StateValue(*left_contract.state, slot) !=
							RegionExecution::StateValue(*right_contract.state, slot))
					{
						return false;
					}
				}
				return true;
			}

			const GprLinkSignature* CompatibleTargetSignature(u32 pc) const
			{
				if (!Persistent() || m_emit_semantic_kernel)
					return nullptr;
				const CompileOptions::PersistentDispatch& persistent =
					*m_options.persistent_dispatch;
				for (size_t index = 0; index < persistent.compatible_target_count;
					index++)
				{
					if (persistent.compatible_targets[index].pc == pc)
						return &persistent.compatible_targets[index].signature;
				}
				return nullptr;
			}

			bool SignatureWordHost(const GprLinkSignature& signature,
				u8 guest, u8 word, const GprLinkMapping** mapping_out,
				unsigned* host) const
			{
				if (!mapping_out || !host)
					return false;
				for (u8 index = 0; index < signature.count; index++)
				{
					const GprLinkMapping& mapping = signature.mappings[index];
					if (mapping.guest != guest)
						continue;
					if (word == 0)
					{
						*mapping_out = &mapping;
						*host = mapping.low_host;
						return true;
					}
					if (word == 1 && mapping.width == GprLinkWidth::Low64)
					{
						*mapping_out = &mapping;
						*host = mapping.high_host;
						return true;
					}
					return false;
				}
				return false;
			}

			bool CompatibleTargetDefersCanonicalStore(
				const GprLinkSignature* signature, size_t slot, u8 word) const
			{
				if (!signature)
					return false;
				const RegionExecution::StateSlot decoded =
					RegionExecution::DecodeStateSlot(slot);
				if (decoded.state_class != RegionExecution::StateClass::Gpr)
					return false;
				const GprLinkMapping* mapping = nullptr;
				unsigned host = 0;
				return SignatureWordHost(*signature, decoded.index, word,
					&mapping, &host) &&
					mapping->dirty == GprLinkDirtyState::WriteBack;
			}

			bool EmitCompatibleExitSeeds(const RegionExecution::ExitSite& site,
				const RegionExecution::ExitContractView& contract,
				const GprLinkSignature& signature)
			{
				struct CoreMove
				{
					unsigned destination = 0;
					unsigned source = 0;
				};
				struct DeferredMove
				{
					unsigned destination = 0;
					size_t slot = 0;
					u8 word = 0;
					ValueId value = INVALID_VALUE;
					bool canonical = false;
				};
				std::vector<CoreMove> core_moves;
				std::vector<DeferredMove> deferred;
				for (u8 index = 0; index < signature.count; index++)
				{
					const GprLinkMapping& mapping = signature.mappings[index];
					const u8 words = mapping.width == GprLinkWidth::Low64 ? 2 : 1;
					for (u8 word = 0; word < words; word++)
					{
						const unsigned destination = word == 0 ?
							mapping.low_host : mapping.high_host;
						const size_t slot = mapping.guest;
						if ((site.dirty_words[slot] & (1u << word)) == 0)
						{
							deferred.push_back({destination, slot, word,
								INVALID_VALUE, true});
							continue;
						}
						const ValueId value = RegionExecution::StateValue(
							*contract.state, slot);
						const RegionAllocation::Location& location = Location(value);
						if ((ValueCapabilityMask(value) & (1u << word)) == 0)
							return false;
						if ((LocationMask(location) & (1u << word)) != 0 &&
							(location.kind == RegionAllocation::LocationKind::Core ||
							 location.kind == RegionAllocation::LocationKind::FixedCycle))
						{
							core_moves.push_back({destination,
								CoreRegister(location, word)});
						}
						else
						{
							deferred.push_back({destination, slot, word,
								value, false});
						}
					}
				}

				core_moves.erase(std::remove_if(core_moves.begin(), core_moves.end(),
					[](const CoreMove& move) {
						return move.destination == move.source;
					}), core_moves.end());
				while (!core_moves.empty())
				{
					size_t ready = core_moves.size();
					for (size_t index = 0; index < core_moves.size(); index++)
					{
						bool destination_is_source = false;
						for (const CoreMove& other : core_moves)
							destination_is_source |=
								other.source == core_moves[index].destination;
						if (!destination_is_source)
						{
							ready = index;
							break;
						}
					}
					if (ready == core_moves.size())
					{
						const unsigned preserved = core_moves.front().destination;
						if (!EmitMove(TEMP0, preserved))
							return false;
						for (CoreMove& move : core_moves)
						{
							if (move.source == preserved)
								move.source = TEMP0;
						}
						continue;
					}
					const CoreMove move = core_moves[ready];
					if (!EmitMove(move.destination, move.source))
						return false;
					core_moves.erase(core_moves.begin() + ready);
				}

				for (const DeferredMove& move : deferred)
				{
					if (move.canonical)
					{
						const size_t offset = RegionExecution::CanonicalStateWordOffset(
							move.slot, move.word);
						if (offset == SIZE_MAX || !EmitStateLoad(
								static_cast<u32>(offset), move.destination))
						{
							return false;
						}
					}
					else if (!EmitLoadValueWord(move.value, move.word,
							move.destination))
					{
						return false;
					}
				}
				return true;
			}

			bool EmitPreEntryFallbackAccounting(ExitReason reason)
			{
				if (!Persistent())
				{
					return true;
				}
				u32* counter = nullptr;
				switch (reason)
				{
					case ExitReason::ProfitabilityFallback:
						counter = m_options.persistent_dispatch->
							profitability_fallback_counter;
						break;
					case ExitReason::EntryStateFallback:
						counter = m_options.persistent_dispatch->
							entry_state_fallback_counter;
						break;
					default:
						return true;
				}
				if (!counter)
					return true;
				// Use only r0/lr here. A pre-entry compatible rejection must
				// preserve r12 and every callee-saved private-state host exactly.
				if (!m_code.EmitMovImm32(TEMP0, static_cast<u32>(
						reinterpret_cast<uptr>(counter))) ||
					!m_code.EmitLdrImm12(TEMP2, TEMP0, 0))
				{
					return false;
				}
				// This is diagnostic accounting, never admission policy. Saturate at
				// UINT32_MAX so an exceptionally long trace cannot wrap and so an
				// already-saturated counter stops dirtying its cache line.
				if (!m_code.EmitCmpImm32(TEMP2, UINT32_MAX))
					return false;
				const size_t saturated =
					m_code.EmitBranchPlaceholder(Condition::EQ);
				if (saturated == static_cast<size_t>(-1))
					return false;
				if (!m_code.EmitAddImm8(TEMP2, TEMP2, 1) ||
					!m_code.EmitStrImm12(TEMP2, TEMP0, 0))
				{
					return false;
				}
				return m_code.PatchBranch(
					saturated, m_code.Size(), Condition::EQ);
			}

			bool EmitCallablePreEntryFallback(ExitReason reason)
			{
				// EmitEntryPrefix() has saved the callable ABI and established
				// CONTEXT, but deliberately has not allocated the region frame or
				// seeded an allocated value. Publish a zero-effect completed exit at
				// the original PC and restore the callable frame exactly.
				return m_code.EmitMovImm8(TEMP0, 1) &&
					m_code.EmitStrImm12(TEMP0, CONTEXT,
						static_cast<u16>(ResultOffset(offsetof(
							ExecutionResult, completed)))) &&
					m_code.EmitMovImm32(TEMP0, static_cast<u32>(reason)) &&
					m_code.EmitStrImm12(TEMP0, CONTEXT,
						static_cast<u16>(ResultOffset(offsetof(
							ExecutionResult, reason)))) &&
					m_code.EmitMovImm8(TEMP0, 0) &&
					m_code.EmitStrImm12(TEMP0, CONTEXT,
						static_cast<u16>(ResultOffset(offsetof(
							ExecutionResult, cycle_commit_deferred)))) &&
					m_code.EmitStrImm12(TEMP0, CONTEXT,
						static_cast<u16>(ResultOffset(offsetof(
							ExecutionResult, pending_raw_cycles)))) &&
					m_code.EmitStrImm12(TEMP0, CONTEXT,
						static_cast<u16>(ResultOffset(offsetof(
							ExecutionResult, memory_address)))) &&
					m_code.EmitVpopDRange(8, 8) &&
					m_code.EmitMovImm8(TEMP0, 1) &&
					m_code.EmitPop(RESTORED_REGISTERS);
			}

			bool EmitPreEntryFallbacks()
			{
				for (const PreEntryFallback& fallback : m_pre_entry_fallbacks)
				{
					if (fallback.branches.empty())
						continue;
					const size_t target = m_code.Size();
					for (const ColdExit::AliasPatch& branch : fallback.branches)
					{
						if (!m_code.PatchBranch(branch.offset, target, branch.condition))
							return false;
					}
					if (!Persistent())
					{
						if (fallback.compatible ||
							!EmitCallablePreEntryFallback(fallback.reason))
							return false;
						m_result.cold_exit_leaves++;
						m_result.pre_entry_profitability_leaves +=
							fallback.reason == ExitReason::ProfitabilityFallback ? 1u : 0u;
						m_result.pre_entry_state_leaves +=
							fallback.reason == ExitReason::EntryStateFallback ? 1u : 0u;
						continue;
					}
					if (!EmitPreEntryFallbackAccounting(fallback.reason))
						return false;
					const size_t tail = m_code.EmitBranchPlaceholder();
					if (tail == static_cast<size_t>(-1))
						return false;
					PersistentExitPatch patch{};
					patch.branch_offset = tail;
					patch.reason = fallback.reason;
					patch.resume_pc = BlockPc(m_program.entry_block);
					patch.entry_passthrough = true;
					if (fallback.compatible)
					{
						const GprLinkSignature* const signature =
							CompatibleEntrySignature();
						if (!signature || !signature->IsValid())
							return false;
						patch.compatible_signature = *signature;
					}
					m_result.persistent_exit_patches.push_back(patch);
					m_result.cold_exit_leaves++;
					m_result.pre_entry_profitability_leaves +=
						fallback.reason == ExitReason::ProfitabilityFallback ? 1u : 0u;
					m_result.pre_entry_state_leaves +=
						fallback.reason == ExitReason::EntryStateFallback ? 1u : 0u;
				}
				return true;
			}

			bool EmitPersistentExitCycleState(
				const RegionExecution::ExitSite& site)
			{
				if (!m_persistent_scheduler_countdown)
					return true;
				const RegionMemoryPlan::IterationTiming& timing =
					m_memory_plan.timing;
				const bool iteration_budget_bias_active =
					!m_memory_plan.control.valid && timing.valid &&
					m_memory_plan.header_seeds_match_entry &&
					timing.maximum_scaled_cycles != 0 &&
					site.kind != RegionExecution::ExitSiteKind::EntryEvent;
				if (iteration_budget_bias_active &&
					!m_code.EmitSubImm32(CYCLE_LOW, CYCLE_LOW,
						timing.maximum_scaled_cycles))
				{
					return false;
				}
				// nextEventCycle + sign_extend(current_countdown) is the exact
				// architectural cycle.  Publish it before any AAPCS cold helper can
				// clobber the dispatcher's private countdown register.
				return m_code.EmitMovRegShiftImm(TEMP0, CYCLE_LOW,
						ShiftType::ASR, 31) &&
				       EmitContextLoad(TEMP1,
						   offsetof(ExecutionContext, next_event_cycle_low)) &&
				       m_code.EmitAddReg(TEMP1, TEMP1, CYCLE_LOW, true) &&
				       EmitContextLoad(TEMP2,
						   offsetof(ExecutionContext, next_event_cycle_high)) &&
				       m_code.EmitAdcReg(TEMP2, TEMP2, TEMP0) &&
				       EmitStateStore(offsetof(CanonicalState, cycle), TEMP1) &&
				       EmitStateStore(offsetof(CanonicalState, cycle) + sizeof(u32),
						   TEMP2);
			}

			bool ResolvePersistentResumePc(
				const RegionExecution::ExitSite& site,
				const RegionExecution::ExitContractView& contract, u32* resume_pc,
				bool* dynamic_resume_pc) const
			{
				if (!resume_pc || !dynamic_resume_pc || site.block >= m_program.blocks.size())
					return false;
				*resume_pc = m_program.blocks[site.block].pc;
				*dynamic_resume_pc = false;
				if (!contract.transfer)
					return true;
				const Node* const pc = Definition(contract.transfer->pc);
				if (!pc)
					return false;
				if (pc->opcode == Opcode::ConstantAddress)
					*resume_pc = static_cast<u32>(pc->literal);
				else
					*dynamic_resume_pc = true;
				return true;
			}

			bool ColdCoreSnapshotOffset(unsigned host, u16* offset) const
			{
				if (!offset)
					return false;
				for (u32 index = 0; index < PERSISTENT_COLD_CORE_REGISTERS.size(); index++)
				{
					if (PERSISTENT_COLD_CORE_REGISTERS[index] != host)
						continue;
					const u32 result = m_cold_core_snapshot_offset + index * sizeof(u32);
					if (result > UINT16_MAX)
						return false;
					*offset = static_cast<u16>(result);
					return true;
				}
				return false;
			}

			bool ColdVfpSnapshotOffset(unsigned physical_s, u16* offset) const
			{
				if (!offset)
					return false;
				u32 index = 0;
				if (physical_s < PERSISTENT_COLD_VFP_LOW_S_COUNT)
					index = physical_s;
				else if (physical_s >= PERSISTENT_COLD_VFP_HIGH_S_FIRST &&
					physical_s < PERSISTENT_COLD_VFP_HIGH_S_FIRST +
						PERSISTENT_COLD_VFP_HIGH_S_COUNT)
				{
					index = PERSISTENT_COLD_VFP_LOW_S_COUNT +
						physical_s - PERSISTENT_COLD_VFP_HIGH_S_FIRST;
				}
				else
				{
					return false;
				}
				const u32 result = m_cold_vfp_snapshot_offset + index * sizeof(u32);
				if (result > UINT16_MAX)
					return false;
				*offset = static_cast<u16>(result);
				return true;
			}

			bool BuildPersistentColdWordSource(ValueId value, u8 word,
				PersistentColdWord* output) const
			{
				if (!output || value >= m_program.value_count)
					return false;
				if (word == 0 && value < m_allocation.folded_effective_addresses.size() &&
					m_allocation.folded_effective_addresses[value] != 0)
				{
					return false;
				}

				bool sign_extend = false;
				if ((RematerializedWordMask(value) & (1u << word)) != 0)
				{
					const RegionAllocation::RematerializationKind kind =
						Rematerialization(value);
					if (word != 1 || kind ==
						RegionAllocation::RematerializationKind::None)
					{
						return false;
					}
					if (kind == RegionAllocation::RematerializationKind::ZeroExtendLow32)
					{
						output->source = PersistentColdWordSource::Immediate;
						output->immediate = 0;
						return true;
					}
					sign_extend = true;
					word = 0;
				}

				const ValueId storage = StorageValue(value);
				const RegionAllocation::Location& location = Location(storage);
				auto apply_sign = [&](PersistentColdWordSource direct,
					PersistentColdWordSource extended) {
					output->source = sign_extend ? extended : direct;
					return true;
				};
				switch (location.kind)
				{
					case RegionAllocation::LocationKind::Immediate:
					{
						u32 immediate = 0;
						if (!ImmediateWord(storage, word, &immediate))
							return false;
						output->source = PersistentColdWordSource::Immediate;
						output->immediate = sign_extend ?
							static_cast<u32>(static_cast<s32>(immediate) >> 31) :
							immediate;
						return true;
					}
					case RegionAllocation::LocationKind::CanonicalState:
					{
						const size_t canonical =
							RegionExecution::CanonicalStateWordOffset(location.index, word);
						PersistentStateLocation runtime{};
						if (canonical == SIZE_MAX || !PersistentRuntimeLocation(
								static_cast<u32>(canonical), &runtime))
						{
							return false;
						}
						output->source_offset = runtime.offset;
						if (runtime.base == CompileOptions::PersistentStateBase::Vu0)
							return apply_sign(PersistentColdWordSource::Vu0,
								PersistentColdWordSource::SignExtendVu0);
						return apply_sign(PersistentColdWordSource::CpuRegisters,
							PersistentColdWordSource::SignExtendCpuRegisters);
					}
					case RegionAllocation::LocationKind::Core:
					case RegionAllocation::LocationKind::FixedCycle:
						if (!ColdCoreSnapshotOffset(CoreRegister(location, word),
								&output->source_offset))
							return false;
						return apply_sign(PersistentColdWordSource::Frame,
							PersistentColdWordSource::SignExtendFrame);
					case RegionAllocation::LocationKind::Spill:
					{
						const u32 offset = SpillOffset(location, word);
						if (offset > UINT16_MAX)
							return false;
						output->source_offset = static_cast<u16>(offset);
						return apply_sign(PersistentColdWordSource::Frame,
							PersistentColdWordSource::SignExtendFrame);
					}
					case RegionAllocation::LocationKind::VfpS:
						if (!ColdVfpSnapshotOffset(
								m_allocation.first_vfp_s + location.index,
								&output->source_offset))
							return false;
						return apply_sign(PersistentColdWordSource::Frame,
							PersistentColdWordSource::SignExtendFrame);
					case RegionAllocation::LocationKind::NeonQ:
					{
						unsigned physical_q = location.index;
						if (m_remap_allocated_neon_high_bank &&
							location.index >= m_neon_remap_first_q &&
							location.index < m_neon_remap_first_q +
								m_neon_remap_q_count)
						{
							physical_q = m_neon_remap_physical_first_q +
								location.index -
								m_neon_remap_first_q;
						}
						if (!ColdVfpSnapshotOffset(physical_q * 4 + word,
								&output->source_offset))
							return false;
						return apply_sign(PersistentColdWordSource::Frame,
							PersistentColdWordSource::SignExtendFrame);
					}
					case RegionAllocation::LocationKind::None:
						return false;
				}
				return false;
			}

			bool BuildPersistentColdDescriptor(
				const RegionExecution::ExitSite& site,
				const RegionExecution::ExitContractView& contract, u16* descriptor_index)
			{
				if (!descriptor_index || !m_use_compact_persistent_exits ||
					m_result.persistent_cold_exit_descriptors.size() >= UINT16_MAX)
				{
					return false;
				}
				const size_t first_word = m_result.persistent_cold_exit_words.size();
				for (size_t slot = 0; slot < RegionExecution::STATE_SLOT_COUNT; slot++)
				{
					if (m_persistent_scheduler_countdown &&
						RegionExecution::DecodeStateSlot(slot).state_class ==
							RegionExecution::StateClass::Cycle)
					{
						continue;
					}
					const u8 mask = site.dirty_words[slot];
					if (site.dirty_state.test(slot) != (mask != 0))
					{
						m_result.persistent_cold_exit_words.resize(first_word);
						return false;
					}
					const ValueId value = RegionExecution::StateValue(
						*contract.state, slot);
					for (u8 word = 0; word < 4; word++)
					{
						if ((mask & (1u << word)) == 0)
							continue;
						const size_t canonical =
							RegionExecution::CanonicalStateWordOffset(slot, word);
						PersistentStateLocation destination{};
						PersistentColdWord compact{};
						if (canonical == SIZE_MAX || !PersistentRuntimeLocation(
								static_cast<u32>(canonical), &destination) ||
							!BuildPersistentColdWordSource(value, word, &compact))
						{
							m_result.persistent_cold_exit_words.resize(first_word);
							return false;
						}
						compact.destination_base = destination.base;
						compact.destination_offset = destination.offset;
						m_result.persistent_cold_exit_words.push_back(compact);
					}
				}
				const size_t word_count =
					m_result.persistent_cold_exit_words.size() - first_word;
				if (first_word > UINT32_MAX || word_count > UINT16_MAX)
				{
					m_result.persistent_cold_exit_words.resize(first_word);
					return false;
				}
				*descriptor_index = static_cast<u16>(
					m_result.persistent_cold_exit_descriptors.size());
				m_result.persistent_cold_exit_descriptors.push_back({
					static_cast<u32>(first_word), static_cast<u16>(word_count),
					nullptr, nullptr});
				return true;
			}

			bool EmitCompactPersistentExit(
				const RegionExecution::ExitSite& site, const ColdExit& persistent_exit,
				const RegionExecution::ExitContractView& contract, u32 resume_pc,
				bool dynamic_resume_pc, bool* handled)
			{
				if (!handled)
					return false;
				*handled = false;
				if (!m_use_compact_persistent_exits ||
					(!dynamic_resume_pc &&
					 persistent_exit.reason == ExitReason::RegionBoundary &&
					 CompatibleTargetSignature(resume_pc)))
				{
					return true;
				}
				u16 descriptor = UINT16_MAX;
				if (!BuildPersistentColdDescriptor(site, contract, &descriptor))
					return true;
				*handled = true;
				if (!EmitPersistentExitCycleState(site))
					return false;
				const size_t descriptor_pointer = m_code.Size();
				if (!m_code.EmitMovImm32Patchable(TEMP0, 0))
					return false;
				const size_t hub_branch = m_code.EmitBranchPlaceholder();
				if (hub_branch == static_cast<size_t>(-1))
					return false;

				PersistentExitPatch patch{};
				patch.reason = persistent_exit.reason;
				patch.resume_pc = resume_pc;
				patch.pending_raw_cycles = contract.pending_raw_cycles;
				patch.cycle_commit_deferred = contract.cycle_commit_deferred;
				patch.dynamic_resume_pc = dynamic_resume_pc;
				patch.compact_descriptor_pointer_patch = descriptor_pointer;
				patch.compact_descriptor_index = descriptor;
				m_compact_persistent_exits.push_back({hub_branch, patch});
				return true;
			}

			bool EmitCompactPersistentExitHub()
			{
				if (m_compact_persistent_exits.empty())
					return true;
				const size_t hub = m_code.Size();
				for (const CompactPersistentExit& exit : m_compact_persistent_exits)
				{
					if (!m_code.PatchBranch(exit.hub_branch, hub))
						return false;
				}

				if (!EmitSpillAddress(TEMP1, m_cold_core_snapshot_offset))
					return false;
				u16 core_mask = 0;
				for (const unsigned host : PERSISTENT_COLD_CORE_REGISTERS)
					core_mask |= static_cast<u16>(1u << host);
				if (!m_code.EmitStmIa(TEMP1, core_mask))
					return false;
				for (u32 scalar = 0; scalar < PERSISTENT_COLD_VFP_LOW_S_COUNT; scalar++)
				{
					const u32 offset = m_cold_vfp_snapshot_offset + scalar * sizeof(u32);
					if (offset > 0x3fcu || !m_code.EmitVstrSImm(scalar, STACK,
							static_cast<u16>(offset)))
						return false;
				}
				for (u32 dreg = 0; dreg < PERSISTENT_COLD_VFP_HIGH_S_COUNT / 2;
					dreg++)
				{
					const u32 offset = m_cold_vfp_snapshot_offset +
						PERSISTENT_COLD_VFP_LOW_S_COUNT * sizeof(u32) +
						dreg * sizeof(u64);
					if (offset > 0x3fcu || !m_code.EmitVstrDImm(
							PERSISTENT_COLD_VFP_HIGH_S_FIRST / 2 + dreg,
							STACK, static_cast<u16>(offset)))
						return false;
				}

				// r0 arrives with the immutable descriptor pointer.  All allocated
				// caller-saved words and every VFP/NEON lane have now been captured;
				// construct a normal AAPCS call frame only on this cold path.
				if (!EmitMove(CYCLE_LOW, STACK) || !EmitMove(CYCLE_HIGH, CPU_REGS) ||
					!m_code.EmitMovImm32(FIRST_ALLOCATED_CORE, static_cast<u32>(
						reinterpret_cast<uptr>(m_options.persistent_dispatch->vu0_state))) ||
					!m_code.EmitCallAbsolute(
						reinterpret_cast<const void*>(&MaterializePersistentColdExit), TEMP1) ||
					!EmitFrameAdjustment(false) || !m_code.EmitBx(TEMP0))
				{
					return false;
				}

				for (CompactPersistentExit& exit : m_compact_persistent_exits)
				{
					exit.patch.branch_offset = m_code.Size();
					if (m_code.EmitBranchPlaceholder() == static_cast<size_t>(-1))
						return false;
					m_result.persistent_exit_patches.push_back(exit.patch);
					m_result.cold_exit_leaves++;
				}
				m_result.cold_exit_epilogues++;
				return true;
			}

			bool EmitExitState(const RegionExecution::ExitSite& site,
				const ColdExit* persistent_exit)
			{
				RegionExecution::ExitContractView contract{};
				if (!RegionExecution::ResolveExitContract(
						m_program, site, &contract) || !contract.state)
				{
					return false;
				}
				if (!EmitPersistentExitCycleState(site))
					return false;
				u32 resume_pc = 0;
				bool dynamic_resume_pc = false;
				if (!ResolvePersistentResumePc(site, contract, &resume_pc,
						&dynamic_resume_pc))
					return false;
				const GprLinkSignature* const compatible_target =
					!dynamic_resume_pc && Persistent() && persistent_exit &&
					persistent_exit->reason == ExitReason::RegionBoundary ?
						CompatibleTargetSignature(resume_pc) : nullptr;

				auto emit_dirty_state = [&](bool include_compatible_writeback) {
					for (size_t slot = 0;
						slot < RegionExecution::STATE_SLOT_COUNT; slot++)
					{
						if (m_persistent_scheduler_countdown &&
							RegionExecution::DecodeStateSlot(slot).state_class ==
								RegionExecution::StateClass::Cycle)
						{
							continue;
						}
						const u8 mask = site.dirty_words[slot];
						if (site.dirty_state.test(slot) != (mask != 0))
							return false;
						const ValueId value = RegionExecution::StateValue(
							*contract.state, slot);
						for (u8 word = 0; word < 4; word++)
						{
							if ((mask & (1u << word)) == 0 ||
								(!include_compatible_writeback &&
								 CompatibleTargetDefersCanonicalStore(
									 compatible_target, slot, word)))
							{
								continue;
							}
							const size_t offset =
								RegionExecution::CanonicalStateWordOffset(slot, word);
							if (offset == SIZE_MAX ||
								!EmitLoadValueWord(value, word, TEMP0) ||
								!EmitStateStore(static_cast<u32>(offset), TEMP0))
							{
								return false;
							}
						}
					}
					return true;
				};

				if (Persistent())
				{
					if (!persistent_exit)
						return false;
					if (persistent_exit->reason ==
							ExitReason::ProfitabilityFallback &&
						!EmitPreEntryFallbackAccounting(
							ExitReason::ProfitabilityFallback))
					{
						return false;
					}

					if (compatible_target)
					{
						// Keep the selector before either frame adjustment. Target
						// retirement can then repatch it to this owner's canonical leaf;
						// spills and allocated values are still available there. The fast
						// leaf stores only dirty words which the outgoing signature does
						// not carry, seeds its exact host mappings, and leaves canonical
						// write-back words resident.
						const size_t selector = m_code.EmitBranchPlaceholder();
						if (selector == static_cast<size_t>(-1))
							return false;

						const size_t canonical_leaf = m_code.Size();
						if (!emit_dirty_state(true) || !EmitFrameAdjustment(false))
							return false;
						const size_t canonical_branch =
							m_code.EmitBranchPlaceholder();
						if (canonical_branch == static_cast<size_t>(-1))
							return false;

						const size_t compatible_leaf = m_code.Size();
						if (!emit_dirty_state(false) ||
							!EmitCompatibleExitSeeds(site, contract,
								*compatible_target) ||
							!EmitFrameAdjustment(false))
						{
							return false;
						}
						const size_t compatible_branch =
							m_code.EmitBranchPlaceholder();
						if (compatible_branch == static_cast<size_t>(-1) ||
							!m_code.PatchBranch(selector, compatible_leaf))
						{
							return false;
						}

						PersistentExitPatch patch{};
						patch.branch_offset = compatible_branch;
						patch.selector_branch_offset = selector;
						patch.canonical_leaf_offset = canonical_leaf;
						patch.canonical_branch_offset = canonical_branch;
						patch.compatible_leaf_offset = compatible_leaf;
						patch.reason = persistent_exit->reason;
						patch.resume_pc = resume_pc;
						patch.pending_raw_cycles = contract.pending_raw_cycles;
						patch.cycle_commit_deferred =
							contract.cycle_commit_deferred;
						patch.dynamic_resume_pc = dynamic_resume_pc;
						patch.compatible_signature = *compatible_target;
						m_result.persistent_exit_patches.push_back(patch);
						return true;
					}

					if (!emit_dirty_state(true))
						return false;
					if (!EmitFrameAdjustment(false))
						return false;
					const size_t branch = m_code.EmitBranchPlaceholder();
					if (branch == static_cast<size_t>(-1))
						return false;
					PersistentExitPatch patch{};
					patch.branch_offset = branch;
					patch.reason = persistent_exit->reason;
					patch.resume_pc = resume_pc;
					patch.pending_raw_cycles = contract.pending_raw_cycles;
					patch.cycle_commit_deferred =
						contract.cycle_commit_deferred;
					patch.dynamic_resume_pc = dynamic_resume_pc;
					m_result.persistent_exit_patches.push_back(patch);
					return true;
				}

				if (!emit_dirty_state(true))
					return false;
				if (!m_code.EmitMovImm8(TEMP0, 1) ||
					!m_code.EmitStrImm12(TEMP0, CONTEXT,
						static_cast<u16>(ResultOffset(offsetof(ExecutionResult, completed)))) ||
					!m_code.EmitMovImm8(TEMP0,
						contract.cycle_commit_deferred ? 1 : 0) ||
					!m_code.EmitStrImm12(TEMP0, CONTEXT,
						static_cast<u16>(ResultOffset(offsetof(
							ExecutionResult, cycle_commit_deferred)))) ||
					!m_code.EmitMovImm32(TEMP0, contract.pending_raw_cycles) ||
					!m_code.EmitStrImm12(TEMP0, CONTEXT,
						static_cast<u16>(ResultOffset(offsetof(
							ExecutionResult, pending_raw_cycles)))) ||
					!EmitFrameAdjustment(false) ||
					!m_code.EmitVpopDRange(8, 8) ||
					!m_code.EmitMovImm8(TEMP0, 1) ||
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
			RegionExecution::Plan m_execution{};
			RegionAllocation::Plan m_allocation{};
			RegionMemoryPlan::Plan m_memory_plan{};
			RegionMemoryPlan::A9ProfitabilityCertificate
				m_profitability_certificate{};
			std::vector<const Node*> m_definitions;
			std::vector<ValueId> m_memory_value_for_effect;
			std::vector<ValueId> m_region_resident_memory_load_values;
			std::vector<u8> m_cop1_normalized;
			std::vector<ValueId> m_cop1_fused_flag_for_clamp;
			std::vector<ValueId> m_cop1_fused_clamp_for_flag;
			std::vector<size_t> m_cop1_normalize_edge_calls;
			std::vector<size_t> m_cop1_ou_edge_calls;
			std::vector<size_t> m_block_offsets;
			std::vector<InternalPatch> m_internal_patches;
			std::vector<ColdExit> m_cold_exits;
			std::vector<Vu0StatusColdPath> m_vu0_status_cold_paths;
			std::vector<Vu0MacColdPath> m_vu0_mac_cold_paths;
			std::vector<Cop1GuardedColdPath> m_cop1_guarded_cold_paths;
			std::vector<PreEntryFallback> m_pre_entry_fallbacks;
			std::vector<CompactPersistentExit> m_compact_persistent_exits;
			std::vector<LiteralPatch> m_literal_patches;
			std::vector<HoistedMemoryBase> m_hoisted_memory_bases;
			std::vector<HoistedMemoryLoad> m_hoisted_memory_loads;
			std::vector<ValueId> m_hoisted_memory_operations;
			std::vector<u64> m_block_cycle_debt;
			u32 m_work_scratch_offset = 0;
			u32 m_work_scratch_bytes = 16;
			u32 m_saved_vtlb_vmap_offset = 0;
			std::vector<u32> m_block_layout;
			std::vector<u32> m_layout_successor;
			u32 m_cycle_snapshot_offset = 0;
			u32 m_cold_core_snapshot_offset = 0;
			u32 m_cold_vfp_snapshot_offset = 0;
			u32 m_frame_bytes = 0;
			std::vector<size_t> m_optional_frame_adjustments;
			bool m_work_scratch_used = false;
			bool m_use_compact_persistent_exits = false;
			bool m_persistent_scheduler_countdown = false;
			bool m_reclaim_vtlb_vmap = false;
			bool m_reclaim_vtlb_host_base = false;
			bool m_memory_free_vtlb_registers_available = false;
			bool m_remap_allocated_neon_high_bank = false;
			bool m_vu0_product_scratch_available = false;
			u8 m_neon_remap_first_q = 0;
			u8 m_neon_remap_physical_first_q = 0;
			u8 m_neon_remap_q_count = 0;
			bool m_aggregate_cycle_edges = false;
			bool m_pre_entry_vu0_idle_proof = false;
			SemanticKernel::Plan m_semantic_plan{};
			std::array<unsigned, 4> m_semantic_affine_work_q{};
			bool m_emit_semantic_kernel = false;
			bool m_emit_semantic_search_island = false;
			size_t m_canonical_entry_event_patch = static_cast<size_t>(-1);
		};
	} // namespace

	CompileResult CompileAllocated(const Program& program,
		VitaA32::CodeBuffer& code, const CompileOptions& options)
	{
		return AllocatedCompiler(program, code, options).Run();
	}
} // namespace VitaEE::RegionA32
