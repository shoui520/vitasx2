// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "Config.h"
#include "MTVU.h"
#include "VUflags.h"
#include "VUmicro.h"

#include <cmath>
#include <cstring>

extern void _vuBackupVI(VURegs* VU, u32 reg);
extern void _vuXGKICKTransfer(s32 cycles, bool flush);
extern u32* GET_VU_MEM(VURegs* VU, u32 addr);

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
		ABS,
		FTOI0,
		FTOI4,
		FTOI12,
		FTOI15,
		ITOF0,
		ITOF4,
		ITOF12,
		ITOF15,
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
			case 0x08:
				return UpperFastKind::MADDx;
			case 0x09:
				return UpperFastKind::MADDy;
			case 0x0a:
				return UpperFastKind::MADDz;
			case 0x0b:
				return UpperFastKind::MADDw;
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
			case 0x21:
				return UpperFastKind::MADDq;
			case 0x23:
				return UpperFastKind::MADDi;
			case 0x29:
				return UpperFastKind::MADD;
			case 0x2a:
				return UpperFastKind::MUL;
			case 0x2b:
				return UpperFastKind::MAX;
			case 0x2f:
				return UpperFastKind::MINI;
			case 0x3c:
				switch ((code >> 6) & 0x1f)
				{
					case 0x02:
						return UpperFastKind::MADDAx;
					case 0x04:
						return UpperFastKind::ITOF0;
					case 0x05:
						return UpperFastKind::FTOI0;
					case 0x06:
						return UpperFastKind::MULAx;
					case 0x07:
						return UpperFastKind::MULAq;
					default:
						return UpperFastKind::None;
				}
			case 0x3d:
				switch ((code >> 6) & 0x1f)
				{
					case 0x02:
						return UpperFastKind::MADDAy;
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
					case 0x0a:
						return UpperFastKind::MADDA;
					default:
						return UpperFastKind::None;
				}
			case 0x3e:
				switch ((code >> 6) & 0x1f)
				{
					case 0x02:
						return UpperFastKind::MADDAz;
					case 0x04:
						return UpperFastKind::ITOF12;
					case 0x05:
						return UpperFastKind::FTOI12;
					case 0x06:
						return UpperFastKind::MULAz;
					case 0x07:
						return UpperFastKind::MULAi;
					case 0x0a:
						return UpperFastKind::MULA;
					default:
						return UpperFastKind::None;
				}
			case 0x3f:
				switch ((code >> 6) & 0x1f)
				{
					case 0x02:
						return UpperFastKind::MADDAw;
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

	static inline bool AnalyzeUpperNoLower(u32 code, _VURegsNum* regs)
	{
		const UpperFastKind kind = DecodeUpper(code);
		if (kind == UpperFastKind::None)
			return false;

		*regs = {};

		switch (kind)
		{
			case UpperFastKind::MAX:
			case UpperFastKind::MINI:
				AnalyzeUpperFdfsft(code, XYZW(code), regs);
				return true;
			case UpperFastKind::MUL:
				AnalyzeUpperFdfsft(code, XYZW(code), regs);
				return true;
			case UpperFastKind::MADD:
				AnalyzeUpperFdfsftReadAcc(code, XYZW(code), regs);
				return true;
			case UpperFastKind::MAXi:
			case UpperFastKind::MINIi:
				AnalyzeUpperFdfsi(code, regs);
				return true;
			case UpperFastKind::MULi:
				AnalyzeUpperFdfsi(code, regs);
				return true;
			case UpperFastKind::MADDi:
				AnalyzeUpperFdfsiReadAcc(code, regs);
				return true;
			case UpperFastKind::MULq:
				AnalyzeUpperFdfsq(code, regs);
				return true;
			case UpperFastKind::MADDq:
				AnalyzeUpperFdfsqReadAcc(code, regs);
				return true;
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
			case UpperFastKind::CLIP:
				AnalyzeUpperClip(code, regs);
				return true;
			case UpperFastKind::MULA:
				AnalyzeUpperAccFsFt(code, false, regs);
				return true;
			case UpperFastKind::MULAi:
				AnalyzeUpperAccFsI(code, false, regs);
				return true;
			case UpperFastKind::MULAq:
				AnalyzeUpperAccFsQ(code, false, regs);
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
				AnalyzeUpperAccFsFt(code, true, regs);
				return true;
			case UpperFastKind::MADDAi:
				AnalyzeUpperAccFsI(code, true, regs);
				return true;
			case UpperFastKind::MADDAq:
				AnalyzeUpperAccFsQ(code, true, regs);
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

	static inline void LoadVfMasked(VURegs* VU, unsigned ft, unsigned mask, const u32* ptr)
	{
		if (ft == 0)
			return;
		if (mask & 0x8) VU->VF[ft].UL[0] = ptr[0];
		if (mask & 0x4) VU->VF[ft].UL[1] = ptr[1];
		if (mask & 0x2) VU->VF[ft].UL[2] = ptr[2];
		if (mask & 0x1) VU->VF[ft].UL[3] = ptr[3];
	}

	static inline void StoreVfMasked(VURegs* VU, unsigned fs, unsigned mask, u32* ptr)
	{
		if (mask & 0x8) ptr[0] = VU->VF[fs].UL[0];
		if (mask & 0x4) ptr[1] = VU->VF[fs].UL[1];
		if (mask & 0x2) ptr[2] = VU->VF[fs].UL[2];
		if (mask & 0x1) ptr[3] = VU->VF[fs].UL[3];
	}

	static inline void FillVfMasked(VURegs* VU, unsigned ft, unsigned mask, u32 value)
	{
		if (ft == 0)
			return;
		if (mask & 0x8) VU->VF[ft].UL[0] = value;
		if (mask & 0x4) VU->VF[ft].UL[1] = value;
		if (mask & 0x2) VU->VF[ft].UL[2] = value;
		if (mask & 0x1) VU->VF[ft].UL[3] = value;
	}

	static inline void StoreViToVectorMasked(VURegs* VU, unsigned ft, unsigned mask, s32 value)
	{
		if (ft == 0)
			return;
		if (mask & 0x8) VU->VF[ft].SL[0] = value;
		if (mask & 0x4) VU->VF[ft].SL[1] = value;
		if (mask & 0x2) VU->VF[ft].SL[2] = value;
		if (mask & 0x1) VU->VF[ft].SL[3] = value;
	}

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

	static inline void ExecuteClip(VURegs* VU, u32 code)
	{
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
			case UpperFastKind::ABS:
				StoreUnaryUpperMasked(VU, Ft(code), XYZW(code), Fs(code), [](u32 bits) { return bits & 0x7fffffffu; });
				return;
			case UpperFastKind::FTOI0:
				StoreUnaryUpperMasked(VU, Ft(code), XYZW(code), Fs(code), FloatToIntBits<0>);
				return;
			case UpperFastKind::FTOI4:
				StoreUnaryUpperMasked(VU, Ft(code), XYZW(code), Fs(code), FloatToIntBits<4>);
				return;
			case UpperFastKind::FTOI12:
				StoreUnaryUpperMasked(VU, Ft(code), XYZW(code), Fs(code), FloatToIntBits<12>);
				return;
			case UpperFastKind::FTOI15:
				StoreUnaryUpperMasked(VU, Ft(code), XYZW(code), Fs(code), FloatToIntBits<15>);
				return;
			case UpperFastKind::ITOF0:
				StoreUnaryUpperMasked(VU, Ft(code), XYZW(code), Fs(code), IntToFloatBits<0>);
				return;
			case UpperFastKind::ITOF4:
				StoreUnaryUpperMasked(VU, Ft(code), XYZW(code), Fs(code), IntToFloatBits<4>);
				return;
			case UpperFastKind::ITOF12:
				StoreUnaryUpperMasked(VU, Ft(code), XYZW(code), Fs(code), IntToFloatBits<12>);
				return;
			case UpperFastKind::ITOF15:
				StoreUnaryUpperMasked(VU, Ft(code), XYZW(code), Fs(code), IntToFloatBits<15>);
				return;
			case UpperFastKind::MAX:
				StoreBinaryUpperMasked(VU, Fd(code), XYZW(code), Fs(code), Ft(code), FpMaxBits);
				return;
			case UpperFastKind::MAXi:
				StoreBinaryUpperBroadcastMasked(VU, Fd(code), XYZW(code), Fs(code), VU->VI[REG_I].UL, FpMaxBits);
				return;
			case UpperFastKind::MAXx:
				StoreBinaryUpperBroadcastMasked(VU, Fd(code), XYZW(code), Fs(code), VU->VF[Ft(code)].UL[0], FpMaxBits);
				return;
			case UpperFastKind::MAXy:
				StoreBinaryUpperBroadcastMasked(VU, Fd(code), XYZW(code), Fs(code), VU->VF[Ft(code)].UL[1], FpMaxBits);
				return;
			case UpperFastKind::MAXz:
				StoreBinaryUpperBroadcastMasked(VU, Fd(code), XYZW(code), Fs(code), VU->VF[Ft(code)].UL[2], FpMaxBits);
				return;
			case UpperFastKind::MAXw:
				StoreBinaryUpperBroadcastMasked(VU, Fd(code), XYZW(code), Fs(code), VU->VF[Ft(code)].UL[3], FpMaxBits);
				return;
			case UpperFastKind::MINI:
				StoreBinaryUpperMasked(VU, Fd(code), XYZW(code), Fs(code), Ft(code), FpMinBits);
				return;
			case UpperFastKind::MINIi:
				StoreBinaryUpperBroadcastMasked(VU, Fd(code), XYZW(code), Fs(code), VU->VI[REG_I].UL, FpMinBits);
				return;
			case UpperFastKind::MINIx:
				StoreBinaryUpperBroadcastMasked(VU, Fd(code), XYZW(code), Fs(code), VU->VF[Ft(code)].UL[0], FpMinBits);
				return;
			case UpperFastKind::MINIy:
				StoreBinaryUpperBroadcastMasked(VU, Fd(code), XYZW(code), Fs(code), VU->VF[Ft(code)].UL[1], FpMinBits);
				return;
			case UpperFastKind::MINIz:
				StoreBinaryUpperBroadcastMasked(VU, Fd(code), XYZW(code), Fs(code), VU->VF[Ft(code)].UL[2], FpMinBits);
				return;
			case UpperFastKind::MINIw:
				StoreBinaryUpperBroadcastMasked(VU, Fd(code), XYZW(code), Fs(code), VU->VF[Ft(code)].UL[3], FpMinBits);
				return;
			case UpperFastKind::CLIP:
				ExecuteClip(VU, code);
				return;
			case UpperFastKind::MUL:
				ExecuteMulMasked(VU, code, false, [VU, code](unsigned lane) { return VU->VF[Ft(code)].UL[lane]; });
				return;
			case UpperFastKind::MULi:
				ExecuteMulMasked(VU, code, false, [VU](unsigned) { return VU->VI[REG_I].UL; });
				return;
			case UpperFastKind::MULq:
				ExecuteMulMasked(VU, code, false, [VU](unsigned) { return VU->VI[REG_Q].UL; });
				return;
			case UpperFastKind::MULx:
				ExecuteMulMasked(VU, code, false, [VU, code](unsigned) { return VU->VF[Ft(code)].UL[0]; });
				return;
			case UpperFastKind::MULy:
				ExecuteMulMasked(VU, code, false, [VU, code](unsigned) { return VU->VF[Ft(code)].UL[1]; });
				return;
			case UpperFastKind::MULz:
				ExecuteMulMasked(VU, code, false, [VU, code](unsigned) { return VU->VF[Ft(code)].UL[2]; });
				return;
			case UpperFastKind::MULw:
				ExecuteMulMasked(VU, code, false, [VU, code](unsigned) { return VU->VF[Ft(code)].UL[3]; });
				return;
			case UpperFastKind::MULA:
				ExecuteMulMasked(VU, code, true, [VU, code](unsigned lane) { return VU->VF[Ft(code)].UL[lane]; });
				return;
			case UpperFastKind::MULAi:
				ExecuteMulMasked(VU, code, true, [VU](unsigned) { return VU->VI[REG_I].UL; });
				return;
			case UpperFastKind::MULAq:
				ExecuteMulMasked(VU, code, true, [VU](unsigned) { return VU->VI[REG_Q].UL; });
				return;
			case UpperFastKind::MULAx:
				ExecuteMulMasked(VU, code, true, [VU, code](unsigned) { return VU->VF[Ft(code)].UL[0]; });
				return;
			case UpperFastKind::MULAy:
				ExecuteMulMasked(VU, code, true, [VU, code](unsigned) { return VU->VF[Ft(code)].UL[1]; });
				return;
			case UpperFastKind::MULAz:
				ExecuteMulMasked(VU, code, true, [VU, code](unsigned) { return VU->VF[Ft(code)].UL[2]; });
				return;
			case UpperFastKind::MULAw:
				ExecuteMulMasked(VU, code, true, [VU, code](unsigned) { return VU->VF[Ft(code)].UL[3]; });
				return;
			case UpperFastKind::MADD:
				ExecuteMaddMasked(VU, code, false, [VU, code](unsigned lane) { return VU->VF[Ft(code)].UL[lane]; });
				return;
			case UpperFastKind::MADDi:
				ExecuteMaddMasked(VU, code, false, [VU](unsigned) { return VU->VI[REG_I].UL; });
				return;
			case UpperFastKind::MADDq:
				ExecuteMaddMasked(VU, code, false, [VU](unsigned) { return VU->VI[REG_Q].UL; });
				return;
			case UpperFastKind::MADDx:
				ExecuteMaddMasked(VU, code, false, [VU, code](unsigned) { return VU->VF[Ft(code)].UL[0]; });
				return;
			case UpperFastKind::MADDy:
				ExecuteMaddMasked(VU, code, false, [VU, code](unsigned) { return VU->VF[Ft(code)].UL[1]; });
				return;
			case UpperFastKind::MADDz:
				ExecuteMaddMasked(VU, code, false, [VU, code](unsigned) { return VU->VF[Ft(code)].UL[2]; });
				return;
			case UpperFastKind::MADDw:
				ExecuteMaddMasked(VU, code, false, [VU, code](unsigned) { return VU->VF[Ft(code)].UL[3]; });
				return;
			case UpperFastKind::MADDA:
				ExecuteMaddMasked(VU, code, true, [VU, code](unsigned lane) { return VU->VF[Ft(code)].UL[lane]; });
				return;
			case UpperFastKind::MADDAi:
				ExecuteMaddMasked(VU, code, true, [VU](unsigned) { return VU->VI[REG_I].UL; });
				return;
			case UpperFastKind::MADDAq:
				ExecuteMaddMasked(VU, code, true, [VU](unsigned) { return VU->VI[REG_Q].UL; });
				return;
			case UpperFastKind::MADDAx:
				ExecuteMaddMasked(VU, code, true, [VU, code](unsigned) { return VU->VF[Ft(code)].UL[0]; });
				return;
			case UpperFastKind::MADDAy:
				ExecuteMaddMasked(VU, code, true, [VU, code](unsigned) { return VU->VF[Ft(code)].UL[1]; });
				return;
			case UpperFastKind::MADDAz:
				ExecuteMaddMasked(VU, code, true, [VU, code](unsigned) { return VU->VF[Ft(code)].UL[2]; });
				return;
			case UpperFastKind::MADDAw:
				ExecuteMaddMasked(VU, code, true, [VU, code](unsigned) { return VU->VF[Ft(code)].UL[3]; });
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
		const float x = VuLane(VU, reg, 0);
		const float y = VuLane(VU, reg, 1);
		const float z = VuLane(VU, reg, 2);
		return x * x + y * y + z * z;
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
				const u16* ptr = reinterpret_cast<const u16*>(VuMemQword(VU, addr));
				if (XYZW(code) & 0x8) VU->VI[It(code)].US[0] = ptr[0];
				if (XYZW(code) & 0x4) VU->VI[It(code)].US[0] = ptr[2];
				if (XYZW(code) & 0x2) VU->VI[It(code)].US[0] = ptr[4];
				if (XYZW(code) & 0x1) VU->VI[It(code)].US[0] = ptr[6];
				return;
			}
			case LowerFastKind::ISW:
			{
				const u16 addr = static_cast<u16>((Imm11(code) + VU->VI[Is(code)].SS[0]) * 16);
				u16* ptr = reinterpret_cast<u16*>(VuMemQword(VU, addr));
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
				const u16* ptr = reinterpret_cast<const u16*>(VuMemQword(VU, VU->VI[Is(code)].US[0] * 16));
				if (XYZW(code) & 0x8) VU->VI[It(code)].US[0] = ptr[0];
				if (XYZW(code) & 0x4) VU->VI[It(code)].US[0] = ptr[2];
				if (XYZW(code) & 0x2) VU->VI[It(code)].US[0] = ptr[4];
				if (XYZW(code) & 0x1) VU->VI[It(code)].US[0] = ptr[6];
				return;
			}
			case LowerFastKind::ISWR:
			{
				u16* ptr = reinterpret_cast<u16*>(VuMemQword(VU, VU->VI[Is(code)].US[0] * 16));
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
