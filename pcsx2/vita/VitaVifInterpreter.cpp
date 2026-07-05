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
u32 g_qemuVifNeonVectors = 0;
u32 g_qemuVifGenericVectors = 0;
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
#if defined(VITASX2_QEMU_VALIDATION)
		++g_qemuVifNeonVectors;
#endif
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

	void VitaVifLoadV4_16Words(const u8* src, bool usn, u32& x, u32& y, u32& z, u32& w)
	{
		x = usn ? static_cast<u32>(VitaVifLoadU16(src)) :
				  static_cast<u32>(static_cast<s32>(VitaVifLoadS16(src)));
		y = usn ? static_cast<u32>(VitaVifLoadU16(src + 2)) :
				  static_cast<u32>(static_cast<s32>(VitaVifLoadS16(src + 2)));
		z = usn ? static_cast<u32>(VitaVifLoadU16(src + 4)) :
				  static_cast<u32>(static_cast<s32>(VitaVifLoadS16(src + 4)));
		w = usn ? static_cast<u32>(VitaVifLoadU16(src + 6)) :
				  static_cast<u32>(static_cast<s32>(VitaVifLoadS16(src + 6)));
	}

	void VitaVifLoadV4_8Words(const u8* src, bool usn, u32& x, u32& y, u32& z, u32& w)
	{
		x = usn ? static_cast<u32>(VitaVifLoadU8(src)) :
				  static_cast<u32>(static_cast<s32>(VitaVifLoadS8(src)));
		y = usn ? static_cast<u32>(VitaVifLoadU8(src + 1)) :
				  static_cast<u32>(static_cast<s32>(VitaVifLoadS8(src + 1)));
		z = usn ? static_cast<u32>(VitaVifLoadU8(src + 2)) :
				  static_cast<u32>(static_cast<s32>(VitaVifLoadS8(src + 2)));
		w = usn ? static_cast<u32>(VitaVifLoadU8(src + 3)) :
				  static_cast<u32>(static_cast<s32>(VitaVifLoadS8(src + 3)));
	}

	void VitaVifLoadV3_32Words(const u8* src, u32 generated_iteration, u32 generated_alignment, u32& x, u32& y, u32& z, u32& w)
	{
		// PCSX2 owners: x86/Vif_Dynarec.cpp::ModUnpack() and
		// x86/Vif_UnpackSSE.cpp::xUPK_V3_32(). Generated V3 unpacks use
		// the V4 data shape, but zero W when the generated iteration does
		// not match the packet alignment slot.
		x = VitaVifLoadU32(src);
		y = VitaVifLoadU32(src + 4);
		z = VitaVifLoadU32(src + 8);
		w = (generated_iteration == generated_alignment) ? VitaVifLoadU32(src + 12) : 0;
	}

	void VitaVifLoadV3_16Words(const u8* src, bool usn, u32 generated_iteration, u32 generated_alignment, u32& x, u32& y, u32& z, u32& w)
	{
		// PCSX2 owner: x86/Vif_UnpackSSE.cpp::xUPK_V3_16().
		VitaVifLoadV4_16Words(src, usn, x, y, z, w);
		const u32 result = (((generated_iteration / 4) + 1 + (4 - generated_alignment)) & 0x3);
		if ((generated_iteration & 0x1) == 0 && result == 0)
			w = 0;
	}

	void VitaVifLoadV3_8Words(const u8* src, bool usn, u32 generated_iteration, u32 generated_alignment, u32& x, u32& y, u32& z, u32& w)
	{
		// PCSX2 owners: x86/Vif_Dynarec.cpp::ModUnpack() and
		// x86/Vif_UnpackSSE.cpp::xUPK_V3_8().
		x = usn ? static_cast<u32>(VitaVifLoadU8(src)) :
				  static_cast<u32>(static_cast<s32>(VitaVifLoadS8(src)));
		y = usn ? static_cast<u32>(VitaVifLoadU8(src + 1)) :
				  static_cast<u32>(static_cast<s32>(VitaVifLoadS8(src + 1)));
		z = usn ? static_cast<u32>(VitaVifLoadU8(src + 2)) :
				  static_cast<u32>(static_cast<s32>(VitaVifLoadS8(src + 2)));
		w = (generated_iteration == generated_alignment) ?
				(usn ? static_cast<u32>(VitaVifLoadU8(src + 3)) :
					   static_cast<u32>(static_cast<s32>(VitaVifLoadS8(src + 3)))) :
				0;
	}

