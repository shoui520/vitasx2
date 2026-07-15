// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "vita/VitaGsMemory.h"

#include "GS/GSExtra.h"

#include "common/AlignedMalloc.h"
#include "common/Assertions.h"
#include "common/Console.h"

namespace
{
	void FillPsmct32Rows(u32* vm, const GSOffset& offset,
		const GSVector4i& rect, u32 color)
	{
		if (rect.rempty())
			return;

		for (int y = rect.top; y < rect.bottom; y++)
		{
			const GSOffset::PAHelper pa = offset.paMulti(0, y);
			for (int x = rect.left; x < rect.right; x++)
				vm[pa.value(x)] = color;
		}
	}
}

void VitaGS::FillPsmct32Rect(GSLocalMemory& memory, const GSOffset& offset,
	const GSVector4i& rect, u32 color)
{
	pxAssert(offset.psm() == PSMCT32);
	if (rect.rempty())
		return;

	// PCSX2 owner: GS/Renderers/SW/GSDrawScanline.cpp free functions
	// DrawRectT()/FillRect()/FillBlock(). Split partial 8x8 blocks into edge
	// rows, then fill each complete PSMCT32 block with 16 vector stores.
	// GSRendererHW::ClearGSLocalMemory() owns the same constant-write
	// optimization for page-aligned hardware clears.
	const GSOffset psmct32_offset =
		offset.assertSizesMatch(GSLocalMemory::swizzle32);
	const GSVector4i block_rect =
		rect.ralign<Align_Inside>(GSVector2i(8, 8));
	u32* const vm = memory.vm32();

	if (block_rect.rempty())
	{
		FillPsmct32Rows(vm, psmct32_offset, rect, color);
		return;
	}

	FillPsmct32Rows(vm, psmct32_offset,
		GSVector4i(rect.left, rect.top, rect.right, block_rect.top), color);
	FillPsmct32Rows(vm, psmct32_offset,
		GSVector4i(rect.left, block_rect.bottom, rect.right, rect.bottom), color);
	FillPsmct32Rows(vm, psmct32_offset,
		GSVector4i(rect.left, block_rect.top, block_rect.left, block_rect.bottom),
		color);
	FillPsmct32Rows(vm, psmct32_offset,
		GSVector4i(block_rect.right, block_rect.top, rect.right, block_rect.bottom),
		color);

	const GSVector4i vector_color(static_cast<int>(color));
	for (int y = block_rect.top; y < block_rect.bottom; y += 8)
	{
		const GSOffset::PAHelper pa = psmct32_offset.paMulti(0, y);
		for (int x = block_rect.left; x < block_rect.right; x += 8)
		{
			GSVector4i* const block =
				reinterpret_cast<GSVector4i*>(&vm[pa.value(x)]);
			for (int i = 0; i < 16; i += 4)
			{
				block[i + 0] = vector_color;
				block[i + 1] = vector_color;
				block[i + 2] = vector_color;
				block[i + 3] = vector_color;
			}
		}
	}
}

#if defined(VITASX2_QEMU_VALIDATION)

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/mman.h>

static void* s_vita_gs_mapping = nullptr;
static size_t s_vita_gs_mapping_size = 0;

