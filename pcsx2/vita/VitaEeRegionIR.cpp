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
		constexpr u32 SA_PARAMETER = 34;
		constexpr u32 FPR_PARAMETER_BASE = 35;
		constexpr u32 FPR_COUNT = 32;
		constexpr u32 FCR0_PARAMETER = FPR_PARAMETER_BASE + FPR_COUNT;
		constexpr u32 FCR31_PARAMETER = FCR0_PARAMETER + 1;
		constexpr u32 ACC_PARAMETER = FCR31_PARAMETER + 1;
		constexpr u32 ACC_FLAG_PARAMETER = ACC_PARAMETER + 1;
		constexpr u32 VU0_VF_PARAMETER_BASE = ACC_FLAG_PARAMETER + 1;
		constexpr u32 VU0_VF_COUNT = 32;
		constexpr u32 VU0_VPU_STAT_PARAMETER =
			VU0_VF_PARAMETER_BASE + VU0_VF_COUNT;
		constexpr u32 CYCLE_PARAMETER = VU0_VPU_STAT_PARAMETER + 1;
		constexpr u32 MEMORY_EFFECT_PARAMETER = CYCLE_PARAMETER + 1;
		constexpr u32 PARAMETER_COUNT = MEMORY_EFFECT_PARAMETER + 1;
		enum class NoEffectKind : u32
		{
			Sync,
			Prefetch,
			Cache,
		};
		enum class PureMmiKind : u8
		{
			MoveFromHi1,
			MoveToHi1,
			MoveFromLo1,
			MoveToLo1,
			MoveFromHi,
			MoveFromLo,
			MoveToHi,
			MoveToLo,
			And,
			Xor,
			Or,
			Nor,
			CopyLowDoubleword,
			CopyUpperDoubleword,
			CopyHalfword,
		};
		enum class PureCop1StateKind : u8
		{
			MoveFromFpr,
			MoveToFpr,
			MoveFpr,
		};
		enum class BasicCop1ArithmeticKind : u8
		{
			Add,
			Subtract,
			Multiply,
			AddAccumulator,
			SubtractAccumulator,
			MultiplyAccumulator,
		};
		enum class CompoundCop1ArithmeticKind : u8
		{
			MultiplyAdd,
			MultiplySubtract,
		};
		enum class Cop1CompareKind : u8
		{
			False,
			Equal,
			Less,
			LessEqual,
		};

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
		constexpr u32 FCR31_C = 0x00800000u;

		constexpr u32 RS(u32 op) { return (op >> 21) & 0x1fu; }
		constexpr u32 RT(u32 op) { return (op >> 16) & 0x1fu; }
		constexpr u32 RD(u32 op) { return (op >> 11) & 0x1fu; }
		constexpr u32 SA(u32 op) { return (op >> 6) & 0x1fu; }
		constexpr u32 FUNCT(u32 op) { return op & 0x3fu; }
		constexpr u32 FS(u32 op) { return RD(op); }
		constexpr u32 FD(u32 op) { return SA(op); }
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

		bool IsRegisterJump(u32 op)
		{
			return (op >> 26) == 0x00 &&
			       (FUNCT(op) == 0x08 || FUNCT(op) == 0x09);
		}

		bool CanLowerRegisterJump(u32 op, const LiftOptions& options)
		{
			// SCE defines JALR rd==rs as undefined. PCSX2's tier-zero provider
			// snapshots it deterministically, but Region IR does not need to claim
			// an undefined source pair to cover ordinary calls/returns.
			return IsRegisterJump(op) &&
			       !(FUNCT(op) == 0x09 && RD(op) == RS(op)) &&
			       !options.goemon_tlb_hack;
		}

		bool IsLinkedControl(u32 op)
		{
			return (R5900::GetInstruction(op).flags & IS_LINKED) != 0;
		}

		u32 ControlLinkRegister(u32 op)
		{
			return IsRegisterJump(op) ? RD(op) : 31;
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
				case 0x04: // SLLV
				case 0x06: // SRLV
				case 0x07: // SRAV
				case 0x0a: // MOVZ
				case 0x0b: // MOVN
				case 0x0f: // SYNC
				case 0x10: // MFHI
				case 0x11: // MTHI
				case 0x12: // MFLO
				case 0x13: // MTLO
				case 0x14: // DSLLV
				case 0x16: // DSRLV
				case 0x17: // DSRAV
				case 0x21: // ADDU
				case 0x23: // SUBU
				case 0x24: // AND
				case 0x25: // OR
				case 0x26: // XOR
				case 0x27: // NOR
				case 0x28: // MFSA
				case 0x29: // MTSA
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

		bool DecodeNoEffect(u32 op, const LiftOptions& options, NoEffectKind* kind)
		{
			NoEffectKind decoded{};
			if ((op >> 26) == 0x00 && FUNCT(op) == 0x0f)
				decoded = NoEffectKind::Sync;
			else if ((op >> 26) == 0x33)
				decoded = NoEffectKind::Prefetch;
			else if ((op >> 26) == 0x2f && !options.ee_cache_enabled)
				decoded = NoEffectKind::Cache;
			else
				return false;
			if (kind)
				*kind = decoded;
			return true;
		}

		bool IsVariableShift(u32 op)
		{
			if ((op >> 26) != 0x00)
				return false;
			switch (FUNCT(op))
			{
				case 0x04: // SLLV
				case 0x06: // SRLV
				case 0x07: // SRAV
				case 0x14: // DSLLV
				case 0x16: // DSRLV
				case 0x17: // DSRAV
					return true;
				default:
					return false;
			}
		}

		bool IsConditionalMove(u32 op)
		{
			return (op >> 26) == 0x00 &&
			       (FUNCT(op) == 0x0a || FUNCT(op) == 0x0b);
		}

		bool IsMoveFromHiLo(u32 op)
		{
			return (op >> 26) == 0x00 &&
			       (FUNCT(op) == 0x10 || FUNCT(op) == 0x12);
		}

		bool IsMoveToHiLo(u32 op)
		{
			return (op >> 26) == 0x00 &&
			       (FUNCT(op) == 0x11 || FUNCT(op) == 0x13);
		}

		bool IsMoveFromSa(u32 op)
		{
			return (op >> 26) == 0x00 && FUNCT(op) == 0x28;
		}

		bool IsMoveToSa(u32 op)
		{
			if ((op >> 26) == 0x00)
				return FUNCT(op) == 0x29;
			return (op >> 26) == 0x01 &&
			       (RT(op) == 0x18 || RT(op) == 0x19);
		}

		bool DecodePureMmi(u32 op, PureMmiKind* kind)
		{
			if ((op >> 26) != 0x1c)
				return false;

			PureMmiKind decoded{};
			switch (FUNCT(op))
			{
				case 0x10:
					decoded = PureMmiKind::MoveFromHi1;
					break;
				case 0x11:
					decoded = PureMmiKind::MoveToHi1;
					break;
				case 0x12:
					decoded = PureMmiKind::MoveFromLo1;
					break;
				case 0x13:
					decoded = PureMmiKind::MoveToLo1;
					break;
				case 0x09: // MMI2
					switch (SA(op))
					{
						case 0x08:
							decoded = PureMmiKind::MoveFromHi;
							break;
						case 0x09:
							decoded = PureMmiKind::MoveFromLo;
							break;
						case 0x0e:
							decoded = PureMmiKind::CopyLowDoubleword;
							break;
						case 0x12:
							decoded = PureMmiKind::And;
							break;
						case 0x13:
							decoded = PureMmiKind::Xor;
							break;
						default:
							return false;
					}
					break;
				case 0x29: // MMI3
					switch (SA(op))
					{
						case 0x08:
							decoded = PureMmiKind::MoveToHi;
							break;
						case 0x09:
							decoded = PureMmiKind::MoveToLo;
							break;
						case 0x0e:
							decoded = PureMmiKind::CopyUpperDoubleword;
							break;
						case 0x12:
							decoded = PureMmiKind::Or;
							break;
						case 0x13:
							decoded = PureMmiKind::Nor;
							break;
						case 0x1b:
							decoded = PureMmiKind::CopyHalfword;
							break;
						default:
							return false;
					}
					break;
				default:
					return false;
			}
			if (kind)
				*kind = decoded;
			return true;
		}

		bool IsPureMmiGprWrite(PureMmiKind kind)
		{
			return kind != PureMmiKind::MoveToHi1 &&
			       kind != PureMmiKind::MoveToLo1 &&
			       kind != PureMmiKind::MoveToHi &&
			       kind != PureMmiKind::MoveToLo;
		}

		bool IsPureMmiHiWrite(PureMmiKind kind)
		{
			return kind == PureMmiKind::MoveToHi1 ||
			       kind == PureMmiKind::MoveToHi;
		}

		bool IsPureMmiLoWrite(PureMmiKind kind)
		{
			return kind == PureMmiKind::MoveToLo1 ||
			       kind == PureMmiKind::MoveToLo;
		}

		bool DecodePureCop1State(u32 op, PureCop1StateKind* kind)
		{
			if ((op >> 26) != 0x11)
				return false;

			PureCop1StateKind decoded{};
			switch (RS(op))
			{
				case 0x00: // MFC1 rt, fs
					if ((op & 0x7ffu) != 0)
						return false;
					decoded = PureCop1StateKind::MoveFromFpr;
					break;
				case 0x04: // MTC1 rt, fs
					if ((op & 0x7ffu) != 0)
						return false;
					decoded = PureCop1StateKind::MoveToFpr;
					break;
				case 0x10: // MOV.S fd, fs
					if (RT(op) != 0 || FUNCT(op) != 0x06)
						return false;
					decoded = PureCop1StateKind::MoveFpr;
					break;
				default:
					return false;
			}
			if (kind)
				*kind = decoded;
			return true;
		}

		bool DecodeBasicCop1Arithmetic(u32 op, BasicCop1ArithmeticKind* kind)
		{
			if ((op >> 26) != 0x11 || RS(op) != 0x10)
				return false;

			BasicCop1ArithmeticKind decoded{};
			switch (FUNCT(op))
			{
				case 0x00:
					decoded = BasicCop1ArithmeticKind::Add;
					break;
				case 0x01:
					decoded = BasicCop1ArithmeticKind::Subtract;
					break;
				case 0x02:
					decoded = BasicCop1ArithmeticKind::Multiply;
					break;
				case 0x18:
					decoded = BasicCop1ArithmeticKind::AddAccumulator;
					break;
				case 0x19:
					decoded = BasicCop1ArithmeticKind::SubtractAccumulator;
					break;
				case 0x1a:
					decoded = BasicCop1ArithmeticKind::MultiplyAccumulator;
					break;
				default:
					return false;
			}

			// SCE reserves fd as zero for the accumulator-destination forms.
			if (FUNCT(op) >= 0x18 && FD(op) != 0)
				return false;
			if (kind)
				*kind = decoded;
			return true;
		}

		bool IsBasicCop1Accumulator(BasicCop1ArithmeticKind kind)
		{
			return kind == BasicCop1ArithmeticKind::AddAccumulator ||
			       kind == BasicCop1ArithmeticKind::SubtractAccumulator ||
			       kind == BasicCop1ArithmeticKind::MultiplyAccumulator;
		}

		bool DecodeCompoundCop1Arithmetic(u32 op,
			CompoundCop1ArithmeticKind* kind)
		{
			if ((op >> 26) != 0x11 || RS(op) != 0x10)
				return false;

			CompoundCop1ArithmeticKind decoded{};
			switch (FUNCT(op))
			{
				case 0x1c:
					decoded = CompoundCop1ArithmeticKind::MultiplyAdd;
					break;
				case 0x1d:
					decoded = CompoundCop1ArithmeticKind::MultiplySubtract;
					break;
				default:
					return false;
			}
			if (kind)
				*kind = decoded;
			return true;
		}

		Opcode CompoundCop1FinalRawOpcode(CompoundCop1ArithmeticKind kind)
		{
			return kind == CompoundCop1ArithmeticKind::MultiplyAdd ?
				Opcode::Cop1AddRaw : Opcode::Cop1SubRaw;
		}

		bool IsCop1OuArithmetic(u32 op)
		{
			return DecodeBasicCop1Arithmetic(op, nullptr) ||
			       DecodeCompoundCop1Arithmetic(op, nullptr);
		}

		bool DecodeCop1Compare(u32 op, Cop1CompareKind* kind)
		{
			if ((op >> 26) != 0x11 || RS(op) != 0x10 || FD(op) != 0)
				return false;

			Cop1CompareKind decoded{};
			switch (FUNCT(op))
			{
				case 0x30:
					decoded = Cop1CompareKind::False;
					break;
				case 0x32:
					decoded = Cop1CompareKind::Equal;
					break;
				case 0x34:
					decoded = Cop1CompareKind::Less;
					break;
				case 0x36:
					decoded = Cop1CompareKind::LessEqual;
					break;
				default:
					return false;
			}
			if (kind)
				*kind = decoded;
			return true;
		}

		Opcode Cop1CompareOpcode(Cop1CompareKind kind)
		{
			switch (kind)
			{
				case Cop1CompareKind::Equal:
					return Opcode::Cop1CompareEqual;
				case Cop1CompareKind::Less:
					return Opcode::Cop1CompareLess;
				case Cop1CompareKind::LessEqual:
					return Opcode::Cop1CompareLessEqual;
				case Cop1CompareKind::False:
					break;
			}
			return Opcode::Parameter;
		}

		bool IsCop1ConvertWord(u32 op)
		{
			// The SCE encoding reserves ft as zero. Undefined encodings remain on
			// tier zero rather than inheriting an accidental decoder behavior.
			return (op >> 26) == 0x11 && RS(op) == 0x10 && RT(op) == 0 &&
			       FUNCT(op) == 0x24;
		}

		bool IsCop1ConvertSingle(u32 op)
		{
			// CVT.S.W uses the W format and reserves ft as zero.
			return (op >> 26) == 0x11 && RS(op) == 0x14 && RT(op) == 0 &&
			       FUNCT(op) == 0x20;
		}

		Opcode BasicCop1RawOpcode(BasicCop1ArithmeticKind kind)
		{
			switch (kind)
			{
				case BasicCop1ArithmeticKind::Add:
				case BasicCop1ArithmeticKind::AddAccumulator:
					return Opcode::Cop1AddRaw;
				case BasicCop1ArithmeticKind::Subtract:
				case BasicCop1ArithmeticKind::SubtractAccumulator:
					return Opcode::Cop1SubRaw;
				case BasicCop1ArithmeticKind::Multiply:
				case BasicCop1ArithmeticKind::MultiplyAccumulator:
					return Opcode::Cop1MulRaw;
			}
			return Opcode::Parameter;
		}

		u32 NormalizeCop1Input(u32 value)
		{
			const u32 exponent = value & COP1_EXPONENT;
			if (exponent == 0)
				return value & COP1_SIGN;
			if (exponent == COP1_EXPONENT)
				return (value & COP1_SIGN) | COP1_MAX_FINITE;
			return value;
		}

		u32 EvaluateBasicCop1Raw(Opcode opcode, u32 left_bits, u32 right_bits)
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

		u32 ClampBasicCop1Result(u32 raw)
		{
			if ((raw & ~COP1_SIGN) == COP1_EXPONENT)
				return (raw & COP1_SIGN) | COP1_MAX_FINITE;
			if ((raw & COP1_EXPONENT) == 0 && (raw & COP1_FRACTION) != 0)
				return raw & COP1_SIGN;
			return raw;
		}

		u32 UpdateBasicCop1OuFlags(u32 fcr31, u32 raw)
		{
			// FPU.cpp::checkOverflow() returns before checkUnderflow(), so an
			// overflow leaves the previous U cause untouched while setting O/SO.
			if ((raw & ~COP1_SIGN) == COP1_EXPONENT)
				return fcr31 | FCR31_O | FCR31_SO;

			fcr31 &= ~FCR31_O;
			if ((raw & COP1_EXPONENT) == 0 && (raw & COP1_FRACTION) != 0)
				return fcr31 | FCR31_U | FCR31_SU;
			return fcr31 & ~FCR31_U;
		}

		u32 ConvertCop1Word(u32 raw)
		{
			const u32 exponent_bits = raw & COP1_EXPONENT;
			if (exponent_bits > COP1_CVT_W_MAX_EXPONENT)
				return (raw & COP1_SIGN) != 0 ? 0x80000000u : 0x7fffffffu;

			const u32 exponent = exponent_bits >> COP1_MANTISSA_BITS;
			if (exponent < COP1_EXPONENT_BIAS)
				return 0;

			const u32 unbiased = exponent - COP1_EXPONENT_BIAS;
			const u32 mantissa = (raw & COP1_FRACTION) | COP1_IMPLICIT_MANTISSA;
			const u32 magnitude = unbiased >= COP1_MANTISSA_BITS ?
				mantissa << (unbiased - COP1_MANTISSA_BITS) :
				mantissa >> (COP1_MANTISSA_BITS - unbiased);
			return (raw & COP1_SIGN) != 0 ? 0u - magnitude : magnitude;
		}

		u32 ConvertCop1Single(u32 raw)
		{
			// PCSX2 FPU.cpp::CVT_S() interprets the source word as signed I32.
			// The cast uses the active FPUFPCR (ChopZero by default), matching
			// the Cortex-A9 VCVT lowering in VitaEeBlockCompiler.
			const float converted = static_cast<float>(static_cast<s32>(raw));
			return std::bit_cast<u32>(converted);
		}

		bool EvaluateCop1Compare(Opcode opcode, u32 left_bits, u32 right_bits)
		{
			const float left = std::bit_cast<float>(left_bits);
			const float right = std::bit_cast<float>(right_bits);
			switch (opcode)
			{
				case Opcode::Cop1CompareEqual:
					return left == right;
				case Opcode::Cop1CompareLess:
					return left < right;
				case Opcode::Cop1CompareLessEqual:
					return left <= right;
				default:
					return false;
			}
		}

		u32 UpdateCop1ConditionFlag(u32 fcr31, bool condition)
		{
			return condition ? (fcr31 | FCR31_C) : (fcr31 & ~FCR31_C);
		}

		bool IsCop1ControlWrite(u32 op)
		{
			// CTC1 is defined only for FCR31. Reserved control registers remain
			// with tier zero rather than inheriting a host-specific no-op.
			return (op >> 26) == 0x11 && RS(op) == 0x06 && FS(op) == 31 &&
			       (op & 0x7ffu) == 0;
		}

		bool IsExtendedScalarGprWrite(u32 op)
		{
			return IsVariableShift(op) || IsConditionalMove(op) ||
			       IsMoveFromHiLo(op) || IsMoveFromSa(op);
		}

		bool CanLowerPureNonBranch(u32 op, const LiftOptions& options)
		{
			if (IsAnyControlFlow(op))
				return false;
			if (DecodeNoEffect(op, options, nullptr))
				return true;

			switch (op >> 26)
			{
				case 0x00:
					return CanLowerSpecial(op);
				case 0x01:
					return IsMoveToSa(op);
				case 0x09: // ADDIU
				case 0x0a: // SLTI
				case 0x0b: // SLTIU
				case 0x0c: // ANDI
				case 0x0d: // ORI
				case 0x0e: // XORI
				case 0x0f: // LUI
				case 0x19: // DADDIU
					return true;
				case 0x1c: // Pure, non-multiplying MMI moves/logical/copies.
					return DecodePureMmi(op, nullptr);
				case 0x11: // Raw COP1 state plus exact S/W-format numerics.
					return DecodePureCop1State(op, nullptr) || IsCop1ControlWrite(op) ||
					       DecodeBasicCop1Arithmetic(op, nullptr) ||
					       DecodeCompoundCop1Arithmetic(op, nullptr) ||
					       DecodeCop1Compare(op, nullptr) ||
					       IsCop1ConvertWord(op) || IsCop1ConvertSingle(op);
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
				case 0x31: // LWC1
					decoded = MemoryAccessKind::LoadF32Bits;
					break;
				case 0x37:
					decoded = MemoryAccessKind::Load64;
					break;
				case 0x1e:
					decoded = MemoryAccessKind::Load128;
					break;
				case 0x36: // LQC2
					decoded = MemoryAccessKind::LoadVu0Vector;
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
				case 0x39: // SWC1
					decoded = MemoryAccessKind::StoreF32Bits;
					break;
				case 0x3f:
					decoded = MemoryAccessKind::Store64;
					break;
				case 0x1f:
					decoded = MemoryAccessKind::Store128;
					break;
				case 0x3e: // SQC2
					decoded = MemoryAccessKind::StoreVu0Vector;
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
			return kind <= MemoryAccessKind::LoadVu0Vector;
		}

		bool IsFprMemoryAccess(MemoryAccessKind kind)
		{
			return kind == MemoryAccessKind::LoadF32Bits ||
			       kind == MemoryAccessKind::StoreF32Bits;
		}

		bool IsVu0MemoryAccess(MemoryAccessKind kind)
		{
			return kind == MemoryAccessKind::LoadVu0Vector ||
			       kind == MemoryAccessKind::StoreVu0Vector;
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
				case MemoryAccessKind::LoadF32Bits:
				case MemoryAccessKind::Store32:
				case MemoryAccessKind::StoreF32Bits:
					return 3;
				case MemoryAccessKind::Load64:
				case MemoryAccessKind::Store64:
					return 7;
				// Unlike EE LQ/SQ's masked address contract, the SCE COP2 transfer
				// contract requires a 128-bit-aligned effective address.
				case MemoryAccessKind::LoadVu0Vector:
				case MemoryAccessKind::StoreVu0Vector:
					return 15;
				default:
					return 0;
			}
		}

		bool IsQuadMemoryAccess(MemoryAccessKind kind)
		{
			return kind == MemoryAccessKind::Load128 ||
			       kind == MemoryAccessKind::Store128 ||
			       IsVu0MemoryAccess(kind);
		}

		bool CanLowerNonBranch(u32 op, const LiftOptions& options)
		{
			return CanLowerPureNonBranch(op, options) ||
			       DecodeMemoryAccess(op, nullptr);
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

		const SourceBlockContract* FindSourceBlockContract(
			const std::vector<SourceBlockContract>& contracts, u32 start_pc)
		{
			const auto found = std::lower_bound(contracts.begin(), contracts.end(),
				start_pc, [](const SourceBlockContract& contract, u32 pc) {
					return contract.start_pc < pc;
				});
			return found != contracts.end() && found->start_pc == start_pc ?
				&*found : nullptr;
		}

		bool ValidateSourceBlockContracts(u32 source_base_pc,
			const std::vector<u32>& source_words,
			const std::vector<SourceBlockContract>& contracts, u32 entry_pc,
			const LiftOptions& options, u32* failure_pc, std::string* detail)
		{
			const auto fail = [&](u32 pc, const char* message) {
				if (failure_pc)
					*failure_pc = pc;
				if (detail)
					*detail = message;
				return false;
			};
			if (contracts.empty())
				return true;

			const u64 source_end = static_cast<u64>(source_base_pc) +
				static_cast<u64>(source_words.size()) * sizeof(u32);
			u64 previous_end = 0;
			for (u32 index = 0; index < contracts.size(); index++)
			{
				const SourceBlockContract& contract = contracts[index];
				if ((contract.start_pc & 3u) != 0 || contract.instruction_count == 0 ||
					(contract.dependency_start_pc & 3u) != 0 ||
					contract.dependency_instruction_count == 0)
				{
					return fail(contract.start_pc,
						"source-block range is empty or unaligned");
				}
				const u64 end = static_cast<u64>(contract.start_pc) +
					static_cast<u64>(contract.instruction_count) * sizeof(u32);
				const u64 dependency_end =
					static_cast<u64>(contract.dependency_start_pc) +
					static_cast<u64>(contract.dependency_instruction_count) * sizeof(u32);
				if (end > static_cast<u64>(UINT32_MAX) ||
					dependency_end > static_cast<u64>(UINT32_MAX) ||
					contract.start_pc < source_base_pc || end > source_end ||
					contract.dependency_start_pc < source_base_pc ||
					dependency_end > source_end ||
					contract.start_pc < contract.dependency_start_pc || end > dependency_end)
				{
					return fail(contract.start_pc,
						"source-block or dependency range leaves the immutable image");
				}
				if (index != 0 && contract.start_pc < previous_end)
					return fail(contract.start_pc, "source-block contracts overlap");
				previous_end = end;
			}

			const auto entry_contract = std::lower_bound(contracts.begin(), contracts.end(),
				entry_pc, [](const SourceBlockContract& contract, u32 pc) {
					return contract.start_pc < pc;
				});
			if (entry_contract == contracts.end() || entry_contract->start_pc != entry_pc)
				return fail(entry_pc, "entry is not an attested source-block start");
			if (entry_contract->charged_scaled_cycles_before != 0)
				return fail(entry_pc,
					"entry begins inside a charged A32 split dependency");
			if (entry_contract != contracts.begin())
			{
				const SourceBlockContract& predecessor = *std::prev(entry_contract);
				const u32 predecessor_end = predecessor.start_pc +
					predecessor.instruction_count * sizeof(u32);
				if (predecessor_end == entry_pc && !predecessor.scheduler_test_at_end)
					return fail(entry_pc,
						"entry follows an untested scheduler continuation");
			}

			for (const SourceBlockContract& contract : contracts)
			{
				const u32 end = contract.start_pc +
					contract.instruction_count * sizeof(u32);
				if (!contract.scheduler_test_at_end &&
					!FindSourceBlockContract(contracts, end))
				{
					return fail(contract.start_pc,
						"scheduler-elided fragment has no owned continuation");
				}
			}

			std::set<std::pair<u32, u32>> verified_dependencies;
			for (const SourceBlockContract& owner : contracts)
			{
				const std::pair<u32, u32> dependency = {
					owner.dependency_start_pc, owner.dependency_instruction_count};
				if (!verified_dependencies.insert(dependency).second)
					continue;

				const u32 dependency_end = owner.dependency_start_pc +
					owner.dependency_instruction_count * sizeof(u32);
				u32 next_pc = owner.dependency_start_pc;
				u32 charged_cycles = 0;
				for (const SourceBlockContract& fragment : contracts)
				{
					if (fragment.dependency_start_pc != owner.dependency_start_pc ||
						fragment.dependency_instruction_count != owner.dependency_instruction_count)
					{
						continue;
					}
					if (fragment.start_pc != next_pc ||
						fragment.charged_scaled_cycles_before != charged_cycles)
					{
						return fail(fragment.start_pc,
							"split dependency is not a complete charged-cycle chain");
					}
					u32 raw_cycles = 0;
					for (u32 instruction = 0;
						instruction < fragment.instruction_count; instruction++)
					{
						raw_cycles += RawRecompilerCycles(ReadSourceWord(source_base_pc,
							source_words, fragment.start_pc + instruction * sizeof(u32)),
							options.cycle_factor);
					}
					next_pc += fragment.instruction_count * sizeof(u32);
					charged_cycles += ScaleBlockCycles(raw_cycles, options.ee_cycle_rate);
				}
				if (next_pc != dependency_end)
					return fail(owner.dependency_start_pc,
						"split dependency is not fully represented by source blocks");
			}
			return true;
		}

		ExitReason ClassifyExternalResume(u32 source_base_pc,
			const std::vector<u32>& source_words, u32 pc,
			const LiftOptions& options)
		{
			if (!ContainsPc(source_base_pc, static_cast<u32>(source_words.size()), pc))
				return ExitReason::RegionBoundary;

			const u32 op = ReadSourceWord(source_base_pc, source_words, pc);
			if (IsConditionalBranch(op) || CanLowerStaticJump(op, options) ||
				CanLowerRegisterJump(op, options))
			{
				const u32 delay_pc = pc + sizeof(u32);
				if (pc <= UINT32_MAX - sizeof(u32) &&
					ContainsPc(source_base_pc, static_cast<u32>(source_words.size()), delay_pc))
				{
					const u32 delay = ReadSourceWord(source_base_pc, source_words, delay_pc);
					if (!IsAnyControlFlow(delay) &&
						CanLowerPureNonBranch(delay, options))
						return ExitReason::RegionBoundary;
				}
				return ExitReason::UnsupportedControlFlow;
			}
			if (CanLowerNonBranch(op, options))
				return ExitReason::RegionBoundary;
			return ClassifyExit(op);
		}

		enum class RawControlKind : u8
		{
			None,
			ConditionalBranch,
			StaticJump,
			RegisterJump,
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
			const std::vector<SourceBlockContract>& source_blocks,
			const std::set<u32>& leaders, u32 start_pc, const LiftOptions& options,
			RawBlock* output,
			LiftFailure* failure, u32* failure_pc)
		{
			RawBlock raw{};
			raw.pc = start_pc;
			u32 pc = start_pc;
			u32 contract_end_pc = 0;
			if (!source_blocks.empty())
			{
				const SourceBlockContract* contract =
					FindSourceBlockContract(source_blocks, start_pc);
				if (!contract || contract->instruction_count >
						(UINT32_MAX - contract->start_pc) / sizeof(u32))
				{
					*failure = LiftFailure::SourceBlockContract;
					*failure_pc = start_pc;
					return false;
				}
				contract_end_pc = contract->start_pc +
					contract->instruction_count * sizeof(u32);
			}

			for (;;)
			{
				if (!source_blocks.empty() && pc == contract_end_pc)
				{
					raw.transfer_pc = pc;
					break;
				}
				if (!source_blocks.empty() && pc > contract_end_pc)
				{
					*failure = LiftFailure::SourceBlockContract;
					*failure_pc = pc;
					return false;
				}
				if (pc != start_pc && leaders.contains(pc))
				{
					if (!source_blocks.empty())
					{
						*failure = LiftFailure::SourceBlockContract;
						*failure_pc = pc;
						return false;
					}
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
				const bool register_jump = CanLowerRegisterJump(op, options);
				if (conditional_branch || static_jump || register_jump)
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
					if (!source_blocks.empty() && delay_pc + sizeof(u32) != contract_end_pc)
					{
						*failure = LiftFailure::SourceBlockContract;
						*failure_pc = pc;
						return false;
					}

					// A branch and its delay slot are one architectural unit. If the
					// delay slot is not yet expressible, leave both to the existing
					// compiler/interpreter at a canonical side exit.
					if (!CanLowerPureNonBranch(delay, options))
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
					                       (static_jump ? RawControlKind::StaticJump :
					                                      RawControlKind::RegisterJump);
					raw.branch_pc = pc;
					raw.branch_opcode = op;
					raw.delay = {delay_pc, delay, true};
					break;
				}

				if (!CanLowerNonBranch(op, options))
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
					block.parameters.sa = AddNode(block, Opcode::Parameter, ValueType::I32,
						{}, 0, SA_PARAMETER, 0, block.pc);
					for (u32 fpr = 0; fpr < FPR_COUNT; fpr++)
					{
						block.parameters.fpr[fpr] = AddNode(block, Opcode::Parameter,
							ValueType::F32Bits, {}, 0, FPR_PARAMETER_BASE + fpr, 0,
							block.pc);
						if (block.parameters.fpr[fpr] == INVALID_VALUE)
							return false;
					}
					block.parameters.fcr0 = AddNode(block, Opcode::Parameter,
						ValueType::I32, {}, 0, FCR0_PARAMETER, 0, block.pc);
					block.parameters.fcr31 = AddNode(block, Opcode::Parameter,
						ValueType::I32, {}, 0, FCR31_PARAMETER, 0, block.pc);
					block.parameters.acc = AddNode(block, Opcode::Parameter,
						ValueType::F32Bits, {}, 0, ACC_PARAMETER, 0, block.pc);
					block.parameters.acc_flag = AddNode(block, Opcode::Parameter,
						ValueType::I32, {}, 0, ACC_FLAG_PARAMETER, 0, block.pc);
					for (u32 vf = 0; vf < VU0_VF_COUNT; vf++)
					{
						block.parameters.vu0_vf[vf] = AddNode(block,
							Opcode::Parameter, ValueType::I128, {}, 0,
							VU0_VF_PARAMETER_BASE + vf, 0, block.pc);
						if (block.parameters.vu0_vf[vf] == INVALID_VALUE)
							return false;
					}
					block.parameters.vu0_vpu_stat = AddNode(block,
						Opcode::Parameter, ValueType::I32, {}, 0,
						VU0_VPU_STAT_PARAMETER, 0, block.pc);
					block.parameters.cycle =
						AddNode(block, Opcode::Parameter, ValueType::Cycle, {}, 0,
							CYCLE_PARAMETER, 0, block.pc);
					block.parameters.memory_effect =
						AddNode(block, Opcode::Parameter, ValueType::MemoryEffect, {}, 0,
							MEMORY_EFFECT_PARAMETER, 0, block.pc);
					if (block.parameters.hi == INVALID_VALUE ||
						block.parameters.lo == INVALID_VALUE ||
						block.parameters.sa == INVALID_VALUE ||
						block.parameters.fcr0 == INVALID_VALUE ||
						block.parameters.fcr31 == INVALID_VALUE ||
						block.parameters.acc == INVALID_VALUE ||
						block.parameters.acc_flag == INVALID_VALUE ||
						block.parameters.vu0_vpu_stat == INVALID_VALUE ||
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

			ValueId Ternary(Block& block, Opcode opcode, ValueType type, ValueId first,
				ValueId second, ValueId third, u32 source_pc)
			{
				return AddNode(block, opcode, type, {first, second, third}, 3, 0, 0,
					source_pc);
			}

			ValueId Constant32(Block& block, u32 value, u32 source_pc)
			{
				return AddNode(block, Opcode::ConstantI32, ValueType::I32, {}, 0, 0, value,
					source_pc);
			}

			ValueId ConstantBool(Block& block, bool value, u32 source_pc)
			{
				return AddNode(block, Opcode::ConstantI1, ValueType::I1, {}, 0, 0,
					value ? 1u : 0u, source_pc);
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

			ValueId AddressFromI32(Block& block, ValueId value, u32 source_pc)
			{
				return Unary(block, Opcode::AddressFromI32, ValueType::Address, value,
					source_pc);
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

			bool WriteFullGpr(Block& block, StateMap* state, u32 reg, ValueId value,
				u32 source_pc)
			{
				if (reg == 0)
					return true;
				if (value == INVALID_VALUE ||
					AddNode(block, Opcode::BindGpr, ValueType::Void,
						{value, INVALID_VALUE, INVALID_VALUE}, 1, reg, 0,
						source_pc) == INVALID_VALUE)
				{
					return false;
				}
				state->gpr[reg] = value;
				return true;
			}

			bool WriteHiLoLow64(Block& block, StateMap* state, bool hi, ValueId low,
				u32 source_pc)
			{
				ValueId& destination = hi ? state->hi : state->lo;
				const ValueId complete = Binary(block, Opcode::ReplaceLow64,
					ValueType::I128, destination, low, source_pc);
				const Opcode bind = hi ? Opcode::BindHi : Opcode::BindLo;
				if (complete == INVALID_VALUE ||
					AddNode(block, bind, ValueType::Void,
						{complete, INVALID_VALUE, INVALID_VALUE}, 1, 0, 0,
						source_pc) == INVALID_VALUE)
				{
					return false;
				}
				destination = complete;
				return true;
			}

			bool WriteHiLoHigh64(Block& block, StateMap* state, bool hi, ValueId high,
				u32 source_pc)
			{
				ValueId& destination = hi ? state->hi : state->lo;
				const ValueId complete = Binary(block, Opcode::ReplaceHigh64,
					ValueType::I128, destination, high, source_pc);
				const Opcode bind = hi ? Opcode::BindHi : Opcode::BindLo;
				if (complete == INVALID_VALUE ||
					AddNode(block, bind, ValueType::Void,
						{complete, INVALID_VALUE, INVALID_VALUE}, 1, 0, 0,
						source_pc) == INVALID_VALUE)
				{
					return false;
				}
				destination = complete;
				return true;
			}

			bool WriteFullHiLo(Block& block, StateMap* state, bool hi, ValueId value,
				u32 source_pc)
			{
				const Opcode bind = hi ? Opcode::BindHi : Opcode::BindLo;
				if (value == INVALID_VALUE ||
					AddNode(block, bind, ValueType::Void,
						{value, INVALID_VALUE, INVALID_VALUE}, 1, 0, 0,
						source_pc) == INVALID_VALUE)
				{
					return false;
				}
				(hi ? state->hi : state->lo) = value;
				return true;
			}

			bool WriteSa(Block& block, StateMap* state, ValueId value, u32 source_pc)
			{
				if (value == INVALID_VALUE ||
					AddNode(block, Opcode::BindSa, ValueType::Void,
						{value, INVALID_VALUE, INVALID_VALUE}, 1, 0, 0,
						source_pc) == INVALID_VALUE)
				{
					return false;
				}
				state->sa = value;
				return true;
			}

			bool WriteFpr(Block& block, StateMap* state, u32 reg, ValueId value,
				u32 source_pc)
			{
				if (value == INVALID_VALUE ||
					AddNode(block, Opcode::BindFpr, ValueType::Void,
						{value, INVALID_VALUE, INVALID_VALUE}, 1, reg, 0,
						source_pc) == INVALID_VALUE)
				{
					return false;
				}
				state->fpr[reg] = value;
				return true;
			}

			bool WriteVu0Vf(Block& block, StateMap* state, u32 reg, ValueId value,
				u32 source_pc)
			{
				// VF0 is the architectural (0,0,0,1) constant. LQC2 still performs
				// its memory read, but discards the loaded value.
				if (reg == 0)
					return true;
				if (value == INVALID_VALUE ||
					AddNode(block, Opcode::BindVu0Vf, ValueType::Void,
						{value, INVALID_VALUE, INVALID_VALUE}, 1, reg, 0,
						source_pc) == INVALID_VALUE)
				{
					return false;
				}
				state->vu0_vf[reg] = value;
				return true;
			}

			bool WriteFcr31(Block& block, StateMap* state, ValueId value,
				u32 source_pc)
			{
				if (value == INVALID_VALUE ||
					AddNode(block, Opcode::BindFcr31, ValueType::Void,
						{value, INVALID_VALUE, INVALID_VALUE}, 1, 31, 0,
						source_pc) == INVALID_VALUE)
				{
					return false;
				}
				state->fcr31 = value;
				return true;
			}

			bool WriteAcc(Block& block, StateMap* state, ValueId value,
				u32 source_pc)
			{
				if (value == INVALID_VALUE ||
					AddNode(block, Opcode::BindAcc, ValueType::Void,
						{value, INVALID_VALUE, INVALID_VALUE}, 1, 0, 0,
						source_pc) == INVALID_VALUE)
				{
					return false;
				}
				state->acc = value;
				return true;
			}

			bool LowerBasicCop1Arithmetic(Block& block, StateMap* state, u32 op,
				u32 pc, BasicCop1ArithmeticKind kind)
			{
				const u32 fs = FS(op);
				const u32 ft = RT(op);
				const ValueId left = Unary(block, Opcode::Cop1NormalizeInput,
					ValueType::F32Bits, state->fpr[fs], pc);
				if (left == INVALID_VALUE)
					return false;
				const ValueId right = fs == ft ? left :
					Unary(block, Opcode::Cop1NormalizeInput, ValueType::F32Bits,
						state->fpr[ft], pc);
				if (right == INVALID_VALUE)
					return false;
				const ValueId raw = Binary(block, BasicCop1RawOpcode(kind),
					ValueType::F32Bits, left, right, pc);
				if (raw == INVALID_VALUE)
					return false;
				const ValueId result = Unary(block, Opcode::Cop1ClampOuResult,
					ValueType::F32Bits, raw, pc);
				if (result == INVALID_VALUE)
					return false;
				const ValueId flags = Binary(block, Opcode::Cop1UpdateOuFlags,
					ValueType::I32, state->fcr31, raw, pc);
				if (flags == INVALID_VALUE || !WriteFcr31(block, state, flags, pc))
				{
					return false;
				}
				return IsBasicCop1Accumulator(kind) ?
					WriteAcc(block, state, result, pc) :
					WriteFpr(block, state, FD(op), result, pc);
			}

			bool LowerCop1Compare(Block& block, StateMap* state, u32 op, u32 pc,
				Cop1CompareKind kind)
			{
				ValueId condition = INVALID_VALUE;
				if (kind == Cop1CompareKind::False)
				{
					condition = ConstantBool(block, false, pc);
				}
				else
				{
					const u32 fs = FS(op);
					const u32 ft = RT(op);
					const ValueId left = Unary(block, Opcode::Cop1NormalizeInput,
						ValueType::F32Bits, state->fpr[fs], pc);
					if (left == INVALID_VALUE)
						return false;
					const ValueId right = fs == ft ? left :
						Unary(block, Opcode::Cop1NormalizeInput, ValueType::F32Bits,
							state->fpr[ft], pc);
					if (right == INVALID_VALUE)
						return false;
					condition = Binary(block, Cop1CompareOpcode(kind), ValueType::I1,
						left, right, pc);
				}
				const ValueId flags = Binary(block, Opcode::Cop1UpdateConditionFlag,
					ValueType::I32, state->fcr31, condition, pc);
				return flags != INVALID_VALUE && WriteFcr31(block, state, flags, pc);
			}

			bool LowerCompoundCop1Arithmetic(Block& block, StateMap* state, u32 op,
				u32 pc, CompoundCop1ArithmeticKind kind)
			{
				// PCSX2 FPU.cpp::MADD_S()/MSUB_S() intentionally perform two
				// single-precision operations. The temporary product is written as
				// raw F32 bits, normalized through fpuDouble(), and only then combined
				// with the independently normalized ACC. Keeping both stages explicit
				// prevents a later backend from contracting this into a host FMA.
				const u32 fs = FS(op);
				const u32 ft = RT(op);
				const ValueId left = Unary(block, Opcode::Cop1NormalizeInput,
					ValueType::F32Bits, state->fpr[fs], pc);
				if (left == INVALID_VALUE)
					return false;
				const ValueId right = fs == ft ? left :
					Unary(block, Opcode::Cop1NormalizeInput, ValueType::F32Bits,
						state->fpr[ft], pc);
				if (right == INVALID_VALUE)
					return false;
				const ValueId product = Binary(block, Opcode::Cop1MulRaw,
					ValueType::F32Bits, left, right, pc);
				if (product == INVALID_VALUE)
					return false;
				const ValueId normalized_product = Unary(block,
					Opcode::Cop1NormalizeInput, ValueType::F32Bits, product, pc);
				const ValueId normalized_acc = Unary(block, Opcode::Cop1NormalizeInput,
					ValueType::F32Bits, state->acc, pc);
				if (normalized_product == INVALID_VALUE ||
					normalized_acc == INVALID_VALUE)
				{
					return false;
				}
				const ValueId raw = Binary(block, CompoundCop1FinalRawOpcode(kind),
					ValueType::F32Bits, normalized_acc, normalized_product, pc);
				const ValueId result = Unary(block, Opcode::Cop1ClampOuResult,
					ValueType::F32Bits, raw, pc);
				const ValueId flags = Binary(block, Opcode::Cop1UpdateOuFlags,
					ValueType::I32, state->fcr31, raw, pc);
				if (raw == INVALID_VALUE || result == INVALID_VALUE ||
					flags == INVALID_VALUE || !WriteFcr31(block, state, flags, pc))
				{
					return false;
				}
				return WriteFpr(block, state, FD(op), result, pc);
			}

			bool LowerMemory(Block& block, StateMap* state, u32 op, u32 pc,
				MemoryAccessKind kind)
			{
				const bool fpr_access = IsFprMemoryAccess(kind);
				const bool vu0_access = IsVu0MemoryAccess(kind);
				const u32 destination = RT(op);
				ValueId architectural_value = fpr_access ? state->fpr[destination] :
					(vu0_access ? state->vu0_vf[destination] :
					              state->gpr[destination]);
				if (vu0_access)
				{
					architectural_value = Binary(block, Opcode::Vu0RequireIdle,
						ValueType::I128, state->vu0_vpu_stat, architectural_value, pc);
					if (architectural_value == INVALID_VALUE)
						return false;
				}
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
						{state->memory_effect, address, architectural_value}, 3,
						encoded_kind, 0, pc);
					if (effect == INVALID_VALUE)
						return false;
					state->memory_effect = effect;
					if (!fpr_access && destination == 0)
						return true;
					const ValueId value = Unary(block, Opcode::MemoryLoadValue,
						fpr_access ? ValueType::F32Bits : ValueType::I128,
						effect, pc);
					if (fpr_access)
						return WriteFpr(block, state, destination, value, pc);
					if (vu0_access)
						return WriteVu0Vf(block, state, destination, value, pc);
					if (value == INVALID_VALUE ||
						AddNode(block, Opcode::BindGpr, ValueType::Void,
							{value, INVALID_VALUE, INVALID_VALUE}, 1, destination, 0,
							pc) == INVALID_VALUE)
					{
						return false;
					}
					state->gpr[destination] = value;
					return true;
				}

				const ValueId effect = AddNode(block, Opcode::MemoryStore,
					ValueType::MemoryEffect,
					{state->memory_effect, address, architectural_value}, 3,
					encoded_kind, 0, pc);
				if (effect == INVALID_VALUE)
					return false;
				state->memory_effect = effect;
				return true;
			}

			bool LowerNonBranch(Block& block, StateMap* state, u32 op, u32 pc)
			{
				NoEffectKind no_effect{};
				if (DecodeNoEffect(op, m_program->options, &no_effect))
				{
					return AddNode(block, Opcode::NoEffect, ValueType::Void, {}, 0,
						static_cast<u32>(no_effect), 0, pc) != INVALID_VALUE;
				}

				MemoryAccessKind memory_kind{};
				if (DecodeMemoryAccess(op, &memory_kind))
					return LowerMemory(block, state, op, pc, memory_kind);

				const u32 primary = op >> 26;
				if (primary == 0x11)
				{
					PureCop1StateKind kind{};
					if (!DecodePureCop1State(op, &kind))
					{
						if (IsCop1ControlWrite(op))
						{
							return WriteFcr31(block, state,
								Low32(block, *state, RT(op), pc), pc);
						}
						BasicCop1ArithmeticKind arithmetic{};
						if (DecodeBasicCop1Arithmetic(op, &arithmetic))
						{
							return LowerBasicCop1Arithmetic(block, state, op, pc,
								arithmetic);
						}
						CompoundCop1ArithmeticKind compound{};
						if (DecodeCompoundCop1Arithmetic(op, &compound))
						{
							return LowerCompoundCop1Arithmetic(block, state, op, pc,
								compound);
						}
						Cop1CompareKind compare{};
						if (DecodeCop1Compare(op, &compare))
							return LowerCop1Compare(block, state, op, pc, compare);
						if (IsCop1ConvertWord(op))
						{
							return WriteFpr(block, state, FD(op),
								Unary(block, Opcode::Cop1ConvertWord, ValueType::F32Bits,
									state->fpr[FS(op)], pc), pc);
						}
						if (IsCop1ConvertSingle(op))
						{
							return WriteFpr(block, state, FD(op),
								Unary(block, Opcode::Cop1ConvertSingle,
									ValueType::F32Bits, state->fpr[FS(op)], pc), pc);
						}
						return false;
					}
					ValueId value = INVALID_VALUE;
					switch (kind)
					{
						case PureCop1StateKind::MoveFromFpr:
							value = Unary(block, Opcode::BitcastF32BitsToI32,
								ValueType::I32, state->fpr[FS(op)], pc);
							value = Unary(block, Opcode::SignExtend32To64,
								ValueType::I64, value, pc);
							return WriteLow64(block, state, RT(op), value, pc);
						case PureCop1StateKind::MoveToFpr:
							value = Low32(block, *state, RT(op), pc);
							value = Unary(block, Opcode::BitcastI32ToF32Bits,
								ValueType::F32Bits, value, pc);
							return WriteFpr(block, state, FS(op), value, pc);
						case PureCop1StateKind::MoveFpr:
							return WriteFpr(block, state, FD(op), state->fpr[FS(op)], pc);
					}
				}
				if (primary == 0x1c)
				{
					PureMmiKind kind{};
					if (!DecodePureMmi(op, &kind))
						return false;
					const u32 rd = RD(op);
					const u32 rs = RS(op);
					const u32 rt = RT(op);
					ValueId value = INVALID_VALUE;
					switch (kind)
					{
						case PureMmiKind::MoveFromHi1:
						case PureMmiKind::MoveFromLo1:
							value = Unary(block, Opcode::ExtractHigh64,
								ValueType::I64,
								kind == PureMmiKind::MoveFromHi1 ? state->hi : state->lo,
								pc);
							return WriteLow64(block, state, rd, value, pc);
						case PureMmiKind::MoveToHi1:
						case PureMmiKind::MoveToLo1:
							value = Low64(block, *state, rs, pc);
							return WriteHiLoHigh64(block, state,
								kind == PureMmiKind::MoveToHi1, value, pc);
						case PureMmiKind::MoveFromHi:
							return WriteFullGpr(block, state, rd, state->hi, pc);
						case PureMmiKind::MoveFromLo:
							return WriteFullGpr(block, state, rd, state->lo, pc);
						case PureMmiKind::MoveToHi:
							return WriteFullHiLo(block, state, true, state->gpr[rs], pc);
						case PureMmiKind::MoveToLo:
							return WriteFullHiLo(block, state, false, state->gpr[rs], pc);
						case PureMmiKind::And:
						case PureMmiKind::Xor:
						case PureMmiKind::Or:
						case PureMmiKind::Nor:
						{
							const Opcode logical =
								kind == PureMmiKind::And ? Opcode::And128 :
								kind == PureMmiKind::Xor ? Opcode::Xor128 :
								kind == PureMmiKind::Or ? Opcode::Or128 : Opcode::Nor128;
							value = Binary(block, logical, ValueType::I128,
								state->gpr[rs], state->gpr[rt], pc);
							return WriteFullGpr(block, state, rd, value, pc);
						}
						case PureMmiKind::CopyLowDoubleword:
							value = Binary(block, Opcode::PackLow64, ValueType::I128,
								state->gpr[rs], state->gpr[rt], pc);
							return WriteFullGpr(block, state, rd, value, pc);
						case PureMmiKind::CopyUpperDoubleword:
							value = Binary(block, Opcode::PackHigh64, ValueType::I128,
								state->gpr[rs], state->gpr[rt], pc);
							return WriteFullGpr(block, state, rd, value, pc);
						case PureMmiKind::CopyHalfword:
							value = Unary(block, Opcode::BroadcastLowHalfwordPer64,
								ValueType::I128, state->gpr[rt], pc);
							return WriteFullGpr(block, state, rd, value, pc);
					}
				}
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
						case 0x04: // SLLV
						case 0x06: // SRLV
						case 0x07: // SRAV
						case 0x14: // DSLLV
						case 0x16: // DSRLV
						case 0x17: // DSRAV
						{
							const bool doubleword = function >= 0x14;
							value = doubleword ? Low64(block, *state, rt, pc) :
								Low32(block, *state, rt, pc);
							const ValueId shift = Low32(block, *state, rs, pc);
							const Opcode shift_opcode =
								function == 0x04 ? Opcode::ShiftLeft32Variable :
								function == 0x06 ? Opcode::ShiftRightLogical32Variable :
								function == 0x07 ? Opcode::ShiftRightArithmetic32Variable :
								function == 0x14 ? Opcode::ShiftLeft64Variable :
								function == 0x16 ? Opcode::ShiftRightLogical64Variable :
								                   Opcode::ShiftRightArithmetic64Variable;
							value = Binary(block, shift_opcode,
								doubleword ? ValueType::I64 : ValueType::I32, value,
								shift, pc);
							if (!doubleword)
								value = Unary(block, Opcode::SignExtend32To64,
									ValueType::I64, value, pc);
							return WriteLow64(block, state, rd, value, pc);
						}
						case 0x0a: // MOVZ
						case 0x0b: // MOVN
						{
							const ValueId condition_value = Low64(block, *state, rt, pc);
							const ValueId zero = Constant64(block, 0, pc);
							const ValueId condition = Binary(block,
								function == 0x0a ? Opcode::CompareEqual64 :
								                   Opcode::CompareNotEqual64,
								ValueType::I1, condition_value, zero, pc);
							const ValueId source = Low64(block, *state, rs, pc);
							const ValueId old = Low64(block, *state, rd, pc);
							value = Ternary(block, Opcode::Select64, ValueType::I64,
								condition, source, old, pc);
							return WriteLow64(block, state, rd, value, pc);
						}
						case 0x10: // MFHI
						case 0x12: // MFLO
							value = Unary(block, Opcode::ExtractLow64, ValueType::I64,
								function == 0x10 ? state->hi : state->lo, pc);
							return WriteLow64(block, state, rd, value, pc);
						case 0x11: // MTHI
						case 0x13: // MTLO
							value = Low64(block, *state, rs, pc);
							return WriteHiLoLow64(block, state, function == 0x11,
								value, pc);
						case 0x28: // MFSA
							value = Unary(block, Opcode::ZeroExtend32To64,
								ValueType::I64, state->sa, pc);
							return WriteLow64(block, state, rd, value, pc);
						case 0x29: // MTSA
							value = Low32(block, *state, rs, pc);
							return WriteSa(block, state, value, pc);
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
					case 0x01: // MTSAB / MTSAH
						left = Low32(block, *state, rs, pc);
						right = Constant32(block, RT(op) == 0x18 ? 0x0fu : 0x07u, pc);
						value = Binary(block, Opcode::And32, ValueType::I32,
							left, right, pc);
						right = Constant32(block,
							IMM_U(op) & (RT(op) == 0x18 ? 0x0fu : 0x07u), pc);
						value = Binary(block, Opcode::Xor32, ValueType::I32,
							value, right, pc);
						if (RT(op) == 0x19)
							value = Unary(block, Opcode::ShiftLeft32,
								ValueType::I32, value, pc, 1);
						return WriteSa(block, state, value, pc);
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
				u32 source_pc, bool link_internal = true,
				bool event_horizon_check = true)
			{
				Transfer transfer{};
				transfer.state = state;
				transfer.pc = ConstantAddress(block, target_pc, source_pc);
				transfer.external_reason = reason;
				const auto found = block_indices.find(target_pc);
				if (link_internal && found != block_indices.end())
					transfer.target_block = found->second;
				transfer.event_horizon_check = event_horizon_check;
				return transfer;
			}

			Transfer MakeRegisterTransfer(
				const StateMap& state, ValueId target, ExitReason reason,
				bool event_horizon_check = true)
			{
				Transfer transfer{};
				transfer.state = state;
				transfer.pc = target;
				transfer.external_reason = reason;
				transfer.event_horizon_check = event_horizon_check;
				return transfer;
			}

			Transfer MakeDeferredObserverTransfer(Block& block,
				const StateMap& state, u32 target_pc, ExitReason reason,
				u32 source_pc, u32 pending_raw_cycles)
			{
				Transfer transfer{};
				transfer.state = state;
				transfer.pc = ConstantAddress(block, target_pc, source_pc);
				transfer.external_reason = reason;
				transfer.cycle_commit_deferred = true;
				transfer.pending_raw_cycles = pending_raw_cycles;
				transfer.event_horizon_check = false;
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
			       left.sa == right.sa && left.fpr == right.fpr &&
			       left.fcr0 == right.fcr0 && left.fcr31 == right.fcr31 &&
			       left.acc == right.acc && left.acc_flag == right.acc_flag &&
			       left.vu0_vf == right.vu0_vf &&
			       left.vu0_vpu_stat == right.vu0_vpu_stat &&
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

	u32 RawRecompilerCycles(u32 opcode, u32 cycle_factor)
	{
		// PCSX2 owner: x86/ix86-32/iR5900.cpp::recompileNextInstruction().
		// The selected recompiler timing contract gives the architectural NOP a
		// deliberately measured raw cost of nine; every other instruction uses
		// the opcode table. VitaEeBlockCompiler.cpp::CompileBlock() preserves the
		// same distinction. cycle_factor is CP0.Config.DIE-derived and is already
		// constrained to one or two by Lift().
		return (opcode == 0 ? 9u : R5900::GetInstruction(opcode).cycles) *
		       cycle_factor;
	}

	static LiftResult LiftInternal(u32 source_base_pc, const u32* source_words,
		u32 source_word_count, const SourceBlockContract* source_blocks,
		u32 source_block_count, u32 entry_pc, const LiftOptions& options)
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
		if (source_blocks && source_block_count != 0)
			result.program.source_blocks.assign(source_blocks,
				source_blocks + source_block_count);
		result.program.options = options;
		if ((source_blocks == nullptr) != (source_block_count == 0))
		{
			result.failure = LiftFailure::SourceBlockContract;
			result.failure_pc = entry_pc;
			return result;
		}
		std::string source_contract_detail;
		if (!ValidateSourceBlockContracts(source_base_pc,
				result.program.source_words, result.program.source_blocks, entry_pc,
				options, &result.failure_pc, &source_contract_detail))
		{
			result.failure = LiftFailure::SourceBlockContract;
			return result;
		}

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
				if (!ScanRawBlock(source_base_pc, result.program.source_words,
						result.program.source_blocks, leaders, pc, options, &raw,
						&result.failure, &result.failure_pc))
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
				else if (raw.control_kind == RawControlKind::StaticJump ||
						 raw.control_kind == RawControlKind::RegisterJump)
				{
					// Jumps are complete Region IR control units, but do not recursively
					// pull a call/jump target into this first bounded CFG. A static jump
					// may still link to a target already owned through conditional
					// control; a register target always returns to dispatch. Wider
					// call/return construction belongs to the profiled reducible-CFG
					// phase and must not turn a useful prefix into a whole-region
					// block/overlap failure.
				}
				else if ((leaders.contains(raw.transfer_pc) ||
							 FindSourceBlockContract(result.program.source_blocks,
								raw.transfer_pc)) &&
						 ContainsPc(source_base_pc, source_word_count,
							 raw.transfer_pc))
				{
					leaders.insert(raw.transfer_pc);
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
			const SourceBlockContract* source_contract =
				FindSourceBlockContract(result.program.source_blocks, block.pc);
			const bool event_horizon_check = !source_contract ||
				source_contract->scheduler_test_at_end;
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
			ValueId register_target = INVALID_VALUE;
			const bool has_control = raw.control_kind != RawControlKind::None;
			const bool conditional_branch =
				raw.control_kind == RawControlKind::ConditionalBranch;
			const bool register_jump =
				raw.control_kind == RawControlKind::RegisterJump;
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
				else if (register_jump)
				{
					// Interpreter.cpp::{JR,JALR} and the SCE operation definition
					// snapshot GPR[rs].low32 before either the JALR link or the delay
					// instruction can overwrite its source.
					const ValueId target =
						builder.Low32(block, state, RS(raw.branch_opcode), raw.branch_pc);
					register_target =
						builder.AddressFromI32(block, target, raw.branch_pc);
					if (target == INVALID_VALUE || register_target == INVALID_VALUE)
					{
						result.failure = LiftFailure::ValueLimit;
						result.failure_pc = raw.branch_pc;
						return result;
					}
				}
				const u32 link_register = linked_control ?
				                              ControlLinkRegister(raw.branch_opcode) :
				                              0;
				if (linked_control && link_register != 0)
				{
					const ValueId link =
						builder.Constant64(block, raw.branch_pc + 2 * sizeof(u32),
							raw.branch_pc);
					if (link == INVALID_VALUE ||
						!builder.WriteLow64(block, &state, link_register, link,
							raw.branch_pc))
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
				block.raw_cycle_cost +=
					RawRecompilerCycles(instruction.opcode, options.cycle_factor);
			block.not_taken_raw_cycle_cost = block.raw_cycle_cost;
			if (likely_branch)
			{
				const u32 delay_cycles = RawRecompilerCycles(raw.delay.opcode,
					options.cycle_factor);
				block.not_taken_raw_cycle_cost -= delay_cycles;
			}
			if (!block.source.empty())
			{
				block.scaled_cycle_cost =
					ScaleBlockCycles(block.raw_cycle_cost, options.ee_cycle_rate);
				block.not_taken_scaled_cycle_cost = ScaleBlockCycles(
					block.not_taken_raw_cycle_cost, options.ee_cycle_rate);
			}
			const bool deferred_observer =
				raw.control_kind == RawControlKind::None &&
				raw.transfer_reason != ExitReason::RegionBoundary;
			const u32 primary_cycle_pc =
				block.source.empty() ? block.pc : block.source.back().pc;
			if ((!deferred_observer &&
					!builder.AdvanceCycles(block, &state, block.scaled_cycle_cost,
						primary_cycle_pc)) ||
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
					raw.delay.pc, true, event_horizon_check);
				block.terminator.not_taken = builder.MakeTransfer(
					block, not_taken_state, not_taken_pc, ExitReason::RegionBoundary,
					block_indices, likely_branch ? raw.branch_pc : raw.delay.pc, true,
					event_horizon_check);
			}
			else if (raw.control_kind == RawControlKind::StaticJump)
			{
				block.terminator.kind = TerminatorKind::Jump;
				block.terminator.branch_pc = raw.branch_pc;
				block.terminator.delay_slot_pc = raw.delay.pc;
				block.terminator.taken = builder.MakeTransfer(
					block, state, JumpTarget(raw.branch_pc, raw.branch_opcode),
					ExitReason::RegionBoundary, block_indices, raw.delay.pc, true,
					event_horizon_check);
			}
			else if (raw.control_kind == RawControlKind::RegisterJump)
			{
				block.terminator.kind = TerminatorKind::RegisterJump;
				block.terminator.branch_pc = raw.branch_pc;
				block.terminator.delay_slot_pc = raw.delay.pc;
				block.terminator.taken = builder.MakeRegisterTransfer(
					state, register_target, ExitReason::RegionBoundary,
					event_horizon_check);
			}
			else
			{
				block.terminator.kind = TerminatorKind::Transfer;
				if (deferred_observer)
				{
					block.terminator.taken = builder.MakeDeferredObserverTransfer(
						block, state, raw.transfer_pc, raw.transfer_reason,
						block.source.empty() ? block.pc : block.source.back().pc,
						block.raw_cycle_cost);
				}
				else
				{
					block.terminator.taken = builder.MakeTransfer(
						block, state, raw.transfer_pc, raw.transfer_reason, block_indices,
						block.source.empty() ? block.pc : block.source.back().pc,
						true, event_horizon_check);
				}
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

	LiftResult Lift(u32 source_base_pc, const u32* source_words,
		u32 source_word_count, u32 entry_pc, const LiftOptions& options)
	{
		// Semantic/adversarial and source-coverage fixtures may remain unattested.
		// This overload cannot authorize future product execution.
		return LiftInternal(source_base_pc, source_words, source_word_count,
			nullptr, 0, entry_pc, options);
	}

	LiftResult LiftWithSourceBlocks(u32 source_base_pc, const u32* source_words,
		u32 source_word_count, const SourceBlockContract* source_blocks,
		u32 source_block_count, u32 entry_pc, const LiftOptions& options)
	{
		if (!source_blocks || source_block_count == 0)
		{
			LiftResult result{};
			result.failure = LiftFailure::SourceBlockContract;
			result.failure_pc = entry_pc;
			return result;
		}
		return LiftInternal(source_base_pc, source_words, source_word_count,
			source_blocks, source_block_count, entry_pc, options);
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
		std::string source_contract_detail;
		if (!ValidateSourceBlockContracts(program.source_base_pc,
				program.source_words, program.source_blocks,
				program.blocks[program.entry_block].pc, program.options,
				nullptr, &source_contract_detail))
		{
			return Fail(VerifyFailure::SourceBlockContract, program.entry_block,
				UINT32_MAX, std::move(source_contract_detail));
		}

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
			const SourceBlockContract* source_contract =
				FindSourceBlockContract(program.source_blocks, block.pc);
			if (!program.source_blocks.empty() && !source_contract)
			{
				return Fail(VerifyFailure::SourceBlockContract, block_index,
					UINT32_MAX, "IR block does not begin at an attested source block");
			}
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
			const SourceBlockContract* source_contract =
				FindSourceBlockContract(program.source_blocks, block.pc);
			if (!ContainsPc(program.source_base_pc,
					static_cast<u32>(program.source_words.size()), block.pc))
			{
				return Fail(VerifyFailure::SourceMismatch, block_index, UINT32_MAX,
					"block entry is outside the immutable source image");
			}
			if (block.terminator.kind != TerminatorKind::Transfer &&
				block.terminator.kind != TerminatorKind::Branch &&
				block.terminator.kind != TerminatorKind::Jump &&
				block.terminator.kind != TerminatorKind::RegisterJump)
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
				const bool fpr_parameter = slot >= FPR_PARAMETER_BASE &&
				                           slot < FPR_PARAMETER_BASE + FPR_COUNT;
				const bool vu0_vf_parameter = slot >= VU0_VF_PARAMETER_BASE &&
				                              slot < VU0_VF_PARAMETER_BASE + VU0_VF_COUNT;
				const bool i32_parameter = slot == SA_PARAMETER ||
				                           slot == FCR0_PARAMETER ||
				                           slot == FCR31_PARAMETER ||
				                           slot == ACC_FLAG_PARAMETER ||
				                           slot == VU0_VPU_STAT_PARAMETER;
				const ValueType expected_type = i32_parameter ? ValueType::I32 :
					fpr_parameter ? ValueType::F32Bits :
					slot == ACC_PARAMETER ? ValueType::F32Bits :
					slot == CYCLE_PARAMETER ? ValueType::Cycle :
					slot == MEMORY_EFFECT_PARAMETER ? ValueType::MemoryEffect :
					                                    ValueType::I128;
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
				else if (slot == SA_PARAMETER)
					expected.sa = parameter.id;
				else if (fpr_parameter)
					expected.fpr[slot - FPR_PARAMETER_BASE] = parameter.id;
				else if (slot == FCR0_PARAMETER)
					expected.fcr0 = parameter.id;
				else if (slot == FCR31_PARAMETER)
					expected.fcr31 = parameter.id;
				else if (slot == ACC_PARAMETER)
					expected.acc = parameter.id;
				else if (slot == ACC_FLAG_PARAMETER)
					expected.acc_flag = parameter.id;
				else if (vu0_vf_parameter)
					expected.vu0_vf[slot - VU0_VF_PARAMETER_BASE] = parameter.id;
				else if (slot == VU0_VPU_STAT_PARAMETER)
					expected.vu0_vpu_stat = parameter.id;
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
				block.terminator.kind == TerminatorKind::Jump ||
				block.terminator.kind == TerminatorKind::RegisterJump;
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
							(block.terminator.kind == TerminatorKind::Jump ?
								CanLowerStaticJump(source.opcode, program.options) :
								CanLowerRegisterJump(source.opcode, program.options));
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
					if (!CanLowerNonBranch(source.opcode, program.options))
						return Fail(
							VerifyFailure::SourceMismatch, block_index, i,
							"source instruction is outside the represented semantic surface");
				}
				raw_cycles += RawRecompilerCycles(source.opcode,
					program.options.cycle_factor);
			}
			const bool likely_branch =
				block.terminator.kind == TerminatorKind::Branch &&
				block.source.size() >= 2 &&
				IsLikelyBranch(block.source[block.source.size() - 2].opcode);
			u32 not_taken_raw_cycles = raw_cycles;
			if (likely_branch)
			{
				not_taken_raw_cycles -= RawRecompilerCycles(
					block.source.back().opcode, program.options.cycle_factor);
			}
			const bool deferred_observer =
				block.terminator.kind == TerminatorKind::Transfer &&
				block.terminator.taken.target_block == INVALID_BLOCK &&
				block.terminator.taken.external_reason != ExitReason::RegionBoundary;
			if (source_contract)
			{
				const u32 represented_end = block.source.empty() ? block.pc :
					block.source.back().pc + sizeof(u32);
				const u32 contract_end = source_contract->start_pc +
					source_contract->instruction_count * sizeof(u32);
				if (represented_end > contract_end ||
					(!deferred_observer && represented_end != contract_end))
				{
					return Fail(VerifyFailure::SourceBlockContract, block_index,
						UINT32_MAX,
						"represented source does not reach its attested timing boundary");
				}
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
			const u32 control_link_register =
				linked_control ? ControlLinkRegister(control_opcode) : 0;
			u32 link_bind_count = 0;
			std::map<u32, u32> memory_operation_count;
			std::map<u32, u32> memory_value_count;
			std::map<u32, u32> memory_bind_count;
			std::map<u32, u32> no_effect_count;
			std::map<u32, u32> extended_gpr_bind_count;
			std::map<u32, u32> pure_mmi_gpr_bind_count;
			std::map<u32, u32> pure_cop1_gpr_bind_count;
			std::map<u32, u32> fpr_bind_count;
			std::map<u32, u32> vu0_idle_guard_count;
			std::map<u32, ValueId> vu0_idle_guard_value;
			std::map<u32, u32> vu0_vf_bind_count;
			std::map<u32, u32> fcr31_bind_count;
			std::map<u32, u32> acc_bind_count;
			std::map<u32, ValueId> cop1_ou_raw_result;
			std::map<u32, u32> hi_bind_count;
			std::map<u32, u32> lo_bind_count;
			std::map<u32, u32> sa_bind_count;
			auto source_opcode_at = [&](u32 pc, u32* opcode) {
				const auto found = std::find_if(block.source.begin(), block.source.end(),
					[pc](const SourceInstruction& source) { return source.pc == pc; });
				if (found == block.source.end())
					return false;
				*opcode = found->opcode;
				return true;
			};
			auto local_node = [&](ValueId value) -> const Node* {
				if (value >= program.value_count ||
					defining_block[value] != block_index ||
					defining_node[value] >= block.nodes.size())
				{
					return nullptr;
				}
				return &block.nodes[defining_node[value]];
			};
			auto exact_unary = [&](ValueId value, Opcode opcode, ValueId operand,
				u32 source_pc) {
				const Node* node = local_node(value);
				return node && node->opcode == opcode && node->operand_count == 1 &&
				       node->operands[0] == operand && node->source_pc == source_pc;
			};
			auto exact_binary = [&](ValueId value, Opcode opcode, ValueId left,
				ValueId right, u32 source_pc) {
				const Node* node = local_node(value);
				return node && node->opcode == opcode && node->operand_count == 2 &&
				       node->operands[0] == left && node->operands[1] == right &&
				       node->source_pc == source_pc;
			};
			auto exact_constant64 = [&](ValueId value, u64 literal, u32 source_pc) {
				const Node* node = local_node(value);
				return node && node->opcode == Opcode::ConstantI64 &&
				       node->operand_count == 0 && node->literal == literal &&
				       node->source_pc == source_pc;
			};
			auto exact_constant32 = [&](ValueId value, u32 literal, u32 source_pc) {
				const Node* node = local_node(value);
				return node && node->opcode == Opcode::ConstantI32 &&
				       node->operand_count == 0 && node->literal == literal &&
				       node->source_pc == source_pc;
			};
			auto exact_constant_bool = [&](ValueId value, bool literal,
				u32 source_pc) {
				const Node* node = local_node(value);
				return node && node->opcode == Opcode::ConstantI1 &&
				       node->operand_count == 0 && node->literal == (literal ? 1u : 0u) &&
				       node->source_pc == source_pc;
			};
			auto exact_extended_gpr_bind = [&](const Node& bind, u32 source_opcode,
				const StateMap& input) {
				const u32 destination = RD(source_opcode);
				if (destination == 0 || bind.immediate != destination)
					return false;
				const Node* replace = local_node(bind.operands[0]);
				if (!replace || replace->opcode != Opcode::ReplaceLow64 ||
					replace->operand_count != 2 ||
					replace->operands[0] != input.gpr[destination] ||
					replace->source_pc != bind.source_pc)
				{
					return false;
				}

				ValueId low = replace->operands[1];
				if (IsVariableShift(source_opcode))
				{
					const u32 function = FUNCT(source_opcode);
					const bool doubleword = function >= 0x14;
					if (!doubleword)
					{
						const Node* sign_extend = local_node(low);
						if (!sign_extend ||
							sign_extend->opcode != Opcode::SignExtend32To64 ||
							sign_extend->operand_count != 1 ||
							sign_extend->source_pc != bind.source_pc)
						{
							return false;
						}
						low = sign_extend->operands[0];
					}
					const Node* shift = local_node(low);
					const Opcode expected_shift =
						function == 0x04 ? Opcode::ShiftLeft32Variable :
						function == 0x06 ? Opcode::ShiftRightLogical32Variable :
						function == 0x07 ? Opcode::ShiftRightArithmetic32Variable :
						function == 0x14 ? Opcode::ShiftLeft64Variable :
						function == 0x16 ? Opcode::ShiftRightLogical64Variable :
						                   Opcode::ShiftRightArithmetic64Variable;
					if (!shift || shift->opcode != expected_shift ||
						shift->operand_count != 2 || shift->source_pc != bind.source_pc)
					{
						return false;
					}
					return exact_unary(shift->operands[0],
						doubleword ? Opcode::ExtractLow64 : Opcode::ExtractLow32,
						input.gpr[RT(source_opcode)], bind.source_pc) &&
					       exact_unary(shift->operands[1], Opcode::ExtractLow32,
						input.gpr[RS(source_opcode)], bind.source_pc);
				}

				if (IsConditionalMove(source_opcode))
				{
					const Node* select = local_node(low);
					if (!select || select->opcode != Opcode::Select64 ||
						select->operand_count != 3 || select->source_pc != bind.source_pc)
					{
						return false;
					}
					const Node* condition = local_node(select->operands[0]);
					const Opcode expected_compare = FUNCT(source_opcode) == 0x0a ?
						Opcode::CompareEqual64 : Opcode::CompareNotEqual64;
					return condition && condition->opcode == expected_compare &&
					       condition->operand_count == 2 &&
					       condition->source_pc == bind.source_pc &&
					       exact_unary(condition->operands[0], Opcode::ExtractLow64,
						input.gpr[RT(source_opcode)], bind.source_pc) &&
					       exact_constant64(condition->operands[1], 0, bind.source_pc) &&
					       exact_unary(select->operands[1], Opcode::ExtractLow64,
						input.gpr[RS(source_opcode)], bind.source_pc) &&
					       exact_unary(select->operands[2], Opcode::ExtractLow64,
						input.gpr[destination], bind.source_pc);
				}

				if (IsMoveFromHiLo(source_opcode))
				{
					return exact_unary(low, Opcode::ExtractLow64,
						FUNCT(source_opcode) == 0x10 ? input.hi : input.lo,
						bind.source_pc);
				}
				if (IsMoveFromSa(source_opcode))
				{
					return exact_unary(low, Opcode::ZeroExtend32To64, input.sa,
						bind.source_pc);
				}
				return false;
			};
			auto exact_hilo_bind = [&](const Node& bind, u32 source_opcode,
				const StateMap& input, bool hi) {
				const ValueId destination = hi ? input.hi : input.lo;
				if (IsMoveToHiLo(source_opcode))
				{
					const Node* replace = local_node(bind.operands[0]);
					return (FUNCT(source_opcode) == 0x11) == hi && replace &&
					       replace->opcode == Opcode::ReplaceLow64 &&
					       replace->operand_count == 2 &&
					       replace->operands[0] == destination &&
					       replace->source_pc == bind.source_pc &&
					       exact_unary(replace->operands[1], Opcode::ExtractLow64,
							input.gpr[RS(source_opcode)], bind.source_pc);
				}

				PureMmiKind kind{};
				if (!DecodePureMmi(source_opcode, &kind) ||
					(hi ? !IsPureMmiHiWrite(kind) : !IsPureMmiLoWrite(kind)))
				{
					return false;
				}
				if (kind == PureMmiKind::MoveToHi ||
					kind == PureMmiKind::MoveToLo)
				{
					return bind.operands[0] == input.gpr[RS(source_opcode)];
				}
				const Node* replace = local_node(bind.operands[0]);
				return replace && replace->opcode == Opcode::ReplaceHigh64 &&
				       replace->operand_count == 2 &&
				       replace->operands[0] == destination &&
				       replace->source_pc == bind.source_pc &&
				       exact_unary(replace->operands[1], Opcode::ExtractLow64,
						input.gpr[RS(source_opcode)], bind.source_pc);
			};
			auto exact_move_to_sa = [&](const Node& bind, u32 source_opcode,
				const StateMap& input) {
				if (!IsMoveToSa(source_opcode))
					return false;
				if ((source_opcode >> 26) == 0x00)
				{
					return exact_unary(bind.operands[0], Opcode::ExtractLow32,
						input.gpr[RS(source_opcode)], bind.source_pc);
				}

				ValueId value = bind.operands[0];
				const bool halfword = RT(source_opcode) == 0x19;
				if (halfword)
				{
					const Node* shift = local_node(value);
					if (!shift || shift->opcode != Opcode::ShiftLeft32 ||
						shift->operand_count != 1 || shift->immediate != 1 ||
						shift->source_pc != bind.source_pc)
					{
						return false;
					}
					value = shift->operands[0];
				}
				const Node* xored = local_node(value);
				if (!xored || xored->opcode != Opcode::Xor32 ||
					xored->operand_count != 2 || xored->source_pc != bind.source_pc)
				{
					return false;
				}
				const Node* masked = local_node(xored->operands[0]);
				const u32 mask = halfword ? 0x07u : 0x0fu;
				return masked && masked->opcode == Opcode::And32 &&
				       masked->operand_count == 2 &&
				       masked->source_pc == bind.source_pc &&
				       exact_unary(masked->operands[0], Opcode::ExtractLow32,
						input.gpr[RS(source_opcode)], bind.source_pc) &&
				       exact_constant32(masked->operands[1], mask, bind.source_pc) &&
				       exact_constant32(xored->operands[1], IMM_U(source_opcode) & mask,
						bind.source_pc);
			};
			auto exact_pure_mmi_gpr_bind = [&](const Node& bind, u32 source_opcode,
				const StateMap& input) {
				PureMmiKind kind{};
				const u32 destination = RD(source_opcode);
				if (!DecodePureMmi(source_opcode, &kind) ||
					!IsPureMmiGprWrite(kind) || destination == 0 ||
					bind.immediate != destination)
				{
					return false;
				}

				if (kind == PureMmiKind::MoveFromHi ||
					kind == PureMmiKind::MoveFromLo)
				{
					return bind.operands[0] ==
						(kind == PureMmiKind::MoveFromHi ? input.hi : input.lo);
				}
				if (kind == PureMmiKind::MoveFromHi1 ||
					kind == PureMmiKind::MoveFromLo1)
				{
					const Node* replace = local_node(bind.operands[0]);
					return replace && replace->opcode == Opcode::ReplaceLow64 &&
					       replace->operand_count == 2 &&
					       replace->operands[0] == input.gpr[destination] &&
					       replace->source_pc == bind.source_pc &&
					       exact_unary(replace->operands[1], Opcode::ExtractHigh64,
							kind == PureMmiKind::MoveFromHi1 ? input.hi : input.lo,
							bind.source_pc);
				}

				Opcode operation{};
				switch (kind)
				{
					case PureMmiKind::And:
						operation = Opcode::And128;
						break;
					case PureMmiKind::Xor:
						operation = Opcode::Xor128;
						break;
					case PureMmiKind::Or:
						operation = Opcode::Or128;
						break;
					case PureMmiKind::Nor:
						operation = Opcode::Nor128;
						break;
					case PureMmiKind::CopyLowDoubleword:
						operation = Opcode::PackLow64;
						break;
					case PureMmiKind::CopyUpperDoubleword:
						operation = Opcode::PackHigh64;
						break;
					case PureMmiKind::CopyHalfword:
						return exact_unary(bind.operands[0],
							Opcode::BroadcastLowHalfwordPer64,
							input.gpr[RT(source_opcode)], bind.source_pc);
					default:
						return false;
				}
				return exact_binary(bind.operands[0], operation,
					input.gpr[RS(source_opcode)], input.gpr[RT(source_opcode)],
					bind.source_pc);
			};
			auto exact_pure_cop1_gpr_bind = [&](const Node& bind, u32 source_opcode,
				const StateMap& input) {
				PureCop1StateKind kind{};
				const u32 destination = RT(source_opcode);
				if (!DecodePureCop1State(source_opcode, &kind) ||
					kind != PureCop1StateKind::MoveFromFpr || destination == 0 ||
					bind.immediate != destination)
				{
					return false;
				}
				const Node* replace = local_node(bind.operands[0]);
				if (!replace || replace->opcode != Opcode::ReplaceLow64 ||
					replace->operand_count != 2 ||
					replace->operands[0] != input.gpr[destination] ||
					replace->source_pc != bind.source_pc)
				{
					return false;
				}
				const Node* sign_extend = local_node(replace->operands[1]);
				return sign_extend &&
				       sign_extend->opcode == Opcode::SignExtend32To64 &&
				       sign_extend->operand_count == 1 &&
				       sign_extend->source_pc == bind.source_pc &&
				       exact_unary(sign_extend->operands[0],
						Opcode::BitcastF32BitsToI32, input.fpr[FS(source_opcode)],
						bind.source_pc);
			};
			auto exact_pure_cop1_fpr_bind = [&](const Node& bind, u32 source_opcode,
				const StateMap& input) {
				PureCop1StateKind kind{};
				if (!DecodePureCop1State(source_opcode, &kind) ||
					kind == PureCop1StateKind::MoveFromFpr)
				{
					return false;
				}
				if (kind == PureCop1StateKind::MoveFpr)
				{
					return bind.immediate == FD(source_opcode) &&
					       bind.operands[0] == input.fpr[FS(source_opcode)];
				}
				if (bind.immediate != FS(source_opcode))
					return false;
				const Node* bitcast = local_node(bind.operands[0]);
				return bitcast && bitcast->opcode == Opcode::BitcastI32ToF32Bits &&
				       bitcast->operand_count == 1 &&
				       bitcast->source_pc == bind.source_pc &&
				       exact_unary(bitcast->operands[0], Opcode::ExtractLow32,
						input.gpr[RT(source_opcode)], bind.source_pc);
			};
			auto exact_cop1_ou_raw = [&](ValueId value, u32 source_opcode,
				u32 source_pc, const StateMap& input) {
				BasicCop1ArithmeticKind kind{};
				if (DecodeBasicCop1Arithmetic(source_opcode, &kind))
				{
					const Node* raw = local_node(value);
					if (!raw || raw->opcode != BasicCop1RawOpcode(kind) ||
						raw->operand_count != 2 || raw->source_pc != source_pc)
					{
						return false;
					}
					const u32 fs = FS(source_opcode);
					const u32 ft = RT(source_opcode);
					if (!exact_unary(raw->operands[0], Opcode::Cop1NormalizeInput,
							input.fpr[fs], raw->source_pc))
					{
						return false;
					}
					if (fs == ft)
						return raw->operands[1] == raw->operands[0];
					return exact_unary(raw->operands[1], Opcode::Cop1NormalizeInput,
						input.fpr[ft], raw->source_pc);
				}

				CompoundCop1ArithmeticKind compound{};
				if (!DecodeCompoundCop1Arithmetic(source_opcode, &compound))
					return false;
				const Node* raw = local_node(value);
				if (!raw || raw->opcode != CompoundCop1FinalRawOpcode(compound) ||
					raw->operand_count != 2 || raw->source_pc != source_pc ||
					!exact_unary(raw->operands[0], Opcode::Cop1NormalizeInput,
						input.acc, source_pc))
				{
					return false;
				}
				const Node* normalized_product = local_node(raw->operands[1]);
				if (!normalized_product ||
					normalized_product->opcode != Opcode::Cop1NormalizeInput ||
					normalized_product->operand_count != 1 ||
					normalized_product->source_pc != source_pc)
				{
					return false;
				}
				const Node* product = local_node(normalized_product->operands[0]);
				if (!product || product->opcode != Opcode::Cop1MulRaw ||
					product->operand_count != 2 || product->source_pc != source_pc)
				{
					return false;
				}
				const u32 fs = FS(source_opcode);
				const u32 ft = RT(source_opcode);
				if (!exact_unary(product->operands[0], Opcode::Cop1NormalizeInput,
						input.fpr[fs], source_pc))
				{
					return false;
				}
				if (fs == ft)
					return product->operands[1] == product->operands[0];
				return exact_unary(product->operands[1], Opcode::Cop1NormalizeInput,
					input.fpr[ft], source_pc);
			};
			auto exact_basic_cop1_result = [&](ValueId value, ValueId raw,
				u32 source_pc) {
				return exact_unary(value, Opcode::Cop1ClampOuResult, raw, source_pc);
			};
			auto exact_cop1_compare_condition = [&](ValueId value, u32 source_opcode,
				u32 source_pc, const StateMap& input) {
				Cop1CompareKind kind{};
				if (!DecodeCop1Compare(source_opcode, &kind))
					return false;
				if (kind == Cop1CompareKind::False)
					return exact_constant_bool(value, false, source_pc);

				const Node* compare = local_node(value);
				if (!compare || compare->opcode != Cop1CompareOpcode(kind) ||
					compare->operand_count != 2 || compare->source_pc != source_pc)
				{
					return false;
				}
				const u32 fs = FS(source_opcode);
				const u32 ft = RT(source_opcode);
				if (!exact_unary(compare->operands[0], Opcode::Cop1NormalizeInput,
						input.fpr[fs], source_pc))
				{
					return false;
				}
				if (fs == ft)
					return compare->operands[1] == compare->operands[0];
				return exact_unary(compare->operands[1], Opcode::Cop1NormalizeInput,
					input.fpr[ft], source_pc);
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
					case Opcode::ConstantI1:
					case Opcode::ConstantI32:
					case Opcode::ConstantI64:
					case Opcode::ConstantAddress:
					{
						const ValueType expected_type =
							node.opcode == Opcode::ConstantI1 ? ValueType::I1 :
							node.opcode == Opcode::ConstantI32 ? ValueType::I32 :
							node.opcode == Opcode::ConstantI64 ? ValueType::I64 : ValueType::Address;
						if (node.operand_count != 0 || node.type != expected_type ||
							node.immediate != 0 ||
							(node.opcode == Opcode::ConstantI1 && node.literal > 1) ||
							(node.opcode != Opcode::ConstantI1 &&
							 node.opcode != Opcode::ConstantI64 && node.literal > UINT32_MAX))
							checked = Fail(VerifyFailure::ResultType, block_index, node_index,
								"constant has the wrong arity, type, immediate, or width");
						break;
					}
					case Opcode::NoEffect:
					{
						u32 source_opcode = 0;
						NoEffectKind expected_kind{};
						if (node.operand_count != 0 || node.type != ValueType::Void ||
							node.literal != 0 ||
							!source_opcode_at(node.source_pc, &source_opcode) ||
							!DecodeNoEffect(source_opcode, program.options,
								&expected_kind) ||
							node.immediate != static_cast<u32>(expected_kind))
						{
							checked = Fail(VerifyFailure::SourceMismatch, block_index,
								node_index,
								"no-effect node does not match SYNC/PREF/CACHE source");
							break;
						}
						no_effect_count[node.source_pc]++;
						break;
					}
					case Opcode::ExtractLow32:
						checked = unary(ValueType::I128, ValueType::I32);
						break;
					case Opcode::ExtractLow64:
					case Opcode::ExtractHigh64:
						checked = unary(ValueType::I128, ValueType::I64);
						break;
					case Opcode::ReplaceLow64:
					case Opcode::ReplaceHigh64:
						checked = binary(ValueType::I128, ValueType::I64, ValueType::I128);
						break;
					case Opcode::BitcastI32ToF32Bits:
						checked = unary(ValueType::I32, ValueType::F32Bits);
						break;
					case Opcode::BitcastF32BitsToI32:
						checked = unary(ValueType::F32Bits, ValueType::I32);
						break;
					case Opcode::Cop1NormalizeInput:
					case Opcode::Cop1ClampOuResult:
					{
						checked = unary(ValueType::F32Bits, ValueType::F32Bits);
						u32 source_opcode = 0;
						if (checked &&
							(!source_opcode_at(node.source_pc, &source_opcode) ||
							 (!IsCop1OuArithmetic(source_opcode) &&
							  !DecodeCop1Compare(source_opcode, nullptr))))
						{
							checked = Fail(VerifyFailure::SourceMismatch, block_index,
								node_index,
								"COP1 normalization node has no O/U arithmetic source");
						}
						break;
					}
					case Opcode::Cop1AddRaw:
					case Opcode::Cop1SubRaw:
					case Opcode::Cop1MulRaw:
					{
						checked = binary(ValueType::F32Bits, ValueType::F32Bits,
							ValueType::F32Bits);
						u32 source_opcode = 0;
						BasicCop1ArithmeticKind basic{};
						CompoundCop1ArithmeticKind compound{};
						const bool have_source =
							source_opcode_at(node.source_pc, &source_opcode);
						const bool basic_source = have_source &&
							DecodeBasicCop1Arithmetic(source_opcode, &basic);
						const bool compound_source = have_source && !basic_source &&
							DecodeCompoundCop1Arithmetic(source_opcode, &compound);
						const bool matching_basic = basic_source &&
							node.opcode == BasicCop1RawOpcode(basic);
						const bool matching_compound = compound_source &&
							(node.opcode == Opcode::Cop1MulRaw ||
							 node.opcode == CompoundCop1FinalRawOpcode(compound));
						if (checked && !matching_basic && !matching_compound)
						{
							checked = Fail(VerifyFailure::SourceMismatch, block_index,
								node_index,
								"COP1 raw arithmetic node does not match source");
						}
						break;
					}
					case Opcode::Cop1UpdateOuFlags:
					{
						checked = binary(ValueType::I32, ValueType::F32Bits,
							ValueType::I32);
						u32 source_opcode = 0;
						if (checked &&
							(!source_opcode_at(node.source_pc, &source_opcode) ||
							 !IsCop1OuArithmetic(source_opcode)))
						{
							checked = Fail(VerifyFailure::SourceMismatch, block_index,
								node_index,
								"COP1 O/U flag node has no arithmetic source");
						}
						break;
					}
					case Opcode::Cop1CompareEqual:
					case Opcode::Cop1CompareLess:
					case Opcode::Cop1CompareLessEqual:
					{
						checked = binary(ValueType::F32Bits, ValueType::F32Bits,
							ValueType::I1);
						u32 source_opcode = 0;
						Cop1CompareKind kind{};
						if (checked &&
							(!source_opcode_at(node.source_pc, &source_opcode) ||
							 !DecodeCop1Compare(source_opcode, &kind) ||
							 kind == Cop1CompareKind::False ||
							 node.opcode != Cop1CompareOpcode(kind)))
						{
							checked = Fail(VerifyFailure::SourceMismatch, block_index,
								node_index,
								"COP1 comparison node does not match its source");
						}
						break;
					}
					case Opcode::Cop1UpdateConditionFlag:
					{
						checked = binary(ValueType::I32, ValueType::I1, ValueType::I32);
						u32 source_opcode = 0;
						if (checked &&
							(!source_opcode_at(node.source_pc, &source_opcode) ||
							 !DecodeCop1Compare(source_opcode, nullptr)))
						{
							checked = Fail(VerifyFailure::SourceMismatch, block_index,
								node_index,
								"COP1 condition update has no comparison source");
						}
						break;
					}
					case Opcode::Cop1ConvertWord:
					{
						checked = unary(ValueType::F32Bits, ValueType::F32Bits);
						u32 source_opcode = 0;
						if (checked &&
							(!source_opcode_at(node.source_pc, &source_opcode) ||
							 !IsCop1ConvertWord(source_opcode)))
						{
							checked = Fail(VerifyFailure::SourceMismatch, block_index,
								node_index,
								"COP1 word conversion has no CVT.W.S source");
						}
						break;
					}
					case Opcode::Cop1ConvertSingle:
					{
						checked = unary(ValueType::F32Bits, ValueType::F32Bits);
						u32 source_opcode = 0;
						if (checked &&
							(!source_opcode_at(node.source_pc, &source_opcode) ||
							 !IsCop1ConvertSingle(source_opcode)))
						{
							checked = Fail(VerifyFailure::SourceMismatch, block_index,
								node_index,
								"COP1 single conversion has no CVT.S.W source");
						}
						break;
					}
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
					case Opcode::And32:
					case Opcode::Xor32:
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
					case Opcode::And128:
					case Opcode::Or128:
					case Opcode::Xor128:
					case Opcode::Nor128:
					case Opcode::PackLow64:
					case Opcode::PackHigh64:
						checked = binary(ValueType::I128, ValueType::I128,
							ValueType::I128);
						break;
					case Opcode::BroadcastLowHalfwordPer64:
						checked = unary(ValueType::I128, ValueType::I128);
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
					case Opcode::ShiftLeft32Variable:
					case Opcode::ShiftRightLogical32Variable:
					case Opcode::ShiftRightArithmetic32Variable:
						checked = binary(ValueType::I32, ValueType::I32,
							ValueType::I32);
						break;
					case Opcode::ShiftLeft64Variable:
					case Opcode::ShiftRightLogical64Variable:
					case Opcode::ShiftRightArithmetic64Variable:
						checked = binary(ValueType::I64, ValueType::I32,
							ValueType::I64);
						break;
					case Opcode::Select64:
						if (node.operand_count != 3 || node.type != ValueType::I64)
						{
							checked = Fail(VerifyFailure::ResultType, block_index,
								node_index,
								"I64 select has the wrong arity or result type");
							break;
						}
						checked = require_operand(node, node_index, 0, ValueType::I1);
						if (checked)
							checked = require_operand(node, node_index, 1, ValueType::I64);
						if (checked)
							checked = require_operand(node, node_index, 2, ValueType::I64);
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
					case Opcode::AddressFromI32:
						checked = unary(ValueType::I32, ValueType::Address);
						break;
					case Opcode::EffectiveAddress32:
						checked = binary(ValueType::I32, ValueType::I32,
							ValueType::Address);
						break;
					case Opcode::Vu0RequireIdle:
					{
						checked = binary(ValueType::I32, ValueType::I128,
							ValueType::I128);
						if (!checked)
							break;
						u32 source_opcode = 0;
						MemoryAccessKind kind{};
						if (!source_opcode_at(node.source_pc, &source_opcode) ||
							!DecodeMemoryAccess(source_opcode, &kind) ||
							!IsVu0MemoryAccess(kind) || node.immediate != 0 ||
							node.literal != 0 ||
							node.operands[0] != expected.vu0_vpu_stat ||
							node.operands[1] != expected.vu0_vf[RT(source_opcode)] ||
							!vu0_idle_guard_value.emplace(node.source_pc, node.id).second)
						{
							checked = Fail(VerifyFailure::SourceMismatch, block_index,
								node_index,
								"VU0 idle guard does not consume the decoded live state exactly once");
							break;
						}
						vu0_idle_guard_count[node.source_pc]++;
						break;
					}
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
						if (!checked)
							break;

						u32 source_opcode = 0;
						MemoryAccessKind expected_kind{};
						if (!source_opcode_at(node.source_pc, &source_opcode) ||
							!DecodeMemoryAccess(source_opcode, &expected_kind) ||
							load != IsMemoryLoad(expected_kind) ||
							node.immediate != static_cast<u32>(expected_kind))
						{
							checked = Fail(VerifyFailure::SourceMismatch, block_index,
								node_index,
								"memory kind does not match source");
							break;
						}
						const bool fpr_access = IsFprMemoryAccess(expected_kind);
						const bool vu0_access = IsVu0MemoryAccess(expected_kind);
						checked = require_operand(node, node_index, 2,
							fpr_access ? ValueType::F32Bits : ValueType::I128);
						if (!checked)
							break;
						ValueId expected_value = fpr_access ?
							expected.fpr[RT(source_opcode)] :
							(vu0_access ? expected.vu0_vf[RT(source_opcode)] :
							              expected.gpr[RT(source_opcode)]);
						if (vu0_access)
						{
							const auto guard = vu0_idle_guard_value.find(node.source_pc);
							if (guard == vu0_idle_guard_value.end())
							{
								checked = Fail(VerifyFailure::SourceMismatch, block_index,
									node_index,
									"VU0 vector memory effect precedes its idle observer");
								break;
							}
							expected_value = guard->second;
						}
						if (node.operands[0] != expected.memory_effect ||
							node.operands[2] != expected_value)
						{
							checked = Fail(VerifyFailure::SourceMismatch, block_index,
								node_index,
								"memory effect chain or decoded register does not match source");
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
						if (node.operand_count != 1 ||
							node.operands[0] >= program.value_count)
						{
							checked = Fail(VerifyFailure::ResultType, block_index,
								node_index, "memory load value has the wrong arity");
							break;
						}
						checked = require_operand(node, node_index, 0,
							ValueType::MemoryEffect);
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
							!IsMemoryLoad(kind) ||
							(!IsFprMemoryAccess(kind) && RT(source_opcode) == 0) ||
							node.type != (IsFprMemoryAccess(kind) ?
								ValueType::F32Bits : ValueType::I128))
						{
							checked = Fail(VerifyFailure::SourceMismatch, block_index,
								node_index,
								"memory load value does not match its decoded register file");
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
							u32 source_opcode = 0;
							NoEffectKind no_effect_kind{};
							if (!source_opcode_at(node.source_pc, &source_opcode))
							{
								checked = Fail(VerifyFailure::SourceMismatch,
									block_index, node_index,
									"GPR binding has no owning source instruction");
								break;
							}
							PureMmiKind pure_mmi_kind{};
							const bool pure_mmi =
								DecodePureMmi(source_opcode, &pure_mmi_kind);
							PureCop1StateKind pure_cop1_kind{};
							const bool pure_cop1 =
								DecodePureCop1State(source_opcode, &pure_cop1_kind);
							if (IsExtendedScalarGprWrite(source_opcode))
							{
								if (!exact_extended_gpr_bind(node, source_opcode,
										expected))
								{
									checked = Fail(VerifyFailure::SourceMismatch,
										block_index, node_index,
										"extended scalar GPR binding does not match source");
									break;
								}
								extended_gpr_bind_count[node.source_pc]++;
							}
							else if (pure_mmi && IsPureMmiGprWrite(pure_mmi_kind))
							{
								if (!exact_pure_mmi_gpr_bind(node, source_opcode,
										expected))
								{
									checked = Fail(VerifyFailure::SourceMismatch,
										block_index, node_index,
										"pure MMI GPR binding does not match source");
									break;
								}
								pure_mmi_gpr_bind_count[node.source_pc]++;
							}
							else if (pure_cop1 &&
								pure_cop1_kind == PureCop1StateKind::MoveFromFpr)
							{
								if (!exact_pure_cop1_gpr_bind(node, source_opcode,
										expected))
								{
									checked = Fail(VerifyFailure::SourceMismatch,
										block_index, node_index,
										"COP1-to-GPR binding does not match source");
									break;
								}
								pure_cop1_gpr_bind_count[node.source_pc]++;
							}
							else if (IsMoveToHiLo(source_opcode) ||
								IsMoveToSa(source_opcode) ||
								pure_mmi ||
								pure_cop1 ||
								DecodeNoEffect(source_opcode, program.options,
									&no_effect_kind))
							{
								checked = Fail(VerifyFailure::SourceMismatch,
									block_index, node_index,
									"source instruction cannot bind a GPR");
								break;
							}
							const Node& value =
								block.nodes[defining_node[node.operands[0]]];
							if (has_delayed_control &&
								node.source_pc == block.terminator.branch_pc)
							{
								if (!linked_control || control_link_register == 0 ||
									node.immediate != control_link_register ||
									value.opcode != Opcode::ReplaceLow64 ||
									value.source_pc != block.terminator.branch_pc ||
									value.operand_count != 2 ||
									value.operands[0] !=
										expected.gpr[control_link_register] ||
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
						{
							u32 source_opcode = 0;
							if (!source_opcode_at(node.source_pc, &source_opcode) ||
								!exact_hilo_bind(node, source_opcode, expected, true))
							{
								checked = Fail(VerifyFailure::SourceMismatch,
									block_index, node_index,
									"HI binding does not match decoded move");
								break;
							}
							hi_bind_count[node.source_pc]++;
							expected.hi = node.operands[0];
						}
						break;
					case Opcode::BindLo:
						checked = unary(ValueType::I128, ValueType::Void);
						if (checked)
						{
							u32 source_opcode = 0;
							if (!source_opcode_at(node.source_pc, &source_opcode) ||
								!exact_hilo_bind(node, source_opcode, expected, false))
							{
								checked = Fail(VerifyFailure::SourceMismatch,
									block_index, node_index,
									"LO binding does not match decoded move");
								break;
							}
							lo_bind_count[node.source_pc]++;
							expected.lo = node.operands[0];
						}
						break;
					case Opcode::BindSa:
						checked = unary(ValueType::I32, ValueType::Void);
						if (checked)
						{
							u32 source_opcode = 0;
							if (!source_opcode_at(node.source_pc, &source_opcode) ||
								!exact_move_to_sa(node, source_opcode, expected))
							{
								checked = Fail(VerifyFailure::SourceMismatch,
									block_index, node_index,
									"SA binding does not match decoded MTSA/MTSAB/MTSAH");
								break;
							}
							sa_bind_count[node.source_pc]++;
							expected.sa = node.operands[0];
						}
						break;
					case Opcode::BindFpr:
						checked = unary(ValueType::F32Bits, ValueType::Void);
						if (checked && node.immediate >= FPR_COUNT)
						{
							checked = Fail(VerifyFailure::OperandOutOfRange,
								block_index, node_index,
								"FPR binding targets an invalid slot");
						}
						if (checked)
						{
							u32 source_opcode = 0;
							if (!source_opcode_at(node.source_pc, &source_opcode))
							{
								checked = Fail(VerifyFailure::SourceMismatch,
									block_index, node_index,
									"FPR binding has no owning source instruction");
								break;
							}
							MemoryAccessKind memory_kind{};
							const bool memory_load =
								DecodeMemoryAccess(source_opcode, &memory_kind) &&
								memory_kind == MemoryAccessKind::LoadF32Bits;
							BasicCop1ArithmeticKind arithmetic_kind{};
							const bool basic_arithmetic =
								DecodeBasicCop1Arithmetic(source_opcode, &arithmetic_kind) &&
								!IsBasicCop1Accumulator(arithmetic_kind);
							const bool compound_arithmetic =
								DecodeCompoundCop1Arithmetic(source_opcode, nullptr);
							const bool convert_word = IsCop1ConvertWord(source_opcode);
							const bool convert_single = IsCop1ConvertSingle(source_opcode);
							if (memory_load)
							{
								const Node* value = local_node(node.operands[0]);
								if (node.immediate != RT(source_opcode) || !value ||
									value->opcode != Opcode::MemoryLoadValue ||
									value->source_pc != node.source_pc)
								{
									checked = Fail(VerifyFailure::SourceMismatch,
										block_index, node_index,
										"loaded FPR binding does not match decoded ft");
									break;
								}
								memory_bind_count[node.source_pc]++;
							}
							else if (basic_arithmetic || compound_arithmetic)
							{
								const auto raw = cop1_ou_raw_result.find(node.source_pc);
								if (node.immediate != FD(source_opcode) ||
									raw == cop1_ou_raw_result.end() ||
									!exact_basic_cop1_result(node.operands[0], raw->second,
										node.source_pc))
								{
									checked = Fail(VerifyFailure::SourceMismatch,
										block_index, node_index,
										"basic COP1 result does not bind decoded fd");
									break;
								}
							}
							else if (convert_word)
							{
								if (node.immediate != FD(source_opcode) ||
									!exact_unary(node.operands[0], Opcode::Cop1ConvertWord,
										expected.fpr[FS(source_opcode)], node.source_pc))
								{
									checked = Fail(VerifyFailure::SourceMismatch,
										block_index, node_index,
										"CVT.W.S result does not bind decoded fd");
									break;
								}
							}
							else if (convert_single)
							{
								if (node.immediate != FD(source_opcode) ||
									!exact_unary(node.operands[0], Opcode::Cop1ConvertSingle,
										expected.fpr[FS(source_opcode)], node.source_pc))
								{
									checked = Fail(VerifyFailure::SourceMismatch,
										block_index, node_index,
										"CVT.S.W result does not bind decoded fd");
									break;
								}
							}
							else if (!exact_pure_cop1_fpr_bind(node, source_opcode,
								expected))
							{
								checked = Fail(VerifyFailure::SourceMismatch,
									block_index, node_index,
									"FPR binding does not match raw COP1 source");
								break;
							}
							fpr_bind_count[node.source_pc]++;
							expected.fpr[node.immediate] = node.operands[0];
						}
						break;
					case Opcode::BindVu0Vf:
						checked = unary(ValueType::I128, ValueType::Void);
						if (checked &&
							(node.immediate == 0 || node.immediate >= VU0_VF_COUNT))
						{
							checked = Fail(VerifyFailure::OperandOutOfRange,
								block_index, node_index,
								"VU0 VF binding targets immutable VF0 or an invalid slot");
						}
						if (checked)
						{
							u32 source_opcode = 0;
							MemoryAccessKind kind{};
							const Node* value = local_node(node.operands[0]);
							if (!source_opcode_at(node.source_pc, &source_opcode) ||
								!DecodeMemoryAccess(source_opcode, &kind) ||
								kind != MemoryAccessKind::LoadVu0Vector ||
								node.immediate != RT(source_opcode) || !value ||
								value->opcode != Opcode::MemoryLoadValue ||
								value->source_pc != node.source_pc)
							{
								checked = Fail(VerifyFailure::SourceMismatch,
									block_index, node_index,
									"loaded VU0 VF binding does not match decoded ft");
								break;
							}
							memory_bind_count[node.source_pc]++;
							vu0_vf_bind_count[node.source_pc]++;
							expected.vu0_vf[node.immediate] = node.operands[0];
						}
						break;
					case Opcode::BindFcr31:
						checked = unary(ValueType::I32, ValueType::Void);
						if (checked)
						{
							u32 source_opcode = 0;
							const Node* value = local_node(node.operands[0]);
							if (node.immediate != 31 || !value ||
								!source_opcode_at(node.source_pc, &source_opcode))
							{
								checked = Fail(VerifyFailure::SourceMismatch,
									block_index, node_index,
									"FCR31 binding has no decoded COP1 owner");
								break;
							}
							if (IsCop1ControlWrite(source_opcode))
							{
								if (value->opcode != Opcode::ExtractLow32 ||
									value->operand_count != 1 ||
									value->source_pc != node.source_pc ||
									value->operands[0] != expected.gpr[RT(source_opcode)])
								{
									checked = Fail(VerifyFailure::SourceMismatch,
										block_index, node_index,
										"FCR31 binding does not match decoded CTC1 source");
									break;
								}
							}
							else if (IsCop1OuArithmetic(source_opcode))
							{
								if (value->opcode != Opcode::Cop1UpdateOuFlags ||
									value->operand_count != 2 ||
									value->source_pc != node.source_pc ||
									value->operands[0] != expected.fcr31 ||
									!exact_cop1_ou_raw(value->operands[1],
										source_opcode, node.source_pc, expected) ||
									!cop1_ou_raw_result.emplace(node.source_pc,
										value->operands[1]).second)
								{
									checked = Fail(VerifyFailure::SourceMismatch,
										block_index, node_index,
										"FCR31 O/U binding does not share exact COP1 raw result");
									break;
								}
							}
							else if (DecodeCop1Compare(source_opcode, nullptr))
							{
								if (value->opcode != Opcode::Cop1UpdateConditionFlag ||
									value->operand_count != 2 ||
									value->source_pc != node.source_pc ||
									value->operands[0] != expected.fcr31 ||
									!exact_cop1_compare_condition(value->operands[1],
										source_opcode, node.source_pc, expected))
								{
									checked = Fail(VerifyFailure::SourceMismatch,
										block_index, node_index,
										"FCR31.C binding does not match exact COP1 comparison");
									break;
								}
							}
							else
							{
								checked = Fail(VerifyFailure::SourceMismatch,
									block_index, node_index,
									"FCR31 binding has an unsupported COP1 owner");
								break;
							}
							fcr31_bind_count[node.source_pc]++;
							expected.fcr31 = node.operands[0];
						}
						break;
					case Opcode::BindAcc:
						checked = unary(ValueType::F32Bits, ValueType::Void);
						if (checked)
						{
							u32 source_opcode = 0;
							BasicCop1ArithmeticKind kind{};
							const auto raw = cop1_ou_raw_result.find(node.source_pc);
							if (node.immediate != 0 ||
								!source_opcode_at(node.source_pc, &source_opcode) ||
								!DecodeBasicCop1Arithmetic(source_opcode, &kind) ||
								!IsBasicCop1Accumulator(kind) ||
								raw == cop1_ou_raw_result.end() ||
								!exact_basic_cop1_result(node.operands[0], raw->second,
									node.source_pc))
							{
								checked = Fail(VerifyFailure::SourceMismatch,
									block_index, node_index,
									"basic COP1 accumulator result has the wrong owner");
								break;
							}
							acc_bind_count[node.source_pc]++;
							expected.acc = node.operands[0];
						}
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
				NoEffectKind no_effect_kind{};
				if (DecodeNoEffect(source.opcode, program.options, &no_effect_kind))
				{
					if (no_effect_count[source.pc] != 1 ||
						extended_gpr_bind_count[source.pc] != 0 ||
						pure_mmi_gpr_bind_count[source.pc] != 0 ||
						pure_cop1_gpr_bind_count[source.pc] != 0 ||
						fpr_bind_count[source.pc] != 0 ||
						hi_bind_count[source.pc] != 0 || lo_bind_count[source.pc] != 0 ||
						sa_bind_count[source.pc] != 0)
					{
						return Fail(VerifyFailure::SourceMismatch, block_index,
							UINT32_MAX,
							"no-effect source lacks one exact witness or changes state");
					}
				}
				else if (IsExtendedScalarGprWrite(source.opcode))
				{
					const u32 expected_binds = RD(source.opcode) == 0 ? 0u : 1u;
					if (extended_gpr_bind_count[source.pc] != expected_binds ||
						pure_mmi_gpr_bind_count[source.pc] != 0 ||
						pure_cop1_gpr_bind_count[source.pc] != 0 ||
						fpr_bind_count[source.pc] != 0 ||
						hi_bind_count[source.pc] != 0 || lo_bind_count[source.pc] != 0 ||
						sa_bind_count[source.pc] != 0)
					{
						return Fail(VerifyFailure::SourceMismatch, block_index,
							UINT32_MAX,
							"extended scalar source has the wrong architectural binding");
					}
				}
				else if (IsMoveToHiLo(source.opcode))
				{
					const bool hi = FUNCT(source.opcode) == 0x11;
					if (hi_bind_count[source.pc] != (hi ? 1u : 0u) ||
						lo_bind_count[source.pc] != (hi ? 0u : 1u) ||
						extended_gpr_bind_count[source.pc] != 0 ||
						pure_mmi_gpr_bind_count[source.pc] != 0 ||
						pure_cop1_gpr_bind_count[source.pc] != 0 ||
						fpr_bind_count[source.pc] != 0 ||
						sa_bind_count[source.pc] != 0)
					{
						return Fail(VerifyFailure::SourceMismatch, block_index,
							UINT32_MAX,
							"MTHI/MTLO source has the wrong architectural binding");
					}
				}
				else if (IsMoveToSa(source.opcode))
				{
					if (sa_bind_count[source.pc] != 1 ||
						extended_gpr_bind_count[source.pc] != 0 ||
						pure_mmi_gpr_bind_count[source.pc] != 0 ||
						pure_cop1_gpr_bind_count[source.pc] != 0 ||
						fpr_bind_count[source.pc] != 0 ||
						hi_bind_count[source.pc] != 0 || lo_bind_count[source.pc] != 0)
					{
						return Fail(VerifyFailure::SourceMismatch, block_index,
							UINT32_MAX,
							"MTSA/MTSAB/MTSAH source has the wrong SA binding");
					}
				}
				else if (IsCop1ControlWrite(source.opcode))
				{
					if (fcr31_bind_count[source.pc] != 1 ||
						extended_gpr_bind_count[source.pc] != 0 ||
						pure_mmi_gpr_bind_count[source.pc] != 0 ||
						pure_cop1_gpr_bind_count[source.pc] != 0 ||
						fpr_bind_count[source.pc] != 0 ||
						hi_bind_count[source.pc] != 0 ||
						lo_bind_count[source.pc] != 0 ||
						sa_bind_count[source.pc] != 0)
					{
						return Fail(VerifyFailure::SourceMismatch, block_index,
							UINT32_MAX,
							"CTC1 source lacks one exact FCR31 binding");
					}
				}
				else if (BasicCop1ArithmeticKind arithmetic_kind{};
					DecodeBasicCop1Arithmetic(source.opcode, &arithmetic_kind))
				{
					const bool accumulator =
						IsBasicCop1Accumulator(arithmetic_kind);
					if (fcr31_bind_count[source.pc] != 1 ||
						fpr_bind_count[source.pc] != (accumulator ? 0u : 1u) ||
						acc_bind_count[source.pc] != (accumulator ? 1u : 0u) ||
						extended_gpr_bind_count[source.pc] != 0 ||
						pure_mmi_gpr_bind_count[source.pc] != 0 ||
						pure_cop1_gpr_bind_count[source.pc] != 0 ||
						hi_bind_count[source.pc] != 0 ||
						lo_bind_count[source.pc] != 0 ||
						sa_bind_count[source.pc] != 0)
					{
						return Fail(VerifyFailure::SourceMismatch, block_index,
							UINT32_MAX,
							"basic COP1 arithmetic lacks exact result and FCR31 bindings");
					}
				}
				else if (DecodeCompoundCop1Arithmetic(source.opcode, nullptr))
				{
					if (fcr31_bind_count[source.pc] != 1 ||
						fpr_bind_count[source.pc] != 1 ||
						acc_bind_count[source.pc] != 0 ||
						extended_gpr_bind_count[source.pc] != 0 ||
						pure_mmi_gpr_bind_count[source.pc] != 0 ||
						pure_cop1_gpr_bind_count[source.pc] != 0 ||
						hi_bind_count[source.pc] != 0 ||
						lo_bind_count[source.pc] != 0 ||
						sa_bind_count[source.pc] != 0)
					{
						return Fail(VerifyFailure::SourceMismatch, block_index,
							UINT32_MAX,
							"MADD.S/MSUB.S lacks exact FPR and FCR31 bindings");
					}
				}
				else if (DecodeCop1Compare(source.opcode, nullptr))
				{
					if (fcr31_bind_count[source.pc] != 1 ||
						fpr_bind_count[source.pc] != 0 ||
						acc_bind_count[source.pc] != 0 ||
						extended_gpr_bind_count[source.pc] != 0 ||
						pure_mmi_gpr_bind_count[source.pc] != 0 ||
						pure_cop1_gpr_bind_count[source.pc] != 0 ||
						hi_bind_count[source.pc] != 0 ||
						lo_bind_count[source.pc] != 0 ||
						sa_bind_count[source.pc] != 0)
					{
						return Fail(VerifyFailure::SourceMismatch, block_index,
							UINT32_MAX,
							"COP1 comparison lacks one exact FCR31.C binding");
					}
				}
				else if (IsCop1ConvertWord(source.opcode) ||
					IsCop1ConvertSingle(source.opcode))
				{
					if (fpr_bind_count[source.pc] != 1 ||
						fcr31_bind_count[source.pc] != 0 ||
						acc_bind_count[source.pc] != 0 ||
						extended_gpr_bind_count[source.pc] != 0 ||
						pure_mmi_gpr_bind_count[source.pc] != 0 ||
						pure_cop1_gpr_bind_count[source.pc] != 0 ||
						hi_bind_count[source.pc] != 0 ||
						lo_bind_count[source.pc] != 0 ||
						sa_bind_count[source.pc] != 0)
					{
						return Fail(VerifyFailure::SourceMismatch, block_index,
							UINT32_MAX,
							"COP1 conversion source lacks one exact raw-FPR binding");
					}
				}
				else
				{
					PureMmiKind pure_mmi_kind{};
					if (DecodePureMmi(source.opcode, &pure_mmi_kind))
					{
						const u32 expected_gpr =
							IsPureMmiGprWrite(pure_mmi_kind) && RD(source.opcode) != 0 ?
								1u : 0u;
						const u32 expected_hi = IsPureMmiHiWrite(pure_mmi_kind) ? 1u : 0u;
						const u32 expected_lo = IsPureMmiLoWrite(pure_mmi_kind) ? 1u : 0u;
						if (pure_mmi_gpr_bind_count[source.pc] != expected_gpr ||
							hi_bind_count[source.pc] != expected_hi ||
							lo_bind_count[source.pc] != expected_lo ||
							extended_gpr_bind_count[source.pc] != 0 ||
							pure_cop1_gpr_bind_count[source.pc] != 0 ||
							fpr_bind_count[source.pc] != 0 ||
							sa_bind_count[source.pc] != 0)
						{
							return Fail(VerifyFailure::SourceMismatch, block_index,
								UINT32_MAX,
								"pure MMI source has the wrong architectural binding");
						}
					}
					else
					{
						PureCop1StateKind pure_cop1_kind{};
						if (DecodePureCop1State(source.opcode, &pure_cop1_kind))
						{
							const u32 expected_gpr =
								pure_cop1_kind == PureCop1StateKind::MoveFromFpr &&
									RT(source.opcode) != 0 ? 1u : 0u;
							const u32 expected_fpr =
								pure_cop1_kind == PureCop1StateKind::MoveFromFpr ? 0u : 1u;
							if (pure_cop1_gpr_bind_count[source.pc] != expected_gpr ||
								fpr_bind_count[source.pc] != expected_fpr ||
								pure_mmi_gpr_bind_count[source.pc] != 0 ||
								extended_gpr_bind_count[source.pc] != 0 ||
								hi_bind_count[source.pc] != 0 ||
								lo_bind_count[source.pc] != 0 ||
								sa_bind_count[source.pc] != 0)
							{
								return Fail(VerifyFailure::SourceMismatch, block_index,
									UINT32_MAX,
									"raw COP1 source has the wrong architectural binding");
							}
						}
					}
				}

				MemoryAccessKind kind{};
				if (!DecodeMemoryAccess(source.opcode, &kind))
					continue;
				const bool fpr_access = IsFprMemoryAccess(kind);
				const bool vu0_access = IsVu0MemoryAccess(kind);
				const u32 expected_values =
					IsMemoryLoad(kind) && (fpr_access || RT(source.opcode) != 0) ? 1u : 0u;
				if (memory_operation_count[source.pc] != 1 ||
					memory_value_count[source.pc] != expected_values ||
					memory_bind_count[source.pc] != expected_values ||
					fpr_bind_count[source.pc] !=
						(fpr_access && IsMemoryLoad(kind) ? 1u : 0u) ||
					vu0_idle_guard_count[source.pc] != (vu0_access ? 1u : 0u) ||
					vu0_vf_bind_count[source.pc] !=
						(vu0_access && IsMemoryLoad(kind) && RT(source.opcode) != 0 ?
							1u : 0u))
				{
					return Fail(VerifyFailure::SourceMismatch, block_index, UINT32_MAX,
						"decoded memory instruction lacks one exact ordered effect/value/bind");
				}
			}
			if (has_delayed_control && !captured_delay_input)
				return Fail(VerifyFailure::ControlFlowMismatch, block_index, UINT32_MAX,
					"delayed control has no mechanically derived pre-delay state");
			const u32 expected_link_binds =
				linked_control && control_link_register != 0 ? 1u : 0u;
			if (link_bind_count != expected_link_binds)
				return Fail(VerifyFailure::SourceMismatch, block_index, UINT32_MAX,
					"control link binding does not match its decoded source");
			if ((primary_cycle_advance != INVALID_VALUE) !=
					(!block.source.empty() && !deferred_observer) ||
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
				const StateMap& expected_state, bool expected_deferred,
				u32 expected_pending_raw_cycles,
				bool expected_event_horizon_check) -> VerifyResult {
				if (!StateMapsEqual(transfer.state, expected_state))
					return Fail(
						VerifyFailure::StateMapMismatch, block_index, UINT32_MAX,
						"edge does not publish the mechanically derived canonical state");
				if (transfer.cycle_commit_deferred != expected_deferred ||
					transfer.pending_raw_cycles != expected_pending_raw_cycles)
				{
					return Fail(VerifyFailure::CycleMismatch, block_index, UINT32_MAX,
						"edge deferred-cycle contract does not match its source prefix");
				}
				if (transfer.event_horizon_check != expected_event_horizon_check)
				{
					return Fail(VerifyFailure::CycleMismatch, block_index, UINT32_MAX,
						"edge scheduler test does not match its attested source boundary");
				}
				if (!expected_deferred && !expected_event_horizon_check &&
					transfer.target_block == INVALID_BLOCK)
				{
					return Fail(VerifyFailure::SourceBlockContract, block_index,
						UINT32_MAX,
						"scheduler-elided source fragment leaves the owned region");
				}
				if (!type_is(transfer.pc, ValueType::Address) ||
					defining_block[transfer.pc] != block_index)
				{
					return Fail(VerifyFailure::ControlFlowMismatch, block_index, UINT32_MAX,
						"edge PC is not a local Address value");
				}
				const Node& pc_node = block.nodes[defining_node[transfer.pc]];
				if (block.terminator.kind == TerminatorKind::RegisterJump)
				{
					if (!captured_control_input ||
						transfer.target_block != INVALID_BLOCK ||
						transfer.external_reason != ExitReason::RegionBoundary ||
						pc_node.opcode != Opcode::AddressFromI32 ||
						pc_node.source_pc != block.terminator.branch_pc ||
						pc_node.operand_count != 1)
					{
						return Fail(VerifyFailure::ControlFlowMismatch, block_index,
							UINT32_MAX,
							"register jump target is not one external pre-delay Address");
					}
					const Node& target =
						block.nodes[defining_node[pc_node.operands[0]]];
					if (target.opcode != Opcode::ExtractLow32 ||
						target.source_pc != block.terminator.branch_pc ||
						target.operand_count != 1 ||
						target.operands[0] != control_input.gpr[RS(control_opcode)])
					{
						return Fail(VerifyFailure::ControlFlowMismatch, block_index,
							UINT32_MAX,
							"register jump target does not snapshot decoded rs low32");
					}
					return {};
				}
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

			const bool expected_event_horizon_check = !deferred_observer &&
				(!source_contract || source_contract->scheduler_test_at_end);
			VerifyResult transfer_check = verify_transfer(block.terminator.taken,
				primary_expected, deferred_observer,
				deferred_observer ? raw_cycles : 0,
				expected_event_horizon_check);
			if (!transfer_check)
				return transfer_check;
			if (source_contract && !source_contract->scheduler_test_at_end &&
				!deferred_observer &&
				block.terminator.kind != TerminatorKind::Transfer)
			{
				return Fail(VerifyFailure::SourceBlockContract, block_index,
					UINT32_MAX,
					"scheduler-elided source fragment is not a straight continuation");
			}
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

				transfer_check = verify_transfer(block.terminator.not_taken,
					not_taken_expected, false, 0, expected_event_horizon_check);
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
				if (block.terminator.kind == TerminatorKind::RegisterJump)
				{
					if (block.source.size() < 2 ||
						block.source[block.source.size() - 2].pc !=
							block.terminator.branch_pc ||
						block.source.back().pc != block.terminator.delay_slot_pc ||
						!CanLowerRegisterJump(control_opcode, program.options) ||
						next_pc.opcode != Opcode::AddressFromI32)
					{
						return Fail(VerifyFailure::ControlFlowMismatch, block_index,
							UINT32_MAX,
							"register jump does not match its source pair and target");
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
		current.vu0_vf[0] = Bits(0, 0x3f80000000000000ull);
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
			values[block.parameters.sa] = {ValueType::I32, Bits(state.sa)};
			for (u32 fpr = 0; fpr < FPR_COUNT; fpr++)
				values[block.parameters.fpr[fpr]] =
					{ValueType::F32Bits, Bits(state.fpr[fpr])};
			values[block.parameters.fcr0] = {ValueType::I32, Bits(state.fcr0)};
			values[block.parameters.fcr31] = {ValueType::I32, Bits(state.fcr31)};
			values[block.parameters.acc] = {ValueType::F32Bits, Bits(state.acc)};
			values[block.parameters.acc_flag] =
				{ValueType::I32, Bits(state.acc_flag)};
			for (u32 vf = 0; vf < VU0_VF_COUNT; vf++)
				values[block.parameters.vu0_vf[vf]] =
					{ValueType::I128, state.vu0_vf[vf]};
			values[block.parameters.vu0_vpu_stat] =
				{ValueType::I32, Bits(state.vu0_vpu_stat)};
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
			state.sa = static_cast<u32>(values[transfer.state.sa].bits.lo);
			for (u32 fpr = 0; fpr < FPR_COUNT; fpr++)
				state.fpr[fpr] =
					static_cast<u32>(values[transfer.state.fpr[fpr]].bits.lo);
			state.fcr0 = static_cast<u32>(values[transfer.state.fcr0].bits.lo);
			state.fcr31 = static_cast<u32>(values[transfer.state.fcr31].bits.lo);
			state.acc = static_cast<u32>(values[transfer.state.acc].bits.lo);
			state.acc_flag =
				static_cast<u32>(values[transfer.state.acc_flag].bits.lo);
			for (u32 vf = 0; vf < VU0_VF_COUNT; vf++)
				state.vu0_vf[vf] = values[transfer.state.vu0_vf[vf]].bits;
			state.vu0_vf[0] = Bits(0, 0x3f80000000000000ull);
			state.vu0_vpu_stat = static_cast<u32>(
				values[transfer.state.vu0_vpu_stat].bits.lo);
			state.cycle = values[transfer.state.cycle].bits.lo;
			state.pc = static_cast<u32>(values[transfer.pc].bits.lo);
			return state;
		};

		u32 block_index = program.entry_block;
		assign_parameters(program.blocks[block_index], current,
			{ValueType::MemoryEffect, Bits(0)});
		auto exit_before_source = [&](const Block& block, const Node& node,
									 ExitReason reason) {
			u32 pending_raw_cycles = 0;
			u32 source_instructions_executed = 0;
			for (const SourceInstruction& source : block.source)
			{
				if (source.pc == node.source_pc)
					break;
				pending_raw_cycles += RawRecompilerCycles(source.opcode,
					program.options.cycle_factor);
				source_instructions_executed++;
			}
			result.source_instructions_executed +=
				source_instructions_executed;
			current.gpr[0] = {};
			current.pc = node.source_pc;
			*output = current;
			result.completed = true;
			result.reason = reason;
			result.cycle_commit_deferred = true;
			result.pending_raw_cycles = pending_raw_cycles;
		};
		auto exit_before_memory = [&](const Block& block, const Node& node,
									  u32 address, ExitReason reason) {
			exit_before_source(block, node, reason);
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
				const u64 third =
					node.operand_count > 2 ? values[node.operands[2]].bits.lo : 0;
				u128 bits{};
				switch (node.opcode)
				{
					case Opcode::Parameter:
						break;
					case Opcode::ConstantI1:
					case Opcode::ConstantI32:
					case Opcode::ConstantI64:
					case Opcode::ConstantAddress:
						bits = Bits(node.literal);
						break;
					case Opcode::NoEffect:
						break;
					case Opcode::ExtractLow32:
						bits = Bits(static_cast<u32>(left));
						break;
					case Opcode::ExtractLow64:
						bits = Bits(left);
						break;
					case Opcode::ExtractHigh64:
						bits = Bits(values[node.operands[0]].bits.hi);
						break;
					case Opcode::ReplaceLow64:
						bits = values[node.operands[0]].bits;
						bits.lo = right;
						break;
					case Opcode::ReplaceHigh64:
						bits = values[node.operands[0]].bits;
						bits.hi = right;
						break;
					case Opcode::BitcastI32ToF32Bits:
					case Opcode::BitcastF32BitsToI32:
						bits = Bits(static_cast<u32>(left));
						break;
					case Opcode::Cop1NormalizeInput:
						bits = Bits(NormalizeCop1Input(static_cast<u32>(left)));
						break;
					case Opcode::Cop1AddRaw:
					case Opcode::Cop1SubRaw:
					case Opcode::Cop1MulRaw:
						bits = Bits(EvaluateBasicCop1Raw(node.opcode,
							static_cast<u32>(left), static_cast<u32>(right)));
						break;
					case Opcode::Cop1ClampOuResult:
						bits = Bits(ClampBasicCop1Result(static_cast<u32>(left)));
						break;
					case Opcode::Cop1UpdateOuFlags:
						bits = Bits(UpdateBasicCop1OuFlags(static_cast<u32>(left),
							static_cast<u32>(right)));
						break;
					case Opcode::Cop1CompareEqual:
					case Opcode::Cop1CompareLess:
					case Opcode::Cop1CompareLessEqual:
						bits = Bits(EvaluateCop1Compare(node.opcode,
							static_cast<u32>(left), static_cast<u32>(right)) ? 1u : 0u);
						break;
					case Opcode::Cop1UpdateConditionFlag:
						bits = Bits(UpdateCop1ConditionFlag(static_cast<u32>(left),
							right != 0));
						break;
					case Opcode::Cop1ConvertWord:
						bits = Bits(ConvertCop1Word(static_cast<u32>(left)));
						break;
					case Opcode::Cop1ConvertSingle:
						bits = Bits(ConvertCop1Single(static_cast<u32>(left)));
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
					case Opcode::And32:
						bits = Bits(static_cast<u32>(left) & static_cast<u32>(right));
						break;
					case Opcode::And64:
						bits = Bits(left & right);
						break;
					case Opcode::Or64:
						bits = Bits(left | right);
						break;
					case Opcode::Xor32:
						bits = Bits(static_cast<u32>(left) ^ static_cast<u32>(right));
						break;
					case Opcode::Xor64:
						bits = Bits(left ^ right);
						break;
					case Opcode::Nor64:
						bits = Bits(~(left | right));
						break;
					case Opcode::And128:
						bits = {
							values[node.operands[0]].bits.lo &
								values[node.operands[1]].bits.lo,
							values[node.operands[0]].bits.hi &
								values[node.operands[1]].bits.hi};
						break;
					case Opcode::Or128:
						bits = {
							values[node.operands[0]].bits.lo |
								values[node.operands[1]].bits.lo,
							values[node.operands[0]].bits.hi |
								values[node.operands[1]].bits.hi};
						break;
					case Opcode::Xor128:
						bits = {
							values[node.operands[0]].bits.lo ^
								values[node.operands[1]].bits.lo,
							values[node.operands[0]].bits.hi ^
								values[node.operands[1]].bits.hi};
						break;
					case Opcode::Nor128:
						bits = {
							~(values[node.operands[0]].bits.lo |
								values[node.operands[1]].bits.lo),
							~(values[node.operands[0]].bits.hi |
								values[node.operands[1]].bits.hi)};
						break;
					case Opcode::PackLow64:
						bits = {values[node.operands[1]].bits.lo,
							values[node.operands[0]].bits.lo};
						break;
					case Opcode::PackHigh64:
						bits = {values[node.operands[0]].bits.hi,
							values[node.operands[1]].bits.hi};
						break;
					case Opcode::BroadcastLowHalfwordPer64:
					{
						constexpr u64 REPEAT_HALFWORD = 0x0001000100010001ull;
						const u128 source = values[node.operands[0]].bits;
						bits = {(source.lo & 0xffffu) * REPEAT_HALFWORD,
							(source.hi & 0xffffu) * REPEAT_HALFWORD};
						break;
					}
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
					case Opcode::ShiftLeft32Variable:
						bits = Bits(static_cast<u32>(left) << (static_cast<u32>(right) & 31u));
						break;
					case Opcode::ShiftRightLogical32Variable:
						bits = Bits(static_cast<u32>(left) >> (static_cast<u32>(right) & 31u));
						break;
					case Opcode::ShiftRightArithmetic32Variable:
						bits = Bits(static_cast<u32>(std::bit_cast<s32>(
							static_cast<u32>(left)) >> (static_cast<u32>(right) & 31u)));
						break;
					case Opcode::ShiftLeft64Variable:
						bits = Bits(left << (static_cast<u32>(right) & 63u));
						break;
					case Opcode::ShiftRightLogical64Variable:
						bits = Bits(left >> (static_cast<u32>(right) & 63u));
						break;
					case Opcode::ShiftRightArithmetic64Variable:
						bits = Bits(std::bit_cast<u64>(std::bit_cast<s64>(left) >>
							(static_cast<u32>(right) & 63u)));
						break;
					case Opcode::Select64:
						bits = Bits(left != 0 ? right : third);
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
					case Opcode::AddressFromI32:
						bits = Bits(static_cast<u32>(left));
						break;
					case Opcode::EffectiveAddress32:
						bits = Bits(static_cast<u32>(left) +
									static_cast<u32>(right));
						break;
					case Opcode::Vu0RequireIdle:
						if ((static_cast<u32>(left) & 1u) != 0)
						{
							exit_before_source(block, node,
								ExitReason::HelperObserver);
							return result;
						}
						bits = values[node.operands[1]].bits;
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
					case Opcode::BindSa:
						current.sa = static_cast<u32>(values[node.operands[0]].bits.lo);
						break;
					case Opcode::BindFpr:
						current.fpr[node.immediate] =
							static_cast<u32>(values[node.operands[0]].bits.lo);
						break;
					case Opcode::BindVu0Vf:
						current.vu0_vf[node.immediate] =
							values[node.operands[0]].bits;
						break;
					case Opcode::BindFcr31:
						current.fcr31 =
							static_cast<u32>(values[node.operands[0]].bits.lo);
						break;
					case Opcode::BindAcc:
						current.acc =
							static_cast<u32>(values[node.operands[0]].bits.lo);
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
			if (transfer->event_horizon_check && event_due(current.cycle))
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
				result.cycle_commit_deferred =
					transfer->cycle_commit_deferred;
				result.pending_raw_cycles = transfer->pending_raw_cycles;
				return result;
			}
			block_index = transfer->target_block;
			assign_parameters(program.blocks[block_index], current,
				outgoing_memory_effect);
		}
	}
} // namespace VitaEE::RegionIR
