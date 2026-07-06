// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "Config.h"
#include "MTVU.h"
#include "VUflags.h"
#include "VUmicro.h"

#if defined(ARCH_ARM32)
#include <arm_neon.h>
#endif

#include <cmath>
#include <cstring>

extern void _vuBackupVI(VURegs* VU, u32 reg);
extern void _vuXGKICKTransfer(s32 cycles, bool flush);
extern u32* GET_VU_MEM(VURegs* VU, u32 addr);

#if defined(VITASX2_QEMU_VALIDATION)
extern u32 g_qemuVuLowerNeonQwordOps;
extern u32 g_qemuVuUpperNeonQwordOps;
#endif

namespace VUInterpFast
{
	enum class LowerFastKind : u8
	{
		None,
		LQ,
		SQ,
		ILW,
		ISW,
		IADDIU,
		ISUBIU,
		FCAND,
		FCSET,
		FCEQ,
		FCOR,
		FSEQ,
		FSSET,
		FSAND,
		FSOR,
		FMEQ,
		FMAND,
		FMOR,
		FCGET,
		IADD,
		ISUB,
		IADDI,
		IAND,
		IOR,
		LQI,
		LQD,
		SQI,
		SQD,
		ILWR,
		ISWR,
		MOVE,
		MR32,
		MFIR,
		MTIR,
		RINIT,
		RGET,
		RNEXT,
		RXOR,
		IBEQ,
		IBNE,
		IBLTZ,
		IBGTZ,
		IBLEZ,
		IBGEZ,
		B,
		BAL,
		JR,
		JALR,
		MFP,
		WAITQ,
		WAITP,
		XITOP,
		XTOP,
		DIV,
		SQRT,
		RSQRT,
		XGKICK,
		ESADD,
		ERSADD,
		ELENG,
		ERLENG,
		EATANxy,
		EATANxz,
		ESUM,
		ERCPR,
		ESQRT,
		ERSQRT,
		ESIN,
		EATAN,
		EEXP,
	};

	enum class UpperFastKind : u8
	{
		None,
		NOP,
		ABS,
		FTOI0,
		FTOI4,
		FTOI12,
		FTOI15,
		ITOF0,
		ITOF4,
		ITOF12,
		ITOF15,
		ADD,
		ADDi,
		ADDq,
		ADDx,
		ADDy,
		ADDz,
		ADDw,
		ADDA,
		ADDAi,
		ADDAq,
		ADDAx,
		ADDAy,
		ADDAz,
		ADDAw,
		SUB,
		SUBi,
		SUBq,
		SUBx,
		SUBy,
		SUBz,
		SUBw,
		SUBA,
		SUBAi,
		SUBAq,
		SUBAx,
		SUBAy,
		SUBAz,
		SUBAw,
		MAX,
		MAXi,
		MAXx,
		MAXy,
		MAXz,
		MAXw,
		MINI,
		MINIi,
		MINIx,
		MINIy,
		MINIz,
		MINIw,
		CLIP,
		MUL,
		MULi,
		MULq,
		MULx,
		MULy,
		MULz,
		MULw,
		MULA,
		MULAi,
		MULAq,
		MULAx,
		MULAy,
		MULAz,
		MULAw,
		MADD,
		MADDi,
		MADDq,
		MADDx,
		MADDy,
		MADDz,
		MADDw,
		MADDA,
		MADDAi,
		MADDAq,
		MADDAx,
		MADDAy,
		MADDAz,
		MADDAw,
		MSUB,
		MSUBi,
		MSUBq,
		MSUBx,
		MSUBy,
		MSUBz,
		MSUBw,
		MSUBA,
		MSUBAi,
		MSUBAq,
		MSUBAx,
		MSUBAy,
		MSUBAz,
		MSUBAw,
		OPMULA,
		OPMSUB,
	};

	static constexpr unsigned Ft(u32 code) { return (code >> 16) & 0x1f; }
	static constexpr unsigned Fs(u32 code) { return (code >> 11) & 0x1f; }
	static constexpr unsigned Fd(u32 code) { return (code >> 6) & 0x1f; }
	static constexpr unsigned It(u32 code) { return Ft(code) & 0x0f; }
	static constexpr unsigned Is(u32 code) { return Fs(code) & 0x0f; }
	static constexpr unsigned Id(u32 code) { return Fd(code) & 0x0f; }
	static constexpr unsigned XYZW(u32 code) { return (code >> 21) & 0x0f; }
	static constexpr unsigned Fsf(u32 code) { return (code >> 21) & 0x03; }

	static constexpr u32 Vf0Flag(unsigned reg)
	{
		return reg == 0 ? (1u << REG_VF0_FLAG) : 0;
	}

	static constexpr s16 Imm5(u32 code)
	{
		const u16 imm = static_cast<u16>((code >> 6) & 0x1f);
		return static_cast<s16>((imm & 0x10) ? (0xfff0u | (imm & 0x0f)) : imm);
	}

	static constexpr s16 Imm11(u32 code)
	{
		return static_cast<s16>((code & 0x400) ? (0xfc00u | (code & 0x03ffu)) : (code & 0x03ffu));
	}

	static constexpr s32 Imm15(u32 code)
	{
		return static_cast<s32>(((code >> 10) & 0x7800u) | (code & 0x07ffu));
	}

	static constexpr u16 FlagImm12(u32 code)
	{
		return static_cast<u16>((((code >> 21) & 0x1u) << 11) | (code & 0x07ffu));
	}

	static constexpr LowerFastKind DecodeLower(u32 code)
	{
		switch (code >> 25)
		{
			case 0x00:
				return LowerFastKind::LQ;
			case 0x01:
				return LowerFastKind::SQ;
			case 0x04:
				return LowerFastKind::ILW;
			case 0x05:
				return LowerFastKind::ISW;
			case 0x08:
				return LowerFastKind::IADDIU;
			case 0x09:
				return LowerFastKind::ISUBIU;
			case 0x12:
				return LowerFastKind::FCAND;
			case 0x11:
				return LowerFastKind::FCSET;
			case 0x10:
				return LowerFastKind::FCEQ;
			case 0x13:
				return LowerFastKind::FCOR;
			case 0x14:
				return LowerFastKind::FSEQ;
			case 0x15:
				return LowerFastKind::FSSET;
			case 0x16:
				return LowerFastKind::FSAND;
			case 0x17:
				return LowerFastKind::FSOR;
			case 0x18:
				return LowerFastKind::FMEQ;
			case 0x1a:
				return LowerFastKind::FMAND;
			case 0x1b:
				return LowerFastKind::FMOR;
			case 0x1c:
				return LowerFastKind::FCGET;
			case 0x20:
				return LowerFastKind::B;
			case 0x21:
				return LowerFastKind::BAL;
			case 0x24:
				return LowerFastKind::JR;
			case 0x25:
				return LowerFastKind::JALR;
			case 0x28:
				return LowerFastKind::IBEQ;
			case 0x29:
				return LowerFastKind::IBNE;
			case 0x2c:
				return LowerFastKind::IBLTZ;
			case 0x2d:
				return LowerFastKind::IBGTZ;
			case 0x2e:
				return LowerFastKind::IBLEZ;
			case 0x2f:
				return LowerFastKind::IBGEZ;
			case 0x40:
				break;
			default:
				return LowerFastKind::None;
		}

		switch (code & 0x3f)
		{
			case 0x30:
				return LowerFastKind::IADD;
			case 0x31:
				return LowerFastKind::ISUB;
			case 0x32:
				return LowerFastKind::IADDI;
			case 0x34:
				return LowerFastKind::IAND;
			case 0x35:
				return LowerFastKind::IOR;
			case 0x3c:
				switch ((code >> 6) & 0x1f)
				{
					case 0x0c:
						return LowerFastKind::MOVE;
					case 0x0d:
						return LowerFastKind::LQI;
					case 0x0e:
						return LowerFastKind::DIV;
					case 0x0f:
						return LowerFastKind::MTIR;
					case 0x10:
						return LowerFastKind::RNEXT;
					case 0x19:
						return LowerFastKind::MFP;
					case 0x1a:
						return LowerFastKind::XTOP;
					case 0x1b:
						return LowerFastKind::XGKICK;
					case 0x1c:
						return LowerFastKind::ESADD;
					case 0x1d:
						return LowerFastKind::EATANxy;
					case 0x1e:
						return LowerFastKind::ESQRT;
					case 0x1f:
						return LowerFastKind::ESIN;
					default:
						return LowerFastKind::None;
				}
			case 0x3d:
				switch ((code >> 6) & 0x1f)
				{
					case 0x0c:
						return LowerFastKind::MR32;
					case 0x0d:
						return LowerFastKind::SQI;
					case 0x0e:
						return LowerFastKind::SQRT;
					case 0x0f:
						return LowerFastKind::MFIR;
					case 0x10:
						return LowerFastKind::RGET;
					case 0x1a:
						return LowerFastKind::XITOP;
					case 0x1c:
						return LowerFastKind::ERSADD;
					case 0x1d:
						return LowerFastKind::EATANxz;
					case 0x1e:
						return LowerFastKind::ERSQRT;
					case 0x1f:
						return LowerFastKind::EATAN;
					default:
						return LowerFastKind::None;
				}
			case 0x3e:
				switch ((code >> 6) & 0x1f)
				{
					case 0x0d:
						return LowerFastKind::LQD;
					case 0x0e:
						return LowerFastKind::RSQRT;
					case 0x0f:
						return LowerFastKind::ILWR;
					case 0x10:
						return LowerFastKind::RINIT;
					case 0x1c:
						return LowerFastKind::ELENG;
					case 0x1d:
						return LowerFastKind::ESUM;
					case 0x1e:
						return LowerFastKind::ERCPR;
					case 0x1f:
						return LowerFastKind::EEXP;
					default:
						return LowerFastKind::None;
				}
			case 0x3f:
				switch ((code >> 6) & 0x1f)
				{
					case 0x0d:
						return LowerFastKind::SQD;
					case 0x0e:
						return LowerFastKind::WAITQ;
					case 0x0f:
						return LowerFastKind::ISWR;
					case 0x10:
						return LowerFastKind::RXOR;
					case 0x1c:
						return LowerFastKind::ERLENG;
					case 0x1e:
						return LowerFastKind::WAITP;
					default:
						return LowerFastKind::None;
				}
			default:
				return LowerFastKind::None;
		}
	}

	static constexpr UpperFastKind DecodeUpper(u32 code)
	{
		switch (code & 0x3f)
		{
			case 0x00:
				return UpperFastKind::ADDx;
			case 0x01:
				return UpperFastKind::ADDy;
			case 0x02:
				return UpperFastKind::ADDz;
			case 0x03:
				return UpperFastKind::ADDw;
			case 0x04:
				return UpperFastKind::SUBx;
			case 0x05:
				return UpperFastKind::SUBy;
			case 0x06:
				return UpperFastKind::SUBz;
			case 0x07:
				return UpperFastKind::SUBw;
			case 0x08:
				return UpperFastKind::MADDx;
			case 0x09:
				return UpperFastKind::MADDy;
			case 0x0a:
				return UpperFastKind::MADDz;
			case 0x0b:
				return UpperFastKind::MADDw;
			case 0x0c:
				return UpperFastKind::MSUBx;
			case 0x0d:
				return UpperFastKind::MSUBy;
			case 0x0e:
				return UpperFastKind::MSUBz;
			case 0x0f:
				return UpperFastKind::MSUBw;
			case 0x10:
				return UpperFastKind::MAXx;
			case 0x11:
				return UpperFastKind::MAXy;
			case 0x12:
				return UpperFastKind::MAXz;
			case 0x13:
				return UpperFastKind::MAXw;
			case 0x14:
				return UpperFastKind::MINIx;
			case 0x15:
				return UpperFastKind::MINIy;
			case 0x16:
				return UpperFastKind::MINIz;
			case 0x17:
				return UpperFastKind::MINIw;
			case 0x18:
				return UpperFastKind::MULx;
			case 0x19:
				return UpperFastKind::MULy;
			case 0x1a:
				return UpperFastKind::MULz;
			case 0x1b:
				return UpperFastKind::MULw;
			case 0x1c:
				return UpperFastKind::MULq;
			case 0x1d:
				return UpperFastKind::MAXi;
			case 0x1e:
				return UpperFastKind::MULi;
			case 0x1f:
				return UpperFastKind::MINIi;
			case 0x20:
				return UpperFastKind::ADDq;
			case 0x21:
				return UpperFastKind::MADDq;
			case 0x22:
				return UpperFastKind::ADDi;
			case 0x23:
				return UpperFastKind::MADDi;
			case 0x24:
				return UpperFastKind::SUBq;
			case 0x25:
				return UpperFastKind::MSUBq;
			case 0x26:
				return UpperFastKind::SUBi;
			case 0x27:
				return UpperFastKind::MSUBi;
			case 0x28:
				return UpperFastKind::ADD;
			case 0x29:
				return UpperFastKind::MADD;
			case 0x2a:
				return UpperFastKind::MUL;
			case 0x2b:
				return UpperFastKind::MAX;
			case 0x2c:
				return UpperFastKind::SUB;
			case 0x2d:
				return UpperFastKind::MSUB;
			case 0x2e:
				return UpperFastKind::OPMSUB;
			case 0x2f:
				return UpperFastKind::MINI;
			case 0x3c:
				switch ((code >> 6) & 0x1f)
				{
					case 0x00:
						return UpperFastKind::ADDAx;
					case 0x01:
						return UpperFastKind::SUBAx;
					case 0x02:
						return UpperFastKind::MADDAx;
					case 0x03:
						return UpperFastKind::MSUBAx;
					case 0x04:
						return UpperFastKind::ITOF0;
					case 0x05:
						return UpperFastKind::FTOI0;
					case 0x06:
						return UpperFastKind::MULAx;
					case 0x07:
						return UpperFastKind::MULAq;
					case 0x08:
						return UpperFastKind::ADDAq;
					case 0x09:
						return UpperFastKind::SUBAq;
					case 0x0a:
						return UpperFastKind::ADDA;
					case 0x0b:
						return UpperFastKind::SUBA;
					default:
						return UpperFastKind::None;
				}
			case 0x3d:
				switch ((code >> 6) & 0x1f)
				{
					case 0x00:
						return UpperFastKind::ADDAy;
					case 0x01:
						return UpperFastKind::SUBAy;
					case 0x02:
						return UpperFastKind::MADDAy;
					case 0x03:
						return UpperFastKind::MSUBAy;
					case 0x04:
						return UpperFastKind::ITOF4;
					case 0x05:
						return UpperFastKind::FTOI4;
					case 0x06:
						return UpperFastKind::MULAy;
					case 0x07:
						return UpperFastKind::ABS;
					case 0x08:
						return UpperFastKind::MADDAq;
					case 0x09:
						return UpperFastKind::MSUBAq;
					case 0x0a:
						return UpperFastKind::MADDA;
					case 0x0b:
						return UpperFastKind::MSUBA;
					default:
						return UpperFastKind::None;
				}
			case 0x3e:
				switch ((code >> 6) & 0x1f)
				{
					case 0x00:
						return UpperFastKind::ADDAz;
					case 0x01:
						return UpperFastKind::SUBAz;
					case 0x02:
						return UpperFastKind::MADDAz;
					case 0x03:
						return UpperFastKind::MSUBAz;
					case 0x04:
						return UpperFastKind::ITOF12;
					case 0x05:
						return UpperFastKind::FTOI12;
					case 0x06:
						return UpperFastKind::MULAz;
					case 0x07:
						return UpperFastKind::MULAi;
					case 0x08:
						return UpperFastKind::ADDAi;
					case 0x09:
						return UpperFastKind::SUBAi;
					case 0x0a:
						return UpperFastKind::MULA;
					case 0x0b:
						return UpperFastKind::OPMULA;
					default:
						return UpperFastKind::None;
				}
			case 0x3f:
				switch ((code >> 6) & 0x1f)
				{
					case 0x00:
						return UpperFastKind::ADDAw;
					case 0x01:
						return UpperFastKind::SUBAw;
					case 0x02:
						return UpperFastKind::MADDAw;
					case 0x03:
						return UpperFastKind::MSUBAw;
					case 0x04:
						return UpperFastKind::ITOF15;
					case 0x05:
						return UpperFastKind::FTOI15;
					case 0x06:
						return UpperFastKind::MULAw;
					case 0x07:
						return UpperFastKind::CLIP;
					case 0x08:
						return UpperFastKind::MADDAi;
					case 0x09:
						return UpperFastKind::MSUBAi;
					case 0x0b:
						return UpperFastKind::NOP;
					default:
						return UpperFastKind::None;
				}
			default:
				return UpperFastKind::None;
		}
	}

	static inline void AnalyzeUpperFdfsft(u32 code, u32 ft_xyzw, _VURegsNum* regs)
	{
		regs->pipe = VUPIPE_FMAC;
		regs->VFwrite = Fd(code);
		regs->VFwxyzw = XYZW(code);
		regs->VFread0 = Fs(code);
		regs->VFr0xyzw = XYZW(code);
		regs->VFread1 = Ft(code);
		regs->VFr1xyzw = ft_xyzw;
		regs->VIread = Vf0Flag(Fs(code)) | Vf0Flag(Ft(code));
	}

