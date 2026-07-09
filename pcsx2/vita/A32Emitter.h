// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <cstddef>

namespace VitaA32
{
	enum class Condition : u8
	{
		EQ = 0x0,
		NE = 0x1,
		CS = 0x2,
		CC = 0x3,
		MI = 0x4,
		PL = 0x5,
		VS = 0x6,
		VC = 0x7,
		HI = 0x8,
		LS = 0x9,
		GE = 0xa,
		LT = 0xb,
		GT = 0xc,
		LE = 0xd,
		AL = 0xe,
	};

	enum class ShiftType : u8
	{
		LSL = 0,
		LSR = 1,
		ASR = 2,
		ROR = 3,
	};

	// Vita ARM-state code buffer for the future EE recompiler.
	// PCSX2's current x86 owners for the first real consumer are
	// x86/ix86-32/iR5900.cpp::iBranchTest() and x86/BaseblockEx.cpp::BaseBlocks::Link().
	class CodeBuffer
	{
	public:
		CodeBuffer() = default;
		explicit CodeBuffer(size_t capacity);
		~CodeBuffer();

		CodeBuffer(const CodeBuffer&) = delete;
		CodeBuffer& operator=(const CodeBuffer&) = delete;

		CodeBuffer(CodeBuffer&& other) noexcept;
		CodeBuffer& operator=(CodeBuffer&& other) noexcept;

		bool Allocate(size_t capacity);
		bool Attach(u8* data, size_t capacity);
		void Reset();
		void Release();

		u8* Data() const { return m_base; }
		void* EntryPoint() const { return m_base; }
		size_t Size() const { return m_offset; }
		size_t Capacity() const { return m_capacity; }

