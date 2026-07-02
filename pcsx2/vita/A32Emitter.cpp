// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "pcsx2/vita/A32Emitter.h"

#include "common/Assertions.h"
#include "common/HostSys.h"
#include "common/Vita/VitaJitMemory.h"

#include <cstring>
#include <utility>

namespace VitaA32
{
	namespace
	{
		constexpr u32 DATA_PROCESSING_IMM = 0x02000000u;
		constexpr u32 OPCODE_AND = 0x00000000u;
		constexpr u32 OPCODE_EOR = 0x00200000u;
		constexpr u32 OPCODE_ADD = 0x00800000u;
		constexpr u32 OPCODE_ADC = 0x00a00000u;
		constexpr u32 OPCODE_CMP = 0x01400000u;
		constexpr u32 OPCODE_MOV = 0x01a00000u;
		constexpr u32 OPCODE_MVN = 0x01e00000u;
		constexpr u32 OPCODE_ORR = 0x01800000u;
		constexpr u32 OPCODE_SUB = 0x00400000u;
		constexpr u32 OPCODE_SBC = 0x00c00000u;
		constexpr u32 SET_FLAGS = 0x00100000u;
		constexpr u32 MOVW = 0x03000000u;
		constexpr u32 MOVT = 0x03400000u;
		constexpr u32 LDR_IMM = 0x05900000u;
		constexpr u32 STR_IMM = 0x05800000u;
		constexpr u32 BRANCH = 0x0a000000u;
		constexpr u32 PUSH = 0x092d0000u;
		constexpr u32 POP = 0x08bd0000u;
		constexpr u32 BX = 0x012fff10u;
		constexpr u32 BLX = 0x012fff30u;

		bool IsRegister(unsigned reg)
		{
			return reg < 16;
		}

		bool IsLowRegister(unsigned reg)
		{
			return reg < 15;
		}

		u32 CondBits(Condition condition)
		{
			return static_cast<u32>(condition) << 28;
		}
	} // namespace

	CodeBuffer::CodeBuffer(size_t capacity)
	{
		Allocate(capacity);
	}

	CodeBuffer::~CodeBuffer()
	{
		Release();
	}

	CodeBuffer::CodeBuffer(CodeBuffer&& other) noexcept
		: m_base(std::exchange(other.m_base, nullptr))
		, m_capacity(std::exchange(other.m_capacity, 0))
		, m_offset(std::exchange(other.m_offset, 0))
	{
	}

	CodeBuffer& CodeBuffer::operator=(CodeBuffer&& other) noexcept
	{
		if (this != &other)
		{
			Release();
			m_base = std::exchange(other.m_base, nullptr);
			m_capacity = std::exchange(other.m_capacity, 0);
			m_offset = std::exchange(other.m_offset, 0);
		}

		return *this;
	}

	bool CodeBuffer::Allocate(size_t capacity)
	{
		Release();
		m_base = static_cast<u8*>(VitaVM::AllocJitMemory(capacity));
		m_capacity = m_base ? capacity : 0;
		m_offset = 0;
		return (m_base != nullptr);
	}

	void CodeBuffer::Reset()
	{
		m_offset = 0;
	}

	void CodeBuffer::Release()
	{
		if (m_base)
		{
			VitaVM::FreeJitMemory(m_base);
			m_base = nullptr;
		}

		m_capacity = 0;
		m_offset = 0;
	}

	bool CodeBuffer::EmitU32(u32 instruction)
	{
		if (!HasSpace(sizeof(instruction)))
			return false;

		std::memcpy(m_base + m_offset, &instruction, sizeof(instruction));
		m_offset += sizeof(instruction);
		return true;
	}

	bool CodeBuffer::EmitMovImm8(unsigned rd, u8 value, Condition condition)
	{
		if (!IsRegister(rd))
			return false;
		return EmitU32(EncodeMovImm8(rd, value, condition));
	}

