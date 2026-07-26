// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "vita/VitaGpuVuVifInput.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>

#if !defined(VITASX2_QEMU_VALIDATION)
#include "common/Console.h"
#include "vita/VitaGxmMemory.h"

#include <chrono>
#include <psp2/gxm.h>
#include <psp2/kernel/sysmem.h>
#endif

namespace VitaGpuVu {
namespace {

constexpr u32 Vu1MemoryQwords = 1024;

u32 NormalizedCycle(u8 value) {
  return value != 0 ? value : 256u;
}

std::atomic<u64> s_captures{0};
std::atomic<u64> s_captured_bytes{0};
std::atomic<u64> s_capture_fallbacks{0};
std::atomic<u64> s_slot_reuses{0};
std::atomic<u64> s_ring_waits{0};
std::atomic<u64> s_ring_wait_spins{0};
std::atomic<u64> s_live_references{0};
std::atomic<u64> s_peak_live_references{0};
std::atomic<u64> s_deferred_unpacks{0};
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
  s_capture_fallbacks.store(0, std::memory_order_relaxed);
  s_slot_reuses.store(0, std::memory_order_relaxed);
  s_ring_waits.store(0, std::memory_order_relaxed);
  s_ring_wait_spins.store(0, std::memory_order_relaxed);
  s_live_references.store(0, std::memory_order_relaxed);
  s_peak_live_references.store(0, std::memory_order_relaxed);
  s_deferred_unpacks.store(0, std::memory_order_relaxed);
  s_replayed_unpacks.store(0, std::memory_order_relaxed);
}

#if !defined(VITASX2_QEMU_VALIDATION)

constexpr u32 SlotCount = 4;
constexpr u32 SlotSize = 2 * 1024 * 1024;
constexpr u32 PayloadAlignment = 16;

std::mutex s_active_ring_mutex;
InputRing* s_active_ring = nullptr;

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

struct InputRing::Impl {
#if !defined(VITASX2_QEMU_VALIDATION)
  struct Slot {
    VitaGXM::MappedBlock block;
    std::atomic<u32> generation{1};
    std::atomic<u32> references{0};
    u32 write_offset = 0;
  };

  std::array<Slot, SlotCount> slots;
  std::mutex allocation_mutex;
  std::condition_variable slot_released;
  std::atomic<u32> active_capture_calls{0};
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
  for (u32 slot = 0; slot < SlotCount; slot++) {
    char name[32];
    std::snprintf(name, sizeof(name), "VitaSX2 VIF input %u", slot);
    const int result = VitaGXM::AllocateMappedBlock(
        name, SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_NC_RW, SlotSize,
        SCE_GXM_MEMORY_ATTRIB_READ, &m_impl->slots[slot].block);
    if (result < 0) {
      Console.Warning(
          "GPU-VU: VIF input slot %u allocation failed (%08x); "
          "retaining inline MTVU payloads.",
          slot, static_cast<u32>(result));
      for (u32 release = 0; release < slot; release++)
        VitaGXM::ReleaseMappedBlock(&m_impl->slots[release].block);
      return false;
    }
    m_impl->slots[slot].generation.store(1, std::memory_order_relaxed);
    m_impl->slots[slot].references.store(0, std::memory_order_relaxed);
    m_impl->slots[slot].write_offset = 0;
  }