#if VITASX2_VIF_HAS_ARM_NEON
	void VitaVifStoreS_32WordsNeon(u8* dest, const u8* src)
	{
		// PCSX2 owner: Vif_Unpack.cpp::UNPACK_S().
		const uint32x4_t value = vdupq_n_u32(VitaVifLoadU32(src));
		vst1q_u32(reinterpret_cast<u32*>(dest), value);
#if defined(VITASX2_QEMU_VALIDATION)
		++g_qemuVifNeonVectors;
#endif
	}

	void VitaVifStoreS_16WordsNeon(u8* dest, const u8* src, bool usn)
	{
		// PCSX2 owner: Vif_Unpack.cpp::UNPACK_S().
		if (usn)
		{
			const uint16x4_t packed = vdup_n_u16(VitaVifLoadU16(src));
			const uint32x4_t widened = vmovl_u16(packed);
			vst1q_u32(reinterpret_cast<u32*>(dest), widened);
		}
		else
		{
			const int16x4_t packed = vdup_n_s16(VitaVifLoadS16(src));
			const int32x4_t widened = vmovl_s16(packed);
			vst1q_s32(reinterpret_cast<s32*>(dest), widened);
		}
#if defined(VITASX2_QEMU_VALIDATION)
		++g_qemuVifNeonVectors;
#endif
	}

	void VitaVifStoreS_8WordsNeon(u8* dest, const u8* src, bool usn)
	{
		// PCSX2 owner: Vif_Unpack.cpp::UNPACK_S().
		if (usn)
		{
			const uint8x8_t packed = vdup_n_u8(VitaVifLoadU8(src));
			const uint16x8_t halves = vmovl_u8(packed);
			const uint32x4_t widened = vmovl_u16(vget_low_u16(halves));
			vst1q_u32(reinterpret_cast<u32*>(dest), widened);
		}
		else
		{
			const int8x8_t packed = vdup_n_s8(VitaVifLoadS8(src));
			const int16x8_t halves = vmovl_s8(packed);
			const int32x4_t widened = vmovl_s16(vget_low_s16(halves));
			vst1q_s32(reinterpret_cast<s32*>(dest), widened);
		}
#if defined(VITASX2_QEMU_VALIDATION)
		++g_qemuVifNeonVectors;
#endif
	}

	void VitaVifStoreV2_32WordsNeon(u8* dest, const u8* src)
	{
		// PCSX2 owner: Vif_Unpack.cpp::UNPACK_V2(); output is v1v0v1v0.
		const u32 x = VitaVifLoadU32(src);
		const u32 y = VitaVifLoadU32(src + sizeof(u32));
		uint32x2_t pair = vdup_n_u32(x);
		pair = vset_lane_u32(y, pair, 1);
		const uint32x4_t value = vcombine_u32(pair, pair);
		vst1q_u32(reinterpret_cast<u32*>(dest), value);
#if defined(VITASX2_QEMU_VALIDATION)
		++g_qemuVifNeonVectors;
#endif
	}

	void VitaVifStoreV2_16WordsNeon(u8* dest, const u8* src, bool usn)
	{
		// PCSX2 owner: Vif_Unpack.cpp::UNPACK_V2(); output is v1v0v1v0.
		const uint32x2_t packed_pair = vdup_n_u32(VitaVifLoadU32(src));
		if (usn)
		{
			const uint32x4_t widened = vmovl_u16(vreinterpret_u16_u32(packed_pair));
			vst1q_u32(reinterpret_cast<u32*>(dest), widened);
		}
		else
		{
			const int32x4_t widened = vmovl_s16(vreinterpret_s16_u32(packed_pair));
			vst1q_s32(reinterpret_cast<s32*>(dest), widened);
		}
#if defined(VITASX2_QEMU_VALIDATION)
		++g_qemuVifNeonVectors;
#endif
	}

	void VitaVifStoreV2_8WordsNeon(u8* dest, const u8* src, bool usn)
	{
		// PCSX2 owner: Vif_Unpack.cpp::UNPACK_V2(); output is v1v0v1v0.
		const u32 xy = VitaVifLoadU16(src);
		const u32 packed = (xy & 0xffu) | (xy & 0xff00u) |
						   ((xy & 0xffu) << 16) | ((xy & 0xff00u) << 16);
		const uint32x2_t packed_pair = vdup_n_u32(packed);
		if (usn)
		{
			const uint16x8_t halves = vmovl_u8(vreinterpret_u8_u32(packed_pair));
			const uint32x4_t widened = vmovl_u16(vget_low_u16(halves));
			vst1q_u32(reinterpret_cast<u32*>(dest), widened);
		}
		else
		{
			const int16x8_t halves = vmovl_s8(vreinterpret_s8_u32(packed_pair));
			const int32x4_t widened = vmovl_s16(vget_low_s16(halves));
			vst1q_s32(reinterpret_cast<s32*>(dest), widened);
		}
#if defined(VITASX2_QEMU_VALIDATION)
		++g_qemuVifNeonVectors;
#endif
	}

	void VitaVifStoreV4_16WordsNeon(u8* dest, const u8* src, bool usn)
	{
		// PCSX2 owner: Vif_Unpack.cpp::UNPACK_V4(). This mirrors
		// arm64/Vif_UnpackNEON.cpp::xUPK_V4_16() for the ARMv7 runtime.
		if (usn)
		{
			const uint16x4_t packed = vld1_u16(reinterpret_cast<const u16*>(src));
			const uint32x4_t widened = vmovl_u16(packed);
			vst1q_u32(reinterpret_cast<u32*>(dest), widened);
		}
		else
		{
			const int16x4_t packed = vld1_s16(reinterpret_cast<const s16*>(src));
			const int32x4_t widened = vmovl_s16(packed);
			vst1q_s32(reinterpret_cast<s32*>(dest), widened);
		}
#if defined(VITASX2_QEMU_VALIDATION)
		++g_qemuVifNeonVectors;
#endif
	}

	void VitaVifStoreV4_8WordsNeon(u8* dest, const u8* src, bool usn)
	{
		// Load exactly one V4-8 packet word, then widen in NEON lanes. The
		// scalar load avoids reading past the current VIF packet item.
		const uint32x2_t packed_word = vdup_n_u32(VitaVifLoadU32(src));
		if (usn)
		{
			const uint16x8_t halves = vmovl_u8(vreinterpret_u8_u32(packed_word));
			const uint32x4_t widened = vmovl_u16(vget_low_u16(halves));
			vst1q_u32(reinterpret_cast<u32*>(dest), widened);
		}
		else
		{
			const int16x8_t halves = vmovl_s8(vreinterpret_s8_u32(packed_word));
			const int32x4_t widened = vmovl_s16(vget_low_s16(halves));
			vst1q_s32(reinterpret_cast<s32*>(dest), widened);
		}
#if defined(VITASX2_QEMU_VALIDATION)
		++g_qemuVifNeonVectors;
#endif
	}

	void VitaVifStoreMaskedMode0WordsNeon(const vifStruct& vif, const VIFregisters& regs, u8* dest, u32 x, u32 y, u32 z, u32 w)
	{
		// PCSX2 owner: Vif_Unpack.cpp::writeXYZW(). MODE 0 masked lanes select
		// data, MaskRow, MaskCol, or write-protect without mutating MaskRow.
		const u32 cl = vif.cl < 3 ? vif.cl : 3;
		const u32 cycle_mask = (regs.mask >> (cl * 8)) & 0xffu;

		uint32x4_t data = vdupq_n_u32(x);
		data = vsetq_lane_u32(y, data, 1);
		data = vsetq_lane_u32(z, data, 2);
		data = vsetq_lane_u32(w, data, 3);

		const int16x4_t shifts = {0, -2, -4, -6};
		const uint16x4_t selectors16 = vand_u16(
			vshl_u16(vdup_n_u16(static_cast<u16>(cycle_mask)), shifts),
			vdup_n_u16(3));
		const uint32x4_t selectors = vmovl_u16(selectors16);
		uint32x4_t result = data;

		const uint32x4_t row = vld1q_u32(vif.MaskRow._u32);
		const uint32x4_t col = vdupq_n_u32(vif.MaskCol._u32[cl]);
		const uint32x4_t old = vld1q_u32(reinterpret_cast<const u32*>(dest));

		result = vbslq_u32(vceqq_u32(selectors, vdupq_n_u32(1)), row, result);
		result = vbslq_u32(vceqq_u32(selectors, vdupq_n_u32(2)), col, result);
		result = vbslq_u32(vceqq_u32(selectors, vdupq_n_u32(3)), old, result);
		vst1q_u32(reinterpret_cast<u32*>(dest), result);
#if defined(VITASX2_QEMU_VALIDATION)
		++g_qemuVifNeonVectors;
#endif
	}

	void VitaVifStoreUnmaskedModeWordsNeon(vifStruct& vif, u8* dest, u32 mode, u32 x, u32 y, u32 z, u32 w)
	{
		// PCSX2 owner: Vif_Unpack.cpp::writeXYZW(). With no write mask, every
		// lane takes the data path, so modes 1-3 map directly to vector row
		// add/replace behavior.
		uint32x4_t data = vdupq_n_u32(x);
		data = vsetq_lane_u32(y, data, 1);
		data = vsetq_lane_u32(z, data, 2);
		data = vsetq_lane_u32(w, data, 3);
		uint32x4_t result = data;

		if (mode == 1 || mode == 2)
		{
			const uint32x4_t row = vld1q_u32(vif.MaskRow._u32);
			result = vaddq_u32(data, row);
		}

		if (mode == 2 || mode == 3)
			vst1q_u32(vif.MaskRow._u32, result);

		vst1q_u32(reinterpret_cast<u32*>(dest), result);
#if defined(VITASX2_QEMU_VALIDATION)
		++g_qemuVifNeonVectors;
#endif
	}