	static inline void AnalyzeUpperFdfsi(u32 code, _VURegsNum* regs)
	{
		regs->pipe = VUPIPE_FMAC;
		regs->VFwrite = Fd(code);
		regs->VFwxyzw = XYZW(code);
		regs->VFread0 = Fs(code);
		regs->VFr0xyzw = XYZW(code);
		regs->VIread = (1u << REG_I) | Vf0Flag(Fs(code));
	}

	static inline void AnalyzeUpperFdfsq(u32 code, _VURegsNum* regs)
	{
		regs->pipe = VUPIPE_FMAC;
		regs->VFwrite = Fd(code);
		regs->VFwxyzw = XYZW(code);
		regs->VFread0 = Fs(code);
		regs->VFr0xyzw = XYZW(code);
		regs->VIread = (1u << REG_Q) | Vf0Flag(Fs(code));
	}

	static inline void AnalyzeUpperFdfsftReadAcc(u32 code, u32 ft_xyzw, _VURegsNum* regs)
	{
		regs->pipe = VUPIPE_FMAC;
		regs->VFwrite = Fd(code);
		regs->VFwxyzw = XYZW(code);
		regs->VFread0 = Fs(code);
		regs->VFr0xyzw = XYZW(code);
		regs->VFread1 = Ft(code);
		regs->VFr1xyzw = ft_xyzw;
		regs->VIread = (1u << REG_ACC_FLAG) | Vf0Flag(Fs(code)) | Vf0Flag(Ft(code));
	}

	static inline void AnalyzeUpperFdfsiReadAcc(u32 code, _VURegsNum* regs)
	{
		regs->pipe = VUPIPE_FMAC;
		regs->VFwrite = Fd(code);
		regs->VFwxyzw = XYZW(code);
		regs->VFread0 = Fs(code);
		regs->VFr0xyzw = XYZW(code);
		regs->VIread = (1u << REG_I) | (1u << REG_ACC_FLAG) | Vf0Flag(Fs(code));
	}

	static inline void AnalyzeUpperFdfsqReadAcc(u32 code, _VURegsNum* regs)
	{
		regs->pipe = VUPIPE_FMAC;
		regs->VFwrite = Fd(code);
		regs->VFwxyzw = XYZW(code);
		regs->VFread0 = Fs(code);
		regs->VFr0xyzw = XYZW(code);
		regs->VIread = (1u << REG_Q) | (1u << REG_ACC_FLAG) | Vf0Flag(Fs(code));
	}

	static inline void AnalyzeUpperMaddBroadcast(u32 code, u32 ft_xyzw, bool read_fs_vf0, _VURegsNum* regs)
	{
		regs->pipe = VUPIPE_FMAC;
		regs->VFwrite = Fd(code);
		regs->VFwxyzw = XYZW(code);
		regs->VFread0 = Fs(code);
		regs->VFr0xyzw = XYZW(code);
		regs->VFread1 = Ft(code);
		regs->VFr1xyzw = ft_xyzw;
		regs->VIread = (1u << REG_ACC_FLAG) | (read_fs_vf0 ? Vf0Flag(Fs(code)) : 0);
	}

	static inline void AnalyzeUpperAccFsFt(u32 code, bool read_acc, _VURegsNum* regs)
	{
		regs->pipe = VUPIPE_FMAC;
		regs->VFwxyzw = XYZW(code);
		regs->VFread0 = Fs(code);
		regs->VFr0xyzw = XYZW(code);
		regs->VFread1 = Ft(code);
		regs->VFr1xyzw = XYZW(code);
		regs->VIwrite = 1u << REG_ACC_FLAG;
		regs->VIread = Vf0Flag(Fs(code)) | Vf0Flag(Ft(code)) |
			(read_acc || XYZW(code) != 0x0f ? (1u << REG_ACC_FLAG) : 0);
	}

	static inline void AnalyzeUpperAccFsFtXyzw(u32 code, u32 ft_xyzw, bool read_acc, _VURegsNum* regs)
	{
		regs->pipe = VUPIPE_FMAC;
		regs->VFwxyzw = XYZW(code);
		regs->VFread0 = Fs(code);
		regs->VFr0xyzw = XYZW(code);
		regs->VFread1 = Ft(code);
		regs->VFr1xyzw = ft_xyzw;
		regs->VIwrite = 1u << REG_ACC_FLAG;
		regs->VIread = Vf0Flag(Fs(code)) | Vf0Flag(Ft(code)) |
			(read_acc || XYZW(code) != 0x0f ? (1u << REG_ACC_FLAG) : 0);
	}

	static inline void AnalyzeUpperAccFsI(u32 code, bool read_acc, _VURegsNum* regs)
	{
		regs->pipe = VUPIPE_FMAC;
		regs->VFwxyzw = XYZW(code);
		regs->VFread0 = Fs(code);
		regs->VFr0xyzw = XYZW(code);
		regs->VIwrite = 1u << REG_ACC_FLAG;
		regs->VIread = (1u << REG_I) | Vf0Flag(Fs(code)) |
			(read_acc || XYZW(code) != 0x0f ? (1u << REG_ACC_FLAG) : 0);
	}

	static inline void AnalyzeUpperAccFsQ(u32 code, bool read_acc, _VURegsNum* regs)
	{
		regs->pipe = VUPIPE_FMAC;
		regs->VFwxyzw = XYZW(code);
		regs->VFread0 = Fs(code);
		regs->VFr0xyzw = XYZW(code);
		regs->VIwrite = 1u << REG_ACC_FLAG;
		regs->VIread = (1u << REG_Q) | Vf0Flag(Fs(code)) |
			(read_acc || XYZW(code) != 0x0f ? (1u << REG_ACC_FLAG) : 0);
	}

	static inline void AnalyzeUpperMulBroadcast(u32 code, u32 ft_xyzw, bool acc, _VURegsNum* regs)
	{
		regs->pipe = VUPIPE_FMAC;
		regs->VFwrite = acc ? 0 : Fd(code);
		regs->VFwxyzw = XYZW(code);
		regs->VFread0 = Fs(code);
		regs->VFr0xyzw = XYZW(code);
		regs->VFread1 = Ft(code);
		regs->VFr1xyzw = ft_xyzw;
		regs->VIwrite = acc ? (1u << REG_ACC_FLAG) : 0;
		regs->VIread = Vf0Flag(Fs(code)) |
			(acc && XYZW(code) != 0x0f ? (1u << REG_ACC_FLAG) : 0);
	}

	static inline void AnalyzeUpperClip(u32 code, _VURegsNum* regs)
	{
		regs->pipe = VUPIPE_FMAC;
		regs->VFread0 = Fs(code);
		regs->VFr0xyzw = 0xe;
		regs->VFread1 = Ft(code);
		regs->VFr1xyzw = 0x1;
		regs->VIwrite = 1u << REG_CLIP_FLAG;
		regs->VIread = Vf0Flag(Fs(code)) | Vf0Flag(Ft(code)) | (1u << REG_CLIP_FLAG);
	}

	static inline void AnalyzeUpperOpmula(u32 code, _VURegsNum* regs)
	{
		regs->pipe = VUPIPE_FMAC;
		regs->VFwxyzw = 0xe;
		regs->VFread0 = Fs(code);
		regs->VFr0xyzw = 0xe;
		regs->VFread1 = Ft(code);
		regs->VFr1xyzw = 0xe;
		regs->VIwrite = 1u << REG_ACC_FLAG;
		regs->VIread = Vf0Flag(Fs(code)) | Vf0Flag(Ft(code)) | (1u << REG_ACC_FLAG);
	}

	static inline void AnalyzeUpperOpmsub(u32 code, _VURegsNum* regs)
	{
		regs->pipe = VUPIPE_FMAC;
		regs->VFwrite = Fd(code);
		regs->VFwxyzw = 0xe;
		regs->VFread0 = Fs(code);
		regs->VFr0xyzw = 0xe;
		regs->VFread1 = Ft(code);
		regs->VFr1xyzw = 0xe;
		regs->VIread = Vf0Flag(Fs(code)) | Vf0Flag(Ft(code)) | (1u << REG_ACC_FLAG);
	}

	static inline bool AnalyzeUpperNoLower(u32 code, _VURegsNum* regs)
	{
		const UpperFastKind kind = DecodeUpper(code);
		if (kind == UpperFastKind::None)
			return false;

		*regs = {};

		switch (kind)
		{
			case UpperFastKind::NOP:
				return true;
			case UpperFastKind::ADD:
			case UpperFastKind::SUB:
			case UpperFastKind::MAX:
			case UpperFastKind::MINI:
				AnalyzeUpperFdfsft(code, XYZW(code), regs);
				return true;
			case UpperFastKind::MUL:
				AnalyzeUpperFdfsft(code, XYZW(code), regs);
				return true;
			case UpperFastKind::MADD:
			case UpperFastKind::MSUB:
				AnalyzeUpperFdfsftReadAcc(code, XYZW(code), regs);
				return true;
			case UpperFastKind::ADDi:
			case UpperFastKind::SUBi:
			case UpperFastKind::MAXi:
			case UpperFastKind::MINIi:
				AnalyzeUpperFdfsi(code, regs);
				return true;
			case UpperFastKind::MULi:
				AnalyzeUpperFdfsi(code, regs);
				return true;
			case UpperFastKind::MADDi:
			case UpperFastKind::MSUBi:
				AnalyzeUpperFdfsiReadAcc(code, regs);
				return true;
			case UpperFastKind::ADDq:
			case UpperFastKind::SUBq:
			case UpperFastKind::MULq:
				AnalyzeUpperFdfsq(code, regs);
				return true;
			case UpperFastKind::MADDq:
			case UpperFastKind::MSUBq:
				AnalyzeUpperFdfsqReadAcc(code, regs);
				return true;
			case UpperFastKind::ADDx:
			case UpperFastKind::SUBx:
			case UpperFastKind::MAXx:
			case UpperFastKind::MINIx:
				AnalyzeUpperFdfsft(code, 0x8, regs);
				return true;
			case UpperFastKind::MULx:
				AnalyzeUpperMulBroadcast(code, 0x8, false, regs);
				return true;
			case UpperFastKind::MADDx:
				AnalyzeUpperMaddBroadcast(code, 0x8, Ft(code) != 0, regs);
				return true;
			case UpperFastKind::MSUBx:
				AnalyzeUpperFdfsftReadAcc(code, 0x8, regs);
				return true;
			case UpperFastKind::ADDy:
			case UpperFastKind::SUBy:
			case UpperFastKind::MAXy:
			case UpperFastKind::MINIy:
				AnalyzeUpperFdfsft(code, 0x4, regs);
				return true;
			case UpperFastKind::MULy:
				AnalyzeUpperMulBroadcast(code, 0x4, false, regs);
				return true;
			case UpperFastKind::MADDy:
				AnalyzeUpperMaddBroadcast(code, 0x4, Ft(code) != 0, regs);
				return true;
			case UpperFastKind::MSUBy:
				AnalyzeUpperFdfsftReadAcc(code, 0x4, regs);
				return true;
			case UpperFastKind::ADDz:
			case UpperFastKind::SUBz:
			case UpperFastKind::MAXz:
			case UpperFastKind::MINIz:
				AnalyzeUpperFdfsft(code, 0x2, regs);
				return true;
			case UpperFastKind::MULz:
				AnalyzeUpperMulBroadcast(code, 0x2, false, regs);
				return true;
			case UpperFastKind::MADDz:
				AnalyzeUpperMaddBroadcast(code, 0x2, Ft(code) != 0, regs);
				return true;
			case UpperFastKind::MSUBz:
				AnalyzeUpperFdfsftReadAcc(code, 0x2, regs);
				return true;
			case UpperFastKind::ADDw:
			case UpperFastKind::SUBw:
			case UpperFastKind::MAXw:
			case UpperFastKind::MINIw:
				AnalyzeUpperFdfsft(code, 0x1, regs);
				return true;
			case UpperFastKind::MULw:
				AnalyzeUpperMulBroadcast(code, 0x1, false, regs);
				return true;
			case UpperFastKind::MADDw:
				AnalyzeUpperMaddBroadcast(code, 0x1, true, regs);
				return true;
			case UpperFastKind::MSUBw:
				AnalyzeUpperFdfsftReadAcc(code, 0x1, regs);
				return true;
			case UpperFastKind::CLIP:
				AnalyzeUpperClip(code, regs);
				return true;
			case UpperFastKind::OPMULA:
				AnalyzeUpperOpmula(code, regs);
				return true;
			case UpperFastKind::OPMSUB:
				AnalyzeUpperOpmsub(code, regs);
				return true;
			case UpperFastKind::ADDA:
			case UpperFastKind::SUBA:
			case UpperFastKind::MULA:
				AnalyzeUpperAccFsFt(code, false, regs);
				return true;
			case UpperFastKind::ADDAi:
			case UpperFastKind::SUBAi:
			case UpperFastKind::MULAi:
				AnalyzeUpperAccFsI(code, false, regs);
				return true;
			case UpperFastKind::ADDAq:
			case UpperFastKind::SUBAq:
			case UpperFastKind::MULAq:
				AnalyzeUpperAccFsQ(code, false, regs);
				return true;
			case UpperFastKind::ADDAx:
			case UpperFastKind::SUBAx:
				AnalyzeUpperAccFsFtXyzw(code, 0x8, false, regs);
				return true;
			case UpperFastKind::ADDAy:
			case UpperFastKind::SUBAy:
				AnalyzeUpperAccFsFtXyzw(code, 0x4, false, regs);
				return true;
			case UpperFastKind::ADDAz:
			case UpperFastKind::SUBAz:
				AnalyzeUpperAccFsFtXyzw(code, 0x2, false, regs);
				return true;
			case UpperFastKind::ADDAw:
			case UpperFastKind::SUBAw:
				AnalyzeUpperAccFsFtXyzw(code, 0x1, false, regs);
				return true;
			case UpperFastKind::MULAx:
				AnalyzeUpperMulBroadcast(code, 0x8, true, regs);
				return true;
			case UpperFastKind::MULAy:
				AnalyzeUpperMulBroadcast(code, 0x4, true, regs);
				return true;
			case UpperFastKind::MULAz:
				AnalyzeUpperMulBroadcast(code, 0x2, true, regs);
				return true;
			case UpperFastKind::MULAw:
				AnalyzeUpperMulBroadcast(code, 0x1, true, regs);
				return true;
			case UpperFastKind::MADDA:
			case UpperFastKind::MSUBA:
				AnalyzeUpperAccFsFt(code, true, regs);
				return true;
			case UpperFastKind::MADDAi:
			case UpperFastKind::MSUBAi:
				AnalyzeUpperAccFsI(code, true, regs);
				return true;
			case UpperFastKind::MADDAq:
			case UpperFastKind::MSUBAq:
				AnalyzeUpperAccFsQ(code, true, regs);
				return true;
			case UpperFastKind::MSUBAx:
				AnalyzeUpperAccFsFtXyzw(code, 0x8, true, regs);
				return true;
			case UpperFastKind::MSUBAy:
				AnalyzeUpperAccFsFtXyzw(code, 0x4, true, regs);
				return true;
			case UpperFastKind::MSUBAz:
				AnalyzeUpperAccFsFtXyzw(code, 0x2, true, regs);
				return true;
			case UpperFastKind::MSUBAw:
				AnalyzeUpperAccFsFtXyzw(code, 0x1, true, regs);
				return true;
			case UpperFastKind::MADDAx:
				AnalyzeUpperFdfsftReadAcc(code, 0x8, regs);
				regs->VFwrite = 0;
				regs->VIwrite = 1u << REG_ACC_FLAG;
				return true;
			case UpperFastKind::MADDAy:
				AnalyzeUpperFdfsftReadAcc(code, 0x4, regs);
				regs->VFwrite = 0;
				regs->VIwrite = 1u << REG_ACC_FLAG;
				return true;
			case UpperFastKind::MADDAz:
				AnalyzeUpperFdfsftReadAcc(code, 0x2, regs);
				regs->VFwrite = 0;
				regs->VIwrite = 1u << REG_ACC_FLAG;
				return true;
			case UpperFastKind::MADDAw:
				AnalyzeUpperFdfsftReadAcc(code, 0x1, regs);
				regs->VFwrite = 0;
				regs->VIwrite = 1u << REG_ACC_FLAG;
				return true;
			default:
				break;
		}

		regs->pipe = VUPIPE_FMAC;
		regs->VFwrite = Ft(code);
		regs->VFwxyzw = XYZW(code);
		regs->VFread0 = Fs(code);
		regs->VFr0xyzw = XYZW(code);
		regs->VFr1xyzw = 0xff;
		regs->VIread = Ft(code) != 0 ? Vf0Flag(Fs(code)) : 0;
		return true;
	}