		bool EmitU32(u32 instruction);
		bool EmitMovImm8(unsigned rd, u8 value, Condition condition = Condition::AL);
		bool EmitMovw(unsigned rd, u16 value, Condition condition = Condition::AL);
		bool EmitMovt(unsigned rd, u16 value, Condition condition = Condition::AL);
		bool EmitMovImm32(unsigned rd, u32 value, Condition condition = Condition::AL);
		bool EmitMovImm32Patchable(unsigned rd, u32 value);
		bool EmitAddImm8(unsigned rd, unsigned rn, u8 value, bool set_flags = false);
		bool EmitAddImm32(unsigned rd, unsigned rn, u32 value, bool set_flags = false);
		bool EmitSubImm8(unsigned rd, unsigned rn, u8 value, bool set_flags = false);
		bool EmitSubImm32(unsigned rd, unsigned rn, u32 value, bool set_flags = false);
		bool EmitRsbImm32(unsigned rd, unsigned rn, u32 value, bool set_flags = false);
		bool EmitRscImm32(unsigned rd, unsigned rn, u32 value, bool set_flags = false);
		bool EmitAndImm8(unsigned rd, unsigned rn, u8 value, bool set_flags = false);
		bool EmitAndImm32(unsigned rd, unsigned rn, u32 value, bool set_flags = false);
		bool EmitBicImm32(unsigned rd, unsigned rn, u32 value, bool set_flags = false);
		bool EmitEorImm8(unsigned rd, unsigned rn, u8 value, bool set_flags = false);
		bool EmitEorImm32(unsigned rd, unsigned rn, u32 value, bool set_flags = false);
		bool EmitOrrImm8(unsigned rd, unsigned rn, u8 value, bool set_flags = false);
		bool EmitOrrImm32(unsigned rd, unsigned rn, u32 value, bool set_flags = false);
		bool EmitAdcImm8(unsigned rd, unsigned rn, u8 value, bool set_flags = false);
		bool EmitAdcImm32(unsigned rd, unsigned rn, u32 value, bool set_flags = false);
		bool EmitSbcImm8(unsigned rd, unsigned rn, u8 value, bool set_flags = false);
		bool EmitSbcImm32(unsigned rd, unsigned rn, u32 value, bool set_flags = false);
		bool EmitMovRegShiftImm(unsigned rd, unsigned rm, ShiftType shift, u8 amount, bool set_flags = false,
			Condition condition = Condition::AL);
		bool EmitMovRegShiftReg(unsigned rd, unsigned rm, ShiftType shift, unsigned rs, bool set_flags = false);
		bool EmitAddReg(unsigned rd, unsigned rn, unsigned rm, bool set_flags = false);
		bool EmitAddRegShiftImm(unsigned rd, unsigned rn, unsigned rm, ShiftType shift, u8 amount,
			bool set_flags = false);
		bool EmitAddRegShiftReg(unsigned rd, unsigned rn, unsigned rm, ShiftType shift, unsigned rs,
			bool set_flags = false);
		bool EmitAdcReg(unsigned rd, unsigned rn, unsigned rm, bool set_flags = false);
		bool EmitAndReg(unsigned rd, unsigned rn, unsigned rm, bool set_flags = false);
		bool EmitAndRegShiftImm(unsigned rd, unsigned rn, unsigned rm, ShiftType shift, u8 amount,
			bool set_flags = false);
		bool EmitAndRegShiftReg(unsigned rd, unsigned rn, unsigned rm, ShiftType shift, unsigned rs,
			bool set_flags = false);
		bool EmitBicRegShiftImm(unsigned rd, unsigned rn, unsigned rm, ShiftType shift, u8 amount,
			bool set_flags = false);
		bool EmitBicRegShiftReg(unsigned rd, unsigned rn, unsigned rm, ShiftType shift, unsigned rs,
			bool set_flags = false);
		bool EmitEorReg(unsigned rd, unsigned rn, unsigned rm, bool set_flags = false);
		bool EmitEorRegShiftImm(unsigned rd, unsigned rn, unsigned rm, ShiftType shift, u8 amount,
			bool set_flags = false);
		bool EmitEorRegShiftReg(unsigned rd, unsigned rn, unsigned rm, ShiftType shift, unsigned rs,
			bool set_flags = false);
		bool EmitOrrReg(unsigned rd, unsigned rn, unsigned rm, bool set_flags = false);
		bool EmitOrrRegShiftImm(unsigned rd, unsigned rn, unsigned rm, ShiftType shift, u8 amount,
			bool set_flags = false);
		bool EmitOrrRegShiftReg(unsigned rd, unsigned rn, unsigned rm, ShiftType shift, unsigned rs,
			bool set_flags = false);
		bool EmitMvnReg(unsigned rd, unsigned rm, bool set_flags = false);
		bool EmitClz(unsigned rd, unsigned rm);
		bool EmitSsat(unsigned rd, u8 bits, unsigned rm);
		bool EmitUbfx(unsigned rd, unsigned rn, u8 lsb, u8 width);
		bool EmitSxtb(unsigned rd, unsigned rm);
		bool EmitSxth(unsigned rd, unsigned rm);
		bool EmitUxth(unsigned rd, unsigned rm);
		bool EmitSubReg(unsigned rd, unsigned rn, unsigned rm, bool set_flags = false);
		bool EmitSubRegShiftImm(unsigned rd, unsigned rn, unsigned rm, ShiftType shift, u8 amount,
			bool set_flags = false);
		bool EmitSubRegShiftReg(unsigned rd, unsigned rn, unsigned rm, ShiftType shift, unsigned rs,
			bool set_flags = false);
		bool EmitSbcReg(unsigned rd, unsigned rn, unsigned rm, bool set_flags = false);
		bool EmitPkhbt(unsigned rd, unsigned rn, unsigned rm, u8 lsl_amount = 0);
		bool EmitUmull(unsigned rdlo, unsigned rdhi, unsigned rn, unsigned rm, bool set_flags = false);
		bool EmitSmull(unsigned rdlo, unsigned rdhi, unsigned rn, unsigned rm, bool set_flags = false);
		bool EmitCmpReg(unsigned rn, unsigned rm, Condition condition = Condition::AL);
		bool EmitCmpImm32(unsigned rn, u32 value, Condition condition = Condition::AL);
		bool EmitTstImm32(unsigned rn, u32 value, Condition condition = Condition::AL);
		bool EmitLdrImm12(unsigned rd, unsigned rn, u16 offset, Condition condition = Condition::AL);
		bool EmitLdrRegShift(unsigned rd, unsigned rn, unsigned rm, ShiftType shift, u8 amount);
		bool EmitStrImm12(unsigned rd, unsigned rn, u16 offset);
		bool EmitStrRegShift(unsigned rd, unsigned rn, unsigned rm, ShiftType shift, u8 amount);
		bool EmitLdrdImm8(unsigned rdlo, unsigned rdhi, unsigned rn, u8 offset,
			Condition condition = Condition::AL);
		bool EmitStrdImm8(unsigned rdlo, unsigned rdhi, unsigned rn, u8 offset);
		bool EmitLdrbImm12(unsigned rd, unsigned rn, u16 offset);
		bool EmitLdrbRegShift(unsigned rd, unsigned rn, unsigned rm, ShiftType shift, u8 amount);
		bool EmitStrbImm12(unsigned rd, unsigned rn, u16 offset);
		bool EmitStrbRegShift(unsigned rd, unsigned rn, unsigned rm, ShiftType shift, u8 amount);
		bool EmitLdrhImm8(unsigned rd, unsigned rn, u8 offset);
		bool EmitLdrhReg(unsigned rd, unsigned rn, unsigned rm);
		bool EmitStrhImm8(unsigned rd, unsigned rn, u8 offset);
		bool EmitStrhReg(unsigned rd, unsigned rn, unsigned rm);
		bool EmitLdrsbImm8(unsigned rd, unsigned rn, u8 offset);
		bool EmitLdrsbReg(unsigned rd, unsigned rn, unsigned rm);
		bool EmitLdrshImm8(unsigned rd, unsigned rn, u8 offset);
		bool EmitLdrshReg(unsigned rd, unsigned rn, unsigned rm);
		bool EmitVld1Q32(unsigned qd, unsigned rn);
		bool EmitVld1Q32Aligned(unsigned qd, unsigned rn);
		bool EmitVst1Q32(unsigned qd, unsigned rn);
		bool EmitVst1Q32Aligned(unsigned qd, unsigned rn);
		bool EmitVst1D32(unsigned dd, unsigned rn);
		bool EmitVldrSImm(unsigned sd, unsigned rn, u16 offset);
		bool EmitVstrSImm(unsigned sd, unsigned rn, u16 offset);
		bool EmitVldrDImm(unsigned dd, unsigned rn, u16 offset);
		bool EmitVstrDImm(unsigned dd, unsigned rn, u16 offset);
		bool EmitVdupI16D(unsigned dd, unsigned dm, u8 lane);
		bool EmitVdupI32QFromCore(unsigned qd, unsigned rt);
		bool EmitVmovS(unsigned sd, unsigned sm);
		bool EmitVmovCoreToS(unsigned sd, unsigned rt);
		bool EmitVmovSToCore(unsigned rt, unsigned sd);
		bool EmitVmovCorePairToD(unsigned dd, unsigned rt, unsigned rt2);
		bool EmitVcvtF32S32(unsigned sd, unsigned sm);
		bool EmitVcvtF64F32(unsigned dd, unsigned sm);
		bool EmitVcvtF32F64(unsigned sd, unsigned dm);
		bool EmitVcvtF32S32Q(unsigned qd, unsigned qm);
		bool EmitVcvtS32F32Q(unsigned qd, unsigned qm);
		bool EmitVaddF32(unsigned sd, unsigned sn, unsigned sm);
		bool EmitVaddF64(unsigned dd, unsigned dn, unsigned dm);
		bool EmitVsubF32(unsigned sd, unsigned sn, unsigned sm);
		bool EmitVmulF32(unsigned sd, unsigned sn, unsigned sm);
		bool EmitVmulF64(unsigned dd, unsigned dn, unsigned dm);
		bool EmitVaddF32Q(unsigned qd, unsigned qn, unsigned qm);
		bool EmitVsubF32Q(unsigned qd, unsigned qn, unsigned qm);
		bool EmitVmulF32Q(unsigned qd, unsigned qn, unsigned qm);
		bool EmitVdivF32(unsigned sd, unsigned sn, unsigned sm);
		bool EmitVdivF64(unsigned dd, unsigned dn, unsigned dm);
		bool EmitVsqrtF32(unsigned sd, unsigned sm);
		bool EmitVaddI8Q(unsigned qd, unsigned qn, unsigned qm);
		bool EmitVaddI16Q(unsigned qd, unsigned qn, unsigned qm);
		bool EmitVaddI32Q(unsigned qd, unsigned qn, unsigned qm);
		bool EmitVaddI64Q(unsigned qd, unsigned qn, unsigned qm);
		bool EmitVsubI8Q(unsigned qd, unsigned qn, unsigned qm);
		bool EmitVsubI16Q(unsigned qd, unsigned qn, unsigned qm);
		bool EmitVsubI32Q(unsigned qd, unsigned qn, unsigned qm);
		bool EmitVshlI16Q(unsigned qd, unsigned qm, u8 amount);
		bool EmitVshlI32Q(unsigned qd, unsigned qm, u8 amount);
		bool EmitVshrU16Q(unsigned qd, unsigned qm, u8 amount);
		bool EmitVshrU32Q(unsigned qd, unsigned qm, u8 amount);
		bool EmitVshrS16Q(unsigned qd, unsigned qm, u8 amount);
		bool EmitVshrS32Q(unsigned qd, unsigned qm, u8 amount);
		bool EmitVshlU32Q(unsigned qd, unsigned qm, unsigned qn);
		bool EmitVshlS32Q(unsigned qd, unsigned qm, unsigned qn);
		bool EmitVmullS16Q(unsigned qd, unsigned dn, unsigned dm);
		bool EmitVmullS32Q(unsigned qd, unsigned dn, unsigned dm);
		bool EmitVmullU32Q(unsigned qd, unsigned dn, unsigned dm);
		bool EmitVnegS32Q(unsigned qd, unsigned qm);
		bool EmitVqmovnS32D(unsigned dd, unsigned qm);
		bool EmitVqmovnS64D(unsigned dd, unsigned qm);
		bool EmitVcgtS8Q(unsigned qd, unsigned qn, unsigned qm);
		bool EmitVcgtS16Q(unsigned qd, unsigned qn, unsigned qm);
		bool EmitVcgtS32Q(unsigned qd, unsigned qn, unsigned qm);
		bool EmitVceqI8Q(unsigned qd, unsigned qn, unsigned qm);
		bool EmitVceqI16Q(unsigned qd, unsigned qn, unsigned qm);
		bool EmitVceqI32Q(unsigned qd, unsigned qn, unsigned qm);
		bool EmitVminS16Q(unsigned qd, unsigned qn, unsigned qm);
		bool EmitVminS32Q(unsigned qd, unsigned qn, unsigned qm);
		bool EmitVmaxS16Q(unsigned qd, unsigned qn, unsigned qm);
		bool EmitVmaxS32Q(unsigned qd, unsigned qn, unsigned qm);
		bool EmitVqaddS8Q(unsigned qd, unsigned qn, unsigned qm);
		bool EmitVqaddS16Q(unsigned qd, unsigned qn, unsigned qm);
		bool EmitVqaddS32Q(unsigned qd, unsigned qn, unsigned qm);
		bool EmitVqsubS8Q(unsigned qd, unsigned qn, unsigned qm);
		bool EmitVqsubS16Q(unsigned qd, unsigned qn, unsigned qm);
		bool EmitVqsubS32Q(unsigned qd, unsigned qn, unsigned qm);
		bool EmitVqaddU8Q(unsigned qd, unsigned qn, unsigned qm);
		bool EmitVqaddU16Q(unsigned qd, unsigned qn, unsigned qm);
		bool EmitVqaddU32Q(unsigned qd, unsigned qn, unsigned qm);
		bool EmitVqsubU8Q(unsigned qd, unsigned qn, unsigned qm);
		bool EmitVqsubU16Q(unsigned qd, unsigned qn, unsigned qm);
		bool EmitVqsubU32Q(unsigned qd, unsigned qn, unsigned qm);
		bool EmitVqabsS16Q(unsigned qd, unsigned qm);
		bool EmitVqabsS32Q(unsigned qd, unsigned qm);
		bool EmitVrev64I32D(unsigned dd, unsigned dm);
		bool EmitVrev64I32Q(unsigned qd, unsigned qm);
		bool EmitVrev64I16Q(unsigned qd, unsigned qm);
		bool EmitVswpD(unsigned dd, unsigned dm);
		bool EmitVtrnI16D(unsigned dd, unsigned dm);
		bool EmitVtrnI16Q(unsigned qd, unsigned qm);
		bool EmitVtrnI32D(unsigned dd, unsigned dm);
		bool EmitVtrnI32Q(unsigned qd, unsigned qm);
		bool EmitVzipI8Q(unsigned qd, unsigned qm);
		bool EmitVzipI16Q(unsigned qd, unsigned qm);
		bool EmitVzipI32Q(unsigned qd, unsigned qm);
		bool EmitVuzpI8Q(unsigned qd, unsigned qm);
		bool EmitVuzpI16Q(unsigned qd, unsigned qm);
		bool EmitVuzpI32Q(unsigned qd, unsigned qm);
		bool EmitVextI8Q(unsigned qd, unsigned qn, unsigned qm, u8 byte_offset);
		bool EmitVandQ(unsigned qd, unsigned qn, unsigned qm);
		bool EmitVeorQ(unsigned qd, unsigned qn, unsigned qm);
		bool EmitVeorD(unsigned dd, unsigned dn, unsigned dm);
		bool EmitVorrQ(unsigned qd, unsigned qn, unsigned qm);
			bool EmitVorrD(unsigned dd, unsigned dn, unsigned dm);
			bool EmitVmvnQ(unsigned qd, unsigned qm);
			size_t EmitBranchPlaceholder(Condition condition = Condition::AL);
			bool PatchBranch(size_t instruction_offset, size_t target_offset, Condition condition = Condition::AL);
			bool PatchBranchToAddress(size_t instruction_offset, const void* target,
				Condition condition = Condition::AL);
			bool PatchNop(size_t instruction_offset);
			bool EmitPush(u16 register_list);
		bool EmitPop(u16 register_list);
		bool EmitBx(unsigned rm);
		bool EmitBlx(unsigned rm);
		bool EmitCallAbsolute(const void* function, unsigned scratch_reg = 12);
		bool PatchMovImm32(size_t instruction_offset, unsigned rd, u32 value);
		bool Flush();

