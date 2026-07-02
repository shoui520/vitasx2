// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

namespace VitaA32
{
	class CodeBuffer;
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
		bool EmitADDU(u32 op);
		bool EmitSUBU(u32 op);
		bool EmitDADDU(u32 op);
		bool EmitDSUBU(u32 op);
		bool EmitAND(u32 op);
		bool EmitOR(u32 op);
		bool EmitXOR(u32 op);
		bool EmitSLT(u32 op);
		bool EmitSLTU(u32 op);
		bool EmitSetLessThan64(unsigned guest_reg, bool signed_compare);
		bool EmitLoadGprLow(unsigned guest_reg, unsigned host_reg);
		bool EmitLoadGpr64(unsigned guest_reg, unsigned host_low, unsigned host_high);
		bool EmitStorePc(u32 pc);
		bool EmitStoreGpr64(unsigned guest_reg, unsigned host_low, unsigned host_high);

		VitaA32::CodeBuffer& m_code;
	};
} // namespace VitaEE
