// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/GSExtra.h"

#include "common/AlignedMalloc.h"
#include "common/Assertions.h"
#include "common/Console.h"

#if defined(VITASX2_QEMU_VALIDATION)

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static int s_vita_gs_shm_fd = -1;

void* GSAllocateWrappedMemory(size_t size, size_t repeat)
{
	pxAssert(s_vita_gs_shm_fd == -1);

	const char* file_name = "/vitasx2-gs.mem";
	s_vita_gs_shm_fd = shm_open(file_name, O_RDWR | O_CREAT | O_EXCL, 0600);
	if (s_vita_gs_shm_fd != -1)
	{
		shm_unlink(file_name);
	}
	else
	{
		std::fprintf(stderr, "Failed to open %s due to %s\n", file_name, std::strerror(errno));
		return nullptr;
	}

	if (ftruncate(s_vita_gs_shm_fd, repeat * size) < 0)
		std::fprintf(stderr, "Failed to reserve GS memory due to %s\n", std::strerror(errno));

	void* fifo = mmap(nullptr, size * repeat, PROT_READ | PROT_WRITE, MAP_SHARED, s_vita_gs_shm_fd, 0);
	if (fifo == MAP_FAILED)
	{
		std::fprintf(stderr, "Failed to mmap GS memory due to %s\n", std::strerror(errno));
		close(s_vita_gs_shm_fd);
		s_vita_gs_shm_fd = -1;
		return nullptr;
	}

	for (size_t i = 1; i < repeat; i++)
	{
		void* base = static_cast<u8*>(fifo) + size * i;
		u8* next = static_cast<u8*>(mmap(base, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, s_vita_gs_shm_fd, 0));
		if (next != base)
			std::fprintf(stderr, "Failed to mmap contiguous GS segment\n");
	}

	return fifo;
}

void GSFreeWrappedMemory(void* ptr, size_t size, size_t repeat)
{
	pxAssert(s_vita_gs_shm_fd >= 0);

	if (s_vita_gs_shm_fd < 0)
		return;

	munmap(ptr, size * repeat);
	close(s_vita_gs_shm_fd);
	s_vita_gs_shm_fd = -1;
}

#else

#include <limits>

#include <psp2/kernel/sysmem.h>

static SceUID s_vita_gs_memblock = -1;
static void* s_vita_gs_memblock_base = nullptr;

void* GSAllocateWrappedMemory(size_t size, size_t repeat)
{
	pxAssertRel(s_vita_gs_memblock < 0, "GS memory has no existing Vita memblock");
	if (s_vita_gs_memblock >= 0 || size == 0 || repeat == 0 ||
		size > (std::numeric_limits<size_t>::max() / repeat))
	{
		return nullptr;
	}

	// PCSX2 owns the repeated-storage contract in GS.cpp.  The documented PSP2
	// user API exposes no operation for aliasing one memblock at several virtual
	// addresses, so the known Vita fallback still reserves every repeat
	// contiguously.  Allocate that large, long-lived object directly from cached
	// LPDDR instead of fragmenting newlib's heap.
	// Sony's sysmem contract requires an LPDDR memblock size rounded to 4 KiB.
	constexpr size_t MEMBLOCK_ALIGNMENT = 4096;
	const size_t requested_size = size * repeat;
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