#endif

	u32 VitaVifApplyMode(vifStruct& vif, u32 lane, u32 mode, u32 data)
	{
		switch (mode)
		{
			case 1:
				return data + vif.MaskRow._u32[lane];
			case 2:
				vif.MaskRow._u32[lane] += data;
				return vif.MaskRow._u32[lane];
			case 3:
				vif.MaskRow._u32[lane] = data;
				return data;
			default:
				return data;
		}
	}

	void VitaVifStoreModeWords(vifStruct& vif, const VIFregisters& regs, u8* dest, u32 mode, bool doMask, u32 x, u32 y, u32 z, u32 w)
	{
		// PCSX2 owner: Vif_Unpack.cpp::writeXYZW().
		if (mode == 0 && !doMask)
		{
			VitaVifStoreWords(dest, x, y, z, w);
			return;
		}

#if VITASX2_VIF_HAS_ARM_NEON
		if (!doMask)
		{
			VitaVifStoreUnmaskedModeWordsNeon(vif, dest, mode, x, y, z, w);
			return;
		}

		if (mode == 0)
		{
			VitaVifStoreMaskedMode0WordsNeon(vif, regs, dest, x, y, z, w);
			return;
		}
#endif

		u32* out = reinterpret_cast<u32*>(dest);
		const u32 values[4] = {x, y, z, w};
		const u32 cl = vif.cl < 3 ? vif.cl : 3;
		const u32 cycle_mask = doMask ? ((regs.mask >> (cl * 8)) & 0xffu) : 0u;
		for (u32 lane = 0; lane < 4; lane++)
		{
			switch ((cycle_mask >> (lane * 2)) & 0x3u)
			{
				case 0:
					out[lane] = VitaVifApplyMode(vif, lane, mode, values[lane]);
					break;
				case 1:
					out[lane] = vif.MaskRow._u32[lane];
					break;
				case 2:
					out[lane] = vif.MaskCol._u32[cl];
					break;
				default:
					break;
			}
		}
	}

	bool VitaVifIsFastVectorFormat(u32 format)
	{
		switch (format)
		{
			case 0x00:
			case 0x01:
			case 0x02:
			case 0x04:
			case 0x05:
			case 0x06:
			case 0x08:
			case 0x09:
			case 0x0a:
			case 0x0c:
			case 0x0d:
			case 0x0e:
				return true;
			default:
				return false;
		}
	}

	bool VitaVifIsV3Format(u32 format)
	{
		return format == 0x08 || format == 0x09 || format == 0x0a;
	}

	u32 VitaVifGeneratedAlignment(const vifStruct& vif, u32 format)
	{
		// PCSX2 owner: x86/Vif_Dynarec.cpp::dVifUnpack(). The generated
		// block key keeps full packet alignment for V3-16 and only bit 0
		// for the other V3 formats.
		return (format == 0x09) ? static_cast<u32>(vif.start_aligned) :
								  (static_cast<u32>(vif.start_aligned) & 0x1u);
	}

	bool VitaVifUnpackVector(vifStruct& vif, const VIFregisters& regs, u8* dest, const u8* src, u32 format, u32 mode, bool usn, bool doMask, u32 generated_iteration, u32 generated_alignment)
	{
		// PCSX2 owners: Vif_Unpack.cpp::UNPACK_S(), UNPACK_V2(),
		// UNPACK_V4(), and writeXYZW().
		switch (format)
		{
			case 0x00: // S-32
			{
				if (mode == 0 && !doMask)
				{
#if VITASX2_VIF_HAS_ARM_NEON
					VitaVifStoreS_32WordsNeon(dest, src);
#else
					const u32 x = VitaVifLoadU32(src);
					VitaVifStoreWords(dest, x, x, x, x);
#endif
				}
				else
				{
					const u32 x = VitaVifLoadU32(src);
					VitaVifStoreModeWords(vif, regs, dest, mode, doMask, x, x, x, x);
				}
				return true;
			}

			case 0x01: // S-16
			{
				if (mode == 0 && !doMask)
				{
#if VITASX2_VIF_HAS_ARM_NEON
					VitaVifStoreS_16WordsNeon(dest, src, usn);
#else
					const u32 x = usn ? static_cast<u32>(VitaVifLoadU16(src)) :
										static_cast<u32>(static_cast<s32>(VitaVifLoadS16(src)));
					VitaVifStoreWords(dest, x, x, x, x);
#endif
				}
				else
				{
					const u32 x = usn ? static_cast<u32>(VitaVifLoadU16(src)) :
										static_cast<u32>(static_cast<s32>(VitaVifLoadS16(src)));
					VitaVifStoreModeWords(vif, regs, dest, mode, doMask, x, x, x, x);
				}
				return true;
			}

			case 0x02: // S-8
			{
				if (mode == 0 && !doMask)
				{
#if VITASX2_VIF_HAS_ARM_NEON
					VitaVifStoreS_8WordsNeon(dest, src, usn);
#else
					const u32 x = usn ? static_cast<u32>(VitaVifLoadU8(src)) :
										static_cast<u32>(static_cast<s32>(VitaVifLoadS8(src)));
					VitaVifStoreWords(dest, x, x, x, x);
#endif
				}
				else
				{
					const u32 x = usn ? static_cast<u32>(VitaVifLoadU8(src)) :
										static_cast<u32>(static_cast<s32>(VitaVifLoadS8(src)));
					VitaVifStoreModeWords(vif, regs, dest, mode, doMask, x, x, x, x);
				}
				return true;
			}

			case 0x04: // V2-32
			{
				if (mode == 0 && !doMask)
				{
#if VITASX2_VIF_HAS_ARM_NEON
					VitaVifStoreV2_32WordsNeon(dest, src);
#else
					const u32 x = VitaVifLoadU32(src);
					const u32 y = VitaVifLoadU32(src + sizeof(u32));
					VitaVifStoreWords(dest, x, y, x, y);
#endif
				}
				else
				{
					const u32 x = VitaVifLoadU32(src);
					const u32 y = VitaVifLoadU32(src + sizeof(u32));
					VitaVifStoreModeWords(vif, regs, dest, mode, doMask, x, y, x, y);
				}
				return true;
			}

			case 0x05: // V2-16
			{
				if (mode == 0 && !doMask)
				{
#if VITASX2_VIF_HAS_ARM_NEON
					VitaVifStoreV2_16WordsNeon(dest, src, usn);
#else
					const u32 x = usn ? static_cast<u32>(VitaVifLoadU16(src)) :
										static_cast<u32>(static_cast<s32>(VitaVifLoadS16(src)));
					const u32 y = usn ? static_cast<u32>(VitaVifLoadU16(src + sizeof(u16))) :
										static_cast<u32>(static_cast<s32>(VitaVifLoadS16(src + sizeof(u16))));
					VitaVifStoreWords(dest, x, y, x, y);
#endif
				}
				else
				{
					const u32 x = usn ? static_cast<u32>(VitaVifLoadU16(src)) :
										static_cast<u32>(static_cast<s32>(VitaVifLoadS16(src)));
					const u32 y = usn ? static_cast<u32>(VitaVifLoadU16(src + sizeof(u16))) :
										static_cast<u32>(static_cast<s32>(VitaVifLoadS16(src + sizeof(u16))));
					VitaVifStoreModeWords(vif, regs, dest, mode, doMask, x, y, x, y);
				}
				return true;
			}

			case 0x06: // V2-8
			{
				if (mode == 0 && !doMask)
				{
#if VITASX2_VIF_HAS_ARM_NEON
					VitaVifStoreV2_8WordsNeon(dest, src, usn);
#else
					const u32 x = usn ? static_cast<u32>(VitaVifLoadU8(src)) :
										static_cast<u32>(static_cast<s32>(VitaVifLoadS8(src)));
					const u32 y = usn ? static_cast<u32>(VitaVifLoadU8(src + sizeof(u8))) :
										static_cast<u32>(static_cast<s32>(VitaVifLoadS8(src + sizeof(u8))));
					VitaVifStoreWords(dest, x, y, x, y);
#endif
				}
				else
				{
					const u32 x = usn ? static_cast<u32>(VitaVifLoadU8(src)) :
										static_cast<u32>(static_cast<s32>(VitaVifLoadS8(src)));
					const u32 y = usn ? static_cast<u32>(VitaVifLoadU8(src + sizeof(u8))) :
										static_cast<u32>(static_cast<s32>(VitaVifLoadS8(src + sizeof(u8))));
					VitaVifStoreModeWords(vif, regs, dest, mode, doMask, x, y, x, y);
				}
				return true;
			}

			case 0x08: // V3-32, owned by Vif_Unpack.cpp::UNPACK_V4().
			{
				u32 x, y, z, w;
				VitaVifLoadV3_32Words(src, generated_iteration, generated_alignment, x, y, z, w);
				VitaVifStoreModeWords(vif, regs, dest, mode, doMask, x, y, z, w);
				return true;
			}

			case 0x09: // V3-16, owned by Vif_Unpack.cpp::UNPACK_V4().
			{
				u32 x, y, z, w;
				VitaVifLoadV3_16Words(src, usn, generated_iteration, generated_alignment, x, y, z, w);
				VitaVifStoreModeWords(vif, regs, dest, mode, doMask, x, y, z, w);
				return true;
			}

			case 0x0a: // V3-8, owned by Vif_Unpack.cpp::UNPACK_V4().
			{
				u32 x, y, z, w;
				VitaVifLoadV3_8Words(src, usn, generated_iteration, generated_alignment, x, y, z, w);
				VitaVifStoreModeWords(vif, regs, dest, mode, doMask, x, y, z, w);
				return true;
			}

			case 0x0c: // V4-32
				if (mode == 0 && !doMask)
				{
					VitaVifCopyQword(dest, src);
				}
				else
				{
					VitaVifStoreModeWords(vif, regs, dest, mode, doMask,
						VitaVifLoadU32(src),
						VitaVifLoadU32(src + 4),
						VitaVifLoadU32(src + 8),
						VitaVifLoadU32(src + 12));
				}
				return true;

			case 0x0d: // V4-16
			{
				if (mode == 0 && !doMask)
				{
#if VITASX2_VIF_HAS_ARM_NEON
					VitaVifStoreV4_16WordsNeon(dest, src, usn);
#else
					u32 x, y, z, w;
					VitaVifLoadV4_16Words(src, usn, x, y, z, w);
					VitaVifStoreWords(dest, x, y, z, w);
#endif
				}
				else
				{
					u32 x, y, z, w;
					VitaVifLoadV4_16Words(src, usn, x, y, z, w);
					VitaVifStoreModeWords(vif, regs, dest, mode, doMask, x, y, z, w);
				}
				return true;
			}

			case 0x0e: // V4-8
			{
				if (mode == 0 && !doMask)
				{
#if VITASX2_VIF_HAS_ARM_NEON
					VitaVifStoreV4_8WordsNeon(dest, src, usn);
#else
					u32 x, y, z, w;
					VitaVifLoadV4_8Words(src, usn, x, y, z, w);
					VitaVifStoreWords(dest, x, y, z, w);
#endif
				}
				else
				{
					u32 x, y, z, w;
					VitaVifLoadV4_8Words(src, usn, x, y, z, w);
					VitaVifStoreModeWords(vif, regs, dest, mode, doMask, x, y, z, w);
				}
				return true;
			}

			default:
				return false;
		}
	}

	void VitaVifUnpackV4_5Vector(vifStruct& vif, const VIFregisters& regs, u8* dest, const u8* src, bool doMask)
	{
		// PCSX2 owner: Vif_Unpack.cpp::UNPACK_V4_5(). V4-5 ignores MODE.
		const u32 data = VitaVifLoadU16(src);
		const u32 x = (data & 0x001fu) << 3;
		const u32 y = (data & 0x03e0u) >> 2;
		const u32 z = (data & 0x7c00u) >> 7;
		const u32 w = (data & 0x8000u) >> 8;
		if (doMask)
			VitaVifStoreModeWords(vif, regs, dest, 0, true, x, y, z, w);
		else
			VitaVifStoreWords(dest, x, y, z, w);
	}

	bool VitaVifStorePlainUnmaskedVector(u8* dest, const u8* src, u32 format, bool usn)
	{
		// PCSX2 owners: Vif_Unpack.cpp::UNPACK_S(), UNPACK_V2(),
		// UNPACK_V4(), and UNPACK_V4_5(). V3 uses the same V4 data shape.
		switch (format)
		{
			case 0x00: // S-32
			{
#if VITASX2_VIF_HAS_ARM_NEON
				VitaVifStoreS_32WordsNeon(dest, src);
#else
				const u32 x = VitaVifLoadU32(src);
				VitaVifStoreWords(dest, x, x, x, x);
#endif
				return true;
			}

			case 0x01: // S-16
			{
#if VITASX2_VIF_HAS_ARM_NEON
				VitaVifStoreS_16WordsNeon(dest, src, usn);
#else
				const u32 x = usn ? static_cast<u32>(VitaVifLoadU16(src)) :
									static_cast<u32>(static_cast<s32>(VitaVifLoadS16(src)));
				VitaVifStoreWords(dest, x, x, x, x);
#endif
				return true;
			}

			case 0x02: // S-8
			{
#if VITASX2_VIF_HAS_ARM_NEON
				VitaVifStoreS_8WordsNeon(dest, src, usn);
#else
				const u32 x = usn ? static_cast<u32>(VitaVifLoadU8(src)) :
									static_cast<u32>(static_cast<s32>(VitaVifLoadS8(src)));
				VitaVifStoreWords(dest, x, x, x, x);
#endif
				return true;
			}

			case 0x04: // V2-32
			{
#if VITASX2_VIF_HAS_ARM_NEON
				VitaVifStoreV2_32WordsNeon(dest, src);
#else
				const u32 x = VitaVifLoadU32(src);
				const u32 y = VitaVifLoadU32(src + sizeof(u32));
				VitaVifStoreWords(dest, x, y, x, y);
#endif
				return true;
			}

			case 0x05: // V2-16
			{
#if VITASX2_VIF_HAS_ARM_NEON
				VitaVifStoreV2_16WordsNeon(dest, src, usn);
#else
				const u32 x = usn ? static_cast<u32>(VitaVifLoadU16(src)) :
									static_cast<u32>(static_cast<s32>(VitaVifLoadS16(src)));
				const u32 y = usn ? static_cast<u32>(VitaVifLoadU16(src + sizeof(u16))) :
									static_cast<u32>(static_cast<s32>(VitaVifLoadS16(src + sizeof(u16))));
				VitaVifStoreWords(dest, x, y, x, y);
#endif
				return true;
			}

			case 0x06: // V2-8
			{
#if VITASX2_VIF_HAS_ARM_NEON
				VitaVifStoreV2_8WordsNeon(dest, src, usn);
#else
				const u32 x = usn ? static_cast<u32>(VitaVifLoadU8(src)) :
									static_cast<u32>(static_cast<s32>(VitaVifLoadS8(src)));
				const u32 y = usn ? static_cast<u32>(VitaVifLoadU8(src + sizeof(u8))) :
									static_cast<u32>(static_cast<s32>(VitaVifLoadS8(src + sizeof(u8))));
				VitaVifStoreWords(dest, x, y, x, y);
#endif
				return true;
			}

			case 0x08: // V3-32, owned by Vif_Unpack.cpp::UNPACK_V4().
			case 0x0c: // V4-32
				VitaVifCopyQword(dest, src);
				return true;

			case 0x09: // V3-16, owned by Vif_Unpack.cpp::UNPACK_V4().
			case 0x0d: // V4-16
			{
#if VITASX2_VIF_HAS_ARM_NEON
				VitaVifStoreV4_16WordsNeon(dest, src, usn);
#else
				u32 x, y, z, w;
				VitaVifLoadV4_16Words(src, usn, x, y, z, w);
				VitaVifStoreWords(dest, x, y, z, w);
#endif
				return true;
			}

			case 0x0a: // V3-8, owned by Vif_Unpack.cpp::UNPACK_V4().
			case 0x0e: // V4-8
			{
#if VITASX2_VIF_HAS_ARM_NEON
				VitaVifStoreV4_8WordsNeon(dest, src, usn);
#else
				u32 x, y, z, w;
				VitaVifLoadV4_8Words(src, usn, x, y, z, w);
				VitaVifStoreWords(dest, x, y, z, w);
#endif
				return true;
			}

			case 0x0f: // V4-5
			{
				const u32 data = VitaVifLoadU16(src);
				const u32 x = (data & 0x001fu) << 3;
				const u32 y = (data & 0x03e0u) >> 2;
				const u32 z = (data & 0x7c00u) >> 7;
				const u32 w = (data & 0x8000u) >> 8;
				VitaVifStoreWords(dest, x, y, z, w);
				return true;
			}

			default:
				return false;
		}
	}

	template <int idx>
	bool VitaVifTryFastPlainUnmasked(const u8* data, bool isFill)
	{
		vifStruct& vif = GetVifX;
		VIFregisters& regs = vifXRegs;
		const u32 upk_num = static_cast<u32>(vif.cmd & 0x1f);
		const u32 format = upk_num & 0x0f;
		const bool doMask = (upk_num & 0x10) != 0;
		if (doMask || ((regs.mode & 0x3) != 0 && format != 0x0f))
			return false;
		if (nVifT[format] == 0)
			return false;
		if (VitaVifIsV3Format(format))
			return false;

		const int vsize = nVifT[format];
		const int skip_size = (regs.cycle.cl - regs.cycle.wl) * 16;
		do
		{
			if (!VitaVifStorePlainUnmaskedVector(VitaVifVuMemPtr<idx>(vif.tag.addr), data, format, vif.usn != 0))
				return false;
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
	bool VitaVifTryFastV4_5(const u8* data, bool isFill)
	{
		vifStruct& vif = GetVifX;
		VIFregisters& regs = vifXRegs;
		const u32 upk_num = static_cast<u32>(vif.cmd & 0x1f);
		if ((upk_num & 0x0f) != 0x0f)
			return false;

		const bool doMask = (upk_num & 0x10) != 0;
		constexpr int vsize = 2;
		const int skip_size = (regs.cycle.cl - regs.cycle.wl) * 16;
		do
		{
			VitaVifUnpackV4_5Vector(vif, regs, VitaVifVuMemPtr<idx>(vif.tag.addr), data, doMask);
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
	bool VitaVifTryFastUnpack(const u8* data, bool isFill)
	{
		vifStruct& vif = GetVifX;
		VIFregisters& regs = vifXRegs;
		const u32 upk_num = static_cast<u32>(vif.cmd & 0x1f);
		const u32 format = upk_num & 0x0f;
		if (!VitaVifIsFastVectorFormat(format))
			return false;

		const bool doMask = (upk_num & 0x10) != 0;
		const u32 mode = regs.mode & 0x3;
		const int vsize = nVifT[format];
		const int skip_size = (regs.cycle.cl - regs.cycle.wl) * 16;
		u32 generated_iteration = 0;
		const u32 generated_alignment = VitaVifGeneratedAlignment(vif, format);
		do
		{
			u32 vector_iteration = generated_iteration;
			if (format == 0x09 || format == 0x0a)
				vector_iteration = ++generated_iteration;

			VitaVifUnpackVector(
				vif, regs, VitaVifVuMemPtr<idx>(vif.tag.addr), data, format, mode, vif.usn != 0, doMask,
				vector_iteration, generated_alignment);
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

			if (format == 0x08)
				generated_iteration = (generated_iteration + 1) & 0x1u;
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
#if defined(VITASX2_QEMU_VALIDATION)
			++g_qemuVifGenericVectors;
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
	if (VitaVifTryFastPlainUnmasked<idx>(data, isFill))
		return;

	if (VitaVifTryFastV4_5<idx>(data, isFill))
		return;

	if (VitaVifTryFastUnpack<idx>(data, isFill))
		return;

	VitaVifGenericUnpackLoop<idx>(data, isFill);
}

template void dVifUnpack<0>(const u8* data, bool isFill);
template void dVifUnpack<1>(const u8* data, bool isFill);
