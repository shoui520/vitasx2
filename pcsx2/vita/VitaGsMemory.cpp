// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/GSExtra.h"

#include "common/AlignedMalloc.h"
#include "common/Assertions.h"

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

void* GSAllocateWrappedMemory(size_t size, size_t repeat)
{
	return _aligned_malloc(size * repeat, __pagesize);
}

void GSFreeWrappedMemory(void* ptr, size_t size, size_t repeat)
{
	(void)size;
	(void)repeat;
	_aligned_free(ptr);
}

#endif
