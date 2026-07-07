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
extern u32 g_qemuSprCopyNeon128ByteGroups;
extern u32 g_qemuSprCopyNeon256ByteGroups;
extern u32 g_qemuSprCopyNeon1024ByteGroups;
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

#if defined(ARCH_ARM32)
static __forceinline void SprCopy64Bytes(u8* dst, const u8* src)
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

static __forceinline void SprCopy128Bytes(u8* dst, const u8* src)
{
	SprCopy64Bytes(dst, src);
	SprCopy64Bytes(dst + 64, src + 64);
}

static __forceinline void SprCopy256Bytes(u8* dst, const u8* src)
{
	SprCopy128Bytes(dst, src);
	SprCopy128Bytes(dst + 128, src + 128);
}

static __forceinline void SprCopy1024Bytes(u8* dst, const u8* src)
{
	SprCopy256Bytes(dst, src);
	SprCopy256Bytes(dst + 256, src + 256);
	SprCopy256Bytes(dst + 512, src + 512);
	SprCopy256Bytes(dst + 768, src + 768);
}

static __forceinline void SprCountNeonCopy(size_t qwords, size_t groups64, size_t groups128, size_t groups256, size_t groups1024)
{
#if defined(VITASX2_QEMU_VALIDATION)
	g_qemuSprCopyNeonQwords += static_cast<u32>(qwords);
	g_qemuSprCopyNeon64ByteGroups += static_cast<u32>(groups64);
	g_qemuSprCopyNeon128ByteGroups += static_cast<u32>(groups128);
	g_qemuSprCopyNeon256ByteGroups += static_cast<u32>(groups256);
	g_qemuSprCopyNeon1024ByteGroups += static_cast<u32>(groups1024);
#endif
}
#endif

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
	const size_t groups1024 = size >> 10;
	for (size_t i = 0; i < groups1024; i++)
	{
		if ((i + 1) < groups1024)
			__builtin_prefetch(csrc + 1024, 0, 1);

		SprCopy1024Bytes(cdst, csrc);
		csrc += 1024;
		cdst += 1024;
	}

	const size_t remaining_after_1024 = size & 1023;
	const size_t groups256 = remaining_after_1024 >> 8;
	for (size_t i = 0; i < groups256; i++)
	{
		if ((i + 1) < groups256)
			__builtin_prefetch(csrc + 256, 0, 1);

		SprCopy256Bytes(cdst, csrc);
		csrc += 256;
		cdst += 256;
	}

	const size_t remaining_after_256 = remaining_after_1024 & 255;
	const size_t groups128 = remaining_after_256 >> 7;
	for (size_t i = 0; i < groups128; i++)
	{
		if ((i + 1) < groups128)
			__builtin_prefetch(csrc + 128, 0, 1);

		SprCopy128Bytes(cdst, csrc);
		csrc += 128;
		cdst += 128;
	}

	const size_t remaining_after_128 = remaining_after_256 & 127;
	const size_t groups64 = remaining_after_128 >> 6;
	for (size_t i = 0; i < groups64; i++)
	{
		if ((i + 1) < groups64)
			__builtin_prefetch(csrc + 64, 0, 1);

		SprCopy64Bytes(cdst, csrc);
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

	SprCountNeonCopy((groups1024 << 6) + (groups256 << 4) + (groups128 << 3) + (groups64 << 2) + tail_qwords,
		(groups1024 << 4) + (groups256 << 2) + (groups128 << 1) + groups64,
		(groups1024 << 3) + (groups256 << 1) + groups128,
		(groups1024 << 2) + groups256,
		groups1024);
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