	bool CodeBuffer::EmitMovImm32(unsigned rd, u32 value)
	{
		if (!IsRegister(rd))
			return false;
		return EmitU32(EncodeMovw(rd, static_cast<u16>(value))) &&
			   EmitU32(EncodeMovt(rd, static_cast<u16>(value >> 16)));
	}

	bool CodeBuffer::EmitAddImm8(unsigned rd, unsigned rn, u8 value, bool set_flags)
	{
		if (!IsRegister(rd) || !IsRegister(rn))
			return false;
		return EmitU32(EncodeAddImm8(rd, rn, value, set_flags));
	}

	bool CodeBuffer::EmitSubImm8(unsigned rd, unsigned rn, u8 value, bool set_flags)
	{
		if (!IsRegister(rd) || !IsRegister(rn))
			return false;
		return EmitU32(EncodeSubImm8(rd, rn, value, set_flags));
	}

	bool CodeBuffer::EmitAndImm8(unsigned rd, unsigned rn, u8 value, bool set_flags)
	{
		if (!IsRegister(rd) || !IsRegister(rn))
			return false;
		return EmitU32(EncodeAndImm8(rd, rn, value, set_flags));
	}

	bool CodeBuffer::EmitEorImm8(unsigned rd, unsigned rn, u8 value, bool set_flags)
	{
		if (!IsRegister(rd) || !IsRegister(rn))
			return false;
		return EmitU32(EncodeEorImm8(rd, rn, value, set_flags));
	}

	bool CodeBuffer::EmitOrrImm8(unsigned rd, unsigned rn, u8 value, bool set_flags)
	{
		if (!IsRegister(rd) || !IsRegister(rn))
			return false;
		return EmitU32(EncodeOrrImm8(rd, rn, value, set_flags));
	}

	bool CodeBuffer::EmitAdcImm8(unsigned rd, unsigned rn, u8 value, bool set_flags)
	{
		if (!IsRegister(rd) || !IsRegister(rn))
			return false;
		return EmitU32(EncodeAdcImm8(rd, rn, value, set_flags));
	}

	bool CodeBuffer::EmitMovRegShiftImm(unsigned rd, unsigned rm, ShiftType shift, u8 amount, bool set_flags)
	{
		if (!IsRegister(rd) || !IsRegister(rm) || amount > 31)
			return false;
		return EmitU32(EncodeMovRegShiftImm(rd, rm, shift, amount, set_flags));
	}

	bool CodeBuffer::EmitMovRegShiftReg(unsigned rd, unsigned rm, ShiftType shift, unsigned rs, bool set_flags)
	{
		if (!IsRegister(rd) || !IsRegister(rm) || !IsRegister(rs))
			return false;
		return EmitU32(EncodeMovRegShiftReg(rd, rm, shift, rs, set_flags));
	}

	bool CodeBuffer::EmitAddReg(unsigned rd, unsigned rn, unsigned rm, bool set_flags)
	{
		if (!IsRegister(rd) || !IsRegister(rn) || !IsRegister(rm))
			return false;
		return EmitU32(EncodeAddReg(rd, rn, rm, set_flags));
	}

	bool CodeBuffer::EmitAdcReg(unsigned rd, unsigned rn, unsigned rm, bool set_flags)
	{
		if (!IsRegister(rd) || !IsRegister(rn) || !IsRegister(rm))
			return false;
		return EmitU32(EncodeAdcReg(rd, rn, rm, set_flags));
	}

	bool CodeBuffer::EmitAndReg(unsigned rd, unsigned rn, unsigned rm, bool set_flags)
	{
		if (!IsRegister(rd) || !IsRegister(rn) || !IsRegister(rm))
			return false;
		return EmitU32(EncodeAndReg(rd, rn, rm, set_flags));
	}

	bool CodeBuffer::EmitEorReg(unsigned rd, unsigned rn, unsigned rm, bool set_flags)
	{
		if (!IsRegister(rd) || !IsRegister(rn) || !IsRegister(rm))
			return false;
		return EmitU32(EncodeEorReg(rd, rn, rm, set_flags));
	}

