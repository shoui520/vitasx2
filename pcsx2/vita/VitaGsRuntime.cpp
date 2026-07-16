// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/GSExtra.h"
#include "GS/GSXXH.h"
#include "GS/GSVector.h"
#include "GS/MultiISA.h"

#include "common/BitUtils.h"

#include <cstring>

// PCSX2's desktop MultiISA.cpp selects among x86 variants at process startup.
// Cortex-A9 has one compiled NEON implementation, so publish that same dispatch
// contract directly without pulling cpuinfo and desktop ISA variants into Vita.
const ProcessorFeatures g_cpu = {};

u64 (&MultiISAFunctions::GSXXH3_64_Long)(const void* data, size_t len) =
	isa_native::GSXXH3_64_Long;
u32 (&MultiISAFunctions::GSXXH3_64_Update)(void* state, const void* data, size_t len) =
	isa_native::GSXXH3_64_Update;
u64 (&MultiISAFunctions::GSXXH3_64_Digest)(void* state) =
	isa_native::GSXXH3_64_Digest;

// Exact PCSX2 owner: GS.cpp::GSGetRGBA8AlphaMinMax(). GSRendererHW's texture
// cache uses this on every host ISA, so keep the vector implementation intact;
// GSVector4i maps these operations to NEON in the Vita build.
std::pair<u8, u8> GSGetRGBA8AlphaMinMax(const void* data, u32 width, u32 height, u32 stride)
{
	GSVector4i minc = GSVector4i::xffffffff();
	GSVector4i maxc = GSVector4i::zero();

	const u8* ptr = static_cast<const u8*>(data);
	if ((width % 4) == 0)
	{
		for (u32 r = 0; r < height; r++)
		{
			const u8* rptr = ptr;
			for (u32 c = 0; c < width; c += 4)
			{
				const GSVector4i v = GSVector4i::load<false>(rptr);
				rptr += sizeof(GSVector4i);
				minc = minc.min_u32(v);
				maxc = maxc.max_u32(v);
			}

			ptr += stride;
		}
	}
	else
	{
		const u32 aligned_width = Common::AlignDownPow2(width, 4);
		static constexpr const GSVector4i masks[3][2] = {
			{GSVector4i::cxpr(0xFFFFFFFF, 0, 0, 0), GSVector4i::cxpr(0, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF)},
			{GSVector4i::cxpr(0xFFFFFFFF, 0xFFFFFFFF, 0, 0), GSVector4i::cxpr(0, 0, 0xFFFFFFFF, 0xFFFFFFFF)},
			{GSVector4i::cxpr(0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0), GSVector4i::cxpr(0, 0, 0, 0xFFFFFFFF)},
		};
		const u32 unaligned_pixels = width & 3;
		const GSVector4i last_mask_and = masks[unaligned_pixels - 1][0];
		const GSVector4i last_mask_or = masks[unaligned_pixels - 1][1];

		for (u32 r = 0; r < height; r++)
		{
			const u8* rptr = ptr;
			for (u32 c = 0; c < aligned_width; c += 4)
			{
				const GSVector4i v = GSVector4i::load<false>(rptr);
				rptr += sizeof(GSVector4i);
				minc = minc.min_u32(v);
				maxc = maxc.max_u32(v);
			}

			GSVector4i v;
			u32 vu;
			if (unaligned_pixels == 3)
			{
				v = GSVector4i::loadl(rptr);
				std::memcpy(&vu, rptr + sizeof(u32) * 2, sizeof(vu));
				v = v.insert32<2>(vu);
			}
			else if (unaligned_pixels == 2)
			{
				v = GSVector4i::loadl(rptr);
			}
			else
			{
				std::memcpy(&vu, rptr, sizeof(vu));
				v = GSVector4i::load(vu);
			}

			minc = minc.min_u32(v | last_mask_or);
			maxc = maxc.max_u32(v & last_mask_and);

			ptr += stride;
		}
	}

	return std::make_pair<u8, u8>(static_cast<u8>(minc.minv_u32() >> 24),
		static_cast<u8>(maxc.maxv_u32() >> 24));
}
