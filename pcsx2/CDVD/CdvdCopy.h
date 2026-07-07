// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#if defined(ARCH_ARM32)
#include <arm_neon.h>
#endif

#include <cstddef>
#include <cstring>

#if defined(VITASX2_QEMU_VALIDATION)
extern u32 g_qemuCdvdBlockCopyNeonQwords;
extern u32 g_qemuCdvdBlockCopyNeon64ByteGroups;
extern u32 g_qemuCdvdBlockCopyNeon128ByteGroups;
extern u32 g_qemuCdvdBlockCopyNeon256ByteGroups;
extern u32 g_qemuCdvdBlockCopyNeon1024ByteGroups;
extern u32 g_qemuCdvdBlockCopyNeon2048ByteGroups;
extern u32 g_qemuCdvdBlockCopyNeon2328ByteGroups;
extern u32 g_qemuCdvdBlockCopyNeon2340ByteGroups;
extern u32 g_qemuCdvdBlockCopyNeon2352ByteGroups;
#endif

#if defined(ARCH_ARM32)
static __forceinline void CdvdCopy64Bytes(u8* dst, const u8* src)
{
	const uint8x16_t qword0 = vld1q_u8(src);
	const uint8x16_t qword1 = vld1q_u8(src + 16);
	const uint8x16_t qword2 = vld1q_u8(src + 32);
	const uint8x16_t qword3 = vld1q_u8(src + 48);
	vst1q_u8(dst, qword0);
	vst1q_u8(dst + 16, qword1);
	vst1q_u8(dst + 32, qword2);
	vst1q_u8(dst + 48, qword3);
}

static __forceinline void CdvdCopy128Bytes(u8* dst, const u8* src)
{
	CdvdCopy64Bytes(dst, src);
	CdvdCopy64Bytes(dst + 64, src + 64);
}

static __forceinline void CdvdCopy256Bytes(u8* dst, const u8* src)
{
	CdvdCopy128Bytes(dst, src);
	CdvdCopy128Bytes(dst + 128, src + 128);
}

static __forceinline void CdvdCopy1024Bytes(u8* dst, const u8* src)
{
	CdvdCopy256Bytes(dst, src);
	CdvdCopy256Bytes(dst + 256, src + 256);
	CdvdCopy256Bytes(dst + 512, src + 512);
	CdvdCopy256Bytes(dst + 768, src + 768);
}

static __forceinline void CdvdCopy2048Bytes(u8* dst, const u8* src)
{
	CdvdCopy1024Bytes(dst, src);
	CdvdCopy1024Bytes(dst + 1024, src + 1024);
}

static __forceinline void CdvdCopy2328Bytes(u8* dst, const u8* src)
{
	CdvdCopy2048Bytes(dst, src);
	CdvdCopy256Bytes(dst + 2048, src + 2048);

	const uint8x16_t tail16 = vld1q_u8(src + 2304);
	const uint8x8_t tail8 = vld1_u8(src + 2320);
	vst1q_u8(dst + 2304, tail16);
	vst1_u8(dst + 2320, tail8);
}

static __forceinline void CdvdCopy2340Bytes(u8* dst, const u8* src)
{
	CdvdCopy2048Bytes(dst, src);
	CdvdCopy256Bytes(dst + 2048, src + 2048);

	const uint8x16_t tail0 = vld1q_u8(src + 2304);
	const uint8x16_t tail1 = vld1q_u8(src + 2320);
	vst1q_u8(dst + 2304, tail0);
	vst1q_u8(dst + 2320, tail1);
	dst[2336] = src[2336];
	dst[2337] = src[2337];
	dst[2338] = src[2338];
	dst[2339] = src[2339];
}

static __forceinline void CdvdCopy2352Bytes(u8* dst, const u8* src)
{
	CdvdCopy2048Bytes(dst, src);
	CdvdCopy256Bytes(dst + 2048, src + 2048);

	const uint8x16_t tail0 = vld1q_u8(src + 2304);
	const uint8x16_t tail1 = vld1q_u8(src + 2320);
	const uint8x16_t tail2 = vld1q_u8(src + 2336);
	vst1q_u8(dst + 2304, tail0);
	vst1q_u8(dst + 2320, tail1);
	vst1q_u8(dst + 2336, tail2);
}

static __forceinline void CdvdCountNeonCopy(
	size_t qwords,
	size_t groups64,
	size_t groups128,
	size_t groups256,
	size_t groups1024,
	size_t groups2048,
	size_t groups2328 = 0,
	size_t groups2340 = 0,
	size_t groups2352 = 0)
{
#if defined(VITASX2_QEMU_VALIDATION)
	g_qemuCdvdBlockCopyNeonQwords += static_cast<u32>(qwords);
	g_qemuCdvdBlockCopyNeon64ByteGroups += static_cast<u32>(groups64);
	g_qemuCdvdBlockCopyNeon128ByteGroups += static_cast<u32>(groups128);
	g_qemuCdvdBlockCopyNeon256ByteGroups += static_cast<u32>(groups256);
	g_qemuCdvdBlockCopyNeon1024ByteGroups += static_cast<u32>(groups1024);
	g_qemuCdvdBlockCopyNeon2048ByteGroups += static_cast<u32>(groups2048);
	g_qemuCdvdBlockCopyNeon2328ByteGroups += static_cast<u32>(groups2328);
	g_qemuCdvdBlockCopyNeon2340ByteGroups += static_cast<u32>(groups2340);
	g_qemuCdvdBlockCopyNeon2352ByteGroups += static_cast<u32>(groups2352);
#endif
}
#endif

