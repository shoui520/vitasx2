// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#if defined(ARCH_ARM32)
#include <arm_neon.h>
#endif

namespace VitaVU
{
	// Maximum-tier VU1 arithmetic. ARM ARM A2 "Reciprocal estimate and step"
	// specifies that VRECPE supplies an estimate in 1/256 units and that
	// x1=x0*(2-d*x0) is one Newton-Raphson refinement. The corresponding square-
	// root iteration is x1=x0*(3-d*x0*x0)/2. The Q and P helpers retain that
	// refinement. Cortex-A9 MPE TRM table 3-8 shows that Q's dependent VRECPS plus
	// multiply chain is not by itself lower latency than scalar VDIV.F32; the JIT
	// obtains its saving from paired inputs, fast normalization, and direct stores.
	//
	// The generated JIT emits these operations directly. These helpers give
	// VUops/VUmicroFast fallback seams the identical hardware arithmetic.
	static inline float ApproximateReciprocal(float value)
	{
#if defined(ARCH_ARM32)
		const float32x2_t operand = vdup_n_f32(value);
		float32x2_t estimate = vrecpe_f32(operand);
		estimate = vmul_f32(estimate, vrecps_f32(operand, estimate));
		return vget_lane_f32(estimate, 0);
#else
		return 1.0f / value;
#endif
	}

	static inline float ApproximateDivide(float numerator, float denominator)
	{
#if defined(ARCH_ARM32)
		float32x2_t inputs = vdup_n_f32(numerator);
		inputs = vset_lane_f32(denominator, inputs, 1);
		const float32x2_t estimate = vrecpe_f32(inputs);
		const float32x2_t step = vrecps_f32(inputs, estimate);
		const float32x2_t scaled = vmul_lane_f32(inputs, estimate, 1);
		return vget_lane_f32(vmul_lane_f32(scaled, step, 1), 0);
#else
		return numerator / denominator;
#endif
	}

	static inline float ApproximateReciprocalSqrt(float value)
	{
#if defined(ARCH_ARM32)
		const float32x2_t operand = vdup_n_f32(value);
		float32x2_t estimate = vrsqrte_f32(operand);
		const float32x2_t step = vrsqrts_f32(operand, vmul_f32(estimate, estimate));
		estimate = vmul_f32(estimate, step);
		return vget_lane_f32(estimate, 0);
#else
		return 1.0f / __builtin_sqrtf(value);
#endif
	}

	static inline float ApproximateSqrt(float value)
	{
#if defined(ARCH_ARM32)
		const float32x2_t operand = vdup_n_f32(value);
		float32x2_t estimate = vrsqrte_f32(operand);
		const float32x2_t step = vrsqrts_f32(operand, vmul_f32(estimate, estimate));
		estimate = vmul_f32(estimate, step);
		return vget_lane_f32(vmul_f32(operand, estimate), 0);
#else
		return __builtin_sqrtf(value);
#endif
	}
} // namespace VitaVU