	private:
		bool HasSpace(size_t bytes) const;

		u8* m_base = nullptr;
		size_t m_capacity = 0;
		size_t m_offset = 0;
		bool m_owns_memory = false;
	};

	u32 EncodeMovImm8(unsigned rd, u8 value, Condition condition = Condition::AL);
	u32 EncodeMovw(unsigned rd, u16 value, Condition condition = Condition::AL);
	u32 EncodeMovt(unsigned rd, u16 value, Condition condition = Condition::AL);
	u32 EncodeAddImm8(unsigned rd, unsigned rn, u8 value, bool set_flags = false);
	u32 EncodeSubImm8(unsigned rd, unsigned rn, u8 value, bool set_flags = false);
	u32 EncodeAndImm8(unsigned rd, unsigned rn, u8 value, bool set_flags = false);
	u32 EncodeAndImm32(unsigned rd, unsigned rn, u32 value, bool set_flags = false);
	u32 EncodeBicImm32(unsigned rd, unsigned rn, u32 value, bool set_flags = false);
	u32 EncodeEorImm8(unsigned rd, unsigned rn, u8 value, bool set_flags = false);
	u32 EncodeEorImm32(unsigned rd, unsigned rn, u32 value, bool set_flags = false);
	u32 EncodeOrrImm8(unsigned rd, unsigned rn, u8 value, bool set_flags = false);
	u32 EncodeOrrImm32(unsigned rd, unsigned rn, u32 value, bool set_flags = false);
	u32 EncodeAdcImm8(unsigned rd, unsigned rn, u8 value, bool set_flags = false);
	u32 EncodeSbcImm8(unsigned rd, unsigned rn, u8 value, bool set_flags = false);
	u32 EncodeMovRegShiftImm(unsigned rd, unsigned rm, ShiftType shift, u8 amount, bool set_flags = false,
		Condition condition = Condition::AL);
	u32 EncodeMovRegShiftReg(unsigned rd, unsigned rm, ShiftType shift, unsigned rs, bool set_flags = false);
	u32 EncodeAddReg(unsigned rd, unsigned rn, unsigned rm, bool set_flags = false);
	u32 EncodeAddRegShiftImm(unsigned rd, unsigned rn, unsigned rm, ShiftType shift, u8 amount,
		bool set_flags = false);
	u32 EncodeAddRegShiftReg(unsigned rd, unsigned rn, unsigned rm, ShiftType shift, unsigned rs,
		bool set_flags = false);
	u32 EncodeAdcReg(unsigned rd, unsigned rn, unsigned rm, bool set_flags = false);
	u32 EncodeAndReg(unsigned rd, unsigned rn, unsigned rm, bool set_flags = false);
	u32 EncodeAndRegShiftImm(unsigned rd, unsigned rn, unsigned rm, ShiftType shift, u8 amount,
		bool set_flags = false);
	u32 EncodeAndRegShiftReg(unsigned rd, unsigned rn, unsigned rm, ShiftType shift, unsigned rs,
		bool set_flags = false);
	u32 EncodeBicRegShiftImm(unsigned rd, unsigned rn, unsigned rm, ShiftType shift, u8 amount,
		bool set_flags = false);
	u32 EncodeBicRegShiftReg(unsigned rd, unsigned rn, unsigned rm, ShiftType shift, unsigned rs,
		bool set_flags = false);
	u32 EncodeEorReg(unsigned rd, unsigned rn, unsigned rm, bool set_flags = false);
	u32 EncodeEorRegShiftImm(unsigned rd, unsigned rn, unsigned rm, ShiftType shift, u8 amount,
		bool set_flags = false);
	u32 EncodeEorRegShiftReg(unsigned rd, unsigned rn, unsigned rm, ShiftType shift, unsigned rs,
		bool set_flags = false);
	u32 EncodeOrrReg(unsigned rd, unsigned rn, unsigned rm, bool set_flags = false);
	u32 EncodeOrrRegShiftImm(unsigned rd, unsigned rn, unsigned rm, ShiftType shift, u8 amount,
		bool set_flags = false);
	u32 EncodeOrrRegShiftReg(unsigned rd, unsigned rn, unsigned rm, ShiftType shift, unsigned rs,
		bool set_flags = false);
	u32 EncodeMvnReg(unsigned rd, unsigned rm, bool set_flags = false);
	u32 EncodeClz(unsigned rd, unsigned rm);
	u32 EncodeSsat(unsigned rd, u8 bits, unsigned rm);
	u32 EncodeUbfx(unsigned rd, unsigned rn, u8 lsb, u8 width);
	u32 EncodeSxtb(unsigned rd, unsigned rm);
	u32 EncodeSxth(unsigned rd, unsigned rm);
	u32 EncodeUxth(unsigned rd, unsigned rm);
	u32 EncodeSubReg(unsigned rd, unsigned rn, unsigned rm, bool set_flags = false);
	u32 EncodeSubRegShiftImm(unsigned rd, unsigned rn, unsigned rm, ShiftType shift, u8 amount,
		bool set_flags = false);
	u32 EncodeSubRegShiftReg(unsigned rd, unsigned rn, unsigned rm, ShiftType shift, unsigned rs,
		bool set_flags = false);
	u32 EncodeSbcReg(unsigned rd, unsigned rn, unsigned rm, bool set_flags = false);
	u32 EncodePkhbt(unsigned rd, unsigned rn, unsigned rm, u8 lsl_amount = 0);
	u32 EncodeUmull(unsigned rdlo, unsigned rdhi, unsigned rn, unsigned rm, bool set_flags = false);
	u32 EncodeSmull(unsigned rdlo, unsigned rdhi, unsigned rn, unsigned rm, bool set_flags = false);
	u32 EncodeCmpReg(unsigned rn, unsigned rm, Condition condition = Condition::AL);
	u32 EncodeLdrImm12(unsigned rd, unsigned rn, u16 offset, Condition condition = Condition::AL);
	u32 EncodeLdrRegShift(unsigned rd, unsigned rn, unsigned rm, ShiftType shift, u8 amount);
	u32 EncodeStrImm12(unsigned rd, unsigned rn, u16 offset);
	u32 EncodeStrRegShift(unsigned rd, unsigned rn, unsigned rm, ShiftType shift, u8 amount);
	u32 EncodeLdrdImm8(unsigned rdlo, unsigned rdhi, unsigned rn, u8 offset,
		Condition condition = Condition::AL);
	u32 EncodeStrdImm8(unsigned rdlo, unsigned rdhi, unsigned rn, u8 offset);
	u32 EncodeLdrbImm12(unsigned rd, unsigned rn, u16 offset);
	u32 EncodeLdrbRegShift(unsigned rd, unsigned rn, unsigned rm, ShiftType shift, u8 amount);
	u32 EncodeStrbImm12(unsigned rd, unsigned rn, u16 offset);
	u32 EncodeStrbRegShift(unsigned rd, unsigned rn, unsigned rm, ShiftType shift, u8 amount);
	u32 EncodeLdrhImm8(unsigned rd, unsigned rn, u8 offset);
	u32 EncodeLdrhReg(unsigned rd, unsigned rn, unsigned rm);
	u32 EncodeStrhImm8(unsigned rd, unsigned rn, u8 offset);
	u32 EncodeStrhReg(unsigned rd, unsigned rn, unsigned rm);
	u32 EncodeLdrsbImm8(unsigned rd, unsigned rn, u8 offset);
	u32 EncodeLdrsbReg(unsigned rd, unsigned rn, unsigned rm);
	u32 EncodeLdrshImm8(unsigned rd, unsigned rn, u8 offset);
	u32 EncodeLdrshReg(unsigned rd, unsigned rn, unsigned rm);
	u32 EncodeVld1Q32(unsigned qd, unsigned rn);
	u32 EncodeVld1Q32Aligned(unsigned qd, unsigned rn);
	u32 EncodeVst1Q32(unsigned qd, unsigned rn);
	u32 EncodeVst1Q32Aligned(unsigned qd, unsigned rn);
	u32 EncodeVst1D32(unsigned dd, unsigned rn);
	u32 EncodeVldrSImm(unsigned sd, unsigned rn, u16 offset);
	u32 EncodeVstrSImm(unsigned sd, unsigned rn, u16 offset);
	u32 EncodeVldrDImm(unsigned dd, unsigned rn, u16 offset);
	u32 EncodeVstrDImm(unsigned dd, unsigned rn, u16 offset);
	u32 EncodeVdupI16D(unsigned dd, unsigned dm, u8 lane);
	u32 EncodeVdupI32QFromCore(unsigned qd, unsigned rt);
	u32 EncodeVmovS(unsigned sd, unsigned sm);
	u32 EncodeVmovCoreToS(unsigned sd, unsigned rt);
	u32 EncodeVmovSToCore(unsigned rt, unsigned sd);
	u32 EncodeVmovCorePairToD(unsigned dd, unsigned rt, unsigned rt2);
	u32 EncodeVcvtF32S32(unsigned sd, unsigned sm);
	u32 EncodeVcvtF64F32(unsigned dd, unsigned sm);
	u32 EncodeVcvtF32F64(unsigned sd, unsigned dm);
	u32 EncodeVcvtF32S32Q(unsigned qd, unsigned qm);
	u32 EncodeVcvtS32F32Q(unsigned qd, unsigned qm);
	u32 EncodeVaddF32(unsigned sd, unsigned sn, unsigned sm);
	u32 EncodeVaddF64(unsigned dd, unsigned dn, unsigned dm);
	u32 EncodeVsubF32(unsigned sd, unsigned sn, unsigned sm);
	u32 EncodeVmulF32(unsigned sd, unsigned sn, unsigned sm);
	u32 EncodeVmulF64(unsigned dd, unsigned dn, unsigned dm);
	u32 EncodeVaddF32Q(unsigned qd, unsigned qn, unsigned qm);
	u32 EncodeVsubF32Q(unsigned qd, unsigned qn, unsigned qm);
	u32 EncodeVmulF32Q(unsigned qd, unsigned qn, unsigned qm);
	u32 EncodeVdivF32(unsigned sd, unsigned sn, unsigned sm);
	u32 EncodeVdivF64(unsigned dd, unsigned dn, unsigned dm);
	u32 EncodeVsqrtF32(unsigned sd, unsigned sm);
	u32 EncodeVaddI8Q(unsigned qd, unsigned qn, unsigned qm);
	u32 EncodeVaddI16Q(unsigned qd, unsigned qn, unsigned qm);
	u32 EncodeVaddI32Q(unsigned qd, unsigned qn, unsigned qm);
	u32 EncodeVaddI64Q(unsigned qd, unsigned qn, unsigned qm);
	u32 EncodeVsubI8Q(unsigned qd, unsigned qn, unsigned qm);
	u32 EncodeVsubI16Q(unsigned qd, unsigned qn, unsigned qm);
	u32 EncodeVsubI32Q(unsigned qd, unsigned qn, unsigned qm);
	u32 EncodeVshlI16Q(unsigned qd, unsigned qm, u8 amount);
	u32 EncodeVshlI32Q(unsigned qd, unsigned qm, u8 amount);
	u32 EncodeVshrU16Q(unsigned qd, unsigned qm, u8 amount);
	u32 EncodeVshrU32Q(unsigned qd, unsigned qm, u8 amount);
	u32 EncodeVshrS16Q(unsigned qd, unsigned qm, u8 amount);
	u32 EncodeVshrS32Q(unsigned qd, unsigned qm, u8 amount);
	u32 EncodeVshlU32Q(unsigned qd, unsigned qm, unsigned qn);
	u32 EncodeVshlS32Q(unsigned qd, unsigned qm, unsigned qn);
	u32 EncodeVmullS16Q(unsigned qd, unsigned dn, unsigned dm);
	u32 EncodeVmullS32Q(unsigned qd, unsigned dn, unsigned dm);
	u32 EncodeVmullU32Q(unsigned qd, unsigned dn, unsigned dm);
	u32 EncodeVnegS32Q(unsigned qd, unsigned qm);
	u32 EncodeVqmovnS32D(unsigned dd, unsigned qm);
	u32 EncodeVqmovnS64D(unsigned dd, unsigned qm);
	u32 EncodeVcgtS8Q(unsigned qd, unsigned qn, unsigned qm);
	u32 EncodeVcgtS16Q(unsigned qd, unsigned qn, unsigned qm);
	u32 EncodeVcgtS32Q(unsigned qd, unsigned qn, unsigned qm);
	u32 EncodeVceqI8Q(unsigned qd, unsigned qn, unsigned qm);
	u32 EncodeVceqI16Q(unsigned qd, unsigned qn, unsigned qm);
	u32 EncodeVceqI32Q(unsigned qd, unsigned qn, unsigned qm);
	u32 EncodeVminS16Q(unsigned qd, unsigned qn, unsigned qm);
	u32 EncodeVminS32Q(unsigned qd, unsigned qn, unsigned qm);
	u32 EncodeVmaxS16Q(unsigned qd, unsigned qn, unsigned qm);
	u32 EncodeVmaxS32Q(unsigned qd, unsigned qn, unsigned qm);
	u32 EncodeVqaddS8Q(unsigned qd, unsigned qn, unsigned qm);
	u32 EncodeVqaddS16Q(unsigned qd, unsigned qn, unsigned qm);
	u32 EncodeVqaddS32Q(unsigned qd, unsigned qn, unsigned qm);
	u32 EncodeVqsubS8Q(unsigned qd, unsigned qn, unsigned qm);
	u32 EncodeVqsubS16Q(unsigned qd, unsigned qn, unsigned qm);
	u32 EncodeVqsubS32Q(unsigned qd, unsigned qn, unsigned qm);
	u32 EncodeVqaddU8Q(unsigned qd, unsigned qn, unsigned qm);
	u32 EncodeVqaddU16Q(unsigned qd, unsigned qn, unsigned qm);
	u32 EncodeVqaddU32Q(unsigned qd, unsigned qn, unsigned qm);
	u32 EncodeVqsubU8Q(unsigned qd, unsigned qn, unsigned qm);
	u32 EncodeVqsubU16Q(unsigned qd, unsigned qn, unsigned qm);
	u32 EncodeVqsubU32Q(unsigned qd, unsigned qn, unsigned qm);
	u32 EncodeVqabsS16Q(unsigned qd, unsigned qm);
	u32 EncodeVqabsS32Q(unsigned qd, unsigned qm);
	u32 EncodeVrev64I32D(unsigned dd, unsigned dm);
	u32 EncodeVrev64I32Q(unsigned qd, unsigned qm);
	u32 EncodeVrev64I16Q(unsigned qd, unsigned qm);
	u32 EncodeVswpD(unsigned dd, unsigned dm);
	u32 EncodeVtrnI16D(unsigned dd, unsigned dm);
	u32 EncodeVtrnI16Q(unsigned qd, unsigned qm);
	u32 EncodeVtrnI32D(unsigned dd, unsigned dm);
	u32 EncodeVtrnI32Q(unsigned qd, unsigned qm);
	u32 EncodeVzipI8Q(unsigned qd, unsigned qm);
	u32 EncodeVzipI16Q(unsigned qd, unsigned qm);
	u32 EncodeVzipI32Q(unsigned qd, unsigned qm);
	u32 EncodeVuzpI8Q(unsigned qd, unsigned qm);
	u32 EncodeVuzpI16Q(unsigned qd, unsigned qm);
	u32 EncodeVuzpI32Q(unsigned qd, unsigned qm);
	u32 EncodeVextI8Q(unsigned qd, unsigned qn, unsigned qm, u8 byte_offset);
	u32 EncodeVandQ(unsigned qd, unsigned qn, unsigned qm);
	u32 EncodeVeorQ(unsigned qd, unsigned qn, unsigned qm);
	u32 EncodeVeorD(unsigned dd, unsigned dn, unsigned dm);
	u32 EncodeVorrQ(unsigned qd, unsigned qn, unsigned qm);
	u32 EncodeVorrD(unsigned dd, unsigned dn, unsigned dm);
	u32 EncodeVmvnQ(unsigned qd, unsigned qm);
	u32 EncodePush(u16 register_list);
	u32 EncodePop(u16 register_list);
	u32 EncodeBx(unsigned rm);
	u32 EncodeBlx(unsigned rm);
	bool EncodeBranch(u8* instruction, u8* target, u32* out_instruction, Condition condition = Condition::AL);
} // namespace VitaA32
