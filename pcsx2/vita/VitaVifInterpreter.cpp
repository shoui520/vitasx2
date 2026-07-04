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
u32 g_qemuVifFastVectors = 0;
#endif

namespace
{
	template <int idx>
	u8* VitaVifVuMemPtr(int offset)
	{
		return vuRegs[idx].Mem + (offset & (idx ? 0x3ff0 : 0xff0));
	}

	u8 VitaVifLoadU8(const u8* src)
	{
		return *src;
	}

	s8 VitaVifLoadS8(const u8* src)
	{
		return static_cast<s8>(*src);
	}

	u16 VitaVifLoadU16(const u8* src)
	{
		u16 value;
		std::memcpy(&value, src, sizeof(value));
		return value;
	}

	s16 VitaVifLoadS16(const u8* src)
	{
		s16 value;
		std::memcpy(&value, src, sizeof(value));
		return value;
	}

	u32 VitaVifLoadU32(const u8* src)
	{
		u32 value;
		std::memcpy(&value, src, sizeof(value));
		return value;
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

	void VitaVifStoreWords(u8* dest, u32 x, u32 y, u32 z, u32 w)
	{
		u32* out = reinterpret_cast<u32*>(dest);
		out[0] = x;
		out[1] = y;
		out[2] = z;
		out[3] = w;
	}

	bool VitaVifIsFastMode0NoMaskFormat(u32 format)
	{
		switch (format)
		{
			case 0x00:
			case 0x01:
			case 0x02:
			case 0x04:
			case 0x05:
			case 0x06:
			case 0x0c:
			case 0x0d:
			case 0x0e:
				return true;
			default:
				return false;
		}
	}

	bool VitaVifUnpackMode0NoMaskVector(u8* dest, const u8* src, u32 format, bool usn)
	{
		// PCSX2 owners: Vif_Unpack.cpp::UNPACK_S(), UNPACK_V2(), and
		// UNPACK_V4() when mode == 0 and masking is disabled.
		switch (format)
		{
			case 0x00: // S-32
			{
				const u32 x = VitaVifLoadU32(src);
				VitaVifStoreWords(dest, x, x, x, x);
				return true;
			}

			case 0x01: // S-16
			{
				const u32 x = usn ? static_cast<u32>(VitaVifLoadU16(src)) :
									static_cast<u32>(static_cast<s32>(VitaVifLoadS16(src)));
				VitaVifStoreWords(dest, x, x, x, x);
				return true;
			}

			case 0x02: // S-8
			{
				const u32 x = usn ? static_cast<u32>(VitaVifLoadU8(src)) :
									static_cast<u32>(static_cast<s32>(VitaVifLoadS8(src)));
				VitaVifStoreWords(dest, x, x, x, x);
				return true;
			}

			case 0x04: // V2-32
			{
				const u32 x = VitaVifLoadU32(src);
				const u32 y = VitaVifLoadU32(src + sizeof(u32));
				VitaVifStoreWords(dest, x, y, x, y);
				return true;
			}

			case 0x05: // V2-16
			{
				const u32 x = usn ? static_cast<u32>(VitaVifLoadU16(src)) :
									static_cast<u32>(static_cast<s32>(VitaVifLoadS16(src)));
				const u32 y = usn ? static_cast<u32>(VitaVifLoadU16(src + sizeof(u16))) :
									static_cast<u32>(static_cast<s32>(VitaVifLoadS16(src + sizeof(u16))));
				VitaVifStoreWords(dest, x, y, x, y);
				return true;
			}

			case 0x06: // V2-8
			{
				const u32 x = usn ? static_cast<u32>(VitaVifLoadU8(src)) :
									static_cast<u32>(static_cast<s32>(VitaVifLoadS8(src)));
				const u32 y = usn ? static_cast<u32>(VitaVifLoadU8(src + sizeof(u8))) :
									static_cast<u32>(static_cast<s32>(VitaVifLoadS8(src + sizeof(u8))));
				VitaVifStoreWords(dest, x, y, x, y);
				return true;
			}

			case 0x0c: // V4-32
				VitaVifCopyQword(dest, src);
				return true;

			case 0x0d: // V4-16
			{
				const u32 x = usn ? static_cast<u32>(VitaVifLoadU16(src)) :
									static_cast<u32>(static_cast<s32>(VitaVifLoadS16(src)));
				const u32 y = usn ? static_cast<u32>(VitaVifLoadU16(src + 2)) :
									static_cast<u32>(static_cast<s32>(VitaVifLoadS16(src + 2)));
				const u32 z = usn ? static_cast<u32>(VitaVifLoadU16(src + 4)) :
									static_cast<u32>(static_cast<s32>(VitaVifLoadS16(src + 4)));
				const u32 w = usn ? static_cast<u32>(VitaVifLoadU16(src + 6)) :
									static_cast<u32>(static_cast<s32>(VitaVifLoadS16(src + 6)));
				VitaVifStoreWords(dest, x, y, z, w);
				return true;
			}

			case 0x0e: // V4-8
			{
				const u32 x = usn ? static_cast<u32>(VitaVifLoadU8(src)) :
									static_cast<u32>(static_cast<s32>(VitaVifLoadS8(src)));
				const u32 y = usn ? static_cast<u32>(VitaVifLoadU8(src + 1)) :
									static_cast<u32>(static_cast<s32>(VitaVifLoadS8(src + 1)));
				const u32 z = usn ? static_cast<u32>(VitaVifLoadU8(src + 2)) :
									static_cast<u32>(static_cast<s32>(VitaVifLoadS8(src + 2)));
				const u32 w = usn ? static_cast<u32>(VitaVifLoadU8(src + 3)) :
									static_cast<u32>(static_cast<s32>(VitaVifLoadS8(src + 3)));
				VitaVifStoreWords(dest, x, y, z, w);
				return true;
			}

			default:
				return false;
		}
	}

	template <int idx>
	bool VitaVifTryFastMode0NoMask(const u8* data, bool isFill)
	{
		vifStruct& vif = GetVifX;
		VIFregisters& regs = vifXRegs;
		const u32 upk_num = static_cast<u32>(vif.cmd & 0x1f);
		const u32 format = upk_num & 0x0f;
		if ((upk_num & 0x10) != 0 || regs.mode != 0 || !VitaVifIsFastMode0NoMaskFormat(format))
			return false;

		const int vsize = nVifT[format];
		const int skip_size = (regs.cycle.cl - regs.cycle.wl) * 16;
		do
		{
			VitaVifUnpackMode0NoMaskVector(
				VitaVifVuMemPtr<idx>(vif.tag.addr), data, format, vif.usn != 0);
#if defined(VITASX2_QEMU_VALIDATION)
			++g_qemuVifFastVectors;
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
	if (VitaVifTryFastMode0NoMask<idx>(data, isFill))
		return;

	VitaVifGenericUnpackLoop<idx>(data, isFill);
}

template void dVifUnpack<0>(const u8* data, bool isFill);
template void dVifUnpack<1>(const u8* data, bool isFill);
