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

extern "C"
{
	// VitaSDK's newlib_heapsize_ctrl sample documents a 128 MiB default and
	// explicitly permits applications which own independent memblocks to reduce
	// it.  HostMemoryMap's permanent 65 MiB data arena is now such a memblock.
	// The complete 80 MiB SOTC gate sampled 67,900,600 bytes in use and only
	// 15,985,480 bytes of aggregate headroom. 81 MiB is the smallest whole-MiB
	// ceiling which preserves at least 16 MiB above that observed peak while
	// leaving a separately gated 32 MiB LPDDR reserve.
	unsigned int _newlib_heap_size_user = 81u * 1024u * 1024u;
}

namespace VitaSharedMemory
{
	// Memory.cpp's Vita owner creates exactly one opaque data-map handle; its
	// legacy code-map allocation is deliberately omitted on this target. Retain
	// the kernel identity rather than rediscovering it from a caller-supplied
	// address, which could otherwise name an unrelated cached-RW memblock.
	static std::mutex s_mutex;
	static SceUID s_uid = -1;
	static void* s_base = nullptr;
	static size_t s_size = 0;
} // namespace VitaSharedMemory

namespace VitaVM
{
	// VitaSDK documents a 16 MiB maximum VM-domain allocation.  Daedalus and
	// Play! both use that proven complete-block shape, while Sony's public memory
	// guidance recommends large startup arenas and suballocation.  PCSX2 likewise
	// owns one process code arena and gives each recompiler a fixed slice.  Keep
	// the Vita mapping alive for the process so an IOP/VU cache rewind never asks
	// the kernel for executable memory after code publication has begun.
	static constexpr size_t JIT_ARENA_SIZE = 16 * 1024 * 1024;
	static constexpr size_t JIT_ALLOCATION_GRANULARITY = 1024 * 1024;
	static constexpr size_t JIT_ARENA_SLOT_COUNT =
		JIT_ARENA_SIZE / JIT_ALLOCATION_GRANULARITY;

	struct Block
	{
		SceUID uid;
		size_t size;
	};

	static std::mutex s_blocks_mutex;
	static std::map<uptr, Block> s_blocks; // base address -> block
	static std::map<uptr, size_t> s_allocations; // slice base -> rounded size
	static SceUID s_arena_uid = -1;
	static u8* s_arena_base = nullptr;
	static u32 s_arena_slots = 0;
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

		// The kernel UID covers the permanent 16 MiB arena, but publication must
		// still respect the logical EE/IOP/VU slice lifetime. Otherwise a stale
		// CodeBuffer could synchronize code after its slice had been returned and
		// possibly reassigned to another engine.
		auto allocation_it = s_allocations.upper_bound(addr);
		if (allocation_it == s_allocations.begin())
			return false;
		--allocation_it;
		const uptr allocation_end = allocation_it->first + allocation_it->second;
		if (allocation_end < allocation_it->first || addr < allocation_it->first ||
			end > allocation_end)
		{
			return false;
		}

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
			Console.Error("sceKernelSyncVMDomain(%08x, %p, %u) failed: %08x",
				static_cast<u32>(it->second.uid), address,
				static_cast<u32>(size), static_cast<u32>(result));
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
		if (size == 0 || size > JIT_ARENA_SIZE)
		{
			Console.Error("Vita JIT allocation size is invalid: %u",
				static_cast<u32>(size));
			return nullptr;
		}
		const size_t aligned_size =
			(size + JIT_ALLOCATION_GRANULARITY - 1) &
			~(JIT_ALLOCATION_GRANULARITY - 1);
		const u32 requested_slots = static_cast<u32>(
			aligned_size / JIT_ALLOCATION_GRANULARITY);

		std::unique_lock lock(s_blocks_mutex);
		if (!s_arena_base)
		{
			const SceUID uid =
				sceKernelAllocMemBlockForVM("vitasx2_jit", JIT_ARENA_SIZE);
			if (uid < 0)
			{
				// The Vita-linked std::vsnprintf/newlib formatter does not implement
				// %zu.  Keep both values fixed-width so the actual kernel error cannot
				// be mistaken for the requested size in a hardware log.
				Console.Error("sceKernelAllocMemBlockForVM(%u) failed: %08x",
					static_cast<u32>(JIT_ARENA_SIZE), static_cast<u32>(uid));
				return nullptr;
			}

			void* base = nullptr;
			const int base_result = sceKernelGetMemBlockBase(uid, &base);
			if (base_result < 0 || !base)
			{
				Console.Error("sceKernelGetMemBlockBase(%08x) failed: %08x",
					static_cast<u32>(uid), static_cast<u32>(base_result));
				sceKernelFreeMemBlock(uid);
				return nullptr;
			}

			s_arena_uid = uid;
			s_arena_base = static_cast<u8*>(base);
			s_blocks.emplace(reinterpret_cast<uptr>(base),
				Block{uid, JIT_ARENA_SIZE});
			Console.WriteLn("Vita JIT VM arena: base=%p size=%u uid=%08x",
				base, static_cast<u32>(JIT_ARENA_SIZE), static_cast<u32>(uid));
		}

		u32 first_slot = 0;
		for (; first_slot + requested_slots <= JIT_ARENA_SLOT_COUNT;
			 first_slot++)
		{
			const u32 mask = ((1u << requested_slots) - 1u) << first_slot;
			if ((s_arena_slots & mask) != 0)
				continue;

			s_arena_slots |= mask;
			u8* const allocation =
				s_arena_base + first_slot * JIT_ALLOCATION_GRANULARITY;
			s_allocations.emplace(reinterpret_cast<uptr>(allocation), aligned_size);
			if (out_uid)
				*out_uid = s_arena_uid;
			return allocation;
		}

