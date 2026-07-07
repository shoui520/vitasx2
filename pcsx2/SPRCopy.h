// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#if defined(ARCH_ARM32)
#include <arm_neon.h>
#endif

#include <cstddef>
#include <cstdint>
#include <cstring>

static constexpr size_t SPR_SCRATCH_BYTES = 16 * 1024;

#if defined(VITASX2_QEMU_VALIDATION)
extern u32 g_qemuSprCopyNeonQwords;
extern u32 g_qemuSprCopyNeon64ByteGroups;
extern u32 g_qemuSprCopyToScratchCalls;
extern u32 g_qemuSprCopyFromScratchCalls;
extern u32 g_qemuSprCopyWrappedToScratch;
extern u32 g_qemuSprCopyWrappedFromScratch;
extern u32 g_qemuSprCopyOverlapFallbacks;
#endif

static __forceinline bool SprCopyRangesOverlap(const void* dst, const void* src, size_t size)
{
	const std::uintptr_t d = reinterpret_cast<std::uintptr_t>(dst);
	const std::uintptr_t s = reinterpret_cast<std::uintptr_t>(src);
	return size != 0 && d < (s + size) && s < (d + size);
}

static __forceinline void SprCopyBytes(void* dst, const void* src, size_t size)
{
	if (size == 0)
		return;

#if defined(ARCH_ARM32)
	if (SprCopyRangesOverlap(dst, src, size))
	{
		std::memcpy(dst, src, size);
#if defined(VITASX2_QEMU_VALIDATION)
		++g_qemuSprCopyOverlapFallbacks;
#endif
		return;
	}

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
	g_qemuSprCopyNeonQwords += static_cast<u32>((groups64 << 2) + tail_qwords);
	g_qemuSprCopyNeon64ByteGroups += static_cast<u32>(groups64);
#endif
	return;
#endif

	std::memcpy(dst, src, size);
}

static __forceinline void SprCopyToScratch(u8* scratch, u32 dst, const void* src, size_t size)
{
	dst &= static_cast<u32>(SPR_SCRATCH_BYTES - 1);
	const u8* csrc = static_cast<const u8*>(src);

#if defined(VITASX2_QEMU_VALIDATION)
	++g_qemuSprCopyToScratchCalls;
#endif

	if (dst + size >= SPR_SCRATCH_BYTES)
	{
		const size_t end = SPR_SCRATCH_BYTES - dst;
		SprCopyBytes(scratch + dst, csrc, end);
		SprCopyBytes(scratch, csrc + end, size - end);
#if defined(VITASX2_QEMU_VALIDATION)
		++g_qemuSprCopyWrappedToScratch;
#endif
	}
	else
	{
		SprCopyBytes(scratch + dst, csrc, size);
	}
}

static __forceinline void SprCopyFromScratch(void* dst, const u8* scratch, u32 src, size_t size)
{
	src &= static_cast<u32>(SPR_SCRATCH_BYTES - 1);
	u8* cdst = static_cast<u8*>(dst);

#if defined(VITASX2_QEMU_VALIDATION)
	++g_qemuSprCopyFromScratchCalls;
#endif

	if (src + size >= SPR_SCRATCH_BYTES)
	{
		const size_t end = SPR_SCRATCH_BYTES - src;
		SprCopyBytes(cdst, scratch + src, end);
		SprCopyBytes(cdst + end, scratch, size - end);
#if defined(VITASX2_QEMU_VALIDATION)
		++g_qemuSprCopyWrappedFromScratch;
#endif
	}
	else
	{
		SprCopyBytes(cdst, scratch + src, size);
	}
}
