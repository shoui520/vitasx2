// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "vita/VitaGpuVuDraw.h"

#include "common/Assertions.h"
#include "common/Threading.h"

#include <atomic>
#include <cstddef>
#include <limits>
#include <new>

namespace VitaGpuVu {
namespace {

std::atomic<u64> s_ordering_sequence{0};
std::atomic<u64> s_queued{0};
std::atomic<u64> s_consumed{0};
std::atomic<u64> s_rejected{0};
// Phase accounting. These separate "the GPU root exists" from "every dispatch
// which could use it actually does", which is the only way to tell a draining
// CPU PATH1 backlog from one that is still being refilled.
std::atomic<u64> s_cpu_vu1_executions{0};
std::atomic<u64> s_cpu_path1_packets{0};
std::atomic<u64> s_cpu_path1_bytes{0};
std::array<std::atomic<u64>, VitaGpuVu::AdmissionFailureCount>
    s_admission_failures{};
std::atomic<u64> s_encoded_objects{0};
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
std::atomic<u64> s_descriptor_pool_waits{0};
std::atomic<u64> s_descriptor_pool_in_use{0};
std::atomic<u64> s_peak_descriptor_pool_in_use{0};
std::atomic<u64> s_uniform_pool_waits{0};
std::atomic<u64> s_uniform_pool_in_use{0};
std::atomic<u64> s_peak_uniform_pool_in_use{0};
std::atomic<u64> s_live_draws{0};
std::atomic<u64> s_peak_live_draws{0};

#if defined(__vita__) && defined(VITASX2_GPU_VU_DIRECT_ADMISSION) && \
    VITASX2_GPU_VU_DIRECT_ADMISSION
// The EE can have thousands of PATH1 reservations queued while the GS worker
// is still draining CPU-generated packets. A heap object per admitted VU
// dispatch therefore made descriptor ownership unbounded and exhausted
// newlib before the first GPU descriptor reached the GS thread. Two complete
// 256-draw mailbox runs are enough to keep producer and consumer concurrent;
// reuse sleeps only when both older runs are still owned by the mailbox/GS.
constexpr u32 GpuVuDrawPoolCapacity = 512;
// One active continuation seed can retain a block after every descriptor that
// referenced it has left the 512-slot draw pool. Leave a small bounded margin
// for that seed and for the explicit-entry block under construction.
constexpr u32 GpuVuUniformBlockPoolCapacity = 520;

template <typename Object, u32 Capacity>
class GpuVuObjectPool final {
public:
  GpuVuObjectPool(std::atomic<u64> &waits,
                  std::atomic<u64> &in_use,
                  std::atomic<u64> &peak_in_use)
      : m_waits(waits), m_in_use(in_use), m_peak_in_use(peak_in_use) {
    static_assert(Capacity != 0);
    static_assert(Capacity <= std::numeric_limits<u16>::max());
    for (u32 index = 0; index < Capacity; index++) {
      m_next[index] =
          static_cast<u16>(index + 1 < Capacity ? index + 2 : 0);
      m_allocated[index].store(0, std::memory_order_relaxed);
    }
    m_free_head.store(1, std::memory_order_relaxed);
  }

  void* Acquire() {
    bool counted_wait = false;
    for (;;) {
      u32 index = 0;
      if (TryPop(&index))
        return AccountAcquire(index);

      if (!counted_wait) {
        m_waits.fetch_add(1, std::memory_order_relaxed);
        counted_wait = true;
      }

      // The free-list is the resource authority. The semaphore is only an
      // empty-list wakeup, so a delayed/stale wake can at worst cause another
      // loop and can never grant a slot which is still owned. Publish the
      // sleeping intent before retrying the list to close the lost-wake race.
      m_waiting.store(true, std::memory_order_release);
      if (TryPop(&index)) {
        m_waiting.store(false, std::memory_order_release);
        return AccountAcquire(index);
      }
      m_available.Wait();
    }
  }

