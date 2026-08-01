// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#include "PrecompiledHeader.h"

#include "pcsx2/vita/A32Emitter.h"
#include "pcsx2/vita/VitaEeRegionA32.h"

#include <algorithm>
#include <array>
#include <cstddef>
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
		static_assert(sizeof(ExecutionContext) == 52);
#endif

		u32 Rs(u32 opcode) { return (opcode >> 21) & 0x1f; }
		u32 Rt(u32 opcode) { return (opcode >> 16) & 0x1f; }
		u32 Rd(u32 opcode) { return (opcode >> 11) & 0x1f; }
		s16 Immediate(u32 opcode) { return static_cast<s16>(opcode); }

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
				primary == 0x23)
			{
				return true; // ADDIU, SLTI, ANDI, LW.
			}
			return primary == 0 && (opcode & 0x3f) == 0x2b &&
			       ((opcode >> 6) & 0x1f) == 0; // SLTU.
		}

		struct GuestPair
		{
			u8 low = 0xff;
			u8 high = 0xff;
			bool valid = false;
		};

		struct InternalPatch
		{
			size_t offset = static_cast<size_t>(-1);
			u32 target_block = INVALID_BLOCK;
		};

		struct ColdExit
		{
			size_t branch_offset = static_cast<size_t>(-1);
			Condition condition = Condition::AL;
			ExitReason reason = ExitReason::RegionBoundary;
			u32 pc = 0;
			u32 pending_raw_cycles = 0;
			bool cycle_commit_deferred = false;
			bool publish_memory_address = false;
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
			}

			CompileResult Run()
			{
				m_code.Reset();
				const VerifyResult verified = Verify(m_program);
				if (!verified)
					return Fail(CompileFailure::InvalidProgram,
						verified.block < m_program.blocks.size() ? m_program.blocks[verified.block].pc : 0);
				if (m_program.source_blocks.empty())
					return Fail(CompileFailure::UnattestedSource,
						m_program.blocks[m_program.entry_block].pc);
				if (m_options.max_mapped_gprs == 0 ||
					m_options.max_mapped_gprs > m_result.mapped_gprs.size())
				{
					return Fail(CompileFailure::RegisterPressure,
						m_program.blocks[m_program.entry_block].pc);
				}
				if (!AnalyzeSurface())
					return m_result;

				m_block_offsets.assign(m_program.blocks.size(), static_cast<size_t>(-1));
				if (!EmitPrologue())
					return Fail(CompileFailure::Emission,
						m_program.blocks[m_program.entry_block].pc);

				if (!EmitEntryEventCheck())
					return m_result;
				const size_t entry_branch = m_code.EmitBranchPlaceholder();
				if (entry_branch == static_cast<size_t>(-1))
					return Fail(CompileFailure::Emission,
						m_program.blocks[m_program.entry_block].pc);

				for (u32 block_index = 0; block_index < m_program.blocks.size();
					 block_index++)
				{
					m_block_offsets[block_index] = m_code.Size();
					if (!EmitBlock(block_index))
						return m_result;
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

				for (const ColdExit& exit : m_cold_exits)
				{
					const size_t target = m_code.Size();
					if (!m_code.PatchBranch(exit.branch_offset, target, exit.condition) ||
						!EmitExit(exit))
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
				m_result.host_instructions = static_cast<u32>(
					m_code.AnalyzeGeneratedCode(CONTEXT).host_instructions);
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

			bool AddGuest(u32 guest, u32 pc)
			{
				if (guest == 0 || m_guest_pairs[guest].valid)
					return true;
				if (m_result.mapped_gpr_count >= m_options.max_mapped_gprs)
				{
					Fail(CompileFailure::RegisterPressure, pc);
					return false;
				}
				const u8 index = m_result.mapped_gpr_count++;
				m_result.mapped_gprs[index] = static_cast<u8>(guest);
				m_guest_pairs[guest] = {static_cast<u8>(FIRST_GPR_HOST + index * 2),
					static_cast<u8>(FIRST_GPR_HOST + index * 2 + 1),
					true};
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
					return AddGuest(Rs(opcode), pc) && AddGuest(Rt(opcode), pc);
				}

				if (!IsSupportedBody(opcode))
				{
					Fail(CompileFailure::UnsupportedInstruction, pc);
					return false;
				}
				if (opcode == 0)
					return true;
				const u32 primary = opcode >> 26;
				if (primary == 0x23)
				{
					m_result.memory_loads++;
					return AddGuest(Rs(opcode), pc) && AddGuest(Rt(opcode), pc);
				}
				if (primary == 0)
				{
					return AddGuest(Rs(opcode), pc) && AddGuest(Rt(opcode), pc) &&
					       AddGuest(Rd(opcode), pc);
				}
				return AddGuest(Rs(opcode), pc) && AddGuest(Rt(opcode), pc);
			}

			bool AnalyzeSurface()
			{
				for (const Block& block : m_program.blocks)
				{
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
						if (node.opcode == Opcode::MemoryStore ||
							(node.opcode == Opcode::MemoryLoad &&
								node.immediate != static_cast<u32>(MemoryAccessKind::LoadS32)))
						{
							Fail(CompileFailure::UnsupportedMemory, node.source_pc);
							return false;
						}
					}
				}
				return true;
			}

			const GuestPair& Pair(u32 guest) const { return m_guest_pairs[guest]; }

			bool EmitMove(unsigned destination, unsigned source)
			{
				return m_code.EmitMovRegShiftImm(destination, source, ShiftType::LSL, 0);
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
				for (u32 index = 0; index < m_result.mapped_gpr_count; index++)
				{
					const u32 guest = m_result.mapped_gprs[index];
					const GuestPair& pair = Pair(guest);
					const size_t offset = StateGprLowOffset(guest);
					if (!m_code.EmitLdrImm12(pair.low, RETURN_VALUE,
							static_cast<u16>(offset)) ||
						!m_code.EmitLdrImm12(pair.high, RETURN_VALUE,
							static_cast<u16>(offset + sizeof(u32))))
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

			bool AppendColdBranch(Condition condition, ExitReason reason, u32 pc,
				u32 pending_raw_cycles = 0, bool deferred = false,
				bool memory_address = false)
			{
				const size_t branch = m_code.EmitBranchPlaceholder(condition);
				if (branch == static_cast<size_t>(-1))
					return false;
				m_cold_exits.push_back({branch, condition, reason, pc, pending_raw_cycles,
					deferred, memory_address});
				return true;
			}

			bool EmitEntryEventCheck()
			{
				return EmitUnsigned64AtLeastNextEvent() &&
				       AppendColdBranch(Condition::CS, ExitReason::EventHorizon,
						   m_program.blocks[m_program.entry_block].pc);
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
				const GuestPair& input = Pair(Rs(opcode));
				// PCSX2 owner: R5900OpcodeImpl.cpp::ADDIU() wraps RS.low32 plus
				// the signed immediate, then sign-extends that 32-bit result. ADDIU
				// is not DADDIU; carrying into RS bits 32..63 is architecturally
				// wrong even though ordinary pointer induction rarely exposes it.
				return m_code.EmitMovImm32(RETURN_VALUE, static_cast<u32>(immediate)) &&
				       m_code.EmitAddReg(output.low, input.low, RETURN_VALUE) &&
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
				const GuestPair& input = Pair(Rs(opcode));
				return m_code.EmitAndImm32(output.low, input.low, opcode & 0xffffu) &&
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

			bool EmitLoadWord(const Block& block, const SourceInstruction& source)
			{
				const u32 opcode = source.opcode;
				const u32 base_guest = Rs(opcode);
				const s32 immediate = Immediate(opcode);
				if (base_guest == 0)
				{
					if (!m_code.EmitMovImm32(RETURN_VALUE, static_cast<u32>(immediate)))
						return false;
				}
				else
				{
					const GuestPair& base = Pair(base_guest);
					if (immediate == 0)
					{
						if (!EmitMove(RETURN_VALUE, base.low))
							return false;
					}
					else if (!m_code.EmitAddImm32(RETURN_VALUE, base.low,
								 static_cast<u32>(immediate)))
					{
						return false;
					}
				}

				const u32 pending = PendingRawBefore(block, source.pc);
				if (!m_code.EmitTstImm32(RETURN_VALUE, 3) ||
					!AppendColdBranch(Condition::NE, ExitReason::MemoryAlignment, source.pc,
						pending, true, true) ||
					!m_code.EmitLdrImm12(SCRATCH, CONTEXT,
						static_cast<u16>(offsetof(
							ExecutionContext, identity_main_ram_limit))) ||
					!m_code.EmitCmpReg(RETURN_VALUE, SCRATCH))
				{
					return false;
				}
				const size_t identity = m_code.EmitBranchPlaceholder(Condition::CC);
				if (identity == static_cast<size_t>(-1) ||
					!m_code.EmitLdrImm12(
						SCRATCH, CONTEXT,
						static_cast<u16>(offsetof(ExecutionContext, vmap))) ||
					!m_code.EmitMovRegShiftImm(LINK_SCRATCH, RETURN_VALUE, ShiftType::LSR,
						12) ||
					!m_code.EmitLdrRegShift(SCRATCH, SCRATCH, LINK_SCRATCH, ShiftType::LSL,
						2) ||
					!m_code.EmitAddReg(SCRATCH, SCRATCH, RETURN_VALUE, true) ||
					!AppendColdBranch(Condition::MI, ExitReason::MemoryHandler, source.pc,
						pending, true, true) ||
					!m_code.EmitLdrImm12(
						LINK_SCRATCH, CONTEXT,
						static_cast<u16>(offsetof(ExecutionContext, host_memory_base))) ||
					!m_code.EmitAddReg(SCRATCH, SCRATCH, LINK_SCRATCH) ||
					!m_code.EmitLdrImm12(
						LINK_SCRATCH, CONTEXT,
						static_cast<u16>(offsetof(ExecutionContext, main_ram))) ||
					!m_code.EmitCmpReg(SCRATCH, LINK_SCRATCH) ||
					!AppendColdBranch(Condition::CC, ExitReason::MemoryTranslation,
						source.pc, pending, true, true) ||
					!m_code.EmitLdrImm12(
						LINK_SCRATCH, CONTEXT,
						static_cast<u16>(offsetof(ExecutionContext, main_ram_last_word))) ||
					!m_code.EmitCmpReg(SCRATCH, LINK_SCRATCH) ||
					!AppendColdBranch(Condition::HI, ExitReason::MemoryTranslation,
						source.pc, pending, true, true))
				{
					return false;
				}

				const size_t translated = m_code.EmitBranchPlaceholder();
				const size_t identity_target = m_code.Size();
				if (translated == static_cast<size_t>(-1) ||
					!m_code.PatchBranch(identity, identity_target, Condition::CC) ||
					!m_code.EmitLdrImm12(
						SCRATCH, CONTEXT,
						static_cast<u16>(offsetof(ExecutionContext, main_ram))) ||
					!m_code.EmitAddReg(SCRATCH, SCRATCH, RETURN_VALUE) ||
					!m_code.PatchBranch(translated, m_code.Size()))
				{
					return false;
				}

				const u32 destination = Rt(opcode);
				if (destination == 0)
					return m_code.EmitLdrImm12(LINK_SCRATCH, SCRATCH, 0);
				const GuestPair& output = Pair(destination);
				return m_code.EmitLdrImm12(output.low, SCRATCH, 0) &&
				       m_code.EmitMovRegShiftImm(output.high, output.low, ShiftType::ASR,
						   31);
			}

			bool EmitBodyInstruction(const Block& block,
				const SourceInstruction& source)
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
						return EmitLoadWord(block, source);
					case 0:
						return EmitSltu(opcode);
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
				if (!transfer.cycle_commit_deferred && !EmitAddCycles(scaled_cycles))
					return false;
				u32 pc = 0;
				if (!ResolveTransferPc(block, transfer, &pc))
					return false;
				if (transfer.event_horizon_check)
				{
					if (!EmitUnsigned64AtLeastNextEvent() ||
						!AppendColdBranch(Condition::CS, ExitReason::EventHorizon, pc))
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
				return EmitExit({static_cast<size_t>(-1), Condition::AL,
					transfer.external_reason, pc, transfer.pending_raw_cycles,
					transfer.cycle_commit_deferred, false});
			}

			Condition TakenCondition(u32 opcode) const
			{
				const u32 primary = opcode >> 26;
				return primary == 0x04 || primary == 0x14 ? Condition::EQ : Condition::NE;
			}

			bool EmitBlock(u32 block_index)
			{
				const Block& block = m_program.blocks[block_index];
				if (block.terminator.kind == TerminatorKind::Transfer)
				{
					for (const SourceInstruction& source : block.source)
					{
						if (!EmitBodyInstruction(block, source))
							return static_cast<bool>(Fail(CompileFailure::Emission, source.pc));
					}
					return EmitEdge(block, block.terminator.taken, block.scaled_cycle_cost);
				}

				if (block.source.size() < 2)
					return false;
				const size_t body_count = block.source.size() - 2;
				for (size_t index = 0; index < body_count; index++)
				{
					if (!EmitBodyInstruction(block, block.source[index]))
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

				if (!block.terminator.likely && !EmitBodyInstruction(block, delay))
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
					!EmitBodyInstruction(block, delay) ||
					!EmitEdge(block, block.terminator.taken, block.scaled_cycle_cost))
				{
					return false;
				}
				return true;
			}

			bool EmitMaterializeState(u32 pc)
			{
				if (!m_code.EmitLdrImm12(
						RETURN_VALUE, CONTEXT,
						static_cast<u16>(offsetof(ExecutionContext, state))))
				{
					return false;
				}
				for (u32 index = 0; index < m_result.mapped_gpr_count; index++)
				{
					const u32 guest = m_result.mapped_gprs[index];
					const GuestPair& pair = Pair(guest);
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
				       m_code.EmitMovImm32(SCRATCH, pc) &&
				       m_code.EmitStrImm12(SCRATCH, RETURN_VALUE,
						   static_cast<u16>(offsetof(CanonicalState, pc)));
			}

			bool EmitExit(const ColdExit& exit)
			{
				// Memory exits arrive with the exact guest effective address in r0.
				// Materialization needs r0 for the canonical-state pointer, so retain
				// the address in the otherwise-dead saved link register first.
				if (exit.publish_memory_address && !EmitMove(LINK_SCRATCH, RETURN_VALUE))
				{
					return false;
				}

				if (!EmitMaterializeState(exit.pc) || !m_code.EmitMovImm8(SCRATCH, 1) ||
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
			std::array<GuestPair, 32> m_guest_pairs{};
			std::vector<size_t> m_block_offsets;
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
		}
		return "unknown";
	}
} // namespace VitaEE::RegionA32
