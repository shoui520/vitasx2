// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Types.h"

#include <bit>

namespace VitaA32
{
	// Convert under the FPCR/FPSCR already installed by the EE or VU execution
	// boundary. ARMv7 scalar VFP VCVT.F32.S32 observes FPSCR.RMode; unlike the
	// Advanced SIMD form, it is not fixed to round-to-nearest. ITOF's optional
	// scale is an exact power of two. Every nonzero converted signed integer is
	// normal for the supported offsets, so adjusting the exponent cannot lose a
	// mantissa bit or cross zero/subnormal infinity boundaries.
	inline u32 ConvertSignedIntToFloatBits(u32 bits, unsigned scale_offset)
	{
		if (scale_offset > 31)
			return 0;

#if defined(ARCH_ARM32)
		const s32 source = static_cast<s32>(bits);
		float converted;
		__asm__(
			"vmov %0, %1\n\t"
			"vcvt.f32.s32 %0, %0"
			: "=&t"(converted)
			: "r"(source));
		u32 result = std::bit_cast<u32>(converted);
		if (scale_offset != 0 && result != 0)
			result -= scale_offset << 23;
		return result;
#else
		float value = static_cast<float>(static_cast<s32>(bits));
		u32 result = std::bit_cast<u32>(value);
		if (scale_offset != 0 && result != 0)
			result -= scale_offset << 23;
		return result;
#endif
	}

}
