// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#include "PrecompiledHeader.h"

#include "R5900OpcodeTables.h"
#include "pcsx2/vita/VitaEeRegionIR.h"

#include <algorithm>
#include <bit>
#include <deque>
#include <limits>
#include <map>
#include <set>
#include <utility>

namespace VitaEE::RegionIR
{
	namespace
	{
		constexpr u32 GPR_COUNT = 32;
		constexpr u32 HI_PARAMETER = 32;
		constexpr u32 LO_PARAMETER = 33;
		constexpr u32 CYCLE_PARAMETER = 34;
		constexpr u32 MEMORY_EFFECT_PARAMETER = 35;
		constexpr u32 PARAMETER_COUNT = 36;

		constexpr u32 RS(u32 op) { return (op >> 21) & 0x1fu; }
		constexpr u32 RT(u32 op) { return (op >> 16) & 0x1fu; }
		constexpr u32 RD(u32 op) { return (op >> 11) & 0x1fu; }
		constexpr u32 SA(u32 op) { return (op >> 6) & 0x1fu; }
		constexpr u32 FUNCT(u32 op) { return op & 0x3fu; }
		constexpr s16 IMM_S(u32 op) { return static_cast<s16>(op); }
		constexpr u16 IMM_U(u32 op) { return static_cast<u16>(op); }

		constexpr u32 BranchTarget(u32 pc, u32 op)
		{
			return pc + 4 + static_cast<s32>(IMM_S(op)) * 4;
		}

		constexpr u32 JumpTarget(u32 pc, u32 op)
		{
			return ((pc + sizeof(u32)) & 0xf0000000u) |
			       ((op & 0x03ffffffu) << 2);
		}

		bool IsConditionalBranch(u32 op)
		{
			switch (op >> 26)
			{
				case 0x01:
				{
					const u32 rt = RT(op);
					const bool supported = rt <= 0x03 ||
					                       (rt >= 0x10 && rt <= 0x13);
					// SCE defines a linked REGIMM using r31 as its source as
					// undefined. Leave that pair to the tier-zero provider.
					return supported && !(rt >= 0x10 && RS(op) == 31);
				}
				case 0x04: // BEQ
				case 0x05: // BNE
				case 0x06: // BLEZ
				case 0x07: // BGTZ
				case 0x14: // BEQL
				case 0x15: // BNEL
				case 0x16: // BLEZL
				case 0x17: // BGTZL
					return true;
				default:
					return false;
			}
		}

		bool IsStaticJump(u32 op)
		{
			const u32 primary = op >> 26;
			return primary == 0x02 || primary == 0x03;
		}

		bool CanLowerStaticJump(u32 op, const LiftOptions& options)
		{
			return IsStaticJump(op) && !options.goemon_tlb_hack;
		}

		bool IsLinkedControl(u32 op)
		{
			return (R5900::GetInstruction(op).flags & IS_LINKED) != 0;
		}

		bool IsLikelyBranch(u32 op)
		{
			return IsConditionalBranch(op) &&
			       (R5900::GetInstruction(op).flags & IS_LIKELY) != 0;
		}

		bool IsAnyControlFlow(u32 op)
		{
			return (R5900::GetInstruction(op).flags & IS_BRANCH) != 0;
		}

		bool CanLowerSpecial(u32 op)
		{
			switch (FUNCT(op))
			{
				case 0x00: // SLL
				case 0x02: // SRL
				case 0x03: // SRA
				case 0x21: // ADDU
				case 0x23: // SUBU
				case 0x24: // AND
				case 0x25: // OR
				case 0x26: // XOR
				case 0x27: // NOR
				case 0x2a: // SLT
				case 0x2b: // SLTU
				case 0x2d: // DADDU
				case 0x2f: // DSUBU
				case 0x38: // DSLL
				case 0x3a: // DSRL
				case 0x3b: // DSRA
				case 0x3c: // DSLL32
				case 0x3e: // DSRL32
				case 0x3f: // DSRA32
					return true;
				default:
					return false;
			}
		}

		bool CanLowerPureNonBranch(u32 op)
		{
			if (IsAnyControlFlow(op))
				return false;

			switch (op >> 26)
			{
				case 0x00:
					return CanLowerSpecial(op);
				case 0x09: // ADDIU
				case 0x0a: // SLTI
				case 0x0b: // SLTIU
				case 0x0c: // ANDI
				case 0x0d: // ORI
				case 0x0e: // XORI
				case 0x0f: // LUI
				case 0x19: // DADDIU
					return true;
				default:
					return false;
			}
		}

		bool DecodeMemoryAccess(u32 op, MemoryAccessKind* kind)
		{
			MemoryAccessKind decoded{};
			switch (op >> 26)
			{
				case 0x20:
					decoded = MemoryAccessKind::LoadS8;
					break;
				case 0x24:
					decoded = MemoryAccessKind::LoadU8;
					break;
				case 0x21:
					decoded = MemoryAccessKind::LoadS16;
					break;
				case 0x25:
					decoded = MemoryAccessKind::LoadU16;
					break;
				case 0x23:
					decoded = MemoryAccessKind::LoadS32;
					break;
				case 0x27:
					decoded = MemoryAccessKind::LoadU32;
					break;
				case 0x37:
					decoded = MemoryAccessKind::Load64;
					break;
				case 0x1e:
					decoded = MemoryAccessKind::Load128;
					break;
				case 0x28:
					decoded = MemoryAccessKind::Store8;
					break;
				case 0x29:
					decoded = MemoryAccessKind::Store16;
					break;
				case 0x2b:
					decoded = MemoryAccessKind::Store32;
					break;
				case 0x3f:
					decoded = MemoryAccessKind::Store64;
					break;
				case 0x1f:
					decoded = MemoryAccessKind::Store128;
					break;
				default:
					return false;
			}
			if (kind)
				*kind = decoded;
			return true;
		}

		bool IsMemoryLoad(MemoryAccessKind kind)
		{
			return kind <= MemoryAccessKind::Load128;
		}

		u32 MemoryAlignmentMask(MemoryAccessKind kind)
		{
			switch (kind)
			{
				case MemoryAccessKind::LoadS16:
				case MemoryAccessKind::LoadU16:
				case MemoryAccessKind::Store16:
					return 1;
				case MemoryAccessKind::LoadS32:
				case MemoryAccessKind::LoadU32:
				case MemoryAccessKind::Store32:
					return 3;
				case MemoryAccessKind::Load64:
				case MemoryAccessKind::Store64:
					return 7;
				default:
					return 0;
			}
		}

		bool IsQuadMemoryAccess(MemoryAccessKind kind)
		{
			return kind == MemoryAccessKind::Load128 ||
			       kind == MemoryAccessKind::Store128;
		}

		bool CanLowerNonBranch(u32 op)
		{
			return CanLowerPureNonBranch(op) || DecodeMemoryAccess(op, nullptr);
		}

		bool IsExceptionCapableInstruction(u32 op)
		{
			// SCE EE Core Instruction Set Manual sections 6.3.4 and the
			// per-instruction exception lists define this surface. PCSX2's
			// executable owners are R5900OpcodeImpl.cpp::{_add32_Overflow,
			// _add64_Overflow,SYSCALL,BREAK,trap}. Memory exceptions remain under
			// the separate typed memory contract.
			switch (op >> 26)
			{
				case 0x00: // SPECIAL
					switch (FUNCT(op))
					{
						case 0x0c: // SYSCALL
						case 0x0d: // BREAK
						case 0x20: // ADD
						case 0x22: // SUB
						case 0x2c: // DADD
						case 0x2e: // DSUB
						case 0x30: // TGE
						case 0x31: // TGEU
						case 0x32: // TLT
						case 0x33: // TLTU
						case 0x34: // TEQ
						case 0x36: // TNE
							return true;
						default:
							return false;
					}
				case 0x01: // REGIMM trap-immediate family
					switch (RT(op))
					{
						case 0x08: // TGEI
						case 0x09: // TGEIU
						case 0x0a: // TLTI
						case 0x0b: // TLTIU
						case 0x0c: // TEQI
						case 0x0e: // TNEI
							return true;
						default:
							return false;
					}
				case 0x08: // ADDI
				case 0x18: // DADDI
					return true;
				default:
					return false;
			}
		}

		ExitReason ClassifyExit(u32 op)
		{
			const u32 flags = R5900::GetInstruction(op).flags;
			if (IsExceptionCapableInstruction(op))
				return ExitReason::ExceptionObserver;
			if ((flags & IS_BRANCH) != 0)
				return ExitReason::UnsupportedControlFlow;
			if ((flags & IS_MEMORY) != 0)
				return ExitReason::MemoryObserver;
			if (R5900::GetInstruction(op).interpret != nullptr)
				return ExitReason::HelperObserver;
			return ExitReason::UnsupportedOpcode;
		}

		bool ContainsPc(u32 base, u32 word_count, u32 pc)
		{
			if ((pc & 3u) != 0 || pc < base)
				return false;
			const u64 offset = static_cast<u64>(pc) - base;
			return offset < static_cast<u64>(word_count) * sizeof(u32);
		}

		u32 ReadSourceWord(u32 base, const std::vector<u32>& words, u32 pc)
		{
			return words[(pc - base) / sizeof(u32)];
		}

		ExitReason ClassifyExternalResume(u32 source_base_pc,
			const std::vector<u32>& source_words, u32 pc,
			const LiftOptions& options)
		{
			if (!ContainsPc(source_base_pc, static_cast<u32>(source_words.size()), pc))
				return ExitReason::RegionBoundary;

			const u32 op = ReadSourceWord(source_base_pc, source_words, pc);
			if (IsConditionalBranch(op) || CanLowerStaticJump(op, options))
			{
				const u32 delay_pc = pc + sizeof(u32);
				if (pc <= UINT32_MAX - sizeof(u32) &&
					ContainsPc(source_base_pc, static_cast<u32>(source_words.size()), delay_pc))
				{
					const u32 delay = ReadSourceWord(source_base_pc, source_words, delay_pc);
					if (!IsAnyControlFlow(delay) && CanLowerPureNonBranch(delay))
						return ExitReason::RegionBoundary;
				}
				return ExitReason::UnsupportedControlFlow;
			}
			if (CanLowerNonBranch(op))
				return ExitReason::RegionBoundary;
			return ClassifyExit(op);
		}

		enum class RawControlKind : u8
		{
			None,
			ConditionalBranch,
			StaticJump,
		};

