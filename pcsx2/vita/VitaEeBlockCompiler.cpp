// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "pcsx2/vita/VitaEeBlockCompiler.h"

#include "pcsx2/R5900.h"
#include "pcsx2/vita/A32Emitter.h"

#include <cstddef>

namespace VitaEE
{
	namespace
	{
		constexpr u16 REG_R4 = 1u << 4;
		constexpr u16 REG_LR = 1u << 14;
		constexpr u16 REG_PC = 1u << 15;

		constexpr unsigned HOST_CPU_REGS = 4;
		constexpr unsigned HOST_TMP0 = 0;
		constexpr unsigned HOST_TMP1 = 1;
		constexpr unsigned HOST_TMP2 = 2;

		constexpr size_t GPR_OFFSET = offsetof(cpuRegisters, GPR);

		constexpr unsigned RS(u32 op)
		{
			return (op >> 21) & 0x1f;
		}

		constexpr unsigned RT(u32 op)
		{
			return (op >> 16) & 0x1f;
		}

		constexpr unsigned RD(u32 op)
		{
			return (op >> 11) & 0x1f;
		}

		constexpr unsigned SA(u32 op)
		{
			return (op >> 6) & 0x1f;
		}

		constexpr u16 IMM_U(u32 op)
		{
			return static_cast<u16>(op);
		}

		constexpr s16 IMM_S(u32 op)
		{
			return static_cast<s16>(op);
		}

		constexpr size_t GprOffset(unsigned guest_reg)
		{
			return GPR_OFFSET + sizeof(GPR_reg) * guest_reg;
		}
	} // namespace

	static_assert(GprOffset(31) + sizeof(u64) <= 0x0fff);

	BlockCompiler::BlockCompiler(VitaA32::CodeBuffer& code)
		: m_code(code)
	{
	}

	bool BlockCompiler::BeginBlock()
	{
		return m_code.EmitPush(REG_R4 | REG_LR) &&
			   m_code.EmitMovImm32(HOST_CPU_REGS, static_cast<u32>(reinterpret_cast<uptr>(&cpuRegs)));
	}

	bool BlockCompiler::EmitOpcode(u32 op)
	{
		switch (op >> 26)
		{
			case 0x00:
				return EmitSPECIAL(op);
			case 0x09: // ADDIU, owned by R5900OpcodeImpl.cpp::ADDIU().
				return EmitADDIU(op);
			case 0x0d: // ORI, owned by R5900OpcodeImpl.cpp::ORI().
				return EmitORI(op);
			case 0x0f: // LUI, owned by R5900OpcodeImpl.cpp::LUI().
				return EmitLUI(op);
			default:
				return false;
		}
	}

	bool BlockCompiler::EndBlockReturn(u8 value)
	{
		return m_code.EmitMovImm8(0, value) &&
			   m_code.EmitPop(REG_R4 | REG_PC);
	}

	bool BlockCompiler::EmitSPECIAL(u32 op)
	{
		switch (op & 0x3f)
		{
			case 0x00: // SLL, owned by R5900OpcodeImpl.cpp::SLL().
				return EmitSLL(op);
			case 0x21: // ADDU, owned by R5900OpcodeImpl.cpp::ADDU().
				return EmitADDU(op);
			default:
				return false;
		}
	}

	bool BlockCompiler::EmitADDIU(u32 op)
	{
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		const s32 imm = static_cast<s32>(IMM_S(op));

		if (rt == 0)
			return true;

		if (!EmitLoadGprLow(rs, HOST_TMP0))
			return false;

		if (imm >= 0 && imm <= 255)
		{
			if (!m_code.EmitAddImm8(HOST_TMP0, HOST_TMP0, static_cast<u8>(imm)))
				return false;
		}
		else if (imm < 0 && imm >= -255)
		{
			if (!m_code.EmitSubImm8(HOST_TMP0, HOST_TMP0, static_cast<u8>(-imm)))
				return false;
		}
		else
		{
			if (!m_code.EmitMovImm32(HOST_TMP2, static_cast<u32>(imm)) ||
				!m_code.EmitAddReg(HOST_TMP0, HOST_TMP0, HOST_TMP2))
			{
				return false;
			}
		}

		return m_code.EmitMovRegShiftImm(HOST_TMP1, HOST_TMP0, VitaA32::ShiftType::ASR, 31) &&
			   EmitStoreGpr64(rt, HOST_TMP0, HOST_TMP1);
	}

