// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "common/Assertions.h"
#include "common/Console.h"

#include "Vif_Dynarec.h"
#include "Vif_Dma.h"
#include "Vif_Unpack.h"

#include <cstring>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define VITASX2_VIF_HAS_ARM_NEON 1
#else
#define VITASX2_VIF_HAS_ARM_NEON 0
#endif

#if defined(VITASX2_QEMU_VALIDATION)
u32 g_qemuVifFastV4_32Vectors = 0;
#endif

namespace
{
	template <int idx>
	u8* VitaVifVuMemPtr(int offset)
	{
		return vuRegs[idx].Mem + (offset & (idx ? 0x3ff0 : 0xff0));
	}

	void VitaVifCopyQword(u8* dest, const u8* src)
	{
#if VITASX2_VIF_HAS_ARM_NEON
		const uint32x4_t value = vld1q_u32(reinterpret_cast<const u32*>(src));
		vst1q_u32(reinterpret_cast<u32*>(dest), value);
#else
		std::memcpy(dest, src, 16);
#endif
	}

	template <int idx>
	bool VitaVifTryFastV4_32(const u8* data, bool isFill)
	{
		// PCSX2 owner: Vif_Unpack.cpp::UNPACK_V4<u32> with mode 0 and no mask.
		vifStruct& vif = GetVifX;
		VIFregisters& regs = vifXRegs;
		const u32 upk_num = static_cast<u32>(vif.cmd & 0x1f);
		if ((upk_num & 0x0f) != 0x0c || (upk_num & 0x10) != 0 || regs.mode != 0)
			return false;

		constexpr int vsize = 16;
		const int skip_size = (regs.cycle.cl - regs.cycle.wl) * 16;
		do
		{
			VitaVifCopyQword(VitaVifVuMemPtr<idx>(vif.tag.addr), data);
#if defined(VITASX2_QEMU_VALIDATION)
			++g_qemuVifFastV4_32Vectors;
#endif
			vif.tag.addr += 16;
			--regs.num;
			++vif.cl;

			if (isFill)
			{
				if (vif.cl <= regs.cycle.cl)
					data += vsize;
				else if (vif.cl == regs.cycle.wl)
					vif.cl = 0;
			}
			else
			{
				data += vsize;
				if (vif.cl >= regs.cycle.wl)
				{
					vif.tag.addr += skip_size;
					vif.cl = 0;
				}
			}
		} while (regs.num);

		return true;
	}

	template <int idx>
	void VitaVifGenericUnpackLoop(const u8* data, bool isFill)
	{
		// PCSX2 owner: Vif_Unpack.cpp::_nVifUnpackLoop(). Vita reuses the
		// interpreter-owned VIFfuncTable entries and avoids the desktop
		// generated nVifUpk table, which is not produced on ARM32 yet.
		vifStruct& vif = GetVifX;
		VIFregisters& regs = vifXRegs;
		const int skip_size = (regs.cycle.cl - regs.cycle.wl) * 16;
		const int upk_num = vif.cmd & 0x1f;
		const u8 vsize = nVifT[upk_num & 0x0f];
		const UNPACKFUNCTYPE unpack =
			VIFfuncTable[idx][regs.mode ? regs.mode : 0][((vif.usn ? 1 : 0) * 2 * 16) + upk_num];

		do
		{
			unpack(VitaVifVuMemPtr<idx>(vif.tag.addr), data);
			vif.tag.addr += 16;
			--regs.num;
			++vif.cl;

			if (isFill)
			{
				if (vif.cl <= regs.cycle.cl)
					data += vsize;
				else if (vif.cl == regs.cycle.wl)
					vif.cl = 0;
			}
			else
			{
				data += vsize;
				if (vif.cl >= regs.cycle.wl)
				{
					vif.tag.addr += skip_size;
					vif.cl = 0;
				}
			}
		} while (regs.num);
	}
} // namespace

void VifUnpackSSE_Init()
{
}

void dVifReset(int idx)
{
}

void dVifRelease(int idx)
{
}

template <int idx>
void dVifUnpack(const u8* data, bool isFill)
{
	if (VitaVifTryFastV4_32<idx>(data, isFill))
		return;

	VitaVifGenericUnpackLoop<idx>(data, isFill);
}

template void dVifUnpack<0>(const u8* data, bool isFill);
template void dVifUnpack<1>(const u8* data, bool isFill);
