// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#include "VitaGxmArena.h"

#if !defined(VITASX2_QEMU_VALIDATION)

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <limits>
#include <utility>

namespace VitaGXM
{
	namespace
	{
		bool IsPowerOfTwo(std::size_t value)
		{
			return value != 0 && (value & (value - 1)) == 0;
		}

		bool AlignUp(std::size_t value, std::size_t alignment, std::size_t* aligned)
		{
			if (!aligned || !IsPowerOfTwo(alignment) ||
				value > std::numeric_limits<std::size_t>::max() - (alignment - 1))
			{
				return false;
			}

			*aligned = (value + alignment - 1) & ~(alignment - 1);
			return true;
		}

		bool AlignSlabOffset(const void* base, std::size_t offset,
			std::size_t alignment, std::size_t* aligned_offset)
		{
			if (!base || !aligned_offset)
				return false;

			const std::size_t base_address =
				reinterpret_cast<std::size_t>(base);
			if (offset > std::numeric_limits<std::size_t>::max() - base_address)
				return false;

			std::size_t aligned_address = 0;
			if (!AlignUp(base_address + offset, alignment, &aligned_address) ||
				aligned_address < base_address)
			{
				return false;
			}

			*aligned_offset = aligned_address - base_address;
			return true;
		}
	} // namespace

	ArenaAllocation::ArenaAllocation(Arena* arena, void* data, std::size_t size,
		std::size_t offset, std::uint32_t slab_index,
		std::uint64_t token)
		: m_arena(arena)
		, m_data(data)
		, m_size(size)
		, m_offset(offset)
		, m_slab_index(slab_index)
		, m_token(token)
	{
	}

	ArenaAllocation::ArenaAllocation(ArenaAllocation&& other) noexcept
	{
		*this = std::move(other);
	}

	ArenaAllocation& ArenaAllocation::operator=(ArenaAllocation&& other) noexcept
	{
		if (this == &other)
			return *this;

		Reset();
		m_arena = other.m_arena;
		m_data = other.m_data;
		m_size = other.m_size;
		m_offset = other.m_offset;
		m_slab_index = other.m_slab_index;
		m_token = other.m_token;
		other.Clear();
		return *this;
	}

	ArenaAllocation::~ArenaAllocation() { Reset(); }

	void ArenaAllocation::Reset()
	{
		if (!m_arena)
			return;

		Arena* const arena = m_arena;
		const int result = arena->Release(this);
		assert(result >= 0);
		(void)result;
	}

	void ArenaAllocation::Clear()
	{
		m_arena = nullptr;
		m_data = nullptr;
		m_size = 0;
		m_offset = 0;
		m_slab_index = 0;
		m_token = 0;
	}

	Arena::~Arena()
	{
		assert(m_live_allocations == 0);
		if (m_live_allocations == 0)
			Destroy();
	}

	int Arena::Initialize(ArenaMemory memory, std::string_view name,
		std::size_t preferred_slab_size,
		SceGxmMemoryAttribFlags attributes)
	{
		if (m_initialized)
			return SCE_GXM_ERROR_ALREADY_INITIALIZED;
		if (name.empty() || preferred_slab_size == 0)
			return SCE_GXM_ERROR_INVALID_VALUE;
		if ((attributes & SCE_GXM_MEMORY_ATTRIB_RW) == 0 ||
			(attributes & ~SCE_GXM_MEMORY_ATTRIB_RW) != 0)
			return SCE_GXM_ERROR_INVALID_VALUE;

		m_memory = memory;
		m_name.assign(name.data(), name.size());
		m_preferred_slab_size = preferred_slab_size;
		m_attributes = attributes;
		m_initialized = true;
		return 0;
	}

	int Arena::Destroy()
	{
		if (!m_initialized)
			return 0;
		if (m_live_allocations != 0)
			return SCE_GXM_ERROR_INVALID_VALUE;

		int first_error = 0;
		for (Slab& slab : m_slabs)
		{
			if (!slab.block.IsAllocated())
				continue;

			const int result = ReleaseMappedBlock(&slab.block);
			if (result < 0 && first_error >= 0)
				first_error = result;
			else if (result >= 0)
				slab.free_ranges.clear();
		}

		if (first_error < 0)
			return first_error;

		m_slabs.clear();
		m_name.clear();
		m_preferred_slab_size = 0;
		m_allocated_bytes = 0;
		m_peak_allocated_bytes = 0;
		m_live_allocations = 0;
		m_next_token = 1;
		m_initialized = false;
		return 0;
	}

