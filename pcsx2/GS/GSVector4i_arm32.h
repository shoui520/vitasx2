// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <algorithm>
#include <cstring>

class alignas(16) GSVector4i
{
public:
	union
	{
		struct
		{
			int x, y, z, w;
		};
		struct
		{
			int left, top, right, bottom;
		};
		int v[4];
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

	GSVector4i() = default;

	explicit GSVector4i(int value)
	{
		for (size_t i = 0; i < 4; i++)
			I32[i] = value;
	}

	GSVector4i(int x_, int y_, int z_, int w_)
	{
		I32[0] = x_;
		I32[1] = y_;
		I32[2] = z_;
		I32[3] = w_;
	}

	explicit GSVector4i(const GSVector4& v, bool truncate = true);

	static GSVector4i cast(const GSVector4& v);

	static GSVector4i load(int value)
	{
		return GSVector4i(value);
	}

	template <bool aligned = false>
	static GSVector4i load(const void* src)
	{
		GSVector4i ret;
		std::memcpy(ret.U8, src, sizeof(ret.U8));
		return ret;
	}

	static GSVector4i loadl(const void* src)
	{
		GSVector4i ret(0);
		std::memcpy(ret.U8, src, sizeof(u64));
		return ret;
	}

	static void storel(void* dst, const GSVector4i& value)
	{
		std::memcpy(dst, value.U8, sizeof(u64));
	}

	static GSVector4i x0f(u32 count)
	{
		GSVector4i ret(0);
		const u32 limit = std::min<u32>(count, 16);
		for (u32 i = 0; i < limit; i++)
			ret.U8[i] = 0x0f;
		return ret;
	}

	static GSVector4i xffffffff()
	{
		return GSVector4i(static_cast<int>(0xffffffffu));
	}

	int width() const
	{
		return right - left;
	}

	int height() const
	{
		return bottom - top;
	}

	bool eq(const GSVector4i& other) const
	{
		return std::memcmp(U8, other.U8, sizeof(U8)) == 0;
	}

	GSVector4i eq8(const GSVector4i& other) const
	{
		GSVector4i ret(0);
		for (size_t i = 0; i < 16; i++)
			ret.U8[i] = (U8[i] == other.U8[i]) ? 0xff : 0x00;
		return ret;
	}

	u32 mask() const
	{
		u32 ret = 0;
		for (u32 i = 0; i < 16; i++)
			ret |= ((U8[i] & 0x80) ? 1u : 0u) << i;
		return ret;
	}

	GSVector4i upl8(const GSVector4i& other) const
	{
		GSVector4i ret(0);
		for (size_t i = 0; i < 8; i++)
		{
			ret.U8[i * 2 + 0] = U8[i];
			ret.U8[i * 2 + 1] = other.U8[i];
		}
		return ret;
	}

	GSVector4i ge32(const GSVector4i& other) const
	{
		GSVector4i ret(0);
		for (size_t i = 0; i < 4; i++)
			ret.U32[i] = (I32[i] >= other.I32[i]) ? 0xffffffffu : 0u;
		return ret;
	}

	GSVector4i le32(const GSVector4i& other) const
	{
		GSVector4i ret(0);
		for (size_t i = 0; i < 4; i++)
			ret.U32[i] = (I32[i] <= other.I32[i]) ? 0xffffffffu : 0u;
		return ret;
	}

	bool allfalse() const
	{
		return (U32[0] | U32[1] | U32[2] | U32[3]) == 0;
	}

	bool alltrue() const
	{
		return U32[0] == 0xffffffffu && U32[1] == 0xffffffffu &&
			U32[2] == 0xffffffffu && U32[3] == 0xffffffffu;
	}

	GSVector4i operator>>(int bits) const
	{
		GSVector4i ret;
		for (size_t i = 0; i < 4; i++)
			ret.U32[i] = U32[i] >> bits;
		return ret;
	}

	GSVector4i operator&(const GSVector4i& other) const
	{
		GSVector4i ret;
		for (size_t i = 0; i < 4; i++)
			ret.U32[i] = U32[i] & other.U32[i];
		return ret;
	}

	GSVector4i& operator&=(const GSVector4i& other)
	{
		for (size_t i = 0; i < 4; i++)
			U32[i] &= other.U32[i];
		return *this;
	}

	GSVector4i operator|(const GSVector4i& other) const
	{
		GSVector4i ret;
		for (size_t i = 0; i < 4; i++)
			ret.U32[i] = U32[i] | other.U32[i];
		return ret;
	}

	GSVector4i operator+(const GSVector4i& other) const
	{
		GSVector4i ret;
		for (size_t i = 0; i < 4; i++)
			ret.U32[i] = U32[i] + other.U32[i];
		return ret;
	}

	static void sw32_inv(GSVector4i& a, GSVector4i& b, GSVector4i& c, GSVector4i& d);
};
