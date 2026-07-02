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
		void Reset();
		void Release();

		u8* Data() const { return m_base; }
		void* EntryPoint() const { return m_base; }
		size_t Size() const { return m_offset; }
		size_t Capacity() const { return m_capacity; }

		bool EmitU32(u32 instruction);
		bool EmitMovImm8(unsigned rd, u8 value);
		bool EmitMovImm32(unsigned rd, u32 value);
		bool EmitAddImm8(unsigned rd, unsigned rn, u8 value, bool set_flags = false);
		bool EmitSubImm8(unsigned rd, unsigned rn, u8 value, bool set_flags = false);
		bool EmitOrrImm8(unsigned rd, unsigned rn, u8 value, bool set_flags = false);
		bool EmitAdcImm8(unsigned rd, unsigned rn, u8 value, bool set_flags = false);
		bool EmitMovRegShiftImm(unsigned rd, unsigned rm, ShiftType shift, u8 amount, bool set_flags = false);
		bool EmitSubReg(unsigned rd, unsigned rn, unsigned rm, bool set_flags = false);
		bool EmitSbcReg(unsigned rd, unsigned rn, unsigned rm, bool set_flags = false);
		bool EmitLdrImm12(unsigned rd, unsigned rn, u16 offset);
		bool EmitStrImm12(unsigned rd, unsigned rn, u16 offset);
		size_t EmitBranchPlaceholder(Condition condition = Condition::AL);
		bool PatchBranch(size_t instruction_offset, size_t target_offset, Condition condition = Condition::AL);
		bool EmitPush(u16 register_list);
		bool EmitPop(u16 register_list);
		bool EmitBx(unsigned rm);
		bool EmitBlx(unsigned rm);
		bool EmitCallAbsolute(const void* function, unsigned scratch_reg = 12);
		bool Flush();

	private:
		bool HasSpace(size_t bytes) const;

		u8* m_base = nullptr;
		size_t m_capacity = 0;
		size_t m_offset = 0;
	};

	u32 EncodeMovImm8(unsigned rd, u8 value);
	u32 EncodeMovw(unsigned rd, u16 value);
	u32 EncodeMovt(unsigned rd, u16 value);
	u32 EncodeAddImm8(unsigned rd, unsigned rn, u8 value, bool set_flags = false);
	u32 EncodeSubImm8(unsigned rd, unsigned rn, u8 value, bool set_flags = false);
	u32 EncodeOrrImm8(unsigned rd, unsigned rn, u8 value, bool set_flags = false);
	u32 EncodeAdcImm8(unsigned rd, unsigned rn, u8 value, bool set_flags = false);
	u32 EncodeMovRegShiftImm(unsigned rd, unsigned rm, ShiftType shift, u8 amount, bool set_flags = false);
	u32 EncodeSubReg(unsigned rd, unsigned rn, unsigned rm, bool set_flags = false);
	u32 EncodeSbcReg(unsigned rd, unsigned rn, unsigned rm, bool set_flags = false);
	u32 EncodeLdrImm12(unsigned rd, unsigned rn, u16 offset);
	u32 EncodeStrImm12(unsigned rd, unsigned rn, u16 offset);
	u32 EncodePush(u16 register_list);
	u32 EncodePop(u16 register_list);
	u32 EncodeBx(unsigned rm);
	u32 EncodeBlx(unsigned rm);
	bool EncodeBranch(u8* instruction, u8* target, u32* out_instruction, Condition condition = Condition::AL);
} // namespace VitaA32
