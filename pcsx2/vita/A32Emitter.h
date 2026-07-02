// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <cstddef>

namespace VitaA32
{
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
		bool EmitAddImm8(unsigned rd, unsigned rn, u8 value);
		size_t EmitBranchPlaceholder();
		bool PatchBranch(size_t instruction_offset, size_t target_offset);
		bool EmitBx(unsigned rm);
		bool Flush();

	private:
		bool HasSpace(size_t bytes) const;

		u8* m_base = nullptr;
		size_t m_capacity = 0;
		size_t m_offset = 0;
	};

	u32 EncodeMovImm8(unsigned rd, u8 value);
	u32 EncodeAddImm8(unsigned rd, unsigned rn, u8 value);
	u32 EncodeBx(unsigned rm);
	bool EncodeBranch(u8* instruction, u8* target, u32* out_instruction);
} // namespace VitaA32
