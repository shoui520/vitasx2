// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "common/Assertions.h"
#include "common/Console.h"

#include "Vif_Dynarec.h"
#include "Vif_Dma.h"
#include "Vif_Unpack.h"
#include "VUmicro.h"

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
u32 g_qemuVifBurstVectors = 0;
u32 g_qemuVifBurstWidenVectors = 0;
u32 g_qemuVifBurstColorVectors = 0;
u32 g_qemuVifBurstSAndV2Vectors = 0;
u32 g_qemuVifBurstV3Vectors = 0;
u32 g_qemuVifBurstCopyVectors = 0;
u32 g_qemuVifBurstCopy64ByteGroups = 0;
u32 g_qemuVifBurstCopy128ByteGroups = 0;
u32 g_qemuVifBurstCopy256ByteGroups = 0;
u32 g_qemuVifBurstCopy1024ByteGroups = 0;
u32 g_qemuVifBurstModeMaskVectors = 0;
u32 g_qemuVifBurstV4_32ModeVectors = 0;
u32 g_qemuVifBurstV4_16PairGroups = 0;
u32 g_qemuVifCycleBurstVectors = 0;
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

#if VITASX2_VIF_HAS_ARM_NEON
	void VitaVifCopy64Bytes(u8* dest, const u8* src)
	{
		const uint32x4_t qword0 = vld1q_u32(reinterpret_cast<const u32*>(src));
		const uint32x4_t qword1 = vld1q_u32(reinterpret_cast<const u32*>(src + 16));
		const uint32x4_t qword2 = vld1q_u32(reinterpret_cast<const u32*>(src + 32));
		const uint32x4_t qword3 = vld1q_u32(reinterpret_cast<const u32*>(src + 48));
		vst1q_u32(reinterpret_cast<u32*>(dest), qword0);
		vst1q_u32(reinterpret_cast<u32*>(dest + 16), qword1);
		vst1q_u32(reinterpret_cast<u32*>(dest + 32), qword2);
		vst1q_u32(reinterpret_cast<u32*>(dest + 48), qword3);
	}

	void VitaVifCopy128Bytes(u8* dest, const u8* src)
	{
		VitaVifCopy64Bytes(dest, src);
		VitaVifCopy64Bytes(dest + 64, src + 64);
	}

	void VitaVifCopy256Bytes(u8* dest, const u8* src)
	{
		VitaVifCopy128Bytes(dest, src);
		VitaVifCopy128Bytes(dest + 128, src + 128);
	}

	void VitaVifCopy1024Bytes(u8* dest, const u8* src)
	{
		VitaVifCopy256Bytes(dest, src);
		VitaVifCopy256Bytes(dest + 256, src + 256);
		VitaVifCopy256Bytes(dest + 512, src + 512);
		VitaVifCopy256Bytes(dest + 768, src + 768);
	}
