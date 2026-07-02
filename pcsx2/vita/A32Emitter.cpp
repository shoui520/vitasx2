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
		constexpr u32 COND_AL = 0xe0000000u;
		constexpr u32 DATA_PROCESSING_IMM = 0x02000000u;
		constexpr u32 OPCODE_ADD = 0x00800000u;
		constexpr u32 OPCODE_MOV = 0x01a00000u;
		constexpr u32 MOVW = 0x03000000u;
		constexpr u32 MOVT = 0x03400000u;
		constexpr u32 BRANCH = 0x0a000000u;
		constexpr u32 PUSH = 0x092d0000u;
		constexpr u32 POP = 0x08bd0000u;
		constexpr u32 BX = 0x012fff10u;
		constexpr u32 BLX = 0x012fff30u;

		bool IsRegister(unsigned reg)
		{
			return reg < 16;
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

	bool CodeBuffer::EmitMovImm8(unsigned rd, u8 value)
	{
		if (!IsRegister(rd))
			return false;
		return EmitU32(EncodeMovImm8(rd, value));
	}

	bool CodeBuffer::EmitMovImm32(unsigned rd, u32 value)
	{
		if (!IsRegister(rd))
			return false;
		return EmitU32(EncodeMovw(rd, static_cast<u16>(value))) &&
			   EmitU32(EncodeMovt(rd, static_cast<u16>(value >> 16)));
	}

	bool CodeBuffer::EmitAddImm8(unsigned rd, unsigned rn, u8 value)
	{
		if (!IsRegister(rd) || !IsRegister(rn))
			return false;
		return EmitU32(EncodeAddImm8(rd, rn, value));
	}

	size_t CodeBuffer::EmitBranchPlaceholder()
	{
		const size_t offset = m_offset;
		return EmitU32(COND_AL | BRANCH) ? offset : static_cast<size_t>(-1);
	}

	bool CodeBuffer::PatchBranch(size_t instruction_offset, size_t target_offset)
	{
		if (!m_base || instruction_offset + sizeof(u32) > m_offset || target_offset > m_offset ||
			(instruction_offset & 3) != 0 || (target_offset & 3) != 0)
		{
			return false;
		}

		u32 instruction = 0;
		if (!EncodeBranch(m_base + instruction_offset, m_base + target_offset, &instruction))
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

	u32 EncodeMovImm8(unsigned rd, u8 value)
	{
		pxAssert(IsRegister(rd));
		return COND_AL | DATA_PROCESSING_IMM | OPCODE_MOV | ((rd & 0xfu) << 12) | value;
	}

	u32 EncodeMovw(unsigned rd, u16 value)
	{
		pxAssert(IsRegister(rd));
		return COND_AL | MOVW | ((static_cast<u32>(value) & 0xf000u) << 4) | ((rd & 0xfu) << 12) |
			   (static_cast<u32>(value) & 0x0fffu);
	}

	u32 EncodeMovt(unsigned rd, u16 value)
	{
		pxAssert(IsRegister(rd));
		return COND_AL | MOVT | ((static_cast<u32>(value) & 0xf000u) << 4) | ((rd & 0xfu) << 12) |
			   (static_cast<u32>(value) & 0x0fffu);
	}

	u32 EncodeAddImm8(unsigned rd, unsigned rn, u8 value)
	{
		pxAssert(IsRegister(rd));
		pxAssert(IsRegister(rn));
		return COND_AL | DATA_PROCESSING_IMM | OPCODE_ADD | ((rn & 0xfu) << 16) | ((rd & 0xfu) << 12) | value;
	}

	u32 EncodePush(u16 register_list)
	{
		pxAssert(register_list != 0);
		return COND_AL | PUSH | register_list;
	}

	u32 EncodePop(u16 register_list)
	{
		pxAssert(register_list != 0);
		return COND_AL | POP | register_list;
	}

	u32 EncodeBx(unsigned rm)
	{
		pxAssert(IsRegister(rm));
		return COND_AL | BX | (rm & 0xfu);
	}

	u32 EncodeBlx(unsigned rm)
	{
		pxAssert(IsRegister(rm));
		return COND_AL | BLX | (rm & 0xfu);
	}

	bool EncodeBranch(u8* instruction, u8* target, u32* out_instruction)
	{
		if (!instruction || !target || !out_instruction)
			return false;

		const intptr_t delta = reinterpret_cast<intptr_t>(target) - (reinterpret_cast<intptr_t>(instruction) + 8);
		if ((delta & 3) != 0)
			return false;

		const intptr_t words = delta >> 2;
		if (words < -0x800000 || words > 0x7fffff)
			return false;

		*out_instruction = COND_AL | BRANCH | (static_cast<u32>(words) & 0x00ffffffu);
		return true;
	}
} // namespace VitaA32