	static inline void AnalyzeIaluItIs(u32 code, _VURegsNum* regs)
	{
		regs->pipe = VUPIPE_IALU;
		regs->VIwrite = 1u << It(code);
		regs->VIread = 1u << Is(code);
	}

	static inline void AnalyzeIaluIdIsIt(u32 code, _VURegsNum* regs)
	{
		regs->pipe = VUPIPE_IALU;
		regs->VIwrite = 1u << Id(code);
		regs->VIread = (1u << Is(code)) | (1u << It(code));
	}

	static inline void AnalyzeFmacNoVf(u32 viwrite, u32 viread, _VURegsNum* regs)
	{
		regs->pipe = VUPIPE_FMAC;
		regs->VIwrite = viwrite;
		regs->VIread = viread;
	}

	static inline void AnalyzeEfuXyzw(u32 code, u32 cycles, _VURegsNum* regs)
	{
		regs->pipe = VUPIPE_EFU;
		regs->VFread0 = Fs(code);
		regs->VFr0xyzw = XYZW(code);
		regs->VIwrite = 1u << REG_P;
		regs->VIread = Vf0Flag(Fs(code));
		regs->cycles = cycles;
	}

	static inline void AnalyzeEfuFsf(u32 code, u32 cycles, _VURegsNum* regs)
	{
		regs->pipe = VUPIPE_EFU;
		regs->VFread0 = Fs(code);
		regs->VFr0xyzw = 1u << (3 - Fsf(code));
		regs->VIwrite = 1u << REG_P;
		regs->VIread = Vf0Flag(Fs(code));
		regs->cycles = cycles;
	}

	static inline bool AnalyzeLowerNoUpper(u32 code, _VURegsNum* regs)
	{
		const LowerFastKind kind = DecodeLower(code);
		if (kind == LowerFastKind::None)
			return false;

		*regs = {};

		switch (kind)
		{
			case LowerFastKind::LQ:
				regs->pipe = VUPIPE_FMAC;
				regs->VFwrite = Ft(code);
				regs->VFwxyzw = XYZW(code);
				regs->VIread = 1u << Is(code);
				return true;
			case LowerFastKind::SQ:
				regs->pipe = VUPIPE_FMAC;
				regs->VFread0 = Fs(code);
				regs->VFr0xyzw = XYZW(code);
				regs->VIread = 1u << It(code);
				return true;
			case LowerFastKind::ILW:
				AnalyzeIaluItIs(code, regs);
				regs->cycles = 4;
				return true;
			case LowerFastKind::ISW:
				regs->pipe = VUPIPE_IALU;
				regs->VIread = (1u << Is(code)) | (1u << It(code));
				return true;
			case LowerFastKind::IADDIU:
			case LowerFastKind::ISUBIU:
			case LowerFastKind::IADDI:
				AnalyzeIaluItIs(code, regs);
				return true;
			case LowerFastKind::IADD:
			case LowerFastKind::ISUB:
			case LowerFastKind::IAND:
			case LowerFastKind::IOR:
				AnalyzeIaluIdIsIt(code, regs);
				return true;
			case LowerFastKind::FCAND:
				AnalyzeFmacNoVf(1u << 1, 1u << REG_CLIP_FLAG, regs);
				return true;
			case LowerFastKind::FCSET:
				AnalyzeFmacNoVf(1u << REG_CLIP_FLAG, 0, regs);
				return true;
			case LowerFastKind::FCEQ:
			case LowerFastKind::FCOR:
				AnalyzeFmacNoVf(1u << 1, 1u << REG_CLIP_FLAG, regs);
				return true;
			case LowerFastKind::FSEQ:
			case LowerFastKind::FSAND:
			case LowerFastKind::FSOR:
				AnalyzeFmacNoVf(1u << It(code), 1u << REG_STATUS_FLAG, regs);
				return true;
			case LowerFastKind::FSSET:
				AnalyzeFmacNoVf(1u << REG_STATUS_FLAG, 0, regs);
				return true;
			case LowerFastKind::FMEQ:
			case LowerFastKind::FMAND:
			case LowerFastKind::FMOR:
				AnalyzeFmacNoVf(1u << It(code), (1u << REG_MAC_FLAG) | (1u << Is(code)), regs);
				return true;
			case LowerFastKind::FCGET:
				AnalyzeFmacNoVf(1u << It(code), 1u << REG_CLIP_FLAG, regs);
				return true;
			case LowerFastKind::LQI:
				regs->pipe = VUPIPE_FMAC;
				regs->VFwrite = Ft(code);
				regs->VFwxyzw = XYZW(code);
				regs->VIwrite = 1u << Is(code);
				regs->VIread = 1u << Is(code);
				return true;
			case LowerFastKind::LQD:
				regs->pipe = VUPIPE_FMAC;
				regs->VFwrite = Ft(code);
				regs->VFwxyzw = XYZW(code);
				regs->VIwrite = 1u << Is(code);
				regs->VIread = 1u << Is(code);
				return true;
			case LowerFastKind::SQI:
				regs->pipe = VUPIPE_FMAC;
				regs->VFread0 = Fs(code);
				regs->VFr0xyzw = XYZW(code);
				regs->VIwrite = 1u << It(code);
				regs->VIread = 1u << It(code);
				return true;
			case LowerFastKind::SQD:
				regs->pipe = VUPIPE_FMAC;
				regs->VFread0 = Fs(code);
				regs->VFr0xyzw = XYZW(code);
				regs->VIwrite = 1u << It(code);
				regs->VIread = 1u << It(code);
				return true;
			case LowerFastKind::ILWR:
				AnalyzeIaluItIs(code, regs);
				regs->cycles = 4;
				return true;
			case LowerFastKind::ISWR:
				regs->pipe = VUPIPE_IALU;
				regs->VIread = (1u << Is(code)) | (1u << It(code));
				return true;
			case LowerFastKind::MOVE:
				regs->pipe = Ft(code) == 0 ? VUPIPE_NONE : VUPIPE_FMAC;
				regs->VFwrite = Ft(code);
				regs->VFwxyzw = XYZW(code);
				regs->VFread0 = Fs(code);
				regs->VFr0xyzw = XYZW(code);
				regs->VIread = Ft(code) != 0 ? Vf0Flag(Fs(code)) : 0;
				return true;
			case LowerFastKind::MR32:
				regs->pipe = VUPIPE_FMAC;
				regs->VFwrite = Ft(code);
				regs->VFwxyzw = XYZW(code);
				regs->VFread0 = Fs(code);
				regs->VFr0xyzw = (XYZW(code) >> 1) | ((XYZW(code) << 3) & 0x8);
				regs->VFr1xyzw = 0xff;
				regs->VIread = Ft(code) != 0 ? Vf0Flag(Fs(code)) : 0;
				return true;
			case LowerFastKind::MFIR:
				regs->pipe = VUPIPE_FMAC;
				regs->VFwrite = Ft(code);
				regs->VFwxyzw = XYZW(code);
				regs->VIread = 1u << Is(code);
				return true;
			case LowerFastKind::MTIR:
				regs->pipe = VUPIPE_FMAC;
				regs->VFread0 = Fs(code);
				regs->VFr0xyzw = 1u << (3 - Fsf(code));
				regs->VIwrite = 1u << It(code);
				regs->VIread = Vf0Flag(Fs(code));
				return true;
			case LowerFastKind::RINIT:
				regs->pipe = VUPIPE_FMAC;
				regs->VFread0 = Fs(code);
				regs->VFr0xyzw = 1u << (3 - Fsf(code));
				regs->VIwrite = 1u << REG_R;
				regs->VIread = Vf0Flag(Fs(code));
				return true;
			case LowerFastKind::RGET:
				regs->pipe = VUPIPE_FMAC;
				regs->VFwrite = Ft(code);
				regs->VFwxyzw = XYZW(code);
				regs->VIread = 1u << REG_R;
				return true;
			case LowerFastKind::RNEXT:
				regs->pipe = VUPIPE_FMAC;
				regs->VFwrite = Ft(code);
				regs->VFwxyzw = XYZW(code);
				regs->VIwrite = 1u << REG_R;
				regs->VIread = 1u << REG_R;
				return true;
			case LowerFastKind::RXOR:
				regs->pipe = VUPIPE_FMAC;
				regs->VFread0 = Fs(code);
				regs->VFr0xyzw = 1u << (3 - Fsf(code));
				regs->VIwrite = 1u << REG_R;
				regs->VIread = (1u << REG_R) | Vf0Flag(Fs(code));
				return true;
			case LowerFastKind::IBEQ:
			case LowerFastKind::IBNE:
				regs->pipe = VUPIPE_BRANCH;
				regs->VIread = (1u << Is(code)) | (1u << It(code));
				return true;
			case LowerFastKind::IBLTZ:
			case LowerFastKind::IBGTZ:
			case LowerFastKind::IBLEZ:
			case LowerFastKind::IBGEZ:
			case LowerFastKind::JR:
				regs->pipe = VUPIPE_BRANCH;
				regs->VIread = 1u << Is(code);
				return true;
			case LowerFastKind::B:
				regs->pipe = VUPIPE_BRANCH;
				return true;
			case LowerFastKind::BAL:
				regs->pipe = VUPIPE_BRANCH;
				regs->VIwrite = 1u << It(code);
				return true;
			case LowerFastKind::JALR:
				regs->pipe = VUPIPE_BRANCH;
				regs->VIwrite = 1u << It(code);
				regs->VIread = 1u << Is(code);
				return true;
			case LowerFastKind::MFP:
				regs->pipe = VUPIPE_FMAC;
				regs->VFwrite = Ft(code);
				regs->VFwxyzw = XYZW(code);
				regs->VIread = 1u << REG_P;
				return true;
			case LowerFastKind::WAITQ:
				regs->pipe = VUPIPE_FDIV;
				return true;
			case LowerFastKind::WAITP:
				regs->pipe = VUPIPE_EFU;
				return true;
			case LowerFastKind::XITOP:
			case LowerFastKind::XTOP:
				regs->pipe = VUPIPE_IALU;
				regs->VIwrite = 1u << It(code);
				return true;
			case LowerFastKind::DIV:
				regs->pipe = VUPIPE_FDIV;
				regs->VFread0 = Fs(code);
				regs->VFr0xyzw = 1u << (3 - Fsf(code));
				regs->VFread1 = Ft(code);
				regs->VFr1xyzw = 1u << (3 - ((code >> 23) & 0x03));
				regs->VIwrite = 1u << REG_Q;
				regs->VIread = Vf0Flag(Fs(code)) | Vf0Flag(Ft(code));
				regs->cycles = 7;
				return true;
			case LowerFastKind::SQRT:
				regs->pipe = VUPIPE_FDIV;
				regs->VFread1 = Ft(code);
				regs->VFr1xyzw = 1u << (3 - ((code >> 23) & 0x03));
				regs->VIwrite = 1u << REG_Q;
				regs->VIread = Vf0Flag(Ft(code));
				regs->cycles = 7;
				return true;
			case LowerFastKind::RSQRT:
				regs->pipe = VUPIPE_FDIV;
				regs->VFread0 = Fs(code);
				regs->VFr0xyzw = 1u << (3 - Fsf(code));
				regs->VFread1 = Ft(code);
				regs->VFr1xyzw = 1u << (3 - ((code >> 23) & 0x03));
				regs->VIwrite = 1u << REG_Q;
				regs->VIread = Vf0Flag(Fs(code)) | Vf0Flag(Ft(code));
				regs->cycles = 13;
				return true;
			case LowerFastKind::XGKICK:
				regs->pipe = VUPIPE_XGKICK;
				regs->VIread = 1u << Is(code);
				return true;
			case LowerFastKind::ESADD:
				AnalyzeEfuXyzw(code, 11, regs);
				return true;
			case LowerFastKind::ERSADD:
			case LowerFastKind::ELENG:
				AnalyzeEfuXyzw(code, 18, regs);
				return true;
			case LowerFastKind::ERLENG:
				AnalyzeEfuXyzw(code, 24, regs);
				return true;
			case LowerFastKind::EATANxy:
			case LowerFastKind::EATANxz:
				AnalyzeEfuXyzw(code, 54, regs);
				return true;
			case LowerFastKind::ESUM:
				AnalyzeEfuXyzw(code, 12, regs);
				return true;
			case LowerFastKind::ERCPR:
			case LowerFastKind::ESQRT:
				AnalyzeEfuFsf(code, 12, regs);
				return true;
			case LowerFastKind::ERSQRT:
				AnalyzeEfuFsf(code, 18, regs);
				return true;
			case LowerFastKind::ESIN:
				AnalyzeEfuFsf(code, 29, regs);
				return true;
			case LowerFastKind::EATAN:
				AnalyzeEfuFsf(code, 54, regs);
				return true;
			case LowerFastKind::EEXP:
				AnalyzeEfuFsf(code, 44, regs);
				return true;
			case LowerFastKind::None:
				break;
		}

		return false;
	}

	static inline u32* VuMemQword(VURegs* VU, u32 address)
	{
		return GET_VU_MEM(VU, address);
	}

	static constexpr u8 XYZW_LAST_HALFWORD_INDEX[16] = {
		0, 6, 4, 6,
		2, 6, 4, 6,
		0, 6, 4, 6,
		2, 6, 4, 6,
	};

	static inline void LoadViHalfwordFromMemoryMasked(VURegs* VU, unsigned it, unsigned mask, const u16* ptr)
	{
		if (it == 0 || mask == 0)
			return;

		VU->VI[it].US[0] = ptr[XYZW_LAST_HALFWORD_INDEX[mask]];
	}

#if defined(ARCH_ARM32)
	alignas(16) static constexpr u32 XYZW_LANE_WRITE_MASKS_NEON[16][4] = {
		{0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u},
		{0x00000000u, 0x00000000u, 0x00000000u, 0xffffffffu},
		{0x00000000u, 0x00000000u, 0xffffffffu, 0x00000000u},
		{0x00000000u, 0x00000000u, 0xffffffffu, 0xffffffffu},
		{0x00000000u, 0xffffffffu, 0x00000000u, 0x00000000u},
		{0x00000000u, 0xffffffffu, 0x00000000u, 0xffffffffu},
		{0x00000000u, 0xffffffffu, 0xffffffffu, 0x00000000u},
		{0x00000000u, 0xffffffffu, 0xffffffffu, 0xffffffffu},
		{0xffffffffu, 0x00000000u, 0x00000000u, 0x00000000u},
		{0xffffffffu, 0x00000000u, 0x00000000u, 0xffffffffu},
		{0xffffffffu, 0x00000000u, 0xffffffffu, 0x00000000u},
		{0xffffffffu, 0x00000000u, 0xffffffffu, 0xffffffffu},
		{0xffffffffu, 0xffffffffu, 0x00000000u, 0x00000000u},
		{0xffffffffu, 0xffffffffu, 0x00000000u, 0xffffffffu},
		{0xffffffffu, 0xffffffffu, 0xffffffffu, 0x00000000u},
		{0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu},
	};

	static inline uint32x4_t BlendQwordMaskedNeon(const u32* old_bits, unsigned mask, uint32x4_t result)
	{
		if (mask == 0x0f)
			return result;

		const uint32x4_t old_value = vld1q_u32(old_bits);
		const uint32x4_t write_mask = vld1q_u32(XYZW_LANE_WRITE_MASKS_NEON[mask]);
		return vbslq_u32(write_mask, result, old_value);
	}

	static inline void StoreLowerVfResultMaskedNeon(u32* dest, unsigned mask, uint32x4_t result)
	{
		if (mask == 0)
			return;

		vst1q_u32(dest, BlendQwordMaskedNeon(dest, mask, result));
#if defined(VITASX2_QEMU_VALIDATION)
		++::g_qemuVuLowerNeonQwordOps;
#endif
	}

	static inline void StoreUpperResultMaskedNeon(VURegs* VU, unsigned fd, unsigned mask, uint32x4_t result)
	{
		if (mask == 0)
			return;

		vst1q_u32(VU->VF[fd].UL, BlendQwordMaskedNeon(VU->VF[fd].UL, mask, result));
#if defined(VITASX2_QEMU_VALIDATION)
		++::g_qemuVuUpperNeonQwordOps;
#endif
	}
#endif

	static inline void LoadVfMasked(VURegs* VU, unsigned ft, unsigned mask, const u32* ptr)
	{
		if (ft == 0)
			return;
#if defined(ARCH_ARM32)
		if (mask != 0)
		{
			StoreLowerVfResultMaskedNeon(VU->VF[ft].UL, mask, vld1q_u32(ptr));
			return;
		}
#endif
		if (mask & 0x8) VU->VF[ft].UL[0] = ptr[0];
		if (mask & 0x4) VU->VF[ft].UL[1] = ptr[1];
		if (mask & 0x2) VU->VF[ft].UL[2] = ptr[2];
		if (mask & 0x1) VU->VF[ft].UL[3] = ptr[3];
	}