	bool BlockCompiler::EmitORI(u32 op)
	{
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		const u16 imm = IMM_U(op);

		if (rt == 0)
			return true;

		if (!EmitLoadGpr64(rs, HOST_TMP0, HOST_TMP1))
			return false;

		if (imm <= 255)
		{
			if (!m_code.EmitOrrImm8(HOST_TMP0, HOST_TMP0, static_cast<u8>(imm)))
				return false;
		}
		else
		{
			if (!m_code.EmitMovImm32(HOST_TMP2, imm) ||
				!m_code.EmitOrrReg(HOST_TMP0, HOST_TMP0, HOST_TMP2))
			{
				return false;
			}
		}

		return EmitStoreGpr64(rt, HOST_TMP0, HOST_TMP1);
	}

	bool BlockCompiler::EmitLUI(u32 op)
	{
		const unsigned rt = RT(op);
		if (rt == 0)
			return true;

		const u32 value = op << 16;
		return m_code.EmitMovImm32(HOST_TMP0, value) &&
			   m_code.EmitMovRegShiftImm(HOST_TMP1, HOST_TMP0, VitaA32::ShiftType::ASR, 31) &&
			   EmitStoreGpr64(rt, HOST_TMP0, HOST_TMP1);
	}

	bool BlockCompiler::EmitSLL(u32 op)
	{
		const unsigned rt = RT(op);
		const unsigned rd = RD(op);
		const unsigned sa = SA(op);

		if (rd == 0)
			return true;

		return EmitLoadGprLow(rt, HOST_TMP0) &&
			   m_code.EmitMovRegShiftImm(HOST_TMP0, HOST_TMP0, VitaA32::ShiftType::LSL, static_cast<u8>(sa)) &&
			   m_code.EmitMovRegShiftImm(HOST_TMP1, HOST_TMP0, VitaA32::ShiftType::ASR, 31) &&
			   EmitStoreGpr64(rd, HOST_TMP0, HOST_TMP1);
	}

	bool BlockCompiler::EmitADDU(u32 op)
	{
		const unsigned rs = RS(op);
		const unsigned rt = RT(op);
		const unsigned rd = RD(op);

		if (rd == 0)
			return true;

		return EmitLoadGprLow(rs, HOST_TMP0) &&
			   EmitLoadGprLow(rt, HOST_TMP1) &&
			   m_code.EmitAddReg(HOST_TMP0, HOST_TMP0, HOST_TMP1) &&
			   m_code.EmitMovRegShiftImm(HOST_TMP1, HOST_TMP0, VitaA32::ShiftType::ASR, 31) &&
			   EmitStoreGpr64(rd, HOST_TMP0, HOST_TMP1);
	}

	bool BlockCompiler::EmitLoadGprLow(unsigned guest_reg, unsigned host_reg)
	{
		if (guest_reg == 0)
			return m_code.EmitMovImm8(host_reg, 0);

		return m_code.EmitLdrImm12(host_reg, HOST_CPU_REGS, static_cast<u16>(GprOffset(guest_reg)));
	}

	bool BlockCompiler::EmitLoadGpr64(unsigned guest_reg, unsigned host_low, unsigned host_high)
	{
		if (guest_reg == 0)
			return m_code.EmitMovImm8(host_low, 0) &&
				   m_code.EmitMovImm8(host_high, 0);

		const size_t offset = GprOffset(guest_reg);
		return m_code.EmitLdrImm12(host_low, HOST_CPU_REGS, static_cast<u16>(offset)) &&
			   m_code.EmitLdrImm12(host_high, HOST_CPU_REGS, static_cast<u16>(offset + sizeof(u32)));
	}

	bool BlockCompiler::EmitStoreGpr64(unsigned guest_reg, unsigned host_low, unsigned host_high)
	{
		if (guest_reg == 0)
			return true;

		const size_t offset = GprOffset(guest_reg);
		return m_code.EmitStrImm12(host_low, HOST_CPU_REGS, static_cast<u16>(offset)) &&
			   m_code.EmitStrImm12(host_high, HOST_CPU_REGS, static_cast<u16>(offset + sizeof(u32)));
	}
} // namespace VitaEE