		struct RawBlock
		{
			u32 pc = 0;
			std::vector<SourceInstruction> body;
			RawControlKind control_kind = RawControlKind::None;
			u32 branch_pc = 0;
			u32 branch_opcode = 0;
			SourceInstruction delay{};
			u32 transfer_pc = 0;
			ExitReason transfer_reason = ExitReason::RegionBoundary;
		};

		bool ScanRawBlock(u32 source_base_pc, const std::vector<u32>& source_words,
			const std::set<u32>& leaders, u32 start_pc, const LiftOptions& options,
			RawBlock* output,
			LiftFailure* failure, u32* failure_pc)
		{
			RawBlock raw{};
			raw.pc = start_pc;
			u32 pc = start_pc;

			for (;;)
			{
				if (pc != start_pc && leaders.contains(pc))
				{
					raw.transfer_pc = pc;
					break;
				}

				if (!ContainsPc(source_base_pc, static_cast<u32>(source_words.size()),
						pc))
				{
					raw.transfer_pc = pc;
					break;
				}

				const u32 op = ReadSourceWord(source_base_pc, source_words, pc);
				const bool conditional_branch = IsConditionalBranch(op);
				const bool static_jump = CanLowerStaticJump(op, options);
				if (conditional_branch || static_jump)
				{
					const u32 delay_pc = pc + sizeof(u32);
					if (!ContainsPc(source_base_pc, static_cast<u32>(source_words.size()),
							delay_pc))
					{
						*failure = LiftFailure::MissingDelaySlot;
						*failure_pc = pc;
						return false;
					}

					const u32 delay = ReadSourceWord(source_base_pc, source_words, delay_pc);
					if (IsAnyControlFlow(delay))
					{
						*failure = LiftFailure::BranchInDelaySlot;
						*failure_pc = delay_pc;
						return false;
					}

					// A branch and its delay slot are one architectural unit. If the
					// delay slot is not yet expressible, leave both to the existing
					// compiler/interpreter at a canonical side exit.
					if (!CanLowerPureNonBranch(delay))
					{
						raw.transfer_pc = pc;
						// The existing provider must execute the branch and its
						// unsupported delay slot as one unit. The observer at this
						// boundary is therefore the branch, not the delay opcode.
						raw.transfer_reason = ExitReason::UnsupportedControlFlow;
						break;
					}

					raw.control_kind = conditional_branch ?
					                       RawControlKind::ConditionalBranch :
					                       RawControlKind::StaticJump;
					raw.branch_pc = pc;
					raw.branch_opcode = op;
					raw.delay = {delay_pc, delay, true};
					break;
				}

				if (!CanLowerNonBranch(op))
				{
					raw.transfer_pc = pc;
					raw.transfer_reason = ClassifyExit(op);
					break;
				}

				raw.body.push_back({pc, op, false});
				if (pc > UINT32_MAX - sizeof(u32))
				{
					*failure = LiftFailure::InvalidSource;
					*failure_pc = pc;
					return false;
				}
				pc += sizeof(u32);
			}

			*output = std::move(raw);
			return true;
		}

		class Builder
		{
		public:
			explicit Builder(Program* program)
				: m_program(program)
			{
			}

			bool AllocateParameters()
			{
				for (Block& block : m_program->blocks)
				{
					for (u32 gpr = 0; gpr < GPR_COUNT; gpr++)
					{
						block.parameters.gpr[gpr] = AddNode(
							block, Opcode::Parameter, ValueType::I128, {}, 0, gpr, 0, block.pc);
						if (block.parameters.gpr[gpr] == INVALID_VALUE)
							return false;
					}
					block.parameters.hi = AddNode(block, Opcode::Parameter, ValueType::I128,
						{}, 0, HI_PARAMETER, 0, block.pc);
					block.parameters.lo = AddNode(block, Opcode::Parameter, ValueType::I128,
						{}, 0, LO_PARAMETER, 0, block.pc);
					block.parameters.cycle =
						AddNode(block, Opcode::Parameter, ValueType::Cycle, {}, 0,
							CYCLE_PARAMETER, 0, block.pc);
					block.parameters.memory_effect =
						AddNode(block, Opcode::Parameter, ValueType::MemoryEffect, {}, 0,
							MEMORY_EFFECT_PARAMETER, 0, block.pc);
					if (block.parameters.hi == INVALID_VALUE ||
						block.parameters.lo == INVALID_VALUE ||
						block.parameters.cycle == INVALID_VALUE ||
						block.parameters.memory_effect == INVALID_VALUE)
					{
						return false;
					}
				}
				return true;
			}

			ValueId AddNode(Block& block, Opcode opcode, ValueType type,
				std::array<ValueId, 3> operands, u8 operand_count,
				u32 immediate, u64 literal, u32 source_pc)
			{
				if (m_next_value == INVALID_VALUE)
					return INVALID_VALUE;
				const ValueId id = m_next_value++;
				block.nodes.push_back({id, opcode, type, operands, operand_count, immediate,
					literal, source_pc});
				return id;
			}

			ValueId Unary(Block& block, Opcode opcode, ValueType type, ValueId operand,
				u32 source_pc, u32 immediate = 0)
			{
				return AddNode(block, opcode, type, {operand, INVALID_VALUE, INVALID_VALUE},
					1, immediate, 0, source_pc);
			}

			ValueId Binary(Block& block, Opcode opcode, ValueType type, ValueId left,
				ValueId right, u32 source_pc)
			{
				return AddNode(block, opcode, type, {left, right, INVALID_VALUE}, 2, 0, 0,
					source_pc);
			}

			ValueId Constant32(Block& block, u32 value, u32 source_pc)
			{
				return AddNode(block, Opcode::ConstantI32, ValueType::I32, {}, 0, 0, value,
					source_pc);
			}

			ValueId Constant64(Block& block, u64 value, u32 source_pc)
			{
				return AddNode(block, Opcode::ConstantI64, ValueType::I64, {}, 0, 0, value,
					source_pc);
			}

			ValueId ConstantAddress(Block& block, u32 value, u32 source_pc)
			{
				return AddNode(block, Opcode::ConstantAddress, ValueType::Address, {}, 0, 0,
					value, source_pc);
			}

			ValueId Low32(Block& block, const StateMap& state, u32 reg, u32 pc)
			{
				return Unary(block, Opcode::ExtractLow32, ValueType::I32, state.gpr[reg],
					pc);
			}

			ValueId Low64(Block& block, const StateMap& state, u32 reg, u32 pc)
			{
				return Unary(block, Opcode::ExtractLow64, ValueType::I64, state.gpr[reg],
					pc);
			}

			bool WriteLow64(Block& block, StateMap* state, u32 reg, ValueId low,
				u32 source_pc)
			{
				if (reg == 0)
					return true;
				const ValueId complete =
					Binary(block, Opcode::ReplaceLow64, ValueType::I128, state->gpr[reg],
						low, source_pc);
				if (complete == INVALID_VALUE ||
					AddNode(block, Opcode::BindGpr, ValueType::Void,
						{complete, INVALID_VALUE, INVALID_VALUE}, 1, reg, 0,
						source_pc) == INVALID_VALUE)
				{
					return false;
				}
				state->gpr[reg] = complete;
				return true;
			}

			bool LowerMemory(Block& block, StateMap* state, u32 op, u32 pc,
				MemoryAccessKind kind)
			{
				const ValueId base = Low32(block, *state, RS(op), pc);
				const ValueId offset =
					Constant32(block, static_cast<u32>(static_cast<s32>(IMM_S(op))), pc);
				const ValueId address =
					Binary(block, Opcode::EffectiveAddress32, ValueType::Address, base,
						offset, pc);
				if (address == INVALID_VALUE)
					return false;

				const u32 encoded_kind = static_cast<u32>(kind);
				if (IsMemoryLoad(kind))
				{
					const ValueId effect = AddNode(block, Opcode::MemoryLoad,
						ValueType::MemoryEffect,
						{state->memory_effect, address, state->gpr[RT(op)]}, 3,
						encoded_kind, 0, pc);
					if (effect == INVALID_VALUE)
						return false;
					state->memory_effect = effect;
					if (RT(op) == 0)
						return true;
					const ValueId value = Unary(block, Opcode::MemoryLoadValue,
						ValueType::I128, effect, pc);
					if (value == INVALID_VALUE ||
						AddNode(block, Opcode::BindGpr, ValueType::Void,
							{value, INVALID_VALUE, INVALID_VALUE}, 1, RT(op), 0,
							pc) == INVALID_VALUE)
					{
						return false;
					}
					state->gpr[RT(op)] = value;
					return true;
				}

				const ValueId effect = AddNode(block, Opcode::MemoryStore,
					ValueType::MemoryEffect,
					{state->memory_effect, address, state->gpr[RT(op)]}, 3,
					encoded_kind, 0, pc);
				if (effect == INVALID_VALUE)
					return false;
				state->memory_effect = effect;
				return true;
			}

