// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#if !defined(VITASX2_QEMU_VALIDATION)

#include <psp2/gxm.h>
#include <psp2/kernel/sysmem.h>

#include <cstddef>
#include <cstdint>

namespace VitaGXM
{
	enum class MemoryMapping : std::uint8_t
	{
		None,
		General,
		VertexUsse,
		FragmentUsse,
	};

	// One kernel memblock and the GXM mapping which owns it. The mapped size is
	// retained because libgxm mappings must be torn down before the memblock.
	struct MappedBlock
	{
		SceUID uid = -1;
		void* base = nullptr;
		SceSize size = 0;
		MemoryMapping mapping = MemoryMapping::None;
		std::uint32_t usse_offset = 0;
		bool mapped = false;

		bool IsAllocated() const { return uid >= 0; }
		bool IsMapped() const { return IsAllocated() && base && mapped; }
	};

	// These helpers port the allocation/mapping lifecycle owned by Sony's PSP2
	// SDK graphics/api_libgxm/basic sample (graphicsAlloc and the two USSE
	// allocators). CDRAM and LPDDR sizes are rounded to their documented kernel
	// memblock granularities before being mapped for the GPU.
	int AllocateMappedBlock(const char* name, SceKernelMemBlockType type,
		std::size_t requested_size,
		SceGxmMemoryAttribFlags attributes, MappedBlock* block);
	int AllocateVertexUsseBlock(const char* name, std::size_t requested_size,
		MappedBlock* block);
	int AllocateFragmentUsseBlock(const char* name, std::size_t requested_size,
		MappedBlock* block);

	// Returns the first unmap/free error. Failed operations retain enough state
	// for a later call to retry instead of forgetting a live kernel allocation.
	int ReleaseMappedBlock(MappedBlock* block);
} // namespace VitaGXM

#endif