	bool CodeBuffer::EmitOrrReg(unsigned rd, unsigned rn, unsigned rm, bool set_flags)
	{
		if (!IsRegister(rd) || !IsRegister(rn) || !IsRegister(rm))
			return false;
		return EmitU32(EncodeOrrReg(rd, rn, rm, set_flags));
	}

	bool CodeBuffer::EmitMvnReg(unsigned rd, unsigned rm, bool set_flags)
	{
		if (!IsRegister(rd) || !IsRegister(rm))
			return false;
		return EmitU32(EncodeMvnReg(rd, rm, set_flags));
	}

	bool CodeBuffer::EmitSubReg(unsigned rd, unsigned rn, unsigned rm, bool set_flags)
	{
		if (!IsRegister(rd) || !IsRegister(rn) || !IsRegister(rm))
			return false;
		return EmitU32(EncodeSubReg(rd, rn, rm, set_flags));
	}

	bool CodeBuffer::EmitSbcReg(unsigned rd, unsigned rn, unsigned rm, bool set_flags)
	{
		if (!IsRegister(rd) || !IsRegister(rn) || !IsRegister(rm))
			return false;
		return EmitU32(EncodeSbcReg(rd, rn, rm, set_flags));
	}

	bool CodeBuffer::EmitCmpReg(unsigned rn, unsigned rm, Condition condition)
	{
		if (!IsRegister(rn) || !IsRegister(rm))
			return false;
		return EmitU32(EncodeCmpReg(rn, rm, condition));
	}

	bool CodeBuffer::EmitLdrImm12(unsigned rd, unsigned rn, u16 offset)
	{
		if (!IsLowRegister(rd) || !IsLowRegister(rn) || offset > 0x0fff)
			return false;
		return EmitU32(EncodeLdrImm12(rd, rn, offset));
	}

	bool CodeBuffer::EmitStrImm12(unsigned rd, unsigned rn, u16 offset)
	{
		if (!IsLowRegister(rd) || !IsLowRegister(rn) || offset > 0x0fff)
			return false;
		return EmitU32(EncodeStrImm12(rd, rn, offset));
	}

	size_t CodeBuffer::EmitBranchPlaceholder(Condition condition)
	{
		const size_t offset = m_offset;
		return EmitU32(CondBits(condition) | BRANCH) ? offset : static_cast<size_t>(-1);
	}

	bool CodeBuffer::PatchBranch(size_t instruction_offset, size_t target_offset, Condition condition)
	{
		if (!m_base || instruction_offset + sizeof(u32) > m_offset || target_offset > m_offset ||
			(instruction_offset & 3) != 0 || (target_offset & 3) != 0)
		{
			return false;
		}

		u32 instruction = 0;
		if (!EncodeBranch(m_base + instruction_offset, m_base + target_offset, &instruction, condition))
			return false;

		std::memcpy(m_base + instruction_offset, &instruction, sizeof(instruction));
		return true;
	}

	bool CodeBuffer::EmitPush(u16 register_list)
	{
		if (register_list == 0)
			return false;
		return EmitU32(EncodePush(register_list));
	}

	bool CodeBuffer::EmitPop(u16 register_list)
	{
		if (register_list == 0)
			return false;
		return EmitU32(EncodePop(register_list));
	}

	bool CodeBuffer::EmitBx(unsigned rm)
	{
		if (!IsRegister(rm))
			return false;
		return EmitU32(EncodeBx(rm));
	}

	bool CodeBuffer::EmitBlx(unsigned rm)
	{
		if (!IsRegister(rm))
			return false;
		return EmitU32(EncodeBlx(rm));
	}

	bool CodeBuffer::EmitCallAbsolute(const void* function, unsigned scratch_reg)
	{
		if (!function || !IsRegister(scratch_reg) || scratch_reg == 13 || scratch_reg == 14 || scratch_reg == 15)
			return false;

		return EmitMovImm32(scratch_reg, static_cast<u32>(reinterpret_cast<uptr>(function))) &&
			   EmitBlx(scratch_reg);
	}