	static inline void StoreVfMasked(VURegs* VU, unsigned fs, unsigned mask, u32* ptr)
	{
#if defined(ARCH_ARM32)
		if (mask != 0)
		{
			StoreLowerVfResultMaskedNeon(ptr, mask, vld1q_u32(VU->VF[fs].UL));
			return;
		}
#endif
		if (mask & 0x8) ptr[0] = VU->VF[fs].UL[0];
		if (mask & 0x4) ptr[1] = VU->VF[fs].UL[1];
		if (mask & 0x2) ptr[2] = VU->VF[fs].UL[2];
		if (mask & 0x1) ptr[3] = VU->VF[fs].UL[3];
	}

	static inline void FillVfMasked(VURegs* VU, unsigned ft, unsigned mask, u32 value)
	{
		if (ft == 0)
			return;
#if defined(ARCH_ARM32)
		if (mask != 0)
		{
			StoreLowerVfResultMaskedNeon(VU->VF[ft].UL, mask, vdupq_n_u32(value));
			return;
		}
#endif
		if (mask & 0x8) VU->VF[ft].UL[0] = value;
		if (mask & 0x4) VU->VF[ft].UL[1] = value;
		if (mask & 0x2) VU->VF[ft].UL[2] = value;
		if (mask & 0x1) VU->VF[ft].UL[3] = value;
	}

	static inline void StoreViToVectorMasked(VURegs* VU, unsigned ft, unsigned mask, s32 value)
	{
		if (ft == 0)
			return;
#if defined(ARCH_ARM32)
		if (mask != 0)
		{
			StoreLowerVfResultMaskedNeon(VU->VF[ft].UL, mask, vreinterpretq_u32_s32(vdupq_n_s32(value)));
			return;
		}
#endif
		if (mask & 0x8) VU->VF[ft].SL[0] = value;
		if (mask & 0x4) VU->VF[ft].SL[1] = value;
		if (mask & 0x2) VU->VF[ft].SL[2] = value;
		if (mask & 0x1) VU->VF[ft].SL[3] = value;
	}

#if defined(ARCH_ARM32)
	static inline void StoreViHalfwordToMemoryQwordMasked(u16 value, unsigned mask, u16* ptr)
	{
		StoreLowerVfResultMaskedNeon(reinterpret_cast<u32*>(ptr), mask, vdupq_n_u32(value));
	}

	static inline void StoreMr32Masked(VURegs* VU, unsigned ft, unsigned mask, unsigned fs)
	{
		const uint32x4_t source = vld1q_u32(VU->VF[fs].UL);
		StoreLowerVfResultMaskedNeon(VU->VF[ft].UL, mask, vextq_u32(source, source, 1));
	}
#endif

	static inline float FloatFromBits(u32 bits)
	{
		float value;
		std::memcpy(&value, &bits, sizeof(value));
		return value;
	}

	static inline u32 FloatToBits(float value)
	{
		u32 bits;
		std::memcpy(&bits, &value, sizeof(bits));
		return bits;
	}

	static inline float VuDouble(u32 bits);

	template <typename Unary>
	static inline void StoreUnaryUpperMasked(VURegs* VU, unsigned ft, unsigned mask, unsigned fs, Unary fn)
	{
		if (ft == 0)
			return;
		if (mask & 0x8) VU->VF[ft].UL[0] = fn(VU->VF[fs].UL[0]);
		if (mask & 0x4) VU->VF[ft].UL[1] = fn(VU->VF[fs].UL[1]);
		if (mask & 0x2) VU->VF[ft].UL[2] = fn(VU->VF[fs].UL[2]);
		if (mask & 0x1) VU->VF[ft].UL[3] = fn(VU->VF[fs].UL[3]);
	}

	static inline bool StoreAbsUpperMaskedNeon(VURegs* VU, unsigned ft, unsigned mask, unsigned fs)
	{
#if defined(ARCH_ARM32)
		if (ft != 0 && mask != 0)
		{
			const uint32x4_t signless = vandq_u32(vld1q_u32(VU->VF[fs].UL), vdupq_n_u32(0x7fffffffu));
			StoreUpperResultMaskedNeon(VU, ft, mask, signless);
			return true;
		}
#endif
		return false;
	}

	template <u32 Offset>
	static inline bool StoreFtoiUpperMaskedNeon(VURegs* VU, unsigned ft, unsigned mask, unsigned fs)
	{
#if defined(ARCH_ARM32)
		if (ft == 0 || mask == 0)
			return false;

		const uint32x4_t source_bits = vld1q_u32(VU->VF[fs].UL);
		float32x4_t scaled = vreinterpretq_f32_u32(source_bits);
		if (Offset != 0)
			scaled = vmulq_f32(scaled, vdupq_n_f32(FloatFromBits(0x3f800000u + (Offset << 23))));

		const uint32x4_t scaled_bits = vreinterpretq_u32_f32(scaled);
		const uint32x4_t exponent = vandq_u32(scaled_bits, vdupq_n_u32(0x7f800000u));
		const uint32x4_t sign_mask = vtstq_u32(source_bits, vdupq_n_u32(0x80000000u));
		const uint32x4_t saturated = vbslq_u32(
			sign_mask,
			vdupq_n_u32(0x80000000u),
			vdupq_n_u32(0x7fffffffu));
		const uint32x4_t converted = vreinterpretq_u32_s32(vcvtq_s32_f32(scaled));
		const uint32x4_t saturate_mask = vcgeq_u32(exponent, vdupq_n_u32(0x4f000000u));
		StoreUpperResultMaskedNeon(VU, ft, mask, vbslq_u32(saturate_mask, saturated, converted));
		return true;
#endif
		return false;
	}

	template <u32 Offset>
	static inline bool StoreItofUpperMaskedNeon(VURegs* VU, unsigned ft, unsigned mask, unsigned fs)
	{
#if defined(ARCH_ARM32)
		if (ft == 0 || mask == 0)
			return false;

		float32x4_t result = vcvtq_f32_s32(vreinterpretq_s32_u32(vld1q_u32(VU->VF[fs].UL)));
		if (Offset != 0)
			result = vmulq_f32(result, vdupq_n_f32(FloatFromBits(0x3f800000u - (Offset << 23))));

		StoreUpperResultMaskedNeon(VU, ft, mask, vreinterpretq_u32_f32(result));
		return true;
#endif
		return false;
	}

#if defined(ARCH_ARM32)
	static inline uint32x4_t MinMaxBitsNeon(uint32x4_t fs_bits, uint32x4_t ft_bits, bool take_max)
	{
		const int32x4_t fs_signed = vreinterpretq_s32_u32(fs_bits);
		const int32x4_t ft_signed = vreinterpretq_s32_u32(ft_bits);
		const uint32x4_t signed_min = vreinterpretq_u32_s32(vminq_s32(fs_signed, ft_signed));
		const uint32x4_t signed_max = vreinterpretq_u32_s32(vmaxq_s32(fs_signed, ft_signed));
		const uint32x4_t both_negative =
			vtstq_u32(vandq_u32(fs_bits, ft_bits), vdupq_n_u32(0x80000000u));
		return take_max ?
			vbslq_u32(both_negative, signed_min, signed_max) :
			vbslq_u32(both_negative, signed_max, signed_min);
	}
#endif

	static inline bool StoreMinMaxUpperMaskedNeon(VURegs* VU, unsigned fd, unsigned mask, unsigned fs, unsigned ft, bool take_max)
	{
#if defined(ARCH_ARM32)
		if (fd != 0 && mask != 0)
		{
			const uint32x4_t fs_bits = vld1q_u32(VU->VF[fs].UL);
			const uint32x4_t ft_bits = vld1q_u32(VU->VF[ft].UL);
			StoreUpperResultMaskedNeon(VU, fd, mask, MinMaxBitsNeon(fs_bits, ft_bits, take_max));
			return true;
		}
#endif
		return false;
	}

	static inline bool StoreMinMaxUpperBroadcastMaskedNeon(VURegs* VU, unsigned fd, unsigned mask, unsigned fs, u32 ft_bits, bool take_max)
	{
#if defined(ARCH_ARM32)
		if (fd != 0 && mask != 0)
		{
			const uint32x4_t fs_bits = vld1q_u32(VU->VF[fs].UL);
			StoreUpperResultMaskedNeon(VU, fd, mask, MinMaxBitsNeon(fs_bits, vdupq_n_u32(ft_bits), take_max));
			return true;
		}
#endif
		return false;
	}

	template <typename Binary>
	static inline void StoreBinaryUpperMasked(VURegs* VU, unsigned fd, unsigned mask, unsigned fs, unsigned ft, Binary fn)
	{
		if (fd == 0)
			return;
		if (mask & 0x8) VU->VF[fd].UL[0] = fn(VU->VF[fs].UL[0], VU->VF[ft].UL[0]);
		if (mask & 0x4) VU->VF[fd].UL[1] = fn(VU->VF[fs].UL[1], VU->VF[ft].UL[1]);
		if (mask & 0x2) VU->VF[fd].UL[2] = fn(VU->VF[fs].UL[2], VU->VF[ft].UL[2]);
		if (mask & 0x1) VU->VF[fd].UL[3] = fn(VU->VF[fs].UL[3], VU->VF[ft].UL[3]);
	}

	template <typename Binary>
	static inline void StoreBinaryUpperBroadcastMasked(VURegs* VU, unsigned fd, unsigned mask, unsigned fs, u32 ft, Binary fn)
	{
		if (fd == 0)
			return;
		if (mask & 0x8) VU->VF[fd].UL[0] = fn(VU->VF[fs].UL[0], ft);
		if (mask & 0x4) VU->VF[fd].UL[1] = fn(VU->VF[fs].UL[1], ft);
		if (mask & 0x2) VU->VF[fd].UL[2] = fn(VU->VF[fs].UL[2], ft);
		if (mask & 0x1) VU->VF[fd].UL[3] = fn(VU->VF[fs].UL[3], ft);
	}

	static inline u32 FpMaxBits(u32 a, u32 b)
	{
		const s32 sa = static_cast<s32>(a);
		const s32 sb = static_cast<s32>(b);
		if (sa < 0 && sb < 0)
			return static_cast<u32>(sa < sb ? sa : sb);
		return static_cast<u32>(sa < sb ? sb : sa);
	}

	static inline u32 FpMinBits(u32 a, u32 b)
	{
		const s32 sa = static_cast<s32>(a);
		const s32 sb = static_cast<s32>(b);
		if (sa < 0 && sb < 0)
			return static_cast<u32>(sa < sb ? sb : sa);
		return static_cast<u32>(sa < sb ? sa : sb);
	}

	static inline bool ExecuteClipNeon(VURegs* VU, u32 code)
	{
#if defined(ARCH_ARM32)
		const unsigned fs = Fs(code);
		const u32 ft_w = VU->VF[Ft(code)].UL[3];
		const s32 value = (ft_w & 0x7f800000u) ?
			static_cast<s32>(ft_w & 0x7fffffffu) :
			static_cast<s32>(0x007fffffu);

		const uint32x4_t fs_bits = vld1q_u32(VU->VF[fs].UL);
		const int32x4_t limit = vdupq_n_s32(value);
		const uint32x4_t pos = vcgtq_s32(vreinterpretq_s32_u32(fs_bits), limit);
		const uint32x4_t neg_bits = veorq_u32(fs_bits, vdupq_n_u32(0x80000000u));
		const uint32x4_t neg = vcgtq_s32(vreinterpretq_s32_u32(neg_bits), limit);
		const u32 flags =
			((vgetq_lane_u32(pos, 0) >> 31) << 0) |
			((vgetq_lane_u32(neg, 0) >> 31) << 1) |
			((vgetq_lane_u32(pos, 1) >> 31) << 2) |
			((vgetq_lane_u32(neg, 1) >> 31) << 3) |
			((vgetq_lane_u32(pos, 2) >> 31) << 4) |
			((vgetq_lane_u32(neg, 2) >> 31) << 5);

		VU->clipflag = ((VU->clipflag << 6) | flags) & 0x00ffffffu;
		return true;
#endif
		return false;
	}

	static inline void ExecuteClip(VURegs* VU, u32 code)
	{
		if (ExecuteClipNeon(VU, code))
			return;

		const s32 value = (VU->VF[Ft(code)].UL[3] & 0x7f800000u) ?
			static_cast<s32>(VU->VF[Ft(code)].UL[3] & 0x7fffffffu) :
			static_cast<s32>(0x007fffffu);
		const u32 pos = 0x00000000u;
		const u32 neg = 0x80000000u;
		const unsigned fs = Fs(code);

		VU->clipflag <<= 6;
		if (static_cast<s32>(VU->VF[fs].UL[0] ^ pos) > value) VU->clipflag |= 0x01;
		if (static_cast<s32>(VU->VF[fs].UL[0] ^ neg) > value) VU->clipflag |= 0x02;
		if (static_cast<s32>(VU->VF[fs].UL[1] ^ pos) > value) VU->clipflag |= 0x04;
		if (static_cast<s32>(VU->VF[fs].UL[1] ^ neg) > value) VU->clipflag |= 0x08;
		if (static_cast<s32>(VU->VF[fs].UL[2] ^ pos) > value) VU->clipflag |= 0x10;
		if (static_cast<s32>(VU->VF[fs].UL[2] ^ neg) > value) VU->clipflag |= 0x20;
		VU->clipflag &= 0x00ffffffu;
	}

	static inline void ClearMacLane(VURegs* VU, unsigned lane)
	{
		switch (lane)
		{
			case 0:
				VU_MACx_CLEAR(VU);
				return;
			case 1:
				VU_MACy_CLEAR(VU);
				return;
			case 2:
				VU_MACz_CLEAR(VU);
				return;
			default:
				VU_MACw_CLEAR(VU);
				return;
		}
	}

	static inline u32 UpdateMacLane(VURegs* VU, unsigned lane, float value)
	{
		switch (lane)
		{
			case 0:
				return VU_MACx_UPDATE(VU, value);
			case 1:
				return VU_MACy_UPDATE(VU, value);
			case 2:
				return VU_MACz_UPDATE(VU, value);
			default:
				return VU_MACw_UPDATE(VU, value);
		}
	}

	static inline void WriteMacResult(VURegs* VU, bool acc, unsigned fd, unsigned lane, u32 value)
	{
		if (acc)
			VU->ACC.UL[lane] = value;
		else if (fd != 0)
			VU->VF[fd].UL[lane] = value;
	}

	static inline float VuAddTriAceHack(u32 a, u32 b)
	{
		const s32 a_exp = (a >> 23) & 0xff;
		const s32 b_exp = (b >> 23) & 0xff;
		if (a_exp - b_exp >= 25)
			b &= 0x80000000u;
		if (a_exp - b_exp <= -25)
			a &= 0x80000000u;
		return VuDouble(a) + VuDouble(b);
	}

#if defined(ARCH_ARM32)
	static inline uint32x4_t VuDoubleBitsNeon(uint32x4_t bits)
	{
		// PCSX2 owner: VUops.cpp::vuDouble(). Normalize inputs before the
		// NEON arithmetic body, then keep MAC/status writes on VUflags.cpp.
		const uint32x4_t exponent_mask = vdupq_n_u32(0x7f800000u);
		const uint32x4_t sign_mask = vdupq_n_u32(0x80000000u);
		const uint32x4_t exponent = vandq_u32(bits, exponent_mask);
		const uint32x4_t sign = vandq_u32(bits, sign_mask);
		const uint32x4_t denormal = vceqq_u32(exponent, vdupq_n_u32(0));
		bits = vbslq_u32(denormal, sign, bits);

#ifndef INT_VUDOUBLEHACK
		if (CHECK_VU_OVERFLOW(0))
		{
			const uint32x4_t infinite = vceqq_u32(exponent, exponent_mask);
			bits = vbslq_u32(infinite, vorrq_u32(sign, vdupq_n_u32(0x7f7fffffu)), bits);
		}
#endif
		return bits;
	}

	static inline float32x4_t VuFloatQNeon(uint32x4_t bits)
	{
		return vreinterpretq_f32_u32(VuDoubleBitsNeon(bits));
	}

	static inline float VuSumXYZSquaresNeon(VURegs* VU, unsigned reg)
	{
		const float32x4_t value = VuFloatQNeon(vld1q_u32(VU->VF[reg].UL));
		const float32x4_t squared = vmulq_f32(value, value);
#if defined(VITASX2_QEMU_VALIDATION)
		++::g_qemuVuLowerNeonQwordOps;
#endif
		return (vgetq_lane_f32(squared, 0) + vgetq_lane_f32(squared, 1)) +
			vgetq_lane_f32(squared, 2);
	}

	static inline void FinishMacVectorNeon(VURegs* VU, bool acc, unsigned fd, unsigned mask, float32x4_t result)
	{
		if (mask & 0x8)
			WriteMacResult(VU, acc, fd, 0, UpdateMacLane(VU, 0, vgetq_lane_f32(result, 0)));
		else
			ClearMacLane(VU, 0);
		if (mask & 0x4)
			WriteMacResult(VU, acc, fd, 1, UpdateMacLane(VU, 1, vgetq_lane_f32(result, 1)));
		else
			ClearMacLane(VU, 1);
		if (mask & 0x2)
			WriteMacResult(VU, acc, fd, 2, UpdateMacLane(VU, 2, vgetq_lane_f32(result, 2)));
		else
			ClearMacLane(VU, 2);
		if (mask & 0x1)
			WriteMacResult(VU, acc, fd, 3, UpdateMacLane(VU, 3, vgetq_lane_f32(result, 3)));
		else
			ClearMacLane(VU, 3);

		VU_STAT_UPDATE(VU);
#if defined(VITASX2_QEMU_VALIDATION)
		++::g_qemuVuUpperNeonQwordOps;
#endif
	}

