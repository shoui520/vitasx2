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
		constexpr u32 LDRB_IMM = 0x05d00000u;
		constexpr u32 STRB_IMM = 0x05c00000u;
		constexpr u32 LDRH_IMM = 0x01d000b0u;
		constexpr u32 STRH_IMM = 0x01c000b0u;
		constexpr u32 PKHBT = 0x06800010u;
		constexpr u32 CLZ = 0x016f0f10u;
		constexpr u32 SSAT = 0x06a00010u;
		constexpr u32 UMULL = 0x00800090u;
		constexpr u32 SMULL = 0x00c00090u;
		constexpr u32 VLD1_32_Q = 0xf4200a8fu;
		constexpr u32 VST1_32_Q = 0xf4000a8fu;
		constexpr u32 VST1_32_D = 0xf400078fu;
		constexpr u32 VMOV_CORE_TO_S = 0xee000a10u;
		constexpr u32 VMOV_S_TO_CORE = 0xee100a10u;
		constexpr u32 VCVT_F32_S32 = 0xeeb80ac0u;
		constexpr u32 VADD_F32 = 0xee300a00u;
		constexpr u32 VSUB_F32 = 0xee300a40u;
		constexpr u32 VMUL_F32 = 0xee200a00u;
		constexpr u32 VDIV_F32 = 0xee800a00u;
		constexpr u32 VSQRT_F32 = 0xeeb10ac0u;
		constexpr u32 VADD_I8_Q = 0xf2000840u;
		constexpr u32 VADD_I16_Q = 0xf2100840u;
		constexpr u32 VADD_I32_Q = 0xf2200840u;
		constexpr u32 VSUB_I8_Q = 0xf3000840u;
		constexpr u32 VSUB_I16_Q = 0xf3100840u;
		constexpr u32 VSUB_I32_Q = 0xf3200840u;
		constexpr u32 VSHL_I16_Q = 0xf2900550u;
		constexpr u32 VSHL_I32_Q = 0xf2a00550u;
		constexpr u32 VSHR_U_Q = 0xf3800050u;
		constexpr u32 VSHR_S_Q = 0xf2800050u;
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
		constexpr u32 VZIP_I8_Q = 0xf3b201c0u;
		constexpr u32 VZIP_I16_Q = 0xf3b601c0u;
		constexpr u32 VZIP_I32_Q = 0xf3ba01c0u;
		constexpr u32 VUZP_I8_Q = 0xf3b20140u;
		constexpr u32 VUZP_I16_Q = 0xf3b60140u;
		constexpr u32 VUZP_I32_Q = 0xf3ba0140u;
		constexpr u32 VEXT_I8_Q = 0xf2b00040u;
		constexpr u32 VAND_Q = 0xf2000150u;
		constexpr u32 VEOR_Q = 0xf3000150u;
		constexpr u32 VORR_Q = 0xf2200150u;
		constexpr u32 VMVN_Q = 0xf3b005c0u;
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

		u32 CondBits(Condition condition)
		{
			return static_cast<u32>(condition) << 28;
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

		u32 NeonQn(unsigned qreg)
		{
			const unsigned dreg = qreg * 2;
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

	bool CodeBuffer::EmitMovRegShiftImm(unsigned rd, unsigned rm, ShiftType shift, u8 amount, bool set_flags,
		Condition condition)
	{
		if (!IsRegister(rd) || !IsRegister(rm) || amount > 31)
			return false;
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

	bool CodeBuffer::EmitLdrbImm12(unsigned rd, unsigned rn, u16 offset)
	{
		if (!IsLowRegister(rd) || !IsLowRegister(rn) || offset > 0x0fff)
			return false;
		return EmitU32(EncodeLdrbImm12(rd, rn, offset));
	}

	bool CodeBuffer::EmitStrbImm12(unsigned rd, unsigned rn, u16 offset)
	{
		if (!IsLowRegister(rd) || !IsLowRegister(rn) || offset > 0x0fff)
			return false;
		return EmitU32(EncodeStrbImm12(rd, rn, offset));
	}

	bool CodeBuffer::EmitLdrhImm8(unsigned rd, unsigned rn, u8 offset)
	{
		if (!IsLowRegister(rd) || !IsLowRegister(rn))
			return false;
		return EmitU32(EncodeLdrhImm8(rd, rn, offset));
	}

	bool CodeBuffer::EmitStrhImm8(unsigned rd, unsigned rn, u8 offset)
	{
		if (!IsLowRegister(rd) || !IsLowRegister(rn))
			return false;
		return EmitU32(EncodeStrhImm8(rd, rn, offset));
	}

	bool CodeBuffer::EmitVld1Q32(unsigned qd, unsigned rn)
	{
		if (!IsQRegister(qd) || !IsLowRegister(rn))
			return false;
		return EmitU32(EncodeVld1Q32(qd, rn));
	}

	bool CodeBuffer::EmitVst1Q32(unsigned qd, unsigned rn)
	{
		if (!IsQRegister(qd) || !IsLowRegister(rn))
			return false;
		return EmitU32(EncodeVst1Q32(qd, rn));
	}

	bool CodeBuffer::EmitVst1D32(unsigned dd, unsigned rn)
	{
		if (!IsDRegister(dd) || !IsLowRegister(rn))
			return false;
		return EmitU32(EncodeVst1D32(dd, rn));
	}

	bool CodeBuffer::EmitVmovCoreToS(unsigned sd, unsigned rt)
	{
		if (!IsSRegister(sd) || !IsRegister(rt))
			return false;
		return EmitU32(EncodeVmovCoreToS(sd, rt));
	}

	bool CodeBuffer::EmitVmovSToCore(unsigned rt, unsigned sd)
	{
		if (!IsRegister(rt) || !IsSRegister(sd))
			return false;
		return EmitU32(EncodeVmovSToCore(rt, sd));
	}

	bool CodeBuffer::EmitVcvtF32S32(unsigned sd, unsigned sm)
	{
		if (!IsSRegister(sd) || !IsSRegister(sm))
			return false;
		return EmitU32(EncodeVcvtF32S32(sd, sm));
	}

	bool CodeBuffer::EmitVaddF32(unsigned sd, unsigned sn, unsigned sm)
	{
		if (!IsSRegister(sd) || !IsSRegister(sn) || !IsSRegister(sm))
			return false;
		return EmitU32(EncodeVaddF32(sd, sn, sm));
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

	bool CodeBuffer::EmitVdivF32(unsigned sd, unsigned sn, unsigned sm)
	{
		if (!IsSRegister(sd) || !IsSRegister(sn) || !IsSRegister(sm))
			return false;
		return EmitU32(EncodeVdivF32(sd, sn, sm));
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
		return EmitU32(EncodeVaddI8Q(qd, qn, qm));
	}

	bool CodeBuffer::EmitVaddI16Q(unsigned qd, unsigned qn, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qn) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVaddI16Q(qd, qn, qm));
	}

	bool CodeBuffer::EmitVaddI32Q(unsigned qd, unsigned qn, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qn) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVaddI32Q(qd, qn, qm));
	}

	bool CodeBuffer::EmitVsubI8Q(unsigned qd, unsigned qn, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qn) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVsubI8Q(qd, qn, qm));
	}

	bool CodeBuffer::EmitVsubI16Q(unsigned qd, unsigned qn, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qn) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVsubI16Q(qd, qn, qm));
	}

	bool CodeBuffer::EmitVsubI32Q(unsigned qd, unsigned qn, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qn) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVsubI32Q(qd, qn, qm));
	}

	bool CodeBuffer::EmitVshlI16Q(unsigned qd, unsigned qm, u8 amount)
	{
		if (!IsQRegister(qd) || !IsQRegister(qm) || amount == 0 || amount >= 16)
			return false;
		return EmitU32(EncodeVshlI16Q(qd, qm, amount));
	}

	bool CodeBuffer::EmitVshlI32Q(unsigned qd, unsigned qm, u8 amount)
	{
		if (!IsQRegister(qd) || !IsQRegister(qm) || amount == 0 || amount >= 32)
			return false;
		return EmitU32(EncodeVshlI32Q(qd, qm, amount));
	}

	bool CodeBuffer::EmitVshrU16Q(unsigned qd, unsigned qm, u8 amount)
	{
		if (!IsQRegister(qd) || !IsQRegister(qm) || amount == 0 || amount >= 16)
			return false;
		return EmitU32(EncodeVshrU16Q(qd, qm, amount));
	}

	bool CodeBuffer::EmitVshrU32Q(unsigned qd, unsigned qm, u8 amount)
	{
		if (!IsQRegister(qd) || !IsQRegister(qm) || amount == 0 || amount >= 32)
			return false;
		return EmitU32(EncodeVshrU32Q(qd, qm, amount));
	}

	bool CodeBuffer::EmitVshrS16Q(unsigned qd, unsigned qm, u8 amount)
	{
		if (!IsQRegister(qd) || !IsQRegister(qm) || amount == 0 || amount >= 16)
			return false;
		return EmitU32(EncodeVshrS16Q(qd, qm, amount));
	}

	bool CodeBuffer::EmitVshrS32Q(unsigned qd, unsigned qm, u8 amount)
	{
		if (!IsQRegister(qd) || !IsQRegister(qm) || amount == 0 || amount >= 32)
			return false;
		return EmitU32(EncodeVshrS32Q(qd, qm, amount));
	}

	bool CodeBuffer::EmitVcgtS8Q(unsigned qd, unsigned qn, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qn) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVcgtS8Q(qd, qn, qm));
	}

	bool CodeBuffer::EmitVcgtS16Q(unsigned qd, unsigned qn, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qn) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVcgtS16Q(qd, qn, qm));
	}

	bool CodeBuffer::EmitVcgtS32Q(unsigned qd, unsigned qn, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qn) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVcgtS32Q(qd, qn, qm));
	}

	bool CodeBuffer::EmitVceqI8Q(unsigned qd, unsigned qn, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qn) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVceqI8Q(qd, qn, qm));
	}

	bool CodeBuffer::EmitVceqI16Q(unsigned qd, unsigned qn, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qn) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVceqI16Q(qd, qn, qm));
	}

	bool CodeBuffer::EmitVceqI32Q(unsigned qd, unsigned qn, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qn) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVceqI32Q(qd, qn, qm));
	}

	bool CodeBuffer::EmitVminS16Q(unsigned qd, unsigned qn, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qn) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVminS16Q(qd, qn, qm));
	}

	bool CodeBuffer::EmitVminS32Q(unsigned qd, unsigned qn, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qn) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVminS32Q(qd, qn, qm));
	}

	bool CodeBuffer::EmitVmaxS16Q(unsigned qd, unsigned qn, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qn) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVmaxS16Q(qd, qn, qm));
	}

	bool CodeBuffer::EmitVmaxS32Q(unsigned qd, unsigned qn, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qn) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVmaxS32Q(qd, qn, qm));
	}

	bool CodeBuffer::EmitVqaddS8Q(unsigned qd, unsigned qn, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qn) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVqaddS8Q(qd, qn, qm));
	}

	bool CodeBuffer::EmitVqaddS16Q(unsigned qd, unsigned qn, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qn) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVqaddS16Q(qd, qn, qm));
	}

	bool CodeBuffer::EmitVqaddS32Q(unsigned qd, unsigned qn, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qn) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVqaddS32Q(qd, qn, qm));
	}

	bool CodeBuffer::EmitVqsubS8Q(unsigned qd, unsigned qn, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qn) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVqsubS8Q(qd, qn, qm));
	}

	bool CodeBuffer::EmitVqsubS16Q(unsigned qd, unsigned qn, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qn) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVqsubS16Q(qd, qn, qm));
	}

	bool CodeBuffer::EmitVqsubS32Q(unsigned qd, unsigned qn, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qn) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVqsubS32Q(qd, qn, qm));
	}

	bool CodeBuffer::EmitVqaddU8Q(unsigned qd, unsigned qn, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qn) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVqaddU8Q(qd, qn, qm));
	}

	bool CodeBuffer::EmitVqaddU16Q(unsigned qd, unsigned qn, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qn) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVqaddU16Q(qd, qn, qm));
	}

	bool CodeBuffer::EmitVqaddU32Q(unsigned qd, unsigned qn, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qn) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVqaddU32Q(qd, qn, qm));
	}

	bool CodeBuffer::EmitVqsubU8Q(unsigned qd, unsigned qn, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qn) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVqsubU8Q(qd, qn, qm));
	}

	bool CodeBuffer::EmitVqsubU16Q(unsigned qd, unsigned qn, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qn) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVqsubU16Q(qd, qn, qm));
	}

	bool CodeBuffer::EmitVqsubU32Q(unsigned qd, unsigned qn, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qn) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVqsubU32Q(qd, qn, qm));
	}

	bool CodeBuffer::EmitVqabsS16Q(unsigned qd, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVqabsS16Q(qd, qm));
	}

	bool CodeBuffer::EmitVqabsS32Q(unsigned qd, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVqabsS32Q(qd, qm));
	}

	bool CodeBuffer::EmitVzipI8Q(unsigned qd, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVzipI8Q(qd, qm));
	}

	bool CodeBuffer::EmitVzipI16Q(unsigned qd, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVzipI16Q(qd, qm));
	}

	bool CodeBuffer::EmitVzipI32Q(unsigned qd, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVzipI32Q(qd, qm));
	}

	bool CodeBuffer::EmitVuzpI8Q(unsigned qd, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVuzpI8Q(qd, qm));
	}

	bool CodeBuffer::EmitVuzpI16Q(unsigned qd, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVuzpI16Q(qd, qm));
	}

	bool CodeBuffer::EmitVuzpI32Q(unsigned qd, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVuzpI32Q(qd, qm));
	}

	bool CodeBuffer::EmitVextI8Q(unsigned qd, unsigned qn, unsigned qm, u8 byte_offset)
	{
		if (!IsQRegister(qd) || !IsQRegister(qn) || !IsQRegister(qm) || byte_offset >= 16)
			return false;
		return EmitU32(EncodeVextI8Q(qd, qn, qm, byte_offset));
	}

	bool CodeBuffer::EmitVandQ(unsigned qd, unsigned qn, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qn) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVandQ(qd, qn, qm));
	}

	bool CodeBuffer::EmitVeorQ(unsigned qd, unsigned qn, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qn) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVeorQ(qd, qn, qm));
	}

	bool CodeBuffer::EmitVorrQ(unsigned qd, unsigned qn, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qn) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVorrQ(qd, qn, qm));
	}

	bool CodeBuffer::EmitVmvnQ(unsigned qd, unsigned qm)
	{
		if (!IsQRegister(qd) || !IsQRegister(qm))
			return false;
		return EmitU32(EncodeVmvnQ(qd, qm));
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

	u32 EncodeLdrbImm12(unsigned rd, unsigned rn, u16 offset)
	{
		pxAssert(IsLowRegister(rd));
		pxAssert(IsLowRegister(rn));
		pxAssert(offset <= 0x0fff);
		return CondBits(Condition::AL) | LDRB_IMM | ((rn & 0xfu) << 16) | ((rd & 0xfu) << 12) | offset;
	}

	u32 EncodeStrbImm12(unsigned rd, unsigned rn, u16 offset)
	{
		pxAssert(IsLowRegister(rd));
		pxAssert(IsLowRegister(rn));
		pxAssert(offset <= 0x0fff);
		return CondBits(Condition::AL) | STRB_IMM | ((rn & 0xfu) << 16) | ((rd & 0xfu) << 12) | offset;
	}

	u32 EncodeLdrhImm8(unsigned rd, unsigned rn, u8 offset)
	{
		pxAssert(IsLowRegister(rd));
		pxAssert(IsLowRegister(rn));
		return CondBits(Condition::AL) | LDRH_IMM | ((rn & 0xfu) << 16) | ((rd & 0xfu) << 12) |
			   ((static_cast<u32>(offset) & 0xf0u) << 4) | (offset & 0x0fu);
	}

	u32 EncodeStrhImm8(unsigned rd, unsigned rn, u8 offset)
	{
		pxAssert(IsLowRegister(rd));
		pxAssert(IsLowRegister(rn));
		return CondBits(Condition::AL) | STRH_IMM | ((rn & 0xfu) << 16) | ((rd & 0xfu) << 12) |
			   ((static_cast<u32>(offset) & 0xf0u) << 4) | (offset & 0x0fu);
	}

	u32 EncodeVld1Q32(unsigned qd, unsigned rn)
	{
		pxAssert(IsQRegister(qd));
		pxAssert(IsLowRegister(rn));
		return VLD1_32_Q | ((rn & 0xfu) << 16) | NeonQd(qd);
	}

	u32 EncodeVst1Q32(unsigned qd, unsigned rn)
	{
		pxAssert(IsQRegister(qd));
		pxAssert(IsLowRegister(rn));
		return VST1_32_Q | ((rn & 0xfu) << 16) | NeonQd(qd);
	}

	u32 EncodeVst1D32(unsigned dd, unsigned rn)
	{
		pxAssert(IsDRegister(dd));
		pxAssert(IsLowRegister(rn));
		return VST1_32_D | ((rn & 0xfu) << 16) | NeonDd(dd);
	}

	u32 EncodeVmovCoreToS(unsigned sd, unsigned rt)
	{
		pxAssert(IsSRegister(sd));
		pxAssert(IsRegister(rt));
		return VMOV_CORE_TO_S | ((rt & 0xfu) << 12) | VfpSn(sd);
	}

	u32 EncodeVmovSToCore(unsigned rt, unsigned sd)
	{
		pxAssert(IsRegister(rt));
		pxAssert(IsSRegister(sd));
		return VMOV_S_TO_CORE | ((rt & 0xfu) << 12) | VfpSn(sd);
	}

	u32 EncodeVcvtF32S32(unsigned sd, unsigned sm)
	{
		pxAssert(IsSRegister(sd));
		pxAssert(IsSRegister(sm));
		return VCVT_F32_S32 | VfpSd(sd) | VfpSm(sm);
	}

	u32 EncodeVaddF32(unsigned sd, unsigned sn, unsigned sm)
	{
		pxAssert(IsSRegister(sd));
		pxAssert(IsSRegister(sn));
		pxAssert(IsSRegister(sm));
		return VADD_F32 | VfpSd(sd) | VfpSn(sn) | VfpSm(sm);
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

	u32 EncodeVdivF32(unsigned sd, unsigned sn, unsigned sm)
	{
		pxAssert(IsSRegister(sd));
		pxAssert(IsSRegister(sn));
		pxAssert(IsSRegister(sm));
		return VDIV_F32 | VfpSd(sd) | VfpSn(sn) | VfpSm(sm);
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

	u32 EncodeVorrQ(unsigned qd, unsigned qn, unsigned qm)
	{
		pxAssert(IsQRegister(qd));
		pxAssert(IsQRegister(qn));
		pxAssert(IsQRegister(qm));
		return VORR_Q | NeonQd(qd) | NeonQn(qn) | NeonQm(qm);
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