	bool CodeBuffer::PatchMovImm32(size_t instruction_offset, unsigned rd, u32 value)
	{
		if (!m_base || !IsRegister(rd) || instruction_offset + sizeof(u32) * 2 > m_offset ||
			(instruction_offset & 3) != 0)
		{
			return false;
		}

		const u32 movw = EncodeMovw(rd, static_cast<u16>(value));
		const u32 movt = EncodeMovt(rd, static_cast<u16>(value >> 16));
		std::memcpy(m_base + instruction_offset, &movw, sizeof(movw));
		std::memcpy(m_base + instruction_offset + sizeof(movw), &movt, sizeof(movt));
		return true;
	}

	bool CodeBuffer::Flush()
	{
		if (!m_base || m_offset == 0)
			return false;

		HostSys::FlushInstructionCache(m_base, static_cast<u32>(m_offset));
		return true;
	}

	bool CodeBuffer::HasSpace(size_t bytes) const
	{
		return m_base && bytes <= (m_capacity - m_offset);
	}

	u32 EncodeMovImm8(unsigned rd, u8 value, Condition condition)
	{
		pxAssert(IsRegister(rd));
		return CondBits(condition) | DATA_PROCESSING_IMM | OPCODE_MOV | ((rd & 0xfu) << 12) | value;
	}

	u32 EncodeMovw(unsigned rd, u16 value)
	{
		pxAssert(IsRegister(rd));
		return CondBits(Condition::AL) | MOVW | ((static_cast<u32>(value) & 0xf000u) << 4) | ((rd & 0xfu) << 12) |
			   (static_cast<u32>(value) & 0x0fffu);
	}

	u32 EncodeMovt(unsigned rd, u16 value)
	{
		pxAssert(IsRegister(rd));
		return CondBits(Condition::AL) | MOVT | ((static_cast<u32>(value) & 0xf000u) << 4) | ((rd & 0xfu) << 12) |
			   (static_cast<u32>(value) & 0x0fffu);
	}

	u32 EncodeAddImm8(unsigned rd, unsigned rn, u8 value, bool set_flags)
	{
		pxAssert(IsRegister(rd));
		pxAssert(IsRegister(rn));
		return CondBits(Condition::AL) | DATA_PROCESSING_IMM | OPCODE_ADD | (set_flags ? SET_FLAGS : 0) |
			   ((rn & 0xfu) << 16) | ((rd & 0xfu) << 12) | value;
	}

	u32 EncodeSubImm8(unsigned rd, unsigned rn, u8 value, bool set_flags)
	{
		pxAssert(IsRegister(rd));
		pxAssert(IsRegister(rn));
		return CondBits(Condition::AL) | DATA_PROCESSING_IMM | OPCODE_SUB | (set_flags ? SET_FLAGS : 0) |
			   ((rn & 0xfu) << 16) | ((rd & 0xfu) << 12) | value;
	}

	u32 EncodeAndImm8(unsigned rd, unsigned rn, u8 value, bool set_flags)
	{
		pxAssert(IsRegister(rd));
		pxAssert(IsRegister(rn));
		return CondBits(Condition::AL) | DATA_PROCESSING_IMM | OPCODE_AND | (set_flags ? SET_FLAGS : 0) |
			   ((rn & 0xfu) << 16) | ((rd & 0xfu) << 12) | value;
	}

	u32 EncodeEorImm8(unsigned rd, unsigned rn, u8 value, bool set_flags)
	{
		pxAssert(IsRegister(rd));
		pxAssert(IsRegister(rn));
		return CondBits(Condition::AL) | DATA_PROCESSING_IMM | OPCODE_EOR | (set_flags ? SET_FLAGS : 0) |
			   ((rn & 0xfu) << 16) | ((rd & 0xfu) << 12) | value;
	}

	u32 EncodeOrrImm8(unsigned rd, unsigned rn, u8 value, bool set_flags)
	{
		pxAssert(IsRegister(rd));
		pxAssert(IsRegister(rn));
		return CondBits(Condition::AL) | DATA_PROCESSING_IMM | OPCODE_ORR | (set_flags ? SET_FLAGS : 0) |
			   ((rn & 0xfu) << 16) | ((rd & 0xfu) << 12) | value;
	}