	static inline bool ExecuteAddSubMaskedNeon(VURegs* VU, u32 code, bool acc, bool subtract, uint32x4_t operand_bits)
	{
		const unsigned mask = XYZW(code);
		if (mask == 0)
			return false;

		const float32x4_t fs = VuFloatQNeon(vld1q_u32(VU->VF[Fs(code)].UL));
		const float32x4_t operand = VuFloatQNeon(operand_bits);
		FinishMacVectorNeon(VU, acc, Fd(code), mask, subtract ? vsubq_f32(fs, operand) : vaddq_f32(fs, operand));
		return true;
	}

	static inline bool ExecuteMulMaskedNeon(VURegs* VU, u32 code, bool acc, uint32x4_t operand_bits)
	{
		const unsigned mask = XYZW(code);
		if (mask == 0)
			return false;

		const float32x4_t fs = VuFloatQNeon(vld1q_u32(VU->VF[Fs(code)].UL));
		const float32x4_t operand = VuFloatQNeon(operand_bits);
		FinishMacVectorNeon(VU, acc, Fd(code), mask, vmulq_f32(fs, operand));
		return true;
	}

	static inline bool ExecuteMaddMsubMaskedNeon(VURegs* VU, u32 code, bool acc, bool subtract, uint32x4_t operand_bits)
	{
		const unsigned mask = XYZW(code);
		if (mask == 0)
			return false;

		const float32x4_t acc_value = VuFloatQNeon(vld1q_u32(VU->ACC.UL));
		const float32x4_t fs = VuFloatQNeon(vld1q_u32(VU->VF[Fs(code)].UL));
		const float32x4_t operand = VuFloatQNeon(operand_bits);
		const float32x4_t product = vmulq_f32(fs, operand);
		FinishMacVectorNeon(VU, acc, Fd(code), mask,
			subtract ? vsubq_f32(acc_value, product) : vaddq_f32(acc_value, product));
		return true;
	}

	static inline float32x4_t OuterProductNeon(VURegs* VU, u32 code)
	{
		// PCSX2 owner: VUops.cpp::_vuOPMULA()/_vuOPMSUB() use
		// {Fs.y * Ft.z, Fs.z * Ft.x, Fs.x * Ft.y}; W is ignored.
		const float32x4_t fs = VuFloatQNeon(vld1q_u32(VU->VF[Fs(code)].UL));
		const float32x4_t ft = VuFloatQNeon(vld1q_u32(VU->VF[Ft(code)].UL));
		float32x4_t fs_yzx = vextq_f32(fs, fs, 1);
		float32x4_t ft_zxy = vextq_f32(ft, ft, 2);
		fs_yzx = vsetq_lane_f32(vgetq_lane_f32(fs, 0), fs_yzx, 2);
		ft_zxy = vsetq_lane_f32(vgetq_lane_f32(ft, 0), ft_zxy, 1);
		ft_zxy = vsetq_lane_f32(vgetq_lane_f32(ft, 1), ft_zxy, 2);
		return vmulq_f32(fs_yzx, ft_zxy);
	}

	static inline bool ExecuteOpmulaNeon(VURegs* VU, u32 code)
	{
		const float32x4_t product = OuterProductNeon(VU, code);
		VU->ACC.UL[0] = UpdateMacLane(VU, 0, vgetq_lane_f32(product, 0));
		VU->ACC.UL[1] = UpdateMacLane(VU, 1, vgetq_lane_f32(product, 1));
		VU->ACC.UL[2] = UpdateMacLane(VU, 2, vgetq_lane_f32(product, 2));
		VU_STAT_UPDATE(VU);
#if defined(VITASX2_QEMU_VALIDATION)
		++::g_qemuVuUpperNeonQwordOps;
#endif
		return true;
	}

	static inline bool ExecuteOpmsubNeon(VURegs* VU, u32 code)
	{
		const float32x4_t result = vsubq_f32(VuFloatQNeon(vld1q_u32(VU->ACC.UL)), OuterProductNeon(VU, code));
		const unsigned fd = Fd(code);
		WriteMacResult(VU, false, fd, 0, UpdateMacLane(VU, 0, vgetq_lane_f32(result, 0)));
		WriteMacResult(VU, false, fd, 1, UpdateMacLane(VU, 1, vgetq_lane_f32(result, 1)));
		WriteMacResult(VU, false, fd, 2, UpdateMacLane(VU, 2, vgetq_lane_f32(result, 2)));
		VU_STAT_UPDATE(VU);
#if defined(VITASX2_QEMU_VALIDATION)
		++::g_qemuVuUpperNeonQwordOps;
#endif
		return true;
	}
#endif

	static inline bool TryExecuteAddSubVectorNeon(VURegs* VU, u32 code, bool acc, bool subtract)
	{
#if defined(ARCH_ARM32)
		return ExecuteAddSubMaskedNeon(VU, code, acc, subtract, vld1q_u32(VU->VF[Ft(code)].UL));
#else
		return false;
#endif
	}

	static inline bool TryExecuteAddSubBroadcastNeon(VURegs* VU, u32 code, bool acc, bool subtract, u32 operand_bits)
	{
#if defined(ARCH_ARM32)
		return ExecuteAddSubMaskedNeon(VU, code, acc, subtract, vdupq_n_u32(operand_bits));
#else
		return false;
#endif
	}

	static inline bool TryExecuteMulVectorNeon(VURegs* VU, u32 code, bool acc)
	{
#if defined(ARCH_ARM32)
		return ExecuteMulMaskedNeon(VU, code, acc, vld1q_u32(VU->VF[Ft(code)].UL));
#else
		return false;
#endif
	}

	static inline bool TryExecuteMulBroadcastNeon(VURegs* VU, u32 code, bool acc, u32 operand_bits)
	{
#if defined(ARCH_ARM32)
		return ExecuteMulMaskedNeon(VU, code, acc, vdupq_n_u32(operand_bits));
#else
		return false;
#endif
	}

	static inline bool TryExecuteMaddMsubVectorNeon(VURegs* VU, u32 code, bool acc, bool subtract)
	{
#if defined(ARCH_ARM32)
		return ExecuteMaddMsubMaskedNeon(VU, code, acc, subtract, vld1q_u32(VU->VF[Ft(code)].UL));
#else
		return false;
#endif
	}

	static inline bool TryExecuteMaddMsubBroadcastNeon(VURegs* VU, u32 code, bool acc, bool subtract, u32 operand_bits)
	{
#if defined(ARCH_ARM32)
		return ExecuteMaddMsubMaskedNeon(VU, code, acc, subtract, vdupq_n_u32(operand_bits));
#else
		return false;
#endif
	}

	static inline bool TryExecuteOpmulaNeon(VURegs* VU, u32 code)
	{
#if defined(ARCH_ARM32)
		return ExecuteOpmulaNeon(VU, code);
#else
		return false;
#endif
	}

	static inline bool TryExecuteOpmsubNeon(VURegs* VU, u32 code)
	{
#if defined(ARCH_ARM32)
		return ExecuteOpmsubNeon(VU, code);
#else
		return false;
#endif
	}

	template <typename Operand>
	static inline void ExecuteAddSubMasked(VURegs* VU, u32 code, bool acc, bool subtract, bool triace_add, Operand operand)
	{
		const unsigned fd = Fd(code);
		const unsigned fs = Fs(code);
		const unsigned mask = XYZW(code);

		for (unsigned lane = 0; lane < 4; lane++)
		{
			const unsigned lane_mask = 1u << (3 - lane);
			if ((mask & lane_mask) == 0)
			{
				ClearMacLane(VU, lane);
				continue;
			}

			const u32 fs_bits = VU->VF[fs].UL[lane];
			const u32 operand_bits = operand(lane);
			const float result = subtract ? (VuDouble(fs_bits) - VuDouble(operand_bits)) :
				(triace_add ? VuAddTriAceHack(fs_bits, operand_bits) : VuDouble(fs_bits) + VuDouble(operand_bits));
			WriteMacResult(VU, acc, fd, lane, UpdateMacLane(VU, lane, result));
		}

		VU_STAT_UPDATE(VU);
	}

	template <typename Operand>
	static inline void ExecuteMulMasked(VURegs* VU, u32 code, bool acc, Operand operand)
	{
		const unsigned fd = Fd(code);
		const unsigned fs = Fs(code);
		const unsigned mask = XYZW(code);

		for (unsigned lane = 0; lane < 4; lane++)
		{
			const unsigned lane_mask = 1u << (3 - lane);
			if ((mask & lane_mask) == 0)
			{
				ClearMacLane(VU, lane);
				continue;
			}

			const float result = VuDouble(VU->VF[fs].UL[lane]) * VuDouble(operand(lane));
			WriteMacResult(VU, acc, fd, lane, UpdateMacLane(VU, lane, result));
		}

		VU_STAT_UPDATE(VU);
	}

	template <typename Operand>
	static inline void ExecuteMaddMasked(VURegs* VU, u32 code, bool acc, Operand operand)
	{
		const unsigned fd = Fd(code);
		const unsigned fs = Fs(code);
		const unsigned mask = XYZW(code);

		for (unsigned lane = 0; lane < 4; lane++)
		{
			const unsigned lane_mask = 1u << (3 - lane);
			if ((mask & lane_mask) == 0)
			{
				ClearMacLane(VU, lane);
				continue;
			}

			const float result = VuDouble(VU->ACC.UL[lane]) +
				VuDouble(VU->VF[fs].UL[lane]) * VuDouble(operand(lane));
			WriteMacResult(VU, acc, fd, lane, UpdateMacLane(VU, lane, result));
		}

		VU_STAT_UPDATE(VU);
	}

	template <typename Operand>
	static inline void ExecuteMsubMasked(VURegs* VU, u32 code, bool acc, Operand operand)
	{
		const unsigned fd = Fd(code);
		const unsigned fs = Fs(code);
		const unsigned mask = XYZW(code);

		for (unsigned lane = 0; lane < 4; lane++)
		{
			const unsigned lane_mask = 1u << (3 - lane);
			if ((mask & lane_mask) == 0)
			{
				ClearMacLane(VU, lane);
				continue;
			}

			const float result = VuDouble(VU->ACC.UL[lane]) -
				VuDouble(VU->VF[fs].UL[lane]) * VuDouble(operand(lane));
			WriteMacResult(VU, acc, fd, lane, UpdateMacLane(VU, lane, result));
		}

		VU_STAT_UPDATE(VU);
	}

	static inline void ExecuteOpmula(VURegs* VU, u32 code)
	{
		const unsigned fs = Fs(code);
		const unsigned ft = Ft(code);

		const float ftx = VuDouble(VU->VF[ft].UL[0]);
		const float fty = VuDouble(VU->VF[ft].UL[1]);
		const float ftz = VuDouble(VU->VF[ft].UL[2]);
		const float fsx = VuDouble(VU->VF[fs].UL[0]);
		const float fsy = VuDouble(VU->VF[fs].UL[1]);
		const float fsz = VuDouble(VU->VF[fs].UL[2]);

		VU->ACC.UL[0] = UpdateMacLane(VU, 0, fsy * ftz);
		VU->ACC.UL[1] = UpdateMacLane(VU, 1, fsz * ftx);
		VU->ACC.UL[2] = UpdateMacLane(VU, 2, fsx * fty);
		VU_STAT_UPDATE(VU);
	}

	static inline void ExecuteOpmsub(VURegs* VU, u32 code)
	{
		const unsigned fd = Fd(code);
		const unsigned fs = Fs(code);
		const unsigned ft = Ft(code);

		const float ftx = VuDouble(VU->VF[ft].UL[0]);
		const float fty = VuDouble(VU->VF[ft].UL[1]);
		const float ftz = VuDouble(VU->VF[ft].UL[2]);
		const float fsx = VuDouble(VU->VF[fs].UL[0]);
		const float fsy = VuDouble(VU->VF[fs].UL[1]);
		const float fsz = VuDouble(VU->VF[fs].UL[2]);

		WriteMacResult(VU, false, fd, 0, UpdateMacLane(VU, 0, VuDouble(VU->ACC.UL[0]) - fsy * ftz));
		WriteMacResult(VU, false, fd, 1, UpdateMacLane(VU, 1, VuDouble(VU->ACC.UL[1]) - fsz * ftx));
		WriteMacResult(VU, false, fd, 2, UpdateMacLane(VU, 2, VuDouble(VU->ACC.UL[2]) - fsx * fty));
		VU_STAT_UPDATE(VU);
	}

	template <u32 Offset>
	static inline u32 FloatToIntBits(u32 bits)
	{
		float value = FloatFromBits(bits);
		if (Offset != 0)
			value *= FloatFromBits(0x3f800000u + (Offset << 23));
		bits = FloatToBits(value);

		if ((bits & 0x7f800000u) >= 0x4f000000u)
			return (bits & 0x80000000u) ? 0x80000000u : 0x7fffffffu;
		return static_cast<u32>(static_cast<s32>(value));
	}

	template <u32 Offset>
	static inline u32 IntToFloatBits(u32 bits)
	{
		float value = static_cast<float>(static_cast<s32>(bits));
		if (Offset != 0)
			value *= FloatFromBits(0x3f800000u - (Offset << 23));
		return FloatToBits(value);
	}