	int Arena::Allocate(std::size_t size, std::size_t alignment,
		ArenaAllocation* allocation)
	{
		if (!allocation)
			return SCE_GXM_ERROR_INVALID_POINTER;
		if (!m_initialized)
			return SCE_GXM_ERROR_UNINITIALIZED;
		if (*allocation || size == 0 || !IsPowerOfTwo(alignment))
			return SCE_GXM_ERROR_INVALID_VALUE;

		// Best-fit across existing slabs limits fragmentation without changing
		// the address of any live allocation.
		std::uint32_t best_slab = std::numeric_limits<std::uint32_t>::max();
		std::size_t best_waste = std::numeric_limits<std::size_t>::max();
		for (std::uint32_t i = 0; i < m_slabs.size(); i++)
		{
			if (!m_slabs[i].block.IsMapped())
				continue;
			for (const auto& [offset, range_size] : m_slabs[i].free_ranges)
			{
				std::size_t aligned_offset = 0;
				if (!AlignSlabOffset(m_slabs[i].block.base, offset, alignment,
						&aligned_offset) ||
					aligned_offset < offset)
					continue;
				const std::size_t prefix = aligned_offset - offset;
				if (prefix > range_size || size > range_size - prefix)
					continue;
				const std::size_t waste = range_size - prefix - size;
				if (waste < best_waste)
				{
					best_waste = waste;
					best_slab = i;
				}
			}
		}

		if (best_slab == std::numeric_limits<std::uint32_t>::max())
		{
			if (size > std::numeric_limits<std::size_t>::max() - (alignment - 1))
				return SCE_GXM_ERROR_INVALID_VALUE;
			const int result = AddSlab(size + (alignment - 1), &best_slab);
			if (result < 0)
				return result;
		}

		if (!AllocateFromSlab(best_slab, size, alignment, allocation))
			return SCE_GXM_ERROR_OUT_OF_MEMORY;
		return 0;
	}

	int Arena::Release(ArenaAllocation* allocation)
	{
		if (!allocation)
			return SCE_GXM_ERROR_INVALID_POINTER;
		if (allocation->m_arena != this || allocation->m_slab_index >= m_slabs.size())
			return SCE_GXM_ERROR_INVALID_VALUE;

		Slab& slab = m_slabs[allocation->m_slab_index];
		auto live = slab.live_ranges.find(allocation->m_offset);
		if (live == slab.live_ranges.end() ||
			live->second.token != allocation->m_token ||
			live->second.size != allocation->m_size)
		{
			return SCE_GXM_ERROR_INVALID_VALUE;
		}

		const std::size_t offset = allocation->m_offset;
		const std::size_t size = live->second.size;
		slab.live_ranges.erase(live);
		InsertFreeRange(slab, offset, size);
		m_allocated_bytes -= size;
		m_live_allocations--;
		allocation->Clear();
		return 0;
	}

	Arena::Statistics Arena::GetStatistics() const
	{
		Statistics stats;
		stats.allocated_bytes = m_allocated_bytes;
		stats.peak_allocated_bytes = m_peak_allocated_bytes;
		stats.live_allocations = m_live_allocations;
		stats.slab_count = m_slabs.size();
		for (const Slab& slab : m_slabs)
		{
			if (!slab.block.IsMapped())
				continue;
			stats.mapped_capacity += slab.block.size;
			stats.logical_capacity += slab.logical_size;
			stats.fetch_guard_bytes += slab.block.size - slab.logical_size;
			for (const auto& [offset, size] : slab.free_ranges)
			{
				(void)offset;
				stats.free_bytes += size;
				stats.largest_free_range = std::max(stats.largest_free_range, size);
			}
		}
		return stats;
	}