	u32 EncodeAdcImm8(unsigned rd, unsigned rn, u8 value, bool set_flags)
	{
		pxAssert(IsRegister(rd));
		pxAssert(IsRegister(rn));
		return CondBits(Condition::AL) | DATA_PROCESSING_IMM | OPCODE_ADC | (set_flags ? SET_FLAGS : 0) |
			   ((rn & 0xfu) << 16) | ((rd & 0xfu) << 12) | value;
	}

	u32 EncodeMovRegShiftImm(unsigned rd, unsigned rm, ShiftType shift, u8 amount, bool set_flags)
	{
		pxAssert(IsRegister(rd));
		pxAssert(IsRegister(rm));
		pxAssert(amount <= 31);
		return CondBits(Condition::AL) | OPCODE_MOV | (set_flags ? SET_FLAGS : 0) |
			   ((rd & 0xfu) << 12) | ((static_cast<u32>(amount) & 0x1fu) << 7) |
			   ((static_cast<u32>(shift) & 0x3u) << 5) | (rm & 0xfu);
	}

	u32 EncodeMovRegShiftReg(unsigned rd, unsigned rm, ShiftType shift, unsigned rs, bool set_flags)
	{
		pxAssert(IsRegister(rd));
		pxAssert(IsRegister(rm));
		pxAssert(IsRegister(rs));
		return CondBits(Condition::AL) | OPCODE_MOV | (set_flags ? SET_FLAGS : 0) |
			   ((rs & 0xfu) << 8) | ((rd & 0xfu) << 12) |
			   ((static_cast<u32>(shift) & 0x3u) << 5) | 0x10u | (rm & 0xfu);
	}

	u32 EncodeAddReg(unsigned rd, unsigned rn, unsigned rm, bool set_flags)
	{
		pxAssert(IsRegister(rd));
		pxAssert(IsRegister(rn));
		pxAssert(IsRegister(rm));
		return CondBits(Condition::AL) | OPCODE_ADD | (set_flags ? SET_FLAGS : 0) |
			   ((rn & 0xfu) << 16) | ((rd & 0xfu) << 12) | (rm & 0xfu);
	}

	u32 EncodeAdcReg(unsigned rd, unsigned rn, unsigned rm, bool set_flags)
	{
		pxAssert(IsRegister(rd));
		pxAssert(IsRegister(rn));
		pxAssert(IsRegister(rm));
		return CondBits(Condition::AL) | OPCODE_ADC | (set_flags ? SET_FLAGS : 0) |
			   ((rn & 0xfu) << 16) | ((rd & 0xfu) << 12) | (rm & 0xfu);
	}

	u32 EncodeAndReg(unsigned rd, unsigned rn, unsigned rm, bool set_flags)
	{
		pxAssert(IsRegister(rd));
		pxAssert(IsRegister(rn));
		pxAssert(IsRegister(rm));
		return CondBits(Condition::AL) | OPCODE_AND | (set_flags ? SET_FLAGS : 0) |
			   ((rn & 0xfu) << 16) | ((rd & 0xfu) << 12) | (rm & 0xfu);
	}

	u32 EncodeEorReg(unsigned rd, unsigned rn, unsigned rm, bool set_flags)
	{
		pxAssert(IsRegister(rd));
		pxAssert(IsRegister(rn));
		pxAssert(IsRegister(rm));
		return CondBits(Condition::AL) | OPCODE_EOR | (set_flags ? SET_FLAGS : 0) |
			   ((rn & 0xfu) << 16) | ((rd & 0xfu) << 12) | (rm & 0xfu);
	}

	u32 EncodeOrrReg(unsigned rd, unsigned rn, unsigned rm, bool set_flags)
	{
		pxAssert(IsRegister(rd));
		pxAssert(IsRegister(rn));
		pxAssert(IsRegister(rm));
		return CondBits(Condition::AL) | OPCODE_ORR | (set_flags ? SET_FLAGS : 0) |
			   ((rn & 0xfu) << 16) | ((rd & 0xfu) << 12) | (rm & 0xfu);
	}

