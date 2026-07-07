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
#endif

static __forceinline void CdvdCopyBytes(void* dst, const void* src, size_t size)
{
#if defined(ARCH_ARM32)
	u8* cdst = static_cast<u8*>(dst);
	const u8* csrc = static_cast<const u8*>(src);
	const size_t groups64 = size >> 6;
	for (size_t i = 0; i < groups64; i++)
	{
		if ((i + 1) < groups64)
			__builtin_prefetch(csrc + 64, 0, 1);

		const uint8x16_t qword0 = vld1q_u8(csrc);
		const uint8x16_t qword1 = vld1q_u8(csrc + 16);
		const uint8x16_t qword2 = vld1q_u8(csrc + 32);
		const uint8x16_t qword3 = vld1q_u8(csrc + 48);
		vst1q_u8(cdst, qword0);
		vst1q_u8(cdst + 16, qword1);
		vst1q_u8(cdst + 32, qword2);
		vst1q_u8(cdst + 48, qword3);
		csrc += 64;
		cdst += 64;
	}

	const size_t tail_bytes = size & 63;
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

#if defined(VITASX2_QEMU_VALIDATION)
	g_qemuCdvdBlockCopyNeonQwords += static_cast<u32>((groups64 << 2) + tail_qwords);
	g_qemuCdvdBlockCopyNeon64ByteGroups += static_cast<u32>(groups64);
#endif
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