  void Release(void* pointer) {
    const uptr first = reinterpret_cast<uptr>(m_storage.data());
    const uptr address = reinterpret_cast<uptr>(pointer);
    const uptr bytes = sizeof(Object) * Capacity;
    const bool valid =
        address >= first && address < first + bytes &&
        ((address - first) % sizeof(Object)) == 0;
    pxAssertRel(valid, "foreign pointer returned to GPU-VU object pool");
    if (!valid)
      return;

    const u32 index =
        static_cast<u32>((address - first) / sizeof(Object));
    const bool slot_was_owned =
        m_allocated[index].exchange(0, std::memory_order_acq_rel) != 0;

    pxAssertRel(slot_was_owned,
                "GPU-VU pooled object released more than once");
    if (!slot_was_owned)
      return;

    const u64 previous =
        m_in_use.fetch_sub(1, std::memory_order_relaxed);
    pxAssertRel(previous != 0, "GPU-VU pool usage counter underflow");

    // Only the MTVU producer pops. GS retirement and rejected MTVU builds can
    // push concurrently, so use a multi-producer/single-consumer Treiber
    // stack. A slot cannot re-enter the list before its unique owner releases
    // it, which removes the ABA case that requires tagged pointers in a fully
    // multi-consumer stack.
    u32 head = m_free_head.load(std::memory_order_relaxed);
    do {
      m_next[index] = static_cast<u16>(head);
    } while (!m_free_head.compare_exchange_weak(
        head, index + 1, std::memory_order_release,
        std::memory_order_relaxed));

    if (m_waiting.exchange(false, std::memory_order_acq_rel))
      m_available.Post();
  }

private:
  bool TryPop(u32* index) {
    u32 head = m_free_head.load(std::memory_order_acquire);
    while (head != 0) {
      const u32 candidate = head - 1;
      const u32 next = m_next[candidate];
      if (m_free_head.compare_exchange_weak(
              head, next, std::memory_order_acq_rel,
              std::memory_order_acquire)) {
        const bool slot_was_free =
            m_allocated[candidate].exchange(
                1, std::memory_order_acq_rel) == 0;
        pxAssertRel(slot_was_free,
                    "GPU-VU pool acquired an owned object");
        if (!slot_was_free)
          return false;
        *index = candidate;
        return true;
      }
    }
    return false;
  }

  void* AccountAcquire(u32 index) {
    const u64 in_use =
        m_in_use.fetch_add(1, std::memory_order_relaxed) + 1;
    u64 peak = m_peak_in_use.load(std::memory_order_relaxed);
    while (in_use > peak &&
           !m_peak_in_use.compare_exchange_weak(
               peak, in_use, std::memory_order_relaxed,
               std::memory_order_relaxed)) {
    }
    return m_storage.data() + sizeof(Object) * index;
  }

