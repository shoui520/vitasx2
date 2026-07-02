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
	class BlockCompiler
	{
	public:
		explicit BlockCompiler(VitaA32::CodeBuffer& code);

		static bool CanCompileOpcode(u32 op);

		bool BeginBlock();
		bool CompileStraightLineBlock(u32 start_pc, u32 instruction_count, const void* direct_exit, const void* event_exit,
			u32* scaled_cycles = nullptr);
		bool EmitOpcode(u32 op);
		bool EndBlockReturn(u8 value);
		bool EndBlockWithCycleTest(u32 block_cycles, const void* direct_exit, const void* event_exit);

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
		bool EmitSetLessThan64(unsigned guest_reg, bool signed_compare);
		bool EmitLoadGprLow(unsigned guest_reg, unsigned host_reg);
		bool EmitLoadGpr64(unsigned guest_reg, unsigned host_low, unsigned host_high);
		bool EmitStorePc(u32 pc);
		bool EmitStoreGpr64(unsigned guest_reg, unsigned host_low, unsigned host_high);

		VitaA32::CodeBuffer& m_code;
	};
} // namespace VitaEE
