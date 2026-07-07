// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GS/GSVector.h"
#include "SPU2/defs.h"

#if defined(ARCH_ARM32)
#include <arm_neon.h>
#endif

#if defined(VITASX2_QEMU_VALIDATION)
u32 g_qemuSpu2ReverbDownsampleNeon = 0;
u32 g_qemuSpu2ReverbUpsampleNeon = 0;
u32 g_qemuSpu2ReverbSparseTapAccumulates = 0;
#endif

MULTI_ISA_UNSHARED_START

static constexpr u32 NUM_TAPS = 39;
static constexpr std::array<s16, 48> filter_down_coefs alignas(32) = {
	-1,
	0,
	2,
	0,
	-10,
	0,
	35,
	0,
	-103,
	0,
	266,
	0,
	-616,
	0,
	1332,
	0,
	-2960,
	0,
	10246,
	16384,
	10246,
	0,
	-2960,
	0,
	1332,
	0,
	-616,
	0,
	266,
	0,
	-103,
	0,
	35,
	0,
	-10,
	0,
	2,
	0,
	-1,
};

static constexpr std::array<s16, 48> make_up_coefs()
{
	std::array<s16, 48> ret = {};

	for (u32 i = 0; i < NUM_TAPS; i++)
	{
		ret[i] = static_cast<s16>(std::clamp<s32>(filter_down_coefs[i] * 2, INT16_MIN, INT16_MAX));
	}

	return ret;
}

static constexpr std::array<s16, 48> filter_up_coefs alignas(32) = make_up_coefs();

static constexpr std::array<s16, 20> make_sparse_even_coefs(const std::array<s16, 48>& source)
{
	std::array<s16, 20> ret = {};

	for (u32 i = 0; i < ret.size(); i++)
		ret[i] = source[i * 2];

	return ret;
}

static constexpr std::array<s16, 20> filter_down_sparse_even_coefs alignas(16) =
	make_sparse_even_coefs(filter_down_coefs);
static constexpr std::array<s16, 20> filter_up_sparse_even_coefs alignas(16) =
	make_sparse_even_coefs(filter_up_coefs);

s32 __forceinline ReverbDownsample_reference(V_Core& core, bool right)
{
	int index = (core.RevbSampleBufPos - NUM_TAPS) & 63;
	s32 out = 0;

	for (u32 i = 0; i < NUM_TAPS; i++)
	{
		out += core.RevbDownBuf[right][index + i] * filter_down_coefs[i];
	}

	out >>= 15;

	return clamp_mix(out);
}

#if defined(ARCH_ARM32)
static __forceinline int32x4_t ReverbAccumulateEven8(int32x4_t acc, const s16* samples, const s16* coefs)
{
	const int16x8x2_t sample_pairs = vld2q_s16(samples);
	const int16x8_t coef_vec = vld1q_s16(coefs);
	acc = vmlal_s16(acc, vget_low_s16(sample_pairs.val[0]), vget_low_s16(coef_vec));
	acc = vmlal_s16(acc, vget_high_s16(sample_pairs.val[0]), vget_high_s16(coef_vec));
	return acc;
}

static __forceinline int32x4_t ReverbAccumulateEven4(int32x4_t acc, const s16* samples, const s16* coefs)
{
	const int16x4x2_t sample_pairs = vld2_s16(samples);
	return vmlal_s16(acc, sample_pairs.val[0], vld1_s16(coefs));
}

static __forceinline int32x4_t ReverbAccumulateSparseTaps(
	int32x4_t acc, const s16* samples, const s16* even_coefs, s16 center_coef)
{
	// PCSX2 owner: ReverbDownsample_reference()/ReverbUpsample_reference().
	// The 39-tap filters have zero odd taps except the center tap at index 19;
	// skip those zero multiplies on Cortex-A9 and keep the same 32-bit sum.
	acc = ReverbAccumulateEven8(acc, &samples[0], &even_coefs[0]);
	acc = ReverbAccumulateEven8(acc, &samples[16], &even_coefs[8]);
	acc = ReverbAccumulateEven4(acc, &samples[32], &even_coefs[16]);

	const s32 center_product = static_cast<s32>(samples[19]) * static_cast<s32>(center_coef);
	acc = vaddq_s32(acc, vsetq_lane_s32(center_product, vdupq_n_s32(0), 0));
#if defined(VITASX2_QEMU_VALIDATION)
	++::g_qemuSpu2ReverbSparseTapAccumulates;
#endif
	return acc;
}

static __forceinline s32 ReverbFinishAccumulation(int32x4_t acc)
{
	int32x2_t sum = vadd_s32(vget_low_s32(acc), vget_high_s32(acc));
	sum = vpadd_s32(sum, sum);
	return clamp_mix(vget_lane_s32(sum, 0) >> 15);
}