			bool LowerNonBranch(Block& block, StateMap* state, u32 op, u32 pc)
			{
				MemoryAccessKind memory_kind{};
				if (DecodeMemoryAccess(op, &memory_kind))
					return LowerMemory(block, state, op, pc, memory_kind);

				const u32 primary = op >> 26;
				if (primary == 0x00)
				{
					const u32 function = FUNCT(op);
					const u32 rd = RD(op);
					const u32 rs = RS(op);
					const u32 rt = RT(op);
					ValueId left = INVALID_VALUE;
					ValueId right = INVALID_VALUE;
					ValueId value = INVALID_VALUE;

					switch (function)
					{
						case 0x00: // SLL, including architectural NOP.
						case 0x02: // SRL
						case 0x03: // SRA
						{
							value = Low32(block, *state, rt, pc);
							const Opcode shift =
								function == 0x00 ? Opcode::ShiftLeft32 : (function == 0x02 ? Opcode::ShiftRightLogical32 : Opcode::ShiftRightArithmetic32);
							value = Unary(block, shift, ValueType::I32, value, pc, SA(op));
							value =
								Unary(block, Opcode::SignExtend32To64, ValueType::I64, value, pc);
							return WriteLow64(block, state, rd, value, pc);
						}
						case 0x21: // ADDU
						case 0x23: // SUBU
						{
							left = Low32(block, *state, rs, pc);
							right = Low32(block, *state, rt, pc);
							value = Binary(block, function == 0x21 ? Opcode::Add32 : Opcode::Sub32,
								ValueType::I32, left, right, pc);
							value =
								Unary(block, Opcode::SignExtend32To64, ValueType::I64, value, pc);
							return WriteLow64(block, state, rd, value, pc);
						}
						case 0x2d: // DADDU
						case 0x2f: // DSUBU
						{
							left = Low64(block, *state, rs, pc);
							right = Low64(block, *state, rt, pc);
							value = Binary(block, function == 0x2d ? Opcode::Add64 : Opcode::Sub64,
								ValueType::I64, left, right, pc);
							return WriteLow64(block, state, rd, value, pc);
						}
						case 0x24: // AND
						case 0x25: // OR
						case 0x26: // XOR
						case 0x27: // NOR
						{
							left = Low64(block, *state, rs, pc);
							right = Low64(block, *state, rt, pc);
							const Opcode logical =
								function == 0x24 ? Opcode::And64 : (function == 0x25 ? Opcode::Or64 : (function == 0x26 ? Opcode::Xor64 : Opcode::Nor64));
							value = Binary(block, logical, ValueType::I64, left, right, pc);
							return WriteLow64(block, state, rd, value, pc);
						}
						case 0x2a: // SLT
						case 0x2b: // SLTU
						{
							left = Low64(block, *state, rs, pc);
							right = Low64(block, *state, rt, pc);
							value = Binary(block,
								function == 0x2a ? Opcode::CompareSignedLess64 : Opcode::CompareUnsignedLess64,
								ValueType::I1, left, right, pc);
							value =
								Unary(block, Opcode::ZeroExtend32To64, ValueType::I64, value, pc);
							return WriteLow64(block, state, rd, value, pc);
						}
						case 0x38: // DSLL
						case 0x3a: // DSRL
						case 0x3b: // DSRA
						case 0x3c: // DSLL32
						case 0x3e: // DSRL32
						case 0x3f: // DSRA32
						{
							value = Low64(block, *state, rt, pc);
							const bool high_shift = (function & 0x04u) != 0;
							const u32 shift_amount = SA(op) + (high_shift ? 32u : 0u);
							const Opcode shift = (function == 0x38 || function == 0x3c) ? Opcode::ShiftLeft64 : ((function == 0x3a || function == 0x3e) ? Opcode::ShiftRightLogical64 : Opcode::ShiftRightArithmetic64);
							value = Unary(block, shift, ValueType::I64, value, pc, shift_amount);
							return WriteLow64(block, state, rd, value, pc);
						}
						default:
							return false;
					}
				}

				const u32 rs = RS(op);
				const u32 rt = RT(op);
				ValueId left = INVALID_VALUE;
				ValueId right = INVALID_VALUE;
				ValueId value = INVALID_VALUE;
				switch (primary)
				{
					case 0x09: // ADDIU
						left = Low32(block, *state, rs, pc);
						right =
							Constant32(block, static_cast<u32>(static_cast<s32>(IMM_S(op))), pc);
						value = Binary(block, Opcode::Add32, ValueType::I32, left, right, pc);
						value = Unary(block, Opcode::SignExtend32To64, ValueType::I64, value, pc);
						return WriteLow64(block, state, rt, value, pc);
					case 0x19: // DADDIU
						left = Low64(block, *state, rs, pc);
						right =
							Constant64(block, static_cast<u64>(static_cast<s64>(IMM_S(op))), pc);
						value = Binary(block, Opcode::Add64, ValueType::I64, left, right, pc);
						return WriteLow64(block, state, rt, value, pc);
					case 0x0c: // ANDI
					case 0x0d: // ORI
					case 0x0e: // XORI
						left = Low64(block, *state, rs, pc);
						right = Constant64(block, IMM_U(op), pc);
						value = Binary(block,
							primary == 0x0c ? Opcode::And64 : (primary == 0x0d ? Opcode::Or64 : Opcode::Xor64),
							ValueType::I64, left, right, pc);
						return WriteLow64(block, state, rt, value, pc);
					case 0x0a: // SLTI
					case 0x0b: // SLTIU
						left = Low64(block, *state, rs, pc);
						right =
							Constant64(block, static_cast<u64>(static_cast<s64>(IMM_S(op))), pc);
						value = Binary(block,
							primary == 0x0a ? Opcode::CompareSignedLess64 : Opcode::CompareUnsignedLess64,
							ValueType::I1, left, right, pc);
						value = Unary(block, Opcode::ZeroExtend32To64, ValueType::I64, value, pc);
						return WriteLow64(block, state, rt, value, pc);
					case 0x0f: // LUI
						value = Constant32(block, static_cast<u32>(IMM_U(op)) << 16, pc);
						value = Unary(block, Opcode::SignExtend32To64, ValueType::I64, value, pc);
						return WriteLow64(block, state, rt, value, pc);
					default:
						return false;
				}
			}

			ValueId LowerBranchCondition(Block& block, const StateMap& state, u32 op,
				u32 pc)
			{
				const u32 encoded_primary = op >> 26;
				const u32 primary = encoded_primary >= 0x14 && encoded_primary <= 0x17 ?
				                        encoded_primary - 0x10 :
				                        encoded_primary;
				const ValueId rs = Low64(block, state, RS(op), pc);
				switch (primary)
				{
					case 0x01:
						return Unary(block,
							(RT(op) & 1u) == 0 ? Opcode::CompareSignedLessZero64 : Opcode::CompareSignedGreaterEqualZero64,
							ValueType::I1, rs, pc);
					case 0x04:
					case 0x05:
					{
						const ValueId rt = Low64(block, state, RT(op), pc);
						return Binary(block,
							primary == 0x04 ? Opcode::CompareEqual64 : Opcode::CompareNotEqual64,
							ValueType::I1, rs, rt, pc);
					}
					case 0x06:
						return Unary(block, Opcode::CompareSignedLessEqualZero64, ValueType::I1,
							rs, pc);
					case 0x07:
						return Unary(block, Opcode::CompareSignedGreaterZero64, ValueType::I1, rs,
							pc);
					default:
						return INVALID_VALUE;
				}
			}

			bool AdvanceCycles(Block& block, StateMap* state, u32 scaled_cycles,
				u32 source_pc)
			{
				if (scaled_cycles == 0)
					return true;
				const ValueId advanced =
					Unary(block, Opcode::AdvanceCycles, ValueType::Cycle, state->cycle,
						source_pc, scaled_cycles);
				if (advanced == INVALID_VALUE)
					return false;
				state->cycle = advanced;
				return true;
			}

			Transfer MakeTransfer(Block& block, const StateMap& state, u32 target_pc,
				ExitReason reason,
				const std::map<u32, u32>& block_indices,
				u32 source_pc, bool link_internal = true)
			{
				Transfer transfer{};
				transfer.state = state;
				transfer.pc = ConstantAddress(block, target_pc, source_pc);
				transfer.external_reason = reason;
				const auto found = block_indices.find(target_pc);
				if (link_internal && found != block_indices.end())
					transfer.target_block = found->second;
				return transfer;
			}

			u32 Finish() const { return m_next_value; }

		private:
			Program* m_program = nullptr;
			ValueId m_next_value = 0;
		};

		VerifyResult Fail(VerifyFailure failure, u32 block, u32 node,
			std::string detail)
		{
			return {failure, block, node, std::move(detail)};
		}

		bool StateMapsEqual(const StateMap& left, const StateMap& right)
		{
			return left.gpr == right.gpr && left.hi == right.hi && left.lo == right.lo &&
			       left.cycle == right.cycle &&
			       left.memory_effect == right.memory_effect;
		}

		struct RuntimeValue
		{
			ValueType type = ValueType::Void;
			u128 bits{};
		};

		u128 Bits(u64 lo, u64 hi = 0)
		{
			u128 value{};
			value.lo = lo;
			value.hi = hi;
			return value;
		}
	} // namespace

	u32 ScaleBlockCycles(u32 raw_cycles, s8 ee_cycle_rate)
	{
		// Exact owner: PCSX2 x86/ix86-32/iR5900.cpp::
		// scaleblockcycles_calculation() and Interpreter.cpp::
		// intUpdateCPUCycles(). raw_cycles is the three-bit fixed-point value.
		const bool lowcycles = raw_cycles <= 40;
		u32 scaled = 0;
		if (ee_cycle_rate == 0 || lowcycles || ee_cycle_rate < -99 ||
			ee_cycle_rate > 3)
			scaled = raw_cycles >> 3;
		else if (ee_cycle_rate > 1)
			scaled = raw_cycles >> (2 + ee_cycle_rate);
		else if (ee_cycle_rate == 1)
			scaled = static_cast<u32>((raw_cycles >> 3) / 1.3f);
		else if (ee_cycle_rate == -1)
			scaled = (raw_cycles <= 80 || raw_cycles > 168 ? 5 : 7) * raw_cycles / 32;
		else
			scaled = ((5 + (-2 * (ee_cycle_rate + 1))) * raw_cycles) >> 5;
		return std::max(1u, scaled);
	}

