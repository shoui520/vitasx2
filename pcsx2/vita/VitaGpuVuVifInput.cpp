// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "vita/VitaGpuVuVifInput.h"

#include "vita/VitaGpuVuDraw.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <exception>
#include <limits>
#include <mutex>

#if !defined(VITASX2_QEMU_VALIDATION)
#include "common/Console.h"
#include "common/Threading.h"
#include "vita/VitaGxmMemory.h"
#include "vita/VitaGpuVuHealthJournal.h"
#include "vita/VitaGsMailbox.h"

#include <chrono>
#include <psp2/gxm.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#endif

namespace VitaGpuVu {
namespace {

constexpr u32 Vu1MemoryQwords = 1024;
constexpr u32 DirectInputEpochReservationBytes = Vu1MemoryQwords * 16u;
static_assert(DirectInputEpochReservationBytes == 16 * 1024);

u32 NormalizedCycle(u8 value) {
  return value != 0 ? value : 256u;
}

std::atomic<u64> s_captures{0};
std::atomic<u64> s_captured_bytes{0};
std::atomic<u64> s_derived_captures{0};
std::atomic<u64> s_derived_bytes{0};
std::atomic<u64> s_publication_batches{0};
std::atomic<u64> s_published_bytes{0};
std::atomic<u64> s_fetch_guard_scans{0};
std::atomic<u64> s_fetch_guard_failures{0};
std::atomic<u64> s_logical_high_water_bytes{0};
std::atomic<u64> s_capture_bypasses{0};
std::atomic<u64> s_capture_bypass_bytes{0};
std::atomic<u64> s_capture_fallbacks{0};
std::atomic<u64> s_slot_reuses{0};
std::atomic<u64> s_ring_waits{0};
std::atomic<u64> s_ring_wait_spins{0};
std::atomic<u64> s_live_references{0};
std::atomic<u64> s_peak_live_references{0};
std::atomic<u64> s_retain_generation_rollbacks{0};
std::atomic<u64> s_retain_range_rollbacks{0};
std::atomic<u64> s_retain_overflow_rejections{0};
std::atomic<u64> s_release_generation_mismatches{0};
std::atomic<u64> s_release_underflow_rejections{0};
std::atomic<u64> s_generation_exhaustions{0};
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

void RecordLogicalHighWater(u32 bytes) {
  u64 high_water = s_logical_high_water_bytes.load(
      std::memory_order_relaxed);
  while (high_water < bytes &&
         !s_logical_high_water_bytes.compare_exchange_weak(
             high_water, bytes, std::memory_order_relaxed,
             std::memory_order_relaxed)) {
  }
}

void ResetStatistics() {
  s_captures.store(0, std::memory_order_relaxed);
  s_captured_bytes.store(0, std::memory_order_relaxed);
  s_derived_captures.store(0, std::memory_order_relaxed);
  s_derived_bytes.store(0, std::memory_order_relaxed);
  s_publication_batches.store(0, std::memory_order_relaxed);
  s_published_bytes.store(0, std::memory_order_relaxed);
  s_fetch_guard_scans.store(0, std::memory_order_relaxed);
  s_fetch_guard_failures.store(0, std::memory_order_relaxed);
  s_logical_high_water_bytes.store(0, std::memory_order_relaxed);
  s_capture_bypasses.store(0, std::memory_order_relaxed);
  s_capture_bypass_bytes.store(0, std::memory_order_relaxed);
  s_capture_fallbacks.store(0, std::memory_order_relaxed);
  s_slot_reuses.store(0, std::memory_order_relaxed);
  s_ring_waits.store(0, std::memory_order_relaxed);
  s_ring_wait_spins.store(0, std::memory_order_relaxed);
  s_live_references.store(0, std::memory_order_relaxed);
  s_peak_live_references.store(0, std::memory_order_relaxed);
  s_retain_generation_rollbacks.store(0, std::memory_order_relaxed);
  s_retain_range_rollbacks.store(0, std::memory_order_relaxed);
  s_retain_overflow_rejections.store(0, std::memory_order_relaxed);
  s_release_generation_mismatches.store(0, std::memory_order_relaxed);
  s_release_underflow_rejections.store(0, std::memory_order_relaxed);
  s_generation_exhaustions.store(0, std::memory_order_relaxed);
  s_deferred_unpacks.store(0, std::memory_order_relaxed);
  s_affine_span_merges.store(0, std::memory_order_relaxed);
  s_replayed_unpacks.store(0, std::memory_order_relaxed);
}

#if !defined(VITASX2_QEMU_VALIDATION)

constexpr u32 PayloadAlignment = 16;
constexpr u32 MinimumInputRingSlotCount = 2;
static_assert(MinimumInputRingSlotCount <= InputRingSlotCount);

// Generation and reference count form one ownership identity. Keeping them in
// separate atomics lets a stale release validate generation G, lose a race to
// G becoming unreferenced and recycled as G+1, then decrement an equal-looking
// G+1 reference count. Cortex-A9 supplies the aligned LDREXD/STREXD primitive
// used by the Vita toolchain for this lock-free 64-bit compare/exchange.
static_assert(std::atomic<u64>::is_always_lock_free,
              "Vita input-ring ownership requires lock-free 64-bit atomics");
static_assert(alignof(std::atomic<u64>) >= 8u,
              "Vita input-ring ownership must be eight-byte aligned");

constexpr u64 PackSlotOwnership(u32 generation, u32 references) {
  return (static_cast<u64>(generation) << 32u) | references;
}

constexpr u32 SlotOwnershipGeneration(u64 ownership) {
  return static_cast<u32>(ownership >> 32u);
}

constexpr u32 SlotOwnershipReferences(u64 ownership) {
  return static_cast<u32>(ownership);
}

class ActiveRingCall final {
 public:
  explicit ActiveRingCall(std::atomic<u32>* active) : m_active(active) {
    m_active->fetch_add(1u, std::memory_order_acq_rel);
  }

  ~ActiveRingCall() {
    m_active->fetch_sub(1u, std::memory_order_acq_rel);
  }

  ActiveRingCall(const ActiveRingCall&) = delete;
  ActiveRingCall& operator=(const ActiveRingCall&) = delete;

 private:
  std::atomic<u32>* m_active;
};

// Raw VIF capture is owned by the EE producer while generated sparse tables
// are owned by MTVU. Both reference one bounded mapped arena. CapturePayload()
// serializes its short reservation/copy section, while this process-wide call
// count protects the owner itself: Shutdown() first unpublishes the pointer
// and then waits for every producer which could have loaded it.
std::atomic<InputRing*> s_active_input_ring{nullptr};
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
  binding->outer_byte_stride = 0u;
  binding->payload_byte_extent = static_cast<u32>(last_byte - byte_offset);
  return true;
}

bool BindGridRawQwords(const VifUnpackSpan& span,
                      u16 first_qword_address,
                      s32 outer_invocation_coefficient,
                      u32 outer_invocation_count,
                      s32 child_invocation_coefficient,
                      u32 child_invocation_count,
                      RawQwordBinding* binding) {
  if (!binding || !span.payload.IsValid() ||
      !IsDirectAffineV4_32Span(span) ||
      outer_invocation_coefficient < 0 ||
      child_invocation_coefficient < 0 || outer_invocation_count == 0u ||
      child_invocation_count == 0u ||
      (outer_invocation_coefficient == 0 &&
       child_invocation_coefficient == 0)) {
    return false;
  }

  const u32 first = first_qword_address & (Vu1MemoryQwords - 1u);
  const u32 destination =
      span.destination_qword & (Vu1MemoryQwords - 1u);
  const u32 first_source_vector =
      (first - destination) & (Vu1MemoryQwords - 1u);
  const u64 outer_extent =
      static_cast<u64>(outer_invocation_count - 1u) *
      static_cast<u32>(outer_invocation_coefficient);
  const u64 child_extent =
      static_cast<u64>(child_invocation_count - 1u) *
      static_cast<u32>(child_invocation_coefficient);
  const u64 last_source_vector =
      static_cast<u64>(first_source_vector) + outer_extent + child_extent;
  if (last_source_vector >= span.vector_count)
    return false;

  const u64 byte_offset = static_cast<u64>(first_source_vector) * 16u;
  const u64 outer_byte_stride =
      static_cast<u64>(outer_invocation_coefficient) * 16u;
  const u64 child_byte_stride =
      static_cast<u64>(child_invocation_coefficient) * 16u;
  const u64 byte_extent = (outer_extent + child_extent + 1u) * 16u;
  const u64 invocation_count =
      static_cast<u64>(outer_invocation_count) * child_invocation_count;
  if (byte_offset > std::numeric_limits<u32>::max() ||
      outer_byte_stride > std::numeric_limits<u32>::max() ||
      child_byte_stride > std::numeric_limits<u32>::max() ||
      byte_extent > std::numeric_limits<u32>::max() ||
      invocation_count > std::numeric_limits<u32>::max() ||
      byte_offset + byte_extent > span.source_size ||
      byte_offset + byte_extent > span.payload.size) {
    return false;
  }

  binding->payload = span.payload;
  binding->payload_byte_offset = static_cast<u32>(byte_offset);
  binding->byte_stride = static_cast<u32>(child_byte_stride);
  binding->invocation_count = static_cast<u32>(invocation_count);
  binding->outer_byte_stride = static_cast<u32>(outer_byte_stride);
  binding->payload_byte_extent = static_cast<u32>(byte_extent);
  return true;
}

struct PersistentVifMemoryProvenance::Impl {
  static constexpr u16 NoOwner = 0;
  static constexpr u32 MaximumOwners = 1024;