		Console.Error("Vita JIT VM arena exhausted: request=%u used_slots=%04x",
			static_cast<u32>(aligned_size), s_arena_slots);
		return nullptr;
	}

	void FreeJitMemory(void* ptr)
	{
		std::unique_lock lock(s_blocks_mutex);
		const auto it = s_allocations.find(reinterpret_cast<uptr>(ptr));
		if (it == s_allocations.end())
		{
			pxFailRel("FreeJitMemory() called with unknown pointer");
			return;
		}

		const size_t offset = static_cast<u8*>(ptr) - s_arena_base;
		const u32 first_slot = static_cast<u32>(
			offset / JIT_ALLOCATION_GRANULARITY);
		const u32 slot_count = static_cast<u32>(
			it->second / JIT_ALLOCATION_GRANULARITY);
		const u32 mask = ((1u << slot_count) - 1u) << first_slot;
		s_arena_slots &= ~mask;
		s_allocations.erase(it);
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
			Console.Error("sceKernelOpenVMDomain() failed: %08x",
				static_cast<u32>(result));
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
		if (s_write_owner != std::this_thread::get_id())
		{
			Console.Error("sceKernelCloseVMDomain() requested by a non-owner thread.");
			return false;
		}

		const int result = sceKernelCloseVMDomain();
		FinishJitWrite(lock);
		if (result < 0)
		{
			Console.Error("sceKernelCloseVMDomain() failed: %08x",
				static_cast<u32>(result));
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
		if (s_write_owner != std::this_thread::get_id())
		{
			Console.Error("Vita JIT publication requested by a non-owner thread.");
			return false;
		}

		const int close_result = sceKernelCloseVMDomain();
		const bool sync_result = close_result >= 0 && SyncJitMemoryLocked(address, size);
		FinishJitWrite(lock);
		if (close_result < 0)
		{
			Console.Error("sceKernelCloseVMDomain() failed before publication: %08x",
				static_cast<u32>(close_result));
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
	// PSP2 has no PC-style shared-memory views, but PCSX2's one Vita data-map
	// owner is a permanent 65 MiB arena.  Keep it out of newlib's fixed heap by
	// following Sony PSP2 SDK memblock.h and the official
	// tutorial_shooting_game_trc_compliant/base/heapallocator.cpp lifecycle:
	// cached LPDDR allocation, obtain its base, then free the UID at teardown.
	constexpr size_t LPDDR_ALIGNMENT = 4 * 1024;
	if (size == 0 ||
		size > std::numeric_limits<SceSize>::max() - (LPDDR_ALIGNMENT - 1))
	{
		Console.Error("Invalid Vita shared-memory size: %u",
			static_cast<u32>(size));
		return nullptr;
	}

	const size_t aligned_size =
		(size + LPDDR_ALIGNMENT - 1) & ~(LPDDR_ALIGNMENT - 1);
	std::unique_lock lock(VitaSharedMemory::s_mutex);
	if (VitaSharedMemory::s_base)
	{
		Console.Error("Vita data-map arena already exists: base=%p size=%u",
			VitaSharedMemory::s_base,
			static_cast<u32>(VitaSharedMemory::s_size));
		return nullptr;
	}

	const SceUID uid = sceKernelAllocMemBlock(
		name ? name : "vitasx2_shared", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
		static_cast<SceSize>(aligned_size), nullptr);
	if (uid < 0)
	{
		Console.Error("sceKernelAllocMemBlock(%u) failed: %08x",
			static_cast<u32>(aligned_size), static_cast<u32>(uid));
		return nullptr;
	}

	void* base = nullptr;
	const int base_result = sceKernelGetMemBlockBase(uid, &base);
	if (base_result < 0 || !base)
	{
		Console.Error("sceKernelGetMemBlockBase(%08x) failed: %08x",
			static_cast<u32>(uid), static_cast<u32>(base_result));
		sceKernelFreeMemBlock(uid);
		return nullptr;
	}

	VitaSharedMemory::s_uid = uid;
	VitaSharedMemory::s_base = base;
	VitaSharedMemory::s_size = aligned_size;
	Console.WriteLn("Vita cached-LPDDR arena: base=%p size=%u uid=%08x",
		base, static_cast<u32>(aligned_size), static_cast<u32>(uid));
	return base;
}

void HostSys::DestroySharedMemory(void* ptr)
{
	if (!ptr)
		return;

	std::unique_lock lock(VitaSharedMemory::s_mutex);
	if (ptr != VitaSharedMemory::s_base || VitaSharedMemory::s_uid < 0)
	{
		Console.Error("Refusing unowned Vita shared-memory release: ptr=%p owned=%p",
			ptr, VitaSharedMemory::s_base);
		return;
	}

	const SceUID uid = VitaSharedMemory::s_uid;
	const int result = sceKernelFreeMemBlock(VitaSharedMemory::s_uid);
	if (result < 0)
	{
		Console.Error("sceKernelFreeMemBlock(%08x) failed: %08x",
			static_cast<u32>(uid), static_cast<u32>(result));
		return;
	}

	VitaSharedMemory::s_uid = -1;
	VitaSharedMemory::s_base = nullptr;
	VitaSharedMemory::s_size = 0;
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
	Console.Error("SharedMemoryMappingArea::Create(%u) is unsupported on the Vita.",
		static_cast<u32>(size));
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