	LiftResult Lift(u32 source_base_pc, const u32* source_words,
		u32 source_word_count, u32 entry_pc,
		const LiftOptions& options)
	{
		LiftResult result{};
		if (!source_words || source_word_count == 0 || (source_base_pc & 3u) != 0 ||
			options.cycle_factor < 1 || options.cycle_factor > 2 ||
			options.max_blocks == 0 || options.max_source_instructions == 0 ||
			static_cast<u64>(source_base_pc) +
					static_cast<u64>(source_word_count) * sizeof(u32) >
				static_cast<u64>(UINT32_MAX) + 1)
		{
			result.failure = LiftFailure::InvalidSource;
			result.failure_pc = source_base_pc;
			return result;
		}
		if (source_word_count > options.max_source_instructions)
		{
			result.failure = LiftFailure::SourceLimit;
			result.failure_pc = source_base_pc;
			return result;
		}
		if (!ContainsPc(source_base_pc, source_word_count, entry_pc))
		{
			result.failure = LiftFailure::EntryOutsideSource;
			result.failure_pc = entry_pc;
			return result;
		}

		result.program.source_base_pc = source_base_pc;
		result.program.source_words.assign(source_words,
			source_words + source_word_count);
		result.program.options = options;

		std::set<u32> leaders = {entry_pc};
		std::map<u32, RawBlock> raw_blocks;
		for (u32 pass = 0; pass <= options.max_blocks; pass++)
		{
			const size_t old_leader_count = leaders.size();
			raw_blocks.clear();
			std::deque<u32> pending = {entry_pc};
			std::set<u32> visited;

			while (!pending.empty())
			{
				const u32 pc = pending.front();
				pending.pop_front();
				if (!visited.insert(pc).second)
					continue;
				if (visited.size() > options.max_blocks)
				{
					result.failure = LiftFailure::BlockLimit;
					result.failure_pc = pc;
					return result;
				}

				RawBlock raw{};
				if (!ScanRawBlock(source_base_pc, result.program.source_words, leaders,
						pc, options, &raw, &result.failure, &result.failure_pc))
				{
					return result;
				}

				if (raw.control_kind == RawControlKind::ConditionalBranch)
				{
					const std::array<u32, 2> targets = {
						BranchTarget(raw.branch_pc, raw.branch_opcode),
						raw.branch_pc + 2 * sizeof(u32)};
					for (const u32 target : targets)
					{
						if (ContainsPc(source_base_pc, source_word_count, target))
						{
							leaders.insert(target);
							pending.push_back(target);
						}
					}
				}
				else if (raw.control_kind == RawControlKind::StaticJump)
				{
					// Static jumps are complete Region IR control units, but do not
					// recursively pull a call/jump target into this first bounded CFG.
					// They may still link to a target already owned through conditional
					// control. Wider call/return construction belongs to the profiled
					// reducible-CFG phase and must not turn a useful prefix into a whole-
					// region block/overlap failure.
				}
				else if (leaders.contains(raw.transfer_pc) &&
						 ContainsPc(source_base_pc, source_word_count,
							 raw.transfer_pc))
				{
					pending.push_back(raw.transfer_pc);
				}
				raw_blocks.emplace(pc, std::move(raw));
			}

			if (leaders.size() == old_leader_count)
				break;
			if (pass == options.max_blocks)
			{
				result.failure = LiftFailure::BlockLimit;
				result.failure_pc = entry_pc;
				return result;
			}
		}

		if (raw_blocks.empty() || raw_blocks.size() > options.max_blocks)
		{
			result.failure = LiftFailure::InternalError;
			result.failure_pc = entry_pc;
			return result;
		}

		std::set<u32> source_owners;
		for (const auto& entry : raw_blocks)
		{
			const RawBlock& raw = entry.second;
			for (const SourceInstruction& source : raw.body)
			{
				if (!source_owners.insert(source.pc).second)
				{
					result.failure = LiftFailure::OverlappingSource;
					result.failure_pc = source.pc;
					return result;
				}
			}
			if (raw.control_kind != RawControlKind::None &&
				(!source_owners.insert(raw.branch_pc).second ||
					!source_owners.insert(raw.delay.pc).second))
			{
				result.failure = LiftFailure::OverlappingSource;
				result.failure_pc = raw.delay.pc;
				return result;
			}
		}

		std::map<u32, u32> block_indices;
		for (const auto& [pc, raw] : raw_blocks)
		{
			const u32 index = static_cast<u32>(result.program.blocks.size());
			block_indices.emplace(pc, index);
			result.program.blocks.push_back({});
			result.program.blocks.back().pc = pc;
		}
		result.program.entry_block = block_indices.at(entry_pc);

		Builder builder(&result.program);
		if (!builder.AllocateParameters())
		{
			result.failure = LiftFailure::ValueLimit;
			result.failure_pc = entry_pc;
			return result;
		}

		for (const auto& [pc, raw] : raw_blocks)
		{
			Block& block = result.program.blocks[block_indices.at(pc)];
			StateMap state = block.parameters;
			StateMap not_taken_state = state;
			block.source = raw.body;

			for (const SourceInstruction& instruction : raw.body)
			{
				if (!builder.LowerNonBranch(block, &state, instruction.opcode,
						instruction.pc))
				{
					result.failure = LiftFailure::InternalError;
					result.failure_pc = instruction.pc;
					return result;
				}
			}

			ValueId condition = INVALID_VALUE;
			const bool has_control = raw.control_kind != RawControlKind::None;
			const bool conditional_branch =
				raw.control_kind == RawControlKind::ConditionalBranch;
			const bool likely_branch =
				conditional_branch && IsLikelyBranch(raw.branch_opcode);
			const bool linked_control =
				has_control && IsLinkedControl(raw.branch_opcode);
			if (has_control)
			{
				block.source.push_back({raw.branch_pc, raw.branch_opcode, false});
				block.source.push_back(raw.delay);
				if (conditional_branch)
				{
					condition = builder.LowerBranchCondition(block, state,
						raw.branch_opcode, raw.branch_pc);
					if (condition == INVALID_VALUE)
					{
						result.failure = LiftFailure::InternalError;
						result.failure_pc = raw.branch_pc;
						return result;
					}
				}
				if (linked_control)
				{
					const ValueId link =
						builder.Constant64(block, raw.branch_pc + 2 * sizeof(u32),
							raw.branch_pc);
					if (link == INVALID_VALUE ||
						!builder.WriteLow64(block, &state, 31, link, raw.branch_pc))
					{
						result.failure = LiftFailure::ValueLimit;
						result.failure_pc = raw.branch_pc;
						return result;
					}
				}
				not_taken_state = state;
				if (!builder.LowerNonBranch(block, &state, raw.delay.opcode,
						raw.delay.pc))
				{
					result.failure = LiftFailure::InternalError;
					result.failure_pc = raw.branch_pc;
					return result;
				}
			}

			for (const SourceInstruction& instruction : block.source)
			{
				const u32 cycles = R5900::GetInstruction(instruction.opcode).cycles;
				block.raw_cycle_cost += cycles * options.cycle_factor;
			}
			block.not_taken_raw_cycle_cost = block.raw_cycle_cost;
			if (likely_branch)
			{
				const u32 delay_cycles =
					R5900::GetInstruction(raw.delay.opcode).cycles * options.cycle_factor;
				block.not_taken_raw_cycle_cost -= delay_cycles;
			}
			if (!block.source.empty())
			{
				block.scaled_cycle_cost =
					ScaleBlockCycles(block.raw_cycle_cost, options.ee_cycle_rate);
				block.not_taken_scaled_cycle_cost = ScaleBlockCycles(
					block.not_taken_raw_cycle_cost, options.ee_cycle_rate);
			}
			const u32 primary_cycle_pc =
				block.source.empty() ? block.pc : block.source.back().pc;
			if (!builder.AdvanceCycles(block, &state, block.scaled_cycle_cost,
					primary_cycle_pc) ||
				(likely_branch &&
					!builder.AdvanceCycles(block, &not_taken_state,
						block.not_taken_scaled_cycle_cost, raw.branch_pc)))
			{
				result.failure = LiftFailure::ValueLimit;
				result.failure_pc = pc;
				return result;
			}

			if (conditional_branch)
			{
				if (!likely_branch)
					not_taken_state = state;
				const u32 taken_pc = BranchTarget(raw.branch_pc, raw.branch_opcode);
				const u32 not_taken_pc = raw.branch_pc + 2 * sizeof(u32);
				block.terminator.kind = TerminatorKind::Branch;
				block.terminator.likely = likely_branch;
				block.terminator.condition = condition;
				block.terminator.branch_pc = raw.branch_pc;
				block.terminator.delay_slot_pc = raw.delay.pc;
				block.terminator.taken = builder.MakeTransfer(
					block, state, taken_pc, ExitReason::RegionBoundary, block_indices,
					raw.delay.pc);
				block.terminator.not_taken = builder.MakeTransfer(
					block, not_taken_state, not_taken_pc, ExitReason::RegionBoundary,
					block_indices, likely_branch ? raw.branch_pc : raw.delay.pc);
			}
			else if (raw.control_kind == RawControlKind::StaticJump)
			{
				block.terminator.kind = TerminatorKind::Jump;
				block.terminator.branch_pc = raw.branch_pc;
				block.terminator.delay_slot_pc = raw.delay.pc;
				block.terminator.taken = builder.MakeTransfer(
					block, state, JumpTarget(raw.branch_pc, raw.branch_opcode),
					ExitReason::RegionBoundary, block_indices, raw.delay.pc);
			}
			else
			{
				block.terminator.kind = TerminatorKind::Transfer;
				block.terminator.taken = builder.MakeTransfer(
					block, state, raw.transfer_pc, raw.transfer_reason, block_indices,
					block.source.empty() ? block.pc : block.source.back().pc,
					raw.transfer_reason == ExitReason::RegionBoundary);
			}
		}

		result.program.value_count = builder.Finish();
		const VerifyResult verified = Verify(result.program);
		if (!verified)
		{
			result.failure = LiftFailure::InternalError;
			result.failure_pc = verified.block < result.program.blocks.size() ? result.program.blocks[verified.block].pc : entry_pc;
		}
		return result;
	}