#endif

	void VitaVifCopyQwordBurst(u8* dest, const u8* src, u32 count)
	{
		const u32 groups1024 = count >> 6;
#if VITASX2_VIF_HAS_ARM_NEON
		for (u32 i = 0; i < groups1024; i++)
		{
			if ((i + 1) < groups1024)
				__builtin_prefetch(src + 1024, 0, 1);

			VitaVifCopy1024Bytes(dest, src);
			src += 1024;
			dest += 1024;
		}

		const u32 remaining_after_1024 = count & 63u;
		const u32 groups256 = remaining_after_1024 >> 4;
		for (u32 i = 0; i < groups256; i++)
		{
			if ((i + 1) < groups256)
				__builtin_prefetch(src + 256, 0, 1);

			VitaVifCopy256Bytes(dest, src);
			src += 256;
			dest += 256;
		}

		const u32 remaining_after_256 = remaining_after_1024 & 15u;
		const u32 groups128 = remaining_after_256 >> 3;
		for (u32 i = 0; i < groups128; i++)
		{
			if ((i + 1) < groups128)
				__builtin_prefetch(src + 128, 0, 1);

			VitaVifCopy128Bytes(dest, src);
			src += 128;
			dest += 128;
		}

		const u32 remaining_after_128 = remaining_after_256 & 7u;
		const u32 groups64 = remaining_after_128 >> 2;
		for (u32 i = 0; i < groups64; i++)
		{
			if ((i + 1) < groups64)
				__builtin_prefetch(src + 64, 0, 1);

			VitaVifCopy64Bytes(dest, src);
			src += 64;
			dest += 64;
		}

		const u32 tail_qwords = remaining_after_128 & 3u;
		for (u32 i = 0; i < tail_qwords; i++)
		{
			const uint32x4_t value = vld1q_u32(reinterpret_cast<const u32*>(src));
			vst1q_u32(reinterpret_cast<u32*>(dest), value);
			src += 16;
			dest += 16;
		}
#if defined(VITASX2_QEMU_VALIDATION)
		g_qemuVifNeonVectors += count;
#endif
#else
		std::memcpy(dest, src, count * 16);
#endif
#if defined(VITASX2_QEMU_VALIDATION)
		const u32 validation_remaining_after_1024 = count & 63u;
		const u32 validation_groups256 = validation_remaining_after_1024 >> 4;
		const u32 validation_remaining_after_256 = validation_remaining_after_1024 & 15u;
		const u32 validation_groups128 = validation_remaining_after_256 >> 3;
		g_qemuVifFastVectors += count;
		g_qemuVifBurstVectors += count;
		g_qemuVifBurstCopyVectors += count;
		g_qemuVifBurstCopy64ByteGroups += (groups1024 << 4) + (validation_groups256 << 2) + (validation_groups128 << 1) + ((validation_remaining_after_256 & 7u) / 4u);
		g_qemuVifBurstCopy128ByteGroups += (groups1024 << 3) + (validation_groups256 << 1) + validation_groups128;
		g_qemuVifBurstCopy256ByteGroups += (groups1024 << 2) + validation_groups256;
		g_qemuVifBurstCopy1024ByteGroups += groups1024;
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
	void VitaVifStoreVectorNeon(u8* dest, uint32x4_t value)
	{
		vst1q_u32(reinterpret_cast<u32*>(dest), value);
#if defined(VITASX2_QEMU_VALIDATION)
		++g_qemuVifNeonVectors;
#endif
	}

	uint32x4_t VitaVifVectorFromWordsNeon(u32 x, u32 y, u32 z, u32 w)
	{
		uint32x4_t value = vdupq_n_u32(x);
		value = vsetq_lane_u32(y, value, 1);
		value = vsetq_lane_u32(z, value, 2);
		value = vsetq_lane_u32(w, value, 3);
		return value;
	}

	uint32x4_t VitaVifLoadS_32VectorNeon(const u8* src)
	{
		// PCSX2 owner: Vif_Unpack.cpp::UNPACK_S().
		return vdupq_n_u32(VitaVifLoadU32(src));
	}

	uint32x4_t VitaVifLoadS_16VectorNeon(const u8* src, bool usn)
	{
		// PCSX2 owner: Vif_Unpack.cpp::UNPACK_S().
		if (usn)
		{
			const uint16x4_t packed = vdup_n_u16(VitaVifLoadU16(src));
			return vmovl_u16(packed);
		}

		const int16x4_t packed = vdup_n_s16(VitaVifLoadS16(src));
		return vreinterpretq_u32_s32(vmovl_s16(packed));
	}

	uint32x4_t VitaVifLoadS_8VectorNeon(const u8* src, bool usn)
	{
		// PCSX2 owner: Vif_Unpack.cpp::UNPACK_S().
		if (usn)
		{
			const uint8x8_t packed = vdup_n_u8(VitaVifLoadU8(src));
			const uint16x8_t halves = vmovl_u8(packed);
			return vmovl_u16(vget_low_u16(halves));
		}

		const int8x8_t packed = vdup_n_s8(VitaVifLoadS8(src));
		const int16x8_t halves = vmovl_s8(packed);
		return vreinterpretq_u32_s32(vmovl_s16(vget_low_s16(halves)));
	}

	uint32x4_t VitaVifLoadV2_32VectorNeon(const u8* src)
	{
		// PCSX2 owner: Vif_Unpack.cpp::UNPACK_V2(); output is v1v0v1v0.
		const u32 x = VitaVifLoadU32(src);
		const u32 y = VitaVifLoadU32(src + sizeof(u32));
		uint32x2_t pair = vdup_n_u32(x);
		pair = vset_lane_u32(y, pair, 1);
		return vcombine_u32(pair, pair);
	}

	uint32x4_t VitaVifLoadV2_16VectorNeon(const u8* src, bool usn)
	{
		// PCSX2 owner: Vif_Unpack.cpp::UNPACK_V2(); output is v1v0v1v0.
		const uint32x2_t packed_pair = vdup_n_u32(VitaVifLoadU32(src));
		if (usn)
			return vmovl_u16(vreinterpret_u16_u32(packed_pair));

		return vreinterpretq_u32_s32(vmovl_s16(vreinterpret_s16_u32(packed_pair)));
	}

	uint32x4_t VitaVifLoadV2_8VectorNeon(const u8* src, bool usn)
	{
		// PCSX2 owner: Vif_Unpack.cpp::UNPACK_V2(); output is v1v0v1v0.
		const u32 xy = VitaVifLoadU16(src);
		const u32 packed = (xy & 0xffu) | (xy & 0xff00u) |
						   ((xy & 0xffu) << 16) | ((xy & 0xff00u) << 16);
		const uint32x2_t packed_pair = vdup_n_u32(packed);
		if (usn)
		{
			const uint16x8_t halves = vmovl_u8(vreinterpret_u8_u32(packed_pair));
			return vmovl_u16(vget_low_u16(halves));
		}

		const int16x8_t halves = vmovl_s8(vreinterpret_s8_u32(packed_pair));
		return vreinterpretq_u32_s32(vmovl_s16(vget_low_s16(halves)));
	}

	uint32x4_t VitaVifLoadV4_16VectorNeon(const u8* src, bool usn)
	{
		// PCSX2 owner: Vif_Unpack.cpp::UNPACK_V4(). This mirrors
		// arm64/Vif_UnpackNEON.cpp::xUPK_V4_16() for the ARMv7 runtime.
		if (usn)
		{
			const uint16x4_t packed = vld1_u16(reinterpret_cast<const u16*>(src));
			return vmovl_u16(packed);
		}

		const int16x4_t packed = vld1_s16(reinterpret_cast<const s16*>(src));
		return vreinterpretq_u32_s32(vmovl_s16(packed));
	}

	uint32x4_t VitaVifLoadV4_8VectorNeon(const u8* src, bool usn)
	{
		// Load exactly one V4-8 packet word, then widen in NEON lanes. The
		// scalar load avoids reading past the current VIF packet item.
		const uint32x2_t packed_word = vdup_n_u32(VitaVifLoadU32(src));
		if (usn)
		{
			const uint16x8_t halves = vmovl_u8(vreinterpret_u8_u32(packed_word));
			return vmovl_u16(vget_low_u16(halves));
		}

		const int16x8_t halves = vmovl_s8(vreinterpret_s8_u32(packed_word));
		return vreinterpretq_u32_s32(vmovl_s16(vget_low_s16(halves)));
	}

	uint32x4_t VitaVifLoadV4_5VectorNeon(const u8* src)
	{
		// PCSX2 owners: Vif_Unpack.cpp::UNPACK_V4_5() and
		// arm64/Vif_UnpackNEON.cpp::xUPK_V4_5(). V4-5 ignores USN/MODE and
		// expands packed 5:5:5:1 color into byte-scaled XYZW lanes.
		const u32 data = VitaVifLoadU16(src);
		return VitaVifVectorFromWordsNeon(
			(data & 0x001fu) << 3,
			(data & 0x03e0u) >> 2,
			(data & 0x7c00u) >> 7,
			(data & 0x8000u) >> 8);
	}

	bool VitaVifLoadUnpackVectorNeon(
		const u8* src,
		u32 format,
		bool usn,
		u32 generated_iteration,
		u32 generated_alignment,
		uint32x4_t& out)
	{
		// PCSX2 owners: Vif_Unpack.cpp::UNPACK_S(), UNPACK_V2(), UNPACK_V4()
		// and x86/Vif_UnpackSSE.cpp::xUPK_V3_*() for generated V3.
		switch (format)
		{
			case 0x00:
				out = VitaVifLoadS_32VectorNeon(src);
				return true;
			case 0x01:
				out = VitaVifLoadS_16VectorNeon(src, usn);
				return true;
			case 0x02:
				out = VitaVifLoadS_8VectorNeon(src, usn);
				return true;
			case 0x04:
				out = VitaVifLoadV2_32VectorNeon(src);
				return true;
			case 0x05:
				out = VitaVifLoadV2_16VectorNeon(src, usn);
				return true;
			case 0x06:
				out = VitaVifLoadV2_8VectorNeon(src, usn);
				return true;
			case 0x08:
				out = vld1q_u32(reinterpret_cast<const u32*>(src));
				if (generated_iteration != generated_alignment)
					out = vsetq_lane_u32(0, out, 3);
				return true;
			case 0x09:
				out = VitaVifLoadV4_16VectorNeon(src, usn);
				{
					const u32 result = (((generated_iteration / 4) + 1 + (4 - generated_alignment)) & 0x3);
					if ((generated_iteration & 0x1) == 0 && result == 0)
						out = vsetq_lane_u32(0, out, 3);
				}
				return true;
			case 0x0a:
				out = VitaVifLoadV4_8VectorNeon(src, usn);
				if (generated_iteration != generated_alignment)
					out = vsetq_lane_u32(0, out, 3);
				return true;
			case 0x0c:
				out = vld1q_u32(reinterpret_cast<const u32*>(src));
				return true;
			case 0x0d:
				out = VitaVifLoadV4_16VectorNeon(src, usn);
				return true;
			case 0x0e:
				out = VitaVifLoadV4_8VectorNeon(src, usn);
				return true;
			default:
				return false;
		}
	}

	void VitaVifStoreS_32WordsNeon(u8* dest, const u8* src)
	{
		VitaVifStoreVectorNeon(dest, VitaVifLoadS_32VectorNeon(src));
	}

	void VitaVifStoreS_16WordsNeon(u8* dest, const u8* src, bool usn)
	{
		VitaVifStoreVectorNeon(dest, VitaVifLoadS_16VectorNeon(src, usn));
	}

	void VitaVifStoreS_8WordsNeon(u8* dest, const u8* src, bool usn)
	{
		VitaVifStoreVectorNeon(dest, VitaVifLoadS_8VectorNeon(src, usn));
	}

	void VitaVifStoreV2_32WordsNeon(u8* dest, const u8* src)
	{
		VitaVifStoreVectorNeon(dest, VitaVifLoadV2_32VectorNeon(src));
	}

	void VitaVifStoreV2_16WordsNeon(u8* dest, const u8* src, bool usn)
	{
		VitaVifStoreVectorNeon(dest, VitaVifLoadV2_16VectorNeon(src, usn));
	}

	void VitaVifStoreV2_8WordsNeon(u8* dest, const u8* src, bool usn)
	{
		VitaVifStoreVectorNeon(dest, VitaVifLoadV2_8VectorNeon(src, usn));
	}

	void VitaVifStoreV4_16WordsNeon(u8* dest, const u8* src, bool usn)
	{
		VitaVifStoreVectorNeon(dest, VitaVifLoadV4_16VectorNeon(src, usn));
	}

	void VitaVifStoreV4_16PairBurstNeon(u8* dest, const u8* src, u32 count, bool usn)
	{
		// PCSX2 owner: Vif_Unpack.cpp::UNPACK_V4(). Plain V4-16 bursts write
		// one widened qword per vector; two source vectors fit in one NEON qword.
		const u32 pairs = count >> 1;
		for (u32 i = 0; i < pairs; i++)
		{
			const uint16x8_t packed = vld1q_u16(reinterpret_cast<const u16*>(src));
			if (usn)
			{
				VitaVifStoreVectorNeon(dest, vmovl_u16(vget_low_u16(packed)));
				VitaVifStoreVectorNeon(dest + 16, vmovl_u16(vget_high_u16(packed)));
			}
			else
			{
				const int16x8_t signed_packed = vreinterpretq_s16_u16(packed);
				VitaVifStoreVectorNeon(dest, vreinterpretq_u32_s32(vmovl_s16(vget_low_s16(signed_packed))));
				VitaVifStoreVectorNeon(dest + 16, vreinterpretq_u32_s32(vmovl_s16(vget_high_s16(signed_packed))));
			}

			src += 16;
			dest += 32;
		}

		if ((count & 1u) != 0)
			VitaVifStoreV4_16WordsNeon(dest, src, usn);

#if defined(VITASX2_QEMU_VALIDATION)
		g_qemuVifBurstV4_16PairGroups += pairs;
#endif
	}

	void VitaVifStoreV4_8WordsNeon(u8* dest, const u8* src, bool usn)
	{
		VitaVifStoreVectorNeon(dest, VitaVifLoadV4_8VectorNeon(src, usn));
	}

	void VitaVifStoreMaskedMode0VectorNeon(const vifStruct& vif, const VIFregisters& regs, u8* dest, uint32x4_t data)
	{
		// PCSX2 owner: Vif_Unpack.cpp::writeXYZW(). MODE 0 masked lanes select
		// data, MaskRow, MaskCol, or write-protect without mutating MaskRow.
		const u32 cl = vif.cl < 3 ? vif.cl : 3;
		const u32 cycle_mask = (regs.mask >> (cl * 8)) & 0xffu;

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
		VitaVifStoreVectorNeon(dest, result);
	}

	void VitaVifStoreUnmaskedModeVectorNeon(vifStruct& vif, u8* dest, u32 mode, uint32x4_t data)
	{
		// PCSX2 owner: Vif_Unpack.cpp::writeXYZW(). With no write mask, every
		// lane takes the data path, so modes 1-3 map directly to vector row
		// add/replace behavior.
		uint32x4_t result = data;

		if (mode == 1 || mode == 2)
		{
			const uint32x4_t row = vld1q_u32(vif.MaskRow._u32);
			result = vaddq_u32(data, row);
		}

		if (mode == 2 || mode == 3)
			vst1q_u32(vif.MaskRow._u32, result);

		VitaVifStoreVectorNeon(dest, result);
	}

	void VitaVifStoreMaskedModeVectorNeon(vifStruct& vif, const VIFregisters& regs, u8* dest, u32 mode, uint32x4_t data)
	{
		// PCSX2 owner: Vif_Unpack.cpp::writeXYZW(). Masked modes still update
		// MaskRow only for selector-zero data lanes; row/col/protect lanes do
		// not participate in the mode side effect.
		const u32 cl = vif.cl < 3 ? vif.cl : 3;
		const u32 cycle_mask = (regs.mask >> (cl * 8)) & 0xffu;

		const int16x4_t shifts = {0, -2, -4, -6};
		const uint16x4_t selectors16 = vand_u16(
			vshl_u16(vdup_n_u16(static_cast<u16>(cycle_mask)), shifts),
			vdup_n_u16(3));
		const uint32x4_t selectors = vmovl_u16(selectors16);
		const uint32x4_t row = vld1q_u32(vif.MaskRow._u32);
		uint32x4_t mode_data = data;

		if (mode == 1 || mode == 2)
			mode_data = vaddq_u32(data, row);

		const uint32x4_t data_mask = vceqq_u32(selectors, vdupq_n_u32(0));
		if (mode == 2 || mode == 3)
			vst1q_u32(vif.MaskRow._u32, vbslq_u32(data_mask, mode_data, row));

		const uint32x4_t col = vdupq_n_u32(vif.MaskCol._u32[cl]);
		const uint32x4_t old = vld1q_u32(reinterpret_cast<const u32*>(dest));
		uint32x4_t result = mode_data;
		result = vbslq_u32(vceqq_u32(selectors, vdupq_n_u32(1)), row, result);
		result = vbslq_u32(vceqq_u32(selectors, vdupq_n_u32(2)), col, result);
		result = vbslq_u32(vceqq_u32(selectors, vdupq_n_u32(3)), old, result);
		VitaVifStoreVectorNeon(dest, result);
	}

	void VitaVifStoreModeVectorNeon(vifStruct& vif, const VIFregisters& regs, u8* dest, u32 mode, bool doMask, uint32x4_t data)
	{
		// PCSX2 owner: Vif_Unpack.cpp::writeXYZW().
		if (mode == 0 && !doMask)
		{
			VitaVifStoreVectorNeon(dest, data);
			return;
		}

		if (!doMask)
		{
			VitaVifStoreUnmaskedModeVectorNeon(vif, dest, mode, data);
			return;
		}

		if (mode == 0)
		{
			VitaVifStoreMaskedMode0VectorNeon(vif, regs, dest, data);
			return;
		}

		VitaVifStoreMaskedModeVectorNeon(vif, regs, dest, mode, data);
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
		VitaVifStoreModeVectorNeon(vif, regs, dest, mode, doMask, VitaVifVectorFromWordsNeon(x, y, z, w));
		return;
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

	bool VitaVifIsSOrV2Format(u32 format)
	{
		return format <= 0x02 || (format >= 0x04 && format <= 0x06);
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
#if VITASX2_VIF_HAS_ARM_NEON
		uint32x4_t unpacked;
		if (VitaVifLoadUnpackVectorNeon(src, format, usn, generated_iteration, generated_alignment, unpacked))
		{
			VitaVifStoreModeVectorNeon(vif, regs, dest, mode, doMask, unpacked);
			return true;
		}
#endif
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
#if VITASX2_VIF_HAS_ARM_NEON
		const uint32x4_t unpacked = VitaVifLoadV4_5VectorNeon(src);
		if (doMask)
			VitaVifStoreModeVectorNeon(vif, regs, dest, 0, true, unpacked);
		else
			VitaVifStoreVectorNeon(dest, unpacked);
		return;
#endif
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
#if VITASX2_VIF_HAS_ARM_NEON
				VitaVifStoreVectorNeon(dest, VitaVifLoadV4_5VectorNeon(src));
#else
				const u32 data = VitaVifLoadU16(src);
				const u32 x = (data & 0x001fu) << 3;
				const u32 y = (data & 0x03e0u) >> 2;
				const u32 z = (data & 0x7c00u) >> 7;
				const u32 w = (data & 0x8000u) >> 8;
				VitaVifStoreWords(dest, x, y, z, w);
#endif
				return true;
			}

			default:
				return false;
		}
	}

	template <int idx>
	bool VitaVifTryFastSAndV2Burst(const u8* data, bool isFill)
	{
		// PCSX2 owners: Vif_Unpack.cpp::UNPACK_S() and UNPACK_V2(). This path
		// keeps only the contiguous no-mode/no-mask case; MODE, row/col masks,
		// fill, skip, and VU-memory wrap stay on the per-vector path.
		vifStruct& vif = GetVifX;
		VIFregisters& regs = vifXRegs;
		const u32 upk_num = static_cast<u32>(vif.cmd & 0x1f);
		const u32 format = upk_num & 0x0f;
		const u32 wl = regs.cycle.wl;
		if (isFill || (upk_num & 0x10) != 0 || !VitaVifIsSOrV2Format(format) ||
			(regs.mode & 0x3) != 0 || regs.cycle.cl != wl || wl == 0 ||
			vif.cl != 0 || regs.num == 0)
		{
			return false;
		}

		const u32 count = regs.num;
		const u32 bytes = count * 16;
		const u32 vu_mem_size = idx ? VU1_MEMSIZE : VU0_MEMSIZE;
		const u32 vu_mem_offset = vif.tag.addr & (idx ? 0x3ff0u : 0xff0u);
		if (vu_mem_offset + bytes > vu_mem_size)
			return false;

		const u32 vsize = nVifT[format];
		u8* dest = vuRegs[idx].Mem + vu_mem_offset;
		for (u32 i = 0; i < count; i++)
		{
			if (!VitaVifStorePlainUnmaskedVector(dest + i * 16, data + i * vsize, format, vif.usn != 0))
				return false;
#if defined(VITASX2_QEMU_VALIDATION)
			++g_qemuVifFastVectors;
			++g_qemuVifBurstVectors;
			++g_qemuVifBurstSAndV2Vectors;
#endif
		}

		vif.tag.addr += bytes;
		vif.cl = static_cast<u8>(count % wl);
		regs.num = 0;
		return true;
	}

	template <int idx>
	bool VitaVifTryFastV3Burst(const u8* data, bool isFill)
	{
		// PCSX2 owners: x86/Vif_Dynarec.cpp::ModUnpack() and
		// x86/Vif_UnpackSSE.cpp::xUPK_V3_*(). Generated V3 has alignment-based
		// W-lane zeroing, so only the contiguous no-mode/no-mask case is hoisted.
		vifStruct& vif = GetVifX;
		VIFregisters& regs = vifXRegs;
		const u32 upk_num = static_cast<u32>(vif.cmd & 0x1f);
		const u32 format = upk_num & 0x0f;
		const u32 wl = regs.cycle.wl;
		if (isFill || (upk_num & 0x10) != 0 || !VitaVifIsV3Format(format) ||
			(regs.mode & 0x3) != 0 || regs.cycle.cl != wl || wl == 0 ||
			vif.cl != 0 || regs.num == 0)
		{
			return false;
		}

		const u32 count = regs.num;
		const u32 bytes = count * 16;
		const u32 vu_mem_size = idx ? VU1_MEMSIZE : VU0_MEMSIZE;
		const u32 vu_mem_offset = vif.tag.addr & (idx ? 0x3ff0u : 0xff0u);
		if (vu_mem_offset + bytes > vu_mem_size)
			return false;

		const u32 vsize = nVifT[format];
		const u32 generated_alignment = VitaVifGeneratedAlignment(vif, format);
		u32 generated_iteration = 0;
		u8* dest = vuRegs[idx].Mem + vu_mem_offset;
		for (u32 i = 0; i < count; i++)
		{
			u32 vector_iteration = generated_iteration;
			if (format == 0x09 || format == 0x0a)
				vector_iteration = ++generated_iteration;

			if (!VitaVifUnpackVector(
					vif, regs, dest + i * 16, data + i * vsize, format, 0, vif.usn != 0, false,
					vector_iteration, generated_alignment))
			{
				return false;
			}

			if (format == 0x08)
				generated_iteration = (generated_iteration + 1) & 0x1u;
#if defined(VITASX2_QEMU_VALIDATION)
			++g_qemuVifFastVectors;
			++g_qemuVifBurstVectors;
			++g_qemuVifBurstV3Vectors;
#endif
		}

		vif.tag.addr += bytes;
		vif.cl = static_cast<u8>(count % wl);
		regs.num = 0;
		return true;
	}

	template <int idx>
	bool VitaVifTryFastV4Burst(const u8* data, bool isFill)
	{
		// PCSX2 owner: Vif_Unpack.cpp::UNPACK_V4(). This is the contiguous
		// no-mask/no-mode V4 case; row/col, fill, skip, and VU-memory wrap
		// cases stay on the generic fast vector path below.
		vifStruct& vif = GetVifX;
		VIFregisters& regs = vifXRegs;
		const u32 upk_num = static_cast<u32>(vif.cmd & 0x1f);
		const u32 format = upk_num & 0x0f;
		const u32 wl = regs.cycle.wl;
		if (isFill || (upk_num & 0x10) != 0 ||
			(format != 0x0c && format != 0x0d && format != 0x0e) ||
			(regs.mode & 0x3) != 0 || regs.cycle.cl != wl || wl == 0 ||
			vif.cl != 0 || regs.num == 0)
		{
			return false;
		}

		const u32 count = regs.num;
		const u32 bytes = count * 16;
		const u32 vu_mem_size = idx ? VU1_MEMSIZE : VU0_MEMSIZE;
		const u32 vu_mem_offset = vif.tag.addr & (idx ? 0x3ff0u : 0xff0u);
		if (vu_mem_offset + bytes > vu_mem_size)
			return false;

		u8* dest = vuRegs[idx].Mem + vu_mem_offset;
		if (format == 0x0c)
		{
			VitaVifCopyQwordBurst(dest, data, count);
			vif.tag.addr += bytes;
			vif.cl = static_cast<u8>(count % wl);
			regs.num = 0;
			return true;
		}

#if VITASX2_VIF_HAS_ARM_NEON
		if (format == 0x0d)
		{
			VitaVifStoreV4_16PairBurstNeon(dest, data, count, vif.usn != 0);
#if defined(VITASX2_QEMU_VALIDATION)
			g_qemuVifFastVectors += count;
			g_qemuVifBurstVectors += count;
			g_qemuVifBurstWidenVectors += count;
#endif
			vif.tag.addr += bytes;
			vif.cl = static_cast<u8>(count % wl);
			regs.num = 0;
			return true;
		}
#endif

		for (u32 i = 0; i < count; i++)
		{
			switch (format)
			{
				case 0x0d:
#if VITASX2_VIF_HAS_ARM_NEON
					VitaVifStoreV4_16WordsNeon(dest + i * 16, data + i * 8, vif.usn != 0);
#else
				{
					u32 x, y, z, w;
					VitaVifLoadV4_16Words(data + i * 8, vif.usn != 0, x, y, z, w);
					VitaVifStoreWords(dest + i * 16, x, y, z, w);
				}
#endif
#if defined(VITASX2_QEMU_VALIDATION)
					++g_qemuVifBurstWidenVectors;
#endif
					break;
				default:
#if VITASX2_VIF_HAS_ARM_NEON
					VitaVifStoreV4_8WordsNeon(dest + i * 16, data + i * 4, vif.usn != 0);
#else
				{
					u32 x, y, z, w;
					VitaVifLoadV4_8Words(data + i * 4, vif.usn != 0, x, y, z, w);
					VitaVifStoreWords(dest + i * 16, x, y, z, w);
				}
#endif
#if defined(VITASX2_QEMU_VALIDATION)
					++g_qemuVifBurstWidenVectors;
#endif
					break;
			}
#if defined(VITASX2_QEMU_VALIDATION)
			++g_qemuVifFastVectors;
			++g_qemuVifBurstVectors;
#endif
		}

		vif.tag.addr += bytes;
		vif.cl = static_cast<u8>(count % wl);
		regs.num = 0;
		return true;
	}

	template <int idx>
	bool VitaVifTryFastV4_5Burst(const u8* data, bool isFill)
	{
		// PCSX2 owner: Vif_Unpack.cpp::UNPACK_V4_5(). V4-5 ignores USN/MODE,
		// but row/col mask, fill, skip, and VU-memory wrap still need the
		// per-vector path below.
		vifStruct& vif = GetVifX;
		VIFregisters& regs = vifXRegs;
		const u32 upk_num = static_cast<u32>(vif.cmd & 0x1f);
		const u32 wl = regs.cycle.wl;
		if (isFill || (upk_num & 0x10) != 0 || (upk_num & 0x0f) != 0x0f ||
			regs.cycle.cl != wl || wl == 0 || vif.cl != 0 || regs.num == 0)
		{
			return false;
		}

		const u32 count = regs.num;
		const u32 bytes = count * 16;
		const u32 vu_mem_size = idx ? VU1_MEMSIZE : VU0_MEMSIZE;
		const u32 vu_mem_offset = vif.tag.addr & (idx ? 0x3ff0u : 0xff0u);
		if (vu_mem_offset + bytes > vu_mem_size)
			return false;

		u8* dest = vuRegs[idx].Mem + vu_mem_offset;
		for (u32 i = 0; i < count; i++)
		{
			VitaVifUnpackV4_5Vector(vif, regs, dest + i * 16, data + i * 2, false);
#if defined(VITASX2_QEMU_VALIDATION)
			++g_qemuVifFastVectors;
			++g_qemuVifBurstVectors;
			++g_qemuVifBurstColorVectors;
#endif
		}

		vif.tag.addr += bytes;
		vif.cl = static_cast<u8>(count % wl);
		regs.num = 0;
		return true;
	}

	template <int idx>
	bool VitaVifTryFastV4_32ModeBurst(const u8* data, bool isFill)
	{
		// PCSX2 owners: Vif_Unpack.cpp::UNPACK_V4() and writeXYZW().
		// Contiguous unmasked V4-32 MODE traffic has no lane selectors, so keep
		// the same MaskRow side effects in one NEON loop instead of the generic
		// per-vector mode/mask dispatcher.
		vifStruct& vif = GetVifX;
		VIFregisters& regs = vifXRegs;
		const u32 upk_num = static_cast<u32>(vif.cmd & 0x1f);
		const u32 mode = regs.mode & 0x3;
		const u32 wl = regs.cycle.wl;
		if (isFill || upk_num != 0x0c || mode == 0 || regs.cycle.cl != wl ||
			wl == 0 || vif.cl != 0 || regs.num == 0)
		{
			return false;
		}

		const u32 count = regs.num;
		const u32 bytes = count * 16u;
		const u32 vu_mem_size = idx ? VU1_MEMSIZE : VU0_MEMSIZE;
		const u32 vu_mem_offset = vif.tag.addr & (idx ? 0x3ff0u : 0xff0u);
		if (vu_mem_offset + bytes > vu_mem_size)
			return false;

		u8* dest = vuRegs[idx].Mem + vu_mem_offset;
#if VITASX2_VIF_HAS_ARM_NEON
		uint32x4_t row = vld1q_u32(vif.MaskRow._u32);
		for (u32 i = 0; i < count; i++)
		{
			const uint32x4_t unpacked = vld1q_u32(reinterpret_cast<const u32*>(data + i * 16u));
			uint32x4_t result = unpacked;
			if (mode == 1 || mode == 2)
				result = vaddq_u32(unpacked, row);
			if (mode == 2 || mode == 3)
				row = result;
			VitaVifStoreVectorNeon(dest + i * 16u, result);
#if defined(VITASX2_QEMU_VALIDATION)
			++g_qemuVifFastVectors;
			++g_qemuVifBurstVectors;
			++g_qemuVifBurstV4_32ModeVectors;
#endif
		}
		if (mode == 2 || mode == 3)
			vst1q_u32(vif.MaskRow._u32, row);
#else
		for (u32 i = 0; i < count; i++)
		{
			VitaVifStoreModeWords(vif, regs, dest + i * 16u, mode, false,
				VitaVifLoadU32(data + i * 16u),
				VitaVifLoadU32(data + i * 16u + 4u),
				VitaVifLoadU32(data + i * 16u + 8u),
				VitaVifLoadU32(data + i * 16u + 12u));
#if defined(VITASX2_QEMU_VALIDATION)
			++g_qemuVifFastVectors;
			++g_qemuVifBurstVectors;
			++g_qemuVifBurstV4_32ModeVectors;
#endif
		}
#endif

		vif.tag.addr += bytes;
		vif.cl = static_cast<u8>(count % wl);
		regs.num = 0;
		return true;
	}

	template <int idx>
	bool VitaVifTryFastModeMaskBurst(const u8* data, bool isFill)
	{
		// PCSX2 owners: Vif_Unpack.cpp::writeXYZW(), UNPACK_S(),
		// UNPACK_V2(), UNPACK_V4(), UNPACK_V4_5(), and generated V3 from
		// x86/Vif_Dynarec.cpp::ModUnpack(). This keeps row/col/protect and
		// MODE side effects per-vector while hoisting contiguous loop control.
		vifStruct& vif = GetVifX;
		VIFregisters& regs = vifXRegs;
		const u32 upk_num = static_cast<u32>(vif.cmd & 0x1f);
		const u32 format = upk_num & 0x0f;
		const bool doMask = (upk_num & 0x10) != 0;
		const u32 mode = regs.mode & 0x3;
		const u32 wl = regs.cycle.wl;
		if (isFill || (!doMask && mode == 0) || regs.cycle.cl != wl ||
			wl == 0 || vif.cl != 0 || regs.num == 0)
		{
			return false;
		}
		if (nVifT[format] == 0 || (!VitaVifIsFastVectorFormat(format) && format != 0x0f))
			return false;

		const u32 count = regs.num;
		const u32 bytes = count * 16;
		const u32 vu_mem_size = idx ? VU1_MEMSIZE : VU0_MEMSIZE;
		const u32 vu_mem_offset = vif.tag.addr & (idx ? 0x3ff0u : 0xff0u);
		if (vu_mem_offset + bytes > vu_mem_size)
			return false;

		const u32 vsize = nVifT[format];
		const u32 generated_alignment = VitaVifGeneratedAlignment(vif, format);
		u32 generated_iteration = 0;
		u8* dest = vuRegs[idx].Mem + vu_mem_offset;
		for (u32 i = 0; i < count; i++)
		{
			vif.cl = static_cast<u8>(i % wl);
			if (format == 0x0f)
			{
				VitaVifUnpackV4_5Vector(vif, regs, dest + i * 16, data + i * vsize, doMask);
			}
			else
			{
				u32 vector_iteration = generated_iteration;
				if (format == 0x09 || format == 0x0a)
					vector_iteration = ++generated_iteration;

				if (!VitaVifUnpackVector(
						vif, regs, dest + i * 16, data + i * vsize, format, mode, vif.usn != 0, doMask,
						vector_iteration, generated_alignment))
				{
					return false;
				}

				if (format == 0x08)
					generated_iteration = (generated_iteration + 1) & 0x1u;
			}
#if defined(VITASX2_QEMU_VALIDATION)
			++g_qemuVifFastVectors;
			++g_qemuVifBurstVectors;
			++g_qemuVifBurstModeMaskVectors;
#endif
		}

		vif.tag.addr += bytes;
		vif.cl = static_cast<u8>(count % wl);
		regs.num = 0;
		return true;
	}

	template <int idx>
	bool VitaVifTryFastCycleBurst(const u8* data, bool isFill)
	{
		// PCSX2 owners: Vif_Unpack.cpp::_nVifUnpackLoop(), writeXYZW(),
		// UNPACK_S(), UNPACK_V2(), UNPACK_V4(), UNPACK_V4_5(), and generated
		// V3 from x86/Vif_Dynarec.cpp::ModUnpack(). This path keeps the exact
		// per-vector unpack/write semantics, but hoists fill/skip cycle control
		// when the destination VU memory span does not wrap.
		vifStruct& vif = GetVifX;
		VIFregisters& regs = vifXRegs;
		const u32 upk_num = static_cast<u32>(vif.cmd & 0x1f);
		const u32 format = upk_num & 0x0f;
		const u32 mode = regs.mode & 0x3;
		const bool doMask = (upk_num & 0x10) != 0;
		const u32 wl = regs.cycle.wl ? static_cast<u32>(regs.cycle.wl) : 256u;
		if (vif.cl != 0 || regs.num == 0 || nVifT[format] == 0 ||
			(!VitaVifIsFastVectorFormat(format) && format != 0x0f))
		{
			return false;
		}
		if (!isFill && regs.cycle.cl == wl)
			return false;

		const u32 count = regs.num;
		const u32 vu_mem_size = idx ? VU1_MEMSIZE : VU0_MEMSIZE;
		const u32 vu_mem_offset = vif.tag.addr & (idx ? 0x3ff0u : 0xff0u);
		u32 max_write_end = count * 16u;
		if (!isFill)
		{
			const u32 skip_size = (static_cast<u32>(regs.cycle.cl) - wl) * 16u;
			max_write_end += ((count - 1u) / wl) * skip_size;
		}
		if (vu_mem_offset + max_write_end > vu_mem_size)
			return false;

		const u32 vsize = nVifT[format];
		const u32 generated_alignment = VitaVifGeneratedAlignment(vif, format);
		const u32 skip_size = isFill ? 0u : (static_cast<u32>(regs.cycle.cl) - wl) * 16u;
		u32 generated_iteration = 0;
		u32 cycle = 0;
		u32 dest_offset = 0;
		u8* dest_base = vuRegs[idx].Mem + vu_mem_offset;
		for (u32 i = 0; i < count; i++)
		{
			vif.cl = static_cast<u8>(cycle);
			u32 vector_iteration = generated_iteration;
			if (format == 0x09 || format == 0x0a)
				vector_iteration = ++generated_iteration;

			if (format == 0x0f)
			{
				VitaVifUnpackV4_5Vector(vif, regs, dest_base + dest_offset, data, doMask);
			}
			else if (!VitaVifUnpackVector(
						 vif, regs, dest_base + dest_offset, data, format, mode, vif.usn != 0, doMask,
						 vector_iteration, generated_alignment))
			{
				return false;
			}

			if (format == 0x08)
				generated_iteration = (generated_iteration + 1) & 0x1u;

#if defined(VITASX2_QEMU_VALIDATION)
			++g_qemuVifFastVectors;
			++g_qemuVifBurstVectors;
			++g_qemuVifCycleBurstVectors;
#endif
			dest_offset += 16u;
			--regs.num;
			++cycle;

			if (isFill)
			{
				if (cycle <= regs.cycle.cl)
					data += vsize;
				else if (cycle == wl)
					cycle = 0;
			}
			else
			{
				data += vsize;
				if (cycle >= wl)
				{
					dest_offset += skip_size;
					cycle = 0;
				}
			}
		}

		vif.tag.addr += dest_offset;
		vif.cl = static_cast<u8>(cycle);
		return true;
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
	if (VitaVifTryFastV4Burst<idx>(data, isFill))
		return;

	if (VitaVifTryFastV4_5Burst<idx>(data, isFill))
		return;

	if (VitaVifTryFastSAndV2Burst<idx>(data, isFill))
		return;

	if (VitaVifTryFastV3Burst<idx>(data, isFill))
		return;

	if (VitaVifTryFastV4_32ModeBurst<idx>(data, isFill))
		return;

	if (VitaVifTryFastModeMaskBurst<idx>(data, isFill))
		return;

	if (VitaVifTryFastCycleBurst<idx>(data, isFill))
		return;

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
