// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "vita/VitaGpuVuDraw.h"

#include <atomic>
#include <limits>

namespace VitaGpuVu {
namespace {

std::atomic<u64> s_ordering_sequence{0};
std::atomic<u64> s_queued{0};
std::atomic<u64> s_consumed{0};
std::atomic<u64> s_rejected{0};
std::atomic<u64> s_generated_parallel_invocations{0};
std::atomic<u64> s_generated_serial_invocations{0};
std::atomic<u64> s_interpreter_invocations{0};
std::atomic<u64> s_fused_vertices{0};
std::atomic<u64> s_fused_primitives{0};
std::atomic<u64> s_tfx_vertex_exports{0};
std::atomic<u64> s_raw_path1_exports{0};
std::atomic<u64> s_retirement_batches{0};
std::atomic<u64> s_retired_draws{0};
std::atomic<u64> s_retirement_ring_waits{0};
std::atomic<u64> s_notification_waits{0};
std::atomic<u64> s_live_draws{0};
std::atomic<u64> s_peak_live_draws{0};

bool Fail(std::string *error, const char *message) {
  if (error)
    *error = message;
  return false;
}

void RecordLiveDraws(u64 count) {
  const u64 live =
      s_live_draws.fetch_add(count, std::memory_order_relaxed) + count;
  u64 peak = s_peak_live_draws.load(std::memory_order_relaxed);
  while (live > peak && !s_peak_live_draws.compare_exchange_weak(
                            peak, live, std::memory_order_relaxed,
                            std::memory_order_relaxed)) {
  }
}

void ReleaseLiveDraws(u64 count) {
  u64 live = s_live_draws.load(std::memory_order_relaxed);
  while (live != 0) {
    const u64 remaining = live > count ? live - count : 0;
    if (s_live_draws.compare_exchange_weak(live, remaining,
                                           std::memory_order_relaxed,
                                           std::memory_order_relaxed)) {
      break;
    }
  }
}

} // namespace

bool FinalStatePublication::IsRequired() const {
  return vf_mask != 0 || vi_mask != 0 || acc || q || p || i || flags || vif ||
         memory;
}

GpuVuDraw::~GpuVuDraw() {
  for (VifUnpackSpan &span : m_input_spans)
    ReleaseRawVifPayload(&span.payload);
}

bool GpuVuDraw::AddInputSpan(const VifUnpackSpan &span) {
  if (!span.payload.IsValid() || !RetainRawVifPayload(span.payload))
    return false;
  m_input_spans.push_back(span);
  return true;
}

bool GpuVuDraw::Validate(std::string *error) const {
  if (error)
    error->clear();
  if ((program.low == 0 && program.high == 0) || ordering_sequence == 0)
    return Fail(error, "missing generated-program key or ordering sequence");
  if (invocation_count == 0)
    return Fail(error, "empty GPU VU invocation count");
  if (lowering != OutputLowering::RawPath1Export &&
      (vertex_count == 0 || primitive_count == 0 || index_count == 0)) {
    return Fail(error, "empty GPU VU draw dimensions");
  }
  if (lowering == OutputLowering::DirectTfx &&
      (direct_tfx.vertex_count != vertex_count ||
       direct_tfx.vertex_count != invocation_count)) {
    return Fail(error, "direct TFX invocation/vertex contract mismatch");
  }
  if (streams.size() > 16)
    return Fail(error, "more than 16 generated vertex streams");

  u32 attribute_mask = 0;
  for (const StreamBinding &stream : streams) {
    if (stream.attribute_index >= 16 ||
        (attribute_mask & (1u << stream.attribute_index)) != 0) {
      return Fail(error, "duplicate or invalid generated attribute index");
    }
    attribute_mask |= 1u << stream.attribute_index;
    if (stream.input_span >= m_input_spans.size())
      return Fail(error, "generated stream refers to a missing VIF span");
    const VifUnpackSpan &span = m_input_spans[stream.input_span];
    if (!span.payload.IsValid() ||
        stream.payload_byte_offset > span.payload.size) {
      return Fail(error, "generated stream begins outside its VIF payload");
    }
    const u64 last_offset =
        static_cast<u64>(stream.payload_byte_offset) +
        static_cast<u64>(invocation_count - 1) * stream.byte_stride;
    if (last_offset > span.payload.size ||
        sizeof(u128) > span.payload.size - static_cast<u32>(last_offset)) {
      return Fail(error, "generated stream extends outside its VIF payload");
    }
  }

  u32 vf_mask = 0;
  for (const VectorUniform &uniform : vf_uniforms) {
    if (uniform.register_index == 0 || uniform.register_index >= 32 ||
        (vf_mask & (1u << uniform.register_index)) != 0) {
      return Fail(error, "duplicate or invalid VF uniform");
    }
    vf_mask |= 1u << uniform.register_index;
  }
  if (constant_uniforms.size() > 32)
    return Fail(error, "too many generated constant uniforms");
  u32 constant_mask = 0;
  for (const ConstantUniform &uniform : constant_uniforms) {
    if (uniform.input_index >= 32 ||
        (constant_mask & (1u << uniform.input_index)) != 0) {
      return Fail(error, "duplicate or invalid generated constant uniform");
    }
    constant_mask |= 1u << uniform.input_index;
  }
  if ((scalar_uniforms.present & ~(ScalarUniformQ | ScalarUniformP |
                                   ScalarUniformI | ScalarUniformGifQ)) != 0) {
    return Fail(error, "unknown scalar-uniform mask bit");
  }
  if (target_bounds.valid && (target_bounds.right <= target_bounds.left ||
                              target_bounds.bottom <= target_bounds.top)) {
    return Fail(error, "empty target bounds");
  }
  if (texture_bounds.valid && (texture_bounds.right <= texture_bounds.left ||
                               texture_bounds.bottom <= texture_bounds.top)) {
    return Fail(error, "empty texture bounds");
  }
  if (static_gs_writes.size() > std::numeric_limits<u16>::max())
    return Fail(error, "too many static GS writes");
  for (const StaticGsWrite &write : static_gs_writes) {
    if (write.address >= 0x80)
      return Fail(error, "static GS write address is outside GIF A+D");
  }
  return true;
}

u64 NextGpuVuOrderingSequence() {
  u64 sequence =
      s_ordering_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
  if (sequence == 0)
    sequence = s_ordering_sequence.fetch_add(1, std::memory_order_relaxed) + 1;
  return sequence;
}

bool IsDirectDrawAdmissionConnected() {
  // GSRendererHW cannot yet derive a draw configuration for vertexless
  // GpuVuDraw geometry. Keep the cold generated-root seam dormant until the
  // producer can replace, rather than duplicate, the CPU VU/PATH1 work.
  return false;
}

bool HasCompletedNotificationValue(u32 completed, u32 required) {
  return ((completed - required) & 0x80000000u) == 0;
}

void RecordGpuVuDrawQueued() {
  s_queued.fetch_add(1, std::memory_order_relaxed);
  RecordLiveDraws(1);
}

void RecordGpuVuDrawConsumed() {
  s_consumed.fetch_add(1, std::memory_order_relaxed);
}

void RecordGpuVuDrawRejected() {
  s_rejected.fetch_add(1, std::memory_order_relaxed);
  ReleaseLiveDraws(1);
}

void RecordGpuVuDrawExecuted(const GpuVuDraw &draw) {
  switch (draw.execution) {
  case ExecutionKind::GeneratedParallel:
    s_generated_parallel_invocations.fetch_add(draw.invocation_count,
                                               std::memory_order_relaxed);
    break;
  case ExecutionKind::GeneratedSerial:
    s_generated_serial_invocations.fetch_add(draw.invocation_count,
                                             std::memory_order_relaxed);
    break;
  case ExecutionKind::UniversalInterpreter:
    s_interpreter_invocations.fetch_add(draw.invocation_count,
                                        std::memory_order_relaxed);
    break;
  }
  switch (draw.lowering) {
  case OutputLowering::DirectTfx:
    s_fused_vertices.fetch_add(draw.vertex_count, std::memory_order_relaxed);
    s_fused_primitives.fetch_add(draw.primitive_count,
                                 std::memory_order_relaxed);
    break;
  case OutputLowering::TfxVertexExport:
    s_tfx_vertex_exports.fetch_add(draw.vertex_count,
                                   std::memory_order_relaxed);
    break;
  case OutputLowering::RawPath1Export:
    s_raw_path1_exports.fetch_add(1, std::memory_order_relaxed);
    break;
  }
}

void RecordGpuVuRetirementBatch() {
  s_retirement_batches.fetch_add(1, std::memory_order_relaxed);
}

void RecordGpuVuDrawsRetired(u64 count) {
  s_retired_draws.fetch_add(count, std::memory_order_relaxed);
  ReleaseLiveDraws(count);
}

void RecordGpuVuRetirementRingWait() {
  s_retirement_ring_waits.fetch_add(1, std::memory_order_relaxed);
}

void RecordGpuVuNotificationWait() {
  s_notification_waits.fetch_add(1, std::memory_order_relaxed);
}

DrawStatistics GetGpuVuDrawStatistics() {
  DrawStatistics stats;
  stats.queued = s_queued.load(std::memory_order_relaxed);
  stats.consumed = s_consumed.load(std::memory_order_relaxed);
  stats.rejected = s_rejected.load(std::memory_order_relaxed);
  stats.generated_parallel_invocations =
      s_generated_parallel_invocations.load(std::memory_order_relaxed);
  stats.generated_serial_invocations =
      s_generated_serial_invocations.load(std::memory_order_relaxed);
  stats.interpreter_invocations =
      s_interpreter_invocations.load(std::memory_order_relaxed);
  stats.fused_vertices = s_fused_vertices.load(std::memory_order_relaxed);
  stats.fused_primitives = s_fused_primitives.load(std::memory_order_relaxed);
  stats.tfx_vertex_exports =
      s_tfx_vertex_exports.load(std::memory_order_relaxed);
  stats.raw_path1_exports = s_raw_path1_exports.load(std::memory_order_relaxed);
  stats.retirement_batches =
      s_retirement_batches.load(std::memory_order_relaxed);
  stats.retired_draws = s_retired_draws.load(std::memory_order_relaxed);
  stats.retirement_ring_waits =
      s_retirement_ring_waits.load(std::memory_order_relaxed);
  stats.notification_waits =
      s_notification_waits.load(std::memory_order_relaxed);
  stats.live_draws = s_live_draws.load(std::memory_order_relaxed);
  stats.peak_live_draws = s_peak_live_draws.load(std::memory_order_relaxed);
  return stats;
}

} // namespace VitaGpuVu