	VerifyResult Verify(const Program& program)
	{
		if (program.blocks.empty() || program.value_count == 0 ||
			program.blocks.size() > program.options.max_blocks ||
			program.source_words.empty() ||
			program.source_words.size() > program.options.max_source_instructions ||
			program.options.max_blocks == 0 ||
			program.options.max_source_instructions == 0 ||
			program.options.cycle_factor < 1 || program.options.cycle_factor > 2 ||
			(program.source_base_pc & 3u) != 0 ||
			static_cast<u64>(program.source_base_pc) +
					static_cast<u64>(program.source_words.size()) * sizeof(u32) >
				static_cast<u64>(UINT32_MAX) + 1)
		{
			return Fail(VerifyFailure::InvalidProgram, INVALID_BLOCK, UINT32_MAX,
				"invalid region or lift options");
		}
		if (program.entry_block >= program.blocks.size())
			return Fail(VerifyFailure::InvalidEntry, program.entry_block, UINT32_MAX,
				"entry block is outside the CFG");

		std::map<u32, u32> pc_to_block;
		std::vector<ValueType> types(program.value_count, ValueType::Void);
		std::vector<u32> defining_block(program.value_count, INVALID_BLOCK);
		std::vector<u32> defining_node(program.value_count, UINT32_MAX);
		std::vector<bool> seen(program.value_count, false);
		std::map<u32, u32> source_owners;
		for (u32 block_index = 0; block_index < program.blocks.size();
			 block_index++)
		{
			const Block& block = program.blocks[block_index];
			if (!pc_to_block.emplace(block.pc, block_index).second)
				return Fail(VerifyFailure::DuplicateBlockPc, block_index, UINT32_MAX,
					"two blocks own the same guest PC");
			for (u32 node_index = 0; node_index < block.nodes.size(); node_index++)
			{
				const Node& node = block.nodes[node_index];
				if (node.id >= program.value_count || seen[node.id])
					return Fail(VerifyFailure::ValueIdMismatch, block_index, node_index,
						"value ID is out of range or multiply defined");
				seen[node.id] = true;
				types[node.id] = node.type;
				defining_block[node.id] = block_index;
				defining_node[node.id] = node_index;
			}
		}
		if (std::find(seen.begin(), seen.end(), false) != seen.end())
			return Fail(VerifyFailure::ValueIdMismatch, INVALID_BLOCK, UINT32_MAX,
				"value IDs are not dense");

		auto type_is = [&](ValueId value, ValueType type) {
			return value < types.size() && types[value] == type;
		};
		auto source_word_matches = [&](const SourceInstruction& source) {
			return ContainsPc(program.source_base_pc,
					   static_cast<u32>(program.source_words.size()),
					   source.pc) &&
			       ReadSourceWord(program.source_base_pc, program.source_words,
					   source.pc) == source.opcode;
		};

		for (u32 block_index = 0; block_index < program.blocks.size();
			 block_index++)
		{
			const Block& block = program.blocks[block_index];
			if (!ContainsPc(program.source_base_pc,
					static_cast<u32>(program.source_words.size()), block.pc))
			{
				return Fail(VerifyFailure::SourceMismatch, block_index, UINT32_MAX,
					"block entry is outside the immutable source image");
			}
			if (block.terminator.kind != TerminatorKind::Transfer &&
				block.terminator.kind != TerminatorKind::Branch &&
				block.terminator.kind != TerminatorKind::Jump)
			{
				return Fail(VerifyFailure::ControlFlowMismatch, block_index, UINT32_MAX,
					"block has an invalid terminator kind");
			}
			if (block.nodes.size() < PARAMETER_COUNT)
				return Fail(VerifyFailure::ParameterContract, block_index, UINT32_MAX,
					"block lacks its complete canonical parameter set");

			StateMap expected{};
			for (u32 slot = 0; slot < PARAMETER_COUNT; slot++)
			{
				const Node& parameter = block.nodes[slot];
				const ValueType expected_type = slot == CYCLE_PARAMETER ?
				                                    ValueType::Cycle :
				                                    (slot == MEMORY_EFFECT_PARAMETER ? ValueType::MemoryEffect :
																					   ValueType::I128);
				if (parameter.opcode != Opcode::Parameter ||
					parameter.type != expected_type || parameter.operand_count != 0 ||
					parameter.immediate != slot)
				{
					return Fail(VerifyFailure::ParameterContract, block_index, slot,
						"parameter order/type does not match canonical EE state");
				}
				if (slot < GPR_COUNT)
					expected.gpr[slot] = parameter.id;
				else if (slot == HI_PARAMETER)
					expected.hi = parameter.id;
				else if (slot == LO_PARAMETER)
					expected.lo = parameter.id;
				else if (slot == CYCLE_PARAMETER)
					expected.cycle = parameter.id;
				else
					expected.memory_effect = parameter.id;
			}
			if (!StateMapsEqual(expected, block.parameters))
				return Fail(VerifyFailure::ParameterContract, block_index, UINT32_MAX,
					"published parameter map differs from parameter nodes");

			const bool has_delayed_control =
				block.terminator.kind == TerminatorKind::Branch ||
				block.terminator.kind == TerminatorKind::Jump;
			u32 raw_cycles = 0;
			for (u32 i = 0; i < block.source.size(); i++)
			{
				const SourceInstruction& source = block.source[i];
				const bool is_control_instruction =
					has_delayed_control && i + 2 == block.source.size();
				if (!source_word_matches(source) ||
					(i == 0 ? source.pc != block.pc : source.pc != block.source[i - 1].pc + sizeof(u32)))
				{
					return Fail(
						VerifyFailure::SourceMismatch, block_index, i,
						"source record is not a contiguous byte-exact program word");
				}
				if (!source_owners.emplace(source.pc, block_index).second)
				{
					return Fail(VerifyFailure::SourceOverlap, block_index, i,
						"one immutable source instruction is owned by multiple blocks");
				}
				if (source.delay_slot !=
					(has_delayed_control && i + 1 == block.source.size()))
				{
					return Fail(VerifyFailure::SourceMismatch, block_index, i,
						"delay-slot marker does not match the block terminator");
				}
				if (is_control_instruction)
				{
					const bool matches_kind =
						block.terminator.kind == TerminatorKind::Branch ?
							IsConditionalBranch(source.opcode) :
							CanLowerStaticJump(source.opcode, program.options);
					if (!matches_kind)
						return Fail(
							VerifyFailure::ControlFlowMismatch, block_index, i,
							"control source slot does not match its terminator kind");
				}
				else
				{
					if (IsAnyControlFlow(source.opcode))
						return Fail(VerifyFailure::ControlFlowMismatch, block_index, i,
							"control flow appears outside the branch source slot");
					if (!CanLowerNonBranch(source.opcode))
						return Fail(
							VerifyFailure::SourceMismatch, block_index, i,
							"source instruction is outside the represented semantic surface");
				}
				const u32 cycles = R5900::GetInstruction(source.opcode).cycles;
				raw_cycles += cycles * program.options.cycle_factor;
			}
			const bool likely_branch =
				block.terminator.kind == TerminatorKind::Branch &&
				block.source.size() >= 2 &&
				IsLikelyBranch(block.source[block.source.size() - 2].opcode);
			u32 not_taken_raw_cycles = raw_cycles;
			if (likely_branch)
			{
				not_taken_raw_cycles -=
					R5900::GetInstruction(block.source.back().opcode).cycles *
					program.options.cycle_factor;
			}
			const u32 scaled_cycles = block.source.empty() ?
			                              0 :
			                              ScaleBlockCycles(raw_cycles,
								  program.options.ee_cycle_rate);
			const u32 not_taken_scaled_cycles = block.source.empty() ?
			                                        0 :
			                                        ScaleBlockCycles(not_taken_raw_cycles,
										program.options.ee_cycle_rate);
			if (raw_cycles != block.raw_cycle_cost ||
				scaled_cycles != block.scaled_cycle_cost ||
				not_taken_raw_cycles != block.not_taken_raw_cycle_cost ||
				not_taken_scaled_cycles != block.not_taken_scaled_cycle_cost)
			{
				return Fail(VerifyFailure::CycleMismatch, block_index, UINT32_MAX,
					"block cost differs from PCSX2 opcode-cycle scaling");
			}

			ValueId primary_cycle_advance = INVALID_VALUE;
			ValueId not_taken_cycle_advance = INVALID_VALUE;
			bool captured_control_input = false;
			bool captured_delay_input = false;
			StateMap control_input{};
			StateMap delay_input{};
			const u32 control_opcode = has_delayed_control && block.source.size() >= 2 ?
			                               block.source[block.source.size() - 2].opcode :
			                               0;
			const bool linked_control =
				has_delayed_control && IsLinkedControl(control_opcode);
			u32 link_bind_count = 0;
			std::map<u32, u32> memory_operation_count;
			std::map<u32, u32> memory_value_count;
			std::map<u32, u32> memory_bind_count;
			auto source_opcode_at = [&](u32 pc, u32* opcode) {
				const auto found = std::find_if(block.source.begin(), block.source.end(),
					[pc](const SourceInstruction& source) { return source.pc == pc; });
				if (found == block.source.end())
					return false;
				*opcode = found->opcode;
				return true;
			};
			auto require_operand = [&](const Node& node, u32 node_index, u32 operand,
									   ValueType required) -> VerifyResult {
				if (operand >= node.operand_count ||
					node.operands[operand] >= program.value_count)
					return Fail(VerifyFailure::OperandOutOfRange, block_index, node_index,
						"operand is absent or outside the value table");
				const ValueId value = node.operands[operand];
				if (defining_block[value] != block_index ||
					defining_node[value] >= node_index)
					return Fail(VerifyFailure::OperandNotLocal, block_index, node_index,
						"value crosses a block without a parameter or is a forward "
						"reference");
				if (types[value] != required)
					return Fail(VerifyFailure::OperandType, block_index, node_index,
						"operand type is incompatible with the opcode");
				return {};
			};

			for (u32 node_index = PARAMETER_COUNT; node_index < block.nodes.size();
				 node_index++)
			{
				const Node& node = block.nodes[node_index];
				if (!captured_control_input && has_delayed_control &&
					node.source_pc == block.terminator.branch_pc)
				{
					control_input = expected;
					captured_control_input = true;
				}
				if (!captured_delay_input && has_delayed_control &&
					node.source_pc == block.terminator.delay_slot_pc)
				{
					delay_input = expected;
					captured_delay_input = true;
				}
				auto unary = [&](ValueType input, ValueType output) -> VerifyResult {
					if (node.operand_count != 1 || node.type != output)
						return Fail(VerifyFailure::ResultType, block_index, node_index,
							"unary opcode has the wrong arity or result type");
					return require_operand(node, node_index, 0, input);
				};
				auto binary = [&](ValueType left, ValueType right,
								  ValueType output) -> VerifyResult {
					if (node.operand_count != 2 || node.type != output)
						return Fail(VerifyFailure::ResultType, block_index, node_index,
							"binary opcode has the wrong arity or result type");
					VerifyResult checked = require_operand(node, node_index, 0, left);
					return checked ? require_operand(node, node_index, 1, right) : checked;
				};

				VerifyResult checked{};
				switch (node.opcode)
				{
					case Opcode::Parameter:
						return Fail(VerifyFailure::ParameterContract, block_index, node_index,
							"parameter appears after executable nodes");
					case Opcode::ConstantI32:
					case Opcode::ConstantI64:
					case Opcode::ConstantAddress:
					{
						const ValueType expected_type =
							node.opcode == Opcode::ConstantI32 ? ValueType::I32 : (node.opcode == Opcode::ConstantI64 ? ValueType::I64 : ValueType::Address);
						if (node.operand_count != 0 || node.type != expected_type ||
							node.immediate != 0 ||
							(node.opcode != Opcode::ConstantI64 && node.literal > UINT32_MAX))
							checked = Fail(VerifyFailure::ResultType, block_index, node_index,
								"constant has the wrong arity, type, immediate, or width");
						break;
					}
					case Opcode::ExtractLow32:
						checked = unary(ValueType::I128, ValueType::I32);
						break;
					case Opcode::ExtractLow64:
						checked = unary(ValueType::I128, ValueType::I64);
						break;
					case Opcode::ReplaceLow64:
						checked = binary(ValueType::I128, ValueType::I64, ValueType::I128);
						break;
					case Opcode::SignExtend32To64:
						checked = unary(ValueType::I32, ValueType::I64);
						break;
					case Opcode::ZeroExtend32To64:
						if (node.operand_count != 1 || node.type != ValueType::I64)
							checked = Fail(VerifyFailure::ResultType, block_index, node_index,
								"zero extension has the wrong arity or result type");
						else if (!type_is(node.operands[0], ValueType::I32) &&
								 !type_is(node.operands[0], ValueType::I1))
							checked = Fail(VerifyFailure::OperandType, block_index, node_index,
								"zero extension requires I1 or I32");
						else if (defining_block[node.operands[0]] != block_index ||
								 defining_node[node.operands[0]] >= node_index)
							checked =
								Fail(VerifyFailure::OperandNotLocal, block_index, node_index,
									"zero-extension operand is not a prior local value");
						break;
					case Opcode::Add32:
					case Opcode::Sub32:
						checked = binary(ValueType::I32, ValueType::I32, ValueType::I32);
						break;
					case Opcode::Add64:
					case Opcode::Sub64:
					case Opcode::And64:
					case Opcode::Or64:
					case Opcode::Xor64:
					case Opcode::Nor64:
						checked = binary(ValueType::I64, ValueType::I64, ValueType::I64);
						break;
					case Opcode::ShiftLeft32:
					case Opcode::ShiftRightLogical32:
					case Opcode::ShiftRightArithmetic32:
						checked = unary(ValueType::I32, ValueType::I32);
						if (checked && node.immediate >= 32)
							checked = Fail(VerifyFailure::OperandOutOfRange, block_index,
								node_index, "I32 shift amount is out of range");
						break;
					case Opcode::ShiftLeft64:
					case Opcode::ShiftRightLogical64:
					case Opcode::ShiftRightArithmetic64:
						checked = unary(ValueType::I64, ValueType::I64);
						if (checked && node.immediate >= 64)
							checked = Fail(VerifyFailure::OperandOutOfRange, block_index,
								node_index, "I64 shift amount is out of range");
						break;
					case Opcode::CompareEqual64:
					case Opcode::CompareNotEqual64:
					case Opcode::CompareSignedLess64:
					case Opcode::CompareUnsignedLess64:
						checked = binary(ValueType::I64, ValueType::I64, ValueType::I1);
						break;
					case Opcode::CompareSignedLessEqualZero64:
					case Opcode::CompareSignedGreaterZero64:
					case Opcode::CompareSignedLessZero64:
					case Opcode::CompareSignedGreaterEqualZero64:
						checked = unary(ValueType::I64, ValueType::I1);
						break;
					case Opcode::EffectiveAddress32:
						checked = binary(ValueType::I32, ValueType::I32,
							ValueType::Address);
						break;
					case Opcode::MemoryLoad:
					case Opcode::MemoryStore:
					{
						const bool load = node.opcode == Opcode::MemoryLoad;
						if (node.operand_count != 3 ||
							node.type != ValueType::MemoryEffect)
						{
							checked = Fail(VerifyFailure::ResultType, block_index,
								node_index,
								"memory operation has the wrong arity or result type");
							break;
						}
						checked = require_operand(node, node_index, 0,
							ValueType::MemoryEffect);
						if (checked)
							checked = require_operand(node, node_index, 1,
								ValueType::Address);
						if (checked)
							checked = require_operand(node, node_index, 2,
								ValueType::I128);
						if (!checked)
							break;

						u32 source_opcode = 0;
						MemoryAccessKind expected_kind{};
						if (!source_opcode_at(node.source_pc, &source_opcode) ||
							!DecodeMemoryAccess(source_opcode, &expected_kind) ||
							load != IsMemoryLoad(expected_kind) ||
							node.immediate != static_cast<u32>(expected_kind) ||
							node.operands[0] != expected.memory_effect ||
							node.operands[2] != expected.gpr[RT(source_opcode)])
						{
							checked = Fail(VerifyFailure::SourceMismatch, block_index,
								node_index,
								"memory kind, effect chain, or decoded GPR does not match source");
							break;
						}

						const Node& address =
							block.nodes[defining_node[node.operands[1]]];
						if (address.opcode != Opcode::EffectiveAddress32 ||
							address.source_pc != node.source_pc ||
							address.operand_count != 2)
						{
							checked = Fail(VerifyFailure::SourceMismatch, block_index,
								node_index,
								"memory address is not its decoded I32 base-plus-offset");
							break;
						}
						const Node& base =
							block.nodes[defining_node[address.operands[0]]];
						const Node& offset =
							block.nodes[defining_node[address.operands[1]]];
						if (base.opcode != Opcode::ExtractLow32 ||
							base.source_pc != node.source_pc ||
							base.operand_count != 1 ||
							base.operands[0] != expected.gpr[RS(source_opcode)] ||
							offset.opcode != Opcode::ConstantI32 ||
							offset.source_pc != node.source_pc ||
							static_cast<u32>(offset.literal) !=
								static_cast<u32>(static_cast<s32>(IMM_S(source_opcode))))
						{
							checked = Fail(VerifyFailure::SourceMismatch, block_index,
								node_index,
								"memory effective address operands do not match decoded source");
							break;
						}
						expected.memory_effect = node.id;
						memory_operation_count[node.source_pc]++;
						break;
					}
					case Opcode::MemoryLoadValue:
					{
						checked = unary(ValueType::MemoryEffect, ValueType::I128);
						if (!checked)
							break;
						const Node& memory =
							block.nodes[defining_node[node.operands[0]]];
						u32 source_opcode = 0;
						MemoryAccessKind kind{};
						if (memory.opcode != Opcode::MemoryLoad ||
							memory.source_pc != node.source_pc ||
							!source_opcode_at(node.source_pc, &source_opcode) ||
							!DecodeMemoryAccess(source_opcode, &kind) ||
							!IsMemoryLoad(kind) || RT(source_opcode) == 0)
						{
							checked = Fail(VerifyFailure::SourceMismatch, block_index,
								node_index,
								"memory load value does not match a decoded nonzero destination");
							break;
						}
						memory_value_count[node.source_pc]++;
						break;
					}
					case Opcode::BindGpr:
						checked = unary(ValueType::I128, ValueType::Void);
						if (checked && (node.immediate == 0 || node.immediate >= GPR_COUNT))
							checked =
								Fail(VerifyFailure::OperandOutOfRange, block_index, node_index,
									"GPR binding targets the immutable zero register or an "
									"invalid slot");
						if (checked)
						{
							const Node& value =
								block.nodes[defining_node[node.operands[0]]];
							if (has_delayed_control &&
								node.source_pc == block.terminator.branch_pc)
							{
								if (!linked_control || node.immediate != 31 ||
									value.opcode != Opcode::ReplaceLow64 ||
									value.source_pc != block.terminator.branch_pc ||
									value.operand_count != 2 ||
									value.operands[0] != expected.gpr[31] ||
									value.operands[1] >= program.value_count ||
									defining_block[value.operands[1]] != block_index)
								{
									checked = Fail(VerifyFailure::SourceMismatch,
										block_index, node_index,
										"linked control does not replace r31 low64 exactly");
									break;
								}
								const Node& link =
									block.nodes[defining_node[value.operands[1]]];
								if (link.opcode != Opcode::ConstantI64 ||
									link.source_pc != block.terminator.branch_pc ||
									link.literal !=
										static_cast<u64>(block.terminator.branch_pc) +
											2 * sizeof(u32))
								{
									checked = Fail(VerifyFailure::SourceMismatch,
										block_index, node_index,
										"linked control publishes the wrong return address");
									break;
								}
								link_bind_count++;
							}
							if (value.opcode == Opcode::MemoryLoadValue)
							{
								u32 source_opcode = 0;
								if (!source_opcode_at(node.source_pc, &source_opcode) ||
									value.source_pc != node.source_pc ||
									node.immediate != RT(source_opcode))
								{
									checked = Fail(VerifyFailure::SourceMismatch,
										block_index, node_index,
										"loaded value binds a GPR other than decoded rt");
									break;
								}
								memory_bind_count[node.source_pc]++;
							}
							expected.gpr[node.immediate] = node.operands[0];
						}
						break;
					case Opcode::BindHi:
						checked = unary(ValueType::I128, ValueType::Void);
						if (checked)
							expected.hi = node.operands[0];
						break;
					case Opcode::BindLo:
						checked = unary(ValueType::I128, ValueType::Void);
						if (checked)
							expected.lo = node.operands[0];
						break;
					case Opcode::AdvanceCycles:
					{
						checked = unary(ValueType::Cycle, ValueType::Cycle);
						if (!checked)
							break;
						if (node.operands[0] != block.parameters.cycle)
						{
							checked = Fail(VerifyFailure::CycleMismatch, block_index, node_index,
								"cycle advance does not consume the block-entry cycle");
							break;
						}
						const u32 primary_source_pc = block.source.empty() ?
							block.pc :
							block.source.back().pc;
						if (node.source_pc == primary_source_pc &&
							node.immediate == block.scaled_cycle_cost &&
							primary_cycle_advance == INVALID_VALUE)
						{
							primary_cycle_advance = node.id;
						}
						else if (likely_branch &&
							node.source_pc == block.terminator.branch_pc &&
							node.immediate == block.not_taken_scaled_cycle_cost &&
							not_taken_cycle_advance == INVALID_VALUE)
						{
							not_taken_cycle_advance = node.id;
						}
						else
						{
							checked = Fail(VerifyFailure::CycleMismatch, block_index,
								node_index,
								"cycle advance has the wrong edge, source PC, or cost");
						}
						break;
					}
					default:
						checked = Fail(VerifyFailure::ResultType, block_index, node_index,
							"node uses an unknown Region IR opcode");
						break;
				}
				if (!checked)
					return checked;
			}
			for (const SourceInstruction& source : block.source)
			{
				MemoryAccessKind kind{};
				if (!DecodeMemoryAccess(source.opcode, &kind))
					continue;
				const u32 expected_values =
					IsMemoryLoad(kind) && RT(source.opcode) != 0 ? 1u : 0u;
				if (memory_operation_count[source.pc] != 1 ||
					memory_value_count[source.pc] != expected_values ||
					memory_bind_count[source.pc] != expected_values)
				{
					return Fail(VerifyFailure::SourceMismatch, block_index, UINT32_MAX,
						"decoded memory instruction lacks one exact ordered effect/value/bind");
				}
			}
			if (has_delayed_control && !captured_delay_input)
				return Fail(VerifyFailure::ControlFlowMismatch, block_index, UINT32_MAX,
					"delayed control has no mechanically derived pre-delay state");
			if (linked_control ? link_bind_count != 1 : link_bind_count != 0)
				return Fail(VerifyFailure::SourceMismatch, block_index, UINT32_MAX,
					"control link binding does not match its decoded source");
			if ((primary_cycle_advance != INVALID_VALUE) != !block.source.empty() ||
				(likely_branch ? not_taken_cycle_advance == INVALID_VALUE :
				                 not_taken_cycle_advance != INVALID_VALUE))
				return Fail(VerifyFailure::CycleMismatch, block_index, UINT32_MAX,
					"cycle advances do not match the source block edges");

			StateMap primary_expected = expected;
			if (primary_cycle_advance != INVALID_VALUE)
				primary_expected.cycle = primary_cycle_advance;
			StateMap not_taken_expected = likely_branch ? delay_input : primary_expected;
			if (likely_branch)
				not_taken_expected.cycle = not_taken_cycle_advance;

			auto verify_transfer = [&](const Transfer& transfer,
				const StateMap& expected_state) -> VerifyResult {
				if (!StateMapsEqual(transfer.state, expected_state))
					return Fail(
						VerifyFailure::StateMapMismatch, block_index, UINT32_MAX,
						"edge does not publish the mechanically derived canonical state");
				if (!type_is(transfer.pc, ValueType::Address) ||
					defining_block[transfer.pc] != block_index)
				{
					return Fail(VerifyFailure::ControlFlowMismatch, block_index, UINT32_MAX,
						"edge PC is not a local Address value");
				}
				const Node& pc_node = block.nodes[defining_node[transfer.pc]];
				if (pc_node.opcode != Opcode::ConstantAddress)
					return Fail(VerifyFailure::ControlFlowMismatch, block_index, UINT32_MAX,
						"first Region IR boundary requires a static edge PC");
				const u32 static_pc = static_cast<u32>(pc_node.literal);
				const auto target = pc_to_block.find(static_pc);
				const u32 expected_target =
					transfer.external_reason == ExitReason::RegionBoundary &&
							target != pc_to_block.end() ?
						target->second :
						INVALID_BLOCK;
				if (transfer.target_block != expected_target)
				{
					return Fail(
						VerifyFailure::ControlFlowMismatch, block_index, UINT32_MAX,
						"edge target does not match the CFG block at its static PC");
				}
				if (transfer.target_block != INVALID_BLOCK)
				{
					if (transfer.target_block >= program.blocks.size() ||
						static_pc != program.blocks[transfer.target_block].pc)
					{
						return Fail(VerifyFailure::ControlFlowMismatch, block_index,
							UINT32_MAX, "internal edge PC and target block disagree");
					}
				}
				else
				{
					const ExitReason expected_reason = ClassifyExternalResume(
						program.source_base_pc, program.source_words, static_pc,
						program.options);
					if (transfer.external_reason != expected_reason)
					{
						return Fail(VerifyFailure::ExitContractMismatch, block_index,
							UINT32_MAX,
							"external edge reason does not match its byte-exact resume opcode");
					}
				}
				return {};
			};

			VerifyResult transfer_check =
				verify_transfer(block.terminator.taken, primary_expected);
			if (!transfer_check)
				return transfer_check;
			if (block.terminator.kind == TerminatorKind::Branch)
			{
				if (block.source.size() < 2 || !captured_control_input ||
					block.terminator.likely != likely_branch ||
					block.terminator.condition >= program.value_count ||
					!type_is(block.terminator.condition, ValueType::I1) ||
					defining_block[block.terminator.condition] != block_index ||
					block.source[block.source.size() - 2].pc !=
						block.terminator.branch_pc ||
					block.source.back().pc != block.terminator.delay_slot_pc ||
					!IsConditionalBranch(block.source[block.source.size() - 2].opcode))
				{
					return Fail(
						VerifyFailure::ControlFlowMismatch, block_index, UINT32_MAX,
						"branch terminator does not match its source pair and predicate");
				}
				const Node& condition =
					block.nodes[defining_node[block.terminator.condition]];
				if (condition.source_pc != block.terminator.branch_pc)
					return Fail(
						VerifyFailure::ControlFlowMismatch, block_index, UINT32_MAX,
						"branch predicate was not snapped at the branch instruction");
				for (u32 i = 0; i < defining_node[block.terminator.condition]; i++)
				{
					if (block.nodes[i].source_pc == block.terminator.delay_slot_pc)
						return Fail(VerifyFailure::ControlFlowMismatch, block_index, i,
							"delay-slot work precedes the branch predicate snapshot");
				}

				auto is_branch_gpr = [&](ValueId value, u32 gpr) {
					if (value >= program.value_count ||
						defining_block[value] != block_index)
					{
						return false;
					}
					const Node& extract = block.nodes[defining_node[value]];
					return extract.opcode == Opcode::ExtractLow64 &&
					       extract.operand_count == 1 &&
					       extract.operands[0] == control_input.gpr[gpr] &&
					       extract.source_pc == block.terminator.branch_pc;
				};
				const u32 branch_opcode = block.source[block.source.size() - 2].opcode;
				const u32 encoded_primary = branch_opcode >> 26;
				const u32 primary =
					encoded_primary >= 0x14 && encoded_primary <= 0x17 ?
						encoded_primary - 0x10 :
						encoded_primary;
				Opcode expected_condition = Opcode::CompareEqual64;
				bool predicate_matches = false;
				switch (primary)
				{
					case 0x01:
						expected_condition = (RT(branch_opcode) & 1u) == 0 ? Opcode::CompareSignedLessZero64 : Opcode::CompareSignedGreaterEqualZero64;
						predicate_matches =
							condition.opcode == expected_condition &&
							condition.operand_count == 1 &&
							is_branch_gpr(condition.operands[0], RS(branch_opcode));
						break;
					case 0x04:
					case 0x05:
						expected_condition = primary == 0x04 ? Opcode::CompareEqual64 : Opcode::CompareNotEqual64;
						predicate_matches =
							condition.opcode == expected_condition &&
							condition.operand_count == 2 &&
							is_branch_gpr(condition.operands[0], RS(branch_opcode)) &&
							is_branch_gpr(condition.operands[1], RT(branch_opcode));
						break;
					case 0x06:
						expected_condition = Opcode::CompareSignedLessEqualZero64;
						predicate_matches =
							condition.opcode == expected_condition &&
							condition.operand_count == 1 &&
							is_branch_gpr(condition.operands[0], RS(branch_opcode));
						break;
					case 0x07:
						expected_condition = Opcode::CompareSignedGreaterZero64;
						predicate_matches =
							condition.opcode == expected_condition &&
							condition.operand_count == 1 &&
							is_branch_gpr(condition.operands[0], RS(branch_opcode));
						break;
				}
				if (!predicate_matches)
				{
					return Fail(
						VerifyFailure::ControlFlowMismatch, block_index,
						defining_node[block.terminator.condition],
						"branch predicate does not read the decoded pre-delay operands");
				}

				transfer_check = verify_transfer(
					block.terminator.not_taken, not_taken_expected);
				if (!transfer_check)
					return transfer_check;

				const Node& taken_pc =
					block.nodes[defining_node[block.terminator.taken.pc]];
				const Node& not_taken_pc =
					block.nodes[defining_node[block.terminator.not_taken.pc]];
				if (static_cast<u32>(taken_pc.literal) !=
						BranchTarget(block.terminator.branch_pc, branch_opcode) ||
					static_cast<u32>(not_taken_pc.literal) !=
						block.terminator.branch_pc + 2 * sizeof(u32))
				{
					return Fail(
						VerifyFailure::ControlFlowMismatch, block_index, UINT32_MAX,
						"branch edges do not match the decoded target and fallthrough PCs");
				}
			}
			else
			{
				if (block.terminator.condition != INVALID_VALUE)
					return Fail(VerifyFailure::ControlFlowMismatch, block_index,
						UINT32_MAX, "unconditional transfer carries a predicate");
				if (block.terminator.likely)
					return Fail(VerifyFailure::ControlFlowMismatch, block_index,
						UINT32_MAX, "unconditional transfer is marked branch-likely");
				const Node& next_pc =
					block.nodes[defining_node[block.terminator.taken.pc]];
				if (block.terminator.kind == TerminatorKind::Jump)
				{
					if (block.source.size() < 2 ||
						block.source[block.source.size() - 2].pc !=
							block.terminator.branch_pc ||
						block.source.back().pc != block.terminator.delay_slot_pc ||
						!CanLowerStaticJump(control_opcode, program.options) ||
						static_cast<u32>(next_pc.literal) !=
							JumpTarget(block.terminator.branch_pc, control_opcode))
					{
						return Fail(VerifyFailure::ControlFlowMismatch, block_index,
							UINT32_MAX,
							"static jump does not match its source pair and target");
					}
					continue;
				}
				if (block.terminator.kind != TerminatorKind::Transfer)
					return Fail(VerifyFailure::ControlFlowMismatch, block_index,
						UINT32_MAX, "terminator kind is not represented");
				const u32 expected_pc = block.source.empty() ? block.pc : block.source.back().pc + sizeof(u32);
				if (static_cast<u32>(next_pc.literal) != expected_pc)
				{
					return Fail(
						VerifyFailure::ControlFlowMismatch, block_index, UINT32_MAX,
						"unconditional edge does not follow the represented source span");
				}
			}
		}

		std::vector<bool> reachable(program.blocks.size(), false);
		std::deque<u32> pending = {program.entry_block};
		while (!pending.empty())
		{
			const u32 block_index = pending.front();
			pending.pop_front();
			if (reachable[block_index])
				continue;
			reachable[block_index] = true;
			const Block& block = program.blocks[block_index];
			if (block.terminator.taken.target_block != INVALID_BLOCK)
				pending.push_back(block.terminator.taken.target_block);
			if (block.terminator.kind == TerminatorKind::Branch &&
				block.terminator.not_taken.target_block != INVALID_BLOCK)
			{
				pending.push_back(block.terminator.not_taken.target_block);
			}
		}
		if (std::find(reachable.begin(), reachable.end(), false) != reachable.end())
			return Fail(VerifyFailure::UnreachableBlock, INVALID_BLOCK, UINT32_MAX,
				"CFG contains a block unreachable from the entry");

		return {};
	}

