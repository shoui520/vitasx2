// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Vita host memory/system layer.
//
// The Vita has no user-mode mprotect, no page-fault handler, and no
// multi-mapping of physical pages. Consequently:
//  - MemProtect is a no-op: SMC detection cannot use page protection and must
//    use explicit invalidation checks (the vtlb/JIT side owns that decision).
//  - SharedMemoryMappingArea (PCSX2's fastmem substrate) is unsupported; the
//    core must run with fastmem disabled and use the vtlb table path.
//  - JIT code lives in kernel "VM domain" memblocks; FlushInstructionCache
//    goes through sceKernelSyncVMDomain on the owning block.

#include "common/HostSys.h"
#include "common/AlignedMalloc.h"
#include "common/Assertions.h"
#include "common/Console.h"
#include "common/Error.h"
#include "common/Vita/VitaJitMemory.h"

#include <condition_variable>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <thread>

#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>

namespace VitaVM
{
	struct Block
	{
		SceUID uid;
		size_t size;
	};

	static std::mutex s_blocks_mutex;
	static std::map<uptr, Block> s_blocks; // base address -> block
	static std::mutex s_write_mutex;
	static std::condition_variable s_write_cv;
	static bool s_write_active = false;
	static std::thread::id s_write_owner;

	static bool SyncJitMemoryLocked(void* address, size_t size)
	{
		if (!address || size == 0 || size > std::numeric_limits<SceSize>::max())
			return false;

		std::unique_lock lock(s_blocks_mutex);
		const uptr addr = reinterpret_cast<uptr>(address);
		const uptr end = addr + size;
		if (end < addr)
			return false;

		auto it = s_blocks.upper_bound(addr);
		if (it == s_blocks.begin())
			return false;
		--it;
		const uptr block_end = it->first + it->second.size;
		if (addr < it->first || end > block_end)
			return false;

		const int result = sceKernelSyncVMDomain(
			it->second.uid, address, static_cast<SceSize>(size));
		if (result < 0)
		{
			Console.Error("sceKernelSyncVMDomain(%08x, %p, %zu) failed: %08x",
				it->second.uid, address, size, result);
			return false;
		}
		return true;
	}

	static void FinishJitWrite(std::unique_lock<std::mutex>& lock)
	{
		s_write_active = false;
		s_write_owner = {};
		lock.unlock();
		s_write_cv.notify_one();
	}

	void* AllocJitMemory(size_t size, SceUID* out_uid)
	{
		const size_t aligned_size = (size + 0xFFFFF) & ~static_cast<size_t>(0xFFFFF); // 1 MiB granularity
		const SceUID uid = sceKernelAllocMemBlockForVM("vitasx2_jit", aligned_size);
		if (uid < 0)
		{
			Console.Error("sceKernelAllocMemBlockForVM(%zu) failed: %08x", aligned_size, uid);
			return nullptr;
		}

		void* base = nullptr;
		if (sceKernelGetMemBlockBase(uid, &base) < 0 || !base)
		{
			sceKernelFreeMemBlock(uid);
			return nullptr;
		}

		{
			std::unique_lock lock(s_blocks_mutex);
			s_blocks.emplace(reinterpret_cast<uptr>(base), Block{uid, aligned_size});
		}

		if (out_uid)
			*out_uid = uid;
		return base;
	}

	void FreeJitMemory(void* ptr)
	{
		std::unique_lock lock(s_blocks_mutex);
		const auto it = s_blocks.find(reinterpret_cast<uptr>(ptr));
		if (it == s_blocks.end())
		{
			pxFailRel("FreeJitMemory() called with unknown pointer");
			return;
		}
		sceKernelFreeMemBlock(it->second.uid);
		s_blocks.erase(it);
	}

	bool BeginJitWrite()
	{
		const std::thread::id owner = std::this_thread::get_id();
		std::unique_lock lock(s_write_mutex);
		if (s_write_active && s_write_owner == owner)
		{
			Console.Error("Nested Vita VM-domain code writes are unsupported.");
			return false;
		}

		s_write_cv.wait(lock, [] { return !s_write_active; });
		const int result = sceKernelOpenVMDomain();
		if (result < 0)
		{
			Console.Error("sceKernelOpenVMDomain() failed: %08x", result);
			return false;
		}

		s_write_active = true;
		s_write_owner = owner;
		return true;
	}

	bool EndJitWrite()
	{
		std::unique_lock lock(s_write_mutex);
		if (!s_write_active)
		{
			Console.Error("sceKernelCloseVMDomain() requested without an active code write.");
			return false;
		}

		const int result = sceKernelCloseVMDomain();
		FinishJitWrite(lock);
		if (result < 0)
		{
			Console.Error("sceKernelCloseVMDomain() failed: %08x", result);
			return false;
		}
		return true;
	}

	bool EndJitWriteAndSync(void* address, size_t size)
	{
		std::unique_lock lock(s_write_mutex);
		if (!s_write_active)
		{
			Console.Error("Vita JIT publication requested without an active code write.");
			return false;
		}

		const int close_result = sceKernelCloseVMDomain();
		const bool sync_result = close_result >= 0 && SyncJitMemoryLocked(address, size);
		FinishJitWrite(lock);
		if (close_result < 0)
		{
			Console.Error("sceKernelCloseVMDomain() failed before publication: %08x",
				close_result);
			return false;
		}
		return sync_result;
	}