	static inline void ExecuteUpperNoLower(VURegs* VU, u32 code)
	{
		switch (DecodeUpper(code))
		{
			case UpperFastKind::NOP:
				return;
			case UpperFastKind::ABS:
				if (StoreAbsUpperMaskedNeon(VU, Ft(code), XYZW(code), Fs(code)))
					return;
				StoreUnaryUpperMasked(VU, Ft(code), XYZW(code), Fs(code), [](u32 bits) { return bits & 0x7fffffffu; });
				return;
			case UpperFastKind::FTOI0:
				if (StoreFtoiUpperMaskedNeon<0>(VU, Ft(code), XYZW(code), Fs(code)))
					return;
				StoreUnaryUpperMasked(VU, Ft(code), XYZW(code), Fs(code), FloatToIntBits<0>);
				return;
			case UpperFastKind::FTOI4:
				if (StoreFtoiUpperMaskedNeon<4>(VU, Ft(code), XYZW(code), Fs(code)))
					return;
				StoreUnaryUpperMasked(VU, Ft(code), XYZW(code), Fs(code), FloatToIntBits<4>);
				return;
			case UpperFastKind::FTOI12:
				if (StoreFtoiUpperMaskedNeon<12>(VU, Ft(code), XYZW(code), Fs(code)))
					return;
				StoreUnaryUpperMasked(VU, Ft(code), XYZW(code), Fs(code), FloatToIntBits<12>);
				return;
			case UpperFastKind::FTOI15:
				if (StoreFtoiUpperMaskedNeon<15>(VU, Ft(code), XYZW(code), Fs(code)))
					return;
				StoreUnaryUpperMasked(VU, Ft(code), XYZW(code), Fs(code), FloatToIntBits<15>);
				return;
			case UpperFastKind::ITOF0:
				if (StoreItofUpperMaskedNeon<0>(VU, Ft(code), XYZW(code), Fs(code)))
					return;
				StoreUnaryUpperMasked(VU, Ft(code), XYZW(code), Fs(code), IntToFloatBits<0>);
				return;
			case UpperFastKind::ITOF4:
				if (StoreItofUpperMaskedNeon<4>(VU, Ft(code), XYZW(code), Fs(code)))
					return;
				StoreUnaryUpperMasked(VU, Ft(code), XYZW(code), Fs(code), IntToFloatBits<4>);
				return;
			case UpperFastKind::ITOF12:
				if (StoreItofUpperMaskedNeon<12>(VU, Ft(code), XYZW(code), Fs(code)))
					return;
				StoreUnaryUpperMasked(VU, Ft(code), XYZW(code), Fs(code), IntToFloatBits<12>);
				return;
			case UpperFastKind::ITOF15:
				if (StoreItofUpperMaskedNeon<15>(VU, Ft(code), XYZW(code), Fs(code)))
					return;
				StoreUnaryUpperMasked(VU, Ft(code), XYZW(code), Fs(code), IntToFloatBits<15>);
				return;
			case UpperFastKind::ADD:
				if (TryExecuteAddSubVectorNeon(VU, code, false, false))
					return;
				ExecuteAddSubMasked(VU, code, false, false, false, [VU, code](unsigned lane) { return VU->VF[Ft(code)].UL[lane]; });
				return;
			case UpperFastKind::ADDi:
				if (!CHECK_VUADDSUBHACK && TryExecuteAddSubBroadcastNeon(VU, code, false, false, VU->VI[REG_I].UL))
					return;
				ExecuteAddSubMasked(VU, code, false, false, CHECK_VUADDSUBHACK, [VU](unsigned) { return VU->VI[REG_I].UL; });
				return;
			case UpperFastKind::ADDq:
				if (TryExecuteAddSubBroadcastNeon(VU, code, false, false, VU->VI[REG_Q].UL))
					return;
				ExecuteAddSubMasked(VU, code, false, false, false, [VU](unsigned) { return VU->VI[REG_Q].UL; });
				return;
			case UpperFastKind::ADDx:
				if (TryExecuteAddSubBroadcastNeon(VU, code, false, false, VU->VF[Ft(code)].UL[0]))
					return;
				ExecuteAddSubMasked(VU, code, false, false, false, [VU, code](unsigned) { return VU->VF[Ft(code)].UL[0]; });
				return;
			case UpperFastKind::ADDy:
				if (TryExecuteAddSubBroadcastNeon(VU, code, false, false, VU->VF[Ft(code)].UL[1]))
					return;
				ExecuteAddSubMasked(VU, code, false, false, false, [VU, code](unsigned) { return VU->VF[Ft(code)].UL[1]; });
				return;
			case UpperFastKind::ADDz:
				if (TryExecuteAddSubBroadcastNeon(VU, code, false, false, VU->VF[Ft(code)].UL[2]))
					return;
				ExecuteAddSubMasked(VU, code, false, false, false, [VU, code](unsigned) { return VU->VF[Ft(code)].UL[2]; });
				return;
			case UpperFastKind::ADDw:
				if (TryExecuteAddSubBroadcastNeon(VU, code, false, false, VU->VF[Ft(code)].UL[3]))
					return;
				ExecuteAddSubMasked(VU, code, false, false, false, [VU, code](unsigned) { return VU->VF[Ft(code)].UL[3]; });
				return;
			case UpperFastKind::ADDA:
				if (TryExecuteAddSubVectorNeon(VU, code, true, false))
					return;
				ExecuteAddSubMasked(VU, code, true, false, false, [VU, code](unsigned lane) { return VU->VF[Ft(code)].UL[lane]; });
				return;
			case UpperFastKind::ADDAi:
				if (TryExecuteAddSubBroadcastNeon(VU, code, true, false, VU->VI[REG_I].UL))
					return;
				ExecuteAddSubMasked(VU, code, true, false, false, [VU](unsigned) { return VU->VI[REG_I].UL; });
				return;
			case UpperFastKind::ADDAq:
				if (TryExecuteAddSubBroadcastNeon(VU, code, true, false, VU->VI[REG_Q].UL))
					return;
				ExecuteAddSubMasked(VU, code, true, false, false, [VU](unsigned) { return VU->VI[REG_Q].UL; });
				return;
			case UpperFastKind::ADDAx:
				if (TryExecuteAddSubBroadcastNeon(VU, code, true, false, VU->VF[Ft(code)].UL[0]))
					return;
				ExecuteAddSubMasked(VU, code, true, false, false, [VU, code](unsigned) { return VU->VF[Ft(code)].UL[0]; });
				return;
			case UpperFastKind::ADDAy:
				if (TryExecuteAddSubBroadcastNeon(VU, code, true, false, VU->VF[Ft(code)].UL[1]))
					return;
				ExecuteAddSubMasked(VU, code, true, false, false, [VU, code](unsigned) { return VU->VF[Ft(code)].UL[1]; });
				return;
			case UpperFastKind::ADDAz:
				if (TryExecuteAddSubBroadcastNeon(VU, code, true, false, VU->VF[Ft(code)].UL[2]))
					return;
				ExecuteAddSubMasked(VU, code, true, false, false, [VU, code](unsigned) { return VU->VF[Ft(code)].UL[2]; });
				return;
			case UpperFastKind::ADDAw:
				if (TryExecuteAddSubBroadcastNeon(VU, code, true, false, VU->VF[Ft(code)].UL[3]))
					return;
				ExecuteAddSubMasked(VU, code, true, false, false, [VU, code](unsigned) { return VU->VF[Ft(code)].UL[3]; });
				return;
			case UpperFastKind::SUB:
				if (TryExecuteAddSubVectorNeon(VU, code, false, true))
					return;
				ExecuteAddSubMasked(VU, code, false, true, false, [VU, code](unsigned lane) { return VU->VF[Ft(code)].UL[lane]; });
				return;
			case UpperFastKind::SUBi:
				if (TryExecuteAddSubBroadcastNeon(VU, code, false, true, VU->VI[REG_I].UL))
					return;
				ExecuteAddSubMasked(VU, code, false, true, false, [VU](unsigned) { return VU->VI[REG_I].UL; });
				return;
			case UpperFastKind::SUBq:
				if (TryExecuteAddSubBroadcastNeon(VU, code, false, true, VU->VI[REG_Q].UL))
					return;
				ExecuteAddSubMasked(VU, code, false, true, false, [VU](unsigned) { return VU->VI[REG_Q].UL; });
				return;
			case UpperFastKind::SUBx:
				if (TryExecuteAddSubBroadcastNeon(VU, code, false, true, VU->VF[Ft(code)].UL[0]))
					return;
				ExecuteAddSubMasked(VU, code, false, true, false, [VU, code](unsigned) { return VU->VF[Ft(code)].UL[0]; });
				return;
			case UpperFastKind::SUBy:
				if (TryExecuteAddSubBroadcastNeon(VU, code, false, true, VU->VF[Ft(code)].UL[1]))
					return;
				ExecuteAddSubMasked(VU, code, false, true, false, [VU, code](unsigned) { return VU->VF[Ft(code)].UL[1]; });
				return;
			case UpperFastKind::SUBz:
				if (TryExecuteAddSubBroadcastNeon(VU, code, false, true, VU->VF[Ft(code)].UL[2]))
					return;
				ExecuteAddSubMasked(VU, code, false, true, false, [VU, code](unsigned) { return VU->VF[Ft(code)].UL[2]; });
				return;
			case UpperFastKind::SUBw:
				if (TryExecuteAddSubBroadcastNeon(VU, code, false, true, VU->VF[Ft(code)].UL[3]))
					return;
				ExecuteAddSubMasked(VU, code, false, true, false, [VU, code](unsigned) { return VU->VF[Ft(code)].UL[3]; });
				return;
			case UpperFastKind::SUBA:
				if (TryExecuteAddSubVectorNeon(VU, code, true, true))
					return;
				ExecuteAddSubMasked(VU, code, true, true, false, [VU, code](unsigned lane) { return VU->VF[Ft(code)].UL[lane]; });
				return;
			case UpperFastKind::SUBAi:
				if (TryExecuteAddSubBroadcastNeon(VU, code, true, true, VU->VI[REG_I].UL))
					return;
				ExecuteAddSubMasked(VU, code, true, true, false, [VU](unsigned) { return VU->VI[REG_I].UL; });
				return;
			case UpperFastKind::SUBAq:
				if (TryExecuteAddSubBroadcastNeon(VU, code, true, true, VU->VI[REG_Q].UL))
					return;
				ExecuteAddSubMasked(VU, code, true, true, false, [VU](unsigned) { return VU->VI[REG_Q].UL; });
				return;
			case UpperFastKind::SUBAx:
				if (TryExecuteAddSubBroadcastNeon(VU, code, true, true, VU->VF[Ft(code)].UL[0]))
					return;
				ExecuteAddSubMasked(VU, code, true, true, false, [VU, code](unsigned) { return VU->VF[Ft(code)].UL[0]; });
				return;
			case UpperFastKind::SUBAy:
				if (TryExecuteAddSubBroadcastNeon(VU, code, true, true, VU->VF[Ft(code)].UL[1]))
					return;
				ExecuteAddSubMasked(VU, code, true, true, false, [VU, code](unsigned) { return VU->VF[Ft(code)].UL[1]; });
				return;
			case UpperFastKind::SUBAz:
				if (TryExecuteAddSubBroadcastNeon(VU, code, true, true, VU->VF[Ft(code)].UL[2]))
					return;
				ExecuteAddSubMasked(VU, code, true, true, false, [VU, code](unsigned) { return VU->VF[Ft(code)].UL[2]; });
				return;
			case UpperFastKind::SUBAw:
				if (TryExecuteAddSubBroadcastNeon(VU, code, true, true, VU->VF[Ft(code)].UL[3]))
					return;
				ExecuteAddSubMasked(VU, code, true, true, false, [VU, code](unsigned) { return VU->VF[Ft(code)].UL[3]; });
				return;
			case UpperFastKind::MAX:
				if (StoreMinMaxUpperMaskedNeon(VU, Fd(code), XYZW(code), Fs(code), Ft(code), true))
					return;
				StoreBinaryUpperMasked(VU, Fd(code), XYZW(code), Fs(code), Ft(code), FpMaxBits);
				return;
			case UpperFastKind::MAXi:
				if (StoreMinMaxUpperBroadcastMaskedNeon(VU, Fd(code), XYZW(code), Fs(code), VU->VI[REG_I].UL, true))
					return;
				StoreBinaryUpperBroadcastMasked(VU, Fd(code), XYZW(code), Fs(code), VU->VI[REG_I].UL, FpMaxBits);
				return;
			case UpperFastKind::MAXx:
				if (StoreMinMaxUpperBroadcastMaskedNeon(VU, Fd(code), XYZW(code), Fs(code), VU->VF[Ft(code)].UL[0], true))
					return;
				StoreBinaryUpperBroadcastMasked(VU, Fd(code), XYZW(code), Fs(code), VU->VF[Ft(code)].UL[0], FpMaxBits);
				return;
			case UpperFastKind::MAXy:
				if (StoreMinMaxUpperBroadcastMaskedNeon(VU, Fd(code), XYZW(code), Fs(code), VU->VF[Ft(code)].UL[1], true))
					return;
				StoreBinaryUpperBroadcastMasked(VU, Fd(code), XYZW(code), Fs(code), VU->VF[Ft(code)].UL[1], FpMaxBits);
				return;
			case UpperFastKind::MAXz:
				if (StoreMinMaxUpperBroadcastMaskedNeon(VU, Fd(code), XYZW(code), Fs(code), VU->VF[Ft(code)].UL[2], true))
					return;
				StoreBinaryUpperBroadcastMasked(VU, Fd(code), XYZW(code), Fs(code), VU->VF[Ft(code)].UL[2], FpMaxBits);
				return;
			case UpperFastKind::MAXw:
				if (StoreMinMaxUpperBroadcastMaskedNeon(VU, Fd(code), XYZW(code), Fs(code), VU->VF[Ft(code)].UL[3], true))
					return;
				StoreBinaryUpperBroadcastMasked(VU, Fd(code), XYZW(code), Fs(code), VU->VF[Ft(code)].UL[3], FpMaxBits);
				return;
			case UpperFastKind::MINI:
				if (StoreMinMaxUpperMaskedNeon(VU, Fd(code), XYZW(code), Fs(code), Ft(code), false))
					return;
				StoreBinaryUpperMasked(VU, Fd(code), XYZW(code), Fs(code), Ft(code), FpMinBits);
				return;
			case UpperFastKind::MINIi:
				if (StoreMinMaxUpperBroadcastMaskedNeon(VU, Fd(code), XYZW(code), Fs(code), VU->VI[REG_I].UL, false))
					return;
				StoreBinaryUpperBroadcastMasked(VU, Fd(code), XYZW(code), Fs(code), VU->VI[REG_I].UL, FpMinBits);
				return;
			case UpperFastKind::MINIx:
				if (StoreMinMaxUpperBroadcastMaskedNeon(VU, Fd(code), XYZW(code), Fs(code), VU->VF[Ft(code)].UL[0], false))
					return;
				StoreBinaryUpperBroadcastMasked(VU, Fd(code), XYZW(code), Fs(code), VU->VF[Ft(code)].UL[0], FpMinBits);
				return;
			case UpperFastKind::MINIy:
				if (StoreMinMaxUpperBroadcastMaskedNeon(VU, Fd(code), XYZW(code), Fs(code), VU->VF[Ft(code)].UL[1], false))
					return;
				StoreBinaryUpperBroadcastMasked(VU, Fd(code), XYZW(code), Fs(code), VU->VF[Ft(code)].UL[1], FpMinBits);
				return;
			case UpperFastKind::MINIz:
				if (StoreMinMaxUpperBroadcastMaskedNeon(VU, Fd(code), XYZW(code), Fs(code), VU->VF[Ft(code)].UL[2], false))
					return;
				StoreBinaryUpperBroadcastMasked(VU, Fd(code), XYZW(code), Fs(code), VU->VF[Ft(code)].UL[2], FpMinBits);
				return;
			case UpperFastKind::MINIw:
				if (StoreMinMaxUpperBroadcastMaskedNeon(VU, Fd(code), XYZW(code), Fs(code), VU->VF[Ft(code)].UL[3], false))
					return;
				StoreBinaryUpperBroadcastMasked(VU, Fd(code), XYZW(code), Fs(code), VU->VF[Ft(code)].UL[3], FpMinBits);
				return;
			case UpperFastKind::CLIP:
				ExecuteClip(VU, code);
				return;
			case UpperFastKind::OPMULA:
				if (TryExecuteOpmulaNeon(VU, code))
					return;
				ExecuteOpmula(VU, code);
				return;
			case UpperFastKind::OPMSUB:
				if (TryExecuteOpmsubNeon(VU, code))
					return;
				ExecuteOpmsub(VU, code);
				return;
			case UpperFastKind::MUL:
				if (TryExecuteMulVectorNeon(VU, code, false))
					return;
				ExecuteMulMasked(VU, code, false, [VU, code](unsigned lane) { return VU->VF[Ft(code)].UL[lane]; });
				return;
			case UpperFastKind::MULi:
				if (TryExecuteMulBroadcastNeon(VU, code, false, VU->VI[REG_I].UL))
					return;
				ExecuteMulMasked(VU, code, false, [VU](unsigned) { return VU->VI[REG_I].UL; });
				return;
			case UpperFastKind::MULq:
				if (TryExecuteMulBroadcastNeon(VU, code, false, VU->VI[REG_Q].UL))
					return;
				ExecuteMulMasked(VU, code, false, [VU](unsigned) { return VU->VI[REG_Q].UL; });
				return;
			case UpperFastKind::MULx:
				if (TryExecuteMulBroadcastNeon(VU, code, false, VU->VF[Ft(code)].UL[0]))
					return;
				ExecuteMulMasked(VU, code, false, [VU, code](unsigned) { return VU->VF[Ft(code)].UL[0]; });
				return;
			case UpperFastKind::MULy:
				if (TryExecuteMulBroadcastNeon(VU, code, false, VU->VF[Ft(code)].UL[1]))
					return;
				ExecuteMulMasked(VU, code, false, [VU, code](unsigned) { return VU->VF[Ft(code)].UL[1]; });
				return;
			case UpperFastKind::MULz:
				if (TryExecuteMulBroadcastNeon(VU, code, false, VU->VF[Ft(code)].UL[2]))
					return;
				ExecuteMulMasked(VU, code, false, [VU, code](unsigned) { return VU->VF[Ft(code)].UL[2]; });
				return;
			case UpperFastKind::MULw:
				if (TryExecuteMulBroadcastNeon(VU, code, false, VU->VF[Ft(code)].UL[3]))
					return;
				ExecuteMulMasked(VU, code, false, [VU, code](unsigned) { return VU->VF[Ft(code)].UL[3]; });
				return;
			case UpperFastKind::MULA:
				if (TryExecuteMulVectorNeon(VU, code, true))
					return;
				ExecuteMulMasked(VU, code, true, [VU, code](unsigned lane) { return VU->VF[Ft(code)].UL[lane]; });
				return;
			case UpperFastKind::MULAi:
				if (TryExecuteMulBroadcastNeon(VU, code, true, VU->VI[REG_I].UL))
					return;
				ExecuteMulMasked(VU, code, true, [VU](unsigned) { return VU->VI[REG_I].UL; });
				return;
			case UpperFastKind::MULAq:
				if (TryExecuteMulBroadcastNeon(VU, code, true, VU->VI[REG_Q].UL))
					return;
				ExecuteMulMasked(VU, code, true, [VU](unsigned) { return VU->VI[REG_Q].UL; });
				return;
			case UpperFastKind::MULAx:
				if (TryExecuteMulBroadcastNeon(VU, code, true, VU->VF[Ft(code)].UL[0]))
					return;
				ExecuteMulMasked(VU, code, true, [VU, code](unsigned) { return VU->VF[Ft(code)].UL[0]; });
				return;
			case UpperFastKind::MULAy:
				if (TryExecuteMulBroadcastNeon(VU, code, true, VU->VF[Ft(code)].UL[1]))
					return;
				ExecuteMulMasked(VU, code, true, [VU, code](unsigned) { return VU->VF[Ft(code)].UL[1]; });
				return;
			case UpperFastKind::MULAz:
				if (TryExecuteMulBroadcastNeon(VU, code, true, VU->VF[Ft(code)].UL[2]))
					return;
				ExecuteMulMasked(VU, code, true, [VU, code](unsigned) { return VU->VF[Ft(code)].UL[2]; });
				return;
			case UpperFastKind::MULAw:
				if (TryExecuteMulBroadcastNeon(VU, code, true, VU->VF[Ft(code)].UL[3]))
					return;
				ExecuteMulMasked(VU, code, true, [VU, code](unsigned) { return VU->VF[Ft(code)].UL[3]; });
				return;
			case UpperFastKind::MADD:
				if (TryExecuteMaddMsubVectorNeon(VU, code, false, false))
					return;
				ExecuteMaddMasked(VU, code, false, [VU, code](unsigned lane) { return VU->VF[Ft(code)].UL[lane]; });
				return;
			case UpperFastKind::MADDi:
				if (TryExecuteMaddMsubBroadcastNeon(VU, code, false, false, VU->VI[REG_I].UL))
					return;
				ExecuteMaddMasked(VU, code, false, [VU](unsigned) { return VU->VI[REG_I].UL; });
				return;
			case UpperFastKind::MADDq:
				if (TryExecuteMaddMsubBroadcastNeon(VU, code, false, false, VU->VI[REG_Q].UL))
					return;
				ExecuteMaddMasked(VU, code, false, [VU](unsigned) { return VU->VI[REG_Q].UL; });
				return;
			case UpperFastKind::MADDx:
				if (TryExecuteMaddMsubBroadcastNeon(VU, code, false, false, VU->VF[Ft(code)].UL[0]))
					return;
				ExecuteMaddMasked(VU, code, false, [VU, code](unsigned) { return VU->VF[Ft(code)].UL[0]; });
				return;
			case UpperFastKind::MADDy:
				if (TryExecuteMaddMsubBroadcastNeon(VU, code, false, false, VU->VF[Ft(code)].UL[1]))
					return;
				ExecuteMaddMasked(VU, code, false, [VU, code](unsigned) { return VU->VF[Ft(code)].UL[1]; });
				return;
			case UpperFastKind::MADDz:
				if (TryExecuteMaddMsubBroadcastNeon(VU, code, false, false, VU->VF[Ft(code)].UL[2]))
					return;
				ExecuteMaddMasked(VU, code, false, [VU, code](unsigned) { return VU->VF[Ft(code)].UL[2]; });
				return;
			case UpperFastKind::MADDw:
				if (TryExecuteMaddMsubBroadcastNeon(VU, code, false, false, VU->VF[Ft(code)].UL[3]))
					return;
				ExecuteMaddMasked(VU, code, false, [VU, code](unsigned) { return VU->VF[Ft(code)].UL[3]; });
				return;
			case UpperFastKind::MADDA:
				if (TryExecuteMaddMsubVectorNeon(VU, code, true, false))
					return;
				ExecuteMaddMasked(VU, code, true, [VU, code](unsigned lane) { return VU->VF[Ft(code)].UL[lane]; });
				return;
			case UpperFastKind::MADDAi:
				if (TryExecuteMaddMsubBroadcastNeon(VU, code, true, false, VU->VI[REG_I].UL))
					return;
				ExecuteMaddMasked(VU, code, true, [VU](unsigned) { return VU->VI[REG_I].UL; });
				return;
			case UpperFastKind::MADDAq:
				if (TryExecuteMaddMsubBroadcastNeon(VU, code, true, false, VU->VI[REG_Q].UL))
					return;
				ExecuteMaddMasked(VU, code, true, [VU](unsigned) { return VU->VI[REG_Q].UL; });
				return;
			case UpperFastKind::MADDAx:
				if (TryExecuteMaddMsubBroadcastNeon(VU, code, true, false, VU->VF[Ft(code)].UL[0]))
					return;
				ExecuteMaddMasked(VU, code, true, [VU, code](unsigned) { return VU->VF[Ft(code)].UL[0]; });
				return;
			case UpperFastKind::MADDAy:
				if (TryExecuteMaddMsubBroadcastNeon(VU, code, true, false, VU->VF[Ft(code)].UL[1]))
					return;
				ExecuteMaddMasked(VU, code, true, [VU, code](unsigned) { return VU->VF[Ft(code)].UL[1]; });
				return;
			case UpperFastKind::MADDAz:
				if (TryExecuteMaddMsubBroadcastNeon(VU, code, true, false, VU->VF[Ft(code)].UL[2]))
					return;
				ExecuteMaddMasked(VU, code, true, [VU, code](unsigned) { return VU->VF[Ft(code)].UL[2]; });
				return;
			case UpperFastKind::MADDAw:
				if (TryExecuteMaddMsubBroadcastNeon(VU, code, true, false, VU->VF[Ft(code)].UL[3]))
					return;
				ExecuteMaddMasked(VU, code, true, [VU, code](unsigned) { return VU->VF[Ft(code)].UL[3]; });
				return;
			case UpperFastKind::MSUB:
				if (TryExecuteMaddMsubVectorNeon(VU, code, false, true))
					return;
				ExecuteMsubMasked(VU, code, false, [VU, code](unsigned lane) { return VU->VF[Ft(code)].UL[lane]; });
				return;
			case UpperFastKind::MSUBi:
				if (TryExecuteMaddMsubBroadcastNeon(VU, code, false, true, VU->VI[REG_I].UL))
					return;
				ExecuteMsubMasked(VU, code, false, [VU](unsigned) { return VU->VI[REG_I].UL; });
				return;
			case UpperFastKind::MSUBq:
				if (TryExecuteMaddMsubBroadcastNeon(VU, code, false, true, VU->VI[REG_Q].UL))
					return;
				ExecuteMsubMasked(VU, code, false, [VU](unsigned) { return VU->VI[REG_Q].UL; });
				return;
			case UpperFastKind::MSUBx:
				if (TryExecuteMaddMsubBroadcastNeon(VU, code, false, true, VU->VF[Ft(code)].UL[0]))
					return;
				ExecuteMsubMasked(VU, code, false, [VU, code](unsigned) { return VU->VF[Ft(code)].UL[0]; });
				return;
			case UpperFastKind::MSUBy:
				if (TryExecuteMaddMsubBroadcastNeon(VU, code, false, true, VU->VF[Ft(code)].UL[1]))
					return;
				ExecuteMsubMasked(VU, code, false, [VU, code](unsigned) { return VU->VF[Ft(code)].UL[1]; });
				return;
			case UpperFastKind::MSUBz:
				if (TryExecuteMaddMsubBroadcastNeon(VU, code, false, true, VU->VF[Ft(code)].UL[2]))
					return;
				ExecuteMsubMasked(VU, code, false, [VU, code](unsigned) { return VU->VF[Ft(code)].UL[2]; });
				return;
			case UpperFastKind::MSUBw:
				if (TryExecuteMaddMsubBroadcastNeon(VU, code, false, true, VU->VF[Ft(code)].UL[3]))
					return;
				ExecuteMsubMasked(VU, code, false, [VU, code](unsigned) { return VU->VF[Ft(code)].UL[3]; });
				return;
			case UpperFastKind::MSUBA:
				if (TryExecuteMaddMsubVectorNeon(VU, code, true, true))
					return;
				ExecuteMsubMasked(VU, code, true, [VU, code](unsigned lane) { return VU->VF[Ft(code)].UL[lane]; });
				return;
			case UpperFastKind::MSUBAi:
				if (TryExecuteMaddMsubBroadcastNeon(VU, code, true, true, VU->VI[REG_I].UL))
					return;
				ExecuteMsubMasked(VU, code, true, [VU](unsigned) { return VU->VI[REG_I].UL; });
				return;
			case UpperFastKind::MSUBAq:
				if (TryExecuteMaddMsubBroadcastNeon(VU, code, true, true, VU->VI[REG_Q].UL))
					return;
				ExecuteMsubMasked(VU, code, true, [VU](unsigned) { return VU->VI[REG_Q].UL; });
				return;
			case UpperFastKind::MSUBAx:
				if (TryExecuteMaddMsubBroadcastNeon(VU, code, true, true, VU->VF[Ft(code)].UL[0]))
					return;
				ExecuteMsubMasked(VU, code, true, [VU, code](unsigned) { return VU->VF[Ft(code)].UL[0]; });
				return;
			case UpperFastKind::MSUBAy:
				if (TryExecuteMaddMsubBroadcastNeon(VU, code, true, true, VU->VF[Ft(code)].UL[1]))
					return;
				ExecuteMsubMasked(VU, code, true, [VU, code](unsigned) { return VU->VF[Ft(code)].UL[1]; });
				return;
			case UpperFastKind::MSUBAz:
				if (TryExecuteMaddMsubBroadcastNeon(VU, code, true, true, VU->VF[Ft(code)].UL[2]))
					return;
				ExecuteMsubMasked(VU, code, true, [VU, code](unsigned) { return VU->VF[Ft(code)].UL[2]; });
				return;
			case UpperFastKind::MSUBAw:
				if (TryExecuteMaddMsubBroadcastNeon(VU, code, true, true, VU->VF[Ft(code)].UL[3]))
					return;
				ExecuteMsubMasked(VU, code, true, [VU, code](unsigned) { return VU->VF[Ft(code)].UL[3]; });
				return;
			case UpperFastKind::None:
				return;
		}
	}