  struct Owner {
    VifUnpackSpan span;
    u16 owned_qword_count = 0;
    bool active = false;
  };

  std::array<Owner, MaximumOwners> owners{};
  // Zero is unowned; nonzero values are owner indices plus one.
  std::array<u16, 1024> qword_owners{};
  std::array<u16, MaximumOwners> free_owners{};
  u32 free_owner_count = MaximumOwners;
  u32 active_owner_count = 0;
  u32 owned_qword_count = 0;

  Impl() {
    for (u32 index = 0; index < free_owners.size(); index++)
      free_owners[index] = static_cast<u16>(free_owners.size() - index - 1u);
  }

  void InvalidateOwner(u32 owner_index) {
    if (owner_index >= owners.size() || !owners[owner_index].active)
      return;
    Owner& owner = owners[owner_index];
    const u16 token = static_cast<u16>(owner_index + 1u);
    // ApplyDirectAffineSpan() retires a completely superseded owner after its
    // last qword has already moved to the replacement.  Walking the old span
    // again at that point made every full V4-32 replacement pay for its bytes
    // three times on CPU1.  A zero live count is the ownership invariant that
    // no qword token can still name this slot, so release it directly.  Clear()
    // and partial invalidation retain the exhaustive walk below.
    if (owner.owned_qword_count != 0u) {
      for (u32 vector = 0; vector < owner.span.vector_count; vector++) {
        const u32 qword =
            (static_cast<u32>(owner.span.destination_qword) + vector) & 0x3ffu;
        if (qword_owners[qword] != token)
          continue;
        qword_owners[qword] = NoOwner;
        if (owned_qword_count != 0u)
          owned_qword_count--;
      }
    }
    ReleaseRawVifPayload(&owner.span.payload);
    owner = {};
    free_owners[free_owner_count++] = static_cast<u16>(owner_index);
    active_owner_count--;
  }
};

PersistentVifMemoryProvenance::PersistentVifMemoryProvenance()
    : m_impl(std::make_unique<Impl>()) {}

PersistentVifMemoryProvenance::~PersistentVifMemoryProvenance() {
  Clear();
}

std::shared_ptr<PersistentVifMemoryProvenance>
PersistentVifMemoryProvenance::Create() {
  try {
    return std::shared_ptr<PersistentVifMemoryProvenance>(
        new PersistentVifMemoryProvenance());
  } catch (const std::bad_alloc&) {
    return {};
  }
}

void PersistentVifMemoryProvenance::Clear() {
  if (!m_impl)
    return;
  for (u32 index = 0; index < m_impl->owners.size(); index++)
    m_impl->InvalidateOwner(index);
}

bool PersistentVifMemoryProvenance::ApplyDirectAffineSpan(
    const VifUnpackSpan& span) {
  if (!m_impl || !IsDirectAffineV4_32Span(span) || span.vector_count == 0u ||
      !RetainRawVifPayload(span.payload)) {
    return false;
  }

  // Validate every prior token before changing ownership. Product admission
  // may call this only after the generated transaction is accepted, so an
  // internal provenance fault must remain non-mutating and recoverable by the
  // caller's exact materialization fallback.
  for (u32 vector = 0; vector < span.vector_count; vector++) {
    const u32 qword =
        (static_cast<u32>(span.destination_qword) + vector) & 0x3ffu;
    const u16 token = m_impl->qword_owners[qword];
    if (token == Impl::NoOwner)
      continue;
    const u32 owner_index = static_cast<u32>(token - 1u);
    if (owner_index >= m_impl->owners.size() ||
        !m_impl->owners[owner_index].active ||
        m_impl->owners[owner_index].owned_qword_count == 0u) {
      RawVifPayloadRef retained = span.payload;
      ReleaseRawVifPayload(&retained);
      return false;
    }
  }

  if (m_impl->free_owner_count == 0u) {
    RawVifPayloadRef retained = span.payload;
    ReleaseRawVifPayload(&retained);
    return false;
  }
  const u32 owner_index =
      m_impl->free_owners[--m_impl->free_owner_count];
  Impl::Owner& owner = m_impl->owners[owner_index];
  owner.span = span;
  owner.owned_qword_count = 0u;
  owner.active = true;
  m_impl->active_owner_count++;
  const u16 token = static_cast<u16>(owner_index + 1u);
  for (u32 vector = 0; vector < span.vector_count; vector++) {
    const u32 qword =
        (static_cast<u32>(span.destination_qword) + vector) & 0x3ffu;
    const u16 prior_token = m_impl->qword_owners[qword];
    if (prior_token == token)
      continue;
    u32 emptied_prior_owner = Impl::MaximumOwners;
    if (prior_token == Impl::NoOwner) {
      m_impl->owned_qword_count++;
    } else {
      const u32 prior_index = static_cast<u32>(prior_token - 1u);
      Impl::Owner& prior = m_impl->owners[prior_index];
      prior.owned_qword_count--;
      if (prior.owned_qword_count == 0u)
        emptied_prior_owner = prior_index;
    }
    m_impl->qword_owners[qword] = token;
    owner.owned_qword_count++;
    if (emptied_prior_owner != Impl::MaximumOwners)
      m_impl->InvalidateOwner(emptied_prior_owner);
  }
  return true;
}

void PersistentVifMemoryProvenance::InvalidateQword(u16 qword_address) {
  if (!m_impl)
    return;
  const u32 qword = static_cast<u32>(qword_address) & 0x3ffu;
  const u16 token = m_impl->qword_owners[qword];
  if (token == Impl::NoOwner)
    return;
  const u32 owner_index = static_cast<u32>(token - 1u);
  if (owner_index >= m_impl->owners.size() ||
      !m_impl->owners[owner_index].active ||
      m_impl->owners[owner_index].owned_qword_count == 0u) {
    return;
  }
  m_impl->qword_owners[qword] = Impl::NoOwner;
  m_impl->owned_qword_count--;
  Impl::Owner& owner = m_impl->owners[owner_index];
  owner.owned_qword_count--;
  if (owner.owned_qword_count == 0u)
    m_impl->InvalidateOwner(owner_index);
}

bool PersistentVifMemoryProvenance::MaterializeAndInvalidateQword(
    void* vu_memory, u32 vu_memory_size, u16 qword_address) {
  if (!m_impl || !vu_memory || vu_memory_size < 1024u * 16u)
    return false;
  const u32 qword = static_cast<u32>(qword_address) & 0x3ffu;
  if (m_impl->qword_owners[qword] == Impl::NoOwner)
    return true;

  // Resolve before publishing or changing ownership.  A stale ring token is
  // therefore a non-mutating failure rather than a partially committed qword.
  const u8* const source = ResolveQword(static_cast<u16>(qword));
  if (!source)
    return false;
  std::memcpy(static_cast<u8*>(vu_memory) + qword * 16u, source, 16u);
  InvalidateQword(static_cast<u16>(qword));
  return true;
}

bool PersistentVifMemoryProvenance::InvalidateFullyOverwrittenQwords(
    const u32* qword_masks, u32 mask_word_count) {
  if (!m_impl || !qword_masks || mask_word_count > 1024u / 32u)
    return false;

  // Validate every referenced token before changing ownership. A corrupt
  // token must remain a pre-effect internal failure, never a partially
  // invalidated private generation.
  for (u32 mask_word = 0u; mask_word < mask_word_count; mask_word++) {
    u32 pending = qword_masks[mask_word];
    while (pending != 0u) {
      const u32 bit = static_cast<u32>(__builtin_ctz(pending));
      const u32 qword = mask_word * 32u + bit;
      const u16 token = m_impl->qword_owners[qword];
      if (token != Impl::NoOwner) {
        const u32 owner_index = static_cast<u32>(token - 1u);
        if (owner_index >= m_impl->owners.size() ||
            !m_impl->owners[owner_index].active ||
            m_impl->owners[owner_index].owned_qword_count == 0u) {
          return false;
        }
      }
      pending &= pending - 1u;
    }
  }
  for (u32 mask_word = 0u; mask_word < mask_word_count; mask_word++) {
    u32 pending = qword_masks[mask_word];
    while (pending != 0u) {
      const u32 bit = static_cast<u32>(__builtin_ctz(pending));
      InvalidateQword(static_cast<u16>(mask_word * 32u + bit));
      pending &= pending - 1u;
    }
  }
  return true;
}

const u8* PersistentVifMemoryProvenance::ResolveQword(
    u16 qword_address) const {
  if (!m_impl)
    return nullptr;
  const u32 qword = static_cast<u32>(qword_address) & 0x3ffu;
  const u16 token = m_impl->qword_owners[qword];
  if (token == Impl::NoOwner)
    return nullptr;
  const u32 owner_index = static_cast<u32>(token - 1u);
  if (owner_index >= m_impl->owners.size() ||
      !m_impl->owners[owner_index].active) {
    return nullptr;
  }
  const VifUnpackSpan& span = m_impl->owners[owner_index].span;
  const u32 vector =
      (qword - static_cast<u32>(span.destination_qword)) & 0x3ffu;
  const u32 byte_offset = vector * 16u;
  const u8* const payload = ResolveRawVifPayload(span.payload);
  if (vector >= span.vector_count || !payload ||
      byte_offset > span.payload.size ||
      16u > span.payload.size - byte_offset) {
    return nullptr;
  }
  return payload + byte_offset;
}

bool PersistentVifMemoryProvenance::MaterializeOwnedQwords(
    void* vu_memory, u32 vu_memory_size) const {
  if (!m_impl || !vu_memory || vu_memory_size < 1024u * 16u)
    return false;
  u8* const destination = static_cast<u8*>(vu_memory);
  for (u32 qword = 0u; qword < m_impl->qword_owners.size(); qword++) {
    if (m_impl->qword_owners[qword] == Impl::NoOwner)
      continue;
    const u8* const source = ResolveQword(static_cast<u16>(qword));
    if (!source)
      return false;
    std::memcpy(destination + qword * 16u, source, 16u);
  }
  return true;
}

bool PersistentVifMemoryProvenance::BindGridRawQwords(
    u16 first_qword_address, s32 outer_invocation_coefficient,
    u32 outer_invocation_count, s32 child_invocation_coefficient,
    u32 child_invocation_count, RawVifPayloadRef* payload,
    RawQwordBinding* binding) const {
  if (!m_impl || !payload || !binding || outer_invocation_count == 0u ||
      child_invocation_count == 0u) {
    return false;
  }
  const u16 token =
      m_impl->qword_owners[static_cast<u32>(first_qword_address) & 0x3ffu];
  if (token == Impl::NoOwner)
    return false;
  const u32 owner_index = static_cast<u32>(token - 1u);
  if (owner_index >= m_impl->owners.size() ||
      !m_impl->owners[owner_index].active) {
    return false;
  }
  const VifUnpackSpan& span = m_impl->owners[owner_index].span;
  if (m_impl->owners[owner_index].owned_qword_count != span.vector_count) {
    for (u32 outer = 0u; outer < outer_invocation_count; outer++) {
      for (u32 child = 0u; child < child_invocation_count; child++) {
        const s64 address = static_cast<s64>(first_qword_address) +
            static_cast<s64>(outer_invocation_coefficient) * outer +
            static_cast<s64>(child_invocation_coefficient) * child;
        const u32 qword = static_cast<u32>(address) & 0x3ffu;
        if (m_impl->qword_owners[qword] != token)
          return false;
      }
    }
  }
  RawQwordBinding resolved;
  if (!VitaGpuVu::BindGridRawQwords(
          span, first_qword_address, outer_invocation_coefficient,
          outer_invocation_count, child_invocation_coefficient,
          child_invocation_count, &resolved)) {
    return false;
  }
  *payload = span.payload;
  *binding = resolved;
  return true;
}

u32 PersistentVifMemoryProvenance::ActiveSpanCount() const {
  return m_impl ? m_impl->active_owner_count : 0u;
}

u32 PersistentVifMemoryProvenance::OwnedQwordCount() const {
  return m_impl ? m_impl->owned_qword_count : 0u;
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

bool HasSingleAddressableRawInputWindow(
    const RawVifPayloadRef* payloads, size_t payload_count,
    const StreamBinding* bindings, size_t binding_count) {
  GeneratedInputWindow window;
  return ResolveGeneratedInputWindow(
      payloads, payload_count, bindings, binding_count,
      GeneratedRawInputBufferQwords, Vu1MemoryQwords,
      GeneratedRawInputDeclaredQwords,
      GeneratedRawInputMaximumRelativeQword, &window);
}

bool HasSingleAddressableRawInputWindow(const GpuVuDraw& draw) {
  if (draw.invocation_count == 0)
    return false;
  return HasSingleAddressableRawInputWindow(
      draw.InputPayloads().data(), draw.InputPayloads().size(),
      draw.streams.data(), draw.streams.size());
}

struct InputRing::Impl {
#if !defined(VITASX2_QEMU_VALIDATION)
  struct Slot {
    VitaGXM::MappedBlock block;
    alignas(8) std::atomic<u64> ownership{PackSlotOwnership(1u, 0u)};
    std::atomic<u32> committed_offset{0};
    std::atomic<u32> published_offset{0};
    // Zero means this generation has not yet crossed the final physical
    // guard gate.  Publication scans each 4 KiB suffix once per generation;
    // repeated per-draw publications remain an O(1) atomic comparison.
    std::atomic<u32> fetch_guard_generation{0};
    u32 write_offset = 0;
  };

  std::array<Slot, InputRingSlotCount> slots;
  // CapturePayload() serializes the two producers. Publish the exact slot the
  // active producer is waiting to reuse before rechecking its reference
  // count. The last owner claims that token before signaling, which makes
  // release-before-wait and shutdown races lossless without putting
  // VitaSDK's cancellation-polling pthread condition variable on this path.
  SceUID slot_released_sema = -1;
  std::atomic<s32> waiting_slot{-1};
  std::atomic<bool> accepting{false};
  // A last release publishes references==0 before it finishes the semaphore
  // handoff. Shutdown samples references first and this counter second so it
  // cannot delete that semaphore or unmap the owner during the interval.
  std::atomic<u32> active_reference_calls{0};
  // The two producers never hold this lock beyond one mapped copy or a real
  // ring-pressure wait. GPU retirement does not take the lock, so a blocked
  // producer cannot prevent the last generation reference from waking it.
  Threading::KernelMutex capture_mutex;
  u32 current_slot = 0;
  u32 active_slot_count = 0;
  bool initialized = false;
#endif
};

InputRing::InputRing() : m_impl(new Impl()) {}

InputRing::~InputRing() {
  if (!Shutdown()) {
#if !defined(VITASX2_QEMU_VALIDATION)
    // RawVifPayloadRef names this InputRing object, not only its Impl. Leaking
    // the Impl while allowing the enclosing GS device to destroy this owner
    // would therefore remain a use-after-free. Refuse to return from teardown:
    // preserve one bounded durable snapshot, then let the kernel revoke this
    // title's mappings as a unit. std::terminate is the last-resort title-local
    // fail-stop if sceKernelExitProcess unexpectedly returns an error.
    Console.Error(
        "GPU-VU FATAL: input-ring destruction refused with live ownership; "
        "exiting title without releasing indeterminate mappings.");
    (void)HealthJournal::FlushForFatal();
    (void)sceKernelExitProcess(1);
#endif
    std::terminate();
  }
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
  for (u32 slot = 0; slot < InputRingSlotCount; slot++) {
    const u64 ownership =
        m_impl->slots[slot].ownership.load(std::memory_order_acquire);
    if (SlotOwnershipReferences(ownership) != 0u)
      return false;
    if (SlotOwnershipGeneration(ownership) ==
        std::numeric_limits<u32>::max()) {
      s_generation_exhaustions.fetch_add(1, std::memory_order_relaxed);
      Console.Error(
          "GPU-VU: VIF input generation exhausted before ring initialize "
          "(slot=%u); mapped input ownership remains disabled.",
          slot);
      return false;
    }
  }
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
  m_impl->active_reference_calls.store(0, std::memory_order_relaxed);
  for (u32 slot = 0; slot < InputRingSlotCount; slot++) {
    char name[32];
    std::snprintf(name, sizeof(name), "VitaSX2 VU input %u", slot);
    // libGXM's memory contract makes USER_RW coherent between CPU caches and
    // the GPU: an SGX cache miss snoops the Cortex-A9 caches. Capture directly
    // into this mapped storage so each immutable VIF byte is copied once.
    // Independent generations and the existing retirement notification
    // still prevent the producer from overwriting bytes the GPU can observe.
    const int result = VitaGXM::AllocateMappedBlock(
        name, SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, InputRingSlotMappedSize,
        SCE_GXM_MEMORY_ATTRIB_READ, &m_impl->slots[slot].block);
    if (result < 0) {
      if (slot < MinimumInputRingSlotCount) {
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
      Console.Warning(
          "GPU-VU: VIF input slot %u allocation failed (%08x); "
          "continuing with %u mapped slots.",
          slot, static_cast<u32>(result), slot);
      break;
    }
    if (m_impl->slots[slot].block.size < InputRingSlotMappedSize) {
      const u32 mapped_size = m_impl->slots[slot].block.size;
      VitaGXM::ReleaseMappedBlock(&m_impl->slots[slot].block);
      if (slot < MinimumInputRingSlotCount) {
        Console.Warning(
            "GPU-VU: VIF input slot %u has a short mapped allocation "
            "(%u < %u); retaining inline MTVU payloads.",
            slot, mapped_size, InputRingSlotMappedSize);
        for (u32 release = 0; release < slot; release++)
          VitaGXM::ReleaseMappedBlock(&m_impl->slots[release].block);
        sceKernelDeleteSema(m_impl->slot_released_sema);
        m_impl->slot_released_sema = -1;
        return false;
      }
      Console.Warning(
          "GPU-VU: VIF input slot %u has a short mapped allocation "
          "(%u < %u); continuing with %u mapped slots.",
          slot, mapped_size, InputRingSlotMappedSize, slot);
      break;
    }
    // PVR's Series5 driver overallocates vertex buffers because a PDS fetch at
    // the end can pull an extra cache line.  The two independent GPU dumps at
    // 0x8f900000 identified the same condition at the exact end of slot 3.
    // Keep the complete page mapped and deterministic while all logical range
    // checks continue to stop at the preceding 2 MiB boundary.
    std::fill_n(reinterpret_cast<u32*>(
                    static_cast<u8*>(m_impl->slots[slot].block.base) +
                    InputRingSlotSize),
                InputRingSlotFetchGuardSize / sizeof(u32),
                InputRingSlotFetchGuardWord);
    Console.WriteLn(
        "GPU-VU input_slot=%u base=%p logical_end=%p mapped_end=%p "
        "fetch_guard_bytes=%u.",
        slot, m_impl->slots[slot].block.base,
        static_cast<u8*>(m_impl->slots[slot].block.base) +
            InputRingSlotSize,
        static_cast<u8*>(m_impl->slots[slot].block.base) +
            InputRingSlotMappedSize,
        InputRingSlotFetchGuardSize);
    const u32 generation = SlotOwnershipGeneration(
        m_impl->slots[slot].ownership.load(std::memory_order_relaxed)) + 1u;
    m_impl->slots[slot].ownership.store(
        PackSlotOwnership(generation, 0u), std::memory_order_relaxed);
    m_impl->slots[slot].committed_offset.store(
        0, std::memory_order_relaxed);
    m_impl->slots[slot].published_offset.store(
        0, std::memory_order_relaxed);
    m_impl->slots[slot].fetch_guard_generation.store(
        0, std::memory_order_relaxed);
    m_impl->slots[slot].write_offset = 0;
    m_impl->active_slot_count = slot + 1u;
  }

  if (m_impl->active_slot_count < MinimumInputRingSlotCount) {
    for (u32 slot = 0; slot < m_impl->active_slot_count; slot++)
      VitaGXM::ReleaseMappedBlock(&m_impl->slots[slot].block);
    sceKernelDeleteSema(m_impl->slot_released_sema);
    m_impl->slot_released_sema = -1;
    m_impl->active_slot_count = 0;
    return false;
  }
  m_impl->current_slot = 0;
  m_impl->accepting.store(true, std::memory_order_release);
  InputRing* expected = nullptr;
  if (!s_active_input_ring.compare_exchange_strong(
          expected, this, std::memory_order_release,
          std::memory_order_relaxed)) {
    m_impl->accepting.store(false, std::memory_order_relaxed);
    for (u32 slot = 0; slot < m_impl->active_slot_count; slot++)
      VitaGXM::ReleaseMappedBlock(&m_impl->slots[slot].block);
    sceKernelDeleteSema(m_impl->slot_released_sema);
    m_impl->slot_released_sema = -1;
    m_impl->active_slot_count = 0;
    return false;
  }
  m_impl->initialized = true;
  Console.WriteLn(
      "GPU-VU: immutable raw-VIF/sparse-generated input ring ready "
      "(%u x 2 MiB logical slots with 4 KiB mapped fetch guards, "
      "two serialized producers).",
      m_impl->active_slot_count);
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
  s_active_input_ring.compare_exchange_strong(
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
    bool busy = false;
    for (u32 slot = 0; slot < m_impl->active_slot_count; slot++) {
      busy |= SlotOwnershipReferences(
                  m_impl->slots[slot].ownership.load(
                      std::memory_order_acquire)) != 0u;
    }
    busy |= m_impl->active_reference_calls.load(
                std::memory_order_acquire) != 0;
    busy |= s_active_capture_calls.load(std::memory_order_acquire) != 0;
    if (!busy)
      break;
    if (std::chrono::steady_clock::now() >= deadline) {
      Console.Error(
          "GPU-VU: refusing to unmap a VIF input slot with live owners.");
      return false;
    }
    // Vita's fixed-priority scheduler does not require yield to run a lower-
    // priority retirement owner. Sleep so teardown can actually drain it.
    Threading::Sleep(1);
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
  for (u32 slot = 0; slot < m_impl->active_slot_count; slot++) {
    InputRing::Impl::Slot& active_slot = m_impl->slots[slot];
    RecordLogicalHighWater(active_slot.write_offset);
    if (active_slot.block.IsAllocated() &&
        VitaGXM::ReleaseMappedBlock(&active_slot.block) < 0) {
      released = false;
    }
    active_slot.write_offset = 0;
  }
  if (!released)
    return false;
  m_impl->current_slot = 0;
  m_impl->active_slot_count = 0;
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

bool InputRing::CapturePayload(const void* source, u32 size,
                               RawVifCaptureMode mode,
                               RawVifPayloadRef* payload) {
#if defined(VITASX2_QEMU_VALIDATION)
  (void)source;
  (void)size;
  (void)mode;
  (void)payload;
  return false;
#else
  InputRing* const ring = this;
  InputRing::Impl& impl = *m_impl;
  std::unique_lock<Threading::KernelMutex> capture_lock(impl.capture_mutex);
  if (size > InputRingSlotSize ||
      impl.active_slot_count < MinimumInputRingSlotCount ||
      !impl.accepting.load(std::memory_order_acquire)) {
    s_capture_fallbacks.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  u32 reserved_slot = 0;
  u32 reserved_offset = 0;
  u32 reserved_generation = 0;
  bool counted_wait = false;
  for (;;) {
    if (!impl.accepting.load(std::memory_order_acquire)) {
      s_capture_fallbacks.fetch_add(1, std::memory_order_relaxed);
      return false;
    }

    InputRing::Impl::Slot& current =
        impl.slots[impl.current_slot];
    u32 aligned_offset = 0;
    // The generated phase-one root binds one GXM raw-buffer base for every
    // VU command epoch. VU1 data memory is 16 KiB, so require that much free
    // logical capacity before the first captured UNPACK. This biases the
    // common affine epoch toward one 2 MiB slot; it is not a reservation
    // against the serialized CPU1 derived-payload producer. An epoch split by
    // such an interleave is rejected by HasSingleAddressableRawInputWindow()
    // before CpuVU1 can be bypassed.
    const u32 required_capacity =
        mode == RawVifCaptureMode::BeginVuCommandEpoch
            ? std::max(size, DirectInputEpochReservationBytes)
            : size;
    if (AlignUp(current.write_offset, PayloadAlignment, &aligned_offset) &&
        IsInputRingLogicalRange(aligned_offset, required_capacity)) {
      reserved_slot = impl.current_slot;
      reserved_offset = aligned_offset;
      u64 ownership = current.ownership.load(std::memory_order_acquire);
      bool began_new_generation = false;
      for (;;) {
        const u32 generation = SlotOwnershipGeneration(ownership);
        const u32 references = SlotOwnershipReferences(ownership);
        if (references == std::numeric_limits<u32>::max())
          break;
        if (references == 0u &&
            generation == std::numeric_limits<u32>::max()) {
          impl.accepting.store(false, std::memory_order_release);
          s_generation_exhaustions.fetch_add(1, std::memory_order_relaxed);
          s_capture_fallbacks.fetch_add(1, std::memory_order_relaxed);
          Console.Error(
              "GPU-VU: VIF input generation exhausted at current slot %u; "
              "mapped input ownership disabled before identity reuse.",
              impl.current_slot);
          return false;
        }
        reserved_generation =
            references == 0u ? generation + 1u : generation;
        const u64 desired =
            PackSlotOwnership(reserved_generation, references + 1u);
        if (current.ownership.compare_exchange_weak(
                ownership, desired, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
          began_new_generation = references == 0u;
          break;
        }
      }
      if (SlotOwnershipReferences(ownership) ==
          std::numeric_limits<u32>::max()) {
        s_retain_overflow_rejections.fetch_add(1, std::memory_order_relaxed);
        s_capture_fallbacks.fetch_add(1, std::memory_order_relaxed);
        return false;
      }
      if (began_new_generation) {
        // A zero-reference slot has no surviving payload owner. Start a fresh
        // non-wrapping identity before another capture can publish ownership;
        // retaining the append offset avoids rewriting initialized bytes, but
        // every generation-local publication proof must start empty.
        current.committed_offset.store(0, std::memory_order_relaxed);
        current.published_offset.store(0, std::memory_order_relaxed);
        current.fetch_guard_generation.store(0, std::memory_order_relaxed);
      }
      current.write_offset = aligned_offset + size;
      RecordReferenceCreated();
      break;
    }

    RecordLogicalHighWater(current.write_offset);

    const u32 next =
        (impl.current_slot + 1u) % impl.active_slot_count;
    InputRing::Impl::Slot& candidate = impl.slots[next];
    u64 candidate_ownership =
        candidate.ownership.load(std::memory_order_acquire);
    if (SlotOwnershipReferences(candidate_ownership) == 0u) {
      const bool guard_is_mapped = candidate.block.IsMapped() &&
          candidate.block.size >= InputRingSlotMappedSize;
      const u32* const guard = guard_is_mapped
          ? reinterpret_cast<const u32*>(
                static_cast<const u8*>(candidate.block.base) +
                InputRingSlotSize)
          : nullptr;
      u32 damaged_word = 0u;
      s_fetch_guard_scans.fetch_add(1, std::memory_order_relaxed);
      if (!guard_is_mapped || !IsInputRingFetchGuardIntact(
              guard, InputRingSlotFetchGuardSize / sizeof(u32),
              &damaged_word)) {
        const u32 damaged_byte = damaged_word * sizeof(u32);
        impl.accepting.store(false, std::memory_order_release);
        s_fetch_guard_failures.fetch_add(1, std::memory_order_relaxed);
        s_capture_fallbacks.fetch_add(1, std::memory_order_relaxed);
        Console.Error(
            "GPU-VU: VIF input fetch guard FAILED before slot reuse "
            "(slot=%u generation=%u byte=%u value=%08x expected=%08x); "
            "mapped input ownership disabled before another GPU submission.",
            next, SlotOwnershipGeneration(candidate_ownership),
            damaged_byte, guard_is_mapped ? guard[damaged_word] : 0u,
            InputRingSlotFetchGuardWord);
        return false;
      }
      const u32 previous_generation =
          SlotOwnershipGeneration(candidate_ownership);
      if (previous_generation == std::numeric_limits<u32>::max()) {
        // Generation is an ownership identity, not a wrapping sequence.  A
        // wrap to one could make an ancient stale payload name live storage.
        impl.accepting.store(false, std::memory_order_release);
        s_generation_exhaustions.fetch_add(1, std::memory_order_relaxed);
        s_capture_fallbacks.fetch_add(1, std::memory_order_relaxed);
        Console.Error(
            "GPU-VU: VIF input generation exhausted at slot %u; mapped input "
            "ownership disabled before identity reuse.",
            next);
        return false;
      }
      const u32 generation = previous_generation + 1u;
      const u64 recycled_ownership = PackSlotOwnership(generation, 0u);
      if (!candidate.ownership.compare_exchange_strong(
              candidate_ownership, recycled_ownership,
              std::memory_order_acq_rel, std::memory_order_acquire)) {
        // Retain cannot join a zero-reference generation, but keep the
        // ownership transition self-proving if a future caller changes that
        // contract. No generation-local metadata has been reset yet.
        continue;
      }
      candidate.write_offset = 0;
      candidate.committed_offset.store(0, std::memory_order_relaxed);
      candidate.published_offset.store(0, std::memory_order_relaxed);
      candidate.fetch_guard_generation.store(0, std::memory_order_relaxed);
      impl.current_slot = next;
      s_slot_reuses.fetch_add(1, std::memory_order_relaxed);
      continue;
    }

    if (!counted_wait) {
      const u64 wait_count =
          s_ring_waits.fetch_add(1, std::memory_order_relaxed) + 1u;
      counted_wait = true;
      if (wait_count <= 8u || (wait_count & (wait_count - 1u)) == 0u) {
        candidate_ownership =
            candidate.ownership.load(std::memory_order_acquire);
        Console.WriteLn(
            "GPU-VU input_ring_wait=begin wait_count=%llu slot=%u "
            "generation=%u references=%u current_slot=%u current_offset=%u.",
            static_cast<unsigned long long>(wait_count), next,
            SlotOwnershipGeneration(candidate_ownership),
            SlotOwnershipReferences(candidate_ownership),
            impl.current_slot, current.write_offset);
      }
    }
    RawVifPayloadRef blocked_generation;
    blocked_generation.owner = reinterpret_cast<uptr>(ring);
    blocked_generation.slot = next;
    candidate_ownership = candidate.ownership.load(std::memory_order_acquire);
    blocked_generation.generation =
        SlotOwnershipGeneration(candidate_ownership);
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
      return false;
    }

    // Publishing the waiter before this recheck closes both release-before-
    // wait and shutdown-before-wait races. If the producer clears its own
    // token there is nothing to consume. If another thread already claimed
    // it, that thread has posted exactly one semaphore signal.
    const bool should_wait =
        impl.accepting.load(std::memory_order_acquire) &&
        SlotOwnershipReferences(candidate.ownership.load(
            std::memory_order_acquire)) != 0u;
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
          SlotOwnershipReferences(candidate.ownership.load(
              std::memory_order_acquire)),
          waiter_after_wait);
      s_capture_fallbacks.fetch_add(1, std::memory_order_relaxed);
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
  // Reservation and copy are serialized across CPU0/CPU1. Publishing the
  // contiguous prefix after its bytes are initialized lets the GS owner bind
  // the mapping directly without a second transfer-arena copy.
  slot.committed_offset.store(
      reserved_offset + size, std::memory_order_release);
  s_captures.fetch_add(1, std::memory_order_relaxed);
  s_captured_bytes.fetch_add(size, std::memory_order_relaxed);
  return true;
#endif
}

bool CaptureRawVifPayload(const void* source, u32 size,
                          RawVifCaptureMode mode,
                          RawVifPayloadRef* payload) {
  if (!source || !payload || size == 0u)
    return false;
  *payload = {};
#if defined(VITASX2_QEMU_VALIDATION)
  (void)mode;
  return false;
#else
  ActiveRingCall active_call(&s_active_capture_calls);
  InputRing* const ring =
      s_active_input_ring.load(std::memory_order_acquire);
  if (!ring || !ring->m_impl ||
      !ring->m_impl->accepting.load(std::memory_order_acquire)) {
    return false;
  }
  return ring->CapturePayload(source, size, mode, payload);
#endif
}

bool CaptureDerivedGpuVuPayload(const void* source, u32 size,
                                RawVifPayloadRef* payload) {
  if (!source || !payload || size == 0u)
    return false;
  *payload = {};
#if defined(VITASX2_QEMU_VALIDATION)
  return false;
#else
  ActiveRingCall active_call(&s_active_capture_calls);
  InputRing* const ring =
      s_active_input_ring.load(std::memory_order_acquire);
  if (!ring || !ring->m_impl ||
      !ring->m_impl->accepting.load(std::memory_order_acquire)) {
    return false;
  }
  if (!ring->CapturePayload(source, size,
                            RawVifCaptureMode::ContinueEpoch, payload)) {
    return false;
  }
  s_derived_captures.fetch_add(1, std::memory_order_relaxed);
  s_derived_bytes.fetch_add(size, std::memory_order_relaxed);
  return true;
#endif
}

bool DerivedGpuVuPayloadSharesRawInputArena() {
#if defined(VITASX2_QEMU_VALIDATION)
  return false;
#else
  ActiveRingCall active_call(&s_active_capture_calls);
  InputRing* const ring =
      s_active_input_ring.load(std::memory_order_acquire);
  return ring && ring->IsReady();
#endif
}

const char* RawVifPayloadResolveFailureName(
    RawVifPayloadResolveFailure failure) {
  switch (failure) {
    case RawVifPayloadResolveFailure::None:
      return "none";
    case RawVifPayloadResolveFailure::InvalidReference:
      return "invalid-reference";
    case RawVifPayloadResolveFailure::InvalidOwner:
      return "invalid-owner";
    case RawVifPayloadResolveFailure::InvalidSlot:
      return "invalid-slot";
    case RawVifPayloadResolveFailure::UnmappedSlot:
      return "unmapped-slot";
    case RawVifPayloadResolveFailure::GenerationMismatch:
      return "generation-mismatch";
    case RawVifPayloadResolveFailure::LogicalRange:
      return "logical-range";
    case RawVifPayloadResolveFailure::InsufficientCommittedPrefix:
      return "insufficient-committed-prefix";
    case RawVifPayloadResolveFailure::InsufficientPublishedPrefix:
      return "insufficient-published-prefix";
    case RawVifPayloadResolveFailure::NoReferences:
      return "no-references";
    case RawVifPayloadResolveFailure::ValidationOwnerUnavailable:
      return "validation-owner-unavailable";
  }
  return "unknown";
}

const u8* ResolveRawVifPayloadInternal(
    const RawVifPayloadRef& payload, bool require_published,
    RawVifPayloadResolveFailure* failure,
    RawVifPayloadResolveDiagnostics* diagnostics) {
  RawVifPayloadResolveDiagnostics observed;
  observed.owner = payload.owner;
  observed.slot = payload.slot;
  observed.wanted_generation = payload.generation;
  observed.offset = payload.offset;
  observed.size = payload.size;
  const auto reject = [&](RawVifPayloadResolveFailure reason) -> const u8* {
    if (failure)
      *failure = reason;
    if (diagnostics)
      *diagnostics = observed;
    return nullptr;
  };
  if (!payload.IsValid())
    return reject(RawVifPayloadResolveFailure::InvalidReference);
#if defined(VITASX2_QEMU_VALIDATION)
  return reject(RawVifPayloadResolveFailure::ValidationOwnerUnavailable);
#else
  auto* ring = reinterpret_cast<InputRing*>(payload.owner);
  if (!ring || ring != s_active_input_ring.load(std::memory_order_acquire) ||
      !ring->m_impl) {
    return reject(RawVifPayloadResolveFailure::InvalidOwner);
  }
  if (payload.slot >= InputRingSlotCount)
    return reject(RawVifPayloadResolveFailure::InvalidSlot);
  const auto& slot = ring->m_impl->slots[payload.slot];
  const u64 ownership = slot.ownership.load(std::memory_order_acquire);
  observed.actual_generation = SlotOwnershipGeneration(ownership);
  observed.committed_prefix =
      slot.committed_offset.load(std::memory_order_acquire);
  observed.published_prefix =
      slot.published_offset.load(std::memory_order_acquire);
  observed.references = SlotOwnershipReferences(ownership);
  if (!slot.block.IsMapped())
    return reject(RawVifPayloadResolveFailure::UnmappedSlot);
  if (observed.actual_generation != payload.generation)
    return reject(RawVifPayloadResolveFailure::GenerationMismatch);
  if (!IsInputRingLogicalRange(payload.offset, payload.size))
    return reject(RawVifPayloadResolveFailure::LogicalRange);
  const u32 required_prefix = payload.offset + payload.size;
  if (observed.committed_prefix < required_prefix) {
    return reject(
        RawVifPayloadResolveFailure::InsufficientCommittedPrefix);
  }
  if (require_published && observed.published_prefix < required_prefix) {
    return reject(
        RawVifPayloadResolveFailure::InsufficientPublishedPrefix);
  }
  if (observed.references == 0u)
    return reject(RawVifPayloadResolveFailure::NoReferences);
  // A correctly owned payload pins this generation. Confirm the ownership
  // pair once more after reading its generation-local prefix fields so an
  // invalid unretained caller cannot receive storage across a recycle.
  const u64 confirmed_ownership =
      slot.ownership.load(std::memory_order_acquire);
  const u32 confirmed_generation =
      SlotOwnershipGeneration(confirmed_ownership);
  const u32 confirmed_references =
      SlotOwnershipReferences(confirmed_ownership);
  if (confirmed_generation != payload.generation) {
    observed.actual_generation = confirmed_generation;
    observed.references = confirmed_references;
    return reject(RawVifPayloadResolveFailure::GenerationMismatch);
  }
  if (confirmed_references == 0u) {
    observed.references = 0u;
    return reject(RawVifPayloadResolveFailure::NoReferences);
  }
  observed.actual_generation = confirmed_generation;
  observed.references = confirmed_references;
  if (failure)
    *failure = RawVifPayloadResolveFailure::None;
  if (diagnostics)
    *diagnostics = observed;
  return static_cast<const u8*>(slot.block.base) + payload.offset;
#endif
}

const u8* ResolveRawVifPayload(
    const RawVifPayloadRef& payload,
    RawVifPayloadResolveFailure* failure,
    RawVifPayloadResolveDiagnostics* diagnostics) {
  return ResolveRawVifPayloadInternal(
      payload, false, failure, diagnostics);
}

const u8* ResolveGpuRawVifPayload(
    const RawVifPayloadRef& payload,
    RawVifPayloadResolveFailure* failure,
    RawVifPayloadResolveDiagnostics* diagnostics) {
  return ResolveRawVifPayloadInternal(
      payload, true, failure, diagnostics);
}

bool InputRing::PublishRawVifPayloads(const RawVifPayloadRef* payloads,
                                     u32 payload_count) {
#if !defined(VITASX2_QEMU_VALIDATION)
  if (!m_impl || !m_impl->accepting.load(std::memory_order_acquire) ||
      (payload_count != 0 && !payloads)) {
    Console.Error(
        "GPU-VU: input publication rejected (ring unavailable, payloads=%u).",
        payload_count);
    return false;
  }

  std::array<RawVifPayloadRef, InputRingSlotCount> generations{};
  u32 generation_count = 0;
  for (u32 payload_index = 0; payload_index < payload_count;
       payload_index++) {
    const RawVifPayloadRef& payload = payloads[payload_index];
    if (!payload.IsValid() ||
        payload.owner != reinterpret_cast<uptr>(this) ||
        payload.slot >= InputRingSlotCount ||
        !IsInputRingLogicalRange(payload.offset, payload.size)) {
      Console.Error(
          "GPU-VU: input publication rejected "
          "(bad payload %u: owner=%08x expected=%08x slot=%u "
          "generation=%u size=%u).",
          payload_index, static_cast<u32>(payload.owner),
          static_cast<u32>(reinterpret_cast<uptr>(this)), payload.slot,
          payload.generation, payload.size);
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
            "(more than %u live slot generations at payload %u, "
            "slot=%u generation=%u).",
            InputRingSlotCount, payload_index, payload.slot,
            payload.generation);
        return false;
      }
      generations[generation_count++] = payload;
    }
  }

  u64 published_bytes = 0;
  for (u32 index = 0; index < generation_count; index++) {
    const RawVifPayloadRef& payload = generations[index];
    auto& slot = m_impl->slots[payload.slot];
    const auto generation_is_live = [&]() {
      const u64 ownership = slot.ownership.load(std::memory_order_acquire);
      return SlotOwnershipGeneration(ownership) == payload.generation &&
          SlotOwnershipReferences(ownership) != 0u;
    };
    // The pending draw owns a reference to this exact generation. Therefore
    // the producer cannot enter the references==0 reuse transition while its
    // contiguous committed prefix is copied.
    if (!generation_is_live()) {
      const u64 observed_ownership =
          slot.ownership.load(std::memory_order_relaxed);
      Console.Error(
          "GPU-VU: input publication rejected "
          "(retired generation slot=%u wanted=%u actual=%u refs=%u).",
          payload.slot, payload.generation,
          SlotOwnershipGeneration(observed_ownership),
          SlotOwnershipReferences(observed_ownership));
      return false;
    }
    if (slot.fetch_guard_generation.load(std::memory_order_acquire) !=
        payload.generation) {
      const bool guard_is_mapped = slot.block.IsMapped() &&
          slot.block.size >= InputRingSlotMappedSize;
      const u32* const guard = guard_is_mapped
          ? reinterpret_cast<const u32*>(
                static_cast<const u8*>(slot.block.base) +
                InputRingSlotSize)
          : nullptr;
      u32 damaged_word = 0u;
      s_fetch_guard_scans.fetch_add(1, std::memory_order_relaxed);
      if (!guard_is_mapped || !IsInputRingFetchGuardIntact(
              guard, InputRingSlotFetchGuardSize / sizeof(u32),
              &damaged_word)) {
        m_impl->accepting.store(false, std::memory_order_release);
        s_fetch_guard_failures.fetch_add(1, std::memory_order_relaxed);
        const u32 damaged_value =
            guard_is_mapped ? guard[damaged_word] : 0u;
        Console.Error(
            "GPU-VU: VIF input fetch guard FAILED before GPU publication "
            "(slot=%u generation=%u byte=%u value=%08x expected=%08x "
            "mapped=%u mapped_size=%u); generated admission disabled "
            "before effects.",
            payload.slot, payload.generation,
            damaged_word * sizeof(u32), damaged_value,
            InputRingSlotFetchGuardWord,
            slot.block.IsMapped() ? 1u : 0u, slot.block.size);
        return false;
      }
      // Guard validation is diagnostic state for this exact generation. Do
      // not publish it if a malformed caller let the owner recycle while the
      // guard was being inspected.
      if (!generation_is_live()) {
        const u64 observed_ownership =
            slot.ownership.load(std::memory_order_relaxed);
        Console.Error(
            "GPU-VU: input publication rejected after guard validation "
            "(retired generation slot=%u wanted=%u actual=%u refs=%u).",
            payload.slot, payload.generation,
            SlotOwnershipGeneration(observed_ownership),
            SlotOwnershipReferences(observed_ownership));
        return false;
      }
      slot.fetch_guard_generation.store(
          payload.generation, std::memory_order_release);
    }
    const u32 committed =
        slot.committed_offset.load(std::memory_order_acquire);
    const u32 published =
        slot.published_offset.load(std::memory_order_relaxed);
    if (committed <= published)
      continue;
    if (!slot.block.IsMapped() || committed > InputRingSlotSize) {
      Console.Error(
          "GPU-VU: input publication rejected "
          "(bad logical range slot=%u mapped=%u committed=%u "
          "logical_size=%u mapped_size=%u).",
          payload.slot,
          slot.block.IsMapped() ? 1u : 0u, committed, InputRingSlotSize,
          slot.block.size);
      return false;
    }

    // Capture wrote the cacheable GXM mapping itself. The producer's release
    // publication makes those initialized bytes visible to this thread, and
    // libGXM's documented CPU/GPU coherence makes them visible to SGX without
    // a second copy or a cache-maintenance operation.
    if (!generation_is_live()) {
      const u64 observed_ownership =
          slot.ownership.load(std::memory_order_relaxed);
      Console.Error(
          "GPU-VU: input publication rejected before prefix publication "
          "(retired generation slot=%u wanted=%u actual=%u refs=%u).",
          payload.slot, payload.generation,
          SlotOwnershipGeneration(observed_ownership),
          SlotOwnershipReferences(observed_ownership));
      return false;
    }
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
  (void)payloads;
  (void)payload_count;
  return true;
#endif
}

bool PublishPendingRawVifPayloads(const GpuVuDraw* first_draw,
                                 u32 draw_count) {
#if defined(VITASX2_QEMU_VALIDATION)
  (void)first_draw;
  (void)draw_count;
  return true;
#else
  if (draw_count != 0 && !first_draw)
    return false;
  const GpuVuDraw* draw = first_draw;
  for (u32 draw_index = 0; draw_index < draw_count; draw_index++) {
    if (!draw) {
      Console.Error(
          "GPU-VU: input publication rejected "
          "(short direct run at %u/%u).",
          draw_index, draw_count);
      return false;
    }
    const auto& inputs = draw->InputPayloads();
    // Publication only advances a generation's already-committed mapped
    // prefix.  Publish each descriptor's bounded inline payload list in
    // place: collecting an arbitrarily long direct run in std::vectors made
    // the MTGS worker throw std::bad_alloc in physical r200 after product
    // ownership had already begun. Repeated references become cheap no-ops
    // once the slot's published offset reaches its committed offset.
    for (const RawVifPayloadRef& payload : inputs) {
      auto* const owner = reinterpret_cast<InputRing*>(payload.owner);
      if (!owner || !owner->PublishRawVifPayloads(&payload, 1u))
        return false;
    }
    draw = draw->path1_next;
  }
  return true;
#endif
}

bool PublishPendingRawVifPayloads(const VifUnpackSpan* spans,
                                 u32 span_count) {
#if defined(VITASX2_QEMU_VALIDATION)
  (void)spans;
  (void)span_count;
  return true;
#else
  // An execute-only epoch has no immutable VIF bytes to publish. This must
  // remain a valid universal transaction even when the optional mapped input
  // ring could not be allocated: the fixed GXP binds its mapped zero-payload
  // page and consumes only the Execute/End command records. Requiring an
  // active ring here rejected every such epoch before the GS owner could
  // submit it.
  if (span_count == 0)
    return true;
  if (!spans)
    return false;
  for (u32 index = 0; index < span_count; index++) {
    const RawVifPayloadRef& payload = spans[index].payload;
    auto* const owner = reinterpret_cast<InputRing*>(payload.owner);
    if (!owner || !owner->PublishRawVifPayloads(&payload, 1u))
      return false;
  }
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
  if (!payload.IsValid() || payload.slot >= InputRingSlotCount)
    return false;
  auto* ring = reinterpret_cast<InputRing*>(payload.owner);
  if (!ring || !ring->m_impl)
    return false;
  ActiveRingCall active_call(&ring->m_impl->active_reference_calls);
  auto& slot = ring->m_impl->slots[payload.slot];
  if (!IsInputRingLogicalRange(payload.offset, payload.size))
    return false;
  const u32 required_prefix = payload.offset + payload.size;
  if (!slot.block.IsMapped() ||
      slot.committed_offset.load(std::memory_order_acquire) <
          required_prefix) {
    return false;
  }

  // Increment generation and reference count as one ownership identity. A
  // stale CAS can no longer join a recycled generation merely because its
  // reference count happens to equal the old operand.
  u64 ownership = slot.ownership.load(std::memory_order_acquire);
  if (SlotOwnershipGeneration(ownership) != payload.generation)
    return false;
  for (;;) {
    const u32 generation = SlotOwnershipGeneration(ownership);
    const u32 references = SlotOwnershipReferences(ownership);
    if (generation != payload.generation) {
      // The initial identity matched, but the last owner and the producer won
      // the paired transition before this retain could acquire a reference.
      s_retain_generation_rollbacks.fetch_add(
          1, std::memory_order_relaxed);
      return false;
    }
    if (references == 0u)
      return false;
    if (references == std::numeric_limits<u32>::max()) {
      s_retain_overflow_rejections.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    const u64 desired =
        PackSlotOwnership(generation, references + 1u);
    if (slot.ownership.compare_exchange_weak(
            ownership, desired, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
      const u32 confirmed_committed =
          slot.committed_offset.load(std::memory_order_acquire);
      const bool range_ok = slot.block.IsMapped() &&
          confirmed_committed >= required_prefix &&
          ring == s_active_input_ring.load(std::memory_order_acquire) &&
          ring->m_impl->accepting.load(std::memory_order_acquire);
      if (range_ok) {
        RecordReferenceCreated();
        return true;
      }

      s_retain_range_rollbacks.fetch_add(1, std::memory_order_relaxed);

      // This unreported physical reference pins the exact generation while it
      // is rolled back. Other owners may release first, so retain the ordinary
      // last-owner waiter handoff without changing live-reference telemetry.
      u64 physical_ownership =
          slot.ownership.load(std::memory_order_acquire);
      for (;;) {
        const u32 physical_generation =
            SlotOwnershipGeneration(physical_ownership);
        const u32 physical_references =
            SlotOwnershipReferences(physical_ownership);
        if (physical_generation != payload.generation ||
            physical_references == 0u) {
          s_release_underflow_rejections.fetch_add(
              1, std::memory_order_relaxed);
          return false;
        }
        const u64 rollback = PackSlotOwnership(
            physical_generation, physical_references - 1u);
        if (!slot.ownership.compare_exchange_weak(
                physical_ownership, rollback,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
          continue;
        }
        if (physical_references == 1u) {
          s32 expected_waiter = static_cast<s32>(payload.slot);
          if (ring->m_impl->waiting_slot.compare_exchange_strong(
                  expected_waiter, -1, std::memory_order_acq_rel,
                  std::memory_order_acquire)) {
            const int result = sceKernelSignalSema(
                ring->m_impl->slot_released_sema, 1);
            if (result < 0) {
              Console.Error(
                  "GPU-VU: VIF input slot rollback signal failed (%08x).",
                  static_cast<u32>(result));
            }
          }
        }
        return false;
      }
    }
  }
#endif
}

void ReleaseRawVifPayload(RawVifPayloadRef* payload) {
  if (!payload || !payload->IsValid())
    return;
#if !defined(VITASX2_QEMU_VALIDATION)
  auto* ring = reinterpret_cast<InputRing*>(payload->owner);
  if (ring && ring->m_impl && payload->slot < InputRingSlotCount) {
    ActiveRingCall active_call(&ring->m_impl->active_reference_calls);
    auto& slot = ring->m_impl->slots[payload->slot];
    u64 ownership = slot.ownership.load(std::memory_order_acquire);
    for (;;) {
      const u32 generation = SlotOwnershipGeneration(ownership);
      const u32 references = SlotOwnershipReferences(ownership);
      if (generation != payload->generation) {
        s_release_generation_mismatches.fetch_add(
            1, std::memory_order_relaxed);
        break;
      }
      if (references == 0u) {
        s_release_underflow_rejections.fetch_add(
            1, std::memory_order_relaxed);
        break;
      }
      const u64 desired = PackSlotOwnership(generation, references - 1u);
      if (!slot.ownership.compare_exchange_weak(
              ownership, desired, std::memory_order_acq_rel,
              std::memory_order_acquire)) {
        continue;
      }
      s_live_references.fetch_sub(1, std::memory_order_relaxed);
      if (references == 1u) {
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
      break;
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
  const u64 ownership = slot.ownership.load(std::memory_order_acquire);
  return SlotOwnershipGeneration(ownership) == payload.generation ?
      SlotOwnershipReferences(ownership) : 0u;
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
  stats.derived_captures =
      s_derived_captures.load(std::memory_order_relaxed);
  stats.derived_bytes =
      s_derived_bytes.load(std::memory_order_relaxed);
  stats.publication_batches =
      s_publication_batches.load(std::memory_order_relaxed);
  stats.published_bytes =
      s_published_bytes.load(std::memory_order_relaxed);
  stats.fetch_guard_scans =
      s_fetch_guard_scans.load(std::memory_order_relaxed);
  stats.fetch_guard_failures =
      s_fetch_guard_failures.load(std::memory_order_relaxed);
  stats.logical_high_water_bytes =
      s_logical_high_water_bytes.load(std::memory_order_relaxed);
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
  stats.retain_generation_rollbacks =
      s_retain_generation_rollbacks.load(std::memory_order_relaxed);
  stats.retain_range_rollbacks =
      s_retain_range_rollbacks.load(std::memory_order_relaxed);
  stats.retain_overflow_rejections =
      s_retain_overflow_rejections.load(std::memory_order_relaxed);
  stats.release_generation_mismatches =
      s_release_generation_mismatches.load(std::memory_order_relaxed);
  stats.release_underflow_rejections =
      s_release_underflow_rejections.load(std::memory_order_relaxed);
  stats.generation_exhaustions =
      s_generation_exhaustions.load(std::memory_order_relaxed);
  stats.deferred_unpacks =
      s_deferred_unpacks.load(std::memory_order_relaxed);
  stats.affine_span_merges =
      s_affine_span_merges.load(std::memory_order_relaxed);
  stats.replayed_unpacks =
      s_replayed_unpacks.load(std::memory_order_relaxed);
  return stats;
}

} // namespace VitaGpuVu
