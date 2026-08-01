// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#include "PrecompiledHeader.h"

#include "Memory.h"
#include "pcsx2/vita/VitaEeExecutor.h"
#include "pcsx2/vita/VitaEeRegionMemory.h"

#include <algorithm>
#include <cstring>

namespace VitaEE::RegionIR
{
	namespace
	{
		constexpr u32 SOURCE_PAGE_SHIFT = BlockExecutor::RAM_SOURCE_PAGE_SHIFT;
		constexpr u32 SOURCE_CHUNK_SHIFT = BlockExecutor::RAM_SOURCE_CHUNK_SHIFT;
		constexpr u32 SOURCE_PAGE_COUNT = BlockExecutor::RAM_SOURCE_PAGE_COUNT;
		constexpr u32 SOURCE_CHUNK_COUNT = BlockExecutor::RAM_SOURCE_CHUNK_COUNT;
		static_assert(SOURCE_PAGE_SHIFT == vtlb_private::VTLB_PAGE_BITS);
		static_assert(SOURCE_CHUNK_SHIFT == 6);

		u32 AccessSize(MemoryAccessKind kind)
		{
			switch (kind)
			{
				case MemoryAccessKind::LoadS8:
				case MemoryAccessKind::LoadU8:
				case MemoryAccessKind::Store8:
					return 1;
				case MemoryAccessKind::LoadS16:
				case MemoryAccessKind::LoadU16:
				case MemoryAccessKind::Store16:
					return 2;
				case MemoryAccessKind::LoadS32:
				case MemoryAccessKind::LoadU32:
				case MemoryAccessKind::Store32:
					return 4;
				case MemoryAccessKind::Load64:
				case MemoryAccessKind::Store64:
					return 8;
				case MemoryAccessKind::Load128:
				case MemoryAccessKind::Store128:
					return 16;
			}
			return 0;
		}

		bool IsStore(MemoryAccessKind kind)
		{
			return kind >= MemoryAccessKind::Store8;
		}

		bool ResolveMainRam(const MemoryRequest& request, uptr* host,
			u32* backing_offset)
		{
			if (!eeMem || !vtlb_private::vtlbdata.vmap)
				return false;
			const u32 size = AccessSize(request.kind);
			if (size == 0 || size > vtlb_private::VTLB_PAGE_SIZE -
										(request.address & vtlb_private::VTLB_PAGE_MASK))
			{
				return false;
			}

			const vtlb_private::VTLBVirtual mapping =
				vtlb_private::vtlbdata.vmap[request.address >> vtlb_private::VTLB_PAGE_BITS];
			if (mapping.isHandler(request.address))
				return false;
			const uptr resolved = mapping.assumePtr(request.address);
			const uptr ram_begin = reinterpret_cast<uptr>(eeMem->Main);
			const uptr ram_end = ram_begin +
			                     std::min(Ps2MemSize::ExposedRam, Ps2MemSize::MainRam);
			if (resolved < ram_begin || resolved >= ram_end ||
				static_cast<uptr>(size) > ram_end - resolved)
			{
				return false;
			}
			*host = resolved;
			*backing_offset = static_cast<u32>(resolved - ram_begin);
			return true;
		}

		bool DirectRead(void* opaque, const MemoryRequest& request, u128* value)
		{
			if (!value || ProbeVtlbMemory(opaque, request) !=
							  MemoryProbeResult::Direct)
			{
				return false;
			}
			uptr host = 0;
			u32 backing_offset = 0;
			if (!ResolveMainRam(request, &host, &backing_offset))
				return false;
			*value = {};
			std::memcpy(value, reinterpret_cast<const void*>(host),
				AccessSize(request.kind));
			return true;
		}

		bool DirectWrite(void* opaque, const MemoryRequest& request,
			const u128& value)
		{
			if (ProbeVtlbMemory(opaque, request) != MemoryProbeResult::Direct)
				return false;
			uptr host = 0;
			u32 backing_offset = 0;
			if (!ResolveMainRam(request, &host, &backing_offset))
				return false;
			std::memcpy(reinterpret_cast<void*>(host), &value,
				AccessSize(request.kind));
			return true;
		}
	} // namespace

	VtlbMemoryContext MakeVtlbMemoryContext(const VitaEE::BlockExecutor& executor)
	{
		return {executor.RamSourcePageLiveFlags(),
			executor.RamSourceChunkLiveBits()};
	}

	RegionMemoryInterface MakeVtlbMemoryInterface(VtlbMemoryContext* context)
	{
		return {context, ProbeVtlbMemory, DirectRead, DirectWrite};
	}

	MemoryProbeResult ProbeVtlbMemory(void* opaque,
		const MemoryRequest& request)
	{
		VtlbMemoryContext* const context =
			static_cast<VtlbMemoryContext*>(opaque);
		if (!context || !eeMem || !vtlb_private::vtlbdata.vmap)
			return MemoryProbeResult::Translation;
		const u32 size = AccessSize(request.kind);
		if (size == 0 || size > vtlb_private::VTLB_PAGE_SIZE -
									(request.address & vtlb_private::VTLB_PAGE_MASK))
		{
			return MemoryProbeResult::Translation;
		}

		const vtlb_private::VTLBVirtual mapping =
			vtlb_private::vtlbdata.vmap[request.address >> vtlb_private::VTLB_PAGE_BITS];
		if (mapping.isHandler(request.address))
			return MemoryProbeResult::Handler;

		uptr host = 0;
		u32 backing_offset = 0;
		if (!ResolveMainRam(request, &host, &backing_offset))
			return MemoryProbeResult::Translation;
		if (!IsStore(request.kind))
			return MemoryProbeResult::Direct;

		// A store without the authoritative maps is not proven safe.
		if (!context->ram_source_page_live_flags ||
			!context->ram_source_chunk_live_bits)
		{
			return MemoryProbeResult::SelfModifyingCode;
		}
		const u32 end = backing_offset + size;
		const u32 first_page = backing_offset >> SOURCE_PAGE_SHIFT;
		const u32 last_page = (end - 1) >> SOURCE_PAGE_SHIFT;
		for (u32 page = first_page; page <= last_page; page++)
		{
			if (page >= SOURCE_PAGE_COUNT ||
				context->ram_source_page_live_flags[page] == 0)
			{
				continue;
			}
			const u32 first_chunk =
				std::max(backing_offset, page << SOURCE_PAGE_SHIFT) >>
				SOURCE_CHUNK_SHIFT;
			const u32 last_chunk =
				(std::min(end, (page + 1) << SOURCE_PAGE_SHIFT) - 1) >>
				SOURCE_CHUNK_SHIFT;
			for (u32 chunk = first_chunk;
				 chunk <= last_chunk && chunk < SOURCE_CHUNK_COUNT; chunk++)
			{
				if ((context->ram_source_chunk_live_bits[chunk >> 3] &
						static_cast<u8>(1u << (chunk & 7))) != 0)
				{
					return MemoryProbeResult::SelfModifyingCode;
				}
			}
		}
		return MemoryProbeResult::Direct;
	}
} // namespace VitaEE::RegionIR