	static inline float VuDouble(u32 bits)
	{
#ifndef INT_VUDOUBLEHACK
		switch (bits & 0x7f800000u)
		{
			case 0x00000000u:
				bits &= 0x80000000u;
				return FloatFromBits(bits);
			case 0x7f800000u:
				if (CHECK_VU_OVERFLOW(0))
					return FloatFromBits((bits & 0x80000000u) | 0x7f7fffffu);
				break;
		}
#endif
		return FloatFromBits(bits);
	}

	static inline void WriteVuQ(VURegs* VU, float value)
	{
		VU->q.F = VuDouble(FloatToBits(value));
	}

	static inline float VuLane(VURegs* VU, unsigned reg, unsigned lane)
	{
		return VuDouble(VU->VF[reg].UL[lane]);
	}

	static inline float VuSumXYZSquares(VURegs* VU, unsigned reg)
	{
#if defined(ARCH_ARM32)
		return VuSumXYZSquaresNeon(VU, reg);
#else
		const float x = VuLane(VU, reg, 0);
		const float y = VuLane(VU, reg, 1);
		const float z = VuLane(VU, reg, 2);
		return x * x + y * y + z * z;
#endif
	}

	static inline float VuCalculateEatan(float input)
	{
		static constexpr float eatanconst[9] = {
			0.999999344348907f, -0.333298563957214f, 0.199465364217758f, -0.13085337519646f,
			0.096420042216778f, -0.055909886956215f, 0.021861229091883f, -0.004054057877511f,
			0.785398185253143f};

		float result = (eatanconst[0] * input) + (eatanconst[1] * std::pow(input, 3)) +
			(eatanconst[2] * std::pow(input, 5)) + (eatanconst[3] * std::pow(input, 7)) +
			(eatanconst[4] * std::pow(input, 9)) + (eatanconst[5] * std::pow(input, 11)) +
			(eatanconst[6] * std::pow(input, 13)) + (eatanconst[7] * std::pow(input, 15));
		result += eatanconst[8];
		return VuDouble(FloatToBits(result));
	}

	static inline void AdvanceR(VURegs* VU)
	{
		const int x = (VU->VI[REG_R].UL >> 4) & 1;
		const int y = (VU->VI[REG_R].UL >> 22) & 1;
		VU->VI[REG_R].UL <<= 1;
		VU->VI[REG_R].UL ^= x ^ y;
		VU->VI[REG_R].UL = (VU->VI[REG_R].UL & 0x007fffffu) | 0x3f800000u;
	}

	static inline s16 ReadViBranchOperand(VURegs* VU, unsigned reg)
	{
		u32 value = VU->VI[reg].US[0];
		if (VU->VIBackupCycles > 0 && VU->VIRegNumber == reg)
			value = VU->VIOldValue;
		return static_cast<s16>(static_cast<u16>(value));
	}

	static inline u32 BranchAddress(VURegs* VU, u32 code)
	{
		s32 bpc = VU->VI[REG_TPC].SL + (static_cast<s32>(Imm11(code)) * 8);
		bpc &= (VU == &VU1) ? VU1_PROGMASK : VU0_PROGMASK;
		return static_cast<u32>(bpc);
	}

	static inline void SetBranch(VURegs* VU, u32 bpc)
	{
		if (VU->branch == 1)
		{
			VU->delaybranchpc = bpc;
			VU->takedelaybranch = true;
		}
		else
		{
			VU->branch = 2;
			VU->branchpc = bpc;
		}
	}

	static inline void WriteBranchLink(VURegs* VU, unsigned reg)
	{
		if (reg == 0)
			return;

		if (VU->branch == 1)
			VU->VI[reg].US[0] = static_cast<u16>((VU->branchpc + 8) / 8);
		else
			VU->VI[reg].US[0] = static_cast<u16>((VU->VI[REG_TPC].UL + 8) / 8);
	}

