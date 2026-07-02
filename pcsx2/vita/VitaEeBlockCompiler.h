// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

namespace VitaA32
{
	class CodeBuffer;
	enum class ShiftType : u8;
}

namespace VitaEE
{
	struct DirectLinkSlot
	{
		u32 target_pc = 0;
		size_t target_offset = 0;
		bool valid = false;
	};

	struct DirectLinkSlots
	{
		DirectLinkSlot slots[2]{};
	};

	enum class SignedBranchCondition : u8
	{
		LessThanZero,
		GreaterEqualZero,
		LessEqualZero,
		GreaterThanZero,
	};

	class BlockCompiler
	{
	public:
		explicit BlockCompiler(VitaA32::CodeBuffer& code);

		static bool CanCompileOpcode(u32 op);
		static bool IsSupportedBranchOpcode(u32 op);
		static bool CanCompileDelaySlotOpcode(u32 op);

		bool BeginBlock();
		bool CompileStraightLineBlock(u32 start_pc, u32 instruction_count, const void* direct_exit, const void* event_exit,
			u32* scaled_cycles = nullptr, DirectLinkSlots* direct_links = nullptr);
		bool EmitOpcode(u32 op, u32 pc = 0, u32 raw_cycles_through_instruction = 0, const void* event_exit = nullptr);
		bool EndBlockReturn(u8 value);
		bool EndBlockWithCycleTest(u32 block_cycles, const void* direct_exit, const void* event_exit,
			size_t* direct_link_target_offset = nullptr, size_t* taken_link_target_offset = nullptr);
		bool EndBlockWithLikelyCycleTest(u32 taken_cycles, u32 not_taken_cycles, const void* direct_exit,
			const void* event_exit, size_t* not_taken_link_target_offset = nullptr,
			size_t* taken_link_target_offset = nullptr);
		static bool RequiresBlockEndAfterOpcode(u32 op);

	private:
		bool EmitSPECIAL(u32 op);
		bool EmitADDIU(u32 op);
		bool EmitDADDIU(u32 op);
		bool EmitSLTI(u32 op);
		bool EmitSLTIU(u32 op);
		bool EmitANDI(u32 op);
		bool EmitORI(u32 op);
		bool EmitXORI(u32 op);
		bool EmitLUI(u32 op);
		bool EmitSLL(u32 op);
		bool EmitSRL(u32 op);
		bool EmitSRA(u32 op);
		bool EmitSLLV(u32 op);
		bool EmitSRLV(u32 op);
		bool EmitSRAV(u32 op);
		bool EmitMOVZ(u32 op);
		bool EmitMOVN(u32 op);
		bool EmitREGIMM(u32 op, u32 pc);
		bool EmitJ(u32 op, u32 pc);
		bool EmitJAL(u32 op, u32 pc);
		bool EmitJR(u32 op, u32 pc);
		bool EmitJALR(u32 op, u32 pc);
		bool EmitBEQ(u32 op);
		bool EmitBNE(u32 op);
		bool EmitBLEZ(u32 op);
		bool EmitBGTZ(u32 op);
		bool EmitBEQL(u32 op);
		bool EmitBNEL(u32 op);
		bool EmitBLEZL(u32 op);
		bool EmitBGTZL(u32 op);
		bool EmitLB(u32 op, u32 pc, u32 raw_cycles_through_instruction, const void* event_exit);
		bool EmitLH(u32 op, u32 pc, u32 raw_cycles_through_instruction, const void* event_exit);
		bool EmitLW(u32 op, u32 pc, u32 raw_cycles_through_instruction, const void* event_exit);
		bool EmitLBU(u32 op, u32 pc, u32 raw_cycles_through_instruction, const void* event_exit);
		bool EmitLHU(u32 op, u32 pc, u32 raw_cycles_through_instruction, const void* event_exit);
		bool EmitLWU(u32 op);
		bool EmitSB(u32 op);
		bool EmitSH(u32 op);
		bool EmitSW(u32 op);
		bool EmitDSLLV(u32 op);
		bool EmitDSRLV(u32 op);
		bool EmitDSRAV(u32 op);
		bool EmitDSLL(u32 op);
		bool EmitDSRL(u32 op);
		bool EmitDSRA(u32 op);
		bool EmitDSLL32(u32 op);
		bool EmitDSRL32(u32 op);
		bool EmitDSRA32(u32 op);
		bool EmitADDU(u32 op);
		bool EmitSUBU(u32 op);
		bool EmitDADDU(u32 op);
		bool EmitDSUBU(u32 op);
		bool EmitAND(u32 op);
		bool EmitOR(u32 op);
		bool EmitXOR(u32 op);
		bool EmitNOR(u32 op);
		bool EmitSLT(u32 op);
		bool EmitSLTU(u32 op);
		bool EmitShift32Immediate(u32 op, VitaA32::ShiftType shift);
		bool EmitShift32Variable(u32 op, VitaA32::ShiftType shift);
		bool EmitShift64LeftImmediate(u32 op, unsigned amount);
		bool EmitShift64RightImmediate(u32 op, unsigned amount, bool arithmetic);
		bool EmitShift64LeftVariable(u32 op);
		bool EmitShift64RightVariable(u32 op, bool arithmetic);
		bool EmitConditionalMove(u32 op, bool move_on_zero);
		bool EmitGoemonBlockStartHook(u32 start_pc);
		bool EmitLink(unsigned guest_reg, u32 pc);
		bool EmitJump(u32 pc, bool link);
		bool EmitRegisterJump(u32 op, u32 pc, bool link);
		bool EmitGoemonTranslateHostReg(unsigned host_reg);
		bool EmitBranchEqual(u32 op, bool branch_on_equal);
		bool EmitBranchSigned(u32 op, SignedBranchCondition condition);
		bool EmitSetLessThan64(unsigned guest_reg, bool signed_compare);
		bool EmitLoadWithCounterReadEvent(u32 op, u32 pc, u32 raw_cycles_through_instruction,
			const void* event_exit, const void* read_helper, bool sign_extend, unsigned sign_shift);
		bool EmitCounterReadFlagFromAddress(unsigned host_reg);
		bool EmitCounterReadEventExit(u32 next_pc, u32 raw_cycles_through_instruction, const void* event_exit);
		bool EmitAddScaledCyclesToCpu(u32 cycles);
		bool EmitEffectiveAddress(u32 op, unsigned host_reg);
		bool EmitLoadGprLow(unsigned guest_reg, unsigned host_reg);
		bool EmitLoadGprHigh(unsigned guest_reg, unsigned host_reg);
		bool EmitLoadGpr64(unsigned guest_reg, unsigned host_low, unsigned host_high);
		bool EmitStorePcFromHostReg(unsigned host_reg);
		bool EmitStoreBranchPc(u32 target_pc, u32 fallthrough_pc);
		bool EmitStorePc(u32 pc);
		bool EmitStoreGpr64(unsigned guest_reg, unsigned host_low, unsigned host_high);

		VitaA32::CodeBuffer& m_code;
	};
} // namespace VitaEE