void* GSAllocateWrappedMemory(size_t size, size_t repeat)
{
	pxAssert(s_vita_gs_mapping == nullptr);
	if (s_vita_gs_mapping || size == 0 || repeat == 0)
		return nullptr;

	// Vita folds every PAHelper address into the canonical 4 MiB GS ring and
	// splits the few raw spans which can cross its seam. Keep QEMU on that same
	// representation so an accidental dependency on PCSX2's repeated mappings
	// cannot hide behind the Linux validation environment.
	s_vita_gs_mapping = mmap(nullptr, size, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (s_vita_gs_mapping == MAP_FAILED)
	{
		std::fprintf(stderr, "Failed to map canonical GS memory due to %s\n",
			std::strerror(errno));
		s_vita_gs_mapping = nullptr;
		return nullptr;
	}
	s_vita_gs_mapping_size = size;
	return s_vita_gs_mapping;
}

void GSFreeWrappedMemory(void* ptr, size_t size, size_t repeat)
{
	pxAssert(ptr == s_vita_gs_mapping);
	pxAssert(size == s_vita_gs_mapping_size);
	(void)repeat;
	if (!s_vita_gs_mapping)
		return;

	munmap(s_vita_gs_mapping, s_vita_gs_mapping_size);
	s_vita_gs_mapping = nullptr;
	s_vita_gs_mapping_size = 0;
}

#else

#include <limits>

#include <psp2/kernel/sysmem.h>

static SceUID s_vita_gs_memblock = -1;
static void* s_vita_gs_memblock_base = nullptr;

void* GSAllocateWrappedMemory(size_t size, size_t repeat)
{
	pxAssertRel(s_vita_gs_memblock < 0, "GS memory has no existing Vita memblock");
	if (s_vita_gs_memblock >= 0 || size == 0 || repeat == 0)
	{
		return nullptr;
	}

	// PCSX2 owns the wrapped-storage contract in GS.cpp by repeating one mapping.
	// The documented PSP2 user API exposes no fixed-alias operation. Vita instead
	// keeps one canonical 4 MiB ring: GSOffset::PAHelper folds scalar and image-
	// transfer addresses, while CLUT stages its rare raw seam crossings. Allocate
	// that long-lived store directly from cached LPDDR instead of newlib's heap.
	// Sony's sysmem contract requires an LPDDR memblock size rounded to 4 KiB.
	constexpr size_t MEMBLOCK_ALIGNMENT = 4096;
	const size_t requested_size = size;
	if (requested_size > (std::numeric_limits<SceSize>::max() - (MEMBLOCK_ALIGNMENT - 1)))
		return nullptr;
	const SceSize allocation_size = static_cast<SceSize>(
		(requested_size + (MEMBLOCK_ALIGNMENT - 1)) & ~(MEMBLOCK_ALIGNMENT - 1));

	s_vita_gs_memblock = sceKernelAllocMemBlock("vitasx2_gs_local",
		SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, allocation_size, nullptr);
	if (s_vita_gs_memblock < 0)
	{
		Console.Error("sceKernelAllocMemBlock(GS, %u) failed: %08x",
			allocation_size, s_vita_gs_memblock);
		return nullptr;
	}

	const int result = sceKernelGetMemBlockBase(s_vita_gs_memblock, &s_vita_gs_memblock_base);
	if (result < 0 || !s_vita_gs_memblock_base)
	{
		Console.Error("sceKernelGetMemBlockBase(GS) failed: %08x", result);
		const int free_result = sceKernelFreeMemBlock(s_vita_gs_memblock);
		if (free_result < 0)
		{
			Console.Error("sceKernelFreeMemBlock(GS cleanup) failed: %08x", free_result);
			s_vita_gs_memblock_base = nullptr;
			return nullptr;
		}
		s_vita_gs_memblock = -1;
		s_vita_gs_memblock_base = nullptr;
		return nullptr;
	}

	return s_vita_gs_memblock_base;
}

void GSFreeWrappedMemory(void* ptr, size_t size, size_t repeat)
{
	pxAssertRel(s_vita_gs_memblock >= 0, "GS memory has a Vita memblock");
	pxAssertRel(ptr == s_vita_gs_memblock_base, "GS memory owns the Vita memblock base");
	(void)size;
	(void)repeat;
	if (s_vita_gs_memblock >= 0)
	{
		const int result = sceKernelFreeMemBlock(s_vita_gs_memblock);
		if (result < 0)
		{
			Console.Error("sceKernelFreeMemBlock(GS) failed: %08x", result);
			return;
		}
	}
	s_vita_gs_memblock = -1;
	s_vita_gs_memblock_base = nullptr;
}

#endif