  alignas(Object)
      std::array<std::byte, sizeof(Object) * Capacity> m_storage{};
  std::array<u16, Capacity> m_next{};
  std::array<std::atomic<u8>, Capacity> m_allocated{};
  std::atomic<u32> m_free_head{0};
  std::atomic<bool> m_waiting{false};
  Threading::KernelSemaphore m_available;
  std::atomic<u64>& m_waits;
  std::atomic<u64>& m_in_use;
  std::atomic<u64>& m_peak_in_use;
};

GpuVuObjectPool<GpuVuDraw, GpuVuDrawPoolCapacity>
    s_gpu_vu_draw_pool(s_descriptor_pool_waits,
                       s_descriptor_pool_in_use,
                       s_peak_descriptor_pool_in_use);
GpuVuObjectPool<GpuVuUniformBlock, GpuVuUniformBlockPoolCapacity>
    s_gpu_vu_uniform_pool(s_uniform_pool_waits,
                          s_uniform_pool_in_use,
                          s_peak_uniform_pool_in_use);
#else
constexpr u32 GpuVuDrawPoolCapacity = 0;
constexpr u32 GpuVuUniformBlockPoolCapacity = 0;
#endif

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
  for (RawVifPayloadRef &payload : m_input_payloads)
    ReleaseRawVifPayload(&payload);
}

#if defined(__vita__)
void* GpuVuUniformBlock::operator new(std::size_t size) {
  pxAssertRel(size == sizeof(GpuVuUniformBlock),
              "invalid GPU-VU uniform allocation size");
#if defined(VITASX2_GPU_VU_DIRECT_ADMISSION) && \
    VITASX2_GPU_VU_DIRECT_ADMISSION
  void* const pointer = s_gpu_vu_uniform_pool.Acquire();
  if (pointer)
    return pointer;
  throw std::bad_alloc();
#else
  return ::operator new(size);
#endif
}

void GpuVuUniformBlock::operator delete(void* pointer) noexcept {
  if (!pointer)
    return;
#if defined(VITASX2_GPU_VU_DIRECT_ADMISSION) && \
    VITASX2_GPU_VU_DIRECT_ADMISSION
  s_gpu_vu_uniform_pool.Release(pointer);
#else
  ::operator delete(pointer);
#endif
}

void GpuVuUniformBlock::operator delete(
    void* pointer, std::size_t size) noexcept {
  pxAssertRel(size == sizeof(GpuVuUniformBlock),
              "invalid GPU-VU uniform deletion size");
  GpuVuUniformBlock::operator delete(pointer);
}

void* GpuVuDraw::operator new(std::size_t size) {
  pxAssertRel(size == sizeof(GpuVuDraw),
              "invalid GPU-VU descriptor allocation size");
#if defined(VITASX2_GPU_VU_DIRECT_ADMISSION) && \
    VITASX2_GPU_VU_DIRECT_ADMISSION
  void* const pointer = s_gpu_vu_draw_pool.Acquire();
  if (pointer)
    return pointer;
  throw std::bad_alloc();
#else
  return ::operator new(size);
#endif
}

void GpuVuDraw::operator delete(void* pointer) noexcept {
  if (!pointer)
    return;
#if defined(VITASX2_GPU_VU_DIRECT_ADMISSION) && \
    VITASX2_GPU_VU_DIRECT_ADMISSION
  s_gpu_vu_draw_pool.Release(pointer);
#else
  ::operator delete(pointer);
#endif
}

void GpuVuDraw::operator delete(
    void* pointer, std::size_t size) noexcept {
  pxAssertRel(size == sizeof(GpuVuDraw),
              "invalid GPU-VU descriptor deletion size");
  GpuVuDraw::operator delete(pointer);
}
#endif

bool GpuVuDraw::AddInputPayload(const RawVifPayloadRef &payload) {
  if (!payload.IsValid() || !RetainRawVifPayload(payload))
    return false;
  m_input_payloads.push_back(payload);
  return true;
}

const InlineDescriptorVector<ConstantUniform, 16>&
GpuVuDraw::ConstantUniforms() const {
  static const InlineDescriptorVector<ConstantUniform, 16> empty;
  return m_uniform_block ? m_uniform_block.Get()->constant_uniforms : empty;
}

const InlineDescriptorVector<VectorUniform, 16>&
GpuVuDraw::VfUniforms() const {
  static const InlineDescriptorVector<VectorUniform, 16> empty;
  return m_uniform_block ? m_uniform_block.Get()->vf_uniforms : empty;
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
    if (stream.input_span >= m_input_payloads.size())
      return Fail(error, "generated stream refers to a missing VIF span");
    const RawVifPayloadRef &payload = m_input_payloads[stream.input_span];
    if (!payload.IsValid() ||
        stream.payload_byte_offset > payload.size) {
      return Fail(error, "generated stream begins outside its VIF payload");
    }
    const u64 last_offset =
        static_cast<u64>(stream.payload_byte_offset) +
        static_cast<u64>(invocation_count - 1) * stream.byte_stride;
    if (last_offset > payload.size ||
        sizeof(u128) > payload.size - static_cast<u32>(last_offset)) {
      return Fail(error, "generated stream extends outside its VIF payload");
    }
  }