s32 __forceinline ReverbDownsample_neon(V_Core& core, bool right)
{
	const int index = (core.RevbSampleBufPos - NUM_TAPS) & 63;
	const s16* samples = &core.RevbDownBuf[right][index];
	int32x4_t acc = vdupq_n_s32(0);

	acc = ReverbAccumulateSparseTaps(
		acc, samples, filter_down_sparse_even_coefs.data(), filter_down_coefs[19]);

#if defined(VITASX2_QEMU_VALIDATION)
	++::g_qemuSpu2ReverbDownsampleNeon;
#endif
	return ReverbFinishAccumulation(acc);
}
#endif

#if !defined(ARCH_ARM32)
#if _M_SSE >= 0x501
s32 __forceinline ReverbDownsample_avx(V_Core& core, bool right)
{
	int index = (core.RevbSampleBufPos - NUM_TAPS) & 63;

	auto c = GSVector8i::load<true>(&filter_down_coefs[0]);
	auto s = GSVector8i::load<false>(&core.RevbDownBuf[right][index]);
	auto acc = s.mul16hrs(c);

	c = GSVector8i::load<true>(&filter_down_coefs[16]);
	s = GSVector8i::load<false>(&core.RevbDownBuf[right][index + 16]);
	acc = acc.adds16(s.mul16hrs(c));

	c = GSVector8i::load<true>(&filter_down_coefs[32]);
	s = GSVector8i::load<false>(&core.RevbDownBuf[right][index + 32]);
	acc = acc.adds16(s.mul16hrs(c));

	acc = acc.adds16(acc.ba());

	acc = acc.hadds16(acc);
	acc = acc.hadds16(acc);
	acc = acc.hadds16(acc);

	return acc.I16[0];
}
#endif

s32 __forceinline ReverbDownsample_sse(V_Core& core, bool right)
{
	int index = (core.RevbSampleBufPos - NUM_TAPS) & 63;

	auto c = GSVector4i::load<true>(&filter_down_coefs[0]);
	auto s = GSVector4i::load<false>(&core.RevbDownBuf[right][index]);
	auto acc = s.mul16hrs(c);

	c = GSVector4i::load<true>(&filter_down_coefs[8]);
	s = GSVector4i::load<false>(&core.RevbDownBuf[right][index + 8]);
	acc = acc.adds16(s.mul16hrs(c));

	c = GSVector4i::load<true>(&filter_down_coefs[16]);
	s = GSVector4i::load<false>(&core.RevbDownBuf[right][index + 16]);
	acc = acc.adds16(s.mul16hrs(c));

	c = GSVector4i::load<true>(&filter_down_coefs[24]);
	s = GSVector4i::load<false>(&core.RevbDownBuf[right][index + 24]);
	acc = acc.adds16(s.mul16hrs(c));

	c = GSVector4i::load<true>(&filter_down_coefs[32]);
	s = GSVector4i::load<false>(&core.RevbDownBuf[right][index + 32]);
	acc = acc.adds16(s.mul16hrs(c));

	acc = acc.hadds16(acc);
	acc = acc.hadds16(acc);
	acc = acc.hadds16(acc);

	return acc.I16[0];
}
#endif

s32 ReverbDownsample(V_Core& core, bool right)
{
#if defined(ARCH_ARM32)
	return ReverbDownsample_neon(core, right);
#elif _M_SSE >= 0x501
	return ReverbDownsample_avx(core, right);
#else
	return ReverbDownsample_sse(core, right);
#endif
}

StereoOut32 __forceinline ReverbUpsample_reference(V_Core& core)
{
	int index = (core.RevbSampleBufPos - NUM_TAPS) & 63;
	s32 l = 0, r = 0;

	for (u32 i = 0; i < NUM_TAPS; i++)
	{
		l += core.RevbUpBuf[0][index + i] * filter_up_coefs[i];
		r += core.RevbUpBuf[1][index + i] * filter_up_coefs[i];
	}

	l >>= 15;
	r >>= 15;

	return {clamp_mix(l), clamp_mix(r)};
}

#if defined(ARCH_ARM32)
StereoOut32 __forceinline ReverbUpsample_neon(V_Core& core)
{
	const int index = (core.RevbSampleBufPos - NUM_TAPS) & 63;
	const s16* left = &core.RevbUpBuf[0][index];
	const s16* right = &core.RevbUpBuf[1][index];
	int32x4_t left_acc = vdupq_n_s32(0);
	int32x4_t right_acc = vdupq_n_s32(0);

	left_acc = ReverbAccumulateSparseTaps(
		left_acc, left, filter_up_sparse_even_coefs.data(), filter_up_coefs[19]);
	right_acc = ReverbAccumulateSparseTaps(
		right_acc, right, filter_up_sparse_even_coefs.data(), filter_up_coefs[19]);

#if defined(VITASX2_QEMU_VALIDATION)
	++::g_qemuSpu2ReverbUpsampleNeon;
#endif
	return {ReverbFinishAccumulation(left_acc), ReverbFinishAccumulation(right_acc)};
}
#endif

