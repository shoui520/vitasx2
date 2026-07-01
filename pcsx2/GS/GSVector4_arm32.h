// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <cstring>

class alignas(16) GSVector4
{
public:
	union
	{
		struct
		{
			float x, y, z, w;
		};
		float v[4];
		float F32[4];
		s8 I8[16];
		s16 I16[8];
		s32 I32[4];
		s64 I64[2];
		u8 U8[16];
		u16 U16[8];
		u32 U32[4];
		u64 U64[2];
	};

	GSVector4() = default;

	explicit GSVector4(float value)
	{
		for (size_t i = 0; i < 4; i++)
			F32[i] = value;
	}

	GSVector4(float x_, float y_, float z_, float w_)
	{
		F32[0] = x_;
		F32[1] = y_;
		F32[2] = z_;
		F32[3] = w_;
	}

	explicit GSVector4(const GSVector4i& v);

	static GSVector4 cast(const GSVector4i& v);

	GSVector4 xzxz(const GSVector4& other) const
	{
		return GSVector4(F32[0], F32[2], other.F32[0], other.F32[2]);
	}

	GSVector4 ywyw(const GSVector4& other) const
	{
		return GSVector4(F32[1], F32[3], other.F32[1], other.F32[3]);
	}
};