  u32 vf_mask = 0;
  for (const VectorUniform &uniform : VfUniforms()) {
    if (uniform.register_index == 0 || uniform.register_index >= 32 ||
        (vf_mask & (1u << uniform.register_index)) != 0) {
      return Fail(error, "duplicate or invalid VF uniform");
    }
    vf_mask |= 1u << uniform.register_index;
  }
  if (ConstantUniforms().size() > 32)
    return Fail(error, "too many generated constant uniforms");
  u32 constant_mask = 0;
  for (const ConstantUniform &uniform : ConstantUniforms()) {
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
  if ((final_vi_write_mask & ~0xfffeu) != 0)
    return Fail(error, "invalid descriptor-scale final VI mask");
  if (final_vi_values[0] != 0)
    return Fail(error, "descriptor-scale final VI0 is not zero");
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

bool GpuVuDraw::ValidateForQueue(std::string *error) {
  if (m_validated_for_queue)
    return true;
  if (!Validate(error))
    return false;
  m_validated_for_queue = true;
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
#if defined(VITASX2_GPU_VU_DIRECT_ADMISSION) && \
    VITASX2_GPU_VU_DIRECT_ADMISSION
  return true;
#else
  // The disconnected product compiles out VIF capture and all hot-path
  // preparation. Admission builds enable the MTVU/GS ownership contract as
  // one process-wide boundary.
  return false;
#endif
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

void RecordCpuVu1Execution(u32 path1_packet_bytes) {
  s_cpu_vu1_executions.fetch_add(1, std::memory_order_relaxed);
  if (path1_packet_bytes != 0) {
    s_cpu_path1_packets.fetch_add(1, std::memory_order_relaxed);
    s_cpu_path1_bytes.fetch_add(path1_packet_bytes, std::memory_order_relaxed);
  }
}

void RecordDirectAdmissionFailure(AdmissionFailure reason) {
  const size_t index = static_cast<size_t>(reason);
  if (index < s_admission_failures.size())
    s_admission_failures[index].fetch_add(1, std::memory_order_relaxed);
}

void RecordGpuVuObjectsEncoded(u64 count) {
  s_encoded_objects.fetch_add(count, std::memory_order_relaxed);
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
  stats.cpu_vu1_executions =
      s_cpu_vu1_executions.load(std::memory_order_relaxed);
  stats.cpu_path1_packets = s_cpu_path1_packets.load(std::memory_order_relaxed);
  stats.cpu_path1_bytes = s_cpu_path1_bytes.load(std::memory_order_relaxed);
  for (size_t index = 0; index < s_admission_failures.size(); index++) {
    stats.admission_failures[index] =
        s_admission_failures[index].load(std::memory_order_relaxed);
  }
  stats.encoded_objects = s_encoded_objects.load(std::memory_order_relaxed);
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
  stats.descriptor_pool_waits =
      s_descriptor_pool_waits.load(std::memory_order_relaxed);
  stats.descriptor_pool_in_use =
      s_descriptor_pool_in_use.load(std::memory_order_relaxed);
  stats.peak_descriptor_pool_in_use =
      s_peak_descriptor_pool_in_use.load(std::memory_order_relaxed);
  stats.descriptor_pool_capacity = GpuVuDrawPoolCapacity;
  stats.descriptor_size = sizeof(GpuVuDraw);
  stats.uniform_pool_waits =
      s_uniform_pool_waits.load(std::memory_order_relaxed);
  stats.uniform_pool_in_use =
      s_uniform_pool_in_use.load(std::memory_order_relaxed);
  stats.peak_uniform_pool_in_use =
      s_peak_uniform_pool_in_use.load(std::memory_order_relaxed);
  stats.uniform_pool_capacity = GpuVuUniformBlockPoolCapacity;
  stats.uniform_block_size = sizeof(GpuVuUniformBlock);
  stats.live_draws = s_live_draws.load(std::memory_order_relaxed);
  stats.peak_live_draws = s_peak_live_draws.load(std::memory_order_relaxed);
  return stats;
}

} // namespace VitaGpuVu
