// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "vita/VitaGpuVuGifContract.h"
#include "vita/VitaGpuVuProgramRegistry.h"
#include "vita/VitaGpuVuVifInput.h"

#include <atomic>
#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace VitaGpuVu {

// Phase-one direct descriptors are produced thousands of times per frame on a
// 496 MHz Cortex-A9. std::vector's empty object is cheap, but growing five
// separate vectors for every VU dispatch made allocator traffic part of the
// hot path. Keep the common generated-program shapes inline and retain a
// vector fallback so semantic coverage is not limited by an inline capacity.
template <typename T, size_t InlineCapacity>
class InlineDescriptorVector final {
  static_assert(InlineCapacity != 0);
  static_assert(std::is_nothrow_move_constructible_v<T>);
  static_assert(std::is_nothrow_move_assignable_v<T>);

public:
  InlineDescriptorVector() = default;
  InlineDescriptorVector(const InlineDescriptorVector&) = delete;
  InlineDescriptorVector& operator=(const InlineDescriptorVector&) = delete;
  InlineDescriptorVector(InlineDescriptorVector&&) = delete;
  InlineDescriptorVector& operator=(InlineDescriptorVector&&) = delete;

  size_t size() const { return m_size; }
  bool empty() const { return m_size == 0; }

  T* data() { return m_using_heap ? m_heap.data() : m_inline.data(); }
  const T* data() const {
    return m_using_heap ? m_heap.data() : m_inline.data();
  }
  T* begin() { return data(); }
  const T* begin() const { return data(); }
  T* end() { return data() + m_size; }
  const T* end() const { return data() + m_size; }

  T& operator[](size_t index) { return data()[index]; }
  const T& operator[](size_t index) const { return data()[index]; }
  T& back() { return (*this)[m_size - 1]; }
  const T& back() const { return (*this)[m_size - 1]; }

  void reserve(size_t requested) {
    if (requested > InlineCapacity)
      UseHeap(requested);
  }

  void push_back(const T& value) {
    EnsureSpaceForOne();
    if (m_using_heap)
      m_heap.push_back(value);
    else
      m_inline[m_size] = value;
    m_size++;
  }

  void push_back(T&& value) {
    EnsureSpaceForOne();
    if (m_using_heap)
      m_heap.push_back(std::move(value));
    else
      m_inline[m_size] = std::move(value);
    m_size++;
  }

  void clear() {
    if (m_using_heap)
      m_heap.clear();
    m_size = 0;
  }

private:
  void EnsureSpaceForOne() {
    if (!m_using_heap && m_size == InlineCapacity)
      UseHeap(InlineCapacity * 2);
  }

  void UseHeap(size_t requested) {
    if (m_using_heap) {
      m_heap.reserve(requested);
      return;
    }
    const size_t capacity =
        requested > InlineCapacity * 2 ? requested : InlineCapacity * 2;
    m_heap.reserve(capacity);
    for (size_t index = 0; index < m_size; index++)
      m_heap.push_back(std::move(m_inline[index]));
    m_using_heap = true;
  }

  std::array<T, InlineCapacity> m_inline{};
  std::vector<T> m_heap;
  size_t m_size = 0;
  bool m_using_heap = false;
};

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

struct ConstantUniform {
  std::array<u32, 4> bits{};
  u8 input_index = 0;
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

// Descriptor-scale register and constant inputs are immutable for one direct
// MSCAL/MSCNT chain when the resume slice has no current-memory constant
// inputs. Keep them in one allocation shared by every compact continuation
// descriptor instead of copying roughly 700 bytes for every 34-vertex chunk.
// A chain with dynamic resume-time constants receives a distinct block through
// the general builder, preserving the same semantic contract.
struct GpuVuUniformBlock final {
#if defined(__vita__)
  static void* operator new(std::size_t size);
  static void operator delete(void* pointer) noexcept;
  static void operator delete(void* pointer, std::size_t size) noexcept;
#endif

  void Retain() {
    m_references.fetch_add(1, std::memory_order_relaxed);
  }
  void Release() {
    if (m_references.fetch_sub(1, std::memory_order_acq_rel) == 1)
      delete this;
  }