	static inline void ExecuteLowerNoUpper(VURegs* VU, u32 code)
	{
		switch (DecodeLower(code))
		{
			case LowerFastKind::LQ:
			{
				const u16 addr = static_cast<u16>((Imm11(code) + VU->VI[Is(code)].SS[0]) * 16);
				LoadVfMasked(VU, Ft(code), XYZW(code), VuMemQword(VU, addr));
				return;
			}
			case LowerFastKind::SQ:
			{
				const u16 addr = static_cast<u16>((Imm11(code) + VU->VI[It(code)].SS[0]) * 16);
				StoreVfMasked(VU, Fs(code), XYZW(code), VuMemQword(VU, addr));
				return;
			}
			case LowerFastKind::ILW:
			{
				if (It(code) == 0)
					return;
				const u16 addr = static_cast<u16>((Imm11(code) + VU->VI[Is(code)].SS[0]) * 16);
				LoadViHalfwordFromMemoryMasked(VU, It(code), XYZW(code),
					reinterpret_cast<const u16*>(VuMemQword(VU, addr)));
				return;
			}
			case LowerFastKind::ISW:
			{
				const u16 addr = static_cast<u16>((Imm11(code) + VU->VI[Is(code)].SS[0]) * 16);
				u16* ptr = reinterpret_cast<u16*>(VuMemQword(VU, addr));
#if defined(ARCH_ARM32)
				if (XYZW(code) != 0)
				{
					StoreViHalfwordToMemoryQwordMasked(VU->VI[It(code)].US[0], XYZW(code), ptr);
					return;
				}
#endif
				if (XYZW(code) & 0x8) { ptr[0] = VU->VI[It(code)].US[0]; ptr[1] = 0; }
				if (XYZW(code) & 0x4) { ptr[2] = VU->VI[It(code)].US[0]; ptr[3] = 0; }
				if (XYZW(code) & 0x2) { ptr[4] = VU->VI[It(code)].US[0]; ptr[5] = 0; }
				if (XYZW(code) & 0x1) { ptr[6] = VU->VI[It(code)].US[0]; ptr[7] = 0; }
				return;
			}
			case LowerFastKind::IADDIU:
				if (It(code) != 0)
				{
					_vuBackupVI(VU, It(code));
					VU->VI[It(code)].SS[0] = static_cast<s16>(VU->VI[Is(code)].SS[0] + Imm15(code));
				}
				return;
			case LowerFastKind::ISUBIU:
				if (It(code) != 0)
				{
					_vuBackupVI(VU, It(code));
					VU->VI[It(code)].SS[0] = static_cast<s16>(VU->VI[Is(code)].SS[0] - Imm15(code));
				}
				return;
			case LowerFastKind::IADD:
				if (Id(code) != 0)
				{
					_vuBackupVI(VU, Id(code));
					VU->VI[Id(code)].SS[0] = static_cast<s16>(VU->VI[Is(code)].SS[0] + VU->VI[It(code)].SS[0]);
				}
				return;
			case LowerFastKind::ISUB:
				if (Id(code) != 0)
				{
					_vuBackupVI(VU, Id(code));
					VU->VI[Id(code)].SS[0] = static_cast<s16>(VU->VI[Is(code)].SS[0] - VU->VI[It(code)].SS[0]);
				}
				return;
			case LowerFastKind::IADDI:
				if (It(code) != 0)
				{
					_vuBackupVI(VU, It(code));
					VU->VI[It(code)].SS[0] = static_cast<s16>(VU->VI[Is(code)].SS[0] + Imm5(code));
				}
				return;
			case LowerFastKind::IAND:
				if (Id(code) != 0)
				{
					_vuBackupVI(VU, Id(code));
					VU->VI[Id(code)].US[0] = VU->VI[Is(code)].US[0] & VU->VI[It(code)].US[0];
				}
				return;
			case LowerFastKind::IOR:
				if (Id(code) != 0)
				{
					_vuBackupVI(VU, Id(code));
					VU->VI[Id(code)].US[0] = VU->VI[Is(code)].US[0] | VU->VI[It(code)].US[0];
				}
				return;
			case LowerFastKind::FCAND:
				VU->VI[1].US[0] = ((VU->VI[REG_CLIP_FLAG].UL & 0x00ffffffu) & (code & 0x00ffffffu)) ? 1 : 0;
				return;
			case LowerFastKind::FCSET:
				VU->clipflag = code & 0x00ffffffu;
				return;
			case LowerFastKind::FCEQ:
				VU->VI[1].US[0] = ((VU->VI[REG_CLIP_FLAG].UL & 0x00ffffffu) == (code & 0x00ffffffu)) ? 1 : 0;
				return;
			case LowerFastKind::FCOR:
				VU->VI[1].US[0] = (((VU->VI[REG_CLIP_FLAG].UL & 0x00ffffffu) | (code & 0x00ffffffu)) == 0x00ffffffu) ? 1 : 0;
				return;
			case LowerFastKind::FSEQ:
				if (It(code) != 0)
					VU->VI[It(code)].US[0] = ((VU->VI[REG_STATUS_FLAG].US[0] & 0x0fffu) == FlagImm12(code)) ? 1 : 0;
				return;
			case LowerFastKind::FSSET:
				VU->statusflag = (FlagImm12(code) & 0x0fc0u) | (VU->statusflag & 0x003fu);
				return;
			case LowerFastKind::FSAND:
				if (It(code) != 0)
					VU->VI[It(code)].US[0] = (VU->VI[REG_STATUS_FLAG].US[0] & 0x0fffu) & FlagImm12(code);
				return;
			case LowerFastKind::FSOR:
				if (It(code) != 0)
					VU->VI[It(code)].US[0] = (VU->VI[REG_STATUS_FLAG].US[0] & 0x0fffu) | FlagImm12(code);
				return;
			case LowerFastKind::FMEQ:
				if (It(code) != 0)
					VU->VI[It(code)].US[0] = ((VU->VI[REG_MAC_FLAG].UL & 0xffffu) == VU->VI[Is(code)].US[0]) ? 1 : 0;
				return;
			case LowerFastKind::FMAND:
				if (It(code) != 0)
					VU->VI[It(code)].US[0] = VU->VI[Is(code)].US[0] & (VU->VI[REG_MAC_FLAG].UL & 0xffffu);
				return;
			case LowerFastKind::FMOR:
				if (It(code) != 0)
					VU->VI[It(code)].US[0] = (VU->VI[REG_MAC_FLAG].UL & 0xffffu) | VU->VI[Is(code)].US[0];
				return;
			case LowerFastKind::FCGET:
				if (It(code) != 0)
					VU->VI[It(code)].US[0] = VU->VI[REG_CLIP_FLAG].UL & 0x0fffu;
				return;
			case LowerFastKind::LQI:
				_vuBackupVI(VU, Is(code));
				if (Ft(code) != 0)
					LoadVfMasked(VU, Ft(code), XYZW(code), VuMemQword(VU, VU->VI[Is(code)].US[0] * 16));
				if (Fs(code) != 0)
					VU->VI[Is(code)].US[0]++;
				return;
			case LowerFastKind::LQD:
				_vuBackupVI(VU, Is(code));
				if (Is(code) != 0)
					VU->VI[Is(code)].US[0]--;
				if (Ft(code) != 0)
					LoadVfMasked(VU, Ft(code), XYZW(code), VuMemQword(VU, VU->VI[Is(code)].US[0] * 16));
				return;
			case LowerFastKind::SQI:
				_vuBackupVI(VU, It(code));
				StoreVfMasked(VU, Fs(code), XYZW(code), VuMemQword(VU, VU->VI[It(code)].US[0] * 16));
				if (Ft(code) != 0)
					VU->VI[It(code)].US[0]++;
				return;
			case LowerFastKind::SQD:
				_vuBackupVI(VU, It(code));
				if (Ft(code) != 0)
					VU->VI[It(code)].US[0]--;
				StoreVfMasked(VU, Fs(code), XYZW(code), VuMemQword(VU, VU->VI[It(code)].US[0] * 16));
				return;
			case LowerFastKind::ILWR:
			{
				if (It(code) == 0)
					return;
				LoadViHalfwordFromMemoryMasked(VU, It(code), XYZW(code),
					reinterpret_cast<const u16*>(VuMemQword(VU, VU->VI[Is(code)].US[0] * 16)));
				return;
			}
			case LowerFastKind::ISWR:
			{
				u16* ptr = reinterpret_cast<u16*>(VuMemQword(VU, VU->VI[Is(code)].US[0] * 16));
#if defined(ARCH_ARM32)
				if (XYZW(code) != 0)
				{
					StoreViHalfwordToMemoryQwordMasked(VU->VI[It(code)].US[0], XYZW(code), ptr);
					return;
				}
#endif
				if (XYZW(code) & 0x8) { ptr[0] = VU->VI[It(code)].US[0]; ptr[1] = 0; }
				if (XYZW(code) & 0x4) { ptr[2] = VU->VI[It(code)].US[0]; ptr[3] = 0; }
				if (XYZW(code) & 0x2) { ptr[4] = VU->VI[It(code)].US[0]; ptr[5] = 0; }
				if (XYZW(code) & 0x1) { ptr[6] = VU->VI[It(code)].US[0]; ptr[7] = 0; }
				return;
			}
			case LowerFastKind::MOVE:
				if (Ft(code) != 0)
					LoadVfMasked(VU, Ft(code), XYZW(code), VU->VF[Fs(code)].UL);
				return;
			case LowerFastKind::MR32:
				if (Ft(code) != 0)
				{
#if defined(ARCH_ARM32)
					if (XYZW(code) != 0)
					{
						StoreMr32Masked(VU, Ft(code), XYZW(code), Fs(code));
						return;
					}
#endif
					const u32 tx = VU->VF[Fs(code)].i.x;
					if (XYZW(code) & 0x8) VU->VF[Ft(code)].i.x = VU->VF[Fs(code)].i.y;
					if (XYZW(code) & 0x4) VU->VF[Ft(code)].i.y = VU->VF[Fs(code)].i.z;
					if (XYZW(code) & 0x2) VU->VF[Ft(code)].i.z = VU->VF[Fs(code)].i.w;
					if (XYZW(code) & 0x1) VU->VF[Ft(code)].i.w = tx;
				}
				return;
			case LowerFastKind::MFIR:
				StoreViToVectorMasked(VU, Ft(code), XYZW(code), static_cast<s32>(VU->VI[Is(code)].SS[0]));
				return;
			case LowerFastKind::MTIR:
				if (It(code) != 0)
				{
					_vuBackupVI(VU, It(code));
					VU->VI[It(code)].US[0] = static_cast<u16>(VU->VF[Fs(code)].UL[Fsf(code)] & 0xffffu);
				}
				return;
			case LowerFastKind::RINIT:
				VU->VI[REG_R].UL = 0x3f800000u | (VU->VF[Fs(code)].UL[Fsf(code)] & 0x007fffffu);
				return;
			case LowerFastKind::RGET:
				FillVfMasked(VU, Ft(code), XYZW(code), VU->VI[REG_R].UL);
				return;
			case LowerFastKind::RNEXT:
				if (Ft(code) != 0)
				{
					AdvanceR(VU);
					FillVfMasked(VU, Ft(code), XYZW(code), VU->VI[REG_R].UL);
				}
				return;
			case LowerFastKind::RXOR:
				VU->VI[REG_R].UL = 0x3f800000u | ((VU->VI[REG_R].UL ^ VU->VF[Fs(code)].UL[Fsf(code)]) & 0x007fffffu);
				return;
			case LowerFastKind::IBEQ:
				if (ReadViBranchOperand(VU, It(code)) == ReadViBranchOperand(VU, Is(code)))
					SetBranch(VU, BranchAddress(VU, code));
				return;
			case LowerFastKind::IBNE:
				if (ReadViBranchOperand(VU, It(code)) != ReadViBranchOperand(VU, Is(code)))
					SetBranch(VU, BranchAddress(VU, code));
				return;
			case LowerFastKind::IBLTZ:
				if (ReadViBranchOperand(VU, Is(code)) < 0)
					SetBranch(VU, BranchAddress(VU, code));
				return;
			case LowerFastKind::IBGTZ:
				if (ReadViBranchOperand(VU, Is(code)) > 0)
					SetBranch(VU, BranchAddress(VU, code));
				return;
			case LowerFastKind::IBLEZ:
				if (ReadViBranchOperand(VU, Is(code)) <= 0)
					SetBranch(VU, BranchAddress(VU, code));
				return;
			case LowerFastKind::IBGEZ:
				if (ReadViBranchOperand(VU, Is(code)) >= 0)
					SetBranch(VU, BranchAddress(VU, code));
				return;
			case LowerFastKind::B:
				SetBranch(VU, BranchAddress(VU, code));
				return;
			case LowerFastKind::BAL:
				WriteBranchLink(VU, It(code));
				SetBranch(VU, BranchAddress(VU, code));
				return;
			case LowerFastKind::JR:
				SetBranch(VU, VU->VI[Is(code)].US[0] * 8);
				return;
			case LowerFastKind::JALR:
				WriteBranchLink(VU, It(code));
				SetBranch(VU, VU->VI[Is(code)].US[0] * 8);
				return;
			case LowerFastKind::MFP:
				FillVfMasked(VU, Ft(code), XYZW(code), VU->VI[REG_P].UL);
				return;
			case LowerFastKind::WAITQ:
			case LowerFastKind::WAITP:
				return;
			case LowerFastKind::XITOP:
				if (It(code) != 0)
				{
					if (VU == &VU1 && THREAD_VU1)
						VU->VI[It(code)].US[0] = static_cast<u16>(vu1Thread.vifRegs.itop);
					else
						VU->VI[It(code)].US[0] = static_cast<u16>(VU->GetVifRegs().itop);
				}
				return;
			case LowerFastKind::XTOP:
				if (VU == &VU1 && It(code) != 0)
				{
					if (THREAD_VU1)
						VU->VI[It(code)].US[0] = static_cast<u16>(vu1Thread.vifRegs.top);
					else
						VU->VI[It(code)].US[0] = static_cast<u16>(VU->GetVifRegs().top);
				}
				return;
			case LowerFastKind::DIV:
			{
				const unsigned ft_lane = (code >> 23) & 0x03;
				const float ft = VuDouble(VU->VF[Ft(code)].UL[ft_lane]);
				const float fs = VuDouble(VU->VF[Fs(code)].UL[Fsf(code)]);

				VU->statusflag &= ~0x30u;
				if (ft == 0.0f)
				{
					VU->statusflag |= (fs == 0.0f) ? 0x10u : 0x20u;
					VU->q.UL = ((VU->VF[Ft(code)].UL[ft_lane] & 0x80000000u) ^
						(VU->VF[Fs(code)].UL[Fsf(code)] & 0x80000000u)) ? 0xff7fffffu : 0x7f7fffffu;
				}
				else
				{
					WriteVuQ(VU, fs / ft);
				}
				return;
			}
			case LowerFastKind::SQRT:
			{
				const unsigned ft_lane = (code >> 23) & 0x03;
				const float ft = VuDouble(VU->VF[Ft(code)].UL[ft_lane]);

				VU->statusflag &= ~0x30u;
				if (ft < 0.0f)
					VU->statusflag |= 0x10u;
				WriteVuQ(VU, std::sqrt(std::fabs(ft)));
				return;
			}
			case LowerFastKind::RSQRT:
			{
				const unsigned ft_lane = (code >> 23) & 0x03;
				const float ft = VuDouble(VU->VF[Ft(code)].UL[ft_lane]);
				const float fs = VuDouble(VU->VF[Fs(code)].UL[Fsf(code)]);

				VU->statusflag &= ~0x30u;
				if (ft == 0.0f)
				{
					VU->statusflag |= 0x20u;
					const bool negative = ((VU->VF[Ft(code)].UL[ft_lane] & 0x80000000u) ^
						(VU->VF[Fs(code)].UL[Fsf(code)] & 0x80000000u)) != 0;
					if (fs != 0.0f)
					{
						VU->q.UL = negative ? 0xff7fffffu : 0x7f7fffffu;
					}
					else
					{
						VU->q.UL = negative ? 0x80000000u : 0;
						VU->statusflag |= 0x10u;
					}
				}
				else
				{
					if (ft < 0.0f)
						VU->statusflag |= 0x10u;
					WriteVuQ(VU, fs / std::sqrt(std::fabs(ft)));
				}
				return;
			}
			case LowerFastKind::XGKICK:
			{
				if (VU != &VU1)
					return;

				if (VU->xgkickenable)
					_vuXGKICKTransfer(0, true);

				const u32 addr = (VU->VI[Is(code)].US[0] & 0x3ffu) * 16;
				VU->xgkickenable = true;
				VU->xgkickaddr = addr;
				VU->xgkickdiff = 0x4000 - addr;
				VU->xgkicksizeremaining = 0;
				VU->xgkickendpacket = false;
				VU->xgkicklastcycle = VU->cycle;
				VU->xgkickcyclecount = 1;
				VU0.VI[REG_VPU_STAT].UL |= (1 << 12);
				return;
			}
			case LowerFastKind::ESADD:
				VU->p.F = VuSumXYZSquares(VU, Fs(code));
				return;
			case LowerFastKind::ERSADD:
			{
				float p = VuSumXYZSquares(VU, Fs(code));
				if (p != 0.0f)
					p = 1.0f / p;
				VU->p.F = p;
				return;
			}
			case LowerFastKind::ELENG:
			{
				float p = VuSumXYZSquares(VU, Fs(code));
				if (p >= 0.0f)
					p = std::sqrt(p);
				VU->p.F = p;
				return;
			}
			case LowerFastKind::ERLENG:
			{
				float p = VuSumXYZSquares(VU, Fs(code));
				if (p >= 0.0f)
				{
					p = std::sqrt(p);
					if (p != 0.0f)
						p = 1.0f / p;
				}
				VU->p.F = p;
				return;
			}
			case LowerFastKind::EATANxy:
			{
				float p = 0.0f;
				const float x = VuLane(VU, Fs(code), 0);
				if (x != 0.0f)
					p = VuCalculateEatan(VuLane(VU, Fs(code), 1) / x);
				VU->p.F = p;
				return;
			}
			case LowerFastKind::EATANxz:
			{
				float p = 0.0f;
				const float x = VuLane(VU, Fs(code), 0);
				if (x != 0.0f)
					p = VuCalculateEatan(VuLane(VU, Fs(code), 2) / x);
				VU->p.F = p;
				return;
			}
			case LowerFastKind::ESUM:
				VU->p.F = VuLane(VU, Fs(code), 0) + VuLane(VU, Fs(code), 1) +
					VuLane(VU, Fs(code), 2) + VuLane(VU, Fs(code), 3);
				return;
			case LowerFastKind::ERCPR:
			{
				float p = VuLane(VU, Fs(code), Fsf(code));
				if (p != 0.0f)
					p = static_cast<float>(1.0 / p);
				VU->p.F = p;
				return;
			}
			case LowerFastKind::ESQRT:
			{
				float p = VuLane(VU, Fs(code), Fsf(code));
				if (p >= 0.0f)
					p = std::sqrt(p);
				VU->p.F = p;
				return;
			}
			case LowerFastKind::ERSQRT:
			{
				float p = VuLane(VU, Fs(code), Fsf(code));
				if (p >= 0.0f)
				{
					p = std::sqrt(p);
					if (p != 0.0f)
						p = 1.0f / p;
				}
				VU->p.F = p;
				return;
			}
			case LowerFastKind::ESIN:
			{
				static constexpr float sinconsts[5] = {
					1.0f, -0.166666567325592f, 0.008333025500178f, -0.000198074136279f, 0.000002601886990f};
				float p = VuLane(VU, Fs(code), Fsf(code));
				p = (sinconsts[0] * p) + (sinconsts[1] * std::pow(p, 3)) +
					(sinconsts[2] * std::pow(p, 5)) + (sinconsts[3] * std::pow(p, 7)) +
					(sinconsts[4] * std::pow(p, 9));
				VU->p.F = VuDouble(FloatToBits(p));
				return;
			}
			case LowerFastKind::EATAN:
				VU->p.F = VuCalculateEatan(VuLane(VU, Fs(code), Fsf(code)));
				return;
			case LowerFastKind::EEXP:
			{
				static constexpr float consts[6] = {
					0.249998688697815f, 0.031257584691048f, 0.002591371303424f,
					0.000171562001924f, 0.000005430199963f, 0.000000690600018f};
				float p = VuLane(VU, Fs(code), Fsf(code));
				p = 1.0f + (consts[0] * p) + (consts[1] * std::pow(p, 2)) +
					(consts[2] * std::pow(p, 3)) + (consts[3] * std::pow(p, 4)) +
					(consts[4] * std::pow(p, 5)) + (consts[5] * std::pow(p, 6));
				p = std::pow(p, 4);
				p = VuDouble(FloatToBits(p));
				p = 1.0f / p;
				VU->p.F = p;
				return;
			}
			case LowerFastKind::None:
				return;
		}
	}
} // namespace VUInterpFast