	int Arena::AddSlab(std::size_t minimum_size, std::uint32_t* slab_index)
	{
		if (!slab_index)
			return SCE_GXM_ERROR_INVALID_POINTER;
		if (m_slabs.size() >= std::numeric_limits<std::uint32_t>::max())
			return SCE_GXM_ERROR_OUT_OF_MEMORY;

		Slab slab;
		char block_name[32];
		std::snprintf(block_name, sizeof(block_name), "%.22s-%u", m_name.c_str(),
			static_cast<unsigned>(m_slabs.size()));

		const std::size_t logical_request =
			std::max(m_preferred_slab_size, minimum_size);
		const std::size_t fetch_guard =
			m_memory == ArenaMemory::MainNonCached ?
				GpuMappedFetchGuardSize : 0;
		if (logical_request >
			std::numeric_limits<std::size_t>::max() - fetch_guard)
		{
			return SCE_GXM_ERROR_INVALID_VALUE;
		}
		const std::size_t requested_size = logical_request + fetch_guard;
		const SceKernelMemBlockType block_type =
			(m_memory == ArenaMemory::Cdram) ? SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW : SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_NC_RW;
		const int result = AllocateMappedBlock(block_name, block_type, requested_size,
			m_attributes, &slab.block);
		if (result < 0)
			return result;

		if (slab.block.size < fetch_guard ||
			slab.block.size - fetch_guard < minimum_size)
		{
			ReleaseMappedBlock(&slab.block);
			return SCE_GXM_ERROR_OUT_OF_MEMORY;
		}
		slab.logical_size = slab.block.size - fetch_guard;
		if (fetch_guard != 0)
		{
			std::memset(static_cast<std::uint8_t*>(slab.block.base) +
				slab.logical_size, 0, fetch_guard);
		}
		slab.free_ranges.emplace(0, slab.logical_size);
		m_slabs.emplace_back(std::move(slab));
		*slab_index = static_cast<std::uint32_t>(m_slabs.size() - 1);
		return 0;
	}

	bool Arena::AllocateFromSlab(std::uint32_t slab_index, std::size_t size,
		std::size_t alignment,
		ArenaAllocation* allocation)
	{
		Slab& slab = m_slabs[slab_index];
		auto best = slab.free_ranges.end();
		std::size_t best_aligned_offset = 0;
		std::size_t best_waste = std::numeric_limits<std::size_t>::max();

		for (auto it = slab.free_ranges.begin(); it != slab.free_ranges.end(); ++it)
		{
			std::size_t aligned_offset = 0;
			if (!AlignSlabOffset(slab.block.base, it->first, alignment,
					&aligned_offset) ||
				aligned_offset < it->first)
				continue;
			const std::size_t prefix = aligned_offset - it->first;
			if (prefix > it->second || size > it->second - prefix)
				continue;
			const std::size_t waste = it->second - prefix - size;
			if (waste < best_waste)
			{
				best = it;
				best_aligned_offset = aligned_offset;
				best_waste = waste;
			}
		}

		if (best == slab.free_ranges.end())
			return false;

		const std::size_t range_offset = best->first;
		const std::size_t range_size = best->second;
		slab.free_ranges.erase(best);
		if (best_aligned_offset > range_offset)
			slab.free_ranges.emplace(range_offset, best_aligned_offset - range_offset);
		const std::size_t allocation_end = best_aligned_offset + size;
		const std::size_t range_end = range_offset + range_size;
		if (allocation_end < range_end)
			slab.free_ranges.emplace(allocation_end, range_end - allocation_end);

		std::uint64_t token = m_next_token++;
		if (token == 0)
			token = m_next_token++;
		slab.live_ranges.emplace(best_aligned_offset, LiveRange{size, token});

		void* const data =
			static_cast<std::uint8_t*>(slab.block.base) + best_aligned_offset;
		*allocation =
			ArenaAllocation(this, data, size, best_aligned_offset, slab_index, token);
		m_allocated_bytes += size;
		m_peak_allocated_bytes = std::max(m_peak_allocated_bytes, m_allocated_bytes);
		m_live_allocations++;
		return true;
	}

	void Arena::InsertFreeRange(Slab& slab, std::size_t offset, std::size_t size)
	{
		auto next = slab.free_ranges.lower_bound(offset);
		if (next != slab.free_ranges.begin())
		{
			auto previous = std::prev(next);
			if (previous->first + previous->second == offset)
			{
				offset = previous->first;
				size += previous->second;
				slab.free_ranges.erase(previous);
			}
		}

		next = slab.free_ranges.lower_bound(offset);
		if (next != slab.free_ranges.end() && offset + size == next->first)
		{
			size += next->second;
			slab.free_ranges.erase(next);
		}
		slab.free_ranges.emplace(offset, size);
	}
} // namespace VitaGXM

#endif