static __forceinline void CdvdCopyBytes(void* dst, const void* src, size_t size)
{
#if defined(ARCH_ARM32)
	u8* cdst = static_cast<u8*>(dst);
	const u8* csrc = static_cast<const u8*>(src);

	if (size == 2328)
	{
		CdvdCopy2328Bytes(cdst, csrc);
		CdvdCountNeonCopy(145, 36, 18, 9, 2, 1, 1);
		return;
	}
	if (size == 2340)
	{
		CdvdCopy2340Bytes(cdst, csrc);
		CdvdCountNeonCopy(146, 36, 18, 9, 2, 1, 0, 1);
		return;
	}
	if (size != 0 && (size % 2352) == 0)
	{
		const size_t groups2352 = size / 2352;
		for (size_t i = 0; i < groups2352; i++)
		{
			if ((i + 1) < groups2352)
				__builtin_prefetch(csrc + 2352, 0, 1);

			CdvdCopy2352Bytes(cdst, csrc);
			csrc += 2352;
			cdst += 2352;
		}

		CdvdCountNeonCopy(groups2352 * 147, groups2352 * 36, groups2352 * 18, groups2352 * 9, groups2352 * 2, groups2352, 0, 0, groups2352);
		return;
	}

	const size_t groups2048 = size >> 11;
	for (size_t i = 0; i < groups2048; i++)
	{
		if ((i + 1) < groups2048)
			__builtin_prefetch(csrc + 2048, 0, 1);

		CdvdCopy2048Bytes(cdst, csrc);
		csrc += 2048;
		cdst += 2048;
	}

	const size_t remaining_after_2048 = size & 2047;
	const size_t groups1024 = remaining_after_2048 >> 10;
	for (size_t i = 0; i < groups1024; i++)
	{
		if ((i + 1) < groups1024)
			__builtin_prefetch(csrc + 1024, 0, 1);

		CdvdCopy1024Bytes(cdst, csrc);
		csrc += 1024;
		cdst += 1024;
	}

	const size_t remaining_after_1024 = remaining_after_2048 & 1023;
	const size_t groups256 = remaining_after_1024 >> 8;
	for (size_t i = 0; i < groups256; i++)
	{
		if ((i + 1) < groups256)
			__builtin_prefetch(csrc + 256, 0, 1);

		CdvdCopy256Bytes(cdst, csrc);
		csrc += 256;
		cdst += 256;
	}

	const size_t remaining_after_256 = remaining_after_1024 & 255;
	const size_t groups128 = remaining_after_256 >> 7;
	for (size_t i = 0; i < groups128; i++)
	{
		if ((i + 1) < groups128)
			__builtin_prefetch(csrc + 128, 0, 1);

		CdvdCopy128Bytes(cdst, csrc);
		csrc += 128;
		cdst += 128;
	}

	const size_t remaining_after_128 = remaining_after_256 & 127;
	const size_t groups64 = remaining_after_128 >> 6;
	for (size_t i = 0; i < groups64; i++)
	{
		if ((i + 1) < groups64)
			__builtin_prefetch(csrc + 64, 0, 1);

		CdvdCopy64Bytes(cdst, csrc);
		csrc += 64;
		cdst += 64;
	}

	const size_t tail_bytes = remaining_after_128 & 63;
	const size_t tail_qwords = tail_bytes >> 4;
	for (size_t i = 0; i < tail_qwords; i++)
	{
		const uint8x16_t qword = vld1q_u8(csrc);
		vst1q_u8(cdst, qword);
		csrc += 16;
		cdst += 16;
	}

	if (tail_bytes & 8)
	{
		const uint8x8_t half = vld1_u8(csrc);
		vst1_u8(cdst, half);
		csrc += 8;
		cdst += 8;
	}
	for (size_t i = 0; i < (tail_bytes & 7); i++)
		cdst[i] = csrc[i];

	CdvdCountNeonCopy((groups2048 << 7) + (groups1024 << 6) + (groups256 << 4) + (groups128 << 3) + (groups64 << 2) + tail_qwords,
		(groups2048 << 5) + (groups1024 << 4) + (groups256 << 2) + (groups128 << 1) + groups64,
		(groups2048 << 4) + (groups1024 << 3) + (groups256 << 1) + groups128,
		(groups2048 << 3) + (groups1024 << 2) + groups256,
		(groups2048 << 1) + groups1024,
		groups2048);
	return;
#endif
	std::memcpy(dst, src, size);
}

static __forceinline size_t CdvdCopyBlocks(void* dst, const void* src, size_t size, u32 blocksize, int internalBlockSize)
{
	char* cdst = static_cast<char*>(dst);
	const char* csrc = static_cast<const char*>(src);
	const char* cend = csrc + size;
	if (internalBlockSize)
	{
		for (; csrc < cend; csrc += internalBlockSize, cdst += blocksize)
			CdvdCopyBytes(cdst, csrc, blocksize);

		return cdst - static_cast<char*>(dst);
	}

	CdvdCopyBytes(dst, src, size);
	return size;
}
