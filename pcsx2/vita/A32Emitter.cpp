// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "pcsx2/vita/A32Emitter.h"

#include "common/Assertions.h"
#if defined(VITASX2_QEMU_VALIDATION)
#include <sys/mman.h>
#include <unistd.h>
#else
#include "common/HostSys.h"
#include "common/Vita/VitaJitMemory.h"
#endif

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
		constexpr u32 OPCODE_TST = 0x01000000u;
		constexpr u32 OPCODE_CMP = 0x01400000u;
		constexpr u32 OPCODE_CMN = 0x01600000u;
		constexpr u32 OPCODE_MOV = 0x01a00000u;
		constexpr u32 OPCODE_MVN = 0x01e00000u;
		constexpr u32 OPCODE_ORR = 0x01800000u;
		constexpr u32 OPCODE_RSB = 0x00600000u;
		constexpr u32 OPCODE_RSC = 0x00e00000u;
		constexpr u32 OPCODE_SUB = 0x00400000u;
		constexpr u32 OPCODE_SBC = 0x00c00000u;
		constexpr u32 OPCODE_BIC = 0x01c00000u;
		constexpr u32 SET_FLAGS = 0x00100000u;
		constexpr u32 MOVW = 0x03000000u;
		constexpr u32 MOVT = 0x03400000u;
		constexpr u32 LDR_IMM = 0x05900000u;
		constexpr u32 LDR_IMM_POST_INDEX = 0x04900000u;
		constexpr u32 LDR_REG = 0x07900000u;
		constexpr u32 STR_IMM = 0x05800000u;
		constexpr u32 STR_REG = 0x07800000u;
		constexpr u32 LDRD_IMM = 0x01c000d0u;
		constexpr u32 STRD_IMM = 0x01c000f0u;
		constexpr u32 VPUSH_D = 0xed2d0b00u;
		constexpr u32 VPOP_D = 0xecbd0b00u;
		constexpr u32 LDRB_IMM = 0x05d00000u;
		constexpr u32 LDRB_IMM_POST_INDEX = 0x04d00000u;
		constexpr u32 LDRB_REG = 0x07d00000u;
		constexpr u32 STRB_IMM = 0x05c00000u;
		constexpr u32 STRB_REG = 0x07c00000u;
		constexpr u32 LDRH_IMM = 0x01d000b0u;
		constexpr u32 LDRH_REG = 0x019000b0u;
		constexpr u32 STRH_IMM = 0x01c000b0u;
		constexpr u32 STRH_REG = 0x018000b0u;
		constexpr u32 LDRSB_IMM = 0x01d000d0u;
		constexpr u32 LDRSB_REG = 0x019000d0u;
		constexpr u32 LDRSH_IMM = 0x01d000f0u;
		constexpr u32 LDRSH_REG = 0x019000f0u;
		constexpr u32 PKHBT = 0x06800010u;
		constexpr u32 CLZ = 0x016f0f10u;
		constexpr u32 SSAT = 0x06a00010u;
		constexpr u32 UBFX = 0x07e00050u;
		constexpr u32 SXTB = 0x06af0070u;
		constexpr u32 SXTH = 0x06bf0070u;
		constexpr u32 UXTH = 0x06ff0070u;
		constexpr u32 UMULL = 0x00800090u;
		constexpr u32 SMULL = 0x00c00090u;
		constexpr u32 VLD1_32_Q = 0xf4200a8fu;
		constexpr u32 VLD1_32_Q_ALIGNED = 0xf4200aafu;
		constexpr u32 VST1_32_Q = 0xf4000a8fu;
		constexpr u32 VST1_32_Q_ALIGNED = 0xf4000aafu;
		// Rm=13 selects immediate writeback by the 16-byte transfer size.
		constexpr u32 VST1_32_Q_ALIGNED_WRITEBACK = 0xf4000aadu;
		constexpr u32 VST1_32_D = 0xf400078fu;
		constexpr u32 VST1_32_D_LANE = 0xf480080fu;
		constexpr u32 VST1_8_D_LANE0 = 0xf480000fu;
		constexpr u32 VST1_16_D_LANE0 = 0xf480040fu;
		constexpr u32 VLDR_S_IMM = 0x0d900a00u;
		constexpr u32 VSTR_S_IMM = 0x0d800a00u;
		constexpr u32 VLDR_D_IMM = 0x0d900b00u;
		constexpr u32 VSTR_D_IMM = 0x0d800b00u;
		constexpr u32 VDUP_I16_D = 0xf3b20c00u;
		constexpr u32 VDUP_I32_Q_CORE = 0xeea00b10u;
		constexpr u32 VMOV_S = 0xeeb00a40u;
		constexpr u32 VMOV_CORE_TO_S = 0xee000a10u;
		constexpr u32 VMOV_S_TO_CORE = 0xee100a10u;
		constexpr u32 VMOV_CORE_TO_D32_LANE = 0x0e000b10u;
		constexpr u32 VMOV_D32_LANE_TO_CORE = 0x0e100b10u;
		constexpr u32 VMOV_CORE_PAIR_TO_D = 0xec400b10u;
		constexpr u32 VCVT_F32_S32 = 0xeeb80ac0u;
		constexpr u32 VCVT_F64_F32 = 0xeeb70ac0u;
		constexpr u32 VCVT_F32_F64 = 0xeeb70bc0u;
		constexpr u32 VCVT_F32_S32_Q = 0xf3bb0640u;
		constexpr u32 VCVT_S32_F32_Q = 0xf3bb0740u;
		constexpr u32 VADD_F32 = 0xee300a00u;
		constexpr u32 VADD_F64 = 0xee300b00u;
		constexpr u32 VSUB_F32 = 0xee300a40u;
		constexpr u32 VMUL_F32 = 0xee200a00u;
		constexpr u32 VMUL_F64 = 0xee200b00u;
		constexpr u32 VADD_F32_Q = 0xf2000d40u;
		constexpr u32 VSUB_F32_Q = 0xf2200d40u;
		constexpr u32 VMUL_F32_Q = 0xf3000d50u;
		constexpr u32 VDIV_F32 = 0xee800a00u;
		constexpr u32 VDIV_F64 = 0xee800b00u;
		constexpr u32 VSQRT_F32 = 0xeeb10ac0u;
		constexpr u32 VADD_I8_Q = 0xf2000840u;
		constexpr u32 VADD_I16_Q = 0xf2100840u;
		constexpr u32 VADD_I32_Q = 0xf2200840u;
		constexpr u32 VADD_I64_Q = 0xf2300840u;
		constexpr u32 VSUB_I8_Q = 0xf3000840u;
		constexpr u32 VSUB_I16_Q = 0xf3100840u;
		constexpr u32 VSUB_I32_Q = 0xf3200840u;
		constexpr u32 VSHL_I16_Q = 0xf2900550u;
		constexpr u32 VSHL_I32_Q = 0xf2a00550u;
		constexpr u32 VSHR_U_Q = 0xf3800050u;
		constexpr u32 VSHR_S_Q = 0xf2800050u;
		constexpr u32 VSHL_U32_REG_Q = 0xf3200440u;
		constexpr u32 VSHL_S32_REG_Q = 0xf2200440u;
		constexpr u32 VMULL_S16_Q = 0xf2900c00u;
		constexpr u32 VMULL_S32_Q = 0xf2a00c00u;
		constexpr u32 VMULL_U32_Q = 0xf3a00c00u;
		constexpr u32 VNEG_S32_Q = 0xf3b903c0u;
		constexpr u32 VQMOVN_S32_D = 0xf3b60280u;
		constexpr u32 VQMOVN_S64_D = 0xf3ba0280u;
		constexpr u32 VCGT_S8_Q = 0xf2000340u;
		constexpr u32 VCGT_S16_Q = 0xf2100340u;
		constexpr u32 VCGT_S32_Q = 0xf2200340u;
		constexpr u32 VCEQ_I8_Q = 0xf3000850u;
		constexpr u32 VCEQ_I16_Q = 0xf3100850u;
		constexpr u32 VCEQ_I32_Q = 0xf3200850u;
		constexpr u32 VMIN_S16_Q = 0xf2100650u;
		constexpr u32 VMIN_S32_Q = 0xf2200650u;
		constexpr u32 VMAX_S16_Q = 0xf2100640u;
		constexpr u32 VMAX_S32_Q = 0xf2200640u;
		constexpr u32 VQADD_S8_Q = 0xf2000050u;
		constexpr u32 VQADD_S16_Q = 0xf2100050u;
		constexpr u32 VQADD_S32_Q = 0xf2200050u;
		constexpr u32 VQSUB_S8_Q = 0xf2000250u;
		constexpr u32 VQSUB_S16_Q = 0xf2100250u;
		constexpr u32 VQSUB_S32_Q = 0xf2200250u;
		constexpr u32 VQADD_U8_Q = 0xf3000050u;
		constexpr u32 VQADD_U16_Q = 0xf3100050u;
		constexpr u32 VQADD_U32_Q = 0xf3200050u;
		constexpr u32 VQSUB_U8_Q = 0xf3000250u;
		constexpr u32 VQSUB_U16_Q = 0xf3100250u;
		constexpr u32 VQSUB_U32_Q = 0xf3200250u;
		constexpr u32 VQABS_S16_Q = 0xf3b40740u;
		constexpr u32 VQABS_S32_Q = 0xf3b80740u;
		constexpr u32 VCLS_S32_Q = 0xf3b80440u;
		constexpr u32 VREV64_I32_D = 0xf3b80000u;
		constexpr u32 VREV64_I32_Q = 0xf3b80040u;
		constexpr u32 VREV64_I16_Q = 0xf3b40040u;
		constexpr u32 VSWP_D = 0xf3b20000u;
		constexpr u32 VTRN_I16_D = 0xf3b60080u;
		constexpr u32 VTRN_I16_Q = 0xf3b600c0u;
		constexpr u32 VTRN_I32_D = 0xf3ba0080u;
		constexpr u32 VTRN_I32_Q = 0xf3ba00c0u;
		constexpr u32 VZIP_I8_Q = 0xf3b201c0u;
		constexpr u32 VZIP_I16_Q = 0xf3b601c0u;
		constexpr u32 VZIP_I32_Q = 0xf3ba01c0u;
		constexpr u32 VUZP_I8_Q = 0xf3b20140u;
		constexpr u32 VUZP_I16_Q = 0xf3b60140u;
		constexpr u32 VUZP_I32_Q = 0xf3ba0140u;
		constexpr u32 VEXT_I8_Q = 0xf2b00040u;
		constexpr u32 VAND_Q = 0xf2000150u;
		constexpr u32 VEOR_Q = 0xf3000150u;
		constexpr u32 VEOR_D = 0xf3000110u;
		constexpr u32 VORR_Q = 0xf2200150u;
			constexpr u32 VORR_D = 0xf2200110u;
			constexpr u32 VMVN_Q = 0xf3b005c0u;
			constexpr u32 BRANCH = 0x0a000000u;
			constexpr u32 NOP = 0x0320f000u;
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

		bool IsGeneralRegister(unsigned reg)
		{
			return reg < 15;
		}

		bool CanElideSameRegisterWrite(unsigned rd, unsigned rn)
		{
			return rd == rn && IsGeneralRegister(rd);
		}

		bool IsQRegister(unsigned reg)
		{
			return reg < 16;
		}

		bool IsDRegister(unsigned reg)
		{
			return reg < 32;
		}

		bool IsSRegister(unsigned reg)
		{
			return reg < 32;
		}

		bool IsLowEvenRegisterPair(unsigned rdlo, unsigned rdhi)
		{
			return rdlo < 14 && rdhi == rdlo + 1 && ((rdlo & 1u) == 0);
		}

		u32 CondBits(Condition condition)
		{
			return static_cast<u32>(condition) << 28;
		}

		u32 RotateRight(u32 value, unsigned amount)
		{
			amount &= 31u;
			return amount == 0 ? value : ((value >> amount) | (value << (32u - amount)));
		}

		u32 RotateLeft(u32 value, unsigned amount)
		{
			amount &= 31u;
			return amount == 0 ? value : ((value << amount) | (value >> (32u - amount)));
		}

		bool EncodeModifiedImmediate(u32 value, u32* encoded)
		{
			for (unsigned rotate = 0; rotate < 16; rotate++)
			{
				const unsigned amount = rotate * 2;
				const u32 imm8 = RotateLeft(value, amount);
				if ((imm8 & ~0xffu) == 0 && RotateRight(imm8, amount) == value)
				{
					*encoded = (rotate << 8) | imm8;
					return true;
				}
			}

			return false;
		}

		u32 EncodeDataProcessingRegShiftImm(
			u32 opcode, unsigned rd, unsigned rn, unsigned rm, ShiftType shift, u8 amount, bool set_flags)
		{
			pxAssert(IsRegister(rd));
			pxAssert(IsRegister(rn));
			pxAssert(IsRegister(rm));
			pxAssert(amount <= 31);
			return CondBits(Condition::AL) | opcode | (set_flags ? SET_FLAGS : 0) |
				   ((rn & 0xfu) << 16) | ((rd & 0xfu) << 12) |
				   ((static_cast<u32>(amount) & 0x1fu) << 7) |
				   ((static_cast<u32>(shift) & 0x3u) << 5) | (rm & 0xfu);
		}

		u32 EncodeDataProcessingRegShiftReg(
			u32 opcode, unsigned rd, unsigned rn, unsigned rm, ShiftType shift, unsigned rs, bool set_flags)
		{
			pxAssert(IsRegister(rd));
			pxAssert(IsRegister(rn));
			pxAssert(IsRegister(rm));
			pxAssert(IsRegister(rs));
			return CondBits(Condition::AL) | opcode | (set_flags ? SET_FLAGS : 0) |
				   ((rn & 0xfu) << 16) | ((rd & 0xfu) << 12) | ((rs & 0xfu) << 8) |
				   ((static_cast<u32>(shift) & 0x3u) << 5) | 0x10u | (rm & 0xfu);
		}

		u32 NeonQd(unsigned qreg)
		{
			const unsigned dreg = qreg * 2;
			return ((dreg & 0xfu) << 12) | ((dreg & 0x10u) << 18);
		}

		u32 NeonDd(unsigned dreg)
		{
			return ((dreg & 0xfu) << 12) | ((dreg & 0x10u) << 18);
		}

		u32 NeonDm(unsigned dreg)
		{
			return (dreg & 0xfu) | ((dreg & 0x10u) << 1);
		}

		u32 VfpDd(unsigned dreg)
		{
			return ((dreg & 0xfu) << 12) | ((dreg & 0x10u) << 18);
		}

		u32 VfpDn(unsigned dreg)
		{
			return ((dreg & 0xfu) << 16) | ((dreg & 0x10u) << 3);
		}

		u32 VfpDm(unsigned dreg)
		{
			return (dreg & 0xfu) | ((dreg & 0x10u) << 1);
		}

		u32 NeonQn(unsigned qreg)
		{
			const unsigned dreg = qreg * 2;
			return ((dreg & 0xfu) << 16) | ((dreg & 0x10u) << 3);
		}

		u32 NeonDn(unsigned dreg)
		{
			return ((dreg & 0xfu) << 16) | ((dreg & 0x10u) << 3);
		}

		u32 NeonQm(unsigned qreg)
		{
			const unsigned dreg = qreg * 2;
			return (dreg & 0xfu) | ((dreg & 0x10u) << 1);
		}

		u32 VfpSd(unsigned sreg)
		{
			return ((sreg >> 1) << 12) | ((sreg & 1u) << 22);
		}

		u32 VfpSn(unsigned sreg)
		{
			return ((sreg >> 1) << 16) | ((sreg & 1u) << 7);
		}

		u32 VfpSm(unsigned sreg)
		{
			return (sreg >> 1) | ((sreg & 1u) << 5);
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
		, m_owns_memory(std::exchange(other.m_owns_memory, false))
		, m_neon_logical_first_q(std::exchange(other.m_neon_logical_first_q, 0))
		, m_neon_physical_first_q(std::exchange(other.m_neon_physical_first_q, 0))
		, m_neon_mapped_q_count(std::exchange(other.m_neon_mapped_q_count, 0))
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
			m_owns_memory = std::exchange(other.m_owns_memory, false);
			m_neon_logical_first_q = std::exchange(other.m_neon_logical_first_q, 0);
			m_neon_physical_first_q = std::exchange(other.m_neon_physical_first_q, 0);
			m_neon_mapped_q_count = std::exchange(other.m_neon_mapped_q_count, 0);
		}

		return *this;
	}

	bool CodeBuffer::Allocate(size_t capacity)
	{
		Release();
#if defined(VITASX2_QEMU_VALIDATION)
		const long page_size = sysconf(_SC_PAGESIZE);
		const size_t aligned_capacity = (capacity + static_cast<size_t>(page_size - 1)) &
			~static_cast<size_t>(page_size - 1);
		void* mapping = mmap(nullptr, aligned_capacity, PROT_READ | PROT_WRITE | PROT_EXEC,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (mapping == MAP_FAILED)
			return false;

		m_base = static_cast<u8*>(mapping);
		m_capacity = aligned_capacity;
		m_offset = 0;
		m_owns_memory = true;
		return true;
#else
		m_base = static_cast<u8*>(VitaVM::AllocJitMemory(capacity));
		m_capacity = m_base ? capacity : 0;
		m_offset = 0;
		m_owns_memory = (m_base != nullptr);
		return (m_base != nullptr);
#endif
	}

	bool CodeBuffer::Attach(u8* data, size_t capacity)
	{
		Release();
		if (!data || capacity == 0)
			return false;

		m_base = data;
		m_capacity = capacity;
		m_offset = 0;
		m_owns_memory = false;
		return true;
	}

	void CodeBuffer::Reset()
	{
		m_offset = 0;
	}

	void CodeBuffer::SetNeonQRegisterBankMapping(unsigned logical_first_q,
		unsigned physical_first_q, unsigned q_count)
	{
		pxAssert(logical_first_q <= 16);
		pxAssert(physical_first_q <= 16);
		pxAssert(q_count <= 16 - logical_first_q);
		pxAssert(q_count <= 16 - physical_first_q);

		m_neon_logical_first_q = static_cast<u8>(logical_first_q);
		m_neon_physical_first_q = static_cast<u8>(physical_first_q);
		m_neon_mapped_q_count = static_cast<u8>(q_count);
	}

	unsigned CodeBuffer::MapNeonQRegister(unsigned qreg) const
	{
		if (qreg >= m_neon_logical_first_q &&
			qreg - m_neon_logical_first_q < m_neon_mapped_q_count)
		{
			return m_neon_physical_first_q + (qreg - m_neon_logical_first_q);
		}

		return qreg;
	}

	unsigned CodeBuffer::MapNeonDRegister(unsigned dreg) const
	{
		const unsigned logical_first_d = m_neon_logical_first_q * 2;
		const unsigned mapped_d_count = m_neon_mapped_q_count * 2;
		if (dreg >= logical_first_d && dreg - logical_first_d < mapped_d_count)
		{
			return m_neon_physical_first_q * 2 + (dreg - logical_first_d);
		}

		return dreg;
	}

	void CodeBuffer::Release()
	{
		if (m_base)
		{
#if defined(VITASX2_QEMU_VALIDATION)
			if (m_owns_memory)
				munmap(m_base, m_capacity);
#else
			if (m_owns_memory)
				VitaVM::FreeJitMemory(m_base);
#endif
			m_base = nullptr;
		}

		m_capacity = 0;
		m_offset = 0;
		m_owns_memory = false;
		m_neon_logical_first_q = 0;
		m_neon_physical_first_q = 0;
		m_neon_mapped_q_count = 0;
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

	bool CodeBuffer::EmitMovw(unsigned rd, u16 value, Condition condition)
	{
		if (!IsRegister(rd))
			return false;
		return EmitU32(EncodeMovw(rd, value, condition));
	}

	bool CodeBuffer::EmitMovt(unsigned rd, u16 value, Condition condition)
	{
		if (!IsRegister(rd))
			return false;
		return EmitU32(EncodeMovt(rd, value, condition));
	}

	bool CodeBuffer::EmitMovImm32(unsigned rd, u32 value, Condition condition)
	{
		if (!IsRegister(rd))
			return false;

		u32 encoded = 0;
		if (EncodeModifiedImmediate(value, &encoded))
			return EmitU32(CondBits(condition) | DATA_PROCESSING_IMM | OPCODE_MOV |
						   ((rd & 0xfu) << 12) | encoded);

		if (EncodeModifiedImmediate(~value, &encoded))
			return EmitU32(CondBits(condition) | DATA_PROCESSING_IMM | OPCODE_MVN |
						   ((rd & 0xfu) << 12) | encoded);

		if ((value >> 16) == 0)
			return EmitU32(EncodeMovw(rd, static_cast<u16>(value), condition));

		if (condition != Condition::AL)
		{
			return EmitU32(EncodeMovw(rd, static_cast<u16>(value), condition)) &&
				   EmitU32(EncodeMovt(rd, static_cast<u16>(value >> 16), condition));
		}

		return EmitMovImm32Patchable(rd, value);
	}

	bool CodeBuffer::EmitMovImm32Patchable(unsigned rd, u32 value)
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
		if (!set_flags && value == 0 && CanElideSameRegisterWrite(rd, rn))
			return true;
		return EmitU32(EncodeAddImm8(rd, rn, value, set_flags));
	}

	bool CodeBuffer::EmitAddImm32(unsigned rd, unsigned rn, u32 value, bool set_flags)
	{
		if (!IsRegister(rd) || !IsRegister(rn))
			return false;
		if (!set_flags && value == 0 && CanElideSameRegisterWrite(rd, rn))
			return true;

		u32 encoded = 0;
		if (!EncodeModifiedImmediate(value, &encoded))
		{
			if (!EncodeModifiedImmediate(0u - value, &encoded))
				return false;

			// For nonzero immediates, ADD x, value and SUB x, -value have
			// identical results and NZCV. Zero takes the direct ADD path above.
			return EmitU32(CondBits(Condition::AL) | DATA_PROCESSING_IMM | OPCODE_SUB |
						   (set_flags ? SET_FLAGS : 0) |
						   ((rn & 0xfu) << 16) | ((rd & 0xfu) << 12) | encoded);
		}

		return EmitU32(CondBits(Condition::AL) | DATA_PROCESSING_IMM | OPCODE_ADD |
					   (set_flags ? SET_FLAGS : 0) | ((rn & 0xfu) << 16) |
					   ((rd & 0xfu) << 12) | encoded);
	}

	bool CodeBuffer::EmitSubImm8(unsigned rd, unsigned rn, u8 value, bool set_flags)
	{
		if (!IsRegister(rd) || !IsRegister(rn))
			return false;
		if (!set_flags && value == 0 && CanElideSameRegisterWrite(rd, rn))
			return true;
		return EmitU32(EncodeSubImm8(rd, rn, value, set_flags));
	}

	bool CodeBuffer::EmitSubImm32(unsigned rd, unsigned rn, u32 value, bool set_flags)
	{
		if (!IsRegister(rd) || !IsRegister(rn))
			return false;
		if (!set_flags && value == 0 && CanElideSameRegisterWrite(rd, rn))
			return true;

		u32 encoded = 0;
		if (!EncodeModifiedImmediate(value, &encoded))
		{
			if (!EncodeModifiedImmediate(0u - value, &encoded))
				return false;

			// The inverse ADD form preserves NZCV as well as the subtraction
			// result; zero is already encoded as SUB directly.
			return EmitU32(CondBits(Condition::AL) | DATA_PROCESSING_IMM | OPCODE_ADD |
						   (set_flags ? SET_FLAGS : 0) |
						   ((rn & 0xfu) << 16) | ((rd & 0xfu) << 12) | encoded);
		}

		return EmitU32(CondBits(Condition::AL) | DATA_PROCESSING_IMM | OPCODE_SUB |
					   (set_flags ? SET_FLAGS : 0) | ((rn & 0xfu) << 16) |
					   ((rd & 0xfu) << 12) | encoded);
	}

	bool CodeBuffer::EmitRsbImm32(unsigned rd, unsigned rn, u32 value, bool set_flags)
	{
		if (!IsRegister(rd) || !IsRegister(rn))
			return false;

		u32 encoded = 0;
		if (!EncodeModifiedImmediate(value, &encoded))
			return false;

		return EmitU32(CondBits(Condition::AL) | DATA_PROCESSING_IMM | OPCODE_RSB |
					   (set_flags ? SET_FLAGS : 0) | ((rn & 0xfu) << 16) |
					   ((rd & 0xfu) << 12) | encoded);
	}

	bool CodeBuffer::EmitRscImm32(unsigned rd, unsigned rn, u32 value, bool set_flags)
	{
		if (!IsRegister(rd) || !IsRegister(rn))
			return false;

		u32 encoded = 0;
		if (!EncodeModifiedImmediate(value, &encoded))
			return false;

		return EmitU32(CondBits(Condition::AL) | DATA_PROCESSING_IMM | OPCODE_RSC |
					   (set_flags ? SET_FLAGS : 0) | ((rn & 0xfu) << 16) |
					   ((rd & 0xfu) << 12) | encoded);
	}

	bool CodeBuffer::EmitAndImm8(unsigned rd, unsigned rn, u8 value, bool set_flags)
	{
		if (!IsRegister(rd) || !IsRegister(rn))
			return false;
		return EmitU32(EncodeAndImm8(rd, rn, value, set_flags));
	}

	bool CodeBuffer::EmitAndImm32(unsigned rd, unsigned rn, u32 value, bool set_flags)
	{
		if (!IsRegister(rd) || !IsRegister(rn))
			return false;
		if (!set_flags && value == 0xffffffffu && CanElideSameRegisterWrite(rd, rn))
			return true;

		u32 encoded = 0;
		if (EncodeModifiedImmediate(value, &encoded))
		{
			return EmitU32(CondBits(Condition::AL) | DATA_PROCESSING_IMM | OPCODE_AND |
						   (set_flags ? SET_FLAGS : 0) | ((rn & 0xfu) << 16) |
						   ((rd & 0xfu) << 12) | encoded);
		}

		if (set_flags || !EncodeModifiedImmediate(~value, &encoded))
			return false;

		return EmitU32(CondBits(Condition::AL) | DATA_PROCESSING_IMM | OPCODE_BIC |
					   ((rn & 0xfu) << 16) |
					   ((rd & 0xfu) << 12) | encoded);
	}

	bool CodeBuffer::EmitBicImm32(unsigned rd, unsigned rn, u32 value, bool set_flags)
	{
		if (!IsRegister(rd) || !IsRegister(rn))
			return false;
		if (!set_flags && value == 0 && CanElideSameRegisterWrite(rd, rn))
			return true;

		u32 encoded = 0;
		if (EncodeModifiedImmediate(value, &encoded))
		{
			return EmitU32(CondBits(Condition::AL) | DATA_PROCESSING_IMM | OPCODE_BIC |
						   (set_flags ? SET_FLAGS : 0) | ((rn & 0xfu) << 16) |
						   ((rd & 0xfu) << 12) | encoded);
		}

		if (set_flags || !EncodeModifiedImmediate(~value, &encoded))
			return false;

		return EmitU32(CondBits(Condition::AL) | DATA_PROCESSING_IMM | OPCODE_AND |
					   ((rn & 0xfu) << 16) |
					   ((rd & 0xfu) << 12) | encoded);
	}

	bool CodeBuffer::EmitEorImm8(unsigned rd, unsigned rn, u8 value, bool set_flags)
	{
		if (!IsRegister(rd) || !IsRegister(rn))
			return false;
		if (!set_flags && value == 0 && CanElideSameRegisterWrite(rd, rn))
			return true;
		return EmitU32(EncodeEorImm8(rd, rn, value, set_flags));
	}

	bool CodeBuffer::EmitEorImm32(unsigned rd, unsigned rn, u32 value, bool set_flags)
	{
		if (!IsRegister(rd) || !IsRegister(rn))
			return false;
		if (!set_flags && value == 0 && CanElideSameRegisterWrite(rd, rn))
			return true;
		if (!set_flags && value == 0xffffffffu)
			return EmitMvnReg(rd, rn);

		u32 encoded = 0;
		if (!EncodeModifiedImmediate(value, &encoded))
			return false;

		return EmitU32(CondBits(Condition::AL) | DATA_PROCESSING_IMM | OPCODE_EOR |
					   (set_flags ? SET_FLAGS : 0) | ((rn & 0xfu) << 16) |
					   ((rd & 0xfu) << 12) | encoded);
	}

	bool CodeBuffer::EmitOrrImm8(unsigned rd, unsigned rn, u8 value, bool set_flags)
	{
		if (!IsRegister(rd) || !IsRegister(rn))
			return false;
		if (!set_flags && value == 0 && CanElideSameRegisterWrite(rd, rn))
			return true;
		return EmitU32(EncodeOrrImm8(rd, rn, value, set_flags));
	}

	bool CodeBuffer::EmitOrrImm32(unsigned rd, unsigned rn, u32 value, bool set_flags)
	{
		if (!IsRegister(rd) || !IsRegister(rn))
			return false;
		if (!set_flags && value == 0 && CanElideSameRegisterWrite(rd, rn))
			return true;

		u32 encoded = 0;
		if (!EncodeModifiedImmediate(value, &encoded))
			return false;

		return EmitU32(CondBits(Condition::AL) | DATA_PROCESSING_IMM | OPCODE_ORR |
					   (set_flags ? SET_FLAGS : 0) | ((rn & 0xfu) << 16) |
					   ((rd & 0xfu) << 12) | encoded);
	}

	bool CodeBuffer::EmitAdcImm8(unsigned rd, unsigned rn, u8 value, bool set_flags)
	{
		if (!IsRegister(rd) || !IsRegister(rn))
			return false;
		return EmitU32(EncodeAdcImm8(rd, rn, value, set_flags));
	}

	bool CodeBuffer::EmitAdcImm32(unsigned rd, unsigned rn, u32 value, bool set_flags)
	{
		if (!IsRegister(rd) || !IsRegister(rn))
			return false;

		u32 encoded = 0;
		if (!EncodeModifiedImmediate(value, &encoded))
		{
			if (!EncodeModifiedImmediate(~value, &encoded))
				return false;

			// ADC x, value and SBC x, ~value are identical, including NZCV,
			// because SBC subtracts both the complemented value and !carry.
			return EmitU32(CondBits(Condition::AL) | DATA_PROCESSING_IMM | OPCODE_SBC |
						   (set_flags ? SET_FLAGS : 0) | ((rn & 0xfu) << 16) |
						   ((rd & 0xfu) << 12) | encoded);
		}

		return EmitU32(CondBits(Condition::AL) | DATA_PROCESSING_IMM | OPCODE_ADC |
					   (set_flags ? SET_FLAGS : 0) | ((rn & 0xfu) << 16) |
					   ((rd & 0xfu) << 12) | encoded);
	}

	bool CodeBuffer::EmitSbcImm8(unsigned rd, unsigned rn, u8 value, bool set_flags)
	{
		if (!IsRegister(rd) || !IsRegister(rn))
			return false;
		return EmitU32(EncodeSbcImm8(rd, rn, value, set_flags));
	}

	bool CodeBuffer::EmitSbcImm32(unsigned rd, unsigned rn, u32 value, bool set_flags)
	{
		if (!IsRegister(rd) || !IsRegister(rn))
			return false;

		u32 encoded = 0;
		if (!EncodeModifiedImmediate(value, &encoded))
		{
			if (!EncodeModifiedImmediate(~value, &encoded))
				return false;

			// The inverse ADC form preserves the SBC result and NZCV while
			// allowing the complemented modified immediate.
			return EmitU32(CondBits(Condition::AL) | DATA_PROCESSING_IMM | OPCODE_ADC |
						   (set_flags ? SET_FLAGS : 0) | ((rn & 0xfu) << 16) |
						   ((rd & 0xfu) << 12) | encoded);
		}

		return EmitU32(CondBits(Condition::AL) | DATA_PROCESSING_IMM | OPCODE_SBC |
					   (set_flags ? SET_FLAGS : 0) | ((rn & 0xfu) << 16) |
					   ((rd & 0xfu) << 12) | encoded);
	}

	bool CodeBuffer::EmitMovRegShiftImm(unsigned rd, unsigned rm, ShiftType shift, u8 amount, bool set_flags,
		Condition condition)
	{
		if (!IsRegister(rd) || !IsRegister(rm) || amount > 31)
			return false;
		if (!set_flags && shift == ShiftType::LSL && amount == 0 && CanElideSameRegisterWrite(rd, rm))
			return true;
		return EmitU32(EncodeMovRegShiftImm(rd, rm, shift, amount, set_flags, condition));
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

	bool CodeBuffer::EmitAddRegShiftImm(unsigned rd, unsigned rn, unsigned rm, ShiftType shift, u8 amount,
		bool set_flags)
	{
		if (!IsRegister(rd) || !IsRegister(rn) || !IsRegister(rm) || amount > 31)
			return false;
		return EmitU32(EncodeAddRegShiftImm(rd, rn, rm, shift, amount, set_flags));
	}

	bool CodeBuffer::EmitAddRegShiftReg(unsigned rd, unsigned rn, unsigned rm, ShiftType shift, unsigned rs,
		bool set_flags)
	{
		if (!IsRegister(rd) || !IsRegister(rn) || !IsRegister(rm) || !IsRegister(rs))
			return false;
		return EmitU32(EncodeAddRegShiftReg(rd, rn, rm, shift, rs, set_flags));
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
		if (!set_flags && rd == rn && rn == rm && IsGeneralRegister(rd))
			return true;
		return EmitU32(EncodeAndReg(rd, rn, rm, set_flags));
	}

	bool CodeBuffer::EmitAndRegShiftImm(unsigned rd, unsigned rn, unsigned rm, ShiftType shift, u8 amount,
		bool set_flags)
	{
		if (!IsRegister(rd) || !IsRegister(rn) || !IsRegister(rm) || amount > 31)
			return false;
		if (!set_flags && rd == rn && rn == rm && shift == ShiftType::LSL && amount == 0 &&
			IsGeneralRegister(rd))
		{
			return true;
		}
		return EmitU32(EncodeAndRegShiftImm(rd, rn, rm, shift, amount, set_flags));
	}

	bool CodeBuffer::EmitAndRegShiftReg(unsigned rd, unsigned rn, unsigned rm, ShiftType shift, unsigned rs,
		bool set_flags)
	{
		if (!IsRegister(rd) || !IsRegister(rn) || !IsRegister(rm) || !IsRegister(rs))
			return false;
		return EmitU32(EncodeAndRegShiftReg(rd, rn, rm, shift, rs, set_flags));
	}

	bool CodeBuffer::EmitBicRegShiftImm(unsigned rd, unsigned rn, unsigned rm, ShiftType shift, u8 amount,
		bool set_flags)
	{
		if (!IsRegister(rd) || !IsRegister(rn) || !IsRegister(rm) || amount > 31)
			return false;
		return EmitU32(EncodeBicRegShiftImm(rd, rn, rm, shift, amount, set_flags));
	}

	bool CodeBuffer::EmitBicRegShiftReg(unsigned rd, unsigned rn, unsigned rm, ShiftType shift, unsigned rs,
		bool set_flags)
	{
		if (!IsRegister(rd) || !IsRegister(rn) || !IsRegister(rm) || !IsRegister(rs))
			return false;
		return EmitU32(EncodeBicRegShiftReg(rd, rn, rm, shift, rs, set_flags));
	}

	bool CodeBuffer::EmitEorReg(unsigned rd, unsigned rn, unsigned rm, bool set_flags)
	{
		if (!IsRegister(rd) || !IsRegister(rn) || !IsRegister(rm))
			return false;
		return EmitU32(EncodeEorReg(rd, rn, rm, set_flags));
	}

	bool CodeBuffer::EmitEorRegShiftImm(unsigned rd, unsigned rn, unsigned rm, ShiftType shift, u8 amount,
		bool set_flags)
	{
		if (!IsRegister(rd) || !IsRegister(rn) || !IsRegister(rm) || amount > 31)
			return false;
		return EmitU32(EncodeEorRegShiftImm(rd, rn, rm, shift, amount, set_flags));
	}

	bool CodeBuffer::EmitEorRegShiftReg(unsigned rd, unsigned rn, unsigned rm, ShiftType shift, unsigned rs,
		bool set_flags)
	{
		if (!IsRegister(rd) || !IsRegister(rn) || !IsRegister(rm) || !IsRegister(rs))
			return false;
		return EmitU32(EncodeEorRegShiftReg(rd, rn, rm, shift, rs, set_flags));
	}

	bool CodeBuffer::EmitOrrReg(unsigned rd, unsigned rn, unsigned rm, bool set_flags)
	{
		if (!IsRegister(rd) || !IsRegister(rn) || !IsRegister(rm))
			return false;
		if (!set_flags && rd == rn && rn == rm && IsGeneralRegister(rd))
			return true;
		return EmitU32(EncodeOrrReg(rd, rn, rm, set_flags));
	}

	bool CodeBuffer::EmitOrrRegShiftImm(unsigned rd, unsigned rn, unsigned rm, ShiftType shift, u8 amount,
		bool set_flags)
	{
		if (!IsRegister(rd) || !IsRegister(rn) || !IsRegister(rm) || amount > 31)
			return false;
		if (!set_flags && rd == rn && rn == rm && shift == ShiftType::LSL && amount == 0 &&
			IsGeneralRegister(rd))
		{
			return true;
		}
		return EmitU32(EncodeOrrRegShiftImm(rd, rn, rm, shift, amount, set_flags));
	}

	bool CodeBuffer::EmitOrrRegShiftReg(unsigned rd, unsigned rn, unsigned rm, ShiftType shift, unsigned rs,
		bool set_flags)
	{
		if (!IsRegister(rd) || !IsRegister(rn) || !IsRegister(rm) || !IsRegister(rs))
			return false;
		return EmitU32(EncodeOrrRegShiftReg(rd, rn, rm, shift, rs, set_flags));
	}

	bool CodeBuffer::EmitMvnReg(unsigned rd, unsigned rm, bool set_flags)
	{
		if (!IsRegister(rd) || !IsRegister(rm))
			return false;
		return EmitU32(EncodeMvnReg(rd, rm, set_flags));
	}

	bool CodeBuffer::EmitClz(unsigned rd, unsigned rm)
	{
		if (!IsLowRegister(rd) || !IsLowRegister(rm))
			return false;
		return EmitU32(EncodeClz(rd, rm));
	}

	bool CodeBuffer::EmitSsat(unsigned rd, u8 bits, unsigned rm)
	{
		if (!IsLowRegister(rd) || !IsLowRegister(rm) || bits == 0 || bits > 32)
			return false;
		return EmitU32(EncodeSsat(rd, bits, rm));
	}

	bool CodeBuffer::EmitUbfx(unsigned rd, unsigned rn, u8 lsb, u8 width)
	{
		if (!IsLowRegister(rd) || !IsLowRegister(rn) || lsb >= 32 || width == 0 || width > 32 - lsb)
			return false;
		return EmitU32(EncodeUbfx(rd, rn, lsb, width));
	}

	bool CodeBuffer::EmitSxtb(unsigned rd, unsigned rm)
	{
		if (!IsLowRegister(rd) || !IsLowRegister(rm))
			return false;
		return EmitU32(EncodeSxtb(rd, rm));
	}

	bool CodeBuffer::EmitSxth(unsigned rd, unsigned rm)
	{
		if (!IsLowRegister(rd) || !IsLowRegister(rm))
			return false;
		return EmitU32(EncodeSxth(rd, rm));
	}

	bool CodeBuffer::EmitUxth(unsigned rd, unsigned rm)
	{
		if (!IsLowRegister(rd) || !IsLowRegister(rm))
			return false;
		return EmitU32(EncodeUxth(rd, rm));
	}

	bool CodeBuffer::EmitSubReg(unsigned rd, unsigned rn, unsigned rm, bool set_flags)
	{
		if (!IsRegister(rd) || !IsRegister(rn) || !IsRegister(rm))
			return false;
		return EmitU32(EncodeSubReg(rd, rn, rm, set_flags));
	}

	bool CodeBuffer::EmitSubRegShiftImm(unsigned rd, unsigned rn, unsigned rm, ShiftType shift, u8 amount,
		bool set_flags)
	{
		if (!IsRegister(rd) || !IsRegister(rn) || !IsRegister(rm) || amount > 31)
			return false;
		return EmitU32(EncodeSubRegShiftImm(rd, rn, rm, shift, amount, set_flags));
	}

	bool CodeBuffer::EmitSubRegShiftReg(unsigned rd, unsigned rn, unsigned rm, ShiftType shift, unsigned rs,
		bool set_flags)
	{
		if (!IsRegister(rd) || !IsRegister(rn) || !IsRegister(rm) || !IsRegister(rs))
			return false;
		return EmitU32(EncodeSubRegShiftReg(rd, rn, rm, shift, rs, set_flags));
	}

	bool CodeBuffer::EmitSbcReg(unsigned rd, unsigned rn, unsigned rm, bool set_flags)
	{
		if (!IsRegister(rd) || !IsRegister(rn) || !IsRegister(rm))
			return false;
		return EmitU32(EncodeSbcReg(rd, rn, rm, set_flags));
	}

	bool CodeBuffer::EmitPkhbt(unsigned rd, unsigned rn, unsigned rm, u8 lsl_amount)
	{
		if (!IsLowRegister(rd) || !IsLowRegister(rn) || !IsLowRegister(rm) || lsl_amount > 31)
			return false;
		return EmitU32(EncodePkhbt(rd, rn, rm, lsl_amount));
	}

	bool CodeBuffer::EmitUmull(unsigned rdlo, unsigned rdhi, unsigned rn, unsigned rm, bool set_flags)
	{
		if (!IsLowRegister(rdlo) || !IsLowRegister(rdhi) || !IsLowRegister(rn) || !IsLowRegister(rm) ||
			rdlo == rdhi)
		{
			return false;
		}
		return EmitU32(EncodeUmull(rdlo, rdhi, rn, rm, set_flags));
	}

	bool CodeBuffer::EmitSmull(unsigned rdlo, unsigned rdhi, unsigned rn, unsigned rm, bool set_flags)
	{
		if (!IsLowRegister(rdlo) || !IsLowRegister(rdhi) || !IsLowRegister(rn) || !IsLowRegister(rm) ||
			rdlo == rdhi)
		{
			return false;
		}
		return EmitU32(EncodeSmull(rdlo, rdhi, rn, rm, set_flags));
	}

	bool CodeBuffer::EmitCmpReg(unsigned rn, unsigned rm, Condition condition)
	{
		if (!IsRegister(rn) || !IsRegister(rm))
			return false;
		return EmitU32(EncodeCmpReg(rn, rm, condition));
	}

	bool CodeBuffer::EmitCmpImm32(unsigned rn, u32 value, Condition condition)
	{
		if (!IsRegister(rn))
			return false;

		u32 encoded = 0;
		if (EncodeModifiedImmediate(value, &encoded))
		{
			return EmitU32(CondBits(condition) | DATA_PROCESSING_IMM | OPCODE_CMP | SET_FLAGS |
						   ((rn & 0xfu) << 16) | encoded);
		}

		if (!EncodeModifiedImmediate(0u - value, &encoded))
			return false;

		return EmitU32(CondBits(condition) | DATA_PROCESSING_IMM | OPCODE_CMN | SET_FLAGS |
					   ((rn & 0xfu) << 16) | encoded);
	}

	bool CodeBuffer::EmitTstImm32(unsigned rn, u32 value, Condition condition)
	{
		if (!IsRegister(rn))
			return false;

		u32 encoded = 0;
		if (!EncodeModifiedImmediate(value, &encoded))
			return false;

		return EmitU32(CondBits(condition) | DATA_PROCESSING_IMM | OPCODE_TST | SET_FLAGS |
					   ((rn & 0xfu) << 16) | encoded);
	}

	bool CodeBuffer::EmitLdrImm12(unsigned rd, unsigned rn, u16 offset, Condition condition)
	{
		if (!IsLowRegister(rd) || !IsLowRegister(rn) || offset > 0x0fff)
			return false;
		return EmitU32(EncodeLdrImm12(rd, rn, offset, condition));
	}

	bool CodeBuffer::EmitLdrImm12PostIndex(unsigned rd, unsigned rn, u16 offset)
	{
		if (!IsLowRegister(rd) || !IsLowRegister(rn) || rd == rn || offset > 0x0fff)
			return false;
		return EmitU32(EncodeLdrImm12PostIndex(rd, rn, offset));
	}

	bool CodeBuffer::EmitLdrRegShift(unsigned rd, unsigned rn, unsigned rm, ShiftType shift, u8 amount)
	{
		if (!IsLowRegister(rd) || !IsLowRegister(rn) || !IsLowRegister(rm) || amount > 31)
			return false;
		return EmitU32(EncodeLdrRegShift(rd, rn, rm, shift, amount));
	}

	bool CodeBuffer::EmitStrImm12(unsigned rd, unsigned rn, u16 offset)
	{
		if (!IsLowRegister(rd) || !IsLowRegister(rn) || offset > 0x0fff)
			return false;
		return EmitU32(EncodeStrImm12(rd, rn, offset));
	}

	bool CodeBuffer::EmitStrRegShift(unsigned rd, unsigned rn, unsigned rm, ShiftType shift, u8 amount)
	{
		if (!IsLowRegister(rd) || !IsLowRegister(rn) || !IsLowRegister(rm) || amount > 31)
			return false;
		return EmitU32(EncodeStrRegShift(rd, rn, rm, shift, amount));
	}

	bool CodeBuffer::EmitLdrdImm8(unsigned rdlo, unsigned rdhi, unsigned rn, u8 offset,
		Condition condition)
	{
		if (!IsLowEvenRegisterPair(rdlo, rdhi) || !IsLowRegister(rn))
			return false;
		return EmitU32(EncodeLdrdImm8(rdlo, rdhi, rn, offset, condition));
	}

	bool CodeBuffer::EmitStrdImm8(unsigned rdlo, unsigned rdhi, unsigned rn, u8 offset,
		Condition condition)
	{
		if (!IsLowEvenRegisterPair(rdlo, rdhi) || !IsLowRegister(rn))
			return false;
		return EmitU32(EncodeStrdImm8(rdlo, rdhi, rn, offset, condition));
	}

	bool CodeBuffer::EmitLdrbImm12(unsigned rd, unsigned rn, u16 offset)
	{
		if (!IsLowRegister(rd) || !IsLowRegister(rn) || offset > 0x0fff)
			return false;
		return EmitU32(EncodeLdrbImm12(rd, rn, offset));
	}

	bool CodeBuffer::EmitLdrbImm12PostIndex(unsigned rd, unsigned rn, u16 offset)
	{
		if (!IsLowRegister(rd) || !IsLowRegister(rn) || rd == rn || offset > 0x0fff)
			return false;
		return EmitU32(EncodeLdrbImm12PostIndex(rd, rn, offset));
	}

	bool CodeBuffer::EmitLdrbRegShift(unsigned rd, unsigned rn, unsigned rm, ShiftType shift, u8 amount)
	{
		if (!IsLowRegister(rd) || !IsLowRegister(rn) || !IsLowRegister(rm) || amount > 31)
			return false;
		return EmitU32(EncodeLdrbRegShift(rd, rn, rm, shift, amount));
	}

	bool CodeBuffer::EmitStrbImm12(unsigned rd, unsigned rn, u16 offset)
	{
		if (!IsLowRegister(rd) || !IsLowRegister(rn) || offset > 0x0fff)
			return false;
		return EmitU32(EncodeStrbImm12(rd, rn, offset));
	}

	bool CodeBuffer::EmitStrbRegShift(unsigned rd, unsigned rn, unsigned rm, ShiftType shift, u8 amount)
	{
		if (!IsLowRegister(rd) || !IsLowRegister(rn) || !IsLowRegister(rm) || amount > 31)
			return false;
		return EmitU32(EncodeStrbRegShift(rd, rn, rm, shift, amount));
	}

	bool CodeBuffer::EmitLdrhImm8(unsigned rd, unsigned rn, u8 offset)
	{
		if (!IsLowRegister(rd) || !IsLowRegister(rn))
			return false;
		return EmitU32(EncodeLdrhImm8(rd, rn, offset));
	}

	bool CodeBuffer::EmitLdrhReg(unsigned rd, unsigned rn, unsigned rm)
	{
		if (!IsLowRegister(rd) || !IsLowRegister(rn) || !IsLowRegister(rm))
			return false;
		return EmitU32(EncodeLdrhReg(rd, rn, rm));
	}

	bool CodeBuffer::EmitStrhImm8(unsigned rd, unsigned rn, u8 offset)
	{
		if (!IsLowRegister(rd) || !IsLowRegister(rn))
			return false;
		return EmitU32(EncodeStrhImm8(rd, rn, offset));
	}

	bool CodeBuffer::EmitStrhReg(unsigned rd, unsigned rn, unsigned rm)
	{
		if (!IsLowRegister(rd) || !IsLowRegister(rn) || !IsLowRegister(rm))
			return false;
		return EmitU32(EncodeStrhReg(rd, rn, rm));
	}

	bool CodeBuffer::EmitLdrsbImm8(unsigned rd, unsigned rn, u8 offset)
	{
		if (!IsLowRegister(rd) || !IsLowRegister(rn))
			return false;
		return EmitU32(EncodeLdrsbImm8(rd, rn, offset));
	}

	bool CodeBuffer::EmitLdrsbReg(unsigned rd, unsigned rn, unsigned rm)
	{
		if (!IsLowRegister(rd) || !IsLowRegister(rn) || !IsLowRegister(rm))
			return false;
		return EmitU32(EncodeLdrsbReg(rd, rn, rm));
	}

	bool CodeBuffer::EmitLdrshImm8(unsigned rd, unsigned rn, u8 offset)
	{
		if (!IsLowRegister(rd) || !IsLowRegister(rn))
			return false;
		return EmitU32(EncodeLdrshImm8(rd, rn, offset));
	}

	bool CodeBuffer::EmitLdrshReg(unsigned rd, unsigned rn, unsigned rm)
	{
		if (!IsLowRegister(rd) || !IsLowRegister(rn) || !IsLowRegister(rm))
			return false;
		return EmitU32(EncodeLdrshReg(rd, rn, rm));
	}

	bool CodeBuffer::EmitVld1Q32(unsigned qd, unsigned rn)
	{
		if (!IsQRegister(qd) || !IsLowRegister(rn))
			return false;
		return EmitU32(EncodeVld1Q32(MapNeonQRegister(qd), rn));
	}

	bool CodeBuffer::EmitVld1Q32Aligned(unsigned qd, unsigned rn)
	{
		if (!IsQRegister(qd) || !IsLowRegister(rn))
			return false;
		return EmitU32(EncodeVld1Q32Aligned(MapNeonQRegister(qd), rn));
	}

	bool CodeBuffer::EmitVst1Q32(unsigned qd, unsigned rn)
	{
		if (!IsQRegister(qd) || !IsLowRegister(rn))
			return false;
		return EmitU32(EncodeVst1Q32(MapNeonQRegister(qd), rn));
	}

	bool CodeBuffer::EmitVst1Q32Aligned(unsigned qd, unsigned rn)
	{
		if (!IsQRegister(qd) || !IsLowRegister(rn))
			return false;
		return EmitU32(EncodeVst1Q32Aligned(MapNeonQRegister(qd), rn));
	}

	bool CodeBuffer::EmitVst1Q32AlignedWriteback(unsigned qd, unsigned rn)
	{
		if (!IsQRegister(qd) || !IsLowRegister(rn))
			return false;
		return EmitU32(EncodeVst1Q32AlignedWriteback(MapNeonQRegister(qd), rn));
	}

	bool CodeBuffer::EmitVst1D32(unsigned dd, unsigned rn)
	{
		if (!IsDRegister(dd) || !IsLowRegister(rn))
			return false;
		return EmitU32(EncodeVst1D32(MapNeonDRegister(dd), rn));
	}

	bool CodeBuffer::EmitVst1D32Lane(unsigned dd, u8 lane, unsigned rn)
	{
		if (!IsDRegister(dd) || lane >= 2 || !IsLowRegister(rn))
			return false;
		return EmitU32(EncodeVst1D32Lane(MapNeonDRegister(dd), lane, rn));
	}

	bool CodeBuffer::EmitVst1D8Lane0(unsigned dd, unsigned rn)
	{
		if (!IsDRegister(dd) || !IsLowRegister(rn))
			return false;
		return EmitU32(EncodeVst1D8Lane0(MapNeonDRegister(dd), rn));
	}

	bool CodeBuffer::EmitVst1D16Lane0(unsigned dd, unsigned rn)
	{
		if (!IsDRegister(dd) || !IsLowRegister(rn))
			return false;
		return EmitU32(EncodeVst1D16Lane0(MapNeonDRegister(dd), rn));
	}

	bool CodeBuffer::EmitVldrDImm(unsigned dd, unsigned rn, u16 offset)
	{
		if (!IsDRegister(dd) || !IsLowRegister(rn) || offset > 0x3fc || (offset & 0x3u) != 0)
			return false;
		return EmitU32(EncodeVldrDImm(MapNeonDRegister(dd), rn, offset));
	}

	bool CodeBuffer::EmitVldrSImm(unsigned sd, unsigned rn, u16 offset)
	{
		if (!IsSRegister(sd) || !IsLowRegister(rn) || offset > 0x3fc || (offset & 0x3u) != 0)
			return false;
		return EmitU32(EncodeVldrSImm(sd, rn, offset));
	}

	bool CodeBuffer::EmitVstrSImm(unsigned sd, unsigned rn, u16 offset)
	{
		if (!IsSRegister(sd) || !IsLowRegister(rn) || offset > 0x3fc || (offset & 0x3u) != 0)
			return false;
		return EmitU32(EncodeVstrSImm(sd, rn, offset));
	}

	bool CodeBuffer::EmitVstrDImm(unsigned dd, unsigned rn, u16 offset, Condition condition)
	{
		if (!IsDRegister(dd) || !IsLowRegister(rn) || offset > 0x3fc || (offset & 0x3u) != 0)
			return false;
		return EmitU32(EncodeVstrDImm(MapNeonDRegister(dd), rn, offset, condition));
	}

	bool CodeBuffer::EmitVdupI16D(unsigned dd, unsigned dm, u8 lane)
	{
		if (!IsDRegister(dd) || !IsDRegister(dm) || lane >= 4)
			return false;
		return EmitU32(EncodeVdupI16D(MapNeonDRegister(dd), MapNeonDRegister(dm), lane));
	}

	bool CodeBuffer::EmitVdupI32QFromCore(unsigned qd, unsigned rt)
	{
		if (!IsQRegister(qd) || !IsGeneralRegister(rt))
			return false;
		return EmitU32(EncodeVdupI32QFromCore(MapNeonQRegister(qd), rt));
	}

	bool CodeBuffer::EmitVmovS(unsigned sd, unsigned sm)
	{
		if (!IsSRegister(sd) || !IsSRegister(sm))
			return false;
		return EmitU32(EncodeVmovS(sd, sm));
	}

	bool CodeBuffer::EmitVmovCoreToS(unsigned sd, unsigned rt)
	{
		if (!IsSRegister(sd) || !IsRegister(rt))
			return false;
		return EmitU32(EncodeVmovCoreToS(sd, rt));
	}

	bool CodeBuffer::EmitVmovSToCore(unsigned rt, unsigned sd, Condition condition)
	{
		if (!IsRegister(rt) || !IsSRegister(sd))
			return false;
		return EmitU32(EncodeVmovSToCore(rt, sd, condition));
	}

	bool CodeBuffer::EmitVmovCoreToD32Lane(unsigned dd, u8 lane, unsigned rt, Condition condition)
	{
		if (!IsDRegister(dd) || lane >= 2 || !IsRegister(rt))
			return false;
		return EmitU32(EncodeVmovCoreToD32Lane(MapNeonDRegister(dd), lane, rt, condition));
	}

	bool CodeBuffer::EmitVmovD32LaneToCore(unsigned rt, unsigned dd, u8 lane, Condition condition)
	{
		if (!IsRegister(rt) || !IsDRegister(dd) || lane >= 2)
			return false;
		return EmitU32(EncodeVmovD32LaneToCore(rt, MapNeonDRegister(dd), lane, condition));
	}

	bool CodeBuffer::EmitVmovCorePairToD(unsigned dd, unsigned rt, unsigned rt2)
	{
		if (!IsDRegister(dd) || !IsRegister(rt) || !IsRegister(rt2))
			return false;
		return EmitU32(EncodeVmovCorePairToD(MapNeonDRegister(dd), rt, rt2));
	}

	bool CodeBuffer::EmitVcvtF32S32(unsigned sd, unsigned sm)
	{
		if (!IsSRegister(sd) || !IsSRegister(sm))
			return false;
		return EmitU32(EncodeVcvtF32S32(sd, sm));
	}

	bool CodeBuffer::EmitVcvtF64F32(unsigned dd, unsigned sm)
	{
		if (!IsDRegister(dd) || !IsSRegister(sm))
			return false;
		return EmitU32(EncodeVcvtF64F32(MapNeonDRegister(dd), sm));
	}

	bool CodeBuffer::EmitVcvtF32F64(unsigned sd, unsigned dm)
	{
		if (!IsSRegister(sd) || !IsDRegister(dm))
			return false;
		return EmitU32(EncodeVcvtF32F64(sd, MapNeonDRegister(dm)));
	}

	bool CodeBuffer::EmitVcvtF32S32Q(unsigned qd, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVcvtF32S32Q(MapNeonQRegister(qd), MapNeonQRegister(qm)));
	}

	bool CodeBuffer::EmitVcvtS32F32Q(unsigned qd, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVcvtS32F32Q(MapNeonQRegister(qd), MapNeonQRegister(qm)));
	}

	bool CodeBuffer::EmitVaddF32(unsigned sd, unsigned sn, unsigned sm)
	{
		if (!IsSRegister(sd) || !IsSRegister(sn) || !IsSRegister(sm))
			return false;
		return EmitU32(EncodeVaddF32(sd, sn, sm));
	}

	bool CodeBuffer::EmitVaddF64(unsigned dd, unsigned dn, unsigned dm)
	{
		if (!IsDRegister(dd) || !IsDRegister(dn) || !IsDRegister(dm))
			return false;
		return EmitU32(EncodeVaddF64(MapNeonDRegister(dd), MapNeonDRegister(dn), MapNeonDRegister(dm)));
	}

	bool CodeBuffer::EmitVsubF32(unsigned sd, unsigned sn, unsigned sm)
	{
		if (!IsSRegister(sd) || !IsSRegister(sn) || !IsSRegister(sm))
			return false;
		return EmitU32(EncodeVsubF32(sd, sn, sm));
	}

	bool CodeBuffer::EmitVmulF32(unsigned sd, unsigned sn, unsigned sm)
	{
		if (!IsSRegister(sd) || !IsSRegister(sn) || !IsSRegister(sm))
			return false;
		return EmitU32(EncodeVmulF32(sd, sn, sm));
	}

	bool CodeBuffer::EmitVmulF64(unsigned dd, unsigned dn, unsigned dm)
	{
		if (!IsDRegister(dd) || !IsDRegister(dn) || !IsDRegister(dm))
			return false;
		return EmitU32(EncodeVmulF64(MapNeonDRegister(dd), MapNeonDRegister(dn), MapNeonDRegister(dm)));
	}

	bool CodeBuffer::EmitVaddF32Q(unsigned qd, unsigned qn, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qn) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVaddF32Q(MapNeonQRegister(qd), MapNeonQRegister(qn), MapNeonQRegister(qm)));
	}

	bool CodeBuffer::EmitVsubF32Q(unsigned qd, unsigned qn, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qn) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVsubF32Q(MapNeonQRegister(qd), MapNeonQRegister(qn), MapNeonQRegister(qm)));
	}

	bool CodeBuffer::EmitVmulF32Q(unsigned qd, unsigned qn, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qn) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVmulF32Q(MapNeonQRegister(qd), MapNeonQRegister(qn), MapNeonQRegister(qm)));
	}

	bool CodeBuffer::EmitVdivF32(unsigned sd, unsigned sn, unsigned sm)
	{
		if (!IsSRegister(sd) || !IsSRegister(sn) || !IsSRegister(sm))
			return false;
		return EmitU32(EncodeVdivF32(sd, sn, sm));
	}

	bool CodeBuffer::EmitVdivF64(unsigned dd, unsigned dn, unsigned dm)
	{
		if (!IsDRegister(dd) || !IsDRegister(dn) || !IsDRegister(dm))
			return false;
		return EmitU32(EncodeVdivF64(MapNeonDRegister(dd), MapNeonDRegister(dn), MapNeonDRegister(dm)));
	}

	bool CodeBuffer::EmitVsqrtF32(unsigned sd, unsigned sm)
	{
		if (!IsSRegister(sd) || !IsSRegister(sm))
			return false;
		return EmitU32(EncodeVsqrtF32(sd, sm));
	}

	bool CodeBuffer::EmitVaddI8Q(unsigned qd, unsigned qn, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qn) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVaddI8Q(MapNeonQRegister(qd), MapNeonQRegister(qn), MapNeonQRegister(qm)));
	}

	bool CodeBuffer::EmitVaddI16Q(unsigned qd, unsigned qn, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qn) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVaddI16Q(MapNeonQRegister(qd), MapNeonQRegister(qn), MapNeonQRegister(qm)));
	}

	bool CodeBuffer::EmitVaddI32Q(unsigned qd, unsigned qn, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qn) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVaddI32Q(MapNeonQRegister(qd), MapNeonQRegister(qn), MapNeonQRegister(qm)));
	}

	bool CodeBuffer::EmitVaddI64Q(unsigned qd, unsigned qn, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qn) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVaddI64Q(MapNeonQRegister(qd), MapNeonQRegister(qn), MapNeonQRegister(qm)));
	}

	bool CodeBuffer::EmitVsubI8Q(unsigned qd, unsigned qn, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qn) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVsubI8Q(MapNeonQRegister(qd), MapNeonQRegister(qn), MapNeonQRegister(qm)));
	}

	bool CodeBuffer::EmitVsubI16Q(unsigned qd, unsigned qn, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qn) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVsubI16Q(MapNeonQRegister(qd), MapNeonQRegister(qn), MapNeonQRegister(qm)));
	}

	bool CodeBuffer::EmitVsubI32Q(unsigned qd, unsigned qn, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qn) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVsubI32Q(MapNeonQRegister(qd), MapNeonQRegister(qn), MapNeonQRegister(qm)));
	}

	bool CodeBuffer::EmitVshlI16Q(unsigned qd, unsigned qm, u8 amount)
	{
		if (!IsQRegister(qd) || !IsQRegister(qm) || amount == 0 || amount >= 16)
			return false;
		return EmitU32(EncodeVshlI16Q(MapNeonQRegister(qd), MapNeonQRegister(qm), amount));
	}

	bool CodeBuffer::EmitVshlI32Q(unsigned qd, unsigned qm, u8 amount)
	{
		if (!IsQRegister(qd) || !IsQRegister(qm) || amount == 0 || amount >= 32)
			return false;
		return EmitU32(EncodeVshlI32Q(MapNeonQRegister(qd), MapNeonQRegister(qm), amount));
	}

	bool CodeBuffer::EmitVshrU16Q(unsigned qd, unsigned qm, u8 amount)
	{
		if (!IsQRegister(qd) || !IsQRegister(qm) || amount == 0 || amount >= 16)
			return false;
		return EmitU32(EncodeVshrU16Q(MapNeonQRegister(qd), MapNeonQRegister(qm), amount));
	}

	bool CodeBuffer::EmitVshrU32Q(unsigned qd, unsigned qm, u8 amount)
	{
		if (!IsQRegister(qd) || !IsQRegister(qm) || amount == 0 || amount >= 32)
			return false;
		return EmitU32(EncodeVshrU32Q(MapNeonQRegister(qd), MapNeonQRegister(qm), amount));
	}

	bool CodeBuffer::EmitVshrS16Q(unsigned qd, unsigned qm, u8 amount)
	{
		if (!IsQRegister(qd) || !IsQRegister(qm) || amount == 0 || amount >= 16)
			return false;
		return EmitU32(EncodeVshrS16Q(MapNeonQRegister(qd), MapNeonQRegister(qm), amount));
	}

	bool CodeBuffer::EmitVshrS32Q(unsigned qd, unsigned qm, u8 amount)
	{
		if (!IsQRegister(qd) || !IsQRegister(qm) || amount == 0 || amount >= 32)
			return false;
		return EmitU32(EncodeVshrS32Q(MapNeonQRegister(qd), MapNeonQRegister(qm), amount));
	}

	bool CodeBuffer::EmitVshlU32Q(unsigned qd, unsigned qm, unsigned qn)
	{
		if (!IsQRegister(qd) || !IsQRegister(qm) || !IsQRegister(qn))
			return false;
		return EmitU32(EncodeVshlU32Q(MapNeonQRegister(qd), MapNeonQRegister(qm), MapNeonQRegister(qn)));
	}

	bool CodeBuffer::EmitVshlS32Q(unsigned qd, unsigned qm, unsigned qn)
	{
		if (!IsQRegister(qd) || !IsQRegister(qm) || !IsQRegister(qn))
			return false;
		return EmitU32(EncodeVshlS32Q(MapNeonQRegister(qd), MapNeonQRegister(qm), MapNeonQRegister(qn)));
	}

	bool CodeBuffer::EmitVmullS16Q(unsigned qd, unsigned dn, unsigned dm)
	{
		if (!IsQRegister(qd) || !IsDRegister(dn) || !IsDRegister(dm))
			return false;
		return EmitU32(EncodeVmullS16Q(MapNeonQRegister(qd), MapNeonDRegister(dn), MapNeonDRegister(dm)));
	}

	bool CodeBuffer::EmitVmullS32Q(unsigned qd, unsigned dn, unsigned dm)
	{
		if (!IsQRegister(qd) || !IsDRegister(dn) || !IsDRegister(dm))
			return false;
		return EmitU32(EncodeVmullS32Q(MapNeonQRegister(qd), MapNeonDRegister(dn), MapNeonDRegister(dm)));
	}

	bool CodeBuffer::EmitVmullU32Q(unsigned qd, unsigned dn, unsigned dm)
	{
		if (!IsQRegister(qd) || !IsDRegister(dn) || !IsDRegister(dm))
			return false;
		return EmitU32(EncodeVmullU32Q(MapNeonQRegister(qd), MapNeonDRegister(dn), MapNeonDRegister(dm)));
	}

	bool CodeBuffer::EmitVnegS32Q(unsigned qd, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVnegS32Q(MapNeonQRegister(qd), MapNeonQRegister(qm)));
	}

	bool CodeBuffer::EmitVqmovnS32D(unsigned dd, unsigned qm)
	{
		if (!IsDRegister(dd) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVqmovnS32D(MapNeonDRegister(dd), MapNeonQRegister(qm)));
	}

	bool CodeBuffer::EmitVqmovnS64D(unsigned dd, unsigned qm)
	{
		if (!IsDRegister(dd) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVqmovnS64D(MapNeonDRegister(dd), MapNeonQRegister(qm)));
	}

	bool CodeBuffer::EmitVcgtS8Q(unsigned qd, unsigned qn, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qn) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVcgtS8Q(MapNeonQRegister(qd), MapNeonQRegister(qn), MapNeonQRegister(qm)));
	}

	bool CodeBuffer::EmitVcgtS16Q(unsigned qd, unsigned qn, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qn) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVcgtS16Q(MapNeonQRegister(qd), MapNeonQRegister(qn), MapNeonQRegister(qm)));
	}

	bool CodeBuffer::EmitVcgtS32Q(unsigned qd, unsigned qn, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qn) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVcgtS32Q(MapNeonQRegister(qd), MapNeonQRegister(qn), MapNeonQRegister(qm)));
	}

	bool CodeBuffer::EmitVceqI8Q(unsigned qd, unsigned qn, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qn) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVceqI8Q(MapNeonQRegister(qd), MapNeonQRegister(qn), MapNeonQRegister(qm)));
	}

	bool CodeBuffer::EmitVceqI16Q(unsigned qd, unsigned qn, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qn) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVceqI16Q(MapNeonQRegister(qd), MapNeonQRegister(qn), MapNeonQRegister(qm)));
	}

	bool CodeBuffer::EmitVceqI32Q(unsigned qd, unsigned qn, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qn) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVceqI32Q(MapNeonQRegister(qd), MapNeonQRegister(qn), MapNeonQRegister(qm)));
	}

	bool CodeBuffer::EmitVminS16Q(unsigned qd, unsigned qn, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qn) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVminS16Q(MapNeonQRegister(qd), MapNeonQRegister(qn), MapNeonQRegister(qm)));
	}

	bool CodeBuffer::EmitVminS32Q(unsigned qd, unsigned qn, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qn) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVminS32Q(MapNeonQRegister(qd), MapNeonQRegister(qn), MapNeonQRegister(qm)));
	}

	bool CodeBuffer::EmitVmaxS16Q(unsigned qd, unsigned qn, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qn) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVmaxS16Q(MapNeonQRegister(qd), MapNeonQRegister(qn), MapNeonQRegister(qm)));
	}

	bool CodeBuffer::EmitVmaxS32Q(unsigned qd, unsigned qn, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qn) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVmaxS32Q(MapNeonQRegister(qd), MapNeonQRegister(qn), MapNeonQRegister(qm)));
	}

	bool CodeBuffer::EmitVqaddS8Q(unsigned qd, unsigned qn, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qn) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVqaddS8Q(MapNeonQRegister(qd), MapNeonQRegister(qn), MapNeonQRegister(qm)));
	}

	bool CodeBuffer::EmitVqaddS16Q(unsigned qd, unsigned qn, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qn) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVqaddS16Q(MapNeonQRegister(qd), MapNeonQRegister(qn), MapNeonQRegister(qm)));
	}

	bool CodeBuffer::EmitVqaddS32Q(unsigned qd, unsigned qn, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qn) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVqaddS32Q(MapNeonQRegister(qd), MapNeonQRegister(qn), MapNeonQRegister(qm)));
	}

	bool CodeBuffer::EmitVqsubS8Q(unsigned qd, unsigned qn, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qn) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVqsubS8Q(MapNeonQRegister(qd), MapNeonQRegister(qn), MapNeonQRegister(qm)));
	}

	bool CodeBuffer::EmitVqsubS16Q(unsigned qd, unsigned qn, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qn) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVqsubS16Q(MapNeonQRegister(qd), MapNeonQRegister(qn), MapNeonQRegister(qm)));
	}

	bool CodeBuffer::EmitVqsubS32Q(unsigned qd, unsigned qn, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qn) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVqsubS32Q(MapNeonQRegister(qd), MapNeonQRegister(qn), MapNeonQRegister(qm)));
	}

	bool CodeBuffer::EmitVqaddU8Q(unsigned qd, unsigned qn, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qn) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVqaddU8Q(MapNeonQRegister(qd), MapNeonQRegister(qn), MapNeonQRegister(qm)));
	}

	bool CodeBuffer::EmitVqaddU16Q(unsigned qd, unsigned qn, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qn) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVqaddU16Q(MapNeonQRegister(qd), MapNeonQRegister(qn), MapNeonQRegister(qm)));
	}

	bool CodeBuffer::EmitVqaddU32Q(unsigned qd, unsigned qn, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qn) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVqaddU32Q(MapNeonQRegister(qd), MapNeonQRegister(qn), MapNeonQRegister(qm)));
	}

	bool CodeBuffer::EmitVqsubU8Q(unsigned qd, unsigned qn, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qn) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVqsubU8Q(MapNeonQRegister(qd), MapNeonQRegister(qn), MapNeonQRegister(qm)));
	}

	bool CodeBuffer::EmitVqsubU16Q(unsigned qd, unsigned qn, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qn) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVqsubU16Q(MapNeonQRegister(qd), MapNeonQRegister(qn), MapNeonQRegister(qm)));
	}

	bool CodeBuffer::EmitVqsubU32Q(unsigned qd, unsigned qn, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qn) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVqsubU32Q(MapNeonQRegister(qd), MapNeonQRegister(qn), MapNeonQRegister(qm)));
	}

	bool CodeBuffer::EmitVqabsS16Q(unsigned qd, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVqabsS16Q(MapNeonQRegister(qd), MapNeonQRegister(qm)));
	}

	bool CodeBuffer::EmitVqabsS32Q(unsigned qd, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVqabsS32Q(MapNeonQRegister(qd), MapNeonQRegister(qm)));
	}

	bool CodeBuffer::EmitVclsS32Q(unsigned qd, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVclsS32Q(MapNeonQRegister(qd), MapNeonQRegister(qm)));
	}

	bool CodeBuffer::EmitVrev64I32D(unsigned dd, unsigned dm)
	{
		if (!IsDRegister(dd) || !IsDRegister(dm))
			return false;
		return EmitU32(EncodeVrev64I32D(MapNeonDRegister(dd), MapNeonDRegister(dm)));
	}

	bool CodeBuffer::EmitVrev64I32Q(unsigned qd, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVrev64I32Q(MapNeonQRegister(qd), MapNeonQRegister(qm)));
	}

	bool CodeBuffer::EmitVrev64I16Q(unsigned qd, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVrev64I16Q(MapNeonQRegister(qd), MapNeonQRegister(qm)));
	}

	bool CodeBuffer::EmitVswpD(unsigned dd, unsigned dm)
	{
		if (!IsDRegister(dd) || !IsDRegister(dm))
			return false;
		if (dd == dm)
			return true;
		return EmitU32(EncodeVswpD(MapNeonDRegister(dd), MapNeonDRegister(dm)));
	}

	bool CodeBuffer::EmitVtrnI16D(unsigned dd, unsigned dm)
	{
		if (!IsDRegister(dd) || !IsDRegister(dm))
			return false;
		return EmitU32(EncodeVtrnI16D(MapNeonDRegister(dd), MapNeonDRegister(dm)));
	}

	bool CodeBuffer::EmitVtrnI16Q(unsigned qd, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVtrnI16Q(MapNeonQRegister(qd), MapNeonQRegister(qm)));
	}

	bool CodeBuffer::EmitVtrnI32D(unsigned dd, unsigned dm)
	{
		if (!IsDRegister(dd) || !IsDRegister(dm))
			return false;
		return EmitU32(EncodeVtrnI32D(MapNeonDRegister(dd), MapNeonDRegister(dm)));
	}

	bool CodeBuffer::EmitVtrnI32Q(unsigned qd, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVtrnI32Q(MapNeonQRegister(qd), MapNeonQRegister(qm)));
	}

	bool CodeBuffer::EmitVzipI8Q(unsigned qd, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVzipI8Q(MapNeonQRegister(qd), MapNeonQRegister(qm)));
	}

	bool CodeBuffer::EmitVzipI16Q(unsigned qd, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVzipI16Q(MapNeonQRegister(qd), MapNeonQRegister(qm)));
	}

	bool CodeBuffer::EmitVzipI32Q(unsigned qd, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVzipI32Q(MapNeonQRegister(qd), MapNeonQRegister(qm)));
	}

	bool CodeBuffer::EmitVuzpI8Q(unsigned qd, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVuzpI8Q(MapNeonQRegister(qd), MapNeonQRegister(qm)));
	}

	bool CodeBuffer::EmitVuzpI16Q(unsigned qd, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVuzpI16Q(MapNeonQRegister(qd), MapNeonQRegister(qm)));
	}

	bool CodeBuffer::EmitVuzpI32Q(unsigned qd, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVuzpI32Q(MapNeonQRegister(qd), MapNeonQRegister(qm)));
	}

	bool CodeBuffer::EmitVextI8Q(unsigned qd, unsigned qn, unsigned qm, u8 byte_offset)
	{
		if (!IsQRegister(qd) || !IsQRegister(qn) || !IsQRegister(qm) || byte_offset >= 16)
			return false;
		return EmitU32(EncodeVextI8Q(MapNeonQRegister(qd), MapNeonQRegister(qn), MapNeonQRegister(qm), byte_offset));
	}

	bool CodeBuffer::EmitVandQ(unsigned qd, unsigned qn, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qn) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVandQ(MapNeonQRegister(qd), MapNeonQRegister(qn), MapNeonQRegister(qm)));
	}

	bool CodeBuffer::EmitVeorQ(unsigned qd, unsigned qn, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qn) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVeorQ(MapNeonQRegister(qd), MapNeonQRegister(qn), MapNeonQRegister(qm)));
	}

	bool CodeBuffer::EmitVeorD(unsigned dd, unsigned dn, unsigned dm)
	{
		if (!IsDRegister(dd) || !IsDRegister(dn) || !IsDRegister(dm))
			return false;
		return EmitU32(EncodeVeorD(MapNeonDRegister(dd), MapNeonDRegister(dn), MapNeonDRegister(dm)));
	}

	bool CodeBuffer::EmitVorrQ(unsigned qd, unsigned qn, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qn) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVorrQ(MapNeonQRegister(qd), MapNeonQRegister(qn), MapNeonQRegister(qm)));
	}

	bool CodeBuffer::EmitVorrD(unsigned dd, unsigned dn, unsigned dm)
	{
		if (!IsDRegister(dd) || !IsDRegister(dn) || !IsDRegister(dm))
			return false;
		return EmitU32(EncodeVorrD(MapNeonDRegister(dd), MapNeonDRegister(dn), MapNeonDRegister(dm)));
	}

	bool CodeBuffer::EmitVmvnQ(unsigned qd, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVmvnQ(MapNeonQRegister(qd), MapNeonQRegister(qm)));
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

	bool CodeBuffer::PatchBranchToAddress(size_t instruction_offset, const void* target, Condition condition)
	{
		if (!m_base || !target || instruction_offset + sizeof(u32) > m_offset ||
			(instruction_offset & 3) != 0)
		{
			return false;
		}

		u32 instruction = 0;
		if (!EncodeBranch(m_base + instruction_offset,
				const_cast<u8*>(static_cast<const u8*>(target)), &instruction, condition))
		{
			return false;
		}

		std::memcpy(m_base + instruction_offset, &instruction, sizeof(instruction));
		return true;
	}

	bool CodeBuffer::PatchNop(size_t instruction_offset)
	{
		if (!m_base || instruction_offset + sizeof(u32) > m_offset || (instruction_offset & 3) != 0)
			return false;

		const u32 instruction = CondBits(Condition::AL) | NOP;
		std::memcpy(m_base + instruction_offset, &instruction, sizeof(instruction));
		return true;
	}

	bool CodeBuffer::ReadInstruction(size_t instruction_offset, u32* instruction) const
	{
		if (!m_base || !instruction || instruction_offset + sizeof(u32) > m_offset ||
			(instruction_offset & 3) != 0)
		{
			return false;
		}

		std::memcpy(instruction, m_base + instruction_offset, sizeof(*instruction));
		return true;
	}

	bool CodeBuffer::PatchInstruction(size_t instruction_offset, u32 instruction)
	{
		if (!m_base || instruction_offset + sizeof(u32) > m_offset ||
			(instruction_offset & 3) != 0)
		{
			return false;
		}

		std::memcpy(m_base + instruction_offset, &instruction, sizeof(instruction));
		return true;
	}

	bool CodeBuffer::EmitPush(u16 register_list)
	{
		if (register_list == 0)
			return false;
		return EmitU32(EncodePush(register_list));
	}

	bool CodeBuffer::EmitVpushDRange(unsigned first_d, unsigned d_count)
	{
		if (!IsDRegister(first_d) || d_count == 0 || d_count > 16 ||
			first_d + d_count > 32)
		{
			return false;
		}
		return EmitU32(EncodeVpushDRange(first_d, d_count));
	}

	bool CodeBuffer::EmitPop(u16 register_list)
	{
		if (register_list == 0)
			return false;
		return EmitU32(EncodePop(register_list));
	}

	bool CodeBuffer::EmitVpopDRange(unsigned first_d, unsigned d_count)
	{
		if (!IsDRegister(first_d) || d_count == 0 || d_count > 16 ||
			first_d + d_count > 32)
		{
			return false;
		}
		return EmitU32(EncodeVpopDRange(first_d, d_count));
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

#if defined(VITASX2_QEMU_VALIDATION)
		__builtin___clear_cache(reinterpret_cast<char*>(m_base), reinterpret_cast<char*>(m_base + m_offset));
#else
		HostSys::FlushInstructionCache(m_base, static_cast<u32>(m_offset));
#endif
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

	u32 EncodeMovw(unsigned rd, u16 value, Condition condition)
	{
		pxAssert(IsRegister(rd));
		return CondBits(condition) | MOVW | ((static_cast<u32>(value) & 0xf000u) << 4) | ((rd & 0xfu) << 12) |
			   (static_cast<u32>(value) & 0x0fffu);
	}

	u32 EncodeMovt(unsigned rd, u16 value, Condition condition)
	{
		pxAssert(IsRegister(rd));
		return CondBits(condition) | MOVT | ((static_cast<u32>(value) & 0xf000u) << 4) | ((rd & 0xfu) << 12) |
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

	u32 EncodeAndImm32(unsigned rd, unsigned rn, u32 value, bool set_flags)
	{
		pxAssert(IsRegister(rd));
		pxAssert(IsRegister(rn));
		u32 encoded = 0;
		pxAssert(EncodeModifiedImmediate(value, &encoded));
		return CondBits(Condition::AL) | DATA_PROCESSING_IMM | OPCODE_AND | (set_flags ? SET_FLAGS : 0) |
			   ((rn & 0xfu) << 16) | ((rd & 0xfu) << 12) | encoded;
	}

	u32 EncodeBicImm32(unsigned rd, unsigned rn, u32 value, bool set_flags)
	{
		pxAssert(IsRegister(rd));
		pxAssert(IsRegister(rn));
		u32 encoded = 0;
		pxAssert(EncodeModifiedImmediate(value, &encoded));
		return CondBits(Condition::AL) | DATA_PROCESSING_IMM | OPCODE_BIC | (set_flags ? SET_FLAGS : 0) |
			   ((rn & 0xfu) << 16) | ((rd & 0xfu) << 12) | encoded;
	}

	u32 EncodeEorImm8(unsigned rd, unsigned rn, u8 value, bool set_flags)
	{
		pxAssert(IsRegister(rd));
		pxAssert(IsRegister(rn));
		return CondBits(Condition::AL) | DATA_PROCESSING_IMM | OPCODE_EOR | (set_flags ? SET_FLAGS : 0) |
			   ((rn & 0xfu) << 16) | ((rd & 0xfu) << 12) | value;
	}

	u32 EncodeEorImm32(unsigned rd, unsigned rn, u32 value, bool set_flags)
	{
		pxAssert(IsRegister(rd));
		pxAssert(IsRegister(rn));
		u32 encoded = 0;
		pxAssert(EncodeModifiedImmediate(value, &encoded));
		return CondBits(Condition::AL) | DATA_PROCESSING_IMM | OPCODE_EOR | (set_flags ? SET_FLAGS : 0) |
			   ((rn & 0xfu) << 16) | ((rd & 0xfu) << 12) | encoded;
	}

	u32 EncodeOrrImm8(unsigned rd, unsigned rn, u8 value, bool set_flags)
	{
		pxAssert(IsRegister(rd));
		pxAssert(IsRegister(rn));
		return CondBits(Condition::AL) | DATA_PROCESSING_IMM | OPCODE_ORR | (set_flags ? SET_FLAGS : 0) |
			   ((rn & 0xfu) << 16) | ((rd & 0xfu) << 12) | value;
	}

	u32 EncodeOrrImm32(unsigned rd, unsigned rn, u32 value, bool set_flags)
	{
		pxAssert(IsRegister(rd));
		pxAssert(IsRegister(rn));
		u32 encoded = 0;
		pxAssert(EncodeModifiedImmediate(value, &encoded));
		return CondBits(Condition::AL) | DATA_PROCESSING_IMM | OPCODE_ORR | (set_flags ? SET_FLAGS : 0) |
			   ((rn & 0xfu) << 16) | ((rd & 0xfu) << 12) | encoded;
	}

	u32 EncodeAdcImm8(unsigned rd, unsigned rn, u8 value, bool set_flags)
	{
		pxAssert(IsRegister(rd));
		pxAssert(IsRegister(rn));
		return CondBits(Condition::AL) | DATA_PROCESSING_IMM | OPCODE_ADC | (set_flags ? SET_FLAGS : 0) |
			   ((rn & 0xfu) << 16) | ((rd & 0xfu) << 12) | value;
	}

	u32 EncodeSbcImm8(unsigned rd, unsigned rn, u8 value, bool set_flags)
	{
		pxAssert(IsRegister(rd));
		pxAssert(IsRegister(rn));
		return CondBits(Condition::AL) | DATA_PROCESSING_IMM | OPCODE_SBC | (set_flags ? SET_FLAGS : 0) |
			   ((rn & 0xfu) << 16) | ((rd & 0xfu) << 12) | value;
	}

	u32 EncodeMovRegShiftImm(unsigned rd, unsigned rm, ShiftType shift, u8 amount, bool set_flags,
		Condition condition)
	{
		pxAssert(IsRegister(rd));
		pxAssert(IsRegister(rm));
		pxAssert(amount <= 31);
		return CondBits(condition) | OPCODE_MOV | (set_flags ? SET_FLAGS : 0) |
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

	u32 EncodeAddRegShiftImm(unsigned rd, unsigned rn, unsigned rm, ShiftType shift, u8 amount,
		bool set_flags)
	{
		return EncodeDataProcessingRegShiftImm(OPCODE_ADD, rd, rn, rm, shift, amount, set_flags);
	}

	u32 EncodeAddRegShiftReg(unsigned rd, unsigned rn, unsigned rm, ShiftType shift, unsigned rs,
		bool set_flags)
	{
		return EncodeDataProcessingRegShiftReg(OPCODE_ADD, rd, rn, rm, shift, rs, set_flags);
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

	u32 EncodeAndRegShiftImm(unsigned rd, unsigned rn, unsigned rm, ShiftType shift, u8 amount,
		bool set_flags)
	{
		return EncodeDataProcessingRegShiftImm(OPCODE_AND, rd, rn, rm, shift, amount, set_flags);
	}

	u32 EncodeAndRegShiftReg(unsigned rd, unsigned rn, unsigned rm, ShiftType shift, unsigned rs,
		bool set_flags)
	{
		return EncodeDataProcessingRegShiftReg(OPCODE_AND, rd, rn, rm, shift, rs, set_flags);
	}

	u32 EncodeBicRegShiftImm(unsigned rd, unsigned rn, unsigned rm, ShiftType shift, u8 amount,
		bool set_flags)
	{
		return EncodeDataProcessingRegShiftImm(OPCODE_BIC, rd, rn, rm, shift, amount, set_flags);
	}

	u32 EncodeBicRegShiftReg(unsigned rd, unsigned rn, unsigned rm, ShiftType shift, unsigned rs,
		bool set_flags)
	{
		return EncodeDataProcessingRegShiftReg(OPCODE_BIC, rd, rn, rm, shift, rs, set_flags);
	}

	u32 EncodeEorReg(unsigned rd, unsigned rn, unsigned rm, bool set_flags)
	{
		pxAssert(IsRegister(rd));
		pxAssert(IsRegister(rn));
		pxAssert(IsRegister(rm));
		return CondBits(Condition::AL) | OPCODE_EOR | (set_flags ? SET_FLAGS : 0) |
			   ((rn & 0xfu) << 16) | ((rd & 0xfu) << 12) | (rm & 0xfu);
	}

	u32 EncodeEorRegShiftImm(unsigned rd, unsigned rn, unsigned rm, ShiftType shift, u8 amount,
		bool set_flags)
	{
		return EncodeDataProcessingRegShiftImm(OPCODE_EOR, rd, rn, rm, shift, amount, set_flags);
	}

	u32 EncodeEorRegShiftReg(unsigned rd, unsigned rn, unsigned rm, ShiftType shift, unsigned rs,
		bool set_flags)
	{
		return EncodeDataProcessingRegShiftReg(OPCODE_EOR, rd, rn, rm, shift, rs, set_flags);
	}

	u32 EncodeOrrReg(unsigned rd, unsigned rn, unsigned rm, bool set_flags)
	{
		pxAssert(IsRegister(rd));
		pxAssert(IsRegister(rn));
		pxAssert(IsRegister(rm));
		return CondBits(Condition::AL) | OPCODE_ORR | (set_flags ? SET_FLAGS : 0) |
			   ((rn & 0xfu) << 16) | ((rd & 0xfu) << 12) | (rm & 0xfu);
	}

	u32 EncodeOrrRegShiftImm(unsigned rd, unsigned rn, unsigned rm, ShiftType shift, u8 amount,
		bool set_flags)
	{
		return EncodeDataProcessingRegShiftImm(OPCODE_ORR, rd, rn, rm, shift, amount, set_flags);
	}

	u32 EncodeOrrRegShiftReg(unsigned rd, unsigned rn, unsigned rm, ShiftType shift, unsigned rs,
		bool set_flags)
	{
		return EncodeDataProcessingRegShiftReg(OPCODE_ORR, rd, rn, rm, shift, rs, set_flags);
	}

	u32 EncodeMvnReg(unsigned rd, unsigned rm, bool set_flags)
	{
		pxAssert(IsRegister(rd));
		pxAssert(IsRegister(rm));
		return CondBits(Condition::AL) | OPCODE_MVN | (set_flags ? SET_FLAGS : 0) |
			   ((rd & 0xfu) << 12) | (rm & 0xfu);
	}

	u32 EncodeClz(unsigned rd, unsigned rm)
	{
		pxAssert(IsLowRegister(rd));
		pxAssert(IsLowRegister(rm));
		return CondBits(Condition::AL) | CLZ | ((rd & 0xfu) << 12) | (rm & 0xfu);
	}

	u32 EncodeSsat(unsigned rd, u8 bits, unsigned rm)
	{
		pxAssert(IsLowRegister(rd));
		pxAssert(IsLowRegister(rm));
		pxAssert(bits > 0 && bits <= 32);
		return CondBits(Condition::AL) | SSAT | ((static_cast<u32>(bits - 1) & 0x1fu) << 16) |
			   ((rd & 0xfu) << 12) | (rm & 0xfu);
	}

	u32 EncodeUbfx(unsigned rd, unsigned rn, u8 lsb, u8 width)
	{
		pxAssert(IsLowRegister(rd));
		pxAssert(IsLowRegister(rn));
		pxAssert(lsb < 32);
		pxAssert(width > 0 && width <= 32 - lsb);
		return CondBits(Condition::AL) | UBFX | ((static_cast<u32>(width - 1) & 0x1fu) << 16) |
			   ((rd & 0xfu) << 12) | ((static_cast<u32>(lsb) & 0x1fu) << 7) | (rn & 0xfu);
	}

	u32 EncodeSxtb(unsigned rd, unsigned rm)
	{
		pxAssert(IsLowRegister(rd));
		pxAssert(IsLowRegister(rm));
		return CondBits(Condition::AL) | SXTB | ((rd & 0xfu) << 12) | (rm & 0xfu);
	}

	u32 EncodeSxth(unsigned rd, unsigned rm)
	{
		pxAssert(IsLowRegister(rd));
		pxAssert(IsLowRegister(rm));
		return CondBits(Condition::AL) | SXTH | ((rd & 0xfu) << 12) | (rm & 0xfu);
	}

	u32 EncodeUxth(unsigned rd, unsigned rm)
	{
		pxAssert(IsLowRegister(rd));
		pxAssert(IsLowRegister(rm));
		return CondBits(Condition::AL) | UXTH | ((rd & 0xfu) << 12) | (rm & 0xfu);
	}

	u32 EncodeSubReg(unsigned rd, unsigned rn, unsigned rm, bool set_flags)
	{
		pxAssert(IsRegister(rd));
		pxAssert(IsRegister(rn));
		pxAssert(IsRegister(rm));
		return CondBits(Condition::AL) | OPCODE_SUB | (set_flags ? SET_FLAGS : 0) |
			   ((rn & 0xfu) << 16) | ((rd & 0xfu) << 12) | (rm & 0xfu);
	}

	u32 EncodeSubRegShiftImm(unsigned rd, unsigned rn, unsigned rm, ShiftType shift, u8 amount,
		bool set_flags)
	{
		return EncodeDataProcessingRegShiftImm(OPCODE_SUB, rd, rn, rm, shift, amount, set_flags);
	}

	u32 EncodeSubRegShiftReg(unsigned rd, unsigned rn, unsigned rm, ShiftType shift, unsigned rs,
		bool set_flags)
	{
		return EncodeDataProcessingRegShiftReg(OPCODE_SUB, rd, rn, rm, shift, rs, set_flags);
	}

	u32 EncodeSbcReg(unsigned rd, unsigned rn, unsigned rm, bool set_flags)
	{
		pxAssert(IsRegister(rd));
		pxAssert(IsRegister(rn));
		pxAssert(IsRegister(rm));
		return CondBits(Condition::AL) | OPCODE_SBC | (set_flags ? SET_FLAGS : 0) |
			   ((rn & 0xfu) << 16) | ((rd & 0xfu) << 12) | (rm & 0xfu);
	}

	u32 EncodePkhbt(unsigned rd, unsigned rn, unsigned rm, u8 lsl_amount)
	{
		pxAssert(IsLowRegister(rd));
		pxAssert(IsLowRegister(rn));
		pxAssert(IsLowRegister(rm));
		pxAssert(lsl_amount <= 31);
		return CondBits(Condition::AL) | PKHBT | ((rn & 0xfu) << 16) | ((rd & 0xfu) << 12) |
			   ((static_cast<u32>(lsl_amount) & 0x1fu) << 7) | (rm & 0xfu);
	}

	u32 EncodeUmull(unsigned rdlo, unsigned rdhi, unsigned rn, unsigned rm, bool set_flags)
	{
		pxAssert(IsLowRegister(rdlo));
		pxAssert(IsLowRegister(rdhi));
		pxAssert(IsLowRegister(rn));
		pxAssert(IsLowRegister(rm));
		pxAssert(rdlo != rdhi);
		return CondBits(Condition::AL) | UMULL | (set_flags ? SET_FLAGS : 0) |
			   ((rdhi & 0xfu) << 16) | ((rdlo & 0xfu) << 12) | ((rm & 0xfu) << 8) | (rn & 0xfu);
	}

	u32 EncodeSmull(unsigned rdlo, unsigned rdhi, unsigned rn, unsigned rm, bool set_flags)
	{
		pxAssert(IsLowRegister(rdlo));
		pxAssert(IsLowRegister(rdhi));
		pxAssert(IsLowRegister(rn));
		pxAssert(IsLowRegister(rm));
		pxAssert(rdlo != rdhi);
		return CondBits(Condition::AL) | SMULL | (set_flags ? SET_FLAGS : 0) |
			   ((rdhi & 0xfu) << 16) | ((rdlo & 0xfu) << 12) | ((rm & 0xfu) << 8) | (rn & 0xfu);
	}

	u32 EncodeCmpReg(unsigned rn, unsigned rm, Condition condition)
	{
		pxAssert(IsRegister(rn));
		pxAssert(IsRegister(rm));
		return CondBits(condition) | OPCODE_CMP | SET_FLAGS | ((rn & 0xfu) << 16) | (rm & 0xfu);
	}

	u32 EncodeLdrImm12(unsigned rd, unsigned rn, u16 offset, Condition condition)
	{
		pxAssert(IsLowRegister(rd));
		pxAssert(IsLowRegister(rn));
		pxAssert(offset <= 0x0fff);
		return CondBits(condition) | LDR_IMM | ((rn & 0xfu) << 16) | ((rd & 0xfu) << 12) | offset;
	}

	u32 EncodeLdrImm12PostIndex(unsigned rd, unsigned rn, u16 offset)
	{
		pxAssert(IsLowRegister(rd));
		pxAssert(IsLowRegister(rn));
		pxAssert(rd != rn);
		pxAssert(offset <= 0x0fff);
		return CondBits(Condition::AL) | LDR_IMM_POST_INDEX |
			((rn & 0xfu) << 16) | ((rd & 0xfu) << 12) | offset;
	}

	u32 EncodeLdrRegShift(unsigned rd, unsigned rn, unsigned rm, ShiftType shift, u8 amount)
	{
		pxAssert(IsLowRegister(rd));
		pxAssert(IsLowRegister(rn));
		pxAssert(IsLowRegister(rm));
		pxAssert(amount <= 31);
		return CondBits(Condition::AL) | LDR_REG | ((rn & 0xfu) << 16) | ((rd & 0xfu) << 12) |
			   ((static_cast<u32>(amount) & 0x1fu) << 7) |
			   ((static_cast<u32>(shift) & 0x3u) << 5) | (rm & 0xfu);
	}

	u32 EncodeStrImm12(unsigned rd, unsigned rn, u16 offset)
	{
		pxAssert(IsLowRegister(rd));
		pxAssert(IsLowRegister(rn));
		pxAssert(offset <= 0x0fff);
		return CondBits(Condition::AL) | STR_IMM | ((rn & 0xfu) << 16) | ((rd & 0xfu) << 12) | offset;
	}

	u32 EncodeStrRegShift(unsigned rd, unsigned rn, unsigned rm, ShiftType shift, u8 amount)
	{
		pxAssert(IsLowRegister(rd));
		pxAssert(IsLowRegister(rn));
		pxAssert(IsLowRegister(rm));
		pxAssert(amount <= 31);
		return CondBits(Condition::AL) | STR_REG | ((rn & 0xfu) << 16) | ((rd & 0xfu) << 12) |
			   ((static_cast<u32>(amount) & 0x1fu) << 7) |
			   ((static_cast<u32>(shift) & 0x3u) << 5) | (rm & 0xfu);
	}

	u32 EncodeLdrdImm8(unsigned rdlo, unsigned rdhi, unsigned rn, u8 offset, Condition condition)
	{
		pxAssert(IsLowEvenRegisterPair(rdlo, rdhi));
		pxAssert(IsLowRegister(rn));
		return CondBits(condition) | LDRD_IMM | ((rn & 0xfu) << 16) | ((rdlo & 0xfu) << 12) |
			   ((static_cast<u32>(offset) & 0xf0u) << 4) | (offset & 0x0fu);
	}

	u32 EncodeStrdImm8(unsigned rdlo, unsigned rdhi, unsigned rn, u8 offset, Condition condition)
	{
		pxAssert(IsLowEvenRegisterPair(rdlo, rdhi));
		pxAssert(IsLowRegister(rn));
		return CondBits(condition) | STRD_IMM | ((rn & 0xfu) << 16) | ((rdlo & 0xfu) << 12) |
			   ((static_cast<u32>(offset) & 0xf0u) << 4) | (offset & 0x0fu);
	}

	u32 EncodeVldrDImm(unsigned dd, unsigned rn, u16 offset)
	{
		pxAssert(IsDRegister(dd));
		pxAssert(IsLowRegister(rn));
		pxAssert(offset <= 0x3fc && (offset & 0x3u) == 0);
		return CondBits(Condition::AL) | VLDR_D_IMM | ((rn & 0xfu) << 16) | VfpDd(dd) |
			   ((offset >> 2) & 0xffu);
	}

	u32 EncodeVldrSImm(unsigned sd, unsigned rn, u16 offset)
	{
		pxAssert(IsSRegister(sd));
		pxAssert(IsLowRegister(rn));
		pxAssert(offset <= 0x3fc && (offset & 0x3u) == 0);
		return CondBits(Condition::AL) | VLDR_S_IMM | ((rn & 0xfu) << 16) | VfpSd(sd) |
			   ((offset >> 2) & 0xffu);
	}

	u32 EncodeVstrSImm(unsigned sd, unsigned rn, u16 offset)
	{
		pxAssert(IsSRegister(sd));
		pxAssert(IsLowRegister(rn));
		pxAssert(offset <= 0x3fc && (offset & 0x3u) == 0);
		return CondBits(Condition::AL) | VSTR_S_IMM | ((rn & 0xfu) << 16) | VfpSd(sd) |
			   ((offset >> 2) & 0xffu);
	}

	u32 EncodeVstrDImm(unsigned dd, unsigned rn, u16 offset, Condition condition)
	{
		pxAssert(IsDRegister(dd));
		pxAssert(IsLowRegister(rn));
		pxAssert(offset <= 0x3fc && (offset & 0x3u) == 0);
		return CondBits(condition) | VSTR_D_IMM | ((rn & 0xfu) << 16) | VfpDd(dd) |
			   ((offset >> 2) & 0xffu);
	}

	u32 EncodeVdupI16D(unsigned dd, unsigned dm, u8 lane)
	{
		pxAssert(IsDRegister(dd));
		pxAssert(IsDRegister(dm));
		pxAssert(lane < 4);
		return VDUP_I16_D | NeonDd(dd) | NeonDm(dm) | ((static_cast<u32>(lane) & 0x3u) << 18);
	}

	u32 EncodeVdupI32QFromCore(unsigned qd, unsigned rt)
	{
		pxAssert(IsQRegister(qd));
		pxAssert(IsGeneralRegister(rt));
		return VDUP_I32_Q_CORE | NeonQn(qd) | ((rt & 0xfu) << 12);
	}

	u32 EncodeLdrbImm12(unsigned rd, unsigned rn, u16 offset)
	{
		pxAssert(IsLowRegister(rd));
		pxAssert(IsLowRegister(rn));
		pxAssert(offset <= 0x0fff);
		return CondBits(Condition::AL) | LDRB_IMM | ((rn & 0xfu) << 16) | ((rd & 0xfu) << 12) | offset;
	}

	u32 EncodeLdrbImm12PostIndex(unsigned rd, unsigned rn, u16 offset)
	{
		pxAssert(IsLowRegister(rd));
		pxAssert(IsLowRegister(rn));
		pxAssert(rd != rn);
		pxAssert(offset <= 0x0fff);
		return CondBits(Condition::AL) | LDRB_IMM_POST_INDEX |
			((rn & 0xfu) << 16) | ((rd & 0xfu) << 12) | offset;
	}

	u32 EncodeLdrbRegShift(unsigned rd, unsigned rn, unsigned rm, ShiftType shift, u8 amount)
	{
		pxAssert(IsLowRegister(rd));
		pxAssert(IsLowRegister(rn));
		pxAssert(IsLowRegister(rm));
		pxAssert(amount <= 31);
		return CondBits(Condition::AL) | LDRB_REG | ((rn & 0xfu) << 16) | ((rd & 0xfu) << 12) |
			   ((static_cast<u32>(amount) & 0x1fu) << 7) |
			   ((static_cast<u32>(shift) & 0x3u) << 5) | (rm & 0xfu);
	}

	u32 EncodeStrbImm12(unsigned rd, unsigned rn, u16 offset)
	{
		pxAssert(IsLowRegister(rd));
		pxAssert(IsLowRegister(rn));
		pxAssert(offset <= 0x0fff);
		return CondBits(Condition::AL) | STRB_IMM | ((rn & 0xfu) << 16) | ((rd & 0xfu) << 12) | offset;
	}

	u32 EncodeStrbRegShift(unsigned rd, unsigned rn, unsigned rm, ShiftType shift, u8 amount)
	{
		pxAssert(IsLowRegister(rd));
		pxAssert(IsLowRegister(rn));
		pxAssert(IsLowRegister(rm));
		pxAssert(amount <= 31);
		return CondBits(Condition::AL) | STRB_REG | ((rn & 0xfu) << 16) | ((rd & 0xfu) << 12) |
			   ((static_cast<u32>(amount) & 0x1fu) << 7) |
			   ((static_cast<u32>(shift) & 0x3u) << 5) | (rm & 0xfu);
	}

	u32 EncodeLdrhImm8(unsigned rd, unsigned rn, u8 offset)
	{
		pxAssert(IsLowRegister(rd));
		pxAssert(IsLowRegister(rn));
		return CondBits(Condition::AL) | LDRH_IMM | ((rn & 0xfu) << 16) | ((rd & 0xfu) << 12) |
			   ((static_cast<u32>(offset) & 0xf0u) << 4) | (offset & 0x0fu);
	}

	u32 EncodeLdrhReg(unsigned rd, unsigned rn, unsigned rm)
	{
		pxAssert(IsLowRegister(rd));
		pxAssert(IsLowRegister(rn));
		pxAssert(IsLowRegister(rm));
		return CondBits(Condition::AL) | LDRH_REG | ((rn & 0xfu) << 16) | ((rd & 0xfu) << 12) |
			   (rm & 0xfu);
	}

	u32 EncodeStrhImm8(unsigned rd, unsigned rn, u8 offset)
	{
		pxAssert(IsLowRegister(rd));
		pxAssert(IsLowRegister(rn));
		return CondBits(Condition::AL) | STRH_IMM | ((rn & 0xfu) << 16) | ((rd & 0xfu) << 12) |
			   ((static_cast<u32>(offset) & 0xf0u) << 4) | (offset & 0x0fu);
	}

	u32 EncodeStrhReg(unsigned rd, unsigned rn, unsigned rm)
	{
		pxAssert(IsLowRegister(rd));
		pxAssert(IsLowRegister(rn));
		pxAssert(IsLowRegister(rm));
		return CondBits(Condition::AL) | STRH_REG | ((rn & 0xfu) << 16) | ((rd & 0xfu) << 12) |
			   (rm & 0xfu);
	}

	u32 EncodeLdrsbImm8(unsigned rd, unsigned rn, u8 offset)
	{
		pxAssert(IsLowRegister(rd));
		pxAssert(IsLowRegister(rn));
		return CondBits(Condition::AL) | LDRSB_IMM | ((rn & 0xfu) << 16) | ((rd & 0xfu) << 12) |
			   ((static_cast<u32>(offset) & 0xf0u) << 4) | (offset & 0x0fu);
	}

	u32 EncodeLdrsbReg(unsigned rd, unsigned rn, unsigned rm)
	{
		pxAssert(IsLowRegister(rd));
		pxAssert(IsLowRegister(rn));
		pxAssert(IsLowRegister(rm));
		return CondBits(Condition::AL) | LDRSB_REG | ((rn & 0xfu) << 16) | ((rd & 0xfu) << 12) |
			   (rm & 0xfu);
	}

	u32 EncodeLdrshImm8(unsigned rd, unsigned rn, u8 offset)
	{
		pxAssert(IsLowRegister(rd));
		pxAssert(IsLowRegister(rn));
		return CondBits(Condition::AL) | LDRSH_IMM | ((rn & 0xfu) << 16) | ((rd & 0xfu) << 12) |
			   ((static_cast<u32>(offset) & 0xf0u) << 4) | (offset & 0x0fu);
	}

	u32 EncodeLdrshReg(unsigned rd, unsigned rn, unsigned rm)
	{
		pxAssert(IsLowRegister(rd));
		pxAssert(IsLowRegister(rn));
		pxAssert(IsLowRegister(rm));
		return CondBits(Condition::AL) | LDRSH_REG | ((rn & 0xfu) << 16) | ((rd & 0xfu) << 12) |
			   (rm & 0xfu);
	}

	u32 EncodeVld1Q32(unsigned qd, unsigned rn)
	{
		pxAssert(IsQRegister(qd));
		pxAssert(IsLowRegister(rn));
		return VLD1_32_Q | ((rn & 0xfu) << 16) | NeonQd(qd);
	}

	u32 EncodeVld1Q32Aligned(unsigned qd, unsigned rn)
	{
		pxAssert(IsQRegister(qd));
		pxAssert(IsLowRegister(rn));
		return VLD1_32_Q_ALIGNED | ((rn & 0xfu) << 16) | NeonQd(qd);
	}

	u32 EncodeVst1Q32(unsigned qd, unsigned rn)
	{
		pxAssert(IsQRegister(qd));
		pxAssert(IsLowRegister(rn));
		return VST1_32_Q | ((rn & 0xfu) << 16) | NeonQd(qd);
	}

	u32 EncodeVst1Q32Aligned(unsigned qd, unsigned rn)
	{
		pxAssert(IsQRegister(qd));
		pxAssert(IsLowRegister(rn));
		return VST1_32_Q_ALIGNED | ((rn & 0xfu) << 16) | NeonQd(qd);
	}

	u32 EncodeVst1Q32AlignedWriteback(unsigned qd, unsigned rn)
	{
		pxAssert(IsQRegister(qd));
		pxAssert(IsLowRegister(rn));
		return VST1_32_Q_ALIGNED_WRITEBACK | ((rn & 0xfu) << 16) | NeonQd(qd);
	}

	u32 EncodeVst1D32(unsigned dd, unsigned rn)
	{
		pxAssert(IsDRegister(dd));
		pxAssert(IsLowRegister(rn));
		return VST1_32_D | ((rn & 0xfu) << 16) | NeonDd(dd);
	}

	u32 EncodeVst1D32Lane(unsigned dd, u8 lane, unsigned rn)
	{
		pxAssert(IsDRegister(dd));
		pxAssert(lane < 2);
		pxAssert(IsLowRegister(rn));
		return VST1_32_D_LANE | ((rn & 0xfu) << 16) | NeonDd(dd) |
			((static_cast<u32>(lane) & 1u) << 7);
	}

	u32 EncodeVst1D8Lane0(unsigned dd, unsigned rn)
	{
		pxAssert(IsDRegister(dd));
		pxAssert(IsLowRegister(rn));
		return VST1_8_D_LANE0 | ((rn & 0xfu) << 16) | NeonDd(dd);
	}

	u32 EncodeVst1D16Lane0(unsigned dd, unsigned rn)
	{
		pxAssert(IsDRegister(dd));
		pxAssert(IsLowRegister(rn));
		return VST1_16_D_LANE0 | ((rn & 0xfu) << 16) | NeonDd(dd);
	}

	u32 EncodeVmovCoreToS(unsigned sd, unsigned rt)
	{
		pxAssert(IsSRegister(sd));
		pxAssert(IsRegister(rt));
		return VMOV_CORE_TO_S | ((rt & 0xfu) << 12) | VfpSn(sd);
	}

	u32 EncodeVmovSToCore(unsigned rt, unsigned sd, Condition condition)
	{
		pxAssert(IsRegister(rt));
		pxAssert(IsSRegister(sd));
		return (VMOV_S_TO_CORE & 0x0fffffffu) | CondBits(condition) |
			((rt & 0xfu) << 12) | VfpSn(sd);
	}

	u32 EncodeVmovCoreToD32Lane(unsigned dd, u8 lane, unsigned rt, Condition condition)
	{
		pxAssert(IsDRegister(dd));
		pxAssert(lane < 2);
		pxAssert(IsRegister(rt));
		return CondBits(condition) | VMOV_CORE_TO_D32_LANE |
			((static_cast<u32>(lane) & 1u) << 21) | ((rt & 0xfu) << 12) |
			((dd & 0xfu) << 16) | ((dd & 0x10u) << 3);
	}

	u32 EncodeVmovD32LaneToCore(unsigned rt, unsigned dd, u8 lane, Condition condition)
	{
		pxAssert(IsRegister(rt));
		pxAssert(IsDRegister(dd));
		pxAssert(lane < 2);
		return CondBits(condition) | VMOV_D32_LANE_TO_CORE |
			((static_cast<u32>(lane) & 1u) << 21) | ((rt & 0xfu) << 12) |
			((dd & 0xfu) << 16) | ((dd & 0x10u) << 3);
	}

	u32 EncodeVmovCorePairToD(unsigned dd, unsigned rt, unsigned rt2)
	{
		pxAssert(IsDRegister(dd));
		pxAssert(IsRegister(rt));
		pxAssert(IsRegister(rt2));
		return VMOV_CORE_PAIR_TO_D | ((rt2 & 0xfu) << 16) | ((rt & 0xfu) << 12) | VfpDm(dd);
	}

	u32 EncodeVmovS(unsigned sd, unsigned sm)
	{
		pxAssert(IsSRegister(sd));
		pxAssert(IsSRegister(sm));
		return VMOV_S | VfpSd(sd) | VfpSm(sm);
	}

	u32 EncodeVcvtF32S32(unsigned sd, unsigned sm)
	{
		pxAssert(IsSRegister(sd));
		pxAssert(IsSRegister(sm));
		return VCVT_F32_S32 | VfpSd(sd) | VfpSm(sm);
	}

	u32 EncodeVcvtF64F32(unsigned dd, unsigned sm)
	{
		pxAssert(IsDRegister(dd));
		pxAssert(IsSRegister(sm));
		return VCVT_F64_F32 | VfpDd(dd) | VfpSm(sm);
	}

	u32 EncodeVcvtF32F64(unsigned sd, unsigned dm)
	{
		pxAssert(IsSRegister(sd));
		pxAssert(IsDRegister(dm));
		return VCVT_F32_F64 | VfpSd(sd) | VfpDm(dm);
	}

	u32 EncodeVcvtF32S32Q(unsigned qd, unsigned qm)
	{
		pxAssert(IsQRegister(qd));
		pxAssert(IsQRegister(qm));
		return VCVT_F32_S32_Q | NeonQd(qd) | NeonQm(qm);
	}

	u32 EncodeVcvtS32F32Q(unsigned qd, unsigned qm)
	{
		pxAssert(IsQRegister(qd));
		pxAssert(IsQRegister(qm));
		return VCVT_S32_F32_Q | NeonQd(qd) | NeonQm(qm);
	}

	u32 EncodeVaddF32(unsigned sd, unsigned sn, unsigned sm)
	{
		pxAssert(IsSRegister(sd));
		pxAssert(IsSRegister(sn));
		pxAssert(IsSRegister(sm));
		return VADD_F32 | VfpSd(sd) | VfpSn(sn) | VfpSm(sm);
	}

	u32 EncodeVaddF64(unsigned dd, unsigned dn, unsigned dm)
	{
		pxAssert(IsDRegister(dd));
		pxAssert(IsDRegister(dn));
		pxAssert(IsDRegister(dm));
		return VADD_F64 | VfpDd(dd) | VfpDn(dn) | VfpDm(dm);
	}

	u32 EncodeVsubF32(unsigned sd, unsigned sn, unsigned sm)
	{
		pxAssert(IsSRegister(sd));
		pxAssert(IsSRegister(sn));
		pxAssert(IsSRegister(sm));
		return VSUB_F32 | VfpSd(sd) | VfpSn(sn) | VfpSm(sm);
	}

	u32 EncodeVmulF32(unsigned sd, unsigned sn, unsigned sm)
	{
		pxAssert(IsSRegister(sd));
		pxAssert(IsSRegister(sn));
		pxAssert(IsSRegister(sm));
		return VMUL_F32 | VfpSd(sd) | VfpSn(sn) | VfpSm(sm);
	}

	u32 EncodeVmulF64(unsigned dd, unsigned dn, unsigned dm)
	{
		pxAssert(IsDRegister(dd));
		pxAssert(IsDRegister(dn));
		pxAssert(IsDRegister(dm));
		return VMUL_F64 | VfpDd(dd) | VfpDn(dn) | VfpDm(dm);
	}

	u32 EncodeVaddF32Q(unsigned qd, unsigned qn, unsigned qm)
	{
		pxAssert(IsQRegister(qd));
		pxAssert(IsQRegister(qn));
		pxAssert(IsQRegister(qm));
		return VADD_F32_Q | NeonQd(qd) | NeonQn(qn) | NeonQm(qm);
	}

	u32 EncodeVsubF32Q(unsigned qd, unsigned qn, unsigned qm)
	{
		pxAssert(IsQRegister(qd));
		pxAssert(IsQRegister(qn));
		pxAssert(IsQRegister(qm));
		return VSUB_F32_Q | NeonQd(qd) | NeonQn(qn) | NeonQm(qm);
	}

	u32 EncodeVmulF32Q(unsigned qd, unsigned qn, unsigned qm)
	{
		pxAssert(IsQRegister(qd));
		pxAssert(IsQRegister(qn));
		pxAssert(IsQRegister(qm));
		return VMUL_F32_Q | NeonQd(qd) | NeonQn(qn) | NeonQm(qm);
	}

	u32 EncodeVdivF32(unsigned sd, unsigned sn, unsigned sm)
	{
		pxAssert(IsSRegister(sd));
		pxAssert(IsSRegister(sn));
		pxAssert(IsSRegister(sm));
		return VDIV_F32 | VfpSd(sd) | VfpSn(sn) | VfpSm(sm);
	}

	u32 EncodeVdivF64(unsigned dd, unsigned dn, unsigned dm)
	{
		pxAssert(IsDRegister(dd));
		pxAssert(IsDRegister(dn));
		pxAssert(IsDRegister(dm));
		return VDIV_F64 | VfpDd(dd) | VfpDn(dn) | VfpDm(dm);
	}

	u32 EncodeVsqrtF32(unsigned sd, unsigned sm)
	{
		pxAssert(IsSRegister(sd));
		pxAssert(IsSRegister(sm));
		return VSQRT_F32 | VfpSd(sd) | VfpSm(sm);
	}

	u32 EncodeVaddI8Q(unsigned qd, unsigned qn, unsigned qm)
	{
		pxAssert(IsQRegister(qd));
		pxAssert(IsQRegister(qn));
		pxAssert(IsQRegister(qm));
		return VADD_I8_Q | NeonQd(qd) | NeonQn(qn) | NeonQm(qm);
	}

	u32 EncodeVaddI16Q(unsigned qd, unsigned qn, unsigned qm)
	{
		pxAssert(IsQRegister(qd));
		pxAssert(IsQRegister(qn));
		pxAssert(IsQRegister(qm));
		return VADD_I16_Q | NeonQd(qd) | NeonQn(qn) | NeonQm(qm);
	}

	u32 EncodeVaddI32Q(unsigned qd, unsigned qn, unsigned qm)
	{
		pxAssert(IsQRegister(qd));
		pxAssert(IsQRegister(qn));
		pxAssert(IsQRegister(qm));
		return VADD_I32_Q | NeonQd(qd) | NeonQn(qn) | NeonQm(qm);
	}

	u32 EncodeVaddI64Q(unsigned qd, unsigned qn, unsigned qm)
	{
		pxAssert(IsQRegister(qd));
		pxAssert(IsQRegister(qn));
		pxAssert(IsQRegister(qm));
		return VADD_I64_Q | NeonQd(qd) | NeonQn(qn) | NeonQm(qm);
	}

	u32 EncodeVsubI8Q(unsigned qd, unsigned qn, unsigned qm)
	{
		pxAssert(IsQRegister(qd));
		pxAssert(IsQRegister(qn));
		pxAssert(IsQRegister(qm));
		return VSUB_I8_Q | NeonQd(qd) | NeonQn(qn) | NeonQm(qm);
	}

	u32 EncodeVsubI16Q(unsigned qd, unsigned qn, unsigned qm)
	{
		pxAssert(IsQRegister(qd));
		pxAssert(IsQRegister(qn));
		pxAssert(IsQRegister(qm));
		return VSUB_I16_Q | NeonQd(qd) | NeonQn(qn) | NeonQm(qm);
	}

	u32 EncodeVsubI32Q(unsigned qd, unsigned qn, unsigned qm)
	{
		pxAssert(IsQRegister(qd));
		pxAssert(IsQRegister(qn));
		pxAssert(IsQRegister(qm));
		return VSUB_I32_Q | NeonQd(qd) | NeonQn(qn) | NeonQm(qm);
	}

	u32 EncodeVshlI16Q(unsigned qd, unsigned qm, u8 amount)
	{
		pxAssert(IsQRegister(qd));
		pxAssert(IsQRegister(qm));
		pxAssert(amount > 0 && amount < 16);
		return VSHL_I16_Q | ((static_cast<u32>(amount) & 0x3fu) << 16) |
			   NeonQd(qd) | NeonQm(qm);
	}

	u32 EncodeVshlI32Q(unsigned qd, unsigned qm, u8 amount)
	{
		pxAssert(IsQRegister(qd));
		pxAssert(IsQRegister(qm));
		pxAssert(amount > 0 && amount < 32);
		return VSHL_I32_Q | ((static_cast<u32>(amount) & 0x3fu) << 16) |
			   NeonQd(qd) | NeonQm(qm);
	}

	u32 EncodeVshrU16Q(unsigned qd, unsigned qm, u8 amount)
	{
		pxAssert(IsQRegister(qd));
		pxAssert(IsQRegister(qm));
		pxAssert(amount > 0 && amount < 16);
		const u8 imm = static_cast<u8>(32 - amount);
		return VSHR_U_Q | ((static_cast<u32>(imm) & 0x3fu) << 16) |
			   NeonQd(qd) | NeonQm(qm);
	}

	u32 EncodeVshrU32Q(unsigned qd, unsigned qm, u8 amount)
	{
		pxAssert(IsQRegister(qd));
		pxAssert(IsQRegister(qm));
		pxAssert(amount > 0 && amount < 32);
		const u8 imm = static_cast<u8>(64 - amount);
		return VSHR_U_Q | ((static_cast<u32>(imm) & 0x3fu) << 16) |
			   NeonQd(qd) | NeonQm(qm);
	}

	u32 EncodeVshrS16Q(unsigned qd, unsigned qm, u8 amount)
	{
		pxAssert(IsQRegister(qd));
		pxAssert(IsQRegister(qm));
		pxAssert(amount > 0 && amount < 16);
		const u8 imm = static_cast<u8>(32 - amount);
		return VSHR_S_Q | ((static_cast<u32>(imm) & 0x3fu) << 16) |
			   NeonQd(qd) | NeonQm(qm);
	}

	u32 EncodeVshrS32Q(unsigned qd, unsigned qm, u8 amount)
	{
		pxAssert(IsQRegister(qd));
		pxAssert(IsQRegister(qm));
		pxAssert(amount > 0 && amount < 32);
		const u8 imm = static_cast<u8>(64 - amount);
		return VSHR_S_Q | ((static_cast<u32>(imm) & 0x3fu) << 16) |
			   NeonQd(qd) | NeonQm(qm);
	}

	u32 EncodeVshlU32Q(unsigned qd, unsigned qm, unsigned qn)
	{
		pxAssert(IsQRegister(qd));
		pxAssert(IsQRegister(qm));
		pxAssert(IsQRegister(qn));
		return VSHL_U32_REG_Q | NeonQd(qd) | NeonQm(qm) | NeonQn(qn);
	}

	u32 EncodeVshlS32Q(unsigned qd, unsigned qm, unsigned qn)
	{
		pxAssert(IsQRegister(qd));
		pxAssert(IsQRegister(qm));
		pxAssert(IsQRegister(qn));
		return VSHL_S32_REG_Q | NeonQd(qd) | NeonQm(qm) | NeonQn(qn);
	}

	u32 EncodeVmullS16Q(unsigned qd, unsigned dn, unsigned dm)
	{
		pxAssert(IsQRegister(qd));
		pxAssert(IsDRegister(dn));
		pxAssert(IsDRegister(dm));
		return VMULL_S16_Q | NeonQd(qd) | NeonDn(dn) | NeonDm(dm);
	}

	u32 EncodeVmullS32Q(unsigned qd, unsigned dn, unsigned dm)
	{
		pxAssert(IsQRegister(qd));
		pxAssert(IsDRegister(dn));
		pxAssert(IsDRegister(dm));
		return VMULL_S32_Q | NeonQd(qd) | NeonDn(dn) | NeonDm(dm);
	}

	u32 EncodeVmullU32Q(unsigned qd, unsigned dn, unsigned dm)
	{
		pxAssert(IsQRegister(qd));
		pxAssert(IsDRegister(dn));
		pxAssert(IsDRegister(dm));
		return VMULL_U32_Q | NeonQd(qd) | NeonDn(dn) | NeonDm(dm);
	}

	u32 EncodeVnegS32Q(unsigned qd, unsigned qm)
	{
		pxAssert(IsQRegister(qd));
		pxAssert(IsQRegister(qm));
		return VNEG_S32_Q | NeonQd(qd) | NeonQm(qm);
	}

	u32 EncodeVqmovnS32D(unsigned dd, unsigned qm)
	{
		pxAssert(IsDRegister(dd));
		pxAssert(IsQRegister(qm));
		return VQMOVN_S32_D | NeonDd(dd) | NeonQm(qm);
	}

	u32 EncodeVqmovnS64D(unsigned dd, unsigned qm)
	{
		pxAssert(IsDRegister(dd));
		pxAssert(IsQRegister(qm));
		return VQMOVN_S64_D | NeonDd(dd) | NeonQm(qm);
	}

	u32 EncodeVcgtS8Q(unsigned qd, unsigned qn, unsigned qm)
	{
		pxAssert(IsQRegister(qd));
		pxAssert(IsQRegister(qn));
		pxAssert(IsQRegister(qm));
		return VCGT_S8_Q | NeonQd(qd) | NeonQn(qn) | NeonQm(qm);
	}

	u32 EncodeVcgtS16Q(unsigned qd, unsigned qn, unsigned qm)
	{
		pxAssert(IsQRegister(qd));
		pxAssert(IsQRegister(qn));
		pxAssert(IsQRegister(qm));
		return VCGT_S16_Q | NeonQd(qd) | NeonQn(qn) | NeonQm(qm);
	}

	u32 EncodeVcgtS32Q(unsigned qd, unsigned qn, unsigned qm)
	{
		pxAssert(IsQRegister(qd));
		pxAssert(IsQRegister(qn));
		pxAssert(IsQRegister(qm));
		return VCGT_S32_Q | NeonQd(qd) | NeonQn(qn) | NeonQm(qm);
	}

	u32 EncodeVceqI8Q(unsigned qd, unsigned qn, unsigned qm)
	{
		pxAssert(IsQRegister(qd));
		pxAssert(IsQRegister(qn));
		pxAssert(IsQRegister(qm));
		return VCEQ_I8_Q | NeonQd(qd) | NeonQn(qn) | NeonQm(qm);
	}

	u32 EncodeVceqI16Q(unsigned qd, unsigned qn, unsigned qm)
	{
		pxAssert(IsQRegister(qd));
		pxAssert(IsQRegister(qn));
		pxAssert(IsQRegister(qm));
		return VCEQ_I16_Q | NeonQd(qd) | NeonQn(qn) | NeonQm(qm);
	}

	u32 EncodeVceqI32Q(unsigned qd, unsigned qn, unsigned qm)
	{
		pxAssert(IsQRegister(qd));
		pxAssert(IsQRegister(qn));
		pxAssert(IsQRegister(qm));
		return VCEQ_I32_Q | NeonQd(qd) | NeonQn(qn) | NeonQm(qm);
	}

	u32 EncodeVminS16Q(unsigned qd, unsigned qn, unsigned qm)
	{
		pxAssert(IsQRegister(qd));
		pxAssert(IsQRegister(qn));
		pxAssert(IsQRegister(qm));
		return VMIN_S16_Q | NeonQd(qd) | NeonQn(qn) | NeonQm(qm);
	}

	u32 EncodeVminS32Q(unsigned qd, unsigned qn, unsigned qm)
	{
		pxAssert(IsQRegister(qd));
		pxAssert(IsQRegister(qn));
		pxAssert(IsQRegister(qm));
		return VMIN_S32_Q | NeonQd(qd) | NeonQn(qn) | NeonQm(qm);
	}

	u32 EncodeVmaxS16Q(unsigned qd, unsigned qn, unsigned qm)
	{
		pxAssert(IsQRegister(qd));
		pxAssert(IsQRegister(qn));
		pxAssert(IsQRegister(qm));
		return VMAX_S16_Q | NeonQd(qd) | NeonQn(qn) | NeonQm(qm);
	}

	u32 EncodeVmaxS32Q(unsigned qd, unsigned qn, unsigned qm)
	{
		pxAssert(IsQRegister(qd));
		pxAssert(IsQRegister(qn));
		pxAssert(IsQRegister(qm));
		return VMAX_S32_Q | NeonQd(qd) | NeonQn(qn) | NeonQm(qm);
	}

	u32 EncodeVqaddS8Q(unsigned qd, unsigned qn, unsigned qm)
	{
		pxAssert(IsQRegister(qd));
		pxAssert(IsQRegister(qn));
		pxAssert(IsQRegister(qm));
		return VQADD_S8_Q | NeonQd(qd) | NeonQn(qn) | NeonQm(qm);
	}

	u32 EncodeVqaddS16Q(unsigned qd, unsigned qn, unsigned qm)
	{
		pxAssert(IsQRegister(qd));
		pxAssert(IsQRegister(qn));
		pxAssert(IsQRegister(qm));
		return VQADD_S16_Q | NeonQd(qd) | NeonQn(qn) | NeonQm(qm);
	}

	u32 EncodeVqaddS32Q(unsigned qd, unsigned qn, unsigned qm)
	{
		pxAssert(IsQRegister(qd));
		pxAssert(IsQRegister(qn));
		pxAssert(IsQRegister(qm));
		return VQADD_S32_Q | NeonQd(qd) | NeonQn(qn) | NeonQm(qm);
	}

	u32 EncodeVqsubS8Q(unsigned qd, unsigned qn, unsigned qm)
	{
		pxAssert(IsQRegister(qd));
		pxAssert(IsQRegister(qn));
		pxAssert(IsQRegister(qm));
		return VQSUB_S8_Q | NeonQd(qd) | NeonQn(qn) | NeonQm(qm);
	}

	u32 EncodeVqsubS16Q(unsigned qd, unsigned qn, unsigned qm)
	{
		pxAssert(IsQRegister(qd));
		pxAssert(IsQRegister(qn));
		pxAssert(IsQRegister(qm));
		return VQSUB_S16_Q | NeonQd(qd) | NeonQn(qn) | NeonQm(qm);
	}

	u32 EncodeVqsubS32Q(unsigned qd, unsigned qn, unsigned qm)
	{
		pxAssert(IsQRegister(qd));
		pxAssert(IsQRegister(qn));
		pxAssert(IsQRegister(qm));
		return VQSUB_S32_Q | NeonQd(qd) | NeonQn(qn) | NeonQm(qm);
	}

	u32 EncodeVqaddU8Q(unsigned qd, unsigned qn, unsigned qm)
	{
		pxAssert(IsQRegister(qd));
		pxAssert(IsQRegister(qn));
		pxAssert(IsQRegister(qm));
		return VQADD_U8_Q | NeonQd(qd) | NeonQn(qn) | NeonQm(qm);
	}

	u32 EncodeVqaddU16Q(unsigned qd, unsigned qn, unsigned qm)
	{
		pxAssert(IsQRegister(qd));
		pxAssert(IsQRegister(qn));
		pxAssert(IsQRegister(qm));
		return VQADD_U16_Q | NeonQd(qd) | NeonQn(qn) | NeonQm(qm);
	}

	u32 EncodeVqaddU32Q(unsigned qd, unsigned qn, unsigned qm)
	{
		pxAssert(IsQRegister(qd));
		pxAssert(IsQRegister(qn));
		pxAssert(IsQRegister(qm));
		return VQADD_U32_Q | NeonQd(qd) | NeonQn(qn) | NeonQm(qm);
	}

	u32 EncodeVqsubU8Q(unsigned qd, unsigned qn, unsigned qm)
	{
		pxAssert(IsQRegister(qd));
		pxAssert(IsQRegister(qn));
		pxAssert(IsQRegister(qm));
		return VQSUB_U8_Q | NeonQd(qd) | NeonQn(qn) | NeonQm(qm);
	}

	u32 EncodeVqsubU16Q(unsigned qd, unsigned qn, unsigned qm)
	{
		pxAssert(IsQRegister(qd));
		pxAssert(IsQRegister(qn));
		pxAssert(IsQRegister(qm));
		return VQSUB_U16_Q | NeonQd(qd) | NeonQn(qn) | NeonQm(qm);
	}

	u32 EncodeVqsubU32Q(unsigned qd, unsigned qn, unsigned qm)
	{
		pxAssert(IsQRegister(qd));
		pxAssert(IsQRegister(qn));
		pxAssert(IsQRegister(qm));
		return VQSUB_U32_Q | NeonQd(qd) | NeonQn(qn) | NeonQm(qm);
	}

	u32 EncodeVqabsS16Q(unsigned qd, unsigned qm)
	{
		pxAssert(IsQRegister(qd));
		pxAssert(IsQRegister(qm));
		return VQABS_S16_Q | NeonQd(qd) | NeonQm(qm);
	}

	u32 EncodeVqabsS32Q(unsigned qd, unsigned qm)
	{
		pxAssert(IsQRegister(qd));
		pxAssert(IsQRegister(qm));
		return VQABS_S32_Q | NeonQd(qd) | NeonQm(qm);
	}

	u32 EncodeVclsS32Q(unsigned qd, unsigned qm)
	{
		pxAssert(IsQRegister(qd));
		pxAssert(IsQRegister(qm));
		return VCLS_S32_Q | NeonQd(qd) | NeonQm(qm);
	}

	u32 EncodeVrev64I32D(unsigned dd, unsigned dm)
	{
		pxAssert(IsDRegister(dd));
		pxAssert(IsDRegister(dm));
		return VREV64_I32_D | NeonDd(dd) | NeonDm(dm);
	}

	u32 EncodeVrev64I32Q(unsigned qd, unsigned qm)
	{
		pxAssert(IsQRegister(qd));
		pxAssert(IsQRegister(qm));
		return VREV64_I32_Q | NeonQd(qd) | NeonQm(qm);
	}

	u32 EncodeVrev64I16Q(unsigned qd, unsigned qm)
	{
		pxAssert(IsQRegister(qd));
		pxAssert(IsQRegister(qm));
		return VREV64_I16_Q | NeonQd(qd) | NeonQm(qm);
	}

	u32 EncodeVswpD(unsigned dd, unsigned dm)
	{
		pxAssert(IsDRegister(dd));
		pxAssert(IsDRegister(dm));
		pxAssert(dd != dm);
		return VSWP_D | NeonDd(dd) | NeonDm(dm);
	}

	u32 EncodeVtrnI16D(unsigned dd, unsigned dm)
	{
		pxAssert(IsDRegister(dd));
		pxAssert(IsDRegister(dm));
		return VTRN_I16_D | NeonDd(dd) | NeonDm(dm);
	}

	u32 EncodeVtrnI16Q(unsigned qd, unsigned qm)
	{
		pxAssert(IsQRegister(qd));
		pxAssert(IsQRegister(qm));
		return VTRN_I16_Q | NeonQd(qd) | NeonQm(qm);
	}

	u32 EncodeVtrnI32D(unsigned dd, unsigned dm)
	{
		pxAssert(IsDRegister(dd));
		pxAssert(IsDRegister(dm));
		return VTRN_I32_D | NeonDd(dd) | NeonDm(dm);
	}

	u32 EncodeVtrnI32Q(unsigned qd, unsigned qm)
	{
		pxAssert(IsQRegister(qd));
		pxAssert(IsQRegister(qm));
		return VTRN_I32_Q | NeonQd(qd) | NeonQm(qm);
	}

	u32 EncodeVzipI8Q(unsigned qd, unsigned qm)
	{
		pxAssert(IsQRegister(qd));
		pxAssert(IsQRegister(qm));
		return VZIP_I8_Q | NeonQd(qd) | NeonQm(qm);
	}

	u32 EncodeVzipI16Q(unsigned qd, unsigned qm)
	{
		pxAssert(IsQRegister(qd));
		pxAssert(IsQRegister(qm));
		return VZIP_I16_Q | NeonQd(qd) | NeonQm(qm);
	}

	u32 EncodeVzipI32Q(unsigned qd, unsigned qm)
	{
		pxAssert(IsQRegister(qd));
		pxAssert(IsQRegister(qm));
		return VZIP_I32_Q | NeonQd(qd) | NeonQm(qm);
	}

	u32 EncodeVuzpI8Q(unsigned qd, unsigned qm)
	{
		pxAssert(IsQRegister(qd));
		pxAssert(IsQRegister(qm));
		return VUZP_I8_Q | NeonQd(qd) | NeonQm(qm);
	}

	u32 EncodeVuzpI16Q(unsigned qd, unsigned qm)
	{
		pxAssert(IsQRegister(qd));
		pxAssert(IsQRegister(qm));
		return VUZP_I16_Q | NeonQd(qd) | NeonQm(qm);
	}

	u32 EncodeVuzpI32Q(unsigned qd, unsigned qm)
	{
		pxAssert(IsQRegister(qd));
		pxAssert(IsQRegister(qm));
		return VUZP_I32_Q | NeonQd(qd) | NeonQm(qm);
	}

	u32 EncodeVextI8Q(unsigned qd, unsigned qn, unsigned qm, u8 byte_offset)
	{
		pxAssert(IsQRegister(qd));
		pxAssert(IsQRegister(qn));
		pxAssert(IsQRegister(qm));
		pxAssert(byte_offset < 16);
		return VEXT_I8_Q | NeonQd(qd) | NeonQn(qn) | NeonQm(qm) |
			   ((static_cast<u32>(byte_offset) & 0x0fu) << 8);
	}

	u32 EncodeVandQ(unsigned qd, unsigned qn, unsigned qm)
	{
		pxAssert(IsQRegister(qd));
		pxAssert(IsQRegister(qn));
		pxAssert(IsQRegister(qm));
		return VAND_Q | NeonQd(qd) | NeonQn(qn) | NeonQm(qm);
	}

	u32 EncodeVeorQ(unsigned qd, unsigned qn, unsigned qm)
	{
		pxAssert(IsQRegister(qd));
		pxAssert(IsQRegister(qn));
		pxAssert(IsQRegister(qm));
		return VEOR_Q | NeonQd(qd) | NeonQn(qn) | NeonQm(qm);
	}

	u32 EncodeVeorD(unsigned dd, unsigned dn, unsigned dm)
	{
		pxAssert(IsDRegister(dd));
		pxAssert(IsDRegister(dn));
		pxAssert(IsDRegister(dm));
		return VEOR_D | NeonDd(dd) | NeonDn(dn) | NeonDm(dm);
	}

	u32 EncodeVorrQ(unsigned qd, unsigned qn, unsigned qm)
	{
		pxAssert(IsQRegister(qd));
		pxAssert(IsQRegister(qn));
		pxAssert(IsQRegister(qm));
		return VORR_Q | NeonQd(qd) | NeonQn(qn) | NeonQm(qm);
	}

	u32 EncodeVorrD(unsigned dd, unsigned dn, unsigned dm)
	{
		pxAssert(IsDRegister(dd));
		pxAssert(IsDRegister(dn));
		pxAssert(IsDRegister(dm));
		return VORR_D | NeonDd(dd) | NeonDn(dn) | NeonDm(dm);
	}

	u32 EncodeVmvnQ(unsigned qd, unsigned qm)
	{
		pxAssert(IsQRegister(qd));
		pxAssert(IsQRegister(qm));
		return VMVN_Q | NeonQd(qd) | NeonQm(qm);
	}

	u32 EncodePush(u16 register_list)
	{
		pxAssert(register_list != 0);
		return CondBits(Condition::AL) | PUSH | register_list;
	}

	u32 EncodeVpushDRange(unsigned first_d, unsigned d_count)
	{
		pxAssert(IsDRegister(first_d));
		pxAssert(d_count != 0 && d_count <= 16);
		pxAssert(first_d + d_count <= 32);
		return VPUSH_D | VfpDd(first_d) | (d_count * 2);
	}

	u32 EncodePop(u16 register_list)
	{
		pxAssert(register_list != 0);
		return CondBits(Condition::AL) | POP | register_list;
	}

	u32 EncodeVpopDRange(unsigned first_d, unsigned d_count)
	{
		pxAssert(IsDRegister(first_d));
		pxAssert(d_count != 0 && d_count <= 16);
		pxAssert(first_d + d_count <= 32);
		return VPOP_D | VfpDd(first_d) | (d_count * 2);
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