  InlineDescriptorVector<ConstantUniform, 16> constant_uniforms;
  InlineDescriptorVector<VectorUniform, 16> vf_uniforms;

private:
  std::atomic<u32> m_references{1};
};

// Vita's libstdc++ may select non-atomic shared_ptr reference counts. This
// explicit intrusive owner crosses MTVU and GS with an architectural atomic
// count, matching PreparedProgramReference's ownership rule.
class GpuVuUniformBlockRef final {
public:
  GpuVuUniformBlockRef() = default;
  static GpuVuUniformBlockRef Adopt(GpuVuUniformBlock* block) {
    GpuVuUniformBlockRef result;
    result.m_block = block;
    return result;
  }
  explicit GpuVuUniformBlockRef(GpuVuUniformBlock* block)
      : m_block(block) {
    if (m_block)
      m_block->Retain();
  }
  GpuVuUniformBlockRef(const GpuVuUniformBlockRef& other)
      : GpuVuUniformBlockRef(other.m_block) {}
  GpuVuUniformBlockRef& operator=(const GpuVuUniformBlockRef& other) {
    if (this == &other)
      return *this;
    GpuVuUniformBlockRef replacement(other);
    Swap(replacement);
    return *this;
  }
  GpuVuUniformBlockRef(GpuVuUniformBlockRef&& other) noexcept
      : m_block(std::exchange(other.m_block, nullptr)) {}
  GpuVuUniformBlockRef& operator=(GpuVuUniformBlockRef&& other) noexcept {
    if (this == &other)
      return *this;
    GpuVuUniformBlockRef replacement(std::move(other));
    Swap(replacement);
    return *this;
  }
  ~GpuVuUniformBlockRef() {
    if (m_block)
      m_block->Release();
  }

  GpuVuUniformBlock* Get() const { return m_block; }
  explicit operator bool() const { return m_block != nullptr; }
  void Swap(GpuVuUniformBlockRef& other) noexcept {
    std::swap(m_block, other.m_block);
  }

private:
  GpuVuUniformBlock* m_block = nullptr;
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
// GS/GXM-owning thread. Input span references are retained by AddInputSpan();
// the underlying bytes remain owned until GPU vertex completion or rejection.
class GpuVuDraw final {
public:
  GpuVuDraw() = default;
  GpuVuDraw(const GpuVuDraw &) = delete;
  GpuVuDraw &operator=(const GpuVuDraw &) = delete;
  GpuVuDraw(GpuVuDraw &&) = delete;
  GpuVuDraw &operator=(GpuVuDraw &&) = delete;
  ~GpuVuDraw();

#if defined(__vita__)
  static void* operator new(std::size_t size);
  static void operator delete(void* pointer) noexcept;
  static void operator delete(void* pointer, std::size_t size) noexcept;
#endif

  bool AddInputSpan(const VifUnpackSpan &span);
  bool Validate(std::string *error) const;

  const InlineDescriptorVector<VifUnpackSpan, 4>& InputSpans() const {
    return m_input_spans;
  }

  ShaderKey program;
  DirectTfxContract direct_tfx;
  std::array<u32, 4> gif_tag{};
  InlineDescriptorVector<StreamBinding, 4> streams;
  // Entry-slice roots commonly lift one 4x4 matrix from sixteen fixed VU
  // qwords. Keep that complete descriptor set inline: promoting every MSCNT
  // continuation through malloc would put allocator traffic back into the
  // 496 MHz worker hot path.
  const InlineDescriptorVector<ConstantUniform, 16>& ConstantUniforms() const;
  const InlineDescriptorVector<VectorUniform, 16>& VfUniforms() const;
  const GpuVuUniformBlockRef& UniformBlock() const {
    return m_uniform_block;
  }
  void SetUniformBlock(GpuVuUniformBlockRef block) {
    m_uniform_block = std::move(block);
  }
  std::array<u32, 4> acc_uniform{};
  ScalarUniforms scalar_uniforms;
  std::array<std::array<float, 4>, 3> vertex_scale_offset{};
  float max_depth = 0.0f;
  InlineDescriptorVector<StaticGsWrite, 4> static_gs_writes;
  IntegerRect target_bounds;
  IntegerRect texture_bounds;
  // PairPlan-derived descriptor-scale exit state. MTVU publishes these VI
  // values after the immutable draw handoff succeeds, before it releases the
  // ordinary VU completion flag. They are not GPU readback requirements.
  std::array<u16, 16> final_vi_values{};
  u32 final_vi_write_mask = 0;
  FinalStatePublication final_state;
  u64 ordering_sequence = 0;
  u32 invocation_count = 0;
  u32 vertex_count = 0;
  u32 primitive_count = 0;
  u32 index_count = 0;
  OutputLowering lowering = OutputLowering::DirectTfx;
  ExecutionKind execution = ExecutionKind::GeneratedParallel;
  PrimitiveBoundary primitive_boundary = PrimitiveBoundary::Native;

