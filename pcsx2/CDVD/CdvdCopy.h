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
extern u32 g_qemuCdvdBlockCopyNeon256ByteGroups;
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

static __forceinline void CdvdCopy256Bytes(u8* dst, const u8* src)
{
	CdvdCopy64Bytes(dst, src);
	CdvdCopy64Bytes(dst + 64, src + 64);
	CdvdCopy64Bytes(dst + 128, src + 128);
	CdvdCopy64Bytes(dst + 192, src + 192);
}

static __forceinline void CdvdCountNeonCopy(size_t qwords, size_t groups64, size_t groups256)
{
#if defined(VITASX2_QEMU_VALIDATION)
	g_qemuCdvdBlockCopyNeonQwords += static_cast<u32>(qwords);
	g_qemuCdvdBlockCopyNeon64ByteGroups += static_cast<u32>(groups64);
	g_qemuCdvdBlockCopyNeon256ByteGroups += static_cast<u32>(groups256);
#endif
}
#endif

static __forceinline void CdvdCopyBytes(void* dst, const void* src, size_t size)
{
#if defined(ARCH_ARM32)
	u8* cdst = static_cast<u8*>(dst);
	const u8* csrc = static_cast<const u8*>(src);
	const size_t groups256 = size >> 8;
	for (size_t i = 0; i < groups256; i++)
	{
		if ((i + 1) < groups256)
			__builtin_prefetch(csrc + 256, 0, 1);

		CdvdCopy256Bytes(cdst, csrc);
		csrc += 256;
		cdst += 256;
	}

	const size_t remaining_after_256 = size & 255;
	const size_t groups64 = remaining_after_256 >> 6;
	for (size_t i = 0; i < groups64; i++)
	{
		if ((i + 1) < groups64)
			__builtin_prefetch(csrc + 64, 0, 1);

		CdvdCopy64Bytes(cdst, csrc);
		csrc += 64;
		cdst += 64;
	}

	const size_t tail_bytes = remaining_after_256 & 63;
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

	CdvdCountNeonCopy((groups256 << 4) + (groups64 << 2) + tail_qwords,
		(groups256 << 2) + groups64,
		groups256);
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
