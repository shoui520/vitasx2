// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "vita/VitaGpuVuGifContract.h"
#include "vita/VitaGpuVuLoopKernel.h"

#include <string>
#include <vector>

namespace VitaGpuVu {

struct CgMemoryInput {
  AffineQwordAddress address;
  u32 attribute_index = 0;
  // Bit N means the flat instance root declares VertexN for this logical
  // stream. Geometry dependencies require every primitive lane; a
  // color-only dependency requires only the final/provoking lane.
  u8 flat_attribute_vertex_mask = 0;
};

// One descriptor-scale VU-memory qword which is invariant across every
// invocation in the draw. It is uploaded once through the vertex default
// uniform buffer, never repeated as a per-vertex stream.
struct CgConstantInput {
  AffineQwordAddress address;
  u32 uniform_index = 0;
};

struct GeneratedCgProgram {
  std::string source;
  std::vector<CgMemoryInput> memory_inputs;
  std::vector<CgConstantInput> constant_inputs;
  u32 vf_uniform_mask = 0;
  std::array<u8, 32> stable_initial_vf_lanes{};
  u8 stable_initial_acc_lanes = 0;
  u32 emitted_expression_count = 0;
  u8 flat_vertices_per_primitive = 0;
  u8 flat_instance_vertex_step = 0;
  bool stable_initial_q = false;
  bool stable_initial_p = false;
  bool stable_initial_i = false;
  bool requires_dynamic_entry_state = false;
  bool uses_acc_uniform = false;
  bool uses_q_uniform = false;
  bool uses_p_uniform = false;
  bool uses_i_uniform = false;
  bool uses_gif_q_uniform = false;
  bool uses_tfx_uniforms = false;
  bool uses_flat_instance_inputs = false;
  bool flat_strip_winding = false;
};

// Emits a vertex program which evaluates the proven parallel semantic slice
// and exposes each VU qword store as a varying. This is the debug/oracle root
// used to inspect ShaccCg allocation and compare generated values. The final
// fused root reuses the same expression emitter and replaces these store
// varyings with the existing TFX tail.
bool GenerateParallelStoreValidationCg(const ParallelLoopKernel &kernel,
                                       GeneratedCgProgram *program,
                                       std::string *error);

// Emits the primary playable root: the VU semantic slice feeds the existing
// TFX vertex tail directly, with no VU output buffer or CPU vertex expansion.
bool GenerateParallelTfxCg(const ParallelLoopKernel &kernel,
                           const DirectTfxContract &contract,
                           GeneratedCgProgram *program,
                           std::string *error);

} // namespace VitaGpuVu
