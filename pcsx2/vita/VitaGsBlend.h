// SPDX-FileCopyrightText: 2026 VitaSX2-NG Project
// SPDX-License-Identifier: GPL-3.0+
#pragma once

#include <cstdint>

namespace VitaGS
{
// GS ALPHA operands, not host BlendFactor/BlendOp. Keep this value independent
// of the representation used by any graphics API. GS manual 3.8; PCSX2
// GSDrawScanline.cpp::DrawScanline's AlphaBlend stage owns the arithmetic.
struct BlendOperation
{
	std::uint8_t a, b, c, d;
	std::uint8_t fix;
	bool pabe;
	bool enabled;
	bool clamp;

	constexpr bool ReadsDestination() const
	{
		return enabled && (d == 1 ||
			(a != b && (a == 1 || b == 1 || c == 1)));
	}
};
static_assert(sizeof(BlendOperation) == 8);

// Unclamped RGB result: dithering/format conversion/write masks are later
// operations. Negative products round down (signed >>7), not toward zero.
// This executable specification is also used by the GXM readback fixture;
// run_pcsx2_gs_blend_oracle.py checks it against the actual PCSX2 SIMD stage.
constexpr int BlendComponent(const BlendOperation& op, int source,
	int destination, int source_alpha, int destination_alpha)
{
	if (!op.enabled || (op.pabe && source_alpha < 128))
		return source;
	const int colors[3] = {source, destination, 0};
	const int alphas[3] = {source_alpha, destination_alpha, op.fix};
	const int product = (colors[op.a] - colors[op.b]) * alphas[op.c];
	const int scaled = product >= 0 ? product / 128 : -((-product + 127) / 128);
	return scaled + colors[op.d];
}

constexpr std::uint8_t StoreBlendComponent(const BlendOperation& op, int value)
{
	return op.clamp ? static_cast<std::uint8_t>(value < 0 ? 0 : (value > 255 ? 255 : value)) :
		static_cast<std::uint8_t>(value);
}
}
