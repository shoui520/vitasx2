// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// This module contains implementations of _aligned_malloc for platforms that don't have
// it built into their CRT/libc.

#if !defined(_WIN32)

#include "common/AlignedMalloc.h"
#include "common/Assertions.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>

void* _aligned_malloc(size_t size, size_t align)
{
	pxAssert(align < 0x10000);
#if defined(__vita__)
	const size_t effective_align = std::max<size_t>(align, sizeof(void*));
	const size_t total_size = size + effective_align - 1 + sizeof(void*);
	void* raw = malloc(total_size);
	if (!raw)
		return nullptr;

	const uintptr_t aligned = (reinterpret_cast<uintptr_t>(raw) + sizeof(void*) + effective_align - 1) & ~(effective_align - 1);
	reinterpret_cast<void**>(aligned)[-1] = raw;
	return reinterpret_cast<void*>(aligned);
#elif defined(__USE_ISOC11) && !defined(ASAN_WORKAROUND) // not supported yet on gcc 4.9
	return aligned_alloc(align, size);
#else
#ifdef __APPLE__
	// MacOS has a bug where posix_memalign is ridiculously slow on unaligned sizes
	// This especially bad on M1s for some reason
	size = (size + align - 1) & ~(align - 1);
#endif
	void* result = 0;
	posix_memalign(&result, align, size);
	return result;
#endif
}

void* pcsx2_aligned_realloc(void* handle, size_t new_size, size_t align, size_t old_size)
{
	pxAssert(align < 0x10000);

	void* newbuf = _aligned_malloc(new_size, align);

	if (newbuf != NULL && handle != NULL)
	{
		memcpy(newbuf, handle, std::min(old_size, new_size));
		_aligned_free(handle);
	}
	return newbuf;
}

__fi void _aligned_free(void* pmem)
{
#if defined(__vita__)
	if (!pmem)
		return;
	free(reinterpret_cast<void**>(pmem)[-1]);
#else
	free(pmem);
#endif
}
#endif
