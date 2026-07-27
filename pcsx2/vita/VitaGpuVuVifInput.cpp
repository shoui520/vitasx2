// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "vita/VitaGpuVuVifInput.h"

#include "vita/VitaGpuVuDraw.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <limits>
#include <thread>

#if !defined(VITASX2_QEMU_VALIDATION)
#include "common/Console.h"
#include "vita/VitaGxmMemory.h"
#include "vita/VitaGsMailbox.h"

#include <chrono>
#include <psp2/gxm.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#endif

namespace VitaGpuVu {
namespace {

constexpr u32 Vu1MemoryQwords = 1024;
constexpr u32 DirectInputEpochReservationBytes = Vu1MemoryQwords * 16u;
constexpr u32 MaximumRawInputRelativeQword =
    static_cast<u32>(std::numeric_limits<s16>::max());
static_assert(DirectInputEpochReservationBytes == 16 * 1024);

u32 NormalizedCycle(u8 value) {
  return value != 0 ? value : 256u;
}

std::atomic<u64> s_captures{0};
std::atomic<u64> s_captured_bytes{0};
std::atomic<u64> s_publication_batches{0};
std::atomic<u64> s_published_bytes{0};
std::atomic<u64> s_capture_bypasses{0};
std::atomic<u64> s_capture_bypass_bytes{0};
std::atomic<u64> s_capture_fallbacks{0};
std::atomic<u64> s_slot_reuses{0};
std::atomic<u64> s_ring_waits{0};
std::atomic<u64> s_ring_wait_spins{0};
std::atomic<u64> s_live_references{0};
std::atomic<u64> s_peak_live_references{0};
std::atomic<u64> s_deferred_unpacks{0};
std::atomic<u64> s_affine_span_merges{0};
std::atomic<u64> s_replayed_unpacks{0};

void RecordReferenceCreated() {
  const u64 live =
      s_live_references.fetch_add(1, std::memory_order_relaxed) + 1;
  u64 peak = s_peak_live_references.load(std::memory_order_relaxed);
  while (peak < live &&
         !s_peak_live_references.compare_exchange_weak(
             peak, live, std::memory_order_relaxed,
             std::memory_order_relaxed)) {
  }
}

void ResetStatistics() {
  s_captures.store(0, std::memory_order_relaxed);
  s_captured_bytes.store(0, std::memory_order_relaxed);
  s_publication_batches.store(0, std::memory_order_relaxed);
  s_published_bytes.store(0, std::memory_order_relaxed);
  s_capture_bypasses.store(0, std::memory_order_relaxed);
  s_capture_bypass_bytes.store(0, std::memory_order_relaxed);
  s_capture_fallbacks.store(0, std::memory_order_relaxed);
  s_slot_reuses.store(0, std::memory_order_relaxed);
  s_ring_waits.store(0, std::memory_order_relaxed);
  s_ring_wait_spins.store(0, std::memory_order_relaxed);
  s_live_references.store(0, std::memory_order_relaxed);
  s_peak_live_references.store(0, std::memory_order_relaxed);
  s_deferred_unpacks.store(0, std::memory_order_relaxed);
  s_affine_span_merges.store(0, std::memory_order_relaxed);
  s_replayed_unpacks.store(0, std::memory_order_relaxed);
}

#if !defined(VITASX2_QEMU_VALIDATION)

constexpr u32 PayloadAlignment = 16;

// VU_Thread::VifUnpack() is the only payload producer. Keep registration
// atomic so that the per-UNPACK path does not enter a process mutex merely to
// rediscover the same lifetime-stable ring. The process-wide capture count is
// incremented before loading the pointer: Shutdown() first unpublishes the
// pointer and then waits for this count, so a producer can never retain a
// pointer across unmapping.
std::atomic<InputRing*> s_active_ring{nullptr};
std::atomic<u32> s_active_capture_calls{0};

bool AlignUp(u32 value, u32 alignment, u32* aligned) {
  if (!aligned || alignment == 0 || (alignment & (alignment - 1)) != 0 ||
      value > std::numeric_limits<u32>::max() - (alignment - 1)) {
    return false;
  }
  *aligned = (value + alignment - 1) & ~(alignment - 1);
  return true;
}

#endif

} // namespace

bool IsDirectAffineV4_32Span(const VifUnpackSpan& span) {
  constexpr u8 CommandMaskWithoutInterrupt = 0x7f;
  constexpr u8 UnpackV4_32 = 0x6c;
  const u8 command = span.command & CommandMaskWithoutInterrupt;
  if (command != UnpackV4_32 || span.mode != 0 ||
      span.unsigned_data > 1 || span.vector_count == 0 ||
      span.destination_qword >= Vu1MemoryQwords ||
      NormalizedCycle(span.cycle_cl) != NormalizedCycle(span.cycle_wl)) {
    return false;
  }
  const u64 required_bytes =
      static_cast<u64>(span.vector_count) * 16u;
  return span.source_size >= required_bytes;
}

bool GetDirectAffineV4_32PayloadSize(const VifUnpackSpan& span,
                                     u32* payload_size) {
  if (!payload_size || !IsDirectAffineV4_32Span(span))
    return false;
  *payload_size = static_cast<u32>(span.vector_count) * sizeof(u128);
  return true;
}

bool DirectAffineSpanFullyOverwrites(const VifUnpackSpan& newer,
                                     const VifUnpackSpan& older) {
  if (!IsDirectAffineV4_32Span(newer) ||
      !IsDirectAffineV4_32Span(older)) {
    return false;
  }

  const u32 newer_start =
      static_cast<u32>(newer.destination_qword) &
      (Vu1MemoryQwords - 1);
  const u32 older_start =
      static_cast<u32>(older.destination_qword) &
      (Vu1MemoryQwords - 1);
  const u32 older_offset =
      (older_start - newer_start) & (Vu1MemoryQwords - 1);
  return older_offset < newer.vector_count &&
         older.vector_count <= newer.vector_count - older_offset;
}

bool MergeAdjacentDirectAffineV4_32Spans(VifUnpackSpan* earlier,
                                         VifUnpackSpan* later) {
  if (!earlier || !later || earlier == later ||
      !IsDirectAffineV4_32Span(*earlier) ||
      !IsDirectAffineV4_32Span(*later)) {
    return false;
  }

  // This compact representation is intentionally limited to a single
  // non-overlapping VU-memory revolution. Beyond 1024 qwords, one destination
  // would have multiple ordered source producers and BindAffineRawQwords()
  // could no longer identify it with one affine source offset.
  const u32 combined_vectors =
      static_cast<u32>(earlier->vector_count) + later->vector_count;
  const u64 earlier_bytes =
      static_cast<u64>(earlier->vector_count) * sizeof(u128);
  const u64 later_bytes =
      static_cast<u64>(later->vector_count) * sizeof(u128);
  const u64 combined_bytes = earlier_bytes + later_bytes;
  if (combined_vectors > Vu1MemoryQwords ||
      earlier->source_size != earlier_bytes ||
      later->source_size != later_bytes ||
      earlier->payload.size != earlier_bytes ||
      later->payload.size != later_bytes ||
      earlier->payload.owner != later->payload.owner ||
      earlier->payload.slot != later->payload.slot ||
      earlier->payload.generation != later->payload.generation ||
      static_cast<u64>(earlier->payload.offset) + earlier_bytes !=
          later->payload.offset ||
      static_cast<u64>(earlier->payload.offset) + combined_bytes >
          InputRingSlotSize ||
      ((static_cast<u32>(earlier->destination_qword) +
        earlier->vector_count) &
       (Vu1MemoryQwords - 1u)) != later->destination_qword) {
    return false;
  }

  earlier->payload.size = static_cast<u32>(combined_bytes);
  earlier->source_size = static_cast<u32>(combined_bytes);
  earlier->tag_size_words = static_cast<u32>(combined_bytes / sizeof(u32));
  earlier->vector_count = static_cast<u16>(combined_vectors);
  ReleaseRawVifPayload(&later->payload);
  s_affine_span_merges.fetch_add(1, std::memory_order_relaxed);
  return true;
}

bool BindAffineRawQwords(const VifUnpackSpan& span,
                        u16 first_qword_address,
                        s32 invocation_coefficient,
                        u32 invocation_count,
                        RawQwordBinding* binding) {
  if (!binding || !span.payload.IsValid() ||
      !IsDirectAffineV4_32Span(span) ||
      invocation_coefficient <= 0 || invocation_count == 0) {
    return false;
  }

  const u32 first = first_qword_address & (Vu1MemoryQwords - 1);
  const u32 destination =
      span.destination_qword & (Vu1MemoryQwords - 1);
  const u32 first_source_vector =
      (first - destination) & (Vu1MemoryQwords - 1);
  const u64 last_source_vector =
      static_cast<u64>(first_source_vector) +
      static_cast<u64>(invocation_coefficient) *
          static_cast<u64>(invocation_count - 1);
  if (last_source_vector >= span.vector_count)
    return false;

  const u64 byte_offset = static_cast<u64>(first_source_vector) * 16u;
  const u64 byte_stride =
      static_cast<u64>(invocation_coefficient) * 16u;
  const u64 last_byte = byte_offset +
      static_cast<u64>(invocation_count - 1) * byte_stride + 16u;
  if (byte_offset > std::numeric_limits<u32>::max() ||
      byte_stride > std::numeric_limits<u32>::max() ||
      last_byte > span.source_size || last_byte > span.payload.size) {
    return false;
  }

  binding->payload = span.payload;
  binding->payload_byte_offset = static_cast<u32>(byte_offset);
  binding->byte_stride = static_cast<u32>(byte_stride);
  binding->invocation_count = invocation_count;
  return true;
}

bool MaterializeDirectAffineV4_32Span(const VifUnpackSpan& span,
                                     const void* source,
                                     void* vu_memory,
                                     u32 vu_memory_size) {
  if (!source || !vu_memory || !IsDirectAffineV4_32Span(span) ||
      vu_memory_size == 0 || (vu_memory_size & 0x0fu) != 0) {
    return false;
  }

  const u64 required_bytes =
      static_cast<u64>(span.vector_count) * 16u;
  if (required_bytes > span.source_size ||
      required_bytes > std::numeric_limits<u32>::max()) {
    return false;
  }

  const u8* source_bytes = static_cast<const u8*>(source);
  u8* memory_bytes = static_cast<u8*>(vu_memory);
  u32 remaining = static_cast<u32>(required_bytes);
  u32 source_offset = 0;
  u32 destination_offset =
      (static_cast<u32>(span.destination_qword) * 16u) %
      vu_memory_size;
  while (remaining != 0) {
    const u32 chunk =
        std::min(remaining, vu_memory_size - destination_offset);
    std::memcpy(memory_bytes + destination_offset,
                source_bytes + source_offset, chunk);
    remaining -= chunk;
    source_offset += chunk;
    destination_offset = 0;
  }
  return true;
}

bool HasSingleAddressableRawInputWindow(const GpuVuDraw& draw) {
  if (draw.invocation_count == 0 || draw.streams.empty())
    return false;

  uptr owner = 0;
  u32 slot = 0;
  u32 generation = 0;
  u32 first_qword = std::numeric_limits<u32>::max();
  u32 last_qword = 0;
  for (const StreamBinding& binding : draw.streams) {
    if (binding.input_span >= draw.InputPayloads().size())
      return false;
    const RawVifPayloadRef& payload =
        draw.InputPayloads()[binding.input_span];
    if (!payload.IsValid() || payload.slot >= InputRingSlotCount ||
        payload.offset > InputRingSlotSize ||
        payload.size > InputRingSlotSize - payload.offset ||
        binding.payload_byte_offset > payload.size) {
      return false;
    }
    if (owner == 0) {
      owner = payload.owner;
      slot = payload.slot;
      generation = payload.generation;
    } else if (payload.owner != owner || payload.slot != slot ||
               payload.generation != generation) {
      return false;
    }

    const u64 relative_last =
        static_cast<u64>(binding.payload_byte_offset) +
        static_cast<u64>(draw.invocation_count - 1u) *
            binding.byte_stride +
        15u;
    if ((binding.payload_byte_offset & 15u) != 0 ||
        relative_last >= payload.size) {
      return false;
    }
    const u64 absolute_first =
        static_cast<u64>(payload.offset) + binding.payload_byte_offset;
    const u64 absolute_last =
        static_cast<u64>(payload.offset) + relative_last;
    if ((absolute_first & 15u) != 0 ||
        absolute_last >= InputRingSlotSize) {
      return false;
    }
    first_qword =
        std::min(first_qword, static_cast<u32>(absolute_first / 16u));
    last_qword =
        std::max(last_qword, static_cast<u32>(absolute_last / 16u));
  }

  return owner != 0 && first_qword != std::numeric_limits<u32>::max() &&
         last_qword - first_qword <= MaximumRawInputRelativeQword;
}

struct InputRing::Impl {
#if !defined(VITASX2_QEMU_VALIDATION)
  struct Slot {
    VitaGXM::MappedBlock block;
    std::atomic<u32> generation{1};
    std::atomic<u32> references{0};
    std::atomic<u32> committed_offset{0};
    std::atomic<u32> published_offset{0};
    u32 write_offset = 0;
  };