  {
    std::lock_guard lock(s_active_ring_mutex);
    if (s_active_ring) {
      for (auto& slot : m_impl->slots)
        VitaGXM::ReleaseMappedBlock(&slot.block);
      return false;
    }
    m_impl->accepting.store(true, std::memory_order_release);
    s_active_ring = this;
  }
  m_impl->current_slot = 0;
  m_impl->initialized = true;
  Console.WriteLn(
      "GPU-VU: immutable VIF input ring ready (4 x 2 MiB mapped slots).");
  return true;
#endif
}

bool InputRing::Shutdown() {
#if defined(VITASX2_QEMU_VALIDATION)
  return true;
#else
  if (!m_impl || !m_impl->initialized)
    return true;

  {
    std::lock_guard lock(s_active_ring_mutex);
    if (s_active_ring == this)
      s_active_ring = nullptr;
    m_impl->accepting.store(false, std::memory_order_release);
  }
  m_impl->slot_released.notify_all();

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  for (;;) {
    bool busy =
        m_impl->active_capture_calls.load(std::memory_order_acquire) != 0;
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
                          RawVifPayloadRef* payload) {
  if (!source || !payload || size == 0)
    return false;
  *payload = {};
#if defined(VITASX2_QEMU_VALIDATION)
  return false;
#else
  InputRing* ring = nullptr;
  {
    std::lock_guard lock(s_active_ring_mutex);
    ring = s_active_ring;
    if (!ring || !ring->m_impl ||
        !ring->m_impl->accepting.load(std::memory_order_acquire)) {
      return false;
    }
    ring->m_impl->active_capture_calls.fetch_add(
        1, std::memory_order_acq_rel);
  }

  InputRing::Impl& impl = *ring->m_impl;
  const auto leave = [&impl]() {
    impl.active_capture_calls.fetch_sub(1, std::memory_order_acq_rel);
  };
  if (size > SlotSize ||
      !impl.accepting.load(std::memory_order_acquire)) {
    s_capture_fallbacks.fetch_add(1, std::memory_order_relaxed);
    leave();
    return false;
  }

  u32 reserved_slot = 0;
  u32 reserved_offset = 0;
  u32 reserved_generation = 0;
  bool counted_wait = false;
  std::unique_lock allocation_lock(impl.allocation_mutex);
  for (;;) {
    if (!impl.accepting.load(std::memory_order_acquire)) {
      s_capture_fallbacks.fetch_add(1, std::memory_order_relaxed);
      allocation_lock.unlock();
      leave();
      return false;
    }

    InputRing::Impl::Slot& current =
        impl.slots[impl.current_slot];
    u32 aligned_offset = 0;
    if (AlignUp(current.write_offset, PayloadAlignment, &aligned_offset) &&
        aligned_offset <= current.block.size &&
        size <= current.block.size - aligned_offset) {
      reserved_slot = impl.current_slot;
      reserved_offset = aligned_offset;
      reserved_generation =
          current.generation.load(std::memory_order_relaxed);
      current.write_offset = aligned_offset + size;
      current.references.fetch_add(1, std::memory_order_release);
      RecordReferenceCreated();
      break;
    }

    const u32 next = (impl.current_slot + 1) % SlotCount;
    InputRing::Impl::Slot& candidate = impl.slots[next];
    if (candidate.references.load(std::memory_order_acquire) == 0) {
      u32 generation =
          candidate.generation.load(std::memory_order_relaxed) + 1;
      if (generation == 0)
        generation = 1;
      candidate.generation.store(generation, std::memory_order_release);
      candidate.write_offset = 0;
      impl.current_slot = next;
      s_slot_reuses.fetch_add(1, std::memory_order_relaxed);
      continue;
    }

    if (!counted_wait) {
      s_ring_waits.fetch_add(1, std::memory_order_relaxed);
      counted_wait = true;
    }
    impl.slot_released.wait(allocation_lock, [&impl, next]() {
      return !impl.accepting.load(std::memory_order_acquire) ||
             impl.slots[next].references.load(
                 std::memory_order_acquire) == 0;
    });
    // Retain the v=1 telemetry field name for comparable hardware logs. This
    // now counts blocking condition-variable wakeups, never polling spins.
    s_ring_wait_spins.fetch_add(1, std::memory_order_relaxed);
  }
  allocation_lock.unlock();

  InputRing::Impl::Slot& slot = impl.slots[reserved_slot];
  std::memcpy(static_cast<u8*>(slot.block.base) + reserved_offset,
              source, size);
  payload->owner = reinterpret_cast<uptr>(ring);
  payload->slot = reserved_slot;
  payload->generation = reserved_generation;
  payload->offset = reserved_offset;
  payload->size = size;
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
  if (!ring || !ring->m_impl || payload.slot >= SlotCount)
    return nullptr;
  const auto& slot = ring->m_impl->slots[payload.slot];
  if (!slot.block.IsMapped() ||
      slot.generation.load(std::memory_order_acquire) !=
          payload.generation ||
      payload.offset > slot.block.size ||
      payload.size > slot.block.size - payload.offset ||
      slot.references.load(std::memory_order_acquire) == 0) {
    return nullptr;
  }
  return static_cast<const u8*>(slot.block.base) + payload.offset;
#endif
}

bool RetainRawVifPayload(const RawVifPayloadRef& payload) {
#if defined(VITASX2_QEMU_VALIDATION)
  (void)payload;
  return false;
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
  if (ring && ring->m_impl && payload->slot < SlotCount) {
    auto& slot = ring->m_impl->slots[payload->slot];
    if (slot.generation.load(std::memory_order_acquire) ==
        payload->generation) {
      const u32 previous =
          slot.references.fetch_sub(1, std::memory_order_acq_rel);
      if (previous != 0) {
        s_live_references.fetch_sub(1, std::memory_order_relaxed);
        if (previous == 1)
          ring->m_impl->slot_released.notify_one();
      } else {
        slot.references.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }
#endif
  *payload = {};
}

void RecordDeferredVifUnpack() {
  s_deferred_unpacks.fetch_add(1, std::memory_order_relaxed);
}

void RecordReplayedVifUnpack() {
  s_replayed_unpacks.fetch_add(1, std::memory_order_relaxed);
}

InputRingStatistics GetInputRingStatistics() {
  InputRingStatistics stats;
  stats.captures = s_captures.load(std::memory_order_relaxed);
  stats.captured_bytes =
      s_captured_bytes.load(std::memory_order_relaxed);
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
  stats.replayed_unpacks =
      s_replayed_unpacks.load(std::memory_order_relaxed);
  return stats;
}

} // namespace VitaGpuVu
