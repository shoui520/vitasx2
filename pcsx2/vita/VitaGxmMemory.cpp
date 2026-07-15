// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#include "VitaGxmMemory.h"

#if !defined(VITASX2_QEMU_VALIDATION)

#include <limits>

namespace VitaGXM
{
	namespace
	{
		constexpr std::uint64_t LPDDR_MEMBLOCK_ALIGNMENT = 4 * 1024;
		constexpr std::uint64_t CDRAM_MEMBLOCK_ALIGNMENT = 256 * 1024;

		bool IsCdramType(SceKernelMemBlockType type)
		{
			return type == SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW ||
			       type == SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_R ||
			       type == SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_L1WBWA_RW;
		}

		int GetAlignedSize(SceKernelMemBlockType type, std::size_t requested_size,
			SceSize* aligned_size)
		{
			if (!aligned_size)
				return SCE_GXM_ERROR_INVALID_POINTER;
			if (requested_size == 0)
				return SCE_GXM_ERROR_INVALID_VALUE;

			const std::uint64_t alignment =
				IsCdramType(type) ? CDRAM_MEMBLOCK_ALIGNMENT : LPDDR_MEMBLOCK_ALIGNMENT;
			const std::uint64_t requested = static_cast<std::uint64_t>(requested_size);
			const std::uint64_t rounded =
				(requested + alignment - 1u) & ~(alignment - 1u);
			if (rounded < requested || rounded > std::numeric_limits<SceSize>::max())
			{
				return SCE_GXM_ERROR_INVALID_VALUE;
			}

			*aligned_size = static_cast<SceSize>(rounded);
			return 0;
		}

		int AllocateKernelBlock(const char* name, SceKernelMemBlockType type,
			std::size_t requested_size, MemoryMapping mapping,
			MappedBlock* block)
		{
			if (!name || !block)
				return SCE_GXM_ERROR_INVALID_POINTER;
			if (block->IsAllocated() || block->mapped)
				return SCE_GXM_ERROR_INVALID_VALUE;

			SceSize size = 0;
			const int size_result = GetAlignedSize(type, requested_size, &size);
			if (size_result < 0)
				return size_result;

			block->size = size;
			block->mapping = mapping;
			block->uid = sceKernelAllocMemBlock(name, type, size, nullptr);
			if (block->uid < 0)
			{
				const int result = block->uid;
				*block = {};
				return result;
			}

			const int base_result = sceKernelGetMemBlockBase(block->uid, &block->base);
			if (base_result < 0 || !block->base)
			{
				const int result =
					base_result < 0 ? base_result : static_cast<int>(SCE_GXM_ERROR_INVALID_POINTER);
				ReleaseMappedBlock(block);
				return result;
			}

			return 0;
		}

		int AllocateUsseBlock(const char* name, std::size_t requested_size,
			MemoryMapping mapping, MappedBlock* block)
		{
			if (!block)
				return SCE_GXM_ERROR_INVALID_POINTER;

			const int allocation_result =
				AllocateKernelBlock(name, SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_NC_RW,
					requested_size, mapping, block);
			if (allocation_result < 0)
				return allocation_result;

			int map_result = 0;
			if (mapping == MemoryMapping::VertexUsse)
			{
				map_result = sceGxmMapVertexUsseMemory(block->base, block->size,
					&block->usse_offset);
			}
			else
			{
				map_result = sceGxmMapFragmentUsseMemory(block->base, block->size,
					&block->usse_offset);
			}

			if (map_result < 0)
			{
				ReleaseMappedBlock(block);
				return map_result;
			}

			block->mapped = true;
			return 0;
		}
	} // namespace

	int AllocateMappedBlock(const char* name, SceKernelMemBlockType type,
		std::size_t requested_size,
		SceGxmMemoryAttribFlags attributes,
		MappedBlock* block)
	{
		if (!block)
			return SCE_GXM_ERROR_INVALID_POINTER;

		const int allocation_result = AllocateKernelBlock(
			name, type, requested_size, MemoryMapping::General, block);
		if (allocation_result < 0)
			return allocation_result;

		const int map_result = sceGxmMapMemory(block->base, block->size, attributes);
		if (map_result < 0)
		{
			ReleaseMappedBlock(block);
			return map_result;
		}

		block->mapped = true;
		return 0;
	}

	int AllocateVertexUsseBlock(const char* name, std::size_t requested_size,
		MappedBlock* block)
	{
		return AllocateUsseBlock(name, requested_size, MemoryMapping::VertexUsse,
			block);
	}

	int AllocateFragmentUsseBlock(const char* name, std::size_t requested_size,
		MappedBlock* block)
	{
		return AllocateUsseBlock(name, requested_size, MemoryMapping::FragmentUsse,
			block);
	}

	int ReleaseMappedBlock(MappedBlock* block)
	{
		if (!block)
			return SCE_GXM_ERROR_INVALID_POINTER;

		if (block->mapped && block->base)
		{
			int unmap_result = 0;
			switch (block->mapping)
			{
				case MemoryMapping::General:
					unmap_result = sceGxmUnmapMemory(block->base);
					break;
				case MemoryMapping::VertexUsse:
					unmap_result = sceGxmUnmapVertexUsseMemory(block->base);
					break;
				case MemoryMapping::FragmentUsse:
					unmap_result = sceGxmUnmapFragmentUsseMemory(block->base);
					break;
				case MemoryMapping::None:
					return SCE_GXM_ERROR_INVALID_VALUE;
			}

			if (unmap_result < 0)
				return unmap_result;
			block->mapped = false;
		}

		if (block->uid >= 0)
		{
			const int free_result = sceKernelFreeMemBlock(block->uid);
			if (free_result < 0)
				return free_result;
		}

		*block = {};
		return 0;
	}
} // namespace VitaGXM

#endif