	bool SyncJitMemory(void* address, size_t size)
	{
		std::unique_lock write_lock(s_write_mutex);
		s_write_cv.wait(write_lock, [] { return !s_write_active; });
		return SyncJitMemoryLocked(address, size);
	}
} // namespace VitaVM

void HostSys::MemProtect(void* baseaddr, size_t size, const PageProtectionMode& mode)
{
	// No user-mode page protection on the Vita. Code that relies on protection
	// for correctness (fastmem, page-fault SMC) must be disabled at the core.
}

std::string HostSys::GetFileMappingName(const char* prefix)
{
	return std::string(prefix);
}

void* HostSys::CreateSharedMemory(const char* name, size_t size)
{
	// No shared-memory objects; return plain page-aligned memory. Multiple
	// views of the same physical pages are not possible on this host.
	return _aligned_malloc(size, __pagesize);
}

void HostSys::DestroySharedMemory(void* ptr)
{
	_aligned_free(ptr);
}

void HostSys::FlushInstructionCache(void* address, u32 size)
{
	pxAssertRel(VitaVM::SyncJitMemory(address, size),
		"FlushInstructionCache() failed or addressed memory outside a VM block");
}

size_t HostSys::GetRuntimePageSize()
{
	return __pagesize;
}

size_t HostSys::GetRuntimeCacheLineSize()
{
	return __cachelinesize;
}

SharedMemoryMappingArea::SharedMemoryMappingArea(u8* base_ptr, size_t size, size_t num_pages)
	: m_base_ptr(base_ptr)
	, m_size(size)
	, m_num_pages(num_pages)
{
}

SharedMemoryMappingArea::~SharedMemoryMappingArea()
{
	pxAssertRel(m_num_mappings == 0, "No mappings left");
	_aligned_free(m_base_ptr);
}

std::unique_ptr<SharedMemoryMappingArea> SharedMemoryMappingArea::Create(size_t size, bool jit)
{
	// There is no address-space reservation or view mapping on the Vita.
	// PCSX2 only uses this for fastmem; the Vita core runs without it.
	Console.Error("SharedMemoryMappingArea::Create(%zu) is unsupported on the Vita.", size);
	return nullptr;
}

u8* SharedMemoryMappingArea::Map(void* file_handle, size_t file_offset, void* map_base, size_t map_size, const PageProtectionMode& mode)
{
	pxFailRel("SharedMemoryMappingArea::Map() is unsupported on the Vita.");
	return nullptr;
}

bool SharedMemoryMappingArea::Unmap(void* map_base, size_t map_size, bool is_file)
{
	pxFailRel("SharedMemoryMappingArea::Unmap() is unsupported on the Vita.");
	return false;
}

namespace PageFaultHandler
{
	bool Install(Error* error)
	{
		// No user-mode fault handling on the Vita; nothing to install. The
		// core must not depend on HandlePageFault() ever firing.
		return true;
	}

	bool InstallSecondaryThread()
	{
		return true;
	}
} // namespace PageFaultHandler

// --------------------------------------------------------------------------
// Spin timing (see common/HostSys.cpp upstream; cpuinfo-free Vita version)
// --------------------------------------------------------------------------

static u32 PAUSE_TIME = 0;

static void MultiPause()
{
	__asm__ __volatile__("yield");
	__asm__ __volatile__("yield");
	__asm__ __volatile__("yield");
	__asm__ __volatile__("yield");
	__asm__ __volatile__("yield");
	__asm__ __volatile__("yield");
	__asm__ __volatile__("yield");
	__asm__ __volatile__("yield");
}

static u32 MeasurePauseTime()
{
	for (int testcnt = 64; true; testcnt *= 2)
	{
		u64 start = GetCPUTicks();
		for (int i = 0; i < testcnt; i++)
		{
			MultiPause();
		}
		u64 time = GetCPUTicks() - start;
		if (time > 100)
		{
			u64 nanos = (time * 1000000000) / GetTickFrequency();
			return (nanos / testcnt) + 1;
		}
	}
}

__noinline static void UpdatePauseTime()
{
	u64 wait = GetCPUTicks() + GetTickFrequency() / 100;
	while (GetCPUTicks() < wait)
		;
	u32 pause = MeasurePauseTime();
	for (int i = 0; i < 4; i++)
		pause = std::min(pause, MeasurePauseTime());
	PAUSE_TIME = pause;
	DevCon.WriteLn("MultiPause time: %uns", pause);
}

u32 ShortSpin()
{
	u32 inc = PAUSE_TIME;
	if (inc == 0) [[unlikely]]
	{
		UpdatePauseTime();
		inc = PAUSE_TIME;
	}

	u32 time = 0;
	for (; time < 500; time += inc)
		MultiPause();

	return time;
}

const u32 SPIN_TIME_NS = 50 * 1000; // 50µs

void AbortWithMessage(const char* msg)
{
	sceClibPrintf("ABORT: %s\n", msg);
	abort();
}

const CPUInfo& GetCPUInfo()
{
	// Fixed hardware; nothing to detect.
	static const CPUInfo info = {
		.name = "ARM Cortex-A9 MPCore (PS Vita)",
		.num_big_cores = 4,
		.num_small_cores = 0,
		.num_threads = 4,
		.num_clusters = 1,
	};
	return info;
}