	InterpretResult Interpret(const Program& program, const CanonicalState& input,
		CanonicalState* output,
		const InterpretOptions& options)
	{
		InterpretResult result{};
		if (!output)
		{
			result.error = "output state is null";
			return result;
		}
		const VerifyResult verified = Verify(program);
		if (!verified)
		{
			result.error = verified.detail;
			return result;
		}

		std::vector<RuntimeValue> values(program.value_count);
		CanonicalState current = input;
		current.gpr[0] = {};
		auto event_due = [&](u64 cycle) {
			return options.next_event_cycle != UINT64_MAX &&
			       cycle >= options.next_event_cycle;
		};
		if (event_due(current.cycle))
		{
			*output = current;
			result.completed = true;
			result.reason = ExitReason::EventHorizon;
			return result;
		}
		auto assign_parameters = [&](const Block& block,
									 const CanonicalState& state,
									 const RuntimeValue& memory_effect) {
			for (u32 gpr = 0; gpr < GPR_COUNT; gpr++)
				values[block.parameters.gpr[gpr]] = {ValueType::I128, state.gpr[gpr]};
			values[block.parameters.hi] = {ValueType::I128, state.hi};
			values[block.parameters.lo] = {ValueType::I128, state.lo};
			values[block.parameters.cycle] = {ValueType::Cycle, Bits(state.cycle)};
			values[block.parameters.memory_effect] = memory_effect;
		};
		auto materialize = [&](const Transfer& transfer) {
			CanonicalState state{};
			for (u32 gpr = 0; gpr < GPR_COUNT; gpr++)
				state.gpr[gpr] = values[transfer.state.gpr[gpr]].bits;
			state.gpr[0] = {};
			state.hi = values[transfer.state.hi].bits;
			state.lo = values[transfer.state.lo].bits;
			state.cycle = values[transfer.state.cycle].bits.lo;
			state.pc = static_cast<u32>(values[transfer.pc].bits.lo);
			return state;
		};

		u32 block_index = program.entry_block;
		assign_parameters(program.blocks[block_index], current,
			{ValueType::MemoryEffect, Bits(0)});
		auto exit_before_memory = [&](const Block& block, const Node& node,
									  u32 address, ExitReason reason) {
			u32 pending_raw_cycles = 0;
			u32 source_instructions_executed = 0;
			for (const SourceInstruction& source : block.source)
			{
				if (source.pc == node.source_pc)
					break;
				pending_raw_cycles +=
					R5900::GetInstruction(source.opcode).cycles *
					program.options.cycle_factor;
				source_instructions_executed++;
			}
			result.source_instructions_executed +=
				source_instructions_executed;
			current.gpr[0] = {};
			current.pc = node.source_pc;
			*output = current;
			result.completed = true;
			result.reason = reason;
			result.pending_raw_cycles = pending_raw_cycles;
			result.memory_address = address;
		};
		for (;;)
		{
			if (result.blocks_executed >= options.max_block_executions)
			{
				result.error = "Region IR execution exceeded its validation block budget";
				return result;
			}
			const Block& block = program.blocks[block_index];
			result.blocks_executed++;
			for (u32 node_index = PARAMETER_COUNT; node_index < block.nodes.size();
				 node_index++)
			{
				const Node& node = block.nodes[node_index];
				const u64 left =
					node.operand_count > 0 ? values[node.operands[0]].bits.lo : 0;
				const u64 right =
					node.operand_count > 1 ? values[node.operands[1]].bits.lo : 0;
				u128 bits{};
				switch (node.opcode)
				{
					case Opcode::Parameter:
						break;
					case Opcode::ConstantI32:
					case Opcode::ConstantI64:
					case Opcode::ConstantAddress:
						bits = Bits(node.literal);
						break;
					case Opcode::ExtractLow32:
						bits = Bits(static_cast<u32>(left));
						break;
					case Opcode::ExtractLow64:
						bits = Bits(left);
						break;
					case Opcode::ReplaceLow64:
						bits = values[node.operands[0]].bits;
						bits.lo = right;
						break;
					case Opcode::SignExtend32To64:
						bits = Bits(static_cast<u64>(
							static_cast<s64>(std::bit_cast<s32>(static_cast<u32>(left)))));
						break;
					case Opcode::ZeroExtend32To64:
						bits = Bits(static_cast<u32>(left));
						break;
					case Opcode::Add32:
						bits = Bits(static_cast<u32>(left) + static_cast<u32>(right));
						break;
					case Opcode::Add64:
						bits = Bits(left + right);
						break;
					case Opcode::Sub32:
						bits = Bits(static_cast<u32>(left) - static_cast<u32>(right));
						break;
					case Opcode::Sub64:
						bits = Bits(left - right);
						break;
					case Opcode::And64:
						bits = Bits(left & right);
						break;
					case Opcode::Or64:
						bits = Bits(left | right);
						break;
					case Opcode::Xor64:
						bits = Bits(left ^ right);
						break;
					case Opcode::Nor64:
						bits = Bits(~(left | right));
						break;
					case Opcode::ShiftLeft32:
						bits = Bits(static_cast<u32>(left) << node.immediate);
						break;
					case Opcode::ShiftRightLogical32:
						bits = Bits(static_cast<u32>(left) >> node.immediate);
						break;
					case Opcode::ShiftRightArithmetic32:
						bits = Bits(static_cast<u32>(
							std::bit_cast<s32>(static_cast<u32>(left)) >> node.immediate));
						break;
					case Opcode::ShiftLeft64:
						bits = Bits(left << node.immediate);
						break;
					case Opcode::ShiftRightLogical64:
						bits = Bits(left >> node.immediate);
						break;
					case Opcode::ShiftRightArithmetic64:
						bits = Bits(
							std::bit_cast<u64>(std::bit_cast<s64>(left) >> node.immediate));
						break;
					case Opcode::CompareEqual64:
						bits = Bits(left == right);
						break;
					case Opcode::CompareNotEqual64:
						bits = Bits(left != right);
						break;
					case Opcode::CompareSignedLess64:
						bits = Bits(std::bit_cast<s64>(left) < std::bit_cast<s64>(right));
						break;
					case Opcode::CompareUnsignedLess64:
						bits = Bits(left < right);
						break;
					case Opcode::CompareSignedLessEqualZero64:
						bits = Bits(std::bit_cast<s64>(left) <= 0);
						break;
					case Opcode::CompareSignedGreaterZero64:
						bits = Bits(std::bit_cast<s64>(left) > 0);
						break;
					case Opcode::CompareSignedLessZero64:
						bits = Bits(std::bit_cast<s64>(left) < 0);
						break;
					case Opcode::CompareSignedGreaterEqualZero64:
						bits = Bits(std::bit_cast<s64>(left) >= 0);
						break;
					case Opcode::EffectiveAddress32:
						bits = Bits(static_cast<u32>(left) +
									static_cast<u32>(right));
						break;
					case Opcode::MemoryLoad:
					case Opcode::MemoryStore:
					{
						const MemoryAccessKind kind =
							static_cast<MemoryAccessKind>(node.immediate);
						const u32 unaligned_address =
							static_cast<u32>(values[node.operands[1]].bits.lo);
						const u32 alignment_mask = MemoryAlignmentMask(kind);
						if ((unaligned_address & alignment_mask) != 0)
						{
							exit_before_memory(block, node, unaligned_address,
								ExitReason::MemoryAlignment);
							return result;
						}
						const u32 address = IsQuadMemoryAccess(kind) ?
						                        (unaligned_address & ~0xfu) :
						                        unaligned_address;
						MemoryRequest request{node.source_pc, address, kind};
						if (!options.memory || !options.memory->probe)
						{
							exit_before_memory(block, node, address,
								ExitReason::MemoryObserver);
							return result;
						}
						const MemoryProbeResult probe =
							options.memory->probe(options.memory->context, request);
						if (probe != MemoryProbeResult::Direct)
						{
							const ExitReason reason =
								probe == MemoryProbeResult::Handler ?
									ExitReason::MemoryHandler :
									(probe == MemoryProbeResult::Translation ?
											ExitReason::MemoryTranslation :
											ExitReason::SelfModifyingCode);
							exit_before_memory(block, node, address, reason);
							return result;
						}

						if (node.opcode == Opcode::MemoryLoad)
						{
							if (!options.memory->read)
							{
								result.error = "direct memory load lacks a read callback";
								return result;
							}
							u128 raw{};
							if (!options.memory->read(options.memory->context, request,
									&raw))
							{
								result.error = "direct memory load callback failed";
								return result;
							}
							bits = values[node.operands[2]].bits;
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
									bits.lo = static_cast<u32>(raw.lo);
									break;
								case MemoryAccessKind::Load64:
									bits.lo = raw.lo;
									break;
								case MemoryAccessKind::Load128:
									bits = raw;
									break;
								default:
									result.error = "load node carries a store kind";
									return result;
							}
						}
						else
						{
							if (!options.memory->write)
							{
								result.error = "direct memory store lacks a write callback";
								return result;
							}
							if (!options.memory->write(options.memory->context, request,
									values[node.operands[2]].bits))
							{
								result.error = "direct memory store callback failed";
								return result;
							}
							bits = Bits(0);
						}
						break;
					}
					case Opcode::MemoryLoadValue:
						bits = values[node.operands[0]].bits;
						break;
					case Opcode::BindGpr:
						current.gpr[node.immediate] =
							values[node.operands[0]].bits;
						break;
					case Opcode::BindHi:
						current.hi = values[node.operands[0]].bits;
						break;
					case Opcode::BindLo:
						current.lo = values[node.operands[0]].bits;
						break;
					case Opcode::AdvanceCycles:
						bits = Bits(left + node.immediate);
						current.cycle = bits.lo;
						break;
				}
				values[node.id] = {node.type, bits};
			}

			const Transfer* transfer = &block.terminator.taken;
			if (block.terminator.kind == TerminatorKind::Branch &&
				values[block.terminator.condition].bits.lo == 0)
			{
				transfer = &block.terminator.not_taken;
			}
			u32 block_source_instructions =
				static_cast<u32>(block.source.size());
			if (block.terminator.kind == TerminatorKind::Branch &&
				block.terminator.likely &&
				transfer == &block.terminator.not_taken)
			{
				block_source_instructions--;
			}
			result.source_instructions_executed += block_source_instructions;
			const RuntimeValue outgoing_memory_effect =
				values[transfer->state.memory_effect];
			current = materialize(*transfer);
			if (event_due(current.cycle))
			{
				*output = current;
				result.completed = true;
				result.reason = ExitReason::EventHorizon;
				return result;
			}
			if (transfer->target_block == INVALID_BLOCK)
			{
				*output = current;
				result.completed = true;
				result.reason = transfer->external_reason;
				return result;
			}
			block_index = transfer->target_block;
			assign_parameters(program.blocks[block_index], current,
				outgoing_memory_effect);
		}
	}
} // namespace VitaEE::RegionIR