	u32 EncodeMvnReg(unsigned rd, unsigned rm, bool set_flags)
	{
		pxAssert(IsRegister(rd));
		pxAssert(IsRegister(rm));
		return CondBits(Condition::AL) | OPCODE_MVN | (set_flags ? SET_FLAGS : 0) |
			   ((rd & 0xfu) << 12) | (rm & 0xfu);
	}

	u32 EncodeSubReg(unsigned rd, unsigned rn, unsigned rm, bool set_flags)
	{
		pxAssert(IsRegister(rd));
		pxAssert(IsRegister(rn));
		pxAssert(IsRegister(rm));
		return CondBits(Condition::AL) | OPCODE_SUB | (set_flags ? SET_FLAGS : 0) |
			   ((rn & 0xfu) << 16) | ((rd & 0xfu) << 12) | (rm & 0xfu);
	}

	u32 EncodeSbcReg(unsigned rd, unsigned rn, unsigned rm, bool set_flags)
	{
		pxAssert(IsRegister(rd));
		pxAssert(IsRegister(rn));
		pxAssert(IsRegister(rm));
		return CondBits(Condition::AL) | OPCODE_SBC | (set_flags ? SET_FLAGS : 0) |
			   ((rn & 0xfu) << 16) | ((rd & 0xfu) << 12) | (rm & 0xfu);
	}

	u32 EncodeCmpReg(unsigned rn, unsigned rm, Condition condition)
	{
		pxAssert(IsRegister(rn));
		pxAssert(IsRegister(rm));
		return CondBits(condition) | OPCODE_CMP | SET_FLAGS | ((rn & 0xfu) << 16) | (rm & 0xfu);
	}

	u32 EncodeLdrImm12(unsigned rd, unsigned rn, u16 offset)
	{
		pxAssert(IsLowRegister(rd));
		pxAssert(IsLowRegister(rn));
		pxAssert(offset <= 0x0fff);
		return CondBits(Condition::AL) | LDR_IMM | ((rn & 0xfu) << 16) | ((rd & 0xfu) << 12) | offset;
	}

	u32 EncodeStrImm12(unsigned rd, unsigned rn, u16 offset)
	{
		pxAssert(IsLowRegister(rd));
		pxAssert(IsLowRegister(rn));
		pxAssert(offset <= 0x0fff);
		return CondBits(Condition::AL) | STR_IMM | ((rn & 0xfu) << 16) | ((rd & 0xfu) << 12) | offset;
	}

	u32 EncodePush(u16 register_list)
	{
		pxAssert(register_list != 0);
		return CondBits(Condition::AL) | PUSH | register_list;
	}

	u32 EncodePop(u16 register_list)
	{
		pxAssert(register_list != 0);
		return CondBits(Condition::AL) | POP | register_list;
	}

	u32 EncodeBx(unsigned rm)
	{
		pxAssert(IsRegister(rm));
		return CondBits(Condition::AL) | BX | (rm & 0xfu);
	}

	u32 EncodeBlx(unsigned rm)
	{
		pxAssert(IsRegister(rm));
		return CondBits(Condition::AL) | BLX | (rm & 0xfu);
	}

	bool EncodeBranch(u8* instruction, u8* target, u32* out_instruction, Condition condition)
	{
		if (!instruction || !target || !out_instruction)
			return false;

		const intptr_t delta = reinterpret_cast<intptr_t>(target) - (reinterpret_cast<intptr_t>(instruction) + 8);
		if ((delta & 3) != 0)
			return false;

		const intptr_t words = delta >> 2;
		if (words < -0x800000 || words > 0x7fffff)
			return false;

		*out_instruction = CondBits(condition) | BRANCH | (static_cast<u32>(words) & 0x00ffffffu);
		return true;
	}
} // namespace VitaA32
