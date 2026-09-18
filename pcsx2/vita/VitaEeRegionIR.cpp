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
#include <type_traits>
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
		constexpr u32 VU0_ACC_PARAMETER =
			VU0_VF_PARAMETER_BASE + VU0_VF_COUNT;
		constexpr u32 VU0_MACFLAG_PARAMETER = VU0_ACC_PARAMETER + 1;
		constexpr u32 VU0_STATUSFLAG_PARAMETER = VU0_MACFLAG_PARAMETER + 1;
		constexpr u32 VU0_CLIPFLAG_PARAMETER = VU0_STATUSFLAG_PARAMETER + 1;
		constexpr u32 VU0_Q_PARAMETER = VU0_CLIPFLAG_PARAMETER + 1;
		constexpr u32 VU0_VI_PARAMETER_BASE = VU0_Q_PARAMETER + 1;
		constexpr u32 VU0_VI_COUNT = 32;
		constexpr u32 VU0_MICRO_MACFLAG_PARAMETER_BASE =
			VU0_VI_PARAMETER_BASE + VU0_VI_COUNT;
		constexpr u32 VU0_MICRO_CLIPFLAG_PARAMETER_BASE =
			VU0_MICRO_MACFLAG_PARAMETER_BASE + 4;
		constexpr u32 VU0_MICRO_STATUSFLAG_PARAMETER_BASE =
			VU0_MICRO_CLIPFLAG_PARAMETER_BASE + 4;
		constexpr u32 CYCLE_PARAMETER = VU0_MICRO_STATUSFLAG_PARAMETER_BASE + 4;
		constexpr u32 MEMORY_EFFECT_PARAMETER = CYCLE_PARAMETER + 1;
		constexpr u32 PARAMETER_COUNT = MEMORY_EFFECT_PARAMETER + 1;
		enum class NoEffectKind : u32
		{
			Sync,
			Prefetch,
			Cache,
			Vu0Nop,
			Vu0WaitQ,
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
		struct IntegerMultiplyOp
		{
			bool valid = false;
			bool signed_multiply = false;
			bool accumulate = false;
			bool upper_pipeline = false;
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
		enum class Cop1UnaryWordKind : u8
		{
			Absolute,
			Negate,
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
		enum class Vu0FmacKind : u8
		{
			Add,
			Subtract,
			Multiply,
			MultiplyAdd,
			MultiplySubtract,
		};
		enum class Vu0FmacOperand : u8
		{
			Vector,
			BroadcastLane,
			ScalarQ,
		};

		struct Vu0FmacOp
		{
			bool valid = false;
			Vu0FmacKind kind = Vu0FmacKind::Multiply;
			Vu0FmacOperand operand = Vu0FmacOperand::Vector;
			bool accumulator_destination = false;
			u32 lane = 0;
		};
		enum class Vu0FdivKind : u8
		{
			Divide,
			SquareRoot,
			ReciprocalSquareRoot,
		};

		struct Vu0FdivOp
		{
			bool valid = false;
			Vu0FdivKind kind = Vu0FdivKind::Divide;
			u32 fs_lane = 0;
			u32 ft_lane = 0;
		};
		enum class Vu0UnaryKind : u8
		{
			Move,
			ConvertFixed,
			ConvertIntegerToFloat,
			Rotate32,
		};

		struct Vu0UnaryOp
		{
			bool valid = false;
			Vu0UnaryKind kind = Vu0UnaryKind::Move;
			u32 offset = 0;
		};
		enum class Vu0VectorTransferKind : u8
		{
			FromVu0,
			ToVu0,
		};

		struct Vu0VectorTransferOp
		{
			bool valid = false;
			Vu0VectorTransferKind kind = Vu0VectorTransferKind::FromVu0;
		};
		struct Vu0ControlReadOp
		{
			bool valid = false;
			u32 source = 0;
		};
		struct Vu0ControlWriteOp
		{
			bool valid = false;
			u32 target = 0;
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
		constexpr u32 VU_FLOAT_SIGN = 0x80000000u;
		constexpr u32 VU_FLOAT_EXPONENT = 0x7f800000u;
		constexpr u32 VU_FLOAT_MAX_FINITE = 0x7f7fffffu;
		constexpr u32 VU0_STATUS_FLAG = 16;
		constexpr u32 VU0_MAC_FLAG = 17;
		constexpr u32 VU0_CLIP_FLAG = 18;
		constexpr u32 VU0_R = 20;
		constexpr u32 VU0_TPC = 26;
		constexpr u32 VU0_VPU_STAT = 29;

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

		bool DecodeCop1Branch(u32 op, bool* branch_on_true = nullptr,
			bool* likely = nullptr)
		{
			if ((op >> 26) != 0x11 || RS(op) != 0x08 || RT(op) > 0x03)
				return false;
			if (branch_on_true)
				*branch_on_true = (RT(op) & 0x01u) != 0;
			if (likely)
				*likely = (RT(op) & 0x02u) != 0;
			return true;
		}

		bool IsConditionalBranch(u32 op)
		{
			if (DecodeCop1Branch(op))
				return true;
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
				case 0x18: // MULT
				case 0x19: // MULTU
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

		IntegerMultiplyOp DecodeIntegerMultiply(u32 op)
		{
			IntegerMultiplyOp decoded{};
			const u32 primary = op >> 26;
			const u32 function = FUNCT(op);
			if (primary == 0x00 && (function == 0x18 || function == 0x19))
			{
				decoded.valid = true;
				decoded.signed_multiply = function == 0x18;
				return decoded;
			}
			if (primary != 0x1c)
				return decoded;

			switch (function)
			{
				case 0x00: // MADD
				case 0x01: // MADDU
					decoded.accumulate = true;
					break;
				case 0x18: // MULT1
				case 0x19: // MULTU1
					decoded.upper_pipeline = true;
					break;
				case 0x20: // MADD1
				case 0x21: // MADDU1
					decoded.accumulate = true;
					decoded.upper_pipeline = true;
					break;
				default:
					return decoded;
			}
			decoded.valid = true;
			decoded.signed_multiply = (function & 1u) == 0;
			return decoded;
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
			else if ((op >> 26) == 0x12 && (RS(op) & 0x10u) != 0 &&
				(op & 0x3cu) == 0x3cu)
			{
				// PCSX2 owners: VU0.cpp::COP2_SPECIAL() and
				// x86/microVU_Macro.inl::{recVNOP,recVWAITQ}. Their SPECIAL2
				// bodies are empty, while the outer macro dispatcher still owns
				// the VU0 interlock. LowerNonBranch installs that observer.
				const u32 special2 = (op & 0x3u) | ((op >> 4) & 0x7cu);
				if (special2 == 0x2f)
					decoded = NoEffectKind::Vu0Nop;
				else if (special2 == 0x3b)
					decoded = NoEffectKind::Vu0WaitQ;
				else
					return false;
			}
			else
				return false;
			if (kind)
				*kind = decoded;
			return true;
		}

		bool NoEffectRequiresVu0Idle(NoEffectKind kind)
		{
			return kind == NoEffectKind::Vu0Nop ||
			       kind == NoEffectKind::Vu0WaitQ;
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

		bool DecodePackedBinaryMmi(u32 op, PackedBinaryKind* kind)
		{
			if ((op >> 26) != 0x1c)
				return false;

			PackedBinaryKind decoded{};
			const u32 sub = SA(op);
			switch (FUNCT(op))
			{
				case 0x08: // SCE EE MMI0 instruction class.
					switch (sub)
					{
						case 0x00: decoded = PackedBinaryKind::AddWrap32; break;
						case 0x01: decoded = PackedBinaryKind::SubtractWrap32; break;
						case 0x02: decoded = PackedBinaryKind::CompareGreaterSigned32; break;
						case 0x03: decoded = PackedBinaryKind::MaximumSigned32; break;
						case 0x04: decoded = PackedBinaryKind::AddWrap16; break;
						case 0x05: decoded = PackedBinaryKind::SubtractWrap16; break;
						case 0x06: decoded = PackedBinaryKind::CompareGreaterSigned16; break;
						case 0x07: decoded = PackedBinaryKind::MaximumSigned16; break;
						case 0x08: decoded = PackedBinaryKind::AddWrap8; break;
						case 0x09: decoded = PackedBinaryKind::SubtractWrap8; break;
						case 0x0a: decoded = PackedBinaryKind::CompareGreaterSigned8; break;
						case 0x10: decoded = PackedBinaryKind::AddSaturateSigned32; break;
						case 0x11: decoded = PackedBinaryKind::SubtractSaturateSigned32; break;
						case 0x12: decoded = PackedBinaryKind::InterleaveLower32; break;
						case 0x14: decoded = PackedBinaryKind::AddSaturateSigned16; break;
						case 0x15: decoded = PackedBinaryKind::SubtractSaturateSigned16; break;
						case 0x18: decoded = PackedBinaryKind::AddSaturateSigned8; break;
						case 0x19: decoded = PackedBinaryKind::SubtractSaturateSigned8; break;
						default: return false;
					}
					break;
				case 0x28: // SCE EE MMI1 instruction class.
					switch (sub)
					{
						case 0x02: decoded = PackedBinaryKind::CompareEqual32; break;
						case 0x03: decoded = PackedBinaryKind::MinimumSigned32; break;
						case 0x06: decoded = PackedBinaryKind::CompareEqual16; break;
						case 0x07: decoded = PackedBinaryKind::MinimumSigned16; break;
						case 0x0a: decoded = PackedBinaryKind::CompareEqual8; break;
						case 0x10: decoded = PackedBinaryKind::AddSaturateUnsigned32; break;
						case 0x11: decoded = PackedBinaryKind::SubtractSaturateUnsigned32; break;
						case 0x12: decoded = PackedBinaryKind::InterleaveUpper32; break;
						case 0x14: decoded = PackedBinaryKind::AddSaturateUnsigned16; break;
						case 0x15: decoded = PackedBinaryKind::SubtractSaturateUnsigned16; break;
						case 0x18: decoded = PackedBinaryKind::AddSaturateUnsigned8; break;
						case 0x19: decoded = PackedBinaryKind::SubtractSaturateUnsigned8; break;
						default: return false;
					}
					break;
				default:
					return false;
			}

			if (kind)
				*kind = decoded;
			return true;
		}

		bool DecodePackedShiftMmi(u32 op, PackedShiftKind* kind)
		{
			if ((op >> 26) != 0x1c || RS(op) != 0)
				return false;

			PackedShiftKind decoded{};
			switch (FUNCT(op))
			{
				case 0x34: decoded = PackedShiftKind::LeftLogical16; break;
				case 0x36: decoded = PackedShiftKind::RightLogical16; break;
				case 0x37: decoded = PackedShiftKind::RightArithmetic16; break;
				case 0x3c: decoded = PackedShiftKind::LeftLogical32; break;
				case 0x3e: decoded = PackedShiftKind::RightLogical32; break;
				case 0x3f: decoded = PackedShiftKind::RightArithmetic32; break;
				default: return false;
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

		bool DecodeCop1UnaryWord(u32 op, Cop1UnaryWordKind* kind)
		{
			// EE Core Instruction Set Manual ABS.S/NEG.S: S format with ft
			// reserved as zero. Undefined encodings remain on tier zero.
			if ((op >> 26) != 0x11 || RS(op) != 0x10 || RT(op) != 0)
				return false;
			Cop1UnaryWordKind decoded{};
			switch (FUNCT(op))
			{
				case 0x05:
					decoded = Cop1UnaryWordKind::Absolute;
					break;
				case 0x07:
					decoded = Cop1UnaryWordKind::Negate;
					break;
				default:
					return false;
			}
			if (kind)
				*kind = decoded;
			return true;
		}

		Opcode Cop1UnaryWordOpcode(Cop1UnaryWordKind kind)
		{
			return kind == Cop1UnaryWordKind::Absolute ?
				Opcode::Cop1AbsoluteWord : Opcode::Cop1NegateWord;
		}

		bool IsCop1ConvertSingle(u32 op)
		{
			// CVT.S.W uses the W format and reserves ft as zero.
			return (op >> 26) == 0x11 && RS(op) == 0x14 && RT(op) == 0 &&
			       FUNCT(op) == 0x20;
		}

		Vu0FmacOp DecodeVu0Fmac(u32 op)
		{
			if ((op >> 26) != 0x12 || (RS(op) & 0x10u) == 0)
				return {};

			const u32 function = FUNCT(op);
			if (function < 0x3c)
			{
				if (function <= 0x03)
					return {true, Vu0FmacKind::Add,
						Vu0FmacOperand::BroadcastLane, false, function};
				if (function <= 0x07)
					return {true, Vu0FmacKind::Subtract,
						Vu0FmacOperand::BroadcastLane, false, function - 0x04};
				if (function <= 0x0b)
					return {true, Vu0FmacKind::MultiplyAdd,
						Vu0FmacOperand::BroadcastLane, false, function - 0x08};
				if (function <= 0x0f)
					return {true, Vu0FmacKind::MultiplySubtract,
						Vu0FmacOperand::BroadcastLane, false, function - 0x0c};
				if (function >= 0x18 && function <= 0x1b)
					return {true, Vu0FmacKind::Multiply,
						Vu0FmacOperand::BroadcastLane, false, function - 0x18};
				switch (function)
				{
					case 0x1c:
						return {true, Vu0FmacKind::Multiply,
							Vu0FmacOperand::ScalarQ, false, 0};
					case 0x20:
						return {true, Vu0FmacKind::Add,
							Vu0FmacOperand::ScalarQ, false, 0};
					case 0x21:
						return {true, Vu0FmacKind::MultiplyAdd,
							Vu0FmacOperand::ScalarQ, false, 0};
					case 0x24:
						return {true, Vu0FmacKind::Subtract,
							Vu0FmacOperand::ScalarQ, false, 0};
					case 0x25:
						return {true, Vu0FmacKind::MultiplySubtract,
							Vu0FmacOperand::ScalarQ, false, 0};
					case 0x28:
						return {true, Vu0FmacKind::Add,
							Vu0FmacOperand::Vector, false, 0};
					case 0x29:
						return {true, Vu0FmacKind::MultiplyAdd,
							Vu0FmacOperand::Vector, false, 0};
					case 0x2a:
						return {true, Vu0FmacKind::Multiply,
							Vu0FmacOperand::Vector, false, 0};
					case 0x2c:
						return {true, Vu0FmacKind::Subtract,
							Vu0FmacOperand::Vector, false, 0};
					case 0x2d:
						return {true, Vu0FmacKind::MultiplySubtract,
							Vu0FmacOperand::Vector, false, 0};
					default:
						return {};
				}
			}

			const u32 special2 = (op & 0x3u) | ((op >> 4) & 0x7cu);
			if (special2 <= 0x03)
			{
				return {true, Vu0FmacKind::Add,
					Vu0FmacOperand::BroadcastLane, true, special2};
			}
			if (special2 <= 0x07)
			{
				return {true, Vu0FmacKind::Subtract,
					Vu0FmacOperand::BroadcastLane, true, special2 - 0x04};
			}
			if (special2 <= 0x0b)
			{
				return {true, Vu0FmacKind::MultiplyAdd,
					Vu0FmacOperand::BroadcastLane, true, special2 - 0x08};
			}
			if (special2 <= 0x0f)
			{
				return {true, Vu0FmacKind::MultiplySubtract,
					Vu0FmacOperand::BroadcastLane, true, special2 - 0x0c};
			}
			if (special2 >= 0x18 && special2 <= 0x1b)
			{
				return {true, Vu0FmacKind::Multiply,
					Vu0FmacOperand::BroadcastLane, true, special2 - 0x18};
			}
			switch (special2)
			{
				case 0x1c:
					return {true, Vu0FmacKind::Multiply,
						Vu0FmacOperand::ScalarQ, true, 0};
				case 0x20:
					return {true, Vu0FmacKind::Add,
						Vu0FmacOperand::ScalarQ, true, 0};
				case 0x21:
					return {true, Vu0FmacKind::MultiplyAdd,
						Vu0FmacOperand::ScalarQ, true, 0};
				case 0x24:
					return {true, Vu0FmacKind::Subtract,
						Vu0FmacOperand::ScalarQ, true, 0};
				case 0x25:
					return {true, Vu0FmacKind::MultiplySubtract,
						Vu0FmacOperand::ScalarQ, true, 0};
				case 0x28:
					return {true, Vu0FmacKind::Add,
						Vu0FmacOperand::Vector, true, 0};
				case 0x29:
					return {true, Vu0FmacKind::MultiplyAdd,
						Vu0FmacOperand::Vector, true, 0};
				case 0x2a:
					return {true, Vu0FmacKind::Multiply,
						Vu0FmacOperand::Vector, true, 0};
				case 0x2c:
					return {true, Vu0FmacKind::Subtract,
						Vu0FmacOperand::Vector, true, 0};
				case 0x2d:
					return {true, Vu0FmacKind::MultiplySubtract,
						Vu0FmacOperand::Vector, true, 0};
				default:
					return {};
			}
		}

		Vu0FdivOp DecodeVu0Fdiv(u32 op)
		{
			if ((op >> 26) != 0x12 || (RS(op) & 0x10u) == 0 ||
				(op & 0x3cu) != 0x3cu)
			{
				return {};
			}
			const u32 special2 = (op & 0x3u) | ((op >> 4) & 0x7cu);
			Vu0FdivKind kind{};
			switch (special2)
			{
				case 0x38:
					kind = Vu0FdivKind::Divide;
					break;
				case 0x39:
					kind = Vu0FdivKind::SquareRoot;
					break;
				case 0x3a:
					kind = Vu0FdivKind::ReciprocalSquareRoot;
					break;
				default:
					return {};
			}
			return {true, kind, (op >> 21) & 0x3u, (op >> 23) & 0x3u};
		}

		u32 EncodeVu0FdivImmediate(const Vu0FdivOp& fdiv)
		{
			return static_cast<u32>(fdiv.kind) | (fdiv.fs_lane << 2) |
			       (fdiv.ft_lane << 4);
		}

		Vu0FdivOp DecodeVu0FdivImmediate(u32 immediate)
		{
			if ((immediate & ~0x3fu) != 0 || (immediate & 0x3u) >
				static_cast<u32>(Vu0FdivKind::ReciprocalSquareRoot))
			{
				return {};
			}
			return {true, static_cast<Vu0FdivKind>(immediate & 0x3u),
				(immediate >> 2) & 0x3u, (immediate >> 4) & 0x3u};
		}

		Vu0UnaryOp DecodeVu0Unary(u32 op)
		{
			if ((op >> 26) != 0x12 || (RS(op) & 0x10u) == 0 ||
				(op & 0x3cu) != 0x3cu)
			{
				return {};
			}

			const u32 special2 = (op & 0x3u) | ((op >> 4) & 0x7cu);
			if (special2 == 0x30)
				return {true, Vu0UnaryKind::Move, 0};
			if (special2 == 0x31)
				return {true, Vu0UnaryKind::Rotate32, 0};
			switch (special2)
			{
				case 0x10:
					return {true, Vu0UnaryKind::ConvertIntegerToFloat, 0};
				case 0x11:
					return {true, Vu0UnaryKind::ConvertIntegerToFloat, 4};
				case 0x12:
					return {true, Vu0UnaryKind::ConvertIntegerToFloat, 12};
				case 0x13:
					return {true, Vu0UnaryKind::ConvertIntegerToFloat, 15};
				case 0x14:
					return {true, Vu0UnaryKind::ConvertFixed, 0};
				case 0x15:
					return {true, Vu0UnaryKind::ConvertFixed, 4};
				case 0x16:
					return {true, Vu0UnaryKind::ConvertFixed, 12};
				case 0x17:
					return {true, Vu0UnaryKind::ConvertFixed, 15};
				default:
					return {};
			}
		}

		Vu0VectorTransferOp DecodeVu0VectorTransfer(u32 op)
		{
			if ((op >> 26) != 0x12)
				return {};
			switch (RS(op))
			{
				case 0x01: // QMFC2, PCSX2 VU0.cpp::QMFC2().
					return {true, Vu0VectorTransferKind::FromVu0};
				case 0x05: // QMTC2, PCSX2 VU0.cpp::QMTC2().
					// PCSX2 deliberately reads the raw backing GPR[0], whereas Region
					// IR owns the architectural zero value. The read is irrelevant when
					// VF0 discards the result; otherwise retain tier zero until raw GPR0
					// is made an explicit state input.
					if (RT(op) == 0 && RD(op) != 0)
						return {};
					return {true, Vu0VectorTransferKind::ToVu0};
				default:
					return {};
			}
		}

		Vu0ControlReadOp DecodeVu0ControlRead(u32 op)
		{
			if ((op >> 26) != 0x12 || RS(op) != 0x02)
				return {};

			const u32 source = RD(op);
			// PCSX2 owner: VU0.cpp::CFC2(), with the native contract mirrored by
			// VitaEeBlockCompiler.cpp::EmitCOP2ControlReadBody().  Region state
			// currently owns only these complete control words. VI0 is constant;
			// rt=0 observes only vu0Sync() and discards any control-register read.
			if (RT(op) == 0 || source == 0 || source == 16 || source == 17 ||
				source == 29)
			{
				return {true, source};
			}
			return {};
		}

		Vu0ControlWriteOp DecodeVu0ControlWrite(u32 op)
		{
			if ((op >> 26) != 0x12 || RS(op) != 0x06)
				return {};

			const u32 target = RD(op);
			// PCSX2 owner: x86/microVU_Macro.inl::recCTC2(), mirrored by
			// VitaEeBlockCompiler.cpp::EmitCOP2ControlWriteBody().  FBRST can
			// reset either VU and CMSAR1 starts VU1, so both remain exact cold
			// observers.  Every other target is either ignored, a masked write, or
			// one state-contained control-word write represented by Region IR.
			if (target == 28 || target == 31)
				return {};
			return {true, target};
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

		bool IsExceptionalBasicCop1Result(u32 raw)
		{
			return (raw & ~COP1_SIGN) == COP1_EXPONENT ||
			       ((raw & COP1_EXPONENT) == 0 && (raw & COP1_FRACTION) != 0);
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

		u32 VuVectorLane(const u128& value, u32 lane)
		{
			const u64 half = lane < 2 ? value.lo : value.hi;
			return static_cast<u32>(half >> ((lane & 1u) * 32u));
		}

		template <typename Lane>
		Lane PackedLane(const u128& value, u32 lane)
		{
			using Unsigned = std::make_unsigned_t<Lane>;
			constexpr u32 bits = sizeof(Lane) * 8;
			constexpr u32 lanes_per_half = sizeof(u64) / sizeof(Lane);
			const u64 half = lane < lanes_per_half ? value.lo : value.hi;
			const u32 shift = (lane % lanes_per_half) * bits;
			const Unsigned raw = static_cast<Unsigned>(half >> shift);
			return std::bit_cast<Lane>(raw);
		}

		template <typename Lane>
		void SetPackedLane(u128* value, u32 lane, Lane lane_value)
		{
			using Unsigned = std::make_unsigned_t<Lane>;
			constexpr u32 bits = sizeof(Lane) * 8;
			constexpr u32 lanes_per_half = sizeof(u64) / sizeof(Lane);
			const u32 shift = (lane % lanes_per_half) * bits;
			const u64 lane_mask = sizeof(Lane) == sizeof(u32) ? UINT64_C(0xffffffff) :
				((UINT64_C(1) << bits) - 1);
			u64& half = lane < lanes_per_half ? value->lo : value->hi;
			half = (half & ~(lane_mask << shift)) |
			       ((static_cast<u64>(std::bit_cast<Unsigned>(lane_value)) & lane_mask)
				   << shift);
		}

		template <typename Unsigned>
		Unsigned PackedShiftRightArithmetic(Unsigned value, u32 amount)
		{
			static_assert(std::is_unsigned_v<Unsigned>);
			constexpr u32 bits = sizeof(Unsigned) * 8;
			if (amount == 0)
				return value;
			const u64 shifted = static_cast<u64>(value) >> amount;
			if ((static_cast<u64>(value) & (UINT64_C(1) << (bits - 1))) == 0)
				return static_cast<Unsigned>(shifted);
			const u64 sign_fill = ((UINT64_C(1) << amount) - 1) << (bits - amount);
			return static_cast<Unsigned>(shifted | sign_fill);
		}

		template <typename Lane>
		u128 MapPackedBinary(const u128& left, const u128& right,
			Lane (*operation)(Lane, Lane))
		{
			u128 result{};
			for (u32 lane = 0; lane < sizeof(u128) / sizeof(Lane); lane++)
			{
				SetPackedLane(&result, lane,
					operation(PackedLane<Lane>(left, lane),
						PackedLane<Lane>(right, lane)));
			}
			return result;
		}

		template <typename Lane>
		Lane PackedAddWrap(Lane left, Lane right)
		{
			using Unsigned = std::make_unsigned_t<Lane>;
			return std::bit_cast<Lane>(static_cast<Unsigned>(
				std::bit_cast<Unsigned>(left) + std::bit_cast<Unsigned>(right)));
		}

		template <typename Lane>
		Lane PackedSubtractWrap(Lane left, Lane right)
		{
			using Unsigned = std::make_unsigned_t<Lane>;
			return std::bit_cast<Lane>(static_cast<Unsigned>(
				std::bit_cast<Unsigned>(left) - std::bit_cast<Unsigned>(right)));
		}

		template <typename Lane>
		Lane PackedCompareGreaterSigned(Lane left, Lane right)
		{
			using Unsigned = std::make_unsigned_t<Lane>;
			return std::bit_cast<Lane>(left > right ?
				std::numeric_limits<Unsigned>::max() : Unsigned{0});
		}

		template <typename Lane>
		Lane PackedCompareEqual(Lane left, Lane right)
		{
			using Unsigned = std::make_unsigned_t<Lane>;
			return std::bit_cast<Lane>(left == right ?
				std::numeric_limits<Unsigned>::max() : Unsigned{0});
		}

		template <typename Lane>
		Lane PackedMaximumSigned(Lane left, Lane right)
		{
			return left > right ? left : right;
		}

		template <typename Lane>
		Lane PackedMinimumSigned(Lane left, Lane right)
		{
			return left > right ? right : left;
		}

		template <typename Lane>
		Lane PackedAddSaturateSigned(Lane left, Lane right)
		{
			using Wide = std::conditional_t<sizeof(Lane) < sizeof(s32), s32, s64>;
			const Wide result = static_cast<Wide>(left) + static_cast<Wide>(right);
			return static_cast<Lane>(std::clamp(result,
				static_cast<Wide>(std::numeric_limits<Lane>::min()),
				static_cast<Wide>(std::numeric_limits<Lane>::max())));
		}

		template <typename Lane>
		Lane PackedSubtractSaturateSigned(Lane left, Lane right)
		{
			using Wide = std::conditional_t<sizeof(Lane) < sizeof(s32), s32, s64>;
			const Wide result = static_cast<Wide>(left) - static_cast<Wide>(right);
			return static_cast<Lane>(std::clamp(result,
				static_cast<Wide>(std::numeric_limits<Lane>::min()),
				static_cast<Wide>(std::numeric_limits<Lane>::max())));
		}

		template <typename Lane>
		Lane PackedAddSaturateUnsigned(Lane left, Lane right)
		{
			const u64 result = static_cast<u64>(left) + static_cast<u64>(right);
			return static_cast<Lane>(std::min<u64>(result,
				std::numeric_limits<Lane>::max()));
		}

		template <typename Lane>
		Lane PackedSubtractSaturateUnsigned(Lane left, Lane right)
		{
			return left > right ? static_cast<Lane>(left - right) : Lane{0};
		}

		u128 EvaluatePackedBinary(PackedBinaryKind kind, const u128& left,
			const u128& right)
		{
			switch (kind)
			{
				case PackedBinaryKind::AddWrap8: return MapPackedBinary<s8>(left, right, PackedAddWrap<s8>);
				case PackedBinaryKind::AddWrap16: return MapPackedBinary<s16>(left, right, PackedAddWrap<s16>);
				case PackedBinaryKind::AddWrap32: return MapPackedBinary<s32>(left, right, PackedAddWrap<s32>);
				case PackedBinaryKind::SubtractWrap8: return MapPackedBinary<s8>(left, right, PackedSubtractWrap<s8>);
				case PackedBinaryKind::SubtractWrap16: return MapPackedBinary<s16>(left, right, PackedSubtractWrap<s16>);
				case PackedBinaryKind::SubtractWrap32: return MapPackedBinary<s32>(left, right, PackedSubtractWrap<s32>);
				case PackedBinaryKind::CompareGreaterSigned8: return MapPackedBinary<s8>(left, right, PackedCompareGreaterSigned<s8>);
				case PackedBinaryKind::CompareGreaterSigned16: return MapPackedBinary<s16>(left, right, PackedCompareGreaterSigned<s16>);
				case PackedBinaryKind::CompareGreaterSigned32: return MapPackedBinary<s32>(left, right, PackedCompareGreaterSigned<s32>);
				case PackedBinaryKind::MaximumSigned16: return MapPackedBinary<s16>(left, right, PackedMaximumSigned<s16>);
				case PackedBinaryKind::MaximumSigned32: return MapPackedBinary<s32>(left, right, PackedMaximumSigned<s32>);
				case PackedBinaryKind::AddSaturateSigned8: return MapPackedBinary<s8>(left, right, PackedAddSaturateSigned<s8>);
				case PackedBinaryKind::AddSaturateSigned16: return MapPackedBinary<s16>(left, right, PackedAddSaturateSigned<s16>);
				case PackedBinaryKind::AddSaturateSigned32: return MapPackedBinary<s32>(left, right, PackedAddSaturateSigned<s32>);
				case PackedBinaryKind::SubtractSaturateSigned8: return MapPackedBinary<s8>(left, right, PackedSubtractSaturateSigned<s8>);
				case PackedBinaryKind::SubtractSaturateSigned16: return MapPackedBinary<s16>(left, right, PackedSubtractSaturateSigned<s16>);
				case PackedBinaryKind::SubtractSaturateSigned32: return MapPackedBinary<s32>(left, right, PackedSubtractSaturateSigned<s32>);
				case PackedBinaryKind::CompareEqual8: return MapPackedBinary<u8>(left, right, PackedCompareEqual<u8>);
				case PackedBinaryKind::CompareEqual16: return MapPackedBinary<u16>(left, right, PackedCompareEqual<u16>);
				case PackedBinaryKind::CompareEqual32: return MapPackedBinary<u32>(left, right, PackedCompareEqual<u32>);
				case PackedBinaryKind::MinimumSigned16: return MapPackedBinary<s16>(left, right, PackedMinimumSigned<s16>);
				case PackedBinaryKind::MinimumSigned32: return MapPackedBinary<s32>(left, right, PackedMinimumSigned<s32>);
				case PackedBinaryKind::AddSaturateUnsigned8: return MapPackedBinary<u8>(left, right, PackedAddSaturateUnsigned<u8>);
				case PackedBinaryKind::AddSaturateUnsigned16: return MapPackedBinary<u16>(left, right, PackedAddSaturateUnsigned<u16>);
				case PackedBinaryKind::AddSaturateUnsigned32: return MapPackedBinary<u32>(left, right, PackedAddSaturateUnsigned<u32>);
				case PackedBinaryKind::SubtractSaturateUnsigned8: return MapPackedBinary<u8>(left, right, PackedSubtractSaturateUnsigned<u8>);
				case PackedBinaryKind::SubtractSaturateUnsigned16: return MapPackedBinary<u16>(left, right, PackedSubtractSaturateUnsigned<u16>);
				case PackedBinaryKind::SubtractSaturateUnsigned32: return MapPackedBinary<u32>(left, right, PackedSubtractSaturateUnsigned<u32>);
				case PackedBinaryKind::InterleaveLower32:
				case PackedBinaryKind::InterleaveUpper32:
				{
					const u32 first = kind == PackedBinaryKind::InterleaveLower32 ? 0 : 2;
					u128 result{};
					SetPackedLane(&result, 0, PackedLane<u32>(right, first));
					SetPackedLane(&result, 1, PackedLane<u32>(left, first));
					SetPackedLane(&result, 2, PackedLane<u32>(right, first + 1));
					SetPackedLane(&result, 3, PackedLane<u32>(left, first + 1));
					return result;
				}
				case PackedBinaryKind::Count: break;
			}
			return {};
		}

		void SetVuVectorLane(u128* value, u32 lane, u32 word)
		{
			u64& half = lane < 2 ? value->lo : value->hi;
			const u32 shift = (lane & 1u) * 32u;
			half = (half & ~(UINT64_C(0xffffffff) << shift)) |
			       (static_cast<u64>(word) << shift);
		}

		u32 NormalizeVuFloat(u32 value, bool overflow_clamp)
		{
			const u32 exponent = value & VU_FLOAT_EXPONENT;
			if (exponent == 0)
				return value & VU_FLOAT_SIGN;
			if (overflow_clamp && exponent == VU_FLOAT_EXPONENT)
				return (value & VU_FLOAT_SIGN) | VU_FLOAT_MAX_FINITE;
			return value;
		}

		u128 NormalizeVuVector(const u128& value, bool overflow_clamp)
		{
			u128 result{};
			for (u32 lane = 0; lane < 4; lane++)
				SetVuVectorLane(&result, lane,
					NormalizeVuFloat(VuVectorLane(value, lane), overflow_clamp));
			return result;
		}

		u128 BroadcastVuLane(const u128& value, u32 lane)
		{
			u128 result{};
			const u32 word = VuVectorLane(value, lane);
			for (u32 output_lane = 0; output_lane < 4; output_lane++)
				SetVuVectorLane(&result, output_lane, word);
			return result;
		}

		u128 BroadcastVuScalar(u32 value, bool overflow_clamp)
		{
			u128 result{};
			const u32 normalized = NormalizeVuFloat(value, overflow_clamp);
			for (u32 lane = 0; lane < 4; lane++)
				SetVuVectorLane(&result, lane, normalized);
			return result;
		}

		u32 EvaluateVu0FdivQ(const Vu0FdivOp& fdiv, const u128& fs_vector,
			const u128& ft_vector, bool overflow_clamp)
		{
			const u32 fs_raw = VuVectorLane(fs_vector, fdiv.fs_lane);
			const u32 ft_raw = VuVectorLane(ft_vector, fdiv.ft_lane);
			const u32 fs_bits = NormalizeVuFloat(fs_raw, overflow_clamp);
			const u32 ft_bits = NormalizeVuFloat(ft_raw, overflow_clamp);
			const float fs = std::bit_cast<float>(fs_bits);
			const float ft = std::bit_cast<float>(ft_bits);
			u32 q = 0;
			switch (fdiv.kind)
			{
				case Vu0FdivKind::Divide:
					if ((ft_bits & ~VU_FLOAT_SIGN) == 0)
					{
						q = ((fs_raw ^ ft_raw) & VU_FLOAT_SIGN) |
							VU_FLOAT_MAX_FINITE;
					}
					else
					{
						q = std::bit_cast<u32>(fs / ft);
					}
					break;
				case Vu0FdivKind::SquareRoot:
					q = std::bit_cast<u32>(std::sqrt(std::fabs(ft)));
					break;
				case Vu0FdivKind::ReciprocalSquareRoot:
					if ((ft_bits & ~VU_FLOAT_SIGN) == 0)
					{
						q = (fs_raw ^ ft_raw) & VU_FLOAT_SIGN;
						if ((fs_bits & ~VU_FLOAT_SIGN) != 0)
							q |= VU_FLOAT_MAX_FINITE;
					}
					else
					{
						const float root = std::sqrt(std::fabs(ft));
						q = std::bit_cast<u32>(fs / root);
					}
					break;
			}
			return NormalizeVuFloat(q, overflow_clamp);
		}

		u32 EvaluateVu0FdivFlags(const Vu0FdivOp& fdiv,
			const u128& fs_vector, const u128& ft_vector, bool overflow_clamp)
		{
			const u32 fs = NormalizeVuFloat(
				VuVectorLane(fs_vector, fdiv.fs_lane), overflow_clamp);
			const u32 ft = NormalizeVuFloat(
				VuVectorLane(ft_vector, fdiv.ft_lane), overflow_clamp);
			const bool fs_zero = (fs & ~VU_FLOAT_SIGN) == 0;
			const bool ft_zero = (ft & ~VU_FLOAT_SIGN) == 0;
			switch (fdiv.kind)
			{
				case Vu0FdivKind::Divide:
					return !ft_zero ? 0u : (fs_zero ? 0x10u : 0x20u);
				case Vu0FdivKind::SquareRoot:
					return !ft_zero && (ft & VU_FLOAT_SIGN) != 0 ? 0x10u : 0u;
				case Vu0FdivKind::ReciprocalSquareRoot:
					return ft_zero ? (fs_zero ? 0x30u : 0x20u) :
						((ft & VU_FLOAT_SIGN) != 0 ? 0x10u : 0u);
			}
			return 0;
		}

		u128 EvaluateVuRawBinary(Opcode opcode, const u128& left,
			const u128& right)
		{
			u128 result{};
			for (u32 lane = 0; lane < 4; lane++)
			{
				const float left_value =
					std::bit_cast<float>(VuVectorLane(left, lane));
				const float right_value =
					std::bit_cast<float>(VuVectorLane(right, lane));
				float raw = 0.0f;
				switch (opcode)
				{
					case Opcode::Vu0MulRaw:
						raw = left_value * right_value;
						break;
					case Opcode::Vu0AddRaw:
						raw = left_value + right_value;
						break;
					case Opcode::Vu0SubRaw:
						raw = left_value - right_value;
						break;
					default:
						return {};
				}
				SetVuVectorLane(&result, lane, std::bit_cast<u32>(raw));
			}
			return result;
		}

		u128 ClampVuFmacResult(const u128& raw, u32 mask, bool overflow_clamp)
		{
			u128 result = raw;
			for (u32 lane = 0; lane < 4; lane++)
			{
				if ((mask & (1u << (3u - lane))) == 0)
					continue;
				const u32 value = VuVectorLane(raw, lane);
				const u32 exponent = value & VU_FLOAT_EXPONENT;
				u32 clamped = value;
				if ((value & ~VU_FLOAT_SIGN) == 0)
					clamped = value;
				else if (exponent == 0)
					clamped = value & VU_FLOAT_SIGN;
				else if (overflow_clamp && exponent == VU_FLOAT_EXPONENT)
					clamped = (value & VU_FLOAT_SIGN) | VU_FLOAT_MAX_FINITE;
				SetVuVectorLane(&result, lane, clamped);
			}
			return result;
		}

		u32 VuMacFlagsFromRaw(const u128& raw, u32 mask)
		{
			u32 flags = 0;
			for (u32 lane = 0; lane < 4; lane++)
			{
				const u32 lane_bit = 1u << (3u - lane);
				if ((mask & lane_bit) == 0)
					continue;
				const u32 value = VuVectorLane(raw, lane);
				const u32 shift = 3u - lane;
				if ((value & VU_FLOAT_SIGN) != 0)
					flags |= 0x0010u << shift;
				if ((value & ~VU_FLOAT_SIGN) == 0)
					flags |= 0x0001u << shift;
				else if ((value & VU_FLOAT_EXPONENT) == 0)
					flags |= 0x0101u << shift;
				else if ((value & VU_FLOAT_EXPONENT) == VU_FLOAT_EXPONENT)
					flags |= 0x1000u << shift;
			}
			return flags;
		}

		u32 VuStatusFlagsFromMac(u32 mac)
		{
			return ((mac & 0x000fu) != 0 ? 0x1u : 0u) |
			       ((mac & 0x00f0u) != 0 ? 0x2u : 0u) |
			       ((mac & 0x0f00u) != 0 ? 0x4u : 0u) |
			       ((mac & 0xf000u) != 0 ? 0x8u : 0u);
		}

		u128 ConvertVu0Fixed(const u128& source, u32 offset)
		{
			u128 result{};
			const float scale = std::bit_cast<float>(
				0x3f800000u + (offset << 23));
			for (u32 lane = 0; lane < 4; lane++)
			{
				float value = std::bit_cast<float>(VuVectorLane(source, lane));
				if (offset != 0)
					value *= scale;
				const u32 raw = std::bit_cast<u32>(value);
				const u32 converted = (raw & VU_FLOAT_EXPONENT) >= 0x4f000000u ?
					((raw & VU_FLOAT_SIGN) != 0 ? 0x80000000u : 0x7fffffffu) :
					static_cast<u32>(static_cast<s32>(value));
				SetVuVectorLane(&result, lane, converted);
			}
			return result;
		}

		u128 ConvertVu0IntegerToFloat(const u128& source, u32 offset)
		{
			u128 result{};
			const float scale = std::bit_cast<float>(
				0x3f800000u - (offset << 23));
			for (u32 lane = 0; lane < 4; lane++)
			{
				float value = static_cast<float>(
					static_cast<s32>(VuVectorLane(source, lane)));
				if (offset != 0)
					value *= scale;
				SetVuVectorLane(&result, lane, std::bit_cast<u32>(value));
			}
			return result;
		}

		u128 RotateVu0Words(const u128& source)
		{
			u128 result{};
			for (u32 lane = 0; lane < 4; lane++)
			{
				SetVuVectorLane(&result, lane,
					VuVectorLane(source, (lane + 1u) & 3u));
			}
			return result;
		}

		u128 MergeVuMasked(const u128& old_value, const u128& new_value,
			u32 mask)
		{
			u128 result = old_value;
			for (u32 lane = 0; lane < 4; lane++)
			{
				if ((mask & (1u << (3u - lane))) != 0)
					SetVuVectorLane(&result, lane, VuVectorLane(new_value, lane));
			}
			return result;
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

		bool IsGuardedExceptionInstruction(u32 op)
		{
			// ADDI is the first exception-capable instruction represented inside a
			// region. Its wrapping value and signed-overflow predicate are both IR;
			// overflow transfers to the existing PCSX2 provider before ADDI (or,
			// when it is a delay instruction, before the whole branch/delay pair).
			return (op >> 26) == 0x08;
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
				case 0x08: // ADDI, with an explicit guarded exceptional transfer.
				case 0x09: // ADDIU
				case 0x0a: // SLTI
				case 0x0b: // SLTIU
				case 0x0c: // ANDI
				case 0x0d: // ORI
				case 0x0e: // XORI
				case 0x0f: // LUI
				case 0x19: // DADDIU
					return true;
				case 0x1c: // Scalar HI/LO arithmetic plus verified MMI operations.
					return DecodeIntegerMultiply(op).valid || DecodePureMmi(op, nullptr) ||
					       DecodePackedBinaryMmi(op, nullptr) ||
					       DecodePackedShiftMmi(op, nullptr);
				case 0x11: // Raw COP1 state plus exact S/W-format numerics.
					return DecodePureCop1State(op, nullptr) || IsCop1ControlWrite(op) ||
					       DecodeCop1UnaryWord(op, nullptr) ||
					       DecodeBasicCop1Arithmetic(op, nullptr) ||
					       DecodeCompoundCop1Arithmetic(op, nullptr) ||
					       DecodeCop1Compare(op, nullptr) ||
					       IsCop1ConvertWord(op) || IsCop1ConvertSingle(op);
				case 0x12: // Guarded VU0 macro arithmetic and raw transfers.
					return DecodeVu0ControlRead(op).valid ||
					       DecodeVu0ControlWrite(op).valid ||
					       DecodeVu0VectorTransfer(op).valid ||
					       DecodeVu0Fdiv(op).valid ||
					       DecodeVu0Fmac(op).valid ||
					       DecodeVu0Unary(op).valid;
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

		bool CanLowerNonBranch(u32 op, const LiftOptions& options)
		{
			return CanLowerPureNonBranch(op, options) ||
			       DecodeMemoryAccess(op, nullptr);
		}

		bool CanLowerDelaySlot(u32 control, u32 delay,
			const LiftOptions& options)
		{
			if (IsAnyControlFlow(delay))
				return false;

			// Branch-likely delay nodes are explicitly control-dependent on the taken
			// edge: the interpreter and both A32 backends skip the complete delay-node
			// slice on the annulled edge. Memory can therefore keep its exact
			// pre-control restart transfer. ADDI's exceptional guard is not yet part of
			// that taken-only contract and remains with tier zero.
			if (IsLikelyBranch(control))
			{
				return CanLowerNonBranch(delay, options) &&
				       !IsGuardedExceptionInstruction(delay);
			}
			return CanLowerNonBranch(delay, options);
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

		const SourceSpan* FindSourceSpan(const std::vector<SourceSpan>& spans, u32 pc)
		{
			if ((pc & 3u) != 0 || spans.empty())
				return nullptr;
			const auto after = std::upper_bound(spans.begin(), spans.end(), pc,
				[](u32 address, const SourceSpan& span) {
					return address < span.base_pc;
				});
			if (after == spans.begin())
				return nullptr;
			const SourceSpan& span = *std::prev(after);
			const u64 offset = static_cast<u64>(pc) - span.base_pc;
			return offset < static_cast<u64>(span.words.size()) * sizeof(u32) ?
				&span : nullptr;
		}

		bool ContainsPc(const std::vector<SourceSpan>& spans, u32 pc)
		{
			return FindSourceSpan(spans, pc) != nullptr;
		}

		u32 ReadSourceWord(const std::vector<SourceSpan>& spans, u32 pc)
		{
			const SourceSpan* span = FindSourceSpan(spans, pc);
			return span->words[(pc - span->base_pc) / sizeof(u32)];
		}

		bool ValidateSourceSpans(const std::vector<SourceSpan>& spans,
			const LiftOptions& options, u32* failure_pc, std::string* detail)
		{
			const auto fail = [&](u32 pc, const char* message) {
				if (failure_pc)
					*failure_pc = pc;
				if (detail)
					*detail = message;
				return false;
			};
			if (spans.empty())
				return fail(0, "immutable source image is empty");

			u64 total_words = 0;
			u64 previous_end = 0;
			for (u32 index = 0; index < spans.size(); index++)
			{
				const SourceSpan& span = spans[index];
				const u64 end = static_cast<u64>(span.base_pc) +
					static_cast<u64>(span.words.size()) * sizeof(u32);
				if ((span.base_pc & 3u) != 0 || span.words.empty() ||
					end > static_cast<u64>(UINT32_MAX) + 1)
				{
					return fail(span.base_pc,
						"immutable source span is empty, unaligned, or wraps");
				}
				if (index != 0 && static_cast<u64>(span.base_pc) < previous_end)
					return fail(span.base_pc, "immutable source spans overlap or are unsorted");
				previous_end = end;
				total_words += span.words.size();
				if (total_words > options.max_source_instructions)
					return fail(span.base_pc, "immutable source image exceeds its word limit");
			}
			return true;
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

		bool ValidateSourceBlockContracts(const std::vector<SourceSpan>& source_spans,
			const std::vector<SourceBlockContract>& contracts, u32 entry_pc,
			const LiftOptions& options, u32* failure_pc, std::string* detail,
			LiftFailureDetail* failure_detail)
		{
			const auto fail = [&](u32 pc, const char* message,
				LiftFailureDetail reason) {
				if (failure_pc)
					*failure_pc = pc;
				if (detail)
					*detail = message;
				if (failure_detail)
					*failure_detail = reason;
				return false;
			};
			if (contracts.empty())
				return true;

			u64 previous_end = 0;
			for (u32 index = 0; index < contracts.size(); index++)
			{
				const SourceBlockContract& contract = contracts[index];
				if ((contract.start_pc & 3u) != 0 || contract.instruction_count == 0 ||
					(contract.dependency_start_pc & 3u) != 0 ||
					contract.dependency_instruction_count == 0)
				{
					return fail(contract.start_pc,
						"source-block range is empty or unaligned",
						LiftFailureDetail::SourceBlockInvalidRange);
				}
				const u64 end = static_cast<u64>(contract.start_pc) +
					static_cast<u64>(contract.instruction_count) * sizeof(u32);
				const u64 dependency_end =
					static_cast<u64>(contract.dependency_start_pc) +
					static_cast<u64>(contract.dependency_instruction_count) * sizeof(u32);
				const SourceSpan* owner_span =
					FindSourceSpan(source_spans, contract.start_pc);
				const SourceSpan* dependency_span =
					FindSourceSpan(source_spans, contract.dependency_start_pc);
				const u64 owner_span_end = owner_span ?
					static_cast<u64>(owner_span->base_pc) +
						static_cast<u64>(owner_span->words.size()) * sizeof(u32) : 0;
				const u64 dependency_span_end = dependency_span ?
					static_cast<u64>(dependency_span->base_pc) +
						static_cast<u64>(dependency_span->words.size()) * sizeof(u32) : 0;
				if (end > static_cast<u64>(UINT32_MAX) + 1 ||
					dependency_end > static_cast<u64>(UINT32_MAX) + 1 ||
					!owner_span || !dependency_span || end > owner_span_end ||
					dependency_end > dependency_span_end ||
					contract.start_pc < contract.dependency_start_pc || end > dependency_end)
				{
					return fail(contract.start_pc,
						"source-block or dependency range leaves the immutable image",
						LiftFailureDetail::SourceBlockInvalidRange);
				}
				if (index != 0 && contract.start_pc < previous_end)
					return fail(contract.start_pc, "source-block contracts overlap",
						LiftFailureDetail::SourceBlockOverlap);
				previous_end = end;
			}

			const auto entry_contract = std::lower_bound(contracts.begin(), contracts.end(),
				entry_pc, [](const SourceBlockContract& contract, u32 pc) {
					return contract.start_pc < pc;
				});
			if (entry_contract == contracts.end() || entry_contract->start_pc != entry_pc)
				return fail(entry_pc, "entry is not an attested source-block start",
					LiftFailureDetail::SourceBlockMissingEntry);
			if (entry_contract->charged_scaled_cycles_before != 0)
				return fail(entry_pc,
					"entry begins inside a charged A32 split dependency",
					LiftFailureDetail::SourceBlockChargedEntry);
			if (entry_contract != contracts.begin())
			{
				const SourceBlockContract& predecessor = *std::prev(entry_contract);
				const u32 predecessor_end = predecessor.start_pc +
					predecessor.instruction_count * sizeof(u32);
				if (predecessor_end == entry_pc && !predecessor.scheduler_test_at_end)
					return fail(entry_pc,
						"entry follows an untested scheduler continuation",
						LiftFailureDetail::SourceBlockUntestedEntryPredecessor);
			}

			for (const SourceBlockContract& contract : contracts)
			{
				const u32 end = contract.start_pc +
					contract.instruction_count * sizeof(u32);
				if (!contract.scheduler_test_at_end &&
					!FindSourceBlockContract(contracts, end))
				{
					return fail(contract.start_pc,
						"scheduler-elided fragment has no owned continuation",
						LiftFailureDetail::SourceBlockMissingContinuation);
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
							"split dependency is not a complete charged-cycle chain",
							LiftFailureDetail::SourceBlockDiscontinuousDependency);
					}
					u32 raw_cycles = 0;
					for (u32 instruction = 0;
						instruction < fragment.instruction_count; instruction++)
					{
						raw_cycles += RawRecompilerCycles(ReadSourceWord(source_spans,
							fragment.start_pc + instruction * sizeof(u32)),
							options.cycle_factor);
					}
					next_pc += fragment.instruction_count * sizeof(u32);
					charged_cycles += ScaleBlockCycles(raw_cycles, options.ee_cycle_rate);
				}
				if (next_pc != dependency_end)
					return fail(owner.dependency_start_pc,
						"split dependency is not fully represented by source blocks",
						LiftFailureDetail::SourceBlockIncompleteDependency);
			}
			return true;
		}

		ExitReason ClassifyExternalResume(const std::vector<SourceSpan>& source_spans,
			u32 pc,
			const LiftOptions& options)
		{
			if (!ContainsPc(source_spans, pc))
				return ExitReason::RegionBoundary;

			const u32 op = ReadSourceWord(source_spans, pc);
			if (IsConditionalBranch(op) || CanLowerStaticJump(op, options) ||
				CanLowerRegisterJump(op, options))
			{
				const u32 delay_pc = pc + sizeof(u32);
				if (pc <= UINT32_MAX - sizeof(u32) && ContainsPc(source_spans, delay_pc))
				{
					const u32 delay = ReadSourceWord(source_spans, delay_pc);
					if (CanLowerDelaySlot(op, delay, options))
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

		bool ScanRawBlock(const std::vector<SourceSpan>& source_spans,
			const std::vector<SourceBlockContract>& source_blocks,
			const std::set<u32>& leaders, u32 start_pc, const LiftOptions& options,
			RawBlock* output,
			LiftFailure* failure, u32* failure_pc,
			LiftFailureDetail* failure_detail)
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
					if (failure_detail)
						*failure_detail = LiftFailureDetail::SourceBlockMissingLeader;
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
					if (failure_detail)
						*failure_detail = LiftFailureDetail::SourceBlockCrossesContract;
					*failure_pc = pc;
					return false;
				}
				if (pc != start_pc && leaders.contains(pc))
				{
					if (!source_blocks.empty())
					{
						*failure = LiftFailure::SourceBlockContract;
						if (failure_detail)
							*failure_detail = LiftFailureDetail::SourceBlockCrossesContract;
						*failure_pc = pc;
						return false;
					}
					raw.transfer_pc = pc;
					break;
				}

				if (!ContainsPc(source_spans, pc))
				{
					raw.transfer_pc = pc;
					break;
				}

				const u32 op = ReadSourceWord(source_spans, pc);
				const bool conditional_branch = IsConditionalBranch(op);
				const bool static_jump = CanLowerStaticJump(op, options);
				const bool register_jump = CanLowerRegisterJump(op, options);
				if (conditional_branch || static_jump || register_jump)
				{
					const u32 delay_pc = pc + sizeof(u32);
					if (!ContainsPc(source_spans, delay_pc))
					{
						*failure = LiftFailure::MissingDelaySlot;
						*failure_pc = pc;
						return false;
					}

					const u32 delay = ReadSourceWord(source_spans, delay_pc);
					if (IsAnyControlFlow(delay))
					{
						*failure = LiftFailure::BranchInDelaySlot;
						*failure_pc = delay_pc;
						return false;
					}
					if (!source_blocks.empty() && delay_pc + sizeof(u32) != contract_end_pc)
					{
						*failure = LiftFailure::SourceBlockContract;
						if (failure_detail)
							*failure_detail = LiftFailureDetail::SourceBlockControlNotAtEnd;
						*failure_pc = pc;
						return false;
					}

					// A branch and its delay slot are one architectural unit. If the
					// delay is not expressible with the exact edge/restart contract,
					// leave both to the existing provider.
					if (!CanLowerDelaySlot(op, delay, options))
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

		bool DescribeDirectCall(const std::vector<SourceSpan>& source_spans,
			const std::vector<SourceBlockContract>& source_blocks,
			const RawBlock& caller, const LiftOptions& options,
			DirectCallContract* output,
			const std::set<u32>* execution_owners = nullptr)
		{
			const auto execution_owns = [&](u32 pc) {
				return !execution_owners || execution_owners->contains(pc);
			};
			if (!output || options.max_direct_calls == 0 || source_blocks.empty() ||
				caller.control_kind != RawControlKind::StaticJump ||
				(caller.branch_opcode >> 26) != 0x03 ||
				caller.branch_pc > UINT32_MAX - 2 * sizeof(u32))
			{
				return false;
			}

			const u32 callee_pc = JumpTarget(caller.branch_pc, caller.branch_opcode);
			const u32 return_pc = caller.branch_pc + 2 * sizeof(u32);
			if (callee_pc == caller.pc || callee_pc == return_pc ||
				!ContainsPc(source_spans, callee_pc) ||
				!ContainsPc(source_spans, return_pc) ||
				!FindSourceBlockContract(source_blocks, callee_pc) ||
				!FindSourceBlockContract(source_blocks, return_pc))
			{
				return false;
			}

			// Prove the complete source-attested callee CFG rather than assuming that
			// the first tier-zero block is also the return leaf.  SDK routines commonly
			// have a scheduler-elided prologue followed by one reducible natural loop
			// and a JR r31 tail.  Every traversed edge must land on an immutable PCSX2
			// source-block contract; an omitted/cold arm therefore cannot accidentally
			// become part of the call contract merely because its bytes share a span.
			std::set<u32> callee_leaders;
			for (const SourceBlockContract& contract : source_blocks)
			{
				if (execution_owns(contract.start_pc))
					callee_leaders.insert(contract.start_pc);
			}
			std::deque<u32> pending = {callee_pc};
			std::set<u32> visited;
			u32 return_jump_pc = UINT32_MAX;
			while (!pending.empty())
			{
				const u32 pc = pending.front();
				pending.pop_front();
				if (!visited.insert(pc).second)
					continue;
				if (visited.size() > options.max_blocks || pc == return_pc ||
					!execution_owns(pc) ||
					!ContainsPc(source_spans, pc) ||
					!FindSourceBlockContract(source_blocks, pc))
				{
					return false;
				}

				RawBlock callee{};
				LiftFailure ignored_failure = LiftFailure::None;
				u32 ignored_pc = 0;
				if (!ScanRawBlock(source_spans, source_blocks, callee_leaders,
						pc, options, &callee, &ignored_failure, &ignored_pc,
						nullptr))
				{
					return false;
				}

				std::array<u32, 2> successors{};
				u32 successor_count = 0;
				if (callee.control_kind == RawControlKind::ConditionalBranch)
				{
					successors[successor_count++] =
						BranchTarget(callee.branch_pc, callee.branch_opcode);
					successors[successor_count++] =
						callee.branch_pc + 2 * sizeof(u32);
				}
				else if (callee.control_kind == RawControlKind::StaticJump)
				{
					// Nested link-register ownership needs a call stack contract.  Until
					// that is represented explicitly, accept only an ordinary J/tail edge.
					if ((callee.branch_opcode >> 26) != 0x02)
						return false;
					successors[successor_count++] =
						JumpTarget(callee.branch_pc, callee.branch_opcode);
				}
				else if (callee.control_kind == RawControlKind::RegisterJump)
				{
					if ((callee.branch_opcode >> 26) != 0 ||
						(callee.branch_opcode & 0x3fu) != 0x08 ||
						RS(callee.branch_opcode) != 31 ||
						(return_jump_pc != UINT32_MAX &&
						 return_jump_pc != callee.branch_pc))
					{
						return false;
					}
					return_jump_pc = callee.branch_pc;
					continue;
				}
				else
				{
					if (callee.transfer_reason != ExitReason::RegionBoundary)
						return false;
					successors[successor_count++] = callee.transfer_pc;
				}

				for (u32 index = 0; index < successor_count; index++)
				{
					const u32 target = successors[index];
					if (target == return_pc || !execution_owns(target) ||
						!ContainsPc(source_spans, target) ||
						!FindSourceBlockContract(source_blocks, target))
					{
						return false;
					}
					pending.push_back(target);
				}
			}
			if (return_jump_pc == UINT32_MAX)
				return false;

			*output = {caller.branch_pc, callee_pc, return_jump_pc, return_pc};
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
							Opcode::Parameter, ValueType::VuF32x4Bits, {}, 0,
							VU0_VF_PARAMETER_BASE + vf, 0, block.pc);
						if (block.parameters.vu0_vf[vf] == INVALID_VALUE)
							return false;
					}
					block.parameters.vu0_acc = AddNode(block,
						Opcode::Parameter, ValueType::VuF32x4Bits, {}, 0,
						VU0_ACC_PARAMETER, 0, block.pc);
					block.parameters.vu0_macflag = AddNode(block,
						Opcode::Parameter, ValueType::I32, {}, 0,
						VU0_MACFLAG_PARAMETER, 0, block.pc);
					block.parameters.vu0_statusflag = AddNode(block,
						Opcode::Parameter, ValueType::I32, {}, 0,
						VU0_STATUSFLAG_PARAMETER, 0, block.pc);
					block.parameters.vu0_clipflag = AddNode(block,
						Opcode::Parameter, ValueType::I32, {}, 0,
						VU0_CLIPFLAG_PARAMETER, 0, block.pc);
					block.parameters.vu0_q = AddNode(block,
						Opcode::Parameter, ValueType::I32, {}, 0,
						VU0_Q_PARAMETER, 0, block.pc);
					for (u32 vi = 0; vi < VU0_VI_COUNT; vi++)
					{
						block.parameters.vu0_vi[vi] = AddNode(block,
							Opcode::Parameter, ValueType::I32, {}, 0,
							VU0_VI_PARAMETER_BASE + vi, 0, block.pc);
					}
					for (u32 instance = 0; instance < 4; instance++)
					{
						block.parameters.vu0_micro_macflags[instance] = AddNode(block,
							Opcode::Parameter, ValueType::I32, {}, 0,
							VU0_MICRO_MACFLAG_PARAMETER_BASE + instance, 0, block.pc);
					}
					for (u32 instance = 0; instance < 4; instance++)
					{
						block.parameters.vu0_micro_clipflags[instance] = AddNode(block,
							Opcode::Parameter, ValueType::I32, {}, 0,
							VU0_MICRO_CLIPFLAG_PARAMETER_BASE + instance, 0, block.pc);
					}
					for (u32 instance = 0; instance < 4; instance++)
					{
						block.parameters.vu0_micro_statusflags[instance] = AddNode(block,
							Opcode::Parameter, ValueType::I32, {}, 0,
							VU0_MICRO_STATUSFLAG_PARAMETER_BASE + instance, 0, block.pc);
					}
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
						block.parameters.vu0_acc == INVALID_VALUE ||
						block.parameters.vu0_macflag == INVALID_VALUE ||
						block.parameters.vu0_statusflag == INVALID_VALUE ||
						block.parameters.vu0_clipflag == INVALID_VALUE ||
						block.parameters.vu0_q == INVALID_VALUE ||
						std::find(block.parameters.vu0_vi.begin(),
							block.parameters.vu0_vi.end(), INVALID_VALUE) !=
							block.parameters.vu0_vi.end() ||
						std::find(block.parameters.vu0_micro_macflags.begin(),
							block.parameters.vu0_micro_macflags.end(), INVALID_VALUE) !=
							block.parameters.vu0_micro_macflags.end() ||
						std::find(block.parameters.vu0_micro_clipflags.begin(),
							block.parameters.vu0_micro_clipflags.end(), INVALID_VALUE) !=
							block.parameters.vu0_micro_clipflags.end() ||
						std::find(block.parameters.vu0_micro_statusflags.begin(),
							block.parameters.vu0_micro_statusflags.end(), INVALID_VALUE) !=
							block.parameters.vu0_micro_statusflags.end() ||
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

			ValueId HiLoLaneLowWord(Block& block, ValueId value,
				bool upper_pipeline, u32 source_pc)
			{
				if (!upper_pipeline)
				{
					return Unary(block, Opcode::ExtractLow32, ValueType::I32,
						value, source_pc);
				}
				const ValueId high = Unary(block, Opcode::ExtractHigh64,
					ValueType::I64, value, source_pc);
				return Unary(block, Opcode::Truncate64To32, ValueType::I32,
					high, source_pc);
			}

			bool LowerIntegerMultiply(Block& block, StateMap* state, u32 op,
				u32 pc, const IntegerMultiplyOp& multiply)
			{
				if (!multiply.valid)
					return false;
				const ValueId left = Low32(block, *state, RS(op), pc);
				const ValueId right = Low32(block, *state, RT(op), pc);
				ValueId result = Binary(block,
					multiply.signed_multiply ? Opcode::MultiplySigned32 :
					                           Opcode::MultiplyUnsigned32,
					ValueType::I64, left, right, pc);
				if (result == INVALID_VALUE)
					return false;

				if (multiply.accumulate)
				{
					const ValueId lo_word = HiLoLaneLowWord(block, state->lo,
						multiply.upper_pipeline, pc);
					const ValueId hi_word = HiLoLaneLowWord(block, state->hi,
						multiply.upper_pipeline, pc);
					const ValueId lo = Unary(block, Opcode::ZeroExtend32To64,
						ValueType::I64, lo_word, pc);
					ValueId hi = Unary(block, Opcode::ZeroExtend32To64,
						ValueType::I64, hi_word, pc);
					hi = Unary(block, Opcode::ShiftLeft64, ValueType::I64, hi,
						pc, 32);
					const ValueId accumulator = Binary(block, Opcode::Or64,
						ValueType::I64, lo, hi, pc);
					result = Binary(block, Opcode::Add64, ValueType::I64,
						accumulator, result, pc);
					if (result == INVALID_VALUE)
						return false;
				}

				const ValueId low_word = Unary(block, Opcode::Truncate64To32,
					ValueType::I32, result, pc);
				const ValueId shifted_high = Unary(block,
					Opcode::ShiftRightLogical64, ValueType::I64, result, pc, 32);
				const ValueId high_word = Unary(block, Opcode::Truncate64To32,
					ValueType::I32, shifted_high, pc);
				const ValueId low_lane = Unary(block, Opcode::SignExtend32To64,
					ValueType::I64, low_word, pc);
				const ValueId high_lane = Unary(block, Opcode::SignExtend32To64,
					ValueType::I64, high_word, pc);
				if (low_lane == INVALID_VALUE || high_lane == INVALID_VALUE)
					return false;

				// The selected pipeline is a lane of the architectural 128-bit HI/LO
				// values. Keep the other lane byte-exact, including for MULT1/MADD1.
				const bool wrote_lo = multiply.upper_pipeline ?
					WriteHiLoHigh64(block, state, false, low_lane, pc) :
					WriteHiLoLow64(block, state, false, low_lane, pc);
				if (!wrote_lo)
					return false;
				const bool wrote_hi = multiply.upper_pipeline ?
					WriteHiLoHigh64(block, state, true, high_lane, pc) :
					WriteHiLoLow64(block, state, true, high_lane, pc);
				return wrote_hi &&
				       WriteLow64(block, state, RD(op), low_lane, pc);
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

			bool WriteVu0State(Block& block, ValueId* destination, Opcode bind,
				ValueId value, u32 source_pc)
			{
				if (!destination || value == INVALID_VALUE ||
					AddNode(block, bind, ValueType::Void,
						{value, INVALID_VALUE, INVALID_VALUE}, 1, 0, 0,
						source_pc) == INVALID_VALUE)
				{
					return false;
				}
				*destination = value;
				return true;
			}

			bool WriteVu0IndexedState(Block& block, ValueId* destination,
				Opcode bind, u32 index, ValueId value, u32 source_pc)
			{
				if (!destination || value == INVALID_VALUE ||
					AddNode(block, bind, ValueType::Void,
						{value, INVALID_VALUE, INVALID_VALUE}, 1, index, 0,
						source_pc) == INVALID_VALUE)
				{
					return false;
				}
				*destination = value;
				return true;
			}

			ValueId RequireVu0Idle(Block& block, const StateMap& state,
				ValueId value, ValueType value_type,
				const StateMap& fallback_state, u32 fallback_resume_pc, u32 pc,
				u32 pending_raw_cycles)
			{
				Transfer fallback = MakeDeferredObserverTransfer(block, fallback_state,
					fallback_resume_pc, ExitReason::HelperObserver, pc,
					pending_raw_cycles);
				if (fallback.pc == INVALID_VALUE)
					return INVALID_VALUE;
				const ValueId guarded = Binary(block, Opcode::Vu0RequireIdle,
					value_type, state.vu0_vi[29], value, pc);
				if (guarded == INVALID_VALUE)
					return INVALID_VALUE;
				block.observer_exits.push_back({guarded, std::move(fallback)});
				return guarded;
			}

			bool LowerVu0VectorTransfer(Block& block, StateMap* state, u32 op,
				u32 pc, const StateMap& fallback_state, u32 fallback_resume_pc,
				u32 pending_raw_cycles)
			{
				const Vu0VectorTransferOp transfer = DecodeVu0VectorTransfer(op);
				if (!transfer.valid)
					return false;

				const u32 rt = RT(op);
				const u32 vf = RD(op);
				if (transfer.kind == Vu0VectorTransferKind::FromVu0)
				{
					const ValueId guarded = RequireVu0Idle(block, *state,
						state->vu0_vf[vf], ValueType::VuF32x4Bits,
						fallback_state, fallback_resume_pc, pc, pending_raw_cycles);
					if (guarded == INVALID_VALUE)
						return false;
					if (rt == 0)
						return true;
					const ValueId bits = Unary(block,
						Opcode::BitcastVuF32x4BitsToI128, ValueType::I128,
						guarded, pc);
					return WriteFullGpr(block, state, rt, bits, pc);
				}

				// A VF0 destination discards the transfer but retains vu0Sync() and
				// the optional M-bit interlock. For a real destination, the decoder
				// has already rejected PCSX2's non-architectural raw-GPR0 source.
				if (vf == 0)
				{
					return RequireVu0Idle(block, *state, state->vu0_vf[0],
						ValueType::VuF32x4Bits, fallback_state, fallback_resume_pc,
						pc, pending_raw_cycles) != INVALID_VALUE;
				}
				const ValueId guarded = RequireVu0Idle(block, *state,
					state->gpr[rt], ValueType::I128, fallback_state,
					fallback_resume_pc, pc, pending_raw_cycles);
				if (guarded == INVALID_VALUE)
					return false;
				const ValueId bits = Unary(block,
					Opcode::BitcastI128ToVuF32x4Bits, ValueType::VuF32x4Bits,
					guarded, pc);
				return WriteVu0Vf(block, state, vf, bits, pc);
			}

			bool LowerVu0ControlRead(Block& block, StateMap* state, u32 op,
				u32 pc, const StateMap& fallback_state, u32 fallback_resume_pc,
				u32 pending_raw_cycles)
			{
				const Vu0ControlReadOp read = DecodeVu0ControlRead(op);
				if (!read.valid)
					return false;

				ValueId value = state->vu0_vi[29];
				if (RT(op) != 0)
				{
					switch (read.source)
					{
						case 0:
							value = Constant32(block, 0, pc);
							break;
						case 16:
							value = state->vu0_vi[16];
							break;
						case 17:
							value = state->vu0_vi[17];
							break;
						case 29:
							value = state->vu0_vi[29];
							break;
						default:
							return false;
					}
				}

				const ValueId guarded = RequireVu0Idle(block, *state, value,
					ValueType::I32, fallback_state, fallback_resume_pc, pc,
					pending_raw_cycles);
				if (guarded == INVALID_VALUE || RT(op) == 0)
					return guarded != INVALID_VALUE;
				const ValueId extended = Unary(block, Opcode::SignExtend32To64,
					ValueType::I64, guarded, pc);
				return extended != INVALID_VALUE &&
				       WriteLow64(block, state, RT(op), extended, pc);
			}

			bool LowerVu0ControlWrite(Block& block, StateMap* state, u32 op,
				u32 pc, const StateMap& fallback_state, u32 fallback_resume_pc,
				u32 pending_raw_cycles)
			{
				const Vu0ControlWriteOp write = DecodeVu0ControlWrite(op);
				if (!write.valid)
					return false;

				const ValueId source = Unary(block, Opcode::ExtractLow32,
					ValueType::I32, state->gpr[RT(op)], pc);
				const ValueId guarded = RequireVu0Idle(block, *state, source,
					ValueType::I32, fallback_state, fallback_resume_pc, pc,
					pending_raw_cycles);
				if (guarded == INVALID_VALUE)
					return false;

				const ValueId value = AddNode(block, Opcode::Vu0ControlWrite,
					ValueType::I32,
					{state->vu0_vi[write.target], guarded, INVALID_VALUE}, 2,
					write.target, 0, pc);
				if (value == INVALID_VALUE)
					return false;

				// VI0 and the three read-only controls retain their current state.
				if (write.target == 0 || write.target == VU0_MAC_FLAG ||
					write.target == VU0_TPC || write.target == VU0_VPU_STAT)
				{
					return true;
				}

				if (write.target == VU0_STATUS_FLAG)
				{
					const ValueId micro = Unary(block, Opcode::Vu0DenormalizeStatus,
						ValueType::I32, value, pc);
					if (micro == INVALID_VALUE ||
						!WriteVu0IndexedState(block, &state->vu0_vi[write.target],
							Opcode::BindVu0Vi, write.target, value, pc))
					{
						return false;
					}
					for (u32 instance = 0; instance < 4; instance++)
					{
						if (!WriteVu0IndexedState(block,
								&state->vu0_micro_statusflags[instance],
								Opcode::BindVu0MicroStatusFlag, instance, micro, pc))
						{
							return false;
						}
					}
					return true;
				}

				if (write.target == VU0_CLIP_FLAG &&
					!WriteVu0IndexedState(block, &state->vu0_clipflag,
						Opcode::BindVu0ClipFlag, 0, value, pc))
				{
					return false;
				}
				return WriteVu0IndexedState(block, &state->vu0_vi[write.target],
					Opcode::BindVu0Vi, write.target, value, pc);
			}

			bool LowerVu0Unary(Block& block, StateMap* state, u32 op, u32 pc,
				const StateMap& fallback_state, u32 fallback_resume_pc,
				u32 pending_raw_cycles)
			{
				const Vu0UnaryOp unary = DecodeVu0Unary(op);
				if (!unary.valid)
					return false;

				const u32 mask = RS(op) & 0x0fu;
				const u32 destination = RT(op);
				const u32 source = RD(op);
				const ValueId guarded = RequireVu0Idle(block, *state,
					state->vu0_vf[source], ValueType::VuF32x4Bits, fallback_state,
					fallback_resume_pc, pc, pending_raw_cycles);
				if (guarded == INVALID_VALUE)
					return false;
				if (mask == 0 || destination == 0)
					return true;

				ValueId value = guarded;
				if (unary.kind == Vu0UnaryKind::ConvertFixed)
				{
					value = Unary(block, Opcode::Vu0ConvertFixed,
						ValueType::VuF32x4Bits, guarded, pc, unary.offset);
					if (value == INVALID_VALUE)
						return false;
				}
				else if (unary.kind == Vu0UnaryKind::ConvertIntegerToFloat)
				{
					value = Unary(block, Opcode::Vu0ConvertIntegerToFloat,
						ValueType::VuF32x4Bits, guarded, pc, unary.offset);
					if (value == INVALID_VALUE)
						return false;
				}
				else if (unary.kind == Vu0UnaryKind::Rotate32)
				{
					value = Unary(block, Opcode::Vu0Rotate32,
						ValueType::VuF32x4Bits, guarded, pc);
					if (value == INVALID_VALUE)
						return false;
				}
				const ValueId merged = AddNode(block, Opcode::Vu0MergeMasked,
					ValueType::VuF32x4Bits,
					{state->vu0_vf[destination], value, INVALID_VALUE}, 2,
					mask, 0, pc);
				return merged != INVALID_VALUE &&
				       WriteVu0Vf(block, state, destination, merged, pc);
			}

			bool LowerVu0Fmac(Block& block, StateMap* state, u32 op,
				u32 pc, const StateMap& fallback_state, u32 fallback_resume_pc,
				u32 pending_raw_cycles)
			{
				const Vu0FmacOp fmac = DecodeVu0Fmac(op);
				if (!fmac.valid)
					return false;

				const u32 mask = RS(op) & 0x0fu;
				const u32 ft = RT(op);
				const u32 fs = RD(op);
				const u32 fd = SA(op);
				const ValueId guarded_fs = RequireVu0Idle(block, *state,
					state->vu0_vf[fs], ValueType::VuF32x4Bits, fallback_state,
					fallback_resume_pc, pc, pending_raw_cycles);
				const ValueId normalized_fs = Unary(block, Opcode::Vu0NormalizeVector,
					ValueType::VuF32x4Bits, guarded_fs, pc);
				ValueId operand = INVALID_VALUE;
				if (fmac.operand == Vu0FmacOperand::ScalarQ)
				{
					operand = Unary(block, Opcode::Vu0BroadcastScalar,
						ValueType::VuF32x4Bits, state->vu0_vi[22], pc);
				}
				else
				{
					const ValueId normalized_ft = Unary(block,
						Opcode::Vu0NormalizeVector, ValueType::VuF32x4Bits,
						state->vu0_vf[ft], pc);
					operand = normalized_ft;
					if (fmac.operand == Vu0FmacOperand::BroadcastLane)
					{
						operand = Unary(block, Opcode::Vu0BroadcastLane,
							ValueType::VuF32x4Bits, normalized_ft, pc, fmac.lane);
					}
				}
				if (guarded_fs == INVALID_VALUE || normalized_fs == INVALID_VALUE ||
					operand == INVALID_VALUE)
				{
					return false;
				}

				ValueId raw = INVALID_VALUE;
				switch (fmac.kind)
				{
					case Vu0FmacKind::Add:
						raw = Binary(block, Opcode::Vu0AddRaw,
							ValueType::VuF32x4Bits, normalized_fs, operand, pc);
						break;
					case Vu0FmacKind::Subtract:
						raw = Binary(block, Opcode::Vu0SubRaw,
							ValueType::VuF32x4Bits, normalized_fs, operand, pc);
						break;
					case Vu0FmacKind::Multiply:
						raw = Binary(block, Opcode::Vu0MulRaw,
							ValueType::VuF32x4Bits, normalized_fs, operand, pc);
						break;
					case Vu0FmacKind::MultiplyAdd:
					case Vu0FmacKind::MultiplySubtract:
					{
						const ValueId product = Binary(block, Opcode::Vu0MulRaw,
							ValueType::VuF32x4Bits, normalized_fs, operand, pc);
						const ValueId normalized_acc = Unary(block,
							Opcode::Vu0NormalizeVector, ValueType::VuF32x4Bits,
							state->vu0_acc, pc);
						raw = Binary(block,
							fmac.kind == Vu0FmacKind::MultiplyAdd ?
								Opcode::Vu0AddRaw : Opcode::Vu0SubRaw,
							ValueType::VuF32x4Bits, normalized_acc, product, pc);
						if (product == INVALID_VALUE || normalized_acc == INVALID_VALUE)
							return false;
						break;
					}
				}
				if (raw == INVALID_VALUE)
					return false;

				const ValueId result = Unary(block, Opcode::Vu0ClampFmacResult,
					ValueType::VuF32x4Bits, raw, pc, mask);
				const ValueId mac = Unary(block, Opcode::Vu0MacFlagsFromRaw,
					ValueType::I32, raw, pc, mask);
				const ValueId status = Unary(block, Opcode::Vu0StatusFlagsFromMac,
					ValueType::I32, mac, pc);
				const ValueId vi_status = Binary(block, Opcode::Vu0SyncStatusControl,
					ValueType::I32, state->vu0_vi[16], status, pc);
				if (result == INVALID_VALUE || mac == INVALID_VALUE ||
					status == INVALID_VALUE || vi_status == INVALID_VALUE ||
					!WriteVu0State(block, &state->vu0_macflag,
						Opcode::BindVu0MacFlag, mac, pc) ||
					!WriteVu0State(block, &state->vu0_statusflag,
						Opcode::BindVu0StatusFlag, status, pc) ||
					!WriteVu0State(block, &state->vu0_vi[17],
						Opcode::BindVu0ViMac, mac, pc) ||
					!WriteVu0State(block, &state->vu0_vi[16],
						Opcode::BindVu0ViStatus, vi_status, pc))
				{
					return false;
				}

				if (mask == 0)
					return true;
				ValueId& old_destination = fmac.accumulator_destination ?
					state->vu0_acc : state->vu0_vf[fd];
				const ValueId merged = AddNode(block, Opcode::Vu0MergeMasked,
					ValueType::VuF32x4Bits,
					{old_destination, result, INVALID_VALUE}, 2, mask, 0, pc);
				if (merged == INVALID_VALUE)
					return false;
				if (fmac.accumulator_destination)
				{
					return WriteVu0State(block, &state->vu0_acc,
						Opcode::BindVu0Acc, merged, pc);
				}
				return fd == 0 || WriteVu0Vf(block, state, fd, merged, pc);
			}

			bool LowerVu0Fdiv(Block& block, StateMap* state, u32 op,
				u32 pc, const StateMap& fallback_state, u32 fallback_resume_pc,
				u32 pending_raw_cycles)
			{
				const Vu0FdivOp fdiv = DecodeVu0Fdiv(op);
				if (!fdiv.valid)
					return false;

				const u32 fs = RD(op);
				const u32 ft = RT(op);
				const bool sqrt_only = fdiv.kind == Vu0FdivKind::SquareRoot;
				const u32 guarded_register = sqrt_only ? ft : fs;
				const ValueId guarded = RequireVu0Idle(block, *state,
					state->vu0_vf[guarded_register], ValueType::VuF32x4Bits,
					fallback_state, fallback_resume_pc, pc, pending_raw_cycles);
				if (guarded == INVALID_VALUE)
					return false;
				const ValueId fs_value = guarded;
				const ValueId ft_value = sqrt_only || fs == ft ?
					guarded : state->vu0_vf[ft];
				const u32 immediate = EncodeVu0FdivImmediate(fdiv);
				const ValueId q = AddNode(block, Opcode::Vu0FdivQ, ValueType::I32,
					{fs_value, ft_value, INVALID_VALUE}, 2, immediate, 0, pc);
				const ValueId flags = AddNode(block, Opcode::Vu0FdivFlags, ValueType::I32,
					{fs_value, ft_value, INVALID_VALUE}, 2, immediate, 0, pc);
				const ValueId status = Binary(block, Opcode::Vu0UpdateFdivStatus,
					ValueType::I32, state->vu0_statusflag, flags, pc);
				const ValueId vi_status = Binary(block,
					Opcode::Vu0SyncFdivStatusControl, ValueType::I32,
					state->vu0_vi[16], status, pc);
				if (q == INVALID_VALUE || flags == INVALID_VALUE ||
					status == INVALID_VALUE || vi_status == INVALID_VALUE ||
					!WriteVu0State(block, &state->vu0_q, Opcode::BindVu0Q, q, pc) ||
					!WriteVu0State(block, &state->vu0_vi[22], Opcode::BindVu0ViQ, q, pc) ||
					!WriteVu0State(block, &state->vu0_statusflag,
						Opcode::BindVu0StatusFlag, status, pc) ||
					!WriteVu0State(block, &state->vu0_vi[16],
						Opcode::BindVu0ViStatus, vi_status, pc))
				{
					return false;
				}
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
				if (m_program->options.cop1_lazy_ou_guards)
				{
					const ValueId exceptional = Unary(block,
						Opcode::Cop1ExceptionalOuResult, ValueType::I1, raw, pc);
					if (exceptional == INVALID_VALUE)
						return false;
				}
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
				if (raw == INVALID_VALUE)
					return false;
				if (m_program->options.cop1_lazy_ou_guards)
				{
					const ValueId exceptional = Unary(block,
						Opcode::Cop1ExceptionalOuResult, ValueType::I1, raw, pc);
					if (exceptional == INVALID_VALUE)
						return false;
				}
				const ValueId result = Unary(block, Opcode::Cop1ClampOuResult,
					ValueType::F32Bits, raw, pc);
				const ValueId flags = Binary(block, Opcode::Cop1UpdateOuFlags,
					ValueType::I32, state->fcr31, raw, pc);
				if (result == INVALID_VALUE ||
					flags == INVALID_VALUE || !WriteFcr31(block, state, flags, pc))
				{
					return false;
				}
				return WriteFpr(block, state, FD(op), result, pc);
			}

			bool LowerMemory(Block& block, StateMap* state, u32 op, u32 pc,
				MemoryAccessKind kind, const StateMap& fallback_state,
				u32 fallback_resume_pc, u32 pending_raw_cycles)
			{
				Transfer fallback = MakeDeferredObserverTransfer(block, fallback_state,
					fallback_resume_pc, ExitReason::MemoryObserver, pc,
					pending_raw_cycles);
				if (fallback.pc == INVALID_VALUE)
					return false;
				const bool fpr_access = IsFprMemoryAccess(kind);
				const bool vu0_access = IsVu0MemoryAccess(kind);
				const u32 destination = RT(op);
				ValueId architectural_value = fpr_access ? state->fpr[destination] :
					(vu0_access ? state->vu0_vf[destination] :
					              state->gpr[destination]);
				if (vu0_access)
				{
					architectural_value = RequireVu0Idle(block, *state,
						architectural_value, ValueType::VuF32x4Bits, fallback_state,
						fallback_resume_pc, pc, pending_raw_cycles);
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
					block.memory_exits.push_back({effect, std::move(fallback)});
					state->memory_effect = effect;
					if (!fpr_access && destination == 0)
						return true;
					const ValueId value = Unary(block, Opcode::MemoryLoadValue,
						fpr_access ? ValueType::F32Bits :
							(vu0_access ? ValueType::VuF32x4Bits : ValueType::I128),
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
				block.memory_exits.push_back({effect, std::move(fallback)});
				state->memory_effect = effect;
				return true;
			}

			bool LowerNonBranch(Block& block, StateMap* state, u32 op, u32 pc,
				const StateMap& exceptional_state, u32 exceptional_resume_pc,
				u32 pending_raw_cycles)
			{
				NoEffectKind no_effect{};
				if (DecodeNoEffect(op, m_program->options, &no_effect))
				{
					// PCSX2 owner: VU0.cpp::COP2_SPECIAL(). Even VNOP and
					// VWAITQ pass through _vu0FinishMicro() before the empty
					// SPECIAL2 body. Preserve that observer on a running VU0;
					// only the already-idle operation body is a no-op.
					if (NoEffectRequiresVu0Idle(no_effect) &&
						RequireVu0Idle(block, *state, state->vu0_vf[0],
							ValueType::VuF32x4Bits, exceptional_state,
							exceptional_resume_pc, pc, pending_raw_cycles) == INVALID_VALUE)
					{
						return false;
					}
					return AddNode(block, Opcode::NoEffect, ValueType::Void, {}, 0,
						static_cast<u32>(no_effect), 0, pc) != INVALID_VALUE;
				}

				MemoryAccessKind memory_kind{};
				if (DecodeMemoryAccess(op, &memory_kind))
				{
					return LowerMemory(block, state, op, pc, memory_kind,
						exceptional_state, exceptional_resume_pc,
						pending_raw_cycles);
				}
				const IntegerMultiplyOp integer_multiply =
					DecodeIntegerMultiply(op);
				if (integer_multiply.valid)
					return LowerIntegerMultiply(block, state, op, pc, integer_multiply);

				const u32 primary = op >> 26;
				if (primary == 0x12)
				{
					if (DecodeVu0Fdiv(op).valid)
					{
						return LowerVu0Fdiv(block, state, op, pc,
							exceptional_state, exceptional_resume_pc,
							pending_raw_cycles);
					}
					if (DecodeVu0ControlRead(op).valid)
					{
						return LowerVu0ControlRead(block, state, op, pc,
							exceptional_state, exceptional_resume_pc,
							pending_raw_cycles);
					}
					if (DecodeVu0ControlWrite(op).valid)
					{
						return LowerVu0ControlWrite(block, state, op, pc,
							exceptional_state, exceptional_resume_pc,
							pending_raw_cycles);
					}
					if (DecodeVu0VectorTransfer(op).valid)
					{
						return LowerVu0VectorTransfer(block, state, op, pc,
							exceptional_state, exceptional_resume_pc,
							pending_raw_cycles);
					}
					if (DecodeVu0Fmac(op).valid)
					{
						return LowerVu0Fmac(block, state, op, pc,
							exceptional_state, exceptional_resume_pc,
							pending_raw_cycles);
					}
					return LowerVu0Unary(block, state, op, pc, exceptional_state,
						exceptional_resume_pc, pending_raw_cycles);
				}
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
						Cop1UnaryWordKind unary_kind{};
						if (DecodeCop1UnaryWord(op, &unary_kind))
						{
							const ValueId result = Unary(block,
								Cop1UnaryWordOpcode(unary_kind), ValueType::F32Bits,
								state->fpr[FS(op)], pc);
							const ValueId flags = Unary(block, Opcode::Cop1ClearOuFlags,
								ValueType::I32, state->fcr31, pc);
							return result != INVALID_VALUE && flags != INVALID_VALUE &&
							       WriteFcr31(block, state, flags, pc) &&
							       WriteFpr(block, state, FD(op), result, pc);
						}
						BasicCop1ArithmeticKind arithmetic{};
						if (DecodeBasicCop1Arithmetic(op, &arithmetic))
						{
							return LowerBasicCop1Arithmetic(
								block, state, op, pc, arithmetic);
						}
						CompoundCop1ArithmeticKind compound{};
						if (DecodeCompoundCop1Arithmetic(op, &compound))
						{
							return LowerCompoundCop1Arithmetic(
								block, state, op, pc, compound);
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
					PackedShiftKind shift_kind{};
					if (DecodePackedShiftMmi(op, &shift_kind))
					{
						const u32 amount = static_cast<u32>(SA(op)) &
							(shift_kind <= PackedShiftKind::RightArithmetic16 ? 0x0fu : 0x1fu);
						const ValueId value = AddNode(block, Opcode::PackedShift128,
							ValueType::I128,
							{state->gpr[RT(op)], INVALID_VALUE, INVALID_VALUE}, 1,
							static_cast<u32>(shift_kind), amount, pc);
						return WriteFullGpr(block, state, RD(op), value, pc);
					}
					PackedBinaryKind packed_kind{};
					if (DecodePackedBinaryMmi(op, &packed_kind))
					{
						const ValueId value = AddNode(block, Opcode::PackedBinary128,
							ValueType::I128,
							{state->gpr[RS(op)], state->gpr[RT(op)], INVALID_VALUE}, 2,
							static_cast<u32>(packed_kind), 0, pc);
						return WriteFullGpr(block, state, RD(op), value, pc);
					}
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
					case 0x08: // ADDI
						left = Low32(block, *state, rs, pc);
						right = Constant32(block,
							static_cast<u32>(static_cast<s32>(IMM_S(op))), pc);
						value = Binary(block, Opcode::Add32, ValueType::I32, left,
							right, pc);
						{
							const ValueId overflow = Binary(block,
								Opcode::SignedAddOverflow32, ValueType::I1, left, right,
								pc);
							if (value == INVALID_VALUE || overflow == INVALID_VALUE ||
								!AddGuardedExit(block, overflow, exceptional_state,
									exceptional_resume_pc, pc, pending_raw_cycles))
							{
								return false;
							}
						}
						value = Unary(block, Opcode::SignExtend32To64,
							ValueType::I64, value, pc);
						return WriteLow64(block, state, rt, value, pc);
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
				bool cop1_true = false;
				if (DecodeCop1Branch(op, &cop1_true))
				{
					return Unary(block, Opcode::Cop1BranchCondition, ValueType::I1,
						state.fcr31, pc, cop1_true ? 1u : 0u);
				}
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
				bool event_horizon_check = true,
				u32 target_block = INVALID_BLOCK, u32 proven_target_pc = 0)
			{
				Transfer transfer{};
				transfer.state = state;
				transfer.pc = target;
				transfer.external_reason = reason;
				transfer.event_horizon_check = event_horizon_check;
				transfer.target_block = target_block;
				transfer.register_target_proven = target_block != INVALID_BLOCK;
				transfer.proven_register_target_pc = proven_target_pc;
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

			bool AddGuardedExit(Block& block, ValueId condition,
				const StateMap& state, u32 resume_pc, u32 source_pc,
				u32 pending_raw_cycles)
			{
				Transfer transfer = MakeDeferredObserverTransfer(block, state,
					resume_pc, ExitReason::ExceptionObserver, source_pc,
					pending_raw_cycles);
				if (transfer.pc == INVALID_VALUE ||
					block.guarded_exits.size() >= UINT32_MAX)
				{
					return false;
				}
				const u32 index = static_cast<u32>(block.guarded_exits.size());
				block.guarded_exits.push_back(std::move(transfer));
				return AddNode(block, Opcode::ExitIfTrue, ValueType::Void,
					{condition, INVALID_VALUE, INVALID_VALUE}, 1, index, 0,
					source_pc) != INVALID_VALUE;
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
			       left.vu0_acc == right.vu0_acc &&
			       left.vu0_macflag == right.vu0_macflag &&
			       left.vu0_statusflag == right.vu0_statusflag &&
			       left.vu0_clipflag == right.vu0_clipflag &&
			       left.vu0_q == right.vu0_q &&
			       left.vu0_vi == right.vu0_vi &&
			       left.vu0_micro_macflags == right.vu0_micro_macflags &&
			       left.vu0_micro_clipflags == right.vu0_micro_clipflags &&
			       left.vu0_micro_statusflags == right.vu0_micro_statusflags &&
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

	u128 NormalizeVu0Vector(const u128& value, bool overflow_clamp)
	{
		return NormalizeVuVector(value, overflow_clamp);
	}

	u128 BroadcastVu0Lane(const u128& value, u32 lane)
	{
		return BroadcastVuLane(value, lane);
	}

	u128 EvaluateVu0RawBinary(Opcode opcode, const u128& left,
		const u128& right)
	{
		return EvaluateVuRawBinary(opcode, left, right);
	}

	u128 ClampVu0FmacResult(const u128& raw, u32 mask,
		bool overflow_clamp)
	{
		return ClampVuFmacResult(raw, mask, overflow_clamp);
	}

	u32 EvaluateVu0MacFlags(const u128& raw, u32 mask)
	{
		return VuMacFlagsFromRaw(raw, mask);
	}

	u32 EvaluateVu0StatusFlags(u32 mac)
	{
		return VuStatusFlagsFromMac(mac);
	}

	u128 MergeVu0Masked(const u128& old_value, const u128& new_value,
		u32 mask)
	{
		return MergeVuMasked(old_value, new_value, mask);
	}

	u32 SyncVu0StatusControl(u32 old_status, u32 current_status)
	{
		return (old_status & 0x0fc0u) | current_status |
			(current_status << 6);
	}

	bool IsMemoryLoad(MemoryAccessKind kind)
	{
		return kind <= MemoryAccessKind::LoadVu0Vector;
	}

	SourceSpanMergeFailure MergeImmutableSourceSpan(
		std::vector<SourceSpan>* spans, SourceSpan incoming,
		u32 max_source_instructions, u32* required_source_instructions)
	{
		if (required_source_instructions)
			*required_source_instructions = 0;
		if (!spans || max_source_instructions == 0)
			return SourceSpanMergeFailure::InvalidSource;

		std::vector<SourceSpan> pending = *spans;
		pending.push_back(std::move(incoming));
		for (const SourceSpan& span : pending)
		{
			const u64 end = static_cast<u64>(span.base_pc) +
				static_cast<u64>(span.words.size()) * sizeof(u32);
			if ((span.base_pc & 3u) != 0 || span.words.empty() ||
				end > static_cast<u64>(UINT32_MAX) + 1)
			{
				return SourceSpanMergeFailure::InvalidSource;
			}
		}

		std::sort(pending.begin(), pending.end(),
			[](const SourceSpan& left, const SourceSpan& right) {
				return left.base_pc < right.base_pc;
			});
		std::vector<SourceSpan> merged;
		merged.reserve(pending.size());
		for (SourceSpan& next : pending)
		{
			if (merged.empty())
			{
				merged.push_back(std::move(next));
				continue;
			}

			SourceSpan& current = merged.back();
			const u64 current_end = static_cast<u64>(current.base_pc) +
				static_cast<u64>(current.words.size()) * sizeof(u32);
			if (static_cast<u64>(next.base_pc) > current_end)
			{
				merged.push_back(std::move(next));
				continue;
			}

			const size_t offset =
				static_cast<size_t>((next.base_pc - current.base_pc) / sizeof(u32));
			const size_t overlap = offset < current.words.size() ?
				std::min(current.words.size() - offset, next.words.size()) : 0;
			for (size_t index = 0; index < overlap; index++)
			{
				if (current.words[offset + index] != next.words[index])
					return SourceSpanMergeFailure::ConflictingSource;
			}
			if (next.words.size() > overlap)
			{
				current.words.insert(current.words.end(),
					next.words.begin() + overlap, next.words.end());
			}
		}

		u64 total_words = 0;
		for (const SourceSpan& span : merged)
			total_words += span.words.size();
		if (required_source_instructions)
		{
			*required_source_instructions = static_cast<u32>(
				std::min<u64>(total_words, UINT32_MAX));
		}
		if (total_words > max_source_instructions)
			return SourceSpanMergeFailure::SourceLimit;

		*spans = std::move(merged);
		return SourceSpanMergeFailure::None;
	}

	SuffixPartitionFailure BuildCycleProvenSuffixPartition(
		const SourceBlockContract& wide, const SourceBlockContract& suffix,
		bool cycle_timeline_proven, SourceBlockContract* prefix)
	{
		if (!prefix || (wide.start_pc & 3u) != 0 ||
			(suffix.start_pc & 3u) != 0 || wide.instruction_count == 0 ||
			suffix.instruction_count == 0 || suffix.start_pc <= wide.start_pc)
		{
			return SuffixPartitionFailure::InvalidRange;
		}

		const u64 wide_end = static_cast<u64>(wide.start_pc) +
			static_cast<u64>(wide.instruction_count) * sizeof(u32);
		const u64 suffix_end = static_cast<u64>(suffix.start_pc) +
			static_cast<u64>(suffix.instruction_count) * sizeof(u32);
		if (wide_end > static_cast<u64>(UINT32_MAX) + 1 ||
			suffix_end > static_cast<u64>(UINT32_MAX) + 1 ||
			wide_end != suffix_end)
		{
			return SuffixPartitionFailure::NotExactSuffix;
		}
		if (wide.dependency_start_pc != wide.start_pc ||
			wide.dependency_instruction_count != wide.instruction_count ||
			wide.charged_scaled_cycles_before != 0 ||
			suffix.dependency_start_pc != suffix.start_pc ||
			suffix.dependency_instruction_count != suffix.instruction_count ||
			suffix.charged_scaled_cycles_before != 0)
		{
			return SuffixPartitionFailure::NonStandaloneDependency;
		}
		if (wide.scheduler_test_at_end != suffix.scheduler_test_at_end)
			return SuffixPartitionFailure::SchedulerMismatch;
		if (!cycle_timeline_proven)
			return SuffixPartitionFailure::CycleTimeline;

		const u32 prefix_instructions =
			(suffix.start_pc - wide.start_pc) / sizeof(u32);
		if (prefix_instructions == 0 ||
			prefix_instructions >= wide.instruction_count)
		{
			return SuffixPartitionFailure::InvalidRange;
		}
		*prefix = {wide.start_pc, prefix_instructions, wide.start_pc,
			prefix_instructions, 0, false};
		return SuffixPartitionFailure::None;
	}

	u32 MemoryAccessWidth(MemoryAccessKind kind)
	{
		switch (kind)
		{
			case MemoryAccessKind::LoadS8:
			case MemoryAccessKind::LoadU8:
			case MemoryAccessKind::Store8:
				return 1;
			case MemoryAccessKind::LoadS16:
			case MemoryAccessKind::LoadU16:
			case MemoryAccessKind::Store16:
				return 2;
			case MemoryAccessKind::LoadS32:
			case MemoryAccessKind::LoadU32:
			case MemoryAccessKind::LoadF32Bits:
			case MemoryAccessKind::Store32:
			case MemoryAccessKind::StoreF32Bits:
				return 4;
			case MemoryAccessKind::Load64:
			case MemoryAccessKind::Store64:
				return 8;
			case MemoryAccessKind::Load128:
			case MemoryAccessKind::LoadVu0Vector:
			case MemoryAccessKind::Store128:
			case MemoryAccessKind::StoreVu0Vector:
				return 16;
		}
		return 0;
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
			// EE LQ/SQ mask the effective address down. The SCE COP2 transfer
			// contract instead requires a 128-bit-aligned effective address.
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
		       kind == MemoryAccessKind::LoadVu0Vector ||
		       kind == MemoryAccessKind::StoreVu0Vector;
	}

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

	static LiftResult LiftInternal(const SourceSpan* source_spans,
		u32 source_span_count, const SourceBlockContract* source_blocks,
		u32 source_block_count, u32 entry_pc, const LiftOptions& options,
		const u32* execution_owner_pcs = nullptr,
		u32 execution_owner_count = 0)
	{
		LiftResult result{};
		if (!source_spans || source_span_count == 0 ||
			options.cycle_factor < 1 || options.cycle_factor > 2 ||
			options.max_blocks == 0 || options.max_source_instructions == 0)
		{
			result.failure = LiftFailure::InvalidSource;
			result.failure_pc = entry_pc;
			return result;
		}
		result.program.source_spans.assign(source_spans,
			source_spans + source_span_count);
		u64 total_source_words = 0;
		for (const SourceSpan& span : result.program.source_spans)
			total_source_words += span.words.size();
		if (total_source_words > options.max_source_instructions)
		{
			result.failure = LiftFailure::SourceLimit;
			result.failure_pc = result.program.source_spans.front().base_pc;
			return result;
		}
		std::string source_span_detail;
		if (!ValidateSourceSpans(result.program.source_spans, options,
				&result.failure_pc, &source_span_detail))
		{
			result.failure = LiftFailure::InvalidSource;
			return result;
		}
		if (!ContainsPc(result.program.source_spans, entry_pc))
		{
			result.failure = LiftFailure::EntryOutsideSource;
			result.failure_pc = entry_pc;
			return result;
		}

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
		if (!ValidateSourceBlockContracts(result.program.source_spans,
				result.program.source_blocks, entry_pc,
				options, &result.failure_pc, &source_contract_detail,
				&result.failure_detail))
		{
			result.failure = LiftFailure::SourceBlockContract;
			return result;
		}
		std::set<u32> execution_owners;
		if (execution_owner_count != 0)
		{
			if (!execution_owner_pcs)
			{
				result.failure = LiftFailure::SourceBlockContract;
				result.failure_pc = entry_pc;
				return result;
			}
			execution_owners.insert(execution_owner_pcs,
				execution_owner_pcs + execution_owner_count);
			if (!execution_owners.contains(entry_pc))
			{
				result.failure = LiftFailure::SourceBlockContract;
				result.failure_pc = entry_pc;
				result.failure_detail = LiftFailureDetail::SourceBlockMissingEntry;
				return result;
			}
			for (const u32 pc : execution_owners)
			{
				if (!FindSourceBlockContract(result.program.source_blocks, pc))
				{
					result.failure = LiftFailure::SourceBlockContract;
					result.failure_pc = pc;
					result.failure_detail =
						LiftFailureDetail::SourceBlockMissingLeader;
					return result;
				}
			}
		}
		const auto execution_owns = [&](u32 pc) {
			return execution_owners.empty() || execution_owners.contains(pc);
		};

		std::set<u32> leaders = {entry_pc};
		std::map<u32, RawBlock> raw_blocks;
		std::map<u32, DirectCallContract> direct_calls;
		for (u32 pass = 0; pass <= options.max_blocks; pass++)
		{
			const size_t old_leader_count = leaders.size();
			raw_blocks.clear();
			direct_calls.clear();
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
				if (!ScanRawBlock(result.program.source_spans,
						result.program.source_blocks, leaders, pc, options, &raw,
						&result.failure, &result.failure_pc,
						&result.failure_detail))
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
						if (execution_owns(target) &&
							ContainsPc(result.program.source_spans, target) &&
							(result.program.source_blocks.empty() ||
							 FindSourceBlockContract(result.program.source_blocks,
								target)))
						{
							leaders.insert(target);
							pending.push_back(target);
						}
					}
				}
				else if (raw.control_kind == RawControlKind::StaticJump)
				{
					const u32 static_target =
						JumpTarget(raw.branch_pc, raw.branch_opcode);
					// A source-attested J is ordinary intraprocedural control flow. The
					// old single-span lifter left every static jump external because JAL
					// needs a separately proven return contract; that also prevented a
					// natural loop from spanning disjoint immutable source blocks. J has
					// no link-register effect, so an exact target already present in the
					// bounded source/contract set is safe to traverse like a branch edge.
					const bool internal_jump = (raw.branch_opcode >> 26) == 0x02 &&
						execution_owns(static_target) &&
						ContainsPc(result.program.source_spans, static_target) &&
						FindSourceBlockContract(result.program.source_blocks,
							static_target);
					if (internal_jump)
					{
						leaders.insert(static_target);
						pending.push_back(static_target);
					}
					else
					{
						DirectCallContract call{};
						const u32 return_pc = raw.branch_pc + 2 * sizeof(u32);
						const u32 callee_pc =
							JumpTarget(raw.branch_pc, raw.branch_opcode);
						bool unique = execution_owns(callee_pc) &&
							execution_owns(return_pc) && DescribeDirectCall(
							result.program.source_spans,
							result.program.source_blocks, raw, options, &call,
							execution_owners.empty() ? nullptr : &execution_owners) &&
							direct_calls.size() < options.max_direct_calls;
						for (const auto& [other_pc, other] : direct_calls)
						{
							(void)other_pc;
							// Several call sites may share one immutable callee/JR pair.
							// Their link values and return blocks must remain distinct,
							// while a shared callee must identify the same JR source.
							unique &= other.call_pc != call.call_pc &&
								other.return_pc != call.return_pc &&
								(other.callee_pc != call.callee_pc ||
								 other.return_jump_pc == call.return_jump_pc) &&
								(other.return_jump_pc != call.return_jump_pc ||
								 other.callee_pc == call.callee_pc);
						}
						if (unique)
						{
							direct_calls.emplace(call.call_pc, call);
							leaders.insert(call.callee_pc);
							leaders.insert(call.return_pc);
							pending.push_back(call.callee_pc);
							pending.push_back(call.return_pc);
						}
					}
					// Other jumps remain complete external control units. We never
					// recursively absorb arbitrary jump targets or unproven returns.
				}
				else if (raw.control_kind == RawControlKind::RegisterJump)
				{
					// A register target is external unless it is the exact callee return
					// paired with an admitted direct-call contract above.
				}
				else if (execution_owns(raw.transfer_pc) &&
						(leaders.contains(raw.transfer_pc) ||
							 FindSourceBlockContract(result.program.source_blocks,
								raw.transfer_pc)) &&
						 ContainsPc(result.program.source_spans, raw.transfer_pc))
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
			result.internal_stage = LiftInternalStage::RawBlockSet;
			result.failure_pc = entry_pc;
			return result;
		}
		result.program.direct_calls.reserve(direct_calls.size());
		for (const auto& [call_pc, call] : direct_calls)
		{
			(void)call_pc;
			result.program.direct_calls.push_back(call);
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
			std::vector<const DirectCallContract*> direct_returns;
			for (const DirectCallContract& call : result.program.direct_calls)
			{
				if (call.return_jump_pc == raw.branch_pc)
				{
					direct_returns.push_back(&call);
				}
			}
			const SourceBlockContract* source_contract =
				FindSourceBlockContract(result.program.source_blocks, block.pc);
			const bool event_horizon_check = !source_contract ||
				source_contract->scheduler_test_at_end;
			StateMap state = block.parameters;
			StateMap not_taken_state = state;
			block.source = raw.body;

			u32 body_raw_cycles = 0;
			for (const SourceInstruction& instruction : raw.body)
			{
				const StateMap exceptional_state = state;
				if (!builder.LowerNonBranch(block, &state, instruction.opcode,
						instruction.pc, exceptional_state, instruction.pc,
						body_raw_cycles))
				{
					result.failure = LiftFailure::InternalError;
					result.internal_stage = LiftInternalStage::Instruction;
					result.failure_pc = instruction.pc;
					return result;
				}
				body_raw_cycles += RawRecompilerCycles(instruction.opcode,
					options.cycle_factor);
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
			const StateMap pre_control_state = state;
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
						result.internal_stage = LiftInternalStage::BranchCondition;
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
						raw.delay.pc, pre_control_state, raw.branch_pc,
						body_raw_cycles))
				{
					result.failure = LiftFailure::InternalError;
					result.internal_stage = LiftInternalStage::DelaySlot;
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
				u32 return_block = INVALID_BLOCK;
				u32 return_pc = 0;
				block.terminator.taken = builder.MakeRegisterTransfer(state,
					register_target, ExitReason::RegionBoundary,
					event_horizon_check);
				for (const DirectCallContract* direct_return : direct_returns)
				{
					const auto found = block_indices.find(direct_return->return_pc);
					if (found == block_indices.end() ||
						state.gpr[31] != block.parameters.gpr[31])
					{
						result.failure = LiftFailure::DirectCallContract;
						result.failure_pc = raw.branch_pc;
						return result;
					}
					return_block = found->second;
					return_pc = direct_return->return_pc;
					block.terminator.register_targets.push_back(
						builder.MakeRegisterTransfer(state, register_target,
							ExitReason::RegionBoundary, event_horizon_check,
							return_block, return_pc));
				}
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
			result.internal_stage = LiftInternalStage::Verification;
			result.verify_failure = verified.failure;
			result.failure_pc = verified.block < result.program.blocks.size() ? result.program.blocks[verified.block].pc : entry_pc;
		}
		return result;
	}

	LiftResult Lift(u32 source_base_pc, const u32* source_words,
		u32 source_word_count, u32 entry_pc, const LiftOptions& options)
	{
		// Semantic/adversarial and source-coverage fixtures may remain unattested.
		// This overload cannot authorize future product execution.
		if (!source_words || source_word_count == 0)
		{
			LiftResult result{};
			result.failure = LiftFailure::InvalidSource;
			result.failure_pc = source_base_pc;
			return result;
		}
		SourceSpan span{};
		span.base_pc = source_base_pc;
		span.words.assign(source_words, source_words + source_word_count);
		return LiftInternal(&span, 1, nullptr, 0, entry_pc, options);
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
		if (!source_words || source_word_count == 0)
		{
			LiftResult result{};
			result.failure = LiftFailure::InvalidSource;
			result.failure_pc = source_base_pc;
			return result;
		}
		SourceSpan span{};
		span.base_pc = source_base_pc;
		span.words.assign(source_words, source_words + source_word_count);
		return LiftInternal(&span, 1, source_blocks, source_block_count,
			entry_pc, options);
	}

	LiftResult LiftWithSourceSpans(const SourceSpan* source_spans,
		u32 source_span_count, const SourceBlockContract* source_blocks,
		u32 source_block_count, u32 entry_pc, const LiftOptions& options,
		const u32* execution_owner_pcs, u32 execution_owner_count)
	{
		if (!source_blocks || source_block_count == 0)
		{
			LiftResult result{};
			result.failure = LiftFailure::SourceBlockContract;
			result.failure_pc = entry_pc;
			return result;
		}
		return LiftInternal(source_spans, source_span_count, source_blocks,
			source_block_count, entry_pc, options, execution_owner_pcs,
			execution_owner_count);
	}

	bool ProgramContainsPc(const Program& program, u32 pc)
	{
		return ContainsPc(program.source_spans, pc);
	}

	bool ReadProgramSourceWord(const Program& program, u32 pc, u32* word)
	{
		if (!word || !ContainsPc(program.source_spans, pc))
			return false;
		*word = ReadSourceWord(program.source_spans, pc);
		return true;
	}

	u32 ProgramSourceInstructionCount(const Program& program)
	{
		u64 count = 0;
		for (const SourceSpan& span : program.source_spans)
			count += span.words.size();
		return count <= UINT32_MAX ? static_cast<u32>(count) : UINT32_MAX;
	}

	bool HasExhaustiveDirectReturnTargets(const Program& program, u32 block_index)
	{
		if (block_index >= program.blocks.size())
			return false;
		const Block& block = program.blocks[block_index];
		if (block.terminator.kind != TerminatorKind::RegisterJump ||
			block.terminator.register_targets.empty())
		{
			return false;
		}

		u32 matching_calls = 0;
		for (const DirectCallContract& call : program.direct_calls)
		{
			if (call.return_jump_pc != block.terminator.branch_pc)
				continue;
			matching_calls++;
			u32 matches = 0;
			for (const Transfer& target : block.terminator.register_targets)
			{
				matches += target.register_target_proven &&
					target.proven_register_target_pc == call.return_pc &&
					target.target_block < program.blocks.size() &&
					program.blocks[target.target_block].pc == call.return_pc;
			}
			if (matches != 1)
				return false;
		}
		if (matching_calls == 0 ||
			matching_calls != block.terminator.register_targets.size())
		{
			return false;
		}
		for (const Transfer& target : block.terminator.register_targets)
		{
			u32 matches = 0;
			for (const DirectCallContract& call : program.direct_calls)
			{
				matches += call.return_jump_pc == block.terminator.branch_pc &&
					target.register_target_proven &&
					target.proven_register_target_pc == call.return_pc;
			}
			if (matches != 1)
				return false;
		}
		return true;
	}

	VerifyResult Verify(const Program& program)
	{
		std::string source_span_detail;
		if (program.blocks.empty() || program.value_count == 0 ||
			program.blocks.size() > program.options.max_blocks ||
			!ValidateSourceSpans(program.source_spans, program.options, nullptr,
				&source_span_detail) ||
			program.options.max_blocks == 0 ||
			program.options.max_source_instructions == 0 ||
			program.options.cycle_factor < 1 || program.options.cycle_factor > 2)
		{
			return Fail(VerifyFailure::InvalidProgram, INVALID_BLOCK, UINT32_MAX,
				source_span_detail.empty() ? "invalid region or lift options" :
					std::move(source_span_detail));
		}
		if (program.entry_block >= program.blocks.size())
			return Fail(VerifyFailure::InvalidEntry, program.entry_block, UINT32_MAX,
				"entry block is outside the CFG");
		std::string source_contract_detail;
		if (!ValidateSourceBlockContracts(program.source_spans,
				program.source_blocks,
				program.blocks[program.entry_block].pc, program.options,
				nullptr, &source_contract_detail, nullptr))
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
		if (program.direct_calls.size() > program.options.max_direct_calls)
		{
			return Fail(VerifyFailure::DirectCallContract, INVALID_BLOCK, UINT32_MAX,
				"direct-call count exceeds the configured semantic bound");
		}
		std::set<u32> direct_call_pcs;
		std::set<u32> direct_return_pcs;
		std::map<u32, u32> direct_return_jump_owners;
		std::map<u32, u32> expected_callee_callers;
		for (const DirectCallContract& call : program.direct_calls)
		{
			const auto callee_found = pc_to_block.find(call.callee_pc);
			const auto return_found = pc_to_block.find(call.return_pc);
			const auto return_block_found = std::find_if(program.blocks.begin(),
				program.blocks.end(), [&](const Block& block) {
					return block.terminator.branch_pc == call.return_jump_pc;
				});
			const auto caller_found = std::find_if(program.blocks.begin(),
				program.blocks.end(), [&](const Block& block) {
					return block.terminator.branch_pc == call.call_pc;
				});
			u32 call_opcode = 0;
			u32 return_opcode = 0;
			if (!direct_call_pcs.insert(call.call_pc).second ||
				!direct_return_pcs.insert(call.return_pc).second ||
				caller_found == program.blocks.end() ||
				callee_found == pc_to_block.end() ||
				return_found == pc_to_block.end() ||
				return_block_found == program.blocks.end() ||
				call.call_pc > UINT32_MAX - 2 * sizeof(u32) ||
				call.return_pc != call.call_pc + 2 * sizeof(u32) ||
				!ReadProgramSourceWord(program, call.call_pc, &call_opcode) ||
				!ReadProgramSourceWord(program, call.return_jump_pc, &return_opcode) ||
				(call_opcode >> 26) != 0x03 ||
				JumpTarget(call.call_pc, call_opcode) != call.callee_pc ||
				(return_opcode >> 26) != 0 ||
				(return_opcode & 0x3fu) != 0x08 || RS(return_opcode) != 31)
			{
				return Fail(VerifyFailure::DirectCallContract, INVALID_BLOCK,
					UINT32_MAX,
					"direct-call source, target, return, or uniqueness proof is invalid");
			}
			const auto return_owner =
				direct_return_jump_owners.emplace(call.return_jump_pc, call.callee_pc);
			if (!return_owner.second && return_owner.first->second != call.callee_pc)
			{
				return Fail(VerifyFailure::DirectCallContract, INVALID_BLOCK,
					UINT32_MAX, "one leaf-return instruction belongs to two callees");
			}

			const u32 caller_index = static_cast<u32>(
				caller_found - program.blocks.begin());
			const Block& caller = *caller_found;
			const u32 return_block_index = static_cast<u32>(
				return_block_found - program.blocks.begin());
			const Block& return_block = *return_block_found;
			const auto internal_return = std::find_if(
				return_block.terminator.register_targets.begin(),
				return_block.terminator.register_targets.end(),
				[&](const Transfer& transfer) {
					return transfer.target_block == return_found->second &&
						transfer.register_target_proven &&
						transfer.proven_register_target_pc == call.return_pc;
				});

			// Mechanically prove that every internal path from the callee entry reaches
			// its attested return block without changing r31.  This admits reducible
			// prologue/loop/tail routines while retaining the exact link-register
			// invariant previously obtained only from a one-block leaf.
			std::deque<u32> pending_callee = {callee_found->second};
			std::set<u32> visited_callee;
			bool complete_callee = true;
			while (!pending_callee.empty() && complete_callee)
			{
				const u32 block_index = pending_callee.front();
				pending_callee.pop_front();
				if (!visited_callee.insert(block_index).second)
					continue;
				const Block& block = program.blocks[block_index];
				if (block_index == return_block_index)
				{
					complete_callee =
						block.terminator.kind == TerminatorKind::RegisterJump &&
						block.terminator.branch_pc == call.return_jump_pc &&
						block.terminator.taken.state.gpr[31] ==
							block.parameters.gpr[31];
					continue;
				}

				u32 internal_successors = 0;
				auto inspect_transfer = [&](const Transfer& transfer, u8) {
					if (transfer.target_block == INVALID_BLOCK ||
						transfer.state.gpr[31] != block.parameters.gpr[31])
					{
						complete_callee = false;
						return false;
					}
					internal_successors++;
					pending_callee.push_back(transfer.target_block);
					return true;
				};
				complete_callee =
					block.terminator.kind != TerminatorKind::RegisterJump &&
					VisitInternalTransfers(block.terminator, inspect_transfer) &&
					internal_successors != 0;
			}
			complete_callee &= visited_callee.contains(return_block_index);
			if (caller.terminator.kind != TerminatorKind::Jump ||
				caller.terminator.taken.target_block != callee_found->second ||
				internal_return == return_block.terminator.register_targets.end() ||
				!complete_callee)
			{
				return Fail(VerifyFailure::DirectCallContract, caller_index,
					UINT32_MAX,
					"direct-call IR edges or unchanged r31 return proof disagree");
			}

			expected_callee_callers[call.callee_pc]++;
		}
		for (const auto& [callee_pc, expected_callers] : expected_callee_callers)
		{
			const u32 callee_index = pc_to_block.at(callee_pc);
			u32 incoming_callee_edges = 0;
			for (const Block& source : program.blocks)
			{
				incoming_callee_edges +=
					source.terminator.taken.target_block == callee_index;
				if (source.terminator.kind == TerminatorKind::Branch)
				{
					incoming_callee_edges +=
						source.terminator.not_taken.target_block == callee_index;
				}
				for (const Transfer& target : source.terminator.register_targets)
					incoming_callee_edges += target.target_block == callee_index;
			}
			const auto direct_call = std::find_if(program.direct_calls.begin(),
				program.direct_calls.end(), [&](const DirectCallContract& call) {
					return call.callee_pc == callee_pc;
				});
			const auto return_owner = direct_call == program.direct_calls.end() ?
				program.blocks.end() : std::find_if(program.blocks.begin(),
					program.blocks.end(), [&](const Block& block) {
						return block.terminator.branch_pc ==
							direct_call->return_jump_pc;
					});
			if (incoming_callee_edges != expected_callers ||
				return_owner == program.blocks.end() ||
				return_owner->terminator.register_targets.size() != expected_callers)
			{
				return Fail(VerifyFailure::DirectCallContract, callee_index,
					UINT32_MAX,
					"shared direct-call callers and return targets disagree");
			}
		}

		auto type_is = [&](ValueId value, ValueType type) {
			return value < types.size() && types[value] == type;
		};
		auto source_word_matches = [&](const SourceInstruction& source) {
			return ContainsPc(program.source_spans, source.pc) &&
			       ReadSourceWord(program.source_spans, source.pc) == source.opcode;
		};

		for (u32 block_index = 0; block_index < program.blocks.size();
			 block_index++)
		{
			const Block& block = program.blocks[block_index];
			auto direct_return_for_pc = [&](u32 return_pc) {
				return std::find_if(program.direct_calls.begin(),
					program.direct_calls.end(), [&](const DirectCallContract& call) {
						return call.return_jump_pc == block.terminator.branch_pc &&
							call.return_pc == return_pc;
					});
			};
			const SourceBlockContract* source_contract =
				FindSourceBlockContract(program.source_blocks, block.pc);
			if (!ContainsPc(program.source_spans, block.pc))
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
			if (block.terminator.kind != TerminatorKind::RegisterJump &&
				!block.terminator.register_targets.empty())
			{
				return Fail(VerifyFailure::DirectCallContract, block_index,
					UINT32_MAX,
					"non-register terminator carries internal register targets");
			}
			if (block.terminator.kind == TerminatorKind::RegisterJump)
			{
				if (block.terminator.register_targets.size() >
						program.options.max_direct_calls ||
					block.terminator.taken.register_target_proven ||
					block.terminator.taken.proven_register_target_pc != 0)
				{
					return Fail(VerifyFailure::DirectCallContract, block_index,
						UINT32_MAX,
						"register target set exceeds its bound or forges the unmatched edge");
				}
				std::set<u32> proven_pcs;
				std::set<u32> proven_blocks;
				for (const Transfer& target : block.terminator.register_targets)
				{
					if (target.target_block == INVALID_BLOCK ||
						!target.register_target_proven ||
						target.proven_register_target_pc == 0 ||
						!proven_pcs.insert(
							target.proven_register_target_pc).second ||
						!proven_blocks.insert(target.target_block).second)
					{
						return Fail(VerifyFailure::DirectCallContract, block_index,
							UINT32_MAX,
							"register return targets are absent, external, or ambiguous");
					}
				}
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
				const bool vu0_vi_parameter = slot >= VU0_VI_PARAMETER_BASE &&
				                              slot < VU0_VI_PARAMETER_BASE + VU0_VI_COUNT;
				const bool vu0_micro_flag_parameter =
					slot >= VU0_MICRO_MACFLAG_PARAMETER_BASE &&
					slot < VU0_MICRO_STATUSFLAG_PARAMETER_BASE + 4;
				const bool i32_parameter = slot == SA_PARAMETER ||
				                           slot == FCR0_PARAMETER ||
				                           slot == FCR31_PARAMETER ||
				                           slot == ACC_FLAG_PARAMETER ||
				                           slot == VU0_MACFLAG_PARAMETER ||
				                           slot == VU0_STATUSFLAG_PARAMETER ||
				                           slot == VU0_CLIPFLAG_PARAMETER ||
					                           slot == VU0_Q_PARAMETER ||
				                           vu0_vi_parameter ||
				                           vu0_micro_flag_parameter;
				const ValueType expected_type = i32_parameter ? ValueType::I32 :
					fpr_parameter ? ValueType::F32Bits :
					slot == ACC_PARAMETER ? ValueType::F32Bits :
					(vu0_vf_parameter || slot == VU0_ACC_PARAMETER) ?
						ValueType::VuF32x4Bits :
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
				else if (slot == VU0_ACC_PARAMETER)
					expected.vu0_acc = parameter.id;
				else if (slot == VU0_MACFLAG_PARAMETER)
					expected.vu0_macflag = parameter.id;
				else if (slot == VU0_STATUSFLAG_PARAMETER)
					expected.vu0_statusflag = parameter.id;
				else if (slot == VU0_CLIPFLAG_PARAMETER)
					expected.vu0_clipflag = parameter.id;
				else if (slot == VU0_Q_PARAMETER)
					expected.vu0_q = parameter.id;
				else if (vu0_vi_parameter)
					expected.vu0_vi[slot - VU0_VI_PARAMETER_BASE] = parameter.id;
				else if (slot >= VU0_MICRO_MACFLAG_PARAMETER_BASE &&
					slot < VU0_MICRO_MACFLAG_PARAMETER_BASE + 4)
				{
					expected.vu0_micro_macflags[
						slot - VU0_MICRO_MACFLAG_PARAMETER_BASE] = parameter.id;
				}
				else if (slot >= VU0_MICRO_CLIPFLAG_PARAMETER_BASE &&
					slot < VU0_MICRO_CLIPFLAG_PARAMETER_BASE + 4)
				{
					expected.vu0_micro_clipflags[
						slot - VU0_MICRO_CLIPFLAG_PARAMETER_BASE] = parameter.id;
				}
				else if (slot >= VU0_MICRO_STATUSFLAG_PARAMETER_BASE &&
					slot < VU0_MICRO_STATUSFLAG_PARAMETER_BASE + 4)
				{
					expected.vu0_micro_statusflags[
						slot - VU0_MICRO_STATUSFLAG_PARAMETER_BASE] = parameter.id;
				}
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
			std::map<u32, u32> raw_cycles_before_source;
			for (u32 i = 0; i < block.source.size(); i++)
			{
				const SourceInstruction& source = block.source[i];
				raw_cycles_before_source.emplace(source.pc, raw_cycles);
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
			std::map<u32, StateMap> source_input_state;
			std::map<u32, u32> addi_overflow_count;
			std::map<u32, ValueId> addi_overflow_condition;
			std::map<u32, u32> addi_guard_count;
			std::map<u32, u32> cop1_exception_count;
			std::map<u32, u32> addi_gpr_bind_count;
			std::vector<bool> guarded_exit_seen(block.guarded_exits.size(), false);
			std::vector<bool> observer_exit_seen(block.observer_exits.size(), false);
			std::vector<bool> memory_exit_seen(block.memory_exits.size(), false);
			std::map<u32, u32> extended_gpr_bind_count;
			std::map<u32, u32> integer_multiply_gpr_bind_count;
			std::map<u32, ValueId> integer_multiply_result;
			std::map<u32, u32> pure_mmi_gpr_bind_count;
			std::map<u32, u32> pure_cop1_gpr_bind_count;
			std::map<u32, u32> fpr_bind_count;
			std::map<u32, u32> vu0_idle_guard_count;
			std::map<u32, ValueId> vu0_idle_guard_value;
			std::map<u32, u32> vu0_control_read_gpr_bind_count;
			std::map<u32, ValueId> vu0_control_write_result;
			std::map<u32, ValueId> vu0_denormalized_status_result;
			std::map<u32, u32> vu0_vi_bind_count;
			std::map<u32, u32> vu0_clipflag_bind_count;
			std::map<u32, u32> vu0_micro_status_bind_count;
			std::map<u32, u32> vu0_transfer_gpr_bind_count;
			std::map<u32, u32> vu0_vf_bind_count;
			std::map<u32, u32> vu0_acc_bind_count;
			std::map<u32, u32> vu0_macflag_bind_count;
			std::map<u32, u32> vu0_statusflag_bind_count;
			std::map<u32, u32> vu0_vi_mac_bind_count;
			std::map<u32, u32> vu0_vi_status_bind_count;
			std::map<u32, u32> vu0_q_bind_count;
			std::map<u32, u32> vu0_vi_q_bind_count;
			std::map<u32, ValueId> vu0_fdiv_q_result;
			std::map<u32, ValueId> vu0_fdiv_flags_result;
			std::map<u32, ValueId> vu0_fdiv_status_result;
			std::map<u32, ValueId> vu0_fdiv_vi_status_result;
			std::map<u32, ValueId> vu0_fmac_raw_result;
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
			auto exact_integer_multiply_result = [&](ValueId value,
				u32 source_opcode, u32 source_pc, const StateMap& input) {
				const IntegerMultiplyOp multiply = DecodeIntegerMultiply(source_opcode);
				if (!multiply.valid)
					return false;

				const Node* product = local_node(value);
				if (multiply.accumulate)
				{
					const Node* sum = product;
					if (!sum || sum->opcode != Opcode::Add64 ||
						sum->operand_count != 2 || sum->source_pc != source_pc)
						return false;
					product = local_node(sum->operands[1]);
					const Node* accumulator = local_node(sum->operands[0]);
					if (!product || !accumulator ||
						accumulator->opcode != Opcode::Or64 ||
						accumulator->operand_count != 2 ||
						accumulator->source_pc != source_pc)
						return false;

					const Node* lo_extend = local_node(accumulator->operands[0]);
					const Node* hi_shift = local_node(accumulator->operands[1]);
					const Node* hi_extend = hi_shift && hi_shift->operand_count == 1 ?
						local_node(hi_shift->operands[0]) : nullptr;
					if (!lo_extend || lo_extend->opcode != Opcode::ZeroExtend32To64 ||
						lo_extend->operand_count != 1 ||
						lo_extend->source_pc != source_pc || !hi_shift ||
						hi_shift->opcode != Opcode::ShiftLeft64 ||
						hi_shift->operand_count != 1 || hi_shift->immediate != 32 ||
						hi_shift->source_pc != source_pc ||
						!hi_extend || hi_extend->opcode != Opcode::ZeroExtend32To64 ||
						hi_extend->operand_count != 1 ||
						hi_extend->source_pc != source_pc)
						return false;

					auto exact_lane_word = [&](ValueId word, ValueId hilo) {
						if (!multiply.upper_pipeline)
							return exact_unary(word, Opcode::ExtractLow32, hilo,
								source_pc);
						const Node* truncate = local_node(word);
						return truncate && truncate->opcode == Opcode::Truncate64To32 &&
						       truncate->operand_count == 1 &&
						       truncate->source_pc == source_pc &&
						       exact_unary(truncate->operands[0], Opcode::ExtractHigh64,
							   hilo, source_pc);
					};
					if (!exact_lane_word(lo_extend->operands[0], input.lo) ||
						!exact_lane_word(hi_extend->operands[0], input.hi))
						return false;
				}

				const Opcode expected_product = multiply.signed_multiply ?
					Opcode::MultiplySigned32 : Opcode::MultiplyUnsigned32;
				return product && product->opcode == expected_product &&
				       product->operand_count == 2 && product->source_pc == source_pc &&
				       exact_unary(product->operands[0], Opcode::ExtractLow32,
					   input.gpr[RS(source_opcode)], source_pc) &&
				       exact_unary(product->operands[1], Opcode::ExtractLow32,
					   input.gpr[RT(source_opcode)], source_pc);
			};
			auto exact_integer_multiply_lane = [&](ValueId value,
				u32 source_opcode, u32 source_pc, const StateMap& input,
				bool high_word, ValueId* result) {
				const Node* extend = local_node(value);
				if (!extend || extend->opcode != Opcode::SignExtend32To64 ||
					extend->operand_count != 1 || extend->source_pc != source_pc)
					return false;
				const Node* truncate = local_node(extend->operands[0]);
				if (!truncate || truncate->opcode != Opcode::Truncate64To32 ||
					truncate->operand_count != 1 || truncate->source_pc != source_pc)
					return false;

				ValueId raw_result = truncate->operands[0];
				if (high_word)
				{
					const Node* shift = local_node(raw_result);
					if (!shift || shift->opcode != Opcode::ShiftRightLogical64 ||
						shift->operand_count != 1 || shift->immediate != 32 ||
						shift->source_pc != source_pc)
						return false;
					raw_result = shift->operands[0];
				}
				if (!exact_integer_multiply_result(raw_result, source_opcode,
						source_pc, input))
				{
					return false;
				}
				if (result)
					*result = raw_result;
				return true;
			};
			auto record_integer_multiply_result = [&](u32 source_pc,
				ValueId value) {
				const auto [found, inserted] =
					integer_multiply_result.emplace(source_pc, value);
				return inserted || found->second == value;
			};
			auto exact_integer_multiply_gpr_bind = [&](const Node& bind,
				u32 source_opcode, const StateMap& input) {
				const u32 destination = RD(source_opcode);
				const Node* replace = local_node(bind.operands[0]);
				if (destination == 0 || bind.immediate != destination || !replace ||
					replace->opcode != Opcode::ReplaceLow64 ||
					replace->operand_count != 2 ||
					replace->operands[0] != input.gpr[destination] ||
					replace->source_pc != bind.source_pc)
				{
					return false;
				}
				ValueId result = INVALID_VALUE;
				return exact_integer_multiply_lane(replace->operands[1],
						source_opcode, bind.source_pc, input, false, &result) &&
				       record_integer_multiply_result(bind.source_pc, result);
			};
			auto exact_integer_multiply_hilo_bind = [&](const Node& bind,
				u32 source_opcode, const StateMap& input, bool hi) {
				const IntegerMultiplyOp multiply = DecodeIntegerMultiply(source_opcode);
				const Node* replace = local_node(bind.operands[0]);
				const Opcode expected_replace = multiply.upper_pipeline ?
					Opcode::ReplaceHigh64 : Opcode::ReplaceLow64;
				if (!multiply.valid || !replace || replace->opcode != expected_replace ||
					replace->operand_count != 2 ||
					replace->operands[0] != (hi ? input.hi : input.lo) ||
					replace->source_pc != bind.source_pc)
				{
					return false;
				}
				ValueId result = INVALID_VALUE;
				return exact_integer_multiply_lane(replace->operands[1],
						source_opcode, bind.source_pc, input, hi, &result) &&
				       record_integer_multiply_result(bind.source_pc, result);
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
			auto exact_packed_mmi_gpr_bind = [&](const Node& bind,
				u32 source_opcode, const StateMap& input) {
				PackedBinaryKind kind{};
				const u32 destination = RD(source_opcode);
				if (!DecodePackedBinaryMmi(source_opcode, &kind) || destination == 0 ||
					bind.immediate != destination)
				{
					return false;
				}
				const Node* operation = local_node(bind.operands[0]);
				return operation && operation->opcode == Opcode::PackedBinary128 &&
				       operation->type == ValueType::I128 &&
				       operation->operand_count == 2 &&
				       operation->operands[0] == input.gpr[RS(source_opcode)] &&
				       operation->operands[1] == input.gpr[RT(source_opcode)] &&
				       operation->immediate == static_cast<u32>(kind) &&
				       operation->source_pc == bind.source_pc;
			};
			auto exact_packed_shift_gpr_bind = [&](const Node& bind,
				u32 source_opcode, const StateMap& input) {
				PackedShiftKind kind{};
				const u32 destination = RD(source_opcode);
				if (!DecodePackedShiftMmi(source_opcode, &kind) || destination == 0 ||
					bind.immediate != destination)
				{
					return false;
				}
				const u32 amount = static_cast<u32>(SA(source_opcode)) &
					(kind <= PackedShiftKind::RightArithmetic16 ? 0x0fu : 0x1fu);
				const Node* operation = local_node(bind.operands[0]);
				return operation && operation->opcode == Opcode::PackedShift128 &&
				       operation->type == ValueType::I128 &&
				       operation->operand_count == 1 &&
				       operation->operands[0] == input.gpr[RT(source_opcode)] &&
				       operation->immediate == static_cast<u32>(kind) &&
				       operation->literal == amount &&
				       operation->source_pc == bind.source_pc;
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
			auto exact_addi_gpr_bind = [&](const Node& bind, u32 source_opcode,
				const StateMap& input) {
				const u32 destination = RT(source_opcode);
				if (!IsGuardedExceptionInstruction(source_opcode) || destination == 0 ||
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
				if (!sign_extend || sign_extend->opcode != Opcode::SignExtend32To64 ||
					sign_extend->operand_count != 1 ||
					sign_extend->source_pc != bind.source_pc)
				{
					return false;
				}
				const Node* sum = local_node(sign_extend->operands[0]);
				const auto condition = addi_overflow_condition.find(bind.source_pc);
				const Node* overflow = condition == addi_overflow_condition.end() ?
					nullptr : local_node(condition->second);
				return sum && sum->opcode == Opcode::Add32 && sum->operand_count == 2 &&
				       sum->source_pc == bind.source_pc && overflow &&
				       overflow->opcode == Opcode::SignedAddOverflow32 &&
				       overflow->operand_count == 2 &&
				       overflow->operands[0] == sum->operands[0] &&
				       overflow->operands[1] == sum->operands[1] &&
				       exact_unary(sum->operands[0], Opcode::ExtractLow32,
						input.gpr[RS(source_opcode)], bind.source_pc) &&
				       exact_constant32(sum->operands[1],
						static_cast<u32>(static_cast<s32>(IMM_S(source_opcode))),
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
			auto exact_vu0_fmac_raw = [&](ValueId value, u32 source_opcode,
				u32 source_pc, const StateMap& input) {
				const Vu0FmacOp fmac = DecodeVu0Fmac(source_opcode);
				const auto guard = vu0_idle_guard_value.find(source_pc);
				if (!fmac.valid || guard == vu0_idle_guard_value.end())
					return false;

				auto exact_operand = [&](ValueId operand) {
					if (fmac.operand == Vu0FmacOperand::ScalarQ)
					{
						return exact_unary(operand, Opcode::Vu0BroadcastScalar,
							input.vu0_vi[22], source_pc);
					}
					if (fmac.operand == Vu0FmacOperand::Vector)
					{
						return exact_unary(operand, Opcode::Vu0NormalizeVector,
							input.vu0_vf[RT(source_opcode)], source_pc);
					}
					const Node* broadcast = local_node(operand);
					return broadcast && broadcast->opcode == Opcode::Vu0BroadcastLane &&
					       broadcast->operand_count == 1 &&
					       broadcast->immediate == fmac.lane &&
					       broadcast->source_pc == source_pc &&
					       exact_unary(broadcast->operands[0],
							Opcode::Vu0NormalizeVector, input.vu0_vf[RT(source_opcode)],
							source_pc);
				};
				auto exact_binary = [&](const Node* raw, Opcode opcode,
					ValueId left, bool normalized_left) {
					return raw && raw->opcode == opcode && raw->operand_count == 2 &&
					       raw->source_pc == source_pc &&
					       (normalized_left ?
							exact_unary(raw->operands[0], Opcode::Vu0NormalizeVector,
								left, source_pc) : raw->operands[0] == left) &&
					       exact_operand(raw->operands[1]);
				};

				const Node* const final_raw = local_node(value);
				switch (fmac.kind)
				{
					case Vu0FmacKind::Add:
						return exact_binary(final_raw, Opcode::Vu0AddRaw,
							guard->second, true);
					case Vu0FmacKind::Subtract:
						return exact_binary(final_raw, Opcode::Vu0SubRaw,
							guard->second, true);
					case Vu0FmacKind::Multiply:
						return exact_binary(final_raw, Opcode::Vu0MulRaw,
							guard->second, true);
					case Vu0FmacKind::MultiplyAdd:
					case Vu0FmacKind::MultiplySubtract:
					{
						const Opcode final_opcode =
							fmac.kind == Vu0FmacKind::MultiplyAdd ?
								Opcode::Vu0AddRaw : Opcode::Vu0SubRaw;
						if (!final_raw || final_raw->opcode != final_opcode ||
							final_raw->operand_count != 2 ||
							final_raw->source_pc != source_pc ||
							!exact_unary(final_raw->operands[0],
								Opcode::Vu0NormalizeVector, input.vu0_acc, source_pc))
						{
							return false;
						}
						const Node* const product = local_node(final_raw->operands[1]);
						return exact_binary(product, Opcode::Vu0MulRaw,
							guard->second, true);
					}
				}
				return false;
			};
			auto exact_vu0_fmac_clamp = [&](ValueId value, ValueId raw,
				u32 source_opcode, u32 source_pc) {
				const Node* clamp = local_node(value);
				return clamp && clamp->opcode == Opcode::Vu0ClampFmacResult &&
				       clamp->operand_count == 1 && clamp->operands[0] == raw &&
				       clamp->immediate == (RS(source_opcode) & 0x0fu) &&
				       clamp->source_pc == source_pc;
			};
			auto exact_vu0_fdiv_node = [&](ValueId value, Opcode opcode,
				u32 source_opcode, u32 source_pc, const StateMap& input) {
				const Vu0FdivOp fdiv = DecodeVu0Fdiv(source_opcode);
				const auto guard = vu0_idle_guard_value.find(source_pc);
				const Node* node = local_node(value);
				if (!fdiv.valid || guard == vu0_idle_guard_value.end() || !node ||
					node->opcode != opcode || node->type != ValueType::I32 ||
					node->operand_count != 2 ||
					node->immediate != EncodeVu0FdivImmediate(fdiv) ||
					node->literal != 0 || node->source_pc != source_pc)
				{
					return false;
				}
				const bool sqrt_only = fdiv.kind == Vu0FdivKind::SquareRoot;
				const u32 fs = RD(source_opcode);
				const u32 ft = RT(source_opcode);
				const ValueId expected_guarded = guard->second;
				const ValueId expected_ft = sqrt_only || fs == ft ?
					expected_guarded : input.vu0_vf[ft];
				return node->operands[0] == expected_guarded &&
				       node->operands[1] == expected_ft;
			};
			auto exact_vu0_unary_result = [&](ValueId value, u32 source_opcode,
				u32 source_pc) {
				const Vu0UnaryOp unary = DecodeVu0Unary(source_opcode);
				const auto guard = vu0_idle_guard_value.find(source_pc);
				if (!unary.valid || guard == vu0_idle_guard_value.end())
					return false;
				if (unary.kind == Vu0UnaryKind::Move)
					return value == guard->second;
				const Node* convert = local_node(value);
				Opcode expected_opcode = Opcode::Vu0ConvertIntegerToFloat;
				if (unary.kind == Vu0UnaryKind::ConvertFixed)
					expected_opcode = Opcode::Vu0ConvertFixed;
				else if (unary.kind == Vu0UnaryKind::Rotate32)
					expected_opcode = Opcode::Vu0Rotate32;
				return convert && convert->opcode == expected_opcode &&
				       convert->type == ValueType::VuF32x4Bits &&
				       convert->operand_count == 1 &&
				       convert->operands[0] == guard->second &&
				       convert->immediate == unary.offset && convert->literal == 0 &&
				       convert->source_pc == source_pc;
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
				u32 owning_source_opcode = 0;
				if (source_opcode_at(node.source_pc, &owning_source_opcode) &&
					!source_input_state.contains(node.source_pc))
				{
					source_input_state.emplace(node.source_pc, expected);
				}
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
								"no-effect node does not match its decoded source");
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
					case Opcode::BitcastI128ToVuF32x4Bits:
					case Opcode::BitcastVuF32x4BitsToI128:
					{
						const bool to_vu0 =
							node.opcode == Opcode::BitcastI128ToVuF32x4Bits;
						checked = to_vu0 ?
							unary(ValueType::I128, ValueType::VuF32x4Bits) :
							unary(ValueType::VuF32x4Bits, ValueType::I128);
						u32 source_opcode = 0;
						const Vu0VectorTransferOp transfer =
							source_opcode_at(node.source_pc, &source_opcode) ?
								DecodeVu0VectorTransfer(source_opcode) :
								Vu0VectorTransferOp{};
						const auto guard = vu0_idle_guard_value.find(node.source_pc);
						if (checked && (!transfer.valid ||
							(to_vu0 != (transfer.kind ==
								Vu0VectorTransferKind::ToVu0)) ||
							guard == vu0_idle_guard_value.end() ||
							node.operands[0] != guard->second || node.immediate != 0 ||
							node.literal != 0))
						{
							checked = Fail(VerifyFailure::SourceMismatch, block_index,
								node_index,
								"VU0 vector-transfer bitcast does not consume its exact idle-guarded source");
						}
						break;
					}
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
					case Opcode::Cop1ExceptionalOuResult:
					{
						checked = unary(ValueType::F32Bits, ValueType::I1);
						u32 source_opcode = 0;
						const auto input = source_input_state.find(node.source_pc);
						if (checked &&
							(!source_opcode_at(node.source_pc, &source_opcode) ||
							 !IsCop1OuArithmetic(source_opcode) ||
							 input == source_input_state.end() ||
							 !exact_cop1_ou_raw(node.operands[0], source_opcode,
								node.source_pc, input->second)))
						{
							checked = Fail(VerifyFailure::SourceMismatch, block_index,
								node_index,
								"COP1 exceptional predicate does not own the exact final raw result");
							break;
						}
						if (cop1_exception_count[node.source_pc]++ != 0)
						{
							checked = Fail(VerifyFailure::SourceMismatch, block_index,
								node_index,
								"COP1 arithmetic owns more than one exceptional predicate");
							break;
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
					case Opcode::Cop1BranchCondition:
					{
						checked = unary(ValueType::I32, ValueType::I1);
						u32 source_opcode = 0;
						bool branch_on_true = false;
						const auto input = source_input_state.find(node.source_pc);
						if (checked &&
							(!source_opcode_at(node.source_pc, &source_opcode) ||
							 !DecodeCop1Branch(source_opcode, &branch_on_true) ||
							 input == source_input_state.end() ||
							 node.operands[0] != input->second.fcr31 ||
							 node.immediate != (branch_on_true ? 1u : 0u) ||
							 node.literal != 0))
						{
							checked = Fail(VerifyFailure::SourceMismatch, block_index,
								node_index,
								"COP1 branch predicate does not snapshot decoded FCR31.C");
						}
						break;
					}
					case Opcode::Cop1AbsoluteWord:
					case Opcode::Cop1NegateWord:
					{
						checked = unary(ValueType::F32Bits, ValueType::F32Bits);
						u32 source_opcode = 0;
						Cop1UnaryWordKind kind{};
						const auto input = source_input_state.find(node.source_pc);
						if (checked &&
							(!source_opcode_at(node.source_pc, &source_opcode) ||
							 !DecodeCop1UnaryWord(source_opcode, &kind) ||
							 node.opcode != Cop1UnaryWordOpcode(kind) ||
							 input == source_input_state.end() ||
							 node.operands[0] != input->second.fpr[FS(source_opcode)] ||
							 node.immediate != 0 || node.literal != 0))
						{
							checked = Fail(VerifyFailure::SourceMismatch, block_index,
								node_index,
								"COP1 unary word transform does not match its decoded source");
						}
						break;
					}
					case Opcode::Cop1ClearOuFlags:
					{
						checked = unary(ValueType::I32, ValueType::I32);
						u32 source_opcode = 0;
						const auto input = source_input_state.find(node.source_pc);
						if (checked &&
							(!source_opcode_at(node.source_pc, &source_opcode) ||
							 !DecodeCop1UnaryWord(source_opcode, nullptr) ||
							 input == source_input_state.end() ||
							 node.operands[0] != input->second.fcr31 ||
							 node.immediate != 0 || node.literal != 0))
						{
							checked = Fail(VerifyFailure::SourceMismatch, block_index,
								node_index,
								"COP1 unary O/U clear does not consume exact entry FCR31");
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
					case Opcode::MultiplySigned32:
					case Opcode::MultiplyUnsigned32:
					{
						checked = binary(ValueType::I32, ValueType::I32,
							ValueType::I64);
						u32 source_opcode = 0;
						const auto input = source_input_state.find(node.source_pc);
						const IntegerMultiplyOp multiply =
							source_opcode_at(node.source_pc, &source_opcode) ?
								DecodeIntegerMultiply(source_opcode) : IntegerMultiplyOp{};
						if (checked && (!multiply.valid ||
							(node.opcode == Opcode::MultiplySigned32) !=
								multiply.signed_multiply ||
							input == source_input_state.end() ||
							!exact_unary(node.operands[0], Opcode::ExtractLow32,
								input->second.gpr[RS(source_opcode)], node.source_pc) ||
							!exact_unary(node.operands[1], Opcode::ExtractLow32,
								input->second.gpr[RT(source_opcode)], node.source_pc)))
						{
							checked = Fail(VerifyFailure::SourceMismatch, block_index,
								node_index,
								"integer multiply does not match its decoded low-word operands");
						}
						break;
					}
					case Opcode::Truncate64To32:
						checked = unary(ValueType::I64, ValueType::I32);
						break;
					case Opcode::Add32:
					case Opcode::Sub32:
					case Opcode::And32:
					case Opcode::Xor32:
						checked = binary(ValueType::I32, ValueType::I32, ValueType::I32);
						break;
					case Opcode::SignedAddOverflow32:
					{
						checked = binary(ValueType::I32, ValueType::I32, ValueType::I1);
						u32 source_opcode = 0;
						const auto source_state = source_input_state.find(node.source_pc);
						if (checked &&
							(!source_opcode_at(node.source_pc, &source_opcode) ||
							 !IsGuardedExceptionInstruction(source_opcode) ||
							 source_state == source_input_state.end() ||
							 !exact_unary(node.operands[0], Opcode::ExtractLow32,
								source_state->second.gpr[RS(source_opcode)], node.source_pc) ||
							 !exact_constant32(node.operands[1],
								static_cast<u32>(static_cast<s32>(IMM_S(source_opcode))),
								node.source_pc)))
						{
							checked = Fail(VerifyFailure::SourceMismatch, block_index,
								node_index,
								"signed-overflow predicate does not match decoded ADDI operands");
							break;
						}
						if (addi_overflow_count[node.source_pc]++ != 0)
						{
							checked = Fail(VerifyFailure::SourceMismatch, block_index,
								node_index,
								"ADDI owns more than one signed-overflow predicate");
							break;
						}
						addi_overflow_condition[node.source_pc] = node.id;
						break;
					}
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
					case Opcode::PackedBinary128:
					{
						checked = binary(ValueType::I128, ValueType::I128,
							ValueType::I128);
						u32 source_opcode = 0;
						PackedBinaryKind kind{};
						const auto input = source_input_state.find(node.source_pc);
						if (checked &&
							(!source_opcode_at(node.source_pc, &source_opcode) ||
							 !DecodePackedBinaryMmi(source_opcode, &kind) ||
							 node.immediate != static_cast<u32>(kind) ||
							 node.immediate >= static_cast<u32>(PackedBinaryKind::Count) ||
							 input == source_input_state.end() ||
							 node.operands[0] != input->second.gpr[RS(source_opcode)] ||
							 node.operands[1] != input->second.gpr[RT(source_opcode)]))
						{
							checked = Fail(VerifyFailure::SourceMismatch, block_index,
								node_index,
								"packed MMI operation does not match its decoded lane semantics");
						}
						break;
					}
					case Opcode::PackedShift128:
					{
						checked = unary(ValueType::I128, ValueType::I128);
						u32 source_opcode = 0;
						PackedShiftKind kind{};
						const auto input = source_input_state.find(node.source_pc);
						if (checked &&
							(!source_opcode_at(node.source_pc, &source_opcode) ||
							 !DecodePackedShiftMmi(source_opcode, &kind) ||
							 node.immediate != static_cast<u32>(kind) ||
							 node.immediate >= static_cast<u32>(PackedShiftKind::Count) ||
							 input == source_input_state.end() ||
							 node.operands[0] != input->second.gpr[RT(source_opcode)] ||
							 node.literal != (static_cast<u32>(SA(source_opcode)) &
								(kind <= PackedShiftKind::RightArithmetic16 ? 0x0fu : 0x1fu))))
						{
							checked = Fail(VerifyFailure::SourceMismatch, block_index,
								node_index,
								"packed shift does not match its decoded lane semantics");
						}
						break;
					}
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
						u32 source_opcode = 0;
						MemoryAccessKind kind{};
						const bool have_source =
							source_opcode_at(node.source_pc, &source_opcode);
						const bool vector_memory = have_source &&
							DecodeMemoryAccess(source_opcode, &kind) &&
							IsVu0MemoryAccess(kind);
						const Vu0FmacOp fmac = have_source ?
							DecodeVu0Fmac(source_opcode) : Vu0FmacOp{};
						const Vu0FdivOp fdiv = have_source ?
							DecodeVu0Fdiv(source_opcode) : Vu0FdivOp{};
						const Vu0UnaryOp unary = have_source ?
							DecodeVu0Unary(source_opcode) : Vu0UnaryOp{};
						const Vu0VectorTransferOp vector_transfer = have_source ?
							DecodeVu0VectorTransfer(source_opcode) :
							Vu0VectorTransferOp{};
						const Vu0ControlReadOp control_read = have_source ?
							DecodeVu0ControlRead(source_opcode) : Vu0ControlReadOp{};
						const Vu0ControlWriteOp control_write = have_source ?
							DecodeVu0ControlWrite(source_opcode) : Vu0ControlWriteOp{};
						NoEffectKind no_effect{};
						const bool vu0_no_effect = have_source &&
							DecodeNoEffect(source_opcode, program.options, &no_effect) &&
							NoEffectRequiresVu0Idle(no_effect);
						const bool transfer_gpr = vector_transfer.valid &&
							vector_transfer.kind == Vu0VectorTransferKind::ToVu0 &&
							RD(source_opcode) != 0;
						const ValueType guarded_type =
							(control_read.valid || control_write.valid) ?
							ValueType::I32 : (transfer_gpr ?
								ValueType::I128 : ValueType::VuF32x4Bits);
						checked = binary(ValueType::I32, guarded_type, guarded_type);
						if (!checked)
							break;
						const u32 source_vf = vu0_no_effect ? 0u :
							(vector_memory ? RT(source_opcode) :
							 (fdiv.valid && fdiv.kind == Vu0FdivKind::SquareRoot ?
								RT(source_opcode) : RD(source_opcode)));
						ValueId expected_guarded = transfer_gpr ?
							expected.gpr[RT(source_opcode)] : expected.vu0_vf[source_vf];
						if (control_read.valid)
						{
							if (RT(source_opcode) == 0 || control_read.source == 29)
								expected_guarded = expected.vu0_vi[29];
							else if (control_read.source == 16)
								expected_guarded = expected.vu0_vi[16];
							else if (control_read.source == 17)
								expected_guarded = expected.vu0_vi[17];
							else if (control_read.source == 0)
							{
								const Node* zero = local_node(node.operands[1]);
								expected_guarded = node.operands[1];
								if (!zero || zero->opcode != Opcode::ConstantI32 ||
									zero->type != ValueType::I32 || zero->operand_count != 0 ||
									zero->literal != 0 || zero->source_pc != node.source_pc)
								{
									checked = Fail(VerifyFailure::SourceMismatch, block_index,
										node_index,
										"CFC2 VI0 guard does not consume an exact zero");
									break;
								}
							}
						}
						else if (control_write.valid)
						{
							const Node* source_low = local_node(node.operands[1]);
							const auto input = source_input_state.find(node.source_pc);
							if (!source_low || input == source_input_state.end() ||
								source_low->opcode != Opcode::ExtractLow32 ||
								source_low->type != ValueType::I32 ||
								source_low->operand_count != 1 ||
								source_low->operands[0] != input->second.gpr[RT(source_opcode)] ||
								source_low->immediate != 0 || source_low->literal != 0 ||
								source_low->source_pc != node.source_pc)
							{
								checked = Fail(VerifyFailure::SourceMismatch, block_index,
									node_index,
									"CTC2 idle guard does not consume the decoded low GPR word");
								break;
							}
							expected_guarded = node.operands[1];
						}
						if (!source_opcode_at(node.source_pc, &source_opcode) ||
							(!vector_memory && !fmac.valid && !fdiv.valid && !unary.valid &&
								!vu0_no_effect && !vector_transfer.valid &&
								!control_read.valid && !control_write.valid) ||
							node.immediate != 0 ||
							node.literal != 0 ||
							node.operands[0] != expected.vu0_vi[29] ||
							node.operands[1] != expected_guarded ||
							!vu0_idle_guard_value.emplace(node.source_pc, node.id).second)
						{
							checked = Fail(VerifyFailure::SourceMismatch, block_index,
								node_index,
								"VU0 idle guard does not consume the decoded live state exactly once");
							break;
						}

						size_t observer_index = block.observer_exits.size();
						for (size_t index = 0; index < block.observer_exits.size(); index++)
						{
							if (block.observer_exits[index].operation != node.id)
								continue;
							if (observer_index != block.observer_exits.size())
							{
								checked = Fail(VerifyFailure::ExitContractMismatch,
									block_index, node_index,
									"VU0 idle observer has duplicate fallback transfers");
								break;
							}
							observer_index = index;
						}
						if (!checked)
							break;
						const auto source = std::find_if(block.source.begin(),
							block.source.end(), [&](const SourceInstruction& candidate) {
								return candidate.pc == node.source_pc;
							});
						const bool delay_slot = source != block.source.end() &&
							source->delay_slot;
						const auto input = source_input_state.find(node.source_pc);
						if (observer_index == block.observer_exits.size() ||
							observer_exit_seen[observer_index] || source == block.source.end() ||
							(delay_slot && (!has_delayed_control ||
								!captured_control_input)) ||
							(!delay_slot && input == source_input_state.end()))
						{
							checked = Fail(VerifyFailure::ExitContractMismatch,
								block_index, node_index,
								"VU0 idle observer lacks one exact restartable fallback");
							break;
						}
						const StateMap& fallback_state =
							delay_slot ? control_input : input->second;
						const u32 resume_pc = delay_slot ?
							block.terminator.branch_pc : node.source_pc;
						const auto prefix = raw_cycles_before_source.find(resume_pc);
						const Transfer& transfer =
							block.observer_exits[observer_index].transfer;
						const Node* transfer_pc = local_node(transfer.pc);
						if (prefix == raw_cycles_before_source.end() ||
							!StateMapsEqual(transfer.state, fallback_state) ||
							transfer.target_block != INVALID_BLOCK ||
							transfer.external_reason != ExitReason::HelperObserver ||
							!transfer.cycle_commit_deferred ||
							transfer.pending_raw_cycles != prefix->second ||
							transfer.event_horizon_check || !transfer_pc ||
							transfer_pc->opcode != Opcode::ConstantAddress ||
							transfer_pc->type != ValueType::Address ||
							transfer_pc->operand_count != 0 ||
							transfer_pc->source_pc != node.source_pc ||
							static_cast<u32>(transfer_pc->literal) != resume_pc ||
							defining_node[transfer.pc] >= node_index)
						{
							checked = Fail(VerifyFailure::ExitContractMismatch,
								block_index, node_index,
								"VU0 idle observer loses pre-instruction state, PC, or cycle debt");
							break;
						}
						observer_exit_seen[observer_index] = true;
						vu0_idle_guard_count[node.source_pc]++;
						break;
					}
					case Opcode::Vu0ControlWrite:
					{
						checked = binary(ValueType::I32, ValueType::I32,
							ValueType::I32);
						u32 source_opcode = 0;
						const bool have_source =
							source_opcode_at(node.source_pc, &source_opcode);
						const Vu0ControlWriteOp write = have_source ?
							DecodeVu0ControlWrite(source_opcode) : Vu0ControlWriteOp{};
						const auto guard = vu0_idle_guard_value.find(node.source_pc);
						if (checked && (!write.valid || node.immediate != write.target ||
							node.literal != 0 ||
							node.operands[0] != expected.vu0_vi[write.target] ||
							guard == vu0_idle_guard_value.end() ||
							node.operands[1] != guard->second ||
							!vu0_control_write_result.emplace(node.source_pc,
								node.id).second))
						{
							checked = Fail(VerifyFailure::SourceMismatch, block_index,
								node_index,
								"CTC2 result loses its decoded target, old word, or guarded source");
						}
						break;
					}
					case Opcode::Vu0DenormalizeStatus:
					{
						checked = unary(ValueType::I32, ValueType::I32);
						u32 source_opcode = 0;
						const auto write = vu0_control_write_result.find(node.source_pc);
						if (checked &&
							(!source_opcode_at(node.source_pc, &source_opcode) ||
							 DecodeVu0ControlWrite(source_opcode).target != VU0_STATUS_FLAG ||
							 write == vu0_control_write_result.end() ||
							 node.operands[0] != write->second || node.immediate != 0 ||
							 node.literal != 0 ||
							 !vu0_denormalized_status_result.emplace(node.source_pc,
								node.id).second))
						{
							checked = Fail(VerifyFailure::SourceMismatch, block_index,
								node_index,
								"CTC2 STATUS denormalization does not consume its exact result");
						}
						break;
					}
					case Opcode::Vu0ConvertFixed:
					{
						checked = unary(ValueType::VuF32x4Bits,
							ValueType::VuF32x4Bits);
						u32 source_opcode = 0;
						const bool have_source =
							source_opcode_at(node.source_pc, &source_opcode);
						const Vu0UnaryOp decoded = have_source ?
							DecodeVu0Unary(source_opcode) : Vu0UnaryOp{};
						const auto guard = vu0_idle_guard_value.find(node.source_pc);
						if (checked && (!decoded.valid ||
							decoded.kind != Vu0UnaryKind::ConvertFixed ||
							guard == vu0_idle_guard_value.end() ||
							node.operands[0] != guard->second ||
							node.immediate != decoded.offset || node.literal != 0))
						{
							checked = Fail(VerifyFailure::SourceMismatch, block_index,
								node_index,
								"VU0 fixed conversion does not match its decoded source");
						}
						break;
					}
					case Opcode::Vu0ConvertIntegerToFloat:
					{
						checked = unary(ValueType::VuF32x4Bits,
							ValueType::VuF32x4Bits);
						u32 source_opcode = 0;
						const bool have_source =
							source_opcode_at(node.source_pc, &source_opcode);
						const Vu0UnaryOp decoded = have_source ?
							DecodeVu0Unary(source_opcode) : Vu0UnaryOp{};
						const auto guard = vu0_idle_guard_value.find(node.source_pc);
						if (checked && (!decoded.valid ||
							decoded.kind != Vu0UnaryKind::ConvertIntegerToFloat ||
							guard == vu0_idle_guard_value.end() ||
							node.operands[0] != guard->second ||
							node.immediate != decoded.offset || node.literal != 0))
						{
							checked = Fail(VerifyFailure::SourceMismatch, block_index,
								node_index,
								"VU0 integer-to-float conversion does not match its decoded source");
						}
						break;
					}
					case Opcode::Vu0Rotate32:
					{
						checked = unary(ValueType::VuF32x4Bits,
							ValueType::VuF32x4Bits);
						u32 source_opcode = 0;
						const bool have_source =
							source_opcode_at(node.source_pc, &source_opcode);
						const Vu0UnaryOp decoded = have_source ?
							DecodeVu0Unary(source_opcode) : Vu0UnaryOp{};
						const auto guard = vu0_idle_guard_value.find(node.source_pc);
						if (checked && (!decoded.valid ||
							decoded.kind != Vu0UnaryKind::Rotate32 ||
							guard == vu0_idle_guard_value.end() ||
							node.operands[0] != guard->second ||
							node.immediate != 0 || node.literal != 0))
						{
							checked = Fail(VerifyFailure::SourceMismatch, block_index,
								node_index,
								"VU0 MR32 does not consume its exact guarded source");
						}
						break;
					}
					case Opcode::Vu0NormalizeVector:
					{
						checked = unary(ValueType::VuF32x4Bits,
							ValueType::VuF32x4Bits);
						u32 source_opcode = 0;
						if (checked &&
							(!source_opcode_at(node.source_pc, &source_opcode) ||
							 !DecodeVu0Fmac(source_opcode).valid ||
							 node.immediate != 0 || node.literal != 0))
						{
							checked = Fail(VerifyFailure::SourceMismatch, block_index,
								node_index,
								"VU0 normalization has no FMAC owner");
						}
						break;
					}
					case Opcode::Vu0BroadcastLane:
					{
						checked = unary(ValueType::VuF32x4Bits,
							ValueType::VuF32x4Bits);
						u32 source_opcode = 0;
						const bool have_source =
							source_opcode_at(node.source_pc, &source_opcode);
						const Vu0FmacOp fmac = have_source ?
							DecodeVu0Fmac(source_opcode) : Vu0FmacOp{};
						if (checked && (!fmac.valid ||
							fmac.operand != Vu0FmacOperand::BroadcastLane ||
							node.immediate != fmac.lane ||
							node.literal != 0))
						{
							checked = Fail(VerifyFailure::SourceMismatch, block_index,
								node_index,
								"VU0 broadcast lane does not match its FMAC source");
						}
						break;
					}
					case Opcode::Vu0BroadcastScalar:
					{
						checked = unary(ValueType::I32, ValueType::VuF32x4Bits);
						u32 source_opcode = 0;
						const bool have_source =
							source_opcode_at(node.source_pc, &source_opcode);
						const Vu0FmacOp fmac = have_source ?
							DecodeVu0Fmac(source_opcode) : Vu0FmacOp{};
						if (checked && (!fmac.valid ||
							fmac.operand != Vu0FmacOperand::ScalarQ ||
							node.operands[0] != expected.vu0_vi[22] ||
							node.immediate != 0 || node.literal != 0))
						{
							checked = Fail(VerifyFailure::SourceMismatch, block_index,
								node_index,
								"VU0 Q broadcast does not consume the current VI[Q]");
						}
						break;
					}
					case Opcode::Vu0FdivQ:
					case Opcode::Vu0FdivFlags:
					{
						checked = binary(ValueType::VuF32x4Bits,
							ValueType::VuF32x4Bits, ValueType::I32);
						u32 source_opcode = 0;
						const auto input = source_input_state.find(node.source_pc);
						if (checked &&
							(!source_opcode_at(node.source_pc, &source_opcode) ||
							 input == source_input_state.end() ||
							 !exact_vu0_fdiv_node(node.id, node.opcode,
								source_opcode, node.source_pc, input->second)))
						{
							checked = Fail(VerifyFailure::SourceMismatch, block_index,
								node_index,
								"VU0 FDIV result/flags do not match decoded lanes");
							break;
						}
						const bool unique = node.opcode == Opcode::Vu0FdivQ ?
							vu0_fdiv_q_result.emplace(node.source_pc, node.id).second :
							vu0_fdiv_flags_result.emplace(node.source_pc, node.id).second;
						if (!unique)
						{
							checked = Fail(VerifyFailure::SourceMismatch, block_index,
								node_index, "VU0 FDIV has duplicate result/flags nodes");
						}
						break;
					}
					case Opcode::Vu0UpdateFdivStatus:
					{
						checked = binary(ValueType::I32, ValueType::I32,
							ValueType::I32);
						u32 source_opcode = 0;
						const auto flags = vu0_fdiv_flags_result.find(node.source_pc);
						if (checked &&
							(!source_opcode_at(node.source_pc, &source_opcode) ||
							 !DecodeVu0Fdiv(source_opcode).valid ||
							 flags == vu0_fdiv_flags_result.end() ||
							 node.operands[0] != expected.vu0_statusflag ||
							 node.operands[1] != flags->second ||
							 node.immediate != 0 || node.literal != 0))
						{
							checked = Fail(VerifyFailure::SourceMismatch, block_index,
								node_index,
								"VU0 FDIV status update loses old or current D/I flags");
						}
						if (checked &&
							!vu0_fdiv_status_result.emplace(node.source_pc, node.id).second)
						{
							checked = Fail(VerifyFailure::SourceMismatch, block_index,
								node_index, "VU0 FDIV has duplicate status updates");
						}
						break;
					}
					case Opcode::Vu0SyncFdivStatusControl:
					{
						checked = binary(ValueType::I32, ValueType::I32,
							ValueType::I32);
						u32 source_opcode = 0;
						const auto status = vu0_fdiv_status_result.find(node.source_pc);
						if (checked &&
							(!source_opcode_at(node.source_pc, &source_opcode) ||
							 !DecodeVu0Fdiv(source_opcode).valid ||
							 node.operands[0] != expected.vu0_vi[16] ||
							 status == vu0_fdiv_status_result.end() ||
							 node.operands[1] != status->second ||
							 node.immediate != 0 || node.literal != 0))
						{
							checked = Fail(VerifyFailure::SourceMismatch, block_index,
								node_index,
								"VU0 FDIV VI status mirror loses current/sticky D/I bits");
						}
						if (checked &&
							!vu0_fdiv_vi_status_result.emplace(node.source_pc, node.id).second)
						{
							checked = Fail(VerifyFailure::SourceMismatch, block_index,
								node_index, "VU0 FDIV has duplicate VI status updates");
						}
						break;
					}
					case Opcode::Vu0MulRaw:
					case Opcode::Vu0AddRaw:
					case Opcode::Vu0SubRaw:
					{
						checked = binary(ValueType::VuF32x4Bits,
							ValueType::VuF32x4Bits, ValueType::VuF32x4Bits);
						u32 source_opcode = 0;
						const bool have_source =
							source_opcode_at(node.source_pc, &source_opcode);
						const Vu0FmacOp fmac = have_source ?
							DecodeVu0Fmac(source_opcode) : Vu0FmacOp{};
						bool operation_allowed = false;
						if (fmac.valid)
						{
							switch (node.opcode)
							{
								case Opcode::Vu0MulRaw:
									operation_allowed =
										fmac.kind == Vu0FmacKind::Multiply ||
										fmac.kind == Vu0FmacKind::MultiplyAdd ||
										fmac.kind == Vu0FmacKind::MultiplySubtract;
									break;
								case Opcode::Vu0AddRaw:
									operation_allowed = fmac.kind == Vu0FmacKind::Add ||
										fmac.kind == Vu0FmacKind::MultiplyAdd;
									break;
								case Opcode::Vu0SubRaw:
									operation_allowed = fmac.kind == Vu0FmacKind::Subtract ||
										fmac.kind == Vu0FmacKind::MultiplySubtract;
									break;
								default:
									break;
							}
						}
						if (checked && (!fmac.valid || node.immediate != 0 ||
							node.literal != 0 || !operation_allowed))
						{
							checked = Fail(VerifyFailure::SourceMismatch, block_index,
								node_index,
								"VU0 raw FMAC operation does not match its source");
						}
						break;
					}
					case Opcode::Vu0ClampFmacResult:
					case Opcode::Vu0MacFlagsFromRaw:
					{
						checked = unary(ValueType::VuF32x4Bits,
							node.opcode == Opcode::Vu0ClampFmacResult ?
								ValueType::VuF32x4Bits : ValueType::I32);
						u32 source_opcode = 0;
						if (checked &&
							(!source_opcode_at(node.source_pc, &source_opcode) ||
							 !DecodeVu0Fmac(source_opcode).valid ||
							 node.immediate != (RS(source_opcode) & 0x0fu) ||
							 node.literal != 0))
						{
							checked = Fail(VerifyFailure::SourceMismatch, block_index,
								node_index,
								"VU0 result/flag mask does not match its FMAC source");
						}
						break;
					}
					case Opcode::Vu0StatusFlagsFromMac:
					{
						checked = unary(ValueType::I32, ValueType::I32);
						u32 source_opcode = 0;
						if (checked &&
							(!source_opcode_at(node.source_pc, &source_opcode) ||
							 !DecodeVu0Fmac(source_opcode).valid ||
							 node.immediate != 0 || node.literal != 0))
						{
							checked = Fail(VerifyFailure::SourceMismatch, block_index,
								node_index,
								"VU0 status reduction has no FMAC source");
						}
						break;
					}
					case Opcode::Vu0MergeMasked:
					{
						checked = binary(ValueType::VuF32x4Bits,
							ValueType::VuF32x4Bits, ValueType::VuF32x4Bits);
						u32 source_opcode = 0;
						if (checked &&
							(!source_opcode_at(node.source_pc, &source_opcode) ||
							 (!DecodeVu0Fmac(source_opcode).valid &&
							  !DecodeVu0Unary(source_opcode).valid) ||
							 node.immediate != (RS(source_opcode) & 0x0fu) ||
							 node.immediate == 0 || node.literal != 0))
						{
							checked = Fail(VerifyFailure::SourceMismatch, block_index,
								node_index,
								"VU0 masked merge does not match its decoded source");
						}
						break;
					}
					case Opcode::Vu0SyncStatusControl:
					{
						checked = binary(ValueType::I32, ValueType::I32,
							ValueType::I32);
						u32 source_opcode = 0;
						if (checked &&
							(!source_opcode_at(node.source_pc, &source_opcode) ||
							 !DecodeVu0Fmac(source_opcode).valid ||
							 node.immediate != 0 || node.literal != 0))
						{
							checked = Fail(VerifyFailure::SourceMismatch, block_index,
								node_index,
								"VU0 status mirror has no FMAC source");
						}
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
							fpr_access ? ValueType::F32Bits :
								(vu0_access ? ValueType::VuF32x4Bits :
								              ValueType::I128));
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

						size_t memory_exit_index = block.memory_exits.size();
						for (size_t index = 0; index < block.memory_exits.size(); index++)
						{
							if (block.memory_exits[index].operation != node.id)
								continue;
							if (memory_exit_index != block.memory_exits.size())
							{
								checked = Fail(VerifyFailure::ExitContractMismatch,
									block_index, node_index,
									"memory operation has duplicate fallback transfers");
								break;
							}
							memory_exit_index = index;
						}
						if (!checked)
							break;
						const auto source = std::find_if(block.source.begin(),
							block.source.end(), [&](const SourceInstruction& candidate) {
								return candidate.pc == node.source_pc;
							});
						const bool delay_slot = source != block.source.end() &&
							source->delay_slot;
						const auto input = source_input_state.find(node.source_pc);
						if (memory_exit_index == block.memory_exits.size() ||
							memory_exit_seen[memory_exit_index] ||
							source == block.source.end() ||
							(delay_slot && (!has_delayed_control ||
								!captured_control_input)) ||
							(!delay_slot && input == source_input_state.end()))
						{
							checked = Fail(VerifyFailure::ExitContractMismatch,
								block_index, node_index,
								"memory operation lacks one exact restartable fallback");
							break;
						}
						const StateMap& fallback_state =
							delay_slot ? control_input : input->second;
						const u32 resume_pc = delay_slot ?
							block.terminator.branch_pc : node.source_pc;
						const auto prefix = raw_cycles_before_source.find(resume_pc);
						const Transfer& transfer =
							block.memory_exits[memory_exit_index].transfer;
						const Node* transfer_pc = local_node(transfer.pc);
						if (prefix == raw_cycles_before_source.end() ||
							!StateMapsEqual(transfer.state, fallback_state) ||
							transfer.target_block != INVALID_BLOCK ||
							transfer.external_reason != ExitReason::MemoryObserver ||
							!transfer.cycle_commit_deferred ||
							transfer.pending_raw_cycles != prefix->second ||
							transfer.event_horizon_check || !transfer_pc ||
							transfer_pc->opcode != Opcode::ConstantAddress ||
							transfer_pc->type != ValueType::Address ||
							transfer_pc->operand_count != 0 ||
							transfer_pc->source_pc != node.source_pc ||
							static_cast<u32>(transfer_pc->literal) != resume_pc ||
							defining_node[transfer.pc] >= node_index)
						{
							checked = Fail(VerifyFailure::ExitContractMismatch,
								block_index, node_index,
								"memory fallback loses pre-access state, PC, or cycle debt");
							break;
						}
						memory_exit_seen[memory_exit_index] = true;
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
								ValueType::F32Bits :
								(IsVu0MemoryAccess(kind) ? ValueType::VuF32x4Bits :
								                            ValueType::I128)))
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
							PackedBinaryKind packed_mmi_kind{};
							const bool packed_mmi =
								DecodePackedBinaryMmi(source_opcode, &packed_mmi_kind);
							PackedShiftKind packed_shift_kind{};
							const bool packed_shift =
								DecodePackedShiftMmi(source_opcode, &packed_shift_kind);
							const IntegerMultiplyOp integer_multiply =
								DecodeIntegerMultiply(source_opcode);
							const Vu0VectorTransferOp vu0_transfer =
								DecodeVu0VectorTransfer(source_opcode);
							const Vu0ControlReadOp vu0_control_read =
								DecodeVu0ControlRead(source_opcode);
							PureCop1StateKind pure_cop1_kind{};
							const bool pure_cop1 =
								DecodePureCop1State(source_opcode, &pure_cop1_kind);
							if (IsGuardedExceptionInstruction(source_opcode))
							{
								const auto input =
									source_input_state.find(node.source_pc);
								if (input == source_input_state.end() ||
									!exact_addi_gpr_bind(node, source_opcode,
										input->second))
								{
									checked = Fail(VerifyFailure::SourceMismatch,
										block_index, node_index,
										"ADDI GPR binding is not the guarded exact sum");
									break;
								}
								addi_gpr_bind_count[node.source_pc]++;
							}
							else if (integer_multiply.valid)
							{
								const auto input = source_input_state.find(node.source_pc);
								if (input == source_input_state.end() ||
									!exact_integer_multiply_gpr_bind(node,
										source_opcode, input->second))
								{
									checked = Fail(VerifyFailure::SourceMismatch,
										block_index, node_index,
										"integer multiply GPR result is not its exact LO lane");
									break;
								}
								integer_multiply_gpr_bind_count[node.source_pc]++;
							}
							else if (vu0_control_read.valid)
							{
								const auto guard =
									vu0_idle_guard_value.find(node.source_pc);
								const Node* replace = local_node(node.operands[0]);
								const Node* extension = replace && replace->operand_count == 2 ?
									local_node(replace->operands[1]) : nullptr;
								if (RT(source_opcode) == 0 ||
									node.immediate != RT(source_opcode) ||
									guard == vu0_idle_guard_value.end() || !replace ||
									replace->opcode != Opcode::ReplaceLow64 ||
									replace->type != ValueType::I128 ||
									replace->operand_count != 2 ||
									replace->operands[0] != expected.gpr[RT(source_opcode)] ||
									replace->source_pc != node.source_pc || !extension ||
									extension->opcode != Opcode::SignExtend32To64 ||
									extension->type != ValueType::I64 ||
									extension->operand_count != 1 ||
									extension->operands[0] != guard->second ||
									extension->source_pc != node.source_pc)
								{
									checked = Fail(VerifyFailure::SourceMismatch,
										block_index, node_index,
										"CFC2 GPR binding does not sign-extend its guarded control word");
									break;
								}
								vu0_control_read_gpr_bind_count[node.source_pc]++;
							}
							else if (vu0_transfer.valid &&
								vu0_transfer.kind == Vu0VectorTransferKind::FromVu0)
							{
								const Node* value = local_node(node.operands[0]);
								const auto guard = vu0_idle_guard_value.find(node.source_pc);
								if (node.immediate != RT(source_opcode) || !value ||
									value->opcode != Opcode::BitcastVuF32x4BitsToI128 ||
									value->operand_count != 1 ||
									guard == vu0_idle_guard_value.end() ||
									value->operands[0] != guard->second)
								{
									checked = Fail(VerifyFailure::SourceMismatch,
										block_index, node_index,
										"QMFC2 GPR binding does not consume its guarded VF source");
									break;
								}
								vu0_transfer_gpr_bind_count[node.source_pc]++;
							}
							else if (IsExtendedScalarGprWrite(source_opcode))
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
							else if (packed_mmi)
							{
								if (!exact_packed_mmi_gpr_bind(node, source_opcode,
										expected))
								{
									checked = Fail(VerifyFailure::SourceMismatch,
										block_index, node_index,
										"packed MMI GPR binding does not match source");
									break;
								}
								pure_mmi_gpr_bind_count[node.source_pc]++;
							}
							else if (packed_shift)
							{
								const auto input = source_input_state.find(node.source_pc);
								if (input == source_input_state.end() ||
									!exact_packed_shift_gpr_bind(node, source_opcode,
										input->second))
								{
									checked = Fail(VerifyFailure::SourceMismatch,
										block_index, node_index,
										"packed shift GPR binding is not the decoded result");
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
								integer_multiply.valid || pure_mmi || packed_mmi ||
								pure_cop1 || vu0_control_read.valid || vu0_transfer.valid ||
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
							const auto input = source_input_state.find(node.source_pc);
							const bool have_source =
								source_opcode_at(node.source_pc, &source_opcode);
							const IntegerMultiplyOp multiply = have_source ?
								DecodeIntegerMultiply(source_opcode) : IntegerMultiplyOp{};
							const bool exact = multiply.valid ?
								(input != source_input_state.end() &&
								 exact_integer_multiply_hilo_bind(node, source_opcode,
									 input->second, true)) :
								(have_source &&
								 exact_hilo_bind(node, source_opcode, expected, true));
							if (!exact)
							{
								checked = Fail(VerifyFailure::SourceMismatch,
									block_index, node_index,
									"HI binding does not match decoded owner");
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
							const auto input = source_input_state.find(node.source_pc);
							const bool have_source =
								source_opcode_at(node.source_pc, &source_opcode);
							const IntegerMultiplyOp multiply = have_source ?
								DecodeIntegerMultiply(source_opcode) : IntegerMultiplyOp{};
							const bool exact = multiply.valid ?
								(input != source_input_state.end() &&
								 exact_integer_multiply_hilo_bind(node, source_opcode,
									 input->second, false)) :
								(have_source &&
								 exact_hilo_bind(node, source_opcode, expected, false));
							if (!exact)
							{
								checked = Fail(VerifyFailure::SourceMismatch,
									block_index, node_index,
									"LO binding does not match decoded owner");
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
							Cop1UnaryWordKind unary_word_kind{};
							const bool unary_word =
								DecodeCop1UnaryWord(source_opcode, &unary_word_kind);
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
							else if (unary_word)
							{
								if (node.immediate != FD(source_opcode) ||
									!exact_unary(node.operands[0],
										Cop1UnaryWordOpcode(unary_word_kind),
										expected.fpr[FS(source_opcode)], node.source_pc))
								{
									checked = Fail(VerifyFailure::SourceMismatch,
										block_index, node_index,
										"COP1 unary result does not bind decoded fd");
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
						checked = unary(ValueType::VuF32x4Bits, ValueType::Void);
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
							if (!source_opcode_at(node.source_pc, &source_opcode))
							{
								checked = Fail(VerifyFailure::SourceMismatch,
									block_index, node_index,
									"VU0 VF binding has no owning source");
								break;
							}
							const bool vector_load =
								DecodeMemoryAccess(source_opcode, &kind) &&
								kind == MemoryAccessKind::LoadVu0Vector;
							const Vu0FmacOp fmac = DecodeVu0Fmac(source_opcode);
							const Vu0UnaryOp unary = DecodeVu0Unary(source_opcode);
							const Vu0VectorTransferOp vector_transfer =
								DecodeVu0VectorTransfer(source_opcode);
							if (vector_load)
							{
								if (node.immediate != RT(source_opcode) || !value ||
									value->opcode != Opcode::MemoryLoadValue ||
									value->source_pc != node.source_pc)
								{
									checked = Fail(VerifyFailure::SourceMismatch,
										block_index, node_index,
										"loaded VU0 VF binding does not match decoded ft");
									break;
								}
								memory_bind_count[node.source_pc]++;
							}
							else if (vector_transfer.valid &&
								vector_transfer.kind == Vu0VectorTransferKind::ToVu0)
							{
								const auto guard = vu0_idle_guard_value.find(node.source_pc);
								if (node.immediate != RD(source_opcode) || !value ||
									value->opcode != Opcode::BitcastI128ToVuF32x4Bits ||
									value->operand_count != 1 ||
									guard == vu0_idle_guard_value.end() ||
									value->operands[0] != guard->second)
								{
									checked = Fail(VerifyFailure::SourceMismatch,
										block_index, node_index,
										"QMTC2 VF binding does not consume its guarded GPR source");
									break;
								}
							}
							else if (fmac.valid)
							{
								const auto raw = vu0_fmac_raw_result.find(node.source_pc);
								const u32 mask = RS(source_opcode) & 0x0fu;
								if (fmac.accumulator_destination ||
									mask == 0 || node.immediate != SA(source_opcode) ||
									raw == vu0_fmac_raw_result.end() || !value ||
									value->opcode != Opcode::Vu0MergeMasked ||
									value->operand_count != 2 || value->immediate != mask ||
									value->operands[0] != expected.vu0_vf[node.immediate] ||
									!exact_vu0_fmac_clamp(value->operands[1], raw->second,
										source_opcode, node.source_pc))
								{
									checked = Fail(VerifyFailure::SourceMismatch,
										block_index, node_index,
										"VU0 FMAC result does not bind decoded fd exactly");
									break;
								}
							}
							else
							{
								const u32 mask = RS(source_opcode) & 0x0fu;
								if (!unary.valid || mask == 0 ||
									node.immediate != RT(source_opcode) || !value ||
									value->opcode != Opcode::Vu0MergeMasked ||
									value->operand_count != 2 ||
									value->immediate != mask ||
									value->operands[0] != expected.vu0_vf[node.immediate] ||
									!exact_vu0_unary_result(value->operands[1],
										source_opcode, node.source_pc))
								{
									checked = Fail(VerifyFailure::SourceMismatch,
										block_index, node_index,
										"VU0 unary result does not bind decoded ft exactly");
									break;
								}
							}
							vu0_vf_bind_count[node.source_pc]++;
							expected.vu0_vf[node.immediate] = node.operands[0];
						}
						break;
					case Opcode::BindVu0Vi:
					{
						checked = unary(ValueType::I32, ValueType::Void);
						u32 source_opcode = 0;
						const auto value = vu0_control_write_result.find(node.source_pc);
						const bool have_source =
							source_opcode_at(node.source_pc, &source_opcode);
						const Vu0ControlWriteOp write = have_source ?
							DecodeVu0ControlWrite(source_opcode) : Vu0ControlWriteOp{};
						if (checked && (!write.valid || node.immediate == 0 ||
							node.immediate >= VU0_VI_COUNT ||
							node.immediate != write.target ||
							write.target == VU0_MAC_FLAG || write.target == VU0_TPC ||
							write.target == VU0_VPU_STAT ||
							value == vu0_control_write_result.end() ||
							node.operands[0] != value->second))
						{
							checked = Fail(VerifyFailure::SourceMismatch, block_index,
								node_index,
								"CTC2 VI binding does not publish its exact decoded result");
							break;
						}
						vu0_vi_bind_count[node.source_pc]++;
						expected.vu0_vi[node.immediate] = node.operands[0];
						break;
					}
					case Opcode::BindVu0ClipFlag:
					{
						checked = unary(ValueType::I32, ValueType::Void);
						u32 source_opcode = 0;
						const auto value = vu0_control_write_result.find(node.source_pc);
						if (checked &&
							(!source_opcode_at(node.source_pc, &source_opcode) ||
							 DecodeVu0ControlWrite(source_opcode).target != VU0_CLIP_FLAG ||
							 node.immediate != 0 ||
							 value == vu0_control_write_result.end() ||
							 node.operands[0] != value->second))
						{
							checked = Fail(VerifyFailure::SourceMismatch, block_index,
								node_index,
								"CTC2 CLIP scalar mirror differs from its VI result");
							break;
						}
						vu0_clipflag_bind_count[node.source_pc]++;
						expected.vu0_clipflag = node.operands[0];
						break;
					}
					case Opcode::BindVu0MicroStatusFlag:
					{
						checked = unary(ValueType::I32, ValueType::Void);
						u32 source_opcode = 0;
						const auto value =
							vu0_denormalized_status_result.find(node.source_pc);
						if (checked &&
							(!source_opcode_at(node.source_pc, &source_opcode) ||
							 DecodeVu0ControlWrite(source_opcode).target != VU0_STATUS_FLAG ||
							 node.immediate >= 4 ||
							 value == vu0_denormalized_status_result.end() ||
							 node.operands[0] != value->second))
						{
							checked = Fail(VerifyFailure::SourceMismatch, block_index,
								node_index,
								"CTC2 STATUS hidden mirror is missing or not denormalized");
							break;
						}
						vu0_micro_status_bind_count[node.source_pc]++;
						expected.vu0_micro_statusflags[node.immediate] =
							node.operands[0];
						break;
					}
					case Opcode::BindVu0MacFlag:
					{
						checked = unary(ValueType::I32, ValueType::Void);
						u32 source_opcode = 0;
						const Node* value = local_node(node.operands[0]);
						if (checked &&
							(!source_opcode_at(node.source_pc, &source_opcode) ||
							 !DecodeVu0Fmac(source_opcode).valid ||
							 node.immediate != 0 || !value ||
							 value->opcode != Opcode::Vu0MacFlagsFromRaw ||
							 value->operand_count != 1 ||
							 value->immediate != (RS(source_opcode) & 0x0fu) ||
							 !exact_vu0_fmac_raw(value->operands[0], source_opcode,
								node.source_pc, expected) ||
							 !vu0_fmac_raw_result.emplace(node.source_pc,
								value->operands[0]).second))
						{
							checked = Fail(VerifyFailure::SourceMismatch, block_index,
								node_index,
								"VU0 MAC binding does not share the exact FMAC raw result");
							break;
						}
						vu0_macflag_bind_count[node.source_pc]++;
						expected.vu0_macflag = node.operands[0];
						break;
					}
					case Opcode::BindVu0Q:
					{
						checked = unary(ValueType::I32, ValueType::Void);
						u32 source_opcode = 0;
						const auto q = vu0_fdiv_q_result.find(node.source_pc);
						if (checked &&
							(!source_opcode_at(node.source_pc, &source_opcode) ||
							 !DecodeVu0Fdiv(source_opcode).valid ||
							 node.immediate != 0 ||
							 q == vu0_fdiv_q_result.end() ||
							 node.operands[0] != q->second))
						{
							checked = Fail(VerifyFailure::SourceMismatch, block_index,
								node_index,
								"VU0 Q binding does not publish the exact FDIV result");
							break;
						}
						vu0_q_bind_count[node.source_pc]++;
						expected.vu0_q = node.operands[0];
						break;
					}
					case Opcode::BindVu0ViQ:
					{
						checked = unary(ValueType::I32, ValueType::Void);
						u32 source_opcode = 0;
						if (checked &&
							(!source_opcode_at(node.source_pc, &source_opcode) ||
							 !DecodeVu0Fdiv(source_opcode).valid ||
							 node.immediate != 0 ||
							 node.operands[0] != expected.vu0_q))
						{
							checked = Fail(VerifyFailure::SourceMismatch, block_index,
								node_index,
								"VU0 VI[Q] binding differs from the current Q result");
							break;
						}
						vu0_vi_q_bind_count[node.source_pc]++;
						expected.vu0_vi[22] = node.operands[0];
						break;
					}
					case Opcode::BindVu0StatusFlag:
					{
						checked = unary(ValueType::I32, ValueType::Void);
						u32 source_opcode = 0;
						const Node* value = local_node(node.operands[0]);
						const bool have_source =
							source_opcode_at(node.source_pc, &source_opcode);
						const bool fmac = have_source &&
							DecodeVu0Fmac(source_opcode).valid;
						const bool fdiv = have_source &&
							DecodeVu0Fdiv(source_opcode).valid;
						const auto flags = vu0_fdiv_flags_result.find(node.source_pc);
						const auto fdiv_status =
							vu0_fdiv_status_result.find(node.source_pc);
						const bool exact_fmac = fmac && value &&
							value->opcode == Opcode::Vu0StatusFlagsFromMac &&
							value->operand_count == 1 &&
							value->operands[0] == expected.vu0_macflag;
						const bool exact_fdiv = fdiv && value &&
							fdiv_status != vu0_fdiv_status_result.end() &&
							node.operands[0] == fdiv_status->second &&
							value->opcode == Opcode::Vu0UpdateFdivStatus &&
							value->operand_count == 2 &&
							value->operands[0] == expected.vu0_statusflag &&
							flags != vu0_fdiv_flags_result.end() &&
							value->operands[1] == flags->second;
						if (checked && (node.immediate != 0 ||
							(!exact_fmac && !exact_fdiv)))
						{
							checked = Fail(VerifyFailure::SourceMismatch, block_index,
								node_index,
								"VU0 status binding does not match FMAC or FDIV flags");
							break;
						}
						vu0_statusflag_bind_count[node.source_pc]++;
						expected.vu0_statusflag = node.operands[0];
						break;
					}
					case Opcode::BindVu0ViMac:
					{
						checked = unary(ValueType::I32, ValueType::Void);
						u32 source_opcode = 0;
						if (checked &&
							(!source_opcode_at(node.source_pc, &source_opcode) ||
							 !DecodeVu0Fmac(source_opcode).valid ||
							 node.immediate != 0 ||
							 node.operands[0] != expected.vu0_macflag))
						{
							checked = Fail(VerifyFailure::SourceMismatch, block_index,
								node_index,
								"VU0 VI MAC mirror does not publish the current MAC flags");
							break;
						}
						vu0_vi_mac_bind_count[node.source_pc]++;
						expected.vu0_vi[17] = node.operands[0];
						break;
					}
					case Opcode::BindVu0ViStatus:
					{
						checked = unary(ValueType::I32, ValueType::Void);
						u32 source_opcode = 0;
						const Node* value = local_node(node.operands[0]);
						const bool have_source =
							source_opcode_at(node.source_pc, &source_opcode);
						const bool fmac = have_source &&
							DecodeVu0Fmac(source_opcode).valid;
						const bool fdiv = have_source &&
							DecodeVu0Fdiv(source_opcode).valid;
						const auto fdiv_vi_status =
							vu0_fdiv_vi_status_result.find(node.source_pc);
						const bool exact_fmac = fmac && value &&
							value->opcode == Opcode::Vu0SyncStatusControl &&
							value->operand_count == 2 &&
							value->operands[0] == expected.vu0_vi[16] &&
							value->operands[1] == expected.vu0_statusflag;
						const bool exact_fdiv = fdiv && value &&
							fdiv_vi_status != vu0_fdiv_vi_status_result.end() &&
							node.operands[0] == fdiv_vi_status->second &&
							value->opcode == Opcode::Vu0SyncFdivStatusControl &&
							value->operand_count == 2 &&
							value->operands[0] == expected.vu0_vi[16] &&
							value->operands[1] == expected.vu0_statusflag;
						if (checked && (node.immediate != 0 ||
							(!exact_fmac && !exact_fdiv)))
						{
							checked = Fail(VerifyFailure::SourceMismatch, block_index,
								node_index,
								"VU0 VI status mirror loses FMAC/FDIV state");
							break;
						}
						vu0_vi_status_bind_count[node.source_pc]++;
						expected.vu0_vi[16] = node.operands[0];
						break;
					}
					case Opcode::BindVu0Acc:
					{
						checked = unary(ValueType::VuF32x4Bits, ValueType::Void);
						u32 source_opcode = 0;
						const Node* value = local_node(node.operands[0]);
						const bool have_source =
							source_opcode_at(node.source_pc, &source_opcode);
						const Vu0FmacOp fmac = have_source ?
							DecodeVu0Fmac(source_opcode) : Vu0FmacOp{};
						const auto raw = vu0_fmac_raw_result.find(node.source_pc);
						const u32 mask = RS(source_opcode) & 0x0fu;
						if (checked &&
							(!fmac.valid ||
							 !fmac.accumulator_destination ||
							 mask == 0 || node.immediate != 0 ||
							 raw == vu0_fmac_raw_result.end() || !value ||
							 value->opcode != Opcode::Vu0MergeMasked ||
							 value->operand_count != 2 || value->immediate != mask ||
							 value->operands[0] != expected.vu0_acc ||
							 !exact_vu0_fmac_clamp(value->operands[1], raw->second,
								source_opcode, node.source_pc)))
						{
							checked = Fail(VerifyFailure::SourceMismatch, block_index,
								node_index,
								"VU0 FMAC accumulator binding is not an exact masked result");
							break;
						}
						vu0_acc_bind_count[node.source_pc]++;
						expected.vu0_acc = node.operands[0];
						break;
					}
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
							else if (DecodeCop1UnaryWord(source_opcode, nullptr))
							{
								if (value->opcode != Opcode::Cop1ClearOuFlags ||
									value->operand_count != 1 ||
									value->source_pc != node.source_pc ||
									value->operands[0] != expected.fcr31 ||
									value->immediate != 0 || value->literal != 0)
								{
									checked = Fail(VerifyFailure::SourceMismatch,
										block_index, node_index,
										"COP1 unary FCR31 binding does not clear exact O/U causes");
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
					case Opcode::ExitIfTrue:
					{
						checked = unary(ValueType::I1, ValueType::Void);
						const auto addi_condition =
							addi_overflow_condition.find(node.source_pc);
						const auto source = std::find_if(block.source.begin(),
							block.source.end(), [&](const SourceInstruction& candidate) {
								return candidate.pc == node.source_pc;
							});
						const bool addi_guard =
							addi_condition != addi_overflow_condition.end() &&
							addi_condition->second == node.operands[0];
						if (checked &&
							(!addi_guard ||
							 source == block.source.end() ||
							 !IsGuardedExceptionInstruction(source->opcode) ||
							 node.immediate >= block.guarded_exits.size() ||
							 guarded_exit_seen[node.immediate]))
						{
							checked = Fail(VerifyFailure::ExitContractMismatch,
								block_index, node_index,
								"conditional exit is not the unique guard for its decoded instruction");
							break;
						}

						const bool delay_slot = source->delay_slot;
						const auto input = source_input_state.find(node.source_pc);
						if ((delay_slot && (!has_delayed_control ||
								!captured_control_input || likely_branch)) ||
							(!delay_slot && input == source_input_state.end()))
						{
							checked = Fail(VerifyFailure::ExitContractMismatch,
								block_index, node_index,
								"guarded instruction has no exact restartable source state");
							break;
						}
						const StateMap& exceptional_state =
							delay_slot ? control_input : input->second;
						const u32 resume_pc = delay_slot ?
							block.terminator.branch_pc : node.source_pc;
						const auto prefix = raw_cycles_before_source.find(resume_pc);
						const Transfer& transfer = block.guarded_exits[node.immediate];
						const Node* transfer_pc = local_node(transfer.pc);
						if (prefix == raw_cycles_before_source.end() ||
							!StateMapsEqual(transfer.state, exceptional_state) ||
							transfer.target_block != INVALID_BLOCK ||
							transfer.external_reason != ExitReason::ExceptionObserver ||
							!transfer.cycle_commit_deferred ||
							transfer.pending_raw_cycles != prefix->second ||
							transfer.event_horizon_check || !transfer_pc ||
							transfer_pc->opcode != Opcode::ConstantAddress ||
							transfer_pc->type != ValueType::Address ||
							transfer_pc->operand_count != 0 ||
							transfer_pc->source_pc != node.source_pc ||
							static_cast<u32>(transfer_pc->literal) != resume_pc ||
							defining_node[transfer.pc] >= node_index)
						{
							checked = Fail(VerifyFailure::ExitContractMismatch,
								block_index, node_index,
								"guarded instruction loses its pre-instruction/pair state, PC, or cycle debt");
							break;
						}
						guarded_exit_seen[node.immediate] = true;
						addi_guard_count[node.source_pc]++;
						break;
					}
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
			if (std::find(guarded_exit_seen.begin(), guarded_exit_seen.end(), false) !=
				guarded_exit_seen.end())
			{
				return Fail(VerifyFailure::ExitContractMismatch, block_index,
					UINT32_MAX,
					"guarded transfer has no unique executable condition node");
			}
			if (std::find(memory_exit_seen.begin(), memory_exit_seen.end(), false) !=
				memory_exit_seen.end())
			{
				return Fail(VerifyFailure::ExitContractMismatch, block_index,
					UINT32_MAX,
					"memory fallback transfer has no unique memory operation");
			}
			if (std::find(observer_exit_seen.begin(), observer_exit_seen.end(), false) !=
				observer_exit_seen.end())
			{
				return Fail(VerifyFailure::ExitContractMismatch, block_index,
					UINT32_MAX,
					"observer fallback transfer has no unique synchronization node");
			}
			for (const SourceInstruction& source : block.source)
			{
				const Vu0ControlReadOp vu0_control_read =
					DecodeVu0ControlRead(source.opcode);
				const Vu0ControlWriteOp vu0_control_write =
					DecodeVu0ControlWrite(source.opcode);
				const Vu0VectorTransferOp vu0_transfer =
					DecodeVu0VectorTransfer(source.opcode);
				const Vu0FdivOp vu0_fdiv = DecodeVu0Fdiv(source.opcode);
				const Vu0FmacOp vu0_fmac = DecodeVu0Fmac(source.opcode);
				const Vu0UnaryOp vu0_unary = DecodeVu0Unary(source.opcode);
				NoEffectKind no_effect_kind{};
				if (IsGuardedExceptionInstruction(source.opcode))
				{
					const u32 expected_gpr = RT(source.opcode) != 0 ? 1u : 0u;
					if (addi_overflow_count[source.pc] != 1 ||
						addi_guard_count[source.pc] != 1 ||
						addi_gpr_bind_count[source.pc] != expected_gpr ||
						memory_operation_count[source.pc] != 0 ||
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
							"ADDI lacks one exact overflow guard and normal result binding");
					}
				}
				else if (vu0_control_read.valid)
				{
					const u32 expected_gpr = RT(source.opcode) != 0 ? 1u : 0u;
					if (vu0_idle_guard_count[source.pc] != 1 ||
						vu0_control_read_gpr_bind_count[source.pc] != expected_gpr ||
						vu0_transfer_gpr_bind_count[source.pc] != 0 ||
						vu0_vf_bind_count[source.pc] != 0 ||
						vu0_acc_bind_count[source.pc] != 0 ||
						vu0_macflag_bind_count[source.pc] != 0 ||
						vu0_statusflag_bind_count[source.pc] != 0 ||
						vu0_vi_mac_bind_count[source.pc] != 0 ||
						vu0_vi_status_bind_count[source.pc] != 0 ||
						memory_operation_count[source.pc] != 0 ||
						no_effect_count[source.pc] != 0 ||
						extended_gpr_bind_count[source.pc] != 0 ||
						integer_multiply_gpr_bind_count[source.pc] != 0 ||
						pure_mmi_gpr_bind_count[source.pc] != 0 ||
						pure_cop1_gpr_bind_count[source.pc] != 0 ||
						fpr_bind_count[source.pc] != 0 ||
						fcr31_bind_count[source.pc] != 0 ||
						acc_bind_count[source.pc] != 0 ||
						hi_bind_count[source.pc] != 0 ||
						lo_bind_count[source.pc] != 0 ||
						sa_bind_count[source.pc] != 0)
					{
						return Fail(VerifyFailure::SourceMismatch, block_index,
							UINT32_MAX,
							"CFC2 lacks its exact idle guard or control-word destination");
					}
				}
				else if (vu0_control_write.valid)
				{
					const bool read_only = vu0_control_write.target == 0 ||
						vu0_control_write.target == VU0_MAC_FLAG ||
						vu0_control_write.target == VU0_TPC ||
						vu0_control_write.target == VU0_VPU_STAT;
					const u32 expected_vi = read_only ? 0u : 1u;
					const u32 expected_clip =
						vu0_control_write.target == VU0_CLIP_FLAG ? 1u : 0u;
					const u32 expected_micro_status =
						vu0_control_write.target == VU0_STATUS_FLAG ? 4u : 0u;
					const bool have_status_conversion =
						vu0_denormalized_status_result.contains(source.pc);
					if (vu0_idle_guard_count[source.pc] != 1 ||
						!vu0_control_write_result.contains(source.pc) ||
						vu0_vi_bind_count[source.pc] != expected_vi ||
						vu0_clipflag_bind_count[source.pc] != expected_clip ||
						vu0_micro_status_bind_count[source.pc] != expected_micro_status ||
						have_status_conversion != (expected_micro_status != 0) ||
						vu0_control_read_gpr_bind_count[source.pc] != 0 ||
						vu0_transfer_gpr_bind_count[source.pc] != 0 ||
						vu0_vf_bind_count[source.pc] != 0 ||
						vu0_acc_bind_count[source.pc] != 0 ||
						vu0_macflag_bind_count[source.pc] != 0 ||
						vu0_statusflag_bind_count[source.pc] != 0 ||
						vu0_vi_mac_bind_count[source.pc] != 0 ||
						vu0_vi_status_bind_count[source.pc] != 0 ||
						vu0_q_bind_count[source.pc] != 0 ||
						vu0_vi_q_bind_count[source.pc] != 0 ||
						memory_operation_count[source.pc] != 0 ||
						no_effect_count[source.pc] != 0 ||
						extended_gpr_bind_count[source.pc] != 0 ||
						pure_mmi_gpr_bind_count[source.pc] != 0 ||
						pure_cop1_gpr_bind_count[source.pc] != 0 ||
						fpr_bind_count[source.pc] != 0 ||
						fcr31_bind_count[source.pc] != 0 || acc_bind_count[source.pc] != 0 ||
						hi_bind_count[source.pc] != 0 || lo_bind_count[source.pc] != 0 ||
						sa_bind_count[source.pc] != 0)
					{
						return Fail(VerifyFailure::SourceMismatch, block_index,
							UINT32_MAX,
							"CTC2 lacks its exact guard, target write, or hidden mirrors");
					}
				}
				else if (vu0_transfer.valid)
				{
					const bool from_vu0 = vu0_transfer.kind ==
						Vu0VectorTransferKind::FromVu0;
					const u32 expected_gpr = from_vu0 && RT(source.opcode) != 0 ? 1u : 0u;
					const u32 expected_vf = !from_vu0 && RD(source.opcode) != 0 ? 1u : 0u;
					if (vu0_idle_guard_count[source.pc] != 1 ||
						vu0_transfer_gpr_bind_count[source.pc] != expected_gpr ||
						vu0_vf_bind_count[source.pc] != expected_vf ||
						vu0_acc_bind_count[source.pc] != 0 ||
						vu0_macflag_bind_count[source.pc] != 0 ||
						vu0_statusflag_bind_count[source.pc] != 0 ||
						vu0_vi_mac_bind_count[source.pc] != 0 ||
						vu0_vi_status_bind_count[source.pc] != 0 ||
						memory_operation_count[source.pc] != 0 ||
						no_effect_count[source.pc] != 0 ||
						extended_gpr_bind_count[source.pc] != 0 ||
						integer_multiply_gpr_bind_count[source.pc] != 0 ||
						pure_mmi_gpr_bind_count[source.pc] != 0 ||
						pure_cop1_gpr_bind_count[source.pc] != 0 ||
						fpr_bind_count[source.pc] != 0 ||
						fcr31_bind_count[source.pc] != 0 ||
						acc_bind_count[source.pc] != 0 ||
						hi_bind_count[source.pc] != 0 ||
						lo_bind_count[source.pc] != 0 ||
						sa_bind_count[source.pc] != 0)
					{
						return Fail(VerifyFailure::SourceMismatch, block_index,
							UINT32_MAX,
							"VU0 vector transfer lacks its exact guard or destination");
					}
				}
				else if (vu0_fdiv.valid)
				{
					if (vu0_idle_guard_count[source.pc] != 1 ||
						vu0_q_bind_count[source.pc] != 1 ||
						vu0_vi_q_bind_count[source.pc] != 1 ||
						vu0_statusflag_bind_count[source.pc] != 1 ||
						vu0_vi_status_bind_count[source.pc] != 1 ||
						!vu0_fdiv_q_result.contains(source.pc) ||
						!vu0_fdiv_flags_result.contains(source.pc) ||
						!vu0_fdiv_status_result.contains(source.pc) ||
						!vu0_fdiv_vi_status_result.contains(source.pc) ||
						vu0_vf_bind_count[source.pc] != 0 ||
						vu0_acc_bind_count[source.pc] != 0 ||
						vu0_macflag_bind_count[source.pc] != 0 ||
						vu0_vi_mac_bind_count[source.pc] != 0 ||
						memory_operation_count[source.pc] != 0 ||
						extended_gpr_bind_count[source.pc] != 0 ||
						pure_mmi_gpr_bind_count[source.pc] != 0 ||
						pure_cop1_gpr_bind_count[source.pc] != 0 ||
						fpr_bind_count[source.pc] != 0 ||
						fcr31_bind_count[source.pc] != 0 ||
						acc_bind_count[source.pc] != 0 ||
						hi_bind_count[source.pc] != 0 ||
						lo_bind_count[source.pc] != 0 ||
						sa_bind_count[source.pc] != 0)
					{
						return Fail(VerifyFailure::SourceMismatch, block_index,
							UINT32_MAX,
							"VU0 FDIV lacks its exact guard, Q mirrors, or D/I flags");
					}
				}
				else if (vu0_fmac.valid)
				{
					const u32 mask = RS(source.opcode) & 0x0fu;
					const bool vector_destination =
						!vu0_fmac.accumulator_destination;
					const u32 expected_vf =
						vector_destination && mask != 0 && SA(source.opcode) != 0 ?
							1u : 0u;
					const u32 expected_acc =
						!vector_destination && mask != 0 ? 1u : 0u;
					if (vu0_idle_guard_count[source.pc] != 1 ||
						vu0_macflag_bind_count[source.pc] != 1 ||
						vu0_statusflag_bind_count[source.pc] != 1 ||
						vu0_vi_mac_bind_count[source.pc] != 1 ||
						vu0_vi_status_bind_count[source.pc] != 1 ||
						vu0_vf_bind_count[source.pc] != expected_vf ||
						vu0_acc_bind_count[source.pc] != expected_acc ||
						!vu0_fmac_raw_result.contains(source.pc) ||
						memory_operation_count[source.pc] != 0 ||
						extended_gpr_bind_count[source.pc] != 0 ||
						pure_mmi_gpr_bind_count[source.pc] != 0 ||
						pure_cop1_gpr_bind_count[source.pc] != 0 ||
						fpr_bind_count[source.pc] != 0 ||
						fcr31_bind_count[source.pc] != 0 ||
						acc_bind_count[source.pc] != 0 ||
						hi_bind_count[source.pc] != 0 ||
						lo_bind_count[source.pc] != 0 ||
						sa_bind_count[source.pc] != 0)
					{
						return Fail(VerifyFailure::SourceMismatch, block_index,
							UINT32_MAX,
							"VU0 FMAC lacks its exact guard, flags, or destination");
					}
				}
				else if (vu0_unary.valid)
				{
					const u32 mask = RS(source.opcode) & 0x0fu;
					const u32 expected_vf =
						mask != 0 && RT(source.opcode) != 0 ? 1u : 0u;
					if (vu0_idle_guard_count[source.pc] != 1 ||
						vu0_vf_bind_count[source.pc] != expected_vf ||
						vu0_acc_bind_count[source.pc] != 0 ||
						vu0_macflag_bind_count[source.pc] != 0 ||
						vu0_statusflag_bind_count[source.pc] != 0 ||
						vu0_vi_mac_bind_count[source.pc] != 0 ||
						vu0_vi_status_bind_count[source.pc] != 0 ||
						memory_operation_count[source.pc] != 0 ||
						extended_gpr_bind_count[source.pc] != 0 ||
						pure_mmi_gpr_bind_count[source.pc] != 0 ||
						pure_cop1_gpr_bind_count[source.pc] != 0 ||
						fpr_bind_count[source.pc] != 0 ||
						fcr31_bind_count[source.pc] != 0 ||
						acc_bind_count[source.pc] != 0 ||
						hi_bind_count[source.pc] != 0 ||
						lo_bind_count[source.pc] != 0 ||
						sa_bind_count[source.pc] != 0)
					{
						return Fail(VerifyFailure::SourceMismatch, block_index,
							UINT32_MAX,
							"VU0 unary source lacks its exact guard or destination");
					}
				}
				else if (DecodeNoEffect(source.opcode, program.options, &no_effect_kind))
				{
					const u32 expected_idle =
						NoEffectRequiresVu0Idle(no_effect_kind) ? 1u : 0u;
					if (no_effect_count[source.pc] != 1 ||
						vu0_idle_guard_count[source.pc] != expected_idle ||
						vu0_vf_bind_count[source.pc] != 0 ||
						vu0_acc_bind_count[source.pc] != 0 ||
						vu0_macflag_bind_count[source.pc] != 0 ||
						vu0_statusflag_bind_count[source.pc] != 0 ||
						vu0_vi_mac_bind_count[source.pc] != 0 ||
						vu0_vi_status_bind_count[source.pc] != 0 ||
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
				else if (const IntegerMultiplyOp multiply =
						DecodeIntegerMultiply(source.opcode); multiply.valid)
				{
					const u32 expected_gpr = RD(source.opcode) != 0 ? 1u : 0u;
					if (integer_multiply_gpr_bind_count[source.pc] != expected_gpr ||
						hi_bind_count[source.pc] != 1 ||
						lo_bind_count[source.pc] != 1 ||
						!integer_multiply_result.contains(source.pc) ||
						memory_operation_count[source.pc] != 0 ||
						extended_gpr_bind_count[source.pc] != 0 ||
						pure_mmi_gpr_bind_count[source.pc] != 0 ||
						pure_cop1_gpr_bind_count[source.pc] != 0 ||
						fpr_bind_count[source.pc] != 0 ||
						fcr31_bind_count[source.pc] != 0 ||
						acc_bind_count[source.pc] != 0 ||
						sa_bind_count[source.pc] != 0)
					{
						return Fail(VerifyFailure::SourceMismatch, block_index,
							UINT32_MAX,
							"integer multiply lacks exact shared GPR/HI/LO publication");
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
					const u32 expected_guard =
						program.options.cop1_lazy_ou_guards ? 1u : 0u;
					if (cop1_exception_count[source.pc] != expected_guard ||
						fcr31_bind_count[source.pc] != 1 ||
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
							"basic COP1 arithmetic lacks its exact guard, result, or FCR31 binding");
					}
				}
				else if (DecodeCompoundCop1Arithmetic(source.opcode, nullptr))
				{
					const u32 expected_guard =
						program.options.cop1_lazy_ou_guards ? 1u : 0u;
					if (cop1_exception_count[source.pc] != expected_guard ||
						fcr31_bind_count[source.pc] != 1 ||
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
							"MADD.S/MSUB.S lacks its exact guard, FPR, or FCR31 binding");
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
				else if (DecodeCop1UnaryWord(source.opcode, nullptr))
				{
					if (fpr_bind_count[source.pc] != 1 ||
						fcr31_bind_count[source.pc] != 1 ||
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
							"COP1 unary source lacks exact FPR and FCR31 bindings");
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
					else if (DecodePackedBinaryMmi(source.opcode, nullptr))
					{
						const u32 expected_gpr = RD(source.opcode) != 0 ? 1u : 0u;
						if (pure_mmi_gpr_bind_count[source.pc] != expected_gpr ||
							hi_bind_count[source.pc] != 0 ||
							lo_bind_count[source.pc] != 0 ||
							extended_gpr_bind_count[source.pc] != 0 ||
							pure_cop1_gpr_bind_count[source.pc] != 0 ||
							fpr_bind_count[source.pc] != 0 ||
							sa_bind_count[source.pc] != 0)
						{
							return Fail(VerifyFailure::SourceMismatch, block_index,
								UINT32_MAX,
								"packed MMI source has the wrong architectural binding");
						}
					}
					else if (DecodePackedShiftMmi(source.opcode, nullptr))
					{
						const u32 expected_gpr = RD(source.opcode) != 0 ? 1u : 0u;
						if (pure_mmi_gpr_bind_count[source.pc] != expected_gpr ||
							hi_bind_count[source.pc] != 0 || lo_bind_count[source.pc] != 0 ||
							extended_gpr_bind_count[source.pc] != 0 ||
							pure_cop1_gpr_bind_count[source.pc] != 0 ||
							fpr_bind_count[source.pc] != 0 || sa_bind_count[source.pc] != 0)
						{
							return Fail(VerifyFailure::SourceMismatch, block_index,
								UINT32_MAX,
								"packed shift source has the wrong architectural binding");
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
				bool expected_event_horizon_check,
				const DirectCallContract* internal_return = nullptr) -> VerifyResult {
				if (block.terminator.kind != TerminatorKind::RegisterJump &&
					(transfer.register_target_proven ||
					 transfer.proven_register_target_pc != 0))
				{
					return Fail(VerifyFailure::ControlFlowMismatch, block_index,
						UINT32_MAX,
						"non-register edge carries a forged register-target proof");
				}
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
						(internal_return ?
							(transfer.target_block == INVALID_BLOCK ||
							 !transfer.register_target_proven ||
							 transfer.proven_register_target_pc !=
								internal_return->return_pc ||
							 transfer.target_block >= program.blocks.size() ||
							 program.blocks[transfer.target_block].pc !=
								internal_return->return_pc ||
							 expected_state.gpr[31] != block.parameters.gpr[31]) :
							(transfer.target_block != INVALID_BLOCK ||
							 transfer.register_target_proven ||
							 transfer.proven_register_target_pc != 0)) ||
						transfer.external_reason != ExitReason::RegionBoundary ||
						pc_node.opcode != Opcode::AddressFromI32 ||
						pc_node.source_pc != block.terminator.branch_pc ||
						pc_node.operand_count != 1)
					{
						return Fail(VerifyFailure::ControlFlowMismatch, block_index,
							UINT32_MAX,
							"register jump target lacks its exact pre-delay or internal-return proof");
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
						program.source_spans, static_pc, program.options);
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
			if (block.terminator.kind == TerminatorKind::RegisterJump)
			{
				for (const Transfer& target : block.terminator.register_targets)
				{
					const auto direct_return =
						direct_return_for_pc(target.proven_register_target_pc);
					if (direct_return == program.direct_calls.end())
					{
						return Fail(VerifyFailure::DirectCallContract, block_index,
							UINT32_MAX,
							"register return target has no attested call site");
					}
					transfer_check = verify_transfer(target, primary_expected,
						deferred_observer, deferred_observer ? raw_cycles : 0,
						expected_event_horizon_check, &*direct_return);
					if (!transfer_check)
						return transfer_check;
				}
			}
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
				bool cop1_branch_on_true = false;
				const bool cop1_branch =
					DecodeCop1Branch(branch_opcode, &cop1_branch_on_true);
				const u32 encoded_primary = branch_opcode >> 26;
				const u32 primary =
					encoded_primary >= 0x14 && encoded_primary <= 0x17 ?
						encoded_primary - 0x10 :
						encoded_primary;
				Opcode expected_condition = Opcode::CompareEqual64;
				bool predicate_matches = false;
				if (cop1_branch)
				{
					predicate_matches =
						condition.opcode == Opcode::Cop1BranchCondition &&
						condition.operand_count == 1 &&
						condition.operands[0] == control_input.fcr31 &&
						condition.immediate == (cop1_branch_on_true ? 1u : 0u) &&
						condition.literal == 0;
				}
				else switch (primary)
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
			for (const Transfer& target : block.terminator.register_targets)
			{
				if (target.target_block != INVALID_BLOCK)
					pending.push_back(target.target_block);
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
					{ValueType::VuF32x4Bits, state.vu0_vf[vf]};
			values[block.parameters.vu0_acc] =
				{ValueType::VuF32x4Bits, state.vu0_acc};
			values[block.parameters.vu0_macflag] =
				{ValueType::I32, Bits(state.vu0_macflag)};
			values[block.parameters.vu0_statusflag] =
				{ValueType::I32, Bits(state.vu0_statusflag)};
			values[block.parameters.vu0_clipflag] =
				{ValueType::I32, Bits(state.vu0_clipflag)};
			values[block.parameters.vu0_q] =
				{ValueType::I32, Bits(state.vu0_q)};
			for (u32 vi = 0; vi < VU0_VI_COUNT; vi++)
				values[block.parameters.vu0_vi[vi]] =
					{ValueType::I32, Bits(state.vu0_vi[vi])};
			for (u32 instance = 0; instance < 4; instance++)
			{
				values[block.parameters.vu0_micro_macflags[instance]] =
					{ValueType::I32, Bits(state.vu0_micro_macflags[instance])};
				values[block.parameters.vu0_micro_clipflags[instance]] =
					{ValueType::I32, Bits(state.vu0_micro_clipflags[instance])};
				values[block.parameters.vu0_micro_statusflags[instance]] =
					{ValueType::I32, Bits(state.vu0_micro_statusflags[instance])};
			}
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
			state.vu0_acc = values[transfer.state.vu0_acc].bits;
			state.vu0_macflag = static_cast<u32>(
				values[transfer.state.vu0_macflag].bits.lo);
			state.vu0_statusflag = static_cast<u32>(
				values[transfer.state.vu0_statusflag].bits.lo);
			state.vu0_clipflag = static_cast<u32>(
				values[transfer.state.vu0_clipflag].bits.lo);
			state.vu0_q = static_cast<u32>(
				values[transfer.state.vu0_q].bits.lo);
			for (u32 vi = 0; vi < VU0_VI_COUNT; vi++)
				state.vu0_vi[vi] = static_cast<u32>(
					values[transfer.state.vu0_vi[vi]].bits.lo);
			for (u32 instance = 0; instance < 4; instance++)
			{
				state.vu0_micro_macflags[instance] = static_cast<u32>(
					values[transfer.state.vu0_micro_macflags[instance]].bits.lo);
				state.vu0_micro_clipflags[instance] = static_cast<u32>(
					values[transfer.state.vu0_micro_clipflags[instance]].bits.lo);
				state.vu0_micro_statusflags[instance] = static_cast<u32>(
					values[transfer.state.vu0_micro_statusflags[instance]].bits.lo);
			}
			state.cycle = values[transfer.state.cycle].bits.lo;
			state.pc = static_cast<u32>(values[transfer.pc].bits.lo);
			return state;
		};

		u32 block_index = program.entry_block;
		assign_parameters(program.blocks[block_index], current,
			{ValueType::MemoryEffect, Bits(0)});
		auto exit_before_observer = [&](const Block& block, const Node& node,
									 ExitReason reason) {
			const auto found = std::find_if(block.observer_exits.begin(),
				block.observer_exits.end(), [&](const ObserverExit& exit) {
					return exit.operation == node.id;
				});
			if (found == block.observer_exits.end())
			{
				result.error = "synchronization observer lacks its verified fallback transfer";
				return;
			}
			const Transfer& transfer = found->transfer;
			current = materialize(transfer);
			for (const SourceInstruction& source : block.source)
			{
				if (source.pc == current.pc)
					break;
				result.source_instructions_executed++;
			}
			*output = current;
			result.completed = true;
			result.reason = reason;
			result.cycle_commit_deferred = transfer.cycle_commit_deferred;
			result.pending_raw_cycles = transfer.pending_raw_cycles;
		};
		auto exit_before_memory = [&](const Block& block, const Node& node,
									  u32 address, ExitReason reason) {
			const auto found = std::find_if(block.memory_exits.begin(),
				block.memory_exits.end(), [&](const MemoryExit& exit) {
					return exit.operation == node.id;
				});
			if (found == block.memory_exits.end())
			{
				result.error = "memory operation lacks its verified fallback transfer";
				return;
			}
			const Transfer& transfer = found->transfer;
			current = materialize(transfer);
			for (const SourceInstruction& source : block.source)
			{
				if (source.pc == current.pc)
					break;
				result.source_instructions_executed++;
			}
			*output = current;
			result.completed = true;
			result.reason = reason;
			result.cycle_commit_deferred = transfer.cycle_commit_deferred;
			result.pending_raw_cycles = transfer.pending_raw_cycles;
			result.memory_address = address;
		};
		for (;;)
		{
			if (result.blocks_executed >= options.max_block_executions)
			{
				// Preserve the last mechanically materialized edge state for bounded
				// differential diagnostics.  This is not a successful execution result,
				// but it identifies the exact looping PC/state without another hot-path
				// logging mechanism.
				*output = current;
				result.error = "Region IR execution exceeded its validation block budget";
				return result;
			}
			const Block& block = program.blocks[block_index];
			result.blocks_executed++;
			for (u32 node_index = PARAMETER_COUNT; node_index < block.nodes.size();
				 node_index++)
			{
				const Node& node = block.nodes[node_index];
				const bool annulled_likely_delay =
					block.terminator.kind == TerminatorKind::Branch &&
					block.terminator.likely && node.opcode != Opcode::Parameter &&
					node.source_pc == block.terminator.delay_slot_pc &&
					values[block.terminator.condition].bits.lo == 0;
				if (annulled_likely_delay)
					continue;
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
					case Opcode::BitcastI128ToVuF32x4Bits:
					case Opcode::BitcastVuF32x4BitsToI128:
						bits = values[node.operands[0]].bits;
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
					case Opcode::Cop1ExceptionalOuResult:
						bits = Bits(IsExceptionalBasicCop1Result(
							static_cast<u32>(left)) ? 1u : 0u);
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
					case Opcode::Cop1BranchCondition:
						bits = Bits((((static_cast<u32>(left) & FCR31_C) != 0) ==
							(node.immediate != 0)) ? 1u : 0u);
						break;
					case Opcode::Cop1AbsoluteWord:
						bits = Bits(static_cast<u32>(left) & 0x7fffffffu);
						break;
					case Opcode::Cop1NegateWord:
						bits = Bits(static_cast<u32>(left) ^ 0x80000000u);
						break;
					case Opcode::Cop1ClearOuFlags:
						bits = Bits(static_cast<u32>(left) & ~(FCR31_O | FCR31_U));
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
					case Opcode::MultiplySigned32:
						bits = Bits(static_cast<u64>(
							static_cast<s64>(std::bit_cast<s32>(static_cast<u32>(left))) *
							static_cast<s64>(std::bit_cast<s32>(static_cast<u32>(right)))));
						break;
					case Opcode::MultiplyUnsigned32:
						bits = Bits(static_cast<u64>(static_cast<u32>(left)) *
							static_cast<u64>(static_cast<u32>(right)));
						break;
					case Opcode::Truncate64To32:
						bits = Bits(static_cast<u32>(left));
						break;
					case Opcode::Add32:
						bits = Bits(static_cast<u32>(left) + static_cast<u32>(right));
						break;
					case Opcode::SignedAddOverflow32:
					{
						const u32 lhs = static_cast<u32>(left);
						const u32 rhs = static_cast<u32>(right);
						const u32 sum = lhs + rhs;
						bits = Bits(((~(lhs ^ rhs) & (lhs ^ sum)) >> 31) & 1u);
						break;
					}
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
					case Opcode::PackedBinary128:
						bits = EvaluatePackedBinary(
							static_cast<PackedBinaryKind>(node.immediate),
							values[node.operands[0]].bits,
							values[node.operands[1]].bits);
						break;
					case Opcode::PackedShift128:
					{
						const PackedShiftKind kind =
							static_cast<PackedShiftKind>(node.immediate);
						const u32 amount = static_cast<u32>(node.literal);
						const u128 source = values[node.operands[0]].bits;
						bits = {};
						if (kind <= PackedShiftKind::RightArithmetic16)
						{
							for (u32 lane = 0; lane < 8; lane++)
							{
								const u16 raw = PackedLane<u16>(source, lane);
								u16 value = raw;
								if (kind == PackedShiftKind::LeftLogical16)
									value = static_cast<u16>(raw << amount);
								else if (kind == PackedShiftKind::RightLogical16)
									value = static_cast<u16>(raw >> amount);
								else
									value = PackedShiftRightArithmetic(raw, amount);
								SetPackedLane(&bits, lane, value);
							}
						}
						else
						{
							for (u32 lane = 0; lane < 4; lane++)
							{
								const u32 raw = PackedLane<u32>(source, lane);
								u32 value = raw;
								if (kind == PackedShiftKind::LeftLogical32)
									value = raw << amount;
								else if (kind == PackedShiftKind::RightLogical32)
									value = raw >> amount;
								else
									value = PackedShiftRightArithmetic(raw, amount);
								SetPackedLane(&bits, lane, value);
							}
						}
						break;
					}
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
							exit_before_observer(block, node,
								ExitReason::HelperObserver);
							return result;
						}
						bits = values[node.operands[1]].bits;
						break;
					case Opcode::Vu0ConvertFixed:
						bits = ConvertVu0Fixed(values[node.operands[0]].bits,
							node.immediate);
						break;
					case Opcode::Vu0ConvertIntegerToFloat:
						bits = ConvertVu0IntegerToFloat(
							values[node.operands[0]].bits, node.immediate);
						break;
					case Opcode::Vu0Rotate32:
						bits = RotateVu0Words(values[node.operands[0]].bits);
						break;
					case Opcode::Vu0NormalizeVector:
						bits = NormalizeVuVector(values[node.operands[0]].bits,
							program.options.vu0_overflow_clamp);
						break;
					case Opcode::Vu0BroadcastLane:
						bits = BroadcastVuLane(values[node.operands[0]].bits,
							node.immediate);
						break;
					case Opcode::Vu0BroadcastScalar:
						bits = BroadcastVuScalar(static_cast<u32>(left),
							program.options.vu0_overflow_clamp);
						break;
					case Opcode::Vu0FdivQ:
					{
						const Vu0FdivOp fdiv = DecodeVu0FdivImmediate(node.immediate);
						bits = Bits(EvaluateVu0FdivQ(fdiv,
							values[node.operands[0]].bits,
							values[node.operands[1]].bits,
							program.options.vu0_overflow_clamp));
						break;
					}
					case Opcode::Vu0FdivFlags:
					{
						const Vu0FdivOp fdiv = DecodeVu0FdivImmediate(node.immediate);
						bits = Bits(EvaluateVu0FdivFlags(fdiv,
							values[node.operands[0]].bits,
							values[node.operands[1]].bits,
							program.options.vu0_overflow_clamp));
						break;
					}
					case Opcode::Vu0UpdateFdivStatus:
						bits = Bits((static_cast<u32>(left) & ~0x30u) |
							(static_cast<u32>(right) & 0x30u));
						break;
					case Opcode::Vu0SyncFdivStatusControl:
					{
						const u32 current = static_cast<u32>(right) & 0x30u;
						bits = Bits((static_cast<u32>(left) & 0x3cfu) |
							current | (current << 6));
						break;
					}
					case Opcode::Vu0MulRaw:
					case Opcode::Vu0AddRaw:
					case Opcode::Vu0SubRaw:
						bits = EvaluateVuRawBinary(node.opcode,
							values[node.operands[0]].bits,
							values[node.operands[1]].bits);
						break;
					case Opcode::Vu0ClampFmacResult:
						bits = ClampVuFmacResult(values[node.operands[0]].bits,
							node.immediate, program.options.vu0_overflow_clamp);
						break;
					case Opcode::Vu0MacFlagsFromRaw:
						bits = Bits(VuMacFlagsFromRaw(values[node.operands[0]].bits,
							node.immediate));
						break;
					case Opcode::Vu0StatusFlagsFromMac:
						bits = Bits(VuStatusFlagsFromMac(static_cast<u32>(left)));
						break;
					case Opcode::Vu0MergeMasked:
						bits = MergeVuMasked(values[node.operands[0]].bits,
							values[node.operands[1]].bits, node.immediate);
						break;
					case Opcode::Vu0SyncStatusControl:
						bits = Bits((static_cast<u32>(left) & 0x0fc0u) |
							static_cast<u32>(right) |
							(static_cast<u32>(right) << 6));
						break;
					case Opcode::Vu0ControlWrite:
					{
						const u32 target = node.immediate;
						const u32 old_value = static_cast<u32>(left);
						const u32 source = static_cast<u32>(right);
						u32 value = source;
						if (target == 0 || target == VU0_MAC_FLAG ||
							target == VU0_TPC || target == VU0_VPU_STAT)
						{
							value = old_value;
						}
						else if (target < VU0_STATUS_FLAG)
							value = (old_value & 0xffff0000u) | (source & 0xffffu);
						else if (target == VU0_STATUS_FLAG)
							value = (old_value & 0x3fu) | (source & 0x0fc0u);
						else if (target == VU0_R)
							value = (source & 0x007fffffu) | 0x3f800000u;
						bits = Bits(value);
						break;
					}
					case Opcode::Vu0DenormalizeStatus:
					{
						const u32 status = static_cast<u32>(left);
						bits = Bits(((status >> 3) & 0x18u) |
							((status << 11) & 0x1800u) |
							((status << 14) & 0x03cf0000u));
						break;
					}
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
					case Opcode::BindVu0Acc:
						current.vu0_acc = values[node.operands[0]].bits;
						break;
					case Opcode::BindVu0MacFlag:
						current.vu0_macflag = static_cast<u32>(left);
						break;
					case Opcode::BindVu0StatusFlag:
						current.vu0_statusflag = static_cast<u32>(left);
						break;
					case Opcode::BindVu0ViMac:
						current.vu0_vi[17] = static_cast<u32>(left);
						break;
					case Opcode::BindVu0ViStatus:
						current.vu0_vi[16] = static_cast<u32>(left);
						break;
					case Opcode::BindVu0Q:
						current.vu0_q = static_cast<u32>(left);
						break;
					case Opcode::BindVu0ViQ:
						current.vu0_vi[22] = static_cast<u32>(left);
						break;
					case Opcode::BindVu0Vi:
						current.vu0_vi[node.immediate] = static_cast<u32>(left);
						break;
					case Opcode::BindVu0ClipFlag:
						current.vu0_clipflag = static_cast<u32>(left);
						break;
					case Opcode::BindVu0MicroStatusFlag:
						current.vu0_micro_statusflags[node.immediate] =
							static_cast<u32>(left);
						break;
					case Opcode::BindFcr31:
						current.fcr31 =
							static_cast<u32>(values[node.operands[0]].bits.lo);
						break;
					case Opcode::BindAcc:
						current.acc =
							static_cast<u32>(values[node.operands[0]].bits.lo);
						break;
					case Opcode::ExitIfTrue:
						if (left != 0)
						{
							const Transfer& transfer =
								block.guarded_exits[node.immediate];
							current = materialize(transfer);
							for (const SourceInstruction& source : block.source)
							{
								if (source.pc == current.pc)
									break;
								result.source_instructions_executed++;
							}
							*output = current;
							result.completed = true;
							result.reason = transfer.external_reason;
							result.cycle_commit_deferred =
								transfer.cycle_commit_deferred;
							result.pending_raw_cycles = transfer.pending_raw_cycles;
							return result;
						}
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
			else if (block.terminator.kind == TerminatorKind::RegisterJump)
			{
				const u32 target_pc =
					static_cast<u32>(values[block.terminator.taken.pc].bits.lo);
				const auto internal = std::find_if(
					block.terminator.register_targets.begin(),
					block.terminator.register_targets.end(),
					[&](const Transfer& target) {
						return target.register_target_proven &&
							target.proven_register_target_pc == target_pc;
					});
				if (internal != block.terminator.register_targets.end())
					transfer = &*internal;
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