  // VitaGsMailbox links consecutive direct PATH1 descriptors without a
  // per-dispatch allocation. The GS owner clears this before normal descriptor
  // validation and ownership transfer.
  GpuVuDraw* path1_next = nullptr;

private:
  // Generic affine vertex programs commonly use position, normal, texture,
  // and one auxiliary stream. Keeping four references inline avoids one heap
  // promotion per IGA-style three-stream dispatch on the 496 MHz MTVU core.
  InlineDescriptorVector<VifUnpackSpan, 4> m_input_spans;
  GpuVuUniformBlockRef m_uniform_block;
};

u64 NextGpuVuOrderingSequence();

// This becomes true only when CPU0 can retain the immutable VIF epoch, omit
// CpuVU1->Execute(), and queue its GpuVuDraw at the matching PATH1 position.
// Cold shader preparation must not consume MTVU time before that handoff
// exists.
bool IsDirectDrawAdmissionConnected();

// PhyreEngine's GXM resource contract: notification values are monotonically
// increasing modulo 2^32, and any later completed value retires an older one.
bool HasCompletedNotificationValue(u32 completed, u32 required);

// Why one VU1 dispatch could not become a direct GPU draw. These are semantic
// groups derived from the analysis and the invocation's own state, never a
// title, program, hash or instruction-sequence identity.
enum class AdmissionFailure {
  Disconnected,
  NoProgramToken,
  BuildFailed,
  QueueRejected,
  NoInputSpans,
  NoReadyCandidate,
  TagMismatch,
  SeedUnstable,
  GeometryFailed,
  InputResolveFailed,
};
inline constexpr size_t AdmissionFailureCount = 10;

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
  u64 descriptor_pool_waits = 0;
  u64 descriptor_pool_in_use = 0;
  u64 peak_descriptor_pool_in_use = 0;
  u32 descriptor_pool_capacity = 0;
  u32 descriptor_size = 0;
  u64 uniform_pool_waits = 0;
  u64 uniform_pool_in_use = 0;
  u64 peak_uniform_pool_in_use = 0;
  u32 uniform_pool_capacity = 0;
  u32 uniform_block_size = 0;
  u64 live_draws = 0;
  u64 peak_live_draws = 0;
  // Phase accounting: what the CPU still executed and still had to publish.
  u64 cpu_vu1_executions = 0;
  u64 cpu_path1_packets = 0;
  u64 cpu_path1_bytes = 0;
  u64 encoded_objects = 0;
  std::array<u64, 10> admission_failures{};
};

void RecordGpuVuDrawQueued();
void RecordGpuVuDrawConsumed();
void RecordGpuVuDrawRejected();
void RecordCpuVu1Execution(u32 path1_packet_bytes);
void RecordDirectAdmissionFailure(AdmissionFailure reason);
void RecordGpuVuObjectsEncoded(u64 count);
void RecordGpuVuDrawExecuted(const GpuVuDraw &draw);
void RecordGpuVuRetirementBatch();
void RecordGpuVuDrawsRetired(u64 count);
void RecordGpuVuRetirementRingWait();
void RecordGpuVuNotificationWait();
DrawStatistics GetGpuVuDrawStatistics();

} // namespace VitaGpuVu