#if !defined(ARCH_ARM32)
#if _M_SSE >= 0x501
StereoOut32 __forceinline ReverbUpsample_avx(V_Core& core)
{
	int index = (core.RevbSampleBufPos - NUM_TAPS) & 63;

	auto c = GSVector8i::load<true>(&filter_up_coefs[0]);
	auto l = GSVector8i::load<false>(&core.RevbUpBuf[0][index]);
	auto r = GSVector8i::load<false>(&core.RevbUpBuf[1][index]);

	auto lacc = l.mul16hrs(c);
	auto racc = r.mul16hrs(c);

	c = GSVector8i::load<true>(&filter_up_coefs[16]);
	l = GSVector8i::load<false>(&core.RevbUpBuf[0][index + 16]);
	r = GSVector8i::load<false>(&core.RevbUpBuf[1][index + 16]);
	lacc = lacc.adds16(l.mul16hrs(c));
	racc = racc.adds16(r.mul16hrs(c));

	c = GSVector8i::load<true>(&filter_up_coefs[32]);
	l = GSVector8i::load<false>(&core.RevbUpBuf[0][index + 32]);
	r = GSVector8i::load<false>(&core.RevbUpBuf[1][index + 32]);
	lacc = lacc.adds16(l.mul16hrs(c));
	racc = racc.adds16(r.mul16hrs(c));

	lacc = lacc.adds16(lacc.ba());
	racc = racc.adds16(racc.ba());

	lacc = lacc.hadds16(lacc);
	lacc = lacc.hadds16(lacc);
	lacc = lacc.hadds16(lacc);

	racc = racc.hadds16(racc);
	racc = racc.hadds16(racc);
	racc = racc.hadds16(racc);

	return {lacc.I16[0], racc.I16[0]};
}
#endif

StereoOut32 __forceinline ReverbUpsample_sse(V_Core& core)
{
	int index = (core.RevbSampleBufPos - NUM_TAPS) & 63;

	auto c = GSVector4i::load<true>(&filter_up_coefs[0]);
	auto l = GSVector4i::load<false>(&core.RevbUpBuf[0][index]);
	auto r = GSVector4i::load<false>(&core.RevbUpBuf[1][index]);

	auto lacc = l.mul16hrs(c);
	auto racc = r.mul16hrs(c);

	c = GSVector4i::load<true>(&filter_up_coefs[8]);
	l = GSVector4i::load<false>(&core.RevbUpBuf[0][index + 8]);
	r = GSVector4i::load<false>(&core.RevbUpBuf[1][index + 8]);
	lacc = lacc.adds16(l.mul16hrs(c));
	racc = racc.adds16(r.mul16hrs(c));

	c = GSVector4i::load<true>(&filter_up_coefs[16]);
	l = GSVector4i::load<false>(&core.RevbUpBuf[0][index + 16]);
	r = GSVector4i::load<false>(&core.RevbUpBuf[1][index + 16]);
	lacc = lacc.adds16(l.mul16hrs(c));
	racc = racc.adds16(r.mul16hrs(c));

	c = GSVector4i::load<true>(&filter_up_coefs[24]);
	l = GSVector4i::load<false>(&core.RevbUpBuf[0][index + 24]);
	r = GSVector4i::load<false>(&core.RevbUpBuf[1][index + 24]);
	lacc = lacc.adds16(l.mul16hrs(c));
	racc = racc.adds16(r.mul16hrs(c));

	c = GSVector4i::load<true>(&filter_up_coefs[32]);
	l = GSVector4i::load<false>(&core.RevbUpBuf[0][index + 32]);
	r = GSVector4i::load<false>(&core.RevbUpBuf[1][index + 32]);
	lacc = lacc.adds16(l.mul16hrs(c));
	racc = racc.adds16(r.mul16hrs(c));

	lacc = lacc.hadds16(lacc);
	lacc = lacc.hadds16(lacc);
	lacc = lacc.hadds16(lacc);

	racc = racc.hadds16(racc);
	racc = racc.hadds16(racc);
	racc = racc.hadds16(racc);

	return {lacc.I16[0], racc.I16[0]};
}
#endif

StereoOut32 ReverbUpsample(V_Core& core)
{
#if defined(ARCH_ARM32)
	return ReverbUpsample_neon(core);
#elif _M_SSE >= 0x501
	return ReverbUpsample_avx(core);
#else
	return ReverbUpsample_sse(core);
#endif
}

#if defined(VITASX2_QEMU_VALIDATION)
s32 ReverbDownsampleScalarForValidation(V_Core& core, bool right)
{
	return ReverbDownsample_reference(core, right);
}

StereoOut32 ReverbUpsampleScalarForValidation(V_Core& core)
{
	return ReverbUpsample_reference(core);
}
#endif

MULTI_ISA_UNSHARED_END
