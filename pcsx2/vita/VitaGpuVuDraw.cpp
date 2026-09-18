// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "vita/VitaGpuVuDraw.h"
#include "vita/VitaGpuVuGeneratedUniversal.h"
#include "GS/GSRegs.h"

#include "common/Assertions.h"
#include "common/Threading.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <limits>
#include <mutex>
#include <new>

namespace VitaGpuVu {

bool RebaseExactIndicesForGpuVuBatch(const u16* local_indices,
    size_t index_count, u32 invocation_count, u32 object_index,
    u16* global_indices) {
  return RebaseExactIndicesForGpuVuVariableBatch(
      local_indices, index_count, invocation_count, invocation_count,
      object_index, global_indices);
}

bool RebaseExactIndicesForGpuVuVariableBatch(const u16* local_indices,
    size_t index_count, u32 active_invocation_count,
    u32 batch_invocation_stride, u32 object_index, u16* global_indices) {
  if (!local_indices || !global_indices || index_count == 0u ||
      active_invocation_count == 0u || batch_invocation_stride == 0u ||
      active_invocation_count > batch_invocation_stride)
    return false;
  const u64 vertex_base =
      static_cast<u64>(object_index) * batch_invocation_stride;
  if (vertex_base + batch_invocation_stride > (1ull << 16u))
    return false;
  for (size_t index = 0u; index < index_count; index++) {
    if (local_indices[index] >= active_invocation_count)
      return false;
  }
  for (size_t index = 0u; index < index_count; index++) {
    global_indices[index] =
        static_cast<u16>(vertex_base + local_indices[index]);
  }
  return true;
}

bool ComputeGeneratedLoopKernelPrivateOutputBatchLayout(
    u32 object_count, u32 capacity_invocations,
    u32 stores_per_invocation, bool has_ftoi_probe, u32 guard_bytes,
    GeneratedLoopKernelPrivateOutputBatchLayout* layout) {
  if (!layout || object_count == 0u || capacity_invocations == 0u ||
      stores_per_invocation == 0u || guard_bytes == 0u)
    return false;

  const auto multiply = [](u64 left, u64 right, u64* product) {
    if (!product || (right != 0u &&
                     left > std::numeric_limits<u64>::max() / right))
      return false;
    *product = left * right;
    return true;
  };
  const auto add = [](u64 left, u64 right, u64* sum) {
    if (!sum || left > std::numeric_limits<u64>::max() - right)
      return false;
    *sum = left + right;
    return true;
  };
  u64 payload_entries = 0u;
  u64 payload_stride = 0u;
  u64 probe_stride = 0u;
  u64 payload_total = 0u;
  u64 probe_total = 0u;
  u64 guard_offset = 0u;
  u64 allocation = 0u;
  if (!multiply(capacity_invocations, stores_per_invocation,
                &payload_entries) ||
      !multiply(payload_entries, 4u * sizeof(u32), &payload_stride) ||
      (has_ftoi_probe &&
       !multiply(capacity_invocations, 8u * sizeof(u32), &probe_stride)) ||
      !multiply(payload_stride, object_count, &payload_total) ||
      !multiply(probe_stride, object_count, &probe_total) ||
      !add(payload_total, probe_total, &guard_offset) ||
      !add(guard_offset, guard_bytes, &allocation)) {
    return false;
  }
  if (payload_stride > std::numeric_limits<u32>::max() ||
      probe_stride > std::numeric_limits<u32>::max() ||
      payload_total > std::numeric_limits<u32>::max() ||
      probe_total > std::numeric_limits<u32>::max() ||
      guard_offset > std::numeric_limits<u32>::max() ||
      allocation > std::numeric_limits<u32>::max()) {
    return false;
  }

  GeneratedLoopKernelPrivateOutputBatchLayout resolved;
  resolved.object_count = object_count;
  resolved.capacity_invocations = capacity_invocations;
  resolved.stores_per_invocation = stores_per_invocation;
  resolved.payload_stride_bytes = static_cast<u32>(payload_stride);
  resolved.probe_stride_bytes = static_cast<u32>(probe_stride);
  resolved.payload_total_bytes = static_cast<u32>(payload_total);
  resolved.probe_total_bytes = static_cast<u32>(probe_total);
  resolved.guard_offset_bytes = static_cast<u32>(guard_offset);
  resolved.allocation_bytes = static_cast<u32>(allocation);
  *layout = resolved;
  return true;
}

bool ResolveGeneratedLoopKernelPrivateOutputBatchSlice(
    const GeneratedLoopKernelPrivateOutputBatchLayout& layout,
    u32 object_index, u32 active_invocations,
    GeneratedLoopKernelPrivateOutputSlice* slice) {
  if (!slice || layout.object_count == 0u ||
      layout.capacity_invocations == 0u ||
      layout.stores_per_invocation == 0u ||
      object_index >= layout.object_count || active_invocations == 0u ||
      active_invocations > layout.capacity_invocations) {
    return false;
  }

  if (layout.allocation_bytes <= layout.guard_offset_bytes)
    return false;
  GeneratedLoopKernelPrivateOutputBatchLayout expected;
  if (!ComputeGeneratedLoopKernelPrivateOutputBatchLayout(
          layout.object_count, layout.capacity_invocations,
          layout.stores_per_invocation,
          layout.probe_stride_bytes != 0u,
          layout.allocation_bytes - layout.guard_offset_bytes, &expected) ||
      expected.payload_stride_bytes != layout.payload_stride_bytes ||
      expected.probe_stride_bytes != layout.probe_stride_bytes ||
      expected.payload_total_bytes != layout.payload_total_bytes ||
      expected.probe_total_bytes != layout.probe_total_bytes ||
      expected.guard_offset_bytes != layout.guard_offset_bytes ||
      expected.allocation_bytes != layout.allocation_bytes) {
    return false;
  }

  const u64 entry_count = static_cast<u64>(active_invocations) *
      layout.stores_per_invocation;
  const u64 payload_bytes = entry_count * 4u * sizeof(u32);
  const u64 payload_offset = static_cast<u64>(object_index) *
      layout.payload_stride_bytes;
  const u64 probe_count = layout.probe_stride_bytes != 0u ?
      active_invocations : 0u;
  const u64 probe_bytes = probe_count * 8u * sizeof(u32);
  const u64 probe_offset = layout.probe_stride_bytes != 0u ?
      static_cast<u64>(layout.payload_total_bytes) +
          static_cast<u64>(object_index) * layout.probe_stride_bytes : 0u;
  if (entry_count > std::numeric_limits<u32>::max() ||
      payload_bytes > std::numeric_limits<u32>::max() ||
      payload_offset + payload_bytes > layout.payload_total_bytes ||
      probe_count > std::numeric_limits<u32>::max() ||
      probe_bytes > std::numeric_limits<u32>::max() ||
      (layout.probe_stride_bytes != 0u &&
       probe_offset + probe_bytes > layout.guard_offset_bytes)) {
    return false;
  }

  GeneratedLoopKernelPrivateOutputSlice resolved;
  resolved.payload_offset_bytes = static_cast<u32>(payload_offset);
  resolved.payload_bytes = static_cast<u32>(payload_bytes);
  resolved.entry_count = static_cast<u32>(entry_count);
  resolved.probe_offset_bytes = static_cast<u32>(probe_offset);
  resolved.probe_bytes = static_cast<u32>(probe_bytes);
  resolved.probe_count = static_cast<u32>(probe_count);
  *slice = resolved;
  return true;
}

bool ResolveGeneratedLoopKernelPrivateOutputWriteExtent(
    u32 invocation_first, u32 invocation_last,
    u32 stores_per_invocation, u32 payload_capacity_words,
    u32 probe_capacity_words,
    GeneratedLoopKernelPrivateOutputWriteExtent* extent) {
  if (!extent || invocation_first > invocation_last ||
      stores_per_invocation == 0u || payload_capacity_words == 0u) {
    return false;
  }

  // Generated Cg emits:
  //   BUFFER2[(invocation * stores + store) * 4 + lane]
  //   BUFFER3[invocation * 2 + {0,1}] (each element is int4)
  // Use 64-bit intermediates so a malformed INDEX domain is rejected before
  // either expression can wrap in the host-side proof.
  const u64 invocation_count = static_cast<u64>(invocation_last) + 1u;
  const u64 payload_maximum = invocation_count * stores_per_invocation * 4u - 1u;
  if (payload_maximum >= payload_capacity_words ||
      payload_maximum > std::numeric_limits<u32>::max()) {
    return false;
  }
  u64 probe_maximum = 0u;
  if (probe_capacity_words != 0u) {
    probe_maximum = invocation_count * 8u - 1u;
    if (probe_maximum >= probe_capacity_words ||
        probe_maximum > std::numeric_limits<u32>::max()) {
      return false;
    }
  }

  GeneratedLoopKernelPrivateOutputWriteExtent resolved;
  resolved.invocation_first = invocation_first;
  resolved.invocation_last = invocation_last;
  resolved.payload_capacity_words = payload_capacity_words;
  resolved.payload_maximum_write_word = static_cast<u32>(payload_maximum);
  resolved.probe_capacity_words = probe_capacity_words;
  resolved.probe_maximum_write_word = static_cast<u32>(probe_maximum);
  *extent = resolved;
  return true;
}

const char* GeneratedInputWindowFailureName(
    GeneratedInputWindowFailure failure) {
  switch (failure) {
    case GeneratedInputWindowFailure::None:
      return "none";
    case GeneratedInputWindowFailure::MissingBindings:
      return "missing-bindings";
    case GeneratedInputWindowFailure::InvalidCanonicalBinding:
      return "invalid-canonical-binding";
    case GeneratedInputWindowFailure::MixedOwners:
      return "mixed-owners";
    case GeneratedInputWindowFailure::InvalidBindingOwner:
      return "invalid-binding-owner";
    case GeneratedInputWindowFailure::MissingPayloads:
      return "missing-payloads";
    case GeneratedInputWindowFailure::PayloadIndex:
      return "payload-index";
    case GeneratedInputWindowFailure::InvalidPayload:
      return "invalid-payload";
    case GeneratedInputWindowFailure::PayloadLogicalRange:
      return "payload-logical-range";
    case GeneratedInputWindowFailure::MixedGeneration:
      return "mixed-generation";
    case GeneratedInputWindowFailure::InvalidBindingExtent:
      return "invalid-binding-extent";
    case GeneratedInputWindowFailure::BindingOutsidePayload:
      return "binding-outside-payload";
    case GeneratedInputWindowFailure::BindingOutsideOwner:
      return "binding-outside-owner";
    case GeneratedInputWindowFailure::EmptyWindow:
      return "empty-window";
    case GeneratedInputWindowFailure::DeclaredWindow:
      return "declared-window";
  }
  return "unknown";
}

bool ResolveGeneratedInputWindow(
    const RawVifPayloadRef* payloads, size_t payload_count,
    const StreamBinding* bindings, size_t binding_count,
    u32 raw_owner_qwords, u32 canonical_owner_qwords,
    u32 declared_qword_count, u32 maximum_relative_qword,
    GeneratedInputWindow* window, GeneratedInputWindowFailure* failure) {
  const auto reject = [failure](GeneratedInputWindowFailure reason) {
    if (failure)
      *failure = reason;
    return false;
  };
  if (!window)
    return reject(GeneratedInputWindowFailure::EmptyWindow);
  if (!bindings || binding_count == 0u)
    return reject(GeneratedInputWindowFailure::MissingBindings);
  if (raw_owner_qwords == 0u || canonical_owner_qwords == 0u)
    return reject(GeneratedInputWindowFailure::BindingOutsideOwner);

  const u64 raw_owner_bytes = static_cast<u64>(raw_owner_qwords) * 16u;
  const u64 canonical_owner_bytes =
      static_cast<u64>(canonical_owner_qwords) * 16u;
  GeneratedInputWindow resolved;
  u32 first_qword = std::numeric_limits<u32>::max();
  u32 last_qword = 0u;
  for (size_t binding_index = 0u; binding_index < binding_count;
       binding_index++) {
    const StreamBinding& binding = bindings[binding_index];
    if (binding.owner == StreamInputOwner::CanonicalVuMemory) {
      if (resolved.uses_raw)
        return reject(GeneratedInputWindowFailure::MixedOwners);
      const u64 end = static_cast<u64>(binding.payload_byte_offset) +
                      binding.payload_byte_extent;
      if (binding.input_span != std::numeric_limits<u16>::max() ||
          binding.payload_byte_extent < sizeof(u128) ||
          (binding.payload_byte_offset & 15u) != 0u ||
          (binding.payload_byte_extent & 15u) != 0u ||
          end < binding.payload_byte_offset || end > canonical_owner_bytes) {
        return reject(
            GeneratedInputWindowFailure::InvalidCanonicalBinding);
      }
      resolved.uses_canonical = true;
      first_qword =
          std::min(first_qword, binding.payload_byte_offset / 16u);
      last_qword = std::max(
          last_qword, static_cast<u32>((end - 1u) / 16u));
      continue;
    }
    if (binding.owner != StreamInputOwner::RawInput)
      return reject(GeneratedInputWindowFailure::InvalidBindingOwner);
    if (resolved.uses_canonical)
      return reject(GeneratedInputWindowFailure::MixedOwners);
    if (!payloads || payload_count == 0u)
      return reject(GeneratedInputWindowFailure::MissingPayloads);
    if (binding.input_span >= payload_count)
      return reject(GeneratedInputWindowFailure::PayloadIndex);

    const RawVifPayloadRef& payload = payloads[binding.input_span];
    if (!payload.IsValid() || payload.slot >= InputRingSlotCount)
      return reject(GeneratedInputWindowFailure::InvalidPayload);
    const u64 payload_end =
        static_cast<u64>(payload.offset) + payload.size;
    if (payload_end < payload.offset || payload_end > raw_owner_bytes)
      return reject(GeneratedInputWindowFailure::PayloadLogicalRange);
    if (!resolved.uses_raw) {
      resolved.owner = payload.owner;
      resolved.slot = payload.slot;
      resolved.generation = payload.generation;
      resolved.uses_raw = true;
    } else if (payload.owner != resolved.owner ||
               payload.slot != resolved.slot) {
      return reject(GeneratedInputWindowFailure::MixedOwners);
    } else if (payload.generation != resolved.generation) {
      return reject(GeneratedInputWindowFailure::MixedGeneration);
    }

    if (binding.payload_byte_extent < sizeof(u128) ||
        (binding.payload_byte_offset & 15u) != 0u ||
        (binding.payload_byte_extent & 15u) != 0u) {
      return reject(GeneratedInputWindowFailure::InvalidBindingExtent);
    }
    const u64 relative_end =
        static_cast<u64>(binding.payload_byte_offset) +
        binding.payload_byte_extent;
    if (relative_end < binding.payload_byte_offset ||
        relative_end > payload.size) {
      return reject(GeneratedInputWindowFailure::BindingOutsidePayload);
    }
    const u64 absolute_first =
        static_cast<u64>(payload.offset) + binding.payload_byte_offset;
    const u64 absolute_end = absolute_first + binding.payload_byte_extent;
    if ((absolute_first & 15u) != 0u || absolute_end < absolute_first ||
        absolute_end > raw_owner_bytes) {
      return reject(GeneratedInputWindowFailure::BindingOutsideOwner);
    }
    first_qword =
        std::min(first_qword, static_cast<u32>(absolute_first / 16u));
    last_qword = std::max(
        last_qword, static_cast<u32>((absolute_end - 1u) / 16u));
  }

  if ((!resolved.uses_raw && !resolved.uses_canonical) ||
      first_qword == std::numeric_limits<u32>::max()) {
    return reject(GeneratedInputWindowFailure::EmptyWindow);
  }
  const u32 owner_qwords =
      resolved.uses_raw ? raw_owner_qwords : canonical_owner_qwords;
  u32 bound_first_qword = 0u;
  if (!ResolveGeneratedBufferWindow(
          first_qword, last_qword, owner_qwords, declared_qword_count,
          maximum_relative_qword, &bound_first_qword)) {
    return reject(GeneratedInputWindowFailure::DeclaredWindow);
  }

  resolved.required_first_qword = first_qword;
  resolved.required_last_qword = last_qword;
  resolved.bound_first_qword = bound_first_qword;
  *window = resolved;
  if (failure)
    *failure = GeneratedInputWindowFailure::None;
  return true;
}

bool ResolveGeneratedBufferWindow(u32 required_first_qword,
                                  u32 required_last_qword,
                                  u32 owner_qword_count,
                                  u32 declared_qword_count,
                                  u32 maximum_relative_qword,
                                  u32* bound_first_qword) {
  if (!bound_first_qword || owner_qword_count == 0u ||
      declared_qword_count == 0u ||
      declared_qword_count > owner_qword_count ||
      required_first_qword > required_last_qword ||
      required_last_qword >= owner_qword_count) {
    return false;
  }

  const u32 candidate = std::min(
      required_first_qword, owner_qword_count - declared_qword_count);
  if (required_last_qword - candidate > maximum_relative_qword)
    return false;
  *bound_first_qword = candidate;
  return true;
}

bool ValidateGeneratedBufferReadWindow(u64 absolute_byte_offset,
                                       u32 payload_byte_extent,
                                       u32 bound_first_qword,
                                       u32 required_last_qword,
                                       u32 maximum_relative_qword,
                                       u32* relative_base_qword) {
  if (!relative_base_qword || (absolute_byte_offset & 15u) != 0u ||
      payload_byte_extent < sizeof(u128) ||
      (payload_byte_extent & 15u) != 0u ||
      bound_first_qword > required_last_qword) {
    return false;
  }
  const u64 absolute_qword = absolute_byte_offset / 16u;
  if (absolute_qword < bound_first_qword)
    return false;
  const u64 relative_qword = absolute_qword - bound_first_qword;
  const u64 extent_qwords = payload_byte_extent / 16u;
  const u64 last_qword = relative_qword + extent_qwords - 1u;
  if (last_qword < relative_qword ||
      relative_qword > maximum_relative_qword ||
      last_qword > maximum_relative_qword ||
      last_qword > required_last_qword - bound_first_qword) {
    return false;
  }
  *relative_base_qword = static_cast<u32>(relative_qword);
  return true;
}

bool ResolveGeneratedCompactStorageQwords(u32 logical_qword_count,
                                          u32 declared_qword_count,
                                          u32* storage_qword_count) {
  if (!storage_qword_count || logical_qword_count == 0u ||
      declared_qword_count == 0u) {
    return false;
  }
  *storage_qword_count =
      std::max(logical_qword_count, declared_qword_count);
  return true;
}
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

const char* PrivateArchitecturalStateValueKindName(
    PrivateArchitecturalStateValueKind kind) {
  switch (kind) {
  case PrivateArchitecturalStateValueKind::Vf:
    return "vf";
  case PrivateArchitecturalStateValueKind::Acc:
    return "acc";
  case PrivateArchitecturalStateValueKind::Q:
    return "q";
  case PrivateArchitecturalStateValueKind::P:
    return "p";
  case PrivateArchitecturalStateValueKind::I:
    return "i";
  case PrivateArchitecturalStateValueKind::Vi:
    return "vi";
  case PrivateArchitecturalStateValueKind::Tpc:
    return "tpc";
  }
  return "unknown";
}

bool GeneratedLoopKernelTransactionLayout::Seal() {
  if (sealed)
    return IsSealed();
  if (store_targets.empty() ||
      store_targets.size() > std::numeric_limits<u32>::max() / 4u) {
    return false;
  }

  store_word_masks.fill(0u);
  store_qword_masks.fill(0u);
  complete_write_qword_masks.fill(0u);
  store_word_count = 0u;
  for (const GeneratedLoopKernelStoreTarget& target : store_targets) {
    if (target.address_qword >= MemoryQwordCount || target.lane_mask == 0u ||
        (target.lane_mask & ~0x0fu) != 0u || target.reserved != 0u) {
      return false;
    }
    store_qword_masks[target.address_qword >> 5u] |=
        1u << (target.address_qword & 31u);
    for (u32 lane = 0u; lane < 4u; lane++) {
      if ((target.lane_mask & (0x8u >> lane)) == 0u)
        continue;
      const u32 word = static_cast<u32>(target.address_qword) * 4u + lane;
      const u32 bit = 1u << (word & 31u);
      if ((store_word_masks[word >> 5u] & bit) == 0u)
        store_word_count++;
      store_word_masks[word >> 5u] |= bit;
    }
  }
  for (u16 qword : adc_patch_qwords) {
    if (qword >= MemoryQwordCount)
      return false;
  }
  for (u32 qword = 0u; qword < MemoryQwordCount; qword++) {
    const u32 first_word = qword * 4u;
    u32 written_lanes =
        (store_word_masks[first_word >> 5u] >> (first_word & 31u)) & 0x0fu;
    if (std::find(adc_patch_qwords.begin(), adc_patch_qwords.end(), qword) !=
        adc_patch_qwords.end()) {
      written_lanes |= 0x8u;
    }
    if (written_lanes == 0x0fu) {
      complete_write_qword_masks[qword >> 5u] |=
          1u << (qword & 31u);
    }
  }
  sealed_store_target_count = store_targets.size();
  sealed_adc_patch_count = adc_patch_qwords.size();
  if (store_word_count == 0u)
    return false;
  sealed = true;
  return true;
}

bool GeneratedLoopKernelTransactionShape::Seal() {
  if (sealed)
    return true;
  if (!layout || !layout->IsSealed() || layout->store_targets.empty() ||
      output_numeric_profile == GeneratedLoopKernelNumericProfile::None ||
      (store_commit_mode ==
           GeneratedLoopKernelStoreCommitMode::GpuNativeOutputOnly &&
       output_numeric_profile !=
           GeneratedLoopKernelNumericProfile::NativeSgxOutputOnly) ||
      (final_vi_write_mask & ~0xfffeu) != 0u ||
      unique_resume_pc > 0x4000u || (unique_resume_pc & 7u) != 0u ||
      executed_pairs == 0u || (final_acc_lanes & ~0x0fu) != 0u ||
      final_vf_lanes[0] != 0u) {
    return false;
  }
  for (u8 lanes : final_vf_lanes) {
    if ((lanes & ~0x0fu) != 0u)
      return false;
  }
  sealed = true;
  return true;
}

bool GeneratedLoopKernelTransactionShape::Matches(
    const std::shared_ptr<const GeneratedLoopKernelTransactionLayout>&
        candidate_layout,
    GeneratedLoopKernelStoreCommitMode candidate_store_commit_mode,
    GeneratedLoopKernelNumericProfile candidate_output_numeric_profile,
    const std::array<u8, 32>& candidate_final_vf_lanes,
    u8 candidate_final_acc_lanes, bool candidate_final_q,
    bool candidate_final_p, bool candidate_final_i,
    u32 candidate_final_vi_write_mask, u32 candidate_unique_resume_pc,
    u32 candidate_executed_pairs) const {
  return sealed && layout == candidate_layout &&
      store_commit_mode == candidate_store_commit_mode &&
      output_numeric_profile == candidate_output_numeric_profile &&
      final_vf_lanes == candidate_final_vf_lanes &&
      final_acc_lanes == candidate_final_acc_lanes &&
      final_q == candidate_final_q && final_p == candidate_final_p &&
      final_i == candidate_final_i &&
      final_vi_write_mask == candidate_final_vi_write_mask &&
      unique_resume_pc == candidate_unique_resume_pc &&
      executed_pairs == candidate_executed_pairs;
}

const char* GeneratedLoopKernelTransactionStageName(
    GeneratedLoopKernelTransactionStage stage) {
  switch (stage) {
  case GeneratedLoopKernelTransactionStage::Prepared:
    return "prepared";
  case GeneratedLoopKernelTransactionStage::GsAccepted:
    return "gs-accepted";
  case GeneratedLoopKernelTransactionStage::GpuCompleted:
    return "gpu-completed";
  case GeneratedLoopKernelTransactionStage::Failed:
    return "failed";
  case GeneratedLoopKernelTransactionStage::Adopted:
    return "adopted";
  }
  return "unknown";
}

const char* GeneratedLoopKernelTransactionFailureName(
    GeneratedLoopKernelTransactionFailure failure) {
  switch (failure) {
  case GeneratedLoopKernelTransactionFailure::None:
    return "none";
  case GeneratedLoopKernelTransactionFailure::Unspecified:
    return "unspecified";
  case GeneratedLoopKernelTransactionFailure::
      DescriptorReleasedBeforeGsAcceptance:
    return "descriptor-released-before-gs-acceptance";
  case GeneratedLoopKernelTransactionFailure::
      GsOutputRecordReleasedBeforeCompletion:
    return "gs-output-record-released-before-completion";
  case GeneratedLoopKernelTransactionFailure::GsRejectedBeforeEffects:
    return "gs-rejected-before-effects";
  case GeneratedLoopKernelTransactionFailure::GsSubmissionFailedAfterEffects:
    return "gs-submission-failed-after-effects";
  case GeneratedLoopKernelTransactionFailure::InvalidGpuOutputPublication:
    return "invalid-gpu-output-publication";
  case GeneratedLoopKernelTransactionFailure::InvalidGpuCompletionPublication:
    return "invalid-gpu-completion-publication";
  case GeneratedLoopKernelTransactionFailure::CpuJournalAttachment:
    return "cpu-journal-attachment";
  case GeneratedLoopKernelTransactionFailure::GpuRetirementOwnerFailure:
    return "gpu-retirement-owner-failure";
  }
  return "unknown";
}

GeneratedLoopKernelTransaction::~GeneratedLoopKernelTransaction() {
  for (VifUnpackSpan &span : m_replay_unpacks)
    ReleaseRawVifPayload(&span.payload);
}

std::shared_ptr<GeneratedLoopKernelTransaction>
AcquireGeneratedLoopKernelTransaction(u32 preferred_output_words) {
  // Keep every product BUFFER2 journal in process-lifetime storage. The old
  // make_shared/vector path allocated roughly 6 KiB per large BSpline Execute
  // and made widening the asynchronous window fail through newlib bad_alloc.
  // Four MiB of bounded BSS is substantially cheaper than a CPU1 full-drain
  // every one-to-two guest frames and cannot fragment the process heap.
  struct Pool final {
    Threading::KernelMutex mutex;
    std::array<std::shared_ptr<GeneratedLoopKernelTransaction>,
               GeneratedLoopKernelTransactionPoolCapacity> slots{};
    alignas(64) std::array<
        std::array<u32, GeneratedLoopKernelTransactionOutputWordsPerSlot>,
        GeneratedLoopKernelTransactionPoolCapacity> output_words{};
    u32 next_slot = 0u;
    Pool() {
      for (u32 index = 0u;
           index < GeneratedLoopKernelTransactionPoolCapacity; index++) {
        slots[index] = std::make_shared<GeneratedLoopKernelTransaction>();
        slots[index]->AttachFixedOutputStorage(output_words[index].data(),
            GeneratedLoopKernelTransactionOutputWordsPerSlot);
      }
    }
  };
  static Pool pool;

  std::lock_guard lock(pool.mutex);
  if (preferred_output_words >
      GeneratedLoopKernelTransactionOutputWordsPerSlot)
    return {};
  // CPU1 acquires and GXM retires slots in sequence order. Starting every hot
  // allocation at slot zero made each Execute reread dozens of live shared_ptr
  // control blocks. A rotating cursor normally reaches the just-retired slot
  // immediately while retaining a bounded full scan under real pressure.
  for (u32 offset = 0u;
       offset < GeneratedLoopKernelTransactionPoolCapacity; offset++) {
    const u32 index =
        (pool.next_slot + offset) % GeneratedLoopKernelTransactionPoolCapacity;
    auto& slot = pool.slots[index];
    // The pool's own reference is permanent. If it is the sole owner, no CPU
    // or GXM thread can still observe this generation and its retained vector
    // capacities can be reset safely for the next immutable transaction.
    if (slot.use_count() != 1)
      continue;
    pool.next_slot =
        (index + 1u) % GeneratedLoopKernelTransactionPoolCapacity;
    slot->ResetForReuse(preferred_output_words != 0u);
    return slot;
  }
  return {};
}

void GeneratedLoopKernelTransaction::AttachFixedOutputStorage(
    u32* words, u32 capacity_words) {
  pxAssertRel(!m_configured && m_output_words_data == nullptr && words &&
                  capacity_words != 0u,
              "invalid generated GPU-VU fixed output journal");
  m_output_words_data = words;
  m_output_words_capacity = capacity_words;
  m_output_words_size = 0u;
}

bool GeneratedLoopKernelTransaction::ResizeOutputWords(u32 word_count) {
  if (m_output_words_capacity != 0u) {
    if (!m_output_words_data || word_count > m_output_words_capacity)
      return false;
    m_output_words_size = word_count;
    return true;
  }
  m_output_words_fallback.resize(word_count);
  m_output_words_data = m_output_words_fallback.data();
  m_output_words_size = word_count;
  return true;
}

void GeneratedLoopKernelTransaction::ResetForReuse(
    bool preserve_output_words) {
  for (VifUnpackSpan &span : m_replay_unpacks)
    ReleaseRawVifPayload(&span.payload);
  m_replay_unpacks.clear();
  m_replay_unpacks_attached = false;
  m_shape.reset();
  m_layout.reset();
  m_exact_store_words.clear();
  m_pre_loop_store_targets.clear();
  m_pre_loop_store_words.clear();
  m_output_words_size = 0u;
  if (m_output_words_capacity == 0u) {
    m_output_words_fallback.clear();
    m_output_words_data = m_output_words_fallback.data();
  }
  m_deferred_successor.reset();
  m_deferred_stores.reset();
  m_final_vf_lanes = {};
  m_final_vf_values = {};
  m_final_acc_values = {};
  m_final_vi_values = {};
  m_sequence = 0u;
  m_final_vi_write_mask = 0u;
  m_unique_resume_pc = 0u;
  m_executed_pairs = 0u;
  m_final_q_value = 0u;
  m_final_p_value = 0u;
  m_final_i_value = 0u;
  m_final_acc_lanes = 0u;
  m_store_commit_mode = GeneratedLoopKernelStoreCommitMode::PairPlanExact;
  m_output_numeric_profile = GeneratedLoopKernelNumericProfile::None;
  m_final_q = false;
  m_final_p = false;
  m_final_i = false;
  m_validation_canary = false;
  m_configured = false;
  m_gpu_output_owner.store(false, std::memory_order_relaxed);
  m_failure.store(GeneratedLoopKernelTransactionFailure::None,
                  std::memory_order_relaxed);
  m_failure_detail.store(nullptr, std::memory_order_relaxed);
  m_stage.store(GeneratedLoopKernelTransactionStage::Prepared,
                std::memory_order_relaxed);
}

bool GeneratedLoopKernelTransaction::Configure(
    std::vector<GeneratedLoopKernelStoreTarget> store_targets,
    std::vector<u32> exact_store_words,
    std::vector<GeneratedLoopKernelStoreTarget> pre_loop_store_targets,
    std::vector<u32> pre_loop_store_words, std::vector<u16> adc_patch_qwords,
    GeneratedLoopKernelStoreCommitMode store_commit_mode,
    GeneratedLoopKernelNumericProfile output_numeric_profile,
    const std::array<u8, 32> &final_vf_lanes, u8 final_acc_lanes, bool final_q,
    bool final_p, bool final_i,
    const std::array<std::array<u32, 4>, 32> &final_vf_values,
    const std::array<u32, 4> &final_acc_values, u32 final_q_value,
    u32 final_p_value, u32 final_i_value,
    const std::array<u16, 16> &final_vi_values, u32 final_vi_write_mask,
    u32 unique_resume_pc, u32 executed_pairs,
    std::shared_ptr<const GeneratedLoopKernelDeferredSuccessor>
        deferred_successor,
    std::shared_ptr<const GeneratedLoopKernelDeferredStores>
        deferred_stores) {
  auto layout = std::make_shared<GeneratedLoopKernelTransactionLayout>();
  layout->store_targets = std::move(store_targets);
  layout->adc_patch_qwords = std::move(adc_patch_qwords);
  if (!layout->Seal())
    return false;
  return Configure(std::move(layout), std::move(exact_store_words),
                   std::move(pre_loop_store_targets),
                   std::move(pre_loop_store_words), store_commit_mode,
                   output_numeric_profile, final_vf_lanes, final_acc_lanes,
                   final_q, final_p, final_i, final_vf_values, final_acc_values,
                   final_q_value, final_p_value, final_i_value, final_vi_values,
                   final_vi_write_mask, unique_resume_pc, executed_pairs,
                   std::move(deferred_successor), std::move(deferred_stores));
}

bool GeneratedLoopKernelTransaction::Configure(
    std::shared_ptr<const GeneratedLoopKernelTransactionLayout> layout,
    std::vector<u32> exact_store_words,
    std::vector<GeneratedLoopKernelStoreTarget> pre_loop_store_targets,
    std::vector<u32> pre_loop_store_words,
    GeneratedLoopKernelStoreCommitMode store_commit_mode,
    GeneratedLoopKernelNumericProfile output_numeric_profile,
    const std::array<u8, 32> &final_vf_lanes, u8 final_acc_lanes, bool final_q,
    bool final_p, bool final_i,
    const std::array<std::array<u32, 4>, 32> &final_vf_values,
    const std::array<u32, 4> &final_acc_values, u32 final_q_value,
    u32 final_p_value, u32 final_i_value,
    const std::array<u16, 16> &final_vi_values, u32 final_vi_write_mask,
    u32 unique_resume_pc, u32 executed_pairs,
    std::shared_ptr<const GeneratedLoopKernelDeferredSuccessor>
        deferred_successor,
    std::shared_ptr<const GeneratedLoopKernelDeferredStores>
        deferred_stores) {
  const bool gpu_native_output_only =
      store_commit_mode ==
      GeneratedLoopKernelStoreCommitMode::GpuNativeOutputOnly;
  const bool deferred_pairplan =
      store_commit_mode ==
      GeneratedLoopKernelStoreCommitMode::DeferredPairPlanExact;
  const size_t store_target_count = layout ? layout->store_targets.size() : 0u;
  const bool store_word_shape_valid =
      gpu_native_output_only || deferred_pairplan
          ? exact_store_words.empty()
          : exact_store_words.size() == store_target_count * 4u;
  if (m_configured || !layout || !layout->IsSealed() ||
      store_target_count == 0u ||
      store_target_count > std::numeric_limits<u32>::max() / 4u ||
      !store_word_shape_valid ||
      pre_loop_store_targets.size() > std::numeric_limits<u32>::max() / 4u ||
      pre_loop_store_words.size() != pre_loop_store_targets.size() * 4u ||
      output_numeric_profile == GeneratedLoopKernelNumericProfile::None ||
      (gpu_native_output_only &&
       output_numeric_profile !=
           GeneratedLoopKernelNumericProfile::NativeSgxOutputOnly) ||
      (deferred_pairplan && !deferred_stores) ||
      (!deferred_pairplan && deferred_stores) ||
      (final_vi_write_mask & ~0xfffeu) != 0u || unique_resume_pc > 0x4000u ||
      (unique_resume_pc & 7u) != 0u || executed_pairs == 0u ||
      (final_acc_lanes & ~0x0fu) != 0u) {
    return false;
  }
  for (u32 reg = 0u; reg < final_vf_lanes.size(); reg++) {
    if ((final_vf_lanes[reg] & ~0x0fu) != 0u)
      return false;
  }
  if (final_vf_lanes[0] != 0u)
    return false;
  const auto valid_store_targets =
      [](const std::vector<GeneratedLoopKernelStoreTarget> &targets) {
        for (const GeneratedLoopKernelStoreTarget &target : targets) {
          if (target.address_qword >= 1024u || target.lane_mask == 0u ||
              (target.lane_mask & ~0x0fu) != 0u || target.reserved != 0u) {
            return false;
          }
        }
        return true;
      };
  if (!valid_store_targets(pre_loop_store_targets)) {
    return false;
  }
  if (deferred_pairplan) {
    if (deferred_stores->LayoutIdentity() != layout.get())
      return false;
  }
  /*
   * The pre-loop journal contains exact PairPlan stores which precede the
   * generated parallel body (for example, a packet GIF-tag SQ).  They have no
   * corresponding GPU BUFFER2 words, so keep them distinct from the body
   * journal whose shape is fixed by the generated invocation grid.
   */
  const u64 word_count = static_cast<u64>(store_target_count) * 4u;
  if (word_count > std::numeric_limits<u32>::max())
    return false;
  m_layout = std::move(layout);
  m_exact_store_words = std::move(exact_store_words);
  m_pre_loop_store_targets = std::move(pre_loop_store_targets);
  m_pre_loop_store_words = std::move(pre_loop_store_words);
  // A deferred-exact product is completion-only: the GPU emits vertices and
  // the CPU1-private state owns the dormant PairPlan store formula.  Keeping a
  // zero-filled BUFFER2-sized vector in every in-flight transaction retained
  // memory which neither GXM nor retirement could consume.
  if (!deferred_pairplan && !ResizeOutputWords(static_cast<u32>(word_count)))
    return false;
  m_deferred_successor = std::move(deferred_successor);
  m_deferred_stores = std::move(deferred_stores);
  m_final_vf_lanes = final_vf_lanes;
  m_final_vf_values = final_vf_values;
  m_final_acc_lanes = final_acc_lanes;
  m_store_commit_mode = store_commit_mode;
  m_output_numeric_profile = output_numeric_profile;
  m_final_acc_values = final_acc_values;
  m_final_q = final_q;
  m_final_p = final_p;
  m_final_i = final_i;
  m_final_q_value = final_q_value;
  m_final_p_value = final_p_value;
  m_final_i_value = final_i_value;
  m_final_vi_values = final_vi_values;
  m_final_vi_write_mask = final_vi_write_mask;
  m_unique_resume_pc = unique_resume_pc;
  m_executed_pairs = executed_pairs;
  m_configured = true;
  return true;
}

bool GeneratedLoopKernelTransaction::Configure(
    std::shared_ptr<const GeneratedLoopKernelTransactionShape> shape,
    std::vector<u32> exact_store_words,
    std::vector<GeneratedLoopKernelStoreTarget> pre_loop_store_targets,
    std::vector<u32> pre_loop_store_words,
    const std::array<u16, 16>& final_vi_values,
    std::shared_ptr<const GeneratedLoopKernelDeferredSuccessor>
        deferred_successor,
    std::shared_ptr<const GeneratedLoopKernelDeferredStores> deferred_stores) {
  if (m_configured || !shape || !shape->IsSealed() || !shape->layout)
    return false;
  const bool gpu_native_output_only =
      shape->store_commit_mode ==
      GeneratedLoopKernelStoreCommitMode::GpuNativeOutputOnly;
  const bool deferred_pairplan =
      shape->store_commit_mode ==
      GeneratedLoopKernelStoreCommitMode::DeferredPairPlanExact;
  const size_t store_target_count = shape->layout->store_targets.size();
  if ((gpu_native_output_only || deferred_pairplan
           ? !exact_store_words.empty()
           : exact_store_words.size() != store_target_count * 4u) ||
      pre_loop_store_targets.size() >
          std::numeric_limits<u32>::max() / 4u ||
      pre_loop_store_words.size() != pre_loop_store_targets.size() * 4u ||
      (deferred_pairplan &&
       (!deferred_stores ||
        deferred_stores->LayoutIdentity() != shape->layout.get())) ||
      (!deferred_pairplan && deferred_stores)) {
    return false;
  }
  for (const GeneratedLoopKernelStoreTarget& target :
       pre_loop_store_targets) {
    if (target.address_qword >= 1024u || target.lane_mask == 0u ||
        (target.lane_mask & ~0x0fu) != 0u || target.reserved != 0u) {
      return false;
    }
  }

  m_shape = std::move(shape);
  m_layout = m_shape->layout;
  m_exact_store_words = std::move(exact_store_words);
  m_pre_loop_store_targets = std::move(pre_loop_store_targets);
  m_pre_loop_store_words = std::move(pre_loop_store_words);
  if (!deferred_pairplan &&
      !ResizeOutputWords(static_cast<u32>(store_target_count * 4u)))
    return false;
  m_deferred_successor = std::move(deferred_successor);
  m_deferred_stores = std::move(deferred_stores);
  m_final_vi_values = final_vi_values;
  m_configured = true;
  return true;
}

bool GeneratedLoopKernelTransaction::AttachReplayUnpacks(
    std::vector<VifUnpackSpan> *spans) {
  if (!m_configured || !spans || m_replay_unpacks_attached ||
      Stage() != GeneratedLoopKernelTransactionStage::Prepared) {
    return false;
  }
  m_replay_unpacks = std::move(*spans);
  spans->clear();
  m_replay_unpacks_attached = true;
  return true;
}

std::vector<VifUnpackSpan> GeneratedLoopKernelTransaction::TakeReplayUnpacks() {
  if (Stage() != GeneratedLoopKernelTransactionStage::GpuCompleted ||
      !m_replay_unpacks_attached)
    return {};
  std::vector<VifUnpackSpan> result = std::move(m_replay_unpacks);
  m_replay_unpacks.clear();
  m_replay_unpacks_attached = false;
  return result;
}

bool GeneratedLoopKernelTransaction::RecoverReplayUnpacksBeforeGsAcceptance(
    std::vector<VifUnpackSpan>* spans) {
  if (!spans || !spans->empty() || !m_replay_unpacks_attached ||
      m_gpu_output_owner.load(std::memory_order_acquire) ||
      Stage() == GeneratedLoopKernelTransactionStage::GsAccepted ||
      Stage() == GeneratedLoopKernelTransactionStage::GpuCompleted ||
      Stage() == GeneratedLoopKernelTransactionStage::Adopted) {
    return false;
  }
  *spans = std::move(m_replay_unpacks);
  m_replay_unpacks.clear();
  m_replay_unpacks_attached = false;
  return true;
}

bool GeneratedLoopKernelTransaction::CanClaimGpuOutputOwner() const {
  return m_configured && m_sequence != 0u &&
         Stage() == GeneratedLoopKernelTransactionStage::Prepared &&
         !m_gpu_output_owner.load(std::memory_order_acquire);
}

bool GeneratedLoopKernelTransaction::ClaimGpuOutputOwner() {
  if (!CanClaimGpuOutputOwner()) {
    return false;
  }
  bool expected = false;
  if (!m_gpu_output_owner.compare_exchange_strong(
          expected, true, std::memory_order_acq_rel,
          std::memory_order_acquire)) {
    return false;
  }
  GeneratedLoopKernelTransactionStage expected_stage =
      GeneratedLoopKernelTransactionStage::Prepared;
  if (!m_stage.compare_exchange_strong(
          expected_stage, GeneratedLoopKernelTransactionStage::GsAccepted,
          std::memory_order_release, std::memory_order_acquire)) {
    m_gpu_output_owner.store(false, std::memory_order_release);
    return false;
  }
  return true;
}

void GeneratedLoopKernelTransaction::ReleaseDescriptorOwner() {
  if (!m_gpu_output_owner.load(std::memory_order_acquire))
    MarkFailed(GeneratedLoopKernelTransactionFailure::
                   DescriptorReleasedBeforeGsAcceptance);
}

bool GeneratedLoopKernelTransaction::PublishGpuOutput(const u32 *words,
                                                      u32 word_count) {
  if (!words || !m_configured || m_sequence == 0u ||
      !m_gpu_output_owner.load(std::memory_order_acquire) ||
      word_count != OutputWordCount() ||
      Stage() != GeneratedLoopKernelTransactionStage::GsAccepted) {
    MarkFailed(GeneratedLoopKernelTransactionFailure::
                   InvalidGpuOutputPublication);
    return false;
  }
  std::copy_n(words, word_count, m_output_words_data);
  GeneratedLoopKernelTransactionStage expected =
      GeneratedLoopKernelTransactionStage::GsAccepted;
  if (!m_stage.compare_exchange_strong(
          expected, GeneratedLoopKernelTransactionStage::GpuCompleted,
          std::memory_order_release, std::memory_order_acquire)) {
    return false;
  }
  return true;
}

bool GeneratedLoopKernelTransaction::PublishGpuCompletion() {
  if (!m_configured || m_sequence == 0u ||
      !m_gpu_output_owner.load(std::memory_order_acquire) ||
      UsesGpuNativeOutputOnlyStoreCommit() ||
      (!UsesDeferredPairPlanStoreCommit() &&
       m_exact_store_words.size() != OutputWordCount()) ||
      Stage() != GeneratedLoopKernelTransactionStage::GsAccepted) {
    MarkFailed(GeneratedLoopKernelTransactionFailure::
                   InvalidGpuCompletionPublication);
    return false;
  }
  GeneratedLoopKernelTransactionStage expected =
      GeneratedLoopKernelTransactionStage::GsAccepted;
  return m_stage.compare_exchange_strong(
      expected, GeneratedLoopKernelTransactionStage::GpuCompleted,
      std::memory_order_release, std::memory_order_acquire);
}

void GeneratedLoopKernelTransaction::MarkFailed(
    GeneratedLoopKernelTransactionFailure failure, const char* detail) {
  if (failure == GeneratedLoopKernelTransactionFailure::None)
    failure = GeneratedLoopKernelTransactionFailure::Unspecified;

  GeneratedLoopKernelTransactionFailure no_failure =
      GeneratedLoopKernelTransactionFailure::None;
  m_failure.compare_exchange_strong(no_failure, failure,
                                    std::memory_order_relaxed,
                                    std::memory_order_relaxed);
  const char* no_detail = nullptr;
  if (detail) {
    m_failure_detail.compare_exchange_strong(
        no_detail, detail, std::memory_order_relaxed,
        std::memory_order_relaxed);
  }

  GeneratedLoopKernelTransactionStage expected =
      GeneratedLoopKernelTransactionStage::Prepared;
  if (m_stage.compare_exchange_strong(
          expected, GeneratedLoopKernelTransactionStage::Failed,
          std::memory_order_release, std::memory_order_acquire)) {
    return;
  }
  if (expected == GeneratedLoopKernelTransactionStage::GsAccepted) {
    m_stage.compare_exchange_strong(
        expected, GeneratedLoopKernelTransactionStage::Failed,
        std::memory_order_release, std::memory_order_acquire);
  }
}

void GeneratedLoopKernelTransaction::WaitForTerminal() {
  // Product ownership drains a complete descriptor batch and polls all stage
  // words together. Giving every logical Execute its own progress event used
  // two SceKernelLwMutexWork objects plus a semaphore on Vita, so constructing
  // and deleting 36 transactions at each coarse boundary consumed tens of
  // milliseconds without advancing either VU1 or GXM. This path is now only a
  // conservative failed-batch/validation fallback; a 1 ms poll preserves its
  // blocking contract without putting kernel synchronization objects in every
  // hot transaction.
  for (;;) {
    const GeneratedLoopKernelTransactionStage observed = Stage();
    if (observed != GeneratedLoopKernelTransactionStage::Prepared &&
        observed != GeneratedLoopKernelTransactionStage::GsAccepted)
      return;
    Threading::Sleep(1);
  }
}

bool GeneratedLoopKernelTransaction::MarkAdopted() {
  GeneratedLoopKernelTransactionStage expected =
      GeneratedLoopKernelTransactionStage::GpuCompleted;
  return m_stage.compare_exchange_strong(
      expected, GeneratedLoopKernelTransactionStage::Adopted,
      std::memory_order_release, std::memory_order_acquire);
}

GpuVuDraw::~GpuVuDraw() {
  if (m_generated_loop_kernel_transaction)
    m_generated_loop_kernel_transaction->ReleaseDescriptorOwner();
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
  if (m_generated_input_plan_installed || HasCompactRawInputs() ||
      !payload.IsValid() ||
      !RetainRawVifPayload(payload))
    return false;
  try {
    m_input_payloads.push_back(payload);
  } catch (const std::bad_alloc&) {
    RawVifPayloadRef retained = payload;
    ReleaseRawVifPayload(&retained);
    return false;
  }
  return true;
}

bool GpuVuDraw::SetGeneratedInputPlan(
    const RawVifPayloadRef* payloads, size_t payload_count,
    const StreamBinding* bindings, size_t binding_count,
    bool require_generated_window) {
  if (m_validated_for_queue || m_generated_input_plan_installed ||
      !m_input_payloads.empty() || !streams.empty() ||
      HasCompactRawInputs() || !bindings || binding_count == 0u ||
      (payload_count != 0u && !payloads)) {
    return false;
  }

  // The generated ABI has at most sixteen BUFFER0 inputs. Keep temporary
  // retains on the stack so making installation transactional does not add a
  // new per-Execute allocation to CPU1's hot path.
  std::array<RawVifPayloadRef, 16> retained{};
  if (payload_count > retained.size())
    return false;
  size_t retained_count = 0u;
  for (; retained_count < payload_count; retained_count++) {
    if (!RetainRawVifPayload(payloads[retained_count])) {
      for (size_t index = 0u; index < retained_count; index++)
        ReleaseRawVifPayload(&retained[index]);
      return false;
    }
    retained[retained_count] = payloads[retained_count];
  }

  GeneratedInputWindow resolved_window;
  if (require_generated_window &&
      !ResolveGeneratedInputWindow(
          payloads, payload_count, bindings, binding_count,
          GeneratedRawInputBufferQwords, 16u * 1024u / 16u,
          GeneratedRawInputDeclaredQwords,
          GeneratedRawInputMaximumRelativeQword, &resolved_window)) {
    for (size_t index = 0u; index < retained_count; index++)
      ReleaseRawVifPayload(&retained[index]);
    return false;
  }

  try {
    m_input_payloads.reserve(payload_count);
    streams.reserve(binding_count);
  } catch (const std::bad_alloc&) {
    for (size_t index = 0u; index < retained_count; index++)
      ReleaseRawVifPayload(&retained[index]);
    return false;
  }

  for (size_t index = 0u; index < payload_count; index++) {
    m_input_payloads.push_back(retained[index]);
    retained[index] = {};
  }
  for (size_t index = 0u; index < binding_count; index++)
    streams.push_back(bindings[index]);
  m_generated_input_window = resolved_window;
  m_generated_input_payload_count = payload_count;
  m_generated_input_stream_count = binding_count;
  m_generated_input_window_valid = require_generated_window;
  m_generated_input_plan_installed = true;
  return true;
}

bool GpuVuDraw::TransferInputPayloadOwnership(
    size_t index, RawVifPayloadRef* destination) {
  if (!destination || destination->IsValid() ||
      index >= m_input_payloads.size() ||
      !m_input_payloads[index].IsValid()) {
    return false;
  }
  *destination = m_input_payloads[index];
  m_input_payloads[index] = {};
  return true;
}

bool GpuVuDraw::SetCompactRawInputWords(std::vector<u32> words) {
  if (m_validated_for_queue || m_generated_input_plan_installed ||
      !m_input_payloads.empty() ||
      !m_compact_raw_input_words.empty() || words.empty() ||
      (words.size() & 3u) != 0u) {
    return false;
  }
  m_compact_raw_input_words = std::move(words);
  return true;
}

bool GpuVuDraw::SetStructuredDirectInput(
    const RawVifPayloadRef& payload) {
  if (m_structured_direct_input_span !=
          std::numeric_limits<u16>::max() ||
      m_input_payloads.size() >= std::numeric_limits<u16>::max() ||
      !AddInputPayload(payload)) {
    return false;
  }
  m_structured_direct_input_span =
      static_cast<u16>(m_input_payloads.size() - 1u);
  return true;
}

bool GpuVuDraw::ConfigurePrivateStoreJournal(
    u8 stores_per_invocation,
    std::vector<PrivateStoreExpectation> expectations) {
  if (m_validated_for_queue || HasExpandedExactIndices() || stores_per_invocation == 0u ||
      stores_per_invocation > 8u || !private_store_expectations.empty()) {
    return false;
  }
  const u32 private_invocations = PrivateStoreInvocationCount();
  if (private_invocations == 0u)
    return false;
  if (!expectations.empty() &&
      expectations.size() !=
          static_cast<size_t>(private_invocations) * stores_per_invocation) {
    return false;
  }
  if (private_store_count != 0u &&
      private_store_count != stores_per_invocation) {
    return false;
  }
  private_store_count = stores_per_invocation;
  m_requires_gpu_private_store_output = true;
  cpu_shadow_private_compare = !expectations.empty();
  private_store_expectations = std::move(expectations);
  return true;
}

bool GpuVuDraw::ConfigureGeneratedLoopKernelTransaction(
    u8 stores_per_invocation,
    std::shared_ptr<GeneratedLoopKernelTransaction> transaction,
    bool requires_gpu_private_store_output) {
  if (m_validated_for_queue || IsPrivateStateCanary() || !transaction ||
      (HasExpandedExactIndices() && requires_gpu_private_store_output) ||
      stores_per_invocation == 0u || stores_per_invocation > 8u ||
      (private_store_count != 0u &&
       private_store_count != stores_per_invocation) ||
      cpu_shadow_private_compare ||
      !private_store_expectations.empty() ||
      transaction->StoreEntryCount() !=
          PrivateStoreInvocationCount() * stores_per_invocation) {
    return false;
  }
  private_store_count = stores_per_invocation;
  m_requires_gpu_private_store_output = requires_gpu_private_store_output;
  m_generated_loop_kernel_transaction = std::move(transaction);
  final_state.vf_mask = 0xfffffffeu;
  final_state.vi_mask = final_vi_write_mask;
  final_state.acc = true;
  final_state.q = true;
  final_state.p = true;
  final_state.i = true;
  final_state.memory = true;
  return true;
}

bool GpuVuDraw::ConfigureFtoiProbeExpectations(
    std::vector<FtoiProbeExpectation> expectations) {
  if (m_validated_for_queue || !cpu_shadow_private_compare ||
      private_store_count == 0u || !ftoi_probe_expectations.empty() ||
      expectations.size() != PrivateStoreInvocationCount()) {
    return false;
  }
  for (const FtoiProbeExpectation& expectation : expectations) {
    if (expectation.left_node == 0u || expectation.right_node == 0u ||
        expectation.product_node == 0u || expectation.ftoi_node == 0u) {
      return false;
    }
  }
  ftoi_probe_expectations = std::move(expectations);
  return true;
}

bool GpuVuDraw::ConfigurePrivateArchitecturalStateExpectation(
    PrivateArchitecturalStateExpectation expectation) {
  if (m_validated_for_queue || !cpu_shadow_private_compare ||
      m_private_architectural_state_configured ||
      expectation.final_vf_lanes[0] != 0u ||
      expectation.final_acc_lanes != 0x0fu || !expectation.final_q ||
      !expectation.final_p || !expectation.final_i ||
      !expectation.playable_profile_available ||
      (expectation.final_vi_write_mask & ~0xfffeu) != 0u ||
      expectation.unique_resume_pc > 0x4000u ||
      (expectation.unique_resume_pc & 7u) != 0u) {
    return false;
  }
  for (u32 reg = 1u; reg < expectation.final_vf_lanes.size(); reg++) {
    if (expectation.final_vf_lanes[reg] != 0x0fu)
      return false;
  }
  m_private_architectural_state_expectation = std::move(expectation);
  m_private_architectural_state_configured = true;
  return true;
}

bool GpuVuDraw::ComparePrivateArchitecturalState(
    const u32* vf_words, const u32* acc_words,
    u32 q, u32 p, u32 i,
    const std::array<u16, 16>& vi_values, u32 tpc_bytes,
    u32* mismatch_lanes, u32* playable_mismatch_lanes,
    PrivateArchitecturalStateComparison* comparison) {
  if (m_validated_for_queue || !m_private_architectural_state_configured ||
      m_private_architectural_state_compared || !vf_words || !acc_words ||
      vi_values[0] != 0u) {
    return false;
  }

  const PrivateArchitecturalStateExpectation& expected =
      m_private_architectural_state_expectation;
  u32 mismatches = 0u;
  u32 playable_mismatches = 0u;
  if (comparison)
    *comparison = {};
  const auto record_exact_mismatch =
      [comparison](PrivateArchitecturalStateValueKind kind, u8 reg, u8 lane,
                   u32 actual, u32 exact_expected,
                   u32 playable_expected) {
        if (!comparison)
          return;
        if (comparison->exact_mismatch_count <
            comparison->exact_mismatches.size()) {
          PrivateArchitecturalStateMismatch& detail =
              comparison->exact_mismatches[
                  comparison->exact_mismatch_count++];
          detail.kind = kind;
          detail.reg = reg;
          detail.lane = lane;
          detail.actual = actual;
          detail.exact_expected = exact_expected;
          detail.playable_expected = playable_expected;
        }
        comparison->exact_mismatch_total++;
      };
  for (u32 reg = 1u; reg < expected.final_vf_lanes.size(); reg++) {
    for (u32 lane = 0u; lane < 4u; lane++) {
      if ((expected.final_vf_lanes[reg] & (0x8u >> lane)) == 0u)
        continue;
      const u32 actual = vf_words[reg * 4u + lane];
      const u32 exact_expected = expected.final_vf_values[reg][lane];
      const bool exact_mismatch = actual != exact_expected;
      mismatches += exact_mismatch;
      if (exact_mismatch) {
        record_exact_mismatch(
            PrivateArchitecturalStateValueKind::Vf,
            static_cast<u8>(reg), static_cast<u8>(lane), actual,
            exact_expected, expected.playable_final_vf_values[reg][lane]);
      }
      playable_mismatches +=
          actual !=
          expected.playable_final_vf_values[reg][lane];
    }
  }
  for (u32 lane = 0u; lane < 4u; lane++) {
    if ((expected.final_acc_lanes & (0x8u >> lane)) == 0u)
      continue;
    const u32 actual = acc_words[lane];
    const u32 exact_expected = expected.final_acc_values[lane];
    const bool exact_mismatch = actual != exact_expected;
    mismatches += exact_mismatch;
    if (exact_mismatch) {
      record_exact_mismatch(
          PrivateArchitecturalStateValueKind::Acc, 0u,
          static_cast<u8>(lane), actual, exact_expected,
          expected.playable_final_acc_values[lane]);
    }
    playable_mismatches +=
        actual != expected.playable_final_acc_values[lane];
  }
  if (expected.final_q && q != expected.final_q_value) {
    mismatches++;
    record_exact_mismatch(PrivateArchitecturalStateValueKind::Q, 0u, 0u,
                          q, expected.final_q_value,
                          expected.playable_final_q_value);
  }
  if (expected.final_p && p != expected.final_p_value) {
    mismatches++;
    record_exact_mismatch(PrivateArchitecturalStateValueKind::P, 0u, 0u,
                          p, expected.final_p_value,
                          expected.playable_final_p_value);
  }
  if (expected.final_i && i != expected.final_i_value) {
    mismatches++;
    record_exact_mismatch(PrivateArchitecturalStateValueKind::I, 0u, 0u,
                          i, expected.final_i_value,
                          expected.playable_final_i_value);
  }
  playable_mismatches +=
      expected.final_q && q != expected.playable_final_q_value;
  playable_mismatches +=
      expected.final_p && p != expected.playable_final_p_value;
  playable_mismatches +=
      expected.final_i && i != expected.playable_final_i_value;
  for (u32 reg = 1u; reg < expected.final_vi_values.size(); reg++) {
    if ((expected.final_vi_write_mask & (1u << reg)) != 0u &&
        vi_values[reg] != expected.final_vi_values[reg]) {
      mismatches++;
      playable_mismatches++;
      record_exact_mismatch(
          PrivateArchitecturalStateValueKind::Vi,
          static_cast<u8>(reg), 0u, vi_values[reg],
          expected.final_vi_values[reg], expected.final_vi_values[reg]);
    }
  }
  const bool tpc_mismatch = tpc_bytes != expected.unique_resume_pc;
  mismatches += tpc_mismatch;
  playable_mismatches += tpc_mismatch;
  if (tpc_mismatch) {
    record_exact_mismatch(PrivateArchitecturalStateValueKind::Tpc, 0u, 0u,
                          tpc_bytes, expected.unique_resume_pc,
                          expected.unique_resume_pc);
  }
  m_private_architectural_state_mismatch_lanes = mismatches;
  m_private_architectural_state_exact = mismatches == 0u;
  m_private_architectural_state_playable_mismatch_lanes =
      playable_mismatches;
  m_private_architectural_state_playable_profile_matches =
      expected.playable_profile_available && playable_mismatches == 0u;
  m_private_architectural_state_compared = true;
  if (mismatch_lanes)
    *mismatch_lanes = mismatches;
  if (playable_mismatch_lanes)
    *playable_mismatch_lanes = playable_mismatches;
  return true;
}

bool GpuVuDraw::ConfigureGeneratedLoopKernelAttestation(
    GeneratedLoopKernelAttestationIdentity identity) {
  if (m_validated_for_queue || !identity.IsValid() ||
      HasGeneratedLoopKernelAttestation() || !(identity.key == program)) {
    return false;
  }
  m_generated_loop_kernel_attestation = identity;
  return true;
}

bool GpuVuDraw::ConfigureGeneratedLoopKernelProductAttestation(
    GeneratedLoopKernelAttestationIdentity canary_identity) {
  if (m_validated_for_queue || IsPrivateStateCanary() || !canary_identity.IsValid() ||
      HasGeneratedLoopKernelAttestation() ||
      canary_identity.key == program ||
      (program.low == 0u && program.high == 0u)) {
    return false;
  }
  m_generated_loop_kernel_attestation = canary_identity;
  return true;
}

bool GpuVuDraw::ConfigureGeneratedLoopKernelExecutableAttestation(
    GeneratedLoopKernelAttestationIdentity canary_identity,
    bool uses_private_store_output) {
  if (IsPrivateStateCanary())
    return false;
  const bool self_attested = canary_identity.key == program;
  if (self_attested != uses_private_store_output)
    return false;
  return self_attested
             ? ConfigureGeneratedLoopKernelAttestation(canary_identity)
             : ConfigureGeneratedLoopKernelProductAttestation(
                   canary_identity);
}

bool GpuVuDraw::ConfigurePrivateStatePoints() {
  if (m_validated_for_queue || primitive_boundary != PrimitiveBoundary::Native ||
      HasExactIndices() || HasGeneratedLoopKernelTransaction() ||
      HasGeneratedLoopKernelAttestation() || private_store_count != 0u ||
      cpu_shadow_private_compare || execution != ExecutionKind::GeneratedParallel ||
      lowering != OutputLowering::DirectTfx || invocation_count == 0u ||
      invocation_count > (1u << 16u) || vertex_count != invocation_count ||
      direct_tfx.vertex_count != invocation_count)
    return false;
  primitive_boundary = PrimitiveBoundary::PrivateStatePoints;
  primitive_count = invocation_count;
  index_count = invocation_count;
  return true;
}

u32 GpuVuDraw::ExactIndicesPerPrimitive() const {
  switch (direct_tfx.primitive) {
  case GS_LINESTRIP: return 2u;
  case GS_TRIANGLELIST:
  case GS_TRIANGLESTRIP: return 3u;
  default: return 0u;
  }
}

u32 GpuVuDraw::ShaderIndexDomainCount() const {
  if (HasExpandedExactIndices())
    return direct_tfx.primitive == GS_LINESTRIP
        ? DirectTfxFlatLineIndexDomain(invocation_count) : 0u;
  return primitive_boundary == PrimitiveBoundary::ExpandedIndexed
      ? index_count : invocation_count;
}

bool GpuVuDraw::IsExactPostLoopIndexShapeValid(
    std::span<const u16> indices, bool expanded) const {
  const u32 width = ExactIndicesPerPrimitive();
  const u32 domain = expanded ? DirectTfxFlatLineIndexDomain(invocation_count)
                              : invocation_count;
  if (IsPrivateStateCanary() || indices.empty() || width == 0u || (indices.size() % width) != 0u ||
      indices.size() > std::numeric_limits<u32>::max() || domain == 0u ||
      domain > (1u << 16u) ||
      (expanded ? (direct_tfx.primitive != GS_LINESTRIP || direct_tfx.gouraud ||
                   HasPrivateStoreJournal() || cpu_shadow_private_compare)
                : !direct_tfx.gouraud))
    return false;
  for (u16 index : indices) {
    if (index >= domain)
      return false;
  }
  if (expanded) {
    for (size_t first = 0u; first < indices.size(); first += 2u) {
      if ((indices[first] & 1u) != 0u ||
          indices[first + 1u] != static_cast<u32>(indices[first]) + 1u ||
          (first != 0u && indices[first] <= indices[first - 2u]))
        return false;
    }
  }
  return true;
}

bool GpuVuDraw::SetExactPostLoopIndices(std::vector<u16> indices) {
  const bool expanded = primitive_boundary == PrimitiveBoundary::ExpandedIndexed ||
                        HasExpandedExactIndices();
  if (m_validated_for_queue ||
      m_shared_exact_indices || m_shared_exact_indices_owner ||
      !IsExactPostLoopIndexShapeValid(indices, expanded))
    return false;
  exact_indices = std::move(indices);
  index_count = static_cast<u32>(exact_indices.size());
  primitive_count = index_count / ExactIndicesPerPrimitive();
  primitive_boundary = expanded ? PrimitiveBoundary::ExactPostLoopExpandedIndexed
                                : PrimitiveBoundary::ExactPostLoopIndexed;
  return true;
}

bool GpuVuDraw::SetSharedExactPostLoopIndices(
    const std::vector<u16>& indices,
    std::shared_ptr<const void> lifetime_owner) {
  const bool expanded = primitive_boundary == PrimitiveBoundary::ExpandedIndexed ||
                        HasExpandedExactIndices();
  if (m_validated_for_queue || !exact_indices.empty() ||
      m_shared_exact_indices || m_shared_exact_indices_owner ||
      !lifetime_owner || !IsExactPostLoopIndexShapeValid(indices, expanded))
    return false;
  m_shared_exact_indices = &indices;
  m_shared_exact_indices_owner = std::move(lifetime_owner);
  index_count = static_cast<u32>(indices.size());
  primitive_count = index_count / ExactIndicesPerPrimitive();
  primitive_boundary = expanded ? PrimitiveBoundary::ExactPostLoopExpandedIndexed
                                : PrimitiveBoundary::ExactPostLoopIndexed;
  return true;
}

bool GpuVuDraw::CapturePrivateStoreExpected(
    const void* vu_memory, size_t bytes) {
  if (!cpu_shadow_private_compare || m_private_store_expected_captured ||
      !vu_memory || bytes < 1024u * 16u || private_store_count == 0u ||
      private_store_expectations.size() !=
          static_cast<size_t>(PrivateStoreInvocationCount()) *
              private_store_count) {
    return false;
  }

  const u32* const words = static_cast<const u32*>(vu_memory);
  for (PrivateStoreExpectation& expectation : private_store_expectations) {
    if (expectation.address_qword >= 1024u || expectation.lane_mask == 0u ||
        (expectation.lane_mask & ~0x0fu) != 0u) {
      return false;
    }
    const u32* const qword = words + expectation.address_qword * 4u;
    for (u32 lane = 0u; lane < 4u; lane++) {
      if ((expectation.lane_mask & (0x8u >> lane)) != 0u)
        expectation.expected[lane] = qword[lane];
    }
  }
  m_private_store_expected_captured = true;
  return true;
}

const InlineDescriptorVector<ConstantUniform, 32>&
GpuVuDraw::ConstantUniforms() const {
  static const InlineDescriptorVector<ConstantUniform, 32> empty;
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
  if (IsPrivateStateCanary() &&
      (execution != ExecutionKind::GeneratedParallel ||
       lowering != OutputLowering::DirectTfx || !HasPrivateStoreJournal() ||
       !HasPrivateStoreComparison() || HasGeneratedLoopKernelTransaction() ||
       HasExactIndices() || index_count != invocation_count ||
       primitive_count != invocation_count || invocation_count > (1u << 16u) ||
       GeneratedLoopKernelAttestation().key != program)) {
    return Fail(error, "private state canary lacks a complete non-committing domain");
  }
  if (precompute_program_count > precompute_programs.size() ||
      precompute_stage_count > GpuVuDirectPrecomputeMaximumStages ||
      ((precompute_program_count == 0) !=
       (precompute_stage_count == 0))) {
    return Fail(error, "invalid generated direct precompute dimensions");
  }
  std::array<bool, GpuVuDirectPrecomputeMaximumStages> represented_stages{};
  for (u32 index = 0; index < precompute_program_count; index++) {
    if ((precompute_programs[index].low == 0 &&
         precompute_programs[index].high == 0) ||
        precompute_stages[index] >= precompute_stage_count ||
        (index != 0 &&
         precompute_stages[index] < precompute_stages[index - 1])) {
      return Fail(error, "invalid generated direct precompute program");
    }
    represented_stages[precompute_stages[index]] = true;
  }
  for (u32 stage = 0; stage < precompute_stage_count; stage++) {
    if (!represented_stages[stage])
      return Fail(error, "generated direct precompute stage is empty");
  }
  if (lowering != OutputLowering::RawPath1Export &&
      (vertex_count == 0 || primitive_count == 0 || index_count == 0)) {
    return Fail(error, "empty GPU VU draw dimensions");
  }
  if (lowering == OutputLowering::DirectTfx &&
      (direct_tfx.vertex_count != vertex_count ||
       direct_tfx.vertex_count != invocation_count)) {
    return Fail(error, "direct TFX invocation/vertex contract mismatch");
  }
  if (private_store_count > 8u)
    return Fail(error, "private store journal exceeds the generated ABI");
  if (private_store_count != 0u && PrivateStoreInvocationCount() == 0u)
    return Fail(error, "private store journal has an empty invocation domain");
  if (HasGeneratedLoopKernelTransaction()) {
    const auto& transaction = m_generated_loop_kernel_transaction;
    const u64 expected_entries =
        static_cast<u64>(PrivateStoreInvocationCount()) *
        private_store_count;
    if (execution != ExecutionKind::GeneratedParallel ||
        lowering != OutputLowering::DirectTfx ||
        cpu_shadow_private_compare || private_store_count == 0u ||
        expected_entries != transaction->StoreEntryCount() ||
        ((transaction->UsesGpuNativeOutputOnlyStoreCommit() ||
          transaction->UsesDeferredPairPlanStoreCommit())
             ? !transaction->ExactStoreWords().empty()
             : transaction->ExactStoreWords().size() !=
                   transaction->OutputWordCount()) ||
        transaction->OutputNumericProfile() ==
            GeneratedLoopKernelNumericProfile::None ||
        !HasGeneratedLoopKernelAttestation() ||
        transaction->Sequence() != ordering_sequence ||
        transaction->Stage() !=
            GeneratedLoopKernelTransactionStage::Prepared ||
        final_vi_values != transaction->FinalViValues() ||
        final_vi_write_mask != transaction->FinalViWriteMask() ||
        unique_resume_pc != transaction->UniqueResumePc() ||
        !final_state.IsRequired() || !final_state.memory ||
        final_state.vf_mask != 0xfffffffeu ||
        final_state.vi_mask != final_vi_write_mask || !final_state.acc ||
        !final_state.q || !final_state.p || !final_state.i ||
        final_state.flags || final_state.vif) {
      return Fail(error,
                  "generated loop-kernel transaction is incomplete");
    }
  } else if (final_state.IsRequired()) {
    return Fail(error, "final-state publication has no transaction owner");
  }
  if (cpu_shadow_private_compare) {
    const size_t expected_entries =
        static_cast<size_t>(PrivateStoreInvocationCount()) *
        private_store_count;
    if (execution != ExecutionKind::GeneratedParallel ||
        lowering != OutputLowering::DirectTfx || private_store_count == 0u ||
        private_store_expectations.size() != expected_entries ||
        !m_private_store_expected_captured) {
      return Fail(error, "private store oracle journal is incomplete");
    }
    if (!HasGeneratedLoopKernelAttestation() ||
        !m_private_architectural_state_configured ||
        !m_private_architectural_state_compared) {
      return Fail(error,
                  "private generated attestation lacks architectural state");
    }
    for (const PrivateStoreExpectation& expectation :
         private_store_expectations) {
      if (expectation.address_qword >= 1024u ||
          expectation.lane_mask == 0u ||
          (expectation.lane_mask & ~0x0fu) != 0u ||
          expectation.exact_profile_mask != expectation.lane_mask ||
          expectation.playable_profile_mask != expectation.lane_mask ||
          (expectation.output_only_mask & ~expectation.lane_mask) != 0u) {
        return Fail(error, "private store oracle entry is invalid");
      }
    }
    if (!ftoi_probe_expectations.empty() &&
        ftoi_probe_expectations.size() != PrivateStoreInvocationCount()) {
      return Fail(error, "FTOI probe oracle domain is incomplete");
    }
    for (const FtoiProbeExpectation& expectation : ftoi_probe_expectations) {
      if (expectation.left_node == 0u || expectation.right_node == 0u ||
          expectation.product_node == 0u || expectation.ftoi_node == 0u) {
        return Fail(error, "FTOI probe oracle node is invalid");
      }
    }
  } else if (!private_store_expectations.empty() ||
             !ftoi_probe_expectations.empty() ||
             m_private_store_expected_captured ||
             m_private_architectural_state_configured ||
             m_private_architectural_state_compared ||
             (HasGeneratedLoopKernelAttestation() &&
              !HasGeneratedLoopKernelTransaction())) {
    return Fail(error, "private store expectations lack oracle ownership");
  }
  if (HasExactIndices()) {
    const std::vector<u16>& resolved_indices = ExactIndices();
    if ((primitive_boundary != PrimitiveBoundary::ExactPostLoopIndexed &&
         !HasExpandedExactIndices()) ||
        !IsExactPostLoopIndexShapeValid(resolved_indices, HasExpandedExactIndices()) ||
        resolved_indices.size() != index_count ||
        primitive_count != index_count / ExactIndicesPerPrimitive()) {
      return Fail(error, "exact post-loop index geometry is inconsistent");
    }
  } else if (primitive_boundary ==
                 PrimitiveBoundary::ExactPostLoopIndexed || HasExpandedExactIndices()) {
    return Fail(error, "exact post-loop boundary has no indices");
  }
  if (streams.size() > 16)
    return Fail(error, "more than 16 generated vertex streams");
  if (HasCompactRawInputs() && !m_input_payloads.empty())
    return Fail(error, "compact and raw-ring GPU VU inputs overlap");

  u32 attribute_mask = 0;
  for (const StreamBinding &stream : streams) {
    if (stream.attribute_index >= 16 ||
        (attribute_mask & (1u << stream.attribute_index)) != 0) {
      return Fail(error, "duplicate or invalid generated attribute index");
    }
    attribute_mask |= 1u << stream.attribute_index;
    u64 input_bytes = 0u;
    if (stream.owner == StreamInputOwner::CanonicalVuMemory) {
      constexpr u64 CanonicalVuMemoryBytes = 16u * 1024u;
      if (HasCompactRawInputs() ||
          stream.input_span != std::numeric_limits<u16>::max()) {
        return Fail(error,
                    "canonical generated stream has a raw input owner");
      }
      input_bytes = CanonicalVuMemoryBytes;
    } else if (stream.owner == StreamInputOwner::RawInput) {
      input_bytes = HasCompactRawInputs()
          ? static_cast<u64>(m_compact_raw_input_words.size()) * sizeof(u32)
          : (stream.input_span < m_input_payloads.size()
                 ? m_input_payloads[stream.input_span].size
                 : 0u);
      if (HasCompactRawInputs()) {
        if (stream.input_span != std::numeric_limits<u16>::max()) {
          return Fail(error,
                      "compact generated stream has a raw-ring span index");
        }
      } else {
        if (stream.input_span >= m_input_payloads.size())
          return Fail(error, "generated stream refers to a missing VIF span");
        if (!m_input_payloads[stream.input_span].IsValid())
          return Fail(error, "generated stream refers to an invalid VIF span");
      }
    } else {
      return Fail(error, "generated stream has an unknown input owner");
    }
    if (stream.payload_byte_offset > input_bytes)
      return Fail(error, "generated stream begins outside its input table");
    if (stream.payload_byte_extent < sizeof(u128) ||
        (stream.payload_byte_extent & 15u) != 0u ||
        stream.payload_byte_extent >
            input_bytes - stream.payload_byte_offset) {
      return Fail(error, "generated stream extends outside its input table");
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
  if (unique_resume_pc > 0x4000u || (unique_resume_pc & 7u) != 0u)
    return Fail(error, "descriptor-scale resume PC is invalid");
  if (const RawVifPayloadRef* structured = StructuredDirectInput()) {
    if (!streams.empty() || structured->size != GeneratedNestedDirectInputBytes)
      return Fail(error, "structured direct input ABI differs from generated root");
    if (executed_pair_count == 0)
      return Fail(error, "structured direct draw has no executed PairPlan work");
    if (precompute_program_count != 0 &&
        invocation_count > StructuredGeneratedMaximumScratchInvocations) {
      return Fail(error, "generated direct precompute scratch range is too large");
    }
  } else if (m_structured_direct_input_span !=
             std::numeric_limits<u16>::max()) {
    return Fail(error, "structured direct input span is out of range");
  } else if (precompute_program_count != 0) {
    return Fail(error, "generated direct precompute lacks structured input");
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

bool GpuVuDraw::ValidateForQueue(std::string *error) {
  if (m_validated_for_queue)
    return true;
  if (!Validate(error))
    return false;
  m_validated_for_queue = true;
  return true;
}

bool GpuVuDraw::SealGeneratedLoopKernelProductForQueue(
    u64 sequence, std::string *error) {
  if (error)
    error->clear();
  const auto reject = [error](const char* message) {
    if (error)
      *error = message;
    return false;
  };
  const auto& transaction = m_generated_loop_kernel_transaction;
  const std::vector<u16>& resolved_indices = ExactIndices();
  if (m_validated_for_queue || ordering_sequence != 0u || sequence == 0u ||
      !transaction || transaction->Sequence() != 0u ||
      transaction->Stage() != GeneratedLoopKernelTransactionStage::Prepared) {
    return reject("generated product transaction cannot be sealed");
  }

  // Every collection below is constructed by checked mutators in
  // BuildGeneratedLoopKernelGpuVuDrawInternal(): SetExactPostLoopIndices(),
  // AddInputPayload(), SetUniformBlock(), and
  // ConfigureGeneratedLoopKernelTransaction().  Keep this final guard bounded
  // and independent of invocation/store/index counts.
  const u64 expected_store_entries =
      static_cast<u64>(PrivateStoreInvocationCount()) * private_store_count;
  const u32 exact_width = ExactIndicesPerPrimitive();
  if ((program.low == 0u && program.high == 0u) ||
      execution != ExecutionKind::GeneratedParallel ||
      lowering != OutputLowering::DirectTfx ||
      (primitive_boundary != PrimitiveBoundary::ExactPostLoopIndexed &&
       !HasExpandedExactIndices()) ||
      exact_width == 0u || ShaderIndexDomainCount() == 0u ||
      (HasExpandedExactIndices() && (direct_tfx.gouraud || HasPrivateStoreJournal())) ||
      invocation_count == 0u || vertex_count != invocation_count ||
      primitive_count == 0u || index_count == 0u ||
      resolved_indices.size() != index_count || (index_count % exact_width) != 0u ||
      primitive_count != index_count / exact_width ||
      direct_tfx.vertex_count != vertex_count ||
      private_store_count == 0u || private_store_count > 8u ||
      expected_store_entries != transaction->StoreEntryCount() ||
      cpu_shadow_private_compare || !private_store_expectations.empty() ||
      !ftoi_probe_expectations.empty() ||
      !HasGeneratedLoopKernelAttestation() ||
      transaction->OutputNumericProfile() ==
          GeneratedLoopKernelNumericProfile::None ||
      ((transaction->UsesGpuNativeOutputOnlyStoreCommit() ||
        transaction->UsesDeferredPairPlanStoreCommit())
           ? !transaction->ExactStoreWords().empty()
           : transaction->ExactStoreWords().size() !=
                 transaction->OutputWordCount()) ||
      final_vi_values != transaction->FinalViValues() ||
      final_vi_write_mask != transaction->FinalViWriteMask() ||
      unique_resume_pc != transaction->UniqueResumePc() ||
      !final_state.IsRequired() || !final_state.memory ||
      final_state.vf_mask != 0xfffffffeu ||
      final_state.vi_mask != final_vi_write_mask || !final_state.acc ||
      !final_state.q || !final_state.p || !final_state.i ||
      final_state.flags || final_state.vif ||
      UsesQuarantinedSnapshotArchitecture()) {
    return reject("generated product descriptor changed after construction");
  }
  for (const RawVifPayloadRef& payload : m_input_payloads) {
    if (!payload.IsValid())
      return reject("generated product input generation is invalid");
  }

  GeneratedInputWindow sealed_window;
  if (HasCompactRawInputs()) {
    if (!m_input_payloads.empty() || streams.empty())
      return reject("generated product compact input ownership is invalid");
    const u64 logical_qwords = m_compact_raw_input_words.size() / 4u;
    if (logical_qwords == 0u ||
        logical_qwords > std::numeric_limits<u32>::max()) {
      return reject("generated product compact input range is invalid");
    }
    u32 first_qword = std::numeric_limits<u32>::max();
    u32 last_qword = 0u;
    for (const StreamBinding& binding : streams) {
      const u64 end = static_cast<u64>(binding.payload_byte_offset) +
                      binding.payload_byte_extent;
      if (binding.owner != StreamInputOwner::RawInput ||
          binding.input_span != std::numeric_limits<u16>::max() ||
          binding.payload_byte_extent < sizeof(u128) ||
          (binding.payload_byte_offset & 15u) != 0u ||
          (binding.payload_byte_extent & 15u) != 0u ||
          end > logical_qwords * 16u) {
        return reject("generated product compact stream range is invalid");
      }
      first_qword =
          std::min(first_qword, binding.payload_byte_offset / 16u);
      last_qword =
          std::max(last_qword, static_cast<u32>((end - 1u) / 16u));
    }
    u32 storage_qwords = 0u;
    if (first_qword == std::numeric_limits<u32>::max() ||
        !ResolveGeneratedCompactStorageQwords(
            static_cast<u32>(logical_qwords),
            GeneratedRawInputDeclaredQwords, &storage_qwords) ||
        !ResolveGeneratedBufferWindow(
            first_qword, last_qword, storage_qwords,
            GeneratedRawInputDeclaredQwords,
            GeneratedRawInputMaximumRelativeQword,
            &sealed_window.bound_first_qword)) {
      return reject("generated product compact input window is invalid");
    }
    sealed_window.required_first_qword = first_qword;
    sealed_window.required_last_qword = last_qword;
    sealed_window.uses_raw = true;
  } else {
    GeneratedInputWindowFailure window_failure =
        GeneratedInputWindowFailure::None;
    if (!m_generated_input_plan_installed ||
        !m_generated_input_window_valid) {
      return reject("generated product input plan was not installed");
    }
    if (m_generated_input_payload_count != m_input_payloads.size() ||
        m_generated_input_stream_count != streams.size()) {
      return reject("generated product input plan counts changed");
    }
    if (!ResolveGeneratedInputWindow(
            m_input_payloads.data(), m_input_payloads.size(), streams.data(),
            streams.size(), GeneratedRawInputBufferQwords,
            16u * 1024u / 16u, GeneratedRawInputDeclaredQwords,
            GeneratedRawInputMaximumRelativeQword, &sealed_window,
            &window_failure)) {
      return reject(GeneratedInputWindowFailureName(window_failure));
    }
    const GeneratedInputWindow& built = m_generated_input_window;
    if (sealed_window.owner != built.owner ||
        sealed_window.slot != built.slot ||
        sealed_window.generation != built.generation ||
        sealed_window.required_first_qword != built.required_first_qword ||
        sealed_window.required_last_qword != built.required_last_qword ||
        sealed_window.bound_first_qword != built.bound_first_qword ||
        sealed_window.uses_raw != built.uses_raw ||
        sealed_window.uses_canonical != built.uses_canonical) {
      return reject("generated product input proof changed after construction");
    }
#if !defined(VITASX2_QEMU_VALIDATION)
    for (const RawVifPayloadRef& payload : m_input_payloads) {
      if (!ResolveRawVifPayload(payload))
        return reject("generated product input generation is not committed");
    }
#endif
  }
  m_generated_input_window = sealed_window;
  m_generated_input_window_valid = true;

  transaction->SetSequence(sequence);
  ordering_sequence = sequence;
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
