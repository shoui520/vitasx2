// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#if !defined(VITASX2_QEMU_VALIDATION)

#include "VitaGxmMemory.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace VitaGXM
{
	class Arena;

	enum class ArenaMemory : std::uint8_t
	{
		MainNonCached,
		Cdram,
	};

	// A move-only suballocation. The arena must outlive every allocation made
	// from it. Releasing the handle returns its exact byte range to the owning
	// slab and coalesces adjacent free ranges.
	class ArenaAllocation final
	{
	public:
		ArenaAllocation() = default;
		ArenaAllocation(const ArenaAllocation&) = delete;
		ArenaAllocation& operator=(const ArenaAllocation&) = delete;
		ArenaAllocation(ArenaAllocation&& other) noexcept;
		ArenaAllocation& operator=(ArenaAllocation&& other) noexcept;
		~ArenaAllocation();

		explicit operator bool() const { return m_arena != nullptr; }
		void* Data() const { return m_data; }
		std::size_t Size() const { return m_size; }
		std::size_t Offset() const { return m_offset; }

		void Reset();

	private:
		friend class Arena;

		ArenaAllocation(Arena* arena, void* data, std::size_t size,
			std::size_t offset, std::uint32_t slab_index,
			std::uint64_t token);
		void Clear();

		Arena* m_arena = nullptr;
		void* m_data = nullptr;
		std::size_t m_size = 0;
		std::size_t m_offset = 0;
		std::uint32_t m_slab_index = 0;
		std::uint64_t m_token = 0;
	};

	// Single-thread-owned slab allocator for memory registered with libGXM.
	// Kernel memblocks are intentionally coarse (especially CDRAM's 256 KiB
	// granularity); individual textures and staging regions are suballocated.
	class Arena final
	{
	public:
		struct Statistics
		{
			std::size_t mapped_capacity = 0;
			std::size_t logical_capacity = 0;
			std::size_t fetch_guard_bytes = 0;
			std::size_t allocated_bytes = 0;
			std::size_t peak_allocated_bytes = 0;
			std::size_t free_bytes = 0;
			std::size_t largest_free_range = 0;
			std::size_t live_allocations = 0;
			std::size_t slab_count = 0;
		};

		Arena() = default;
		Arena(const Arena&) = delete;
		Arena& operator=(const Arena&) = delete;
		~Arena();

		// Slabs are allocated lazily. preferred_slab_size is a growth policy,
		// not an accounting approximation; Statistics reports the rounded kernel
		// capacity and exact live suballocation bytes separately.
		int Initialize(ArenaMemory memory, std::string_view name,
			std::size_t preferred_slab_size,
			SceGxmMemoryAttribFlags attributes = SCE_GXM_MEMORY_ATTRIB_RW);
		int Destroy();

		int Allocate(std::size_t size, std::size_t alignment,
			ArenaAllocation* allocation);
		int Release(ArenaAllocation* allocation);

		bool IsInitialized() const { return m_initialized; }
		ArenaMemory Memory() const { return m_memory; }
		Statistics GetStatistics() const;

	private:
		struct LiveRange
		{
			std::size_t size = 0;
			std::uint64_t token = 0;
		};

		struct Slab
		{
			MappedBlock block;
			std::size_t logical_size = 0;
			std::map<std::size_t, std::size_t> free_ranges;
			std::map<std::size_t, LiveRange> live_ranges;
		};

		int AddSlab(std::size_t minimum_size, std::uint32_t* slab_index);
		bool AllocateFromSlab(std::uint32_t slab_index, std::size_t size,
			std::size_t alignment, ArenaAllocation* allocation);
		void InsertFreeRange(Slab& slab, std::size_t offset, std::size_t size);

		std::vector<Slab> m_slabs;
		std::string m_name;
		ArenaMemory m_memory = ArenaMemory::MainNonCached;
		SceGxmMemoryAttribFlags m_attributes = SCE_GXM_MEMORY_ATTRIB_RW;
		std::size_t m_preferred_slab_size = 0;
		std::size_t m_allocated_bytes = 0;
		std::size_t m_peak_allocated_bytes = 0;
		std::size_t m_live_allocations = 0;
		std::uint64_t m_next_token = 1;
		bool m_initialized = false;
	};
} // namespace VitaGXM

#endif
