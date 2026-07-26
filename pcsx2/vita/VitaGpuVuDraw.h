// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "vita/VitaGpuVuGifContract.h"
#include "vita/VitaGpuVuProgramRegistry.h"
#include "vita/VitaGpuVuVifInput.h"

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace VitaGpuVu {

enum class OutputLowering : u8 {
  DirectTfx,
  TfxVertexExport,
  RawPath1Export,
};

enum class ExecutionKind : u8 {
  GeneratedParallel,
  GeneratedSerial,
  UniversalInterpreter,
};

enum class PrimitiveBoundary : u8 {
  Native,
  InstanceIndexed,
  IndexWrap,
};

struct IntegerRect {
  s32 left = 0;
  s32 top = 0;
  s32 right = 0;
  s32 bottom = 0;
  bool valid = false;
  bool exact = false;
};

// One statically proven GS register write at this PATH1 ordering point. Dynamic
// A+D output is represented only by RawPath1Export.
struct StaticGsWrite {
  u64 value = 0;
  u8 address = 0;
};

struct StreamBinding {
  u32 payload_byte_offset = 0;
  u32 byte_stride = 0;
  u16 input_span = 0;
  u8 attribute_index = 0;
  u8 reserved = 0;
};

struct VectorUniform {
  std::array<u32, 4> bits{};
  u8 register_index = 0;
};

enum ScalarUniformMask : u32 {
  ScalarUniformQ = 1u << 0,
  ScalarUniformP = 1u << 1,
  ScalarUniformI = 1u << 2,
  ScalarUniformGifQ = 1u << 3,
};

struct ScalarUniforms {
  u32 present = 0;
  u32 q = 0;
  u32 p = 0;
  u32 i = 0;
  u32 gif_q = 0;
};

struct FinalStatePublication {
  u32 vf_mask = 0;
  u32 vi_mask = 0;
  bool acc = false;
  bool q = false;
  bool p = false;
  bool i = false;
  bool flags = false;
  bool vif = false;
  bool memory = false;

  bool IsRequired() const;
};

// Immutable, sequence-numbered handoff from the EE/VIF producer to the
// GS/GXM-owning thread. Input span references are retained exactly once by
// AddInputSpan() and released only after GPU vertex completion or rejection.
class GpuVuDraw final {
public:
  GpuVuDraw() = default;
  GpuVuDraw(const GpuVuDraw &) = delete;
  GpuVuDraw &operator=(const GpuVuDraw &) = delete;
  GpuVuDraw(GpuVuDraw &&) = delete;
  GpuVuDraw &operator=(GpuVuDraw &&) = delete;
  ~GpuVuDraw();

  bool AddInputSpan(const VifUnpackSpan &span);
  bool Validate(std::string *error) const;

  const std::vector<VifUnpackSpan> &InputSpans() const { return m_input_spans; }

  ShaderKey program;
  DirectTfxContract direct_tfx;
  std::array<u32, 4> gif_tag{};
  std::vector<StreamBinding> streams;
  std::vector<VectorUniform> vf_uniforms;
  std::array<u32, 4> acc_uniform{};
  ScalarUniforms scalar_uniforms;
  std::array<std::array<float, 4>, 3> vertex_scale_offset{};
  float max_depth = 0.0f;
  std::vector<StaticGsWrite> static_gs_writes;
  IntegerRect target_bounds;
  IntegerRect texture_bounds;
  FinalStatePublication final_state;
  u64 ordering_sequence = 0;
  u32 invocation_count = 0;
  u32 vertex_count = 0;
  u32 primitive_count = 0;
  u32 index_count = 0;
  OutputLowering lowering = OutputLowering::DirectTfx;
  ExecutionKind execution = ExecutionKind::GeneratedParallel;
  PrimitiveBoundary primitive_boundary = PrimitiveBoundary::Native;

private:
  std::vector<VifUnpackSpan> m_input_spans;
};

u64 NextGpuVuOrderingSequence();

// PhyreEngine's GXM resource contract: notification values are monotonically
// increasing modulo 2^32, and any later completed value retires an older one.
bool HasCompletedNotificationValue(u32 completed, u32 required);

struct DrawStatistics {
  u64 queued = 0;
  u64 consumed = 0;
  u64 rejected = 0;
  u64 generated_parallel_invocations = 0;
  u64 generated_serial_invocations = 0;
  u64 interpreter_invocations = 0;
  u64 fused_vertices = 0;
  u64 fused_primitives = 0;
  u64 tfx_vertex_exports = 0;
  u64 raw_path1_exports = 0;
  u64 retirement_batches = 0;
  u64 retired_draws = 0;
  u64 retirement_ring_waits = 0;
  u64 notification_waits = 0;
  u64 live_draws = 0;
  u64 peak_live_draws = 0;
};

void RecordGpuVuDrawQueued();
void RecordGpuVuDrawConsumed();
void RecordGpuVuDrawRejected();
void RecordGpuVuDrawExecuted(const GpuVuDraw &draw);
void RecordGpuVuRetirementBatch();
void RecordGpuVuDrawsRetired(u64 count);
void RecordGpuVuRetirementRingWait();
void RecordGpuVuNotificationWait();
DrawStatistics GetGpuVuDrawStatistics();

} // namespace VitaGpuVu
