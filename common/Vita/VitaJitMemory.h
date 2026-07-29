// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <cstddef>
#include <cstdint>

#if defined(VITASX2_QEMU_VALIDATION)
using SceUID = int;
#else
#include <psp2common/types.h>
#endif

namespace VitaVM
{
	enum class JitMemoryBackend : uint8_t
	{
		Uninitialized,
		OfficialVmDomain,
		KuBridgeRxStaging,
		KuBridgeDirectRwx,
	};

	struct JitMemoryDiagnostics
	{
		JitMemoryBackend backend = JitMemoryBackend::Uninitialized;
		bool kubridge_attempted = false;
		bool protect_import_resolved = false;
		int kubridge_module_result = 0;
		int kubridge_allocation_result = 0;
		int kubridge_base_result = 0;
		int kubridge_protect_result = 0;
		size_t arena_size = 0;
	};

	// Reserve the process-lifetime executable arena before large data/GXM
	// memblocks fragment LPDDR. No logical EE/IOP/VU slice is consumed.
	bool ReserveJitMemory();
	void* AllocJitMemory(size_t size, SceUID* out_uid = nullptr);
	// Large code-cache requests are admitted only by the kuBridge-backed arena.
	// This lets EE select 14 MiB while IOP/VU share the same 22 MiB backing
	// block. The caller can retry AllocJitMemory() to use the official 16 MiB
	// VM-domain fallback. Native validation has no PSP2 permission split.
#if defined(VITASX2_QEMU_VALIDATION)
	inline void* AllocLargeJitMemory(size_t size, SceUID* out_uid = nullptr)
	{
		return AllocJitMemory(size, out_uid);
	}
#else
	void* AllocLargeJitMemory(size_t size, SceUID* out_uid = nullptr);
#endif
	void FreeJitMemory(void* ptr);
	bool BeginJitWrite();
#if defined(VITASX2_QEMU_VALIDATION)
	inline bool BeginJitWrite(void* address, size_t existing_size,
		size_t capacity, void** write_address, size_t* write_capacity)
	{
		if (!address || existing_size > capacity || !write_address ||
			!write_capacity || !BeginJitWrite())
		{
			return false;
		}
		*write_address = address;
		*write_capacity = capacity;
		return true;
	}
	inline bool ExpandJitWrite(size_t preserve_size, size_t minimum_capacity,
		void**, size_t* write_capacity)
	{
		return write_capacity && preserve_size <= *write_capacity &&
			minimum_capacity <= *write_capacity;
	}
#else
	// The official VM and bythos14 direct-RWX backends return address itself.
	// Upstream kuBridge returns a process-global staging range which
	// EndJitWriteAndSync() publishes into its RX allocation through the
	// unrestricted-copy syscall.
	bool BeginJitWrite(void* address, size_t existing_size, size_t capacity,
		void** write_address, size_t* write_capacity);
	bool ExpandJitWrite(size_t preserve_size, size_t minimum_capacity,
		void** write_address, size_t* write_capacity);
#endif
	bool EndJitWrite();
	bool EndJitWriteAndSync(void* address, size_t size);
	bool SyncJitMemory(void* address, size_t size);
#if !defined(VITASX2_QEMU_VALIDATION)
	JitMemoryDiagnostics GetJitMemoryDiagnostics();
#endif
} // namespace VitaVM
