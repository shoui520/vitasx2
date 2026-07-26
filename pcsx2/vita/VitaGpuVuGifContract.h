// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "vita/VitaGpuVuLoopKernel.h"

#include <array>
#include <string>

namespace VitaGpuVu {

struct PackedIntegerExpression {
  u32 expression = 0;
  u32 mask = 0;
  u8 right_shift = 0;
};

struct DirectTfxContract {
  u32 vertex_count = 0;
  u32 primitive = 0;
  u8 output_base_vi = 0;
  s32 output_base_qword = 0;
  std::array<u32, 2> st{};
  std::array<PackedIntegerExpression, 4> color{};
  u32 q = 0;
  std::array<PackedIntegerExpression, 2> position{};
  PackedIntegerExpression depth;
  std::array<PackedIntegerExpression, 2> uv{};
  PackedIntegerExpression fog;
  u32 adc_expression = 0;
  bool gouraud = false;
  bool textured = false;
  bool fog_enabled = false;
  bool fixed_texture_coordinates = false;
  bool adc_always_clear = false;
  bool has_color = false;
  bool has_stq = false;
  bool has_uv = false;
  bool has_position = false;
};

// Matches one static packed GIF tag to the loop's affine qword stores. The
// proof uses only decoded tag semantics and expression/address relationships;
// it has no program, title, address, or instruction-sequence recognition.
bool BuildDirectTfxContract(const ParallelLoopKernel &kernel,
                            const void *gif_tag_qword,
                            DirectTfxContract *contract,
                            std::string *error);

} // namespace VitaGpuVu
