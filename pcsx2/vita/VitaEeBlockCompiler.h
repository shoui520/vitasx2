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

		bool BeginBlock();
		bool EmitOpcode(u32 op);
		bool EndBlockReturn(u8 value);

	private:
		bool EmitADDIU(u32 op);
		bool EmitORI(u32 op);
		bool EmitLoadGprLow(unsigned guest_reg, unsigned host_reg);
		bool EmitLoadGpr64(unsigned guest_reg, unsigned host_low, unsigned host_high);
		bool EmitStoreGpr64(unsigned guest_reg, unsigned host_low, unsigned host_high);

		VitaA32::CodeBuffer& m_code;
	};
} // namespace VitaEE
