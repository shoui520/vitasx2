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
//  - JIT code lives in one process arena. bythos14 kuBridge supplies a larger
//    directly writable executable arena; upstream kuBridge retains an RX
//    publication path, and the official 16 MiB VM-domain arena remains the
//    fail-closed fallback.

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
#include <memory>
#include <mutex>
#include <new>
#include <thread>

#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/vshbridge.h>
#include <kubridge.h>

#ifndef SCE_KERNEL_MEMBLOCK_TYPE_USER_RX
// VitaSDK exposes this kernel memtype only from the kernel header, while
// upstream kuBridge accepts it from user mode. Keep GTA's proven ABI value.
#define SCE_KERNEL_MEMBLOCK_TYPE_USER_RX (0x0C20D050)
#endif

extern "C"
{
	// VitaSDK's newlib_heapsize_ctrl sample documents a 128 MiB default and
	// explicitly permits applications which own independent memblocks to reduce
	// it. HostMemoryMap's permanent 65 MiB data arena is now such a memblock.
	// A matched six-window PES run with a 16 MiB EE cache, 64 KiB Vita
	// translation cache, native-resolution 4 MiB GS scratch, and lazy ShaccCg
	// arena reached 64,833,032 bytes in use and retained 15,907,320 bytes of
	// aggregate heap headroom. The final exact 14 MiB EE / 3 MiB IOP split
	// retained 14,823,144 bytes after an additional fourth 120-VSync window.
	// Keep the 77 MiB ceiling measured by those boundaries; allocator headroom
	// is not a largest-contiguous-allocation guarantee.
	unsigned int _newlib_heap_size_user = 77u * 1024u * 1024u;
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
	// VitaSDK's user VM-domain API is capped at 16 MiB. The selected product
	// layout needs EE 14 MiB + IOP 3 MiB + VU0 1 MiB + VU1 4 MiB, so one
	// kuBridge-backed 22 MiB arena provides exactly those fixed slices. A 24 MiB
	// reservation starved the fourth 2 MiB immutable GPU-VU input slot on real
	// hardware, so executable memory must not carry unused spare capacity.
	// bythos14 kuBridge v0.3.1 adds kuKernelMemProtect, so its arena is made RWX
	// once and receives ordinary cached stores followed by exact cache
	// maintenance. Upstream kuBridge predates that API; GTA's Vita loader proves
	// its supported live-code contract of USER_RX allocation, unrestricted-copy
	// publication, then exact cache maintenance. If kuBridge is absent or
	// rejects the allocation, retain the previous official arena and 8 MiB EE
	// slice.
	//
	// Keep either backing allocation alive for the process. Cache rewind returns
	// its slice without asking the kernel for executable memory after code
	// publication has begun.
	static constexpr size_t KUBRIDGE_JIT_ARENA_SIZE = 22 * 1024 * 1024;
	static constexpr size_t OFFICIAL_JIT_ARENA_SIZE = 16 * 1024 * 1024;
	static constexpr size_t JIT_ALLOCATION_GRANULARITY = 1024 * 1024;
	static constexpr size_t KUBRIDGE_ALLOCATION_GRANULARITY = 4 * 1024;
	static constexpr size_t KUBRIDGE_INITIAL_STAGING_SIZE = 4 * 1024;
	static_assert(KUBRIDGE_JIT_ARENA_SIZE / JIT_ALLOCATION_GRANULARITY < 32);

	enum class BlockBackend : u8
	{
		OfficialVmDomain,
		KuBridgeRxStaging,
		KuBridgeDirectRwx,
	};

	static constexpr u32 UNRESOLVED_IMPORT_STUB_INSTRUCTION_0 = 0xe24fc008;
	static constexpr u32 UNRESOLVED_IMPORT_STUB_INSTRUCTION_1 = 0xe12fff1e;

	struct Block
	{
		SceUID uid;
		size_t size;
		BlockBackend backend;
	};

	struct Allocation
	{
		size_t size;
		BlockBackend backend;
	};

	static std::mutex s_blocks_mutex;
	static std::map<uptr, Block> s_blocks; // base address -> block
	static std::map<uptr, Allocation> s_allocations; // slice base -> ownership
	static SceUID s_arena_uid = -1;
	static u8* s_arena_base = nullptr;
	static size_t s_arena_size = 0;
	static BlockBackend s_arena_backend = BlockBackend::OfficialVmDomain;
	static JitMemoryDiagnostics s_diagnostics;
	static u32 s_arena_slots = 0;
	static std::mutex s_write_mutex;
	static std::condition_variable s_write_cv;
	static bool s_write_active = false;
	static std::thread::id s_write_owner;
	static bool s_write_vm_domain_open = false;
	static BlockBackend s_write_backend = BlockBackend::OfficialVmDomain;
	static u8* s_write_target_base = nullptr;
	static size_t s_write_target_capacity = 0;
	static std::unique_ptr<u8[]> s_write_staging;
	static size_t s_write_staging_capacity = 0;

	static bool IsKuBridgeBackend(BlockBackend backend)
	{
		return backend == BlockBackend::KuBridgeRxStaging ||
			backend == BlockBackend::KuBridgeDirectRwx;
	}

	static JitMemoryBackend ToPublicBackend(BlockBackend backend)
	{
		switch (backend)
		{
			case BlockBackend::OfficialVmDomain:
				return JitMemoryBackend::OfficialVmDomain;
			case BlockBackend::KuBridgeRxStaging:
				return JitMemoryBackend::KuBridgeRxStaging;
			case BlockBackend::KuBridgeDirectRwx:
				return JitMemoryBackend::KuBridgeDirectRwx;
		}
		return JitMemoryBackend::Uninitialized;
	}

	static bool IsWeakImportResolved(const void* function)
	{
		if (!function)
			return false;

		// taiHEN/taihen.c::taiHookFunctionImportForKernel() owns this
		// discriminator: an unresolved weak import retains these first two A32
		// instructions. Do not call through that stub; doing so previously
		// branched to address zero with upstream kuBridge installed.
		const uptr address = reinterpret_cast<uptr>(function) & ~uptr{1};
		u32 instructions[2]{};
		std::memcpy(instructions, reinterpret_cast<const void*>(address),
			sizeof(instructions));
		return instructions[0] != UNRESOLVED_IMPORT_STUB_INSTRUCTION_0 ||
			instructions[1] != UNRESOLVED_IMPORT_STUB_INSTRUCTION_1;
	}

	static bool FindAllocationBackendLocked(void* address, size_t capacity,
		BlockBackend* backend)
	{
		if (!address || capacity == 0 || !backend)
			return false;

		const uptr addr = reinterpret_cast<uptr>(address);
		const uptr end = addr + capacity;
		if (end < addr)
			return false;
		auto allocation_it = s_allocations.upper_bound(addr);
		if (allocation_it == s_allocations.begin())
			return false;
		--allocation_it;
		const uptr allocation_end =
			allocation_it->first + allocation_it->second.size;
		if (allocation_end < allocation_it->first ||
			addr < allocation_it->first || end > allocation_end)
		{
			return false;
		}

		*backend = allocation_it->second.backend;
		return true;
	}

	static bool SyncJitMemoryLocked(void* address, size_t size)
	{
		if (!address || size == 0 || size > std::numeric_limits<SceSize>::max())
			return false;

		std::unique_lock lock(s_blocks_mutex);
		const uptr addr = reinterpret_cast<uptr>(address);
		const uptr end = addr + size;
		if (end < addr)
			return false;

		// The kernel UID covers the permanent arena, but publication must still
		// respect the logical EE/IOP/VU slice lifetime. Otherwise a stale
		// CodeBuffer could synchronize code after its slice had been returned and
		// possibly reassigned to another engine.
		auto allocation_it = s_allocations.upper_bound(addr);
		if (allocation_it == s_allocations.begin())
			return false;
		--allocation_it;
		const uptr allocation_end =
			allocation_it->first + allocation_it->second.size;
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
		if (addr < it->first || end > block_end ||
			allocation_it->second.backend != it->second.backend)
			return false;

		if (IsKuBridgeBackend(it->second.backend))
		{
			kuKernelFlushCaches(address, static_cast<SceSize>(size));
			return true;
		}

		const int result = sceKernelSyncVMDomain(it->second.uid, address,
			static_cast<SceSize>(size));
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
		s_write_vm_domain_open = false;
		s_write_backend = BlockBackend::OfficialVmDomain;
		s_write_target_base = nullptr;
		s_write_target_capacity = 0;
		lock.unlock();
		s_write_cv.notify_one();
	}

	static bool RegisterArenaLocked(SceUID uid, void* base, size_t size,
		BlockBackend backend)
	{
		const uptr address = reinterpret_cast<uptr>(base);
		const auto [it, inserted] =
			s_blocks.emplace(address, Block{uid, size, backend});
		if (!inserted)
		{
			(void)it;
			Console.Error("Vita JIT arena ownership range collided: %p", base);
			return false;
		}

		s_arena_uid = uid;
		s_arena_base = static_cast<u8*>(base);
		s_arena_size = size;
		s_arena_backend = backend;
		s_arena_slots = 0;
		s_diagnostics.backend = ToPublicBackend(backend);
		s_diagnostics.arena_size = size;
		return true;
	}

	static bool TryInitializeKuBridgeArenaLocked()
	{
		s_diagnostics.kubridge_attempted = true;
		int search_unk[2]{};
		const SceUID module_uid =
			_vshKernelSearchModuleByName("kubridge", search_unk);
		s_diagnostics.kubridge_module_result = module_uid;
		if (module_uid < 0)
		{
			Console.Warning(
				"kuBridge is not loaded: %08x; using the official VM arena",
				static_cast<u32>(module_uid));
			return false;
		}

		SceKernelAllocMemBlockKernelOpt opt{};
		opt.size = sizeof(opt);
		const SceUID uid = kuKernelAllocMemBlock(
			"vitasx2_jit", SCE_KERNEL_MEMBLOCK_TYPE_USER_RX,
			static_cast<SceSize>(KUBRIDGE_JIT_ARENA_SIZE), &opt);
		s_diagnostics.kubridge_allocation_result = uid;
		if (uid < 0)
		{
			Console.Warning(
				"kuKernelAllocMemBlock RX(%u) failed: %08x; using the official VM arena",
				static_cast<u32>(KUBRIDGE_JIT_ARENA_SIZE),
				static_cast<u32>(uid));
			return false;
		}

		void* base = nullptr;
		const int base_result = sceKernelGetMemBlockBase(uid, &base);
		s_diagnostics.kubridge_base_result = base_result;
		if (base_result < 0 || !base)
		{
			Console.Error("sceKernelGetMemBlockBase(%08x) failed: %08x",
				static_cast<u32>(uid), static_cast<u32>(base_result));
			sceKernelFreeMemBlock(uid);
			return false;
		}
		if ((reinterpret_cast<uptr>(base) &
				(KUBRIDGE_ALLOCATION_GRANULARITY - 1)) != 0)
		{
			Console.Error("kuBridge JIT base is not page aligned: %p", base);
			sceKernelFreeMemBlock(uid);
			return false;
		}

		BlockBackend backend = BlockBackend::KuBridgeRxStaging;
		int protect_result = 0;
		s_diagnostics.protect_import_resolved = IsWeakImportResolved(
			reinterpret_cast<const void*>(&kuKernelMemProtect));
		if (s_diagnostics.protect_import_resolved)
		{
			protect_result = kuKernelMemProtect(base,
				static_cast<SceSize>(KUBRIDGE_JIT_ARENA_SIZE),
				KU_KERNEL_PROT_READ | KU_KERNEL_PROT_WRITE |
					KU_KERNEL_PROT_EXEC);
			if (protect_result == 0)
				backend = BlockBackend::KuBridgeDirectRwx;
			else
				Console.Warning(
					"kuKernelMemProtect RWX(%p, %u) failed: %08x; using RX staging",
					base, static_cast<u32>(KUBRIDGE_JIT_ARENA_SIZE),
					static_cast<u32>(protect_result));
		}
		s_diagnostics.kubridge_protect_result = protect_result;

		if (!RegisterArenaLocked(uid, base, KUBRIDGE_JIT_ARENA_SIZE,
				backend))
		{
			sceKernelFreeMemBlock(uid);
			return false;
		}

		Console.WriteLn(
			"Vita JIT kuBridge arena: base=%p size=%u uid=%08x module=%08x direct_rwx=%u",
			base, static_cast<u32>(KUBRIDGE_JIT_ARENA_SIZE),
			static_cast<u32>(uid), static_cast<u32>(module_uid),
			backend == BlockBackend::KuBridgeDirectRwx ? 1u : 0u);
		return true;
	}

	static bool InitializeOfficialArenaLocked()
	{
		const SceUID uid = sceKernelAllocMemBlockForVM(
			"vitasx2_jit", OFFICIAL_JIT_ARENA_SIZE);
		if (uid < 0)
		{
			// The Vita-linked std::vsnprintf/newlib formatter does not implement
			// %zu. Keep both values fixed-width so the kernel error cannot be
			// mistaken for the requested size in a hardware log.
			Console.Error("sceKernelAllocMemBlockForVM(%u) failed: %08x",
				static_cast<u32>(OFFICIAL_JIT_ARENA_SIZE),
				static_cast<u32>(uid));
			return false;
		}

		void* base = nullptr;
		const int base_result = sceKernelGetMemBlockBase(uid, &base);
		if (base_result < 0 || !base)
		{
			Console.Error("sceKernelGetMemBlockBase(%08x) failed: %08x",
				static_cast<u32>(uid), static_cast<u32>(base_result));
			sceKernelFreeMemBlock(uid);
			return false;
		}

		if (!RegisterArenaLocked(uid, base, OFFICIAL_JIT_ARENA_SIZE,
				BlockBackend::OfficialVmDomain))
		{
			sceKernelFreeMemBlock(uid);
			return false;
		}

		Console.WriteLn("Vita JIT VM arena: base=%p size=%u uid=%08x",
			base, static_cast<u32>(OFFICIAL_JIT_ARENA_SIZE),
			static_cast<u32>(uid));
		return true;
	}

	static bool EnsureJitArenaLocked()
	{
		if (s_arena_base)
			return true;
		return TryInitializeKuBridgeArenaLocked() ||
			InitializeOfficialArenaLocked();
	}

	static void* AllocateArenaSliceLocked(size_t size, SceUID* out_uid)
	{
		if (size == 0 || size > s_arena_size)
			return nullptr;

		const size_t aligned_size =
			(size + JIT_ALLOCATION_GRANULARITY - 1) &
			~(JIT_ALLOCATION_GRANULARITY - 1);
		const u32 requested_slots = static_cast<u32>(
			aligned_size / JIT_ALLOCATION_GRANULARITY);
		const u32 arena_slot_count = static_cast<u32>(
			s_arena_size / JIT_ALLOCATION_GRANULARITY);
		for (u32 first_slot = 0;
			 first_slot + requested_slots <= arena_slot_count; first_slot++)
		{
			const u32 mask = ((1u << requested_slots) - 1u) << first_slot;
			if ((s_arena_slots & mask) != 0)
				continue;

			s_arena_slots |= mask;
			u8* const allocation =
				s_arena_base + first_slot * JIT_ALLOCATION_GRANULARITY;
			s_allocations.emplace(reinterpret_cast<uptr>(allocation),
				Allocation{aligned_size, s_arena_backend});
			if (out_uid)
				*out_uid = s_arena_uid;
			return allocation;
		}

		Console.Error(
			"Vita JIT arena exhausted: request=%u used_slots=%08x capacity=%u backend=%u",
			static_cast<u32>(aligned_size), s_arena_slots,
			static_cast<u32>(s_arena_size),
			static_cast<u32>(s_arena_backend));
		return nullptr;
	}

	bool ReserveJitMemory()
	{
		std::unique_lock lock(s_blocks_mutex);
		return EnsureJitArenaLocked();
	}

	void* AllocJitMemory(size_t size, SceUID* out_uid)
	{
		if (size == 0 || size > KUBRIDGE_JIT_ARENA_SIZE)
		{
			Console.Error("Vita JIT allocation size is invalid: %u",
				static_cast<u32>(size));
			return nullptr;
		}

		std::unique_lock lock(s_blocks_mutex);
		if (!EnsureJitArenaLocked())
			return nullptr;
		return AllocateArenaSliceLocked(size, out_uid);
	}

	void* AllocLargeJitMemory(size_t size, SceUID* out_uid)
	{
		if (size == 0 || size > KUBRIDGE_JIT_ARENA_SIZE)
		{
			Console.Error("Vita large JIT allocation size is invalid: %u",
				static_cast<u32>(size));
			return nullptr;
		}

		std::unique_lock lock(s_blocks_mutex);
		if (!EnsureJitArenaLocked() ||
			!IsKuBridgeBackend(s_arena_backend))
		{
			return nullptr;
		}
		return AllocateArenaSliceLocked(size, out_uid);
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

		if (!s_arena_base ||
			it->second.backend != s_arena_backend)
		{
			pxFailRel("FreeJitMemory() lost arena ownership");
			return;
		}

		const size_t offset = static_cast<u8*>(ptr) - s_arena_base;
		const u32 first_slot = static_cast<u32>(
			offset / JIT_ALLOCATION_GRANULARITY);
		const u32 slot_count = static_cast<u32>(
			it->second.size / JIT_ALLOCATION_GRANULARITY);
		const u32 mask = ((1u << slot_count) - 1u) << first_slot;
		s_arena_slots &= ~mask;
		s_allocations.erase(it);
	}

	static bool GrowStagingLocked(size_t preserve_size,
		size_t minimum_capacity, size_t maximum_capacity)
	{
		if (preserve_size > s_write_staging_capacity ||
			minimum_capacity > maximum_capacity)
		{
			return false;
		}
		if (minimum_capacity <= s_write_staging_capacity)
			return true;

		size_t new_capacity = std::min(maximum_capacity,
			std::max(KUBRIDGE_INITIAL_STAGING_SIZE,
				s_write_staging_capacity));
		while (new_capacity < minimum_capacity)
		{
			const size_t doubled = new_capacity * 2;
			if (doubled < new_capacity)
				return false;
			new_capacity = std::min(maximum_capacity, doubled);
		}

		std::unique_ptr<u8[]> replacement(new (std::nothrow) u8[new_capacity]);
		if (!replacement)
		{
			Console.Error("Vita JIT staging allocation failed: %u",
				static_cast<u32>(new_capacity));
			return false;
		}
		if (preserve_size != 0 && s_write_staging)
			std::memcpy(replacement.get(), s_write_staging.get(), preserve_size);
		s_write_staging = std::move(replacement);
		s_write_staging_capacity = new_capacity;
		return true;
	}

	static bool BeginJitWriteForAddress(void* address, size_t existing_size,
		size_t capacity, void** write_address, size_t* write_capacity)
	{
		if (!address || existing_size > capacity || !write_address ||
			!write_capacity)
		{
			return false;
		}

		const std::thread::id owner = std::this_thread::get_id();
		std::unique_lock lock(s_write_mutex);
		if (s_write_active && s_write_owner == owner)
		{
			Console.Error("Nested Vita JIT code writes are unsupported.");
			return false;
		}

		s_write_cv.wait(lock, [] { return !s_write_active; });
		BlockBackend backend = BlockBackend::OfficialVmDomain;
		{
			std::unique_lock blocks_lock(s_blocks_mutex);
			if (!FindAllocationBackendLocked(address, capacity, &backend))
			{
				Console.Error(
					"Vita JIT write requested for unowned range: %p/%u",
					address, static_cast<u32>(capacity));
				return false;
			}
		}

		bool vm_domain_open = false;
		void* selected_write_address = address;
		size_t selected_write_capacity = capacity;
		if (backend == BlockBackend::OfficialVmDomain)
		{
			const int result = sceKernelOpenVMDomain();
			if (result < 0)
			{
				Console.Error("sceKernelOpenVMDomain() failed: %08x",
					static_cast<u32>(result));
				return false;
			}
			vm_domain_open = true;
		}
		else if (backend == BlockBackend::KuBridgeRxStaging)
		{
			const size_t initial_capacity = std::min(capacity,
				std::max(existing_size, KUBRIDGE_INITIAL_STAGING_SIZE));
			if (!GrowStagingLocked(0, initial_capacity, capacity))
				return false;
			if (existing_size != 0)
			{
				std::memcpy(s_write_staging.get(), address, existing_size);
			}
			selected_write_address = s_write_staging.get();
			selected_write_capacity =
				std::min(s_write_staging_capacity, capacity);
		}

		s_write_active = true;
		s_write_owner = owner;
		s_write_vm_domain_open = vm_domain_open;
		s_write_backend = backend;
		s_write_target_base = static_cast<u8*>(address);
		s_write_target_capacity = capacity;
		*write_address = selected_write_address;
		*write_capacity = selected_write_capacity;
		return true;
	}

	bool BeginJitWrite()
	{
		const std::thread::id owner = std::this_thread::get_id();
		std::unique_lock lock(s_write_mutex);
		if (s_write_active && s_write_owner == owner)
		{
			Console.Error("Nested Vita JIT code writes are unsupported.");
			return false;
		}
		s_write_cv.wait(lock, [] { return !s_write_active; });
		const int result = sceKernelOpenVMDomain();
		if (result < 0)
			return false;
		s_write_active = true;
		s_write_owner = owner;
		s_write_vm_domain_open = true;
		s_write_backend = BlockBackend::OfficialVmDomain;
		return true;
	}

	bool BeginJitWrite(void* address, size_t existing_size, size_t capacity,
		void** write_address, size_t* write_capacity)
	{
		return BeginJitWriteForAddress(address, existing_size, capacity,
			write_address, write_capacity);
	}

	bool ExpandJitWrite(size_t preserve_size, size_t minimum_capacity,
		void** write_address, size_t* write_capacity)
	{
		if (!write_address || !write_capacity)
			return false;
		std::unique_lock lock(s_write_mutex);
		if (!s_write_active ||
			s_write_owner != std::this_thread::get_id())
		{
			Console.Error(
				"Vita JIT staging growth requested without an owned write.");
			return false;
		}
		if (s_write_backend != BlockBackend::KuBridgeRxStaging)
			return minimum_capacity <= *write_capacity;
		if (!GrowStagingLocked(preserve_size, minimum_capacity,
				s_write_target_capacity))
		{
			return false;
		}
		*write_address = s_write_staging.get();
		*write_capacity =
			std::min(s_write_staging_capacity, s_write_target_capacity);
		return true;
	}

	bool EndJitWrite()
	{
		std::unique_lock lock(s_write_mutex);
		if (!s_write_active)
		{
			Console.Error("Vita JIT write end requested without an active code write.");
			return false;
		}
		if (s_write_owner != std::this_thread::get_id())
		{
			Console.Error("Vita JIT write end requested by a non-owner thread.");
			return false;
		}

		const int result = s_write_vm_domain_open ?
			sceKernelCloseVMDomain() : 0;
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

		const int close_result = s_write_vm_domain_open ?
			sceKernelCloseVMDomain() : 0;
		bool sync_result = false;
		if (close_result >= 0 &&
			s_write_backend == BlockBackend::KuBridgeRxStaging)
		{
			const uptr target = reinterpret_cast<uptr>(address);
			const uptr write_base =
				reinterpret_cast<uptr>(s_write_target_base);
			const uptr write_end = write_base + s_write_target_capacity;
			const uptr target_end = target + size;
			if (write_end >= write_base && target_end >= target &&
				target >= write_base && target_end <= write_end)
			{
				const size_t offset = target - write_base;
				if (offset <= s_write_staging_capacity &&
					size <= s_write_staging_capacity - offset)
				{
					const int copy_result = kuKernelCpuUnrestrictedMemcpy(
						address, s_write_staging.get() + offset,
						static_cast<SceSize>(size));
					if (copy_result < 0)
					{
						Console.Error(
							"kuKernelCpuUnrestrictedMemcpy(%p, %u) failed: %08x",
							address, static_cast<u32>(size),
							static_cast<u32>(copy_result));
					}
					else
					{
						sync_result = SyncJitMemoryLocked(address, size);
					}
				}
			}
		}
		else if (close_result >= 0)
		{
			sync_result = SyncJitMemoryLocked(address, size);
		}
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

	JitMemoryDiagnostics GetJitMemoryDiagnostics()
	{
		std::unique_lock lock(s_blocks_mutex);
		return s_diagnostics;
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