  std::array<Slot, InputRingSlotCount> slots;
  // VU_Thread::VifUnpack() is the sole producer. Publish the exact slot it is
  // waiting to reuse before rechecking its reference count. The last owner
  // claims that token before signaling, which makes release-before-wait and
  // shutdown races lossless without putting VitaSDK's cancellation-polling
  // pthread condition variable on this hot Cortex-A9 path.
  SceUID slot_released_sema = -1;
  std::atomic<s32> waiting_slot{-1};
  std::atomic<bool> accepting{false};
  u32 current_slot = 0;
  bool initialized = false;
#endif
};

InputRing::InputRing() : m_impl(new Impl()) {}

InputRing::~InputRing() {
  Shutdown();
  delete m_impl;
  m_impl = nullptr;
}

bool InputRing::Initialize() {
#if defined(VITASX2_QEMU_VALIDATION)
  return false;
#else
  if (!m_impl || m_impl->initialized)
    return false;

  ResetStatistics();
  m_impl->slot_released_sema =
      sceKernelCreateSema("VitaSX2 VIF slot", 0, 0, 1, nullptr);
  if (m_impl->slot_released_sema < 0) {
    Console.Warning(
        "GPU-VU: VIF input slot semaphore creation failed (%08x); "
        "retaining inline MTVU payloads.",
        static_cast<u32>(m_impl->slot_released_sema));
    m_impl->slot_released_sema = -1;
    return false;
  }
  m_impl->waiting_slot.store(-1, std::memory_order_relaxed);
  for (u32 slot = 0; slot < InputRingSlotCount; slot++) {
    char name[32];
    std::snprintf(name, sizeof(name), "VitaSX2 VIF input %u", slot);
    // libGXM's memory contract makes USER_RW coherent between CPU caches and
    // the GPU: an SGX cache miss snoops the Cortex-A9 caches. Capture directly
    // into this mapped storage so each immutable VIF byte is copied once.
    // Four independent generations and the existing retirement notification
    // still prevent the producer from overwriting bytes the GPU can observe.
    const int result = VitaGXM::AllocateMappedBlock(
        name, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, InputRingSlotSize,
        SCE_GXM_MEMORY_ATTRIB_READ, &m_impl->slots[slot].block);
    if (result < 0) {
      Console.Warning(
          "GPU-VU: VIF input slot %u allocation failed (%08x); "
          "retaining inline MTVU payloads.",
          slot, static_cast<u32>(result));
      for (u32 release = 0; release < slot; release++)
        VitaGXM::ReleaseMappedBlock(&m_impl->slots[release].block);
      sceKernelDeleteSema(m_impl->slot_released_sema);
      m_impl->slot_released_sema = -1;
      return false;
    }
    m_impl->slots[slot].generation.store(1, std::memory_order_relaxed);
    m_impl->slots[slot].references.store(0, std::memory_order_relaxed);
    m_impl->slots[slot].committed_offset.store(
        0, std::memory_order_relaxed);
    m_impl->slots[slot].published_offset.store(
        0, std::memory_order_relaxed);
    m_impl->slots[slot].write_offset = 0;
  }

  m_impl->current_slot = 0;
  m_impl->accepting.store(true, std::memory_order_release);
  InputRing* expected = nullptr;
  if (!s_active_ring.compare_exchange_strong(
          expected, this, std::memory_order_release,
          std::memory_order_relaxed)) {
    m_impl->accepting.store(false, std::memory_order_relaxed);
    for (auto& slot : m_impl->slots)
      VitaGXM::ReleaseMappedBlock(&slot.block);
    sceKernelDeleteSema(m_impl->slot_released_sema);
    m_impl->slot_released_sema = -1;
    return false;
  }
  m_impl->initialized = true;
  Console.WriteLn(
      "GPU-VU: immutable VIF input ring ready "
      "(4 x 2 MiB cacheable GPU-coherent slots, direct capture).");
  return true;
#endif
}

bool InputRing::Shutdown() {
#if defined(VITASX2_QEMU_VALIDATION)
  return true;
#else
  if (!m_impl || !m_impl->initialized)
    return true;

  InputRing* expected = this;
  s_active_ring.compare_exchange_strong(
      expected, nullptr, std::memory_order_acq_rel,
      std::memory_order_relaxed);
  m_impl->accepting.store(false, std::memory_order_release);
  if (m_impl->waiting_slot.exchange(
          -1, std::memory_order_acq_rel) >= 0 &&
      m_impl->slot_released_sema >= 0) {
    sceKernelSignalSema(m_impl->slot_released_sema, 1);
  }

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  for (;;) {
    bool busy =
        s_active_capture_calls.load(std::memory_order_acquire) != 0;
    for (const auto& slot : m_impl->slots) {
      busy |= slot.references.load(std::memory_order_acquire) != 0;
    }
    if (!busy)
      break;
    if (std::chrono::steady_clock::now() >= deadline) {
      Console.Error(
          "GPU-VU: refusing to unmap a VIF input slot with live owners.");
      return false;
    }
    std::this_thread::yield();
  }

  if (m_impl->slot_released_sema >= 0) {
    const int result =
        sceKernelDeleteSema(m_impl->slot_released_sema);
    if (result < 0) {
      Console.Error(
          "GPU-VU: VIF input slot semaphore deletion failed (%08x).",
          static_cast<u32>(result));
      return false;
    }
    m_impl->slot_released_sema = -1;
  }

  bool released = true;
  for (auto& slot : m_impl->slots) {
    if (slot.block.IsAllocated() &&
        VitaGXM::ReleaseMappedBlock(&slot.block) < 0) {
      released = false;
    }
    slot.write_offset = 0;
  }
  if (!released)
    return false;
  m_impl->current_slot = 0;
  m_impl->initialized = false;
  return true;
#endif
}

bool InputRing::IsReady() const {
#if defined(VITASX2_QEMU_VALIDATION)
  return false;
#else
  return m_impl && m_impl->initialized &&
         m_impl->accepting.load(std::memory_order_acquire);
#endif
}

bool CaptureRawVifPayload(const void* source, u32 size,
                          RawVifCaptureMode mode,
                          RawVifPayloadRef* payload) {
  if (!source || !payload || size == 0)
    return false;
  *payload = {};
#if defined(VITASX2_QEMU_VALIDATION)
  return false;
#else
  s_active_capture_calls.fetch_add(1, std::memory_order_acq_rel);
  InputRing* const ring =
      s_active_ring.load(std::memory_order_acquire);
  if (!ring || !ring->m_impl ||
      !ring->m_impl->accepting.load(std::memory_order_acquire)) {
    s_active_capture_calls.fetch_sub(1, std::memory_order_acq_rel);
    return false;
  }

  InputRing::Impl& impl = *ring->m_impl;
  const auto leave = []() {
    s_active_capture_calls.fetch_sub(1, std::memory_order_acq_rel);
  };
  if (size > InputRingSlotSize ||
      !impl.accepting.load(std::memory_order_acquire)) {
    s_capture_fallbacks.fetch_add(1, std::memory_order_relaxed);
    leave();
    return false;
  }

  u32 reserved_slot = 0;
  u32 reserved_offset = 0;
  u32 reserved_generation = 0;
  bool counted_wait = false;
  for (;;) {
    if (!impl.accepting.load(std::memory_order_acquire)) {
      s_capture_fallbacks.fetch_add(1, std::memory_order_relaxed);
      leave();
      return false;
    }

    InputRing::Impl::Slot& current =
        impl.slots[impl.current_slot];
    u32 aligned_offset = 0;
    // The generated phase-one root binds one GXM raw-buffer base for every
    // VU command epoch. VU1 data memory is 16 KiB, so reserving that much
    // address space before the first captured UNPACK keeps the common affine
    // epoch within one 2 MiB slot. An epoch containing more source data, or
    // referring back to an older slot, is still rejected by
    // HasSingleAddressableRawInputWindow() before CpuVU1 can be bypassed.
    const u32 required_capacity =
        mode == RawVifCaptureMode::BeginVuCommandEpoch
            ? std::max(size, DirectInputEpochReservationBytes)
            : size;
    if (AlignUp(current.write_offset, PayloadAlignment, &aligned_offset) &&
        aligned_offset <= current.block.size &&
        required_capacity <= current.block.size - aligned_offset) {
      reserved_slot = impl.current_slot;
      reserved_offset = aligned_offset;
      reserved_generation =
          current.generation.load(std::memory_order_relaxed);
      current.write_offset = aligned_offset + size;
      current.references.fetch_add(1, std::memory_order_release);
      RecordReferenceCreated();
      break;
    }

    const u32 next = (impl.current_slot + 1) % InputRingSlotCount;
    InputRing::Impl::Slot& candidate = impl.slots[next];
    if (candidate.references.load(std::memory_order_acquire) == 0) {
      u32 generation =
          candidate.generation.load(std::memory_order_relaxed) + 1;
      if (generation == 0)
        generation = 1;
      candidate.write_offset = 0;
      candidate.committed_offset.store(0, std::memory_order_relaxed);
      candidate.published_offset.store(0, std::memory_order_relaxed);
      candidate.generation.store(generation, std::memory_order_release);
      impl.current_slot = next;
      s_slot_reuses.fetch_add(1, std::memory_order_relaxed);
      continue;
    }

    if (!counted_wait) {
      s_ring_waits.fetch_add(1, std::memory_order_relaxed);
      counted_wait = true;
    }
    RawVifPayloadRef blocked_generation;
    blocked_generation.owner = reinterpret_cast<uptr>(ring);
    blocked_generation.slot = next;
    blocked_generation.generation =
        candidate.generation.load(std::memory_order_acquire);
    // Only the slot generation identifies the storage being reused here.
    // Give the request a nonzero sentinel size so it obeys the opaque
    // reference contract without claiming a byte range.
    blocked_generation.size = 1;
    s32 expected_waiter = -1;
    if (!impl.waiting_slot.compare_exchange_strong(
            expected_waiter, static_cast<s32>(next),
            std::memory_order_acq_rel, std::memory_order_acquire)) {
      Console.Error(
          "GPU-VU: VIF input ring found a second slot waiter (%d).",
          expected_waiter);
      s_capture_fallbacks.fetch_add(1, std::memory_order_relaxed);
      leave();
      return false;
    }

    // Publishing the waiter before this recheck closes both release-before-
    // wait and shutdown-before-wait races. If the producer clears its own
    // token there is nothing to consume. If another thread already claimed
    // it, that thread has posted exactly one semaphore signal.
    const bool should_wait =
        impl.accepting.load(std::memory_order_acquire) &&
        candidate.references.load(std::memory_order_acquire) != 0;
    if (!should_wait) {
      s32 owned_waiter = static_cast<s32>(next);
      if (impl.waiting_slot.compare_exchange_strong(
              owned_waiter, -1, std::memory_order_acq_rel,
              std::memory_order_acquire)) {
        continue;
      }
    }

    VitaGS::RequestGpuVuInputRetirement(blocked_generation);
    const int wait_result = sceKernelWaitSema(
        impl.slot_released_sema, 1, nullptr);
    const s32 waiter_after_wait =
        impl.waiting_slot.load(std::memory_order_acquire);
    s32 owned_waiter = static_cast<s32>(next);
    impl.waiting_slot.compare_exchange_strong(
        owned_waiter, -1, std::memory_order_acq_rel,
        std::memory_order_acquire);
    if (wait_result < 0) {
      Console.Error(
          "GPU-VU: VIF input slot wait failed (%08x, slot=%u refs=%u "
          "waiter=%d).",
          static_cast<u32>(wait_result), next,
          candidate.references.load(std::memory_order_acquire),
          waiter_after_wait);
      s_capture_fallbacks.fetch_add(1, std::memory_order_relaxed);
      leave();
      return false;
    }
    // Retain the v=1 telemetry field name for comparable hardware logs. This
    // now counts native semaphore wakeups, never polling spins.
    s_ring_wait_spins.fetch_add(1, std::memory_order_relaxed);
  }

  InputRing::Impl::Slot& slot = impl.slots[reserved_slot];
  std::memcpy(static_cast<u8*>(slot.block.base) + reserved_offset,
              source, size);
  payload->owner = reinterpret_cast<uptr>(ring);
  payload->slot = reserved_slot;
  payload->generation = reserved_generation;
  payload->offset = reserved_offset;
  payload->size = size;
  // This is the sole producer. Publishing the contiguous prefix after its
  // bytes are initialized lets the MTVU worker copy many tiny UNPACK payloads
  // to the GXM mapping in one large sequential operation.
  slot.committed_offset.store(
      reserved_offset + size, std::memory_order_release);
  s_captures.fetch_add(1, std::memory_order_relaxed);
  s_captured_bytes.fetch_add(size, std::memory_order_relaxed);
  leave();
  return true;
#endif
}

const u8* ResolveRawVifPayload(const RawVifPayloadRef& payload) {
  if (!payload.IsValid())
    return nullptr;
#if defined(VITASX2_QEMU_VALIDATION)
  return nullptr;
#else
  auto* ring = reinterpret_cast<InputRing*>(payload.owner);
  if (!ring || !ring->m_impl || payload.slot >= InputRingSlotCount)
    return nullptr;
  const auto& slot = ring->m_impl->slots[payload.slot];
  if (!slot.block.IsMapped() ||
      slot.generation.load(std::memory_order_acquire) !=
          payload.generation ||
      payload.offset > InputRingSlotSize ||
      payload.size > InputRingSlotSize - payload.offset ||
      slot.committed_offset.load(std::memory_order_acquire) <
          payload.offset + payload.size ||
      slot.references.load(std::memory_order_acquire) == 0) {
    return nullptr;
  }
  return static_cast<const u8*>(slot.block.base) + payload.offset;
#endif
}

const u8* ResolveGpuRawVifPayload(const RawVifPayloadRef& payload) {
  if (!payload.IsValid())
    return nullptr;
#if defined(VITASX2_QEMU_VALIDATION)
  return nullptr;
#else
  auto* ring = reinterpret_cast<InputRing*>(payload.owner);
  if (!ring || !ring->m_impl || payload.slot >= InputRingSlotCount)
    return nullptr;
  const auto& slot = ring->m_impl->slots[payload.slot];
  if (!slot.block.IsMapped() ||
      slot.generation.load(std::memory_order_acquire) !=
          payload.generation ||
      payload.offset > slot.block.size ||
      payload.size > slot.block.size - payload.offset ||
      slot.published_offset.load(std::memory_order_acquire) <
          payload.offset + payload.size ||
      slot.references.load(std::memory_order_acquire) == 0) {
    return nullptr;
  }
  return static_cast<const u8*>(slot.block.base) + payload.offset;
#endif
}

bool PublishPendingRawVifPayloads(const GpuVuDraw* first_draw,
                                 u32 draw_count) {
#if !defined(VITASX2_QEMU_VALIDATION)
  InputRing* const ring = s_active_ring.load(std::memory_order_acquire);
  if (!ring || !ring->m_impl ||
      !ring->m_impl->accepting.load(std::memory_order_acquire)) {
    Console.Error(
        "GPU-VU: input publication rejected (ring unavailable, draws=%u).",
        draw_count);
    return false;
  }

  std::array<RawVifPayloadRef, InputRingSlotCount> generations{};
  u32 generation_count = 0;
  const GpuVuDraw* draw = first_draw;
  for (u32 draw_index = 0; draw_index < draw_count; draw_index++) {
    if (!draw) {
      Console.Error(
          "GPU-VU: input publication rejected "
          "(short direct run at %u/%u).",
          draw_index, draw_count);
      return false;
    }
    for (const RawVifPayloadRef& payload : draw->InputPayloads()) {
      if (!payload.IsValid() ||
          payload.owner != reinterpret_cast<uptr>(ring) ||
          payload.slot >= InputRingSlotCount) {
        Console.Error(
            "GPU-VU: input publication rejected "
            "(bad payload at draw %u: owner=%08x expected=%08x "
            "slot=%u generation=%u size=%u).",
            draw_index, static_cast<u32>(payload.owner),
            static_cast<u32>(reinterpret_cast<uptr>(ring)),
            payload.slot, payload.generation, payload.size);
        return false;
      }
      bool seen = false;
      for (u32 index = 0; index < generation_count; index++) {
        if (generations[index].slot == payload.slot &&
            generations[index].generation == payload.generation) {
          seen = true;
          break;
        }
      }
      if (!seen) {
        if (generation_count >= generations.size()) {
          Console.Error(
              "GPU-VU: input publication rejected "
              "(more than %u live slot generations at draw %u, "
              "slot=%u generation=%u).",
              InputRingSlotCount, draw_index, payload.slot,
              payload.generation);
          return false;
        }
        generations[generation_count++] = payload;
      }
    }
    draw = draw->path1_next;
  }

  u64 published_bytes = 0;
  for (u32 index = 0; index < generation_count; index++) {
    const RawVifPayloadRef& payload = generations[index];
    auto& slot = ring->m_impl->slots[payload.slot];
    // The pending draw owns a reference to this exact generation. Therefore
    // the producer cannot enter the references==0 reuse transition while its
    // contiguous committed prefix is copied.
    if (slot.generation.load(std::memory_order_acquire) !=
            payload.generation ||
        slot.references.load(std::memory_order_acquire) == 0) {
      Console.Error(
          "GPU-VU: input publication rejected "
          "(retired generation slot=%u wanted=%u actual=%u refs=%u).",
          payload.slot, payload.generation,
          slot.generation.load(std::memory_order_relaxed),
          slot.references.load(std::memory_order_relaxed));
      return false;
    }
    const u32 committed =
        slot.committed_offset.load(std::memory_order_acquire);
    const u32 published =
        slot.published_offset.load(std::memory_order_relaxed);
    if (committed <= published)
      continue;
    if (!slot.block.IsMapped() || committed > slot.block.size) {
      Console.Error(
          "GPU-VU: input publication rejected "
          "(bad range slot=%u mapped=%u committed=%u size=%u).",
          payload.slot,
          slot.block.IsMapped() ? 1u : 0u, committed, slot.block.size);
      return false;
    }

    // Capture wrote the cacheable GXM mapping itself. The producer's release
    // publication makes those initialized bytes visible to this thread, and
    // libGXM's documented CPU/GPU coherence makes them visible to SGX without
    // a second copy or a cache-maintenance operation.
    slot.published_offset.store(committed, std::memory_order_release);
    published_bytes += committed - published;
  }
  if (published_bytes != 0) {
    s_publication_batches.fetch_add(1, std::memory_order_relaxed);
    s_published_bytes.fetch_add(published_bytes,
                                std::memory_order_relaxed);
  }
  return true;
#else
  (void)first_draw;
  (void)draw_count;
  return true;
#endif
}

bool RetainRawVifPayload(const RawVifPayloadRef& payload) {
#if defined(VITASX2_QEMU_VALIDATION)
  // ARM validation descriptors use opaque fake owners: there is no GXM ring
  // to retain, but preserving the payload lets semantic fixtures exercise the
  // same descriptor construction and destruction paths as the Vita product.
  return payload.IsValid();
#else
  if (!ResolveRawVifPayload(payload))
    return false;
  auto* ring = reinterpret_cast<InputRing*>(payload.owner);
  auto& slot = ring->m_impl->slots[payload.slot];
  slot.references.fetch_add(1, std::memory_order_acq_rel);
  RecordReferenceCreated();
  return true;
#endif
}

void ReleaseRawVifPayload(RawVifPayloadRef* payload) {
  if (!payload || !payload->IsValid())
    return;
#if !defined(VITASX2_QEMU_VALIDATION)
  auto* ring = reinterpret_cast<InputRing*>(payload->owner);
  if (ring && ring->m_impl && payload->slot < InputRingSlotCount) {
    auto& slot = ring->m_impl->slots[payload->slot];
    if (slot.generation.load(std::memory_order_acquire) ==
        payload->generation) {
      const u32 previous =
          slot.references.fetch_sub(1, std::memory_order_acq_rel);
      if (previous != 0) {
        s_live_references.fetch_sub(1, std::memory_order_relaxed);
        if (previous == 1) {
          s32 expected_waiter = static_cast<s32>(payload->slot);
          if (ring->m_impl->waiting_slot.compare_exchange_strong(
                  expected_waiter, -1, std::memory_order_acq_rel,
                  std::memory_order_acquire)) {
            const int result = sceKernelSignalSema(
                ring->m_impl->slot_released_sema, 1);
            if (result < 0) {
              Console.Error(
                  "GPU-VU: VIF input slot signal failed (%08x).",
                  static_cast<u32>(result));
            }
          }
        }
      } else {
        slot.references.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }
#endif
  *payload = {};
}

u32 GetRawVifPayloadGenerationReferenceCount(
    const RawVifPayloadRef& payload) {
#if defined(VITASX2_QEMU_VALIDATION)
  (void)payload;
  return 0;
#else
  if (!payload.IsValid() || payload.slot >= InputRingSlotCount)
    return 0;
  auto* ring = reinterpret_cast<InputRing*>(payload.owner);
  if (!ring || !ring->m_impl)
    return 0;
  const auto& slot = ring->m_impl->slots[payload.slot];
  if (slot.generation.load(std::memory_order_acquire) !=
      payload.generation) {
    return 0;
  }
  return slot.references.load(std::memory_order_acquire);
#endif
}

void RecordDeferredVifUnpack() {
  s_deferred_unpacks.fetch_add(1, std::memory_order_relaxed);
}

void RecordReplayedVifUnpack() {
  s_replayed_unpacks.fetch_add(1, std::memory_order_relaxed);
}

void RecordCaptureBypass(u32 size) {
  s_capture_bypasses.fetch_add(1, std::memory_order_relaxed);
  s_capture_bypass_bytes.fetch_add(size, std::memory_order_relaxed);
}

InputRingStatistics GetInputRingStatistics() {
  InputRingStatistics stats;
  stats.captures = s_captures.load(std::memory_order_relaxed);
  stats.captured_bytes =
      s_captured_bytes.load(std::memory_order_relaxed);
  stats.publication_batches =
      s_publication_batches.load(std::memory_order_relaxed);
  stats.published_bytes =
      s_published_bytes.load(std::memory_order_relaxed);
  stats.capture_bypasses =
      s_capture_bypasses.load(std::memory_order_relaxed);
  stats.capture_bypass_bytes =
      s_capture_bypass_bytes.load(std::memory_order_relaxed);
  stats.capture_fallbacks =
      s_capture_fallbacks.load(std::memory_order_relaxed);
  stats.slot_reuses = s_slot_reuses.load(std::memory_order_relaxed);
  stats.ring_waits = s_ring_waits.load(std::memory_order_relaxed);
  stats.ring_wait_spins =
      s_ring_wait_spins.load(std::memory_order_relaxed);
  stats.live_references =
      s_live_references.load(std::memory_order_relaxed);
  stats.peak_live_references =
      s_peak_live_references.load(std::memory_order_relaxed);
  stats.deferred_unpacks =
      s_deferred_unpacks.load(std::memory_order_relaxed);
  stats.affine_span_merges =
      s_affine_span_merges.load(std::memory_order_relaxed);
  stats.replayed_unpacks =
      s_replayed_unpacks.load(std::memory_order_relaxed);
  return stats;
}

} // namespace VitaGpuVu
